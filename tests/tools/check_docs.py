#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文档门禁：三条判据，防的是"文档与代码各说各话"而不是错别字。

## 判据

  1. **选项表一致**：根 `CMakeLists.txt` 里真正的旋钮（`option()` 与 `UVCPP_*`
     的 `CACHE STRING`），与两版 README 的选项表**双向相等**。少一行是漏文档，
     多一行是文档写了不存在的东西。
  2. **引用可解析**：文档里的相对链接与反引号里的仓库路径必须真的存在。
  3. **无孤儿文档**：`doc/*.md` 每一篇都要被别的文档**在正文里**提到 —— 正文指的
     是代码块之外，因为只出现在结构树里的文件名**点不动**，人到了那个页面也去不了。

## 这份脚本继承的三条约定（来自 `check_doc_versions.py`）

  * 退出码 `0` 全过 / `1` 判据红了 / `3` 前提不满足。**3 不是"红了"，是"没判"**，
    汇总里必须分得开 —— "绿"和"没判"在报告末尾长得一样是最坏的一种假绿。
  * 前置只用标准库 + 仓库内确定性的东西。**绝不调 `git`**：CI 的 mingw64 档在 MSYS2
    的 PATH 上没有 git，曾导致整个脚本一条判据都没跑就退 3。也不调别的外部命令。
  * 扫的范围**逐个印出来**，让边界可见。

## 有意的收窄（都是为了避免假红，假红会训练人忽略这个门禁）

  * **跳过围栏代码块**。C++ 的 lambda `[](auto& x)` 在 markdown 链接语法里长得就像
    一个链接，不跳会把整篇代码示例读成引用。仓库里现在有二十多处这种形状。
  * **只认带已知扩展名、且以 `doc/` `tests/` `src/` `examples/` `cmake/` 开头的
    反引号 token**。不认 `build/` 之类产物路径（那些本来就不该在仓库里），
    也不认裸文件名（`README.md` 满篇都是，指代不明）。
  * **不认 `docs/`（复数）**。它在 `.gitignore` 里、只存在于开发机上，
    引用它的话 clone 下来必然断链 —— 这正是判据 2 要抓的形状之一。
  * 带 `<>` 的 token 是占位符（`src/<module>/`），跳过；带 `*`/`?` 的按 glob 判。

用法：
    python tests/tools/check_docs.py
