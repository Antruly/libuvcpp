#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
webapp 多循环（`uvcpp_web_app::set_loops(n)`）那批判据的变异验证。

被测的判据在 `tests/functional/web_app_multiloop_func.cpp` 里。本脚本逐条把
**实现里的那个守卫**拆掉，看对应用例是不是真的红 —— 没红就说明那条断言是摆设
（本仓吃过一次这个亏：`n == 1` 等价的重构单独落地时，变异**恒绿**）。

  M1  拆掉 `stop()` 的逐槽位扇出（只投 0 号）
                                → 判据 2 红，红法是那条 ERROR
  M2  WS 停机改成"只让 0 号发 Close"
                                → 判据 6/7 红（1 号循环上的会话收不到 Close 帧）
  M3  WS 会话分片键从 `client->loop_index()` 换成常量 0
                                → 判据 7 红（会话记在 0 号分片上）
  M4  `connection_count_at(i)` 不看 i，一律答 0 号
                                → 红（逐格之和 ≠ 总数）
                                   **第一次跑没抓住**：判据原本写在"16 条请求全
                                   跑完"之后，那时总数是 0，`0 == 0+0` 对坏实现
                                   一样成立 —— 恒真的判据。已把采样点挪进处理
                                   函数（连接活着的时刻），前提 `at1 >= 1` 自
                                   己先断言。补齐判据之后重跑本表才把它抓住。
  M5  静态缓存 `put()` 里"同一个键重插时先扣掉旧字节"那句去掉
                                → `test_static_cache_reput_bytes` 红
                                   （**这条用例是补出来的**：只有"12 次请求"那条
                                   时 M5 **没抓住** —— `put()` 只在**未命中**
                                   那条分支上被调（`uvcpp_web_static.cpp:1150`），
                                   而 12 次请求是"1 次未命中 + 11 次命中"，
                                   一步都进不了那句。缺口是先被本脚本证明、
                                   才补的用例 —— 这正是本脚本存在的意义。）

M0 是**对照组**（未变异）。它必须全绿 —— 没有它的话，"M1 红了"也可能只是这棵树
本来就跑不过。

M1 的红法：为什么不是"挂住"
--------------------------
`join()` 的等待是**有界**的（`shutdown_grace_ms` 默认 3000 + `kJoinSlackMs`
5000），坏实现 8 秒就返回并打一条 ERROR。所以用例里"`join()` 在 15 秒内返回"
那条断言**在坏实现上照样绿** —— 有牙的是 `capture_sink` 抓的那条
「join() 已等 N ms，仍有 M 条循环没退出」。本脚本的存在就是为了证明这一点：
它要是抓不住 M1，那条判据就该重写，而不是收进"全绿"里。

诚实行：锁本身拿不到牙
--------------------
M5 证明的是**共享缓存的字节账**有牙，**不是** `Impl::mu_` 那把锁有牙 —— 本仓
构建里没有 sanitizer（没有 TSan/ASan 档位），"少拿一次锁"没有确定性的观测方式。
那条修复能给的只有单写者论证 + 重复跑的压力形状。**"跑了 N 遍没崩"不是判据**，
别在记录里升级成"验过了"。这一条在下面 M6 里被显式跑一遍并如实报"抓不住"。

用法：python -u tests/tools/multiloop_mutation.py [--tree build-h2] [--only M5,M1]
      （会改源码再还原；跑完核对"源码按字节还原: 是"）
      `--only` 只跑指定的那几条（M0 对照组始终跑）；补判据之后重跑全表是分钟级 ×6，
      这条筛子是用来把"改完立刻验一次"压到两条构建的。
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# 变异前的原文件**另存成文件**，还原从这份拷回 —— 不是从内存里的字符串还。
# 本仓踩过：脚本被 `| head` 截断、或进程被 KILL 时 `finally` 根本不跑，内存里那份
# 原件随进程消失，仓库里留下变异过的源码。落成文件之后，即使脚本死于非命，
# `.good` 还在，人工拷回来就行。
BACKUP_DIR = os.path.join(os.path.dirname(ROOT), "_probe",
                          "multiloop_mutation_backup")
APP_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")
WSS_CPP = os.path.join(ROOT, "src", "web", "uvcpp_ws_server.cpp")
STATIC_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_static.cpp")

EXE = "test_web_app_multiloop_func.exe"

