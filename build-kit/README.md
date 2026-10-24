# Portable build kits

A build kit is a zip that contains **this source tree plus a prebuilt dependency
prefix**. It removes the two things that make old-toolchain builds painful:

- OpenSSL 1.1.1w, libuv 1.33/1.40 (with the CVE-2024-24806 backport), zlib and
  llhttp are already built for the kit's compiler; and
- `build-kit\build.cmd` knows the paths, the profile and the Win7 target flags.

The user only needs the matching compiler. For MinGW kits the pinned GCC 8.1
toolchain itself can be carried under `deps\mingw32` / `deps\mingw64`.

## Kit zip layout

```text
libuvcpp-<version>-<target>/
  CMakeLists.txt
  src/ cmake/ tests/ doc/ ...
  build-kit/
    build.cmd
    build.ps1
    collect-deps.ps1        # maintainer/CI only
  deps/
    libuv/include/uv.h
    libuv/lib/uv_a.lib       (or libuv_a.a)
    openssl/include/...  openssl/lib/...
    zlib/...
    llhttp/                  (source tree used by CMake)
    cmake/                   (optional portable CMake)
    mingw32/ or mingw64/     (MinGW kits only)
  KIT-README.txt
```

## Using a kit

```bat
build-kit\build.cmd vs2015-x86
build-kit\build.cmd vs2015-x64
build-kit\build.cmd vs2013-x86 -Profile web-lite
build-kit\build.cmd mingw32
build-kit\build.cmd mingw64
```

Output goes to `out\<target>\` (DLL + import library + headers + CMake config).

| Target | Compiler required | Default profile |
|---|---|---|
| `vs2013-x86` | Visual Studio 2013 (v120), x86 | `web-lite` |
| `vs2015-x86` | Visual Studio 2015 Update 3 (v140), x86 | `web` |
| `vs2015-x64` | Visual Studio 2015 Update 3 (v140), x64 | `web` |
| `mingw32` | GCC 8.1 carried in `deps\mingw32` | `web` |
| `mingw64` | GCC 8.1 carried in `deps\mingw64` | `web` |

`-Profile` accepts `net`, `net-ssl`, `web-lite`, `web`, `full`. The official
Win7 `web` profile is HTTP server/client + WebSocket + static files + gzip.
`web-lite` is the VS2013-friendly subset (HTTP server/client only).

The MSVC compiler is **not** in the kit: Visual Studio is not redistributable
that way. The kit only removes the dependency-building work.

MSVC kits build with the static CRT (`/MT`) so uvcpp and the static OpenSSL /
zlib / libuv agree; a consuming EXE should use `/MT` as well (or use the C ABI
from a different runtime).

## Producing a kit (maintainer / CI)

A kit is assembled from a CMake tree that already built the dependencies:

```powershell
# 1. Configure/build once with FetchContent (see .appveyor.yml)
#    The build tree now has _deps/libuv-src, _deps/zlib-src, _deps/llhttp-src.

# 2. Copy the dependency prefix we want to ship
powershell -File build-kit\collect-deps.ps1 `
  -Target vs2015-x86 `
  -BuildDir build-win7-vs2015-x86 `
  -DepsRoot kit-deps `
  -OpenSSLRoot deps\openssl-x86 `
  -CmakeRoot deps\cmake

# 3. Zip the source + deps + build-kit
python packaging\make_build_kit.py `
  --deps kit-deps --target vs2015-x86 --out dist\libuvcpp-kit-vs2015-x86.zip
```

The packager ships what a clone of the repository contains: in a git work tree
it copies the tracked files plus untracked files that are not ignored, and it
skips build trees (`build*`, `*-build*`), dist dirs, `prebuilt/`, `docs/` and
test residue. Run it from a clean checkout (or at least after `git status`
shows nothing you would not ship) — otherwise a local `.exe`, a `consumer.cpp`
or a stray test directory rides along in the zip.

`.appveyor.yml` performs the same steps for the VS2013 and VS2015 jobs and
uploads the kit zips. It passes `--prebuilt stage`, so the kit also contains a
ready-to-use build under `prebuilt/` (consume it directly, or rebuild with
`build-kit\build.cmd`). The VS2013 kit uses the same recipe with the
`vs2013-web-lite` preset, libuv 1.33.0 and `VC-WIN32`.

### VS2015 and a newer Windows SDK

VS2015's CRT/STL predates `_mm_loadu_si64`; a post-2015 Windows SDK's
`<wchar.h>` uses it, so even `#include <cstdio>` fails with `C3861` on that
combination. `build-kit\build.cmd` force-includes
`build-kit\msvc2015-sdk-compat.h` through the `CL` environment variable (not
through `CMAKE_CXX_FLAGS`, which would replace CMake's `/EHsc /GR /W3` defaults
and leave the DLL built without unwind semantics). A consumer of the kit needs
the same switch:

```bat
cl /EHsc /MT /FI"%KIT%\build-kit\msvc2015-sdk-compat.h" ...
```

AppVeyor also builds the two MinGW kits with the pinned GCC 8.1 toolchains:
it downloads `i686-8.1.0-release-posix-dwarf-rt_v6-rev0.7z` /
`x86_64-8.1.0-release-posix-seh-rt_v6-rev0.7z` into `deps\mingw32` /
`deps\mingw64`, builds OpenSSL and uvcpp with that toolchain, and copies the
toolchain into the kit so the user does not need to install MinGW.

To build a MinGW kit by hand, put the toolchain under `deps\mingw32` /
`deps\mingw64`, build the project with `-G "MinGW Makefiles"`, then run
`collect-deps.ps1 -MingwRoot deps\mingw32 ...`.

## Why this supports VS2013

The VS2013 problem is not CMake or the C++ source; it is that nobody wants to
build a 2013-era OpenSSL 1.1.1w and libuv 1.33 by hand. The kit carries those
prebuilt for v120, plus the VS2013 compatibility layer in
`src/uvcpp/uvcpp_compat.h`. A VS2013 user extracts the zip, runs
`build-kit\build.cmd vs2013-x86`, and gets the net+ssl or web-lite library.

See [`doc/compatibility.md`](../doc/compatibility.md) for the exact
feature/toolchain matrix and the MinGW GCC 8.1 pin.
