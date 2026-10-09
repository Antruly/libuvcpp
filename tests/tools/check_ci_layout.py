#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核 CI 的**布局**：一个平台一个 workflow 文件、文件内按功能分格，且与规范一致。

## 为什么要它

`.github/workflows/` 原先是一个 1369 行、10 个 job、四个平台、八类功能全塞在一起
的 `ci.yml`。功能矩阵长起来之后（QUIC 是最近一格）按平台读它已经读不动：两个
Windows job 之间隔着 900 行别的平台。改成一平台一个文件、文件内按功能分格之后，
**"少了一格"这件事在 diff 里长得和一个普通的小改动一模一样** —— 删掉一个矩阵条目
只会让那一格静默不跑，四种平台文件删掉一个只会让那个平台静默不跑，徽章指向一个
被删掉的文件则是静默 404（`check_docs.py` 明说不判 `http(s)://` 目标）。
这条门禁把那份规范变成可判的。

## 判据（12 条）

  1. `.github/workflows/` 下**恰好**是那四个 `ci-*.yml`（多一个平台文件、少一个都
     红），`ci.yml` 不在；且没有任何 workflow 落在 `.github/workflows/` 的**子目录**
     里 —— `check_doc_lines.py` 的扫描范围是 `.github/workflows/*.yml`（没有 `**`），
     子目录里的文件会让它里面的引用**静默**掉出扫描集。
  2. 四个文件各有且只有一个顶层 `name:`，且四个 `name:` 互不相同。
  3. 四个文件的 `on:` 里**同时**有 `push` 与 `pull_request`（删掉一个触发条件等于
     那条腿只在半边事件上跑，而这一点在文件里看不出来）。
  4. 每个文件的 job 名与 `doc/ci-guide.md` §1 那张表里**同一个文件那一行**的 job
     两向相等（表里多一行、文件里多一个 job 都红）。单向判据挡不住"表里多一行"。
  5. 每个文件的**功能格集合**与表里那一行的 `Features` 单元格两向相等。这条是
     "某一格被悄悄删掉"不可能发生的原因 —— 尤其是 QUIC 那一格。
  6. h2 / quic 那两格的**门禁串**必须写在那一格自己的矩阵条目里（各四个：缓存开关、
     `<dep> integrated`、`Including <module> module in build`、注册用例名），且文件里
     真的用 `matrix.gate_*` 把它们接进了步骤。串在条目里、步骤里没用 = 门禁是装饰。
  7. 平台专属的字符串只在它该在的文件里，**两向**：该在的在、不该在的不在
     （`nm -D` / `openssl-3.5.0` 只在 ubuntu，`_SSL_set_quic_tls_cbs` 只在 macOS，
     `objdump -p` / `api-ms-win-` / `--platform mingw-x64` 只在 MinGW，`ilammy/…` 与
     `--cxx cl` 只在 MSVC）。这条防的是"搬错文件"与"两处都留了一份"。
  8. 每个 `if: matrix.feature == 'X'` 里的 X 必须在**同一个文件**的矩阵条目里真的存在
     （把那一格改名会让挂在它上面的步骤**无声消失**）；五条文档门禁只在一个文件里。
  9. 本脚本自己被**恰好一个**文件调用（搬运时把它漏掉，就等于这条门禁没了）。
     判据 8/9 的"调用"都按**非注释行**算 —— 四个文件顶上都在解释布局规范，顺手提一句
     `check_ci_layout.py` 就够了；只按"名字出现过"判的话，把真正的调用步骤删掉再留一行
     注释，这条判据照样绿。
 10. 两版 README 里每一个 `actions/workflows/*.yml` URL 都指向存在的文件，且四个
     平台文件在两个 README 里都被引用（徽章不会静默 404）。
 11. MinGW 那个文件保留 job 级的 `defaults: shell: msys2 {0}`，且其中没有 step 声明
     `shell: bash`（MSYS2 的 PATH 上没有那些命令，混一个 bash 进去是静默换环境）。
 12. 两个 `config-contract` job 各自带着正确的 `--platform`/`--cxx` 组合
     （`linux-x64` + `g++` / `msvc-x64` + `cl`）—— 这一对抄错的话，门禁跑的
     是另一套工具链而它自己不会说。

