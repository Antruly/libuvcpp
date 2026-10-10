#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 tag 从 `RELEASE.md` 切出这一版的发布正文，前面缀一张下载表。

**为什么要有它。** 此前 `publish` 把整份 `RELEASE.md` 当成 Release 正文
（`gh release create --notes-file RELEASE.md`）—— 那是一份八百多行的历史，每一次
发版贴的都是同一份，读者得自己在里面找"这一版新增了什么"；而"这一版有哪六个包、
点哪个"文件里**根本没有**（原来那个 `## 下载` 只列纯文本 zip 名，一个 URL 都没有）。

现在正文由本工具按 tag 现切：`## 下载 (Download)` + 六行直链表 → 空行 → 目标版本
那个 `## vX.Y.Z 重点 (Highlights)` 小节 → 一行指向 CHANGELOG.md。`RELEASE.md` 从此
只是**归档**与切片源，旧小节不追改（要补后来的事实补进 CHANGELOG.md）。

**切片怎么认版本。** 找标题里含版本记号的 `##` 小节（`v1.6.0` 与裸 `1.6.0` 都认），
切到下一个 `##` 为止。**按"标题含记号"而不是按标题字面量匹配**，是因为历史小节不
同一个形状：`v1.2.0` 那条写的是 `## v1.2.0 重点 (Highlights)`，而 v1.1.0 那条是
`## 新增模块 (New in v1.1.0)` —— 只认前一种写法，重发 v1.1.0 时就会说"没有这一版"。

**平台清单与资产命名式只有一处来源**：直接 `from package_release import PLATFORMS`。
抄一份到本文件里，两处迟早会漂 —— 而漂掉的症状是"下载表里有一个 404 的链接"，
CI 里的绝对 URL 没有任何门禁看得见。

**v1.0.0 切不出来，这是事实不是缺陷**：它早于这套机制，`RELEASE.md` 里就没有它的小节，
`workflow_dispatch` 重发它会退 1 并直说原因。（实测过：它连 GitHub Release 都没有，
所以"切不出来"这件事并没有堵住任何真的重发路径 —— 其余五个 tag `v1.1.0`～`v1.5.0`
都切得出，且逐个对着线上资产列表核过。）已知，记在 `doc/release-process.md` 的
「Known gaps」里。

**tag 与版本头不符，只在版本头自称"发布版"时才判红。** 一开始写的是"不符就退 1"，
那是错的：`master` 上的版本头永远是**下一个**版本的 `-dev`，而重发历史 tag
（`workflow_dispatch` 输入 `v1.5.0` 去 `gh release edit` 改旧正文）本来就该在这样一棵
树上跑 —— 拿版本头去卡它，等于把自己的门禁架在文档里写明的路径上。现在只在
`UVCPP_VERSION_IS_RELEASE` 非 0（这棵树自称"我就是 X.Y.Z 这个发布"）时才要求两者相等；
开发树上不符只打一行说明。

**这条仍有洞，写在这里不粉饰**：拿 `v1.6.0` 去标一棵版本头还是 `1.5.3-dev` 的树，
本工具不会拦。拦它的是别处：`package_release.py` 的包名与 `uvcpp.pc` 的 `Version`
都取自同一个版本头，所以那时产出的会是 `libuvcpp-1.5.3-*.zip`，本工具生成的下载表
指的 `.../download/v1.6.0/libuvcpp-1.6.0-*.zip` 当场 404。症状在发版那一步可见，
只是不在这一步。

用法（**stdout 只有正文，诊断全在 stderr** —— 所以不打 `--out` 也能直接管进文件）：
    python3 tests/tools/release_notes.py --tag v1.6.0 > /tmp/notes.md
    python3 tests/tools/release_notes.py --tag v1.6.0 --out /tmp/notes.md
    python3 tests/tools/release_notes.py --tag v1.6.0 | head -40
    python3 tests/tools/release_notes.py --self-test

