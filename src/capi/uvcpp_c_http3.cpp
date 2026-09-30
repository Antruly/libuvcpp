/**
 * @file src/capi/uvcpp_c_http3.cpp
 * @brief `uvcpp_c_http3.h` 的实现：一层薄适配，所有承重的规矩都在
 *        `uvcpp_c_internal.h` 里。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一份做四件事
 * --------------
 * 1. **把 C 的回调表翻成 `uvcpp_h3_connection::callbacks`**：四条各一个跳板。
 * 2. **回调期句柄**（`uvcpp_c_h3_request_view`）在跳板的**栈上**造、登记，出栈时
 *    由 `FrameScope` 摘表 —— 与 `uvcpp_c_http2.cpp` 里那一个逐字同形（含"构造
 *    器在回调里抛异常那条路径也漏不掉"这一条）。
 * 3. **把两个构造器（请求 / 响应）在提交的那一刻拷成 C++ 对象**（规矩 4 第一类）。
 * 4. **每次进用户代码之前先把要用的东西取出来**（见下面那一段）。
 *
 * 为什么每个跳板都"先把值取出来再进用户代码"
 * -------------------------------------------
 * 这一层与 h2 那一层有一处**实质差别**：h3 的头里把"`on_disconnect` 里必须
 * `uvcpp_c_h3_connection_free(h)`"写成了硬要求（C++ 那边就是这么设计的：本对象
 * 不拥有 QUIC 连接，连接一没它就没有立足点）。于是**用户代码在回调里删掉包装器
 * 是一条正常路径，不是误用**。
 *
 * C++ 那一侧自己扛得住这件事：它每次调用户回调之前都把 `std::function` **拷一份
 * 到局部**（`uvcpp_h3_connection.cpp` 里 `if (cbs_.on_stream_close) cb =
 * cbs_.on_stream_close;` 那一套），本对象被删掉时那个局部副本照样活到调用返回，
 * 之后每一次用到 `this` 的地方前面都有一次 `token_alive()`。
 *
 * 本层的跳板**没有**那样的保护 —— 它捕获的是一个裸指针。所以纪律落在这里：
 * **进用户代码之前，把它要用的每一格都读到局部变量里；用户代码返回之后，
 * 一个字节都不许再碰 `h`。** 这条例外只有一个，而且是刻意留的：`on_disconnect`
 * 的末尾要叫一声 `uvcpp_c_detail::quic_conn_detach()` 去毒化那枚**借来的**连接
 * 句柄 —— 它用的是进用户代码**之前**就取出来的那个指针值，所以那条路也不碰 `h`。
 *
 * （为什么不干脆在 `_free()` 里毒化那枚句柄：那枚句柄属于**端点**，不属于本
 * 对象。"h3 对象被释放"不等于"QUIC 连接没了" —— 用户完全可能提前扔掉 h3 但继续
 * 用那条连接。所以毒化只由"连接真的没了"这一件事触发，也就是这里。）
 */

#include "capi/uvcpp_c_http3.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>

#include "capi/uvcpp_c_internal.h"

// ---------------------------------------------------------------------------
// 句柄的真身
// ---------------------------------------------------------------------------
//
// 四份都定义在**全局作用域**：头里那句 `typedef struct uvcpp_c_h3_connection
// uvcpp_c_h3_connection;` 声明的是 `::uvcpp_c_h3_connection` 这个不完整类型，
// 下面这四份就是**把它定义完整**的地方。放进匿名命名空间会造出四个**不同的**
// 类型，那时导出函数的签名与头里那份对不上，表现是一屏 "redeclared as
// different kind of entity"（与 `uvcpp_c_http2.cpp` 顶上那一段同一个形状）。

/**
 * @brief h3 驱动层句柄。**调用方建、调用方废。**
 *
 * 回调表**按值存**：C 侧常见写法是拿一个栈上的 `uvcpp_c_h3_callbacks` 传进来，
 * 存指针的话 `_start()` 一返回那张表就悬垂了（理由与 `uvcpp_c_h2_connection`
 * 逐字相同）。
 */
