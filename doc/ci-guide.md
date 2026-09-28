# CI Guidelines & Rules

This document defines the rules and best practices for maintaining CI in this project.
**All contributors must read and follow these guidelines before modifying `.github/workflows/ci.yml`.**

---

## 1. CI Job Matrix Overview

| Job | Platforms | Config | Why |
|-----|-----------|--------|-----|
| `basic` | Ubuntu, macOS | static + shared, web=OFF | Core library sanity |
| `windows-basic` | Windows | shared only, web=OFF | Windows shared coverage (static is in `windows-static`) |
| `windows-static` | Windows | static only, web=OFF, MSVC gen | Windows static + MSVC coverage |
| `web` | Ubuntu, macOS, Windows | shared, web=ON | HTTP/WebSocket module |
| `ssl` | Ubuntu, macOS, Windows | shared, web=ON, OpenSSL=ON | SSL/TLS module |
| `h2` | Ubuntu, macOS, Windows | shared, web=ON, OpenSSL=ON, nghttp2=ON | HTTP/2 coverage; the only job asserting `NGHTTP2=ON` |
| `quic` | **Ubuntu only** | shared, net=ON, **web=OFF**, OpenSSL=ON (3.5 built in-job), quic=ON | QUIC skeleton; the only job asserting `UVCPP_ENABLE_QUIC=ON`, and the only one covering "SSL on + web off" |
| `full` | Ubuntu, macOS | shared, web=ON, OpenSSL=ON, zlib=ON | All features enabled |
| `mingw64` | Windows (MSYS2 MinGW64) | shared, self-contained DLL, **+ Debug artifact** | MinGW release shape + import-table assertion |
| `config-contract` | Ubuntu, Windows | package → install → consumer, **+ Debug artifact** | The `UVCPP_*_ENABLE` macro contract (see §5) |

The two rows marked **+ Debug artifact** build a second, `UVCPP_BUILD_TESTS=OFF` tree
(`build-mingw-dbg` / `build-cfg-dbg`) and pass `--debug-tree` to `package_release.py`.
That is what puts the Debug staging, the `.pdb`, and `uvcpp-debug.pc` under a per-push
gate — the release chain only runs on tags, so without this the whole Debug path would
first execute on the day of a release.

**Rationale**: Windows MSVC compilation is slow. Basic builds are split into two separate jobs (`windows-basic` shared + `windows-static` static) so each job has only ONE build cycle.

---

## 2. Adding a New Job

Checklist before adding a job:

1. **Single *test* cycle per Windows job** — never run two `cmake --build` + `ctest`
   cycles in one Windows job. An extra **artifact-only** build (no tests, no ctest) is
   allowed when the job's deliverable needs a second configuration — see the Debug
   artifact above — but it still costs compile time, so raise `timeout-minutes` to cover it.
2. **Add `--timeout 30`** to every `ctest` invocation — prevents a single hung test from blocking the entire CI.
3. **Add `--exclude-regex "test_shutdown_func"`** to every `ctest` invocation. See §4 for why it is the only one still excluded.
4. **Copy runtime DLLs on Windows** before running ctest. See §3 below.
5. **Set `timeout-minutes`** — 90 min for multi-platform jobs, 60 min for single-platform Windows jobs.
6. **Use `shell: bash`** for all run blocks — cross-platform compatibility.
7. **Matrix `fail-fast: false`** — a single platform failure must not cancel others.

---

## 3. Windows DLL Management (CRITICAL)

When building shared libraries on Windows, test executables need DLLs next to them at runtime.
The CMakeLists.txt POST_BUILD step copies `uvcpp.dll`, but FetchContent-built dependencies
are NOT auto-copied.

### Required DLL copy list

| Job | uvcpp.dll | uv.dll | llhttp.dll | OpenSSL DLLs | nghttp2 DLL |
|-----|-----------|--------|------------|--------------|-------------|
| `windows-basic` | ✓ | ✓ | — | — | — |
| `windows-static` | — | ✓ | — | — | — |
| `web` | ✓ | ✓ | ✓ | — | — |
| `ssl` | ✓ | ✓ | ✓ | ✓ | — |
| `h2` | ✓ | ✓ | ✓ | ✓ | — |

