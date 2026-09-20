# libuvcpp v1.1.0 Release Notes

## 简介 (Introduction)

**libuvcpp** 是一个基于 libuv 的现代 C++ 封装库，提供简洁的面向对象接口来使用 libuv 的异步 I/O 功能。

v1.1.0 在 v1.0.0 的 libuv 封装之上，**新增了网络层、HTTP/1.1 与 WebSocket、TLS、以及一套 Web 应用框架**，
并首次提供**预编译的 Windows x64 动态库**。全部为向后兼容的增量。

## 新增模块 (New in v1.1.0)

### net — 面向对象的网络层
- `uvcpp_tcp_server` / `uvcpp_tcp_client` — TCP 服务端与客户端
- `uvcpp_udp_server` / `uvcpp_udp_client` — UDP 服务端与客户端
- `uvcpp_net_read` — 统一的读回调

### web — HTTP/1.1 与 WebSocket
- `uvcpp_http_server` / `uvcpp_http_client` — HTTP 服务端与客户端（含流水线）
- `uvcpp_http_parser` / `uvcpp_http_request` / `uvcpp_http_response` — 基于 llhttp 的解析与报文构造
- `uvcpp_http_compress` — gzip / deflate 压缩（zlib）
- `uvcpp_static_server` — 静态文件服务
- `uvcpp_ws_server` / `uvcpp_ws_client` / `uvcpp_ws_parser` / `uvcpp_ws_frame` / `uvcpp_ws_sessions` — WebSocket（RFC 6455）

### ssl — TLS
- `uvcpp_ssl_context` / `uvcpp_ssl` — TLS 上下文与连接（OpenSSL）

### webapp — Web 应用框架
- `uvcpp_web_app` + `uvcpp_web_router` + `uvcpp_web_handler` — 应用与路由
- `uvcpp_web_middleware` — 中间件
- `uvcpp_web_static` / `uvcpp_web_mime` / `uvcpp_web_file` — 静态资源与文件下发
- `uvcpp_web_stream` — 流式响应
- `uvcpp_web_multipart` / `uvcpp_web_upload` — multipart 上传
- `uvcpp_web_json` — JSON（nlohmann/json）
- `uvcpp_web_ws` / `uvcpp_web_ws_client` — 接入 webapp 的 WebSocket
- `uvcpp_log` / `uvcpp_log_console` — 日志

## 预编译产物 (Prebuilt binaries)

本版本提供三个平台的 x64 预编译动态库：

| 平台 | 工具链 | 产物 |
|---|---|---|
| Windows x64 | MinGW-w64 (GCC) | `libuvcpp.dll` + `libuvcpp.dll.a` |
| Windows x64 | MSVC (VS2022) | `uvcpp.dll` + `uvcpp.lib` |
| Linux x64 | GCC | `libuvcpp.so` |

每个 zip 内含 `bin/`（动态库）、`lib/`（导入库）、`include/`（公开头，含 libuv、
nlohmann/json、zlib 的头）、`lib/pkgconfig/uvcpp.pc` 与文档。

| 文件 | 说明 |
|---|---|
| `bin/libuvcpp.dll` | 动态库。**libuv / llhttp / zlib / OpenSSL 以及 MinGW 运行时均已静态链接进去** |
| `lib/libuvcpp.dll.a` | 导入库（供 MinGW/GCC 链接，`-luvcpp`） |
| `include/` | 公开头文件，含 `expand/`（内存池） |
| `include/uvcpp/uvcpp_config.h` | **生成的**模块使能宏。每个公开头自己包含它，使用者**不必再传任何 `-D`**（见下） |

> 命名遵循各工具链的惯例：**MinGW/GCC 产出 `libuvcpp.dll`**，MSVC 产出 `uvcpp.dll`。

MinGW 版 `libuvcpp.dll` 的依赖只有 Windows 自带系统库（`KERNEL32` / `WS2_32` / `CRYPT32` /
`ADVAPI32` / `USER32` / `SHELL32` / `IPHLPAPI` / `USERENV` / `ole32` / `dbghelp` / `msvcrt`），
**不需要安装 MSYS2 或 MinGW 运行时**，任意 64 位 Windows 直接可用。

MSVC 版 `uvcpp.dll` 用 `/MD` 构建，因此 `bin/` 里一并带了
`msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll`，
**不需要预装 VC++ 可再发行组件**。若构建机上只有动态版 OpenSSL，`bin/` 里还会多出
`libssl-3-x64.dll` / `libcrypto-3-x64.dll`（本机构建用的是静态版，因此本地这份包没有）。

