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

#include <functional>
#include <string>

#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_request.h>

namespace uvcpp {

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

  uvcpp_ws_connection* conn_;
  std::string route_;
};

/**
 * @brief WebSocket 处理器的签名。
 *
 * 参数是栈上的请求视图，返回后即销毁 —— **不要把它存起来**（连接可以存，
 * 请求不可以）。
 */
using uvcpp_web_ws_handler = std::function<void(uvcpp_web_ws_request&)>;

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_WS_H
