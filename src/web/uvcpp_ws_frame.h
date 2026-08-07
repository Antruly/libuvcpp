/**
 * @file src/web/uvcpp_ws_frame.h
 * @brief WebSocket frame types per RFC 6455 Section 5.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_FRAME_H
#define SRC_WEB_UVCPP_WS_FRAME_H

#if UVCPP_WEB_ENABLE

#include <cstdint>
#include <string>
#include <uvcpp/uvcpp_buf.h>

namespace uvcpp {

// =========================================================================
// WebSocket opcodes — RFC 6455 Section 5.2
// =========================================================================

enum class ws_opcode : uint8_t {
  CONTINUATION = 0x0,
  TEXT         = 0x1,
  BINARY       = 0x2,
  // 0x3 - 0x7: reserved for further non-control frames
  CLOSE        = 0x8,
  PING         = 0x9,
  PONG         = 0xA,
  // 0xB - 0xF: reserved for further control frames
};

// =========================================================================
// Close status codes — RFC 6455 Section 7.4
// =========================================================================

enum class ws_close_code : uint16_t {
  NORMAL            = 1000,
  GOING_AWAY        = 1001,
  PROTOCOL_ERROR    = 1002,
  UNSUPPORTED_DATA  = 1003,
  NO_STATUS         = 1005,
  ABNORMAL_CLOSE    = 1006,
  INVALID_PAYLOAD   = 1007,
  POLICY_VIOLATION  = 1008,
  MESSAGE_TOO_BIG   = 1009,
  EXTENSION_NEEDED  = 1010,
  INTERNAL_ERROR    = 1011,
};

// =========================================================================
// WebSocket frame
// =========================================================================

struct uvcpp_ws_frame {
  bool       fin       = true;
  bool       rsv1      = false;   // RFC 7692 compression flag
  bool       rsv2      = false;   // reserved (RFC 8441 WebSocket over HTTP/2)
  bool       rsv3      = false;   // reserved
  ws_opcode  opcode    = ws_opcode::TEXT;
  bool       masked    = false;
  uint8_t    mask_key[4] = {0};
  uint64_t   payload_len = 0;
  uvcpp_buf  payload;

  bool is_control_frame() const {
    return static_cast<uint8_t>(opcode) >= 0x8;
  }
  bool is_data_frame() const {
    return static_cast<uint8_t>(opcode) <= 0x2;
  }

  /** Parse close code + reason from payload (big-endian 2-byte code). */
  ws_close_code get_close_code() const {
    if (payload.size() < 2) return ws_close_code::NO_STATUS;
    const unsigned char* d = payload.get_const_udata();
    uint16_t code = (static_cast<uint16_t>(d[0]) << 8) | d[1];
    return static_cast<ws_close_code>(code);
  }

  std::string get_close_reason() const {
    if (payload.size() <= 2) return "";
    return std::string(payload.get_const_data() + 2, payload.size() - 2);
  }

  void set_close_payload(ws_close_code code, const std::string& reason = "") {
    uint16_t c = static_cast<uint16_t>(code);
    unsigned char buf[2] = {static_cast<unsigned char>(c >> 8),
                             static_cast<unsigned char>(c & 0xFF)};
    payload.clear();
    payload.append_data(reinterpret_cast<const char*>(buf), 2);
    if (!reason.empty()) {
      payload.append_data(reason.c_str(), reason.size());
    }
  }
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_FRAME_H
