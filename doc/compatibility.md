# Compatibility and feature profiles

This page is the authoritative matrix for **toolchain / libuv / Windows** support and for
the `UVCPP_FEATURES` profiles. The generated `include/uvcpp/uvcpp_config.h` is the
authoritative answer for *one built artifact*: it tells a consumer exactly which modules and
sub-features are inside that binary. Do not hand-define those macros; the generated header
turns a mismatch into a compile-time `#error`.

## 1. Official Windows 7 binaries (4)

These are the four artifacts the official CI builds. They all target **Windows 7 SP1 or
later**, use **libuv 1.40.0**, **OpenSSL 1.1.1w**, and **zlib**, and expose the same feature
set.

| Artifact | Compiler | Arch | ABI |
|---|---|---|---|
| `win7-x86-msvc2015` | VS2015 Update 3 (v140) | x86 | MSVC v140; consumable by VS2015–VS2022 |
| `win7-x64-msvc2015` | VS2015 Update 3 (v140) | x64 | MSVC v140; consumable by VS2015–VS2022 |
| `win7-x86-mingw` | GCC 8.1 (i686, posix-dwarf) | x86 | GCC 5+ C++11 ABI (new ABI) |
| `win7-x64-mingw` | GCC 8.1 (x86_64) | x64 | GCC 5+ C++11 ABI (new ABI) |

Common configuration:

```text
UVCPP_FEATURES=web
UVCPP_REQUIRE_FEATURES=net;ssl;web-server;web-client;web-ws;web-static;http-compress
UVCPP_WINVER=0x0601
LIBUV_VERSION=v1.40.0
UVCPP_BUILD_LIBUV_FROM_SOURCE=ON
UVCPP_BUILD_TESTS=OFF
BUILD_SHARED_LIBS=OFF          # libuv is linked statically into uvcpp
UVCPP_BUILD_SHARED=ON          # the product is a DLL + import library
UVCPP_BUILD_STATIC=OFF
OPENSSL_ROOT_DIR=<OpenSSL 1.1.1w prefix>
```

- Web = HTTP/1.1 server and client, WebSocket, static-file server, gzip.
- **Not included:** webapp, HTTP/2, QUIC, HTTP/3, database, yyjson, C ABI, compile-time expand.
- MSVC: `/MT` is recommended (`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`), and the OpenSSL
  static libraries must use the same runtime.
- MinGW: `UVCPP_STATIC_RUNTIME=ON` and the `mingw{32,64}-win7.cmake` toolchains add
  `-static -static-libgcc -static-libstdc++` plus `_WIN32_WINNT=0x0601` for third-party C
  sources.
- The generated `uvcpp_config.h` is the contract: a consumer that sees
  `UVCPP_WEB_WS_ENABLE 0` must not include `web/uvcpp_ws_*.h`.

### MinGW toolchain pin (important)

The official MinGW binaries are built with **GCC 8.1 and its matching
mingw-w64 headers**. A modern mingw-w64 cross toolchain (GCC 11/13) does not
compile libuv 1.40 for Windows 7:

```text
libuv:src/win/winapi.h:4730: error: redefinition of 'struct _TCP_INITIAL_RTO_PARAMETERS'
```