**反空转**：一条判据都没能判（四个文件都不在、§1 那张表解析不出、README 里一条
workflow URL 都没有）时退 3，不许长成"全过"。这就是 `check_doc_snippets.py` 的
`DEFAULT_OFF` 那条规矩的同一条：**"全过"与"什么都没判"在输出末尾长得一样。**

注意它与"有红就报红"的先后：**空仓库拿到的是 1 而不是 3** —— 四个文件缺席本身是判据 1
的**红**（那是真缺陷），红压过没判的那几条。3 留给"文件都在、只是判据的前提不满足"，
比如 §1 那两行 HTML 注释标记被删掉 —— 那时判据 4/5/12 一条都没得判，而它们没判的原因
不是"文件错了"。反空转这一条是结构上的兜底：它防的是有人把 `PLATFORM_FILES` 改空、
或者把判据一条条注释掉，最后"零条比较"长成"全过"。

用法：
    python tests/tools/check_ci_layout.py              # push 上跑
    python tests/tools/check_ci_layout.py --root /tmp # 反对照组（突变测试）用

退出码 0 全过；1 有判据红了；3 前提不满足（**没判**，不是"红了"）。
"""

import argparse
import glob
import os
import re
import sys

failures = []
premises = []

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


def halt(what):
    """前提不满足：这条判据**没判**。它不是红，但也绝不是绿。"""
    premises.append(what)
    print("  [停] %s" % what)


WORKFLOW_DIR = os.path.join(".github", "workflows")

# 四个平台文件。**文件名的 `ci-` 前缀与平台名是规范的一部分**（见 doc/ci-guide.md §1）：
# `ci.yml` 这个名字已经被删掉了，别把它加回来 —— 一个"汇总文件"会把刚拆开的
# 十个 job 又收回来，而那样就没有任何东西拦得住"顺手在里面多跑一遍别人的平台"。
PLATFORM_FILES = [
    "ci-linux-ubuntu.yml",
    "ci-windows-msvc.yml",
    "ci-mingw64.yml",
    "ci-macos.yml",
]

# 表里那一格写"没有矩阵"时的字面量。MinGW 那个文件是**单 job**：整条 job 挂在
# job 级 `defaults` 上、尾巴是写死的 package→consumer 合约，按功能拆它是下一批的事。
NO_MATRIX_CELL = "（无矩阵）"

GUIDE = os.path.join("doc", "ci-guide.md")
TABLE_START = "<!-- ci-layout:start -->"
TABLE_END = "<!-- ci-layout:end -->"

READMES = ["README.md", "README.zh.md"]

# ---- 判据 6：功能格自己的门禁串 -------------------------------------------------
# 每格的四个串。前三个在配置之后核（缓存开关、FetchContent 走通、`src/<module>/`
# 真进了源列表），第四个在测试之前核（用例真的被注册）。少任何一个，那一格都会去
# 编一个**不带该模块**的库然后全绿。
FEATURE_GATES = {
    "h2": ["UVCPP_ENABLE_NGHTTP2:BOOL=ON", "nghttp2 integrated",
           "Including http2 module in build", "test_h2_session_func"],
    "quic": ["UVCPP_ENABLE_QUIC:BOOL=ON", "ngtcp2 integrated",
             "Including quic module in build", "test_quic_api_func"],
    # 1.4.1：web 层多出来的那条 h3 传输。它在三条腿上（linux / macOS / MSVC），
    # **不在 MinGW 那条上** —— 与 quic 同一个理由（单 job，且 h3 还要 QUIC 先开），
    # 记在 doc/ci-guide.md §1 的"未覆盖的格"里。
    "http3": ["UVCPP_ENABLE_HTTP3:BOOL=ON", "nghttp3 integrated",
              "Including http3 module in build", "test_http3_web_func"],
    # 1.5.2：`UVCPP_ENABLE_WSDL` 默认 OFF，而发布腿自这一版起**显式开**它（发布包
    # 从此带 WSDL/SOAP）。这一格就是那条发布配置在 push/PR 阶段的替身。
    #
    # **登记在这里不是可选的** —— `FEATURE_GATES.get(feat)` 对未登记的 feature
    # 返回 None，判据 6 会**静默跳过**它。不登记就等于新加的那一格没有任何东西
    # 拦得住"它其实编了一个不带 wsdl 的库然后全绿"，而那正是这一整条判据存在的
    # 理由（见上面那段注释）。
    "wsdl": ["UVCPP_ENABLE_WSDL:BOOL=ON", "pugixml integrated",
             "Including wsdl module in build", "test_wsdl_document_func"],
}

# 矩阵条目上承载这些串的字段名。文件里必须真的出现 `matrix.<字段>`（判据 6 后半）。
GATE_FIELDS = ["gate_switch", "gate_integrated", "gate_module", "gate_test"]

# ---- 判据 7：平台专属字符串，**两向** --------------------------------------------
# 值是"这个串允许出现在哪些文件里"，缺一个、多一个都红。
LITERAL_OWNERS = {
    # ubuntu：QUIC 那一格自己编一份 OpenSSL（唯一一档这么做的）
    "nm -D": {"ci-linux-ubuntu.yml"},
    "openssl-3.5.0": {"ci-linux-ubuntu.yml"},
    "$HOME/openssl-quic": {"ci-linux-ubuntu.yml"},
    "--platform linux-x64": {"ci-linux-ubuntu.yml"},
    # macOS：Homebrew 的 keg-only openssl@3，必须显式钉 root；符号带前导下划线
    "brew --prefix openssl@3": {"ci-macos.yml"},
    "_SSL_set_quic_tls_cbs": {"ci-macos.yml"},
    # MSVC
    #
    # 2026-09-30：Windows 上装 OpenSSL 的三个地方（矩阵那四格、config-contract、
    # release.yml 的 msvc-x64）统一收到 `.github/scripts/win-openssl-deps.sh`。
    # 起因是裸 `choco install openssl` 从 2026-09-29 20:46 UTC 起在那两张 windows
    # 镜像上都恒非 0 退出（148，没有查到有出处的解释），而 workflow 逐字节没改
    # （`git diff 0addc23f 699d27e -- .github/workflows/ci-windows-msvc.yml` 是空的）。
    # 那一步真正要的是"这台机器上有一份 CMake 找得到的 OpenSSL"，所以判据从
    # "choco 退出 0"改成"文件在不在"，理由与实测都写在那个脚本头部。
    #
    # 于是这里**两向**钉住两件事：调用点在，且裸 choco 那一行从四个平台文件里
    # 消失（第二项是空集 —— 判据 7 本来就支持空集，它判的是"多一个也红"）。
    # 换句话说：谁要是把某处改回裸 `choco install openssl --no-progress` 来"修"
    # 一次红，这条断言会把那一改钉回来。
    "bash .github/scripts/win-openssl-deps.sh": {"ci-windows-msvc.yml"},
    "choco install openssl --no-progress": set(),
    "ilammy/msvc-dev-cmd": {"ci-windows-msvc.yml"},
    "--platform msvc-x64": {"ci-windows-msvc.yml"},
    "--cxx cl": {"ci-windows-msvc.yml"},
    # MinGW：自包含 DLL 的断言 + MSYS2 那一套
    "objdump -p": {"ci-mingw64.yml"},
    "api-ms-win-": {"ci-mingw64.yml"},
    "--platform mingw-x64": {"ci-mingw64.yml"},
    "shell: msys2 {0}": {"ci-mingw64.yml"},
    "msys2/setup-msys2@v2": {"ci-mingw64.yml"},
    # 两档都有 g++ 的：linux/ELF 与 MinGW/PE 各一次
    "--cxx g++": {"ci-linux-ubuntu.yml", "ci-mingw64.yml"},
    # runner 标签：macOS / ubuntu / windows-2022 各只该有一个文件点名；
    # windows-latest 是 MSVC 矩阵格与 MinGW 共用的
    "macos-latest": {"ci-macos.yml"},
    "ubuntu-latest": {"ci-linux-ubuntu.yml"},
    "windows-2022": {"ci-windows-msvc.yml"},
    "windows-latest": {"ci-windows-msvc.yml", "ci-mingw64.yml"},
}

# ---- 判据 8：文档门禁 ------------------------------------------------------------
# 这五条是纯 python、只读仓库、与操作系统无关，所以只在一个文件的一条腿上跑。
# 把它们摊回四个文件不会更安全（判据本身与平台无关），但会让"跑了几次"变得
# 不可知 —— 而"这一格改名让五条门禁无声消失"正是判据 8 前半要拦的那种事。
DOC_GATES = ["check_doc_versions.py", "check_docs.py", "check_doc_lines.py",
             "check_doc_lines_scenes.py", "check_ci_layout.py"]
DOC_GATE_FILE = "ci-linux-ubuntu.yml"

# ---- 判据 12：使能宏契约的两档工具链 --------------------------------------------
CONFIG_CONTRACT = {
    "ci-linux-ubuntu.yml": ("--platform linux-x64", "--cxx g++"),
    "ci-windows-msvc.yml": ("--platform msvc-x64", "--cxx cl"),
}

RE_NAME = re.compile(r"^name:\s*(\S.*?)\s*$")
RE_JOB = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")
RE_FEATURE = re.compile(r"^(\s*)- feature:\s*(\S+)\s*$")
RE_GATE_FIELD = re.compile(r"^\s*(%s):" % "|".join(GATE_FIELDS))
RE_IF_FEATURE = re.compile(r"if:\s*matrix\.feature\s*==\s*'([^']*)'")
RE_MATRIX_REF = re.compile(r"matrix\.([A-Za-z_][A-Za-z0-9_]*)")
RE_BACKTICK = re.compile(r"`([^`]+)`")
RE_WORKFLOW_URL = re.compile(
    r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/actions/workflows/"
    r"([A-Za-z0-9_.-]+\.ya?ml)")


def read(root, rel):
    """读一个文件，返回 `(text, err)`。按 utf-8 读：这些文件里全是中文注释。"""
    p = os.path.join(root, rel)
    if not os.path.exists(p):
        return None, "找不到 %s" % rel
    try:
        with open(p, encoding="utf-8") as f:
            return f.read(), None
    except (IOError, OSError, UnicodeDecodeError) as e:
        return None, "读不了 %s：%s" % (rel, e)


def block_after_key(lines, start):
    """从 `start`（一个顶层键所在行）之后取出属于它的行，直到下一个顶层键。

    顶层键的定义是**行首无缩进**的非空、非注释行 —— `on:` 与 `jobs:` 都靠它收边，
    否则 `on:` 底下的 `push:` 会被当成 job。
    """
    out = []
    for i in range(start + 1, len(lines)):
        line = lines[i]
        if line.strip() == "" or line.lstrip().startswith("#"):
            out.append(line)
            continue
        if not line[:1].isspace():
            break
        out.append(line)
    return out


def job_ids(lines):
    """`jobs:` 之后按**两空格**缩进的键，就是 job 名。

    只在 `jobs:` 之后解析：`on:` 底下的 `push:` / `pull_request:` 也是两空格缩进，
    全文件扫会把它们当成 job（而"多了一个不存在的 job"正好是判据 4 要报的东西 ——
    一个恒红的判据会被训练成噪声）。
    """
    for i, line in enumerate(lines):
        if line.rstrip() == "jobs:":
            blk = block_after_key(lines, i)
            return [m.group(1) for m in (RE_JOB.match(l) for l in blk) if m]
    return []


def feature_entries(lines):
    """矩阵条目：`(feature 名, 起行, 止行)`（止行不含）。

    止行 = 下一个 `- feature:` 行，或第一行缩进 ≤ 4 的非空非注释行（那就是 job 级
    的另一个键，`steps:` 也是这么收的）。
    """
    starts = [(i, RE_FEATURE.match(l)) for i, l in enumerate(lines)]
    starts = [(i, m.group(2)) for i, m in starts if m]
    out = []
    for n, (i, feat) in enumerate(starts):
        end = len(lines)
        if n + 1 < len(starts):
            end = starts[n + 1][0]
        for j in range(i + 1, len(lines)):
            line = lines[j]
            if line.strip() == "" or line.lstrip().startswith("#"):
                continue
            indent = len(line) - len(line.lstrip())
            if indent <= 4:
                end = min(end, j)
                break
        out.append((feat, i, end))
    return out


def parse_guide_table(text):
    """`doc/ci-guide.md` §1 那张表 → `[(文件基名, job, Features 单元格原文)]`。

    用两行 HTML 注释当边界（`<!-- ci-layout:start -->` / `<!-- ci-layout:end -->`），
    而不是"§1 那一节里的第一张表"：按节次找太脆 —— 表前面多一段散文、多一张别的表，
    判据就会开始读错东西，而且它读错的样子是"红在一个无关的地方"。
    """
    if TABLE_START not in text or TABLE_END not in text:
        return None, "doc/ci-guide.md 里找不到 %s / %s 这两行标记" % (TABLE_START, TABLE_END)
    seg = text.split(TABLE_START, 1)[1].split(TABLE_END, 1)[0]
    rows = []
    for raw in seg.splitlines():
        line = raw.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 3:
            continue
        if set(cells[0]) <= set("-: "):        # 分隔行
            continue
        if cells[0].lower().startswith("workflow"):   # 表头
            continue
        fname = cells[0].strip("`")
        job = cells[1].strip("`")
        rows.append((os.path.basename(fname), job, cells[2]))
    return rows, None


def cell_features(cell):
    """`Features` 单元格 → 功能名集合；`（无矩阵）` → 空集。

    返回 `(集合, err)`：单元格既没有反引号里的名字、也不是 `（无矩阵）` 时给错 ——
    那说明这张表的写法变了，而不是"这一格没有功能"。
    """
    if NO_MATRIX_CELL in cell:
        return set(), None
    names = set(RE_BACKTICK.findall(cell))
    if not names:
        return None, ("Features 单元格既没有反引号包着的功能名、也不是 %s：%r"
                      % (NO_MATRIX_CELL, cell))
    return names, None


def invokes(text, script):
    """`text` 里有没有**真的**调用 `script`（而不是只在注释里提了它）。

    只按文件里"出现过这个名字"判会被注释骗过：四个文件顶上都在解释布局规范，
    顺手写一句"见 `check_ci_layout.py`"就够了 —— 于是把真正的调用步骤删掉，
    这条判据照样绿。跳过注释行之后，"删掉调用、留下注释"才会红。
    （行扫描器的能力边界就在这里：它读得懂注释，读不懂 YAML 语义。
    判据 8/9 要的正是"这一行是一句命令"，注释行不是。）
    """
    for l in text.splitlines():
        if l.lstrip().startswith("#"):
            continue
        if script in l:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None,
                    help="仓库根（默认取本脚本的上两级目录）。给对照组用。")
    args = ap.parse_args()
    root = args.root
    if root is None:
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    print("仓库根: %s" % root)

    texts = {}
    for name in PLATFORM_FILES:
        text, err = read(root, os.path.join(WORKFLOW_DIR, name))
        if text is None:
            halt(err)
        else:
            texts[name] = text
    readmes = {}
    for name in READMES:
        text, err = read(root, name)
        if text is None:
            halt(err)
        else:
            readmes[name] = text
    guide, gerr = read(root, GUIDE)

    # ---- 判据 1 ----
    print("\n---- 判据 1：四个平台文件恰好存在，`ci.yml` 不在，且没有子目录 ----------------")
    found = sorted(os.path.basename(p) for p in
                   glob.glob(os.path.join(root, WORKFLOW_DIR, "ci*.yml")))
    if found == sorted(PLATFORM_FILES):
        ok("恰好四个：%s" % " ".join(found))
    else:
        fail("`.github/workflows/ci*.yml` 实际是 %s，应当是 %s（`ci.yml` 已废弃，"
             "别把它加回来）" % (found, sorted(PLATFORM_FILES)))
    # 子目录会让 check_doc_lines.py 的 `.github/workflows/*.yml`（没有 `**`）扫不到 ——
    # 而"扫不到"这件事在那个门禁的输出里是**看不见**的。
    nested = sorted(os.path.relpath(p, root) for p in
                    glob.glob(os.path.join(root, WORKFLOW_DIR, "*", "*.y*ml")))
    if nested:
        fail("`.github/workflows/` 下有子目录里的 workflow：%s —— `check_doc_lines.py` "
             "的扫描范围是 `.github/workflows/*.yml`（没有 `**`），它们里面的行号引用"
             "会**静默**掉出扫描集" % nested)
    else:
        ok("`.github/workflows/` 下没有子目录里的 workflow（平铺，扫描范围盖得住）")

    # ---- 判据 2 ----
    print("\n---- 判据 2：四个顶层 `name:` 互不相同 ----")
    names = {}
    for name, text in sorted(texts.items()):
        tops = [RE_NAME.match(l).group(1) for l in text.splitlines()
                if RE_NAME.match(l)]
        if len(tops) != 1:
            fail("%s 的顶层 `name:` 有 %d 个（应当恰好一个）：%s" % (name, len(tops), tops))
        else:
            names[name] = tops[0]
    if len(names) == len(set(names.values())) == len(texts) and texts:
        ok("四个 name 互不相同：%s" % " / ".join("%s=%s" % kv for kv in sorted(names.items())))
    elif names:
        seen = {}
        for f, n in sorted(names.items()):
            seen.setdefault(n, []).append(f)
        for n, fs in sorted(seen.items()):
            if len(fs) > 1:
                fail("两个文件的顶层 `name:` 都是 %r：%s —— check 名会撞在一起" % (n, fs))

    # ---- 判据 3 ----
    print("\n---- 判据 3：四个文件的 `on:` 同时有 `push` 与 `pull_request` ----")
    for name, text in sorted(texts.items()):
        lines = text.splitlines()
        starts = [i for i, l in enumerate(lines) if l.rstrip() == "on:"]
        if not starts:
            fail("%s 没有顶层 `on:`" % name)
            continue
        blk = [l.strip() for l in block_after_key(lines, starts[0])]
        got = set(l.split(":")[0] for l in blk if l.endswith(":") or ":" in l)
        missing = [e for e in ("push", "pull_request") if e not in got]
        if missing:
            fail("%s 的 `on:` 里少了 %s（只有半边事件会触发）" % (name, missing))
        else:
            ok("%s：push + pull_request" % name)

    # ---- 判据 4/5：与 doc/ci-guide.md §1 那张表两向对齐 ----
    print("\n---- 判据 4：每个文件的 job 名与 §1 表里那一行两向相等 ----")
    rows = None
    if guide is None:
        halt("读不到 %s（%s）—— 判据 4/5/12 没得判" % (GUIDE, gerr))
    else:
        rows, terr = parse_guide_table(guide)
        if rows is None:
            halt(terr)
        elif not rows:
            halt("%s 里 %s..%s 之间一行表都没有" % (GUIDE, TABLE_START, TABLE_END))
    doc_jobs, doc_feats = {}, {}
    if rows is not None:
        for fname, job, cell in rows:
            if fname not in PLATFORM_FILES:
                fail("§1 表里有一行的文件名是 %r —— 不是四个平台文件之一" % fname)
                continue
            doc_jobs.setdefault(fname, set()).add(job)
            fs, ferr = cell_features(cell)
            if fs is None:
                fail("%s 那一行的 %s" % (fname, ferr))
            else:
                doc_feats.setdefault(fname, set()).update(fs)
        for name in PLATFORM_FILES:
            if name not in doc_jobs:
                fail("§1 表里没有 %s 这一行" % name)

    file_jobs, file_feats = {}, {}
    for name, text in sorted(texts.items()):
        lines = text.splitlines()
        file_jobs[name] = set(job_ids(lines))
        file_feats[name] = set(f for f, _, _ in feature_entries(lines))
    for name in sorted(texts):
        if name not in doc_jobs:
            continue
        if file_jobs[name] == doc_jobs[name]:
            ok("%s：job %s" % (name, sorted(file_jobs[name])))
        else:
            fail("%s 的 job 与 §1 表不符：文件里 %s / 表里 %s"
                 % (name, sorted(file_jobs[name]), sorted(doc_jobs[name])))

    print("\n---- 判据 5：每格的功能名与 §1 表里那一行的 Features 单元格两向相等 ----")
    for name in sorted(texts):
        if name not in doc_feats:
            continue
        if file_feats[name] == doc_feats[name]:
            ok("%s：%s" % (name, sorted(file_feats[name]) or "（无矩阵）"))
        else:
            fail("%s 的功能格与 §1 表不符：文件里 %s / 表里 %s —— 差集是"
                 "文件多 %s、表多 %s"
                 % (name, sorted(file_feats[name]), sorted(doc_feats[name]),
                    sorted(file_feats[name] - doc_feats[name]),
                    sorted(doc_feats[name] - file_feats[name])))

    # ---- 判据 6 ----
    print("\n---- 判据 6：h2 / quic / http3 / wsdl 的门禁串在**那一格自己的条目里**，且真接进了步骤 ----")
    for name, text in sorted(texts.items()):
        lines = text.splitlines()
        entries = feature_entries(lines)
        for field in GATE_FIELDS:
            if not any(RE_GATE_FIELD.match(l) for l in lines):
                continue
            if ("matrix.%s" % field) not in text:
                fail("%s 的矩阵条目上有 `%s:`，但文件里没有 `matrix.%s` —— "
                     "串写在条目上却没接进步骤，门禁是装饰" % (name, field, field))
        for feat, start, end in entries:
            want = FEATURE_GATES.get(feat)
            if not want:
                continue
            body = "\n".join(lines[start:end])
            missing = [w for w in want if w not in body]
            if missing:
                fail("%s 的 `%s` 那一格里少了门禁串 %s —— 这一格会去编一个不带该模块的"
                     "库然后全绿" % (name, feat, missing))
            else:
                ok("%s 的 %s 那一格：四个门禁串都在条目里" % (name, feat))

    # ---- 判据 7 ----
    print("\n---- 判据 7：平台专属字符串只在它该在的文件里（两向）----")
    for lit, owners in sorted(LITERAL_OWNERS.items()):
        got = set(n for n, t in texts.items() if lit in t)
        if got == owners:
            ok("%r 只在 %s" % (lit, sorted(owners)))
        else:
            fail("%r 的文件集合不对：实际 %s / 应当 %s（多 %s、少 %s）"
                 % (lit, sorted(got), sorted(owners),
                    sorted(got - owners), sorted(owners - got)))

    # ---- 判据 8 ----
    print("\n---- 判据 8：`if: matrix.feature` 指向存在的格；文档门禁只在一个文件 ----")
    for name, text in sorted(texts.items()):
        lines = text.splitlines()
        feats = set(f for f, _, _ in feature_entries(lines))
        # 只在**代码**行里找 `if:`：文件里那几段解释"为什么用字符串比字符串"的注释
        # 把判据本身抄了一遍（`` `if: matrix.feature == 'X'` ``），照全文扫会命中
        # 注释里的示例值 `X` 并报出一个不存在的格。
        used = set()
        for l in lines:
            if l.lstrip().startswith("#"):
                continue
            used.update(RE_IF_FEATURE.findall(l))
        bad = sorted(u for u in used if u not in feats)
        if bad:
            fail("%s 里有 `if: matrix.feature == %s`，但矩阵里没有这一格 —— "
                 "那一格改名之后，挂在它上面的步骤会**无声消失**" % (name, bad))
        else:
            ok("%s：%s 都指向存在的格" % (name, sorted(used) or "（没有这种 if）"))
    for script in DOC_GATES:
        got = sorted(n for n, t in texts.items() if invokes(t, script))
        if got == [DOC_GATE_FILE]:
            ok("%s 只被 %s 调用" % (script, DOC_GATE_FILE))
        else:
            fail("%s 出现在 %s，应当恰好是 [%s]" % (script, got, DOC_GATE_FILE))

    # ---- 判据 9 ----
    print("\n---- 判据 9：本脚本自己被恰好一个文件调用 ----")
    me = os.path.basename(__file__)
    got = sorted(n for n, t in texts.items() if invokes(t, me))
    if len(got) == 1:
        ok("%s 被 %s 调用" % (me, got[0]))
    else:
        fail("%s 被 %d 个文件调用（%s）—— 应当是恰好一个" % (me, len(got), got))

    # ---- 判据 10 ----
    print("\n---- 判据 10：两版 README 的 CI 徽章指向存在的文件，四个平台都引到 ----")
    for name, text in sorted(readmes.items()):
        refs = set(RE_WORKFLOW_URL.findall(text))
        if not refs:
            halt("%s 里一个 `actions/workflows/*.yml` URL 都没有 —— 判据没得判" % name)
            continue
        missing = sorted(r for r in refs
                         if not os.path.exists(os.path.join(root, WORKFLOW_DIR, r)))
        if missing:
            fail("%s 的徽章指向不存在的文件：%s（删掉一个 workflow 而徽章还写着它，"
                 "就是静默 404）" % (name, missing))
        absent = sorted(set(PLATFORM_FILES) - refs)
        if absent:
            fail("%s 没有引用这些平台文件：%s" % (name, absent))
        if not missing and not absent:
            ok("%s：%s 都解析得到，四个平台文件都在" % (name, sorted(refs)))

    # ---- 判据 11 ----
    print("\n---- 判据 11：MinGW 那个文件保留 job 级 msys2 shell，且没有 step 用 bash ----")
    mingw = texts.get("ci-mingw64.yml")
    if mingw is None:
        halt("没有 ci-mingw64.yml —— 判据 11 没得判")
    else:
        lines = mingw.splitlines()
        # `defaults:` 缩进在 job 之下（四空格），所以比 `strip()` 而不是 `rstrip()`。
        if any(l.strip() == "defaults:" for l in lines) and "shell: msys2 {0}" in mingw:
            ok("job 级 `defaults: shell: msys2 {0}` 在")
        else:
            fail("ci-mingw64.yml 里没有 job 级 `defaults: shell: msys2 {0}` —— "
                 "MSYS2 的 MINGW64 才提供那套 PATH，默认的 pwsh 上一条命令都跑不起来")
        bad = [i + 1 for i, l in enumerate(lines) if re.match(r"^\s*shell:\s*bash\s*$", l)]
        if bad:
            fail("ci-mingw64.yml:%s 声明了 `shell: bash` —— 那一步会拿到 pwsh 的 PATHEXT"
                 " 与命令集，而注释里说的都是 MSYS2 的前提" % bad)
        else:
            ok("ci-mingw64.yml 里没有 step 声明 `shell: bash`")

    # ---- 判据 12 ----
    print("\n---- 判据 12：两个 config-contract 各带对的 --platform/--cxx ----")
    for name, (plat, cxx) in sorted(CONFIG_CONTRACT.items()):
        text = texts.get(name)
        if text is None:
            halt("没有 %s —— 判据 12 对它的那一半没得判" % name)
            continue
        lines = text.splitlines()
        start = None
        for i, l in enumerate(lines):
            if l.rstrip() == "  config-contract:":
                start = i
                break
        if start is None:
            fail("%s 里没有 `config-contract` 这个 job" % name)
            continue
        end = len(lines)
        for j in range(start + 1, len(lines)):
            if RE_JOB.match(lines[j]):
                end = j
                break
        body = "\n".join(lines[start:end])
        missing = [s for s in (plat, cxx) if s not in body]
        if missing:
            fail("%s 的 config-contract 里少了 %s —— 这一对抄错的话，门禁跑的是"
                 "另一套工具链而它自己不会说" % (name, missing))
        else:
            ok("%s 的 config-contract：%s + %s" % (name, plat, cxx))

    # ---- 反空转 ----
    print("\n---- 反空转：一条判据都没判时不许长成「全过」 ----")
    judged = len(texts) + len(rows or []) + sum(len(RE_WORKFLOW_URL.findall(t))
                                               for t in readmes.values())
    if judged == 0:
        halt("四个平台文件、§1 表、README 徽章**一样都没读到** —— 什么都没判")
    else:
        ok("读到 %d 个文件 / %d 行表 / %d 个徽章 URL"
           % (len(texts), len(rows or []),
              sum(len(RE_WORKFLOW_URL.findall(t)) for t in readmes.values())))

    print("\n==== 汇总 ====")
    if premises:
        for p in premises:
            print("（**没判**：%s）" % p)
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures:
            print("  - %s" % f)
        # 有红就报红：一条判据没跑成不该把已经判出来的红盖掉。
        return 1
    if premises:
        print("前提不满足，退出 3（判过了的都在上面，没判的见上）")
        return 3
    print("全过（4 个平台文件 / %d 行表 / 四个平台的徽章都在）" % len(rows or []))
    return 0


if __name__ == "__main__":
    sys.exit(main())
