/**
 * @file src/grpc/uvcpp_grpc.cpp
 * @brief `src/grpc/uvcpp_grpc.h` 的实现。
 * @author zhuweiye
 * @version 1.6.10
 *
 * 这个文件里没有状态、没有全局量、没有 I/O —— 纯函数进纯函数出。这也是它不
 * 需要任何 `#if` 守卫的原因：它连 web 层是什么都不知道。
 */

#include <grpc/uvcpp_grpc.h>

#include <cstring>

namespace uvcpp {

namespace {

/// varint 的字节数上界：64 位按 7 位一组要 10 组。
constexpr size_t kVarintMaxBytes = 10u;

/// protobuf 的字段号上界 `(1 << 29) - 1`（低三位留给 wire type）。
constexpr uint32_t kFieldNumberMax = (1u << 29) - 1u;

/// 帧长度前缀能表达的最大值（4 字节大端）。
constexpr size_t kFrameLengthMax = 0xFFFFFFFFu;

void set_err(std::string* err, const char* msg) {
  if (err != nullptr) *err = msg;
}

/// 前 4 个字节按**大端**读成一个长度。gRPC 沿用 HTTP/2 的网络序，逐字节拼，
/// 不看主机序（`ntohl` 也能用，但这一句的意图更需要被看见）。
uint32_t read_be32(const char* p) {
  const unsigned char* u = reinterpret_cast<const unsigned char*>(p);
  return (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
         (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
}

}  // namespace

// =========================================================================
// varint
// =========================================================================

int grpc_varint_encode(std::string& out, uint64_t value) {
  unsigned char buf[kVarintMaxBytes];
  size_t        n = 0;
  do {
    unsigned char b = static_cast<unsigned char>(value & 0x7Fu);
    value >>= 7;
    if (value != 0) b |= 0x80u;  // 还有下一组
    buf[n++] = b;
  } while (value != 0);
  out.append(reinterpret_cast<const char*>(buf), n);
  return 0;
}

int grpc_varint_decode(const char* data, size_t len, uint64_t& value,
                       size_t& consumed, std::string* err) {
  value    = 0;
  consumed = 0;

  if (data == nullptr) {
    if (len == 0) {
      set_err(err, "varint：一个字节都没有");
      return UV_ENODATA;
    }
    set_err(err, "varint：data 为空但 len 非 0");
    return UV_EINVAL;
  }

  size_t shift = 0;
  for (size_t i = 0; i < len && i < kVarintMaxBytes; ++i) {
    const unsigned char b = static_cast<unsigned char>(data[i]);
    if (i == kVarintMaxBytes - 1) {
      // 第 10 个字节只带最高那一位（bit 63），所以它只能是 0 或 1，且不能挂续位。
      if ((b & 0x80u) != 0 || b > 0x01u) {
        set_err(err, "varint：第 10 个字节越界（64 位装不下）");
        return UV_EINVAL;
      }
    }
    value |= static_cast<uint64_t>(b & 0x7Fu) << shift;
    shift += 7;
    if ((b & 0x80u) == 0) {
      consumed = i + 1;
      return 0;
    }
  }

  // 走到这里有两种可能，处置完全相反：手里的字节够 10 个（那就是对端编错了），
  // 或者不够（那只是还没读完）。判据是"我们是因为什么停下的"。
  if (len >= kVarintMaxBytes) {
    set_err(err, "varint：第 10 个字节仍挂着续位");
    return UV_EINVAL;
  }
  set_err(err, "varint：字节不够（可能在等下一段）");
  return UV_ENODATA;
}

// =========================================================================
// 字段
// =========================================================================

int grpc_field_put_int32(std::string& out, uint32_t field, int32_t value) {
  if (field == 0 || field > kFieldNumberMax) return UV_EINVAL;
  grpc_varint_encode(out, (static_cast<uint64_t>(field) << 3) | 0u);  // wire type 0
  // 负数按 64 位符号扩展：protobuf 对 `int32` 就是这么发的（10 字节 varint）。
  grpc_varint_encode(out, static_cast<uint64_t>(static_cast<int64_t>(value)));
  return 0;
}

int grpc_field_get_int32(const char* data, size_t len, uint32_t field,
                         int32_t& value, std::string* err) {
  if (field == 0 || field > kFieldNumberMax) {
    set_err(err, "字段号越界（protobuf 的字段号是 1..(2^29-1)）");
    return UV_EINVAL;
  }
  if (data == nullptr && len != 0) {
    set_err(err, "data 为空但 len 非 0");
    return UV_EINVAL;
  }

  bool    found = false;
  int32_t last  = 0;
  size_t  pos   = 0;

  while (pos < len) {
    // 每个字段都以一个 varint 的 tag 开头：低三位是 wire type，其余是字段号。
    uint64_t tag    = 0;
    size_t   used   = 0;
    const int tag_rc = grpc_varint_decode(data + pos, len - pos, tag, used, err);
    if (tag_rc != 0) return tag_rc;
    pos += used;

    const uint32_t number     = static_cast<uint32_t>(tag >> 3);
    const uint32_t wire_type  = static_cast<uint32_t>(tag & 0x07u);
    if (number == 0) {
      set_err(err, "字段号为 0（protobuf 里不存在这个字段号，只可能是字节坏了）");
      return UV_EINVAL;
    }

    if (wire_type == 0) {  // varint
      uint64_t v      = 0;
      size_t   vused  = 0;
      const int v_rc = grpc_varint_decode(data + pos, len - pos, v, vused, err);
      if (v_rc != 0) return v_rc;
      pos += vused;
      if (number == field) {
        // 窄回 32 位：对端合法编码时这里是精确的（负数是 64 位符号扩展来的），
        // 而对端用了非最简编码时按截断处理 —— protobuf 的读侧也是这么做的。
        last  = static_cast<int32_t>(static_cast<int64_t>(v));
        found = true;
      }
      continue;
    }

    if (wire_type == 2) {  // 长度前缀 + 载荷
      uint64_t l      = 0;
      size_t   lused  = 0;
      const int l_rc = grpc_varint_decode(data + pos, len - pos, l, lused, err);
      if (l_rc != 0) return l_rc;
      pos += lused;
      if (l > static_cast<uint64_t>(len - pos)) {
        set_err(err, "长度前缀字段的载荷被截断");
        return UV_ENODATA;
      }
      pos += static_cast<size_t>(l);
      continue;
    }

    if (wire_type == 1) {  // 8 字节定长
      if (len - pos < 8u) {
        set_err(err, "64 位定长字段被截断");
        return UV_ENODATA;
      }
      pos += 8u;
      continue;
    }

    if (wire_type == 5) {  // 4 字节定长
      if (len - pos < 4u) {
        set_err(err, "32 位定长字段被截断");
        return UV_ENODATA;
      }
      pos += 4u;
      continue;
    }

    // 3/4 是已废弃的 group，6/7 保留。本模块不支持它们（见头注释的边界那一段），
    // 而"跳过"一个自己不认识长度的东西是猜不得的 —— 只能判非法。
    set_err(err, "wire type 3/4（group）与 6/7 本模块不支持");
    return UV_EINVAL;
  }

  if (!found) return UV_ENOENT;
  value = last;
  return 0;
}

// =========================================================================
// 帧
// =========================================================================

int grpc_frame_encode(const std::string& message, std::string& out,
                      size_t max_message_bytes) {
  // 上限取"调用方给的"与"4 字节前缀能表达的"里更小的那个：后者不是防御性编程，
  // 是编码本身的定义域（超了就没有表示法了）。
  const size_t cap =
      max_message_bytes < kFrameLengthMax ? max_message_bytes : kFrameLengthMax;
  if (message.size() > cap) return UV_EMSGSIZE;

  const uint32_t n = static_cast<uint32_t>(message.size());
  const char     header[GRPC_FRAME_HEADER_SIZE] = {
      0x00,  // 压缩标志：0 = 没压
      static_cast<char>((n >> 24) & 0xFFu), static_cast<char>((n >> 16) & 0xFFu),
      static_cast<char>((n >> 8) & 0xFFu), static_cast<char>(n & 0xFFu)};
  out.append(header, GRPC_FRAME_HEADER_SIZE);
  out.append(message);
  return 0;
}

int grpc_frame_decode(const char* data, size_t len, std::string& message,
                      size_t& consumed, std::string* err,
                      size_t max_message_bytes) {
  message.clear();
  consumed = 0;

  if (len < GRPC_FRAME_HEADER_SIZE) {
    set_err(err, "gRPC 帧：5 字节前缀还没收全");
    return UV_ENODATA;
  }
  if (data == nullptr) {
    set_err(err, "gRPC 帧：data 为空但 len 非 0");
    return UV_EINVAL;
  }

  const unsigned char flags = static_cast<unsigned char>(data[0]);
  if (flags != 0) {
    set_err(err, "gRPC 帧：压缩标志非 0，本库不支持 gRPC 的消息压缩");
    return UV_EINVAL;
  }

  const uint32_t n = read_be32(data + 1);
  if (static_cast<size_t>(n) > max_message_bytes) {
    set_err(err, "gRPC 帧：长度前缀报的载荷超过单条消息上限");
    return UV_EMSGSIZE;
  }
  if (len - GRPC_FRAME_HEADER_SIZE < static_cast<size_t>(n)) {
    set_err(err, "gRPC 帧：载荷还没收全");
    return UV_ENODATA;
  }

  message.assign(data + GRPC_FRAME_HEADER_SIZE, static_cast<size_t>(n));
  consumed = GRPC_FRAME_HEADER_SIZE + static_cast<size_t>(n);
  return 0;
}

int grpc_frame_decode_exact(const char* data, size_t len, std::string& message,
                            std::string* err, size_t max_message_bytes) {
  size_t    used = 0;
  const int rc   = grpc_frame_decode(data, len, message, used, err, max_message_bytes);
  if (rc != 0) return rc;

  if (used != len) {
    // 把 message 清掉：调用方拿到非 0 时不该再去看它，但"解了一半的字节留在
    // 出参里"是那种会在下游被误用的形状（比如被当成"部分成功"）。
    message.clear();
    set_err(err, "gRPC unary 请求体：第一帧之后还有多余的字节");
    return UV_EINVAL;
  }
  return 0;
}

}  // namespace uvcpp
