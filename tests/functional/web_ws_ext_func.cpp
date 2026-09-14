/**
 * @file tests/functional/web_ws_ext_func.cpp
 * @brief permessage-deflate 协商（RFC 7692 §7.1）的策略表测试。
 *
 * 这里是**纯函数**测试：不建 socket、不起事件循环、不需要 zlib。
 * 协商是握手期的纯字符串决策，和帧格式无关，所以它应该能被单独钉死 ——
 * 把它和压缩的收发混在一起测，出问题时根本分不清是"参数谈错了"还是
 * "字节压错了"。
 *
 * 覆盖的三块：
 * 1. `Sec-WebSocket-Extensions` 的语法解析（含无值参数、空白、大小写）
 * 2. 服务端方向的两张策略表（`server_max_window_bits` 5 行、
 *    `client_max_window_bits` 7 行）+ context takeover echo 规则 + 拒绝对端
 * 3. 客户端方向的应答校验（比服务端严：应答里出现没提过的参数要**失败**，
 *    而不是降级 —— 降级会让两侧对帧格式的理解不一致）
 *
 * 最后一条 `handshake_matrix` 是本文件最有价值的用例：跑一组配置矩阵，
 * 把「客户端造请求 → 服务端定参 → 客户端校验应答」整条链走一遍，断言
 * **方向对齐** —— 服务端打算用来压的窗口位数，必须正好等于客户端打算用来解
 * 的窗口位数，反之亦然，context takeover 两个开关同样。这条不变式一旦破了，
 * 两侧各自自测都会绿，真连起来就解出乱码。
 */
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_ws_ext.h>
using namespace uvcpp;

// =========================================================================
// 小工具
// =========================================================================

/** @brief 服务端方向的便捷入口：从原始头部值定参并回出应答头部的值。 */
static std::string srv_accept(const char* offer_header,
                              const uvcpp_ws_deflate_config& cfg,
                              uvcpp_ws_deflate_params* out) {
  std::vector<uvcpp_ws_ext_offer> offers = ws_parse_extensions(offer_header);
  uvcpp_ws_deflate_params p = ws_deflate_accept_server(offers, cfg);
  if (out) *out = p;
  return ws_deflate_response_header(p);
}

/**
 * @brief 把应答头部的值解析回去查参数 —— 顺带把「格式化 → 解析」往返测了。
 *
 * 返回值**按值带出**，不返回指向解析结果的指针：解析出来的 `offer` 是局部
 * 变量，返回指向它内部 `params` 的指针就是返回悬垂指针（第一版就是这么写的，
 * 表现是几条用例随机失败，而直接打印实际值却一切正常）。
 *
 * @param[out] present   参数在不在（区分「没回」与「回了 0」）
 * @param[out] has_value 在的话，带没带值
 * @param[out] value     在且带值时的值
 * @return 应答本身是不是一条合法的 permessage-deflate（false = 应答格式就不对）
 */
static bool resp_lookup(const std::string& resp, const char* name, bool* present,
                        bool* has_value, std::string* value) {
  *present = false;
  if (has_value) *has_value = false;
  if (value) value->clear();

  std::vector<uvcpp_ws_ext_offer> o = ws_parse_extensions(resp);
  if (o.size() != 1) return false;
  if (o[0].name != "permessage-deflate") return false;

  const uvcpp_ws_ext_param* p = o[0].find(name);
  if (p == nullptr) return true;  // 应答合法，只是没有这个参数
  *present = true;
  if (has_value) *has_value = p->has_value;
  if (value) *value = p->value;
  return true;
}

/**
 * @brief 取应答里某个窗口位数参数。
 * @param[out] present 参数在不在且带值（无值形态与没回都算 false）
 * @return 位数；应答本身非法或参数没带值时无意义（调用方应先看 present）
 */
