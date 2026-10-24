/**
 * @file src/net/uvcpp_tcp_client.cpp
 * @brief Implementation of uvcpp_tcp_client.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_socks5_common.h>
#include <req/uvcpp_connect.h>
#include <req/uvcpp_write.h>
#include <req/uvcpp_getaddrinfo.h>
#include <uvcpp/uvcpp_alloc.h>
#include <uvcpp/uvcpp_define.h>
#include <handle/uvcpp_timer.h>   // 握手超时（见 tls_arm_handshake_timer）
#include <handle/uvcpp_async.h>   // 多地址连接的下一轮尝试（#44）

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl.h>
#include <ssl/uvcpp_ssl_context.h>
// SSL_ERROR_* 常量：`uvcpp_ssl::read/write/handshake` 把 WANT_READ/WANT_WRITE
// 归一化成 0，光看返回值分不出「还没完」和「对端干净关闭」，必须看这个。
#include <openssl/ssl.h>
#endif

namespace uvcpp {

// ---------------------------------------------------------------------------
// 读缓冲的线程局部自由表
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief 每线程一张的**读缓冲**自由表（按精确尺寸取还，只服务大块）。
 *
 * 背景：libuv 给 TCP 流的 `suggested_size` 是 **64 KiB**，于是每条连接**每次读**
 * 都要向分配器要一块 64 KiB，读回调返回后再还掉 —— `pipeline = 1` 的档上这就是
 * "每请求一进一出"。而这一进一出的代价远不止"一次 `malloc` 一次 `free`"：
 *
 * - 64 KiB 落在 glibc 的 large bin 上。`_int_malloc` 接这种请求时得先把 unsorted
 *   bin 里的块逐个归位（`unlink_chunk`）、要不到还要**顺手做一次 fastbin 合并**
 *   （`malloc_consolidate`，把本线程所有小块 free 表扫一遍塞进 unsorted bin）。
 * - 这笔账在"每请求只分配十来次"的 h1 baseline 上量不出来（实测在±0 量级），
 *   但在**每应答分配上千次**的那几条路上（nlohmann DOM 的 json 档、async-db 的
 *   行物化、WS 回声）会被放大成热点。
 *
 * 把这块缓冲挂到**线程**上（而不是随每次读生灭）之后，`/json/50?m=6` 上
 * `_int_malloc` 12.01% → 4.06%、`malloc_consolidate` 4.72% → 0.03%、
 * `unlink_chunk` 1.84% → 0.02%（callgrind，同一份入口、同一个 URL）。
 *
 * 为什么按**精确尺寸**取还而不是尺寸类：libuv 在给定平台上的 `suggested_size`
 * 是个常数（64 KiB），`sz == 0` 那条路要 4096 —— 实际只会用到这两种尺寸，向上
 * 取整反而会把"取到一块"变成"取到一块尺寸不对的"，只能丢掉。两个槽、每槽两块
 * 就够了：每条循环线程**同时活着**的读缓冲只有 1~2 块。
 *
 * **为什么不与 `uvcpp_malloc_block_cache`（`uvcpp_alloc.h`）合并成一张**：那张表
 * 的上限是 4 KiB、槽位 4×64，服务的是 `uvcpp_buf` 自己的块（尺寸多而小）。合成
 * 一张要么把上限抬到 64 KiB（那张表的最大占用会从 1 MiB 涨到 8 MiB），要么让大块
 * 挤掉小块 —— 而这两类块的寿命形状本来就不同。
 *
 * 链表指针在**表里**、不在块里：于是对块的内容不作任何假设，接手方拿到的内容也
 * 同样不保证是 0 —— 与 `alloc_raw()` 的契约一致（那条路本来就"不清零"，见
 * `uvcpp_buf.cpp` 里的说明）。
 *
 * 内存上界：每线程 `kSlotCount × kSlotMax` 块，实际就是 2 × 2 × 64 KiB =
 * 256 KiB（那张 4 KiB 的表是 1 MiB）。
 */
class read_block_cache {
 public:
  enum {
    kSlotCount = 2,                 ///< 两个尺寸槽（实测只会用到 4096 / 64 KiB）
    kSlotMax = 2,                   ///< 每槽最多留几块
    kMinCachedBytes = 4096,         ///< 比这更小的块根本不走这条路（见上面的 4096 兜底）
    kMaxCachedBytes = 256 * 1024    ///< 再大就是"这东西不该留"，直接还回去
  };

  /** @brief 取一块**正好** `size` 字节的存货；没有就返回 nullptr（调用方退回真分配）。 */
  void* take(std::size_t size) noexcept {
    if (size < kMinCachedBytes || size > kMaxCachedBytes) return nullptr;
    for (unsigned i = 0; i < kSlotCount; ++i) {
      slot& s = slots_[i];
      if (s.size != size || s.count == 0) continue;
      return s.head[--s.count];
    }
    return nullptr;
  }

  /**
   * @brief 还一块。收下了返回 true；尺寸不认 / 表里没有空槽返回 false —— 这时
   *        **调用方自己**走 `uvcpp_free_bytes()`（那张表与这张服务的是同一族块，
   *        但"谁来还"必须由调用方决定：内存池配置下这块不是 `std::malloc` 来的）。
   */
  bool put(void* p, std::size_t size) noexcept {
    if (p == nullptr) return true;  // 没什么可还的，也算处理完了
    if (size < kMinCachedBytes || size > kMaxCachedBytes) return false;
    for (unsigned i = 0; i < kSlotCount; ++i) {
      slot& s = slots_[i];
      if (s.size == size) {
        if (s.count >= kSlotMax) return false;  // 表满了，多留只是白占内存
        s.head[s.count++] = p;
        return true;
      }
      if (s.size == 0) {  // 空槽，收下这个尺寸
        s.size    = size;
        s.head[0] = p;
        s.count   = 1;
        return true;
      }
    }
    return false;
  }

 private:
  struct slot {
    std::size_t size;      ///< 这个槽服务的块尺寸；0 = 槽还空着
    unsigned count;        ///< 表上有几块
    void* head[kSlotMax];  ///< 自由块表 —— 在**表里**，不在块里（见类注释）
  };
  slot slots_[kSlotCount];
};

/// 本线程的表。`= {}` 让它常量初始化（没有 `__cxa_guard_*`），结构体平凡析构
/// ⇒ **不登记** `__cxa_thread_atexit`；跨线程还块只是把块留在还的那个线程的表里，
/// 正确性不受影响（块是 `malloc` 来的，谁 free 都行）。
inline read_block_cache& read_block_cache_here() noexcept {
  static UVCPP_THREAD_LOCAL read_block_cache cache = {};
  return cache;
}

/// 取一块读缓冲：先看本线程的表，没有就真分配。
inline void alloc_read_buf(uv_buf_t* bf, std::size_t len) {
#if !UVCPP_ENABLE_MEMORY_POOL
  void* p = read_block_cache_here().take(len);
  if (p != nullptr) {
    bf->base = static_cast<char*>(p);
    bf->len  = len;
    return;
  }
#endif
  uvcpp_buf::alloc_buf(bf, len);
}

/**
 * @brief 还一块读缓冲：先进本线程的表，表满或尺寸不认才真还。
 *
 * 尺寸在这一侧是**已知**的：`uv_buf_t::len` 就是当初 `alloc_buf()` 要的那个尺寸
 * （libuv 把 alloc 回调填的那个 `uv_buf_t` 原样传回读回调）。所以不需要
 * `malloc_usable_size()` 那类平台相关的反查 —— 这正是这张表能按精确尺寸做的前提。
 *
 * 企业内存池配置下这条表整条不启用（与 `uvcpp_buf::free_own()` 同一条纪律：
 * 池里的块要按池的规矩还，不能留在表里过夜），行为与改前**逐字相同**。
 */
inline void free_read_buf(const uv_buf_t* bf) {
  if (bf == nullptr || bf->base == nullptr) return;
#if !UVCPP_ENABLE_MEMORY_POOL
  if (read_block_cache_here().put(bf->base, bf->len)) return;
#endif
  uvcpp_free_bytes(bf->base);
}

}  // namespace

// =========================================================================
// Trampolines — bridge C-style callbacks to std::function stored on heap.
// These break the std::function copy-chain through libuv's callback layers.
// =========================================================================

static void trampoline_connect(int status, void* arg) {
  auto* cb = static_cast<std::function<void(int)>*>(arg);
  (*cb)(status);
  delete cb;
}

static void trampoline_write(int status, void* arg) {
  auto* cb = static_cast<std::function<void(int)>*>(arg);
  (*cb)(status);
  delete cb;
}

// ---------------------------------------------------------------------------
// 复用形态写请求（uvcpp_tcp_client::write_owned）的按连接状态
// ---------------------------------------------------------------------------
//
// **归属：请求对象** —— 它挂在 `uvcpp_req::set_data` / `get_data` 那个槽上（那
// 个槽本来就在，所以这一整套一个字都不改公开类型的布局）。请求在、状态就在；
// 请求由调用方按连接留着重用，于是头部缓冲的容量也只申请一次，稳态下这一笔
// 零分配（见 write_owned）。
//
// 为什么不按连接放在 `conn_ctx` 里（那才是"按连接"天然的家）：完成回调可能在
// 连接上下文**已经被抹掉之后**才到（对端在途断开走 `remove_ctx` → `tbl.erase`），
// 那一刻按 ctx 存的状态已经释放过了；而请求对象是 libuv 保证活到完成回调之后
// 的。**判据是"谁的生命周期由 libuv 兜底"，不是"谁看起来更像按连接的"。**
struct owned_write_state {
  uvcpp_tcp_client* client = nullptr;
  uvcpp_write*      w      = nullptr;
  uvcpp_tcp_client::owned_write_cb_t fn = nullptr;
  void*             arg    = nullptr;

  // 头部块：按连接复用，容量只增不减；初始化只许 uv_buf_init（{nullptr,0} 在 Windows 上编不过）
  uv_buf_t head     = uv_buf_init(nullptr, 0);
  char*    head_buf = nullptr;
  size_t   head_cap = 0;

  // 状态随请求一起放掉 ⇒ 头部缓冲也只在连接结束（或这一笔交不出去）时释放。
  ~owned_write_state() {
    if (head_buf != nullptr) {
      uvcpp::uvcpp_free(head_buf);
      head_buf = nullptr;
    }
  }
};

static void trampoline_read(uvcpp_buf* buf, void* arg) {
  auto* cb = static_cast<std::function<void(uvcpp_buf*)>*>(arg);
  (*cb)(buf);
  // Note: cb is NOT deleted here — read callback persists for multiple reads
}

static void trampoline_close(void* arg) {
  auto* cb = static_cast<std::function<void()>*>(arg);
  (*cb)();
  delete cb;
}

// =========================================================================
// 回收支路计数（诊断用，见 uvcpp_tcp_client::reclaim_stat）
// =========================================================================

static std::atomic<uint64_t> g_reclaim_released{0};
static std::atomic<uint64_t> g_reclaim_deferred{0};
static std::atomic<uint64_t> g_reclaim_skipped{0};

uvcpp_tcp_client::reclaim_stat uvcpp_tcp_client::reclaim_stats() {
  reclaim_stat s;
  s.released = g_reclaim_released.load(std::memory_order_relaxed);
  s.deferred = g_reclaim_deferred.load(std::memory_order_relaxed);
  s.skipped  = g_reclaim_skipped.load(std::memory_order_relaxed);
  return s;
}

// =========================================================================
// 快速路径计数（诊断用，见 uvcpp_tcp_client::try_write_stat）
// =========================================================================

static std::atomic<uint64_t> g_trywrite_attempted{0};
static std::atomic<uint64_t> g_trywrite_consumed{0};
static std::atomic<uint64_t> g_trywrite_skipped{0};
static std::atomic<uint64_t> g_trywrite_bytes{0};

uvcpp_tcp_client::try_write_stat uvcpp_tcp_client::try_write_stats() {
  try_write_stat s;
  s.attempted = g_trywrite_attempted.load(std::memory_order_relaxed);
  s.consumed  = g_trywrite_consumed.load(std::memory_order_relaxed);
  s.skipped   = g_trywrite_skipped.load(std::memory_order_relaxed);
  s.bytes     = g_trywrite_bytes.load(std::memory_order_relaxed);
  return s;
}

// =========================================================================
// Construction / Destruction
// =========================================================================

uvcpp_tcp_client::uvcpp_tcp_client() {
  loop_ = new uvcpp_loop();  // constructor already calls init()

  tcp_ = new uvcpp_tcp(loop_);

  status_ = TCP_CLIENT_NONE;
}

uvcpp_tcp_client::uvcpp_tcp_client(uvcpp_loop* external_loop) {
  owns_loop_ = false;
  loop_ = external_loop;

  tcp_ = new uvcpp_tcp(loop_);

  status_ = TCP_CLIENT_NONE;
}

uvcpp_tcp_client::uvcpp_tcp_client(uvcpp_loop* external_loop,
                                   uv_os_sock_t adopted_sock) {
  owns_loop_ = false;
  loop_ = external_loop;

  tcp_ = new uvcpp_tcp(loop_);

  status_ = TCP_CLIENT_NONE;

  // 认领一个**已经属于 external_loop 这条循环**的 socket（转手过来的那种）。
  //
  // 失败（`uv_tcp_open` 不接管）时只留一个失败码 + ERROR 态，**不留半个对象**
  // —— 但 socket 仍归调用方：libuv 在 `uv_tcp_open` 失败时不会关掉它，
  // 调用方必须自己收尾（见 `uvcpp_socket_handoff.h` 的 `uvcpp_handoff_close_raw`）。
  const int rc = tcp_->open(adopted_sock);
  if (rc != 0) {
    last_error_code_ = rc;
    status_ = TCP_CLIENT_ERROR;
  }
}

int uvcpp_tcp_client::loop_index() const { return loop_index_; }

void uvcpp_tcp_client::set_loop_index(int index) {
  // 只有 0 与正数是合法的循环号；负数当 0（**不是**"没设过"）—— 取值只要落在
  // 任何一张按循环切的表外面都会变成越界，静默容错比报错更难查。
  loop_index_ = index > 0 ? index : 0;
}

std::shared_ptr<char> uvcpp_tcp_client::alive_token() {
  if (!alive_token_) alive_token_.reset(new char(0));
  return alive_token_;
}

bool uvcpp_tcp_client::token_alive(const std::shared_ptr<char>& token) {
  return token && *token == 0;
}

