[![version](https://img.shields.io/badge/version-1.1.34--dev-blue.svg)](./RELEASE.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![CI](https://github.com/Antruly/libuvcpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci.yml)

# libuvcpp

🔧 Modern C++11 wrapper for [libuv](https://github.com/libuv/libuv) — event-driven I/O with
object-oriented APIs, dual-mode async/sync support, HTTP/1.1, WebSocket (RFC 6455), and SSL/TLS.

- **Version**: `1.1.34-dev` — **Author**: `zhuweiye` — **License**: `MIT`
- **Languages**: [English](./README.md) · [中文](./README.zh.md)

---

## Overview

libuvcpp provides a thin, idiomatic C++ layer over libuv's event loop, handles, and requests.
It preserves libuv's performance while adding RAII resource management, `std::function` callbacks,
and higher-level client/server abstractions for TCP, UDP, HTTP, and WebSocket.

The library is organized into layered modules:

```
application
┌────────────┐
│ web (HTTP/WS) │  ← uvcpp_http_client/server, uvcpp_ws_client/server
├────────────┤
│ ssl (TLS)    │  ← uvcpp_ssl, uvcpp_ssl_context (OpenSSL wrapper)
├────────────┤
│ net          │  ← uvcpp_tcp_client/server, uvcpp_udp_client/server
├────────────┤
│ handle + req │  ← uvcpp_loop, uvcpp_tcp, uvcpp_timer, uvcpp_write, ...
├────────────┤
│ uvcpp core   │  ← uvcpp_buf, uvcpp_thread, uvcpp_alloc, ...
└────────────┘
```

---

## Features

### Core (handle + req)

| Category | Classes |
|----------|---------|
| **Event loop** | `uvcpp_loop` |
| **Stream handles** | `uvcpp_tcp`, `uvcpp_pipe`, `uvcpp_udp`, `uvcpp_tty` |
| **Timers & hooks** | `uvcpp_timer`, `uvcpp_idle`, `uvcpp_prepare`, `uvcpp_check` |
| **Signals & process** | `uvcpp_signal`, `uvcpp_process` |
| **File system** | `uvcpp_fs`, `uvcpp_fs_event`, `uvcpp_fs_poll` |
| **Polling** | `uvcpp_poll`, `uvcpp_async` |
| **Requests** | `uvcpp_write`, `uvcpp_connect`, `uvcpp_shutdown`, `uvcpp_work`, `uvcpp_getaddrinfo`, `uvcpp_getnameinfo`, `uvcpp_udp_send`, `uvcpp_random` |

### Utilities (`src/uvcpp/`)

Thread pool, RW locks, barriers, CPU info, network interfaces, passwd/group, directory traversal,
environment variables, buffer management (`uvcpp_buf`), metrics, and allocator integration.

### Expand module (`src/expand/`)

TCMalloc-style memory pool: page heap, span allocator, thread cache, enterprise allocator.
**Off by default** (`UVCPP_BUILD_EXPAND=OFF`) since v1.1.0 — pass
`-DUVCPP_BUILD_EXPAND=ON` to enable it. The prebuilt binaries ship **with** the pool;
consumers define nothing — the package's `uvcpp/uvcpp_config.h` carries the value this
build actually used, and defining a conflicting one yourself is a hard compile error
instead of a silent allocator mismatch (see `RELEASE.md`).

### Net module (`src/net/`) — `UVCPP_BUILD_NET=ON` (default)

| Class | Description |
|-------|-------------|
| `uvcpp_tcp_client` | High-level TCP client with dual-mode API (async callback / sync `wait()` with timeout) |
| `uvcpp_tcp_server` | TCP server with `bind()`/`listen()`/`on_connection()` |
| `uvcpp_udp_client` | UDP client with dual-mode send/recv |
| `uvcpp_udp_server` | UDP server with `bind()`/`recv_start()` |

### Web module (`src/web/`) — `UVCPP_BUILD_WEB=ON`

| Class | Description |
|-------|-------------|
| `uvcpp_http_client` | HTTP client with keep-alive, streaming parse, dual-mode `send()`/`send_wait()`. HTTP/1.1 by default; opt into HTTP/2 with `set_http2_enabled()` |
| `uvcpp_http_server` | HTTP server with route registration, per-connection parser, upgrade detection. HTTP/1.1 by default; opt into HTTP/2 with `set_http2_enabled()` |
| `uvcpp_http_parser` | Streaming HTTP parser (wraps [llhttp](https://github.com/nodejs/llhttp)), PIMPL pattern |
| `uvcpp_http_request` | Request object with `to_string()` / `from_parser()` serialization |
| `uvcpp_http_response` | Response object with factory methods (`ok()`, `not_found()`, etc.) |
| `uvcpp_ws_client` | WebSocket client (RFC 6455), parses `ws://`/`wss://` URLs, upgrade handshake |
| `uvcpp_ws_server` | WebSocket server, auto-upgrade from HTTP via `on_upgrade()` |
| `uvcpp_ws_connection` | WebSocket connection: `send_text()`/`send_binary()`/`send_ping()`/`send_close()` |
| `uvcpp_ws_parser` | Streaming WebSocket frame parser (RFC 6455), 8-state state machine |
| `uvcpp_ws_frame` | Frame struct with opcode, mask, close-code helpers |
| `uvcpp_http_common` | HTTP method/status enums, version enum (HVER_10/11/20), header helpers |

**Optional features** (opt-in, not auto-enabled):

- `UVCPP_ENABLE_ZLIB=ON` — Per-Message Deflate compression (RFC 7692) for WebSocket
- `UVCPP_ENABLE_OPENSSL=ON` — HTTPS (WSS) via SSL/TLS module
- `UVCPP_ENABLE_NGHTTP2=ON` — HTTP/2 (RFC 9113) via [nghttp2](https://github.com/nghttp2/nghttp2).
  Requires `UVCPP_ENABLE_OPENSSL=ON` (force-disabled without it) — h2 is **TLS + ALPN
  only**: no h2c, no prior knowledge, no RFC 8441, no `:protocol`. It is **off by
  default and nothing upgrades automatically**: `uvcpp_http_server` needs
  `set_http2_enabled(true)` **and** an explicit `set_alpn_select_protos({"h2","http/1.1"})`
  on the SSL context, `uvcpp_http_client` needs `set_http2_enabled(true)` (it builds the
  per-connection ALPN list itself), and `uvcpp_web_app` wires all of it for you. See
  [§13 of the webapp guide](doc/webapp-guide.md#13-http2).

### Web app framework (`src/webapp/`) — `UVCPP_BUILD_WEBAPP=ON`

A higher-level application layer built on the web module: **you write handlers, not
wire formats.** Requires `UVCPP_BUILD_WEB=ON` (webapp is force-disabled when web is off,
rather than leaving a configuration that does not compile). Pulls in
[nlohmann/json](https://github.com/nlohmann/json) via FetchContent for its JSON backend.

| Class | Description |
|-------|-------------|
| `uvcpp_web_app` | The application: config, routing, middleware, lifecycle (`start`/`stop`/`join`) |
| `uvcpp_web_router` | Pattern router — `/user/:id` params, `/files/*path` wildcards, static > param > wildcard |
| `uvcpp_web_request` / `uvcpp_web_response` | Request/response wrappers: query, form, cookies, path params, HTTP status helpers, chunked, `send_file` |
| `uvcpp_web_context` | Per-request context: middleware chain, `post()`, `hold()`/`release()`, user data |
| `uvcpp_web_static` | Static file service: ETag, Last-Modified, Range/206/416, LRU cache, SPA fallback, dotfile policy |
| `uvcpp_web_upload` / `uvcpp_web_multipart` | Streaming multipart upload to disk with random leaf names and six size limits |
| `uvcpp_web_stream` | Streaming request bodies (`on_data`/`on_end`/`pause`/`resume`) |
| `uvcpp_web_ws` | WebSocket endpoints on the app (`app.websocket("/chat/:room", handler)`) |
| `uvcpp_web_ws_client` | WebSocket client with callbacks on the client itself and optional **auto-reconnect** |
| `uvcpp_web_work_limit` | Work-pool admission gate (backpressure for `uv_queue_work`) |
| `uvcpp_log` / `uvcpp_log_console` | Two-axis logging: level + category, pluggable sink |
| `uvcpp_web_json` | JSON helpers around nlohmann/json — no exceptions across libuv callbacks, depth pre-scan |

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

  app.start();   // binds, then runs the loop on a background thread
  app.join();
  return 0;
}
```

**→ Full guide: [doc/webapp-guide.md](doc/webapp-guide.md)** — routing rules, middleware,
static/upload/streaming, WebSocket client & reconnect, security, and known limitations.
Runnable example: `examples/webapp_demo.cpp`.

### SSL module (`src/ssl/`) — `UVCPP_ENABLE_OPENSSL=ON`

| Class | Description |
|-------|-------------|
| `uvcpp_ssl_context` | SSL/TLS context (wraps `SSL_CTX*`), cert/key loading, self-signed cert generation |
| `uvcpp_ssl` | Per-connection SSL wrapper, `handshake()`/`read()`/`write()`/`shutdown()` |
| `uvcpp_ssl_common` | `tls_version`, `tls_mode`, `tls_verify_mode`, `tls_cert_info` enums/structs |

---

## Requirements

- **C++11** or later
- **CMake** ≥ 3.20
- **Compiler**: MSVC 2019+, GCC 7+, Clang 5+
- **libuv**: auto-fetched via FetchContent if not found on system
- **Platforms**: Windows, Linux, macOS

---

## Build

### Basic build (core only)

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON
cmake --build . --config Release --parallel
ctest --output-on-failure -C Release
```

### With Web module (HTTP + WebSocket)

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON \
  -DUVCPP_BUILD_WEB=ON
cmake --build . --config Release --parallel
```

### With Web + SSL + compression (full)

```bash
# Linux: install system deps first
sudo apt-get install libssl-dev zlib1g-dev

cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON \
  -DUVCPP_BUILD_WEB=ON \
  -DUVCPP_ENABLE_OPENSSL=ON \
  -DUVCPP_ENABLE_ZLIB=ON
cmake --build . --config Release --parallel
```

### With the web app framework

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON \
  -DUVCPP_BUILD_WEB=ON \
  -DUVCPP_BUILD_WEBAPP=ON \
  -DUVCPP_BUILD_EXAMPLES=ON
cmake --build . --config Release --parallel
# runnable example (self-checking):
./examples/Release/webapp_demo
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `UVCPP_BUILD_TESTS` | `ON` | Build test executables |
| `BUILD_SHARED_LIBS` | `ON` | Build shared libraries |
| `UVCPP_BUILD_STATIC` | auto | Build static uvcpp library |
| `UVCPP_BUILD_SHARED` | auto | Build shared uvcpp library |
| `UVCPP_BUILD_EXPAND` | `OFF` | Build expand module (memory pool) |
| `UVCPP_BUILD_NET` | `ON` | Build net module (TCP/UDP client/server) |
| `UVCPP_BUILD_WEB` | `OFF` | Build web module (HTTP + WebSocket) |
| `UVCPP_BUILD_WEBAPP` | `OFF` | Build web app framework (router/middleware/static/upload/log). Requires `UVCPP_BUILD_WEB=ON` |
| `UVCPP_BUILD_EXAMPLES` | `OFF` | Build the examples in `examples/` |
| `UVCPP_ENABLE_ZLIB` | `OFF` | Enable zlib (WebSocket compression) |
| `UVCPP_ENABLE_OPENSSL` | `OFF` | Enable OpenSSL (HTTPS/WSS) |
| `UVCPP_ENABLE_NGHTTP2` | `OFF` | Enable HTTP/2 (nghttp2, linked static). Requires `UVCPP_ENABLE_OPENSSL=ON` and `UVCPP_BUILD_WEB=ON` |
| `UVCPP_USE_SYSTEM_LIBUV` | `ON` | Prefer system-installed libuv |

**Important**: `UVCPP_ENABLE_ZLIB` and `UVCPP_ENABLE_OPENSSL` are NOT auto-enabled
when `UVCPP_BUILD_WEB=ON`. You must opt in explicitly. `UVCPP_ENABLE_NGHTTP2` is
**force-disabled** when `UVCPP_ENABLE_OPENSSL=OFF` (it warns rather than leaving a
configuration that cannot work) — HTTP/2 here has no cleartext mode.

---

## Quick Start

### TCP Echo Server

```cpp
#include "uvcpp.h"
#include "net/uvcpp_tcp_server.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_tcp_server server;
    server.bind("127.0.0.1", 8080);

    server.on_connection([](uvcpp::uvcpp_tcp_client* client) {
        client->on_data([](uvcpp::uvcpp_tcp_client* c, const char* data, size_t len) {
            std::cout << "Received: " << std::string(data, len) << std::endl;
            c->write(data, len);  // echo back
        });
        client->on_close([](uvcpp::uvcpp_tcp_client* c) {
            std::cout << "Client disconnected" << std::endl;
        });
    });

    server.listen();
    std::cout << "Echo server on :8080" << std::endl;
    server.run();
    return 0;
}
```

### HTTP GET Request

```cpp
#include "web/uvcpp_http_client.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_http_client client;

    client.get("http://httpbin.org/get",
               [](const uvcpp::uvcpp_http_response& rsp, int err) {
        if (err) return;
        std::cout << "Status: " << static_cast<int>(rsp.status_code) << std::endl;
        std::cout << "Body: "
                  << std::string(rsp.body.get_const_data(), rsp.body.size())
                  << std::endl;
    });

    client.run();
    return 0;
}
```

### WebSocket Client

```cpp
#include "web/uvcpp_ws_client.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_ws_client client;

    client.connect("ws://echo.websocket.org/chat",
        [](uvcpp::uvcpp_ws_connection* conn, int err) {
            if (err) return;

            conn->on_text([](const std::string& msg) {
                std::cout << "Echo reply: " << msg << std::endl;
            });

            const std::string msg = "Hello WebSocket!";
            conn->send_text(msg.c_str(), msg.size());
        });

    client.run();
    return 0;
}
```

### WebSocket Server

```cpp
#include "web/uvcpp_ws_server.h"
#include <iostream>

int main() {
    uvcpp::uvcpp_ws_server server;
    server.bind("127.0.0.1", 8080);

    server.on_connection([](uvcpp::uvcpp_ws_connection* conn) {
        std::cout << "WS client connected" << std::endl;

        conn->on_text([conn](const std::string& msg) {
            std::cout << "Received: " << msg << std::endl;
            const std::string reply = "Echo: " + msg;
            conn->send_text(reply.c_str(), reply.size());
        });

        conn->on_close([](uvcpp::uvcpp_ws_connection*) {
            std::cout << "WS client disconnected" << std::endl;
        });
    });

    server.listen();
    server.run();
    return 0;
}
```

### SSL/TLS Server

```cpp
#include "ssl/uvcpp_ssl_context.h"
#include "net/uvcpp_tcp_server.h"

int main() {
    // Create SSL context
    uvcpp::uvcpp_ssl_context ssl_ctx(uvcpp::tls_mode::SERVER);
    ssl_ctx.generate_self_signed("localhost");  // or load_certificate_file()

    uvcpp::uvcpp_tcp_server server;
    server.set_ssl_context(&ssl_ctx);
    server.bind("127.0.0.1", 8443);
    // ... on_connection, listen, run
}
```

---

## Testing

```bash
# Build with tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON
cmake --build build --config Release --parallel

# Run all tests
ctest --test-dir build --output-on-failure -C Release

# Run only web module tests
ctest --test-dir build -C Release -R "web_"

# Run with exclusions (test_shutdown_func fails ~1% of runs — flaky, not hanging; doc/ci-guide.md §4)
ctest --test-dir build -C Release --exclude-regex "test_shutdown_func"
```

Test coverage:
- **Unit tests**: `tests/unit/` — handle types, request types, uvcpp utilities
- **Functional tests**: `tests/functional/` — runtime behavior for all modules
- **Expand tests**: `tests/expand/` — memory pool allocation tests

---

## Project Structure

```
libuvcpp/
├── src/
│   ├── uvcpp/     # Core utilities (buf, thread, version, alloc, ...)
│   ├── handle/    # libuv handle wrappers (loop, tcp, udp, timer, ...)
│   ├── req/       # libuv request wrappers (write, connect, fs, work, ...)
│   ├── expand/    # Memory pool (page heap, span, enterprise allocator)
│   ├── net/       # TCP/UDP client/server
│   ├── web/       # HTTP client/server, WebSocket client/server, frame parser
│   ├── webapp/    # Web app framework (router, middleware, static, upload, WS client, log)
│   └── ssl/       # SSL/TLS context and connection wrapper
├── tests/
│   ├── unit/      # Unit tests
│   ├── functional/# Functional/integration tests
│   ├── tools/     # Test tooling (e.g. mutation harnesses)
│   └── expand/    # Memory pool tests
├── examples/      # Runnable examples (webapp_demo)
├── doc/           # Documentation
│   ├── ci-guide.md        # CI maintenance guidelines
│   └── webapp-guide.md    # Web app framework guide
├── cmake/         # CMake config templates
├── .github/workflows/  # CI pipeline
├── CMakeLists.txt
├── README.md
└── README.zh.md
```

---

## CI & Contributing

CI runs on every push and PR via GitHub Actions. See [doc/ci-guide.md](doc/ci-guide.md) for
the CI maintenance guidelines — contributors modifying the CI must read it first.

Contributions are welcome. Please open an issue or PR, keep changes focused, and follow
the existing code style.

---

## Changelog

The current source tree is **1.1.34-dev** — that is what `UVCPP_VERSION_STRING`
(`src/uvcpp/uvcpp_version.h`) reports. Only `v1.0.0` and `v1.1.0` were ever tagged; every
`1.1.x` since is a development version (`UVCPP_VERSION_IS_RELEASE = 0`) that has not been
released. Release notes for the tagged versions are in [RELEASE.md](./RELEASE.md); below is
everything that landed in the `1.1.x` line, by theme, with the version each change first
appeared in. Several of the fixes came from issue reports by the project's first external
contributor, [@sercebr](https://github.com/sercebr).

### HTTP/2

- [nghttp2](https://github.com/nghttp2/nghttp2) integration, ALPN plumbing, and the h2
  session/connection layers (`1.1.1`), wired into the request layer and the web app
  framework (`1.1.2`)
- `uvcpp_http_client` and `uvcpp_http_server` stay on HTTP/1.1 unless you call
  `set_http2_enabled()`; `uvcpp_web_app` negotiates the version itself (`1.1.3`)
- GOAWAY is no longer treated as a no-op, and shutdown says goodbye before tearing a
  connection down (`1.1.6`, `1.1.7`)
- Sending past the peer's advertised limit no longer drops frames silently (`1.1.5`)
- Teardown fixes: two use-after-frees and a leak (`1.1.7`), and no re-entrant `mem_send()`
  from inside a callback (`1.1.9`)

### HTTP & WebSocket semantics

- Error paths no longer invent a status code, and a connection that drops mid-stream no
  longer delivers a fabricated `200` (`1.1.10`, `1.1.11`)
- An explicit `q=0` in `Accept-Encoding` is no longer overridden by `*` (`1.1.17`)
- `HEAD` and `GET` produce identical headers, compressed responses included (`1.1.18`, `1.1.21`)
- `206 Partial Content` responses are never compressed — `Content-Range` and
  `Content-Encoding` contradict each other (`1.1.26`)
- WebSocket: the server enforces client masking and validates UTF-8 in text frames, and the
  client actually masks (`1.1.25`)
- A request straddling a read boundary is no longer swallowed (`1.1.32`)
- `req.path()` folds repeated slashes, matching how routes are split (`1.1.19`); request
  header and URL lengths are capped while receiving (`431` / `414`) (`1.1.20`)
- Static file serving no longer returns `503` on a cache hit (`1.1.15`)

### TLS & networking

- The full certificate chain is loaded, and the TLS version floor and ceiling both apply (`1.1.13`)
- TLS handshakes have a timeout, with no orphaned timer left behind (`1.1.14`)
- Destroying a `tcp_client` from inside its own callback no longer accumulates wrappers (`1.1.16`)
- A dropped peer fires the HTTP client's close observer instead of leaving the callback
  silent forever (`1.1.4`)

### Memory & buffers

- Large allocations count towards `span->in_use` again — they leaked a whole span each time (`1.1.12`)
- The second write slot of `uvcpp_write` no longer leaks when its occupant is replaced (`1.1.29`)
- The memory pool's in-use block count follows allocation again (`1.1.33`)
- The compression variant table's byte account is derived from the table rather than kept in
  a counter that could drift, and the byte cap finally has a test (`1.1.34`)

### Performance

- `write()` tries `uv_try_write()` first, so what fits is not copied into the pending buffer (`1.1.22`)
- `uvcpp_stream::try_write()` no longer copies the `uv_buf_t` array (`1.1.23`)
- Static responses get a compressed-variant cache (`1.1.24`)
- Response bodies are taken over without a copy and go out with the headers as two write
  blocks (`nbufs = 2`) (`1.1.28`); the variant table stores and returns handles instead of
  whole bodies (`1.1.31`)

### Configuration, packaging & CI

- Enable macros ship with the package, and a macro set that disagrees with the DLL is now a
  compile-time failure instead of a silently wrong value (`1.1.27`)
- 22 public headers carry a UTF-8 BOM, so consumers that do not pass `/utf-8` no longer fail
  in a cascade (`1.1.30`)
- The Linux package's `libuvcpp.so` moved from `bin/` to `lib/`, where the docs point (`1.1.28`)
- Six packaging jobs pass `UVCPP_ENABLE_NGHTTP2`, so h2 packages no longer lack it (`1.1.6`)
- ABI note for consumers: the layout of `uvcpp_buf` (`1.1.28`) and `uvcpp_http_server`
  (`1.1.34`) changed — **rebuild, do not just swap the binary** (see [RELEASE.md](./RELEASE.md))

---

## License

MIT License — see [LICENSE](./LICENSE).

uvcpp — C++ wrapper for libuv
