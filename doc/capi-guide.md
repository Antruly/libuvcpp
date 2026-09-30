# C ABI 指南

`src/capi/` 是本库的 **C 接口层**：一组 `extern "C"` 导出的函数、几份**纯 C**
（C99）的头，装到 `include/capi/`。它的用途很具体 —— 让 C#、Rust、Go、Python
这些语言能通过 FFI（P/Invoke 那一路）用到本库的 webapp / web / net / http2 /
http3 / quic，而不需要写一个 C++ 中间层。

它与 C++ ABI 的关系只有一句：**同一份库、同一个版本号、同一个 `.dll`/`.so`**。
C 层是薄包装（约 150–250 个函数的精选门面，见 §4），不新增第三方依赖、不新增
产物、不改动 C++ 那一侧的任何导出行为。

> ## 1.4.2 起它的第一批是"能用的"：地基 + net
>
> 这一批交付了 `uvcpp_c_common.h` 与 `uvcpp_c_net.h`：ABI 自洽、错误码与文案、
> 句柄的生死与类型检查、版本化回调表的 `size` 规则、线程纪律，以及
> **`tcp_client` / `tcp_server` 的完整可用面**。判据是 `tests/capi/` 下两个
> **纯 C 编译**的用例（`capi_common_func.c`、`capi_net_func.c`）：它们本机真起
> 两端、真收字节，并在 `tests/tools/capi_mutation.py` 那张变异表下被逐条拆守卫。
>
> **没做的**逐条列在 [§7](#7-没做的如实列出)：web / webapp / http2 / http3 /
> quic 的 C 面、TLS 参数入口、UDP、DNS。别在别处另维护一份。

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
| 本层的码 | `-20001` … `-20009` | `enum uvcpp_c_error`，见 `include/capi/uvcpp_c_common.h` |
| libuv 的码 | `-errno` 与 `-4095` 那一带（`UV_EOF`、`UV_EAI_*`…） | **原样透传**，不翻译 |

两段不可能撞（libuv 最负的一个是 `UV_UNKNOWN` = -4096），所以
`uvcpp_c_strerror(err)` 能一句判出"这是谁家的码"：本层那九格给中文/英文文案，
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

## 4. 提供什么、明确不提供什么

这一批（1.4.2）只到地基 + net。**名字都是 `uvcpp_c_` 前缀。**

| 头 | 提供 | 不提供 |
|---|---|---|
| `capi/uvcpp_c_common.h` | `abi_version` / `version_string` / `strerror` / `last_error_string` / `live_handle_count`；错误码表；导出宏 `UVCPP_C_API` | 日志级别、内存分配器入口 —— 这一层不返回分配的内存（§3.3） |
| `capi/uvcpp_c_net.h`（客户端，17 个） | `new` / `free` / `set_events` / `connect` / `connect_wait` / `write` / `write_wait` / `read_pause` / `read_resume` / `read_stop` / `close` / `run` / `stop` / `is_connected` / `last_error` / `is_tls` / `alpn_selected` | TLS **参数**入口（证书、私钥、SNI、校验开关）—— 那是 C++ 的 `uvcpp_ssl_context`，C 面没有对应类型；DNS 解析（与 C++ 侧一致，只收 IPv4/IPv6 串） |
| `capi/uvcpp_c_net.h`（服务端，13 个） | `new` / `free` / `set_events` / `bind` / `local_port` / `listen` / `set_loops` / `loop_count` / `run` / `stop` / `client_count` / `close_all_clients` / `last_error` | 每连接独立的 accept 策略（回调里给句柄，动作自己定） |
| `capi/uvcpp_c.h` | 伞头：按各模块宏 include 上面几份 | 任何 C++ 类型、任何 libuv 类型（§1 第 2 条） |

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
cmake --build build-capi -j"$(nproc)"

# ① C 层自己的用例（两个都是**纯 C**编的）
ctest --test-dir build-capi -R capi --output-on-failure
#   test_capi_common_func：地基（ABI 自洽、错误码文案、句柄生死与类型、
#                          活句柄数收支平衡、事件表 size 规则）
#   test_capi_net_func   ：net 端到端（真起两端、真收字节的回显、线程纪律、
#                          异常不过边界）

# ② 头**真是 C 的**，不是"看起来像 C"：每一份都过一个 C 编译器
for h in src/capi/uvcpp_c*.h; do
  gcc -x c -std=c99 -pedantic-errors -Werror -fsyntax-only -I src "$h" \
    || echo "不是纯 C: $h"
done

# ③ 反空转：把承重的守卫逐条拆掉，看用例有没有一声响
python3 tests/tools/capi_mutation.py --tree build-capi

# ④ 符号面锁
python3 tests/tools/check_capi_symbols.py --tree build-capi

# ⑤ 守卫链的反例（两条都应当**配置成功**、打 warning、把模块排除掉）
cmake -S . -B /tmp/capi-bad \
  -DUVCPP_ENABLE_CAPI=ON -DUVCPP_BUILD_WEB=OFF 2>&1 | grep -i capi
```

### 变异表量出来的（2026-09-30，Linux / gcc，`--tree build-capi --jobs 8`）

八条变异（一条基线 + M1–M7），判据是"实际结果与**先写下来的预期**一致、源码按字节还原、
还原后两个用例复跑全绿"。实测：

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

### 符号面锁：两边都真的会红

`check_capi_symbols.py` 报绿不算数，得看它能不能红。实测（2026-09-30，Linux）：

| 动什么 | 判据 1（头 ↔ 锁） | 判据 2（库 ↔ 锁） | 退出码 |
|---|---|---|---|
| 不动 | 绿（35 个都在锁里） | 绿（35 个逐条对上） | `0` |
| 锁里**多**一行库没有的 | 绿 | **红**（"锁里承诺过、库里没了"） | `1` |
| 锁里**少**一行库里有的 | **红**（"头里声明了、锁里没有"） | **红**（"库里导出了、锁里没记"） | `1` |
| 跑 `--update` 而头里有、库里没有 | —— | —— | `1`（**拒绝写锁**） |

最后一行是本轮补的一条：`--update` 第一版把"头里的声明 ∪ 库里的导出"直接写进锁，
于是头里多了一个声明、库里没有定义这种漂移会被**静默固化成一条承诺** —— 而那正是
判据 2 存在的理由。现在它拒绝写锁并把名字打出来。

顺带记一笔自己踩的坑：判据 1 第一版直接扫头文件原文，于是文中那句举例
"`uvcpp_c_xxx(句柄, char* buf, size_t cap)`"被当成一个**真的声明**，
`--update` 把它写进锁，紧接着判据 2 报"承诺过、库里没了 `uvcpp_c_xxx`" ——
一条彻头彻尾的假红。修法是扫之前先剥注释（`strip_comments()`）。头里的注释本来
就该随便举例。

### 文档片段与"关掉开关还能不能编"

`doc/capi-guide.md` 的 C 片段由 `tests/tools/check_doc_snippets.py` 编。**注意
`capi/` 那几份头没有模块守卫**（它们无条件声明全部函数与句柄），所以在
`UVCPP_CAPI_ENABLE=0` 的包上这些片段**照样编得过** —— 本门只编到 `.o`、不链接，
"符号其实不在库里"这件事它看不见。实测（2026-09-30）：

```
# 发布包（CAPI=ON 的树出的）：
python3 tests/tools/check_doc_snippets.py --pkg dist/libuvcpp-1.4.2-linux-x64 \
    --cxx g++ --docs doc/capi-guide.md
  [绿] doc/capi-guide.md  编过 1/1 条（其中 C 片段 1/1）（648 B）   rc=0

# 把包里生成头的 UVCPP_CAPI_ENABLE 改成 0，同一条命令重跑：
  [绿] doc/capi-guide.md  编过 1/1 条（其中 C 片段 1/1）（648 B）   rc=0
```

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

- **web / webapp / http2 / http3 / quic 的 C 面**：这一批没有。它们各自会带自己
  的头（`uvcpp_c_web.h` / `uvcpp_c_webapp.h` / `uvcpp_c_http2.h` /
  `uvcpp_c_quic.h` / `uvcpp_c_http3.h`）与自己的用例，逐条照这一批的规矩来。
- **TLS 参数入口**（证书、私钥、SNI、校验开关）：见 §4 那张表。`is_tls()` 与
  `alpn_selected()` 这两个**查询**有，**设置**没有。
- **UDP**：`net` 那一层有，C 面没给 —— 这一批的判据是"一个真实的 TCP 程序能用
  起来"，UDP 不在里面。
- **DNS 解析**：与 C++ 侧一致，只收 IPv4 / IPv6 地址串。
- **字符串编解码**：这一层收 `const char*` 加长度，按字节原样用，不做任何转换。
- **句柄的直接解引用**：定义在不透明句柄里的东西一律没有稳定性承诺，头里给的是
  不完整类型（§5 第 3 条）。
- **`uvcpp_c_live_handle_count()` 之外的任何内部状态**：加这一个是因为它是
  收支平衡唯一可观测的形式（§6），不是"顺手开的调试口子"。
