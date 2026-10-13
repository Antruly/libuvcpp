#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
静态腿的**每请求 syscall / 分配预算**门禁（Linux，LD_PRELOAD 量具）。

判的不是"响应对不对"（那是 `test_web_app_static_func` 别的用例的事），而是
**命中一个已缓存文件时，热路径每请求花了几次系统调用、投了几次线程池**。
这几件事只有数系统调用才看得见：响应字节逐字相同，没到饱和时 rps 也看不出
差别，而它们正是 `1.6.25-dev` 起那一批静态优化（C1–C5）的全部内容 ——
没有一个能自动判红的量具，那些优化下一轮就会被无声地退回去。

## 为什么是这套形状

`strace` 在这台机器上**用不了**（`ptrace_scope` 关着），也更不该进 CI。所以量具
是 `tests/tools/fs_count_shim.c` —— 一个 `LD_PRELOAD` 的计数器（它自己解释了
三个"只看符号名会数到零"的坑：libuv 的 `statx` 走 `syscall()`、glibc 的
`realpath` 用内部别名、futex 是内联指令而池提交走 `pthread_cond_signal`）。

量具必须**被预载**才有读数，而 `ld.so` 的预载只能由**父进程**在 `exec` 前设好，
所以"跑用例"这件事本身必须发生在脚本里：用例在没预载时只打印一行说明、**不判**
（本仓的规矩：没量过 ≠ 量过了），真正判的是这里。

## 判据（都按**每请求**算，分母是窗口里真的回来的 200 条数）

| 量 | 界 | 为什么是这个界 |
|---|---|---|
| `realpath` | `== 0` | 命中路径不该再解析路径（那是沿路径逐分量 `lstat`） |
| `lstat` / `newfstatat` | `== 0` | 同上；libuv 在 Linux 上走 `statx`，另两个出现就是有人绕了路 |
| `statx` | `1 ≤ x ≤ 1 + 4/req` | **恰好一次**：少一次是"没校验"（缓存会把旧字节一直发下去），多一次是白做 |
| `openat + open` | `≤ 4/req` | 命中时字节在内存里，不许再读盘 |
| `cond_signal` | `POOL`：`1 ≤ x ≤ 1 + 4/req`；`LOOP_STAT`：`== 0` | 这一格就是"每请求投了几次池"，也是两档唯一的差别 |
| `malloc` | `≤ 80` | **只当烟感**：这是跨编译器/标准库会漂的数，留了 ~75% 余量，抓的是"整类分配回来了"，不是抠 3 次 |
| `req` | `== 100` | 防空转：分母要是缩了，上面每一格都会跟着变好看 |

余量（`4/req`）是留给窗口边上那些不在请求路径上的零星调用的（app 的定时器、
预热后还在飞的池任务），不是给热路径用的。

## 反空转：这些变异必须让它红（三条都在本机实测过，读数如下）

  1. **让 `LOOP_STAT` 的命中路径也投池**（把 `serve()` 里那句 `if (im->loop_revalidate())`
     短路掉）⇒ `LOOP_STAT` 的 `cond_signal` 变成 **1.000**（界是 `== 0`）；
  2. **命中路径改回 `realpath`**（在 `probe_job_inner()` 开头加一次 `web_real_path()`）
     ⇒ 两档的 `realpath` 都变成 **1.000**（界是 `== 0`）；
  3. **命中路径不做指纹校验**（快路径直接返回 `OK`、不 `stat`）⇒ 两档的 `statx`
     变成 **0.000**（界是 `≥ 1`）—— 这一条正是"缓存永远不回源"。

后两条顺带证明了这个计数器**真的看得到库自己的调用**：变异 2 那一格如果恒为 0，
"命中路径不该 realpath"就是一句空话。

`--self-test` 是本门禁自己的负对照：**不预载**量具跑一遍，必须得到"这条门禁判红"
（用例那时只打印 `[note] 未预载`，脚本必须认得出这是红而不是绿）。

用法：
    python -u tests/tools/run_static_fs_gate.py --tree build-all
    python -u tests/tools/run_static_fs_gate.py --tree build-all --self-test

退出码：0 全过；1 有量测项越界（**这才是门禁该拦的**）；3 没跑成
（非 Linux / 那棵树里没有 webapp 的那个 exe / 量具编不过 —— 都是"没判"，
不是"通过"）。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.stdout.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))  # tests/tools -> tests -> repo
SHIM_SRC = os.path.join(HERE, "fs_count_shim.c")
EXE_REL = os.path.join("tests", "functional", "test_web_app_static_func")

# 用例打印的那一行：`FSCNT leg=webapp mode=POOL req=100 realpath=0.000 ...`
FSCNT_RE = re.compile(r"FSCNT\s+leg=(\S+)\s+mode=(\S+)\s+req=(\d+)\s+(.*)$")
NOTE_NO_SHIM = "未预载"

