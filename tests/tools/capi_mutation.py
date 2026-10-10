#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C ABI（`src/capi/`）的变异表 —— 仓内可重跑的那一份。

为什么要有这个文件
------------------
C 门的五条承重规矩（甲、不透明句柄 + 活句柄表；乙、异常不过边界；丙、回调表按
`size` 逐格看；丁、所有权三类；戊、线程纪律）里，**甲、乙、丙**都是"我们很小心"
很容易写成、而"到底有没有用"很容易想当然的三条。这个驱动把它们各自的守卫**逐条
拆掉**，看 `tests/capi/` 那几个纯 C 用例（`test_capi_common_func` 量甲、丙，
`test_capi_net_func` 量乙和线程纪律，`test_capi_webapp_func` 量甲在 webapp 那侧的
**回调期句柄**形态与"查询不存在"那条约定，`test_capi_h2_func` 量它们在 http2 那侧
的同几种形态）**有没有一声响**。没响的如实记成覆盖缺口，不许写"等价"。

被测的不变式（每条都在用例里有对应的断言）
------------------------------------------
  A. 句柄失效之后再用要给 `UVCPP_C_E_STALE`，不给段错误。守卫是**两道**：
     `unregister_head()` 里既 `registry_remove()` 又 `poison_head()`。见 M1~M3。
     这条**必须拆成两句判**，而且第二句是第一版漏掉的：
       A1. 释放后再用/双重释放给 `E_STALE`（M1~M3 三种情况下都绿 —— 它量的是
           分配器，见 M2 上面那段）；
       A2. `uvcpp_c_live_handle_count()` 收支平衡（只有它能咬 M2/M3）。M1 按
           设计活下来。批 3a 起
           `capi_h2_func` 也有这条断言（M16 全靠它）。
     A 在 webapp 那侧还多一种形态：**回调期句柄**（`req` / `resp` 那些栈对象）
     出了回调就不许再用了。守卫是 `FrameScope` 的析构里那句"全部摘表"。
     见 M8；h2 那侧是同一个 `FrameScope` 的同一句（`stream` 那枚），见 M16。
  B. 句柄**类型**也要对：服务端句柄传给客户端那一族函数必须是 `E_STALE`。这一条
     打的只是第二道（魔数），见 M5。
  C. 回调表**只读调用方声明覆盖到了的那几格**：老客户端写一张只有前几格的表，
     后面那几格的字节**一个都不许读**。见 M4；"连 `size` 自己都不足 4 字节"
     那半条见 M15（h2 那侧）。
  D. C++ 异常不许穿过 `extern "C"`。见 M6 —— 判断它"没响"的标准是**进程没了**
     （abort），因为那正是"异常逃出去"的真实表现。
  E. 头里的 `UVCPP_C_ABI_VERSION` 与库返回的那个数必须一致（P/Invoke 最常见的
     故障：头与 .so/.dll 不是一次编出来的）。见 M7。
  F. **"查不到"与"查到空值"是两件事**（`1.4.3` 加的 `UVCPP_C_E_NOT_FOUND`）：
     `header("X-Nope")` 必须是负数，不能退化成 `0` —— 退化了就把"没带这个头"
     与"带了这个头但值是空的"合并成同一个回答。见 M9；h2 那侧同一条规矩的另一种
     形态（"响应头还没到"与"状态码是 0"在那层数据里是同一个值）见 M14。
  G. 入参那一层要**当场**拒绝（`E_INVALID_ARG`），不能"注册成功、运行时才炸"。
     C 面最容易漏的就是这个：C 调用方没有编译期检查，一个 NULL 回调会一路走到
     事件循环里。见 M10。
  H. 客户端的"连上了没有"必须问**底下那条连接**，不能问 C++ 那层按位或的状态
     （`CONNECTED` 一旦置上就再也不会清）。见 M11。这条只有"关掉之后还不肯承认
     已断开"这种方向能量出来 —— 所以断言写在 `close()` 之后。
  I. `uvcpp_c_ws_client_connect` 只认 `ws://`：别的 scheme 当场 `E_INVALID_ARG`，
     不许往下走成"某个连接错误"。见 M12。

怎么判"抓住了"
--------------
与别的驱动同一条：(1) 某个用例的退出码非零，**且** (2) 它自己那行
`checks=… failures=…` 里 `failures` 非零。第二条是为了区分"这一组自己红的"和
"被别人的红带下去的"；三个用例是分开跑的，所以它天然成立，但仍然逐条印出来。
`rc` 是信号（M6 预期就是它）时不会有 `checks=` 那行 —— 那种情况下**非零退出码
就是判据**，脚本会照实写明"没有 checks= 那一行"，免得读的人以为它是一句 FAIL。

对**预期活下来**的变异，除三个用例照常跑之外**再跑一次整棵树**的 ctest；全绿才
算那条判定站得住（`doc/testing-guide.md` 的规矩，与 `run_idle_mutation.py` M1、
`http3_mutation.py` 同一条）。

用法
----
    python3 tests/tools/capi_mutation.py [--tree build-capi] [--jobs 8]

要的是一棵 **CAPI=ON** 的树（本机怎么配见 `doc/capi-guide.md` §怎么开）。
每一段变异还各自要一棵**开着它那个模块**的树：M13~M16（h2 那四条）要
`UVCPP_ENABLE_NGHTTP2`，M17~M20（quic + h3 那四条）要 `UVCPP_ENABLE_HTTP3`
—— 那两个用例各自跟着这两个开关走。选了某个变异而这棵树没编它要的那个 exe 时，
脚本**退 3**（「没判」，见 `main()` 里那段），不会拿 `run_targets()` 给的 127
去冒充"抓住了"；反过来，这棵树没编、而这一趟**没**选它的变异，照跑剩下的。

