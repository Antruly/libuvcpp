#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第 8 条（262 个无属主的泄漏循环）的定位探针。

「泄漏循环」的定义就是这个探针挂的那一处：`~uvcpp_loop` 走到
`uv_loop_close(loop) != 0` 那个分支 —— 句柄队列没清干净，于是**不释放**这块
内存（见 `src/handle/uvcpp_loop.cpp` 里那段说明）。有属主的那六族第九批已经
逐族收口了；剩下的这些没有归属，要**按句柄族**定位。

探针把每个循环的结局分成四类，四类加起来必须等于构造数 —— 对不上就是探针
自己漏记，脚本会当场断言失败（这条已经救过一次：多线程用例里裸 `long` 自增
丢更新，两次跑出 ctor=1 / ctor=2 而 leaked 都是 2）：

  freed     析构时 `uv_loop_close()` 成功，内存还回去了
  leaked    **析构时关不掉 → 有意泄漏**（就是第 8 条要数的那些）
  detached  析构时句柄已经被别人摘走了，提前返回
  never     构造了**从来没被析构**（例如第 10 条那个 `is_running()` 守卫
            就是有意把 loop 交出去的）

泄漏时对那个循环做一次 `uv_walk`，逐条打出 `handle->type` 与
`active/closing`，并在进程退出时（静态析构）汇总一张按族的直方图。

为什么要 `uv_walk` 而不是别的：泄漏分支上队列里挂着的句柄，其 `uv_handle_t`
内存**一定还在**（`uv_close` 之后要到关闭回调里才会从队列摘下来、才轮到
wrapper 释放），所以遍历是安全的。这一点后面用 PageHeap 复核（不安全的话它
会当场 unmap 崩溃，而不是安静地给出错的数）。

用法：python -u tests/tools/run_loop_leak_probe.py [--tree build-webapp]
                                            [--exe test_tcp_client_func]
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

# 同目录模块：`python tests/tools/xxx.py` 会把脚本所在目录放进 sys.path[0]
import ctest_list  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOOP_CPP = os.path.join(ROOT, "src", "handle", "uvcpp_loop.cpp")

PROBE_HEAD = """\
// ==== 第 8 条定位探针（临时插桩，跑完删） ====
#include <atomic>
#include <cstdio>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
namespace {
// 计数器必须是原子的：多线程用例里裸 `long` 自增会丢更新。
std::atomic<long> g_probe_ctor(0);
std::atomic<long> g_probe_freed(0);
std::atomic<long> g_probe_leaked(0);
std::atomic<long> g_probe_detached(0);
std::atomic<long> g_probe_types[UV_HANDLE_TYPE_MAX + 2];

// 每个泄漏一行的缓冲区。**不能共用全局的**：多线程同时泄漏时两个线程会
// 互相覆写（实测 `test_pipe_func` 打出了两条一模一样的 #2）。所以每行一个
// 局部量，经 `uv_walk` 的 arg 指针传进去。
struct probe_line {
  char buf[4096];
  int off;
  long queued;
  probe_line() : off(0), queued(0) { buf[0] = '\\0'; }
};

void probe_walk_cb(uv_handle_t *h, void *arg) {
  if (h == nullptr) return;
  probe_line *L = static_cast<probe_line *>(arg);
  if (L == nullptr) return;
  ++L->queued;
  const int left = static_cast<int>(sizeof(L->buf)) - L->off;
  if (left <= 0) return;
  const char *n = uv_handle_type_name(h->type);
  L->off += ::snprintf(L->buf + L->off, left, " %s(active=%d closing=%d)",
                       n ? n : "?", static_cast<int>(uv_is_active(h)),
                       static_cast<int>(uv_is_closing(h)));
}

// 泄漏点的调用栈。Release 的 exe 旁边就有 PDB，dbghelp 能直接解析出符号 ——
// 这比"按句柄族猜"精确：`closing=0` 与 `closing=1` 是两种病，家族名区分不了，
// 调用点能。
void probe_backtrace() {
  void *frames[32];
  const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
  static bool sym_ready = false;
  if (!sym_ready) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE)
      sym_ready = true;
  }
  for (USHORT i = 0; i < n; ++i) {
    const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
    char storage[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(storage);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (sym_ready && SymFromAddr(GetCurrentProcess(), addr, &disp, sym))
      ::fprintf(stderr, "        #%u %s+0x%llx\\n", i, sym->Name,
                static_cast<unsigned long long>(disp));
    else
      ::fprintf(stderr, "        #%u %p\\n", i,
                reinterpret_cast<void *>(addr));
  }
}

void probe_walk_cb_count(uv_handle_t *h, void *) {
  if (h == nullptr) return;
  const int t = static_cast<int>(h->type);
  if (t >= 0 && t < static_cast<int>(UV_HANDLE_TYPE_MAX) + 2)
    g_probe_types[t].fetch_add(1);
}

struct probe_reporter {
  ~probe_reporter() {
    ::fprintf(stderr,
              "[leak-probe] ctor=%ld freed=%ld leaked=%ld"
              " detached=%ld types:",
              g_probe_ctor.load(), g_probe_freed.load(),
              g_probe_leaked.load(), g_probe_detached.load());
    for (int i = 0; i < static_cast<int>(UV_HANDLE_TYPE_MAX) + 2; ++i) {
      const long cnt = g_probe_types[i].load();
      if (cnt == 0) continue;
      const char *n = uv_handle_type_name(static_cast<uv_handle_type>(i));
      ::fprintf(stderr, " %s=%ld", n ? n : "?", cnt);
    }
    ::fprintf(stderr, "\\n");
    ::fflush(stderr);
  }
};
probe_reporter g_probe_reporter;
}  // namespace

namespace uvcpp {
"""

