/**
 * @file src/capi/uvcpp_c_web.cpp
 * @brief `uvcpp_c_web.h` 的实现：HTTP 客户端与 WebSocket 客户端。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件里三处值得先读
 * ----------------------
 * 1. **回调是"值捕获 + 不许删"两件事一起做的。**
 *    每一处注册给 C++ 的回调都按值捕获 `cb` / `user_data`（**绝不捕获 C 句柄
 *    的地址**：那是个栈上会失效的东西），同时捕获**一枚 C 句柄指针**用来把
 *    `cb_depth` 加一 —— 于是"在回调里 free 自己"会被 `free()` 拒掉，而不是
 *    把正在执行的那个对象的回调表连根删掉。
 *
 *    为什么捕获句柄指针是安全的：那些闭包**存在 C++ 对象里面**，而 C++ 对象
 *    由 C 句柄持有。句柄被删 ⇒ C++ 对象被删 ⇒ 闭包随之消失 ⇒ 不会有人在
 *    free 之后再调它。所以"回调跑起来时句柄一定还活着"这件事由闭包自己的
 *    存在性保证，不需要额外簿记。
 *
 * 2. **`_is_connected()` 不能直接问 `has_status(CONNECTED)`。**
 *    那是按位或，置上就不会清（`uvcpp_c_net.cpp` 里那段长注释讲的是同一个
 *    坑）。所以这里问**底下的 TCP 客户端**：连上了、不在关、也没关。
 *
 * 3. **响应句柄是回调期句柄**（与 webapp 那三枚同一类）：进回调登记、出回调
 *    毒化，且用了 RAII —— 于是它在任何一条返回路径上都被摘掉。
 */

#include "capi/uvcpp_c_web.h"

#include <cstddef>
#include <functional>
#include <new>
#include <string>

#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_connection.h>

#include "capi/uvcpp_c_internal.h"

using uvcpp_c_detail::alive;
using uvcpp_c_detail::arm_loop_thread;
using uvcpp_c_detail::copy_out;
using uvcpp_c_detail::field_present;
using uvcpp_c_detail::register_head;
using uvcpp_c_detail::table_size_ok;
using uvcpp_c_detail::thread_ok;
using uvcpp_c_detail::unregister_head;

// ---------------------------------------------------------------------------
// 魔数
// ---------------------------------------------------------------------------

#define UVCPP_C_MAGIC_HTTP_CLIENT UVCPP_C_MAGIC('e', 'h', 'p', '1')
#define UVCPP_C_MAGIC_HTTP_RESP   UVCPP_C_MAGIC('e', 'h', 'r', '1')
#define UVCPP_C_MAGIC_WS_CLIENT   UVCPP_C_MAGIC('e', 'w', 's', '1')

namespace uv = uvcpp;

// ---------------------------------------------------------------------------
// 句柄的真身（**全局作用域**：头里的 `typedef struct uvcpp_c_xxx uvcpp_c_xxx;`
// 已经把标签声明在全局，写进匿名命名空间就成了另一个类型）
// ---------------------------------------------------------------------------

struct uvcpp_c_http_client {
  uvcpp_c_detail::handle_head head;  ///< 必须在偏移 0

  uv::uvcpp_http_client* cli;
  int                    cb_depth;  ///< 正在跑的回调层数（> 0 时不许 free）
};

struct uvcpp_c_http_response {
  uvcpp_c_detail::handle_head head;

  const uv::uvcpp_http_response* resp;
  uvcpp_c_http_client*           cli;  ///< 记着"这条响应属于谁"（诊断用）
};

struct uvcpp_c_ws_client {
  uvcpp_c_detail::handle_head head;

  uv::uvcpp_ws_client* cli;
  int                  cb_depth;

  /// 事件表的一份**拷贝** + 给它的用户数据。存下来是为了"`connect()` 之前装、
  /// 之后也能装"这条（与 C++ 侧"回调存在客户端上"同一条）。
  uvcpp_c_ws_client_events ev;
  void*                   user_data;
};

static_assert(offsetof(uvcpp_c_http_client, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_http_response, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_ws_client, head) == 0, "handle_head 必须在偏移 0");

namespace {

/// @brief 回调期间挂上的"不许 free"计数。
class CallbackScope {
 public:
  explicit CallbackScope(uvcpp_c_http_client* c) : http_(c), ws_(nullptr) {
    if (http_ != nullptr) ++http_->cb_depth;
  }
  explicit CallbackScope(uvcpp_c_ws_client* c) : http_(nullptr), ws_(c) {
    if (ws_ != nullptr) ++ws_->cb_depth;
  }
  ~CallbackScope() {
    if (http_ != nullptr) --http_->cb_depth;
    if (ws_ != nullptr) --ws_->cb_depth;
  }
  CallbackScope(const CallbackScope&) = delete;
  CallbackScope& operator=(const CallbackScope&) = delete;

