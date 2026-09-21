# HTTP/2 低层指南

`src/http2/` 只有两个公开类，分成两层：

| 层 | 类 | 干什么 | 碰不碰 socket |
|---|---|---|---|
| 协议层 | `uvcpp_h2_session` | 收发帧、维护流表、判协议违例、算流控。**只跟内存说话** | 不碰 |
| 传输层 | `uvcpp_h2_connection` | 把 session 榨出来的字节写进 `uvcpp_tcp_client`，把读到的字节喂回去；一次只允许一个异步写在飞 | 碰（经由 `net/`） |

**这一层不会自己接线到 socket。** 建会话、判 ALPN、装读回调、销毁，全都要调用方自己做。
`web/` 的两条线（`uvcpp_http_server` / `uvcpp_http_client`）各有一份接好的实现可以照抄，
本页在 [§5](#5-服务端怎么回一条响应) 与 [§6](#6-客户端怎么发一条请求) 给了它们的调用序列。

- 打开方式：`-DUVCPP_ENABLE_NGHTTP2=ON`。**默认 OFF**（`CMakeLists.txt:75`），
  而且缺 OpenSSL 或缺 web 模块时会被**强制**置 OFF 并打 warning
  （`CMakeLists.txt:256`、`CMakeLists.txt:265`）—— 因为 `uvcpp_h2_stream`
  里直接放 `uvcpp_http_request` / `uvcpp_http_response`
  （`src/http2/uvcpp_h2_session.h:70`）。
- 包含方式：`<http2/uvcpp_h2_session.h>`、`<http2/uvcpp_h2_connection.h>`、
  `<http2/uvcpp_h2_common.h>`。私有的 `<http2/uvcpp_h2_nghttp2.h>` **不安装**
  （`CMakeLists.txt:1077`）。

> 本页讲**怎么用**。它与 [HTTP/2 支持现状](./http2-status.md) 分工：那篇讲
> **实现进度、写死的折衷、已知缺口**，本页不重复那些，需要时直接链过去。
>
> 签名、默认值、行为都对着当前源码核过，非显然的结论后面都跟了 `文件:行号`。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [编译期条件](#2-编译期条件)
3. [会话层的最小用法](#3-会话层的最小用法)
4. [传输层](#4-传输层)
5. [服务端怎么回一条响应](#5-服务端怎么回一条响应)
6. [客户端怎么发一条请求](#6-客户端怎么发一条请求)
7. [三个回调什么时候来](#7-三个回调什么时候来)
8. [上限与旋钮](#8-上限与旋钮)
9. [错误处理](#9-错误处理)
10. [典型坑](#10-典型坑)
11. [没做的（如实列出）](#11-没做的如实列出)

---

## 1. 这一层是什么

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
explicit uvcpp_h2_session(bool server_side);                      // src/http2/uvcpp_h2_session.h:187
int init(const callbacks& cbs,
         size_t max_header_list_size = 64u * 1024u,
         uint32_t max_concurrent_streams = H2_DEFAULT_MAX_CONCURRENT_STREAMS,
         uint32_t initial_window_size = 0);                       // src/http2/uvcpp_h2_session.h:201
int recv(const char* data, size_t len);                           // src/http2/uvcpp_h2_session.h:217
int drain(std::string& out);                                      // src/http2/uvcpp_h2_session.h:227
```

**服务端还是客户端由构造参数一次性决定，构造后改不了。** 也没有 `is_server()` 之类的读法
（`src/http2/uvcpp_h2_session.h:187` 是唯一一处）—— 外部要问侧只能自己记。
`server_side` 影响三件事：伪头白名单（`src/http2/uvcpp_h2_session.cpp:300`）、
常规头分流（`:431`）、body 收尾分派（`:508`）。

**一条流不是 `uvcpp_h2_connection`，是 `uvcpp_h2_stream`。**
`uvcpp_h2_connection` 代表**整条连接**（一个 h2 会话），会话内部用一个
`std::map<int32_t, uvcpp_h2_stream>` 按 stream id 管多条流
（`src/http2/uvcpp_h2_session.cpp:182`）。`uvcpp_h2_stream` 只在会话存活期间有效，
`on_close` 之后指针即失效（`src/http2/uvcpp_h2_session.h:63`）。

---

## 2. 编译期条件

`UVCPP_NGHTTP2_ENABLE` **在任何构建里都有定义**，只是值不同：ON ⇒ `1`，OFF ⇒ `0`
（`CMakeLists.txt:589`）。三个公开头把**全部内容**包在 `#if UVCPP_NGHTTP2_ENABLE` 里
（`src/http2/uvcpp_h2_common.h:19`、`src/http2/uvcpp_h2_session.h:33`、`src/http2/uvcpp_h2_connection.h:27`）。

为 0 时：头**还在**（源码树里能 include、也编得过），但里面**一个类型都不声明** ——
写 `uvcpp_h2_session s(true);` 会"未定义类型"。安装出来的包里则**根本没有这几个头**
（`CMakeLists.txt:1074` 只在开关 ON 时装）。

所以使用者的写法是把自己的那段包起来：

```cpp
#include <uvcpp/uvcpp_config.h>

#if UVCPP_NGHTTP2_ENABLE
// 这里才能用 uvcpp_h2_session / uvcpp_h2_connection
#endif
```

**别自己给使用者 TU 加 nghttp2 的 include 路径、也别自己 include `<nghttp2/nghttp2.h>`。**
这库把"唯一包含 nghttp2"收在一个私有头里（`src/http2/uvcpp_h2_nghttp2.h`），
其中有一段 MSVC 专用的 `#define ssize_t int` 补丁（`:42`）—— 写 `SSIZE_T`
会静默 ABI 不符。

---

## 3. 会话层的最小用法

这一层**不碰 socket**，所以最小的用法就是"两个会话面对面搬字节"。
`tests/functional/h2_session_func.cpp` 就是这么测的，`uvcpp_h2_connection`
做的也正是这两段搬运（[§4](#4-传输层)）。

```cpp
#include <cstdint>
#include <string>

#include <http2/uvcpp_h2_session.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

void doc_h2_memory_pair() {
  uvcpp::uvcpp_h2_session server(/*server_side=*/true);
  uvcpp::uvcpp_h2_session client(/*server_side=*/false);

  uvcpp::uvcpp_h2_session::callbacks sc;
  sc.on_request = [&server](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& st, bool) {
    // 提交只是**排队**，一个字节都还没出去。
    server.submit_response(st.stream_id, uvcpp::uvcpp_http_response::ok("ok", 2));
  };
  server.init(sc);

  uvcpp::uvcpp_h2_session::callbacks cc;
  cc.on_response_end = [](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& st) {
    (void)st.response.status_code;   // 到这儿才是"响应全收完了"
  };
  client.init(cc);

  // 提交请求（返回新流号，负值是错误码）。
  const int32_t sid = client.submit_request(uvcpp::uvcpp_http_request::make_get("/"), "");
  if (sid <= 0) return;
  (void)sid;

  // 搬字节：drain 出来的喂给对端，recv 吃进去；两边各来一轮。
  std::string wire;
  if (client.drain(wire) != 0 || wire.empty()) return;
  if (server.recv(wire.data(), wire.size()) != 0) return;   // 这里会同步跑 on_request

  wire.clear();
  if (server.drain(wire) != 0 || wire.empty()) return;
  (void)client.recv(wire.data(), wire.size());              // 这里同步跑 on_response_end
}
```

这一段是真的能跑通的：`drain()` 榨出来的是**一整块**待发字节，`recv()`
会把它吃干净并**同步**回调进你的 `callbacks`。

---

## 4. 传输层

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
uvcpp_h2_connection(uvcpp_tcp_client* client, bool server_side);  // src/http2/uvcpp_h2_connection.h:58
int start(const uvcpp_h2_session::callbacks& h2_cbs,
          const callbacks& conn_cbs);                             // src/http2/uvcpp_h2_connection.h:70
int send_response(int32_t stream_id, const uvcpp_http_response& resp,
                  bool omit_body = false);                        // src/http2/uvcpp_h2_connection.h:78
int send_status(int32_t stream_id, int status, const std::string& body);  // :82
int send_headers(int32_t stream_id, const uvcpp_http_response& resp);     // :87
int send_data(int32_t stream_id, const char* data, size_t len,
              bool end_stream, std::function<void(int)> done);        // :96
int flush();                                                              // :104
int begin_goaway();                                                       // :117
void shutdown();                                                          // :120
void close_now();                                                         // :123
```

它**不拥有** `uvcpp_tcp_client`（`src/http2/uvcpp_h2_connection.h:34`），
但**拥有自己的 `uvcpp_h2_session`** —— 会话是在构造函数里 `new` 出来的
（`src/http2/uvcpp_h2_connection.cpp:18`），由 `shared_ptr` 回收。
**会话可以脱离 connection 单独用**（[§3](#3-会话层的最小用法) 就是），
反过来不行。

`callbacks` 只有一个成员：

```cpp
// doc-snippet: fragment — 结构体定义摘录，不是完整翻译单元
struct callbacks {
  std::function<void(uvcpp_h2_connection&)> on_disconnect;   // src/http2/uvcpp_h2_connection.h:46
};
```

### 建它的四条硬要求

1. **socket 必须已经连上。** connection 在 `start()` 里自己装读回调
   （`src/http2/uvcpp_h2_connection.cpp:68` 用 `read_start_events`），
   而没连上的 client 上这个调用返回 `UV_ENOTCONN` —— **而且那个失败是静默的**，
   没人会再 arm 一次。库内正解是等到 connect 成功回调里才 `new`
   （`src/web/uvcpp_http_client.cpp:265`）。
2. **ALPN 必须已经协商成 `h2`。** connection 的构造函数**不校验**这一点
   （`src/http2/uvcpp_h2_connection.cpp:18` 只存指针），但头文件的参数说明要求它
   （`src/http2/uvcpp_h2_connection.h:54`）。谁建谁自己判：
   `client->is_tls() && client->tls_alpn_selected() == "h2"`
   （照 `src/web/uvcpp_http_server.cpp:165`）。
3. **`on_disconnect` 里必须销毁这个对象。** 头文件写得很直白：
   "回调返回后**不要**再碰本对象 —— 持有者应当在这里把它销毁"
   （`src/http2/uvcpp_h2_connection.h:46-51`）。
4. **`start()` 会把 `on_fatal` 换成自己的包装**（就地 `shutdown()`，
   `src/http2/uvcpp_h2_connection.cpp:49`）—— 你装的那个仍会被调用，但不是唯一的
   收尾动作。

TLS 与 ALPN 全是 `uvcpp_tcp_client` 内部的事，这一层只读一个字符串
（`src/net/uvcpp_tcp_client.h:189` / `:192` / `:204` / `:226`）。
`src/http2/` 里**没有一次 `#include` ssl 头**。

### 唯一的那个连接级回调

`on_disconnect` 在三种情况下触发：对端关了、读错了、我们自己关完了
（`src/http2/uvcpp_h2_connection.cpp:176`、`:332`）。它**排在框架自己的关闭回调之前**，
所以那是你最后一次能安全碰这条连接的机会。

---

## 5. 服务端怎么回一条响应

**能用 `uvcpp_h2_connection` 上的 `send_*` 就别用 `session().submit_*`。**
那四个 `send_*` 是"`submit_*` + `flush()`"的合并 —— 忘了 `flush()` 就是
"响应提交了但一个字节都没发"这种最难查的静默失败（`src/http2/uvcpp_h2_connection.h:74`）。

库内那份接好的实现（`src/web/uvcpp_http_server.cpp:1396` 起）的调用序列：

1. 分流点判 ALPN，是 h2 就走另一条路（`src/web/uvcpp_http_server.cpp:165`）；
2. `new uvcpp_h2_connection(client, /*server_side=*/true)`（`:1396`）；
3. 装 `uvcpp_h2_session::callbacks`（`:1401`）与 `uvcpp_h2_connection::callbacks`（`:1457`）；
4. 挂框架自己的关闭回调 —— **最终 `delete` 在那里**（`:1462`）；
5. `h2->start(h2c, cc)`（`:1464`）；失败就 `h2->close_now()`（`:1465`）；
6. 业务处理完之后 `h2->send_response(stream_id, resp, omit_body)`（`:1501` 起）。

一个能编的最小响应侧写法：

```cpp
#include <cstdint>
#include <string>

#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_session.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_response.h>

// 只演示"装回调 + 回一条响应"的形状；真接线还要照 §4 那四条要求做。
void doc_h2_server_wiring(uvcpp::uvcpp_tcp_client* tcp) {
  uvcpp::uvcpp_h2_connection* h2 =
      new uvcpp::uvcpp_h2_connection(tcp, /*server_side=*/true);

  uvcpp::uvcpp_h2_session::callbacks h2c;
  // 回调里一律**重新查表**，不缓存 stream 指针 —— 它过了 on_close 就失效了。
  h2c.on_request = [h2](uvcpp::uvcpp_h2_session&, uvcpp::uvcpp_h2_stream& st, bool) {
    h2->send_response(st.stream_id, uvcpp::uvcpp_http_response::ok("ok", 2));
  };

  uvcpp::uvcpp_h2_connection::callbacks cc;
  cc.on_disconnect = [](uvcpp::uvcpp_h2_connection& self) {
    delete &self;   // 契约要求：在这里销毁
  };

  if (h2->start(h2c, cc) != 0) h2->close_now();
}
```

### 流式响应

三段式：`send_headers`（只发头、不结束流）→ 若干次 `send_data(..., end_stream,
done)` → 最后一块 `end_stream = true`。三处要点：

- **提交过 `end_stream` 之后这条流不再接受新的 `submit_data`**，再发就是协议违例
  （`src/http2/uvcpp_h2_session.h:291`）；
- **每一块的 `done` 必须恰好跑一次**，流中途被 RST 或连接断了也会跑，参数是
  `UV_ECANCELED`（`src/http2/uvcpp_h2_connection.h:99-102`）；
- **拆传输的正确顺序**是 `take_cancelled_dones(out)` → 销毁对象 → 在**自己的上下文
  已经拆干净之后**逐个跑那些 `done`（`src/http2/uvcpp_h2_connection.h:125-137`）。
  它只取走、不跑，理由写在头里。

---

## 6. 客户端怎么发一条请求

**低层没有请求侧的合并入口** —— `uvcpp_h2_connection` 上的四个 `send_*` 全是响应侧的。
客户端提交完请求**必须自己调 `flush()`**。这一点在
`tests/functional/web_ssl_h2_server_func.cpp:457` 有一段专门的告诫，
库内正解在 `src/web/uvcpp_http_client.cpp:1212`：

```cpp
// doc-snippet: fragment — 从库内调用点摘的三句，前后文不在本页
const int32_t sid = conn->session().submit_request(req, body);
if (sid <= 0) return sid;   // 负值是错误码，且**什么都没发生**
conn->flush();              // ← 少了这一句，请求就停在队列里
```

`submit_request` 的三个同步失败码都是有保证的"什么都没发生"：
`UV_EINVAL`（字段非法）、`UV_EMSGSIZE`（头部块超限）、`UV_ENOTCONN`
（收到过对端 GOAWAY，或本端流号用尽）—— 没有流被建、没有字节被排队、
没有回调会被叫（`src/http2/uvcpp_h2_session.h:332-341`）。

```cpp
#include <cstdint>
#include <string>

#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_session.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_http_request.h>

void doc_h2_client_submit(uvcpp::uvcpp_h2_connection* conn) {
  uvcpp::uvcpp_http_request req = uvcpp::uvcpp_http_request::make_get("/index.html");

  const int32_t sid = conn->session().submit_request(req, std::string());
  if (sid <= 0) return;   // UV_EINVAL / UV_EMSGSIZE / UV_ENOTCONN 都是同步的"没发生"
  (void)sid;

  if (conn->flush() != 0) return;   // ← 请求侧的合并入口就是这两句
}
```

`uvcpp_http_client` 那条线是同一套顺序：`set_http2_enabled(true)`
（`src/web/uvcpp_http_client.cpp:1124`）→ `connect()` 里 `enable_tls` 加 ALPN 名单
（`:228` / `:244`，`h2` 在前、`http/1.1` 兜底）→ connect 成功回调里读
`tls_alpn_selected()`（`:263`）→ `start_h2()`（`:267` → `:1151`）→ 装回调（`:1153`）
→ `h2_->start(sc, cc)`（`:1187`）→ `send()` 分流到 `send_h2`（`:382`）。

**`set_http2_enabled` 默认是关的**（`src/web/uvcpp_http_client.h:385`、
`src/web/uvcpp_http_server.h:190`）—— 低层的两条线都要显式打开，
只有 `webapp/` 框架层是零配置自动协商。

---

## 7. 三个回调什么时候来

| 回调 | 服务端 | 客户端 | 触发点 |
|---|---|---|---|
| `on_request(session, stream, end_stream)` | ✅ | ✗ | 请求头收全（`src/http2/uvcpp_h2_session.cpp:481`） |
| `on_request_end(session, stream)` | ✅ | ✗ | 请求体收完（`:482`、`:505`） |
| `on_response(session, stream, end_stream)` | ✗ | ✅ | 响应头收全（`:484`） |
| `on_response_end(session, stream)` | ✗ | ✅ | 响应全收完（`:486`、`:512`） |
| `on_body(session, stream, data, len)` | ✅ 请求体 | ✅ 响应体 | 每块 DATA（`:531`） |
| `on_close(session, stream_id, error_code)` | ✅ | ✅ | 流结束（`:536`），**回调之后流引用即失效** |
| `on_fatal(session, nghttp2_error)` | ✅ | ✅ | 见 [§9](#9-错误处理) |

两条不看源码一定会写错的地方：

**一、无 body 的请求上 `on_request` 与 `on_request_end` 是背靠背的。**
`on_request_end` 的触发条件是**「这条流的 END_STREAM 到了」**，不是"有请求体才算"
（`src/http2/uvcpp_h2_session.cpp:482`）。HEADERS 自带 END_STREAM 的请求（也就是
绝大多数 GET）**也会触发**，而且是紧接着 `on_request` 同步来的 —— 库内自己就依赖
这一点：`src/web/uvcpp_http_server.cpp:1421-1422` 明说那里**不能** `erase` 流状态，
无 body 的请求这两下是背靠背的，擦掉之后那条请求就没人派发了。

**二、客户端交付响应只能放 `on_response_end`。** 带 body 的响应里 `on_response` 的
`end_stream` **恒为 false**，而 DATA 的 END_STREAM 不经过任何回调 ——
没有 `on_response_end` 就分不出"响应到头了"和"响应全收完了"
（`src/web/uvcpp_http_client.cpp:1165`、`src/http2/uvcpp_h2_session.h:141-147`）。

另外，**服务端侧的 `uvcpp_h2_stream::response` 初值是 `HTTP_STATUS_NONE` 而不是 `200`**
（`src/http2/uvcpp_h2_session.h:71`）。这不是疏漏：流在响应头到达之前被 RST
（`REFUSED_STREAM` 正是如此）时，一个编出来的 200 会一路交付到业务层。

---

## 8. 上限与旋钮

`init()` 有三个参数，**默认值就是全部旋钮**（`src/http2/uvcpp_h2_session.h:201`）：

| 旋钮 | 默认 | 含义 |
|---|---|---|
| `max_header_list_size` | `64u * 1024u` | 我们**愿意接收**的头部列表上限 |
| `max_concurrent_streams` | `H2_DEFAULT_MAX_CONCURRENT_STREAMS` = 100 | 我们宣告出去的最大并发流 |
| `initial_window_size` | `0` | **0 = 这条 SETTINGS 不发** |

**`max_header_list_size` 只是宣告出去的礼貌值** —— 收方向的强制由 `h2_header_budget`
自己做（`src/http2/uvcpp_h2_common.h:95`）。头文件把这一点写明了，别以为设了就防住了。

**`initial_window_size = 0` 不是"窗口为 0"**，是"这一条 SETTINGS 不 push 进去"
（`src/http2/uvcpp_h2_session.cpp:708`）。于是实际用的是 nghttp2 的初值 65535 ——
数值上与 `H2_DEFAULT_INITIAL_WINDOW_SIZE` 恰好相等，但**路径完全不同**，
而那个常量全仓**零引用**（`src/http2/uvcpp_h2_common.h:40` 是它唯一出现的地方）。

实际发出去的 SETTINGS（`src/http2/uvcpp_h2_session.cpp:703`）：
`ENABLE_PUSH = 0`（我们不收也绝不发 PUSH_PROMISE）、`MAX_CONCURRENT_STREAMS`、
`MAX_HEADER_LIST_SIZE`，`INITIAL_WINDOW_SIZE` 按上面的规则。

其余几个硬上限（都是写死的，没有旋钮）：

| 项 | 值 | 位置 |
|---|---|---|
| 单个待发头部块 | 64 KiB（`H2_MAX_SEND_HEADER_BLOCK`） | `src/http2/uvcpp_h2_common.h:53`、设置点 `src/http2/uvcpp_h2_session.cpp:694`、自查 `:917` |
| 单流 body | 64 MiB（`H2_DEFAULT_MAX_BODY_BYTES`）→ RST | `src/http2/uvcpp_h2_common.h:32`、`src/http2/uvcpp_h2_session.cpp:522` |
| `content-length` 荒谬值 | `> 1<<40` 判非法 | `src/http2/uvcpp_h2_session.cpp:386` |
| 控制帧令牌桶 | burst 64、补充 32 个/秒 | `src/http2/uvcpp_h2_session.cpp:55-56`、`control_frame_ok()` `:165` |
| `:scheme` 白名单 | 只接受 `https` | `src/http2/uvcpp_h2_session.cpp:311` |

**没有闲置超时**（`src/http2/` 里 grep `idle|timeout|keepalive` 零命中）。
`src/web/uvcpp_http_server.h:877-880` 提到的那个 idle sweep 属于 **webapp 层**
（默认 60 s），既不是 h1 服务器自带的，也不覆盖 h2 连接。

对端的观测口：`peer_max_concurrent_streams()`（`src/http2/uvcpp_h2_session.h:359`）、
`peer_goaway_received()` / `peer_goaway_error_code()` / `peer_goaway_last_stream_id()`
（`:372` / `:374` / `:376`）、`goaway_code()`（`:394`）、`stream_count()`。

---

## 9. 错误处理

**同步返回 `int`（0 成功）加回调带码，不用异常。** 唯一的例外是 `submit_request`：
它返回**新流号**（正数）或负的错误码（`src/http2/uvcpp_h2_session.h:343`）。

### nghttp2 的错误码一律收紧成致命，不逐码判

```cpp
// doc-snippet: fragment — 引的是库内实现片段，不是本页可复制的用法
if (rv < 0) {                        // src/http2/uvcpp_h2_session.cpp:732
  impl_->last_error = static_cast<int>(rv);
  if (!impl_->fatal_reported) { impl_->fatal_reported = true;
    if (impl_->cbs.on_fatal) impl_->cbs.on_fatal(*this, (int)rv); }
  return static_cast<int>(rv);
}
```

理由写在 `src/http2/uvcpp_h2_session.h:210-215`：`nghttp2_session_mem_recv2` 有若干返回值
只表示"这条流有问题"，但 `-905`（CONTINUATION 过多）这种是**连接级**的，而它偏偏不在
返回值上区分 —— 既不发 RST 也不发 GOAWAY。把"看起来像流级"的错误当流级处理，
就会留下一条状态已经错乱的连接继续用。所以**负值 ⇒ 会话作废**。

**致命只通知一次**（`fatal_reported`），而且致命之后**不再进 nghttp2**：
`recv()` 直接回上次那个错误（`src/http2/uvcpp_h2_session.cpp:721`）。

`on_fatal` 有三个触发点（`src/http2/uvcpp_h2_session.cpp:738`、`:748`、`:761`）：

- `nghttp2_session_mem_recv2` 返回负值；
- 输入**没吃完**（自造一个 `-1`）；
- 控制帧超令牌桶 —— 这条是**在 `mem_recv` 之外**收尾的，返回 `UV_ECANCELED`，
  发出去的 GOAWAY 码取 `goaway_code()`（`ENHANCE_YOUR_CALM`）。

### 哪些是致命、哪些只是流级

| 事件 | 归类 |
|---|---|
| `mem_recv` 返回负值 / 输入没吃完 / 控制帧洪泛 | **致命**：`on_fatal` 加会话作废 |
| 头部块超预算 | **流级**：RST(`ENHANCE_YOUR_CALM`)，连接照用（`src/http2/uvcpp_h2_session.cpp:416`） |
| 单流 body 超 64 MiB | **流级**：RST(`ENHANCE_YOUR_CALM`)（`:522`） |
| 伪头顺序 / 白名单 / 走私 | **流级**：RST(`PROTOCOL_ERROR`) |
| 发方向头部块超上限 | **同步返回** `UV_EMSGSIZE`，且流状态没被改过 |

可不可重试的判定**不在这一层**，在 `web/` 侧：`H2_ERR_REFUSED_STREAM` 才算
`retryable`，`NO_ERROR` 与 `CANCEL` 都算正常收尾
（`src/web/uvcpp_http_client.cpp:1298`）。

---

## 10. 典型坑

**客户端提交请求后必须自己 `flush()`。** 见 [§6](#6-客户端怎么发一条请求)。
连接层没有请求侧的合并入口，那句 `flush()` 少了就是**静默的什么都没发**。

**`uvcpp_h2_connection` 必须在 socket 连上之后才建。** 它自己装读回调，
而没连上的 client 上 `read_start_events` 返回 `UV_ENOTCONN` —— 那个失败没人会再 arm
（`src/web/uvcpp_http_client.cpp:265`、`tests/functional/web_ssl_h2_server_func.cpp:407`）。

**ALPN 要自己确认。** 构造函数不校验 `tls_alpn_selected() == "h2"`，但头文件的参数
说明要求它已经协商好（`src/http2/uvcpp_h2_connection.h:54`）。谁建谁判。

**`on_disconnect` 里必须销毁 connection。** 见 [§4](#4-传输层)。

**回调栈里不许 `drain()`。** 那等于在 nghttp2 自己的栈里重入 `mem_send`，
实测会在"回调里把 RST_STREAM 冲出去"那条路上读一块 nghttp2 刚释放的
`nghttp2_stream`（`src/http2/uvcpp_h2_session.h:229`）。用 `in_nghttp2()` 判自己是不是
在回调里；推迟到 `recv()` 返回之后再冲即可，连接层本来就在返回之后冲一次。
**注意 `flush()` 在回调里调用是安全的** —— 它自己会推迟，排队的字节一个都不会丢
（`src/http2/uvcpp_h2_connection.h:104-108`）。

**`recv()` 的返回值不能在连接已死之后用来判据。** 回调可能把宿主整条析构掉，
那时 `this` 已经没了。所以调用方进来之前要先本地持一份 `shared_ptr`
（`src/http2/uvcpp_h2_session.h:108-112` 的 `@code` 就是这段）。

**收到对端 GOAWAY 之后不要拆这条连接，要另起一条。** GOAWAY 关的是"新流"，
不是"连接" —— 已有的流照跑完，新流会被 `submit_request` 拦成 `UV_ENOTCONN`
（`src/http2/uvcpp_h2_session.h:361`）。

**每一块流式 body 的 `done` 必须恰好跑一次。** 传输层要在断开时整体作废
（`cancel_pending_out()` + `take_cancelled_dones()`），少了这一步，
框架的流式响应会等一个永远不来的回调（`src/http2/uvcpp_h2_session.h:326`）。

**`run_completed()` 只在没有写在飞的时候调**，而且 `done` 跑起来之后**一个字都不能
写成员** —— `done` 跑的是用户代码，它可能再补一笔写
（`src/http2/uvcpp_h2_connection.h:156`）。

**`content-length` 校验只在服务端请求方向做。** 客户端侧的响应头只挡连接专属头，
不校验 `content-length`（`src/http2/uvcpp_h2_session.cpp:434`）。

---

## 11. 没做的（如实列出）

这一节只列**用法上会撞到**的。实现进度、写死的折衷与完整缺口清单在
[HTTP/2 支持现状](./http2-status.md) 的 §2 / §3 / §4，不在这里重复。

- **不自动接线。** 建会话、判 ALPN、装读回调、销毁，全要调用方做（[§4](#4-传输层)）。
- **没有闲置超时、没有心跳**（[§8](#8-上限与旋钮)）。
- **没有请求侧的合并入口**：`uvcpp_h2_connection` 的四个 `send_*` 全是响应侧的
  （[§6](#6-客户端怎么发一条请求)）。
- **流控没有策略**，`H2_DEFAULT_INITIAL_WINDOW_SIZE` 全仓零引用 —— 详见
  [现状 §2](./http2-status.md#2-做了但有折衷写死了)。
- **不做 h2c**（明文升级）：没有 ALPN 就没有 h2。
- **服务端不推 PUSH_PROMISE**，`ENABLE_PUSH` 恒为 0，而且也没有发 PUSH 的 API。
- **`want_read`/`want_write` 不参与致命判定**：`on_fatal` 只有两类触发者，这两个公开
  成员本层一处都没读（`src/http2/uvcpp_h2_session.h:168-180`）—— 详见
  [现状 §2](./http2-status.md#2-做了但有折衷写死了)。

---

相关文档：[HTTP/2 支持现状](./http2-status.md)、[web HTTP 指南](./web-http-guide.md)、
[net 网络层指南](./net-guide.md)、[webapp 应用框架开发者指南](./webapp-guide.md)、
[项目 README](../README.zh.md)。

本页用到的头文件：`<http2/uvcpp_h2_session.h>`、`<http2/uvcpp_h2_connection.h>`、
`<http2/uvcpp_h2_common.h>`、`<net/uvcpp_tcp_client.h>`、
`<web/uvcpp_http_request.h>`、`<web/uvcpp_http_response.h>`。
