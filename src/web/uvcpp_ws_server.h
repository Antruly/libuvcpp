/**
 * @file src/web/uvcpp_ws_server.h
 * @brief WebSocket server — standalone or backed by uvcpp_http_server.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Detects WebSocket upgrade requests in the embedded HTTP server,
 * completes the handshake (RFC 6455 Section 4), and delivers new
 * connections as uvcpp_ws_connection instances.
 *
 * Standalone usage:
 * @code
 *   uvcpp_ws_server ws;
 *   ws.bind("0.0.0.0", 9000);
 *   ws.on_connection([](uvcpp_ws_connection* c) {
 *     c->on_text([](const std::string& msg) { ... });
 *   });
 *   ws.run(UV_RUN_DEFAULT);
 * @endcode
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_SERVER_H
#define SRC_WEB_UVCPP_WS_SERVER_H

#if UVCPP_WEB_ENABLE

#include <functional>
#include <string>
#include <uvcpp/uvcpp_define.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_ws_ext.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_sessions.h>
#include <web/uvcpp_http_server.h>

namespace uvcpp {

class UVCPP_API uvcpp_ws_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ws_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ws_server)

  /**
   * @brief 挂在一个**已存在**的 HTTP 服务器上，但**不接管** `on_upgrade`。
   *
   * 与 `attach()` 的区别只有一点，但很关键：`attach()` 会把本服务器装成
   * HTTP 层的升级处理器（`on_upgrade` 是**单槽**），于是升级的分派权归本类；
   * 本构造函数不装，分派权留在调用方手里 —— 框架要把升级按**路径**分派给
   * 不同的 WS 路由，就必须自己攥着那个槽，只在路径命中时调 `handle_upgrade()`。
   *
   * 用它的另一个好处是不必"先默认构造一个自带 loop 的 http_server 再扔掉"。
   *
   * @note 调用方仍需保证 `http` 活得比本对象久（本对象不持有它）。
   */
  explicit uvcpp_ws_server(uvcpp_http_server* http);

  // -------------------------------------------------------------------
  // Standalone mode: bind + listen
  // -------------------------------------------------------------------

  int bind(const char* ip, int port);
  int listen(int backlog = 128);

  // -------------------------------------------------------------------
  // Attach to existing HTTP server (shared mode)
  // -------------------------------------------------------------------

  /** @brief Handle WS upgrades on an existing HTTP server. */
  void attach(uvcpp_http_server* http);

  /**
   * @brief 处理一个**已决定接受**的升级请求：101 + 建会话 + 回调用户。
   *
   * 由 `attach()` 自动接到 `on_upgrade` 上用；分派权自持时（见上）由调用方
   * 在路径命中后调用。**本函数只负责"接受"，不负责"判断该不该接受"** ——
   * 后者是调用方的事（框架层按路由表判断，没命中就走普通 HTTP 路由）。
   *
   * 缺 `Sec-WebSocket-Key` 时静默返回（不建会话、不应答）。这个"静默"对
   * 调用方是有代价的：连接会哑在那里，既不断也不回。所以自持分派权的调用
   * 方**必须自己先校验**，缺 key 就回 400。
   */
  void handle_upgrade(uvcpp_http_request& req, uvcpp_tcp_client* client);

  /**
   * @brief 同上，但这次升级的结果**只**报给 `on_ready`，不碰 `on_connection`。
   *
   * 为什么需要重载：`on_connection` 是**单槽**，而自持分派权的调用方（框架层）
   * 往往要**同时**处理多条 WS 路由，每条路由的处理逻辑不同。它当然可以在
   * `on_connection` 里再查一次表，但那要求它记住"这次升级对应哪个请求" ——
   * 而 `on_ready` 是在 101 的**写完成回调**里触发的（异步，晚于 `handle_upgrade`
   * 返回），所以栈上的任何临时信息那时都已经没了，必须靠一个随本次升级走的
   * 闭包把上下文带过去。这个参数就是这个闭包。
   *
   * `on_ready` 为假时行为与上面那个重载完全一致。
   *
   * @param on_ready 收到**已握手完成、已接管所有权**的会话。**只有真建了会话
   *                 才会调用** —— 101 写失败（对端提前断开）时不调用，此时
   *                 这个闭包随写回调一起销毁，调用方不必自己清理。
   */
  void handle_upgrade(uvcpp_http_request& req, uvcpp_tcp_client* client,
                      std::function<void(uvcpp_ws_connection*)> on_ready);

  // -------------------------------------------------------------------
  // Connection callback
  // -------------------------------------------------------------------

  void on_connection(std::function<void(uvcpp_ws_connection*)> cb);

  // -------------------------------------------------------------------
  // Loop
  // -------------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /**
   * @brief 停止服务：先给所有活动会话发 Close 帧，再停底层 HTTP 服务。
   *
   * 会话的**回收**不在这里完成：Close 帧发出去要几轮循环，那些会话随后由
   * 终结回调送回并回收。循环如果立刻停了，剩下的由析构兜底 —— 不会漏。
   */
  void stop(std::function<void()> on_stopped = nullptr);

  /**
   * @brief 只给所有活动会话发 Close 帧，**不碰** HTTP 服务与循环。
   *
   * `stop()` 拆出来的前半。分派权自持时（框架层），停机流程由框架自己编排：
   * 它要先让 WS 会话优雅关闭、再停 HTTP —— 而它**不能**调 `stop()`，因为那会
   * 把 HTTP 服务一并停掉，框架自己还有收尾要做。
   *
   * 与 `stop()` 一样，这一步只是**发起**关闭：帧要几轮循环才发得出去，回收
   * 由会话终结回调完成。
   *
   * @param code 发出去的 Close 帧状态码。属主停机时传 1001 `GOING_AWAY`，
   *             让对端知道"是服务器要关了"而不是"对方正常告别"。
   */
  void close_all_sessions(ws_close_code code = ws_close_code::NORMAL);

  // -------------------------------------------------------------------
  // Accessors
  // -------------------------------------------------------------------

  uvcpp_http_server* get_http_server();

  /** @brief 当前活动的 WebSocket 会话数。 */
  size_t session_count() const;

  /**
   * @brief 累计已回收（真正 `delete`）的会话数。**单调递增**。
   *
   * 用来回答"会话到底有没有被释放"：只数活动会话数是不够的 —— 漏掉一个
   * 会话既可能表现为"还挂在活动表里"，也可能表现为"终结了但没人删"。
   * 计数对上（活动 0 + 已回收 == 建过的总数）才算闭环。
   */
  size_t recycled_session_count() const;

  // -------------------------------------------------------------------
  // permessage-deflate 协商（RFC 7692）— 仅 UVCPP_ZLIB_ENABLE=1
  // -------------------------------------------------------------------
#if UVCPP_ZLIB_ENABLE
  /**
   * @brief 配置 permessage-deflate 协商策略。**默认开启**。
   *
   * 默认开启是安全的：压缩只有在**对端主动在请求里提**了这个扩展、且本端
   * 接受之后才会生效，本端从不单方面压缩。对端没提就退化成普通 WS。
   *
   * 协商不上（对端的约束本端满足不了、参数非法……）时**不应答**这个扩展，
   * 这在 RFC 7692 §7.1.2 里是完全合法的降级，不会让握手失败。
   */
  void set_compression(const uvcpp_ws_deflate_config& cfg);
  uvcpp_ws_deflate_config get_compression() const;
#endif

 private:
  static std::string compute_accept_key(const std::string& client_key);
  static std::string sha1(const std::string& input);

  uvcpp_http_server* http_server_ = nullptr;
  bool owns_http_ = false;

  /// 本服务器创建的全部会话。会话终结时把自身交回这里回收。
  uvcpp_ws_sessions sessions_;

  std::function<void(uvcpp_ws_connection*)> on_conn_;

#if UVCPP_ZLIB_ENABLE
  uvcpp_ws_deflate_config deflate_cfg_;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_SERVER_H
