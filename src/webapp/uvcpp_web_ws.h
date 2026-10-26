/**
 * @file src/webapp/uvcpp_web_ws.h
 * @brief WebSocket 处理器拿到的东西：一次升级请求 + 建立起来的连接。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么要有这一层
 * ----------------
 * 协议层（`uvcpp_ws_server`）的升级回调只给两样东西：`uvcpp_http_request&`（原始
 * 报文）和 `uvcpp_tcp_client*`（传输层）。业务处理器要的是**应用层**的东西 ——
 * 路径、路径参数、查询串、Cookie、对端地址，这些都在 `uvcpp_web_request` 里，
 * 而那是框架层的东西，协议层不认识。
 *
 * 所以本类是两者的接合处：内部**持有一个 `uvcpp_web_request`**（构造时
 * `take_from()` 接管原始请求），把框架层已经解析好的东西原样转发出来，另外
 * 挂上这一层独有的两样 —— 命中的**路由模式**（`route()`）和建立起来的
 * **WS 连接**（`connection()`）。
 *
 * 关于生命周期
 * ------------
 * 对象**归框架所有**，用户处理器收到的引用只在**本次调用期间**有效 ——
 * 处理器返回后它就销毁了。不要把引用存起来。
 *
 * 为什么框架不把它放在自己的栈上：升级不是同步的。`handle_upgrade()` 先写
 * 101，会话是在**那次写的完成回调**里建起来的（异步，晚于 `handle_upgrade`
 * 返回），框架要在那时才知道 `connection()` 是谁。所以对象必须活过
 * `handle_upgrade` 返回那一刻，由框架用一个随本次升级走的 `shared_ptr` 持有。
 * 对用户而言这一点不可见：拿到的仍然只是一个"活着的时候用、返回即失效"的引用。
 *
 * 底层 `uvcpp_web_request` 的拷贝构造是 `delete` 的（它持有从源请求搬来的
 * body），所以本类也不可拷贝，这条约束是自动的而不是靠约定。
 *
 * `connection()` 返回的会话指针则**归框架所有**：用户不要 `delete`，也不要
 * 在 `on_close` 之后再用它（会话已终结）。会话的所有权与回收见
 * `uvcpp_ws_sessions`。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_WS_H
#define SRC_WEBAPP_UVCPP_WEB_WS_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_frame.h>
#include <webapp/uvcpp_web_request.h>

namespace uvcpp {

class uvcpp_web_ws_session;

/**
 * @brief 一次 WebSocket 升级请求的应用层视图。
 *
 * 由框架在栈上构造，按引用传给 `uvcpp_web_ws_handler`。
 */
class UVCPP_API uvcpp_web_ws_request {
 public:
  /**
   * @brief 从一个 http 层的升级请求构造。
   *
   * **会搬走 `raw` 的 body**（`uvcpp_web_request::take_from` 的语义）。
   * 升级请求按 RFC 6455 §4.1 是 GET、正常没有 body，所以这条代价通常是零。
   *
   * `connection()` 此时还是 nullptr —— 会话要等 101 写完成才建得起来，见
   * `bind_connection()`。框架保证交给用户处理器之前已经绑好。
   *
   * @param raw 协议层的升级请求。构造后不得再读它的 body。
   */
  explicit uvcpp_web_ws_request(uvcpp_http_request& raw);
  ~uvcpp_web_ws_request();

  uvcpp_web_ws_request(const uvcpp_web_ws_request&) = delete;
  uvcpp_web_ws_request& operator=(const uvcpp_web_ws_request&) = delete;

  // -------------------------------------------------------------------
  // 这一层独有的两样
  // -------------------------------------------------------------------

  /**
   * @brief 建立起来的 WS 会话。用户在这上面装 `on_text`/`on_binary`/`on_close`。
   *
   * **归框架所有**：不要 `delete`。会话终结（对端断开、协议错误、服务器停机）
   * 后框架会自动回收它，之后再用这个指针就是悬垂访问。
   */
  uvcpp_ws_connection* connection() const { return conn_; }

  /**
   * @brief 绑定建好的会话。**由框架在 101 写完成后调用**，用户不要碰。
   *
   * 单独一步而不是构造函数参数：会话是异步建起来的，构造本对象时它还不存在。
   */
  void bind_connection(uvcpp_ws_connection* conn) { conn_ = conn; }

  /**
   * @brief 传输无关的会话视图（extended CONNECT：h2/h3，#58）。
   *
   * **h1 的 `Upgrade` 那条路恒为 `nullptr`** —— 那种会话用 @ref connection()。
   * 要写一份对三种传输都成立的处理器，就用 `session()`；它的发送/回调接口与
   * `uvcpp_ws_connection` 同名同义。
   *
   * 生命周期与 `connection()` 一样：**归框架所有**，`on_close` 之后不要再用。
   */
  uvcpp_web_ws_session* session() const { return sess_; }

  /** @brief 由框架绑定。用户不要碰。 */
  void bind_session(uvcpp_web_ws_session* s) { sess_ = s; }

  /** @brief 命中的路由模式（如 `/chat/:room`）。未设置时为空串。 */
  const std::string& route() const { return route_; }

  /** @brief 由框架的分派器填入命中的模式。 */
  void set_route(const std::string& pattern) { route_ = pattern; }

  /**
   * @brief 由框架填入对端地址。
   *
   * 框架从连接登记表里取（与 HTTP 请求同一条路径），所以 WS 处理器看到的
   * `peer_ip()` 与同一连接上 HTTP 请求看到的**是同一个来源**。
   */
  void set_peer(const std::string& ip, unsigned int port) {
    req_.set_peer(ip, port);
  }

