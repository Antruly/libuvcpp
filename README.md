<p align="center">
  <img src="./uvcpp.svg" alt="libuvcpp logo" width="160" height="160">
</p>

[![version](https://img.shields.io/badge/version-1.4.1--dev-blue.svg)](./RELEASE.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![Linux (Ubuntu)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml)
[![Windows (MSVC)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml)
[![Windows (MinGW64)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml)
[![macOS](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml)

# libuvcpp

🔧 Modern C++11 wrapper for [libuv](https://github.com/libuv/libuv) — event-driven I/O with
object-oriented APIs, dual-mode async/sync support, HTTP/1.1, WebSocket (RFC 6455), and SSL/TLS.

- **Version**: `1.4.1-dev` — **Author**: `zhuweiye` — **License**: `MIT`
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
│ http2        │  ← h2 session/connection layers, nghttp2 glue (`UVCPP_ENABLE_NGHTTP2=ON`)
├────────────┤
│ ssl (TLS)    │  ← uvcpp_ssl, uvcpp_ssl_context (OpenSSL wrapper)
├────────────┤
│ quic + http3 │  ← uvcpp_quic_client/server/connection, ngtcp2 glue (net layer, `UVCPP_ENABLE_QUIC=ON`); h3 rides on it, nghttp3 glue (web layer, `UVCPP_ENABLE_HTTP3=ON`)
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
environment variables, buffer management (`uvcpp_buf`), metrics, allocator integration,
and JSON construction (`uvcpp_json_writer`).

### Expand module (`src/expand/`)

TCMalloc-style memory pool: page heap, span allocator, thread cache, enterprise allocator.
**Off by default** (`UVCPP_BUILD_EXPAND=OFF`) since v1.1.0 — pass
`-DUVCPP_BUILD_EXPAND=ON` to enable it. The prebuilt binaries ship **with** the pool;
consumers define nothing — the package's `uvcpp/uvcpp_config.h` carries the value this
build actually used, and defining a conflicting one yourself is a hard compile error
instead of a silent allocator mismatch (see `RELEASE.md`).

Each prebuilt package also carries a **Debug** build of the library (`uvcppd.dll` /
`libuvcppd.so`, link with `-luvcppd`, plus `uvcppd.pdb` on MSVC) for stepping into the
library. Its prerequisites — chiefly that MSVC's non-redistributable Debug CRT is *not*
in the package — are in [`RELEASE.md`](./RELEASE.md#调试档debug-版).

### Net module (`src/net/`) — `UVCPP_BUILD_NET=ON` (default)

| Class | Description |
|-------|-------------|
| `uvcpp_tcp_client` | High-level TCP client with dual-mode API (async callback / sync `wait()` with timeout) |
| `uvcpp_tcp_server` | TCP server, `bind()` + `listen(connection_cb, backlog)` (the connection callback is `listen()`'s first argument; there is no separate `on_connection()`) |
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
- `UVCPP_ENABLE_QUIC=ON` — QUIC transport (RFC 9000) over [ngtcp2](https://github.com/ngtcp2/ngtcp2),
  linked static, in the **net layer**. Requires `UVCPP_ENABLE_OPENSSL=ON` **and** an
  **OpenSSL ≥ 3.2 with the QUIC API**, plus `UVCPP_BUILD_NET=ON` (force-disabled without
  any of them — there is no cleartext QUIC). As of **1.4.1** it is a real link protocol:
  handshake, streams, connection close and idle timeout all work. HTTP/3 rides on top of
  it — see the next bullet. See [doc/quic-guide.md](doc/quic-guide.md).
- `UVCPP_ENABLE_HTTP3=ON` — HTTP/3 (RFC 9114) in the **web layer**, parsed by
  [nghttp3](https://github.com/ngtcp2/nghttp3) (note the org: `ngtcp2`, not `nghttp2`)
  and carried over the QUIC transport, both linked static. Requires
  `UVCPP_ENABLE_QUIC=ON` and `UVCPP_BUILD_WEB=ON` (force-disabled without them). As of
  **1.4.1** `uvcpp_http_client` / `uvcpp_http_server` speak h3 transparently —
  `set_http3_enabled(true)` on the client, `listen_quic()` on the server — and the
  routing table, handlers and response type are the same ones h1/h2 use. HTTP/3 runs on
  **UDP on its own sockets**, so **HTTP/1.1 and HTTP/2 are untouched** (the compile
  commands and symbol sizes of the h1 path are byte-identical with the switch off). See
  [doc/http3-guide.md](doc/http3-guide.md).

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

### Module guides

The tables above list **what exists**; the pages below explain **how to use it** —
typical flows, error handling, and the places where the header comments disagree with
the implementation. All of them live in `doc/`, and **every code example in them is
compiled by CI** (`tests/tools/check_doc_snippets.py`), so they are safe to copy.

| Module | Guide | What it covers |
|---|---|---|
| Low level (handle + req) | [doc/lowlevel-guide.md](doc/lowlevel-guide.md) | The event loop, handle/request lifetimes, and what `<uvcpp.h>` actually aggregates |
| JSON (core) | [doc/json-guide.md](doc/json-guide.md) | Building a JSON response by hand: the escape contract, sticky failures, limits, and how it meets the response layer |
| JSON (reflection) | [doc/json-reflect-guide.md](doc/json-reflect-guide.md) | Declaring a field table with one macro and sharing it in both directions: the two-layer split, read semantics, error reporting, and what C++11 costs |
| net | [doc/net-guide.md](doc/net-guide.md) | TCP/UDP clients and servers; the async and sync modes, and where they must not be mixed |
| web (HTTP half) | [doc/web-http-guide.md](doc/web-http-guide.md) | HTTP server/client/parser/static server; why shutdown takes two calls |
| web (WS half) | [doc/web-ws-guide.md](doc/web-ws-guide.md) | WebSocket handshake, frames, close codes |
| webapp (framework) | [doc/webapp-guide.md](doc/webapp-guide.md) | Routing, middleware, request/response, upload, static, WebSocket, logging |
| webapp (support types) | [doc/webapp-support-guide.md](doc/webapp-support-guide.md) | The seven types under the framework: connection identity, web utilities, MIME, multipart, file transfer, per-request context, console logging |
| ssl | [doc/ssl-guide.md](doc/ssl-guide.md) | TLS context and per-connection wrapper — the shortest header set and the easiest to get wrong |
| http2 | [doc/http2-guide.md](doc/http2-guide.md) | Using the low-level session/connection layer; for progress and trade-offs see [doc/http2-status.md](doc/http2-status.md) |
| quic | [doc/quic-guide.md](doc/quic-guide.md) | The QUIC transport: the build contract, why it needs an OpenSSL ≥ 3.2, the API shape, and an honest list of what does not work yet |
| http3 | [doc/http3-guide.md](doc/http3-guide.md) | HTTP/3 as the web layer's second transport: the three QUIC extensions it needed, the API shape (session vs connection, the three critical streams), the CMake wiring, and why HTTP/1.1 is unaffected by it |
| expand | [doc/expand-guide.md](doc/expand-guide.md) | Memory pool, page heap and span, and why they ship disabled |
| WSDL (document + publishing) | [doc/wsdl-guide.md](doc/wsdl-guide.md) | Parsing a WSDL 1.1 document into a model, looking things up by QName, serving it or generating one |
| SOAP (envelope + dispatch) | [doc/soap-guide.md](doc/soap-guide.md) | Envelopes and `soap:Fault` in 1.1 and 1.2, the dispatch key derived from the binding, the nine rejections and which side each one belongs to, and why the response wrapper is not the dispatch key |
| C ABI (`uvcpp_c_*`) | [doc/capi-guide.md](doc/capi-guide.md) | The `extern "C"` surface for C#/P-Invoke and other FFI: the option and its guard chain, the five ABI rules (error codes, callback-table `size`, ownership classes, thread rule, ABI version), what each module provides and deliberately does not, and what the mutation table actually measured. **1.4.1 ships the foundation + net; the other modules are not there yet** |

Outside the modules there is also [doc/benchmark.md](doc/benchmark.md) for measured
performance, [doc/build-guide.md](doc/build-guide.md) for every CMake switch and build
tree, [doc/testing-guide.md](doc/testing-guide.md) for the test layers,
[doc/ci-guide.md](doc/ci-guide.md) for CI maintenance, and
[doc/release-process.md](doc/release-process.md) for how a release is cut and what that
pipeline does not check.

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
| `UVCPP_BUILD_FUNCTIONAL` | `ON` | Build the functional test suite (only read when `UVCPP_BUILD_TESTS=ON`) |
| `BUILD_SHARED_LIBS` | `ON` | Build shared libraries |
| `UVCPP_BUILD_STATIC` | auto | Build static uvcpp library |
| `UVCPP_BUILD_SHARED` | auto | Build shared uvcpp library |
| `UVCPP_FOLLOW_LIBUV_BUILD` | `ON` | Keep `UVCPP_BUILD_SHARED`/`UVCPP_BUILD_STATIC` at `auto`, following libuv's own `BUILD_SHARED_LIBS` |
| `UVCPP_BUILD_EXPAND` | `OFF` | Build expand module (memory pool) |
| `UVCPP_BUILD_NET` | `ON` | Build net module (TCP/UDP client/server) |
| `UVCPP_BUILD_WEB` | `OFF` | Build web module (HTTP + WebSocket) |
| `UVCPP_BUILD_WEBAPP` | `OFF` | Build web app framework (router/middleware/static/upload/log). Requires `UVCPP_BUILD_WEB=ON` |
| `UVCPP_BUILD_EXAMPLES` | `OFF` | Build the examples in `examples/` |
| `UVCPP_BUILD_BENCH` | `OFF` | Build the benchmark range in `bench/` — see [`doc/benchmark-rig.md`](doc/benchmark-rig.md) |
| `UVCPP_ENABLE_ZLIB` | `OFF` | Enable zlib (WebSocket compression) |
| `UVCPP_ENABLE_OPENSSL` | `OFF` | Enable OpenSSL (HTTPS/WSS) |
| `UVCPP_ENABLE_NGHTTP2` | `OFF` | Enable HTTP/2 (nghttp2, linked static). Requires `UVCPP_ENABLE_OPENSSL=ON` and `UVCPP_BUILD_WEB=ON` |
| `UVCPP_ENABLE_QUIC` | `OFF` | Enable the QUIC transport (ngtcp2, linked static) in the **net layer**. Requires `UVCPP_ENABLE_OPENSSL=ON`, an **OpenSSL ≥ 3.2 with the QUIC API**, and `UVCPP_BUILD_NET=ON` — force-disabled without them. **1.4.1 is a real link protocol: handshake, streams, close and idle timeout work; HTTP/3 rides on top of it (next row).** See [`doc/quic-guide.md`](doc/quic-guide.md) |
| `UVCPP_ENABLE_HTTP3` | `OFF` | Enable HTTP/3 (RFC 9114) in the **web layer**, parsed by nghttp3 and carried over the QUIC transport (both linked static). Requires `UVCPP_ENABLE_QUIC=ON` and `UVCPP_BUILD_WEB=ON` — force-disabled without them. **1.4.1 is an end-to-end transport: `uvcpp_http_client` / `uvcpp_http_server` speak it, h1/h2 are untouched (it runs on UDP).** See [`doc/http3-guide.md`](doc/http3-guide.md) |
| `UVCPP_ENABLE_WSDL` | `OFF` | Enable the WSDL/SOAP module (XML via pugixml, linked static). Requires `UVCPP_BUILD_WEBAPP=ON`. See [`doc/wsdl-guide.md`](doc/wsdl-guide.md) (the document half) and [`doc/soap-guide.md`](doc/soap-guide.md) (the runtime half) |
| `UVCPP_ENABLE_CAPI` | `OFF` | Export the **C ABI** (`src/capi/`, C99 headers for C#/P-Invoke and any other FFI) out of the same `uvcpp` library — no extra artifact. Requires `UVCPP_BUILD_NET=ON` and `UVCPP_BUILD_WEB=ON`; force-disabled without them (the C surface spans net/web/webapp). **Turned on for every release configuration.** See [`doc/capi-guide.md`](doc/capi-guide.md) |
| `UVCPP_USE_SYSTEM_LIBUV` | `ON` | Prefer system-installed libuv |
| `UVCPP_BUILD_LIBUV_FROM_SOURCE` | `OFF` | Fetch and build libuv from source via `FetchContent` |
| `UVCPP_STATIC_RUNTIME` | `OFF` | Statically link the compiler runtime (`libgcc`/`libstdc++`) into the library. **MinGW and Linux only — a no-op on MSVC**, which uses `/MD` and ships `vcruntime`/`msvcp` in the package |
| `UVCPP_ENABLE_TRY_WRITE` | `ON` | Try `uv_try_write` before copying into a write buffer |
| `UVCPP_TRY_WRITE_MIN_BYTES` | `32768` | Smallest payload worth attempting `uv_try_write` for (a `CACHE STRING`, not an `option()`) |

**Important**: `UVCPP_ENABLE_ZLIB` and `UVCPP_ENABLE_OPENSSL` are NOT auto-enabled
when `UVCPP_BUILD_WEB=ON`. You must opt in explicitly. `UVCPP_ENABLE_NGHTTP2` is
**force-disabled** when `UVCPP_ENABLE_OPENSSL=OFF` (it warns rather than leaving a
configuration that cannot work) — HTTP/2 here has no cleartext mode. `UVCPP_ENABLE_QUIC`
is force-disabled the same way for **three** separate missing prerequisites
(`UVCPP_ENABLE_OPENSSL=OFF`, `UVCPP_BUILD_NET=OFF`, or an OpenSSL that has no QUIC API —
anything below 3.2), each with its own warning that says how to fix it. `UVCPP_ENABLE_HTTP3`
is force-disabled the same way for **two** missing prerequisites (`UVCPP_ENABLE_QUIC=OFF`
or `UVCPP_BUILD_WEB=OFF`), also with its own warning. `UVCPP_ENABLE_WSDL`
is force-disabled the same way when `UVCPP_BUILD_WEBAPP=OFF`: the module is built on top of
the framework.

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

    // One shared read callback for every connection: data, peer-close and
    // read-error arrive as three distinct events, so you never have to work
    // out why the data stopped coming.
    server.set_read_callback([](uvcpp::uvcpp_tcp_client& client,
                                const uvcpp::net_read_result& r) {
        if (r.is_data()) {
            std::cout << "Received: " << std::string(r.data, r.size) << std::endl;
            // Passing the callback is what makes this an async write; with no
            // callback it degrades to write_wait() and waits on the loop thread.
            client.write(r.data, r.size, [](int) {});
        } else {
            std::cout << "Client disconnected" << std::endl;
        }
    });

    // The connection callback is listen()'s first argument, not a separate
    // on_connection(). Set the read callback before listen().
    server.listen([](uvcpp::uvcpp_tcp_client* client) {
        std::cout << "Client connected" << std::endl;
    });

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

        // Session end is (close code, reason) — not the connection pointer.
        conn->on_close([](uvcpp::ws_close_code code, const std::string& reason) {
            std::cout << "WS client disconnected: "
                      << static_cast<int>(code) << " " << reason << std::endl;
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
    // ... set_read_callback, listen, run
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

### Optional gate: run the whole suite under PageHeap

On a plain run a freed page is still mapped and still holds the old bytes — **use-after-free is
silent**. Full PageHeap unmaps a freed block immediately, turning the same read into an access
violation on the spot. It once caught 8 use-after-free cases while the normal build, the
no-memory-pool build and the unit-test layer were all green.

```bash
# Needs gflags.exe from the Windows SDK debugging tools (usually an elevated shell)
python -u tests/tools/run_pageheap_gate.py --tree build-webapp
```

It takes a baseline by running the suite plainly first, then goes case by case
"enable PageHeap → re-read the registry → run → disable → re-read", and only counts
"green baseline, crashed under PageHeap" as a catch. Exit codes: `0` all green,
`1` the gate failed, `3` the gate itself could not run (baseline red / PageHeap never took
effect / did not clean up / it was exercising a stale DLL).

**It is markedly slower**, which is why it is not in the default ctest suite. PageHeap is
switched off in each case's `finally`, with `atexit` and `Ctrl-C` as backstops — leftovers make
every later test on the machine an order of magnitude slower.

Test coverage:
- **Unit tests**: `tests/unit/` — handle types, request types, uvcpp utilities
- **Functional tests**: `tests/functional/` — runtime behavior for all modules
- **Expand tests**: `tests/expand/` — memory pool allocation tests

---

## Multi-loop scaling (`set_loops`)

One event loop can only ever occupy one core. Once that core is saturated, `set_loops(n)`
runs **1 acceptor loop + n−1 worker loops** inside a single process: the accept path stays
on one thread, and connection I/O is spread across the workers.

```cpp
#include <webapp/uvcpp_web_app.h>
using namespace uvcpp;

int main() {
  uvcpp_web_app app;
  app.set_host("0.0.0.0").set_port(8080).set_access_log(false);

  // 1 acceptor + 3 worker loops. Not chainable, and must precede start()/run().
  const int rc = app.set_loops(4);
  if (rc != 0) return 1;

  app.get("/json", [](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next) {
    resp.json_str("{\"hello\":\"world\"}");
    resp.end();
  });

  app.start();   // n > 1 needs start()/start_background(); run(md) is rejected
  app.join();
  return 0;
}
```

Worth knowing before you turn it on:

- **Call it before `start()` / `run()`.** Register your routes before `start()` as well —
  with `n > 1` that stops being advice and becomes a requirement.
- **`set_loops` returns `int`, so it cannot be chained** onto the
  `set_host(...).set_port(...)` builder. It returns `0` on success, `UV_EINVAL` when `n`
  is outside `1..64`, and `UV_EBUSY` if the runtime has already started.
- **`n == 1` is byte-for-byte the behaviour of never calling it** — no slots, no hooks, no
  extra thread. Switching the knob on at 1 costs nothing.
- **With `n > 1`, `run(md)` is rejected with `UV_EINVAL`.** Use `start()` /
  `start_background()`, then `stop()` and `join()`.
- **Where a connection lands is a platform property, and you can ask which one you
  got.** With `n > 1`, `is_fanout()` answers it: on platforms that accept
  `UV_TCP_REUSEPORT` (Linux) every loop binds its own listener on the same port —
  **index `0` included** — and the kernel spreads new connections across them by 4-tuple
  hash, so `connection_count_at(0)` is normally **non-zero** and nothing beyond "each
  connection is counted in exactly one slot" is promised; on Windows (and anywhere the
  flag is rejected) index `0` is a pure acceptor that hands every connection to `1..n-1`
  by explicit rotation, so it stays at zero. `loop_count()` reports how many loops exist
  and `connection_count_at(i)` how many each holds. **To exercise the other leg on
  Linux**, `set_handoff_forced(true)` (call it **before** `set_loops()`) forces the handoff
  shape — a **test-only** hook that trades kernel fan-out for user-space handoff; don't ship it.

**→ Sizing, core pinning, and what makes a scaling reading valid or invalid:
[doc/benchmark-rig.md](doc/benchmark-rig.md).**

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
│   ├── http2/     # HTTP/2 session/connection layers, nghttp2 glue, ALPN (uvcpp_h2_nghttp2.h is private)
│   ├── quic/      # QUIC transport, ngtcp2 glue (uvcpp_quic_ngtcp2.h / uvcpp_quic_session.h are private)
│   ├── http3/     # HTTP/3 session/connection layers, nghttp3 glue (uvcpp_h3_nghttp3.h / uvcpp_h3_session.h are private)
│   └── ssl/       # SSL/TLS context and connection wrapper
├── tests/
│   ├── unit/      # Unit tests
│   ├── functional/# Functional/integration tests
│   ├── tools/     # Test tooling (e.g. mutation harnesses)
│   └── expand/    # Memory pool tests
├── examples/      # Runnable examples (webapp_demo)
├── doc/           # Documentation
│   ├── benchmark.md       # Measured per-connection memory, throughput, stability
│   ├── build-guide.md     # Every CMake switch, build trees, platform deps
│   ├── capi-guide.md      # The C ABI (uvcpp_c_*): ABI rules, what is provided, what is not
│   ├── ci-guide.md        # CI maintenance guidelines
│   ├── expand-guide.md    # Memory pool / page heap / span usage
│   ├── http2-guide.md     # Using the low-level HTTP/2 session and connection layers
│   ├── http2-status.md    # HTTP/2 support status
│   ├── http3-guide.md     # HTTP/3 in the web layer: the needed QUIC extensions, API shape, CMake wiring, h1 non-regression
│   ├── json-guide.md      # Building JSON by hand: the escape contract and its limits
│   ├── json-reflect-guide.md # Field-table reflection: both directions, read semantics, limits
│   ├── lowlevel-guide.md  # Event loop, handles and requests (the foundation)
│   ├── net-guide.md       # TCP/UDP clients and servers
│   ├── quic-guide.md      # QUIC transport: build contract, API shape, what is not done
│   ├── release-process.md # How a release is cut, and what it does not check
│   ├── soap-guide.md      # SOAP envelopes, fault shapes, dispatch from the binding
│   ├── ssl-guide.md       # TLS context and per-connection wrapper
│   ├── testing-guide.md   # Test layers, filename filters, tests/tools index
│   ├── web-http-guide.md  # The HTTP half of the web layer
│   ├── web-ws-guide.md    # The WebSocket half of the web layer
│   ├── webapp-guide.md    # Web app framework guide
│   ├── webapp-support-guide.md # Types under webapp not covered by the framework guide
│   └── wsdl-guide.md      # WSDL 1.1 model, QName lookup, publishing, and generating one
├── cmake/         # CMake config templates
├── .github/workflows/  # One workflow file per platform, feature matrix inside
├── CMakeLists.txt
├── CONTRIBUTING.md     # Clone -> build -> test, plus the repo's conventions
├── README.md
├── README.zh.md
└── RELEASE.md          # Release notes / history
```

---

## CI & Contributing

CI runs on every push and PR via GitHub Actions. See [doc/ci-guide.md](doc/ci-guide.md) for
the CI maintenance guidelines — contributors modifying the CI must read it first.

Contributions are welcome. Please open an issue or PR, keep changes focused, and follow
the existing code style.

---

## Changelog

The current source tree is **1.4.1** — that is what `UVCPP_VERSION_STRING`
(`src/uvcpp/uvcpp_version.h`) reports. `v1.0.0`, `v1.1.0`, `v1.2.0`, `v1.3.0` and `v1.4.0`
are the tagged releases. Everything the `1.1.x`, `1.2.x` and `1.3.x` development lines
accumulated through `v1.4.0`, plus what the `1.4.x` line has added since, is below, by
theme, with the version each change first appeared in;
release notes for the tagged versions are in [RELEASE.md](./RELEASE.md). Several of the
fixes came from issue reports by the project's first external contributor,
[@sercebr](https://github.com/sercebr).

### QUIC transport (net layer)

- [ngtcp2](https://github.com/ngtcp2/ngtcp2) is wired into the build behind
  `UVCPP_ENABLE_QUIC` (**off by default**), statically linked, with the same FetchContent
  + private-header pattern nghttp2 uses for HTTP/2 (`1.4.1`)
- **1.4.1 turns the skeleton into a real link protocol**: handshake, streams,
  connection close and idle timeout all work end to end. The `UV_ENOSYS` contract the
  skeleton carried was pinned by `tests/functional/quic_api_func.cpp` rather than promised
  in prose, so implementing it turned that test red and forced the contract to be rewritten
  deliberately instead of the claim quietly going stale. `quic_api_func.cpp` now pins the
  *implementation*: 81 checks covering the endpoint contract, the close path and idle
  timeout. New: `quic_handshake_func.cpp` (22 checks) and `quic_stream_func.cpp`
  (33 checks) run a real server and a real client on one loop (`1.4.1`)
- **The stream events are per-stream.** `on_read` gained an `int64_t stream_id`
  parameter and a new `on_write` completion callback was added — one call to
  `write_stream()` is one `on_write`, the same "accepted, not sent" contract
  `uvcpp_tcp_client::write()` already had. No `uvcpp_quic_stream` class: the style is
  `src/http2/uvcpp_h2_session.h`'s, where callbacks carry the stream id (`1.4.1`)
- **The read side ends per stream, at the moment it ends**: a peer FIN reports
  `PEER_CLOSED` from `on_read`; a peer `RESET_STREAM` reports there too (app error code 0
  → `PEER_CLOSED`, non-zero → `READ_ERROR`). `on_stream_close` is deliberately left
  unwired — it fires only when *both* directions are done, by which time the read side has
  nothing new to say, and reporting from it would make callers finish one stream twice
  (`1.4.1`)
- **A latent data-loss bug on a quiet connection got fixed**: `write_stream()` called from
  *outside* a callback (right after `connect()`, or from a user timer) queued the bytes and
  then relied on the next inbound packet to push them out — on a settled connection that
  packet never comes, and the wait ends in an idle timeout. The flush loop skipped picking
  a stream on its first round, and a quiet connection has no pending non-stream frame, so
  `writev_stream` returned 0 and the loop treated that as "done". Caught by
  `quic_stream_func.cpp`'s third phase, which writes from outside a callback on purpose
  (`1.4.1`)
- Turning it on requires `UVCPP_ENABLE_OPENSSL=ON` **and** an OpenSSL ≥ 3.2 with the QUIC
  API (`SSL_set_quic_tls_cbs`) **and** `UVCPP_BUILD_NET=ON`; without any one of the three
  the switch is force-disabled with a warning that says how to fix it, instead of leaving
  a configuration that configures but cannot link (`1.4.1`)
- The parts that are real are the parts verifiable today: the build contract, `bind()`
  argument validation (`uv_inet_pton` — the address is rejected up front, not at
  `listen()`), the `UVCPP_QUIC_ENABLE` config macro, and three backend probes that
  genuinely call into `libngtcp2` and `ngtcp2_crypto_ossl_static` (`1.4.1`)
- Fixes a pre-existing defect it happened to need: the OpenSSL discovery block lived
  *inside* `if(UVCPP_BUILD_WEB)`, so `-DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_BUILD_WEB=OFF`
  compiled `src/ssl/` but never defined `UVCPP_SSL_LIBS`, and an empty `UVCPP_SSL_LIBS`
  expands to a **silent no-op** `target_link_libraries()` — the symptom was a link
  failure, and it took the net layer's own TLS client path down with it (`1.4.1`)
- **Still missing**: 0-RTT, connection migration, stateless reset, datagram (RFC 9221),
  multipath, and multi-loop support. The other two gaps this bullet used to carry are
  both closed in 1.4.1: HTTP/3 now sits on top of this transport (next section), and a
  peer `STOP_SENDING` reaches the application as `on_stop_sending`. The transport still
  does not answer one by itself — that is deliberate (the protocol layer above decides),
  so `doc/quic-guide.md` §8 says "the contract changed", not "the gap is gone" (`1.4.1`)
- **The private-header pair.** `uvcpp_quic_session.h` holds `ngtcp2_conn*`, `SSL*` and
  `ngtcp2_path_storage`, so its layout tracks the ngtcp2 version — it is the second
  private header, alongside `uvcpp_quic_ngtcp2.h`. Both are excluded from the install
  (`CMakeLists.txt:2015`) and from the package
  (`tests/tools/package_release.py`'s `PRIVATE_HEADERS`); measured with
  `cmake --install build-quic --prefix /tmp/inst`, which lands exactly the four public
  headers in `include/quic/` (`1.4.1`)
- Also closes a hole the new snippets opened in a **gate**: the four QUIC examples in
  `doc/quic-guide.md` made `tests/tools/check_doc_snippets.py` exit 3 forever on a
  **release package** (QUIC is structurally off there, so those snippets always `[跳]`),
  and the CI step is `exit "$rc"` — the config-contract job was permanently red no matter
  how correct the code was. The tool's `DEFAULT_OFF` table now records modules that are
  structurally off in release packages: they still print `[跳·默认关]` but no longer raise
  the exit code. The cost is written next to that table (those snippets are not compiled
  in CI; judging them needs a package that enables the module), as is the anti-vacuity
  rule — if expected absence absorbs every candidate, it exits 3 again (`1.4.1`)

### HTTP/3 (web layer)

- [nghttp3](https://github.com/ngtcp2/nghttp3) is wired into the build behind
  `UVCPP_ENABLE_HTTP3` (**off by default**), statically linked, in the **web layer**, and
  it needs the QUIC transport underneath (`UVCPP_ENABLE_QUIC=ON`) — h3 *is* QUIC, there is
  no h3-over-TCP (`1.4.1`)
- **1.4.1 makes it an end-to-end transport, not a skeleton.** `uvcpp_http_client` gained
  `set_http3_enabled(true)` and `uvcpp_http_server` gained
  `set_quic_ssl_context()` + `listen_quic(port)`, and both go through the **same** routing
  table, the same handler signature and the same `uvcpp_http_response` as h1/h2 —
  handlers do not know which transport they are answering (`1.4.1`)
- **`"h3"` never enters the TCP ALPN list.** Advertising `h3` in a TCP ClientHello is
  meaningless: a peer that picks it feeds h3's binary framing into the HTTP/1.1 parser,
  and the symptom is "connected, writes go out, no response ever arrives, nothing errors".
  The client's TCP ALPN list is unchanged; h3 travels through `uvcpp_quic_client`, which
  carries its own ALPN (`1.4.1`)
- **HTTP/1.1 is not on the h3 path at all** — that was a hard requirement for this
  batch, and it is measured rather than promised: with `-DUVCPP_ENABLE_HTTP3=OFF` the
  compiler command lines for `uvcpp_http_server.cpp`, `uvcpp_http_client.cpp` and
  `uvcpp_tcp_client.cpp` are byte-identical to the previous revision, and
  `nm --print-size --size-sort` gives identical sizes for the h1 hot path
  (`on_tcp_connection`, `on_connection_data`, `on_request_complete`). h3 uses a separate
  context table (the h1 `conn_ctx` gained **no fields**) and UDP sockets that h1 never
  touches (`1.4.1`)
- **It needed three things from QUIC, and all three are now public API**: FIN and
  `RESET_STREAM` told apart (`net_read_result::fin`), a stream-credit event
  (`on_streams_available` + `streams_left()`) so the three critical unidirectional streams
  can be opened when the credit arrives instead of being polled, and `STOP_SENDING`
  (`on_stop_sending` + `shutdown_stream_read()`). `shutdown_stream()` also stopped
  hard-coding error code 0 — cancelling a stream is not the same as finishing it
  (`1.4.1`)
- **The layer is split the way `src/http2/` is**: `uvcpp_h3_session` is a pure
  byte-in/byte-out engine (it does not know what a socket is) and
  `uvcpp_h3_connection` drives it over one `uvcpp_quic_connection`. Only
  `uvcpp_h3_common.h` and `uvcpp_h3_connection.h` ship; `uvcpp_h3_nghttp3.h` (the only
  place that includes nghttp3) and `uvcpp_h3_session.h` are private, matching the quic
  pair (`1.4.1`)
- **Two functional tests pin it**, and they pin values, not "something happened":
  `http3_request_func.cpp` (95 checks — handshake, a real GET served by a real handler,
  the `:method`/`:path`/`:status` token mapping, a body that is exactly `hello-h3`, the
  once-and-only-once completion queue, close) and `http3_web_func.cpp` (92 checks — the
  same thing through the web layer, plus the h1 request answered by the *same* server
  object on the other socket) (`1.4.1`)
- **Two of the mutation-table entries come back 0 red, and that is recorded as such**:
  re-entering nghttp3's write path from inside its own callback is reachable only by
  review or a sanitizer, and one `add_write_offset()` variation is unreachable by
  construction (this layer always sets EOF with the last data block). They are written
  down in `doc/http3-guide.md` rather than counted as covered (`1.4.1`)
- Which half is real and which is not: `POST` bodies work, but request/response trailers,
  GOAWAY, server push, extended CONNECT, streaming routes over h3, and multi-loop h3 are
  **not implemented** — `doc/http3-guide.md` §4 and §8 list them, and the unsupported
  paths are refused explicitly instead of being silently mis-routed (`1.4.1`)
- The same gate hole the QUIC snippets opened now has a second entry: the three examples
  in `doc/http3-guide.md` are judged against a package that has h3 on (3/3 compile), and
  on the release package they print `[跳·默认关]` instead of raising
  `check_doc_snippets.py`'s exit code — the table and its cost are documented in the tool
  and in §5 of the guide (`1.4.1`)
- The Linux http3 entry carries one extra step that configures **h2 + quic + http3
  together** and asserts all three integration messages are present. Reason: nghttp2,
  ngtcp2 and nghttp3 each add an unconditional target named `check`, and the third one to
  arrive makes `cmake` fail with a collision that names neither the library nor the file.
  It lives on the Linux leg because that is the one leg where all three are configured in
  a single `cmake` invocation anyway — the collision itself is platform-independent
  (`1.4.1`)

### HTTP/2

For the current state of the implementation — what is enforced, what the defaults are, and
what is deliberately not supported — see [`doc/http2-status.md`](doc/http2-status.md).

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
- Stream-level backpressure: `pause_stream()` / `resume_stream()` for the receive
  direction, an outbound per-stream queue cap, and `peer_window_size()` (`1.2.25`). This is
  **protocol-layer machinery only — no application-layer caller exists yet**; nothing in
  the web framework drives it, so streamed h2 request bodies remain unavailable

### Horizontal scaling (multiple event loops)

- `uvcpp_tcp_server::set_loops(n)` spreads accepted connections over `n` event loops — one
  acceptor loop plus `n-1` worker loops, each on its own dedicated `std::thread` (not a
  libuv threadpool thread). Not calling it, or `set_loops(1)`, is byte-for-byte the old
  behaviour (`1.2.21`)
- Sockets move between loops through a handoff primitive of their own
  (`net/uvcpp_socket_handoff.h`): `WSADuplicateSocketW` + `WSASocketW` on Windows, `dup()`
  elsewhere. **On Windows this rides a known libuv defect** — read
  [doc/net-guide.md](doc/net-guide.md) §4 and [RELEASE.md](./RELEASE.md) before enabling it
- `set_handoff_forced(true)` (`1.3.15`) forces the acceptor + handoff shape so the POSIX
  `dup()` leg can be exercised on Linux too (**a test hook**; don't ship it)
- `set_loop_start_hook()` (`1.2.22`) and `set_loop_exit_hook()` (`1.2.24`) run on each
  worker loop, so an owner can create and tear down handles there; a throwing start hook
  becomes `UV_ECANCELED` instead of a promise that never resolves
- Connection ids encode the loop index in their high 20 bits, so ids stay unique across
  loops; at `n == 1` the encoding is the identity and ids are unchanged (`1.2.22`)
- `uvcpp_web_app::set_loops(n)` (`1.2.23`) brings the same to the framework, with
  `loop_count()` and `connection_count_at()`. Shutdown now fans out per loop slot — the
  earlier path posted to loop 0 only, so worker loops never entered shutdown, their handles
  were never freed, and `uv_loop_close` failed with `UV_EBUSY`, leaking the loop
- `uvcpp_web_app::connections()` now returns **this loop's** registry and
  `connection_count()` sums **all** loops (`1.2.23`) — use `connection_count_at(int)` per
  loop. Same names, same signatures: old code compiles and reads the wrong table once
  `set_loops(n > 1)` is in play

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
- `set_keep_alive(false)` actually sends `connection: close` when the caller has not set the
  header itself, and a response arriving on a connection the peer is closing no longer
  reports a write that can never complete (`1.2.2`)
- `uvcpp_web_request::take_from()` empties the source request's `url` and `headers` as well
  as its `body`; the contract is in the header and asserted up front (`1.2.7`)
- `uvcpp_web_router::match()` no longer fills in `allow` / `allowed_methods` on a successful
  match — those fields are empty when the result is `MATCHED` (`1.2.12`)
- Every response the framework serialises carries a `Date` (both HTTP/1.1 and HTTP/2, whole
  and streamed), formatted once per second instead of once per response. The two messages it
  does *not* serialise — the raw `100 Continue` and the WebSocket `101 Switching Protocols`
  handshake, both written as literal byte strings rather than routed through the response
  sink — carry no `Date`. RFC 9110 §6.6.1 requires `Date` on 2xx/3xx/4xx and only *permits*
  it on 1xx/5xx, so leaving those two without one is conformant rather than a deliberate
  exclusion. `UVCPP_SERVER_TOKEN` / `uvcpp::server_token()` name the default `Server` value
  (still without a version, on purpose), and `uvcpp::version_string()` finally exposes
  `UVCPP_VERSION_STRING`, which had no reader anywhere in the tree (`1.3.1`)

### TLS & networking

- The full certificate chain is loaded, and the TLS version floor and ceiling both apply (`1.1.13`)
- TLS handshakes have a timeout, with no orphaned timer left behind (`1.1.14`)
- Destroying a `tcp_client` from inside its own callback no longer accumulates wrappers (`1.1.16`)
- A dropped peer fires the HTTP client's close observer instead of leaving the callback
  silent forever (`1.1.4`)
- `tls_verify_mode::PEER_STRICT` verifies the hostname on the client: the name passed to
  `connect()` is pinned to the certificate (`X509_VERIFY_PARAM_set1_host()`, or
  `set1_ip_asc()` for an IP literal), so a peer whose certificate names something else never
  establishes (`1.2.24`). It used to be exactly equivalent to `PEER` — it still is on the
  server side, which sends no SNI and asks for no client certificate, leaving no name to check
- Callers that use `uvcpp_ssl` directly, bypassing `tcp_client` / `http_client`, do not get
  that check; call `uvcpp_ssl::set_verify_hostname()` yourself

### Memory & buffers

- Large allocations count towards `span->in_use` again — they leaked a whole span each time (`1.1.12`)
- The second write slot of `uvcpp_write` no longer leaks when its occupant is replaced (`1.1.29`)
- The memory pool's in-use block count follows allocation again (`1.1.33`)
- The compression variant table's byte account is derived from the table rather than kept in
  a counter that could drift, and the byte cap finally has a test (`1.1.34`)
- `memory_pool_config::max_total_memory` is enforced: the pool accounts the bytes it really
  holds, block headers included, and refuses allocations past the cap, counting them in
  `failed_allocations` (`1.2.3`). The default of `0` means no cap and changes nothing
- The pool's SUPER tier (>256 KiB) no longer leaks — its push to the thread-local cache
  claimed success and dropped the block, so the memory never reached the global pool and the
  quota never came back, while the stats still recorded a release (`1.2.9`)
- Three release paths read a block header after `free()` (use-after-free), and
  `static_release_callback`'s teardown branch returned an `_aligned_malloc`'d block with
  `::operator delete` (`STATUS_HEAP_CORRUPTION`) — both fixed (`1.2.9`)
- The pool can be wired to the global `operator new`: its own metadata went through `new`,
  which re-entered a function-local static's initialisation guard and deadlocked (`1.2.13`)

### API contracts & safety

- `uvcpp_handle`'s copy constructor, copy assignment and `clone()` are **deleted** from the
  public interface — they `memcpy`'d a live `uv_handle_t`, loop and neighbour pointers
  included, which double-frees or unlinks a running handle from its loop (`1.2.5`).
  `uvcpp_req`'s copy operations stay, since a `uv_req_t` has no intrusive queue and no loop
  pointer; only its `clone()` is gone (`1.2.5`)
- `DEFINE_FUNC_REQ_CPP` / `DEFINE_COPY_FUNC_REQ_CPP` compile again — two public macros that
  had never been compiled carried three hard errors, and their tests now cover them (`1.2.2`)
- `uvcpp_ws_connection`'s default constructor has a definition; `uvcpp_ws_connection c;` used
  to compile and fail at link time (`1.2.2`)

### Performance

- `write()` tries `uv_try_write()` first, so what fits is not copied into the pending buffer (`1.1.22`)
- `uvcpp_stream::try_write()` no longer copies the `uv_buf_t` array (`1.1.23`)
- Static responses get a compressed-variant cache (`1.1.24`)
- Response bodies are taken over without a copy and go out with the headers as two write
  blocks (`nbufs = 2`) (`1.1.28`); the variant table stores and returns handles instead of
  whole bodies (`1.1.31`)
- Responses are serialized without `std::ostringstream`: −2.52% total CPU, −10.19% user,
  91,330 → 93,688 RPS, with all nine endpoints' bytes unchanged (`1.2.4`)
- The read buffer is no longer zeroed — libuv hands a TCP stream 64 KiB and a request takes
  two trips, so every request wiped 128 KiB for nothing: −13.66% total, −22.88% user,
  +12.02% RPS (`1.2.4`)
- The response header table reserves on first insert, and `uvcpp_web_context`'s object and
  control block became a single allocation — 16.00 → 13.00 allocations per request (`1.2.20`)
- The in-flight request queue is no longer rebuilt per request: on teardown the now-empty
  vector is `swap`ped into a per-loop recycle slot and swapped back on the next enqueue
  (both `swap`s, O(1), no allocation) while the **key is still erased** — an empty key left
  in the table would permanently exempt that connection from the idle sweep, a steady leak
  on a long-running server. 23.02 → 22.02 allocations per request (`1.3.20`)
- The response side's two per-request tables (response headers + `on_sent` callbacks) are no
  longer rebuilt per request either: on teardown they are cleared and `swap`ped into a
  per-loop recycle slot, then swapped back in **before the next request is dispatched**. That
  removes the `reserve(4)`, the fifth header's growth, and the access-log `on_sent` growth in
  steady state. The price is that what is handed to the next request must be an **empty**
  table, so neither `clear()` may be dropped. 22.02 → 19.02 allocations per request
  (`1.3.21`)
- The write path's five per-response heap allocations became one: the new
  `uvcpp_tcp_client::write_owned()` lets the caller bring its own write request, so the
  `uv_write_t`, the `uvcpp_write`, and both `std::function` closures are no longer
  constructed per response, and the head no longer goes through a temporary
  `uvcpp_buf` (the body keeps travelling as an iovec view — still zero-copy). A
  per-connection recycle slot holds the request while it is not in flight. 19.02 →
  13.02 allocations per request (`1.3.22`)
- The HTTP server's read path now uses the framework's event-style read callback, so
  each read no longer `clone_data()`s a throwaway copy first (`data` is valid only for
  the duration of the callback, which is exactly what the parser needs). Event
  granularity and the `nread < 0` semantics are unchanged. 13.02 → 12.02 allocations
  per request (`1.3.22`)
- `uvcpp_web_next` is no longer a `std::function<void()>` but a class holding a single
  reference to the context: `std::function` only uses its inline storage when the
  callable is **trivially copyable** (GCC's `__is_location_invariant`, which is
  independent of `sizeof`), and a closure holding a `shared_ptr` never is — so every
  link of the chain cost two heap allocations (one to build it, one more to pass it by
  value), twice per request here. Both are gone. The semantics are unchanged (keeping a
  copy still means "suspend", the by-value copy still **must not** be turned into a
  move — the criterion is exactly that copy raising the refcount — and calling an empty
  `next` still throws). The price, stated plainly: `sizeof(uvcpp_web_next)` **32 → 48**
  and `sizeof(uvcpp_web_stream)` **288 → 304**, every other public type unchanged — a
  **source and ABI break**. 12.02 → 8.02 allocations per request (`1.3.23`)
- The block `uvcpp_web_context::create()` allocates (object and control block in
  one) is no longer handed back to malloc: it stays in a **thread-local** free list
  for the next request (`uvcpp_block_cache` in `uvcpp_alloc.h` — `std::allocate_shared`
  plus a stateless allocator that only adds block recycling). **The memory block is
  recycled, the object is not** — construction, destruction and refcounting are
  untouched, so `shared_from_this()`, `user_data_` and `hold_count_` mean exactly what
  they meant. The price, stated plainly: the block no longer passes through
  `operator delete`, so ASAN/valgrind cannot see it being freed (a known blind spot
  when chasing memory bugs). 8.02 → 7.02 allocations per request (`1.3.24`)
- The **key** of the in-flight request queue table (`loop_slot::inflight`) is no
  longer rebuilt per request: a key now lives as long as `connection alive`
  union `queue non-empty`, whichever ends last (`context_finished()` on the final
  dequeue, `on_close()` when the connection goes). The `std::map` node, and the
  queue buffer hanging off it, therefore go from **once per request** to **once per
  connection**. Every "is anything in flight?" predicate had to follow (`idle_sweep()`
  idle exemption, the first step of `shutdown_step()`): they now test **queue
  emptiness**, never key presence. Both ways of getting it wrong fail silently — a
  keep-alive connection that already finished a request would be exempted from the
  idle timeout forever, and shutdown would wait for a watchdog that never fires.
  M1's empty-queue recycle slot (`out_recycle`) is gone: this is the same win in a
  better shape. 7.02 → 6.02 allocations per request (`1.3.25`)
- The request header table now **moves into a per-connection destination** instead
  of a fresh vector per request: the parser gains
  `take_headers_into(http_headers& dst)` (`clear()` then move the elements in —
  `clear()` only changes the size, it keeps the capacity, so a destination with
  room costs zero allocations), and the HTTP layer points the two view-building
  paths and the accumulating path at `conn_ctx::stream_request.headers` /
  `conn_ctx::request.headers`. The webapp gains
  `uvcpp_web_request::adopt_headers()` / `yield_headers()` (non-virtual, same
  shape as the response tables from the previous cut) plus one spare table per
  connection (`loop_slot::req_hdr_recycle`), claimed before dispatch and returned
  in `context_finished()`. Note that handlers now receive **the per-connection**
  request object (no longer a local in `on_request_complete`), so reading it after
  the handler returns goes from **crash** to **silently seeing the next request**;
  likewise the request headers are empty once the response has gone out (the
  return happens no earlier than `notify_sent()`).
  6.02 → 5.02 allocations per request (`1.3.26`)
- Response serialization no longer builds a throwaway string per request:
  `uvcpp_http_response` gains the **additive** API
  `to_string_into(std::string& out, bool include_body)` (`out.clear()` and then
  write exactly as before, keeping the capacity; `to_string` degrades to "make an
  empty string and call it", so both forms produce byte-identical output), and the
  server points it at `conn_ctx::wire_recycle`, a **per-connection** buffer, so from
  the second request on `est > capacity()` is always false and a whole response
  costs zero allocations. The allocation it removes was pure waste: the temporary
  string's bytes are immediately copied into the write request's own head buffer
  (`uvcpp_tcp_client::write_owned`) and it is destroyed right away. The head
  parameter of `enqueue_write` / `start_write` goes from by-value to by `const`
  reference (both write paths copy the bytes before returning); the queued path
  keeps its own copy.
  5.02 → 4.02 allocations per request (`1.3.28`)
- The registry's two indexes move from `std::map` to `std::unordered_map`, each key
  getting an **explicit mixing** (splitmix64) hash: both keys carry low-bit structure
  (a monotonically increasing `conn_id`; equal-size allocations landing at the same
  page offset), while MSVC's `unordered_map` uses power-of-two bucket counts and an
  identity `std::hash`, so without mixing the table **silently** degrades to chain
  walking -- unmeasurable on Linux, which uses prime buckets. Isolated microbenchmark:
  key comparisons per request **40.45 -> 4.00** (10.11x, n=400); the in-process sampler
  over 3 same-round interleaved pairs puts the registry cluster at **+1.16 pp median
  (3/3 positive)**; the paired microsecond deltas (-0.08/-0.29/-0.46) sit inside the
  device's own +/-0.25 us floor, so they are quoted as corroboration only. `ids()` used
  to be ascending for free from `std::map` iteration; that no longer holds, so it now
  sorts explicitly. **Public layout break: `sizeof(uvcpp_web_connection_registry)`
  120 -> 136 (+16)** (on libstdc++ `unordered_map` is 8 bytes wider than `map`, times
  two tables; the width differs on MSVC, but it is a break there too); `uvcpp_web_app`
  and every other public type keep their size. 3.0161 allocations per request
  (unmoved, `1.3.30`)
- The **take/release** of a `uvcpp_buf`'s own block now goes through a
  **thread-local** free list for the `malloc` family (`uvcpp_malloc_block_cache`,
  exact-size slots, 4 slots x 64 blocks x at most 4 KiB per block => at most
  1 MiB retained per thread): taken in the two "fresh block" branches of
  `resize_impl()`, released in `free_own()` **and in the write request's
  `release_second()`**. Both are inside `#if !UVCPP_ENABLE_MEMORY_POOL` - it is
  deliberately **not** the same family as the previous cut's cache
  (`::operator new` vs `malloc`; mixing them is UB).
  Note why the release has to live on the write-request side: the response body
  block is adopted by the write request (`adopt_body()`; `release_uv_buf()`
  zeroes `capacity_` first), so it never goes through `free_own()` - the version
  that only hooked `free_own()` moved the reading not at all.
  4.02 → 3.02 allocations per request (`1.3.29`)
- The http layer's **response header table**: the backing storage of that
  `std::vector<http_header>` is now kept per connection (`uvcpp_http_response::adopt_tables()`
  / `yield_tables()`, a pair of swaps; claimed in `uvcpp_http_server::on_request_complete()`
  after `resp` is built and before the handler runs, returned at the h1 exit of
  `send_response()`). The same capacity used to be grown from scratch on every request
  -- `reserve(4)` plus two `_M_realloc_insert` (2->4->8) plus the `operator=` that
  `resp = uvcpp_http_response::ok(...)` brings with it: **4.00 allocations per request**
  on the call-site census, while not one extra header byte was ever stored. On the
  `GET /` probe this reads **7.00 -> 4.00 per request** (two interleaved runs; hical
  reads 3.00 on that quantity too).
  Because the table now outlives the request, "the handler always sees a clean, empty
  table" stops being obvious and becomes an **invariant**: it is cleared before the swap
  (the `clear()` in `yield_tables()` is the primary guard, the one in `adopt_tables()`
  is a second net that **cannot be observed when removed alone**), and the new case
  `response_headers_do_not_accumulate` covers it with two criteria -- reading the table
  the handler is handed, plus the end-to-end check -- because "the second response cannot
  see the first request's header" is vacuously true when the residue is an entry with an
  **empty name**.
  Contract surface: `uvcpp_http_response` gains **two non-virtual methods** and no data
  member, so neither `sizeof` nor the vtable moves -- **no ABI break**; `conn_ctx` is a
  private nested type, so `sizeof(uvcpp_http_server)` does not move either. One
  **behavioural** change, stated plainly rather than as "no change": after the table is
  returned, calling `send_response()` a second time on the **same response object** emits
  a message with no headers (it used to emit a second full message) -- two responses to
  one request is already a framing error in HTTP/1.1, and the function itself calls
  `set_header`, so the two calls were never idempotent. Two of the remaining 4.00 come
  from the **temporary response the handler builds itself** (`http_reserve_headers` inside
  `ok()`), whose table belongs to no connection and is out of the recycle slot's reach
  (`1.3.31`)
- Fixes an **extra 2.00 allocations per request** that the previous entry
  (`1.3.31`) introduced **on the webapp path**: the per-connection response-header recycle
  slot (`conn_ctx::resp_hdr_recycle`) and the per-loop webapp slot (`loop_slot::resp_recycle`)
  were **fighting over one buffer**. `uvcpp_http_server::send_response()` takes a
  **reference**, and the response object handed to it is **not necessarily the one that
  claimed the buffer** -- the webapp path is exactly that case: `on_request_complete()`
  claims on its own **local** `resp` and then returns early because `resp.deferred` is set
  (the webapp only sets that bit on the object handed in at dispatch time), while what
  actually gets sent is **the webapp's own** object. An unconditional `yield_tables()`
  therefore swapped the webapp's **capacity-bearing** table into the connection slot and
  handed it a **capacity-0** table instead, and `context_finished()` then pushed that
  capacity-0 table into `resp_recycle` -- **poisoning the slot once per request**, so every
  next request grew its table from zero again. On the acceptance route `/` (`set_loops(1)`,
  global allocation counter) this reads **5.03 -> 3.03 per request** (delta -2.00, two
  interleaved runs, no overlap), back to and slightly below the two entries before H1
  (`1.3.28` 3.0356, `1.3.30` 3.0326).
  The fix records **who claimed it** on the connection (`conn_ctx::resp_hdr_owner`) so only
  the object that took the buffer gives it back, plus an RAII backstop for the exits of
  `on_request_complete()` that **never reach `send_response()`** (upgrade / deferred).
  This entry needs **two criteria, one per path**, because the **primary guard differs
  between them** (the exact opposite of the `1.3.31` pair, where removing either one alone
  was unobservable):
  · the **capacity** criterion (new case `response_header_capacity_reused`) covers the
    **webapp path** -- dropping the owner test inside `send_response()` turns it red
    (readings `0/0/0`, addresses `nil`);
  · the **allocation integer** (`probe_h`) covers the **http-layer path** -- dropping the
    owner test inside the RAII backstop turns it red (4.0051 -> 7.0053: the same path
    returns the table twice and empties the slot).
    Removing the RAII block entirely leaves **both criteria green** (count 4.0052, capacity
    8) -- it is a **defensive** line: it only matters when something claims and then never
    reaches `send_response()`, which neither arm does today.
  · the new case carries its own **opposite-face control**: the first request must observe
    capacity **0** (cold), otherwise "the second one has capacity" could simply mean capacity
    was always there. It reads **capacity**, not `data()` -- after a swap `reserve` often
    hands back the same address, so an address-only criterion is blind.
  · **the `1.3.31` behaviour note now narrows to the path where the owner test is true**:
    with the hand-back gated on `ctx.resp_hdr_owner == &resp`, only a response object that
    **claimed** the slot gives it back -- on the webapp path nothing is handed back at
    `send_response()` time at all (that side is collected by the per-loop webapp slot, in
    `context_finished()`). Stated structurally, because that is exactly what the two guards
    say; no arm here exercises a double `send_response()`.
  H1's **http-layer** gain is untouched (`probe_h` 4.0051, same as `1.3.31`), and the
  response bytes are **byte-identical** to the previous entry (5 routes: hit, path
  parameter, middleware, 404; only the `date` value is normalised).
  Contract surface: `conn_ctx` is a **private nested type**, so this adds one data member;
  `sizeof` is unchanged for all seven public types, with **no new public name and no ABI
  break** (`1.3.32`)
- Header lookups take a non-owning `const char*` overload (14 class members, four free
  functions), so a literal longer than the SSO limit stops constructing a temporary
  `std::string` (`1.2.16`) — the same for values in `text()` / `html()` / `json()` /
  `json_str()` (`1.2.17`)
- `uvcpp_http_parser::take_headers()` moves elements rather than the vector, so the parser
  keeps its header high-water mark across requests — 19.00 → 18.00 allocations per request
  (`1.2.18`)
- `uvcpp_http_request` has real move operations again: a user-declared copy assignment had
  suppressed the implicit move assignment, so `req = std::move(...)` on the claim path
  silently deep-copied the whole header table (`1.2.8`)
- The 14 `uvcpp_buf` entry points that resized and then `memcpy`'d no longer zero bytes they
  are about to overwrite — a 100 B GET went from 3.4 `memset`s / ~200 B to 0, and a 1 MiB
  POST from 38 / 3.00 MiB to 0 (`1.2.15`)
- The write queue coalesces what it can carry into one write instead of one per completed
  block — segments per request 3.43 → 2.06 (−40%), server CPU per request about −30%,
  RPS 38,839 → 57,067 (`1.2.14`)
- A response on an idle connection skips the write queue (`1.2.10`), a plain response no
  longer pays two heap allocations for its completion callback (`1.2.6`), and the router
  stops computing `allow` on a hit (`1.2.12`)

**Measured, not estimated** — see [doc/benchmark.md](doc/benchmark.md): **4.62 KiB per idle
connection** (least squares over eight tiers, R² = 0.999987 → 1 M connections ≈ 4.42 GiB),
75 k RPS on a single event loop, and a 10-minute soak at 38 M requests / 0 errors. That page
also compares the per-connection figure against [Hical](https://github.com/Hical61/Hical)'s
own report, and states the caveats that comparison carries. The `1.2.x` figures above come
from different rigs and different runs and do not add up with these. A load rig of its own
ships in `bench/` behind `UVCPP_BUILD_BENCH` (off by default, not built in CI) and is
described in [doc/benchmark-rig.md](doc/benchmark-rig.md).

### Configuration, packaging & CI

- Enable macros ship with the package, and a macro set that disagrees with the DLL is now a
  compile-time failure instead of a silently wrong value (`1.1.27`)
- 22 public headers carry a UTF-8 BOM, so consumers that do not pass `/utf-8` no longer fail
  in a cascade (`1.1.30`)
- The Linux package's `libuvcpp.so` moved from `bin/` to `lib/`, where the docs point (`1.1.28`)
- Six packaging jobs pass `UVCPP_ENABLE_NGHTTP2`, so h2 packages no longer lack it (`1.1.6`)
- Every package now also carries a **Debug** build of the library (`uvcppd.dll` / `libuvcppd.so`,
  `-luvcppd`, plus `uvcppd.pdb` on MSVC) for stepping through library internals (`1.1.35`)
- Twelve guides were added: eight for users (`lowlevel-guide.md`, `net-guide.md`,
  `ssl-guide.md`, `web-http-guide.md`, `web-ws-guide.md`, `http2-guide.md`,
  `expand-guide.md`, `webapp-support-guide.md`) and four for contributors (`CONTRIBUTING.md`,
  `build-guide.md`, `testing-guide.md`, `release-process.md`) (`1.2.1`)
- Three documentation gates run in CI: the build system's options and the README option
  table close both ways, every link, path and anchor resolves, and each guide is referenced
  from prose (`check_docs.py`); the `file:line` references across 22 documents resolve and
  match a content lock (`check_doc_lines.py`); every `cpp` snippet compiles against a staged
  package with no `-D` flags (`check_doc_snippets.py`) (`1.2.1`)
- Four examples in the two READMEs did not compile and were fixed (`1.2.1`)
- ABI note for consumers: the layout of `uvcpp_buf` (`1.1.28`) and `uvcpp_http_server`
  (`1.1.34`) changed, and the `1.2.x` line changed more — `uvcpp_h2_stream` (`1.2.25`),
  `uvcpp_ssl_context` (`1.2.24`), `uvcpp_memory_pool` (`1.2.x`) and the five multi-loop
  classes (`1.2.21`–`1.2.23`) all have new layouts, and `uvcpp_handle`'s copy constructor,
  copy assignment and `clone()`, plus `uvcpp_req::clone()`, were **removed** (`1.2.5`) —
  **rebuild, do not just swap the binary** (see [RELEASE.md](./RELEASE.md))

- One line did not compile on Windows: `owned_write_state` initialised
  `uv_buf_t head = {nullptr, 0}` with the **Unix** member order, while Windows declares
  `uv_buf_t` as `{ULONG len; char* base;}` (`uv/win.h`) and Unix as `{char* base; size_t len;}`
  (`uv/unix.h`) — the two members are **mirrored**, so MSVC raised C2440 (`nullptr` into a
  `ULONG`) and MinGW the same error, and every Windows job had been red since the
  write-path cut (`1.3.22`). It now uses `uv_buf_init(nullptr, 0)`, which is
  member-order independent (`1.3.27`)
- CI is now **one workflow file per platform** — `ci-linux-ubuntu.yml`,
  `ci-windows-msvc.yml`, `ci-mingw64.yml`, `ci-macos.yml` — each with its own triggers and a
  **feature matrix** (basic / web / ssl / h2 / quic / full) written once per file, replacing
  the single 1369-line `ci.yml` that spanned four platforms and ten jobs. The README carries
  four per-platform badges instead of one aggregate, so a red leg names the platform that is
  red. QUIC gained **macOS and Windows MSVC** entries (only Ubuntu builds its OpenSSL 3.5 from
  source; the other two use the package manager's, and macOS pins `-DOPENSSL_ROOT_DIR` because
  `openssl@3` is keg-only), and a new gate, `check_ci_layout.py`, holds the files and
  `doc/ci-guide.md`'s layout table equal **both ways**: a silently deleted matrix entry, a
  renamed one that would take the steps guarded by `if: matrix.feature == …` with it, or a
  badge pointing at a deleted file is red rather than unnoticed (`1.4.1`)
- HTTP/3 gained the **same three** entries — Linux, macOS and Windows MSVC (no MinGW: that
  leg has neither an h2 nor a quic entry). The Linux and macOS cells went green on their
  first run; the Windows cell says in a comment that it could not be checked locally (there
  is no MSVC on the machine that wrote it), so its first green is a CI run — and as of
  `1.4.1` it has not had one, because its `Install deps` step fails before `Configure &
  Build` ever runs (next bullet). The Linux leg of the whole feature was exercised locally
  end to end before it was pushed (`1.4.1`)
- Windows dependency install moved into `.github/scripts/win-openssl-deps.sh`, shared by the
  four MSVC matrix entries, the MSVC `config-contract` job, and `release.yml`'s
  `msvc-x64`. From 2026-09-29 20:46 UTC, `choco install openssl --no-progress` exits
  non-zero **every** time on both `windows-latest` and `windows-2022` (148), while the
  workflow file is byte-identical to the last green run (`git diff 0addc23f 699d27e --
  .github/workflows/ci-windows-msvc.yml` is empty) — the runners changed, not the project.
  The step's real requirement was never "choco exited 0" but "this machine has an OpenSSL
  `find_package(OpenSSL)` will find", so the decision now rests on the file system: look in
  the same four directories the DLL-copy step uses, fall back to `choco install openssl
  --no-progress --yes` with three backoff retries, re-check, and `::error` with the
  directories searched plus choco's own words if there is still nothing. `check_ci_layout.py`
  pins it both ways — the call site must be in `ci-windows-msvc.yml`, and the bare
  `choco install openssl --no-progress` line must appear in no platform file (`1.4.1`)

---

## License

MIT License — see [LICENSE](./LICENSE).

uvcpp — C++ wrapper for libuv
