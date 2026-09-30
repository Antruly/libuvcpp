#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""HTTP/3 的变异表 —— 仓内可重跑的那一份（`tests/tools/` 里第一条覆盖 http3 的驱动）。

为什么要有这个文件
------------------
1.4.1 的 HTTP/3 那一批（`fe35eca`…`ec0d77f`）做过一整张变异表，红几条都数过，
但那些脚本当时写在一个临时目录里，**没有被提交**。于是 `doc/testing-guide.md`
的「Mutation drivers」那张表里 `quic` / `http3` 一格都没有，任何人 grep 这个目录
都找不到一条能重跑的 h3 变异驱动 —— 覆盖是真的，可重跑性不是。这个脚本补的就是
后半句：把当时量过的几条重新写成可执行的驱动，谁都能再跑一遍。

被测的不变式（每一条都在用例里有对应的断言）
--------------------------------------------
  A. `on_write` 的**每流 FIFO** 是承重的：一次 `write_stream()` 对应一次
     `on_write`，字节数按**那条流**记账给 nghttp3（`add_ack_offset`）。记错流
     就等于 QPACK 的记账错位，而那是静默的。
  B. 一条请求的完成**恰好一次**。这条契约有两道守卫：`complete_response()`
     开头那道 `if (s.completed) return;`，和 `on_stream_close` 里的
     `!s.completed`。**两道是成对的**：单独拆掉任何一道，另一个调用点仍然挡着，
     所以单点变异**抓不住**；必须**同时**拆掉两道才看得见（M4）。
  C. `take_completed()` **取一次少一次**（第 5 条断言：再取一次是空）。
  D. QUIC 那一层把 FIN 与 RESET 分开报（`net_read_result::fin`）—— h3 的
     `nghttp3_conn_read_stream2(..., fin, ...)` 直接吃这一位。D1/D2 各拆一半。

变异与**实测**（2026-09-30，Linux，`--tree build-h3 --jobs 4`；`预期` 是先写下
来的，跑完对账，对不上就照实打印 —— 第一版就因为写错锚点对不上过一次，见 M3）
--------------------------------------------------------------------
  M1  把 `add_ack_offset` 的归属流换成 `pending_` 的第一条流（单计数器近似）
      → **没抓住**（`failures=0`，整棵树 79/79 也全绿）
  M2  拆掉 `complete_response()` 内部那道去重          → **没抓住**（见 B）
  M3  拆掉 `on_stream_close` 里那道 `!s.completed`      → **没抓住**（见 B）
  M4  M2 + M3 **同时**拆                                  → **抓住**：`http3_request`
      `rc=2 failures=5`，头一条正是 `那一条请求只给过一次：期望 1，实得 2`
  M5  `take_completed()` 取出后不 pop                     → **抓住**，但表现**不是
      一句 FAIL**：两次跑分别得到 `http3_request rc=124`（挂死到 60 s 超时）与
      `rc=-6`（SIGABRT），`http3_web` 两次都是 `rc=124` —— 同一个变异在两次跑里
      表现不同，而且都不是 `[FAIL]`。抓住它是靠"非零退出"这条底线，所以这条
      变异**只说明有东西在管它，不说明用例把它指出来了**。记下来，别把"挂死/
      中止"当成"红得漂亮"
  M6  `add_write_offset` 只在 `len > 0` 时调              → **没抓住**，且是
      **设计上不可达**：本层 `read_data` 永远把 EOF 与最后一块数据一起置上，
      nghttp3 因此不会产生"零字节 + 仅 FIN"的块；空体响应走 `dr == nullptr`，
      END_STREAM 骑在 HEADERS 上
  M7  拆掉 `on_quic_close` 里那道 `!s.completed`          → **没抓住**
  D1  QUIC：DATA 那一格的 `r.fin = fin` 改成恒 false      → **抓住**：`quic_api`
      `rc=2 failures=1`（`服务端 DATA 事件的 fin == true`）
  D2  QUIC：RESET 那一格的 `r.fin = false` 改成恒 true    → **抓住**：`quic_api`
      `rc=2 failures=1`（`RESET-0 的 PEER_CLOSED 带 fin == false`）

活下来的四条（M2/M3/M6/M7）里，**三条有出处、一条是缺口**：

  - M2/M3 是**成对的**：各自单独拆掉时另一个调用点仍然挡着，所以单点变异本来就
    不该红；M4 把两条一起拆就红了 —— 这正是"恰好一次"那条契约的守卫是**一对**
    而不是一处。换句话说 M2/M3 单独活下来不是覆盖缺口。
  - M6 是**不可达**（上面的写侧论证），M7 的路径（连接收尾时的兜底补一次）在现有
    用例里不会与"已经完成"重叠。
  - **M1 是真正的覆盖缺口**：把 `add_ack_offset` 的字节记到另一条流上，nghttp3
    看到的记账确实错了（不是等价变异），但现有三个用例没有一条能看出来 ——
    要有"多条流并发 + 用到 QPACK 动态表"的用例才可能观测到。这一条如实记下来，
    不许写"等价"。

