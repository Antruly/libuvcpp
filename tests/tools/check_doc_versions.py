#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核 README 顶上那几个版本号：**必须等于源码树自己的版本**（`UVCPP_VERSION_STRING`）。

## 判据的前提

README 顶上那个徽章与 Version 行显示的是**当前源码树的版本** —— 也就是
`src/uvcpp/uvcpp_version.h` 报出来的那个串，开发版带 `-dev`。所以「README 写着什么」
与「头文件写着什么」**任何时刻都必须一致**，这条判据在每次 push 上都能真判。

（2026-09-20 之前不是这样：那时 README 顶上跟的是「**最新发布**」，而 1.1.1 起每一档
都是开发版、从未发布 ⇒ 在 master 上「README 写着 1.1.0 而版本头是 1.1.34」是**对的**，
判据 3 只能标 `[跳过]`。用户要求版本显示改成反映当前源码树之后，才有上面这条前提。
这条留在这里是因为它是**判据的前提**而不是判据本身 —— 前提写错时判据会在一个
本来正确的树上恒红，而"恒红"比"恒绿"更容易被当成噪声忽略。）

真正的病在**出包那一刻**：`package_release.py` 只是 `shutil.copy2` 把 README 拷进
包里，没有任何一步看一眼里面的版本号。于是切 1.1.35 的时候，产出的
`uvcpp-1.1.35-<platform>.zip` 里会装着一份**自称 1.1.34** 的 README —— 这个形状在本仓
已经判过两次刑（`release.yml` 里那条死掉的 `env: UVCPP_VERSION: '1.1.0'`、
`package_release.py` 曾经写死的 "1.1.0"），两次都是"手写在一个没人同步的地方"。

## 判据

  1. **4 处版本串必须完全相同**，抓"只改了英文那份、忘了中文那份"（反过来也一样）。
  2. **命中总数必须恰好 4**。防的是判据 1 空转：把版本行**删掉**、或者把写法改掉，
     只留判据 1 的话它一声不响地全绿。一个恒绿的判据比没有判据更坏 ——
     它会训练人忽略它。
  3. **4 处必须都等于头文件那个串**（`--expect` 可以换掉其中"版本号"那一段）。
     改头文件与改 README 两步于是**少不掉任何一步**，因为判据就长在它们中间。

扫两种写法：shields 徽章的 `badge/version-<X.Y.Z>[-dev]`，以及加粗的 Version /
版本 行（值在反引号里）。两种写法、两个 README 各两处，一共 4 处。

**徽章里那个连字符要写成 `--`**：shields 的静态徽章拿 `-` 当分隔符，
`badge/version-1.1.34-dev-blue.svg` 直接 404（实测过），只有
`badge/version-1.1.34--dev-blue.svg` 才渲染成 `version | 1.1.34-dev`。本脚本匹配到
之后把 `--` 还原成 `-` 再比。

**不扫 `RELEASE.md`**：它是发布说明，"讲的是哪一版"与"现在是哪一版"是两件事，
把它卡进来只会逼着每次升档去改一份历史文档的标题。

用法：
    python tests/tools/check_doc_versions.py                 # push 上跑（判据 1+2+3）
    python tests/tools/check_doc_versions.py --expect 1.1.35 # 出包前跑（换掉版本号那段）

