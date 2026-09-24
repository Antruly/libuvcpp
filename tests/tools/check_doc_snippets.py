#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""示例门禁：文档里的 ```cpp 片段必须真编得过。

## 为什么要有这条

文档腐烂**不会**有人报 bug —— 读者照着抄、编不过，然后自己想办法，最后不吭声。
本仓已经栽过一次：两版 README 的快速上手各有一条**完整程序**形状的例子早就编不过
（`server.on_connection()` 那套 API 早改成 `listen(cb)` 的参数了），而没有任何东西
会出声。这不是错别字，是"文档与代码各说各话"，跟 `check_docs.py` 防的是同一类东西，
只是那条只能防住链接、路径和旋钮名，防不住**签名**。

## 约定（全仓统一，写在 `CONTRIBUTING.md`）

  ```cpp                       ⇒ **完整翻译单元**（自带 `#include`），必须编得过
  ```cpp + 首行注释            ⇒ `// doc-snippet: fragment — <理由>` 明示"这不是独立
                                   TU"（摘录，或**故意编不过的反例**）；不编，**理由必填**
  文档里的 `<!-- doc-snippets: fragments-default -->`
                               ⇒ 该篇默认全是片段；要编的片段逐条写
                                  `// doc-snippet: compile`

**"完整 TU" 不等于"完整程序"**：本脚本用 `-c` 只编不链，**不要求 `main`** ——
一个自带 include 的函数或类定义就是合法 TU。功能文档不必给每段套一个 `main`
（那才是把文档写肿的元凶）。附带的好处是：片段的形参类型本身就被断言了，
**改过的回调签名会被抓住**，正是 README 那次腐烂的形态。

## 编在哪儿：包，而且**一个 `-D` 都不加**

`-I <pkg>/include`、零 `-D`。这不是图省事，是**规矩**：包里的 `uvcpp/uvcpp_config.h`
设计成"消费者什么都不定义才是对的"，外部定义且与本次构建不一致的值是**硬 `#error`**。
传 `-D` 是自我否定，任何 per-page 的宏表都是假红制造机。
`check_config_contract.py` 已用"裸调用 + 零个 `-D`"证明过同一条契约。

包里 `include/` 就是完整的 include 集（`uv.h` / `nlohmann/` / `zlib.h` 都随包发），
**不需要 OpenSSL 头、也不需要 nghttp2 头** —— 公开头一律前置声明 + PIMPL。
（注意：`grep '<openssl/'` **会**命中 `src/ssl/uvcpp_ssl_context.h`，但那是**注释**。
这一条是编译出来的结论，不是 grep 出来的 —— 验证要用编译器。）

## 判据与退出码（继承 `check_doc_versions.py` 的三值约定）

  * `0` 全过。
  * `1` 判红了：某条片段编不过、标了 `fragment` 却没写理由、片段没有本库头、
    围栏信息串写错、围栏没闭合……
  * `3` 前提不满足、**这条门禁没判**：包里没有这条片段要的模块/头、
    片段要的头不在包里（`<openssl/…>` 这种）、一条 cpp 片段都没扫到。
  **`1` 优先于 `3`** —— 已经判出来的红不该被"没判成的那部分"盖掉。

包缺模块而跳过的片段会打 `[跳]` 并说明缺哪个开关，头不在包里打 `[停]`。两者都**不是红**：
那是环境缺口，不是文档腐烂。报红就是假红，而假红会训练人忽略门禁。

## 反空转（本仓规矩）