脚本改源码、重编、再按字节还原，结束时核对 md5 并再跑一次确认全绿；任一步不对
就非 0 退出。
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

INTERNAL = os.path.join(ROOT, "src", "capi", "uvcpp_c_internal.h")
COMMON = os.path.join(ROOT, "src", "capi", "uvcpp_c_common.cpp")
NET = os.path.join(ROOT, "src", "capi", "uvcpp_c_net.cpp")
WEB = os.path.join(ROOT, "src", "capi", "uvcpp_c_web.cpp")
WEBAPP = os.path.join(ROOT, "src", "capi", "uvcpp_c_webapp.cpp")
HTTP2 = os.path.join(ROOT, "src", "capi", "uvcpp_c_http2.cpp")
QUIC = os.path.join(ROOT, "src", "capi", "uvcpp_c_quic.cpp")
HTTP3 = os.path.join(ROOT, "src", "capi", "uvcpp_c_http3.cpp")
DB = os.path.join(ROOT, "src", "capi", "uvcpp_c_db.cpp")

# 纯 C 用例（在 <tree>/tests/capi/ 下）。顺序无所谓，超时都是 60 s。
TARGETS = [
    "test_capi_common_func",
    "test_capi_net_func",
    "test_capi_webapp_func",
]

# 第四个用例**跟着 `UVCPP_ENABLE_NGHTTP2` 走**，不是跟着 CAPI 走（判据在
# `tests/capi/CMakeLists.txt` 里，与 `src/capi/uvcpp_c_http2.*` 同一个开关）。
# 所以一棵 `CAPI=ON, NGHTTP2=OFF` 的树是**合法**的，那棵树里这个 exe 根本不存在
# —— 那种情况下它不是"跑红了"，是"没得跑"。M13~M16 只碰 `uvcpp_c_http2.cpp`，
# 少了这个 exe 就一条都判不了，于是 main() 里显式分两种情形处理（见那里的两段
# 注释），而不是让基线报一个 127 出来冒充"红了"。
H2_TARGET = "test_capi_h2_func"

# 第五个用例跟着 `UVCPP_ENABLE_HTTP3` 走（`tests/capi/CMakeLists.txt` 里那一条
# `if(UVCPP_ENABLE_HTTP3)`，而守卫链保证 HTTP3 ⇒ QUIC ⇒ OPENSSL ⇒ WEB）。
# M17~M20 碰的是 `uvcpp_c_quic.cpp` / `uvcpp_c_http3.cpp`，两处都在它的覆盖范围
# 里，所以这里**一个目标管两条腿**（QUIC 与 HTTP3 的 C 面）—— 本机那棵
# `build-capi-h3` 就是这么配的。少了这个 exe 时与 h2 那条同样的处理：退 3
# （「没判」），不是拿一个 127 去冒充"抓住了"。
H3_TARGET = "test_capi_quic_h3_func"

# 第六个用例跟着 `UVCPP_ENABLE_DB` 走 —— 这一片**唯一**有自己开关的，与
# `src/capi/uvcpp_c_db.*` 同一个开关（`tests/capi/CMakeLists.txt` 里那一条）。
# ★ 它比其他几个多一个前提：**这棵树得有一个能连的后端**。用例的后端是从
#   `UVCPP_DB_TEST_*_URL` 与 `UVCPP_DB_SQLITE_ENABLE` 里挑的，一个都没有时它
#   退 3。本机与 CI 的 `capi` 腿都开着 SQLite，所以正常情形下它是真跑的；
#   万一落到"没后端"那一档，`run_targets()` 会看到 rc=3 → 记成红，也就是
#   **判不了就报红**，不会假装抓住/没抓住。（要跑这棵树的 db 变异，务必让
#   SQLite 开着 —— 配树命令见 doc/capi-guide.md。）
DB_TARGET = "test_capi_db_func"

# 卸掉守卫的两句原文（`unregister_head()` 的函数体），M1~M3 共用。
UNREGISTER_BODY = ("  registry_remove(h);\n"
                   "  poison_head(reinterpret_cast<handle_head&>(*h));")

