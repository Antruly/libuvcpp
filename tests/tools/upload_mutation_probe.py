#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
给 `upload_mutation.py` 里"没抓住"的四条变异（U2/U3/U4/U5）做**探针**，
区分「等价变异」与「真实覆盖缺口」。

计划里的规矩（`assert-test-preconditions`）：抓不住的**不许直接写"等价"**，
必须先用探针把两种可能分开。读源码能给出一个*猜想*，但猜想不是证据 ——
这一轮四条里有两条（U4/U5）读起来"显然等价"，另两条（U2/U3）读起来"显然可达"，
而读源码分不出哪条猜想对。探针才能分开。

三个探针，各自要回答一个**是非题**，答案直接对应一种分类：

  A（**不变异**，跑全量）—— 两条"分支到底可不可达"
    A4  `pump()` 里 `if (j.failed)` 那一支被走到过吗？
        源码事实：四处 `j.failed = true`（:506 :519 :541 :557）后面**紧跟** `fail()`，
        而 `fail()` 无条件置 `discarding_ = true`；`pump()` 的 `discarding_` 分支
        （:352）排在 `if (j.failed)`（:371）**之前**。所以 `!discarding_ && j.failed`
        应当**恒假** —— 命中 0 次 ⇒ U4 是等价变异（不可达），不是覆盖缺口。
    A2  析构兜底的清理循环里，`kept && discarding_` 那个**变异会改变行为**的状态
        出现过吗？（`fail()` 已经把 `kept` 的 job 排回 `queue_`，所以那些 job 本该
        由 `pump()` 的丢弃分支删除；只有"排队还没轮到、会话就析构了"才会落到析构。）
        命中 0 次 ⇒ U2 是等价变异。

  B（**施加 U5 变异**，跑 `chunked_feed`）—— "是谁补偿的"
    在 `pump()` 那条同样的换槽路径上插桩。命中 >0 ⇒ 是 `pump()` 在补偿 ⇒
    U5 是等价变异（那份换槽是冗余的，不是死代码，也不是缺口）。

  C（**施加 U3 变异**，跑 `open_failure_keeps_dir_clean`）—— "行为到底变没变"
    `submit_close` 上插桩，打印 fd。U3 的变异让 open 失败时 `fd_valid` 也置真，
    于是丢弃分支会对一个**无效 fd** 发 close。命中"fd < 0"的行 ⇒ 行为**确实不同**
    ⇒ **不是等价变异**，是覆盖缺口（公开 API 上看不出后果，但守卫没被测到）。

用法：
  python -u tests/tools/upload_mutation_probe.py --probe A
  python -u tests/tools/upload_mutation_probe.py --probe B
  python -u tests/tools/upload_mutation_probe.py --probe C
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UPLOAD_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_upload.cpp")
T_UPLOAD = "test_web_app_upload_func"


# ---- A4：`pump()` 里 `if (j.failed)` 分支（与 U4 的靶点同一处） ----
A4_OLD = """    if (j.failed) {
      queue_.pop_front();
      continue;
    }"""
A4_NEW = """    if (j.failed) {
      std::fprintf(stderr, "[probe-A4] j.failed branch TAKEN\\n");
      queue_.pop_front();
      continue;
    }"""

# ---- A2：析构兜底（与 U2 的靶点同一处）----
A2_OLD = """    if (j.kept && !discarding_) continue;
    if (j.fd_valid && !j.closed) {"""
A2_NEW = """    if (j.kept && !discarding_) {
      std::fprintf(stderr,
                   "[probe-A2] SKIPS kept job i=%zu (discarding=%d)\\n",
                   i, (int)discarding_);
      continue;
    }
    // **只在这一行说明析构真的要动手**（关或删）—— 单纯的"走到了这里"不算数：
    // 每个没被跳过、也没活干的 job 都会流到下面。U2 的判据是"析构在
    // `kept && discarding_` 的状态下**真的删了文件**"，不是"看过一眼"。
    if (j.fd_valid && (!j.closed || !j.removed)) {
      std::fprintf(stderr,
                   "[probe-A2] ACTS i=%zu kept=%d discarding=%d closed=%d "
                   "removed=%d\\n",
                   i, (int)j.kept, (int)discarding_, (int)j.closed,
                   (int)j.removed);
    }
    if (j.fd_valid && !j.closed) {"""

# ---- B：U5 的变异 + `pump()` 换槽上的插桩 ----
B_MUT_OLD = """    if (j.pending.empty() && !j.incoming.empty()) {
      j.pending.swap(j.incoming);
      j.incoming.clear();
    }"""
B_MUT_NEW = "    /* MUTATION: 写完不在这里换槽 */"
B_PUMP_OLD = """    if (!j.incoming.empty()) {
      j.pending.swap(j.incoming);
      j.incoming.clear();
      submit_write(idx);
      return;
    }"""
B_PUMP_NEW = """    if (!j.incoming.empty()) {
      std::fprintf(stderr, "[probe-B] pump() swapped incoming\\n");
      j.pending.swap(j.incoming);
      j.incoming.clear();
      submit_write(idx);
      return;
    }"""

# ---- C：U3 的变异 + `submit_close` 上的插桩 ----
C_MUT_OLD = """  if (rc < 0) {
    j.failed = true;
    fail(std::string("打开上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
  } else {
    j.fd = static_cast<int>(rc);
    j.fd_valid = true;
  }"""
