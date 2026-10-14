# gRPC 指南

`src/grpc/` 只有两半，**边界切在"要不要 web 层"上**：

| 半 | 文件 | 里面是什么 | 需要 `UVCPP_BUILD_WEB` |
|---|---|---|---|
| 编解码 | `uvcpp_grpc.h` / `.cpp` | 5 字节长度前缀帧 + 一小撮 protobuf wire format（varint 字段）。**只跟字节说话**，不认识连接、不认识响应 | 不要 |
| 服务端 | `uvcpp_grpc_server.h` / `.cpp` | 把上面那半接到 `uvcpp_http_server` 上：三段应答的顺序、尾随头、百分号编码 | **要** |

这个分法不是洁癖，是**依赖事实**：编解码那半的每一个参数都是 `std::string` / `uint32_t`
（`src/grpc/uvcpp_grpc.h:100-194`），而服务端那半的每个参数都是 web 层的类型
（`src/grpc/uvcpp_grpc_server.h:116-132`）。所以关掉 web 模块时前者照编照装、后者
**从构建与安装里都摘掉**（`CMakeLists.txt:1827-1829`、`CMakeLists.txt:2729-2734`）——
装在 `include/grpc/` 下，而不是 `include/web/` 下。

- 打开方式：**没有开关**。编解码那半任何配置下都在；服务端那半跟着 `UVCPP_BUILD_WEB`
  走（`CMakeLists.txt:1827-1827`）。加一个 `UVCPP_ENABLE_GRPC` 只会多一个能与
  `UVCPP_BUILD_WEB` 打架的旋钮，而两半的依赖是死的、不需要调用方表态。
- 包含方式：`<grpc/uvcpp_grpc.h>`、`<grpc/uvcpp_grpc_server.h>`。
- 依赖：帧与 varint 那半**只依赖 `<cstdint>` 与 `libuv` 的错误码**（
  `src/grpc/uvcpp_grpc.h:41`）；服务端那半额外依赖 `web/uvcpp_http_common.h`
  与 `uvcpp_http_server::send_trailers()`（`src/web/uvcpp_http_server.h:801-801`）。

> 本页讲**怎么用**，以及**为什么这么切**。与本页配对的两份用例是
> `tests/functional/grpc_codec_func.cpp`（编解码，任何配置下都跑）与
> `tests/functional/web_h2_grpc_func.cpp`（真 socket + 真路由的往返，web/nghttp2
> 关掉时**整个文件不参与构建** —— 于是"没测"表现为"用例不存在"，而不是一个假装
> 通过的 SKIP）。
>
> 签名、默认值、行为都对着当前源码核过，非显然的结论后面都跟了 `文件:行号`。

---

## 目录

