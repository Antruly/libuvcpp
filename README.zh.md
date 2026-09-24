<p align="center">
  <img src="./uvcpp.svg" alt="libuvcpp logo" width="160" height="160">
</p>

[![版本](https://img.shields.io/badge/version-1.3.15--dev-blue.svg)](./RELEASE.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![CI](https://github.com/Antruly/libuvcpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci.yml)

# libuvcpp

🔧 基于 [libuv](https://github.com/libuv/libuv) 的现代 C++11 封装库 — 面向对象的异步 I/O，
支持双模式（异步回调/同步等待）、HTTP/1.1、WebSocket（RFC 6455）和 SSL/TLS。

- **版本**：`1.3.15-dev` — **作者**：`zhuweiye` — **许可证**：`MIT`
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
| expand | [doc/expand-guide.md](doc/expand-guide.md) | 内存池、页堆、span，以及它们默认关着的理由 |
| WSDL（文档 + 发布） | [doc/wsdl-guide.md](doc/wsdl-guide.md) | 把 WSDL 1.1 文档解析成模型、按 QName 查它、发出去或从模型生成一份 |
| SOAP（信封 + 派发） | [doc/soap-guide.md](doc/soap-guide.md) | 1.1 与 1.2 的信封与 `soap:Fault`、从 binding 推出来的派发键、九种拒绝各算谁的错，以及响应包装元素为什么不是派发键的对称 |

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
| `UVCPP_ENABLE_WSDL` | `OFF` | 启用 WSDL/SOAP 模块（XML 后端 pugixml，静态链入）。需要 `UVCPP_BUILD_WEBAPP=ON`。见 [`doc/wsdl-guide.md`](doc/wsdl-guide.md)（文档那一半）与 [`doc/soap-guide.md`](doc/soap-guide.md)（运行时那一半） |
| `UVCPP_USE_SYSTEM_LIBUV` | `ON` | 优先使用系统安装的 libuv |
| `UVCPP_BUILD_LIBUV_FROM_SOURCE` | `OFF` | 用 `FetchContent` 拉取并源码构建 libuv |
| `UVCPP_STATIC_RUNTIME` | `OFF` | 把编译器运行时（`libgcc`/`libstdc++`）静态链进库。**仅 MinGW 与 Linux 有效，MSVC 上是空操作** —— MSVC 用 `/MD`，发布包里自带 `vcruntime`/`msvcp` |
| `UVCPP_ENABLE_TRY_WRITE` | `ON` | 拷贝进写缓冲之前先试一次 `uv_try_write` |
| `UVCPP_TRY_WRITE_MIN_BYTES` | `32768` | 值得尝试 `uv_try_write` 的最小载荷（是 `CACHE STRING`，不是 `option()`） |

**注意**：开启 `UVCPP_BUILD_WEB=ON` 不会自动启用 `UVCPP_ENABLE_ZLIB` 或 `UVCPP_ENABLE_OPENSSL`。
这些选项需要显式手动开启。`UVCPP_ENABLE_NGHTTP2` 在 `UVCPP_ENABLE_OPENSSL=OFF` 时
**强制关闭**（给一条 warning，而不是留一个根本跑不起来的配置）—— 本库的 HTTP/2
没有明文形态。`UVCPP_ENABLE_WSDL` 同理，在 `UVCPP_BUILD_WEBAPP=OFF` 时**强制关闭**
（它建在 webapp 之上）。

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
│   ├── ci-guide.md        # CI 维护指南
│   ├── expand-guide.md    # 内存池 / 页堆 / span 的功能说明
│   ├── http2-guide.md     # HTTP/2 低层会话与连接层的用法
│   ├── http2-status.md    # HTTP/2 支持现状
│   ├── json-guide.md      # 手写 JSON 构造：转义契约与它的边界
│   ├── json-reflect-guide.md # JSON 反射：一个宏标出字段表，两个方向共用
│   ├── lowlevel-guide.md  # 事件循环、句柄与请求（地基）
│   ├── net-guide.md       # TCP/UDP 客户端与服务端
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
├── .github/workflows/  # CI 流水线
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

当前源码树是 **1.3.15-dev** —— 即 `UVCPP_VERSION_STRING`（`src/uvcpp/uvcpp_version.h`）
报告的那个串。本仓打过 `v1.0.0`、`v1.1.0`、`v1.2.0`、`v1.3.0` 四个 tag。下面是
`1.1.x` 与 `1.2.x` 这两条开发线从 `v1.1.0` 到 `v1.3.0` 之间落地的全部改动，按主题
分组，括号里是它**首次出现**的那一档；已发布版本的说明在
[RELEASE.md](./RELEASE.md)。其中若干条来自本仓第一位外部贡献者
[@sercebr](https://github.com/sercebr) 报的 issue。

### HTTP/2

实现现状 —— 强制了什么、默认值是什么、哪些是刻意不支持的 —— 见
[`doc/http2-status.md`](doc/http2-status.md)。

- 接入 [nghttp2](https://github.com/nghttp2/nghttp2)、ALPN 基础设施、h2 会话层与连接层
  （`1.1.1`），并接进请求层与 Web 应用框架（`1.1.2`）
- `uvcpp_http_client` 与 `uvcpp_http_server` 默认仍是 HTTP/1.1，要显式
  `set_http2_enabled()`；`uvcpp_web_app` 自己协商版本（`1.1.3`）
- 不再把 GOAWAY 当没发生；停机时先道别再拆连接（`1.1.6`、`1.1.7`）
- 发方向超出对端公布的上限时不再静默丢帧（`1.1.5`）
- 拆连接路上的两个 use-after-free 与一处泄漏（`1.1.7`），回调栈里不再重入
  `mem_send()`（`1.1.9`）
- 流级背压：收方向的 `pause_stream()` / `resume_stream()`、发方向的单流待发队列上界、
  以及 `peer_window_size()`（`1.2.25`）。这是**协议层机件，暂无应用层调用方** ——
  框架侧没有任何调用点，h2 上"边收边给"的流式请求体因此仍不可达

### 横向扩展（多事件循环）

- `uvcpp_tcp_server::set_loops(n)`：把接受的连接摊到 `n` 条事件循环上 —— 一条接受者
  加 `n-1` 条工作循环，每条一个**专用 `std::thread`**（不是 libuv 线程池的线程）。
  不调用它、或 `set_loops(1)`，与旧行为逐字节相同（`1.2.21`）
- socket 转手有自己的一套原语（`net/uvcpp_socket_handoff.h`）：Windows 走
  `WSADuplicateSocketW` + `WSASocketW`，其余走 `dup()`。**Windows 上这条腿压着一个
  已知的 libuv 缺陷** —— 开之前先读 [doc/net-guide.md](doc/net-guide.md) §4 与
  [RELEASE.md](./RELEASE.md)
- `set_handoff_forced(true)`（`1.3.15`）强制走「接受者 + 转手」，好让 POSIX 那条
  `dup()` 腿在 Linux 上也能被跑到（**测试口子**；生产别用）
- `set_loop_start_hook()`（`1.2.22`）与 `set_loop_exit_hook()`（`1.2.24`）在每条工作
  循环上跑，属主可以就地在上面建/拆句柄；启动钩子抛异常会翻成 `UV_ECANCELED`，
  而不是留下一个永远不兑现的承诺
- 连接 id 的高 20 位编码循环号，多循环下 id 仍唯一；`n == 1` 时这个编码是恒等变换，
  id 与旧版逐字节相同（`1.2.22`）
- `uvcpp_web_app::set_loops(n)`（`1.2.23`）把同一件事带到框架层，配套 `loop_count()`
  与 `connection_count_at()`。停机改成**逐槽位扇出** —— 此前那条路只投到 0 号循环，
  工作循环压根不进停机状态机、句柄没人删，`uv_loop_close` 于是撞 `UV_EBUSY`，
  整块循环内存泄漏
- `uvcpp_web_app::connections()` 现在返回**本循环**那一份登记表，`connection_count()`
  变成**所有循环求和**（`1.2.23`）—— 逐循环请用 `connection_count_at(int)`。同名同
  签名：旧代码**编得过**，只有在 `set_loops(n > 1)` 之后才会读到错的那一份

### HTTP 与 WebSocket 语义

- 错误路径不再编造状态码；连接中途断开时不再交付编出来的 `200`（`1.1.10`、`1.1.11`）
- `Accept-Encoding` 里显式写出的 `q=0` 不再被 `*` 覆盖（`1.1.17`）
- `HEAD` 与 `GET` 的头完全一致，压缩响应也一样（`1.1.18`、`1.1.21`）
- `206 Partial Content` 一律不压缩 —— `Content-Range` 与 `Content-Encoding` 自相矛盾（`1.1.26`）
- WebSocket：服务端强制客户端掩码、校验文本帧的 UTF-8，客户端也真的掩码了（`1.1.25`）
- 跨读边界的请求不再被吃掉（`1.1.32`）
- `req.path()` 折叠连续斜杠，与路由切段看齐（`1.1.19`）；请求头与 URL 长度在收的过程中
  就被卡住（`431` / `414`）（`1.1.20`）
- 静态文件服务在缓存命中时不再返回 `503`（`1.1.15`）
- `set_keep_alive(false)` 真的发 `connection: close` 了（调用方自己没设那个头时）；
  并且在「对端正关的连接」上收到响应时，不再报一笔永远兑现不了的写（`1.2.2`）
- `uvcpp_web_request::take_from()` 之后源请求的 `url` 与 `headers` 也空了（原先只承诺
  `body`）；契约写进了头文件，并配了前置断言（`1.2.7`）
- `uvcpp_web_router::match()` 命中路径上不再收集 `allow` / `allowed_methods` ——
  结果是 `MATCHED` 时这两个字段是空的（`1.2.12`）
- 框架序列化出去的每个响应都带 `Date`（HTTP/1.1 与 HTTP/2、整包与流式四条路都在内），
  且按秒格式化一次而不是每响应一次。它**没经手**的两条报文 —— 裸写的 `100 Continue`
  与 WebSocket 的 `101 Switching Protocols` 握手 —— 都不带 `Date`（那两条是字面量
  字节串直接写出去的，不走响应收口）。RFC 9110 §6.6.1 对 2xx/3xx/4xx 是**必发**、
  对 1xx/5xx 只是**可发**，所以这两条不发也合规 —— 那是"没经手"，不是"写了排除"。
  `UVCPP_SERVER_TOKEN` / `uvcpp::server_token()` 给出 `Server` 的默认值（仍然有意
  不带版本号），`uvcpp::version_string()` 则终于把 `UVCPP_VERSION_STRING` 接出来了
  —— 那个宏此前全仓没有一个读者（`1.3.1`）

### TLS 与网络

- 加载完整证书链，TLS 版本上下限双向生效（`1.1.13`）
- TLS 握手有超时，且不在超时那条路上留下孤儿定时器（`1.1.14`）
- 在自己的回调里析构 `tcp_client` 不再按连接数累积包装对象（`1.1.16`）
- 对端断开时会触发 HTTP 客户端的关闭观察者，而不是让回调永远不来（`1.1.4`）
- `tls_verify_mode::PEER_STRICT` 在客户端侧真的校验主机名：`connect()` 收到的那个名字
  被钉给证书（数字 IP 字面量走 `X509_VERIFY_PARAM_set1_ip_asc()`，其余走
  `set1_host()`），名字对不上的对端**建立不起来**（`1.2.24`）。它此前与 `PEER`
  **完全等价** —— 服务端侧至今仍然等价：本库的服务端不发 SNI、也不要求客户端证书，
  没有可校验的名字
- 绕开 `tcp_client` / `http_client` 直接用 `uvcpp_ssl` 的调用方**拿不到**这层校验，
  得自己调 `uvcpp_ssl::set_verify_hostname()`

### 内存与缓冲

- 大块分配重新计入 `span->in_use` —— 在那之前每次分配都整段泄漏（`1.1.12`）
- `uvcpp_write` 第 2 块槽位换占用者时不再漏一块（`1.1.29`）
- 内存池的「在用块数」重新跟着分配走（`1.1.33`）
- 压缩变体表的字节账改成从表里算出来的派生量（不再存一个会漂移的计数器），
  字节上限那条腿也终于有了判据（`1.1.34`）
- `memory_pool_config::max_total_memory` 真的限额了：池按**实占**字节记账（含每块的
  块头），超额拒绝分配并计入 `failed_allocations`（`1.2.3`）。默认值 `0` 表示不限额，
  行为零变化
- 池的 SUPER 档（>256 KiB）不再静默泄漏 —— 它那次进线程本地缓存的 push 谎报成功、
  把块丢了，内存回不到全局池、额度也不退，而统计照常记一次「已释放」（`1.2.9`）
- 三条释放路径在 `free` 之后读块头（use-after-free），以及 `static_release_callback`
  的拆除分支拿 `::operator delete` 去还 `_aligned_malloc` 来的块
  （`STATUS_HEAP_CORRUPTION`）—— 两处都已修（`1.2.9`）
- 池可以接全局 `operator new` 了：它的元数据此前自己走 `new`，重入撞上函数局部静态的
  初始化守卫，会**卡死**（`1.2.13`）

### 公开契约与安全

- `uvcpp_handle` 的拷贝构造、拷贝赋值与 `clone()` **从公开接口删除** —— 它们的实现是
  `memcpy` 一个活着的 `uv_handle_t`（连着 loop 指针与邻居指针），结果是双重释放，或把
  活句柄从自己的循环队列里摘掉（`1.2.5`）。`uvcpp_req` 的拷贝操作**保留**（`uv_req_t`
  没有侵入式队列、也没有 loop 指针），只删它的 `clone()`（`1.2.5`）
- `DEFINE_FUNC_REQ_CPP` / `DEFINE_COPY_FUNC_REQ_CPP` 编得过了 —— 这两个公开宏从来没被
  编译过，里面压着三处硬错误；现在它们各自有用例盖住（`1.2.2`）
- `uvcpp_ws_connection` 的默认构造从此**有定义**：`uvcpp_ws_connection c;` 此前编得过、
  **链接不过**（`1.2.2`）

### 性能

- `write()` 先试 `uv_try_write()`，吃得下的部分不再拷进待发缓冲（`1.1.22`）
- `uvcpp_stream::try_write()` 不再白拷一份 `uv_buf_t` 数组（`1.1.23`）
- 静态响应带上压缩变体缓存（`1.1.24`）
- 响应体零拷贝接管，并与头一起作为两块写出去（`nbufs = 2`）（`1.1.28`）；
  变体表按句柄存/取，不再整份拷体（`1.1.31`）
- 响应序列化不再走 `std::ostringstream`：合计 −2.52%、用户态 −10.19%、
  91 330 → 93 688 RPS，九个端点报文逐字节不变（`1.2.4`）
- 读缓冲不再清零 —— libuv 对 TCP 流给 64 KiB、一次请求跑两趟，所以每请求白清
  128 KiB：合计 −13.66%、用户态 −22.88%、QPS +12.02%（`1.2.4`）
- 响应头表改成**按需**预留，`uvcpp_web_context` 的对象与控制块并成一次分配 ——
  每请求 16.00 → 13.00 次分配（`1.2.20`）
- 头名查找多了一组**不拥有**的 `const char*` 重载（14 个类成员 + 4 个自由函数），
  超过 SSO 上限的字面量不再构造临时 `std::string`（`1.2.16`）；值位置
  `text()` / `html()` / `json()` / `json_str()` 同理（`1.2.17`）
- `uvcpp_http_parser::take_headers()` 改成搬元素而不是搬 vector，解析器的头表
  high-water 因此按连接钉住 —— 每请求 19.00 → 18.00 次分配（`1.2.18`）
- `uvcpp_http_request` 重新有了真正的移动操作：用户声明的拷贝赋值会抑制隐式移动赋值，
  于是 claim 路径上那句 `req = std::move(...)` 在静默地深拷整张头表（`1.2.8`）
- `uvcpp_buf` 那 14 个「先 resize 再 memcpy」的入口不再清零马上要被盖掉的那段 ——
  100 B GET 从 3.4 次 `memset` / 约 200 B 变成 0，1 MiB POST 从 38 次 / 3.00 MiB 变成 0
  （`1.2.15`）
- 写队列把能带走的整批拼成**一次**写，不再每完成一块发一块 —— 段/请求
  3.43 → 2.06（−40%）、同装置服务端每请求 CPU 约 −30%、RPS 38 839 → 57 067（`1.2.14`）
- 空闲连接上的响应不再绕写队列（`1.2.10`）；普通响应不再为完成回调付两次堆分配
  （`1.2.6`）；路由命中路径不再算 `allow`（`1.2.12`）

**实测数据（不是估算）** —— 见 [doc/benchmark.md](doc/benchmark.md)：每条空闲连接
**4.62 KiB**（4 734 B，八档最小二乘，R² = 0.999987，外推 100 万连接 ≈ 4.42 GiB）、单事件循环
75 k RPS、10 分钟长跑 3 840 万请求 0 错误。那一页还把每连接这个数与
[Hical](https://github.com/Hical61/Hical) 自己的报告做了对照，并写明了该对照带的口径问题。
上面 `1.2.x` 那几笔读数来自**别的装置、别的轮次**，不能与这些相加。本仓另有一套压测靶场，
在 `bench/` 下、由 `UVCPP_BUILD_BENCH` 开关控制（默认 OFF，不进 CI），说明见
[doc/benchmark-rig.md](doc/benchmark-rig.md)。

### 配置、打包与 CI

- 使能宏随包发出；宏集与 DLL 不一致从静默垃圾值改为编译期硬失败（`1.1.27`）
- 22 个公开头带上 UTF-8 BOM，消费者不传 `/utf-8` 时不再级联报错（`1.1.30`）
- Linux 包里的 `libuvcpp.so` 从 `bin/` 挪到 `lib/`，也就是文档里指的那个位置（`1.1.28`）
- 六个打包 job 补上 `UVCPP_ENABLE_NGHTTP2`，h2 不再从包里缺席（`1.1.6`）
- 每份包里多了一档**调试版**动态库（`uvcppd.dll` / `libuvcppd.so`，`-luvcppd`，
  MSVC 那份还带 `uvcppd.pdb`），方便单步进库内部（`1.1.35`）
- 新增十二篇指南：使用者向八篇（`lowlevel-guide.md`、`net-guide.md`、`ssl-guide.md`、
  `web-http-guide.md`、`web-ws-guide.md`、`http2-guide.md`、`expand-guide.md`、
  `webapp-support-guide.md`）与贡献者向四篇（`CONTRIBUTING.md`、`build-guide.md`、
  `testing-guide.md`、`release-process.md`）（`1.2.1`）
- 三道文档门禁进 CI：构建系统的选项与 README 选项表双向闭合、链接/路径/锚点可解析、
  每篇指南都被正文提到（`check_docs.py`）；22 篇文档里的 `文件:行号` 引用能解析且与
  内容哈希锁一致（`check_doc_lines.py`）；每个 `cpp` 片段都对着一个已 stage 的包、
  不加任何 `-D` 真编译（`check_doc_snippets.py`）（`1.2.1`）
- 两版 README 里 4 处**编不过**的示例已修（`1.2.1`）
- 给消费者的 ABI 提示：`uvcpp_buf`（`1.1.28`）与 `uvcpp_http_server`（`1.1.34`）的布局
  变过，`1.2.x` 这条线改得更多 —— `uvcpp_h2_stream`（`1.2.25`）、`uvcpp_ssl_context`
  （`1.2.24`）、`uvcpp_memory_pool`（`1.2.x`）与五个多循环类（`1.2.21`–`1.2.23`）布局
  都变了；`uvcpp_handle` 的拷贝构造、拷贝赋值与 `clone()`，以及 `uvcpp_req::clone()`
  则被**删除**（`1.2.5`）—— **必须重编，别只换二进制**（见 [RELEASE.md](./RELEASE.md)）

---

## 许可证

MIT License — 详见 [LICENSE](./LICENSE)。

uvcpp — libuv 的 C++ 封装库