static int resp_bits(const std::string& resp, const char* name, bool* present) {
  bool hv = false;
  std::string v;
  if (!resp_lookup(resp, name, present, &hv, &v)) {
    *present = false;
    return -1;  // 应答格式就不对，让断言响亮地失败，而不是伪装成"没回"
  }
  if (!hv) {
    *present = false;
    return 0;
  }
  return std::atoi(v.c_str());
}

/** @brief 应答里有没有这个开关参数。 */
static bool resp_flag(const std::string& resp, const char* name) {
  bool present = false;
  if (!resp_lookup(resp, name, &present, nullptr, nullptr)) return false;
  return present;
}

// =========================================================================
// 1. 语法层
// =========================================================================

static bool test_parse_basic() {
  // 两个扩展、四种参数形态：无值 / 有值 / 带空白 / 大小写
  const std::string h =
      "permessage-deflate; client_max_window_bits; "
      "server_no_context_takeover, foo; bar=baz ; qux";
  std::vector<uvcpp_ws_ext_offer> o = ws_parse_extensions(h);
  if (o.size() != 2) return false;

  if (o[0].name != "permessage-deflate") return false;
  if (o[0].params.size() != 2) return false;
  if (o[0].params[0].name != "client_max_window_bits") return false;
  if (o[0].params[0].has_value) return false;  // 无值形态
  if (o[0].params[1].name != "server_no_context_takeover") return false;
  if (o[0].params[1].has_value) return false;

  if (o[1].name != "foo") return false;
  if (o[1].params.size() != 2) return false;
  if (o[1].params[0].name != "bar" || !o[1].params[0].has_value) return false;
  if (o[1].params[0].value != "baz") return false;
  if (o[1].params[1].name != "qux" || o[1].params[1].has_value) return false;

  // token 大小写不敏感（RFC 7230 §3.2.6）
  if (!o[0].has("CLIENT_MAX_WINDOW_BITS")) return false;
  if (!o[0].has("client_max_window_bits")) return false;
  if (o[0].has("nope")) return false;
  return true;
}

static bool test_parse_whitespace_and_edges() {
  // 前后空白、`=` 两侧空白都要吃掉
  std::vector<uvcpp_ws_ext_offer> o =
      ws_parse_extensions("  permessage-deflate ;  client_max_window_bits = 13  ");
  if (o.size() != 1) return false;
  if (o[0].name != "permessage-deflate") return false;
  if (o[0].params.size() != 1) return false;
  if (o[0].params[0].name != "client_max_window_bits") return false;
  if (!o[0].params[0].has_value) return false;
  if (o[0].params[0].value != "13") return false;

  // 空头部 → 空列表（不是一条空扩展）
  if (!ws_parse_extensions("").empty()) return false;
  if (!ws_parse_extensions("   ").empty()) return false;
  // 空段被跳过
  if (ws_parse_extensions("a,,b").size() != 2) return false;
  if (ws_parse_extensions(",,").size() != 0) return false;
  // 只有扩展名、没有参数
  o = ws_parse_extensions("permessage-deflate");
  if (o.size() != 1 || !o[0].params.empty()) return false;
  // 空参数段被跳过（`;` 之间的空白、末尾多出来的 `;`）
  o = ws_parse_extensions("foo; bar=baz;");
  if (o.size() != 1 || o[0].params.size() != 1) return false;
  if (o[0].params[0].name != "bar" || o[0].params[0].value != "baz") return false;
  o = ws_parse_extensions("foo; ;bar");
  if (o.size() != 1 || o[0].params.size() != 1) return false;
  if (o[0].params[0].name != "bar" || o[0].params[0].has_value) return false;
  // 有值但值为空：`bar=` 是"带了值、值为空"，不是"无值"。
  // （严格按语法 token 非空，这里只要求：不崩、且别把它当成无值形态 ——
  //  认成无值形态会让"对端把选择权交给我"和"对端给了一个空值"混为一谈。）
  o = ws_parse_extensions("foo; bar=");
  if (o.size() != 1 || o[0].params.size() != 1) return false;
  if (!o[0].params[0].has_value || !o[0].params[0].value.empty()) return false;
  return true;
}

