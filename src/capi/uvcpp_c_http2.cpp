/**
 * @file src/capi/uvcpp_c_http2.cpp
 * @brief `uvcpp_c_http2.h` 的实现：一层薄适配，所有承重的规矩都在
 *        `uvcpp_c_internal.h` 里。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一份只做三件事
 * ----------------
 * 1. **把 C 的回调表翻成 `uvcpp_h2_session::callbacks`**：每条回调外面包一层
 *    trampoline，回调期句柄（`uvcpp_c_h2_stream`）在 trampoline 的**栈上**造、
 *    登记、出栈时由 `FrameScope` 摘表（构造器在回调里抛异常那条路径也漏不掉）。
 * 2. **把两个构造器（请求 / 响应）在提交的那一刻拷成 C++ 对象**（规矩 4 第一类）。
 * 3. **所有导出函数都包在 `UVCPP_C_TRY` / `UVCPP_C_CATCH` 里**（规矩 2）。

 * 为什么**不**在这里 include `uvcpp_c_net.h` 的实现细节
 * -----------------------------------------------------
 * `uvcpp_c_tcp_client` 的不透明类型真身在 `uvcpp_c_net.cpp` 的匿名命名空间里，
 * 本文件拿不到。所以"从 C 句柄里解出 `uvcpp_tcp_client*`"这一步由 net 那一侧
 * 提供一个口子（`uvcpp_c_detail::tcp_client_unwrap()`，声明在
 * `uvcpp_c_internal.h`）—— 而不是把句柄布局抄成第二份。抄一份的话，将来给
 * TCP 句柄加一个字段就会在这里静默读错偏移。
 */

#include "capi/uvcpp_c_http2.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_session.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#include "capi/uvcpp_c_internal.h"

// ---------------------------------------------------------------------------
// 句柄的真身
// ---------------------------------------------------------------------------
//
// 四份都定义在**全局作用域**：头里那句 `typedef struct uvcpp_c_h2_connection
// uvcpp_c_h2_connection;` 声明的是 `::uvcpp_c_h2_connection` 这个不完整类型，
// 而下面这四份就是**把它定义完整**的地方。放进匿名命名空间会造出四个**不同的**
// 类型（`{anon}::uvcpp_c_h2_connection`），那时导出函数的签名与头里那份对不上，
// 表现是一屏 "redeclared as different kind of entity"。
// 与 `uvcpp_c_net.cpp` / `uvcpp_c_webapp.cpp` 是同一个形状。

/**
 * @brief h2 驱动层句柄。
 *
 * 与别的句柄同一个形状：`handle_head` 必须在偏移 0（`alive()` 按它读），
 * 其余都是标量 / 指针 / 两张**按值存着**的回调表。
 *
 * 回调表**按值存**而不是存调用方那个指针：C 侧常见写法是
 *
 *     uvcpp_c_h2_callbacks cb = {0}; cb.size = sizeof(cb); cb.on_body = f;
 *     uvcpp_c_h2_conn_start(h, &cb, NULL, ctx);   // cb 是栈上的
 *
 * —— 存指针的话，`start()` 一返回那张表就悬垂了。按值存多出的那点字节换来的是
 * "回调表可以是个临时量"。
 */
struct uvcpp_c_h2_connection {
  uvcpp_c_detail::handle_head head;

  uvcpp::uvcpp_h2_connection* conn;  ///< 本层 `new` 的，`_free()` 里 delete
  void* user_data;                   ///< 原样回传给每一条回调的第一个参数

  uvcpp_c_h2_callbacks h2c;      ///< 只装"用户真的给了"的那几格
  uvcpp_c_h2_conn_callbacks cc;  ///< 同上
};

/** @brief 回调期句柄：一条流。指向 C++ 那侧 `uvcpp_h2_stream`，**回调返回即失效**。 */
struct uvcpp_c_h2_stream {
  uvcpp_c_detail::handle_head head;
  uvcpp::uvcpp_h2_stream* st;
};

/** @brief 请求构造器。`req` 按值存着 —— 每个 `_set_*` 改的都是它。 */
struct uvcpp_c_h2_request {
  uvcpp_c_detail::handle_head head;
  uvcpp::uvcpp_http_request req;
};

/** @brief 响应构造器。 */
struct uvcpp_c_h2_response {
  uvcpp_c_detail::handle_head head;
  uvcpp::uvcpp_http_response resp;
};

