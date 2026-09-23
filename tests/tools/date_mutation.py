#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
`Date` 响应头那批判据的变异验证（任务 1）。

被测判据：
  - `tests/functional/web_http_date_func.cpp`（新用例，5 条）
  - 三处**既有**用例里补的那条断言（h1 流式 / h2 整包 / h2 流式）

五类变异，缺一类就留一个洞：

  一、**格式化本身**（M1/M2/M5）—— 表写错、走 locale、发本地时间。这一类无论
      如何都会被 `date_format_known_values` 那 7 组硬编码向量抓住；它存在的意义
      是证明**那 7 组向量的牙**，以及"格式判据"与"新鲜度判据"是分开的两件事。
  二、**缓存**（M3/M4）—— 按秒复用失效、观测计数器恒返 0。M4 是**反假绿**：
      计数器恒返 0 会让"同一秒 1001 次调用 0 次格式化"那种断言**恒绿**，所以
      用例里先有一条"新线程第一次调用必须 +1"。
  三、**语义**（M6）—— 处理函数设过的不许覆盖。
  四、**接线**（M7/M8/M9/M10）—— 四处注入点各撤一处。**这一类是本节的重点**：
      没有它，"四个注入点"完全可能是"一个真点 + 三个空操作"，而所有机制类的
      变异照样被抓住（它们改的是 `uvcpp_http_date.cpp`，与注入点无关）。
  五、**判据自身**（M11）—— 抓"修法被撤销、而判据的名字没变"那种形态：
      `head:` 那条判据改成"摘掉 `date` 再比"之后配了一条反向对照，M11 把
      `drop_header_line` 改成恒等，此时它**退回成原来的 `gh == hh`**（每秒
      15% 假红的那条）。名字没变、还在跑，只有那条对照能戳破。

判据（沿用 `limits_mutation.py`）：
  1. 期望的那条**子用例**红（不是"全量里红了别的"）—— 本仓用例 `main()` 逐条
     累加 `ok` 但不早退，所以这里用 exe 的 `argv[1]` 过滤器**单跑那条**，再单独
     跑一次全量记附带损伤；
  2. 红的**是不是那条断言** —— 抠 `[FAIL]`/`[err]` 行，核对期望的签名出现在里面。
     只有"红了"而不核签名的话，一个构建期错误或者别的断言红都会被记成"抓住"。

M0 是对照组（未变异），必须全绿 —— 没有它的话，"M1 红了"也可能只是这棵树本来
就跑不过。

M0 允许**复跑一次**（但这条记录的**归因换过一次**，读的人注意）
-------------------------------------------------------------
首跑实测到过一次红：`test_web_app_stream_resp_func`。**我第一版把它解释错了** ——
写成"`quiet_stream_not_killed` 那个 300 ms 看门狗在 `-j4` 满负荷下误杀自己"，
依据只是"这个文件里有一个已知的时序看门狗"。后来去读 `LastTest.log` 里的红行，
红的是**另一条**子用例：

    [FAIL] head: HEAD 的头部与 GET 逐字节相同（含 transfer-encoding）

这是 `Date` 改动**真带出来的**问题，不是抖动：那条用例把 GET 与 HEAD 两条响应
（两次独立请求、两个时刻）的**整个头部块逐字节**比，而 `Date` 是挂钟值，一跨秒
边界就必然不同 ⇒ 每秒约 15% 概率红（`-j4` 把两次请求的间隔拉长，概率更高）。
单独跑永远绿，正是因为间隔只有几毫秒。修法在**用例**那边（摘掉 `Date` 再比，
另加"两边都得有合格的 `Date`"与一条反向对照），不在框架里。

教训是**报告纪律**的：红了要报"红在哪一条子用例"，不能靠"这个文件里有什么已知
弱点"去猜 —— 猜出来的解释会写进注释，然后被下一个人当真。

M0 的机制不变：首跑红就复跑一次，两次都红才停，**并且把首跑的红如实打出来**
—— 不能让复跑把一次真实的红悄悄吃掉（那正是"处方把证据吃掉"）。这条也**不进
"已知 flaky 清单"**：它根本不是 flaky，是回归。

为什么要 h2 树
-------------
M8/M9/M10 覆盖的两处注入点在 `#if UVCPP_NGHTTP2_ENABLE` 里面。普通树
（`v130`）上那三个用例走 `#else` 的 SKIP 桩、**SKIP 也 PASS**（本仓已知坑），
于是撤掉注入点**一个字都不会红**。所以本脚本默认在 h2 树上跑。