// =========================================================================
// 2. 服务端方向：server_max_window_bits 表（5 行）
// =========================================================================

static bool test_server_max_window_bits_table() {
  uvcpp_ws_deflate_config cfg;
  bool present = false;

  // 行 1：本端无配置 + 对端无请求 → 不回
  std::string r = srv_accept("permessage-deflate", cfg, nullptr);
  resp_bits(r, "server_max_window_bits", &present);
  if (present) return false;

  // 行 2：本端无配置 + 对端请求 M → 必须尊重 M
  r = srv_accept("permessage-deflate; server_max_window_bits=9", cfg, nullptr);
  if (resp_bits(r, "server_max_window_bits", &present) != 9 || !present) return false;

  // 行 3：本端配置 N + 对端无请求 → N
  cfg.server_max_window_bits = 12;
  r = srv_accept("permessage-deflate", cfg, nullptr);
  if (resp_bits(r, "server_max_window_bits", &present) != 12 || !present) return false;

  // 行 4：N + M ≤ N → M
  r = srv_accept("permessage-deflate; server_max_window_bits=9", cfg, nullptr);
  if (resp_bits(r, "server_max_window_bits", &present) != 9 || !present) return false;

  // 行 4 边界：M == N → M（等价于 N）
  r = srv_accept("permessage-deflate; server_max_window_bits=12", cfg, nullptr);
  if (resp_bits(r, "server_max_window_bits", &present) != 12 || !present) return false;

  // 行 5：N + M > N → N
  r = srv_accept("permessage-deflate; server_max_window_bits=15", cfg, nullptr);
  if (resp_bits(r, "server_max_window_bits", &present) != 12 || !present) return false;

  // 回出来的数同时要落在 params 里（调用方拿它去配压缩器）
  uvcpp_ws_deflate_params p;
  srv_accept("permessage-deflate; server_max_window_bits=9", cfg, &p);
  if (!p.accepted || p.server_max_window_bits != 9) return false;
  return true;
}

// =========================================================================
// 3. 服务端方向：client_max_window_bits 表（7 行）
// =========================================================================

static bool test_server_client_window_bits_table() {
  uvcpp_ws_deflate_config cfg;
  bool present = false;

  // 行 1：无配置 + 无请求 → 不回
  std::string r = srv_accept("permessage-deflate", cfg, nullptr);
  resp_bits(r, "client_max_window_bits", &present);
  if (present) return false;

  // 行 2：无配置 + 有值 M → 回 M
  r = srv_accept("permessage-deflate; client_max_window_bits=9", cfg, nullptr);
  if (resp_bits(r, "client_max_window_bits", &present) != 9 || !present) return false;

  // 行 3：无配置 + **无值** → 不回。
  // 这是最容易写错的一行：应答里**不允许**出现无值形态（§7.1.2.2），
  // 所以"照抄对端"的实现会在这里输出 `client_max_window_bits`（无值），
  // 被严格的对端判为非法应答。
  r = srv_accept("permessage-deflate; client_max_window_bits", cfg, nullptr);
  resp_bits(r, "client_max_window_bits", &present);
  if (present) return false;

  // 行 4：配置 N + 无请求 → **不回**（对端没提这个参数，应答里就不能出现）
  cfg.client_max_window_bits = 10;
  r = srv_accept("permessage-deflate", cfg, nullptr);
  resp_bits(r, "client_max_window_bits", &present);
  if (present) return false;

  // 行 5：N + 无值 → N（对端把选择权交出来了，这时候才轮到本端定）
  r = srv_accept("permessage-deflate; client_max_window_bits", cfg, nullptr);
  if (resp_bits(r, "client_max_window_bits", &present) != 10 || !present) return false;

  // 行 6：N + 有值 M ≤ N → M
  r = srv_accept("permessage-deflate; client_max_window_bits=9", cfg, nullptr);
  if (resp_bits(r, "client_max_window_bits", &present) != 9 || !present) return false;

  // 行 7：N + 有值 M > N → N
  r = srv_accept("permessage-deflate; client_max_window_bits=13", cfg, nullptr);
  if (resp_bits(r, "client_max_window_bits", &present) != 10 || !present) return false;

  // 行 5 的数同样要落进 params（那是本端解压要用的窗口）
  uvcpp_ws_deflate_params p;
  srv_accept("permessage-deflate; client_max_window_bits", cfg, &p);
  if (!p.accepted || p.client_max_window_bits != 10) return false;
  return true;
}