// `alive()` 会把这四个句柄当作 `handle_head*` 读第一格 —— 偏移 0 这件事必须
// 钉住，将来在前面加一个字段就是一次静默的内存踩踏。`uvcpp_c_net.cpp` 里
// 那两个句柄有同样的两行。
static_assert(offsetof(uvcpp_c_h2_connection, head) == 0,
              "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_h2_stream, head) == 0,
              "handle_head 必须在偏移 0");

// 请求 / 响应那两个**不写 `offsetof` 断言**，而这个不对称是量出来的：它们的
// 第二个成员是 `uvcpp_http_request` / `uvcpp_http_response`（带 `std::string`），
// 于是这两个类型**不是** standard-layout，而对非 standard-layout 类型用
// `offsetof` 只是"条件支持"（gcc 会给 `-Winvalid-offsetof`：在打字的这一刻
// 它就在编译日志里）。为了钉住一件**语言本来就保证**的事（首成员偏移恒为 0）
// 换取一条每次编译都出现的警告，是把判据换成噪音。
// 这两个类型仍然受同样的约束，只是判据落在别处：下面是 `init_head()` 里那句
// `magic` 的写入，读它的 `alive()` 走的是同一段指针运算。

namespace {

using uvcpp_c_detail::alive;
using uvcpp_c_detail::copy_out;

// ---------------------------------------------------------------------------
// 回调期句柄的登记范围
// ---------------------------------------------------------------------------

/**
 * @brief "进回调登记、出回调摘表"的一份作用域。
 *
 * 与 `uvcpp_c_webapp.cpp` 里那个 `FrameScope` 是同一个东西，只是这里最多只有
 * 一个句柄（一条回调只递一条流）。**用作用域而不是手写 unregister**：用户回调
 * 抛异常（C 回调不会，但 C++ 侧的同名路径会）时栈展开照样会走到析构，手写的
 * 那一句就漏了。
 *
 * ★ 有一件**量出来的**事必须写在这里：句柄住在 trampoline 的栈上，所以回调一
 *   返回、那块栈就被后续循环代码复用，**魔数会被无关写入盖掉**。也就是说
 *   "读魔数"这个判据区分不出"被毒化"和"被栈复用盖掉" —— 出了回调再用它，
 *   本层的登记表**已经摘干净了**，所以拿到的一定是 `UVCPP_C_E_STALE`（先查表，
 *   表里没有再读魔数，见 `uvcpp_c_internal.h`），但**别拿"魔数没了"当这条
 *   承诺的机制证据**（`tests/tools/capi_mutation.py` 的 M8 就是这条的代价，
 *   记在 `doc/capi-guide.md` §6）。
 */
class FrameScope {
 public:
  FrameScope() : n_(0) {}
  ~FrameScope() {
    for (int i = n_ - 1; i >= 0; --i) {
      uvcpp_c_detail::unregister_head(slots_[i]);
    }
  }

  template <typename H>
  H* add(H* h, uint32_t magic) {
    uvcpp_c_detail::register_head(h, magic);
    uvcpp_c_detail::arm_loop_thread(h);
    if (n_ < 1) slots_[n_++] = reinterpret_cast<uvcpp_c_detail::handle_head*>(h);
    return h;
  }