"""

import argparse
import glob
import os
import re
import sys

failures = []

# 与 check_doc_versions.py 同一个理由：Windows 的 CI runner 默认用 GBK 解 stdout，
# 中文判据会打成乱码 —— 而红的那几行恰好是最需要看清的。
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (ValueError, OSError):
        pass


def fail(what):
    failures.append(what)
    print("  [红] %s" % what)


def ok(what):
    print("  [绿] %s" % what)


# ---------------------------------------------------------------------------
# 判据 1 的解析
# ---------------------------------------------------------------------------

# **必须允许前导空白**：`UVCPP_BUILD_FUNCTIONAL` 是缩进在 `if(UVCPP_BUILD_TESTS)`
# 里面的（CMakeLists.txt:929），写成 `^option\(` 会静默少一个 —— 而少的那一个
# 恰好是"只在某个前提成立时才存在"的那一类，也就是最该被盯住的。
OPTION_RE = re.compile(r"^\s*option\(\s*([A-Za-z_][A-Za-z0-9_]*)", re.M)

# `UVCPP_TRY_WRITE_MIN_BYTES` 跨两行（`set(` 换行后才写值），而 `[^)]*` 是取反字符类、
# 天然跨行，所以不用 DOTALL。**只认 `CACHE STRING`**：`set(UVCPP_LIB_TARGET ... CACHE
# INTERNAL ...)`（CMakeLists.txt:914）同样是 `UVCPP_` 前缀 + CACHE 形状，但它是个内部
# 变量，不是旋钮。
CACHE_STRING_RE = re.compile(
    r"^\s*set\(\s*(UVCPP_[A-Za-z0-9_]*)\s+[^)]*CACHE\s+STRING", re.M)

# README 表里那一行的第一格：`| \`UVCPP_FOO\` | ... |`
ROW_RE = re.compile(r"^\|\s*`([A-Za-z_][A-Za-z0-9_]*)`\s*\|")

HEADING_EN = re.compile(r"^#{2,4}\s*CMake Options\s*$")
HEADING_ZH = re.compile(r"^#{2,4}\s*CMake 选项\s*$")

# CMake 自带的变量，出现在这张表里是合理的 —— 反向判据对它们放行，但**只在列出来的
# 这几个里面**放行。放行集故意写得很小：它每多一个，反向判据就少咬一口。
CMAKE_BUILTINS = {
    "CMAKE_BUILD_TYPE",
    "CMAKE_CXX_STANDARD",
    "CMAKE_INSTALL_PREFIX",
    "CMAKE_POSITION_INDEPENDENT_CODE",
}


def table_rows_after_heading(text, heading_re):
    """取标题**紧跟着的**那张表的数据行；取不到返回 `None`。"""
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        if not heading_re.match(ln):
            continue
        rows = []
        started = False
        for ln2 in lines[i + 1:]:
            s = ln2.strip()
            if s.startswith("|"):
                rows.append(s)
                started = True
                continue
            if not s and not started:
                continue          # 标题与表之间的空行
            break
        return rows, i + 1
    return None, None


def read(root, rel):
    p = os.path.join(root, rel)
    if not os.path.exists(p):
        return None, "找不到 %s" % rel
    try:
        with open(p, encoding="utf-8") as f:
            return f.read(), None
    except (IOError, OSError) as e:
        return None, "读不了 %s：%s" % (rel, e)
    except UnicodeDecodeError as e:
        return None, "%s 不是 UTF-8：%s" % (rel, e)


# ---------------------------------------------------------------------------
# 判据 2 的解析
# ---------------------------------------------------------------------------

FENCE_RE = re.compile(r"^\s*(?:```|~~~)")

# 只认 `](…)` 里的目标，且**不带 `://`**（外链不归本门禁管，也不该管）。
LINK_RE = re.compile(r"\]\(\s*<?([^)\s>]+)>?\s*\)")

TICK_RE = re.compile(r"`([^`\n]+)`")

PATH_PREFIXES = ("doc/", "tests/", "src/", "examples/", "cmake/")
PATH_EXTS = (".md", ".py", ".h", ".hpp", ".cpp", ".cc", ".json", ".yml", ".yaml",
             ".cmake", ".txt", ".sh", ".svg", ".pc")

# `uvcpp_h2_common.h:88` 是本仓的行号写法，`#anchor` 是章节锚点 —— 两样都不是路径的
# 一部分，先剥掉再判存在。
LINE_SUFFIX_RE = re.compile(r":\d+(-\d+)?$")


def strip_fences(text):
    """把围栏代码块（含标记行本身）整段去掉，返回 (剩余行, 原始行号) 的序列。

    行号要保留：报红时能直接指到行，才有人愿意去改。
    """
    out = []
    inside = False
    for lineno, ln in enumerate(text.splitlines(), 1):
        if FENCE_RE.match(ln):
            inside = not inside
            continue
        if not inside:
            out.append((lineno, ln))
    return out


HEADING_RE = re.compile(r"^#{1,6}\s+(.*?)\s*$")


def anchors_of(text):
    """一篇 markdown 里所有可被 `#锚点` 命中的标题 id。

    这是 GitHub 那套规则的转写：**去掉**标题里的标点、空格换成连字符、全部小写，
    中文原样保留。转写而非调用工具，理由与 `check_doc_versions.py` 转写版本头规则
    一样 —— 前置不许依赖外部命令。

    已知的不覆盖：同名标题 GitHub 会加 `-1`/`-2` 后缀，本仓没有这种情况。
    """
    found = set()
    for ln in text.splitlines():
        m = HEADING_RE.match(ln)
        if not m:
            continue
        a = m.group(1).strip().lower()
        # 只留字母数字、连字符、下划线、空格与 CJK —— 其余（`**`、`.`、`(`…）去掉。
        a = re.sub(r"[^\w \-]", "", a, flags=re.UNICODE)
        found.add(a.replace(" ", "-"))
    return found


def exists_exact(root, rel):
    """按**逐段精确名**判存在，不用 `os.path.exists`。

    Windows 与 macOS 的文件系统大小写不敏感：`doc/Doc-Guide.md` 与
    `doc/doc-guide.md` 在 `os.path.exists` 眼里是同一个东西，于是拼错大小写的链接
    在开发机上绿、在 Linux 上（CI 的 ubuntu 档、以及所有 GitHub 页面）断。
    逐级 `os.listdir` 比精确名，两个方向都对得上。
    """
    cur = root
    parts = rel.replace("\\", "/").split("/")
    for part in parts:
        if part in ("", "."):
            continue
        if part == "..":
            cur = os.path.dirname(cur)
            continue
        try:
            names = os.listdir(cur)
        except (IOError, OSError):
            return False
        if part not in names:
            return False
        cur = os.path.join(cur, part)
    return True


def collect_docs(root):
    """要扫的文档：仓库根这一层的 `*.md` + `doc/` 下的 `*.md`。

    与 `check_doc_versions.py` 一样**不查 git、不递归**。两个理由都在那边写过，
    这里只补一条：`doc/` 是本仓唯一放跟踪文档的目录（`docs/` 在 `.gitignore` 里），
    所以扫这两处就是全部；往别处放文档的话这个门禁看不见，判据 3 也抓不到孤儿。
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


