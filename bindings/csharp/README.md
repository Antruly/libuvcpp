# libuvcpp 的 C# 绑定（P/Invoke）

一句话：仓库里 `src/capi/` 那套 C ABI（不透明句柄 + `extern "C"` 函数 + C 函数
指针回调）在这里被**逐条镜像**成 C# 的 `DllImport`。拿上下面两个 `.cs` 文件，再
配一份 `libuvcpp` 动态库，C# 工程就能直接用这个库了 —— 不需要 C++ 编译器，也不
需要任何 NuGet 包。

这一层量的是什么、C 面的契约（错误码、回调表 `size` 规则、数据所有权、线程规
则）都写在 [`doc/capi-guide.md`](../../doc/capi-guide.md) 里。这份 README 只讲
**C# 这一侧**的事。

---

## 一、这几个文件各是什么

| 文件 | 对应 C 头 | DllImport 条数 |
|---|---|---|
| `UvcppNative.cs` | `uvcpp_c_common.h` / `_net.h` / `_web.h` / `_webapp.h` | 183 |
| `UvcppNative.Protocols.cs` | `uvcpp_c_http2.h` / `_quic.h` / `_http3.h` | 138 |
| `examples/QuicEcho/` | —— | 一个能跑的例子（回显服务端 + 自带客户端） |

两份 `.cs` 合起来 321 条，与 `tests/tools/capi_symbols.lock` 里的 321 个 C 符号
**一一对应**（怎么自己核，见 §七）。它们是同一个 `UvcppNative` partial 类的两半：
`Lib` 这个 DllImport 名字、`UVCPP_C_ABI_VERSION`、`UvcppError` / `UvcppCException` /
`UvcppErrorInfo` 都只在第一半里声明，第二半直接用。

镜像的规矩是**不增不减**：C# 方法名就是 C 里的名字（`uvcpp_c_quic_server_bind`
就叫这个名字），参数与返回值的映射见 §五。

---

## 二、怎么用（三步）

**1. 把两个 `.cs` 丢进你的工程。** 它们只用 `System.Runtime.InteropServices` 和
基础 BCL —— 没有 `unsafe`、没有 `NativeLibrary`、没有源生成器。工程文件里显式
列上即可（也可以直接放进目录让它被默认 glob 收走）：

```xml
<ItemGroup>
  <Compile Include="bindings/csharp/UvcppNative.cs" />
  <Compile Include="bindings/csharp/UvcppNative.Protocols.cs" />
</ItemGroup>
```

**2. 让运行时找得到动态库。** 名字、放在哪、Windows 上那个坑，全在 §三。

**3. 第一件事：比对 ABI 版本。** 这是 P/Invoke 最常见的故障模式 —— 头（也就是
这两个 `.cs`）与 `.so` / `.dll` 不是一次编出来的。不比对的话，症状通常不是一句
"版本不对"，而是某个结构体少一格、某次回调把栈写坏：

```csharp
if (UvcppNative.uvcpp_c_abi_version() != UvcppNative.UVCPP_C_ABI_VERSION)
    throw new InvalidOperationException("这份绑定和这份动态库不是一套");
```

`examples/QuicEcho/Program.cs` 的 `CheckAbi()` 就是这一段，可以直接抄。

---

## 三、动态库怎么被找到（这一节在 Windows 上最容易踩）

`DllImport` 用的名字是 **`uvcpp`**，各平台由运行时自己补前后缀：

| 平台 | 运行时实际会找 | 发布包里那份文件 |
|---|---|---|
| Linux | `uvcpp` → `libuvcpp` → `uvcpp.so` → **`libuvcpp.so`** | `libuvcpp.so` |
| macOS（自行构建） | `uvcpp` → `libuvcpp` → `uvcpp.dylib` → `libuvcpp.dylib` | —— |
| Windows + MSVC 包 | `uvcpp.dll` → `uvcpp.exe` | **`uvcpp.dll`** ✅ 名字正好对上 |
| Windows + MinGW 包 | `uvcpp.dll` → `uvcpp.exe` | **`libuvcpp.dll`** ⚠️ 对不上 |