 private:
  uvcpp_c_detail::handle_head* slots_[1];
  int n_;
};

// ---------------------------------------------------------------------------
// trampoline
// ---------------------------------------------------------------------------
//
// 每条都长一个样：造一个栈上的 `uvcpp_c_h2_stream` → 包进 FrameScope →
// 用户没给这条回调就直接返回（**一个字节都不碰**，这是"表里没覆盖的当不关心"
// 的落点）→ 否则调它。
//
// `c->h2c.xxx` 每次都**现读**（不是把函数指针拷进 lambda 的自由变量）：这样
// 再 `_start()` 一次、或者换一张表，装的还是同一个 lambda，读到的却是新的那一格。

void call_on_request(uvcpp_c_h2_connection* c, uvcpp::uvcpp_h2_stream& s,
                     bool end_stream) {
  if (c->h2c.on_request == nullptr) return;
  uvcpp_c_h2_stream cs;
  FrameScope scope;
  scope.add(&cs, UVCPP_C_MAGIC_H2_STREAM);
  cs.st = &s;
  c->h2c.on_request(c->user_data, c, &cs, end_stream ? 1 : 0);
}

void call_on_request_end(uvcpp_c_h2_connection* c, uvcpp::uvcpp_h2_stream& s) {
  if (c->h2c.on_request_end == nullptr) return;
  uvcpp_c_h2_stream cs;
  FrameScope scope;
  scope.add(&cs, UVCPP_C_MAGIC_H2_STREAM);
  cs.st = &s;
  c->h2c.on_request_end(c->user_data, c, &cs);
}

void call_on_response(uvcpp_c_h2_connection* c, uvcpp::uvcpp_h2_stream& s,
                      bool end_stream) {
  if (c->h2c.on_response == nullptr) return;
  uvcpp_c_h2_stream cs;
  FrameScope scope;
  scope.add(&cs, UVCPP_C_MAGIC_H2_STREAM);
  cs.st = &s;
  c->h2c.on_response(c->user_data, c, &cs, end_stream ? 1 : 0);
}

void call_on_response_end(uvcpp_c_h2_connection* c, uvcpp::uvcpp_h2_stream& s) {
  if (c->h2c.on_response_end == nullptr) return;
  uvcpp_c_h2_stream cs;
  FrameScope scope;
  scope.add(&cs, UVCPP_C_MAGIC_H2_STREAM);
  cs.st = &s;
  c->h2c.on_response_end(c->user_data, c, &cs);
}

void call_on_body(uvcpp_c_h2_connection* c, uvcpp::uvcpp_h2_stream& s,
                  const char* data, size_t len) {
  if (c->h2c.on_body == nullptr) return;
  uvcpp_c_h2_stream cs;
  FrameScope scope;
  scope.add(&cs, UVCPP_C_MAGIC_H2_STREAM);
  cs.st = &s;
  // `data` 原样透传：它的生存期就是这次回调（规矩 4 第二类），本层不拷。
  c->h2c.on_body(c->user_data, c, &cs, data, len);
}

void call_on_close(uvcpp_c_h2_connection* c, int32_t stream_id,
                   uint32_t error_code) {
  if (c->h2c.on_close == nullptr) return;
  // 这里**不造流句柄**：C++ 侧的契约是"回到这里之后 `uvcpp_h2_stream&` 立即
  // 失效"，所以递的是流号。造一个已经在失效边缘的句柄只会给出一条写得出来、
  // 但读什么都不可靠的路径。
  c->h2c.on_close(c->user_data, c, stream_id, error_code);
}

void call_on_fatal(uvcpp_c_h2_connection* c, int nghttp2_error) {
  if (c->h2c.on_fatal == nullptr) return;
  c->h2c.on_fatal(c->user_data, c, nghttp2_error);
}

void call_on_disconnect(uvcpp_c_h2_connection* c) {
  if (c->cc.on_disconnect == nullptr) return;
  c->cc.on_disconnect(c->user_data, c);
}

// ---------------------------------------------------------------------------
// 取句柄
// ---------------------------------------------------------------------------

/// 活着吗。三个都过了才给用。
inline bool ok(const uvcpp_c_h2_connection* c) {
  return alive(c, UVCPP_C_MAGIC_H2_CONN) && c->conn != nullptr;
}

inline uvcpp_c_h2_stream* stream_of(const void* h) {
  return const_cast<uvcpp_c_h2_stream*>(
      static_cast<const uvcpp_c_h2_stream*>(h));
}

}  // namespace