struct uvcpp_c_h3_connection {
  uvcpp_c_detail::handle_head head;

  uvcpp::uvcpp_h3_connection* conn;  ///< 本层 new 的，`_free()` 里 delete

  /// **借来的**，不拥有、也不 free（见 `uvcpp_c_quic.h` 文件头）。
  /// 存着它只有一个用处：连接真没了的时候叫一声 `quic_conn_detach()`。
  uvcpp_c_quic_connection* quic;

  void* user_data;

  /// 只装"用户真的给了"的那几格（按 `size` 逐字段判）。
  uvcpp_c_h3_callbacks cbs;
};

/** @brief 请求构造器。`req` 按值存着 —— 每个 `_set_*` 改的都是它。 */
struct uvcpp_c_h3_request {
  uvcpp_c_detail::handle_head head;
  uvcpp::h3_request          req;
};

/** @brief 响应构造器 / 接收容器。两用，与 C++ 那个结构体一样。 */
struct uvcpp_c_h3_response {
  uvcpp_c_detail::handle_head head;
  uvcpp::h3_response         resp;
};

/**
 * @brief `on_request` 递出来的**回调期**视图。
 *
 * 指向 C++ 会话攒出来的那条请求，**只在这次回调里有效** —— 回调一返回本层就
 * 反登记 + 毒化，再用是 `UVCPP_C_E_STALE`。
 */
struct uvcpp_c_h3_request_view {
  uvcpp_c_detail::handle_head head;
  const uvcpp::h3_request*    req;
};

// `alive()` 会把这几个句柄当作 `handle_head*` 读第一格 —— 偏移 0 这件事必须
// 钉住。请求 / 响应那两个**不写 `offsetof` 断言**：它们的第二个成员带
// `std::string` / `std::vector`，于是类型不是 standard-layout，对非
// standard-layout 用 `offsetof` 只是"条件支持"（gcc 会给 `-Winvalid-offsetof`）。
// 同一个不对称、同一条理由写在 `uvcpp_c_http2.cpp` 里。
static_assert(offsetof(uvcpp_c_h3_connection, head) == 0,
              "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_h3_request_view, head) == 0,
              "handle_head 必须在偏移 0");