打包脚本不靠手写的依赖清单，而是**读产物自己的导入表**：凡是不是 Windows 自带的
模块，包里必须有，找不到就拒绝出包（`tests/tools/package_release.py`）。早先那张手写
清单是猜的 —— MSYS2 同时装了 `libssl.a` 和 `libssl.dll.a`，`find_library` 默认挑
`.dll.a`，产物会凭空多一个 `libssl-3-x64.dll` 而清单里没有。

> ⚠️ 两个 Windows 版互为替代、不可混用：由 MinGW-w64 编译的动态库**不能被 MSVC
> 链接**，反之亦然（C++ ABI 不同）。用哪套工具链就用哪个 zip。

### 使用方式

**使用者不需要传任何 `-D`。** 模块使能宏由包里的
`include/uvcpp/uvcpp_config.h` 给出，每个公开头都会在自己第一个模块守卫**之前**
包含它，所以只要 `-I` 指对，宏就自动与这个 dll 一致：

```bash
export PKG_CONFIG_PATH=/path/to/libuvcpp-1.1.0-mingw-x64/lib/pkgconfig
g++ -std=c++11 $(pkg-config --cflags uvcpp) your_app.cpp $(pkg-config --libs uvcpp) -o your_app.exe
```

> ⚠️ `--cflags` 与 `--libs` **要分开写、`--libs` 要放在源文件之后**。
> 合成一条 `$(pkg-config --cflags --libs uvcpp) your_app.cpp` 会把 `-luvcpp`
> 排到源文件前面，`ld` 不会回头再看一遍归档文件 ⇒ 28 个 `undefined reference`
> （库本身没问题，纯粹是命令行顺序）。

不用 pkg-config 时，等价的命令行是：

```bash
g++ -std=c++11 -I include your_app.cpp -L lib -luvcpp -o your_app.exe
```

（上面两条命令都在包的根目录下执行；`-L lib` 是导入库所在处。跑的时候
`bin/libuvcpp.dll` 要在 `PATH` 上，或直接拷到 exe 旁边。）

> MSVC 消费者**不需要额外传 `/utf-8`**。仓内头文件里含中文注释的那批本来靠
> `add_compile_options(/utf-8)` 兜着，而那个开关是**目录作用域**的 —— 既不进导出集，
> 也到不了预编译包的消费者。按系统代码页（936）读这些头时，中文注释的末字节会吞掉
> 换行、把 `*/` 吃掉，注释不闭合，报错却落在 `<algorithm>` 里（`C4819` 是唯一的线索）。
> 因此 1.1.27 起，**打包脚本给所有含非 ASCII 的头补了 UTF-8 BOM**，MSVC 会据此自动
> 按 UTF-8 读，什么开关都不用加。包里的头因此与源码树里的**不逐字节相同**（多一个 BOM），
> gcc/clang 前导 BOM 一样接受，各平台包保持同一份字节。

> ⚠️ **不要自己定义这些宏。** 如果你显式传了一个与包**不一致**的值
> （`-DUVCPP_OPENSSL_ENABLE=0` 拿到一个开着 OpenSSL 编的包），
> `uvcpp_config.h` 会**直接 `#error` 停编译**，并指出包里那个值。
> 这是刻意的：宏不一致时公开头里的**成员布局**会与 dll 不同，
> 内联访问器按错误偏移读成员，拿到的是一个天文数字，
> **没有编译错误、没有链接错误、没有运行时告警** —— 比编不过危险得多。
> 需要别的宏集就重新构建 uvcpp，不要从这一侧改。
>
> 1.1.26 及更早的包需要你手工传一串 `-D`，且那份清单是**写死**的
> （与包实际怎么编无关）。升级到本版时把那些 `-D` **删掉**即可；
> 留着它们只有在与包冲突时才会报错，值相同时无害。

头文件的入口是包根 `include/uvcpp.h`（聚合头，含 loop / handle / req）。
`net` / `web` / `webapp` / `ssl` 的类**不在聚合头里**，按模块显式 include，例如
`#include "handle/uvcpp_tcp.h"`、`#include "web/uvcpp_http_server.h"`。

## 主要特性 (Key Features)

### Handles（句柄）
- `uvcpp_loop` - 事件循环核心
- `uvcpp_tcp` - TCP 客户端/服务端
- `uvcpp_pipe` - 管道通信（支持 IPC）
- `uvcpp_udp` - UDP 通信
- `uvcpp_tty` - 终端设备
- `uvcpp_poll` - 文件描述符轮询
- `uvcpp_timer` - 定时器
- `uvcpp_signal` - 信号处理
- `uvcpp_fs_event` / `uvcpp_fs_poll` - 文件系统监控
- `uvcpp_async` - 异步通知
- `uvcpp_process` - 进程管理
- `uvcpp_idle` / `uvcpp_prepare` / `uvcpp_check` - 事件循环钩子