退出码 0 生成成功；1 判据红了（没有该 tag 的小节 / 小节畸形 / tag 与版本头不符）；
3 前提不满足（`RELEASE.md` 读不到、`package_release` import 失败、tag 不合法）。
**3 的意思不是"红了"，是"没判"** —— 但 CI 里非 0 一律当失败，别把 3 读成放行。
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# 与 `package_release.py` 同一个默认值。CI 里 `$GITHUB_REPOSITORY` 一定在，
# 本机跑时它不在，才落回这个常量。
DEFAULT_REPO = "Antruly/libuvcpp"

RELEASE_MD = os.path.join(ROOT, "RELEASE.md")

# `v1.6.0` 与 `1.6.0` 都收；`v1.6`、`1.6.0-dev`、`release-1.6.0` 都不收 —— 前两个
# 不是本仓的 tag 形状，第三个是另一个记号，认了只会把"打错的 tag"放行到下一步。
TAG_RE = re.compile(r"^v?(\d+\.\d+\.\d+)$")

# `## 标题`。**只要 `##`**：`###` 是小节内部的标题（`### 修复` 之类），认它会把
# 那一版切碎。
H2_RE = re.compile(r"^##\s+(.*?)\s*$")

failures = []
premises = []

# Windows 的 CI runner 默认用 GBK 解两个流，中文判据会打成乱码 —— 红的那几行
# 恰好是最需要看清的。只在能改的时候改（3.7+），失败也不影响判据。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        try:
            _stream.reconfigure(encoding="utf-8")
        except (ValueError, OSError):
            pass


def log(msg=""):
    """**一切诊断走 stderr，stdout 只承载正文。**

    混在一起的话 `--out` 就从可选项变成了必需项：`release_notes.py --tag v1.6.0`
    的 stdout 里会先冒出两行 `tag=… 版本头=…`，于是 `| head -40` 看到的不是正文，
    而"把带诊断的 stdout 直接当正文用"就是这么发生的。
    """
    print(msg, file=sys.stderr)


def fail(what):
    failures.append(what)
    log("  [红] %s" % what)


def halt(what):
    """前提不满足：这一步**没判**。它不是红，但也绝不是绿。"""
    premises.append(what)
    log("  [停] %s" % what)


# --------------------------------------------------------------------------
# 纯函数（自检直接喂内存 fixture，不碰盘）
# --------------------------------------------------------------------------

def normalize_tag(tag):
    """`v1.6.0` / `1.6.0` -> `1.6.0`；不合法返回 `None`。"""
    m = TAG_RE.match((tag or "").strip())
    return m.group(1) if m else None


def version_marker(version):
    """标题里认版本记号的那条正则。

    两侧都要挡：`(?<![\\w.])` 挡住 `v1.1.0` 被当成 `1.1.0` 之外的东西匹配、
    也挡住 `xv1.6.0`；`(?![\\w.])` 挡住 `1.1.01`、`1.1.0-beta` 这类更长的串。
    `v?` 让"裸版本号"与"带 v"两种写法都认。
    """
    return re.compile(r"(?<![\w.])v?" + re.escape(version) + r"(?![\w.])")


def slice_section(text, version):
    """从 `text` 里切出标题含 `version` 的那个 `##` 小节。

    返回 `(正文, 错误)`，两者恰有一个非 None。正文**含标题行**，不含尾随空行。
    """
    lines = text.split("\n")
    pat = version_marker(version)

    starts = [i for i, l in enumerate(lines)
              if H2_RE.match(l) and pat.search(H2_RE.match(l).group(1))]
    if not starts:
        return None, ("RELEASE.md 里没有标题含 `v%s` 的 `##` 小节 —— "
                      "这一版要么还没写，要么标题里的版本记号形状变了" % version)
    if len(starts) > 1:
        titles = ", ".join("第 %d 行" % (i + 1) for i in starts)
        return None, ("RELEASE.md 里有 %d 个标题含 `v%s` 的 `##` 小节（%s）—— "
                      "切不出唯一一节，先合并它们" % (len(starts), version, titles))

    s = starts[0]
    ends = [i for i, l in enumerate(lines) if i > s and H2_RE.match(l)]
    e = ends[0] if ends else len(lines)
    body = "\n".join(lines[s:e]).rstrip("\n")

    # 畸形：只有标题、或者标题之后一行实质内容都没有。一个空的 Release 正文比
    # 一次失败的发版更坏 —— 它发出去就没法"重跑一下"了。
    rest = [l for l in body.split("\n")[1:] if l.strip()]
    if not rest:
        return None, ("`v%s` 那一节只有标题、没有正文（第 %d 行）" % (version, s + 1))
    return body, None