# (标签, 预期[True=抓得住], [(文件, 原文, 替换), …])
#
# 预期是怎么来的（**跑出来之后才写的，不是想当然**）：M1~M3 这三条第一版按
# "拆掉任何一道守卫都该红"写成 True/True/True，本机一跑，M1、M2、M3 **全都没抓住**。
# 追下去发现病不在守卫，在**用例**：`*_free()` 之后那块内存的第一个字节在 glibc 上
# 被 tcache 的 `next` 指针改写了，于是 `alive()` 读魔数那一句照样读出一个不等于
# 魔数的值 —— "释放后再用必须给 E_STALE"这几条断言量到的是**分配器**，不是守卫。
# 补上 `uvcpp_c_live_handle_count()` 与它那条收支平衡断言之后（登记表**只增不减**
# 这件事没有别的可观测形式），M2、M3 才真的红起来；M1 依然是"没抓住"，而这一次
# 是**如实**的：摘表还在，所以残局照样被挡住，毒化在这一条路上是冗余的。
MUTATIONS = [
    # ---- 甲：句柄失效的两道守卫，逐道拆 ----
    #
    # M1 是**预期活下来**的那一条，理由要写清楚：两道守卫互为备份 ——
    # `alive()` 先问登记表（不读内存）、再读魔数。拆掉毒化之后，摘表那一句仍然
    # 让 `registry_has()` 为假。所以"拆一道看不出差别"是这两道守卫的**设计意图**，
    # 不是覆盖缺口；真正的缺口是"两道一起拆"曾经也看不出差别（见 M3）。
    ("M1 只拆毒化（留摘表）",
     False,
     [(INTERNAL, UNREGISTER_BODY,
       "  registry_remove(h);  /* MUTATION: 不毒化 */")]),

    # M2：摘表没了 => 登记表**只增不减** => 活句柄数不回到基线。
    # 注意它红的是 `test_live_handle_count_balances()` 那一条，**不是**
    # "释放后再用给 E_STALE"那一组（那组在 M1/M2/M3 三种情况下都是绿的）。
    ("M2 只拆摘表（留毒化）",
     True,
     [(INTERNAL, UNREGISTER_BODY,
       "  poison_head(reinterpret_cast<handle_head&>(*h));"
       "  /* MUTATION: 不摘表 */")]),

    # M3：同上，且此时**两道手段一个都不剩** —— 这正是"没有活句柄数就完全量不
    # 出来"的那一条。
    ("M3 两道一起拆",
     True,
     [(INTERNAL, UNREGISTER_BODY,
       "  /* MUTATION: 摘表与毒化都拆掉 */")]),

    # ---- 丙：`size` 那套"逐格看" ----
    ("M4 field_present 恒真（=整表读满）",
     True,
     [(INTERNAL,
       "  return static_cast<std::size_t>(size) >= off + sizeof(M);",
       "  return static_cast<std::size_t>(size) + 0x100000u >= off + sizeof(M);"
       "  /* MUTATION: 恒真 */")]),

    # ---- 甲的第二道：魔数（类型）----
    ("M5 alive() 不看魔数",
     True,
     [(INTERNAL,
       "  return reinterpret_cast<const handle_head*>(h)->magic == magic;",
       "  return true;  /* MUTATION: 不看魔数 */")]),

    # ---- 乙：异常收口 ----
    # 两处锚点合起来只把 `UVCPP_C_TRY` 换成 `{`、把 `UVCPP_C_CATCH(...)` 删掉：
    # `connect_wait` 的**可观察行为一个字都没变**（还是那句 `connect_wait` 会抛），
    # 变的只有"抛出来之后有没有人接"。所以这条变异红掉的形态是**进程终止**，
    # 不是某条断言失败 —— 见脚本头部"怎么判抓住了"那一段。
    #
    # 第一版这条锚点写坏过一次（替换后的花括号不配对，cpp 直接编不过，脚本按
    # "构建失败"退 3 停在那里）—— 那一次是**脚本自己的 bug**，不是变异被抓住了。
    # 构建失败一律当场退 3，不许当成"抓住了"，这一条是刻意的。
    ("M6 connect_wait 去掉异常收口",
     True,
     [(NET,
       "    uvcpp_c_tcp_client* c, const char* ip, int port, int timeout_ms) {\n"
       "  UVCPP_C_TRY\n",
       "    uvcpp_c_tcp_client* c, const char* ip, int port, int timeout_ms) {\n"
       "  {  /* MUTATION: 异常边界拆了（没有 try、也没有 catch） */\n"),
      (NET,
       "    const int arm = maybe_arm_read(c);\n"
       "    return arm == 0 ? UVCPP_C_OK : arm;\n"
       "  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)\n"
       "}\n",
       "    const int arm = maybe_arm_read(c);\n"
       "    return arm == 0 ? UVCPP_C_OK : arm;\n"
       "  }  /* MUTATION: 没有 catch，异常直接穿过 extern \"C\" */\n"
       "}\n")]),

    # ---- 戊之外的一枚地基：ABI 自洽 ----
    ("M7 abi_version 与头里的宏不一致",
     True,
     [(COMMON,
       "  return static_cast<unsigned int>(UVCPP_C_ABI_VERSION);",
       "  return static_cast<unsigned int>(UVCPP_C_ABI_VERSION) + 1u;"
       "  /* MUTATION */")]),

    # =====================================================================
    # 批 2（webapp + web）。下面五条都只有 `test_capi_webapp_func` 咬得住
    # —— 批 1 的两个用例一个都不碰 webapp/web 那两片源码。
    # =====================================================================

    # ---- 甲在 webapp 那侧的形态：回调期句柄出了回调就不许再用 ----
    #
    # `FrameScope::~FrameScope()` 里那句循环是**唯一**一处"回调返回后把这些栈
    # 对象摘出登记表"。拆掉它之后，**只有** `uvcpp_c_live_handle_count()` 那条
    # 断言会红（`= 5, want 0`）。用例里另外那两条"把回调期句柄带出回调再问它"
    # 的 `E_STALE` 断言**照样过**。
    #
    # 原本这里的预期是"那两条会红在 `UVCPP_C_E_WRONG_THREAD` 上"，**实测不是**
    # —— 所以别照着那个预期去改用例。原因是 `creq` / `cresp` 是
    # `route_trampoline` 的**栈上局部量**：回调一返回，那块栈立刻被后续的循环
    # 代码复用，魔数被无关的写入盖掉，`alive()` 在"读魔数"那一句就判假，根本走
    # 不到线程检查。也就是说**只要句柄住在栈上，"魔数"这个判据就区分不出"被毒
    # 化"和"被栈复用盖掉"**。
    #
    # 那两条断言量的是**契约**（"回调之外问它必须给 `E_STALE`，且不许是 UB"），
    # 这条变异量得出来的是**机制**（登记表到底有没有摘干净）—— 两者都该有，错在
    # 拿前者当后者的证据。完整经过见 doc/capi-guide.md §6 的"第二次假绿"。
    ("M8 回调期句柄不摘表（FrameScope 拆掉）",
     True,
     [(WEBAPP,
       "    for (int i = n_ - 1; i >= 0; --i) unregister_head(slots_[i]);",
       "    /* MUTATION: 回调返回后不摘表 */")]),

    # ---- F：查不到 ≠ 空值 ----
    ("M9 查不到的头退化成 0",
     True,
     [(WEB,
       "    if (!resp->resp->has_header(name)) return UVCPP_C_E_NOT_FOUND;",
       "    if (!resp->resp->has_header(name)) return 0;  /* MUTATION */")]),

    # ---- G：入参当场拒绝 ----
    ("M10 serve_static 不查空参",
     True,
     [(WEBAPP,
       "    if (prefix == nullptr || root_dir == nullptr) "
       "return UVCPP_C_E_INVALID_ARG;",
       "    /* MUTATION: 空参不查（让它一路走到运行时） */")]),

    # ---- H：连上了没有要问底下那条连接 ----
    ("M11 is_connected 直接说自己连着",
     True,
     [(WEB,
       "    const bool up = tcp->has_status(uv::TCP_CLIENT_CONNECTED) &&\n"
       "                    !tcp->has_status(uv::TCP_CLIENT_CLOSING) &&\n"
       "                    !tcp->has_status(uv::TCP_CLIENT_CLOSED);\n"
       "    return up ? 1 : 0;\n",
       "    return 1;  /* MUTATION: 不查底层连接 */\n")]),

    # ---- I：只认 ws:// ----
    ("M12 ws_client_connect 不查 scheme",
     True,
     [(WEB,
       '    const std::string u(url);\n'
       '    if (u.compare(0, 5, "ws://") != 0) {',
       '    const std::string u(url);\n'
       '    if (false) {  /* MUTATION: 不查 scheme */')]),

    # =====================================================================
    # 批 3a（http2）。下面四条都只有 `test_capi_h2_func` 咬得住。
    #
    # ★ 这四条要求树里**开着 NGHTTP2**（那个用例跟着这个开关走）。树里没有它
    #   时 main() 直接退 3（"没判"），不会拿一个 127 冒充"抓住了"。
    # =====================================================================

    # ---- 戊：线程纪律，h2 那一侧 ----
    #
    # 服务端那条线程上：连接活着的时候从**另一条线程**喊 `flush`。C 面的承诺
    # 是当场 `E_WRONG_THREAD`。拆掉这一句之后 `c->conn->flush()` 会在没有 loop
    # 亲和的那条线程上跑起来 —— 要么返回 0（断言拿到 0、红），要么直接崩
    # （rc 非 0，也是红）。两种都算抓住，所以这条的判据不依赖它在哪一边。
    ("M13 h2 flush 不查线程",
     True,
     [(HTTP2,
       "    if (!ok(c)) return UVCPP_C_E_STALE;\n"
       "    if (!uvcpp_c_detail::thread_ok(&c->head)) "
       "return UVCPP_C_E_WRONG_THREAD;\n"
       "    return c->conn->flush();",
       "    if (!ok(c)) return UVCPP_C_E_STALE;\n"
       "    /* MUTATION: 不查线程 */\n"
       "    return c->conn->flush();")]),

    # ---- F 在 h2 那一侧的形态：响应头还没到 ≠ 状态码是 0 ----
    #
    # `h2_response_not_received()` 把 `status_code` 填成 `HTTP_STATUS_NONE`
    # （就是 0），所以"还没到"和"到了但状态码是 0"在这层数据里是**同一个值**。
    # C 面把它译成 `E_NOT_FOUND`，这里拆成"照实返回 0"。用例在请求回调里就问
    # 一次（那时头当然还没到），两条流各一次 —— 拿到 0 就红。
    ("M14 h2 response_status 把'没到'当 0 返回",
     True,
     [(HTTP2,
       "    if (code == static_cast<int>(uvcpp::HTTP_STATUS_NONE)) {\n"
       "      return UVCPP_C_E_NOT_FOUND;\n"
       "    }",
       "    if (code == static_cast<int>(uvcpp::HTTP_STATUS_NONE)) {\n"
       "      return 0;  /* MUTATION: 把'没到'当成 0 */\n"
       "    }")]),

    # ---- 丙在 h2 那一侧的形态：回调表先看 `size` ----
    #
    # 和 M4（`field_present` 恒真）量的是**同一条规矩的两半**：M4 管"逐格问你
    # 覆盖到哪了"，这条管"连你自己声明的 size 都不足 4 字节时，整张表都不读"。
    # 用例拿一张 `size = 0` 的表去 `start`，必须当场 `E_INVALID_ARG`。
    #
    # ★ 这条的预期是**改过用例之后**才成立的，写在这里免得下次又猜：按 True 写
    #   （"用例里明明有那么一次坏表调用"），本机一跑**没抓住，四个用例全绿**。
    #   病不在守卫，在**用例**：那两处坏表调用喂的都是 `h2_cbs` 那一格
    #   （`capi_h2_func.c` 里两张 `uvcpp_c_h2_callbacks`），而这条拆的是 `start`
    #   里**另一条** `table_size_ok`（`conn_cbs`）。同一句守卫的两个分支，只量了
    #   一个 —— 一个覆盖缺口，不是"等价"。补上"`h2` 表缺席、`conn` 表 `size = 0`"
    #   那一次调用（服务端那段，注释里带 ★）之后这条才真的红，而且红得像一份
    #   事故报告：那一次 `start` 居然返回 0（副作用全落地了），紧接着真 `start`
    #   拿到 `-114`（`UV_EALREADY` —— 会话已经被上一趟开起来了）、GOAWAY 也没了。
    ("M15 h2 连接回调表的 size 不校验",
     True,
     [(HTTP2,
       "    if (conn_cbs != nullptr && "
       "!uvcpp_c_detail::table_size_ok(conn_cbs->size)) {",
       "    if (false && conn_cbs != nullptr) {  /* MUTATION: 不查 size */")]),

    # ---- 甲在 h2 那一侧的形态：回调期句柄出栈即摘表 ----
    #
    # ★ 这一条与 M8 是**同一件事、同一个代价，但红在哪一条断言上不一样**，写
    #   在这里免得下次又猜：h2 用例里那三条"把 stream 带出回调再问它"的
    #   `E_STALE` 断言**照样过**（理由与 M8 完全相同：栈被复用，魔数早没了），
    #   真正咬住它的是这一趟跑完时的 `uvcpp_c_live_handle_count() == 0` ——
    #   那也是加这条断言的全部理由。
    ("M16 h2 回调期句柄不摘表（FrameScope 拆掉）",
     True,
     [(HTTP2,
       "    for (int i = n_ - 1; i >= 0; --i) {\n"
       "      uvcpp_c_detail::unregister_head(slots_[i]);\n"
       "    }",
       "    /* MUTATION: 回调返回后不摘表 */")]),

    # ---- 丙在 quic 那一侧的形态：**端点那份**回调表也先看 `size` ----
    #
    # 与 M4、M15 量的是同一条规矩，但落在**另一张表**上：M4 拆的是逐格问
    # "你覆盖到哪了"（`field_present`），M15 拆的是 h2 那份表的 `size` 自检，
    # 这一条拆的是 `uvcpp_c_quic_callbacks` 的自检。
    #
    # ★ 这一格**只有返回码能当判据**，写在这里免得下次以为断言写少了：`size = 3`
    #   那张截断表里的格本来就不会被填（连 `field_present` 都轮不到），所以
    #   "装上了"与"没装上"在**行为**上长得一模一样。拆掉守卫之后，服务端那次
    #   `_conn_set_callbacks()` 与客户端那次都返回 `0`，而用例断言的是
    #   `E_INVALID_ARG` —— 红在两句话上，不在行为上。用例里那两处注释写着同一件事。
    ("M17 quic 端点回调表的 size 不校验",
     True,
     [(QUIC,
       "  if (!uvcpp_c_detail::table_size_ok(table->size)) {\n"
       "    uvcpp_c_detail::set_last_error(\n"
       "        \"uvcpp_c_quic_callbacks.size 连第一格都没盖住\");\n"
       "    return UVCPP_C_E_INVALID_ARG;\n"
       "  }",
       "  /* MUTATION: 不查 size */")]),

    # ---- 甲在 quic + h3 那一侧的形态：借来的连接句柄由谁反登记 ----
    #
    # 这一条是批 3b 最要紧的一格，理由在 `uvcpp_c_quic.cpp` 里那段注释里：
    # **装上 h3 之后，本模块 `on_close` 那套跳板被 h3 整个换掉**，于是"这条
    # 连接没了"这件事只剩 h3 的 `on_disconnect` 一个入口。这里把那个入口摘掉，
    # 借出的 `uvcpp_c_quic_connection` 就永远留在登记表里。
    #
    # ★ 它会从哪一句上红，本机跑出来之后才知道（见文件头"预期是怎么来的"）：
    #   最直接的是收尾那句 `uvcpp_c_live_handle_count() == 0`，但**不排除**它先
    #   在别处炸掉 —— 句柄还活着、`conn` 还指着一个已经析构掉的 C++ 对象，
    #   `_conn_state()` 就是在读一块野内存。两种都算抓住（一个是 rc 非 0 且
    #   `checks=` 那行照旧在，另一个是进程直接没了），驱动里都印出来。
    ("M18 h3 on_disconnect 不毒化借来的 QUIC 句柄",
     True,
     [(HTTP3,
       "  uvcpp_c_detail::quic_conn_detach(qconn);",
       "  /* MUTATION: 不毒化借来的连接句柄 */")]),

    # ---- 甲在 h3 那一侧的形态：回调期句柄出栈即摘表 ----
    #
    # 与 M8（webapp）、M16（h2）是同一件事的第三处，代价与红在哪一句上也同一个
    # 形状：那几条"把 view 带出回调再问它"的 `E_STALE` 断言**未必**会红（栈被
    # 复用，魔数早没了 —— 见 M8 那段），真正咬住它的是收尾的
    # `uvcpp_c_live_handle_count() == 0`。断言写在哪儿由本机跑出来的结果说了算。
    ("M19 h3 回调期句柄不摘表（FrameScope 拆掉）",
     True,
     [(HTTP3,
       "    if (h_ != nullptr) uvcpp_c_detail::unregister_head(h_);",
       "    /* MUTATION: 回调返回后不摘表 */")]),

    # ---- G 在 h3 那一侧的形态：流号是负数要**当场**拦 ----
    #
    # `-1` 是"还没有归属"的日子值（QUIC 里 0 是一条**真的**流），不是一句
    # "随便一个流"。C++ 那侧的 `send_response()` 会以 `UV_EINVAL` 拒收，但那是
    # **发的时候**才知道；这里拆掉的是"设的时候就拦住"那一句。用例在构造器那
    # 一段问了一次：`_set_stream_id(resp, -1)` 必须 `E_INVALID_ARG`。
    ("M20 h3 _set_stream_id 不拦负数",
     True,
     [(HTTP3,
       "    if (stream_id < 0) return UVCPP_C_E_INVALID_ARG;\n"
       "    resp->resp.stream_id = stream_id;",
       "    resp->resp.stream_id = stream_id;  /* MUTATION: 负数照收 */")]),

    # ---- quic 端点收尾：带着活连接 free，别边遍历边 erase ----
    #
    # 这一枚**不是想出来的**：它是 C# 例子（`bindings/csharp/examples/QuicEcho`）
    # 第一次跑通回显之后暴露的必现 SIGSEGV —— `_server_free()` 边遍历
    # `server->conns` 边 `detach_conn()`，而后者会从那张表里 `erase`（"反登记"
    # 那一半），`unordered_map::erase` 把**当前这个迭代器**弄失效，`++it` 踩在
    # 已回收的桶上。修复是"先把句柄抄进一个局部数组，再统一 detach"。
    #
    # ★ 这条变异第一版**抓不住**，写在头里省得下次误判：本目录批 3 的 QUIC 用例
    #   清一色是"先关连接、再释放端点"，`_free()` 那一刻 `conns` 永远是空的 ——
    #   空的（或只有一条也罢，`it` 失效后 `++it` 同样是 UB）都轮不到。真正咬住它
    #   的是 `capi_quic_h3_func.c` 里补的 `test_free_with_live_conn()`（自签 TLS +
    #   一次真握手，**不 close** 直接 free）。所以加这条变异与加那条用例是同一件
    #   事的两半，缺一个这格就永远是空的。
    #
    # 它红的方式与 M18 同一档：进程直接没了（rc 139），因此抓没抓住**不看
    # `failures=`**，看的是"没有 `checks=` 那一行"或者活句柄数对不上。
    ("M21 quic server_free 边遍历 conns 边 detach（带着活连接释放）",
     True,
     [(QUIC,
       "    std::vector<uvcpp_c_quic_connection*> live_handles;\n"
       "    live_handles.reserve(server->conns.size());\n"
       "    for (std::unordered_map<uvcpp::uvcpp_quic_connection*,\n"
       "                            uvcpp_c_quic_connection*>::iterator it =\n"
       "             server->conns.begin();\n"
       "         it != server->conns.end(); ++it) {\n"
       "      live_handles.push_back(it->second);\n"
       "    }\n"
       "    for (size_t i = 0; i < live_handles.size(); ++i) {\n"
       "      detach_conn(live_handles[i]);\n"
       "    }\n",
       "    /* MUTATION: 边遍历 conns 边 detach —— 后者 erase 掉了当前迭代器 */\n"
       "    for (std::unordered_map<uvcpp::uvcpp_quic_connection*,\n"
       "                            uvcpp_c_quic_connection*>::iterator it =\n"
       "             server->conns.begin();\n"
       "         it != server->conns.end(); ++it) {\n"
       "      detach_conn(it->second);\n"
       "    }\n")]),

    # ---- db（`uvcpp_c_db.cpp`）----
    #
    # 这几条量的都是**C 面特有**的那些判据 —— C++ 那侧有 RAII、有异常、有类型
    # 挡着的东西，到 C 面只能变成一句显式的判断，而"那句判断在不在"正是这一片
    # 最容易被悄悄改掉的地方（改掉之后 C++ 那套用例一个字都不会红）。

    # M22：`column_index()` 的"没有这一列"。0 是个**合法下标**，拿它当"没有"
    # 就是撒谎 —— 调用方会去读第 0 列，而且读到的东西看着还挺正常。
    ("M22 db column_index 的没有这一列退化成 0",
     True,
     [(DB,
       "    if (idx == std::string::npos) return UVCPP_C_E_NOT_FOUND;",
       "    if (idx == std::string::npos) return 0;  /* MUTATION: 拿 0 冒充没有 */")]),

    # M23：越界的那一格。`at()` 越界返回的是**一枚静态 NULL 值**（可读），用例靠
    # 这一条把"这一格没有"变成 `_is_null()` 为真、不需要第二条分支。改成返回
    # NULL 之后，每个读格子的人都得先判空 —— 那正是这条设计要消掉的东西。
    ("M23 db table_cell 越界不给可读的 NULL 视图",
     True,
     [(DB,
       "    return wrap(&table->t.at(row, column));",
       "    if (row >= table->t.row_count() || column >= table->t.column_count())\n"
       "      return nullptr;  /* MUTATION: 越界给 NULL */\n"
       "    return wrap(&table->t.at(row, column));")]),

    # M24：池子借出的那枚句柄不能拿去 `_client_free()`。拆掉这一句之后，调用方
    # "顺手 free 掉"就真的把池子那条连接删了 —— 池子的账上还记着它，之后还回来
    # 的是一块已释放的内存。用例在借出之后立刻问了一次：`_client_free(a)` 必须
    # `E_STATE`。
    #
    # ★ 实测（本机 build-capi-all，2026-10-10）：**rc=-6（SIGABRT），没有
    #   `checks=` 那一行** —— 红在退出前那一趟里：连接被提前删掉之后池子收尾时
    #   又删了一次，glibc 的 tcache 直接 abort。也就是说这条变异不是被某条断言
    #   拦下的（那条断言其实也红了，只是没活到打印），而是被**分配器**拦下的。
    #   这正是 M1 那段注释讲的同一件事的反面：句柄守卫拆掉之后，最后的防线变成了
    #   分配器，而分配器给的是 abort，不是一个能读懂的返回码。
    ("M24 db 借来的句柄也能 _client_free",
     True,
     [(DB,
       "    if (!client->owned) return UVCPP_C_E_STATE;",
       "    /* MUTATION: 借来的也照放 */")]),

    # M25：池子被异步门面绑着时不许 free。这是本片唯一一处"次序约束必须当场
    # 报错"的地方：门面在池线程里借还连接，池子一没它就在读已释放的内存。
    #
    # ★ 实测：**rc=-11（SIGSEGV）** —— 门面的循环线程在池子没了之后照样接活，
    #   读的是已释放的池子。这条红得比 M24 更"远"：崩在另一条线程里，与本线程
    #   的断言完全无关，所以它只能靠 `alive()` 那一层拦在**调用那一刻**。
    ("M25 db 有门面绑着的池子也照 free",
     True,
     [(DB,
       "    if (owner_has_async(pool)) return UVCPP_C_E_STATE;",
       "    /* MUTATION: 门面绑着也照拆 */")]),

    # M26：投递时"这一笔的交付通道"必须给了。拆掉之后，一个 `on_table == NULL`
    # 的投递会被**收下**，然后在交付那一刻调一个空的函数指针。
    #
    # ★ 实测：**rc=-11（SIGSEGV）**，与预测的同一档。它红的方式值得记下来：一个
    #   "参数没给全"的调用，如果不在**投递时**拦掉，就会变成**在循环线程上**的
    #   一次跳转 —— 那时调用方早已返回，没有任何人能接住它。
    ("M26 db check_events 不收 NULL 交付通道",
     True,
     [(DB,
       "    if (!field_present(ev->size, &uvcpp_c_db_query_events::on_table) ||\n"
       "        ev->on_table == nullptr) {",
       "    if (!field_present(ev->size, &uvcpp_c_db_query_events::on_table)) {\n"
       "      /* MUTATION: on_table == NULL 也收 */")]),
]

