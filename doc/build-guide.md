# Build Guide

Everything about configuring a build of `libuvcpp`: what each switch does, how the switches
interact, how to lay out several build trees, and what a release build looks like as opposed to
a development one.

**The authoritative list of switch names and their defaults is the CMake Options table in
[`README.md`](../README.md#cmake-options) / [`README.zh.md`](../README.zh.md#cmake-选项)** — that
table is the one a gate checks against `CMakeLists.txt`, so it cannot silently rot. This page
does not repeat it; it explains what the switches *mean* and how they combine.

For the shortest path from a fresh clone to a green test run, see
[`CONTRIBUTING.md`](../CONTRIBUTING.md).

## Toolchain requirements

| | Value | Where it comes from |
|---|---|---|
| CMake | **3.20 or newer** | `cmake_minimum_required(VERSION 3.20)`, declared by the root `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/CMakeLists.txt` and `tests/functional/CMakeLists.txt` — `tests/expand/` declares none and inherits |
| C++ standard | **C++11, required** | `CMAKE_CXX_STANDARD 11` + `CMAKE_CXX_STANDARD_REQUIRED ON` |
| MSVC source charset | `/utf-8` added automatically | `if(MSVC) add_compile_options(/utf-8) endif()` |

That last row deserves a warning. `add_compile_options` is **directory-scoped**: the option
never lands in `INTERFACE_COMPILE_OPTIONS` of the exported target, so a `find_package(uvcpp)`
consumer does not inherit it. This is why every public header is stored as **UTF-8 with BOM**
— MSVC reading a BOM-less UTF-8 file uses the system code page, and a Chinese comment then
eats the following newline, producing a syntax error reported inside `<algorithm>` with only
`C4819` as a clue. If you add a header containing non-ASCII characters, **save it with a BOM**,
or consumers on MSVC will not be able to compile it. `tests/tools/package_release.py` adds a
BOM to any header that needs one when packaging, but that only protects the prebuilt package,
not `find_package` users.

## The switches

### Module switches

`UVCPP_BUILD_NET` (ON), `UVCPP_BUILD_WEB` (OFF), `UVCPP_BUILD_WEBAPP` (OFF),
`UVCPP_BUILD_EXPAND` (OFF), `UVCPP_BUILD_EXAMPLES` (OFF).

Two of these are **force-disabled** rather than left in an unbuildable state, and both do so
loudly:

- `UVCPP_BUILD_WEBAPP=ON` with `UVCPP_BUILD_WEB=OFF` → webapp is turned off with a message.
  The framework is built on top of the web module; there is no configuration where it compiles
  without it.
- `UVCPP_BUILD_EXAMPLES` is independent, but `examples/` is only added near the end of the
  root file, because the compile definitions it needs are directory-scoped and must be
  established first.

The force-disable uses a plain `set()`, which does **not** write to the cache — so
`CMakeCache.txt` will still say `ON` for a module that was switched off. Read the configure
output, not the cache, when you want to know what a tree is actually building.

`UVCPP_BUILD_EXPAND` gates the memory pool, and it defaults **OFF** — a source-tree build that
wants the pool passes `-DUVCPP_BUILD_EXPAND=ON` explicitly. The prebuilt packages go the other
way: they ship with the pool **on** and carry a generated header that supplies that package's
actual value and `#error`s on a mismatch, so a package user passes nothing.

The reason the default matters is that mixing the two allocators is a **silent** heap
corruption rather than a crash: a translation unit that does not see `UVCPP_ENABLE_MEMORY_POOL=1`
calls `std::malloc` where the library calls the pool, and the pool then frees a `malloc`
pointer. The generated header is what stops that from depending on whether someone remembered a
`-D`. For the history — the MinGW-w64 crash that produced this default, and its emutls root
cause — see [`RELEASE.md`](../RELEASE.md).

### Dependency switches

| Switch | Default | Needs | Force-disabled when |
|---|---|---|---|
| `UVCPP_USE_SYSTEM_LIBUV` | `ON` | — | — |
| `UVCPP_BUILD_LIBUV_FROM_SOURCE` | `OFF` | network (or `_local_deps/`) | — |
| `UVCPP_ENABLE_ZLIB` | `OFF` | zlib | — |
| `UVCPP_ENABLE_OPENSSL` | `OFF` | OpenSSL | — |
| `UVCPP_ENABLE_NGHTTP2` | `OFF` | nghttp2 + OpenSSL + web | OpenSSL off, or web off |
| `UVCPP_ENABLE_QUIC` | `OFF` | ngtcp2 + OpenSSL ≥ 3.2 **with the QUIC API** + net | OpenSSL off, net off, or OpenSSL without the QUIC API |
| `UVCPP_ENABLE_WSDL` | `OFF` | pugixml + webapp | webapp off |
| `UVCPP_ENABLE_CAPI` | `OFF` | net + web (nothing else — the C surface is a thin wrapper) | net off, or web off |
| `UVCPP_ENABLE_DB` | `OFF` | any one of libsqlite3 / libmysqlclient / libpq | **all three** backend libraries missing |
| `UVCPP_DB_SQLITE_FROM_SOURCE` | `OFF` | network, or the hash-pinned zip placed by hand | — |

The **target** used for linking is chosen by testing `TARGET uv_a` / `TARGET uv`, not by assuming
a name: libuv 1.36 built both unconditionally, but from 1.51 `uv` is controlled by libuv's own
`LIBUV_BUILD_SHARED`. When `BUILD_SHARED_LIBS=OFF` and a static `uv_a` exists, it is preferred —
that is what lets the released DLL carry libuv statically.

**Turning on a module does not turn on what it needs.** `UVCPP_BUILD_WEB=ON` does not enable
zlib or OpenSSL; you opt in explicitly. `UVCPP_ENABLE_NGHTTP2` is the exception in the other
direction: it is *force-disabled* with a message when OpenSSL is off, because HTTP/2 here is
TLS + ALPN only — there is no cleartext h2 (h2c) support. `UVCPP_ENABLE_WSDL` is
force-disabled the same way when `UVCPP_BUILD_WEBAPP=OFF`: it is built on top of the framework,
so "on but unbuildable" is a worse configuration than "off".

`UVCPP_ENABLE_QUIC` follows the same rule with **three** independent prerequisites: OpenSSL
off, net off, or an OpenSSL that has no QUIC API (anything below 3.2, and the 3.0.13 that
Ubuntu 24.04 ships). Each one gets its own warning that says how to fix it — the third one
also tells you that the fix is `-DOPENSSL_ROOT_DIR=<a 3.2+ prefix>`, because a warning that
only says "not supported" is a warning nobody can act on. There is no cleartext QUIC: ALPN is
a TLS extension, so "no OpenSSL" and "no QUIC" are the same statement.

**The QUIC transport is a net-layer protocol, not a web-layer one** — which is why it is
governed by `UVCPP_BUILD_NET` rather than `UVCPP_BUILD_WEB`, and why this build needed the
OpenSSL discovery block moved out of `if(UVCPP_BUILD_WEB)`. Before that move,
`-DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_BUILD_WEB=OFF` compiled `src/ssl/` but never defined
`UVCPP_SSL_LIBS`, and an empty `UVCPP_SSL_LIBS` expands to a **silent no-op**
`target_link_libraries()` — the symptom was a link failure, not a configure error, and it took
the net layer's own TLS client path down with it. The `quic` CI entries are the only legs that
cover "SSL on + web off" — there are three of them, one per platform file (Ubuntu, macOS,
Windows MSVC).

nghttp2 is linked **PRIVATE** and statically. It appears only in `src/http2/*.cpp` behind a
pimpl, never in a public header, so it adds no DLL dependency to anything you build. zlib, by
contrast, is linked publicly, because a public header includes `<zlib.h>`.

pugixml — the WSDL module's XML backend, pulled in only when `UVCPP_ENABLE_WSDL=ON` — is linked
**PRIVATE** and statically for the same reason: `<pugixml.hpp>` appears in exactly one file,
`src/wsdl/uvcpp_wsdl_pugixml.h`, which is a *private* header (not installed, and filtered out of
the package by `package_release.py`). So it adds no DLL beside `uvcpp.dll`, and the package
carries no pugixml headers — a consumer of the WSDL module never needs pugixml installed.

`UVCPP_ENABLE_CAPI` is the one switch whose prerequisite is another **module** rather than a
library: the exported surface spans net/web/webapp, so it is force-disabled without `net` and
`web`. It adds no dependency of its own — the C layer is a thin wrapper over the C++ classes.

The db module is the only one that **needs a third-party client library**, and it is three
switches deep: `UVCPP_ENABLE_DB` plus `UVCPP_ENABLE_DB_SQLITE` / `_MYSQL` / `_PGSQL` (all ON by
default). Each backend is force-disabled **on its own** with a warning when its library is
missing, and only when **all three** are gone is the module itself turned off — so a machine
with sqlite3 but no libpq still builds a working, one-backend-lighter module, and the configure
log says which ones made it (`db: SQLite 后端开（3.54.0）`). As with every force-disable in this
file, the cache keeps reading `ON`; the message to grep is `Including db module in build`.
Unlike the other dependencies listed above, these client libraries are **not** absorbed into the
library: all three are linked `PRIVATE`, which keeps the consumer's include path and link line
clean, but `PRIVATE` on a **shared** library still writes the dependency into that library's own
`DT_NEEDED` — a source build with the system client libraries gives you a `libuvcpp.so` that
needs `libmysqlclient.so.21` / `libpq.so.5` at load time. That is why the release configurations
turn both of them **explicitly** `OFF` and ship SQLite only, and it is also why the release
workflow's `ldd` assertion names `sqlite3` as well as the two: the point is to make "a client
library leaked into the package" a red line rather than something discovered on a user's machine.
For the static libraries (`uvcpp_a` / `uvcpp_a_s`) the export set carries none of the three, so
their consumers link the client library themselves — spelled out in [`db-guide.md`](db-guide.md).

`UVCPP_DB_SQLITE_FROM_SOURCE` decides *where* the SQLite backend comes from: `find_package`
(system) or a hash-pinned amalgamation compiled into a static `uvcpp_sqlite3` with explicit PIC.
It needs network at configure time, and it does **not** silently fall back to the system library
when the download or the hash check fails — the same discipline as
`UVCPP_BUILD_LIBUV_FROM_SOURCE`, because a fallback that only shows up on a machine that happens
to have the library is a difference you cannot see. Both the reason the release legs need it and
the offline recipe are in [`db-guide.md`](db-guide.md).

### Library shape

`BUILD_SHARED_LIBS` and `UVCPP_FOLLOW_LIBUV_BUILD` decide what `UVCPP_BUILD_SHARED` and
`UVCPP_BUILD_STATIC` default to.

- `UVCPP_FOLLOW_LIBUV_BUILD` (ON by default) makes `uvcpp`'s shape follow libuv's — build libuv
  shared, get `uvcpp` shared.
- `UVCPP_BUILD_SHARED` / `UVCPP_BUILD_STATIC`, when left at their defaults, follow that
  decision; you can also set them explicitly to build one, the other, or both.

Note the reach of `BUILD_SHARED_LIBS`: it governs the `FetchContent` dependencies (llhttp and
friends) but **not** libuv, which decides for itself. That asymmetry is the whole reason the
static-release recipe below needs two switches rather than one.

### Runtime behaviour

`UVCPP_STATIC_RUNTIME` (OFF) links `libgcc`/`libstdc++` statically into the library. It applies
**only to MinGW and to non-Apple Unix**, and is a **no-op on MSVC**, which uses `/MD` and ships
`vcruntime`/`msvcp` alongside the DLL instead. Setting it on an MSVC build does nothing at all;
it is not a way to make an MSVC package self-contained.

`UVCPP_ENABLE_TRY_WRITE` (ON) lets the write path attempt `uv_try_write` before copying the
payload into an internal buffer. `UVCPP_TRY_WRITE_MIN_BYTES` (32768) is the smallest payload
worth trying — below it, the write goes down the ordinary path without even attempting the fast
one. The fast path costs one extra system call per write (libuv does not short-circuit a
zero-length `uv_write` on either platform) and saves a copy of `len` bytes, so on small
messages it is a net loss; the break-even sits around 16 KiB and jitters there, hence the
32 KiB default. This is the only knob in the project that is a `CACHE STRING` rather than an
`option()`, so it takes a value, not `ON`/`OFF`.

`UVCPP_ENABLE_UDP_GSO` (derived: `ON` only when `WIN32` **and** `UVCPP_ENABLE_QUIC=ON`) turns on
the QUIC transport's send-side UDP segmentation offload. On Windows libuv sends one datagram per
`WSASendTo`, so a QUIC connection pays one system call per datagram; this path hands ngtcp2's
whole equal-sized aggregate to the stack in **one** call and lets the kernel split it at
`UDP_SEND_MSG_SIZE`. The splitting happens in the kernel, so the wire still carries exactly the
datagrams it carries today — only the call count changes. Linux needs none of this: libuv's
`uv__udp_sendmsg()` already folds up to 20 requests into one `sendmmsg()`, and this library's
QUIC send shape is what triggers it. The code is compiled out when the switch is off, so on
Linux "enabling" it is a no-op rather than "on but ineffective". See
[`quic-guide.md`](quic-guide.md) §1.2 for the two gates, and for why the segment size can only
come from what each aggregate write reports rather than from a constant.

### Tests

`UVCPP_BUILD_TESTS` (ON) and `UVCPP_BUILD_FUNCTIONAL` (ON). The latter is declared **inside**
`if(UVCPP_BUILD_TESTS)`, so it only exists when tests are on. `tests/expand` is gated on
`UVCPP_BUILD_EXPAND` rather than on a switch of its own.

### Dependency version pins

Not in the README table, because they pin *which revision of a dependency* to fetch rather than
toggling a feature. All are `CACHE STRING`:

| Variable | Default |
|---|---|
| `LLHTTP_VERSION` | `release/v9.2.0` |
| `ZLIB_VERSION` | `v1.3.1` |
| `OPENSSL_VERSION` | `openssl-3.4.0` |
| `NGHTTP2_VERSION` | `v1.70.0` |
| `NGTCP2_VERSION` | `v1.25.0` |
| `PUGIXML_VERSION` | `v1.14` |
| `LIBUV_VERSION` | `v1.51.0` |
| `NLOHMANN_JSON_VERSION` | `v3.11.3` |

They only matter on the `FetchContent` path, i.e. when the dependency is not found on the
system.

## A release-shaped build

To produce a library whose dependencies are **all static** — no extra DLL beside it — you need
two switches together:

```bash
cmake -S . -B build-static \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON
```

`BUILD_SHARED_LIBS=OFF` handles llhttp (otherwise you get `llhttp.dll` next to `uvcpp.dll`);
`UVCPP_BUILD_LIBUV_FROM_SOURCE=ON` handles libuv (without it, libuv is found on the system as
an import library and you get `libuv-1.dll`). Pass only one and you still ship an extra DLL.

**This shape does not reproduce on a normal development machine.** Local work goes through
`CMakePresets.json`, which does not pass these, and if the system happens to have a static
OpenSSL the difference can be invisible until a package is inspected. The release workflow
configures with plain `cmake` and passes the static switches explicitly — see
[`release-process.md`](release-process.md). If you need to check what a package actually
depends on, read imports, do not infer from the cache.

## Presets and build trees

`CMakePresets.json` defines two configure presets:

| Preset | Binary dir | Modules | Usable on a fresh clone? |
|---|---|---|---|
| `default` | `build/` | net only | yes |
| `web` | `build_web/` | net + web | **no** — see below |

```bash
cmake --preset default
cmake --build build --config Release --parallel
```

**`--preset web` fails on a fresh clone.** It sets
`FETCHCONTENT_SOURCE_DIR_{LLHTTP,LIBUV,NGHTTP2}` to `${sourceDir}/_local_deps/*`, and
`_local_deps/` is matched by the `_local_*/` line in `.gitignore` — it is not in the
repository. Populate `_local_deps/` with checkouts of those three projects first, or skip the
preset and pass the flags you want to a plain `cmake -S . -B <tree>`.

**`--preset quic` fails on a fresh clone for a stronger reason**, and it is worth knowing
before you try it: besides the two `_local_deps/` source checkouts it needs a **built** OpenSSL
≥ 3.2 at `_local_deps/openssl-3.5-inst`, which is the expensive part (~5 minutes with a
parallel make). [`doc/quic-guide.md`](quic-guide.md) has the configure-and-build recipe. The
preset exists so that once that prefix is in place the whole thing is one command; it is not a
one-command setup.

Neither preset sets `CMAKE_BUILD_TYPE` or a generator, so on Linux `cmake --preset default`
gives you an unoptimised single-config build.

### One tree per configuration

Beyond the two presets, this project's normal way of working is a hand-made build tree per
feature combination, named `build-<feature>`: `build-webapp`, `build-ssl`, `build-web`,
`build-h2`, `build-nopool`, `build-mingw64`, and so on.

The reason is that CMake cache variables **stick**. Re-configuring an existing tree with a
different switch can leave stale state behind, and a tree that was configured one way is easy
to mistake for another. A fresh tree costs a configure and buys certainty about what is on.
If you would rather reuse a tree, pass `--fresh` (`cmake --fresh -S . -B build-webapp`) so it
is genuinely rebuilt from defaults.

Two gitignore notes. `build-*/` and `build_*/` are both ignored, but `build-ssl-run.sh` is a
*tracked file* in the repository root — which is why the ignore rules are written with a
trailing slash rather than as `build*`.

## Where the artifacts go

| Thing | Path |
|---|---|
| Shared library | `<tree>/Release/uvcpp.dll` (MSVC), `<tree>/libuvcpp.so` (Linux) |
| Import library | `<tree>/Release/uvcpp.lib` (MSVC), `.dll.a` (MinGW), `.so` (Linux) |
| Test executables | `<tree>/tests/functional/Release/test_<name>.exe` |
| Examples | `<tree>/examples/Release/webapp_demo.exe` |

On Windows, the DLLs must be **copied** next to the executables before anything can run. The
`copy_test_dlls` custom target does that: it puts `uvcpp.dll`, `uv.dll` and the rest of the
runtime dependencies (llhttp, zlib, OpenSSL, and the compiler runtime where applicable) into
`tests/{unit,functional,expand}/<config>/` and `examples/`.

It is declared `ALL`, which means a normal `cmake --build` performs it. That is not an
accident: for a period it was not `ALL`, and because `cmake --build .` therefore never ran it,
79 tests failed instantly with `STATUS_DLL_NOT_FOUND` while looking like a mass breakage. If you
build a single target instead of the default one — `cmake --build build --target test_foo` —
the copy does not happen, and your test runs against the **previous** library. Follow such a
build with `cmake --build build --target copy_test_dlls`, or just build the default target.

nghttp2 is deliberately absent from that copy list, because it is linked statically. ngtcp2
(and its OpenSSL binding) are absent for the same reason.

## Platform dependencies

Development-machine packages only. The CI runners' own package lists live in
[`ci-guide.md`](ci-guide.md) and are not repeated here.

| Platform | Command |
|---|---|
| Debian / Ubuntu | `apt install libuv1-dev zlib1g-dev libssl-dev` |
| Fedora / RHEL | `dnf install libuv-devel zlib-devel openssl-devel` |
| macOS | `brew install libuv openssl zlib` |
| MSYS2 / MinGW-w64 | `pacman -S mingw-w64-x86_64-libuv mingw-w64-x86_64-zlib mingw-w64-x86_64-openssl` |
| MSVC | no system package: pass `-DOPENSSL_ROOT_DIR=<prefix>` and `-DOPENSSL_USE_STATIC_LIBS=ON` |

On MSVC, those two `-D`s are not optional if you want the SSL tests to link. `FindOpenSSL`
attaches `crypt32` to the `OpenSSL::*` targets **only** when `OPENSSL_USE_STATIC_LIBS` is true,
and a static `libcrypto` calls `Cert*` directly; without it you get six test executables failing
with `LNK2019` errors reported inside libcrypto. `tests/functional/CMakeLists.txt` links
`crypt32` itself for the SSL cases precisely so that a missing `-D` cannot silently decide
whether the tests compile.

## Repository layout

```
libuvcpp/
├── src/
│   ├── uvcpp/     # Core utilities (buf, thread, version, alloc, ...)
│   ├── handle/    # libuv handle wrappers (loop, tcp, udp, timer, ...)
│   ├── req/       # libuv request wrappers (write, connect, fs, work, ...)
│   ├── expand/    # Memory pool (page heap, span, enterprise allocator)
│   ├── net/       # TCP/UDP client/server, the QUIC transport, socket hand-off
│   ├── web/       # HTTP client/server, WebSocket client/server, frame parser
│   ├── webapp/    # Web app framework (router, middleware, static, upload, WS client, log)
│   ├── http2/     # HTTP/2 session/connection layers, nghttp2 glue, ALPN
│   ├── quic/      # QUIC session/connection layers, ngtcp2 glue
│   ├── http3/     # HTTP/3 session/connection layers, nghttp3 glue
│   ├── db/        # One client per connection over SQLite / MySQL / PostgreSQL
│   ├── ssl/       # SSL/TLS context and connection wrapper
│   └── wsdl/      # WSDL 1.1 model and SOAP runtime
├── tests/
│   ├── unit/      # Unit tests
│   ├── functional/# Functional/integration tests
│   ├── expand/    # Memory pool tests
│   └── tools/     # Gate scripts, mutation drivers, probes, the release packager
├── examples/      # Runnable examples (webapp_demo)
├── bench/         # The benchmark rig
├── bindings/      # C# binding for the C ABI
├── doc/           # Every guide — the index is the Documentation table in README.md
├── cmake/         # CMake config templates
├── .github/       # Workflows, runner-side scripts, issue and PR templates
├── CMakeLists.txt
├── CMakePresets.json
├── CONTRIBUTING.md
├── CHANGELOG.md
├── RELEASE.md
├── README.md / README.zh.md
└── LICENSE
```

Not every header in a module directory is public. `tests/tools/package_release.py` copies every
`.h` under `src/<module>/` **except** a named `PRIVATE_HEADERS` set: the glue layers that name
third-party types a consumer of the package does not have (`uvcpp_h2_nghttp2.h`,
`uvcpp_quic_ngtcp2.h`, `uvcpp_quic_session.h`, `uvcpp_h3_nghttp3.h`, `uvcpp_h3_session.h`,
`uvcpp_wsdl_pugixml.h`) plus two internal contracts (`uvcpp_c_internal.h`,
`uvcpp_db_driver.h`). The same exclusions are written a second time, as `list(FILTER …)` calls
in `CMakeLists.txt`, and the two lists have to be changed together.

## Common failures

| Symptom | Cause | Fix |
|---|---|---|
| Every test fails instantly, `STATUS_DLL_NOT_FOUND` (0xC0000135) | the DLLs were never copied next to the executables | build the default target, or run `--target copy_test_dlls` |
| Tests fail with `0xC0000409` and no output at all | a test executable built against an older class layout | full rebuild — a header changed |
| The version string does not match what you edited | the version header is read at configure time only | re-run `cmake -S . -B <tree>` |
| `test_h2_session_func` is "passing" but nothing about h2 ran | it was never registered — the tree has no nghttp2 | see [`testing-guide.md`](testing-guide.md#tests-are-removed-not-skipped) |
| `llhttp.dll` or `libuv-1.dll` appears beside a supposedly static build | one of the two static switches is missing | see [A release-shaped build](#a-release-shaped-build) |
| `LNK2019` inside libcrypto, six executables at once | `OPENSSL_USE_STATIC_LIBS` not passed on MSVC | add it, or let `tests/functional/CMakeLists.txt` handle it |
| `cc1plus: out of memory` on a large parallel build | too many compiler processes for the machine's commit limit | lower `--parallel` |
| `C1041` with MSVC | two targets sharing one `/Fd` | the project already passes `/FS`; check for a locally overridden PDB path |

## See also

- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — the shortest path to a green build, and the
  repository's conventions
- [`testing-guide.md`](testing-guide.md) — the test layers, the naming conventions, and the
  `tests/tools/` index
- [`release-process.md`](release-process.md) — how a release is cut
- [`ci-guide.md`](ci-guide.md) — the CI job matrix and the runner-side setup
