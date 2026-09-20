# Testing Guide

How the test suite is organised, how to add a test, what the scripts in `tests/tools/` do, and
why several of the conventions here exist — most of them were paid for once already.

## The three layers

| Directory | Shape | Registered by |
|---|---|---|
| `tests/unit/` | three executables, wired by hand | `tests/CMakeLists.txt` → `add_subdirectory(unit)` |
| `tests/functional/` | **one executable per `.cpp`**, from a `file(GLOB)` | root `CMakeLists.txt`, inside `if(UVCPP_BUILD_FUNCTIONAL)` |
| `tests/expand/` | one executable, the memory pool | root `CMakeLists.txt`, inside `if(UVCPP_BUILD_EXPAND)` |

`tests/CMakeLists.txt` is eight lines long and adds **only** `unit`. The other two directories
are added from the root file, because they depend on switches the root file owns. Adding a test
directory means editing the root `CMakeLists.txt`, not `tests/CMakeLists.txt`.

`tests/expand` does not have a switch of its own — it follows `UVCPP_BUILD_EXPAND`.

## Filename is the filter key

`tests/functional/CMakeLists.txt` globs every `.cpp` in the directory and then **removes** the
ones the current configuration cannot build. There is no per-file registration and no list to
keep in sync: the file name decides.

| File name matches | Required switch | Filter |
|---|---|---|
| `web_*.cpp` | `UVCPP_BUILD_WEB` | `EXCLUDE REGEX "web_.*\.cpp$"` |
| `web_app_*.cpp` | `UVCPP_BUILD_WEBAPP` | `EXCLUDE REGEX "web_app_.*\.cpp$"` |
| `web_ssl_app_*.cpp` | `UVCPP_BUILD_WEBAPP` + OpenSSL | `EXCLUDE REGEX "web_ssl_app_.*\.cpp$"` |
| `web_ssl_*.cpp` | `UVCPP_ENABLE_OPENSSL` | `EXCLUDE REGEX "web_ssl_.*\.cpp$"` |
| any file with `h2_` in it | `UVCPP_ENABLE_NGHTTP2` | `EXCLUDE REGEX "/[^/]*h2_[^/]*\.cpp$"` |

Each executable is named `test_<file-stem>`, with every character outside `[A-Za-z0-9_]`
replaced by `_`. So `web_ssl_app_ws_func.cpp` becomes `test_web_ssl_app_ws_func`, and that is
also the ctest test name — **the file name is what you pass to `ctest -R`**.

### The `h2_` filter and the `[^/]*` prefix

That prefix is not cosmetic. `file(GLOB)` yields **full paths**, so an anchored `^h2_` never
matches anything and the exclusion silently does nothing. The failure mode is not a build error,
because `UVCPP_NGHTTP2_ENABLE` is always defined — as `0` in a tree without nghttp2 — and the
file has a fallback `main()` for exactly that case (see the next section). The result was
`test_h2_session_func` **registered, passing, and asserting nothing**: a green test in a tree
with no HTTP/2 at all.

When you write a filter, verify it against a real tree — `ctest -N` in a tree that should have
the test excluded, and check the test is genuinely absent from the list.

## Tests are removed, not skipped

**A test that cannot run in this configuration must not exist in this configuration.** Returning
0 from a stub `main()` is indistinguishable from a pass in ctest output, and the exclusion filter
is what actually removes the file.

Two files still carry a fallback `main()`, and both are examples of the trap rather than of the
rule:

```cpp
// tests/functional/h2_session_func.cpp
#else  // UVCPP_NGHTTP2_ENABLE
int main() {
  std::cout << "[h2_session] SKIP (nghttp2 disabled)\n";
  return 0;
}
#endif
```

```cpp
// tests/functional/web_app_pipeline_func.cpp
#else
int main() { return 0; }
#endif
```

The second one does not even print anything. Both are dead code as long as the CMake filter
works, and both turn into a silent false pass the moment it stops working. Do not copy this
pattern into a new test; if a file must not be built, exclude it in CMake.