def version_mismatch_reason(header, is_release, version):
    """tag 与版本头不符时该说什么。**返回 None 表示不用判红。**

    只有 `is_release` 为真才较真：那时这棵树自称"我就是 X.Y.Z 这个发布"，tag 说
    别的版本就是自相矛盾。开发树（`-dev`）不符是常态 —— `master` 的头永远指着
    下一个版本，而重发历史 tag 正是在它上面跑。
    """
    if header is None:
        return None
    if header == version:
        return None
    if is_release:
        return ("tag 是 v%s，而 src/uvcpp/uvcpp_version.h 自称发布版 %s —— "
                "打 tag 与翻版本头是同一件事的两半，别分开做"
                "（见 doc/release-process.md）。演练用 --allow-version-mismatch"
                % (version, header))
    return None


def asset_name(version, slug):
    """资产名。**与 `package_release.py` 的 `name = "libuvcpp-%s-%s"` 同式** ——
    它那边是 `(args.version, args.platform)`，这里 `version` 就是同一条字符串。"""
    return "libuvcpp-%s-%s.zip" % (version, slug)


def asset_url(repo, version, slug):
    return "https://github.com/%s/releases/download/v%s/%s" % (
        repo, version, asset_name(version, slug))


def download_table(platforms, repo, version):
    """六行表。顺序取 `platforms` 自己的插入顺序（那是发布腿的排列顺序），
    不在这里另排一份 —— 另排一份就又成了一处会漂的地方。"""
    rows = ["| 平台 | 包 |", "|---|---|"]
    for slug in platforms:
        rows.append("| `%s` | [%s](%s) |" % (
            slug, asset_name(version, slug), asset_url(repo, version, slug)))
    return "\n".join(rows)


def build_body(platforms, repo, version, section):
    """正文 = 下载表 + 目标小节 + 一行指针。"""
    return "\n".join([
        "## 下载 (Download)",
        "",
        "六个平台各一份预编译包，点名字直接下载：",
        "",
        download_table(platforms, repo, version),
        "",
        section,
        "",
        "---",
        "",
        "本版的逐条说明，以及**按主题汇总的完整清单**（每条都标出它首次出现的开发档号）：",
        "[CHANGELOG.md](https://github.com/%s/blob/master/CHANGELOG.md)。" % repo,
        "",
    ])


# --------------------------------------------------------------------------
# 自检：把每一种形状各造一遍，写明期望它怎么判
# --------------------------------------------------------------------------

# 内存 fixture。**故意把 v1.1.0 那节写成历史里的怪形状**（标题里没有 `重点`），
# 切片规则要是哪天被改成"按标题字面量匹配"，这一格会当场红。
FIXTURE = "\n".join([
    "# libuvcpp Release Notes",
    "",
    "## 简介 (Introduction)",
    "",
    "一段简介。",
    "",
    "## v1.6.0 重点 (Highlights)",
    "",
    "### 新增模块：数据库",
    "",
    "正文 A。",
    "",
    "## v1.5.0 重点 (Highlights)",
    "",
    "正文 B。",
    "",
    "## 新增模块 (New in v1.1.0)",
    "",
    "正文 C。",
    "",
    "## 预编译产物 (Prebuilt binaries)",
    "",
    "正文 D。",
    "",
])


