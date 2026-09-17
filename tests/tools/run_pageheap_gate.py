#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第 9 条：PageHeap 全量门禁（可选）。

第九批把"打开 PageHeap 重跑一遍全量"证明成了一条能抓 use-after-free 的门 ——
既有三层验收（普通构建 / 无内存池构建 / 单元测试）全绿的同时，它一次抓出 8 个
用例的释放后使用。但那次只写在「给下一轮的建议」里，没固化。这个脚本就是固化。

它为什么能抓：裸跑时 `free` 掉的内存页还在、内容还是旧的，读它读不出毛病来；
`/p /enable <exe> /full`（完整页堆）把释放过的块**立刻 unmap**，同一个读当场
变成访问违例（`0xC0000005`）。所以这里的输出**不是**"跑绿了"，而是两个数：
基线退出码 与 页堆退出码。

两个必须守住的点，都是第九批实测踩出来的：

  1. **gflags 在 Git Bash 里静默不生效。** 直接敲 `gflags -p /enable x.exe /full`
     会被 MSYS 的路径转换把 `/enable`、`/full` 当成盘符路径改写掉，命令照旧
     **退出 0**，注册表里什么都没写。所以这里全程走 `subprocess` 的**列表形式**，
     不经 shell；而且开、关之后**都回查注册表**（`GlobalFlag & 0x02000000`），
     不认退出码。回查时注意 gflags 写进去的是 `REG_SZ`（`"0x02000000"`），
     不是 `DWORD` —— 拿字符串直接 `&` 会 `TypeError`。

  2. **必须保证跑完 `-p /disable`。** 页堆残留会让这台机器上后面所有测试都慢一个
     量级，而且很难联想到这里。所以每一个 enable 都配 `finally` 关闭，另加
     `atexit` 与 `SIGINT`/`SIGTERM` 兜底；崩溃、异常、超时、Ctrl-C 都要关。
     开跑前还会扫一遍残留（上一个脚本崩在半路留下的）。

判据是**对照**，不是单独一次跑绿：先裸跑拿基线，再开页堆跑一遍，只有
「基线绿 + 页堆红」才算抓到。基线本身就红的话这条门禁什么也证明不了，直接以
退出码 3 停下 —— 免得把普通断言失败读成"页堆抓到了"。

`0xC0000139`（入口点找不到）与 `0xC0000135`（找不到 DLL）单独归类并且**算门禁
自身失败**：那是 DLL 没刷新，不是缺陷。本机 `copy_test_dlls` 因为没有 `pwsh.exe`
而静默失效，所以开跑前自己按 mtime 刷一遍 DLL。

本门禁自己的效力由 `run_tcp_client_dtor_mutation.py` 证明（它的 M1/M2/M3 三个
变异全靠页堆才抓得住），`--self-test` 就是转调它。

用法：
    python -u tests/tools/run_pageheap_gate.py --tree build-webapp
    python -u tests/tools/run_pageheap_gate.py --tree build-nopool --exe test_random_func
    python -u tests/tools/run_pageheap_gate.py --tree build-webapp --self-test