namespace {

using uvcpp_c_detail::alive;
using uvcpp_c_detail::copy_out;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

/** @brief `new (std::nothrow)` + 登记。所有"用户建"的句柄都这么造。 */
template <typename W>
W* make_wrapper(uint32_t magic) {
  W* raw = new (std::nothrow) W();
  if (raw == nullptr) return nullptr;
  std::unique_ptr<W> w(raw);
  uvcpp_c_detail::register_head(w.get(), magic);
  return w.release();
}

/**
 * @brief 反登记 + 毒化 + 删。`*_free()` 的全部内容。
 *
 * 与 `uvcpp_c_http2.cpp` 里那个同名模板逐字同形，包括"空指针"与"用过的句柄"
 * 这两个错误码的分工（`E_INVALID_ARG` / `E_STALE`）—— 那一段是这一层对外
 * 承诺的一部分，抄成两份就会漂。**连接那一枚不用它**：它自己不拥有底下的
 * C++ 对象（那个是 `uvcpp_h3_connection`，得单独 delete），见 `_free()` 里那几行。
 */
template <typename W>
int free_wrapper(W* w, uint32_t magic) {
  if (w == nullptr) return UVCPP_C_E_INVALID_ARG;
  if (!alive(w, magic)) return UVCPP_C_E_STALE;
  uvcpp_c_detail::unregister_head(w);
  delete w;
  return UVCPP_C_OK;
}

/** @brief 带头部名的查找用的：ASCII 大小写无关比较。 */
bool name_equals(const std::string& a, const char* b) {
  const size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    // `static_cast<unsigned char>`：`std::tolower` 的参数得是"可表示为
    // unsigned char 的值"，直接喂一个负的 `char` 是 UB（UB 的那一档正是
    // 头名里出现非 ASCII 字节时）。
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

/// @brief 在一条头列表里找一个名字（大小写不敏感）。找不到返回 NULL。
const uvcpp::h3_header* find_header(const std::vector<uvcpp::h3_header>& hs,
                                    const char* name) {
  if (name == nullptr) return nullptr;
  for (size_t i = 0; i < hs.size(); ++i) {
    if (name_equals(hs[i].name, name)) return &hs[i];
  }
  return nullptr;
}

/**
 * @brief 回调期句柄的登记范围（一份作用域一格）。
 *
 * 与 `uvcpp_c_http2.cpp` 里的 `FrameScope` 逐字同形，包括那条**量出来的**事：
 * 句柄住在跳板的栈上，回调一返回那块栈就被后续循环代码复用，**魔数会被无关
 * 写入盖掉**。所以"读魔数"区分不出"被毒化"和"被栈复用盖掉" —— 出了回调再用
 * 它，判据是**登记表已经摘干净了**（`alive()` 先查表、表里没有再读魔数），
 * 拿到的一定是 `UVCPP_C_E_STALE`。
 *
 * 用作用域而不是手写一句 `unregister`：用户回调里抛异常（C 回调不会，但 C++
 * 侧的同名路径会）时栈展开照样会走到析构。
 */
class FrameScope {
 public:
  FrameScope() : h_(nullptr) {}
  ~FrameScope() {
    if (h_ != nullptr) uvcpp_c_detail::unregister_head(h_);
  }
  uvcpp_c_h3_request_view* arm(uvcpp_c_h3_request_view* v) {
    uvcpp_c_detail::register_head(v, UVCPP_C_MAGIC_H3_REQ_VIEW);
    // 与 h2 那一侧同一个理由：句柄虽然只活一次回调，但它是"这条循环上的东西"，
    // 记上线程归属，出了回调之后再拿线程规则去问它还有话说。
    uvcpp_c_detail::arm_loop_thread(v);
    h_ = reinterpret_cast<uvcpp_c_detail::handle_head*>(v);
    return v;
  }
  FrameScope(const FrameScope&) = delete;
  FrameScope& operator=(const FrameScope&) = delete;

 private:
  uvcpp_c_detail::handle_head* h_;
};

// ---------------------------------------------------------------------------
// 跳板
// ---------------------------------------------------------------------------
//
// 每一条的解剖：先把"用户代码要用的东西"全取到局部 → 没有这一格就直接返回
// （**一个字节都不碰**，这就是"表里没覆盖的当不关心"的落点）→ 调用户代码 →
// **什么也不再碰**（见文件头那一段）。
//
// 函数指针每次都**现读** `h->cbs.xxx`（不是把它拷进 lambda 的自由变量）：这样
// 换一张表、或者再 `start()` 一次，装的还是同一个 lambda，读到的却是新的那一格。

void call_on_request(uvcpp_c_h3_connection* h,
                     const uvcpp::h3_request& req) {
  void (*fn)(void*, uvcpp_c_h3_connection*, const uvcpp_c_h3_request_view*) =
      h->cbs.on_request;
  if (fn == nullptr) return;
  void* ud = h->user_data;

  uvcpp_c_h3_request_view view;
  view.req = &req;
  FrameScope scope;
  scope.arm(&view);
  fn(ud, h, &view);
}

void call_on_stream_close(uvcpp_c_h3_connection* h,
                          const uvcpp::h3_stream_close_info& info) {
  void (*fn)(void*, uvcpp_c_h3_connection*, const uvcpp_c_h3_stream_close_info*) =
      h->cbs.on_stream_close;
  if (fn == nullptr) return;
  void* ud = h->user_data;

  uvcpp_c_h3_stream_close_info out;
  // 逐字段赋值，不整块 memcpy：两边的布局没有互相承诺（与 net 那侧
  // `fill_result()` 同一条）。`size` 是这张结构体的自检栏，照头里那句填上。
  out.size             = static_cast<uint32_t>(sizeof(out));
  out.stream_id        = info.stream_id;
  out.rx_error         = info.rx_error ? 1 : 0;
  out.tx_error         = info.tx_error ? 1 : 0;
  out.rx_app_error_code = info.rx_app_error_code;
  out.tx_app_error_code = info.tx_app_error_code;
  fn(ud, h, &out);
}

void call_on_error(uvcpp_c_h3_connection* h, int error_code) {
  void (*fn)(void*, uvcpp_c_h3_connection*, int) = h->cbs.on_error;
  if (fn == nullptr) return;
  void* ud = h->user_data;
  fn(ud, h, error_code);
}

void call_on_disconnect(uvcpp_c_h3_connection* h) {
  // ★ 这两句**必须在用户代码之前**：用户完全可能在这条回调里把 `h` free 掉
  //   （头里写着"必须"），那之后 `h` 就是一块已释放的内存。取出来的这两个值
  //   都是纯数据，活到本函数返回没问题。
  void (*fn)(void*, uvcpp_c_h3_connection*) = h->cbs.on_disconnect;
  void* ud    = h->user_data;
  void* qconn = h->quic;
  if (fn != nullptr) fn(ud, h);
  // 连接真的没了 —— 那枚借来的 QUIC 连接句柄到此为止。**放在用户回调之后**是
  // 为了在 `on_disconnect` 里它还是可用的（那一刻 C++ 那侧的连接对象确实还
  // 活着）；`quic_conn_detach()` 幂等，用户在自己那句 free 里没做过也不影响。
  uvcpp_c_detail::quic_conn_detach(qconn);
}

/// @brief 活着吗。两个都过了才给用。
inline bool ok(const uvcpp_c_h3_connection* h) {
  return alive(h, UVCPP_C_MAGIC_H3_CONN) && h->conn != nullptr;
}

/// @brief 那几则只读访问器的公共入口检查。
inline bool readonly_ok(const uvcpp_c_h3_connection* h) {
  return ok(h) && uvcpp_c_detail::thread_ok(h);
}

/// @brief 回调表逐格拷进来（规矩 3：按 `size` 判覆盖到哪一格）。
int copy_h3_callbacks(uvcpp_c_h3_connection* h,
                      const uvcpp_c_h3_callbacks* table, void* user_data) {
  std::memset(&h->cbs, 0, sizeof(h->cbs));
  h->user_data = user_data;
  if (table == nullptr) return UVCPP_C_OK;

  if (!uvcpp_c_detail::table_size_ok(table->size)) {
    uvcpp_c_detail::set_last_error("uvcpp_c_h3_callbacks.size 连第一格都没盖住");
    return UVCPP_C_E_INVALID_ARG;
  }
  const uint32_t sz = table->size;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_h3_callbacks::on_request))
    h->cbs.on_request = table->on_request;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_h3_callbacks::on_stream_close))
    h->cbs.on_stream_close = table->on_stream_close;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_h3_callbacks::on_error))
    h->cbs.on_error = table->on_error;
  if (uvcpp_c_detail::field_present(sz, &uvcpp_c_h3_callbacks::on_disconnect))
    h->cbs.on_disconnect = table->on_disconnect;
  return UVCPP_C_OK;
}

}  // namespace

