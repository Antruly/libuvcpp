# libuvcpp

<!-- badges -->
[![GitHub release](https://img.shields.io/badge/release-1.0.0-blue.svg)](./)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![CI](https://img.shields.io/badge/ci-GitHub%20Actions-blue)](.github/workflows/ci.yml)

# libuvcpp

🔧 C++ wrapper for libuv — lightweight, header-friendly wrappers around libuv handles and requests.

- Version: `1.0.0` — Author: `zhuweiye` — License: `MIT`

- **Languages**: [English](./README.md) · [中文](./README.zh.md)

Overview
--------

libuvcpp is designed to make building event-driven C++ applications with libuv faster and more pleasant without sacrificing libuv performance or control. It organizes libuv's primitives into intuitive C++ classes and groups functionality into two main categories:

- handle: object wrappers for libuv handle types (loop, stream, tcp, udp, pipe, timer, etc.)  
- req: request wrappers for one-off operations (fs, write, connect, getaddrinfo, etc.)  
- uvcpp: utility helpers (buffers, threads, rwlocks, version info, etc.)

Design goals
------------

- Object-oriented convenience: expose libuv primitives as classes so developers can use inheritance, RAII and std::function callbacks.  
- Preserve libuv semantics: APIs keep the original libuv flow and performance characteristics.  
- Compatibility: aims to be compatible with libuv v1.x.x series.  
- Developer ergonomics: methods and helpers are grouped into the most relevant class to reduce boilerplate and make code easier to read and maintain.

Why use libuvcpp
----------------

- Rapid C++ development on top of a proven async I/O engine.  
- Minimal overhead — wrapper is thin and designed to be ABI-friendly with libuv structures.  
- Clean, documented headers: core APIs are declared in `src/handle`, `src/req` and `src/uvcpp`.

Installation & Build
--------------------

Prerequisites:
- CMake (>= 3.16 recommended).  
- A C++ compiler; on Windows a Visual Studio environment is required (see below).

Notes:
- The project's CMakeLists is able to automatically download/build libuv when it's not available on the system; you do not need to pre-install libuv.  
- On Windows, install Visual Studio (see examples below).

Build examples:

- Linux / macOS:

```bash
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
# (optional) run tests:
ctest --output-on-failure
```

- Windows (x64, Visual Studio 2022):

```powershell
mkdir build
cmake -S . -B build -A x64 -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -- /m
```

- Windows (x86, Visual Studio 2013):

```powershell
mkdir build
cmake -S . -B build -A Win32 -G "Visual Studio 12 2013" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -- /m
```

Quick start example
-------------------

Simple program that prints the library version and runs the default loop:

```cpp
#include "uvcpp.h"
#include <iostream>

int main() {
  std::cout << "libuvcpp version: " << UVCPP_VERSION_STRING << std::endl;
  uvcpp::uvcpp_loop loop;
  loop.init();
  loop.run();
  return 0;
}
```

Common usage snippets
---------------------

- Timer (run a callback after timeout and optionally repeat):

```cpp
#include "uvcpp.h"
#include <iostream>

int main() {
  uvcpp::uvcpp_loop loop;
  loop.init();

  uvcpp::uvcpp_timer timer(&loop);
  timer.start([](uvcpp::uvcpp_timer* t){
    std::cout << "timer fired\n";
    t->stop(); // single-shot
  }, 1000, 0); // timeout=1000ms, repeat=0

  loop.run();
  return 0;
}
```

- Idle (run when loop is idle):

```cpp
#include "uvcpp.h"
#include <iostream>

int main() {
  uvcpp::uvcpp_loop loop;
  loop.init();

  uvcpp::uvcpp_idle idle(&loop);
  idle.start([](uvcpp::uvcpp_idle* i){
    static int count = 0;
    std::cout << "idle callback: " << ++count << std::endl;
    if (count >= 5) {
      i->stop();
    }
  });

  loop.run();
  return 0;
}
```

Notes & next steps
------------------
- Browse `src/handle` and `src/req` to explore available wrappers and their callback signatures.  
- Use the included `tests/functional` folder as working examples for the most common patterns (timer, pipe, poll, etc.).  
- Contributions welcome — open an issue or PR; keep changes focused and follow the project's style.

License
-------

This project is available under the MIT License — see `LICENSE`.

uvcpp — C++ wrapper for libuv
================================

This repository contains a C++ wrapper around libuv (`uvcpp`) with:
- Core wrapper sources under `src/`
- Unit tests (type/init checks) under `tests/handle` and `tests/req`
- Functional tests (runtime feature tests) under `tests/functional`

Build (example)
-------------

Prerequisites:
- CMake >= 3.20
- A C/C++ compiler toolchain (Visual Studio on Windows, clang/gcc on macOS/Linux)

Basic build (shared lib, tests and functional tests):
```
mkdir build
cd build
cmake .. -DBUILD_SHARED_LIBS=ON -DUVCPP_BUILD_TESTS=ON -DUVCPP_BUILD_FUNCTIONAL=ON
cmake --build . --config Release --parallel
ctest --output-on-failure
```

Notes
-----
- The top-level CMake will try to find a system libuv first. If not found it will fetch libuv using FetchContent and build it.
- Control build options via:
  - `BUILD_SHARED_LIBS` (ON/OFF)
  - `UVCPP_BUILD_TESTS` (ON/OFF)
  - `UVCPP_BUILD_FUNCTIONAL` (ON/OFF)

Version
-------
The project version is defined in `src/uvcpp/uvcpp_version.h`. CMake reads that header at configure-time and prints the version.

Tests
-----
- Unit tests (handle/req) are lightweight checks ensuring types initialize.
- Functional tests under `tests/functional` are runtime behavior checks (tcp/udp/fs/async/prepare/poll/pipe/tty/process).
  These may require network/IPC capabilities in CI and may be disabled there as needed.

License & contribution
----------------------
See repository license and CONTRIBUTING notes (add if you maintain one).


