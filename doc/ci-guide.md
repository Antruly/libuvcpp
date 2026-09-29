# CI Guidelines & Rules

This document defines the rules and best practices for maintaining CI in this project.
**All contributors must read and follow these guidelines before modifying anything under
`.github/workflows/`.**

---

## 1. One platform per file, one feature per entry

CI is **four workflow files, one per platform/toolchain**, each with its own `push` and
`pull_request` triggers. There is no aggregate file and no aggregate badge. The layout is
machine-checked by `tests/tools/check_ci_layout.py`, which reads the table below between the
two HTML comments — if you add a feature entry or a platform, that table is part of the change.

<!-- ci-layout:start -->
| Workflow file | `job` | Features (matrix entries) | Workflow `name:` | Check names | Runner |
|---|---|---|---|---|---|
| `.github/workflows/ci-linux-ubuntu.yml` | `linux` | `basic-static`, `basic-shared`, `web`, `ssl`, `h2`, `full`, `quic` | `Linux (Ubuntu)` | `Linux (Ubuntu) / <feature>` | `ubuntu-latest` |
| `.github/workflows/ci-linux-ubuntu.yml` | `config-contract` | （无矩阵） | `Linux (Ubuntu)` | `Linux (Ubuntu) / config-contract` | `ubuntu-latest` |
| `.github/workflows/ci-windows-msvc.yml` | `windows` | `basic-shared`, `basic-static`, `web`, `ssl`, `h2`, `quic` | `Windows (MSVC)` | `Windows (MSVC) / <feature>` | `windows-latest` |
| `.github/workflows/ci-windows-msvc.yml` | `config-contract` | （无矩阵） | `Windows (MSVC)` | `Windows (MSVC) / config-contract` | `windows-2022` |
| `.github/workflows/ci-mingw64.yml` | `mingw64` | （无矩阵） | `Windows (MinGW64)` | `Windows (MinGW64) / mingw64` | `windows-latest` (MSYS2) |
| `.github/workflows/ci-macos.yml` | `macos` | `basic-static`, `basic-shared`, `web`, `ssl`, `h2`, `full`, `quic` | `macOS` | `macOS / <feature>` | `macos-latest` |
<!-- ci-layout:end -->

The `Features` cell is a comma-separated list of the file's `feature:` values, or `（无矩阵）`
for a file whose job is not a matrix. `check_ci_layout.py` compares it **both ways** against
the file, so deleting a feature entry without touching this table is red — that is what makes
"QUIC was quietly dropped from the MSVC leg" impossible to merge.

**This layout is now the measured one, not the intended one.** The first run of a commit
carrying all four files produced exactly the 23 checks in the table above — none missing, none
extra, no collisions — and the run that also carries the two corrections described below is
green on all four platforms (2026-09-29). Two things were only learnable by running it:
`src/quic/*.cpp` and `ngtcp2` compile on **MSVC** for the first time here, and the macOS leg's
own OpenSSL prelude was wrong in a way that had nothing to do with OpenSSL (see §5).

**Why one file per platform.** The old single file was 1369 lines with 10 jobs spanning four
platforms and eight feature sets; the two Windows jobs sat 900 lines apart. A feature matrix
that has grown to eight entries stops being readable when it is interleaved with three other
platforms. Splitting is not a coverage change: every gate that existed before still exists,
and the `check_ci_layout.py` criteria were written to keep it that way.

**Inside a file, the skeleton is written once and the entries carry the differences.** Each
matrix entry has `feature` (also the check name), `flags` (a **block sequence** of `-D`
options, expanded with `join(matrix.flags, ' ')`), and — for the entries that need them —
`gate_*` strings and `openssl_root`. Three rules about the entries are load-bearing:

- **Never use a YAML boolean in an `if:`.** GitHub coerces it into a numeric comparison
  (`true == 'true'` is `1 == NaN`, i.e. **false**), so that leg would never run. Compare
  strings (`matrix.feature == 'quic'`), and remember that an entry missing the key evaluates
  to null, which compares false — exactly what is wanted.
