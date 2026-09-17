#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
第 10 条（`~uvcpp_tcp_client`）收口的变异验证。

被测的两件事都在 `src/net/uvcpp_tcp_client.cpp`：

  (a) 析构里**不许有睡眠**：原先那条"关闭 + 泵"的路是
      `while (!close_done) { run(NOWAIT); 看墙钟; sleep(1ms); }`
      —— `sleep` 在条件复查**之前**，所以每一次"连着、没关就析构"都至少
      1 毫秒，一次也躲不掉。判据 `test_destructor_has_no_sleep`：30 轮取
      **最小值**（绝对墙钟，不是"A 比 B 快"），阈值 1.0ms 正好是改前的硬下界。

  (b) **在自己的回调里析构**不许崩：`test_delete_client_in_read_cb` 两条腿
      （收数据时删 / 对端断开时删）。判据只能靠 **PageHeap**（把释放过的块
      直接 unmap）—— 裸跑时那块内存还在、读到的还是旧值，所以不崩。

三个动作各钉一处，所以打五个变异：

  变异                              预期
  --------------------------------  --------------------------------------------
  M1  数据路径那处"栈上拷一份再调"去掉      腿 1 崩（PageHeap）
  M2  对端断开那处的同一句去掉             腿 2 崩（PageHeap）
  M3  回调之后那句存活令牌判断去掉          腿 2 崩（`fire_close_callbacks()`
                                          在已释放的对象上跑）
  M4  泵循环换回"1ms 睡眠"的老样子          计时腿 FAIL（**不开 PageHeap**，
                                          证明这条判据自己不依赖 PageHeap）
  M5  开头的 `is_running()` 守卫关掉        崩（PageHeap）—— 重入 `uv_run`、
                                          当场 `delete loop_` / `delete tcp_`

M1/M2 为什么必须分开打：两处**长得很像但钉的不是同一件事**。M1 那条腿里，
回调返回之后只剩 `uvcpp_free_bytes(base)`（局部量）；M2 那条腿里，回调返回
之后还有 `fire_close_callbacks()`（全是成员）。

gflags 的两个坑（都踩过）：
  - 在 Git Bash 里直接写 `gflags -p /enable …` 会被 MSYS 的路径转换把
    `/enable`、`/full` 变成盘符路径，命令**静默不生效**（退出码还是 0）。
    所以这里用 `subprocess` 的列表形式调，不经 shell。
  - 开/关之后必须**回查注册表确认**（`GlobalFlag & 0x02000000`），
    跑完必须关掉并再次回查 —— 页堆残留会让后面所有测试都慢一个量级。

用法：python -u tests/tools/run_tcp_client_dtor_mutation.py [--tree build-webapp]
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

try:
    import winreg
except ImportError:  # 非 Windows
    winreg = None

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLIENT_CPP = os.path.join(ROOT, "src", "net", "uvcpp_tcp_client.cpp")

IFEO = (r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"
        r"\Image File Execution Options")
FLG_HEAP_PAGE_ALLOCS = 0x02000000
EXE = "test_tcp_client_func.exe"

# label, 原文, 替换, 要不要开 PageHeap
MUTATIONS = [
    ("M1 数据路径不拷闭包",
     CLIENT_CPP,
     "            uvcpp_net_read_cb cb = net_read_cb_;\n"
     "            try {\n"
     "              cb(*this, r);\n",
     "            try {\n"
     "              net_read_cb_(*this, r); /* MUTATION */\n",
     True),
    ("M2 断开路径不拷闭包",
     CLIENT_CPP,
     "              ::std::shared_ptr<char> life = alive_token();\n"
     "              uvcpp_net_read_cb       cb   = net_read_cb_;\n"
     "              try {\n"
     "                cb(*this, r);\n",
     "              ::std::shared_ptr<char> life = alive_token();\n"
     "              try {\n"
     "                net_read_cb_(*this, r); /* MUTATION */\n",
     True),
    ("M3 不看存活令牌",
     CLIENT_CPP,
     "              if (token_alive(life)) fire_close_callbacks();",
     "              if (token_alive(life) || true) fire_close_callbacks();"
     " /* MUTATION */",
     True),
    ("M4 泵换回 1ms 睡眠",
     CLIENT_CPP,
     "        for (int i = 0; i < 64 && !close_done; ++i) {\n"
     "          if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);\n"
     "        }\n",
     "        while (!close_done) { /* MUTATION */\n"
     "          if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);\n"
     "          std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
     "        }\n",
     False),
    ("M5 关掉 is_running 守卫",
     CLIENT_CPP,
     "  if (loop_ != nullptr && loop_->is_running()) {\n",
     "  if (false && loop_ != nullptr && loop_->is_running()) { /* MUTATION */\n",
     True),
]

RUN_TIMEOUT_S = 300
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


def run(cmd, cwd=ROOT, timeout=RUN_TIMEOUT_S):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "*** TIMEOUT ***"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree):
    rc, out = run(["cmake", "--build", tree, "--config", "Release",
                   "--parallel", "4"], timeout=BUILD_TIMEOUT_S)
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    return rc, nerr, out


