#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一棵 `cmake --install` 前缀打成 .deb 与 .rpm。

输入的树**必须**是「发行版包」那棵：系统 libuv + 系统 zlib + SONAME（见
`packaging/README.md` 与 `CMakeLists.txt` 的 `UVCPP_SET_SOVERSION`）。发布 zip
那棵是自包含的，两者形状不同，不能混用。

用法：
    python3 packaging/nfpm/make_packages.py --stage /tmp/pkgstage/usr
    python3 packaging/nfpm/make_packages.py --stage ... --format rpm --arch arm64
    python3 packaging/nfpm/make_packages.py --check-tool

产物落到 `--out`（默认 `dist/`）：
    libuvcpp1_<ver>-1_<debarch>.deb       libuvcpp1-<ver>-1.<rpmarch>.rpm
    libuvcpp-dev_<ver>-1_<debarch>.deb    libuvcpp-devel-<ver>-1.<rpmarch>.rpm

退出码 0 成功，非 0 失败（不出残缺的包，错误信息在 stderr）。
"""

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# 不从包里发的东西：系统包负责（否则 dpkg 会因文件冲突拒绝安装）。
#   * include/nlohmann —— `nlohmann-json3-dev` / `json-devel`
#   * include/libuv   —— `libuv1-dev` / `libuv-devel`（公开头 `#include <uv.h>`）
#   * zlib.h/zconf.h  —— `zlib1g-dev` / `zlib-devel`
# 后两组只在「从源码建依赖」的树上才会出现；发行版包那棵树本来就不装它们，
# 这里仍然列出来：判据放在自己这一侧，不依赖构建开关今天的样子。
#   * llhttp 三样 —— 它是**私有**依赖：静态库本来就不进包（contents 只挑 .so、
#     公开头、cmake 配置与 pkgconfig），但 FetchContent 会把 `llhttp.h`、
#     `libllhttp.pc` 与 `lib/cmake/llhttp/` 一起 install 进 stage。照发的话
#     devel 包里会多一个 `.pc`，它指着一条不存在的 `-lllhttp`。
PRUNE = [
    "include/nlohmann",
    "include/libuv",
    "include/zlib.h",
    "include/zconf.h",
    "include/llhttp.h",
    "lib/pkgconfig/libllhttp.pc",
    "lib/cmake/llhttp",
]

MAINTAINER = "Antruly <antruly@users.noreply.github.com>"
HOMEPAGE = "https://github.com/Antruly/libuvcpp"


def die(msg, code=2):
    sys.stderr.write("错误：%s\n" % msg)
    raise SystemExit(code)


def read_version():
    p = os.path.join(ROOT, "src", "uvcpp", "uvcpp_version.h")
    with open(p, encoding="utf-8") as f:
        s = f.read()

    def num(macro):
        m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % macro, s, re.M)
        if not m:
            die("读不到 %s（%s）" % (macro, p))
        return m.group(1)

    return "%s.%s.%s" % (num("UVCPP_VERSION_MAJOR"),
                         num("UVCPP_VERSION_MINOR"),
                         num("UVCPP_VERSION_PATCH"))


def read_pin():
    p = os.path.join(HERE, "nfpm.pin")
    pin = {}
    with open(p, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            pin[k.strip()] = v.strip()
    for k in ("version", "base_url"):
        if k not in pin:
            die("%s 缺少 %s" % (p, k))
    return pin


def host_triple():
    m = platform.machine().lower()
    if m in ("x86_64", "amd64"):
        return "x86_64"
    if m in ("aarch64", "arm64"):
        return "arm64"
    die("不支持的构建机架构：%s" % m)


def ensure_nfpm(pin):
    """返回 nfpm 可执行文件路径。优先 $NFPM，其次缓存，最后按 pin 下载。"""
    env = os.environ.get("NFPM")
    if env:
        if not os.access(env, os.X_OK):
            die("NFPM=%s 不可执行" % env)
        return env

    triple = host_triple()
    ver = pin["version"]
    cache = os.path.join(os.path.expanduser("~"), ".cache", "libuvcpp-nfpm", ver)
    exe = os.path.join(cache, "nfpm")
    if os.access(exe, os.X_OK):
        return exe

    key = "linux_%s_sha256" % triple
    if key not in pin:
        die("nfpm.pin 缺少 %s" % key)
    want = pin[key]
    name = "nfpm_%s_Linux_%s.tar.gz" % (ver, triple)
    url = "%s/%s" % (pin["base_url"], name)

    os.makedirs(cache, exist_ok=True)
    tmp = os.path.join(cache, name + ".part")
    sys.stderr.write("下载 %s\n" % url)
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)

    h = hashlib.sha256()
    with open(tmp, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    got = h.hexdigest()
    if got != want:
        os.unlink(tmp)
        die("%s 的 sha256 不符：期望 %s，得到 %s" % (name, want, got))

    # 解包：只取 `nfpm` 这一个成员，不展开整棵归档。
    with tarfile.open(tmp, "r:gz") as tf:
        try:
            member = tf.getmember("nfpm")
        except KeyError:
            die("%s 里没有 nfpm 可执行文件" % name)
        if not member.isfile():
            die("%s 里的 nfpm 不是普通文件" % name)
        with tf.extractfile(member) as src, open(exe, "wb") as dst:
            shutil.copyfileobj(src, dst)
    os.unlink(tmp)
    os.chmod(exe, 0o755)
    return exe


def prepare_tree(stage, work):
    """复制 stage 到 work/full 并剪掉系统包负责的头。返回 full 目录。"""
    full = os.path.join(work, "full")
    shutil.copytree(stage, full, symlinks=True)
    for rel in PRUNE:
        p = os.path.join(full, rel)
        if os.path.islink(p) or os.path.isfile(p):
            os.unlink(p)
        elif os.path.isdir(p):
            shutil.rmtree(p)
    return full


def check_stage(full, version, major):
    lib = os.path.join(full, "lib")
    real = os.path.join(lib, "libuvcpp.so.%s" % version)
    link = os.path.join(lib, "libuvcpp.so.%s" % major)
    if not os.path.isfile(real):
        die("stage 里没有 %s —— 这棵树没开 UVCPP_SET_SOVERSION，或版本不对" % real)
    if not os.path.islink(link):
        die("stage 里没有符号链接 %s" % link)
    if not os.path.exists(os.path.join(lib, "libuvcpp.so")):
        die("stage 里没有 dev 符号链接 %s" % os.path.join(lib, "libuvcpp.so"))


def yaml_common(name, version, arch, description, depends, section):
    lines = [
        "name: %s" % name,
        "arch: %s" % arch,
        "platform: linux",
        "version: %s" % version,
        'release: "1"',
        "section: %s" % section,
        "priority: optional",
        "maintainer: %s" % MAINTAINER,
        "vendor: Antruly",
        "homepage: %s" % HOMEPAGE,
        "license: MIT",
        "description: %s" % description,
    ]
    lines.append("depends:")
    for d in depends:
        lines.append("  - %s" % d)
    lines.append("contents:")
    return lines


def write_yaml(path, lines, contents):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines + contents) + "\n")


def run_nfpm(nfpm, packager, cfg, target):
    cmd = [nfpm, "package", "--packager", packager, "--config", cfg,
           "--target", target]
    subprocess.check_call(cmd)


def specs_for(fmt, version, arch):
    """返回该格式下两个包的 (name, section, depends, target_name)。"""
    evr = "%s-1" % version            # 发行版修订号，重打包时 +1
    if fmt == "deb":
        runtime_deps = [
            "libc6 (>= 2.34)",
            "libstdc++6 (>= 11)",
            "libgcc-s1",
            "libuv1 (>= 1.44.0)",
            "zlib1g",
        ]
        devel_deps = [
            "libuvcpp1 (= %s)" % evr,
            "libuv1-dev",
            "zlib1g-dev",
            "nlohmann-json3-dev",
        ]
        return [
            ("libuvcpp1", "libs", runtime_deps,
             "libuvcpp1_%s_%s.deb" % (evr, arch)),
            ("libuvcpp-dev", "libdevel", devel_deps,
             "libuvcpp-dev_%s_%s.deb" % (evr, arch)),
        ]

    runtime_deps = ["glibc", "libstdc++", "libgcc", "libuv", "zlib"]
    devel_deps = [
        "libuvcpp1 = %s" % evr,
        "libuv-devel",
        "zlib-devel",
        "json-devel",
    ]
    rpmarch = {"amd64": "x86_64", "arm64": "aarch64"}[arch]
    return [
        ("libuvcpp1", "libs", runtime_deps,
         "libuvcpp1-%s.%s.rpm" % (evr, rpmarch)),
        ("libuvcpp-devel", "devel", devel_deps,
         "libuvcpp-devel-%s.%s.rpm" % (evr, rpmarch)),
    ]


def contents_for(fmt, version, major, full):
    """两个包的 contents 段。libdir 按格式分开：rh 系是 /usr/lib64。"""
    libdir = "/usr/lib64" if fmt == "rpm" else "/usr/lib"
    # 符号链接的 src 写**裸文件名**，不写绝对路径：nfpm 原样把 src 当链接目标，
    # 给绝对路径就得到一个绝对符号链接 —— 在 usrmerge 的发行版上指向 /usr/lib
    # 之外时直接是断的，而 dpkg-deb -c 里那一行看起来完全正常。
    runtime = [
        "  - src: %s/lib/libuvcpp.so.%s" % (full, version),
        "    dst: %s/libuvcpp.so.%s" % (libdir, version),
        "  - src: libuvcpp.so.%s" % version,
        "    dst: %s/libuvcpp.so.%s" % (libdir, major),
        "    type: symlink",
    ]
    devel = [
        "  - src: %s/include" % full,
        "    dst: /usr/include",
        "    type: tree",
        "  - src: %s/lib/cmake" % full,
        "    dst: %s/cmake" % libdir,
        "    type: tree",
        "  - src: %s/lib/pkgconfig" % full,
        "    dst: %s/pkgconfig" % libdir,
        "    type: tree",
        "  - src: libuvcpp.so.%s" % major,
        "    dst: %s/libuvcpp.so" % libdir,
        "    type: symlink",
    ]
    return runtime, devel


def build(version, full, nfpm, out, arch, formats):
    major = version.split(".")[0]
    desc = "Modern C++ wrapper for libuv"
    made = []

    for fmt in formats:
        runtime_contents, devel_contents = contents_for(fmt, version, major, full)
        for i, (name, section, deps, target_name) in enumerate(
                specs_for(fmt, version, arch)):
            contents = runtime_contents if i == 0 else devel_contents
            cfg = os.path.join(os.path.dirname(full), "%s-%s.yaml" % (name, fmt))
            write_yaml(cfg, yaml_common(name, version, arch, desc, deps, section),
                       contents)
            target = os.path.join(out, target_name)
            run_nfpm(nfpm, fmt, cfg, target)
            if not os.path.isfile(target):
                die("nfpm 没有产出 %s" % target)
            made.append(target)

    return made


def report(paths):
    """用系统工具读回包字段（没有工具就跳过，不假装验过）。"""
    for p in paths:
        print("\n== %s (%d 字节)" % (os.path.basename(p), os.path.getsize(p)))
        if p.endswith(".deb") and shutil.which("dpkg-deb"):
            out = subprocess.check_output(
                ["dpkg-deb", "-f", p, "Package", "Version", "Architecture",
                 "Depends"], text=True)
            print(out.rstrip())
        elif p.endswith(".rpm") and shutil.which("rpm"):
            out = subprocess.check_output(
                ["rpm", "-qp", "--qf",
                 "%{NAME} %{EVR} %{ARCH}\nRequires: %{REQUIRENAME}\n", p],
                text=True)
            print(out.rstrip())
        else:
            print("（本机没有对应的查询工具，未读回字段）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", help="cmake --install 的前缀根（含 include/ 与 lib/）")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--arch", default=None,
                    help="deb 架构名：amd64 / arm64（默认取本机）")
    ap.add_argument("--format", default="all", choices=("deb", "rpm", "all"))
    ap.add_argument("--nfpm", default=None, help="用指定的 nfpm（否则按 pin 取）")
    ap.add_argument("--check-tool", action="store_true",
                    help="只确保 nfpm 可用，不打包")
    args = ap.parse_args()

    pin = read_pin()
    nfpm = args.nfpm or ensure_nfpm(pin)
    if args.check_tool:
        out = subprocess.check_output([nfpm, "--version"], text=True).strip()
        print("nfpm 可用：%s（pin %s）" % (out, pin["version"]))
        return

    if not args.stage:
        die("需要 --stage")
    stage = os.path.abspath(args.stage)
    if not os.path.isdir(os.path.join(stage, "lib")):
        die("%s 下没有 lib/ —— 不是 cmake --install 的前缀" % stage)

    version = read_version()
    major = version.split(".")[0]
    arch = args.arch or {"x86_64": "amd64", "amd64": "amd64",
                         "aarch64": "arm64", "arm64": "arm64"}.get(
                             platform.machine().lower())
    if arch not in ("amd64", "arm64"):
        die("--arch 只支持 amd64 / arm64（得到 %r）" % arch)

    formats = ("deb", "rpm") if args.format == "all" else (args.format,)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    work = tempfile.mkdtemp(prefix="uvcpp-pkg-")
    try:
        full = prepare_tree(stage, work)
        check_stage(full, version, major)
        made = build(version, full, nfpm, out, arch, formats)
        report(made)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
