# HTTP/2 在 libuvcpp 里的实际状态

这一页是**盘查记录**，不是路线图：把 HTTP/2 在这个库里的三向状态逐条列清 ——
**真的做了** / **做了但有折衷（写死了）** / **明确不做** —— 每条带出处（`文件:行`）。
目的是让下次读的人不用再从代码和记忆里重新推断一遍。

一句话结论：协议栈本身的完整度来自 `nghttp2`，本层做的是**策略与边界** ——
谁能发什么、收到什么必须当场断、失败怎么让调用方知道。所以「能跑」和「协议栈
完整」不是一回事，第 3 节列的就是这两者之间的差。

---

## 1. 真的做了

### 1.1 会话层（`uvcpp_h2_session`）

建在 `nghttp2` 上，**不含任何 socket、不含任何 libuv 调用** —— 收发就是一对
字节流接口 `recv()` / `drain()`（`src/http2/uvcpp_h2_session.h:231`、`:241`）。
所以这一层既不依赖 TLS 也不依赖事件循环，用例可以拿两个对象面对面地喂字节
（`tests/functional/h2_session_func.cpp`）。

提交面（`src/http2/uvcpp_h2_session.h`）：

| API | 行 | 干什么 |
|---|---|---|
| `submit_request` | :288 | 客户端提请求；body 由本层持有到发完 |
| `submit_response` / `submit_status` | :222 / :229 | 服务端一次发完整响应 / 只发状态码的极简响应 |
| `submit_headers` + `submit_data` + `take_completed` | :244 / :257 / :271 | 流式响应。拆成"提交"与"取完成回调"两步，是为了让 `done` **绝不在调用方栈上同步跑** |
| `submit_rst` / `submit_goaway` | :294 / :296 | 流级 / 连接级收尾 |

### 1.2 边界（这些是"本层自己做的"，不是 nghttp2 给的）

- **收方向头部列表预算** `h2_header_budget`（`src/http2/uvcpp_h2_common.h:119`）。
  已实测：nghttp2 会把我们宣告的 `SETTINGS_MAX_HEADER_LIST_SIZE` 存进
  `local_settings`，但**接收路径从不累加、也没跟它比过** —— 所以本层自己按
  `namelen + valuelen + 32` 累加，越界立刻 RST(0x0b)。**这是唯一防线，不是第二道。**
- **发方向头部块上限** `H2_MAX_SEND_HEADER_BLOCK`（`src/http2/uvcpp_h2_common.h:55`）。
  nghttp2 送帧前会拿 `nghttp2_hd_deflate_bound()` 估一个上界，超了它 `return
  NGHTTP2_ERR_FRAME_SIZE_ERROR`，而那个错误码是 `is_non_fatal` 的 —— 它在上层
  被处理成"丢掉整帧、关掉这条流、继续跑"，既不通知我们、也不发 RST_STREAM。
  本层在 `submit_*` 里按同一个公式先算一遍，换成同步的 `UV_EMSGSIZE`
  （`src/http2/uvcpp_h2_session.cpp:953`）。
- **收到对端 GOAWAY 之后不再接受新流。** `on_frame_recv` 记下 `last_stream_id` 与
  错误码，`submit_request` 用 `nghttp2_session_check_request_allowed()` 提前拦，
  同步返回 `UV_ENOTCONN`；`peer_goaway_received()` 等三个取值函数把它暴露出去。
  **在飞的流一条都不动** —— GOAWAY 关的是"新流"，不是"连接"。
- **控制帧令牌桶** `H2_CONTROL_BURST = 64` / `H2_CONTROL_REFILL_PER_SEC = 32`
  （`src/http2/uvcpp_h2_session.cpp:55-56`）：SETTINGS/PING/RST/PRIORITY/WINDOW_UPDATE 不带
  业务数据，所以给它们单独一个桶；泼出去的那次以 GOAWAY(0x0b) 收尾，而不是
  NO_ERROR —— 否则对端只看到一次"正常关闭"，不知道为什么。
- **协议白名单**：伪头按**方向**白名单（服务端收到 `:status` 即拒）、`:scheme`
  只认 `https`（接受 `http` 等于给混淆代理开后门）、连接专属头一律拒、
  重复且不一致的 `content-length` 即拒、多份 `cookie` 按 `; ` 拼回原样、
  收尾的 trailer 识别成"流的结束信号"（`src/http2/uvcpp_h2_session.cpp:499`）。
- **流关闭的错误码分三档**（`src/web/uvcpp_http_client.cpp:1295`，RFC 9113 §8.7）：
  `NO_ERROR` 是我们自己收摊、`REFUSED_STREAM(7)` 是"这条请求没被处理过"、
  `CANCEL(8)` 是"对端不要这条流了" —— 三档都报 `UV_ECANCELED`；其余一律
  `UV_EPROTO`（协议失败）。其中**只有 `REFUSED_STREAM`** 会把
  `uvcpp_http_response::retryable` 置真：`CANCEL` 不保证对端没处理过（重发就是
  重复副作用），连接断开是"结果未知"，两者都为假。判据只能是"真 ⇒ 可以重试"，
  反过来推**不成立**。`H2_ERR_*` 那几个常量在 `uvcpp_h2_session.cpp` 里各有一条
  `static_assert` 对着 nghttp2 的枚举 —— 那是 `uvcpp_h2_common.h` 里"不在这里造
  一张平行表"那条规矩的保险丝，加了常量却不加断言就等于把它从明处搬到暗处。
