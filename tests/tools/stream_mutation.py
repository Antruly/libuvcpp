#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Phase 3a（流式请求管线 + 100-continue + 工作池上限）—— 变异验证驱动。

跑两棵树合成的覆盖面：`build-webapp` 里的
    test_web_stream_func           （http 层：认领 / 100-continue / 提前 413）
    test_web_app_stream_func       （框架层：流式请求对象 / 停顿保护）
    test_web_app_worklimit_func    （工作池准入）

每条变异都**指定期望被哪一组抓住**，脚本会核对"红的确实是那一组" —— 只看
"退出码非 0"的话，"碰巧红了别的用例"也会被记成一次成功。

判定规格（两条，缺一不可）：
  1. 退出码非 0（有断言红了）；
  2. 红的那一组里包含**期望的那一组**（不是别的组在红）。

抓不住的**照实记**，并且按 `assert-test-preconditions` 的规矩：先用探针区分
"等价变异"和"真实覆盖缺口"，不许直接写"等价"。
"""
import os
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8')

ROOT = r"D:\public\libuvcpp\libuvcpp"
BUILD = "build-webapp"

HTTP = os.path.join(ROOT, "src", "web", "uvcpp_http_server.cpp")
STREAM = os.path.join(ROOT, "src", "webapp", "uvcpp_web_stream.cpp")
APP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")
CONN = os.path.join(ROOT, "src", "webapp", "uvcpp_web_connection.cpp")
LIMIT = os.path.join(ROOT, "src", "webapp", "uvcpp_web_work_limit.cpp")
STATIC = os.path.join(ROOT, "src", "webapp", "uvcpp_web_static.cpp")

TESTS = [
    ("test_web_stream_func",      ["claim", "expect", "413"]),
    ("test_web_app_stream_func",  ["handler_runs_before_body", "chunks_in_order",
                                   "on_end_then_response", "pause_is_real",
                                   "stall_killed", "slow_but_progressing",
                                   "abort_mid_stream", "progress_reported"]),
    ("test_web_app_worklimit_func", ["accounting", "static_integration",
                                     "does_not_queue"]),
]

# (名字, 文件, 原文, 变异后, 期望被抓的组)
MUTATIONS = [
    # --- 认领 ---------------------------------------------------------
    ("S1 claim_hook_never_called", HTTP,
     "        sh = stream_claim_(pctx->stream_request, client);",
     "        sh = http_stream_handler();  /* MUTATION */",
     "claim_matched / handler_runs_before_body"),

    # --- 投递 ---------------------------------------------------------
    ("S2 body_not_delivered", APP,
     "        ctx->stream_deliver(data, len);",
     "        (void)data; (void)len;  /* MUTATION: 丢掉 body */",
     "chunks_in_order"),

    ("S3 end_not_delivered", APP,
     "        ctx->stream_end();",
     "        /* MUTATION: 不投递 END */",
     "on_end_then_response"),

    # --- 链的挂起 ------------------------------------------------------
    ("S4 next_not_retained", APP,
     "        if (s != nullptr) s->bind_resume(next);",
     "        (void)s;  /* MUTATION: 不替用户留住 next */",
     "handler_runs_before_body"),

    # --- 背压 ---------------------------------------------------------
    ("S5 pause_does_not_stop_reads", STREAM,
     "  c->read_pause();",
     "  /* MUTATION: pause() 不真的停读 */",
     "pause_is_real"),

    # --- 停顿保护 ------------------------------------------------------
    ("S6 streaming_timed_by_request_start", CONN,
     "  if (it->second.streaming) return it->second.last_read_ms;",
     "  /* MUTATION: 流式也按整段预算计时 */",
     "slow_but_progressing_survives"),

    ("S7 streaming_still_exempt", APP,
     "    if (inflight_.find(id) != inflight_.end() && !registry_.is_streaming(id))",
     "    if (inflight_.find(id) != inflight_.end())  /* MUTATION: 流式无停顿保护 */",
     "stall_killed"),

    # --- 100-continue / 417 / 提前 413 ----------------------------------
    ("S8 no_100_continue", HTTP,
     '      enqueue_write(*pctx, client, "HTTP/1.1 100 Continue\\r\\n\\r\\n");',
     "      /* MUTATION: 不发 100 Continue */",
     "expect_continue"),

    ("S9 unknown_expect_not_417", HTTP,
     "  reject_early(ctx, client, http_status::EXPECTATION_FAILED);",
     "  return true;  /* MUTATION: 未知 Expect 不回 417 */",
     "expect_unknown_417"),

    ("S10 no_early_413", HTTP,
     "  reject_early(ctx, client, http_status::PAYLOAD_TOO_LARGE);",
     "  return false;  /* MUTATION: 不做提前 413 */",
     "early_413_by_content_length"),

    # --- 工作池上限 -----------------------------------------------------
    ("S11 cap_never_rejects", LIMIT,
     "    if (lim != 0 && cur >= lim) return false;",
     "    /* MUTATION: 上限永不拒绝 */",
     "accounting / does_not_queue"),

    ("S12 static_no_admission", STATIC,
     "  if (im->work_limit && !im->work_limit->acquire()) {",
     "  if (false) {  /* MUTATION: 静态路径不做准入 */",
     "static_integration(503)"),

    ("S13 slot_never_released", STATIC,
     "        if (j->self->work_limit) j->self->work_limit->release();",
     "        /* MUTATION: 名额只拿不还 */",
     "static_integration(连发 5 次)"),
]


def run(cmd, cwd=ROOT, timeout=1800):
    return subprocess.run(cmd, cwd=cwd, shell=True, timeout=timeout,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def build_and_test(label):
    """重建 uvcpp + 三个用例目标，返回 {用例名: (rc, 失败行)}。"""
    targets = " ".join(t for t, _ in TESTS)
    r = run(f"cmake --build {BUILD} --config Release --parallel 4 "
            f"--target uvcpp {targets}")
    out = r.stdout.decode("utf-8", "replace")
    errs = [ln for ln in out.splitlines()
            if "error C" in ln or "error LNK" in ln or "error MSB" in ln]
    if errs:
        print(f"    BUILD FAILED ({len(errs)} errors)")
        for ln in errs[:3]:
            print("      " + ln.strip())
        return None
    # DLL 必须重新拷 —— 只核对 ctest 会跑陈旧二进制（本仓库踩过三次）。
    run(f"cmake --build {BUILD} --config Release --target copy_test_dlls")

    results = {}
    for name, _ in TESTS:
        exe = os.path.join(ROOT, BUILD, "tests", "functional", "Release",
                           name + ".exe")
        if not os.path.exists(exe):
            results[name] = (-1, ["<exe missing>"])
            continue
        r = run(f'"{exe}"', cwd=os.path.dirname(exe), timeout=600)
        txt = r.stdout.decode("utf-8", "replace")
        # 打印用的是 unitbuf，逐行判断即可。失败行 [FAIL] 前面最近的那个
        # `[casename]` 就是它所属的组。
        cur = ""
        fails = []
        for ln in txt.splitlines():
            s = ln.strip()
            if s.startswith("[") and "]" in s and not s.startswith("[FAIL]"):
                cur = s
            elif s.startswith("[FAIL]"):
                fails.append(cur + " :: " + s)
        results[name] = (r.returncode, fails)
    return results


def main():
    print("### 基线")
    base = build_and_test("baseline")
    if base is None:
        print("基线构建失败，中止")
        return 2
    bad = {k: v for k, v in base.items() if v[0] != 0}
    for k, v in base.items():
        print(f"    {k}: rc={v[0]}")
    if bad:
        print("基线不绿，中止")
        return 2

    originals = {}
    for path in {m[1] for m in MUTATIONS}:
        with open(path, "rb") as f:
            originals[path] = f.read()

    results = []
    for name, path, old, new, expect in MUTATIONS:
        src = originals[path].decode("utf-8")
        n = src.count(old)
        if n != 1:
            print(f"### {name}: 变异**施加失败**（锚点匹配 {n} 次）")
            results.append((name, "未施加", expect, {}))
            continue

        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(src.replace(old, new))
        try:
            r = build_and_test(name)
        finally:
            with open(path, "wb") as f:
                f.write(originals[path])

        if r is None:
            results.append((name, "构建失败", expect, {}))
            continue

        red = {k: v[1] for k, v in r.items() if v[0] != 0}
        caught = "抓住" if red else "没抓住"
        results.append((name, caught, expect, red))
        print(f"### {name}: {caught}  (期望 {expect})")
        for tname, fl in red.items():
            print(f"      {tname}: {len(fl)} 条")
            for ln in fl[:4]:
                print("        " + ln)

    print("\n### 还原校验")
    allsame = True
    for path, orig in originals.items():
        with open(path, "rb") as f:
            same = f.read() == orig
        allsame = allsame and same
        print(f"    {os.path.basename(path)}: {'字节一致' if same else '!!! 不一致 !!!'}")
    dirty = run("grep -rn MUTATION src/").stdout.decode("utf-8", "replace").strip()
    print(f"    MUTATION 残留: {dirty if dirty else '无'}")

    # **还原源码不等于还原构建树**：上面只把 .cpp 写回去了，树里留着的仍是
    # **最后一条变异**的二进制。脚本退出后谁在这个树上跑用例，跑的都是变异版
    # —— 表现是"全红"，而盘上源码是好的，极易被误判成回归（本次实测：
    # `test_web_app_worklimit_func` 20/20 全红，失败签名正是 S13 的
    # 「名额只拿不还」）。本仓库的老陷阱 `stale-test-exe-looks-like-crash`
    # 是"二进制旧了"，这里是"二进制脏了"，方向相反但同一族。
    # 所以：**重建 + 复跑**，跑完这棵树才算是干净的。
    print("\n### 还原后重建 + 复跑（把变异版二进制换回当前源码）")
    after = build_and_test("restore")
    if after is None:
        print("    重建失败")
        allsame = False
    else:
        for k, v in after.items():
            print(f"    {k}: rc={v[0]}")
        still_red = {k: v for k, v in after.items() if v[0] != 0}
        if still_red:
            print("    !!! 还原后仍有用例红 —— 源码没还原干净，或构建树仍是脏的")
            allsame = False
        else:
            print("    全绿：构建树已与盘上源码一致")

    print("\n### 汇总")
    for name, got, expect, red in results:
        # 判据：抓住了，**且**红的是期望的那一组（按关键字粗匹配）。
        keys = [k.strip().split("(")[0] for k in expect.split("/")]
        hit = False
        for tname, fl in red.items():
            for k in keys:
                if k and (k in tname or any(k in ln for ln in fl)):
                    hit = True
        if got == "抓住":
            mark = "OK " if hit else "?? 红了但不是期望的组"
        elif got in ("未施加", "构建失败"):
            mark = "-- "
        else:
            mark = "?? 没抓住"
        print(f"    {mark} {name:<36} {got:<6} (期望 {expect})")

    return 0 if allsame and not dirty else 1


if __name__ == "__main__":
    sys.exit(main())