 private:
  uvcpp_c_http_client* http_;
  uvcpp_c_ws_client*   ws_;
};

/**
 * @brief 回调期句柄的登记/毒化守卫（本文件里就 `uvcpp_c_http_response` 一枚）。
 *
 * 用 RAII 而不是"回调后面补一句 unregister"：后者在回调提前 return 或异常时
 * 会把一枚栈地址留在活句柄表里，而那块栈下一轮就被复用了。
 */
class RespScope {
 public:
  explicit RespScope(uvcpp_c_http_response* h) : h_(h) {
    register_head(h_, UVCPP_C_MAGIC_HTTP_RESP);
    arm_loop_thread(h_);
  }
  ~RespScope() { unregister_head(h_); }
  RespScope(const RespScope&) = delete;
  RespScope& operator=(const RespScope&) = delete;

 private:
  uvcpp_c_http_response* h_;
};

}  // namespace

// ===========================================================================
// HTTP 客户端
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_http_client* uvcpp_c_http_client_new(void) {
  UVCPP_C_TRY
    uvcpp_c_http_client* h = new (std::nothrow) uvcpp_c_http_client();
    if (h == nullptr) return nullptr;
    h->cli = new (std::nothrow) uv::uvcpp_http_client();
    if (h->cli == nullptr) {
      delete h;
      return nullptr;
    }
    h->cb_depth = 0;
    register_head(h, UVCPP_C_MAGIC_HTTP_CLIENT);
    return h;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_free(uvcpp_c_http_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cb_depth > 0) return UVCPP_C_E_STATE;  // 回调里不许自毁
    if (client->cli == nullptr) return UVCPP_C_E_STALE;

    unregister_head(client);
    uv::uvcpp_http_client* cli = client->cli;
    client->cli = nullptr;
    delete cli;  // 连带它自带的那条循环
    delete client;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_set_keep_alive(
    uvcpp_c_http_client* client, int enable) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    client->cli->set_keep_alive(enable != 0);
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_connect(
    uvcpp_c_http_client* client, const char* host, int port,
    uvcpp_c_web_status_cb cb, void* user_data) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    if (host == nullptr || cb == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (port <= 0 || port > 65535) return UVCPP_C_E_INVALID_ARG;

    uvcpp_c_http_client* self = client;
    return client->cli->connect(host, port,
                                [self, cb, user_data](int status) {
                                  CallbackScope guard(self);
                                  cb(user_data, status);
                                });
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

namespace {

/**
 * @brief `get()` / `post()` 共用的那层包装：把 C++ 的 `(resp, err)` 回调摊成
 *        "成功给句柄、失败给 NULL"。
 *
 * 写成一份而不是两处各写一遍：这里那段"err != 0 就不递句柄"的判断是**语义**
 * 而不是格式 —— 抄成两份就会在某一处漂成"失败也递一个半空的响应"。
 */
template <typename SendFn>
int send_with_cb(uvcpp_c_http_client* client, SendFn&& send,
                 uvcpp_c_http_response_cb cb, void* user_data) {
  uvcpp_c_http_client* self = client;
  return send([self, cb, user_data](const uv::uvcpp_http_response& r, int err) {
    CallbackScope guard(self);
    if (err != 0) {
      cb(user_data, nullptr, err);
      return;
    }
    uvcpp_c_http_response h;
    h.resp = &r;
    h.cli  = self;
    RespScope scope(&h);
    cb(user_data, &h, 0);
  });
}

}  // namespace

extern "C" UVCPP_C_API int uvcpp_c_http_client_get(
    uvcpp_c_http_client* client, const char* path, uvcpp_c_http_response_cb cb,
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    if (path == nullptr || cb == nullptr) return UVCPP_C_E_INVALID_ARG;

    const std::string p(path);
    return send_with_cb(
        client,
        [&](std::function<void(const uv::uvcpp_http_response&, int)> f) {
          return client->cli->get(p, std::move(f));
        },
        cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_post(
    uvcpp_c_http_client* client, const char* path, const void* body, size_t len,
    const char* content_type, uvcpp_c_http_response_cb cb, void* user_data) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    if (path == nullptr || cb == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (body == nullptr && len > 0) return UVCPP_C_E_INVALID_ARG;

    // 内容立刻被 `uvcpp_http_request::make_post()` 拷走（规矩 4 第一类），
    // 所以这两个串只在本函数里活着就够。
    const std::string p(path);
    const std::string ct(content_type != nullptr
                             ? content_type
                             : "application/octet-stream");
    const std::string body_copy(body != nullptr && len > 0
                                    ? std::string(static_cast<const char*>(body), len)
                                    : std::string());
    return send_with_cb(
        client,
        [&](std::function<void(const uv::uvcpp_http_response&, int)> f) {
          return client->cli->post(p, body_copy.data(), body_copy.size(), ct,
                                   std::move(f));
        },
        cb, user_data);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_run(uvcpp_c_http_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    arm_loop_thread(client);  // 从这里起，"循环线程"就是当前这条
    return client->cli->run(UV_RUN_DEFAULT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_stop(uvcpp_c_http_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    client->cli->stop();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_close(
    uvcpp_c_http_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    uv::uvcpp_tcp_client* tcp = client->cli->get_tcp_client();
    if (tcp != nullptr) tcp->close();  // 没连过就是无操作
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_is_connected(
    uvcpp_c_http_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    // 理由见文件开头第 2 条：`uvcpp_http_client` 自己的 `has_status` 是按位或，
    // `CONNECTED` 一旦置上就再也不会清（它连 `CLOSED` 都不置，见那个枚举的
    // 注释）。所以要问底下的 TCP 客户端。
    uv::uvcpp_tcp_client* tcp = client->cli->get_tcp_client();
    if (tcp == nullptr) return 0;
    const bool up = tcp->has_status(uv::TCP_CLIENT_CONNECTED) &&
                    !tcp->has_status(uv::TCP_CLIENT_CLOSING) &&
                    !tcp->has_status(uv::TCP_CLIENT_CLOSED);
    return up ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_client_last_error(
    uvcpp_c_http_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_HTTP_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    return client->cli->get_last_error();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// HTTP 响应（回调期）
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_http_response_status_code(
    uvcpp_c_http_response* resp) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_HTTP_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    return static_cast<int>(resp->resp->status_code);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_response_status_message(
    uvcpp_c_http_response* resp, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_HTTP_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(resp->resp->status_message, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_response_header(
    uvcpp_c_http_response* resp, const char* name, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_HTTP_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!resp->resp->has_header(name)) return UVCPP_C_E_NOT_FOUND;
    return copy_out(resp->resp->get_header(name), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_response_has_header(
    uvcpp_c_http_response* resp, const char* name) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_HTTP_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (name == nullptr) return UVCPP_C_E_INVALID_ARG;
    return resp->resp->has_header(name) ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_response_content_type(
    uvcpp_c_http_response* resp, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_HTTP_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    return copy_out(resp->resp->content_type(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_http_response_body(
    uvcpp_c_http_response* resp, const char** data, size_t* len) {
  UVCPP_C_TRY
    if (!alive(resp, UVCPP_C_MAGIC_HTTP_RESP) || resp->resp == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(resp)) return UVCPP_C_E_WRONG_THREAD;
    if (data == nullptr || len == nullptr) return UVCPP_C_E_INVALID_ARG;
    *data = resp->resp->body.get_const_data();
    *len  = resp->resp->body.size();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// WebSocket 客户端
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_ws_client* uvcpp_c_ws_client_new(void) {
  UVCPP_C_TRY
    uvcpp_c_ws_client* h = new (std::nothrow) uvcpp_c_ws_client();
    if (h == nullptr) return nullptr;
    h->cli = new (std::nothrow) uv::uvcpp_ws_client();
    if (h->cli == nullptr) {
      delete h;
      return nullptr;
    }
    h->cb_depth  = 0;
    h->user_data = nullptr;
    h->ev        = uvcpp_c_ws_client_events();
    h->ev.size   = static_cast<uint32_t>(sizeof(uvcpp_c_ws_client_events));
    register_head(h, UVCPP_C_MAGIC_WS_CLIENT);
    return h;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_free(uvcpp_c_ws_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT)) return UVCPP_C_E_STALE;
    if (client->cb_depth > 0) return UVCPP_C_E_STATE;
    if (client->cli == nullptr) return UVCPP_C_E_STALE;

    unregister_head(client);
    uv::uvcpp_ws_client* cli = client->cli;
    client->cli = nullptr;
    delete cli;
    delete client;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_set_events(
    uvcpp_c_ws_client* client, const uvcpp_c_ws_client_events* table,
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;

    // 先全部清零（`table == NULL` 就是"把这一组全摘掉"），再按 `size` 逐格搬。
    client->ev        = uvcpp_c_ws_client_events();
    client->ev.size   = static_cast<uint32_t>(sizeof(uvcpp_c_ws_client_events));
    client->user_data = user_data;

    if (table != nullptr) {
      if (!table_size_ok(table->size)) {
        uvcpp_c_detail::set_last_error(
            "uvcpp_c_ws_client_events.size does not even cover the size field");
        return UVCPP_C_E_INVALID_ARG;
      }
      if (field_present(table->size, &uvcpp_c_ws_client_events::on_text))
        client->ev.on_text = table->on_text;
      if (field_present(table->size, &uvcpp_c_ws_client_events::on_binary))
        client->ev.on_binary = table->on_binary;
      if (field_present(table->size, &uvcpp_c_ws_client_events::on_close))
        client->ev.on_close = table->on_close;
      if (field_present(table->size, &uvcpp_c_ws_client_events::on_error))
        client->ev.on_error = table->on_error;
    }

    // 装到 C++ 客户端上。**按值捕获 cb 与 user_data**（它们进的是会活到下一次
    // 连接的闭包）；句柄指针只用来做"回调里不许 free"的计数。
    uvcpp_c_ws_client* self = client;

    const uvcpp_c_ws_client_events ev = client->ev;
    const void* const ud = client->user_data;

    client->cli->on_text([self, ev, ud](const std::string& s) {
      if (ev.on_text == nullptr) return;
      CallbackScope guard(self);
      ev.on_text(const_cast<void*>(ud), s.data(), s.size());
    });
    client->cli->on_binary([self, ev, ud](const uint8_t* d, size_t n) {
      if (ev.on_binary == nullptr) return;
      CallbackScope guard(self);
      ev.on_binary(const_cast<void*>(ud), reinterpret_cast<const char*>(d), n);
    });
    client->cli->on_close([self, ev, ud](uv::ws_close_code code,
                                         const std::string& reason) {
      if (ev.on_close == nullptr) return;
      CallbackScope guard(self);
      ev.on_close(const_cast<void*>(ud), static_cast<int>(code), reason.data(),
                  reason.size());
    });
    client->cli->on_error([self, ev, ud](int status, const std::string& what) {
      if (ev.on_error == nullptr) return;
      CallbackScope guard(self);
      ev.on_error(const_cast<void*>(ud), status, what.data(), what.size());
    });
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_connect(
    uvcpp_c_ws_client* client, const char* url, uvcpp_c_web_status_cb cb,
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    if (url == nullptr || cb == nullptr) return UVCPP_C_E_INVALID_ARG;

    const std::string u(url);
    if (u.compare(0, 5, "ws://") != 0) {
      uvcpp_c_detail::set_last_error(
          "uvcpp_c_ws_client_connect(): URL must start with ws://");
      return UVCPP_C_E_INVALID_ARG;  // wss:// 要 SSL 上下文，本批不给
    }

    uvcpp_c_ws_client* self = client;
    // 回调里的连接指针**不往 C 面递**（见头文件），只把 error 交出去。
    return client->cli->connect(
        u, [self, cb, user_data](uv::uvcpp_ws_connection* /*conn*/, int error) {
          CallbackScope guard(self);
          cb(user_data, error);
        });
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_send_text(
    uvcpp_c_ws_client* client, const char* data, size_t len) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    if (data == nullptr && len > 0) return UVCPP_C_E_INVALID_ARG;
    // 没有会话时 C++ 侧给 `UV_ENOTCONN`（不静默丢），原样透传。
    return client->cli->send_text(data != nullptr ? data : "", len, nullptr);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_send_binary(
    uvcpp_c_ws_client* client, const char* data, size_t len) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    if (data == nullptr && len > 0) return UVCPP_C_E_INVALID_ARG;
    return client->cli->send_binary(data != nullptr ? data : "", len, nullptr);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_close(uvcpp_c_ws_client* client,
                                                   int code,
                                                   const char* reason) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    if (!thread_ok(client)) return UVCPP_C_E_WRONG_THREAD;
    client->cli->close(static_cast<uv::ws_close_code>(code),
                       reason != nullptr ? std::string(reason) : std::string());
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_run(uvcpp_c_ws_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    arm_loop_thread(client);
    return client->cli->run(UV_RUN_DEFAULT);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_stop(uvcpp_c_ws_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    client->cli->stop();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_session_count(
    uvcpp_c_ws_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(client->cli->session_count());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_is_open(uvcpp_c_ws_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    return client->cli->has_status(uv::WS_CLIENT_OPEN) ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_ws_client_last_error(
    uvcpp_c_ws_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_WS_CLIENT) || client->cli == nullptr)
      return UVCPP_C_E_STALE;
    return client->cli->get_last_error();
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}
