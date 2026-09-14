#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WS 会话所有权 —— 变异验证驱动。

对每个变异：改一处源码 → 重建 uvcpp + 测试可执行文件 → 拷 DLL → 跑用例 →
记录结果 → 按字节还原。判定只看退出码（0 = 没抓住）。
"""
import os
import subprocess
import sys
import hashlib

sys.stdout.reconfigure(encoding='utf-8')

ROOT = r"D:\public\libuvcpp\libuvcpp"
BUILD = "build-webapp"
TEST = "test_web_ws_ownership_func"

SESS = os.path.join(ROOT, "src", "web", "uvcpp_ws_sessions.cpp")
CONN = os.path.join(ROOT, "src", "web", "uvcpp_ws_connection.cpp")

# (名字, 文件, 原文, 替换, 期望)
MUTATIONS = [
    ("S1 drain_never_deletes",
     SESS,
     "    delete batch[i];\n    ++recycled_;",
     "    ++recycled_;   /* MUTATION: 不删只计数 */",
     "没抓住 -> 真实覆盖缺口（见报告）"),

    ("S2 recycled_never_counted",
     SESS,
     "    ++recycled_;",
     "    ;   /* MUTATION */",
     "抓住"),

    ("S3 on_retired_not_enqueued",
     SESS,
     "  retired_.push_back(c);",
     "  ;   /* MUTATION: 不登记，回收永不发生 */",
     "抓住"),

    ("S4 always_fire_1006",
     CONN,
     "  if (!close_notified_) {\n    close_notified_ = true;\n    if (on_close_) on_close_(ws_close_code::ABNORMAL_CLOSE, std::string());",
     "  if (true) {   /* MUTATION */\n    close_notified_ = true;\n    if (on_close_) on_close_(ws_close_code::ABNORMAL_CLOSE, std::string());",
     "抓住"),

    ("S5 adopt_no_retire_fn",
     SESS,
     "  c->set_retire_callback([this](uvcpp_ws_connection* s) { on_retired(s); });",
     "  (void)0;   /* MUTATION: 不装终结回调 */",
     "抓住"),

    ("S6 alive_token_guard_removed",
     CONN,
     "    if (tok.expired()) return;   // 会话已经终结/回收，别碰任何成员",
     "    ;   /* MUTATION: 令牌守卫拆掉 */",
     "?"),

    ("S7 recycled_double_count",
     SESS,
     "    ++recycled_;",
     "    recycled_ += 2;   /* MUTATION */",
     "抓住"),
]


def run(cmd, cwd=ROOT, timeout=900):
    return subprocess.run(cmd, cwd=cwd, shell=True, timeout=timeout,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


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
    fails = [ln.strip() for ln in out.splitlines() if "[FAIL]" in ln]
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
    for path in (SESS, CONN):
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
            results.append((name, "未施加", expect))
            continue

        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(src.replace(old, new))

        try:
            r = build_and_test(name)
        finally:
            with open(path, "wb") as f:
                f.write(originals[path])

        if r is None:
            results.append((name, "构建失败", expect))
            continue
        rc, fails = r
        caught = "抓住" if rc != 0 else "没抓住"
        results.append((name, caught, expect))
        print(f"### {name}: {caught} (rc={rc}, {len(fails)} 条失败)")
        for ln in fails[:5]:
            print("      " + ln)

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
    for name, got, expect in results:
        mark = "OK" if got == expect or expect == "?" else "??"
        print(f"    {mark}  {name:<34} {got:<8} (期望 {expect})")

    return 0 if ok and not dirty else 1


if __name__ == "__main__":
    sys.exit(main())