**ngtcp2 has no column either, and for the same reason** — it is linked static
(`ngtcp2_static` + `ngtcp2_crypto_ossl_static`), and the `quic` job is Ubuntu-only, so it never
appears in this Windows table at all.

**nghttp2 has no DLL column value on purpose.** It is the one FetchContent dependency
linked **static** (`nghttp2_static` + `NGHTTP2_STATICLIB`, see `CMakeLists.txt`), so
`uvcpp.dll` gains no new runtime dependency and no copy step is needed. That was the
reason for choosing static: the alternative is one more DLL to chase through every
job, every packaging script, and every consumer's `bin/`.

### DLL copy step template

```yaml
- name: Copy runtime DLLs (Windows)
  if: runner.os == 'Windows'
  shell: bash
  run: |
    for d in tests/unit tests/functional tests/expand; do
      mkdir -p "build-xxx/$d/Release"
      cp build-xxx/Release/uvcpp.dll "build-xxx/$d/Release/" 2>/dev/null || true
      cp build-xxx/_deps/libuv-build/Release/uv.dll "build-xxx/$d/Release/" 2>/dev/null || true
      # Add dependency-specific DLLs as needed
    done
```

### Adding a new FetchContent dependency

If you add a new library via FetchContent that builds as a shared DLL (because
`BUILD_SHARED_LIBS=ON` by default), you MUST:

1. Find where the DLL is output (`_deps/<name>-build/Release/<name>.dll`)
2. Add a `cp` line to ALL relevant Windows jobs' DLL copy steps
3. Use `2>/dev/null || true` — the DLL may not exist in static configs

**Do NOT** try to force a FetchContent dependency to build static by setting
`BUILD_SHARED_LIBS=OFF` — some projects (e.g., llhttp) fail to create proper
CMake targets when built that way.

**nghttp2 is the counterexample, and it does it the other way round.** Instead of
flipping the global `BUILD_SHARED_LIBS`, it selects its own static target
(`nghttp2_static`, aliased as `nghttp2`) inside a `function()` scope that also
force-sets `ENABLE_LIB_ONLY` / `ENABLE_APP` / `ENABLE_HPACK_TOOLS` / `ENABLE_EXAMPLES`
as **non-cache** variables — a plain `set(... CACHE ... FORCE)` would leak `BUILD_TESTING`
into the root scope and knock out `tests/functional`'s registration. Two more traps
specific to it:

- `EXCLUDE_FROM_ALL` + the CMake `FetchContent_Populate` + `add_subdirectory` shape
  (copied from zlib, **not** from llhttp's `MakeAvailable`) — the latter registers
  install rules and would ship nghttp2's own `nghttp2Config.cmake` / `libnghttp2.pc`
  out of `cmake --install`.
- nghttp2 does not give consumers `ssize_t` on MSVC. `src/http2/uvcpp_h2_nghttp2.h`
  defines it as `int` (not `SSIZE_T`) — the library is compiled with `int`, so anything
  else is an ABI mismatch. `_CRT_DECLARE_NONSTDC_NAMES` does not help, and
  `NGHTTP2_NO_SSIZE_T` deletes callbacks we need.

---

## 4. Test Exclusion Policy

Tests excluded from CI (`--exclude-regex`):

| Test | Reason | Platforms |
|------|--------|-----------|
| `test_shutdown_func` | Flaky, not hanging: ~1% (2 failures in 210 runs, both trees). libuv shutdown race, still open. | All |

**Only one test is excluded today.** Two others used to be listed here and were
removed on 2026-09-17 after measuring them instead of trusting the label:

| Test | Was listed as | Measured (80 runs each, both trees) |
|------|---------------|-------------------------------------|
| `test_tcp_func` | "Pre-existing hang (dual-loop thread join)" | 0 failures, ≤1 s per run |
| `test_memory_pool` | "Pre-existing hang (multi-thread pool alloc on Windows)" | 0 failures, ≤1 s per run |

Both had been exclusions for defects fixed long before. `test_memory_pool`'s is
documented: it was a missing-DLL-copy bug (see `CMakeLists.txt:1680`), fixed and
left in the exclude list anyway. `test_tcp_func`'s dual-loop teardown is most
likely the `~uvcpp_tcp_server` fix, which is what removed the two `sleep_for`
calls that were joining the worker thread — that is an inference from the
record, not something this measurement proves. What the measurement *does*
show is the part that matters: the exclusion outlived the bug. When you touch
this list, re-measure the entries; a reason written months ago is a hypothesis,
not a finding.

`.github/workflows/ci.yml` was updated to match on 2026-09-17: all seven `ctest`
invocations now pass `--exclude-regex "test_shutdown_func"` and nothing else, so
`test_tcp_func` and `test_memory_pool` run on every job and must stay green.

**Rule**: excluded tests must have a tracking issue. Do not add to the exclude
list without documenting the reason here and filing a GitHub issue.

---

## 5. CMake Build Options in CI

Every CI job must explicitly set these options — never rely on defaults:

```
-DCMAKE_BUILD_TYPE=Release
-DUVCPP_BUILD_TESTS=ON
-DUVCPP_BUILD_STATIC=ON|OFF
-DUVCPP_BUILD_SHARED=ON|OFF
-DUVCPP_BUILD_WEB=ON|OFF
```

For web/ssl/full jobs, also set:
```
-DUVCPP_ENABLE_ZLIB=ON|OFF
-DUVCPP_ENABLE_OPENSSL=ON|OFF
```

The `h2` job adds:
```
-DUVCPP_ENABLE_NGHTTP2=ON
```

**This option is `OFF` by default, and no other job sets it — so `h2` is the only job
that compiles `src/http2/` at all.** Everything else builds a library without HTTP/2
and goes green, which is correct for them and meaningless as evidence about HTTP/2.
Two gates in that job exist purely to keep that from turning into a vacuous green,
and they should not be "simplified" away:

1. after configure, assert `UVCPP_ENABLE_NGHTTP2:BOOL=ON` in `CMakeCache.txt` **and**
   that configure printed `nghttp2 integrated` **and** `Including http2 module in build`.
   The first alone is not enough: the project force-disables nghttp2 when OpenSSL is
   off, so a cache read can be stale relative to what actually got built.
2. before ctest, assert `ctest -N` lists `test_h2_session_func`. Without nghttp2,
   those test files compile their `#else` branch — a `main()` that prints `SKIP` and
   **returns 0** — so "not tested" and "passed" are indistinguishable in a ctest
   summary. Note also that `ctest` reports `100% tests passed` just as happily when
   the tests were never registered.

### The `quic` job

`quic` mirrors the `h2` gates for the same reason (`1.4.1`), and adds three problems of its own:

- **It builds OpenSSL 3.5.0 from source inside the job** — the expensive step, hence
  `timeout-minutes: 90`. It has to: the module cannot be configured against a TLS library
  without the QUIC API, and no runner has one as a package. Ubuntu 24.04 ships 3.0.13, which
  has neither `SSL_provide_quic_data` nor `SSL_set_quic_tls_cbs`. Two traps in that step are
  worth naming, because both were **measured** on a real 3.5 build rather than guessed: the
  exported `SSL_set_quic_tls_cbs` carries a **version suffix**
  (`SSL_set_quic_tls_cbs@@OPENSSL_3.5.0`), so an end-anchored `nm | grep` matches nothing and
  fails the job for no reason; and the installed `bin/openssl` has **no RUNPATH**, so `ld.so`
  picks up the runner's 3.0.13 first and the binary dies with ``version `OPENSSL_3.4.0' not
  found`` — it needs `LD_LIBRARY_PATH`.
- Its configure line is the **only** place in CI that turns SSL on with the web module
  **off** (`-DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_BUILD_WEB=OFF`). That combination used to
  compile `src/ssl/` without ever defining `UVCPP_SSL_LIBS` and then fail at **link** time;
  `1.4.1` moved the OpenSSL discovery block out of `if(UVCPP_BUILD_WEB)` to fix it, and this
  job is the only evidence that the fix holds.
- The gates are the same three, for the same reason: `UVCPP_ENABLE_QUIC:BOOL=ON` in
  `CMakeCache.txt`, both `ngtcp2 integrated` and `Including quic module in build` in the
  configure log, and `ctest -N` really listing `test_quic_api_func`. The cache read alone is
  stale by construction (the downgrade is a plain `set()`, not a cache write), and the
  test-registration gate matters more here than anywhere else: with QUIC off
  `quic_api_func.cpp` compiles its `#else` branch, whose `main()` prints an error and
  **returns 2** — so a test that failed to register is a **failure**, not a silent pass.

- A second, **configure-only** step turns on `nghttp2` and `quic` in one tree. That is the only
  place either job covers the combination: `h2` leaves QUIC off and this job's main configure
  leaves the web module off. It is a real failure mode, not a hypothetical — ngtcp2 and nghttp2
  each create an unconditional `add_custom_target(check)`, so whichever is added second aborts
  the configure. The root `CMakeLists.txt` works around it by copying the ngtcp2 sources into
  the build tree and removing that one line, and this step is what keeps that workaround from
  rotting silently. It is deliberately configure-only: what breaks is a configure-time line, and
  building a transport-less skeleton afterwards would prove nothing extra.

**No macOS or Windows leg this version.** Each would need its own QUIC-capable OpenSSL built
from source in-job, and what is under test is a skeleton that talks to no socket — two more
expensive builds would buy no coverage the ubuntu leg does not already have. The gap is
written down here rather than left implicit. The prebuilt packages ship with
`UVCPP_QUIC_ENABLE 0` regardless, because `release.yml` does not enable QUIC either (that
would need a QUIC-capable OpenSSL on all six legs).

---

### The macro contract: `config-contract` (and the `mingw64` tail)

Consumer-visible `UVCPP_*_ENABLE` macros come from a **generated**
`include/uvcpp/uvcpp_config.h` (`cmake/uvcpp_config.h.in` via `configure_file`), and
every public header includes it **before its first module guard**. A consumer passes no
`-D` at all; a conflicting `-D` from the consumer is a hard `#error` naming this build's
value.

Why a separate job rather than a step on `web`/`h2`: on Ubuntu those legs install
`libuv1-dev`, and `package_release.py` requires libuv to live under the tree's `_deps/`
— otherwise it deliberately exits 2 and produces no package. This job configures the way
`release.yml` does (`UVCPP_BUILD_LIBUV_FROM_SOURCE=ON`) and **with OpenSSL ON**, because
the member-layout shift being guarded against (`ssl_ctx_` is declared before
`http_`/`registry_` in `uvcpp_web_app`) only bites when `UVCPP_OPENSSL_ENABLE=1`. The job
asserts that value in the generated header *before* building: with it off, criterion 1
silently degrades into a no-op.

`tests/tools/check_config_contract.py` runs three criteria:

1. **ABI-shaped positive** — compile a consumer with a **raw compiler invocation** (`-I`
   only, zero `-D`), construct a `uvcpp_web_app`, assert `connection_count() == 0`,
   against the packaged DLL. "It compiles and runs" would be vacuous: the header is
   self-consistent either way, while a wrong macro set compiles, links, and then lands in
   one of two places — an inline accessor reading the wrong offset and returning a
   garbage count (`1073741824`, the original symptom), or a **hard crash**
   (`0xC0000409` on MSVC, empty stdout). Asserting "the count must be a weird value"
   would hang forever on the crashing landing; the criterion asserts `== 0`, so both
   landings go red.
2. **Negative** — the same raw invocation plus one conflicting `-D` must fail to compile,
   with the generated header's `#error` text in the output. It must stay a raw
   invocation: through CMake `target_compile_definitions` is emitted after
   `CMAKE_CXX_FLAGS` and both compilers take the **last** `-D` (MSVC only warns C4005),
   so the `#error` never fires and the check becomes vacuous.
3. **Static** — the include's position relative to the first *real* module guard in every
   public header (checking only "is it present" passes a version nested inside its own
   `#if`, where the macro is not yet defined); no handwritten `uvcpp_config.h` under
   `src/`; the packaged header byte-identical to the build tree's and agreeing with that
   tree's exported `INTERFACE_COMPILE_DEFINITIONS`; and **every** `.pc` in the package
   (`uvcpp.pc` *and* `uvcpp-debug.pc`) points at a library that is actually in the
   package's own `lib/`. That last one iterates a `glob` rather than naming `uvcpp.pc`:
   with a hardcoded name the debug `.pc` was checked zero times, and it is the one whose
   `-luvcppd` exists nowhere else in the package — measured, not assumed: mutating it to
   `-luvcppdx` used to leave the gate green (`1.1.35`).

It is **not** compared against `CMakeCache.txt`: OpenSSL/nghttp2/webapp are silently
downgraded to OFF when their dependency is missing (the `set(UVCPP_ENABLE_OPENSSL OFF)`
/ `set(UVCPP_ENABLE_NGHTTP2 OFF)` / `set(UVCPP_BUILD_WEBAPP OFF)` lines in the top-level
`CMakeLists.txt` — plain `set()` calls, so the cache still reads ON). The export file holds the
post-downgrade values, generated from the same literals as the header.

Toolchains covered: `ubuntu` (gcc/ELF), `config-contract`'s `windows` leg (MSVC/PE — it
needs `ilammy/msvc-dev-cmd` because `cl.exe` is not on the Windows runner's default
PATH), and `mingw64` (MinGW/PE; that job gained `mingw-w64-x86_64-python` for this).

