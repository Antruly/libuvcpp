/**
 * @file src/webapp/uvcpp_web_ws_client.h
 * @brief WebSocket 客户端（框架层）：回调装在客户端上 + 可选自动重连。
 * @author zhuweiye
 * @version 1.1.0
 *
 * 服务端在框架层是 `app.websocket("/chat/:room", handler)`；客户端这一侧原先
 * 只有协议层的 `uvcpp_ws_client` —— `connect()` 的回调里拿到会话指针，自己
 * 往上装 `on_text`/`on_close`。本类是它的框架层封装，多出来的是三样：
 *
 * 1. **回调装在客户端上**（`on_open`/`on_text`/`on_binary`/`on_close`/`on_error`）：
 *    `connect()` 之前装、之后装都行，**重连之后照旧生效**。
 * 2. **可选自动重连**（`set_reconnect()`）。默认**关**：服务端正常关闭就静默
 *    重连，会把"服务端出问题了"这件事盖掉，要不要重连由使用者显式决定。
 * 3. `wss://` 的 TLS 上下文透传（`set_ssl_context`）。
 *
 * 为什么要重建而不是复用底层客户端
 * ---------------------------------
 * 协议层的 `uvcpp_ws_client` **不承诺可重连**（`web_ws_client_api_func.cpp` 里
 * 如实记为覆盖不到的边界）：它内部那个 `uvcpp_tcp_client` 的句柄关掉之后
 * `uvcpp_tcp` 会把底层指针置空，且 `has_async_connect_cb_` 在明文成功路径上
 * 从不清除（`uvcpp_tcp_client.cpp:594-598`）—— 也就是说**同一个协议层客户端
 * 对象根本连不了第二次**。
 *
 * 所以本层按协议层文档给的那条路走：**每次连接尝试都换一个协议层客户端**。
 * 新客户端带一个新的事件循环，`connect()` 之前把本层存下的回调重新装一遍。
 * 对使用者不可见（回调存在本层，不在底层）。
 *
 * 由此带来两条必须遵守的约束：
 *
 * - **不要在回调里析构本对象**：本层的 `run()` 是自己一帧一帧驱动底层的
 *   （`inner_->run(UV_RUN_ONCE)`），析构会把**这一帧脚下的对象**连同底层
 *   客户端一起删掉。协议层（`~uvcpp_ws_client`）在自己的回调里被析构是兜得住
 *   的（它整块交出去、不漏一个字节地泄漏），但**本层兜不住** —— 那说的是
 *   "底层客户端"这一层，管不到"正在跑 `run()` 的包装对象"这一层。
 * - **不要在回调里调 `connect()`**：底层循环正在跑，换不得。本层会把它推迟
 *   到当前这一轮循环返回之后（`restart_pending_`），所以不会崩，但这一次调用
 *   只是**登记**，不是立即发起。
 *
 * 线程约定与协议层一致：**非线程安全**，全部调用都在跑 `run()` 的那个线程上。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_WS_CLIENT_H
#define SRC_WEBAPP_UVCPP_WEB_WS_CLIENT_H

// 这一层是 webapp 的零件，判据就用 webapp 那个宏。写成 UVCPP_WEB_ENABLE 是错的：
// 两个宏在"web 开、webapp 关"的构建里取值不同，那样本头文件会在一个不编译
// webapp 的配置里被放行，而它的实现（src/webapp/*.cpp）根本不在库里。
#if UVCPP_WEBAPP_ENABLE

#include <functional>
#include <string>

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>
#include <web/uvcpp_ws_client.h>

#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

namespace uvcpp {

class uvcpp_timer;

/**
 * @brief 本层自己产生的错误码（`on_error` 的第一个参数）。
 *
 * 取值刻意落在 `1..999`：libuv 错误码是负数、WS 关闭码是 `1000+`，三者不会
 * 撞车。`on_error` 的 error 因此可以用一张表判完：
 *
 * - `< 0`    libuv / 协议层错误（`UV_ECONNREFUSED`、`UV_ECANCELED`……）
 * - `>= 1000` 对端或本端协议错误带来的 WS 关闭码（1002 协议错误、1009 过大……）
 * - `1..999`  本层的错误，见下面这个枚举
 */
enum uvcpp_web_ws_client_errno : int {
  /** 重连次数用尽（`max_attempts > 0` 且已经用完）。之后就真的不再连了。 */
  WEB_WS_ERR_RECONNECT_EXHAUSTED = 1,
};

/**
 * @brief 自动重连策略。
 *
 * 默认构造写成显式构造函数而不是成员初始化器：后者会让结构体失去聚合初始化
 * 资格（`uvcpp_web_ws_reconnect{}` 之外再想按位初始化就编译不过）。
 */
