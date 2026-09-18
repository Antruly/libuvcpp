/**
 * @file src/http2/uvcpp_h2_common.h
 * @brief HTTP/2 层的公开小件：调参默认值、头部预算、以及不依赖 nghttp2 的枚举。
 * @author zhuweiye
 * @version 1.1.0
 *
 * 本头**不包含** `<nghttp2/nghttp2.h>`（理由见 `uvcpp_h2_nghttp2.h`）。
 */

#ifndef SRC_HTTP2_UVCPP_H2_COMMON_H
#define SRC_HTTP2_UVCPP_H2_COMMON_H

#include <cstddef>
#include <cstdint>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_NGHTTP2_ENABLE

namespace uvcpp {

// =========================================================================
// 默认调参
// =========================================================================

/// `SETTINGS_MAX_CONCURRENT_STREAMS` 的默认值。取代 h1 的
/// `max_pipelined_requests` —— h2 的并发是协议内建的，不靠一个自定的上限。
const uint32_t H2_DEFAULT_MAX_CONCURRENT_STREAMS = 100;

/// 单个**响应体**在内存里最多攒多少字节。与 h1 的 body 上限同量级。
const size_t H2_DEFAULT_MAX_BODY_BYTES = 64u * 1024u * 1024u;

/// 初始流控窗口（每流）。
const uint32_t H2_DEFAULT_INITIAL_WINDOW_SIZE = 65535u;

// =========================================================================
// 头部列表预算
// =========================================================================

/**
 * @brief 单个头部块（HEADERS + 若干 CONTINUATION）解码后的字节预算。
 *
 * 每个字段按 `namelen + valuelen + 32` 计（RFC 9113 §6.5.2 的计法）。
 *
 * **这是我们唯一的防线，不是"第二道"。** 实测 nghttp2 v1.70.0：它把我们宣告的
 * `SETTINGS_MAX_HEADER_LIST_SIZE` 只存进 `local_settings`，接收路径
 * (`nghttp2_session.c` 的 `inflate_header_block`) 从头到尾**没有**做任何尺寸累加，
 * 更没有跟那个值比过 —— 连 `local_max_header_list_size` 这个名字在源码里都不存在。
 * 于是"我们宣告了 64 KiB，所以对端不会超"是一个**对端自愿遵守**的假设，而
 * HPACK bomb 恰恰是不遵守的那种对端。累加必须在这里自己做，且必须在上限处**断开**，
 * 而不是解完再看。
 */
class UVCPP_API h2_header_budget {
 public:
  h2_header_budget() = default;
  explicit h2_header_budget(size_t limit) : limit_(limit) {}

  /// 新起一个头部块时重置（CONTINUATION 属于同一个块，**不要**在中间重置）。
  void reset(size_t limit) {
    limit_      = limit;
    used_       = 0;
    overflowed_ = false;
  }

  /**
   * @brief 记一个字段。
   * @return 还在预算内返回 true；**超了返回 false，调用方应当立刻 RST_STREAM**。
   *
   * 超限之后再调也恒为 false —— 一旦越界，这个头部块就不该再被累积。
   */
  bool add(size_t name_len, size_t value_len) {
    if (overflowed_) return false;
    used_ += name_len + value_len + 32;
    if (used_ > limit_) {
      overflowed_ = true;
      return false;
    }
    return true;
  }

  /// 已经用掉的字节数。
  size_t used() const { return used_; }
  /// 是否已经越界。越界后 `used()` 不再增长（避免无界累加本身变成攻击面）。
  bool overflowed() const { return overflowed_; }

 private:
  size_t limit_;
  size_t used_      = 0;
  bool   overflowed_ = false;
};

// =========================================================================
// 流状态
// =========================================================================

/// 一条 h2 流在本层的生命周期。**不是** nghttp2 的内部状态 —— 那个我们读不到，
/// 也不该依赖。
enum class h2_stream_state {
  OPEN,       ///< 请求头已收全，业务处理中（可能正在收 body）
  SENT,       ///< 响应已提交（HEADERS 或 DATA 已交出）
  CLOSED,     ///< 本端与对端都已结束
  REJECTED,   ///< 我们在协议层拒了它（已发 RST_STREAM），业务层不该再看到
};

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE

#endif  // SRC_HTTP2_UVCPP_H2_COMMON_H