- **回调栈里不冲字节。** `uvcpp_h2_session::in_nghttp2()` 为真（正跑在 `mem_recv`
  或 `mem_send` 上）时，`uvcpp_h2_connection::flush()` **直接返回**，把这一冲推迟
  到 `recv()` 返回之后由 `on_read()` 做掉。理由是实测的：在回调里 `submit_rst` +
  `flush()` 会让 nghttp2 在 `session_after_frame_sent1` 里就地关流（释放
  `nghttp2_stream`），而它自己的 `mem_recv` 循环还在用那个指针 —— 完整页堆下必崩
  `0xC0000005`，裸跑却**全绿**（页堆门禁就是为这一类存在的）。推迟是无损的：
  `on_read()` 本来就在 `recv()` 之后冲一次，`on_write_done()` 那条路同理；`drain()`
  自己也会置这个标记，所以"发送期的回调里再冲一次"同样被挡住。

### 1.3 三个接入面

- **低层库**：`uvcpp_http_client::set_http2_enabled`（`src/web/uvcpp_http_client.h:255`）
  与 `uvcpp_http_server::set_http2_enabled`（`src/web/uvcpp_http_server.h:190-190`），
  **都默认关**。
- **框架（webapp）**：零配置自动协商。ALPN 名单里 `h2` 在前
  （`src/webapp/uvcpp_web_app.cpp:447-447`，`kDefaultAlpn` 的定义），用户只能关掉它
  （`set_http2_enabled(false)`，`src/webapp/uvcpp_web_app.h:356`），或者自己设
  一份更权威的 ALPN 名单 —— 设过就不覆盖。
- **关掉 nghttp2 也能编**：`http2/*.h` 不出现在任何公开头里，`src/web/uvcpp_http_client.h:43-45`
  写明了理由（引了就把 nghttp2 的 include 路径扩散给每个使用者与测试 TU）。
  `UVCPP_ENABLE_NGHTTP2` 默认 OFF（`CMakeLists.txt:61`），缺 OpenSSL 或缺 web
  模块时还会被强制置 OFF 并告警。

### 1.4 依赖与产物

`nghttp2` 是**唯一被强制静态**的依赖：`uvcpp_nghttp2_configure()`
（`CMakeLists.txt:266`，在 function 作用域里 `BUILD_STATIC_LIBS ON`）把一个
静态 `nghttp2_static` 交给链接器。实测 `pe_imports()`：开了 h2 的 DLL 与没开的
相比，非白名单依赖集合**都是** `llhttp.dll uv.dll zlib1.dll` —— 开 h2 不新增
任何 DLL 依赖。

### 1.5 测试

| 文件 | 测什么 |
|---|---|
| `tests/functional/h2_session_func.cpp` | 会话层面对面（自定义头部往返、流式、RST、洪泛、头部预算的两个方向、GOAWAY 的两个方向…） |
| `tests/functional/web_ssl_h2_client_func.cpp` | 客户端走真 TLS + ALPN；场景 6 是**在 h2 回调里 `delete` 客户端**；场景 7 里对端的 handler **故意在 `recv()` 的栈上 `flush()`**（与 `uvcpp_http_server` 的处理函数同一形状），是"回调栈里不冲字节"那条不变式的活体判据；场景 7 用**裸 h2 对端**（`uvcpp_http_server` 造不出指定错误码的 RST —— 它的 `reject()` 把码写死了）按剧本发 `REFUSED_STREAM` / `CANCEL` / `NO_ERROR` / `PROTOCOL_ERROR`，八条流串行发、每条都断言"恰好回调一次 + 流号是递增奇数"，中间夹的 `/hello` 是"连接没被流级 RST 带下水"的判据；状态码那一维**两头都钉**：`/refuse` 那几条头没到过，必须是 `HTTP_STATUS_NONE`，`/part`（对端先发一个不结束流的头、再 RST）必须把真的 `200` 交出去 —— 少了后者，一个"永远交 `HTTP_STATUS_NONE`"的实现也能过；场景 9 钉**断开时还在飞**的流（`on_h2_disconnect` 那条兜底路）—— 对端收下请求之后既不回也不 RST、直接拆 TCP，交付的必须是"没收到过"配一个正的奇数流号，而同一条路上"头先到、连接后断"的那半边必须把真的 `200` 与 `x-mark` 交出去；场景 8 钉响应的拷贝构造 / 赋值（ALPN=h2 处处写成显式前置） |
| `tests/functional/h2_backpressure_func.cpp` | 收方向背压（面对面内存装置，无 socket 无 TLS）。场景 1 用**4096** 的每流窗口只钉流级（暂停的流恰好收到一个窗口就停住、暂停期一个该流的 WINDOW_UPDATE 都没有、恢复后欠账**恰好**还 4096 且余下字节到齐）；场景 2 用 **131072** 的窗口只钉连接级 —— 暂停的流推进到**超过 65535** 而同一连接上另一条流照样收满 393216，这是"真流级背压"与"连接级误伤"唯一的区分点，`server_window > 65535` 是硬要求（窗口 ≤ 65535 时"连接级也扣住"的坏实现照样绿）；场景 3 钉 `UV_EINVAL` 与幂等 |
| `tests/functional/h2_backpressure_flush_func.cpp` | 恢复必须**真的把字节冲出去**（真 socket，手搓帧的裸对端）。会话层的 `resume_stream()` 只排队，应用最自然的调用时机（自己的定时器里）**没有任何人会替你 flush** —— 漏掉它的表现是"对端永远等不到窗口、流挂死"且**不报错**。本用例把 `conn->resume_stream(1)` 换成 `conn->session().resume_stream(1)` 就必须变红 |
| `tests/functional/web_ssl_h2_server_func.cpp` | 服务端走真 TLS + ALPN |
| `tests/functional/web_http_client_selfdestroy_func.cpp` | h1：在响应回调 / connect 回调 / keep-alive 第二次请求的回调里 `delete` 客户端（三条路都不许崩） |
| `tests/functional/web_http_client_close_func.cpp` | h1：对端在响应收完之前断开（回调必须落地、不许重复交付、死连接上 `send()` 报 `UV_ENOTCONN`；场景 3 钉"`404` 的头到了、正文没发完就断"时交付的状态码必须是**那个 404**） |
| `tests/functional/web_ssl_app_h2_func.cpp` | 框架自动协商（h2 / 退回 h1）；外加**停机道别**与**拆连接时在途流的收尾**两条路 |

