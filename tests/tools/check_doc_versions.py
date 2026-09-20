#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核 README 顶上那几个版本号：**两份 README 必须说同一个版本**，且出包时那个版本
必须是**正在出的那一版**。

## 为什么是这两条

README 的版本号语义是「**最新发布**」，不是「源码树的开发版」——
`README.md:1` 那个徽章就叫 GitHub release。本仓只发过 v1.0.0 与 v1.1.0 两个 tag，
1.1.1 起的每一档都是开发版（`UVCPP_VERSION_IS_RELEASE = 0`），**从未发布**。
所以在 master 上「README 写着 1.1.0 而版本头是 1.1.34」是**对的**，
不是过期 —— 这正是判据 3 只在出包时启用的原因。

真正的病在**出包那一刻**：`package_release.py` 只是 `shutil.copy2` 把 README 拷进
包里，没有任何一步看一眼里面的版本号。于是切 1.1.34 的时候，产出的
`uvcpp-1.1.34-<platform>.zip` 里会装着一份**自称 1.1.0** 的 README —— 这个形状在本仓
已经判过两次刑（`release.yml` 里那条死掉的 `env: UVCPP_VERSION: '1.1.0'`、
`package_release.py` 曾经写死的 "1.1.0"），两次都是"手写在一个没人同步的地方"。

## 判据

  1. **两份 README 必须一致**。任何一处与其他处不同就红。这条**不需要**知道当前
     版本，所以在每次 push 上都能跑 —— 它抓的是"只改了英文那份、忘了中文那份"
     （反过来也一样）。这是唯一一条在 master 上就该绿的。
  2. **命中总数必须恰好 4**。防的是判据 1 空转：只留判据 1 的话，把 README 的版本行
     **删掉**、或者把写法改掉，它一声不响地全绿。一个恒绿的判据比没有判据更坏 ——
     它会训练人忽略它。
  3. **（只在 `--expect` / `--expect-header` 时，且只在真发布时）** 所有命中必须等于
     `src/uvcpp/uvcpp_version.h`（或 `--expect` 给的那个）版本。这是"**发布前**校验"：
     出包脚本在拷 README **之前**调它，不相等就停下来。读不到头文件也停 ——
     一个名字说谎的包比不出包更坏。

     **这条默认只在发布版上真判**：头文件说 `UVCPP_VERSION_IS_RELEASE = 0`（开发版）时
     它标 `[跳过]`，因为**开发版包与 README 本来就不该一致** —— README 跟的是「最新
     发布」，而每次 push 出的都是 1.1.34 这种开发版包。第一版判据没这条区分，于是在
     CI 的 `config-contract` 档上（每次 push 都出开发版包）直接红 —— 判据本身写错了
     前提，不是树错了。要在这个前提下强制判它（自家对照、或从一棵没翻
     `IS_RELEASE` 的树上裁发布），传 `--this-is-a-release`。

扫两种写法：shields 徽章的 `badge/release-<X.Y.Z>`，以及加粗的 Version / 版本 行
（值在反引号里）。两种写法、两个 README 各两处，一共 4 处。

**不扫 `RELEASE.md`**：它是发布说明，"讲的是哪一版"与"现在是哪一版"是两件事，
把它卡进来只会逼着每次升档去改一份历史文档的标题。

用法：
    python tests/tools/check_doc_versions.py                  # push 上跑（判据 1+2）
    python tests/tools/check_doc_versions.py --expect-header  # 出包前跑（发布版上 +判据 3）

