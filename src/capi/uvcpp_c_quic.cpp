/**
 * @file src/capi/uvcpp_c_quic.cpp
 * @brief `uvcpp_c_quic.h` 的实现：QUIC 端点、连接（**借来的**）、TLS 配置。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件里有四件事值得先读一遍（其余都是逐条对着 C++ 方法搬的）
 * ----------------------------------------------------------------------
 * 1. **借来的句柄：谁建的、谁废的、什么时候毒化。**
 *    `uvcpp_c_quic_connection` 这个类型上一个 `_free()` 都没有（见头里那一段）。
 *    它由端点持有：
 *      - 客户端那一枚**按值住在** `uvcpp_c_quic_client` 里（`_client_new()` 那一刻
 *        就存在，那时 `conn == nullptr`）——所以地址稳定，"永远返回同一个指针"
 *        这条承诺不花任何代价；
 *      - 服务端每一枚在连接回调里 `new` 出来，记在 `server->conns` 里。
 *    两条路都汇到 `detach_conn()`：**反登记 + 毒化 + 把 `conn` 置空**。此后任何
 *    调用都是 `UVCPP_C_E_STALE`，而不是段错误。
 *
 * 2. **谁在驱动"毒化"这件事：看有没有 h3。**
 *    本文件给 C++ 连接装的那套跳板（`install_conn_callbacks()`）里就有 `on_close`
 *    那一格，本来是它负责 `detach_conn()`。但 h3 的 `start()` 会把整张回调表
 *    **换成它自己的**（C++ 侧本来就是这么设计的：h3 要吃 `on_read` / `on_alpn`），
 *    于是装了 h3 之后本文件的跳板一次都不会响 —— 那个 `on_close` 也就永远不会
 *    跑。所以毒化有**两个**入口，谁在谁负责：
 *      - 裸 QUIC（没装 h3）：本文件的 `on_close` 跳板；
 *      - 装了 h3：`uvcpp_c_http3.cpp` 那侧在 h3 的 `on_disconnect` 里叫一声
 *        `uvcpp_c_detail::quic_conn_detach()`。
 *    这条分工是本层唯一一处"两个文件共同维护一个句柄的死期"，所以写在两个文件的
 *    开头。漏掉任何一半的症状都一样：**活句柄数收不回来**（`capi_mutation.py`
 *    的 M18 量的就是它）。
 *
 * 3. **回调期间不许删任何人。** 用户回调里可以 `_conn_close()`，也可以 `_free()`
 *    端点。前者是正常用法（关闭是异步的，回调还会来）；后者当场返回
 *    `UVCPP_C_E_STATE`（`cb_depth` 非零）—— 与 `uvcpp_c_net.cpp` 那条"回调里
 *    free 是错的"同一件事，只是那边用坟场兜、这边直接拒。
 *
 * 4. **TLS 只给最小的一套。** `uvcpp_c_quic_tls` 就是 `uvcpp_ssl_context` 的一个
 *    壳，三种建法各写死了该写死的东西（版本 **TLS_1_3** —— QUIC 的握手就是
 *    TLS 1.3，C++ 的 ssl 默认值 1.2 在这里是错的；客户端默认**不校验**对端）。
 *    那些"能不能建起来"的失败（文件打不开、证书与私钥不配对）在构造期就判掉：
 *    `new` 交 NULL，同时把 OpenSSL 的原话记进 `uvcpp_c_last_error_string()`。
 */

#include "capi/uvcpp_c_quic.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_common.h>
#include <ssl/uvcpp_ssl_context.h>

#include "capi/uvcpp_c_internal.h"

// 状态枚举的编号必须逐值对上：C 侧把 `state()` 当整数交出去，错位就是"状态静默
// 换了意思"这种最难查的错。与 `uvcpp_c_net.cpp` 顶上那几条 `static_assert` 同一
// 个形状（那里钉的是读事件的编号）。
static_assert(static_cast<int>(uvcpp::quic_connection_state::IDLE) ==
                  UVCPP_C_QUIC_IDLE,
              "quic_connection_state::IDLE 必须与 UVCPP_C_QUIC_IDLE 同值");
static_assert(static_cast<int>(uvcpp::quic_connection_state::HANDSHAKING) ==
                  UVCPP_C_QUIC_HANDSHAKING,
              "quic_connection_state::HANDSHAKING 编号漂了");
static_assert(static_cast<int>(uvcpp::quic_connection_state::ESTABLISHED) ==
                  UVCPP_C_QUIC_ESTABLISHED,
              "quic_connection_state::ESTABLISHED 编号漂了");
static_assert(static_cast<int>(uvcpp::quic_connection_state::CLOSING) ==
                  UVCPP_C_QUIC_CLOSING,
              "quic_connection_state::CLOSING 编号漂了");
static_assert(static_cast<int>(uvcpp::quic_connection_state::DRAINING) ==
                  UVCPP_C_QUIC_DRAINING,
              "quic_connection_state::DRAINING 编号漂了");
static_assert(static_cast<int>(uvcpp::quic_connection_state::CLOSED) ==
                  UVCPP_C_QUIC_CLOSED,
              "quic_connection_state::CLOSED 编号漂了");

using uvcpp_c_detail::alive;
using uvcpp_c_detail::copy_out;

struct uvcpp_c_quic_client;
struct uvcpp_c_quic_server;

// ---------------------------------------------------------------------------
// 句柄
// ---------------------------------------------------------------------------