1. [gRPC 在 HTTP/2 上长什么样](#1-grpc-在-http2-上长什么样)
2. [编解码那半：帧](#2-编解码那半帧)
3. [编解码那半：varint 与字段](#3-编解码那半varint-与字段)
4. [服务端那半：三段应答](#4-服务端那半三段应答)
5. [服务端那半：收一条请求](#5-服务端那半收一条请求)
6. [错误码对照](#6-错误码对照)
7. [三个典型坑](#7-三个典型坑)
8. [为什么放进库里，而不是在调用方手写](#8-为什么放进库里而不是在调用方手写)
9. [没做的（如实列出）](#9-没做的如实列出)

---

## 1. gRPC 在 HTTP/2 上长什么样

一条 **unary**（一问一答）的 gRPC 应答在线上是**三段**，顺序是硬约束：

```
HEADERS   :status: 200  +  content-type: application/grpc
DATA      帧（5 字节前缀 + 消息）
HEADERS   尾随头 grpc-status: 0（自带 END_STREAM）
```

第三段就是 gRPC 与普通 HTTP 的分界，也是这一层最容易误解的一处：

- **HTTP 层的状态码永远是 200。** 一条"查无此项"在 HTTP 层是 `200 OK`；真正的成败在
  `grpc-status` 上。所以**只看 HTTP 状态码的客户端永远看不出失败** —— 它会把一条
  Trailers-Only 的失败应答读成"200，body 是空的"。
- **`grpc-status` 按规范必须走尾随头**，不走首部（gRPC 的 `PROTOCOL-HTTP2.md` 定在
  那里；RFC 9113 §8.1 提供了"首部之后还能有一个头块"这条机制）。本库为此在 1.6.9 补了
  `uvcpp_http_server::send_trailers()`（`src/web/uvcpp_http_server.h:801-801`）。
- 失败应答可以**只有第三段**（没有 DATA）—— gRPC 里叫 **Trailers-Only**。本模块的
  两条失败路径都是这个形状（`src/grpc/uvcpp_grpc_server.cpp:115-131`）。

**gRPC 只存在于能承载多条流的协议上**，也就是 h2 / h3。h1 上的 gRPC 要 chunked 编码
兜尾随头，本库没做 —— 在 h1 上调用本模块会拿到 `UV_EINVAL`（判据是
`resp.stream_id == 0`，`src/grpc/uvcpp_grpc_server.cpp:73-81`）。

---

## 2. 编解码那半：帧

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
constexpr size_t GRPC_FRAME_HEADER_SIZE = 5u;                      // src/grpc/uvcpp_grpc.h:51
constexpr size_t GRPC_FRAME_MAX_MESSAGE_BYTES = 4u * 1024u * 1024u; // src/grpc/uvcpp_grpc.h:54

int grpc_frame_encode(const std::string& message, std::string& out,
                      size_t max_message_bytes = GRPC_FRAME_MAX_MESSAGE_BYTES);
                                                                   // src/grpc/uvcpp_grpc.h:100
int grpc_frame_decode(const char* data, size_t len, std::string& message, size_t& consumed,
                      std::string* err = nullptr,
                      size_t max_message_bytes = GRPC_FRAME_MAX_MESSAGE_BYTES);
                                                                   // src/grpc/uvcpp_grpc.h:119
int grpc_frame_decode_exact(const char* data, size_t len, std::string& message,
                            std::string* err = nullptr,
                            size_t max_message_bytes = GRPC_FRAME_MAX_MESSAGE_BYTES);
                                                                   // src/grpc/uvcpp_grpc.h:133
```

前缀是 **1 字节压缩标志 + 4 字节大端长度**（`src/grpc/uvcpp_grpc.cpp:34` 手工按字节
拼，不用 `ntohl` —— 这个格式的字节序是协议规定的，不是主机序）。载荷是消息本身，
**不带任何长度之外的包装**。

拿 HttpArena 那条 grpc-sum 请求当例子，`a=1, b=2` 的请求体逐字节是：

```
00  00 00 00 04  08 01 10 02
│   └────┬────┘  └────┬────┘
│        │            载荷 4 字节：字段1=1、字段2=2
│        └─ 大端长度 = 4
└─ 压缩标志 0（没压）
```

五行全是手算的，`tests/functional/web_h2_grpc_func.cpp` 里钉的就是这 9 个字节。

**`encode` 是追加语义**（`out += `），不是覆盖：流式那条路上调用方要的是"往同一个
缓冲区里接着放一帧"，覆盖语义会让每帧都白拷一次。`UV_EMSGSIZE` 时它**一个字节都不动**
（`src/grpc/uvcpp_grpc.cpp:205-221`）。

**`decode` 与 `decode_exact` 的差别只在结尾**：

- `decode` 是流式的 —— 成功时写回 `consumed`（这一帧占用的总字节数），调用方据此
  推进游标接着解下一帧；字节不够时回 `UV_ENODATA`，**这不是格式错误，是"再读一段"**，
  流式解码器就是靠这个码决定要不要等。
- `decode_exact` 要求**整段刚好是一帧**，多一个字节、少一个字节都算错
  （`src/grpc/uvcpp_grpc.cpp:259-273`）。unary 按定义只有一帧，所以服务端这一侧用
  "恰好"比"至少"更严：多出来的字节只可能是对端把两条请求拼在了一起，或者一条**流式**
  请求打到了 unary 方法上 —— 两种都不该被当成正常请求照跑（照跑的后果是第二条消息被
  静默丢弃，而调用方认为它成功了）。

---

## 3. 编解码那半：varint 与字段

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
int grpc_varint_encode(std::string& out, uint64_t value);          // src/grpc/uvcpp_grpc.h:150
int grpc_varint_decode(const char* data, size_t len, uint64_t& value, size_t& consumed,
                       std::string* err = nullptr);                // src/grpc/uvcpp_grpc.h:164
int grpc_field_put_int32(std::string& out, uint32_t field, int32_t value);
                                                                   // src/grpc/uvcpp_grpc.h:178
int grpc_field_get_int32(const char* data, size_t len, uint32_t field, int32_t& value,
                         std::string* err = nullptr);              // src/grpc/uvcpp_grpc.h:193
```

varint 是 protobuf 的 base-128：每字节 7 位，最高位是"后面还有"的续位，**小端组**
（先出的那组是低位）。合法 varint 最长 **10 字节** —— 64 位按 7 位一组要 10 组，第 10 个
字节只带最高那一位，所以它要么 `0x00`、要么 `0x01`，**而且不能再挂续位**
（`src/grpc/uvcpp_grpc.cpp:74-96`）。超了是格式错（`UV_EINVAL`），不是"读不完"。

字段 = `(字段号 << 3) | wire type` 的 tag，后跟值。本模块**只编解 wire type 0
（varint）字段**，其余 wire type 一律**跳过**（0 = varint，1 = 8 字节，2 = 长度前缀 + 载荷，
5 = 4 字节；3/4 是已废弃的 group、6/7 保留，都判非法，
`src/grpc/uvcpp_grpc.cpp:192`）。字段号范围是 `1..(2^29-1)`
（`src/grpc/uvcpp_grpc.cpp:23`）—— **字段号 0 不存在**。

两个 `int32` 的例子：

```
int32 1       → 08 01
int32 127     → 08 7f
int32 128     → 08 80 01
int32 -1      → 08 ff ff ff ff ff ff ff ff ff 01
int32 100000  → 08 a0 8d 06
```

**最后那一行是这一层的重点。** protobuf 把负的 `int32` **按 64 位 varint 发**（符号
扩展），所以 `-1` 是 10 字节的 `ff×9 01`，不是 1 字节的 `01`。少这一步不是"编码得难看
一点"，而是**对端会解出一个正数**、且这条路径上没有任何东西会报错
（`src/grpc/uvcpp_grpc.cpp:105-111`）。

**同一个字段号出现多次时取最后那个**（`src/grpc/uvcpp_grpc.cpp:113-199`）—— protobuf
对奇异标量字段就是这么定的（后者覆盖前者）。走"取第一个"会在对端重发字段时解出一个
过时的值。

---

## 4. 服务端那半：三段应答

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
const char* grpc_content_type();                                   // src/grpc/uvcpp_grpc_server.h:64
http_headers grpc_ok_trailers();                                   // src/grpc/uvcpp_grpc_server.h:74
http_headers grpc_status_trailers(grpc_status status, const std::string& message);
                                                                   // src/grpc/uvcpp_grpc_server.h:82
std::string grpc_percent_encode(const std::string& s);             // src/grpc/uvcpp_grpc_server.h:92

int grpc_send_unary(uvcpp_http_server& server, uvcpp_tcp_client* client,
                    uvcpp_http_response& resp, const std::string& message);
                                                                   // src/grpc/uvcpp_grpc_server.h:116
int grpc_send_status(uvcpp_http_server& server, uvcpp_tcp_client* client,
                     uvcpp_http_response& resp, grpc_status status,
                     const std::string& message);                  // src/grpc/uvcpp_grpc_server.h:130
```

一条成功应答就这样出去 —— 顺序**全部由这一个函数包办**：

```cpp
// 完整翻译单元：一条 unary 的 grpc-sum 路由（真往返的那条用例就是这么写的）
#include <grpc/uvcpp_grpc.h>
#include <grpc/uvcpp_grpc_server.h>
#include <web/uvcpp_http_server.h>

using namespace uvcpp;

// 处理函数拿到的那份 resp 里，只有 stream_id 会被本模块读。其余一概不看 ——
// 状态码与 content-type 由 grpc_send_unary() 定（gRPC 就是这么定的），
// body 由 message 编码得来。所以在这里写别的都不会生效，这是有意的：
// 一条 gRPC 应答的形状不该有第二种。
void handle_grpc_sum(uvcpp_http_server&                server,
                     uvcpp_tcp_client*                 client,
                     const uvcpp_http_request&         req,
                     uvcpp_http_response&              resp) {
  std::string   message;
  grpc_status   fail_status = grpc_status::INVALID_ARGUMENT;
  std::string   err;
  if (grpc_parse_unary_body(req.body, message, fail_status, err) != 0) {
    // 失败：只有尾随头的 Trailers-Only。`grpc-message` 里放的就是那句人话。
    grpc_send_status(server, client, resp, fail_status, err);
    return;
  }

  int32_t a = 0;
  int32_t b = 0;
  // 缺省值是调用方的事：字段没给就回 UV_ENOENT，那是"用默认值"而不是"报错"。
  grpc_field_get_int32(message.data(), message.size(), 1, a, nullptr);
  grpc_field_get_int32(message.data(), message.size(), 2, b, nullptr);

  std::string reply;
  grpc_field_put_int32(reply, 1, a + b);

  // 0 = 已入队，尾随头会在字节写完时自动发出。非 0 = 这条流已经没了，
  // 尾随头**不会**发（write_stream() 非 0 时不调 done，不能指望它兜底）。
  if (grpc_send_unary(server, client, resp, reply) != 0) {
    // 这里已经没有"回一个错"的机会了 —— 流没了。记日志即可。
  }
}
```

为什么顺序不留给调用方拼：第二段 `write_stream()` 的 `done` 回调是"**那批字节真的写完了**
的唯一信号"，尾随头必须在它里面发 —— 早一步就是"END_STREAM 在前、字节在后"的协议违例，
对端把这条流判成残缺。把这套顺序留给调用方，等于让每个写 gRPC 路由的人各踩一次同一个坑，
而**踩错时没有任何本地症状**（对端只是解包失败或者干脆超时）。

`grpc_send_unary()` 因此只动 `resp` 的两个字段：它置 `deferred = true`（这条应答由处理
函数自己发，框架别再回一条），然后读只读的 `stream_id`
（`src/grpc/uvcpp_grpc_server.cpp:73-113`）。

**Tailers-Only 为什么可以两句背靠背**：失败那条走
`begin_stream()` → `send_trailers()`，中间**没有任何数据在飞**，所以
`send_trailers()` 那条"body 一个字节都不欠"的前置条件当场成立 —— 它的判据只看 **DATA
队列空不空**（`chunks.empty()`），与发过几段头无关；而 `begin_stream()` 里的
`submit_headers()` 是**同步**把会话置成 `HEADERS_SENT` 的，所以紧接着的
`send_trailers()` 不会撞上 `UV_EALREADY`（`src/grpc/uvcpp_grpc_server.cpp:115-131`）。

### web 框架那一侧：声明，而不是发送

`uvcpp_web_app` 的路由签名里既没有 `uvcpp_http_server&` 也没有 `uvcpp_tcp_client*`
—— 它拿到的是 `uvcpp_web_request&` / `uvcpp_web_response&`，**发送者是框架**。所以框架
那一侧是另外两个入口（它们在 `UVCPP_BUILD_WEBAPP=ON` 才在库里，webapp 是可选模块且
**默认关**）：

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
int grpc_web_send_unary(uvcpp_web_response& resp, const std::string& message);
int grpc_web_send_status(uvcpp_web_response& resp, grpc_status status,
                         const std::string& message);
int grpc_parse_unary_body(const std::string& body, std::string& message,
                          grpc_status& fail_status, std::string& err);
```

两个发送版本的差别只有一个，而且是被框架结构定死的：**谁负责把三段的顺序排对**。

- 底层版面对的是裸 `uvcpp_http_server`，处理函数之后没有人替它发送，所以它自己
  `begin_stream` → `write_stream` →（在 `done` 里）`send_trailers`。它的 0 是"**已入队**"。
- web 版面对的是 `uvcpp_web_response`，它的发送者是 `uvcpp_web_app`：框架在处理函数
  返回后统一发。所以它只**声明**这条应答长什么样（`status` / `content-type` / body /
  `set_trailers()`），三段顺序由 `uvcpp_web_app::send_response()` 包办 —— 那是整个框架
  里唯一同时看得见 `write_stream()` 的 `done` 与 `send_trailers()` 的地方。
  它的 0 是"**已声明**"，字节还没上线。

```cpp
// 完整翻译单元：同一件事，写在一个 uvcpp_web_app 路由里
#include <grpc/uvcpp_grpc.h>
#include <grpc/uvcpp_grpc_server.h>
#include <webapp/uvcpp_web_app.h>

using namespace uvcpp;

// 处理函数只声明形状；三段顺序由框架排，尾随头由框架在 DATA 写完时发。
void handle_grpc_sum(uvcpp_web_request&  req,
                     uvcpp_web_response& resp,
                     uvcpp_web_next      next) {
  std::string message;
  grpc_status fail_status = grpc_status::INVALID_ARGUMENT;
  std::string err;
  if (grpc_parse_unary_body(req.body_str(), message, fail_status, err) != 0) {
    // 失败：只有尾随头的 Trailers-Only（框架看到空 body + 有尾随头，就只发两段）。
    grpc_web_send_status(resp, fail_status, err);
    resp.end();
    return;
  }

  int32_t a = 0;
  int32_t b = 0;
  grpc_field_get_int32(message.data(), message.size(), 1, a, nullptr);
  grpc_field_get_int32(message.data(), message.size(), 2, b, nullptr);

  std::string reply;
  grpc_field_put_int32(reply, 1, a + b);
  grpc_web_send_unary(resp, reply);
  resp.end();

  (void)next;  // 不调 next() = 链停在这里，由框架收尾
}
```

**`grpc_parse_unary_body()` 为什么有 `std::string` 那个重载**：`uvcpp_web_request`
暴露的是 `body_str()`（`std::string`），而底层那版收 `uvcpp_buf` —— 这两样**不是
"同一份字节的两个入口"**：`uvcpp_web_request::take_from()` 把 body **搬**进了自己的
`body_`，源请求的 `src_.body` 那之后就空了（`src/webapp/uvcpp_web_request.h:120` 明写
"调用之后不得再使用 `src.body`"）。所以 `req.raw().body` 既不该被碰、也拿不到东西。
让每个写 gRPC 路由的人自己把 `std::string` 包回 `uvcpp_buf` 只为了喂这一句，是把库内
的类型选择漏到了使用者面前。两个重载**同一个实现**，语义逐字相同。

**头表：摘三个，留其余。** `grpc_web_send_unary()` / `grpc_web_send_status()` 会把
`content-length` / `transfer-encoding` / `content-encoding` 摘掉，**其余的头原地保留**。
两半的理由不同：

- 摘的那三个是**线上错帧**的来源，不是"头表里多个字段"。`uvcpp_web_response::sync_meta()`
  的规则是"**已存在** `Content-Length` 或 `Transfer-Encoding: chunked` 时不动它"
  （那条规则对普通响应是对的：使用者在做分块/流式时不能被覆盖），但对这两个函数是致命的
  —— 处理函数先设过一个 `content-length: 999` 再调本函数，这条应答就会带着"999 字节"的
  声明上线，而实际只发得出去 7 个：**对端会一直等剩下 992 个字节**。
  `content-encoding` 同理，gRPC 自己的压缩走 `grpc-encoding`。
- 留的那些就是 gRPC 的 **initial metadata**（`:status` 之后、DATA 之前那一段头块）。
  服务端给客户端捎带应答元数据是规范内的用法，而这一段头块正是由 `set_header()` 攒出来的。
  底层版**做不到**这件事（它另起一份干净的 `uvcpp_http_response head`），web 版做得到
  —— 这是两个版本**有意**留下来的唯一一处行为差异。

**h1 与流式上会被丢弃，各留一句 WARN。** `set_trailers()` 的前提是"h2 且非流式"：

- **h1**（`stream_id == 0`）：尾随头按规范要 chunked 编码兜，本库没做那条路，所以字段被
  丢弃、**体照发**（对端看到一条语法完好的 200 + 那段帧，只是永远等不到 `grpc-status`）。
- **流式**（`begin_chunked()` 之后）：那条路的收尾是 `end_stream()`，没有第二段头块的
  位置，同样丢弃。

两条都只记一句 WARN 而不改行为 —— "以为发了、其实没发"是尾随头最容易发生的失败，而它
对本端完全静默（对端只是拿不到状态码，多半表现为超时）。

### 尾随头的内容

- `grpc_ok_trailers()` 成功时**只有** `grpc-status: 0`。不带空的 `grpc-message` ——
  那个字段按规范是"对失败的说明"，成功时它没有内容可说，而一个空值字段是"声明了一件
  没发生的事"（`src/grpc/uvcpp_grpc_server.cpp:48-55`）。
- `grpc_status_trailers()` 带 `grpc-status` + `grpc-message`，后者已**百分号编码**
  （`src/grpc/uvcpp_grpc_server.cpp:57-67`）。规范只允许 `0x20..0x7E` 里**除 `%` 外的**
  字符原样出现，其余一律 `%XX`（**大写**十六进制，`src/grpc/uvcpp_grpc_server.cpp:25`）。
  所以一句中文的失败说明到了对端手里是一串 `%E4%B8%AD...`；空串仍然是空串。
- 本函数**不校验** `status` 的取值范围：对端认不出的码按 `UNKNOWN` 处置，这是规范说的。

---

## 5. 服务端那半：收一条请求

```cpp
// doc-snippet: fragment — 对着头文件抄的接口清单，不是完整翻译单元
int grpc_parse_unary_body(const uvcpp_buf& body, std::string& message,
                          grpc_status& fail_status, std::string& err);
                                                                   // src/grpc/uvcpp_grpc_server.h:229

// web 框架那一侧（UVCPP_BUILD_WEBAPP=ON）：入参是 std::string，
// 因为 uvcpp_web_request 暴露的就是 body_str()。实现与上面那条完全相同。
int grpc_parse_unary_body(const std::string& body, std::string& message,
                          grpc_status& fail_status, std::string& err);
```

两个出参都是必填的，因为它们就是这个函数存在的理由：失败时要回的那条 Trailers-Only
需要**同时**拿到一句人话和一个 gRPC 状态码，分两次调用去凑只会让每条路由各写一遍那张
对照表（`src/grpc/uvcpp_grpc_server.cpp:207-224`）。

出参语义：成功时 `message` 是解出来的消息字节；失败时它被清空、`fail_status` 是
该用的状态码、`err` 是那句该放进 `grpc-message` 的话。返回值非 0 时**就是编解码层那个
错误码**（不另行翻译），方便调用方自己判 `UV_EMSGSIZE` 这类具体情形。

---

## 6. 错误码对照

编解码层的错误码 → gRPC 状态码（`src/grpc/uvcpp_grpc_server.cpp:207-224` 那张表）：

| 编解码层 | gRPC | 为什么 |
|---|---|---|
| 0 | —— （成功） | |
| `UV_EMSGSIZE` | `RESOURCE_EXHAUSTED` (8) | 对端说"这条消息很大"，是**容量**问题，与"字节坏了"的处置完全不同 |
| `UV_EINVAL` | `INVALID_ARGUMENT` (3) | 前缀坏、压缩标志非 0、帧后还有字节、varint 越界、字段号 0、wire type 认不出 |
| `UV_ENODATA` | `INVALID_ARGUMENT` (3) | 截断 —— **在流式解码里这只是"再读一段"，但到了这里已经收完整个请求体，所以它是错的** |
| 空请求体 | `INVALID_ARGUMENT` (3) | 至少该有一个 5 字节的帧前缀 |

最后那一行值得单说：**空请求体必须回失败**。把它答成"两个字段都没给，和为 0"是一次
成功应答 —— 那就把一条非法请求答成了成功。这条用例在
`tests/functional/web_h2_grpc_func.cpp` 第 3 轮里单列了一条。

## 7. 三个典型坑

**① 长度前缀是 4 字节，不是 3。** 5 个字节的前缀写起来少一个 `\x00` 太自然了
（`00 00 00 04` 看着就像"标志 + 长度"）。后果很隐蔽：长度从 4 变成 1032，解码器报
`UV_ENODATA`（"载荷还没收全"），落到同一个 `INVALID_ARGUMENT` 上 —— **判据照样绿，
而一个字节都没钉住**。写这种用例时，每个手写的 fixture 都该先过一道"它是不是我以为的
那份字节"的自检（`tests/functional/web_h2_grpc_func.cpp:350-373` 那个 `fixture_is_sum_frame`
就是干这个的，而它的第一版正是栽在这里）。

**② 负的 `int32` 要符号扩展到 64 位。** 见 §3 —— `-1` 是 10 字节，不是 1 字节。
只做"编了再解"的往返断言**抓不住这个错**：解码器把值截回 32 位，一个按 `uint32_t`
编的实现也能往返。真正的锚是**逐字节**的期望值。

**③ "只断言 END_STREAM"是假绿。** `send_trailers()` 与
`end_stream(client, sid, false)` 都在末段带 `END_STREAM` —— 把前者换成后者，
"整条响应收完"那条判据**照样绿**。真正的判据是"`on_response_end` 那一刻的头列表里有
`grpc-status`"（web 层第 ⑧ 轮的实测，见 `CHANGELOG.md` 里 1.6.9 那条）。

---

## 8. 为什么放进库里，而不是在调用方手写

帧格式与 varint 是**协议事实**，不是应用代码：帧定在 gRPC 的 `PROTOCOL-HTTP2.md` 里，
varint 定在 protobuf 的编码规范里，两边都不随调用方变化。放进库里的直接好处是这条路
**有头注释、有用例、有门禁**；散在路由里则是每个用它的人各写一遍、各错一遍 ——
上面那三个坑就是"各错一遍"的清单。

这一条也有对外的分量。把 gRPC 做成一等模块之后，用它的人满足的是"**用框架的文档化
高层 API**"这条要求：帧、varint、尾随头的顺序都由库提供，调用方写的是路由逻辑。
反过来，把编解码抄进应用里（哪怕抄得一样快）就是"绕过框架手搓"。

**为什么不上 `libprotobuf`**：本库的依赖就是 libuv 加可选的那几个协议库，为两个
`int32` 拉进一整套描述符运行时不成比例。需要完整 protobuf 的场合请照常生成代码，
把编码好的字节交给 `grpc_frame_encode()` —— 这一层只认"一段字节"，与它怎么来的无关，
两件事的边界正好切在这里（`src/grpc/uvcpp_grpc.h:16-25`）。

---

## 9. 没做的（如实列出）

- **编解码只做 varint 字段 + 跳过别的 wire type。** 不做嵌套消息、不做 `map`、
  不做 packed repeated、不做 group（wire type 3/4 直接判非法）、不做反射与描述符。
  要这些的场合走 §8 最后那条（自带生成代码 + `grpc_frame_encode()`）。
- **不做消息压缩**（`grpc-encoding` 那条路）。帧前缀里的压缩标志恒写 0，读到非 0 回
  `UV_EINVAL`（`src/grpc/uvcpp_grpc.cpp:240`）。标志位本身留在那里，将来要加看得见往哪加。
- **不做流式 RPC。** 本模块只有 unary：`grpc_frame_decode_exact()` 的存在就是为了让
  "一条流式请求打到 unary 方法上"这件事**被拒**而不是被当成两条 unary。
- **不做 h1 上的 gRPC**（要 chunked 兜尾随头），也不做 WebSocket 上的。
  web 框架那一侧撞上 h1 时的行为是"**尾随头丢弃、体照发 + 一句 WARN**"，
  不是静默成功也不是报错 —— 见 §4 那条子节。
- **不做客户端 stub。** 这一层给的是"发一条 gRPC 应答"和"解一条 gRPC 请求"，
  客户端那一侧请照 §1 的三段形状自己收（`uvcpp_h2_session` 的收尾随头那条路本来就在，
  尾随头块 category 是 `HCAT_HEADERS`、字段折进同一个 `response.headers`）。
- **不做 `grpc-timeout` / 取消 / 重试**这些调用侧语义 —— 它们是应用的事，
  而且超时在 h2 上要靠 `RST_STREAM`，与"这一层只管信封"的边界不符。
