# -*- coding: utf-8 -*-
"""Phase 3c 变异验证：chunked 流式响应 + send_file 分片读下发。

判定规格（两条缺一不可）：
  1. 施加变异后**单独跑期望的那一组**必须红（scoped rc != 0）—— 本仓用例
     `main()` 在**第一个**失败用例上就 `return 2`，所以全量跑看不出排在后面的
     组；全量跑的退出码只作旁证，记录连带伤害。
  2. 跑每条变异**之前**，期望那一组必须先证明自己是绿的（基线 scoped rc == 0）。
     少了这一步，"红了"可能只是那组用例**不存在**（例如没开 zlib 的树里
     `stream_not_compressed` 压根没编译进去），于是"没测"被读成"抓住了"。

`RUN_TIMEOUT_S = 600`，超时按 **124** 上报（GNU timeout 同值，且仍是非零退出
码，绝不会被误读成通过）。这个驱动是"跑完才还原"的，任何让用例挂起的变异都会
把 `src/` 留在变异状态。

末尾一律**还原 → 重建 → 复跑**：不做这一步，驱动退出时构建树里留着的是最后
一条变异的二进制（源码干净、二进制脏 —— Phase 3a 的 S13 就是这么骗过一次）。
"""
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUN_TIMEOUT_S = 600

# 目标 exe 的过滤参数形态（**各文件不一致，实测过**）：
#   "argv1"  ->  exe <子串>        （find() 子串匹配）
#   "--only" ->  exe --only <全名> （名字必须**完全相同**）
TARGETS = {
    "web_http_server_func": ("test_web_http_server_func", "argv1"),
    "web_stream_response_func": ("test_web_stream_response_func", "argv1"),
    "web_app_stream_resp_func": ("test_web_app_stream_resp_func", "argv1"),
    "web_app_stream_func": ("test_web_app_stream_func", "argv1"),
    "web_app_send_file_func": ("test_web_app_send_file_func", "argv1"),
    "web_app_app_func": ("test_web_app_app_func", "--only"),
}

R = "src/webapp/uvcpp_web_response.cpp"
A = "src/webapp/uvcpp_web_app.cpp"
F = "src/webapp/uvcpp_web_file.cpp"
S = "src/webapp/uvcpp_web_static.cpp"
H = "src/web/uvcpp_http_response.cpp"
V = "src/web/uvcpp_http_server.cpp"