- **`flags` is a block sequence, never a flow sequence and never a folded scalar.** The
  comment explaining why `-DUVCPP_BUILD_EXPAND=ON` is present has to sit on that `-D` line;
  a flow `[...]` has nowhere to put it, and a folded `>` silently joins continuation lines
  into one space and swallows arguments (see the note on that in `release.yml`).
- **Every matrix job sets `name:` explicitly** (to `<platform> / ${{ matrix.feature }}` here).
  Without it GitHub splices the whole matrix — `flags` array included — into the check name,
  which is both unreadable and unstable, while branch protection remembers the name.

**Two things that are deliberately not in these files.**

- **No `concurrency:`.** It would cancel an in-flight run on the next push, and cancelling a
  run **drops a gate**. The gates are the only product here.
- **No aggregate "run everything" workflow.** That would put us back where we started.

### Gaps: combinations that are *not* covered

These are written down, not left implicit. None of them is a regression — the single-file
layout did not cover them either.

| Gap | Why |
|---|---|
| MinGW + QUIC | Deferred to the same batch as splitting the MinGW job by feature: that job is a single job whose `defaults: shell: msys2 {0}` is job-level (it cannot be conditionalised) and whose tail is a hard-coded `--platform mingw-x64` package→consumer contract. |
| MinGW + h2 | Never covered. MinGW has never compiled the HTTP/2 module. |
| MSVC + full | Windows has never had a `full` leg (`full` is Ubuntu + macOS only). |
| Windows static + web/webapp | The static entry is `WEB=OFF`. |
| macOS `config-contract` | `package_release.py`'s `PLATFORMS` has no macOS key — see §5. |
| WSDL with tests | Both places that set `WSDL=ON` also set `TESTS=OFF`; the WSDL module has never been run by ctest. |
| **base build with `UVCPP_BUILD_NET=OFF`** | `tests/functional/CMakeLists.txt` filters web/webapp/ssl/h2/wsdl/quic and **not** net, so `NET=OFF` would compile net test files against a library with `src/net/` filtered out — red by construction. Opening this cell needs that filter first. Today "base" therefore means net at its default (on) with `WEB=OFF`, i.e. the two `basic-*` entries. |

The three workflows that need a second (artifact-only, `UVCPP_BUILD_TESTS=OFF`) tree —
`mingw64`, `config-contract` on Linux and `config-contract` on MSVC — build
`build-mingw-dbg` / `build-cfg-dbg` and pass `--debug-tree` to `package_release.py`. That is
what puts the Debug staging, the `.pdb`, and `uvcpp-debug.pc` under a per-push gate; the
release chain only runs on tags, so without this the whole Debug path would first execute on
the day of a release.

**Rationale for the runners**: Windows MSVC compilation is slow, so each Windows entry has
exactly ONE `cmake --build` + `ctest` cycle.

---

## 2. Adding a Feature Entry (or a Platform)

Checklist before adding a `feature:` entry:

1. **Single *test* cycle per entry** — never run two `cmake --build` + `ctest` cycles in one
   entry. An extra **artifact-only** build (no tests, no ctest) is allowed when the entry's
   deliverable needs a second configuration — see the Debug tree above — but raise
   `timeout-minutes` to cover it.
2. **Add `--timeout 30`** to every `ctest` invocation — prevents a single hung test from
   blocking the whole leg. (`mingw64` uses `--timeout 60`: that tree runs the whole suite
   under MSYS2 and the same value is what `release.yml` uses.)
3. **Add `--exclude-regex "test_shutdown_func"`** to every `ctest` invocation. See §4 for why
   it is the only one still excluded.
4. **Copy runtime DLLs on Windows** before running ctest — the MSVC file has one shared,
   tolerant copy step; extend it if the entry adds a shared dependency. See §3.
