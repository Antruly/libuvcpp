# web 层 WebSocket 指南

`src/web/` 的 `ws_*` 那半边：RFC 6455 的握手、帧解析、掩码、分片重组、压缩、关闭握手，
以及一个会话表。它建在 `net/` 的 `uvcpp_tcp_client` 之上，与 `web/` 的 HTTP 那半边
（见 [web HTTP 指南](./web-http-guide.md)）**没有共同类、没有共同生命周期** ——
HTTP 的服务端与 WS 的服务端会在同一个 `uvcpp_http_server` 上碰头，但那是接线，不是继承。

- 打开方式：`-DUVCPP_BUILD_WEB=ON`（默认开）；压缩还要 `-DUVCPP_ENABLE_ZLIB=ON`
- 包含方式：`<web/uvcpp_ws_server.h>` / `<web/uvcpp_ws_client.h>` /
  `<web/uvcpp_ws_connection.h>` / `<web/uvcpp_ws_sessions.h>` / `<web/uvcpp_ws_frame.h>`
- 命名空间都是 `uvcpp`

> 本指南里的签名、默认值、行为都对着当前源码核过，非显然的结论后面都跟了
> `文件:行号`。凡是"这一层没做"的地方都明确标出来 —— 那些地方比 API 更容易踩。

**这一层不是 `webapp` 的 WS。** `webapp/` 里的 `uvcpp_web_ws_client` 是**框架层**，
带自动重连、带闲置超时、按路由挂载；本页讲的 `uvcpp_ws_client` 是**一次性**的，
连一次就完，没有重连也没有心跳（见 [§8](#8-客户端) 与 [§15](#15-没做的如实列出)）。
写应用优先用框架层那个，除非你要的就是裸的那一层。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [最小可运行程序](#2-最小可运行程序)
3. [服务端](#3-服务端)
4. [握手](#4-握手)
5. [连接对象的生命周期](#5-连接对象的生命周期)
6. [收](#6-收)
7. [发](#7-发)
8. [客户端](#8-客户端)
9. [关闭](#9-关闭)
10. [会话表](#10-会话表)
11. [压缩与扩展](#11-压缩与扩展)
12. [上限与解析器](#12-上限与解析器)
13. [错误处理](#13-错误处理)
14. [典型坑](#14-典型坑)
15. [没做的（如实列出）](#15-没做的如实列出)

---

## 1. 这一层是什么

| 类 | 头 | 角色 |
|---|---|---|
| `uvcpp_ws_server` | `src/web/uvcpp_ws_server.h:46` | 挂在 HTTP 服务器上做**握手**（识别升级 → 算 Accept → 回 101 → 建会话）。自己不管帧收发 |
| `uvcpp_ws_client` | `src/web/uvcpp_ws_client.h:56` | 解析 `ws://` / `wss://`、发升级请求、校验 101、建会话。**同时持有自己的 loop** |
| `uvcpp_ws_connection` | `src/web/uvcpp_ws_connection.h:61` | 真正的 WS 语义：帧解析与序列化、掩码、分片重组、压缩、Close 握手、发送队列 |
| `uvcpp_ws_sessions` | `src/web/uvcpp_ws_sessions.h:36` | 会话归属表 + **延迟回收器**。不是广播器 |
| `uvcpp_ws_parser` | `src/web/uvcpp_ws_parser.h:38` | 纯字节流解析器。正常用不到，`uvcpp_ws_connection` 内部自己持有一个 |
| `uvcpp_ws_ext` | `src/web/uvcpp_ws_ext.h:110` | `permessage-deflate` 的语法层与策略层 |

**服务端与客户端拿到的是同一个类**（`uvcpp_ws_connection`），靠 `ws_role` 区分方向
（`src/web/uvcpp_ws_connection.h:56`）：服务端 `new uvcpp_ws_connection(client, ws_role::SERVER)`
（`src/web/uvcpp_ws_server.cpp:236`），客户端
`new uvcpp_ws_connection(tcp_, ws_role::CLIENT)`（`src/web/uvcpp_ws_client.cpp:374`）。
两侧唯一的差别是掩码方向（`src/web/uvcpp_ws_connection.cpp:194`）与压缩窗口方向
（`src/web/uvcpp_ws_parser.cpp:278`）。

---

## 2. 最小可运行程序

```cpp
#include <string>

#include <uv.h>
#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_server.h>

void doc_run_echo_server() {
  uvcpp::uvcpp_ws_server ws;
  if (ws.bind("0.0.0.0", 9000) != 0) return;   // bind 只是 bind，不 listen

  ws.on_connection([](uvcpp::uvcpp_ws_connection* c) {
    c->on_text([c](const std::string& msg) {
      // 回显。send_text 是异步的：它把帧推进发送队列就返回（§7）。
      c->send_text(msg.data(), msg.size(), nullptr);
    });
  });

  if (ws.listen(128) != 0) return;   // ← 少了这一句，永远不会有连接进来
  ws.run(UV_RUN_DEFAULT);
}
```

**`listen()` 是独立的一步，头文件里的官方示例漏了它。** `src/web/uvcpp_ws_server.h:11-20`
的 "Standalone usage" 是 `bind` → `on_connection` → `run`，**没有 `listen()`** ——
照抄那段代码的服务端会 bind 成功、跑起来、然后一条连接都收不到。
`bind()` 只转发 `http_server_->bind`（`src/web/uvcpp_ws_server.cpp:151`），
`listen()` 才转发 `http_server_->listen`（`src/web/uvcpp_ws_server.cpp:155`）。
完整顺序照 `tests/functional/web_ws_func.cpp:24`。

---

## 3. 服务端

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
explicit uvcpp_ws_server();                          // src/web/uvcpp_ws_server.h:48
explicit uvcpp_ws_server(uvcpp_http_server* http);   // src/web/uvcpp_ws_server.h:63
int  bind(const char* ip, int port);                 // src/web/uvcpp_ws_server.h:69
int  listen(int backlog = 128);                      // src/web/uvcpp_ws_server.h:70
void attach(uvcpp_http_server* http);                // src/web/uvcpp_ws_server.h:77
void on_connection(std::function<void(uvcpp_ws_connection*)> cb);  // :115
int  run(uv_run_mode md = UV_RUN_DEFAULT);           // :121
void stop(std::function<void()> on_stopped = nullptr);  // :129
void close_all_sessions(ws_close_code code = ws_close_code::NORMAL);  // :145
size_t session_count() const;                        // :191
```

**没有 `close()`。** 停机只有两条路：`stop()` 和 `close_all_sessions()`。
`stop()` 先 `close_all_sessions(GOING_AWAY)` 再停底下的 HTTP 服务器
（`src/web/uvcpp_ws_server.cpp:366`）—— 也就是说正常停机**会**给每条连接发 1001。

**服务端只有 `on_connection` 一个回调。** 没有 `on_message`、没有 `on_close`、
没有 `on_error` —— 那些全部在 `uvcpp_ws_connection` 上装（§5）。
`on_connection` 是**单槽**（`on_conn_ = std::move(cb)`，`src/web/uvcpp_ws_server.cpp:279`），
后装的覆盖先装的，不会报错。

---

## 4. 握手

用户**什么都不用写**。`handle_upgrade` 一条龙包办：算 `Sec-WebSocket-Accept`
（`src/web/uvcpp_ws_server.cpp:200`）→ 拼 101 应答 → `write` 出去 → 在**写完成回调**里
建会话（`src/web/uvcpp_ws_server.cpp:217-262`）。顺序上有三处做对了，写文档时值得知道：

- 101 写失败（`status != 0`）直接返回，**不建会话也不回调**（`src/web/uvcpp_ws_server.cpp:221`）；
- `shard->adopt(conn)` 在 `conn->start()` **之前**（`:239`）—— 会话先归表，再开始收帧；
- 升级请求里多读出来的字节在用户回调**之前**取走、在用户回调**之后**补投（`:250`）。

### 子协议完全没有

`Sec-WebSocket-Protocol` 在两侧都**不存在**：服务端应答里只写 `Upgrade` / `Connection` /
`Sec-WebSocket-Accept` / 可选 `Sec-WebSocket-Extensions`（`src/web/uvcpp_ws_server.cpp:202`），
客户端请求里只写 `Host` / `Upgrade` / `Connection` / `Sec-WebSocket-Key` /
`Sec-WebSocket-Version: 13` / 可选扩展（`src/web/uvcpp_ws_client.cpp:288`）。
要协商子协议得在 HTTP 层自己做：用 `handle_upgrade` 的重载
（`src/web/uvcpp_ws_server.h:108`）或者自持 `uvcpp_http_server::on_upgrade` 槽，
读请求头、自己回 101。

### 缺 `Sec-WebSocket-Key` 时是**静默**的

`src/web/uvcpp_ws_server.cpp:186` 是 `if (ws_key.empty()) return;` —— 不建会话、不应答，
连接既不断也不回，就哑在那里。自己拿 `on_upgrade` 槽分派的人**必须自己先校验**
（头文件 `src/web/uvcpp_ws_server.h:86` 如实写明了这一点）。

---

## 5. 连接对象的生命周期

**`uvcpp_ws_connection` 是托管的，用户不得 `new`、不得 `delete`。**
它的分配与释放都在 `web/uvcpp_ws_sessions` 手里（`src/web/uvcpp_ws_connection.h:10-23`）。

回调里拿到的那个指针**不能缓存**。终结之后属主会在**下一轮循环**里 `delete` 它
（`src/web/uvcpp_ws_sessions.cpp:115`）—— 延迟一轮是为了让"写完成回调里还握着会话指针"
这类在途引用先跑完。要判它还在不在，只有两个判据：

```cpp
// doc-snippet: fragment — 两行签名摘录，不是完整翻译单元
bool is_open() const;                 // src/web/uvcpp_ws_connection.h:136
uvcpp_tcp_client* get_tcp_client();   // src/web/uvcpp_ws_connection.h:249，终结后返回 nullptr
```

所有回调都在**跑 `run()` 的那个线程**上同步调用（解析器帧回调 → `on_ws_frame` →
用户回调，`src/web/uvcpp_ws_parser.cpp:193` → `src/web/uvcpp_ws_connection.cpp:180`）。

### 在回调里装回调是正常写法

```cpp
#include <string>

#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_frame.h>

void doc_install(uvcpp::uvcpp_ws_connection* c) {
  c->on_text([](const std::string& msg) { (void)msg; });          // :166
  c->on_binary([](const uint8_t* p, size_t n) { (void)p; (void)n; });  // :167
  c->on_ping([](const uint8_t* p, size_t n) { (void)p; (void)n; });    // :168
  c->on_pong([](const uint8_t* p, size_t n) { (void)p; (void)n; });    // :169
  c->on_close([](uvcpp::ws_close_code code, const std::string& reason) {
    (void)code; (void)reason;
  });                                                             // :183
  c->on_error([](int code, const std::string& reason) {
    (void)code; (void)reason;
  });                                                             // :192
}
```

`on_close` 的触发条件只有两种（`src/web/uvcpp_ws_connection.h:176`）：

- 对端发了 Close 帧 → 帧里的码与原因；
- 对端**没发** Close 就断 → `ABNORMAL_CLOSE`（1006）加空原因。

**本端自己发起的结束不会再触发它** —— `close()` 里第一句就是
`close_notified_ = true`（`src/web/uvcpp_ws_connection.cpp:104`）。

---

## 6. 收

`on_text` / `on_binary` **每条完整消息各一次**，分片已经在层内重组好了
（`src/web/uvcpp_ws_connection.h:169`）。`data` 指针只在回调期间有效 ——
实现里 `message_payload_.clear()` 紧跟在回调之后（`src/web/uvcpp_ws_connection.cpp:389`），
存下来就是悬垂。

**控制帧与数据帧可以交错**：`src/web/uvcpp_ws_connection.cpp:208` 的控制帧分支不碰
`in_message_` 那几个状态，所以一条长分片消息中间夹一个 ping 是合法的、也是正常的。
反过来，**数据帧插在分片消息中间是协议错误**（`src/web/uvcpp_ws_connection.cpp:257`）。

两条"层内自动做掉"的事，使用者不用管：

- **掩码**：接收侧在解析器里就地 XOR，且**跨多次 read 的相位是对的**
  （`off = payload_received_ % 4`，`src/web/uvcpp_ws_parser.cpp:166`）；
- **收 ping 自动回 pong**（`src/web/uvcpp_ws_connection.cpp:210`）—— 注意这是**被动**回，
  这一层没有主动发心跳的定时器（§14）。

---

## 7. 发

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
int send_text(const char* data, size_t len,
              std::function<void(int)> cb = nullptr);     // src/web/uvcpp_ws_connection.h:158
int send_binary(const char* data, size_t len,
                std::function<void(int)> cb = nullptr);   // src/web/uvcpp_ws_connection.h:159
int send_ping(const char* data = nullptr, size_t len = 0);  // :160
int send_pong(const char* data = nullptr, size_t len = 0);  // :161
int send_close(ws_close_code code = ws_close_code::NORMAL,
               const std::string& reason = "");           // src/web/uvcpp_ws_connection.h:162
```

**没有分片发送 API。** 每次 `send_data` 造一个 `uvcpp_ws_frame`（默认 `fin = true`，
`src/web/uvcpp_ws_frame.h:60`），永远单帧发出（`src/web/uvcpp_ws_connection.cpp:492`）
—— 分片只在**接收**方向被重组。

**`send_ping` / `send_pong` 没有完成回调**（对比 `send_text` / `send_binary`），
失败只能看返回值。

**连接已经终结时**，`send_frame` 第一句是
`if (!tcp_) { if (cb) cb(-1); return -1; }`（`src/web/uvcpp_ws_connection.cpp:412`）：

- 返回**裸 `-1`**，**不是** `UV_ENOTCONN` 之类的 libuv 码 —— 别拿 `uv_strerror()` 解释它；
- 传了回调的话，回调**同步**被叫 `cb(-1)`；
- 不传回调就是静默丢弃。

如果发送通道此前已经坏过，走的是粘性错误 `send_error_`
（`src/web/uvcpp_ws_connection.cpp:418`）：所有排队的帧都以同一个错误结算。

---

## 8. 客户端

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
int  connect(const std::string& url,
             std::function<void(uvcpp_ws_connection*, int error)> cb);  // src/web/uvcpp_ws_client.h:66
int  connect_wait(const std::string& url, uvcpp_ws_connection*& out_conn,
                  int timeout_ms = 30000);                              // src/web/uvcpp_ws_client.h:70
uvcpp_ws_connection* session() const;                                   // src/web/uvcpp_ws_client.h:83
void close(ws_close_code code = ws_close_code::NORMAL,
           const std::string& reason = std::string());                  // src/web/uvcpp_ws_client.h:122
int  run(uv_run_mode md = UV_RUN_DEFAULT);                              // src/web/uvcpp_ws_client.h:129
void stop();                                                            // src/web/uvcpp_ws_client.h:130
void set_ssl_context(uvcpp_ssl_context* ctx);                           // src/web/uvcpp_ws_client.h:169
```

```cpp
#include <string>

#include <uv.h>
#include <web/uvcpp_ws_client.h>
#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_frame.h>

void doc_run_client() {
  uvcpp::uvcpp_ws_client cli;
  // 转发回调可以先装：客户端每建一个新会话都会把它们装上去。
  cli.on_text([](const std::string& msg) { (void)msg; });
  cli.on_close([](uvcpp::ws_close_code code, const std::string& reason) {
    (void)code; (void)reason;
  });
  cli.on_error([](int code, const std::string& reason) {
    (void)code; (void)reason;
  });

  cli.connect("ws://127.0.0.1:9000/chat",
              [](uvcpp::uvcpp_ws_connection* c, int err) {
                if (err != 0 || c == nullptr) return;
                c->send_text("hello", 5, nullptr);
              });

  cli.run(UV_RUN_DEFAULT);   // 客户端持自己的 loop
  cli.close();               // 要优雅关闭用 close()，别用 stop()
}
```

### 这一层没有自动重连

退避、上限、放弃条件**全都是零**，因为**根本没有重连代码**。
`src/web/` 下 grep `reconnect` 只命中注释（`src/web/uvcpp_ws_client.cpp:120`、
`src/web/uvcpp_ws_client.cpp:275`），那两处指向的是 `webapp/` 的框架层客户端。
`uvcpp_ws_client` 是**一次性**的：连一次、用它、结束。
要重连就自己写，或者直接用 [webapp 指南 §11](./webapp-guide.md#11-websocket-客户端含自动重连)
里的 `uvcpp_web_ws_client`。相应地，也**没有"停止重连"这个 API**。

### 客户端没有 `on_ping` / `on_pong` 转发

`cli.on_*` 只有 text / binary / close / error 四个（`src/web/uvcpp_ws_client.h:105-108`），
而连接层有 ping / pong（`src/web/uvcpp_ws_connection.h:174`）。要观测 ping/pong 得
`cli.session()->on_ping(...)` 自己拿指针装 —— 注意 `session()` 终结后返回 `nullptr`
（`src/web/uvcpp_ws_client.cpp:424`）。连接层仍会自动回 pong，不受影响。

### 状态

```cpp
// doc-snippet: fragment — 枚举定义摘录，不是完整翻译单元
enum ws_client_status : int {     // src/web/uvcpp_ws_client.h:34
  WS_CLIENT_NONE       = 0x00,
  WS_CLIENT_CONNECTING = 0x01,
  WS_CLIENT_OPEN       = 0x02,
  WS_CLIENT_CLOSING    = 0x04,
  WS_CLIENT_CLOSED     = 0x08,
  WS_CLIENT_ERROR      = 0x10,
};
```

`has_status(flags)` 的实现是 `(status_ & flags) == flags`（`src/web/uvcpp_ws_client.cpp:504`），
而 `WS_CLIENT_NONE` 是 `0x00` —— 所以 **`has_status(WS_CLIENT_NONE)` 恒为真**。
只有一个 flag 能用，多选是可以的，但别拿 `NONE` 当"什么都没发生"用。

### WSS

`wss://` 才会置 `use_tls`、默认端口 443（`src/web/uvcpp_ws_client.cpp:207`），
TLS 本身由 `tcp_->enable_tls(ssl_ctx_)` 的 memory-BIO 过滤器做（`:277`）。

---

## 9. 关闭

```cpp
// doc-snippet: fragment — 两行签名摘录，不是完整翻译单元
void close(ws_close_code code = ws_close_code::NORMAL,
           const std::string& reason = std::string());   // src/web/uvcpp_ws_connection.h:144
void terminate();                                        // src/web/uvcpp_ws_connection.h:153
```

`close()` 的实现只有两句（`src/web/uvcpp_ws_connection.cpp:100`）：
置 `close_notified_`，然后 `send_close(code, reason)`。而 `send_close` 的完成回调是
"自己的 Close 帧一写进 TCP，就立刻关底层连接"
（`src/web/uvcpp_ws_connection.cpp:590`）。

**也就是说它不等对端回 Close 帧。** RFC 6455 §7.1.1 的关闭握手在这一层只做了一半，
而且**没有任何超时或定时器**兜底。头文件 `src/web/uvcpp_ws_connection.h:138` 的措辞与实现
是相符的，但 `src/web/uvcpp_ws_sessions.h:107` 与 `src/web/uvcpp_ws_server.h:123` 把它称作
"优雅关闭" —— 那是**名义上的优雅**，不是 RFC 的完整语义。

`terminate()` 是立即终结、**不发** Close 帧（`src/web/uvcpp_ws_connection.cpp:109`）。
两个别混。

`send_close` 会把 reason **静默截断到 123 字节**（`src/web/uvcpp_ws_connection.cpp:588`）——
控制帧负载上限 125，扣掉 2 字节状态码。截断是有意的：不截就会拼出一个对端
**必须拒绝**的帧。

### 关闭码

```cpp
// doc-snippet: fragment — RFC 6455 §7.4 的枚举摘录，凑成完整 TU 需要造一个
// 包围它的假上下文，而这里要展示的正是"这一层定义了哪些码"。
enum class ws_close_code : uint16_t {   // src/web/uvcpp_ws_frame.h:41
  NORMAL            = 1000,   // 正常关闭
  GOING_AWAY        = 1001,   // 端点即将消失；stop() 发的就是它
  PROTOCOL_ERROR    = 1002,   // 协议错误
  UNSUPPORTED_DATA  = 1003,   // 收到不支持的数据类型
  NO_STATUS         = 1005,   // 无状态码 —— 不能出现在线上帧里
  ABNORMAL_CLOSE    = 1006,   // 无 Close 帧地断开 —— 同样不能上线路
  INVALID_PAYLOAD   = 1007,   // 负载非法（非 UTF-8、解压失败）
  POLICY_VIOLATION  = 1008,   // 策略违规
  MESSAGE_TOO_BIG   = 1009,   // 消息过大
  EXTENSION_NEEDED  = 1010,   // 客户端要求协商扩展，服务端没给
  INTERNAL_ERROR    = 1011,   // 服务端遇到意外情况
};
```

枚举本身**一条注释都没有**（`src/web/uvcpp_ws_frame.h:41-53` 只有一行 RFC 标题），
上表里的含义是按 RFC 补的。缺 1004、1012 / 1013 / 1014。

**没有"1005 / 1006 不能发"的保护**：`send_close(NO_STATUS)` 会照发一个含 1005 的帧
—— `send_close` 只截断长度，不校验码（`src/web/uvcpp_ws_connection.cpp:584`）。

---

## 10. 会话表

`uvcpp_ws_sessions` 是**归属表 + 延迟回收器**，**不是广播器** ——
没有任何 `broadcast` / `send_all`（`src/web/uvcpp_ws_sessions.h:1-33` 说明了定位）。
服务端与客户端各持一个，用户一般不直接调它，但它的两条性质使用者会撞上：

- **会话没有 id。** `uvcpp_ws_connection` 上也没有。要按 id 找得自己在
  `on_connection` 里建映射。
- **零线程安全。** 没有 mutex、没有原子量。`set_loop` 的注释明确要求必须在
  **该循环的线程上、且循环正在跑的时候**调用（`uv_init` 系列不是线程安全的，
  `src/web/uvcpp_ws_sessions.h:70`）；`adopt` / `close_all` / `recycle_all` 同理。

还有一条是**给你写代码时的约束**（`src/web/uvcpp_ws_sessions.h:97`）：

- 在终结观察者里**不要 `delete` 会话** —— 会和 `drain()` 双删；
- 也**不要做耗时操作**，那是循环线程。

---

## 11. 压缩与扩展

**只支持 `permessage-deflate`（RFC 7692），没有别的扩展。**
`rsv2` / `rsv3` 在帧结构里标着"保留给 RFC 8441（HTTP/2 上的 WebSocket）"，
但**没有实现** —— 收到 rsv2/rsv3 一律回 1002
（`src/web/uvcpp_ws_connection.cpp:238`，理由串写的是
`"RSV2/RSV3 set but no extension negotiated"`）。

**用 `uvcpp_ws_server` / `uvcpp_ws_client` 的人什么都不用做**：两侧默认**开启**协商
（`src/web/uvcpp_ws_ext.h:126` 的 `bool enabled = true;`），服务端建会话时自动
`conn->enable_compression(true, dp)`（`src/web/uvcpp_ws_server.cpp:243`），
客户端自动 `conn->enable_compression(false, deflate_params_)`
（`src/web/uvcpp_ws_client.cpp:384`）。要关掉就 `set_compression(cfg)` 把 `enabled` 置 false
（`src/web/uvcpp_ws_server.h:222` / `src/web/uvcpp_ws_client.h:164`，**只在
`UVCPP_ZLIB_ENABLE=1` 时编译**）。

阈值 `set_compress_min_size(size_t n)` 默认 `0`，也就是**都压**
（`src/web/uvcpp_ws_connection.h:242`）。

只有"自己手搓握手"的路线才需要手动调
`conn->enable_compression(bool is_server, const uvcpp_ws_deflate_params& p)`
（`src/web/uvcpp_ws_connection.h:230`）—— 注意 `is_server` **必填、无默认值**，
理由写在 `src/web/uvcpp_ws_connection.h:222`。

**如实记一条边界**：解压**不检测截断**（`src/web/uvcpp_ws_parser.h:179-187` 自己写明了）。

---

## 12. 上限与解析器

**两级上限，都必须卡**（`src/web/uvcpp_ws_connection.h:204`、`src/web/uvcpp_ws_parser.h:96`）：

```cpp
// doc-snippet: fragment — 两个 setter 的签名摘录，展示"两级都要设"这件事；
// 单独包成 TU 也编不出什么，它们要连着 §12 的说明读。
void set_max_frame_size(uint64_t n);    // src/web/uvcpp_ws_parser.h:100，默认 0 = 不限
void set_max_message_size(size_t n);    // src/web/uvcpp_ws_connection.h:213，默认 16 MiB
```

- 解析器的**单帧**上限默认 `0`，就是**不限**（`src/web/uvcpp_ws_parser.h:238`，
  检查点在长度字段解析完成那一刻，`src/web/uvcpp_ws_parser.cpp:201`）；
- 连接层的**单条消息**上限默认 `16u * 1024u * 1024u` = 16 MiB
  （`src/web/uvcpp_ws_connection.h:343`）。

两条都要设的理由是**它们互相绕得过**：分片绕得过单帧（每条分片都合法但合起来巨大），
单帧绕得过消息（一个声明了 1 GiB 长度的帧根本不进聚合）。连接层构造时会把消息上限
同步给解析器当单帧上限（`src/web/uvcpp_ws_connection.cpp:32`）。

解析器本身使用者正常不用碰。要用的时候记两条：`execute()` 一次只吃一帧、
完成后停在 `COMPLETE` 且**不自己 reset**（`src/web/uvcpp_ws_parser.cpp:62`）；
帧回调传的是内部 `frame_` 的**引用**（`src/web/uvcpp_ws_parser.cpp:193`），
回调返回后即失效。

---

## 13. 错误处理

**错误码域不统一，别把它当 libuv 码用。**

| 场景 | 表达 | 位置 |
|---|---|---|
| 收到非法帧 | 先发对应 Close 帧再关连接，然后 `on_error(code, reason)` | `src/web/uvcpp_ws_connection.cpp:392` |
| 消息聚合超限 | 1009 加 `on_error` | `src/web/uvcpp_ws_connection.cpp:283` |
| 文本消息不是合法 UTF-8 | 1007 加 `on_error` | `src/web/uvcpp_ws_connection.cpp:375` |
| 解压失败 | 1007 加 `on_error` | `src/web/uvcpp_ws_connection.cpp:359` |
| 掩码方向不对 | 1002 加 `on_error` | `src/web/uvcpp_ws_connection.cpp:195` |
| 对端没发 Close 就断 | `on_close(ABNORMAL_CLOSE, "")` | `src/web/uvcpp_ws_connection.cpp:92` |
| 终结时队列里还有帧 | 逐条以 `-1` 结算回调 | `src/web/uvcpp_ws_connection.cpp:136` |
| 客户端无会话时 send | 返回 `UV_ENOTCONN` 并**立刻**回调 | `src/web/uvcpp_ws_client.cpp:434` |
| 握手响应不是 101 | `on_handshake_complete(-2)` | `src/web/uvcpp_ws_client.cpp:320` |
| 扩展应答非法 | `on_handshake_complete(-3)` | `src/web/uvcpp_ws_client.cpp:335` |
| `connect_wait` 超时 | 返回 `UV_ETIMEDOUT` | `src/web/uvcpp_ws_client.cpp:244` |
| 连接中被 `close()` 取消 | `cb(nullptr, UV_ECANCELED)` | `src/web/uvcpp_ws_client.cpp:492` |

连接层用裸 `-1`，客户端握手用 `-1` / `-2` / `-3`，`connect_wait` 用 `UV_ETIMEDOUT`，
取消用 `UV_ECANCELED`，底下的 TCP 层又是 libuv 码。
**别承诺 `get_last_error()` 的返回值是 libuv 码。**

---

## 14. 典型坑

**头文件里的服务端示例漏了 `listen()`。** 见 [§2](#2-最小可运行程序)。
`src/web/uvcpp_ws_server.h:11-20` 的原样照抄会得到一个收不到连接的服务端。

**`close()` 不等对端回帧，也没有超时。** 见 [§9](#9-关闭)。

**客户端 `wss://` 撞上没有 OpenSSL 的构建时会"哑掉"。**
`src/web/uvcpp_ws_client.cpp:210` 是 `#if !UVCPP_OPENSSL_ENABLE` 下的
`last_error_ = -1; return -1;` —— **不设 `status_`、也不调 connect 回调**，
调用方永远等不到通知。而同一份"没有上下文"的失败在 `:227` 那条路上走的是
`on_handshake_complete(-1)`，是会回调的。两种失败行为不同。

**`get_last_error()` 也不是这套失败的统一出口。** 见 [§13](#13-错误处理)。

**重复 `connect()` 会累积会话。** `connect()` 不检查已有会话
（`src/web/uvcpp_ws_client.cpp:198`），新会话直接覆盖 `session_`，旧的**仍在
`sessions_` 里**、回调也还是旧的那一组。`on_session_retired` 里那句
`if (conn != session_) return;`（`src/web/uvcpp_ws_client.cpp:423`）只防"清错对象"，
不回收旧会话。

**`attach()` 会静默顶掉别人装的升级处理器。**
`src/web/uvcpp_ws_server.cpp:164` 先 `delete` 掉自己拥有的 http server，然后
`http->on_upgrade(...)` —— 而 `on_upgrade` 在 HTTP 层是**单槽**
（`src/web/uvcpp_http_server.cpp:175-175`）。谁后 `attach()` 谁赢。

**`on_connection` 是单槽，而且会被 `handle_upgrade` 的 `on_ready` 顶掉。**
`src/web/uvcpp_ws_server.cpp:254` 是 `if (on_ready) { on_ready(conn); }
else if (on_conn_) { on_conn_(conn); }` —— 用带 `on_ready` 的那个重载时，
**不会**再走 `on_connection`。

**单帧超限被映射成 1002，消息超限是 1009。** `src/web/uvcpp_ws_connection.cpp:171`
把**任何**解析错误一律转成 `PROTOCOL_ERROR`，包括解析器的 `-6`（单帧超限）；
而消息聚合超限走的是 `MESSAGE_TOO_BIG`（`:283`）。同一份 `max_message_size_`
值下达的两级限制，对端看到的码不同 —— 对端按 RFC 会把 1009 当"我扩容量重试"、
把 1002 当"你实现错了"。

**在回调里析构客户端 = 有意泄漏。**
`~uvcpp_ws_client` 有一条专门的路径（`src/web/uvcpp_ws_client.cpp:146`）：
`sessions_.abandon()`，loop / tcp / 会话**全部不释放**。理由是三层 use-after-free，
PageHeap 下实测 SEGFAULT（`src/web/uvcpp_ws_client.cpp:124-145`）。
头文件 `src/web/uvcpp_ws_client.h:43-55` 专门写了这一条：
"回调里 `delete cli` 不崩"是承诺，但代价是漏对象。

**析构不会发 GOING_AWAY。** `~uvcpp_ws_server` 先 `sessions_.shutdown()` 再
`delete http_server_`（`src/web/uvcpp_ws_server.cpp:123`），**不调 `stop()`** ——
析构路径上对端只会看到连接消失。要 1001 就显式 `stop()`。

---

## 15. 没做的（如实列出）

- **没有 `Sec-WebSocket-Protocol` 子协议协商**（[§4](#4-握手)）。
- **没有主动心跳、没有闲置超时、没有任何默认的定时器。** 唯一的 ping 相关行为是
  收到 ping 自动回 pong（`src/web/uvcpp_ws_connection.cpp:210`）。
- **`uvcpp_ws_client` 没有自动重连** —— 那是 `webapp/` 框架层的事（[§8](#8-客户端)）。
- **没有分片发送 API**（[§7](#7-发)）；分片只在接收方向重组。
- **没有广播 API、没有会话 id**（[§10](#10-会话表)）。
- **没有 `rsv2` / `rsv3` 扩展**（HTTP/2 上的 WebSocket 没做）。
- **关闭握手只有一半**：发了 Close 就关连接，不等对端回帧、也没有超时（[§9](#9-关闭)）。
- **解压不检测截断**（`src/web/uvcpp_ws_parser.h:179`）。
- **`send_close` 不校验关闭码**，1005 / 1006 发得出去（[§9](#9-关闭)）。

---

相关文档：[web HTTP 指南](./web-http-guide.md)、[webapp 应用框架开发者指南](./webapp-guide.md)、
[net 网络层指南](./net-guide.md)、[项目 README](../README.zh.md)。

本页用到的头文件：`<web/uvcpp_ws_server.h>`、`<web/uvcpp_ws_client.h>`、
`<web/uvcpp_ws_connection.h>`、`<web/uvcpp_ws_sessions.h>`、`<web/uvcpp_ws_parser.h>`、
`<web/uvcpp_ws_ext.h>`、`<web/uvcpp_ws_frame.h>`、`<web/uvcpp_http_server.h>`。
