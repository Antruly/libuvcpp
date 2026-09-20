# Contributing to libuvcpp

This page takes you from a fresh clone to a green test run, and records the conventions this
repository actually follows. It is deliberately short on philosophy — the *why* behind each
sharp edge lives in the linked pages, not here.

| I want to… | Read |
|---|---|
| get it building and testing | this page |
| understand a build switch, or set up another configuration | [`doc/build-guide.md`](doc/build-guide.md) |
| add or run a test, or find out what a script in `tests/tools/` does | [`doc/testing-guide.md`](doc/testing-guide.md) |
| cut a release | [`doc/release-process.md`](doc/release-process.md) |
| fix or extend CI | [`doc/ci-guide.md`](doc/ci-guide.md) |
| measure throughput on this machine | [`doc/benchmark-rig.md`](doc/benchmark-rig.md) |
| use the library, not develop it | [`README.md`](README.md) / [`README.zh.md`](README.zh.md) |

## Prerequisites

| | |
|---|---|
| CMake | **3.20 or newer** (`cmake_minimum_required(VERSION 3.20)`) |
| Compiler | anything with C++11: MSVC 2019+, GCC, Clang, MinGW-w64 |
| libuv | a system package, or fetched from source with `UVCPP_BUILD_LIBUV_FROM_SOURCE=ON` |