`/big`（1 MiB 流式响应）是后两条路共用的那根杠杆：它比默认流控窗口（65535）长
16 倍，所以服务端队列里**一定**还压着"没上线的块"，而客户端那边又不可能在一圈
循环里把它读完 —— 这正是"被作废的块有没有人来收尾"能被观察到的前提。用例因此
把"还没整条收完"也写成显式前置：少了它，一条已经跑完的流会让判据照样通过。

---

## 2. 做了但有折衷（"写死了"）

- **不自实现帧层与 HPACK，直接用 `nghttp2`。** 这与最初那份模块开发计划里的设想不同
  —— 那份计划是 `docs/web-module-development-plan.md`，而 `docs/` 在 `.gitignore` 里，
  **只存在于开发机上**（clone 下来没有这个文件），§1.3 已如实记了一笔。
- **流控有自己的策略，但只有**协议层**那一半。** 会话层打开
  `nghttp2_option_set_no_auto_window_update()` 并自己归还窗口
  （`src/http2/uvcpp_h2_session.cpp` 的 `on_data_chunk`），对外给出
  `pause_stream()` / `resume_stream()`（会话层、连接层各一对）、
  `set_max_out_stream_bytes()` 那道出站上界、`peer_window_size()`。
  **框架侧一个调用点都没有** —— `uvcpp_web_response` / `uvcpp_http_server`
  全仓零命中 `pause_stream`，所以今天它只对**直接用低层会话/连接**的调用方可达。
  详见 §4.2。`H2_DEFAULT_INITIAL_WINDOW_SIZE`
  （`src/http2/uvcpp_h2_common.h:44`）**仍然**只有定义、别处不读它：那套策略拿
  "已经宣告出去的窗口"当尺子记账，不改这个常量的取值。
- **流状态机只用了一半。** `h2_stream_state` 有五格
  （`src/http2/uvcpp_h2_common.h:166-173`），真正被赋过值的只有 `OPEN` / `HEADERS_SENT` /
  `SENT`；`CLOSED` 与 `REJECTED` **从没被赋值过**（两个枚举值上也标了这一点）。
- **`on_fatal` 实际只有两类触发者。** 头注释已写明（`src/http2/uvcpp_h2_session.h:182-194`）：
  三类里的"`want_read`/`want_write` 双双为假"那一类**不可达** —— 这两个是公开成员
  （`src/http2/uvcpp_h2_session.h:373`、`:375`），但本层没有任何一处拿它们判定致命。
- **发方向上限的公式是复刻的。** `header_block_fits()` 与
  `nghttp2_hd_deflate_bound()` 逐字一致（后者 `(void)deflater`，是 nv 数组的
  纯函数，所以复刻不会随连接状态漂），`+5` 是 `NGHTTP2_PRIORITY_SPECLEN`。
  **升级 nghttp2 时这几处要一起核。**
- **没有发 trailer 的 API。** 收方向认 trailer，发方向只能"HEADERS 带 END_STREAM"。
- **"在回调里被析构"走的是有意泄漏。** `~uvcpp_http_client` 判到 `loop_->is_running()`
  —— 也就是本对象正压在它自己的某个回调栈上被删 —— 就**一个都不拆**：`tcp_` /
  `loop_` / `parser_` / `h2_` / `ssl_` 全留给循环，只把**底层**读停掉、把裸句柄关掉。
  那一摞栈帧各自还在用这些成员（`execute()` 在用 `parser_`、正在执行的读闭包就住在
  `tcp_` 里、`uv_run` 的嵌套计数要在 `uv_run` 返回之后才减），谁来拆都是往栈上还在用的
  内存里写。代价是**每个这样被删掉的客户端漏一份**（`uvcpp_loop` + `uvcpp_tcp_client`
  + 解析器，h2 上再加一个 nghttp2 会话）。这与 `~uvcpp_tcp_client`
  （`src/net/uvcpp_tcp_client.cpp:155-155`，同一句判据）、`~uvcpp_ws_client` 是同一条策略：
  **泄漏一块仍然有效的内存，换掉一个必然发生的 use-after-free**。要收干净得先让
  "析构可以从回调里被调到"这件事本身消失。