// =========================================================================
// 4. 服务端方向：context takeover
// =========================================================================

static bool test_server_context_takeover() {
  // 谁都没提 → 都不回
  std::string r = srv_accept("permessage-deflate", uvcpp_ws_deflate_config(), nullptr);
  if (resp_flag(r, "client_no_context_takeover")) return false;
  if (resp_flag(r, "server_no_context_takeover")) return false;

  // 对端提了 → 照抄回去（对端提它，是"允许服务端要求客户端不用"的意思）
  r = srv_accept("permessage-deflate; client_no_context_takeover",
                 uvcpp_ws_deflate_config(), nullptr);
  if (!resp_flag(r, "client_no_context_takeover")) return false;

  // 对端提了 server_no_context_takeover → 照抄
  r = srv_accept("permessage-deflate; server_no_context_takeover",
                 uvcpp_ws_deflate_config(), nullptr);
  if (!resp_flag(r, "server_no_context_takeover")) return false;

  // 本端可以**单方面**承诺自己不用上下文（§7.1.1.2）—— 对端没提也能回
  uvcpp_ws_deflate_config cfg;
  cfg.server_no_context_takeover = true;
  r = srv_accept("permessage-deflate", cfg, nullptr);
  if (!resp_flag(r, "server_no_context_takeover")) return false;

  // 但 client_no_context_takeover **不能**单方面回（§7.1.1.1）：
  // 那是在要求对端放弃上下文，对端没提过就不能强加
  cfg = uvcpp_ws_deflate_config();
  cfg.client_no_context_takeover = true;
  r = srv_accept("permessage-deflate", cfg, nullptr);
  if (resp_flag(r, "client_no_context_takeover")) return false;
  // 而这一位的真实语义是"本端解压器可以假定对端不用上下文" —— 对端没提，
  // 它很可能在用，所以本端解压器绝不能按 no_context_takeover 配
  uvcpp_ws_deflate_params p;
  srv_accept("permessage-deflate", cfg, &p);
  if (p.client_no_context_takeover) return false;
  return true;
}

// =========================================================================
// 5. 服务端方向：拒绝
// =========================================================================