def check_criterion1(root):
    print("\n---- 判据 1：CMake 旋钮与两版 README 的选项表必须双向相等 ----")

    text, err = read(root, "CMakeLists.txt")
    if text is None:
        print("  [停] %s" % err)
        return err
    knobs = set(OPTION_RE.findall(text))
    cached = set(CACHE_STRING_RE.findall(text))
    all_knobs = knobs | cached
    print("  CMakeLists.txt：option() %d 个，UVCPP_* 的 CACHE STRING %d 个 "
          "（%s）" % (len(knobs), len(cached), " ".join(sorted(cached)) or "无"))
    if not all_knobs:
        print("  [停] 一个旋钮都没解析出来 —— CMakeLists.txt 的形状变了，先修解析")
        return "解析不出旋钮"

    for rel, heading, label in (("README.md", HEADING_EN, "英文"),
                                ("README.zh.md", HEADING_ZH, "中文")):
        text, err = read(root, rel)
        if text is None:
            print("  [停] %s" % err)
            return err
        rows, lineno = table_rows_after_heading(text, heading)
        if rows is None:
            print("  [停] %s 里找不到那个选项表标题 —— 表被挪走或改了标题"
                  % rel)
            return "%s 找不到选项表" % rel

        listed = []
        for row in rows:
            m = ROW_RE.match(row)
            if m:
                listed.append(m.group(1))
        if not listed:
            print("  [停] %s 的选项表里一行都没解析出来" % rel)
            return "%s 选项表解析为空" % rel
        print("  %s（%s）选项表 %d 行，起于第 %d 行" % (rel, label, len(listed),
                                                       lineno))

        listed_set = set(listed)
        missing = sorted(all_knobs - listed_set)
        if missing:
            fail("%s 的选项表少了 %d 个旋钮：%s" % (rel, len(missing),
                                                    " ".join(missing)))
        else:
            ok("%s 收录了全部 %d 个旋钮" % (rel, len(all_knobs)))

        extra = []
        for name in sorted(listed_set - all_knobs):
            if name in CMAKE_BUILTINS:
                continue
            extra.append(name)
        if extra:
            fail("%s 的选项表里有 %d 个不是本工程的旋钮：%s"
                 % (rel, len(extra), " ".join(extra)))
        else:
            ok("%s 没有多出 CMake 里不存在的旋钮" % rel)

    return None


def check_criterion2(root, docs):
    print("\n---- 判据 2：文档里的相对链接与仓库路径必须存在 ----")

    # token 池：先收 <路径, 出处>，再判。同一个路径被几十处引用时只报一次，
    # 否则一条断链会把报告刷满，真正的红反而看不见。
    seen = {}
    checked = 0
    for rel in docs:
        text, err = read(root, rel)
        if text is None:
            fail(err)
            continue
        base = os.path.dirname(os.path.join(root, rel))

        for lineno, ln in strip_fences(text):
            # (a) markdown 相对链接
            for m in LINK_RE.finditer(ln):
                target = m.group(1)
                if "://" in target or target.startswith("mailto:"):
                    continue
                # `#锚点` 单独出现时指的是本文件自己 —— 但要按**相对本文件所在目录**
                # 的形状给，不能直接把 `rel`（`doc/x.md`）当路径用：那会去解
                # `doc/doc/x.md`。写这一版时正是这么错的，把 webapp-guide 里 20 多条
                # 自链接全判成了"文件不存在"。
                path, _, frag = target.partition("#")
                sub = path or os.path.basename(rel)
                checked += 1
                if not exists_exact(base, sub):
                    seen.setdefault(("%s:%d" % (rel, lineno), target),
                                    "链接 %s 指向不存在的 %s" % (rel, target))
                    continue
                # 锚点也要判。**光判文件是不够的**：文件在、章节名改了，链接照样
                # 点不动，而且点下去只是停在页首 —— 一个静默的坏引用。
                # （写这轮文档时我自己就写出两条：`#switches` 与 `#build-trees`，
                #   都是照猜的标题写的。）
                if not frag:
                    continue
                tpath = os.path.join(base, sub.replace("/", os.sep))
                try:
                    with open(tpath, encoding="utf-8") as fh:
                        text = fh.read()
                except (IOError, OSError, UnicodeDecodeError):
                    continue          # 读不动就不判锚点，上面已经判过存在
                if frag not in anchors_of(text):
                    seen.setdefault(("%s:%d" % (rel, lineno), target + "@"),
                                    "%s 第 %d 行的锚点 `#%s` 在 %s 里没有对应标题"
                                    % (rel, lineno, frag, sub))

            # (b) 反引号里的仓库路径
            for m in TICK_RE.finditer(ln):
                tok = m.group(1).strip()
                if LINE_SUFFIX_RE.search(tok):
                    tok = LINE_SUFFIX_RE.sub("", tok)
                if any(c in tok for c in "<>"):
                    continue                      # `src/<module>/` 是占位符
                if not tok.startswith(PATH_PREFIXES):
                    continue
                if not tok.endswith(PATH_EXTS):
                    continue
                checked += 1
                hits = glob.glob(os.path.join(root, tok))
                if not hits:
                    seen.setdefault(("%s:%d" % (rel, lineno), tok),
                                    "%s 第 %d 行引用的 `%s` 不存在"
                                    % (rel, lineno, tok))

    print("  判了 %d 处引用（%d 个文档）" % (checked, len(docs)))
    if checked == 0:
        fail("一处引用都没判到 —— 判据 2 在空转，扫描规则八成写死了")
    for _, msg in sorted(seen.items()):
        fail(msg)
    if not seen and checked:
        ok("全部可解析")
    return None


