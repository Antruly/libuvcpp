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
  6. **没有 `.ext:行号` 简写**：`.cpp:123` 这种省掉文件名的写法**直接判红**。
     它是这个门禁自己的一处盲区：`CITE_RE` 要求首字符是字母数字，`BARE_RE` 的
     前视断言又挡住紧跟在词字符后面的 `:`，**两条都匹配不上** —— 于是这种引用
     从来没有被校验过一次，锁文件里也没有它们的条目。而它恰恰是最危险的一种：
     `.cpp` 到底指哪个文件要靠上下文猜。实测就猜错过 —— 文档里"压缩变体缓存
     三道限（`.cpp:830-840`）"那一句，同段落最近的一条完整引用是
     `uvcpp_http_compress.cpp`（全文 284 行），而三道限其实在
     `uvcpp_http_server.cpp:831-841`。约定只有一种形状（`CONTRIBUTING.md`
     「Line references in documentation」：「Fully qualified, always.」），所以
     这里不按上下文猜归属，直接判红。

## `--update` 先找"内容整体挪走了"，而不是原地重哈希

旧形状是"按引用的位置重算一遍哈希"。**上方插了几行**时这就错了，而且是静默的：

    src/web/uvcpp_http_parser.cpp:415-463  36c72595b65f436f     <- 上方插 5 行之前

插 5 行之后，旧形状写出来的还是 `415-463`，只是哈希换成了现在装在 `415-463` 里的
那份内容的 —— 于是这条**本来正确**的引用变成"指向前 5 行"的错引用，而从此全绿。
实测这一次是被外部贡献者复核时抓到的，不是门禁抓到的。

现在的形状：旧哈希在新位置上对不上时，先在 ±`SHIFT_SCAN` 行、**行数不变**的区间里
找内容与旧哈希逐字节相同的那一段。找得到就跟着挪、**哈希保持不变**，并且在报告里
指名道姓地说"文档里那条引用的行号也要一起改" —— 锁里的行号挪了而文档里的没挪，
下一次判据 3 就会报"锁文件里没有这一条"，那不是门禁抽风，是它在提醒这件事。

找不到才当成内容真的改了（`[刷]`），一样逐条印出来。附近有不止一段内容相同时报
`[歧]`、不自动挪；平移的目标键上已经记着别的内容时报 `[撞]`、不覆盖。

## 引用的写法（本批规范化后只有一种形状）

    src/<module>/<file>:<line>
    src/<module>/<file>:<start>-<end>

解析器对历史上的走样写法仍然容忍（先按原样、再补 `src/`、再按 basename 在 `src/`
下**唯一**匹配），容忍是为了让门禁在旧文档上是"红"而不是"扫不出来"。
同名文件出现在两个模块下时报红 —— 猜一个比报错更危险。

**裸 `:NNN`**（`web/uvcpp_ws_connection.h:23` 之后再写 `:31`）按**同一段落**里
最近一条可解析的引用归属。段落里没有文件名可归属时**不算引用**、直接忽略 ——
否则 `localhost:8080` 会被读成一条引用，而假红会训练人忽略门禁。

**`.ext:行号` 简写不在容忍之列**：它不按上下文猜归属，而是**判据 6 直接判红**。
裸 `:NNN` 至少还带着"同一段落"这个可预期的归属范围，`.cpp` 连这个都没有 ——
它要读者自己回想这一段在讲哪个文件，而猜正是它出错的方式。`<file>.h:88` 那种
占位符（`CONTRIBUTING.md` 讲引用写法时用的）带 `>`，被同一个前视断言放过。

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
    python tests/tools/check_doc_lines.py --update    # 复核后刷新锁文件，并打印平移/重刷报告
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

