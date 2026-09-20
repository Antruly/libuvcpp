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
import subprocess
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
# `lib_dll` 是动态库（默认进 `bin/`；ELF 平台用 `lib_dest` 改到 `lib/`），
# `import_lib` 是链接时用的导入库/静态库（进 `lib/`），
# `runtime` 是要跟着 dll 一起发的第三方运行时。
#
# `*_debug` 是同名的调试档（`CMakeLists.txt` 的 `DEBUG_POSTFIX "d"` 产出
# `uvcppd.*` / `libuvcppd.*`）。**必须先有 `--debug-tree` 才会用到**，见 main()。
#
# **调试档的候选表与发布档是两张独立的表，绝不互相 fallback。** 把两档混进一张
# 候选表是致命的：`find_first` 返回第一个存在的路径，发布档缺失时会**静默**取到
# 调试档的文件，再以 `uvcpp.dll` 的名字发出去 —— 判据全绿而包是错的。
# 同理不要给 find_first 加"猜 Debug/ 前缀"的行为。
# `pdb_debug` 只有 MSVC 非空：它的符号是**外置**的 `.pdb`。GCC/Clang 的调试信息
# 内嵌在动态库自己的 `.debug_*` 节里，没有独立符号文件可发。
PLATFORMS = {
    "mingw-x64": {
        "lib_dll": ["libuvcpp.dll"],
        "import_lib": ["libuvcpp.dll.a"],
        "lib_dll_debug": ["libuvcppd.dll"],
        "import_lib_debug": ["libuvcppd.dll.a"],
        "pdb_debug": [],
        "runtime": [],          # 运行时已静态链进 dll
        "pc_libs": "-L${libdir} -luvcpp",
    },
    # 与 mingw-x64 逐字同构：MSYS2 的 `lib` 前缀是工具链决定的，与架构无关
    # （CLANGARM64 同样产出 libuvcpp.dll + libuvcpp.dll.a）。
    "mingw-arm64": {
        "lib_dll": ["libuvcpp.dll"],
        "import_lib": ["libuvcpp.dll.a"],
        "lib_dll_debug": ["libuvcppd.dll"],
        "import_lib_debug": ["libuvcppd.dll.a"],
        "pdb_debug": [],
        "runtime": [],
        "pc_libs": "-L${libdir} -luvcpp",
    },
    "msvc-x64": {
        "lib_dll": ["uvcpp.dll", "Release/uvcpp.dll"],
        "import_lib": ["uvcpp.lib", "Release/uvcpp.lib"],
        "lib_dll_debug": ["Debug/uvcppd.dll", "uvcppd.dll"],
        "import_lib_debug": ["Debug/uvcppd.lib", "uvcppd.lib"],
        "pdb_debug": ["Debug/uvcppd.pdb", "uvcppd.pdb"],
        # MSVC 用 /MD：发布包里必须自带运行库 dll，否则使用者机器上
        # 没有 VC++ 可再发行组件时直接 0xc0000135。
        #
        # **这张表只管发布档。** 调试档链的是 /MDd（msvcp140d / vcruntime140d /
        # ucrtbased），那几份微软不允许再分发、只在 VS 安装树里，所以**刻意不发**
        # —— 见 DEBUG_CRT_NAMES 与 RELEASE.md 里"Debug 版前提"那段。
        "runtime": ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"],
        "pc_libs": "-L${libdir} -luvcpp",
        "msvc_arch": "x64",
    },
    "msvc-arm64": {
        "lib_dll": ["uvcpp.dll", "Release/uvcpp.dll"],
        "import_lib": ["uvcpp.lib", "Release/uvcpp.lib"],
        "lib_dll_debug": ["Debug/uvcppd.dll", "uvcppd.dll"],
        "import_lib_debug": ["Debug/uvcppd.lib", "uvcppd.lib"],
        "pdb_debug": ["Debug/uvcppd.pdb", "uvcppd.pdb"],
        "runtime": ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"],
        "pc_libs": "-L${libdir} -luvcpp",
        "msvc_arch": "arm64",
    },
    # 无 SOVERSION/VERSION（根 CMakeLists 一处都没设），所以实体就叫 libuvcpp.so，
    # 没有 `.so.1` / `.so.1.1.0` 符号链接链。x64 与 arm64 同名，只有内容不同。
    #
    # **这个 .so 必须落 `lib/`，不能像 Windows 那样放 `bin/`。** ELF 没有"导入库"
    # 这回事：同一个 libuvcpp.so 既是要链的、也是运行时要装的。放 `bin/` 的话，
    # 包自己的 `uvcpp.pc`（`-L${libdir} -luvcpp`）与 RELEASE.md 那条
    # `-L lib -luvcpp` 在 Linux 上**全都链不上**（`ld: cannot find -luvcpp`）。
    # MinGW/MSVC 各有一份导入库落在 `lib/`，所以这个洞一直没露出来 ——
    # config-contract 门禁的 ubuntu 那一档第一次撞到它。
    "linux-x64": {
        "lib_dll": ["libuvcpp.so"],
        "lib_dest": "lib",
        "import_lib": [],
        "lib_dll_debug": ["libuvcppd.so"],
        "import_lib_debug": [],
        "pdb_debug": [],
        "runtime": [],
        "pc_libs": "-L${libdir} -luvcpp",
    },
    "linux-arm64": {
        "lib_dll": ["libuvcpp.so"],
        "lib_dest": "lib",
        "import_lib": [],
        "lib_dll_debug": ["libuvcppd.so"],
        "import_lib_debug": [],
        "pdb_debug": [],
        "runtime": [],
        "pc_libs": "-L${libdir} -luvcpp",
    },
}

