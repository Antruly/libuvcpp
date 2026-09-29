/**
 * @file src/http3/uvcpp_h3_common.h
 * @brief HTTP/3 层的公开小件：调参默认值、头部预算、不依赖 nghttp3 的枚举与常量。
 * @author zhuweiye
 * @version 1.4.1
 *
 * 本头**不包含** `<nghttp3/nghttp3.h>`（理由见 `uvcpp_h3_nghttp3.h`）：这里一个
 * nghttp3 的类型、枚举、常量都没有 —— 使用者不需要装 nghttp3，也不需要知道
 * QPACK 的 token 编号。
 */

#ifndef SRC_HTTP3_UVCPP_H3_COMMON_H
#define SRC_HTTP3_UVCPP_H3_COMMON_H

#include <uvcpp/uvcpp_config.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_HTTP3_ENABLE

namespace uvcpp {

// =========================================================================
// 默认调参
// =========================================================================

/**
 * @brief 我们**愿意接收**的单个头块（field section）上限，字节。
 *
 * 计法是 RFC 9114 §4.2.2 的：每个字段按 `namelen + valuelen + 32` 算。
 *
 * **这是我们唯一的防线，不是"第二道"。** nghttp3 自己的文档就写着
 * "nghttp3 library does not enforce this limit.  Applications are responsible
 * for imposing their own limits to protect against resource exhaustion."
 * 实测也对得上：`max_field_section_size` 在 `nghttp3_conn.c` 里只被存进
 * `remote.settings`（`nghttp3_session.c` 的接收路径从头到尾没有对它做过一次
 * 累加比较）。它唯一的尺寸保护是 `NGHTTP3_QPACK_MAX_NAMELEN` 那一重 ——
 * 管的是**单个名字/值**，管不了"很多个小字段"这种 QPACK bomb。
 * 所以累加必须在这里自己算，且必须在上限处**断开**，而不是解完再看。
 * 累加器是 `h3_header_budget`。
 */
const uint64_t H3_DEFAULT_MAX_FIELD_SECTION_SIZE = 64u * 1024u;

/**
 * @brief 我们**自己愿意发出**的单个头块上限，字节。计法与收方向那条相同。
 *
 * **这一条拦不住对端。** 它拦的是"调用方要求我们发一个荒谬大的头块" ——
 * nghttp3 在发方向一个尺寸检查都没有（它把 `nghttp3_nv` 直接交给 QPACK 编码器），
 * 而**对端宣告的上限我们读不到**：nghttp3 没有暴露远端 SETTINGS 的取值
 * （`nghttp3_conn_get_*` 一族里没有它，实测 v1.18.0）。所以这里用的是**我们自己
 * 宣告出去的那个数**：一个头块既然我们不肯收，也就不该往外发。
 *
 * 拦住的后果是 `submit_request()` / `submit_response()` 返回 `UV_EMSGSIZE`，
 * 调用方当场就知道 —— 比"发出去、对端拿 `H3_EXCESSIVE_LOAD` 把这条流掐掉、
 * 我们这边只看到一条流莫名关掉"要好定位得多。
 */
const size_t H3_MAX_SEND_HEADER_BLOCK = 64u * 1024u;

/// QPACK 动态表容量上限（我们宣告出去的 `SETTINGS_QPACK_MAX_TABLE_CAPACITY`）。
const size_t H3_DEFAULT_QPACK_MAX_DTABLE_CAPACITY = 4096;

/**
 * @brief 我们容许"因为等动态表而卡住"的流数上限。
 *
 * 对端可以故意让很多条流都引用一个还没到的动态表条目，把内存钉住 —— 这个数就是
 * 那件事的上界（`SETTINGS_QPACK_BLOCKED_STREAMS`）。
 */
const size_t H3_DEFAULT_QPACK_BLOCKED_STREAMS = 100;

/**
 * @brief 单个**请求体/响应体**在内存里最多攒多少字节。
 *
 * 与 h1/h2 的同名上限同量级。**规矩**：攒到这个数就不再攒，当场把这条流按
 * 失败收场（而不是让对端把内存吃干）—— 一个"攒不完就默默继续攒"的上限不是上限。
 */
const size_t H3_DEFAULT_MAX_BODY_BYTES = 64u * 1024u * 1024u;

/**
 * @brief 一次 `drain()` 最多能吐出多少段待发缓冲。
 *
 * 那是 nghttp3 的一次 `nghttp3_conn_writev_stream()` 调用能填的
 * `nghttp3_vec` 段数，取 16 与 ngtcp2 自己那份 HTTP/3 样例（`std::array<nghttp3_vec, 16>`）
 * 同一个数。**这不是"一次能发多少字节"的上限** —— 吐不完的下一轮接着吐。
 */
const size_t H3_MAX_WRITE_VECS = 16;

// =========================================================================
// 错误码（RFC 9114 §8.1 的应用错误码）
// =========================================================================

/**
 * @brief h3 应用错误码。
 *
 * **只命名有具名消费者的几个。** 其余的一律以裸数字透传（`h3_stream_close_info`
 * 里的 rx/tx 码，以及 `uvcpp_h3_session::quic_app_error_code()` 给 QUIC 收尾用的
 * 那个）——**不在这里造一张平行表**：那种表会跟上游一起漂移，而且漂移是静默的。
 *
 * 上面那条规矩的保险丝在 `uvcpp_h3_session.cpp`：下面三个值各有一条
 * `static_assert` 对着 nghttp3 自己的宏（`NGHTTP3_H3_NO_ERROR` /
 * `NGHTTP3_H3_INTERNAL_ERROR` / `NGHTTP3_H3_EXCESSIVE_LOAD`）。加了常量却不加
 * 断言，就等于把"平行表"从明处搬到了暗处。
 *
 * 取值是 HTTP/3 的规矩，不是随便定的：`0x0100` 起、每个 +1，落在 QUIC 自己的
 * 保留区间之外（`0x00`–`0x1f` 是 QUIC 传输层错误码）。
 */
const uint64_t H3_NO_ERROR = 0x0100;
/**
 * @brief 本端自己出了错（关键单向流开不出来、绑不上之类）。
 *
 * 这一条的消费者是 `uvcpp_h3_connection` 里那两处"本层继续不下去"的收场：
 * 它们必须给对端一个**真话**的错误码，而 `H3_NO_ERROR`（"正常关闭"）是句假话。
 */
const uint64_t H3_INTERNAL_ERROR = 0x0102;
/// 一个头块（或一个请求/响应）超出了我们愿意付出的资源 —— 本层用它收尾"头部
/// 预算超限"那条路（见 `H3_DEFAULT_MAX_FIELD_SECTION_SIZE`）。
const uint64_t H3_EXCESSIVE_LOAD = 0x0107;

// =========================================================================
// 一个头字段
// =========================================================================

/**
 * @brief 头字段的种类。
 *
 * 伪头（`:method` 这类）用**具名枚举**表达，理由有两条：
 *
 * 1. 调用方不该写 `h.name == ":method"` 这种字面量比较 —— 拼错了只会静默走进
 *    "没认出来"那一支，编译期一声不响。
 * 2. **也不该把 nghttp3 的 `nghttp3_qpack_token` 数值漏出去**：那是一张会随
 *    上游漂移的表。映射写在 `uvcpp_h3_session.cpp` 里，用的是 nghttp3 的**具名
 *    常量**（`NGHTTP3_QPACK_TOKEN__METHOD` 那四个），所以上游改数值也不影响我们。
 */
enum class h3_header_kind {
  REGULAR,    ///< 普通头字段。
  METHOD,     ///< `:method`
  SCHEME,     ///< `:scheme`
  PATH,       ///< `:path`
  STATUS,     ///< `:status`
  AUTHORITY,  ///< `:authority`
};

/**
 * @brief 一个头字段：名字、值、以及（伪头才有意义的）种类。
 *
 * `name` 里**带冒号**（伪头就是 `":method"` 这个字符串），不拆成"伪头+名字"两栏：
 * h3 的伪头必须出现在普通头**之前**，而那个顺序只有"一条列表"能表达。
 */
struct UVCPP_API h3_header {
  std::string    name;
  std::string    value;
  h3_header_kind kind = h3_header_kind::REGULAR;
};

// =========================================================================
// 一条流的收场
// =========================================================================

/**
 * @brief 一条流关闭时，两个方向各自是怎么收场的。
 *
 * `rx_error` / `tx_error` 为假表示那一侧是**干净**结束的（对端发了 FIN、或者
 * 本端把要发的都发完了）；为真表示那一侧是被 `RESET_STREAM` / `STOP_SENDING`
 * 掐断的，`*_app_error_code` 才是有意义的原因码。
 *
 * 两个方向分开报是**必须的**：一条请求的收场在 h3 里是两件独立的事（对端还发
 * 不发 vs 我还发不发），折成一个"流关了"，调用方就分不出"请求体收全了"和
 * "对端把请求取消了"。
 */
struct UVCPP_API h3_stream_close_info {
  int64_t  stream_id = -1;
  bool     rx_error  = false;
  bool     tx_error  = false;
  uint64_t rx_app_error_code = 0;
  uint64_t tx_app_error_code = 0;
};

// =========================================================================
// 头部列表预算
// =========================================================================

/**
 * @brief 单个头块解码后的字节预算累加器。
 *
 * 规矩与 `h2_header_budget` 逐字相同（`namelen + valuelen + 32`，越界后不再
 * 累加、也不再放行）—— 它是本层的**唯一**防线，理由见
 * `H3_DEFAULT_MAX_FIELD_SECTION_SIZE` 那段。
 *
 * 用法：`on_begin_headers` 时 `reset(limit)`；每个字段 `add()`；返回 false 就是
 * 越界，调用方**必须当场断开**这条流，而不是"解完再看"。
 */
class UVCPP_API h3_header_budget {
 public:
  h3_header_budget() = default;
  explicit h3_header_budget(size_t limit) : limit_(limit) {}

