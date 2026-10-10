#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C ABI 的**符号面锁**：`uvcpp_c_*` 那些导出符号，一个都不许悄悄消失。

为什么非要有这一条
------------------
这一层的全部价值就是"别人能链上我"。而没有这条门禁时，删掉/改掉一个 C 符号
是**六道门禁全绿**的：C++ 那侧照样编、`tests/capi/` 那几个用例只用到自己调到的
那几个函数、`check_docs.py` 只管文档里的路径与链接。于是"改个名字"和"发布一个
结构上不兼容的版本"在 CI 里长得一模一样，第一个发现的人是**已经编好的 C# 侧**
（`EntryPointNotFoundException`，运行时，不是编译期）。

锁文件 `tests/tools/capi_symbols.lock` 是**写下来的承诺**：这批符号属于
`UVCPP_C_ABI_VERSION` 那一条线，删一个就要 +1（见 `doc/capi-guide.md` §ABI 契约）。
门禁把"实际导出的"与"承诺过的"逐条对，两边都报。

两条判据，前提不同
------------------
  1. **静态**：`src/capi/` 每一份头里被 `UVCPP_C_API` 标过的函数名，都必须在锁里，
     **而且必须落在它那份头对应的那一片**（片与头的对应见下面 `MODULES`）。
     这条**不需要构建**，所以在任何机器上都能跑；它管的是"新加了一个函数却忘了
     把它记进锁"，外加"记错了片"。
  2. **动态**：构建树里那份库**真的导出**的 `uvcpp_c_*` 符号与锁逐条相等
     （少一个报"承诺过却没了"，多一个报"没记进锁"）。**逐片判**，见下。
     这条要一个 ELF/Mach-O 的动态库 —— 拿不到就退 3（**没判**），不许当绿。

判据 2 为什么用 `nm -D`（动态符号表）而不是 `nm` 的全表：**导出面**是
动态符号表说了算的。静态符号表里有没有那个 `T`，与"别人能不能链上它"是两件事
—— `-fvisibility=hidden`、版本脚本、链接器的 `--exclude-libs` 都只改前者到后者
这一步。拿全表当导出面，会在真正收紧导出时**假绿**。

锁是**分片**的，判据 2 逐片判（`1.4.3` 加的，为什么必须这样）
-------------------------------------------------------------
第一版锁是**平**的（183 行一个集合），因为那时只有一个模块维度可开可关，而 CI
那条 `capi` 腿恰好全开。批 3a 加 http2、批 3b 还要加 quic/http3 之后，这条假设
就不成立了：`capi` 那条腿的 flags 里**没有** `-DUVCPP_ENABLE_NGHTTP2=ON`（它
也拿不到那份 nghttp2），于是那片符号在那棵树上**本来就不该导出** —— 平的锁会把
"这一片这棵树没有"和"这个符号被删了"报成同一件事：一个**假红**，而且它红得
很有说服力（"锁里承诺过、库里没了"）。

所以锁按**模块**分片，每片前面一行 `#@ module <名>`，而判据 2 按**每片自己的
那份开关**（从这棵树的 `uvcpp_config.h` 里读，见 `MODULES` 第三列）逐片判：

  开关 ON  → 这一片与导出面**逐条相等**（少一个红、多一个红）；
  开关 OFF → 这一片要求**一个都不许出现**（出现了就是红 —— 一个关着的模块
            不该在库里留下面子，那正是"守卫没生效"的样子）；
             并且**如实印一行「未判」**，不含糊成"绿"。

"这棵树没开那个模块"因此**不是**一句"跳过"，而是一条被印出来的、有内容的判定
—— 见下面那段 `★`。

最后一片叫 `undeclared`：库里有、头里没声明的那些（历史遗留、别名）。它没有
开关，**永远判**，规矩与"开关 ON"的片一样。这一片是空的才是正常的。

用法
----
    python3 tests/tools/check_capi_symbols.py                 # 判据 1 + 2（树默认 build-capi）
    python3 tests/tools/check_capi_symbols.py --tree <树> --update   # 重新生成锁
    python3 tests/tools/check_capi_symbols.py --headers-only  # 只跑判据 1（不用构建）

