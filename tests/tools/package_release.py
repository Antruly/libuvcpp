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
import re
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))  # tests/tools -> tests -> repo

def _header_version():
    """版本号的唯一来源是 `src/uvcpp/uvcpp_version.h`（`CMakeLists.txt:472-480`
    在 configure 期读的也是它）。这里以前写死 "1.1.0"：标签打到 v1.1.5 时，产出的
    zip 名和 uvcpp.pc 的 Version 仍然自称 1.1.0 —— 两者只差一个字符串，出包时谁也
    不会去核对。读不到就停下，不自作主张退回默认值：一个名字说谎的包比不出包更坏。
    """
    p = os.path.join(ROOT, "src", "uvcpp", "uvcpp_version.h")
    with open(p, encoding="utf-8") as f:
        s = f.read()

    def num(macro):
        m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % macro, s, re.M)
        if not m:
            raise SystemExit("读不到 %s（%s）—— 版本号来源变了，先修这里" % (macro, p))
        return m.group(1)

    return "%s.%s.%s" % (num("UVCPP_VERSION_MAJOR"),
                         num("UVCPP_VERSION_MINOR"),
                         num("UVCPP_VERSION_PATCH"))


VERSION = _header_version()

# 各模块的公开头目录。expand 现在也要装 —— 内存池已修复，发布产物带池
# （见 RELEASE.md），使用者需要 uvcpp_page_heap.h 才能用 uvcpp_alloc。
MODULES = ["uvcpp", "handle", "req", "expand", "net", "web", "webapp", "ssl", "http2"]

# 不发的头：`uvcpp_h2_nghttp2.h` 把 `<nghttp2/nghttp2.h>` 拉进来（这是它存在的
# 全部理由 —— 让别的头不用拉），装出去就把"使用者不需要 nghttp2"这个结论作废了，
# 而且使用者根本没装 nghttp2 的头，一 include 就是硬编译错误。
# CMakeLists.txt:985-986 的 install 规则里同一条排除，两处必须一起改。
PRIVATE_HEADERS = {"uvcpp_h2_nghttp2.h"}

