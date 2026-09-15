#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Phase 3b 步骤 3（上传落盘 sink）的变异驱动。

判定规格是**两条缺一不可**（沿用 `stream_mutation.py` 的规矩）：
  1. 退出码非 0；
  2. 红了的用例里**包含期望的那一组**。

只看"退出码非 0"的话，"碰巧红了别的用例"会被记成一次成功 —— 那不是证据。

**第一版驱动踩到的两个坑，这一版都修掉了（值得留给后面的驱动）**

坑一：**"期望的那一组"必须用子串过滤单独跑，不能从全量输出里抠。**
本仓的用例 `main()` 在**第一个失败的用例**上就 `return 2`，所以一次运行
**永远只有一个** `-> FAIL`。于是"某个变异同时打红了更靠前的一组"时，全量输出
里只会看到那一组，期望的那一组根本没机会跑 —— 判据就成了"没抓住"，而实际上
抓住了。实测：U6 期望 `empty_file`，可 `single_chunk` 排在前面也被打红，
报告写成"红了但不是期望的组"。
现在改成：**先 `exe <期望用例名>` 单独跑**（rc != 0 即判定为抓住），
再跑一次全量只为记录附带损伤。

坑二：**一个驱动可以横跨多个用例文件，别把期望写死在一个 exe 上。**
U9 打的是解析器的 quoted-pair 反转义。那条不变式由 `web_app_multipart_func.cpp`
的 `quoted_filename` 组钉住（`filename="a\\"b.txt"` → `a"b.txt`），而上传用例
里 `filename_metadata` 比的是"上传层转发值 == 解析器给的值" —— **两边同源**，
解析器不反转义时两边一起变，等式照旧成立。所以 U9 在 `web_app_upload_func`
上**必然**抓不住，而这不是覆盖缺口。现在每条变异带自己的 `target`。

每施加一条变异 → 重建 → 跑 → 逐字节还原 → 汇总。
**末尾一定要重建成还原后的版本**，否则驱动退出时树里留着的是最后一条变异的
二进制（`stream_mutation.py` 踩过这个坑）。

用法：
  python -u tests/tools/upload_mutation.py [--tree build-webapp] [--only U1]
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UPLOAD_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_upload.cpp")
MULTIPART_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_multipart.cpp")
APP_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")
REQUEST_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_request.cpp")

# 用例目标名（exe 名 = 目标名 + ".exe"）。
T_UPLOAD = "test_web_app_upload_func"
T_MULTIPART = "test_web_app_multipart_func"
T_ROUTE = "test_web_app_upload_route_func"

