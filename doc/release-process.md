# Release Process

How a release is cut: what triggers it, what each of the six build legs produces, how the
packages are verified, and what the process still does **not** check.

[`RELEASE.md`](../RELEASE.md) is the *history* of what shipped, not the procedure — and since
1.6.0 it is also **not** the body of any Release. `publish` slices that file by tag through
[`tests/tools/release_notes.py`](../tests/tools/release_notes.py), which emits a six-platform
download table, the `## vX.Y.Z` section for that tag, and a pointer to `CHANGELOG.md`. This page
is the procedure.

## Trigger

`.github/workflows/release.yml` runs on either:

- a **push of a `v*` tag**, or
- **`workflow_dispatch`**, which takes a `tag` input (it must already exist) and a `dry_run`
  boolean (default `false`).

A re-run is **idempotent by design**, and this is load-bearing: the first attempt usually fails
on one platform, and the fix is to re-run just that one. `publish` uploads with
`gh release upload --clobber`, and edits the Release rather than erroring if it already exists.

`dry_run: true` builds and packages everything and then reports what *would* be published,
without touching any public ref. Use it to rehearse.

## The six legs

| Job | Runner | Toolchain |
|---|---|---|
| `mingw-x64` | `windows-latest` | MSYS2 MINGW64 (GCC) |
| `mingw-arm64` | `windows-11-arm` | MSYS2 CLANGARM64 (clang, **libc++**, not libstdc++) |
| `msvc-x64` | `windows-2022` | Visual Studio 17 2022, x64 |
| `msvc-arm64` | `windows-11-arm` | Visual Studio 17 2022, arm64 |
| `linux-x64` | `ubuntu-22.04` | GCC |
| `linux-arm64` | `ubuntu-22.04-arm` | GCC |

`mingw-x64` is the primary artifact: its dependencies are fully static, so it runs on any 64-bit
Windows without shipping extra DLLs.

`mingw-arm64` is not "the same leg with a different `-A`". MSYS2 has no aarch64 GCC, so arm64
MinGW means clang in a libc++ + libunwind environment rather than libstdc++. Its self-containment
assertion matters more than x64's, not less.

Each leg builds a Release tree and a Debug tree, runs the tests, asserts the product's shape,
packages both into one zip, and uploads it as an artifact.

The four single-configuration legs build **two separate trees** and pass the second as
`--debug-tree` (`build-mingw` + `build-mingw-dbg`, `build-linux` + `build-linux-dbg`). The two
MSVC legs pass the **same** tree twice — `--tree build-msvc --config Release --debug-tree
build-msvc` — because the Visual Studio generator is multi-config and holds both configurations
in one tree. So `--debug-tree` does not imply a second build directory; it names where the Debug
artifacts can be found.

### The static recipe

Every leg configures with plain `cmake` and passes two switches that local development does not:

```
-DBUILD_SHARED_LIBS=OFF -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON
```

The first makes the `FetchContent` dependencies static (otherwise `llhttp.dll`); the second makes
libuv static by building it from source (otherwise the system import library gives you
`libuv-1.dll`). Passing only one still ships an extra DLL. There is no `.dll`-side symptom of
getting this wrong that a naive check would catch, which is why the assertion below is a
whitelist.

### Self-containment is asserted as a whitelist

The MinGW legs run `objdump -p` over the DLL and require every imported module to be **Windows
itself** — `KERNEL32`, `WS2_32`, `CRYPT32`, `api-ms-win-*` and so on. Anything outside that list
fails the leg.

It is a whitelist rather than a blacklist on purpose, and the reason is a real escape: an early
blacklist grepped for `libgcc|libstdc|libwinpthread` and **missed OpenSSL**, because MSYS2
installs both `libssl.a` and `libssl.dll.a` and `find_library` prefers the `.dll.a`. The build
quietly gained a `libssl-3-x64.dll` dependency and the assertion passed it. The two failure
directions are not symmetric: a whitelist that is too strict blocks a release (visible, one line
to fix), while a blacklist that is too loose ships a package that will not start on a clean
machine.

Both the Release **and** Debug trees are checked. They are two independent configures, so a
switch omitted from one produces an extra DLL in that one only — and it would be the Debug
build, the one a user reaches for exactly when they are already debugging something.

### Per-leg self-verification

Beyond the import whitelist, each leg asserts what only it can see:

- **MSVC arm64** asserts the generator and platform are exactly `Visual Studio 17 2022` /
  `arm64`. A drifting runner image would silently change the ABI and make this leg
  incompatible with `msvc-x64`.
- **MSVC legs** assert a list of `UVCPP_*` cache entries are `ON`. `find_package(OpenSSL QUIET)`
  silently sets `UVCPP_ENABLE_OPENSSL` to `OFF` when it cannot find OpenSSL, and the generated
  header then honestly reports `0` — so a package with no SSL at all still builds, passes its
  tests and packages cleanly. `.github/scripts/win-openssl-deps.sh` runs first specifically so
  the leg does not proceed without an OpenSSL: it either *finds* one (the image's own copy counts,
  and it names the path and version it found) or exits 1. A bare `choco install openssl` used to
  stand in for that, but it stopped working on both Windows images — see `doc/ci-guide.md` §6.