# (名字, target, 期望用例, [(文件, 原文, 变异后), ...], 说明)
MUTATIONS = [
    ("X1", "web_http_server_func", "chunked_empty_terminated",
     [(H, '    result += "0\\r\\n\\r\\n";\n  } else if (body.size() > 0) {',
          '    if (body.size() > 0) result += "0\\r\\n\\r\\n";  /* MUTATION X1 */\n'
          '  } else if (body.size() > 0) {')],
     "chunked 分支永不发终止块（原始缺陷）"),

    ("X2", "web_app_stream_resp_func", "begin_write_end",
     [(R, '  const std::string hex = chunk_hex(len);\n'
          '  pending_buf_.append(hex);\n'
          '  pending_buf_.append("\\r\\n");\n'
          '  pending_buf_.append(data, len);\n'
          '  pending_buf_.append("\\r\\n");\n'
          '  pending_bytes_ += hex.size() + len + 4;',
          '  /* MUTATION X2: 不组 hex 帧 */\n'
          '  pending_buf_.append(data, len);\n'
          '  pending_bytes_ += len;')],
     "write_chunk 不组 hex 帧"),

    ("X3", "web_app_stream_resp_func", "begin_write_end",
     [(R, '    pending_buf_.append("0\\r\\n\\r\\n");\n'
          '    pending_bytes_ += 5;\n'
          '    if (pending_bytes_ > max_stream_bytes_) drain_armed_ = true;\n'
          '  }',
          '    /* MUTATION X3: end() 不发终止块 */\n  }')],
     "end() 不发终止块"),

    ("X4", "web_stream_response_func", "stream_head_only",
     [(V, '  ctx.out_streaming = true;\n\n'
          '  enqueue_write(ctx, client, resp.to_string(/*include_body=*/false));\n}',
          '  ctx.out_streaming = true;\n\n'
          '  enqueue_write(ctx, client, resp.to_string(/*include_body=*/true));'
          '  /* MUTATION X4 */\n}')],
     "begin_stream 改回带 body 序列化（流式与普通响应共用一个出口）"),

    ("X5", "web_stream_response_func", "stream_not_compressed",
     [(V, '  ctx.out_streaming = true;\n\n'
          '  enqueue_write(ctx, client, resp.to_string(/*include_body=*/false));',
          '  apply_compression(ctx, resp);  /* MUTATION X5 */\n'
          '  ctx.out_streaming = true;\n\n'
          '  enqueue_write(ctx, client, resp.to_string(/*include_body=*/false));')],
     "往 begin_stream 里加一句 apply_compression"),

    ("X5b", "web_stream_response_func", "stream_not_compressed",
     [(V, '  if (resp.has_header("transfer-encoding")) return false;',
          '  /* MUTATION X5b: 去掉第二道门 */')],
     "去掉 apply_compression 的 transfer-encoding 早返回（第二道门）"),

    ("X5c", "web_stream_response_func", "stream_not_compressed",
     [(V, '  ctx.out_streaming = true;\n\n'
          '  enqueue_write(ctx, client, resp.to_string(/*include_body=*/false));',
          '  apply_compression(ctx, resp);  /* MUTATION X5c */\n'
          '  ctx.out_streaming = true;\n\n'
          '  enqueue_write(ctx, client, resp.to_string(/*include_body=*/false));'),
      (V, '  if (resp.has_header("transfer-encoding")) return false;',
          '  /* MUTATION X5c: 两道门一起去掉 */')],
     "X5 + X5b 同时施加（只剩第一道门时是否可观测）"),

    ("X6", "web_app_stream_resp_func", "on_sent_deferred",
     [(A, '    r.pump_stream();\n    return;\n  }\n\n  // 交给 HTTP 层序列化并异步写出。',
          '    { /* MUTATION X6: 头部入队时就通知，body_bytes 按此刻的 resp 记 */\n'
          '      uvcpp_web_sent_info early;\n'
          '      early.status_code   = rp->status_code();\n'
          '      early.body_bytes    = rp->body_size();\n'
          '      early.connection_id = id;\n'
          '      early.ok            = true;\n'
          '      rp->notify_sent(early);\n'
          '    }\n'
          '    r.pump_stream();\n    return;\n  }\n\n'
          '  // 交给 HTTP 层序列化并异步写出。')],
     "notify_sent 仍在头部入队时触发（原始缺陷）"),

    ("X7", "web_app_stream_resp_func", "head_no_chunks",
     [(R, '  if (head_only_) {\n'
          '    // HEAD 的头部必须与 GET **逐字节相同**（含 transfer-encoding:\n'
          '    // chunked），而 body 一个字节都不发 —— 这是协议合法且正确的。\n'
          '    return true;\n  }',
          '  if (false) {  /* MUTATION X7 */\n'
          '    // HEAD 的头部必须与 GET **逐字节相同**（含 transfer-encoding:\n'
          '    // chunked），而 body 一个字节都不发 —— 这是协议合法且正确的。\n'
          '    return true;\n  }')],
     "HEAD 下照样发 chunk"),

    ("X7b", "web_http_server_func", "head_no_terminator",
     [(V, '  std::string wire = resp.to_string(/*include_body=*/!ctx.is_head);',
          '  std::string wire = resp.to_string();  /* MUTATION X7b */')],
     "HEAD 分支改回 to_string()（不带 false）"),

    ("X8", "web_app_send_file_func", "large_file",
     [(F, '  const uint64_t remain = last_ - offset_ + 1u;',
          '  const uint64_t remain = last_ - offset_;  /* MUTATION X8 */')],
     "末片偏移差一（闭区间当半开区间算）"),

    # 期望用例原本写的是 `large_file`，**探针实测是错的**（见步骤 8 记录）：
    # 未知长度的传输 arm 的是 `last_ = UINT64_MAX - 1`（uvcpp_web_response.cpp
    # 的 `arm_file_transfer(..., UINT64_MAX - 1u, ...)`），于是变异里那句守卫
    # `offset_ <= last_` **恒真**，它在**第一次**不足片的读上就触发 —— 而那正是
    # chunked 那半边。`large_file` 是已知长度、且每次读都正好落在边界上，同一次
    # 读会让 `offset_ > last_`，**结构上**抓不住它。
    ("X9", "web_app_send_file_func", "small_file",
     [(F, '  const size_t n = static_cast<size_t>(nread);\n'
          '  offset_ += n;\n  bytes_sent_ += n;',
          '  const size_t n = static_cast<size_t>(nread);\n'
          '  offset_ += n;\n  bytes_sent_ += n;\n'
          '  if (n < slice_bytes_ && offset_ <= last_) '
          '{ submit_close(0); return; }  /* MUTATION X9 */')],
     "短读不补（把一次短读当成正常收尾）—— 抓住的是 chunked 半边的 "
     "small_file；**真正的文件中途短读**在本平台仍走不到（3b 步骤 1 的结论），"
     "是两件事"),

    ("X10", "web_app_send_file_func", "read_error_midstream",
     [(R, '  if (status != 0 && !head_sent_) {',
          '  if (status != 0) {  /* MUTATION X10 */')],
     "中途读失败改成走 fail_stream_before_head（想改状态码）"),

    ("X11", "web_app_send_file_func", "static_caches_small",
     [(S, '  if (static_cast<uint64_t>(pr.size) > static_cast<uint64_t>(thr)) {',
          '  if (true) {  /* MUTATION X11 */')],
     "静态阈值无视，一律走流式"),

    ("X12", "web_app_send_file_func", "static_streams_large",
     [(S, '  if (static_cast<uint64_t>(pr.size) > static_cast<uint64_t>(thr)) {',
          '  if (false) {  /* MUTATION X12 */')],
     "静态阈值无视，一律走整读"),

    ("X13", "web_app_app_func", "sent_bytes_head",
     [(A, '  r.sync_meta();\n\n  uvcpp_web_sent_info info;\n'
          '  info.status_code   = r.status_code();\n'
          '  info.connection_id = ctx.connection_id();\n'
          '  info.ok            = true;',
          '  uvcpp_web_sent_info info;\n'
          '  info.status_code   = r.status_code();\n'
          '  info.connection_id = ctx.connection_id();\n'
          '  info.ok            = true;\n'
          '  /* MUTATION X13: body_bytes 采集早于 sync_meta() */\n'
          '  info.body_bytes    = r.body_size();\n\n  r.sync_meta();'),
      (A, '  info.body_bytes = r.body_size();\n\n  r.notify_sent(info);\n}',
          '  r.notify_sent(info);\n}')],
     "body_bytes 在 sync_meta() 之前采集（HEAD 下读到 GET 的长度）"),

    ("X13b", "web_app_app_func", "sent_bytes_compressed",
     [(A, '  http_->send_response(client, r.raw());',
          '  info.body_bytes = r.body_size();  /* MUTATION X13b: 压缩前采集 */\n'
          '  http_->send_response(client, r.raw());'),
      (A, '  info.body_bytes = r.body_size();\n\n  r.notify_sent(info);\n}',
          '  r.notify_sent(info);\n}')],
     "body_bytes 采集早于压缩（记成压缩前的长度）"),

    # 期望用例原本写的是 `client_disconnect_frees_transfer`（web_app_stream_resp_func），
    # **探针实测也是错的**：那一组断言的恰恰是**相反**的行为（"对端断开**不**结算
    # 响应流"，`web_app_stream_resp_func.cpp:934-937` 的注释原文），而且它跑在
    # 驱动原本那 5 个 target 里 —— 所以它在**结构上**不可能抓住这条变异。
    # 这段代码管的是**请求**流：`ctx->stream_abort()` 全仓只有这一个调用点，而它是
    # `deliver_abort()` 的唯一调用者。抓它的是 `web_app_stream_func::abort_mid_stream`
    # （Phase 3a 就在钉「对端断开 ⇒ on_abort 恰好一次」），探针实测 scoped rc=2、
    # 失败签名 `abort: on_abort **恰好一次**（实测 0）`。
    ("X14", "web_app_stream_func", "abort_mid_stream",
     [(A, '          if (e->ctx->streaming()) victims.push_back(e->ctx);',
          '          /* MUTATION X14: 断连时不通知流对象 */\n'
          '          (void)e;')],
     "断连时不通知流对象 —— 抓它的是**请求**流的 abort_mid_stream，"
     "而不是响应流的 client_disconnect_frees_transfer（后者断言的是相反的现状）"),
]