- **一处已死的成员**（只报告，本批没动）：
  - `HTTP_CLIENT_CLOSING = 0x10`（`src/web/uvcpp_http_client.h:62`）全仓零引用 ——
    这一处**确实不影响行为**，它只是个没接线的状态位。
- **`keep_alive_` 那一处已经修掉了**（原先列在上面这条清单里，因为后果是真的）。
  当时的形状是：它只在 `on_response_complete()` 里被写、**从没被读**，于是调用方
  在**对端已经声明要关**的连接上接着 `send()`，写进一个正在收摊的 socket，拿到的
  是一条"连接被重置"的失败（状态码 `HTTP_STATUS_NONE`）；`set_keep_alive(false)`
  也是空的，一个字节都不影响发出去的请求。
  现在两个方向都兑现了：响应说 `close`（含 `Connection` 逗号列表里不在首项的
  `close`）就在交付回调之前清掉 `HTTP_CLIENT_CONNECTED`
  （`src/web/uvcpp_http_client.cpp:636-656`），第二次 `send()` 同步拒成
  `UV_ENOTCONN` —— 与 h2 那条 `peer_goaway_received()` 的提前拦截同一形状；
  `set_keep_alive(false)` 则让请求真的带上 `connection: close`
  （`src/web/uvcpp_http_client.cpp:452-467`）。
  用例：`tests/functional/web_http_client_keepalive_func.cpp`。
  两件事都属于 h1，与本页的 h2 无关，留在这儿只为了让下次盘查的人不必再核一遍。

---

## 3. 明确不做（非目标）

| 不做 | 说明 |
|---|---|
| **h2c（明文 h2）** | 不做 `Upgrade: h2c`、不做 prior-knowledge、不做协议嗅探。本库的 h2 一律走 TLS + ALPN。 |
| **RFC 8441（WebSocket over HTTP/2）** | 不做，看到 `:protocol` 伪头一律按未知伪头拒（`uvcpp_h2_session.cpp` 的 `handle_pseudo`）。 |
| **HTTP/3** | 不做。 |
| **SERVER_PUSH** | 既不发也不收：`SETTINGS_ENABLE_PUSH = 0` 在 `init()` 里就宣告了。 |
| **h2spec** | 不引入。协议一致性靠上面 1.5 那四个用例 + 会话层用例。 |
| **改 `uvcpp_s` / `uvcpp_a_s` 的语义** | 不动。 |

---

## 4. 已知缺口

### 4.1 已修（除下面注明的一条外，都有用例钉着，且都做过变异 A/B）

批 2（发方向上限与流登记）：

| 缺陷 | 症状 | 修法 |
|---|---|---|
| 发方向超上限**静默丢帧** | `submit_request` 返回一个正的流号，然后两边一个字节都不发、永远等下去 | `submit_*` 里按同一公式先算，返回同步的 `UV_EMSGSIZE` |
| 空 body 的 `submit_request` **不登记流** | 对端一个 `RST_STREAM` 打过来时 `on_stream_close` 在 `streams.find()` 那步静默返回，`cbs.on_close` 一声不吭 —— 而 `uvcpp_http_client` 全靠它给挂起的请求结算（`on_h2_stream_close`）。**空 body 恰恰是 GET 的常态** | 两条路都登记 |
| `submit_response` / `submit_headers` **先置位再失败** | 置了 `SENT`/`HEADERS_SENT` 之后才因头部超限退掉，于是流停在"说过要发、其实一个字节没发"：重试被 `UV_EALREADY` 挡，对端永远等一条不会来的响应 | 校验挪到置位之前，失败保证**流状态没被改过** |

批 3（收到的 GOAWAY）：

| 缺陷 | 症状 | 修法 |
|---|---|---|
| `submit_goaway` 的 `last_stream_id` **写死 0** | 填 0 的含义是"我一条都没处理"，对端据此把**所有**在飞的流当成可重试的关掉（`REFUSED_STREAM`）—— 而这些请求在我们这边的副作用可能已经跑完了，对端一重试就是重复副作用 | 改填 `nghttp2_session_get_last_proc_stream_id()`（nghttp2 的文档原话：这个返回值可以直接当 `submit_goaway()` 的 `last_stream_id`） |
| 收到 GOAWAY 后**照样发新请求** | 对端已经说"不再处理新流"，我们仍然照发，还发得出去 | `on_frame_recv` 记下三个字段；`submit_request` 用 `nghttp2_session_check_request_allowed()` 提前拦，同步返回 `UV_ENOTCONN`（"同步拒绝、什么都没发生"，与 `UV_EINVAL`/`UV_EMSGSIZE` 同一条契约） |
| 对端发过 GOAWAY 这件事**本端不可查询** | 错误码是 0 正是**正常**的优雅退出，跟"什么都没收到"分不开；`uvcpp_http_client` 会继续接受 `send()` | 新增 `peer_goaway_received()` / `peer_goaway_error_code()` / `peer_goaway_last_stream_id()` |

批 4（收尾：道别 + 拆连接）：

