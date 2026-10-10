<p align="center">
  <img src="./uvcpp.svg" alt="libuvcpp logo" width="160" height="160">
</p>

[![version](https://img.shields.io/badge/version-1.5.3--dev-blue.svg)](./RELEASE.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![Linux (Ubuntu)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml)
[![Windows (MSVC)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml)
[![Windows (MinGW64)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml)
[![macOS](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml)

# libuvcpp

🔧 Modern C++11 wrapper for [libuv](https://github.com/libuv/libuv) — event-driven I/O with
object-oriented APIs, dual-mode async/sync support, HTTP/1.1, WebSocket (RFC 6455), and SSL/TLS.

- **Version**: `1.5.3-dev` — **Author**: `zhuweiye` — **License**: `MIT`
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
| db (SQLite / MySQL / PostgreSQL) | [doc/db-guide.md](doc/db-guide.md) | One `uvcpp_db_client` per connection over three backends, the URL grammar, the status codes, the one cross-backend contract the shared suite enforces (and the three places the backends genuinely differ), parameter binding, transactions and the no-retry-inside-a-transaction rule, what `DECIMAL` costs you, the async facade (`uvcpp_db_async`, callbacks + a future) and the optional connection pool built on the same connection, and how to run the tests against a real server |
| C ABI (`uvcpp_c_*`) | [doc/capi-guide.md](doc/capi-guide.md) | The `extern "C"` surface for C#/P-Invoke and other FFI: the option and its guard chain, the five ABI rules (error codes, callback-table `size`, ownership classes, thread rule, ABI version), what each module provides and deliberately does not, and what the mutation table actually measured. **1.5.3 completes all eight slices** (foundation + net + webapp/web + HTTP/2 + QUIC + HTTP/3 + db, 402 functions); all it needs is `-DUVCPP_ENABLE_CAPI=ON`, and db additionally needs its own `-DUVCPP_ENABLE_DB=ON` plus a backend. **As of 1.5.0 all six release legs ship it**, alongside the C# binding in [`bindings/csharp/`](bindings/csharp/README.md) (which covers the first seven slices, 321 of the 402 — db is a stated gap) |

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
| `UVCPP_ENABLE_QUIC` | `OFF` | Enable the QUIC transport (ngtcp2, linked static) in the **net layer**. Requires `UVCPP_ENABLE_OPENSSL=ON`, an **OpenSSL ≥ 3.2 with the QUIC API**, and `UVCPP_BUILD_NET=ON` — force-disabled without them. **1.4.1 is a real link protocol: handshake, streams, close and idle timeout work; HTTP/3 rides on top of it (next row).** **As of 1.5.0 all six release legs enable it** (earlier packages did not). See [`doc/quic-guide.md`](doc/quic-guide.md) |
| `UVCPP_ENABLE_HTTP3` | `OFF` | Enable HTTP/3 (RFC 9114) in the **web layer**, parsed by nghttp3 and carried over the QUIC transport (both linked static). Requires `UVCPP_ENABLE_QUIC=ON` and `UVCPP_BUILD_WEB=ON` — force-disabled without them. **1.4.1 is an end-to-end transport: `uvcpp_http_client` / `uvcpp_http_server` speak it, h1/h2 are untouched (it runs on UDP).** **As of 1.5.0 all six release legs enable it** (earlier packages did not). See [`doc/http3-guide.md`](doc/http3-guide.md) |
| `UVCPP_ENABLE_WSDL` | `OFF` | Enable the WSDL/SOAP module (XML via pugixml, linked static). Requires `UVCPP_BUILD_WEBAPP=ON`. See [`doc/wsdl-guide.md`](doc/wsdl-guide.md) (the document half) and [`doc/soap-guide.md`](doc/soap-guide.md) (the runtime half) |
| `UVCPP_ENABLE_CAPI` | `OFF` | Export the **C ABI** (`src/capi/`, C99 headers for C#/P-Invoke and any other FFI) out of the same `uvcpp` library — no extra artifact. Requires `UVCPP_BUILD_NET=ON` and `UVCPP_BUILD_WEB=ON`; force-disabled without them (the C surface spans net/web/webapp). **Turned on for every release configuration.** See [`doc/capi-guide.md`](doc/capi-guide.md) |
| `UVCPP_ENABLE_DB` | `OFF` | Enable the **database module** (`src/db/`): one `uvcpp_db_client` per connection over SQLite / MySQL / PostgreSQL, with table-style access to the result rows. It is the only module that **requires a third-party client library** (libsqlite3 / libmysqlclient / libpq) — force-disabled when none of the three is found. **Every release configuration turns it on**, so the prebuilt packages ship it. See [`doc/db-guide.md`](doc/db-guide.md) |
| `UVCPP_ENABLE_DB_SQLITE` | `ON` | Build the SQLite backend of the db module (needs `sqlite3.h` + libsqlite3). Not found ⇒ **this one backend is force-disabled with a warning**; the others still build |
| `UVCPP_DB_SQLITE_FROM_SOURCE` | `OFF` | Build that SQLite backend from a **hash-pinned source amalgamation** (one static `sqlite3.c`, forced PIC) instead of `find_package(SQLite3)`. **The six release legs use it**: linking the system `libsqlite3.so` would add a `DT_NEEDED` and break the packages' self-contained promise, while the system `libsqlite3.a` on Ubuntu 24.04 is not PIC (verified: `R_X86_64_PC32 … can not be used when making a shared object`). Needs network at configure time, or the zip placed by hand; both are described in [`doc/db-guide.md`](doc/db-guide.md) |
| `UVCPP_ENABLE_DB_MYSQL` | `ON` | Build the MySQL backend (needs `mysql.h` + libmysqlclient). Non-standard installs: `-DCMAKE_PREFIX_PATH=…` or `-DUVCPP_DB_MYSQL_INCLUDE_DIR=… -DUVCPP_DB_MYSQL_LIBRARY=…` |
| `UVCPP_ENABLE_DB_PGSQL` | `ON` | Build the PostgreSQL backend (needs `libpq-fe.h` + libpq). Non-standard installs: `-DCMAKE_PREFIX_PATH=…` or `-DPostgreSQL_INCLUDE_DIR=… -DPostgreSQL_LIBRARY=…` |
| `UVCPP_USE_SYSTEM_LIBUV` | `ON` | Prefer system-installed libuv |
| `UVCPP_BUILD_LIBUV_FROM_SOURCE` | `OFF` | Fetch and build libuv from source via `FetchContent` |
| `UVCPP_STATIC_RUNTIME` | `OFF` | Statically link the compiler runtime (`libgcc`/`libstdc++`) into the library. **MinGW and Linux only — a no-op on MSVC**, which uses `/MD` and ships `vcruntime`/`msvcp` in the package |
| `UVCPP_ENABLE_TRY_WRITE` | `ON` | Try `uv_try_write` before copying into a write buffer |
| `UVCPP_TRY_WRITE_MIN_BYTES` | `32768` | Smallest payload worth attempting `uv_try_write` for (a `CACHE STRING`, not an `option()`) |
| `UVCPP_ENABLE_UDP_GSO` | `ON` when Windows **and** `UVCPP_ENABLE_QUIC=ON`, else `OFF` | Send-side UDP Segmentation Offload for the QUIC transport: hand a batch of equal-sized datagrams to the stack in **one** `WSASendTo` via `UDP_SEND_MSG_SIZE`, instead of one syscall per datagram. The segment size is taken from what each aggregate write reports, so the wire carries exactly the datagrams it carries today. **Windows only** — the code is compiled out elsewhere and the switch is a no-op there. See [`doc/quic-guide.md`](doc/quic-guide.md) |

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

The current source tree is **1.5.3-dev** — that is what `UVCPP_VERSION_STRING`
(`src/uvcpp/uvcpp_version.h`) reports. `v1.0.0`, `v1.1.0`, `v1.2.0`, `v1.3.0`, `v1.4.0` and
`v1.5.0` are the tagged releases. Everything the `1.1.x`, `1.2.x` and `1.3.x` development
lines accumulated through `v1.4.0`, plus what the `1.4.x` line (released as `v1.5.0`) added
and what the `1.5.x` line has added since, is below, by theme, with the version each change
first appeared in;
release notes for the tagged versions are in [RELEASE.md](./RELEASE.md). Several of the
fixes came from issue reports by the project's first external contributor,
[@sercebr](https://github.com/sercebr).

### C ABI (`uvcpp_c_*`)

- **The `extern "C"` surface exists**, behind `UVCPP_ENABLE_CAPI` (**off by default**,
  but on for every release configuration and for the `full` CI cell). `src/capi/`
  installs to `include/capi/` and compiles into the existing `uvcpp` library — no extra
  artifact, no extra DLL to copy, no new third-party dependency, no `.def` (`1.4.2`)
- **It is a curated facade, not a per-method mirror of the C++ API.** A faithful mirror
  would be 1500+ entry points (`uvcpp_web_app` alone has ~107 public methods), which is
  not a surface anybody binds to by hand; what is published is the slice a C#/P-Invoke
  caller actually needs. JSON never enters a C header (`uvcpp_json` *is*
  `nlohmann::json`), and neither does any libuv type (`1.4.2`)
- **Five rules carry the ABI, each with a test rather than a sentence**: handles are
  opaque pointers with a magic word *and* a live-handle registry (use-after-free gives
  `UVCPP_C_E_STALE`, not UB); no C++ exception crosses the boundary; callback tables start
  with a `uint32_t size` that is checked field by field, so a field appended at the tail
  cannot break an already-compiled client; every function says which of three ownership
  classes it belongs to; the thread rule is the C++ one, and it is enforced (`1.4.2`)
- **`uvcpp_c_abi_version()` makes the most common P/Invoke failure fail loudly** — a header
  and a `.so` / `.dll` that were not built from the same tree. It is deliberately a
  separate line from the library version, and the test asserts it equals the header
  macro (`1.4.2`)
- **1.4.2 ships the foundation + net**: 35 entry points (5 common, 17 `tcp_client`,
  13 `tcp_server`). The web, webapp, HTTP/2, QUIC and HTTP/3 C surfaces are **not** there
  yet, and `[doc/capi-guide.md](doc/capi-guide.md)` §7 lists what is missing instead of
  implying otherwise — no TLS *parameter* entry points, no UDP, no DNS (`1.4.2`)
- **The mutation table found a false green on its first run**, which is the part worth
  recording. Seven deliberate breakages, expected verdicts written down first; the three
  handle-death mutants survived, because the use-after-free assertions were reading the
  first four bytes of freed memory — where glibc's tcache has already written its `next`
  pointer — i.e. they were measuring the allocator. `uvcpp_c_live_handle_count()`, which
  makes the registry's balance externally observable, is what made them catchable; one
  mutant still survives and is documented as redundant by design (`1.4.2`)
- **1.4.3 lands the webapp and web slices**: `uvcpp_c_webapp.h` (119 entry points) and
  `uvcpp_c_web.h` (29), for **183 functions** in total — the app, routing, middleware,
  static files, uploads, WebSocket routes, `req` / `resp` / `next` / deferred responses,
  plus `http_client`, `http_server` and `ws_server` / `ws_connection` / `ws_client`.
  **JSON still does not enter a C header**: the response side gets
  `uvcpp_c_resp_json_str()` (one string) and the request side gets raw body bytes (`1.4.3`)
- **Callback-scope handles are the one thing in this layer that is easy to misuse, and
  1.4.3 turns that into a mechanism rather than a warning**: `uvcpp_c_req`, `resp`, `next`,
  `ws_req`, `ws_conn` and `http_response` are built **on the stack**, registered on the way
  into the callback and unregistered on the way out (in `FrameScope`'s destructor, so a
  user callback that throws cannot skip it). Using one after the callback returns gives
  `UVCPP_C_E_STALE`, **never UB**. The only thing that may leave a callback is
  `uvcpp_c_deferred`, which really holds `ctx` — deferred responses and cross-thread
  hand-backs both go through it (`1.4.3`)
- **One new error code and no ABI break**: `UVCPP_C_E_NOT_FOUND` (-20010) was appended at
  the **tail** of the enum, which by this layer's own rule is not an ABI change, so
  `UVCPP_C_ABI_VERSION` is still **1**. The criterion is not "we think nothing broke" but
  "`git diff` shows that none of batch 1's 35 symbols changed signature" (`1.4.3`)
- **A third pure-C test, this time a C-written client against a C-written server**:
  `test_capi_webapp_func` (210 checks) starts a pure-C app in the same process and then
  drives it with pure-C HTTP and WebSocket clients — seven requests over one keep-alive
  connection, bodies compared byte for byte, headers round-tripped, middleware ordering, a
  static directory, a cross-thread deferred response that arrives late but on time, both
  shapes of WebSocket close, and the registry's balance checked at the end (`1.4.3`)
- **Batch 3a wires up the HTTP/2 C surface**: `uvcpp_c_http2.h` (46 entry points) brings
  the total to **229 functions**. The C side has exactly one handle — the driver layer
  (`uvcpp_c_h2_connection`) — because exposing the session layer as a second handle would
  ask the caller to write the socket driver again *and* make "which one do I free first"
  a second source of truth. Requests and responses are **constructors** (caller allocates,
  caller frees); `uvcpp_c_h2_stream` is a **callback-scope handle**. **Not provided**:
  priority, dependencies, push, and per-frame callbacks — `uvcpp_h2_session` has no
  priority or push to begin with. A fourth pure-C test, `test_capi_h2_func` (244 checks),
  runs three streams over **one** connection (POST with a body, GET, streaming GET),
  compares status codes and bodies byte for byte, and checks the registry balance at the
  end (`1.4.3`)
- **The symbol lock became slice-aware in this batch** (`#@ module <name>` starts a
  slice), because one leg only ever enables part of the module set: CI's `capi` leg has
  no NGHTTP2, so the h2 slice is *not supposed to be exported there* — a flat lock would
  report "this slice is absent from this tree" as "this symbol was deleted", a very
  persuasive false red. The gate now judges **slice by slice against that slice's own
  switch** (read from the tree's `uvcpp_config.h`): on → the slice must match the export
  surface exactly; off → the slice must appear **nowhere**, plus an explicit "not judged"
  line. In CI the `capi` and `h2` legs together cover every slice, which is why the `h2`
  leg now carries `-DUVCPP_ENABLE_CAPI=ON` (`1.4.3`)
- **The mutation table caught a false green a second time, and this verdict is narrower**:
  `1.4.3` added M8–M12, and the two assertions saying "a callback-scope handle carried out
  of its callback must report `E_STALE`" **failed to catch M8** (deleting `FrameScope`'s
  unregistration). Two separate things were wrong. They used to run *after* `app_join()`,
  where the server thread's stack has already gone back to glibc — the magic read as zero,
  so they passed because the memory was gone, and reading it at all was the very UB the
  assertion claimed to rule out (they now run before the join). And even moved, M8 still
  passes them: the handles live on the stack, so the frame is reused the moment the
  callback returns and the magic is overwritten by unrelated writes — **a magic-based check
  cannot tell "poisoned" from "clobbered by stack reuse"**, so M8 is observable only
  through `uvcpp_c_live_handle_count()`. Those two assertions measure the *contract*, not
  the *mechanism*; both are worth having, and the error is taking the first as evidence for
  the second (`1.4.3`)
- **Batch 3b lands the QUIC and HTTP/3 C surfaces, which completes the layer**: new
  `uvcpp_c_quic.h` (42 entries) and `uvcpp_c_http3.h` (50 entries), bringing the total to
  **321 functions across seven slices**. The QUIC slice is the full "many streams on one
  connection" surface (TLS contexts, ALPN, idle timeout, stream shutdown, byte counters);
  the HTTP/3 slice attaches to a **borrowed** QUIC connection handle, which is why an h3
  connection handle must be freed by its owner inside `on_disconnect` — the rule is in the
  header and pinned by the test (`1.4.4`)
- **An interface gap this batch wrote itself into**: `uvcpp_c_h3_conn_send_response()` was
  **unusable** as first written. C++'s `send_response()` returns `UV_EINVAL` when
  `stream_id < 0`, and the C-side response container's stream id (internally `-1`) had no
  setter at all. The fix is `uvcpp_c_h3_response_set_stream_id()`, which rejects negatives
  **at the boundary** — `-1` is the "no owner yet" sentinel, not a stream, and in QUIC
  **stream 0 is a real stream**, so the default cannot be 0 either (`1.4.4`)
- **Who unregisters a borrowed connection handle is the load-bearing part of this batch**:
  once h3 is attached it **replaces** the connection's whole callback table, so the bare
  QUIC `on_close` trampoline never fires — leaving h3's `on_disconnect` as the only entry
  point for "this connection is gone". Drop it and the borrowed QUIC handle stays in the
  registry forever, and the whole battery of "reusing it must give `E_STALE`" assertions
  **cannot see that** (the handle is still live, the magic is still there). The only thing
  that can is the closing `uvcpp_c_live_handle_count() == 0` — mutation M18 (`1.4.4`)
- **A fifth pure-C test, `test_capi_quic_h3_func` (280 checks)**: it differs from the h2
  one in exactly one place, forced by what QUIC is — it pumps **two loops on one thread**
  (both ends are on UDP, so two threads would mean handling "who runs first" as timing
  luck), then, after the handshake (ALPN `h3`), sends three requests **serially** (POST
  with a body and a header set twice, GET with `_send_status`, GET with an empty 204). The
  price is that it cannot measure `E_WRONG_THREAD`; what it measures instead are two things
  only the C surface has: reading a callback table field by field by `size`, and type
  confusion with a magic per type (passing an h3 handle as a QUIC connection must give
  `E_STALE`) (`1.4.4`)
- **The mutation table grew a third time, and "which tree can run the table" became
  mechanism rather than folklore**: M17–M20 are the quic + h3 four (the endpoint callback
  table's `size`, that unregistration above, h3's `FrameScope`, and the negative stream
  id). The driver itself changed twice: **both callback tables now get a truncated table**
  (h3's and the endpoint's `uvcpp_c_quic_callbacks`), because "one guard, two branches, only
  one of them fed" is exactly what batch 3a's M15 dug up; and **a missing test executable
  now exits 3 and names the mutations that have no criterion left** — the three local trees
  mirroring CI each lack one piece (`build-capi` has no SSL/h2/quic, `build-capi-h3` has no
  webapp), so the whole table needs one tree with everything on, `build-capi-all` (`1.4.4`)
- **The symbol lock gained a third CI leg**: the `http3` leg carries
  `-DUVCPP_ENABLE_CAPI=ON` from this batch on. No single leg can enable all seven slices
  (the `capi` leg has no SSL/h2/quic, the `http3` leg has no webapp), so three legs
  **together** cover all seven: `capi` judges four, `h2` five, `http3` six (in that tree the
  `webapp` slice is judged by the "a disabled module must export nothing" rule and honestly
  prints "not judged"). No slice gets to coast on "another leg will judge it" (`1.4.4`)
- **A batch-3a header defect got fixed along the way, and only "feed each header to a C
  compiler on its own" can see it**: `uvcpp_c_http2.h`'s parameter list names
  `struct uvcpp_c_tcp_client*` without including the header that owns that type,
  `capi/uvcpp_c_net.h`. It compiles fine **through the umbrella** (`uvcpp_c.h` includes
  net first) and fails the moment you include that header **alone** — a tag first seen in a
  parameter list gets a brand-new type scoped to that prototype, one `-Wvisibility` red
  under `-Werror`. The fix is that one include, shaped like `uvcpp_c_quic.h` already does
  it. The lesson is that the umbrella's include order hides this whole class of defect,
  while "include only the one slice I use" is entirely legitimate (`1.4.4`)
- **1.5.3 lands the database module's C surface**: new `uvcpp_c_db.h` (**81 entries**),
  bringing the total to **402 functions across eight slices**. Connections, synchronous
  queries and transactions, parameters, result tables and values, a **connection pool** and
  an **async facade** — C#, Rust and Python's `ctypes` can drive the database module today
  without writing the `uv_queue_work` boilerplate or managing a set of connections
  themselves. **This slice has its own module switch** (`UVCPP_DB_ENABLE`): db hangs off
  neither net nor web, so `CAPI=ON, DB=OFF` is a legal combination, and with none of the
  three backends (SQLite / MySQL / PostgreSQL) built the module is force-disabled. The
  backend-level switches have nothing to do with the **symbol surface** — they only decide
  which names `uvcpp_c_db_drivers()` reports, which is why the symbol lock judges
  `UVCPP_DB_ENABLE` and no backend at all (`1.5.3`)
- **"Adding a slice does not bump it", the fourth time — and this time the criterion is
  blunter than in earlier batches**: `UVCPP_C_ABI_VERSION` stays **1**, because
  `git diff --stat origin/master -- src/capi/` prints **exactly one line**
  (`src/capi/uvcpp_c.h | 12 ++++++++++++`, an `#if UVCPP_DB_ENABLE` include in the
  umbrella) — the seven existing headers and their seven `.cpp` files are untouched, byte
  for byte (`1.5.3`)
- **The C async interface takes no loop parameter — the one shape difference from the C++
  side**: C++'s `uvcpp_db_async::query(uvcpp_loop* loop, …)` takes this library's
  `uvcpp_loop*`, and on the C side **there is nothing legal to pass**: no public function
  produces a `uvcpp_loop*`, and "borrow the caller's own `uv_loop_t*`" is simply wrong —
  `~uvcpp_loop()` calls `uv_loop_close()` on whatever pointer it holds and frees **the
  caller's** memory with it. A parameter nobody can legally fill is worse than no parameter
  (it invites C# to pass a guessed pointer), so `uvcpp_c_db_async_*` takes none: completion
  callbacks always land on the facade's own **lazily started** loop thread, stopped and
  joined by `_async_free()`. To move results onto **your** loop, post one of the
  `uvcpp_c_net.h` `*_post()` wakeups from inside the callback (`1.5.3`)
- **A sixth pure-C test, per-backend this time**: `test_capi_db_func` asserts through
  create-table / insert / query / transaction / pool / async (296 checks on this machine's
  `build-capi-all`, 792 with connection strings for all three backends). The load-bearing
  ones are "after borrowing the only connection, a second `acquire` returns
  `NO_CONNECTION` on timeout instead of hanging", "`_async_free()` inside a callback gives
  `E_STATE`" and "an out-of-range column returns a static NULL view". With no backend at
  all it exits 3 (**not judged**, not passed). All three `capi` CI legs carry
  `-DUVCPP_ENABLE_DB=ON -DUVCPP_ENABLE_DB_SQLITE=ON` from this batch, and the `db-servers`
  job now enables the C surface too, so the C test runs against **real** MySQL and
  PostgreSQL (`1.5.3`)
- **The mutation table's fourth expansion**: M22–M26 are the db five (an out-of-range
  column index, `free` on a borrowed client, `free` on a pool still bound to a facade, a
  callback table missing `on_table`). Expectations were written down **before** running, and
  all five are caught; three of them are caught as **crashes** (one `SIGABRT`, two
  `SIGSEGV`, no `checks=` line), recorded as such rather than read as a plain FAIL
  (`1.5.3`)
- **The db slice of the symbol lock is judged by one leg only, so that leg gained a gate**:
  the `capi` entry is the only one with CAPI and DB open at once, and when
  `UVCPP_ENABLE_DB` is force-disabled the lock **does not go red** — it honestly prints
  "not judged" on the db line and exits 0. In other words that leg could degrade from "the
  only leg judging db" to "a leg not judging db" in complete silence. So it now has a step:
  the configure log must contain `db: SQLite 后端开` and `Including db module in build`,
  and `ctest -N` must list `test_capi_db_func` (`1.5.3`)

### C# binding (`bindings/csharp/`)

- **The C surface now has a real consumer, and it ships with the repo**: two `.cs` files
  under `bindings/csharp/` (`UvcppNative.cs`, 183 declarations + `UvcppNative.Protocols.cs`,
  138 — two halves of one partial class), plus a runnable QUIC echo example (`dotnet run`
  does one round trip by default and asserts `uvcpp_c_live_handle_count()` returns to 0 at
  the end). **The criterion is "not one more, not one fewer"**: the 321 `DllImport`
  declarations and the 321 `uvcpp_c_*` symbols in `tests/tools/capi_symbols.lock` are
  reconciled **both ways**, and both set differences are empty; the check is a plain-text
  script needing neither a library nor a compiler (`1.5.0`)
- **Its first run crashed the library, which is exactly what a first real consumer is
  for**: after a successful echo round trip the example did **not** close the connection and
  called `uvcpp_c_quic_server_free()` directly → SIGSEGV in "iterate `server->conns` while
  `detach_conn()` erases from it" (`unordered_map::erase` invalidates the *current*
  iterator, so `++it` steps onto a reclaimed bucket; a single live connection is enough).
  The fix snapshots the handles into a local vector first; the regression
  `test_free_with_live_conn()` and mutation M21 go with it (`1.5.0`)
- **The same version closed a documentation gap, at the price of echoing twice**: QUIC's
  `on_read` end-of-stream is **two** callbacks — a STREAM frame carrying data **and** FIN
  first reports `DATA` (whose `fin` is **also non-zero**: it is the "this block is the last
  one" bit), then immediately reports `PEER_CLOSED`. So **the end-of-stream criterion is
  `PEER_CLOSED`, not `fin`**. The example's first version keyed on `fin`, echoed a second
  time onto a half-closed stream (ngtcp2 returned -219), and the exception escaped a
  native-invoked callback → abort (measured: exit code 134). This is now written into
  `uvcpp_c_quic.h` and the binding README (`1.5.0`)
- **The compile floor was measured, not guessed**: `net8.0` compiles and really runs;
  `netstandard2.1` + `LangVersion 10` compiles (compile-only); `netstandard2.0` **does not**
  (the reference assemblies have no `UnmanagedType.LPUTF8Str`); C# 9 and earlier **do not**
  (file-scoped namespaces need C# 10) — so "Unity / Mono compile it too" **is false**
  (Unity 2022 is still C# 9), and the claim in `UvcppNative.Protocols.cs` was corrected to
  the measured floors (`1.5.0`)
- **How hand-written P/Invoke declarations go wrong, measured line by line against the real
  headers** (binding README §8): the nastiest cell is `bool uvcpp_c_app_running(app)` —
  managed `bool` marshals as a Win32 `BOOL` (non-zero is `true`), so asking a freed handle
  reads `-20002` (`E_STALE`) through an `int` declaration and **`True`** through a `bool`
  one: "it errored" is read as "it is running". Also "an invented callback parameter"
  (`uvcpp_c_tcp_server_listen` is really `(server, int backlog)`; declare a callback pointer
  there and C reads the pointer's low 32 bits as the backlog — compiles, runs, wrong), and
  "assuming it lends you a string pointer" (`uvcpp_c_req_path` is really "you pass a buffer,
  it returns the length"; as `IntPtr f(req)` it is two arguments short and C writes to
  stack garbage). **That table is the reason this layer needs a checked definition file**
  (`1.5.0`)
- **The db slice (81 entries) is not bound in this batch — a gap stated as a gap**: the
  binding is a hand-written, line-by-line-checked artifact, so it stays honest only when it
  grows with a batch and every line gets read. The lock holds 402 symbols today; the
  plain-text reconciliation script in `bindings/csharp/README.md` §7 prints
  `db 0/81 ← not bound`, while the other seven slices (321) still have **both** set
  differences empty (`1.5.3`)

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
  (`CMakeLists.txt:2389-2389`) and from the package
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
- **A large-payload crash in the send path was root-caused and fixed.** `write_stream()`
  handed ngtcp2 a pointer into a `std::vector<uint8_t>` that kept growing. ngtcp2 stores the
  application's *pointers* (a retransmission frame chain holds them), and `ngtcp2.h` requires
  the covered bytes stay **in tact** until `acked_stream_data_offset` says they are
  acknowledged — so the buffer violated the contract in **two** ways: growth reallocated the
  address away, and dropping the acked prefix `memmove`d the tail up so the address stayed
  but the bytes shifted. Symptom: `--mode=echo --sizes=2097152` died with `0xC0000005` in 3
  of 20 runs, the faulting read inside `ngtcp2_cpymem` ← `ngtcp2_pkt_encode_stream_frame`,
  targeting a `MEM_RESERVE` region (past the committed end of a heap block). Holding address
  and bytes still (a temporary reserve + no-compaction patch) took it to **0/20** against the
  unpatched control's 3/20; the real fix is a **chunked send queue** — 64 KiB chunks, each
  `reserve`d on birth and never written past, dropped only when *entirely* acknowledged —
  which satisfies both constraints by construction, drops in O(1) and shifts no bytes. There
  is no "cost N%" figure for it, and the reason is itself the finding: with HEAD's storage
  swapped back in, the rig reported `DATA MISMATCH` in **6 of 6 runs** (3 rebuilds × push and
  echo, 15 rounds each) at 2 MiB — never printing a timing row at all, and the first bad
  offset wandering between 63 605 and 1 811 177. The old shape ships corrupted bytes every
  time rather than merely running slowly, so it provides no baseline to divide by; the crash
  (3 of 20) was only its loudest symptom. The chunk size is measured rather than guessed
  (16 KiB 25.410 ms / 64 KiB 21.907 / 256 KiB 21.913 one-way at 2 MiB — the knee is at
  64 KiB). `quic_stream_func.cpp` gained a fourth phase that echoes 64 KiB
  and 3 × 64 KiB + 1234 B and compares the **whole stream byte for byte**, because a
  count-based check is immune to exactly the misplacement this bug class produces: a mutant
  that swaps two equal-sized chunks' contents — same length, same accounting, same delivery,
  and the same byte count on both sides — is caught by nothing but the byte comparison, at
  the chunk boundary (`1.5.1`)
- **The QUIC datagram path no longer copies every packet.** Both endpoints try
  `uv_udp_try_send()` first — synchronous, so `data` is dead before the call returns and no
  copy is needed — and fall back to the existing async copy-and-queue path on anything but
  "sent". On a 2 MB push that removes ~1700 `new char[]`/`memcpy`/`delete[]` triples and
  ~1700 extra loop turns. Only success takes the fast path, and that is not conservatism:
  returning on failure would silently drop the datagram, which QUIC would read as loss and
  retransmit (the symptom being "works, but slowly"). libuv returns `UV_EAGAIN` while an
  async send is queued, so a fast-path packet can never overtake a slow-path one (`1.5.1`)
- **The QUIC send path batches its datagrams on Windows.** `UVCPP_ENABLE_UDP_GSO` (derived
  default: on for a Windows build that has QUIC) hands a whole aggregate of equal-sized
  datagrams to the stack in **one** `WSASendTo`, segmented by `UDP_SEND_MSG_SIZE`. On
  Windows libuv sends one datagram per `WSASendTo` (`uv__udp_try_send2()` is a plain loop),
  which is where the per-datagram cost that the MsQuic comparison kept landing on lives;
  Linux already folds up to 20 requests into one `sendmmsg()`, so the switch is compiled out
  there and is a no-op if you turn it on. The segment size is whatever that batch reports,
  never a constant, so the wire carries the datagrams it carried before — and the receive
  half is deliberately not implemented, since a half-open receive side would let libuv hand
  coalesced datagrams to the application as a single read (`1.5.2`)

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

- **As of 1.5.0 all six release legs ship full-featured artifacts** (QUIC + HTTP3 + C API
  enabled). QUIC needs an OpenSSL >= 3.2 with the QUIC API, and the six legs get it from
  different places — the table at the top of `release.yml` records each one: the two Linux
  legs deliberately **do not install** Ubuntu 22.04's `libssl-dev` (3.0.2, no QUIC API —
  installing it only adds an unusable candidate to `find_package`) and build OpenSSL 3.5.0
  in-job instead (`no-shared no-tests -fPIC`, where `-fPIC` is **load-bearing, not
  belt-and-braces** — measured locally by diffing the `CFLAGS` in the generated Makefiles),
  then check `SSL_set_quic_tls_cbs` is *defined* in `libssl.a` with `nm --defined-only`;
  the two MinGW legs use MSYS2's package (3.6.x); MSVC-x64 uses the one the runner script
  provides; MSVC-arm64 builds 3.5.8 itself with `VC-WIN64-ARM no-asm` (`1.5.0`)
- **All six legs' module assertions now read the generated header instead of
  `CMakeCache.txt`**: QUIC/HTTP3 are guarded by `message(WARNING)` plus a plain-variable
  `set(... OFF)`, so the cache still says `=ON` while the compiler sees 0 — a
  cache-grepping assertion **lets a package without the feature through**, which is exactly
  what it exists to stop. The criterion is `<tree>/include/uvcpp/uvcpp_config.h`: the number
  the compiler actually reads, and the one the `config-contract` gate reads. The name
  mapping is copied entry by entry from `_uvcpp_literal01(...)` in `CMakeLists.txt` (the
  first version guessed from the name and wrote `UVCPP_WSDL_ENABLE` for
  `UVCPP_BUILD_EXPAND`; run against a real generated header it failed immediately); the
  four MinGW/Linux legs check **both trees**, because release and debug are two independent
  configures and omitting a flag only breaks one of them (`1.5.0`)
- **As of 1.5.2 the release packages carry WSDL/SOAP as well**, which took the six legs'
  assertion lists from ten macros to eleven. Until then `UVCPP_ENABLE_WSDL` was the one module the six
  legs left off, and the failure mode was quiet: the packages still *shipped*
  `include/wsdl/*.h` (that list is `package_release.py`'s `MODULES`, independent of the
  switch), but with `UVCPP_WSDL_ENABLE 0` in the generated header their whole contents sat
  inside `#if` — headers present, functionality absent. All ten configure sites (six legs,
  four of which also have a debug tree) now pass `-DUVCPP_ENABLE_WSDL=ON`, and all six
  assertion lists add `UVCPP_WSDL_ENABLE`. pugixml is pulled by `FetchContent`, forced
  static, and linked **privately**, so it goes into the DLL like the other private
  dependencies: no new runtime DLL, no `.pc` change, nothing for a consumer to install. A
  new `wsdl` matrix entry on Ubuntu and Windows MSVC compiles **and runs** this configuration
  on every push — which matters because until now the four `wsdl`/`soap` test files had never
  been executed by `ctest` anywhere, the two places that turned the module on being
  configure-only (`1.5.2`)
- **As of 1.5.3 the release packages carry the database module too, and the db in them has
  SQLite as its only backend** (so the six legs' assertion lists now hold **thirteen**
  macros). Each leg passes four `-D`s: `-DUVCPP_ENABLE_DB=ON`
  `-DUVCPP_DB_SQLITE_FROM_SOURCE=ON -DUVCPP_ENABLE_DB_MYSQL=OFF
  -DUVCPP_ENABLE_DB_PGSQL=OFF`. The two `OFF`s are **explicit**, not "not found": the release
  runners do carry `libpq-dev`, so `find_package` succeeds **silently** and `libuvcpp.so`
  quietly grows a `libpq.so.5` `DT_NEEDED` — exactly the shape the legs' `ldd` assertion
  exists to catch. **`PRIVATE` linking does not prevent this**: `PRIVATE` governs headers and
  compile definitions, but on a shared library it still writes `DT_NEEDED`. The SQLite
  backend comes from a source amalgamation (hash-pinned, built static, forced PIC) rather
  than the system `libsqlite3`: linking the `.so` would add a `DT_NEEDED`, and the system
  `libsqlite3.a` on Ubuntu 24.04 is not PIC (measured: `R_X86_64_PC32 against symbol
  'sqlite3CtypeMap' can not be used when making a shared object`). One thing to know before
  touching that assertion: it reads the **generated header**, not `CMakeCache.txt`, because
  when all three backends are missing the module is force-disabled with a plain `set()` — the
  cache keeps saying `ON` while the compiler sees 0, so a cache-only assertion would wave
  through a package with `UVCPP_DB_ENABLE 0`. On the CI side each of the four platforms has
  an entry: Ubuntu, macOS and MSVC turn SQLite on, and the MinGW leg turns it on in both
  configures. The **two remote backends get a job of their own** (Ubuntu's `db-servers`,
  running real MySQL 8.0 and PostgreSQL 16 containers, with `UVCPP_DB_TEST_REQUIRE=1`
  turning "never connected" from "not judged" into a failure), and every side carries a
  reverse assertion — the `db` entries must not log a remote backend as on, and `db-servers`
  must neither log `db: SQLite 后端开` nor register `test_db_sqlite_func` (`1.5.3`)
- **All six legs' timeouts went from 120 to 150 minutes** (with QUIC/HTTP3 on, each leg
  builds ngtcp2 and nghttp3 via FetchContent on top of its own sources, and the two Linux
  legs build OpenSSL as well; msvc-arm64 is 180). The Linux self-containment assertion now
  also names ngtcp2 / nghttp3: this repo pins them static with `ENABLE_SHARED_LIB=OFF`
  today, so they **cannot** show up in `ldd` — naming them turns "somebody flips that
  upstream switch to ON" into a failure you see immediately (`1.5.0`)

---

## License

MIT License — see [LICENSE](./LICENSE).

uvcpp — C++ wrapper for libuv
