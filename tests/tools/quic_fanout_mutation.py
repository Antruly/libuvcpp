#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""QUIC 扇出的变异表 —— 钉住「n 个端点抢同一个端口」这条地基。

为什么单独一个文件（而不是往 `multiloop_mutation.py` 里补一条）
----------------------------------------------------------------
1. 那个脚本**只能在 Windows 上跑**：它的 `sync_dll()` 打开的是
   `tree/Release/uvcpp.dll`。这里的判据要能在 Linux 上跑，因为
   `UV_UDP_REUSEPORT` 的**负载均衡语义只有 Linux/FreeBSD/DragonFly/Solaris
   11.4/AIX 7.2.5 上有**（见 `handle/uvcpp_udp.h` 末尾），判据 3 那一类离了
   这个语义就没有可观测的差别。
2. 它钉的是 **web 层的多循环**（h1/h2/ws 的 `set_loops`）；这里钉的是 **QUIC 的
   UDP 端点** —— 另一套 socket 策略（标志 + 回落）、另外两条用例。

被测的不变式（每一条都在用例里有对应的断言）
--------------------------------------------
  A. `listen_quic()` 在 `loop_count() > 1` 时**真的**建了 1..n−1 号端点，而不是
     "表按 n 定尺寸、端点只建 0 号"。后者是个能过「合计守恒」那一条的坏实现。
  B. `is_reuseport()` 报的是"**这次绑定真的带上标志了没有**"，不是"调用方请求了
     没有"。这两个值在"标志被静默丢掉"时正好分叉。
  C. `bind_udp()` 里的标志**该带就带**。
  D. `set_reuseport()` 那道"装晚了不生效"的闸门是**诊断性**的（差的只有 stderr
     那一行，`reuseport` 早在 `listen()` 里定死了）—— 这一条**预期活下来**，
     留着是为了说明"哪一类变异本来就不该被抓住"，不是覆盖缺口。

变异与实测（2026-10-12，Linux 18 核，`--tree build-rel15 --jobs 6`；**预期**是先写
下来的，跑完对账，对不上就照实打印）
--------------------------------------------------------------------
  M1  `listen_quic()` 的端点循环 `for (i = 1; i < n; ++i)` 写死成 `i < 1`
      → **抓住**：`http3_multiloop` `rc=2 checks=37 failures=1`，那一句正是
      「内核给得出 REUSEPORT、`set_loops(n>1)` 也答应了，`quic_fanout()` 就必须
      为真」。纯 net 层那条**不受影响**（它不经过 web 层）
  M2  `bind_udp()` 里 `reuseport = (flags != 0)` 改成恒 `true`（把"请求了"当成
      "带上了"）→ **抓住**：`quic_server_multiloop` `rc=2 checks=29 failures=3`
      （判据 1 的 `!a.reuseport`、判据 6 的 `!srv.is_reuseport()`）。**只有 net
      层那条抓得住**，而且这是对的：在"内核给得出"的平台上 `want` 与真值本来就
      相等，这个谎**只在没请求标志的那些调用点上**看得出来
  M3  `bind_udp()` 里 `flags` 恒 0（无条件把标志丢掉）→ **抓住**：两条用例都红
      （`http3_multiloop` 判据 9、`quic_server_multiloop` 判据 8），红的都是那句
      **库外探针**的话
  M4  拆掉 `set_reuseport()` 里"已经在收包就忽略"那道闸门 → **没抓住**，且是
      **行为等价**：`reuseport` 是 `listen()` 里定死的，装晚了改 `want_reuseport`
      不影响任何已经发生的事情 —— 两者只差 stderr 那一行。用例不该去抓它

这张表第一版**不是**这个结果，差的地方值得记下来，因为修的是**用例**而不是脚本：

  - **第一版里 M1 没抓住、M3 只被 net 层抓住**。原因同一条：两条用例的判据**全部
    分叉在库自己报的数上**（`quic_fanout()` / `is_reuseport()`），而这两种坏实现
    报出来的形状与"平台拒绝标志"**一模一样** —— 它们在判据 1..8 下全绿。分得开
    两者的只有**库外**的事实：这个平台到底给不给得出分流。于是补了
    `tests/functional/reuseport_probe.h`（裸 `uv_loop_t`，同端口带标志绑两次）与
    两条用例各自的判据（h3 的判据 9、net 层的判据 8）。补完之后 M1/M3 都红，
    M3 从"一条抓到"变成"两条都抓到"。
  - M2 仍然只有 net 层那条抓得住，**这不是缺口**：理由见上（在给得出的平台上
    `want == 真值`，而 h3 用例里每个端点都请求了标志，谎没有落点）。