static bool test_server_decline() {
  const uvcpp_ws_deflate_config cfg;  // 全部默认（最宽松）
  uvcpp_ws_deflate_params p;

  struct { const char* header; const char* why; } bad[] = {
    // 越界 / 非十进制
    {"permessage-deflate; server_max_window_bits=7",  "server bits < 8"},
    {"permessage-deflate; server_max_window_bits=16", "server bits > 15"},
    {"permessage-deflate; server_max_window_bits=0",  "server bits == 0"},
    {"permessage-deflate; server_max_window_bits=08", "leading zero"},
    {"permessage-deflate; server_max_window_bits=8x", "trailing junk"},
    {"permessage-deflate; client_max_window_bits=99", "client bits > 15"},
    {"permessage-deflate; client_max_window_bits=09", "leading zero"},
    {"permessage-deflate; client_max_window_bits=",   "empty value"},
    {"permessage-deflate; client_max_window_bits=abc","not a number"},
    // server_max_window_bits 必须带值
    {"permessage-deflate; server_max_window_bits",    "valueless server bits"},
    // 开关参数不得带值
    {"permessage-deflate; client_no_context_takeover=1", "ctxt flag has value"},
    {"permessage-deflate; server_no_context_takeover=1", "ctxt flag has value"},
    // 重复参数
    {"permessage-deflate; client_max_window_bits=9; client_max_window_bits=10",
     "duplicate param"},
    // 未知参数：必须让整条扩展不可用，不能「忽略不认识的继续用」
    {"permessage-deflate; x-unknown-param=1", "unknown param"},
    {"permessage-deflate; client_max_window_bits_extra=9", "unknown param"},
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    std::string r = srv_accept(bad[i].header, cfg, &p);
    if (p.accepted) {
      std::cout << "    declined expected (" << bad[i].why << "): " << bad[i].header
                << "\n";
      return false;
    }
    // 拒绝时**一个字节都不能回** —— 回了半条参数对端会当成协商成功
    if (!r.empty()) {
      std::cout << "    non-empty response on decline: " << r << "\n";
      return false;
    }
  }

  // 别的扩展一律不碰
  if (!srv_accept("x-webkit-deflate-frame", cfg, &p).empty()) return false;
  if (p.accepted) return false;
  if (!srv_accept("", cfg, &p).empty()) return false;
  if (p.accepted) return false;

  // 总开关关掉 → 一律不接受（哪怕对端提得完全合规）
  uvcpp_ws_deflate_config off;
  off.enabled = false;
  std::string r = srv_accept("permessage-deflate; client_max_window_bits", off, &p);
  if (p.accepted || !r.empty()) return false;
  return true;
}

static bool test_server_picks_first_acceptable_offer() {
  const uvcpp_ws_deflate_config cfg;
  uvcpp_ws_deflate_params p;
  bool present = false;

  // 前两条都不可用（未知参数 / 取值越界），第三条才是好的。
  // RFC 允许客户端一次提多个备选，服务端应当继续往下找，而不是整条拒掉。
  std::string r = srv_accept(
      "permessage-deflate; x-bogus, "
      "permessage-deflate; server_max_window_bits=16, "
      "permessage-deflate; client_max_window_bits=12",
      cfg, &p);
  if (!p.accepted) return false;
  if (resp_bits(r, "client_max_window_bits", &present) != 12 || !present) return false;

  // 反过来：第一条可用就用第一条（不会去挑"更好"的）
  r = srv_accept(
      "permessage-deflate; client_max_window_bits=12, "
      "permessage-deflate; client_max_window_bits=9",
      cfg, &p);
  if (!p.accepted) return false;
  if (resp_bits(r, "client_max_window_bits", &present) != 12 || !present) return false;
  return true;
}

static bool test_server_real_client_offers() {
  uvcpp_ws_deflate_config cfg;
  bool present = false;

  // Chrome / Edge 的 offer：只带无值的 client_max_window_bits
  std::string r = srv_accept("permessage-deflate; client_max_window_bits", cfg, nullptr);
  if (r != "permessage-deflate") return false;  // 配置无约束 → 一个参数都不回

  // 本端想把对端压到 12 位才回得出东西
  cfg.client_max_window_bits = 12;
  r = srv_accept("permessage-deflate; client_max_window_bits", cfg, nullptr);
  if (resp_bits(r, "client_max_window_bits", &present) != 12 || !present) return false;

  // Firefox 的 offer：光秃秃一条
  cfg = uvcpp_ws_deflate_config();
  r = srv_accept("permessage-deflate", cfg, nullptr);
  if (r != "permessage-deflate") return false;

  // 对端主动限制本端压它的窗口
  r = srv_accept("permessage-deflate; server_max_window_bits=10", cfg, nullptr);
  if (resp_bits(r, "server_max_window_bits", &present) != 10 || !present) return false;
  return true;
}

