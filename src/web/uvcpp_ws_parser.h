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
  void enable_compression(bool client_no_ctxt, bool server_no_ctxt,
                          int client_max_bits = 15, int server_max_bits = 15);
  bool is_compression_enabled() const;
  int decompress(const uint8_t* in, size_t in_len, uvcpp_buf& out);
  int compress(const uint8_t* in, size_t in_len, uvcpp_buf& out);
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

  // -------------------------------------------------------------------
  // Member variables
  // -------------------------------------------------------------------

  ws_parser_state  state_ = ws_parser_state::IDLE;
  uvcpp_ws_frame   frame_;
  uint64_t          payload_received_ = 0;  // Bytes of payload received so far
  uint64_t          payload_expected_ = 0;  // Total payload bytes expected
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
  z_stream deflate_ctx_;
  z_stream inflate_ctx_;
  bool deflate_inited_ = false;
  bool inflate_inited_ = false;
  bool compression_enabled_ = false;
  bool client_no_context_takeover_ = false;
  bool server_no_context_takeover_ = false;
  int client_max_window_bits_ = 15;
  int server_max_window_bits_ = 15;
#endif
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_PARSER_H