**这张表跑在哪个平台上，结论不一样，而且不一样得对**：M1/M3 记作 `"can"`，在
"内核给得出 REUSEPORT"的平台上该被抓住，在给不出的平台上（Windows/macOS/
libuv < 1.50）**该活下来** —— 那时回落本来就是正确行为，两者的形状本来就该一样。
脚本按基线里那条探针的输出对账，并在摘要里写明这一次是哪种。

平台相关的预期
--------------
M1/M3 的差别**只有在"内核给得出 REUSEPORT"的平台上才存在**：给不出的平台
（Windows 无条件拒、macOS `UV_ENOTSUP`、libuv < 1.50 没有这个标志）上，
`listen_quic()` 本来就该回落成单端点 —— 那时 M1/M3 与正确的实现**行为完全一样**，
抓不住才是对的。所以这两条在表里记作 `"can"`：**给得出就该抓住，给不出就该
活下来**，跑完按基线里那条探针输出对账（`platform_honors_reuseport_flag()` 的
结论会打在用例的输出里）。

用法
----
    python3 tests/tools/quic_fanout_mutation.py [--tree build-rel15] [--jobs 8]

要的是一棵 **QUIC=ON 的树**（`-DUVCPP_ENABLE_QUIC=ON`，h3 那条还要
`-DUVCPP_ENABLE_HTTP3=ON`）。脚本改源码、重编、跑两条用例，**按字节**还原并
用 md5 复核，最后再编一次确认全绿；任一步不对就非 0 退出。
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

WEB   = os.path.join(ROOT, "src", "web", "uvcpp_http_server.cpp")
QSRV  = os.path.join(ROOT, "src", "quic", "uvcpp_quic_server.cpp")

# 两条用例：web 层的 h3 扇出，与纯 net 层的端点原语。
TARGETS = [
    ("test_http3_multiloop_func", 180),
    ("test_quic_server_multiloop_func", 120),
]

# (标签, 预期, [(文件, 原文, 替换), …])
#   预期 True / False → 与平台无关
#   预期 "can"        → 只在"内核给得出 REUSEPORT"的平台上该被抓住，见文件头
MUTATIONS = [
    ("M1 listen_quic 的端点循环写死 1",
     "can",
     [(WEB,
       "  int first_err = 0;\n  for (int i = 1; i < n; ++i) {\n",
       "  int first_err = 0;\n"
       "  /* MUTATION: 端点循环写死 1 条（1..n-1 号一个都不建） */\n"
       "  for (int i = 1; i < 1; ++i) {\n")]),

    ("M2 探测值恒真（把请求当成结果）",
     True,
     [(QSRV,
       "    reuseport = (flags != 0);\n",
       "    reuseport = true;  /* MUTATION: 把 want 当成真的绑上了 */\n")]),

    ("M3 无条件丢掉标志",
     "can",
     [(QSRV,
       "    const int flags = want_reuseport ? UVCPP_UDP_REUSEPORT_FLAG : 0;\n",
       "    const int flags = 0;  /* MUTATION: 无条件不带标志 */\n")]),

    ("M4 装晚了那道闸门拆掉",
     False,
     [(QSRV,
       "  if (impl_->listening) {\n"
       "    std::fprintf(stderr,\n"
       "                 \"[uvcpp_quic_server] set_reuseport(): 端点已经在收包了，\"\n"
       "                 \"这一句被忽略（标志是 bind() 那一刻用的）\\n\");\n"
       "    return;\n"
       "  }\n"
       "  impl_->want_reuseport = on;\n",
       "  /* MUTATION: 装晚了也照样记下来（行为等价，只少一行 stderr） */\n"
       "  impl_->want_reuseport = on;\n")]),
]

RUN_TIMEOUT_S = 600