/** @brief TLS 配置句柄：一个 `uvcpp_ssl_context` 加一个句柄头。 */
struct uvcpp_c_quic_tls {
  uvcpp_c_detail::handle_head head;  ///< 必须在偏移 0
  uvcpp::uvcpp_ssl_context*   ctx;   ///< 属于本句柄（`_free()` 里 delete）
};

/**
 * @brief 连接句柄（**借来的**）。
 *
 * `conn == nullptr` 有两种意思，都不算错：客户端还没 `connect` 过（壳在、连接
 * 不在），或者底层连接已经没了（那时 `live` 是 0，任何入口都先给 `E_STALE`）。
 */
struct uvcpp_c_quic_connection {
  uvcpp_c_detail::handle_head head;  ///< 必须在偏移 0

  uvcpp::uvcpp_quic_connection* conn;  ///< 底下那条连接；nullptr = 还没有 / 已经没了
  int                           live;  ///< 1 = 还在活句柄表里

  uvcpp_c_quic_client* owner_client;  ///< 非 NULL = 客户端那一枚（嵌在它里面）
  uvcpp_c_quic_server* owner_server;  ///< 非 NULL = 服务端交出来的

  /// 用户那套回调的**一份拷贝**（按 `size` 逐格拷进来的）。跳板在**调用时刻**
  /// 读它，所以 `_conn_set_callbacks()` 只要改这一份，不用重装。
  uvcpp_c_quic_callbacks cbs;
  void*                  user_data;
  int                    installed;  ///< 1 = C++ 那侧已经装上跳板了
};

/** @brief 客户端端点。借来的那一枚连接句柄**按值**住在里面。 */
struct uvcpp_c_quic_client {
  uvcpp_c_detail::handle_head head;

  uvcpp::uvcpp_quic_client* cli;
  int                       cb_depth;  ///< 正在跑的回调层数（> 0 时不许 free）

  uvcpp_c_status_cb on_connect;
  void*             on_connect_user_data;

  uvcpp_c_quic_connection conn;  ///< 借来的：地址稳定，`_client_new()` 起就在
};

/** @brief 服务端端点。每条被接受的连接在这里有一枚堆上的借用句柄。 */
struct uvcpp_c_quic_server {
  uvcpp_c_detail::handle_head head;

  uvcpp::uvcpp_quic_server* srv;
  int                       cb_depth;

  void (*on_connection)(void* user_data, uvcpp_c_quic_connection* c);
  void* on_connection_user_data;

  /// C++ 连接对象 → 借用句柄。**同一个指针只对应一枚**（用户在连接回调里拿到的
  /// 句柄，与之后所有回调里拿到的**是同一个**）。
  std::unordered_map<uvcpp::uvcpp_quic_connection*, uvcpp_c_quic_connection*> conns;

  /// 已经毒化、等着回收的借用句柄。**不在 `detach_conn()` 里直接 delete**：那条
  /// 路是从某个回调栈里出来的，栈上还压着它的跳板（跳板捕获了这个指针）。
  /// 内存由 `_server_free()` 统一收 —— 与 `uvcpp_c_net.cpp` 的坟场同一条理由。
  std::vector<uvcpp_c_quic_connection*> retired;
};

static_assert(offsetof(uvcpp_c_quic_tls, head) == 0,
              "handle_head 必须在偏移 0：alive() 会按它读任意句柄");
static_assert(offsetof(uvcpp_c_quic_connection, head) == 0,
              "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_quic_client, head) == 0,
              "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_quic_server, head) == 0,
              "handle_head 必须在偏移 0");

namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

/** @brief `new (std::nothrow)` + 登记。所有句柄都这么造（顺序是承重的）。 */
template <typename W>
W* make_wrapper(uint32_t magic) {
  W* raw = new (std::nothrow) W();
  if (raw == nullptr) return nullptr;
  std::unique_ptr<W> w(raw);
  uvcpp_c_detail::register_head(w.get(), magic);
  return w.release();
}

/**
 * @brief 把一次 C++ 读事件摊平成 C 的结构体。
 *
 * 逐字段赋值而不是整块 `memcpy`，理由与 `uvcpp_c_net.cpp` 里那个同名函数逐字
 * 相同（两边的布局没有互相承诺）。
 *
 * @note `net_read_result` 里那个 `fin` 在 QUIC 上**真的有值**（一条流可以同时
 *       带数据与 FIN），与 TCP 那边"永远为假"不同 —— 这个结构体是同一个，
 *       语义由底下那一层决定。
 */
void fill_read_result(const uvcpp::net_read_result& r, uvcpp_c_read_result* out) {
  out->event = static_cast<int>(r.event);
  out->data  = r.data;
  out->size  = r.size;
  out->error = r.error;
  out->fin   = r.fin ? 1 : 0;
}

/**
 * @brief 借来的句柄的线程记录从它的**持有者**那里来。
 *
 * 连接自己没有循环（它的循环就是端点那条），所以"循环线程 = 谁"这件事只有端点
 * 知道。指向持有者那个 `handle_head`（而不是拷一份）是刻意的：拷一份就变成两处
 * 会各自漂的记录。
 */
uvcpp_c_detail::handle_head* conn_owner_head(uvcpp_c_quic_connection* w) {
  if (w->owner_client != nullptr) return &w->owner_client->head;
  if (w->owner_server != nullptr) return &w->owner_server->head;
  return nullptr;
}