# (名字, 文件, 被替换的原文, 变异后的文本, 用例目标, 期望抓住它的用例, 说明)
MUTATIONS = [
    (
        "U1",
        UPLOAD_CPP,
        """  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].kept) queue_.push_back(i);
  }""",
        "  /* MUTATION: 不把已完成的 job 排回丢弃队列 */",
        T_UPLOAD,
        "abort_deletes_completed_file",
        "失败路径不删已经写完的文件（`fail()` 不重排 `kept`）",
    ),
    (
        "U2",
        UPLOAD_CPP,
        """  if (j.kept && !discarding_) continue;""",
        "  if (j.kept) continue; /* MUTATION */",
        T_UPLOAD,
        "abort_deletes_completed_file",
        "析构兜底也漏掉已完成的文件",
    ),
    (
        "U3",
        UPLOAD_CPP,
        """  if (rc < 0) {
    j.failed = true;
    fail(std::string("打开上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
  } else {
    j.fd = static_cast<int>(rc);
    j.fd_valid = true;
  }""",
        """  j.fd = static_cast<int>(rc);
  j.fd_valid = true; /* MUTATION: open 失败也算 fd 有效 */
  if (rc < 0) {
    j.failed = true;
    fail(std::string("打开上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
  }""",
        T_UPLOAD,
        "open_failure_keeps_dir_clean",
        "open 失败也算 fd 有效（清理路径会对负 fd 发 close）",
    ),
    (
        "U4",
        UPLOAD_CPP,
        """  if (j.failed) {
    queue_.pop_front();
    return true;
  }
  if (!j.fd_valid) {""",
        """  if (false) {
    queue_.pop_front();
    return true;
  }
  if (!j.fd_valid) {""",
        T_UPLOAD,
        "open_failure_keeps_dir_clean",
        "去掉 open 失败的短路（这一支按源码注释是防御性的第二道）",
    ),
    (
        "U5",
        UPLOAD_CPP,
        """    if (j.pending.empty() && !j.incoming.empty()) {
      j.pending.swap(j.incoming);
      j.incoming.clear();
    }""",
        "    /* MUTATION: 写完不在这里换槽 */",
        T_UPLOAD,
        "chunked_feed",
        "写完的换槽被拆掉（`pump()` 里还有一次同样的换槽）",
    ),
    (
        "U6",
        UPLOAD_CPP,
        """  if (j.end_seen && !j.closed) {""",
        """  if (false && j.end_seen && !j.closed) {""",
        T_UPLOAD,
        "empty_file",
        "0 字节部件永远走不到 fsync/close",
    ),
    (
        "U7",
        UPLOAD_CPP,
        """  j.incoming.append(data, len);""",
        """  /* MUTATION: 收到的数据直接丢掉 */""",
        T_UPLOAD,
        "single_chunk",
        "body 字节根本没进缓冲",
    ),
    (
        "U8",
        UPLOAD_CPP,
        """  const int rc = fs_->open(loop_, j.meta.path_.c_str(), k_open_flags, k_open_mode,
                           [self, idx](uvcpp_fs* f) {
                             self->on_open_done(
                                 idx, static_cast<long long>(f->get_result()));
                           });""",
        """  const int rc = fs_->open(loop_, j.meta.path_.c_str(), k_open_flags, k_open_mode,
                           ::std::function<void(uvcpp_fs*)>()); /* MUTATION: 空回调 */""",
        T_UPLOAD,
        "single_chunk",
        "异步提交给一个空回调（永远没人推进 —— 看门狗该抓它）",
    ),
    (
        "U9",
        MULTIPART_CPP,
        """  for (size_t i = 1; i + 1 < v.size(); ++i) {
    if (v[i] == '\\\\' && i + 2 < v.size()) {
      out += v[i + 1];
      ++i;
      continue;
    }
    out += v[i];
  }""",
        """  for (size_t i = 1; i + 1 < v.size(); ++i) {
    out += v[i]; /* MUTATION: 不做 quoted-pair 反转义 */
  }""",
        T_MULTIPART,
        "quoted_filename",
        "解析器不再做 quoted-pair 反转义（由 multipart 套件钉住）",
    ),
    (
        "U10",
        UPLOAD_CPP,
        "const int k_open_flags = O_WRONLY | O_CREAT | O_EXCL | O_BINARY;",
        "const int k_open_flags = O_WRONLY | O_CREAT | O_TRUNC | O_BINARY;"
        " /* MUTATION: 去掉 O_EXCL */",
        T_UPLOAD,
        "excl_no_clobber",
        "落盘不加 O_EXCL（正对着预先种好目标路径的攻击面）",
    ),
    (
        "U11",
        APP_CPP,
        """  if (upload_dir_unsafe_) {
    reject_upload(ctx, 500,
                  "上传目录位于静态文档根内，按配置已被拒绝（详见启动日志）");
    return false;
  }""",
        "  /* MUTATION: 配置期查出来的逃逸不拦 */",
        T_ROUTE,
        "upload_dir_in_static_root",
        "上传目录落在静态文档根内时不再拒绝（配置检查成了只打日志）",
    ),
    (
        "U12",
        REQUEST_CPP,
        "      f.filename = web_sanitize_filename(info.filename);",
        "      f.filename = info.filename; /* MUTATION: 缓冲 sink 不清洗文件名 */",
        T_ROUTE,
        "buffered_multipart_form",
        "普通路由的 multipart sink 直接把客户端原值当元数据（不清洗）",
    ),
]


def read_text(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


# 子进程一律带超时。**这个驱动在源码处于变异状态时跑用例**，所以任何一个让
# 用例永久挂起的变异都会把驱动一起挂住 —— 而驱动是"跑完才还原源码"，于是外部
# 杀掉它时 `src/` 会**留在变异状态**。本步真的发生过：U8 的全量跑挂了 14 分钟
# （见 `upload_mutation_probe.py` 与那段看门狗分析）。
# 超时按 124 上报（与 GNU timeout 同值），它同时也是一个非零退出码，所以
# "跑挂了"不会被误读成"用例通过了"。
RUN_TIMEOUT_S = 600


def run(cmd, cwd=ROOT):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace",
                           timeout=RUN_TIMEOUT_S)
    except subprocess.TimeoutExpired as e:
        out = b"".join(x for x in (e.stdout, e.stderr) if x)
        if isinstance(out, str):
            out = out.encode("utf-8", "replace")
        return 124, out.decode("utf-8", "replace") + \
            "\n*** TIMEOUT after %ds: %s ***\n" % (RUN_TIMEOUT_S, " ".join(cmd))
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree):
    rc, out = run(["cmake", "--build", tree, "--config", "Release", "--parallel", "4"])
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    return rc, nerr, out


