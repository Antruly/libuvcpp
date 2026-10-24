#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Assemble a portable libuvcpp build kit.

Layout of the produced zip:

    libuvcpp-<version>-<target>/
        CMakeLists.txt, src/, cmake/, build-kit/, ...
        deps/                 prebuilt dependency prefix
        KIT-README.txt        how to run build-kit/build.cmd

The dependency prefix is produced by CI (AppVeyor) or by the maintainer with
build-kit/collect-deps.ps1. End users only run build-kit/build.cmd <target>;
the kit already carries OpenSSL 1.1.1w, libuv 1.33/1.40 (CVE-patched), zlib and
llhttp.

Usage:
    python packaging/make_build_kit.py --deps kit-deps --target vs2015-x86 \
        --out dist/kit-vs2015-x86.zip
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

# Top-level directories that are never part of a kit.
SKIP_DIRS = {
    ".git", ".github", ".vscode", ".idea",
    "_backup_release_20260918", "_local_deps", "_probe",
    "dist", "dist-msvc", "dist-120", "Testing", "out", "stage",
    "deps", "deps-src", "kit-deps", "deps-kit", "stage",
    "prebuilt", "docs",
}
SKIP_PREFIXES = ("build", "build-", "build_", "stage-")  # build trees; keep build-kit
# Dependency build trees kept under their upstream name: libuv-build64/, zlib-build64/.
SKIP_INFIX = re.compile(r"[-_](build|obj|install)", re.I)
SKIP_NAMES = {"stderr.txt", "stdout.txt"}
# Local linker/compiler output that must not ride along at the top level.
SKIP_SUFFIXES = (".exe", ".obj", ".dll", ".lib", ".exp", ".pdb", ".ilk",
                 ".zip", ".7z", ".tar", ".gz", ".log")


def version_of(source):
    p = os.path.join(source, "src", "uvcpp", "uvcpp_version.h")
    try:
        text = open(p, encoding="utf-8-sig", errors="ignore").read()
        maj = re.search(r'^#define\s+UVCPP_VERSION_MAJOR\s+(\d+)', text, re.M)
        minor = re.search(r'^#define\s+UVCPP_VERSION_MINOR\s+(\d+)', text, re.M)
        patch = re.search(r'^#define\s+UVCPP_VERSION_PATCH\s+(\d+)', text, re.M)
        if maj and minor and patch:
            return "%s.%s.%s" % (maj.group(1), minor.group(1), patch.group(1))
    except OSError:
        pass
    return "unknown"


def should_skip_dir(name):
    if name == "build-kit":
        return False
    if name in SKIP_DIRS:
        return True
    if any(name == p or name.startswith(p) for p in SKIP_PREFIXES):
        return True
    return bool(SKIP_INFIX.search(name))


def should_skip_file(name):
    if name in SKIP_NAMES:
        return True
    if name.endswith(".zip") and name.startswith(("uvcpp-win7-", "uvcpp-kit-")):
        return True
    return os.path.splitext(name)[1].lower() in SKIP_SUFFIXES