struct uvcpp_web_ws_reconnect {
  uvcpp_web_ws_reconnect()
      : enabled(false),
        delay_ms(1000),
        max_delay_ms(30000),
        backoff(true),
        max_attempts(0) {}

  /** @brief 是否启用。默认 **false**。 */
  bool enabled;

  /** @brief 第一次重连前的等待毫秒数。 */
  int delay_ms;

  /**
   * @brief 退避上限毫秒数（仅 `backoff` 打开时用）。
   *
   * `<= 0` 表示不设上限 —— 但内部仍会按 2^30 ms 截断，免得翻倍溢出成负数
   * 让定时器立刻触发（那就成了忙等）。
   */
  int max_delay_ms;

  /** @brief 指数退避：每失败一次，等待翻倍，直到 `max_delay_ms`。 */
  bool backoff;

  /** @brief 最多重试几次；**0 = 不限次数**。 */
  int max_attempts;
};

/**
 * @brief WebSocket 客户端（框架层）。
 *
 * @code
 *   uvcpp::uvcpp_web_ws_client cli;
 *   cli.on_text([](const std::string& m) { std::cout << m << std::endl; })
 *      .on_open([](uvcpp::uvcpp_ws_connection*) { ... });
 *   uvcpp::uvcpp_web_ws_reconnect rc;  rc.enabled = true;
 *   cli.set_reconnect(rc);
 *   cli.connect("ws://127.0.0.1:8080/echo");
 *   cli.run();               // 循环在"没有活句柄"时自然返回
 * @endcode
 */