LEAK_BODY = """\
    const long nth = g_probe_leaked.fetch_add(1) + 1;
    probe_line L;
    L.off = ::snprintf(L.buf, sizeof(L.buf),
                       "[leak-probe] LEAK #%ld loop=%p alive=%d handles:", nth,
                       static_cast<void *>(loop), uv_loop_alive(loop));
    uv_walk(loop, probe_walk_cb, &L);
    {
      const int left = static_cast<int>(sizeof(L.buf)) - L.off;
      if (left > 0)
        L.off += ::snprintf(L.buf + L.off, left, " |queued=%ld", L.queued);
    }
    if (L.queued == 0) {
      const int left = static_cast<int>(sizeof(L.buf)) - L.off;
      if (left > 0)
        L.off += ::snprintf(L.buf + L.off, left,
                            " <== 队列上没有非 internal 句柄（在途请求卡的）");
    }
    if (L.off < static_cast<int>(sizeof(L.buf)) - 1) {
      L.buf[L.off++] = '\\n';
      L.buf[L.off] = '\\0';
    }
    ::fputs(L.buf, stderr);
    probe_backtrace();
"""

# 每一处都是 (原文, 替换)；按文件自己的行尾对齐后再比对，且必须唯一命中。
PATCHES = [
    # 锚点只圈 `namespace uvcpp {`，**不要**连上面前一行一起写。原先写的是
    # `#include "uvcpp_loop.h"\nnamespace uvcpp {`，而 `f4f264c`（macOS 的
    # `sigemptyset` 是宏）在两者之间插了一段 `#if !defined(_WIN32) /
    # #include <signal.h> / #endif`，于是这条锚点一处都命中不了 —— 探针从那
    # 一笔起就再也跑不起来（`PROBE_HEAD` 自己以 `namespace uvcpp {` 收尾，
    # 插在它前面即可，插桩位置与原先完全一致）。
    (
        "\nnamespace uvcpp {\n",
        "\n" + PROBE_HEAD,
    ),
    (
        "uvcpp_loop::uvcpp_loop() : uvcpp_handle() {\n",
        "uvcpp_loop::uvcpp_loop() : uvcpp_handle() {\n"
        "  g_probe_ctor.fetch_add(1);\n",
    ),
    (
        "  uv_handle_t *h = this->get_handle();\n  if (h == nullptr)\n    return;\n",
        "  uv_handle_t *h = this->get_handle();\n"
        "  if (h == nullptr) {\n"
        "    g_probe_detached.fetch_add(1);\n"
        "    return;\n"
        "  }\n",
    ),
    (
        "  if (!closed_ && uv_loop_close(loop) != 0) {\n",
        "  if (!closed_ && uv_loop_close(loop) != 0) {\n" + LEAK_BODY,
    ),
    (
        "  uvcpp_free_bytes(h);\n  this->detach_handle();\n}\n",
        "  uvcpp_free_bytes(h);\n"
        "  g_probe_freed.fetch_add(1);\n"
        "  this->detach_handle();\n"
        "}\n",
    ),
]

EXE_TIMEOUT_S = 180
BUILD_TIMEOUT_S = 900


def read_text(p):
    with open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(p, s):
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def run(cmd, cwd=ROOT, timeout=EXE_TIMEOUT_S):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "*** TIMEOUT ***"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree):
    rc, out = run(["cmake", "--build", tree, "--config", "Release",
                   "--parallel", "4"], timeout=BUILD_TIMEOUT_S)
    return rc, len(re.findall(r"error C\d+|error LNK|error MSB", out)), out


def sync_dll(tree):
    """同步测试侧的 uvcpp.dll（`copy_test_dlls` 是 ALL 目标，单 target 构建不跑它）。"""
    src = os.path.join(tree, "Release", "uvcpp.dll")
    with open(src, "rb") as f:
        data = f.read()
    n = 0
    for base, _dirs, files in os.walk(tree):
        if "uvcpp.dll" in files:
            dst = os.path.join(base, "uvcpp.dll")
            if os.path.abspath(dst) != os.path.abspath(src):
                with open(dst, "wb") as f:
                    f.write(data)
                n += 1
    return n