5. **Set `timeout-minutes`** — 90 minutes for every job here. A matrix cannot set a
   per-entry timeout, so the worst-case Windows leg sets the job's value.
6. **Use `shell: bash`** for all run blocks in the Linux/Windows/macOS files — except the
   MinGW file, whose job-level `defaults` already puts every `run:` in MSYS2's bash.
7. **Matrix `fail-fast: false`** — a single entry's failure must not cancel the others.
8. **Update the §1 table** and run `python tests/tools/check_ci_layout.py`.

**Adding a platform** means adding a fifth file — which `check_ci_layout.py` criterion 1
rejects outright. That is intentional: a new platform is a spec change (add it to
`PLATFORM_FILES` in the gate, to the §1 table, and to both READMEs' badges), not something
that should slip in unnoticed.

**Files must stay flat under `.github/workflows/`.** `check_doc_lines.py`'s scan set is
`.github/workflows/*.yml` — no `**` — so a workflow in a subdirectory would silently drop its
line-number citations out of that gate's scan set. `check_ci_layout.py` asserts there is no
nested workflow file for exactly this reason.

---

## 3. Windows DLL Management (CRITICAL)

When building shared libraries on Windows, test executables need DLLs next to them at runtime.
The CMakeLists.txt POST_BUILD step copies `uvcpp.dll`, but FetchContent-built dependencies
are NOT auto-copied. The MSVC file has **one** copy step shared by every entry (every `cp`
is tolerant: `2>/dev/null || true`), so the table below is "what that step must find on
disk for this entry", not "what a given job hardcodes":

| Entry | uvcpp.dll | uv.dll | llhttp.dll | OpenSSL DLLs | nghttp2 DLL | ngtcp2 DLL |
|---|---|---|---|---|---|---|
| `basic-shared` | ✓ | ✓ | — | — | — | — |
| `basic-static` | — | ✓ | — | — | — | — |
| `web` | ✓ | ✓ | ✓ | — | — | — |
| `ssl` | ✓ | ✓ | ✓ | ✓ | — | — |
| `h2` | ✓ | ✓ | ✓ | ✓ | — | — |
| `quic` | ✓ | ✓ | — | ✓ | — | — |

Two of those `—` are the kind that look like an omission and are not:

- **`basic-static` has no `uvcpp.dll` cell value** because a static-only entry never
  produces one — nothing links against it, the test executables carry the library inside
  themselves. That is why the old `windows-static` job copied `uv.dll` and nothing else; the
  shared copy step in `ci-windows-msvc.yml` still *attempts* `uvcpp.dll` for every entry, and
  this cell is not a claim about that step, it is a claim about what exists on disk.
- **`llhttp.dll` is `—` on every entry with `UVCPP_BUILD_WEB=OFF`** (`basic-*`, `quic`) —
  **and not merely unneeded, but not built at all**: llhttp is fetched from inside the
  `if(UVCPP_BUILD_WEB)` block, and `CMakeLists.txt` says so in as many words ("llhttp — HTTP
  解析器, WEB=ON 时必须下载"). Measured on this machine: the `web` tree has
  `_deps/llhttp-build/`, the `basic-static` and `quic` trees have no `_deps/llhttp*` at all.
  So a `cp` for it there is a tolerant no-op covering a file that was never compiled.

**nghttp2 and ngtcp2 have no column value on purpose.** Both are linked **static**
(`nghttp2_static` + `NGHTTP2_STATICLIB`; `ngtcp2_static` + `ngtcp2_crypto_ossl_static`), so
`uvcpp.dll` gains no new runtime dependency and no copy step is needed. That was the reason
for choosing static: the alternative is one more DLL to chase through every entry, every
packaging script, and every consumer's `bin/`. The copy step is therefore also the
**assertion** that the static decision still holds — if either ever becomes shared again, the
test executables fail to start (`0xC0000135`) and the `quic`/`h2` entries go red.

**OpenSSL is a different story and must be copied** for `ssl` / `h2` / `quic` (all three link
it, and on the MSVC runner it is the dynamic `choco` build): without `libssl-3-x64.dll` and
`libcrypto-3-x64.dll` next to the executables, every case in those entries exits with
`0xC0000135`, which reads like a DLL-copy bug rather than a TLS/QUIC problem.

### DLL copy step template

```yaml
- name: Copy runtime DLLs (Windows)
  shell: bash
  run: |
    feat="${{ matrix.feature }}"
    tree="build-$feat"
    for d in tests/unit tests/functional tests/expand; do
      mkdir -p "$tree/$d/Release"
      cp "$tree/Release/uvcpp.dll" "$tree/$d/Release/" 2>/dev/null || true
      cp "$tree/_deps/libuv-build/Release/uv.dll" "$tree/$d/Release/" 2>/dev/null || true
      # Add dependency-specific DLLs as needed
    done
```

### Adding a new FetchContent dependency

If you add a new library via FetchContent that builds as a shared DLL (because
`BUILD_SHARED_LIBS=ON` by default), you MUST:

1. Find where the DLL is output (`_deps/<name>-build/Release/<name>.dll`)
2. Add a `cp` line to the shared Windows copy step in `ci-windows-msvc.yml`
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

ngtcp2 needs the same static-selection trick, and one extra: both it and nghttp2 create an
unconditional `add_custom_target(check)`, so turning them on **together** used to abort the
configure. The root `CMakeLists.txt` copies the ngtcp2 sources into the build tree and
removes that one line; §5 has the gate that keeps that workaround honest.

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
documented: it was a missing-DLL-copy bug (see `CMakeLists.txt:1685`), fixed and
left in the exclude list anyway. `test_tcp_func`'s dual-loop teardown is most
likely the `~uvcpp_tcp_server` fix, which is what removed the two `sleep_for`
calls that were joining the worker thread — that is an inference from the
record, not something this measurement proves. What the measurement *does*
show is the part that matters: the exclusion outlived the bug. When you touch
this list, re-measure the entries; a reason written months ago is a hypothesis,
not a finding.

The four platform files were updated to match on 2026-09-17: **every** one of the 21 matrix
legs that runs `ctest` passes `--exclude-regex "test_shutdown_func"` and nothing else
(21 = 7 Linux + 6 Windows MSVC + 7 macOS + 1 MinGW, i.e. the sum of the `Features` column in
§1, plus the MinGW job), so `test_tcp_func` and `test_memory_pool` run on every leg and must
stay green.

**Rule**: excluded tests must have a tracking issue. Do not add to the exclude list
without documenting the reason here and filing a GitHub issue.

---

## 5. CMake Build Options in CI

Every matrix entry must explicitly set these options — never rely on defaults:

```
-DCMAKE_BUILD_TYPE=Release
-DUVCPP_BUILD_TESTS=ON
-DUVCPP_BUILD_STATIC=ON|OFF
-DUVCPP_BUILD_SHARED=ON|OFF
-DUVCPP_BUILD_WEB=ON|OFF
```

For the web/ssl/h2/full entries, also set:
```
-DUVCPP_ENABLE_ZLIB=ON|OFF
-DUVCPP_ENABLE_OPENSSL=ON|OFF
```

The `h2` entries add:
```
-DUVCPP_ENABLE_NGHTTP2=ON
```

**This option is `OFF` by default, and no other entry sets it — so `h2` is the only entry
that compiles `src/http2/` at all.** Everything else builds a library without HTTP/2
and goes green, which is correct for them and meaningless as evidence about HTTP/2.
Two gates in that entry exist purely to keep that from turning into a vacuous green,
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

The strings live in the matrix entry (`gate_switch` / `gate_integrated` / `gate_module` /
`gate_test`) and the steps consume them as `matrix.gate_*`, because the skeleton is shared
with the entries that have no gates. `check_ci_layout.py` asserts both halves: the four
strings are inside the entry that needs them, and the file really references `matrix.gate_*`.

### The `quic` entries

`quic` mirrors the `h2` gates for the same reason (`1.4.1`), and adds problems of its own.
It runs on **three legs** — Ubuntu, macOS and Windows MSVC. MinGW is the gap (see §1).

- **Only the Ubuntu leg builds OpenSSL from source, and it has to.** The module cannot be
  configured against a TLS library without the QUIC API, and no *package* on that runner
  provides one: Ubuntu 24.04 ships 3.0.13, which has neither `SSL_provide_quic_data` nor
  `SSL_set_quic_tls_cbs`. So the leg builds 3.5.0 from source (the expensive step: the job's
  `timeout-minutes: 90` covers it). Two traps in that step are worth naming, because both
  were **measured** on a real 3.5 build rather than guessed: the exported
  `SSL_set_quic_tls_cbs` carries a **version suffix** (`SSL_set_quic_tls_cbs@@OPENSSL_3.5.0`),
  so an end-anchored `nm | grep` matches nothing and fails the leg for no reason; and the
  installed `bin/openssl` has **no RUNPATH**, so `ld.so` picks up the runner's 3.0.13 first
  and the binary dies with ``version `OPENSSL_3.4.0' not found`` — it needs `LD_LIBRARY_PATH`.
- **The macOS leg uses Homebrew's `openssl@3`** (≥ 3.2, so it has the API) and needs no
  in-job build — but it **must** pass `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`:
  `openssl@3` is keg-only and `FindOpenSSL` has no Homebrew hints at all. A prelude step
  looks at that dylib, but it is **diagnostic only: it emits `::warning` and never `exit 1`**.
  That is a deliberate correction, not an oversight, and the first-ever macOS run bought it:
  the prelude ran `nm -gU` and failed the leg for a reason **unrelated to OpenSSL**. `-U`
  means *defined-only* to LLVM's `nm` and the opposite convention to GNU's (`-u`), and a
  Mach-O dylib may be stripped so `nm` has no symbol table to read at all — two ways to red
  that say nothing about the library. The rule this produced: **a self-check must never be
  easier to red than the gate it serves**, because a false red costs a whole macOS rotation
  (seven entries) and points the reader at the wrong cause. What actually decides is gate ②,
  and it now quotes the configure log's own words into its `::error` (see the next-but-one
  bullet), so the *reason* still survives. The probe is not vacuous even though it no longer
  decides: `nm -g` plus a name match **does** find `_SSL_set_quic_tls_cbs` on the runner
  (measured — it is what turned this leg green after the `-gU` form had reddened it), so a
  genuinely QUIC-incapable `openssl@3` would still produce a warning naming itself.
- **The MSVC leg uses `choco install openssl`** (the same source the `ssl`/`h2` entries use).
  Do **not** add `-DOPENSSL_USE_STATIC_LIBS=ON` there: the choco package ships no static
  libraries, `find_package` would fail, and `UVCPP_ENABLE_OPENSSL` would be silently
  downgraded to OFF — red in the wrong place. Because OpenSSL is then dynamic there, the
  shared DLL-copy step has to place `libssl-3-x64.dll` / `libcrypto-3-x64.dll` next to the
  executables (§3).
- **What a QUIC-incapable OpenSSL looks like, on every leg**: *not* a configure-time fatal.
  The root `CMakeLists.txt`'s QUIC precheck warns and force-`set()`s `UVCPP_ENABLE_QUIC OFF`
  first, while the cache still reads `ON`. So the load-bearing gate is
  `ngtcp2 integrated` being **absent** — gate ① (the cache read) is the weak one. Because the
  root precheck only *warns*, gate ②'s failure message carries the configure log's own
  `CMake Warning`/`CMake Error` lines**: without them the annotation would name the symptom
  (ngtcp2 missing) and hide the cause (the OpenSSL), and the log itself is not readable
  without a login (artifact zips answer **401** anonymously). The Linux leg additionally keeps
  a **hard-failing** `nm -D` prelude on its self-built OpenSSL — that one is safe to let fail
  because it was verified green on a real 3.5 build, and it guards the leg that builds
  OpenSSL in the first place.
- Its configure line is the **only** place in CI that turns SSL on with the web module
  **off** (`-DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_BUILD_WEB=OFF`). That combination used to
  compile `src/ssl/` without ever defining `UVCPP_SSL_LIBS` and then fail at **link** time;
  `1.4.1` moved the OpenSSL discovery block out of `if(UVCPP_BUILD_WEB)` to fix it, and these
  entries are the evidence that the fix holds. All three keep web off, so this coverage did
  not get diluted by adding legs.
- The test-registration gate matters more here than anywhere else: with QUIC off all three
  QUIC test files (`quic_api_func.cpp`, `quic_handshake_func.cpp`, `quic_stream_func.cpp`)
  compile their `#else` branch, whose `main()` prints an error and **returns 2** — so a test
  that failed to register is a **failure**, not a silent pass. (This is also why those files
  are excluded rather than merely skipped; see `doc/testing-guide.md`.) The gate string in the
  matrix entry names one of the three (`test_quic_api_func`) as a sentinel; the other two ride
  the same `ctest` invocation.
- On Ubuntu, a second, **configure-only** step turns on `nghttp2` and `quic` in one tree. That
  is the only place either entry covers the combination: `h2` leaves QUIC off and this entry's
  main configure leaves the web module off. It is a real failure mode, not a hypothetical — the
  two `add_custom_target(check)` lines collide (§3). It is deliberately configure-only: what
  breaks is a configure-time line, and building the library afterwards would prove nothing
  extra about that collision.
- **A QUIC-incapable OpenSSL on MSVC would need the Ubuntu recipe** (build it in-job) as the
  fallback. That path has not been taken because `nmake` does not parallelise on Windows and
  the package-manager route is tried first. If the `quic` entry on MSVC goes red with
  `ngtcp2 integrated` missing, that is the fallback, not a redesign.

**macOS and Windows prebuilt packages still ship `UVCPP_QUIC_ENABLE 0`** regardless:
`release.yml` does not enable QUIC (that would need a QUIC-capable OpenSSL on all six legs).

### The macro contract: `config-contract` (and the `mingw64` tail)

Consumer-visible `UVCPP_*_ENABLE` macros come from a **generated**
`include/uvcpp/uvcpp_config.h` (`cmake/uvcpp_config.h.in` via `configure_file`), and
every public header includes it **before its first module guard**. A consumer passes no
`-D` at all; a conflicting `-D` from the consumer is a hard `#error` naming this build's
value.

Why a separate job rather than a step on `web`/`h2`: on Ubuntu those entries install
`libuv1-dev`, and `package_release.py` requires libuv to live under the tree's `_deps/`
— otherwise it deliberately exits 2 and produces no package. This job configures the way
`release.yml` does (`UVCPP_BUILD_LIBUV_FROM_SOURCE=ON`) and **with OpenSSL ON**, because
the member-layout shift being guarded against (`ssl_ctx_` is declared before
`http_`/`registry_` in `uvcpp_web_app`) only bites when `UVCPP_OPENSSL_ENABLE=1`. The job
asserts that value in the generated header *before* building: with it off, criterion 1
silently degrades into a no-op. The same is asserted for `UVCPP_WSDL_ENABLE`, because
`check_doc_snippets.py` treats "module really on" as the premise for the `wsdl/` snippets.

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
`CMakeLists.txt` — plain `set()` calls, so the cache still reads ON). The export file holds
the post-downgrade values, generated from the same literals as the header.

Toolchains covered: `ubuntu` (gcc/ELF), `ci-windows-msvc.yml`'s `config-contract` (MSVC/PE —
it needs `ilammy/msvc-dev-cmd` because `cl.exe` is not on the Windows runner's default
PATH), and `mingw64` (MinGW/PE; that job gained `mingw-w64-x86_64-python` for this).
`check_ci_layout.py` pins the `--platform` / `--cxx` pair of each of the two standalone
`config-contract` jobs, because a wrong pair means the gate silently tests another toolchain.

