<!-- doc-snippets: fragments-default — 本页的例子是摘录，不是完整翻译单元；要编的逐条标 `// doc-snippet: compile`。约定见 CONTRIBUTING.md。 -->

# webapp 应用框架开发者指南

`src/webapp/` 是建在 web 层（HTTP / WebSocket 协议）之上的**应用框架**：路由、中间件、
请求/响应封装、静态文件、上传、流式响应、WebSocket（含客户端）、日志、JSON。

底层 `src/web/` 是"协议工具箱"——要自己拼响应报文、自己匹配路由、自己管连接生命周期。
框架层把这些收口掉，使用者只写业务 handler。

- 打开方式：`-DUVCPP_BUILD_WEBAPP=ON`（**需要** `-DUVCPP_BUILD_WEB=ON`；web 关掉时会
  强制关掉 webapp，而不是留一个编译不过的配置）
- 依赖：webapp 会 `FetchContent` 拉 **nlohmann/json**（`UVCPP_BUILD_WEBAPP=OFF` 时完全不碰）
- 全程异步 IO：**handler 跑在事件循环线程上**，耗时的活要交给工作线程池（见
  [异步与工作池](#14-异步与工作池)）

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"框架没做"的地方都明确标出来
> ——那些地方比 API 更容易踩。

---

## 目录

1. [最小可运行程序](#1-最小可运行程序)
2. [配置](#2-配置)
3. [路由](#3-路由)
4. [中间件](#4-中间件)
5. [请求](#5-请求)
6. [响应](#6-响应)
7. [静态文件服务](#7-静态文件服务)
8. [上传（multipart 落盘）](#8-上传multipart-落盘)
9. [流式响应：chunked / SSE](#9-流式响应chunked--sse)
10. [WebSocket 服务端](#10-websocket-服务端)
11. [WebSocket 客户端（含自动重连）](#11-websocket-客户端含自动重连)
12. [HTTPS / WSS](#12-https--wss)
13. [HTTP/2](#13-http2)
14. [异步与工作池](#14-异步与工作池)
15. [JSON](#15-json)
16. [日志](#16-日志)
17. [安全](#17-安全)
18. [已知限制](#18-已知限制)

---

## 1. 最小可运行程序

```cpp
// doc-snippet: compile — 本页其余片段是摘录（见文件头上的页面级标记），这一条是
// 自足的完整翻译单元，留着它这条门禁在本页就还有牙。
#include <webapp/uvcpp_web_app.h>
using namespace uvcpp;

int main() {
  uvcpp_web_app app;
  app.set_port(8080)
     .set_max_body_size(8 * 1024 * 1024)
     .use(web_middleware_access_log())
     .use(web_middleware_error_handler(
         [](uvcpp_web_request&, uvcpp_web_response& resp, const std::string& what) {
           resp.status(500).json_str("{\"error\":\"internal\"}");
           resp.end();
         }));

  app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    resp.json_str("{\"hello\":\"world\"}");
    resp.end();
  });

  app.get("/user/:id", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_web_next next) {
    const std::string* id = req.param("id");
    resp.text(id ? *id : "");
    resp.end();
  });

  app.start();   // 起后台线程跑事件循环（bind 有结果才返回）
  app.join();    // 主线程在这儿等
  return 0;
}
```

要点：

- **没有 `listen()`**：地址/端口走配置，`start()` 里做 bind + listen。
- `start()` 阻塞到 bind 有**结果**，所以绑定失败能当场知道返回码；用 `set_port(0)`
  让系统分配，再从 `bound_port()` 取实际端口（写测试常用）。
- `app.start()` 与 `app.start_background()` 是同一个东西（后者是别名）。
- 一个实例只能启动一次；停掉之后再 `start()` 返回 `UV_EBUSY`。
- `~uvcpp_web_app()` 会兜底 `stop()` + `join()`。

---

## 2. 配置

所有 setter 返回 `uvcpp_web_app&`，可链式。出厂默认值（`uvcpp_web_app.cpp`）：

| setter | 默认 | 说明 |
|---|---|---|
| `set_host(const std::string&)` | `"0.0.0.0"` | 含 `:` 按 IPv6 处理 |
| `set_port(int)` | `8080` | `0` = 系统分配，从 `bound_port()` 取 |
| `set_backlog(int)` | `128` | `<= 0` 被夹成 128 |
| `set_max_body_size(size_t)` | 16 MiB | 超过回 **413 且不路由**；`0` = 不限 |
| `set_max_header_bytes(size_t)` | 16 KiB | 整个头块的上限，超过回 **431 且不路由**；**边收边判**，不等头块收完；`0` = 不限 |
| `set_max_url_bytes(size_t)` | 8 KiB | 请求行的目标（URL）上限，超过回 **414 且不路由**；同样边收边判；`0` = 不限 |
| `set_compression(bool)` | `true` | 自动压缩（需 zlib） |
| `set_compress_min_body_size(size_t)` | `1024` | 触发压缩的最小 body |
| `set_access_log(bool)` | `true` | 自动装访问日志中间件 |
| `set_log_level(log_level)` | `INFO` | 全局最低日志等级 |
| `set_server_header(const std::string&)` | `"uvcpp"` | 空 = 不发这个头 |
| `set_shutdown_grace_ms(int)` | `3000` | 优雅关闭宽限；`0` = 立刻强关 |
| `set_idle_timeout_ms(int)` | `60000` | 闲置超时（slowloris 防御）；`0` = 关闭 |
| `set_max_pipelined_requests(size_t)` | `8` | 同一条连接上的在途请求上限；超了回 **503 + close**；`0` = 不限 |
| `set_auto_options(bool)` | `true` | 未命中且路径存在时自动答 OPTIONS |
| `set_head_as_get(bool)` | `true` | HEAD 无注册时回退到 GET handler |
| `set_work_limit(size_t)` | 见 [异步与工作池](#14-异步与工作池) | 工作池在途上限；`0` = 不限 |

**工作线程数没有配置项**：线程池大小是 libuv 的 `UV_THREADPOOL_SIZE`，且**只在进程
启动前设置才生效**（libuv 只读一次，没有运行时扩容 API）。没设时 `start()` 会打一条 WARN。

其他配置族：上传（`set_upload_dir` / `set_upload_fsync` / 六条上限，见 §8）、
TLS（`enable_ssl` 等，见 §12）。

---

## 3. 路由

```cpp
app.get(pattern, handler);
app.post(pattern, handler);
app.put(pattern, handler);
app.del(pattern, handler);      // 注意：是 del，不是 delete
app.patch(pattern, handler);
app.head(pattern, handler);
app.options(pattern, handler);
app.any(pattern, handler);      // 所有方法命中同一 handler
uvcpp_web_router group = app.group("/api");   // app.group("/api").get("/x", h) → /api/x
```

`uvcpp_web_handler` 就是中间件的类型（见 §4）：

```cpp
using uvcpp_web_handler =
    std::function<void(uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next)>;
```

### 模式语法

| 写法 | 含义 | 例 |
|---|---|---|
| `/user/list` | 字面量，逐段精确 | 只匹配 `/user/list` |
| `/user/:id` | 单段参数 | `/user/7` → `req.param("id") == "7"` |
| `/files/*fp` | 通配，吃掉**剩余全部**段 | `/files/a/b` → `req.param("fp") == "a/b"` |

- 引导符是 `:` 与 `*`，**必须位于段首**；段中间出现会被判非法。不支持 `{id}` 语法。
- 通配必须写在**最后一段**，且**至少吃一段**：`/files/*fp` **不**匹配 `/files`。
- 取参数：`const std::string* req.param(name)`（取不到返回 `nullptr`），
  全部参数是 `req.params()`。

### 匹配规则

- **优先级与注册顺序无关**：全序是「静态 > 参数 > 通配」，按逐段特异度比较。
  先注册 `/user/:id` 再注册 `/user/list`，`/user/list` 依然走字面量那条。
- **大小写敏感**，逐字节比较，没有任何归一化。
- **尾斜杠**：`split_path()` 折叠连续 `/` 并忽略结尾 `/`，所以 `/a//b/` 与 `/a/b` 等价。
  `req.path()` 返回的也是折叠后的形态（`//api/me` 读出来就是 `/api/me`，原始串在
  `raw_path()` 里），所以中间件里按前缀写的判据不会被多余的斜杠绕过去。
- **路径参数是解码后的**：`%2F` 会解码成 `/`，于是**充当分隔符** —— `/api/a%2Fb`
  解出来是两段，不是一段里带斜杠。

### 未命中时

| 情况 | 结果 |
|---|---|
| 路径不存在 | **404** |
| 路径存在、方法不对 | **405** + `Allow`（GET 隐含 HEAD；开了 `auto_options` 再加 OPTIONS） |
| OPTIONS 且 `auto_options` | **204** + `Allow` |
| HEAD 未注册且 `head_as_get` | 走同路径 GET，丢掉 body 但保留 `Content-Length` |

**404 / 405 没有专门的注册 API。** 框架内建一条特殊链（全局中间件 + 一个终端
handler）。想接管 404，只能写一个**在终端之前短路**的中间件：

```cpp
app.use([](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next next) {
  resp.on_sent([&req](const uvcpp_web_sent_info& info) {
    if (info.status_code == 404) { /* 统计、上报 */ }
  });
  next();
});
```

（`on_sent` 是响应真正发出后的回调，可以叠加，见 §6。）

### 注册失败的返回值被丢掉了

`app.get(...)` 返回的是 `uvcpp_web_app&`，**不是** bool。模式非法或 handler 为空时
框架会打 `WARN(ROUTER)` 并拒绝注册，但调用方看不到。要检查注册是否成功，走
`app.router().get(...)`：

```cpp
if (!app.router().get("/x", handler)) { /* 没注册上 */ }
```

---

## 4. 中间件

**中间件就是"会调 `next()` 的 handler"**，类型完全相同：

```cpp
using uvcpp_web_next     = std::function<void()>;
using uvcpp_web_handler  = std::function<void(uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next)>;
using uvcpp_web_middleware = uvcpp_web_handler;

app.use(mw);            // 顺序 = 执行顺序，先注册的在最外层
```

**洋葱模型**：链 = 全局中间件（按注册序）+ 路由 handler。`next()` 之前的代码按注册序
执行，`next()` 之后的代码**逆序**执行。

**短路**：不调 `next()` 而直接 `resp.xxx().end()` 就是链的正常终点 —— 框架识别这种
写法，**不会**打"既没调 next 也没结束响应"的 WARN。`body_limit` 与 CORS 预检就靠这个。

**错误传递不是靠 `next()`**：handler 抛异常由框架的 `advance()` 接住 → 走错误处理器。
装了 `web_middleware_error_handler(...)` 就用它，否则记 `ERROR(REQUEST)` 并回 500。

### 内置中间件

| 工厂 | 作用 |
|---|---|
| `web_middleware_access_log()` | 每请求一条 `INFO(REQUEST)`：`GET /user/42 -> 200 (128B, 3ms)` |
| `web_middleware_error_handler(const uvcpp_web_error_handler&)` | 装错误处理器（传空 = 不注册） |
| `web_middleware_cors(const web_cors_options&)` | CORS；预检就地回 204 并终止链 |
| `web_middleware_cors()` | 默认配置（`origin = "*"`，不带凭据） |
| `web_middleware_request_id(header = "X-Request-Id", trust_client = true)` | 分配/校验请求 ID 并写进响应头 |
| `web_middleware_body_limit(size_t max_bytes)` | 超限回 413 并终止链；`0` = 不限 |
| `web_middleware_powered_by(value = "uvcpp")` | 加 `X-Powered-By` |

注意：`access_log` 配置项为真（默认）时，访问日志中间件被 `insert(begin())` 插到
**所有用户 `use()` 之前**。

---

## 5. 请求

`uvcpp_web_request` 不可拷贝，由 context 按值持有。

### 基本信息与报头

```cpp
http_method method() const;              const char* method_name() const;
const std::string& raw_url() const;      // 原始请求目标，含查询串
const std::string& path() const;         // 已解码、已归一化，查询串已剥离
const std::string& raw_path() const;     // 未解码
const std::string& query_string() const; // 原始查询串，未解码
const std::string& peer_ip() const;      unsigned int peer_port() const;

std::string header(const std::string& name, const std::string& def = "") const;  // 大小写不敏感
bool has_header(const std::string& name) const;
const http_headers& headers() const;     const std::string& content_type() const;
size_t content_length() const;           const std::string& host() const;
bool accepts_encoding(const std::string& enc) const;   // 尊重 q 值
bool is_keep_alive() const;
```

### 查询串 / 表单 / Cookie / 路径参数

```cpp
const std::string* query(const std::string& name) const;   // 取不到 → nullptr
const std::vector<std::pair<std::string,std::string>>& query_params() const;  // 保序、允许重复键
bool is_form() const;      const std::string* form(const std::string&) const;
bool is_multipart() const; const std::vector<uvcpp_web_form_file>& files() const;
const uvcpp_web_form_file* file(const std::string& name) const;
bool multipart_ok() const;
const std::string* cookie(const std::string& name) const;
const std::string* param(const std::string& name) const;   // 路径参数
```

- 查询串惰性解析；URL 表单复用同一套解析。
- **路径里的 `+` 是字面加号**（`plus_as_space=false`），只有查询串里 `+` 才是空格。
- 普通路由上的 multipart 是**缓冲式**的：内容在内存里（`uvcpp_web_form_file::data`）。

### body

```cpp
const char* body_data() const;   // 无 body → nullptr
size_t      body_size() const;   // 二进制安全，不是 C 串长
bool        body_empty() const;
std::string body_str() const;    // 复制
```

### JSON

```cpp
bool is_json() const;
bool json(uvcpp_json& out, json_status* status = nullptr) const;
uvcpp_json json() const;         // 失败返回 null
```

### 逃生口

`req.raw()` 返回底层 `uvcpp_http_request&`。

---

## 6. 响应

`uvcpp_web_response` 不可拷贝；所有 mutator 返回 `*this`，可链式。

### 状态码 / 头 / Cookie

```cpp
resp.status(int).status(http_status).status_message("...");   // reason phrase 自动补
resp.set_header(name, value);        // 覆盖同名头
resp.add_header(name, value);        // 追加（Set-Cookie 唯一正确发法）
resp.set_cookie(name, value, path = "/", max_age = -1, http_only = true,
                secure = false, same_site = "Lax");   // 内部走 add_header，可重复调
resp.clear_cookie(name, path = "/");
resp.set_content_type(ct);   resp.get_header(name, def);   resp.remove_header(name);
```

### 发送 body

```cpp
resp.body(const char* data, size_t len, ct = "");   // 深拷贝，覆盖语义
resp.body(const std::string& s, ct = "");
resp.body_move(uvcpp_buf& src, ct = "");            // 接管所有权，不拷贝字节
resp.text(s);   resp.html(s);   resp.json(j);   resp.json_str(s);   resp.binary(p, n, ct);
resp.clear_body();
```

**`body()` 系列是覆盖语义**（每次先清空），不是追加。要多次写用 chunked（§9）。

### 文件下发

```cpp
void send_file(const std::string& path, const std::function<void(int,uint64_t)>& done = {});
void send_file(const std::string& path, int64_t known_size, const std::function<...>& done = {});
void send_file_range(const std::string& path, uint64_t first, uint64_t last,
                     const std::function<...>& done = {});
```

分片读，**不整包进内存**；`done(status, bytes_sent)` 恰好一次。这三条自足——
**不需要**先 `begin_chunked()`，也**不需要**事后补 `end()`。`known_size` 传 `-1` 走 chunked；
`send_file_range` 是**闭区间**。

### chunked / 流式

见 §9。

### `end()` 的语义 —— 这里没有 `send()`

```cpp
void end();          // 标记"我写完了"
bool ended() const;
```

- **`end()` 不是发送动作**，只是标记。框架收尾时才真正把响应发出去。
- 不调不会丢响应，但会打一条 WARN 并按现状发送。
- 一旦 `resp.ended()` 为真，**后面的处理器不再执行**。
- 流式响应上 `end()` 的语义变成"结束这条流"。
- 类里**没有** `send()` 成员，`deferred()` / `set_deferred()` 只是本层一枚**没人读**的
  记录标志；真正决定"框架替不替你发"的是 `uvcpp_http_response::deferred`
  （`src/web/uvcpp_http_response.h:45-49`）。

### elsewhere

```cpp
void on_sent(const uvcpp_web_sent_cb& cb);            // 响应真正发出后回调，可叠加
void set_error_handler(const uvcpp_web_error_handler&);   // 只保留最后一个
uvcpp_http_response& raw();                           // 逃生口（非 const 版会先 sync_meta）
```

`uvcpp_web_sent_info { int status_code; size_t body_bytes; uint64_t connection_id;
bool ok; bool streamed; }` —— `ok == false` 即连接已断/写失败。

常用状态码 helper（语义统一为"设状态码；当前 body 为空时再补一段默认纯文本"）：
`ok()` `created()` `accepted()` `no_content()` `not_modified()` `redirect(url, code = 302)`
`bad_request()` `unauthorized(challenge = "Bearer")` `forbidden()` `not_found()`
`method_not_allowed(allow)` `conflict()` `payload_too_large()` `unsupported_media_type()`
`range_not_satisfiable(total = 0)` `unprocessable()` `too_many_requests()` `server_error()`
`service_unavailable()`。

**keep-alive 没有公开开关**：判定在 HTTP 层（压缩、keep-alive 判定、写队列串行化都在
那里）。只读侧的入口是 `req.is_keep_alive()`。

---

## 7. 静态文件服务

```cpp
std::shared_ptr<uvcpp_web_static> serve_static(
    const std::string& prefix, const std::string& root_dir,
    const uvcpp_web_static_options& opts = uvcpp_web_static_options());
```

```cpp
app.serve_static("/assets", "./public");
```

- 前缀归一化：空 → `/`；补前导 `/`；去尾部 `/`。
- 注册四条路由：`get(p)` / `head(p)` / `get(p + "/*path")` / `head(p + "/*path")`
  （根挂载时是 `/` 与 `/*path`）。单独注册前缀那一版是因为通配**至少吃一段**，
  而 `/assets` 与 `/assets/` 都要能取到索引文件。
- 命中静态路由后**一定给响应（含 404），不会 `next()`** —— 它是该前缀下的兜底。
  想让别的路由优先，就注册在静态之前（静态字面量 > 参数 > 通配的优先级会保证
  `app.get("/assets/special")` 仍然赢过 `/assets/*path`）。
- 通配段从 `req.param("path")` 取；取不到按 `/` 处理（所以挂载前缀本身走索引文件）。
- 文档根在**构造时**解析一次并缓存；解析失败不抛，而是让之后每个请求回 500。

返回的 `shared_ptr` 想用 `clear_cache()` / 读命中统计时留着；不接也活到 App 结束。

### 选项与默认值

| 字段 | 默认 | 说明 |
|---|---|---|
| `index_files` | `{"index.html"}` | 按顺序试 |
| `spa_fallback` / `spa_file` | `false` / `"index.html"` | 路径不存在时回落 |
| `max_file_size` | 64 MiB | 闸门在读盘**之前**，超限一个字节都不读 |
| `etag` | `true` | `"<大小>-<mtime秒>-<mtime纳秒>"` |
| `last_modified` | `true` | `If-Modified-Since` 只在没有 `If-None-Match` 时才用 |
| `range` | `true` | 打开才发 `accept-ranges: bytes` |
| `cache_control` | 空 = 不发 | 框架不替你猜 |
| `cache_max_entries` / `cache_max_bytes` | 256 / 32 MiB | `entries` 为 0 = 关缓存 |
| `max_cached_file_size` | 1 MiB | **`0` = 一律流式**（与别的 setter"0 = 用默认"相反） |
| `dotfiles` | `HIDE`（404） | 见 [§17](#17-安全) |
| `follow_symlinks` | `true` | 关掉时只查**最后一段** |
| `add_charset` | `true` | |
| `mime` | `nullptr` = 默认表 | **不持有所有权** |

### Range / 条件请求

- 只支持**单个区间**。多区间按 RFC 7233 §3.1 合规地**忽略并回 200 全量**。
- `bytes=9999-` 这类**语法对但越界**的必须回 **416**（空文件补 `content-range: bytes */0`）。
- `If-Range` 不匹配时**忽略 Range 回全量**，不是 412。
- 命中 → **206** + `content-range`。
- **HEAD 与 GET 走同一条路**（一样读盘、一样进缓存），`Content-Length` 与
  `Content-Encoding` 因此与 GET 逐字节相同 —— 它们是 HTTP 层压缩决策的结果，
  不给真 body 就算不出来。**旧的"HEAD 不读盘、按文件原始长度报 CL"是错的**：
  开了压缩之后 GET 报的是压缩后的长度，HEAD 报原始长度，拿 HEAD 探长度再按
  长度读满的客户端会一直等到超时。大文件（超 `max_cached_file_size`）两边都
  不读、都不压缩，本来就没有这个问题。
- 超过 `max_cached_file_size` 走 `send_file_range(...)` 分片下发，峰值 ≈ 1.5 MiB
  且**与文件大小无关**。

---

## 8. 上传（multipart 落盘）

```cpp
app.set_upload_dir("./uploads");                       // 目录必须**已存在**
app.upload_route(http_method::HTTP_POST, "/upload/:room", handler);
app.post_upload("/upload", handler);                   // 便利写法
```

`upload_route` 就是 `stream_route` **加上**自动挂上去的 multipart 机制。框架替你做掉的判定：

- 不是 `multipart/form-data` → **415**；是但 `boundary` 缺失/畸形 → **400**。
  两者都发生在**用户 handler 之前**（并带 `connection: close`）。

### 落盘

- 真实路径在**配置期**解析一次，请求期用三重载传下去 —— 每请求零 realpath。
- 落盘名由框架生成：`<upload_dir_real>/<16 位随机十六进制>[.<白名单过滤后的扩展名>]`。
- **客户端给的名字永远不参与落盘**（`original_filename()` 只是清洗过的元数据）。
- 三道防线：`O_EXCL`（flags 里**没有** `O_TRUNC`，撞名重摇）、叶子名校验、最终路径再过
  `web_is_within_root`。
- 结果路径一定是**绝对路径**，可直接 `rename`。

### 上限（全 App 一份，**没有按路由覆盖**）

| setter | 默认 | 超限行为 |
|---|---|---|
| `set_max_upload_size(uint64_t)` | **0 = 不限**（启动时 WARN） | **413 + close**，临时文件全删，用户 `on_end` 不再触发 |
| `set_max_file_size(uint64_t)` | **0 = 不限** | **截断**该部件 + `truncated()` 置位，文件**留着** |
| `set_max_upload_files(size_t)` | 32 | 413 + close，临时文件全删 |
| `set_max_form_fields(size_t)` | 128 | 413 + close，临时文件全删 |
| `set_max_field_size(uint64_t)` | 1 MiB | 413 + close，临时文件全删 |
| `set_max_part_header_bytes(size_t)` | 8 KiB | 400 + close，临时文件全删 |

总长**判在喂之前**（判"喂完会不会超"），越界的字节一个都不落盘。**不能**依赖
`body_limit` 中间件来兜上传（chunked 无条件放行）。

单文件截断与其余五条的差别是刻意的：文件部件流式落盘所以能截断；字段部件攒在内存里，
半截是错的，所以只能拒。

### 拿结果

```cpp
const uvcpp_web_upload_result* req.upload() const;   // 只在 stream()->on_end() 里非空
```

```cpp
app.set_upload_dir("./uploads");
app.post_upload("/upload", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                              uvcpp_web_next next) {
  req.stream()->on_end([&req, &resp]() {
    const uvcpp_web_upload_result* r = req.upload();
    if (r == nullptr) { resp.bad_request().end(); return; }   // 报文畸形/被截断
    uvcpp_json j;
    for (size_t i = 0; i < r->files().size(); ++i) {
      const uvcpp_web_upload_file& f = r->files()[i];
      j["files"].push_back({{"path", f.path()}, {"size", f.size()},
                            {"name", f.original_filename()}});
    }
    resp.json(j);
    resp.end();
  });
});
```

- 失败时 `upload() == nullptr` 且临时文件已删干净；**正常结束后文件归调用方**，框架不再碰。
- 协议错误由**用户**在 `on_end` 里自己回状态码。
- `set_upload_fsync(bool)` 默认**开**；用户的 `on_end` 被**推迟**到 fsync/close 真正跑完之后。

### 进度

**没有 upload 专属的进度回调**。能用的两样：

```cpp
req.stream()->on_progress([](uint64_t received, uint64_t total) { ... });  // 长度未知时 total == 0
req.upload()->received_bytes();   // 交给 sink 的部件 body 字节总数（不含边界与部件头）
```

---

## 9. 流式响应：chunked / SSE

```cpp
void begin_chunked(const std::string& ct = "text/plain; charset=utf-8");
bool write_chunk(const std::string& data);
bool write_chunk(const char* data, size_t len);
void on_drain(const std::function<void()>& cb);
bool streaming() const;
size_t stream_bytes_written() const;    // 不含组帧的 hex 与 CRLF
size_t stream_pending_bytes() const;    // 未落地字节数（背压水位计）
void set_max_stream_buffer_bytes(size_t n);   // 默认 1 MiB
```

SSE 的标准写法：

```cpp
resp.begin_chunked("text/event-stream");
resp.write_chunk("data: 1\n\n");
resp.write_chunk("data: 2\n\n");
resp.end();                 // 流式下 end() = 结束这条流（补终止块）
```

- `begin_chunked` 必须**在任何 body 之前**调一次；它清 body、去 `content-length`、
  设 `transfer-encoding: chunked`。之后 `body()` / `text()` / `json()` 不再有意义。
- **组帧归框架**：`write_chunk("data: 1\n\n")` 在线上的字节是 `b\r\ndata: 1\n\n\r\n`。
- **背压**：`write_chunk` 返回 **false = 缓冲已到高水位**，应等 `on_drain()` 再继续。
  **数据仍然被收下了，false 不是丢弃**。空块是无操作并返回 true。
- **没有公开的 flush 方法**。要"立刻推出去"靠的是 `write_chunk()` 自己 + 水位。
- **写失败怎么知道**：`on_sent(...)` 里看 `uvcpp_web_sent_info::ok` / `streamed`。
  分片下发那条看 `send_file*` 的 `done(status, bytes_sent)`。

### 请求体流（收方向）

```cpp
void on_data(uvcpp_web_stream_data_cb cb);    // std::function<bool(const char*, size_t)>
void on_end(uvcpp_web_stream_end_cb cb);      // std::function<void()>
void on_abort(uvcpp_web_stream_abort_cb cb);  // 对端断开/框架掐断
void on_progress(uvcpp_web_stream_progress_cb cb);
bool pause();   bool resume();   void abort(int status);
uint64_t received() const;   bool has_total() const;   uint64_t total() const;
```

- 注册入口是 `app.stream_route(method, pattern, handler)` / `app.post_stream(pattern, handler)`。
  流式路由里**不要调 `next()`**，框架代管。
- `on_data` 返回 **false = 中止**（等价 `abort(413)`，连接会被关闭）；装 `on_data` 必须在
  `on_end()` **之前**。
- `req.stream()` 对非流式请求返回 `nullptr`；反之 `stream() == nullptr` 时 `body_*` 一定有
  完整内容。两者互斥。流式请求上 `body_size()` **恒为 0**。
- `pause()` **不保证立刻停**（当前块会交付完）。

---

## 10. WebSocket 服务端

```cpp
app.websocket("/chat/:room", [](uvcpp_web_ws_request& ws) {
  const std::string room = ws.param("room") ? *ws.param("room") : "";
  uvcpp_ws_connection* c = ws.connection();
  c->on_text([room](const std::string& msg) { /* broadcast(room, msg); */ });
  c->on_close([room](ws_close_code code, const std::string&) { /* leave(room, code); */ });
});
```

```cpp
using uvcpp_web_ws_handler = std::function<void(uvcpp_web_ws_request&)>;

uvcpp_web_app& websocket(const std::string& pattern, const uvcpp_web_ws_handler&);  // 隐式 enable_wss()
uvcpp_web_app& enable_wss();                 // 幂等
uvcpp_ws_server* ws_server();                // 配置逃生口
bool wss_enabled() const;
size_t ws_session_count() const;             // 实时值
size_t ws_recycled_session_count() const;    // 单调递增
```

- 模式语法与 HTTP 路由**完全相同**，但**两张表独立**（`ws_router_` / `ws_handlers_`）。
- 注册必须在 `start()` **之前**。
- **升级请求没命中 WS 路由时回落成普通 HTTP 请求** —— 不会哑掉，也不会因为注册过 WS
  就让同路径的 HTTP 路由失效。
- 缺 `Sec-WebSocket-Key` 时框架自己回 **400**（否则连接会哑掉）。
- **TLS 不需要额外工作**：`enable_ssl()` 一旦生效，WS 自动就是 WSS。

### 升级请求对象（只在本次调用期间有效）

```cpp
uvcpp_ws_connection* connection() const;     // 归框架所有，不要 delete
const std::string& route() const;            // 命中的模式，如 "/chat/:room"
uvcpp_web_request& request();                // 内层请求（逃生口）
const std::string* param(const std::string&) const;
const std::string* query(const std::string&) const;
const std::string* cookie(const std::string&) const;
std::string header(const std::string&, const std::string& def = "") const;
const std::string& peer_ip() const;   unsigned int peer_port() const;
```

对象归框架所有，handler 返回后销毁。会话是**异步**建起来的（在 101 写完成的回调里
`bind_connection` 之后才调你的 handler），所以 `connection()` 在 handler 里一定非空。

### 连接对象

```cpp
int send_text(const char* data, size_t len, std::function<void(int)> cb = nullptr);
int send_binary(const char* data, size_t len, std::function<void(int)> cb = nullptr);
int send_ping(const char* data = nullptr, size_t len = 0);
int send_pong(const char* data = nullptr, size_t len = 0);
int send_close(ws_close_code code = NORMAL, ...);
void close(ws_close_code code = NORMAL, ...);
void on_text(std::function<void(const std::string&)> cb);
void on_binary(std::function<void(const uint8_t*, size_t)> cb);
void on_ping(...);   void on_pong(...);
void on_close(std::function<void(ws_close_code, const std::string&)> cb);
void set_max_message_size(size_t n);
void enable_compression(bool is_server, const uvcpp_ws_deflate_params& p);
void set_compress_min_size(size_t n);
```

`on_text` / `on_binary` **每条完整消息调用一次**（不是每帧）。

### 会话管理与广播

会话容器是 `uvcpp_ws_sessions`（在 **`src/web/`**，不在 `src/webapp/`）：

```cpp
void adopt(uvcpp_ws_connection* c);              // 接管所有权 + 装终结回调
void set_retire_observer(std::function<void(uvcpp_ws_connection*)>);
void close_all(ws_close_code code = NORMAL);
void recycle_all();                              // 只删会话，句柄由析构在循环停掉后处理
void shutdown();   size_t size() const;   size_t pending() const;   size_t recycled() const;
const std::vector<uvcpp_ws_connection*>& all() const;
```

服务端层面的收口是 `uvcpp_ws_server::close_all_sessions(...)`。App 层的观测口径：
`ws_session_count()` 是**实时**值（可能是"已开始关闭但还没回收"），等它归零要用带上限的
重试循环，配 `ws_recycled_session_count()` 才闭环。

> ⚠️ **框架里没有 `broadcast()` 这个 API。** `uvcpp_web_app.h` 里那段示例注释出现的
> `broadcast(room, msg)` 是**用户自备**的函数。要做房间广播得自己维护
> `room → 会话` 的表，然后遍历发 `send_text`。

### 闲置超时的例外

升级成功后那条连接**不再受 `set_idle_timeout_ms` 约束**。代价是 WS 连接从此**没有任何
超时保护** —— 需要保活就自己在会话上发 ping/pong。

---

## 11. WebSocket 客户端（含自动重连）

服务端在框架层是 `app.websocket(...)`；客户端这一侧是 `uvcpp_web_ws_client`。

它比协议层的 `uvcpp_ws_client` 多三样：

1. **回调装在客户端上**（`on_open` / `on_text` / `on_binary` / `on_close` / `on_error`）：
   `connect()` 之前装、之后装都行，**重连之后照旧生效**。
2. **可选自动重连**（`set_reconnect()`）。默认**关**。
3. `wss://` 的 TLS 上下文透传（`set_ssl_context`）。

```cpp
#include <webapp/uvcpp_web_ws_client.h>

uvcpp_web_ws_client cli;
cli.on_open ([](uvcpp_ws_connection* c) { /* 刚连上 */ })
   .on_text ([](const std::string& m) { std::cout << m << std::endl; })
   .on_close([](ws_close_code code, const std::string& reason) { /* 对端怎么走的 */ })
   .on_error([](int err, const std::string& what) { /* 握手失败 / 协议错 / 重连耗尽 */ });

uvcpp_web_ws_reconnect rc;   // enabled=false 时其余字段被忽略
rc.enabled      = true;
rc.delay_ms     = 1000;      // 第一次重连前等多久
rc.backoff      = true;      // 每失败一次翻倍，直到 max_delay_ms
rc.max_delay_ms = 30000;
rc.max_attempts = 0;         // 0 = 不限次数

cli.set_reconnect(rc);
cli.connect("ws://127.0.0.1:8080/echo");   // 也可以 wss://
cli.run();                                  // 生产写法：循环在"没有活句柄"时自然返回
```

### 连接

```cpp
int connect(const std::string& url, std::function<void(int)> cb = nullptr);
int connect_wait(const std::string& url, int timeout_ms = 30000);
```

- `cb(0)` = 握手完成、`session()` 可用；非 0 是失败码。**可以不传** ——
  `on_open` / `on_error` 一样能拿到结果。
- `cb` **最多跑一次**，报的是**这一次** `connect()` 的结果；后续每次重连尝试的结果走
  `on_open` / `on_error`。连接建立中被 `close()` 取消时收到 `UV_ECANCELED`。
- URL 会被存下来给重连用（`url()`）。开着重连时**不要**自己再 `connect()` 插队。
- `connect_wait` **只给脚本与测试用**：它在本线程里 `run(UV_RUN_NOWAIT)` + 1ms 睡眠轮询，
  与"不在循环线程上跑耗时操作"相抵触。生产路径请用 `connect()` + `run()`。

### 回调语义

| 回调 | 什么时候 |
|---|---|
| `on_open(uvcpp_ws_connection*)` | 握手完成。重连成功会**再触发一次**，每次都是**新指针**（底层客户端整个换掉了）——不要缓存上一次的指针去比较 |
| `on_text(const std::string&)` | 收到一条文本消息 |
| `on_binary(const uint8_t*, size_t)` | 收到一条二进制消息；`data` 只在回调期间有效 |
| `on_close(ws_close_code, const std::string&)` | **对端**结束了这条连接（对端 Close 帧、或没发帧就断开 = 1006）。本端自己发起的结束（`close()`、协议错误）**不走这里** |
| `on_error(int, const std::string&)` | 握手/连接失败、会话上的协议错误、重连次数用尽。**不含**对端的正常关闭 |
| `on_reconnect(int attempt, int delay_ms)` | 每次重连**排定**时（还没连上）。可以在这里 `set_reconnect()` 改策略 —— 改的是**下一次**的等待 |

`on_close` 的触发顺序是**先**回调使用者、**再**决定要不要重连 —— 所以在这个回调里调
`close()` 就是"这一条到此为止，别再重连了"。

`on_error` 的 error 取值域可以一张表判完（三块不撞车）：

| 范围 | 含义 |
|---|---|
| `< 0` | libuv / 协议层错误（`UV_ECONNREFUSED`、`UV_ECANCELED`……） |
| `>= 1000` | 对端或本端协议错误带来的 WS 关闭码（1002 协议错误、1009 过大……） |
| `1..999` | 本层错误：目前只有 `WEB_WS_ERR_RECONNECT_EXHAUSTED = 1`（重连次数用尽） |

### 发送与关闭

```cpp
int send_text(const char* data, size_t len, std::function<void(int)> cb = nullptr);
int send_text(const std::string& text, std::function<void(int)> cb = nullptr);
int send_binary(const char* data, size_t len, std::function<void(int)> cb = nullptr);
void close(ws_close_code code = NORMAL, const std::string& reason = "");
void stop();
```

- 没有会话时发送返回 `UV_ENOTCONN` 并**立刻**回调 `cb(UV_ENOTCONN)` —— 不静默丢，
  与协议层一致："发出去了"这件事只能由回调确认。
- **`close()` 关的是连接，`stop()` 停的是循环**：
  - `close()` 主动关闭当前连接，**并顺手取消重连**（"我不想要它了"和"连接掉了"必须分开，
    否则使用者永远关不掉一个开着重连的客户端）。有会话时发 Close 帧、等真正的终结；
    还在连接建立中时等于**取消**这次连接，等 `connect()` 回调的人会当场收到 `UV_ECANCELED`。
  - `stop()` 只请求停止 `run()`，**不关连接、不发 Close 帧、不动重连状态**。
- 本层**没有** `send_ping` / `send_pong` 转发方法。要 ping/pong/压缩/单条消息上限，
  用 `on_open` 拿到的 `uvcpp_ws_connection*` 上协议层那套。

### 状态与循环

```cpp
bool is_open() const;                 // = session() != nullptr
int  get_status() const;   int get_last_error() const;
int  reconnect_attempts() const;      // 本轮已排定的重连次数，连上即清零；用尽后不再增长
const std::string& url() const;
uvcpp_ws_connection* session() const; // 没连上或已终结时 nullptr
int  run(uv_run_mode md = UV_RUN_DEFAULT);
uvcpp_loop* get_loop();               // 换客户端之后是**另一个**循环
void set_ssl_context(uvcpp_ssl_context* ctx);   // 仅 UVCPP_OPENSSL_ENABLE
```

**`run(UV_RUN_DEFAULT)` 不是简单转发给底层**：本层按"一次一轮"驱动，好让**换底层客户端**
这个动作落在两次循环之间（换客户端会连带删掉正在跑的那个循环，落在回调里就是 `uv_run`
重入）。在没有活句柄时自然返回，`stop()` 也会让它返回。

**必须自己起线程**：本类**非线程安全**，全部调用都在跑 `run()` 的那个线程上；`run()` 是
阻塞的。框架不替你起线程。

### 两条硬约束

- **不要在回调里析构本对象**：析构会连带删掉底层客户端与它正在跑的事件循环（`uv_run`
  不可重入）。与协议层"不要在回调里 delete 会话"同一类约束。
- **不要在回调里调 `connect()`**：底层循环正在跑，换不得。本层会把它推迟到当前这一轮
  循环返回之后，所以不会崩，但那一次调用只是**登记**，不是立即发起。

### 为什么是"每次连接尝试换一个客户端"

协议层的 `uvcpp_ws_client` **不承诺可重连**：它内部那个 `uvcpp_tcp_client` 的句柄关掉
之后 `uvcpp_tcp` 会把底层指针置空，且 `has_async_connect_cb_` 在明文成功路径上从不清除
（`src/net/uvcpp_tcp_client.cpp:623-627`）——**同一个协议层客户端对象根本连不了第二次**。

所以本层的做法是：每次连接尝试都换一个协议层客户端，新客户端带一个新的事件循环，在
`connect()` 之前把本层存下的回调重新装一遍。对使用者不可见。重连定时器挂在 `inner_` 的
循环上，随 `inner_` 一起换（顺序要紧：先关旧定时器再删旧客户端，否则
`uv_loop_close` 会因未关的定时器返回 `UV_EBUSY`）。

---

## 12. HTTPS / WSS

```cpp
#if UVCPP_OPENSSL_ENABLE
uvcpp_web_app& enable_ssl(const std::string& cert_file, const std::string& key_file);
uvcpp_web_app& enable_ssl_pem(const std::string& cert_pem, const std::string& key_pem);
uvcpp_web_app& enable_self_signed(const std::string& host = "localhost", int bits = 2048);
bool ssl_enabled() const;
uvcpp_ssl_context* ssl_context() const;
const std::string& ssl_error() const;
#endif
```

```cpp
app.enable_ssl("cert.pem", "key.pem").set_port(8443);
app.websocket("/chat", handler);          // 自动就是 WSS
app.start();
```

- `enable_ssl(...)` **在调用点**即读入并校验（含证书/私钥配对检查）。
- 配置失败时 `start()` 返回 `UV_EINVAL`，**不会静默降级成明文**；原因在 `ssl_error()`。
- TLS 在传输层，所以 WSS 免费得到。
- 客户端侧：`cli.set_ssl_context(&ctx)`。与协议层不同，这里**可以在 `connect()` 之后再设**
  （本层存着，每次重建底层客户端时重新装上）。TLS 上下文的**生命周期要覆盖整条连接**，
  本层**不持有**。

---

## 13. HTTP/2

**默认就开着，而且零配置。** 上面那段 `enable_ssl()` 的例子里的服务端，浏览器用
HTTP/2 来访问时走的就是 h2 —— 你一行都不用改。这一节只讲**怎么关**、以及关掉之后
还剩什么。

### 唯一的一个开关

```cpp
uvcpp_web_app& set_http2_enabled(bool on);   // 默认 true
bool http2_enabled() const;                  // 读的是**实际**能不能：内部 && UVCPP_NGHTTP2_ENABLE
```

协议协商在 TLS 握手内完成（ALPN），发生在任何 HTTP 字节被解析之前，所以框架这边的
分流点只有一个：`uvcpp_tcp_server` 把连接交上来时读一次 `tls_alpn_selected()`。
协商出 `h2` 就走 h2 会话，否则走 HTTP/1.1 —— 两条路共用**同一套**路由、中间件、
静态文件、上传与流式响应。

关掉它（`set_http2_enabled(false)`）的效果是服务端的 ALPN 名单里不再有 `h2`，
客户端因此协商回 `http/1.1`。**降级是能用的**，不是"协商结果不同"而已。
`UVCPP_ENABLE_NGHTTP2=OFF` 编出来的库 `http2_enabled()` 恒为 `false`，行为与关掉一致。

### 低层那两层：手动、默认关

框架的"自动"是**建在**低层之上的，低层自己一点都不自动。用 `uvcpp_http_server` /
`uvcpp_http_client` 时要**自己说**要哪个协议，而且**每一项都得说全** —— 没有哪一层
替你把另一层补上。

**服务端**：两个开关都要，缺一个都协商不出 h2。注意 TLS 不在 `uvcpp_http_server`
上，得挂到它下面的 `uvcpp_tcp_server`，**而且必须在 `listen()` 之前**。

```cpp
uvcpp_ssl_context ctx(tls_mode::SERVER, tls_version::TLS_1_2);
ctx.load_certificate_file("server.crt");           // 证书/私钥，同 HTTPS 那套
ctx.load_private_key_file("server.key");
// ① ALPN 名单：不设就没有可协商的东西，服务端会按"没谈 ALPN"回，连接落到 h1
ctx.set_alpn_select_protos({"h2", "http/1.1"});    // 顺序即优先级

uvcpp_http_server srv;
srv.get_tcp_server()->set_ssl_context(&ctx);       // 必须在 listen() 之前
srv.set_http2_enabled(true);                       // ② 协议层开关，默认 false
```

那个上下文的生命周期要覆盖整个服务端（`set_ssl_context` **不接管所有权**）。

服务端的选择回调**不匹配时返回 `NOACK` 而不是 fatal**，所以不带 ALPN 的
HTTP/1.1 客户端照常握手成功 —— 这就是"关掉 h2 之后降级是能用的"的机制。

**客户端**：只设协议层，ALPN 名单由 `connect()` 按开关现拼。

```cpp
uvcpp_ssl_context cctx(tls_mode::CLIENT, tls_version::TLS_1_2);

uvcpp_http_client cli;
cli.set_ssl_context(&cctx);     // 这个上下文上**不用**设 ALPN —— 见下
cli.set_http2_enabled(true);
cli.connect("host", 443, [&](int err) {
  // 握手内谈完，这里已是终局；不需要嗅字节，也不需要等
  printf("%s\n", cli.negotiated_alpn().c_str());   // "h2" / "http/1.1" / ""
  // cli.send(req, cb)  —— 现在按实际协商结果走 h2 或 h1
});
```

- `set_http2_enabled(false)`（默认）钉 `{"http/1.1"}`；`true` 发 `{"h2","http/1.1"}`。
  这一对是**每条连接**现拼的（`connect()` 里算、装到那条连接的 `SSL*` 上），
  所以上下文上设的客户端 ALPN 会被它盖掉 —— 客户端这边只有协议层这一个旋钮。
- **协商不出 h2 不是错误** —— 名字叫"开启"不是"强制"，服务端不认就照走 h1。
  要判到底走了哪条，读 `negotiated_alpn()`。
- **h2 连接上 `send_wait()` 返回 `UV_ENOTSUP`**。理由与既有的"异步 `connect()` 配
  `send_wait()`"是同一条：阻塞式 `SSL_read/SSL_write` 要独占 socket，接管不了 h2
  那条内存 BIO 会话。宁可报错，也不把 HTTP/1.1 明文写进一条对端按二进制帧解析的
  连接。**h2 只支持异步系列**（`connect()` + `send()`）。
- **h2 上没有单槽状态**：一条连接上并发几条流时，应答**按 `resp.stream_id` 归位**
  （h1 那条路上它恒为 0）。回调是逐条的，`stream_id` 是你唯一的"这条回应的是哪次
  请求"的凭据。
- 必须配 TLS。明文上没有 ALPN，本库也不做 h2c，所以没设 SSL 上下文时打开它只是
  **记录意图**，连接照样是 HTTP/1.1。

### h2 上原来就成立的

路由（含路径参数）、中间件、`next()`、静态服务（含 Range/ETag/304）、multipart 上传、
chunked/SSE 流式响应 —— 全部照常，API 一个都没变。几处值得知道的差别：

- **响应顺序**：HTTP/1.1 的流水线要求响应按请求到达顺序发出（RFC 7230 §6.3.2），
  所以框架有一道"只有队首能发"的闸门。**h2 恰恰相反** —— 响应可以乱序，一道慢流
  不会扣住同一连接上的其他流。闸门在 h2 上被绕过。
- **并发**：h1 的 `set_max_pipelined_requests()` 是框架自定的上限；h2 换成协议内建的
  `SETTINGS_MAX_CONCURRENT_STREAMS`。
- **一次上传一条流**：上传结果按 (连接, 流) 二元组归位，同一条 h2 连接上的并发上传互不干扰。

### 这一版写死的几个值

h2 会话的参数在 `uvcpp_h2_session::init()` 上是带默认值的形参，**webapp 这一层没有
把它们开放成配置项** —— 所以下面是"实际生效的值"，不是"可调项"。要改得从
`src/http2/` 那一层传。

| 项 | 值 | 说明 |
|---|---|---|
| `SETTINGS_MAX_CONCURRENT_STREAMS` | 100 | 我们宣告给对端的并发上限（`H2_DEFAULT_MAX_CONCURRENT_STREAMS`） |
| `SETTINGS_MAX_HEADER_LIST_SIZE` | 64 KiB | **只是宣告值**，接收侧的强制靠自己的累加（见下） |
| `SETTINGS_INITIAL_WINDOW_SIZE` | 65535 | **靠不宣告拿到**：默认参数是 0，这一项就不发，对端按 RFC 9113 的初值 65535 走 |
| 头部列表预算 | 64 KiB | 逐字段累加 `namelen+valuelen+32`，越界即断（`h2_header_budget`） |
| 单流 body 上限 | 64 MiB | `H2_DEFAULT_MAX_BODY_BYTES`，与 h1 的 body 上限同量级；**两个方向都算**（服务端收到的请求体、客户端收到的响应体走的是同一个 `on_data_chunk`），越界即 `RST_STREAM(ENHANCE_YOUR_CALM)` |

> **上面那格"头部列表预算"和 h1 的 `set_max_header_bytes` 不是一回事**，
> 别当成同一个旋钮的两处配置。h1 数的是**线上字节**（字段名、`": "`、CRLF 全都算），
> h2 数的是 **HPACK 解出来的**字节（`namelen+valuelen+32`，RFC 9113 §6.5.2 的定义）
> —— 同一个数字在两边含义不同，而"同一个名字两处含义"正是这一版在修的那类 bug，
> 所以**没有**把 webapp 的 `set_max_header_bytes` 接到 h2 上。h2 侧本来就有一道
> 64 KiB 的预算，缺的是"把它开放成配置项"，不是"没有上限"。

### h2 上还差一口气的（如实列出）

- **上传背压仍是连接级的。** `stream()->pause()` / `resume()` 落在
  `uvcpp_tcp_client::read_pause()` 上，也就是**整条连接**。h2 一条连接上有好几条流，
  所以一条流的落盘跟不上时，同连接上其他流会跟着一起停一下 —— 等它写完 `resume()`
  就恢复（`context_finished()` 是无条件收尾的兜底），**不会永久卡死**，但那一小段
  延迟是真实的、会串到邻居身上。
  正确的做法是用 h2 自己的流控（`NGHTTP2_OPT_NO_AUTO_WINDOW_UPDATE` +
  `nghttp2_session_consume_connection` / `consume_stream`，只推迟那条流的窗口），
  **这一版没做**。它是个全局开关：漏掉任何一条消费路径都会让上传在 64 KiB 处
  永久停住，所以要么整套做对、要么先不动 —— 半套比现状更危险。

### 非目标（如实列出）

- **不做 h2c / 明文 h2**，两个方向都不做：
  - `Upgrade: h2c` 这条**已经被移除**了 —— 它曾经在 RFC 7540 §3.2，RFC 9113 §3.1
    明确删掉了它。不是"我们懒得做"，是协议里没有了。
  - prior-knowledge（直接按 h2 发而不协商）也不做：本库没有明文嗅探，一个不认识的
    明文连接按 HTTP/1.1 处理。
  所以**h2 只在 TLS 上**。明文端口永远是 HTTP/1.1。
- **不做 RFC 8441**（WebSocket over h2）：不宣告 `SETTINGS_ENABLE_CONNECT_PROTOCOL`，
  收到 `:protocol` 一律拒。`app.websocket()` 继续走 HTTP/1.1 的 `Upgrade`。
- **不做 SERVER_PUSH**（RFC 9113 已废弃它）：我们宣告 `SETTINGS_ENABLE_PUSH=0` 且从不发
  `PUSH_PROMISE`。
- **不做 HTTP/3**。
- 不实现优先级树的完整语义（`PRIORITY` 帧照收，只是不据此调度）。

### 安全边界

| 面 | 对策 |
|---|---|
| 伪头畸形 / 顺序错 / 重复 | `RST_STREAM(PROTOCOL_ERROR)`（RFC 9113 §8.1.2） |
| `:authority` 与 `host` 冲突 | RST —— 请求走私/缓存投毒的经典入口 |
| `:scheme` 不是 `https` | RST |
| 连接专属头（`connection` / `keep-alive` / `transfer-encoding` / `upgrade` / `proxy-connection`） | 收到即 RST；发出时一律剥掉 |
| `content-length` 与 DATA 实长不符 | RST（h2→h1 走私的手法） |
| 头部膨胀 / HPACK bomb | 逐字段累加 `namelen+valuelen+32` 封顶 64 KiB —— **这是唯一防线**，`SETTINGS_MAX_HEADER_LIST_SIZE` 只是宣告出去的值，nghttp2 的接收路径**不做任何强制**（对端可以不遵守，bomb 正是不遵守的那种对端） |
| CONTINUATION 洪泛 | nghttp2 内建的 8 帧上限（越界返负值，按致命处理）+ 上面的头部预算 |
| SETTINGS/PING/RST 帧风暴 | 令牌桶（突发 64、32/秒补充），越界 → `GOAWAY(ENHANCE_YOUR_CALM)` |
| 请求体 cap | **按流**记，不是按连接 —— 连接级一个标量在并发流下会互相记成溢出 |
| 静态目录穿越 | 与 h1 同一套防护；h2 上 `:path` 由对端原样送来（没有 llhttp 那层请求行解析），用例**单独实测**过 |

一条 h2 连接上的响应**没有** HTTP/1.1 那种 reason phrase，也不该有 `transfer-encoding`；
`content-length` 在 h2 里只是参考值（边界是 END_STREAM）。

---

## 14. 异步与工作池

**handler 跑在事件循环线程上**，任何耗时的活（文件 IO、DNS、CPU 密集）都必须交给工作
线程池，否则整个服务卡住。

### 用法：留一个 `next` 的副本，在 work 完成回调里续跑

```cpp
app.get("/slow", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                    uvcpp_web_next next) {
  uvcpp_loop* loop = /* 从 app 拿，见下 */;
  uvcpp_work* w = new uvcpp_work();
  w->queue_work(loop,
                [/* 工作线程体：不要碰 req/resp */]() { /* 阻塞的活 */ },
                [next, &resp, w](uvcpp_work* /*self*/, int status) {
                  resp.text(status == 0 ? "done" : "failed");
                  resp.end();
                  next();        // 续跑链（其实 resp.end() 之后链已经到底）
                  delete w;
                });
});
```

- **不要把 `req` / `resp` 的引用带进工作线程体**：它们归 context 所有，只在本次调用
  期间有效。要在回调里用，就把需要的东西**拷出来**。
- `app.loop()` 拿底层事件循环；`uvcpp_web_context::loop()` 也有。
- 不续跑链、但要保住上下文的场合，用 `ctx.hold()` / `ctx.release()`（配对），或者
  `ctx.post(fn)`（已在 loop 线程上会**就地同步执行**）。

### 工作池闸门 `uvcpp_web_work_limit`

```cpp
app.set_work_limit(size_t limit);                 // 0 = 不限
std::shared_ptr<uvcpp_web_work_limit> app.work_limit() const;   // 恒非空
```

- **默认并发数** = `UV_THREADPOOL_SIZE * 4`，下限 16。
- **超限时的行为：既不排队也不拒绝** —— 它只回答"有没有名额"（`acquire()` 返回 bool），
  怎么处理由调用方按自己的退路决定：
  - **静态文件**：`acquire()` 拿不到就回 **503 + `Retry-After`**。取名额发生在 worker 里
    **真正要读盘的那一步之前**，所以不读盘的分支（LRU 命中 / 404 / 403）**不占名额**、
    也不会被回绝；反过来说超限的请求**已经进了线程池**了，这道闸门不是投递前的准入
    （被回绝的那些只做了一次 `stat`，代价与卡住线程池队列的那点排队是两回事）。
  - **上传**：`pause()` 把压力退回给对端，并向 `work_limit` 注册唤醒（`add_wakeup()`）；
    **那个唤醒是必需的**，否则永久卡死。
- 唤醒回调**在 `release()` 的调用栈上被同步调用**（loop 线程），里面应重试 `acquire()`，
  拿不到就**重新注册**（注册是一次性的）。
- 记账纪律：`release()` 必须与成功的 `acquire()` **一一配对**。漏一次 = 名额永久泄漏，
  最终表现为服务"无缘无故"开始回 503。

---

## 15. JSON

后端是 **nlohmann/json**（`uvcpp_json` 就是 `nlohmann::json` 的别名），集中在
`uvcpp_web_json.h` 一个头里。

```cpp
enum class json_status : int { OK = 0, EMPTY, SYNTAX, TOO_DEEP, TOO_LARGE };

json_status uvcpp_json_parse(const char* data, size_t len, uvcpp_json& out);
json_status uvcpp_json_parse(const char* data, size_t len, uvcpp_json& out,
                             const json_parse_options& opts);   // max_depth = 64, max_bytes = 8 MiB
size_t      uvcpp_json_max_nesting(const char* data, size_t len);
std::string uvcpp_json_dump(const uvcpp_json& j);
std::string uvcpp_json_dump(const uvcpp_json& j, int indent);
std::string uvcpp_json_get_string(const uvcpp_json& j, const char* key, const std::string& def);
bool        uvcpp_json_get_bool (const uvcpp_json& j, const char* key, bool def);
bool        uvcpp_json_get_int  (const uvcpp_json& j, const char* key, long long& out);
bool        uvcpp_json_get_double(const uvcpp_json& j, const char* key, double& out);
```

### 五个坑（都在 `uvcpp_web_json.h` 里写明了）

1. **不允许异常穿透 libuv 回调**：nlohmann 的 `parse()` 默认抛，而调用点全在 libuv 回调里，
   异常穿过 C 写的 `uv_run` 栈帧是 UB（MSVC 直接 terminate）。所以这里**不抛异常**，
   用 `json_status`。`uvcpp_json_dump` 也内部吞异常，**失败返回空串**。
2. **深度预扫描防爆栈**：nlohmann 是递归下降且**没有内建深度上限**，`[[[[[...` 能爆栈
   （真实 DoS 手法）。`uvcpp_json_max_nesting` **跳过字符串字面量**，所以
   `{"a":"[[[[[["}` 深度是 1 不是 7。
3. **取值别用 `j["k"]`**：非 const 对象上遇到缺失键会**插入一个 null**，const 对象上会**抛**。
   用上面那组"查不到给默认值、类型不对也给默认值"的 getter。注意 `uvcpp_json_get_int`
   是**返回 bool + 出参**，浮点按截断（`3.9` → `3`）。
4. **空输入返回 `EMPTY` 而不是 `SYNTAX`** —— "没有 body"和"body 不是 JSON"通常要回不同
   状态（400 vs 415）。失败时 `out` **不被改动**。
5. **跨 DLL**：nlohmann 是 header-only，`uvcpp_json` 对象会跨 DLL 传，使用者的 TU 必须用
   **同一版本、同一组配置宏**。

---

## 16. 日志

两级结构：等级 `log_level`（多严重）+ 模块 `log_category`（来自哪个功能模块）。
"模块决定 category，事件性质决定 level"。

```cpp
// 等级（数值越小越详细）
enum class log_level { TRACE, DEBUG, INFO, WARN, ERR, FATAL, OFF };
// 模块
enum class log_category { CORE, HTTP, REQUEST, RESPONSE, HEADER, BODY, ROUTER,
                          STATIC, WEBSOCKET, SSL, UPLOAD, DOWNLOAD, IO, RAW, JSON, ... };
```

> ⚠️ **是 `ERR` 不是 `ERROR`**：`<wingdi.h>`（经 `<windows.h>` → `<uv.h>`）里有
> `#define ERROR 0`，宏展开不看 `enum class` 作用域。对外展示用
> `uvcpp_log_level_name()`，返回的仍然是 `"ERROR"`。

用法是**流式宏**，不是 printf 变参：

```cpp
UVCPP_LOG_INFO(log_category::ROUTER) << "matched " << path << " -> " << name;
```

宏：`UVCPP_LOG_TRACE` / `UVCPP_LOG_DEBUG` / `UVCPP_LOG_INFO` / `UVCPP_LOG_WARN` /
`UVCPP_LOG_ERROR` / `UVCPP_LOG_FATAL`（都带 category 参数）。被过滤时**零开销** ——
连流对象都不构造、消息都不拼装。

printf 风格逃生口 `uvcpp_logf(level, category, fmt, ...)`：**即使等级被过滤，参数也已经
被求值**，高频路径别用。

配置（`uvcpp_logger` 单例，默认全局等级 `INFO`，默认 sink 是内置控制台）：

```cpp
uvcpp_logger::instance().set_level(log_level::DEBUG);
uvcpp_logger::instance().set_level(log_category::WEBSOCKET, log_level::TRACE);
uvcpp_logger::instance().set_all_category_levels(log_level::WARN);
uvcpp_logger::instance().clear_category_overrides();
uvcpp_logger::instance().set_sink(my_sink);      // nullptr = 恢复内置控制台
```

自定义 sink：实现 `uvcpp_log_sink`（`virtual void write(const uvcpp_log_record&) = 0;`），
**实现必须自己保证线程安全**（`write()` 可能从工作线程池线程调用）。`set_sink`
**不接管所有权**。sink **不得长期持有 `uvcpp_log_record` 的引用**（只在下游 `write()`
调用期间有效）。

---

## 17. 安全

框架里**确实做了**的（都是代码可核的）：

### 路径

- **先解码、再逐字节检查**（`web_sanitize_path`）：`%2e%2e` 解码成 `..` 之后才判，
  否则 `%00` / `%5c` / `%3a` 全都能溜过去。检查项：NUL、控制字符、`\`（Windows 上
  `..\..\` 靠它绕过）、`:`（盘符 `C:`、NTFS 数据流 `file.txt:evil`）。
- 拒绝非绝对路径、拒绝前导 `//`（UNC）。
- **`..` 越根判 `TRAVERSAL`，明确不做静默截断** —— 截断会把一次攻击变成一条正常响应。
- 段数上限（默认 64）、长度上限（默认 4096）。
- **静态路由里的调用点是 `decode=false`**：`req.path()` 已经解过一次，再解一次会把文件名
  里字面的 `%2e` 变成 `.`、`a%2fb` 变成目录分隔符 —— **凭空造出一个新的路径**。
  归一化失败 → **404**（不是 403）。
- **符号链接**：靠 `web_resolve_within_root()` 判"解析后的真实路径是否还在根内"，
  包含判断是"前缀 + 分隔符"，所以 `/srv/webroot2` 不会被误判为在 `/srv/webroot` 内。
  `follow_symlinks = false` 时只用 `lstat` 查**最后一段** —— 这条**不是**安全边界，
  中间段指向根外靠真实路径包含判断兜。
- **dotfile 策略**在投递线程池任务**之前**生效：`DENY` → 403，`HIDE` → **404**，
  `ALLOW` → 照常。404 而不是 403 是刻意的（403 等于确认文件存在，对 `.env` 探测就已经
  泄漏信息）。只按**段首**判断，所以 `lib..min.js` 不算 dotfile、`a/.b/c` 算。
- **SPA 回退只对"路径不存在"生效**：被安全策略拒绝（穿越 / dotfile）时**不回落**，
  否则一次探测会变成 200。
- 索引文件名与 SPA 入口名都过一遍合法性校验。
- **上传目录不得落在静态根内**：三处检查（`set_upload_dir()` / `serve_static()` / `start()`）。

### 请求

- **请求体上限** `set_max_body_size`：超过回 413 且**不路由**。
- **请求头上限** `set_max_header_bytes`（默认 16 KiB）：整个头块超了回 431；
  **请求行上限** `set_max_url_bytes`（默认 8 KiB）：超了回 414。两条都**边收边判** ——
  在 llhttp 的头回调里就累加、超了当场停，不是等头块收完再看，所以一个
  "声明了很长、但一直不结束"的头不会让连接把内存吃满再被拒。
  这是 `set_idle_timeout_ms` 之外的另一道 slowloris 防线：闲置超时管的是**慢**，
  这两条管的是**大**。
- **这两个上限只管 h1**。h2 的头部预算是另一套（逐字段累加 HPACK **解码后**的
  `namelen+valuelen+32`，见 [§13](#13-http2)）—— 同一个数字在两边含义不同，
  所以没有把 webapp 的这两个旋钮接到 h2 上。
- **闲置超时** `set_idle_timeout_ms`（默认 60s，slowloris 防御）：半截请求、连上不说话、
  慢速滴字节都被关掉；**在途请求不被误杀**。
- **multipart 只认 CRLF**，裸 LF 判 400 —— 理由不是"RFC 这么写"而是**请求走私**。
- `[LWSP]` 上限 128 字节，超了判 400。
- 上传落盘名**不用客户端给的名字**，`O_EXCL` 且**不带 `O_TRUNC`**。
- JSON 深度预扫描（§15）。
- 工作池闸门 + 静态 503（§14）。

### 没做的（如实列出）

- **没有 CSRF / SameSite 的自动防护**：`set_cookie` 的 `same_site` 默认 `"Lax"`，
  但要不要用、用哪种是你的事。
- **没有内建的鉴权 / 会话 / 限流**：`too_many_requests()` 只是个状态码 helper。
- **没有请求体解压**（gzip 请求体不认识）。
- **没有读背压**：流水线里排在后面的请求照常解析、照常跑，超上限直接拒（§18）。
- **h2 只走 TLS + ALPN**，没有 h2c（见下）。

---

## 18. 已知限制

| 限制 | 说明 |
|---|---|
| **流水线的读背压不存在** | 支持流水线：同一条连接上的响应**按请求到达顺序**发出（RFC 7230 §6.3.2），但排在后面的请求会被**照常解析、照常跑**，不因为前面的响应还没发出去就暂停收。同连接在途上限由 `set_max_pipelined_requests()` 定，默认 8；到上限的那条回 `503` + `Connection: close`，排在它前面的响应照常发完再关。 |
| ~~**HEAD 在客户端侧不可用**~~ | ✅ **已修**：`uvcpp_http_client` 的三条发送路径都认 HEAD 了 —— 异步路径把方法交给解析器（llhttp 只认 `flags & F_SKIPBODY`，它自己从不看 `method`），两条阻塞路径按 RFC 7231 §4.3.2 不再等 body。响应的 `Content-Length` **原样保留**（那是 HEAD 的正当语义），body 为空。用 `uvcpp_http_request::make_head(url)` 构造。 |
| **WS 连接没有超时保护** | 升级成功后不受 `idle_timeout_ms` 约束（§10）。 |
| **重连能力属于框架层** | 协议层 `uvcpp_ws_client` 一个对象只能连一次（§11）。 |
| **`run()` 只在"没有活句柄"时才自然返回** | 连着、有在途收发、或有重连定时器在等的时候，`run()` **不会自己返回**，得由使用者 `stop()` —— 所以要让 `run()` 收场，要么 `on_close`/`on_text` 里调 `stop()`，要么让它在无事可等时自己结束。**"无事可等"是确切的两条**：不开重连时对端走掉、开重连时次数用尽且连不上。这正是 §11 示例那句"没有活句柄时自然返回"的意思。（`uvcpp_web_ws_client::run()` 的另一条出口是 `stopping_`。改前这一条不成立 —— 内部那个延迟回收用的 async 句柄一直把循环算成活的，实测见 `web_app_ws_client_func.cpp` 的 `run_default_returns_when_idle`。） |
| **上传没有进度回调** | 用 `stream()->on_progress` 或 `upload()->received_bytes()`（§8）。 |
| **`broadcast()` 不存在** | 自己维护房间表（§10）。 |
| **路由注册失败看不见** | `app.get()` 返回 `uvcpp_web_app&`；要检查就走 `app.router()`（§3）。 |
| **静态 dotfile 默认值是 404（`HIDE`）** | 枚举成员刻意**不**叫 `IGNORE` —— `winnt.h` 有 `#define IGNORE 0`，而枚举类的作用域挡不住宏（§7）。 |
| **上传上限全 App 一份** | 没有按路由覆盖（§8）。 |
| **在回调里 `delete` 客户端：能用，但那份内存不回收** | 在 `connect()` / `send()` 的回调里 `delete` 掉 `uvcpp_http_client` 是**支持**的（"响应回来就把客户端扔了"是本类最自然的用法），删掉之后不许再碰、也不许再 `send()`。代价是那一次析构**故意不拆内部对象**：`uvcpp_loop` + `uvcpp_tcp_client` + 解析器（h2 上再加一个 nghttp2 会话）留在循环上不释放 —— 换掉一个必然发生的 use-after-free（析构里那段"泵到句柄关完"会把**还压在栈上**的那条读路径再叫一遍，实测 `0xC0000005`）。要让内存回归，就别在回调里删：回调里置一个标志，循环退出之后再删。 |
| **`uvcpp_http_client` 只在"有请求在飞"时才看得见对端断开** | 异步路径上对端在响应收完之前断开，`send()` 的回调会**落地**并以 `UV_ECONNRESET` 交付（本层写死的值，与传输层看到 EOF 还是 RST 无关），`HTTP_CLIENT_CONNECTED` 同时被清掉，此后的 `send()` 直接回 `UV_ENOTCONN` 而不是写进一条死 socket。**但只 `connect()` 过、从没 `send()` 过的客户端仍然不知道** —— 关闭通知只能搭在读路径上，而读是在第一次 `send()` 里才装的（`src/web/uvcpp_http_client.cpp:418-429`）。 |
| **h2 上"可以重试"只有 `REFUSED_STREAM` 一个来源** | 对端关掉一条 h2 流时分三档（RFC 9113 §8.7）：`NO_ERROR`（多半是我们自己收摊）、`REFUSED_STREAM`（**这条请求没被处理过**）、`CANCEL`（对端不要这条流了）—— 三档都以 `UV_ECANCELED` 交付，其余错误码是 `UV_EPROTO`（协议失败）。其中只有 `REFUSED_STREAM` 会把 `resp.retryable` 置真。**假不等于"处理过了"**：连接断开是"结果未知"，`CANCEL` 不保证对端没处理过（重发就是重复副作用），两者都为假 —— 判据只能是"真 ⇒ 可以重试"，反过来推不成立。h1 上它恒为假（这个协议没有等价信号）。这个字段挂在**响应**上而不是客户端上：h2 一条连接同时有好几条流在飞，挂在客户端上分不清是哪一条。 |

---

## 可运行示例

`examples/webapp_demo.cpp` —— 默认**自校验**：起一个 app（路由 + 中间件 + 静态 +
WS 回显），然后用框架自带的 HTTP 客户端和 WS 客户端连它自己，把本指南讲的几条
契约挨个跑一遍（状态码、路径参数、工作线程里恢复的 `next`、静态文件、点文件与
越界路径、WS 回显），退出码 0 = 全过。

静态服务那几条断言用的是它**自己造的**临时文档根 `webapp_demo_public/`
（跑完删掉）—— 因为框架对不存在的文档根回的是 500 而不是 404，拿一个不存在的
目录去断言 404 是在空跑（§7）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON -DUVCPP_BUILD_EXAMPLES=ON
cmake --build build --config Release --target webapp_demo
./build/examples/Release/webapp_demo.exe            # 自校验，退出码 0 = 通过
./build/examples/Release/webapp_demo.exe --serve    # 只起服务，Ctrl-C 停
./build/examples/Release/webapp_demo.exe --help
```

---

## 对照表：常见需求的入口

| 我要…… | 用 |
|---|---|
| 加一条路由 | `app.get/post/put/del/patch/...`（§3） |
| 路径参数 | `/user/:id` + `req.param("id")`（返回指针） |
| 拦一手所有请求 | `app.use(mw)`，洋葱模型（§4） |
| 自定义 404 | 一个在终端之前短路的中间件（§3） |
| 改错误响应 | `web_middleware_error_handler(...)` |
| 发 JSON | `resp.json(j)` / `resp.json_str(s)`（§6） |
| 发文件 | `resp.send_file(path)` / `send_file_range(...)`（§6） |
| 挂静态目录 | `app.serve_static("/assets", "./public")`（§7） |
| 收上传 | `app.set_upload_dir(...)` + `app.post_upload(...)`（§8） |
| SSE | `resp.begin_chunked("text/event-stream")` + `write_chunk(...)`（§9） |
| 收流式请求体 | `app.post_stream(...)` + `req.stream()->on_data/on_end`（§9） |
| WS 服务端 | `app.websocket(pattern, handler)`（§10） |
| WS 客户端（带重连） | `uvcpp_web_ws_client` + `set_reconnect(rc)`（§11） |
| HTTPS / WSS | `app.enable_ssl(cert, key)`（§12） |
| 跑阻塞的活 | `uvcpp_work` + `next` 的副本（§14） |
| 打日志 | `UVCPP_LOG_INFO(category) << ...`（§16） |

相关文档：[CI 维护指南](./ci-guide.md)、[项目 README](../README.zh.md)、
[多进程横向扩展设计](./worker-process-design.md)、[多循环横向扩展设计](./multiloop-design.md)（两篇都是设计稿，未实现）。