- **The ABI assertions use `sed -n`, not `grep`.** GitHub's `shell: bash` runs under `-eo
  pipefail`, so a `grep` with no match returns non-zero, `-e` aborts the whole step, and the
  `echo` / `::error` lines never execute — leaving a bare "exit code 1" with no explanation.

### What `package_release.py` does that `cmake --install` cannot

The packager **deliberately bypasses `cmake --install`** and reproduces the install layout by
hand. The reason is that libuv arrives via `FetchContent_MakeAvailable`, which registers install
rules referencing a `libuv.dll` that is never built — and those rules are ordered *before* this
project's. The install step therefore aborts as a whole and produces none of this project's
headers or libraries.

The packager also verifies its own output, and these checks are not decoration:

- **Debug artifacts carry CodeView.** Each Debug DLL must contain an `RSDS` debug directory
  naming its own `.pdb`; zero `RSDS` hits means `/DEBUG` did not take effect.
- **The `.pdb` matches the `.dll`.** A `.pdb` is matched by GUID + age, recorded in both the PE
  debug directory and the PDB stream. A mismatch means the `.pdb` belongs to a different build,
  and a debugger would either refuse it or silently use wrong symbols.
- **Headers get a BOM if they need one.** See
  [`build-guide.md`](build-guide.md#toolchain-requirements).

## What `publish` checks that no single leg can

`publish` needs all six legs, merges their artifacts, and runs one **cross-leg** gate: every zip
must contain a debug library (`uvcppd.*` / `libuvcppd.*`) and a `uvcpp-debug.pc`, and every
`-msvc-` zip must additionally contain `uvcppd.pdb`.

This exists because the packager cannot catch it. If a leg forgets `--debug-tree`, the packager
prints "this package does not contain a debug build" and **succeeds** — each package is verified
on its own leg, so nothing there notices that one package differs from the other five. The
result would be five platforms with debug artifacts and one without.

The MSVC `.pdb` requirement is stated separately because MinGW and Linux embed debug info in the
shared library and have no separate symbol file. Do not ask all six for the same shape.

Then it builds the Release body from the tag and creates or updates the Release:

```bash
python3 tests/tools/release_notes.py --tag "$TAG" --repo "$GITHUB_REPOSITORY" \
  --out "$RUNNER_TEMP/release-notes.md"
