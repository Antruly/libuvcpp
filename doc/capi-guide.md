# C ABI 指南

`src/capi/` 是本库的 **C 接口层**：一组 `extern "C"` 导出的函数、几份**纯 C**
（C99）的头，装到 `include/capi/`。它的用途很具体 —— 让 C#、Rust、Go、Python
这些语言能通过 FFI（P/Invoke 那一路）用到本库的 webapp / web / net / http2 /
http3 / quic，而不需要写一个 C++ 中间层。

它与 C++ ABI 的关系只有一句：**同一份库、同一个版本号、同一个 `.dll`/`.so`**。
C 层是薄包装（约 150–250 个函数的精选门面，见 §4），不新增第三方依赖、不新增
产物、不改动 C++ 那一侧的任何导出行为。

> ## 进度：1.4.4 七片全部就位（这批做完了）
>
> **批 1（1.4.2）**交付了 `uvcpp_c_common.h` 与 `uvcpp_c_net.h`：ABI 自洽、错误码
> 与文案、句柄的生死与类型检查、版本化回调表的 `size` 规则、线程纪律，以及
> **`tcp_client` / `tcp_server` 的完整可用面**。
>
> **批 2（1.4.3）**交付了 `uvcpp_c_webapp.h`（服务端那一大片：app / 路由 /
> 中间件 / 静态 / 上传 / ws 路由 / `req` / `resp` / `next` / **延迟应答**）与
> `uvcpp_c_web.h`（**客户端**：`http_client` / `http_response` / `ws_client`）。
>
> **批 3a（1.4.3）**交付了 `uvcpp_c_http2.h`（驱动层一个句柄 + 构造器 + 流视图），
> **批 3b（1.4.4）**交付了 `uvcpp_c_quic.h`（TLS 上下文 / 客户端 / 服务端 /
> 借来的连接句柄）与 `uvcpp_c_http3.h`（接在借来的 QUIC 连接上的一层）。
> 到此共 **321 个函数 / 七片**。
>
> 判据是 `tests/capi/` 下五个**纯 C 编译**的用例（`capi_common_func.c`、
> `capi_net_func.c`、`capi_webapp_func.c`、`capi_h2_func.c`、
> `capi_quic_h3_func.c`）：三个端到端用例都是拿 C 写的客户端去打 C 写的服务端
> （webapp / h2 在 TCP 上，quic + h3 在 UDP 上），body 逐字节比，并在
> `tests/tools/capi_mutation.py` 那张变异表（二十条 + 一条基线）下被逐条拆守卫。
>
> **没做的**逐条列在 [§7](#7-没做的如实列出)（UDP 的通用面、h3 的 trailers 与
> push、逐帧回调、TLS 参数入口……）。别在别处另维护一份。

## 目录

- [1. 这一层是什么](#1-这一层是什么)
- [2. 怎么开](#2-怎么开)
- [3. ABI 契约](#3-abi-契约)
- [4. 提供什么、明确不提供什么](#4-提供什么明确不提供什么)
- [5. 从 C# P/Invoke 用](#5-从-c-pinvoke-用)
- [6. 怎么本地验证](#6-怎么本地验证)
- [7. 没做的（如实列出）](#7-没做的如实列出)

---

## 1. 这一层是什么

一句话：**把 C++ 的公开面按"FFI 用得上的那一部分"重新说一遍，说成 C 听得懂的
样子。** 它不是"再来一套 API"，也不是"给每个 C++ 方法配一个 `..._c()`"。

三条设计上的取舍，都是先量过再定的：

1. **不逐方法镜像。** 本库的公开方法有 `uvcpp_web_app` ≈107 个、
   `uvcpp_tcp_client` ≈60 个、`uvcpp_http_server` ≈56 个、
   `uvcpp_h2_session` ≈28 个、`uvcpp_h3_connection` ≈20 个 —— 逐方法镜像会到
   **1500+ 个函数**，而其中大半在 FFI 侧一次都用不上（临时的 C++ 类型入口、
   模板、`std::function` 的重载）。所以这一层是**精选门面**：每个模块给一组
   句柄 + 一组动作 + 一组查询，能建成一个真实的程序就够了。
2. **C++ 的那些"额外类型"才需要 C 版本，基本类型不需要。** `uvcpp_json` 是
   `nlohmann::json`，``std::vector<std::string>` 是模板 —— 这些绝不进
   `include/capi/`。响应侧给的是
   `uvcpp_c_resp_json_str(resp, const char* json, size_t n)`（一个字符串），
   请求侧给的是 body 的原始字节；C# 侧本来就用 `System.Text.Json` 序列化成
   字符串再传。而"底层就是 libuv"的那些东西（`uv_loop_t`、`uv_tcp_t`、
   `uv_buf_t`）**不进 C 头**：这一层不接受也不返回 libuv 类型，调用方也从不
   需要看见它们。
3. **ABI 稳定是机制，不是口号。** §3 的每一条契约都对应一条能当场红的判据
   （`tests/capi/` 的断言、`tests/tools/capi_mutation.py` 的变异表、
   `tests/tools/check_capi_symbols.py` 的符号锁）。

`src/capi/` 里没有任何 `.c` 文件 —— 实现全是 C++（`.cpp`），只有**头**是纯 C 的。
这不是偷懒：实现要用 `try/catch` 收口、要用登记表那把锁，那是 C++ 的活；而头
只要"能被 C 看见"就够了，那件事由编译器和用例来量（§6）。

## 2. 怎么开

```bash
cmake -S . -B build-capi -DUVCPP_BUILD_TESTS=ON \
  -DUVCPP_BUILD_NET=ON -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_BUILD_EXPAND=ON -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_ZLIB=ON \
  -DUVCPP_ENABLE_CAPI=ON
```

`UVCPP_ENABLE_CAPI` **默认 OFF**。它有一条守卫链（`CMakeLists.txt` 里两段
形状相同的 `message(WARNING)` + `set(... OFF)`）：

| 缺什么 | 后果 |
|---|---|
| `UVCPP_BUILD_NET=OFF` | 强制关闭 CAPI，打一条说明"地基是 net 那一层"的 warning |
| `UVCPP_BUILD_WEB=OFF` | 强制关闭 CAPI，打一条说明"web / webapp / http2 / http3 四块都接在 web 层上"的 warning |

两条分开写而不是合成一条 `NOT WEB OR NOT NET`：这两种缺失是两件不同的事，
出路也不同（一个是"上层四块没有宿主"，一个是"地基没编进来"）。

**它是普通变量 `set()`，不是 cache 写** —— 所以被强制关掉之后
`CMakeCache.txt` 里仍然写着 `UVCPP_ENABLE_CAPI:BOOL=ON`。要判"这一格到底开没开"
必须看 configure 日志里那句 `Including capi module in build`（CI 就是这么判的，
见 `doc/ci-guide.md` §5）。

**发布与 `full` 都开着它**：`release.yml` 的六条腿（含各自的 Debug 配置，共十个
configure）都传 `-DUVCPP_ENABLE_CAPI=ON`，CI 的 `full` 格与专门的 `capi` 格也都
开着。也就是说，**"库里能导出 C 接口"这件事每天都有腿在跑**，"关着"的配置也有
（其余各格）。

### 关着的时候，头还在包里吗

要分开说，因为两个来源不同：

- `cmake --install`（`install(FILES CAPI_HEADER_FILES ...)`）—— **不在**。
  那条规则套在 `if(UVCPP_ENABLE_CAPI)` 里。
- `tests/tools/package_release.py`（发布 zip）—— **在**。它是从 `src/<模块>/`
  逐目录拷头的，与开关无关（`wsdl` 那一路是同一个形状）。发布腿现在都开 CAPI，
  所以这两种来源在发布包里是一致的；但如果你自己拿一棵 `CAPI=OFF` 的树出包，
  会得到"头在、符号不在"的包 —— 那正是 §6 里那条实测要说的事。

## 3. ABI 契约

### 3.1 错误码

所有返回 `int` 的导出函数遵守同一条：**0 或正数 = 成功，负数 = 失败**。失败码
只有两个来源，混在同一条 `int` 上：

| 段 | 值 | 出处 |
|---|---|---|
| 本层的码 | `-20001` … `-20010` | `enum uvcpp_c_error`，见 `include/capi/uvcpp_c_common.h` |
| libuv 的码 | `-errno` 与 `-4095` 那一带（`UV_EOF`、`UV_EAI_*`…） | **原样透传**，不翻译 |

**`-20010`（`UVCPP_C_E_NOT_FOUND`）是 1.4.3 加的**，它解决的是"查不到"与"查到
空值"必须分得开这件事：`uvcpp_c_http_response_header(resp, "X-Foo", …)` 返回
负数 = 这次响应里**没有这个头**；返回 `0` = 有这个头、值是空串。合并成一个答案
的话，调用方就没法把"服务端没给 Cache-Control"与"服务端给了 Cache-Control
但值是空的"分开。它落在枚举**末尾**，所以老客户端不受影响（见 §3.5 那条规矩）。

两段不可能撞（libuv 最负的一个是 `UV_UNKNOWN` = -4096），所以
`uvcpp_c_strerror(err)` 能一句判出"这是谁家的码"：本层那十格给中文/英文文案，
落在 `-4096 … -1` 这一段的交给 `uv_strerror_r()`（**线程局部**缓冲，不是
`uv_strerror()` 那个进程共享的静态缓冲 —— 这一层就是给多线程的 FFI 用的）。
两段都不是时返回 `"unknown error code"`。

`uvcpp_c_last_error_string()` 是另一件事：它是**最近一次 C++ 异常**的 `what()`
（线程局部），只在某次调用返回了 `UVCPP_C_E_EXCEPTION` 之后有意义。先看返回码，
再看这句话。

### 3.2 回调表：以 `uint32_t size` 打头，逐格看

所有"一组回调"的参数都是一张**版本化结构体**，第一格是 `uint32_t size`：

```c
/* 这一段是完整翻译单元：它由 `tests/tools/check_doc_snippets.py` 用 **C 编译器**
 * （`-std=c99`）编，不是 C++。 */
#include <stddef.h>
#include <stdint.h>

#include <capi/uvcpp_c_net.h>

static void on_read(void* user_data, uvcpp_c_tcp_client* c,
                    const uvcpp_c_read_result* r) {
  (void)user_data;
  (void)c;
  if (r->event == UVCPP_C_READ_DATA && r->size > 0) {
    /* r->data 只在这次回调里有效 —— 要留就当场拷走 */
  }
}

void install(void* ctx, uvcpp_c_tcp_client* c) {
  uvcpp_c_tcp_client_events ev = {0};
  ev.size = (uint32_t)sizeof(ev);   /* ← 这一格是承重的 */
  ev.on_read = on_read;
  ev.on_read_user_data = ctx;
  (void)uvcpp_c_tcp_client_set_events(c, &ev);
}
```

本层**只读 `size` 覆盖到了的那几格**，不把整张表读满。差别在于老客户端：1.4.2
编出来的表有 4 格，将来 1.5.0 的表有 6 格，那个老客户端传进来的 `size` 只到第 4
格 —— "读满"会去读它**根本没有的字节**（那是调用方栈上的别的东西，取值随机），
"逐格"则干净地当"第 5、6 格没给"。这就是"以后加字段不破坏已编好的客户端"从
口号变成机制的那一粒。

两条边界：

- `size` 连自己那一格都没盖住（`< sizeof(uint32_t)`）⇒ `UVCPP_C_E_INVALID_ARG`，
  整张表都不读。一张连自己多大都说不清的表，没有一格是可信的。
- 某一格是 `NULL` = "这个回调我不关心"，那一次就不回调。**不是错误。**
- `size` 比本层知道的还大也合法（那是"你比我新"），多余部分忽略。

`size` 这一格的**类型也是 ABI 的一部分**：写 `uint16_t size` 的调用方会让本层
读它没有声明的字节（`tests/capi/capi_common_func.c` 里有一条被改过的用例专门
记着这件事）。

### 3.3 所有权：三类，每个函数各自属哪类都写在头里

| 类 | 例子 | 规矩 |
|---|---|---|
| **立刻拷贝** | `uvcpp_c_tcp_client_write(c, data, len, ...)` 的 `data` | 函数返回后调用方随便处置它 |
| **只在这次回调里有效** | `uvcpp_c_read_result::data`、回调参数里的句柄 | 要留就当场拷走 / 换成别的句柄 |
| **必须显式释放** | `uvcpp_c_tcp_client_new()` / `*_free()` | 谁建谁废；废过之后再用给 `E_STALE` |

**这一层不返回分配的内存** —— 所以没有 `uvcpp_c_free` 这种东西。凡是要取出一段
变长字符串的函数，签名都是 `int uvcpp_c_xxx(句柄, char* buf, size_t cap)`：
返回字符串真实长度（不含结尾 NUL），只有 `buf != NULL && cap >= 长度 + 1` 时才
写、并补 NUL，否则一个字节都不写（于是"先 `cap = 0` 问长度、再开缓冲"这条路是
通的）。写不下时返回 `UVCPP_C_E_BUFFER_TOO_SMALL`。

唯一例外是 `uvcpp_c_version_string()` / `uvcpp_c_strerror()` /
`uvcpp_c_last_error_string()` 这三个：它们返回**静态或线程局部的常量串**，
不要释放，但"_下次调用就可能被改写"这件事只对最后那个成立（见头里各自的注释）。

### 3.4 线程规则

与 C++ 侧一致：**一个句柄只能在那条事件循环的线程上用**。违者返回
`UVCPP_C_E_WRONG_THREAD`，不是 UB。

- "循环线程"是**跑这条循环的那个线程**，不是"建对象的那个线程" —— 所以它由
  `*_run()` 记下；`run()` 之前不检查（那一刻还没有"循环线程"可言）。
- 服务端交出来的那条连接句柄，与它的服务端**共享**同一份线程记录。
- 这一层是给**多线程**的 FFI 用的：两条循环线程各用各的句柄是日常用法，本层
  内部有锁保护的是"另一个线程正在释放"那一瞬间，**不是**"你可以跨线程用同一个
  句柄"。

### 3.5 ABI 版本与兼容承诺

- `UVCPP_C_ABI_VERSION`（头里的宏）是这一层的**承诺**；`uvcpp_c_version_string()`
  说的那个 `1.4.x` 是**版本号**。两件事。
- `uvcpp_c_abi_version()` 返回库里编出来的那个数。**任何 ABI 可见的改变都 +1**：
  删函数、改函数签名、改结构体里已有字段的类型或顺序、改错误码的数值。
  在尾部**加**字段不算（§3.2 的机制就是为它存在的）。
- 客户端应当在初始化时比一次 `uvcpp_c_abi_version() == UVCPP_C_ABI_VERSION`，
  不等就拒绝启动。P/Invoke 最常见的故障（头与 `.so`/`.dll` 不是一次编出来的）
  会在这一步变成一条能读懂的失败，而不是某个函数调用点上的访问违例。
- `tests/tools/check_capi_symbols.py` 把**实际导出的** `uvcpp_c_*` 符号与
  `tests/tools/capi_symbols.lock` 逐条比。删一个/改一个名字 ⇒ 门禁红，而不是
  已经编好的 C# 侧在运行时抛 `EntryPointNotFoundException`。
- **那份锁是分片的**（批 3a 改的，`#@ module <名>` 一行开一片），因为一条腿
  只开得起一部分模块：CI 那条 `capi` 腿的 flags 里**没有** `NGHTTP2` / `QUIC` /
  `HTTP3`，那几片符号在它那棵树上**本来就不该导出**。平的锁会把"这一片这棵树
  没有"和"这个符号被删了"报成同一件事 —— 一个很有说服力的**假红**。
  所以门禁按**每片自己的开关**（从这棵树的 `uvcpp_config.h` 里读）逐片判：
  开着 → 这一片与导出面逐条相等；关着 → 这一片要求**一个都不许出现**，并如实
  印一行「未判」。**「未判」不是「通过」**，它是要拿另一条腿去补的账。
- **没有哪一条腿开得起全部七片**，所以门禁在 ubuntu 上挂**三条**腿，合起来把
  七片都盖上（每片至少被一条真的量过一次）：

  | 腿（`ci-linux-ubuntu.yml`） | 它带的开关 | 判了哪几片 |
  |---|---|---|
  | `capi` | CAPI + WEBAPP，**没有** SSL/h2/quic | common / net / web / webapp 四片；h2 / quic / http3 三片印「未判」|
  | `h2` | 再加 `NGHTTP2` | 再加 h2 片，共五片 |
  | `http3` | 再加 `QUIC` + `HTTP3`（**没有** WEBAPP）| 再加 quic / http3 两片，共六片；webapp 那一片按"关着的模块一个都不许导出"判，印「未判」|

  这条腿的选择本身也是判据的一部分：`quic` 那几片**不是**在 `capi` 腿上假装
  判过的，`webapp` 那一片也**不是**在 `http3` 腿上假装判过的。本机对应的三棵树
  叫 `build-capi` / `build-capi-h2` / `build-capi-h3`（§6），另有一棵
  `build-capi-all` 把七片一次开齐。

**批 2（1.4.3）为什么没有把 `UVCPP_C_ABI_VERSION` 从 1 抬到 2**，以及这条判断
是怎么**量**出来的，不是"我们觉得是加面"：

- 判据是 **`git diff` 里批 1 那 35 个符号一个签名都没动**。这一批对既有文件的
  改动只有四处：`uvcpp_c_common.h` 往枚举末尾加 `-20010`（规矩里写着"往枚举
  末尾加值不算"）、`uvcpp_c_internal.h` 加内部件、`uvcpp_c_net.cpp` 把文件-local
  的 `copy_out()` 收进 `detail` 命名空间（它是匿名命名空间里的静态符号，本来
  就不在导出面上）、以及伞头多两个 `#include`。**没有一条落在"删函数、改签名、
  改结构体已有字段"这三类里。**
- 反过来说，老客户端（按头 1 编出来的）拿到这份新库，它调的那 35 个函数的
  行为逐条不变；新加的那 148 个它看不见。`check_capi_symbols.py` 的两个方向也
  照旧：删一个 ⇒ 判据 2 报"承诺过、库里没了"。
- **批 3a（`1.4.3`）加的 `uvcpp_c_http2.h` 是 46 个**：`git diff` 里批 1+2 那 183 个
  符号同样一个签名都没动，所以 `UVCPP_C_ABI_VERSION` 仍是 **1**。同一条推论照用；
  只要动到既有那 183 个里的任何一个，就 +1。
- **批 3b（`1.4.4`）加的 quic + http3 两片是 92 个**（quic 42 + http3 50），
  `UVCPP_C_ABI_VERSION` 仍是 **1**，理由是同一句话：既有那 229 个符号一个签名
  都没动（`git diff -- src/capi/uvcpp_c_{common,net,web,webapp,http2}.{h,cpp}`
  里没有一处动到已声明的函数）。**批 3b 确实动过一次 `uvcpp_c_http3.h` 的
  接口面**（补上 `uvcpp_c_h3_response_set_stream_id()`）—— 但那一份头是这一批
  新加的、从未发布过，所以那是"把没写完的东西写完"，不是"改了一个承诺"。

## 4. 提供什么、明确不提供什么

到 1.4.4 为止共 **321 个函数**（`tests/tools/capi_symbols.lock` 就是这份名单，
**分片**记着，每片对着一份头；七片：5 + 30 + 29 + 119 + 46 + 42 + 50）。
**名字都是 `uvcpp_c_` 前缀。**

| 头 | 提供 | 不提供 |
|---|---|---|
| `capi/uvcpp_c_common.h`（5 个） | `abi_version` / `version_string` / `strerror` / `last_error_string` / `live_handle_count`；错误码表；导出宏 `UVCPP_C_API` | 日志级别、内存分配器入口 —— 这一层不返回分配的内存（§3.3） |
| `capi/uvcpp_c_net.h`（客户端 17 个） | `new` / `free` / `set_events` / `connect` / `connect_wait` / `write` / `write_wait` / `read_pause` / `read_resume` / `read_stop` / `close` / `run` / `stop` / `is_connected` / `last_error` / `is_tls` / `alpn_selected` | TLS **参数**入口（证书、私钥、SNI、校验开关）—— 那是 C++ 的 `uvcpp_ssl_context`，C 面没有对应类型；DNS 解析（与 C++ 侧一致，只收 IPv4/IPv6 串） |
| `capi/uvcpp_c_net.h`（服务端 13 个） | `new` / `free` / `set_events` / `bind` / `local_port` / `listen` / `set_loops` / `loop_count` / `run` / `stop` / `client_count` / `close_all_clients` / `last_error` | 每连接独立的 accept 策略（回调里给句柄，动作自己定） |
| `capi/uvcpp_c_webapp.h`（119 个） | **app**：`new` / `free` / 一组 `set_*` 配置 / `get·post·put·del·patch·head·options·any` / `use` / `websocket` / `serve_static` / `post_upload` / `start` / `start_background` / `stop` / `join` / `bound_port` / `running` / 几个计数。**请求侧**：`req_*`（method / path / query / header / cookie / body / keep-alive / peer）。**响应侧**：`resp_*`（status / header / text / html / json_str / binary / send_file / redirect / 4xx 5xx 快捷 / `begin_chunked` + `write_chunk` / `on_sent` / `on_drain`）。**流程**：`next_run` / `defer` + `deferred_*`。**ws**：`ws_req_*`（升级期）与 `ws_conn_*`（连接期） | 任何要 C++ 类型的入口（见下面那段）；WSS（`enable_wss` 要 SSL 上下文）；**服务端主动推送 / 广播**：不行 —— 那需要一个活过回调的连接句柄，而 `ws_conn` 是回调期句柄 |
| `capi/uvcpp_c_web.h`（29 个） | **HTTP 客户端**：`new` / `free` / `set_keep_alive` / `connect` / `get` / `post` / `run` / `stop` / `close` / `is_connected` / `last_error`；响应侧 `http_response_*`（status / header / content-type / body）。**WS 客户端**：`new` / `free` / `set_events` / `connect` / `send_text` / `send_binary` / `close` / `run` / `stop` / `session_count` / `is_open` / `last_error` | **服务端那一侧全都不在这里**（http_server / ws_server / ws_connection 走 webapp 那份头）。HTTP 客户端的流式响应体（`on_body` 逐块）不给 —— C 面只在响应回调里给完整 body。`wss://` 不给（同上，要 SSL 上下文） |
| `capi/uvcpp_c_http2.h`（46 个） | **连接**：`connection_new` / `free` / `start` / `flush` / `shutdown` / `close_now` / `closed` / `closing` / `last_error` / `stream_count` / `bytes_in` / `bytes_out` / `pause_stream` / `resume_stream` / `begin_goaway` / `submit_goaway` / `submit_rst` / `submit_request` / `peer_goaway_received` / `peer_goaway_error_code` / `peer_goaway_last_stream_id`。**服务端应答**：`send_status` / `send_headers` / `send_response` / `send_data`。**构造器**（调用方建、调用方废）：`h2_request_new` / `set_header` / `set_body` / `free`、`h2_response_new` / `set_status` / `set_header` / `set_content_type` / `set_body` / `free`。**流视图**（回调期句柄）：`id` / `state` / `paused` / `rejected` / `body_bytes` / `expected_body` / `request_method_name` / `request_path` / `request_header` / `request_has_header` / `response_status` / `response_header` | `uvcpp_h2_session` 这个**独立句柄**（那会要求 C 侧自己写 socket；而且两个句柄指向同一份内部状态时，"先 free 哪个"就成了第二份真相）；`recv` / `drain` / `want_read` / `want_write`（传输层的事，驱动层自己做）；**优先级 / 依赖 / push**（`uvcpp_h2_session` 本来就没有这几项）；`on_begin_headers` / `on_frame_recv` 那类**逐帧**回调；`session().take_completed()`（驱动层自己在写完成路径上跑它，使用方没有插手的余地） |
| `capi/uvcpp_c_quic.h`（42 个） | **进程级**：`crypto_init` / `crypto_free` / `ngtcp2_version` / `error_string`。**TLS**（调用方建、调用方废）：`tls_client_new` / `tls_server_new` / `tls_server_selfsigned` / `set_ca_file` / `set_verify` / `free`。**客户端**：`new` / `connect` / `connection`（借来的连接句柄）/ `set_tls` / `set_alpn_protos` / `set_idle_timeout` / `run` / `run_once` / `stop` / `close` / `free`。**服务端**：`new` / `set_tls` / `set_alpn_protos` / `set_idle_timeout` / `bind` / `configured_ip` / `configured_port` / `listen` / `run` / `run_once` / `stop` / `free`。**连接**（借来的）：`set_callbacks` / `state` / `alpn_selected` / `open_stream` / `write_stream` / `shutdown_stream` / `shutdown_stream_read` / `streams_left` / `close` | 一条流上的**读回调寄存器**（`on_read` 那些走 `_conn_set_callbacks` 那张表，不单开函数）；**每个连接一枚 `*_free()`** —— 借来的句柄没有 `free`（§3.3 第三类）；明文模式（QUIC 没有这回事：不配 TLS 的 `listen` / `connect` 直接失败）；证书/私钥**逐项**入口（服务端只有 `_server_new(证书, 私钥)` 与 `_server_selfsigned()` 两个入口，客户端只吃 CA 文件与一个校验开关）；0-RTT / 迁移 / 版本协商的旋钮 |
| `capi/uvcpp_c_http3.h`（50 个） | **连接**：`connection_new`（接在一枚借来的 QUIC 连接句柄上）/ `free`（**持有者销毁**，见文件头第 2 条）/ `start` / `send_request` / `take_completed` / `completed_count` / `send_response` / `send_status` / `flush` / `close` / `ready` / `closed` / `server_side` / `bytes_in` / `bytes_out` / `alpn_selected` / 三条关键流各自的流号（`_control_stream_id` / `_qpack_encoder_stream_id` / `_qpack_decoder_stream_id`，还没开出来时 -1）；另有进程级的 `h3_version`（nghttp3 的版本串）。**构造器**（调用方建、调用方废）：`h3_request_new` / `set_scheme` / `set_authority` / `set_header` / `set_body` / `free`，`h3_response_new` / `set_status` / `set_stream_id` / `set_header` / `set_content_type` / `set_body` / `reset` / `free`，加它们的读侧（`status` / `stream_id` / `body` / `body_size` / `error` / `header` / `has_header`）。**请求视图**（回调期句柄）：`method` / `path` / `scheme` / `authority` / `header` / `has_header` / `body` / `body_size` / `stream_id` | **trailers**（两边都不给：请求侧与响应侧都没有）、**server push**（与 `doc/http3-guide.md` 的"真实现"表一致）；**客户端侧的响应回调** —— 客户端拿响应走 `_take_completed()` + 那枚 `h3_response` 的读侧，没有第四条 `on_response` 吊桥；`uvcpp_h3_connection` 那层的 `on_stream_close` **只给收尾信息**（流号 + 四个错误位），逐帧的 `on_frame_*` 不给 |
| `capi/uvcpp_c.h` | 伞头：按各模块宏 include 上面几份 | 任何 C++ 类型、任何 libuv 类型（§1 第 2 条） |

**`ws_client` 那一族的取舍要单独讲一句**：它**不给连接句柄**（收发都从客户端对象
走）。理由与"服务端不许广播"是同一条 —— 连接句柄要活过回调，而这一层承诺不了
那个生命周期。C# 侧若确实要"按连接"做事，走 webapp 那侧的 ws 路由（升级期有
`ws_req`，连接期有 `ws_conn`，动作在那两个回调里做完）。

不提供的那几件事都有**明确的原因**，不是"还没做"：C 头里出现
`uvcpp_ssl_context` 就等于要求调用方理解 C++ 的对象生命周期；而"参数入口"在
C# 侧本来就是一个 `P/Invoke` 到别处的字符串转换。

**`live_handle_count()` 是唯一一个能看见内部状态的口子**，加它的理由不是"方便
调试"：它是"句柄登记表收支平衡"这件事**唯一**可被外部量到的形式，而那条平衡是
`alive()` 前半段的前提（详见 `include/capi/uvcpp_c_common.h` 里那段注释，以及
§6 里那条变异表）。C# 侧拿它做泄漏断言也很直接：跑完一轮之后它该回到 0。

## 5. 从 C# P/Invoke 用

这一节只列**会真出事**的那几条。

1. **委托必须自己保活。** `set_events` 收的是一个函数指针；C# 那边是一个
   `Marshal.GetFunctionPointerForDelegate` 出来的 `IntPtr`，而被包装的
   `delegate` 对象**GC 看不见这条引用** —— 委托被回收之后，回调指针指向一块
   已经被复用的内存。常见做法是把委托存进一个 `static` 字段或
   `GCHandle.Alloc(d, GCHandleType.Normal)`，在 `*_free()` 之后再释放。
2. **`user_data` 用一个 `GCHandle`。** `GCHandle.ToIntPtr(GCHandle.Alloc(ctx))`
   传给 `*_user_data` 那几格，回调里再 `GCHandle.FromIntPtr` 取回来；
   收尾时**必须** `Free()`，否则那个托管对象永远不回收（它与 §4 说的
   `live_handle_count()` 是两件不同的事：后者数的是 C 句柄，前者是托管对象）。
3. **句柄是 `IntPtr`，别去解引用。** 头里给的是不完整类型，C# 侧照抄成
   `IntPtr` 就行。**不要**自己声明一个同名的 `struct` 去 `Marshal.PtrToStructure`
   —— 布局没有承诺。
4. **字符编码写死 `CharSet.Ansi`**（这一层收 `const char*`，按字节原样用，
   自己不做任何编码转换）。路径、JSON、body 都由调用方决定编码。
5. **`*_wait()` 那一族会阻塞**，只能在循环还没跑的时候用；在回调里调它们会把
   循环卡死（`connect_wait` 甚至会**抛异常**，见 §6 那条异常边界的用例）。
6. **`size` 那一格给 `(uint32_t)Marshal.SizeOf<T>()`**，并且你的 `StructLayout`
   要与头里的字段顺序逐格对齐（含每个 user_data 那一格）。
7. **回调必须立刻返回。** 它们跑在循环线程上，而这层是 `uv_read_start` 的热路
   —— 在回调里做重活等于把整条循环卡住。

## 6. 怎么本地验证

```bash
# 配一棵 CAPI=ON 的树（§2 那条命令）
#
# ★ 动过 `src/uvcpp/uvcpp_version.h` 之后要**重跑一次 configure**（`cmake -S . -B
#   build-capi`，不需要重给选项，缓存在）：`test_capi_common_func` 比的那个版本前缀
#   是 configure 期从那个头里抓出来、以 -D 传进用例的，不重配就是"库说 1.4.3、用例
#   还拿着 1.4.2"，于是**基线**整个红掉，看起来像 C 层坏了。这条是踩出来的。
cmake --build build-capi -j"$(nproc)"

# ① C 层自己的用例（三个都是**纯 C**编的）
ctest --test-dir build-capi -R capi --output-on-failure
#   test_capi_common_func：地基（ABI 自洽、错误码文案、句柄生死与类型、
#                          活句柄数收支平衡、事件表 size 规则）
#   test_capi_net_func   ：net 端到端（真起两端、真收字节的回显、线程纪律、
#                          异常不过边界）
#   test_capi_webapp_func：webapp + web 端到端（**C 写的客户端打 C 写的
#                          服务端**：七条 HTTP 走同一条 keep-alive 连接，
#                          跨线程的延迟应答，中间件次序，静态目录，两种 WS
#                          关闭方式，回调期句柄越界，登记表收支平衡）
#   test_capi_h2_func    ：http2 端到端（**这条在 build-capi 里不存在** ——
#                          它跟着 `UVCPP_ENABLE_NGHTTP2` 走，见下面 ①b）

# ①b HTTP/2 那一份要另配一棵树：`src/capi/uvcpp_c_http2.*` 与它那条用例
#    都挂在 NGHTTP2 那个开关下（顶层 `UVCPP_CAPI_DISABLED` 表 +
#    `tests/capi/CMakeLists.txt`），而上面那棵 build-capi 是**故意**不带
#    OpenSSL 的（那是 CI 的 capi 格的样子）。这条树与 CI 的 `h2` 格逐字同构：
cmake -S . -B build-capi-h2 -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_BUILD_TESTS=ON -DUVCPP_BUILD_SHARED=ON \
  -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_ZLIB=ON \
  -DUVCPP_ENABLE_NGHTTP2=ON -DUVCPP_ENABLE_CAPI=ON \
  -DFETCHCONTENT_SOURCE_DIR_LIBUV=$PWD/_local_deps/libuv
cmake --build build-capi-h2 -j"$(nproc)"
ctest --test-dir build-capi-h2 -R capi --output-on-failure
#   test_capi_h2_func（244 条）：纯 C 的服务端与纯 C 的客户端在**一条**连接上
#   跑三条流（POST 带 body / GET / 流式 GET），状态码与 body 逐字节比，
#   `expected_body()` 的两条路各自断言，跨线程 `flush` 必须 `E_WRONG_THREAD`，
#   收尾核对登记表收支平衡（回调期句柄摘干净了没有 —— 这是那件事**唯一**的
#   可观测形式，见下面 M8/M16 那两段）

# ①c quic + h3 那一份同样要另配一棵树：`src/capi/uvcpp_c_quic.*` 与
#     `uvcpp_c_http3.*` 分别挂在 QUIC / HTTP3 两个开关下，用例跟着 HTTP3 走
#     （守卫链保证 HTTP3 ⇒ QUIC）。它与 CI 的 `http3` 格逐字同构 —— 注意
#     **没有 `-DUVCPP_BUILD_WEBAPP=ON`**，与 CI 那一格一样。
#     OpenSSL 要用一份带 QUIC API（`SSL_set_quic_tls_cbs`）的：3.5 起才有。
cmake -S . -B build-capi-h3 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DUVCPP_BUILD_TESTS=ON -DUVCPP_BUILD_SHARED=ON \
  -DUVCPP_BUILD_NET=ON -DUVCPP_BUILD_WEB=ON \
  -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_ZLIB=ON \
  -DUVCPP_ENABLE_QUIC=ON -DUVCPP_ENABLE_HTTP3=ON -DUVCPP_ENABLE_CAPI=ON \
  -DOPENSSL_ROOT_DIR=$PWD/_local_deps/openssl-3.5-inst \
  -DFETCHCONTENT_SOURCE_DIR_LIBUV=$PWD/_local_deps/libuv \
  -DFETCHCONTENT_SOURCE_DIR_NGTCP2=$PWD/_local_deps/ngtcp2 \
  -DFETCHCONTENT_SOURCE_DIR_NGHTTP3=$PWD/_local_deps/nghttp3
cmake --build build-capi-h3 -j"$(nproc)"
ctest --test-dir build-capi-h3 -R capi --output-on-failure
#   test_capi_quic_h3_func（280 条）：纯 C 的两端在**一条 UDP 上**握手（ALPN
#   `h3`）后跑三条请求（POST 带 body + 自定义头 / GET + `_send_status` /
#   GET + 204 空体），响应体逐字节比，`_take_completed()` 恰好一次、
#   `on_disconnect` 恰好一次，借来的 QUIC 连接句柄在连接关掉后一律 `E_STALE`，
#   收尾核对登记表收支平衡 —— **这一句是批 3b 最要紧的判据**（见下面 M18）

# ①d 想一棵树把七片全开齐（webapp + h2 + quic + h3 + CAPI 同时在），用
#     `build-capi-all`：它是**整张变异表**那一趟要的树（M8/M10 要 webapp 的
#     用例、M13–M16 要 h2 的、M17–M20 要 quic+h3 的，而 CI 那三格各缺一块）。
cmake -S . -B build-capi-all -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DUVCPP_BUILD_TESTS=ON -DUVCPP_BUILD_FUNCTIONAL=ON -DUVCPP_BUILD_SHARED=ON \
  -DUVCPP_BUILD_NET=ON -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_ZLIB=ON -DUVCPP_ENABLE_CAPI=ON \
  -DUVCPP_ENABLE_NGHTTP2=ON -DUVCPP_ENABLE_QUIC=ON -DUVCPP_ENABLE_HTTP3=ON \
  -DOPENSSL_ROOT_DIR=$PWD/_local_deps/openssl-3.5-inst \
  -DFETCHCONTENT_SOURCE_DIR_LIBUV=$PWD/_local_deps/libuv \
  -DFETCHCONTENT_SOURCE_DIR_NGHTTP2=$PWD/_local_deps/nghttp2 \
  -DFETCHCONTENT_SOURCE_DIR_NGTCP2=$PWD/_local_deps/ngtcp2 \
  -DFETCHCONTENT_SOURCE_DIR_NGHTTP3=$PWD/_local_deps/nghttp3
cmake --build build-capi-all -j"$(nproc)"

# ② 头**真是 C 的**，不是"看起来像 C"：每一份都过一个 C 编译器
#
# 形状是"写一个只 include 它的 .c"，**不是**把头当主文件喂进去 ——
# `gcc ... uvcpp_c_net.h` 在 `-Werror` 下会红在 `#pragma once in main file` 上，
# 那是调用的毛病，不是头的问题。
#
# `uvcpp_c_internal.h` **不在**这一轮里：它是内部头（CMake 与
# `package_release.py` 两道过滤器都不装它），C 去 include 它本就该失败。
for h in src/capi/uvcpp_c_common.h src/capi/uvcpp_c_net.h \
         src/capi/uvcpp_c_web.h src/capi/uvcpp_c_webapp.h \
         src/capi/uvcpp_c_http2.h src/capi/uvcpp_c_quic.h \
         src/capi/uvcpp_c_http3.h; do
  printf '#include "%s"\n' "${h#src/}" > /tmp/probe.c
  gcc -x c -std=c99 -pedantic-errors -Wall -Wextra -Werror \
      -fsyntax-only -I src /tmp/probe.c || echo "不是纯 C: $h"
done
# 伞头照**装出去的样子**编：`uvcpp_config.h` 是 configure 期生成的，只在构建树里
printf '#include <capi/uvcpp_c.h>\n' > /tmp/probe.c
gcc -x c -std=c99 -pedantic-errors -Wall -Wextra -Werror -fsyntax-only \
  -I src -I build-capi/include /tmp/probe.c || echo "伞头不是纯 C"
# 伞头那一份用**哪个树的 config** 有讲究：`<树>/include/uvcpp/uvcpp_config.h`
# 是 configure 期生成的，伞头按它里面那几枚宏决定 include 哪几份头。所以
# `-I build-capi/include` 只能验到"这一格开着的那些"；要把 h2 / quic / h3 也验
# 一遍就换成 `-I build-capi-h2/include` / `-I build-capi-h3/include`（或直接像
# 上面那样逐份 include —— 一份头不依赖伞头的宏，单独编它反而是更硬的一条）。
#
# ★ 这一轮②当场抓到一个**真缺陷**（批 3a 留下的，1.4.4 修掉）：
#   `uvcpp_c_http2.h` 的参数表里写着 `struct uvcpp_c_tcp_client*`，却没有
#   include 那个类型的所有者 `capi/uvcpp_c_net.h`。伞头里**编得过** ——
#   `uvcpp_c.h` 按顺序先 include 了 net 那份；**单独 include 本头就红**，因为
#   参数表里第一次出现的 tag，C 会当场新造一个只属于这条原型的类型，`-Werror`
#   下就是一条 `-Wvisibility`。修法只有一处：补那一份 include（`uvcpp_c_quic.h`
#   本来就有，形状照它）。
#   这件事的意义不在于"漏了一个 include"，而在于它**只有②量得出来**：伞头
#   的 include 顺序会把这类毛病整个盖住，而消费者"只 include 我要的那一份"是
#   完全正当的用法。所以②不是走过场。

# ③ 反空转：把承重的守卫逐条拆掉，看用例有没有一声响
#   1.4.4 起是二十条（M1–M20）—— M13–M16 是批 3a 的 http2 那四条，M17–M20 是
#   批 3b 的 quic + h3 那四条。单看一条用 --only，省掉整表重跑：
#     python3 tests/tools/capi_mutation.py --tree build-capi-all --only M15
#   ★ 整表要用 **build-capi-all** 那棵树（①d）：M8/M10 要 webapp 的用例、
#     M13–M16 要 h2 的、M17–M20 要 quic+h3 的，而 CI 那三格**各缺一块**。
#     拿缺了一块的树跑整表（比如 build-capi-h3）脚本会**退 3**，并且点名是
#     哪几条变异没有判据 —— 它不肯拿 `run_targets()` 给的 127 去冒充"抓住了"，
#     那种"没判"不许长得像"判过了"。只跑某一段就没事：
#     python3 tests/tools/capi_mutation.py --tree build-capi-h3 --only M17
python3 tests/tools/capi_mutation.py --tree build-capi-all

# ④ 符号面锁。**三棵树各跑一次**：锁是分片的，每片要一条开着那个模块的腿来判
#   （与 CI 那三条腿一一对应，见 §3.5）。
#   build-capi    → common / net / web / webapp 逐条对上，h2 / quic / h3 印「未判」
#   build-capi-h2 → 再加 h2 一片，共五片
#   build-capi-h3 → 再加 quic / http3 两片，共六片（webapp 那一片按"关着的模块
#                   一个都不许导出"判，印「未判」）
python3 tests/tools/check_capi_symbols.py --tree build-capi
python3 tests/tools/check_capi_symbols.py --tree build-capi-h2
python3 tests/tools/check_capi_symbols.py --tree build-capi-h3

# ⑤ 守卫链的反例（两条都应当**配置成功**、打 warning、把模块排除掉）
cmake -S . -B /tmp/capi-bad \
  -DUVCPP_ENABLE_CAPI=ON -DUVCPP_BUILD_WEB=OFF 2>&1 | grep -i capi
```

⑥ **这一层要按 CI 那格的样子再编一次 —— 不带 OpenSSL。**

```bash
cmake -S . -B build-capi-ci -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_BUILD_TESTS=ON -DUVCPP_BUILD_SHARED=ON \
  -DUVCPP_BUILD_NET=ON -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_ENABLE_ZLIB=ON -DUVCPP_ENABLE_CAPI=ON      # 注意：没有 ENABLE_OPENSSL
cmake --build build-capi-ci -j"$(nproc)" && ctest --test-dir build-capi-ci -R capi
```

**这不是"再跑一遍确认"，它抓的是另一类错。** §2 那条命令带着
`-DUVCPP_ENABLE_OPENSSL=ON`，于是 C 层实现里凡是调了**只在某个特性下存在**的
C++ 成员的写法，在本机都是绿的 —— 而 CI 的 capi 格是**故意不带 OpenSSL** 的
（理由：C 面这一批一个加密入口都没有，开了只会让这一格的失败原因变多），
于是同一种写法在那三格上全都编不过。1.4.2 第一次推送就是这么红的：
Linux / macOS / MSVC 三格红在 `uvcpp_c_net.cpp` 那两句
`c->cli->is_tls()` / `c->cli->tls_alpn_selected()` 上，而 MinGW64 那格
**带** OpenSSL，绿。四腿三红一绿，差别只有这一处。

所以判据是：**C 层能不能编出来，不许取决于一个 C 面够不着的特性。**
本层的做法是同一个函数在两种构建里给出同一个答案（`is_tls()` 恒 0、
`alpn_selected()` 恒空串 —— 这一批没有给 C 的 TLS 入口，所以那就是实话），
而不是拿 `#if` 把两边的行为编成两样。

### 变异表量出来的

到 1.4.4 为止共二十条（一条基线 + M1–M20），判据是"实际结果与**先写下来的预期**一致、
源码按字节还原、还原后五个用例复跑全绿"。**整表要在 `build-capi-all` 那棵树上跑**
（七片全开 + CAPI：M1–M12 要在 webapp / net 的用例里判、M13–M16 要 h2 的、
M17–M20 要 quic + h3 的，而 CI 那三格**各缺一块**；选了某个模块的变异、手里却没有
编着它的那棵树时，脚本**退 3**，不拿 127 冒充"抓住了"）。

最近一次整表（2026-09-30，Linux / gcc，`--tree build-capi-all --jobs 8`）：
**基线 5 个用例全绿**（`common 167/0`、`net 74/0`、`webapp 210/0`、`h2 244/0`、
`quic_h3 280/0`），二十条的实际结果与预期**逐条一致**，`EXIT=0`。

批 1（M1–M7）实测（当时树为 `build-capi`）：

| # | 拆掉什么 | 预期 | 实得 | 谁红的 |
|---|---|---|---|---|
| M1 | 只拆 `poison_head()` | 没抓住 | **没抓住**（如设计） | —— |
| M2 | 只拆 `registry_remove()` | 抓住 | 抓住 | `test_capi_common_func`（活句柄数那条断言） |
| M3 | 两道一起拆 | 抓住 | 抓住 | 同上 |
| M4 | 回调表的 `field_present()` 恒真 | 抓住 | 抓住 | `test_capi_net_func`（截断表：5 条断言同时红） |
| M5 | `alive()` 不看魔数 | 抓住 | 抓住 | `test_capi_common_func`（4 条） |
| M6 | `connect_wait` 去掉异常收口 | 抓住 | 抓住 | 进程 `SIGABRT`（`rc=-6`，没有 `checks=` 行） |
| M7 | `abi_version()` 与头里的宏不一致 | 抓住 | 抓住 | `test_capi_common_func`（`= 2, want 1`） |

M1、M2、M3 是第一版**全都没抓住**的三条，下面那段记的就是这件事。

批 2（1.4.3）加的五条，全部由新的 `test_capi_webapp_func` 抓住：

| # | 拆掉什么 | 预期 | 实得 | 谁红的（哪一条断言） |
|---|---|---|---|---|
| M8 | `FrameScope::~FrameScope()` 那句"全部摘表" | 抓住 | 抓住 | `test_capi_webapp_func` —— 但**只**红在 `uvcpp_c_live_handle_count() = 5, want 0`（下面单开一段） |
| M9 | `resp_get_header()` 查不到时退化成 `0` | 抓住 | 抓住 | 同上（7 条 `= 0, want -20010`）—— 这条也把 `UVCPP_C_E_NOT_FOUND` 这个新码钉住了 |
| M10 | `serve_static()` 不查空参 | 抓住 | 抓住 | 同上（`= -20003, want -20001`：一路走到运行时才炸成异常码） |
| M11 | `http_client_is_connected()` 直接返回 1 | 抓住 | 抓住 | 同上（`= 1, want 0`）—— "关掉之后不许再自称连着" |
| M12 | `ws_client_connect()` 不查 `ws://` | 抓住 | 抓住 | 同上（`= -20003, want -20001`） |

M10 / M12 红出来的**码是 `-20003`（异常）而不是 `-20001`（参数）**，这是有信息量的：
少了那句入参检查，错误就推迟到深处、以"某处抛了异常"的形式出现。也就是说断言写的
`E_INVALID_ARG` 量的不是"没崩"，而是"**在边界上**就被挡住了"。

批 3a（1.4.3）加的四条，全部由新的 `test_capi_h2_func` 抓住，四条都是**同一批规矩在
http2 那一侧的形态**：

| # | 拆掉什么 | 预期 | 实得 | 谁红的（哪一条断言） |
|---|---|---|---|---|
| M13 | `h2_conn_flush()` 的线程检查 | 抓住 | 抓住 | `test_capi_h2_func`（`cross_thread_flush_rc = 0, want -20008`） |
| M14 | `h2_stream_response_status()` 把"头还没到"当 `0` 返回 | 抓住 | 抓住 | 同上（6 条 `= 0, want -20010`：三条流 × 请求回调 + 收尾各一次） |
| M15 | `start()` 里 `conn` 表的 `size` 检查 | 抓住 | 抓住（**但改过用例**，见下） | 同上（`= 0, want -20001`，紧接着真 `start` 拿到 `-114`、GOAWAY 也没了） |
| M16 | h2 那份 `FrameScope` 的"出栈摘表" | 抓住 | 抓住 | 同上（`uvcpp_c_live_handle_count() = 7, want 0`） |

M15 与 M16 各自挖出一处**用例**的毛病，两处都不是守卫坏了 —— 见下面那一段。M16 与 M8
是同一件事在另一份文件里的翻版，判据也一样只有活句柄数那条。

批 3b（1.4.4）加的四条，全部由新的 `test_capi_quic_h3_func` 抓住：

| # | 拆掉什么 | 预期 | 实得 | 谁红的（哪一条断言） |
|---|---|---|---|---|
| M17 | `uvcpp_c_quic_conn_set_callbacks()` 不查端点表的 `size` | 抓住 | 抓住 | `test_capi_quic_h3_func`（`g.q_bad_size_rc = 0, want -20001` 与 `g.c_q_bad_size_rc = 0, want -20001`：服务端那一次 + 客户端那一次）|
| M18 | h3 摘连接时 `quic_conn_detach(qconn)` 不毒化借来的 QUIC 句柄 | 抓住 | 抓住（**`rc=-11`**） | 同上 —— 进程在收尾前就 `SIGSEGV`、**没有 `checks=` 那一行**（见下）|
| M19 | h3 那份 `FrameScope` 的"出栈摘表" | 抓住 | 抓住 | 同上（`uvcpp_c_live_handle_count() = 2, want 0`）|
| M20 | `uvcpp_c_h3_response_set_stream_id()` 不拦负数 | 抓住 | 抓住 | 同上（`= 0, want -20001`：`-1` 是哨兵值，不是一条流）|

M17 与 M15 是同一件事在 QUIC 那一侧的翻版，**但判据的形状不同**：h2 那条能红在
"紧接着真 `start` 拿到 `-114`"，因为 h2 的连接表被接受了就会立刻用上；而 QUIC 的端点表
被接受之后**没有任何行为差异** —— 一张被截断的表本来就不会填那些格子，装没装上看起来
一模一样。所以这一条**只有返回码能当判据**，用例里也照这么写的（服务端在
`on_connection` 里喂一张 `size=3` 的、客户端在 `_connect()` 之前喂一张 `size=2` 的，
两处都断言 `-20001`）。

M19 与 M8 / M16 又是同一件事的第三份翻版，判据同样只有活句柄数。M18 值得单说一句：
它量的是"**谁负责把借来的句柄交还**"。装了 h3 之后，`uvcpp_c_quic.cpp` 那份 `on_close`
跳板已经被 h3 换掉了（那句注释写在 `quic_conn_detach()` 头上），所以拆掉 h3
`on_disconnect` 里那一句之后，那个 QUIC 连接句柄**再没有第二条路**能把登记摘掉 ——
用例在收尾时读到一枚悬垂的登记项，然后就走到了 `SIGSEGV`。这条**测得对**：预期是
"抓住"，实得是没有 `checks=` 行、`rc=-11` 的那种抓住。别把它当成"用例崩了要修"。

最后，换到 `build-capi-all` 跑整表之后，**前面几条的"谁红的"会多出新的用例**：M2 / M3
的摘表那两条现在还会红 `webapp`、`h2`、`quic_h3`（这三个用例里也有回调期句柄），
M5 的 `alive()` 不看魔数还会红 `h2`、`quic_h3`，M7 的 ABI 版本不一致还会红 `webapp`。
这不是判据变了，是同一条守卫被更多的用例踩到 —— 表里记的仍是**先写下来的那些预期**
命中与否，命中几个用例只是"这份守卫被几处在用"。

查单条变异不用整表重跑。注意树：每条变异只有编着它那个模块的树才判得了
（M8/M10 要 webapp、M13–M16 要 NGHTTP2、M17–M20 要 HTTP3），而 `build-capi-all`
是七片全开的那一棵，整表与单条都能跑：

```bash
python3 tests/tools/capi_mutation.py --tree build-capi-all --only M18
python3 tests/tools/capi_mutation.py --tree build-capi-h2  --only M13
python3 tests/tools/capi_mutation.py --tree build-capi-h3  --only M17
```

### 第一版的假绿，以及它量错了什么

`capi_mutation.py` 的七条里有三条**第一版没抓住**，而原因不在守卫，在用例 ——
这条值得抄下来，因为它是一类很常见的假绿：

- **M1（只拆毒化）、M2（只拆摘表）、M3（两道一起拆）原本全都没红。** 两道守卫是
  `unregister_head()` 里的 `registry_remove()` 与 `poison_head()`，而"释放之后再用
  必须给 `E_STALE`"那几条断言在三种情况下**都过** —— 因为 `alive()` 接下来要读的
  那 4 个字节（空闲块的第一个字节）在 glibc 上正好被 tcache 的 `next` 指针改写成
  一个不等于魔数的值。也就是说那几条断言量的是**分配器**，不是守卫。
- 补上 `uvcpp_c_live_handle_count()` 与它那条"建了又废必须回到基线"的断言之后，
  **M2、M3 才真的红**（摘表没了 ⇒ 登记表只增不减）。M1 依然"没抓住"，而这一次是
  **如实的**：摘表还在，残局照样被挡住，毒化在这一条路上是冗余的 —— 两道守卫
  **互为备份**是设计意图，不是覆盖缺口。
- **M6（拆异常收口）红掉的形态是"进程终止"**，不是某条断言失败：那条用例故意让
  `connect_wait` 抛一次（异步 `connect` 之后再同步 `connect_wait`），异常没有
  `catch` 就会穿过 `extern "C"` 边界 → `std::terminate`。所以这条变异的判据是
  **非零退出码 / 没有 `checks=` 那一行**，脚本照实写明这一点，不把它读成一句 `FAIL`。
  顺带记一笔：这一条的第一版锚点写坏了（替换后花括号不配对，`cpp` 直接编不过），
  脚本按"构建失败"退 3 —— 那一次是**脚本自己的 bug**，而"构建失败一律不当成
  抓住了"这条纪律正是靠它才有了价值。

### 第二次假绿：M8 与"回调期句柄越界"那两条断言

批 2 的 `test_capi_webapp_func` 里有两条断言，写的是"把一枚回调期句柄带出回调之后再问
它，必须得到 `E_STALE`，而且这不能是 UB"。它们在 M8 下**没有红** —— 抓住 M8 的只有最
后那句 `uvcpp_c_live_handle_count() == 0`。查下来是两个各自独立的问题：

1. **位置错了（读了一块不该读的内存）。** 第一版把它们放在 `stop()` / `join()` **之后**，
   理由是"那时候所有回调都跑完了，最确定"。可是 `join()` 一回来，服务端那条线程的栈
   就被 glibc 收回去、重新映射成零页：`alive()` 读到的魔数是 0，断言照样过，过的是
   "那块内存被清了"，不是"这道守卫挡住了它"。更糟的是，那一刻那块地址**已经不属于
   这个进程**，去读它本身就是潜伏的 UB —— 一条以"不许 UB"为卖点的断言，自己踩在 UB
   上。改到 `join()` **之前**（线程还活着，读的是活栈）之后，这一半问题消掉。
2. **这个判据量不了这个机制（这一半改位置解决不了）。** 我原以为挪到 `join()` 之前之后，
   M8 会让它俩红在 `E_WRONG_THREAD`（`-20008`）上 —— 登记表里那枚地址还在、魔数还在，
   于是 `alive()` 判真、接着线程检查发现问话的是主线程。**实测不是**：改完之后 M8 依然
   只红在活句柄数那条上。原因是 `creq` / `cresp` 是 `route_trampoline` 的**栈上局部量**，
   回调一返回，那块栈立刻被后续的循环代码复用，魔数被无关的写入盖掉了，`alive()` 在
   "读魔数"那一句就判假，根本走不到线程检查。

   → 结论写清楚：**只要句柄住在栈上，"魔数"这个判据就区分不出"被毒化"和"被栈复用盖
   掉"**，所以 M8 这个变异在 C 面**只有** `uvcpp_c_live_handle_count()` 量得出来。那两
   条断言量的是**契约**（"回调返回之后这枚句柄必须给 `E_STALE`"），不是**机制**（"是谁
   让它给的"）。两者都得有，但不能拿前者当后者的证据 —— 这正是第一版假绿的那个毛病，
   只是换了一层。

这一条与批 1 那条（`alive()` 那几步的**顺序是承重的**）是同一件事的两面，区别在于：
批 1 是"先查登记表、再读魔数"这个顺序救了命（M8 下登记表里那枚地址还在，代码**真的去
读了**那块栈；基线里它被登记表那一句挡在读内存之前 —— 两者对外都给 `E_STALE`，差别在
"有没有读一块本不该读的内存"）。所以文档里那句"先查表"不是风格，是这次量出来的。

### 第三次假绿：M15 与"同一句守卫有两个分支，只喂了一个"

批 3a 的 M15 拆的是 `uvcpp_c_h2_conn_start()` 里**两条** `table_size_ok` 检查中的一条
（`conn_cbs` 那条），而 `test_capi_h2_func` 里那两处坏表调用喂的都是**另一条**
（`h2_cbs`）。于是预期写的"抓住"，实测是**四个用例全绿**。病不在守卫，在**用例**：
同一条规矩的两个分支，只量了一个 —— 而这一点在用例里完全看不出来，那两处调用长得
很像"已经覆盖了 size 这件事"。

补上"`h2` 表缺席、`conn` 表 `size = 0`"那一次调用之后，M15 才真的红，而且红得很有
信息量：**那一次 `start` 居然返回了 0**（副作用全落地了：SETTINGS 发了、回调装上了），
紧接着那次真 `start` 拿到 `-114`（`UV_EALREADY`），`peer_goaway` 也因为会话已经歪掉
而没出现。三行断言把"拒绝必须发生在任何动作之前"这条规矩量得比原来清楚得多 ——
这正是把一次假绿改造成一条真判据的样子。

### 符号面锁：两边都真的会红，并且"关着"的那片是「未判」

`check_capi_symbols.py` 报绿不算数，得看它能不能红 —— 而且这一版还要看它**在该说
「没判」的时候说不说「没判」**。实测（2026-09-30，Linux，两棵树）：

| 动什么 | 判据 1（头 ↔ 锁） | 判据 2（库 ↔ 锁） | 退出码 |
|---|---|---|---|
| 不动（`build-capi-all`） | 绿（321 个都在锁里） | 绿（**七片全判**，321 个逐条对上） | `0` |
| 不动（`build-capi-h2`） | 绿 | 绿（五片判，`quic` / `http3` 印「未判」，229 个逐条对上） | `0` |
| 不动（`build-capi-h3`） | 绿 | 绿（六片判，`webapp` / `http2` 印「未判」，156 个逐条对上） | `0` |
| 不动（`build-capi`） | 绿 | 绿（四片判，`http2` / `quic` / `http3` 印「未判」，183 个逐条对上） | `0` |
| 把 `#@ module` 行全删掉（退回平锁） | **红**（229 处"符号出现在任何片头之前"） | —— | `1` |
| 锁里**少**一行头里有的 | **红**（"头里声明了、锁里没有"） | **红**（"库里导出了、锁里没记"） | `1` |
| 往**开着**的那片塞一个库里没有的 | 绿 | **红**（"锁里承诺过、库里没了"） | `1` |
| 往**关着**的那片塞一个，**对关掉它的树**跑 | 绿 | **绿，但那一行如实印「未判」**（"这一片 47 个这棵树一个都没导出，与「关着」对得上"） | `0` |
| 同一份，**对开着它的树**跑 | 绿 | **红**（同上那条） | `1` |
| 头里有一处声明没登记进 `MODULES` | **红** | —— | `1` |
| 树不存在（config 读不出来） | —— | **[停]「判据 2 没判」** | `3` |
| 跑 `--update` 而头里有、库里没有 | —— | —— | `1`（**拒绝写锁**） |

中间那两行是**一对**，也是这一版分片的全部理由：关着的那片在这棵树上量不出来，如实印
「未判」、把账留给开着的那条腿 —— 但那个假符号并没有就这么溜过去，换一棵开着它的树
就是一条红。所以 CI 上 `capi` 与 `h2` 两条腿要**一起**看：任何一片都必须至少有一条腿
是"开着"的，否则那一片永远停在「未判」上而没人发现（这也是批 3a 给 h2 那条腿加
`-DUVCPP_ENABLE_CAPI=ON` 的原因）。

倒数第二行是"不许拿空当绿"：树不在、`uvcpp_config.h` 读不出来时，报告里那 229 个符号
一个都没量，退 `3`。**3 是「没判」，不是「通过」**。

`--update` 那一行是本轮补的一条：`--update` 第一版把"头里的声明 ∪ 库里的导出"直接写进锁，
于是头里多了一个声明、库里没有定义这种漂移会被**静默固化成一条承诺** —— 而那正是
判据 2 存在的理由。现在它拒绝写锁并把名字打出来。

顺带记一笔自己踩的坑：判据 1 第一版直接扫头文件原文，于是文中那句举例
"`uvcpp_c_xxx(句柄, char* buf, size_t cap)`"被当成一个**真的声明**，
`--update` 把它写进锁，紧接着判据 2 报"承诺过、库里没了 `uvcpp_c_xxx`" ——
一条彻头彻尾的假红。修法是扫之前先剥注释（`strip_comments()`）。头里的注释本来
就该随便举例。

同一类坑还踩了第二次，这次是**自己跟自己的格式没对上**：写锁的那半边在片头后面挂了
一句注解（`#@ module common  (5 个)`），读锁那半边的正则要求行尾就结束 —— 于是
`--update` 刚写完，同一条命令再跑一遍就报"229 处：符号出现在任何 `#@ module` 行之前"，
也就是**把刚生成的锁判成平锁**。两边的约定只差一个后缀，症状却是"这份锁与这个门禁不是
一个版本"。修法是正则**显式**认下这一种注解（只认 `(数字 个)` 这一种写法，别的尾巴照旧
算形状不对）—— 注解是给人看的散文，不参与判断，但形状必须唯一。教训是：写与读是同一个
文件的两种解释，**它们得一起测**；只测其中一边，另一边会把绿判成红。

### 文档片段与"关掉开关还能不能编"

`doc/capi-guide.md` 的 C 片段由 `tests/tools/check_doc_snippets.py` 编。**注意
`capi/` 那几份头没有模块守卫**（它们无条件声明全部函数与句柄），所以在
`UVCPP_CAPI_ENABLE=0` 的包上这些片段**照样编得过** —— 本门只编到 `.o`、不链接，
"符号其实不在库里"这件事它看不见。实测（2026-09-30）：

```
# 发布包（CAPI=ON 的树出的）：
python3 tests/tools/check_doc_snippets.py --pkg dist/libuvcpp-1.4.3-linux-x64 \
    --cxx g++ --docs doc/capi-guide.md
  包里的模块: CAPI=1 HTTP3=0 NET=1 NGHTTP2=0 OPENSSL=0 QUIC=0 TRY_WRITE=1 WEBAPP=1 WEB=1 …
  [绿] doc/capi-guide.md          编过 1/1 条，片段 0 条（其中 C 片段 1/1）（648 B）

# 把包里生成头的 UVCPP_CAPI_ENABLE 改成 0，同一条命令重跑：
  [绿] doc/capi-guide.md          编过 1/1 条，片段 0 条（其中 C 片段 1/1）（648 B）
```

（两份输出都是 2026-09-30 在这棵树上量的；注意上面那行 `包里的模块` 里
`CAPI=1` 与 `CAPI=0` 的差别 —— 头照编不误，就是第 ② 件事。）

②就是"它们不靠这个开关"的实测依据，也是这个模块**没有**登记进
`check_doc_snippets.py` 的 `MODULE_REQ` 的理由（登记它会在 ② 的情形下造出一个
**假 `[跳]`**，把本该判绿的片段改判成"没判"、把退出码抬到 3）。

这条 C 片段本身也是本轮给门禁加的一格能力：`check_doc_snippets.py` 原来只认
`cpp` 那一种围栏，C ABI 那层"用法只能用 C 写"的示例因此**一条都不会被编**。
现在标着 `c` 的围栏会用 **C 编译器**（`-std=c99`，MSVC 侧 `/TC`）编，用哪个 C
编译器由 `--cxx` 派生（`g++`→`gcc`、`clang++`→`clang`、`cl`→`cl`），也可以
`--cc` 显式给。**加这一格是按"只增不改"验过的**：同一份包、同一批文档，旧版
186 条候选 / 80 条编过，新版 187 条候选（多出来的就是本页这一条）/ 80 条编过，
其余每一条的结论逐字不变（同一份包上逐行比过）。

顺带记一个自己踩的坑：写这一段时我把"四反引号包住三反引号"的写法用在了**行首**，
而抽取器只认三个反引号 —— 于是它把这行当成了一个**未闭合的围栏**，把后面整篇都
吞了（门禁报的就是这句）。行首别放反引号。

## 7. 没做的（如实列出）

- **UDP 的通用面**（`uvcpp_udp_*` 那一层）：C 面没给。quic 那一块是**例外**，它
  自己那条 UDP 传输走的是 `uvcpp_c_quic_*`（§4），不是给调用方一枚裸 UDP 句柄。
- **h3 的 trailers 与 server push**：见 §4 那张表（C++ 侧的 `doc/http3-guide.md`
  "真实现"表里也没有这两项）。
- **h2/h3 的逐帧回调**：`on_begin_headers` / `on_frame_recv` / `on_frame_*` 那一类
  不给 —— C 面给的是"这条流上发生了什么"（请求视图 / 流关闭信息），不是帧。
- **TLS 参数入口**（证书、私钥、SNI、校验开关）：见 §4 那张表。`is_tls()` 与
  `alpn_selected()` 这两个**查询**有，**设置**没有。
- **DNS 解析**：与 C++ 侧一致，只收 IPv4 / IPv6 地址串。
- **字符串编解码**：这一层收 `const char*` 加长度，按字节原样用，不做任何转换。
- **句柄的直接解引用**：定义在不透明句柄里的东西一律没有稳定性承诺，头里给的是
  不完整类型（§5 第 3 条）。
- **`uvcpp_c_live_handle_count()` 之外的任何内部状态**：加这一个是因为它是
  收支平衡唯一可观测的形式（§6），不是"顺手开的调试口子"。