uvcpp_tcp_client::~uvcpp_tcp_client() {
  // **第一件事就把令牌作废**：此后 libuv 送进来的任何异步完成回调都只收尾
  // （把请求对象还回去），不再碰本对象的任何成员。
  if (alive_token_) *alive_token_ = 1;

  // 复用形态（write_owned）在途的那一笔：存活判据不是令牌，而是按连接状态里
  // 那个 `client` 指针 —— 同样**必须在这里**置空，且必须排在下面
  // `write_arg_ = nullptr;` 之前（那之后就没有成员能找到这块状态了）。
  // 完成回调据此判断"客户端已析构"：那时它连 `client` 都不许碰。
  if (write_fn_ == trampoline_owned && write_arg_ != nullptr) {
    static_cast<owned_write_state*>(write_arg_)->client = nullptr;
  }

  // -------------------------------------------------------------------
  // **本对象可能是在它自己的某个回调里被析构的**（服务端最常见：读回调里
  // "对端断了" → 关闭回调 → 关闭管理器 `delete client`）。这一路上面那套
  // 收尾动作一件都不能做，`loop_` 与那几个 `*_arg_` 堆块只能留给事件循环。
  //
  // 三层悬垂，全部**实测**过（`tcp_client_func.cpp` 的 `delete_in_read_cb`，
  // 读回调里 `delete victim`；Release 裸跑**一次都不崩** —— 那块内存还好端端
  // 在那儿、读到的还是旧值；**PageHeap 下 SEGFAULT**，cdb 的栈是
  // `用户读回调 → std::function 调用器 → uvcpp_stream::callback_read →
  // uv_run → 测试的泵`，故障指令读的正是那个已经 unmap 的地址）：
  //
  //   1. **正在执行的那个闭包本身**：框架层读回调是成员 `net_read_cb_`，它就
  //      存在这个被删掉的对象里 —— 回调里删完对象、**随手再碰一个按引用捕获
  //      的变量**，读的就是已经还给分配器的闭包存储。这是**最先炸的一层**。
  //      （原始读路径同理：`read_arg_` 指向的那个 `std::function` 就是正在
  //      执行的闭包。）
  //   2. **`tcp_`**：读回调闭包与整个句柄都挂在它上面，而外层那一帧
  //      `uvcpp_stream::callback_read` 还在它里面跑。
  //   3. **`loop_`**：外层那一帧 `uv_run` 返回之后还要用它（`uvcpp_loop::run`
  //      的嵌套计数也要减）。
  //
  // **先停读**：句柄还活着、不停读的话循环下一轮还会回头调 `net_read_cb_`
  // —— 那已经是释放过的内存。只调底层那句（`tcp_->read_stop()`），**不能**调
  // 本类的 `read_stop()`：后者会把 `net_read_cb_` / `read_arg_` 就地清掉，
  // 那正是第 1 层。
  //
  // -------------------------------------------------------------------
  // **但包装对象本身不必跟着一起漏** —— 什么时候能删，取决于句柄处在哪一步
  // 而不是"循环是不是在跑"。判据是**句柄还在不在**：
  //
  //   - **句柄已摘**（`get_handle() == nullptr`）⇒ 此刻栈上唯一可能压着的本
  //     客户端回调是 `callback_close` 手里那份**栈上拷贝**的关闭回调
  //     （`uvcpp_handle::callback_close` 先把 `handle_close_cb` 拷到局部、
  //     再把成员清空，然后才调用），而它返回之后**不再碰 wrapper**。读 / 写 /
  //     连接那几条都不可能：它们的闭包要 `uv_read_start` 之类活着才开得起来，
  //     而句柄摘掉正是 `callback_close` 干的、那之后循环里已经没有本句柄。
  //     所以这一支可以**当场 `delete`**。
  //
  //   - **句柄还活着** ⇒ 栈上可能就是本客户端自己的读 / 写 / 连接回调，而那个
  //     闭包（`net_read_cb_` / `stream_read_cb` / `write_arg_`）就存在 `tcp_`
  //     里面，外层还压着 `uvcpp_stream::callback_read` 那一帧。这时不能删，
  //     但**也不必漏**：把包装对象交给**它自己的关闭完成回调**去删。
  //     `uv_close` 的完成回调在**本轮 `uv_run` 的收尾阶段**
  //     （`uv__run_closing_handles`）跑 —— 那时当前这一帧早已解开；而
  //     `callback_close` 保证调用之后不再碰 wrapper（见上）。
  //     顺带：这一句仍然必须发，它不只是"顺手回收" —— 句柄留在队列上、既不
  //     活跃也没在关，`uv__loop_alive()` 一个条件都不占，`uv_run` 连 while 体
  //     都不进，`uv_loop_close()` 从此永远 EBUSY，`~uvcpp_loop` 只能把整块
  //     `uv_loop_t` 泄漏掉（实测 `tests/tools/run_loop_leak_probe.py`：
  //     `tcp_server` 析构里那些"清不掉"的 `tcp(active=0 closing=0)`、以及
  //     webapp 一族 77 个泄漏循环，全走这一条）。
  //
  //   - **句柄正在关**（别人已经 `uv_close` 过）⇒ 关闭回调那个槽被占了
  //     （`handle_close_cb` 只有一个），插不进去，这一支只能留下 —— 这是**唯一
  //     剩下的、真正的泄漏**，而它在实测的两条 churn 路径上都没出现过
  //     （`tests/tools` 的探针：net 层 2000 次、webapp 1000 次，`is_closing`
  //     全为假）。
  //
  // `is_closing()` 那一问不能省：覆写正在执行的那个闭包本身就是第 1 层悬垂。
  //
  // **这一支原先是一个都不删的**，理由是"判据是循环还在跑"。那个判据区分不了
  // "我在自己的回调里"和"循环只是恰好跑着"，而服务运行期间后者恒成立 ——
  // 于是 `tcp_` 按连接数线性累积（实测 net 层 0.55–0.71 MB/min、webapp
  // 0.74–1.17 MB/min，都不收敛）。
  // -------------------------------------------------------------------
  if (loop_ != nullptr && loop_->is_running()) {
    if (tcp_ != nullptr) {
      const bool handle_detached = (tcp_->get_handle() == nullptr);
      if (handle_detached) {
        // 见上：句柄摘掉之后，没有谁的帧还压着这个包装对象。
        delete tcp_;
        g_reclaim_released.fetch_add(1, std::memory_order_relaxed);
        if (read_arg_ != nullptr) {
          delete static_cast<std::function<void(uvcpp_buf*)>*>(read_arg_);
          read_arg_ = nullptr;
        }
      } else {
        tcp_->read_stop();
        if (!tcp_->is_closing()) {
          // 交给自己的关闭完成回调 —— 那时这一帧已经解开（见上）。
          //
          // `read_arg_` 那块也一并交出去：它**不由任何 libuv 请求持有**
          // （`trampoline_read` 明确不删它 —— 读回调是长期注册，会反复进来；
          // 写/连接那几块则由各自的完成回调 `delete`），而读注册上面刚
          // `read_stop` 掉，所以"这一帧解开之后再删"是安全的。漏掉它就是
          // 每次连接漏一个 `std::function` 块 —— 实测那是剩下的大头。
          void* ra = read_arg_;
          read_arg_ = nullptr;
          tcp_->close([ra](uvcpp_handle* wrapper) {
            delete static_cast<std::function<void(uvcpp_buf*)>*>(ra);
            delete wrapper;
            g_reclaim_deferred.fetch_add(1, std::memory_order_relaxed);
          });
        } else {
          // 槽被占，只能留下（见上）。
          g_reclaim_skipped.fetch_add(1, std::memory_order_relaxed);
          std::fprintf(stderr,
                       "[uvcpp_tcp_client] 析构时句柄正在关闭：包装对象的"
                       "关闭回调槽已被占用，只能留在事件循环里\n");
        }
      }
      tcp_ = nullptr;
    }
#if UVCPP_OPENSSL_ENABLE
    // TLS 读路径上 `tls_ssl_` 正压在栈上（`tls_feed` / `tls_flush_out` 还在
    // 下面那几层里），不能删。
    tls_ssl_ = nullptr;

    // 握手超时定时器：先停掉（停是安全的，从它自己的回调里停也行）。
    // 释放要分情况 —— 如果本对象正是在那个超时回调里被拆掉的，删除包装对象
    // 等于析构正在执行的闭包（`uvcpp_timer::timer_start_cb` 就存在它里面）。
    //
    // **但底层句柄无论如何都要关。** 只 `stop()` 不 `uv_close()` 的句柄既不是
    // 活跃也没在关，而 `uv_loop_close()` 是**遍历 `handle_queue`**、只要还有一个
    // 非 internal 句柄就返回 `UV_EBUSY`（`_local_deps/libuv/src/uv-common.c:883`）
    // —— 与活跃无关。于是这条路上的每一个超时都赔掉**整个 `uv_loop_t`**：属主
    // 关不掉它，`~uvcpp_loop` 只能按既有策略"泄漏而不释放"。
    //
    // 这一条是实测出来的：同一套服务端 + 同一个卡住的 ClientHello，只把"销毁
    // 发生在哪个回调里"换掉 —— 超时回调（本分支）下 `uv_loop_close()` 是
    // -4082(EBUSY)、队列上剩下的孤儿是 `timer active=0 closing=0`；改由读回调
    // 销毁（下面 `delete` 那支）则是 0、干净。用例见
    // `tests/functional/web_ssl_server_hs_timeout_func.cpp`（去掉本分支这句
    // `uv_close` 它必红）。而且走的是**框架自己**的路：`uvcpp_tcp_server` 的
    // 管理槽就在 `tls_fail()` 第 5 步里 `delete` 客户端，不需要用户做任何特别
    // 的事。
    if (tls_hs_timer_ != nullptr) {
      tls_hs_timer_->stop();
      if (tls_hs_in_cb_) {
        // 与上面 `tcp_` 同一处理：句柄单独关掉，**包装对象交给它的关闭完成
        // 回调去删**。原先这里是"包装对象留在原地"，理由是"同上" —— 而上面
        // 那句现在不算在"原地"了，所以这里是照同一个理由改过来，不是新加
        // 的依赖：`uv_close` 是异步的，完成回调跑在本轮 `uv_run` 的收尾阶段，
        // 那时定时器自己的回调帧早就解开了。
        if (!tls_hs_timer_->is_closing()) {
          tls_hs_timer_->close([](uvcpp_handle* wrapper) {
            delete wrapper;
            g_reclaim_deferred.fetch_add(1, std::memory_order_relaxed);
          });
        } else {
          g_reclaim_skipped.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        delete tls_hs_timer_;
        g_reclaim_released.fetch_add(1, std::memory_order_relaxed);
      }
      tls_hs_timer_ = nullptr;
    }
#endif
    // 多地址连接把尝试串起来的 async 句柄：没关掉的话整个 `uv_loop_t` 会
    // `EBUSY`（它是非 internal 句柄）。包装对象在关闭完成回调里删 —— 那时
    // 当前回调栈已经解开。
    if (connect_retry_async_ != nullptr) {
      uvcpp_async* a = connect_retry_async_;
      connect_retry_async_ = nullptr;
      if (a->get_handle() == nullptr) {
        delete a;
      } else if (!a->is_closing()) {
        a->close([a](uvcpp_handle*) { delete a; });
      }
    }
    connect_attempt_token_.reset();
    connect_user_cb_ = nullptr;
    loop_          = nullptr;
    read_arg_      = nullptr;
    connect_arg_   = nullptr;
    write_arg_     = nullptr;
    close_arg_     = nullptr;
    close_mgr_arg_ = nullptr;
    // `read_cache_` 不在此列：谁都不引用它，删掉比漏掉好。
    if (read_cache_ != nullptr) {
      delete read_cache_;
      read_cache_ = nullptr;
    }
    return;
  }

#if UVCPP_OPENSSL_ENABLE
  // 握手超时定时器（惰性创建，握手成功即释放；只有超时那一条路上会活到
  // 这里）。这条分支不在任何回调栈上，可以安全释放。
  tls_disarm_handshake_timer();

  // TLS 对象归本客户端所有，跟着一起走。注意这里**不发 close_notify**：
  // 发它要先把 wbio 里的字节异步写出去，而析构路径上没有事件循环可等。
  // 所以对端看到的是 TCP 层的断开（`SSL_ERROR_SYSCALL` / 提前 EOF）而不是
  // 干净的 TLS 关闭 —— 这是有意的取舍，写在 `enable_tls` 的文档里。
  if (tls_ssl_ != nullptr) {
    delete tls_ssl_;
    tls_ssl_ = nullptr;
  }
#endif

  // 多地址连接的 async 句柄（见上面 running-loop 分支里的同一个理由）。
  if (connect_retry_async_ != nullptr) {
    uvcpp_async* a = connect_retry_async_;
    connect_retry_async_ = nullptr;
    if (a->get_handle() == nullptr) {
      delete a;
    } else if (!a->is_closing()) {
      a->close([a](uvcpp_handle*) { delete a; });
    }
  }
  connect_attempt_token_.reset();
  connect_user_cb_ = nullptr;

  // The underlying handle may already be closed and freed (_handle nulled) by
  // an outer owner (e.g. uvcpp_http_client closes the tcp directly). Calling
  // read_stop()/is_closing()/is_active() on a null handle dereferences null,
  // so guard all handle access against it here.
  const bool handle_alive =
      (tcp_ != nullptr && tcp_->get_handle() != nullptr);

  // Stop any active reads
  if (read_started_) {
    if (handle_alive) {
      tcp_->read_stop();
    }
    read_started_ = false;
  }

  // Ensure the TCP handle is fully closed and its endgame processed
  // before we try to close the loop.  libuv asserts the handle queue is
  // empty before uv_loop_close().
  if (tcp_ != nullptr && handle_alive) {
    // **判据只能是 `is_closing()`，不能带上 `is_active()`。**
    //
    // 上面刚调过 `read_stop()`，而 `uv_read_stop` 会摘掉读 watcher —— 句柄在
    // 没有在途写时随之变成"不活跃"。原先的写法是
    // `!is_closing() && is_active()`，于是"我刚把读停掉、socket 还开着"和
    // "句柄早就关完了"这两种完全不同的状态落进了同一个 else（注释里只写了
    // 后者），结果是**析构时根本不关 socket**：对端永远等不到 FIN，服务端的
    // 连接登记表里留着一个死连接，fd 一直占着直到进程退出。
    //
    // 句柄真正关完之后 `uvcpp_handle` 会把内部句柄指针置空，那时
    // `handle_alive` 就是 false，压根走不到这里 —— 所以在 `handle_alive` 为真
    // 的前提下，"不在关闭中"确实等价于"还开着"，`is_active()` 提供不了任何
    // 额外信息，只会把上面那次 read_stop 的副作用误读成"已经关完了"。
    //
    // 这个缺陷由 Phase 4b 的 TLS 回环用例（场景 4，同步接口）撞出来：同步
    // 客户端在析构时正好处于"读过、还开着、已经没有在读"的状态，而异步用例
    // 与写-only 用例都在返回前显式 `close()` 过，所以一直没暴露。
    if (!tcp_->is_closing()) {
      // Handle is still open — close it and pump the loop.
      if (owns_loop_) {
        bool close_done = false;
        tcp_->close([&close_done](uvcpp_handle*) { close_done = true; });

        // **有界、不睡眠、不看墙钟**（与 `~uvcpp_ws_client` 同一形状）。
        //
        // 原先是 `while (!close_done) { run(NOWAIT); sleep(1ms); }` 加一个
        // 5 秒墙钟上限：析构里睡 1 毫秒本身就是"在循环线程上跑耗时操作"，
        // 而且它把一次终结拖成最多 5000 次系统调用 + 5 秒墙钟。64 轮
        // `UV_RUN_NOWAIT` 足够把这一次 `uv_close` 的完成回调放完 ——
        // `closesocket()` 是在 `uv_close()` 里**同步**做的（libuv
        // `win/tcp.c` 的 `uv__tcp_close`），所以对端看到 FIN 与这里的轮数
        // 无关；轮数只决定"句柄的终结收尾"这一轮跑没跑完，没跑完的后果是
        // `uv_loop_close()` 失败，由 `~uvcpp_loop` 按既有策略"泄漏而不释放"。
        // 实测（38 次析构、同一台机）：`close_done` 全部在**第 1 轮**就为真，
        // 而 `loop_close()` 全部返回 0（队列干净、`~uvcpp_loop` 不必走"泄漏
        // 而不释放"那条兜底）。也就是说 64 只是余量，不是"刚好够"。
        for (int i = 0; i < 64 && !close_done; ++i) {
          if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
        }
      } else {
        tcp_->close([](uvcpp_handle*) {});
      }
    } else if (tcp_->is_closing()) {
      // Handle was already closed (e.g. by EOF callback) but its
      // endgame hasn't fired yet.  Pump the loop once to let it run.
      if (owns_loop_ && loop_ != nullptr) {
        loop_->run(UV_RUN_NOWAIT);
      }
    }
    // else: handle already fully closed (endgame fired) — nothing to do.
  }

  // Close the loop BEFORE deleting the TCP handle wrapper.
  // This ensures any internal libuv clean-up queued during handle close
  // is fully drained before uv_loop_close() validates the handle queue.
  //
  // 上面那两处泵只保证**本客户端的 tcp 句柄**收尾了，循环上可能还挂着别人
  // （实测 `test_web_ws_ownership_func`：一个 `async(active=0 closing=1)`，
  // 24 个泄漏循环全是它）。所以关之前再按循环自己的判据泵一遍 ——
  // 判据用 `loop_alive()`，**不能把 `loop_close()` 写进条件**：它内部会
  // `stop()`，而 `uv_run` 的 `while (r != 0 && loop->stop_flag == 0)` 是在进
  // body 之前判的，停标志一立那一轮就整段空转，交替调用等于原地打转。
  if (loop_ != nullptr && owns_loop_) {
    for (int i = 0; i < 256 && loop_->loop_alive() != 0; ++i) {
      loop_->run(UV_RUN_NOWAIT);
    }
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }

  // Now safe to delete the TCP handle — the loop is already closed.
  if (tcp_ != nullptr) {
    delete tcp_;
    tcp_ = nullptr;
  }

  // Free read cache
  if (read_cache_ != nullptr) {
    delete read_cache_;
    read_cache_ = nullptr;
  }

  // Free any pending async callback data
  if (connect_arg_ != nullptr) {
    delete static_cast<std::function<void(int)>*>(connect_arg_);
    connect_arg_ = nullptr;
  }
  if (write_arg_ != nullptr) {
    // **这一格是联合体**：普通异步写（`write`）存 `std::function<void(int)>*`，
    // 复用形态（`write_owned`）存 `owned_write_state*`，按 `write_fn_` 区分。
    //
    // 照 `std::function` 删复用形态那一块是**类型混淆**，而且后果是一种很安静
    // 的"看起来像回调"的崩溃：`owned_write_state` 偏移 16 的那个字段是 `fn`
    // （也就是 `uvcpp_http_server::recycle_write_done`），正好落在 libstdc++
    // `_M_manager` 的槽位上 ⇒ 析构函数把它当管理器调用一次，实参恰好是
    // `(两块 _Any_data, __destroy_functor)`，于是
    // `recycle_write_done(state, state, 3, 垃圾)` 就地跑进 `settle_write`，
    // 拿堆上那块状态当 http 服务器去查 `contexts_`。
    // 实测栈（Release 包树配置、`test_web_http_server_func` 的
    // `deferred_response_is_compressed`）：
    //   ~uvcpp_http_server → ~uvcpp_tcp_server → ~uvcpp_tcp_client:623
    //   → recycle_write_done → settle_write → ctxs_of → SIGSEGV 在空的
    //   `contexts_` 上取 `contexts_[0]`。
    // 发布 zip 那棵树只是"这一笔恰好已经结算完"才没撞上，不是这条分支不存在。
    if (write_fn_ == trampoline_owned) {
      // 状态归请求对象（`w->get_data()`）所有：上面已把 `st->client` 置空，
      // 完成回调走 `owned_write_done` 的"客户端已析构"那条路，连同请求一起
      // 放掉。这里一格都不能删。
      write_arg_ = nullptr;
    } else {
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_arg_ = nullptr;
    }
  }
  if (read_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_buf*)>*>(read_arg_);
    read_arg_ = nullptr;
  }
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  if (close_mgr_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_mgr_arg_);
    close_mgr_arg_ = nullptr;
  }
}

// =========================================================================
// Accessors
// =========================================================================

uvcpp_tcp* uvcpp_tcp_client::get_tcp() {
  return tcp_;
}

uvcpp_loop* uvcpp_tcp_client::get_loop() {
  return loop_;
}

int uvcpp_tcp_client::get_status() const {
  return status_;
}

int uvcpp_tcp_client::get_last_error() const {
  return last_error_code_;
}

bool uvcpp_tcp_client::has_status(int flags) const {
  return (status_ & flags) == flags;
}

bool uvcpp_tcp_client::has_read_callback() const {
  // 自动读不算 "用户设了读回调"：它没有消费者，数据是丢掉的。
  return has_async_read_cb_ && !auto_read_installed_;
}

bool uvcpp_tcp_client::is_auto_read() const {
  return auto_read_installed_;
}

bool uvcpp_tcp_client::has_write_callback() const {
  return has_async_write_cb_;
}

bool uvcpp_tcp_client::has_close_callback() const {
  return close_fn_ != nullptr;
}

void uvcpp_tcp_client::mark_accepted() {
  set_status(TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE | TCP_CLIENT_WRITABLE);
}

// =========================================================================
// Address helpers
// =========================================================================

sockaddr_storage uvcpp_tcp_client::getLocalAddrs(std::string& ip, int& port) {
  struct sockaddr_storage local_addr;
  std::memset(&local_addr, 0, sizeof(local_addr));
  int local_addr_len = sizeof(local_addr);

  if (tcp_ == nullptr) {
    return local_addr;
  }

  int rc = tcp_->getsockname(
      reinterpret_cast<struct sockaddr*>(&local_addr), &local_addr_len);
  if (rc != 0) {
    last_error_code_ = rc;
    return local_addr;
  }

  char local_ip[INET6_ADDRSTRLEN] = {0};

  if (local_addr.ss_family == AF_INET) {
    uv_ip4_name(reinterpret_cast<struct sockaddr_in*>(&local_addr),
                local_ip, sizeof(local_ip));
    ip = local_ip;
    port = ntohs(reinterpret_cast<struct sockaddr_in*>(&local_addr)->sin_port);
  } else if (local_addr.ss_family == AF_INET6) {
    uv_ip6_name(reinterpret_cast<struct sockaddr_in6*>(&local_addr),
                local_ip, sizeof(local_ip));
    ip = local_ip;
    port = ntohs(
        reinterpret_cast<struct sockaddr_in6*>(&local_addr)->sin6_port);
  }

  return local_addr;
}

sockaddr_storage uvcpp_tcp_client::getPeerAddrs(std::string& ip, int& port) {
  struct sockaddr_storage peer_addr;
  std::memset(&peer_addr, 0, sizeof(peer_addr));
  int peer_addr_len = sizeof(peer_addr);

  if (tcp_ == nullptr) {
    return peer_addr;
  }

  int rc = tcp_->getpeername(
      reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_addr_len);
  if (rc != 0) {
    last_error_code_ = rc;
    return peer_addr;
  }

  char peer_ip[INET6_ADDRSTRLEN] = {0};

  if (peer_addr.ss_family == AF_INET) {
    uv_ip4_name(reinterpret_cast<struct sockaddr_in*>(&peer_addr),
                peer_ip, sizeof(peer_ip));
    ip = peer_ip;
    port = ntohs(reinterpret_cast<struct sockaddr_in*>(&peer_addr)->sin_port);
  } else if (peer_addr.ss_family == AF_INET6) {
    uv_ip6_name(reinterpret_cast<struct sockaddr_in6*>(&peer_addr),
                peer_ip, sizeof(peer_ip));
    ip = peer_ip;
    port = ntohs(
        reinterpret_cast<struct sockaddr_in6*>(&peer_addr)->sin6_port);
  }

  return peer_addr;
}

// =========================================================================
// Read cache
// =========================================================================

void uvcpp_tcp_client::set_max_read_cache_size(size_t max_size) {
  max_read_cache_size_ = max_size;
}

size_t uvcpp_tcp_client::get_max_read_cache_size() const {
  return max_read_cache_size_;
}

void uvcpp_tcp_client::ensure_read_cache() {
  if (read_cache_ == nullptr) {
    read_cache_ = new uvcpp_buf();
  }
}

// =========================================================================
// Connect
// =========================================================================

int uvcpp_tcp_client::connect(const char* ip, int port,
                               std::function<void(int)> cb) {
#if UVCPP_OPENSSL_ENABLE
  // 形参名叫 `ip`，但它**同时是**主机名与数字 IP 两条路的入口（下面就在按
  // `uv_ip4_addr` 分流）。`PEER_STRICT` 下证书要对的正是这个名字，所以在分流转成
  // `sockaddr` **之前**先留一份 —— 转完那个字符串就没了。
  //
  // 整个块都在 `UVCPP_OPENSSL_ENABLE` 里：`tls_host_want_` 与 `apply_tls_hostname()`
  // 都只在这个开关下存在（见头文件里同一道守卫）。**关掉 SSL 的档位里
  // `connect()` 照样要编**，漏了这道守卫就是"本机开着 SSL 编得过、CI 的 basic 档全表红"。
  //
  // 配了 SOCKS5 时这里仍是**目标**主机名：TLS 的 SNI 与证书校验必须对目标，
  // 不能变成代理的地址。
  if (ip != nullptr) {
    tls_host_want_ = ip;
    apply_tls_hostname();
  }
#endif  // UVCPP_OPENSSL_ENABLE

  if (socks5_proxy_enabled_) {
    if (ip == nullptr) return UV_EINVAL;
    if (cb == nullptr) {
      return connect_wait(ip, port, 30000);
    }
    return socks5_connect_async(ip, port, std::move(cb));
  }

  return connect_transport(ip, port, std::move(cb));
}

int uvcpp_tcp_client::connect_transport(const char* ip, int port,
                                        std::function<void(int)> cb) {
  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_connect_cb_) {
      return UV_EALREADY;
    }

    has_async_connect_cb_ = true;
    set_status(TCP_CLIENT_CONNECTING);

    // 数字 IPv4 快速路径（无需 DNS）
    struct sockaddr_in addr4;
    if (uv_ip4_addr(ip, port, &addr4) == 0) {
      int rc = connect_async(reinterpret_cast<const struct sockaddr*>(&addr4),
                             std::move(cb));
      if (rc != 0) has_async_connect_cb_ = false;
      return rc;
    }

    // 数字 IPv6 快速路径（IPv6 字面量带 `:`，`uv_ip4_addr` 必失败）。
    // 必须在 DNS 之前：`AF_INET` 的解析器不认识 `::1` 这种写法。
    struct sockaddr_in6 addr6;
    if (uv_ip6_addr(ip, port, &addr6) == 0) {
      int rc = connect_async(reinterpret_cast<const struct sockaddr*>(&addr6),
                             std::move(cb));
      if (rc != 0) has_async_connect_cb_ = false;
      return rc;
    }

    // 主机名：异步 DNS 解析，**AF_UNSPEC** —— IPv6-only 的主机今天连不上，
    // 双栈主机也没有回退（#44）。解析结果按 getaddrinfo 的顺序逐个尝试。
    uvcpp_getaddrinfo* resolver = new uvcpp_getaddrinfo();
    char service[16];
    uvcpp_snprintf(service, sizeof(service), "%d", port);

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int rc = resolver->getaddrinfo(
        loop_, ip, service, &hints,
        [this, cb](uvcpp_getaddrinfo* req, int status, struct addrinfo* res) {
          // DNS 失败或无可解析地址
          if (status != 0 || res == nullptr) {
            int err = (status != 0) ? status : UV_EAI_NONAME;
            clear_status(TCP_CLIENT_CONNECTING);
            set_status(TCP_CLIENT_ERROR);
            last_error_code_      = err;
            has_async_connect_cb_ = false;
            if (res != nullptr) req->freeaddrinfo(res);
            delete req;
            cb(err);
            return;
          }

          std::vector<sockaddr_storage> addrs;
          for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
            if (p->ai_addr == nullptr || p->ai_addrlen == 0) continue;
            if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
            sockaddr_storage ss;
            std::memset(&ss, 0, sizeof(ss));
            std::memcpy(&ss, p->ai_addr,
                        static_cast<size_t>(p->ai_addrlen) > sizeof(ss)
                            ? sizeof(ss)
                            : static_cast<size_t>(p->ai_addrlen));
            addrs.push_back(ss);
          }
          req->freeaddrinfo(res);
          delete req;

          if (addrs.empty()) {
            clear_status(TCP_CLIENT_CONNECTING);
            set_status(TCP_CLIENT_ERROR);
            last_error_code_      = UV_EAI_NONAME;
            has_async_connect_cb_ = false;
            cb(UV_EAI_NONAME);
            return;
          }
          start_multi_connect(std::move(addrs), std::move(cb));
        });

    if (rc != 0) {
      delete resolver;
      clear_status(TCP_CLIENT_CONNECTING);
      set_status(TCP_CLIENT_ERROR);
      last_error_code_      = rc;
      has_async_connect_cb_ = false;
      return rc;
    }

    return 0;
  } else {
    // --- Sync mode ---
    return connect_wait(ip, port, 30000);
  }
}