**Known gap:** the chain cannot run on macOS — `package_release.py`'s `PLATFORMS` has no
macOS key (`mingw-x64/arm64`, `msvc-x64/arm64`, `linux-x64/arm64` only). So the macro
contract is never verified against a clang/macOS toolchain.

---

## 6. Installing System Dependencies

Dependencies are installed **per feature entry**, never as a union — installing
`libssl-dev` for the Ubuntu `quic` entry would put the system's 3.0.13 into the candidate
set and manufacture a fresh silent-downgrade path. Each file has one install step with a
`case "${{ matrix.feature }}"` in it, and an unknown feature name is a hard error there.

### Ubuntu (`ci-linux-ubuntu.yml`)
```bash
# basic-static | basic-shared
sudo apt-get install -y libuv1-dev ninja-build
# web
sudo apt-get install -y libuv1-dev zlib1g-dev ninja-build
# ssl | h2
sudo apt-get install -y libuv1-dev libssl-dev zlib1g-dev ninja-build
# full
sudo apt-get install -y libuv1-dev libssl-dev zlib1g-dev ninja-build
# quic — no libssl-dev on purpose: it builds OpenSSL 3.5 from source
sudo apt-get install -y libuv1-dev zlib1g-dev
# config-contract
sudo apt-get install -y libssl-dev zlib1g-dev ninja-build pkg-config
```

