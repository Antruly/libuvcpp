# webapp 支持类型指南

`webapp-guide.md` 讲的是**框架**：`uvcpp_web_app`、路由、请求响应、中间件、上传、
静态服务、WebSocket、日志等级。这一页讲框架底下那**七个被框架用、但指南一次都没提**
的类型 —— 它们的头文件加起来约 1700 行，全仓零文档：

| 类型 | 头 | 一句话 |
|---|---|---|
| `uvcpp_web_connection_registry` | `webapp/uvcpp_web_connection.h` | 连接身份表：把 `uvcpp_tcp_client*` 换成**永不复用**的 id |
| `uvcpp_web_util` | `webapp/uvcpp_web_util.h` | 39 个自由函数：编码、URL、Cookie、路径安全、MIME、日期 |
| `uvcpp_web_mime_map` | `webapp/uvcpp_web_mime.h` | 在内置 MIME 表之上增删改 |
| `uvcpp_web_multipart` | `webapp/uvcpp_web_multipart.h` | multipart/form-data 的增量解析器（纯逻辑，不碰 IO） |
| `uvcpp_web_file_transfer` | `webapp/uvcpp_web_file.h` | 分片异步读 + 有界滑动窗口的文件下发 |
| `uvcpp_web_context` | `webapp/uvcpp_web_context.h` | 每请求上下文：req/resp、中间件链、异步续跑、生命周期 |
| `uvcpp_console_log_sink` | `webapp/uvcpp_log_console.h` | 框架的默认日志 sink |

**这一页按类型清单界定范围。** 框架行为（怎么注册路由、怎么发响应、什么时候回滚）
**一律以 `webapp-guide.md` 为准**；这里只写这七个类型自己的契约。判定入选用的实测：
`grep -c` 这七个名字在 `doc/webapp-guide.md` 里的命中行数是 0、0、0、0、0、0、1。

- 打开方式：`-DUVCPP_BUILD_WEBAPP=ON`（与 web 层一起，见 `web-http-guide.md`）
- 包含方式：全部是 `<webapp/…>`。注意 `web_multipart_boundary()` 虽然名字像
  multipart 的，**住在 `uvcpp_web_util.h` 里** —— `uvcpp_web_multipart.h` 不含它
- 不需要 OpenSSL 头：`webapp/` 的公开头一律前置声明 + PIMPL

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一层没做"的地方都明确
> 标出来。**头文件里的契约与默认值也已经对着实现校正过一轮** —— 所以下面给的是
> 当前的行为，不是"注释说 A、实现是 B"的对照表；要确认某一条，头文件本身就是依据。

---

## 目录