def self_test(platforms):
    """返回不符期望的格数。**0 才是通过。**

    每一格写的是"期望怎么判"，不是"看它没崩" —— 判据改了之后重跑本表，任何一格
    与期望不符就是回归。
    """
    bad = 0

    def check(name, got, want):
        nonlocal bad
        if got == want:
            log("  [绿] %s" % name)
        else:
            bad += 1
            log("  [红] %s：期望 %r，实得 %r" % (name, want, got))

    # S1 切片只取目标小节，且止于下一个 `##`
    body, err = slice_section(FIXTURE, "1.6.0")
    check("S1 v1.6.0 切得出", err, None)
    check("S2 v1.6.0 含自己的标题", body.split("\n")[0], "## v1.6.0 重点 (Highlights)")
    check("S3 v1.6.0 含自己的正文", "正文 A。" in body, True)
    check("S4 v1.6.0 不含下一节的正文", "正文 B。" in body, False)
    check("S5 v1.6.0 不含下一节的标题", "## v1.5.0" in body, False)
    check("S6 v1.6.0 不含上面的节", "## 简介" in body, False)

    # S7 历史怪形状：标题里没有 `重点`，只有 `(New in v1.1.0)`
    body, err = slice_section(FIXTURE, "1.1.0")
    check("S7 v1.1.0 的怪标题也切得出", err, None)
    check("S8 v1.1.0 切到的是那一节", "正文 C。" in body, True)

    # S9/S10 两侧边界：既要挡住 `v1.1.01` 被当成 `1.1.0`，也要挡住 `xv1.1.0`。
    # 少了左边界，`libuvcpp-1.6.0` 之类也会被吸进来 —— 现在只扫 `## ` 标题行，
    # 但标题行里真出现包名（"下载 libuvcpp-1.6.0-linux-x64"）时就会切错节。
    check("S9 1.1.0 不会匹配 1.1.01（右边界）",
          bool(version_marker("1.1.0").search("v1.1.01")), False)
    check("S10 0.1.0 不会匹配 x0.1.0（左边界）",
          bool(version_marker("0.1.0").search("x0.1.0")), False)
    check("S10b 0.1.0 不会匹配 xv0.1.0（左边界 + v）",
          bool(version_marker("0.1.0").search("xv0.1.0")), False)
    check("S10c 边界不误伤正常标题",
          bool(version_marker("1.1.0").search("新增模块 (New in v1.1.0)")), True)

    # S11 未知 tag -> 红（rc 1 的那条路）
    _, err = slice_section(FIXTURE, "9.9.9")
    check("S11 未知 tag 报错", err is not None, True)

    # S12 只有标题、没有正文 -> 红
    _, err = slice_section("## v2.0.0 重点\n\n## v1.0.0 重点\n\n正文\n", "2.0.0")
    check("S12 空小节报错", err is not None, True)

    # S13 同一版两个小节 -> 红（切不出唯一一节）
    _, err = slice_section("## v2.0.0 重点\n\n甲\n\n## v2.0.0 又一段\n\n乙\n", "2.0.0")
    check("S13 重复小节报错", err is not None, True)

    # S14 下载表六行，slug 集合 == set(PLATFORMS)
    tbl = download_table(platforms, "Antruly/libuvcpp", "1.6.0")
    rows = [l for l in tbl.split("\n") if l.startswith("| `")]
    check("S14 下载表行数 == 平台数", len(rows), len(platforms))
    slugs = set(re.findall(r"^\| `([^`]+)` \|", tbl, re.M))
    check("S15 slug 集合 == set(PLATFORMS)", slugs, set(platforms))

    # S16 URL 形状：逐行核，不是只看一行
    want_urls = ["https://github.com/Antruly/libuvcpp/releases/download/v1.6.0/"
                 "libuvcpp-1.6.0-%s.zip" % s for s in platforms]
    got_urls = re.findall(r"\]\((https://[^)]+)\)", tbl)
    check("S16 URL 逐行相符且有序", got_urls, want_urls)
    check("S17 资产名式与 package_release 同式",
          asset_name("1.6.0", "linux-x64"), "libuvcpp-1.6.0-linux-x64.zip")

    # S18 tag 规范化
    check("S18 v1.6.0 -> 1.6.0", normalize_tag("v1.6.0"), "1.6.0")
    check("S19 1.6.0 -> 1.6.0", normalize_tag("1.6.0"), "1.6.0")
    for bad_tag in ("v1.6", "1.6.0-dev", "release-1.6.0", "", "v1.6.0.1"):
        check("S20 非法 tag %r 不收" % bad_tag, normalize_tag(bad_tag), None)

    # S21 正文：表在前、小节在中、指针在后
    full = build_body(platforms, "Antruly/libuvcpp", "1.6.0", "## v1.6.0 重点\n\n正文。\n")
    check("S21 正文以下载表标题开头", full.startswith("## 下载 (Download)\n"), True)
    check("S22 正文含切出的小节", "## v1.6.0 重点" in full, True)
    check("S23 正文结尾指向 CHANGELOG", full.rstrip().endswith("CHANGELOG.md)。"), True)
    check("S24 正文里没有第二个一级标题", full.count("\n# "), 0)

    # S25 RELEASE.md 读不到 -> rc 3 的那条路
    check("S25 不存在的路径读不出来", read_release(os.path.join(HERE, "no-such-file.md"))[1] is not None, True)

    # S26-S29 tag↔版本头那条判据的四种组合。**关键是 S28**：发布版不符要红，
    # 而开发树不符（S27）必须放行，否则重发历史 tag 会被自己的门禁挡住。
    check("S26 发布版 + 相符 -> 不红",
          version_mismatch_reason("1.6.0", True, "1.6.0"), None)
    check("S27 开发版 + 不符 -> 不红（重发历史 tag 的路）",
          version_mismatch_reason("1.5.3", False, "1.5.0"), None)
    check("S28 发布版 + 不符 -> 红",
          version_mismatch_reason("1.6.0", True, "1.7.0") is not None, True)
    check("S29 版本头读不到 -> 不红（另走 rc 3）",
          version_mismatch_reason(None, True, "1.6.0"), None)

    return bad