**⚠️ 那一格是 Windows 上唯一的坑**：MinGW 工具链产出的文件叫 `libuvcpp.dll`，
而 Windows 的探测**不试 `lib` 前缀**（`lib` 前缀是 Linux/macOS 的规矩），所以
拿 MinGW 包的人直接 `DllImport("uvcpp")` 会得到 `DllNotFoundException`。三个办法，
任选一个：

1. **用 MSVC 包**（`uvcpp-x.y.z-msvc-x64.zip` 里的就是 `uvcpp.dll`，开箱即用）；
2. **把文件复制/改名为 `uvcpp.dll`**，放在 exe 同目录（探测顺序里有应用目录）；
3. **自己装一个解析器**（.NET Core 3.0+ / net5+ 有 `NativeLibrary`）：

```csharp
using System.Reflection;
using System.Runtime.InteropServices;

// 放在 Main 的第一句。
NativeLibrary.SetDllImportResolver(typeof(UvcppNative).Assembly, (name, asm, paths) =>
{
    if (name != "uvcpp") return IntPtr.Zero;          // 只管自己这一枚
    foreach (string cand in new[] { "uvcpp", "libuvcpp" })
        if (NativeLibrary.TryLoad(cand, asm, paths, out IntPtr h)) return h;
    return IntPtr.Zero;                                // 交回默认探测
});
```

Linux 上让运行时找到它：`LD_LIBRARY_PATH=<包里的 lib 目录>`，或者把 `libuvcpp.so`
放到 exe 旁边（同时记得 `rpath`/`patchelf` 或者干脆用同目录）。Windows 上就是
`PATH` 或 exe 同目录。

三方依赖**不用管**：发布包的动态库把 libuv / llhttp / zlib / OpenSSL / nghttp2 /
ngtcp2 / nghttp3 都静态链进去了 —— 三条腿各有各的断言在发布流程里把这件事钉着，
Linux 上 `ldd libuvcpp.so` 只剩 `libc` 与 `ld-linux`，Windows 上导入表只剩系统
dll。MSVC 包额外带三份 VC++ 运行库（`msvcp140.dll` / `vcruntime140.dll` /
`vcruntime140_1.dll`），一起拷过去即可。（发布包**没有 macOS 那一档**，六个平台
是 linux-x64 / linux-arm64 / mingw-x64 / mingw-arm64 / msvc-x64 / msvc-arm64。）

---

## 四、五条必须知道的规矩

前四条是 C ABI 那一层的契约，第五条是 C# 侧的独立坑。

**1. 线程亲和。** 除了少数几个（`*_post()` / `*_stop()` 之类），**所有**函数都只
能在事件循环所在线程上调用，否则返回 `UVCPP_C_E_WRONG_THREAD`。清单在
`doc/capi-guide.md`，判定规则很简单：**回调是在哪条线程上进来的，就在那条线程上
调回去**。

**2. 委托必须自己保活。** 回调是函数指针，托管委托一旦被 GC 回收，C 侧那个指针
就是悬垂的 —— 症状是随机的访问违例，而且往往离出事点很远。所以：**每个装进 C 侧
的委托，都要有一个活得比它长的引用**（`static` 字段、或跟着宿主对象一起活）。
`examples/QuicEcho/Program.cs` 顶部那一排 `static` 字段就是干这个的。

**3. 回调里的指针只在回调期间有效。** `uvcpp_c_read_result.data`、HTTP body 那
一类指针**出了回调就作废**。要留就当场拷：`Marshal.Copy(data, buf, 0, n)`。反过
来，**传进去**的参数（`byte[]`、`string`）在函数返回后随便处置 —— C 层当场拷。

**4. 回调期的句柄不要带出去。** `uvcpp_c_req` / `uvcpp_c_resp` / `uvcpp_c_next`
这类只在**回调执行期间**有效；能带出去的是 `uvcpp_c_deferred`（`uvcpp_c_req_defer()`
拿到的那个），而且 `defer` 之后原来那两个句柄**作废**，要用
`uvcpp_c_deferred_resp()` 拿的那个。带出去再用会拿到 `UVCPP_C_E_STALE` —— 不崩，
但那是一句明确的"你越界了"。