按 `doc/testing-guide.md`「Mutation drivers」那条规矩，活下来不许直接写等价：
本脚本对每一条**预期活下来**的变异，除三个用例照常跑之外**再跑一次整棵树的
ctest**，全绿才算这条判定站得住（与 `run_idle_mutation.py` 的 M1 同一个做法）。
M6 的不可达是**读 `uvcpp_h3_connection.cpp` 的写侧**得到的论证，不是跑出来的，
两者一起才算数 —— 跑只证明"现有用例看不出"。M1 虽然也跑绿了整棵树，但它是
**缺口**不是等价，理由写在上面。

每个用例的超时是 60 s（正常跑完 0.43 s / 0.20 s / 0.76 s，三个都在半秒到一秒
之间），所以像 M5 那样"卡住"会被判成 `rc=124`：**非零即算抓住**，但日志里会
照实写出"没有 checks= 那一行"，免得读的人以为它是一句 FAIL。

判据（与别的驱动同一条）
------------------------
一次变异**算抓住**要同时满足两条：(1) 某个用例的退出码非零，**且** (2) 它自己
那行 `checks=… failures=…` 里 `failures` 非零 —— 也就是说**那一组**跑红了，不是
"前面某一组先红了把它带下去的"。三个用例是**分开跑**的，所以第二条天然成立，
但仍然逐条印出来，免得以后有人改成一条 `ctest -R` 就悄悄丢了这条性质。

用法
----
    python3 tests/tools/http3_mutation.py [--tree build-h3] [--jobs 4]

要的是一棵 **HTTP3=ON** 的树（`-DUVCPP_ENABLE_HTTP3=ON -DUVCPP_ENABLE_QUIC=ON`，
本机怎么配见 `doc/http3-guide.md` §CMake）。脚本改源码、重编、再按字节还原，
结束时核对 md5 并再跑一次确认全绿；任一步不对就非 0 退出。
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

H3_CONN = os.path.join(ROOT, "src", "http3", "uvcpp_h3_connection.cpp")
QUIC_CONN = os.path.join(ROOT, "src", "quic", "uvcpp_quic_connection.cpp")

# 三个目标：两个 h3 用例 + quic 自己的那条（D1/D2 打的就是它）。
TARGETS = [
    ("test_http3_request_func", 60),
    ("test_http3_web_func", 60),
    ("test_quic_api_func", 60),
]

# (标签, 预期[True=抓得住], [(文件, 原文, 替换), …])
MUTATIONS = [
    # 预期活下来 —— 而且是**覆盖缺口**（不是等价变异），见文件头 M1 那一段。
    ("M1 add_ack_offset 记错流",
     False,
     [(H3_CONN,
       "    self->add_ack_offset(stream_id, w.bytes);",
       "    /* MUTATION: 归属改成第一条流 */\n"
       "    self->add_ack_offset(pending_.begin()->first, w.bytes);")]),

    ("M2 complete_response 内部去重",
     False,
     [(H3_CONN,
       "  if (s.completed) return;\n  s.completed = true;",
       "  /* MUTATION: 内部那道去重拆掉 */\n  s.completed = true;")]),

    ("M3 on_stream_close 的 !s.completed",
     False,
     [(H3_CONN,
       "      if (!server_side_ && s.local && s.request && !s.completed) {",
       "      if (!server_side_ && s.local && s.request) { /* MUTATION */")]),

    ("M4 M2+M3 同时拆",
     True,
     [(H3_CONN,
       "  if (s.completed) return;\n  s.completed = true;",
       "  /* MUTATION: 内部那道去重拆掉 */\n  s.completed = true;"),
      (H3_CONN,
       "      if (!server_side_ && s.local && s.request && !s.completed) {",
       "      if (!server_side_ && s.local && s.request) { /* MUTATION */")]),

    ("M5 take_completed 不 pop",
     True,
     [(H3_CONN,
       "  out = std::move(completed_.front());\n  completed_.pop_front();",
       "  out = std::move(completed_.front());\n  /* MUTATION: 不 pop */")]),

    ("M6 add_write_offset 只在 n>0 时调",
     False,
     [(H3_CONN,
       "    const int ar = self->add_write_offset(sid, len);",
       "    const int ar = (len > 0) ? self->add_write_offset(sid, len) : 0;"
       "  /* MUTATION */")]),

    ("M7 on_quic_close 的 !s.completed",
     False,
     [(H3_CONN,
       "      if (s.local && s.request && !s.completed) {",
       "      if (s.local && s.request) { /* MUTATION */")]),

    ("D1 QUIC DATA 格子 fin 恒 false",
     True,
     [(QUIC_CONN,
       "      r.fin   = fin;\n      c.impl_->cbs.on_read(c, stream_id, r);",
       "      r.fin   = false;  /* MUTATION */\n"
       "      c.impl_->cbs.on_read(c, stream_id, r);")]),

    ("D2 QUIC RESET 格子 fin 恒 true",
     True,
     [(QUIC_CONN,
       "    r.fin   = false;\n",
       "    r.fin   = true;  /* MUTATION */\n")]),
]

