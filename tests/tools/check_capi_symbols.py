#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C ABI 的**符号面锁**：`uvcpp_c_*` 那些导出符号，一个都不许悄悄消失。

为什么非要有这一条
------------------
这一层的全部价值就是"别人能链上我"。而没有这条门禁时，删掉/改掉一个 C 符号
是**六道门禁全绿**的：C++ 那侧照样编、`tests/capi/` 那两个用例只用到自己调到的
那几个函数、`check_docs.py` 只管文档里的路径与链接。于是"改个名字"和"发布一个
结构上不兼容的版本"在 CI 里长得一模一样，第一个发现的人是**已经编好的 C# 侧**
（`EntryPointNotFoundException`，运行时，不是编译期）。

锁文件 `tests/tools/capi_symbols.lock` 是**写下来的承诺**：这批符号属于
`UVCPP_C_ABI_VERSION` 那一条线，删一个就要 +1（见 `doc/capi-guide.md` §ABI 契约）。
门禁把"实际导出的"与"承诺过的"逐条对，两边都报。

两条判据，前提不同
------------------
  1. **静态**：`src/capi/` 每一份头里被 `UVCPP_C_API` 标过的函数名，都必须在锁里。
     这条**不需要构建**，所以在任何机器上都能跑；它管的是"新加了一个函数却忘了
     把它记进锁"——那种情况判据 2 会报"库里多了个符号"，但报得不具体。
  2. **动态**：构建树里那份库**真的导出**的 `uvcpp_c_*` 符号，与锁逐条相等
     （少一个报"承诺过却没了"，多一个报"没记进锁"）。
     这条要一个 ELF/Mach-O 的动态库 —— 拿不到就退 3（**没判**），不许当绿。

判据 2 为什么用 `nm -D`（动态符号表）而不是 `nm` 的全表：**导出面**是
动态符号表说了算的。静态符号表里有没有那个 `T`，与"别人能不能链上它"是两件事
—— `-fvisibility=hidden`、版本脚本、链接器的 `--exclude-libs` 都只改前者到后者
这一步。拿全表当导出面，会在真正收紧导出时**假绿**。

Windows 那条腿（`dumpbin /exports`）不在这条门禁里：这脚本按 `nm` 的方言解析，
在 msvc 的 runner 上退 3。导出面的 POSIX 判据由 Linux 的 `capi` 格承担，MSVC 那格
判的是"编得出、链得上"（`tests/capi/` 的用例），两条腿合起来盖住这件事 —— 而不是
让这一条假装在 Windows 上也判了。

用法
----
    python3 tests/tools/check_capi_symbols.py                 # 判据 1 + 2（树默认 build-capi）
    python3 tests/tools/check_capi_symbols.py --tree <树> --update   # 重新生成锁
    python3 tests/tools/check_capi_symbols.py --headers-only  # 只跑判据 1（不用构建）

`--update` 只在"头里的声明与库里的导出面**恰好一致**"时写锁：头里有、库里没有的
一律拒绝（退 1），因为把那种漂移写进锁就等于把它固化成一条承诺。