**5. 不许让异常逃出回调。** 回调是 C 侧调进来的，托管异常要穿过 `uv_run` 那几层
native 帧。运行时的处置是打印 `Unhandled exception` 然后 **abort** —— 本机实测：
进程以 134 退出并留下 core。所以每个回调体自己 `try/catch`，把失败记进字段，
回到主线程再判；`examples/QuicEcho/Program.cs` 的 `Note()` 就是这个模式。

### 另外：QUIC 的"一次收尾 = 两条回调"

这一条不在上面五条里，因为它是 QUIC 独有的，而且**最容易写错**：一个"带数据和
FIN 的 STREAM 帧"会先报一条 `DATA`、紧接着再报一条 `PEER_CLOSED`，而**那条
`DATA` 的 `fin` 也是非 0** —— 它是"这块就是最后一块"的信息位（留给 HTTP/3 那种
要当场分帧的调用方），**不是**"回调报完了"的意思。

所以**流的结束判据是 `event == PEER_CLOSED`，不是 `fin`**。拿 `fin` 当结束判据
会把同一段收尾做两遍（这个例子的第一版就是这么写的，于是回显了两次，第二次时写
方向已经关了）。TCP 那边不存在这个问题（真的是两次回调，`fin` 恒 0）。

---

## 五、C 到 C# 的类型映射

| C | C# | 说明 |
|---|---|---|
| `uvcpp_c_xxx*`（不透明句柄） | `IntPtr` | 文件级 `using` 别名，名字与 C 里同名；**不要解引用** |
| `int` 返回码 | `int` | **负数 = 失败**；`0` 或正数 = 成功 |
| `size_t` | `nuint` | 长度、容量、计数 |
| `int64_t` | `long` | 流号等 |
| `const char*`（借出的） | `[MarshalAs(UnmanagedType.LPUTF8Str)] string` | UTF-8 |
| `const char*` + `size_t`（二进制） | `byte[]` + `nuint` | 二进制安全，允许含 NUL |
| `char* buf` + `size_t cap`（填缓冲区） | `byte[]` + `nuint` | 返回**真实长度**；`cap` 不够时不写、只报长度 |
| 函数指针 | `[UnmanagedFunctionPointer(Cdecl)]` 委托 | 见 §四第 2 条 |
| 以 `uint32_t size` 打头的结构体 | `struct` + `size = (uint)Marshal.SizeOf<T>()` | **必须最先填**，C 层按它决定读到哪一格 |

**一处必须知道的取舍**：C 头里那些写着"可以为 NULL / NULL = 默认"的参数
（`uvcpp_c_quic_tls_client_new(NULL)` 的不校验、`uvcpp_c_quic_server_bind(s, NULL, 0)`
的通配地址、`uvcpp_c_msg_set_content_type(NULL)` 的摘掉……），在这个绑定里一律
声明成**非空** `string` / `byte[]` —— 因为 C# 的默认可空性要一个个显式标 `?`，
而"哪个 C 参数允许 NULL"是**头里的文档行**，不是签名能表达的。要传 NULL 就写
`null!`，运行期完全合法，只是编译器会提醒你一句：

```csharp
IntPtr tls = UvcppNative.uvcpp_c_quic_tls_client_new(null!);   // NULL = 不校验对端证书
```

---

## 六、跑例子

`examples/QuicEcho` 是一个 QUIC 回显服务端 + 一个自带客户端：默认做一次回环往
返（服务端收、回显、客户端收到同样的字节），自带判据、退出码 0 = 通过。

```bash
cd bindings/csharp/examples/QuicEcho

# 让运行时找到动态库（Linux 例；Windows 见 §三）
export LD_LIBRARY_PATH=/path/to/libuvcpp/lib

dotnet run                 # 回环往返
dotnet run -- --serve      # 只当服务端，监听 4433（--port 改）
```

本机实测输出（Linux x64 / .NET 8.0.11 / 发布档 `libuvcpp.so`，`1.5.0`）：