# `.ext:NNN` 简写（`.cpp:123`、`.h:12-34`）—— 省了文件名，只留扩展名。
#
# 这个形状**下面两条正则都匹配不上**：`CITE_RE` 要求文件名首字符是 `[A-Za-z0-9_]`
# （`.` 不是），`BARE_RE` 的 `(?<![\w./:-])` 又挡住紧跟在词字符后面的 `:`（`.cpp`
# 的 `p` 就是词字符）。所以它既没被解析过、也没进过锁文件 —— 判据 6 补上这一格。
#
# 前视断言里的 `>` 是给占位符留的：`CONTRIBUTING.md` 讲引用写法时写
# `<file>.h:88`，那是在说"这个形状"，不是指着某个真文件（与 `CITE_RE` 用 `<`
# 放过 `<file>.h:88` 是同一条约定）。`>` 不会出现在真引用的左边。
SHORTHAND_RE = re.compile(
    r"(?<![\w./:-<>])\.(%s):(\d+)(?:-(\d+))?" % "|".join(SRC_EXTS))

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
    """按文档顺序取出全部引用，裸 `:NNN` 就地归属。

    另返回 `.ext:行号` 简写（判据 6 用）—— 它们**不是引用**：解析不了、也没有
    归属，所以进不了 `cites`，但那正是要判红的东西，得单独收一份。
    """
    index = load_src_index(root)
    cites = []
    external = []
    shorthands = []
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

            for m in SHORTHAND_RE.finditer(ln):
                shorthands.append("%s 第 %d 行 `%s`"
                                  % (doc, i + 1, m.group(0)))

    return cites, unattributed, unreadable, external, shorthands


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
    return hash_lines(lines, start, end)


def hash_lines(lines, start, end):
    """算行表里某个区间的锁哈希（`content_hash` 与平移扫描共用同一套）。"""
    body = "\n".join(TRAIL_WS_RE.sub("", x) for x in lines[start - 1:end])
    return hashlib.sha1(body.encode("utf-8")).hexdigest()[:16]


# `--update` 找"这条引用的内容整体挪走了几行"时的扫描半径。
# 上方插几行注释是最常见的形状（实测是一次 5 行的插入），400 行足够宽，
# 又让一次扫描是 O(400) 次区间哈希而不是 O(文件行数)。
SHIFT_SCAN = 400


