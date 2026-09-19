#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第 1 条（`run()` 在没事可做时不返回）收口的变异验证。

被测的行为：**框架层客户端的 `run(UV_RUN_DEFAULT)` 在"没有任何事在等"时要自己
返回**。用例 `test_run_default_returns_when_idle` 两条腿分别量两个方向：

  腿 1  不开重连，对端走掉      → 循环上再没有活句柄
  腿 2  开重连、次数用尽        → 连重连定时器也没了

两条都**不调 `stop()`** —— 判据就在这里：靠 `stop()` 收场的话改前改后都是绿的。

改法是把 `uvcpp_ws_sessions` 那个延迟回收用的 async 句柄从"一直 referenced"
改成**按需保活**：退休表空着时 unref、有东西要删时 ref。于是不变式是

    **这个句柄 ref ⟺ 退休表 `retired_` 非空**

三个动作各钉一半，所以打四个变异（M4 是另一半机制）：

  M1  `set_loop()` 里建完就 unref 那句去掉
  M2  `drain()` 尾部"删空了就把 ref 还回去"那句去掉
  M3  `on_retired()` 里 send 之前的 `ref()` 去掉
  M4  `uvcpp_web_ws_client::run()` 的泵把 `rc == 0` 这个出口拿掉

M1 **预期抓不住**（见下面"等价判定"），其余三个预期各红一处。不返回时 `run()`
是**永远**不出来（驱动线程停在 `uv_run` 里），所以用例自带 4/8 秒有界等待并
`detach()` 收场 —— 变异的表现应当是 **FAIL，不是挂死**。

M1 的等价判定（不许直接写"等价"，要有证据）
------------------------------------------
`set_loop()` 建出来的句柄只可能落在"刚建好、还没人退休"这个窗口里，而
`set_loop()` 的两处调用点都**先有一个会话**才会走到：

  - `uvcpp_ws_client.cpp:355` 在**握手完成**回调里调，紧跟着就 `adopt()` 一个
    会话 —— 会话活着就意味着它的 TCP 句柄是 active 的，循环本来就活着；
  - `uvcpp_ws_server.cpp:221` 在监听循环上建，那个循环由监听句柄保活。

会话一旦终结就会走 `on_retired()` → `ref()` + `send()`，随后的 `drain()`
在表空时又会 `unref()`。所以那条被去掉的语句在**可达路径**上不改变任何可观测
结果。本脚本对 M1 额外跑**整棵树的 ctest**：全绿才算这条判定站得住。

用法：python -u tests/tools/run_idle_mutation.py [--tree build-webapp]
      （会改源码再还原；跑完核对"源码按字节还原: 是"）
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SESSIONS_CPP = os.path.join(ROOT, "src", "web", "uvcpp_ws_sessions.cpp")
WEBCLI_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_ws_client.cpp")

CASE = "run_default_returns_when_idle"

MUTATIONS = [
    # label, 文件, 原文, 替换
    ("M1 set_loop 的起始 unref",
     SESSIONS_CPP, "  a->unref();\n", "  /* MUTATION: 不 unref */\n", False),
    ("M2 drain 尾部的 unref",
     SESSIONS_CPP,
     "  if (retired_.empty() && drain_async_ != nullptr) drain_async_->unref();",
     "  /* MUTATION: 不还 ref */", True),
    ("M3 on_retired 的 ref",
     SESSIONS_CPP, "    drain_async_->ref();\n", "    /* MUTATION: 不 ref */\n",
     True),
    ("M4 泵少了 rc==0 出口",
     WEBCLI_CPP,
     "    if (!restart_pending_ && rc == 0) break;",
     "    if (!restart_pending_ && rc == 0 && false) break; /* MUTATION */",
     True),
]

# 每个变异跑这三个：第一个是判据本身，后两个是这次改动的**回归哨兵**
# （只 unref 不 ref 的那一版就是它们红的）。
TARGETS = [
    ("test_web_app_ws_client_func.exe", ["--only", CASE], 180),
    ("test_web_ws_ownership_func.exe", [], 300),
    ("test_web_ws_client_api_func.exe", [], 300),
]

RUN_TIMEOUT_S = 1800


def read_text(p):
    with open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(p, s):
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def run(cmd, cwd=ROOT, timeout=RUN_TIMEOUT_S):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "*** TIMEOUT ***"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree):
    rc, out = run(["cmake", "--build", tree, "--config", "Release",
                   "--parallel", "4"])
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    return rc, nerr, out