bool conn_thread_ok(const uvcpp_c_quic_connection* w) {
  uvcpp_c_detail::handle_head* head = conn_owner_head(
      const_cast<uvcpp_c_quic_connection*>(w));
  if (head == nullptr) return true;
  return uvcpp_c_detail::thread_ok_head(head);
}

/// @brief 借来的句柄是不是"活着且底下还有连接"。
bool conn_usable(const uvcpp_c_quic_connection* w) {
  return alive(w, UVCPP_C_MAGIC_QUIC_CONN) &&
         const_cast<uvcpp_c_quic_connection*>(w)->conn != nullptr;
}

void detach_conn(uvcpp_c_quic_connection* w);

/**
 * @brief 给 C++ 连接装上跳板（**只装一次**）。
 *
 * 每个跳板都做三件事，一件不少：查句柄还活着 → 读**当前**那份表里对应的那一格
 * → 非空才调用户代码。顺序承重：用户回调里完全可能把端点 free 掉（那时
 * `cb_depth` 兜住），也可能重装回调（那时表已经换了，下一次读到的就是新的）。
 *
 * 七个跳板**一次全装上**（不是"用户给了哪几个就装哪几个"）：表是可以随时换的，
 * 少装一格就等于"这一格事后补不上"。没给的格子是 NULL，跳板自己会跳过。
 */
void install_conn_callbacks(uvcpp_c_quic_connection* w) {
  if (w->installed || w->conn == nullptr) return;
  uvcpp::uvcpp_quic_connection::callbacks cbs;

  cbs.on_read = [w](uvcpp::uvcpp_quic_connection&, int64_t sid,
                    const uvcpp::net_read_result& r) {
    if (!conn_usable(w) || w->cbs.on_read == nullptr) return;
    uvcpp_c_read_result out;
    fill_read_result(r, &out);
    w->cbs.on_read(w->user_data, w, sid, &out);
  };

  cbs.on_streams_available = [w](uvcpp::uvcpp_quic_connection&, bool bidi,
                                 uint64_t max_streams) {
    if (!conn_usable(w) || w->cbs.on_streams_available == nullptr) return;
    w->cbs.on_streams_available(w->user_data, w, bidi ? 1 : 0, max_streams);
  };

  cbs.on_stop_sending = [w](uvcpp::uvcpp_quic_connection&, int64_t sid,
                            uint64_t code) {
    if (!conn_usable(w) || w->cbs.on_stop_sending == nullptr) return;
    w->cbs.on_stop_sending(w->user_data, w, sid, code);
  };

  cbs.on_stream_open = [w](uvcpp::uvcpp_quic_connection&, int64_t sid) {
    if (!conn_usable(w) || w->cbs.on_stream_open == nullptr) return;
    w->cbs.on_stream_open(w->user_data, w, sid);
  };

  cbs.on_write = [w](uvcpp::uvcpp_quic_connection&, int64_t sid, int status) {
    if (!conn_usable(w) || w->cbs.on_write == nullptr) return;
    w->cbs.on_write(w->user_data, w, sid, status);
  };

  cbs.on_alpn = [w](uvcpp::uvcpp_quic_connection&, const std::string& alpn) {
    if (!conn_usable(w) || w->cbs.on_alpn == nullptr) return;
    w->cbs.on_alpn(w->user_data, w, alpn.c_str());
  };

  // 最后一格：**用户回调跑完就毒化**。头里那句"回调返回后不要再碰这条连接"
  // 就是这一行兑现的 —— 之后任何调用是 `E_STALE`，不是踩到一条已经拆掉的连接。
  cbs.on_close = [w](uvcpp::uvcpp_quic_connection&, int error_code) {
    if (!conn_usable(w)) return;
    if (w->cbs.on_close != nullptr) {
      w->cbs.on_close(w->user_data, w, error_code);
    }
    detach_conn(w);
  };

  w->conn->set_callbacks(cbs);
  w->installed = 1;
}

/// @brief 把一枚（还没有底的）借用句柄接上一条 C++ 连接。客户端那一路用它。
void attach_conn(uvcpp_c_quic_connection* w, uvcpp::uvcpp_quic_connection* c) {
  w->conn      = c;
  w->installed = 0;
  if (!alive(w, UVCPP_C_MAGIC_QUIC_CONN)) {
    // 上一次连接关掉时反登记过它 —— 这一枚壳要重新登记（重连那条路）。
    // **不动 `cbs` / `user_data`**：那是调用方设的，重连不该把它顺手抹掉
    // （抹掉的话"先设表、再连第二次"就是一次静默的取消失效）。
    uvcpp_c_detail::register_head(w, UVCPP_C_MAGIC_QUIC_CONN);
  }
  w->live = 1;
  install_conn_callbacks(w);
}

/**
 * @brief 毒化一枚借用句柄（**底下的连接没了**）。
 *
 * 三件事：置空 `conn`（此后所有操作看到的是 `E_STATE` 或 `E_STALE`，取决于
 * 调用点）、反登记 + 毒化魔数（此后 `alive()` 为假 → `E_STALE`）、从持有者的
 * 表里摘掉。**不 delete** —— 那条路是从回调栈里出来的，见 `retired` 那段。
 */
void detach_conn(uvcpp_c_quic_connection* w) {
  if (w == nullptr || !w->live) return;
  w->live      = 0;
  w->conn      = nullptr;
  w->installed = 0;
  uvcpp_c_detail::unregister_head(w);

  uvcpp_c_quic_server* s = w->owner_server;
  if (s != nullptr) {
    for (std::unordered_map<uvcpp::uvcpp_quic_connection*,
                            uvcpp_c_quic_connection*>::iterator it =
             s->conns.begin();
         it != s->conns.end(); ++it) {
      if (it->second == w) {
        s->conns.erase(it);
        break;
      }
    }
    s->retired.push_back(w);
  }
}

