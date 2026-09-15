# -*- coding: utf-8 -*-
"""一棵构建树的五道验收门。任何一道单独都不够，必须全过。

  1. 构建输出里 error C/LNK/MSB == 0（只看 ctest 会跑陈旧二进制）
  2. copy_test_dlls 后逐目录比对 uvcpp.dll 的 md5
  3. 用例 exe 的 mtime 晚于其源文件（只核对 DLL 不够）
  4. 全量 ctest
  5. 指定用例连跑 N 次，0 次非净

用法：
  python tests/tools/verify_tree.py --tree build-webapp \\
      --stability test_web_stream_response_func test_web_app_stream_resp_func \\
                  test_web_app_send_file_func
  python tests/tools/verify_tree.py --tree build-ssl --reconfigure \\
      --stability test_web_stream_response_func test_web_app_stream_resp_func \\
                  test_web_app_send_file_func
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST_DIRS = ["tests/unit", "tests/functional", "tests/expand"]
ERR_RE = re.compile(r"error C\d+|error LNK|error MSB")


def run(cmd, timeout=None):
    p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def gate1_build(tree, reconfigure):
    if reconfigure:
        # flag 集合必须写全，而且要**和 CI 的 job 一模一样**：只补一个变量的话，
        # 全新工作区会以 BUILD_TESTS=OFF 配出来 —— 构建照样 0 error，ctest 却
        # 一个用例都没有，于是"干净"是"什么都没测"的干净。
        cfg = ["cmake", "-S", ".", "-B", tree, "-DCMAKE_BUILD_TYPE=Release",
               "-DUVCPP_BUILD_TESTS=ON", "-DUVCPP_BUILD_SHARED=ON",
               "-DUVCPP_BUILD_WEB=ON", "-DUVCPP_BUILD_WEBAPP=ON",
               "-DUVCPP_ENABLE_ZLIB=ON"]
        if "ssl" in tree:
            cfg += ["-DUVCPP_ENABLE_OPENSSL=ON",
                    "-DOPENSSL_ROOT_DIR=C:/Program Files/openssl3",
                    "-DOPENSSL_USE_STATIC_LIBS=TRUE"]
        rc, out = run(cfg)
        print("  [1] 重新 configure rc=%d" % rc)
        for ln in out.splitlines():
            if "Including" in ln or "Configured uvcpp" in ln or "zlib" in ln.lower():
                print("      " + ln.strip())
        if rc != 0:
            print(out[-3000:])
            return False
    rc, out = run(["cmake", "--build", tree, "--config", "Release", "--parallel", "4"])
    nerr = len(ERR_RE.findall(out))
    for ln in out.splitlines():
        if ERR_RE.search(ln):
            print("      " + ln.strip())
    ok = rc == 0 and nerr == 0
    print("  [1] 构建 rc=%d errors=%d %s" % (rc, nerr, "OK" if ok else "FAIL"))
    return ok


def gate2_dll(tree):
    rc, out = run(["cmake", "--build", tree, "--config", "Release",
                   "--target", "copy_test_dlls"])
    if rc != 0:
        print("  [2] copy_test_dlls rc=%d FAIL" % rc)
        print(out[-2000:])
        return False
    ref = os.path.join(ROOT, tree, "Release", "uvcpp.dll")
    if not os.path.exists(ref):
        print("  [2] 找不到 %s FAIL" % ref)
        return False
    want = md5(ref)
    bad = []
    for d in TEST_DIRS:
        p = os.path.join(ROOT, tree, d, "Release", "uvcpp.dll")
        if not os.path.exists(p):
            bad.append("%s (缺失)" % d)
        elif md5(p) != want:
            bad.append("%s (%s)" % (d, md5(p)[:8]))
    ok = not bad
    print("  [2] DLL md5 %s (%s)%s" % ("OK" if ok else "FAIL", want[:8],
                                       "" if ok else " 不一致: " + ", ".join(bad)))
    return ok


def gate3_mtime(tree, names):
    """用例 exe 的 mtime 必须晚于它**自己的源文件**。

    刻意不拿整棵 src/ 的最新 mtime 当基准：测试 exe 链的是 uvcpp.dll 的导入库，
    改 src/ 只会重链 DLL，不一定重链 exe —— 那样比会假报（库是新的，exe 也确实
    不需要动）。库那一半由第 2 道门（DLL md5）负责，两道各管一件事。
    """
    bad = []
    for n in names:
        # `--stability` 收的是 **exe 名**（带 `test_` 前缀，gate5 要的），而源文件
        # 叫 `<name>.cpp`（不带前缀）。两个命名必然不同，所以这里两种都试 ——
        # 只试一种会让 gate3 对每个合法输入都报"找不到源文件"，那条 FAIL 是
        # 参数约定的锅，不是 exe 真的旧了。
        cands = [n + ".cpp"]
        if n.startswith("test_"):
            cands.append(n[len("test_"):] + ".cpp")
        src = None
        for c in cands:
            p = os.path.join(ROOT, "tests", "functional", c)
            if os.path.exists(p):
                src = p
                break
        exe = os.path.join(ROOT, tree, "tests", "functional", "Release", n + ".exe")
        if not os.path.exists(exe):
            bad.append("%s (缺失)" % n)
            continue
        if src is None:
            bad.append("%s (找不到源文件: 试过 %s)" % (n, ", ".join(cands)))
            continue
        if os.path.getmtime(exe) < os.path.getmtime(src):
            bad.append("%s (exe 旧于 .cpp)" % n)
    ok = not bad
    print("  [3] exe mtime %s%s" % ("OK" if ok else "FAIL",
                                    "" if ok else "  " + ", ".join(bad)))
    return ok


def gate4_ctest(tree):
    rc, out = run(["ctest", "--test-dir", tree, "-C", "Release",
                   "--timeout", "60", "--output-on-failure"], timeout=3600)
    tail = [ln for ln in out.splitlines()
            if "tests passed" in ln or "tests failed" in ln or ln.strip().startswith("The following tests FAILED")]
    for ln in tail[:4]:
        print("      " + ln.strip())
    failed = re.search(r"(\d+) tests failed out of", out)
    ok = rc == 0 and failed is None
    print("  [4] ctest %s" % ("OK" if ok else "FAIL"))
    return ok


def alive(name):
    p = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + name],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return name.lower() in p.stdout.decode("utf-8", "replace").lower()


def gate5_stability(tree, names, rounds):
    """起循环前先确认没有上一轮的残留进程：同名 exe 的两个副本会争用同一块
    磁盘状态（临时上传目录按相对名建立），症状是齐刷刷的全红。"""
    ok = True
    for n in names:
        exe = os.path.join(ROOT, tree, "tests", "functional", "Release", n + ".exe")
        if not os.path.exists(exe):
            print("  [5] %-40s 缺失 FAIL" % n)
            ok = False
            continue
        if alive(n + ".exe"):
            print("  [5] %-40s 有残留进程，先停掉 FAIL" % n)
            ok = False
            continue
        bad = 0
        for i in range(rounds):
            rc, out = run([exe], timeout=600)
            if rc != 0:
                bad += 1
                if bad == 1:
                    for ln in out.splitlines():
                        if "FAIL" in ln:
                            print("      " + ln.strip())
                            break
        print("  [5] %-40s nonclean=%d/%d %s" % (n, bad, rounds, "OK" if bad == 0 else "FAIL"))
        ok = ok and bad == 0
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True)
    ap.add_argument("--reconfigure", action="store_true")
    ap.add_argument("--stability", nargs="*", default=[])
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--skip-ctest", action="store_true")
    a = ap.parse_args()

    print("=== %s ===" % a.tree)
    results = [("build", gate1_build(a.tree, a.reconfigure)),
               ("dll", gate2_dll(a.tree)),
               ("mtime", gate3_mtime(a.tree, a.stability))]
    if not a.skip_ctest:
        results.append(("ctest", gate4_ctest(a.tree)))
    if a.stability:
        results.append(("stability", gate5_stability(a.tree, a.stability, a.rounds)))

    print("--- 汇总 ---")
    for name, ok in results:
        print("  %-10s %s" % (name, "PASS" if ok else "FAIL"))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