  // -------------------------------------------------------------------
  // 转发给内层的 uvcpp_web_request
  // -------------------------------------------------------------------

  /** @brief 内层请求对象（逃生口：上面没转发的东西都从这里取）。 */
  uvcpp_web_request& request() { return req_; }
  const uvcpp_web_request& request() const { return req_; }

  /** @brief 解码并归一化后的路径。查询串已剥离。 */
  const std::string& path() const { return req_.path(); }

  /** @brief 取一个路径参数（如 `/chat/:room` 里的 `room`）。不存在返回 nullptr。 */
  const std::string* param(const std::string& name) const {
    return req_.param(name);
  }

  /** @brief 全部路径参数，保序。 */
  const std::vector<std::pair<std::string, std::string> >& params() const {
    return req_.params();
  }

  /** @brief 取一个查询参数。不存在返回 nullptr（注意：可能是空串值）。 */
  const std::string* query(const std::string& name) const {
    return req_.query(name);
  }

  /** @brief 取一个 Cookie。不存在返回 nullptr。 */
  const std::string* cookie(const std::string& name) const {
    return req_.cookie(name);
  }

  /** @brief 取一个头（大小写不敏感）。不存在返回 `def`。 */
  std::string header(const std::string& name,
                     const std::string& def = std::string()) const {
    return req_.header(name, def);
  }

  /** @brief 对端 IP。未填充时返回空串。 */
  const std::string& peer_ip() const { return req_.peer_ip(); }
  /** @brief 对端端口。未填充时为 0。 */
  unsigned int peer_port() const { return req_.peer_port(); }

 private:
  /** 从原始升级请求接管过来的应用层视图。 */
  uvcpp_web_request req_;

  uvcpp_ws_connection*     conn_ = nullptr;
  uvcpp_web_ws_session*    sess_ = nullptr;
  std::string route_;
};

/**
 * @brief **传输无关**的 WebSocket 会话（extended CONNECT 用；#58）。
 *
 * 三种传输里的后两种 —— h2（RFC 8441）与 h3（RFC 9220）—— 走的是同一套
 * 帧格式，只是承载在流上；这一层把它们收成一个接口，处理器写一次就能同时
 * 服务两条腿。h1 的 `Upgrade` 会话仍然是 `uvcpp_ws_connection`（它的 API 与
 * 本接口同名同义，只是没有虚表）。
 *
 * 用法（与 `uvcpp_ws_connection` 一致）：
 * @code
 *   app.websocket_connect("/chat/:room", [](uvcpp_web_ws_request& ws_req,
 *                                           uvcpp_web_ws_session& s) {
 *     s.on_text([&s](const std::string& msg) { s.send_text(msg.data(), msg.size()); });
 *   });
 * @endcode
 *
 * **生命周期**：对象归框架所有，只在处理器调用期间与之后的有效期内可用 ——
 * `on_close` 回调里是最后一次能碰它的地方。
 */
class UVCPP_API uvcpp_web_ws_session {
 public:
  virtual ~uvcpp_web_ws_session();

  uvcpp_web_ws_session(const uvcpp_web_ws_session&)            = delete;
  uvcpp_web_ws_session& operator=(const uvcpp_web_ws_session&) = delete;

  /** @brief 这条会话跑在哪条传输上：`HVER_20` 或 `HVER_30`。 */
  virtual uvcpp_http_version transport() const = 0;

  virtual int send_text(const char* data, size_t len)      = 0;
  virtual int send_binary(const void* data, size_t len)    = 0;
  virtual int send_ping(const void* data = nullptr, size_t len = 0) = 0;
  virtual int send_pong(const void* data = nullptr, size_t len = 0) = 0;
  /** @brief 发 Close 帧并结束发送方向。 */
  virtual int close(ws_close_code code = ws_close_code::NORMAL,
                    const std::string& reason = std::string()) = 0;
  virtual bool is_open() const = 0;

  virtual void on_text(std::function<void(const std::string&)> cb)             = 0;
  virtual void on_binary(std::function<void(const uint8_t*, size_t)> cb)       = 0;
  virtual void on_ping(std::function<void(const uint8_t*, size_t)> cb)         = 0;
  virtual void on_pong(std::function<void(const uint8_t*, size_t)> cb)         = 0;
  virtual void on_close(
      std::function<void(ws_close_code, const std::string&)> cb) = 0;
  virtual void on_error(std::function<void(int, const std::string&)> cb) = 0;

 protected:
  uvcpp_web_ws_session() {}
};

/**
 * @brief WebSocket 处理器的签名。
 *
 * 参数是栈上的请求视图，返回后即销毁 —— **不要把它存起来**（连接可以存，
 * 请求不可以）。
 */
using uvcpp_web_ws_handler = std::function<void(uvcpp_web_ws_request&)>;

/**
 * @brief **extended CONNECT** 上 WebSocket 处理器的签名（#58）。
 *
 * 与 @ref uvcpp_web_ws_handler 的区别只有第二个参数：给的是传输无关的
 * @ref uvcpp_web_ws_session，而不是 h1 那条腿的 `uvcpp_ws_connection*`。
 * 由 `uvcpp_web_app::websocket_connect()` 注册。
 */
using uvcpp_web_ws_ext_handler =
    std::function<void(uvcpp_web_ws_request&, uvcpp_web_ws_session&)>;

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_WS_H
