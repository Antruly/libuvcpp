#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把一棵构建树组装成发布 zip。

为什么不走 `cmake --install`：libuv 是 `FetchContent_MakeAvailable` 拉进来的，
它登记了引用 `libuv.dll` 的 install 规则（那个 dll 从没被构建过），而且它的规则
排在本项目的前面 —— 一失败就整体中止，本项目的头和库一个都装不出来。所以这里
照 `CMakeLists.txt` 末尾的 install 规则手工复刻同一套布局，并保留两处**刻意的**
差异（见下）。

用法：
    python tests/tools/package_release.py --tree build-mingw --platform mingw-x64
    python tests/tools/package_release.py --tree build-msvc --platform msvc-x64 \\
        --config Release --out dist

产物：`<out>/<name>/` 与 `<out>/<name>.zip`。
退出码 0 成功；2 缺少必需的产物文件（**不产出残缺的包**）。
"""

import argparse
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))  # tests/tools -> tests -> repo

VERSION = "1.1.0"

# 各模块的公开头目录。expand 现在也要装 —— 内存池已修复，发布产物带池
# （见 RELEASE.md），使用者需要 uvcpp_page_heap.h 才能用 uvcpp_alloc。
MODULES = ["uvcpp", "handle", "req", "expand", "net", "web", "webapp", "ssl"]

# 每个平台一份产物描述：从构建树里的**哪些路径**取**哪些文件**。
# `lib` 是导入库/静态库，`runtime` 是要跟着 dll 一起发的第三方运行时。
PLATFORMS = {
    "mingw-x64": {
        "lib_dll": ["libuvcpp.dll"],
        "import_lib": ["libuvcpp.dll.a"],
        "runtime": [],          # 运行时已静态链进 dll
        "pc_libs": "-L${libdir} -luvcpp",
    },
    "msvc-x64": {
        "lib_dll": ["uvcpp.dll", "Release/uvcpp.dll"],
        "import_lib": ["uvcpp.lib", "Release/uvcpp.lib"],
        # MSVC 用 /MD：发布包里必须自带运行库 dll，否则使用者机器上
        # 没有 VC++ 可再发行组件时直接 0xc0000135。
        "runtime": ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"],
        "pc_libs": "-L${libdir} -luvcpp",
    },
    "linux-x64": {
        "lib_dll": ["libuvcpp.so", "libuvcpp.so.1.1.0"],
        "import_lib": [],
        "runtime": [],
        "pc_libs": "-L${libdir} -luvcpp",
    },
}


def find_first(tree, rels):
    for r in rels:
        p = os.path.join(tree, r)
        if os.path.exists(p):
            return p
    return None


def find_dir(candidates):
    for c in candidates:
        if c and os.path.isdir(c):
            return c
    return None


def dep_src(tree, repo, name):
    """定位第三方依赖的源码目录。

    本机用 `_local_deps/`（FETCHCONTENT_SOURCE_DIR_* 指过去），CI 上
    FetchContent 自己下到 `<tree>/_deps/<name>-src`。两处都要找。
    """
    return find_dir([
        os.path.join(tree, "_deps", name + "-src"),
        os.path.join(repo, "_local_deps", name),
    ])


def copytree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True, help="构建树目录")
    ap.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    ap.add_argument("--version", default=VERSION)
    ap.add_argument("--config", default="", help="多配置生成器的配置名（MSVC 传 Release）")
    ap.add_argument("--out", default="dist", help="stage 与 zip 的输出目录")
    ap.add_argument("--repo", default=ROOT)
    args = ap.parse_args()

    tree = os.path.abspath(args.tree)
    repo = os.path.abspath(args.repo)
    spec = PLATFORMS[args.platform]
    name = "libuvcpp-%s-%s" % (args.version, args.platform)
    stage = os.path.join(os.path.abspath(args.out), name)

    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(os.path.join(stage, "bin"))
    os.makedirs(os.path.join(stage, "lib", "pkgconfig"))

    missing = []

    # ---- bin / lib ----
    dll = find_first(tree, spec["lib_dll"])
    if dll is None:
        missing.append("库 dll (%s)" % " / ".join(spec["lib_dll"]))
    else:
        shutil.copy2(dll, os.path.join(stage, "bin"))
        print("bin: %s" % os.path.basename(dll))

    imp = find_first(tree, spec["import_lib"]) if spec["import_lib"] else None
    if spec["import_lib"] and imp is None:
        missing.append("导入库 (%s)" % " / ".join(spec["import_lib"]))
    elif imp:
        shutil.copy2(imp, os.path.join(stage, "lib"))
        print("lib: %s" % os.path.basename(imp))

    # ---- 第三方运行时 dll ----
    # MSVC 的运行库在 `$env:VCToolsRedistDir` 下，这里按常见位置找；
    # 找不到就**报错退出**，因为缺了它这个包在干净机器上根本起不来 ——
    # 悄悄发一个"看着完整"的包比不发更坏。
    for rt in spec["runtime"]:
        src = None
        for base in filter(None, [os.environ.get("VCToolsRedistDir"),
                                  os.environ.get("VCINSTALLDIR")]):
            for root, _dirs, files in os.walk(base):
                if rt in files and ("x64" in root or "x86" not in root):
                    src = os.path.join(root, rt)
                    break
            if src:
                break
        if src is None:
            # VCToolsRedistDir 只在开发者命令提示符里才有；CI 的 bash 步里
            # 常常是空的。按 VS 的固定布局再找一遍，免得每换一台机器就发不出包。
            for base in (r"C:\Program Files\Microsoft Visual Studio",
                         r"C:\Program Files (x86)\Microsoft Visual Studio"):
                if not os.path.isdir(base):
                    continue
                for root, _dirs, files in os.walk(base):
                    if rt in files and os.sep + "x64" + os.sep in root + os.sep:
                        src = os.path.join(root, rt)
                        break
                if src:
                    break
        if src:
            shutil.copy2(src, os.path.join(stage, "bin"))
            print("bin: %s" % rt)
        else:
            missing.append("MSVC 运行库 %s" % rt)

    # ---- 公开头 ----
    for m in MODULES:
        d = os.path.join(repo, "src", m)
        if os.path.isdir(d):
            dst = os.path.join(stage, "include", m)
            os.makedirs(dst, exist_ok=True)
            for f in os.listdir(d):
                if f.endswith(".h"):
                    shutil.copy2(os.path.join(d, f), dst)
    shutil.copy2(os.path.join(repo, "src", "uvcpp.h"), os.path.join(stage, "include"))
    print("include: %d 个模块" % len([m for m in MODULES
                                      if os.path.isdir(os.path.join(repo, "src", m))]))

    # ---- 第三方头 ----
    # 差异 1：libuv 的头放 include/ **顶层**。本库的公开头写的是 `#include <uv.h>`，
    # 放进 include/libuv/ 使用者就找不到（install 规则里那句 DESTINATION
    # include/libuv 与自己的头不自洽 —— 这也是手工组装偏离 install 规则的原因之一）。
    libuv = dep_src(tree, repo, "libuv")
    if libuv:
        inc = os.path.join(libuv, "include")
        for f in os.listdir(inc):
            s = os.path.join(inc, f)
            if os.path.isfile(s) and f.endswith(".h"):
                shutil.copy2(s, os.path.join(stage, "include"))
            elif os.path.isdir(s):
                copytree(s, os.path.join(stage, "include", f))
    else:
        missing.append("libuv 头文件目录")

    for jname, jsub in (("json-3113", "nlohmann"), ("nlohmann_json", "nlohmann")):
        j = dep_src(tree, repo, jname)
        if j:
            src = find_dir([os.path.join(j, "include", jsub)])
            if src:
                copytree(src, os.path.join(stage, "include", jsub))
                break

    # 差异 2：补 zlib.h / zconf.h。`web/uvcpp_ws_parser.h` 在
    # UVCPP_ZLIB_ENABLE=1 时要 include 它，但 install 规则里没有这一条。
    z = dep_src(tree, repo, "zlib-131") or dep_src(tree, repo, "zlib")
    if z:
        for f in ("zlib.h", "zconf.h"):
            p = os.path.join(z, f)
            if os.path.exists(p):
                shutil.copy2(p, os.path.join(stage, "include"))
        # FetchContent 的 zlib 把 zconf.h 生成到构建目录
        p = os.path.join(tree, "_deps", "zlib-build", "zconf.h")
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(stage, "include"))

    # ---- pkg-config ----
    # 宏必须以编译定义的形式传给使用者：公开头里的 #if UVCPP_*_ENABLE 在宏未定义
    # 时求值为 0，会把 web/webapp/ssl 的类整段编译掉；而 UVCPP_ENABLE_MEMORY_POOL
    # 选错分支会让使用者 TU 的分配器与已编译的 dll 不是同一套（静默堆损坏）。
    pc = os.path.join(stage, "lib", "pkgconfig", "uvcpp.pc")
    with open(pc, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "# uvcpp 的模块使能宏必须显式传给编译器（见 RELEASE.md）。\n"
            "prefix=${pcfiledir}/../..\n"
            "exec_prefix=${prefix}\n"
            "libdir=${prefix}/lib\n"
            "includedir=${prefix}/include\n"
            "\n"
            "Name: uvcpp\n"
            "Description: Modern C++ wrapper for libuv (prebuilt, %s)\n"
            "Version: %s\n"
            "Cflags: -I${includedir} -DUVCPP_NET_ENABLE=1 -DUVCPP_WEB_ENABLE=1"
            " -DUVCPP_WEBAPP_ENABLE=1 -DUVCPP_OPENSSL_ENABLE=1"
            " -DUVCPP_ZLIB_ENABLE=1 -DUVCPP_ENABLE_MEMORY_POOL=1\n"
            "Libs: %s\n" % (args.platform, args.version, spec["pc_libs"]))

    # ---- 文档 ----
    for f in ("README.md", "RELEASE.md", "LICENSE"):
        p = os.path.join(repo, f)
        if os.path.exists(p):
            shutil.copy2(p, stage)

    if missing:
        print("\n**缺少必需的文件，不产出残缺的包**：")
        for m in missing:
            print("  - %s" % m)
        return 2

    # ---- 打包 ----
    # 手写 zipfile 而不是 Compress-Archive：后者只在 Windows 上有，
    # 而且给条目加的是反斜杠路径分隔符，unzip 解出来是一个平铺的怪名字。
    zip_path = os.path.join(os.path.abspath(args.out), name + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _dirs, files in os.walk(stage):
            for f in sorted(files):
                full = os.path.join(root, f)
                z.write(full, os.path.relpath(full, os.path.dirname(stage)))

    size = os.path.getsize(zip_path)
    n = sum(len(fs) for _r, _d, fs in os.walk(stage))
    print("\n%s  (%d 个文件, %.1f KiB)" % (zip_path, n, size / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