# label, 文件, 原文, 替换
MUTATIONS = [
    ("M3 WS 分片键钉成 0 号",
     WSS_CPP,
     "  uvcpp_ws_sessions* shard = shard_for(client->loop_index());",
     "  uvcpp_ws_sessions* shard = shard_for(0);  /* MUTATION */"),

    ("M4 connection_count_at 不看下标",
     APP_CPP,
     "  return loops_[static_cast<size_t>(loop_index)]->registry.size();",
     "  (void)loop_index;\n"
     "  return loops_[0]->registry.size();  /* MUTATION */"),

    ("M5 缓存重插不扣旧字节",
     STATIC_CPP,
     "      bytes -= old->second.data->size();\n",
     "      /* MUTATION: 不扣旧字节 */\n"),

    ("M2 WS 停机只让 0 号发 Close",
     APP_CPP,
     "      ws_server_->close_sessions_of_loop(slot.index,\n",
     "      ws_server_->close_sessions_of_loop(0,  /* MUTATION */\n"),

    ("M1 拆掉停机扇出（只投 0 号）",
     APP_CPP,
     "    post([this]() { begin_shutdown(); }, static_cast<int>(i));",
     "    if (i == 0) post([this]() { begin_shutdown(); });  /* MUTATION */"),

    ("M6 静态缓存 put() 不拿锁（预期抓不住）",
     STATIC_CPP,
     "    std::lock_guard<std::mutex> lk(mu_);\n\n"
     "    const std::map<std::string, cache_entry>::iterator old = cache.find(key);",
     "    /* MUTATION: 不拿锁 */\n\n"
     "    const std::map<std::string, cache_entry>::iterator old = cache.find(key);"),
]

# 每个变异只跑这一条用例：它就是这个批次的判据文件。
RUN_TIMEOUT_S = 300


def read_text(p):
    """读成**统一 LF** 的文本，外加"这个文件原本是不是 CRLF"。

    为什么不是 `open(newline="")` 直接读：本仓的 `.cpp` 两种换行都有 ——
    `uvcpp_web_static.cpp` 是 CRLF，`uvcpp_web_app.cpp` 是 LF。锚点按 `\\n` 写，
    在 CRLF 的文件上一个都命中不了（`count` 断言会拦下来，但那一轮就是白跑）。
    比对前统一成 `\\n`，写回时按原样还原，字节级由 `md5` 复核。

    混合换行（既有 `\\r\\n` 又有裸 `\\n`）的文件直接拒绝：那种文件"还原"这句话
    本身就不成立了。
    """
    with open(p, "rb") as f:
        raw = f.read()
    text = raw.decode("utf-8")
    crlf = b"\r\n" in raw
    if crlf:
        assert raw.count(b"\r\n") == raw.count(b"\n"), "%s 混合换行，别用它" % p
        text = text.replace("\r\n", "\n")
    return text, crlf


def write_text(p, s, crlf):
    with open(p, "w", encoding="utf-8",
              newline=("\r\n" if crlf else "\n")) as f:
        f.write(s)


def md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def good_path(p):
    return os.path.join(BACKUP_DIR, os.path.basename(p) + ".good")


def save_good(p):
    """原文件按字节另存到**仓库外**。

    已有同名 `.good` 就**拒绝开始**：那说明上一次跑没能还原，此时磁盘上这个文件
    很可能正是变异体 —— 把它当"原件"存下去，等于给变异盖了章。
    """
    g = good_path(p)
    if os.path.exists(g):
        raise SystemExit(
            "!! %s 已存在：上一次跑没有还原成功。先核 %s 的内容，确认无误后"
            "删掉那个 .good 再跑。" % (g, p))
    os.makedirs(BACKUP_DIR, exist_ok=True)
    with open(p, "rb") as f:
        data = f.read()
    with open(g, "wb") as f:
        f.write(data)


def restore_good(p):
    with open(good_path(p), "rb") as f:
        data = f.read()
    with open(p, "wb") as f:
        f.write(data)


def run(cmd, cwd=ROOT, timeout=RUN_TIMEOUT_S):
    """超时必须接住：本仓有一条判据（"挂住"）的红法就是超时，
    不接 `TimeoutExpired` 会让脚本死在第一个变异上。"""
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "*** TIMEOUT ***"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree):
    # 只建这两个目标：全部变异都在 .cpp 里，动不到头文件，所以不需要整棵树。
    # `--parallel 2` 是日常档位（4 在 5 个 web_app TU 上会撞 MSVC 的 C1060）。
    rc, out = run(["cmake", "--build", tree, "--config", "Release",
                   "--parallel", "2",
                   "--target", "uvcpp", "test_web_app_multiloop_func"])
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    return rc, nerr, out


def sync_dll(tree):
    """把测试侧那份 uvcpp.dll 同步成 `Release/` 里刚编出来的。

    `copy_test_dlls` 是 `add_custom_target(... ALL)`，只有**默认构建**会跑它；
    本脚本走的是 `--target uvcpp test_web_app_multiloop_func`，那条依赖链里没有
    它，于是测试侧的 DLL 会停在上一版。不手动同步就会拿**上一版库**跑变异，
    结果整体错位一格 —— 这条在 `run_idle_mutation.py` 上踩过，照抄它的做法。
    """
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