// =========================================================================
// 6. 客户端方向
// =========================================================================

static bool test_client_request_header() {
  // 默认：无值形式的 client_max_window_bits（把窗口选择权交给服务端）
  std::string h = ws_deflate_request_header(uvcpp_ws_deflate_config());
  if (h != "permessage-deflate; client_max_window_bits") return false;
  std::vector<uvcpp_ws_ext_offer> o = ws_parse_extensions(h);
  if (o.size() != 1) return false;
  const uvcpp_ws_ext_param* p = o[0].find("client_max_window_bits");
  if (p == nullptr || p->has_value) return false;  // 必须是"无值"形态

  // 关掉 → 空串（调用方据此不加这个头）
  uvcpp_ws_deflate_config off;
  off.enabled = false;
  if (!ws_deflate_request_header(off).empty()) return false;

  // 全参数
  uvcpp_ws_deflate_config cfg;
  cfg.client_max_window_bits = 12;
  cfg.server_max_window_bits = 10;
  cfg.client_no_context_takeover = true;
  cfg.server_no_context_takeover = true;
  h = ws_deflate_request_header(cfg);
  o = ws_parse_extensions(h);
  if (o.size() != 1 || o[0].name != "permessage-deflate") return false;
  if (o[0].params.size() != 4) return false;
  p = o[0].find("client_max_window_bits");
  if (p == nullptr || !p->has_value || p->value != "12") return false;
  p = o[0].find("server_max_window_bits");
  if (p == nullptr || !p->has_value || p->value != "10") return false;
  if (!o[0].has("client_no_context_takeover")) return false;
  if (!o[0].has("server_no_context_takeover")) return false;
  return true;
}

static bool test_client_accept_basic() {
  const uvcpp_ws_deflate_config cfg;

  // 服务端回一条光秃秃的 → 两侧都用默认 15 位
  uvcpp_ws_deflate_params p = ws_deflate_accept_client(
      ws_parse_extensions("permessage-deflate"), cfg);
  if (!p.accepted || p.invalid) return false;
  if (p.client_max_window_bits != 15 || p.server_max_window_bits != 15) return false;
  if (p.client_no_context_takeover || p.server_no_context_takeover) return false;

  // 服务端限了本端的压缩窗口
  p = ws_deflate_accept_client(
      ws_parse_extensions("permessage-deflate; client_max_window_bits=9"), cfg);
  if (!p.accepted || p.invalid || p.client_max_window_bits != 9) return false;

  // 服务端声明自己用 10 位 + 不用上下文
  p = ws_deflate_accept_client(
      ws_parse_extensions("permessage-deflate; server_max_window_bits=10; "
                          "server_no_context_takeover"),
      cfg);
  if (!p.accepted || p.invalid) return false;
  if (p.server_max_window_bits != 10) return false;
  if (!p.server_no_context_takeover) return false;

  // 对端**没**应答 → 正常降级：不是 invalid
  p = ws_deflate_accept_client(ws_parse_extensions(""), cfg);
  if (p.accepted || p.invalid) return false;
  p = ws_deflate_accept_client(ws_parse_extensions("x-webkit-deflate-frame"), cfg);
  if (p.accepted || p.invalid) return false;

  // 总开关关掉 → 即使对端应答了也不启用（这是本端自己选的，不是非法应答）
  uvcpp_ws_deflate_config off;
  off.enabled = false;
  p = ws_deflate_accept_client(ws_parse_extensions("permessage-deflate"), off);
  if (p.accepted || p.invalid) return false;
  return true;
}