To confirm a test is genuinely running rather than absent: run it directly and look for its
output. 88 of the 89 functional tests print a `[<name>]`-prefixed banner; the closing line is
**not** uniform — 59 print an `ALL PASS` / `FAIL` summary, and the rest print something else (a
`done success=` line, or only per-case `-> PASS` lines). There is no single convention to grep
for, so read the executable's actual output rather than pattern-matching it.

### Exit codes

Success is `0`. Failure is non-zero, and the code is **not uniform**: most `main()`s `return 2`,
but twelve functional tests `return 1`. Check for non-zero rather than for a specific value —
this is exactly what the mutation drivers' verdict rule does.

The `main()` of most functional tests returns on the **first** failing case, which is why a
scoped run matters — a full-suite exit code tells you *something* failed, not *which* group, and
it cannot distinguish "my target group failed" from "an earlier group failed". The mutation
drivers below are built around this.

## Adding a test

1. Create `tests/functional/<name>_func.cpp` — the name must carry the right prefix for the
   switch it needs (see the table above).
2. Save it as **UTF-8 with BOM** if it contains non-ASCII characters.
3. Re-run CMake (`cmake -S . -B <tree>`); `CONFIGURE_DEPENDS` usually picks it up on the next
   build, but a stale configure is a common cause of "my test is not in `ctest -N`".
