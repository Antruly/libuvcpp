# Testing Guide

How the test suite is organised, how to add a test, what the scripts in `tests/tools/` do, and
why several of the conventions here exist — most of them were paid for once already.

## The three layers

| Directory | Shape | Registered by |
|---|---|---|
| `tests/unit/` | three executables, wired by hand | `tests/CMakeLists.txt` → `add_subdirectory(unit)` |
| `tests/functional/` | **one executable per `.cpp`**, from a `file(GLOB)` | root `CMakeLists.txt`, inside `if(UVCPP_BUILD_FUNCTIONAL)` |
| `tests/expand/` | one executable, the memory pool | root `CMakeLists.txt`, inside `if(UVCPP_BUILD_EXPAND)` |
| `tests/capi/` | one executable per `.c`, **compiled as C** | `tests/capi/CMakeLists.txt`, added from the root inside `if(UVCPP_ENABLE_CAPI)` |

`tests/CMakeLists.txt` is eight lines long and adds **only** `unit`. The other two directories
are added from the root file, because they depend on switches the root file owns. Adding a test
directory means editing the root `CMakeLists.txt`, not `tests/CMakeLists.txt`.

`tests/expand` does not have a switch of its own — it follows `UVCPP_BUILD_EXPAND`.

`tests/capi` is the one layer whose sources are **not** C++. Its five files
(`capi_common_func.c`, `capi_net_func.c`, `capi_webapp_func.c`, `capi_h2_func.c`,
`capi_quic_h3_func.c` — the last three drive a C-written client against a C-written server,
in one process; on TCP for webapp and h2, on UDP for quic + h3) are compiled by a C compiler
against `include/capi/`, and they are what turns
"the headers are usable from C" from a claim into a measurement — a header that only *looks* like
C (a stray `namespace`, a default argument, `bool` from `<stdbool.h>` missing) fails to compile
here rather than at a C# call site. Because the layer is opt-in, the directory is registered under
`UVCPP_ENABLE_CAPI` instead of a `UVCPP_BUILD_*` switch, and `tests/capi/CMakeLists.txt` may not
include any C++ header. See [`capi-guide.md`](capi-guide.md).

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
| any file with `quic` in it | `UVCPP_ENABLE_QUIC` | `EXCLUDE REGEX "/[^/]*quic[^/]*\.cpp$"` |
| any file with `http3` in it | `UVCPP_ENABLE_HTTP3` | `EXCLUDE REGEX "/[^/]*http3[^/]*\.cpp$"` |

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

Three files still carry a fallback `main()`. The first two are examples of the trap rather than
of the rule:

```cpp
// doc-snippet: fragment — quoted from the middle of a file; the #else half is the point
// tests/functional/h2_session_func.cpp
#else  // UVCPP_NGHTTP2_ENABLE
int main() {
  std::cout << "[h2_session] SKIP (nghttp2 disabled)\n";
  return 0;
}
#endif
```

```cpp
// doc-snippet: fragment — same: the counter-example *is* the excerpt
// tests/functional/web_app_pipeline_func.cpp
#else
int main() { return 0; }
#endif
```

The second one does not even print anything. Both are dead code as long as the CMake filter
works, and both turn into a silent false pass the moment it stops working. Do not copy this
pattern into a new test; if a file must not be built, exclude it in CMake.

The third file is `tests/functional/quic_api_func.cpp`, and its `#else` half is the shape to
copy instead — it prints an error and **returns 2**:

```cpp
// doc-snippet: fragment — the `#else` half only; the load-bearing part above it is a 40-line
// rationale for *this* branch, which is not what this section is about
// tests/functional/quic_api_func.cpp
#else  // UVCPP_QUIC_ENABLE
int main() {
  std::cerr << "[quic_api] UVCPP_QUIC_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}
#endif
```

The other two QUIC test files — `quic_handshake_func.cpp` and `quic_stream_func.cpp`, both
new in `1.4.1` — copy that same `#else` shape. All three match the `quic` filter, so a broken
filter takes all three red together.

The two HTTP/3 test files, `http3_request_func.cpp` and `http3_web_func.cpp` (also new in
`1.4.1`), do the same. Note that the `quic` and `http3` filters do **not** cover each other:
the string `http3` does not contain `quic`, so `-DUVCPP_ENABLE_HTTP3=ON` in a tree where the
guard chain has forced QUIC back to `OFF` drops `quic_*` with one rule and `http3_*` with the
other, and neither rule can stand in for the other. That is why there are two rows in the
table above rather than one.

That is the difference in one line: if the CMake filter stops working, the two files above go
green while testing nothing, and this one goes **red**. The `1.4.1` CI job for QUIC leans on
exactly this (`doc/ci-guide.md` §The `quic` entries) — which is also why the file must be excluded
rather than merely skipped, since a non-zero exit is only usable if a mis-registered test can
actually reach it.

To confirm a test is genuinely running rather than absent: run it directly and look for its
output. 111 of the 112 functional tests print a `[<name>]`-prefixed banner — the odd one out is
`web_app_multiloop_func.cpp`, which prints only per-case lines. The closing line is **not**
uniform either: 73 print an `ALL PASS` / `FAIL` summary, and the rest print something else (a
`done success=` line, or only per-case `-> PASS` lines). There is no single convention to grep
for, so read the executable's actual output rather than pattern-matching it.

Both counts are measurements, not estimates, and they rot — **re-measure them before you quote
them**. They came from

```bash
grep -lE '"\[[a-z0-9_]+' tests/functional/*.cpp | wc -l   # banner: 111
grep -lE 'ALL PASS'      tests/functional/*.cpp | wc -l   # summary: 73
ls tests/functional/*.cpp | wc -l                         # total: 112
```

The earlier form of this paragraph said "88 of the 89" and "59", and then "107 of the 108" and
"108" — those numbers had drifted for several releases because nobody re-ran the command, and
the last set had already drifted **before** HTTP/3 was added: measuring the QUIC batch's own
final commit (`git show 0addc23f`) gives 110 files and 109 banners, i.e. the three QUIC test
files landed without this paragraph being updated. The two HTTP/3 files moved it by two more.
A count in prose with no command next to it is a count that will be wrong; put the command in
with it.

### Exit codes

Success is `0`. Failure is non-zero, and the code is **not uniform**: most `main()`s `return 2`,
but thirteen functional tests `return 1` (`grep -l 'return 1;' tests/functional/*.cpp | wc -l`,
re-measured at `1.4.1` — the prose said "twelve" for a while, which is what a count with no
command next to it does). Check for non-zero rather than for a specific value —
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
via `--exclude-regex "test_shutdown_func"` on every `ctest` invocation in the four platform
files and `release.yml`. It is *not* excluded locally, and it is not hanging — the comment in
the workflow files says so explicitly, because "flaky" and "hangs" get conflated and the second
one would justify a much more invasive fix. Run it locally; if it fails, re-run it before
believing it.

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

Thirty-five scripts (`ls tests/tools/*.py | wc -l`). Most exist because a specific claim needed
to be *measured* rather than argued, so they are evidence-producing tools, not a coherent
framework. Several are one-shots kept because deleting them would lose the method.

### Gate exit codes: `3` is **not** a failure

The gate scripts — `check_doc_versions.py`, `check_docs.py`, `verify_tree.py`,
`check_config_contract.py`, `check_ci_layout.py` — share a three-valued exit code:

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
| `check_doc_snippets.py` | Documentation gate — every ```` ```cpp ```` block in a tracked document actually compiles against a packaged header set. Runs on every push; see [`CONTRIBUTING.md`](../CONTRIBUTING.md#code-blocks-in-documentation) for the block conventions. |
| `check_doc_lines.py` | Documentation gate — every `file:line` reference in a tracked document still points where it did, and none is written in the extension-only shorthand (`<file>.cpp:123`). Runs on every push; see [`CONTRIBUTING.md`](../CONTRIBUTING.md#line-references-in-documentation) for the citation conventions. |
| `check_ci_layout.py` | **CI-layout gate** — `.github/workflows/` is exactly the four per-platform files, each one's job and feature-matrix entries match the table in [`ci-guide.md`](ci-guide.md) **both ways**, each `h2`/`quic`/`http3` entry still carries its four gate strings, and both READMEs' CI badges point at files that exist. Runs on every push. |
| `check_capi_symbols.py` | **C ABI symbol lock** — every function declared `UVCPP_C_API` in `src/capi/*.h` must be in `capi_symbols.lock` *in its own module's slice*, and each slice must match what the library actually exports (`nm -D` / `nm -gU` / `nm` per platform) **both ways**. The lock is sliced by module (`#@ module <name>`) because one CI leg only ever enables part of the module set — the `capi` leg has no NGHTTP2, so the h2 slice is not supposed to be exported there, and a flat lock would call that a deleted symbol. Criterion 2 therefore judges each slice against **that slice's own switch**, read from the tree's `uvcpp_config.h`: on → exact match; off → the slice must appear nowhere, plus an explicit “not judged” line. Criterion 1 needs no build, so a rename is caught even on a machine that never compiled the layer. `--update` rewrites the lock (and refuses to write a declared-but-not-exported symbol into an enabled slice). Runs on every push, on **three** Ubuntu legs — `capi` (common/net/web/webapp judged, h2/quic/http3 not judged), `h2` (five judged, which is why that leg carries `-DUVCPP_ENABLE_CAPI=ON` since batch 3a) and `http3` (six judged — quic and http3 added; webapp not judged, which is why that leg carries the switch since batch 3b). |

**Why `check_doc_lines.py` exists.** `check_docs.py`'s path criterion strips the `:NNN`
suffix (`LINE_SUFFIX_RE`) *before* testing whether the file exists — the line number half was
deliberately discarded, and nothing else looked at it. So the tree carried hundreds of
citations that no criterion could falsify: an editor could reflow a header comment and every
gate would stay green while the surrounding prose quietly pointed at the wrong thing. The
gate closes that path, and the first run of it found real drift that had been shipping —
`recv()` cited at a line that had been blank for some time, two citations to a header that
had been reformatted, and 34 references to file names that do not exist at all (`client.h`,
`req.h`, `h2_session.h`).

Its third criterion is a **lockfile** (`tests/tools/doc_line_refs.lock`) holding a content
hash of every cited range, so a citation cannot rot silently even when the author wrote no
quotation next to it. Editing a cited line therefore requires re-reading the citation and
re-running `--update` to re-affirm it; that is the point, not an inconvenience.

A sixth criterion closes the gate's own blind spot. The extension-only shorthand (`<file>.cpp:123`)
matched **neither** citation regex — one requires a leading alphanumeric, the other's
lookbehind rejects a `:` after a word character — so it was never resolved, never checked
against a line range, and never covered by a lockfile entry. It is also the shape with the
most room to be wrong, since the reader has to infer which file `.cpp` means. All 22
occurrences lived in `doc/web-http-guide.md`, and 17 of them had already rotted: their line
numbers were frozen at the revision the page was written against while the cited files grew,
and one pointed past the end of the file the paragraph's nearest full reference named. The
criterion now reports each occurrence as red instead of resolving it, because resolving it
means guessing — and guessing is what produced the 17.

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
| `http3_mutation.py` | `1.4.1` — HTTP/3's completion accounting, its once-only contract, and the QUIC FIN/RESET split it sits on |
| `capi_mutation.py` | `1.4.2`/`1.4.3` — the C ABI layer's load-bearing rules: handle death (poison + registry), the callback-table `size` rule, the `extern "C"` exception boundary, the ABI-version self-check, the buffer-too-small contract (`1.4.2`); then the callback-scope handle guard, "a header that is not there must not read as an empty one", argument rejection *at the boundary* rather than deep inside, "closed means it must not claim to be connected", and the accepted WebSocket schemes (`1.4.3`, M8–M12); then the same rules as they look on the http2 side — the thread check on `flush`, "headers not arrived" vs status 0, the connection callback table's `size`, and the stack-handle unregister (`1.4.3` batch 3a, M13–M16); then the same again on the quic + http3 side — **the endpoint** callback table's `size`, the borrowed QUIC handle that only h3's `on_disconnect` unregisters once h3 has replaced the connection's callback table, h3's own `FrameScope`, and the negative stream id (`1.4.4` batch 3b, M17–M20). Each segment needs the tree that has *its* module on: M13–M16 have no target in a plain `build-capi`, M17–M20 have none outside a quic + http3 tree, and M8/M10 need a webapp one — so the table as a whole runs on `build-capi-all`. When the tree is missing, the script exits **3** and names the mutations left without a criterion, rather than dressing the missing executable (a `127`) up as a catch. `--only M8` runs a single mutation without rebuilding the whole table |

**`http3_mutation.py` and `capi_mutation.py` are the two drivers here that belong to a `1.4.x`
module**, and the first of them exists because the earlier ones for those modules were not kept:
the QUIC batch (`0eb4e20`…`0addc23f`) and the first HTTP/3 pass (`fe35eca`, `9da32cc`) each ran a
mutation table from a throwaway script that lived outside the repo, so their results were
quotable in a commit message and not re-runnable. This driver re-measures nine mutations over the
two HTTP/3 tests plus `test_quic_api_func`; its own header records each one's verdict, and three
facts from the run are worth repeating here:

- **The once-only contract's guard is a pair, not a site.** Deleting `complete_response()`'s
  internal `if (s.completed) return;` and deleting `on_stream_close`'s `!s.completed` each
  survive *alone* — the other caller still covers it. Delete both at once and `test_http3_request_func`
  goes red with five failures, the first of which is `那一条请求只给过一次：期望 1，实得 2`.
  A mutation table that only ever broke one site at a time would have concluded, wrongly, that
  the contract was untested.
- **A survivor is not automatically a gap, and this driver found one that is.** The
  `add_ack_offset` mutant (attribute a stream's acknowledged bytes to another stream) survives
  the whole tree — all 79 tests green. It is not an equivalent mutant: nghttp3 really does see
  the wrong accounting. It is a **coverage gap**, and the honest thing to write next to it is
  the test that would be needed to close it (concurrent streams with a QPACK dynamic table in
  use), not "equivalent".
- **One caught mutant is caught by hanging, not by failing.** `take_completed()` not popping
  its queue makes both HTTP/3 tests time out (`rc=124`) rather than print a `[FAIL]`. The
  driver prints that distinction instead of flattening it to "red".

**`capi_mutation.py` produced the sharpest of all of these lessons, and it is not about the C
ABI.** Its first version predicted that all three handle-death mutants would be caught, because
the C tests assert "use a freed handle → `UVCPP_C_E_STALE`, never UB". All three survived. The
guards are two (`registry_remove()` + `poison_head()`), so mutating either alone leaves the other
in place — but mutating **both** survived too, which is not redundancy. The assertion was
measuring the allocator: glibc's tcache writes its `next` pointer at offset 0 of a freed chunk,
which is exactly where the handle magic lives, so the "read the magic of freed memory" check
accidentally sees a non-magic value whatever the guards do. The fix was to add an observable that
the allocator cannot fake — `uvcpp_c_live_handle_count()`, i.e. the registry's size — plus a test
that it returns to its baseline; only then did two of the three mutants go red. **The general
rule: an assertion whose mechanism could be satisfied by something other than the code under test
is not evidence, and the only way to find out is to break the code on purpose.** The third mutant
still survives, and the driver now says *why* (the two guards are mutually redundant by design)
instead of relabelling it.

**`capi_mutation.py` then produced the same lesson a second time, one layer up (`1.4.3`).** The
`test_capi_webapp_func` assertion "take a callback-scope handle out of the callback and ask it
something — you must get `E_STALE`, and it must not be UB" looked like the strongest kind of
check. It was not. Its first version ran *after* `app_join()`, i.e. after the server thread's
stack had been returned to glibc: the magic read back as zero, so it passed because the memory
was gone, not because the guard worked — and the read itself was the very UB the assertion
claimed to rule out. Moved to before the join, the UB half goes away, but the mutant that deletes
the unregister loop *still* passes it: the handles live on `route_trampoline`'s stack, so the
frame is reused the moment the callback returns and the magic is overwritten by unrelated writes.
So a magic-based check **cannot** distinguish "poisoned" from "clobbered by stack reuse", and
that mutation is observable only through `uvcpp_c_live_handle_count()`. The two assertions were
measuring the *contract* ("this handle must now report `E_STALE`") while reading as if they
measured the *mechanism* ("the guard is what made it say so") — the `1.4.2` failure mode, wearing
a different hat. Both kinds of assertion are worth having; the error is taking the first as
evidence for the second.

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