**Known gap:** the chain cannot run on macOS — `package_release.py`'s `PLATFORMS` has no
macOS key (`mingw-x64/arm64`, `msvc-x64/arm64`, `linux-x64/arm64` only). So the macro
contract is never verified against a clang/macOS toolchain.

---

## 6. Installing System Dependencies

### Ubuntu
```bash
sudo apt-get update && sudo apt-get install -y libuv1-dev ninja-build
# ssl job adds: libssl-dev
# full job adds: libssl-dev zlib1g-dev
```

### macOS
```bash
brew install libuv ninja
# ssl/full jobs add: openssl
# full job adds: zlib
```

### Windows
```bash
# No system deps needed for basic/web (libuv + llhttp via FetchContent)
# ssl job: choco install openssl --no-progress
# OpenSSL DLL path: C:/Program Files/OpenSSL/bin/ (or OpenSSL-Win64)
```

---

## 7. When Adding a New Source Module

1. Add a `UVCPP_BUILD_<MODULE>` option in `CMakeLists.txt`
2. Add corresponding `UVCPP_<MODULE>_ENABLE` compile definition
3. Filter sources with `list(FILTER ... REGEX "src/<module>/")` when disabled
4. Add a CI job or extend an existing one to test the new module
5. If the module pulls new FetchContent deps, update the DLL copy steps
6. Add functional tests in `tests/functional/`