# 每个平台一份产物描述：从构建树里的**哪些路径**取**哪些文件**。
# `lib` 是导入库/静态库，`runtime` 是要跟着 dll 一起发的第三方运行时。
PLATFORMS = {
    "mingw-x64": {
        "lib_dll": ["libuvcpp.dll"],
        "import_lib": ["libuvcpp.dll.a"],
        "runtime": [],          # 运行时已静态链进 dll
        "pc_libs": "-L${libdir} -luvcpp",
    },
    # 与 mingw-x64 逐字同构：MSYS2 的 `lib` 前缀是工具链决定的，与架构无关
    # （CLANGARM64 同样产出 libuvcpp.dll + libuvcpp.dll.a）。
    "mingw-arm64": {
        "lib_dll": ["libuvcpp.dll"],
        "import_lib": ["libuvcpp.dll.a"],
        "runtime": [],
        "pc_libs": "-L${libdir} -luvcpp",
    },
    "msvc-x64": {
        "lib_dll": ["uvcpp.dll", "Release/uvcpp.dll"],
        "import_lib": ["uvcpp.lib", "Release/uvcpp.lib"],
        # MSVC 用 /MD：发布包里必须自带运行库 dll，否则使用者机器上
        # 没有 VC++ 可再发行组件时直接 0xc0000135。
        "runtime": ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"],
        "pc_libs": "-L${libdir} -luvcpp",
        "msvc_arch": "x64",
    },
    "msvc-arm64": {
        "lib_dll": ["uvcpp.dll", "Release/uvcpp.dll"],
        "import_lib": ["uvcpp.lib", "Release/uvcpp.lib"],
        "runtime": ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"],
        "pc_libs": "-L${libdir} -luvcpp",
        "msvc_arch": "arm64",
    },
    # 无 SOVERSION/VERSION（根 CMakeLists 一处都没设），所以实体就叫 libuvcpp.so，
    # 没有 `.so.1` / `.so.1.1.0` 符号链接链。x64 与 arm64 同名，只有内容不同。
    "linux-x64": {
        "lib_dll": ["libuvcpp.so", "libuvcpp.so.1.1.0"],
        "import_lib": [],
        "runtime": [],
        "pc_libs": "-L${libdir} -luvcpp",
    },
    "linux-arm64": {
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


# Windows 自带的模块（System32 里必有）。这张表只用来判断"要不要打进包里"，
# 不用来判断"能不能跑" —— 表里少写一个，代价是启动时报错退出（响亮），
# 多写一个才是静默的坏包，所以宁可写全。
SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll", "cfgmgr32.dll",
    "comdlg32.dll", "crypt32.dll", "cryptbase.dll", "dbghelp.dll", "dnsapi.dll",
    "dwmapi.dll", "gdi32.dll", "gdi32full.dll", "imm32.dll", "iphlpapi.dll",
    "kernel32.dll", "kernelbase.dll", "msvcrt.dll", "mswsock.dll",
    "netapi32.dll", "normaliz.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll",
    "powrprof.dll", "psapi.dll", "rpcrt4.dll", "secur32.dll", "setupapi.dll",
    "shell32.dll", "shlwapi.dll", "user32.dll", "userenv.dll", "usp10.dll",
    "version.dll", "win32u.dll", "winmm.dll", "winspool.drv", "ws2_32.dll",
    "wtsapi32.dll",
}


def is_system_dll(name):
    return (name in SYSTEM_DLLS
            or name.startswith("api-ms-win-")
            or name.startswith("ext-ms-win-"))


# 这些目录下的运行库是给别的目标编译的（OneCore / Spectre 缓解版 / 调试版），
# 文件名与桌面版**逐个相同**，靠 os.walk 碰运气就会挑中它们。
_NON_DESKTOP_RUNTIME_DIRS = {"onecore", "spectre", "debug_nonredist", "auxiliary"}


def _msvc_runtime_rank(root, arch):
    r"""给一份候选运行库的所在目录打分，越大越该被选中。

    4 = `...\VC\Redist\MSVC\<ver>\<arch>\Microsoft.VC*.CRT`（桌面可再分发的正牌）
    3 = 别的含 `Redist` 的路径
    2 = 架构目录对上、既不在 Redist 下也不是非桌面变体
    1 = onecore / spectre / debug_nonredist —— 同名，但是给别的目标编的
    0 = 根本不是这个架构

    2 与 1 必须分开：合成树（没有 Redist）里两者会同时存在，一档就退化成
    "谁先被 os.walk 遍历到谁赢"，而 `onecore` 按字典序排在 `x64` 前面。
    """
    if os.sep + arch + os.sep not in root + os.sep:
        return 0
    parts = [p for p in root.split(os.sep) if p]
    if any(p.lower() in _NON_DESKTOP_RUNTIME_DIRS for p in parts):
        return 1
    for i in range(len(parts) - 3):
        if parts[i] == "Redist" and parts[i + 1] == "MSVC" and parts[i + 3] == arch:
            return 4
    return 3 if "Redist" in parts else 2


def find_runtime_for_arch(base, name, arch):
    """在 base 下找 name 的**桌面可再分发**运行库，且架构必须是 arch；找不到返回 None。

    这个查找有三处都会被 os.walk 的遍历顺序坑掉，而每一次挑错都是**静默坏包** ——
    打包机自己就是 x64，塞进去一份不对的文件本地照样能跑：

    1. **哪一份拷贝**。同一个 vcruntime140.dll 在 VS 安装树里有近 30 份，
       `Common7\\IDE\\`、`Common7\\IDE\\Remote Debugger\\x64\\`、`CoreCon\\...` 各带一份，
       而且按字典序**全都排在 `VC\\Redist\\` 前面**。那些是 VS 自己运行时用的副本，
       版本未必与编译时用的工具集一致；能再分发的只在 `VC\\Redist\\MSVC\\` 下。
    2. **哪个变体**。`<ver>\\` 下面除了 `<arch>\\`，还有 `onecore\\<arch>\\`、
       `spectre\\<arch>\\`、`debug_nonredist\\`，文件名逐个相同；`onecore` 按字典序
       排在 `x64` 前面，所以"含 \\x64\\ 就算"必然挑中 OneCore 版。
    3. **哪个架构**。`x64` / `arm64` / `x86` 三份并存，挑错就是把 x64 的运行库
       打进 arm64 的包。

    所以按打分选最优（见 `_msvc_runtime_rank`），而不是撞到第一个就返回。
    """
    best_rank = 0
    best = None
    for root, _dirs, files in os.walk(base):
        if name not in files:
            continue
        rank = _msvc_runtime_rank(root, arch)
        if rank > best_rank:
            best_rank = rank
            best = os.path.join(root, name)
            if rank == 4:
                break
    return best


def pe_imports(path):
    """读 PE 导入表，返回依赖的 dll 名（小写、去重、保序）；非 PE 返回 None。

    **注意这个 None 的后果**：调用点（`if deps is not None:`）在非 PE 上会整段跳过，
    也就是说下面那道"bin/ 必须覆盖 dll 真实导入表"的自检在 **ELF / Mach-O 上是空操作**。
    Linux 侧目前靠 release.yml 里的 `ldd` 步骤兜着，macOS 侧没有兜底。
    哪天要覆盖 ELF，在这里加 `readelf -d` / `objdump -p` 取 NEEDED 的分支，
    并配一张 libc/libm/libpthread/ld-linux 之类的系统库白名单。

    为什么不用 objdump：msvc-x64 那个 job 跑在 Git Bash 里，objdump 不保证存在，
    而这道自检要防的恰恰是"发布包在干净机器上起不来"，它在哪台 runner 上都得能跑。

    为什么要有这道自检：原先靠一张手写的依赖清单（msvcp140 / vcruntime140 …），
    清单是猜的。MSYS2 同时装了 `libssl.a` 和 `libssl.dll.a`，find_library 默认挑
    `.dll.a` ⇒ 产物凭空多一个 `libssl-3-x64.dll`，而清单里没有、MinGW 侧的自检
    又只 grep 了 libgcc|libstdc|libwinpthread，于是照样发布出去。
    """
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"MZ":
        return None
    e_lfanew = int.from_bytes(d[0x3C:0x40], "little")
    if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None
    coff = e_lfanew + 4
    nsec = int.from_bytes(d[coff + 2:coff + 4], "little")
    opt_size = int.from_bytes(d[coff + 16:coff + 18], "little")
    opt = coff + 20
    magic = int.from_bytes(d[opt:opt + 2], "little")
    ddir = opt + (112 if magic == 0x20B else 96)   # PE32+ 的 DataDirectory 偏 112
    imp_rva = int.from_bytes(d[ddir + 8:ddir + 12], "little")   # DataDirectory[1]

    sects = []
    for i in range(nsec):
        b = opt + opt_size + i * 40
        va = int.from_bytes(d[b + 12:b + 16], "little")
        vsize = int.from_bytes(d[b + 8:b + 12], "little")
        rsize = int.from_bytes(d[b + 16:b + 20], "little")
        raw = int.from_bytes(d[b + 20:b + 24], "little")
        sects.append((va, max(vsize, rsize), raw))

    def rva2off(rva):
        for va, size, raw in sects:
            if va <= rva < va + size:
                return raw + (rva - va)
        return None

    def cstr(off):
        end = d.index(b"\0", off)
        return d[off:end].decode("ascii", "replace").lower()

    if not imp_rva:
        return []
    off = rva2off(imp_rva)
    if off is None:
        return []
    names, seen = [], set()
    while off + 20 <= len(d):
        desc = d[off:off + 20]
        if desc == b"\0" * 20:
            break
        off += 20
        nrva = int.from_bytes(desc[12:16], "little")
        no = rva2off(nrva) if nrva else None
        if no is None:
            continue
        n = cstr(no)
        if n and n not in seen:
            seen.add(n)
            names.append(n)
    return names


def runtime_roots(tree, repo):
    """运行时 dll 的搜索根。够用就行 —— 找不到会退出 2 并点名，不会静默放过。"""
    roots = []
    cache = os.path.join(tree, "CMakeCache.txt")
    if os.path.exists(cache):
        with open(cache, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("OPENSSL_ROOT_DIR:"):
                    roots.append(os.path.join(line.split("=", 1)[1].strip(), "bin"))
    roots += [os.environ.get("OPENSSL_ROOT_DIR", "") + "/bin",
              os.environ.get("VCToolsRedistDir", ""),
              os.path.join(tree, "Release"), tree, repo]
    roots += [r"C:\Program Files\OpenSSL-Win64\bin",
              r"C:\Program Files\OpenSSL\bin"]
    return [r for r in roots if r and os.path.isdir(r)]


def find_named(root, name):
    """在 root 及其一层子目录里找 `name`（不递归到底：那会把 VS 安装目录走穿）。"""
    p = os.path.join(root, name)
    if os.path.isfile(p):
        return p
    try:
        entries = os.listdir(root)
    except OSError:
        return None
    for e in entries:
        sub = os.path.join(root, e)
        if os.path.isdir(sub) and os.path.isfile(os.path.join(sub, name)):
            return os.path.join(sub, name)
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


def copy_header(src, dst):
    """拷公开头；含非 ASCII 又没 BOM 的，补一个 UTF-8 BOM。

    仓里的约定本来就是「UTF-8 带 BOM」（src/ 下 99 个头里 75 个有），另有 22 个
    跑偏成了无 BOM。仓内编译看不出来：`add_compile_options(/utf-8)`
    （CMakeLists.txt:95-97）替它们兜着。但那个开关是**目录作用域**的 —— 它既不进
    导出集，更到不了预编译包的消费者。于是 MSVC 使用者按系统代码页 936 读这些头，
    中文注释的末字节吞掉换行、把 `*/` 吃掉，注释不闭合，报错却落在 `<algorithm>`
    里。实测：`cl /nologo /std:c++14 /EHsc /I<包>/include /c consumer.cpp` 编不过；
    给这 22 个补上 BOM 之后同一条命令 rc=0（g++ 也照过 —— 前导 BOM 它接受并忽略，
    所以这里不按平台分叉，各平台的包保持同一份字节）。

    这是**下限**不是根因：根因在 src/ 那 22 个头自己没 BOM，走 `find_package`
    的消费者一样中招（那条路不经过本脚本）。这里兜住的是"新加的头忘了 BOM 也不会
    再把发出去的包弄坏"。纯 ASCII 的头不加 BOM。
    """
    with open(src, "rb") as f:
        data = f.read()
    if not data.startswith(b"\xef\xbb\xbf"):
        try:
            data.decode("ascii")
        except UnicodeDecodeError:
            data = b"\xef\xbb\xbf" + data
    with open(dst, "wb") as f:
        f.write(data)
    shutil.copystat(src, dst)


def main():
    # Windows 的 stdout 默认走 ANSI 代码页（CI runner 是 cp1252），下面那些中文
    # 诊断一旦执行到就 UnicodeEncodeError 崩掉。它只在**产物依赖了第三方 dll**
    # （于是 third 非空、真正走到那两行 print）时才触发，表现为某几条腿莫名红、
    # 另几条全绿；本机是 UTF-8 控制台，永远看不出来。CI 实测踩过一次。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")

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
    # 要挑运行库就必须知道目标架构，缺了是配置错误：宁可在这里炸掉，
    # 也不要让 find_runtime_for_arch 退化成"随便挑一份"。
    if spec["runtime"] and not spec.get("msvc_arch"):
        print("平台 %s 声明了 runtime 却没写 msvc_arch，无法判断该挑哪个架构的运行库"
              % args.platform)
        return 2

    for rt in spec["runtime"]:
        src = None
        for base in filter(None, [os.environ.get("VCToolsRedistDir"),
                                  os.environ.get("VCINSTALLDIR")]):
            src = find_runtime_for_arch(base, rt, spec["msvc_arch"])
            if src:
                break
        if src is None:
            # VCToolsRedistDir 只在开发者命令提示符里才有；CI 的 bash 步里
            # 常常是空的。按 VS 的固定布局再找一遍，免得每换一台机器就发不出包。
            for base in (r"C:\Program Files\Microsoft Visual Studio",
                         r"C:\Program Files (x86)\Microsoft Visual Studio"):
                if not os.path.isdir(base):
                    continue
                src = find_runtime_for_arch(base, rt, spec["msvc_arch"])
                if src:
                    break
        if src:
            shutil.copy2(src, os.path.join(stage, "bin"))
            print("bin: %s" % rt)
        else:
            missing.append("MSVC 运行库 %s" % rt)

    # ---- 依赖自检：bin/ 必须覆盖 dll 真实的导入表 ----
    # 静态链接的产物在这里是空操作（导入表里只有 Windows 自带模块）；一旦哪次
    # 构建走了导入库，多出来的依赖会被当场抓到并补进包，补不到就退出 2。
    if dll is not None:
        deps = pe_imports(dll)
        if deps is not None:
            third = [n for n in deps if not is_system_dll(n)]
            print("imports: %s" % " ".join(sorted(deps)))
            if third:
                print("imports 里非 Windows 自带的: %s" % " ".join(sorted(third)))
            roots = runtime_roots(tree, repo)
            for n in third:
                if n in {f.lower() for f in os.listdir(os.path.join(stage, "bin"))}:
                    continue
                src = None
                for r in roots:
                    src = find_named(r, n)
                    if src:
                        break
                if src:
                    shutil.copy2(src, os.path.join(stage, "bin"))
                    print("bin: %s  (dll 导入表要求)" % n)
                else:
                    missing.append("dll 的依赖 %s（导入表里有，包里没有）" % n)

    # ---- 公开头 ----
    # 每个模块目录**至少**要发出一个头。模块名写错（或目录空了）时，包会"成功"
    # 产出，而使用者一 include 就是找不到文件 —— 让它在这里失败，别在消费方炸。
    for m in MODULES:
        d = os.path.join(repo, "src", m)
        if not os.path.isdir(d):
            missing.append("模块目录 src/%s 不存在（MODULES 写错了？）" % m)
            continue
        dst = os.path.join(stage, "include", m)
        os.makedirs(dst, exist_ok=True)
        n = 0
        for f in os.listdir(d):
            if f.endswith(".h") and f not in PRIVATE_HEADERS:
                copy_header(os.path.join(d, f), os.path.join(dst, f))
                n += 1
        if n == 0:
            missing.append("模块 %s 一个头都没发出" % m)
    # 注意给的是**完整目标文件名**：`copy_header` 收文件路径，不是 `copy2` 那种
    # "dst 是目录就放进去"的语义。
    copy_header(os.path.join(repo, "src", "uvcpp.h"),
                os.path.join(stage, "include", "uvcpp.h"))
    print("include: %d 个模块" % len([m for m in MODULES
                                      if os.path.isdir(os.path.join(repo, "src", m))]))

    # ---- 生成的使能宏头 ----
    # CMakeLists.txt 的 configure_file 把它产出在**构建树**里（刻意不落 src/，
    # 否则会被 src/uvcpp/*.h 的 glob 当手写头收走）。每个公开头都包含它，缺了它
    # 整包编不过 —— 所以它进 missing[]，不是"拷不到就算了"。
    gen = os.path.join(tree, "include", "uvcpp", "uvcpp_config.h")
    if os.path.exists(gen):
        shutil.copy2(gen, os.path.join(stage, "include", "uvcpp"))
        print("include/uvcpp: uvcpp_config.h（生成）")
    else:
        missing.append("生成的宏头 %s —— 先在 %s 上跑一次 cmake" % (gen, tree))

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
    # **不在这里传使能宏**：它们由包里的 <uvcpp/uvcpp_config.h> 给出，公开头自己
    # 包含它（见 RELEASE.md）。这里以前写死一整套 `-DUVCPP_*_ENABLE=1`，与包实际
    # 怎么编毫无关系 —— 一个 OPENSSL 关着编出来的包也会被喂
    # `-DUVCPP_OPENSSL_ENABLE=1`，正好把生成头要根治的那个病（宏集与 dll 不一致
    # ⇒ 成员布局错位 ⇒ 内联访问器静默读错偏移）从另一头放回来。
    pc = os.path.join(stage, "lib", "pkgconfig", "uvcpp.pc")
    with open(pc, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "prefix=${pcfiledir}/../..\n"
            "exec_prefix=${prefix}\n"
            "libdir=${prefix}/lib\n"
            "includedir=${prefix}/include\n"
            "\n"
            "Name: uvcpp\n"
            "Description: Modern C++ wrapper for libuv (prebuilt, %s)\n"
            "Version: %s\n"
            "Cflags: -I${includedir}\n"
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
