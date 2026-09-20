#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""行号引用门禁：文档写下的 `文件:行号` 必须还指着它当初指的东西。

## 为什么需要它

写使用者向文档时实测：`doc/` 下有 700 多处 `文件:行号` 引用，其中一百多处整条落在
注释行上。而**今天没有任何工具校验行号那半** —— `check_docs.py` 判据 2 的
`LINE_SUFFIX_RE` 是**先把 `:NNN` 剥掉再判文件存在**，行号被刻意丢弃。于是

    改一行注释 -> 引用撞飞 -> 六道门禁全绿

是一条真实的失效路径。这个脚本堵的就是它。

## 判据

  1. **可解析**：每条引用都要落到 `src/` 下一个真实文件上。解析不了是**红**，
     不是跳过 —— "指着一个不存在的文件"和"指着一个不存在的行"是同一种腐烂，
     而前者以前连"文件存在"都没人判（`h2_session.h`、`client.h`、`h` 这类
     截断名在本仓历史文档里真实存在过 34 处）。
  2. **在范围内且非空行**：行号（含 `a-b` 区间）要落在文件里，且区间里至少有一行
     不是空白。
  3. **内容没变**：`doc_line_refs.lock` 锁文件记着每条引用**目标区间**的内容哈希。
     对不上就是红。这一条才是"改注释把引用撞飞"的真正防线 —— 作者没在被引处
     写任何引述时它照样管用。
  4. **引述吻合**：文档在引用旁边用 `「…」` 引述了被引内容时，引述必须出现在被引
     区间里（归一空白与标点后比对）。这是作者自己声明的期望，比对不上比哈希失配
     更说明问题。带省略号（`…` / `...`）的引述是删节，不判。
  5. **反空转**：解析成功的引用数 == 0 就是红。一个"一条都没查"的门禁是最坏的一种
     绿 —— 它长得和"全过"一模一样。

## 引用的写法（本批规范化后只有一种形状）

    src/<module>/<file>:<line>
    src/<module>/<file>:<start>-<end>

解析器对历史上的走样写法仍然容忍（先按原样、再补 `src/`、再按 basename 在 `src/`
下**唯一**匹配），容忍是为了让门禁在旧文档上是"红"而不是"扫不出来"。
同名文件出现在两个模块下时报红 —— 猜一个比报错更危险。

**裸 `:NNN`**（`web/uvcpp_ws_connection.h:23` 之后再写 `:31`）按**同一段落**里
最近一条可解析的引用归属。段落里没有文件名可归属时**不算引用**、直接忽略 ——
否则 `localhost:8080` 会被读成一条引用，而假红会训练人忽略门禁。

## 约定

  * 退出码 `0` 全过 / `1` 判红 / `3` 前提不满足（**没判**）。`1` 优先于 `3`。
  * 只用标准库，**绝不调 `git` 或别的外部命令**：CI 的 mingw64 档在 MSYS2 的 PATH
    上没有 git，曾导致整个脚本一条判据都没跑就退 3。
  * 扫的范围（根 `*.md` + `doc/*.md`）逐个印出来，让边界可见。
  * **扫围栏代码块**，与 `check_docs.py` 相反：那边跳过围栏是因为 C++ 的
    `[](auto& x)` 看起来像 markdown 链接；这边引用恰恰大量写在示例的
    `// src/web/x.h:12` 注释里，跳了就漏掉一半。

用法：
    python tests/tools/check_doc_lines.py
    python tests/tools/check_doc_lines.py --list      # 列出全部引用（规范化时用）
    python tests/tools/check_doc_lines.py --update    # 复核后刷新锁文件