def sync_dll(tree):
    """`copy_test_dlls` 在本机因为 `pwsh.exe` 找不到而静默失效。"""
    src = os.path.join(tree, "Release", "uvcpp.dll")
    with open(src, "rb") as f:
        data = f.read()
    n = 0
    for base, _dirs, files in os.walk(tree):
        if "uvcpp.dll" not in files:
            continue
        dst = os.path.join(base, "uvcpp.dll")
        if os.path.abspath(dst) == os.path.abspath(src):
            continue
        with open(dst, "wb") as f:
            f.write(data)
        n += 1
    return n


def pageheap_on(exe_path):
    if winreg is None:
        return None
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, IFEO + "\\" + EXE) as k:
            raw = winreg.QueryValueEx(k, "GlobalFlag")[0]
    except OSError:
        return False
    # gflags 写进去的是 REG_SZ（"0x02000000"），不是 DWORD。
    if isinstance(raw, str):
        raw = int(raw, 0)
    return bool(raw & FLG_HEAP_PAGE_ALLOCS)


def gflags(args):
    exe = os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "Windows Kits", "10", "Debuggers", "x64", "gflags.exe")
    if not os.path.exists(exe):
        return None, "gflags.exe 不在"
    return run([exe] + args, timeout=60)


def set_pageheap(exe_path, on):
    """开/关之后**回查注册表**确认 —— gflags 静默失效过一次（见文件头）。"""
    args = ["-p", "/enable" if on else "/disable", exe_path]
    if on:
        args.append("/full")
    rc, out = gflags(args)
    if rc is None:
        return False, out
    state = pageheap_on(exe_path)
    return (state == on), "rc=%s 注册表 page heap=%s" % (rc, state)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")
    exe_path = os.path.join(exe_dir, EXE)

    before = read_text(CLIENT_CPP)
    sum0 = md5(CLIENT_CPP)
    summary = []
    restored_ok = False
    tail_ok = False

    try:
        cases = [("基线（未变异，裸跑）", None, False),
                 ("基线（未变异，PageHeap）", None, True)] + \
                [(m[0], m[1:4], m[4]) for m in MUTATIONS]
        for label, mut, heap in cases:
            write_text(CLIENT_CPP, before)
            if mut is not None:
                path, old, new = mut[0], mut[1], mut[2]
                # 源文件是 CRLF（`newline=""` 读进来时 \r\n 是原样的），
                # 而脚本里的锚点按 \n 写 —— 按文件自己的行尾对齐，别混着比。
                cur = read_text(path)
                nl = "\r\n" if "\r\n" in cur else "\n"
                old = old.replace("\n", nl)
                new = new.replace("\n", nl)
                assert cur.count(old) == 1, f"{label}: anchor 不唯一/找不到 {old!r}"
                write_text(path, cur.replace(old, new, 1))

            okh, why = set_pageheap(exe_path, heap)
            if not okh:
                print(f"[{label}] PageHeap 设置失败：{why}")
                raise SystemExit(4)

            rc, nerr, out = build(args.tree)
            if rc != 0 or nerr:
                print(f"[{label}] 构建失败 rc={rc} errors={nerr}")
                print(out[-3000:])
                raise SystemExit(3)
            n = sync_dll(tree)

            erc, eout = run([exe_path], cwd=exe_dir)
            fails = [s.strip() for s in
                     re.findall(r"\[functional tcp_client\] (\S+ FAIL.*)", eout)]
            timed = [s.strip() for s in
                     re.findall(r"(destructor_no_sleep best=.*)", eout)]
            if heap:
                verdict = "**崩了**（抓）" if erc != 0 else "没崩 —— 没抓住！"
            else:
                verdict = ("**计时腿红了**（抓）" if erc != 0
                           else "全绿 —— 没抓住！")
            summary.append((label, "rc=%s %s" % (erc, verdict)))
            print(f"\n[{label}] dll 同步 {n} 处；{'PageHeap 开' if heap else '裸跑'}"
                  f" → rc={erc} {verdict}", flush=True)
            for t in timed:
                print(f"    {t}")
            for f in fails:
                print(f"    {f}")
    finally:
        write_text(CLIENT_CPP, before)
        restored_ok = md5(CLIENT_CPP) == sum0
        rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")])
        print(f"\n源码按字节还原: {'是' if restored_ok else '否 —— 有问题！'}")
        print(f"grep 残留: {out.strip() or '(无)'}")

        rc, nerr, _out = build(args.tree)
        sync_dll(tree)
        # 收尾：PageHeap **必须关掉**并回查（残留会让后面所有测试慢一个量级）
        okh_off, why_off = set_pageheap(exe_path, False)
        print(f"PageHeap 已关: {'是' if okh_off else '否 —— 有问题！'}（{why_off}）")
        clean_rc, _eo = run([exe_path], cwd=exe_dir)
        tail_ok = (rc == 0 and nerr == 0 and clean_rc == 0 and okh_off
                   and restored_ok)
        print(f"还原后重建 rc={rc} errors={nerr}；裸跑复跑 rc={clean_rc}（应 0）")

    print("\n==== 汇总 ====")
    for label, verdict in summary:
        print(f"  {label:26s} {verdict}")
    return 0 if tail_ok else 1


if __name__ == "__main__":
    sys.exit(main())
