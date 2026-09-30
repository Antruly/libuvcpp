#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C ABI（`src/capi/`）的变异表 —— 仓内可重跑的那一份。

为什么要有这个文件
------------------
C 门的五条承重规矩（甲、不透明句柄 + 活句柄表；乙、异常不过边界；丙、回调表按
`size` 逐格看；丁、所有权三类；戊、线程纪律）里，**甲、乙、丙**都是"我们很小心"
很容易写成、而"到底有没有用"很容易想当然的三条。这个驱动把它们各自的守卫**逐条
拆掉**，看 `tests/capi/` 那三个纯 C 用例（`test_capi_common_func` 量甲、丙，
`test_capi_net_func` 量乙和线程纪律，`test_capi_webapp_func` 量甲在 webapp 那侧的
**回调期句柄**形态与"查询不存在"那条约定）**有没有一声响**。没响的如实记成覆盖
缺口，不许写"等价"。

被测的不变式（每条都在用例里有对应的断言）
------------------------------------------
  A. 句柄失效之后再用要给 `UVCPP_C_E_STALE`，不给段错误。守卫是**两道**：
     `unregister_head()` 里既 `registry_remove()` 又 `poison_head()`。见 M1~M3。
     这条**必须拆成两句判**，而且第二句是第一版漏掉的：
       A1. 释放后再用/双重释放给 `E_STALE`（M1~M3 三种情况下都绿 —— 它量的是
           分配器，见 M2 上面那段）；
       A2. `uvcpp_c_live_handle_count()` 收支平衡（只有它能咬 M2/M3）。M1 按
           设计活下来。
     A 在 webapp 那侧还多一种形态：**回调期句柄**（`req` / `resp` 那些栈对象）
     出了回调就不许再用了。守卫是 `FrameScope` 的析构里那句"全部摘表"。
     见 M8。
  B. 句柄**类型**也要对：服务端句柄传给客户端那一族函数必须是 `E_STALE`。这一条
     打的只是第二道（魔数），见 M5。
  C. 回调表**只读调用方声明覆盖到了的那几格**：老客户端写一张只有前几格的表，
     后面那几格的字节**一个都不许读**。见 M4。
  D. C++ 异常不许穿过 `extern "C"`。见 M6 —— 判断它"没响"的标准是**进程没了**
     （abort），因为那正是"异常逃出去"的真实表现。
  E. 头里的 `UVCPP_C_ABI_VERSION` 与库返回的那个数必须一致（P/Invoke 最常见的
     故障：头与 .so/.dll 不是一次编出来的）。见 M7。
  F. **"查不到"与"查到空值"是两件事**（`1.4.3` 加的 `UVCPP_C_E_NOT_FOUND`）：
     `header("X-Nope")` 必须是负数，不能退化成 `0` —— 退化了就把"没带这个头"
     与"带了这个头但值是空的"合并成同一个回答。见 M9。
  G. 入参那一层要**当场**拒绝（`E_INVALID_ARG`），不能"注册成功、运行时才炸"。
     C 面最容易漏的就是这个：C 调用方没有编译期检查，一个 NULL 回调会一路走到
     事件循环里。见 M10。
  H. 客户端的"连上了没有"必须问**底下那条连接**，不能问 C++ 那层按位或的状态
     （`CONNECTED` 一旦置上就再也不会清）。见 M11。这条只有"关掉之后还不肯承认
     已断开"这种方向能量出来 —— 所以断言写在 `close()` 之后。
  I. `uvcpp_c_ws_client_connect` 只认 `ws://`：别的 scheme 当场 `E_INVALID_ARG`，
     不许往下走成"某个连接错误"。见 M12。

怎么判"抓住了"
--------------
与别的驱动同一条：(1) 某个用例的退出码非零，**且** (2) 它自己那行
`checks=… failures=…` 里 `failures` 非零。第二条是为了区分"这一组自己红的"和
"被别人的红带下去的"；三个用例是分开跑的，所以它天然成立，但仍然逐条印出来。
`rc` 是信号（M6 预期就是它）时不会有 `checks=` 那行 —— 那种情况下**非零退出码
就是判据**，脚本会照实写明"没有 checks= 那一行"，免得读的人以为它是一句 FAIL。

对**预期活下来**的变异，除三个用例照常跑之外**再跑一次整棵树**的 ctest；全绿才
算那条判定站得住（`doc/testing-guide.md` 的规矩，与 `run_idle_mutation.py` M1、
`http3_mutation.py` 同一条）。

用法
----
    python3 tests/tools/capi_mutation.py [--tree build-capi] [--jobs 8]