# --------------------------------------------------------------------------
# I/O
# --------------------------------------------------------------------------

def read_release(path):
    """返回 `(文本, 错误)`。"""
    try:
        with open(path, encoding="utf-8") as f:
            return f.read(), None
    except OSError as e:
        return None, "读不到 %s：%s" % (path, e)


def header_is_release(pr):
    """`UVCPP_VERSION_IS_RELEASE` 非 0 吗？返回 `(布尔, 错误)`。

    版本串走 `package_release._header_version()`（唯一来源），"自称发布版吗"这个
    标志那边没暴露，所以读同一个文件 —— 路径也取 `package_release.ROOT`，不另写
    一份。读不到算前提不满足（rc 3），不当成 False：把"读不到"静默读成"不是发布版"
    正好会关掉上面那条判据。
    """
    p = os.path.join(pr.ROOT, "src", "uvcpp", "uvcpp_version.h")
    try:
        with open(p, encoding="utf-8") as f:
            s = f.read()
    except OSError as e:
        return None, "读不到版本头 %s：%s" % (p, e)
    m = re.search(r"^#define\s+UVCPP_VERSION_IS_RELEASE\s+(\d+)\s*$", s, re.M)
    if not m:
        return None, "版本头 %s 里找不到 UVCPP_VERSION_IS_RELEASE" % p
    return m.group(1) != "0", None