// =========================================================================
// 多地址连接（#44）
// =========================================================================

void uvcpp_tcp_client::init_connect_retry_async() {
  if (connect_retry_async_ != nullptr) return;
  connect_retry_async_ = new uvcpp_async();
  // 存活令牌：析构会把 `*alive_token_` 置 1。`uv_close()` 通常会丢弃还没跑
  // 的 async 通知，但"通常会"不是契约 —— 回调里先问一句，免得在对象已经
  // 析构之后还去碰它的成员。
  std::shared_ptr<char> life = alive_token();
  connect_retry_async_->init(
      [this, life](uvcpp_async*) {
        if (!token_alive(life)) return;
        connect_attempt_next();
      },
      loop_);
}

void uvcpp_tcp_client::post_connect_attempt() {
  init_connect_retry_async();
  if (connect_retry_async_ != nullptr) connect_retry_async_->send();
}

void uvcpp_tcp_client::retire_attempt_handle() {
  if (tcp_ != nullptr) {
    uvcpp_tcp* old = tcp_;
    tcp_ = nullptr;
    if (old->get_handle() == nullptr) {
      delete old;  // 句柄已经摘掉（关闭完成过）
    } else if (!old->is_closing()) {
      old->read_stop();
      // 包装对象交给它自己的关闭完成回调 —— 那时当前回调栈已经解开。
      old->close([old](uvcpp_handle*) { delete old; });
    } else {
      // 关闭回调槽已被占用：包装对象只能留给循环（与析构里同一条取舍）。
    }
  }
  read_started_ = false;

#if UVCPP_OPENSSL_ENABLE
  // 每次尝试的 TLS 会话必须从零开始：握手状态、明文/密文缓冲、ALPN 都是
  // **按连接**的，不能带着上一笔的残留去握第二次。
  tls_handshake_done_  = false;
  tls_connect_pending_ = false;
  tls_write_pending_   = false;
  tls_write_sync_      = false;
  tls_out_busy_        = false;
  tls_out_.clear();
  tls_plain_.clear();
  tls_ssl_error_       = 0;
  tls_alpn_.clear();
  if (tls_hs_timer_ != nullptr) tls_hs_timer_->stop();
  // 失败过的 SSL 会话不能拿去下一次握手：整份重建（memory BIO 是空的）。
  if (tls_ctx_ != nullptr) {
    delete tls_ssl_;
    tls_ssl_ = nullptr;
    clear_status(TCP_CLIENT_ERROR);
    enable_tls(tls_ctx_);
  }
#endif

  tcp_ = new uvcpp_tcp(loop_);
}

void uvcpp_tcp_client::start_multi_connect(
    std::vector<sockaddr_storage> addrs, std::function<void(int)> cb) {
  connect_addrs_.swap(addrs);
  connect_addr_index_    = 0;
  connect_user_cb_       = std::move(cb);
  connect_last_error_    = 0;
  connect_attempt_token_.reset();
  connect_attempt_tcp_up_ = false;
  // 第一次尝试不需要退役句柄：调用方刚建的 `tcp_` 还没连过任何东西。
  connect_attempt_next();
}

void uvcpp_tcp_client::connect_attempt_next() {
  if (connect_user_cb_ == nullptr) return;  // 已经收尾/被取消

  if (connect_addr_index_ >= connect_addrs_.size()) {
    finish_multi_connect(connect_last_error_ != 0 ? connect_last_error_
                                                  : UV_EAI_NONAME);
    return;
  }

  // 上一笔尝试失败后才走到这里：它的句柄不能复用（libuv 在 connect 失败后
  // 再复用会立刻 ECONNABORTED），换一个新的，旧的在关闭回调里回收。
  if (connect_attempt_token_) retire_attempt_handle();

  connect_attempt_token_ = std::make_shared<char>(0);
  std::weak_ptr<char> tok = connect_attempt_token_;
  connect_attempt_tcp_up_ = false;
  const sockaddr_storage cur = connect_addrs_[connect_addr_index_++];

  clear_status(TCP_CLIENT_CONNECTING);
  set_status(TCP_CLIENT_CONNECTING);
  last_error_code_      = 0;
  has_async_connect_cb_ = true;

  const int rc = connect_async(reinterpret_cast<const struct sockaddr*>(&cur),
                               [this, tok](int status) {
                                 if (tok.expired()) return;  // 陈旧尝试
                                 if (status == 0) {
                                   finish_multi_connect(0);
                                   return;
                                 }
                                 connect_last_error_ = status;
                                 if (connect_attempt_tcp_up_) {
                                   // TCP 已经连上，失败发生在 SOCKS5 协商 /
                                   // TLS 握手：换地址不会改变结果，而且这时
                                   // 再连一笔会被当成普通 TCP 成功（见成员
                                   // 注释）。直接收尾。
                                   finish_multi_connect(status);
                                   return;
                                 }
                                 // 下一轮在 async 回调里跑：这里还压着旧句柄
                                 // 的 connect 回调，不能当场换句柄。
                                 post_connect_attempt();
                               });
  if (rc != 0) {
    connect_last_error_ = rc;
    // 这一笔连提交都没成功（socket 创建失败之类）：对 SOCKS5 来说仍是
    // "连代理"阶段失败。
    if (socks5_pending_) socks5_proxy_connect_failed_ = true;
    post_connect_attempt();
  }
}

void uvcpp_tcp_client::finish_multi_connect(int status) {
  connect_addrs_.clear();
  connect_addr_index_ = 0;
  connect_attempt_token_.reset();
  connect_attempt_tcp_up_ = false;
  has_async_connect_cb_ = false;

  std::function<void(int)> cb = std::move(connect_user_cb_);
  connect_user_cb_ = nullptr;

  if (status != 0) {
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = status;
  }
  if (cb) cb(status);
}

int uvcpp_tcp_client::connect_wait(const char* ip, int port, int timeout_ms) {
#if UVCPP_OPENSSL_ENABLE
  // 同 `connect()`：主机名要在 `resolve_host_sync()` 之前留一份。
  if (ip != nullptr) {
    tls_host_want_ = ip;
    apply_tls_hostname();
  }
#endif  // UVCPP_OPENSSL_ENABLE

  if (has_async_connect_cb_) {
    throw std::runtime_error(
        "uvcpp_tcp_client::connect_wait: cannot use sync connect after "
        "async connect callback was registered");
  }

  // 配了 SOCKS5：`ip` 是**目标**，实际要拨的是代理。目标名不做本地解析。
  const bool via_socks5 = socks5_proxy_enabled_;
  if (via_socks5) {
    if (ip == nullptr) {
      last_error_code_ = UV_EINVAL;
      return UV_EINVAL;
    }
    socks5_target_host_ = ip;
    socks5_target_port_ = port;
    socks5_in_.clear();
    socks5_plain_.clear();
    socks5_last_error_.clear();
    socks5_proxy_connect_failed_ = false;
    socks5_phase_    = socks5_phase::NONE;
    socks5_pending_  = true;
  }

  // 解析主机名（数字 IP 快速路径 + DNS 回退），**AF_UNSPEC** —— A 与 AAAA
  // 都要：IPv6-only 的主机今天连不上，双栈主机也没有回退（#44）。
  std::vector<sockaddr_storage> addrs;
  int rc = resolve_host_sync_all(via_socks5 ? socks5_proxy_host_.c_str() : ip,
                                 via_socks5 ? socks5_proxy_port_ : port,
                                 addrs, timeout_ms);
  if (rc != 0) {
    if (via_socks5) socks5_proxy_connect_failed_ = true;
    socks5_pending_ = false;
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = rc;
    return rc;
  }

  // Reset sync state
  sync_connect_done_   = false;
  sync_connect_result_ = 0;

  set_status(TCP_CLIENT_CONNECTING);

  // 按 getaddrinfo 的顺序逐个尝试。每次失败后换一个新的句柄（同一个句柄在
  // connect 失败后不能复用），总时间由**同一个** deadline 约束。
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int  last_err  = 0;
  bool connected = false;
  for (std::size_t attempt = 0; attempt < addrs.size(); ++attempt) {
    const long long remain_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())
            .count();
    if (remain_ms <= 0) {
      last_err = UV_ETIMEDOUT;
      break;
    }
    if (attempt > 0) {
      retire_attempt_handle();
      clear_status(TCP_CLIENT_ERROR);
      set_status(TCP_CLIENT_CONNECTING);
      last_error_code_ = 0;
    }

    sync_connect_done_   = false;
    sync_connect_result_ = 0;

    uvcpp_connect* conn = new uvcpp_connect();
    // 释放交给 callback_connect 在**闭包返回之后**做：回调里 `delete r` 删的是
    // 正在执行的闭包本身。见 `uvcpp_req::invoke_completion`。
    conn->set_self_free(true);
    std::shared_ptr<char> life = alive_token();
    rc = tcp_->connect(
        conn, reinterpret_cast<const struct sockaddr*>(&addrs[attempt]),
        [this, life](uvcpp_connect* r, int status) {
          (void)r;  // 释放由 callback_connect 事后做
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 trampoline 还回去
          }
          clear_status(TCP_CLIENT_CONNECTING);
          if (status == 0) {
            set_status(TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE |
                       TCP_CLIENT_WRITABLE);
          } else {
            set_status(TCP_CLIENT_ERROR);
            last_error_code_ = status;
          }
          sync_connect_result_ = status;
          sync_connect_done_   = true;
        });

    if (rc != 0) {
      delete conn;
      last_err = rc;
      continue;
    }

    if (!wait_for_condition([this]() { return sync_connect_done_; },
                            static_cast<int>(remain_ms))) {
      last_err = UV_ETIMEDOUT;
      continue;
    }

    if (sync_connect_result_ != 0) {
      last_err = sync_connect_result_;
      continue;
    }
    connected = true;
    break;
  }

  if (!connected) {
    if (via_socks5) socks5_proxy_connect_failed_ = true;
    socks5_pending_ = false;
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = last_err != 0 ? last_err : UV_EAI_NONAME;
    return last_error_code_;
  }
  if (via_socks5) socks5_proxy_connect_failed_ = false;

  if (via_socks5) {
    // 协商要在循环里推进：arm_async_read 装的那个回调会在 nread>0 时先看
    // `socks5_phase_`，把字节喂给状态机。
    if (!read_started_) {
      const int arc = arm_async_read();
      if (arc != 0) {
        socks5_pending_ = false;
        set_status(TCP_CLIENT_ERROR);
        last_error_code_ = arc;
        return arc;
      }
    }
    rc = socks5_run_sync(timeout_ms);
    socks5_pending_ = false;
    if (rc != 0) {
      set_status(TCP_CLIENT_ERROR);
      last_error_code_ = rc;
      return rc;
    }
  }

#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) {
    // 握手同样要在循环里推进。这里只负责把循环转起来等它 —— 推进本身发生在
    // 读事件里（arm_async_read 装的那个回调），与异步路径走的是同一条代码。
    if (!read_started_) {
      const int arc = arm_async_read();
      if (arc != 0) {
        set_status(TCP_CLIENT_ERROR);
        last_error_code_ = arc;
        return arc;
      }
    }
    // SOCKS5 协商成功时同一段里可能已经带来目标服务端的 TLS 记录。
    if (!socks5_plain_.empty()) {
      std::string pending;
      pending.swap(socks5_plain_);
      if (tls_feed(pending.data(), pending.size()) != 0) {
        if (last_error_code_ == 0) last_error_code_ = UV_EPROTO;
        set_status(TCP_CLIENT_ERROR);
        return last_error_code_;
      }
      tls_flush_out();
    }
    tls_drive_handshake();
    tls_flush_out();

    if (!tls_handshake_done_ && !has_status(TCP_CLIENT_ERROR)) {
      const bool hs = wait_for_condition(
          [this]() {
            return tls_handshake_done_ || has_status(TCP_CLIENT_ERROR);
          },
          timeout_ms);
      if (!hs) {
        set_status(TCP_CLIENT_ERROR);
        last_error_code_ = UV_ETIMEDOUT;
        return UV_ETIMEDOUT;
      }
    }
    if (!tls_handshake_done_) {
      // 失败原因由 tls_fail 记在 last_error_code_ 上（证书校验失败、
      // 对端不是 TLS 服务端等都归到 UV_EPROTO）。
      if (last_error_code_ == 0) last_error_code_ = UV_EPROTO;
      set_status(TCP_CLIENT_ERROR);
      return last_error_code_;
    }
    tls_deliver_plain();
  }
#endif

  return sync_connect_result_;
}

// -------------------------------------------------------------------------
// 连接辅助：已解析地址的异步连接 / 主机名同步解析
// -------------------------------------------------------------------------

int uvcpp_tcp_client::connect_async(const struct sockaddr* addr,
                                    std::function<void(int)> cb) {
  // Allocate callback data on heap, pass through C-style trampoline
  // to avoid std::function copy-chain corruption through libuv layers.
  connect_fn_  = trampoline_connect;
  connect_arg_ = new std::function<void(int)>(std::move(cb));

  uvcpp_connect* conn = new uvcpp_connect();
  // 释放交给 callback_connect 在**闭包返回之后**做（见 uvcpp_req::invoke_completion）
  conn->set_self_free(true);

  std::shared_ptr<char> life = alive_token();

  int rc = tcp_->connect(
      conn, addr,
      [this, life](uvcpp_connect* r, int status) {
        (void)r;  // 释放由 callback_connect 事后做
        if (!token_alive(life)) {
          return;  // 对象已析构：请求对象由 trampoline 还回去
        }
        clear_status(TCP_CLIENT_CONNECTING);
        if (status == 0) {
          set_status(TCP_CLIENT_CONNECTED | TCP_CLIENT_READABLE |
                     TCP_CLIENT_WRITABLE);
          // TCP 已经连上：此后是 SOCKS5 协商 / TLS 握手，失败不换地址。
          connect_attempt_tcp_up_ = true;
        } else {
          set_status(TCP_CLIENT_ERROR);
          last_error_code_ = status;
          if (socks5_pending_) socks5_proxy_connect_failed_ = true;
          // 配了代理却连不上代理：把这句也记进 `socks5_last_error()`。
          // 否则调用方只能看到一个裸的 ECONNREFUSED，还得自己去猜"是目标
          // 还是代理"。协商阶段的失败由 `socks5_finish()` 负责写。
          if (socks5_enabled() && socks5_last_error_.empty()) {
            socks5_last_error_ = uv_strerror(status);
          }
        }

        if (status == 0 && socks5_pending_) {
          // 代理连上了，但这条 TCP 还只是"代理控制连接"：先协商，协商成功
          // 才算真正连上目标。握手要靠读事件推进（与 TLS 同一个理由）。
          //
          // 阶段标志到此为止：多地址重试里前一个代理地址失败过，这一笔成功
          // 之后就不再是"连代理失败"了 —— 后面的认证 / REP 失败会重新判。
          socks5_proxy_connect_failed_ = false;
          socks5_pending_ = false;
          if (!read_started_) arm_async_read();
          const int hrc = socks5_begin_handshake(life);
          if (hrc != 0 && token_alive(life)) socks5_finish(hrc, life);
          return;
        }

        continue_after_connect(status, life);
      });

  if (rc != 0) {
    clear_status(TCP_CLIENT_CONNECTING);
    set_status(TCP_CLIENT_ERROR);
    last_error_code_ = rc;
    delete conn;
    if (connect_arg_ != nullptr) {
      delete static_cast<std::function<void(int)>*>(connect_arg_);
      connect_arg_ = nullptr;
    }
    connect_fn_ = nullptr;
    return rc;
  }

  return 0;
}

int uvcpp_tcp_client::connect_sockaddr(const struct sockaddr* addr,
                                       std::function<void(int)> cb) {
  if (addr == nullptr || cb == nullptr) return UV_EINVAL;
  if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6) {
    return UV_EAFNOSUPPORT;
  }
  if (has_async_connect_cb_) return UV_EALREADY;

  has_async_connect_cb_ = true;
  set_status(TCP_CLIENT_CONNECTING);

  const int rc = connect_async(addr, std::move(cb));
  if (rc != 0) {
    // 与 `connect()` 的两条分支一致：没发起成功就把在途标志放掉，否则这条
    // 客户端会被一次立即失败永久毒化（下一次 connect 恒 UV_EALREADY）。
    has_async_connect_cb_ = false;
    clear_status(TCP_CLIENT_CONNECTING);
    if (socks5_pending_) socks5_proxy_connect_failed_ = true;
  }
  return rc;
}

// =========================================================================
// SOCKS5 代理（RFC 1928 / RFC 1929）
// =========================================================================

int uvcpp_tcp_client::set_socks5_proxy(const std::string& host, int port,
                                       const std::string& username,
                                       const std::string& password) {
  if (host.empty()) return UV_EINVAL;
  if (port <= 0 || port > 65535) return UV_EINVAL;
  if (username.size() > 255 || password.size() > 255) return UV_EINVAL;

  // 只能在"这条连接还没开始"的时候改：已经连着的话，改掉的只是一份没人会
  // 再读的配置，而使用者会以为下一次写就走了代理。
  if (has_status(TCP_CLIENT_CONNECTING) || has_status(TCP_CLIENT_CONNECTED) ||
      has_async_connect_cb_ || socks5_phase_ != socks5_phase::NONE ||
      socks5_pending_) {
    last_error_code_ = UV_EBUSY;
    return UV_EBUSY;
  }

  socks5_proxy_enabled_ = true;
  socks5_proxy_host_    = host;
  socks5_proxy_port_    = port;
  socks5_proxy_user_    = username;
  socks5_proxy_pass_    = password;
  socks5_last_error_.clear();
  return 0;
}

void uvcpp_tcp_client::clear_socks5_proxy() {
  if (has_status(TCP_CLIENT_CONNECTING) || has_status(TCP_CLIENT_CONNECTED) ||
      has_async_connect_cb_ || socks5_phase_ != socks5_phase::NONE ||
      socks5_pending_) {
    return;  // 连接进行中/已建立：与 set 同一条理由，静默忽略
  }
  socks5_proxy_enabled_ = false;
  socks5_proxy_host_.clear();
  socks5_proxy_port_ = 0;
  socks5_proxy_user_.clear();
  socks5_proxy_pass_.clear();
  socks5_last_error_.clear();
}

bool uvcpp_tcp_client::socks5_enabled() const {
  return socks5_proxy_enabled_;
}

const std::string& uvcpp_tcp_client::socks5_last_error() const {
  return socks5_last_error_;
}

bool uvcpp_tcp_client::socks5_proxy_connect_failed() const {
  return socks5_proxy_connect_failed_;
}

int uvcpp_tcp_client::socks5_connect_async(const std::string& target_host,
                                           int target_port,
                                           std::function<void(int)> cb) {
  socks5_target_host_ = target_host;
  socks5_target_port_ = target_port;
  socks5_in_.clear();
  socks5_plain_.clear();
  socks5_last_error_.clear();
  socks5_proxy_connect_failed_ = false;
  socks5_phase_  = socks5_phase::NONE;
  socks5_sync_   = false;
  socks5_pending_ = true;

  // 只解析**代理**主机；目标主机名原样交给代理（这正是 SOCKS5 的语义）。
  // `connect_transport` 在完成分支里看到 `socks5_pending_` 就转进协商。
  const int rc = connect_transport(socks5_proxy_host_.c_str(),
                                   socks5_proxy_port_, std::move(cb));
  if (rc != 0) {
    socks5_proxy_connect_failed_ = true;
    socks5_pending_ = false;
  }
  return rc;
}

int uvcpp_tcp_client::socks5_begin_handshake(
    const ::std::shared_ptr<char>& life) {
  socks5_in_.clear();
  socks5_phase_ = socks5_phase::GREETING;

  // 只提供**一种**方法：配了用户名密码就只给 0x02，否则只给 0x00。
  // 同时提供两种会让"服务端选了 no-auth 而使用者以为自己在认证"变成可能，
  // 那是一种静默的降级。
  std::vector<std::uint8_t> methods;
  methods.push_back(socks5_proxy_user_.empty()
                        ? static_cast<std::uint8_t>(uvcpp_socks5_method::NO_AUTH)
                        : static_cast<std::uint8_t>(
                              uvcpp_socks5_method::USERNAME_PASSWORD));

  const std::string greeting = socks5_detail::encode_greeting(methods);
  if (greeting.empty()) {
    socks5_last_error_ = "cannot encode SOCKS5 greeting";
    return UV_EINVAL;
  }
  return socks5_send(greeting, life);
}

