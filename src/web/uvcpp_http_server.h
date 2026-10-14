/**
 * @file src/web/uvcpp_http_server.h
 * @brief HTTP/1.1 server — accept connections, route requests, send responses.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Built on uvcpp_tcp_server.  Each TCP connection gets an independent
 * uvcpp_http_parser context.  Incoming requests are matched against
 * registered routes; the matching handler receives the parsed request
 * and a writable response object to fill in.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_SERVER_H
#define SRC_WEB_UVCPP_HTTP_SERVER_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <atomic>
#include <functional>
#include <map>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <uv.h>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

namespace uvcpp {

#if UVCPP_NGHTTP2_ENABLE
// 只前向声明。`uvcpp_h2_connection` 的头是 pimpl 形状的（nghttp2 只在 .cpp 里），
// 这里连它的头都不引 —— 引了就把"关掉 nghttp2 也能编 web 模块"作废。
class uvcpp_h2_connection;
#endif

// 身份凭证那个类同上，但**无条件**前向声明：下面 `set_quic_ssl_context()` 的
// 声明不带 `#if`（理由写在那一条上）。
class uvcpp_ssl_context;

#if UVCPP_HTTP3_ENABLE
// h3 与 QUIC 两层同 h2 那条理由：头都是 pimpl 形状的（nghttp3 / ngtcp2 只在
// .cpp 里），引了就把"关掉 http3 也能编 web 模块"作废。
class uvcpp_h3_connection;
class uvcpp_quic_server;
class uvcpp_quic_connection;
#endif

class uvcpp_http_parser;

// =========================================================================
// Status flags
// =========================================================================

enum uvcpp_http_server_status : int {
  HTTP_SERVER_NONE      = 0x00,  ///< Initial state
  HTTP_SERVER_LISTENING = 0x01,  ///< Bound and listening
  HTTP_SERVER_STOPPING  = 0x04,  ///< Stopping
  HTTP_SERVER_STOPPED   = 0x08,  ///< Fully stopped
  HTTP_SERVER_ERROR     = 0x10,  ///< Error occurred
};

// =========================================================================
// Handler type
// =========================================================================

/**
 * @brief HTTP request handler.
 * @param req     The parsed incoming request.
 * @param resp    The response object to fill in and send back.
 * @param client  The underlying TCP client (for advanced use: close, raw write, etc.)
 */
using http_request_handler = std::function<void(
    uvcpp_http_request& req,
    uvcpp_http_response& resp,
    uvcpp_tcp_client* client)>;

// Streaming-body delivery: instead of accumulating the whole body in memory,
// the server hands each body chunk to the handler as it is parsed.
enum class http_stream_event : uint8_t {
  HEADERS = 0,  // headers done; req has method/url/version/headers (body empty)
  BODY    = 1,  // a body chunk (data/len; may fire many times)
  END     = 2,  // message complete; handler finishes and calls send_response
};

using http_stream_handler = std::function<void(
    http_stream_event ev,
    const char* data, size_t len,
    uvcpp_http_request& req,
    uvcpp_tcp_client* client)>;

/**
 * @brief Streaming claim hook — decides, at headers time, who owns the body.
 *
 * Called once per message, after the headers are parsed but **before any body
 * byte is delivered**. Returning an **empty** handler means "not claimed": the
 * request takes the ordinary path (body accumulated into the request, then the
 * route handler runs). Returning a **non-empty** handler means "claimed": body
 * bytes are handed to it as @ref http_stream_event::BODY, the message end as
 * @ref http_stream_event::END, and **`on_request` / the route handler is never
 * called for this message**.
 *
 * How it differs from @ref post_stream: that one is a convenience registration
 * for "POST + exact path" and is matched first. This hook is consulted for
 * every request and constrains neither the method nor the path shape, which is
 * what a caller with its own router (patterns, params, wildcards) needs — it
 * does the matching itself and claims selectively.
 *
 * @param req     Headers-only view of the request. Valid only for the duration
 *                of the call: copy anything you need to keep.
 * @param client  The connection, for the rare case the decision depends on it.
 * @return The stream handler that takes over this request's body, or an empty
 *         function to leave the request alone.
 *
 * @warning Runs on the event-loop thread, inside the parser's header callback.
 *          Keep it fast — no I/O, no blocking. A claimed request's body is
 *          **not** subject to @ref set_max_body_size (the server never buffers
 *          it); a claimer that needs a cap enforces its own.
 */
using http_stream_claim = std::function<http_stream_handler(
    uvcpp_http_request& req, uvcpp_tcp_client* client)>;

/**
 * @brief Raw TCP data hook — sees every inbound chunk BEFORE the HTTP parser.
 *
 * Lets an application observe or take over a connection at the byte level,
 * which is not otherwise possible: the server owns the socket's read callback,
 * so nothing outside it can be inserted ahead of the parser.
 *
 * @param client  The connection the data arrived on.
 * @param data    The received bytes. **Read-only** — the hook must not modify
 *                them. The parser's buffering arithmetic assumes the bytes it
 *                was handed are the bytes that arrived, so rewriting them in
 *                place corrupts the remaining parse.
 * @param len     Number of bytes in @p data.
 * @return true   the chunk also goes on to the HTTP parser (observe only);
 *         false  the hook consumed the chunk — it is NOT parsed as HTTP.
 *
 * Runs on the event-loop thread: keep it fast, do not block. For heavy
 * inspection (pcap-style dumping, signature scans) copy the bytes and hand
 * them to uvcpp_work.
 */
using http_raw_data_hook = std::function<bool(
    uvcpp_tcp_client* client, const char* data, size_t len)>;

// =========================================================================
// HTTP Server class
// =========================================================================

/**
 * @brief HTTP/1.1 server.
 *
 * Usage:
 * @code
 *   uvcpp_http_server server;
 *   server.bind("0.0.0.0", 8080);
 *   server.get("/hello", [](auto& req, auto& resp, auto* c) {
 *     resp = uvcpp_http_response::ok("world", 5);
 *   });
 *   server.listen();
 *   server.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_http_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_http_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_http_server)

  // -------------------------------------------------------------------
  // Bind / Listen
  // -------------------------------------------------------------------

  int bind(const char* ip, int port);
  int bindIpv4(const char* ip, int port);
  int bindIpv6(const char* ip, int port);

  /**
   * @brief 设定工作循环数。**不调用（或 `set_loops(1)`）= 今天的行为**。
   *
   * 直接转发给 @ref get_tcp_server 的 `uvcpp_tcp_server::set_loops(n)` —— 两条
   * 腿合用一套循环，所以这个数**同时**决定 h1/h2 与 h3 各用几条。那段完整语义
   * （n 条循环各跑各的、内核分流还是接受者+转手、`is_fanout()` 怎么读、
   * 装上之后谁在哪个线程上、Windows 那条路的代价与边界）写在
   * `net/uvcpp_tcp_server.h` 里，这里不重抄。
   *
   * 与那条腿的**唯一区别**是 h3 那半边的接线，写在 @ref listen_quic 里。
   *
   * @param n 循环总数，取 1..64。
   * @return 0 成功；`UV_EINVAL` n 越界；`UV_EBUSY` 已经 `listen()` 过、已经用
   *         `n > 1` 装过一遍，或者**已经 `listen_quic()` 过**（见下）。`n == 1`
   *         是幂等的空操作。
   *
   * @warning **必须在 `listen()` 与 `listen_quic()` 之前调。** 后一条是本层
   *          独有的：QUIC 那些端点是**按 `loop_count()` 定尺寸**一次建齐的
   *          （n 个 socket 各绑各的），端点建完再改 n 就变成"表按 1 张定尺寸、
   *          而回调报 1..n−1 号"，只能拒。TCP 那半边的 `set_loops()` 只看得到
   *          自己 listen 没 listen —— h3-only 的服务端上它一次都没 listen 过，
   *          挡不住这件事，所以这道门补在本层。
   *
   * @note 这个函数**不做** h3 专属的第二个旋钮。两条腿各一个循环数会让表的尺寸、
   *       `run()` / `stop()` 的语义全部分叉，而它们共用一条 `uv_run()`。
   */
  int set_loops(int n);

  /** @brief 循环总数（1 + 工作循环数）。没调过 `set_loops()` 就是 1。 */
  int loop_count() const;

  int listen(int backlog = 128);

#if UVCPP_HTTP3_ENABLE
  /**
   * @brief 起一个 **HTTP/3（QUIC/UDP）** 监听口。
   *
   * 与 @ref listen 那份 TCP 口**各自独立**：两个都开 = 同一个服务端对象同时
   * 用 h1/h2（TCP）与 h3（QUIC）服务，只开一个是合法的。路由表、兜底处理函数、
   * 压缩旋钮全是**同一份** —— 处理函数不需要知道自己被哪条传输调起来。
   *
   * @param port UDP 端口。**0 = 让内核挑**，之后用 @ref quic_listen_port 读回来。
   * @param ip   nullptr = 通配（IPv4 `0.0.0.0`），与 `uvcpp_quic_server::bind()`
   *             同一套取值（IPv6 请用 IP 字面量，如 `"::1"`）。
   *
   * @return 0 成功；`UV_EALREADY` 本对象上已经有一个 QUIC 口；
   *         `UV_EINVAL` 还没装 TLS 上下文（见 @ref set_quic_ssl_context：
   *         QUIC 没有明文模式）；其余是 bind / listen 的失败码。
   *
   * @note **顺序**：`set_quic_ssl_context()` → `listen_quic()` → `run()`。与 TCP
   *       那条腿一样，端口要 `listen_quic()` 之后才真的可连。要用多循环就先
   *       `set_loops(n)`（见那条的 `@warning`）。
   *
   * ## 多循环
   *
   * `set_loops(n)` 过（n > 1）时这里**建 n 个 QUIC 端点**：0 号建在 acceptor
   * 那条循环上，1..n−1 号各投递到对应的工作循环上、在那条线程上建，全部建好才
   * 返回。n 个 socket 各带 `UV_UDP_REUSEPORT`、**抢同一个端口**，内核按四元组
   * 哈希把数据报分给其中一个（QUIC 的 CID 表是按端点各存一份的，够用 —— 理由
   * 与那条边界写在 `quic/uvcpp_quic_server.h` 的类注释里）。所以 @ref run 与
   * @ref stop 仍然一次管全，调用方不必为 h3 多写一行。
   *
   * **这不是"Linux 限定"**：`UV_UDP_REUSEPORT` 要 libuv **1.50+**，而 Windows
   * 上 libuv 无条件拒绝、macOS 上 `uv__sock_reuseport()` 没有实现。那些平台上
   * 0 号那次绑定会**自动去掉标志重绑一次**（并在 stderr 上说明），于是
   * @ref quic_fanout 为 `false`、**只有 0 号一个端点** —— 功能与单循环逐字相同，
   * 只是没有扇出。判据要按 @ref quic_fanout 分叉，别拿 `#ifdef` 猜平台。
   *
   * **扇出失败是原子的**：1..n−1 号里任何一个建不起来（端口被别人抢走之类），
   * 已经建好的全部拆掉、0 号也拆掉，返回那个错误码，本对象回到"没有 QUIC 腿"
   * 的状态。
   *
   * @warning **h3 上暂不支持流式路由**。`post_stream` 注册的流式处理函数与
   *          `set_stream_claim` 装的认领钩子在 TCP 那条路上才生效；一条 h3 请求
   *          撞上它们时**不会**被静默改道，而是回 500 并在 stderr 上说明。
   *          同理，处理函数在 h3 上拿到的是 `client == nullptr`，`resp.deferred`
   *          也没法兑现 —— 撞上它同样回 500 并说明。
   *
   * @note 这一格（连同下面两格）跟着 `UVCPP_HTTP3_ENABLE` 一起在/不在：HTTP/3
   *       没编进来时**整个 h3 公开面都不存在**，与 `uvcpp_http_client` 那边的
   *       `set_http3_enabled()` 同一套做法。这是**编译期**的"没有"，不是运行时
   *       的失败 —— 也因此，`UVCPP_ENABLE_HTTP3=OFF` 的树里本类与从前逐字相同。
   */
  int listen_quic(int port, const char* ip = nullptr);

  /**
   * @brief 给 h3 那条腿装身份凭证。**必须早于 @ref listen_quic。**
   *
   * QUIC 的每条连接在**首包**时就要有一个可用的 SSL_CTX（ALPN 是 TLS 扩展，
   * QUIC 没有明文模式），所以装晚了 `listen_quic()` 直接返回 `UV_EINVAL`
   * —— 而不是收下一个永远握不上手的端口。
   *
   * 与 TCP 那条腿**分开装**（`get_tcp_server()->set_ssl_context()`）：它们可以
   * 不是同一份凭证，而且用途不同 —— TCP 那份要协商 `h2`，QUIC 这份只需要 h3
   * （`uvcpp_quic_server` 默认就用 `"h3"`）。
   */
  void set_quic_ssl_context(uvcpp_ssl_context* ctx);

  /**
   * @brief h3 那条腿实际绑上的 UDP 端口；没起来时返回 0。
   *
   * 与 `uvcpp_tcp_server` 那边取端口的方式不同，这里给的是个直读的口子：
   * QUIC 端点不在 `get_tcp_server()` 的账上，没有别的门能问到它。
   *
   * @note 多循环下**所有端点绑的是同一个端口**（那正是内核分流的前提），所以
   *       报 0 号那一个就够 —— 它们不是"n 条腿各有各的口"。
   */
  int quic_listen_port() const;

  /**
   * @brief 这次 `listen_quic()` 是不是**真的**建出了 n 个端点（内核分流）。
   *
   * **运行时探测的结果，不是编译期常量**：与 `uvcpp_tcp_server::is_fanout()`
   * 同一套理由（`UV_UDP_REUSEPORT` 要 libuv 1.50+，且 Windows 无条件拒绝、
   * macOS 没实现）。`false` 时有且只有 0 号一个端点 —— h3 那半边的功能与
   * 单循环逐字相同。
   *
   * `listen_quic()` 之前、以及从没调过 `set_loops(n>1)` 时恒为 `false`。
   *
   * @note 本层的 `quic_fanout()` 与 `get_tcp_server()->is_fanout()` **可以不同**：
   *       那是两种不同的 socket 选项（`UV_TCP_REUSEPORT` 1.49+ 对
   *       `UV_UDP_REUSEPORT` 1.50+），平台清单也不完全一样。要判 h3 的分布形状
   *       就问这一个。
   */
  bool quic_fanout() const;

  /**
   * @brief 本层**当前登记着**的 h3 连接总数（所有循环相加）。
   *
   * 数的是"h3 驱动层已经为它建起来的那些连接" —— 一条 QUIC 连接在**首包到达**
   * 时就被 `on_quic_connection()` 收下表，所以这个数比"握手完成了几条"早。
   *
   * 与 `client_count()` 那条同一套口径：它是**读数**，不是判据 —— 想知道分布
   * 形状按 @ref quic_fanout 分叉。
   *
   * @note **可以从任何线程调**（各循环的计数由一把小锁护着，而它只在连接登记 /
   *       摘除时碰，不在每请求那条路上），拿到的是"问的那一刻"的近似值：别的
   *       循环正在建的连接可能这一拍还没算进来。要精确值就得在停机之后问，而
   *       那时表已经空了 —— 所以它天生是个**运行中**的读数。
   */
  size_t quic_connection_count() const;

  /**
   * @brief 第 \p loop_index 条循环名下**当前登记着**的 h3 连接数。越界返回 0。
   *
   * **0 号不是恒 0 的**：@ref quic_fanout 为真时 0 号（acceptor 那条）自己也收
   * 属于它的那一份。与 `uvcpp_tcp_server::client_count_at()` 同一条口径，包括
   * 那句"只保证逐格之和 == @ref quic_connection_count，别拍均匀度阈值"。
   *
   * 线程与近似值的说明同 @ref quic_connection_count。
   */
  size_t quic_connection_count_at(int loop_index) const;