static bool test_client_rejects_bad_response() {
  uvcpp_ws_deflate_config cfg;

  // 这几种都是「对端应答了，但我不认」→ 必须 invalid，不能悄悄降级。
  // 因为服务端写了这个头就已经认定压缩生效，我方不启用等于两侧对帧格式的
  // 理解不一致，连接必然错乱 —— 正确动作是让握手失败。
  struct { const char* header; const char* why; } bad[] = {
    {"permessage-deflate; client_max_window_bits",
     "valueless param is offer-only"},
    {"permessage-deflate; client_max_window_bits=7", "bits < 8"},
    {"permessage-deflate; client_max_window_bits=16", "bits > 15"},
    {"permessage-deflate; server_max_window_bits", "valueless server bits"},
    {"permessage-deflate; x-bogus=1", "unknown param"},
    {"permessage-deflate; client_no_context_takeover",
     "not offered by us"},
    {"permessage-deflate, permessage-deflate", "duplicate extension"},
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    uvcpp_ws_deflate_params p =
        ws_deflate_accept_client(ws_parse_extensions(bad[i].header), cfg);
    if (p.accepted || !p.invalid) {
      std::cout << "    invalid expected (" << bad[i].why << "): " << bad[i].header
                << "\n";
      return false;
    }
  }

  // 服务端要的窗口比本端愿意给的大 → 拒
  uvcpp_ws_deflate_config narrow;
  narrow.server_max_window_bits = 9;
  uvcpp_ws_deflate_params p = ws_deflate_accept_client(
      ws_parse_extensions("permessage-deflate; server_max_window_bits=15"), narrow);
  if (p.accepted || !p.invalid) return false;
  // 正好等于 → 接受
  p = ws_deflate_accept_client(
      ws_parse_extensions("permessage-deflate; server_max_window_bits=9"), narrow);
  if (!p.accepted || p.invalid) return false;

  // **对照**：本端主动提了 client_no_context_takeover，服务端回它就是合法的。
  // 没有这条，一个"见到这个参数就拒"的实现也能让上面的断言通过。
  uvcpp_ws_deflate_config ctxt;
  ctxt.client_no_context_takeover = true;
  p = ws_deflate_accept_client(
      ws_parse_extensions("permessage-deflate; client_no_context_takeover"), ctxt);
  if (!p.accepted || p.invalid) return false;
  if (!p.client_no_context_takeover) return false;
  return true;
}

// =========================================================================
// 7. 端到端：客户端造请求 → 服务端定参 → 客户端校验应答
// =========================================================================

/**
 * @brief 配置矩阵下的方向对齐不变式。
 *
 * 断言的是**两侧对同一件事的说法一致**：
 * - 服务端 `server_max_window_bits`（本端压缩窗口）== 客户端同名值（解压窗口）
 * - 服务端 `client_max_window_bits`（对端压缩窗口＝本端解压窗口）
 *   == 客户端同名值（本端压缩窗口）
 * - 两个 context takeover 开关逐位相同
 *
 * 两侧各自的自测都绿、连起来解出乱码，就是这条不变式破了的样子。
 */
