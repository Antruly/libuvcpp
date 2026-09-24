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
| `UVCPP_ENABLE_WSDL` | `OFF` | pugixml + webapp | webapp off |

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

nghttp2 is linked **PRIVATE** and statically. It appears only in `src/http2/*.cpp` behind a
pimpl, never in a public header, so it adds no DLL dependency to anything you build. zlib, by
contrast, is linked publicly, because a public header includes `<zlib.h>`.

pugixml — the WSDL module's XML backend, pulled in only when `UVCPP_ENABLE_WSDL=ON` — is linked
**PRIVATE** and statically for the same reason: `<pugixml.hpp>` appears in exactly one file,
`src/wsdl/uvcpp_wsdl_pugixml.h`, which is a *private* header (not installed, and filtered out of
the package by `package_release.py`). So it adds no DLL beside `uvcpp.dll`, and the package
carries no pugixml headers — a consumer of the WSDL module never needs pugixml installed.

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

nghttp2 is deliberately absent from that copy list, because it is linked statically.

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
