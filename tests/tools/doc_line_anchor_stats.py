#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重测尾部锚点的**覆盖率** —— `check_doc_lines.py` 里那两个百分数是量出来的。

`TAIL_ANCHORS = 2` 是照着这份读数定的（见那个文件头「判『区间被内部插/删了行』」），
而锁条目会随着每次改文档增长 ⇒ 数字会漂。所以给出这条命令，别让它变成一句
"当年测过"：

    python3 tests/tools/doc_line_anchor_stats.py

**判据**（照 `tail_shifts()` 的语义反推，不是另立一套）：某个锚点**能定位**，
等价于"它那一行的内容在 ±`SHIFT_SCAN` 窗口里只有它自己一处"。只有这种行，在内容
被内部插/删挤走之后才会**只留下一个**候选；满文件都有的 `}` 会留下好几个候选 ——
那种情况宁可说"判不了"（`[记]`），也不能猜一处。

于是对每条锁条目取末行（j=0）、倒数第二行（j=1）……逐个判"窗口内唯一"，再数
"前 k 个锚点里**至少一个**唯一"的条目占比。打印绝对条数，方便人把它和门禁输出里
`[记] 判不了 N 条` 的 N 对一遍 —— 两个数应当接近（门禁只在**真的动过**的条目上
才去定位，所以它会略小于这里的 9.5%）。
"""
import argparse
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import check_doc_lines as C  # noqa: E402

MAXK = 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=REPO, help="仓库根（默认从本文件位置推）")
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    os.chdir(root)

    lock, lock_tails = C.read_lock(root)
    if not lock:
        print("读不出锁文件 %s" % os.path.join(root, C.LOCK_REL))
        return 2

    scan = C.SHIFT_SCAN
    lines_cache = {}
    ok = [0] * (MAXK + 1)
    total = 0
    no_file = 0
    short = 0
    for key in lock:
        path, _, rest = key.rpartition(":")
        s, _, e = rest.partition("-")
        start = int(s)
        end = int(e) if e else int(s)
        lines = lines_cache.get(path)
        if lines is None:
            lines = C.read_lines(root, path)
            lines_cache[path] = lines
        if lines is None:
            no_file += 1
            continue
        total += 1
        if any(t is None for t in ((lock_tails or {}).get(key) or [])):
            short += 1
        n = len(lines)
        uniq = []
        for j in range(MAXK):
            pos = end - j
            if pos < start:
                break
            h = C.hash_lines(lines, pos, pos)
            lo, hi = max(1, pos - scan), min(n, pos + scan)
            hits = 0
            for i in range(lo, hi + 1):
                if C.hash_lines(lines, i, i) == h:
                    hits += 1
                    if hits > 1:
                        break
            uniq.append(hits == 1)
        for k in range(1, MAXK + 1):
            if any(uniq[:k]):
                ok[k] += 1

    print("锁条目 %d 条（引用指向的文件读不出来的 %d 条）" % (total, no_file))
    for k in range(1, MAXK + 1):
        print("  前 %d 个锚点：能定位 %d 条（%.1f%%）、一个都定位不了 %d 条（%.1f%%）"
              % (k, ok[k], 100.0 * ok[k] / total,
                 total - ok[k], 100.0 * (total - ok[k]) / total))
    print("  区间太短（没有倒数第二行的）%d 条 —— 少的那个锚点按**不存在**算，"
          "不按「判不了」算。" % short)
    return 0


if __name__ == "__main__":
    sys.exit(main())