用法：
  python3 -u tests/tools/date_mutation.py [--tree /home/zwy/work/b/v130h2] [--only M7]
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DATE_CPP = os.path.join(ROOT, "src", "web", "uvcpp_http_date.cpp")
SRV_CPP = os.path.join(ROOT, "src", "web", "uvcpp_http_server.cpp")

T_DATE = "test_web_http_date_func"
T_STREAM = "test_web_stream_response_func"
T_H2SRV = "test_web_ssl_h2_server_func"
T_H2APP = "test_web_ssl_app_h2_func"
T_STREAM_RESP = "test_web_app_stream_resp_func"

# 四处注入点的唯一上下文（`http_ensure_date(resp);` 在本文件里出现 4 次，
# 单靠那一行定位不到；跟着的下一行各不相同，拿它当唯一键）。
SITE1 = "  http_ensure_date(resp);\n\n#if UVCPP_NGHTTP2_ENABLE\n"
SITE2 = "  http_ensure_date(resp);\n\n  ctx.out_streaming = true;\n"
SITE3 = "    http_ensure_date(resp);\n    ctx.h2->send_headers(stream_id, resp);\n"
SITE4 = "  http_ensure_date(resp);\n\n  auto sit = ctx.h2_streams.find(stream_id);\n"


def unplug(site):
    """把一处注入点换成等长的注释，保持后面那行不动（不换就编不过）。"""
    return site.replace("http_ensure_date(resp);",
                        "/* MUTATION: 这个出口不补 Date */", 1)


MUTATIONS = [
    # ---- 一、格式化本身 ----
    #
    # M1 是**月份表错位**（去掉首项，Nov 变成 Oct），不是把某项写成大写 ——
    # 大写那版**抓不住**：7 组硬编码向量里一个 `Jan` 都没落在比较上（`Jan` 只在
    # 1970/2100 两组里出现，而它们比的是整串），错位才打在对比位上。
    ("M1", DATE_CPP,
     'const char* const k_mon_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",\n',
     'const char* const k_mon_names[] = {"Feb", "Mar", "Apr", "May", "Jun", "Jul",\n',
     T_DATE, "date_format_known_values", "[date]",
     "月份名表错位一格（1994-11-06 会说成 Oct）—— 硬编码向量必须抓住"),
    #
    # M2 必须**真的走 locale**，所以要整块换成 `strftime`：把格式串改成 `%a/%b`
    # 给 `snprintf` 是**抓不住**的（`snprintf` 不认这两个转换，只会原样吐 `a` /
    # `b`，那连 `date_format_known_values` 都过不了 —— 它考的不是 locale 那件事）。
    # 换成 `strftime` 之后：C locale 下它输出**正确**结果（7 组向量照样绿），
    # 只有 `date_is_locale_immune` 在 `setlocale(LC_TIME, "")` 之后变红 ——
    # 这条变异正好把"格式对不对"与"走不走 locale"两件事分开。
    ("M2", DATE_CPP,
     '''  const int n = std::snprintf(buf, sizeof(buf),
                              "%s, %02d %s %04d %02d:%02d:%02d GMT",
                              k_wday_names[tmv.tm_wday % 7], tmv.tm_mday,
                              k_mon_names[tmv.tm_mon % 12], tmv.tm_year + 1900,
                              tmv.tm_hour, tmv.tm_min, tmv.tm_sec);''',
     '''  /* MUTATION: 改回走 locale 的 strftime */
  const int n = static_cast<int>(::strftime(
      buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmv));''',
     T_DATE, "date_is_locale_immune", "[date]",
     "格式化改回 `strftime`（`%a`/`%b` 走 locale）—— 只有 locale 那条判据该红"),
    ("M5", DATE_CPP,
     "#if defined(_WIN32)\n  if (::gmtime_s(&tmv, &t) != 0) return std::string();\n#else\n  if (::gmtime_r(&t, &tmv) == NULL) return std::string();\n#endif\n",
     "#if defined(_WIN32)\n  if (::localtime_s(&tmv, &t) != 0) return std::string();\n#else\n  if (::localtime_r(&t, &tmv) == NULL) return std::string();\n#endif\n",
     T_DATE, "date_format_known_values", "[date]",
     "发**本地时间**而不是 GMT（GMT 后缀照旧）—— 硬编码向量必须抓住"),

    # ---- 二、缓存 ----
    ("M3", DATE_CPP,
     "  if (s.valid && s.sec == now) return s.text;\n",
     "  if (false) return s.text;  /* MUTATION: 每响应都重新格式化 */\n",
     T_DATE, "date_cache_is_per_second", "[date]",
     "按秒复用失效（同一秒 1001 次调用会格式化 1001 次）"),
    ("M4", DATE_CPP,
     "  return format_counter().load(std::memory_order_relaxed);\n",
     "  return 0;  /* MUTATION: 计数器恒返 0 */\n",
     T_DATE, "date_cache_is_per_second", "[date]",
     "观测计数器恒返 0 —— 反假绿的那半（'第一次必须 +1'）必须把它抓住"),

    # ---- 三、语义 ----
    ("M6", DATE_CPP,
     '  if (resp.has_header("date")) return;\n',
     '  /* MUTATION: 处理函数设过的也覆盖掉 */\n',
     T_DATE, "ensure_date_semantics", "[date]",
     "`http_ensure_date` 无条件覆盖（处理函数自己设的 date 被冲掉）"),

    # ---- 四、接线：四处注入点各撤一处 ----
    ("M7", SRV_CPP, SITE1, unplug(SITE1),
     T_DATE, "date_on_the_wire_h1", "[date]",
     "撤 site 1（h1 整包 `send_response`）"),
    ("M8", SRV_CPP, SITE2, unplug(SITE2),
     T_STREAM, "stream_head_only", "[err]",
     "撤 site 3（h1 流式 `begin_stream`）"),
    ("M9", SRV_CPP, SITE4, unplug(SITE4),
     T_H2SRV, "", "[FAIL]",
     "撤 site 2（h2 整包 `send_h2_response`）"),
    ("M10", SRV_CPP, SITE3, unplug(SITE3),
     T_H2APP, "", "[FAIL]",
     "撤 site 4（h2 流式 `begin_stream`）"),

    # ---- 五、反向对照**自己**的牙 ----
    #
    # `head:` 那条判据改成"摘掉 `date` 再比逐字节"之后，它配了一条反向对照
    # （`drop_header_line(gh, "date") != gh`）。M11 把 `drop_header_line` 改成
    # 恒等 —— 此时"摘掉之后逐字节相同"就**退回成原来的 `gh == hh`**，也就是
    # 每秒 15% 假红的那条旧判据。判据的名字一个字都没变，所以只有那条对照能
    # 把它抓出来；对照哪天被人删掉，这条变异就会变成"没抓住"。
    ("M11", os.path.join(ROOT, "tests", "functional",
                         "web_app_stream_resp_func.cpp"),
     'std::string drop_header_line(const std::string& head, const std::string& name) {\n'
     '  const std::string lower = name + ":";\n',
     'std::string drop_header_line(const std::string& head, const std::string& name) {\n'
     '  /* MUTATION: 恒等 —— 一行都不摘，等价于把判据退回 `gh == hh` */\n'
     '  (void)name;\n'
     '  return head;\n'
     '  const std::string lower = name + ":";\n',
     T_STREAM_RESP, "head", "[FAIL]",
     "`drop_header_line` 退化成恒等 —— 只有那条反向对照该红"),
]