int uvcpp_tcp_client::socks5_send(const std::string& bytes,
                                  const ::std::shared_ptr<char>& life) {
  if (bytes.empty()) return UV_EINVAL;

  uvcpp_buf bufcpp(bytes.data(), bytes.size());
  uv_buf_t* raw_buf = bufcpp.out_uv_buf();

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf(raw_buf, true);
  w->set_self_free(true);

  const int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this, life](uvcpp_write* wr, int status) {
        (void)wr;
        if (!token_alive(life)) return;
        if (status != 0) socks5_finish(status, life);
      });

  if (rc != 0) {
    delete w;
    return rc;
  }
  return 0;
}

int uvcpp_tcp_client::socks5_send_request(
    const ::std::shared_ptr<char>& life) {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::DOMAIN_NAME;
  if (!socks5_detail::classify_host(socks5_target_host_, atyp)) {
    socks5_last_error_ = "invalid SOCKS5 target host";
    return UV_EINVAL;
  }
  const uvcpp_socks5_endpoint target(atyp, socks5_target_host_,
                                     socks5_target_port_);
  const std::string req = socks5_detail::encode_request(
      uvcpp_socks5_command::CONNECT, target);
  if (req.empty()) {
    socks5_last_error_ = "cannot encode SOCKS5 CONNECT request";
    return UV_EINVAL;
  }
  return socks5_send(req, life);
}

void uvcpp_tcp_client::socks5_feed(const char* data, std::size_t len,
                                   const ::std::shared_ptr<char>& life) {
  if (socks5_phase_ == socks5_phase::NONE ||
      socks5_phase_ == socks5_phase::DONE) {
    return;
  }
  socks5_in_.append(data, len);

  for (;;) {
    std::size_t used = 0;

    if (socks5_phase_ == socks5_phase::GREETING) {
      uvcpp_socks5_method method = uvcpp_socks5_method::NO_ACCEPTABLE;
      const socks5_detail::parse_status st =
          socks5_detail::parse_method_selection(socks5_in_.data(),
                                                socks5_in_.size(), used,
                                                method);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::MALFORMED) {
        socks5_last_error_ = "malformed method-selection reply";
        socks5_finish(UV_EPROTO, life);
        return;
      }
      socks5_in_.erase(0, used);

      if (method == uvcpp_socks5_method::NO_ACCEPTABLE) {
        socks5_last_error_ = "proxy: no acceptable authentication method";
        socks5_finish(UV_EACCES, life);
        return;
      }
      if (method == uvcpp_socks5_method::USERNAME_PASSWORD) {
        if (socks5_proxy_user_.empty()) {
          socks5_last_error_ =
              "proxy requires username/password, none was configured";
          socks5_finish(UV_EACCES, life);
          return;
        }
        socks5_phase_ = socks5_phase::AUTH;
        const std::string auth = socks5_detail::encode_auth_request(
            socks5_proxy_user_, socks5_proxy_pass_);
        if (auth.empty()) {
          socks5_last_error_ = "invalid username/password";
          socks5_finish(UV_EINVAL, life);
          return;
        }
        const int rc = socks5_send(auth, life);
        if (rc != 0) {
          if (token_alive(life)) socks5_finish(rc, life);
          return;
        }
        continue;
      }
      if (method == uvcpp_socks5_method::NO_AUTH) {
        if (!socks5_proxy_user_.empty()) {
          socks5_last_error_ =
              "proxy selected no-auth although only username/password was "
              "offered";
          socks5_finish(UV_EPROTO, life);
          return;
        }
        socks5_phase_ = socks5_phase::REQUEST;
        const int rc = socks5_send_request(life);
        if (rc != 0) {
          if (token_alive(life)) socks5_finish(rc, life);
          return;
        }
        continue;
      }
      socks5_last_error_ = "unknown authentication method from proxy";
      socks5_finish(UV_EPROTO, life);
      return;
    }

    if (socks5_phase_ == socks5_phase::AUTH) {
      std::uint8_t status = 0xFF;
      const socks5_detail::parse_status st =
          socks5_detail::parse_auth_response(socks5_in_.data(),
                                             socks5_in_.size(), used, status);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::MALFORMED) {
        socks5_last_error_ = "malformed authentication reply";
        socks5_finish(UV_EPROTO, life);
        return;
      }
      socks5_in_.erase(0, used);
      if (status != 0) {
        socks5_last_error_ = "username/password authentication failed";
        socks5_finish(UV_EACCES, life);
        return;
      }
      socks5_phase_ = socks5_phase::REQUEST;
      const int rc = socks5_send_request(life);
      if (rc != 0) {
        if (token_alive(life)) socks5_finish(rc, life);
        return;
      }
      continue;
    }

    if (socks5_phase_ == socks5_phase::REQUEST) {
      uvcpp_socks5_reply rep = uvcpp_socks5_reply::GENERAL_FAILURE;
      uvcpp_socks5_endpoint bound;
      const socks5_detail::parse_status st =
          socks5_detail::parse_reply(socks5_in_.data(), socks5_in_.size(),
                                     used, rep, bound);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::MALFORMED) {
        socks5_last_error_ = "malformed CONNECT reply";
        socks5_finish(UV_EPROTO, life);
        return;
      }
      socks5_in_.erase(0, used);
      if (rep != uvcpp_socks5_reply::SUCCEEDED) {
        socks5_last_error_ = socks5_detail::reply_string(rep);
        socks5_finish(socks5_detail::reply_to_error(rep), life);
        return;
      }

      // 成功。**同一段里可能还跟着目标服务端的应用字节**（隧道已经通了），
      // 先在手上留着，等消费者出现再补投。
      if (!socks5_in_.empty()) {
        socks5_plain_.swap(socks5_in_);
        socks5_in_.clear();
      }
      socks5_finish(0, life);
      return;
    }

    return;
  }
}

void uvcpp_tcp_client::socks5_finish(int error,
                                     const ::std::shared_ptr<char>& life) {
  if (!token_alive(life)) return;

  if (error == 0) {
    socks5_phase_ = socks5_phase::DONE;
    if (socks5_sync_) return;  // 同步路径由 connect_wait 接着走
    continue_after_connect(0, life);
    return;
  }

  socks5_phase_   = socks5_phase::NONE;
  socks5_pending_ = false;
  clear_status(TCP_CLIENT_CONNECTING);
  set_status(TCP_CLIENT_ERROR);
  last_error_code_ = error;
  if (socks5_last_error_.empty()) socks5_last_error_ = uv_strerror(error);

  if (socks5_sync_) return;  // connect_wait 看 last_error_code_

  connect_callback_t fn  = connect_fn_;
  void*              arg = connect_arg_;
  connect_fn_  = nullptr;
  connect_arg_ = nullptr;
  has_async_connect_cb_ = false;
  if (fn != nullptr) fn(error, arg);
}

int uvcpp_tcp_client::socks5_run_sync(int timeout_ms) {
  socks5_sync_ = true;
  const int rc = socks5_begin_handshake(alive_token());
  if (rc != 0) {
    socks5_sync_ = false;
    socks5_phase_ = socks5_phase::NONE;
    socks5_pending_ = false;
    if (socks5_last_error_.empty()) socks5_last_error_ = uv_strerror(rc);
    last_error_code_ = rc;
    return rc;
  }

  const bool done = wait_for_condition(
      [this]() {
        return socks5_phase_ == socks5_phase::DONE ||
               socks5_phase_ == socks5_phase::NONE;
      },
      timeout_ms);
  socks5_sync_ = false;

  if (!done) {
    socks5_phase_ = socks5_phase::NONE;
    socks5_last_error_ = "SOCKS5 handshake timed out";
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }
  if (socks5_phase_ == socks5_phase::NONE) {
    // socks5_finish 已经把具体错误码记下来了
    return last_error_code_ != 0 ? last_error_code_ : UV_EPROTO;
  }
  return 0;
}

void uvcpp_tcp_client::socks5_deliver_plain() {
  if (socks5_plain_.empty()) return;
  // 与服务端自动读同一约定：它不是消费者，留着等真正的消费者。
  if (auto_read_installed_) return;

  if (!net_read_cb_ && read_fn_ == nullptr) {
    if (sync_read_wanted_ && !has_async_read_cb_) {
      ensure_read_cache();
      read_cache_->append_data(socks5_plain_.data(), socks5_plain_.size());
      socks5_plain_.clear();
    }
    return;
  }

  std::string out;
  out.swap(socks5_plain_);

  if (net_read_cb_) {
    net_read_result r;
    r.event = net_read_event::DATA;
    r.data  = out.data();
    r.size  = out.size();
    r.error = 0;
    r.fin   = false;
    uvcpp_net_read_cb cb = net_read_cb_;
    try {
      cb(*this, r);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr,
                   "[uvcpp_tcp_client] read callback threw (unknown)\n");
    }
    return;
  }

  read_callback_t fn  = read_fn_;
  void*           arg = read_arg_;
  if (fn == nullptr) return;
  uvcpp_buf tmp;
  tmp.clone_data(out.data(), out.size());
  fn(&tmp, arg);
}

void uvcpp_tcp_client::continue_after_connect(
    int status, const ::std::shared_ptr<char>& life) {
  if (!token_alive(life)) return;

#if UVCPP_OPENSSL_ENABLE
  if (status == 0 && tls_ssl_ != nullptr) {
    // **握手没完成之前不回调使用者。** 契约是「回调到了就能直接发应用
    // 数据」，而 TLS 连接在握手完成前发不出明文 —— 若在这里就回调，
    // 使用者紧接着的 write() 会撞上还没建立的会话。
    tls_connect_pending_ = true;

    // 握手要靠读事件推进：没人读就永远等不到 ServerHello。
    if (!read_started_) arm_async_read();

    // SOCKS5 协商成功时同一段里可能已经带来目标服务端的 TLS 记录，
    // 直接喂给 SSL 层；失败时 tls_fail 会带着 `tls_connect_pending_` 收尾。
    if (!socks5_plain_.empty()) {
      std::string pending;
      pending.swap(socks5_plain_);
      if (tls_feed(pending.data(), pending.size()) != 0) return;
      if (!token_alive(life)) return;
      tls_flush_out();
    }

    tls_drive_handshake();
    if (token_alive(life)) tls_flush_out();
    if (token_alive(life) && tls_handshake_done_) {
      tls_deliver_plain();  // 此阶段还没有消费者，会先攒着
      if (token_alive(life)) tls_complete_connect();
    }
    return;
  }
#endif

  // 明文：SOCKS5 之后一起到的字节先过一遍补投（此刻还没有消费者，
  // 所以实际上只是留在 socks5_plain_ 里等 read_start/read_wait）。
  socks5_deliver_plain();

  // 用户回调在**闭包还在栈上**的时候调用（invoke_completion 已经把
  // m_connect_cb 搬走了），所以这里碰 this 是安全的。
  if (connect_fn_) {
    connect_fn_(status, connect_arg_);
    connect_fn_  = nullptr;
    connect_arg_ = nullptr;
  }
}

int uvcpp_tcp_client::resolve_host_sync_all(const char* host, int port,
                                            std::vector<sockaddr_storage>& out,
                                            int timeout_ms) {
  out.clear();

  // 数字 IPv4 快速路径（无需 DNS）
  struct sockaddr_in addr4;
  if (uv_ip4_addr(host, port, &addr4) == 0) {
    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    std::memcpy(&ss, &addr4, sizeof(addr4));
    out.push_back(ss);
    return 0;
  }

  // 数字 IPv6 快速路径（与 connect() 同一条规则）
  struct sockaddr_in6 addr6;
  if (uv_ip6_addr(host, port, &addr6) == 0) {
    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    std::memcpy(&ss, &addr6, sizeof(addr6));
    out.push_back(ss);
    return 0;
  }

  // 主机名：DNS 解析（阻塞等待完成）。**AF_UNSPEC**：A 与 AAAA 都收。
  uvcpp_getaddrinfo resolver;
  char service[16];
  uvcpp_snprintf(service, sizeof(service), "%d", port);

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  bool done = false;
  int  status = 0;
  struct addrinfo* res = nullptr;

  int rc = resolver.getaddrinfo(
      loop_, host, service, &hints,
      [&done, &status, &res](uvcpp_getaddrinfo*, int st, struct addrinfo* r) {
        status = st;
        res    = r;
        done   = true;
      });
  if (rc != 0) return rc;

  if (!wait_for_condition([&done]() { return done; }, timeout_ms)) {
    return UV_ETIMEDOUT;
  }

  if (status != 0 || res == nullptr) {
    if (res != nullptr) resolver.freeaddrinfo(res);
    return (status != 0) ? status : UV_EAI_NONAME;
  }

  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
    if (p->ai_addr == nullptr || p->ai_addrlen == 0) continue;
    if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    std::memcpy(&ss, p->ai_addr,
                static_cast<size_t>(p->ai_addrlen) > sizeof(ss)
                    ? sizeof(ss)
                    : static_cast<size_t>(p->ai_addrlen));
    out.push_back(ss);
  }
  resolver.freeaddrinfo(res);

  if (out.empty()) return UV_EAI_NONAME;
  return 0;
}

int uvcpp_tcp_client::resolve_host_sync(const char* host, int port,
                                        sockaddr_storage& out,
                                        int timeout_ms) {
  std::vector<sockaddr_storage> all;
  const int rc = resolve_host_sync_all(host, port, all, timeout_ms);
  if (rc != 0) return rc;
  out = all.front();
  return 0;
}

// =========================================================================
// Write
// =========================================================================

int uvcpp_tcp_client::write(const char* data, size_t len,
                             std::function<void(int)> cb) {
  if (!has_status(TCP_CLIENT_CONNECTED)) {
    return UV_ENOTCONN;
  }

  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_write_cb_) {
      return UV_EALREADY;
    }

    has_async_write_cb_ = true;

    write_fn_  = trampoline_write;
    // **`std::move` 是承重的**：按值收下的 `cb` 到这里已经没人要了，而
    // 拷贝一个 `std::function` 会把它捕获的东西整份复制一遍（捕获里有
    // `shared_ptr` 时还多两次原子引用计数）。移动只是把内部指针接过来。
    write_arg_ = new std::function<void(int)>(std::move(cb));

#if UVCPP_OPENSSL_ENABLE
    if (tls_ssl_ != nullptr) {
      // 把成员恢复原状再报错 —— 调用方收到非 0 就应当认为这次写没发生。
      if (!tls_handshake_done_) {
        has_async_write_cb_ = false;
        delete static_cast<std::function<void(int)>*>(write_arg_);
        write_fn_  = nullptr;
        write_arg_ = nullptr;
        return UV_ENOTCONN;
      }

      const int trc = tls_write_plain(data, len);
      if (trc != 0) {
        last_error_code_ = trc;
        has_async_write_cb_ = false;
        delete static_cast<std::function<void(int)>*>(write_arg_);
        write_fn_  = nullptr;
        write_arg_ = nullptr;
        return trc;
      }

      // 明文已经全交给 SSL 了，密文还在 wbio 里 —— 等它出网才算完成。
      tls_write_pending_ = true;
      tls_write_sync_    = false;
      tls_flush_out();
      return 0;
    }
#endif

#if UVCPP_TRY_WRITE_ENABLE
    // ---------------------------------------------------------------------
    // `uv_try_write` 快速路径
    // ---------------------------------------------------------------------
    // 下面 `uvcpp_buf bufcpp(...)` 拷的那一份，**唯一用途**是让字节活过这次异步
    // 写。但套接字常常一次就吃得下整条 —— 吃下的那部分已经出去了，根本不需要有
    // 东西活着。吃下的部分越大，要拷的那份越小；整条吃下就是不拷。
    //
    // **交付方式一个字都没改**：余量（整条吃下时为 0）照旧交给一次真正的
    // `uv_write`，完成回调仍由 libuv 自己的写完成机制交付。这一条是刻意的，
    // 不是省事：
    //
    //   * 时机不变。从读回调里发起的写，完成仍可能在同一轮内就绪（本机实测
    //     距 `write()` 返回 0 轮，与改动前逐档相同）；循环外发起的写仍是 1 轮。
    //   * 背压记账不变。`uv_write` 仍是唯一入口，`uv__tcp_try_write` 依赖的
    //     `write_reqs_pending` 记账、`write_queue_size` 全部原样。
    //
    // 试过让快路径自己交付（懒建一个 `uvcpp_idle`、下一轮回调），省下的拷贝与
    // 这里一样多，但那**会让交付晚一轮**，而晚一轮是可观测的：`uvcpp_ws_server::
    // handle_upgrade` 是在 101 那笔写的完成回调里建 WS 会话的，晚一轮就出现
    // "对端已经收到 101、服务端会话还没建" 的窗口。同一份二进制里用环境变量切换
    // 两种交付实测（`tests/functional/web_ws_deflate_func.exe`，各 20 次）：
    // 本实现 20/20 通过；idle 交付 11/20，另 5 次断言失败、4 次段错误。所以这条
    // 路不采用 —— 快路径的收益不值得动交付时序。
    //
    // 两条必须守住的：
    //
    //   - **非 0 返回码的契约**。`write()` 的既有契约是"返回非 0 ⇒ 这次写**没
    //     提交**"。所以 `uv_try_write` 的负返回（EAGAIN / EALREADY / 真实错误
    //     如 ECONNRESET）一概**不许**从这一支抛出去 —— 负返回时 `wl` 保持
    //     `len`，自然落回下面那条原路径，真实错误由回退那次 `uv_write` 的完成
    //     回调如实交付。
    //   - **部分接受不得重发前缀**。`n` 在 (0, len) 之间时，已经出去的 n 字节
    //     不能再进待发缓冲；余量只能是 `data + n, len - n`。今天的代码是把整条
    //     `data, len` 拷进去，直接套过来就会把前 n 字节发两遍 —— 对端收到重复
    //     字节，而且是"缓冲不紧时看不出、一紧就错"的形状。
    //
    // ---------------------------------------------------------------------
    // 为什么有个长度门槛（`UVCPP_TRY_WRITE_MIN_BYTES`）
    // ---------------------------------------------------------------------
    // 拷贝放大比**看不见每笔写的固定成本**：快路径每笔写恰好多一次系统调用
    // —— 整条吃下时那次 `uv_write` 走的是 0 长度，而 libuv 在两个平台上都
    // **不对 0 长度短路**（Unix `uv_try_write2` 过掉 guard 直接进 `writev`；
    // Windows `uv__tcp_try_write` → `WSASend`）。于是每笔写的账是：
    //
    //   基线   = memcpy(len) + malloc(uvcpp_buf) + 一次带数据的系统调用
    //   快路径 = 一次带数据的系统调用 + 一次 0 长度系统调用（整条吃下时不分配）
    //   Δ      = t_syscall(0) − t_memcpy(len) − t_malloc
    //
    // 三项分别实测（Windows / MSVC / Release / loopback，20 万次取分布）：
    // 0 长度那次系统调用 311~365 ns、memcpy 4 KiB 163~165 ns、16 KiB
    // 220~398 ns、64 KiB 1275~1397 ns、4 MiB 135~138 µs、省掉的 malloc 24.8 ns。
    // 代进去：4 KiB 约 **+148 ns（亏）**、16 KiB 在 ±90 ns 之间**来回翻**、
    // 64 KiB 起净赚（−963 ns 到 −135 µs，随长度线性长）。
    //
    // 也就是说**零点在 16 KiB 附近**，低于它每笔写净亏几十到三百纳秒 —— 而
    // Web 响应最常见的 4 KiB 恰好落在净亏区（那一档拷贝放大比从 3.11× 降到
    // 2.05×，墙钟却是每响应多约 150 ns）。门槛取 32 KiB：零点在 16 KiB 附近
    // **而且就在那儿来回翻**，取高一点更稳；64 KiB 以上那些档（真正值钱的）
    // 一分不少，小报文回到今天的行为 —— 它们本来也省不下什么。
    //
    // 量级说清楚：4 KiB 上那 ~150 ns 放在一条几十微秒的请求上约 0.2%，不是
    // 事故。门槛的意义是让这个改动**严格更好**，不是修 bug。绝对值随机器与
    // libuv 版本会变，但小尺寸那一侧的方向是稳的（十几纳秒的 memcpy 对三百
    // 多纳秒的一次 `WSASend`，差一个数量级，不是调度噪声能翻过来的）；真实
    // 网络里 socket 缓冲更容易满、试发更容易退化成 EAGAIN，那只会让快路径
    // **更少**被走到，不改变"走到了也是亏"这一侧。
    const char* wp = data;
    size_t      wl = len;
    if (len >= UVCPP_TRY_WRITE_MIN_BYTES) {
      g_trywrite_attempted.fetch_add(1, std::memory_order_relaxed);
      // 长度超过 `UINT_MAX` 时 `uv_buf_init` 会把 `len` 截断 —— 不影响正确性：
      // 试发的**实际**字节数是返回值 `n`，余量一律由 `len - n` 算。
      uv_buf_t probe = uv_buf_init(const_cast<char*>(data),
                                   static_cast<unsigned int>(len));
      const int n = tcp_->try_write(&probe, 1);
      if (n > 0) {
        wp = data + static_cast<size_t>(n);
        wl = len - static_cast<size_t>(n);
        g_trywrite_consumed.fetch_add(1, std::memory_order_relaxed);
        g_trywrite_bytes.fetch_add(static_cast<uint64_t>(n),
                                   std::memory_order_relaxed);
      }
      // 负返回走这里：`wl` 还是 `len`，整条照旧入队，负返回不外泄。
    } else {
      // 没过门槛：连试都不试，直接走今天的路径。
      g_trywrite_skipped.fetch_add(1, std::memory_order_relaxed);
    }

    // 整条吃下时 `wl == 0`：这一份是空块（`uvcpp_buf` 对 0 长度不分配也不拷，
    // 得到一个 `{nullptr, 0}`），下面那次 `uv_write` 只负责**交付完成回调**，
    // 一个字节都不会重发。0 长度的 `uv_write` 不是新形状 —— 调用方本来就可以
    // `write(data, 0)`，那时上游走到的是同一处。值形态下这个空块的 base 是
    // nullptr，于是这一笔连包装带块一共两次分配都省掉了。
    //
    // 交接走 `release_uv_buf()`（值形态）而不是 `out_uv_buf()`，与 4 参重载同一
    // 条理由：后者那个 `uv_buf_t` 包装只是中转 —— `uvcpp_write` 本来就把
    // `uv_buf_t` 按值存着（`first_block_`）—— 于是每笔异步写白付一次分配、一次
    // 释放（WS 回声那条路实测各 1.00 次/消息）。归属一字不改：块还是交给
    // `uvcpp_write`，只是不再有包装要放。
    uvcpp_buf bufcpp(wp, wl);
    uv_buf_t  hb;
    bufcpp.release_uv_buf(&hb);