def run_case(exe_dir):
    """返回 (rc, FAIL 行列表)。

    红的形状有两种，都要认：`check()` 打的 `[FAIL] <判据>`，以及
    `require_within` 等不到时打的 `-> FAIL: 等不到「…」`（它随后 `_Exit(2)`）。
    只认前者的话，"等不到"这条最像挂死的红会被当成"没红"。
    """
    path = os.path.join(exe_dir, EXE)
    rc, out = run([path], cwd=exe_dir, timeout=RUN_TIMEOUT_S)
    fails = [s.strip() for s in re.findall(r"\[[Ff][Aa][Ii][Ll]\]\s*(.*)", out)]
    fails += [s.strip() for s in re.findall(r"-> FAIL:\s*(.*)", out)]
    # 崩溃（0xc0000005 / 139）也是一种红，但要说清楚它不是断言红。
    if rc != 0 and not fails:
        fails = ["（无 [FAIL] 行；rc=%d，多半是崩溃）" % rc]
    return rc, fails, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-h2")
    # 补一条判据 / 调一个变异之后，重跑**全表**是分钟级 ×6。这个筛子只跑其中
    # 几条，但 **M0 对照组永远在**：没有对照组，"它红了"就没法归因到变异上。
    ap.add_argument("--only", default=None,
                    help="只跑标签以这些前缀开头的变异（逗号分隔），"
                         "如 --only M5；M0 对照组始终跑")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")

    picks = MUTATIONS
    if args.only:
        want = [s.strip() for s in args.only.split(",") if s.strip()]
        picks = [m for m in MUTATIONS if any(m[0].startswith(w) for w in want)]
        assert picks, "没有标签匹配 --only %r" % args.only

    files = sorted({m[1] for m in picks})
    texts = {p: read_text(p) for p in files}
    before = {p: t for p, (t, _c) in texts.items()}
    crlf = {p: c for p, (_t, c) in texts.items()}
    sums = {p: md5(p) for p in files}
    summary = []
    restored_ok = False
    tail_rc = 1

    # 锚点先就地全核对一遍：**在动构建之前**。有一个对不上就没有必要开始跑
    # （每个变异一次构建，白跑一轮是分钟级的）。
    for label, path, old, _new in picks:
        n = before[path].count(old)
        assert n == 1, ("锚点必须**唯一**：%s 里 %r 命中 %d 次"
                        % (os.path.basename(path), old[:60], n))

    for p in files:
        save_good(p)
    print("原文件另存：%s" % BACKUP_DIR, flush=True)

    try:
        cases = [("M0 对照组（未变异）", None)] + [(m[0], m[1:]) for m in picks]
        for label, mut in cases:
            for p in files:
                restore_good(p)
            if mut is not None:
                path, old, new = mut[0], mut[1], mut[2]
                cur, _c = read_text(path)
                assert cur.count(old) == 1, label
                write_text(path, cur.replace(old, new, 1), crlf[path])

            rc, nerr, out = build(args.tree)
            if rc != 0 or nerr:
                print("[%s] 构建失败 rc=%d errors=%d" % (label, rc, nerr))
                print(out[-3000:])
                raise SystemExit(3)
            n = sync_dll(tree)

            crc, fails, cout = run_case(exe_dir)
            verdict = ("**没抓住**" if not fails
                       else "抓（%d 条）" % len(fails))
            summary.append((label, verdict))
            print("\n[%s] 同步 dll %d 处 → %s" % (label, n, verdict), flush=True)
            print("    用例 rc=%d" % crc)
            for f in fails:
                print("        %s" % f)

            # 对照组必须全绿 —— 这是"上面那些红是变异造成的"这条归因的前提。
            if label.startswith("M0") and fails:
                print("    !! 对照组就是红的：本脚本的归因不成立，先修好这棵树")
    finally:
        for p in files:
            restore_good(p)
        restored_ok = all(md5(p) == sums[p] for p in files)
        print("\n源码按字节还原: %s（从 %s 拷回）"
              % ("是" if restored_ok else "否 —— 有问题！", BACKUP_DIR))
        if restored_ok:
            for p in files:
                os.remove(good_path(p))
        else:
            print("   !! .good 备份**不删**，人工处理：%s" % BACKUP_DIR)
        rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")])
        print("grep 残留: %s" % (out.strip() or "(无)"))
        rc, nerr, _ = build(args.tree)
        sync_dll(tree)
        crc, fails, _out = run_case(exe_dir)
        tail_rc = 0 if (rc == 0 and nerr == 0 and crc == 0 and not fails) else 1
        print("还原后重建 rc=%d errors=%d；用例复跑 rc=%d（应全 0）"
              % (rc, nerr, crc))

    print("\n==== 汇总 ====")
    for label, verdict in summary:
        print("  %-34s %s" % (label, verdict))
    return 0 if (restored_ok and tail_rc == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