靠"什么都没找到"通过的判据长得和全绿一模一样。所以：

  * 标了 `fragment` 却**不写理由** ⇒ 红。
  * `fragments-default` 的页面**一条都没编过** ⇒ 红（页面级开关不许变成一揽子退出）。
  * 片段**没有包含任何本库的头** ⇒ 红：登记不了模块需求就等于绕过了检查。
  * 围栏信息串**以 `cpp` 开头但不止于此**（```` ```cppp ````）⇒ 红：多打一个字
    就把一条示例静默关了，而"少判了一条"和"判过了"长得一样。

用法：
    python tests/tools/check_doc_snippets.py --pkg <package dir> --cxx g++
    python tests/tools/check_doc_snippets.py --toc doc/net-guide.md
"""

import argparse
import os
import re
import subprocess
import sys

# 与 check_docs.py 同一个理由：Windows 的 CI runner 默认用 GBK 解 stdout，
# 中文判据会打成乱码 —— 而红的那几行恰好是最需要看清的。
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (ValueError, OSError):
        pass

failures = []


def fail(what):
    failures.append(what)
    print("  [红] %s" % what)


def ok(what):
    print("  [绿] %s" % what)


# ---------------------------------------------------------------------------
# 文档与围栏
# ---------------------------------------------------------------------------

FENCE_RE = re.compile(r"^[ \t]*```(.*)$")

# 只有这两种写法算"这是 C++ 片段"。**信息串以 cpp/c++ 开头但不止于此的写法要判红**：
# 那正是"多打一个字就静默关掉一条示例"的形状。
CPP_INFO = ("cpp", "c++")

MARK_COMPILE_RE = re.compile(r"^//\s*doc-snippet:\s*compile\b(.*)$")
MARK_FRAGMENT_RE = re.compile(r"^//\s*doc-snippet:\s*fragment\b(.*)$")

# 页面级默认。写成 HTML 注释，渲染时看不见。**只匹配前缀**，后面可以接理由。
FRAGMENTS_DEFAULT = "<!-- doc-snippets: fragments-default"

# 理由前面的分隔符（`—` / `-` / `:` / 空白）剥掉；剥完还是空的就是没写理由。
REASON_SEP_RE = re.compile(r"^[\s—–\-:：,，]+")


def scan_docs(root):
    """要扫的文档：仓库根这一层的 `*.md` + `doc/` 下的 `*.md`。

    与 `check_docs.py` 的 `collect_docs()` **逐字同规则、非递归**：两个门禁必须对
    "什么算一篇文档"有同一个定义，否则会出现"链接门禁看得见、示例门禁看不见"的
    文档 —— 那种文档里的片段永远不会被编。往 `doc/` 下开子目录就是这种情形。

    `RELEASE.md` **在范围内，这是有意的**：变更历史里的代码片段确实"当时编得过"，
    但本仓的 changelog 是当用法示例读的，一段停在过去的示例比没有更坏。真到了
    "这条记录只能代表过去"的那天，正确的做法是把它标成片段并写下理由，而不是
    把整篇排除掉 —— 排除掉之后它连"有人在看"都没有了。
    """
    docs = []
    for name in sorted(os.listdir(root)):
        if name.lower().endswith(".md"):
            docs.append(name)
    docdir = os.path.join(root, "doc")
    if os.path.isdir(docdir):
        for name in sorted(os.listdir(docdir)):
            if name.lower().endswith(".md"):
                docs.append("doc/" + name)
    return docs


def extract_fences(text):
    """抽出所有围栏，保留**起始行号**（报红时要指得到行，才有人愿意去改）。

    返回 `(fences, unclosed_lineno)`：围栏没闭合会把后面整篇吞掉，是文档 bug，
    单独报出来 —— 静默少几条片段就是静默少几条判据。
    """
    out, body, info, start = [], None, "", 0
    for lineno, ln in enumerate(text.splitlines(), 1):
        m = FENCE_RE.match(ln)
        if m:
            if body is None:
                body, info, start = [], m.group(1).strip(), lineno
            else:
                out.append({"lineno": start, "info": info,
                            "body": "\n".join(body)})
                body = None
            continue
        if body is not None:
            body.append(ln)
    return out, (start if body is not None else 0)