`--update` 只在"头里的声明与库里的导出面**恰好一致**"时写锁：头里有、库里没有的
一律拒绝（退 1），因为把那种漂移写进锁就等于把它固化成一条承诺。**只对开着的那
几片这么要求**：关着的那几片这棵树量不出来，照头里的声明写，并**逐片印一行
「未判」**（那几片的判据只到判据 1 为止）。

Windows 那条腿（`dumpbin /exports`）不在这条门禁里：这脚本按 `nm` 的方言解析，
在 msvc 的 runner 上退 3。导出面的 POSIX 判据由 Linux 的 `capi` 格承担，MSVC 那格
判的是"编得出、链得上"（`tests/capi/` 的用例），两条腿合起来盖住这件事 —— 而不是
让这一条假装在 Windows 上也判了。

退出码 0 全过；1 有判据红了；3 前提不满足（没库 / 没 nm / 没有锁文件 /
读不出这棵树的模块开关）。
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

# 片名 → (那份头, 这棵树里控制它的那个宏)。
#
# ★ 这张表是**显式**的，不从文件名猜：`uvcpp_c_common.h` 的片名是 `common`
#   （不是 `uvcpp_c_common`），伞头 `uvcpp_c.h` 与私有的 `uvcpp_c_internal.h`
#   **一份声明都不该有**（它们不出现在这里，由 `unregistered_headers()` 反向兜住
#   —— 见那里）。
#
# 第三列的宏就是 `cmake/uvcpp_config.h.in` 里那几枚，也就是 CMake 那 `option()`
# 的产物；它**必须与 `src/capi/` 各文件被谁 `#if` 包着**是同一套判断。写错一列
# 的后果不是"少判一片"，而是"把一个不该导出的模块判成该导出"（假红）或反过来
# （假绿）—— 所以那几片开关在 `tests/tools/check_config_contract.py` 里也有账。
MODULES = [
    # 片名,       那份头（None = 不绑头）, 这棵树里的开关宏（None = 没有开关，永远判）
    ("common",     "uvcpp_c_common.h",  "UVCPP_CAPI_ENABLE"),
    ("net",        "uvcpp_c_net.h",     "UVCPP_NET_ENABLE"),
    ("web",        "uvcpp_c_web.h",     "UVCPP_WEB_ENABLE"),
    ("webapp",     "uvcpp_c_webapp.h",  "UVCPP_WEBAPP_ENABLE"),
    ("http2",      "uvcpp_c_http2.h",   "UVCPP_NGHTTP2_ENABLE"),
    ("quic",       "uvcpp_c_quic.h",    "UVCPP_QUIC_ENABLE"),
    ("http3",      "uvcpp_c_http3.h",   "UVCPP_HTTP3_ENABLE"),
    # db 是**唯一一片有自己开关的**：别的几片都被 CAPI 的守卫链顺手拉起来了
    # （CAPI ⇒ NET + WEB），db 不是任何片的依赖、也不是任何片的下层 ——
    # `CAPI=ON, DB=OFF` 是合法组合，那时 `uvcpp_c_db.h` 里的声明一个都不该存在。
    # 开关写 `UVCPP_DB_ENABLE`（这个模块自己的），不是某个后端那一格：
    # 后端的开关只决定 `uvcpp_c_db_drivers()` 报出什么名字，符号面一个字都不差。
    ("db",         "uvcpp_c_db.h",      "UVCPP_DB_ENABLE"),
    ("undeclared", None,                None),
]

# 片头允许挂一句「(N 个)」的注解：`--update` 自己写的就是那个样子。注解是给人看
# 的散文，不参与判断 —— 判的是每片的**符号集**。但形状必须**唯一**：注解只认
# `(数字 个)` 这一种写法，别的尾巴一律算"这份锁与这个门禁不是一个版本"。
SECTION_RE = re.compile(r"^#@\s*module\s+([A-Za-z0-9_]+)\s*(?:\(\s*\d+\s*个\s*\))?\s*$")
MODULE_NAMES = [m for m, _h, _s in MODULES]
HEADER_OF_MODULE = {m: h for m, h, _s in MODULES if h}
SWITCH_OF_MODULE = {m: s for m, _h, s in MODULES}

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

