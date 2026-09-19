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
字节流接口 `recv()` / `drain()`（`src/http2/uvcpp_h2_session.h:187,197`）。
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

- **收方向头部列表预算** `h2_header_budget`（`src/http2/uvcpp_h2_common.h:82`）。
  已实测：nghttp2 会把我们宣告的 `SETTINGS_MAX_HEADER_LIST_SIZE` 存进
  `local_settings`，但**接收路径从不累加、也没跟它比过** —— 所以本层自己按
  `namelen + valuelen + 32` 累加，越界立刻 RST(0x0b)。**这是唯一防线，不是第二道。**
- **发方向头部块上限** `H2_MAX_SEND_HEADER_BLOCK`（`uvcpp_h2_common.h:46`）。
  nghttp2 送帧前会拿 `nghttp2_hd_deflate_bound()` 估一个上界，超了它 `return
  NGHTTP2_ERR_FRAME_SIZE_ERROR`，而那个错误码是 `is_non_fatal` 的 —— 它在上层
  被处理成"丢掉整帧、关掉这条流、继续跑"，既不通知我们、也不发 RST_STREAM。
  本层在 `submit_*` 里按同一个公式先算一遍，换成同步的 `UV_EMSGSIZE`
  （`src/http2/uvcpp_h2_session.cpp:875`）。
- **收到对端 GOAWAY 之后不再接受新流。** `on_frame_recv` 记下 `last_stream_id` 与
  错误码，`submit_request` 用 `nghttp2_session_check_request_allowed()` 提前拦，
  同步返回 `UV_ENOTCONN`；`peer_goaway_received()` 等三个取值函数把它暴露出去。
  **在飞的流一条都不动** —— GOAWAY 关的是"新流"，不是"连接"。
- **控制帧令牌桶** `H2_CONTROL_BURST = 64` / `H2_CONTROL_REFILL_PER_SEC = 32`
  （`uvcpp_h2_session.cpp:44-45`）：SETTINGS/PING/RST/PRIORITY/WINDOW_UPDATE 不带
  业务数据，所以给它们单独一个桶；泼出去的那次以 GOAWAY(0x0b) 收尾，而不是
  NO_ERROR —— 否则对端只看到一次"正常关闭"，不知道为什么。
- **协议白名单**：伪头按**方向**白名单（服务端收到 `:status` 即拒）、`:scheme`
  只认 `https`（接受 `http` 等于给混淆代理开后门）、连接专属头一律拒、
  重复且不一致的 `content-length` 即拒、多份 `cookie` 按 `; ` 拼回原样、
  收尾的 trailer 识别成"流的结束信号"（`uvcpp_h2_session.cpp:471`）。

### 1.3 三个接入面

- **低层库**：`uvcpp_http_client::set_http2_enabled`（`src/web/uvcpp_http_client.h:243`）
  与 `uvcpp_http_server::set_http2_enabled`（`src/web/uvcpp_http_server.h:188`），
  **都默认关**。
- **框架（webapp）**：零配置自动协商。ALPN 名单里 `h2` 在前
  （`src/webapp/uvcpp_web_app.cpp:385-386`），用户只能关掉它
  （`set_http2_enabled(false)`，`src/webapp/uvcpp_web_app.h:352`），或者自己设
  一份更权威的 ALPN 名单 —— 设过就不覆盖。
- **关掉 nghttp2 也能编**：`http2/*.h` 不出现在任何公开头里，`uvcpp_http_client.h:43-45`
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
| `tests/functional/web_ssl_h2_client_func.cpp` | 客户端走真 TLS + ALPN；场景 6 是**在 h2 回调里 `delete` 客户端**（ALPN=h2 写成显式前置） |
| `tests/functional/web_ssl_h2_server_func.cpp` | 服务端走真 TLS + ALPN |
| `tests/functional/web_http_client_selfdestroy_func.cpp` | h1：在响应回调 / connect 回调 / keep-alive 第二次请求的回调里 `delete` 客户端（三条路都不许崩） |
| `tests/functional/web_ssl_app_h2_func.cpp` | 框架自动协商（h2 / 退回 h1）；外加**停机道别**与**拆连接时在途流的收尾**两条路 |

`/big`（1 MiB 流式响应）是后两条路共用的那根杠杆：它比默认流控窗口（65535）长
16 倍，所以服务端队列里**一定**还压着"没上线的块"，而客户端那边又不可能在一圈
循环里把它读完 —— 这正是"被作废的块有没有人来收尾"能被观察到的前提。用例因此
把"还没整条收完"也写成显式前置：少了它，一条已经跑完的流会让判据照样通过。

---

## 2. 做了但有折衷（"写死了"）