def first_line_marker(body):
    """看片段首行有没有 `// doc-snippet: …` 标记，返回 `(kind, reason)`。

    `kind` 是 `""` / `"compile"` / `"fragment"`。空白行跳过（片段开头的空行很常见）。
    """
    for ln in body.splitlines():
        if not ln.strip():
            continue
        m = MARK_COMPILE_RE.match(ln.strip())
        if m:
            return "compile", ""
        m = MARK_FRAGMENT_RE.match(ln.strip())
        if m:
            return "fragment", REASON_SEP_RE.sub("", m.group(1)).strip()
        return "", ""
    return "", ""


# ---------------------------------------------------------------------------
# 片段要哪些模块 / 哪些头
# ---------------------------------------------------------------------------

INC_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^">]+)[">]', re.M)

# 本库的公开头：`<模块/…>` 或聚合头 `<uvcpp.h>`。**只有这些才算"这是本库的片段"**。
LIB_PREFIXES = ("uvcpp/", "handle/", "req/", "net/", "web/", "webapp/",
                "ssl/", "http2/", "expand/", "wsdl/")
LIB_EXACT = ("uvcpp.h",)

# 随包发的第三方头（`package_release.py` 会把它们放进 `include/`）。
BUNDLED = ("uv.h", "uv/", "nlohmann/", "zlib.h", "zconf.h")

# 公开头里**没有**、包里也**没有**的第三方头。文档片段真用上它们就是**环境缺口**
# 而不是文档腐烂 —— 报 `[停]` 并退 3。报红的话就是**假红**，而假红会训练人忽略门禁。
NOT_BUNDLED = ("openssl/", "nghttp2/")

# 头 → 记录"这个模块在不在这份构建里"的那个宏。
#
# 判据不是"这个模块的头有没有守卫"，而是**登记了会不会让结论失真**（两个方向都算）：
#   * 宏为 0 时这个模块的类**会消失** ⇒ 不登记就是**假红**（类名变成未声明类型，
#     gcc 的原话是 `variable or field 'doc_x' declared void`），
#     必须登记。`web/` 15/15、`ssl/` 3/3、`http2/` 4/4 个头整段套在宏里，属于这一档。
#   * 宏为 0 时头**照样完整**（编得过就是编得过）⇒ 登记只会造出**假 `[跳]`**，
#     把一条本该判绿的片段改判成"没判"、顺带把退出码从 0 抬到 3。不登记。
#
# `webapp/` 落在中间，**只有 1/21 个头带守卫**（`src/webapp/uvcpp_web_ws_client.h:52`
# 的 `#if UVCPP_WEBAPP_ENABLE`，到 402 行）。看上去像第二档，但恰恰是那一个头会让
# `uvcpp_web_ws_client` 的片段在 WEBAPP=0 的包上**假红** —— 所以照样登记。**别按"多数
# 头没守卫"把它删掉**：少数派那个才是决定这条记录的那一个。
#
# `wsdl/` 与 `web/` 同档，但机制更彻底一点：它的**两个**公开头整段套在
# `#if UVCPP_WSDL_ENABLE` 里（宏为 0 时类直接消失），而且模块关掉时 `CMakeLists.txt`
# 的 install 规则**根本不装**它们。两条路都通往假红，所以照样登记。**注意头在不在
# 包里**这一半只是**本地**的形状：CI 打包 job 走 `package_release.py`，它是从 `src/`
# 逐目录拷头的、与开关无关 —— 所以在那个包里 wsdl 头**在**、模块**没编**，
# 只有这张表能把它判成"没编那个模块"而不是编不过。
#
# 不登记的：`net/` `expand/` `handle/` `req/` `uvcpp/`（实测各 0 个头用模块宏）。
# 注意 `UVCPP_NET_ENABLE` **在生成头里是有定义的**（`cmake/uvcpp_config.h.in:28`），
# 所以"没登记"不等于"没这个宏"，只是公开头一个都不用、宏为 0 时头依然完整。
# `UVCPP_ENABLE_MEMORY_POOL` 同理不是模块缺席开关：它在 `src/uvcpp/uvcpp_alloc.h:19`
# 选的是走池还是走 malloc，**两支都能编**，所以 `expand/` 不挂它。
#
# `webapp/` 那一行是量出来的，不是推出来的：同一段 `<webapp/uvcpp_web_ws_client.h>`
# 的片段，对着把 `UVCPP_WEBAPP_ENABLE` 改成 0 的包跑 —— 现行表 `[跳]` 退 3，
# 把这一行抽掉则编不过退 1。抽掉它就会造出一个**假红**，所以它承重。
MODULE_REQ = {
    "web/": "UVCPP_WEB_ENABLE",
    "webapp/": "UVCPP_WEBAPP_ENABLE",
    "ssl/": "UVCPP_OPENSSL_ENABLE",
    "http2/": "UVCPP_NGHTTP2_ENABLE",
    "wsdl/": "UVCPP_WSDL_ENABLE",
}

