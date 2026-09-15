#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Phase 3b 步骤 6（尺寸上限矩阵 + 截断 + `PART_BODY_SKIP`）的变异驱动。

判定规格沿用 `upload_route_mutation.py`：
  1. 退出码非 0；
  2. **单独跑期望的那一组**也是红的（不看"全量里红了别的组"）。

第 2 条是必需的：本仓用例 `main()` 在**第一个**失败的用例上就 `return 2`，
所以全量跑只会有一种红。一个变异若同时打红了排在前面的一组，全量输出里就
看不到期望的那一组了 —— 判据会写成"没抓住"，而实际抓住了。

本步的变异分**两类**，两类都要有，缺一类就留一个洞：

  - **机制**（L1–L6）：上限判定、截断、SKIP 扫边界、abort 的钩子通知 ——
    这些是"上限真的拦得住"这件事本身；
  - **接线**（L7–L11）：五个 `mp->set_max_*()` 转发各去掉一条 —— 少了这一类，
    一个"限值配了但压根没送进解析器"的实现能让机制类的变异照旧被抓住
    （因为机制类变异改的是解析器内部，与转发无关），而**用户真正会遇到的那个
    缺陷恰好是转发漏了**。

用法：
  python -u tests/tools/limits_mutation.py [--tree build-webapp] [--only L1]
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_app.cpp")
STREAM_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_stream.cpp")
MP_CPP = os.path.join(ROOT, "src", "webapp", "uvcpp_web_multipart.cpp")

T_ROUTE = "test_web_app_upload_route_func"

# 五条转发语句的公共形状：`mp->set_max_X(...)`。逐条去掉一条。
def forward(line):
    return "  mp->set_max_" + line


# (名字, 文件, 被替换的原文, 变异后的文本, 期望抓住它的用例, 说明)
MUTATIONS = [
    (
        "L1",
        APP_CPP,
        """    if (max_upload_size_ > 0 &&
        mp->received() + static_cast<uint64_t>(len) > max_upload_size_) {
      stream.abort(static_cast<int>(http_status::PAYLOAD_TOO_LARGE));
      return;
    }""",
        """    /* MUTATION: 总长上限不判（指望 body_limit 拦住 chunked） */""",
        "limits_total",
        "总长上限不判 —— chunked 上传会一路收完并落盘（`body_limit` 对它放行）",
    ),
    (
        "L2",
        APP_CPP,
        """    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FILES:
    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FIELDS:
    case uvcpp_web_multipart_result::ERROR_FIELD_TOO_LARGE:
      return static_cast<int>(http_status::PAYLOAD_TOO_LARGE);""",
        """    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FILES:
    case uvcpp_web_multipart_result::ERROR_TOO_MANY_FIELDS:
    case uvcpp_web_multipart_result::ERROR_FIELD_TOO_LARGE:
      return 0; /* MUTATION: 上限族不再映射状态码（交给用户的 on_end） */""",
        "limits_file_count",
        "上限族的错误不回框架状态码（用户拿到一个 200 和一个 nullptr 结果）",
    ),
    (
        "L3",
        APP_CPP,
        """    case uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG:
      return static_cast<int>(http_status::BAD_REQUEST);""",
        """    case uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG:
      return static_cast<int>(
          http_status::PAYLOAD_TOO_LARGE); /* MUTATION: 400 说成 413 */""",
        "limits_part_header",
        "部件头超长映射成 413（把「报文不成形」说成「你给的东西太大」）",
    ),
    (
        "L4",
        MP_CPP,
        """    if (state_ == uvcpp_web_multipart_state::ERROR_STATE ||
        state_ == uvcpp_web_multipart_state::EPILOGUE) {
      return;
    }""",
        """    if (state_ == uvcpp_web_multipart_state::ERROR_STATE ||
        state_ == uvcpp_web_multipart_state::EPILOGUE) {
      return;
    }
    /* MUTATION: SKIP 阶段不再扫边界 —— 后面的部件被这一块整个吃掉 */
    if (state_ == uvcpp_web_multipart_state::PART_BODY_SKIP) return;""",
        "limits_file_truncate",
        "`PART_BODY_SKIP` 不扫边界（截断之后的所有部件静默消失）",
    ),
    (
        "L5",
        MP_CPP,
        """    truncated_ = true;
    state_ = uvcpp_web_multipart_state::PART_BODY_SKIP;""",
        """    /* MUTATION: 截断了却不上报 */
    state_ = uvcpp_web_multipart_state::PART_BODY_SKIP;""",
        "limits_file_truncate",
        "截断发生了但 `truncated()` 不置位（handler 把半截文件当成完整的）",
    ),
    (
        "L6",
        STREAM_CPP,
        """  if (hooks_.on_abort) hooks_.on_abort();
  if (abort_cb_) abort_cb_();""",
        """  /* MUTATION: 框架槽不通知 —— 被上限拦下的上传不会删掉半截文件 */
  if (abort_cb_) abort_cb_();""",
        "limits_total",
        "`abort()` 不跑框架钩子（已经交收的 kept 文件留在盘上 —— 析构兜底特意保留它）",
    ),
    (
        "L7",
        APP_CPP,
        forward("file_size(max_file_size_);"),
        "  /* MUTATION: 单文件上限没送进解析器 */",
        "limits_file_truncate",
        "`set_max_file_size` 没转发给解析器（配了也不截断）",
    ),
    (
        "L8",
        APP_CPP,
        forward("file_count(max_upload_files_);"),
        "  /* MUTATION: 文件数上限没送进解析器 */",
        "limits_file_count",
        "`set_max_upload_files` 没转发给解析器",
    ),
    (
        "L9",
        APP_CPP,
        forward("field_count(max_form_fields_);"),
        "  /* MUTATION: 字段数上限没送进解析器 */",
        "limits_field_count",
        "`set_max_form_fields` 没转发给解析器",
    ),
    (
        "L10",
        APP_CPP,
        forward("field_size(max_field_size_);"),
        "  /* MUTATION: 单字段上限没送进解析器 */",
        "limits_field_size",
        "`set_max_field_size` 没转发给解析器",
    ),
    (
        "L11",
        APP_CPP,
        forward("part_header_bytes(max_part_header_bytes_);"),
        "  /* MUTATION: 部件头上限没送进解析器 */",
        "limits_part_header",
        "`set_max_part_header_bytes` 没转发给解析器",
    ),
]