def check_criterion3(root, docs):
    print("\n---- 判据 3：doc/ 下每一篇都要被别的文档在正文里提到 ----")

    docdir = os.path.join(root, "doc")
    if not os.path.isdir(docdir):
        print("  [停] 没有 doc/ 目录")
        return "没有 doc/ 目录"

    # 先把每份文档"正文"（去掉代码块）拼起来。结构树里的文件名点不动，
    # 所以**不算正文** —— 这正是本判据与"在仓库里 grep 一下文件名"的区别。
    bodies = {}
    for rel in docs:
        text, err = read(root, rel)
        if text is None:
            fail(err)
            continue
        bodies[rel] = "\n".join(ln for _, ln in strip_fences(text))

    ours = [d for d in docs if d.startswith("doc/")]
    print("  doc/ 下 %d 篇：%s" % (len(ours), " ".join(sorted(ours))))
    if not ours:
        fail("doc/ 下一篇 md 都没有 —— 扫描范围变了，判据 3 在空转")
        return None

    for rel in ours:
        base = os.path.basename(rel)
        refs = [other for other, body in bodies.items()
                if other != rel and base in body]
        if refs:
            ok("%s ← %s" % (base, ", ".join(sorted(refs)[:3])
                            + ("…" if len(refs) > 3 else "")))
        else:
            fail("%s 是孤儿：没有任何文档在正文里提到它（只在结构树里列了名字"
                 "不算 —— 那样点不动）" % rel)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None,
                    help="仓库根（默认取本脚本的上两级目录）。"
                         "给对照组用：换一个根就能把「前提不满足」那条路走出来 —— "
                         "rc=3 那条分支平时在真实仓库上永远走不到，"
                         "而一条没人走过分支就是一条没验过的分支。")
    args = ap.parse_args()
    root = args.root or os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))
    root = os.path.abspath(root)
    print("仓库根: %s" % root)

    try:
        docs = collect_docs(root)
    except (IOError, OSError) as e:
        print("  [停] 列不出文档：%s" % e)
        print("\n==== 汇总 ====")
        print("前提不满足，退出 3（**一条判据都没判**）")
        return 3
    if not docs:
        print("  [停] 一个 md 都没扫到 —— 仓库根传错了？")
        print("\n==== 汇总 ====")
        print("前提不满足，退出 3（**一条判据都没判**）")
        return 3
    print("  扫了 %d 个 md：%s" % (len(docs), " ".join(docs)))

    # 三条判据的前提互不相同：判据 1 要 CMakeLists + 两版 README 的选项表，判据 2/3
    # 只要文档本身。所以**能判的先判**，前提缺的那条记下来、最后退 3 —— 而不是
    # 把它下面的判据一起吞掉。
    premise = check_criterion1(root)
    check_criterion2(root, docs)
    check_criterion3(root, docs)

    print("\n==== 汇总 ====")
    if premise:
        print("（判据 1 **没判**：%s）" % premise)
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures:
            print("  - %s" % f)
        # 有红就报红：判据 1 没跑成不该把已经判出来的红盖掉。
        return 1
    if premise:
        print("前提不满足，退出 3（判据 2/3 判过了，判据 1 **没判**）")
        return 3
    print("全过（3 条判据）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