退出码 0 全过；1 有判据红了；3 前提不满足（列不出根目录 / 读不到版本头 / 版本头的
后缀规则不再是本脚本转写的那一条）—— 3 的意思**不是"红了"**，是"没判"。
"""

import argparse
import os
import re
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
# 有意的：它是唯一来源，派生出来的东西（包名、pc 文件、README 顶上这一行）都跟着它走。
VERSION_HEADER = os.path.join("src", "uvcpp", "uvcpp_version.h")

# 徽章那条要连 `badge/version-` 一起匹配 —— 只匹配 `1.1.34` 这种裸串会把 README 里
# 所有提到版本的地方（变更日志、示例输出）一起卷进来，造出假失败。
PATTERNS = [
    ("shields 徽章", re.compile(r"badge/version-(\d+\.\d+\.\d+(?:--dev)?)")),
    ("英文 Version 行", re.compile(r"\*\*Version\*\*:\s*`(\d+\.\d+\.\d+(?:-dev)?)`")),
    ("中文 版本 行", re.compile(r"\*\*版本\*\*：\s*`(\d+\.\d+\.\d+(?:-dev)?)`")),
]

# 两个 README × 两种写法。少一处就说明扫的东西变了，判据 1 可能在空转。
EXPECTED_HITS = 4


def _int_macro(text, name):
    m = re.search(r"^#define\s+%s\s+(\d+)\s*$" % name, text, re.M)
    if not m:
        return None, "读不到 %s —— 版本号来源变了，先修这里" % name
    return m.group(1), None


def _str_macro(text, name):
    m = re.search(r'^#define\s+%s\s+"([^"]*)"\s*$' % name, text, re.M)
    if not m:
        return None, "读不到 %s —— 版本号来源变了，先修这里" % name
    return m.group(1), None


def header_version(root):
    """头文件报出来的那个显示串，外加拼出它的三段。

    返回 `(info, err)`；`info = {"display": "1.1.34-dev", "base": "1.1.34",
    "suffix": "", "dev": "-dev"}`。调用方拿 `base` 换版本号那段、拿 `suffix` 与
    `dev` 重新拼 `--expect` 的期望串。

    **这是头文件里那条规则的转写**，不是为了方便重新发明一条：头文件用
    `#if UVCPP_VERSION_IS_RELEASE` 决定 `UVCPP_VERSION_STRING` 末尾挂不挂
    `UVCPP_RELEASE_TAG`，而 `UVCPP_RELEASE_TAG` 在开发版上是 `"-dev"`。转写的好处是
    不用起预处理器（那要一套工具链，而这个脚本要在没有编译器的 runner 上跑）；
    代价是它可能与头文件**走散** —— 所以下面那条自检不是装饰：后缀字面量一改，
    这里立刻以 rc=3 停下，而不是拿着一个错的期望串去判。
    """
    p = os.path.join(root, VERSION_HEADER)
    if not os.path.exists(p):
        return None, "找不到版本头 %s" % p
    with open(p, encoding="utf-8") as f:
        text = f.read()

    nums = []
    for macro in ("UVCPP_VERSION_MAJOR", "UVCPP_VERSION_MINOR",
                  "UVCPP_VERSION_PATCH"):
        v, err = _int_macro(text, macro)
        if err:
            return None, "%s（%s）" % (err, p)
        nums.append(v)
    base = ".".join(nums)

    # 自检：`-dev` 这条规则必须还是我们转写的这一条。头文件里 RELEASE_TAG 有
    # `#if/#else` 两个定义、注释行里也可能出现别的字面量，所以只认整行的
    # `#define`，再要求**这两个字面量的集合**正好是 `{"", "-dev"}`。
    tags = sorted(set(re.findall(
        r'^#define\s+UVCPP_RELEASE_TAG\s+"([^"]*)"\s*$', text, re.M)))
    if tags != ["", "-dev"]:
        return None, ("版本头里 UVCPP_RELEASE_TAG 的整行定义是 %r，不再是 "
                      "['', '-dev'] —— 后缀规则变了，本脚本那条转写要跟着改"
                      "（%s）" % (tags, p))

    is_release, err = _int_macro(text, "UVCPP_VERSION_IS_RELEASE")
    if err:
        return None, "%s（%s）" % (err, p)
    suffix, err = _str_macro(text, "UVCPP_VERSION_SUFFIX")
    if err:
        return None, "%s（%s）" % (err, p)

    info = {
        "base": base,
        "suffix": suffix,
        "dev": "" if is_release == "1" else "-dev",
    }
    info["display"] = info["base"] + info["suffix"] + info["dev"]
    return info, None


def scan_files(root):
    """要扫的文件：**仓库根这一层的 `*.md`**，不递归、不查 git。

    两条都是踩出来的：

    * **不查 git**：CI 的 `mingw64` 档在 MSYS2 的 MINGW64 环境里跑 python，那套 PATH
      上**没有 git**（脚本报 `[WinError 2]`，而 runner 自己的 git 在
      `C:\\Program Files\\Git\\bin`，不在这条 PATH 上）。原来那版用 `git ls-files`，
      于是那条档上一条判据都没跑就退出 3 —— **判据的前置依赖一个与判据无关的外部
      命令**，就是一个会静默空转的洞。
    * **不递归**：`package_release.py` 出包时会在仓库里落下暂存副本
      （`dist/libuvcpp-<版本>-<平台>/README.md`），递归会把副本一起数进来，判据 2 的
      "恰好 4 处"立刻变假失败；`_backup_*/` 也是同样形状。构建树里的三方 README 更
      麻烦 —— 那里出现 shields 徽章并不罕见，会造出真假难辨的红。

    **边界（写出来是因为它是静默的）**：往里层加的版本串，这条判据看不见。真加了
    就把它一起挪进来。下面会把实际扫到的文件逐个印出来，让扫描范围可见。
    """
    try:
        names = sorted(n for n in os.listdir(root) if n.lower().endswith(".md"))
    except (IOError, OSError) as e:
        return None, "列不出 %s 下的 md：%s" % (root, e)
    if not names:
        return None, "%s 下一个 md 都没有 —— 仓库根传错了？" % root
    return names, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None,
                    help="仓库根（默认取本脚本的上两级目录）")
    ap.add_argument("--expect", default=None, metavar="X.Y.Z",
                    help="换掉期望串里「版本号」那一段（出包脚本传它："
                         "`--version` 能覆盖头文件，那时要出的就是命令行给的那个）。"
                         "带不带 -dev 仍按头文件判。")
    ap.add_argument("--this-is-a-release", action="store_true",
                    help="按发布版口径判（期望串不带 -dev），不看头文件里那个宏。"
                         "给两处用：① 从一棵忘了翻 IS_RELEASE 的树上裁发布；"
                         "② 判据 3 自己的对照 —— 期望串必须真的被比过一次。")
    args = ap.parse_args()

    root = args.root
    if root is None:
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    print("仓库根: %s" % root)

    files, err = scan_files(root)
    if files is None:
        print("  [停] %s" % err)
        print("\n==== 汇总 ====")
        print("前提不满足，退出 3（**一条判据都没判**）")
        return 3
    print("  扫了仓库根下 %d 个 md：%s" % (len(files), " ".join(files)))

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
                    # 徽章里那个连字符是 `--`（见文件头），还原成 `-` 再比。
                    hits.append((rel, lineno, label, m.group(1).replace("--", "-")))

    # ---- 判据 1 ----
    print("\n---- 判据 1：两份 README 必须说同一个版本 ----")
    if not hits:
        fail("一处版本串都没扫到（下面判据 2 会说明该有几处）")
    else:
        counts = {}
        for h in hits:
            counts[h[3]] = counts.get(h[3], 0) + 1
        if len(counts) == 1:
            ok("%d 处写法一致：%s" % (len(hits), hits[0][3]))
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

    # ---- 判据 3 ----
    # 版本头是**这一条**的前提（前两条只读 README，不依赖它）。所以它读不出来时
    # 前两条照样判完，只有这一条标 `[停]` 并让 rc=3 —— "能判的先判，判不了的停下"，
    # 而不是把它上面的两条一起吞掉。
    info, err = header_version(root)
    if info is None:
        print("\n---- 判据 3：这 %d 处必须等于源码树版本 ----" % len(hits))
        print("  [停] %s" % err)
        premise = err
    else:
        print("版本头: %s（%s）" % (info["display"], VERSION_HEADER))
        base = args.expect or info["base"]
        want = base + info["suffix"] + ("" if args.this_is_a_release else info["dev"])
        premise = None

        print("\n---- 判据 3：这 %d 处必须等于源码树版本（%s）----" % (len(hits), want))
        if base != info["base"]:
            print("  （--expect 把版本号那段换成了 %s，头文件里是 %s）"
                  % (base, info["base"]))
        if args.this_is_a_release and info["dev"]:
            print("  （--this-is-a-release：头文件说这是开发版，按发布口径判"
                  " —— 期望串不带 %s）" % info["dev"])
        bad = 0
        for rel, lineno, label, v in hits:
            if v != want:
                fail("%s:%d %s 写的是 %s，而源码树是 %s"
                     % (rel, lineno, label, v, want))
                bad += 1
        if bad == 0 and hits:
            ok("全部等于 %s" % want)
        elif bad == 0:
            print("  （一处都没扫到，这条没得判 —— 上面判据 2 已红）")
        else:
            print("  → 把 README.md / README.zh.md 顶上那几处改成 %s" % want)

    print("\n==== 汇总 ====")
    if premise:
        # 「跳过」不能只在上面那一行 —— 汇总里必须能看出**这条判据没判**，
        # 否则"绿"和"没判"在报告末尾长得一样。
        print("（判据 3 **没判**：%s）" % premise)
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures:
            print("  - %s" % f)
        # 有红就报红：判据 3 没跑成不该把已经判出来的红盖掉。
        return 1
    if premise:
        print("前提不满足，退出 3（判据 1/2 判过了，判据 3 **没判**）")
        return 3
    print("全过（%d 处版本串 == %s，与 %s 一致）" % (len(hits), want, VERSION_HEADER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