### macOS (`ci-macos.yml`)
```bash
# basic-static | basic-shared
brew install libuv ninja
# web
brew install libuv zlib ninja
# ssl | h2 | full
brew install libuv openssl zlib ninja
# quic — explicit openssl@3, and -DOPENSSL_ROOT_DIR points at it
brew install libuv zlib ninja openssl@3
```

### Windows MSVC (`ci-windows-msvc.yml`)
```bash
# basic-static | basic-shared | web — no system deps (libuv + llhttp via FetchContent)
# ssl | h2 | quic
choco install openssl --no-progress
# OpenSSL DLL path: C:/Program Files/OpenSSL/bin/ (or OpenSSL-Win64)
```

### Windows MinGW64 (`ci-mingw64.yml`)
`msys2/setup-msys2` installs `mingw-w64-x86_64-{gcc,cmake,ninja,openssl,python}`.

---

## 7. When Adding a New Source Module

1. Add a `UVCPP_BUILD_<MODULE>` option in `CMakeLists.txt`
2. Add corresponding `UVCPP_<MODULE>_ENABLE` compile definition
3. Filter sources with `list(FILTER ... REGEX "src/<module>/")` when disabled
4. Add a **`feature:` entry** to the platform files where the module must be covered —
   `feature` + `flags`, plus `gate_*` if the entry can go green without the module. Then
   update the §1 table, and update §1's "Gaps" table for the platforms you are *not* adding.
