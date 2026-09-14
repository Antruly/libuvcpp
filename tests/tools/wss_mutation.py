#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WSS（`enable_wss()` 的 TLS 那一半）—— 变异验证驱动。

只跑 build-ssl（`web_ssl_app_ws_func.cpp` 在那个配置里才存在）。

这两条变异打的是同一个问题的两面：
    W1  框架记下了"要 TLS"，却没把上下文装到监听器上
    W2  装是装了，但装在一个永远为假的条件下
两条都应当让场景 1（真 WSS 往返）与场景 2（明文打 WSS 端口必须拿不到 101）
一起红，而场景 3（没开 TLS 的同构对照）**必须照旧绿** —— 场景 3 绿着才说明
红的不是"探针坏了"。
"""
import os
import subprocess
import sys
import hashlib

sys.stdout.reconfigure(encoding='utf-8')

ROOT = r"D:\public\libuvcpp\libuvcpp"
BUILD = "build-ssl"
TEST = "test_web_ssl_app_ws_func"

APP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")

MUTATIONS = [
    ("W1 ctx_never_installed",
     APP,
     "    tcp->set_ssl_context(ssl_ctx_.get());",
     "    /* MUTATION: 记了 ssl_enabled_ 但不装上下文 */",
     "scenario 1 + 2"),

    ("W2 install_under_false_guard",
     APP,
     "    if (!ssl_ctx_ || !ssl_ctx_->is_ready()) {",
     "    if (true) {   /* MUTATION: 永远走「没有 TLS」那一支 */",
     "scenario 1 + 2"),
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
    fails = [ln.strip() for ln in out.splitlines() if "[FAIL]" in ln]
    # 场景 3 是对照：它**必须**在变异下照旧通过，否则红的不是我们想考的东西。
    ctrl_failed = [ln for ln in fails if ln.startswith("[FAIL] 3:")]
    return r.returncode, fails, ctrl_failed


def main():
    print("### 基线")
    base = build_and_test("baseline")
    print("    ", base)
    if base is None or base[0] != 0:
        print("基线不绿，中止")
        return 2

    with open(APP, "rb") as f:
        original = f.read()

    results = []
    for name, path, old, new, expect in MUTATIONS:
        src = original.decode("utf-8")
        if src.count(old) != 1:
            print(f"### {name}: 变异**施加失败**（锚点匹配 {src.count(old)} 次）")
            results.append((name, "未施加", expect, 0, 0))
            continue

        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(src.replace(old, new))
        try:
            r = build_and_test(name)
        finally:
            with open(path, "wb") as f:
                f.write(original)

        if r is None:
            results.append((name, "构建失败", expect, 0, 0))
            continue
        rc, fails, ctrl = r
        caught = "抓住" if rc != 0 else "没抓住"
        results.append((name, caught, expect, len(fails), len(ctrl)))
        print(f"### {name}: {caught} (rc={rc}, {len(fails)} 条失败, "
              f"其中场景 3 的 {len(ctrl)} 条)")
        for ln in fails[:6]:
            print("      " + ln)

    print("\n### 还原校验")
    with open(APP, "rb") as f:
        same = f.read() == original
    print(f"    uvcpp_web_app.cpp: {'字节一致' if same else '!!! 不一致 !!!'}")
    dirty = run("grep -rn MUTATION src/").stdout.decode("utf-8", "replace").strip()
    print(f"    MUTATION 残留: {dirty if dirty else '无'}")

    print("\n### 汇总")
    for name, got, expect, nf, nc in results:
        # 判据有两条：抓住，且**没有**打到场景 3 这条对照。
        ok = (got == "抓住" and nc == 0) or got in ("未施加", "构建失败")
        print(f"    {'OK ' if ok else '?? '} {name:<32} {got:<8} "
              f"失败 {nf} 条 / 对照被打到 {nc} 条  (期望 {expect})")
        if got == "抓住" and nc > 0:
            print("       !! 对照组也红了 —— 这说明用例本身坏了，不是变异被抓")

    return 0 if same and not dirty else 1


if __name__ == "__main__":
    sys.exit(main())