### Requests（请求）
- `uvcpp_write` - 写请求
- `uvcpp_connect` - 连接请求
- `uvcpp_shutdown` - 关闭请求
- `uvcpp_fs` - 文件系统操作
- `uvcpp_work` - 工作请求（线程池）
- `uvcpp_getaddrinfo` / `uvcpp_getnameinfo` - DNS 查询
- `uvcpp_udp_send` - UDP 发送请求

### 缓冲区管理 (Buffer Management)
- `uvcpp_buf` - C++ 封装 `uv_buf_t`，提供安全内存管理

### 实用工具 (Utilities)
- 线程、互斥锁、条件变量
- 线程安全的环境变量、用户/组信息
- 目录遍历、文件描述符操作
- 随机数生成、CPU 信息、网络接口

## 构建要求 (Requirements)

- C++11 或更高版本
- CMake 3.16+
- libuv 1.0.0+
- 支持 Windows / Linux / macOS
- 可选：OpenSSL（TLS）、zlib（压缩）、llhttp 与 nlohmann/json（HTTP/Web 框架，可由 FetchContent 自动获取）

## 使用示例 (Example)

```cpp
#include "uvcpp.h"
#include "handle/uvcpp_tcp.h"
#include <iostream>
#include <string>
using namespace uvcpp;

int main() {
    uvcpp_loop loop;
    loop.init();

    uvcpp_tcp server(&loop);
    server.bindIpv4("127.0.0.1", 8080);
    
    server.listen([&](uvcpp_stream* s, int status) {
        auto client = new uvcpp_tcp(&loop);
        s->accept(client);
        
        client->read_start(
            [](uvcpp_handle*, size_t, uv_buf_t* buf) {
                uvcpp_buf::alloc_buf(buf, 1024);
            },
            [client](uvcpp_stream*, ssize_t nread, const uv_buf_t* buf) {
                if (nread > 0) {
                    std::cout << "Received: " << std::string(buf->base, nread) << std::endl;
                }
                uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
                if (nread <= 0) {
                    client->close([client](uvcpp_handle*) { delete client; });
                }
            }
        );
    }, 128);

    loop.run(UV_RUN_DEFAULT);
    return 0;
}
```

## 变更日志 (Changelog)

### v1.1.0 (2026-09-19)

**新增**:网络层、HTTP/1.1 与 WebSocket、TLS、Web 应用框架

- 新增 `net` 模块：TCP / UDP 的服务端与客户端
- 新增 `web` 模块：HTTP/1.1（llhttp）、WebSocket（RFC 6455）、gzip/deflate 压缩、静态文件服务
- 新增 `ssl` 模块：基于 OpenSSL 的 TLS
- 新增 `webapp` 模块：路由、中间件、静态资源、流式响应、multipart 上传、文件下发、JSON、日志
- 修复内存池在 MinGW-w64 上的线程退出崩溃（根因与修法见「已知问题」），
  `expand` 模块首次随发布产物一起提供
- 预编译动态库：Windows x64 ×2（MinGW-w64 / MSVC）+ Linux x64，依赖全静态链接

### v1.0.0 (2026-02-02)

**首发版本 (Initial Release)**

- ✅ 所有 14 个功能测试通过
- ✅ 支持 Windows / Linux / macOS
- ✅ 完整的 libuv API C++ 封装
- ✅ 现代 C++ 接口设计
- ✅ 智能内存管理
- ✅ 线程池支持
- ✅ 单元测试覆盖

## 下载 (Download)

- Source code
- `libuvcpp-1.1.0-mingw-x64.zip` — Windows x64 预编译动态库（MinGW-w64）
- `libuvcpp-1.1.0-msvc-x64.zip` — Windows x64 预编译动态库（MSVC / VS2022）
- `libuvcpp-1.1.0-linux-x64.zip` — Linux x64 预编译动态库

## 已知问题 (Known Issues)

### 内存池在 v1.1.0 里修好了；CMake 默认仍是 OFF

早先 MinGW-w64 打开 `UVCPP_BUILD_EXPAND=ON` 会让 `ctest` 80 项里 **7 项**崩溃
（`0xc0000374` 堆损坏或 SegFault），而同一份源码用 MSVC 构建全绿。
**根因已定位并修复**，两个平台现在都是 80/80。

根因不属于内存池的数据结构，而在于**线程缓存由谁销毁**：

