/**
 * @file src/web/uvcpp_ws_parser.h
 * @brief WebSocket frame parser — streaming, self-implemented per RFC 6455.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Fully self-implemented C++ frame parser (does NOT use the old
 * websocket_parser.h/c).  Provides:
 * - Streaming state-machine parsing (data may arrive in chunks)
 * - Frame building (for sending direction)
 * - Mask application (XOR)
 * - Compression integration via UVCPP_ZLIB_ENABLE macro (RFC 7692)
 * - Architecture reserves rsv2/rsv3 for HTTP/2 (RFC 8441)
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_PARSER_H
#define SRC_WEB_UVCPP_WS_PARSER_H

#if UVCPP_WEB_ENABLE

#include <functional>
#include <cstdint>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_ws_frame.h>

#if UVCPP_ZLIB_ENABLE
#include <zlib.h>
#endif

namespace uvcpp {

// =========================================================================
// Parser state
// =========================================================================

enum class ws_parser_state : uint8_t {
  IDLE          = 0,  // Waiting for frame start
  OPCODE        = 1,  // Parsing byte 0 (FIN+RSV+OPCODE)
  PAYLOAD_LEN   = 2,  // Parsing byte 1 (MASK+length)
  EXTENDED_2    = 3,  // Extended length 2 bytes (126)
  EXTENDED_8    = 4,  // Extended length 8 bytes (127)
  MASK_KEY      = 5,  // Mask key 4 bytes
  PAYLOAD       = 6,  // Payload data
  COMPLETE      = 7,  // Frame fully parsed
  PARSE_ERROR   = 8,  // Parse error
};

// =========================================================================
// WebSocket frame parser
// =========================================================================

class UVCPP_API uvcpp_ws_parser {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ws_parser)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ws_parser)

  // -------------------------------------------------------------------
  // Streaming parse (receive direction)
  // -------------------------------------------------------------------

  /**
   * @brief Feed data into the parser.  May be called multiple times
   *        as data arrives from the network.
   * @return Number of bytes consumed from @p data.
   */
  size_t execute(const char* data, size_t len);

  /** @brief Reset parser for the next frame. */
  void reset();

  // -------------------------------------------------------------------
  // Callbacks
  // -------------------------------------------------------------------

  /** @brief Called when a complete frame has been parsed. */
  void set_on_frame(std::function<void(const uvcpp_ws_frame&)> cb);

  /** @brief Called on parse error (error_code, reason). */
  void set_on_error(std::function<void(int, const char*)> cb);

  // -------------------------------------------------------------------
  // Frame size limit (security)
  // -------------------------------------------------------------------

  /**
   * @brief 单个帧负载的最大字节数（0 = 不限，默认）。
   *
   * 检查点在**长度字段解析完成的那一刻**，不是等数据到达：对端可以先声明一个
   * 8 EB 的长度，再按自己的节奏把内存喂满 —— 等到数据到了再拦，内存已经被
   * 吃掉了。超限走 `set_error`（code = -6），解析器进入 PARSE_ERROR。
   *
   * @warning 这是**单帧**上限。分片是绕过单帧上限的标准手法（每一帧都合规，
   *          合起来无上限），所以调用方**还必须**限制重组之后的消息总长
   *          （见 `uvcpp_ws_connection::set_max_message_size`）。两级都要卡。
   */
  void set_max_frame_size(uint64_t n);
  uint64_t get_max_frame_size() const;

  // -------------------------------------------------------------------
  // State queries
  // -------------------------------------------------------------------

  ws_parser_state get_state() const;
  bool is_idle() const;
  const uvcpp_ws_frame& get_current_frame() const;
  int get_last_error() const;

  // -------------------------------------------------------------------
  // Frame building (send direction)
  // -------------------------------------------------------------------

  /** @brief Calculate the wire-format size of a frame. */
  static size_t calc_frame_size(const uvcpp_ws_frame& frame);

  /**
   * @brief Serialize a frame into a wire-format buffer.
   * @param out    Output buffer (must have at least calc_frame_size() bytes).
   * @param frame  Frame to serialize.
   * @return       Number of bytes written.
   */
  static size_t build_frame(char* out, const uvcpp_ws_frame& frame);

  // -------------------------------------------------------------------
  // Mask utilities
  // -------------------------------------------------------------------

  /**
   * @brief Apply XOR mask in-place (RFC 6455 Section 5.3).
   * @param data    Data to mask/unmask.
   * @param len     Data length.
   * @param key     4-byte mask key.
   * @param offset  Starting offset within the mask cycle (default 0).
   */
  static void mask_inplace(char* data, size_t len,
                            const uint8_t key[4], size_t offset = 0);

  // -------------------------------------------------------------------
  // Compression (RFC 7692) — only when UVCPP_ZLIB_ENABLE=1
  // -------------------------------------------------------------------

