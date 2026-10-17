# web HTTP 指南

`src/web/` 的 HTTP 半边有四个类：

| 类 | 干什么 | 头文件 |
|---|---|---|
| `uvcpp_http_server` | 建 TCP 服务、每条连接一个解析器、路由、发响应 | `<web/uvcpp_http_server.h>` |
| `uvcpp_http_client` | 发请求、收响应，异步/同步双模式 | `<web/uvcpp_http_client.h>` |
| `uvcpp_http_parser` | llhttp 的包装：把字节变成 `uvcpp_http_request` / `uvcpp_http_response` | `<web/uvcpp_http_parser.h>` |
| `uvcpp_static_server` | 静态文件托管（懒加载 + mtime 校验 + 线程池读盘） | `<web/uvcpp_static_server.h>` |

外加三个被前后两者共用的值类型：`uvcpp_http_request`、`uvcpp_http_response`
（**同一对类在两个方向上用**，见 [§5](#5-请求与响应对象)）、以及
`<web/uvcpp_http_common.h>` 里的枚举与工具函数。

**这一层是自足的：它完全不依赖 `webapp/`。** `src/web/` 里没有任何
`#include <webapp/...>`。反过来 `webapp/` 大量包含 `web/`（`src/webapp/uvcpp_web_app.h:137-138`
拿的就是 `web/uvcpp_http_server.h` 与 `web/uvcpp_ws_server.h`）。

> 本页讲 `web/` 的 **HTTP** 半边。WebSocket 在 [web WS 指南](./web-ws-guide.md)，
> 应用框架在 [webapp 应用框架开发者指南](./webapp-guide.md)，低层 TCP 在
> [net 网络层指南](./net-guide.md)，TLS 在 [TLS 指南](./ssl-guide.md)，
> HTTP/2 在 [HTTP/2 低层指南](./http2-guide.md)。
>
> 签名、默认值、行为都对着当前源码核过，非显然的结论后面都跟了 `文件:行号`。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [编译期条件与模块边界](#2-编译期条件与模块边界)
3. [最小服务端](#3-最小服务端)
4. [路由与匹配规则](#4-路由与匹配规则)
5. [请求与响应对象](#5-请求与响应对象)
6. [异步响应](#6-异步响应)
7. [流式响应](#7-流式响应)
8. [客户端](#8-客户端)
9. [静态文件服务](#9-静态文件服务)
10. [上限与旋钮](#10-上限与旋钮)
11. [错误处理](#11-错误处理)
12. [典型坑](#12-典型坑)
13. [没做的（如实列出）](#13-没做的如实列出)

---

## 1. 这一层是什么

`uvcpp_http_server` **不是从零写的 socket 服务器** —— 它建在 `uvcpp_tcp_server`
之上（`src/web/uvcpp_http_server.cpp:272-274`：构造函数第一件事就是
`new uvcpp_tcp_server()`），每条连接配一个独立的 `uvcpp_http_parser` 上下文
（`src/web/uvcpp_http_server.h:1752-1752` 的 `contexts_` —— 装的是**每连接状态**，
单循环时它是一张 `std::map<uvcpp_tcp_client*, conn_ctx>`；多循环下它按循环分成
若干张，取表见 `doc/multiloop-design.md` §4.1）。

三条贯穿全篇的结论：

**一、连接的生死归 `uvcpp_tcp_server`，不归 http 层。** 新连接在业务回调**之前**
就被登记并装好收尾（`src/net/uvcpp_tcp_server.h:53-58`），关闭时框架摘除并 `delete`
（`src/net/uvcpp_tcp_server.cpp:129-131`）。http 层那张 `contexts_` 表**不拥有**
`uvcpp_tcp_client`。所以地址会被复用 —— 判"还是不是原来那条连接"要用
`connection_generation()`（[§6](#6-异步响应)），不能比指针。

**二、没有统一的 `uvcpp_web_next`。** 那是 `webapp/` 层的东西
（`<webapp/uvcpp_web_handler.h>`）。web 层的异步续跑只有一条路：`resp.deferred = true`。

**三、`web/` 有 h2，但入口有三条。** 服务端 `set_http2_enabled(true)`
（`src/web/uvcpp_http_server.h:357-357`）加 TLS+ALPN，服务端 `set_h2c_enabled(true)`
（明文先验知识，RFC 9113 §3.4），客户端 `set_http2_enabled(true)`
（`src/web/uvcpp_http_client.h:474-474`）。
前两条都**默认关**；只有 `webapp/` 框架层默认开、零配置自动协商。
**不做 `Upgrade: h2c` 协商**（RFC 9113 已删除该机制），**也不做嗅探** —— 明文这条腿
只认那 24 个字节。

---

## 2. 编译期条件与模块边界

整个 HTTP 半边包在 `#if UVCPP_WEB_ENABLE` 里（`src/web/uvcpp_http_server.h:19`）。
三个块是**可选**的，各自有自己的宏（压缩那块有两个来源，见下表）：

| 功能 | 宏 | 关掉之后 |
|---|---|---|
| 响应压缩（gzip / deflate） | `UVCPP_ZLIB_ENABLE` | `set_compression_enabled()` 那一组消失 |
| 响应压缩（`br`） | `UVCPP_BROTLI_ENABLE` | 少了 `br` 这一种编码，其余照旧（[§10](#10-上限与旋钮)） |
| TLS / `https://` | `UVCPP_OPENSSL_ENABLE` | 没有 `set_ssl_context()`、没有 `send_wait_ssl()` |
| HTTP/2 | `UVCPP_NGHTTP2_ENABLE` | `set_http2_enabled()` 那一组消失 |

两行"响应压缩"是**同一个块的两个来源**：压缩器的总开关是
`UVCPP_HTTP_COMPRESS_ENABLE`，它由那两个宏**或**出来（`CMakeLists.txt:1899-1899`）——
任一为 1 就有这个块，两个都为 0 才整块消失。三个"块"的开关都默认 OFF，brotli
那条也是（`CMakeLists.txt:235-249` 那一组 `option()`，四个块开关默认全是 OFF）。编译进 `UVCPP_SRC_FILES` 的是
`src/web/*.cpp` 全体（`CMakeLists.txt:1537-1537`），但每个 `.cpp` 里的功能块同样带宏。

`uvcpp_http_server` **自己不碰 TLS** —— 它没有任何 SSL API。要上 https 必须
`get_tcp_server()->set_ssl_context(...)`，**而且必须在 `listen()` 之前**
（`tests/functional/web_ssl_h2_client_func.cpp:185`）。握手超时归
`uvcpp_tcp_server`（默认 10000 ms，`src/net/uvcpp_tcp_server.h:792`）。

---

## 3. 最小服务端

这就是 `src/web/uvcpp_http_server.h:163-172` 那段官方示例（我把它补成了完整 TU）：

```cpp
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_request.h>
#include <net/uvcpp_tcp_client.h>

int main() {
  uvcpp::uvcpp_http_server server;

  server.get("/hello", [](uvcpp::uvcpp_http_request& req,
                          uvcpp::uvcpp_http_response& resp,
                          uvcpp::uvcpp_tcp_client* client) {
    (void)req;
    (void)client;
    resp = uvcpp::uvcpp_http_response::ok("world", 5);
  });

  if (server.bind("0.0.0.0", 8080) != 0) return 1;
  if (server.listen() != 0) return 1;

  server.run();   // 默认 UV_RUN_DEFAULT
  return 0;
}
```

`bind` / `bindIpv4` / `bindIpv6` / `listen` 四个都在这里转调 `uvcpp_tcp_server`，
返回 libuv 错误码（`src/web/uvcpp_http_server.cpp:631-660`）。`listen` 的默认 backlog 是
128（`src/web/uvcpp_http_server.h:218-218`）。四个里只有 `listen` 多做一件事：转调之前
先把每循环的上下文表按 `loop_count()` 定型 —— 多循环时工作循环的线程是在
`set_loops()` 里放行的，等它接手连接再取表就晚了（`doc/multiloop-design.md` §4）。

### 关服是两步，没有 `close()`

**`uvcpp_http_server` 的公开 API 里没有 `close()`。** 唯一能"关服"的是 `stop()`
（`:385`），而它 = `tcp_server_->stop()`：**只关监听句柄，不动已建立的连接**
（`src/net/uvcpp_tcp_server.h:490-503`）。已经在处理的请求照跑。

```cpp
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_server.h>

void doc_graceful_shutdown(uvcpp::uvcpp_http_server& server) {
  server.stop();                                   // 1) 不再收新连接
  server.get_tcp_server()->close_all_clients();    // 2) 送走剩下的
}
```

想一步到位就两个都调。连接级的 `close_connection()` 是 **private**
（`src/web/uvcpp_http_server.h:1341-1341`）—— 单个连接只能从 `uvcpp_tcp_client` 那侧关。

---

## 4. 路由与匹配规则

注册 API **全部返回 `void`**，没有失败通道（`src/web/uvcpp_http_server.h:463-496`）：

```cpp
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_server.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

void doc_routes(uvcpp::uvcpp_http_server& server) {
  server.get("/api/users", [](uvcpp::uvcpp_http_request& req,
                              uvcpp::uvcpp_http_response& resp,
                              uvcpp::uvcpp_tcp_client* client) {
    (void)req; (void)client;
    resp = uvcpp::uvcpp_http_response::ok("[]", 2, "application/json");
  });
  server.post("/api/users", nullptr);   // 同上，签名完全一样；存的是函数对象
  server.put("/api/users", nullptr);
  server.del("/api/users", nullptr);
  server.options("/api/users", nullptr);
  server.patch("/api/users", nullptr);
  server.head("/api/users", nullptr);

  // 兜底：所有没命中的请求都到这儿。单槽，后装覆盖前装。
  server.on_request([](uvcpp::uvcpp_http_request& req,
                       uvcpp::uvcpp_http_response& resp,
                       uvcpp::uvcpp_tcp_client* client) {
    (void)req; (void)client;
    resp = uvcpp::uvcpp_http_response::not_found();
  });
}
```

**匹配规则（`src/web/uvcpp_http_server.cpp:973-992`）：剥掉 query 再逐字节比路径，
线性扫描，首次命中即返回。** 于是：

- **没有参数、没有通配、没有正则、没有中间件**（`src/webapp/uvcpp_web_router.h:9-14`
  也是这么描述它的）。
- **重复注册同一路径时先注册的赢，后面的永远不可达** —— 而 API 返回 `void`，
  没有任何提示。
- 都没命中 → `default_handler_`；`default_handler_` 为空 → 直接 **404**
  （`src/web/uvcpp_http_server.cpp:1522-1527`）。
- **分不出 404 和 405**：`get("/x")` 已注册时来一个 `POST /x`，走的还是兜底
  或 404，不会回 405。

路由表就是个 `std::vector`（`src/web/uvcpp_http_server.cpp:948-971`），
`listen()` 之后仍可以改；但它没有锁。

### 请求目标里的 query 要自己解

**web 层不提供任何 query / cookie / form / URL 解码工具，`req.url` 就是原始请求目标
（含 `?query`）。** 路由匹配时会把 query 剥掉再去比（`src/web/uvcpp_http_server.cpp:975-978`），
但**解析 query 是 handler 自己的事** —— 这一层没有这个函数。要用就在 `webapp/` 的
`web_parse_query()`（`src/webapp/uvcpp_web_util.h:205-205`）或者自己写一个。

### 流式与认领两条旁路

除精确路由外还有两条：

- `post_stream(path, http_stream_handler)`（`:250`）：只支持 POST，命中后
  body 不再被服务器缓冲，直接以 `HEADERS` / `BODY` / `END` 三个事件交给你。
- `set_stream_claim(hook)`（`:263`）：`post_stream` 没中时问它。它返回一个
  `http_stream_handler`（认领）或空（不认领）。`webapp/` 正是靠它把自己的路由器
  接进来的（`src/webapp/uvcpp_web_app.cpp:1970`）。

`stream_handler` 拿到的 `req` 是 `ctx.stream_request`，**头部视图只在当次调用内有效，
要留就自己拷**（`src/web/uvcpp_http_server.h:119-121`）。

---

## 5. 请求与响应对象

**同一对类在两个方向上用**，靠"你拿它干什么"区分，类本身不区分：

| 类 | 服务端 | 客户端 |
|---|---|---|
| `uvcpp_http_request` | 解析结果（handler 的入参） | 待发送报文（`to_string()` 序列化） |
| `uvcpp_http_response` | 待发送报文（handler 填） | 解析结果（回调的入参） |

两侧都有 `static from_parser(const uvcpp_http_parser&, const uvcpp_buf& body)`
（`src/web/uvcpp_http_request.h:154-154`、`src/web/uvcpp_http_response.h:195`）。

两边**都是公开字段 + 少量方法**，不是 getter/setter 墙：

```cpp
#include <cstdint>
#include <string>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

void doc_req_resp_fields(uvcpp::uvcpp_http_request& req,
                         uvcpp::uvcpp_http_response& resp) {
  // 请求：全是公开字段
  uvcpp::http_method m = req.method;          // 默认 HTTP_GET     src/web/uvcpp_http_request.h:40-40
  std::string        u = req.url;             // 默认 "/"，含 query  src/web/uvcpp_http_request.h:41-41
  uvcpp::http_headers h = req.headers;        // name 已转小写      src/web/uvcpp_http_request.h:43-43
  uvcpp::uvcpp_buf   b = req.body;            // 公开字段，不是方法  src/web/uvcpp_http_request.h:44-44
  (void)m; (void)u; (void)h; (void)b;

  // 这三个是**按值返回**的，别写成 req.get_header("host")[0]
  const std::string host = req.get_header("host", "localhost");
  const std::string ct   = req.content_type();
  const bool        has  = req.has_header("range");
  (void)host; (void)ct; (void)has;

  // 响应：status_code / headers / body 也全是公开字段
  resp.status_code = uvcpp::http_status::CREATED;
  resp.status_message = "Created";
  resp.set_header("content-type", "application/json");
  resp.body.append_data("{}", 2);   // uvcpp_buf，不是 std::string
}
```

### 三个必踩的点

**一、`status_code` 默认就是 `200 OK`。** `src/web/uvcpp_http_response.h:40` 的初值是
`http_status::OK`，不是"未设置"。所以**用 `err != 0` 判失败，别用 `status_code`**
（`src/web/uvcpp_http_common.h:246-247` 也是这么写的）。

**二、`resp = uvcpp_http_response::ok(...)` 会把 `stream_id` 冲成 0。**
`stream_id`（`:59`）由服务端**在处理函数之前**填好，整对象替换就丢了；
h2 连接上要么自己设回去，要么改用带 `stream_id` 的 `send_response` 重载
（`src/web/uvcpp_http_response.h:52-58`）。h1 上 `stream_id` 恒为 0，无所谓。

**三、`uvcpp_http_request` 与 `uvcpp_http_response` 的拷贝构造/赋值是手写的、
逐字段列举的。** 加字段时编译器一声不吭 —— `stream_id` 就漏过一次
（`src/web/uvcpp_http_request.cpp:25-36`、`src/web/uvcpp_http_response.cpp:53-70`）。

---

## 6. 异步响应

**handler 返回之前没发响应，就再也没机会发了** —— 除非把 `resp.deferred` 置真。
框架见到它就直接 return（`src/web/uvcpp_http_server.cpp:1533-1534`），
由你自己在之后（且必须在 loop 线程上）把响应送出去。

### 6.1 `req.reply` + `send_reply()`（`1.6.8-dev` 起，**推荐**）

"之后"用什么把响应送出去，取决于你在哪条传输上。**`req.reply` 把这件事统一了**：
它是这枚请求的「稍后回话」凭据（`<web/uvcpp_http_reply.h>`），框架在 h1 / h2 / h3
三条腿上**都**填好；兑现入口是 `uvcpp_http_server::send_reply()`。处理函数因此不必
知道自己在哪条腿上 —— 这正是它存在的理由。

```cpp
#include <string>

#include <net/uvcpp_tcp_client.h>
#include <req/uvcpp_work.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

void doc_deferred(uvcpp::uvcpp_http_server& server) {
  server.get("/slow", [&server](uvcpp::uvcpp_http_request& req,
                                uvcpp::uvcpp_http_response& resp,
                                uvcpp::uvcpp_tcp_client*) {
    // **按值**留一份凭据，别留引用：`req` 那份对象只活到本函数返回。
    const uvcpp::uvcpp_http_reply reply = req.reply;
    resp.deferred = true;   // 声明"这次不由框架发"

    uvcpp::uvcpp_work* w = new uvcpp::uvcpp_work();
    // 投回**这条请求自己那条循环** —— 异步结果回来时人可能在任何线程上，
    // 而 send_reply() 只在 loop 线程上合法。
    w->queue_work(reply.loop(),
      [](uvcpp::uvcpp_work* self) {
        // 工作线程：这里绝不能碰 server / reply / resp
        self->get_loop();   // 只为演示；真活放这儿
      },
      [&server, reply](uvcpp::uvcpp_work* self, int status) {
        // loop 线程
        if (status == 0 && server.reply_alive(reply)) {
          uvcpp::uvcpp_http_response late =
              uvcpp::uvcpp_http_response::ok("done", 4);
          server.send_reply(reply, late);
        }
        delete self;
      });
  });
}
```

**凭据不保证"还能回话"**：它记住的是**那一刻**那条连接，而对端断开、连接被回收、
指针被新连接复用都可能在排队等结果的期间发生。所以写之前问一次
`reply_alive()`（代次对得上）；`send_reply()` 自己也查，漏问的后果是"响应被丢掉"
（`UV_ENOENT`）而不是"发给别人"。`valid()` 是另一回事 —— 它只说明"这枚凭据是从
一条真请求上取下来的"，取下来之后一直是真。三条传输上凭据背后是什么、为什么它
不是 `uvcpp_http_request` 那种带虚表的类，见 `src/web/uvcpp_http_reply.h` 的头注释。

### 6.2 老写法：`connection_generation()` + `send_response()`

h1 / h2 上还有一条**逐字等价**的老路：手里那个 `uvcpp_tcp_client*` 自己就是凭据，
代次号靠 `connection_generation()` 现取。它在 h1/h2 上仍然完全有效（库内
`uvcpp_static_server` 走过它很多年），但 **h3 上不成立** —— 那条腿上第三个形参恒为
`nullptr`，没有 `client` 可记。新代码用 [§6.1](#61-reqreply--send_reply168-dev-起推荐)。

`connection_generation()`（`src/web/uvcpp_http_server.h:847-847`）是本层最容易被忽略、
又最容易出致命错的 API：**返回 0 表示这条连接已经不在服务器的登记表里，此时不该写它。**
代次号单调递增、永不复用，所以"连接还活着"时号不变、"地址被新连接复用"时号对不上
（`:1711-1721`）。`is_connected()` 就是 `generation != 0`（`:850-850`）。

**`send_response()` 会把 `resp.body` 移走。** 两块写（`nbufs = 2`）时 body 被
`std::move`，之后 `resp.body.size()` 是 0。要记"上线了多少 body"就用它的**返回值**
（`src/web/uvcpp_http_server.h:677-690`）。webapp 正是这么记访问日志的。
`deferred` 的标志位在**检查之后会被清掉**，所以那个 `resp` 对象不必一直活着 ——
但你要发送的那一份得自己留着。

**`stop()` 与 `close_all_clients()` 都不唤醒写队列里的 `done`**
（`src/web/uvcpp_http_server.cpp:370-372` 明说）。

---

## 7. 流式响应

先发头、再分块发体，三段式：`begin_stream` → `write_stream` × N → `end_stream`。

```cpp
#include <string>

#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

void doc_stream(uvcpp::uvcpp_http_server& server) {
  server.get("/stream", [&server](uvcpp::uvcpp_http_request& req,
                                  uvcpp::uvcpp_http_response& resp,
                                  uvcpp::uvcpp_tcp_client* client) {
    (void)req;
    resp.status_code = uvcpp::http_status::OK;
    resp.set_header("content-type", "text/plain");
    resp.deferred = true;   // 流式同样要声明"框架别发"

    server.begin_stream(client, resp);          // 只入队头部

    std::string chunk = "hello ";
    // h1 上写的是**已组好 chunked 帧**的字节；h2 上写的是裸 body（见下）
    if (server.write_stream(client, chunk, [](int) {}) == 0) {
      std::string tail = "world";
      server.write_stream(client, tail, [](int) {});
    }
    server.end_stream(client, /*close_after=*/true);
  });
}
```

**`write_stream` 的 `done` 契约（`src/web/uvcpp_http_server.h:732-755`）：**

- 入队后**保证恰好一次**，**包括连接还在队列里就被关掉的场合**；
- **但只有 `write_stream` 返回 0 才有这个保证** —— 非 0 时 `done` **不会被调**，
  而且**不要两边都调**（`:470-475`）；
- `done` **绝不在函数返回前同步跑**（`:487`）。

**同名方法在 h1 与 h2 上含义不同，而且没有编译期区分手段**
（`src/web/uvcpp_http_server.h:729-776`）：

| | h1 | h2 |
|---|---|---|
| `write_stream(client, bytes, ...)` | `bytes` 是**已组好 chunked 帧**的字节 | `bytes` 是**裸 body** |
| `end_stream(client, close_after)` | 可能关连接 | **不关连接**，只发 `END_STREAM` |
| 在 h2 连接上调 h1 那两个重载 | `write_stream` 返回 `UV_EINVAL`、`end_stream` **静默返回**（`src/web/uvcpp_http_server.cpp:1890-1890`、`src/web/uvcpp_http_server.cpp:1919-1919`）/ `begin_stream(client, resp)` 打一条 stderr 后丢弃（`src/web/uvcpp_http_server.cpp:1791-1796`） | — |

**`is_head` 与 `accept_encoding` 是连接级单槽，h2 上必须按流记。**
`src/web/uvcpp_http_server.h:1016-1046` 把三类后果写得很细；`send_h2_response` 里
是临时把"这条流的值借到连接级字段"再调压缩的
（`src/web/uvcpp_http_server.cpp:3105-3113`）。

### 升级到别的协议

`on_upgrade(handler)`（`src/web/uvcpp_http_server.h:445`）是单槽（`src/web/uvcpp_http_server.cpp:934-936`）。
它被调时这条连接已经从 http 解析里摘出来了，之后的字节归你；升级时**已经读进来但
还没解析的字节**用 `take_upgrade_leftover(client)`（`src/web/uvcpp_http_server.h:460-460` / `src/web/uvcpp_http_server.cpp:938-938`）取走 ——
客户端把 `Upgrade` 请求与第一帧 WebSocket 数据包在同一个 TCP 段里发过来是合法的，
少了这一步那一帧就永远丢了。

`attach()` 之类的安装动作会**覆盖**已装的 `on_upgrade`，而且不报警
（WebSocket 那半边就是这么装上自己的升级处理器的，详见
[web WS 指南](./web-ws-guide.md)）。

---

## 8. 客户端

### 异步（推荐）

```cpp
#include <string>

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

int doc_client_async() {
  uvcpp::uvcpp_http_client client;

  int rc = client.connect("127.0.0.1", 8080, [](int status) {
    if (status != 0) return;
    // 注意：这一段跑在连接完成回调里，禁止阻塞
  });
  if (rc != 0) return rc;

  // 回调签名是 (const uvcpp_http_response&, int err)。
  // 第一个参数是**整条**响应，不是流式；第二个是 libuv 错误码。
  return client.send(uvcpp::uvcpp_http_request::make_get("/"),
                     [](const uvcpp::uvcpp_http_response& resp, int err) {
    if (err != 0) return;                 // 失败一律看 err，别信 status_code
    (void)resp.status_code;
  });
}
```

`send` / `get` / `post` 的回调签名逐字是
`std::function<void(const uvcpp_http_response&, int)>`（`src/web/uvcpp_http_client.h:170-185`）。
交付的 `resp` 是成员 `pending_resp_`（`:449`）的 const 引用 —— **存到下次 `send()`
就被覆写**。

**异步路径没有超时，`connect()` 也不设**（`src/web/uvcpp_http_client.cpp:56-60`）。
要超时只能用 `*_wait` 系列。

**构造里那个 close 观察者不能省**（`src/web/uvcpp_http_client.cpp:51-70`）：少了它，
"对端在响应收完前断开"时 `send()` 的回调永远不来。

**在响应回调里 `delete` 客户端是预期的用法。** 所有闭包都捕一个 `alive_token_`
弱引用（`src/web/uvcpp_http_client.h:432-443`），析构第一件事就 reset
（`src/web/uvcpp_http_client.cpp:76`）—— 你不需要为它做任何额外的事。

### 同步

```cpp
#include <string>

#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

int doc_client_sync() {
  uvcpp::uvcpp_http_client client;
  int rc = client.connect_wait("127.0.0.1", 8080, 5000);
  if (rc != 0) return rc;

  uvcpp::uvcpp_http_response resp;
  rc = client.send_wait(uvcpp::uvcpp_http_request::make_get("/"), resp, 5000);
  if (rc != 0) return rc;
  return static_cast<int>(resp.status_code);
}
```

**同一个 client 上同步与异步不能交叉**，越界一律 `UV_ENOTSUP`
（`src/web/uvcpp_http_client.cpp:271`、`:431`、`:574`、`:598`）。

### 三个容易写错的地方

**一、keep-alive 有两个方向：请求那头听调用方的，响应那头听对端的。**
`keep_alive_` 是**调用方的偏好**（`src/web/uvcpp_http_client.h:427-427` 初值 `true`、
`:235` 声明、`src/web/uvcpp_http_client.cpp:841-841` setter）。`set_keep_alive(false)`
会让请求报文带上 `connection: close`（`src/web/uvcpp_http_client.cpp:516-535`）；
默认的 `true` 则不补头，报文里那条 `connection: keep-alive` 是
`uvcpp_http_request::to_string()` 补的（`src/web/uvcpp_http_request.cpp:156-159`）。
**响应**说的同样作数：`on_response_complete()` 按 llhttp 算出的
`should_keep_alive()`（HTTP 版本默认值 + `Connection` 的**逗号列表**）判断这条连接
还能不能再用，不能就当场清掉 `HTTP_CLIENT_CONNECTED`
（`src/web/uvcpp_http_client.cpp:710-730`）—— 此后第二次 `send()` 拿到
`UV_ENOTCONN`，而不是往一条服务端已决定关闭的连接上写。
`tests/functional/web_http_client_keepalive_func.cpp` 把这三条都钉住了。

**二、开了 OpenSSL 的构建里 `send_wait()` 走的是阻塞 fd，不是事件循环。**
`src/web/uvcpp_http_client.cpp:587-604` 是一个 `#if UVCPP_OPENSSL_ENABLE` 分支：
有 TLS 就走阻塞路径，没有才走"轮询 loop + 1ms sleep"的异步实现。
后果是**同一个程序的 keep-alive 行为会随构建而变**（`src/web/uvcpp_http_client.cpp:992-995` 把这段历史写下来了）。

**三、阻塞路径的 `timeout_ms` 只有 Windows 生效。**
`setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)` 两处都被 `#ifdef _WIN32` 包着
（`src/web/uvcpp_http_client.cpp:1134-1138`、`:1202-1206`），POSIX 上**不设**，
`send_wait()` / `send_wait_plain()` 在 Linux/macOS 上可以无限阻塞。头文件现在把这个
缺口写在参数说明里（`src/web/uvcpp_http_client.h:139-141`、`:155-159`），
不再让读者以为有超时兜底。

**四、阻塞路径读不到任何字节时返回 0，而 `resp` 是默认的 200 OK。**
`read_one_message()` 的返回值被丢弃（`src/web/uvcpp_http_client.cpp:1226-1235`），空头交给
`parse_response_head()` 之后什么都不改，`status_code` 保持初值 200。
**判据只能是返回值**。（异步 h1 路径相反：错误时会把 `status_code` 设成
`HTTP_STATUS_NONE`，`src/web/uvcpp_http_client.cpp:458-460`。）

### 没有的东西

**不跟随重定向。** `src/web/uvcpp_http_client.{h,cpp}` 里
`redirect|30[1238]` 零命中 —— 3xx 只是一个状态码交给你。

**压缩默认关，且门槛改不了。** `set_compression_enabled(true)`
（`src/web/uvcpp_http_client.h:214-226`）之后请求会补
`accept-encoding: gzip, deflate`、响应收全后自动解压 —— **异步与阻塞两条路径都算**
（`send()` 与 `send_wait()` / `send_wait_plain()` / `send_wait_ssl()`）。顺带记一笔
**曾经不是这样**：`1.6.32` 之前这两件事只写在异步那条路上，阻塞那两条静默地什么都不做
（请求不补头、响应也不解压，调用方拿到的是贴着 `content-encoding: gzip` 的压缩字节）。
但门槛硬编码 `compress_min_body_ = 1024`（`h:513`），**客户端没有
`set_compress_min_body_size()`**。于是 **< 1024 字节的 gzip 响应不会被解压，
`content-encoding: gzip` 也照样留着** —— 门槛比的是**压缩后**的长度，而调用方拿到的
就是压缩字节加一个说"这是 gzip"的头。

---

## 9. 静态文件服务

`uvcpp_static_server` 是**独立类，要自己挂**（`uvcpp_http_server` 不会自动启用它）：

```cpp
#include <string>

#include <web/uvcpp_http_server.h>
#include <web/uvcpp_static_server.h>

int doc_static() {
  uvcpp::uvcpp_http_server server;
  // 三个都是**构造参数**，没有 setter：local_dir、url_root、index_file
  uvcpp::uvcpp_static_server files("frontend/dist", "/", "index.html");

  server.on_request(files.handler(&server));   // 兜底：没命中 API 的都给它

  if (server.bind("0.0.0.0", 8000) != 0) return 1;
  return server.listen();
}
```

**`files` 必须活得比 `server` 长。** `handler()` 捕获的是 `this` 裸指针
（`src/web/uvcpp_static_server.h:160-164`）—— 上面那个写法（`files` 先于 `server` 声明、
后于它析构）是对的。

默认值（构造参数 `src/web/uvcpp_static_server.h:153-155`、成员初值 `:484-485`）：
`url_root = "/"`、`index_file = "index.html"`、`spa_fallback = true`、`cache_enabled = true`。
后两个**有 setter**（`set_spa_fallback()` `:177-177`、`set_cache_enabled()` `:180-180`），
前两个只能走构造函数。另有一组 setter：`set_precompressed()`（`:276-276`，默认 `false`）、
缓存的两个预算 `set_cache_max_entries()` / `set_cache_max_bytes()`（`:193-208`，默认
256 条 / 32 MiB）和 `set_revalidate()`（`:293-293`，默认
`uvcpp_static_revalidate::POOL`）—— 前者见下面讲预压缩孪生的那一节，后者见下面讲校验跑在
哪条线程上的那一节。构造函数还会把 `local_dir` 去掉尾斜杠、把 `url_root` 规范成 `"/"` 或
`"/xxx"`（`src/web/uvcpp_static_server.cpp:321-339`）。

**它是懒加载的**：首次请求某文件才读盘并进内存缓存，此后每次请求 stat 一次，
mtime/大小变了就异步重载 —— 所以文件改完**下一个请求就返回新内容**，不需要重启。
那次 stat 默认在**线程池**里做（整个库的硬性约定是"事件循环线程上不做 IO"），也可以
搬回循环线程：见下面「校验跑在哪条线程上」。

**缓存是有界的（1.6.44 起）**：`条目数 ≤ set_cache_max_entries()` 且
`字节数 ≤ set_cache_max_bytes()`，超了按 **LRU**（命中即提升）从队尾淘汰；
单份文件本身就超过字节预算时**根本不缓存它**（否则它一进来就把整张表冲空、下一次仍然
装不下）。`0` 表示"不限"而不是"预算为零"。这张表被多条循环线程共用，所以
`cache_size()` / `cache_bytes()` 都是走锁读的，`clear_cache()` 三者一起归零。

### 能力边界（都是它自己声明的）

- **不做 Range / 206、不做 ETag / 304、不做百分号解码**
  （`src/web/uvcpp_static_server.h:31-53`）。
- **多循环（`set_loops(n > 1)`）下可用**（1.6.8 起）。`cache_` 与 `retired_` 从前
  是"循环线程只有一条"写的（两张表都没有锁）。这条在 h3 上从"没人跑"变成必需：
  h3 端点默认就扇在 n 条循环上，而 h3 上挂静态文件**只有本类可用**
  （`uvcpp_http_server` 没有 `uvcpp_web_app` 那种 `serve_static()`）。
- 防目录穿越靠**段级归一化**（`src/web/uvcpp_static_server.cpp:44-81`），
  并额外拒掉 `\`、`:`、NUL 与控制字符（`:45-49`）。
- **`spa_fallback` 默认开，意味着找不到文件时也返回 200 的 `index.html`**
  （`src/web/uvcpp_static_server.cpp:678-683`）—— 接口写错路径时会静默拿到一页 HTML 而不是 404。
- 防"地址被复用"用的是**凭据里那个代次号**（`src/web/uvcpp_static_server.cpp:739-742`
  取到凭据、`reply_alive()` 逐处核），不是连接 id。
- MIME 表是**硬编码 switch**（`src/web/uvcpp_static_server.cpp:898-931`）；要可配的用 webapp 层的
  `uvcpp_web_static`（`src/webapp/uvcpp_web_static.h`）。
- **实时压缩对它生效了（1.6.44 起）**：`1.6.43` 及以前它不填 `resp.cache_tag`，于是
  `uvcpp_http_server` 那个按 `cache_tag` 做键的响应变体缓存对它**不生效** —— 开着
  `compress` 时它每请求在循环线程上把整份文件 deflate 一遍（20 个文件的语料上整条腿掉到
  **977 rps**）。现在它填了（文件身份：盘上路径 + 大小 + mtime 秒 + 纳秒），同一个装置上
  变成压一次、之后查表（**66 554 rps**）。想连那一次也省掉就用
  `set_precompressed(true)`（1.6.18 起，默认关）：盘上放了 `x.js.br` / `x.js.gz` 就整份
  读它、原样发出去并附 `content-encoding` 与 `vary: accept-encoding`。挑选规则与框架层
  `uvcpp_web_static` 的 `precompressed` **是同一份实现**（`web/uvcpp_http_common.h` 的
  `http_pick_precompressed_variants()`），边界也一样：孪生文件不存在就退回原文件、带
  `Range` 的请求不取孪生。实测（ext4 语料、8 循环，2026-10-14，
  **66 554.63 → 89 791.80 rps，+34.9%**，同时线上字节 −24.5%），见
  [`benchmark.md`](benchmark.md) §4.4。

### 校验跑在哪条线程上（`set_revalidate`，1.6.26 起）

一次**命中**的请求，本类要做的唯一 IO 是重新校验那一次 `stat`。它在哪条线程上做，
由 `uvcpp_static_revalidate` 选：

| 档 | stat 在哪 | 一次命中的池往返 |
|---|---|---|
| `POOL`（默认） | 线程池 worker | 有（排队 + 池锁 + after-work 回调各一次） |
| `LOOP_STAT` | **循环线程就地**（`serve()` 里同步做完） | **零** |

**默认是 `POOL`**，也就是 1.6.25 之前唯一的行为 —— 老调用方一行不改，语义逐字不变。
`LOOP_STAT` 只把**校验那一步**搬回来：缓存未命中时的读盘、以及 `LOOP_STAT` 自己
递归出的目录 index / SPA 回退，都还是同一套逻辑（命中与 404 在调用者的 `resp` 上回完，
未命中才落到异步读盘那条路并置 `resp.deferred`）。

为什么它能省：命中路径上 `stat` 是**唯一**需要池的地方，而一次池往返（排队 + 池锁 +
`uv_work` 的两次 `futex` + after-work 回调）在 8 核饱和、64 连接下是十几微秒量级。
实测（h3 那条腿、平台形状 `-c 64 -m 64 -t 8 -D 5s`，同一份 `.so`、同一个二进制、
只差一个环境变量）：`futex`/req **6.28 → 0.66**、`statx`/req **2.24 → 2.24（一个不少）**、
总系统调用/req **28.81 → 22.62**；`us_per_req` 中位数 **125.733 → 117.361**（−6.7%），
p99 **170.5 ms → 140.5 ms**。读数与口径见 [`benchmark.md`](benchmark.md) §4.8。

> **只在本地盘上开。** 循环线程上那次 `stat` 是**阻塞**调用：ext4 上 1.35 µs，
> FUSE / 网络盘上 46 µs 量级（`uvcpp_static_server.h` 的选项注释里两个数都写着）。
> 后者会拖住同一条循环上的其它连接 —— 尾延迟会先坏，rps 却看着还行，所以要
> **同时**看这两个数。默认档 `POOL` 就是为此留的：一个字段回退。

**池规模不是这条腿的杠杆。** 上面"一次池往返十几微秒"很容易被读成"那就少开几个线程"。
本机拿同一份 `.so`、同一份 `main.cc`（只差 `UV_THREADPOOL_SIZE` 一个环境变量）做过一次 2×2：

| 校验档 | 池 8 线程 | 池 64 线程 |
|---|---|---|
| `POOL`（默认） | 106.1 / 113.6 / 111.3（中位 **111.3**） | 109.0 / 109.4 / 109.9（中位 **109.4**） |
| `LOOP_STAT` | 84.2 / 88.7 / 87.1（中位 **87.1**） | 87.9 / 95.3 / 89.6（中位 **89.6**） |

（`static-h2` 形状、20 个 URI、`-c 256 -m 32 -D 5s`、ext4 语料，每格 3 次取中位数；
单位与口径见 [`benchmark-rig.md`](benchmark-rig.md)；不经静态处理器的 `baseline2` 两档
都在 9.8~10.4 µs，不受影响。）

两个方向都读得出来：**池 8 与池 64 之间只差 1.9~2.5 µs，落在 5~8 µs 的次间散布里**
（8 核上 256 连接本来就把核跑满了，池里排不排队不改变总吞吐）；而**同一个池规模下换成
`LOOP_STAT` 省下约 22 µs/req（−20 %）**。所以要省的是"每请求一次池往返"这件事本身，
不是池里有几个线程 —— 按档位把 `threadpool_threads()` 降下来既换不到读数，又会挤掉
`/async-db` 的槽位。默认 `POOL` 保持不动。

---

## 10. 上限与旋钮

**三个上限的默认值全是 0，也就是全都不限**（`src/web/uvcpp_http_server.h:1707-1709`）：

| 旋钮 | 默认 | 越限的答复 |
|---|---|---|
| `set_max_body_size(n)` | 0 = 不限 | **413 + `Connection: close`** |
| `set_max_header_bytes(n)` | 0 = 不限 | **431 + `Connection: close`** |
| `set_max_url_bytes(n)` | 0 = 不限 | **414 + `Connection: close`** |

**三者"生效时机"不一致**：头/URL 上限在 accept 时被拷进解析器
（`src/web/uvcpp_http_server.cpp:1022-1023`）⇒ **listen 之后改它只对新连接生效**；
`max_body_size_` 是每块 body 现读成员（`src/web/uvcpp_http_server.cpp:1085-1085`）⇒ **立刻生效**。
三处头文件注释一个字都没提这个区别。

**头/URL 上限对"被 claim 的请求"照样生效。** 这两道是在 llhttp 的头部回调里边收边判的
（`src/web/uvcpp_parser` 侧 `src/web/uvcpp_http_parser.cpp:430-478`），而 claim hook
是在 `headers_complete` 才被问的（`src/web/uvcpp_http_server.cpp:1092-1168`）。
所以被认领的请求**照样会被 414/431 拒掉并直接关连接**，认领者连 `HEADERS` 事件
都收不到。**只有 `max_body_size` 真正停在认领边界**（`src/web/uvcpp_http_server.cpp:1075-1091`）——
头文件把这条区别写在了 `src/web/uvcpp_http_server.h:526-529`（「a claimed request is not exempt」）
与 `:126-128`（`max_body_size` 豁免）两处。

**头/URL 是"边收边判"的**：一超就停，代价有界，而不是先收完再判
（`src/web/uvcpp_http_server.h:516-521`）。

### 压缩

zlib 编进来时**压缩默认开**（`src/web/uvcpp_http_server.h:1814-1814`），最小 body 1024
（`src/web/uvcpp_http_server.h:1814-1814`），排除的 MIME 走 `default_excluded_mime_types()`
（`src/web/uvcpp_http_compress.cpp:216-243`）。压缩变体缓存三道限：
单条 ≤ 4 MiB、总量 ≤ 32 MiB、条数 ≤ 1024（`src/web/uvcpp_http_server.cpp:1964-1974`）。

**第五个旋钮：档位。** `set_compress_level()` / `compress_level()`
（`src/web/uvcpp_http_server.h:612-612`），默认 `-1`（zlib 的
`Z_DEFAULT_COMPRESSION`，今天等价第 6 档），传进来的值由
`http_compress::clamp_compress_level()`（`src/web/uvcpp_http_compress.h:112-112`）
夹进 `-1` 与 0..9 —— 越界不报错，落到最近的端点上。它和上面那四个**不是一类**：
那四个改的是**要不要压**，档位改的是**压成哪些字节**，压这件事照样发生。所以档位
**进变体缓存的键**（`编码字符 + 档位字符 + cache_tag`，
`src/web/uvcpp_http_server.cpp:2179-2179`；两个定长前缀使键对"编码 + 档位 + tag"
单射），换档必然 miss 必然重压，而旧档位那条**留在表里** —— 换回去还能拿回它自己的
字节（`test_compress_level()` 钉的就是这一发）。它也可以在服务跑着的时候改：成员是
原子的，改完下一个请求就按新档位来。档位只改字节、不改内容，解压回来永远逐字节等于
原文，**代价结算在"线上字节数 ↔ CPU"上**（本机 zlib 1.3：8 397 B 那份从 6 档换到
1 档是 65.4 → 42.8 µs、1 566 → 1 957 字节），所以默认值一个字节都没动。

**第六个旋钮：`br`（brotli）。** `UVCPP_ENABLE_BROTLI`（默认 OFF，
`CMakeLists.txt:236-236`）给压缩器加上第二种编码：`br` 与 gzip/deflate 走**同一套**
协商（`http_compress::parse_accept_encoding()`，`Accept-Encoding` 里 `q` 相同时
`br` > `gzip` > `deflate`，`*` 兜底、`br;q=0` 排除、指名要一个本构建编不出来的
编码时**原样发原文**而不是拿另一种编码的字节冒充 ——
`src/web/uvcpp_http_compress.cpp:123-184`）、**同一张**排除 MIME 表、
**同一个**变体缓存。这不是为了快：质量档固定在
`http_compress::k_brotli_quality_default`（= 5，`src/web/uvcpp_http_compress.h:160`），
默认档下单份语料比 gzip 6 档**小 11 % 左右**，而每请求的代价与 gzip 相同 ——
因为压缩变体命中时只是一次 `std::map` 查找加一次 `shared_ptr` 交接，编码名只
出现在**键**里（`编码字符 + 档位字符 + cache_tag`），两条路走的是同一条
（`src/web/uvcpp_http_server.cpp:2178-2181`）。反过来说：**不填 `cache_tag` 就
每请求重压一遍**，那时 `br` 比 gzip 更贵（本机 8.4 KB 那份：gzip 51.6 µs、
`br` 88.6 µs —— 这一遍里有一半以上花在**编码器状态每请求重建**要的那 ~728 KB 上，
把它改成线程本地竞技场之后同一把尺子上 `br` 掉到 88.57 µs、`br`/`gzip` 由 3.25 降到
1.78，详见 `benchmark.md` §4.17；**但仍没过 gzip**），所以"要不要开"取决于条目
有没有把响应体的指纹写进 `cache_tag`。
`br` 没有独立的质量旋钮：`set_compress_level()`
（`src/web/uvcpp_http_server.h:612`）只管 gzip/deflate 那个量纲（`-1` 与 0..9），
`br` 用 `0..11`，两把尺子不能共用（`clamp_brotli_quality()` 与
`clamp_compress_level()` 之所以不合并，就是这条）。**`.br` 孪生文件与这个开关
无关**：`http_pick_precompressed_variants()`（`src/web/uvcpp_http_common.h:679-679`）
只看 `Accept-Encoding` 与盘上有没有那个文件，盘上有 `.br` 就照发（连
`vary: accept-encoding` 一起发，`src/webapp/uvcpp_web_static.cpp:2132-2146`）——
开关管的是"现场编得出编不出 `br`"，不管"能不能转发别人编好的 `br`"。

排除表里带 `/` 的条目按**前缀**匹配（`image/` 管住所有位图），不带 `/` 的按**精确**
匹配。字体那一组是刻意写成精确匹配的：`font/woff` 与 `font/woff2` 里装的本来就是
压缩流（WOFF 是 gzip 过的 sfnt、WOFF2 是 brotli 过的），再 deflate 一遍不但白烧
CPU，产物还**比原文件大**（本仓语料 22 528 → 22 556、18 432 → 18 460 字节）；
而 `font/ttf` / `font/otf` / `font/collection` 是未压缩的轮廓数据，deflate 有实打实
的收益 —— 写成 `font/` 前缀会把这两类一起排掉。Caddy 的 `encode` 跳过表同样是把
这两个类型逐个列出来。

### 协议行为

- **keep-alive**：HTTP/1.1 默认开、1.0 默认关；handler 显式设了 `connection`
  头就听 handler 的（`src/web/uvcpp_http_server.cpp:1680-1694`）。webapp 这一层
  还会在**发送前**把「**这一条**请求自己」的结论钉成显式头
  （`src/webapp/uvcpp_web_app.cpp:2697-2700`）——HTTP 层那边现问的是**连接级**的
  解析器状态，而 llhttp 的 flags 属于「最近一条解析完的报文」：流水线里末条写
  `Connection: close` 时，第 1 条响应（异步的那条）序列化时读到的已经是末条的
  结论，服务器会在第 1 条之后关连接、丢掉第 2..N 条（`1.6.46-dev` 修的）。
- **`Date` 每个响应都发，且按秒缓存**：四个出报文的口子各补一次 ——
  `send_response`（`src/web/uvcpp_http_server.cpp:1628-1628`）、h1 流式
  `begin_stream`（`src/web/uvcpp_http_server.cpp:1822-1822`）、h2 流式
  `begin_stream`（`src/web/uvcpp_http_server.cpp:1855-1855`）、h2 整包
  `send_h2_response`（`src/web/uvcpp_http_server.cpp:3097-3097`）。值由
  `uvcpp::http_date()` 造（`src/web/uvcpp_http_date.cpp:86-107`）：IMF-fixdate、
  **恒 GMT**，星期名与月份名是写死的 ASCII 表 —— 它**不**用 `strftime` 的
  `%a`/`%b`，那两个说明符在 `setlocale(LC_TIME, "")` 之后会吐本地文字，对端
  解析不了。handler 自己设过 `Date` 就不覆盖
  （`src/web/uvcpp_http_date.cpp:114-124`）。1xx 那两条报文是**裸字节**直接写
  出去的、没经这个收口，所以不带 `Date`：`100 Continue`
  （`src/web/uvcpp_http_server.cpp:1187-1187`）与 WebSocket 握手
  （`src/web/uvcpp_ws_server.cpp:202-202`）—— RFC 9110 §6.6.1 对 1xx 只是
  **可发**，不发也合规；这是"没经手"，不是"写了排除"。
- **`Expect: 100-continue` 支持**，且在 headers 回调里就补
  `HTTP/1.1 100 Continue\r\n\r\n` 裸字节（`src/web/uvcpp_http_server.cpp:1173-1188`）—— 注释解释了为什么
  不能用 `send_response()`（1xx 不该带 `Content-Length`）。HTTP/1.0 上 `Expect`
  被忽略（`src/web/uvcpp_http_server.cpp:1542-1547`）；**未知的 `Expect` 回 417**（`src/web/uvcpp_http_server.cpp:1557-1562`）。
- **声明的 `Content-Length` 超限 → body 还没到就回 413**（`src/web/uvcpp_http_server.cpp:1565-1577`）。
- **畸形报文走正规响应路径回 400 + `Connection: close`**（`src/web/uvcpp_http_server.cpp:1352-1361`），
  不是手搓字节。
- **流水线**：一次读里可以有多条报文，解析器按消息边界 reset
  （`src/web/uvcpp_http_server.cpp:1049-1072`、`src/web/uvcpp_http_server.cpp:1300-1316`）；**web 层不限条数**，webapp 才限。

### 没有的旋钮

**没有报头条数上限**（既无常量也无成员，`uvcpp_http_common.h` 与
`uvcpp_http_parser.h` 全文都没有）。**web 层没有任何超时** —— 见 [§12](#12-典型坑)。

---

## 11. 错误处理

**不用异常表达失败。** 三条通道：

1. **返回 libuv 错误码**：`bind` / `listen` / `run`；客户端的
   `connect` / `connect_wait` / `send` / `send_wait` / `post_wait`。
   常见值 `UV_ENOTCONN`、`UV_ENOTSUP`、`UV_ETIMEDOUT`、`UV_ECONNRESET`、`UV_EINVAL`。
2. **回调里的 `err` 参数**：客户端 `(resp, err)`、流式写的 `done(int)`。
3. **返回值加 stderr**：`send_response` 在连接已不在表里时**打一条 stderr 警告
   并返回 0**（`src/web/uvcpp_http_server.cpp:1612-1622`）；`begin_stream` 同理
   （`src/web/uvcpp_http_server.cpp:1774-1781`）—— 但它是 `void`，你**无从判断**。

**异常只在一处被接住**：`on_connection` / `stream_claim` / h2 连接钩子三个钩子
里抛出的异常会被吞掉并打 stderr（`src/web/uvcpp_http_server.cpp:1035-1046`、`src/web/uvcpp_http_server.cpp:1141-1152`、`src/web/uvcpp_http_server.cpp:2913-2922`）。
**`handler(req, resp, client)` 本身没有 try/catch**（`src/web/uvcpp_http_server.cpp:1522-1527`），
抛出去会穿过 llhttp 的 C 回调栈。webapp 层自己包了 try/catch 并扇出到错误中间件
（`src/webapp/uvcpp_web_app.cpp:3123-3129`）。

### 静默失败清单

| 静默点 | 表现 |
|---|---|
| `set_keep_alive(false)` | 完全无效（[§8](#8-客户端)） |
| 阻塞 `send_wait` 读空 | 返回 **0** 且 `resp` 是默认 200（[§8](#8-客户端)） |
| 阻塞路径的 `timeout_ms`（非 Windows） | 被忽略，可以无限阻塞 |
| `send_response` 落空 | 只打 stderr，返回 0 |
| `begin_stream` 落空 | 只打 stderr，`void` 无从判断 |
| `write_stream` 非 0 | `done` **不会被调**，要自己结算 |
| 重复注册同一路由 | 后一个永远不可达，无提示 |
| 在 h2 连接上调 h1 的 `write_stream` | 打 stderr 后丢弃 |
| 压缩响应 < 1024 B | 不解压，`content-encoding` 还留着 |

---

## 12. 典型坑

**一、服务端没有超时，一个连上不发字节的连接会永久占着。**
`uvcpp_http_server` 与 `uvcpp_tcp_server` 里没有任何超时机制
（`src/web/` 下 grep `idle_timeout` 只命中 `uvcpp_ws_parser::is_idle`），
`src/web/uvcpp_http_server.cpp:1342-1343`、`src/web/uvcpp_http_server.cpp:2440-2442` 与头文件
`src/web/uvcpp_http_server.h:1370-1373` 现在都明写这一点。
**真实的闲置清扫在 webapp 层**：`idle_timeout_ms` 默认 **60000**
（`src/webapp/uvcpp_web_app.cpp:214`），而且为 0 时连扫描句柄都不建
（`src/webapp/uvcpp_web_app.cpp:2234-2239`）。升级成 WebSocket 的连接被排除在闲置超时之外
（`src/webapp/uvcpp_web_app.h:866`）。

**二、`req` 与 `resp` 只在本次调用期间有效。**
`resp` 仍是 `on_request_complete` 的局部对象（`src/web/uvcpp_http_server.cpp:1474-1474`）；`req` 自 1.3.26 起是
**连接上那个请求对象**的引用（`src/web/uvcpp_http_server.cpp:1435-1436`），handler 返回后它**不被析构**
—— 下一条请求会在**原地**把它重填一遍，所以违反「返回之后不得再读它」的症状变成了
**静默读到下一条请求**（改前是悬垂读）。`deferred` 那句是**唯一**的例外通道。

**三、别在回调里做重活。** 所有回调都在 loop 线程上；重活走 `uvcpp_work`
（[§6](#6-异步响应)）。**也不要在回调里用同步 `write_wait` / `read_wait` 变体** ——
`cb == nullptr` 通常**正是**同步版本。

**四、`ctx.stream_request` 的头部视图是借的。** 只在当次调用内有效
（`src/web/uvcpp_http_server.h:119-121`）。

**五、h1 与 h2 的 `write_stream` / `end_stream` 同名不同义**，且没有编译期区分
（[§7](#7-流式响应)）。

**六、`send_response()` 之后不要回头读 `resp.body.size()`** —— 它已经被移走了，
要计数用返回值（[§6](#6-异步响应)）。

**七、写队列的 `done` 保证是有条件的**（[§7](#7-流式响应)）：只有 `write_stream`
返回 0 才有"恰好一次"的保证；返回非 0 时 `done` 不会被调。

**八、连接指针会被复用。** 异步路径上先记 `connection_generation()`，
回来先比对（[§6](#6-异步响应)）。

---

## 13. 没做的（如实列出）

只列**用法上会撞到**的，不列实现细节。

- **没有 `close()`**：关服是 `stop()` + `close_all_clients()` 两步（[§3](#3-最小服务端)）。
- **没有参数路由、通配、正则、中间件、405 区分**（[§4](#4-路由与匹配规则)）。
- **没有任何 query / cookie / form / URL 解码工具**，`req.url` 就是原始请求目标。
- **没有任何超时**（服务端与客户端异步路径都没有），
  闲置清扫在 webapp 层（[§12](#12-典型坑)）。
- **没有报头条数上限**。
- **客户端不跟随重定向**，**没有请求侧的重试/退避**。
- **压缩门槛在客户端改不了**，且 < 1024 B 的 gzip 响应不解压（[§8](#8-客户端)）。
- **`set_keep_alive()` 在客户端是死代码**（[§8](#8-客户端)）。
- **不做 `Upgrade: h2c` 协商**：明文 h2 只有"先验知识"一条腿
  （`set_h2c_enabled(true)`，默认关，见 [§1](#1-这一层是什么)）。
- **静态服务不做 Range / ETag / 百分号解码**，且 `spa_fallback` 默认开
  （[§9](#9-静态文件服务)）。
- **`uvcpp_http_server` 不自己管 TLS**，要在 `listen()` 之前装到
  `get_tcp_server()` 上。
- **C 面（1.4.3 起）只给精选的那一片**：客户端在
  `include/capi/uvcpp_c_web.h`（`http_client` / `http_response`），服务端那一片在
  `include/capi/uvcpp_c_webapp.h`（**C 侧的服务端就是 app**，没有单独的
  `uvcpp_c_http_server`）。同样没有 TLS 参数、没有重定向跟随。它是什么、不提供
  什么，见 [C ABI 指南](./capi-guide.md) §4。

---

相关文档：[web WS 指南](./web-ws-guide.md)、[net 网络层指南](./net-guide.md)、
[TLS 指南](./ssl-guide.md)、[HTTP/2 低层指南](./http2-guide.md)、
[webapp 应用框架开发者指南](./webapp-guide.md)、[HTTP/2 支持现状](./http2-status.md)、
[项目 README](../README.zh.md)。

本页用到的头文件：`<web/uvcpp_http_server.h>`、`<web/uvcpp_http_client.h>`、
`<web/uvcpp_http_request.h>`、`<web/uvcpp_http_response.h>`、
`<web/uvcpp_http_reply.h>`、`<web/uvcpp_http_common.h>`、
`<web/uvcpp_static_server.h>`、
`<net/uvcpp_tcp_client.h>`、`<net/uvcpp_tcp_server.h>`、`<req/uvcpp_work.h>`。
