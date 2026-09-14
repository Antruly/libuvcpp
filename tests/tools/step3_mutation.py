#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""第三步（permessage-deflate）变异验证驱动。

对每个变异：改一处源码 -> 重建 -> 跑三个用例 -> 还原。
输出一张表：抓住 / 没抓住，以及是哪个用例抓住的。

必须用 utf-8 读源码与构建输出（默认 text=True 会按 GBK 解码，把构建失败的
判定悄悄弄坏）。
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD = os.path.join(ROOT, "build-webapp")
REL = os.path.join(BUILD, "tests", "functional", "Release")

EXT = os.path.join(ROOT, "src", "web", "uvcpp_ws_ext.cpp")
CONN = os.path.join(ROOT, "src", "web", "uvcpp_ws_connection.cpp")
SRV = os.path.join(ROOT, "src", "web", "uvcpp_ws_server.cpp")
CLI = os.path.join(ROOT, "src", "web", "uvcpp_ws_client.cpp")
PARSER = os.path.join(ROOT, "src", "web", "uvcpp_ws_parser.cpp")

TESTS = ["test_web_ws_ext_func", "test_web_ws_deflate_func", "test_web_compress_func",
         "test_web_ws_reassembly_func", "test_web_ws_parser_func"]

# (名字, 文件, old, new)
MUTATIONS = [
    # ---- 协商模块（纯函数）----
    ("E1 server_max_window_bits 忽略本端约束", EXT,
     "      p.server_max_window_bits = (cfg.server_max_window_bits < a.server_bits)\n"
     "                                     ? cfg.server_max_window_bits\n"
     "                                     : a.server_bits;",
     "      p.server_max_window_bits = a.server_bits;  /* MUTATION */"),

    ("E2 无值 client_max_window_bits 无视本端配置", EXT,
     "        p.client_max_window_bits = cfg.client_max_window_bits;",
     "        p.client_max_window_bits = 15;  /* MUTATION */"),

    ("E3 对端没提也回 client_no_context_takeover", EXT,
     "    p.client_no_context_takeover = a.client_no_ctxt;\n"
     "    p.server_no_context_takeover =\n"
     "        a.server_no_ctxt || cfg.server_no_context_takeover;\n"
     "\n"
     "    return p;  // 取第一个能接受的",
     "    p.client_no_context_takeover = true;  /* MUTATION */\n"
     "    p.server_no_context_takeover =\n"
     "        a.server_no_ctxt || cfg.server_no_context_takeover;\n"
     "\n"
     "    return p;  // 取第一个能接受的"),

    ("E4 服务端不回报 server_no_context_takeover", EXT,
     "    p.client_no_context_takeover = a.client_no_ctxt;\n"
     "    p.server_no_context_takeover =\n"
     "        a.server_no_ctxt || cfg.server_no_context_takeover;\n"
     "\n"
     "    return p;  // 取第一个能接受的",
     "    p.client_no_context_takeover = a.client_no_ctxt;\n"
     "    p.server_no_context_takeover = false;  /* MUTATION */\n"
     "\n"
     "    return p;  // 取第一个能接受的"),

    ("E5 窗口位数允许前导零", EXT,
     "  if (v.size() > 1 && v[0] == '0') return false;",
     "  if (false && v.size() > 1 && v[0] == '0') return false;  /* MUTATION */"),

    ("E6 客户端放行非法的无值应答", EXT,
     "  if (a.has_client_bits && a.client_bits_valueless) {\n"
     "    declined.invalid = true;",
     "  if (false && a.has_client_bits && a.client_bits_valueless) {\n"
     "    declined.invalid = true;  /* MUTATION */"),

    # ---- 接线 ----
    ("D1 服务端不给连接配压缩", SRV,
     "    if (dp.accepted) conn->enable_compression(true, dp);",
     "    /* MUTATION D1 */"),

    ("D2 服务端无条件回报扩展头", SRV,
     "  if (dp.accepted) {\n    oss << \"Sec-WebSocket-Extensions: \"",
     "  if (true) {\n    oss << \"Sec-WebSocket-Extensions: \"  /* MUTATION */"),

    ("D3 服务端无条件启用压缩", SRV,
     "    if (dp.accepted) conn->enable_compression(true, dp);",
     "    conn->enable_compression(true, dp);  /* MUTATION D3 */"),

    ("D4 忽略 101 的写失败状态", SRV,
     "    if (status != 0) return;",
     "    if (false && status != 0) return;  /* MUTATION D4 */"),

    ("D5 客户端谈成了却不启用压缩", CLI,
     "    if (deflate_params_.accepted) conn->enable_compression(false, deflate_params_);",
     "    /* MUTATION D5 */"),

    ("D6 客户端不提议 permessage-deflate", CLI,
     "      if (!ext.empty()) req += \"Sec-WebSocket-Extensions: \" + ext + \"\\r\\n\";",
     "      (void)ext;  /* MUTATION D6 */"),

    ("D7 客户端忽略非法的应答（静默降级）", CLI,
     "    if (deflate_params_.invalid) {",
     "    if (false && deflate_params_.invalid) {  /* MUTATION D7 */"),

    ("D8 客户端用错角色配压缩", CLI,
     "    if (deflate_params_.accepted) conn->enable_compression(false, deflate_params_);",
     "    if (deflate_params_.accepted) conn->enable_compression(true, deflate_params_);  /* MUTATION D8 */"),

    ("D9 忽略压缩阈值", CONN,
     "    if (parser_.is_compression_enabled() && n >= compress_min_size_) {",
     "    if (parser_.is_compression_enabled()) {  /* MUTATION D9 */"),

    # 缩进必须与源码逐字节一致 —— 这条第一次写成 10 空格，count=0 没施加成功
    ("D10 压缩了却不置 RSV1", CONN,
     "      f.rsv1 = true;",
     "      f.rsv1 = false;  /* MUTATION */"),

    # ---- 掩码负载路径（本轮修的 bug：逐字节 append）----
    ("D11 掩码负载逐字节 append（原始实现）", PARSER,
     "      const size_t base = frame_.payload.size();   // 追加前的位置\n"
     "      frame_.payload.append_data(reinterpret_cast<const char*>(*pp), to_copy);",
     "      const size_t base = frame_.payload.size();\n"
     "      for (size_t i = 0; i < to_copy; i++)\n"
     "        frame_.payload.append_data(reinterpret_cast<const char*>(&(*pp)[i]), 1);  /* MUTATION */"),

    ("D12 续段掩码相位从 0 重来", PARSER,
     "        d[base + i] ^= static_cast<char>(frame_.mask_key[(off + i) % 4]);",
     "        d[base + i] ^= static_cast<char>(frame_.mask_key[i % 4]);  /* MUTATION */"),
]