def main():
    ap = argparse.ArgumentParser(
        description="按 tag 从 RELEASE.md 切出发布正文（前面缀下载表）。")
    ap.add_argument("--tag", default=None,
                    help="tag 或版本号，`v1.6.0` 与 `1.6.0` 都行")
    ap.add_argument("--repo", default=None,
                    help="owner/name，默认 $GITHUB_REPOSITORY，再默认 %s" % DEFAULT_REPO)
    ap.add_argument("--out", default=None, help="写到这个文件（默认打 stdout）")
    ap.add_argument("--release-md", default=RELEASE_MD, help="切片源（自检/对照用）")
    ap.add_argument("--allow-version-mismatch", action="store_true",
                    help="排练用：版本头还没翻的树上预演某一版的正文。CI 不传它。")
    ap.add_argument("--self-test", action="store_true",
                    help="跑内置的形状表，不读盘、不写盘")
    args = ap.parse_args()

    try:
        from package_release import PLATFORMS, _header_version
        import package_release as pr
    except Exception as e:                                   # noqa: BLE001
        halt("import package_release 失败（%s）—— 平台清单与命名式的唯一来源，"
             "拿不到就不该猜" % e)
        log("\n==== 汇总 ====\n没判（%d 条前提不满足）" % len(premises))
        return 3

    platforms = list(PLATFORMS)
    if len(platforms) < 2:
        halt("PLATFORMS 只有 %d 个平台 —— 下载表会退化成一行" % len(platforms))

    if args.self_test:
        log("---- 形状表（内存 fixture，不碰盘）----")
        bad = self_test(platforms)
        log("\n---- 反空转：一条判据都没判时不许长成「全过」----")
        if len(platforms) < 2:
            halt("PLATFORMS 少于 2 个平台，下载表那几格是空转的")
        log("\n==== 汇总 ====")
        if bad:
            log("红：%d 格与期望不符" % bad)
            return 1
        if premises:
            log("没判（%d 条前提不满足）—— **3 不是绿**" % len(premises))
            return 3
        log("全过（%d 个平台，全部形状格与期望相符）" % len(platforms))
        return 0

    # ---- 真正的切片 -------------------------------------------------------
    if not args.tag:
        halt("没给 --tag")
        log("\n==== 汇总 ====\n没判")
        return 3

    version = normalize_tag(args.tag)
    if version is None:
        halt("tag %r 不是 vX.Y.Z / X.Y.Z 的形状" % args.tag)
        log("\n==== 汇总 ====\n没判")
        return 3

    repo = args.repo or os.environ.get("GITHUB_REPOSITORY") or DEFAULT_REPO

    try:
        header = _header_version()
    except SystemExit as e:
        halt("读不到版本头（%s）" % e)
        log("\n==== 汇总 ====\n没判")
        return 3

    is_release, err = header_is_release(pr)
    if err is not None:
        halt(err)
        log("\n==== 汇总 ====\n没判")
        return 3

    log("tag=%s  version=%s  repo=%s  版本头=%s（自称发布版：%s）"
          % (args.tag, version, repo, header, "是" if is_release else "否"))

    reason = version_mismatch_reason(header, is_release, version)
    if reason is not None:
        if args.allow_version_mismatch:
            log("  [说明] %s —— 已用 --allow-version-mismatch 略过" % reason)
        else:
            fail(reason)
    elif header != version:
        # 开发树上的常态：master 的头指着下一个版本。写出来，免得读者以为没检查。
        log("  [说明] 版本头 %s != tag v%s，但版本头自称开发版，不判红 —— "
              "重发历史 tag 走的就是这条路" % (header, version))

    text, err = read_release(args.release_md)
    if err is not None:
        halt(err)
        log("\n==== 汇总 ====\n没判")
        return 3

    section, err = slice_section(text, version)
    if err is not None:
        fail(err)

    if failures:
        log("\n==== 汇总 ====")
        for f in failures:
            log("红：%s" % f)
        return 1

    body = build_body(platforms, repo, version, section)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
        log("已写 %s（%d 行 / %d 字节）" % (args.out, body.count("\n"), len(body.encode("utf-8"))))
    else:
        sys.stdout.write(body)

    log("\n==== 汇总 ====\n生成成功（切出 `v%s` 那一节，%d 行下载表）" % (version, len(platforms)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
