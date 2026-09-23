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
  M7  注入"1 号格 `init_loop_state()` 失败"（**对照组，带故障**）
                                → **判别式**：那条"有循环没退出"的判据**不响**
  M8  M7 + 拿掉 `init_rc` 那道检查
                                → **判别式**：同一条判据**响**（`start()` 返 0，
                                   而那一格没有 `async` ⇒ 停机退化成 `join()` 的
                                   有界兜底 ⇒「条循环没退出」那条 ERROR）
                                   这条路上**没有自然触发器**（`uv_async_init`
                                   只在 OOM / 非法循环上失败）⇒ 故障必须先注入；
                                   M7/M8 在**那条判据响不响**上的差就是 `init_rc`
                                   那道检查的牙。

  **M7/M8 看的是"哪条判据响了"，不是"红不红"**（2026-09-23 实测改的判据）
  -------------------------------------------------------------------
  原先这里写的是"M7 应绿、M8 应红"，**这条判据本身是坏的**：注入的故障是"1 号格
  初始化失败"，而 1 号格是**同 exe 里每一个 n=2 用例**都要用的 ⇒ 注入之后
  `start()` 对**每一条** n=2 用例都返非 0，其它用例成片红（实测 8 条）。
  于是 M7 与 M8 在"红不红"上分不开 —— 而"分不开的对照组"等于没有对照组。
  （附带损伤里**没有**「有循环没退出」，因为没有任何一条 n=2 的循环真正跑起来过；
  这正是判别式能立住的原因，也是它必须显式判一遍的理由：别再让"红绿之差"
  这种看着像判据的东西回来。）
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

    # M7 / M8：钩子丢 `init_loop_state()` 返回码那条路**没有自然触发器**
    # （`uv_async_init` 只在 OOM / 非法循环上失败），所以这里得**先把故障注进去**
    # 才谈得上判据。两段式的写法就在这儿：`old`/`new` 是列表时按顺序逐段替换。
    #
    # M7 是**对照组（带故障）**：只注入、不改实现 ⇒ 修好的代码如实返非 0。
    # M8 = M7 + 拿掉 `init_rc` 那道检查 ⇒ `start()` 返 0 而那一格没有 `async`，
    # 停机退化成 `join()` 的有界兜底 ⇒ 「有循环没退出」那条 ERROR 出现。
    #
    # **这一对看的是判别式（`MUTATION_SIGNATURES`），不是红绿**：注入的故障把
    # 同 exe 里每一条 n=2 用例都打红了，红绿分不开这两条 —— 理由见文件头。
    ("M7 注入一格初始化失败（对照组：目标判据应**不响**）",
     APP_CPP,
     "      s.init_rc = init_loop_state(index, loop);",
     "      s.init_rc = (index == 1) ? UV_ENOMEM  /* MUTATION: 注入故障 */\n"
     "                                : init_loop_state(index, loop);"),

    ("M8 注入 + 拿掉 init_rc 检查（目标判据应**响**）",
     APP_CPP,
     ["      s.init_rc = init_loop_state(index, loop);",
      "        return teardown_failed_start(loops_[i]->init_rc);"],
     ["      s.init_rc = (index == 1) ? UV_ENOMEM  /* MUTATION: 注入故障 */\n"
      "                                : init_loop_state(index, loop);",
      "        (void)0;  /* MUTATION: 丢掉 init_rc，启动照常返 0 */"]),
]

# 有牙的判据 vs 附带损伤：M7/M8 的**判别式**是"目标判据响没响"，不是"红不红"。
#
# 关键词取自用例里那几句判据的原文（`tests/functional/web_app_multiloop_func.cpp`
# 里 `count_containing(kJoinIncomplete)` 旁边），措辞几处不同，但都带这六个字。
JOIN_INCOMPLETE_SIG = "有循环没退出"

# 标签前缀 -> {"must"/"must_not": [子串]}。没登记的就是老规矩："红 == 抓住"。
# 登记了的：红不红**不判**（附带损伤是多条 n=2 用例一起红），只判目标判据。
MUTATION_SIGNATURES = {
    "M7": {"must_not": [JOIN_INCOMPLETE_SIG]},
    "M8": {"must": [JOIN_INCOMPLETE_SIG]},
}

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
    sig_bad = False

    # 锚点先就地全核对一遍：**在动构建之前**。有一个对不上就没有必要开始跑
    # （每个变异一次构建，白跑一轮是分钟级的）。
    #
    # `old` 是列表时是**分段变异**（注入故障 + 拆守卫那种，见 M8）：列表里每一段
    # 各自都要在本文件里唯一。路径仍只有一个（`m[1]`），所以 `files` 不用改。
    for label, path, old, _new in picks:
        for o in (old if isinstance(old, list) else [old]):
            n = before[path].count(o)
            assert n == 1, ("锚点必须**唯一**：%s 里 %r 命中 %d 次"
                            % (os.path.basename(path), o[:60], n))

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
                olds = old if isinstance(old, list) else [old]
                news = new if isinstance(new, list) else [new]
                assert len(olds) == len(news), label
                cur, _c = read_text(path)
                for o, nn in zip(olds, news):
                    assert cur.count(o) == 1, label
                    cur = cur.replace(o, nn, 1)
                write_text(path, cur, crlf[path])

            rc, nerr, out = build(args.tree)
            if rc != 0 or nerr:
                print("[%s] 构建失败 rc=%d errors=%d" % (label, rc, nerr))
                print(out[-3000:])
                raise SystemExit(3)
            n = sync_dll(tree)

            crc, fails, cout = run_case(exe_dir)
            verdict = ("**没抓住**" if not fails
                       else "抓（%d 条）" % len(fails))

            # 判别式：只对登记过的变异判，而且**不判红绿**（理由见文件头 ——
            # 注入的故障把同 exe 里每一条 n=2 用例都打红了，红绿分不开 M7/M8）。
            sig = MUTATION_SIGNATURES.get(label.split()[0])
            sig_note = ""
            if sig:
                bad = []
                for s in sig.get("must", []):
                    if not any(s in f for f in fails):
                        bad.append("该响的没响（`%s`）" % s)
                for s in sig.get("must_not", []):
                    if any(s in f for f in fails):
                        bad.append("不该响的响了（`%s`）" % s)
                if bad:
                    sig_bad = True
                    sig_note = "；判别式 **✘ %s**" % "；".join(bad)
                else:
                    sig_note = "；判别式 ✔（目标判据%s，%d 条是附带损伤）" % (
                        "响了" if sig.get("must") else "没响", len(fails))

            summary.append((label, verdict + sig_note))
            print("\n[%s] 同步 dll %d 处 → %s%s"
                  % (label, n, verdict, sig_note), flush=True)
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
        print("  %-48s %s" % (label, verdict))
    if sig_bad:
        print("  !! 有判别式没过：把它当判据看，别只看红绿")
    return 0 if (restored_ok and tail_rc == 0 and not sig_bad) else 1


if __name__ == "__main__":
    sys.exit(main())