def git_source_files(source):
    """Paths a clone of this tree would contain.

    `--cached` is the index (tracked files, read from the working tree, so
    uncommitted edits are included), `--others --exclude-standard` adds files
    that are new but not ignored. Anything the packager should not ship — build
    trees, dist zips, test residue — is either ignored or filtered below.
    Returns None when the source is not a git work tree, in which case the
    caller falls back to a plain directory walk.
    """
    try:
        proc = subprocess.run(
            ["git", "-C", source, "ls-files", "-z", "--cached", "--others",
             "--exclude-standard"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    text = proc.stdout.decode("utf-8", "surrogateescape")
    return [p for p in text.split("\0") if p]


def copy_source(source, dest):
    rels = git_source_files(source)
    if rels is not None:
        for rel in sorted(rels):
            top, sep, _ = rel.partition("/")
            if sep and should_skip_dir(top):
                continue
            if not sep and should_skip_file(top):
                continue
            src = os.path.join(source, rel)
            if not os.path.isfile(src):
                continue  # submodule entry or a file staged after deletion
            dst = os.path.join(dest, rel)
            parent = os.path.dirname(dst)
            if parent:
                os.makedirs(parent, exist_ok=True)
            shutil.copy2(src, dst)
        return
    for name in sorted(os.listdir(source)):
        if should_skip_file(name):
            continue
        src = os.path.join(source, name)
        dst = os.path.join(dest, name)
        if os.path.isdir(src):
            if should_skip_dir(name):
                continue
            shutil.copytree(src, dst, symlinks=True)
        else:
            shutil.copy2(src, dst)


def copy_deps(deps, dest):
    if not os.path.isdir(deps):
        raise SystemExit("--deps is not a directory: %s" % deps)
    shutil.copytree(deps, dest, symlinks=True)


def write_readme(dest, version, target, has_prebuilt):
    # VS2015's own CRT/STL predates the _mm_loadu_si64 intrinsic that a newer
    # Windows SDK's <wchar.h> uses, so both the library build and anything that
    # includes <cstdio> need the shim's force-include.
    shim_note = ""
    if target.startswith("vs2015"):
        shim_note = (
            "Consuming the result with VS2015 on a newer Windows SDK:\n"
            "  cl ... /FI\"<kit>\\build-kit\\msvc2015-sdk-compat.h\"\n"
            "The kit uses the same shim when it builds the library.\n\n")
    text = """libuvcpp {version} portable build kit - {target}

This archive already contains the pinned dependency prefix under deps/:
  * libuv (1.33.0 for VS2013, 1.40.0 for VS2015/MinGW; CVE-2024-24806 patched)
  * OpenSSL 1.1.1w (static)
  * zlib 1.3.1 (when the profile needs gzip)
  * llhttp 9.2.0 source (built by CMake)

Build with the compiler for this kit:

  build-kit\\build.cmd {target}

The output is under out\\{target}\\. You need the matching compiler installed:
  * vs2013-x86 : Visual Studio 2013 (v120), x86
  * vs2015-x86 : Visual Studio 2015 Update 3 (v140), x86
  * vs2015-x64 : Visual Studio 2015 Update 3 (v140), x64
  * mingw32    : the deps\\mingw32 toolchain in this kit (GCC 8.1)
  * mingw64    : the deps\\mingw64 toolchain in this kit (GCC 8.1)

{prebuilt_note}{shim_note}Profiles: net, net-ssl, web-lite, web, full.
Default for this kit: {default_profile}.
See doc/compatibility.md for the feature/toolchain matrix.
""".format(version=version, target=target,
           prebuilt_note=("A ready-to-use build of this exact kit is under prebuilt\\.\n"
                          "You can consume it directly instead of running the compiler.\n\n")
                         if has_prebuilt else "",
           shim_note=shim_note,
           default_profile="web-lite" if target == "vs2013-x86" else "web")
    with open(os.path.join(dest, "KIT-README.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def zip_tree(root, out, arc_root):
    out_dir = os.path.dirname(os.path.abspath(out))
    os.makedirs(out_dir, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, allowZip64=True) as z:
        for base, dirs, files in os.walk(root):
            dirs.sort()
            for f in sorted(files):
                full = os.path.join(base, f)
                rel = os.path.relpath(full, root)
                z.write(full, os.path.join(arc_root, rel))


def main():
    if sys.version_info[0] < 3:
        raise SystemExit("Python 3 is required")
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
    ap.add_argument("--deps", required=True)
    ap.add_argument("--target", required=True,
                    choices=["vs2013-x86", "vs2015-x86", "vs2015-x64", "mingw32", "mingw64"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--prebuilt", default="", help="optional staged install to ship as prebuilt/")
    ap.add_argument("--version", default="")
    args = ap.parse_args()

    source = os.path.abspath(args.source)
    deps = os.path.abspath(args.deps)
    version = args.version or version_of(source)
    arc_root = "libuvcpp-%s-%s" % (version, args.target)

    tmp = tempfile.mkdtemp(prefix="uvcpp-kit-")
    try:
        stage = os.path.join(tmp, arc_root)
        os.makedirs(stage)
        copy_source(source, stage)
        copy_deps(deps, os.path.join(stage, "deps"))
        has_prebuilt = bool(args.prebuilt) and os.path.isdir(args.prebuilt)
        if has_prebuilt:
            shutil.copytree(args.prebuilt, os.path.join(stage, "prebuilt"), symlinks=True)
        write_readme(stage, version, args.target, has_prebuilt)
        zip_tree(stage, args.out, arc_root)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("wrote %s" % args.out)
    print("  root: %s" % arc_root)


if __name__ == "__main__":
    main()
