#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Phase 3b 步骤 4（`upload_route()` 接线）的变异驱动。

判定规格沿用 `upload_mutation.py`（也就是 `stream_mutation.py` 的第二版）：
  1. 退出码非 0；
  2. **单独跑期望的那一组**也是红的（不看"全量里红了别的组"）。

第 2 条是上一版驱动踩坑改出来的：本仓用例 `main()` 在**第一个**失败的用例上就
`return 2`，所以全量跑**只会有一种红**。一个变异若同时打红了排在前面的一组，
全量输出里就看不到期望的那一组了 —— 判据会写成"没抓住"，而实际抓住了。

**每条变异带自己的 `target` 和 `expect`**（步骤 3 的 U9 教过：一个驱动可以横跨
多个用例文件）。本步有两组接线，各自钉在不同文件上：

  - `uvcpp_web_app.cpp` 的 `wire_upload()` / 完成回调 / 拒绝路径
    → 钉在 `test_web_app_upload_route_func`（本步新增的端到端用例）上；
  - `uvcpp_web_stream.cpp` 的 `deliver_end()` / `release_end()`
    → 其中"没有框架钩子时也必须清扣"这一支**只有非上传的流式路由能打到**，
      所以那条变异的 target 是 `test_web_app_stream_func`。

用法：
  python -u tests/tools/upload_route_mutation.py [--tree build-webapp] [--only V1]
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")
STREAM_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_stream.cpp")

# 用例目标名（exe 名 = 目标名 + ".exe"）。
T_ROUTE = "test_web_app_upload_route_func"
T_STREAM = "test_web_app_stream_func"

# (名字, 文件, 被替换的原文, 变异后的文本, 用例目标, 期望抓住它的用例, 说明)
MUTATIONS = [
    (
        "V1",
        STREAM_CPP,
        """  end_deferred_ = true;
  if (!hooks_.on_end || !hooks_.on_end()) end_deferred_ = false;""",
        """  /* MUTATION: 先跑钩子、后扣住 —— 同步收口的会话放了个空 */
  if (!hooks_.on_end || !hooks_.on_end()) end_deferred_ = false;
  end_deferred_ = true;""",
        T_ROUTE,
        "fields_only_no_files",
        "`end_deferred_` 的置位挪到钩子之后（纯字段表单的 on_end 永不触发）",
    ),
    (
        "V2",
        STREAM_CPP,
        """  if (!hooks_.on_end || !hooks_.on_end()) end_deferred_ = false;""",
        """  if (hooks_.on_end && !hooks_.on_end()) end_deferred_ = false;
  /* MUTATION: 没有框架槽时恒扣住 */""",
        T_STREAM,
        "normal_route_unaffected",
        "没有框架钩子的流式路由也恒被扣住（`||` 写成 `&&`）",
    ),
    (
        "V3",
        STREAM_CPP,
        """  if (end_deferred_ || end_released_) return;""",
        """  if (end_deferred_) return; /* MUTATION: 幂等守卫拆掉 */""",
        T_ROUTE,
        "fields_only_no_files",
        "`end_released_` 幂等守卫拆掉（同步收口那条路会把 on_end 跑两次）",
    ),
    (
        "V4",
        APP_CPP,
        """    uvcpp_web_stream* s = c.stream();
    if (s != nullptr) s->release_end();""",
        """    uvcpp_web_stream* s = c.stream();
    (void)s; /* MUTATION: 拿到结果也不放行被扣住的 on_end */""",
        T_ROUTE,
        "single_file",
        "落盘完成回调不放行用户的 on_end（结果填好了但用户读不到）",
    ),
    (
        "V5",
        APP_CPP,
        """  hooks.on_abort = [up]() {
    // 对端断了 / 框架掐断 → 会话按失败收场，**本次创建的临时文件全部删掉**。
    // 这是"失败时框架删"那个决策的落地处。
    up->notify_parse_done(false);
  };""",
        """  hooks.on_abort = [up]() {
    (void)up; /* MUTATION: 断开不通知会话 → 本次创建的临时文件不删 */
  };""",
        T_ROUTE,
        "disconnect_deletes_temp",
        "连接中断不通知会话（「失败时框架删」那条决策失效，半截文件留在盘上）",
    ),
    (
        "V6",
        APP_CPP,
        """  r.set_header("connection", "close");""",
        """  (void)0; /* MUTATION: 拒绝时不要关闭连接 */""",
        T_ROUTE,
        "reject_415",
        "拒绝路径不要求关闭连接（残留 body 会污染下一个请求）",
    ),
    (
        "V7",
        APP_CPP,
        """      reject_upload(ctx, 415,""",
        """      reject_upload(ctx, 400, /* MUTATION: 415 说成 400 */""",
        T_ROUTE,
        "reject_415",
        "非 multipart 回 400 而不是 415（「换个格式」与「修你的报文」混成一个码）",
    ),
]

# 跑挂的变异会永久挂住驱动，而驱动是"跑完才还原源码"的 —— 外部杀掉它时 src/
# 会留在变异状态。统一超时，按 124 上报（与 GNU timeout 同值，且仍是非零退出
# 码，不会被误读成"通过"）。
RUN_TIMEOUT_S = 600