def read_text(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8")


def write_text(p, s):
    with open(p, "wb") as f:
        f.write(s.encode("utf-8"))


# 本仓的行尾**不统一**（`uvcpp_http_server.cpp` / `uvcpp_web_app.cpp` 是 CRLF，
# 其余是 LF），而变异表的 old/new 一律按 LF 写。所以匹配前先把文件折成 LF、
# 施加之后按原样折回去 —— 否则锚点会在 CRLF 文件里**一个都命中不了**，而
# "施加失败"看起来像锚点写错了，实际是行尾。还原仍走原始字节，逐字节可比。
def eol_of(s):
    return "\r\n" if "\r\n" in s else "\n"


def lf(s):
    return s.replace("\r\n", "\n")


def to_eol(s, eol):
    return s if eol == "\n" else s.replace("\n", "\r\n")


def md5(p):
    with open(p, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def run(cmd, cwd=ROOT):
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=RUN_TIMEOUT_S)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace")
        return 124, out + "\n*** TIMEOUT %ds: %s\n" % (RUN_TIMEOUT_S, " ".join(cmd))


def build(tree, log):
    log.write("--- 构建 %s\n" % tree)
    rc, out = run(["cmake", "--build", tree, "--config", "Release",
                   "--parallel", "4"])
    nerr = len(re.findall(r"error C\d+|error LNK|error MSB", out))
    for ln in out.splitlines():
        if re.search(r"error C\d+|error LNK|error MSB", ln):
            log.write("    " + ln.strip() + "\n")
    log.write("    构建 rc=%d errors=%d\n" % (rc, nerr))
    return rc, nerr, out


def exe_of(target, tree):
    return os.path.join(ROOT, tree, "tests", "functional", "Release",
                        TARGETS[target][0] + ".exe")


def scoped_cmd(target, case, tree):
    path = exe_of(target, tree)
    if TARGETS[target][1] == "--only":
        return [path, "--only", case]
    return [path, case]


CASE_RE = re.compile(r"^\[[a-z0-9_]+\]\s+(\S+)\s*$")


def failed_cases(out):
    lines = out.splitlines()
    res = []
    for i, ln in enumerate(lines):
        m = CASE_RE.match(ln.strip())
        if m and i + 1 < len(lines) and "FAIL" in lines[i + 1]:
            res.append(m.group(1))
    for ln in lines:
        m = re.search(r"\[FAIL\]\s*(.+?)\s*$", ln)
        if m:
            res.append(m.group(1))
    seen, uniq = set(), []
    for r in res:
        if r not in seen:
            seen.add(r)
            uniq.append(r)
    return uniq


def main():
    args = sys.argv[1:]
    tree = "build-webapp"
    only = None
    if "--tree" in args:
        tree = args[args.index("--tree") + 1]
    if "--only" in args:
        only = args[args.index("--only") + 1]

    report = None
    if "--report" in args:
        report = args[args.index("--report") + 1]

    muts = [m for m in MUTATIONS if only is None or only in m[0]]
    files = sorted({f for m in muts for (f, _o, _n) in m[3]})
    if report is None:
        report = ("D:/tmp/_x3c_report.txt" if os.path.isdir("D:/tmp")
                  else "_x3c_report.txt")
    log = open(report, "w", encoding="utf-8")

    def say(s=""):
        print(s, flush=True)
        log.write(s + "\n")
        log.flush()

    if only is not None:
        say("** 本次只跑 --only %s 选中的 %d 条 **" % (only, len(muts)))
    say("Phase 3c 变异驱动 | tree=%s | 变异 %d 条 | 涉及 %d 个源文件"
        % (tree, len(muts), len(files)))

    before, backup = {}, {}
    for f in files:
        src = read_text(os.path.join(ROOT, f))
        before[f] = md5(os.path.join(ROOT, f))
        backup[f] = src

    # 基线 1：构建干净
    rc, nerr, _out = build(tree, log)
    if nerr != 0:
        say("基线失败：构建输出里有 %d 个 error" % nerr)
        return 3
    run(["cmake", "--build", tree, "--config", "Release", "--target",
         "copy_test_dlls"], cwd=ROOT)

    # 基线 2：每个 (target, 期望用例) 必须先绿 —— 否则"红了"可能只是它不存在
    pairs = sorted({(m[1], m[2]) for m in muts})
    say("基线：%d 个 (target, 期望用例) 各自单跑一次" % len(pairs))
    for tgt, case in pairs:
        rc, out = run(scoped_cmd(tgt, case, tree))
        if rc != 0:
            say("  基线失败：%s / %s scoped rc=%d（这一组本来就红，或根本不存在）"
                % (tgt, case, rc))
            say("  " + "\n  ".join(out.splitlines()[-15:]))
            return 3
        say("    OK %-28s %s" % (tgt, case))

    caught, missed, unapplied, buildfail = [], [], [], []

    for name, tgt, case, edits, desc in muts:
        say("")
        say("=== %s  %s" % (name, desc))
        say("    target=%s  期望=%s" % (tgt, case))

        ok = True
        for (f, old, new) in edits:
            path = os.path.join(ROOT, f)
            raw = read_text(path)
            eol = eol_of(raw)
            src = lf(raw)
            if src.count(old) != 1:
                say("    施加失败：%s 里命中 %d 次（要求恰好 1 次）"
                    % (f, src.count(old)))
                ok = False
                break
            write_text(path, to_eol(src.replace(old, new, 1), eol))
        if not ok:
            for f in files:
                write_text(os.path.join(ROOT, f), backup[f])
            unapplied.append(name)
            continue

        rc, nerr, out = build(tree, log)
        if nerr != 0:
            say("    构建失败（%d 个 error）—— 不计入抓住，如实记" % nerr)
            for f in files:
                write_text(os.path.join(ROOT, f), backup[f])
            buildfail.append(name)
            continue
        run(["cmake", "--build", tree, "--config", "Release", "--target",
             "copy_test_dlls"], cwd=ROOT)

        rc_scoped, out_scoped = run(scoped_cmd(tgt, case, tree))
        rc_full, out_full = run([exe_of(tgt, tree)])
        red = failed_cases(out_full)

        # **立刻还原**，再分析 —— 分析期间 src/ 不许停在变异状态
        for f in files:
            write_text(os.path.join(ROOT, f), backup[f])

        caught_it = (rc_scoped != 0 and rc_full != 0)
        says = "scoped rc=%-4d full rc=%-4d" % (rc_scoped, rc_full)
        if caught_it:
            caught.append(name)
            say("    抓住    %s" % says)
        else:
            missed.append(name)
            say("    没抓住  %s" % says)
        if red:
            say("    全量红了的用例：%s" % ", ".join(red[:8]))
        if rc_scoped != 0 and rc_full == 0:
            say("    （scoped 红而全量绿：不可能，除非时序）")
        if out_scoped:
            for ln in out_scoped.splitlines():
                if "[FAIL]" in ln:
                    say("      " + ln.strip())

    # 尾巴：还原 → 校验 → 重建 → 复跑
    say("")
    say("=== 尾巴")
    bad = []
    for f in files:
        write_text(os.path.join(ROOT, f), backup[f])
        if md5(os.path.join(ROOT, f)) != before[f]:
            bad.append(f)
    say("逐字节还原: %s" % ("是" if not bad else "否 -> " + ", ".join(bad)))
    rc, out = run(["grep", "-rn", "MUTATION", "src/"], cwd=ROOT)
    hits = [l for l in out.splitlines() if l.strip()]
    say("grep MUTATION src/ 残留: %s" % (("(无)" if not hits else "; ".join(hits))))
    rc, nerr, _out = build(tree, log)
    say("还原后重建 errors=%d" % nerr)
    run(["cmake", "--build", tree, "--config", "Release", "--target",
         "copy_test_dlls"], cwd=ROOT)
    for tgt in sorted({m[1] for m in muts}):
        rc, _o = run([exe_of(tgt, tree)])
        say("复跑 %-28s rc=%d" % (TARGETS[tgt][0], rc))

    say("")
    say("抓 住 %d/%d：%s" % (len(caught), len(muts), ", ".join(caught)))
    say("没抓住 %d：%s" % (len(missed), ", ".join(missed)))
    if unapplied:
        say("施加失败 %d：%s" % (len(unapplied), ", ".join(unapplied)))
    if buildfail:
        say("构建失败 %d：%s" % (len(buildfail), ", ".join(buildfail)))
    log.close()
    return 0 if not (missed or unapplied or buildfail) else 1


if __name__ == "__main__":
    sys.exit(main())