# 这棵树的 `uvcpp_config.h` 里那一条 `#  define UVCPP_XXX_ENABLE 0|1`。
CONFIG_DEFINE_RE = re.compile(r"^\s*#\s*define\s+(UVCPP_[A-Z0-9_]+)\s+([01])\s*$")


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


def lock_sections(text):
    """-> (片名 -> 符号集, 形状问题列表)。

    形状问题（旧格式的平锁、`#@ module` 行之前就有符号、不认识的片名）都**不静默
    吞掉**：它们全是"这份锁与这个门禁不是一个版本"的表现，而那种情况下把锁当成
    空集去对，比报错更危险（会报出一堆假红，或者说出一句"全过"）。
    """
    sections = {}
    problems = []
    current = None
    for raw in text.splitlines():
        s = raw.strip()
        if not s:
            continue
        m = SECTION_RE.match(s)
        if m:
            name = m.group(1)
            if name not in MODULE_NAMES:
                problems.append("锁里有不认识的片名：`%s`（不在 MODULES 里）" % name)
            current = name
            sections.setdefault(name, set())
            continue
        if s.startswith("#"):
            continue
        if current is None:
            problems.append("符号 `%s` 出现在任何 `#@ module` 行之前" % s)
            continue
        if s not in sections.setdefault(current, set()):
            sections[current].add(s)
    return sections, problems


def flat(sections):
    out = set()
    for v in sections.values():
        out |= v
    return out


def declared_symbols():
    """-> (符号名 -> 片名, 没问题则 None)。

    只扫 `MODULES` 里登记过的那几份头；别的 `uvcpp_c*.h` 由
    `unregistered_headers()` 反向兜（那里报的是"这份头里有 `UVCPP_C_API` 声明，
    但没人把它登记进 MODULES"）。
    """
    if not os.path.isdir(SRC_CAPI):
        return None
    out = {}
    for module, header, _macro in MODULES:
        if header is None:
            continue
        path = os.path.join(SRC_CAPI, header)
        if not os.path.exists(path):
            # 这份头还不存在（模块还没落地）。那不是错：批 3b 之前
            # `uvcpp_c_quic.h` / `uvcpp_c_http3.h` 就是这个状态。
            continue
        with open(path, encoding="utf-8") as fh:
            for name in DECL_RE.findall(strip_comments(fh.read())):
                out[name] = module
    return out


def unregistered_headers():
    """-> [(文件名, 符号名)]：`src/capi/` 里没登记进 `MODULES` 却带声明的头。"""
    known = {h for _m, h, _s in MODULES if h}
    out = []
    for f in sorted(os.listdir(SRC_CAPI)):
        if not (f.startswith("uvcpp_c") and f.endswith(".h")) or f in known:
            continue
        with open(os.path.join(SRC_CAPI, f), encoding="utf-8") as fh:
            for name in DECL_RE.findall(strip_comments(fh.read())):
                out.append((f, name))
    return out


def find_library(tree):
    for g in LIB_GLOBS:
        hits = sorted(glob.glob(os.path.join(tree, g)))
        hits += sorted(glob.glob(os.path.join(tree, "lib", g)))
        if hits:
            return hits[0]
    return None


