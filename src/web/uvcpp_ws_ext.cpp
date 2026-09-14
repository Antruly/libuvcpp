#include <web/uvcpp_ws_ext.h>
#if UVCPP_WEB_ENABLE
#include <cstdlib>

namespace uvcpp {

using uvcpp_ws_ext_detail::token_equal;
using uvcpp_ws_ext_detail::trim;

namespace {

/**
 * @brief 解析 8..15 的十进制窗口位数。
 *
 * 只认纯数字：`"9"` 可以，`"+9"`/`"09"`/`"9 "`/`"0x9"` 一律不行（§7.1.2.1
 * 明确要求"decimal integer value without leading zeroes between 8 to 15"）。
 */
bool parse_window_bits(const std::string& v, int& out) {
  if (v.empty() || v.size() > 2) return false;
  // §7.1.2.1 明确"without leading zeroes"：`08` 不是合法的 8。
  // 不挡这一条 `atoi` 会把 `08` 当成 8 收下 —— 而这正是"宽松解析"在协议
  // 协商里最典型的坏处：本端认为自己懂了，对端认为本端读错了。
  if (v.size() > 1 && v[0] == '0') return false;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] < '0' || v[i] > '9') return false;
  }
  int n = std::atoi(v.c_str());
  if (n < 8 || n > 15) return false;
  out = n;
  return true;
}

/** @brief 从一条 permessage-deflate 里取出来的参数。 */
struct deflate_args {
  bool has_client_bits       = false;
  bool client_bits_valueless = false;
  int  client_bits           = 15;
  bool has_server_bits       = false;
  int  server_bits           = 15;
  bool client_no_ctxt        = false;
  bool server_no_ctxt        = false;
};

/**
 * @brief 校验并取出参数。false = 这条扩展整体不可用，调用方可以试下一条。
 *
 * 判失败的四种情形：
 * - **未知参数**。§7.1.2.1 要求本端不认识的参数让整个 offer 不可用 ——
 *   不能"忽略不认识的那条继续用"，因为那条参数很可能正是对端的前提。
 * - **同一个参数出现两次**。
 * - **取值不是 8..15 的纯十进制**。
 * - **`server_max_window_bits` 不带值**（请求与应答里都必须带值）。
 */