退出码：0 门禁全绿；1 页堆抓到问题（这才是门禁该拦的）；3 门禁自身没跑成
（基线红 / 页堆没设上 / 没关干净 / 抓到的是过期 DLL）。
"""

import argparse
import atexit
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.stdout.reconfigure(encoding="utf-8")

try:
    import winreg
except ImportError:  # 非 Windows
    winreg = None

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

IFEO = (r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"
        r"\Image File Execution Options")
FLG_HEAP_PAGE_ALLOCS = 0x02000000

# 崩溃码 → 人话。页堆下抓到的释放后使用落在第一行。
CRASH_CODES = {
    0xC0000005: "访问违例（释放后使用 / 越界 / 空指针）",
    0xC0000374: "堆损坏",
    0xC0000409: "fastfail（栈缓冲溢出 / __fastfail）",
    0xC0000602: "fail-fast 异常",
    0xC000001D: "非法指令",
    0xC00000FD: "栈溢出",
    0xC000008C: "数组越界（/GS 或 __CxxFrameHandler）",
}
# 这两个不是缺陷，是 DLL 没刷新 —— 单列，且算门禁自身失败。
STALE_CODES = {
    0xC0000139: "入口点找不到（DLL 过期，先重建）",
    0xC0000135: "找不到 DLL",
    0xC000007B: "映像格式不对（32/64 位混用）",
}

BASELINE_TIMEOUT_S = 180
PAGEHEAP_TIMEOUT_S = 900

# 已经开着页堆、还没关掉的 exe。名字 → 完整路径。所有退出路径都从它恢复。
ENABLED = {}


# ---------------------------------------------------------------------------
# 基础
# ---------------------------------------------------------------------------

def md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def run(cmd, timeout=120, cwd=None):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "*** TIMEOUT ***"
    except OSError as e:
        return -1, "*** OSError: %s ***" % e
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def find_test_exes(tree):
    """全部测试可执行文件，(名字, 路径, ctest 用的工作目录)。

    工作目录按 `add_test` 的默认值取 —— `CMAKE_CURRENT_BINARY_DIR`，也就是
    装 exe 的那层 Release 的**上一级**（`tests/functional` 等）。
    """
    out = []
    for base, _dirs, files in os.walk(os.path.join(tree, "tests")):
        for f in files:
            if f.startswith("test_") and f.lower().endswith(".exe"):
                path = os.path.join(base, f)
                out.append((f[:-4], path, os.path.dirname(base)))
    out.sort()
    return out


def sync_dlls(tree, exe_dirs):
    """按 mtime 取每份 DLL 的最新来源，刷进每个测试输出目录。

    `copy_test_dlls` 在本机因为找不到 `pwsh.exe` 而**静默失效**，DLL 不刷新
    的表现是 `0xC0000139` —— 看着像崩溃，其实是过期。
    """
    names = set()
    for d in exe_dirs:
        for f in os.listdir(d):
            if f.lower().endswith(".dll"):
                names.add(f)

    src = {}
    for base, _dirs, files in os.walk(tree):
        if os.sep + "tests" + os.sep in base + os.sep:
            continue  # 测试输出目录是消费方，不是来源
        for f in files:
            if f not in names:
                continue
            p = os.path.join(base, f)
            # mtime 相同时取路径浅的（`Release/` 优先于 `examples/Release/`）
            key = (os.path.getmtime(p), -len(p))
            if f not in src or key > src[f][0]:
                src[f] = (key, p)

    n = 0
    for d in exe_dirs:
        for f in sorted(names):
            if f not in src:
                continue
            s = src[f][1]
            dst = os.path.join(d, f)
            if os.path.abspath(dst) == os.path.abspath(s):
                continue
            if os.path.exists(dst) and md5(dst) == md5(s):
                continue
            shutil.copyfile(s, dst)
            n += 1
    return n, len(src)


# ---------------------------------------------------------------------------
# gflags / 注册表
# ---------------------------------------------------------------------------

def find_gflags(override=None):
    cands = []
    if override:
        cands.append(override)
    for env in ("ProgramFiles(x86)", "ProgramFiles"):
        root = os.environ.get(env)
        if root:
            for kits in ("10", "8.1"):
                cands.append(os.path.join(root, "Windows Kits", kits,
                                          "Debuggers", "x64", "gflags.exe"))
    for c in cands:
        if c and os.path.exists(c):
            return c
    return None


def globalflag(exe_name):
    """`Image File Execution Options\\<exe>` 的 `GlobalFlag`，没有就是 None。"""
    if winreg is None:
        return None
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            IFEO + "\\" + exe_name) as k:
            raw = winreg.QueryValueEx(k, "GlobalFlag")[0]
    except OSError:
        return None
    if isinstance(raw, str):
        # gflags 写的是 REG_SZ（"0x02000000"），不是 DWORD
        raw = int(raw, 0)
    return raw


def pageheap_on(exe_name):
    v = globalflag(exe_name)
    return bool(v is not None and (v & FLG_HEAP_PAGE_ALLOCS))


def set_pageheap(gflags, exe_path, on):
    """开/关，**回查注册表**确认 —— gflags 静默失效过一次（见文件头）。"""
    name = os.path.basename(exe_path)
    args = [gflags, "-p", "/enable" if on else "/disable", exe_path]
    if on:
        args.append("/full")
    rc, out = run(args, timeout=60)
    state = pageheap_on(name)
    if state == on and on:
        ENABLED[name] = exe_path
    elif not on:
        ENABLED.pop(name, None)
    ok = (state == on)
    return ok, "rc=%s 注册表=%s" % (rc, state)


def disable_all(gflags, verbose=True):
    """把所有还开着的关掉。崩溃 / 异常 / 超时 / Ctrl-C 都得走到这里。"""
    left = list(ENABLED.items())
    if not left:
        return 0
    bad = 0
    for name, path in left:
        ok, why = set_pageheap(gflags, path, False)
        if not ok:
            bad += 1
        if verbose:
            print("  [收尾] %-36s 关页堆 %s（%s）"
                  % (name, "成功" if ok else "**失败**", why), flush=True)
    return bad


def install_guards(gflags):
    """除了 `finally`，再挂一层 atexit 和信号 —— 页堆残留代价太大。"""
    atexit.register(lambda: disable_all(gflags, verbose=False))

    def handler(sig, _frame):
        disable_all(gflags)
        signal.signal(sig, signal.SIG_DFL)
        os.kill(os.getpid(), sig)

    for s in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(s, handler)
        except (ValueError, OSError):
            pass


# ---------------------------------------------------------------------------
# 跑一个 exe
# ---------------------------------------------------------------------------

def tail_of(path, max_bytes=65536):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - max_bytes))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def run_exe(exe_path, workdir, timeout):
    """跑一个 exe，返回 (退出码 或 None, 是否超时, 输出尾部)。

    stdout/stderr 落**临时文件**而不是管道：测试里有的会起子进程
    （`test_process_func`），管道会被孙进程握着不放，超时时 `communicate()`
    会跟着卡死。落文件就没这问题，超时再用 `taskkill /T` 收整棵进程树。
    """
    fd, log = tempfile.mkstemp(prefix="pageheap_", suffix=".log")
    os.close(fd)
    rc, timed_out = None, False
    try:
        with open(log, "wb") as fh:
            p = subprocess.Popen([exe_path], cwd=workdir, stdin=subprocess.DEVNULL,
                                 stdout=fh, stderr=subprocess.STDOUT)
            try:
                rc = p.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)],
                               capture_output=True)
                try:
                    p.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
        return rc, timed_out, tail_of(log)
    finally:
        try:
            os.remove(log)
        except OSError:
            pass


def u32(rc):
    return rc & 0xFFFFFFFF


def rc_str(rc):
    """崩溃码印成 `0x…`（`subprocess` 在 Windows 上给的是无符号 DWORD，
    直接用十进制读会是一串没意义的数字），普通退出码印成十进制。"""
    if rc is None:
        return "None"
    u = u32(rc)
    return "0x%08X" % u if u >= 0x80000000 else str(rc)


def classify(rc, timed_out):
    """→ (类别, 说明)。类别取值：ok / timeout / crash / stale / fail。"""
    if timed_out:
        return "timeout", "超时（页堆下可能只是太慢，也可能是死锁 —— 要人看）"
    if rc == 0:
        return "ok", ""
    u = u32(rc)
    if u in STALE_CODES:
        return "stale", STALE_CODES[u]
    if u in CRASH_CODES:
        return "crash", CRASH_CODES[u]
    if u >= 0x80000000:
        return "crash", "崩溃，未归类"
    return "fail", "普通失败（**不是崩溃**）"


def interesting_lines(text):
    pat = re.compile(r"FAIL|失败|断言|assert|Error|错误|terminate|异常")
    hits = [l.rstrip() for l in text.splitlines() if pat.search(l)]
    return hits[-12:]


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    ap.add_argument("--exe", default="",
                    help="只跑这些（逗号分隔，可带可不带 .exe 与 test_ 前缀）")
    ap.add_argument("--baseline-timeout", type=int, default=BASELINE_TIMEOUT_S)
    ap.add_argument("--pageheap-timeout", type=int, default=PAGEHEAP_TIMEOUT_S)
    ap.add_argument("--gflags", default=None, help="gflags.exe 路径")
    ap.add_argument("--no-baseline", action="store_true",
                    help="跳过裸跑基线（**不建议**：没有基线，「红」就分不清是"
                         "页堆抓到的还是本来就红的）")
    ap.add_argument("--self-test", action="store_true",
                    help="转调 run_tcp_client_dtor_mutation.py，证明门禁自己还咬得住")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    if not os.path.isdir(tree):
        print("找不到构建目录 %s" % tree)
        return 3

    gflags = find_gflags(args.gflags)
    if gflags is None:
        print("找不到 gflags.exe（用 --gflags 指定）。它随 Windows SDK 的"
              "调试工具装，通常在\n"
              r"  C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\gflags.exe")
        return 3
    if winreg is None:
        print("这个脚本依赖 winreg 回查注册表，非 Windows 上跑不了")
        return 3
    print("gflags: %s" % gflags)

    if args.self_test:
        print("\n==== 门禁自检（转调 run_tcp_client_dtor_mutation.py）====")
        rc, out = run([sys.executable, "-u",
                       os.path.join(ROOT, "tests", "tools",
                                    "run_tcp_client_dtor_mutation.py"),
                       "--tree", args.tree], timeout=7200)
        print(out[-4000:])
        print("自检结论：%s（rc=%s）" % ("咬得住" if rc == 0 else "**没咬住**", rc))
        if rc != 0:
            return 3

    all_exes = find_test_exes(tree)
    if not all_exes:
        print("在 %s/tests 下没找到 test_*.exe —— 先建一遍" % tree)
        return 3
    if args.exe:
        want = set()
        for tok in args.exe.split(","):
            tok = tok.strip()
            if not tok:
                continue
            want.add(tok if tok.startswith("test_") else "test_" + tok)
        all_exes = [e for e in all_exes if e[0] in want]
        if not all_exes:
            print("--exe 过滤之后一个都不剩")
            return 3

    exe_dirs = sorted({os.path.dirname(e[1]) for e in all_exes})
    print("构建目录: %s" % tree)
    print("用例: %d 个，输出目录 %d 个" % (len(all_exes), len(exe_dirs)))

    n, total = sync_dlls(tree, exe_dirs)
    print("DLL 刷新: %d 份（覆盖 %d 种）" % (n, total))
    if not any(f.lower() == "uvcpp.dll" for f in os.listdir(exe_dirs[0])):
        print("**输出目录里没有 uvcpp.dll** —— 先建一遍再来")
        return 3
    print("uvcpp.dll md5: %s" % md5(os.path.join(exe_dirs[0], "uvcpp.dll")))

    # 上一个脚本崩在半路留下的页堆，先收掉
    leftovers = [e for e in all_exes if pageheap_on(os.path.basename(e[1]))]
    if leftovers:
        print("\n发现 %d 个残留页堆，先关掉：" % len(leftovers))
        for _name, path, _wd in leftovers:
            ENABLED[os.path.basename(path)] = path
        disable_all(gflags)

    install_guards(gflags)
    result = 3
    try:
        result = _run_gate(args, gflags, all_exes)
    finally:
        bad = disable_all(gflags)
        still = [n for n, _p in ENABLED.items()]
        print("\n页堆清理: %s%s"
              % ("全部关掉" if (bad == 0 and not still) else
                 "**有 %d 个没关掉**: %s" % (bad, still),
                 "" if (bad == 0 and not still) else " —— 手工 gflags -p /disable 收尾"))
        print("注册表复查: %d/%d 已确认关闭"
              % (sum(1 for e in all_exes
                     if not pageheap_on(os.path.basename(e[1]))), len(all_exes)))
        if bad or still:
            result = 3
    return result


def _run_gate(args, gflags, all_exes):
    base = {}

    if not args.no_baseline:
        print("\n==== 第一遍：裸跑基线（不开页堆）====")
        t0 = time.time()
        for name, path, wd in all_exes:
            rc, to, text = run_exe(path, wd, args.baseline_timeout)
            kind, why = classify(rc, to)
            base[name] = kind
            print("  %-38s %s" % (name, "OK" if kind == "ok" else "%s %s" % (kind, why)),
                  flush=True)
            if kind == "stale":
                print("      **先重建再跑门禁**：刷 DLL 之后还是 %s 就是真的缺 DLL"
                      % rc_str(rc))
                return 3
        print("裸跑用时 %.1fs" % (time.time() - t0))
        red = [n for n, k in base.items() if k != "ok"]
        if red:
            print("\n**基线不是全绿（%d 个）**：%s" % (len(red), ", ".join(red)))
            print("基线红的话这条门禁证明不了任何事 —— 先修基线，或以 --no-baseline "
                  "明确接受「分不清」的读法。")
            return 3

    print("\n==== 第二遍：开完整页堆重跑（会显著变慢，预计数倍于上面）====")
    print("（每个用例：开页堆 → 回查注册表 → 跑 → finally 关页堆 → 回查）\n")
    t0 = time.time()
    caught, ordinary, elapsed, stale = [], [], [], []
    for i, (name, path, wd) in enumerate(all_exes, 1):
        ok, why = set_pageheap(gflags, path, True)
        if not ok:
            print("[%2d/%d] %-34s **页堆没设上**（%s）" % (i, len(all_exes), name, why))
            return 3
        try:
            rc, to, text = run_exe(path, wd, args.pageheap_timeout)
        finally:
            okoff, whyoff = set_pageheap(gflags, path, False)
        kind, why2 = classify(rc, to)
        elapsed.append((name, to))

        if kind == "stale":
            stale.append((name, rc, why2, text))
            print("[%2d/%d] %-34s rc=%s %s  ← **门禁自身问题**"
                  % (i, len(all_exes), name, rc_str(rc), why2), flush=True)
            continue
        if kind == "ok":
            print("[%2d/%d] %-34s rc=0 OK%s"
                  % (i, len(all_exes), name, "" if okoff else "（**页堆没关掉！**）"),
                  flush=True)
            continue

        is_new = base.get(name) == "ok"  # `--no-baseline` 时 base 是空的
        why_not = "" if is_new else (
            "（基线也红，不算抓到）" if base else "（没跑基线，分不清）")
        if kind == "timeout":
            ordinary.append((name, rc, why2, text))
            print("[%2d/%d] %-34s **超时**%s"
                  % (i, len(all_exes), name, why_not), flush=True)
        elif kind == "crash" and is_new:
            caught.append((name, rc, why2, text))
            print("[%2d/%d] %-34s rc=%s %s  ← **抓到**"
                  % (i, len(all_exes), name, rc_str(rc), why2), flush=True)
        else:
            ordinary.append((name, rc, why2, text))
            print("[%2d/%d] %-34s rc=%s %s%s"
                  % (i, len(all_exes), name, rc_str(rc), why2, why_not),
                  flush=True)

    print("页堆用时 %.1fs" % (time.time() - t0))

    print("\n==== 汇总 ====")
    print("用例 %d 个；抓到 %d 个；其他异常 %d 个；过期 DLL %d 个"
          % (len(all_exes), len(caught), len(ordinary), len(stale)))

    for label, group in (("抓到（基线绿、页堆崩 —— 这就是门禁该拦的）", caught),
                         ("其他异常（超时 / 普通失败 / 没有基线可比）", ordinary),
                         ("过期 DLL（先重建，不是缺陷）", stale)):
        if not group:
            continue
        print("\n-- %s --" % label)
        for name, rc, why, text in group:
            print("  %-38s rc=%s %s" % (name, rc_str(rc), why))
            for l in interesting_lines(text)[-6:]:
                print("      | %s" % l)

    if caught:
        print("\n**门禁不通过**：%d 个用例在完整页堆下崩溃，裸跑却绿。"
              % len(caught))
        print("这类崩溃就是释放后使用（或越界写）—— 排查起点是崩溃栈，"
              "必要时对着这条命令重跑单个用例：")
        print("   python -u tests/tools/run_pageheap_gate.py --tree %s --exe %s"
              % (args.tree, ",".join(n for n, _r, _w, _t in caught[:3])))
        print("要栈就把页堆开着跑一次 cdb（`kb 25`，同目录的 gflags 旁边就是它）：")
        print("   gflags -p /enable <exe> /full  →  "
              "cdb -g -G -c \"sxe av; g; kb 25; q\" <exe>  →  gflags -p /disable <exe>")
    if ordinary:
        print("\n**门禁不通过**：另有 %d 个用例在页堆下不是绿的。" % len(ordinary))
        print("超时多是「页堆下太慢」，但**也可能是死锁/活锁** —— 别当噪声跳过；"
              "普通失败说明堆布局变了、行为也跟着变，同样要查。")
    if stale:
        print("\n**门禁自身失败**：%d 个用例是过期 DLL（先重建）。" % len(stale))
    if caught or ordinary:
        return 1
    if stale:
        return 3
    print("\n**门禁通过**：%d 个用例裸跑与完整页堆下都是绿的。" % len(all_exes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
