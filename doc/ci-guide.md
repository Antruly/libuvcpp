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
| `full` | Ubuntu, macOS | shared, web=ON, OpenSSL=ON, zlib=ON | All features enabled |

**Rationale**: Windows MSVC compilation is slow. Basic builds are split into two separate jobs (`windows-basic` shared + `windows-static` static) so each job has only ONE build cycle.

---

## 2. Adding a New Job

Checklist before adding a job:

1. **Single build per Windows job** — never run two `cmake --build` + `ctest` cycles in one Windows job.
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

| Job | uvcpp.dll | uv.dll | llhttp.dll | OpenSSL DLLs |
|-----|-----------|--------|------------|--------------|
| `windows-basic` | ✓ | ✓ | — | — |
| `windows-static` | — | ✓ | — | — |
| `web` | ✓ | ✓ | ✓ | — |
| `ssl` | ✓ | ✓ | ✓ | ✓ |

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
documented: it was a missing-DLL-copy bug (see `CMakeLists.txt:814`), fixed and
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

*Last updated: 2026-08-09. This document should be updated whenever CI rules change.*