def apply_patches(text):
    nl = "\r\n" if "\r\n" in text else "\n"
    cur = text
    for old, new in PATCHES:
        o = old.replace("\n", nl)
        n = new.replace("\n", nl)
        assert cur.count(o) == 1, f"锚点不唯一/找不到：{old[:60]!r}"
        cur = cur.replace(o, n, 1)
    return cur


def parse(out):
    m = re.search(r"\[leak-probe\] ctor=(\d+) freed=(\d+) leaked=(\d+)"
                  r" detached=(\d+) types:(.*)", out)
    if m is None:
        return None
    types = {}
    for k, v in re.findall(r"(\S+)=(\d+)", m.group(5)):
        types[k] = int(v)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)),
            int(m.group(4)), types)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    ap.add_argument("--exe", default=None, help="只跑这一个（不带 .exe）")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)

    before = read_text(LOOP_CPP)
    sum0 = md5(LOOP_CPP)
    report = []
    agg_types = {}
    tot = [0, 0, 0, 0, 0]

    try:
        write_text(LOOP_CPP, apply_patches(before))
        rc, nerr, out = build(args.tree)
        if rc != 0 or nerr:
            print(f"构建失败 rc={rc} errors={nerr}\n{out[-3000:]}")
            raise SystemExit(3)
        sync_dll(tree)

        # 清单取自 ctest，不是"磁盘上有哪些 exe" —— 源码删掉后 exe 会留在原地，
        # 跑它得到的泄漏数字是假的（见 ctest_list）。报 stray 要在 --exe 过滤**之前**，
        # 否则过滤剩下的那些会把被滤掉的正常用例全报成 stray。
        all_tests = ctest_list.ctest_tests(tree)
        if not all_tests:
            print(f"在 {args.tree}/tests 下没找到 ctest 登记的 Release 用例 —— 先建一遍")
            raise SystemExit(3)
        ctest_list.report_stray(tree, {t[1] for t in all_tests})
        tests = all_tests
        if args.exe is not None:
            tests = [t for t in all_tests if t[0] == args.exe]
            if not tests:
                print(f"--exe {args.exe} 一个都没匹配上")
                raise SystemExit(3)
        exes = [t[1] for t in tests]
        print(f"跑 {len(exes)} 个 exe（{args.tree}）\n")

        for exe in exes:
            name = os.path.basename(exe)[:-4]
            erc, eout = run([exe], cwd=os.path.dirname(exe))
            got = parse(eout)
            if got is None:
                note = "超时" if erc == 124 else f"rc={erc}，没打出报告"
                print(f"  {name:42s} {note}")
                report.append((name, None, erc))
                continue
            ctor, freed, leaked, detached, types = got
            never = ctor - freed - leaked - detached
            assert never >= 0, (
                f"{name}: ctor({ctor}) < freed({freed}) + leaked({leaked})"
                f" + detached({detached}) —— 探针重复记账")
            tot[0] += ctor
            tot[1] += freed
            tot[2] += leaked
            tot[3] += detached
            tot[4] += never
            for k, v in types.items():
                agg_types[k] = agg_types.get(k, 0) + v
            mark = "  <== 泄漏" if leaked else ""
            print(f"  {name:42s} ctor={ctor:4d} freed={freed:4d} "
                  f"leaked={leaked:4d}{mark}")
            if never:
                print(f"        （另有 {never} 个循环从未被析构）")
            for block in re.findall(
                    r"\[leak-probe\] LEAK #.*(?:\n\s+#\d+ [^\n]*)*", eout):
                for line in block.splitlines():
                    print(f"        {line.strip()[:200]}")
            report.append((name, got, erc))
    finally:
        write_text(LOOP_CPP, before)
        ok = md5(LOOP_CPP) == sum0
        rc, nerr, _o = build(args.tree)
        sync_dll(tree)
        print(f"\n源码按字节还原: {'是' if ok else '否 —— 有问题！'}")
        print(f"还原后重建 rc={rc} errors={nerr}")
        if not ok or rc != 0 or nerr:
            print("*** 收尾没干净，先别信上面的数 ***")

    print("\n==== 汇总（%s）====" % args.tree)
    print(f"  uvcpp_loop  构造 {tot[0]} / 正常释放 {tot[1]} / "
          f"**泄漏 {tot[2]}** / 已 detach 提前返回 {tot[3]} / "
          f"从未析构 {tot[4]}")
    print(f"  账目核对：{tot[1]}+{tot[2]}+{tot[3]}+{tot[4]} = "
          f"{sum(tot[1:])}（构造 {tot[0]}）"
          f" {'OK' if sum(tot[1:]) == tot[0] else '**对不上**'}")
    if agg_types:
        print("  泄漏循环上残留的句柄族（累加）：")
        for k, v in sorted(agg_types.items(), key=lambda kv: -kv[1]):
            print(f"    {k:12s} {v}")
    leaking = [(n, g[2]) for n, g, _e in report if g and g[2]]
    print(f"  有泄漏的用例 {len(leaking)} 个，合计 {sum(v for _n, v in leaking)}：")
    for n, v in leaking:
        print(f"    {n:42s} {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