C_MUT_NEW = """  j.fd = static_cast<int>(rc);
  j.fd_valid = true; /* MUTATION: open 失败也算 fd 有效 */
  if (rc < 0) {
    j.failed = true;
    fail(std::string("打开上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
  }"""
C_CLOSE_OLD = """void uvcpp_web_upload::submit_close(size_t idx) {
  upload_job& j = jobs_[idx];
  busy_ = true;"""
C_CLOSE_NEW = """void uvcpp_web_upload::submit_close(size_t idx) {
  upload_job& j = jobs_[idx];
  std::fprintf(stderr, "[probe-C] submit_close idx=%zu fd=%d fd_valid=%d\\n",
               idx, j.fd, (int)j.fd_valid);
  busy_ = true;"""

PROBES = {
    "A": {
        "what": "不变异：A4 = `j.failed` 分支可达性；A2 = 析构兜底的 `kept && discarding_`",
        "mutations": [],
        "markers": [(A4_OLD, A4_NEW), (A2_OLD, A2_NEW)],
        "scoped": None,
        "tags": ["[probe-A4]", "[probe-A2]"],
    },
    "B": {
        "what": "施加 U5：`pump()` 是否补偿了被拆掉的换槽",
        "mutations": [(B_MUT_OLD, B_MUT_NEW)],
        "markers": [(B_PUMP_OLD, B_PUMP_NEW)],
        "scoped": "chunked_feed",
        "tags": ["[probe-B]"],
    },
    "C": {
        "what": "施加 U3：open 失败时是否真的会对无效 fd 发 close",
        "mutations": [(C_MUT_OLD, C_MUT_NEW)],
        "markers": [(C_CLOSE_OLD, C_CLOSE_NEW)],
        "scoped": "open_failure_keeps_dir_clean",
        "tags": ["[probe-C]"],
    },
}


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def run(cmd, cwd=ROOT):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def with_cstdio(text):
    """探针用了 `std::fprintf`，先确保 <cstdio> 在。

    插入点**不能写死**成某个头文件名 —— 那个头今天在、明天可能被换掉，而这个
    函数一旦"没找到锚点就原样返回"，探针会以缺 <cstdio> 编译失败，看起来像
    "探针无效"。所以插在**第一个 #include 行之前**，插不进去就直接报错停下。
    """
    if "#include <cstdio>" in text:
        return text
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            lines.insert(i, "#include <cstdio>")
            return "\n".join(lines)
    raise SystemExit("探针: 源码里一个 #include 都没有，拒绝猜测插入点")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    ap.add_argument("--probe", required=True, choices=sorted(PROBES))
    args = ap.parse_args()

    spec = PROBES[args.probe]
    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")
    exe = os.path.join(exe_dir, T_UPLOAD + ".exe")

    src = read_text(UPLOAD_CPP)
    before = md5(UPLOAD_CPP)

    print("=" * 72)
    print(f"探针 {args.probe} —— {spec['what']}")
    print("=" * 72)

    patched = with_cstdio(src)
    for old, new in spec["mutations"] + spec["markers"]:
        if old not in patched:
            print(f"  找不到待替换的片段（源码形状变了）:\n{old[:200]}")
            return 3
        patched = patched.replace(old, new, 1)

    write_text(UPLOAD_CPP, patched)
    try:
        rc, out = run(["cmake", "--build", args.tree, "--config", "Release",
                       "--parallel", "4"])
        nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
        if rc != 0 or nerr:
            print(f"  构建失败 rc={rc} errors={nerr}")
            print(out[-1500:])
            return 3
        run(["cmake", "--build", args.tree, "--config", "Release",
             "--target", "copy_test_dlls"])

        cmd = [exe] if spec["scoped"] is None else [exe, spec["scoped"]]
        rc_test, out = run(cmd, cwd=exe_dir)

        print(f"  运行 {spec['scoped'] or '(全量)'}：rc={rc_test}"
              f"（{'绿' if rc_test == 0 else '红'}）")
        for tag in spec["tags"]:
            lines = [l.strip() for l in out.splitlines() if tag in l]
            print(f"\n  {tag} 命中 {len(lines)} 次")
            for line in lines[:4]:
                print(f"    {line}")
            if len(lines) > 4:
                print(f"    …（共 {len(lines)} 条，只列前 4 条）")

        # A2 的判定**不能靠看前 4 条**：要回答的是"16 条里有没有一条
        # `kept=1 且 discarding=1`"，而那一条可能排在很后面。所以按**类别**
        # 出直方图 —— 结论直接读计数，不读样本。
        if args.probe == "A":
            a2 = [l.strip() for l in out.splitlines() if "[probe-A2]" in l]
            skips = [l for l in a2 if "SKIPS" in l]
            acts = [l for l in a2 if "ACTS" in l]
            print("\n  —— A2 分类直方图（判定看这里，不看上面那 4 条样本）——")
            print(f"    SKIPS （`j.kept` 那一半生效）        : {len(skips)}")
            print(f"    ACTS  （析构真的要关/删）            : {len(acts)}")
            print(f"      └ 其中 kept=1 且 discarding=1      : "
                  f"{len([l for l in acts if 'kept=1' in l and 'discarding=1' in l])}"
                  f"   ← **这一项才是 U2 的判据**")
            for l in acts[:6]:
                print(f"        {l}")
    finally:
        write_text(UPLOAD_CPP, src)

    ok = md5(UPLOAD_CPP) == before
    print(f"\n  源码按字节还原: {'是' if ok else '否 —— 有问题！'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
