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
`#include <webapp/...>`。反过来 `webapp/` 大量包含 `web/`（`src/webapp/uvcpp_web_app.h:124-125`
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
之上（`src/web/uvcpp_http_server.cpp:44-46`：构造函数第一件事就是
`new uvcpp_tcp_server()`），每条连接配一个独立的 `uvcpp_http_parser` 上下文
（`src/web/uvcpp_http_server.h:952` 的 `std::map<uvcpp_tcp_client*, conn_ctx> contexts_`）。

三条贯穿全篇的结论：

**一、连接的生死归 `uvcpp_tcp_server`，不归 http 层。** 新连接在业务回调**之前**
就被登记并装好收尾（`src/net/uvcpp_tcp_server.h:44-49`），关闭时框架摘除并 `delete`
（`src/net/uvcpp_tcp_server.cpp:70-72`）。http 层那张 `contexts_` 表**不拥有**
`uvcpp_tcp_client`。所以地址会被复用 —— 判"还是不是原来那条连接"要用
`connection_generation()`（[§6](#6-异步响应)），不能比指针。

**二、没有统一的 `uvcpp_web_next`。** 那是 `webapp/` 层的东西
（`<webapp/uvcpp_web_handler.h>`）。web 层的异步续跑只有一条路：`resp.deferred = true`。

**三、`web/` 有 h2，但入口只有两条。** 服务端 `set_http2_enabled(true)`
（`src/web/uvcpp_http_server.h:190`）加 TLS+ALPN，客户端
`set_http2_enabled(true)`（`src/web/uvcpp_http_client.h:385`）。
两条都**默认关**；只有 `webapp/` 框架层默认开、零配置自动协商。
**不做 h2c** —— 明文升级在 `src/` 里零实现。

---

## 2. 编译期条件与模块边界

整个 HTTP 半边包在 `#if UVCPP_WEB_ENABLE` 里（`src/web/uvcpp_http_server.h:19`）。
三个块是**可选**的，各自有自己的宏：

| 功能 | 宏 | 关掉之后 |
|---|---|---|
| 响应压缩 | `UVCPP_ZLIB_ENABLE` | `set_compression_enabled()` 那一组消失 |
| TLS / `https://` | `UVCPP_OPENSSL_ENABLE` | 没有 `set_ssl_context()`、没有 `send_wait_ssl()` |
| HTTP/2 | `UVCPP_NGHTTP2_ENABLE` | `set_http2_enabled()` 那一组消失 |

**三个都默认 OFF**（`CMakeLists.txt:73-75`）。编译进 `UVCPP_SRC_FILES` 的是
`src/web/*.cpp` 全体（`CMakeLists.txt:481`），但每个 `.cpp` 里的功能块同样带宏。

`uvcpp_http_server` **自己不碰 TLS** —— 它没有任何 SSL API。要上 https 必须
`get_tcp_server()->set_ssl_context(...)`，**而且必须在 `listen()` 之前**
（`tests/functional/web_ssl_h2_client_func.cpp:185`）。握手超时归
`uvcpp_tcp_server`（默认 10000 ms，`src/net/uvcpp_tcp_server.h:449`）。

---

## 3. 最小服务端

这就是 `src/web/uvcpp_http_server.h:149-158` 那段官方示例（我把它补成了完整 TU）：

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

`bind` / `bindIpv4` / `bindIpv6` / `listen` 四个都是**直接转调 `uvcpp_tcp_server`**，
返回 libuv 错误码（`src/web/uvcpp_http_server.cpp:71-89`）。`listen` 的默认 backlog 是
128（`src/web/uvcpp_http_server.h:172`）。

### 关服是两步，没有 `close()`

**`uvcpp_http_server` 的公开 API 里没有 `close()`。** 唯一能"关服"的是 `stop()`
（`:385`），而它 = `tcp_server_->stop()`：**只关监听句柄，不动已建立的连接**
（`src/net/uvcpp_tcp_server.h:190-203`）。已经在处理的请求照跑。

```cpp
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_server.h>

void doc_graceful_shutdown(uvcpp::uvcpp_http_server& server) {
  server.stop();                                   // 1) 不再收新连接
  server.get_tcp_server()->close_all_clients();    // 2) 送走剩下的
}
```

想一步到位就两个都调。连接级的 `close_connection()` 是 **private**
（`src/web/uvcpp_http_server.h:862`）—— 单个连接只能从 `uvcpp_tcp_client` 那侧关。

---

## 4. 路由与匹配规则

注册 API **全部返回 `void`**，没有失败通道（`src/web/uvcpp_http_server.h:246-279`）：

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

**匹配规则（`src/web/uvcpp_http_server.cpp:133-146`）：剥掉 query 再逐字节比路径，
线性扫描，首次命中即返回。** 于是：

- **没有参数、没有通配、没有正则、没有中间件**（`src/webapp/uvcpp_web_router.h:9-14`
  也是这么描述它的）。
- **重复注册同一路径时先注册的赢，后面的永远不可达** —— 而 API 返回 `void`，
  没有任何提示。
- 都没命中 → `default_handler_`；`default_handler_` 为空 → 直接 **404**
  （`src/web/uvcpp_http_server.cpp:484-489`）。
- **分不出 404 和 405**：`get("/x")` 已注册时来一个 `POST /x`，走的还是兜底
  或 404，不会回 405。

路由表就是个 `std::vector`（`src/web/uvcpp_http_server.cpp:108-131`），
`listen()` 之后仍可以改；但它没有锁。

### 请求目标里的 query 要自己解

**web 层不提供任何 query / cookie / form / URL 解码工具，`req.url` 就是原始请求目标
（含 `?query`）。** 路由匹配时会把 query 剥掉再去比（`src/web/uvcpp_http_server.cpp:135-138`），
但**解析 query 是 handler 自己的事** —— 这一层没有这个函数。要用就在 `webapp/` 的
`web_parse_query()`（`src/webapp/uvcpp_web_util.h:179`）或者自己写一个。

### 流式与认领两条旁路

除精确路由外还有两条：

- `post_stream(path, http_stream_handler)`（`:250`）：只支持 POST，命中后
  body 不再被服务器缓冲，直接以 `HEADERS` / `BODY` / `END` 三个事件交给你。
- `set_stream_claim(hook)`（`:263`）：`post_stream` 没中时问它。它返回一个
  `http_stream_handler`（认领）或空（不认领）。`webapp/` 正是靠它把自己的路由器
  接进来的（`src/webapp/uvcpp_web_app.cpp:1683`）。

`stream_handler` 拿到的 `req` 是 `ctx.stream_request`，**头部视图只在当次调用内有效，
要留就自己拷**（`src/web/uvcpp_http_server.h:105-107`）。

---

## 5. 请求与响应对象

**同一对类在两个方向上用**，靠"你拿它干什么"区分，类本身不区分：

| 类 | 服务端 | 客户端 |
|---|---|---|
| `uvcpp_http_request` | 解析结果（handler 的入参） | 待发送报文（`to_string()` 序列化） |
| `uvcpp_http_response` | 待发送报文（handler 填） | 解析结果（回调的入参） |

两侧都有 `static from_parser(const uvcpp_http_parser&, const uvcpp_buf& body)`
（`src/web/uvcpp_http_request.h:108`、`src/web/uvcpp_http_response.h:136`）。

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
  uvcpp::http_method m = req.method;          // 默认 HTTP_GET     src/web/uvcpp_http_request.h:39
  std::string        u = req.url;             // 默认 "/"，含 query  src/web/uvcpp_http_request.h:40
  uvcpp::http_headers h = req.headers;        // name 已转小写      src/web/uvcpp_http_request.h:42
  uvcpp::uvcpp_buf   b = req.body;            // 公开字段，不是方法  src/web/uvcpp_http_request.h:43
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
（`src/web/uvcpp_http_request.cpp:25-36`、`src/web/uvcpp_http_response.cpp:25-42`）。

---

## 6. 异步响应

**handler 返回之前没发响应，就再也没机会发了** —— 除非把 `resp.deferred` 置真。
框架见到它就直接 return（`src/web/uvcpp_http_server.cpp:495-496`），
由你自己在之后（且必须在 loop 线程上）调 `send_response()`。

真实代码里的"之后"是异步完成回调。库内推荐的卸载重活方式是 `uvcpp_work`：

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
                                uvcpp::uvcpp_tcp_client* client) {
    (void)req;
    resp.deferred = true;   // 声明"这次不由框架发"

    // 地址会被复用，所以异步之前先记代次号；回来时对不上就说明换人了。
    const uint64_t gen = server.connection_generation(client);

    uvcpp::uvcpp_work* w = new uvcpp::uvcpp_work();
    w->queue_work(server.get_tcp_server()->get_loop(),
      [](uvcpp::uvcpp_work* self) {
        // 工作线程：这里绝不能碰 server / client / resp
        self->get_loop();   // 只为演示；真活放这儿
      },
      [&server, client, gen](uvcpp::uvcpp_work* self, int status) {
        // loop 线程
        if (status == 0 && server.connection_generation(client) == gen) {
          uvcpp::uvcpp_http_response late =
              uvcpp::uvcpp_http_response::ok("done", 4);
          server.send_response(client, late);
        }
        delete self;
      });
  });
}
```

`connection_generation()`（`src/web/uvcpp_http_server.h:565`）是本层最容易被忽略、
又最容易出致命错的 API：**返回 0 表示这条连接已经不在服务器的登记表里，此时不该写它。**
代次号单调递增、永不复用，所以"连接还活着"时号不变、"地址被新连接复用"时号对不上
（`:944-950`）。`is_connected()` 就是 `generation != 0`（`:568`）。

**`send_response()` 会把 `resp.body` 移走。** 两块写（`nbufs = 2`）时 body 被
`std::move`，之后 `resp.body.size()` 是 0。要记"上线了多少 body"就用它的**返回值**
（`src/web/uvcpp_http_server.h:420-433`）。webapp 正是这么记访问日志的。
`deferred` 的标志位在**检查之后会被清掉**，所以那个 `resp` 对象不必一直活着 ——
但你要发送的那一份得自己留着。

**`stop()` 与 `close_all_clients()` 都不唤醒写队列里的 `done`**
（`src/web/uvcpp_http_server.cpp:59-61` 明说）。

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

**`write_stream` 的 `done` 契约（`src/web/uvcpp_http_server.h:475-498`）：**

- 入队后**保证恰好一次**，**包括连接还在队列里就被关掉的场合**；
- **但只有 `write_stream` 返回 0 才有这个保证** —— 非 0 时 `done` **不会被调**，
  而且**不要两边都调**（`:470-475`）；
- `done` **绝不在函数返回前同步跑**（`:487`）。

**同名方法在 h1 与 h2 上含义不同，而且没有编译期区分手段**
（`src/web/uvcpp_http_server.h:472-519`）：

| | h1 | h2 |
|---|---|---|
| `write_stream(client, bytes, ...)` | `bytes` 是**已组好 chunked 帧**的字节 | `bytes` 是**裸 body** |
| `end_stream(client, close_after)` | 可能关连接 | **不关连接**，只发 `END_STREAM` |
| 在 h2 连接上调 h1 那两个重载 | `write_stream` 返回 `UV_EINVAL`、`end_stream` **静默返回**（`src/web/uvcpp_http_server.cpp:783`、`:811`）/ `begin_stream(client, resp)` 打一条 stderr 后丢弃（`:694-699`） | — |

**`is_head` 与 `accept_encoding` 是连接级单槽，h2 上必须按流记。**
`src/web/uvcpp_http_server.h:664-694` 把三类后果写得很细；`send_h2_response` 里
是临时把"这条流的值借到连接级字段"再调压缩的
（`src/web/uvcpp_http_server.cpp:1486-1494`）。

### 升级到别的协议

`on_upgrade(handler)`（`:229`）是单槽（`src/web/uvcpp_http_server.cpp:95-97`）。
它被调时这条连接已经从 http 解析里摘出来了，之后的字节归你；升级时**已经读进来但
还没解析的字节**用 `take_upgrade_leftover(client)`（`src/web/uvcpp_http_server.h:243` / `src/web/uvcpp_http_server.cpp:99`）取走 ——
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
`std::function<void(const uvcpp_http_response&, int)>`（`src/web/uvcpp_http_client.h:162-177`）。
交付的 `resp` 是成员 `pending_resp_`（`:360`）的 const 引用 —— **存到下次 `send()`
就被覆写**。

**异步路径没有超时，`connect()` 也不设**（`src/web/uvcpp_http_client.cpp:49-53`）。
要超时只能用 `*_wait` 系列。

**构造里那个 close 观察者不能省**（`src/web/uvcpp_http_client.cpp:44-63`）：少了它，
"对端在响应收完前断开"时 `send()` 的回调永远不来。

**在响应回调里 `delete` 客户端是预期的用法。** 所有闭包都捕一个 `alive_token_`
弱引用（`src/web/uvcpp_http_client.h:343-354`），析构第一件事就 reset
（`src/web/uvcpp_http_client.cpp:69`）—— 你不需要为它做任何额外的事。

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
（`src/web/uvcpp_http_client.cpp:224`、`:373`、`:510`、`:524`）。

### 三个容易写错的地方

**一、keep-alive 有两个方向：请求那头听调用方的，响应那头听对端的。**
`keep_alive_` 是**调用方的偏好**（`src/web/uvcpp_http_client.h:338` 初值 `true`、
`:200` 声明、`src/web/uvcpp_http_client.cpp:740` setter）。`set_keep_alive(false)`
会让请求报文带上 `connection: close`（`src/web/uvcpp_http_client.cpp:452-467`）；
默认的 `true` 则不补头，报文里那条 `connection: keep-alive` 是
`uvcpp_http_request::to_string()` 补的（`src/web/uvcpp_http_request.cpp:130-133`）。
**响应**说的同样作数：`on_response_complete()` 按 llhttp 算出的
`should_keep_alive()`（HTTP 版本默认值 + `Connection` 的**逗号列表**）判断这条连接
还能不能再用，不能就当场清掉 `HTTP_CLIENT_CONNECTED`
（`src/web/uvcpp_http_client.cpp:636-656`）—— 此后第二次 `send()` 拿到
`UV_ENOTCONN`，而不是往一条服务端已决定关闭的连接上写。
`tests/functional/web_http_client_keepalive_func.cpp` 把这三条都钉住了。

**二、开了 OpenSSL 的构建里 `send_wait()` 走的是阻塞 fd，不是事件循环。**
`src/web/uvcpp_http_client.cpp:513-530` 是一个 `#if UVCPP_OPENSSL_ENABLE` 分支：
有 TLS 就走阻塞路径，没有才走"轮询 loop + 1ms sleep"的异步实现。
后果是**同一个程序的 keep-alive 行为会随构建而变**（`src/web/uvcpp_http_client.cpp:888-891` 把这段历史写下来了）。