---

## 8. Debugging CI Failures

1. **All tests timeout at exactly `--timeout` seconds**: DLL copy is missing or incomplete.
   Check that ALL transitive DLL dependencies are copied.
2. **CMake configure fails with "could not find any instance of Visual Studio"**:
   The runner's VS version changed. Remove hardcoded `-G "Visual Studio XX YYYY"` and let CMake pick.
3. **Single test hangs**: Check if it has an internal watchdog timer. If not, add one first,
   then investigate the root cause.
4. **`|| true` at end of ctest — removed 2026-09-17.** It had been added to tolerate the
   intermittent failures listed in §4, but it applies to the whole `ctest` invocation, so a **real**
   test failure was swallowed exactly like a flake and the job still went green. That is what made
   the stale exclusions in §4 survivable for months: nothing could go red to contradict them.
   **A failing test now fails the job.** If you are here because CI just went red, that is the
   intended behaviour — the failure was always there, it just was not being reported.

   The one remaining justification is `test_shutdown_func` (~1% flake), which stays excluded. If you
   would rather it ran, replace the exclusion with `ctest --repeat until-pass:3` rather than
   reinstating `|| true` — `--repeat` targets the known flake, `|| true` disables the gate.

---

## 9. Versioning

- CI always builds from the `master` branch.
- Version is read from `src/uvcpp/uvcpp_version.h` at configure time.
- Release versions have `UVCPP_VERSION_IS_RELEASE=1`; development versions have `0` with `-dev` suffix.

---

*Last updated: 2026-09-20. This document should be updated whenever CI rules change.*