class UVCPP_API uvcpp_web_ws_client {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_web_ws_client)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_web_ws_client)

  // -------------------------------------------------------------------
  // 连接
  // -------------------------------------------------------------------

  /**
   * @brief 异步连接 `ws://host:port/path` 或 `wss://…`。
   *
   * `cb(0)` = 握手完成、`session()` 可用；非 0 是失败码。**可以不传** ——
   * `on_open`/`on_error` 一样能拿到结果，`cb` 只是给"只关心这一次"的写法省一个
   * lambda。
   *
   * `cb` **最多跑一次**，报的是**这一次** `connect()` 的结果：重连是后面的事，
   * 后续每次尝试的结果走 `on_open`/`on_error`。连接建立中被 `close()` 取消时
   * 收到 `UV_ECANCELED`（取消也是结果，不悬着）。
   *
   * URL 会被存下来给重连用。开着重连时**不要**自己再调 `connect()` 插队 ——
   * 重连由本层按策略驱动。
   *
   * @return 0 = 已发起（或已登记，见类注释里"在回调里调"那条）。
   */
  int connect(const std::string& url,
              std::function<void(int)> cb = nullptr);

  /**
   * @brief 阻塞版连接：自己拨循环，最长等 `timeout_ms`。
   *
   * **只给脚本与测试用**。它在本线程里 `run(UV_RUN_NOWAIT)` + 1ms 睡眠轮询，
   * 与"不在循环线程上跑耗时操作"相抵触 —— 在回调里调它就是卡住那个回调，
   * 生产路径请用 `connect()` + `run()`。
   *
   * @return 0 = 连上；`UV_ETIMEDOUT` = 等超时（连接可能还在建立中）。
   */
  int connect_wait(const std::string& url, int timeout_ms = 30000);

  // -------------------------------------------------------------------
  // 回调 —— 存在本层，重连之后照旧生效
  // -------------------------------------------------------------------

  /**
   * @brief 握手完成。参数是刚建立、**归本层所有**的会话。
   *
   * 重连成功会**再触发一次**，每次都是**新指针**（底层客户端整个换掉了）——
   * 所以不要缓存上一次的指针去比较。想加 ping/压缩/单条消息上限，就在这个
   * 指针上用 `uvcpp_ws_connection` 的那套。
   */
  uvcpp_web_ws_client& on_open(std::function<void(uvcpp_ws_connection*)> cb);

  /** @brief 收到一条文本消息。 */
  uvcpp_web_ws_client& on_text(std::function<void(const std::string&)> cb);

  /** @brief 收到一条二进制消息。`data` 只在回调期间有效。 */
  uvcpp_web_ws_client& on_binary(std::function<void(const uint8_t*, size_t)> cb);

  /**
   * @brief **对端**结束了这条连接（对端 Close 帧、或没发帧就断开 = 1006）。
   *
   * 与协议层一致：本端自己发起的结束（`close()`、协议错误）**不走这里** ——
   * 那些情况调用方本来就知情。也就是说本回调专门回答"对端是怎么走的"。
   *
   * 触发顺序：**先**回调使用者，**再**决定要不要重连 —— 所以在回调里调
   * `close()` 就是"这一条到此为止，别再重连了"。
   */
  uvcpp_web_ws_client& on_close(
      std::function<void(ws_close_code, const std::string&)> cb);

  /**
   * @brief 出错。error 的取值域见 `uvcpp_web_ws_client_errno`。
   *
   * 覆盖：握手/连接失败、会话上的协议错误、重连次数用尽。**不含**对端的正常
   * 关闭（那是 `on_close`）。
   */
  uvcpp_web_ws_client& on_error(std::function<void(int, const std::string&)> cb);

  // -------------------------------------------------------------------
  // 自动重连
  // -------------------------------------------------------------------

  /**
   * @brief 设置重连策略。`enabled == false` 时其余字段被忽略。
   *
   * 重连在两种情况下被排定：**连接/握手失败**，或**会话结束**（对端走的，
   * 或本端协议错误；使用者自己 `close()` 的那次不算）。连上就把次数清零。
   */
  uvcpp_web_ws_client& set_reconnect(const uvcpp_web_ws_reconnect& cfg);

  /**
   * @brief 每次重连**排定**时回调（还没连上），参数是第几次（从 1 起）与等待
   *        毫秒数。
   *
   * 用来打日志/告警。也可以在这里 `set_reconnect()` 改策略 —— 改的是**下一次**
   * 的等待（这一次的等待已经交给定时器了）。
   */
  uvcpp_web_ws_client& on_reconnect(
      std::function<void(int attempt, int delay_ms)> cb);

  // -------------------------------------------------------------------
  // 收发
  // -------------------------------------------------------------------

  /**
   * @brief 发送。没有会话时返回 `UV_ENOTCONN` 并**立刻**回调 `cb(UV_ENOTCONN)`。
   *
   * 不静默丢，与协议层一致："发出去了"这件事只能由回调确认。
   */
  int send_text(const char* data, size_t len,
                std::function<void(int)> cb = nullptr);
  int send_text(const std::string& text,
                std::function<void(int)> cb = nullptr);
  int send_binary(const char* data, size_t len,
                  std::function<void(int)> cb = nullptr);

  /**
   * @brief 主动关闭当前连接。
   *
   * **会顺手取消重连** —— "我不想要它了"和"连接掉了"必须分开，否则使用者
   * 永远关不掉一个开着重连的客户端。
   *
   * 有会话时发 Close 帧、等真正的终结；还在连接建立中时等于**取消**这次连接，
   * 等 `connect()` 回调的人会当场收到 `UV_ECANCELED`（协议层语义，本层照转）。
   */
  void close(ws_close_code code = ws_close_code::NORMAL,
             const std::string& reason = std::string());

  // -------------------------------------------------------------------
  // 状态
  // -------------------------------------------------------------------

  bool is_open() const;
  int get_status() const;
  int get_last_error() const;

  /** @brief 本轮已经排定的重连次数（连上即清零）。次数用尽后不再增长。 */
  int reconnect_attempts() const;

  /** @brief 正在连（或将要重连到）的 URL。还没 `connect()` 时为空串。 */
  const std::string& url() const { return url_; }

  /** @brief 当前会话；没连上或已终结时 nullptr。 */
  uvcpp_ws_connection* session() const;

  // -------------------------------------------------------------------
  // Loop
  // -------------------------------------------------------------------

  /**
   * @brief 拨事件循环。
   *
   * `UV_RUN_DEFAULT` 不是简单转发给底层：本层按"一次一轮"的方式驱动，好让
   * **重连的换客户端动作落在两次循环之间**（换客户端会连带删掉正在跑的那个
   * 循环，落在回调里就是 `uv_run` 重入）。
   *
   * **出口只有两条**，别指望别的：
   *
   * - **没事可等了** —— 循环上一个活句柄都不剩。确切地说是两种情况：不开
   *   重连时对端走掉；开重连时次数用尽且连不上。连着、有在途收发、或有
   *   重连定时器在等的时候都**不算**"没事可等"，那时它不会返回。
   * - **`stop()`** —— 从回调里调才会立刻生效（见 `stop()`）。
   *
   * 所以"连上了就一直不返回"是**对的**，别把 `run()` 当成"连上就返回"的
   * 阻塞式 API —— 那个是脚本/测试用的 `connect_wait()`。
   */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /**
   * @brief 请求停止 `run()`。
   *
   * 与协议层一样：**从回调里调**才会立刻生效。跨线程、或在循环正阻塞于 poll
   * 时调，要等下一次事件到达才返回（libuv 的 `uv_stop` 语义）。
   */
  void stop();

  /** @brief 底层协议层客户端的事件循环。换客户端之后是**另一个**循环。 */
  uvcpp_loop* get_loop();