**三、阻塞路径的 `timeout_ms` 只有 Windows 生效。**
`setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)` 两处都被 `#ifdef _WIN32` 包着
（`src/web/uvcpp_http_client.cpp:1006-1010`、`:1057-1061`），POSIX 上**不设**，
`send_wait()` / `send_wait_plain()` 在 Linux/macOS 上可以无限阻塞。头文件现在把这个
缺口写在参数说明里（`src/web/uvcpp_http_client.h:131-133`、`:147-151`），
不再让读者以为有超时兜底。

**四、阻塞路径读不到任何字节时返回 0，而 `resp` 是默认的 200 OK。**
`read_one_message()` 的返回值被丢弃（`src/web/uvcpp_http_client.cpp:1077-1090`），空头交给
`parse_response_head()` 之后什么都不改，`status_code` 保持初值 200。
**判据只能是返回值**。（异步 h1 路径相反：错误时会把 `status_code` 设成
`HTTP_STATUS_NONE`，`src/web/uvcpp_http_client.cpp:394-396`。）

### 没有的东西

**不跟随重定向。** `src/web/uvcpp_http_client.{h,cpp}` 里
`redirect|30[1238]` 零命中 —— 3xx 只是一个状态码交给你。

**压缩默认关，且门槛改不了。** `set_compression_enabled(true)`
（`src/web/uvcpp_http_client.h:206-211`）之后请求会补
`accept-encoding: gzip, deflate`、响应收全后自动解压；但门槛硬编码
`compress_min_body_ = 1024`（`h:389`），**客户端没有 `set_compress_min_body_size()`**。
于是 **< 1024 字节的 gzip 响应不会被解压，`content-encoding: gzip` 也照样留着** ——
调用方拿到的是压缩字节加一个说"这是 gzip"的头。

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
（`src/web/uvcpp_static_server.h:88-89`）—— 上面那个写法（`files` 先于 `server` 声明、
后于它析构）是对的。