  /// 新起一个头块时重置。**trailers 是另一个头块**，也要重置（本批不处理
  /// trailers，见 doc 里"没做的"那张表）。
  void reset(size_t limit) {
    limit_      = limit;
    used_       = 0;
    overflowed_ = false;
  }

  /**
   * @brief 记一个字段。
   * @return 还在预算内返回 true；**超了返回 false，调用方应当当场这条流断开**。
   *
   * 超限之后再调也恒为 false —— 一旦越界，这个头块就不该再被累积。
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

  /// 已经用掉的字节数。越界后不再增长。
  size_t used() const { return used_; }
  /// 是否已经越界。
  bool overflowed() const { return overflowed_; }

 private:
  size_t limit_      = H3_DEFAULT_MAX_FIELD_SECTION_SIZE;
  size_t used_       = 0;
  bool   overflowed_ = false;
};

// =========================================================================
// 流状态
// =========================================================================

/// 一条 h3 流在本层的生命周期。**不是** nghttp3 的内部状态 —— 那个我们读不到，
/// 也不该依赖。
enum class h3_stream_state {
  OPEN,          ///< 头块正在收（请求头/响应头还没收全）
  HEADERS_DONE,  ///< 头块收全了，body 可能还在来
  RX_ENDED,      ///< 读侧收场（对端发了 FIN，或者被 RESET 掐断）
  CLOSED,        ///< 流已经关掉了（本端关的，或者两端都收场了）
};

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE

#endif  // SRC_HTTP3_UVCPP_H3_COMMON_H