def read(p):
    return io.open(p, encoding="utf-8").read()


def write(p, s):
    io.open(p, "w", encoding="utf-8", newline="").write(s)


def build():
    r = subprocess.run(
        ["cmake", "--build", BUILD, "--config", "Release", "--parallel", "4",
         "--target", "uvcpp"] + TESTS + ["copy_test_dlls"],
        cwd=ROOT, capture_output=True, encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    if "error C" in out or r.returncode != 0:
        return False, out
    return True, out


def run_all():
    """返回 {test: (ok, 首条失败用例的标记)}"""
    res = {}
    for t in TESTS:
        exe = os.path.join(REL, t + ".exe")
        r = subprocess.run([exe], cwd=REL, capture_output=True,
                           encoding="utf-8", errors="replace", timeout=300)
        out = (r.stdout or "") + (r.stderr or "")
        res[t] = (r.returncode == 0, out)
    return res


def main():
    # 可只跑名字里含某个子串的变异：`step3_mutation.py D10` —— 单条复跑时省掉
    # 整轮重建（整套 16 条约 7 分钟）。
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    todo = [m for m in MUTATIONS if only in m[0]]

    # 先把原始源码留在内存里，最后按字节还原
    pristine = {p: read(p) for p in (EXT, CONN, SRV, CLI, PARSER)}

    print("=== 基线 ===", flush=True)
    ok, out = build()
    if not ok:
        print("基线构建失败：\n" + out[-3000:])
        return 1
    base = run_all()
    for t in TESTS:
        print("  %-28s %s" % (t, "PASS" if base[t][0] else "FAIL"), flush=True)
    if not all(base[t][0] for t in TESTS):
        print("基线不是全绿，先修好再来做变异。")
        return 1

    rows = []
    for name, path, old, new in todo:
        src = pristine[path]
        if src.count(old) != 1:
            rows.append((name, "变异施加失败(count=%d)" % src.count(old), ""))
            print("  %-46s 变异施加失败(count=%d)" % (name, src.count(old)), flush=True)
            continue
        write(path, src.replace(old, new))
        try:
            ok, out = build()
            if not ok:
                rows.append((name, "构建失败", ""))
                print("  %-46s 构建失败" % name, flush=True)
                continue
            res = run_all()
            caught_by = [t.replace("test_web_ws_", "").replace("_func", "")
                         for t in TESTS if not res[t][0]]
            if caught_by:
                # 记下第一条失败用例的名字，便于核对是"该抓的"抓住了
                first = ""
                for t in TESTS:
                    if not res[t][0]:
                        for line in res[t][1].splitlines():
                            if line.strip().startswith(("negotiate", "server_", "client_",
                                                        "disabled_", "declined_", "real_",
                                                        "compress_", "send_", "handshake",
                                                        "parse_", "context_")):
                                first = line.strip()
                                break
                        break
                rows.append((name, "抓住", ",".join(caught_by) + " | " + first))
                print("  %-46s 抓住  <- %s" % (name, ",".join(caught_by)), flush=True)
            else:
                rows.append((name, "**没抓住**", ""))
                print("  %-46s **没抓住**" % name, flush=True)
        finally:
            write(path, pristine[path])

    # 还原 + 校验
    print("\n=== 还原校验 ===", flush=True)
    allok = True
    for p, s in pristine.items():
        cur = read(p)
        same = (cur == s)
        allok = allok and same
        print("  %-40s %s" % (os.path.basename(p), "字节一致" if same else "**不一致**"),
              flush=True)
    if not allok:
        print("还原失败！")
        return 1

    ok, out = build()
    if not ok:
        print("还原后构建失败：\n" + out[-3000:])
        return 1
    final = run_all()
    for t in TESTS:
        print("  还原后 %-30s %s" % (t, "PASS" if final[t][0] else "FAIL"), flush=True)

    print("\n=== 汇总 ===")
    for name, verdict, detail in rows:
        print("| %s | %s | %s |" % (name, verdict, detail))
    n_caught = sum(1 for _, v, _ in rows if v == "抓住")
    print("\n抓住 %d / 共 %d 条变异" % (n_caught, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