#else
    uvcpp_buf bufcpp(data, len);
    uv_buf_t  hb;
    bufcpp.release_uv_buf(&hb);
#endif

    uvcpp_write* w = new uvcpp_write();
    w->set_uv_buf_block(hb);
    // 请求对象的释放交给 callback_write 在**回调返回之后**做。回调里 `delete wr`
    // 删的是正在执行的闭包本身（见 uvcpp_write::callback_write）。
    w->set_self_free(true);

    std::shared_ptr<char> life = alive_token();

    int rc = tcp_->write(
        w, w->get_uv_buf(), 1,
        [this, life](uvcpp_write* wr, int status) {
          (void)wr;
          // 本对象已经析构：`write_fn_` / `write_arg_` / `last_error_code_`
          // 全都不能碰了 —— 直接返回即可，请求对象由 trampoline 还回去。
          if (!token_alive(life)) {
            return;
          }
          if (status != 0) {
            last_error_code_ = status;
          }
          if (write_fn_) {
            // 先把成员清干净、**再**回调。次序是有讲究的：
            // - `has_async_write_cb_` 必须在回调**之前**清。回调里最自然的事就是
            //   接着发起下一次写（WS 的发送队列、Phase 3 的流式写都这么做），
            //   而 write() 见到这个标志还立着会直接返回 UV_EALREADY —— 下一次写
            //   会被判成失败。
            // - `write_fn_` / `write_arg_` 也必须先存后清：回调里新装上的回调
            //   属于**下一次**写，如果在回调返回后无条件置空，就会把它擦掉。
            write_callback_t fn  = write_fn_;
            void*            arg = write_arg_;
            write_fn_  = nullptr;
            write_arg_ = nullptr;
            has_async_write_cb_ = false;
            fn(status, arg);
          } else {
            has_async_write_cb_ = false;
          }
        });

    if (rc != 0) {
      last_error_code_ = rc;
      delete w;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
      // 提交失败这条路上也必须清 —— 与成功路径同一判据。漏了它，一次失败的
      // 异步写会把连接**永久**毒化：此后每一次异步写都拿到 UV_EALREADY。
      has_async_write_cb_ = false;
      return rc;
    }

    return 0;
  } else {
    // --- Sync mode ---
    return write_wait(data, len, 30000);
  }
}

int uvcpp_tcp_client::write_wait(const char* data, size_t len,
                                  int timeout_ms) {
  if (!has_status(TCP_CLIENT_CONNECTED)) {
    return UV_ENOTCONN;
  }

  if (has_async_write_cb_) {
    throw std::runtime_error(
        "uvcpp_tcp_client::write_wait: cannot use sync write after "
        "async write callback was registered");
  }

  // Reset sync state
  sync_write_done_   = false;
  sync_write_result_ = 0;

#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) {
    if (!tls_handshake_done_) return UV_ENOTCONN;

    const int trc = tls_write_plain(data, len);
    if (trc != 0) {
      last_error_code_ = trc;
      return trc;
    }
    tls_write_pending_ = true;
    tls_write_sync_    = true;
    tls_flush_out();

    const bool ok = wait_for_condition(
        [this]() { return sync_write_done_; }, timeout_ms);
    if (!ok) {
      // 超时不代表密文没出去 —— 它可能已经出网、只是完成回调还没送到。
      // 如实按超时报告，调用方据此判断连接状态即可。
      tls_write_pending_ = false;
      tls_write_sync_    = false;
      last_error_code_   = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    return sync_write_result_;
  }
#endif

  uvcpp_buf bufcpp(data, len);            // stack-allocated helper
  uv_buf_t* raw_buf = bufcpp.out_uv_buf();  // transfers data ownership to raw_buf

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf(raw_buf, true);           // w takes ownership of raw_buf and its data
  w->set_self_free(true);                 // 释放挪到 callback_write 里，见该类说明

  // 同步写也可能超时返回，把完成回调留在 libuv 队列里 —— 之后对象被销毁，
  // 那个回调照样会来。所以同步路径用同一套令牌。
  std::shared_ptr<char> life = alive_token();

  int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this, life](uvcpp_write* wr, int status) {
        (void)wr;
        if (!token_alive(life)) {
          return;
        }
        if (status != 0) {
          last_error_code_ = status;
        }
        sync_write_result_ = status;
        sync_write_done_   = true;
      });

  if (rc != 0) {
    last_error_code_ = rc;
    delete w;  // 也放掉第 1 块（值形态，块归 uvcpp_write）
    return rc;
  }

  bool completed = wait_for_condition(
      [this]() { return sync_write_done_; }, timeout_ms);

  if (!completed) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  return sync_write_result_;
}

// uvcpp_buf& overload (copy) — delegates to const char* version
int uvcpp_tcp_client::write(const uvcpp_buf& buf,
                             std::function<void(int)> cb) {
  return write(buf.get_const_data(), buf.size(), cb);
}

int uvcpp_tcp_client::write_wait(const uvcpp_buf& buf, int timeout_ms) {
  return write_wait(buf.get_const_data(), buf.size(), timeout_ms);
}

// 头 + 体两块（`nbufs = 2`）：体不拷。见头文件里那段说明。
//
// 这里**不**做 `uv_try_write` 试发（上面 `const char*` 重载里那一套）。理由写
// 在头文件里，一句话：那一支试发是为了省下"本层自己拷的那一份"里已经出去的
// 前缀，这条路上体那份拷贝根本不存在，试发就只剩一次多余的系统调用。
int uvcpp_tcp_client::write(const char* head, size_t head_len,
                            uvcpp_buf* body, std::function<void(int)> cb) {
  if (body == nullptr) return UV_EINVAL;
  if (!has_status(TCP_CLIENT_CONNECTED)) return UV_ENOTCONN;

  if (cb == nullptr) {
    // 同步分支没有"等回调"这回事，两块就地合并一次走原路 —— 这次的拷贝换到
    // 的是一个简单可靠的语义：`write_wait` 返回时写已经完成。
    ::std::string merged(head, head_len);
    merged.append(body->get_const_data(), body->size());
    return write_wait(merged.data(), merged.size(), 30000);
  }

  if (has_async_write_cb_) return UV_EALREADY;

  has_async_write_cb_ = true;
  write_fn_  = trampoline_write;
  // **`std::move` 是承重的**：按值收下的 `cb` 到这里已经没人要了，而
  // 拷贝一个 `std::function` 会把它捕获的东西整份复制一遍（捕获里有
  // `shared_ptr` 时还多两次原子引用计数）。移动只是把内部指针接过来。
  write_arg_ = new std::function<void(int)>(std::move(cb));

#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) {
    // 与 `write(uvcpp_buf*)` 同一个理由：TLS 下明文要经 `SSL_write`（它**拷**
    // 进去），所以"两块"在这里没有意义，退回合并成一条。回滚也与那两个重载
    // 逐字一致。
    if (!tls_handshake_done_) {
      has_async_write_cb_ = false;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
      return UV_ENOTCONN;
    }

    ::std::string merged(head, head_len);
    merged.append(body->get_const_data(), body->size());
    const int trc = tls_write_plain(merged.data(), merged.size());
    if (trc != 0) {
      last_error_code_ = trc;
      has_async_write_cb_ = false;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
      return trc;
    }

    tls_write_pending_ = true;
    tls_write_sync_    = false;
    tls_flush_out();
    return 0;
  }
#endif

  // 第 1 块：头。仍然照旧拷进一个自有块 —— 这条路上剩下的唯一一份拷贝。
  // 交接走 `release_uv_buf()`（值形态）而不是 `out_uv_buf()`：后者会多分一个
  // `uv_buf_t` 包装，而这个包装只是中转 —— `uvcpp_write` 本来就把 `uv_buf_t`
  // 按值存着。块的所有权一字不改。
  uvcpp_buf headbuf(head, head_len);
  uv_buf_t  hb;
  headbuf.release_uv_buf(&hb);

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf_block(hb);

  // 第 2 块：体。判据与顺序**只有一份**，在 `uvcpp_write::adopt_body` 里
  // （共享视图接引用计数、自有块接所有权，且 `release_uv_buf()` 会 materialize
  // 所以顺序不能反）—— 复用形态 `write_owned` 走的是同一个方法。
  w->adopt_body(body);
  w->set_self_free(true);

  std::shared_ptr<char> life = alive_token();

  int rc = tcp_->write(w, w->get_uv_bufs(),
                       static_cast<unsigned int>(w->get_uv_nbufs()),
                       [this, life](uvcpp_write* wr, int status) {
    (void)wr;
    if (!token_alive(life)) {
      return;  // 对象已析构：请求对象由 trampoline 还回去
    }
    if (status != 0) last_error_code_ = status;
    // 次序与另外两个重载一致：先存后清，且 `has_async_write_cb_` 必须在回调
    // **之前**清 —— 否则回调里接着发起下一次写会拿到 UV_EALREADY。
    write_callback_t fn = write_fn_;
    void* arg = write_arg_;
    write_fn_  = nullptr;
    write_arg_ = nullptr;
    has_async_write_cb_ = false;
    if (fn) fn(status, arg);
  });

  if (rc != 0) {
    last_error_code_ = rc;
    delete w;
    delete static_cast<std::function<void(int)>*>(write_arg_);
    write_fn_  = nullptr;
    write_arg_ = nullptr;
    has_async_write_cb_ = false;
    return rc;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 复用形态：请求对象与头部缓冲都由调用方持有，完成回调是**函数指针**
// ---------------------------------------------------------------------------
//
// 与上面那个 4 参重载的差别**只在归属**：那边请求对象与它的头部块都是这条路上
// 每笔现 `new` 的（还外加一个 `new std::function<void(int)>` 和一个捕获
// `shared_ptr` 的跳板闭包）。这条路把三样都去掉：
//
//   * 请求对象由调用方 new 一次、按连接复用 ⇒ `uv_write_t` 那一笔也跟着从
//     每请求降到每连接（它就在 `uvcpp_write` 的构造里）；
//   * 头部字节拷进请求自己那份按连接复用的缓冲（`owned_write_state`）；
//   * 完成回调是个**静态函数**：`uvcpp_stream::write` 把 `std::function` 按值
//     收下、`std::move` 到底 —— 对平凡可拷贝、且尺寸进得了小对象缓冲的目标
//     是就地存储（GCC 的判据是 `__is_location_invariant`，与 sizeof 无关），
//     而捕获 `shared_ptr` 的闭包必然落堆。这条路上一次分配都没有。
int uvcpp_tcp_client::write_owned(uvcpp_write* w, const char* head,
                                  size_t head_len, uvcpp_buf* body,
                                  owned_write_cb_t fn, void* arg) {
  if (w == nullptr || fn == nullptr) return UV_EINVAL;
  if (!has_status(TCP_CLIENT_CONNECTED)) return UV_ENOTCONN;
  if (has_async_write_cb_) return UV_EALREADY;

  owned_write_state* st = static_cast<owned_write_state*>(w->get_data());
  if (st == nullptr) {
    // 首次使用：这块状态跟着请求对象生灭（见 owned_write_state 的说明）。
    st    = new owned_write_state();
    st->w = w;
    w->set_data(st);
  }
  st->client = this;
  st->fn     = fn;
  st->arg    = arg;

#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) {
    // 与 4 参重载同一个理由：TLS 下明文要经 `SSL_write`（它**拷**进去），
    // 所以"两块"在这里没有意义，退回合并成一条。
    //
    // **这一段必须排在消费 head/body 之前**：下面 `adopt_body()` 会把体块的
    // 所有权接手走（之后 `body->size()` 可能已经是 0），4 参重载把这段排在
    // 消费之前 —— 那个顺序是承重的，这里照排（顺手也不再白拷一次头部）。
    if (!tls_handshake_done_) {
      st->client = nullptr;
      st->fn     = nullptr;
      st->arg    = nullptr;
      return UV_ENOTCONN;
    }

    ::std::string merged(head, head_len);
    if (body != nullptr && body->size() > 0) {
      merged.append(body->get_const_data(), body->size());
    }
    const int trc = tls_write_plain(merged.data(), merged.size());
    if (trc != 0) {
      last_error_code_ = trc;
      st->client = nullptr;
      st->fn     = nullptr;
      st->arg    = nullptr;
      return trc;
    }

    // 标记在**发起之前**立：`tls_flush_out()` 可能同步跑完并回头调
    // `trampoline_owned`，那时这两个成员得已经在位。
    has_async_write_cb_ = true;
    write_fn_  = trampoline_owned;
    write_arg_ = st;
    tls_write_pending_ = true;
    tls_write_sync_    = false;
    tls_flush_out();  // 完成时走 trampoline_owned → owned_write_done
    return 0;
  }
#endif

  // 第 1 块：头部。容量够就地 memcpy，不够才分配 —— 稳态（同一条连接的每一笔
  // 响应）零分配。这块字节要活到完成回调，由"缓冲随状态、状态随请求"履行。
  //
  // 分配失败按本仓的既有形状**抛出**（`uvcpp_alloc_bytes` 会 memset 并抛
  // `bad_alloc`，不返回 nullptr）。这里刻意把在途标记留在分配**之后**立 ——
  // 抛出去时本对象仍是"没在写"的干净状态（4 参重载在这一点上更糙：它的
  // `new uvcpp_write()` 落在标记之后，抛出去会留下一个永远在途的客户端）。
  if (st->head_cap < head_len) {
    void* nb = uvcpp::uvcpp_alloc_bytes(head_len);
    if (st->head_buf != nullptr) uvcpp::uvcpp_free(st->head_buf);
    st->head_buf = static_cast<char*>(nb);
    st->head_cap = head_len;
  }
  if (head_len > 0 && head != nullptr) {
    memcpy(st->head_buf, head, head_len);
  }
  st->head = uv_buf_init(st->head_buf, static_cast<unsigned int>(head_len));

  // 复用前先复位：上一笔的体（自有块 / 视图引用）必须在这里放掉，否则紧接着的
  // `set_uv_buf` 会把 `nbufs_` 归 1，那个槽位里的旧占用者就再也没人放了。
  w->reset_for_reuse();
  w->set_uv_buf(&st->head, /*owner=*/false);  // 借：字节归按连接那份缓冲
  w->adopt_body(body);   // 体：与 4 参重载同一份判据（**顺序也只此一份**）
  // 归属调用方：本类**绝不** delete 它（连"客户端已析构"那条路上也只是把它
  // 连同状态一起放掉，见 owned_write_done）。
  w->set_self_free(false);

  has_async_write_cb_ = true;
  write_fn_  = trampoline_owned;
  write_arg_ = st;

  const int rc = tcp_->write(
      w, w->get_uv_bufs(), static_cast<unsigned int>(w->get_uv_nbufs()),
      &uvcpp_tcp_client::owned_write_done);

  if (rc != 0) {
    // 提交失败：调用方收到非 0 就应当认为这次写没发生 —— 成员恢复原状，请求
    // 对象仍归调用方（它会还回自己的槽继续复用）。
    last_error_code_ = rc;
    has_async_write_cb_ = false;
    write_fn_  = nullptr;
    write_arg_ = nullptr;
    st->client = nullptr;
    st->fn     = nullptr;
    st->arg    = nullptr;
    return rc;
  }
  return 0;
}

void uvcpp_tcp_client::owned_write_done(uvcpp_write* w, int status) {
  owned_write_state* st =
      (w != nullptr) ? static_cast<owned_write_state*>(w->get_data()) : nullptr;
  if (st == nullptr) {
    // 构造上的兜底：状态与请求同生共死，走到这里说明有人 hand-roll 了请求对象。
    delete w;
    return;
  }
  uvcpp_tcp_client* c = st->client;
  if (c == nullptr) {
    // 客户端已经析构（析构函数把这一格置空）：`c` 与本对象的成员一个都不能碰，
    // 调用方的回调也不调（它的 `client` / `arg` 此刻已经不可信）。唯一还能做的
    // 就是替调用方把请求连同状态还回去。
    release_owned_write(w);
    return;
  }
  c->finish_owned_write(status);

  owned_write_cb_t fn = st->fn;
  void*            arg = st->arg;
  st->fn  = nullptr;
  st->arg = nullptr;
  if (fn != nullptr) {
    // 请求的去向由调用方定：还回按连接的槽（复用）或 `release_owned_write`。
    //
    // 这里删 `w` 是**安全**的：`uvcpp_write::callback_write` 走
    // `uvcpp_req::invoke_completion`，那里"读 `is_self_free()` 在调闭包之前、
    // 按它删对象在闭包返回之后"，闭包返回后一个成员都不再碰 —— 所以调用方在
    // 本回调里把 `w` 删掉是本仓既有的合法形状（那个函数的注释就是这么写的：
    // "回调拿到 `self`，有权把它删掉"）。
    fn(w, c, status, arg);
  } else {
    release_owned_write(w);
  }
}

void uvcpp_tcp_client::finish_owned_write(int status) {
  if (status != 0) last_error_code_ = status;
  // 次序与另外几个重载一致：先存后清，且 `has_async_write_cb_` 必须在回调
  // **之前**清 —— 否则回调里接着发起下一次写会拿到 UV_EALREADY。
  write_fn_  = nullptr;
  write_arg_ = nullptr;
  has_async_write_cb_ = false;
}

void uvcpp_tcp_client::release_owned_write(uvcpp_write* w) {
  if (w == nullptr) return;
  // 状态（含头部缓冲）归请求对象，先放状态再放请求。请求的 `uv_buf` 指着状态
  // 里那个 `uv_buf_t`，而它是按"借"登记的（owner=false）⇒ `~uvcpp_write` 不会
  // 去 free 它（它只在 `uv_buf_owner` 为真时释放）—— 顺序在这一点上不承重，
  // 写清楚只为下一个人不必再推一遍。
  delete static_cast<owned_write_state*>(w->get_data());
  w->set_data(nullptr);
  delete w;
}

void uvcpp_tcp_client::trampoline_owned(int status, void* arg) {
  // TLS 收尾那条路的跳板（`tls_finish_write` 走 `fn(status, arg)`），无捕获 ——
  // `std::function` 就地存储。明文路**不经过这里**：那条路的完成回调是
  // `owned_write_done`，由 `uvcpp_write::m_write_cb` 直接送达。
  auto* st = static_cast<owned_write_state*>(arg);
  if (st == nullptr || st->w == nullptr) return;
  owned_write_done(st->w, status);
}

// uvcpp_buf* overload (zero-copy, transfers ownership of buffer data)
//
// TLS 连接上"零拷贝"不成立：明文要经 SSL_write 加密，它是**拷**进去的。所以
// TLS 分支既不走 `out_uv_buf()` 也不碰调用方的缓冲，见下面两个函数里的分支。
int uvcpp_tcp_client::write(uvcpp_buf* buf,
                             std::function<void(int)> cb) {
  if (buf == nullptr) return UV_EINVAL;
  if (!has_status(TCP_CLIENT_CONNECTED)) return UV_ENOTCONN;

  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_write_cb_) return UV_EALREADY;

    has_async_write_cb_ = true;
    write_fn_  = trampoline_write;
    // **`std::move` 是承重的**：按值收下的 `cb` 到这里已经没人要了，而
    // 拷贝一个 `std::function` 会把它捕获的东西整份复制一遍（捕获里有
    // `shared_ptr` 时还多两次原子引用计数）。移动只是把内部指针接过来。
    write_arg_ = new std::function<void(int)>(std::move(cb));