def tree_flags(tree):
    """-> (宏名 -> 0/1, 那份 config 头路径)。读不出就 (None, None)。"""
    p = os.path.join(tree, "include", "uvcpp", "uvcpp_config.h")
    if not os.path.exists(p):
        hits = glob.glob(os.path.join(tree, "**", "uvcpp_config.h"), recursive=True)
        p = sorted(hits)[0] if hits else None
    if p is None:
        return None, None
    flags = {}
    with open(p, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = CONFIG_DEFINE_RE.match(line)
            if m:
                flags[m.group(1)] = int(m.group(2))
    return flags, p


def module_enabled(module, flags):
    """这一片在这棵树里开着吗。`None` = 没有开关（`undeclared`）→ 永远判。"""
    macro = SWITCH_OF_MODULE.get(module)
    if macro is None:
        return True, "没有开关"
    if macro not in flags:
        return None, "这棵树里读不到 %s" % macro
    return flags[macro] == 1, "%s=%d" % (macro, flags[macro])


def write_lock(sections):
    with open(LOCK, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# C ABI 的符号面锁 —— tests/tools/check_capi_symbols.py --update 生成\n"
                 "#\n"
                 "# 分片写：`#@ module <名>` 开一片，后面一行一个符号，字典序。片名与\n"
                 "# 它那份头的对应在 check_capi_symbols.py 的 MODULES 里。\n"
                 "#\n"
                 "# 为什么分片：一条腿只开得起一部分模块（Linux 那条 `capi` 腿没有\n"
                 "# NGHTTP2 / QUIC / HTTP3），而「锁里有、库里没有」在那条腿上不是\n"
                 "# 缺面、是**这一片本来就不该有**。判据 2 按每片自己的开关逐片判：\n"
                 "# 开着 → 逐条相等；关着 → 一个都不许出现（并如实印「未判」）。\n"
                 "#\n"
                 "# 删/改任何一个都要把 UVCPP_C_ABI_VERSION +1（见 doc/capi-guide.md\n"
                 "# 的 ABI 契约那节）。\n"
                 "# 共 %d 个，%d 片。\n"
                 % (len(flat(sections)),
                    len([s for s in sections.values() if s])))
        for module, _header, _macro in MODULES:
            body = sorted(sections.get(module, set()))
            if not body:
                continue
            fh.write("#@ module %s  (%d 个)\n" % (module, len(body)))
            for s in body:
                fh.write(s + "\n")


def judge(tree, headers_only):
    """两条判据都在这儿。`tree` 已是**绝对路径**（`_main()` 拼好传进来的）。

    分成 `judge()` / `update_lock()` 两个函数而不是一个 main 里的两个分支：
    它们的前提**不一样**（`--update` 只要求"开着的那几片对得上"，判据 2 要求
    "每一片都有结论"），混在一个函数里迟早会互相借到不该借的那个变量。
    """
    print("---- 判据 1：头里声明的每一个 C 符号都必须在锁里、且在它自己那一片 ----")
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
    stray = unregistered_headers()
    if stray:
        print("  [红] 没登记进 MODULES 的头里有 %d 处 `UVCPP_C_API` 声明：%s"
              % (len(stray), ", ".join("%s:%s" % (f, n) for f, n in stray[:8])))
        print("       （它是私有的、还是伞头？那两份都不该有声明。是新模块就把那"
              "一份头 + 它的开关宏加进 check_capi_symbols.py 的 MODULES。）")
        return 1
    per_module = {}
    for name, module in declared.items():
        per_module[module] = per_module.get(module, 0) + 1
    print("  src/capi/*.h 声明了 %d 个函数，分 %d 片（%s）"
          % (len(declared), len(per_module),
             "、".join("%s %d" % (m, per_module[m]) for m in MODULE_NAMES
                       if m in per_module)))

    sections = None
    shape_problems = []
    if os.path.exists(LOCK):
        with open(LOCK, encoding="utf-8") as fh:
            sections, shape_problems = lock_sections(fh.read())
    if sections is None:
        print("  [停] 没有锁文件 %s —— 判据 1 **没判**"
              % os.path.relpath(LOCK, ROOT))
    elif shape_problems:
        print("  [红] 锁文件的形状不对（%d 处）—— 这份锁与这个门禁不是一个版本，"
              "跑 --update 重新生成：" % len(shape_problems))
        for s in shape_problems[:8]:
            print("       %s" % s)
        return 1

    lockset = flat(sections) if sections is not None else None
    # 反向表：符号 -> 它在哪一片。判据 1 的第二半靠它。
    where = {}
    for module, body in (sections or {}).items():
        for n in body:
            where.setdefault(n, module)

    missing_from_lock = []
    misplaced = []
    for name in sorted(declared):
        if lockset is not None and name not in lockset:
            missing_from_lock.append(name)
        elif lockset is not None and where.get(name) != declared[name]:
            misplaced.append((name, declared[name], where.get(name)))
    if lockset is None:
        pass
    elif missing_from_lock:
        print("  [红] 头里声明了、锁里没有的 %d 个：%s"
              % (len(missing_from_lock), ", ".join(missing_from_lock)))
    if misplaced:
        print("  [红] 片名记错了的 %d 个（左边是它那份头的片，右边是锁里那一片）：%s"
              % (len(misplaced),
                 ", ".join("%s：%s≠%s" % t for t in misplaced[:8])))
    if lockset is not None and not missing_from_lock and not misplaced:
        print("  [绿] 头里声明过的都在锁里，且片名与头对应")

    if headers_only:
        if lockset is None:
            return 3
        return 1 if (missing_from_lock or misplaced) else 0

    print("\n---- 判据 2：库里的导出面与锁**逐片**相等 ----")
    if lockset is None:
        print("  [停] 没有锁文件 —— 判据 2 无从对起")
        return 3
    if not os.path.isdir(tree):
        print("  [停] 没有这棵树：%s —— 判据 2 **没判**" % tree)
        return 3
    flags, cfg = tree_flags(tree)
    if flags is None:
        print("  [停] 在 %s 里找不到 uvcpp_config.h —— 读不出这棵树的模块开关，"
              "也就分不清「这一片本该有」和「这一片被删了」（判据 2 **没判**）" % tree)
        return 3
    print("  这棵树的模块开关（%s）" % os.path.relpath(cfg, ROOT))
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

    red = False
    judged = 0
    not_judged = 0

    # 先逐片判。**关着的那几片也要说话**：说"一个都没出现"或"出现了几个"。
    for module, _header, _macro in MODULES:
        expected = (sections or {}).get(module, set())
        on, why = module_enabled(module, flags)
        if on is None:
            print("  [停] %s 那一片：%s —— **没判**" % (module, why))
            not_judged += 1
            continue
        if not on:
            leaked = sorted(expected & actual)
            if leaked:
                print("  [红] `%s`（%s）—— 这棵树**没开**这个模块，库里却导出了它"
                      "的 %d 个符号：%s" % (module, why, len(leaked),
                                            ", ".join(leaked[:8])))
                red = True
            else:
                print("  [绿] `%s`（%s，未判）—— 这一片 %d 个符号这棵树一个都没"
                      "导出，与「关着」对得上" % (module, why, len(expected)))
                not_judged += 1
            continue
        if not expected and not [n for n, m in declared.items() if m == module]:
            # 这一片还没落地（批 3b 之前没有 quic 那份头）。开关开着、锁里空、
            # 头里也空 —— 只可能是"这个模块在这条线上还没实现"，印一行，不假装判过。
            print("  [绿] `%s`（%s，未判）—— 锁里与头里这一片都还是空的"
                  % (module, why))
            not_judged += 1
            continue
        gone = sorted(expected - actual)
        only_head = sorted({n for n, m in declared.items() if m == module} - actual)
        if gone:
            print("  [红] `%s`（%s）锁里承诺过、库里没了的 %d 个（**对已编好的 C# 侧"
                  "就是运行时 EntryPointNotFoundException**）：%s"
                  % (module, why, len(gone), ", ".join(gone[:8])))
            red = True
        if only_head:
            print("  [红] `%s`（%s）头里声明了、库里没导出的 %d 个（多半是加了声明"
                  "忘了定义，或者定义被 static / 匿名命名空间吞了）：%s"
                  % (module, why, len(only_head), ", ".join(only_head[:8])))
            red = True
        if not gone and not only_head:
            print("  [绿] `%s`（%s）%d 个符号逐条对上" % (module, why, len(expected)))
            judged += 1

    # 再有"哪一片都装不下"的：库里导出了、锁里根本没记的。
    extra = sorted(actual - lockset)
    if extra:
        print("  [红] 库里导出了、锁里没记的 %d 个：%s"
              % (len(extra), ", ".join(extra[:8])))
        print("       （若是有意加面：跑 --update，并把 UVCPP_C_ABI_VERSION +1）")
        red = True

    print("\n判据：头里声明的都在锁里且片名对、每片按自己的开关对上了 → %s"
          % ("全过" if not red else "**有不对的**"))
    if not_judged:
        print("（其中 %d 片**未判**：库那边量不出来 —— 那是「没判」，不是「通过」；"
              "判过的 %d 片。想判那几片就把那个模块打开再跑一次。）"
              % (not_judged, judged))
    return 1 if red else 0


def update_lock(tree):
    """`--update` 的全部逻辑（单独一个函数：它读库的方式与判据 2 不同）。"""
    declared = declared_symbols()
    if not declared:
        print("[停] src/capi/ 里一个声明都没扫到 —— 拒绝写锁（那会把锁写成空的）")
        return 3
    if not os.path.isdir(tree):
        print("没有这棵树：%s —— 判据 2 要先能读到库。" % tree)
        return 3
    flags, cfg = tree_flags(tree)
    if flags is None:
        print("在 %s 里找不到 uvcpp_config.h —— 读不出模块开关，拒绝写锁。"
              % tree)
        return 3
    lib = find_library(tree)
    if lib is None:
        print("在 %s 里找不到库（%s）" % (tree, "、".join(LIB_GLOBS)))
        return 3
    want, note = exported_symbols(lib)
    if want is None:
        print("  [停] %s" % note)
        return 3
    print("  实际导出：%s" % note)

    sections = {}
    for module, _header, _macro in MODULES:
        sections[module] = {n for n, m in declared.items() if m == module}
    sections["undeclared"] = set(want) - set(declared)

    # 头里声明了、库里**没导出**的：这是漂移，`--update` 不许把它写进锁来
    # "抹平" —— 那正是判据 2 要报的那件事（对已经编好的 C# 侧就是一个
    # `EntryPointNotFoundException`）。**只对开着的那几片这么要求**：关着的
    # 那几片这棵树根本量不出来（比如 `capi` 那条腿没开 NGHTTP2，可 h2 那一片
    # 的头就在那里），拿它去要求"必须有导出"是把一条腿的配置写进承诺里。
    refused = False
    for module in MODULE_NAMES:
        on, why = module_enabled(module, flags)
        if on is None:
            print("  [停] `%s` 那一片：%s" % (module, why))
            return 3
        body = sorted(sections.get(module, set()))
        if not on:
            print("  [注意] `%s`（%s，未判）—— 这棵树量不出这一片，照头里的声明写"
                  "（%d 个）" % (module, why, len(body)))
            continue
        if body and module != "undeclared":
            gone = sorted(set(body) - want)
            if gone:
                print("  [红] `%s`（%s）头里声明了、库里没导出的 %d 个 —— "
                      "**拒绝写锁**（写了就等于把这个漂移固化成承诺）：%s"
                      % (module, why, len(gone), ", ".join(gone[:8])))
                refused = True
    if refused:
        print("       先把它定义出来（或者把声明删掉），再 --update。")
        return 1
    if sections["undeclared"]:
        print("  [注意] 库里导出了 %d 个头里没声明的符号，照实写进 `undeclared` "
              "那一片：%s" % (len(sections["undeclared"]),
                             ", ".join(sorted(sections["undeclared"])[:8])))

    write_lock(sections)
    print("锁已重写：%s（%d 个，%d 片）"
          % (os.path.relpath(LOCK, ROOT), len(flat(sections)),
             len([s for s in sections.values() if s])))
    return 0


def _main():
    """入口：先把 `--update` 分流出去（它的前提与判据 1/2 不是一套）。"""
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-capi",
                    help="构建树（默认 build-capi）。判据 2 从中找库，"
                         "并从中读模块开关。")
    ap.add_argument("--update", action="store_true",
                    help="把实际导出面写回锁文件（**只在有意改面时用**）")
    ap.add_argument("--headers-only", action="store_true",
                    help="只跑判据 1（不找库、不读模块开关）")
    args = ap.parse_args()
    if args.update:
        if args.headers_only:
            print("--update 与 --headers-only 一起给没有意义（更新要读库）。")
            return 3
        return update_lock(os.path.join(ROOT, args.tree))
    return judge(os.path.join(ROOT, args.tree), args.headers_only)


if __name__ == "__main__":
    sys.exit(_main())