| 缺陷 | 症状 | 修法 |
|---|---|---|
| 服务端停机**不道别** | 停机时直接关连接，对端只看到连接断了，分不清"服务端在收摊"和"网络挂了" | 新增 `uvcpp_http_server::begin_h2_goaway()` 与 `uvcpp_h2_connection::begin_goaway()`；webapp 停机的第 0 拍只发 GOAWAY，**隔一拍**才关连接 —— 挤在同一拍里对端拿到的仍然是"断了" |
| `on_read` 里结算出来的 `done` **没人跑** | 对端一个 `RST_STREAM` 打过来，会话会把那条流还没上线的块整体作废并塞进 `completed` —— 而一个字节都不用回，`flush()` 起不了写，`on_write_done` 就不来，那批 `done` 永远躺在队列里；等它的人（框架流式响应的 `pending_bytes_`）永远等不到 ⇒ 那条流的上下文永远不释放，停机时宽限期白等满 | `on_read()` 收尾补一次 `run_completed()`（`src/http2/uvcpp_h2_connection.cpp:110-121`） |
| 拆连接时**丢掉**在飞的 `done` | 同一条路的另一半：连接被整个丢掉（对端 `close()`、不发 RST）时 `remove_ctx` 直接 `delete` 掉 h2 层，队列里那些块一声不吭 —— 与 h1 那边"队列里的写一律以 `UV_ECANCELED` 唤醒"的既定契约不一致 | 新增 `take_cancelled_dones()`：会话层 `cancel_pending_out()` 把待发块整体作废并**取走**，由 `remove_ctx` 在 `contexts_.erase(it)` **之后**才逐个跑 —— 跑早了 `write_stream()` 会掉进 h1 分支，把收尾时补的那笔写挂到一条根本不是 h1 的连接上（`stream_id` 被静默丢掉） |
| `run_completed()` 里两处 **use-after-free** | 令牌已死那一支还去写 `in_dones_ = false`；`on_write_done` 里 `flush()` 可能同步走到 `finish_close()` → `c->close(cb)` → `notify_disconnect()`，而持有者的契约正是"在这里销毁本对象" ⇒ 之后每一句都在往释放过的内存上写 | 令牌在每一句之前重新问一次；令牌真死了**连收尾都不做**（要收尾的对象已经不存在，没什么可收的） |
| 服务端析构**漏掉** h2 层 | `~uvcpp_http_server()` 只 `delete parser`，而 h2 层是连接上下文 `new` 出来的、没有任何别的表登记它 ⇒ 每条还活着的 h2 连接漏一个 nghttp2 会话和它的缓冲区 | 析构里一并 `delete`。走得到这里的只有"客户端先于服务端被释放"那条路：`close_all_clients()` 在句柄已关完时走 `release_client()`，只 `delete`、一个回调都不发 ⇒ `remove_ctx` 从没跑过 |

**最后那条（析构漏 h2 层）没有变异覆盖，如实记一笔。** 它漏的是内存，而本仓没有
任何能数 nghttp2 对象的地方 —— `~uvcpp_h2_session()` 只做一次 `nghttp2_session_del`，
不留任何可观察的痕迹，为它造一个计数器超出这一批的范围。所以这一条是**读代码核
出来的**，不是跑出来的；别把"没有用例"读成"没有缺陷"。

批 5（客户端在**它自己的回调里**被 `delete`）：

| 缺陷 | 症状 | 修法 |
|---|---|---|
| **在它自己的回调里 `delete` 客户端会崩** | "响应回来就把客户端扔了"是本类最自然的用法，而它此前**必然崩**。析构里那段关闭 dance 会 `loop_->run(UV_RUN_NOWAIT)` 泵若干轮，把**还压在栈上**的那条读路径再叫一遍。cdb 实测栈：`llhttp → uvcpp_http_parser::execute → on_tcp_data → ~uvcpp_http_client+0x45e → callback_read → uv_run`，`0xC0000005` | 判到 `loop_->is_running()`（= 本对象正压在自己的回调栈上）就**一个都不拆**，只把**底层**读停掉、把裸句柄关掉。停的是裸句柄的 `read_stop()`，不是包装对象的 —— 后者会把 `read_fn_`/`read_arg_` 清掉，而那正是此刻压在栈上还没返回的那个闭包的家 |
| 析构之后**仍然挂在别人身上**的闭包 | 上一条修完，对象"死"了但内存还在（泄漏换的），而读闭包、解析器那三个回调、h2 的 `on_body`/`on_response_end`/`on_close`/`on_disconnect` 都还捕着 `this` —— 下一次被叫起来就是往释放过的内存上写 | 一枚 `alive_token_`（`std::shared_ptr<char>`）：析构**第一件事** `reset()`，所有捕回本对象的闭包都捕它的 `weak_ptr` 并自查 `expired()`。**必须是令牌而不是 `bool` 成员** —— 闭包跑起来时对象内存已经还了，读任何成员都是释放后使用。`on_tcp_data` 因此多收一个形参，令牌由那条读闭包带进来 |
| 两处"**调完再置空**" | `write` 失败那条与 `on_tcp_data` 里报错那条都是先 `user_cb_(...)` 再 `user_cb_ = nullptr` —— 用户回调里 `delete this` 的话，第二句就是往已释放的内存上写 | 先把 `std::function` **挪出来**再调（与既有的 `on_response_complete` 同形状） |

**批 5 的变异覆盖要分两半说，差别很大。**

被**直接**钉住的只有析构里那个重入判据：把它改成恒假（m1），新用例当场
`0xC0000005`（rc=`3221225477`）—— 那正是它修之前的样子，所以用例**不是空转**。