# ---------------------------------------------------------------------------
# 读写：换行统一成 LF 再匹配，写回按原样还原
# ---------------------------------------------------------------------------
#
# 本仓的 `.cpp` 两种换行都有（`.gitattributes` 让工作树是 CRLF、索引是 LF），
# 锚点按 `\n` 写，在 CRLF 的文件上一个都命中不了。比对前统一，写回时还原，
# 字节级由 md5 复核。

def read_text(p):
    with open(p, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")
    crlf = b"\r\n" in raw
    if crlf:
        assert raw.count(b"\r\n") == raw.count(b"\n"), "%s 混合换行，别用它" % p
        text = text.replace("\r\n", "\n")
    return text, crlf


def write_text(p, s, crlf):
    with open(p, "w", encoding="utf-8", newline=("\r\n" if crlf else "\n")) as f:
        f.write(s)


def read_bytes(p):
    with open(p, "rb") as f:
        return f.read()


def write_bytes(p, b):
    with open(p, "wb") as f:
        f.write(b)


def md5(p):
    return hashlib.md5(read_bytes(p)).hexdigest()


def run(cmd, cwd=ROOT, timeout=RUN_TIMEOUT_S):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "*** TIMEOUT ***"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree, jobs):
    names = [t for t, _ in TARGETS]
    rc, out = run(["cmake", "--build", tree, "--target"] + names +
                  ["--parallel", str(jobs)])
    errs = len(re.findall(r"\berror:", out)) + len(re.findall(r"error C\d+", out))
    return rc, errs, out


def run_targets(tree):
    """-> [(名字, rc, failures, [FAIL 行], [checks, failures], 全部输出), …]"""
    d = os.path.join(tree, "tests", "functional")
    rows = []
    for name, tmo in TARGETS:
        exe = os.path.join(d, name)
        if not os.path.exists(exe):
            rows.append((name, 127, -1, ["（可执行文件不在：%s）" % exe], [], ""))
            continue
        rc, out = run([exe], cwd=d, timeout=tmo)
        m = re.search(r"checks=(\d+) failures=(\d+)", out)
        fails = [s.strip().splitlines()[0]
                 for s in re.findall(r"\[FAIL\]([^\n]*)", out)]
        if not m:
            fails.insert(0, "（没有 checks=/failures= 那一行 —— 进程可能是被"
                            "信号打断的）")
        rows.append((name, rc,
                     int(m.group(2)) if m else -1,
                     fails,
                     [int(m.group(1)), int(m.group(2))] if m else [],
                     out))
    return rows


def ctest_whole_tree(tree):
    rc, out = run(["ctest", "--output-on-failure"], cwd=tree, timeout=1800)
    tot = re.search(r"\d+% tests passed, \d+ tests failed out of \d+", out)
    return rc, (tot.group(0) if tot else out[-200:])


