<p align="center">
  <img src="./uvcpp.svg" alt="libuvcpp logo" width="160" height="160">
</p>

[![version](https://img.shields.io/badge/version-1.8.2-blue.svg)](./RELEASE.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![CI · Linux · Ubuntu](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-linux-ubuntu.yml)
[![CI · Windows · MSVC](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-windows-msvc.yml)
[![CI · Windows · MinGW-w64](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-mingw64.yml)
[![CI · macOS](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/Antruly/libuvcpp/actions/workflows/ci-macos.yml)

# libuvcpp

🔧 Modern C++11 wrapper for [libuv](https://github.com/libuv/libuv) — event-driven I/O with
object-oriented APIs, dual-mode async/sync support, HTTP/1.1, WebSocket (RFC 6455), and SSL/TLS.

- **Version**: `1.8.2` — **Author**: `zhuweiye` — **License**: `MIT`
- **Languages**: [English](./README.md) · [中文](./README.zh.md)

## Download

<!-- downloads:start -->
The newest release is **v1.8.2**. Six prebuilt packages, each one self-contained — the
Release and Debug builds of the library, all the public headers, `uvcpp.pc` and the CMake
package files, and no extra DLLs or `.so` files to ship alongside. Which modules a given
package enables is recorded per version in [CHANGELOG.md](./CHANGELOG.md).

| Platform | Toolchain | Package |
|---|---|---|
| Windows x64 | MSVC 2022 | [libuvcpp-1.8.2-msvc-x64.zip](https://github.com/Antruly/libuvcpp/releases/download/v1.8.2/libuvcpp-1.8.2-msvc-x64.zip) · 23.3 MB |
| Windows arm64 | MSVC 2022 | [libuvcpp-1.8.2-msvc-arm64.zip](https://github.com/Antruly/libuvcpp/releases/download/v1.8.2/libuvcpp-1.8.2-msvc-arm64.zip) · 24.3 MB |
| Windows x64 | MinGW-w64 (GCC) | [libuvcpp-1.8.2-mingw-x64.zip](https://github.com/Antruly/libuvcpp/releases/download/v1.8.2/libuvcpp-1.8.2-mingw-x64.zip) · 28.8 MB |
| Windows arm64 | MinGW-w64 (clang + libc++) | [libuvcpp-1.8.2-mingw-arm64.zip](https://github.com/Antruly/libuvcpp/releases/download/v1.8.2/libuvcpp-1.8.2-mingw-arm64.zip) · 22.2 MB |
| Linux x64 | GCC | [libuvcpp-1.8.2-linux-x64.zip](https://github.com/Antruly/libuvcpp/releases/download/v1.8.2/libuvcpp-1.8.2-linux-x64.zip) · 26.6 MB |
| Linux arm64 | GCC | [libuvcpp-1.8.2-linux-arm64.zip](https://github.com/Antruly/libuvcpp/releases/download/v1.8.2/libuvcpp-1.8.2-linux-arm64.zip) · 26.8 MB |

Every earlier release is on the **[Releases page](https://github.com/Antruly/libuvcpp/releases)**.
What a package contains, and why it is laid out that way:
[doc/release-process.md](doc/release-process.md).
<!-- downloads:end -->

## Install

Besides the zips, releases ship `.deb` / `.rpm` packages and feed two package
channels hosted on GitHub Pages. Both install the dev surface — headers, the CMake
config, `uvcpp.pc` — so `find_package(uvcpp CONFIG)` and
`pkg-config --cflags --libs uvcpp` work right after installing.

Every package is also attached to its release, so you can install it without the
channel:

```sh
sudo apt install ./libuvcpp1_<ver>-1_amd64.deb ./libuvcpp-dev_<ver>-1_amd64.deb
# or, on the RPM side:
sudo rpm -i libuvcpp1-<ver>-1.x86_64.rpm libuvcpp-devel-<ver>-1.x86_64.rpm
```

**Windows** — run `libuvcpp-<ver>-msvc-<arch>-setup.exe` from the release assets.
It is signed, but with a **self-signed** certificate (there is no public CA
certificate yet), so SmartScreen still warns about an unknown publisher on first
run. It installs to `Program Files\libuvcpp`, optionally adds `bin\` to the system
`PATH`, and registers an uninstaller.

**Debian / Ubuntu** (apt):

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://antruly.github.io/libuvcpp/apt/key.asc |
  sudo tee /etc/apt/keyrings/libuvcpp.asc >/dev/null
echo "deb [signed-by=/etc/apt/keyrings/libuvcpp.asc] https://antruly.github.io/libuvcpp/apt stable main" |
  sudo tee /etc/apt/sources.list.d/libuvcpp.list
sudo apt update
sudo apt install libuvcpp-dev
```

**Fedora / RHEL / openSUSE** (dnf, or `zypper` on openSUSE):

```sh
sudo curl -fsSL -o /etc/yum.repos.d/libuvcpp.repo https://antruly.github.io/libuvcpp/rpm/libuvcpp.repo
sudo dnf install libuvcpp-devel
```

The repository metadata is GPG-signed in both channels (`InRelease` for apt,
`repomd.xml.asc` for dnf); the key is the one fetched above.

> The distro packages link the **system** `libuv` and `zlib` (pulled in as
> `libuv1-dev` / `zlib1g-dev`, `libuv-devel` / `zlib-devel`), while the zips above
> are self-contained. Don't mix them: linking a self-contained copy against a process
> that also loads the system libuv puts two libuv instances in one process.

## Table of contents

- [Download](#download)
- [Install](#install)
- [Overview](#overview)
- [Features](#features)
- [Documentation](#documentation)
- [Requirements](#requirements)
- [Build](#build)
- [Quick Start](#quick-start)
- [Testing](#testing)
- [Multi-loop scaling](#multi-loop-scaling)
- [Project structure](#project-structure)
- [CI & Contributing](#ci--contributing)
- [Changelog](#changelog)
- [License](#license)

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

**Optional features** — each is opt-in, and `UVCPP_BUILD_WEB=ON` switches none of
them on:

| Switch | Adds |
|---|---|
| `UVCPP_ENABLE_ZLIB` | Per-Message Deflate compression (RFC 7692) for WebSocket, and gzip/deflate `Content-Encoding` for HTTP response bodies |
| `UVCPP_ENABLE_BROTLI` | `br` as a second `Content-Encoding` for HTTP response bodies (~11 % smaller than gzip at equal effort) |
| `UVCPP_ENABLE_OPENSSL` | HTTPS / WSS |
| `UVCPP_ENABLE_NGHTTP2` | HTTP/2 (RFC 9113) — TLS + ALPN by default, plus cleartext h2 (prior knowledge, RFC 9113 §3.4) when you ask for it. Nothing upgrades on its own |
| `UVCPP_ENABLE_QUIC` | The QUIC transport (RFC 9000) in the net layer — needs an OpenSSL ≥ 3.2 with the QUIC API |
| `UVCPP_ENABLE_HTTP3` | HTTP/3 (RFC 9114) in the web layer, on its own UDP sockets, so HTTP/1.1 and HTTP/2 are untouched |
| `UVCPP_ENABLE_YYJSON` | A second JSON parser (yyjson) next to nlohmann, in the `json` module rather than the web layer — same accessors, same statuses, faster parse; a private dependency compiled into the same library |

What each switch *requires*, and when it is force-disabled instead of failing the
configure, is in [CMake Options](#cmake-options) below. What each one costs at runtime is
in the matching guide under [Documentation](#documentation).

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

## Documentation

The tables above list **what exists**; the pages below explain **how to use it** — typical
flows, error handling, and the places where the header comments disagree with the
implementation. All of them live in `doc/`, and **every code example in them is compiled by
CI** (`tests/tools/check_doc_snippets.py`), so they are safe to copy.

| Area | Guide | What it covers |
|---|---|---|
| Low level (handle + req) | [doc/lowlevel-guide.md](doc/lowlevel-guide.md) | The event loop, handle/request lifetimes, and what `<uvcpp.h>` actually aggregates |
| JSON (core) | [doc/json-guide.md](doc/json-guide.md) | Building a JSON response by hand: the escape contract, sticky failures, limits, and how it meets the response layer |
| JSON (reflection) | [doc/json-reflect-guide.md](doc/json-reflect-guide.md) | Declaring a field table with one macro and sharing it in both directions: the two-layer split, read semantics, error reporting, and what C++11 costs |
| net | [doc/net-guide.md](doc/net-guide.md) | TCP/UDP clients and servers; the async and sync modes, and where they must not be mixed |
| SOCKS5 | [doc/socks5-guide.md](doc/socks5-guide.md) | A complete client and server: CONNECT / BIND / UDP ASSOCIATE, no-auth and RFC 1929, IPv4/IPv6/domains; the transparent CONNECT proxy built into `uvcpp_tcp_client` / HTTP / WS clients, the UDP data plane, backpressure and timeouts, and the "the defaults are an open proxy" warning |
| web (HTTP half) | [doc/web-http-guide.md](doc/web-http-guide.md) | HTTP server/client/parser/static server; why shutdown takes two calls |
| web (WS half) | [doc/web-ws-guide.md](doc/web-ws-guide.md) | WebSocket handshake, frames, close codes |
| webapp (framework) | [doc/webapp-guide.md](doc/webapp-guide.md) | Routing, middleware, request/response, upload, static, WebSocket, logging — and multi-loop scaling in §19 |
| webapp (support types) | [doc/webapp-support-guide.md](doc/webapp-support-guide.md) | The seven types under the framework: connection identity, web utilities, MIME, multipart, file transfer, per-request context, console logging |
| ssl | [doc/ssl-guide.md](doc/ssl-guide.md) | TLS context and per-connection wrapper — the shortest header set and the easiest to get wrong |
| http2 | [doc/http2-guide.md](doc/http2-guide.md) | Using the low-level session/connection layer |
| http2 (status) | [doc/http2-status.md](doc/http2-status.md) | What HTTP/2 does and does not support yet, and the trade-offs behind that |
| quic | [doc/quic-guide.md](doc/quic-guide.md) | The QUIC transport: the build contract, why it needs an OpenSSL ≥ 3.2, the API shape, and an honest list of what does not work yet |
| http3 | [doc/http3-guide.md](doc/http3-guide.md) | HTTP/3 as the web layer's second transport: the three QUIC extensions it needed, the API shape (session vs connection, the three critical streams), the CMake wiring, and why HTTP/1.1 is unaffected by it |
| grpc | [doc/grpc-guide.md](doc/grpc-guide.md) | The gRPC envelope as a library module: the 5-byte length-prefixed frame and a narrow slice of protobuf wire format, the three-segment response shape (why `grpc-status` rides a trailer and the HTTP status is always 200), the error-code mapping, and what it deliberately does not do |
| expand | [doc/expand-guide.md](doc/expand-guide.md) | Memory pool, page heap and span, and why they ship disabled |
| WSDL (document + publishing) | [doc/wsdl-guide.md](doc/wsdl-guide.md) | Parsing a WSDL 1.1 document into a model, looking things up by QName, serving it or generating one |
| SOAP (envelope + dispatch) | [doc/soap-guide.md](doc/soap-guide.md) | Envelopes and `soap:Fault` in 1.1 and 1.2, the dispatch key derived from the binding, the nine rejections and which side each one belongs to, and why the response wrapper is not the dispatch key |
| db (SQLite / MySQL / PostgreSQL) | [doc/db-guide.md](doc/db-guide.md) | One `uvcpp_db_client` per connection over three backends, the URL grammar, the status codes, the one cross-backend contract the shared suite enforces (and the three places the backends genuinely differ), parameter binding, transactions and the no-retry-inside-a-transaction rule, what `DECIMAL` costs you, the async facade (`uvcpp_db_async`, callbacks + a future) and the optional connection pool built on the same connection, and how to run the tests against a real server. What each database's driver policy is — which backends are in the repository, which client libraries are **not** in the package, and what happens when one is missing — is in [`doc/db-driver-policy.md`](doc/db-driver-policy.md) |
| C ABI (`uvcpp_c_*`) | [doc/capi-guide.md](doc/capi-guide.md) | The `extern "C"` surface for C#/P-Invoke and other FFI: the option and its guard chain, the five ABI rules (error codes, callback-table `size`, ownership classes, thread rule, ABI version), what each module provides and deliberately does not, and what the mutation table actually measured. **1.5.3 completes all eight slices** (foundation + net + webapp/web + HTTP/2 + QUIC + HTTP/3 + db, 402 functions); all it needs is `-DUVCPP_ENABLE_CAPI=ON`, and db additionally needs its own `-DUVCPP_ENABLE_DB=ON` plus a backend. **As of 1.5.0 all six release legs ship it**, alongside the C# binding in [`bindings/csharp/`](bindings/csharp/README.md) (which covers the first seven slices, 321 of the 402 — db is a stated gap) |
| Build & packaging | [doc/build-guide.md](doc/build-guide.md) | What every switch means and how the switches combine, the build trees, the platform dependencies, the repository layout, and the failures worth recognising |
| Performance | [doc/benchmark.md](doc/benchmark.md) | Measured per-connection memory, throughput and stability |
| Benchmark rig | [doc/benchmark-rig.md](doc/benchmark-rig.md) | How to size and pin a benchmark run, and which scaling readings are valid rather than noise |
| Tests | [doc/testing-guide.md](doc/testing-guide.md) | The three test layers, filename-as-filter-key, the `tests/tools/` index, and the gate exit codes |
| CI | [doc/ci-guide.md](doc/ci-guide.md) | The CI job matrix for ordinary pushes, and the runner-side setup it depends on |
| Release process | [doc/release-process.md](doc/release-process.md) | How a release is cut, what the six build legs produce, and what the pipeline does not check |
| Multi-loop design | [doc/multiloop-design.md](doc/multiloop-design.md) | The reasoning behind `set_loops(n)`: the two placement shapes, what had to be split per loop, and what is still open |
| Multi-process design | [doc/worker-process-design.md](doc/worker-process-design.md) | The reasoning behind `set_worker_processes(n)`: re-running `main()`, the POSIX and Windows paths, and the open libuv-level defect on the Windows hand-off path |

The two `-design.md` pages are the reasoning behind a feature rather than instructions for
using it; the two `-status.md`/`-process.md` pages are the same, one level up.

## Requirements

- **C++11** or later
- **CMake** ≥ 3.20
- **Compiler**: MSVC 2019+, GCC 7+, Clang 5+
- **libuv**: auto-fetched via FetchContent if not found on system
- **Platforms**: Windows, Linux, macOS

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON
cmake --build . --config Release --parallel
ctest --output-on-failure -C Release
```

Core is all that is on by default; every further module is one more `-D` —
`-DUVCPP_BUILD_WEB=ON` for HTTP and WebSocket, `-DUVCPP_ENABLE_OPENSSL=ON` for HTTPS,
`-DUVCPP_BUILD_WEBAPP=ON` for the application framework, `-DUVCPP_ENABLE_HTTP3=ON` for
HTTP/3. The four configurations worth copying, the platform dependencies behind them (an
OpenSSL ≥ 3.2 with the QUIC API for QUIC and HTTP/3), and how the switches interact:
[doc/build-guide.md](doc/build-guide.md).

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `UVCPP_BUILD_TESTS` | `ON` | Build test executables |
| `UVCPP_BUILD_FUNCTIONAL` | `ON` | Build the functional test suite (only read when `UVCPP_BUILD_TESTS=ON`) |
| `BUILD_SHARED_LIBS` | `ON` | Build shared libraries |
| `UVCPP_BUILD_STATIC` | auto | Build static uvcpp library |
| `UVCPP_BUILD_SHARED` | auto | Build shared uvcpp library |
| `UVCPP_FOLLOW_LIBUV_BUILD` | `ON` | Keep `UVCPP_BUILD_SHARED`/`UVCPP_BUILD_STATIC` at `auto`, following libuv's own `BUILD_SHARED_LIBS` |
| `UVCPP_SET_SOVERSION` | `OFF` | Set `VERSION`/`SOVERSION` on the shared library, so an installed `libuvcpp.so` becomes the chain `libuvcpp.so.1.8.2` → `libuvcpp.so.1` → `libuvcpp.so`. **This is for distro packages only, and it is off by default on purpose**: the release zips deliberately carry one unversioned `libuvcpp.so` (that is the name `tests/tools/package_release.py` copies and `check_config_contract.py` checks), and the `.deb`/`.rpm` build is the only thing that turns it on. ELF only — a no-op on Windows, and skipped on macOS (there it would rename the file to `libuvcpp.1.dylib`). See [`packaging/README.md`](packaging/README.md) |
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
| `UVCPP_ENABLE_YYJSON` | `OFF` | Enable the **yyjson JSON backend** (`src/yyjson/`) — the library's **own bundled** parser alongside nlohmann, with the same accessor names (`uvcpp_json_get_string/bool/int/double`) and the same statuses — **both directions of JSON come out of that one production library**: `uvcpp_json_doc` / `uvcpp_json_value` parse, and `uvcpp_json_builder` / `uvcpp_json_node` construct a tree that `uvcpp_json_write` serialises with **yyjson's own writer** (no hand-rolled serializer on this path; the old bridge into `uvcpp_json_writer` was removed in 1.7.2). Its parse is faster, its DOM is smaller, and nothing on the path recurses — not the parser, not the writer. **Turned off, nothing about the nlohmann path changes** (`using uvcpp_json = nlohmann::json` and every `uvcpp_json_*` keep their behaviour; the exported symbol set and `DT_NEEDED` of the library are unchanged). yyjson is a **private** dependency (one header + one `.c`, compiled as object code into the same `uvcpp` library — no extra artifact), fetched via `FetchContent`. **As of 1.7.0 it is in the release package** — all six release legs enable it (earlier packages did not), and four CI jobs really build it: the Ubuntu / macOS / MSVC matrix entries plus the single `ci-mingw64.yml` job. See [`doc/json-guide.md`](doc/json-guide.md) §13 |
| `UVCPP_ENABLE_BROTLI` | `OFF` | Enable the **brotli body compressor** — `br` as a second `Content-Encoding` for HTTP response bodies next to gzip/deflate, picked by the same `Accept-Encoding` negotiation (`br` wins on `q`-ties with gzip because it is the smaller of the two; `*` and the zlib-less build are covered too). **What it buys is bytes on the wire, not speed**: at the default quality the same body is ~11 % smaller than gzip level 6, and because the compressed variant is served out of the response cache the per-request cost is the same as gzip's — an entry that compresses without a cache tag pays for the encode every request. The default quality is `5` (brotli's own default), deliberately not `0`. **Turned off, nothing changes** (no `br` in `Accept-Encoding` negotiation, and the exported symbol set / `DT_NEEDED` of the library are unchanged). brotli is a **private** dependency, found via pkg-config/`find_library` when the system has it and built from source via `FetchContent` (tag pinned to `BROTLI_VERSION`) otherwise — **two** static libraries, so a static-library user links `-lbrotlienc -lbrotlidec` themselves. **As of 1.7.0 it is in the release package** — every release leg passes `UVCPP_BROTLI_FROM_SOURCE=ON` so the package stays self-contained — and five CI jobs really build it: the Ubuntu `brotli` + `brotli-no-zlib`, macOS and MSVC matrix entries plus the single `ci-mingw64.yml` job. See [`doc/web-http-guide.md`](doc/web-http-guide.md) |
| `UVCPP_BROTLI_FROM_SOURCE` | `OFF` | Skip the system lookup entirely (`find_package(PkgConfig)` → `find_path`/`find_library`) and build brotli from the source pinned by `BROTLI_VERSION`, linking it statically. **The lookup runs system-first**, so on a machine that happens to have `libbrotli-dev` the "links the static `.a`" promise silently turns into three `DT_NEEDED` entries (`libbrotlienc` / `libbrotlidec` / `libbrotlicommon`) — and nothing turns red, because the release workflow's `ldd` blacklist does not name brotli. Any release leg that enables brotli passes this so its package stays self-contained. The discovery block is skipped *without being called* (its results land in the CMake cache, so calling it once would leak the previous answer into the next configure of the same tree). Default `OFF` leaves today's behaviour byte-for-byte. See [`doc/web-http-guide.md`](doc/web-http-guide.md) |
| `UVCPP_ENABLE_CAPI` | `OFF` | Export the **C ABI** (`src/capi/`, C99 headers for C#/P-Invoke and any other FFI) out of the same `uvcpp` library — no extra artifact. Requires `UVCPP_BUILD_NET=ON` and `UVCPP_BUILD_WEB=ON`; force-disabled without them (the C surface spans net/web/webapp). **Turned on for every release configuration.** See [`doc/capi-guide.md`](doc/capi-guide.md) |
| `UVCPP_ENABLE_DB` | `OFF` | Enable the **database module** (`src/db/`): one `uvcpp_db_client` per connection over SQLite / MySQL / PostgreSQL / ODBC, with table-style access to the result rows. It is the only module that **requires a third-party client library** (libsqlite3 / libmysqlclient / libpq / an ODBC driver manager) — force-disabled only when those four SQL back-ends and the MongoDB one below are all missing. **Every release configuration turns it on, with all three client-library backends**: SQLite is compiled in from source, and MySQL/PostgreSQL are in the package but loaded at run time (next rows); whether the ODBC backend is in a given package depends on whether that build machine had `sql.h` (next-next row). The module also carries a MongoDB client (the row after ODBC), which is **not** part of this back-end list. See [`doc/db-guide.md`](doc/db-guide.md) |
| `UVCPP_ENABLE_DB_SQLITE` | `ON` | Build the SQLite backend of the db module (needs `sqlite3.h` + libsqlite3). Not found ⇒ **this one backend is force-disabled with a warning**; the others still build |
| `UVCPP_DB_SQLITE_FROM_SOURCE` | `OFF` | Build that SQLite backend from a **hash-pinned source amalgamation** (one static `sqlite3.c`, forced PIC) instead of `find_package(SQLite3)`. **The six release legs use it**: linking the system `libsqlite3.so` would add a `DT_NEEDED` and break the packages' self-contained promise, while the system `libsqlite3.a` on Ubuntu 24.04 is not PIC (verified: `R_X86_64_PC32 … can not be used when making a shared object`). Needs network at configure time, or the zip placed by hand; both are described in [`doc/db-guide.md`](doc/db-guide.md) |
| `UVCPP_ENABLE_DB_MYSQL` | `ON` | Build the MySQL backend (needs `mysql.h` + libmysqlclient). Non-standard installs: `-DCMAKE_PREFIX_PATH=…` or `-DUVCPP_DB_MYSQL_INCLUDE_DIR=… -DUVCPP_DB_MYSQL_LIBRARY=…`. **On by default, and on in every release package** (with `UVCPP_DB_RUNTIME_LOAD=ON`, which is what keeps the package loadable without libmysqlclient) |
| `UVCPP_ENABLE_DB_PGSQL` | `ON` | Build the PostgreSQL backend (needs `libpq-fe.h` + libpq). Non-standard installs: `-DCMAKE_PREFIX_PATH=…` or `-DPostgreSQL_INCLUDE_DIR=… -DPostgreSQL_LIBRARY=…`. **On by default, and on in every release package** (with `UVCPP_DB_RUNTIME_LOAD=ON`) |
| `UVCPP_ENABLE_DB_ODBC` | `ON` | Build the ODBC backend of the db module (needs `sql.h` + a driver manager: `libodbc` on Linux, the system `odbc32` on Windows). Unlike the three above, it targets **an interface, not one database** — *what* it connects to is chosen by the connection string's `Driver=` / `DSN=` (Microsoft ships no linkable C client library for SQL Server; `msodbcsql18` *is* an ODBC driver), and it tries no other driver name. Not found ⇒ **this one backend is force-disabled with a warning**; the others still build. It honours `UVCPP_DB_RUNTIME_LOAD` like MySQL/PG: what gets linked is the **driver manager**, which is a client library of the same kind (a plain unixODBC package on Linux/macOS; on Windows `odbc32.dll` belongs to the OS, so linking it costs nothing). So "is this backend built" and "is there a driver for the data source you open" are two different questions — the second is answered at `open()`, in the driver manager's own words |
| `UVCPP_ENABLE_DB_MONGODB` | `ON` | Build the **MongoDB backend** (`<db/uvcpp_mongo.h>`, class `uvcpp_mongo_client`): JSON in, JSON out, a sync core plus async over the libuv thread pool. It is **not** an `uvcpp_db_client` back-end — `uvcpp_db_drivers()` does not list it. Only the headers (`mongoc/mongoc.h`, `bson/bson.h`) are needed at build time: `libmongoc-1.0` / `libbson-1.0` are never linked or shipped and are loaded at run time instead (`UVCPP_DB_MONGOC_LIBRARY` / `UVCPP_DB_BSON_LIBRARY` override the search); if they are missing, `open()` returns `NO_DRIVER` and the process carries on. Not found ⇒ **this backend is force-disabled with a warning**. See [`doc/mongodb-guide.md`](doc/mongodb-guide.md) |
| `UVCPP_DB_RUNTIME_LOAD` | `OFF` | Take the MySQL / PostgreSQL / ODBC / MongoDB symbols from a client library loaded **at run time** (`dlopen` / `LoadLibrary`) instead of linking it; for ODBC that library is the **driver manager** (the actual driver is still loaded by the manager itself). **The six release legs use it, and the reason is the packages' users, not the library**: `DT_NEEDED` (the PE import table on Windows) is resolved at **load time**, so linking libmysqlclient/libpq/libodbc means a machine that has never installed those libraries cannot even *load* the library — even if it only ever runs HTTP and never touches `db`. With this on, such a machine runs everything except "actually connecting to MySQL/PG/ODBC", where it gets `uvcpp_db_status::NO_DRIVER` plus a message naming the candidates that were tried (`UVCPP_DB_MYSQL_LIBRARY` / `UVCPP_DB_PGSQL_LIBRARY` / `UVCPP_DB_ODBC_LIBRARY` / `UVCPP_DB_MONGOC_LIBRARY` / `UVCPP_DB_BSON_LIBRARY` override the search). It changes only *where the symbols come from*, not *which backends exist*: `mysql.h` / `libpq-fe.h` / `sql.h` are still needed at configure time (`MYSQL_BIND`'s layout has to come from the real header), so `UVCPP_DB_MYSQL_ENABLE` / `UVCPP_DB_PGSQL_ENABLE` / `UVCPP_DB_ODBC_ENABLE` still mean "the header was found". Default `OFF` leaves today's behaviour byte-for-byte. See [`doc/db-guide.md`](doc/db-guide.md) |
| `UVCPP_ENABLE_REDIS` | `ON` | Enable the **Redis client** (`src/redis/`): built-in RESP2/RESP3, asynchronous over `uvcpp_tcp_client`, with AUTH / SELECT / pipelines / pub-sub / `HELLO 3` / `rediss://`. **No third-party client library is needed** (not hiredis), which is why it is on by default; it depends on the net module and is force-disabled when `UVCPP_BUILD_NET=OFF`. See [`doc/redis-guide.md`](doc/redis-guide.md) |
| `UVCPP_USE_SYSTEM_LIBUV` | `ON` | Prefer system-installed libuv |
| `UVCPP_BUILD_LIBUV_FROM_SOURCE` | `OFF` | Fetch and build libuv from source via `FetchContent` |
| `UVCPP_STATIC_RUNTIME` | `OFF` | Statically link the compiler runtime (`libgcc`/`libstdc++`) into the library. **MinGW and Linux only — a no-op on MSVC**, which uses `/MD` and ships `vcruntime`/`msvcp` in the package. **On Linux it also applies to every executable, and a *shared* `libuvcpp.so` built with it cannot be combined with a consumer that uses the system `libstdc++.so.6`** — both copies instantiate the same COMDAT templates (`std::once_flag`'s `__once_proxy`, `std::__future_base::_State_baseV2`) and ELF's global symbol scope makes one side jump into the other's frame, so the failure is a run-time segfault or `double free`, **not a link error**. **That is why the release workflow's two Linux legs no longer pass it** (1.7.0 onwards) — the Linux packages therefore need the system `libstdc++.so.6` / `libgcc_s.so.1`, which every distro with a C++ runtime already has. The two MinGW legs still pass it: PE has no global symbol scope, so there is nothing to interpose, and a MinGW user's machine may not have `libstdc++-6.dll`. See [`doc/build-guide.md`](doc/build-guide.md) |
| `UVCPP_ENABLE_TRY_WRITE` | `ON` | Try `uv_try_write` before copying into a write buffer |
| `UVCPP_TRY_WRITE_MIN_BYTES` | `32768` | Smallest payload worth attempting `uv_try_write` for (a `CACHE STRING`, not an `option()`) |
| `UVCPP_ENABLE_UDP_GSO` | `ON` when Windows **and** `UVCPP_ENABLE_QUIC=ON`, else `OFF` | Send-side UDP Segmentation Offload for the QUIC transport: hand a batch of equal-sized datagrams to the stack in **one** `WSASendTo` via `UDP_SEND_MSG_SIZE`, instead of one syscall per datagram. The segment size is taken from what each aggregate write reports, so the wire carries exactly the datagrams it carries today. **Windows only** — the code is compiled out elsewhere and the switch is a no-op there. See [`doc/quic-guide.md`](doc/quic-guide.md) |

**Important**: `UVCPP_ENABLE_ZLIB` and `UVCPP_ENABLE_OPENSSL` are NOT auto-enabled
when `UVCPP_BUILD_WEB=ON`. You must opt in explicitly. `UVCPP_ENABLE_NGHTTP2` is
**force-disabled** when `UVCPP_ENABLE_OPENSSL=OFF` (it warns rather than leaving a
configuration that cannot work) — the HTTP/2 module carries the TLS leg as well, so the
whole module is gated on OpenSSL even though the cleartext (`h2c`) leg does not need it.
`UVCPP_ENABLE_QUIC`
is force-disabled the same way for **three** separate missing prerequisites
(`UVCPP_ENABLE_OPENSSL=OFF`, `UVCPP_BUILD_NET=OFF`, or an OpenSSL that has no QUIC API —
anything below 3.2), each with its own warning that says how to fix it. `UVCPP_ENABLE_HTTP3`
is force-disabled the same way for **two** missing prerequisites (`UVCPP_ENABLE_QUIC=OFF`
or `UVCPP_BUILD_WEB=OFF`), also with its own warning. `UVCPP_ENABLE_WSDL`
is force-disabled the same way when `UVCPP_BUILD_WEBAPP=OFF`: the module is built on top of
the framework.

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

The WebSocket client and server, a TLS server, and a complete web app are in
[Documentation](#documentation) — the webapp example is in Features, above.

## Testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUVCPP_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure -C Release
```

Three layers — `tests/unit/`, `tests/functional/` and `tests/expand/` — plus the gate
scripts in `tests/tools/`. The filename is the filter key (`-R "web_"`),
`test_shutdown_func` is a known ~1% flake rather than a hang, and the Windows-only
PageHeap gate (`tests/tools/run_pageheap_gate.py`) — the one gate that has ever caught a
use-after-free — is optional and markedly slower. All of it, including the exit-code
convention where **`3` means "not judged", not "green"**:
[doc/testing-guide.md](doc/testing-guide.md).

## Multi-loop scaling

One event loop can only ever occupy one core. `uvcpp_web_app::set_loops(n)` runs **1
acceptor loop + n−1 worker loops** inside a single process, spreading connection I/O
across the workers:

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

`set_loops()` returns `int`, so it does not join the `set_host(...).set_port(...)` builder
chain, and it has to be called — like route registration — **before `start()`**. `n == 1`
is byte-for-byte the behaviour of never calling it; with `n > 1`, `run(md)` is rejected and
you use `start()` + `join()`.

Which loop a connection lands on is a platform property — kernel fan-out on Linux,
explicit hand-off on Windows — and `is_fanout()` answers which one you got. The readings,
the shutdown rules and the exceptions worth knowing first are in
[§19 of the webapp guide](doc/webapp-guide.md#19-横向扩展多循环set_loops); why it is built
that way is in [doc/multiloop-design.md](doc/multiloop-design.md); sizing and core pinning
are in [doc/benchmark-rig.md](doc/benchmark-rig.md).

**QUIC / HTTP-3 scales too, by a different route** (1.6.6-dev). UDP has no `accept()`, so
`uvcpp_http_server::set_loops(n)` builds **one QUIC endpoint per loop, each holding the
same port with `UV_UDP_REUSEPORT`** — the kernel hashes each datagram's 4-tuple to one of
them. Nothing is ever handed over, and `quic_fanout()` says whether the fan-out really
happened: **the platforms that cannot do it (Windows, macOS, libuv < 1.50) fall back to a
single endpoint** — which is exactly the behaviour of never calling `set_loops()` — and
`is_reuseport()` reports that honestly rather than echoing the request. On this machine
one loop tops out at ~89 k req/s on 0.89 of a core (that one core *is* the ceiling); eight
loops reach ~389–437 k. Full A/B in [doc/benchmark-rig.md](doc/benchmark-rig.md).

## Project structure

```
libuvcpp/
├── src/          # The library, one directory per module
├── tests/        # Unit, functional and expand tests, plus the gate scripts in tools/
├── examples/     # Runnable examples
├── bench/        # The benchmark rig
├── bindings/     # C# binding for the C ABI
├── doc/          # Every guide listed under Documentation
└── cmake/        # CMake config templates
```

The full annotated tree, module by module, is in
[doc/build-guide.md](doc/build-guide.md#repository-layout).

## CI & Contributing

CI runs on every push and PR through GitHub Actions: four workflows, one per platform
(`ci-linux-ubuntu.yml`, `ci-windows-msvc.yml`, `ci-mingw64.yml`, `ci-macos.yml` — the
badges at the top of this page), each holding a feature matrix. Read
[doc/ci-guide.md](doc/ci-guide.md) before changing any of it.

Contributions are welcome — [CONTRIBUTING.md](./CONTRIBUTING.md) has the clone-to-green
path and this repository's conventions. A vulnerability goes to [SECURITY.md](./SECURITY.md),
not to a public issue.

## Changelog

The changelog now lives in **[CHANGELOG.md](./CHANGELOG.md)**: one file, grouped by module,
every entry tagged with the development number it first landed in, with a per-release list at
the end.

It used to sit here, at roughly 920 lines. What stays in this README is the download table,
the feature summary and the CMake options; everything else links out. The per-release
narrative — what a version adds, and what breaks if you swap the binary — is in
[RELEASE.md](./RELEASE.md).

## License

MIT License — see [LICENSE](./LICENSE).

uvcpp — C++ wrapper for libuv