1. [这一页是什么](#1-这一页是什么)
2. [编译期条件与包含](#2-编译期条件与包含)
3. [连接身份](#3-连接身份)
4. [Web 工具函数](#4-web-工具函数)
5. [MIME 表](#5-mime-表)
6. [multipart 解析](#6-multipart-解析)
7. [文件下发](#7-文件下发)
8. [每请求上下文](#8-每请求上下文)
9. [控制台日志](#9-控制台日志)
10. [默认值与头注释对不上](#10-默认值与头注释对不上)
11. [线程与所有权](#11-线程与所有权)
12. [典型坑](#12-典型坑)
13. [没做的（如实列出）](#13-没做的如实列出)

---

## 1. 这一页是什么

这七个类型分两拨：

**框架自己接线用的**（你多半不会直接碰，但出问题时得知道它在记什么账）：
`uvcpp_web_connection_registry`、`uvcpp_web_context`、`uvcpp_console_log_sink`。

**给你写业务代码直接调用的**：`uvcpp_web_util`（39 个自由函数）、
`uvcpp_web_mime_map`、`uvcpp_web_multipart`、`uvcpp_web_file_transfer`。
前两个是纯函数/纯数据，后两个一个纯逻辑、一个纯异步 IO。

它们**互不依赖**：`uvcpp_web_multipart` 不碰 `uvcpp_web_file_transfer`，
`uvcpp_web_util` 不碰任何人。所以你可以只 include 你需要的那一个 —— 实践中这一点
在 `web_multipart_boundary()` 上会绊一下（见 §6）。

---

## 2. 编译期条件与包含

`-DUVCPP_BUILD_WEBAPP=ON` 打开，宏是 `UVCPP_WEBAPP_ENABLE`（由包里的
`uvcpp/uvcpp_config.h` 提供；自己传一个冲突的值是硬 `#error`）。

**这七个头里没有一个用 `UVCPP_WEBAPP_ENABLE` 守卫自己。** 21 个 webapp 头
里只有 `src/webapp/uvcpp_web_ws_client.h:52` 一处（`#endif` 在 `:402`）用了这个宏。
所以"没开 webapp 却 include 了这七个头"是**链接期失败**（那几个 `.cpp` 没进构建，
`CMakeLists.txt:638-639`），不是编译期被 `#if` 挡住。这与 `uvcpp_web_ws_client.h`
的做法不一致 —— 看上去是历史的，不是设计。

头在包里的位置是平铺的 `include/webapp/`：

```cpp
#include <webapp/uvcpp_web_util.h>
#include <webapp/uvcpp_web_mime.h>

void doc_includes() {
  (void)uvcpp::web_to_lower("ABC");
  uvcpp::uvcpp_web_mime_map mime;
  (void)mime.size();
}
```

---

## 3. 连接身份

`uvcpp_web_conn_id` 是一个 `uint64_t`（`src/webapp/uvcpp_web_connection.h:59`），
从 1 开始，**单调递增、永不复用**；0 是无效值
（`UVCPP_WEB_INVALID_CONN_ID`，`:62`）。存在它的唯一理由是：
异步回调里"连接 7 已经死了"这句话，不会因为新连接拿到了 7 而变成谎话。

```cpp
#include <webapp/uvcpp_web_connection.h>

void doc_conn_lookup(uvcpp::uvcpp_web_connection_registry& reg,
                     uvcpp::uvcpp_tcp_client* client, int64_t now_ms) {
  const uvcpp::uvcpp_web_conn_id id = reg.add(client, "127.0.0.1", 12345, now_ms);
  if (id == uvcpp::UVCPP_WEB_INVALID_CONN_ID) return;   // 登记失败

  reg.note_read(id, now_ms);          // 闲置超时的依据
  reg.mark_streaming(id, true);

  uvcpp::uvcpp_tcp_client* live = reg.client(id);        // 断开过就是 nullptr
  if (live == nullptr) return;                           // 丢弃响应，别写

  reg.note_request_done(id);
  reg.mark_streaming(id, false);
  reg.remove(id);
}
```

`add()` 的两个默认参数（`peer_ip` 空串、`peer_port` 0、`now_ms` 0）让它最常用的
形态只有第一个实参。登记表全部方法在 `src/webapp/uvcpp_web_connection.h:139-360`。

**一个连接有 `add()` 过的 id 就直接用它，别拿 `uvcpp_tcp_client*` 当身份。**
旧那套（`src/web/uvcpp_static_server.cpp:418`、`:443` 用
`server->connection_generation(client)`）和新这套（`uvcpp_web_app` 内部的
`registry_`）都是"单调递增永不复用"，**但两者不通用**，别互相替换。
第三套是裸 `uvcpp_tcp_client*` —— 前两套的存在就是为了不用它。

---

## 4. Web 工具函数

`webapp/uvcpp_web_util.h` 是**纯函数模块，没有类**，39 个公开声明。
它的头注释（`:10-11`）解释的是**为什么要新开这个模块** —— web 层连一个 query
解析函数都没有，只把原始请求目标原样交出来（`src/web/uvcpp_http_server.cpp:217-220`），
不是"这一页里的东西不存在"。`src/web/` 下也确实没有任何 query / cookie / URL 解码
helper。

路径安全要**两道一起用**：

```cpp
#include <webapp/uvcpp_web_util.h>

void doc_path_safety(const std::string& raw, const std::string& root_real) {
  std::string safe, real;
  if (uvcpp::web_sanitize_path(raw, safe) != uvcpp::web_path_status::OK) return;
  if (uvcpp::web_resolve_within_root(root_real, safe, real) != uvcpp::web_path_status::OK) return;
  (void)real;
}

void doc_query(const std::string& query) {
  std::vector<std::pair<std::string, std::string> > kv = uvcpp::web_parse_query(query);
  const std::string* first = uvcpp::web_find_param(kv, "q");   // 查不到返回 nullptr
  (void)first;
}
```

`web_sanitize_path()` 是**纯文本、不碰文件系统**（`src/webapp/uvcpp_web_util.h:317-320`），
挡不住符号链接；`web_resolve_within_root()` 才真的去解析真实路径
（`:372-386`，`TRAVERSAL` 对 403、`NOT_FOUND` 对 404）。只用第一个是不够的。

其余分组：

| 组 | 函数 |
|---|---|
| 字符串 | `web_to_lower` `web_to_upper` `web_trim` `web_starts_with` `web_ends_with` `web_starts_with_ci` `web_equals_ci` `web_split`（两个重载） |
| 百分号编解码 | `web_url_decode` `web_url_encode` `web_hex_value` |
| URL 与查询串 | `web_split_path_query` `web_collapse_slashes` `web_parse_query` `web_find_param` |
| Cookie 与 boundary | `web_multipart_boundary` `web_parse_cookies` `web_find_cookie` `web_build_cookie` |
| 路径 | `web_join_url` `web_join_root` `web_is_within_root` `web_real_path` `web_sanitize_filename` `web_path_status_name` |
| MIME 与状态码 | `web_mime_type` `web_mime_builtin_table` `web_mime_is_text` `web_status_text` |
| HTTP 日期 | `web_http_date` `web_http_date_now` `web_parse_http_date` |

`web_path_options` 是这一族里**唯一**用对了构造方式的：
`web_path_options(size_t max_length = 4096, size_t max_depth = 64, bool decode = true)`
（`src/webapp/uvcpp_web_util.h:294-295` 带默认实参的 explicit 构造函数），所以
`web_path_options{1024}` 与 `web_path_options(4096, 64, false)` 两种写法都行。

---

## 5. MIME 表

`uvcpp_web_mime_map` 存在的两个理由（`src/webapp/uvcpp_web_mime.h:10-19`）：
加内置没有的类型，以及**改**内置的判定 —— 内置表里 **`.ts` 是 `video/mp2t`**
（MPEG 传输流），前端项目要的是 `text/typescript`。

```cpp
#include <webapp/uvcpp_web_mime.h>

void doc_mime_override() {
  uvcpp::uvcpp_web_mime_map mime;                 // 已带内置表
  mime.set("ts", "text/typescript");              // 盖掉内置的 video/mp2t
  mime.set(".tsx", "text/typescript");            // 带不带点都行
  mime.set_from_string("yaml=text/yaml;toml=text/toml");

  const char* t = mime.lookup("/src/main.ts");    // 静态字符串，查不到给 octet-stream
  (void)t;
}
```

内置表**只有一份**：`web_mime_builtin_table()`（`src/webapp/uvcpp_web_util.h:456`，实现
`src/webapp/uvcpp_web_util.cpp:981-984`），`builtin_size()` 也转调它
（`src/webapp/uvcpp_web_mime.cpp:135-139`）。所以"同一个文件在 `web_mime_type()` 和静态服务里
类型不一样"这类漂移在结构上被排除了 —— 值得知道，因为这类漂移很难查。

`default_map()`（`src/webapp/uvcpp_web_mime.h:126`）是**进程级共享**的静态表，静态服务没显式
给表时用它。它的构造是 C++11 函数内静态（线程安全），但 `set()` 本身**不是**线程
安全的（内部是 `std::map`）。注册期配置、运行期只读。

---

## 6. multipart 解析

`uvcpp_web_multipart` 是 multipart/form-data（RFC 7578 / 2046）的**增量**解析器：
**没有 IO、不碰 libuv、不碰请求对象**（`src/webapp/uvcpp_web_multipart.h:13-16`）。
内存上界只与 boundary 长度有关、与喂进来多少数据无关 ——
`uvcpp_web_multipart_max_retain(boundary_len)` 把这个上界导出成了函数
（`:206`），好让实现和用例引用同一个数。

```cpp
#include <webapp/uvcpp_web_multipart.h>
#include <webapp/uvcpp_web_util.h>   // web_multipart_boundary 在 util 里

class doc_mp_sink : public uvcpp::uvcpp_web_multipart_sink {
 public:
  bool on_part_begin(const uvcpp::uvcpp_web_part_info& info) override {
    return info.is_file ? true : true;            // false = 中止解析
  }
  bool on_part_data(const char* data, size_t len) override {
    (void)data; (void)len;
    return true;
  }
  void on_part_end(bool truncated) override { (void)truncated; }
};

void doc_multipart_feed(const std::string& content_type) {
  const std::string boundary = uvcpp::web_multipart_boundary(content_type);
  if (boundary.empty()) return;                   // 不是 multipart/form-data

  doc_mp_sink sink;
  uvcpp::uvcpp_web_multipart mp;
  mp.set_sink(&sink);
  if (!mp.set_boundary(boundary)) return;         // 空 / >70 字节
  mp.set_max_file_count(8);
  mp.set_max_file_size(64u * 1024u * 1024u);      // 超限截断并置 truncated

  if (mp.feed("--x\r\n", 5) == uvcpp::uvcpp_web_multipart_result::DONE) return;
  if (mp.finish() != uvcpp::uvcpp_web_multipart_result::DONE) return;  // 报文被掐断
  (void)mp.body_bytes();
}
```

三个 sink 回调**只从 `feed()` 的调用栈里**来（`:131-134`），不用考虑重入；
返回 `false` 就是中止（`ERROR_SINK_ABORTED`）。`filename` 是"**解码后、未清洗**"
的原始值（`:76-83`）：清洗归上传层调 `web_sanitize_filename()`，
**永远不能拿去拼路径**。

**默认上限只写在 `.cpp` 里，头文件一个都没写**（`src/webapp/uvcpp_web_multipart.cpp:288-293`）：

| 旋钮 | 默认 | 超限的处置 |
|---|---|---|
| `max_part_header_bytes` | 8 KiB | 失败 |
| `max_file_count` | 32 | 失败 |
| `max_field_count` | 128 | 失败 |
| `max_file_size` | 0（不限） | **截断**并置 `truncated` |
| `max_field_size` | 0（不限） | 失败（字段不截断） |

**截断和拒绝是两种处置**：文件超限截断（`:368-372`，转 `PART_BODY_SKIP` 继续扫
边界），字段超限直接拒（`:363-367`）。`on_part_end(bool truncated)` 就是拿这个事实。

---

## 7. 文件下发

`uvcpp_web_file_transfer` 把文件的 `[first, last]`（**闭区间**）分片读出来交给
sink，用有界滑动窗口把静态下发的峰值内存从 2N 降到
`high_water + 2*slice`（默认 ≈ 1.5 MiB，**与文件大小无关**，
`src/webapp/uvcpp_web_file.h:26-38`）。它刻意不拉 libuv（fd 用 `int` 存，`:386`）。

```cpp
#include <webapp/uvcpp_web_file.h>

class doc_file_sink : public uvcpp::uvcpp_web_file_sink {
 public:
  void on_data(const char* data, size_t len) override {
    (void)data; (void)len;      // 必须在这一句内把数据拷走
  }
  size_t backlog() const override { return 0; }   // 唯一的背压信号
  void on_done(int status, uint64_t bytes_sent) override {
    (void)status; (void)bytes_sent;               // 0 = 全部交付完
  }
};

void doc_send_file(uvcpp::uvcpp_loop* loop, doc_file_sink& sink,
                   const std::string& path, uint64_t size) {
  std::shared_ptr<uvcpp::uvcpp_web_file_transfer> t(
      new uvcpp::uvcpp_web_file_transfer(loop, &sink));   // 必须 shared_ptr 持有
  t->set_slice_bytes(0);                                  // 0 = 用默认值
  if (t->start(path, 0, size - 1) != 0) return;            // 提交失败，不会有回调
  // 对端断开时：t->cancel();
}
```

四条契约，都对着实现核过：

- **`on_data` 的数据只在那一句里有效**（`:116-123`）：transfer 下一笔 `uv_fs_read`
  立刻覆盖同一块缓冲。不接受"先记下指针，回头再处理"。
- **`start()` 绝不会同步回调**（`:227-228`）：每一次 `on_done` 都来自某笔 fs 操作的
  完成回调，所以 `start()` 返回之后可以放心继续设自己的状态。
- **`start()` 返回非 0 = 提交失败，此时一个回调都不会有**（`:230-232`），
  调用方丢掉自己的 `shared_ptr` 即可。
- **`on_done` 恰好一次**（`:134-146`），`status` 是 **libuv 码**（0 / `UV_ECANCELED` /
  `UV_EINVAL` 之类），不是 HTTP 码 —— 响应层自己会翻
  （`src/webapp/uvcpp_web_response.cpp:1203-1223`）。

**必须由 `shared_ptr` 持有**（`:164`，内部用 `shared_from_this()`）。放栈上会在
`start()` 里抛 `std::bad_weak_ptr`，而且**不是编译错误**。

---

## 8. 每请求上下文

`uvcpp_web_context` 持有一个请求的 req/resp、跑中间件链、管异步续跑与生命周期。
它**只拿 `uvcpp_web_conn_id`，从不持有 `uvcpp_tcp_client*`**
（`src/webapp/uvcpp_web_context.h:58-63`），要原始连接得显式走 `raw_client()`。

```cpp
#include <webapp/uvcpp_web_context.h>

void doc_ctx_async(uvcpp::uvcpp_web_context& ctx) {
  const uvcpp::uvcpp_web_conn_id id = ctx.connection_id();
  ctx.hold();                       // 每次 hold 都要有对应的 release
  ctx.post([&ctx, id]() {           // 已经在 loop 线程上时会就地同步执行
    if (ctx.connection_alive()) {
      (void)id;
    }
    ctx.release();
  });
}
```

**只能由 `create()` 造**（`:156-157`）：构造函数是私有的，直接 `new` 或放栈上会让
`shared_from_this()` 抛异常（`advance()` 依赖它把自己续住）。

异步续跑有两条路，**推荐留 `next`**：把 `next` 按值捕获进完成回调就是"我会异步
恢复"的承诺，框架会自己投回 loop 线程。`post()` 那条路要自己 `hold()`。两条都不走
就是"这个请求永远没有响应"，一直挂到闲置超时 —— 这是这个框架最容易踩的一条。

`post()` 在 loop 线程上**就地同步执行**（`src/webapp/uvcpp_web_context.cpp:66-76`），不能假设
fn 在"下一个循环迭代"跑。

其余入口：`request()` / `response()`（`:271-275`）、`user_data_as<T>()`
（`:258-265`，`typeid` 比名字，跨 DLL 可靠，取错类型返回空 `shared_ptr` 而不是野
指针）、`stream()` / `attach_stream()`（`:293-300`）、`abort()` / `finished()`
（`:345-350`）、`chain_index()` / `chain_size()`（`:353-354`，诊断用）。

---

## 9. 控制台日志

`uvcpp_console_log_sink` 是框架的**默认 sink** —— 不配置任何东西时日志就打到控制台
（`src/webapp/uvcpp_log_console.h:6-9`）。`webapp-guide.md` §16 已经讲了
`uvcpp_logger`、等级、模块与自定义 sink 接口，这里只讲这个内置 sink 的外观与过滤。

```cpp
#include <webapp/uvcpp_log_console.h>

void doc_console_sink() {
  uvcpp::uvcpp_console_log_options opt;   // 只有无参构造
  opt.color = false;                      // 关掉 ANSI
  opt.show_thread = false;                // 默认是 true

  uvcpp::uvcpp_console_log_sink sink(opt);   // sink 必须活得比用法久
  sink.set_min_level(uvcpp::log_level::WARN);  // 等级名是 ERR，不是 ERROR
  uvcpp::uvcpp_logger::instance().set_sink(&sink);   // 不接管所有权
}
```

三个要点：

- **`set_sink()` 不接管所有权**（`src/webapp/uvcpp_log.h:224-229`）：`&sink` 必须比
  用法活得久。内置 sink 由 logger 自己 `new`/`delete`（`src/webapp/uvcpp_log.cpp:256-258`、
  `:273`、`:342`），**用户传进去的 sink 不会被 delete**。
- **`split_streams` 默认 true** ⇒ WARN 及以上走 `stderr` 并且会 `fflush`
  （`src/webapp/uvcpp_log_console.cpp:228-235`）。这是"WARN 日志不会因为进程被杀而丢"的实现点。
- **过滤有三层，排查"看不到日志"要按顺序查**：logger 全局等级 → 模块等级 →
  `sink::min_level`。sink 的默认 `min_level_` 是 `TRACE`（`:134`、`:141`），
  所以默认只在 logger 那层过滤。

等级名是 **`log_level::ERR`（=4），不是 `ERROR`**（`src/webapp/uvcpp_log.h:64-70`：
`<wingdi.h>` 里 `#define ERROR 0`）。`OFF`（=6）只作阈值，`should_log()` 显式挡掉
（`src/webapp/uvcpp_log_console.cpp:149`）。

---

## 10. 默认值与头注释对不上

这一批类型里，"契约类"注释都准；错的是**默认值**与**构造方式**这两类。

### `uvcpp_console_log_options{false, true}` 编不过

```cpp
// doc-snippet: fragment — 反面例子，故意编不过；照抄 src/webapp/uvcpp_log_console.h:25-26 的注释写出来的
uvcpp_console_log_options opt{false, true};
```

`src/webapp/uvcpp_log_console.h:25-26` 的注释现在写的是**真正的成因**：让花括号写法
失效的是那个**用户声明的构造函数**（`uvcpp_console_log_options();`，`:38`），不是
NSDMI —— "避开 NSDMI"并不能让花括号写法变得可用。实测 g++ 的原话是

```
error: no matching function for call to
  'uvcpp::uvcpp_console_log_options::uvcpp_console_log_options(<brace-enclosed initializer list>)'
```

正确写法是"先默认构造，再逐字段赋值"（§9 那段）。同类问题还有
`src/webapp/uvcpp_web_app.h:155-158`：注释声称支持 `uvcpp_web_app_config cfg{8080}`，
而声明在 `:278` 只有 `uvcpp_web_app_config();`。
**对照组**：`web_path_options`（`src/webapp/uvcpp_web_util.h:294-295`）是这一族里唯一写对的
—— 它的构造函数带默认实参，所以花括号写法可用。

### 上不上色是两个条件

`color` 这个字段的默认值是 **`true`**（`src/webapp/uvcpp_log_console.cpp:124`），真正
决定上不上色的是 `color_enabled()` = `color_supported_ && options_.color`
（`:171-173`）。终端能力那一半在 sink 构造时探测一次（`:135`、`:142`，探测逻辑
`:67-85`：`NO_COLOR`、`isatty`、Windows 的 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`）。
所以**默认构造下的有效行为就是"按终端能力判断"**，但那是 `color_supported_` 给的，
不是 `color` 的默认值 —— 而且它**只在构造时算一次**，构造之后再把 stdout 重定向，
上色状态不会跟着更新。

### 真实输出的一行长什么样

头里的样例（`src/webapp/uvcpp_log_console.h:45-47`）画的就是**全默认**形状：时间戳、
等级、模块标签、`(tid:N)`、message，末尾是 `<调用点文件>:<行>`。真实的一行长这样
（末尾取自 `tests/functional/web_app_log_func.cpp:84` 那次调用）：

```
2026-09-13 12:34:56.789 [INFO ] [REQUEST] (tid:14028) GET /index.html  (tests/functional/web_app_log_func.cpp:84)
```

那一列由 `show_thread` 拼出，默认 **true**（`src/webapp/uvcpp_log_console.cpp:126`、
`:209-214`）；不想要就 `opt.show_thread = false`。

### `on_done` 不一定来自 `uv_fs_close`

有三条**没有 close 回调**的路径直接调 `finish()`：open 失败
（`src/webapp/uvcpp_web_file.cpp:241-247` → `finish()`；`fd_ < 0` 时 `:404-408` 直接收尾）、
close 提交同步失败（`:420-424`）、`submit_read` 同步失败（`:325-330`）。
走到 `on_done` 时 fd 「**已经关完，或者根本没打开过**」（`src/webapp/uvcpp_web_file.h:141-143`），
所以"在这里销毁 transfer 是安全的"成立，但**别把"回调已返回"当成前置条件**去依赖。

### `web_split_path_query()` 比头文件说的做得多

`src/webapp/uvcpp_web_util.h:133-147` 只说"拆 path/query + 剥 `#fragment`"，实现还会**先剥掉
绝对形式（代理风格）的 `http://host`**（`src/webapp/uvcpp_web_util.cpp:206-210`）——
`GET http://x/../y HTTP/1.1` 这种请求目标。头文件没写这一条。

### 点落在目录名里时会取错

`lookup("a.b/c")` 得到的是 **`b`**，不是 `c`：实现先取**最后一个点**再截到第一个
分隔符（`src/webapp/uvcpp_web_mime.cpp:110-125`），而 `a.b/c` 里最后一个点在目录名中。
所以传进来的应当是**文件名本身**，这一层不替上游剥目录。头
`src/webapp/uvcpp_web_mime.h:103-104` 举的 `archive.tar.gz` 与 `lib..min.js` 两个例子
不受影响；这条限制写在 `.cpp` 那句注释里（`:111-113`）。

---

## 11. 线程与所有权

### 线程纪律

| 类型 | 约束 | 出处 |
|---|---|---|
| `uvcpp_web_connection_registry` | **只能 loop 线程**（内部 `std::map` 无锁） | `src/webapp/uvcpp_web_connection.h:30-31`、`:142` |
| `uvcpp_web_context` | 除 `post()` 外都在 loop 线程 | `src/webapp/uvcpp_web_context.h:98-99` |
| `uvcpp_web_file_transfer` | loop 线程驱动，回调都在 loop 线程 | `src/webapp/uvcpp_web_file.h:168-172` |
| `uvcpp_web_util` | 纯函数，无状态、线程安全 | — |
| `uvcpp_web_mime_map` | 注册期配置、运行期只读；`default_map()` 是全局的 | `src/webapp/uvcpp_web_mime.h:31-34` |
| `uvcpp_web_multipart` | **无线程设施**，由调用方决定在哪条线程跑 | `src/webapp/uvcpp_web_multipart.h:13-16` |
| `uvcpp_console_log_sink` | `write()` 可从工作线程调用；`options()` 的读取**无锁** | `src/webapp/uvcpp_log.h:135-140`；`src/webapp/uvcpp_log_console.cpp:163-169` |

在别的线程上要查连接，先 `context::post()` 回 loop 线程
（`src/webapp/uvcpp_web_connection.h:31`）。改 `options()` 而 `write()` 正在别的线程上跑是
**数据竞争** —— 选项只在构造时定，或者只在 loop 线程改。

### 所有权

- `uvcpp_web_file_transfer` **必须 `shared_ptr`**（`src/webapp/uvcpp_web_file.h:166`）。
- `uvcpp_web_file_sink::on_data` 的数据**只在那一句里有效**（`:117-124`）。
- `uvcpp_web_context::run()` **只存链的指针**
  （`src/webapp/uvcpp_web_context.h:366`、`:447`）：那张
  `std::vector<uvcpp_web_handler>` 必须在上下文存活期内有效且不被修改 ——
  **别传一个临时 vector**。
- `uvcpp_web_connection_registry::find()` 返回内部 `std::map` 里的指针
  （`src/webapp/uvcpp_web_connection.cpp:210-216`）。
- `uvcpp_web_mime_map::lookup()` 返回表内 `std::string` 的指针
  （`src/webapp/uvcpp_web_mime.cpp:107-108`）。
- `uvcpp_logger::set_sink()` **不接管所有权**（`src/webapp/uvcpp_log.h:224-229`）。

### 错误码

这七个类型返回的失败码是**各自的 enum**（`web_path_status`、
`uvcpp_web_multipart_result`）或 **bool**（`uvcpp_web_connection_registry`、
`uvcpp_web_mime_map::set`）。只有 `uvcpp_web_file_transfer` 用 **libuv 码**
（含 `UV_EINVAL` / `UV_EALREADY` / `UV_EOF` / `UV_ECANCELED`）。

而 `uvcpp_web_util` 里另有**两个哨兵值**：`web_hex_value()` 失败返回 `-1`
（`src/webapp/uvcpp_web_util.cpp:127-132`），`web_parse_http_date()` 失败返回 `(time_t)-1`
（`:1173-1174`）。`-1` 在 POSIX 上**正好等于 `UV_EPERM`** ——
**这几种码一个都不要喂给 `uv_strerror()`**，会得到看似合理的垃圾。

---

## 12. 典型坑

**`set_boundary()` 失败会把解析器打进终态，之后救不回来。** 空或超过 70 字节时
它调 `fail()`（`src/webapp/uvcpp_web_multipart.cpp:316-322`）→ 置 `ERROR_STATE` 并清空保留缓冲
（`:342-348`）。之后**再补一个合法 `set_boundary()` 也没用**：`fail()` 有"首个错误
胜出"（`:341`），`feed()` 进 `ERROR_STATE` 直接返回旧 `result_`（`:650-653`）。
头 `:227-234` 只说了"@return false = 不合法"，**完全没提这个副作用**。所以
boundary 要在建解析器之后**立刻**设，并且检查返回值。

**三个 setter 的 `0` 语义不一样。** 文件/字段**大小**是 `> 0` 才生效
（`src/webapp/uvcpp_web_multipart.cpp:363`、`:368`），文件/字段**数量**也是 `> 0` 才生效
（`:430`、`:436`），但 `max_part_header_bytes_` **没有 `> 0` 守卫**（`:489`、`:502`）
⇒ `set_max_part_header_bytes(0)` 等于"任何部件头都超限"，每个部件都
`ERROR_HEADER_TOO_LONG`。

**`feed()` 在终态后是空操作，但 `received()` 照常累加。**
`src/webapp/uvcpp_web_multipart.cpp:648` 那句 `received_ += len` 在终态判断**之前**。
头里那句"无害"只修饰返回值（`src/webapp/uvcpp_web_multipart.h:281-283`）。上层若用
`received()` 判上传总长，这一点必须知道。

**`set_stop_at_eof(true)` 时 `last` 要传 `UINT64_MAX - 1`，不是 `UINT64_MAX`。**
后者 `remain = last_ - offset_ + 1u` 在第一个切片上就溢出成 0，于是当场收尾、
一个字节都不读（`src/webapp/uvcpp_web_file.h:225-227`）。

**只有 `set_slice_bytes()` 是"必须在 `start()` 之前调"。** 它内部会
`slice_buf_.resize()`（`src/webapp/uvcpp_web_file.cpp:119`），而 `submit_read()` 把
`&slice_buf_[0]` 交给 libuv 当接收目标 —— 在途读期间调它，扩容会重新分配那块缓冲，
那一笔 `uv_fs_read` 就写进**已释放**的内存。**这两个 setter 都没有运行时守卫**，也不改
返回值，顺序错了查不出来。

**`set_chunk_gate()` 是第二个"必须在 `start()` 之前调"的 setter**
（前一个是 `set_slice_bytes()`；`set_stop_at_eof` 只是"建议提前"，
`set_high_water_bytes` 没有约束）。它挂的是工作池的**块级名额闸门**
（`src/webapp/uvcpp_web_file.h:282`）：读一块之前拿一个名额、读完立刻还，拿不到
就停在块边界上等唤醒 —— 于是"真正并行读盘的条数"被名额封着，而"同时开着的大文件
传输数"不必跟着降。它与另外两个 setter 有两处不同：

- **它是一条借还纪律，不是一个配置项。** 名额的"还"被钉在**本传输所在循环的
  线程**上：唤醒回调是在 `release()` 的那条栈上**同步**跑的，回调里的 `resume()`
  会碰本对象的状态；从别的线程归还，就会和同一时刻循环线程上的 `cancel()`
  抢同一个对象（库内两处归还点都在循环线程上：`after_work` 的整读路径、
  `on_read_done` 的块级路径）。
- **它不接管所有权**：传的是**借用**指针，存活期由调用方保证 —— 静态路由借的是
  App 的 `work_limit()`（`src/webapp/uvcpp_web_static.cpp:1086`，且那一步在
  `t->start()` 之前）。它也是唯一一个**会挡掉违约**的 setter：跑起来之后换闸门
  会被**静默忽略**（`src/webapp/uvcpp_web_file.cpp:107`）—— 那时手上可能正攥着旧
  闸门的名额，换掉就没有对象可还了。

不调它（或传 `nullptr`）时**行为与从前逐字节相同**：直接 `new` 本类、不走静态路由
的那些用法不受影响。

`set_stop_at_eof` 没有这条限制（`src/webapp/uvcpp_web_file.h:229-233`）：全类只有一个
读取点，就是"早于 `[first, last]` 撞上 EOF"的那一刻，在那之前设上都算数。提前设是
习惯，不是要求 —— 那一刻何时到取决于文件实际多长。

**`cancel()` 在"因背压停读"这一支上不能省。** 那时**没有任何 fs 操作在途**，
`cancel()` 必须自己推进状态机，否则 `on_done` 永远不来、fd 一直开着
（`src/webapp/uvcpp_web_file.cpp:228-234`）。`start()` 之前 `cancel()` 是空操作且**不触发
`on_done`**（`src/webapp/uvcpp_web_file.h:299-302`）。

**`backlog()` 是唯一的背压信号。** 恒返回 0 的 sink 只是让背压失效，
窗口退化成"切片缓冲那一份"（`src/webapp/uvcpp_web_file.h:127-132`）—— 允许，但不是有界的了。

**`web_url_encode()` 的 `keep` 优先于 `plus_for_space`。**
`src/webapp/uvcpp_web_util.cpp:179` 先判 `unreserved || keep.find(c) != npos`，**再**判
`c == ' ' && plus_for_space`。所以 `keep` 里放空格或 `+` 会让它们**原样输出** ——
查询串里一个原样输出的 `+` 会被对端解成空格。头 `:109-121` 没提这个优先级。
只放行 `/` 是安全的。

**`web_build_cookie()` 只校验 name 与 value。**
`path`（`src/webapp/uvcpp_web_util.cpp:471-474`）与 `same_site`（`:483-488`）是**原样拼接**的
—— `same_site` 传 `"Lax; Domain=evil"` 就能注入额外属性。头 `:228-230` 只描述了取值
集合，没声明"会校验"。`name` 是校验的（`:461-464`，非 token 字符直接返回空串），
`value` 也是编码的（`:469`，用比 RFC 6265 更严的 `A-Za-z0-9-._~`）。附带一条正面
设计：`SameSite=None` 会自动补 `Secure`（`:486-489`）。

**`web_sanitize_filename()` 只能当元数据。** 它不保证是合法文件名（`*` `?` `"`
照过），也**永远不能拿去拼路径**（`src/webapp/uvcpp_web_util.h:415-419`）；`max_len` 是**软**
上限（设备名保护会让结果多一字节，`:411-413`）。

**`web_parse_query()` 只按 `&` 切**（`src/webapp/uvcpp_web_util.cpp:254-284`）—— `;` 会被当成
值的一部分。头 `:160-169` 没写分隔符。

**连接身份有三套，不要混。** 见 §3。

---

## 13. 没做的（如实列出）

- **`uvcpp_web_util` 里没有 JSON 解析**（在 `uvcpp_web_json.h`）、**没有可变 MIME 表**
  （在 `uvcpp_web_mime.h`）、**没有 gzip**（在 `src/web/`）、**没有 `URL` 类**
  （全是自由函数）、**也没有 `std::string_view` 版本** —— 头 `:42-44` 解释了原因：
  C++11 没有，所以全部吃 `const std::string&`。
- **`web_collapse_slashes()` 刻意不做**：不解码、不处理 `.` / `..`（与路由切段保持
  一致，挡目录穿越要用 `web_sanitize_path()`，`src/webapp/uvcpp_web_util.h:158-161`）。
- **`web_join_root()` 不做安全判断**（`src/webapp/uvcpp_web_util.h:342`），判断在
  `web_is_within_root()`（`:347-357`，实现用"前缀 + 分隔符"而不是裸前缀比较）。
- **其他 `multipart/*` 子类型不支持**（`mixed` 等）：`web_multipart_boundary()` 返回
  空串让调用方拒绝（`src/webapp/uvcpp_web_util.h:209-212`）。
- **multipart 只认 CRLF**（`src/webapp/uvcpp_web_multipart.h:69-73`）：裸 LF 在边界处不接受、
  在部件头里直接 400 —— 理由是请求走私。`[LWSP]` 上限 128 字节
  （`src/webapp/uvcpp_web_multipart.h:60-62`），超了明确失败而不是当 body。
- **multipart 不做文件名清洗**（`src/webapp/uvcpp_web_multipart.h:76-83`），`filename` 永远不能
  拼路径。
- **`uvcpp_web_file` 不知道 HTTP 的存在**：断连即释放**不在这里**，接线层调
  `cancel()`（`src/webapp/uvcpp_web_file.h:70-71`）。
- **卡住的出站流没有超时保护**（`src/webapp/uvcpp_web_context.h:393-395`，代价照实记在
  `src/webapp/uvcpp_web_context.cpp:359-364`）。
- **`attach_stream()` 的"必须在链跑起来之前调"没有运行时守卫**
  （`src/webapp/uvcpp_web_context.h:318-330`）：中途挂只会得到一个**永远收不到数据**的 stream，
  静默。
- **如实记一处代价**：`src/webapp/uvcpp_web_file.cpp:191-197` 承认同步失败路径会漏一份
  `shared_ptr`（成环），作者判断"实际走不到"。

> 这七个头里**没有 `TODO` / `FIXME` / 未实现字样**。上面这些是"**明确划界**"
> （设计取舍，可以引用），不是"**还没做**"（待办）。

---

相关文档：[webapp 框架指南](./webapp-guide.md)（框架行为以它为准 —— 路由、响应、
中间件、上传、静态服务、WebSocket）、[web HTTP 指南](./web-http-guide.md)
（下面那一层的 server / client / 静态服务）、
[web WS 指南](./web-ws-guide.md)、[构建指南](./build-guide.md)
（`UVCPP_BUILD_WEBAPP` 与其余开关的全表）。