gh release create "$TAG" --title "libuvcpp $TAG" --notes-file "$RUNNER_TEMP/release-notes.md"
# or, if it already exists:
gh release edit "$TAG" --title "libuvcpp $TAG" --notes-file "$RUNNER_TEMP/release-notes.md"
gh release upload "$TAG" dist/*.zip --clobber
```

The slicing step runs **unconditionally**, so `dry_run` rehearses it too: the thing most likely
to be exercised for the first time on release day is the step that reads the tag. It exits `0`
when a body was produced, `1` when a criterion failed (no section for that tag; the header claims
to be a release but disagrees with the tag), and `3` when a premise is missing (`RELEASE.md`
unreadable, tag not of the form `vX.Y.Z`). **`3` means "not judged", not "green"** — as
everywhere else in `tests/tools/`, a non-zero exit fails the job and the distinction only matters
when reading the log.

Before 1.6.0 this step did not exist and `--notes-file RELEASE.md` published the **entire**
eight-hundred-line archive as every Release's body: a reader arriving at the page had to find
"what is new in this version" themselves, and the one thing they most wanted — which six packages
this version has, and where to click — was not in the file at all. `check_ci_layout.py`
criterion 13 fails if `release_notes.py` stops being invoked or if `--notes-file RELEASE.md`
comes back.

The version in the zip names and in `uvcpp.pc`'s `Version` field is read by the packager from
`src/uvcpp/uvcpp_version.h`. It is **not** written anywhere in the workflow — an early
`env: UVCPP_VERSION: '1.1.0'` sat in the file with zero references, still saying `1.1.0` when the
tag was `v1.1.5`, and was removed rather than left available as a false "source of truth".

## Cutting a release by hand

1. **Bump the major/minor and flip the release flag** in `src/uvcpp/uvcpp_version.h`:
   set `UVCPP_VERSION_MAJOR`/`MINOR`/`PATCH`, and set `UVCPP_VERSION_IS_RELEASE` to `1`.
2. **Update the four version strings in the two READMEs.** Each README carries the version twice
   — a shields badge and a bold Version / 版本 line.
3. **Run `python tests/tools/check_doc_versions.py`.** It asserts the README strings equal the
   one in the source tree, so steps 1 and 2 cannot drift apart unnoticed. It runs in CI on every
   push.
4. **Add this version's entry to `CHANGELOG.md`.** That file is the single changelog now — the
   two READMEs only point at it. Add the version to the per-version list at the end and, when
   the release folds in development lines, say so in the topic section each folded change
   belongs to.
5. **Add the new version's section to `RELEASE.md`** — a `## vX.Y.Z 重点 (Highlights)` heading.
   That section, and *only* that section, becomes the Release body, so write it to stand alone:
   it is what a reader sees directly under the download table, with no lead-in from the file's
   preamble. Everything at or below `## 以下为归档` is archive and never published. Rehearse the
   slice locally first — `python3 tests/tools/release_notes.py --tag vX.Y.Z | head -40` — and
   check it against the real `dist/*.zip` names rather than against the naming formula.
6. Commit, tag `vX.Y.Z`, and push the tag.

Between releases, the tree sits at `<next>-dev`: each feature push bumps the **patch** level and
keeps `UVCPP_VERSION_IS_RELEASE` at `0`, so the tree reports e.g. `1.2.1-dev`. The major/minor
bump and the `IS_RELEASE` flip happen together in the release commit.

**Re-configure after changing the version header.** `cmake --build` rebuilds nothing: the value
is read at configure time and the library does not include the header. Re-run
`cmake -S . -B <tree>`, and check the version string in the generated `uvcppConfig.cmake` rather
than the artifacts' timestamps.

## Known gaps

These are recorded rather than fixed. They are real, and a reader should not assume the process
covers them.

**Nothing asserts the tag matches the version header — almost.** Push `v1.3.0` against a tree
whose header says `1.2.0` and every leg builds happily, the packages are named `1.2.0`, and the
Release is published under the tag `v1.3.0`. Since 1.6.0 `release_notes.py` checks one direction:
if the header says it *is* a release (`UVCPP_VERSION_IS_RELEASE` non-zero) and its version
disagrees with `--tag`, it exits 1 and `publish` stops before creating anything.

The remaining hole is deliberate, not an oversight. The check is skipped when the header reports a
*development* tree, because `master` always carries the next version as `-dev` and re-publishing a
historical tag (`workflow_dispatch` with a tag that already exists, to reformat an old Release)
legitimately runs on exactly that tree — a stricter rule would block the path this document tells
you to take. The symptom still surfaces, one step later: the download table's URLs are built from
the tag while the asset names come from the header, so tagging `v1.6.0` over a `1.5.3-dev` tree
publishes a table of 404s.

`check_doc_versions.py` has a `--this-is-a-release` flag ("judge by release rules, ignoring the
header's flag") that would be the right tool for the strict form of this assertion, but **it has
no call sites** — not in CI, not in the workflows. It was added for manual use and never wired in.

**`--tag v1.0.0` cannot produce a body.** `RELEASE.md` has no `v1.0.0` section — that tag predates
the per-version sections, and it has no GitHub Release either — so `release_notes.py` exits 1 with
that reason. Every tag that *does* have a Release slices fine: `v1.1.0` through `v1.5.0` were each
verified against the live asset list, including `v1.1.0`, whose heading uses the older
`## 新增模块 (New in v1.1.0)` shape instead of `## vX.Y.Z 重点` (the tool matches on "the heading
contains the version marker", not on the heading's wording, for exactly this reason).

**macOS cannot run this chain.** `package_release.py` has no macOS key in its `PLATFORMS` table,
so there is no macOS leg and no macOS package. The library itself builds on macOS — see
[`build-guide.md`](build-guide.md#platform-dependencies) — but it is not released from here.

**`msvc-x64` is pinned to `windows-2022`, not `windows-latest`.** `windows-latest` now points at
a `windows-2025-vs2026` image containing only VS 2026, where the hard-coded
`-G "Visual Studio 17 2022"` fails outright. The alternative — dropping `-G` and letting CMake
choose — was rejected because it would tie the released ABI to GitHub's image rotation schedule.
This is a deliberate choice, not a defect to be tidied away.

**The two MSVC legs ship different OpenSSL major versions.** `msvc-x64` installs OpenSSL via
`choco install openssl` with no version pin, which currently yields 4.x; `msvc-arm64` builds
OpenSSL 3.5.8 from source because choco has no arm64 package. Choco does not carry 3.5.8, so the
two cannot be aligned through choco. This is **accepted status quo**, not a bug: the OpenSSL
major version is not part of this library's ABI, and the alternatives (vendoring, or dropping the
arm64 OpenSSL build) are worse. Note that the `OPENSSL_VERSION` cache variable is an empty shell
— the version in the log is whatever happened to be installed, not something the workflow
controls.

## See also

- [`RELEASE.md`](../RELEASE.md) — the release history, and the source each Release body is sliced from
- [`tests/tools/release_notes.py`](../tests/tools/release_notes.py) — the slicer; `--self-test` is its shape table
- [`build-guide.md`](build-guide.md) — the switches, including the static-build recipe
- [`testing-guide.md`](testing-guide.md) — what the legs' test steps actually run
- [`ci-guide.md`](ci-guide.md) — the CI job matrix for ordinary pushes