RUN_TIMEOUT_S = 600


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
    """-> [(名字, rc, failures, [FAIL 行], [c1, c2]), …]"""
    d = os.path.join(tree, "tests", "functional")
    rows = []
    for name, tmo in TARGETS:
        exe = os.path.join(d, name)
        if not os.path.exists(exe):
            rows.append((name, 127, -1, ["（可执行文件不在：%s）" % exe], []))
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
                     [int(m.group(1)), int(m.group(2))] if m else []))
    return rows


def ctest_whole_tree(tree):
    rc, out = run(["ctest", "--output-on-failure"], cwd=tree, timeout=1800)
    tot = re.search(r"\d+% tests passed, \d+ tests failed out of \d+", out)
    return rc, (tot.group(0) if tot else out[-200:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-h3")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    if not os.path.isdir(tree):
        print("没有这棵树：%s —— 先按 doc/http3-guide.md 配一棵 HTTP3=ON 的。"
              % tree)
        return 3

    files = sorted({e[0] for _l, _x, edits in MUTATIONS for e in edits})
    before = {p: read_bytes(p) for p in files}
    sums = {p: md5(p) for p in before}

    summary = []
    verdict_ok = True
    restored_ok = False
    tail_ok = False

    try:
        cases = [("基线（未变异）", None, None)] + [
            (l, exp, edits) for l, exp, edits in MUTATIONS]

        for label, expect, edits in cases:
            for p, b in before.items():
                write_bytes(p, b)
            if edits is not None:
                for path, old, new in edits:
                    s = read_bytes(path).decode("utf-8")
                    n = s.count(old)
                    assert n == 1, ("%s：锚点在 %s 里出现了 %d 次，"
                                    "必须恰好 1 次\n%r" % (label, path, n, old))
                    write_bytes(path, s.replace(old, new, 1).encode("utf-8"))

            rc, errs, out = build(tree, args.jobs)
            if rc != 0 or errs:
                print("[%s] 构建失败 rc=%s errors=%s" % (label, rc, errs))
                print(out[-3000:])
                return 3

            rows = run_targets(tree)
            red = [e for e, r, f, _fl, _c in rows if r != 0 or f != 0]
            caught = bool(red)
            if expect is None:
                verdict = "基线" + ("（绿）" if not caught else "（**居然红了**）")
                if caught:
                    verdict_ok = False
            else:
                mark = "✓" if caught == expect else "✗**与预期不符**"
                if caught != expect:
                    verdict_ok = False
                verdict = "%s %s（%s）" % (
                    "抓住" if caught else "没抓住", mark,
                    "、".join(x.replace("test_", "").replace("_func", "")
                              for x in red) or "三个都没红")
            summary.append((label, verdict))
            print("\n[%s] %s" % (label, verdict), flush=True)
            for name, r, f, lines, cnt in rows:
                extra = (" checks/failures=%d/%d" % tuple(cnt)) if cnt else ""
                print("    %-26s rc=%-4s%s" % (name, r, extra))
                for s in lines[:6]:
                    print("        %s" % s)
                if len(lines) > 6:
                    print("        …（还有 %d 条 FAIL）" % (len(lines) - 6))

            if expect is False:
                crc, txt = ctest_whole_tree(tree)
                print("    [整棵树 ctest] rc=%s %s" % (crc, txt), flush=True)
                if crc != 0 or "0 tests failed" not in txt:
                    print("    **预期活下来的变异把整棵树弄红了 —— 那条"
                          "\"等价/不可达\"的判定站不住。**")
                    verdict_ok = False
    finally:
        for p, b in before.items():
            write_bytes(p, b)
        restored_ok = all(md5(p) == sums[p] for p in files)
        print("\n源码按字节还原: %s" % ("是" if restored_ok else "否 —— 有问题！"))
        rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")])
        print("grep 残留: %s" % (out.strip() or "(无)"))
        rc, errs, _ = build(tree, args.jobs)
        rows = run_targets(tree)
        tail_ok = (rc == 0 and errs == 0 and
                   all(r == 0 and f == 0 for _n, r, f, _l, _c in rows))
        print("还原后重建 rc=%s errors=%s；三个 exe 复跑 %s（应全 0）"
              % (rc, errs, [(n.replace("test_", ""),
                             r, f) for n, r, f, _l, _c in rows]))

    print("\n==== 汇总 ====")
    for label, verdict in summary:
        print("  %-38s %s" % (label, verdict))
    print("\n判据：每条变异的实际结果与预期一致、还原按字节、还原后全绿 "
          "→ %s" % ("全过" if (verdict_ok and restored_ok and tail_ok)
                    else "**有不对的**"))
    return 0 if (verdict_ok and restored_ok and tail_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