而那 11 处 `tok.expired()` 守卫**单跑一个都钉不住**：逐条删（m2 读闭包不查令牌、
m3 析构不作废令牌、m4 拆掉停读与关句柄、m5 连令牌作废一起拆）两棵树都是绿的。
裸跑看不出毛病是有原因的 —— `free` 掉的那块内存还在、内容还是旧的，读它读不出错。
**换成完整页堆就现原形**：11 处全删（m6）之后裸跑**依旧绿**，页堆下**必然**
`0xC0000005`，cdb 顶帧是 `uvcpp_http_client::on_tcp_data+0x63` ——
`parser_->execute()` 返回之后那句 `parser_->has_error()`，它要从**已释放的对象**里
读 `parser_` 这个成员。守卫都在时，同一道门禁两遍都绿。

所以这两条防线性质不同：**"停读 + 不拆"是裸跑就看得见的**，**令牌守卫是只有页堆
看得见的**，而且它防的是"在响应回调里析构"之外的路径（同一条连接上还有第二个响应
在路上之类），今天**没有用例走到那里**。`tests/tools/run_pageheap_gate.py` 是这一批
配套的判据，不是可选项。

批 6（h2 流关闭的错误语义）：

| 缺陷 | 症状 | 修法 |
|---|---|---|
| **`REFUSED_STREAM` / `CANCEL` 报成协议失败** | 判据是"`error_code == 0` 才当 `UV_ECANCELED`，其余一律 `UV_EPROTO`" ⇒ 对端的"这条请求没被处理过"（§8.7 明说可以安全重发；GOAWAY 牵连时 nghttp2 就是这么关的，见 4.3）和"我不要这条流了"这两种**都不是协议失败**的关闭，交付给调用方的是 `UV_EPROTO` | `on_h2_stream_close` 按 §8.7 分三档：三码 `UV_ECANCELED`、其余 `UV_EPROTO` |
| **"可以重试"这个信号公开面上不存在** | 就算把三档分开，"没被处理过"和"这条流被取消了"在调用方看来仍然一模一样 —— 而 `CANCEL` 是**可能已经处理过**的，盲目重发就是重复副作用 | 新增 `uvcpp_http_response::retryable`。**挂在响应上而不是客户端上**：h2 一条连接同时有好几条流在飞，挂在客户端上分不清是哪一条。只有 `REFUSED_STREAM` 为真 |
| **响应的拷贝构造 / 赋值漏字段** | 两个函数是手写的（`UVCPP_DEFINE_COPY_FUNC` 只声明），逐字段列一遍 —— `stream_id` 从加进来那天起就漏着，`retryable` 会跟着一起漏。在回调里存一份副本再据此重试是很自然的写法，副本里"这条没被处理过"就变成了"处理过了" | 两处都补齐；`scenario 8` 钉住 |

批 6 的变异（只重编库，`--target uvcpp`）：**m1** 三档退回成"只认 `NO_ERROR`" ⇒
`/refuse` 与 `/cancel` 的 `err` 两条断言同时红；**m2** 分类留着但不置 `retryable` ⇒
只有 `/refuse` 的 `retryable` 红；**m3** 拷贝构造漏掉两个身份字段 ⇒ `scenario 8`
的两条红。三次还原之后全绿 —— 用例不是空转。

批 7（错误路径上交付的状态码**是编出来的**）：

| 缺陷 | 症状 | 修法 |
|---|---|---|
| h2：流在响应头到达**之前**被 RST 时，交付的是默认构造的 `200` | `uvcpp_h2_stream::response` 的初值是 `uvcpp_http_response()`，而它的默认构造是 `200 OK`。`on_h2_stream_close` 把"已经解析出来的那部分照给"，可这种流**什么都没有** —— 调用方于是拿到 `{status: 200, retryable: true}` 配一个非零的 `err`：日志里就是"请求失败了，但状态码 200"，而那个 200 对端从没说过 | 新增 `HTTP_STATUS_NONE`（0，不是任何合法状态码，`http_status_reason()` 给它 `"Unknown"`）；`uvcpp_h2_stream::response` 的初值换成 `h2_response_not_received()`。**做成函数而不是在成员上写初始化**：流有三条创建路径（`stream_of()` 建流、`on_begin_headers` 收到头、`submit_request` 发出去），漏掉任何一条就是一个编出来的 200 —— 第一次就是这么漏的（`stream_of()` 那条） |
| h1：同样的编造，而且**连真到过的状态码也丢** | `pending_resp_` 只在 `on_response_complete()`（整条响应收完）里填，而错误路径（`on_tcp_close` / 写失败）交付的正是它 ⇒ 对端明明回过 `404 Not Found`、正文发一半才断，调用方拿到的还是 `200 OK`。请求开始时那个 `pending_resp_ = uvcpp_http_response()` 就是 200 的来源 | 请求起点把状态码显式置成 `HTTP_STATUS_NONE`；`on_headers_complete`（`extract_metadata()` 排在它**之前**，所以那时状态行与头部都已经齐了）就把**真的**状态码、原因短语、头部记进 `pending_resp_` |

批 7 的变异（只重编一个 .cpp，不动头文件）：**m1** `stream_of()` 建流时把状态码摆回
默认的 200 ⇒ 场景 7 的 `/refuse` 四条 RST 断言红；**m2** 流关闭路永远交付
`HTTP_STATUS_NONE` ⇒ `/part`（头到过、流才断）那条红 —— 少了它，一个"永远交
NONE"的实现也能全绿；**m3** h1 不在头完成时记状态码 ⇒ `web_http_client_close_func`
场景 3 红（实际拿到的是 0，不是对端说过的 404）。三次还原之后全绿。