# 调试档的链接行。六个平台**逐字相同**（`--config Debug` 下 CMake 的
# `DEBUG_POSTFIX "d"` 给三个平台的产物都加了同一个 `d`），所以只有一份常量，
# 不按平台重复写六遍。写错的话 `check_config_contract.py` 会红：它拿这行的
# `-L`/`-l` 去包里找**同名**的可链接库（`libuvcppd.dll.a` / `uvcppd.lib` /
# `libuvcppd.so`），指不到就报"那个目录里没有包自己那份库"。
PC_LIBS_DEBUG = "-L${libdir} -luvcppd"


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

# 调试版 CRT（`/MDd` 链的那几份）。**微软不允许再分发**，只在 VS 安装树里
# （`VC\Redist\MSVC\<ver>\debug_nonredist\<arch>\Microsoft.VC*.DebugCRT\`），
# 所以包里**刻意不发** —— 拿了调试档的人必须本机装 VS 才跑得起来（RELEASE.md
# "Debug 版前提"那段）。
#
# 用途一：`:502` 那段"bin/ 必须覆盖 dll 真实导入表"的自动补依赖，对这张表里的
# 名字**只打印、不查找、不拷贝**。调试档的导入表里必然有它们（`/MDd` 就是这么
# 链的），照原样去 runtime_roots 里捞只会捞不到、然后把包判死（rc=2）。
# 用途二：打包完再断言**整棵 stage 里一个都不许有** —— 防的是"哪次改动真把它们
# 拷进来了"。
#
# **必须逐名列举，绝不能用 `*d.dll` 通配**：包里自己就有 uvcppd.dll /
# libuvcppd.dll，通配会把自己的产物判红。
DEBUG_CRT_NAMES = {
    "concrt140d.dll", "msvcp140_1d.dll", "msvcp140_2d.dll", "msvcp140d.dll",
    "ucrtbased.dll", "vccorlib140d.dll", "vcruntime140_1d.dll", "vcruntime140d.dll",
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


def _pe_layout(d):
    """PE 的节表。返回 `(sections, opts_off, opt_size)`；sections 每项是
    `(name, va, size, raw_off, raw_size)`，`size` 取 `max(virtual, raw)`。

    **名字必须走 COFF 的长名机制**：节头里只有 8 字节放名字，超过就写
    `/NNN`（相对字符串表的偏移，字符串表跟在符号表后面）。`.debug_info` 是
    11 个字符，正是这种 —— 直接读那 8 字节会读成 `/4`，于是"这个产物有没有
    调试节"**永远**数出 0 个。这个坑实测踩过一次：MinGW 的 Release dll 明明
    有 9 个 `.debug_*` 节，按 8 字节读却是 0。
    """
    e_lfanew = int.from_bytes(d[0x3C:0x40], "little")
    coff = e_lfanew + 4
    nsec = int.from_bytes(d[coff + 2:coff + 4], "little")
    opt_size = int.from_bytes(d[coff + 16:coff + 18], "little")
    sym_off = int.from_bytes(d[coff + 8:coff + 12], "little")
    n_sym = int.from_bytes(d[coff + 12:coff + 16], "little")
    str_off = sym_off + n_sym * 18
    opt = coff + 20
    sects = []
    for i in range(nsec):
        b = opt + opt_size + i * 40
        raw8 = d[b:b + 8]
        if raw8[:1] == b"/":
            o = int(raw8[1:].split(b"\0")[0] or b"0")
            end = d.find(b"\0", str_off + o)
            name = d[str_off + o:end if end > 0 else str_off + o + 64].decode(
                "ascii", "replace")
        else:
            name = raw8.rstrip(b"\0").decode("ascii", "replace")
        vsize = int.from_bytes(d[b + 8:b + 12], "little")
        va = int.from_bytes(d[b + 12:b + 16], "little")
        rsize = int.from_bytes(d[b + 16:b + 20], "little")
        raw = int.from_bytes(d[b + 20:b + 24], "little")
        sects.append((name, va, max(vsize, rsize), raw, rsize))
    return sects, opt, opt_size


def _elf_debug_sections(d):
    """ELF 里所有 `.debug*` 节的内容（按节头表找）；不是 ELF 返回 None。"""
    if d[:4] != b"\x7fELF":
        return None
    is64 = d[4] == 2
    if is64:
        sh_off = int.from_bytes(d[0x28:0x30], "little")
        entsize = int.from_bytes(d[0x3A:0x3C], "little")
        shnum = int.from_bytes(d[0x3C:0x3E], "little")
        shstrndx = int.from_bytes(d[0x3E:0x40], "little")
    else:
        sh_off = int.from_bytes(d[0x20:0x24], "little")
        entsize = int.from_bytes(d[0x2E:0x30], "little")
        shnum = int.from_bytes(d[0x30:0x32], "little")
        shstrndx = int.from_bytes(d[0x32:0x34], "little")
    if not sh_off or not shnum or entsize == 0:
        return []

    def sh(i):
        b = sh_off + i * entsize
        name = int.from_bytes(d[b:b + 4], "little")
        if is64:
            off = int.from_bytes(d[b + 24:b + 32], "little")
            size = int.from_bytes(d[b + 32:b + 40], "little")
        else:
            off = int.from_bytes(d[b + 16:b + 20], "little")
            size = int.from_bytes(d[b + 20:b + 24], "little")
        return name, off, size

    if shstrndx >= shnum:
        return []
    _n, strtab_off, strtab_size = sh(shstrndx)
    strtab = d[strtab_off:strtab_off + strtab_size]
    out = []
    for i in range(shnum):
        name_off, off, size = sh(i)
        end = strtab.find(b"\0", name_off)
        name = strtab[name_off:end if end > 0 else name_off + 32]
        if name.startswith(b".debug") or name.startswith(b".zdebug"):
            out.append(d[off:off + size])
    return out


def debug_section_bytes(path):
    """把产物里所有调试节的内容拼起来；一个都没有、或格式不认识，返回 None。

    - PE（MinGW / GCC）走 `.debug_*` 节，`_pe_layout` 解长名。
    - ELF（Linux）走节头表。
    - **MSVC 在这里必然返回 None** —— 它的调试信息外置在 `.pdb` 里，动态库自己
      没有调试节。所以 MSVC 那条路的"是不是调试档"要靠导入表与 RSDS 判，
      见 `check_msvc_debug_identity`。
    """
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] == b"MZ":
        e_lfanew = int.from_bytes(d[0x3C:0x40], "little")
        if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            return None
        sects, _opt, _osz = _pe_layout(d)
        parts = [d[raw:raw + rs] for n, _va, _sz, raw, rs in sects
                 if n.startswith(".debug") or n.startswith(".zdebug")]
    else:
        parts = _elf_debug_sections(d)
        if parts is None:
            return None
    return b"".join(parts) if parts else None