bool parse_deflate_args(const uvcpp_ws_ext_offer& o, deflate_args& a) {
  for (size_t k = 0; k < o.params.size(); ++k) {
    const uvcpp_ws_ext_param& pr = o.params[k];
    for (size_t j = 0; j < k; ++j) {
      if (token_equal(o.params[j].name, pr.name.c_str())) return false;
    }
    if (token_equal(pr.name, "client_max_window_bits")) {
      a.has_client_bits = true;
      if (!pr.has_value) {
        a.client_bits_valueless = true;
      } else if (!parse_window_bits(pr.value, a.client_bits)) {
        return false;
      }
    } else if (token_equal(pr.name, "server_max_window_bits")) {
      a.has_server_bits = true;
      if (!pr.has_value) return false;
      if (!parse_window_bits(pr.value, a.server_bits)) return false;
    } else if (token_equal(pr.name, "client_no_context_takeover")) {
      if (pr.has_value) return false;
      a.client_no_ctxt = true;
    } else if (token_equal(pr.name, "server_no_context_takeover")) {
      if (pr.has_value) return false;
      a.server_no_ctxt = true;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 语法层
// ---------------------------------------------------------------------------

std::vector<uvcpp_ws_ext_offer> ws_parse_extensions(const std::string& header) {
  std::vector<uvcpp_ws_ext_offer> out;
  size_t pos = 0;
  for (;;) {
    const size_t comma = header.find(',', pos);
    const std::string seg = trim(header.substr(
        pos, (comma == std::string::npos ? header.size() : comma) - pos));

    uvcpp_ws_ext_offer offer;
    size_t sp = 0;
    bool name_done = false;
    for (;;) {
      const size_t semi = seg.find(';', sp);
      const std::string tok = trim(seg.substr(
          sp, (semi == std::string::npos ? seg.size() : semi) - sp));
      if (!name_done) {
        offer.name = tok;
        name_done = true;
      } else if (!tok.empty()) {
        uvcpp_ws_ext_param p;
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) {
          p.name = trim(tok);
          p.has_value = false;
        } else {
          p.name = trim(tok.substr(0, eq));
          p.has_value = true;
          p.value = trim(tok.substr(eq + 1));
        }
        if (!p.name.empty()) offer.params.push_back(p);
      }
      if (semi == std::string::npos) break;
      sp = semi + 1;
    }
    if (!offer.name.empty()) out.push_back(offer);

    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------
// 服务端方向
// ---------------------------------------------------------------------------

uvcpp_ws_deflate_params ws_deflate_accept_server(
    const std::vector<uvcpp_ws_ext_offer>& offers,
    const uvcpp_ws_deflate_config& cfg) {
  uvcpp_ws_deflate_params declined;
  if (!cfg.enabled) return declined;

  for (size_t i = 0; i < offers.size(); ++i) {
    const uvcpp_ws_ext_offer& o = offers[i];
    if (!token_equal(o.name, "permessage-deflate")) continue;

    deflate_args a;
    if (!parse_deflate_args(o, a)) continue;  // 这条不行，试下一条

    uvcpp_ws_deflate_params p;
    p.accepted = true;

    // --- server_max_window_bits：约束**本端压缩器** ---
    // 本端要用多大的窗口压。对端提了约束就必须尊重（取小者）；没提就用
    // 本端配置（15 = 无约束 = 不输出这个参数）。
    if (a.has_server_bits) {
      p.server_max_window_bits = (cfg.server_max_window_bits < a.server_bits)
                                     ? cfg.server_max_window_bits
                                     : a.server_bits;
    } else {
      p.server_max_window_bits = cfg.server_max_window_bits;
    }

    // --- client_max_window_bits：约束**对端压缩器**（＝本端解压窗口）---
    // §7.1.2.2：**只有对端在 offer 里提过，应答里才可以回这个参数**。
    if (a.has_client_bits) {
      if (a.client_bits_valueless) {
        // 无值形式只允许出现在请求里。应答要么给一个具体值（本端配置），
        // 要么整个不回 —— 而"不回"在这里就等于放弃这个机会，所以取配置。
        p.client_max_window_bits = cfg.client_max_window_bits;
      } else {
        p.client_max_window_bits = (cfg.client_max_window_bits < a.client_bits)
                                       ? cfg.client_max_window_bits
                                       : a.client_bits;
      }
    } else {
      p.client_max_window_bits = 15;  // 不输出
    }

    // --- context takeover ---
    // §7.1.1.1：client_no_context_takeover 只有对端提过才能回（对端提它，
    // 是"我方可以要求客户端不用"的意思，照抄即可）。
    // §7.1.1.2：server_no_context_takeover 本端可以**单方面**回 —— 那只是
    // 承诺自己不用上下文，不增加对端的负担。
    p.client_no_context_takeover = a.client_no_ctxt;
    p.server_no_context_takeover =
        a.server_no_ctxt || cfg.server_no_context_takeover;

    return p;  // 取第一个能接受的
  }
  return declined;
}

std::string ws_deflate_response_header(const uvcpp_ws_deflate_params& p) {
  if (!p.accepted) return std::string();
  std::string h = "permessage-deflate";
  if (p.server_no_context_takeover) h += "; server_no_context_takeover";
  if (p.client_no_context_takeover) h += "; client_no_context_takeover";
  if (p.server_max_window_bits != 15) {
    h += "; server_max_window_bits=" + std::to_string(p.server_max_window_bits);
  }
  if (p.client_max_window_bits != 15) {
    h += "; client_max_window_bits=" + std::to_string(p.client_max_window_bits);
  }
  return h;
}

// ---------------------------------------------------------------------------
// 客户端方向
// ---------------------------------------------------------------------------

std::string ws_deflate_request_header(const uvcpp_ws_deflate_config& cfg) {
  if (!cfg.enabled) return std::string();
  std::string h = "permessage-deflate";
  if (cfg.server_no_context_takeover) h += "; server_no_context_takeover";
  if (cfg.client_no_context_takeover) h += "; client_no_context_takeover";
  if (cfg.server_max_window_bits != 15) {
    h += "; server_max_window_bits=" + std::to_string(cfg.server_max_window_bits);
  }
  // 默认发**无值**形式，把窗口大小的选择权交给服务端（Chrome 等真实客户端
  // 就是这么发的）。要自己定死就配一个 8..15 的值。
  if (cfg.client_max_window_bits != 15) {
    h += "; client_max_window_bits=" + std::to_string(cfg.client_max_window_bits);
  } else {
    h += "; client_max_window_bits";
  }
  return h;
}

uvcpp_ws_deflate_params ws_deflate_accept_client(
    const std::vector<uvcpp_ws_ext_offer>& offers,
    const uvcpp_ws_deflate_config& cfg) {
  uvcpp_ws_deflate_params declined;
  if (!cfg.enabled) return declined;

  const uvcpp_ws_ext_offer* found = nullptr;
  for (size_t i = 0; i < offers.size(); ++i) {
    if (!token_equal(offers[i].name, "permessage-deflate")) continue;
    // 应答里出现**多个** permessage-deflate 本身就是非法的（§7.1.2）。
    if (found != nullptr) {
      declined.invalid = true;
      return declined;
    }
    found = &offers[i];
  }
  if (found == nullptr) return declined;  // 对端没应答 —— 正常降级

  // 从这里往下都是"对端应答了，但我不认"，一律 invalid：不能悄悄降级。
  deflate_args a;
  if (!parse_deflate_args(*found, a)) {
    declined.invalid = true;
    return declined;
  }
  // 无值形式只允许出现在请求里（§7.1.2.2）。
  if (a.has_client_bits && a.client_bits_valueless) {
    declined.invalid = true;
    return declined;
  }
  // 服务端不得回本端没提过的 client_no_context_takeover（§7.1.1.1）。
  if (a.client_no_ctxt && !cfg.client_no_context_takeover) {
    declined.invalid = true;
    return declined;
  }
  // 服务端给自己要的窗口不能大过本端愿意接受的（§7.1.2.1）。
  if (a.has_server_bits && cfg.server_max_window_bits < a.server_bits) {
    declined.invalid = true;
    return declined;
  }

  uvcpp_ws_deflate_params p;
  p.accepted = true;
  // 应答没提就用协议默认值 15（＝无约束），两侧必须用同一套数。
  p.client_max_window_bits = a.has_client_bits ? a.client_bits : 15;
  p.server_max_window_bits = a.has_server_bits ? a.server_bits : 15;
  p.client_no_context_takeover = a.client_no_ctxt;
  p.server_no_context_takeover =
      a.server_no_ctxt || cfg.server_no_context_takeover;
  return p;
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
