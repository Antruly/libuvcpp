/**
 * @file src/web/uvcpp_ws_ext.h
 * @brief WebSocket 扩展协商（RFC 6455 §9.1、RFC 7692 §7）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `uvcpp_ws_parser` 分开：那个管**帧**，这里管**握手期的扩展协商**。
 * 分开的理由是这两件事的失败语义完全不同 —— 帧解析失败要发 1002 关连接，
 * 协商失败只是"这次不用这个扩展"，连接照常建立、退化成普通 WS。
 *
 * ## 为什么单独写一个模块（而不是塞进 parser）
 *
 * `permessage-deflate` 的参数是**按方向**命名的，而且**请求与应答允许出现的
 * 参数集合不一样**（§7.1.2）：
 *
 * | 参数 | 请求里 | 应答里 |
 * |---|---|---|
 * | `server_max_window_bits` | 必须带值 | 必须带值 |
 * | `client_max_window_bits` | 可以无值（把选择权交给服务端） | **必须带值** |
 * | `client_no_context_takeover` | 可以提 | **只有对端提过才能回** |
 * | `server_no_context_takeover` | 可以提 | 服务端可以单方面回 |
 *
 * 搞错方向的后果不是报错，而是解出乱码或对端直接断连，所以这些规则集中
 * 写在一处、并且逐条有测试钉住。
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_EXT_H
#define SRC_WEB_UVCPP_WS_EXT_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <cstddef>
#include <string>
#include <vector>
#include <uvcpp/uvcpp_define.h>

namespace uvcpp {

/**
 * @brief ASCII 大小写不敏感比较（HTTP token 的比较规则，RFC 7230 §3.2.6）。
 * @note 单独放在这里而不是复用 `http_name_equal`，是为了不让本模块依赖
 *       整个 http 公共头。
 */
namespace uvcpp_ws_ext_detail {

inline bool token_equal(const std::string& a, const char* b) {
  size_t n = 0;
  while (b[n] != '\0') ++n;
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

/** @brief 去掉首尾空白（`"a ; b"` 两边的空格）。 */
inline std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
  return s.substr(b, e - b);
}

}  // namespace uvcpp_ws_ext_detail

// =========================================================================
// 语法层：Sec-WebSocket-Extensions 的解析
// =========================================================================

/** @brief 一个扩展参数：`name` 或 `name=value`。 */
struct uvcpp_ws_ext_param {
  std::string name;
  /** @brief 区分 `client_max_window_bits` 与 `client_max_window_bits=13`。 */
  bool        has_value = false;
  std::string value;
};

/** @brief 一个扩展项：扩展名 + 它的参数列表。 */
struct uvcpp_ws_ext_offer {
  std::string                     name;
  std::vector<uvcpp_ws_ext_param> params;

  /** @brief 按名查找参数（token 不分大小写）；找不到返回 nullptr。 */
  const uvcpp_ws_ext_param* find(const char* n) const {
    for (size_t i = 0; i < params.size(); ++i) {
      if (uvcpp_ws_ext_detail::token_equal(params[i].name, n)) return &params[i];
    }
    return nullptr;
  }
  bool has(const char* n) const { return find(n) != nullptr; }
};

/**
 * @brief 解析 `Sec-WebSocket-Extensions` 头的值。
 *
 * 语法（RFC 6455 §9.1）：`extension-list = 1#extension`，
 * `extension = extension-token *( ";" extension-param )`。
 *
 * **容错而非报错**：语法不对的段直接跳过（返回的列表里没有它）。协商不上
 * 某个扩展是本协议的正常结局，不该因此让整个握手失败。
 */
UVCPP_API std::vector<uvcpp_ws_ext_offer> ws_parse_extensions(
    const std::string& header);

// =========================================================================
// 策略层：permessage-deflate
// =========================================================================

/**
 * @brief 本端对 permessage-deflate 的配置。
 *
 * **窗口位数用 15 表示"无约束"**，同时也是"不输出该参数"的判定值 ——
 * 15 本来就是协议默认值，显式声明 `server_max_window_bits=15` 与不声明等价。
 * 这与 `uvcpp_ws_parser::get_extension_header()` 用的是同一个约定。
 */
struct uvcpp_ws_deflate_config {
  /** @brief 总开关。false = 不提也不接受这个扩展。 */
  bool enabled = true;
  int  client_max_window_bits = 15;
  int  server_max_window_bits = 15;
  bool client_no_context_takeover = false;
  bool server_no_context_takeover = false;
};

/**
 * @brief 协商结果。字段名与 RFC 7692 一致，即**按方向**命名
 *        （`client_*` 指客户端那一侧），直接喂给
 *        `uvcpp_ws_connection::enable_compression()`。
 *
 * `accepted == false` 时其余字段无意义；调用方应当**完全不应答**这个扩展
 * （应答里不带 `Sec-WebSocket-Extensions`）—— 这是合法且最安全的降级。
 */
struct uvcpp_ws_deflate_params {
  bool accepted = false;
  /**
   * @brief **仅客户端方向有意义**：对端**应答了**这个扩展，但应答本身不合规。
   *
   * 这个区分不是洁癖，是必须的：`accepted == false` 有两种截然不同的成因 ——
   * 「对端压根没应答」和「对端应答了但我用不了」。前者是正常降级，退化成
   * 普通 WS 即可；**后者不能降级**，因为服务端一旦在 101 里写了
   * `permessage-deflate` 就已经认定压缩生效了，我方单方面不启用等于双方对
   * 帧格式的理解不一致 —— 连接必然错乱（§7.1.2 要求此时**让握手失败**）。
   */
  bool invalid = false;
  int  client_max_window_bits = 15;
  int  server_max_window_bits = 15;
  bool client_no_context_takeover = false;
  bool server_no_context_takeover = false;
};

/**
 * @brief 服务端：从对端 offer 里挑一个可接受的 permessage-deflate 并定参。
 *
 * 逐个 offer 尝试，取**第一个**可接受的（RFC 允许客户端一次提多个备选）。
 * 任何一个参数无法满足（取值越界、未知参数、重复参数、`server_max_window_bits`
 * 没带值……）就跳过这个 offer；全都不行则返回 `accepted == false`。
 */
UVCPP_API uvcpp_ws_deflate_params ws_deflate_accept_server(
    const std::vector<uvcpp_ws_ext_offer>& offers,
    const uvcpp_ws_deflate_config& cfg);

/** @brief 服务端：把协商结果格式化成应答头部的值（未接受则返回空串）。 */
UVCPP_API std::string ws_deflate_response_header(
    const uvcpp_ws_deflate_params& p);

/**
 * @brief 客户端：生成本端请求头部的值。
 *
 * 默认带上**无值**形式的 `client_max_window_bits`（把窗口选择权交给服务端），
 * 这与 Chrome 等真实客户端一致。
 */
UVCPP_API std::string ws_deflate_request_header(
    const uvcpp_ws_deflate_config& cfg);

/**
 * @brief 客户端：校验服务端的应答并取出参数。
 *
 * 校验比服务端方向更严 —— 应答里出现本端没提过的参数、`client_max_window_bits`
 * 无值、窗口位数超出本端愿意接受的范围、出现多个 permessage-deflate，任何一个
 * 都判为 `accepted == false`（此时不应启用压缩）。
 */
UVCPP_API uvcpp_ws_deflate_params ws_deflate_accept_client(
    const std::vector<uvcpp_ws_ext_offer>& offers,
    const uvcpp_ws_deflate_config& cfg);

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_EXT_H