#if UVCPP_OPENSSL_ENABLE
  /**
   * @brief `wss://` 用的 TLS 上下文。**生命周期要覆盖整条连接**（本层不持有）。
   *
   * 与协议层不同，这里可以在 `connect()` 之后再设：本层存着，每次重建底层
   * 客户端时重新装上去。
   */
  uvcpp_web_ws_client& set_ssl_context(uvcpp_ssl_context* ctx);
#endif

#if UVCPP_ZLIB_ENABLE
  /**
   * @brief permessage-deflate（RFC 7692）协商策略。**默认开启**。
   *
   * 与 `set_ssl_context()` 同一个形状，理由也一样：存一份在本层，**每次重建
   * 底层客户端时重新装上去**。`do_restart()` 是**每次 `connect()` 都重建**的
   * （不只是重连），所以策略若只装在构造出来那个实例上，第一次 `connect()`
   * 就丢了。
   *
   * `cfg.enabled = false` 是"本端连提都不提这个扩展"—— 服务端即便支持也协商
   * 不上，双方退回普通 WS 帧。
   */
  uvcpp_web_ws_client& set_compression(const uvcpp_ws_deflate_config& cfg);

  /** @brief 当前生效的策略（未调过 setter 时是默认值，`enabled == true`）。 */
  uvcpp_ws_deflate_config get_compression() const;
#endif

 private:
  /** @brief 换一个底层协议层客户端并重新发起连接（只在循环之外调用）。 */
  void do_restart();
  /** @brief 把本层存下的回调与 TLS 上下文装到当前底层客户端上。 */
  void install();
  /** @brief 底层连接结果的落点：成功 → `on_open`，失败 → 视策略排重连。 */
  void handle_connect_result(uvcpp_ws_connection* conn, int error);
  /** @brief 会话结束的落点（对端走的）。 */
  void handle_close(ws_close_code code, const std::string& reason);
  /** @brief 排定下一次重连。幂等：已经排着就什么都不做。 */
  void schedule_reconnect();
  /** @brief 停掉并释放重连定时器。 */
  void cancel_reconnect();
  /** @brief 重连定时器到期：登记一次 restart（真正的换客户端在循环之外）。 */
  void on_reconnect_timer();
  /** @brief 本次该等多久（退避在这里算）。 */
  int next_delay_ms() const;

  /**
   * @brief 底层客户端；**每次连接尝试换一个**。
   *
   * 不可为空（构造时就建），否则 `run()`/`get_loop()` 都得先判空。
   */
  uvcpp_ws_client* inner_;

  /** @brief 重连定时器，挂在 `inner_` 的循环上；随 `inner_` 一起换。 */
  uvcpp_timer* timer_;

  std::string url_;
  /** @brief 使用者显式 `close()` 过：别再重连了。 */
  bool user_closed_;
  /** @brief 已经排定的重连次数（连上清零）。 */
  int attempts_;
  /** @brief 有一次 restart 待办：换客户端必须落在底层循环之外。 */
  bool restart_pending_;
  /** @brief 当前正跑在底层循环里面（回调中）—— 此时不许换客户端。 */
  bool in_inner_run_;
  /** @brief `stop()` 的请求；`run(UV_RUN_DEFAULT)` 进入/退出时消费掉。 */
  bool stopping_;
  uvcpp_web_ws_reconnect reconnect_;

#if UVCPP_OPENSSL_ENABLE
  uvcpp_ssl_context* ssl_ctx_;
#endif

#if UVCPP_ZLIB_ENABLE
  /// permessage-deflate 策略。存一份的理由同 `ssl_ctx_`：`inner_` 每次
  /// `connect()`（以及每次重连）都换新，装晚了就是"配置在重建时丢了"。
  uvcpp_ws_deflate_config deflate_cfg_;
#endif

  /** @brief 本次 `connect()` 的回调（一次性，报第一次尝试的结果）。 */
  std::function<void(int)> connect_cb_;
  std::function<void(uvcpp_ws_connection*)>              on_open_;
  std::function<void(const std::string&)>                on_text_;
  std::function<void(const uint8_t*, size_t)>            on_bin_;
  std::function<void(ws_close_code, const std::string&)> on_close_;
  std::function<void(int, const std::string&)>           on_error_;
  std::function<void(int, int)>                          on_reconnect_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
#endif  // SRC_WEBAPP_UVCPP_WEB_WS_CLIENT_H