- MinGW-w64 下 DLL 里的 `thread_local` 走的是 **emutls**（libgcc 在堆上按线程
  分配的数组），带非平凡析构的 `thread_local` 对象则经 `__cxa_thread_atexit`
  登记析构。线程退出时 emutls **先**把那个数组还给了堆，登记的回调**才**被调用
  —— 于是析构函数 walk 的是已释放、已被复用的内存：读出来的 `span` 是野指针，
  紧接着在 `central_cache::push` 里对它做 CAS。写进只读页就是访问违例，写进
  可写页就是堆损坏；崩在哪个地址全看那块内存被谁捡走了。
- MSVC 没有 emutls，所以一直是对的。这解释了全部既有现象：只在 MinGW 复现、
  只在开池时复现、崩溃点在"线程退出"、改成共享运行时会更糟（libgcc 的释放
  时机变了）。

修法是**不再依赖 `thread_local` 对象自己的析构函数**：线程缓存改由 OS 的线程
存储槽管理（Windows `FlsAlloc` / POSIX `pthread_key_create`），槽的析构回调
**把缓存指针当参数收进来**，回调自身一个字节的 TLS 都不读，因此不存在
"读一个已经被释放的 TLS 槽"这回事。

因此**本版发布的两个 Windows 动态库都带内存池**（`UVCPP_BUILD_EXPAND=ON`，
产物里 `UVCPP_ENABLE_MEMORY_POOL=1`，由 `include/uvcpp/uvcpp_config.h` 带出来）。

但 **CMake 的默认值仍然是 OFF**，这是刻意的：默认开的话，使用者的 TU 忘了定义
`UVCPP_ENABLE_MEMORY_POOL`，宏求值为 0 ⇒ 使用者侧走 `std::malloc`，而 dll 侧走池,
库会拿池去 free 一个 `malloc` 的指针，**静默**堆损坏。默认关时"忘了定义"拿到的是
0，两边一致；反方向（dll 关、使用者开）则是响亮的链接错误，不会静默。
从源码构建要用池，显式传 `-DUVCPP_BUILD_EXPAND=ON`；用预编译包则**什么都不用传**
—— 包里的生成头会给出这个包实际用的值，不一致时直接 `#error`。

### 其他

- `cmake --install` 在当前树上是坏的：libuv 由 `FetchContent_MakeAvailable` 引入，
  它登记的 install 规则引用了一个从未构建的 `libuv.dll`，且它的规则排在本项目的
  规则之前 —— 一失败就整体中止，本项目的头文件与库一个都装不出来。
  本次的 zip 绕过它、照 `CMakeLists.txt:699-831` 的规则手工组装，与之有两处
  刻意的差异：**libuv 的头放在 `include/` 顶层**（本库的公开头写的是
  `#include <uv.h>`，放进 `include/libuv/` 会找不到），以及**补上了 `zlib.h` /
  `zconf.h`**（`web/uvcpp_ws_parser.h` 在 `UVCPP_ZLIB_ENABLE=1` 时要 include 它，
  但 install 规则里没有这一条）。

### MSVC 从源码树 / 安装树取头时，要自己加 `/utf-8`

**预编译包的消费者不受影响**（打包脚本给含非 ASCII 的头补了 BOM，见上面「使用方式」），
受影响的是 `find_package(uvcpp)` 与直接把 `src/` 加进 `-I` 的那条路：

- 实测 `src/` 下 99 个公开头：75 个带 BOM，24 个不带，**其中 22 个含非 ASCII** ——
  会坏的就是这 22 个。
- MSVC 读无 BOM 的 UTF-8 源码时按系统代码页（936/GBK）解码，中文注释的末字节吞掉
  换行、把 `*/` 吃掉，注释不闭合，报错却落在 `<algorithm>` 里。`C4819` 是唯一的线索
  （`warning C4819: 该文件包含不能在当前代码页(936)中表示的字符`）。
- 仓内编译看不出来，有两个原因叠加：顶层 `CMakeLists.txt` 里
  `if(MSVC) add_compile_options(/utf-8) endif()` 是**目录作用域**的 —— 导出集里
  `INTERFACE_COMPILE_OPTIONS` 是**空**的，`find_package` 的消费者拿不到它；而
  `install(FILES ...)` 是把源码树那几个头**原样**拷出去，BOM 不会凭空多出来。
- 绕行：给自己的目标加 `/utf-8`（或 `/source-charset:utf-8`）。根因是那 22 个头自己
  没 BOM，会在后续版本修。

## 感谢 (Credits)

感谢所有为这个项目做出贡献的人！

## 许可证 (License)

MIT License