def sync_dll(tree):
    """把测试侧那份 uvcpp.dll 同步成 `Release/` 里刚编出来的。

    `copy_test_dlls` 是 `add_custom_target(... ALL)`，只有**默认构建**会跑它；
    本脚本走的是 `--target <单个测试>`，那条依赖链里没有它，于是测试侧的 DLL
    会停在上一版。不手动同步就会拿旧库跑变异，结果整体错位一格（这条踩过）。

    注意日志里那句 `'pwsh.exe' 不是内部或外部命令` **与本步骤无关** —— 那是
    CMake 自己的编译器探测留下的，全仓构建系统里没有任何一处引用 pwsh。
    """
    src = os.path.join(tree, "Release", "uvcpp.dll")
    n = 0
    with open(src, "rb") as f:
        data = f.read()
    for base, _dirs, files in os.walk(tree):
        if "uvcpp.dll" not in files:
            continue
        dst = os.path.join(base, "uvcpp.dll")
        if os.path.abspath(dst) == os.path.abspath(src):
            continue
        with open(dst, "wb") as f:
            f.write(data)
        n += 1
    return n


def run_targets(exe_dir):
    """返回 [(用例名, rc, FAIL 行), …]。"""
    out_rows = []
    for exe, argv, tmo in TARGETS:
        path = os.path.join(exe_dir, exe)
        rc, out = run([path] + argv, cwd=exe_dir, timeout=tmo)
        fails = [s.strip() for s in re.findall(r"\[[Ff][Aa][Ii][Ll]\](.*)", out)]
        out_rows.append((exe, rc, fails))
    return out_rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")

    files = (SESSIONS_CPP, WEBCLI_CPP)
    before = {p: read_text(p) for p in files}
    sums = {p: md5(p) for p in before}
    summary = []
    restored_ok = False
    tail_rc = 1

    try:
        cases = [("基线（未变异）", None)] + [
            (m[0], m[1:]) for m in MUTATIONS]
        for label, mut in cases:
            for p, text in before.items():
                write_text(p, text)
            if mut is not None:
                path, old, new = mut[0], mut[1], mut[2]
                cur = read_text(path)
                assert cur.count(old) >= 1, f"{label}: anchor not found {old!r}"
                write_text(path, cur.replace(old, new, 1))

            rc, nerr, out = build(args.tree)
            if rc != 0 or nerr:
                print(f"[{label}] 构建失败 rc={rc} errors={nerr}")
                print(out[-3000:])
                raise SystemExit(3)
            n = sync_dll(tree)

            rows = run_targets(exe_dir)
            red = [(e, r) for e, r, f in rows if r != 0 and f]
            verdict = "**没抓住**" if not red else \
                      "抓（" + "、".join(f"{e.split('.')[0][5:]} rc={r}" for e, r in red) + "）"
            summary.append((label, verdict))
            print(f"\n[{label}] 同步 dll {n} 处 → {verdict}", flush=True)
            for exe, r, fails in rows:
                print(f"    {exe.split('.')[0]:34s} rc={r}")
                for f in fails:
                    print(f"        {f}")

            # M1 额外跑整棵树：它预期抓不住，得用"全套仍绿"来支撑等价判定
            if label.startswith("M1"):
                crc, cout = run(["ctest", "-C", "Release"], cwd=tree, timeout=1800)
                tot = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", cout)
                print(f"    [M1 整棵树 ctest] rc={crc} "
                      f"{tot.group(0) if tot else cout[-200:]}", flush=True)
                summary[-1] = (label, verdict + ("；整棵树 " + (tot.group(0) if tot else "?")))
    finally:
        for p, text in before.items():
            write_text(p, text)
        restored_ok = all(md5(p) == sums[p] for p in files)
        print(f"\n源码按字节还原: {'是' if restored_ok else '否 —— 有问题！'}")
        rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")])
        print(f"grep 残留: {out.strip() or '(无)'}")
        rc, nerr, _ = build(args.tree)
        sync_dll(tree)
        rows = run_targets(exe_dir)
        tail_rc = 0 if (rc == 0 and nerr == 0 and all(r == 0 for _e, r, _f in rows)) else 1
        print(f"还原后重建 rc={rc} errors={nerr}；"
              f"三个 exe 复跑 rc={[r for _e, r, _f in rows]}（应全 0）")

    print("\n==== 汇总 ====")
    for label, verdict in summary:
        print(f"  {label:26s} {verdict}")
    return 0 if (restored_ok and tail_rc == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
