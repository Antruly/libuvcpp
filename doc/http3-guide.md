# HTTP/3 指南

`src/http3/` 是 **web 层**的一条传输 —— HTTP/3（RFC 9114），解析用
[nghttp3](https://github.com/ngtcp2/nghttp3)，字节跑在 `src/quic/` 那条链路协议
（QUIC，RFC 9000）上。它和 `src/http2/` 对 HTTP/2 的位置逐字相同：协议栈归上游，
本库只做事件循环、缓冲、生命周期，以及**接进 web 层**那一段接线。它自己**不是**
一条链路协议 —— 加密、流控、丢包恢复全在 QUIC 那一层，本层不认识那些东西。

它也是 1.4.1 之前 `doc/quic-guide.md` 收尾那句"下一步只剩 HTTP/3"指的那一步。

> ## 1.4.1 起它是一条真能端到端跑一次请求的传输
>
> 两个功能用例在一条循环上真起一个服务端与一个客户端，真握手、真发请求、真收
> 响应：`http3_request_func.cpp`（95 条断言，h3 这一层）与 `http3_web_func.cpp`
> （92 条断言，走 `uvcpp_http_client` / `uvcpp_http_server` 的公开面）。
> 这些不是"设计好了"，是量出来的。**同一个 `uvcpp_http_server` 对象**在同一条
> 循环上既答 h3 也答 h1，这正是下面 [§6](#6-接进-web-层之后-h1-的判据是什么)
> 那条硬约束的护栏。
>
> **没做的**是 POST 之上的那些东西：trailers、GOAWAY、Server Push、流式路由、
> extended CONNECT（WebSocket over h3）—— 逐条列在下面
> [§8](#8-没做的如实列出)，别在别处另维护一份。

- 打开方式：`-DUVCPP_ENABLE_HTTP3=ON`。**默认 OFF**（`CMakeLists.txt:116`），
  而且下面两种情况会被**强制**置 OFF 并打 warning，而不是留一个"能配置、链不上、
  跑不起来"的组合：
  1. 没开 QUIC（`CMakeLists.txt:711`）—— HTTP/3 是 QUIC 之上的一条应用协议，
     没有 QUIC 它没有字节可以跑在上面；
  2. 没开 web 层（`CMakeLists.txt:718`）—— 这条传输的公开面就在
     `uvcpp_http_client` / `uvcpp_http_server` 上，web 关掉时它没有使用者。
- 包含方式：`<http3/uvcpp_h3_common.h>`、`<http3/uvcpp_h3_connection.h>`。
  私有的 `<http3/uvcpp_h3_nghttp3.h>` 与 `<http3/uvcpp_h3_session.h>` **都不安装**
  （`CMakeLists.txt:2079`）—— 理由见 [§7](#7-典型坑) 第一条。
- 两个公开头整段套在 `#if UVCPP_HTTP3_ENABLE` 里，web 层那边新加的
  `listen_quic()` / `set_http3_enabled()` 也是，所以**不开关就一个名字都看不到**。
  这与 `quic/`、`web/`、`ssl/`、`http2/`、`wsdl/` 同档。
- 开关与依赖：nghttp3 走 FetchContent（`NGHTTP3_VERSION`，`CMakeLists.txt:166`），
  静态链入 `nghttp3_static`（`CMakeLists.txt:1825`）—— 详见
  [§5](#5-cmake-接线与那条-check-撞名的坑)。

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一版没做"的地方都明确
> 标出来 —— 那些地方比 API 更容易踩。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [它要 QUIC 多给的三样东西](#2-它要-quic-多给的三样东西)
3. [公开 API 形状](#3-公开-api-形状)
4. [1.4.1 交付了什么](#4-141-交付了什么)
5. [CMake 接线与那条 check 撞名的坑](#5-cmake-接线与那条-check-撞名的坑)
6. [接进 web 层之后 h1 的判据是什么](#6-接进-web-层之后-h1-的判据是什么)
7. [典型坑](#7-典型坑)
8. [没做的（如实列出）](#8-没做的如实列出)

---

## 1. 这一层是什么

| 类 / 结构 | 头 | 角色 |
|---|---|---|
| `uvcpp_h3_connection` | `src/http3/uvcpp_h3_connection.h:130` | 一条 h3 连接的**驱动层**：持有那条 `uvcpp_quic_connection*`，管三条关键单向流、写序列化、完成队列 |
| `h3_request` / `h3_response` | `src/http3/uvcpp_h3_connection.h:75`、`:100` | 一次请求 / 一次响应（伪头拆成具名成员） |
| `h3_header` / `h3_header_kind` | `src/http3/uvcpp_h3_common.h:151`、`:136` | 一个头字段；伪头用具名枚举，不写 `":method"` 字面量 |
| `h3_header_budget` | `src/http3/uvcpp_h3_common.h:194` | 头块尺寸预算累加器（**本层唯一的防线**，见 [§7](#7-典型坑)） |
| `h3_stream_close_info` | `src/http3/uvcpp_h3_common.h:172` | 一条流的收场，**两个方向分开报** |
| `uvcpp_h3_session` | `src/http3/uvcpp_h3_session.h:107` | **私有**：纯字节/事件引擎，不碰 socket、不碰 libuv、不碰 QUIC 类型 |

**会话层与连接层的分工照抄 `src/http2/`。** 会话只管协议（收字节 / 吐字节），
连接管字节从哪来、往哪去。跳到连接层是**客户端与 web 层**的入口：`send_request()`
/ `take_completed()` / `send_response()`；会话层的 `drain()` / `recv_stream_data()`
是它内部的事，使用者看不到。

**与 h2 那一层的三处实质差别**（都是 QUIC 带来的，不是风格差异，逐条写在
`src/http3/uvcpp_h3_connection.h:1-22`）：

1. **没有 socket。** 底下是一条 QUIC 连接（可能跨好几条路径），不是一条 TCP 连接。
   `uvcpp_h3_connection` **不拥有**它 —— 它属于 `uvcpp_quic_client` /
   `uvcpp_quic_server`，持有者必须在 QUIC 连接被端点释放之前销毁本对象
   （`on_disconnect` 就是那个时机）。
2. **有"关键单向流"。** h3 的控制流与两条 QPACK 流是**握手完就要开出来**的，
   而 QUIC 的流额度可能还没到（`NGTCP2_ERR_STREAM_ID_BLOCKED`）。所以有 `ready()`
   这一位、以及一条"额度来了再补开"的路。
3. **流的收场要自己合成。** QUIC 的公开面没有"某条流关了"这个事件（它按方向报：
   读侧归 `on_read`、写侧归 `on_write`），而 nghttp3 **不会**自己摘掉一条流 ——
   `nghttp3_conn_close_stream()` 必须由应用在两个方向都收场的那一刻调。

---

## 2. 它要 QUIC 多给的三样东西

nghttp3 的几个回调直接对应 QUIC 的几个事件，而 1.4.1 的 QUIC 公开面里它们要么
折在一起、要么干脆没接。本批把三样都接了出来（QUIC 那一侧的完整说明见
`doc/quic-guide.md`）—— 它们不是"顺手加的 API"，每一个都有一个写不出 h3 的调用方：

**(a) FIN 与 RESET 必须能分开。** `nghttp3_conn_read_stream2()` 的 `fin` 参数必须
是真的。给 `net_read_result` 加了一格（`src/net/uvcpp_net_read.h:100`）：

```cpp
// doc-snippet: fragment — 结构体里一格加注释的摘录，不是完整翻译单元（没有 include，也不该有）
  bool           fin;   // 本次事件是不是对端**干净地**结束了它在这个方向上的发送
```

与 `event` 合起来才构成完整语义：`DATA && fin` 是"这块数据后面跟着 FIN"；
`PEER_CLOSED && fin` 是对端发了 FIN 的正常收尾，`PEER_CLOSED && !fin` 是对端发了
**应用错误码 0 的 RESET_STREAM**（同样是"读侧到此为止"，但不是干净收尾）。
TCP 那四处构造点置 `true`（TCP 的 `PEER_CLOSED` 本来就是对端 FIN），所以这个字段
对 TCP 也是真话。**这就是 §8 里那条"FIN 与 RESET 不再折成一件事"的由来。**

**(b) 流额度事件。** `on_streams_available`（`src/quic/uvcpp_quic_connection.h:106`）
报"现在可以多开几条本地流"。三条关键单向流在握手刚完成时可能要开，而那一刻对端的
`initial_max_streams_uni` 未必已经到了 —— 拿不到额度就只能等这条事件，**不能轮询**。
配额用 `streams_left(bool bidi)`（`:292`）问，它是**累计**上限，不是增量。

**(c) STOP_SENDING。** `on_stop_sending`（`:131`）报"对端请你别再发了"。它与
`on_read` 的收尾是**两件事**：那个报"对端不发了"，这个报"对端不要我发了"。
配套的是 `shutdown_stream_read()`（`:279`）—— 反向的那一句"我不收你发的东西了"
（nghttp3 的 `stop_sending` 回调要的就是它）。h3 的连接层同时用这两样，见 [§4](#4-141-交付了什么)。

> **`shutdown_stream()` 的错误码不再写死 0**（`:258` 的签名多了一个默认参数）。
> h3 取消一条流时必须把真实的应用错误码发出去，写死 0 会让对端把"取消"看成
> "正常结束"。

---

## 3. 公开 API 形状

### 3.1 客户端（直接用 h3 这一层）

```cpp
#include <http3/uvcpp_h3_connection.h>
#include <quic/uvcpp_quic_client.h>
#include <ssl/uvcpp_ssl_context.h>

#include <cstdio>
#include <string>

using namespace uvcpp;

// `quic` 是一条**已经握手完**的 QUIC 连接（ALPN 协商出 "h3"）。h3 这一层
// 不管握手 —— 那是 QUIC 那条腿的事。
int one_h3_get(uvcpp_quic_connection* quic) {
  uvcpp_h3_connection h3(quic, /*server_side=*/false);

  uvcpp_h3_connection::callbacks cbs;
  cbs.on_disconnect = [](uvcpp_h3_connection&) {
    // 底层 QUIC 连接结束了。**回调返回后不要再碰那个 h3 对象** —— 持有者
    // 应当在这里销毁它（web 层就是在这个回调里 delete 的）。
  };
  cbs.on_error = [](uvcpp_h3_connection&, int code) {
    // 连接级致命错误：本层已经自己把 QUIC 连接关掉了，这里通常只记日志。
  };
  if (h3.start(cbs) != 0) return 1;

  // 三条关键单向流要等 on_alpn 才开得出来 —— 所以 start() 之后 ready()
  // 还是假，这是正常的，不是失败。真正要发请求的代码应当等 on_alpn（或
  // 在那之前每隔一轮问一次 ready()）；这里为把片段写成一趟直线，就当成
  // "已经就绪"。
  if (!h3.ready()) return 2;

  h3_request req;
  req.method    = "GET";
  req.path      = "/";
  req.authority = "127.0.0.1";
  // 普通头走 headers；**伪头不要自己往这里塞** —— 用上面那四个具名成员。
  // 逐格赋值而不是 `h3_header{"accept", "*/*", …}`：那三个字段里有一格带默认
  // 成员初值，**C++11 下这就不是聚合体**、花括号初始化编不过（本库的编译标准
  // 是 C++11，见 §7 最后一条）。
  h3_header accept;
  accept.name  = "accept";
  accept.value = "*/*";
  req.headers.push_back(accept);

  const int64_t sid = h3.send_request(req);
  if (sid < 0) return 3;       // UV_EAGAIN = 关键流还没开出来

  h3_response resp;
  if (!h3.take_completed(resp)) return 4;   // 还没回来
  if (resp.error != 0) return 5;            // 失败的请求也会**恰好**出现一次
  std::printf("%lld %d %zu B\n", (long long)sid, resp.status, resp.body.size());
  return 0;
}
```

四条签名与语义上的讲究：

- **发出去时 `stream_id` 不用填。** `send_request()` 自己开流，并把它写回结果里；
  `h3_request::stream_id` 在**收到**一侧才有意义。
- **`take_completed()` 每次至多给一条**（`src/http3/uvcpp_h3_connection.h:227`）。
  这是刻意的：队列里可能同时到齐几条，回调里一次性抽干会让调用方在处理第一条时
  看不见第二条的到达，于是"这次回调要不要再排一次"永远答不准。
- **一条请求恰好在那里出现一次。** 正常收全一次（`error == 0`），中途被 reset /
  连接断掉也是一次（`error != 0`）。不会两次，也不会零次 —— 少了"零次"这一半，
  "取不到就是还没回来"和"取不到就是永远回不来了"这两件事就分不开了。
- **`req.body` 会被拷一份。** 签名收 `const h3_request&` 是为了让"传一个临时对象"
  这种常见写法能编译；nghttp3 要求那些字节活到对端确认，所以内部会把所有权接过去。
  一次请求一次拷贝，这是本批愿意付的代价。
- **伪头不许自己塞进 `headers`。** `h3_request` 把 `:method` / `:scheme` /
  `:path` / `:authority` 拆成了具名成员，`headers` 里带 `:` 开头的项会被**原样**
  发出去，而 h3 要求伪头必须自成一段。

### 3.2 服务端（直接用 h3 这一层）

```cpp
#include <http3/uvcpp_h3_connection.h>

#include <string>

using namespace uvcpp;

// `quic` 是 `uvcpp_quic_server::listen()` 回调里刚建出来的那条连接。
void serve_one_h3_conn(uvcpp_quic_connection* quic) {
  uvcpp_h3_connection* h3 = new uvcpp_h3_connection(quic, /*server_side=*/true);

  uvcpp_h3_connection::callbacks cbs;
  cbs.on_request = [](uvcpp_h3_connection& c, const h3_request& req) {
    // 拿到它的时候 **req.body 一定是全的** —— 触发点是这条流的读方向收场，
    // 不是"头块收全"。
    h3_response resp;
    resp.stream_id = req.stream_id;
    resp.status    = 200;
    h3_header ctype;                 // 逐格赋值，理由同 §3.1
    ctype.name  = "content-type";
    ctype.value = "text/plain";
    resp.headers.push_back(ctype);
    resp.body = "hello-h3";
    c.send_response(resp);              // omit_body=true 是 HEAD / 204 / 304
  };
  cbs.on_stream_close = [](uvcpp_h3_connection&, const h3_stream_close_info&) {
    // 两个方向都收场了。rx_error / tx_error 分开报 —— 折成一个"流关了"，
    // 调用方就分不出"请求体收全了"和"对端把请求取消了"。
  };
  cbs.on_disconnect = [h3](uvcpp_h3_connection&) { delete h3; };
  h3->start(cbs);
}
```

- **`on_request` 只在读方向收场时报**，所以拿到的体是完整的，不需要再等一次。
- **`on_disconnect` 恰好一次**，而且它返回之后不许再碰那个对象 —— 持有者在这里
  销毁它。web 层的 `remove_h3_ctx()` 就是这一条。
- **`send_response()` 是"受理"不是"发出去了"**，与本库其它写接口同一条约定；
  失败码里 `UV_EMSGSIZE` 表示头块超了我们自己的上限（见
  `H3_MAX_SEND_HEADER_BLOCK`）。

### 3.3 经 web 层（大多数人用这一档）

h3 接进 web 层之后，`uvcpp_http_client` / `uvcpp_http_server` 的**用法不变**：
路由表、兜底处理函数、`uvcpp_http_request` / `uvcpp_http_response`、压缩那一套
全都是同一份，处理函数不需要知道请求走的是哪条传输。

```cpp
// 最后那一行**不是**用 h3 的必需 —— h3 的两个开关（`set_http3_enabled()` /
// `listen_quic()`）就长在上面两个 web 头里。它是明说"这一条片段要 h3 模块在"：
// 示例门禁正是靠 include 认模块的（原因见 §5）。
#include <http3/uvcpp_h3_connection.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_server.h>

using namespace uvcpp;

// 服务端：多一条 UDP 口。**顺序**：装凭证 → listen_quic → run。
int start_h3_over_http_server(uvcpp_ssl_context& quic_tls,
                              uvcpp_http_server& server) {
  server.set_quic_ssl_context(&quic_tls);   // 必早于 listen_quic
  server.get("/", [](uvcpp_http_request&, uvcpp_http_response& resp,
                     uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok("hello-h3", 8);
  });
  if (server.listen_quic(0, "127.0.0.1") != 0) return 1;  // 0 = 让内核挑端口
  const int udp_port = server.quic_listen_port();         // listen_quic 之后才真
  (void)udp_port;
  // server.run() / server.stop() 对两种传输一次管全 —— 不需要为 h3 另调一次。
  return 0;
}

// 客户端：h3 是**另一条传输**，要显式打开。
void connect_over_http3(uvcpp_http_client& cli, int udp_port) {
  cli.set_http3_enabled(true);          // 默认 false
  cli.connect("127.0.0.1", udp_port, [](int) {
    // 这里的 0 是 **QUIC 握手完成**。h3 那三条关键单向流要等它之后的
    // on_alpn，所以这一刻还发不出请求。
  });
}
```

- `uvcpp_http_server::listen_quic()` 在 `src/web/uvcpp_http_server.h:222`，
  客户端那一侧是 `set_http3_enabled()`（`src/web/uvcpp_http_client.h:322`）。
  **整个 h3 公开面都套在 `#if UVCPP_HTTP3_ENABLE` 里** —— 没编进来时它不存在，
  这是**编译期**的"没有"，不是运行时返回错误。
- **`"h3"` 绝不进 TCP 的 ALPN 列表。** h3 只跑在 QUIC（UDP）上；在 TCP 的
  ClientHello 里报 `h3` 会让对端把 h3 的二进制流喂给 llhttp，症状是"连上了、
  写得出去、响应永远不来、哪里都不报错"。所以 `uvcpp_http_client` 原来那份 ALPN
  列表**一个字没动**，h3 走 `uvcpp_quic_client` 自己的 ALPN。
- **两种传输共用一条循环。** `uvcpp_quic_server` 不做多循环扇出，`run()` 在有
  QUIC 端点时直接跑那条循环；`stop()` 两条腿都停。**只开 h3 也必须调 `stop()`**
  —— 纯 h3 的服务端上 TCP 那条腿一次都没 `listen()`，`uvcpp_tcp_server::stop()`
  会同步早返回，少了"停 QUIC 循环"那一句，`run()` 永远不回来。这条有专门一条
  用例钉着（`http3_web_func.cpp` 的 `test_h3_only_stop_returns_from_run()`）。
- **h3 上暂不支持流式路由。** `post_stream` 注册的流式处理函数与
  `set_stream_claim` 装的认领钩子在 TCP 那条路上才生效；一条 h3 请求撞上它们会被
  **显式拒绝**（500 + stderr 一行），而不是被静默路由错。同理，h3 上
  `resp.deferred` 会被拒，处理函数拿到的是 `client == nullptr`。
  **这三条各有断言**（`test_unsupported_paths_refuse_explicitly()`）——
  静默错路由是这一层最容易犯、也最难查的错。

---

## 4. 1.4.1 交付了什么

| 东西 | 状态 | 判据在哪 |
|---|---|---|
| CMake 接线（开关、两守卫、FetchContent、`check` 撞名补丁、PIC 断言） | **真实现** | `check_ci_layout.py` 的 `FEATURE_GATES["http3"]` |
| 依赖接入（`nghttp3_static`，静态链入） | **真实现** | `http3_request_func.cpp` 的版本串断言（真调 `nghttp3_version()`） |
| 配置契约宏 `UVCPP_HTTP3_ENABLE`（生成头 + `_uvcpp_literal01` + PUBLIC 编译定义三处一致） | **真实现** | `check_config_contract.py` |
| **端到端 GET**（开流 → 头块 → 响应 → `take_completed()` 恰好一次） | **真实现** | `http3_request_func.cpp` |
| **三条关键单向流**（控制流 + QPACK 编码/解码流，含"额度没到时补开"） | **真实现** | `http3_request_func.cpp`（三条流号非 -1） |
| **头部预算**（`namelen + valuelen + 32`，越界当场断开而不是解完再看） | **真实现** | `http3_request_func.cpp` |
| **两条显式拒绝**（认领钩子、流式路由）+ `resp.deferred` 拒绝 | **真实现** | `http3_web_func.cpp` 的 `test_unsupported_paths_refuse_explicitly()` |
| **接进 web 层**（`listen_quic` / `set_http3_enabled`，同一个 server 同时答 h3 与 h1） | **真实现** | `http3_web_func.cpp` |
| **h3-only 的 `stop()` 让 `run()` 回来** | **真实现** | `http3_web_func.cpp` 的 `test_h3_only_stop_returns_from_run()`（带看门狗） |
| 压缩（`accept-encoding` 按**每条流**取，h1/h3 共用同一份实现） | **真实现** | `http3_web_func.cpp`（h3 与 h1 两条腿各验一次往返） |
| HEAD（报 GET 会有的 `content-length`、不发体、之后同一条连接还能用） | **真实现** | `http3_web_func.cpp` |
| **STOP_SENDING / 流额度事件 / FIN 与 RESET 分开** | **真实现**（在 QUIC 那一层） | `quic_api_func.cpp`（详见 `doc/quic-guide.md`） |
| POST 请求体 | **真实现**（在内存里攒满整个体，上限 64 MiB） | `http3_request_func.cpp` |
| trailers、GOAWAY、Server Push、extended CONNECT（WebSocket over h3） | **没有** | [§8](#8-没做的如实列出) |
| h3 上的流式路由（`post_stream` / `set_stream_claim`） | **没有**（显式拒绝，不静默错路由） | [§8](#8-没做的如实列出) |

**"判据在哪"这一列不是装饰。** 上面每一格都指得到一条会因为它坏掉而变红的断言；
指不到的地方**不写**，而是列在下面。这一版**没有**判据的有三处，写出来免得被当成
已经量过：

- **重新入纪律（"nghttp3 的写函数不能在它的回调里调"）功能用例抓不到。** 它只会在
  sanitizer 或 nghttp3 自己的断言上现形。本层的处置是 `in_callback_` 深度计数 +
  `want_write_` 标志（与 `uvcpp_h2_session` 同形），算"照模板评审覆盖"，不算量过。
- **丢包重传仍然没有被量过。** 那是 QUIC 那一层的事，照 `doc/quic-guide.md` §4 里
  那条如实记录办。
- **`add_ack_offset` 记错流，三个用例都看不出来。** 这条不是猜的：
  `tests/tools/http3_mutation.py` 的 M1 把 `on_write` 里那句 `add_ack_offset` 的
  归属流换成 `pending_` 的第一条流（单计数器近似），跑出来 `failures=0`，整棵树
  79/79 也全绿。它**不是等价变异** —— nghttp3 那边看到的记账确实错了 —— 而是
  一条**覆盖缺口**：要有"多条流并发 + 用到 QPACK 动态表"的用例才可能观测到它，
  现在没有这样一条。同一张变异表里另外八条的实测结果（哪几条红、红几条、以及
  `take_completed()` 不 pop 那次是**挂死/中止**而不是 FAIL）都写在那份脚本的头部。

一句话：**这一层现在是一条能握手、能开流、能收发请求与响应、能干净收场的 h3
传输**，而且它接在 web 层上 —— `uvcpp_http_client` / `uvcpp_http_server` 多了一条
`h3` 腿，其余用法不变。

---

## 5. CMake 接线与那条 `check` 撞名的坑

1. `option(UVCPP_ENABLE_HTTP3 ...)` 挨着 QUIC 那个（`CMakeLists.txt:116`），
   **默认 OFF**；两条守卫（`:711`、`:718`）用普通变量 `set(... OFF)` 降级并出声 ——
   照 QUIC 那套。**别只 grep `CMakeCache.txt`**：降级不是 cache 写，所以 cache 里
   仍然是 `UVCPP_ENABLE_HTTP3:BOOL=ON`，一个"被降级了"的配置和一个"真开了"的配置
   在 cache 那一层长得一模一样。配置成功的标志是这行 STATUS：

   ```
   nghttp3 integrated (tag=v1.18.0, static)          # CMakeLists.txt:864
   Including http3 module in build (nghttp3 v1.18.0) # CMakeLists.txt:1351
   ```

2. **依赖用 `FetchContent_Declare` + `Populate` + `add_subdirectory(... EXCLUDE_FROM_ALL)`，
   不用 `FetchContent_MakeAvailable`** —— 后者会把 nghttp3 自己的安装规则带进
   `cmake --install`，于是我们的包里会多出一份上游头。`CMAKE_POSITION_INDEPENDENT_CODE`
   在 function 作用域里设，照 ngtcp2 那段。

3. **`check` 目标撞名，这是第三个来源。** nghttp2（`nghttp2/CMakeLists.txt:195`）、
   ngtcp2（`ngtcp2/CMakeLists.txt:181`）与 nghttp3（`nghttp3/CMakeLists.txt:71`）
   都无条件建一个名叫 `check` 的 custom target，后加进来的那个直接 configure 失败。
   处置与 ngtcp2 那段逐字相同：把源码**复制**进构建目录，在副本上摘掉那一行；
   `string(FIND)` 落空就 `FATAL_ERROR`（**别把这条断言删掉了事** —— 上游改了写法时
   你要看到的是"补丁没打上"，不是"配置莫名其妙的撞名错"）。
   推论也一样：`-DFETCHCONTENT_SOURCE_DIR_NGHTTP3=<本地检出>` 之后改了那份检出，
   要删掉 `build*/_deps/uvcpp-nghttp3-src/` 再 configure。

4. **后置断言两条**：`if(NOT TARGET nghttp3_static)` FATAL
   （`CMakeLists.txt:852`；**裸目标名，没有 ALIAS**，别去猜
   `nghttp3::nghttp3_static`），以及把 `POSITION_INDEPENDENT_CODE` 读回来断言
   （`:806`）—— 后者是"Linux 上 `libuvcpp.so` 链接失败"这件事的成因。

5. **三处编译定义同步**：`cmake/uvcpp_config.h.in` 的 `UVCPP_HTTP3_ENABLE` 段、
   目录级 `add_compile_definitions`、PUBLIC `target_compile_definitions`，
   外加 `_uvcpp_literal01(UVCPP_HTTP3_ENABLE UVCPP_ENABLE_HTTP3)`（`CMakeLists.txt:1586`）。
   四处不一致会以 `#error` 或静默的 ABI 错配收场，`check_config_contract.py` 盯着
   其中一个方向。

6. **链接是 PRIVATE**（`CMakeLists.txt:1825` 的 `$<BUILD_INTERFACE:nghttp3_static>`）：
   nghttp3 的头路径不传播给使用者，所以**功能测试不能** `#include
   <nghttp3/nghttp3.h>`；想知道版本就用 `uvcpp_h3_connection::nghttp3_version()`
   （`src/http3/uvcpp_h3_connection.h:287`），它经公开头调用、链接期解析。

7. **本机可复现**：nghttp3 也可以用
   `-DFETCHCONTENT_SOURCE_DIR_NGHTTP3=<一份已下好的 nghttp3 源码>` 指过去绕开下载
   （v1.18.0 的源码树在 `https://github.com/ngtcp2/nghttp3.git` —— 组织是
   `ngtcp2`，不是 `nghttp2`）。

8. **本文那三条片段要"h3 模块在"才算数，而 CI 手里那份判据包里它是关的。**
   `tests/tools/check_doc_snippets.py` 靠片段 **include 了谁** 登记"这条片段要哪个
   模块"，所以 §3.3 那条明明只用 web 层 API 的片段也列了
   `<http3/uvcpp_h3_connection.h>` —— 不列的话它会**被编**，而它调的
   `set_http3_enabled()` / `listen_quic()` 整段在 `#if UVCPP_HTTP3_ENABLE` 里
   （`uvcpp_http_client.h:299`、`uvcpp_http_server.h:188`），于是报的是"未声明的
   成员"：那是**假红**（文档没腐烂，是那个包没编 h3）。
   登记之后，`config-contract` job 手里那个包会把这三条判成 `[跳·默认关]` 而不是
   `[跳]`，**退出码不会抬到 3**。两个宏（`UVCPP_QUIC_ENABLE`、`UVCPP_HTTP3_ENABLE`）
   在脚本的 `DEFAULT_OFF` 表里，判据与代价都写在那张表旁边。

   > ⚠️ **`1.5.0` 起这句话要说准**：`config-contract` 那份包**不是**发布包的逐字副本，
   > 两份的开关集合本来就不同 —— 它开 `WSDL`、不开 QUIC/HTTP3，而 `1.5.0` 起的发布包
   > 反过来（开 QUIC/HTTP3、不开 WSDL）。所以"发布包里 h3 是关的"**不再成立**；成立的
   > 是"**跑这个门禁的那份包里 h3 是关的**"（`check_doc_snippets.py` 只在两个
   > `config-contract` job 里跑，没有哪条腿拿带 h3 的包跑过它）。代价因此比原来更直白：
   > **这三条在 CI 里仍然一条都不会被编**，要判它们就得本地来一次 —— 本地验过一次
   > （2026-09-30，`3/3` 绿）：

   ```
   cmake --install <HTTP3=ON 的构建树> --prefix /tmp/h3-inst
   python3 tests/tools/check_doc_snippets.py --pkg /tmp/h3-inst \
       --docs doc/http3-guide.md
   ```

   顺带：CI 那个包（h3 关着、webapp/wsdl 全开）上跑**全量**的实测是
   `全过（97 条片段都编过）`、退出 0、其中 7 条是 `[跳·默认关]`（本文 3 条 +
   `doc/quic-guide.md` 4 条）—— 这就是"加了 `DEFAULT_OFF` 之后那条
   `exit "$rc"` 不再永久红"的物证。

---

## 6. 接进 web 层之后 h1 的判据是什么

用户这一轮的硬约束是：**"接进 web 层之后，不能影响原先 HTTP/1.1 的性能。"**
结论**从结构上论证**，另附两条在本机可核对的**物证**。不写成"我们很小心"。

**（a）默认关，而且关掉时 h1 那条路的编译命令一字不改。** `UVCPP_ENABLE_HTTP3`
默认 `OFF`；关掉时 `src/http3/` 根本不参与编译，web 层里新加的那几段全在
`#if UVCPP_HTTP3_ENABLE` 里，`uvcpp_http_client.cpp` / `uvcpp_http_server.cpp` /
`uvcpp_tcp_client.cpp` 三者的编译命令行与接线之前**逐字节相同**（下面 (d) 物证 1）。
**但"逐字节相同"只到编译命令这一层，别往下顺口说成"二进制没变"**：有**一处**
改动是与开关无关的 —— `apply_compression(conn_ctx&, resp)` 拆成了"转一层 +
`apply_compression_for(...)`"（h3 要按**每条流**取 `accept-encoding`）。差的
就是那一处，尺寸见下面物证 2。

**（b）h1 的那些函数一个字不改。** h3 只跑在 QUIC（UDP）上，h1 跑在 TCP 上，
两条 socket 完全不相交 —— "接进去"不等于"往 h1 的路上加东西"。具体到函数：
`on_tcp_connection`（TCP accept → 连接上下文）、`on_connection_data`（llhttp
解析路）、`on_request_complete`（路由分派）、`conn_ctx`、`find_handler` 路由表、
h1 写泵、TCP 的 ALPN 列表与分支 —— **全部不改**。唯一的例外就是 (a) 里那处
`apply_compression` 转调（h1 与 h3 共用同一份实现，拆出一层是为了让"按哪条流的
头决定压不压"这件事有地方放）；它是拆函数，不是改逻辑 —— 体没变、总量没变。

**（c）往共享结构里加了什么：服务端零。** h3 用**另一张表**
（`std::map<uvcpp_quic_connection*, quic_ctx> quic_ctxs_`，
`src/web/uvcpp_http_server.h:1232`），只在 QUIC 回调里碰；`conn_ctx` **不加任何
字段** —— 那个结构体在 h1 的**每请求**热路上，往里加一个字都是每个 h1 请求都要付
的。客户端加了四个成员（两个 `bool`、两个指针，`src/web/uvcpp_http_client.h:473-479`）
—— 那是每**连接**一个的对象，不是每请求一个；`send()` 热路上多的是一个可预测为假
的分支。不新增每连接或每请求的 `std::function`，不新增锁、不新增分配、不新增虚调用。

**（d）两条本机可核对的物证**（1.4.1 量的，命令与结论都记在提交信息里）：

1. **编译命令 diff。** 同一行 configure（`-DUVCPP_ENABLE_HTTP3=OFF`，
   `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`）分别配父提交与工作树，`src/web/uvcpp_http_server.cpp`、
   `src/web/uvcpp_http_client.cpp`、`src/net/uvcpp_tcp_client.cpp` 三条条目
   **逐字节相同**（3/3）。
2. **符号尺寸。** `nm --print-size --size-sort -C libuvcpp.so` 全表对比：符号总数
   4928 → 4929，**尺寸不同的只有 4 条**，而且全是同一次有意的拆分 ——
   `uvcpp_http_server::apply_compression(conn_ctx&, resp)` 从 2218 字节变成 16 字节
   （转一层），新增的 `apply_compression_for(std::string, resp)` 是 2218 字节，
   **体没变、总量没变**（h3 要按**每条流**取 `accept-encoding`，所以加了这一层）。
   h1 热路上的三个函数尺寸**逐字节相同**：`on_tcp_connection` 0x752、
   `on_connection_data` 0x2b1、`on_request_complete` 0x947 —— 它们的 `.cold` 克隆
   也一样。这一条能抓住"有人往 `conn_ctx` 里加字段"。

---

## 7. 典型坑

1. **别指望从公开 h3 头里看到 nghttp3。** `<nghttp3/nghttp3.h>` 只出现在私有的
   `src/http3/uvcpp_h3_nghttp3.h` 里，而它和持有 nghttp3 句柄的
   `src/http3/uvcpp_h3_session.h` **两个都不安装**（`CMakeLists.txt:2079`）、打包
   也排除。理由与 quic 那一对逐字相同：一是使用者不该被逼着去配 nghttp3 的搜索
   路径才能 include 一个本库的头；二是 `uvcpp_h3_session.h` 的字段布局直接跟着
   nghttp3 的版本走 —— 一旦漏进公开面，那个版本就变成了本库的 ABI。这处排除与
   `tests/tools/package_release.py` 的 `PRIVATE_HEADERS` 是**一对**，必须一起改。

2. **头块预算必须自己算，nghttp3 不替你做。** 它的文档写着
   "nghttp3 library does not enforce this limit. Applications are responsible
   for imposing their own limits" —— `max_field_section_size` 在会话里只被存下来，
   接收路径从头到尾没有对它做过一次累加比较。它唯一的尺寸保护是单个名字/值的
   上限，管不了"很多个小字段"这种 QPACK bomb。所以累加在 `h3_header_budget`
   （`src/http3/uvcpp_h3_common.h:194`）里自己算，而且**必须在上限处断开**，
   不是解完再看。

3. **只有 FIN、没有数据的块要 `add_write_offset(sid, 0)`。**
   `nghttp3_conn_writev_stream()` 可能吐出一个零字节、只带 FIN 的块，
   `add_write_offset()` 无论如何都要调 —— 少了那一句，这个 FIN 永远发不出去。
   本层的 `flush()` 就是这么写的（`src/http3/uvcpp_h3_session.h:271` 那条注释）。

4. **别在 nghttp3 的回调里调它的写函数。** 与 nghttp2 同一条硬规矩：那会在它
   自己的栈上重入。本层的处置是"回调里只置 `want_write_`，`recv_stream_data()`
   返回之后由驱动兑现"。同理，`nghttp3_conn_close_stream()` **不能在它自己的回调
   栈里调** —— 本层把要关的流排进队列，退栈之后再兑现。

5. **`vecs` 与 rcbuf 的指针只在本次调用期间有效。** `drain()` 吐出的段指向
   nghttp3 的内部缓冲，必须在同一轮里交给 QUIC；`recv_header` 拿到的
   `nghttp3_rcbuf*` 也只在那个回调期间有效，本层当场拷成 `std::string`。

6. **`on_disconnect` 之后不要再碰那个 h3 对象。** 它恰好跑一次，返回后持有者就
   该销毁它。web 层的 `remove_h3_ctx()` 是幂等的（先摘表、再删对象），就是为了
   兜住"连接没了"和"服务端停机"两条路都往它走。

7. **`UVCPP_HTTP3_ENABLE` 别自己定义。** 包的 `uvcpp/uvcpp_config.h` 里带的是本次
   构建实际用的值，自己定义一个不一致的是**硬 `#error`**，而不是静默的 ABI 错配。

8. **一个头字段的名字里带冒号就是伪头。** `h3_header::name` 对伪头是 `":method"`
   这个字符串（不拆两栏，因为"伪头必须在普通头之前"这个顺序只有一条列表能表达）；
   写回去时**不要**自己往 `headers` 里塞它 —— 用 `h3_request` 的具名成员。

9. **`h3_header` / `h3_response` 这类小件不能花括号初始化，这是 C++11 的硬规矩。**
   它们有一格带默认成员初值（`kind = h3_header_kind::REGULAR`），而"C++11 里带
   默认成员初值的类**不算聚合体**"—— 这条限制到 C++14 才放开。本库的编译标准是
   C++11（`CMakeLists.txt:168` 的 `set(CMAKE_CXX_STANDARD 11)`），所以
   `h3_header{"accept", "*/*", …}` 在消费者那里编不过，报的是
   `no matching function for call to 'h3_header::h3_header(<brace-enclosed
   initializer list>)'`。**逐格赋值**不会踩到它，本库自己的代码也是这么写的
   （`src/http3/uvcpp_h3_connection.cpp:26` 的 `make_pseudo`）。
   **这一条是示例门禁量出来的，不是抄来的**：本文 §3.1 / §3.2 最初就是花括号版本，
   跑一次 `check_doc_snippets.py`（它固定用 `-std=c++11`）报两条红，位置指向这里。

---

## 8. 没做的（如实列出）

按仓库惯例，这一节必须老实写。以下都是**这一版真的没有**，不是"文档没写"：

- **没有 trailers。** 会话层里 `begin_trailers` / `recv_trailer` / `end_trailers`
  三格**留空**，不是填一个"收到就忽略"的实现 —— 忽略会让带 trailers 的响应在
  本层看来"从来没有第二个头块"，调用方等到的是流的收场而不是缺失的原因。
- **不发也不收 GOAWAY。** 会话层的 `shutdown` 回调留空（`src/http3/uvcpp_h3_session.cpp:178`），
  主动平滑退场那一条路因此没有接。
- **没有 Server Push。** RFC 9114 里它本来就只剩一格 `SETTINGS_ENABLE_PUSH`，
  本层既不宣告也不处理 `PUSH_PROMISE`。
- **没有 extended CONNECT**，所以 **h3 上跑不了 WebSocket**。`app.websocket()`
  继续走 HTTP/1.1 的 `Upgrade`（h2 上同样不做，见 `doc/http2-status.md`）。
- **h3 上没有流式路由。** `post_stream` 的流式处理函数与 `set_stream_claim` 的
  认领钩子在 TCP 那条路上才生效；h3 上撞上它们是**显式拒绝**（500 + 一行 stderr），
  不是静默错路由。`resp.deferred` 同理。
- **h3 上 `client` 是 `nullptr`。** 处理函数拿不到 `uvcpp_tcp_client*` —— 一条
  QUIC 连接下面没有 TCP 客户端这回事。
- **没有多循环支持。** `uvcpp_quic_server` 不做多循环扇出（`set_loops()` ），
  所以 h3 只有一条循环在收包。两种传输共用那一条循环。
- **请求体/响应体只在内存里攒**（上限 `H3_DEFAULT_MAX_BODY_BYTES`，64 MiB），
  没有边收边交给使用者的那一档。
- **MinGW 的 CI 腿没有开 HTTP/3**（quic 在那边也没开）。macOS 与 Windows MSVC
  各有一格 `http3`，只有 MSYS2 那条腿没有 —— 它是单个 job，按功能拆它和加 h3 格
  是同一件事，`doc/ci-guide.md` §1 的"未覆盖的格"里有理由。
- **预编译包（`1.5.0` 起）里这个头是活的**（`UVCPP_HTTP3_ENABLE 1`）：六条腿的发布档
  与调试档都开了 HTTP/3（连带 QUIC）。**在 1.5.0 之前这条不成立**：那时候的发布包不带
  HTTP/3，这个头是惰性的（与 `quic` / `http2` 头在非该模块的包里的行为一致）。**从源码
  构建的人不受影响**：`UVCPP_ENABLE_HTTP3` 默认仍是 `OFF`，它与 QUIC + web 两个前提也
  照旧要自己满足。
- **C 面（1.4.4 起）接在一条"借来的" QUIC 连接上**：`include/capi/uvcpp_c_http3.h`
  是 h3 驱动层一个句柄（`uvcpp_c_h3_connection`）+ 请求 / 响应构造器 + 回调期的请求
  视图。它**照抄**了本页那条 h3 与 QUIC 的分工（h3 换掉连接上的回调表），于是 C 面
  也继承了它的边界：装了 h3 之后，那个借来的 QUIC 连接句柄的收尾只能由 h3 的
  `on_disconnect` 做 —— 这一条是本层最容易被漏掉的承重结构，被 `capi_mutation.py`
  的 M18 钉着。trailers / push / 逐帧回调仍不给，见 [C ABI 指南](./capi-guide.md) §4。
- **没有 h3 的优先/依赖（`priority`）语义**，也没有连接级统计。

下一步是**把 h3 上的流式路由补上**（`post_stream` / `set_stream_claim` 需要一条
"稍后把响应送出去"的入口，h3 的 `stream_id` 就是那个句柄），以及 **GOAWAY 的
平滑退场**。0-RTT、连接迁移那些仍然不在路线图上 —— 它们要等有真实需求时才谈。
