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
| `tests/functional/web_ssl_h2_client_func.cpp` | 客户端走真 TLS + ALPN |
| `tests/functional/web_ssl_h2_server_func.cpp` | 服务端走真 TLS + ALPN |
| `tests/functional/web_ssl_app_h2_func.cpp` | 框架自动协商（h2 / 退回 h1） |

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
- **两处已死的成员**（只报告，不影响行为）：`HTTP_CLIENT_CLOSING = 0x10`
  （`src/web/uvcpp_http_client.h:62`）全仓零引用；`keep_alive_` 只在
  `uvcpp_http_client.cpp:517,602` 被写、从没被读。

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

### 4.1 已修（都有用例钉着，且都做过变异 A/B）

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

### 4.2 还没修的（本次盘查发现，**只记录**）

- **收方向没有自己的背压**（见 2 节流控那条）：窗口完全由 nghttp2 自动更新。

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