def failed_cases(out):
    """从测试输出里抠出红了（或跑了）的用例名。

    跟着 `[<tag>] <name>` 走，再看紧跟的那一行是不是 `-> FAIL` —— 与第二版
    `stream_mutation.py` 同一手法（早先按固定前缀抠，与实际输出格式不符，
    那一列全空）。
    """
    red = []
    cur = None
    for line in out.splitlines():
        m = re.match(r"\[[a-z_]+\] (\S+)$", line)
        if m:
            cur = m.group(1)
            continue
        if line.strip() == "-> FAIL" and cur:
            red.append(cur)
    return red


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")

    def exe_of(target):
        return os.path.join(exe_dir, target + ".exe")

    files = sorted({m[1] for m in MUTATIONS})
    backups = {p: read_text(p) for p in files}
    before = {p: md5(p) for p in files}
    targets = sorted({m[4] for m in MUTATIONS})

    print("=" * 72)
    print("基线")
    print("=" * 72)
    rc, nerr, out = build(args.tree)
    print(f"  build rc={rc} errors={nerr}", flush=True)
    if rc != 0 or nerr:
        print(out[-3000:])
        return 3
    rc, out = run(["cmake", "--build", args.tree, "--config", "Release",
                   "--target", "copy_test_dlls"])
    if rc != 0:
        print("  copy_test_dlls 失败")
        return 3

    for t in targets:
        rc, out = run([exe_of(t)], cwd=exe_dir)
        print(f"  基线 {t}: {'ALL PASS' if rc == 0 else 'FAIL'} (rc={rc})",
              flush=True)
        if rc != 0:
            print(out[-3000:])
            print("  基线不绿，后面的判定没有意义 —— 停下")
            return 3

    results = []
    for name, path, old, new, target, expect, desc in MUTATIONS:
        if args.only and name != args.only:
            continue

        src = read_text(path)
        if old not in src:
            results.append((name, "施加失败", target, expect, desc))
            print(f"\n[{name}] 施加失败 —— 源码里找不到待替换的片段", flush=True)
            continue

        write_text(path, src.replace(old, new, 1))
        print(f"\n[{name}] {desc}", flush=True)

        rc, nerr, out = build(args.tree)
        if rc != 0 or nerr:
            write_text(path, src)
            results.append((name, "构建失败", target, expect, desc))
            print(f"  构建失败（rc={rc} errors={nerr}）—— 不算抓住", flush=True)
            print(out[-1200:])
            continue

        run(["cmake", "--build", args.tree, "--config", "Release",
             "--target", "copy_test_dlls"])

        # 判据：**单独跑期望的那一组**。全量输出只用来记附带损伤。
        rc_scoped, out_scoped = run([exe_of(target), expect], cwd=exe_dir)
        rc_full, out_full = run([exe_of(target)], cwd=exe_dir)
        red_full = failed_cases(out_full)

        write_text(path, src)  # 立刻还原

        caught = rc_scoped != 0
        verdict = "抓住" if caught else "没抓住"
        results.append((name, verdict, target, expect, desc))
        extra = "" if red_full[:1] == [expect] else f"  全量首红={red_full[:1] or '(无)'}"
        print(f"  单独跑 {expect}: rc={rc_scoped} → {verdict}；"
              f"全量 rc={rc_full}{extra}", flush=True)

    # ---- 还原后重建：这一步不做完，这棵树就是脏的 ----
    print("\n" + "=" * 72)
    print("还原")
    print("=" * 72)
    for p, text in backups.items():
        write_text(p, text)
    ok = all(md5(p) == before[p] for p in files)
    print(f"  源码按字节还原: {'是' if ok else '否 —— 有问题！'}")
    rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")], cwd=ROOT)
    print(f"  grep MUTATION 残留: {out.strip() or '(无)'}")

    rc, nerr, out = build(args.tree)
    run(["cmake", "--build", args.tree, "--config", "Release",
         "--target", "copy_test_dlls"])
    reruns = []
    for t in targets:
        rc2, _ = run([exe_of(t)], cwd=exe_dir)
        reruns.append(rc2)
    print(f"  还原后重建 rc={rc} errors={nerr}，复跑 " +
          "、".join(f"{t}={'ALL PASS' if r == 0 else 'FAIL'}"
                    for t, r in zip(targets, reruns)))

    print("\n" + "=" * 72)
    print("汇总")
    print("=" * 72)
    for name, verdict, target, expect, desc in results:
        print(f"  {name:4} {verdict:8} {expect:30} @{target}  {desc}")
    return 0 if ok and not any(reruns) else 1


if __name__ == "__main__":
    sys.exit(main())