5. If the module pulls new FetchContent deps, update the shared Windows DLL copy step
6. Add functional tests in `tests/functional/`

---

## 8. Debugging CI Failures

1. **All tests timeout at exactly `--timeout` seconds**: DLL copy is missing or incomplete.
   Check that ALL transitive DLL dependencies are copied.
2. **CMake configure fails with "could not find any instance of Visual Studio"**:
   The runner's VS version changed. Remove hardcoded `-G "Visual Studio XX YYYY"` and let CMake pick.
3. **Single test hangs**: Check if it has an internal watchdog timer. If not, add one first,
   then investigate the root cause.
4. **A whole feature entry stopped running**: check the §1 table against the file with
   `python tests/tools/check_ci_layout.py`. A renamed entry makes every step guarded by
   `if: matrix.feature == '<old name>'` disappear **without an error** — the leg still
   reports success, having done nothing.
5. **`|| true` at end of ctest — removed 2026-09-17.** It had been added to tolerate the
   intermittent failures listed in §4, but it applies to the whole `ctest` invocation, so a **real**
   test failure was swallowed exactly like a flake and the job still went green. That is what made
   the stale exclusions in §4 survivable for months: nothing could go red to contradict them.
   **A failing test now fails the job.** If you are here because CI just went red, that is the
   intended behaviour — the failure was always there, it just was not being reported.

   The one remaining justification is `test_shutdown_func` (~1% flake), which stays excluded. If you
   would rather it ran, replace the exclusion with `ctest --repeat until-pass:3` rather than
   reinstating `|| true` — `--repeat` targets the known flake, `|| true` disables the gate.