#endif

  // -------------------------------------------------------------------
  // Route registration
  // -------------------------------------------------------------------

  /** @brief Default handler (called when no route matches). */
  void on_request(http_request_handler handler);

  /**
   * @brief 让 ALPN 协商出 `h2` 的连接走 HTTP/2 服务。
   *
   * **默认 false** —— 不显式打开就恒为 HTTP/1.1，与既有行为逐字一致。
   * 打开之后也只对"TLS 握手协商出 h2"的连接生效：明文连接、以及 ALPN 没协商出
   * h2 的连接，照旧走 llhttp。服务端要真的能协商出 h2，还得在
   * `uvcpp_ssl_context` 上 `set_alpn_select_protos({"h2","http/1.1"})` ——
   * 这一层不做代理，两条都得显式给。
   */
  void set_http2_enabled(bool on) { http2_enabled_ = on; }
  bool http2_enabled() const { return http2_enabled_; }

  /**
   * @brief 让**明文**连接也能走 HTTP/2（先验知识 h2c）。
   *
   * **默认 false** —— 不显式打开，明文连接就恒走 llhttp，与既有行为逐字节一致。
   *
   * 打开之后，每条非 TLS 连接的头 24 个字节与 RFC 9113 §3.4 的连接前言
   * （`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`）**逐字节比一次**：全中 ⇒ 这条连接交给
   * h2，否则 ⇒ 照旧给 llhttp，而且比过的那几个字节一个不少地补喂回去。
   *
   * 两件事要一起说清楚，因为它们决定了这个开关能不能乱开：
   *
   * - **这不是嗅探。** 判据是固定串的逐字节比较，不是"看着像 HTTP/2 就按 HTTP/2
   *   处理"。所以它**只**支持先验知识那一种 h2c（客户端一上来就发前言），
   *   **不做** `Upgrade: h2c` 协商 —— 那个在 RFC 9113 里已经被删掉了。合法的
   *   HTTP/1.1 请求里没有哪一条会长成前言的样子（`PRI` 不是一个方法），所以这里
   *   没有"误判"这一类风险。
   * - **代价在每条明文连接的头 24 个字节上**，且只在打开之后：比较是逐字节的，
   *   定案后立刻停（`conn_ctx::h2c_probe_state`），此后每条连接零开销。关闭时
   *   （默认）一个字节都不比。
   *
   * @note 还要 @ref set_http2_enabled 也是开的：h2c 是"HTTP/2 走明文"，不是
   *       "另一种 HTTP/2"。只开这一个等于什么都不发生。
   *
   * @note 被改判的那条连接上 @ref on_connection 钩子**只跑一次**（在按 h1 登记
   *       的时候），不会因为改判而再跑一次 —— 那个钩子的契约是"比这条连接上
   *       任何请求都早的簿记点"，跑两次会让调用方的每条连接计数翻倍。
   */
  void set_h2c_enabled(bool on) { h2c_enabled_ = on; }

  /**
   * @brief h2c 是不是真的开着。
   *
   * **与 `set_h2c_enabled()` 给的那个值不一定相等**，两处都可能让它答 false：
   * 这份 libuvcpp 没编进 nghttp2（那种树上 `set_h2c_enabled(true)` 记下的只是
   * "意图"，请求路径上不会有一行跑）、或者 @ref http2_enabled 不是真。**判据要看
   * 这个值**，别拿 `#ifdef` 猜、也别拿自己设过的那个值猜。
   */
  bool h2c_enabled() const {
#if UVCPP_NGHTTP2_ENABLE
    return h2c_enabled_ && http2_enabled_;
#else
    return false;
#endif
  }

  /**
   * @brief 给**本循环上**的 h2 连接发 GOAWAY，但一条都不关。
   *
   * 停机前该先调它，再走关连接那一套：GOAWAY 排进队列之后要几轮循环才出网，
   * 而关连接是立刻生效的 —— 顺序反了，对端只看到"连接断了"，分不清服务端在
   * 停机还是网络挂了，也就没法把在飞的请求安全地挪到别的连接上。
   *
   * `last_stream_id` 是本端已处理的最大流号，所以对端能据此分辨哪几条它发过、
   * 我们**没处理**（那些可以安全重试）。已有的流一条都不受影响，照跑完。
   *
   * @return 真的把 GOAWAY 排出去（且成功冲网）的连接条数。h2 没开时恒为 0。
   *
   * @note **"本循环上"是刻意的**（多循环）：连接上下文是按循环切的，从一条循环
   *       上去翻另一条循环的表就是数据竞争。所以调用方要在**每条**循环上各调
   *       一次（webapp 的停机就是这样按循环推进的），而不是在 0 号循环上指望
   *       一句管全进程。单循环下"本循环"就是"全部"，行为与从前逐字相同。
   *
   * @note 不关连接是**有意的**：调用方还得留出时间让字节出网，之后自己走
   *       `close_connection()` / `uvcpp_tcp_server::close_all_clients()`。
   */
  size_t begin_h2_goaway();

#if UVCPP_NGHTTP2_ENABLE
  /**
   * @brief 在指定的 h2 流上回一个响应。
   *
   * 处理函数**整对象替换**过 `resp` 时用这个：那种写法会把 `resp.stream_id`
   * 冲成 0，于是普通 `send_response` 会把它当成 HTTP/1.1 的应答。
   */
  int send_h2_response(uvcpp_tcp_client* client, int32_t stream_id,
                       uvcpp_http_response& resp);