def platform_can_reuseport(rows):
    """从基线输出里读库外探针的结论（判据 8 会把它打出来）。

    -> True / False / None（没读到 —— 用例没跑到，或者这棵树没编那条用例）
    """
    for name, _rc, _f, _fl, _c, out in rows:
        if "quic_server_multiloop" not in name:
            continue
        m = re.search(r"裸 libuv 探针[^：]*：(\S+)", out)
        if not m:
            return None
        return m.group(1).startswith("都成功")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-rel15")
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    if not os.path.isdir(tree):
        print("没有这棵树：%s —— 先按 doc/quic-guide.md 配一棵 QUIC=ON 的。"
              % tree)
        return 3

    # 上一次被 Ctrl-C / SIGKILL 打断会把这些标记留在源码里。**拒绝开始**，
    # 别让"接着改一个已经变异的源码"变成静默的假绿。
    for p in (WEB, QSRV):
        if "MUTATION" in read_text(p)[0]:
            print("%s 里已经有 MUTATION 标记 —— 上一次跑没还原干净，"
                  "先手工还原（`git diff` 看得出来）再跑。" % p)
            return 3

    files = sorted({e[0] for _l, _x, edits in MUTATIONS for e in edits})
    before = {p: (read_text(p), read_bytes(p)) for p in files}
    sums = {p: md5(p) for p in files}

    summary = []
    verdict_ok = True
    restored_ok = False
    tail_ok = False
    platform_can = None

    try:
        cases = [("基线（未变异）", None, None)] + [
            (l, exp, edits) for l, exp, edits in MUTATIONS]

        for label, expect, edits in cases:
            for p in files:
                write_text(p, before[p][0][0], before[p][0][1])
            if edits is not None:
                for path, old, new in edits:
                    s, crlf = read_text(path)
                    n = s.count(old)
                    assert n == 1, ("%s：锚点在 %s 里出现了 %d 次，"
                                    "必须恰好 1 次\n%r" % (label, path, n, old))
                    write_text(path, s.replace(old, new, 1), crlf)

            rc, errs, out = build(tree, args.jobs)
            if rc != 0 or errs:
                print("[%s] 构建失败 rc=%s errors=%s" % (label, rc, errs))
                print(out[-3000:])
                return 3

            rows = run_targets(tree)
            red = [e for e, r, f, _fl, _c, _o in rows if r != 0 or f != 0]
            caught = bool(red)

            if expect is None:
                platform_can = platform_can_reuseport(rows)
                verdict = "基线" + ("（绿）" if not caught else "（**居然红了**）")
                if caught:
                    verdict_ok = False
                print("\n[%s] %s；平台探针 = %s"
                      % (label, verdict,
                         {True: "内核给得出 REUSEPORT",
                          False: "给不出（回落那条腿）",
                          None: "没读到"}[platform_can]), flush=True)
            else:
                want = platform_can if expect == "can" else expect
                if expect == "can" and platform_can is None:
                    print("    **基线里没读到平台探针的结论 —— 这次的对账"
                          "不可靠。**")
                    verdict_ok = False
                ok = (caught == bool(want))
                if not ok:
                    verdict_ok = False
                mark = "✓" if ok else "✗**与预期不符**"
                why = ""
                if expect == "can" and not platform_can:
                    why = "（本平台给不出 REUSEPORT ⇒ 与正确的回落形状一样）"
                verdict = "%s %s（%s）%s" % (
                    "抓住" if caught else "没抓住", mark,
                    "、".join(x.replace("test_", "").replace("_func", "")
                              for x in red) or "两条都没红", why)
                print("\n[%s] %s" % (label, verdict), flush=True)

            for name, r, f, lines, cnt, _o in rows:
                extra = (" checks/failures=%d/%d" % tuple(cnt)) if cnt else ""
                print("    %-34s rc=%-4s%s" % (name, r, extra))
                for s in lines[:6]:
                    print("        %s" % s)
                if len(lines) > 6:
                    print("        …（还有 %d 条 FAIL）" % (len(lines) - 6))

            summary.append((label, verdict))

            # 预期该活下来的变异：整棵树也得绿，否则"等价"这个判定站不住。
            if expect is False and not caught:
                crc, txt = ctest_whole_tree(tree)
                print("    [整棵树 ctest] rc=%s %s" % (crc, txt), flush=True)
                if crc != 0 or "0 tests failed" not in txt:
                    print("    **预期活下来的变异把整棵树弄红了 —— 那条"
                          "\"行为等价\"的判定站不住。**")
                    verdict_ok = False
    finally:
        for p in files:
            write_text(p, before[p][0][0], before[p][0][1])
        restored_ok = all(md5(p) == sums[p] for p in files)
        print("\n源码按字节还原: %s" % ("是" if restored_ok else "否 —— 有问题！"))
        _rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")])
        print("grep 残留: %s" % (out.strip() or "(无)"))
        b_rc, errs, _ = build(tree, args.jobs)
        rows = run_targets(tree)
        tail_ok = (b_rc == 0 and errs == 0 and
                   all(r == 0 and f == 0 for _n, r, f, _l, _c, _o in rows))
        print("还原后重建 rc=%s errors=%s；两条用例复跑 %s（应全 0）"
              % (b_rc, errs, [(n.replace("test_", "").replace("_func", ""),
                               r, f) for n, r, f, _l, _c, _o in rows]))

    print("\n==== 汇总 ====")
    for label, verdict in summary:
        print("  %-40s %s" % (label, verdict))
    print("\n判据：每条变异的实际结果与预期一致、还原按字节、还原后全绿 "
          "→ %s" % ("全过" if (verdict_ok and restored_ok and tail_ok)
                    else "**有不对的**"))
    return 0 if (verdict_ok and restored_ok and tail_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