"""

import argparse
import glob
import hashlib
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
# 引用的识别
# ---------------------------------------------------------------------------

SRC_EXTS = ("h", "hpp", "hh", "cpp", "cc", "cxx", "c", "inl")

# 文件名部分不允许以 `.` 或 `/` 开头，且前面不能紧贴着别的标识符字符或 `:` ——
# 前者让 `foo.cpp:12` 里的 `oo.cpp:12` 不被另算一条，后者是**外部引用**的写法：
#
#     nghttp2:nghttp2_session.c:2315      <- nghttp2 自己的源码，不在本仓库里
#
# 加个命名空间前缀是有意的：本仓文档确实要引第三方源码（HTTP/2 那几篇要对着
# nghttp2 的实现说话），但**默认必须红**，作者得显式声明"这条不在本仓"。
# 全都不带前缀就少判一条判据，这正是这个门禁要防的形状。
#
# 文件名前面是 `<` 的**不算引用**：`<file>.h:88` 是占位符，不是指着某个真文件。
# 这条与 `check_docs.py` 判据 2 同一套约定（那边也是遇到 `<>` 就当占位符跳过）。
# 起因是实测：写给读者看的**反例**（"别写成裸 basename，比如 `uvcpp_web_util.h:88`"）
# 会被读成一条真引用 —— 一份**讲引用怎么写**的文档，自己把门禁判红了。
# 假红会训练人忽略门禁，所以占位符这条约定必须有。
CITE_RE = re.compile(
    r"(?<![\w./:-<])([A-Za-z0-9_][A-Za-z0-9_./-]*\.(?:%s)):(\d+)(?:-(\d+))?"
    % "|".join(SRC_EXTS))

# 带命名空间前缀的引用：记下来、印出来，但不判存在性。
EXT_RE = re.compile(
    r"(?<![\w./-])([A-Za-z0-9_]+):([A-Za-z0-9_][A-Za-z0-9_./-]*\.(?:%s)):(\d+)(?:-(\d+))?"
    % "|".join(SRC_EXTS))

# 裸 `:NNN`：前面不能是标识符字符、`.`、`/`、`-`、`:`，后面不能是数字。
# `localhost:8080` 里 `:` 前是字母 `t`，天然不匹配；但 `127.0.0.1:8080` 里前一个
# 字符是 `1` —— **是**标识符字符，也不匹配。真正会漏进来的是
# `见 :31` 这种带空格的写法，那正是我们要的。
BARE_RE = re.compile(r"(?<![\w./:-])(?<!:\d):(\d+)(?:-(\d+))?")

TRAIL_WS_RE = re.compile(r"[ \t]+$")


class Cite(object):
    """一条引用。`start`/`end` 是 1 起的闭区间。"""

    def __init__(self, doc, lineno, raw, start, end, attributed):
        self.doc = doc
        self.lineno = lineno        # 引用写在文档的第几行
        self.raw = raw              # 原样文本，报错时回显
        self.start = start
        self.end = end
        self.attributed = attributed  # 裸 :NNN 靠上下文归属时为 True
        self.path = None            # 解析出来的仓库相对路径

    def key(self):
        return "%s:%d-%d" % (self.path, self.start, self.end)


def load_src_index(root):
    """`src/` 下 basename -> [相对路径...]。解析走样写法时用。"""
    index = {}
    src = os.path.join(root, "src")
    for dirpath, _dirnames, filenames in os.walk(src):
        for fn in filenames:
            if not fn.endswith(SRC_EXTS):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            index.setdefault(fn, []).append(rel)
    return index


def resolve(root, index, tok):
    """把一个文件名 token 解析成仓库相对路径。

    返回 `(rel, None)` 或 `(None, 理由)`。三种写法都容忍，但**同名歧义不猜**。
    """
    # 历史写法里 token 可能带 markdown 的 `./` 前缀或尾随的标点。
    tok = tok.strip().lstrip("./")
    tok = tok.rstrip(".,;:)]}）》、，。；：")

    # 1) 原样（`src/web/x.h` 这种规范形状走这条）
    if os.path.isfile(os.path.join(root, tok)):
        return tok.replace(os.sep, "/"), None
    # 2) 补 `src/` 前缀（历史写法 `web/x.h:12`）
    cand = "src/" + tok
    if os.path.isfile(os.path.join(root, cand)):
        return cand, None
    # 3) 按 basename 在 `src/` 下唯一匹配（历史写法 `uvcpp_memory_pool.h:88`）
    hits = index.get(os.path.basename(tok), [])
    if len(hits) == 1:
        return hits[0], None
    if len(hits) > 1:
        return None, ("%s 在 src/ 下有 %d 个同名文件（%s），光看文件名定不下是哪个"
                      % (os.path.basename(tok), len(hits),
                         ", ".join(sorted(hits))))
    return None, "src/ 下没有叫 %s 的文件" % tok


def collect_cites(root, docs):
    """按文档顺序取出全部引用，裸 `:NNN` 就地归属。"""
    index = load_src_index(root)
    cites = []
    external = []
    unattributed = 0
    unreadable = []

    for doc in docs:
        try:
            with open(os.path.join(root, doc), encoding="utf-8") as f:
                text = f.read()
        except (IOError, OSError, UnicodeDecodeError) as e:
            unreadable.append("%s（%s）" % (doc, e))
            continue
        lines = text.splitlines()
        if lines:
            lines[0] = lines[0].lstrip("﻿")

        # 段落 = 空行分隔的块。裸引用只在段落内继承文件名。
        last = None          # 本段落最近一条**能解析**的引用

        for i, ln in enumerate(lines):
            if not ln.strip():
                last = None
                continue

            for m in EXT_RE.finditer(ln):
                external.append("%s 第 %d 行 `%s`" % (doc, i + 1, m.group(0)))

            for m in CITE_RE.finditer(ln):
                tok, a, b = m.group(1), int(m.group(2)), m.group(3)
                c = Cite(doc, i + 1, m.group(0), a, int(b) if b else a, False)
                rel, why = resolve(root, index, tok)
                if rel is None:
                    c.path = None
                    c.why = why
                else:
                    c.path = rel
                    c.why = None
                    last = c
                cites.append(c)

            for m in BARE_RE.finditer(ln):
                if last is None:
                    unattributed += 1
                    continue
                a, b = int(m.group(1)), m.group(2)
                c = Cite(doc, i + 1, m.group(0), a, int(b) if b else a, True)
                c.path = last.path
                c.why = None
                cites.append(c)

    return cites, unattributed, unreadable, external


# ---------------------------------------------------------------------------
# 目标行的读取与归一
# ---------------------------------------------------------------------------

PUNCT_RE = re.compile(r"[\s`*_\\|]+")
ELLIPSIS_RE = re.compile(r"…|\.\.\.")


def norm(s):
    """归一空白与行内标点，用于引述比对。**不动中日韩文字与 ASCII 字母数字。**"""
    s = PUNCT_RE.sub("", s)
    return s


def read_range(root, path, start, end):
    """返回 `(归一后的区间文本, 原始区间文本, 理由)`。理由非空即读不了。"""
    full = os.path.join(root, path)
    try:
        with open(full, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except (IOError, OSError, UnicodeDecodeError) as e:
        return None, None, "读不了 %s：%s" % (path, e)
    if lines:
        lines[0] = lines[0].lstrip("﻿")
    if start < 1 or end < start:
        return None, None, "行号区间不成立（%d-%d）" % (start, end)
    if end > len(lines):
        return None, None, ("行号 %d 超出 %s 的范围（共 %d 行）"
                            % (end, path, len(lines)))
    chunk = lines[start - 1:end]
    if not any(x.strip() for x in chunk):
        return None, None, "%s:%d-%d 整段都是空行" % (path, start, end)
    raw = "\n".join(chunk)
    return norm(raw), raw, None


def content_hash(root, path, start, end):
    """锁文件用的哈希。逐行去尾空白，避免一次格式化就全网飘红。"""
    full = os.path.join(root, path)
    with open(full, encoding="utf-8") as f:
        lines = f.read().splitlines()
    if lines:
        lines[0] = lines[0].lstrip("﻿")
    body = "\n".join(TRAIL_WS_RE.sub("", x) for x in lines[start - 1:end])
    return hashlib.sha1(body.encode("utf-8")).hexdigest()[:16]


# ---------------------------------------------------------------------------
# 引述
# ---------------------------------------------------------------------------

QUOTE_RE = re.compile(r"「([^」]*)」")


def check_quotes(root, cites):
    """同一条文档行上，引用旁的 `「…」` 要能在被引区间里找到。"""
    by_doc = {}
    for c in cites:
        if c.path:
            by_doc.setdefault(c.doc, []).append(c)

    checked = 0
    skipped = 0
    for doc, cs in sorted(by_doc.items()):
        try:
            with open(os.path.join(root, doc), encoding="utf-8") as f:
                lines = f.read().splitlines()
        except (IOError, OSError, UnicodeDecodeError):
            continue
        for c in cs:
            if c.lineno > len(lines):
                continue
            ln = lines[c.lineno - 1]
            for m in QUOTE_RE.finditer(ln):
                q = m.group(1)
                # 删节号意味着作者明确说了"这里省略了"，比对不了。
                if ELLIPSIS_RE.search(q) or not q.strip():
                    skipped += 1
                    continue
                got, _raw, why = read_range(root, c.path, c.start, c.end)
                if got is None:
                    continue          # 区间本身的问题已经由判据 2 报了
                if norm(q) not in got:
                    fail("%s 第 %d 行引述「%s」在被引的 %s:%d-%d 里找不到"
                         % (doc, c.lineno, q, c.path, c.start, c.end))
                else:
                    checked += 1
    if skipped:
        print("  [记] %d 条引述带删节号，按约定不判" % skipped)
    return checked, skipped


# ---------------------------------------------------------------------------
# 锁文件
# ---------------------------------------------------------------------------

LOCK_REL = "tests/tools/doc_line_refs.lock"


def lock_path(root):
    return os.path.join(root, *LOCK_REL.split("/"))


def read_lock(root):
    p = lock_path(root)
    if not os.path.isfile(p):
        return None
    out = {}
    with open(p, encoding="utf-8") as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            parts = ln.split()
            if len(parts) != 2:
                continue
            out[parts[0]] = parts[1]
    return out


def write_lock(root, cites):
    """按目标区间去重后写出。同一区间被多篇引用时共用一个条目。"""
    seen = {}
    for c in cites:
        if not c.path:
            continue
        seen[c.key()] = content_hash(root, c.path, c.start, c.end)
    p = lock_path(root)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 行号引用锁文件 —— 由 tests/tools/check_doc_lines.py --update 生成。\n")
        f.write("# 不要手改：这个文件的作用是让「源码被引用处改了字」必须被人重新确认一次。\n")
        f.write("# 格式：<文件>:<起>-<止> <sha1_16(区间内容, 逐行去尾空白)>\n")
        for k in sorted(seen):
            f.write("%s %s\n" % (k, seen[k]))
    os.replace(tmp, p)
    return len(seen)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def collect_docs(root):
    docs = sorted(os.path.basename(p) for p in glob.glob(os.path.join(root, "*.md")))
    docs += sorted("doc/" + os.path.basename(p)
                   for p in glob.glob(os.path.join(root, "doc", "*.md")))
    return docs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None,
                    help="仓库根（默认取本脚本的上两级目录）。给对照组用。")
    ap.add_argument("--list", action="store_true",
                    help="只列出扫到的引用，不判（规范化时用）")
    ap.add_argument("--update", action="store_true",
                    help="复核后刷新锁文件")
    args = ap.parse_args()
    root = args.root or os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))
    root = os.path.abspath(root)
    print("仓库根: %s" % root)

    docs = collect_docs(root)
    if not docs:
        print("  [停] 一个 md 都没扫到 —— 仓库根传错了？")
        print("\n==== 汇总 ====")
        print("前提不满足，退出 3（**一条判据都没判**）")
        return 3
    print("  扫了 %d 个 md：%s" % (len(docs), " ".join(docs)))

    cites, unattributed, unreadable, external = collect_cites(root, docs)
    for u in unreadable:
        fail("文档读不了：%s" % u)

    print("  引用 %d 条（另有 %d 处裸 `:行号` 附近没有可归属的文件名，按约定不算引用）"
          % (len(cites), unattributed))
    if external:
        # 印出来是为了让"我声明了这是外部引用"这件事**在 CI 日志里留痕**：
        # 一条都不用判，但也没法偷偷用前缀把本仓的引用洗成不判。
        print("  [记] %d 条带命名空间前缀的引用（声明为外部源码，不判存在性）："
              % len(external))
        for e in external:
            print("        %s" % e)

    if args.list:
        for c in cites:
            print("    %-28s %-6s %s%s" % (c.doc, "第%d行" % c.lineno, c.raw,
                                           "" if c.path else "   <-- 解析不了"))
        return 0

    if not cites:
        print("\n==== 汇总 ====")
        print("红 1 条：")
        print("  - 一条引用都没扫到。**反空转**：没有引用的绿和「全过」长得一样，"
              "要么文档被清空了，要么扫描规则坏了。")
        return 1

    # ---- 判据 1：可解析 ----
    bad = [c for c in cites if not c.path]
    if bad:
        for c in bad[:40]:
            fail("%s 第 %d 行的引用 `%s` 解析不了：%s"
                 % (c.doc, c.lineno, c.raw, c.why))
        if len(bad) > 40:
            fail("…另有 %d 条同类" % (len(bad) - 40))
    else:
        ok("判据 1：%d 条引用全部解析到 src/ 下的真实文件" % len(cites))

    good = [c for c in cites if c.path]

    # ---- 判据 2：在范围内且非空行 ----
    range_bad = 0
    for c in good:
        _n, _r, why = read_range(root, c.path, c.start, c.end)
        if why:
            fail("%s 第 %d 行的引用 `%s`：%s" % (c.doc, c.lineno, c.raw, why))
            range_bad += 1
    if not range_bad:
        ok("判据 2：%d 条引用的行号都落在文件里，且不是空行" % len(good))

    # ---- 判据 3：锁文件 ----
    lock = read_lock(root)
    if lock is None:
        fail("%s 不在 —— 删掉锁文件不能绕过判据 3" % LOCK_REL)
    else:
        drifted = []
        for c in good:
            want = lock.get(c.key())
            if want is None:
                drifted.append((c, "锁文件里没有这一条"))
                continue
            got = content_hash(root, c.path, c.start, c.end)
            if got != want:
                drifted.append((c, "%s:%d-%d 的内容变了（锁 %s，实际 %s）"
                                % (c.path, c.start, c.end, want, got)))
        for c, why in drifted[:40]:
            fail("%s 第 %d 行的引用 `%s`：%s" % (c.doc, c.lineno, c.raw, why))
        if len(drifted) > 40:
            fail("…另有 %d 条同类" % (len(drifted) - 40))
        if not drifted:
            ok("判据 3：%d 条引用指向的内容与锁文件一致" % len(good))

    # ---- 判据 4：引述 ----
    #
    # 这条是**条件判据**：只在作者写了 `「…」` 时才判。本仓文档今天一处都没写，
    # 所以它**今天一条都没判** —— 那就必须印成 `[记]` 而不是 `[绿]`。
    # 一个"一条都没查"的判据报成绿，和"全过"长得一模一样，这正是本仓
    # 反空转那条规矩要防的形状（`check_docs.py` 那边叫它"没判不是红了"）。
    n_quotes, n_ellipsis = check_quotes(root, good)
    if n_quotes == 0:
        print("  [记] 判据 4 **没判**：%d 条引用旁边都没有 `「…」` 引述"
              "（本仓约定：想让它判就在引述两侧写 `「」`，用 ASCII `\"` 的"
              "多半是作者自己的强调语，判不了）" % len(good))
    else:
        ok("判据 4：%d 条引述在被引区间里找到" % n_quotes)

    print("\n==== 汇总 ====")
    if args.update:
        n = write_lock(root, good)
        print("锁文件已刷新：%s（%d 条目标区间）" % (LOCK_REL, n))
        return 0
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures[:60]:
            print("  - %s" % f)
        if len(failures) > 60:
            print("  …另有 %d 条" % (len(failures) - 60))
        print("\n改了源码里被引的那几行之后，**先人工核一遍引用还对不对**，"
              "再跑 `--update` 刷新锁文件。")
        return 1
    if n_quotes == 0:
        # 汇总行必须自己说清"少判了一条"。退出码只有一个数，报不了这件事，
        # 而"全过"三个字如果盖住了没跑的那条，就是最坏的那种假绿。
        print("全过（判据 1/2/3 判过；判据 4 **没判** —— 一条引述都没有，"
              "不是判绿了）")
    else:
        print("全过（4 条判据）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