RUN_TIMEOUT_S = 600


def read_bytes(p):
    with open(p, "rb") as f:
        return f.read()


def write_bytes(p, b):
    with open(p, "wb") as f:
        f.write(b)


def md5(p):
    return hashlib.md5(read_bytes(p)).hexdigest()


def run(cmd, cwd=ROOT, timeout=RUN_TIMEOUT_S):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, "*** TIMEOUT ***"
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def build(tree, jobs, targets):
    rc, out = run(["cmake", "--build", tree, "--target"] + targets +
                  ["--parallel", str(jobs)])
    errs = len(re.findall(r"\berror:", out)) + len(re.findall(r"error C\d+", out))
    return rc, errs, out


def run_targets(tree, targets):
    """-> [(名字, rc, failures, [FAIL 行], [c1, c2]), …]"""
    d = os.path.join(tree, "tests", "capi")
    rows = []
    for name in targets:
        exe = os.path.join(d, name)
        if not os.path.exists(exe):
            rows.append((name, 127, -1, ["（可执行文件不在：%s）" % exe], []))
            continue
        rc, out = run([exe], cwd=d, timeout=60)
        m = re.search(r"checks=(\d+) failures=(\d+)", out)
        fails = [s.strip().splitlines()[0]
                 for s in re.findall(r"FAIL ([^\n]*)", out)]
        if not m:
            fails.insert(0, "（没有 checks=/failures= 那一行 —— 进程可能是被"
                            "信号打断的，rc=%s）" % rc)
        rows.append((name, rc,
                     int(m.group(2)) if m else -1,
                     fails,
                     [int(m.group(1)), int(m.group(2))] if m else []))
    return rows