Optional and **all off by default**: OpenSSL (`UVCPP_ENABLE_OPENSSL`), zlib
(`UVCPP_ENABLE_ZLIB`), nghttp2 (`UVCPP_ENABLE_NGHTTP2`, which requires OpenSSL), and the
web-app framework (`UVCPP_BUILD_WEBAPP`, which pulls llhttp and nlohmann/json through
`FetchContent`). Nothing is auto-enabled by turning on a module that uses it — see
[`doc/build-guide.md`](doc/build-guide.md#the-switches) for the full list and the coupling rules.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Three things bite on the first run.

**`--config Release` is not decoration.** With the Visual Studio generator — the default on
Windows — `cmake --build` builds **Debug** unless you say otherwise, while the `ctest` above
runs `-C Release`. So you can build one thing and test another without noticing. The
`CMAKE_BUILD_TYPE` value sitting in `CMakeCache.txt` does not influence a multi-config
generator at all; if you are unsure which one you got, read the output path in the build log.

**Build the default target, not a single test.** `cmake --build build --target test_foo`
recompiles the library but does **not** re-run `copy_test_dlls`, so the test directory keeps
the *previous* `uvcpp.dll` and your new assertions run against the old library — a passing run
that proves nothing. If you must build one target, follow it with
`cmake --build build --target copy_test_dlls`.

**After changing a header, do a full rebuild.** Adding a private member to a class held by
value changes the object layout; any test executable built against the old layout dies with
`0xC0000409` *before* `main`, printing nothing at all.

On Windows, `copy_test_dlls` is what puts `uvcpp.dll`, `uv.dll` and the rest of the runtime
dependencies into `tests/{unit,functional,expand}/<config>/` and `examples/`. It is declared
`ALL` precisely so a normal `cmake --build` performs it; it has not always been, and while it
was not, 79 tests failed instantly with `STATUS_DLL_NOT_FOUND`. Do not "optimise" it away.

### Presets

`CMakePresets.json` defines exactly two configure presets:

```bash
cmake --preset default     # -> build/      net module only
cmake --preset web         # -> build_web/  net + web
```

**`--preset web` does not work on a fresh clone.** It points
`FETCHCONTENT_SOURCE_DIR_{LLHTTP,LIBUV,NGHTTP2}` at `${sourceDir}/_local_deps/*`, and
`_local_deps/` is matched by the `_local_*/` rule in `.gitignore` — it is not in the
repository. Either populate `_local_deps/` with checkouts of those three projects, or skip the
preset and pass the flags you want to a plain `cmake -S . -B <tree>`.

Most configurations here are **not** presets. They are hand-made build trees — `build-webapp`,
`build-ssl`, `build-nopool`, `build-h2` and a dozen more — one tree per feature combination.
See [`doc/build-guide.md`](doc/build-guide.md#presets-and-build-trees).

### Re-configure after touching the version header

`cmake --build` rebuilds **nothing** when you change `src/uvcpp/uvcpp_version.h`. The library
does not include it anywhere; only `tests/unit/uvcpp_unit.cpp` reads it, and the value is
baked in at configure time. Re-run `cmake -S . -B <tree>` after changing it — and check the
version string in the generated `uvcppConfig.cmake`, not the timestamps of the artifacts.

## Layout

```
src/
  uvcpp/       core utilities — buf, thread, version, alloc, env, fs helpers
  handle/      libuv handle wrappers — loop, tcp, udp, timer, signal, ...
  req/         libuv request wrappers — write, connect, fs, work, getaddrinfo, ...
  expand/      memory pool — page heap, span, thread cache, enterprise allocator
  net/         TCP/UDP client and server
  web/         HTTP client/server, WebSocket client/server, frame parser
  http2/       HTTP/2 session and connection layers, nghttp2 glue, ALPN
  webapp/      web app framework — router, middleware, static, upload, log
  ssl/         SSL/TLS context and connection wrapper
  uvcpp.h      the aggregate public header
tests/
  unit/        three consolidated unit-test executables
  functional/  one executable per .cpp (see doc/testing-guide.md)
  expand/      memory pool tests
  tools/       Python tooling — see doc/testing-guide.md for the index
examples/      runnable examples (webapp_demo)
doc/           project documentation
cmake/         CMake package config templates
```

**There is no `include/` directory.** Public headers live beside their implementation in
`src/<module>/` and are flattened into `include/<module>/` at install time by a `GLOB` in
`CMakeLists.txt`. When you add a header, it is picked up automatically — but so is anything
else you drop in that tree, so keep stray files out of `src/`.

Note that `tests/CMakeLists.txt` adds **only** `unit`. `tests/functional` and `tests/expand`
are added from the root `CMakeLists.txt`, inside `if(UVCPP_BUILD_FUNCTIONAL)` and
`if(UVCPP_BUILD_EXPAND)` respectively. Adding a test directory means touching the root file.

## Conventions

### Commit messages

Conventional Commits with a **Chinese** description and body:

```
fix(web): 读边界不再吃掉消息 —— keep-alive 重置改判「消息尾」
docs(benchmark): 修 4.73× → 5.09×、末处 4.62 KB → 4.62 KiB，并把单位口径那条查实
chore(version): 1.1.33 -> 1.1.34（变体表字节账改成派生量之后升一档开发版）
```

Common types are `feat`, `fix`, `docs`, `chore`, `test`, `ci` and `release`. The body explains
what was wrong and why the fix is the fix; write down the reasoning you would otherwise lose.
Mention the evidence you actually gathered — a command, a run id, a count — rather than an
assertion that you checked.

### Versioning

The single source of truth is `src/uvcpp/uvcpp_version.h`:
`UVCPP_VERSION_MAJOR`/`MINOR`/`PATCH`, plus `UVCPP_VERSION_IS_RELEASE` and
`UVCPP_VERSION_SUFFIX` from which `UVCPP_RELEASE_TAG` is derived.

- Each feature push bumps the **patch** level and keeps `UVCPP_VERSION_IS_RELEASE` at `0`, so
  the tree reports e.g. `1.2.1-dev`.
- The two READMEs each carry the same version string twice (a shields badge and a bold
  Version / 版本 line), so four places move together with the header.
  `tests/tools/check_doc_versions.py` enforces this on every push and fails the build if they
  drift apart, so you cannot forget one of them.
- Do not bump the major or minor level until a release is actually being cut; that happens in
  its own commit, together with flipping `IS_RELEASE` to `1`.

### Code style

There is no `.clang-format`, no `.editorconfig`, no `.clang-tidy` and no git hooks — follow
the surrounding code. The house style is 2-space indentation, `snake_case` for types and
functions with a `uvcpp_` prefix on public names, and dense explanatory comments in Chinese
that record *why* a thing is the way it is (including what was tried and rejected). Comments
are cheap; rediscovering a past failure is not.

Save new source files as **UTF-8 with BOM**. MSVC reads a BOM-less UTF-8 file using the system
code page, and a Chinese comment then swallows the following newline — a reported failure
inside `<algorithm>`, with `C4819` as the only clue. `add_compile_options(/utf-8)` covers
in-repo builds but is directory-scoped and does not reach `find_package` consumers.

### Where new documentation goes

`doc/`, singular, as `.md`. **Not `docs/` and not `.txt`** — both are in `.gitignore`, so a
file placed there is silently untracked and never committed. `docs/` also holds local-only
reference material (RFC texts, an early development plan) that is deliberately kept out of the
repository; do not reference those paths from tracked documentation, because they will not
resolve for anyone who clones.

Two gates run on documentation changes, both from CI as well as by hand:

| Command | Checks |
|---|---|
| `python tests/tools/check_docs.py` | The CMake option tables in both READMEs match the options the build actually defines, every relative link and repo path resolves, and no document is orphaned. |
| `python tests/tools/check_doc_snippets.py --pkg <package dir>` | Every ```` ```cpp ```` block in a tracked document actually **compiles** against the packaged headers. |
| `python tests/tools/check_doc_lines.py` | Every `file:line` reference in a tracked document still resolves, still lands on a non-blank line, and still points at the same content it did when the lockfile was written. |

Both use the same three exit codes, which are worth reading carefully: `0` every criterion ran
and passed, `1` a criterion ran and is **red**, and `3` a criterion's **premise was missing, so
it never ran**. `3` is not a failure — a red criterion also outranks an unjudged one — so read
the summary block rather than the exit code. See
[`testing-guide.md`](doc/testing-guide.md#gate-exit-codes-3-is-not-a-failure) for why the
distinction is worth a convention.

User-facing prose is **one page per module**, and that is where new module prose belongs —
not in the READMEs, which carry the class tables. These pages are also what the snippet gate
mostly operates on, so a new module page is a page CI will compile:

| Page | Module |
|---|---|
| [`doc/lowlevel-guide.md`](doc/lowlevel-guide.md) | the event loop, handles and requests (the foundation) |
| [`doc/net-guide.md`](doc/net-guide.md) | TCP/UDP clients and servers |
| [`doc/web-http-guide.md`](doc/web-http-guide.md) | the HTTP half of the web layer |
| [`doc/web-ws-guide.md`](doc/web-ws-guide.md) | the WebSocket half of the web layer |
| [`doc/webapp-guide.md`](doc/webapp-guide.md) | the web app framework |
| [`doc/webapp-support-guide.md`](doc/webapp-support-guide.md) | the types under webapp the framework guide does not cover |
| [`doc/ssl-guide.md`](doc/ssl-guide.md) | TLS context and per-connection wrapper |
| [`doc/http2-guide.md`](doc/http2-guide.md) | the low-level HTTP/2 session and connection layers |
| [`doc/expand-guide.md`](doc/expand-guide.md) | memory pool, page heap and span |

`doc/<module>/` subdirectories do not exist and should not be created: both gates scan
`*.md` at the root plus `doc/*.md` **non-recursively**, so a page in a subdirectory is
invisible to every criterion and, worse, reported by none of them.

### Code blocks in documentation

A ```` ```cpp ```` block is a **complete translation unit**, and the snippet gate compiles it. It
does not have to be a whole program — the gate compiles with `-c`, so a self-contained function
or class is enough, and giving one a real signature is the point: a changed callback signature
then fails the build instead of quietly going stale in the docs. (The two READMEs each shipped a
"quick start" example that had not compiled for months; nothing breaks when documentation rots,
so nothing reported it.)

| Written as | Means |
|---|---|
| ```` ```cpp ```` | Self-contained: it carries its own `#include`s and must compile. |
| ```` ```cpp ```` + first line `// doc-snippet: fragment — <why>` | Not compiled. **The reason is mandatory** — an unexplained exemption is an exemption nobody can review. |
| `<!-- doc-snippets: fragments-default -->` near the top of a page | Every block on that page is an excerpt; the ones that are self-contained opt back in with `// doc-snippet: compile`. At least one must. |

Write the block the way a reader would use it. A block that includes nothing from this library
is **red**: the gate infers which module a snippet needs from its own `#include`s, so a snippet
with no library header has opted out of that check.

**The gate compiles against an installed package, not the source tree**, and it defines nothing:
`-I <pkg>/include`, zero `-D`. That is not a shortcut, it is the contract — `uvcpp_config.h` is
generated so that a consumer who defines nothing is correct, and an externally defined value that
disagrees with the build is a hard `#error`. Do not add per-page macro tables to make a snippet
compile; a package without the module reports that snippet as **skipped** (`3`), which is the
honest answer.

One habit this repository has had to learn twice: **verify header dependencies with the compiler,
not with `grep`.** A `grep '<openssl/'` over the public headers matches a *comment*, and the
conclusion drawn from it — "the public headers need OpenSSL headers" — was simply wrong.

### Line references in documentation

Prose that points into the source writes the reference in exactly one shape:

    src/<module>/<file>:<line>
    src/<module>/<file>:<start>-<end>

**Fully qualified, always.** A bare basename (`<file>.h:88`) is ambiguous the moment a second
module grows a file with the same name, and truncated names (`client.h`, `req.h`,
`h2_session.h`) are how the tree accumulated 34 references to files that never existed. The
gate still *accepts* the loose shapes so that old prose fails loudly rather than silently
going unscanned, but write the qualified form.

Placeholders are written with angle brackets (`<file>`, `src/<module>/<file>`) and are **not**
read as citations — the gate skips them, the same way `check_docs.py` does. That matters for
prose *about* citation syntax, which would otherwise cite itself red.

A reference to third-party source — nghttp2's `nghttp2_session.c`, say — is written with a
namespace prefix:

    nghttp2:nghttp2_session.c:2315

The gate ignores prefixed references and **prints each one it ignored**, so the escape hatch
cannot be used quietly to hide a broken in-repo citation.

`tests/tools/doc_line_refs.lock` is a lockfile, generated by `--update` and never edited by
hand. It records a content hash of every cited range. When you edit a line that documentation
points at:

1. Re-read the citation and confirm it still describes what the code now does.
2. Adjust the line number if the edit moved it.
3. `python tests/tools/check_doc_lines.py --update`
4. Commit the lockfile alongside the change.

Step 1 is the one that matters; the gate exists to make it impossible to skip. If a citation
is wrong rather than merely stale, fix the prose — do not move the citation to whatever line
happens to make the gate green.

The extension is `.lock`, not `.txt`, and it is worth not "tidying" that: `.gitignore` carries a
blanket `*.txt`, so a `doc_line_refs.txt` is **silently untracked** — it works perfectly on the
machine that generated it and is absent from every clone. Criterion 3 fails closed on a missing
lockfile on purpose (deleting it must not be a way to pass), so the symptom would have been
"CI is red and the file is not in the repo".

One more convention, optional but useful: wrapping a quotation in `「…」` immediately next to a
reference makes the gate check that the quoted text actually appears in the cited range. A
quotation containing `…` is treated as elided and is not checked.

### Security-sensitive changes

The library handles sockets, TLS and a memory allocator, so a silent regression here is worse
than a loud one. Two habits from this repository's history are worth keeping:

- **A test whose premise is not asserted tests nothing.** If a race or ownership test assumes
  something about the environment, assert it — otherwise it stays green through the very bug it
  was written to catch.
- **Prove a coverage gap before filling it.** Mutation testing (`tests/tools/*_mutation.py`)
  is how this repository establishes that a test would actually fail if the implementation
  broke. "There is no test for X" is a hypothesis until a mutant survives.

## Getting help

Open an issue. If you are reporting a bug, the most useful thing you can include is the
smallest program that reproduces it, plus what you expected and what happened instead.