```
libuvcpp 1.5.0（C ABI 1）
服务端在 127.0.0.1:36547 上收 QUIC（ALPN uvcpp-echo/1）
  服务端：新连接进来了
  客户端：握手完成，流 0 上发了 20 字节
  服务端：流 0 收到 20 字节，回显并 FIN
  客户端：流 0 收到 20 字节并收到 PEER_CLOSED
  协商出的 ALPN：uvcpp-echo/1
  服务端 on_close 来过：False；客户端 on_close 来过：False
回环通过：20 字节逐字节相等（63 ms）
  收尾后活句柄数：0
```

（端口每次都是内核挑的，所以那一行每次都不同；`(63 ms)` 也随机器变。）
`on_close 来过：False` 那两行不是可有可无的：它说明这条回环**从头到尾没有关过连接**，
例子最后是**带着活连接**把两端释放掉的 —— 而"带着活连接释放端点"正是
`uvcpp_c_quic_server_free()` 曾经 SIGSEGV 的那个形状（这个例子第一次跑就是它撞出来的；
修复、回归用例与变异 M21 都在 `1.5.0` 里）。所以这两行同时也是那条回归的一次真实验证。

最后那行 `活句柄数：0` 不是装饰：它是这个例子自己带的一条断言 —— 跑完把两端与
两个 TLS 上下文都 `_free()` 之后，`uvcpp_c_live_handle_count()` 必须回到 0。

例子里的注释标出了三处**照抄时要保留**的形状：委托保活、回调里的指针当场拷走、
回调体自己 `try/catch`。`--serve` 那一支是 `uvcpp_c_quic_server_run()` 阻塞到
Ctrl+C，真做服务端程序时把每一端放到自己的线程上用 `*_run()` 跑即可（别在两个
线程上泵同一个端点）。

---

## 七、覆盖面：321 个符号，怎么自己核

`tests/tools/capi_symbols.lock` 是 C 面的符号面锁，`tests/tools/check_capi_symbols.py`
拿它与构建出来的库对账。绑定与它的关系是**双向**的：

```bash
# C 头 → 库（需要一份开着 CAPI 的构建树）
python3 tests/tools/check_capi_symbols.py --tree build-capi

# 绑定 → 锁（不需要库，纯文本对账；也是本仓 CI 会跑的那条）
python3 - <<'PY'
import re, pathlib
lock = {l.strip() for l in open("tests/tools/capi_symbols.lock")
        if l.strip().startswith("uvcpp_c_")}
src = "".join(open(p, encoding="utf-8").read()
              for p in ("bindings/csharp/UvcppNative.cs",
                        "bindings/csharp/UvcppNative.Protocols.cs"))
decl = set(re.findall(r'EntryPoint\s*=\s*"([^"]+)"', src))
print("锁里有、绑定没声明：", sorted(lock - decl))
print("绑定声明了、锁里没有：", sorted(decl - lock))
print("两边都是 %d 条" % len(decl))
PY
```

两组都应该是空的，条数是 321。**加了新的 C 函数而没同步绑定**，上面第二个脚本
会当场说出来 —— 别等到运行期才发现某个 `EntryPointNotFoundException`。

---

## 八、为什么不能"手写一份声明"（本机逐条对过的表）

P/Invoke 的声明**没有任何编译器替你核对**：`DllImport` 写错一个参数，编得过、
跑得起来、行为全错 —— 或者更糟，C 侧拿你的垃圾值当地址写。下面每一行都是拿本
仓真实头文件对出来的。"手写容易写成"那一列不是假想的：它是**反复出现**的形状
（模型、搜索结果、IDE 补全都给得出这些）。

