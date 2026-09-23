#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 `check_doc_lines.py` 的每一种"引用被撞飞"的形状各造一遍，看门禁怎么判。

**为什么要有它**：那个门禁的判据全是"在某个前提上再加一层"。前提不成立时
它不报错，只是**安静地走了另一条分支** —— 而"走了另一条分支"和"判过了"在
输出里长得一模一样。所以每一种形状都要有一个自己造的、最小的样例，
并写明**期望它怎么判**；判据改了之后重跑本表，任何一格与期望不符就是回归。

**它自己也需要被证明有牙**（本仓规矩：判据搬了家要重新变异一遍）。下面这些
变异都在改动当时跑过，并写明**抓住了几格**（对照组一律 0 格不符）：

  M1 拆掉 `[胀]` 那条判据        -> S1/S4 退回 `[刷]` + 锁被改写，抓 2 格
  M2 让打印机漏印 `[新]` 条目     -> 报告自检 `新增 0/1` 判红，抓 8 格
  M3 撑长也不再拒写锁            -> S1/S4 报 `[胀]` 但 rc=0 且锁被改写，抓 2 格
  M4 `--force` 不盖章            -> 锁里少一条（当时 847 → 846），之后普通门禁永远红
  M5 `[歧]` 的**写门**短路（`if False:`）  -> S8 退回 rc=0 且锁被改写，抓 1 格
  M6 `[歧]` 分支无条件盖章（写门留着）     -> **抓 0 格**：这一句今天**没有判据**，
                                             写门挡在前面看不到它。如实记，别当它
                                             被验过 —— 它是"写门哪天被拿掉"时的
                                             第二道防线
  M7 门的**错误写法**：`'if False and ' + <整条 if 语句>`
                                         -> 工具 SyntaxError ⇒ 10 格**全部**退在
                                            "基准建锁"那一步。**10 格不是"抓住了"，
                                            是表自己坏了。** 这条是把坑留成记录：
                                            有没有牙不能只看"格数非 0"，得看抓住的
                                            是不是**期望的那一格**

场景（每个都在临时目录里现造，不碰真仓库）：

  S0  基准，什么都不改                      -> 不报，锁不变
  S1  区间**内部**插 2 行（末行唯一）        -> 期望：`[胀]` + 红，且**不写锁**
  S2  区间**上方**插 2 行                    -> 期望：平移（不红）
  S3  区间内部原地改 1 行（宽度不变）        -> 期望：重刷（不红）
  S4  区间内部**删** 1 行                    -> 期望：`[胀]` + 红，且**不写锁**
  S5  末行/倒数第二行都不可定位（都是 `}`）  -> 期望：重刷 + 如实报"判不了"
  S6  文档里新增一条引用                     -> 期望：`[新]` 打印出来（不红）
  S7  文档里删掉一条引用                     -> 期望：`[撤]` 打印出来（不红）
  S8  区间原文在下面另有**两份**拷贝，自己又被改了一行
                                             -> 期望：`[歧]` + 红，且**不写锁**
  S9  S8 加 `--force`                        -> 期望：`[歧]` + 不红，且**写锁**

**已知边界（别把它读成"全覆盖"）**：S1/S4 那条判据靠"尾部锚点在 ±400 行内
唯一"才咬得动。真仓库 860 条锁目实测（`check_doc_lines.py` 文件头也是这几个数；
重测 `tests/tools/doc_line_anchor_stats.py`）：两个尾部锚点里**至少一个**唯一的
778 条（90.5%）、一个都不唯一的 82 条（9.5%）。
那 9.5% 会走 S5 那一格：`[刷]` + 一行 `[记] 判不了 N 条`。**"判不了"必须自己
报数**，否则它和"判过了"在日志里长得一模一样。

用法：
    python tests/tools/check_doc_lines_scenes.py            # 跑本仓库那份门禁
    python tests/tools/check_doc_lines_scenes.py --tool <别的 check_doc_lines.py>
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TOOL = os.path.join(HERE, "check_doc_lines.py")

# 被引用区间 2-6 的末行 `} // end-of-widget` 与倒数第二行 `    step_gamma();`
# 在附近都是唯一的 —— 判据要靠它们定位"老末行挪到哪了"，所以基准里得给足。
BASE_SRC = """\
// prologue
void widget(void) {
    step_alpha();
    step_beta();
    step_gamma();
} // end-of-widget
// epilogue
"""

