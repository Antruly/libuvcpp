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

本版本提供 **Windows x64 动态库**，由 **MinGW-w64 (GCC)** 编译：

| 文件 | 说明 |
|---|---|
| `libuvcpp.dll` | 动态库。**libuv / llhttp / zlib / OpenSSL 以及 MinGW 运行时均已静态链接进去** |
| `libuvcpp.dll.a` | 导入库（供 MinGW/GCC 链接，`-luvcpp`） |
| `include/` | 公开头文件（含 libuv、nlohmann/json、zlib 的头） |

> 命名遵循各工具链的惯例：**MinGW/GCC 产出 `libuvcpp.dll`**，MSVC 产出 `uvcpp.dll`。

`libuvcpp.dll` 的依赖只有 Windows 自带系统库（`KERNEL32` / `WS2_32` / `CRYPT32` / `ADVAPI32` /
`USER32` / `SHELL32` / `IPHLPAPI` / `USERENV` / `ole32` / `dbghelp` / `msvcrt`），
**不需要安装 MSYS2、MinGW 运行时或 VC++ 运行库**，任意 64 位 Windows 直接可用。

> ⚠️ 由 MinGW-w64 编译的动态库**不能被 MSVC 链接**（C++ ABI 不同）。
> MSVC 用户请从源码构建。库本身按 C++11 编写，源码可用 MSVC 编译（需自行准备 OpenSSL 等依赖）。

> ⚠️ **本 DLL 以 `UVCPP_BUILD_EXPAND=OFF` 构建**，即 `expand` 模块（内存池 / 页堆）
> 未编译进去，`UVCPP_ENABLE_MEMORY_POOL=0`，分配走标准 `malloc`/`free`。
> 原因见下方「已知问题」。因此 `include/` 里**没有 `expand/` 目录**，
> 使用者也**不要**把 `UVCPP_ENABLE_MEMORY_POOL` 定义成 1（`uvcpp.pc` 已钉成 0）。

### 使用方式

公开头里的 `UVCPP_*_ENABLE` 宏**必须显式传给编译器**。它们不是可选的开关：
宏未定义时 `#if` 求值为 0，`web` / `webapp` / `ssl` 的类会被整段编译掉，
使用者看到的是「类不存在」的级联语法错误；`UVCPP_ENABLE_MEMORY_POOL`
选错分支还会让使用者 TU 里的分配器与已编译的 dll 不是同一套（ABI 不一致）。
包内附了 `lib/pkgconfig/uvcpp.pc`，用 pkg-config 就不必记这些：

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
g++ -std=c++11 -I include \
    -DUVCPP_NET_ENABLE=1 -DUVCPP_WEB_ENABLE=1 -DUVCPP_WEBAPP_ENABLE=1 \
    -DUVCPP_OPENSSL_ENABLE=1 -DUVCPP_ZLIB_ENABLE=1 -DUVCPP_ENABLE_MEMORY_POOL=0 \
    your_app.cpp -L lib -luvcpp -o your_app.exe
```

（上面两条命令都在包的根目录下执行；`-L lib` 是导入库所在处。跑的时候
`bin/libuvcpp.dll` 要在 `PATH` 上，或直接拷到 exe 旁边。）

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
#include "uvcpp/uvcpp.h"
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
- Windows x64 预编译动态库（MinGW-w64，依赖全静态链接）

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
- `libuvcpp-1.1.0-mingw-x64.zip` — Windows x64 预编译动态库

## 已知问题 (Known Issues)

### MinGW 下启用内存池会让 webapp 用例崩溃

`UVCPP_BUILD_EXPAND=ON`（**项目默认值**）时，MinGW-w64 构建出的库在
`test_web_app_*` 上崩溃：`ctest` 80 项里 **7 项失败**（`0xc0000374` 堆损坏或 SegFault）。
同一份源码用 MSVC 构建、同样的开关，**80 项全绿**。

已经确认的事实：

- 崩溃点在**线程退出**的 TLS 回调里（`LdrShutdownThread` → `LdrpCallTlsInitializers`
  → `ImageTlsCallbackCaller`），出错指令是对 `libuvcpp.dll` 的 `.rdata` 段内一个
  typeinfo 对象做原子读改写 —— 即对只读页写入。
- 关掉内存池后，**同一棵树、同一批用例 100% 通过**（79/79）。
- 把 DLL 的 C++ 运行时从静态改成共享会让情况**恶化**（7 项 → 25 项失败），
  说明问题出在内存池本身，而不是运行时链接方式。

**所以本次发布的 MinGW 产物关闭了内存池（`UVCPP_BUILD_EXPAND=OFF`）。**
这是性能上的取舍，不影响 API 与功能：走标准 `malloc`/`free` 时全部用例通过。
MSVC 侧不受影响，内存池照常可用。根因仍在定位。

### 其他

- `cmake --install` 在当前树上是坏的：libuv 由 `FetchContent_MakeAvailable` 引入，
  它登记的 install 规则引用了一个从未构建的 `libuv.dll`，且它的规则排在本项目的
  规则之前 —— 一失败就整体中止，本项目的头文件与库一个都装不出来。
  本次的 zip 绕过它、照 `CMakeLists.txt:683-772` 的规则手工组装，与之有两处
  刻意的差异：**libuv 的头放在 `include/` 顶层**（本库的公开头写的是
  `#include <uv.h>`，放进 `include/libuv/` 会找不到），以及**补上了 `zlib.h` /
  `zconf.h`**（`web/uvcpp_ws_parser.h` 在 `UVCPP_ZLIB_ENABLE=1` 时要 include 它，
  但 install 规则里没有这一条）。

## 感谢 (Credits)

感谢所有为这个项目做出贡献的人！

## 许可证 (License)

MIT License