#if UVCPP_OPENSSL_ENABLE
    if (tls_ssl_ != nullptr) {
      // TLS 下**不能**像下面那样把 buf 的数据挪走。密文归 SSL 所有，明文是被
      // **拷**进它的 wbio 的（见 `tls_write_plain`），所以这块内存在本函数返回
      // 之后仍然归调用方 —— 挪走了，调用方回调里那次 delete 就会悬垂。
      if (!tls_handshake_done_) {
        has_async_write_cb_ = false;
        delete static_cast<std::function<void(int)>*>(write_arg_);
        write_fn_  = nullptr;
        write_arg_ = nullptr;
        return UV_ENOTCONN;
      }

      const int trc = tls_write_plain(buf->get_const_data(), buf->size());
      if (trc != 0) {
        last_error_code_ = trc;
        has_async_write_cb_ = false;
        delete static_cast<std::function<void(int)>*>(write_arg_);
        write_fn_  = nullptr;
        write_arg_ = nullptr;
        return trc;
      }

      // 明文已经全交给 SSL 了，密文还在 wbio 里 —— 等它出网才算完成。
      tls_write_pending_ = true;
      tls_write_sync_    = false;
      tls_flush_out();
      return 0;
    }
#endif

    uvcpp_write* w = new uvcpp_write();
    if (buf->is_shared()) {
      // 共享视图：**不** materialize，直接把那块借出去 —— 与 `uvcpp_write::
      // adopt_body()` 的 `is_shared()` 支线同一条规则（见 `src/req/uvcpp_write.cpp`）。
      // `out_uv_buf()` 会把共享视图**先物化成一份自有块**（见 `uvcpp_buf.h` 里那条
      // "它不是错误，是一次白拷贝"），所以顺序不能反：先判共享、再谈接走所有权。
      w->append_uv_buf_view(
          uv_buf_init(const_cast<char*>(buf->get_const_data()),
                      static_cast<unsigned int>(buf->size())),
          buf->shared_ref());
    } else {
      uv_buf_t* raw = buf->out_uv_buf();  // transfers ownership from buf
      w->set_uv_buf(raw, true);
    }
    w->set_self_free(true);  // 释放挪到 callback_write 里，见该类说明

    std::shared_ptr<char> life = alive_token();

    int rc = tcp_->write(
        w, w->get_uv_bufs(), static_cast<unsigned int>(w->get_uv_nbufs()),
        [this, life](uvcpp_write* wr, int status) {
          (void)wr;
          if (!token_alive(life)) {
            return;  // 对象已析构：请求对象由 trampoline 还回去
          }
          if (status != 0) last_error_code_ = status;
          // 次序与 const char* 重载一致：先存后清，且 `has_async_write_cb_` 必须在
          // 回调**之前**清 —— 否则回调里接着发起下一次写会拿到 UV_EALREADY。
          write_callback_t fn = write_fn_;
          void* arg = write_arg_;
          write_fn_  = nullptr;
          write_arg_ = nullptr;
          has_async_write_cb_ = false;
          if (fn) fn(status, arg);
        });

    if (rc != 0) {
      last_error_code_ = rc;
      delete w;
      delete static_cast<std::function<void(int)>*>(write_arg_);
      write_fn_  = nullptr;
      write_arg_ = nullptr;
      has_async_write_cb_ = false;
      return rc;
    }
    return 0;
  } else {
    return write_wait(buf, 30000);
  }
}

int uvcpp_tcp_client::write_wait(uvcpp_buf* buf, int timeout_ms) {
  if (buf == nullptr) return UV_EINVAL;
  if (!has_status(TCP_CLIENT_CONNECTED)) return UV_ENOTCONN;
  if (has_async_write_cb_)
    throw std::runtime_error(
        "uvcpp_tcp_client::write_wait: cannot use sync write after "
        "async write callback was registered");

  sync_write_done_   = false;
  sync_write_result_ = 0;

#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) {
    // 与异步重载同一个理由：TLS 下 buf 的数据不被挪走，仍归调用方。
    if (!tls_handshake_done_) return UV_ENOTCONN;

    const int trc = tls_write_plain(buf->get_const_data(), buf->size());
    if (trc != 0) {
      last_error_code_ = trc;
      return trc;
    }
    tls_write_pending_ = true;
    tls_write_sync_    = true;
    tls_flush_out();

    const bool ok = wait_for_condition(
        [this]() { return sync_write_done_; }, timeout_ms);
    if (!ok) {
      // 超时不代表密文没出去 —— 它可能已经出网、只是完成回调还没送到。
      tls_write_pending_ = false;
      tls_write_sync_    = false;
      last_error_code_   = UV_ETIMEDOUT;
      return UV_ETIMEDOUT;
    }
    return sync_write_result_;
  }
#endif

  uv_buf_t* raw = buf->out_uv_buf();  // transfers ownership from buf

  uvcpp_write* w = new uvcpp_write();
  w->set_uv_buf(raw, true);
  w->set_self_free(true);  // 释放挪到 callback_write 里，见该类说明

  std::shared_ptr<char> life = alive_token();

  int rc = tcp_->write(
      w, w->get_uv_buf(), 1,
      [this, life](uvcpp_write* wr, int status) {
        (void)wr;
        if (!token_alive(life)) {
          return;  // 对象已析构：请求对象由 trampoline 还回去
        }
        if (status != 0) last_error_code_ = status;
        sync_write_result_ = status;
        sync_write_done_   = true;
      });

  if (rc != 0) {
    last_error_code_ = rc;
    delete w;
    return rc;
  }

  bool completed = wait_for_condition(
      [this]() { return sync_write_done_; }, timeout_ms);

  if (!completed) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  return sync_write_result_;
}

// =========================================================================
// Read
// =========================================================================

int uvcpp_tcp_client::arm_async_read() {
  ensure_read_cache();

  int rc = tcp_->read_start(
      internal_alloc_cb,
      [this](uvcpp_stream* /*s*/, ssize_t nread, const uv_buf_t* buf) {
        if (nread > 0) {
          set_status(TCP_CLIENT_READABLE);

          // --- SOCKS5 协商 ---
          //
          // 排在 TLS 过滤层之前：协商阶段的字节是**明文 SOCKS5 协议**，
          // 而 TLS 只可能出现在协商**之后**（TLS 是打在隧道上的，隧道还没建
          // 起来时 `tls_ssl_` 必然是空的 —— 但顺序写死更不容易被以后改坏）。
          //
          // `socks5_feed` 收尾时可能触发用户的连接回调，而那个回调里最自然的
          // 写法就是把这条客户端删掉，所以调完只能碰 `buf`（局部），不许再碰
          // 任何成员。
          if (socks5_phase_ == socks5_phase::GREETING ||
              socks5_phase_ == socks5_phase::AUTH ||
              socks5_phase_ == socks5_phase::REQUEST) {
            ::std::shared_ptr<char> life5 = alive_token();
            socks5_feed(buf->base, static_cast<size_t>(nread), life5);
            free_read_buf(buf);
            return;
          }

#if UVCPP_OPENSSL_ENABLE
          // --- TLS 过滤层 ---
          //
          // 所有入站字节都从这一个口子进来（原始读路径与框架层读共用它），
          // 所以过滤器插在这里一处就够：socket 上的密文在这里被解成明文，
          // 下面两条交付路径收发的**已经是明文**。
          if (tls_ssl_ != nullptr) {
            const int trc = tls_feed(buf->base, static_cast<size_t>(nread));
            free_read_buf(buf);
            if (trc != 0) {
              // tls_fail 已经通知过读侧和关闭回调了。
              // **此后 *this 可能已经不存在** —— 直接返回，不许再碰成员。
              return;
            }
            tls_flush_out();

            // 先交付数据，再补 connect 回调 —— 反过来的话，使用者在 connect
            // 回调里注册的读回调会错过这段明文（它会先被攒进 tls_plain_，
            // 注册时再补投，所以两种次序都对，但先交付更直接）。
            tls_deliver_plain();
            // **只有握手真的结束才补 connect 回调。** `tls_complete_connect()`
            // 自己只检查 `tls_connect_pending_`，而那个标记在握手开始前就立了；
            // 服务端的第一段数据往往只够走到 ServerHello/Certificate（真实主机
            // 的证书链常分几个 TCP 段到达，`openssl s_server` 则一次到齐），
            // 无条件调用会在握手没完时就把"已连接"回调给出去 —— 调用方紧接着
            // 的 `write()` 拿到 `UV_ENOTCONN`，而握手其实还在推进。本仓用例的
            // 服务端与客户端同源、证书小，一段就够，所以这条一直没被抓到。
            if (tls_handshake_done_) tls_complete_connect();

            // **握手结束通知必须是这个分支的最后一句**：服务端的 on_connection
            // 可能当场把这条连接关掉（鉴权不过要拒绝它），那之后 `*this` 就没了。
            if (tls_handshake_done_ && tls_ready_cb_) tls_notify_ready(0);
            return;
          }
#endif

          // 两条读路径互斥（read_start_events 里挡住混用），所以这里的分支
          // 不会两条都走。
          if (net_read_cb_) {
            // --- 框架层读 ---
            //
            // 直接把 libuv 的缓冲区**借**给回调，回调返回后再释放。
            // 原始路径为了交给用户一个 `uvcpp_buf*` 必须深拷贝一份，而那份
            // 拷贝在回调返回后随即销毁 —— 整包 malloc + memcpy 纯属白做。
            // 这里给的是 (const char*, size_t)，不需要 uvcpp_buf，所以省掉。
            net_read_result r;
            r.event = net_read_event::DATA;
            r.data  = buf->base;
            r.size  = static_cast<size_t>(nread);
            r.error = 0;
            // TCP 上"有数据"与"对端不发了"是两次独立的 read 回调，所以这一块
            // 后面跟着 FIN 这件事由**下一次**回调（`nread == UV_EOF`）报。
            r.fin   = false;

            // base 先存局部：回调里可能把这个 client 关掉/删掉，之后就再也
            // 不能碰成员了，但 libuv 的缓冲区仍然必须释放。
            char* base = buf->base;
            // **回调先拷到栈上再调**：`net_read_cb_` 是成员，它就存在这个对象
            // 里，而回调里最自然的两种写法（`delete` 这个 client、或者就地
            // `read_stop()`）都会**就地销毁它们正在执行的这个闭包**。那之后
            // 回调里再碰任何一个按引用捕获的变量，读的就是已经还给分配器的
            // 存储 —— 实测见 `~uvcpp_tcp_client` 开头那段（PageHeap 下
            // SEGFAULT，裸跑不崩）。执行栈上这一份就与成员死活无关了。
            //
            // 代价：每收到一次数据拷一个 `std::function`（闭包超过 MSVC 的
            // 小对象缓冲时是一次分配）。读事件本身已经有一次系统调用 + 一次
            // 缓冲区往返，这个拷贝相对可以忽略；换掉的是一条静默的 use-after-free。
            uvcpp_net_read_cb cb = net_read_cb_;
            try {
              cb(*this, r);
            } catch (const std::exception& e) {
              // 用户读回调抛异常不能把 libuv 的循环带崩。
              std::fprintf(stderr,
                           "[uvcpp_tcp_client] read callback threw: %s\n",
                           e.what());
            } catch (...) {
              std::fprintf(
                  stderr,
                  "[uvcpp_tcp_client] read callback threw (unknown)\n");
            }
            free_read_buf(buf);
          } else {
            // --- 原始读路径（保持不变）---
            uvcpp_buf tmp_buf;
            tmp_buf.clone_data(buf->base, static_cast<size_t>(nread));
            free_read_buf(buf);

            if (read_fn_) {
              // 同上面框架那一路：`read_arg_` 指向的**就是此刻正在执行的
              // 那个 `std::function`**，回调里析构客户端会把它还掉。先取到
              // 局部，再调 —— 调完之后这个 lambda 一个成员都不碰。
              read_callback_t fn  = read_fn_;
              void*           arg = read_arg_;
              fn(&tmp_buf, arg);
            } else if (sync_read_wanted_) {
              // 这一条回调是 `arm_async_read()` 装的（不是 `on_internal_read`
              // 那条同步专用回调），所以同步读者原先**收不到**这里的数据：
              // 两条分支都不走，字节被静默丢掉。
              //
              // 平时走不到 —— 同步读在 `read_start(nullptr)` 里会自己装
              // `on_internal_read`。但 SOCKS5 协商会在 connect 阶段就把这条
              // 回调装上（握手要靠它推进），于是 `read_started_` 已经是真，
              // 之后的 `read_start(nullptr)` 直接早返回，同步读者的唯一入口
              // 就成了这里。与 `on_internal_read` 保持同一行为。
              ensure_read_cache();
              read_cache_->append_data(tmp_buf.get_const_data(),
                                       tmp_buf.size());
              if (read_cache_->size() > max_read_cache_size_) {
                tcp_->read_stop();
                read_cache_paused_ = true;
              }
            }
          }
        } else {
          if (nread < 0) {
            last_error_code_ = static_cast<int>(nread);
            if (nread != UV_EOF) {
              set_status(TCP_CLIENT_ERROR);
            }

            // SOCKS5 协商期间对端断开：连接回调必须收到这个错误，否则它会
            // 永远不回来 —— 与下面 TLS 那段同一个理由。
            if (socks5_phase_ == socks5_phase::GREETING ||
                socks5_phase_ == socks5_phase::AUTH ||
                socks5_phase_ == socks5_phase::REQUEST) {
              if (nread == UV_EOF) {
                socks5_last_error_ =
                    "proxy closed the connection during negotiation";
              }
              ::std::shared_ptr<char> life5 = alive_token();
              socks5_finish(static_cast<int>(nread), life5);
              if (buf->base != nullptr) free_read_buf(buf);
              return;  // 回调里可能已经删掉本对象
            }

#if UVCPP_OPENSSL_ENABLE
            // TLS 连接上"对端在握手期间就断了"是第一类常见失败（明文客户端
            // 打 TLS 服务端、非 TLS 对端接到 TLS 连接）。不在这里收尾的话，
            // 等握手的 connect 回调会永远不来。
            if (tls_ssl_ != nullptr && tls_connect_pending_) {
              tls_fail(static_cast<int>(nread), nread == UV_EOF);
              if (buf->base != nullptr) free_read_buf(buf);
              return;  // tls_fail 可能已经删掉本对象
            }
#endif

            // **框架层读的通知排在关闭回调之前。**
            //
            // 顺序是有意的：框架的关闭回调会 delete 这个 client，而用户的读
            // 回调通常要读一下状态/对端信息（"是谁断的、什么错"）。先通知
            // 读，客户端还活着；再 fire_close_callbacks，让它按既定顺序收尾。
            if (net_read_cb_) {
              net_read_result r;
              r.event = (nread == UV_EOF) ? net_read_event::PEER_CLOSED
                                          : net_read_event::READ_ERROR;
              r.data  = nullptr;
              r.size  = 0;
              r.error = (nread == UV_EOF) ? 0 : static_cast<int>(nread);
              // TCP 唯一的"干净收尾"就是 UV_EOF（对端发了 FIN）。出错那一路
              // 置 false —— 它讲的是"读不下去了"，不是"对端说完了"。
              r.fin   = (nread == UV_EOF);
              // 与上面那条数据路径同一件事（栈上拷一份再调），只是这里多一层：
              // **下面还要在本对象上跑 `fire_close_callbacks()`**，而"对端断了
              // 就收摊"的写法（回调里 `delete client`）意味着那会儿对象已经没了。
              // 这里拿一份存活令牌（那块 `char` 由栈上这个 `shared_ptr` 保着，
              // 与对象的死活无关），下一句先问它再动成员 —— 成员访问必须先于
              // 任何判断，所以判据只能来自局部。
              ::std::shared_ptr<char> life = alive_token();
              uvcpp_net_read_cb       cb   = net_read_cb_;
              try {
                cb(*this, r);
              } catch (const std::exception& e) {
                std::fprintf(stderr,
                             "[uvcpp_tcp_client] read callback threw: %s\n",
                             e.what());
              } catch (...) {
                std::fprintf(
                    stderr,
                    "[uvcpp_tcp_client] read callback threw (unknown)\n");
              }
              // **不能提前 return**：下面还要把 libuv 的缓冲区还回去。
              // 对象没了就跳过关闭回调，只补那一次释放。
              if (token_alive(life)) fire_close_callbacks();
            } else {
              // Notify close callbacks on connection close or error.
              // 走统一的触发函数：两处必须用完全一样的槽位处理
              // （见 fire_close_callbacks 里的置空说明）。
              fire_close_callbacks();
            }
          }
          // Free the buffer even on error/EOF
          if (buf->base != nullptr) {
            free_read_buf(buf);
          }
        }
      });

  // **`UV_EALREADY` 在这里不是失败，是"已经在读"。**
  //
  // libuv 对已经在读的 stream 再调一次 `uv_read_start` 就返回 `UV_EALREADY`，
  // 而 `uvcpp_stream::read_start` 是**先换回调、再调 libuv** 的
  // （`uvcpp_stream.cpp:28-34`）—— 所以拿到 `UV_EALREADY` 时，本次调用其实
  // **已经生效了**：它只是没有第二次 arm。原样把错误码往上传就是一个**假
  // 失败**，而假失败比真失败更坏：调用方没法把它和真失败区分开，只能选择
  // 忽略返回值 —— 忽略返回值正是这条路上"静默收不到数据"的来源。
  //
  // 这条路真的会走到，而且是最自然的那种用法：TLS 连接的握手阶段必须自己
  // arm 一次读（没人读就等不到 ServerHello，见 connect 完成回调里那句
  // `if (!read_started_) arm_async_read();`），于是使用者在 connect 回调里
  // 注册读回调时，底层已经在读了。仓库自己的 `web_ssl_client_func.cpp` 就是
  // 这么写的 —— 它没有检查返回值，所以一直没被发现。
  if (rc == UV_EALREADY) rc = 0;

  if (rc != 0) {
    last_error_code_ = rc;
    return rc;
  }

  read_started_ = true;
  set_status(TCP_CLIENT_READABLE);
  return 0;
}

int uvcpp_tcp_client::read_start(std::function<void(uvcpp_buf*)> cb) {
  // **使用者的显式读请求永远优先于框架的自动读。**
  //
  // 自动读是在服务端 accept 之后、用户 `on_connection` 回调**之前**装上的，
  // 所以用户在这个回调里照常 `read_start(...)` 就行 —— 这里先把自动读卸掉，
  // 再走正常的注册流程。两步都是在同一个 accept 回调里同步发生的，中间没有
  // 循环迭代，所以不会丢掉任何数据。
  if (auto_read_installed_) release_auto_read();

  if (cb != nullptr) {
    // --- Async mode ---
    if (has_async_read_cb_) {
      return UV_EALREADY;
    }

    has_async_read_cb_ = true;
    sync_read_wanted_  = false;  // 异步消费者优先，明文不该再往读缓存里搬

    read_fn_  = trampoline_read;
    read_arg_ = new std::function<void(uvcpp_buf*)>(cb);

    const int rc = arm_async_read();
#if UVCPP_OPENSSL_ENABLE
    // 握手期间到的明文一直攒在 tls_plain_ 里（那会儿还没有消费者）——
    // 现在有消费者了，补投给它。漏了这一步的表现是"服务端在握完手立刻发的
    // 数据不见了"，而 TLS 1.3 恰恰允许这样发。
    if (rc == 0 && tls_ssl_ != nullptr) tls_deliver_plain();
#endif
    // SOCKS5 协商成功时一起到的应用字节同理（没有代理/没多出来时是空操作）。
    if (rc == 0) socks5_deliver_plain();
    return rc;
  } else {
    // --- Enable sync read cache ---
    if (has_async_read_cb_) {
      throw std::runtime_error(
          "uvcpp_tcp_client::read_start: cannot use sync read after "
          "async read callback was registered");
    }

    // 标记要在那个早返回**之前**置上：TLS 连接上 `read_started_` 早就为真
    // （`enable_tls` arm 的），于是这里会直接返回 0 —— 但"有人要同步读"这件
    // 事必须记下来，否则握手窗口里到的明文不知道该往哪儿放。
    sync_read_wanted_ = true;

    if (read_started_) {
      return 0;  // Already started
    }

    ensure_read_cache();

    int rc = tcp_->read_start(
        internal_alloc_cb,
        [this](uvcpp_stream* s, ssize_t nread, const uv_buf_t* buf) {
          on_internal_read(s, nread, buf);
        });

    if (rc != 0) {
      last_error_code_ = rc;
      return rc;
    }

    read_started_ = true;
    set_status(TCP_CLIENT_READABLE);
#if UVCPP_OPENSSL_ENABLE
    // 同上：把握手期间攒下的明文补进读缓存，否则 read_wait 永远等不到它。
    if (tls_ssl_ != nullptr) tls_deliver_plain();
#endif
    socks5_deliver_plain();  // 同上，明文侧的对应物
    return 0;
  }
}

