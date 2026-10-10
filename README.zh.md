<p align="center">
  <img src="./uvcpp.svg" alt="libuvcpp logo" width="160" height="160">
</p>

[![版本](https://img.shields.io/badge/version-1.5.3--dev-blue.svg)](./RELEASE.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![CI · Linux · Ubuntu](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml)
[![CI · Windows · MSVC](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml)
[![CI · Windows · MinGW-w64](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml)
[![CI · macOS](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml)

# libuvcpp

🔧 基于 [libuv](https://github.com/libuv/libuv) 的现代 C++11 封装库 — 面向对象的异步 I/O，
支持双模式（异步回调/同步等待）、HTTP/1.1、WebSocket（RFC 6455）和 SSL/TLS。

- **版本**：`1.5.3-dev` — **作者**：`zhuweiye` — **许可证**：`MIT`
- **语言**：[English](./README.md) · [中文](./README.zh.md)

---

## 概述

libuvcpp 在 libuv 的事件循环、句柄和请求之上提供了一层薄而符合 C++ 习惯的封装。
它在保持 libuv 性能的同时，增加了 RAII 资源管理、`std::function` 回调，
以及面向 TCP、UDP、HTTP 和 WebSocket 的高级客户端/服务端抽象。

库按层次模块组织：

```
应用层
┌────────────┐
│ web (HTTP/WS) │  ← uvcpp_http_client/server, uvcpp_ws_client/server
├────────────┤
│ http2        │  ← h2 会话/连接层、nghttp2 胶水（`UVCPP_ENABLE_NGHTTP2=ON`）
├────────────┤
│ ssl (TLS)    │  ← uvcpp_ssl, uvcpp_ssl_context (OpenSSL 封装)
├────────────┤
│ quic + http3 │  ← uvcpp_quic_client/server/connection、ngtcp2 胶水（net 层，`UVCPP_ENABLE_QUIC=ON`）；h3 架在它上面，nghttp3 胶水（web 层，`UVCPP_ENABLE_HTTP3=ON`）
├────────────┤
│ net          │  ← uvcpp_tcp_client/server, uvcpp_udp_client/server
├────────────┤
│ handle + req │  ← uvcpp_loop, uvcpp_tcp, uvcpp_timer, uvcpp_write, ...
├────────────┤
│ uvcpp core   │  ← uvcpp_buf, uvcpp_thread, uvcpp_alloc, ...
└────────────┘
```

---

## 功能特性

### 核心模块（handle + req）

| 类别 | 类 |
|----------|---------|
| **事件循环** | `uvcpp_loop` |
| **流式句柄** | `uvcpp_tcp`, `uvcpp_pipe`, `uvcpp_udp`, `uvcpp_tty` |
| **定时器与钩子** | `uvcpp_timer`, `uvcpp_idle`, `uvcpp_prepare`, `uvcpp_check` |
| **信号与进程** | `uvcpp_signal`, `uvcpp_process` |
| **文件系统** | `uvcpp_fs`, `uvcpp_fs_event`, `uvcpp_fs_poll` |
| **轮询** | `uvcpp_poll`, `uvcpp_async` |
| **请求** | `uvcpp_write`, `uvcpp_connect`, `uvcpp_shutdown`, `uvcpp_work`, `uvcpp_getaddrinfo`, `uvcpp_getnameinfo`, `uvcpp_udp_send`, `uvcpp_random` |

### 工具类（`src/uvcpp/`）

线程池、读写锁、屏障、CPU 信息、网络接口、用户/组信息、目录遍历、
环境变量、缓冲区管理（`uvcpp_buf`）、性能指标、分配器集成，
以及 JSON 构造（`uvcpp_json_writer`）。

### Expand 模块（`src/expand/`）

TCMalloc 风格的内存池：页堆、span 分配器、线程缓存、enterprise 分配器。
v1.1.0 起**默认关闭**（`UVCPP_BUILD_EXPAND=OFF`）—— 要启用需显式传
`-DUVCPP_BUILD_EXPAND=ON`。预编译产物是**带池**发布的；使用者**什么都不用传**
—— 包里的 `uvcpp/uvcpp_config.h` 给出这个包实际用的值，自己再定义成别的值会直接
`#error`，而不是静默的分配器错配（详见 `RELEASE.md`）。

每份预编译包里还多一档**调试版**动态库（`uvcppd.dll` / `libuvcppd.so`，用
`-luvcppd` 链，MSVC 那份还带 `uvcppd.pdb`），方便单步进库内部。它的前提 —— 主要是
MSVC 那份不可再分发的调试版运行库**不在包里** —— 写在
[`RELEASE.md`](./RELEASE.md#调试档debug-版) 里。

### Net 模块（`src/net/`）— `UVCPP_BUILD_NET=ON`（默认）

| 类 | 说明 |
|-------|-------------|
| `uvcpp_tcp_client` | 高级 TCP 客户端，双模式 API（异步回调 / 同步 `wait()` 带超时） |
| `uvcpp_tcp_server` | TCP 服务端，`bind()` + `listen(连接回调, backlog)`（连接回调是 `listen()` 的第一个参数，没有单独的 `on_connection()`） |
| `uvcpp_udp_client` | UDP 客户端，双模式发送/接收 |
| `uvcpp_udp_server` | UDP 服务端，`bind()`/`recv_start()` |

### Web 模块（`src/web/`）— `UVCPP_BUILD_WEB=ON`

| 类 | 说明 |
|-------|-------------|
| `uvcpp_http_client` | HTTP 客户端，支持 keep-alive、流式解析、双模式 `send()`/`send_wait()`。默认 HTTP/1.1，`set_http2_enabled()` 显式开 h2 |
| `uvcpp_http_server` | HTTP 服务端，路由注册、每连接解析器、Upgrade 检测。默认 HTTP/1.1，`set_http2_enabled()` 显式开 h2 |
| `uvcpp_http_parser` | 流式 HTTP 解析器（封装 [llhttp](https://github.com/nodejs/llhttp)），PIMPL 模式 |
| `uvcpp_http_request` | 请求对象，`to_string()` / `from_parser()` 序列化 |
| `uvcpp_http_response` | 响应对象，工厂方法（`ok()`, `not_found()` 等） |
| `uvcpp_ws_client` | WebSocket 客户端（RFC 6455），解析 `ws://`/`wss://` URL，Upgrade 握手 |
| `uvcpp_ws_server` | WebSocket 服务端，通过 `on_upgrade()` 从 HTTP 自动升级 |
| `uvcpp_ws_connection` | WebSocket 连接：`send_text()`/`send_binary()`/`send_ping()`/`send_close()` |
| `uvcpp_ws_parser` | 流式 WebSocket 帧解析器（RFC 6455），8 状态状态机 |
| `uvcpp_ws_frame` | 帧结构体，opcode、mask、close-code 辅助方法 |
| `uvcpp_http_common` | HTTP 方法/状态码枚举、版本枚举（HVER_10/11/20）、header 辅助函数 |

**可选功能**（需显式开启，不会自动启用）：

- `UVCPP_ENABLE_ZLIB=ON` — WebSocket 压缩扩展（RFC 7692, Per-Message Deflate）
- `UVCPP_ENABLE_OPENSSL=ON` — HTTPS/WSS 通过 SSL/TLS 模块
- `UVCPP_ENABLE_NGHTTP2=ON` — HTTP/2（RFC 9113），基于
  [nghttp2](https://github.com/nghttp2/nghttp2)（静态链入）。需要
  `UVCPP_ENABLE_OPENSSL=ON`（缺了强制关）—— 本库的 h2 **只走 TLS + ALPN**：
  不做 h2c、不做 prior-knowledge、不做 RFC 8441、不做 `:protocol`。它**默认关，
  且没有任何自动升级**：`uvcpp_http_server` 要 `set_http2_enabled(true)` **并且**在
  SSL 上下文上显式 `set_alpn_select_protos({"h2","http/1.1"})`；`uvcpp_http_client`
  只要 `set_http2_enabled(true)`（ALPN 名单由 `connect()` 每条连接现拼）；
  `uvcpp_web_app` 那两层都已经替你接好。详见
  [webapp 指南 §13](doc/webapp-guide.md#13-http2)。
- `UVCPP_ENABLE_QUIC=ON` — QUIC 传输（RFC 9000），基于
  [ngtcp2](https://github.com/ngtcp2/ngtcp2)（静态链入），在 **net 层**。需要
  `UVCPP_ENABLE_OPENSSL=ON` **以及**一份**带 QUIC API 的 OpenSSL ≥ 3.2**，再加上
  `UVCPP_BUILD_NET=ON`（三条缺一即强制关 —— 没有明文 QUIC 这回事）。**1.4.1 起它是
  一条真能通信的链路协议**：握手、流收发、连接关闭与空闲超时都通了。HTTP/3 架在它
  上面 —— 见下一条。详见 [doc/quic-guide.md](doc/quic-guide.md)。
- `UVCPP_ENABLE_HTTP3=ON` — **web 层**的 HTTP/3（RFC 9114），解析用
  [nghttp3](https://github.com/ngtcp2/nghttp3)（注意组织是 `ngtcp2`，不是 `nghttp2`），
  跑在 QUIC 传输之上，两者都静态链入。需要 `UVCPP_ENABLE_QUIC=ON` 与
  `UVCPP_BUILD_WEB=ON`（缺一即强制关闭）。**1.4.1 起 `uvcpp_http_client` /
  `uvcpp_http_server` 透明地说它** —— 客户端 `set_http3_enabled(true)`，服务端
  `listen_quic()`，而路由表、处理函数签名、`uvcpp_http_response` 与 h1/h2 是**同
  一份**。HTTP/3 跑在 **UDP 自己的 socket 上**，所以 **HTTP/1.1 与 HTTP/2 一个字节
  没动**（开关关掉时 h1 那条路的编译命令与符号尺寸逐字节相同）。详见
  [doc/http3-guide.md](doc/http3-guide.md)。

### Web 应用框架（`src/webapp/`）— `UVCPP_BUILD_WEBAPP=ON`

建在 web 模块之上的**应用层**：**写业务 handler 就行，不用拼报文。** 需要
`UVCPP_BUILD_WEB=ON`（web 关掉时会强制关掉 webapp，而不是留一个编译不过的配置）。
JSON 后端用 FetchContent 拉 [nlohmann/json](https://github.com/nlohmann/json)。

| 类 | 说明 |
|-------|-------------|
| `uvcpp_web_app` | 应用本体：配置、路由、中间件、生命周期（`start`/`stop`/`join`） |
| `uvcpp_web_router` | 模式路由 —— `/user/:id` 参数、`/files/*path` 通配，静态 > 参数 > 通配 |
| `uvcpp_web_request` / `uvcpp_web_response` | 请求/响应封装：查询串、表单、Cookie、路径参数、状态码 helper、chunked、`send_file` |
| `uvcpp_web_context` | 每请求上下文：中间件链、`post()`、`hold()`/`release()`、用户数据 |
| `uvcpp_web_static` | 静态文件服务：ETag、Last-Modified、Range/206/416、LRU 缓存、SPA 回落、dotfile 策略 |
| `uvcpp_web_upload` / `uvcpp_web_multipart` | multipart 流式落盘，随机叶子名 + 六条大小上限 |
| `uvcpp_web_stream` | 请求体流式接收（`on_data`/`on_end`/`pause`/`resume`） |
| `uvcpp_web_ws` | 应用上的 WebSocket 端点（`app.websocket("/chat/:room", handler)`） |
| `uvcpp_web_ws_client` | WebSocket 客户端：回调装在客户端上 + 可选**自动重连** |
| `uvcpp_web_work_limit` | 工作线程池准入闸门（`uv_queue_work` 的背压） |
| `uvcpp_log` / `uvcpp_log_console` | 两级日志：等级 + 模块，sink 可插拔 |
| `uvcpp_web_json` | nlohmann/json 的收口层 —— 不让异常穿透 libuv 回调、深度预扫描 |

```cpp
#include <webapp/uvcpp_web_app.h>
using namespace uvcpp;

int main() {
  uvcpp_web_app app;
  app.set_port(8080)
     .use(web_middleware_access_log())
     .use(web_middleware_cors());

  app.get("/hello", [](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    resp.json_str("{\"hello\":\"world\"}");
    resp.end();
  });

  app.serve_static("/assets", "./public");
  app.websocket("/echo", [](uvcpp_web_ws_request& ws) {
    uvcpp_ws_connection* c = ws.connection();
    c->on_text([c](const std::string& m) { c->send_text(m.c_str(), m.size()); });
  });

  app.start();   // bind 之后在后台线程跑事件循环
  app.join();
  return 0;
}
```

**→ 完整指南：[doc/webapp-guide.md](doc/webapp-guide.md)** —— 路由规则、中间件、
静态/上传/流式、WebSocket 客户端与重连、安全、已知限制。可运行示例：
`examples/webapp_demo.cpp`。

### SSL 模块（`src/ssl/`）— `UVCPP_ENABLE_OPENSSL=ON`

| 类 | 说明 |
|-------|-------------|
| `uvcpp_ssl_context` | SSL/TLS 上下文（封装 `SSL_CTX*`），证书/密钥加载，自签名证书生成 |
| `uvcpp_ssl` | 每连接 SSL 封装，`handshake()`/`read()`/`write()`/`shutdown()` |
| `uvcpp_ssl_common` | `tls_version`, `tls_mode`, `tls_verify_mode`, `tls_cert_info` 枚举/结构体 |

### 模块指南

上面每张表是**有什么类**；下面这几篇讲**怎么用** —— 典型流程、错误处理、以及
头注释与实现对不上的地方。全部在 `doc/` 下，**每一段的示例都被 CI 逐条编译过**
（`tests/tools/check_doc_snippets.py`），可以照抄。

| 模块 | 指南 | 讲什么 |
|---|---|---|
| 低层（handle + req） | [doc/lowlevel-guide.md](doc/lowlevel-guide.md) | 事件循环、句柄与请求的生命周期、`<uvcpp.h>` 到底聚合了什么 |
| JSON（核心） | [doc/json-guide.md](doc/json-guide.md) | 手写 JSON 响应：转义契约、失败语义、各种上限，以及怎么接到响应层 |
| JSON（反射） | [doc/json-reflect-guide.md](doc/json-reflect-guide.md) | 用一个宏标出字段表、两个方向共用：分层与包含、读入语义、错误报告，以及 C++11 化多出来的那件手工活 |
| net | [doc/net-guide.md](doc/net-guide.md) | TCP/UDP 客户端与服务端；异步与同步双模式，以及两者不能混用的地方 |
| web（HTTP 半边） | [doc/web-http-guide.md](doc/web-http-guide.md) | HTTP 服务端/客户端/解析器/静态服务；关服为什么是两步 |
| web（WS 半边） | [doc/web-ws-guide.md](doc/web-ws-guide.md) | WebSocket 握手、帧、关闭码 |
| webapp（框架） | [doc/webapp-guide.md](doc/webapp-guide.md) | 路由、中间件、请求响应、上传、静态服务、WebSocket、日志 |
| webapp（支撑类型） | [doc/webapp-support-guide.md](doc/webapp-support-guide.md) | 框架底下那七个类型：连接身份、web 工具函数、MIME、multipart、文件下发、每请求上下文、控制台日志 |
| ssl | [doc/ssl-guide.md](doc/ssl-guide.md) | TLS 上下文与每连接封装 —— 头文件最短、最容易写错的一层 |
| http2 | [doc/http2-guide.md](doc/http2-guide.md) | 低层会话/连接层的用法；实现进度与折衷另见 [doc/http2-status.md](doc/http2-status.md) |
| quic | [doc/quic-guide.md](doc/quic-guide.md) | QUIC 传输：构建契约、为什么非 OpenSSL ≥ 3.2 不可、API 形状，以及一份如实列出"还没做"的清单 |
| http3 | [doc/http3-guide.md](doc/http3-guide.md) | 作为 web 层第二条传输的 HTTP/3：它向 QUIC 要的那三样扩展、API 形状（会话 vs 连接、三条关键流）、CMake 接线，以及为什么它不影响 HTTP/1.1 |
| expand | [doc/expand-guide.md](doc/expand-guide.md) | 内存池、页堆、span，以及它们默认关着的理由 |
| WSDL（文档 + 发布） | [doc/wsdl-guide.md](doc/wsdl-guide.md) | 把 WSDL 1.1 文档解析成模型、按 QName 查它、发出去或从模型生成一份 |
| SOAP（信封 + 派发） | [doc/soap-guide.md](doc/soap-guide.md) | 1.1 与 1.2 的信封与 `soap:Fault`、从 binding 推出来的派发键、九种拒绝各算谁的错，以及响应包装元素为什么不是派发键的对称 |
| db（SQLite / MySQL / PostgreSQL） | [doc/db-guide.md](doc/db-guide.md) | 一个连接一个 `uvcpp_db_client`、三个后端、连接串的语法、返回码表、共用套件钉住的那一条跨后端契约（以及三家**真不一样**的三处）、参数绑定、事务与"事务里不重试"的规矩、`DECIMAL` 要付的代价、建在同一条连接上的异步门面（`uvcpp_db_async`，回调 + future）与可选的连接池，以及怎么对着真服务端跑测试 |
| C ABI（`uvcpp_c_*`） | [doc/capi-guide.md](doc/capi-guide.md) | 给 C# / P-Invoke 与其它 FFI 的 `extern "C"` 面：选项与守卫链、五条承重契约（错误码、回调表 `size`、所有权三类、线程规则、ABI 版本）、每个模块提供什么与**明确不提供**什么，以及那张变异表实际量到了什么。**1.5.3 起八片全部就位**（地基 + net + webapp/web + HTTP/2 + QUIC + HTTP/3 + db，共 402 个函数），只差一个 `-DUVCPP_ENABLE_CAPI=ON`（db 那一片还要它自己的 `-DUVCPP_ENABLE_DB=ON` 加一个后端）。**1.5.0 起六条发布腿的预编译包都带着它**，另有 [`bindings/csharp/`](bindings/csharp/README.md) 那份 C# 绑定（覆盖前七片那 321 个，db 那 81 个是如实列出的缺口）|

模块之外还有：[doc/benchmark.md](doc/benchmark.md) 性能实测读数、
[doc/build-guide.md](doc/build-guide.md) 构建开关与构建树、
[doc/testing-guide.md](doc/testing-guide.md) 测试分层、
[doc/ci-guide.md](doc/ci-guide.md) CI 维护、
[doc/release-process.md](doc/release-process.md) 发布流程与它不检查什么。

---

## 编译要求

- **C++11** 或更高版本
- **CMake** ≥ 3.20
- **编译器**：MSVC 2019+, GCC 7+, Clang 5+
- **libuv**：如系统未安装，通过 FetchContent 自动拉取并构建
- **支持平台**：Windows、Linux、macOS

---

## 构建

### 基本构建（仅核心）

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON
cmake --build . --config Release --parallel
ctest --output-on-failure -C Release
```

### 启用 Web 模块（HTTP + WebSocket）

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON \
  -DUVCPP_BUILD_WEB=ON
cmake --build . --config Release --parallel
```

### 完整构建（Web + SSL + 压缩）

```bash
# Linux: 先安装系统依赖
sudo apt-get install libssl-dev zlib1g-dev

cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON \
  -DUVCPP_BUILD_WEB=ON \
  -DUVCPP_ENABLE_OPENSSL=ON \
  -DUVCPP_ENABLE_ZLIB=ON
cmake --build . --config Release --parallel
```

### 加上 Web 应用框架

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON \
  -DUVCPP_BUILD_WEB=ON \
  -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_BUILD_EXAMPLES=ON
cmake --build . --config Release --parallel
# 可运行示例（自校验）：
./examples/Release/webapp_demo
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|--------|---------|-------------|
| `UVCPP_BUILD_TESTS` | `ON` | 构建测试可执行文件 |
| `UVCPP_BUILD_FUNCTIONAL` | `ON` | 构建功能测试套件（只在 `UVCPP_BUILD_TESTS=ON` 时被读） |
| `BUILD_SHARED_LIBS` | `ON` | 构建动态库 |
| `UVCPP_BUILD_STATIC` | 自动 | 构建静态 uvcpp 库 |
| `UVCPP_BUILD_SHARED` | 自动 | 构建动态 uvcpp 库 |
| `UVCPP_FOLLOW_LIBUV_BUILD` | `ON` | `UVCPP_BUILD_SHARED`/`UVCPP_BUILD_STATIC` 保持「自动」，跟随 libuv 自己的 `BUILD_SHARED_LIBS` |
| `UVCPP_BUILD_EXPAND` | `OFF` | 构建 expand 模块（内存池） |
| `UVCPP_BUILD_NET` | `ON` | 构建 net 模块（TCP/UDP 客户端/服务端） |
| `UVCPP_BUILD_WEB` | `OFF` | 构建 web 模块（HTTP + WebSocket） |
| `UVCPP_BUILD_WEBAPP` | `OFF` | 构建 web 应用框架（路由/中间件/静态/上传/日志）。需要 `UVCPP_BUILD_WEB=ON` |
| `UVCPP_BUILD_EXAMPLES` | `OFF` | 构建 `examples/` 下的示例 |
| `UVCPP_BUILD_BENCH` | `OFF` | 构建 `bench/` 下的压测靶场 —— 见 [`doc/benchmark-rig.md`](doc/benchmark-rig.md) |
| `UVCPP_ENABLE_ZLIB` | `OFF` | 启用 zlib（WebSocket 压缩） |
| `UVCPP_ENABLE_OPENSSL` | `OFF` | 启用 OpenSSL（HTTPS/WSS） |
| `UVCPP_ENABLE_NGHTTP2` | `OFF` | 启用 HTTP/2（nghttp2，静态链入）。需要 `UVCPP_ENABLE_OPENSSL=ON` 与 `UVCPP_BUILD_WEB=ON` |
| `UVCPP_ENABLE_QUIC` | `OFF` | 启用 **net 层**的 QUIC 传输（ngtcp2，静态链入）。需要 `UVCPP_ENABLE_OPENSSL=ON`、**一份带 QUIC API 的 OpenSSL ≥ 3.2**，以及 `UVCPP_BUILD_NET=ON` —— 缺一即强制关闭。**1.4.1 是一条真能通信的链路协议：握手、流收发、关闭与空闲超时都通了；HTTP/3 架在它上面（下一行）。** **1.5.0 起六条发布腿的预编译包都打开它**（此前发布包不带 QUIC）。见 [`doc/quic-guide.md`](doc/quic-guide.md) |
| `UVCPP_ENABLE_HTTP3` | `OFF` | 启用 **web 层**的 HTTP/3（RFC 9114），由 nghttp3 解析，跑在 QUIC 传输之上（两者都静态链入）。需要 `UVCPP_ENABLE_QUIC=ON` 与 `UVCPP_BUILD_WEB=ON` —— 缺一即强制关闭。**1.4.1 是一条端到端可用的传输：`uvcpp_http_client` / `uvcpp_http_server` 都说它，而 h1/h2 一个字节没动（它跑在 UDP 上）。** **1.5.0 起六条发布腿的预编译包都打开它**（此前发布包不带 HTTP/3）。见 [`doc/http3-guide.md`](doc/http3-guide.md) |
| `UVCPP_ENABLE_WSDL` | `OFF` | 启用 WSDL/SOAP 模块（XML 后端 pugixml，静态链入）。需要 `UVCPP_BUILD_WEBAPP=ON`。见 [`doc/wsdl-guide.md`](doc/wsdl-guide.md)（文档那一半）与 [`doc/soap-guide.md`](doc/soap-guide.md)（运行时那一半） |
| `UVCPP_ENABLE_CAPI` | `OFF` | 导出 **C ABI**（`src/capi/`，C99 头，给 C#/P-Invoke 以及别的 FFI 用），编进**同一个** `uvcpp` 库 —— 不多一个产物。需要 `UVCPP_BUILD_NET=ON` 与 `UVCPP_BUILD_WEB=ON`，缺一即强制关闭（C 面横跨 net/web/webapp）。**每一个发布配置都打开它。** 见 [`doc/capi-guide.md`](doc/capi-guide.md) |
| `UVCPP_ENABLE_DB` | `OFF` | 启用**数据库模块**（`src/db/`）：一个连接一个 `uvcpp_db_client`，底下是 SQLite / MySQL / PostgreSQL 三个后端，结果行按表的方式取。它是全仓**唯一一个必需第三方客户端库**（libsqlite3 / libmysqlclient / libpq）的模块 —— 三个都找不到就强制关闭。**每一个发布配置都打开它**，所以预编译包里带着这个模块。见 [`doc/db-guide.md`](doc/db-guide.md) |
| `UVCPP_ENABLE_DB_SQLITE` | `ON` | 编 db 模块的 SQLite 后端（要 `sqlite3.h` 与 libsqlite3）。找不到时**只强制关掉这一个**并出声，其余后端照编 |
| `UVCPP_DB_SQLITE_FROM_SOURCE` | `OFF` | 这个 SQLite 后端改成用一份**钉死哈希的源码 amalgamation** 自己编（一个静态 `sqlite3.c`，强制 PIC），不走 `find_package(SQLite3)`。**六条发布腿走的就是它**：链系统的 `libsqlite3.so` 会多一条 `DT_NEEDED`、破坏发布包的"自包含"承诺，而 Ubuntu 24.04 上系统的 `libsqlite3.a` 不是 PIC（实测 `R_X86_64_PC32 … can not be used when making a shared object`）。配置期需要联网，或者自己把 zip 放到位 —— 两种做法都写在 [`doc/db-guide.md`](doc/db-guide.md) 里 |
| `UVCPP_ENABLE_DB_MYSQL` | `ON` | 编 MySQL 后端（要 `mysql.h` 与 libmysqlclient）。装在非标准位置时：`-DCMAKE_PREFIX_PATH=…` 或 `-DUVCPP_DB_MYSQL_INCLUDE_DIR=… -DUVCPP_DB_MYSQL_LIBRARY=…` |
| `UVCPP_ENABLE_DB_PGSQL` | `ON` | 编 PostgreSQL 后端（要 `libpq-fe.h` 与 libpq）。装在非标准位置时：`-DCMAKE_PREFIX_PATH=…` 或 `-DPostgreSQL_INCLUDE_DIR=… -DPostgreSQL_LIBRARY=…` |
| `UVCPP_USE_SYSTEM_LIBUV` | `ON` | 优先使用系统安装的 libuv |
| `UVCPP_BUILD_LIBUV_FROM_SOURCE` | `OFF` | 用 `FetchContent` 拉取并源码构建 libuv |
| `UVCPP_STATIC_RUNTIME` | `OFF` | 把编译器运行时（`libgcc`/`libstdc++`）静态链进库。**仅 MinGW 与 Linux 有效，MSVC 上是空操作** —— MSVC 用 `/MD`，发布包里自带 `vcruntime`/`msvcp` |
| `UVCPP_ENABLE_TRY_WRITE` | `ON` | 拷贝进写缓冲之前先试一次 `uv_try_write` |
| `UVCPP_TRY_WRITE_MIN_BYTES` | `32768` | 值得尝试 `uv_try_write` 的最小载荷（是 `CACHE STRING`，不是 `option()`） |
| `UVCPP_ENABLE_UDP_GSO` | Windows **且** `UVCPP_ENABLE_QUIC=ON` 时为 `ON`，否则 `OFF` | QUIC 传输的发送侧 UDP 分段卸载：把一批等长的数据报用 `UDP_SEND_MSG_SIZE` 在**一次** `WSASendTo` 里交给协议栈，而不是每个数据报一次系统调用。分段尺寸取自每次聚合写回填的值，所以线上跑的还是今天那些数据报。**仅 Windows** —— 其余平台这段代码被编掉，开关在那里是空操作。见 [`doc/quic-guide.md`](doc/quic-guide.md) |

**注意**：开启 `UVCPP_BUILD_WEB=ON` 不会自动启用 `UVCPP_ENABLE_ZLIB` 或 `UVCPP_ENABLE_OPENSSL`。
这些选项需要显式手动开启。`UVCPP_ENABLE_NGHTTP2` 在 `UVCPP_ENABLE_OPENSSL=OFF` 时
**强制关闭**（给一条 warning，而不是留一个根本跑不起来的配置）—— 本库的 HTTP/2
没有明文形态。`UVCPP_ENABLE_QUIC` 同理，而且它有**三个**各自独立的前置条件
（`UVCPP_ENABLE_OPENSSL=OFF`、`UVCPP_BUILD_NET=OFF`、或者那份 OpenSSL 没有 QUIC API
—— 3.2 以下都属于这一类），缺哪个就为哪个打一条说明**怎么修**的 warning。`UVCPP_ENABLE_HTTP3` 同理，它有
**两个**前置条件（`UVCPP_ENABLE_QUIC=OFF` 或 `UVCPP_BUILD_WEB=OFF`），也各自有 warning。
`UVCPP_ENABLE_WSDL` 同理，在 `UVCPP_BUILD_WEBAPP=OFF` 时**强制关闭**（它建在 webapp
之上）。

---

## 快速入门示例

### TCP Echo 服务端

```cpp
#include "uvcpp.h"
#include "net/uvcpp_tcp_server.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_tcp_server server;
    server.bind("127.0.0.1", 8080);

    // 所有连接共用这一个读回调：数据、对端关闭、读错误是三个明确的事件，
    // 不用再去猜"数据怎么不来了"。
    server.set_read_callback([](uvcpp::uvcpp_tcp_client& client,
                                const uvcpp::net_read_result& r) {
        if (r.is_data()) {
            std::cout << "收到: " << std::string(r.data, r.size) << std::endl;
            // 传了回调才是异步写；不传等价于 write_wait()，会在 loop 线程上等死。
            client.write(r.data, r.size, [](int) {});
        } else {
            std::cout << "客户端断开" << std::endl;
        }
    });

    // 连接回调是 listen() 的第一个参数，不是另一个 on_connection()。
    // 读回调要在 listen() 之前设好。
    server.listen([](uvcpp::uvcpp_tcp_client* client) {
        std::cout << "客户端已连接" << std::endl;
    });

    std::cout << "Echo 服务运行在 :8080" << std::endl;
    server.run();
    return 0;
}
```

### HTTP GET 请求

```cpp
#include "web/uvcpp_http_client.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_http_client client;

    client.get("http://httpbin.org/get",
               [](const uvcpp::uvcpp_http_response& rsp, int err) {
        if (err) return;
        std::cout << "状态码: " << static_cast<int>(rsp.status_code) << std::endl;
        std::cout << "响应体: "
                  << std::string(rsp.body.get_const_data(), rsp.body.size())
                  << std::endl;
    });

    client.run();
    return 0;
}
```

### WebSocket 客户端

```cpp
#include "web/uvcpp_ws_client.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_ws_client client;

    client.connect("ws://echo.websocket.org/chat",
        [](uvcpp::uvcpp_ws_connection* conn, int err) {
            if (err) return;

            conn->on_text([](const std::string& msg) {
                std::cout << "回显: " << msg << std::endl;
            });

            const std::string msg = "Hello WebSocket!";
            conn->send_text(msg.c_str(), msg.size());
        });

    client.run();
    return 0;
}
```

### WebSocket 服务端

```cpp
#include "web/uvcpp_ws_server.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_ws_server server;
    server.bind("127.0.0.1", 8080);

    server.on_connection([](uvcpp::uvcpp_ws_connection* conn) {
        std::cout << "WS 客户端已连接" << std::endl;

        conn->on_text([conn](const std::string& msg) {
            std::cout << "收到: " << msg << std::endl;
            const std::string reply = "回显: " + msg;
            conn->send_text(reply.c_str(), reply.size());
        });

        // 会话结束是 (关闭码, 原因) 两个参数，不是"一个连接指针"。
        conn->on_close([](uvcpp::ws_close_code code, const std::string& reason) {
            std::cout << "WS 客户端断开: "
                      << static_cast<int>(code) << " " << reason << std::endl;
        });
    });

    server.listen();
    server.run();
    return 0;
}
```

### SSL/TLS 服务端

```cpp
#include "ssl/uvcpp_ssl_context.h"
#include "net/uvcpp_tcp_server.h"

int main() {
    // 创建 SSL 上下文
    uvcpp::uvcpp_ssl_context ssl_ctx(uvcpp::tls_mode::SERVER);
    ssl_ctx.generate_self_signed("localhost");  // 或 load_certificate_file()

    uvcpp::uvcpp_tcp_server server;
    server.set_ssl_context(&ssl_ctx);
    server.bind("127.0.0.1", 8443);
    // ... set_read_callback, listen, run
}
```

---

## 测试

```bash
# 构建并测试
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON
cmake --build build --config Release --parallel

# 运行全部测试
ctest --test-dir build --output-on-failure -C Release

# 仅运行 web 模块测试
ctest --test-dir build -C Release -R "web_"

# 排除特定测试（test_shutdown_func 约 1% 概率失败——是 flake 不是挂死，详见 doc/ci-guide.md §4）
ctest --test-dir build -C Release --exclude-regex "test_shutdown_func"
```

### 可选门禁：完整页堆（PageHeap）跑一遍全量

裸跑时 `free` 掉的内存页还在、内容还是旧的，**释放后使用是静默的**。完整页堆把
释放过的块立刻 unmap，同一个读当场变成访问违例 —— 它曾在普通构建 / 无内存池构建 /
单元测试三层验收全绿的同时，一次抓出 8 个用例的 use-after-free。

```bash
# 需要 Windows SDK 的调试工具 gflags.exe（通常在管理员终端里跑）
python -u tests/tools/run_pageheap_gate.py --tree build-webapp
```

它先裸跑一遍拿基线，再逐个用例「开页堆 → 回查注册表 → 跑 → 关页堆 → 回查」，
只把「基线绿、页堆崩」算抓到。退出码 `0` 全绿、`1` 门禁不通过、`3` 门禁自身没跑成
（基线红 / 页堆没设上 / 没关干净 / 抓到的是过期 DLL）。

**它显著变慢**，所以不进 ctest 默认套件。页堆在每个用例的 `finally` 里关掉，另有
`atexit` 与 `Ctrl-C` 兜底 —— 残留会让这台机器上后面所有测试都慢一个量级。

测试覆盖：
- **单元测试**：`tests/unit/` — 句柄类型、请求类型、uvcpp 工具类
- **功能测试**：`tests/functional/` — 所有模块的运行时行为
- **Expand 测试**：`tests/expand/` — 内存池分配测试
- **测试工具**：`tests/tools/` — 变异脚本与门禁（变异如 `mutate_ws_client.py`、
  `run_tcp_client_dtor_mutation.py`；门禁如 `run_pageheap_gate.py`）

---

## 多循环横向扩展（`set_loops`）

一条事件循环最多只能占满一个核。那个核吃满之后，`set_loops(n)` 会在**同一个进程内**起
**1 条接受者循环 + n−1 条工作循环**：接受这条路留在一个线程上，连接的 I/O 摊到各条工作循环。

```cpp
#include <webapp/uvcpp_web_app.h>
using namespace uvcpp;

int main() {
  uvcpp_web_app app;
  app.set_host("0.0.0.0").set_port(8080).set_access_log(false);

  // 1 条接受者 + 3 条工作循环。不能链式，且必须在 start()/run() 之前。
  const int rc = app.set_loops(4);
  if (rc != 0) return 1;

  app.get("/json", [](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    resp.json_str("{\"hello\":\"world\"}");
    resp.end();
  });

  app.start();   // n > 1 只能 start()/start_background()，run(md) 会被拒
  app.join();
  return 0;
}
```

打开它之前值得知道这几条：

- **在 `start()` / `run()` 之前调用。** 路由注册也要在 `start()` 之前 —— `n > 1` 时这条
  从「建议」变成「必须」。
- **`set_loops` 返回 `int`，所以不能接在 `set_host(...).set_port(...)` 这条链上。**
  成功返 `0`；`n` 不在 `1..64` 内返 `UV_EINVAL`；运行时已经起来过返 `UV_EBUSY`。
- **`n == 1` 与「从不调用它」逐字节相同** —— 不建格子、不装钩子、不多起线程。
  档位拧到 1 不付任何代价。
- **`n > 1` 时 `run(md)` 会被 `UV_EINVAL` 拒掉。** 用 `start()` / `start_background()`，
  收尾用 `stop()` 与 `join()`。
- **连接落在哪条循环上是平台性质，而且可以问出来。** `n > 1` 时 `is_fanout()`
  回答这件事：平台接受 `UV_TCP_REUSEPORT`（Linux）时每条循环**含下标 `0`** 各自绑
  一个监听口抢同一个端口，内核按四元组哈希把新连接分给它们 ⇒ `connection_count_at(0)`
  通常是**非 0**，而除"每条连接只被记在一格"之外不作任何保证；Windows（以及那个
  标志被拒的平台）上下标 `0` 是纯接受者，按显式轮转把每条连接交给 `1..n-1`，所以
  它恒为 0。`loop_count()` 报一共有几条循环，`connection_count_at(i)` 报每格各有多少。
  **想在 Linux 上验另一条腿**：`set_handoff_forced(true)`（**必须在 `set_loops()` 之前调**）
  强制走转手 —— 这是**测试用的口子**，把内核分流换成用户态转手，生产上别用。

**→ 档位怎么选、核怎么钉、以及什么样的扩展性读数算数或不算数：
[doc/benchmark-rig.md](doc/benchmark-rig.md)。**

---

## 项目结构

```
libuvcpp/
├── src/
│   ├── uvcpp/     # 核心工具（buf, thread, version, alloc, ...）
│   ├── handle/    # libuv 句柄封装（loop, tcp, udp, timer, ...）
│   ├── req/       # libuv 请求封装（write, connect, fs, work, ...）
│   ├── expand/    # 内存池（page heap, span, enterprise allocator）
│   ├── net/       # TCP/UDP 客户端/服务端
│   ├── web/       # HTTP 客户端/服务端, WebSocket 客户端/服务端, 帧解析器
│   ├── webapp/    # Web 应用框架（路由、中间件、静态、上传、WS 客户端、日志）
│   ├── http2/     # HTTP/2 会话/连接层、nghttp2 胶水、ALPN（uvcpp_h2_nghttp2.h 是私有头）
│   ├── quic/      # QUIC 传输、ngtcp2 胶水（uvcpp_quic_ngtcp2.h / uvcpp_quic_session.h 是私有头）
│   ├── http3/     # HTTP/3 会话/连接层、nghttp3 胶水（uvcpp_h3_nghttp3.h / uvcpp_h3_session.h 是私有头）
│   └── ssl/       # SSL/TLS 上下文和连接封装
├── tests/
│   ├── unit/      # 单元测试
│   ├── functional/# 功能/集成测试
│   ├── tools/     # 测试工具（变异脚本等）
│   └── expand/    # 内存池测试
├── examples/      # 可运行示例（webapp_demo）
├── doc/           # 文档
│   ├── benchmark.md       # 每连接内存、吞吐与稳定性实测
│   ├── build-guide.md     # 各个 CMake 开关、构建树、平台依赖
│   ├── capi-guide.md      # C ABI（uvcpp_c_*）：ABI 契约、提供什么、不提供什么
│   ├── ci-guide.md        # CI 维护指南
│   ├── expand-guide.md    # 内存池 / 页堆 / span 的功能说明
│   ├── http2-guide.md     # HTTP/2 低层会话与连接层的用法
│   ├── http2-status.md    # HTTP/2 支持现状
│   ├── http3-guide.md     # web 层的 HTTP/3：它要的 QUIC 扩展、API 形状、CMake 接线、h1 不回归
│   ├── json-guide.md      # 手写 JSON 构造：转义契约与它的边界
│   ├── json-reflect-guide.md # JSON 反射：一个宏标出字段表，两个方向共用
│   ├── lowlevel-guide.md  # 事件循环、句柄与请求（地基）
│   ├── net-guide.md       # TCP/UDP 客户端与服务端
│   ├── quic-guide.md      # QUIC 传输：构建契约、API 形状、以及没做的那一列
│   ├── release-process.md # 发布怎么出，以及这条链**没有**检查什么
│   ├── soap-guide.md      # SOAP 信封、Fault 形状、从 binding 推派发键
│   ├── ssl-guide.md       # TLS 上下文与每连接封装
│   ├── testing-guide.md   # 测试分层、文件名即过滤键、tests/tools 索引
│   ├── web-http-guide.md  # web 层的 HTTP 半边
│   ├── web-ws-guide.md    # web 层的 WebSocket 半边
│   ├── webapp-guide.md    # web 应用框架指南
│   ├── webapp-support-guide.md # webapp 底下那些没被框架指南覆盖的类型
│   └── wsdl-guide.md      # WSDL 1.1 文档模型、QName 查询、发布与生成
├── cmake/         # CMake 配置模板
├── .github/workflows/  # 一个平台一个 workflow 文件，文件内按功能分格
├── CMakeLists.txt
├── CONTRIBUTING.md     # 从 clone 到跑通测试，以及本仓的开发约定
├── README.md
├── README.zh.md
└── RELEASE.md          # 发布说明 / 历史
```

---

## CI 与贡献

每次推送和 PR 都会通过 GitHub Actions 运行 CI。详见 [doc/ci-guide.md](doc/ci-guide.md)
了解 CI 维护规范 — 修改 CI 的贡献者务必先阅读。

欢迎贡献！请提交 issue 或 PR，保持修改小而专注，并遵循现有代码风格。

---

## 变更日志

变更日志现在在 **[CHANGELOG.md](./CHANGELOG.md)**：单一份文件，按模块分组，每条都标出它
**首次出现**的开发档号，末尾附逐版本清单。

它原先在这里，已经长到约 740 行。留在这里的是下载表、功能概览与 CMake 选项，其余一律
链出去。逐版本的发布叙事（这一版新增什么、换二进制会坏什么）在
[RELEASE.md](./RELEASE.md)。

---

## 许可证

MIT License — 详见 [LICENSE](./LICENSE)。

uvcpp — libuv 的 C++ 封装库
