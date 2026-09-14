#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WebSocket 框架接线（enable_wss / websocket / 升级分派 / 停机）—— 变异验证驱动。

对每个变异：改一处源码 → 重建 uvcpp + 测试可执行文件 → 拷 DLL → 跑用例 →
记录结果 → 按字节还原。判定只看退出码（0 = 没抓住）。

用法：
    python tests/tools/webapp_ws_mutation.py            # 全部
    python tests/tools/webapp_ws_mutation.py M1 M3      # 只跑前缀匹配的几条
"""
import os
import subprocess
import sys
import hashlib

sys.stdout.reconfigure(encoding='utf-8')

ROOT = r"D:\public\libuvcpp\libuvcpp"
BUILD = "build-webapp"
TEST = "test_web_app_ws_func"

APP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")
SESS = os.path.join(ROOT, "src", "web", "uvcpp_ws_sessions.cpp")

# (名字, 文件, 原文, 替换, 期望被哪组抓住)
MUTATIONS = [
    # ---- M1：升级不分路径，一律接受（未命中就发第一个 handler）----
    ("M1 upgrade_ignores_path",
     APP,
     """    web_route_match m = ws_router_.match(http_method::HTTP_GET, path);
    if (m.result == web_route_result::MATCHED && m.pattern != nullptr) {
      auto it = ws_handlers_.find(*m.pattern);
      if (it != ws_handlers_.end()) {
        hit = true;
        dispatch_ws(m, it->second, req, client);
      }
    }""",
     """    web_route_match m = ws_router_.match(http_method::HTTP_GET, path);
    if (m.pattern == nullptr) {   /* MUTATION: 不分路径，一律接受 */
      m.result = web_route_result::MATCHED;
      m.pattern = &ws_handlers_.begin()->first;
    }
    if (m.result == web_route_result::MATCHED && m.pattern != nullptr) {
      auto it = ws_handlers_.find(*m.pattern);
      if (it == ws_handlers_.end()) it = ws_handlers_.begin();
      hit = true;
      dispatch_ws(m, it->second, req, client);
    }""",
     "unknown_path_falls_through"),

    # ---- M2：升级后不做闲置豁免（idle_sweep 照扫）----
    ("M2 no_idle_exemption",
     APP,
     "    if (upgraded_.find(id) != upgraded_.end()) continue;",
     "    /* MUTATION: 不豁免 */",
     "idle_timeout_exempt"),

    # ---- M3：adopt() 不装终结回调（会话永不回收）----
    ("M3 adopt_no_retire_fn",
     SESS,
     "  c->set_retire_callback([this](uvcpp_ws_connection* s) { on_retired(s); });",
     "  (void)0;   /* MUTATION: 不装终结回调 */",
     "session_recycled"),

    # ---- M4：停机时不给会话发 Close 帧 ----
    ("M4 no_close_on_shutdown",
     APP,
     "      ws_server_->close_all_sessions(ws_close_code::GOING_AWAY);",
     "      /* MUTATION: 停机不发 Close */",
     "app_shutdown_closes_sessions"),

    # ---- M5：缺 Sec-WebSocket-Key 也照样升级 ----
    ("M5 missing_key_still_upgrades",
     APP,
     '  if (http_get_header(req.headers, "sec-websocket-key").empty()) {',
     '  if (false) {   /* MUTATION: 缺 key 也放行 */',
     "bad_handshake"),

    # ---- M6：websocket() 不隐式 enable_wss() ----
    ("M6 no_implicit_enable_wss",
     APP,
     "  enable_wss();",
     "  /* MUTATION: 不隐式 enable_wss */",
     "route_dispatch / path_params"),
]


def run(cmd, cwd=ROOT, timeout=900):
    return subprocess.run(cmd, cwd=cwd, shell=True, timeout=timeout,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def build_and_test(label):
    r = run(f"cmake --build {BUILD} --config Release --parallel 4 --target uvcpp "
            f"{TEST}")
    out = r.stdout.decode("utf-8", "replace")
    errs = [ln for ln in out.splitlines()
            if "error C" in ln or "error LNK" in ln or "error MSB" in ln]
    if errs:
        print(f"    BUILD FAILED ({len(errs)} errors)")
        for ln in errs[:3]:
            print("      " + ln.strip())
        return None
    run(f"cmake --build {BUILD} --config Release --target copy_test_dlls")
    exe = os.path.join(ROOT, BUILD, "tests", "functional", "Release", TEST + ".exe")
    if not os.path.exists(exe):
        print("    exe missing")
        return None
    r = run(exe, cwd=os.path.dirname(exe), timeout=300)
    out = r.stdout.decode("utf-8", "replace")
    # 用例名打在每条日志前面（`[casename] ...`），FAIL 行本身不带用例名，所以
    # 得一边扫一边记住"当前在用例里"。不这么做，汇总表就只能写"抓住"而说不出
    # "被哪一组抓住" —— 上一轮的报告就是这么缺了一整列。
    fails, current = [], "?"
    for ln in out.splitlines():
        stripped = ln.strip()
        if stripped.startswith("[") and "]" in stripped:
            current = stripped[1:stripped.index("]")]
        if "[FAIL]" in stripped:
            fails.append((current, stripped))
    return r.returncode, fails


def main():
    # 基线
    print("### 基线")
    base = build_and_test("baseline")
    print("    ", base)
    if base is None or base[0] != 0:
        print("基线不绿，中止")
        return 2

    # 记录原始字节，用于逐字节还原校验
    originals = {}
    for path in (APP, SESS):
        with open(path, "rb") as f:
            originals[path] = f.read()

    results = []
    only = sys.argv[1:]
    for name, path, old, new, expect in MUTATIONS:
        if only and not any(name.startswith(p) for p in only):
            continue
        src = originals[path].decode("utf-8")
        if src.count(old) != 1:
            print(f"### {name}: 变异**施加失败**（锚点匹配 {src.count(old)} 次）")
            results.append((name, "未施加", expect, [], False))
            continue

        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(src.replace(old, new))

        try:
            r = build_and_test(name)
        finally:
            with open(path, "wb") as f:
                f.write(originals[path])

        if r is None:
            results.append((name, "构建失败", expect, [], False))
            continue
        rc, fails = r
        caught = "抓住" if rc != 0 else "没抓住"
        # 去重、保序：哪些用例真的报了失败（这才是"被哪组抓住"）
        cases = []
        for c, _ in fails:
            if c not in cases:
                cases.append(c)
        # 期望列写的是用例名，用来核对"抓住它的正是不是它该被抓的那一组"
        hit_expect = any(e.strip() in cases for e in expect.split("/")) if caught == "抓住" else False
        results.append((name, caught, expect, cases, hit_expect))
        print(f"### {name}: {caught} (rc={rc}, {len(fails)} 条失败)")
        print(f"      被抓住的用例: {', '.join(cases) if cases else '-'}")
        if caught == "抓住" and not hit_expect:
            print(f"      !! 但期望的那一组（{expect}）没在其中 —— 要查是不是碰巧红的")
        for c, ln in fails[:4]:
            print(f"      [{c}] {ln}")

    # 还原校验
    print("\n### 还原校验")
    ok = True
    for path, blob in originals.items():
        with open(path, "rb") as f:
            same = f.read() == blob
        print(f"    {os.path.basename(path)}: {'字节一致' if same else '!!! 不一致 !!!'}")
        ok = ok and same
    dirty = run("grep -rn MUTATION src/").stdout.decode("utf-8", "replace").strip()
    print(f"    MUTATION 残留: {dirty if dirty else '无'}")

    print("\n### 汇总")
    for name, got, expect, cases, hit in results:
        # "抓住"要和"是不是**该抓它的那一组**抓的"分开看：只写"抓住"会把
        # "碰巧红了别的用例"也记成一次成功。
        mark = "OK " if (got != "抓住" or hit) else "?? "
        print(f"    {mark} {name:<34} {got:<8} 被抓住: {','.join(cases) if cases else '-':<28} (期望 {expect})")

    return 0 if ok and not dirty else 1


if __name__ == "__main__":
    sys.exit(main())