int uvcpp_tcp_client::read_wait(uvcpp_buf& out_buf, int timeout_ms) {
  if (has_async_read_cb_) {
    throw std::runtime_error(
        "uvcpp_tcp_client::read_wait: cannot use sync read after "
        "async read callback was registered");
  }

  // 有人在同步读 —— 置在 `read_start(nullptr)` **之前**：TLS 连接上
  // `read_started_` 已经为真，下面那句根本不会执行，而这一句才是可靠的标记。
  sync_read_wanted_ = true;

  // Ensure read is started
  if (!read_started_) {
    int rc = read_start(nullptr);
    if (rc != 0) {
      return rc;
    }
  }

  // Resume read if it was paused due to cache full
  if (read_cache_paused_) {
    ensure_read_cache();
    if (read_cache_->size() == 0) {
      int rc = tcp_->read_start(
          internal_alloc_cb,
          [this](uvcpp_stream* s, ssize_t nread, const uv_buf_t* buf) {
            on_internal_read(s, nread, buf);
          });
      if (rc != 0) {
        last_error_code_ = rc;
        return rc;
      }
      read_cache_paused_ = false;
      read_started_ = true;
    } else {
      read_cache_paused_ = false;
    }
  }

  // **窗口里攒下的明文必须在这里补投。**
  //
  // `read_start(nullptr)` 内部会调 `tls_deliver_plain()`，但它在 TLS 连接上
  // **根本不会执行** —— `enable_tls` 早就 arm 过读（没人读就等不到 ServerHello），
  // `read_started_` 为真，于是上面那句 `if (!read_started_)` 直接跳过，
  // `tls_plain_` 里的明文**没有任何人来取**，而本函数等的是 `read_cache_`，
  // 两者就此永远错开。
  //
  // 这个窗口真的会发生，而且是最自然的次序：`write_wait()` 自带泵循环，服务端
  // 的回声完全可能在它返回**之前**到达 —— 那一刻 `sync_read_wanted_` 还是假，
  // 明文按设计留在 `tls_plain_` 里等消费者。消费者（本函数）进来后若不补投，
  // 就只剩"等下一个数据块顺带带出去"这一条路；回声是唯一一块数据时，等到的
  // 就是超时。实测（探针，确定性复现）：
  //
  //     read_started=1 sync_read_wanted=1 tls_plain=11 cache=0
  //     [FAIL] sync_roundtrip: read_wait = -4039     ← UV_ETIMEDOUT
  //
  // 11 就是那声回声的长度 —— 数据一直在，只是没人搬。
  //
  // 放在 `sync_read_wanted_ = true` 之后：`tls_deliver_plain()` 靠这个标记
  // 决定"进读缓存"还是"继续留着等真正的消费者"。
#if UVCPP_OPENSSL_ENABLE
  if (tls_ssl_ != nullptr) tls_deliver_plain();
#endif
  socks5_deliver_plain();  // now sync_read_wanted_ is set, overflow goes to cache

  // Wait for data in the cache
  bool has_data = wait_for_condition(
      [this]() {
        return read_cache_ != nullptr && read_cache_->size() > 0;
      },
      timeout_ms);

  if (!has_data) {
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }

  // Move cached data into out_buf
  out_buf.move_buf(*read_cache_);

  // Check if cache should be paused (will stop on next overflow)
  // But actually we check after appending, so just ensure cache is clean
  if (read_cache_ != nullptr && read_cache_->size() > max_read_cache_size_) {
    tcp_->read_stop();
    read_cache_paused_ = true;
  }

  return 0;
}

int uvcpp_tcp_client::read_start_events(const uvcpp_net_read_cb& cb) {
  if (cb == nullptr) {
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }

  // 同 read_start：使用者的注册顶掉框架的自动读。而且要排在下面那个
  // "已注册"判断之前，否则用户会拿到一个莫名其妙的 UV_EALREADY。
  if (auto_read_installed_) release_auto_read();

  // 和原始读路径互斥。
  //
  // 两条路共用同一个底层 stream，同时注册的话底层 `read_start` 会把先注册的
  // 那个覆盖掉 —— 用户会以为两个回调都在收数据，实际上只有一个。这里挡在
  // 前面，不让那种事静默发生。
  if (read_fn_ != nullptr || has_async_read_cb_) {
    last_error_code_ = UV_EALREADY;
    return UV_EALREADY;
  }

  net_read_cb_ = cb;
  // 置上这个标志有两个作用：一是让同步读（read_start(nullptr)/read_wait）
  // 被挡住，二是让 read_resume() 知道该重新 arm 而不是当作缓存模式返回 0。
  has_async_read_cb_ = true;
  sync_read_wanted_  = false;  // 异步消费者优先，明文不该再往读缓存里搬
  net_read_paused_   = false;

  const int rc = arm_async_read();
#if UVCPP_OPENSSL_ENABLE
  if (rc == 0 && tls_ssl_ != nullptr) tls_deliver_plain();
#endif
  if (rc == 0) socks5_deliver_plain();
  return rc;
}

int uvcpp_tcp_client::read_stop() {
  clear_status(TCP_CLIENT_READABLE);
  read_started_ = false;
  has_async_read_cb_ = false;
  sync_read_wanted_ = false;  // 停读就是两种模式一起停
  net_read_cb_ = nullptr;  // 框架层读的槽也要清，否则下次 arm 会重复投递
  net_read_paused_ = false;
  // 停读就是停读：自动读也一起没了。用户显式停掉之后不该被框架偷偷恢复。
  auto_read_installed_ = false;
  if (read_arg_ != nullptr) {
    delete static_cast<std::function<void(uvcpp_buf*)>*>(read_arg_);
    read_arg_ = nullptr;
  }
  read_fn_ = nullptr;
  return tcp_->read_stop();
}

int uvcpp_tcp_client::read_pause() {
  clear_status(TCP_CLIENT_READABLE);
  read_started_ = false;
  // 注意：**不清回调**。暂停只是暂时不读，恢复时要接着往同一个回调投递。
  net_read_paused_ = true;
  return tcp_->read_stop();
}

int uvcpp_tcp_client::read_resume() {
  if (!has_async_read_cb_) return 0;  // sync/cache mode: nothing to re-arm
  if (read_started_) return 0;        // already reading
  net_read_paused_ = false;
  const int rc = arm_async_read();
#if UVCPP_OPENSSL_ENABLE
  // 暂停期间也可能有明文被解出来攒着（暂停只挡住 socket 读，挡不住已经
  // 在 rbio 里的字节），恢复时一并补投。
  if (rc == 0 && tls_ssl_ != nullptr) tls_deliver_plain();
#endif
  if (rc == 0) socks5_deliver_plain();
  return rc;
}

void uvcpp_tcp_client::install_auto_read() {
  // 已经有读回调了就不动 —— 自动读只是兜底，不该顶掉任何真实消费者。
  if (read_fn_ != nullptr || has_async_read_cb_) return;

  auto_read_installed_ = true;
  auto_read_warned_    = false;

  // 捕获 this 是安全的：这个 std::function 是成员，生命周期跟着客户端走。
  net_read_cb_ = [this](uvcpp_tcp_client& c, const net_read_result& r) {
    if (!r.is_data()) {
      // PEER_CLOSED / READ_ERROR：不用在这里做什么，框架的关闭回调会收尾。
      return;
    }
    if (auto_read_warned_) return;
    auto_read_warned_ = true;

    // 只在**真的丢了数据**的时候才警告，而且每个连接只警告一次。
    //
    // 这条警告是有用的："对端明明发了数据我这边没反应"以前是无声的 ——
    // 现在至少有一行输出指向那个连接。同时它也不会像"accept 就警告"那样
    // 在每一连接上刷屏（自动读是默认行为，那会是噪音）。
    std::fprintf(stderr,
                 "[uvcpp_tcp_client] Warning: client %p received %lu byte(s) "
                 "with no read callback registered — data is being DISCARDED. "
                 "Register one with read_start()/read_start_events() in the "
                 "server's on_connection callback.\n",
                 static_cast<void*>(&c), static_cast<unsigned long>(r.size));
  };

  has_async_read_cb_ = true;
  net_read_paused_   = false;
  const int rc = arm_async_read();
  if (rc != 0) {
    // 装不上就退回去，别留一个"看起来在自动读、其实没有"的状态。
    net_read_cb_ = nullptr;
    has_async_read_cb_ = false;
    auto_read_installed_ = false;
  }
}

void uvcpp_tcp_client::release_auto_read() {
  if (!auto_read_installed_) return;

  auto_read_installed_ = false;
  auto_read_warned_    = false;
  net_read_cb_         = nullptr;
  net_read_paused_     = false;
  has_async_read_cb_   = false;
  read_started_        = false;

  if (tcp_ != nullptr) {
    tcp_->read_stop();
  }
  clear_status(TCP_CLIENT_READABLE);
}

void uvcpp_tcp_client::set_on_close(std::function<void()> cb) {
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  close_fn_  = trampoline_close;
  close_arg_ = new std::function<void()>(cb);
}

void uvcpp_tcp_client::set_close_manager(std::function<void()> cb) {
  if (close_mgr_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_mgr_arg_);
    close_mgr_arg_ = nullptr;
  }
  close_mgr_fn_  = trampoline_close;
  close_mgr_arg_ = new std::function<void()>(cb);
}

void uvcpp_tcp_client::clear_close_manager() {
  if (close_mgr_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_mgr_arg_);
    close_mgr_arg_ = nullptr;
  }
  close_mgr_fn_ = nullptr;
}

bool uvcpp_tcp_client::has_close_manager() const {
  return close_mgr_fn_ != nullptr;
}

// =========================================================================
// 关闭观察者（加法式，见头文件）
// =========================================================================

int uvcpp_tcp_client::add_close_observer(std::function<void()> cb) {
  if (!cb) return 0;
  const int id = next_close_observer_id_++;
  close_observers_.push_back(std::make_pair(id, std::move(cb)));
  return id;
}

void uvcpp_tcp_client::remove_close_observer(int id) {
  if (id == 0) return;
  for (size_t i = 0; i < close_observers_.size(); ++i) {
    if (close_observers_[i].first == id) {
      close_observers_.erase(close_observers_.begin() + static_cast<long>(i));
      return;
    }
  }
}

size_t uvcpp_tcp_client::close_observer_count() const {
  return close_observers_.size();
}

// =========================================================================
// Close
// =========================================================================

int uvcpp_tcp_client::close(const std::function<void()>& after_close) {
  // **先看 get_handle() 再看 is_closing()，顺序不能反。**
  //
  // 句柄关完之后（`uvcpp_handle::callback_close` 里）包装对象的 `_handle`
  // 被置成 nullptr，而 `uvcpp_handle::is_closing()` 是把这个指针直接交给
  // libuv 的 `uv_is_closing()` —— 它解引用。所以对一个已经关掉的连接调
  // `close()`，先问 `is_closing()` 就是空指针解引用（段错误）。
  if (tcp_ == nullptr || tcp_->get_handle() == nullptr) {
    // 句柄都没了，不可能再有关闭事件。调用方的收尾照跑一次 —— 不然它会
    // 以为收尾还欠着（比如 http 层的连接上下文永远不摘）。
    if (after_close) after_close();
    return UV_EINVAL;
  }

  if (tcp_->is_closing()) {
    // 已经有人在关了。此刻**绝不能** fire_close_callbacks()：
    //   1. 我们这次的完成回调不会来（uv_close 只认第一次调用）；
    //   2. 关闭事件还飞在队列里，而 fire 里的 close manager 会 delete 本
    //      对象，那个完成回调随后就会踩到已经释放的句柄包装对象。
    // 只把调用方自己的收尾跑掉（幂等），关闭的收尾交给先发起的那一次。
    if (after_close) after_close();
    return 0;
  }

  set_status(TCP_CLIENT_CLOSING);

  // 顺序是有意的：
  //   1. after_close —— 调用方的收尾，此时客户端还活着（能读状态/对端信息）；
  //   2. fire_close_callbacks() —— 用户回调 + 框架的 close manager，后者可能
  //      `delete this`，所以它必须是最后一句，之后不能再碰任何成员。
  tcp_->close([this, after_close](uvcpp_handle*) {
    set_status(TCP_CLIENT_CLOSED);

    if (after_close) {
      try {
        after_close();
      } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "[uvcpp_tcp_client] close completion threw: %s\n",
                     e.what());
      } catch (...) {
        std::fprintf(stderr,
                     "[uvcpp_tcp_client] close completion threw (unknown)\n");
      }
    }

    fire_close_callbacks();  // 此后 *this 可能已经不存在了
  });

  return 0;
}

void uvcpp_tcp_client::fire_close_callbacks() {
  // **必须在调用前把两个槽都置空。**
  //
  // 框架的管理回调会 delete 本客户端，而 ~uvcpp_tcp_client 会释放
  // close_arg_ 指向的那个 std::function；trampoline_close 返回之后还要再
  // delete 它一次 —— 原来的代码就是这样每次连接关闭都 double free 一次。
  // 置空之后析构函数不再碰它，拥有权临时转移到 trampoline 手里，
  // trampoline 那一次 delete 就成了唯一的一次。
  close_callback_t  ufn  = close_fn_;
  void*             uarg = close_arg_;
  close_fn_  = nullptr;
  close_arg_ = nullptr;

  close_callback_t  mfn  = close_mgr_fn_;
  void*             marg = close_mgr_arg_;
  close_mgr_fn_  = nullptr;
  close_mgr_arg_ = nullptr;

  // 观察者同样**先整体取出来**再跑：某个观察者可能又装/摘观察者，边遍历边改
  // 会出事。取出来之后新装的落在（已空的）close_observers_ 里，不会被本次跑到
  // —— 它是在"关闭"发生之后才挂上去的，本来也不该收到这一次。
  std::vector<std::pair<int, std::function<void()> > > obs;
  obs.swap(close_observers_);

  // 顺序是有意的：用户回调先跑，此时客户端还活着（能读状态、对端信息）；
  // 观察者次之（同样还活着，但连接已经关了，不要写）；框架回调最后跑，
  // 它可能会把这个对象删掉。
  if (ufn != nullptr) {
    ufn(uarg);
  }
  for (size_t i = 0; i < obs.size(); ++i) {
    if (obs[i].second) obs[i].second();
  }
  if (mfn != nullptr) {
    mfn(marg);  // 此后 *this 可能已经不存在了 —— 不能再碰任何成员
  }
}

void uvcpp_tcp_client::clear_on_close() {
  if (close_arg_ != nullptr) {
    delete static_cast<std::function<void()>*>(close_arg_);
    close_arg_ = nullptr;
  }
  close_fn_ = nullptr;
}

// =========================================================================
// TLS 过滤层
// =========================================================================
//
// 全部形状由一条约束决定：**socket 归 libuv，SSL 只跟内存打交道**。
// 于是本层是纯转换器 —— 出方向「明文 → SSL_write → wbio → 抽出来 → uv_write」，
// 入方向「uv_read → 喂 rbio → SSL_read → 明文 → 交给上层」。没有任何一处
// 需要等 socket、转圈、或者 uv_poll。
//
// 用户回调（读回调 / connect 回调 / close 回调）随时可能 `delete` 本对象 ——
// 这是本文件里反复出现的约束。所以每个会进用户代码的函数都在进去之前把要用的
// 东西取成局部量，进去之后用存活令牌判一次，确认对象还在才继续碰成员。

#if UVCPP_OPENSSL_ENABLE

int uvcpp_tcp_client::enable_tls(uvcpp_ssl_context* ctx) {
  if (tls_ssl_ != nullptr) return UV_EALREADY;
  if (ctx == nullptr || !ctx->is_ready()) return UV_EINVAL;
  tls_ctx_ = ctx;

  // fd=0：**不装 socket BIO**。装了就回到 SSL_set_fd 那条死路上了。
  tls_ssl_ = new uvcpp_ssl(ctx, 0);
  if (tls_ssl_->raw_ssl() == nullptr) {
    delete tls_ssl_;
    tls_ssl_ = nullptr;
    return UV_ENOMEM;
  }
  if (!tls_ssl_->use_memory_bio()) {
    delete tls_ssl_;
    tls_ssl_ = nullptr;
    return UV_ENOMEM;
  }

  tls_handshake_done_  = false;
  tls_ssl_error_       = 0;
  tls_connect_pending_ = false;
  tls_alpn_.clear();

  // 在 enable_tls 之前设过的宣告名单，此刻才装上。
  if (!tls_alpn_want_.empty()) tls_ssl_->set_alpn_protos(tls_alpn_want_);

  // 同理：`connect()` 在 `enable_tls()` **之前**调过的话，主机名那时没处可钉，
  // 记在了 `tls_host_want_` 里 —— 现在补上。
  apply_tls_hostname();

  // 连接已经建立（服务端 accept 之后调用的情形）：握手现在就开始。
  // 客户端此时还没连上，起手是空的 —— 等 connect 完成回调进来再发 ClientHello。
  if (has_status(TCP_CLIENT_CONNECTED)) {
    if (!read_started_) {
      arm_async_read();  // 握手要靠读事件推进，没人读就永远握不上
    }
    tls_drive_handshake();
    tls_flush_out();
    if (tls_handshake_done_) tls_deliver_plain();
    // 服务端在 accept 路径上同步就把握手走完（理论上的可能）时也要通知，
    // 但那一步必须留在最后：通知的回调可能把这条连接释放掉。
    if (tls_handshake_done_) {
      tls_notify_ready(0);
      return 0;
    }
  }
  return 0;
}

void uvcpp_tcp_client::set_tls_ready_callback(
    std::function<void(uvcpp_tcp_client*, int)> cb) {
  tls_ready_cb_ = cb;
}

int uvcpp_tcp_client::tls_last_ssl_error() const { return tls_ssl_error_; }

void uvcpp_tcp_client::set_tls_handshake_timeout_ms(int ms) {
  tls_hs_timeout_ms_ = ms > 0 ? ms : 0;
}

void uvcpp_tcp_client::tls_arm_handshake_timer() {
  // 只有"握手已经开始、且还没结束"才需要计时。tls_drive_handshake() 每次读
  // 事件都会调进来，所以这里必须廉价且幂等。
  if (tls_hs_timeout_ms_ <= 0) return;
  if (tls_handshake_done_ || tls_ssl_ == nullptr) return;
  if (tls_hs_timer_ != nullptr) return;   // 已经在计了，不重起
  if (loop_ == nullptr) return;

  tls_hs_timer_ = new uvcpp_timer(loop_);
  tls_hs_timer_->start(
      [this](uvcpp_timer*) {
        // 记下"此刻正跑在超时回调里"。析构要用它判断能不能安全释放这个句柄：
        // 本回调下面的 tls_fail() 会把整条连接拆掉、把本对象释放掉，而那一切
        // 都发生在**这个回调的栈帧还在**的时候 —— 那时析构它就是在踩正在执行
        // 的闭包。
        tls_hs_in_cb_ = true;

        // **回调里只 stop，不 delete**，理由同上。
        if (tls_hs_timer_ != nullptr) tls_hs_timer_->stop();
        if (tls_handshake_done_) return;

        std::fprintf(stderr,
                     "[uvcpp_tcp_client] TLS handshake timed out after %d ms, "
                     "closing connection\n",
                     tls_hs_timeout_ms_);
        // 走与"握手失败"完全相同的那条路：通知上层（绝不把连接交付出去）
        // → 读侧收尾 → 关闭回调 → 服务端摘表并释放本对象。
        //
        // **必须是本回调的最后一句**：tls_fail 会一路走到释放本对象，
        // 之后再碰任何成员都是 use-after-free。
        tls_fail(UV_ETIMEDOUT, false);
      },
      static_cast<uint64_t>(tls_hs_timeout_ms_), 0);   // repeat=0：一次性
}

void uvcpp_tcp_client::tls_disarm_handshake_timer() {
  if (tls_hs_timer_ == nullptr) return;
  tls_hs_timer_->stop();
  delete tls_hs_timer_;
  tls_hs_timer_ = nullptr;
}

bool uvcpp_tcp_client::set_tls_alpn_protos(const std::vector<std::string>& protos) {
  if (tls_handshake_done_) return false;  // 协商已经过去了，改不了
  tls_alpn_want_ = protos;
  if (tls_ssl_ != nullptr) return tls_ssl_->set_alpn_protos(tls_alpn_want_);
  return true;  // SSL 对象还没建，enable_tls 时会装上
}

void uvcpp_tcp_client::apply_tls_hostname() {
  if (tls_ssl_ == nullptr || tls_host_want_.empty()) return;

  // **只有 `PEER_STRICT` 才钉。** 设了主机名之后，无论 `SSL_CTX` 上的 verify 位是
  // 什么，OpenSSL 都会在握手末尾做这次检查；而 `SSL_VERIFY_NONE` 下**检查失败也
  // 不中止握手**。于是对 `NONE` / `PEER` 也钉上去的话：`PEER` 会凭空多出一层
  // 从没承诺过的校验（名字对不上的对端从此连不上），`NONE` 则什么都不会发生 ——
  // 两种都是"悄悄改掉既有行为"。本档位的定义就是"校验对端 **+ 主机名**"，所以
  // 钉与不钉**只**看这一个档位。
  // SNI is not optional: every modern virtual host needs it, and without it the
  // server either aborts the handshake or serves an unrelated default certificate.
  //
  // RFC 6066 §3 禁止在 SNI 里放 IP 字面量：老代价是若干服务端直接拒握手。
  // 判据与 `connect_transport()` 的数字地址快路径同源（`uv_ip4_addr` /
  // `uv_ip6_addr`），名字不是 IP 才发。
  struct sockaddr_in _sni4;
  struct sockaddr_in6 _sni6;
  if (uv_ip4_addr(tls_host_want_.c_str(), 0, &_sni4) != 0 &&
      uv_ip6_addr(tls_host_want_.c_str(), 0, &_sni6) != 0) {
    tls_ssl_->set_server_name(tls_host_want_.c_str(), tls_host_want_.size());
  }

  uvcpp_ssl_context* ctx = tls_ssl_->context();
  if (ctx == nullptr || ctx->verify_mode() != tls_verify_mode::PEER_STRICT) return;

  tls_ssl_->set_verify_hostname(tls_host_want_.c_str(), tls_host_want_.size());
}