/**
 * @brief 回调期的记账：+1 进来、-1 出去，`*_free()` 靠它认出"现在正在回调里"。
 *
 * 两个端点各一个（模板省掉重复的那十行）：`T` 是 `uvcpp_c_quic_client` 或
 * `uvcpp_c_quic_server`，两者都有一个 `cb_depth` 成员。
 */
template <typename T>
class CallbackScope {
 public:
  explicit CallbackScope(T* ep) : ep_(ep) {
    if (ep_ != nullptr) ++ep_->cb_depth;
  }
  ~CallbackScope() {
    if (ep_ != nullptr) --ep_->cb_depth;
  }
  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

 private:
  T* ep_;
};

/** @brief 回调表逐格拷进来（规矩 3：按 `size` 判覆盖到哪一格）。 */
int copy_conn_callbacks(uvcpp_c_quic_connection* w,
                        const uvcpp_c_quic_callbacks* table, void* user_data) {
  std::memset(&w->cbs, 0, sizeof(w->cbs));
  w->user_data = user_data;
  if (table == nullptr) return UVCPP_C_OK;

  if (!uvcpp_c_detail::table_size_ok(table->size)) {
    uvcpp_c_detail::set_last_error(
        "uvcpp_c_quic_callbacks.size 连第一格都没盖住");
    return UVCPP_C_E_INVALID_ARG;
  }
  const uint32_t sz = table->size;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_quic_callbacks::on_read))
    w->cbs.on_read = table->on_read;
  if (uvcpp_c_detail::field_present(sz,
                                    &uvcpp_c_quic_callbacks::on_streams_available))
    w->cbs.on_streams_available = table->on_streams_available;
  if (uvcpp_c_detail::field_present(sz,
                                   &uvcpp_c_quic_callbacks::on_stop_sending))
    w->cbs.on_stop_sending = table->on_stop_sending;
  if (uvcpp_c_detail::field_present(sz,
                                    &uvcpp_c_quic_callbacks::on_stream_open))
    w->cbs.on_stream_open = table->on_stream_open;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_quic_callbacks::on_write))
    w->cbs.on_write = table->on_write;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_quic_callbacks::on_alpn))
    w->cbs.on_alpn = table->on_alpn;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_quic_callbacks::on_close))
    w->cbs.on_close = table->on_close;
  return UVCPP_C_OK;
}

/** @brief 服务端：取（或建）那条 C++ 连接对应的借用句柄。 */
uvcpp_c_quic_connection* server_conn_of(uvcpp_c_quic_server* s,
                                        uvcpp::uvcpp_quic_connection* c) {
  std::unordered_map<uvcpp::uvcpp_quic_connection*,
                     uvcpp_c_quic_connection*>::iterator it = s->conns.find(c);
  if (it != s->conns.end()) {
    if (it->second->live) return it->second;
    s->conns.erase(it);  // 上一枚已经毒化了（同一个地址被新的连接复用）
  }

  uvcpp_c_quic_connection* w =
      make_wrapper<uvcpp_c_quic_connection>(UVCPP_C_MAGIC_QUIC_CONN);
  if (w == nullptr) return nullptr;
  w->conn         = c;
  w->live         = 1;
  w->owner_client = nullptr;
  w->owner_server = s;
  w->user_data    = nullptr;
  w->installed    = 0;
  std::memset(&w->cbs, 0, sizeof(w->cbs));
  install_conn_callbacks(w);
  s->conns[c] = w;
  return w;
}

}  // namespace

// ---------------------------------------------------------------------------
// 跨文件的那一枚：给 `uvcpp_c_http3.cpp` 用
// ---------------------------------------------------------------------------
//
// 理由写在文件开头第 2 条：装了 h3 之后，本文件的 `on_close` 跳板被 h3 换掉了，
// 所以"这条连接没了"这件事得由 h3 那一侧告诉这里。
namespace uvcpp_c_detail {

void quic_conn_detach(const void* handle) {
  uvcpp_c_quic_connection* w = const_cast<uvcpp_c_quic_connection*>(
      static_cast<const uvcpp_c_quic_connection*>(handle));
  if (!alive(w, UVCPP_C_MAGIC_QUIC_CONN)) return;
  detach_conn(w);
}

uvcpp::uvcpp_quic_connection* quic_conn_unwrap(const void* handle) {
  // 与 `tcp_client_unwrap()` 逐条对齐：空句柄、已经废了的、类型不对的、底下还
  // 没有连接的 —— 四种都返回 nullptr（调用方要做的事一样：别用，报错）。
  const uvcpp_c_quic_connection* c =
      static_cast<const uvcpp_c_quic_connection*>(handle);
  if (!conn_usable(c)) return nullptr;
  return const_cast<uvcpp_c_quic_connection*>(c)->conn;
}

}  // namespace uvcpp_c_detail

