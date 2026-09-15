#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
V5 的探针 —— 用来区分「等价变异」和「真实覆盖缺口」。

背景：`upload_route_mutation.py` 的 V5 把 `hooks.on_abort` 换成空实现（连接中断
时**不通知上传会话**），而 `disconnect_deletes_temp` 照样绿。用例断言的是
"断开之后上传目录变空"，所以只有两种可能：

  (a) 文件真的没被删，用例的判据有洞 —— 真实覆盖缺口；
  (b) 文件被**另一条路**删了 —— `uvcpp_web_upload` 的析构兜底。

(b) 是存在的：`~uvcpp_web_upload()` 对每个 `fd_valid && !removed` 的 job 做同步
`unlink`。连接断开 → 上下文收场 → 流对象（连着钩子里的 `shared_ptr`）析构 →
会话析构 → 兜底删除。**但这是推理，不是证据** —— `assert-test-preconditions`
的规矩是先用探针把两条路分开，不许直接写"等价"。

探针打在析构的 unlink 那一句上：数它一共删了几个。两条路各跑一次：

  基线（钩子在）：析构兜底删 **0** 个（`fail()` 已经删过了）
  V5  （钩子空）：析构兜底删 **≥1** 个（只有它兜得住）

前者为 0、后者非 0，就说明"目录变空"这个可观测结果在两种实现下由**不同的机制**
达成 —— 即 V5 对这个判据是等价的，而不是用例漏了。

用法：python -u tests/tools/v5_probe.py [--tree build-webapp]
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
APP_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")

UNLINK_LINE = "      fs_->unlink(loop_, j.meta.path_.c_str());"
PROBED_LINE = (
    '      fprintf(stderr, "[probe] ~uvcpp_web_upload unlink job=%zu\\n", i);\n'
    "      /* PROBE */\n"
    + UNLINK_LINE
)

V5_OLD = """  hooks.on_abort = [up]() {
    // 对端断了 / 框架掐断 → 会话按失败收场，**本次创建的临时文件全部删掉**。
    // 这是"失败时框架删"那个决策的落地处。
    up->notify_parse_done(false);
  };"""
V5_NEW = """  hooks.on_abort = [up]() {
    (void)up; /* MUTATION: 断开不通知会话 */
  };"""

RUN_TIMEOUT_S = 300


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
    rc, out = run(["cmake", "--build", tree, "--config", "Release", "--parallel", "4"],
                  timeout=900)
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    return rc, nerr, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")
    exe = os.path.join(exe_dir, "test_web_app_upload_route_func.exe")

    uw_before, wa_before = read_text(UPLOAD_CPP), read_text(APP_CPP)
    uw_md5, wa_md5 = md5(UPLOAD_CPP), md5(APP_CPP)
    # 还原放 `finally`、判定放 `finally` **之后**：`return` 写在 `finally` 里会
    # 吞掉正在传播的异常/返回值（Python 只给个 SyntaxWarning，不算错误），于是
    # 探针在构建失败时也会"静默返回成功"。`finally` 只负责收尾，不返回。
    try:
        # 1) 装探针（只加，不改行为）
        uw = uw_before
        assert uw.count(UNLINK_LINE) == 1, "析构的 unlink 锚点不唯一"
        write_text(UPLOAD_CPP, uw.replace(UNLINK_LINE, PROBED_LINE, 1))

        for label, mutate in (("基线（钩子在）", False), ("V5（钩子空）", True)):
            wa = wa_before
            if mutate:
                assert wa.count(V5_OLD) == 1, "V5 锚点不唯一"
                wa = wa.replace(V5_OLD, V5_NEW, 1)
            write_text(APP_CPP, wa)

            rc, nerr, out = build(args.tree)
            if rc != 0 or nerr:
                print(f"[{label}] 构建失败 rc={rc} errors={nerr}")
                print(out[-2000:])
                raise SystemExit(3)
            run(["cmake", "--build", args.tree, "--config", "Release",
                 "--target", "copy_test_dlls"])

            rc, out = run([exe, "disconnect_deletes_temp"], cwd=exe_dir)
            n = len(re.findall(r"\[probe\] ~uvcpp_web_upload unlink", out))
            print(f"[{label}] 用例 rc={rc}（0=绿）；析构兜底 unlink 次数 = {n}",
                  flush=True)
    finally:
        write_text(UPLOAD_CPP, uw_before)
        write_text(APP_CPP, wa_before)
        ok = md5(UPLOAD_CPP) == uw_md5 and md5(APP_CPP) == wa_md5
        print(f"\n源码按字节还原: {'是' if ok else '否 —— 有问题！'}")
        rc, out = run(["grep", "-rn", "PROBE\\|MUTATION",
                       os.path.join(ROOT, "src")])
        print(f"grep 残留: {out.strip() or '(无)'}")
        rc, nerr, _ = build(args.tree)
        run(["cmake", "--build", args.tree, "--config", "Release",
             "--target", "copy_test_dlls"])
        rc2, _ = run([exe, "disconnect_deletes_temp"], cwd=exe_dir)
        print(f"还原后重建 rc={rc} errors={nerr}；复跑 rc={rc2}"
              f"（应为 0，且上面不该再有 probe 行）")

    return 0 if ok and rc2 == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