4. Build the **default** target so `copy_test_dlls` runs — see
   [`build-guide.md`](build-guide.md#where-the-artifacts-go).

## The flaky test, and the one with its own timeout

**`test_shutdown_func` is excluded in CI only.** It is flaky at roughly 1%, and it is excluded
via `--exclude-regex "test_shutdown_func"` on every `ctest` invocation in `ci.yml` and
`release.yml`. It is *not* excluded locally, and it is not hanging — the comment in `ci.yml`
says so explicitly, because "flaky" and "hangs" get conflated and the second one would justify a
much more invasive fix. Run it locally; if it fails, re-run it before believing it.

**`test_web_app_static_func` has its own `TIMEOUT 90`.** It is the heaviest test in the tree
(≈16.5 s on the Ubuntu runner, ≈21.4 s on Windows) while CI passes `--timeout 30`. A per-test
`TIMEOUT` property **overrides the command-line `--timeout`** — both directions were measured:
with the property set, `ctest --timeout 1` still runs it to completion and passes; with the
property removed, the same command reports `Timeout` immediately. So the remaining tests keep a
tight 30-second hang detector, and this one gets a budget sized to what it actually does. The
guard is `if(TARGET test_web_app_static_func)`, because with `UVCPP_BUILD_WEBAPP=OFF` the target
has been filtered out and `set_tests_properties` on a missing test is a configure error.

## Stale binaries: the failure that looks like a pass

This is the single most expensive class of mistake in this repository. ctest runs whatever
executable is on disk; CMake does not delete executables whose source files have been removed.

- **A failed build looks like a green run.** If the build fails, `ctest` happily runs the
  *previous* binaries. Grep the build output for `error C` / `error LNK` / `error MSB` on every
  tree before trusting a test result.
- **Building one target leaves the test DLL stale.** `--target test_foo` recompiles the library
  but does not refresh the copy in the test directory, so new assertions run against the old
  library. Compare mtimes, not file sizes.
- **A test whose source was deleted still has an executable.** This is why `ctest_list.py`
  enumerates tests from the ctest manifest rather than from the disk.
- **A header change needs a full rebuild.** Adding a private member changes the object layout;
  any executable built against the old layout dies with `0xC0000409` *before* `main` and prints
  nothing. A zero-output crash is a signal to rebuild everything, not to debug.

## `tests/tools/` — the script index

Twenty-five scripts. Most exist because a specific claim needed to be *measured* rather than
argued, so they are evidence-producing tools, not a coherent framework. Several are one-shots
kept because deleting them would lose the method.

### Gate exit codes: `3` is **not** a failure

The gate scripts — `check_doc_versions.py`, `check_docs.py`, `verify_tree.py`,
`check_config_contract.py` — share a three-valued exit code:

| Code | Means |
|---|---|
| `0` | every criterion ran and passed |
| `1` | a criterion **ran and is red** |
| `3` | a criterion's **premise was missing, so that criterion never ran** |

`3` is the one that misleads. "The README has no option table, so criterion 1 was not judged" is
a completely different state from "criterion 1 was judged and is red" — but a bare non-zero exit
code renders the two identically, and so does a green run to anyone skimming a log. Each gate
therefore says which it is in its **summary block** (`（判据 1 **没判**：…）`, or an explicit
"premise not satisfied" line), and the summary is what you read; the exit code alone is not the
verdict. A red criterion also outranks an unjudged one — if one criterion ran and failed while
another never ran, the gate exits `1`, because the red is the more actionable fact.

The same concern is why each criterion carries an **anti-vacuity** rule. `check_docs.py` insists
on *finding* the option table before comparing against it, and `check_doc_versions.py` requires
*exactly four* version strings rather than "at least one". A criterion that quietly matches
nothing is the failure mode worth spending a rule on: it looks green forever.

### Acceptance gates

| Script | What it does |
|---|---|
| `verify_tree.py` | **The five gates for one build tree** — see below. Use this before claiming a change is verified. |
| `run_pageheap_gate.py` | Full suite under PageHeap — the only gate that has ever caught a use-after-free. Optional, slow, Windows-only. |
| `ctest_list.py` | Answers "which executables should run" **from the ctest manifest, not the disk**. |
| `check_docs.py` | Documentation gate — the CMake option tables in both READMEs match the options the build actually defines (both directions), every relative link and repo path resolves, and no `doc/*.md` is orphaned. Runs on every push. |

**`verify_tree.py`'s five gates.** Any one alone is insufficient; all five must pass.

1. Build output contains zero `error C…` / `error LNK…` / `error MSB…` — ctest alone will run
   stale binaries.
2. After `copy_test_dlls`, compare the md5 of `uvcpp.dll` in each test directory.
3. Test executables' mtime is newer than their source files — checking the DLL alone is not
   enough.
4. The full ctest run.
5. A named set of tests, run N times in a row, with zero non-clean runs — for stability.

`--reconfigure` re-runs the configure step. It passes the **full** flag set, deliberately: a
partial set produces a fresh workspace configured with `BUILD_TESTS=OFF`, where the build is
clean because there is nothing to test. Note that the flags it passes must stay identical to CI's
for that job.

**The PageHeap gate.** PageHeap turns each allocation into its own page with guard pages, so a
read or write past the end and any use-after-free becomes an immediate access violation. The
existing three-layer acceptance (normal build, no-pool build, unit tests) was fully green while
this gate pulled out eight test cases with use-after-free bugs. A plain green run does **not**
mean there is no use-after-free; only this gate can see them. Triage with `cdb`, reading `rax`
and `rcx`.

### Release chain

| Script | What it does |
|---|---|
| `package_release.py` | Assembles one build tree into a release zip. Deliberately bypasses `cmake --install` and reproduces the layout by hand — see [`release-process.md`](release-process.md). |
| `check_doc_versions.py` | Asserts the version string in both READMEs equals the one in the source tree. |
| `check_config_contract.py` | Asserts the "enable macro matches the DLL" contract holds, using **bare compiler invocations** on a packaged header + DLL and adding not one `-D`. |

### Mutation drivers

Each of these takes a set of single-site mutations, applies them one at a time, rebuilds, runs
the tests, and reports whether the mutation was caught — and by which test.

| Script | Covers |
|---|---|
| `stream_mutation.py` | 3a — streaming request pipeline, 100-continue, work-pool limit |
| `limits_mutation.py` | 3b — size-limit matrix, truncation, `PART_BODY_SKIP` |
| `upload_mutation.py` | 3b — upload disk sink |
| `upload_route_mutation.py` | 3b — `upload_route()` wiring |
| `stream_resp_mutation.py` | 3c — chunked streaming responses, `send_file` chunked reads |
| `step3_mutation.py` | permessage-deflate |
| `mutate_ws_client.py` | framework-level WebSocket client |
| `webapp_ws_mutation.py` | WebSocket wiring — `enable_wss`, upgrade dispatch, shutdown |
| `ws_ownership_mutation.py` | WebSocket session ownership |
| `wss_mutation.py` | `enable_wss()`'s TLS half |
| `run_idle_mutation.py` | `run()` returning when there is nothing left to wait for |
| `run_tcp_client_dtor_mutation.py` | `~uvcpp_tcp_client` — no sleeping in the destructor |

**The verdict rule is two conditions, not one**: (1) the exit code is non-zero, **and** (2) the
*expected group*, run on its own, is also red. Running the full suite and observing that
something failed is not enough — the first failing test aborts `main()`, so a later group can
look fine while an earlier one carried the failure.

### Probes

Probes answer the question a mutation driver cannot: when a mutant survives, is that because the
mutation is **equivalent** (it cannot change behaviour) or because there is a **real coverage
gap**? Reading the source can produce a hypothesis, but a hypothesis is not evidence — and in
the round that introduced these, the two mutants that *read* as obviously equivalent and the two
that read as obviously reachable both turned out to be the opposite of the guess. So a surviving
mutant is never written down as "equivalent" without a probe.

| Script | Adjudicates |
|---|---|
| `upload_mutation_probe.py` | The four survivors U2/U3/U4/U5 from `upload_mutation.py` |
| `v5_probe.py` | `upload_route_mutation.py`'s V5 — `hooks.on_abort` replaced with a no-op |
| `x9_probe.py` | Which test actually discriminates the "short read is not retried" mutant |
| `x14_probe.py` | Whether "do not notify the stream object on disconnect" is caught |
| `run_loop_leak_probe.py` | Locates the 262 ownerless leaking loops by handle family |

### Infrastructure

| Script | What it does |
|---|---|
| `patch_trampolines.py` | Rewrites "call a `std::function` member in place" trampolines into "copy first, then call". A source-transform tool, not a test. |

## Mutation-testing rules

These are repository rules, learned the hard way:

- **Restore from a file backup, never from an in-memory copy.** A mutation script that pipes its
  output through `head` gets `SIGPIPE`, its `finally` never runs, and the source is left mutated.
  Copy the file outside the repository first (`.good`) and restore from that.
- **Re-touch the file after restoring.** `shutil.copy2` restores the original mtime, so MSVC
  decides the file is unchanged and skips the rebuild — every subsequent result is off by one.
  Call `utime()` before each compile.
- **A hanging mutant is a caught mutant.** Catch `TimeoutExpired` and record it as caught;
  letting the exception escape kills the script on the first mutant and none of the rest run.
- **A library-side mutant needs one `.obj` only.** Compile the mutated `.cpp` to an `.obj` and
  place it ahead of the `.lib` on the link line, rather than rebuilding the whole library.
- **Read source and build output as UTF-8.** `text=True` decodes as GBK on a Chinese Windows
  install and silently corrupts build-failure detection.
- **Prove the gap before filling it.** "There is no test for X" is a hypothesis until a mutant
  survives, and even then it needs a probe. Asking someone to add coverage for a gap that is
  already covered costs their time twice.

## Running the tests by hand

```bash
# everything
ctest --test-dir build -C Release --output-on-failure

# one test, as CI runs it
ctest --test-dir build -C Release -R test_web_stream_func --output-on-failure

# the CI exclusion, reproduced locally
ctest --test-dir build -C Release --timeout 30 --exclude-regex "test_shutdown_func"
```

A test executable can also be run directly, which is the fastest way to see whether it is really
executing: `<tree>/tests/functional/Release/test_web_stream_func.exe`.

## See also

- [`build-guide.md`](build-guide.md) — switches, build trees, and why the DLL copy matters
- [`release-process.md`](release-process.md) — where these gates sit in the release chain
- [`ci-guide.md`](ci-guide.md) — the job matrix, and which job runs which suite