def own_source_names(repo):
    """本库自己的源文件名（`src/**`）。**只收 src/**，不收 tests/ 与 examples/**
    —— 那两个目录不编进动态库，拿它们的名字去认产物等于凭空放宽判据。
    """
    names = set()
    srcroot = os.path.join(repo, "src")
    for root, _dirs, files in os.walk(srcroot):
        for f in files:
            if f.endswith((".cpp", ".cc", ".cxx")):
                names.add(f)
    if not names:
        raise SystemExit("在 %s 下一个源文件都没找到 —— 判据的前提没了，先修这里"
                         % srcroot)
    return names


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
    sects, opt, _opt_size = _pe_layout(d)
    magic = int.from_bytes(d[opt:opt + 2], "little")
    ddir = opt + (112 if magic == 0x20B else 96)   # PE32+ 的 DataDirectory 偏 112
    imp_rva = int.from_bytes(d[ddir + 8:ddir + 12], "little")   # DataDirectory[1]

    def rva2off(rva):
        for _n, va, size, raw, _rs in sects:
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


def dep_src_paths(tree, repo, name):
    """`dep_src()` 会去找的那两个路径，按顺序。"""
    return [
        os.path.join(tree, "_deps", name + "-src"),
        os.path.join(repo, "_local_deps", name),
    ]


def dep_src(tree, repo, name):
    """定位第三方依赖的源码目录。

    本机用 `_local_deps/`（FETCHCONTENT_SOURCE_DIR_* 指过去），CI 上
    FetchContent 自己下到 `<tree>/_deps/<name>-src`。两处都要找。

    **只找这两处**：`FETCHCONTENT_SOURCE_DIR_*` 指向别处（不算少见）时这里找不到，
    而失败长得像"包坏了"（`缺少必需的文件`）而不是"源码目录没在约定位置" ——
    所以报错文案里把找过的路径印出来。
    """
    return find_dir(dep_src_paths(tree, repo, name))


