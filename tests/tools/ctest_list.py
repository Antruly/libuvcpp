#!/usr/bin/env python3
# -*- coding: utf-8 -*-
""""这次要跑哪些 exe" —— 判据取自 ctest 的清单，不是磁盘。

**不要按磁盘上的 `test_*.exe` 枚举。** 删掉一个测试源文件之后，CMake 会把它从
`CTestTestfile.cmake` 里摘掉，但**不会删掉已经生成的那个 exe**。按磁盘枚举的工具
于是会去跑一个没有对应源码、而且往往只有它一个还链着上一版库的二进制：用例数
虚高，跑出来的结果什么都证明不了（真崩了还会被读成"页堆抓到了"/"这个用例有
泄漏"）。`ctest` 自己不会跑它，这些工具也就不该跑。

实测撞到过的形状：加过一个临时用例、复核完把源码删掉并重跑 cmake 之后，
`CTestTestfile.cmake` 里已经没有它了，可那个 exe 还留在 `Release/` 下。

`ctest_tests()` 是清单（权威）；`on_disk_exes()` 只用来把清单外的那些**报出来**，
让分叉变成可见的，而不是静默。
"""

import os
import re


def ctest_tests(tree, config="Release"):
    """ctest 会跑的用例，`[(名字, exe 绝对路径, ctest 用的工作目录)]`。

    工作目录取装 `CTestTestfile.cmake` 的那一层 —— 就是 `add_test` 的默认值
    `CMAKE_CURRENT_BINARY_DIR`（多配置生成器下它是 `Release` 的上一级）。
    """
    want = config.upper()
    out = []
    for base, _dirs, files in os.walk(os.path.join(tree, "tests")):
        if "CTestTestfile.cmake" not in files:
            continue
        cur = None
        with open(os.path.join(base, "CTestTestfile.cmake"),
                  "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                m = re.match(r'^(?:else)?if\(CTEST_CONFIGURATION_TYPE MATCHES '
                             r'"\^\((.*)\)\$"\)', s)
                if m:
                    # `[Rr][Ee][Ll][Ee][Aa][Ss][Ee]` → "RELEASE"：每对括号取首字母
                    cur = "".join(p[0].upper()
                                  for p in re.findall(r"\[(.)(.)\]", m.group(1)))
                    continue
                if cur != want:
                    continue
                a = re.match(r'^add_test\(\[=\[(.*?)\]=\]\s+"(.*?)"\)', s)
                if a:
                    out.append((a.group(1), a.group(2).replace("/", os.sep), base))
    out.sort()
    return out


def on_disk_exes(tree):
    """磁盘上的 `test_*.exe`，`[(名字, 绝对路径)]`。只用来报"清单里没有的"。"""
    out = []
    for base, _dirs, files in os.walk(os.path.join(tree, "tests")):
        for f in files:
            if f.startswith("test_") and f.lower().endswith(".exe"):
                out.append((f[:-4], os.path.join(base, f)))
    out.sort()
    return out


def report_stray(tree, known_paths):
    """把"磁盘上有、清单里没有"的 exe 打出来并返回它们。没有就返回空。"""
    stray = [e for e in on_disk_exes(tree) if e[1] not in known_paths]
    if stray:
        print("**磁盘上有 %d 个 ctest 没登记的 exe**（源码删掉后 exe 会留在原地，"
              "跑它证明不了任何事）—— 本次不跑：" % len(stray))
        for name, _p in stray:
            print("    %s" % name)
    return stray