static bool test_handshake_matrix() {
  const int bits[] = {15, 9, 12};
  int checked = 0;

  for (size_t sb = 0; sb < 3; ++sb) {
    for (size_t cb = 0; cb < 3; ++cb) {
      for (int srv_nc = 0; srv_nc < 2; ++srv_nc) {
        for (int cli_nc = 0; cli_nc < 2; ++cli_nc) {
          // 客户端 offer 的三种形态。单靠"默认形态"是走不到服务端取 min 那一行
          // 的，所以这三种必须都跑到：
          //   0 = client_max_window_bits 无值（默认，把选择权交给服务端）
          //   1 = client_max_window_bits 带值（本端自己限定压缩窗口）
          //   2 = 上面基础再加 server_max_window_bits（限制服务端的压缩窗口）
          for (int style = 0; style < 3; ++style) {
            // 服务端配置：限定自己压缩用的窗口 / 要求对端压小窗口 / 两个开关
            uvcpp_ws_deflate_config scfg;
            scfg.server_max_window_bits = bits[sb];
            scfg.client_max_window_bits = bits[cb];
            scfg.server_no_context_takeover = (srv_nc != 0);
            scfg.client_no_context_takeover = (cli_nc != 0);

            // 客户端配置：两个开关必须与 offer 一致（只有提过才能回）
            uvcpp_ws_deflate_config ccfg;
            ccfg.client_no_context_takeover = (cli_nc != 0);
            ccfg.server_no_context_takeover = (srv_nc != 0);
            // 刻意用**错开**的一档，而不是和 scfg 取同一个数 —— 取同一个数的话
            // min(N, M) 的两个分支永远相等，矩阵就只是在重复"两边抄一遍"。
            // 错开之后才真的走到 min：有时是服务端配置更严，有时是客户端更严。
            if (style == 1) ccfg.client_max_window_bits = bits[(cb + 1) % 3];
            if (style == 2) ccfg.server_max_window_bits = bits[(sb + 1) % 3];

            const std::string req = ws_deflate_request_header(ccfg);
            uvcpp_ws_deflate_params sp =
                ws_deflate_accept_server(ws_parse_extensions(req), scfg);
            if (!sp.accepted) {
              std::cout << "    server declined a conformant offer: " << req << "\n";
              return false;
            }

            const std::string resp = ws_deflate_response_header(sp);
            uvcpp_ws_deflate_params cp =
                ws_deflate_accept_client(ws_parse_extensions(resp), ccfg);
            if (!cp.accepted || cp.invalid) {
              // 服务端只可能产出合规应答，客户端拒了就说明两边理解不一致
              std::cout << "    client rejected server response: " << resp
                        << " (offered: " << req << ")\n";
              return false;
            }

            if (sp.server_max_window_bits != cp.server_max_window_bits) {
              std::cout << "    server-bits mismatch: srv compresses with "
                        << sp.server_max_window_bits << " but cli decompresses with "
                        << cp.server_max_window_bits << " (" << resp << ")\n";
              return false;
            }
            if (sp.client_max_window_bits != cp.client_max_window_bits) {
              std::cout << "    client-bits mismatch: srv decompresses with "
                        << sp.client_max_window_bits << " but cli compresses with "
                        << cp.client_max_window_bits << " (" << resp << ")\n";
              return false;
            }
            if (sp.client_no_context_takeover != cp.client_no_context_takeover) {
              std::cout << "    client-ctxt mismatch (" << resp << ")\n";
              return false;
            }
            if (sp.server_no_context_takeover != cp.server_no_context_takeover) {
              std::cout << "    server-ctxt mismatch (" << resp << ")\n";
              return false;
            }
            // 窗口位数必须都在合法区间（喂给 zlib 之前就该是合法的）
            if (sp.server_max_window_bits < 8 || sp.server_max_window_bits > 15) return false;
            if (sp.client_max_window_bits < 8 || sp.client_max_window_bits > 15) return false;
            ++checked;
          }
        }
      }
    }
  }
  if (checked != 3 * 3 * 2 * 2 * 3) return false;
  return true;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"parse_basic", test_parse_basic},
    {"parse_whitespace_and_edges", test_parse_whitespace_and_edges},
    {"server_max_window_bits_table", test_server_max_window_bits_table},
    {"server_client_window_bits_table", test_server_client_window_bits_table},
    {"server_context_takeover", test_server_context_takeover},
    {"server_decline", test_server_decline},
    {"server_picks_first_acceptable_offer", test_server_picks_first_acceptable_offer},
    {"server_real_client_offers", test_server_real_client_offers},
    {"client_request_header", test_client_request_header},
    {"client_accept_basic", test_client_accept_basic},
    {"client_rejects_bad_response", test_client_rejects_bad_response},
    {"handshake_matrix", test_handshake_matrix},
  };
  for (const auto& t : tests) {
    std::cout << "[web_ws_ext] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_ws_ext] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}

#else
int main() {
  std::cout << "[web_ws_ext] SKIP (web disabled)\n";
  return 0;
}
#endif