CONFIG_REL = os.path.join("include", "uvcpp", "uvcpp_config.h")

# 与 check_config_contract.py:72 同一个正则 —— 生成头的形状两处必须读得一样。
DEF_RE = re.compile(r"^#\s*define\s+(UVCPP_[A-Z0-9_]+)\s+(\S+)\s*$", re.M)


def read_config(pkg):
    p = os.path.join(pkg, CONFIG_REL)
    if not os.path.exists(p):
        return None, "包里没有 %s（这是构件目录不是包？）" % CONFIG_REL
    with open(p, "rb") as fh:
        text = fh.read().decode("utf-8", errors="replace")
    return {m.group(1): m.group(2) for m in DEF_RE.finditer(text)}, None


def classify_headers(body):
    """把片段 include 的头分成：本库的 / 随包发的第三方 / 包里没有的第三方。"""
    headers = INC_RE.findall(body)
    lib = [h for h in headers
           if h in LIB_EXACT or h.startswith(LIB_PREFIXES)]
    bundled = [h for h in headers if h.startswith(BUNDLED)]
    not_bundled = [h for h in headers if h.startswith(NOT_BUNDLED)]
    return headers, lib, bundled, not_bundled


def requirement_of(header):
    """这条本库头需要哪个开关（`None` = 只要头在包里就行）。"""
    for prefix, macro in MODULE_REQ.items():
        if header.startswith(prefix):
            return prefix, macro
    return None, None


# ---------------------------------------------------------------------------
# 编译
# ---------------------------------------------------------------------------

def is_msvc(cxx):
    base = os.path.basename(cxx).lower()
    return base.startswith("cl") and "clang" not in base


def compile_cmd(cxx, pkg, src, obj):
    """与 `check_config_contract.py:305` 同形（那边只是不带 `/utf-8`）。

    **两处故意不同**，都是为了不制造假红：

      * 生成物写成 UTF-8 带 BOM，并且 MSVC 侧加 `/utf-8`。那边不加是因为它测的
        就是"默认消费条件"；而文档片段的**中文注释不是被测物** —— 不加的话每条带
        中文注释的片段在 MSVC 上都会红，压力会变成"中文文档用英文写注释"。
      * 只编到 `.o`，**不链接**。文档腐烂的形态是签名/名字/枚举/头路径漂移，
        `-c` 全能抓；链接还要牵扯 `-L/-luvcpp`、rpath 与运行时 dll 搜索路径。
      * MSVC 侧再加 `/Zc:preprocessor`。**这一条不是"让红变绿"**：文档里
        `UVCPP_JSON_FIELDS` 那批片段在 cl 的传统预处理器下**真的**编不过
        （见 `src/uvcpp/uvcpp_json_reflect.h` 里那个宏上面那段），而本门的判断轴
        是"签名/名字/枚举/头路径有没有漂移"，不是"MSVC 默认是哪个预处理器"。
        MSVC 那条硬要求由三处各自兜住：头里那句 `static_assert`（说人话）、
        `CMakeLists.txt` 给导出目标挂的 INTERFACE 选项、以及 `doc/json-reflect-
        guide.md` 里那段说明。要测"默认消费条件"是 `check_config_contract.py` 的事。
    """
    inc = os.path.join(pkg, "include")
    if is_msvc(cxx):
        return [cxx, "/nologo", "/std:c++14", "/EHsc", "/utf-8",
                "/Zc:preprocessor", "/I" + inc, "/c", src, "/Fo:" + obj]
    return [cxx, "-std=c++11", "-I" + inc, "-c", src, "-o", obj]