def ctest_whole_tree(tree):
    rc, out = run(["ctest", "--output-on-failure"], cwd=tree, timeout=1800)
    tot = re.search(r"\d+% tests passed, \d+ tests failed out of \d+", out)
    return rc, (tot.group(0) if tot else out[-200:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default="build-capi")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only", default=None,
                    help="只跑标签里含这个子串的变异（比如 M8）。基线照跑 —— 没有"
                         "基线就没有'抓住了'这句话的参照物。")
    args = ap.parse_args()

    tree = os.path.join(ROOT, args.tree)
    if not os.path.isdir(tree):
        print("没有这棵树：%s —— 先按 doc/capi-guide.md 配一棵 CAPI=ON 的。" % tree)
        return 3

    selected = MUTATIONS
    if args.only:
        selected = [m for m in MUTATIONS if args.only.lower() in m[0].lower()]
        if not selected:
            print("没有标签含 %r 的变异。" % args.only)
            return 3
        print("--only %s：只跑 %s" % (args.only, "、".join(m[0].split()[0]
                                                          for m in selected)))

    # 这一趟跑哪几个用例 —— **按这棵树实际编了哪几个决定**。
    #
    # 判据是"用例跟着它量的那个模块的开关走"（`tests/capi/CMakeLists.txt`）：
    # webapp 那份跟 `UVCPP_BUILD_WEBAPP`、h2 那份跟 `UVCPP_ENABLE_NGHTTP2`、
    # quic+h3 那份跟 `UVCPP_ENABLE_HTTP3`。所以"这棵树里没有某个 exe"是一个
    # **合法**情形（本机那棵 build-capi-h3 就没开 WEBAPP，build-capi 没开
    # NGHTTP2），不是"跑红了"，也不是"通过"。
    #
    # 分两种情形处理，**不许混成一种**：
    #   - 那几条变异碰的源文件**不在**这个 exe 的覆盖范围里 → 照跑剩下的；
    #   - 在里面 → 直接退 3，说清"这一趟什么都没判"。
    # 退 3 而不是退 1：`run_targets()` 对不存在的 exe 会给一个 127 行，那个 127
    # 长得和"用例真红了"一模一样，拿它去填"抓住了"是把没判写成判过。
    # （前两格没有可关的开关，恒在。）
    # (用例, 它量的源文件, "它跟着哪个开关走"（只为把话说清楚）)
    exe_covers = [
        ("test_capi_common_func", set(), "恒有（CAPI 开着就有）"),
        ("test_capi_net_func", set(), "恒有（CAPI 开着就有）"),
        ("test_capi_webapp_func", {WEBAPP}, "UVCPP_BUILD_WEBAPP"),
        (H2_TARGET, {HTTP2}, "UVCPP_ENABLE_NGHTTP2"),
        (H3_TARGET, {QUIC, HTTP3}, "UVCPP_ENABLE_HTTP3（它蕴含 QUIC）"),
        (DB_TARGET, {DB}, "UVCPP_ENABLE_DB"),
    ]
    targets = []
    for name, covers, switch in exe_covers:
        exe = os.path.join(tree, "tests", "capi", name)
        if os.path.exists(exe):
            targets.append(name)
            continue
        hit = [l for l, _x, edits in selected
               if any(p in covers for p, _o, _n in edits)]
        if hit:
            print("这棵树里没有 %s：\n  %s\n"
                  "这趟选中的变异里有 %s 要靠它判，而这条用例跟着 %s 走"
                  "（判据在 tests/capi/CMakeLists.txt）。\n"
                  "配一棵编了它的树（本机那三棵与命令见 doc/capi-guide.md）。\n"
                  "★ 这一趟什么都没判 —— 退 3 是「没判」，不是「通过」。"
                  % (name, exe, "、".join(h.split()[0] for h in hit), switch))
            return 3
        print("这棵树没编 %s（%s 关着）：这趟不跑它，碰它的那几条变异不在"
              "判据里。" % (name, switch))
    if not targets:
        print("这棵树里一个 capi 用例都没有 —— 先按 doc/capi-guide.md 配一棵。")
        return 3

    files = sorted({e[0] for _l, _x, edits in selected for e in edits})
    before = {p: read_bytes(p) for p in files}
    sums = {p: md5(p) for p in before}

    summary = []
    verdict_ok = True
    restored_ok = False
    tail_ok = False

    try:
        cases = [("基线（未变异）", None, None)] + [
            (l, exp, edits) for l, exp, edits in selected]

        for label, expect, edits in cases:
            for p, b in before.items():
                write_bytes(p, b)
            if edits is not None:
                for path, old, new in edits:
                    s = read_bytes(path).decode("utf-8")
                    n = s.count(old)
                    assert n == 1, ("%s：锚点在 %s 里出现了 %d 次，必须恰好 1 次"
                                    "\n%r" % (label, path, n, old))
                    write_bytes(path, s.replace(old, new, 1).encode("utf-8"))

            rc, errs, out = build(tree, args.jobs, targets)
            if rc != 0 or errs:
                print("[%s] 构建失败 rc=%s errors=%s" % (label, rc, errs))
                print(out[-3000:])
                return 3

            rows = run_targets(tree, targets)
            red = [n for n, r, f, _fl, _c in rows if r != 0 or f != 0]
            caught = bool(red)
            if expect is None:
                verdict = "基线" + ("（绿）" if not caught else "（**居然红了**）")
                if caught:
                    verdict_ok = False
            else:
                mark = "✓" if caught == expect else "✗**与预期不符**"
                if caught != expect:
                    verdict_ok = False
                verdict = "%s %s（%s）" % (
                    "抓住" if caught else "没抓住", mark,
                    "、".join(x.replace("test_capi_", "").replace("_func", "")
                              for x in red) or ("%d 个都没红" % len(targets)))
            summary.append((label, verdict))
            print("\n[%s] %s" % (label, verdict), flush=True)
            for name, r, f, lines, cnt in rows:
                extra = (" checks/failures=%d/%d" % tuple(cnt)) if cnt else ""
                print("    %-26s rc=%-4s%s" % (name, r, extra))
                for s in lines[:6]:
                    print("        %s" % s)
                if len(lines) > 6:
                    print("        …（还有 %d 条 FAIL）" % (len(lines) - 6))

            if expect is False and not caught:
                crc, txt = ctest_whole_tree(tree)
                print("    [整棵树 ctest] rc=%s %s" % (crc, txt), flush=True)
                if crc != 0 or "0 tests failed" not in txt:
                    print("    **预期活下来的变异把整棵树弄红了 —— 那条判定站不住。**")
                    verdict_ok = False
    finally:
        for p, b in before.items():
            write_bytes(p, b)
        restored_ok = all(md5(p) == sums[p] for p in files)
        print("\n源码按字节还原: %s" % ("是" if restored_ok else "否 —— 有问题！"))
        rc, out = run(["grep", "-rn", "MUTATION", os.path.join(ROOT, "src")])
        print("grep 残留: %s" % (out.strip() or "(无)"))
        rc, errs, _ = build(tree, args.jobs, targets)
        rows = run_targets(tree, targets)
        tail_ok = (rc == 0 and errs == 0 and
                   all(r == 0 and f == 0 for _n, r, f, _l, _c in rows))
        print("还原后重建 rc=%s errors=%s；%d 个 exe 复跑 %s（应全 0）"
              % (rc, errs, len(targets), [(n.replace("test_capi_", ""), r, f)
                            for n, r, f, _l, _c in rows]))

    print("\n==== 汇总 ====")
    for label, verdict in summary:
        print("  %-38s %s" % (label, verdict))
    print("\n判据：每条变异的实际结果与预期一致、还原按字节、还原后全绿 → %s"
          % ("全过" if (verdict_ok and restored_ok and tail_ok) else "**有不对的**"))
    return 0 if (verdict_ok and restored_ok and tail_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
