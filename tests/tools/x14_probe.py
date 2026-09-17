# -*- coding: utf-8 -*-
"""X14 探针：裁决「断连时不通知流对象」到底抓住没有。

驱动给的期望目标是 `client_disconnect_frees_transfer`（`web_app_stream_resp_func`），
而那一组**明确断言的是相反的行为** —— 它的注释原文：

    对端断开本身**不**结算响应流。这是有意钉住的现状：`on_close` 只对
    **请求**流对象调 `stream_abort()`（判据是 `ctx.streaming()`）……

也就是说 X14 那段代码管的是**请求流**（上传/流式路由），声明的目标却是一个
**响应流**的用例 —— 期望写错了，而且驱动跑的那 5 个 target 里**不包含**
真正会走到那条分支的用例文件（`web_app_stream_func`、`web_app_upload_*`）。

所以两种结局：

  (a) 它其实**抓住了**，只是被另一个 exe 抓住（= 表里的 target 写错）
  (b) 全绿（= 要么等价，要么是真缺口，再往下判）

本探针把 (a)/(b) 分开：施加 X14 后单独跑 `abort_mid_stream`
（Phase 3a 就在钉「对端断开 ⇒ on_abort 恰好一次」），再全量跑三个相关 exe。

代码侧的机理（读源码得出）：`ctx->stream_abort()` 在**全仓只有这一个调用点**
（`grep -rn stream_abort src/` 只有定义 + 这一处调用），而它内部是
`deliver_abort()` 的**唯一**调用者。所以摘掉这一段 = 请求流在断连时**再也没有
任何人**通知它。
"""
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TREE = "build-webapp"
A = "src/webapp/uvcpp_web_app.cpp"
RUN_TIMEOUT_S = 600
ERR_RE = re.compile(r"error C\d+|error LNK|error MSB")

# 流水线那次改动把 `on_close` 里的单槽 `it->second` 换成了"扫队列、先收名单再
# 动手"（`inflight_` 从 `map<conn_id, shared_ptr<ctx>>` 变成了按连接分组的
# `deque<out_entry>`），所以锚点跟着挪到队列那一行。**变异语义一模一样**：
# 断连时一条流都不通知。
OLD = '          if (e->ctx->streaming()) victims.push_back(e->ctx);'
NEW = ('          /* MUTATION X14: 断连时不通知流对象 */\n'
       '          (void)e;')

# (exe 名, 单跑时的过滤参数或 None)
SCOPED = ("test_web_app_stream_func", "abort_mid_stream")
FULLS = ["test_web_app_stream_func",
         "test_web_app_upload_route_func",
         "test_web_app_upload_func",
         "test_web_app_stream_resp_func"]


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


def exe(name):
    return os.path.join(ROOT, TREE, "tests", "functional", "Release",
                        name + ".exe")


def build():
    rc, out = run(["cmake", "--build", TREE, "--config", "Release",
                   "--parallel", "4"])
    return rc, len(ERR_RE.findall(out))


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


def report(tag, cmd, label):
    rc, out = run(cmd)
    print("  %-4s %-36s rc=%d" % (tag, label, rc))
    if rc != 0:
        for r in reds(out)[:4]:
            print("      " + r)
    return rc


def main():
    path = os.path.join(ROOT, A)
    original = read_bytes(path)
    before_md5 = hashlib.md5(original).hexdigest()
    eol = "\r\n" if b"\r\n" in original else "\n"
    print("=== X14 探针 | 原始 md5=%s | 行尾=%s" % (before_md5[:12], repr(eol)))

    rc, nerr, = build()
    print("  基线构建            rc=%d errors=%d" % (rc, nerr))
    run(["cmake", "--build", TREE, "--config", "Release", "--target",
         "copy_test_dlls"])
    base = {}
    base[SCOPED[0]] = report("基线", [exe(SCOPED[0]), SCOPED[1]],
                             SCOPED[0] + " " + SCOPED[1])
    for n in FULLS:
        base[n] = report("基线", [exe(n)], n + " 全量")
    if any(v != 0 for v in base.values()):
        print("  基线就不干净，探针没有意义")
        return 3

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
        rc, nerr = build()
        print("  构建(变异)          rc=%d errors=%d" % (rc, nerr))
        if nerr != 0:
            print("  构建失败 —— 不计入结论")
            return 3
        run(["cmake", "--build", TREE, "--config", "Release", "--target",
             "copy_test_dlls"])
        report("X14", [exe(SCOPED[0]), SCOPED[1]],
               SCOPED[0] + " " + SCOPED[1])
        for n in FULLS:
            report("X14", [exe(n)], n + " 全量")
    finally:
        # **立刻还原**，分析期间 src/ 不许停在变异状态
        with open(path, "wb") as f:
            f.write(original)
        after = hashlib.md5(read_bytes(path)).hexdigest()
        print("  逐字节还原          %s" % ("是" if after == before_md5
                                            else "否！" + after))
        rc, nerr = build()
        print("  还原后重建          rc=%d errors=%d" % (rc, nerr))
        run(["cmake", "--build", TREE, "--config", "Release", "--target",
             "copy_test_dlls"])
        for n in FULLS:
            report("还原", [exe(n)], n + " 全量")
    return 0


if __name__ == "__main__":
    sys.exit(main())
