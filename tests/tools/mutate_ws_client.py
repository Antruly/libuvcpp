# -*- coding: utf-8 -*-
"""变异验证 uvcpp_web_ws_client（框架层 WS 客户端）。

基线必须绿；每个变异改一处实现，重编 DLL（含 copy_test_dlls），跑
test_web_app_ws_client_func。变异"被抓住"= 该用例集不再 ALL PASS。

要点（都是这套流程里踩过的坑）：
  * 每次编译前 os.utime：还原文件会把老 mtime 一起还回去，MSVC 会跳过重编，
    后面所有变异的结果就整体错位一格。
  * 必须 copy_test_dlls：改了 src/*.cpp 只重编 DLL 不会刷新 tests/*/Release/
    下的副本，跑的还是旧库。用输出里有没有 "<<<PROBE" 判定在跑哪份库。
  * 补丁串必须真的替换成功（count==1），否则"没打上"会被误记成"变异被抓住"。
  * 超时算**被抓住**（见 run_case）：把重连整条去掉这类变异不会断言失败，而是
    让 run() 永远不返回。第一版没接住 TimeoutExpired，脚本跑完第一个变异就死，
    后面七个一个都没跑。
"""

import os
import re
import shutil
import subprocess
import sys

ROOT = r"D:\public\libuvcpp\libuvcpp"
TREE = os.path.join(ROOT, "build-webapp")
SRC = os.path.join(ROOT, "src", "webapp", "uvcpp_web_ws_client.cpp")
EXE = os.path.join(TREE, "tests", "functional", "Release",
                   "test_web_app_ws_client_func.exe")

# 单次用例集的墙钟上限。全部 11 个用例正常跑完约十几秒；超过这个数就是挂死。
CASE_TIMEOUT = 180

# (名字, 旧串, 新串, 说明, 期望被抓的用例)
MUTATIONS = [
    ("M1", "  if (on_close_) on_close_(code, reason);\n  schedule_reconnect();",
     "  if (on_close_) on_close_(code, reason);",
     "会话结束时不再排重连",
     "reconnect_after_restart / run_default_drives_reconnect（后者挂死）"),
    ("M2", "  user_closed_ = true;\n  restart_pending_ = false;",
     "  restart_pending_ = false;",
     "close() 不置 user_closed_", "close_cancels_reconnect"),
    ("M3", "  if (reconnect_.max_attempts > 0 && attempts_ >= reconnect_.max_attempts) {",
     "  if (false) {",
     "重连次数上限被忽略", "reconnect_backoff_and_exhaustion"),
    ("M4", "  if (reconnect_.backoff && attempts_ > 1) {",
     "  if (false) {",
     "退避不翻倍", "reconnect_backoff_and_exhaustion"),
    ("M5", "    if (restart_pending_) do_restart();\n    if (inner_ == nullptr) {\n      rc = 0;",
     "    if (inner_ == nullptr) {\n      rc = 0;",
     "run() 里丢掉待办的换客户端", "reconnect_after_restart"),
    ("M6", "  inner_->on_text(on_text_);\n  inner_->on_binary(on_bin_);",
     "  inner_->on_binary(on_bin_);",
     "重建后不再装 on_text", "reconnect_after_restart"),
    ("M7", "  timer_ = new uvcpp_timer(inner_->get_loop());",
     "  timer_ = nullptr; if (false) timer_ = new uvcpp_timer(inner_->get_loop());",
     "排了重连却不装定时器", "reconnect_after_restart"),
    ("M8", "    attempts_ = 0;\n    cancel_reconnect();\n    if (on_open_) on_open_(conn);",
     "    cancel_reconnect();\n    if (on_open_) on_open_(conn);",
     "连上之后不清零重连计数", "reconnect_after_restart"),
]


def run(cmd):
    return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")


def build(label):
    r = run('cmake --build "%s" --config Release --target uvcpp copy_test_dlls '
            '--parallel 4' % TREE)
    bad = [ln for ln in (r.stdout + r.stderr).splitlines()
           if "error C" in ln or "error LNK" in ln or "error MSB" in ln]
    if bad:
        print("  [%s] BUILD FAILED" % label)
        for ln in bad[:5]:
            print("      " + ln.strip())
        return False
    return True