// ===========================================================================
// 版本串
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_h3_version(char* buf, size_t cap) {
  UVCPP_C_TRY
    return copy_out(uvcpp::uvcpp_h3_connection::nghttp3_version(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 连接
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_h3_connection* uvcpp_c_h3_connection_new(
    uvcpp_c_quic_connection* conn, int server_side) {
  UVCPP_C_TRY
    // 先把底下那条 QUIC 连接解出来：解不出来就没有"接在谁身上"这回事，也就
    // 没必要去 new 一个马上要删的对象（`quic_conn_unwrap()` 连"借来的句柄底下
    // 还没有连接"这一格也判掉了）。
    uvcpp::uvcpp_quic_connection* qc = uvcpp_c_detail::quic_conn_unwrap(conn);
    if (qc == nullptr) return nullptr;

    // 顺序与 net / h2 那两侧逐字相同：**先造可能抛的 C++ 对象，再造包装器**。
    std::unique_ptr<uvcpp::uvcpp_h3_connection> c(
        new uvcpp::uvcpp_h3_connection(qc, server_side != 0));

    uvcpp_c_h3_connection* w =
        make_wrapper<uvcpp_c_h3_connection>(UVCPP_C_MAGIC_H3_CONN);
    if (w == nullptr) return nullptr;
    w->conn      = c.get();
    w->quic      = conn;
    w->user_data = nullptr;
    std::memset(&w->cbs, 0, sizeof(w->cbs));

    c.release();  // 所有权交给包装器（`_free()` 里 delete）
    return w;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_connection_free(uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (h == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(h, UVCPP_C_MAGIC_H3_CONN)) return UVCPP_C_E_STALE;

    // 先摘表再删：反过来的话，删除过程中（C++ 析构里可能有回调）本句柄还在表
    // 里，一次重入就会拿着一块正在被释放的内存进来。
    //
    // **这里不毒化那枚借来的 QUIC 句柄**：它属于端点，不属于本对象（见文件头）。
    uvcpp_c_detail::unregister_head(h);
    uvcpp::uvcpp_h3_connection* c = h->conn;
    h->conn = nullptr;
    delete c;
    delete h;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_start(uvcpp_c_h3_connection* h,
                                                 const uvcpp_c_h3_callbacks* cbs,
                                                 void* user_data) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;

    const int rc = copy_h3_callbacks(h, cbs, user_data);
    if (rc != UVCPP_C_OK) return rc;

    uvcpp::uvcpp_h3_connection::callbacks c;
    // 四条**全装上**（不是"用户给了哪几条就装哪几条"）：表可以换，少装一格就是
    // "这一格事后补不上"。没给的格子是 NULL，跳板自己会跳过。
    c.on_request = [h](uvcpp::uvcpp_h3_connection&,
                       const uvcpp::h3_request& req) {
      if (!alive(h, UVCPP_C_MAGIC_H3_CONN)) return;
      call_on_request(h, req);
    };
    c.on_stream_close = [h](uvcpp::uvcpp_h3_connection&,
                            const uvcpp::h3_stream_close_info& info) {
      if (!alive(h, UVCPP_C_MAGIC_H3_CONN)) return;
      call_on_stream_close(h, info);
    };
    c.on_error = [h](uvcpp::uvcpp_h3_connection&, int error_code) {
      if (!alive(h, UVCPP_C_MAGIC_H3_CONN)) return;
      call_on_error(h, error_code);
    };
    c.on_disconnect = [h](uvcpp::uvcpp_h3_connection&) {
      // 用户可能已经在别的回调里 free 过 `h` 了（那是允许的）—— 那时
      // `alive()` 为假，什么都不做。这一句对一块**已释放**的地址也是安全的：
      // 它先查登记表，一个字节都不读调用方那块内存（见 `uvcpp_c_internal.h`）。
      if (!alive(h, UVCPP_C_MAGIC_H3_CONN)) return;
      call_on_disconnect(h);
    };
    return h->conn->start(c);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int64_t uvcpp_c_h3_conn_send_request(
    uvcpp_c_h3_connection* h, const uvcpp_c_h3_request* req) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;
    if (req == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(req, UVCPP_C_MAGIC_H3_REQUEST)) return UVCPP_C_E_STALE;
    return h->conn->send_request(req->req);
  UVCPP_C_CATCH(static_cast<int64_t>(UVCPP_C_E_EXCEPTION))
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_take_completed(
    uvcpp_c_h3_connection* h, uvcpp_c_h3_response* out) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;
    if (out == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(out, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    // C++ 那句 `take_completed(h3_response&)` 是**整条赋值**，于是状态码、头表、
    // body **全部**被替换掉 —— 头里那句"`out` 会先被重置"就是它兑现的，本层不
    // 需要（也不该）再手写一遍重置：手写一遍就多一处会与它漂的东西。
    return h->conn->take_completed(out->resp) ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_completed_count(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;
    return static_cast<int>(h->conn->completed_count());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_send_response(
    uvcpp_c_h3_connection* h, const uvcpp_c_h3_response* resp, int omit_body) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;
    if (resp == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    return h->conn->send_response(resp->resp, omit_body != 0);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_send_status(uvcpp_c_h3_connection* h,
                                                       int64_t stream_id,
                                                       int status,
                                                       const char* body,
                                                       size_t body_len) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;
    if (body == nullptr && body_len != 0) return UVCPP_C_E_INVALID_ARG;
    // `body` 可以为 NULL / 0 —— 那一刻就是"只发 :status"。空指针 + 非零长度是
    // 参数错误，不是"空 body"（与 `uvcpp_c_net.cpp` 里那条同一条规矩）。
    const std::string b = (body != nullptr) ? std::string(body, body_len)
                                            : std::string();
    return h->conn->send_status(stream_id, status, b);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_flush(uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;
    return h->conn->flush();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_close(uvcpp_c_h3_connection* h,
                                                 int error_code) {
  UVCPP_C_TRY
    if (!ok(h)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(h)) return UVCPP_C_E_WRONG_THREAD;
    return h->conn->close(error_code);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_ready(const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return UVCPP_C_E_STALE;
    return h->conn->ready() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_closed(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return UVCPP_C_E_STALE;
    return h->conn->closed() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_server_side(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return UVCPP_C_E_STALE;
    return h->conn->server_side() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_conn_alpn_selected(
    const uvcpp_c_h3_connection* h, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return UVCPP_C_E_STALE;
    return copy_out(h->conn->alpn_selected(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int64_t uvcpp_c_h3_conn_control_stream_id(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return UVCPP_C_E_STALE;
    return h->conn->control_stream_id();
  UVCPP_C_CATCH(static_cast<int64_t>(UVCPP_C_E_EXCEPTION))
}

extern "C" UVCPP_C_API int64_t uvcpp_c_h3_conn_qpack_encoder_stream_id(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return UVCPP_C_E_STALE;
    return h->conn->qpack_encoder_stream_id();
  UVCPP_C_CATCH(static_cast<int64_t>(UVCPP_C_E_EXCEPTION))
}

extern "C" UVCPP_C_API int64_t uvcpp_c_h3_conn_qpack_decoder_stream_id(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return UVCPP_C_E_STALE;
    return h->conn->qpack_decoder_stream_id();
  UVCPP_C_CATCH(static_cast<int64_t>(UVCPP_C_E_EXCEPTION))
}

extern "C" UVCPP_C_API uint64_t uvcpp_c_h3_conn_bytes_in(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return 0;
    return static_cast<uint64_t>(h->conn->bytes_in());
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API uint64_t uvcpp_c_h3_conn_bytes_out(
    const uvcpp_c_h3_connection* h) {
  UVCPP_C_TRY
    if (!readonly_ok(h)) return 0;
    return static_cast<uint64_t>(h->conn->bytes_out());
  UVCPP_C_CATCH(0)
}

// ===========================================================================
// 请求构造器
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_h3_request* uvcpp_c_h3_request_new(
    const char* method, const char* path) {
  UVCPP_C_TRY
    // 两个参数任一为空 → NULL（与 `uvcpp_c_h2_request_new()` 逐字同一条：
    // 失败原因不在这里报）。**也不替谁填默认值**：一个悄悄变成 `GET /` 的
    // 构造器，比一次明确的 NULL 难查得多。
    if (method == nullptr || path == nullptr) return nullptr;
    uvcpp_c_h3_request* r =
        make_wrapper<uvcpp_c_h3_request>(UVCPP_C_MAGIC_H3_REQUEST);
    if (r == nullptr) return nullptr;
    r->req.method = method;
    r->req.path   = path;
    return r;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_free(uvcpp_c_h3_request* req) {
  UVCPP_C_TRY
    return free_wrapper(req, UVCPP_C_MAGIC_H3_REQUEST);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_set_scheme(
    uvcpp_c_h3_request* req, const char* scheme) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_H3_REQUEST)) return UVCPP_C_E_STALE;
    if (scheme == nullptr) return UVCPP_C_E_INVALID_ARG;
    req->req.scheme = scheme;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_set_authority(
    uvcpp_c_h3_request* req, const char* authority) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_H3_REQUEST)) return UVCPP_C_E_STALE;
    if (authority == nullptr) return UVCPP_C_E_INVALID_ARG;
    req->req.authority = authority;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

namespace {

/**
 * @brief 在一条头列表里设一个头：同名（大小写不敏感）覆盖，没有就追加。
 *
 * 与 C++ 的 `uvcpp_http_request::set_header()` 是同一条语义 —— 覆盖而不是
 * "又塞一条"。头里每个 `_set_*` 都写着"覆盖"。
 */
void upsert_header(std::vector<uvcpp::h3_header>& hs, const char* name,
                   const char* value) {
  for (size_t i = 0; i < hs.size(); ++i) {
    if (name_equals(hs[i].name, name)) {
      hs[i].value = value;
      return;
    }
  }
  uvcpp::h3_header h;
  h.name  = name;
  h.value = value;
  h.kind  = uvcpp::h3_header_kind::REGULAR;
  hs.push_back(h);
}

}  // namespace

extern "C" UVCPP_C_API int uvcpp_c_h3_request_set_header(
    uvcpp_c_h3_request* req, const char* name, const char* value) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_H3_REQUEST)) return UVCPP_C_E_STALE;
    if (name == nullptr || value == nullptr) return UVCPP_C_E_INVALID_ARG;
    upsert_header(req->req.headers, name, value);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_set_body(uvcpp_c_h3_request* req,
                                                       const void* data,
                                                       size_t len) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_H3_REQUEST)) return UVCPP_C_E_STALE;
    if (data == nullptr && len != 0) return UVCPP_C_E_INVALID_ARG;
    if (data == nullptr) {
      req->req.body.clear();
    } else {
      // 拷一份（规矩 4 第一类）：`data` 的生存期到本函数返回为止。
      req->req.body.assign(static_cast<const char*>(data), len);
    }
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 响应构造器 / 接收容器
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_h3_response* uvcpp_c_h3_response_new(int status) {
  UVCPP_C_TRY
    uvcpp_c_h3_response* r =
        make_wrapper<uvcpp_c_h3_response>(UVCPP_C_MAGIC_H3_RESPONSE);
    if (r == nullptr) return nullptr;
    r->resp.status = status;  // 0 = "还没定"（客户端侧接 take_completed 的起手式）
    return r;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_free(uvcpp_c_h3_response* resp) {
  UVCPP_C_TRY
    return free_wrapper(resp, UVCPP_C_MAGIC_H3_RESPONSE);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_set_status(
    uvcpp_c_h3_response* resp, int status) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    resp->resp.status = status;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_set_stream_id(
    uvcpp_c_h3_response* resp, int64_t stream_id) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    // **负数当场撂倒**：`-1` 是"还没有归属"的日子值，不是一条流。C++ 那一侧的
    // `send_response()` 会以 `UV_EINVAL` 拒收，但那是发的时候才知道 —— 在这里
    // 就拦掉，调用方拿到的位置离犯错的地方更近。
    if (stream_id < 0) return UVCPP_C_E_INVALID_ARG;
    resp->resp.stream_id = stream_id;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_set_header(
    uvcpp_c_h3_response* resp, const char* name, const char* value) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    if (name == nullptr || value == nullptr) return UVCPP_C_E_INVALID_ARG;
    upsert_header(resp->resp.headers, name, value);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_set_content_type(
    uvcpp_c_h3_response* resp, const char* content_type) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    if (content_type == nullptr) return UVCPP_C_E_INVALID_ARG;
    // 就是设一个同名头 —— 单独给一格只是为了让 C 侧少打一遍字（头里那么写的）。
    upsert_header(resp->resp.headers, "content-type", content_type);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_set_body(
    uvcpp_c_h3_response* resp, const void* data, size_t len) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    if (data == nullptr && len != 0) return UVCPP_C_E_INVALID_ARG;
    if (data == nullptr) {
      resp->resp.body.clear();
    } else {
      resp->resp.body.assign(static_cast<const char*>(data), len);
    }
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_reset(uvcpp_c_h3_response* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    // 整条换一个新的：状态码 0（"还没定"）、空头表、空 body，与 C++ 那个
    // 结构体的默认值逐字段相同。
    resp->resp = uvcpp::h3_response();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_status(
    const uvcpp_c_h3_response* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    // 0 是合法的"本层给的初值"，但**"没拿到状态码"必须有一枚自己的码**：
    // 请求在响应头到达之前被掐断时（`_error()` 非 0），业务层要能说出"没有
    // 状态码"这句话。与批 3a 的 `uvcpp_c_h2_stream_response_status()` 同一条。
    if (resp->resp.status == 0) return UVCPP_C_E_NOT_FOUND;
    return resp->resp.status;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int64_t uvcpp_c_h3_response_stream_id(
    const uvcpp_c_h3_response* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    return resp->resp.stream_id;
  UVCPP_C_CATCH(static_cast<int64_t>(UVCPP_C_E_EXCEPTION))
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_error(
    const uvcpp_c_h3_response* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    return resp->resp.error;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_h3_response_body_size(
    const uvcpp_c_h3_response* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return 0;
    return resp->resp.body.size();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_body(
    const uvcpp_c_h3_response* resp, void* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    return copy_out(resp->resp.body, static_cast<char*>(buf), cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_header(
    const uvcpp_c_h3_response* resp, const char* name, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    // "带了但值是空的"在 HTTP 里是两件事，所以先查有没有、再取值 —— 与
    // `uvcpp_c_h2_stream_response_header()` 那两句逐字同形。
    const uvcpp::h3_header* f = find_header(resp->resp.headers, name);
    if (f == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(f->value, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_response_has_header(
    const uvcpp_c_h3_response* resp, const char* name) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H3_RESPONSE)) return UVCPP_C_E_STALE;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    return find_header(resp->resp.headers, name) != nullptr ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 请求视图（回调期）
// ===========================================================================
//
// 一列只读访问器。伪头是**具名**的那几个（与 C++ 把 method / scheme /
// authority / path 拆成四个成员一致）。每一条的入口检查都是**同一个**：
// `alive()` 为假就是 `E_STALE`（回调已经返回 / 传了别的类型 / 传了空指针）。

namespace {

/// @brief 视图那几则访问器的公共入口检查。
const uvcpp::h3_request* view_req(const uvcpp_c_h3_request_view* v) {
  if (!alive(v, UVCPP_C_MAGIC_H3_REQ_VIEW)) return nullptr;
  return v->req;
}

}  // namespace

extern "C" UVCPP_C_API int uvcpp_c_h3_request_view_method(
    const uvcpp_c_h3_request_view* v, char* buf, size_t cap) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    return copy_out(r->method, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_view_scheme(
    const uvcpp_c_h3_request_view* v, char* buf, size_t cap) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    return copy_out(r->scheme, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_view_authority(
    const uvcpp_c_h3_request_view* v, char* buf, size_t cap) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    return copy_out(r->authority, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_view_path(
    const uvcpp_c_h3_request_view* v, char* buf, size_t cap) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    return copy_out(r->path, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int64_t uvcpp_c_h3_request_view_stream_id(
    const uvcpp_c_h3_request_view* v) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    return r->stream_id;
  UVCPP_C_CATCH(static_cast<int64_t>(UVCPP_C_E_EXCEPTION))
}

extern "C" UVCPP_C_API size_t uvcpp_c_h3_request_view_body_size(
    const uvcpp_c_h3_request_view* v) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return 0;
    return r->body.size();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_view_body(
    const uvcpp_c_h3_request_view* v, void* buf, size_t cap) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    return copy_out(r->body, static_cast<char*>(buf), cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_view_header(
    const uvcpp_c_h3_request_view* v, const char* name, char* buf, size_t cap) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    const uvcpp::h3_header* f = find_header(r->headers, name);
    if (f == nullptr) return UVCPP_C_E_NOT_FOUND;
    return copy_out(f->value, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h3_request_view_has_header(
    const uvcpp_c_h3_request_view* v, const char* name) {
  UVCPP_C_TRY
    const uvcpp::h3_request* r = view_req(v);
    if (r == nullptr) return UVCPP_C_E_STALE;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    return find_header(r->headers, name) != nullptr ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}