退出码 0 全过；1 有判据红了；3 前提不满足（没库 / 没 nm / 没有锁文件）。
"""

import argparse
import glob
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SRC_CAPI = os.path.join(ROOT, "src", "capi")
LOCK = os.path.join(ROOT, "tests", "tools", "capi_symbols.lock")

# `UVCPP_C_API` 与函数名之间隔着返回类型（`const char*`、`unsigned int`），
# 所以不能写成"宏后面紧跟着名字"。用 `[^;(]*?` 吃掉返回类型那段，且**不许跨过
# 分号或左括号** —— 否则一个没有 `UVCPP_C_API` 的函数会被上一行的宏顺手吃掉。
#
# 扫之前必须**先去掉注释**（`strip_comments()`）。这一条是量出来的，不是想当然：
# 第一版直接扫原文，于是 `uvcpp_c_common.h` 里那句"`uvcpp_c_xxx(句柄, char* buf…)`
# 是唯一例外"的**散文**被当成了一个声明，`--update` 把一个根本不存在的函数写进了
# 锁文件 —— 紧接着判据 2 就报"锁里承诺过、库里没了：uvcpp_c_xxx"，一条彻头彻尾的
# 假红。头里的注释**本来就该**随便举例，能被注释骗到的门禁是门禁的错。
DECL_RE = re.compile(r"UVCPP_C_API\s+[^;()]*?\b(uvcpp_c_[A-Za-z0-9_]+)\s*\(")

# 块注释在前：先吃掉 `/* */`，剩下的 `//` 才不会被块注释里的星号带偏。
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def strip_comments(text):
    """去掉 `/* */` 与 `//` 注释（字符串字面量按原样保留 —— 头里没有）。

    块注释换成等量的空行而不是直接删掉：行号保持不变，脚本将来若要报"第几行"
    才有意义。
    """
    text = BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT_RE.sub("", text)

# 库文件名。顺序有意：**先动态库**。同一个树里同时有 `.so` 与 `.a` 时，导出面
# 是动态库那一份。
LIB_GLOBS = ["libuvcpp.so*", "libuvcpp.dylib", "uvcpp.dll", "libuvcpp.dll",
             "libuvcpp.a", "uvcpp.lib", "libuvcpp.lib"]

# `nm` 那一行：地址、类型、名字。地址宽度随架构变（32 位 8 位十六进制），也有
# 不带地址的形状，所以不锚地址。类型取下面那条白名单。
NM_LINE_RE = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z])\s+(.+)$")

# 把符号算作"导出面"的类型字母。`T`/`W` = 代码（强/弱），`D`/`B`/`R`/`V` = 数据。
# 小写那几个（`t`/`d`/`b`）是**局部**符号 —— 不进导出面，见了就当没看见。
EXPORTED_TYPES = set("TWDRBV")


def exported_symbols(lib):
    """-> (符号集合, 说明)。拿不到就返回 (None, 理由)。"""
    base = os.path.basename(lib)
    if base.endswith(".dylib"):
        # macOS 的 nm 没有 GNU 那个 `-D`；`-gU` 是它那边的"全局 + 已定义"。
        argv = ["nm", "-gU", lib]
    elif base.endswith(".so") or ".so." in base:
        argv = ["nm", "-D", "--defined-only", lib]
    else:
        # 静态库：没有动态符号表，只能看全表里的**全局**符号（上面那条类型白名单
        # 已经把局部符号滤掉了，所以这一条仍然只收导出面）。
        argv = ["nm", "--defined-only", lib]
    try:
        p = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=300)
    except (OSError, subprocess.TimeoutExpired) as e:
        return None, "跑不了 nm（%s）" % e
    if p.returncode != 0:
        return None, "nm 退出 %s：%s" % (p.returncode, (p.stderr or "").strip()[:200])

    want = set()
    for line in (p.stdout or "").splitlines():
        m = NM_LINE_RE.match(line)
        if not m:
            continue
        if m.group(1) not in EXPORTED_TYPES:
            continue
        name = m.group(2).strip()
        # Mach-O 的符号带一个前导下划线（`_uvcpp_c_abi_version`）。
        if name.startswith("_"):
            name = name[1:]
        if name.startswith("uvcpp_c_"):
            want.add(name)
    return want, "%s：%d 个" % (base, len(want))


def lock_file_symbols(text):
    """锁文件里那一行行符号（`#` 开头的注释行与空行跳过）。"""
    out = set()
    for raw in text.splitlines():
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        out.add(s)
    return out


def declared_symbols():
    """头里被 `UVCPP_C_API` 标过的函数名（判据 1 的输入）。"""
    out = set()
    if not os.path.isdir(SRC_CAPI):
        return None
    for f in sorted(os.listdir(SRC_CAPI)):
        if not (f.startswith("uvcpp_c") and f.endswith(".h")):
            continue
        with open(os.path.join(SRC_CAPI, f), encoding="utf-8") as fh:
            for name in DECL_RE.findall(strip_comments(fh.read())):
                out.add(name)
    return out


def find_library(tree):
    for g in LIB_GLOBS:
        hits = sorted(glob.glob(os.path.join(tree, g)))
        hits += sorted(glob.glob(os.path.join(tree, "lib", g)))
        if hits:
            return hits[0]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-capi",
                    help="构建树（默认 build-capi）。判据 2 从中找库。")
    ap.add_argument("--update", action="store_true",
                    help="把实际导出面写回锁文件（**只在有意改面时用**）")
    ap.add_argument("--headers-only", action="store_true",
                    help="只跑判据 1（不找库，不需要构建）")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)

    print("---- 判据 1：头里声明的每一个 C 符号都必须在锁里 ----")
    declared = declared_symbols()
    if declared is None:
        print("  [停] 没有 src/capi/ —— 判据 1 **没判**")
        return 3
    if not declared:
        # 头在、符号一个都没扫到：这是**空转**，不是"全过"。扫到 0 个只可能是
        # 正则不再匹配真实写法（宏改名、返回类型里出现括号…）。
        print("  [红] src/capi/*.h 里一个 `UVCPP_C_API` 声明都没扫到 —— 这条判据"
              "在空转（宏改名了？返回类型里出现括号了？）")
        return 1
    print("  src/capi/*.h 声明了 %d 个函数" % len(declared))

    if args.update:
        if args.headers_only:
            print("--update 与 --headers-only 一起给没有意义（更新要读库）。")
            return 3
        if not os.path.isdir(tree):
            print("没有这棵树：%s —— 判据 2 要先能读到库。" % tree)
            return 3
        lib = find_library(tree)
        if lib is None:
            print("在 %s 里找不到库（%s）" % (tree, "、".join(LIB_GLOBS)))
            return 3
        want, note = exported_symbols(lib)
        if want is None:
            print("  [停] %s" % note)
            return 3
        # 头里声明了、库里**没导出**的：这是漂移，`--update` 不许把它写进锁来
        # "抹平" —— 那正是判据 2 要报的那件事（对已经编好的 C# 侧就是一个
        # `EntryPointNotFoundException`）。多半是加了声明忘了定义，或者定义被
        # `static` / 匿名命名空间吞了。**拒绝写锁**，把名字打出来。
        only_head = sorted(declared - want)
        if only_head:
            print("  [红] 头里声明了、库里没导出的 %d 个 —— **拒绝写锁**（写了就等于"
                  "把这个漂移固化成承诺）：%s" % (len(only_head), ", ".join(only_head)))
            print("       先把它定义出来（或者把声明删掉），再 --update。")
            return 1
        # 库里若有头里没有的（历史遗留、别名），照实写进去 —— 但**不静默丢**，
        # 打印出来让人看见。
        only_lib = sorted(want - declared)
        if only_lib:
            print("  [注意] 库里导出了 %d 个头里没声明的符号，照实写进锁：%s"
                  % (len(only_lib), ", ".join(only_lib[:8])))
        body = sorted(want | declared)
        with open(LOCK, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# C ABI 的符号面锁 —— tests/tools/check_capi_symbols.py --update 生成\n")
            fh.write("# 一行一个导出的 uvcpp_c_* 符号，字典序。删/改任何一个都要把\n")
            fh.write("# UVCPP_C_ABI_VERSION +1（见 doc/capi-guide.md 的 ABI 契约那节）。\n")
            fh.write("# 共 %d 个。\n" % len(body))
            for s in body:
                fh.write(s + "\n")
        print("锁已重写：%s（%d 个）" % (os.path.relpath(LOCK, ROOT), len(body)))
        return 0

    lockset = None
    if os.path.exists(LOCK):
        with open(LOCK, encoding="utf-8") as fh:
            lockset = lock_file_symbols(fh.read())
        print("  锁文件 %d 个符号" % len(lockset))
    else:
        print("  [停] 没有锁文件 %s —— 判据 1 **没判**"
              % os.path.relpath(LOCK, ROOT))
    missing_from_lock = sorted(declared - (lockset or set()))
    if lockset is not None and missing_from_lock:
        print("  [红] 头里声明了、锁里没有的 %d 个：%s"
              % (len(missing_from_lock), ", ".join(missing_from_lock)))
    elif lockset is not None:
        print("  [绿] 头里声明过的都在锁里")

    if args.headers_only:
        if lockset is None:
            return 3
        return 1 if missing_from_lock else 0

    print("\n---- 判据 2：库里实际导出的 C 符号与锁逐条相等 ----")
    if lockset is None:
        print("  [停] 没有锁文件 —— 判据 2 无从对起")
        return 3
    if not os.path.isdir(tree):
        print("  [停] 没有这棵树：%s —— 判据 2 **没判**" % tree)
        return 3
    lib = find_library(tree)
    if lib is None:
        print("  [停] 在 %s 里找不到库（%s）—— 判据 2 **没判**"
              % (tree, "、".join(LIB_GLOBS)))
        return 3
    actual, note = exported_symbols(lib)
    if actual is None:
        print("  [停] %s" % note)
        return 3
    print("  实际导出：%s" % note)

    gone = sorted(lockset - actual)
    extra = sorted(actual - lockset)
    if gone:
        print("  [红] 锁里承诺过、库里没了的 %d 个（**对已编好的 C# 侧就是"
              "运行时 EntryPointNotFoundException**）：%s"
              % (len(gone), ", ".join(gone)))
    if extra:
        print("  [红] 库里导出了、锁里没记的 %d 个：%s"
              % (len(extra), ", ".join(extra)))
        print("       （若是有意加面：跑 --update，并把 UVCPP_C_ABI_VERSION +1）")
    if not gone and not extra:
        print("  [绿] %d 个符号逐条对上" % len(actual))

    red = bool(gone or extra or missing_from_lock)
    print("\n判据：头里声明的都在锁里、库里的与锁逐条相等 → %s"
          % ("全过" if not red else "**有不对的**"))
    return 1 if red else 0


if __name__ == "__main__":
    sys.exit(main())