### Changed check names (branch protection)

Splitting the file **renamed every check**. A check's name is the job's `name:` **alone** —
GitHub does *not* prefix it with the workflow `name:` (verified against the last single-file
run: it reported `basic (ubuntu-latest)`, not `CI / basic (ubuntu-latest)`). That is why every
job in these four files spells the platform out itself: with a bare `name: ${{ matrix.feature }}`
the four platforms would all report `basic-static` and collide in the checks list.

The left column below is the **complete** old list, read off the last run of `ci.yml` (19
checks, commit `34a7162`), so the mapping is exhaustive rather than illustrative:

| Old check (`ci.yml`, 19) | New check |
|---|---|
| `basic (ubuntu-latest)` | `Linux (Ubuntu) / basic-static` **and** `Linux (Ubuntu) / basic-shared` |
| `basic (macos-latest)` | `macOS / basic-static` **and** `macOS / basic-shared` |
| `windows-basic` | `Windows (MSVC) / basic-shared` (the entry carrying `EXPAND=ON`) |
| `windows-static` | `Windows (MSVC) / basic-static` |
| `web (ubuntu-latest)` | `Linux (Ubuntu) / web` |
| `web (macos-latest)` | `macOS / web` |
| `web (windows-latest)` | `Windows (MSVC) / web` |
| `ssl (ubuntu-latest)` | `Linux (Ubuntu) / ssl` |
| `ssl (macos-latest)` | `macOS / ssl` |
| `ssl (windows-latest)` | `Windows (MSVC) / ssl` |
| `h2 (ubuntu-latest)` | `Linux (Ubuntu) / h2` |
| `h2 (macos-latest)` | `macOS / h2` |
| `h2 (windows-latest)` | `Windows (MSVC) / h2` |
| `full (ubuntu-latest)` | `Linux (Ubuntu) / full` |
| `full (macos-latest)` | `macOS / full` |
| `quic` | `Linux (Ubuntu) / quic` |
| `mingw64` | `Windows (MinGW64) / mingw64` |
| `config-contract (ubuntu-latest, linux-x64, g++, python3)` | `Linux (Ubuntu) / config-contract` |
| `config-contract (windows-2022, msvc-x64, cl, python)` | `Windows (MSVC) / config-contract` |

**New-only checks** (no old counterpart, so nothing to re-point *away* from):
`Windows (MSVC) / quic` and `macOS / quic` are the legs this batch adds; `basic-shared` /
`basic-static` are the old single `basic` leg split in two (it becomes *two* required entries
where there was one).

If branch protection requires status checks **by name**, they must be re-pointed on GitHub
before the first pull request against the new layout, or every PR parks at
"Expected — waiting for status to be reported". There is nothing in the repository that can
do this step for you: it is a repository setting, not a file.

The four badges are 404 until the new files are on the default branch.

---

## 9. Versioning

- CI always builds from the `master` branch.
- Version is read from `src/uvcpp/uvcpp_version.h` at configure time.
- Release versions have `UVCPP_VERSION_IS_RELEASE=1`; development versions have `0` with `-dev` suffix.

---

*Last updated: 2026-09-29. This document should be updated whenever CI rules change.*