def copytree(src, dst):
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def copy_header(src, dst):
    """拷公开头；含非 ASCII 又没 BOM 的，补一个 UTF-8 BOM。

    仓里的约定本来就是「UTF-8 带 BOM」。**这条兜底现在不会命中任何东西**：
    2026-09-21 实测 src/ 下 99 个公开头里 97 个带 BOM，剩下 2 个
    （`expand/uvcpp_memory_pool_span.h`、`expand/uvcpp_page_allocator.h`）是纯 ASCII
    —— 当年那 22 个"含非 ASCII 又没 BOM"的头已经全部补上了。

    留着它的理由是**下限**而不是根因：根因是头文件自己有没有 BOM，走
    `find_package(uvcpp)` 的消费者不经过本脚本（顶层 `CMakeLists.txt` 里
    `if(MSVC) add_compile_options(/utf-8) endif()` 是**目录作用域**的，既不进导出集、
    也到不了预编译包的消费者）。这里兜住的是"新加的头忘了 BOM 也不会再把发出去的包
    弄坏"—— 而 MSVC 按系统代码页 936 读无 BOM 的头时，中文注释的末字节会吞掉换行、
    把 `*/` 吃掉，注释不闭合，报错却落在 `<algorithm>` 里。实测：
    `cl /nologo /std:c++14 /EHsc /I<包>/include /c consumer.cpp` 编不过；补上 BOM
    之后同一条命令 rc=0（g++ 也照过 —— 前导 BOM 它接受并忽略，所以这里不按平台分叉，
    各平台的包保持同一份字节）。纯 ASCII 的头不加 BOM。
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


def check_debug_identity(rel_dll, dbg_dll, pdb_path, pdb_name, own_names):
    """断言两个动态库确实**一档一个**，且各自身份与文件名相符。返回问题清单。

    为什么不能只靠文件名：文件名是给人看的，staging 把调试档拷成 `uvcpp.dll`
    的名字、或反过来，**所有**基于名字的判据都会全绿而包是错的。所以这里一律
    看产物自己说了什么，而且**两个方向都判**（调试档必须像调试档，发布档必须
    像发布档）—— 只判一个方向的话，另一个方向被顶替不会被发现。

    两条路按产物内容分派，不按平台名分派：

    - **有 `.debug_*` 节**（MinGW / Linux，GCC/Clang 把 DWARF 内嵌在库里）：
      判据是"调试档的调试节里出现本库自己的源文件名，发布档一个都没有"。
      这条的对照实测过：MinGW 的 Release dll **也有** 355 KB 调试节（静态链进去
      的依赖带 `-g`），所以"有没有 `.debug_*`"**不是**判据（那样会在每个正确
      构建上恒红）；能分辨的是**调试节里有没有我们自己的源文件**。
      顺带记一笔：那两个名字在 Release 的 `.rdata` 里**是有的**（`__FILE__`
      字符串），所以这条判据必须**只看调试节**，看整个文件会失去分辨力。
    - **没有调试节**（MSVC，符号外置在 `.pdb`）：判据是导入表 + RSDS。
      调试档必须导入 `/MDd` 的那几份 CRTD，发布档必须一份都不导入；
      调试档必须带 CodeView 调试目录（`RSDS`）且里面写着自己的 `.pdb` 名，
      发布档必须一个 `RSDS` 都没有（Release 配置的
      `GenerateDebugInformation=false`，实测命中 0 次）。
    """
    probs = []
    dbg_blob = debug_section_bytes(dbg_dll)

    if dbg_blob is not None:
        rel_blob = debug_section_bytes(rel_dll) or b""
        hit_dbg = sorted(n for n in own_names if n.encode() in dbg_blob)
        hit_rel = sorted(n for n in own_names if n.encode() in rel_blob)
        # 至少 3 个不同的源文件才算数：单个字符串可能是别处漏进来的同名串，
        # 而一个真的按 -g 编出来的库**每个** TU 都会留下自己的名字（实测
        # 86 个源文件里命中一大片，不是一两个）。
        if len(hit_dbg) < 3:
            probs.append(
                "调试档 %s 的调试节里只有 %d 个本库源文件名（要 >=3）—— "
                "它多半不是按 -g 编出来的那份（或 staging 指到了发布档）"
                % (os.path.basename(dbg_dll), len(hit_dbg)))
        if hit_rel:
            probs.append(
                "发布档 %s 的调试节里有 %d 个本库源文件名（%s…）—— "
                "发布档是按 -O3 -DNDEBUG 编的，不该带自己的调试信息，"
                "staging 多半指到了调试档"
                % (os.path.basename(rel_dll), len(hit_rel), ", ".join(hit_rel[:3])))
        return probs

    # ---- MSVC 路线 ----
    d_rel = pe_imports(rel_dll)
    d_dbg = pe_imports(dbg_dll)
    if d_dbg is None or d_rel is None:
        probs.append("读不到 %s / %s 的导入表，没法判身份"
                     % (os.path.basename(dbg_dll), os.path.basename(rel_dll)))
    else:
        hit_dbg = sorted(set(d_dbg) & DEBUG_CRT_NAMES)
        hit_rel = sorted(set(d_rel) & DEBUG_CRT_NAMES)
        if not hit_dbg:
            probs.append(
                "调试档 %s 的导入表里没有任何调试版 CRT（%s 一个都没有）—— "
                "它其实不是 /MDd 链的，不是调试档"
                % (os.path.basename(dbg_dll), "/".join(sorted(DEBUG_CRT_NAMES))))
        if hit_rel:
            probs.append(
                "发布档 %s 导入了调试版 CRT（%s）—— 它其实是 /MDd 链的，"
                "不是发布档" % (os.path.basename(rel_dll), ", ".join(hit_rel)))

    for path, want, label in ((dbg_dll, True, "调试档"), (rel_dll, False, "发布档")):
        with open(path, "rb") as f:
            data = f.read()
        n_rsds = data.count(b"RSDS")
        if want and n_rsds == 0:
            probs.append(
                "%s %s 里没有 CodeView 调试目录（RSDS 命中 0）—— /DEBUG 没生效，"
                "或者它既不是 MSVC 产物、也不是带 -g 编出来的"
                % (label, os.path.basename(path)))
        if not want and n_rsds:
            probs.append(
                "%s %s 里有 %d 处 RSDS —— 发布档的 GenerateDebugInformation "
                "是 false，不该有" % (label, os.path.basename(path), n_rsds))

    if pdb_path is None:
        probs.append("声明的平台要有 .pdb，但没找到（%s）" % pdb_name)
    else:
        with open(dbg_dll, "rb") as f:
            data = f.read()
        if pdb_name.encode() not in data:
            probs.append(
                "调试档 %s 里找不到符号文件名 %s —— 发出去的 .pdb 不是它的"
                % (os.path.basename(dbg_dll), pdb_name))
        probs += check_pdb_matches_dll(dbg_dll, pdb_path)
    return probs


def check_pdb_matches_dll(dll_path, pdb_path):
    """`.pdb` 与动态库必须**同源** —— 这是 PDB 唯一真正的不变量。

    文件名对得上、mtime 对得上都不够：一个对不上符号的 PDB 比没有 PDB 更坏，
    调试器会拿着错的符号安安静静地显示错的行号与错的变量。

    靠的是 CodeView 记录里的 `GUID` + `age`，两头各写一份：
    - PE 侧在 debug directory 的第 4 项（CodeView），指向一个 `RSDS` 块，
      里面是 GUID(16) + age(4) + 路径。
    - PDB 侧在 MSF 目录的 stream 1（PDB info stream），第 0 项就是同样的
      GUID(16) + age(4)。
    两边逐字节相同才算同源。实测：`build/Debug/uvcppd.dll` 与
    `build/Debug/uvcppd.pdb` 相配，与同目录 `uvcpp_sd.pdb` 不相配。
    """
    probs = []
    with open(dll_path, "rb") as f:
        d = open(dll_path, "rb").read()
    guid = _pe_codeview_guid(d)
    if guid is None:
        return ["%s 的 debug directory 里没有 CodeView(RSDS) 记录，"
                "没法与 PDB 对源" % os.path.basename(dll_path)]
    with open(pdb_path, "rb") as f:
        p = f.read()
    pdb_guid = _pdb_guid_age(p)
    if pdb_guid is None:
        return ["%s 不是认得的 MSF/PDB（读不出 stream 1 的 GUID）"
                % os.path.basename(pdb_path)]
    if guid != pdb_guid:
        probs.append(
            "%s 的 GUID+age 与 %s 对不上（dll %s / pdb %s）—— 这份 .pdb 不是"
            "这个 dll 的符号，发出去只会让人调试到错的行号"
            % (os.path.basename(dll_path), os.path.basename(pdb_path),
               guid.hex(), pdb_guid.hex()))
    return probs


def _pe_codeview_guid(d):
    """PE 里 CodeView(RSDS) 记录的 GUID+age（20 字节）；找不到返回 None。"""
    if d[:2] != b"MZ":
        return None
    e_lfanew = int.from_bytes(d[0x3C:0x40], "little")
    if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None
    sects, opt, _osz = _pe_layout(d)
    magic = int.from_bytes(d[opt:opt + 2], "little")
    ddir = opt + (112 if magic == 0x20B else 96)
    dbg_rva = int.from_bytes(d[ddir + 48:ddir + 52], "little")   # DataDirectory[6]
    dbg_size = int.from_bytes(d[ddir + 52:ddir + 56], "little")

    def rva2off(rva):
        for _n, va, size, raw, _rs in sects:
            if va <= rva < va + size:
                return raw + (rva - va)
        return None

    off = rva2off(dbg_rva)
    if off is None:
        return None
    for i in range(dbg_size // 28):
        b = off + i * 28
        typ = int.from_bytes(d[b + 12:b + 16], "little")
        size = int.from_bytes(d[b + 16:b + 20], "little")
        addr = int.from_bytes(d[b + 20:b + 24], "little")
        if typ != 2:          # IMAGE_DEBUG_TYPE_CODEVIEW
            continue
        p = rva2off(addr)
        if p is None or d[p:p + 4] != b"RSDS" or size < 24:
            continue
        return d[p + 4:p + 24]        # GUID(16) + age(4)
    return None


def _pdb_guid_age(p):
    """PDB（MSF 容器）里 stream 1 头部的 GUID+age（20 字节）；不认得返回 None。

    MSF 的布局（字段名同 LLVM 的 `MSFCommon.h`），全部小端：

        superblock（64 字节）
          0   char Magic[32]   "Microsoft C/C++ MSF 7.00\\r\\n\\x1aDS\\0\\0\\0"
          32  u32 BlockSize
          36  u32 FreeBlockMapBlock
          40  u32 NumBlocks
          44  u32 NumDirectoryBytes
          48  u32 Unknown
          52  u32 BlockMapAddr   ← 目录块号数组**所在**的块号

        ceil(NumDirectoryBytes / BlockSize) 个 u32 的"目录块号"数组，
        就存在第 BlockMapAddr 块里；按它拼出 directory：

          u32 NumStreams
          u32 StreamSizes[NumStreams]                  （0xFFFFFFFF = 空流）
          之后每个 stream 一段 u32 块号数组，
          长度是 ceil(该 stream 大小 / BlockSize) 项，按 stream 顺序紧挨着排。

        stream 1（PDB info）的开头是
            u32 Version, u32 Signature, u32 Age, Guid UniqueId(16), ... = 28 字节
        **字段顺序与 PE 那边相反**：RSDS 记录是 GUID(16) + Age(4)，这里是
        Age 在前、GUID 在后。所以要比的是 `stream1[12:28]`（GUID）与
        `stream1[8:12]`（Age）两个字段，**不是**把 stream1 的头 20 字节直接
        拿去比 —— 那样比出来永远是"对不上"，这条判据就会红在每一个正确构建上。
        实测踩过两次（都是拿正例 `uvcppd.dll` + `uvcppd.pdb` 当对照量出来的）：
        GUID 先是读到 `[8:24]`，再量才对到 `[12:28]`。
    """
    MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
    if len(p) < 64 or p[:32] != MAGIC:
        return None
    block_size = int.from_bytes(p[32:36], "little")
    dir_bytes = int.from_bytes(p[44:48], "little")
    block_map = int.from_bytes(p[52:56], "little")
    if block_size == 0 or dir_bytes == 0:
        return None
    n_dir_blocks = (dir_bytes + block_size - 1) // block_size
    bm = p[block_map * block_size:(block_map + 1) * block_size]
    if len(bm) < n_dir_blocks * 4:
        return None
    dir_blocks = [int.from_bytes(bm[i * 4:i * 4 + 4], "little")
                  for i in range(n_dir_blocks)]
    directory = b"".join(p[b * block_size:(b + 1) * block_size]
                         for b in dir_blocks)[:dir_bytes]
    if len(directory) < 4:
        return None
    n_streams = int.from_bytes(directory[0:4], "little")
    if n_streams < 2 or len(directory) < 4 + n_streams * 4:
        return None
    sizes = [int.from_bytes(directory[4 + i * 4:8 + i * 4], "little")
             for i in range(n_streams)]
    # stream 1 的块号数组排在 stream 0 的后面
    off = 4 + n_streams * 4 + ((sizes[0] + block_size - 1) // block_size) * 4
    if off + 4 > len(directory) or sizes[1] < 28:
        return None
    first_block = int.from_bytes(directory[off:off + 4], "little")
    start = first_block * block_size
    if start + 28 > len(p):
        return None
    return p[start + 12:start + 28] + p[start + 8:start + 12]   # GUID + Age


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
    # 调试档的来源树。**给了就必须有**（缺一个就 rc=2，不产出残缺的包）；
    # 不给就是"这个包不带调试档"，并会印一行说明 —— 让"没跑"看得见。
    #
    # 多配置生成器（MSVC）传与 `--tree` 同一个目录即可：调试档在 `Debug/` 子目录，
    # 靠 PLATFORMS 里候选名的 `Debug/` 前缀命中。单配置生成器（Ninja）要给另一棵
    # 树（`-DCMAKE_BUILD_TYPE=Debug`），因为 `CMAKE_BUILD_TYPE` 是 cache 变量，
    # 在同一棵树里翻它会把发布档整棵重编、并把两档产物混在一个目录里。
    #
    # **别把这个接到 `--config` 上**：那个参数在下面一次都没被引用过（候选名表里
    # 的 `Release/` 是写死的），是个空转旋钮，接上去只会让人以为它有用。
    ap.add_argument("--debug-tree", default="",
                    help="调试档的构建树；不给则本包不含调试档")
    args = ap.parse_args()

    tree = os.path.abspath(args.tree)
    repo = os.path.abspath(args.repo)

    # ---- 发布前校验：README 顶上那几个版本号必须是**正在出的这一版** ----
    # 放在建 stage 之前：README 是 `shutil.copy2` 原样进包的，没有任何一步会看
    # 一眼里面的版本号 —— 于是切 1.1.35 时会产出 `uvcpp-1.1.35-<platform>.zip`
    # 里装着一份自称 1.1.34 的 README。与 `_header_version()` 同一个道理：
    # 一个名字说谎的包比不出包更坏，所以这里也是**停**，不是警告。
    #
    # README 顶上写的是**当前源码树**的版本（与头文件同源，开发版带 `-dev`），
    # 所以这一条**不分开发版/发布版、每次都能真判**：本脚本也是 CI 的
    # `config-contract` 档每次 push 出开发版包用的，那一档上 README 与正在出的
    # 版本本来就该一致（`1.1.34-dev`）。
    #
    # 比的是 `args.version` 而不是头文件版本：`--version` 能覆盖头文件，那时
    # "正在出的那一版"就是命令行给的那个；带不带 `-dev` 仍由头文件里那个宏决定。
    rc = subprocess.call([sys.executable,
                          os.path.join(repo, "tests", "tools",
                                       "check_doc_versions.py"),
                          "--root", repo, "--expect", args.version])
    # 只有 rc=1 才是「判据红了」。其余非零（3 = 前提不满足：列不出根目录 / 读不到
    # 版本头；2 = argparse 把参数判错了）都是「**判据根本没跑**」，必须分开说 ——
    # 第一版这里一律印成「版本号不一致」，而 mingw64 那档的真实原因是它那条 PATH 上
    # 没有 git（判据当时依赖 `git ls-files`），报错把人指到了完全另一个方向。
    if rc == 1:
        print("\n**README 的版本号与要出的版本（%s）不一致，停在这里** —— "
              "修好上面那几条红再出包。" % args.version)
        return 2
    if rc != 0:
        print("\n**版本校验没跑成（rc=%d），所以这一条今天没判 —— 它不等于不一致。**"
              "上面那几行说了缺什么，修好它再出包。" % rc)
        return 2

    spec = PLATFORMS[args.platform]
    name = "libuvcpp-%s-%s" % (args.version, args.platform)
    stage = os.path.join(os.path.abspath(args.out), name)

    if os.path.isdir(stage):
        shutil.rmtree(stage)
    # ELF 平台的动态库直接落 lib/（见 PLATFORMS 里 linux-* 那段），所以 bin/ 不再
    # 无条件建：留一个空的 bin/ 会让人以为库放错了地方。只有真的要往里放运行库
    # （MSVC 那三份）时才建。
    dll_dest = spec.get("lib_dest", "bin")
    os.makedirs(os.path.join(stage, dll_dest))
    os.makedirs(os.path.join(stage, "lib", "pkgconfig"))
    if spec["runtime"]:
        os.makedirs(os.path.join(stage, "bin"), exist_ok=True)

    missing = []

    # ---- bin / lib ----
    dll = find_first(tree, spec["lib_dll"])
    if dll is None:
        missing.append("库 dll (%s)" % " / ".join(spec["lib_dll"]))
    else:
        shutil.copy2(dll, os.path.join(stage, dll_dest))
        print("%s: %s" % (dll_dest, os.path.basename(dll)))

    imp = find_first(tree, spec["import_lib"]) if spec["import_lib"] else None
    if spec["import_lib"] and imp is None:
        missing.append("导入库 (%s)" % " / ".join(spec["import_lib"]))
    elif imp:
        shutil.copy2(imp, os.path.join(stage, "lib"))
        print("lib: %s" % os.path.basename(imp))

    # ---- 调试档（只有给了 --debug-tree 才收） ----
    # 调试档进**同一个** bin/（ELF 平台是 lib/），与发布档并排 —— 使用者的调试器
    # 按同目录找符号，PDB 也就落在动态库旁边。文件名**原样保留**、绝不改名：
    # MSVC 的导入库里写死了被导入的 dll 名，MinGW 的 `libuvcppd.dll.a` 与 ELF 的
    # `NEEDED` 同理 —— 改名等于发一个"链得上、跑不起来"的包。
    dll_dbg = imp_dbg = pdb_dbg = None
    if args.debug_tree:
        dtree = os.path.abspath(args.debug_tree)
        if not os.path.isdir(dtree):
            print("--debug-tree 指的不是一个目录：%s" % dtree)
            return 2
        if not spec.get("lib_dll_debug"):
            print("平台 %s 没声明调试档的候选名（PLATFORMS 里缺 lib_dll_debug），"
                  "却给了 --debug-tree —— 这是配置错误" % args.platform)
            return 2
        dll_dbg = find_first(dtree, spec["lib_dll_debug"])
        if dll_dbg is None:
            missing.append("调试档 dll (%s)" % " / ".join(spec["lib_dll_debug"]))
        else:
            shutil.copy2(dll_dbg, os.path.join(stage, dll_dest))
            print("%s: %s  (调试档)" % (dll_dest, os.path.basename(dll_dbg)))
        if spec["import_lib_debug"]:
            imp_dbg = find_first(dtree, spec["import_lib_debug"])
            if imp_dbg is None:
                missing.append("调试档导入库 (%s)"
                               % " / ".join(spec["import_lib_debug"]))
            else:
                shutil.copy2(imp_dbg, os.path.join(stage, "lib"))
                print("lib: %s  (调试档)" % os.path.basename(imp_dbg))
        if spec["pdb_debug"]:
            pdb_dbg = find_first(dtree, spec["pdb_debug"])
            if pdb_dbg is None:
                missing.append("调试档符号 (%s)" % " / ".join(spec["pdb_debug"]))
            else:
                shutil.copy2(pdb_dbg, os.path.join(stage, dll_dest))
                print("%s: %s  (调试档符号)" % (dll_dest,
                                              os.path.basename(pdb_dbg)))
    else:
        print("本次包不含调试档（未给 --debug-tree）")

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
            # 发布档的运行库**绝不能**来自 `debug_nonredist`：那底下放的是同名的
            # 调试版 CRT（一条 `debug_nonredist\<arch>\Microsoft.VC*.DebugCRT\`），
            # 微软不允许再分发。`_msvc_runtime_rank` 给那个目录打的是 1 分而不是
            # 0 分，所以"正牌 Redist 恰好缺席"（比如只装了某些工作负载的机器）时
            # 它会胜出 —— 挑错一次就是往发布包里塞一份不可再分发的二进制。
            # 这里钉死，别让"找不到正牌"变成"那就用调试版"。
            if "debug_nonredist" in src.lower():
                print("找 %s 只找到调试版的那一份（%s）—— 这张表只管发布档，"
                      "停在这里" % (rt, src))
                return 2
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
            skip_dbg = []
            for n in third:
                # 调试档（`/MDd`）的导入表里必然有 msvcp140d / vcruntime140d /
                # ucrtbased 这几份。它们**不可再分发**（只在 VS 安装树里），包里
                # 刻意不发 —— 所以既不去找、也不记进 missing[]：记了就等于把每个
                # 带调试档的包都判死。
                #
                # 这一格必须**显式**写出来，不能指望"反正找不到"。`find_named`
                # 只走 root 加一层，而它们在 `VC\Redist\MSVC\<ver>\debug_nonredist\
                # <arch>\` 里是两层，现在确实捞不到 —— 但那是**巧合**，不是设计：
                # 哪天 find_named 改成递归，调试版 CRT 就会被静默拷进 bin/。
                if n in DEBUG_CRT_NAMES:
                    skip_dbg.append(n)
                    continue
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
            if skip_dbg:
                print("bin: %s —— 调试版 CRT，不可再分发，**刻意不打包**"
                      % " ".join(sorted(skip_dbg)))

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
        missing.append("libuv 头文件目录（找过 %s）"
                       % " 与 ".join(dep_src_paths(tree, repo, "libuv")))

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

    # 调试档的那份 .pc。不发它就等于包里放了一份**谁也没法链**的库：`uvcpp.pc`
    # 指的是 `-luvcpp`，而调试档的库叫 `libuvcppd.*` / `uvcppd.lib` 之类，名字
    # 逐个不同（`check_config_contract.py` 的候选名是**精确名**匹配，`-luvcpp`
    # 不会误命中 `libuvcppd.dll.a`，反方向也不会静默）。
    #
    # 加了第二个 .pc 就必须同时把门禁改成**遍历** `lib/pkgconfig/*.pc` ——
    # `check_config_contract.py:413` 原来写死 `uvcpp.pc`，不遍历的话这一份
    # 就是"恒绿，因为根本没跑"。
    if dll_dbg is not None:
        pc_dbg = os.path.join(stage, "lib", "pkgconfig", "uvcpp-debug.pc")
        with open(pc_dbg, "w", encoding="utf-8", newline="\n") as f:
            f.write(
                "prefix=${pcfiledir}/../..\n"
                "exec_prefix=${prefix}\n"
                "libdir=${prefix}/lib\n"
                "includedir=${prefix}/include\n"
                "\n"
                "Name: uvcpp-debug\n"
                "Description: Modern C++ wrapper for libuv "
                "(prebuilt Debug, %s)\n"
                "Version: %s\n"
                "Cflags: -I${includedir}\n"
                "Libs: %s\n" % (args.platform, args.version, PC_LIBS_DEBUG))

    # ---- 两个动态库必须各自身份自洽（看产物，不看文件名） ----
    if dll is not None and dll_dbg is not None:
        pdb_name = os.path.basename(spec["pdb_debug"][0]) if spec["pdb_debug"] else ""
        probs = check_debug_identity(dll, dll_dbg, pdb_dbg, pdb_name,
                                     own_source_names(repo))
        for p in probs:
            print("  ! %s" % p)
        # 通过时**也要说话**。这条判据拦的是"两档装反了"——只在发布之后才被发现
        # 的那种错，而它平时一声不响：日志里没有 `!` 既可能是"跑了且过了"，也可能
        # 是"根本没跑到这一段"。绿字挂在 `probs` 为空上（不是"没记过 fail"那种
        # 会跟着别处读数跑的写法），所以它只会在这两条判据真的比过之后才出现。
        if not probs:
            print("  身份判据：%s（发布档）与 %s（调试档）各自自洽，凭 %s"
                  % (os.path.basename(dll), os.path.basename(dll_dbg),
                     "调试节里的本库源文件名" if debug_section_bytes(dll_dbg) is not None
                     else "导入表 + RSDS"))
        missing += probs

    # ---- 不可再分发的调试版 CRT 一个都不许进包 ----
    # 上面那条自动补依赖已经不收它们了，这里再按**成品**扫一遍：防的是"哪次改动
    # 又从别的路径把它们拷了进来"，以及"哪台机器的 runtime_roots 恰好捞得到"。
    # 扫整棵 stage 而不是只看 bin/：ELF 平台根本没有 bin/（动态库在 lib/），
    # 而且这样顺带把"有人手工往里塞"也盖住了。
    leaked = []
    for root, _dirs, files in os.walk(stage):
        for f in files:
            if f.lower() in DEBUG_CRT_NAMES:
                leaked.append(os.path.relpath(os.path.join(root, f), stage))
    if leaked:
        missing.append("包里出现了不可再分发的调试版 CRT：%s"
                       % ", ".join(sorted(leaked)))

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