// ===========================================================================
// 进程级：crypto 后端与两个只读的字符串
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_quic_crypto_init(void) {
  UVCPP_C_TRY
    return uvcpp::quic_crypto_backend_init();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_crypto_free(void) {
  UVCPP_C_TRY
    uvcpp::quic_crypto_backend_free();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_ngtcp2_version(char* buf, size_t cap) {
  UVCPP_C_TRY
    return copy_out(uvcpp::quic_ngtcp2_version_string(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_error_string(int code, char* buf,
                                                     size_t cap) {
  UVCPP_C_TRY
    return copy_out(uvcpp::quic_error_string(code), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// TLS 配置
// ===========================================================================

namespace {

/** @brief 建一个 TLS 上下文并包成句柄。`ctx` 为 NULL 时返回 NULL。 */
uvcpp_c_quic_tls* wrap_tls(std::unique_ptr<uvcpp::uvcpp_ssl_context> ctx) {
  if (ctx == nullptr) return nullptr;
  uvcpp_c_quic_tls* w = make_wrapper<uvcpp_c_quic_tls>(UVCPP_C_MAGIC_QUIC_TLS);
  if (w == nullptr) return nullptr;
  w->ctx = ctx.release();
  return w;
}

}  // namespace

extern "C" UVCPP_C_API uvcpp_c_quic_tls* uvcpp_c_quic_tls_server_new(
    const char* cert_file, const char* key_file) {
  UVCPP_C_TRY
    if (cert_file == nullptr || key_file == nullptr) return nullptr;
    // 版本写死 TLS_1_3：QUIC 的握手就是 TLS 1.3（RFC 9001 §4.2），而
    // `uvcpp_ssl_context` 的默认值是 1.2 —— 那一档在 QUIC 上根本不是"更兼容"，
    // 是握手起不来。
    std::unique_ptr<uvcpp::uvcpp_ssl_context> ctx(new uvcpp::uvcpp_ssl_context(
        uvcpp::tls_mode::SERVER, uvcpp::tls_version::TLS_1_3));
    // **证书在前、私钥在后**：`load_private_key_file()` 里面那句
    // `SSL_CTX_check_private_key()` 在还没有证书时返回假（C++ 头里写着这条），
    // 顺序反了会在正确的配置上报"加载失败"。
    if (!ctx->load_certificate_file(cert_file)) {
      uvcpp_c_detail::set_last_error(ctx->get_last_error().c_str());
      return nullptr;
    }
    if (!ctx->load_private_key_file(key_file)) {
      uvcpp_c_detail::set_last_error(ctx->get_last_error().c_str());
      return nullptr;
    }
    // 再确认一次配对：不配对在 OpenSSL 里不影响建上下文、也不影响上面那两个
    // load 的返回值，它只在**每一次握手**时失败（表现是"服务起来了、端口在听、
    // 每条连接建完就被拒"）。在这里判一次，就把它变成一次明确的构造失败。
    if (!ctx->check_private_key()) {
      uvcpp_c_detail::set_last_error(
          "证书与私钥不配对（SSL_CTX_check_private_key 为假）");
      return nullptr;
    }
    return wrap_tls(std::move(ctx));
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API uvcpp_c_quic_tls* uvcpp_c_quic_tls_server_selfsigned(
    const char* common_name) {
  UVCPP_C_TRY
    // 与 `tests/functional/*` 里那些 QUIC / h3 用例逐字同一条路：现场生成一张
    // 自签证书，于是用例不必往仓里塞 PEM、也不必依赖"证书在哪"。
    const char* cn = (common_name != nullptr) ? common_name : "localhost";
    std::unique_ptr<uvcpp::uvcpp_ssl_context> ctx(new uvcpp::uvcpp_ssl_context(
        uvcpp::tls_mode::SERVER, uvcpp::tls_version::TLS_1_3));
    if (!ctx->generate_self_signed(cn, 2048)) {
      uvcpp_c_detail::set_last_error(ctx->get_last_error().c_str());
      return nullptr;
    }
    return wrap_tls(std::move(ctx));
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API uvcpp_c_quic_tls* uvcpp_c_quic_tls_client_new(
    const char* ca_file) {
  UVCPP_C_TRY
    std::unique_ptr<uvcpp::uvcpp_ssl_context> ctx(new uvcpp::uvcpp_ssl_context(
        uvcpp::tls_mode::CLIENT, uvcpp::tls_version::TLS_1_3));
    // **默认关掉校验**：C++ 的 `tls_mode::CLIENT` 默认是 `PEER`（并已装入系统
    // 信任库），而这一层的典型用法是"连自家内网、证书是自签的" —— 默认校验会让
    // 第一次跑起来就是一条握手失败，而失败原因在 C 侧只表现为一个负的错误码。
    ctx->set_verify_mode(uvcpp::tls_verify_mode::NONE);
    if (ca_file != nullptr) {
      if (!ctx->load_ca_file(ca_file)) {
        uvcpp_c_detail::set_last_error(ctx->get_last_error().c_str());
        return nullptr;
      }
    }
    return wrap_tls(std::move(ctx));
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_tls_free(uvcpp_c_quic_tls* tls) {
  UVCPP_C_TRY
    if (tls == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(tls, UVCPP_C_MAGIC_QUIC_TLS)) return UVCPP_C_E_STALE;
    uvcpp_c_detail::unregister_head(tls);
    delete tls->ctx;
    tls->ctx = nullptr;
    delete tls;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_tls_set_ca_file(uvcpp_c_quic_tls* tls,
                                                        const char* ca_file) {
  UVCPP_C_TRY
    if (!alive(tls, UVCPP_C_MAGIC_QUIC_TLS)) return UVCPP_C_E_STALE;
    if (ca_file == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!tls->ctx->load_ca_file(ca_file)) {
      uvcpp_c_detail::set_last_error(tls->ctx->get_last_error().c_str());
      return UVCPP_C_E_INVALID_ARG;
    }
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_tls_set_verify(uvcpp_c_quic_tls* tls,
                                                       int mode) {
  UVCPP_C_TRY
    if (!alive(tls, UVCPP_C_MAGIC_QUIC_TLS)) return UVCPP_C_E_STALE;
    tls->ctx->set_verify_mode(mode != 0 ? uvcpp::tls_verify_mode::PEER
                                        : uvcpp::tls_verify_mode::NONE);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 客户端
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_quic_client* uvcpp_c_quic_client_new(void) {
  UVCPP_C_TRY
    // 先造 C++ 对象（它会建一条循环，可能抛），再造包装器 —— 顺序反过来的话，
    // 构造函数抛出去时包装器已经分配了，而唯一的句柄在栈上那个正在展开的帧里。
    std::unique_ptr<uvcpp::uvcpp_quic_client> cli(
        new uvcpp::uvcpp_quic_client());

    uvcpp_c_quic_client* w =
        make_wrapper<uvcpp_c_quic_client>(UVCPP_C_MAGIC_QUIC_CLIENT);
    if (w == nullptr) return nullptr;
    w->cli                   = cli.get();
    w->cb_depth              = 0;
    w->on_connect            = nullptr;
    w->on_connect_user_data  = nullptr;
    // 借来的那一枚：**在这里就登记**，于是"永远返回同一个指针"这条承诺不需要
    // 任何惰性分配 —— 它从 `_new()` 到 `_free()` 地址都不变。此刻它还没有底
    // （`conn == nullptr`），头里写着那时用它是 `E_STATE`。
    w->conn.conn         = nullptr;
    w->conn.live         = 1;
    w->conn.owner_client = w;
    w->conn.owner_server = nullptr;
    w->conn.user_data    = nullptr;
    w->conn.installed    = 0;
    std::memset(&w->conn.cbs, 0, sizeof(w->conn.cbs));
    uvcpp_c_detail::register_head(&w->conn, UVCPP_C_MAGIC_QUIC_CONN);

    cli.release();  // 所有权交给包装器（`_free()` 里 delete）
    return w;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_free(uvcpp_c_quic_client* client) {
  UVCPP_C_TRY
    if (client == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cb_depth > 0) {
      // 与 `uvcpp_c_net.cpp` 那条"回调里 free 是错的"同一件事。这里不摆坟场：
      // 端点句柄是**调用方所有**的，它随时可以在回调之外再 free 一次 ——
      // 拒掉比"先记下来、回头再删"少一处说不清的时序。
      uvcpp_c_detail::set_last_error(
          "uvcpp_c_quic_client_free() 不能在回调里调（关闭用 "
          "uvcpp_c_quic_client_close()）");
      return UVCPP_C_E_STATE;
    }
    // 顺序承重：**先**摘借来的句柄（它嵌在本对象里，本对象一 delete 它的地址就
    // 没了），**再**摘自己，**最后**才删 C++ 对象 —— C++ 的析构会关掉那条连接
    // 并可能回调进跳板，而那时跳板查到的已经是"句柄不在了"，直接退出去。
    detach_conn(&client->conn);
    uvcpp_c_detail::unregister_head(client);
    uvcpp::uvcpp_quic_client* cli = client->cli;
    client->cli = nullptr;
    delete cli;
    delete client;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_set_tls(
    uvcpp_c_quic_client* client, uvcpp_c_quic_tls* tls) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&client->head))
      return UVCPP_C_E_WRONG_THREAD;
    uvcpp::uvcpp_ssl_context* ctx = nullptr;
    if (tls != nullptr) {
      if (!alive(tls, UVCPP_C_MAGIC_QUIC_TLS)) return UVCPP_C_E_STALE;
      ctx = tls->ctx;
    }
    client->cli->set_ssl_context(ctx);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

namespace {

/**
 * @brief 把 C 的"一组字符串"翻成 `std::vector<std::string>`。
 *
 * `count == 0 || protos == NULL` → 交回空 vector，调用方按"恢复默认"处理
 * （见 `uvcpp_c_quic.h` 里那一格 `@note`：C++ 那边空列表是"一个都不发"，
 * 本层刻意把它定成"回到默认"）。
 *
 * @param ok 出参：元素为空指针（不是数组为空）时置 0 —— 那是参数错误。
 */
std::vector<std::string> to_protos(const char* const* protos, size_t count,
                                  bool* ok) {
  std::vector<std::string> out;
  *ok = true;
  if (protos == nullptr || count == 0) return out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (protos[i] == nullptr) {
      *ok = false;
      return std::vector<std::string>();
    }
    out.push_back(std::string(protos[i]));
  }
  return out;
}

/// @brief "恢复默认"：与 C++ 的 `quic_default_alpn()` 同一个值。
std::vector<std::string> default_protos() {
  std::vector<std::string> out;
  const char* d = uvcpp::quic_default_alpn();
  if (d != nullptr) out.push_back(std::string(d));
  return out;
}

}  // namespace

extern "C" UVCPP_C_API int uvcpp_c_quic_client_set_alpn_protos(
    uvcpp_c_quic_client* client, const char* const* protos, size_t proto_count) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    bool ok = true;
    std::vector<std::string> v = to_protos(protos, proto_count, &ok);
    if (!ok) return UVCPP_C_E_INVALID_ARG;
    if (v.empty()) v = default_protos();
    client->cli->set_alpn_protos(v);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_set_idle_timeout(
    uvcpp_c_quic_client* client, uint64_t ms) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    client->cli->set_idle_timeout(ms);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_connect(
    uvcpp_c_quic_client* client, const char* host, int port,
    uvcpp_c_status_cb cb, void* user_data) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&client->head))
      return UVCPP_C_E_WRONG_THREAD;
    if (host == nullptr || cb == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (port < 0 || port > 65535) return UVCPP_C_E_INVALID_ARG;

    client->on_connect           = cb;
    client->on_connect_user_data = user_data;

    const int rc = client->cli->connect(host, port, [client](int status) {
      if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return;
      CallbackScope<uvcpp_c_quic_client> guard(client);
      uvcpp_c_status_cb fn = client->on_connect;
      if (fn == nullptr) return;
      fn(client->on_connect_user_data, status);
    });
    if (rc != 0) return rc;

    // **就在这段窗口里接上**：C++ 那头写着"`connect()` 返回之后、下一次循环迭代
    // 之前"是装回调的唯一安全窗口，而 `connection()` 也是此刻才非空。
    uvcpp::uvcpp_quic_connection* c = client->cli->connection();
    if (c != nullptr) attach_conn(&client->conn, c);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API uvcpp_c_quic_connection*
uvcpp_c_quic_client_connection(uvcpp_c_quic_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return nullptr;
    return &client->conn;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_close(uvcpp_c_quic_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    return client->cli->close();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_run(uvcpp_c_quic_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    uvcpp_c_detail::arm_loop_thread(&client->head);
    return client->cli->get_loop()->run(UV_RUN_DEFAULT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_run_once(
    uvcpp_c_quic_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    uvcpp_c_detail::arm_loop_thread(&client->head);
    return client->cli->get_loop()->run(UV_RUN_NOWAIT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_client_stop(uvcpp_c_quic_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_QUIC_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;
    client->cli->get_loop()->stop();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 服务端
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_quic_server* uvcpp_c_quic_server_new(void) {
  UVCPP_C_TRY
    std::unique_ptr<uvcpp::uvcpp_quic_server> srv(
        new uvcpp::uvcpp_quic_server());
    uvcpp_c_quic_server* w =
        make_wrapper<uvcpp_c_quic_server>(UVCPP_C_MAGIC_QUIC_SERVER);
    if (w == nullptr) return nullptr;
    w->srv                    = srv.get();
    w->cb_depth               = 0;
    w->on_connection          = nullptr;
    w->on_connection_user_data = nullptr;
    srv.release();
    return w;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_free(uvcpp_c_quic_server* server) {
  UVCPP_C_TRY
    if (server == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->cb_depth > 0) {
      uvcpp_c_detail::set_last_error(
          "uvcpp_c_quic_server_free() 不能在回调里调（停止用 "
          "uvcpp_c_quic_server_stop()）");
      return UVCPP_C_E_STATE;
    }
    // 先把还活着的借用句柄全部毒化：C++ 的析构会拆掉每一条连接，而那时句柄
    // 必须已经在表外（否则"活句柄数"会留下一批永远不会有人回收的账）。
    for (std::unordered_map<uvcpp::uvcpp_quic_connection*,
                            uvcpp_c_quic_connection*>::iterator it =
             server->conns.begin();
         it != server->conns.end(); ++it) {
      detach_conn(it->second);
    }
    server->conns.clear();
    for (size_t i = 0; i < server->retired.size(); ++i) delete server->retired[i];
    server->retired.clear();

    uvcpp_c_detail::unregister_head(server);
    uvcpp::uvcpp_quic_server* srv = server->srv;
    server->srv = nullptr;
    delete srv;
    delete server;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_set_tls(
    uvcpp_c_quic_server* server, uvcpp_c_quic_tls* tls) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&server->head))
      return UVCPP_C_E_WRONG_THREAD;
    uvcpp::uvcpp_ssl_context* ctx = nullptr;
    if (tls != nullptr) {
      if (!alive(tls, UVCPP_C_MAGIC_QUIC_TLS)) return UVCPP_C_E_STALE;
      ctx = tls->ctx;
    }
    server->srv->set_ssl_context(ctx);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_set_alpn_protos(
    uvcpp_c_quic_server* server, const char* const* protos, size_t proto_count) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    bool ok = true;
    std::vector<std::string> v = to_protos(protos, proto_count, &ok);
    if (!ok) return UVCPP_C_E_INVALID_ARG;
    if (v.empty()) v = default_protos();
    server->srv->set_alpn_select_protos(v);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_set_idle_timeout(
    uvcpp_c_quic_server* server, uint64_t ms) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    server->srv->set_idle_timeout(ms);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_bind(uvcpp_c_quic_server* server,
                                                    const char* ip, int port) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    if (port < 0 || port > 65535) return UVCPP_C_E_INVALID_ARG;
    return server->srv->bind(ip, port);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_configured_port(
    uvcpp_c_quic_server* server) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    return server->srv->configured_port();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_configured_ip(
    uvcpp_c_quic_server* server, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    return copy_out(server->srv->configured_ip(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_listen(
    uvcpp_c_quic_server* server,
    void (*cb)(void* user_data, uvcpp_c_quic_connection* c), void* user_data) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&server->head))
      return UVCPP_C_E_WRONG_THREAD;
    if (cb == nullptr) return UVCPP_C_E_INVALID_ARG;

    server->on_connection           = cb;
    server->on_connection_user_data = user_data;

    return server->srv->listen([server](uvcpp::uvcpp_quic_connection* c) {
      if (c == nullptr) return;
      if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return;
      uvcpp_c_quic_connection* w = server_conn_of(server, c);
      if (w == nullptr) return;
      CallbackScope<uvcpp_c_quic_server> guard(server);
      void (*fn)(void*, uvcpp_c_quic_connection*) = server->on_connection;
      if (fn == nullptr) return;
      fn(server->on_connection_user_data, w);
    });
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_run(uvcpp_c_quic_server* server) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    uvcpp_c_detail::arm_loop_thread(&server->head);
    return server->srv->run(UV_RUN_DEFAULT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_run_once(
    uvcpp_c_quic_server* server) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    uvcpp_c_detail::arm_loop_thread(&server->head);
    return server->srv->run(UV_RUN_NOWAIT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_server_stop(uvcpp_c_quic_server* server) {
  UVCPP_C_TRY
    if (!alive(server, UVCPP_C_MAGIC_QUIC_SERVER)) return UVCPP_C_E_STALE;
    if (server->srv == nullptr) return UVCPP_C_E_STALE;
    server->srv->stop();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 连接（借来的句柄）
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_quic_conn_set_callbacks(
    uvcpp_c_quic_connection* c, const uvcpp_c_quic_callbacks* cbs,
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_QUIC_CONN)) return UVCPP_C_E_STALE;
    if (!conn_thread_ok(c)) return UVCPP_C_E_WRONG_THREAD;
    // 底下还没有连接**不是**错误：这一格只是把表记在壳里，等接上时自然会用
    // （客户端那条路就是"先记表、再 connect"）。跳板读的永远是"此刻这一份表"。
    return copy_conn_callbacks(c, cbs, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_conn_state(
    const uvcpp_c_quic_connection* c) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_QUIC_CONN)) return UVCPP_C_E_STALE;
    // 还没有底 → IDLE（"还没有对端"），与 C++ 的 `state()` 在同样处境下的取值
    // 是同一条意思；不是错误码（头里写着）。
    if (!conn_usable(c)) return UVCPP_C_QUIC_IDLE;
    return static_cast<int>(c->conn->state());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_conn_alpn_selected(
    const uvcpp_c_quic_connection* c, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(c, UVCPP_C_MAGIC_QUIC_CONN)) return UVCPP_C_E_STALE;
    if (!conn_usable(c)) return copy_out(std::string(), buf, cap);
    return copy_out(c->conn->alpn_selected(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

namespace {

/**
 * @brief 那几则"要底下有连接"的入口的公共检查（流操作 / 关闭）。
 *
 * **三种处境分得开**，这是本层唯一一处会把它们同时摆在一起的地方：
 * 句柄已经废了 → `E_STALE`；句柄活着但用错了线程 → `E_WRONG_THREAD`；
 * 句柄活着、线程也对、但底下还没有（或已经没有）连接 → `E_STATE`。
 *
 * @return `UVCPP_C_OK` = 可以继续；非 0 = 该当场返回的错误码。
 */
int conn_require(uvcpp_c_quic_connection* c) {
  if (!alive(c, UVCPP_C_MAGIC_QUIC_CONN)) return UVCPP_C_E_STALE;
  if (!conn_thread_ok(c)) return UVCPP_C_E_WRONG_THREAD;
  if (c->conn == nullptr) return UVCPP_C_E_STATE;
  return UVCPP_C_OK;
}

}  // namespace

extern "C" UVCPP_C_API int64_t uvcpp_c_quic_conn_open_stream(
    uvcpp_c_quic_connection* c, int bidi) {
  UVCPP_C_TRY
    const int rc = conn_require(c);
    if (rc != UVCPP_C_OK) return rc;
    return c->conn->open_stream(bidi != 0);
  UVCPP_C_CATCH(static_cast<int64_t>(UVCPP_C_E_EXCEPTION))
}

extern "C" UVCPP_C_API int uvcpp_c_quic_conn_write_stream(
    uvcpp_c_quic_connection* c, int64_t stream_id, const char* data, size_t len,
    int end_stream) {
  UVCPP_C_TRY
    const int rc = conn_require(c);
    if (rc != UVCPP_C_OK) return rc;
    if (data == nullptr && len != 0) return UVCPP_C_E_INVALID_ARG;
    return c->conn->write_stream(stream_id, data, len, end_stream != 0);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_conn_shutdown_stream(
    uvcpp_c_quic_connection* c, int64_t stream_id, uint64_t app_error_code) {
  UVCPP_C_TRY
    const int rc = conn_require(c);
    if (rc != UVCPP_C_OK) return rc;
    return c->conn->shutdown_stream(stream_id, app_error_code);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_conn_shutdown_stream_read(
    uvcpp_c_quic_connection* c, int64_t stream_id, uint64_t app_error_code) {
  UVCPP_C_TRY
    const int rc = conn_require(c);
    if (rc != UVCPP_C_OK) return rc;
    return c->conn->shutdown_stream_read(stream_id, app_error_code);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API uint64_t uvcpp_c_quic_conn_streams_left(
    const uvcpp_c_quic_connection* c, int bidi) {
  UVCPP_C_TRY
    if (!conn_usable(c)) return 0;  // 没连接 = 没额度，与 C++ 那条"0 条"一致
    return c->conn->streams_left(bidi != 0);
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_quic_conn_close(uvcpp_c_quic_connection* c,
                                                   int error_code) {
  UVCPP_C_TRY
    const int rc = conn_require(c);
    if (rc != UVCPP_C_OK) return rc;
    return c->conn->close(error_code);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}