def find_shift_candidates(root, path, start, end, want, scan=SHIFT_SCAN):
    """旧哈希在新位置上对不上时，附近有没有一段内容和它**逐字节相同**。

    只在 ±`scan` 行、**行数不变**的区间里找，返回全部候选（不是第一个）——
    候选多于一个意味着"内容变了"和"内容挪了"分不开，调用方必须拒绝自动跟着挪。
    """
    full = os.path.join(root, path)
    try:
        with open(full, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except (IOError, OSError, UnicodeDecodeError):
        return []
    if lines:
        # 与 `content_hash` 同一套 BOM 处理：不一致的话这里算出来的哈希
        # 永远对不上锁里的那个，平移检测会静默失效（退化成原地重哈希）。
        lines[0] = lines[0].lstrip("﻿")
    n = len(lines)
    span = end - start
    hits = []
    for k in range(1, scan + 1):
        for ns in (start + k, start - k):
            ne = ns + span
            if ns < 1 or ne > n:
                continue
            if hash_lines(lines, ns, ne) == want:
                hits.append((ns, ne))
    return hits


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


def write_lock(root, cites, old_lock=None):
    """按目标区间去重后写出。返回 `(条目数, 报告)`。

    旧形状是"按现在的位置重算一遍哈希"，那在**上方插了几行**时是错的：原地重哈希
    会把一条本来正确的引用盖成"指向别处"的错引用，而且从此全绿。所以现在对不上时
    先找平移（见 `find_shift_candidates`），找得到就**跟着挪、哈希保持不变**。

    报告里四样都是**要人看见**的 —— `--update` 的产出只有被人读过才有意义：

      * `moved`：内容逐字节没变，只是整体挪了。锁跟着挪了，**而文档里那条引用
        自己写的行号没跟着挪** ⇒ 下一次门禁会报"锁文件里没有这一条"。那不是
        门禁抽风，是它在说"文档里那条 `文件:行号` 也要一起改"。
      * `restamped`：同一位置的内容真的改了，哈希重刷，人工核引述还对不对。
      * `ambiguous`：附近有不止一段内容与旧哈希相同，分不清该跟哪一个 ⇒ 不自动
        挪（按"内容变了"处理），交给人定。
      * `conflicts`：平移的目标键上已经有一条**不同**内容的记录 ⇒ 不覆盖。
    """
    by_key = {}
    for c in cites:
        if c.path:
            by_key.setdefault(c.key(), []).append(c)

    seen = {}
    moved, restamped, ambiguous, conflicts = [], [], [], []

    for key in sorted(by_key):
        cs = by_key[key]
        path, start, end = cs[0].path, cs[0].start, cs[0].end
        got = content_hash(root, path, start, end)
        want = (old_lock or {}).get(key)

        if want is None or got == want:
            seen[key] = got
            continue

        cands = find_shift_candidates(root, path, start, end, want)
        if len(cands) > 1:
            ambiguous.append((key, cands, cs))
            seen[key] = got
            continue
        if len(cands) == 1:
            ns, ne = cands[0]
            new_key = "%s:%d-%d" % (path, ns, ne)
            if new_key in seen and seen[new_key] != want:
                conflicts.append((key, new_key, seen[new_key], want, cs))
                seen[key] = got
                continue
            seen[new_key] = want
            moved.append((key, new_key, ns - start, cs))
            continue
        restamped.append((key, want, got, cs))
        seen[key] = got

    p = lock_path(root)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 行号引用锁文件 —— 由 tests/tools/check_doc_lines.py --update 生成。\n")
        f.write("# 不要手改：这个文件的作用是让「源码被引用处改了字」必须被人重新确认一次。\n")
        f.write("# 格式：<文件>:<起>-<止> <sha1_16(区间内容, 逐行去尾空白)>\n")
        for k in sorted(seen):
            f.write("%s %s\n" % (k, seen[k]))
    os.replace(tmp, p)
    return len(seen), {"moved": moved, "restamped": restamped,
                       "ambiguous": ambiguous, "conflicts": conflicts}


def print_update_report(rep):
    """把 `--update` 干了什么逐条印出来。**不判红**：刷新锁文件本身没出错，
    出错的是"刷新完之后没人知道文档里还有行号要改"这件事。"""
    moved, restamped = rep["moved"], rep["restamped"]
    ambiguous, conflicts = rep["ambiguous"], rep["conflicts"]
    print("  复核报告：平移 %d / 重刷 %d / 歧义 %d / 撞键 %d"
          % (len(moved), len(restamped), len(ambiguous), len(conflicts)))

    for old, new, delta, cs in moved:
        print("  [移] %s -> %s（内容逐字节没变，整体挪了 %+d 行）"
              % (old, new, delta))
        for c in cs:
            print("       **%s 第 %d 行写的 `%s` 也要一起改成 %s**"
                  % (c.doc, c.lineno, c.raw, new))

    for key, want, got, cs in restamped:
        print("  [刷] %s 的内容真的改了（锁 %s，实际 %s）" % (key, want, got))
        for c in cs:
            print("       %s 第 %d 行 `%s` —— 人工核一遍引述还对不对"
                  % (c.doc, c.lineno, c.raw))

    for key, cands, cs in ambiguous:
        print("  [歧] %s 附近有 %d 段内容和旧哈希一样（%s），不自动跟着挪"
              % (key, len(cands),
                 ", ".join("%d-%d" % (a, b) for a, b in cands)))
        for c in cs:
            print("       %s 第 %d 行 `%s`" % (c.doc, c.lineno, c.raw))

    for key, new_key, there, want, cs in conflicts:
        print("  [撞] %s 想挪到 %s，但那上面已经记着 %s（这次要写的是 %s），"
              "没覆盖" % (key, new_key, there, want))
        for c in cs:
            print("       %s 第 %d 行 `%s`" % (c.doc, c.lineno, c.raw))


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

    cites, unattributed, unreadable, external, shorthands = \
        collect_cites(root, docs)
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
        for s in shorthands:
            print("    [简写] %s   <-- 判据 6 判红" % s)
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

    # ---- 判据 6：没有 `.ext:行号` 简写 ----
    #
    # 与上面四条不同，这条判的是**形状**，不是目标：这种写法解析不了、也没有
    # 归属，所以它从来没进过判据 1/2/3 —— 锁文件里根本没有它的条目。理由见
    # 文件头：省掉文件名之后，"这是哪个文件"只能靠读者猜，而实测就猜错过。
    if shorthands:
        for s in shorthands[:40]:
            fail("`.ext:行号` 简写：%s —— 补全成 `src/<module>/<file>:<行号>`"
                 % s)
        if len(shorthands) > 40:
            fail("…另有 %d 处同类" % (len(shorthands) - 40))
    else:
        ok("判据 6：%d 个 md 里没有 `.ext:行号` 简写" % len(docs))

    print("\n==== 汇总 ====")
    if args.update:
        n, rep = write_lock(root, good, lock)
        print("锁文件已刷新：%s（%d 条目标区间）" % (LOCK_REL, n))
        print_update_report(rep)
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
        print("全过（判据 1/2/3/6 判过；判据 4 **没判** —— 一条引述都没有，"
              "不是判绿了）")
    else:
        print("全过（5 条判据）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