def read_text(p):
    with open(p, "rb") as f:
        return f.read()


def write_bytes(p, b):
    with open(p, "wb") as f:
        f.write(b)


def md5(p):
    return hashlib.md5(read_text(p)).hexdigest()


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def exe_of(tree, name):
    return os.path.join(tree, "tests", "functional", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="/home/zwy/work/b/v130h2")
    ap.add_argument("--only", default=None)
    ap.add_argument("--jobs", default="7")
    args = ap.parse_args()

    tree = os.path.abspath(args.tree)

    files = sorted({m[1] for m in MUTATIONS})
    backups = {p: read_text(p) for p in files}
    before = {p: md5(p) for p in files}

    def build():
        rc, out = run(["cmake", "--build", tree, "-j", args.jobs])
        nerr = len(re.findall(r"\berror:", out))
        return rc, nerr, out

    print("=" * 72)
    print("M0 对照组（未变异）")
    print("=" * 72)
    rc, nerr, out = build()
    print("  build rc=%d errors=%d" % (rc, nerr), flush=True)
    if rc != 0 or nerr:
        print(out[-3000:])
        return 3
    rc, out = run(["ctest", "--test-dir", tree, "-j", "4"])
    print("  全量 ctest: %s (rc=%d)"
          % ("ALL PASS" if rc == 0 else "FAIL", rc), flush=True)
    if rc != 0:
        reds = re.findall(r"\d+ - (\S+) \(Failed", out)
        print("  首跑红了: %s —— 复跑一次（见文档里那条抖动说明）"
              % ", ".join(reds), flush=True)
        rc, out = run(["ctest", "--test-dir", tree, "-j", "4"])
        print("  复跑: %s (rc=%d)" % ("ALL PASS" if rc == 0 else "FAIL", rc),
              flush=True)
    if rc != 0:
        print(out[-3000:])
        print("  基线不绿（两次都是），后面的判定没有意义 —— 停下")
        return 3

    results = []
    for name, path, old, new, binary, filt, sig, desc in MUTATIONS:
        if args.only and name != args.only:
            continue

        src = read_text(path)
        if src.count(old.encode("utf-8")) != 1:
            results.append((name, "锚点不唯一/缺失", binary, desc))
            print("\n[%s] 锚点在树上命中 %d 次（要 1 次）—— 施加失败"
                  % (name, src.count(old.encode("utf-8"))), flush=True)
            continue

        write_bytes(path, src.replace(old.encode("utf-8"), new.encode("utf-8"), 1))
        print("\n[%s] %s" % (name, desc), flush=True)

        rc, nerr, out = build()
        if rc != 0 or nerr:
            write_bytes(path, src)
            results.append((name, "构建失败", binary, desc))
            print("  构建失败（rc=%d errors=%d）—— 不算抓住" % (rc, nerr), flush=True)
            print(out[-1500:])
            continue

        exe = exe_of(tree, binary)
        # (1) 单跑期望的那条（有过滤器就带上，拿到子用例粒度）
        cmd = [exe] + ([filt] if filt else [])
        rc_scoped, out_scoped = run(cmd)
        # (2) 全量，记附带损伤
        rc_full, out_full = run(["ctest", "--test-dir", tree, "-j", "4"])

        write_bytes(path, src)  # 立刻还原

        reds = out_scoped.count("-> FAIL")
        has_sig = sig in out_scoped
        caught = rc_scoped != 0 and has_sig

        # 附带损伤：全量里红掉的用例名（ctest 的 `***Failed` 行）
        collateral = re.findall(r"\d+ - (\S+) \(Failed", out_full)

        verdict = "抓住" if caught else ("红了但签名不对" if rc_scoped != 0
                                        else "没抓住")
        results.append((name, verdict, binary, desc))
        print("  单跑 %s%s: rc=%d、-> FAIL 计 %d、签名 %r 出现=%s → %s"
              % (binary, ("[" + filt + "]") if filt else "", rc_scoped, reds,
                 sig, has_sig, verdict), flush=True)
        if collateral and collateral != [binary]:
            print("  附带损伤: %s" % ", ".join(sorted(set(collateral))), flush=True)
        if rc_scoped != 0:
            for l in out_scoped.splitlines():
                if l.strip().startswith(("[FAIL]", "[err]", "[date]")):
                    print("    " + l.strip(), flush=True)

    # ---- 还原后重建：不做完这棵树就是脏的 ----
    print("\n" + "=" * 72)
    print("还原")
    print("=" * 72)
    for p, b in backups.items():
        write_bytes(p, b)
    ok = all(md5(p) == before[p] for p in files)
    print("  源码按字节还原: %s" % ("是" if ok else "否 —— 有问题！"))
    rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")])
    print("  grep MUTATION 残留: %s" % (out.strip() or "(无)"))

    rc, nerr, out = build()
    rc2, out2 = run(["ctest", "--test-dir", tree, "-j", "4"])
    print("  还原后重建 rc=%d errors=%d，全量 ctest=%s"
          % (rc, nerr, "ALL PASS" if rc2 == 0 else "FAIL"))
    # **红了必须点名**。这一处原先只印 ALL PASS / FAIL，于是实测到的那两次红
    # 一个用例名都没留下 —— 工具把唯一能定位的证据吞了（"处方把证据吃掉"的
    # 另一种形状）。复跑一次，两次都红才当成真红报出来。
    if rc2 != 0:
        first = re.findall(r"\d+ - (\S+) \(Failed", out2)
        print("  首跑红: %s —— 复跑一次" % (", ".join(first) or "(没抠到名字)"),
              flush=True)
        rc2, out2 = run(["ctest", "--test-dir", tree, "-j", "4"])
        again = re.findall(r"\d+ - (\S+) \(Failed", out2)
        print("  复跑: %s%s（首跑红=%s 复跑红=%s）"
              % ("ALL PASS" if rc2 == 0 else "FAIL",
                 "" if rc2 != 0 else " ⇒ 判定为抖动，且**这是复跑把它吃掉了**"
                                    "，别据此认为它以后不会真红",
                 ",".join(first) or "-", ",".join(again) or "-"))

    print("\n" + "=" * 72)
    print("汇总")
    print("=" * 72)
    for name, verdict, binary, desc in results:
        print("  %-4s %-14s %-30s %s" % (name, verdict, binary, desc))
    bad = [r for r in results if r[1] != "抓住"]
    print("\n  抓住 %d/%d" % (len(results) - len(bad), len(results)))
    return 0 if ok and rc2 == 0 and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