- **不自实现帧层与 HPACK，直接用 `nghttp2`。** 这与 `docs/web-module-development-plan.md`
  当初的设想不同，§1.3 已如实记了一笔。
- **流控没有自己的策略。** 全 `src/` 零命中 `consume_window` /
  `NO_AUTO_WINDOW_UPDATE` —— 窗口更新完全交给 nghttp2 的自动行为，
  本层既不暴露背压也不做自己的窗口管理。`H2_DEFAULT_INITIAL_WINDOW_SIZE`
  （`uvcpp_h2_common.h:33`）只有定义，别处不读它。
- **流状态机只用了一半。** `h2_stream_state` 有五格
  （`uvcpp_h2_common.h:127-133`），真正被赋过值的只有 `OPEN` / `HEADERS_SENT` /
  `SENT`；`CLOSED` 与 `REJECTED` **从没被赋值过**。
- **`on_fatal` 的文档比实现多一类触发者。** `uvcpp_h2_session.h:151` 说它有三类
  触发者，其中"`want_read`/`want_write` 双双为假"那一类**永远不会发生**：
  这两个函数（`uvcpp_h2_session.h:299,301`）零调用方。
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
  （`src/net/uvcpp_tcp_client.cpp:126`，同一句判据）、`~uvcpp_ws_client` 是同一条策略：
  **泄漏一块仍然有效的内存，换掉一个必然发生的 use-after-free**。要收干净得先让
  "析构可以从回调里被调到"这件事本身消失。
- **两处已死的成员**（只报告，不影响行为）：`HTTP_CLIENT_CLOSING = 0x10`
  （`src/web/uvcpp_http_client.h:62`）全仓零引用；`keep_alive_` 只在
  `uvcpp_http_client.cpp:602,687` 被写、从没被读。

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
| `on_read` 里结算出来的 `done` **没人跑** | 对端一个 `RST_STREAM` 打过来，会话会把那条流还没上线的块整体作废并塞进 `completed` —— 而一个字节都不用回，`flush()` 起不了写，`on_write_done` 就不来，那批 `done` 永远躺在队列里；等它的人（框架流式响应的 `pending_bytes_`）永远等不到 ⇒ 那条流的上下文永远不释放，停机时宽限期白等满 | `on_read()` 收尾补一次 `run_completed()`（`uvcpp_h2_connection.cpp:110-121`） |
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

### 4.2 还没修的（**只记录**）

- **收方向没有自己的背压**（见 2 节流控那条）：窗口完全由 nghttp2 自动更新。
- **`REFUSED_STREAM(7)` 没有映射成 `UV_ECANCELED`。** 收到 GOAWAY 时 nghttp2 会把
  被波及的本端流逐条以 `REFUSED_STREAM` 关掉（4.3 有出处），而 `on_h2_stream_close`
  的判据是"`error_code == 0` 才当 `UV_ECANCELED`，其余一律 `UV_EPROTO`"
  （`uvcpp_http_client.cpp:1234`）⇒ **"这条请求没被处理过、可以安全重试"这个
  信号没有被表达出来**，调用方看到的是 `UV_EPROTO`（协议失败）。要做对得先定下这个
  信号从哪儿带出去（`uvcpp_http_response` 上新增字段，还是另立一个取值函数），
  这一批不动。

### 4.3 盘查时判为缺陷、**核下来不是**的（免得下次再盘一遍）

- **"本层可能提交超过对端 `SETTINGS_MAX_CONCURRENT_STREAMS` 的并发流"—— 不成立，
  此处更正。** `peer_max_concurrent_streams()`（`uvcpp_h2_session.cpp:1201`）确实
  零生产调用方，但 nghttp2 自己就按这个上限**排队**而不是拒绝：超出的请求 HEADERS
  留在 `ob_syn`（`nghttp2_session.c:2315,2346` 上的
  `session_is_outgoing_concurrent_streams_max()` 闸门），流一关
  `num_outgoing_streams` 下降，下一次 `flush()` 就把它放出来 ——
  而 `uvcpp_h2_connection::on_read` 每收下一批字节就 `flush()`。
  所以这里缺的是"多一层保险"，不是协议违规。
- **收到 GOAWAY 时在飞的流不会挂住。** nghttp2 的 `session_close_stream_on_goaway()`
  把 `last_stream_id` 以外、非 idle 非 closed 的**本端**流逐条以
  `REFUSED_STREAM(7)` 关掉（`nghttp2_session.c:2392-2446`），每条都走到本层的
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
`uvcpp.dll`（本机没有 `pwsh.exe`，`copy_test_dlls` 静默不跑），拿旧 dll 跑出来的
绿/红都不算数 —— 比 `build-h2/Release/uvcpp.dll` 与
`build-h2/tests/functional/Release/uvcpp.dll` 的 md5，一致了再跑。

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