要的是一棵 **CAPI=ON** 的树（本机怎么配见 `doc/capi-guide.md` §怎么开）。
脚本改源码、重编、再按字节还原，结束时核对 md5 并再跑一次确认全绿；任一步不对
就非 0 退出。
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

INTERNAL = os.path.join(ROOT, "src", "capi", "uvcpp_c_internal.h")
COMMON = os.path.join(ROOT, "src", "capi", "uvcpp_c_common.cpp")
NET = os.path.join(ROOT, "src", "capi", "uvcpp_c_net.cpp")
WEB = os.path.join(ROOT, "src", "capi", "uvcpp_c_web.cpp")
WEBAPP = os.path.join(ROOT, "src", "capi", "uvcpp_c_webapp.cpp")

# 三个纯 C 用例（在 <tree>/tests/capi/ 下）。顺序无所谓，超时都是 60 s。
TARGETS = [
    "test_capi_common_func",
    "test_capi_net_func",
    "test_capi_webapp_func",
]

# 卸掉守卫的两句原文（`unregister_head()` 的函数体），M1~M3 共用。
UNREGISTER_BODY = ("  registry_remove(h);\n"
                   "  poison_head(reinterpret_cast<handle_head&>(*h));")

# (标签, 预期[True=抓得住], [(文件, 原文, 替换), …])
#
# 预期是怎么来的（**跑出来之后才写的，不是想当然**）：M1~M3 这三条第一版按
# "拆掉任何一道守卫都该红"写成 True/True/True，本机一跑，M1、M2、M3 **全都没抓住**。
# 追下去发现病不在守卫，在**用例**：`*_free()` 之后那块内存的第一个字节在 glibc 上
# 被 tcache 的 `next` 指针改写了，于是 `alive()` 读魔数那一句照样读出一个不等于
# 魔数的值 —— "释放后再用必须给 E_STALE"这几条断言量到的是**分配器**，不是守卫。
# 补上 `uvcpp_c_live_handle_count()` 与它那条收支平衡断言之后（登记表**只增不减**
# 这件事没有别的可观测形式），M2、M3 才真的红起来；M1 依然是"没抓住"，而这一次
# 是**如实**的：摘表还在，所以残局照样被挡住，毒化在这一条路上是冗余的。
MUTATIONS = [
    # ---- 甲：句柄失效的两道守卫，逐道拆 ----
    #
    # M1 是**预期活下来**的那一条，理由要写清楚：两道守卫互为备份 ——
    # `alive()` 先问登记表（不读内存）、再读魔数。拆掉毒化之后，摘表那一句仍然
    # 让 `registry_has()` 为假。所以"拆一道看不出差别"是这两道守卫的**设计意图**，
    # 不是覆盖缺口；真正的缺口是"两道一起拆"曾经也看不出差别（见 M3）。
    ("M1 只拆毒化（留摘表）",
     False,
     [(INTERNAL, UNREGISTER_BODY,
       "  registry_remove(h);  /* MUTATION: 不毒化 */")]),

    # M2：摘表没了 => 登记表**只增不减** => 活句柄数不回到基线。
    # 注意它红的是 `test_live_handle_count_balances()` 那一条，**不是**
    # "释放后再用给 E_STALE"那一组（那组在 M1/M2/M3 三种情况下都是绿的）。
    ("M2 只拆摘表（留毒化）",
     True,
     [(INTERNAL, UNREGISTER_BODY,
       "  poison_head(reinterpret_cast<handle_head&>(*h));"
       "  /* MUTATION: 不摘表 */")]),

    # M3：同上，且此时**两道手段一个都不剩** —— 这正是"没有活句柄数就完全量不
    # 出来"的那一条。
    ("M3 两道一起拆",
     True,
     [(INTERNAL, UNREGISTER_BODY,
       "  /* MUTATION: 摘表与毒化都拆掉 */")]),

    # ---- 丙：`size` 那套"逐格看" ----
    ("M4 field_present 恒真（=整表读满）",
     True,
     [(INTERNAL,
       "  return static_cast<std::size_t>(size) >= off + sizeof(M);",
       "  return static_cast<std::size_t>(size) + 0x100000u >= off + sizeof(M);"
       "  /* MUTATION: 恒真 */")]),

    # ---- 甲的第二道：魔数（类型）----
    ("M5 alive() 不看魔数",
     True,
     [(INTERNAL,
       "  return reinterpret_cast<const handle_head*>(h)->magic == magic;",
       "  return true;  /* MUTATION: 不看魔数 */")]),

    # ---- 乙：异常收口 ----
    # 两处锚点合起来只把 `UVCPP_C_TRY` 换成 `{`、把 `UVCPP_C_CATCH(...)` 删掉：
    # `connect_wait` 的**可观察行为一个字都没变**（还是那句 `connect_wait` 会抛），
    # 变的只有"抛出来之后有没有人接"。所以这条变异红掉的形态是**进程终止**，
    # 不是某条断言失败 —— 见脚本头部"怎么判抓住了"那一段。
    #
    # 第一版这条锚点写坏过一次（替换后的花括号不配对，cpp 直接编不过，脚本按
    # "构建失败"退 3 停在那里）—— 那一次是**脚本自己的 bug**，不是变异被抓住了。
    # 构建失败一律当场退 3，不许当成"抓住了"，这一条是刻意的。
    ("M6 connect_wait 去掉异常收口",
     True,
     [(NET,
       "    uvcpp_c_tcp_client* c, const char* ip, int port, int timeout_ms) {\n"
       "  UVCPP_C_TRY\n",
       "    uvcpp_c_tcp_client* c, const char* ip, int port, int timeout_ms) {\n"
       "  {  /* MUTATION: 异常边界拆了（没有 try、也没有 catch） */\n"),
      (NET,
       "    const int arm = maybe_arm_read(c);\n"
       "    return arm == 0 ? UVCPP_C_OK : arm;\n"
       "  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)\n"
       "}\n",
       "    const int arm = maybe_arm_read(c);\n"
       "    return arm == 0 ? UVCPP_C_OK : arm;\n"
       "  }  /* MUTATION: 没有 catch，异常直接穿过 extern \"C\" */\n"
       "}\n")]),

    # ---- 戊之外的一枚地基：ABI 自洽 ----
    ("M7 abi_version 与头里的宏不一致",
     True,
     [(COMMON,
       "  return static_cast<unsigned int>(UVCPP_C_ABI_VERSION);",
       "  return static_cast<unsigned int>(UVCPP_C_ABI_VERSION) + 1u;"
       "  /* MUTATION */")]),

    # =====================================================================
    # 批 2（webapp + web）。下面五条都只有 `test_capi_webapp_func` 咬得住
    # —— 批 1 的两个用例一个都不碰 webapp/web 那两片源码。
    # =====================================================================

    # ---- 甲在 webapp 那侧的形态：回调期句柄出了回调就不许再用 ----
    #
    # `FrameScope::~FrameScope()` 里那句循环是**唯一**一处"回调返回后把这些栈
    # 对象摘出登记表"。拆掉它之后，**只有** `uvcpp_c_live_handle_count()` 那条
    # 断言会红（`= 5, want 0`）。用例里另外那两条"把回调期句柄带出回调再问它"
    # 的 `E_STALE` 断言**照样过**。
    #
    # 原本这里的预期是"那两条会红在 `UVCPP_C_E_WRONG_THREAD` 上"，**实测不是**
    # —— 所以别照着那个预期去改用例。原因是 `creq` / `cresp` 是
    # `route_trampoline` 的**栈上局部量**：回调一返回，那块栈立刻被后续的循环
    # 代码复用，魔数被无关的写入盖掉，`alive()` 在"读魔数"那一句就判假，根本走
    # 不到线程检查。也就是说**只要句柄住在栈上，"魔数"这个判据就区分不出"被毒
    # 化"和"被栈复用盖掉"**。
    #
    # 那两条断言量的是**契约**（"回调之外问它必须给 `E_STALE`，且不许是 UB"），
    # 这条变异量得出来的是**机制**（登记表到底有没有摘干净）—— 两者都该有，错在
    # 拿前者当后者的证据。完整经过见 doc/capi-guide.md §6 的"第二次假绿"。
    ("M8 回调期句柄不摘表（FrameScope 拆掉）",
     True,
     [(WEBAPP,
       "    for (int i = n_ - 1; i >= 0; --i) unregister_head(slots_[i]);",
       "    /* MUTATION: 回调返回后不摘表 */")]),

    # ---- F：查不到 ≠ 空值 ----
    ("M9 查不到的头退化成 0",
     True,
     [(WEB,
       "    if (!resp->resp->has_header(name)) return UVCPP_C_E_NOT_FOUND;",
       "    if (!resp->resp->has_header(name)) return 0;  /* MUTATION */")]),

    # ---- G：入参当场拒绝 ----
    ("M10 serve_static 不查空参",
     True,
     [(WEBAPP,
       "    if (prefix == nullptr || root_dir == nullptr) "
       "return UVCPP_C_E_INVALID_ARG;",
       "    /* MUTATION: 空参不查（让它一路走到运行时） */")]),

    # ---- H：连上了没有要问底下那条连接 ----
    ("M11 is_connected 直接说自己连着",
     True,
     [(WEB,
       "    const bool up = tcp->has_status(uv::TCP_CLIENT_CONNECTED) &&\n"
       "                    !tcp->has_status(uv::TCP_CLIENT_CLOSING) &&\n"
       "                    !tcp->has_status(uv::TCP_CLIENT_CLOSED);\n"
       "    return up ? 1 : 0;\n",
       "    return 1;  /* MUTATION: 不查底层连接 */\n")]),

    # ---- I：只认 ws:// ----
    ("M12 ws_client_connect 不查 scheme",
     True,
     [(WEB,
       '    const std::string u(url);\n'
       '    if (u.compare(0, 5, "ws://") != 0) {',
       '    const std::string u(url);\n'
       '    if (false) {  /* MUTATION: 不查 scheme */')]),
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
    rc, out = run(["cmake", "--build", tree, "--target"] + TARGETS +
                  ["--parallel", str(jobs)])
    errs = len(re.findall(r"\berror:", out)) + len(re.findall(r"error C\d+", out))
    return rc, errs, out


def run_targets(tree):
    """-> [(名字, rc, failures, [FAIL 行], [c1, c2]), …]"""
    d = os.path.join(tree, "tests", "capi")
    rows = []
    for name in TARGETS:
        exe = os.path.join(d, name)
        if not os.path.exists(exe):
            rows.append((name, 127, -1, ["（可执行文件不在：%s）" % exe], []))
            continue
        rc, out = run([exe], cwd=d, timeout=60)
        m = re.search(r"checks=(\d+) failures=(\d+)", out)
        fails = [s.strip().splitlines()[0]
                 for s in re.findall(r"FAIL ([^\n]*)", out)]
        if not m:
            fails.insert(0, "（没有 checks=/failures= 那一行 —— 进程可能是被"
                            "信号打断的，rc=%s）" % rc)
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
    ap.add_argument("--tree", default="build-capi")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only", default=None,
                    help="只跑标签里含这个子串的变异（比如 M8）。基线照跑 —— 没有"
                         "基线就没有'抓住了'这句话的参照物。")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    if not os.path.isdir(tree):
        print("没有这棵树：%s —— 先按 doc/capi-guide.md 配一棵 CAPI=ON 的。" % tree)
        return 3

    selected = MUTATIONS
    if args.only:
        selected = [m for m in MUTATIONS if args.only.lower() in m[0].lower()]
        if not selected:
            print("没有标签含 %r 的变异。" % args.only)
            return 3
        print("--only %s：只跑 %s" % (args.only, "、".join(m[0].split()[0]
                                                          for m in selected)))

    files = sorted({e[0] for _l, _x, edits in selected for e in edits})
    before = {p: read_bytes(p) for p in files}
    sums = {p: md5(p) for p in before}

    summary = []
    verdict_ok = True
    restored_ok = False
    tail_ok = False

    try:
        cases = [("基线（未变异）", None, None)] + [
            (l, exp, edits) for l, exp, edits in selected]

        for label, expect, edits in cases:
            for p, b in before.items():
                write_bytes(p, b)
            if edits is not None:
                for path, old, new in edits:
                    s = read_bytes(path).decode("utf-8")
                    n = s.count(old)
                    assert n == 1, ("%s：锚点在 %s 里出现了 %d 次，必须恰好 1 次"
                                    "\n%r" % (label, path, n, old))
                    write_bytes(path, s.replace(old, new, 1).encode("utf-8"))

            rc, errs, out = build(tree, args.jobs)
            if rc != 0 or errs:
                print("[%s] 构建失败 rc=%s errors=%s" % (label, rc, errs))
                print(out[-3000:])
                return 3

            rows = run_targets(tree)
            red = [n for n, r, f, _fl, _c in rows if r != 0 or f != 0]
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
                    "、".join(x.replace("test_capi_", "").replace("_func", "")
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

            if expect is False and not caught:
                crc, txt = ctest_whole_tree(tree)
                print("    [整棵树 ctest] rc=%s %s" % (crc, txt), flush=True)
                if crc != 0 or "0 tests failed" not in txt:
                    print("    **预期活下来的变异把整棵树弄红了 —— 那条判定站不住。**")
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
              % (rc, errs, [(n.replace("test_capi_", ""), r, f)
                            for n, r, f, _l, _c in rows]))

    print("\n==== 汇总 ====")
    for label, verdict in summary:
        print("  %-38s %s" % (label, verdict))
    print("\n判据：每条变异的实际结果与预期一致、还原按字节、还原后全绿 → %s"
          % ("全过" if (verdict_ok and restored_ok and tail_ok) else "**有不对的**"))
    return 0 if (verdict_ok and restored_ok and tail_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