def write_tu(path, body):
    """写成一个翻译单元：**UTF-8 带 BOM**，行尾固定 LF。"""
    data = b"\xef\xbb\xbf" + body.encode("utf-8")
    with open(path, "wb") as fh:
        fh.write(data)
    with open(path, "rb") as fh:
        head = fh.read(3)
    if head != b"\xef\xbb\xbf":
        fail("%s 的 BOM 没写进去 —— MSVC 会按系统代码页解它" % path)


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


# 两家编译器报的是同一个位置，写法不同：
#   gcc/clang:  <file>.cpp:9:12: error: ...
#   MSVC:       <file>.cpp(9) : error C2039: ...
POS_RE = [
    re.compile(r"^(.*?):(\d+):(\d+):\s*(.*)$"),
    re.compile(r"^(.*?)\((\d+)\)\s*:\s*(?:fatal )?error\s*(.*)$"),
]


def first_error(out, src, fence_line):
    """挑一行最像错误的输出，并把**生成物里的行号换回文档里的行号**。

    编译器说的是 `s07_README_md.cpp:9:12`，而读者手里是 `README.md`。片段正文在
    生成物里从第 1 行开始，而它在文档里从 `fence_line + 1` 行开始，所以生成物的
    第 N 行 = 文档第 `fence_line + N` 行。不换回去的话，报出来的位置指向一个临时
    文件 —— 等于没指。
    """
    for ln in out.splitlines():
        if "error" not in ln.lower():
            continue
        s = ln.strip()
        for rx in POS_RE:
            m = rx.match(s)
            if not m:
                continue
            f, num, rest = m.group(1), int(m.group(2)), m.group(m.lastindex)
            if os.path.basename(f) == os.path.basename(src):
                return "文档第 %d 行：%s" % (fence_line + num, rest.strip()[:150])
            return s[:160]
        return s[:160]
    lines = [l for l in out.splitlines() if l.strip()]
    return lines[0].strip()[:160] if lines else "(编译器没有输出)"


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pkg", help="package_release.py 的 stage 目录")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--root", default=None,
                    help="仓库根（默认取本脚本的上两级目录）")
    ap.add_argument("--docs", nargs="+", default=None,
                    help="只判这几篇（相对 root）；给对照组用")
    ap.add_argument("--work", default="",
                    help="放生成的 .cpp/.obj 的目录（默认 <pkg>/_snippets）")
    ap.add_argument("--toc", default=None,
                    help="只打印这一篇的锚点，不编译 —— 让作者抄而不是猜")
    args = ap.parse_args()

    root = os.path.abspath(args.root or
                           os.path.join(os.path.dirname(__file__), "..", ".."))
    print("仓库根: %s" % root)

    if args.toc:
        return print_toc(root, args.toc)

    if not args.pkg:
        print("  [停] 没给 --pkg：这条门禁要对着一个包跑")
        print("\n==== 汇总 ====")
        print("前提不满足，退出 3（**片段门禁一条都没判**）")
        return 3
    pkg = os.path.abspath(args.pkg)
    print("包: %s" % pkg)
    print("编译器: %s" % args.cxx)

    cfg, err = read_config(pkg)
    if cfg is None:
        print("  [停] %s" % err)
        print("\n==== 汇总 ====")
        print("前提不满足，退出 3（**片段门禁一条都没判**）")
        return 3
    mods = " ".join("%s=%s" % (k.replace("UVCPP_", "").replace("_ENABLE", ""), cfg[k])
                    for k in sorted(cfg) if k.endswith("_ENABLE"))
    print("包里的模块: %s" % mods)

    docs = args.docs or scan_docs(root)
    print("  扫了 %d 个 md：%s" % (len(docs), " ".join(docs)))

    work = os.path.abspath(args.work) if args.work else os.path.join(pkg, "_snippets")
    os.makedirs(work, exist_ok=True)

    print("\n---- 片段 ----")
    n_cand = n_compiled = n_frag = n_skip = n_bytes = 0
    for rel in docs:
        path = os.path.join(root, rel.replace("/", os.sep))
        if not os.path.exists(path):
            fail("%s 不存在" % rel)
            continue
        with open(path, "rb") as fh:
            text = fh.read().decode("utf-8", errors="replace")
        fences, unclosed = extract_fences(text)
        if unclosed:
            fail("%s 第 %d 行的围栏没有闭合 —— 它把后面整篇都吞了"
                 % (rel, unclosed))
        frag_default = FRAGMENTS_DEFAULT in text

        page = {"rel": rel, "cand": 0, "compiled": 0, "frag": 0,
                "skip": 0, "red": 0, "bytes": 0}
        for idx, f in enumerate(fences, 1):
            info = f["info"].lower()
            if info not in CPP_INFO:
                # 信息串"以 cpp 开头但不止于此"——多打一个字就把一条示例静默关了。
                if info.startswith(CPP_INFO):
                    fail("%s 第 %d 行的围栏信息串是 `%s`，不是 `cpp` —— "
                         "写错的标记会让这条片段被静默跳过"
                         % (rel, f["lineno"], f["info"]))
                    page["red"] += 1
                continue
            page["cand"] += 1
            n_cand += 1
            where = "%s 第 %d 行" % (rel, f["lineno"])

            kind, reason = first_line_marker(f["body"])
            if frag_default and kind != "compile":
                # 页面级标记**本身就是理由**，所以不要求逐条再写一遍。
                page["frag"] += 1
                n_frag += 1
                continue
            if kind == "fragment":
                page["frag"] += 1
                n_frag += 1
                if not reason:
                    fail("%s 标了 `fragment` 却没写理由 —— 没理由就等于"
                         "没有理由地不判它" % where)
                    page["red"] += 1
                continue

            headers, lib, bundled, not_bundled = classify_headers(f["body"])
            if not lib:
                # 只 include 了随包发的第三方头（`uv.h` / `nlohmann/…`）也算这一类：
                # 它们确实在包里、编得过，但它们说明不了**用的是本库的哪个模块**，
                # 所以照样登记不了模块需求。
                what = ("只 include 了随包发的第三方头 %s" % " ".join(sorted(set(bundled)))
                        if bundled else
                        "包含的头是 %s" % " ".join(sorted(set(headers)))
                        if headers else "一个 include 都没有")
                fail("%s 没有包含任何本库的头（%s）—— 登记不了它属于哪个模块，"
                     "等于绕过了这条门禁" % (where, what))
                page["red"] += 1
                continue
            if not_bundled:
                print("  [停] %s 要的头 %s 不在包里（公开头也不含它）——"
                      " 这是环境缺口，不是文档腐烂"
                      % (where, " ".join(sorted(set(not_bundled)))))
                page["skip"] += 1
                n_skip += 1
                continue

            missing = [h for h in lib
                       if not os.path.exists(os.path.join(pkg, "include",
                                                          h.replace("/", os.sep)))]
            if missing:
                print("  [跳] %s 要的 %s 不在包里 —— 这个包没编那个模块"
                      % (where, " ".join(sorted(set(missing)))))
                page["skip"] += 1
                n_skip += 1
                continue

            unmet = []
            for h in lib:
                prefix, macro = requirement_of(h)
                if macro and cfg.get(macro) != "1":
                    unmet.append((prefix, macro, cfg.get(macro)))
            if unmet:
                shown = sorted(set("%s 需要 %s（包里是 %s）" % u for u in unmet))
                print("  [跳] %s %s" % (where, "；".join(shown)))
                page["skip"] += 1
                n_skip += 1
                continue

            src = os.path.join(work, "s%02d_%s.cpp"
                               % (idx, re.sub(r"\W+", "_", rel)[:40]))
            obj = os.path.splitext(src)[0] + (".obj" if is_msvc(args.cxx) else ".o")
            write_tu(src, f["body"] + "\n")
            rc, out = run(compile_cmd(args.cxx, pkg, src, obj), work)
            if rc != 0:
                fail("%s 编不过：%s" % (where, first_error(out, src, f["lineno"])))
                for ln in out.splitlines()[:8]:
                    print("        %s" % ln)
                page["red"] += 1
                continue
            page["compiled"] += 1
            page["bytes"] += len(f["body"])
            n_compiled += 1
            n_bytes += len(f["body"])

        # 页面级反空转：`fragments-default` 是一次**面向整篇**的豁免，所以它必须
        # 留下至少一条仍然被编的片段 —— 否则那一行就等于"把这一篇从门禁里摘出去"，
        # 而摘出去之后它长得和全绿一模一样。
        #
        # 只对**页面级**标记生效：逐条写了理由的 `fragment` 是作者在 diff 里
        # 明明白白做的选择，理由会被打印出来（`testing-guide.md` 那两条反例就是
        # 全部标了片段，那是合法的）。
        # `page["cand"]` 那个前置不能省：抽取器要是坏了（一条都没抽到），这条会
        # 抢在全局反空转前面报"页面级豁免"，把"门禁没抽到东西"说成"这一篇有问题"。
        if frag_default and page["cand"] and not page["compiled"] \
                and not page["skip"] and not page["red"]:
            fail("%s 标了 fragments-default，却一条都没编过（%d 条候选）——"
                 " 页面级豁免不许变成一揽子退出，至少留一条标"
                 " `// doc-snippet: compile`" % (rel, page["cand"]))
            page["red"] += 1

        print("  [%s] %-26s 编过 %d/%d 条，片段 %d 条%s"
              % ("红" if page["red"] else ("跳" if page["skip"] else "绿"),
                 rel, page["compiled"], page["cand"], page["frag"],
                 "（%d B）" % page["bytes"] if page["bytes"] else ""))

    print("\n==== 汇总 ====")
    print("候选 %d 条：编过 %d 条（%d B），标记为片段 %d 条，跳过 %d 条"
          % (n_cand, n_compiled, n_bytes, n_frag, n_skip))
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures:
            print("  - %s" % f.splitlines()[0])
        # 有红就报红：前提不够不该把已经判出来的红盖掉。
        return 1
    if n_cand == 0:
        print("前提不满足，退出 3（**片段门禁一条都没判**：一条 ```cpp 都没扫到 ——"
              " 要么抽取器坏了，要么这个仓库真的没有示例）")
        return 3
    if n_skip:
        print("前提不满足，退出 3（判过的都过了，但有 %d 条因为包缺模块/缺头没判 ——"
              " 见上面的 `[跳]`/`[停]`）" % n_skip)
        return 3
    print("全过（%d 条片段都编过）" % n_compiled)
    return 0


def print_toc(root, rel):
    """按 `check_docs.py` 的 `anchors_of()` 逐字同一套规则算锚点。

    让作者**抄**而不是**猜**：标点被删掉后"两侧各一个空格"会产出**双减号**
    （`## 9. 流式响应：chunked / SSE` → `9-流式响应chunked--sse`），
    猜是猜不中的。
    """
    path = os.path.join(root, rel.replace("/", os.sep))
    if not os.path.exists(path):
        print("找不到 %s" % path)
        return 2
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8", errors="replace")
    if text.startswith("﻿"):
        print("[!!] 这篇带 BOM：`#` 前面的 BOM 会让 H1 认不出来，锚点链路会断")
    heading = re.compile(r"^#{1,6}\s+(.*?)\s*$")
    for ln in text.splitlines():
        m = heading.match(ln)
        if not m:
            continue
        a = m.group(1).strip().lower()
        a = re.sub(r"[^\w \-]", "", a, flags=re.UNICODE)
        print("  #%s" % a.replace(" ", "-"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