| 手写容易写成 | `src/capi/` 里实际是 | 会怎么坏 |
|---|---|---|
| `void uvcpp_c_app_free(app)`、`void uvcpp_c_resp_text(r, s)` | `int`（错误码）| 返回码被丢掉 —— `E_STALE` / `E_WRONG_THREAD` 这类"你写错了"的失败静默消失 |
| `bool uvcpp_c_app_running(app)` | `int` | **最危险的一格，本机实测过**：托管 `bool` 默认按 Win32 `BOOL` 封送（非 0 即 `true`）。拿一个已经 `_free()` 掉的句柄问它，`int` 声明读到 `-20002`（`E_STALE`），`bool` 声明读到 **`True`** —— **"出错了"被读成"正在运行"** |
| `int uvcpp_c_app_connection_count(app)` | `size_t` | 64 位下读到的是低 32 位（连接数上 2³¹ 才露），但 `size_t` 一律写 `nuint` 是零成本的 |
| `uvcpp_c_tcp_server_listen(server, onClientPtr)` | `(server, int backlog)` | **凭空多一个回调参数**：多出来的那个被忽略，C 侧把**指针的低 32 位**当 `backlog` 用（多半是个巨大的数）。cdecl 下调用方清栈，所以连崩都不崩 |
| `IntPtr uvcpp_c_req_path(req)`（以为它借出一枚指针）| `int uvcpp_c_req_path(req, char* buf, size_t cap)` | **少了两个参数**：真正被调的是 `(req, <栈上垃圾>, <栈上垃圾>)`，C 侧往那块地址写。`req` / `resp` 这一族**没有"借出字符串"的形态**，一律是"你给缓冲区、它还把长度" |
| `uvcpp_c_req_header(req, key)` | `(req, name, char* buf, size_t cap)` | 同上 |
| `uvcpp_c_resp_send_file(resp, path, size, cb)` | `(resp, path, done_fn, user_data)` —— **没有 `size`**，`done` 是三个参数的回调 | 参数位整排错开 |
| `uvcpp_c_app_get(app, path, cbPtr)` | `(app, pattern, cb, user_data)` —— **四个** | 同上 |
| `Marshal.PtrToStringAnsi` | 头里写的是 UTF-8 | 非 ASCII 当场乱码（`PtrToStringUTF8` 要 .NET 5+；绑定里走的是 `LPUTF8Str`）|
| `GC.KeepAlive(handler)` 写在 `Main` 末尾 | 委托要活得比**每一次** native 回调长 | `KeepAlive` 只保证"到这一句为止还活着"；这句**之后**进来的回调随时可能踩到已回收的委托。要的是 `static` 字段（见 §四第 2 条）|

（顺带一条不是坑、但同属"照抄头的"这条规矩：`uvcpp_c_abi_version()` 在头里是
`unsigned int`，不是 `int`。两个都是 4 字节，正常值下能对上 —— 写在这里只是说明
那条规矩没有例外。）

**这就是这两份 `.cs` 存在的理由**，也是它们为什么是**对着锁生成、再逐条对账**过
的：321 条声明 ↔ `tests/tools/capi_symbols.lock` 里 321 个符号，名字、参数个数、
`size_t`→`nuint` 的映射全在 §七 那个脚本的射程里。要加一个 C 函数，是"改头 →
补锁 → 补绑定"三步，每一步都有东西看着；手写一份则是**没有任何东西看着**。

---

## 九、没验过的部分（照实说）

已经**真跑过**的是：Linux x64、.NET 8（`net8.0`）、发布档 `libuvcpp.so`、
`examples/QuicEcho` 的整个回环。编译门槛是量出来的：

| 目标 | 结果 |
|---|---|
| `net8.0`（C# 10+） | 0 警告 0 错误，而且真跑过 |
| `netstandard2.1` + C# 10 | 0 警告 0 错误（只编过，没跑过） |
| `netstandard2.0` | **编不过**：参考程序集里 `UnmanagedType` 没有 `LPUTF8Str` |
| C# 9 及更早 | **编不过**：文件范围命名空间要 C# 10 |

**没有在 Windows / macOS 上跑过这个绑定**（本机只有 Linux）。Windows 那条路要
小心的就是 §三 表里 MinGW 那一格，以及 `nuint` / 结构体布局在 64 位下的对齐
（两份 `.cs` 里的 `size` 都是 `Marshal.SizeOf<T>()` 算的，不写死数字 —— 就是为了
不让对齐假设藏进代码里）。

也没在 Mono / Unity 上验过：Unity 2022 还是 C# 9，`namespace Uvcpp;` 这种写法
它不认。真要在那上面用，得把那两处改成块形式（要重新缩进整个文件），那一档
**没验过**，别照抄。