def read_text(p):
    with open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(p, s):
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def run(cmd, cwd=ROOT):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace",
                           timeout=RUN_TIMEOUT_S)
    except subprocess.TimeoutExpired as e:
        out = b"".join(x for x in (e.stdout, e.stderr) if x)
        if isinstance(out, str):
            out = out.encode("utf-8", "replace")
        return 124, out.decode("utf-8", "replace") + \
            "\n*** TIMEOUT after %ds: %s ***\n" % (RUN_TIMEOUT_S, " ".join(cmd))
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree):
    rc, out = run(["cmake", "--build", tree, "--config", "Release", "--parallel", "4"])
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    return rc, nerr, out


def failed_cases(out):
    """从测试输出里抠出红了的用例名（跟着 `[<tag>] <name>` 走）。"""
    red = []
    cur = None
    for line in out.splitlines():
        m = re.match(r"\[[a-z_]+\] (\S+)$", line)
        if m:
            cur = m.group(1)
            continue
        if line.strip() == "-> FAIL" and cur:
            red.append(cur)
    return red


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")

    def exe_of(target):
        return os.path.join(exe_dir, target + ".exe")

    files = sorted({m[1] for m in MUTATIONS})
    backups = {p: read_text(p) for p in files}
    before = {p: md5(p) for p in files}
    targets = sorted({m[4] for m in MUTATIONS})

    print("=" * 72)
    print("基线")
    print("=" * 72)
    rc, nerr, out = build(args.tree)
    print(f"  build rc={rc} errors={nerr}", flush=True)
    if rc != 0 or nerr:
        print(out[-3000:])
        return 3
    rc, out = run(["cmake", "--build", args.tree, "--config", "Release",
                   "--target", "copy_test_dlls"])
    if rc != 0:
        print("  copy_test_dlls 失败")
        return 3

    for t in targets:
        rc, out = run([exe_of(t)], cwd=exe_dir)
        print(f"  基线 {t}: {'ALL PASS' if rc == 0 else 'FAIL'} (rc={rc})",
              flush=True)
        if rc != 0:
            print(out[-3000:])
            print("  基线不绿，后面的判定没有意义 —— 停下")
            return 3

    results = []
    for name, path, old, new, target, expect, desc in MUTATIONS:
        if args.only and name != args.only:
            continue

        src = read_text(path)
        if old not in src:
            results.append((name, "施加失败", target, expect, desc))
            print(f"\n[{name}] 施加失败 —— 源码里找不到待替换的片段", flush=True)
            continue

        write_text(path, src.replace(old, new, 1))
        print(f"\n[{name}] {desc}", flush=True)

        rc, nerr, out = build(args.tree)
        if rc != 0 or nerr:
            write_text(path, src)
            results.append((name, "构建失败", target, expect, desc))
            print(f"  构建失败（rc={rc} errors={nerr}）—— 不算抓住", flush=True)
            print(out[-1200:])
            continue

        run(["cmake", "--build", args.tree, "--config", "Release",
             "--target", "copy_test_dlls"])

        # 判据：**单独跑期望的那一组**。全量输出只用来记附带损伤。
        rc_scoped, out_scoped = run([exe_of(target), expect], cwd=exe_dir)
        rc_full, out_full = run([exe_of(target)], cwd=exe_dir)
        red_full = failed_cases(out_full)

        write_text(path, src)  # 立刻还原

        caught = rc_scoped != 0
        verdict = "抓住" if caught else "没抓住"
        results.append((name, verdict, target, expect, desc))
        extra = "" if red_full[:1] == [expect] else f"  全量首红={red_full[:1] or '(无)'}"
        print(f"  单独跑 {expect}: rc={rc_scoped} → {verdict}；"
              f"全量 rc={rc_full}{extra}", flush=True)

    # ---- 还原后重建：这一步不做完，这棵树就是脏的 ----
    print("\n" + "=" * 72)
    print("还原")
    print("=" * 72)
    for p, text in backups.items():
        write_text(p, text)
    ok = all(md5(p) == before[p] for p in files)
    print(f"  源码按字节还原: {'是' if ok else '否 —— 有问题！'}")
    rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")], cwd=ROOT)
    print(f"  grep MUTATION 残留: {out.strip() or '(无)'}")

    rc, nerr, out = build(args.tree)
    run(["cmake", "--build", args.tree, "--config", "Release",
         "--target", "copy_test_dlls"])
    reruns = []
    for t in targets:
        rc2, _ = run([exe_of(t)], cwd=exe_dir)
        reruns.append(rc2)
    print(f"  还原后重建 rc={rc} errors={nerr}，复跑 " +
          "、".join(f"{t}={'ALL PASS' if r == 0 else 'FAIL'}"
                    for t, r in zip(targets, reruns)))

    print("\n" + "=" * 72)
    print("汇总")
    print("=" * 72)
    for name, verdict, target, expect, desc in results:
        print(f"  {name:4} {verdict:8} {expect:26} @{target}  {desc}")
    return 0 if ok and not any(reruns) else 1


if __name__ == "__main__":
    sys.exit(main())