def run_case():
    """跑一次用例集。

    返回 (rc, out)；rc 是 "HANG" 表示超时被杀，None 表示跑的是旧库。

    超时必须当成**被抓住**而不是脚本出错：有的变异（例如把重连整条去掉）会让
    run() 永远不返回 —— 表现是挂死而不是断言失败。第一版直接 subprocess.run
    (timeout=600) 抛 TimeoutExpired，脚本当场死掉，后面七个变异一个都没跑。
    """
    p = subprocess.Popen([EXE], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace")
    try:
        out, _ = p.communicate(timeout=CASE_TIMEOUT)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()  # 收残余输出，好知道挂在哪个用例上
        return "HANG", out
    if "<<<PROBE" in out:
        return None, out  # 跑的是旧库，这次结果不算
    return p.returncode, out


def failed_cases(out):
    return [ln.strip() for ln in out.splitlines()
            if ln.strip().startswith("FAIL") or " FAIL " in ln]


def last_started(out):
    """最后一个"打了名字但没等到 PASS/FAIL"的用例 —— 挂死就挂在它上面。"""
    names = [ln.strip() for ln in out.splitlines()
             if ln.strip().startswith("[") and "]" in ln
             and not ln.strip().startswith("[FAIL")]
    return names[-1] if names else "?"


def main():
    pristine = open(SRC, "rb").read()

    print("== 基线 ==")
    os.utime(SRC, None)
    if not build("base"):
        return 1
    rc, out = run_case()
    if rc is None:
        print("  基线跑到了旧库（输出里有 PROBE），先修 copy_test_dlls")
        return 1
    if rc != 0 or "ALL PASS" not in out:
        print("  基线不绿 rc=%s，变异验证没有意义" % rc)
        print("\n".join(out.splitlines()[-15:]))
        return 1
    m = re.search(r"ALL PASS \((\d+) cases\)", out)
    print("  基线绿：ALL PASS (%s cases)" % (m.group(1) if m else "?"))

    results = []
    try:
        for name, old, new, desc, expect in MUTATIONS:
            open(SRC, "wb").write(pristine)
            text = pristine.decode("utf-8")
            if text.count(old) != 1:
                print("== %s %s：补丁串未命中（count=%d），跳过 ==" %
                      (name, desc, text.count(old)))
                results.append((name, desc, expect, "PATCH-MISS"))
                continue
            open(SRC, "w", encoding="utf-8", newline="").write(
                text.replace(old, new, 1))
            os.utime(SRC, None)
            if not build(name):
                results.append((name, desc, expect, "BUILD-FAIL"))
                continue
            rc, out = run_case()
            if rc is None:
                results.append((name, desc, expect, "STALE-DLL"))
                continue
            # 挂死也是被抓住：run() 不返回本身就是这条变异造成的可观测后果。
            caught = (rc == "HANG") or not (rc == 0 and "ALL PASS" in out)
            hits = failed_cases(out)
            note = "; ".join(hits[:3])
            if rc == "HANG":
                note = "挂死在 %s（%ds 超时）" % (last_started(out), CASE_TIMEOUT)
            print("== %s %s -> %s rc=%s %s" %
                  (name, desc, "被抓" if caught else "**未被抓**", rc, note))
            results.append((name, desc, expect,
                            "caught" if caught else "SURVIVED"))
    finally:
        open(SRC, "wb").write(pristine)
        os.utime(SRC, None)
        build("restore")

    print("\n== 汇总 ==")
    for name, desc, expect, verdict in results:
        print("  %-4s %-22s %-10s expect=%s" % (name, desc, verdict, expect))
    survived = [r for r in results if r[3] == "SURVIVED"]
    bad = [r for r in results if r[3] in ("PATCH-MISS", "BUILD-FAIL", "STALE-DLL")]
    if survived:
        print("  未被抓：%s" % ", ".join(r[0] for r in survived))
    if bad:
        print("  无效：%s" % ", ".join(r[0] + "(" + r[3] + ")" for r in bad))
    return 2 if (survived or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
