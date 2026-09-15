# -*- coding: utf-8 -*-
"""X9 探针：裁决「短读不补」这条变异的真实判别者。

驱动给出的是 scoped(large_file) rc=0 而 full rc=2 —— 按"两条缺一不可"的
判定规格它被记成"没抓住"，可全量明明红了。这个矛盾只有两种结局：

  (a) 红的确实是某个用例，只是不是表里预测的那个（= **抓住了**，预测错）
  (b) 那次全量红是别的噪声（= 真的没抓住）

本探针把 (a)/(b) 分开：施加 X9 后**单独**跑 `small_file`。非 0 就是 (a)。

代码侧的机理（读源码得出，不是推断）：未知长度的传输由
`uvcpp_web_response.cpp:1269/1278` 设 `last_ = UINT64_MAX - 1`，于是变异里那句
守卫 `offset_ <= last_` **恒真** —— 它在**第一次**不足片的读上就触发，而不是
它自己声称的"文件中途"。已知长度的路径上 `last_ = size - 1`，同一次读会让
`offset_ > last_`，守卫为假。这就是为什么同一个用例的 CL 半边照旧绿、只有
chunked 那半边红，也是为什么 `large_file`（已知长度、且每次都正好读到边界）
**在结构上抓不住它**。

所以 X9 的结论是「抓住了，但抓它的是 small_file 而不是表里写的 large_file」，
且这个"抓住"**不等于**覆盖了真正的文件中途短读 —— 后者在本平台仍走不到
（3b 步骤 1 的结论）。三件事要分开记。
"""
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TREE = "build-webapp"
F = "src/webapp/uvcpp_web_file.cpp"
RUN_TIMEOUT_S = 600
ERR_RE = re.compile(r"error C\d+|error LNK|error MSB")

OLD = ('  const size_t n = static_cast<size_t>(nread);\n'
       '  offset_ += n;\n  bytes_sent_ += n;')
NEW = ('  const size_t n = static_cast<size_t>(nread);\n'
       '  offset_ += n;\n  bytes_sent_ += n;\n'
       '  if (n < slice_bytes_ && offset_ <= last_) '
       '{ submit_close(0); return; }  /* MUTATION X9 */')


def read_bytes(p):
    with open(p, "rb") as f:
        return f.read()


def run(cmd):
    try:
        p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=RUN_TIMEOUT_S)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        return 124, (e.stdout or b"").decode("utf-8", "replace")


def exe(case_tree=TREE):
    return os.path.join(ROOT, case_tree, "tests", "functional", "Release",
                        "test_web_app_send_file_func.exe")


def build():
    rc, out = run(["cmake", "--build", TREE, "--config", "Release",
                   "--parallel", "4"])
    nerr = len(ERR_RE.findall(out))
    return rc, nerr, out


def reds(out):
    got = []
    for ln in out.splitlines():
        m = re.match(r"^\[[a-z0-9_]+\]\s+(\S+)\s*$", ln)
        if m:
            got.append(m.group(1))
        m2 = re.search(r"\[FAIL\]\s*(.+)$", ln)
        if m2:
            got.append(m2.group(1).strip())
    return got


def main():
    path = os.path.join(ROOT, F)
    original = read_bytes(path)
    before_md5 = hashlib.md5(original).hexdigest()
    eol = "\r\n" if b"\r\n" in original else "\n"
    print("=== X9 探针 | 原始 md5=%s | 行尾=%s" % (before_md5[:12],
                                                  repr(eol)))

    # 基线：全量必须绿（否则下面"红了"不能归因给变异）
    rc, out = run([exe()])
    print("  基线全量            rc=%d" % rc)
    if rc != 0:
        print("  基线就不干净，探针没有意义：")
        for r in reds(out)[:6]:
            print("      " + r)
        return 3

    # 施加
    src = original.decode("utf-8").replace("\r\n", "\n")
    if src.count(OLD) != 1:
        print("  锚点命中 %d 次（要求 1）—— 锚点写错了，不是结论" % src.count(OLD))
        return 3
    mutated = src.replace(OLD, NEW, 1)
    if eol != "\n":
        mutated = mutated.replace("\n", "\r\n")
    with open(path, "wb") as f:
        f.write(mutated.encode("utf-8"))

    try:
        rc, nerr, _ = build()
        print("  构建(变异)          rc=%d errors=%d" % (rc, nerr))
        if nerr != 0:
            print("  构建失败 —— 不计入结论")
            return 3
        run(["cmake", "--build", TREE, "--config", "Release",
             "--target", "copy_test_dlls"])

        for case in ("small_file", "large_file"):
            rc_s, out_s = run([exe(), case])
            print("  scoped %-12s rc=%-4d %s" % (case, rc_s,
                                                 "" if rc_s else "绿"))
            for r in reds(out_s)[:4]:
                print("      " + r)
        rc_f, out_f = run([exe()])
        print("  全量                rc=%d" % rc_f)
    finally:
        # **立刻还原**，分析期间 src/ 不许停在变异状态
        with open(path, "wb") as f:
            f.write(original)
        after = hashlib.md5(read_bytes(path)).hexdigest()
        print("  逐字节还原          %s" % ("是" if after == before_md5
                                            else "否！" + after))
        rc, nerr, _ = build()
        print("  还原后重建          rc=%d errors=%d" % (rc, nerr))
        run(["cmake", "--build", TREE, "--config", "Release",
             "--target", "copy_test_dlls"])
        rc, out = run([exe()])
        print("  还原后全量          rc=%d %s" % (rc, "ALL PASS" if rc == 0 else "仍有红"))
        if rc != 0:
            for r in reds(out)[:6]:
                print("      " + r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