#if UVCPP_ZLIB_ENABLE
  /**
   * @brief 开启 permessage-deflate 并记录协商结果。
   *
   * @param is_server          **本端角色**。必填，且没有默认值 —— 窗口位数和
   *                           context takeover 两个开关都是**按方向**命名的
   *                           （`client_*` 指客户端那一侧），不知道自己是哪
   *                           一端就没法把参数摆对。
   * @param client_no_ctxt     协商结果：客户端不得跨消息复用压缩上下文
   * @param server_no_ctxt     协商结果：服务端不得跨消息复用压缩上下文
   * @param client_max_bits    协商结果：客户端方向的最大窗口位数
   * @param server_max_bits    协商结果：服务端方向的最大窗口位数
   *
   * **方向规则（RFC 7692 §7.1.2.1–7.1.2.2）**：压缩用的是**自己**这一侧的
   * 位数（`-own_window_bits()`），解压用的是**对端**那一侧的位数。两者都是
   * 以 2 的补码负数传给 zlib 的 raw 模式（无 zlib 头尾）。搞反方向不会报错，
   * 只会在对端窗口比本端小时解出乱码，所以这里靠角色显式区分。
   *
   * 本函数只记录参数，不建 zlib 上下文 —— 上下文在首次 compress/decompress
   * 时惰性创建，这样 `enable_compression` 永远不会失败。
   */
  void enable_compression(bool is_server, bool client_no_ctxt, bool server_no_ctxt,
                          int client_max_bits = 15, int server_max_bits = 15);
  bool is_compression_enabled() const;

  /**
   * @brief 解一条**完整的消息**（RFC 7692 §7.2.2 接收侧）。
   *
   * 入参是剥过尾的线上负载，本函数会补回 `00 00 ff ff` 再 inflate。
   * `out` 会被**整体替换**（不是追加），失败时为空。
   *
   * @note 调用点必须在**消息重组之后**，不是每帧一次：RSV1 只出现在消息的
   *       第一帧上，中间帧没有这个标志可供判断。
   *
   * @warning **不检测截断。** 如果对端声明的负载长度里包含的是被截断的
   *          deflate 数据，本函数会返回 0 并给出**偏短（甚至偏长）**的结果 ——
   *          zlib 在"输入耗尽、块还没结束"和"正常到达同步刷出点"两种情况下
   *          都返回 Z_OK/Z_BUF_ERROR，返回码分不出来。这是**实测确认**的：
   *          一个按 RFC 手写的参照接收方对同样的截断输入给出完全相同的
   *          结果，所以这不是本实现的偏差，而是 zlib 返回码本身的表达能力
   *          边界。真正要防截断得靠 WebSocket 帧长度（`payload_expected_`）
   *          与消息完整性校验，不在这层。
   *
   * @return 0 成功；-1 数据非法（此时 zlib 上下文已被 reset，不会污染后续消息）。
   */
  int decompress(const uint8_t* in, size_t in_len, uvcpp_buf& out);

  /**
   * @brief 压一条**完整的消息**（RFC 7692 §7.2.1 发送侧）。
   *
   * 用 `Z_SYNC_FLUSH` 产出同步刷出点，再按 §7.2.3.4 剥掉结尾的
   * `00 00 ff ff`。**绝不能**用 `Z_FINISH`：那会写出 BFINAL=1 的结束块，
   * 对端 inflate 直接拿到 `Z_STREAM_END`，在 context takeover 下整条流就此
   * 报废，后续消息全部解不开。
   *
   * `out` 会被**整体替换**（不是追加），失败时为空。
   * @return 0 成功；-1 zlib 出错或产物形状不符合预期。
   */
  int compress(const uint8_t* in, size_t in_len, uvcpp_buf& out);

  /** @brief 生成 `Sec-WebSocket-Extensions` 应答/请求串（本端参数集合）。 */
  std::string get_extension_header() const;
#endif

 private:
  // -------------------------------------------------------------------
  // State-machine helper functions
  // -------------------------------------------------------------------

  size_t parse_opcode_byte(const uint8_t** pp, const uint8_t* end);
  size_t parse_len_byte(const uint8_t** pp, const uint8_t* end);
  size_t parse_extended_2(const uint8_t** pp, const uint8_t* end);
  size_t parse_extended_8(const uint8_t** pp, const uint8_t* end);
  size_t parse_mask_key(const uint8_t** pp, const uint8_t* end);
  size_t parse_payload(const uint8_t** pp, const uint8_t* end);

  void finish_frame();
  void set_error(int code, const char* reason);

  /**
   * @brief 长度字段一确定就立刻校验单帧上限。
   * @return true 通过；false 已 set_error，调用方必须立即返回。
   */
  bool check_frame_size();

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

  ws_parser_state  state_ = ws_parser_state::IDLE;
  uvcpp_ws_frame   frame_;
  uint64_t          payload_received_ = 0;  // Bytes of payload received so far
  uint64_t          payload_expected_ = 0;  // Total payload bytes expected
  uint64_t          max_frame_size_ = 0;    // 单帧负载上限（0 = 不限）
  int              last_error_ = 0;
  std::string      error_msg_;

  // Accumulation buffer for multi-chunk extended lengths
  uint8_t  ext_buf_[8];
  uint8_t  ext_pos_ = 0;
  uint8_t  ext_needed_ = 0;

  // Callback trampolines
  using frame_cb_t = void(*)(const uvcpp_ws_frame*, void*);
  using error_cb_t = void(*)(int, const char*, void*);

  frame_cb_t frame_fn_ = nullptr;
  void*      frame_arg_ = nullptr;
  error_cb_t error_fn_ = nullptr;
  void*      error_arg_ = nullptr;

#if UVCPP_ZLIB_ENABLE
  // 方向无关的取值：`client_*` / `server_*` 是**按方向**命名的协商结果，
  // 具体哪一侧是"自己"取决于本端角色，所以统一从这里取，别在 compress /
  // decompress 里直接读 `client_max_window_bits_`。
  int  own_window_bits() const;
  int  peer_window_bits() const;
  bool own_no_context_takeover() const;
  bool peer_no_context_takeover() const;

  z_stream deflate_ctx_;
  z_stream inflate_ctx_;
  bool deflate_inited_ = false;
  bool inflate_inited_ = false;
  bool compression_enabled_ = false;
  bool is_server_ = false;
  bool client_no_context_takeover_ = false;
  bool server_no_context_takeover_ = false;
  int client_max_window_bits_ = 15;
  int server_max_window_bits_ = 15;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_PARSER_H