默认值（构造参数 `src/web/uvcpp_static_server.h:81-83`、成员初值 `:178-179`）：
`url_root = "/"`、`index_file = "index.html"`、`spa_fallback = true`、`cache_enabled = true`。
后两个**有 setter**（`set_spa_fallback()` `:96`、`set_cache_enabled()` `:99`），
前两个只能走构造函数。
构造函数还会把 `local_dir` 去掉尾斜杠、把 `url_root` 规范成 `"/"` 或 `"/xxx"`
（`src/web/uvcpp_static_server.cpp:165-183`）。

**它是懒加载的**：首次请求某文件才读盘并进内存缓存，此后每次请求在线程池里
stat 一次，mtime/大小变了就异步重载 —— 所以文件改完**下一个请求就返回新内容**，
不需要重启。

### 能力边界（都是它自己声明的）

- **不做 Range / 206、不做 ETag / 304、不做百分号解码**
  （`src/web/uvcpp_static_server.h:28-43`）。
- 防目录穿越靠**段级归一化**（`src/web/uvcpp_static_server.cpp:39-76`），
  并额外拒掉 `\`、`:`、NUL 与控制字符（`:40-44`）。
- **`spa_fallback` 默认开，意味着找不到文件时也返回 200 的 `index.html`**
  （`src/web/uvcpp_static_server.cpp:336-341`）—— 接口写错路径时会静默拿到一页 HTML 而不是 404。
- 防"地址被复用"用的是 `connection_generation`（`src/web/uvcpp_static_server.cpp:414-419`），不是连接 id。
- MIME 表是**硬编码 switch**（`src/web/uvcpp_static_server.cpp:469-502`）；要可配的用 webapp 层的
  `uvcpp_web_static`（`src/webapp/uvcpp_web_static.h`）。

---

## 10. 上限与旋钮

**三个上限的默认值全是 0，也就是全都不限**（`src/web/uvcpp_http_server.h:940-942`）：

| 旋钮 | 默认 | 越限的答复 |
|---|---|---|
| `set_max_body_size(n)` | 0 = 不限 | **413 + `Connection: close`** |
| `set_max_header_bytes(n)` | 0 = 不限 | **431 + `Connection: close`** |
| `set_max_url_bytes(n)` | 0 = 不限 | **414 + `Connection: close`** |

**三者"生效时机"不一致**：头/URL 上限在 accept 时被拷进解析器
（`src/web/uvcpp_http_server.cpp:175-176`）⇒ **listen 之后改它只对新连接生效**；
`max_body_size_` 是每块 body 现读成员（`src/web/uvcpp_http_server.cpp:236`）⇒ **立刻生效**。
三处头文件注释一个字都没提这个区别。

**头/URL 上限对"被 claim 的请求"照样生效。** 这两道是在 llhttp 的头部回调里边收边判的
（`src/web/uvcpp_parser` 侧 `src/web/uvcpp_http_parser.cpp:411-459`），而 claim hook
是在 `headers_complete` 才被问的（`src/web/uvcpp_http_server.cpp:243-291`）。
所以被认领的请求**照样会被 414/431 拒掉并直接关连接**，认领者连 `HEADERS` 事件
都收不到。**只有 `max_body_size` 真正停在认领边界**（`src/web/uvcpp_http_server.cpp:226-242`）——
头文件把这条区别写在了 `src/web/uvcpp_http_server.h:309-312`（「a claimed request is not exempt」）
与 `:112-114`（`max_body_size` 豁免）两处。

**头/URL 是"边收边判"的**：一超就停，代价有界，而不是先收完再判
（`src/web/uvcpp_http_server.h:299-304`）。

### 压缩

zlib 编进来时**压缩默认开**（`src/web/uvcpp_http_server.h:955`），最小 body 1024
（`:956`），排除的 MIME 走 `default_excluded_mime_types()`
（`src/web/uvcpp_http_compress.cpp:144-163`）。压缩变体缓存三道限：
单条 ≤ 4 MiB、总量 ≤ 32 MiB、条数 ≤ 1024（`src/web/uvcpp_http_server.cpp:831-841`）。

### 协议行为

- **keep-alive**：HTTP/1.1 默认开、1.0 默认关；handler 显式设了 `connection`
  头就听 handler 的（`src/web/uvcpp_http_server.cpp:598-612`）。
- **`Expect: 100-continue` 支持**，且在 headers 回调里就补
  `HTTP/1.1 100 Continue\r\n\r\n` 裸字节（`src/web/uvcpp_http_server.cpp:296-304`）—— 注释解释了为什么
  不能用 `send_response()`（1xx 不该带 `Content-Length`）。HTTP/1.0 上 `Expect`
  被忽略（`src/web/uvcpp_http_server.cpp:503-508`）；**未知的 `Expect` 回 417**（`src/web/uvcpp_http_server.cpp:518-523`）。
- **声明的 `Content-Length` 超限 → body 还没到就回 413**（`src/web/uvcpp_http_server.cpp:526-538`）。
- **畸形报文走正规响应路径回 400 + `Connection: close`**（`src/web/uvcpp_http_server.cpp:383-392`），
  不是手搓字节。
- **流水线**：一次读里可以有多条报文，解析器按消息边界 reset
  （`src/web/uvcpp_http_server.cpp:201-223`、`:331-347`）；**web 层不限条数**，webapp 才限。

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
   并返回 0**（`src/web/uvcpp_http_server.cpp:574-582`）；`begin_stream` 同理
   （`src/web/uvcpp_http_server.cpp:678-684`）—— 但它是 `void`，你**无从判断**。

**异常只在一处被接住**：`on_connection` / `stream_claim` / h2 连接钩子三个钩子
里抛出的异常会被吞掉并打 stderr（`src/web/uvcpp_http_server.cpp:187-198`、`:266-277`、`:1354-1363`）。
**`handler(req, resp, client)` 本身没有 try/catch**（`src/web/uvcpp_http_server.cpp:484-489`），
抛出去会穿过 llhttp 的 C 回调栈。webapp 层自己包了 try/catch 并扇出到错误中间件
（`src/webapp/uvcpp_web_app.cpp:2216-2222`）。

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
`src/web/uvcpp_http_server.cpp:373-374`、`:1100-1102` 与头文件
`src/web/uvcpp_http_server.h:877-880` 现在都明写这一点。
**真实的闲置清扫在 webapp 层**：`idle_timeout_ms` 默认 **60000**
（`src/webapp/uvcpp_web_app.cpp:193`），而且为 0 时连扫描句柄都不建
（`:1767-1772`）。升级成 WebSocket 的连接被排除在闲置超时之外
（`src/webapp/uvcpp_web_app.h:807`）。

**二、`req` 与 `resp` 只在本次调用期间有效。**
它们是 `on_request_complete` 的局部对象（`src/web/uvcpp_http_server.cpp:445`、`:464`），
handler 返回即析构。`deferred` 那句是**唯一**的例外通道。

**三、别在回调里做重活。** 所有回调都在 loop 线程上；重活走 `uvcpp_work`
（[§6](#6-异步响应)）。**也不要在回调里用同步 `write_wait` / `read_wait` 变体** ——
`cb == nullptr` 通常**正是**同步版本。

**四、`ctx.stream_request` 的头部视图是借的。** 只在当次调用内有效
（`src/web/uvcpp_http_server.h:105-107`）。

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
- **不做 h2c**：h2 必须走 TLS + ALPN。
- **静态服务不做 Range / ETag / 百分号解码**，且 `spa_fallback` 默认开
  （[§9](#9-静态文件服务)）。
- **`uvcpp_http_server` 不自己管 TLS**，要在 `listen()` 之前装到
  `get_tcp_server()` 上。

---

相关文档：[web WS 指南](./web-ws-guide.md)、[net 网络层指南](./net-guide.md)、
[TLS 指南](./ssl-guide.md)、[HTTP/2 低层指南](./http2-guide.md)、
[webapp 应用框架开发者指南](./webapp-guide.md)、[HTTP/2 支持现状](./http2-status.md)、
[项目 README](../README.zh.md)。

本页用到的头文件：`<web/uvcpp_http_server.h>`、`<web/uvcpp_http_client.h>`、
`<web/uvcpp_http_request.h>`、`<web/uvcpp_http_response.h>`、
`<web/uvcpp_http_common.h>`、`<web/uvcpp_static_server.h>`、
`<net/uvcpp_tcp_client.h>`、`<net/uvcpp_tcp_server.h>`、`<req/uvcpp_work.h>`。