批 8（批 7 漏掉的**同一条路**：断开时还在飞的流）：

| 缺陷 | 症状 | 修法 |
|---|---|---|
| h2：`on_h2_disconnect` 的兜底结算交付的是**默认构造**的 `uvcpp_http_response` —— 又是编出来的 `200`，而且 `stream_id` 恒为 0 | 这条路的触发条件是"断开时 `h2_streams_` 里还有没结算的流"：对端收下请求、既不回也不 RST、直接把 TCP 拆掉。此时调用方拿到 `{status: 200, stream_id: 0}` 配 `UV_ECANCELED`。`stream_id` 在 h2 上是"这条回应的是哪次请求"的**唯一**凭据（h1 那条路上它恒为 0），一条连接上同时有几条流在飞时，0 等于把失败和请求的对应关系丢了。批 7 只扫了 `on_h2_stream_close` 与 h1 两条交付路，漏了这里 —— 场景 4 虽然也走断开，但它是**先等 `/hello` 落地再拆**的，那时 `h2_streams_` 已经空了，兜底那段一行都不跑 | `delete h2_` **之前**逐条把在飞流的真响应抄进一张局部表（`find_stream()` + `hs->response`），交付时用它、并按 `kv.first` 填 `stream_id`。抄不到就退回 `h2_response_not_received()`。`retryable` 不动：连接是怎么没的、对端处理过没有都不知道，只有 `REFUSED_STREAM` 那种对端**明说了**"没处理过"的才敢标可重试 |

批 8 的变异（只重编 `uvcpp_http_client.cpp`）：**N1** 抄真响应那段整个拆掉（永远交
`HTTP_STATUS_NONE`）⇒ 场景 9 的 `/vanish-head` 那一半红（状态码 + `x-mark`）；
**N2** 交付退回默认构造的 `uvcpp_http_response`（就是修之前那份代码）⇒ `/vanish`
那一半红（编出来的 200）；**N3** 不填 `stream_id` ⇒ 两半的流号断言都红。三次还原
之后全绿。

场景 9 的判据是**两头钉死**的，理由与批 7 的 `/part` 同源：只有"头没到过必须是
`HTTP_STATUS_NONE`"这半，一个"永远交 `HTTP_STATUS_NONE`"的实现也能全绿；只有另一半，
"永远交默认的 200"也能全绿。而 `/vanish-head` 那半**不敢把判据放在状态码上** ——
`200` 恰好就是默认值，撞上也说得通，所以钉的是一个只有对端会发的头 `x-mark`。

### 4.2 还没修的（**只记录**）

- **收方向的框架级背压**：协议层机件已落地（§2 流控那条、§1.5 的两个新用例），
  但框架侧**没有调用点**。h2 上"边收边给"的流式 body 今天仍不可达：流式认领钩子
  `stream_claim_` 全仓只有一处调用点，在 **llhttp(h1)** 的解析器里
  （`src/web/uvcpp_http_server.cpp`），h2 请求走 `dispatch_h2_request` →
  `find_handler`，从不碰它。

**此处更正（`1.2.25-dev`）—— 本文件此前在这里写过两条判断，都撤回。**

1. 「这个开关是**全局**的（`NO_AUTO_WINDOW_UPDATE`）：打开之后每一条消费路径都得
   自己 `consume_window`，漏掉任何一条都会让上传在 64 KiB 处**永久停住** ——
   半套比现状更危险。」**方向说反了。** 凡到不了我们回调的字节，nghttp2 自己会把
   连接窗口还掉：被忽略的 DATA 走 `nghttp2:nghttp2_session.c:6948-6950`、messaging
   判违规的走 `:6854-6857`、Pad Length / Padding 走 `:6726` / `:6837`。要我们负责的
   只有**从 `on_data_chunk` 进来的那些字节** —— 一条路径，不是一套。而"漏掉"的真实
   后果也不是停住：`adjust_recv_window_size` 一旦失败就
   `nghttp2_session_terminate_session(FLOW_CONTROL_ERROR)`（`:5092` / `:5121`），
   **整条连接当场死**。比停住更响，也更好判。
2. 「要给单条流减速，框架层现成的连接级 `read_pause()` 是眼下更合适的粒度。」
   **两者不是一个粒度。** 连接窗口是全连接共享的 65535，连接级暂停会连带停掉同一条
   连接上**别的流**；这不是理论问题 —— 本笔为此专门把连接级归还做成无条件的，并在
   §1.5 那个场景 2 里把它钉死（窗口取 131072 > 65535 正是为了让"连接级也扣住"的坏
   实现没法蒙混过去）。

### 4.3 盘查时判为缺陷、**核下来不是**的（免得下次再盘一遍）

- **"本层可能提交超过对端 `SETTINGS_MAX_CONCURRENT_STREAMS` 的并发流"—— 不成立，
  此处更正。** `peer_max_concurrent_streams()`（`src/http2/uvcpp_h2_session.cpp:1329`）确实
  零生产调用方，但 nghttp2 自己就按这个上限**排队**而不是拒绝：超出的请求 HEADERS
  留在 `ob_syn`（`nghttp2:nghttp2_session.c:2315,2346` 上的
  `session_is_outgoing_concurrent_streams_max()` 闸门），流一关
  `num_outgoing_streams` 下降，下一次 `flush()` 就把它放出来 ——
  而 `uvcpp_h2_connection::on_read` 每收下一批字节就 `flush()`。
  所以这里缺的是"多一层保险"，不是协议违规。
