## What this changes

<!-- One sentence: what it did before, what it does now. -->

## Why

<!-- The actual defect or gap. If it fixes a bug, say what triggers it. -->

## How it was checked

<!-- The commands you ran and what they printed. "Tested" is not evidence;
     a command, a run id or a count is. See CONTRIBUTING.md. -->

- [ ] `python3 tests/tools/check_docs.py`
- [ ] `python3 tests/tools/check_doc_versions.py`
- [ ] `python3 tests/tools/check_doc_lines.py`
- [ ] `python3 tests/tools/check_ci_layout.py` (only if `.github/` changed)
- [ ] CTest, if code changed — and the exact filter you ran

Remember the exit codes: `0` every criterion ran and passed, `1` a criterion is
red, and `3` a criterion **never ran** because its premise was missing. `3` is
not green — say so rather than reporting the run as passing.

## Conventions

- [ ] New source files are UTF-8 **with** a BOM and LF; new documentation and
      `tests/tools/*.py` are UTF-8 **without** a BOM and LF (CONTRIBUTING.md).
- [ ] Nothing was staged with `git add -A` or committed with `git commit -a`.
- [ ] If a public header, a README CMake options table, or the CI matrix
      changed, the gate that reads it was updated in the same commit.
- [ ] The version string moved in `src/uvcpp/uvcpp_version.h` only — the four
      README anchors move with a release, not with a feature.

<!-- Chinese is welcome in this description. Commit messages are Chinese
     throughout (CONTRIBUTING.md). -->
