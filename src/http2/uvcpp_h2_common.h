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

#include <uvcpp/uvcpp_config.h>

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
/// @warning **零引用** —— 本库当前没有任何流控策略，没有代码读这个常量，也不会把它
///          发进 `SETTINGS`。真正的每流窗口来自 `uvcpp_h2_session::init()` 的
///          `initial_window_size` 参数（默认 `0`，表示**不发**这一项 `SETTINGS`，
///          于是对端按 RFC 9113 的缺省值 65535 走）。这里保留它只是给调用方一个
///          可以自己传进去的字面量，别以为改了它就会生效。
const uint32_t H2_DEFAULT_INITIAL_WINDOW_SIZE = 65535u;

/**
 * @brief 单个**待发**头部块的上限（未压缩估算值）。
 *
 * 与 `SETTINGS_MAX_HEADER_LIST_SIZE` 那条收方向预算同值但**不是同一件事**：
 * 这条是 nghttp2 自己的发送上限（`max_send_header_block_length`），超了它
 * nghttp2 会把整帧**静默丢掉**（见 `uvcpp_h2_session.cpp` 里
 * `header_block_fits()` 的注释）。所以本层在 `submit_*` 里按同一个数先拦一道，
 * 把"永远等不到"换成一个同步的 `UV_EMSGSIZE`。
 *
 * 显式写在 `init()` 里设进 nghttp2，而不是靠它的内部默认值恰好相等。
 */
const size_t H2_MAX_SEND_HEADER_BLOCK = 64u * 1024u;

// =========================================================================
// 错误码
// =========================================================================

/**
 * @brief h2 错误码（RFC 9113 §7）。
 *
 * 只命名**有具名消费者**的几个。其余的一律以 nghttp2 的原始负值透传
 * （`last_error()` / `on_fatal` 的 `nghttp2_error`），**不在这里造一张平行表** ——
 * 那种表会和上游一起漂移，而且漂移是静默的。
 *
 * 上面那条规矩的保险丝在 `uvcpp_h2_session.cpp`：这四个值各有一条
 * `static_assert` 对着 nghttp2 自己的枚举。加了常量却不加断言，就等于把
 * "平行表"从明处搬到了暗处。
 */
const uint32_t H2_ERR_NO_ERROR          = 0x00;
/// 对端说"这条流在被处理之前就关了" ⇒ 那条请求**重发是安全的**（§8.7）。
const uint32_t H2_ERR_REFUSED_STREAM    = 0x07;
/// 对端不要这条流了。**不保证**它没处理过 —— 所以不算可重试（§8.7）。
const uint32_t H2_ERR_CANCEL            = 0x08;
/// 被控制帧令牌桶拦下来时用它收尾：对端看得到"为什么"，而不是一个 NO_ERROR。
const uint32_t H2_ERR_ENHANCE_YOUR_CALM = 0x0b;

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
  OPEN,          ///< 请求头已收全，业务处理中（可能正在收 body）
  HEADERS_SENT,  ///< 流式响应：头部已发，body 还要靠 `submit_data` 一块块补
  SENT,          ///< 响应已一次提交完（HEADERS + 可能的 DATA 都交出去了）
  CLOSED,        ///< 本端与对端都已结束。**当前从不进入** —— 全仓没有一处给它赋值
  REJECTED,      ///< 我们在协议层拒了它（已发 RST_STREAM），业务层不该再看到。
                 ///< **当前从不进入** —— 全仓没有一处给它赋值
};

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE

#endif  // SRC_HTTP2_UVCPP_H2_COMMON_H