#endif


  /**
   * @brief Upgrade handler — called BEFORE routing when Upgrade: websocket
   *        is detected.  The handler should send the 101 response, then take
   *        ownership of the TCP client for WebSocket framing.
   */
  using upgrade_handler_t = std::function<void(uvcpp_http_request&, uvcpp_tcp_client*)>;
  void on_upgrade(upgrade_handler_t handler);

  /**
   * @brief 取走"跟着升级请求一起到达的多余字节"（**升级方专用**）。
   *
   * 升级请求和第一帧挤在同一个 TCP 段里是常态，而那批字节在 HTTP 解析器
   * 吃完请求之后就已经被这次读取走了 —— 升级方如果不取，重新 `read_start`
   * 是读不回来的，表现就是"新协议的第一帧凭空消失"。
   *
   * **必须在升级回调之后取**（升级回调是在解析器 `execute()` 里面同步调的，
   * 那时剩余字节还没算出来），比如在自己的应答写完成回调里 —— WS 层就是这么
   * 用的（`uvcpp_ws_server.cpp` 的 101 写完成回调）。
   *
   * 取走即清空；不是升级连接 / 连接已经没了 / 没有多余字节都返回空串。
   */
  std::string take_upgrade_leftover(uvcpp_tcp_client* client);

  /** @brief Register a handler for GET + exact path. */
  void get(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for POST + exact path. */
  void post(const std::string& path, http_request_handler handler);
  /** @brief Register a streaming-body handler for POST + exact path. */
  void post_stream(const std::string& path, http_stream_handler handler);

  /**
   * @brief Install the streaming claim hook (see @ref http_stream_claim).
   *
   * Single slot — installing a new hook replaces the previous one; passing an
   * empty std::function is equivalent to @ref clear_stream_claim.
   *
   * Matching order per message: a `post_stream` route is tried first (it is a
   * registration, not a policy); only if nothing matched is the claim hook
   * asked. So a hook cannot accidentally shadow an explicitly registered
   * streaming route.
   */
  void set_stream_claim(http_stream_claim hook);

  /** @brief Remove the claim hook (no request is ever claimed). */
  void clear_stream_claim();

  /** @brief Whether a claim hook is currently installed. */
  bool has_stream_claim() const;
  /** @brief Register a handler for PUT + exact path. */
  void put(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for DELETE + exact path. */
  void del(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for OPTIONS + exact path. */
  void options(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for PATCH + exact path. */
  void patch(const std::string& path, http_request_handler handler);
  /** @brief Register a handler for HEAD + exact path. */
  void head(const std::string& path, http_request_handler handler);

  // -------------------------------------------------------------------
  // Request size limit
  // -------------------------------------------------------------------

  /**
   * @brief Cap the request body size (bytes). 0 = unlimited (default).
   *
   * Without a cap, a single large (or malicious) upload is buffered in full
   * in memory. When a body exceeds the cap the server stops buffering it and
   * answers 413 with `Connection: close` without routing the request.
   *
   * **Read live.** Unlike the two limits below, this one is compared against
   * every body chunk as it arrives, so changing it takes effect on connections
   * that are already open.
   */
  void set_max_body_size(size_t max_bytes);
  size_t max_body_size() const;

  /**
   * @brief Cap the request header block (bytes). 0 = unlimited (default).
   *
   * Counts the whole header block as it appears on the wire — every field's
   * `name: value\r\n`, excluding the request line and the final empty line.
   * Checked **while** the headers arrive, not after: the parser stops as soon as
   * the running count passes the cap, so an oversized header block costs the
   * server a bounded amount of CPU and bandwidth rather than all of it. The
   * request is answered 431 with `Connection: close` and never routed.
   *
   * **Unlike `max_body_size()`, a claimed request is not exempt**: the running
   * count is enforced by the parser as the headers arrive, so an oversized block
   * is rejected 431 with `Connection: close` before `headers_complete` — and the
   * claim hook is therefore never asked about it.
   *
   * **Takes effect for new connections only.** The value is copied into each
   * connection's parser when that connection is accepted, so changing it here
   * does not affect a connection that is already open.
   */
  void set_max_header_bytes(size_t max_bytes);
  size_t max_header_bytes() const;

  /**
   * @brief Cap the request target / URL (bytes). 0 = unlimited (default).
   *
   * Same early-stop behaviour and the same per-connection timing as
   * `set_max_header_bytes()`; the request is answered 414 with
   * `Connection: close` and never routed.
   */
  void set_max_url_bytes(size_t max_bytes);
  size_t max_url_bytes() const;

  // -------------------------------------------------------------------
  // Raw TCP data interception
  // -------------------------------------------------------------------

  /**
   * @brief Install a hook that sees inbound bytes before the HTTP parser.
   *
   * Single slot — installing a new hook replaces the previous one. The hook
   * runs on the event-loop thread ahead of parsing; returning false means the
   * hook took the chunk and the parser never sees it.
   *
   * @note Passing an empty std::function is equivalent to clear_raw_data_hook().
   */
  void set_raw_data_hook(http_raw_data_hook hook);

  /** @brief Remove the raw data hook (inbound bytes go straight to the parser). */
  void clear_raw_data_hook();

  /** @brief Whether a raw data hook is currently installed. */
  bool has_raw_data_hook() const;

  // -------------------------------------------------------------------
  // Compression (needs UVCPP_HTTP_COMPRESS_ENABLE=1)
  // -------------------------------------------------------------------
  // 守卫用 `UVCPP_HTTP_COMPRESS_ENABLE`（= zlib 或 brotli 任一编进来了），
  // **不是** `UVCPP_ZLIB_ENABLE`：这一整块（协商 → 变体缓存 → 响应头）与"用哪个
  // 编码器"是两件事 —— `br` 进来之后，zlib 关着也照样能把压缩这条路走完。

#if UVCPP_HTTP_COMPRESS_ENABLE
  /** @brief Enable/disable automatic response body compression.
   *         Default: enabled when UVCPP_HTTP_COMPRESS_ENABLE=1. */
  void set_compression_enabled(bool enable);
  bool is_compression_enabled() const;

  /** @brief Set the minimum body size (bytes) to trigger compression. Default 1024. */
  void set_compress_min_body_size(size_t min_size);

  /** @brief Override the default MIME exclusion list. */
  void set_compress_excluded_types(const std::vector<std::string>& types);

  /** @brief Add a single MIME type to the exclusion list. */
  void add_compress_excluded_type(const std::string& mime_type);

  /**
   * @brief 压缩档位：`-1` 是 zlib 的 `Z_DEFAULT_COMPRESSION`，其余 0..9。
   *        默认 `http_compress::k_compress_level_default`（= `-1`）。
   *
   * 与另外四个压缩旋钮的区别：那四个改的是**判据**（要不要压），这个改的是
   * **压成哪些字节** —— 压这件事照样发生。所以档位进的是变体缓存的**键**
   * （见 `apply_compression_for()` 里造键那一段），而那四个走的是"改完要么
   * 提前返回、要么查不到"。
   *
   * 也可以**在服务跑着的时候改**：成员是原子的，改完下一个请求就按新档位来。
   * 这既是它和那四个的又一处不同，也是"档位进键"能被独立观测到的前提 ——
   * 换档之后旧档位那条**仍然留在表里**（键不同），换回去还能拿回它自己的字节。
   * 取值由 `http_compress::clamp_compress_level()` 夹进合法区间，越界不报错。
   *
   * **只对 gzip / deflate 有效。** `br` 的档位是 brotli 质量，量纲与取值区间都
   * 不同（`-1..9` 对 `0..11`），本类不为它开第二个旋钮：`br` 一律用
   * `http_compress::k_brotli_quality_default`，理由（为什么是 5、为什么这个默认
   * 值本身就是算出来的极大点）写在 `uvcpp_http_compress.h` 那段表里。
   * 于是 `br` 变体的键里那个档位字符是个常量 —— 单射照样成立（见
   * `compress_level_key_char`），只是换档位会把 `br` 那几条也一并作废、重压一遍。
   */
  void set_compress_level(int level);
  int compress_level() const;
#endif

  /**
   * @brief 压缩变体缓存的只读计数。
   *
   * 存在的理由与 `uvcpp_tcp_client::try_write_stats()` 逐字相同：没有它，
   * "缓存根本没被用上"与"缓存命中了"在用例里**完全同形** —— 两种情况发出去
   * 的字节一模一样（那正是缓存的定义），只比响应体的话，把缓存整个拆掉也全绿。
   *
   * 与 `apply_compression` 一样声明上不设 `UVCPP_ZLIB_ENABLE` 条件（zlib 关掉
   * 时返回全零），这样调用方不必跟着条件编译。
   */
  struct compress_variant_stat {
    uint64_t hits   = 0;  ///< 查表命中，省掉了一次 deflate
    uint64_t misses = 0;  ///< 带 cache_tag 但表里没有（压完后按需存入）
    uint64_t stored = 0;  ///< 实际存进表的次数（不可压、压完更大时不存）
    size_t   entries = 0; ///< 当前表内条数
    size_t   bytes   = 0; ///< 当前表内字节
  };
  compress_variant_stat compress_variant_stats() const;

  // -------------------------------------------------------------------
  // Loop control
  // -------------------------------------------------------------------

  /**
   * @brief 跑循环。**两种传输共用这一条循环**（见 @ref listen_quic），所以
   *        TCP 与 QUIC 两个端口都由这一句转起来，不需要为 h3 另调一次。
   */
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  /**
   * @brief 停机：TCP 那条腿收尾，h3 那条腿的循环也停掉。
   *
   * @note **只开 h3 也必须调它。** `uvcpp_tcp_server::stop()` 在"没在 listen"
   *       时是同步早返回的 —— QUIC-only 的服务端正好是那个形状，于是
   *       "h3 那边没人停循环"会让 `run()` 永远不回来。这一条的处置写在本函数
   *       的实现里（两个 server 都要停），调用方不必自己补。
   */
  void stop(std::function<void()> on_stopped = nullptr);

  // -------------------------------------------------------------------
  // Status / accessors
  // -------------------------------------------------------------------

  int get_status() const;
  bool has_status(int flags) const;
  uvcpp_tcp_server* get_tcp_server();

  /**
   * @brief Send a fully-populated response after a delay (deferred response).
   *
   * The handler calls this on the loop thread (e.g. from an async completion
   * callback) instead of letting on_request_complete send immediately. The
   * handler must have set resp.deferred = true so on_request_complete skips
   * its automatic send. Semantics match the inline send in on_request_complete:
   * apply compression, set keep-alive/close header, serialize, and write back.
   *
   * The write is asynchronous and returns immediately — the caller must not
   * assume the bytes have left by the time this returns, and must keep @p resp
   * alive only until the call returns. When the response is not keep-alive the
   * connection is closed once its write completes.
   *
   * **`resp` 的内容在这一句之后不保证还在**：头/体分开走（`nbufs = 2`，见
   * `enqueue_write`）时体会被**移动**出去 —— 移动的是句柄不是字节，所以不产生
   * 拷贝，但 `resp.body` 之后是空的。要记"上线了多少 body"就用本函数的返回值，
   * **不要**回头读 `resp.body.size()`（那正是返回值取代掉的那次读）。
   *
   * @param close_after_write whether to close the connection after the write
   *        when not keep-alive. Default true (same as a normal request).
   *        A streaming endpoint (writing the body in chunks AFTER the header)
   *        must pass false and close the connection itself once the body is done.
   * @return 这次交给写路径的 body 字节数（压缩之后、HEAD 时 `resp.body` 已在
   *         序列化前清空所以为 0），或早退时为 0。
   */
  size_t send_response(uvcpp_tcp_client* client, uvcpp_http_response& resp,
                       bool close_after_write = true);

  /**
   * @brief 流式响应：只把**头部**入队，body 由调用方随后分块送。
   *
   * 与 `send_response()` 的分工是**结构性**的，不是参数式的：本函数
   * **不走压缩**（`apply_compression` 根本不被调用），因为此时 body 还不存在，
   * 而按 `content_type()` 判定并打上 `content-encoding: gzip` 等于**声明了一件
   * 没发生的事**。真正的守卫是"哪条函数在跑"，`apply_compression` 里那句
   * `transfer-encoding` 早返回是第二道。
   *
   * 头部里**不补** `content-length: 0`，也**不**动 `content-length` /
   * `transfer-encoding` —— 长度语义由调用方在两个入口之间选：
   * 长度已知就自己设 `content-length`，未知就设 `transfer-encoding: chunked`
   * （chunked 的组帧同样由调用方负责，本层不猜）。
   *
   * 调用之后这条连接进入**流式模式**：写队列排空**不会**触发关闭，直到
   * `end_stream()` 把模式关掉。HEAD 请求走的就是 `to_string(false)`，
   * 所以头部与 GET 逐字节相同而 body 一个字节都不发。
   */
  void begin_stream(uvcpp_tcp_client* client, uvcpp_http_response& resp);

  /**
   * @brief 流式头部，**指名是哪条流**。h2 下的正路。
   *
   * h2 上一条连接上可以同时有好几条流式响应，所以"这条连接正在流"不是个
   * 充分的身份 —— 必须带 `stream_id`。`stream_id == 0` 即 HTTP/1.1，走
   * 上面那个两参版本。
   *
   * 与 h1 的三点差别（都是 h2 的协议事实，不是取舍）：
   *   - 头部**不补** `transfer-encoding`：那个字段在 h2 里根本不合法；
   *   - 长度由"直到 END_STREAM"表达，所以调用方**不必**设 `content-length`；
   *     设了也照发（已知长度时那是有用信息），但不会被校验；
   *   - 不走 `to_string()`，由会话层折成 HEADERS 帧。
   */
  void begin_stream(uvcpp_tcp_client* client, int32_t stream_id,
                    uvcpp_http_response& resp);

  /**
   * @brief 流式写一块字节（调用方**自己组好帧**：chunked 的 hex 前缀、
   *        CRLF、终止块都不由本层添加）。
   *
   * @param done 这一块**真正写完**之后调用一次，参数是写的状态（0 = 成功）。
   *             **入队之后保证恰好一次**，包括连接在本块还排在队列里时就被关
   *             的场合（那时状态非 0，由收尾路径统一唤醒 —— 见
   *             `queued_done_cancelled_on_close`）。一个永远不回调的 `done`
   *             会让等它的人（`uvcpp_web_response` 的 `pending_bytes_`）永久
   *             挂起。
   *             **但只有 `return 0` 才作这个保证**：见返回值那行。
   * @return 0 = 已入队（`done` 会被调，恰好一次）；非 0 = 连接已不再受管，
   *         **`done` 不会被调用**，调用方必须自己按失败结算这一块
   *         （`uvcpp_web_response::flush_stream` 就是这么做的）。
   *         **非 0 时不要指望 `done` 兜底，也不要两边都调** —— 第二次结算
   *         可能落在响应对象已经被销毁之后。
   */
  int write_stream(uvcpp_tcp_client* client, std::string bytes,
                   std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief 流式写一块字节，**指名是哪条流**。h2 下的正路。
   *
   * @param bytes 在 h2 上是**裸 body 字节**，不是 chunked 帧 —— h2 的分块由
   *              DATA 帧本身承担，没有 hex 长度前缀那回事。调用方在两条路上
   *              给的东西因此不一样，这一点由调用方按 `stream_id != 0` 区分。
   * @param done  与 h1 逐条相同：**只有返回 0 才保证恰好回调一次**，而且
   *              **绝不在本函数返回之前同步跑**（h2 侧由写完成路径执行）。
   */
  int write_stream(uvcpp_tcp_client* client, int32_t stream_id,
                   std::string bytes,
                   std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief 结束流式响应。
   *
   * `close_after` 为真时，队列排空后关闭连接（这也是唯一诚实的收尾方式：
   * 中途失败时头部已经上线、状态码改不了了，只能截断 body 并关连接，
   * 让客户端从"长度对不上"看出传输失败）。
   */
  void end_stream(uvcpp_tcp_client* client, bool close_after);

  /**
   * @brief 结束一条流的流式响应。h2 下的正路。
   *
   * h2 里 `close_after` **不关连接**而只发 END_STREAM：连接是长命的，为一条
   * 流去关掉整条连接会把同一条连接上别的流一起打断，那不是调用方要的语义。
   * 要关连接请用 `close_connection()`。
   */
  void end_stream(uvcpp_tcp_client* client, int32_t stream_id, bool close_after);

  /**
   * @brief 用**尾随头**结束一条流：同一条流上的第二个头块，自带 END_STREAM。
   *
   * 这是 `end_stream(client, stream_id, false)` 的"带字段"版 —— 两者都是"这条流
   * 到此为止"，但只有它能带字段。gRPC 就靠它：`grpc-status` / `grpc-message`
   * 按规范走尾随头，而不是首部（RFC 9113 §8.1）。
   *
   * **只能用在 h2 上**，而且**必须在这条流的 body 一个字节都不欠的时候**调：
   * `begin_stream()` → `write_stream()`（等它的 `done` 回来）→ 本函数。还有字节
   * 在飞时返 `UV_EBUSY`（理由见 `uvcpp_h2_session::submit_trailer()` —— 那会发出
   * 一个"END_STREAM 在前、字节在后"的协议违例）。h1 上返 `UV_ENOTSUP`：h1 的
   * 尾随头要 chunked 编码，本库没做那条路，说清楚比默默发出去强。
   *
   * @param trailers 尾随头字段。**伪首部（`:` 开头）一律拒**（`UV_EINVAL`），
   *        连接专属头按首部那条路的规矩剥掉，与 `on_trailer()` 收到的形状一致。
   *
   * @return 0 受理；`UV_ECANCELED` 连接已经不在表里（或正在关，与
   *         `write_stream()` 同一套口径）；`UV_EINVAL` 字段非法；`UV_EBUSY` 还有 body
   *         欠着；`UV_EALREADY` 这条流的状态不允许（首部还没发、或者已经收过
   *         尾了）；`UV_EMSGSIZE` 头块超上限；`UV_ENOTSUP` 走了 h1 那条腿。
   *         **每一种失败都保证"什么都没发生"。**
   */
  int send_trailers(uvcpp_tcp_client* client, int32_t stream_id,
                    const http_headers& trailers);

  /**
   * @brief Register a callback fired when a connection is removed (closed).
   *        Lets a streaming handler free per-connection state (open file, queued
   *        buffers) on an abrupt disconnect where no END event will fire.
   */
  void on_connection_close(std::function<void(uvcpp_tcp_client*)> cb);

  /**
   * @brief 注册「新连接被接受」回调 —— **早于该连接上的任何请求**。
   *
   * 给上层框架用的入口：要在连接级别做簿记（发连接 id、记对端地址、按连接
   * 计数）的场合，这是唯一能在第一个请求之前动手的地方。回调里抛异常会被
   * 吞掉并打一条 stderr，不影响这个连接继续服务。
   *
   * 钩子跑在该连接**已登记之后**，所以回调里 `connection_generation(client)`
   * 已经是这条连接的有效代次号（可以用来把上层 id 和底层连接对上）。
   *
   * @warning 在 loop 线程上调用；**不要**在这里做耗时操作。也**不要**在这里
   *          自己 `read_start()` —— 读路径由本服务器接管（`set_raw_data_hook`
   *          是观察原始字节的正规入口）。
   */
  void on_connection(std::function<void(uvcpp_tcp_client*)> cb);

  /**
   * @brief 这条连接当前的身份代号；**未连接返回 0**。
   *
   * 存在的唯一理由是让**异步**工作能安全地确认"我要写的还是当初那条连接"。
   * 光有 `uvcpp_tcp_client*` 回答不了这个问题：连接关闭后对象被 delete，新连接
   * 完全可能分配在同一个地址上，于是"地址一样"既可能是同一条连接，也可能是
   * 另一条 —— 拿着旧指针往新连接上写，就是把 A 的响应发给 B。
   *
   * 代次号**单调递增、永不复用**，所以正确的用法是：
   * @code
   *   const uint64_t gen = server->connection_generation(client);  // 开始时记下
   *   // ... 异步读盘/查库 ...
   *   // 完成回调里（loop 线程）：
   *   if (server->connection_generation(client) != gen) return;    // 换人了，丢弃
   * @endcode
   * 地址被新连接复用时 `gen` 对不上（新连接是新号），所以丢弃；连接还活着时
   * 号不变，正常发送。
   *
   * @note 返回 0 表示"这条连接不在服务器的登记表里"，此时**不该**写它。
   */
  uint64_t connection_generation(uvcpp_tcp_client* client) const;

  /** @brief 这条连接是否还挂在服务器上（= `connection_generation() != 0`）。 */
  bool is_connected(uvcpp_tcp_client* client) const;

  // -------------------------------------------------------------------
  // 稍后回话（推迟应答）
  // -------------------------------------------------------------------

  /**
   * @brief 给 h1/h2 这条连接造一枚「稍后回话」凭据 —— 见 @ref uvcpp_http_reply。
   *
   * h3 上**用不到它**：那条腿上处理函数手里没有 `uvcpp_tcp_client*`，凭据由
   * 派发那一步直接填进 `req.reply`。三条腿上处理函数要的东西因此是同一个：
   * 读 `req.reply`。
   *
   * @param client    那条 TCP 连接；**不在本服务器的登记表里就返回无效凭据**。
   * @param stream_id 这条请求的 h2 流号；h1 上传 0。
   */
  uvcpp_http_reply reply_of(uvcpp_tcp_client* client,
                            int32_t stream_id = 0) const;

  /**
   * @brief 这枚凭据**现在**还能用来回话吗。
   *
   * 判据是代次对得上：连接还活着、且没被另一条连接复用那一格。连接断开、服务器
   * 停了、h3 的连接收尾了 —— 都会让它变假。**异步路径上写之前必须问一次**；
   * `send_reply()` 自己也查，所以漏问的后果是"响应被丢掉"，不是"发给别人"。
   */
  bool reply_alive(const uvcpp_http_reply& reply) const;

  /**
   * @brief 凭一枚凭据把响应送出去 —— 「推迟应答」那条路的收口。
   *
   * 与 `send_response()` 的分工：那个收的是 `uvcpp_tcp_client*`（只有 h1/h2 有
   * 这东西），这个收的是凭据（三条腿都有）。h1/h2 上两者等价；h3 上本函数是**唯一**
   * 能稍后回话的入口。
   *
   * 必须在 `reply.loop()` 那条循环的线程上调用。与 `send_response()` 一样，
   * 返回之后**不保证** `resp` 还完整（body 可能已被移动出去）；要记上线了多少
   * body 就用返回值。
   *
   * @return 交出去的 body 字节数；凭据无效/连接换人/这条流已经收场时返回负值
   *         （`UV_EINVAL` / `UV_ENOENT`），且**一个字节都不发**。
   */
  int send_reply(const uvcpp_http_reply& reply, uvcpp_http_response& resp);

 private:
  // -------------------------------------------------------------------
  // Route matching
  // -------------------------------------------------------------------

  struct route_entry {
    http_method method;
    std::string path;
    http_request_handler handler;
    http_stream_handler stream_handler;  // set for streaming routes (empty otherwise)
  };

  http_request_handler find_handler(http_method method, const std::string& path);
  http_stream_handler find_stream_handler(http_method method, const std::string& path);

  // -------------------------------------------------------------------
  // Connection handling
  // -------------------------------------------------------------------

  /** @brief Called when a TCP connection is accepted. */
  void on_tcp_connection(uvcpp_tcp_client* client);

  /**
   * @brief Called when data arrives on a connection.
   *
   * `data` 只在**本次调用期间**有效（框架层读的契约，见 `uvcpp_net_read.h`）：
   * 解析器就地吃、升级残留当场拷走，本函数返回之后一个字节都不能再用。
   */
  void on_connection_data(uvcpp_tcp_client* client, const char* data, size_t len);

  /** @brief Called when a full HTTP request has been parsed. */
  void on_request_complete(uvcpp_tcp_client* client);

  // -------------------------------------------------------------------
  // Per-connection parser context
  // -------------------------------------------------------------------

  /**
   * @brief 一块待写出的字节 + 它写完之后的回调。
   *
   * `done` 可空（普通响应用不上它），实现上只在**流式写**里被填。
   *
   * `body` 是 #17 加的**第二块**：非空时 `bytes` 只是**头部**，体在 `body` 里
   * 且**不拷**，两块一起交给 `uv_write`（`nbufs = 2`）。为什么要分成两块：一条
   * 响应天然是两段，先合并成一条再发就要把体拷一遍；分开写则由写请求持有第二块
   * 直到完成回调（libuv 的契约是缓冲要活到回调）。
   *
   * 用 `uvcpp_buf` 装而不是裸指针：它既能装**共享视图**（静态层整份命中那份），
   * 也能装**自有块**（压缩产出那份），两种都由写请求按同一条契约接手。能用
   * `uvcpp_buf` 按值放进队列，前提是它**有移动语义**（见 `uvcpp_buf` 里的说明：
   * 少了移动构造，`std::move` 会静默退化成深拷贝，那正是这里要消掉的一份）。
   */
  struct queued_write {
    std::string bytes;
    uvcpp_buf body;
    bool has_body = false;
    std::function<void(int)> done;
  };

  /**
   * @brief 一次性结算器：**在途**那一块的 `done` 由它持有。
   *
   * 排队中的块由 `write_queue` 持有，连接关掉时 `close_connection` /
   * `remove_ctx` 能把它们逐个唤醒。**在途那一块不在队列里** —— 它的 `done`
   * 已经交给 `uvcpp_tcp_client::write` 的完成回调了，而那个回调有一条早返回：
   *
   *     if (!token_alive(life)) { delete wr; return; }   // 客户端已析构
   *
   * 服务端连接是**被 tcp_server 的 close manager 删掉**的（`fire_close_callbacks`
   * 里，而它由对端 EOF 直接触发），于是"对端在写入途中断开"这条最普通的路
   * 会让在途那一块的 `done` **永远不响** —— 等它的人（`send_file` 的传输对象）
   * 连同 fd 和缓冲一起永久挂着。
   *
   * 所以这一块的结算权归 `conn_ctx`：完成回调和关闭路径**都**可以唤醒它，而
   * `fired` 保证恰好一次。谁先来谁说了算 —— 正常路径上先到的是完成回调，
   * 于是调用方仍拿到真实的写错误（`ECONNRESET` 之类）而不是一律的
   * `UV_ECANCELED`。
   */
  struct write_done {
    std::function<void(int)> fn;
    bool fired;
    write_done() : fired(false) {}
  };

  /** @brief 唤醒一个结算器；已经响过就什么都不做（幂等）。 */
  void fire_write_done(const std::shared_ptr<write_done>& d, int status);

  /**
   * @brief 一次写完成后的**唯一**结算路径：一块那条路与两块那条路共用它。
   *
   * 「重入次序铁律」就落在这个函数里（`start_write` 那段注释）：只在这一步碰
   * `contexts_`，把要用的值拷出来再离开；不持有任何引用时调用 `done`；之后再
   * 重新 find 决定下一步。抽成函数是因为**两条路必须逐字同一份**逻辑 ——
   * 抄成两份就是给"以后只改了一份"留口子（本仓的 ws 那族就是这么来的）。
   *
   * @param recycle 复用形态那个写请求（见 @ref conn_ctx::wrecycle）；一元素形态
   *                传 nullptr。非空时在本函数里还回 `ctx.wrecycle`，或按
   *                "表项已经没了"那条路直接放掉。
   */
  void settle_write(uvcpp_tcp_client* client,
                    const std::shared_ptr<write_done>& wd,
                    uvcpp_write* recycle, int status);

  /**
   * @brief 复用形态的写完成回调：**无捕获 ⇒ 函数指针**，`std::function` 就地
   *        存储，这一笔的堆分配就是这么省掉的（见 @ref settle_write）。
   */
  static void recycle_write_done(uvcpp_write* w, uvcpp_tcp_client* client,
                                 int status, void* arg);

  struct conn_ctx {
    uvcpp_http_parser* parser = nullptr;

#if UVCPP_NGHTTP2_ENABLE
    /**
     * @brief 这条连接上的 h2 驱动层；非 h2 连接恒为 nullptr。
     *
     * 与 @ref parser **互斥**：h2 连接没有 llhttp 解析器，h1 连接没有 h2 层。
     * 所有原先只看 `parser` 的地方都要先判这条连接走的是哪条路。
     */
    uvcpp_h2_connection* h2 = nullptr;

    /**
     * @brief h2 下**按流**记的状态，键是 stream_id。
     *
     * 不能复用 `body_buf` / `body_overflow` / `is_head` / `accept_encoding` 那四个
     * **连接级**单槽：h2 的一条连接上并发跑着多条流，共用一个标量不是"少判一次"
     * 而是**判错**。三类后果的触发难度不一样，分开写清楚：
     *
     * - **`is_head` / `accept_encoding` —— 确定性的**，只要有延迟应答就必然踩到。
     *   `dispatch_h2_request` 按到达序写这两个字段，`send_h2_response` 按**发送**序
     *   读它们，两者在并发流上不是一个顺序。于是"流 1 是 GET（延迟）、流 2 是
     *   HEAD"会让流 1 的响应被当成 HEAD **掐掉 body**；同构地，流 1 带
     *   `accept-encoding: gzip` 而流 2 不带，流 1 的响应会**不压缩**发出去。
     *   这正是 `web_ssl_h2_server_func.cpp` 场景 7 钉的两条。
     *
     * - **`body_overflow` —— 需要客户端配合一个特定交织。** 旧代码里
     *   `on_request`（**任何**流）会把标志清成 false，所以要踩到它，流 B 的请求头
     *   必须落在"流 A 已经溢出"和"流 A 的 END_STREAM"之间，**并且** A 剩下的
     *   数据块每一块都 ≤ `max_body_size_`（旧 `on_body` 溢出时把 body 清空，之后
     *   每来一块都拿空 buffer 重新比，块一大就又把标志置回去）。交织本身在
     *   `mem_recv` 里是合法的 —— 帧按线序逐帧处理，B 的 HEADERS 完全可以卡在
     *   A 的两个 DATA 帧中间。所以它是**真缺陷**，只是不像上面两条那样随手可复现。
     *
     * 生命周期 = 流的生命周期：请求头到达时建立，`on_close` 时删除。**不能**在
     * 请求收完时删 —— 延迟应答（`resp.deferred`）还要回来读 `is_head`。
     */
    struct h2_stream_state {
      std::string body;             ///< 收集中；请求收完时被 swap 走
      std::string accept_encoding;  ///< 派发时从这条流自己的请求头里取
      bool        overflow = false; ///< 这条流超过了 max_body_size_
      bool        is_head  = false; ///< 这条流是 HEAD
    };
    std::map<int32_t, h2_stream_state> h2_streams;

    /**
     * @brief 明文连接上「这条是不是 h2c」的判定状态。
     *
     * 判据是 RFC 9113 §3.4 的**先验知识**（prior knowledge）：第一段字节与固定的
     * 24 字节前言逐字节相同 ⇒ h2，否则 ⇒ h1。**不是嗅探** —— 两边都由确定的字节
     * 决定，没有"看着像"。所以这个状态机只有三个态，且一旦离开 @ref PROBING 就
     * 再也不回头。
     *
     * - @ref PROBING：还没收够 24 字节，也没比岔。这个态下 @ref h2c_probe 攒着
     *   已经收下的字节，**解析器一个字节都没吃到** —— 那批字节可能整个属于前言，
     *   喂给 llhttp 就是"把一条合法 h2 连接打成畸形请求"。
     * - @ref H1：比岔了。此后这条连接上与 h2c 有关的代码一行都不再跑。
     * - @ref H2：24 字节全中。此刻 `conn_ctx` 会被整个换掉（见
     *   `switch_to_h2c()`），所以这个态只在不换表那条路上（单测直接观察）有意义。
     *
     * 只有 `set_h2c_enabled(true)` 且**明文**连接才会离开 @ref PROBING —— 默认
     * 配置下这个字段恒为 @ref PROBING 且没人读它。
     */
    enum class h2c_probe_state { PROBING, H1, H2 };
    h2c_probe_state h2c = h2c_probe_state::PROBING;

    /**
     * @brief @ref PROBING 期间攒下的字节，长度 `0..23`。
     *
     * 必须攒：TCP 会把 24 字节前言切成任意几段（`"PRI * HT"` + `"TP/2.0\r\n\r\nSM\r\n\r\n"`
     * 是合法的到达方式），而"比岔了"的那一刻得把**前面几段**按原样补喂给解析器
     * —— 它们是用户请求的一部分，丢了就是一条被静默吞掉的请求。
     */
    std::string h2c_probe;
#endif

    /**
     * @brief Connection generation — monotonic, never reused. 0 = never assigned.
     *
     * Exists because a `uvcpp_tcp_client*` alone cannot answer "is this still
     * the same connection?". Once a connection closes, its client object is
     * deleted and a NEW connection can be allocated at the same address, so an
     * async completion holding only the pointer would happily write connection
     * A's file to connection B. An async operation captures the generation when
     * it starts and re-checks it before touching the socket; a recycled address
     * carries a different generation, so the write is dropped.
     *
     * This is the same idea as `uvcpp_web_conn_id` one layer up; the http layer
     * needs its own because handlers here get raw pointers, not contexts.
     */
    uint64_t generation = 0;

    /**
     * @brief 这条连接上**当前这条消息**的请求对象（h1 累积那条路）。
     *
     * ★ 它同时是**头表的搬移目的地**（`uvcpp_http_parser::take_headers_into()`）：
     * 目的地必须活得比请求长，那块 vector 的容量才能跨请求留下 —— 这就是它挂在
     * 连接上、而不是 `on_request_complete` 里一个局部变量的全部理由。请求跑完时
     * 元素已经被 `uvcpp_web_request::take_from()` 搬走，这里只剩**空表 + 容量**。
     *
     * 交给处理函数的就是**这一个对象**（不再是每请求一个局部）。处理函数返回之后
     * 不得再读它 —— 下一条请求会在原地重填。这条约束改前也有（那个局部对象在
     * `on_request_complete` 返回时同样死了），变的是违反之后的**症状**：改前是读
     * 悬垂内存（多半崩），现在是**静默读到下一条请求的内容**。
     */
    uvcpp_http_request request;
    uvcpp_buf body_buf;
    bool headers_done = false;
    bool msg_done = false;
    http_stream_handler stream_handler;  // non-empty once a stream route matched
    /**
     * @brief 请求视图；认领钩子与流式处理函数拿到的就是它。
     *
     * 它同样是**连接级**的：一是"视图只建一次、完成时接着用"（见
     * @ref stream_view_built），二是这条路上的头表**搬进它**（`take_headers_into()`）
     * —— 容量因此也跨请求留下。复位的两处（`on_message_begin` 与流式 END）都必须
     * 只清内容、不还缓冲。
     */
    uvcpp_http_request stream_request;

    /**
     * @brief Whether @ref stream_request was built for this message.
     *
     * A claim hook (the framework installs one for every connection) forces the
     * headers to be copied at headers time even though most requests are never
     * claimed. Without this flag, on_request_complete would rebuild the same
     * request — paying a second full copy of the header vector per request.
     * When set, the completion path reuses @ref stream_request instead.
     */
    bool stream_view_built = false;

    // --- Body accounting for the current message ---
    size_t body_bytes = 0;        // body bytes seen so far
    bool body_overflow = false;   // exceeded max_body_size_; buffering stopped

    /**
     * @brief A response was already sent during the headers callback.
     *
     * Set by the early-rejection paths (417 unknown Expect, 413 by declared
     * Content-Length). The message is still going to be parsed to completion —
     * we cannot make llhttp skip the body — so every later stage has to keep
     * quiet: on_request_complete must not route it (that would send a second
     * response, and the body_overflow branch would send a third), and the body
     * callbacks must not accumulate anything.
     */
    bool rejected = false;

    /**
     * @brief Close the connection once this message ends, not right now.
     *
     * The early rejections answer while the client is still sending the body.
     * Closing at that moment is a real problem on Windows: closing a socket
     * that still has unread data in its receive buffer makes the stack send an
     * RST, and an RST discards whatever the peer has buffered but not yet read
     * — including the 413 we just wrote. So the write is queued, the flag is
     * set, and the close happens in on_request_complete, once the client has
     * stopped sending.
     */
    bool close_after_message = false;

    /**
     * @brief A close was decided while the body was still arriving; do it at
     *        message end instead.
     *
     * @ref close_after_message is set by the headers-time rejections, which know
     * up front that they are answering early. This flag is the *runtime*
     * version of the same rule: a claimed stream handler is free to answer (and
     * ask for the connection to close) from any BODY callback, since it may
     * decide to stop reading an upload the moment it sees enough of it. That
     * response is written while the peer is still sending, so closing right
     * after the write hits exactly the Windows RST problem described on
     * @ref close_after_message — the response we just wrote sits unread in the
     * peer's receive buffer and an RST discards it.
     *
     * pump_write() therefore parks the close here and on_request_complete()
     * honours it once the parser reports the message complete (the peer has
     * stopped sending by then). Only ever set while @ref stream_handler is
     * installed, so a malformed message — which never completes and so has no
     * one to hand the close to — can still close immediately.
     */
    bool defer_close_to_message_end = false;

    /**
     * @brief The message carried `Expect: 100-continue` (HTTP/1.1 only).
     *
     * Recorded at headers time and acted on there — the interim response is
     * queued before the parser returns, so the client can start sending the
     * body without waiting for its own timer.
     */
    bool expect_continue = false;

    /**
     * @brief 这条连接正在升级（`upgrade_handler_` 已经接走）。
     *
     * 升级请求和第一帧常常挤在同一个 TCP 段里到达：HTTP 解析器吃完请求就把
     * 剩下的字节留给了"下一条消息"，而升级之后根本没有下一条 HTTP 消息 ——
     * 那批字节必须交给新协议，否则升级方看到的第一个 WS 帧凭空消失。升级回调
     * 是在 `execute()` **里面**同步调的（那时还不知道剩下多少），所以先立这个
     * 旗标，等 `execute()` 返回后在 @ref pending 里存下来。
     */
    bool upgrading = false;

    /**
     * @brief 升级请求之后多余的那批字节，等升级方来取
     *        （`uvcpp_http_server::take_upgrade_leftover`）。
     */
    std::string pending;

    // --- Per-message facts captured at request completion ---
    // The parser is reset lazily on the next inbound chunk, so anything
    // send_response() needs later must be copied out here rather than read
    // back off the parser.
    std::string accept_encoding;  // client's Accept-Encoding for this message
    bool is_head = false;         // this message was a HEAD request

    // --- Outbound write serialization ---
    // uvcpp_tcp_client permits only one async write in flight at a time, so
    // responses are queued. Draining one queued write at a time would make a
    // pipelined burst cost one syscall (and one packet) per response, so
    // pump_write() gathers a batch of them into a single write instead — see
    // kCoalesceMaxBytes in the .cpp for why that is a per-syscall win.
    bool write_pending = false;
    std::deque<queued_write> write_queue;

    /**
     * @brief **在途**那一块的结算器（见 @ref write_done）。队列非空或正在写
     * 时它必然非空 —— 它是关闭路径唤醒在途那一块的唯一入口。
     */
    std::shared_ptr<write_done> inflight;

    /**
     * @brief 按连接复用的那个写请求（@ref uvcpp_tcp_client::write_owned）。
     *
     * 稳态下这里是 **nullptr**：`start_write` 取走它、写完后 `settle_write`
     * 还回来，中间只在"写请求正在网络层手里"那一刻不在这格。所以它既不与
     * @ref inflight 重叠，也不与 @ref write_queue 重叠。
     *
     * **只有这一张表认它**（库里只在完成回调那一刻认），所以两个抹除表项的地方
     * 都必须放掉它：`remove_ctx`（对端断开、关闭收尾都经它）与析构函数里那个
     * `tbl.clear()` 循环。少一处就是每条连接漏一个写请求对象加它的头部缓冲。
     *
     * 普通响应走这条路是因为它**一次堆分配都不发生**：请求对象（连同里面的
     * `uv_write_t`）与头部缓冲都按连接复用，完成回调是个函数指针。有 `done`
     * 的调用点仍走原来那条 —— 结算器要唤醒那个闭包，而闭包的去向只有它知道。
     */
    uvcpp_write* wrecycle = nullptr;
    std::string wire_recycle;  // 响应序列化的复用缓冲：容量跨请求留下（见 uvcpp_http_response::to_string_into）

    // 响应**头表**的复用槽：与上面那条同一个理由，只是那份容量在头表的向量里。
    // 一次交换就能让「表长到 4~5 个位置」这条路上的一次 `reserve` 加一到两次
    // `_M_realloc_insert` 全部消失（`probe_h` 的调用点普查：这几笔合起来
    // 5.00 次/请求）。认领在 `on_request_complete` 造出 `resp` 之后、处理函数
    // 之前；归还在 `send_response` 的 h1 出口。
    http_headers resp_hdr_recycle;

    /**
     * @brief 谁把 `resp_hdr_recycle` 那块缓冲**领走了**（`nullptr` = 没人领）。
     *
     * 必须记这一笔，是因为归还点 `send_response(client, resp)` 收的是**引用**
     * —— 而交进来的那个响应对象**不一定是领走缓冲的那一个**。webapp 那条路
     * 就是反例：`on_request_complete()` 在自己那个局部 `resp` 上认领，随后因为
     * `resp.deferred` 为真直接返回（那一位由 webapp 在 `uvcpp_web_app.cpp` 设，
     * 它只在"派发那一刻传进来的那个对象"上），真正被发送的是 **webapp 自己**
     * 手上那个对象（`http_->send_response(client, r.raw())`，同一个文件）。
     *
     * 这一层没加之前，`send_response()` 无条件 `yield_tables()` ⇒ 它把 webapp
     * 手上那块**有容量**的表换进本槽，同时塞给 webapp 一块**容量 0** 的表；
     * 紧接着 `uvcpp_web_app::context_finished()` 又把那块容量 0 的表收进
     * `resp_recycle` ⇒ **回收槽每请求被毒化一次**、下一条请求的表从 0 重新长
     * （`reserve(4)` 一次 + 撑过 4 条时再 `_M_realloc_insert` 一次）。实测
     * （验收路由 `/`、C=400/T=4）：`set_loops(1)` **5.04 → 3.03**、
     * `set_loops(4)` **5.01 → 3.01**。而本槽在 **http 层**上原有的收益
     * （`probe_h` 7.00 → 4.00）一分不动 —— 两种路径的差别只是"谁领的谁还"。
     */
    uvcpp_http_response* resp_hdr_owner = nullptr;
    bool close_requested = false;  // a response asked for close after its write
    bool closing = false;          // a close has already been issued

    /**
     * @brief 这条连接正处于**出站**流式响应中（`begin_stream` 到 `end_stream`）。
     *
     * 与入站的 @ref stream_handler 是两码事：那个是"请求体正在流进来"，
     * 这个是"响应体正在流出去"。存在的唯一理由是让 pump_write 在队列**暂时**
     * 排空时不要收尾 —— 头部写完到第一块 body 入队之间总有一个空窗，
     * 没有这个标志，一个非 keep-alive 的流会在第一块 body 之前就被关掉。
     */
    bool out_streaming = false;
  };

  /** @brief Queue bytes for the connection, draining as the socket permits. */
  void enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client, const std::string& wire,
                     std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief 头部与体**分开**入队（体不拷），出队时按 `nbufs = 2` 写出去。
   *
   * `head` 必须与 `body` 拼起来正好是那条报文 —— 这个前提由调用方保证
   * （今天只有 `send_response` 用，判据写在那里：`to_string(false)` 接上 body
   * 与 `to_string(true)` 逐字节相同）。`head` 按 const 引用收 —— 它可以指着跨请求复用的
   * @ref conn_ctx::wire_recycle（入队那条路会拷进 `qw.bytes`）；`body` 按值收、移动的是句柄。
   */
  void enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client, const std::string& head,
                     uvcpp_buf body,
                     std::function<void(int)> done = std::function<void(int)>());

  /**
   * @brief Drain the queue if the connection is idle: **the whole queue in one
   * write** when it fits, otherwise the front block alone.
   */
  void pump_write(uvcpp_tcp_client* client);

  /**
   * @brief @ref pump_write 的**已知上下文**版本 —— 省掉一次 `contexts_` 查找。
   *
   * `ctx` 必须就是 `contexts_` 里 `client` 那一项。调用方必须保证**从拿到 `ctx`
   * 到这一句之间没有跑过用户代码**：`close_connection` 会把表项摘掉，之后这个引用
   * 就悬垂了。`on_written` 尾部的调用处是现查现用，`end_stream` 与
   * `enqueue_write` 的调用处中间只有赋值。
   */
  void pump_write(conn_ctx& ctx, uvcpp_tcp_client* client);

  /**
   * @brief 把**已经拿在手上**的这一块立刻发出去（不经过 `write_queue`）。
   *
   * 前置条件：`ctx.write_pending == false && ctx.closing == false`。这两条成立时
   * 队列**必然**是空的 —— 往队列里放的只有 @ref enqueue_write，而它放完必定泵
   * 一次，于是"队列非空"永远蕴含"有一次写在途"。所以这条路与"入队再泵"逐字等价，
   * 省下的是每个响应一次 `queued_write` 构造 + 一次入队 + 一次出队 + 两次移动。
   *
   * `wire` 按 const 引用收（两条写路径都在返回之前把头部字节拷走）⇒ 可以指着跨请求
   * 复用的 `conn_ctx::wire_recycle`。`body` 为 `nullptr` 时走单块写；非空时那块的内容按
   * @ref uvcpp_tcp_client::write 的消费语义交出去（共享视图接引用计数、自有块接所有权），**返回后 `*body` 即可析构**。
   */
  void start_write(conn_ctx& ctx, uvcpp_tcp_client* client, const std::string& wire,
                   uvcpp_buf* body, bool has_body, std::function<void(int)> done);

  /** @brief Close a connection exactly once and release its context. */
  void close_connection(uvcpp_tcp_client* client);

  /** @brief Apply response compression in place; returns true if applied. */
  bool apply_compression(conn_ctx& ctx, uvcpp_http_response& resp);

  /**
   * @brief @ref apply_compression 的**按编码串取值**版本 —— 不读任何上下文。
   *
   * 压缩决策只用到请求的一个字段（`accept-encoding`），这一整段逻辑是**协议
   * 无关**的。h1 那条路那个字段在连接级单槽里（`conn_ctx::accept_encoding`），
   * h2 把它临时借进去，h3 则连一个可借的上下文字段都没有（它压根不复用
   * `conn_ctx`）—— 于是把实现搬到这里，三边共用同一份：**同一个处理函数在三条
   * 传输上的响应形状必须逐字相同**，抄成两份就是给"以后只改了一份"留口子。
   *
   * @param accept_encoding 这条**请求**的 `accept-encoding` 头值（空 = 没有）。
   */
  bool apply_compression_for(const std::string& accept_encoding,
                             uvcpp_http_response& resp);

  /**
   * @brief Answer during the headers callback and stop caring about the rest.
   *
   * Queues a `Connection: close` response, marks the context rejected so no
   * later stage answers again, and arranges for the close to happen at message
   * end rather than now (see @ref conn_ctx::close_after_message).
   *
   * @param message_will_complete false when the parse was abandoned mid-message
   *        (an oversized URL/header block), where "at message end" never comes
   *        and the close has to ride on the write instead — otherwise the
   *        connection sits there for good, because **this layer has no idle
   *        sweep**: the one named elsewhere is the webapp layer's (60 s default)
   *        and only exists when a webapp sits above this server.
   *        See @ref pump_write.
   */
  void reject_early(conn_ctx& ctx, uvcpp_tcp_client* client, http_status status,
                    bool message_will_complete = true);

  /**
   * @brief Validate `Expect` and record `100-continue` if asked for.
   *
   * @return false if the request was answered (417) and the caller must stop;
   *         true to carry on.
   *
   * HTTP/1.0 is exempt: Expect has no meaning there (RFC 7231 §5.1.1), and
   * failing a 1.0 message over a header it never defined would break clients
   * talking through proxies that forward it blindly.
   *
   * @param msg_headers 这条消息的头表。**必须是调用方手上那一份**：建请求视图
   *        的那两条路会把头从解析器上搬走（`take_headers_into()`），搬过之后
   *        `parser->get_headers()` 就是空表了。
   */
  bool check_expect_header(conn_ctx& ctx, uvcpp_tcp_client* client,
                           const http_headers& msg_headers);

  /**
   * @brief Refuse, before the body arrives, a message whose declared
   *        Content-Length is over @ref max_body_size_.
   *
   * Chunked messages have no declared length and cannot be refused this way;
   * they still hit the message-complete check once the cap is exceeded.
   *
   * @return false if the request was answered (413) and the caller must stop;
   *         true to carry on.
   */
  bool reject_oversized_declared(conn_ctx& ctx, uvcpp_tcp_client* client);

  void remove_ctx(uvcpp_tcp_client* client);

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

#if UVCPP_NGHTTP2_ENABLE
  /**
   * @brief h2 连接的分流入口（`on_tcp_connection` 里判完 ALPN 后进来）。
   *
   * @param run_connection_hook 要不要在登记之后跑 @ref on_connection 钩子。
   *        TLS 那条路（ALPN 判 h2）与明文那条被改判的路（`switch_to_h2c()`）
   *        在这里分叉：前者这条连接是**第一次**被登记，钩子必须跑；后者它已经
   *        按 h1 登记过一次、钩子也跑过了，再跑一次就是让调用方的每条连接簿记
   *        翻倍。**默认 true**，只有改判那条路传 false。
   */
  void on_tcp_connection_h2(uvcpp_tcp_client* client,
                            bool run_connection_hook = true);

  /**
   * @brief 把一条**已经按 h1 登记、已经起了 h1 的读**的明文连接改判成 h2。
   *
   * 只由 @ref on_connection_data 里那段前言比较在"24 字节全中"时调用。
   *
   * @param pending 探测期已经收下、**还没喂给任何解析器**的字节：24 字节前言本身，
   *                加上与它同段到达的那些（前言和第一帧挤在一个 TCP 段里是常态）。
   */
  void switch_to_h2c(uvcpp_tcp_client* client, const std::string& pending);

  /// 把一条**已收全**的 h2 请求交给路由，并在对应的流上回话。
  void dispatch_h2_request(uvcpp_tcp_client* client, int32_t stream_id,
                           uvcpp_http_request& req);
#endif

#if UVCPP_HTTP3_ENABLE
  // -------------------------------------------------------------------
  // HTTP/3 那条腿（QUIC/UDP）
  // -------------------------------------------------------------------

  /**
   * @brief 一条 h3 连接上属于**本服务端**的东西。
   *
   * 今天只有一格（h3 驱动层）。做成结构体而不是直接把 `uvcpp_h3_connection*`
   * 当值塞进表里，与 @ref conn_ctx 同一个形状：这张表的键是连接，将来要挂
   * 别的按连接的东西时不必改表。
   */
  struct quic_ctx {
    /// 这条 QUIC 连接上的 h3 驱动层；`remove_h3_ctx()` 之后为 nullptr。
    uvcpp_h3_connection* h3 = nullptr;

    /**
     * @brief 连接代次 —— 与 @ref conn_ctx::generation **同一个计数器**。
     *
     * 用途也逐字相同：一条连接收场、另一条新连接落在同一个地址上时，靠它把
     * "那枚凭据还是当初那条连接的吗"回答掉。h3 上没有别的东西能回答它：
     * `uvcpp_quic_connection*` 一样是会被复用的地址。
     */
    uint64_t generation = 0;

    /**
     * @brief 这条连接所在的那条循环（= 它那条端点的循环）。
     *
     * 留在这一格里，是因为"稍后回话"的凭据要带上"把工作投回哪条循环"，而
     * `uvcpp_quic_connection` 自己没有循环的入口。凭据只在**循环线程**上造，
     * 所以这一格与连接同生共死，不存在读到过期值的时候。
     */
    uvcpp_loop* loop = nullptr;

    /**
     * @brief 这条连接上正在跑的流 —— 每条一格，流收场时删。
     *
     * 镜像 @ref conn_ctx::h2_streams，但**只留推迟应答要用的那两样**：h3 的
     * `on_request` 拿到的是**已经收全**的请求（不需要攒 body，也没有溢出这一说），
     * 而 `send_h3_response()` 在推迟那条路上没有请求对象可读，得回来查这两样。
     *
     * 生命周期 = 流的生命周期：派发时建立，`on_stream_close` 时删除。**不能**
     * 在响应发出去时删 —— 推迟应答正是"发的时候才回来读它"。
     */
    struct h3_stream_state {
      std::string accept_encoding;  ///< 派发时从这条流自己的请求头里取
      bool        is_head = false;  ///< 这条流是 HEAD
    };
    std::map<int64_t, h3_stream_state> h3_streams;
  };
#endif  // UVCPP_HTTP3_ENABLE

  // -------------------------------------------------------------------
  // 凭据：就地把一枚 uvcpp_http_reply 填满
  // -------------------------------------------------------------------
  //
  // 这一段**不在上面那道 `#if` 里**：`fill_reply` 是 h1 与 h2 两条腿共用的，
  // 把它关进 h3 的守卫里会让 HTTP3=OFF 的配置编译不过（声明没了、定义还在）。
  // 下一道 `#if` 到本文件末尾为止，h3 那半边的声明都在里面。

  /**
   * @brief 填一枚 h1/h2 凭据（派发那两处的收口）。
   *
   * 与公开的 @ref reply_of 的差别只有一个：**不查表**。派发那两处手里已经有
   * `ctx` 了，而那是**每请求**的热路 —— 为了一个代次号再走一次 map 查找是白付
   * 的。`reply_of()` 那条路留给"手里只有 `client`"的调用方。
   */
  void fill_reply(uvcpp_http_reply& r, uvcpp_tcp_client* client,
                  const conn_ctx& ctx, int32_t stream_id) const;

#if UVCPP_HTTP3_ENABLE
  /** @brief @ref fill_reply 的 h3 版（那条腿上没有 `conn_ctx`）。 */
  void fill_h3_reply(uvcpp_http_reply& r, uvcpp_quic_connection* conn,
                     const quic_ctx& qc, int64_t stream_id) const;

  /**
   * @brief 本线程所在那条循环的 h3 登记表。**镜像 @ref ctxs_here 那一套。**
   *
   * "这条连接属于哪张表"不问连接 —— 问当前线程：QUIC 端点是**按循环建的**
   * （@ref listen_quic），所以一条连接的全部回调都在它自己那条循环的线程上跑，
   * 而 h3 层的回调又都是从那些回调里出来的。与 h1 那条腿必须给客户端存一个
   * `loop_index` 不同，这里连那个字段都省了。
   */
  std::map<uvcpp_quic_connection*, quic_ctx>& quic_ctxs_here();

  /**
   * @brief @ref quic_ctxs_here 的三件套取值。
   *
   * @warning **返回的迭代器只能与该表自己的 `end()` 比**：多循环下第 i 张与第 j
   *          张不是同一个容器，跨容器比较迭代器与 @ref ctxs_at 那条 `@warning`
   *          逐字同罪。
   */
  std::map<uvcpp_quic_connection*, quic_ctx>& quic_ctxs_at(int loop_index);

  /// @brief @ref quic_ctxs_at 的 const 版（`reply_alive()` 这类只读入口用）。
  const std::map<uvcpp_quic_connection*, quic_ctx>& quic_ctxs_at(
      int loop_index) const;

  /**
   * @brief 本线程所在那条循环的序号 —— 就是 @ref quic_ctxs_here 拿来取表的那个数。
   *
   * 存在的理由只有一个：连接计数表 `quic_conn_counts_` 是**按序号**存的，而取表
   * 那条路只回一张表、不回序号。两条路的判据必须一模一样（单循环一律算 0 号，
   * 多循环下不在任何循环线程上就 abort），所以**不写成两份**：这里判一次，
   * @ref quic_ctxs_here 也照同一条规矩走。
   */
  int quic_loop_index_here() const;

  /// 新到的 QUIC 连接（`uvcpp_quic_server::listen` 的回调里进来）。
  void on_quic_connection(uvcpp_quic_connection* conn);

  /**
   * @brief 把一条**已收全**的 h3 请求交给路由，并在它那条流上回话。
   *
   * `req` 是已经从 `h3_request` 转过来的 **web 层请求对象**（转换在
   * `on_quic_connection` 的回调里做，`h3_request` 因此不出现在本类的声明里）。
   * `req.version == HVER_30`、`req.stream_id` 是那条 h3 流 —— 一个处理函数
   * 靠这两样分辨自己在替哪条传输、哪条流回话。
   */
  void dispatch_h3_request(uvcpp_quic_connection* conn, uvcpp_http_request& req);

  /**
   * @brief 在指定的 h3 流上回一个响应。
   *
   * 与 `send_h2_response` 同一套规矩：补 `date`、走同一份压缩、HEAD 按 GET 的
   * 长度钉 `content-length` 但不发体、连接专属头交给会话层剥。
   *
   * @param stream_id 由调用方**显式**给（不从 `resp.stream_id` 读）：处理函数
   *        整对象替换过 `resp` 时那一格会被冲成 0（h2 那边同一个理由）。
   *
   * `accept-encoding` 与"是不是 HEAD"不在这张参数表上 —— 与 h2 一样是按流取的，
   * 但那条腿上它们是形参（h2 的派发是本函数的上游，手里就有请求），而 h3 的推迟
   * 应答中间隔了一次"稍后"，回来时请求对象早没了。于是它们随流登记在
   * @ref quic_ctx::h3_streams 里，本函数自己去查。查不到（畸形请求的 400、流已经
   * 收场）就按"没有压缩、不是 HEAD"处理 —— 那正是这两样的默认值。
   */
  int send_h3_response(uvcpp_quic_connection* conn, int64_t stream_id,
                       uvcpp_http_response& resp);

  /**
   * @brief 把 \p fn 投到第 \p loop_index 条循环上跑，并**有界地等它跑完**。
   *
   * 与 `uvcpp_tcp_server::start_worker_listeners()` 同一个装置（`shared_ptr` 包
   * 一个 `promise`、5 秒墙钟上限），理由也一样：投递是异步的，而调用方要的是
   * "这件事已经做完了"这个状态。
   *
   * @param loop_index 取 0..n−1。**0 号就地跑**（那就是调用方自己那条循环，
   *        投递没有意义 —— 见 `uvcpp_tcp_server::post_to_loop()`）。
   * @return true = 跑完了（`fn` 里无论做成没做成，都算跑完）；false = 没投出去
   *         或者超时，两种情况都已经在 stderr 上说明。
   */
  bool run_on_quic_loop(size_t loop_index, std::function<void()> fn);

  /**
   * @brief 建一个 QUIC 端点、绑上、开始收包。**必须在 \p loop 那条循环的线程
   *        上跑**（句柄只能在它自己的循环上建）。
   *
   * @param ip            绑的地址（`nullptr`/空 = 通配 IPv4）。
   * @param reuseport     要不要带 `UV_UDP_REUSEPORT` 去绑（扇出的成员要）。
   * @param port_io       入参是登记的端口；**出参**是内核实际给的那个（`0` 号
   *                      端口那一次要靠它把真号传播给后面的端点）。
   * @param rc_out        失败时的 libuv 错误码。
   * @return 成功时是端点指针（**所有权交给调用方**）；失败时 nullptr，端点已经
   *         自己收干净了。
   */
  uvcpp_quic_server* start_quic_endpoint(uvcpp_loop* loop, const char* ip,
                                         bool reuseport, int* port_io,
                                         int* rc_out);

  /**
   * @brief 收掉第 \p loop_index 条循环上**属于本层**的那一份：先删 h3 层，
   *        再删 QUIC 端点，最后清掉那一格计数。
   *
   * **必须在第 `loop_index` 条循环的线程上跑。** 它是析构的三段里 (A) 与 (C)
   * 的公共正文（A 段把它投到 1..n−1 号上，C 段在 0 号上就地跑），也是
   * `listen_quic()` 建到一半失败时回滚用的那一份。
   *
   * 次序与今天的析构逐字同源：**先删 h3 再删端点** —— h3 层持有 QUIC 连接，
   * 反过来不成立（`uvcpp_h3_connection` 的类注释写着"持有者必须在 QUIC 连接被
   * 端点释放之前销毁本对象"）。这里**不**调 `h3->close()`：此刻对端多半早就
   * 没了，而端点析构本来就会把它的连接放掉。
   */
  void teardown_quic_loop(size_t loop_index);

  /**
   * @brief 底层 QUIC 连接结束了：摘表 + 删 h3 层。**幂等**。
   *
   * 由 `on_disconnect` 调，析构里也走一遍（那条路上 `on_disconnect` 可能从没
   * 跑过）。**先摘表再删对象** —— 与 `remove_ctx` 同一个次序，理由见那里。
   */
  void remove_h3_ctx(uvcpp_quic_connection* conn);
#endif

  /// 没有显式打开就恒 false。
  bool http2_enabled_ = false;

  /**
   * @brief 明文连接的 h2c（先验知识）开关；没有显式打开就恒 false。
   *
   * 与 @ref http2_enabled_ 分开记，因为两者的有效期不同：h2 那条路是**每条连接
   * 建的时候**问一次 ALPN，这条是**每个读段**问一次（判定期要逐字节比前言）。
   * 合成一个字段之后调用方就没法表达"我要 h2，但明文照旧走 h1"。
   */
  bool h2c_enabled_ = false;

  uvcpp_tcp_server* tcp_server_ = nullptr;
  int status_ = HTTP_SERVER_NONE;

#if UVCPP_HTTP3_ENABLE
  /**
   * @brief h3 连接的登记表，**按循环切**（与 @ref contexts_ 同一个形状）。
   *
   * 尺寸 = `loop_count()`，在 @ref listen_quic 里一次定死，此后只读。
   * `set_loops()` 在 `listen_quic()` 之后返回 `UV_EBUSY`，循环数不会再变，
   * 扩容也就无从谈起 —— 取表那一路不需要锁，靠的就是这一句。
   *
   * 键用 `uvcpp_quic_connection*` 是唯一的选择：QUIC 连接没有"代次号"，而
   * h3 层与 QUIC 连接一一对应。**值刻意不是 `conn_ctx`** —— 那个结构体在 h1
   * 的**每请求**热路上，往里加一个字都是每个 h1 请求都要付的，哪怕一条 h3
   * 请求都没有。
   *
   * **每张表只有它自己那条循环的线程碰** —— 一条连接的全部回调都在它自己那条
   * 循环上跑（QUIC 端点就是那么建的），所以"这条连接属于哪张表"这个问题不需要
   * 问连接：**问当前线程**（见 @ref quic_ctxs_here）。
   */
  std::vector<std::map<uvcpp_quic_connection*, quic_ctx> > quic_ctxs_;

  /**
   * @brief QUIC 端点，**每个循环一个**（0 号 = acceptor 那条）。
   *
   * 懒建（@ref listen_quic 里）。`n == 1`（默认）时尺寸为 1，行为与从前逐字
   * 相同。n > 1 时 n 个端点各带 `UV_UDP_REUSEPORT` 抢同一个端口，内核按四元组
   * 哈希分流；平台不支持时**只有 0 号**（见 @ref quic_fanout）。
   *
   * 每个元素归它自己那条循环的线程管 —— 建、收、拆都在那条线程上。
   */
  std::vector<uvcpp_quic_server*> quic_servers_;

  /**
   * @brief 每条循环上**登记着的** h3 连接数，只给 @ref quic_connection_count_at
   *        那一族读数用（`quic_ctxs_[i].size()` 要从别的线程读是不安全的）。
   *
   * `quic_ctxs_` **自己不加锁**（它的每一次查找都在每请求的路上），所以这里
   * 单开一张整数表 + 一把锁：改它的时机只有**建立**与**拆除**一条连接这两处，
   * 与 h1 的 `clients_` / `clients_mu_` 同一条思路，也不在任何热路上。
   *
   * 尺寸与 `quic_ctxs_` 一样在 @ref listen_quic 里定死。
   */
  std::vector<int> quic_conn_counts_;
  /** @brief 护 @ref quic_conn_counts_（跨线程读那两格读数要用）。 */
  mutable std::mutex quic_conn_counts_mu_;

  /// h3 那条腿的身份凭证（@ref set_quic_ssl_context 记下的，建端点时转交）。
  uvcpp_ssl_context* quic_ssl_ctx_ = nullptr;
#endif

  std::vector<route_entry> routes_;
  http_request_handler default_handler_;
  upgrade_handler_t upgrade_handler_;
  std::function<void(uvcpp_tcp_client*)> close_handler_;
  /** @brief 新连接钩子（`on_connection`），早于该连接上的任何请求。 */
  std::function<void(uvcpp_tcp_client*)> connection_handler_;
  http_raw_data_hook raw_data_hook_;
  http_stream_claim stream_claim_;

  /** @brief Request body cap in bytes; 0 means unlimited. */
  size_t max_body_size_ = 0;
  size_t max_header_bytes_ = 0;
  size_t max_url_bytes_ = 0;

  /**
   * @brief 下一个要发的连接代次号。
   *
   * **先自增再赋值**，且**永不复位** —— 0 留给"没这条连接"，复位就等于允许
   * 旧代次复活，那这个机制就失去意义了（见 `conn_ctx::generation`）。
   *
   * **原子**：多循环下每一条工作循环都在自己线程上给新连接发号（代次号是
   * **全服务**一个序列，不按循环切 —— 调用方拿它当"这条连接的身份"比对，
   * 两条循环各自从 1 数起的话身份就不唯一了）。
   */
  std::atomic<uint64_t> next_generation_{0};

  /**
   * @brief 代次号的**跨循环登记簿**：`client -> generation`（@ref connection_generation
   *        与 @ref is_connected 专用）。
   *
   * `contexts_` 答不了那个问题，两个答法都走不通：走 `ctxs_of()` 要先读
   * `client->loop_index()`，而那个指针可能已经悬垂（那正是这条 API 存在的理由）；
   * 逐张表扫一遍则是**跨循环读别人的表**，与那条循环自己的 insert/erase 是数据
   * 竞争。两种都在 `web_static_server_func` 的 `multiloop_concurrency` 上被 ASan
   * 抓到过，细节记在 `connection_generation()` 的实现里。
   *
   * 于是单开这一张**只讲一件事**的表 + 一把锁：写侧只在连接**登记**与**摘除**
   * 那四处（与 `contexts_` 的增删同进同出），读侧只有那两个入口，**不在任何每请求
   * 路径上**（派发走 `ctxs_here()` / 手上的 `ctx`，一次都不碰这里）。
   *
   * 与 `contexts_` 同进同出的那条不变量：**这儿的键集 ≡ 全部表里的键并集**。
   */
  mutable std::mutex ctx_gen_mu_;
  std::map<uvcpp_tcp_client*, uint64_t> ctx_gens_;

  /**
   * @brief 连接上下文登记表，**按循环切**（下标 = `uvcpp_tcp_client::loop_index()`）。
   *
   * 这正是多循环的闸门：一条连接的全部回调都在它自己那条循环的线程上跑，
   * 而这张表是**每请求**都要碰的（53 处引用），所以它必须是 per-loop 的 ——
   * 一张全局表在两条循环上并发 insert/erase 是 UB，不是"慢一点"。
   *
   * 尺寸在 @ref listen 里定死（`uvcpp_tcp_server::loop_count()`），此后
   * **只读**，所以取表这一路不需要锁，也没有扩容。单循环下恒为一个元素。
   */
  std::vector<std::map<uvcpp_tcp_client*, conn_ctx> > contexts_;

  /**
   * @brief 取 \p client 那条连接所在的登记表。
   *
   * **必须在这条连接自己的循环线程上调用** —— 它返回的是一张别的线程可能正在
   * 动的表的引用；跨循环调用等于把它交出去给竞争。调用点全是连接自己的回调，
   * 所以这条约束天然成立。
   *
   * @warning **查完之后要拿这张表自己的 `end()` 比**，不要把 `find()` 的结果去和
   *          `ctxs_here().end()` 比：两张表不是同一个容器时，跨容器比较迭代器是
   *          未定义行为（`std::map` 的 `end()` 是各自表头节点的地址，于是"找不到"
   *          这条早退**不生效**，后面紧跟着的 `it->second` 就解引用了 `end()`）。
   *          n > 1 时两张表不同这件事，正是 `-1`（不在循环线程上）与"回调跑在
   *          别的循环的线程上"这两条路会造出来的。
   *
   *          ```cpp
   *          std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
   *          auto it = tbl.find(client);
   *          if (it == tbl.end()) return;
   *          ```
   */
  std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_of(uvcpp_tcp_client* client);

  /**
   * @brief 取**本线程**所在循环的登记表。
   *
   * 给手上没有连接对象的入口用（停机时"把本循环上的 h2 连接都道别"）。
   * 判"我在几号"用 `uvcpp_loop_index_of_this_thread()`，所以它同样只能在
   * 循环线程上调（n > 1 时在别的线程上调会 `abort`，见 @ref ctxs_at）。
   */
  std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_here();

  /**
   * @brief @ref ctxs_of / @ref ctxs_here 共用的取值。
   *
   * 越界（表外的循环号）夹回 0 号 —— 它至少是越界安全的，而按循环号取表的唯一
   * 来源就是连接自己的循环号与线程自己的循环号，两者都落在表内。
   *
   * @note **`-1` 不是越界，是"不在任何循环线程上"**，在多循环下直接 `abort`：
   *       夹回 0 号只处理 n 张表里的一张，却让调用方（`begin_h2_goaway()` 那类
   *       "每条循环各调一次"的入口）拿到一个看着正常的返回值 —— 错得比崩还安静。
   *       单循环下表只有一张，夹回 0 号与从前逐字相同，那条路原样保留。
   */
  std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_at(int loop_index);

  /// @brief @ref ctxs_of 的 const 版（`connection_generation()` 这类只读入口用）。
  const std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_of(
      uvcpp_tcp_client* client) const;
  const std::map<uvcpp_tcp_client*, conn_ctx>& ctxs_at(int loop_index) const;

  /**
   * @brief @ref ctx_gens_ 的写侧：登记 / 摘除一条连接。
   *
   * **必须与 `contexts_` 的增删成对调用**（登记那两句后面、`tbl.erase()` 后面），
   * 否则那张登记簿会答出"不在表里却报得出代次号"这类自相矛盾的答案。
   */
  void note_ctx_generation(uvcpp_tcp_client* client, uint64_t generation);
  void forget_ctx_generation(uvcpp_tcp_client* client);

#if UVCPP_HTTP_COMPRESS_ENABLE
  bool compress_enabled_ = true;
  size_t compress_min_body_ = 1024;
  /// 压缩档位，-1 = `Z_DEFAULT_COMPRESSION`（与
  /// `http_compress::k_compress_level_default` 同值；那个头文件不引 zlib，
  /// 所以不能在这里直接引用它）。
  ///
  /// 原子而不是普通 int：另四个压缩旋钮改的是判据，
  /// 而这个改的是键里的一个字符 —— 它要能在跑着的
  /// 实例上被改才能被量到（见上面 set_compress_level
  /// 的说明），两条循环线程会同时读写它。
  std::atomic<int> compress_level_{-1};
  std::vector<std::string> compress_excluded_types_;

  // -------------------------------------------------------------------
  // 压缩变体缓存（#11 第 3 条）：`(cache_tag, 编码) -> 压好的字节`
  //
  // **只记"算过的"，不决定"要不要算"**：门槛、MIME 表、accept-encoding 协商
  // 仍然只在 `apply_compression` 一处判，而且永远是**先判完、确定要压**才来
  // 查表。所以本缓存没有可能与配置打架 —— 它不参与任何决策。
  //
  // 只有填了 `cache_tag` 的响应有资格进来（今天只有静态文件那条路填），
  // 于是动态响应的行为与本缓存逐字无关，爆炸半径圈死在一个路由族。
  //
  // 键是 `编码字符 + 档位字符 + cache_tag`，**两个定长前缀都在前**是有意的：单字符前缀使
  // `(编码, tag) -> 键` 单射，后缀则不然（tag 自己含分隔符时会撞成同一个键）。
  // -------------------------------------------------------------------
  struct compress_variant {
    /// 压缩产物**只有这一份**：表与响应共用同一个句柄（`uvcpp_buf::share()`）。
    /// 命中时把句柄递出去、不再拷一份回来；存表时也只是存同一个句柄。表这边
    /// 是 `shared_ptr<const std::string>` —— 拿不到可写指针，那个 `const` 就是
    /// 「共享出去之后被人改了」的防线（有人要写，`uvcpp_buf` 会先物化出私有的
    /// 一份，表里这份纹丝不动）。
    ::std::shared_ptr<const ::std::string> data;
    uint64_t  last_used = 0;
    /// 这一条占多少字节。淘汰与上限都按它算（以前是 `uvcpp_buf::size()`）——
    /// 换成句柄之后**必须**换过来，否则字节账恒为 0、淘汰只剩条数那一条腿。
    size_t bytes() const {
      return this->data != nullptr ? this->data->size() : 0;
    }
  };
  std::map<std::string, compress_variant> compress_variants_;
  uint64_t compress_variant_clock_  = 0;
  uint64_t compress_variant_hits_   = 0;
  uint64_t compress_variant_misses_ = 0;
  uint64_t compress_variant_stored_ = 0;

  /// 护 `compress_variants_` 与上面那四个计数 —— **多循环下本表不是按循环切的**。
  ///
  /// 它是**跨循环共用的缓存**：切了就退回"每条循环各自 deflate 一遍同一个静态
  /// 文件"，命中率按循环数目线性掉（n 条循环 ⇒ 同一个文件被压 n 次、各存一份、
  /// 各占一份字节）。所以这里要的是**互斥**，不是分片。
  ///
  /// 加锁点只有三个**外层入口**（`..._locked()` 那几个帮手不自已加锁，
  /// 因为它们互相调用：淘汰要算字节总量，同一条非递归锁不能进两次）：
  /// 命中那条路、存入（含淘汰）那条路、`compress_variant_stats()`。
  /// 临界区里只碰表与计数，不调任何用户代码 —— `finish_headers()` 在锁外。
  ///
  /// `mutable`：`compress_variant_stats()` 是 const。
  mutable std::mutex compress_mu_;

  /// 表内字节总量。**从表里算出来，不存**：以前有一个 `compress_variants_bytes_`
  /// 计数字段，加在存入那条路上、减在淘汰那条路上 —— 而淘汰那句 `-=` 是要被删掉
  /// 也照样编得过、且**静默**失效的（条数那条腿还在照常淘汰，只有大文件会因为字节
  /// 上限永远触发不了而悄悄不再命中）。账一旦不存在，这种漂移就写不出来了。
  ///
  /// 代价：淘汰循环每轮都要重算一次（`compress_variant_evict_locked()`），而每轮只删一条。
  /// 原来那句线性找 LRU 本来就是每轮 O(n)，所以这里没有换复杂度类，只是常数翻倍。
  ///
  /// 调用方**必须已持有 `compress_mu_`**。
  size_t compress_variant_total_bytes_locked() const;

  /// 按字节淘汰最久未用的若干条；单条可能很大，条数上限卡不住内存。
  /// 调用方**必须已持有 `compress_mu_`**。
  void compress_variant_evict_locked();
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_SERVER_H