libuv 1.40 defines `_TCP_INITIAL_RTO_PARAMETERS` itself when targeting
Windows 7; newer mingw-w64 headers already define the same struct. Raising
`_WIN32_WINNT` to silence it would make the binary require Windows 8, which
defeats the purpose. Use the pinned GCC 8.1 toolchain (or backport a guard
into libuv's `winapi.h`) for the Win7 MinGW artifacts. The Linux-only
`legacy-profiles.yml` workflow does not attempt the MinGW cross build for
this reason.

### Portable build kits (recommended for VS2013)

A kit zip carries the source tree plus a **prebuilt dependency prefix**
(OpenSSL 1.1.1w, libuv 1.33/1.40 with the CVE patch, zlib, llhttp) and a
`build-kit\build.cmd` that knows the target paths. A VS2013 user extracts the
kit and runs `build-kit\build.cmd vs2013-x86`; no manual OpenSSL/libuv build
is needed. The MSVC compiler itself is not in the kit (it is not
redistributable), so the matching Visual Studio must be installed.

Kits are produced by the AppVeyor jobs (`.appveyor.yml`) and assembled with
`packaging/make_build_kit.py`; see [`build-kit/README.md`](../build-kit/README.md)
for the layout and the maintainer recipe.

### Building the MinGW artifacts natively

With the pinned GCC 8.1 toolchain on `PATH` (32-bit `mingw32\bin` or 64-bit
`mingw64\bin`), use CMake's native MinGW Makefiles generator rather than the
Linux cross toolchain files:

```bat
set PATH=C:\mingw32\bin;%PATH%
cmake -S . -B build-mingw32 -G "MinGW Makefiles" ^
  -DUVCPP_FEATURES=web ^
  -DUVCPP_REQUIRE_FEATURES=net;ssl;web-server;web-client;web-ws;web-static;http-compress ^
  -DUVCPP_WINVER=0x0601 ^
  -DUVCPP_BUILD_TESTS=OFF ^
  -DUVCPP_STATIC_RUNTIME=ON ^
  -DBUILD_SHARED_LIBS=OFF -DUVCPP_BUILD_SHARED=ON -DUVCPP_BUILD_STATIC=OFF ^
  -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON -DLIBUV_VERSION=v1.40.0 ^
  -DOPENSSL_ROOT_DIR=C:/deps/openssl-1.1.1w-mingw32 ^
  -DOPENSSL_USE_STATIC_LIBS=ON
cmake --build build-mingw32 --parallel 4
```

Build OpenSSL 1.1.1w with `perl Configure mingw no-shared no-asm` (32-bit) or
`mingw64` (64-bit), using the same GCC 8.1 toolchain. The x64 artifact uses the
`mingw64` target and `mingw64\bin`.

## 2. Source-build profiles

| Level | Toolchain | libuv | Modules | Notes |
|---|---|---|---|---|
| **L0** | VS2013 Update 5, x86 | 1.33.0 | net + ssl | lowest supported source profile |
| **L1** | VS2013 Update 5, x86 | 1.33.0 | net + ssl + web-lite (HTTP server/client) | no WS/static/gzip/webapp |
| **L2** | VS2015 Update 3+, GCC 5.3+ | 1.40.0 for Win7 | net + ssl + web (+ webapp optional) | the official binary configuration |
| **L3** | VS2019 16.5+, GCC 11+ | 1.51.0 | modern line; HTTP/2 / QUIC / HTTP/3 available | Windows 10+ for QUIC |

- **GCC 4.8 is not supported**, from source or as a binary. GCC 5 changed the libstdc++
  `std::string` ABI; a library built with 4.8 cannot be linked into a GCC 5+ consumer without
  `-D_GLIBCXX_USE_CXX11_ABI=0` for the entire program. The minimum GCC is **5.3**; official
  MinGW binaries use **GCC 8.1**.
- **VS2013 has no official binary.** MSVC v120 and v140 are not binary compatible for C++.
  A VS2013 application must build the source tree itself; a VS2015-built C++ DLL is not a
  substitute. (A C-ABI DLL built with `UVCPP_ENABLE_CAPI=ON` is the only possible bridge, and
  it is not part of the default Win7 feature set.)
- **`UVCPP_JSON_FIELDS` / `uvcpp_from_json<T>`** needs a conforming MSVC preprocessor:
  `_MSC_VER >= 1925` (VS2019 16.5) or clang-cl. The webapp *library* builds on VS2015, but a
  consumer translation unit that expands the reflection macro does not.

## 3. libuv version and Windows support

| libuv | Minimum Windows | Notes |
|---|---|---|
| 1.33.0 | Windows 7 | last version VS2013 can build; used by L0/L1 only |
| 1.40.0 | Windows 7 | last libuv whose `SUPPORTED_PLATFORMS.md` says Windows 7; used by the official Win7 binaries |
| 1.41.0 – 1.49.0 | Windows 8 | |
| 1.50.0 – 1.51.0 | Windows 10 | |

The public headers no longer test `UV_VERSION_MINOR`. They test the capability macros
derived from the exact libuv this build used:

```text
UVCPP_LIBUV_HEX              /* e.g. 0x012800 for 1.40.0 */
UVCPP_HAVE_FS_MKSTEMP        /* 1.34 */
UVCPP_HAVE_FS_LUTIME         /* 1.36 */
UVCPP_HAVE_FS_SYSTEM_ERROR   /* 1.38 */
UVCPP_HAVE_UDP_RECVMMSG      /* 1.39 */
UVCPP_HAVE_TRY_WRITE2        /* 1.42 */
UVCPP_HAVE_AVAILABLE_PARALLELISM /* 1.44; the fallback is used when 0 */
UVCPP_HAVE_METRICS           /* 1.45 */
UVCPP_HAVE_OS_GROUP          /* 1.45 */
UVCPP_HAVE_PASSWD2           /* 1.45 */
UVCPP_HAVE_TCP_REUSEPORT     /* 1.49 */
UVCPP_HAVE_UDP_REUSEPORT     /* 1.50 */
```

This is what keeps a DLL and its headers in agreement even when the consumer has a different
`uv.h` on its include path.

**Security note:** libuv 1.40 is from 2021 and is affected by **CVE-2024-24806**
(`uv_getaddrinfo` hostname truncation, fixed in 1.48.0). The Win7 line must carry a backport
of that fix. Review the 1.41–1.51 changelog for other security-relevant fixes before shipping.

## 4. Feature requirements

| Feature | Minimum compiler | Minimum libuv | Other | Windows |
|---|---|---|---|---|
| core / net | MSVC 1800, GCC 5.3 | 1.33.0 | — | Win7 |
| ssl | MSVC 1800, GCC 5.3 | 1.33.0 | OpenSSL 1.1.1 | Win7 |
| HTTP server | MSVC 1800, GCC 5.3 | 1.33.0 | llhttp | Win7 |
| HTTP client | MSVC 1800, GCC 5.3 | 1.33.0 | llhttp | Win7 |
| static file server | MSVC 1800, GCC 5.3 | 1.33.0 | HTTP server | Win7 |
| WebSocket | MSVC 1900, GCC 5.3 | 1.33.0 | non-trivial TLS | Win7 |
| gzip / brotli | MSVC 1900, GCC 5.3 | 1.33.0 | zlib / brotli | Win7 |
| webapp | MSVC 1900, GCC 5.3 | 1.33.0 | nlohmann/json | Win7 |
| `UVCPP_JSON_FIELDS` | MSVC 1925, GCC 5.3 | 1.33.0 | conforming preprocessor | Win7 |
| HTTP/2 | MSVC 1900, GCC 5.3 | 1.33.0 | nghttp2 ≤ 1.66 on MSVC | Win7 |
| QUIC | MSVC 1920, GCC 5.3 | 1.34.0 | OpenSSL ≥ 3.2, ngtcp2 | Win10 |
| HTTP/3 | MSVC 1920, GCC 5.3 | 1.34.0 | QUIC + nghttp3 | Win10 |
| yyjson backend | MSVC 1900, GCC 5.3 | 1.33.0 | — | Win7 |
| database | MSVC 1900, GCC 5.3 | — | client libraries | Win7 |
| expand / C ABI | MSVC 1900, GCC 5.3 | 1.33.0 | — | Win7 |

The table is encoded in `cmake/uvcpp_compat.cmake`. A feature that does not meet its
requirement is switched **off automatically** with a `uvcpp: disabling ...` line; the build
then either succeeds with the remaining features or fails at `UVCPP_REQUIRE_FEATURES`.

## 5. Profiles and macros

`UVCPP_FEATURES` is a convenience that sets the individual options:

| Profile | Contents |
|---|---|
| `net` | net only |
| `net-ssl` | net + OpenSSL |
| `web-lite` | net + ssl + HTTP server/client (no WS, static, gzip, webapp) |
| `web` | net + ssl + HTTP server/client + WS + static + gzip |
| `full` | `web` + webapp |

Leaving it empty keeps the historical behaviour: each `UVCPP_*` option is used as written.

Examples:

```bash
# VS2013 source build, net + ssl only
cmake -S . -B build-vs2013-net -G "Visual Studio 12 2013" -A Win32 \
  -DUVCPP_FEATURES=net-ssl \
  -DUVCPP_REQUIRE_FEATURES=net;ssl \
  -DUVCPP_WINVER=0x0601 \
  -DUVCPP_BUILD_TESTS=OFF \
  -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON -DLIBUV_VERSION=v1.33.0 \
  -DOPENSSL_ROOT_DIR=D:/deps/openssl-1.1.1w-x86

# VS2013 web-lite
cmake -S . -B build-vs2013-web -G "Visual Studio 12 2013" -A Win32 \
  -DUVCPP_FEATURES=web-lite \
  -DUVCPP_REQUIRE_FEATURES=net;ssl;web-server;web-client \
  -DUVCPP_WINVER=0x0601 \
  -DUVCPP_BUILD_TESTS=OFF \
  -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON -DLIBUV_VERSION=v1.33.0 \
  -DOPENSSL_ROOT_DIR=D:/deps/openssl-1.1.1w-x86

# Official Win7 web profile (VS2015 x86)
cmake --preset win7-vs2015-x86 -DOPENSSL_ROOT_DIR=D:/deps/openssl-1.1.1w-x86

# Official Win7 web profile (MinGW32 cross-compile on Linux)
cmake --preset win7-mingw32 -DOPENSSL_ROOT_DIR=/opt/openssl-1.1.1w-mingw32
```

`UVCPP_REQUIRE_FEATURES` is a semicolon-separated list. If any listed feature is not enabled
after all dependency discovery, configure fails. Known names:

```text
net, ssl, web, web-server, web-client, web-ws, web-static, http-compress,
webapp, http2, quic, http3, yyjson, capi, db, redis
```

The generated `uvcpp_config.h` contains the final 0/1 values:

```text
#define UVCPP_NET_ENABLE            1
#define UVCPP_WEB_ENABLE            1
#define UVCPP_WEB_SERVER_ENABLE     1
#define UVCPP_WEB_CLIENT_ENABLE     1
#define UVCPP_WEB_WS_ENABLE         1
#define UVCPP_WEB_STATIC_ENABLE     1
#define UVCPP_HTTP_COMPRESS_ENABLE  1
#define UVCPP_WEBAPP_ENABLE         0
#define UVCPP_NGHTTP2_ENABLE        0
#define UVCPP_QUIC_ENABLE           0
#define UVCPP_LIBUV_HEX             0x012800
#define UVCPP_TARGET_WINVER         0x0601
```

**Do not define these yourself.** For a prebuilt package the header compares your definition
with the build's value and emits `#error` on a mismatch. Feature selection for a source build
is done through CMake, not by editing the generated header.

## 6. Automatic disabling

`cmake/uvcpp_compat.cmake` applies these rules before the module sources are compiled:

- **MSVC 2013** disables webapp, WebSocket, gzip/brotli, HTTP/2, QUIC, HTTP/3, yyjson, WSDL,
  database and the expand module. The net+ssl and web-lite profiles remain.
- **GCC < 5.3** is a hard configure error (old libstdc++ ABI).
- **libuv < 1.34** disables QUIC (the QUIC code uses `uv_sleep`).
- **Web off** disables all web sub-features, webapp, HTTP/2 and HTTP/3.
- **HTTP server off** disables the static server.
- **OpenSSL off** disables QUIC; **QUIC off** disables HTTP/3.
- **webapp off** disables WSDL.

Every automatic disable appends a `name: reason` line to the end-of-configure summary.

## 7. ABI rules

- **MSVC**: v140 (VS2015), v141 (VS2017), v142 (VS2019) and v143 (VS2022) are mutually
  binary compatible. v120 (VS2013) is **not** compatible with v140+.
- **MinGW**: GCC 5 introduced the C++11 ABI (`_GLIBCXX_USE_CXX11_ABI=1`). The official
  MinGW binaries are built with GCC 8.1 and the new ABI. GCC 4.8/4.9 consumers must define
  `-D_GLIBCXX_USE_CXX11_ABI=0` **and** the library must have been built that way; this tree
  does not ship such a binary.
- **libuv headers**: the Win7 binaries statically link libuv 1.40.0. Consumers must compile
  against the matching libuv 1.40 headers; do not mix in a 1.51 `uv.h`. The generated
  `UVCPP_LIBUV_HEX` / `UVCPP_HAVE_*` macros describe the build's surface, but the libuv
  handle layout itself still belongs to 1.40.

## 8. Windows 7 validation

- Build with `_WIN32_WINNT=0x0601` and `WINVER=0x0601`; the CMake target exports both.
- Scan the DLL import table for Win8+/Win10 APIs (`GetSystemTimePreciseAsFileTime`,
  `SetThreadDescription`, `GetDpiForWindow`, …) before shipping.
- Run the functional smoke tests (HTTP, HTTPS, WebSocket, static, gzip) on a **Windows 7 SP1
  x86/x64 VM**. Wine is useful for CI smoke tests but is not a substitute for Win7.
- 32-bit processes have a smaller address space; test with realistic concurrency and
  `--large-address-aware` (MinGW) / `/LARGEADDRESSAWARE` (MSVC).

### Measured on Windows 7 SP1 x86 (2026-10-24)

A real Windows 7 SP1 x86 host (6.1.7601) ran the kit prebuilts against the functional
suites. No Win8+/Win10 API is imported — loading the DLL on Win7 is the import-table
check — and the suites that touch the network pass:

| Artifact | Consumer | Functional suites |
|---|---|---|
| `vs2015-x86` kit `prebuilt/bin/uvcpp.dll` (v140, `/MT`) | `version_string()` → `1.8.2` | compress (12), http_server (28), ssl_client, ssl_server, static_server, ws — `ALL PASS` |
| `mingw32` kit `prebuilt/bin/libuvcpp.dll` (GCC 8.1) | `1.8.2` | compress (13), http_server (28) — `ALL PASS` |
| `mingw64` kit `prebuilt/bin/libuvcpp.dll` (GCC 8.1) | `1.8.2` | http_server — `ALL PASS` (run under Wine; no 64-bit Win7 host) |

That run also produced the `thread_local` rule now written into
`src/web/uvcpp_http_compress.cpp`: on 32-bit MinGW a `thread_local` object **with a
non-trivial destructor** inside the DLL is destroyed through a stale pointer when the
owning thread exits (`deflate_slot::~deflate_slot()` faulted, and the joining thread
waited forever). Thread-local caches in this library must stay trivially destructible;
the full analysis is in `src/expand/uvcpp_page_heap.cpp:558-576`.

## 9. Known limitations and security notes

- **Windows 7 is end-of-life** (2020-01); this line is a compatibility product, not a
  security-supported platform.
- **OpenSSL 1.1.1 is end-of-life** (2023-09). The Win7 line uses 1.1.1w. If the service is
  exposed to untrusted networks, terminate TLS in a maintained proxy or plan an OpenSSL 3.x
  migration.
- **libuv 1.40** is affected by CVE-2024-24806. FetchContent builds of libuv 1.33–1.47
  apply `cmake/patches/libuv-idna-cve-2024-24806.patch` automatically (the Win7 presets
  use FetchContent); a system libuv older than 1.48 must be patched by whoever built it.
  The 2021-era code should also be reviewed against later security fixes.
- **VS2015 CI**: GitHub-hosted runners do not ship the v140 toolset. `.appveyor.yml`
  builds the two VS2015 Win7 artifacts on AppVeyor's "Visual Studio 2015" image; connect
  the repository to AppVeyor to run it.
- **HTTP/2 is not part of the Win7 line.** It would require pinning nghttp2 ≤ 1.66 for MSVC
  (1.68+ uses C99 designated initializers that MSVC 2015 cannot compile).
- **QUIC/HTTP/3 are not part of the Win7 line.** They need a C11 toolchain, OpenSSL ≥ 3.2 and
  libuv ≥ 1.34; MSVC has no C11 mode before VS2019 16.7.
- **webapp on VS2013** is not supported (nlohmann/json's documented minimum is VS2015).
- **Reflection macros** (`UVCPP_JSON_FIELDS`) need VS2019 16.5+ or clang-cl, even when the
  rest of the webapp library builds with VS2015.