退出码 0 全过；1 有判据红了；3 前提不满足（git 列不出文件 / 读不到版本头）。
"""

import argparse
import os
import re
import subprocess
import sys

failures = []

# Windows 的 CI runner 默认用 GBK 解 stdout，中文判据会打成乱码 —— 红的那几行
# 恰好是最需要看清的。只在能改的时候改（3.7+），失败也不影响判据。
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


# 与 `package_release.py:_header_version()` 同一个口径。两处都从**头文件**读是
# 有意的：它是唯一来源，派生出来的东西（CMake 变量、包名、pc 文件）都跟着它走。
VERSION_HEADER = os.path.join("src", "uvcpp", "uvcpp_version.h")

# 徽章那条要连 `badge/release-` 一起匹配 —— 只匹配 `1.1.0` 这种裸串会把 README 里
# 所有提到版本的地方（变更日志、示例输出）一起卷进来，造出假失败。
PATTERNS = [
    ("shields 徽章", re.compile(r"badge/release-(\d+\.\d+\.\d+)")),
    ("英文 Version 行", re.compile(r"\*\*Version\*\*:\s*`(\d+\.\d+\.\d+)`")),
    ("中文 版本 行", re.compile(r"\*\*版本\*\*：\s*`(\d+\.\d+\.\d+)`")),
]

# 两个 README × 两种写法。少一处就说明扫的东西变了，判据 1 可能在空转。
EXPECTED_HITS = 4


def header_version(root):
    p = os.path.join(root, VERSION_HEADER)
    if not os.path.exists(p):
        return None, "找不到版本头 %s" % p
    with open(p, encoding="utf-8") as f:
        s = f.read()

    nums = []
    for macro in ("UVCPP_VERSION_MAJOR", "UVCPP_VERSION_MINOR",
                  "UVCPP_VERSION_PATCH"):
        m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % macro, s, re.M)
        if not m:
            return None, "读不到 %s（%s）—— 版本号来源变了，先修这里" % (macro, p)
        nums.append(m.group(1))
    return ".".join(nums), None


def header_is_release(root):
    """头文件说这是不是发布版（`UVCPP_VERSION_IS_RELEASE`）。

    读不到就返回 `None` —— 调用方当"不是发布版"处理（判据 3 跳过），**不当红**：
    这个宏在 1.1.26 之前不存在，且它是判断据 3 要不要跑的依据，不是判据本身；
    把头文件整个读不到的情况另论（`header_version()` 会以 rc=3 停）。
    """
    p = os.path.join(root, VERSION_HEADER)
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as f:
        s = f.read()
    m = re.search(r"^#define\s+UVCPP_VERSION_IS_RELEASE\s+(\d+)\s*$", s, re.M)
    if not m:
        return None
    return int(m.group(1)) == 1


def tracked_markdown(root):
    """只扫**被 git 跟踪**的 md。

    用 `git ls-files` 而不是遍历目录：出包时 `package_release.py` 会在仓库里落下
    暂存副本（README 也在其中），遍历会把那些副本一起数进来，判据 2 的"恰好 4 处"
    立刻变成假失败。
    """
    try:
        out = subprocess.check_output(["git", "ls-files", "*.md"],
                                      cwd=root, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as e:
        return None, "git ls-files 失败：%s" % e
    files = [l for l in out.decode("utf-8", "replace").splitlines() if l.strip()]
    return files, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None,
                    help="仓库根（默认取本脚本的上两级目录）")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--expect-header", action="store_true",
                   help="额外要求版本串等于 src/uvcpp/uvcpp_version.h 里的版本")
    g.add_argument("--expect", default=None, metavar="X.Y.Z",
                   help="额外要求版本串等于这个版本（出包脚本传它 —— 它才是"
                        "「正在出的那一版」，`--version` 时可以覆盖头文件）")
    ap.add_argument("--this-is-a-release", action="store_true",
                    help="即使版本头说这是开发版，也真判判据 3。给两处用："
                         "① 从一棵忘了翻 IS_RELEASE 的树上裁发布；"
                         "② 判据 3 自己的对照（换一个版本必须红）。")
    args = ap.parse_args()
    want = args.expect

    root = args.root
    if root is None:
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    print("仓库根: %s" % root)

    files, err = tracked_markdown(root)
    if files is None:
        print("  [红] %s" % err)
        print("\n==== 汇总 ====")
        print("前提不满足，退出 3")
        return 3

    hits = []          # [(file, lineno, label, version)]
    for rel in files:
        p = os.path.join(root, rel)
        try:
            with open(p, encoding="utf-8") as f:
                text = f.read()
        except (IOError, OSError) as e:
            fail("读不了 %s：%s" % (rel, e))
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for label, pat in PATTERNS:
                for m in pat.finditer(line):
                    hits.append((rel, lineno, label, m.group(1)))

    # ---- 判据 1 ----
    print("\n---- 判据 1：两份 README 必须说同一个版本 ----")
    if not hits:
        fail("一处版本串都没扫到（下面判据 2 会说明该有几处）")
    else:
        counts = {}
        for h in hits:
            counts[h[3]] = counts.get(h[3], 0) + 1
        if len(counts) == 1:
            ok("4 处写法一致：%s" % hits[0][3])
        else:
            # 只把**少数派**标红、报**多数派**是什么。四处一律标红的话，读的人
            # 不知道到底该改哪一处 —— 一个喊得太响的判据会被训练成背景噪声。
            major = max(counts, key=lambda v: counts[v])
            for rel, lineno, label, v in hits:
                if v != major:
                    fail("%s:%d %s 写的是 %s，其余 %d 处都是 %s"
                         % (rel, lineno, label, v, counts[major], major))

    # ---- 判据 2 ----
    print("\n---- 判据 2：命中总数必须恰好 %d（防判据 1 空转）----" % EXPECTED_HITS)
    if len(hits) == EXPECTED_HITS:
        ok("命中 %d 处" % len(hits))
    else:
        fail("只命中 %d 处（应为 %d）：要么版本行被删/改了写法，要么扫的范围变了"
             "—— 判据 1 可能已经在空转" % (len(hits), EXPECTED_HITS))

    # ---- 判据 3（只在出包时）----
    if args.expect_header and want is None:
        want, err = header_version(root)
        if want is None:
            print("  [红] %s" % err)
            print("\n==== 汇总 ====")
            print("前提不满足，退出 3")
            return 3

    # 判据 3 只在发布版上真判：开发版包与 README 本该不一致（README 跟「最新发布」）。
    # `--this-is-a-release` 兜两种"头文件没翻但确实在出正式版"的情况。
    want_checked = want is not None and (args.this_is_a_release
                                         or header_is_release(root) is True)

    if want is not None and not want_checked:
        print("\n---- 判据 3：版本串必须等于正在出的那一版 ----")
        print("  [跳过] 版本头里 UVCPP_VERSION_IS_RELEASE = 0（开发版）：README 跟的是"
              "「最新发布」，本次要出的 %s 是开发版，两者**本该不一致**。" % want)
        print("         要真判（自家对照 / 裁发布），加 --this-is-a-release。")

    if want_checked:
        print("\n---- 判据 3：版本串必须等于正在出的那一版（%s）----" % want)
        bad = 0
        for rel, lineno, label, v in hits:
            if v != want:
                fail("%s:%d %s 写的是 %s，而要出的是 %s"
                     % (rel, lineno, label, v, want))
                bad += 1
        if bad == 0:
            ok("全部等于头文件版本 %s" % want)
        else:
            print("  → 出包前把 README.md / README.zh.md 的版本串改成 %s" % want)

    print("\n==== 汇总 ====")
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    if want_checked:
        print("全过（发布口径：版本串 == %s）" % want)
    elif want is not None:
        # 「跳过」不能只靠上面那一行 —— 汇总里必须能看出**这条判据没跑**，
        # 否则"绿"和"没判"在报告末尾长得一样。
        print("全过（master 口径：只要求两份 README 一致；判据 3 **已跳过**，"
              "本次没判版本串）")
    else:
        print("全过（master 口径：只要求两份 README 一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