int uvcpp_tcp_client::tls_drive_handshake() {
  if (tls_ssl_ == nullptr || tls_handshake_done_) return 0;

  // 握手从这一刻起算超时（幂等）。放在这里而不是 enable_tls() 里，是因为
  // 服务端与客户端两条路都汇聚到本函数推进握手 —— 一处起算，两处生效。
  tls_arm_handshake_timer();

  const int rc = tls_ssl_->handshake();
  if (rc < 0) {
    // 真出错（不是 WANT_READ/WANT_WRITE —— 那两个在本层被归一化成 0）。
    // 证书校验失败、协议版本不匹配、对端根本不是 TLS 服务端都在这里。
    tls_ssl_error_ = tls_ssl_->last_ssl_error();
    tls_fail(UV_EPROTO, false);
    return -1;
  }
  if (rc == 1) {
    tls_handshake_done_ = true;
    // 握手结束了，这条连接不再需要那道闸门。正常路径上定时器只活了几十微秒。
    tls_disarm_handshake_timer();
    tls_ssl_error_      = tls_ssl_->last_ssl_error();
    // 在**这里**取 ALPN 结果，而不是等上层来问：ready 回调的契约是"回调里
    // 对象可能已被析构"，从回调里反查 SSL* 不安全。这里是唯一置真点、被五个
    // 调用点汇聚，且必然早于 tls_notify_ready。
    tls_alpn_ = tls_ssl_->alpn_selected();
  }
  return 0;
}

void uvcpp_tcp_client::tls_flush_out() {
  if (tls_ssl_ == nullptr) return;

  // wbio 里新产生的密文全部抽干。memory BIO 无界，所以这一步不会失败。
  //
  // ★ 先 `reserve` 一次到位。上一批在下面被 `swap` 走了，所以 `tls_out_` 每批
  //   都从 0 开始涨，而 `append` 的增长是**翻倍**的 —— 一批 250 KB 要额外付
  //   ~2×250 KB 的搬运。`pending_ciphertext()` 恰好给出这批有多少字节，于是
  //   那次翻倍搬运可以整段省掉，只剩"BIO → 串"这一遍（这一遍省不掉：
  //   `take_ciphertext` 就是 `BIO_read`）。
  //
  //   用 `reserve` 而不是 `resize`：后者把新露出的那段**清零**，等于每批白清
  //   250 KB 的 memset，比它要省的那次搬运还贵。
  const size_t pend = tls_ssl_->pending_ciphertext();
  if (pend > 0) tls_out_.reserve(tls_out_.size() + pend);
  char buf[16384];
  size_t n;
  while ((n = tls_ssl_->take_ciphertext(buf, sizeof(buf))) > 0) {
    tls_out_.append(buf, n);
  }

  if (tls_out_.empty() || tls_out_busy_) return;
  if (tcp_ == nullptr || !has_status(TCP_CLIENT_CONNECTED)) return;

  // 同一时刻只投一个密文写。libuv 本身允许多个 uv_write 排队，但"一次一个"
  // 让完成回调的语义变成确定的：回调到了 = 这一批密文已经出网。
  //
  // 这一批**不再拷第二遍**：`swap` 出来的那份 `std::string` 按**共享视图**交给
  // 写层，它的存活由请求对象持有的那份引用计数履行 —— libuv 那句"缓冲要活到
  // 完成回调"因此成立，本函数返回、局部变量析构都不影响那批字节。
  //
  // 改前是 `uvcpp_buf bufcpp(chunk.data(), chunk.size())` + `out_uv_buf()`：把
  // 整批密文**再拷一份**自有块交出去，而 `chunk` 出本函数就析构 —— 那份拷贝
  // 纯属白付。
  std::shared_ptr<std::string> chunk = std::make_shared<std::string>();
  chunk->swap(tls_out_);

  uvcpp_write* w = new uvcpp_write();
  // 只有一块 —— 这批密文本来就是连续的，没有头/体之分，于是第 1 块留空、视图
  // 走第 2 块那个槽位。第 1 块那个 `{nullptr, 0}` 项不贡献任何字节（长度为 0
  // 的 iovec 合法）。
  //
  // `const_cast` 不是多余的：本仓是 `CMAKE_CXX_STANDARD 11`，非 const 的
  // `std::string::data()` 在这个标准下仍返回 `const char*`（返回 `char*` 是 C++17
  // 才有的），而 `uv_buf_init` 收的是 `char*`。视图层只读这块内存，所以这个
  // 转换在语义上也是安全的。上面 `tls_out_.empty()` 已经挡掉了空串。
  w->append_uv_buf_view(
      uv_buf_init(const_cast<char*>(chunk->data()),
                  static_cast<unsigned int>(chunk->size())),
      chunk);
  w->set_self_free(true);  // 释放挪到 callback_write 里，见该类说明

  tls_out_busy_ = true;

  std::shared_ptr<char> life = alive_token();
  int rc = tcp_->write(
      w, w->get_uv_bufs(), static_cast<unsigned int>(w->get_uv_nbufs()),
      [this, life](uvcpp_write* wr, int status) {
        (void)wr;
        if (!token_alive(life)) {
          return;  // 对象没了：请求对象由 trampoline 还回去
        }
        tls_out_busy_ = false;
        if (status != 0) {
          last_error_code_ = status;
          tls_fail(status, false);
          return;
        }
        // wbio 里可能还有（比如握手过程中又产生了 Finished）。
        tls_flush_out();
        if (!token_alive(life)) return;
        // 只有在**密文确实全部出网**时才会真正收尾（见 tls_finish_write）。
        tls_finish_write(0);
      });

  if (rc != 0) {
    tls_out_busy_ = false;
    delete w;
    last_error_code_ = rc;
    tls_fail(rc, false);
  }
}

int uvcpp_tcp_client::tls_write_plain(const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int rc = tls_ssl_->write(data + sent, len - sent);
    if (rc > 0) {
      sent += static_cast<size_t>(rc);
      continue;
    }
    tls_ssl_error_ = tls_ssl_->last_ssl_error();
    if (rc == 0) {
      // WANT_READ / WANT_WRITE。memory BIO 下 wbio 无界，所以「因为发不出去
      // 而写不动」不可能发生；剩下的只是对端要做密钥更新。那需要等更多入站
      // 数据，而这里的调用栈是应用层发起的，等不了。
      //
      // TLS 1.3 没有重协商，正常会话走不到这里。真的走到就如实报错 ——
      // 报错好过"以为发出去了其实没有"。
      return UV_EIO;
    }
    return UV_EPROTO;
  }
  return 0;
}

int uvcpp_tcp_client::tls_feed(const char* data, size_t len) {
  if (tls_ssl_ == nullptr || data == nullptr || len == 0) return 0;

  size_t off = 0;
  while (off < len) {
    const size_t fed = tls_ssl_->feed_ciphertext(data + off, len - off);
    if (fed == 0) break;
    off += fed;

    if (!tls_handshake_done_) {
      if (tls_drive_handshake() < 0) return -1;
    }
    if (!tls_handshake_done_) continue;

    // 握手完了就尽量把明文解出来。SSL_read 一次不一定给全，读到 0 为止。
    char buf[16384];
    for (;;) {
      const int rn = tls_ssl_->read(buf, sizeof(buf));
      if (rn > 0) {
        tls_plain_.append(buf, static_cast<size_t>(rn));
        continue;
      }
      tls_ssl_error_ = tls_ssl_->last_ssl_error();
      if (rn == 0 && tls_ssl_error_ == SSL_ERROR_ZERO_RETURN) {
        // 对端发了 close_notify —— 干净的 TLS 关闭。
        //
        // **先把同一段里已经解出来的明文交付出去，再收尾。** 很多服务端
        // （HTTP/1.0、`Connection: close`、`openssl s_server`）把整条响应与
        // close_notify 放在同一段 TCP 数据里发；先前这里直接 `tls_fail`，
        // `tls_plain_` 里刚解出来的响应就被丢掉，调用方只看到 EOF，而
        // `uvcpp_http_client` 那条路会把它交付成 ECONNRESET（uvp#274）。
        //
        // 消费者可能在 `tls_deliver_plain()` 里把本对象关掉/删掉，之后只能
        // 靠存活令牌判断，绝不能直接碰成员。
        std::shared_ptr<char> life = alive_token();
        tls_deliver_plain();
        if (!token_alive(life)) return -1;
        tls_fail(0, true);
        return -1;
      }
      if (rn < 0) {
        tls_fail(UV_EPROTO, false);
        return -1;
      }
      break;  // WANT_READ：等下一次可读
    }
  }
  return 0;
}

void uvcpp_tcp_client::tls_deliver_plain() {
  if (tls_plain_.empty()) return;

  // **自动读不是消费者。**
  //
  // 它是传输层的兜底，唯一职责是"发现对端断开"（见 install_auto_read 的注释），
  // 收到数据只会打一行 DISCARDED 警告然后扔掉。但在**服务端**它恰好是握手期间
  // 唯一装在 `net_read_cb_` 上的东西（`setup_client_callbacks` 在 accept 时就
  // 装了），而那时连接还没交给上层 —— 于是与 Finished 同一段到达的应用数据被
  // 投给兜底、被丢掉，上层随后注册的读回调再也等不到它（`tls_plain_` 已经空了）。
  //
  // 留在 `tls_plain_` 里，等真正的消费者注册时补投 —— 每个注册路径都会补
  // （`read_start(cb)`、`read_start(nullptr)`、`read_start_events`、
  // `read_resume`），所以留着不会丢。
  if (auto_read_installed_) return;

  // 还没有消费者：**先留着，不能丢**。
  // TLS 1.3 的服务端可以在握完手立刻发应用数据，而客户端的使用者要等
  // connect 回调才有机会注册读回调 —— 中间这段窗口里到的明文必须攒着。
  if (!net_read_cb_ && read_fn_ == nullptr) {
    // 真的有人在同步读时才进读缓存（`read_wait` 就盯着 `read_cache_`）。
    //
    // 这里的判据**不能**是「`read_started_ && !has_async_read_cb_`」，那看着像
    // 同步模式，实际在 TLS 握手窗口里恰好为真（`enable_tls` 自己 arm 了读、
    // 而消费者还没注册）—— 明文会被搬进读缓存，随后注册的异步消费者只认
    // `tls_plain_`，**永远读不到它**。
    if (sync_read_wanted_ && !has_async_read_cb_) {
      ensure_read_cache();
      read_cache_->append_data(tls_plain_.data(), tls_plain_.size());
      tls_plain_.clear();
    }
    return;
  }

  std::string out;
  out.swap(tls_plain_);

  if (net_read_cb_) {
    net_read_result r;
    r.event = net_read_event::DATA;
    r.data  = out.data();
    r.size  = out.size();
    r.error = 0;
    // 明密文分流之后的那条数据路，理由与上面那条一致（TCP 恒为 false）。
    r.fin   = false;
    // 把 std::function 拷到局部再调：回调里可能把这个客户端关掉/删掉，
    // 那样成员 `net_read_cb_` 的存储会在 operator() 执行到一半时消失。
    uvcpp_net_read_cb cb = net_read_cb_;
    try {
      cb(*this, r);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw (unknown)\n");
    }
    return;
  }

  // 原始读路径：fn/arg 是 C 风格函数指针 + void*，取到局部即可。
  read_callback_t fn  = read_fn_;
  void*           arg = read_arg_;
  if (fn == nullptr) return;

  uvcpp_buf tmp;
  tmp.clone_data(out.data(), out.size());
  fn(&tmp, arg);
}

void uvcpp_tcp_client::tls_complete_connect() {
  if (!tls_connect_pending_) return;
  tls_connect_pending_ = false;

  connect_callback_t fn  = connect_fn_;
  void*              arg = connect_arg_;
  connect_fn_  = nullptr;
  connect_arg_ = nullptr;
  has_async_connect_cb_ = false;
  if (fn != nullptr) fn(0, arg);
}

std::vector<std::vector<unsigned char>>
uvcpp_tcp_client::peer_cert_chain_der() const {
  if (tls_ssl_ == nullptr) return std::vector<std::vector<unsigned char>>();
  return tls_ssl_->peer_cert_chain_der();
}

long uvcpp_tcp_client::peer_verify_result() const {
  if (tls_ssl_ == nullptr) return -1;
  return tls_ssl_->peer_verify_result();
}

std::string uvcpp_tcp_client::peer_verify_error() const {
  if (tls_ssl_ == nullptr) return "no TLS session";
  return tls_ssl_->peer_verify_error();
}

void uvcpp_tcp_client::tls_notify_ready(int status) {
  if (!tls_ready_cb_) return;

  // 先拷到局部、再清成员 —— 与写/连接完成回调同一约定：回调里最自然的事
  // 就是发起下一次操作，成员还立着会被误读成"还有一次待通知"。
  std::function<void(uvcpp_tcp_client*, int)> cb = tls_ready_cb_;
  tls_ready_cb_ = nullptr;

  // **这是函数里唯一一处触碰 `*this` 之外的东西，之后一律不许再碰成员** ——
  // 回调可能把这条连接关掉并释放掉（服务端拒绝一条握手完成的连接就是常规
  // 用法）。调用点也必须保证这一句是所在回调的最后一句。
  cb(this, status);
}

void uvcpp_tcp_client::tls_finish_write(int status) {
  if (!tls_write_pending_) return;
  // 密文还在路上就先不收尾 —— 否则调用方以为发出去了，实际还在 wbio 里。
  if (status == 0 && (tls_out_busy_ || !tls_out_.empty())) return;

  tls_write_pending_ = false;

  if (tls_write_sync_) {
    tls_write_sync_    = false;
    sync_write_result_ = status;
    sync_write_done_   = true;
    return;
  }

  // 异步：先清成员**再**回调，次序与明文路径完全一致（回调里最自然的事就是
  // 接着发起下一次写，标志还立着会让那次写拿到 UV_EALREADY）。
  if (write_fn_ != nullptr) {
    write_callback_t fn  = write_fn_;
    void*            arg = write_arg_;
    write_fn_  = nullptr;
    write_arg_ = nullptr;
    has_async_write_cb_ = false;
    fn(status, arg);
  } else {
    has_async_write_cb_ = false;
  }
}

void uvcpp_tcp_client::tls_fail(int err, bool peer_closed) {
  // 任何一条失败路径上都不再需要握手超时。**放在最前面**：下面每一条分支都
  // 可能把本对象释放掉，之后再碰成员就是 use-after-free。
  //
  // 这里**只 stop、不 delete**：本函数可能是从定时器回调里被调用的（超时那
  // 一条路），而在回调压栈期间析构这个句柄就是踩正在执行的闭包 —— 与析构里
  // 那段"三层悬垂"记的是同一件事。释放留给析构（正常路径）或"析构时发现
  // 自己在回调里"那一支（只停不放，见 ~uvcpp_tcp_client）。
  if (tls_hs_timer_ != nullptr) tls_hs_timer_->stop();

  const int code = (err != 0) ? err : (peer_closed ? UV_EOF : UV_EPROTO);
  if (err != 0) {
    last_error_code_ = err;
    set_status(TCP_CLIENT_ERROR);
  }

  // 1. 在等握手的 connect 回调 —— 使用者拿到的是"连接失败"，不是成功。
  if (tls_connect_pending_) {
    tls_connect_pending_ = false;
    if (tls_write_pending_) { tls_write_pending_ = false; tls_write_sync_ = false; }
    connect_callback_t fn  = connect_fn_;
    void*              arg = connect_arg_;
    connect_fn_  = nullptr;
    connect_arg_ = nullptr;
    has_async_connect_cb_ = false;
    if (fn != nullptr) fn(code, arg);
    return;
  }

  // 2. 服务器侧：握手失败必须"通知一次，且绝不把连接交给上层"。
  //
  // 这一步只做通知，**不在这里关连接** —— 上面 connect 分支之外的收尾在下面
  // 第 3/4/5 步里走完（读侧通知 → fire_close_callbacks → 服务端的 close
  // manager 摘除并释放），与明文路径遇到读错误时完全同一条路。服务端的
  // ready 回调拿到非 0 状态时只需记账，不需要自己动手收尾。
  if (tls_ready_cb_) tls_notify_ready(code);

  // 3. 在等的写：让它以错误收尾，而不是永远等下去。
  if (tls_write_pending_) tls_finish_write(code);

  // 4. 通知读侧。框架层读排在关闭回调之前（与明文路径同一约定）：
  //    关闭回调可能 delete 本对象，而用户读回调通常要读一下状态。
  if (net_read_cb_) {
    net_read_result r;
    r.event = peer_closed ? net_read_event::PEER_CLOSED
                          : net_read_event::READ_ERROR;
    r.data  = nullptr;
    r.size  = 0;
    r.error = peer_closed ? 0 : code;
    // TCP 的 PEER_CLOSED 本来就只可能来自对端 FIN（见本函数上面 `peer_closed`
    // 怎么算出来的），所以这里恒等于 `peer_closed`。
    r.fin   = peer_closed;
    uvcpp_net_read_cb cb = net_read_cb_;
    try {
      cb(*this, r);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr, "[uvcpp_tcp_client] read callback threw (unknown)\n");
    }
  }

  // 5. 此后 *this 可能已经不存在 —— 不能再碰任何成员。
  fire_close_callbacks();
}

#endif  // UVCPP_OPENSSL_ENABLE

// =========================================================================
// Loop control
// =========================================================================

int uvcpp_tcp_client::run(uv_run_mode md) {
  return loop_->run(md);
}

void uvcpp_tcp_client::stop() {
  loop_->stop();
}

// =========================================================================
// Internal helpers
// =========================================================================

void uvcpp_tcp_client::set_status(int flags) {
  status_ |= flags;
}

void uvcpp_tcp_client::clear_status(int flags) {
  status_ &= ~flags;
}

void uvcpp_tcp_client::reset_status() {
  status_ = TCP_CLIENT_NONE;
}

bool uvcpp_tcp_client::wait_for_condition(
    std::function<bool()> condition, int timeout_ms) {
  auto start = std::chrono::steady_clock::now();

  while (true) {
    if (condition()) {
      return true;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    if (elapsed >= timeout_ms) {
      return false;
    }

    // Pump the event loop to process pending callbacks
    // UV_RUN_NOWAIT returns immediately if nothing is ready
    if (loop_ != nullptr) {
      loop_->run(UV_RUN_NOWAIT);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void uvcpp_tcp_client::internal_alloc_cb(uvcpp_handle* /*h*/, size_t sz,
                                          uv_buf_t* buf) {
  alloc_read_buf(buf, sz > 0 ? sz : 4096);
}

void uvcpp_tcp_client::on_internal_read(uvcpp_stream* /*s*/, ssize_t nread,
                                         const uv_buf_t* buf) {
  if (nread > 0) {
    set_status(TCP_CLIENT_READABLE);

    // SOCKS5 协商：这条回调正常不会在协商期间装上（协商自己 arm 的是
    // `arm_async_read`），但万一装上，字节也必须进状态机而不是读缓存。
    if (socks5_phase_ == socks5_phase::GREETING ||
        socks5_phase_ == socks5_phase::AUTH ||
        socks5_phase_ == socks5_phase::REQUEST) {
      ::std::shared_ptr<char> life5 = alive_token();
      socks5_feed(buf->base, static_cast<size_t>(nread), life5);
      free_read_buf(buf);
      return;
    }

#if UVCPP_OPENSSL_ENABLE
    // 同步读路径（read_start(nullptr) / read_wait）也必须过 TLS 过滤器。
    // 漏掉这里的后果不是"少个功能"，而是**把密文当明文交给使用者** ——
    // 一个静默的错答案，比报错难查得多。
    if (tls_ssl_ != nullptr) {
      const int trc = tls_feed(buf->base, static_cast<size_t>(nread));
      free_read_buf(buf);
      if (trc != 0) return;  // tls_fail 已经通知过；此后不许再碰成员
      tls_flush_out();
      tls_deliver_plain();  // 同步模式下会攒进 read_cache_
      // 与框架层读路径同一约定：通知留在最后，回调可能释放本对象。
      if (tls_handshake_done_ && tls_ready_cb_) tls_notify_ready(0);
      return;
    }
#endif

    ensure_read_cache();

    // Append data to the read cache
    read_cache_->append_data(buf->base, static_cast<size_t>(nread));
    free_read_buf(buf);

    // Check if cache exceeds limit — pause reading
    if (read_cache_->size() > max_read_cache_size_) {
      tcp_->read_stop();
      read_cache_paused_ = true;
    }
  } else if (nread < 0) {
    if (nread != UV_EOF) {
      last_error_code_ = static_cast<int>(nread);
      set_status(TCP_CLIENT_ERROR);
    } else {
      // UV_EOF: peer closed gracefully
      set_status(TCP_CLIENT_CLOSED);
    }
    // Notify close callbacks on connection close or error.
    fire_close_callbacks();
    if (buf->base != nullptr) {
      free_read_buf(buf);
    }
  } else {
    // nread == 0: no data, but EAGAIN/EWOULDBLOCK — nothing to do
    if (buf->base != nullptr) {
      free_read_buf(buf);
    }
  }
}

}  // namespace uvcpp