- **收到 GOAWAY 时在飞的流不会挂住。** nghttp2 的 `session_close_stream_on_goaway()`
  把 `last_stream_id` 以外、非 idle 非 closed 的**本端**流逐条以
  `REFUSED_STREAM(7)` 关掉（`nghttp2:nghttp2_session.c:2392-2446`），每条都走到本层的
  `on_stream_close` → `cbs.on_close`，调用方拿到的是"可以重试"而不是干等。
  **这件事依赖批 2 那条"空 body 也登记流"的修复** —— 修之前，一条被 GOAWAY
  波及的 GET 在业务层是一声不吭的。

---

## 5. 怎么验证

两棵树都要过（`--parallel 4`，MSVC 并行高了会撞堆上限）：

```
cmake -S . -B build-h2      -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_NGHTTP2=ON
cmake --build build-h2 --config Release --parallel 4
ctest --test-dir build-h2 -C Release --timeout 60
```

**本机陷阱**（踩过不止一次）：构建成功与否要看构建日志里有没有 `error C` /
`error LNK`，不能只看退出码；而 `--target test_xxx` **不会**刷新测试目录里那份
`uvcpp.dll`（见下面第 5 条），拿旧 dll 跑出来的绿/红都不算数 —— 比
`build-h2/Release/uvcpp.dll` 与 `build-h2/tests/functional/Release/uvcpp.dll` 的
时间戳（数值 `mtime`；`cp` 不保留 `mtime`，所以判据是"目标不早于源"，比 md5 更省），
刷新了再跑。

批 5 又补了三条，性质一样（都是**产物或环境的问题冒充代码的问题**）：

1. **动过任何头文件就必须不带 `--target` 全量构建。** 往 `uvcpp_http_client` 里加
   一个私有成员之后，那批**按值**在栈上持有它的测试 exe（旧 `sizeof`）会在 `main`
   之前 `0xC0000409` 死掉、**一个字节都不输出**；用 `new` 的用例和刚重链过的用例
   全绿 —— "一部分红一部分绿"正是它的指纹。只建 `--target uvcpp` 不重链任何 exe。
   纯 `.cpp` 变异不受影响。
2. **自毁那条路要量重复率，单跑一次绿不算数。** 它是不是崩取决于释放掉的内存有没有
   被复用 —— 同一份代码两棵树可以一次绿一次红。这一批修完是两棵树各 60 次全绿。
3. **页堆门禁是这一批的必要判据**，不是"有空再跑"：
   `python -u tests/tools/run_pageheap_gate.py --tree build-h2 --exe test_web_http_client_selfdestroy_func`
   —— 令牌守卫那一层的效力**只有它看得见**（删掉守卫后裸跑照样绿，页堆下必然违例）。

批 6 再补一条，还是环境冒充代码：

4. **`--parallel` 高了会以两种面目失败，两种都与代码无关。** 并行度高时是 MSBuild
   自己的托管节点 `System.OutOfMemoryException`（`error MSB4018` / `MSB4166`），
   日志里一个 `error C` 都没有；降到 `--parallel 1` 之后换 `cl.exe` 自己报
   `fatal error C1002`（第 2 遍编译器的堆空间不足）。两者都会**留下一个没生成的
   exe 而其余目标照常绿** —— "一部分红一部分绿"正是它的指纹。判据始终是
   `grep -c "error C[0-9]\|error LNK\|error MSB"`（注意 `MSB40` 这种收窄的写法会
   漏掉 `MSB8071`），**不能只看退出码**；把失败的那个目标单独重跑一次通常就过了
   —— 内存压力是瞬时的，不是那份代码编不出来。

5. **"`copy_test_dlls` 因为本机没有 `pwsh.exe` 所以静默不跑"—— 这条以前写在这里，
   是错的，批 8 更正。** 两半都不对：

   - `copy_test_dlls` 是 `add_custom_target(... ALL ...)`，命令全是
     `${CMAKE_COMMAND} -E copy_if_different`，**一个 `pwsh` 都不沾**，构建日志里
     "Copy uvcpp/libuv DLLs into test folders" 那行就是它。它确实会跑。真正
     `0xc0000135` 满屏的根因是这张目标**曾经没带 `ALL`**（见 `CMakeLists.txt:1148`
     的注释），与 `pwsh` 无关。
   - 日志里那句 `'pwsh.exe' 不是内部或外部命令` 来自 **vcpkg 的
     `scripts/buildsystems/msbuild/vcpkg.targets`**（`applocal.ps1`），而且它自己
     就写了 "falling back to system PowerShell" —— 只是一条降级提示，不影响退出码。
     它拷的是 vcpkg 侧那些 DLL（如 `zlib1.dll`），与 `uvcpp.dll` / `uv.dll` /
     `llhttp.dll` 无关。

   所以 `--target test_xxx` 之后要手工刷新的**只是那一份 `uvcpp.dll`**（目标目录里
   那份是上一次全量构建的），判据是数值 `mtime`：目标目录里那份不早于
   `<tree>/Release/uvcpp.dll` 即可。升级本机 `pwsh` 也好、把它加进 `PATH` 也好，
   都不会改变这条。