# 跑挂的变异会永久挂住驱动，而驱动是"跑完才还原源码"的 —— 外部杀掉它时 src/
# 会留在变异状态。统一超时，按 124 上报（与 GNU timeout 同值，且仍是非零）。
RUN_TIMEOUT_S = 600


def read_text(p):
    with open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_text(p, s):
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)


def md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


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
    rc, out = run(["cmake", "--build", tree, "--config", "Release",
                   "--parallel", "4"])
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    return rc, nerr, out


def failed_cases(out):
    """从测试输出里抠出红了的用例名（跟着 `[<tag>] <name>` 走）。"""
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


def fail_lines(out):
    """输出里所有 `[FAIL]` 行 —— 用来核对"红的是不是这条断言"。"""
    return [l.strip() for l in out.splitlines() if l.strip().startswith("[FAIL]")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-webapp")
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    exe_dir = os.path.join(tree, "tests", "functional", "Release")
    exe = os.path.join(exe_dir, T_ROUTE + ".exe")

    files = sorted({m[1] for m in MUTATIONS})
    backups = {p: read_text(p) for p in files}
    before = {p: md5(p) for p in files}

    print("=" * 72)
    print("基线")
    print("=" * 72)
    rc, nerr, out = build(args.tree)
    print("  build rc=%d errors=%d" % (rc, nerr), flush=True)
    if rc != 0 or nerr:
        print(out[-3000:])
        return 3
    rc, out = run(["cmake", "--build", args.tree, "--config", "Release",
                   "--target", "copy_test_dlls"])
    if rc != 0:
        print("  copy_test_dlls 失败")
        return 3
    rc, out = run([exe], cwd=exe_dir)
    print("  基线 %s: %s (rc=%d)" % (T_ROUTE, "ALL PASS" if rc == 0 else "FAIL", rc),
          flush=True)
    if rc != 0:
        print(out[-3000:])
        print("  基线不绿，后面的判定没有意义 —— 停下")
        return 3

    results = []
    for name, path, old, new, expect, desc in MUTATIONS:
        if args.only and name != args.only:
            continue

        src = read_text(path)
        if old not in src:
            results.append((name, "施加失败", expect, desc))
            print("\n[%s] 施加失败 —— 源码里找不到待替换的片段" % name, flush=True)
            continue

        write_text(path, src.replace(old, new, 1))
        print("\n[%s] %s" % (name, desc), flush=True)

        rc, nerr, out = build(args.tree)
        if rc != 0 or nerr:
            write_text(path, src)
            results.append((name, "构建失败", expect, desc))
            print("  构建失败（rc=%d errors=%d）—— 不算抓住" % (rc, nerr), flush=True)
            print(out[-1200:])
            continue

        run(["cmake", "--build", args.tree, "--config", "Release",
             "--target", "copy_test_dlls"])

        # 判据：**单独跑期望的那一组**。全量输出用来记附带损伤与失败签名。
        rc_scoped, out_scoped = run([exe, expect], cwd=exe_dir)
        rc_full, out_full = run([exe], cwd=exe_dir)
        red_full = failed_cases(out_full)

        write_text(path, src)  # 立刻还原

        caught = rc_scoped != 0
        verdict = "抓住" if caught else "没抓住"
        results.append((name, verdict, expect, desc))
        extra = "" if red_full[:1] == [expect] else "  全量首红=%s" % (red_full[:1] or "(无)")
        print("  单独跑 %s: rc=%d → %s；全量 rc=%d%s"
              % (expect, rc_scoped, verdict, rc_full, extra), flush=True)
        if caught:
            for l in fail_lines(out_scoped)[:4]:
                print("    " + l, flush=True)

    # ---- 还原后重建：这一步不做完，这棵树就是脏的 ----
    print("\n" + "=" * 72)
    print("还原")
    print("=" * 72)
    for p, text in backups.items():
        write_text(p, text)
    ok = all(md5(p) == before[p] for p in files)
    print("  源码按字节还原: %s" % ("是" if ok else "否 —— 有问题！"))
    rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")], cwd=ROOT)
    print("  grep MUTATION 残留: %s" % (out.strip() or "(无)"))

    rc, nerr, out = build(args.tree)
    run(["cmake", "--build", args.tree, "--config", "Release",
         "--target", "copy_test_dlls"])
    rc2, _ = run([exe], cwd=exe_dir)
    print("  还原后重建 rc=%d errors=%d，复跑 %s=%s"
          % (rc, nerr, T_ROUTE, "ALL PASS" if rc2 == 0 else "FAIL"))

    print("\n" + "=" * 72)
    print("汇总")
    print("=" * 72)
    for name, verdict, expect, desc in results:
        print("  %-4s %-8s %-24s %s" % (name, verdict, expect, desc))
    return 0 if ok and rc2 == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