# 每一格：名字、界（闭区间）、说明。
BOUNDS = [
    ("realpath", 0.0, 0.0, "命中路径上一次都不该有"),
    ("lstat", 0.0, 0.0, "命中路径上一次都不该有"),
    ("newfstatat", 0.0, 0.0, "命中路径上一次都不该有（libuv 走 statx）"),
    ("openat", 0.0, None, "与 open 合起来判：命中不许读盘"),
    ("open", 0.0, None, "与 openat 合起来判"),
    ("close", 0.0, None, "只打印（窗口边上还有连接关闭）"),
    ("malloc", 0.0, 80.0, "烟感：抓「整类分配回来了」，不抠个位数"),
]


def run(cmd, cwd=None, env=None, timeout=None):
    """跑一条命令，返回 `(rc, 合并后的输出)`。"""
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def build_shim(cc, work):
    """编量具。返回 `.so` 路径；编不过返回 `(None, 报错)`。"""
    so = os.path.join(work, "fs_count_shim.so")
    rc, out = run([cc, "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                   "-o", so, SHIM_SRC, "-ldl"], cwd=ROOT)
    if rc != 0 or not os.path.exists(so):
        return None, out
    return so, out


def parse_fscnt(text):
    """把输出里所有 `FSCNT` 行解成 `{mode: (req, {量: 值})}`。"""
    seen = {}
    for line in text.splitlines():
        m = FSCNT_RE.search(line)
        if not m:
            continue
        leg, mode, req, rest = m.group(1), m.group(2), int(m.group(3)), m.group(4)
        vals = {}
        for tok in rest.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    vals[k] = float(v)
                except ValueError:
                    pass
        seen[(leg, mode)] = (req, vals)
    return seen


def check_mode(mode, req, vals, fails):
    """按上表判一档，越界就往 `fails` 里追加一条人话。"""
    def bad(msg):
        fails.append("%s：%s" % (mode, msg))

    if req != 100:
        bad("窗口里的请求数是 %d，不是 100 —— 分母变了，下面每一格都不可比" % req)
    slack = 4.0 / float(req) if req > 0 else 0.0

    for name, lo, hi, why in BOUNDS:
        if name not in vals:
            bad("读数里没有 `%s` 这一格（量具与用例对不上？）" % name)
            continue
        x = vals[name]
        if name == "openat":
            continue                       # 与 open 合起来判，见下
        if name == "open":
            tot = x + vals.get("openat", 0.0)
            if tot > slack:
                bad("命中路径不该再读盘：openat+open = %.3f > %.3f（%s）"
                    % (tot, slack, "命中时字节在内存里"))
            continue
        if hi is None:
            continue                       # 只打印
        hi_eff = hi + slack if hi > 0 else hi
        if x < lo - 1e-9 or x > hi_eff + 1e-9:
            bad("`%s` = %.3f，界 [%.1f, %.3f]（%s）" % (name, x, lo, hi_eff, why))

    # 指纹校验那一次：给 4 的余量，见文件头。
    if "statx" in vals:
        x = vals["statx"]
        if x < 1.0 - 1e-9 or x > 1.0 + slack + 1e-9:
            bad("`statx` = %.3f，界 [1, %.3f] —— 少一次是没校验（缓存会把旧字节"
                "一直发下去），多一次是白做" % (x, 1.0 + slack))

    # 池：这一格是 POOL / LOOP_STAT 两档唯一的差别。
    if "cond_signal" in vals:
        x = vals["cond_signal"]
        if mode == "POOL":
            if x < 1.0 - 1e-9 or x > 1.0 + slack + 1e-9:
                bad("`cond_signal` = %.3f，界 [1, %.3f] —— 每请求恰好一次池任务"
                    % (x, 1.0 + slack))
        else:
            if x > 1e-9:
                bad("`cond_signal` = %.3f，必须是 0 —— 这一档的命中路径不走池"
                    % x)


def print_table(seen):
    """印一张人看的表（顺序固定，方便与上一条提交对照）。"""
    cols = ["realpath", "lstat", "newfstatat", "statx", "openat", "open",
            "close", "cond_signal", "malloc"]
    print("---- 每请求读数（分母 = 窗口里真的回来的 200 条数）----")
    print("  %-10s %5s  %s" % ("mode", "req", "  ".join("%10s" % c for c in cols)))
    for (leg, mode) in sorted(seen):
        req, vals = seen[(leg, mode)]
        print("  %-10s %5d  %s" % (mode, req,
                                   "  ".join("%10.3f" % vals.get(c, float("nan"))
                                             for c in cols)))


def main():
    ap = argparse.ArgumentParser(
        description="静态腿每请求 syscall / 分配预算门禁（Linux，LD_PRELOAD 量具）")
    ap.add_argument("--tree", required=True, help="构建树（要有 webapp 的那个）")
    ap.add_argument("--cc", default="cc", help="编量具用的 C 编译器（默认 cc）")
    ap.add_argument("--work", default=None,
                    help="放量具与输出的目录（默认建一个临时目录）")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="跑用例的墙钟上限（秒，默认 300）")
    ap.add_argument("--self-test", action="store_true",
                    help="负对照：不预载量具跑一遍，必须判红")
    args = ap.parse_args()

    if not sys.platform.startswith("linux"):
        print("本门禁只在 Linux 上判（LD_PRELOAD 计数器的形状）—— 未判。")
        return 3

    tree = os.path.join(ROOT, args.tree)
    exe = os.path.join(tree, EXE_REL)
    if not os.path.exists(exe):
        print("找不到 %s —— 这棵树里没有 webapp 的那个用例（UVCPP_BUILD_WEBAPP=0？）"
              "，未判。" % exe)
        return 3

    work = args.work or tempfile.mkdtemp(prefix="fs_gate_")
    if not os.path.isdir(work):
        os.makedirs(work)

    so, log = build_shim(args.cc, work)
    if so is None:
        print("量具编不过（%s）—— 门禁自身没跑成，未判。\n%s" % (SHIM_SRC, log))
        return 3

    out_file = os.path.join(work, "fscount.out")
    if os.path.exists(out_file):
        os.remove(out_file)

    env = dict(os.environ)
    env["UVCPP_STATIC_TEST_ONLY"] = "fs_count"
    env["FSCOUNT_OUT"] = out_file
    if not args.self_test:
        env["LD_PRELOAD"] = so + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD")
                                  else "")
    else:
        env.pop("LD_PRELOAD", None)

    # **工作目录**：用例里的静态根是相对路径（`uvcpp_static_test_root`），
    # ctest 也是在这个目录下跑的（`add_test` 的默认工作目录 = 那个
    # `CMakeLists.txt` 的二进制目录）。从仓根跑会找不到文件。
    cwd = os.path.dirname(exe)
    try:
        rc, text = run([exe], cwd=cwd, env=env, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print("用例 %s 超过 %.0f 秒没结束 —— 未判。" % (exe, args.timeout))
        return 3

    seen = parse_fscnt(text)

    if args.self_test:
        # 负对照：没预载时必须**判红**，而且红得有理有据（要么用例自己说了
        # "未预载"，要么脚本数不到任何一格）。
        red = (NOTE_NO_SHIM in text) or (not seen)
        print("---- --self-test（不预载量具，必须判红）----")
        print("  用例退出码 %d；读到 %d 档读数；出现「%s」=%s"
              % (rc, len(seen), NOTE_NO_SHIM, NOTE_NO_SHIM in text))
        if red and not args.work:
            shutil.rmtree(work, ignore_errors=True)
        if not red:
            print("==== 汇总 ====\n没预载量具反而「全过」 —— 这条门禁是空转的。")
            return 1
        print("==== 汇总 ====\n负对照成立：没预载时这一条判红（本门禁不是空转）。")
        return 0

    print("量具：%s\n用例：%s\n输出：%s\n" % (so, exe, out_file))

    fails = []
    if rc != 0:
        fails.append("用例退出码 %d（它自己那些 200 / 顺序断言先红了）" % rc)
    if not seen:
        if NOTE_NO_SHIM in text:
            fails.append("用例说「%s」—— 预载没生效（LD_PRELOAD=%r）"
                         % (NOTE_NO_SHIM, env.get("LD_PRELOAD")))
        else:
            fails.append("输出里一条 `FSCNT` 都没有 —— 用例没跑到那条，或者"
                         "它崩在了打印之前")
    for mode in ("POOL", "LOOP_STAT"):
        if not any(k[1] == mode for k in seen):
            fails.append("少了 `%s` 那一档的读数（两档成对才有意义）" % mode)

    if seen:
        print_table(seen)
        print("")
        for (leg, mode) in sorted(seen):
            req, vals = seen[(leg, mode)]
            check_mode(mode, req, vals, fails)

    # 两档的对照：同一份负载、同一个进程，差别只该在"投不投池"上。
    pool = seen.get(("webapp", "POOL"))
    loop = seen.get(("webapp", "LOOP_STAT"))
    if pool and loop:
        for name in ("realpath", "lstat", "statx", "openat", "open"):
            a, b = pool[1].get(name), loop[1].get(name)
            if a is None or b is None:
                continue
            if abs(a - b) > 4.0 / max(pool[0], 1):
                fails.append("两档的 `%s` 不该差这么多：POOL=%.3f、LOOP_STAT=%.3f "
                             "—— 差的应该只是投不投池" % (name, a, b))

    print("==== 汇总 ====")
    if fails:
        print("红 %d 条：" % len(fails))
        for f in fails:
            print("  - %s" % f)
        return 1
    print("全过（每档 %d 条请求的每请求读数都在界内）"
          % (pool[0] if pool else 0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