// ===========================================================================
// 生命周期
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_h2_connection* uvcpp_c_h2_connection_new(
    uvcpp_c_tcp_client* client, int server_side) {
  UVCPP_C_TRY
    // 先解出底下的 C++ 客户端：解不出来就没有"在谁身上建"这回事，
    // 也就没必要去 `new` 一个马上要删的对象。
    uvcpp::uvcpp_tcp_client* cli = uvcpp_c_detail::tcp_client_unwrap(client);
    if (cli == nullptr) return nullptr;

    // 顺序与 net 那一侧逐字相同：**先造可能抛的 C++ 对象，再造包装器**。
    // 反过来的话构造函数抛出去时包装器已经分配了，而唯一的句柄在栈上那个
    // 正在展开的帧里，没人能释放它。
    std::unique_ptr<uvcpp::uvcpp_h2_connection> conn(
        new uvcpp::uvcpp_h2_connection(cli, server_side != 0));

    auto* raw = new (std::nothrow) uvcpp_c_h2_connection();
    if (raw == nullptr) return nullptr;
    std::unique_ptr<uvcpp_c_h2_connection> c(raw);

    c->conn = conn.get();
    c->user_data = nullptr;
    std::memset(&c->h2c, 0, sizeof(c->h2c));
    std::memset(&c->cc, 0, sizeof(c->cc));

    uvcpp_c_detail::register_head(c.get(), UVCPP_C_MAGIC_H2_CONN);
    conn.release();  // 所有权交给包装器（`_free()` 里 delete）
    return c.release();
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_free(uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (c == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(c, UVCPP_C_MAGIC_H2_CONN)) return UVCPP_C_E_STALE;

    // 先摘表再删：反过来的话，删除过程中（C++ 析构里可能有回调）本句柄还在
    // 表里，一次重入就会拿着一块正在被释放的内存进来。
    uvcpp_c_detail::unregister_head(c);
    uvcpp::uvcpp_h2_connection* conn = c->conn;
    c->conn = nullptr;

    // ★ 这一句不是收拾现场，是**契约的一部分**：`_send_data()` 给出去的每个
    //   `done` 都承诺"恰好跑一次"，而库里那份还没上线的块存在会话里 ——
    //   `~uvcpp_h2_connection()` **不会**把它们跑掉（它只作废存活令牌、删写缓冲）。
    //   不在这里取出来跑，那些 `DoneSlot` 就是一次内存泄漏，而且调用方等的那
    //   个回调永远不来（C# 侧那条 `TaskCompletionSource` 就再也完不成）。
    //
    //   顺序承重：**先删干净、再跑**。跑的时候句柄已经被摘表、对象已经析构，
    //   所以回调里若拿旧句柄回来用，拿到的是 `UVCPP_C_E_STALE`（而不是一块
    //   正在被释放的内存）。C++ 侧 `take_cancelled_dones()` 的文档说的正是
    //   这个次序（"调用方在自己的上下文拆干净之后再逐个跑"）。
    std::vector<std::function<void()>> cancelled;
    if (conn != nullptr) conn->take_cancelled_dones(cancelled);

    delete conn;
    delete c;

    // 一律带 `UV_ECANCELED`（连接没了，这些块这辈子发不出去）—— 码是
    // `take_cancelled_dones()` 绑好的，这里只是把控制权交还给调用方。
    for (std::size_t i = 0; i < cancelled.size(); ++i) cancelled[i]();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_start(
    uvcpp_c_h2_connection* c, const uvcpp_c_h2_callbacks* h2_cbs,
    const uvcpp_c_h2_conn_callbacks* conn_cbs, void* user_data) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;

    // 表的大小先查，再逐字段问"你这一格覆盖到了吗"。`size` 连自己都没说清
    // （< sizeof(uint32_t)）就是一次误用 —— 那时**整张表都不读**，而不是
    // "当作空表继续"，理由见 `uvcpp_c_internal.h::table_size_ok`。
    if (h2_cbs != nullptr && !uvcpp_c_detail::table_size_ok(h2_cbs->size)) {
      uvcpp_c_detail::set_last_error(
          "uvcpp_c_h2_callbacks.size is smaller than sizeof(uint32_t)");
      return UVCPP_C_E_INVALID_ARG;
    }
    if (conn_cbs != nullptr && !uvcpp_c_detail::table_size_ok(conn_cbs->size)) {
      uvcpp_c_detail::set_last_error(
          "uvcpp_c_h2_conn_callbacks.size is smaller than sizeof(uint32_t)");
      return UVCPP_C_E_INVALID_ARG;
    }

    // 抄一份。**这一份是"用户给了哪几格"的真相**：没覆盖的格子留零，trampoline
    // 里那句 `== nullptr` 就是"不关心"的落点。再 `_start()` 一次会整份换掉。
    std::memset(&c->h2c, 0, sizeof(c->h2c));
    std::memset(&c->cc, 0, sizeof(c->cc));
    c->user_data = user_data;

    const uint32_t hs = h2_cbs != nullptr ? h2_cbs->size : 0;
    if (h2_cbs != nullptr) {
      if (uvcpp_c_detail::field_present(hs, &uvcpp_c_h2_callbacks::on_request))
        c->h2c.on_request = h2_cbs->on_request;
      if (uvcpp_c_detail::field_present(hs,
                                        &uvcpp_c_h2_callbacks::on_request_end))
        c->h2c.on_request_end = h2_cbs->on_request_end;
      if (uvcpp_c_detail::field_present(hs, &uvcpp_c_h2_callbacks::on_response))
        c->h2c.on_response = h2_cbs->on_response;
      if (uvcpp_c_detail::field_present(hs,
                                        &uvcpp_c_h2_callbacks::on_response_end))
        c->h2c.on_response_end = h2_cbs->on_response_end;
      if (uvcpp_c_detail::field_present(hs, &uvcpp_c_h2_callbacks::on_body))
        c->h2c.on_body = h2_cbs->on_body;
      if (uvcpp_c_detail::field_present(hs, &uvcpp_c_h2_callbacks::on_close))
        c->h2c.on_close = h2_cbs->on_close;
      if (uvcpp_c_detail::field_present(hs, &uvcpp_c_h2_callbacks::on_fatal))
        c->h2c.on_fatal = h2_cbs->on_fatal;
    }
    if (conn_cbs != nullptr) {
      const uint32_t cs = conn_cbs->size;
      if (uvcpp_c_detail::field_present(
              cs, &uvcpp_c_h2_conn_callbacks::on_disconnect))
        c->cc.on_disconnect = conn_cbs->on_disconnect;
    }

    uvcpp::uvcpp_h2_session::callbacks h2;
    // 七条**无条件**装上 trampoline，由 trampoline 自己去问"用户关心这一格吗"。
    // 不在这里按 nullptr 挑着装有个具体好处：`_start()` 之后再换一张表时，
    // 会话上装的东西不用动，判断只有一处。
    h2.on_request = [c](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& s,
                        bool end) { call_on_request(c, s, end); };
    h2.on_request_end = [c](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& s) {
      call_on_request_end(c, s);
    };
    h2.on_response = [c](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& s,
                         bool end) { call_on_response(c, s, end); };
    h2.on_response_end =
        [c](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& s) {
          call_on_response_end(c, s);
        };
    h2.on_body = [c](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& s,
                     const char* data, size_t len) {
      call_on_body(c, s, data, len);
    };
    h2.on_close = [c](uvcpp::uvcpp_h2_session&, int32_t sid, uint32_t ec) {
      call_on_close(c, sid, ec);
    };
    h2.on_fatal = [c](uvcpp::uvcpp_h2_session&, int e) { call_on_fatal(c, e); };

    uvcpp::uvcpp_h2_connection::callbacks ccb;
    ccb.on_disconnect = [c](uvcpp::uvcpp_h2_connection&) {
      call_on_disconnect(c);
    };

    // `start()` 是这条连接上"这一层正式开工"的那一刻，也是线程归属被记下来的
    // 那一刻（规矩 5）—— 在它之前没有"循环线程"可言。
    uvcpp_c_detail::arm_loop_thread(&c->head);
    return c->conn->start(h2, ccb);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 服务端：回话
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_send_status(
    uvcpp_c_h2_connection* c, int32_t stream_id, int status, const char* body,
    size_t body_len) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (body == nullptr && body_len > 0) return UVCPP_C_E_INVALID_ARG;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;

    const std::string b(body != nullptr ? body : "", body_len);
    return c->conn->send_status(stream_id, status, b);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_send_response(
    uvcpp_c_h2_connection* c, int32_t stream_id,
    const uvcpp_c_h2_response* resp, int omit_body) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (resp == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(resp, UVCPP_C_MAGIC_H2_RESPONSE)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;

    // 拷贝发生在**这里**，不在 `_new()`：所以交出去之后改它、或者直接 free 它，
    // 都不影响已经提交出去的那一份（规矩 4 第一类）。
    return c->conn->send_response(stream_id, resp->resp, omit_body != 0);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_send_headers(
    uvcpp_c_h2_connection* c, int32_t stream_id,
    const uvcpp_c_h2_response* resp) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (resp == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(resp, UVCPP_C_MAGIC_H2_RESPONSE)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;

    return c->conn->send_headers(stream_id, resp->resp);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

namespace {

/// `send_data` 的 `done` 那一格：函数指针 + 它自己的 user_data，堆上一份。
struct DoneSlot {
  void (*fn)(void*, int);
  void* ud;
};

}  // namespace

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_send_data(
    uvcpp_c_h2_connection* c, int32_t stream_id, const char* data, size_t len,
    int end_stream, void (*done)(void* user_data, int status),
    void* done_user_data) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (data == nullptr && len > 0) return UVCPP_C_E_INVALID_ARG;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;

    // `done` 跨过一次调用才回来（写完成路径），所以不能捕获栈上的东西 ——
    // 堆上一份，回调里自己删。
    DoneSlot* slot = nullptr;
    if (done != nullptr) {
      slot = new (std::nothrow) DoneSlot();
      if (slot == nullptr) return UVCPP_C_E_NO_MEMORY;
      slot->fn = done;
      slot->ud = done_user_data;
    }

    const int rc = c->conn->send_data(
        stream_id, data, len, end_stream != 0,
        [slot](int status) {
          if (slot == nullptr) return;
          slot->fn(slot->ud, status);
          delete slot;
        });

    // 没受理 ⇒ `done` **不会被调**（C++ 侧写明的契约），那份 slot 由我们自己
    // 收掉。漏了这一句就是一次内存泄漏，而且只在"队列满了"这条罕见路径上出现。
    if (rc != 0 && slot != nullptr) delete slot;
    return rc;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 客户端：发请求
// ===========================================================================

extern "C" UVCPP_C_API int32_t uvcpp_c_h2_conn_submit_request(
    uvcpp_c_h2_connection* c, const uvcpp_c_h2_request* req, const char* body,
    size_t body_len) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (req == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(req, UVCPP_C_MAGIC_H2_REQUEST)) return UVCPP_C_E_STALE;
    if (body == nullptr && body_len > 0) return UVCPP_C_E_INVALID_ARG;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;

    const int32_t sid = c->conn->session().submit_request(
        req->req, std::string(body != nullptr ? body : "", body_len));
    if (sid < 0) return sid;

    // 顺手冲一次。**这不是顺手**：C++ 侧的 `uvcpp_http_client` 走的正是这两步，
    // 而"提交了但一个字节都没发"是本层最难查的一种静默失败 —— 响应那三条
    // （`_send_status` / `_send_response` / `_send_headers`）都由连接层带了
    // `flush()`，这条没有的话就是同一层里两套规矩。
    const int frv = c->conn->flush();
    if (frv != 0) return frv;  // 流已经建了，只是字节没出去（与 C++ 侧同形）
    return sid;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 流控制与收尾
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_pause_stream(uvcpp_c_h2_connection* c,
                                                        int32_t stream_id) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    return c->conn->pause_stream(stream_id);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_resume_stream(
    uvcpp_c_h2_connection* c, int32_t stream_id) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    // 连接层的 `resume_stream()` 带 `flush()`（会话层的那个不带，理由见头文件）。
    return c->conn->resume_stream(stream_id);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_submit_rst(uvcpp_c_h2_connection* c,
                                                      int32_t stream_id,
                                                      uint32_t error_code) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    // 连接层**没有** `send_rst` 这个合并版，本层不凭空空造一个（造了就等于
    // 在 C 面多一条 C++ 侧不存在的规矩）。要发出去紧接着 `_flush()`。
    return c->conn->session().submit_rst(stream_id, error_code);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_flush(uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    return c->conn->flush();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_begin_goaway(uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    return c->conn->begin_goaway();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_submit_goaway(
    uvcpp_c_h2_connection* c, uint32_t error_code, const char* debug,
    size_t debug_len) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (debug == nullptr && debug_len > 0) return UVCPP_C_E_INVALID_ARG;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    return c->conn->session().submit_goaway(
        error_code, std::string(debug != nullptr ? debug : "", debug_len));
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_shutdown(uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    c->conn->shutdown();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_close_now(uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    if (!uvcpp_c_detail::thread_ok(&c->head)) return UVCPP_C_E_WRONG_THREAD;
    c->conn->close_now();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 只读
// ===========================================================================
//
// 这几个**不做线程检查**：它们一个字节都不改。规矩 5 管的是"能不能在这里
// 动这条连接"，不是"能不能读一个计数"。查一遍也不亏，但会让"从别的线程看一眼
// 状态好判断该不该投回去"这件很常见的事变成必须先跳一次线程 —— 而读到的那个
// 数在跨线程场景下本来就只是"某一瞬间的样子"（与 `uvcpp_c_live_handle_count()`
// 的注释同一条）。要动手的入口一个不漏地查着。

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_closed(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    return c->conn->closed() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_closing(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    return c->conn->closing() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_h2_conn_stream_count(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return 0;
    return c->conn->session().stream_count();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_last_error(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    return c->conn->session().last_error();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_h2_conn_bytes_in(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return 0;
    return c->conn->bytes_in();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API size_t uvcpp_c_h2_conn_bytes_out(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return 0;
    return c->conn->bytes_out();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_conn_peer_goaway_received(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return UVCPP_C_E_STALE;
    return c->conn->session().peer_goaway_received() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API uint32_t uvcpp_c_h2_conn_peer_goaway_error_code(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return 0;
    return c->conn->session().peer_goaway_error_code();
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int32_t uvcpp_c_h2_conn_peer_goaway_last_stream_id(
    const uvcpp_c_h2_connection* c) {
  UVCPP_C_TRY
    if (!ok(c)) return 0;
    return c->conn->session().peer_goaway_last_stream_id();
  UVCPP_C_CATCH(0)
}

// ===========================================================================
// 回调期句柄：一条流
// ===========================================================================

namespace {

/// 回调期句柄的公共取值：活着吗 + 底下那个 C++ 流还在吗。
inline bool ok_stream(const uvcpp_c_h2_stream* st) {
  return alive(st, UVCPP_C_MAGIC_H2_STREAM) && st->st != nullptr;
}

}  // namespace

extern "C" UVCPP_C_API int32_t uvcpp_c_h2_stream_id(
    const uvcpp_c_h2_stream* st) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    return st->st->stream_id;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_state(
    const uvcpp_c_h2_stream* st) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    return static_cast<int>(st->st->state);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_rejected(
    const uvcpp_c_h2_stream* st) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    return st->st->rejected ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_paused(
    const uvcpp_c_h2_stream* st) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    return st->st->paused ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_h2_stream_body_bytes(
    const uvcpp_c_h2_stream* st) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return 0;
    return st->st->body_bytes;
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_expected_body(
    const uvcpp_c_h2_stream* st, size_t* out) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    // "没给"与"给了 0"必须分得开：前者是流式体（chunked 一类），后者是一条
    // 空 body 的响应。合成一个 0 的话业务层就没法区分，而它要做的决定不同。
    if (!st->st->has_content_length) return UVCPP_C_E_NOT_FOUND;
    if (out != nullptr) *out = st->st->expected_body;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_request_method_name(
    const uvcpp_c_h2_stream* st, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    // `uvcpp_http_request` 上**没有** `method_name()`（那个在 webapp 的
    // `uvcpp_web_request` 上），所以要自己把枚举转成字符串 —— 用 `web/` 那一侧
    // 现成的 `http_method_str()`，别在这里写第三张表。
    return copy_out(uvcpp::http_method_str(st->st->request.method), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_request_path(
    const uvcpp_c_h2_stream* st, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    return copy_out(st->st->request.url, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_request_header(
    const uvcpp_c_h2_stream* st, const char* name, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    // 顺序承重：`get_header()` 查不到时给的是**空串**，而"没带这个头"与
    // "带了但值是空的"在 HTTP 里是两件事。所以先 `has_header()` 再取值。
    if (!st->st->request.has_header(name)) return UVCPP_C_E_NOT_FOUND;
    return copy_out(st->st->request.get_header(name), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_request_has_header(
    const uvcpp_c_h2_stream* st, const char* name) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    return st->st->request.has_header(name) ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_response_status(
    const uvcpp_c_h2_stream* st) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    // `h2_response_not_received()` 把 `status_code` 填成 `HTTP_STATUS_NONE`
    // （`uvcpp::HTTP_STATUS_NONE`，`static_cast<http_status>(0)`），而不是默认
    // 构造的那个 200 —— 所以这里判的是"头到了没有"，返回的是"没有状态码"这个
    // 事实，不是一个数字。**不写成"返回 0"**：0 是 `HTTP_STATUS_NONE` 自己，
    // 用户会分不清"没到"和"到了但状态码是 0"。
    const int code = static_cast<int>(st->st->response.status_code);
    if (code == static_cast<int>(uvcpp::HTTP_STATUS_NONE)) {
      return UVCPP_C_E_NOT_FOUND;
    }
    return code;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_stream_response_header(
    const uvcpp_c_h2_stream* st, const char* name, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!ok_stream(st)) return UVCPP_C_E_STALE;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!st->st->response.has_header(name)) return UVCPP_C_E_NOT_FOUND;
    return copy_out(st->st->response.get_header(name), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 构造器：请求 / 响应
// ===========================================================================

namespace {

/**
 * @brief 构造器的公共形状：`_new` 先造可能抛的 C++ 对象、再登记包装器。
 *
 * 用模板收口是因为这四个函数（两个 `_new`、两个 `_free`）的差别**只有类型与
 * 魔数**，而"先造谁、先摘哪张表"这两处顺序是承重的 —— 抄四遍就是四份会各自
 * 漂的顺序。
 */
template <typename W>
W* make_wrapper(uint32_t magic) {
  auto* raw = new (std::nothrow) W();
  if (raw == nullptr) return nullptr;
  std::unique_ptr<W> w(raw);
  uvcpp_c_detail::register_head(w.get(), magic);
  return w.release();
}

template <typename W>
int free_wrapper(W* w, uint32_t magic) {
  if (w == nullptr) return UVCPP_C_E_INVALID_ARG;
  if (!alive(w, magic)) return UVCPP_C_E_STALE;
  uvcpp_c_detail::unregister_head(w);
  delete w;
  return UVCPP_C_OK;
}

}  // namespace

extern "C" UVCPP_C_API uvcpp_c_h2_request* uvcpp_c_h2_request_new(
    const char* method, const char* path) {
  UVCPP_C_TRY
    if (method == nullptr || path == nullptr) return nullptr;
    std::unique_ptr<uvcpp_c_h2_request> req(
        make_wrapper<uvcpp_c_h2_request>(UVCPP_C_MAGIC_H2_REQUEST));
    if (req == nullptr) return nullptr;

    // 方法名走**反查**而不是自己判："GET" → `HTTP_GET`。认不出来的方法名
    // 一律 `HTTP_GET`（`uvcpp_http_request::method` 也只有这一组枚举值可放），
    // 所以文档里写明"只支持标准方法名" —— 悄悄退化成 GET 比报错更难查。
    uvcpp::http_method m = uvcpp::http_method::HTTP_GET;
    for (int i = 0; i <= static_cast<int>(uvcpp::http_method::HTTP_PRI); ++i) {
      const char* s = uvcpp::http_method_str(static_cast<uvcpp::http_method>(i));
      if (s != nullptr && std::strcmp(s, method) == 0) {
        m = static_cast<uvcpp::http_method>(i);
        break;
      }
    }
    req->req.method = m;
    req->req.url    = path;
    return req.release();
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_request_free(uvcpp_c_h2_request* req) {
  UVCPP_C_TRY
    return free_wrapper(req, UVCPP_C_MAGIC_H2_REQUEST);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_request_set_header(
    uvcpp_c_h2_request* req, const char* name, const char* value) {
  UVCPP_C_TRY
    if (!alive(req, UVCPP_C_MAGIC_H2_REQUEST)) return UVCPP_C_E_STALE;
    if (name == nullptr || value == nullptr) return UVCPP_C_E_INVALID_ARG;
    req->req.set_header(name, value);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API uvcpp_c_h2_response* uvcpp_c_h2_response_new(int status) {
  UVCPP_C_TRY
    std::unique_ptr<uvcpp_c_h2_response> resp(
        make_wrapper<uvcpp_c_h2_response>(UVCPP_C_MAGIC_H2_RESPONSE));
    if (resp == nullptr) return nullptr;
    resp->resp.status_code = static_cast<uvcpp::http_status>(status);
    return resp.release();
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_response_free(uvcpp_c_h2_response* resp) {
  UVCPP_C_TRY
    return free_wrapper(resp, UVCPP_C_MAGIC_H2_RESPONSE);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_response_set_status(
    uvcpp_c_h2_response* resp, int status) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H2_RESPONSE)) return UVCPP_C_E_STALE;
    resp->resp.status_code = static_cast<uvcpp::http_status>(status);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_response_set_header(
    uvcpp_c_h2_response* resp, const char* name, const char* value) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H2_RESPONSE)) return UVCPP_C_E_STALE;
    if (name == nullptr || value == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp.set_header(name, value);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_response_set_content_type(
    uvcpp_c_h2_response* resp, const char* content_type) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H2_RESPONSE)) return UVCPP_C_E_STALE;
    if (content_type == nullptr) return UVCPP_C_E_INVALID_ARG;
    resp->resp.set_content_type(content_type);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_h2_response_set_body(
    uvcpp_c_h2_response* resp, const void* data, size_t len) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_H2_RESPONSE)) return UVCPP_C_E_STALE;
    if (data == nullptr && len > 0) return UVCPP_C_E_INVALID_ARG;
    // `clone_data` 是**深拷贝**（`set_data` 存的是外部视图，析构不 free 它 ——
    // 在这里用就是把自己交出去的那块内存当成自己的，调用方一释放就悬垂）。
    resp->resp.body.clone_data(static_cast<const char*>(data), len);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}