BASE_DOC = """\
# 测试文档

见 src/m/f.cpp:2-6。
"""

# S5 用：末行与倒数第二行都是 `}`，且附近还有成对的 `}` ⇒ 两头都定位不了。
BRACE_SRC = """\
// prologue
void alpha(void) {
    if (flag) {
        work_one();
    }
}
void beta(void) {
    if (flag) {
        work_two();
    }
}
// epilogue
"""


def write_tree(root, src, doc):
    os.makedirs(os.path.join(root, "src", "m"), exist_ok=True)
    os.makedirs(os.path.join(root, "doc"), exist_ok=True)
    # 锁文件写在 `<root>/tests/tools/` 下 —— 目录不在的话 `--update` 会在
    # 写锁那一步 FileNotFoundError（而不是报一条像样的红），场景表会整表失去意义。
    os.makedirs(os.path.join(root, "tests", "tools"), exist_ok=True)
    with open(os.path.join(root, "src", "m", "f.cpp"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(src)
    with open(os.path.join(root, "doc", "t.md"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(doc)


def run_tool(tool, root, args):
    p = subprocess.run([sys.executable, tool, "--root", root] + args,
                       capture_output=True, text=True, encoding="utf-8",
                       errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def lock_bytes(root):
    p = os.path.join(root, "tests", "tools", "doc_line_refs.lock")
    if not os.path.isfile(p):
        return None
    with open(p, "rb") as f:
        return f.read()


# --- 变异：都作用在 (src, doc) 两份文本上，返回新的两份 -------------------

def insert_inside(src, doc):
    """S1：在区间 2-6 的**内部**（老第 4 行之后）插 2 行。"""
    lines = src.splitlines(True)
    lines[4:4] = ["    step_beta_again();\n", "    step_beta_once_more();\n"]
    return "".join(lines), doc


def insert_above(src, doc):
    """S2：在区间**上方**插 2 行（老区间整体下移 2 行，宽度不变）。"""
    lines = src.splitlines(True)
    lines[0:0] = ["// inserted one\n", "// inserted two\n"]
    return "".join(lines), doc


def edit_inside(src, doc):
    """S3：区间内部原地改一行，行数不变。"""
    lines = src.splitlines(True)
    lines[3] = "    step_beta_v2();\n"
    return "".join(lines), doc


def delete_inside(src, doc):
    """S4：区间内部删掉一行。"""
    lines = src.splitlines(True)
    del lines[3]
    return "".join(lines), doc


def insert_inside_braces(src, doc):
    """S5：同上，但末行/倒数第二行都是 `}`，两头都没有唯一定位。"""
    lines = src.splitlines(True)
    lines[4:4] = ["        work_one_b();\n", "        work_one_c();\n"]
    return "".join(lines), doc


def add_cite(src, doc):
    """S6：文档里再加一条**新**引用。"""
    return src, doc + "\n另见 src/m/f.cpp:2-3。\n"


def drop_cite(src, doc):
    """S7：把原来那条引用从文档里删掉（另留一条，免得撞上"反空转"）。"""
    return src, "# 测试文档\n\n另见 src/m/f.cpp:2-3。\n"


def dup_two_copies(src, doc):
    """S8/S9：老区间**原地**改一行，同时把它的**原文**在下面放两份完全相同的拷贝。

    几何：区间 2-6 的原文（`widget` 那 5 行）现在出现在 8-12 与 13-17 两处，
    而 2-6 自己已经被改掉了 ⇒ `got != want`，`find_shift_candidates` 找到
    **两个**候选 ⇒ "内容改了"和"内容挪了"分不开 ⇒ `[歧]`。
    两格只在**有没有 `--force`** 上不同：不带要红且不写锁，带要按现在的内容盖章。
    """
    lines = src.splitlines(True)
    lines[3] = "    step_beta_v2();\n"            # 2-6 原地改一行
    block = ["void widget(void) {\n", "    step_alpha();\n",
             "    step_beta();\n", "    step_gamma();\n",
             "} // end-of-widget\n"]
    lines[7:7] = block + block                   # 原文两份拷贝，接在 `// epilogue` 后
    return "".join(lines), doc


SCENES = [
    # (名字, 基准 src, 基准 doc, 变异, 期望类别, 期望红?)
    ("S0 基准（什么都不改）", BASE_SRC, BASE_DOC, None, "—", False),
    ("S1 区间内部插 2 行（末行唯一）", BASE_SRC, BASE_DOC,
     insert_inside, "[胀]", True),
    ("S2 区间上方插 2 行", BASE_SRC, BASE_DOC, insert_above, "[移]", False),
    ("S3 区间内原地改 1 行", BASE_SRC, BASE_DOC, edit_inside, "[刷]", False),
    ("S4 区间内部删 1 行", BASE_SRC, BASE_DOC, delete_inside, "[胀]", True),
    ("S5 两头都定位不了（都是 `}`）", BRACE_SRC, BASE_DOC,
     insert_inside_braces, "[刷]+判不了", False),
    ("S6 文档新增一条引用", BASE_SRC, BASE_DOC, add_cite, "[新]", False),
    ("S7 文档删掉一条引用", BASE_SRC, BASE_DOC + "\n另见 src/m/f.cpp:2-3。\n",
     drop_cite, "[撤]", False),
    # 第 7 个元素是额外命令行参数；不带就是 `--update`。S8/S9 是同一格的两臂：
    # 没有 `--force` 要**红且不写锁**，有 `--force` 要**不红且写锁** —— 两臂
    # 结论不同才算证明了这个开关真的有作用（只跑一臂的话，"开关没用"也全绿）。
    ("S8 附近有两段同样内容（不带 --force）", BASE_SRC, BASE_DOC,
     dup_two_copies, "[歧]", True),
    ("S9 同上，带 --force", BASE_SRC, BASE_DOC, dup_two_copies,
     "[歧]", False, ["--force"]),
]

# 这串必须与工具**真正印出来**的标签逐字一致：原来这里是 `[歧义]`，而工具印的
# 是 `[歧]` —— 没有任何地方会输出 `[歧义]`，于是"期望歧义"这一格根本没法表达，
# 歧义场景挂掉了场景表也照样绿。（判"标签出现了"和判"判据过了"是两件事。）
MARKS = ("[移]", "[刷]", "[胀]", "[新]", "[撤]", "[歧]", "[撞]", "[记]")


def marks_in(out):
    return [m for m in MARKS if m in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", default=DEFAULT_TOOL)
    args = ap.parse_args()

    print("被测门禁: %s" % args.tool)
    print("%-32s %-4s %-10s %-12s %s"
          % ("场景", "rc", "报告里的类", "锁被改写?", "判定"))
    print("-" * 88)
    bad = 0
    for scene in SCENES:
        # 第 7 个元素（可选）是这次 `--update` 的额外参数，比如 `--force`。
        name, src, doc, mut, want, want_red = scene[:6]
        extra = list(scene[6]) if len(scene) > 6 else []
        tmp = tempfile.mkdtemp(prefix="docline_scene_")
        try:
            write_tree(tmp, src, doc)
            rc0, out0 = run_tool(args.tool, tmp, ["--update"])
            if rc0 != 0:
                print("%-32s 基准建锁就退了 %d\n%s" % (name, rc0, out0))
                bad += 1
                continue
            before = lock_bytes(tmp)
            if mut:
                s2, d2 = mut(src, doc)
                write_tree(tmp, s2, d2)
            rc, out = run_tool(args.tool, tmp, ["--update"] + extra)
            after = lock_bytes(tmp)
            got = "+".join(marks_in(out)) or "—"
            rewrote = "是" if after != before else "否"
            # 判定两条都要对上：①期望的红/不红 ②期望的类别（`want` 里按 `+` 分开的
            # 每一段都要出现在输出里；`—` 表示"一条类别都不许出现"）
            if want == "—":
                # `[记]` 是"这条判据没判"的如实记录，不是判红类别 —— 期望"什么都不报"
                # 的场景里它照样会出现（判据 4 在本仓一份引述都没有）。
                cls_ok = not [m for m in marks_in(out) if m != "[记]"]
            else:
                cls_ok = all(w in out for w in want.split("+"))
            ok = (rc != 0) == want_red and cls_ok
            if not ok:
                bad += 1
            print("%-32s %-4d %-10s %-12s %s"
                  % (name, rc, got, rewrote, "OK" if ok else "**不符（期望 %s/%s）**"
                     % (want, "红" if want_red else "不红")))
            if not ok:
                for ln in out.splitlines():
                    if any(m in ln for m in MARKS):
                        print("      | %s" % ln.strip())
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("-" * 88)
    print("与期望不符 %d 格" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
