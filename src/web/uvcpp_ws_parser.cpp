/**
 * @file src/web/uvcpp_ws_parser.cpp
 * @brief WebSocket frame parser — RFC 6455 Section 5.  Fully self-implemented.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_ws_parser.h>

#if UVCPP_WEB_ENABLE
#include <cstring>

namespace uvcpp {

static void trampoline_frame(const uvcpp_ws_frame* f, void* arg) {
  auto* cb = static_cast<std::function<void(const uvcpp_ws_frame&)>*>(arg);
  (*cb)(*f);
}
static void trampoline_error(int code, const char* msg, void* arg) {
  auto* cb = static_cast<std::function<void(int, const char*)>*>(arg);
  (*cb)(code, msg);
}

uvcpp_ws_parser::uvcpp_ws_parser() {
  std::memset(ext_buf_, 0, sizeof(ext_buf_));
}

uvcpp_ws_parser::~uvcpp_ws_parser() {
  delete static_cast<std::function<void(const uvcpp_ws_frame&)>*>(frame_arg_); frame_arg_ = nullptr;
  delete static_cast<std::function<void(int, const char*)>*>(error_arg_); error_arg_ = nullptr;
#if UVCPP_ZLIB_ENABLE
  if (deflate_inited_) deflateEnd(&deflate_ctx_);
  if (inflate_inited_) inflateEnd(&inflate_ctx_);
#endif
}

void uvcpp_ws_parser::reset() {
  state_ = ws_parser_state::IDLE;
  frame_ = uvcpp_ws_frame();
  payload_received_ = 0; payload_expected_ = 0;
  last_error_ = 0; error_msg_.clear();
  ext_pos_ = 0; ext_needed_ = 0;
  std::memset(ext_buf_, 0, sizeof(ext_buf_));
}

size_t uvcpp_ws_parser::execute(const char* data, size_t len) {
  if (state_ == ws_parser_state::PARSE_ERROR) return 0;
  size_t total = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
  const uint8_t* end = p + len;

  while (p < end) {
    size_t n = 0;
    switch (state_) {
      case ws_parser_state::IDLE:        n = parse_opcode_byte(&p, end); break;
      case ws_parser_state::OPCODE:
      case ws_parser_state::PAYLOAD_LEN: n = parse_len_byte(&p, end); break;
      case ws_parser_state::EXTENDED_2:  n = parse_extended_2(&p, end); break;
      case ws_parser_state::EXTENDED_8:  n = parse_extended_8(&p, end); break;
      case ws_parser_state::MASK_KEY:    n = parse_mask_key(&p, end); break;
      case ws_parser_state::PAYLOAD:     n = parse_payload(&p, end); break;
      case ws_parser_state::COMPLETE:    return total;
      case ws_parser_state::PARSE_ERROR: return total;
    }
    total += n;
    if (n == 0) break; // no progress
  }
  return total;
}

// Byte 0: FIN+RSV+OPCODE
size_t uvcpp_ws_parser::parse_opcode_byte(const uint8_t** pp, const uint8_t* end) {
  const uint8_t* start = *pp;
  if (*pp >= end) return 0;
  uint8_t b = *(*pp)++;
  frame_.fin = (b & 0x80) != 0;
  frame_.rsv1 = (b & 0x40) != 0;
  frame_.rsv2 = (b & 0x20) != 0;
  frame_.rsv3 = (b & 0x10) != 0;
  frame_.opcode = static_cast<ws_opcode>(b & 0x0F);
  uint8_t op = static_cast<uint8_t>(frame_.opcode);
  if (op > 0xA) { set_error(-1, "Invalid opcode"); return static_cast<size_t>(*pp - start); }
  if (op >= 0x3 && op <= 0x7) { set_error(-2, "Reserved opcode range"); return static_cast<size_t>(*pp - start); }
  state_ = ws_parser_state::PAYLOAD_LEN;
  return static_cast<size_t>(*pp - start);
}

// Byte 1: MASK+length
size_t uvcpp_ws_parser::parse_len_byte(const uint8_t** pp, const uint8_t* end) {
  const uint8_t* start = *pp;
  if (*pp >= end) return 0;
  uint8_t b = *(*pp)++;
  frame_.masked = (b & 0x80) != 0;
  uint8_t len7 = b & 0x7F;
  if (len7 < 126) {
    payload_expected_ = len7;
    if (!check_frame_size()) return static_cast<size_t>(*pp - start);
    if (frame_.masked) { state_ = ws_parser_state::MASK_KEY; ext_needed_ = 4; ext_pos_ = 0; }
    else if (payload_expected_ > 0) { state_ = ws_parser_state::PAYLOAD; payload_received_ = 0; }
    else finish_frame();
  } else if (len7 == 126) { state_ = ws_parser_state::EXTENDED_2; ext_needed_ = 2; ext_pos_ = 0; }
  else { state_ = ws_parser_state::EXTENDED_8; ext_needed_ = 8; ext_pos_ = 0; }
  return static_cast<size_t>(*pp - start);
}

// Extended 2 bytes
size_t uvcpp_ws_parser::parse_extended_2(const uint8_t** pp, const uint8_t* end) {
  const uint8_t* start = *pp;
  while (*pp < end && ext_pos_ < ext_needed_) ext_buf_[ext_pos_++] = *(*pp)++;
  if (ext_pos_ >= ext_needed_) {
    payload_expected_ = (static_cast<uint64_t>(ext_buf_[0]) << 8) | ext_buf_[1];
    if (!check_frame_size()) return static_cast<size_t>(*pp - start);
    if (frame_.masked) { state_ = ws_parser_state::MASK_KEY; ext_needed_ = 4; ext_pos_ = 0; }
    else if (payload_expected_ > 0) { state_ = ws_parser_state::PAYLOAD; payload_received_ = 0; }
    else finish_frame();
  }
  return static_cast<size_t>(*pp - start);
}

// Extended 8 bytes
size_t uvcpp_ws_parser::parse_extended_8(const uint8_t** pp, const uint8_t* end) {
  const uint8_t* start = *pp;
  while (*pp < end && ext_pos_ < ext_needed_) ext_buf_[ext_pos_++] = *(*pp)++;
  if (ext_pos_ >= ext_needed_) {
    payload_expected_ = 0;
    for (int i = 0; i < 8; i++) payload_expected_ = (payload_expected_ << 8) | ext_buf_[i];
    if (payload_expected_ & 0x8000000000000000ULL) { set_error(-3, "Payload exceeds 2^63-1"); return static_cast<size_t>(*pp - start); }
    if (!check_frame_size()) return static_cast<size_t>(*pp - start);
    if (frame_.masked) { state_ = ws_parser_state::MASK_KEY; ext_needed_ = 4; ext_pos_ = 0; }
    else if (payload_expected_ > 0) { state_ = ws_parser_state::PAYLOAD; payload_received_ = 0; }
    else finish_frame();
  }
  return static_cast<size_t>(*pp - start);
}

// Mask key 4 bytes
size_t uvcpp_ws_parser::parse_mask_key(const uint8_t** pp, const uint8_t* end) {
  const uint8_t* start = *pp;
  while (*pp < end && ext_pos_ < ext_needed_) ext_buf_[ext_pos_++] = *(*pp)++;
  if (ext_pos_ >= ext_needed_) {
    for (int i = 0; i < 4; i++) frame_.mask_key[i] = ext_buf_[i];
    if (payload_expected_ > 0) { state_ = ws_parser_state::PAYLOAD; payload_received_ = 0; }
    else finish_frame();
  }
  return static_cast<size_t>(*pp - start);
}

// Payload
size_t uvcpp_ws_parser::parse_payload(const uint8_t** pp, const uint8_t* end) {
  const uint8_t* start = *pp;
  uint64_t remaining = payload_expected_ - payload_received_;
  size_t available = static_cast<size_t>(*pp < end ? end - *pp : 0);
  size_t to_copy = (static_cast<uint64_t>(available) < remaining) ? available : static_cast<size_t>(remaining);
  if (to_copy > 0) {
    if (frame_.masked) {
      const size_t off = static_cast<size_t>(payload_received_ % 4);
      // **一次追加整段，再原地异或** —— 不要逐字节 append。
      //
      // 当初这么写是因为 `uvcpp_buf::resize` 是"精确大小"重新分配（没有容量
      // 翻倍）：逐字节 append 就是每次新分配一块、把旧内容整个搬过去、再释放
      // 旧块 —— O(n²) 的拷贝量（512KB 的帧要搬约 130 GB）加 n 次分配，实测能
      // 让服务端在一个较大的**带掩码**帧上于 `resize` 里抛 `std::bad_alloc`。
      // 现在 `resize` 已有容量翻倍（见 `uvcpp_buf::capacity_`），O(n²) 那一半
      // 没了；仍然整段追加，省掉的是 51.2 万次 `resize` 调用与随之的边界检查。
      //
      // 掩码本来的定义就是"负载的第 i 个字节异或 mask_key[i % 4]"（§5.3），
      // 与追加方式无关，所以拆成"追加 + 原地异或"语义完全不变。`off` 是
      // 本段第一个字节在**整个负载**里的下标模 4 —— 帧跨多次 read 到达时，
      // 续段的相位必须接着上一段，不能从 0 重来。
      const size_t base = frame_.payload.size();   // 追加前的位置
      frame_.payload.append_data(reinterpret_cast<const char*>(*pp), to_copy);
      char* d = frame_.payload.get_data();
      for (size_t i = 0; i < to_copy; i++) {
        d[base + i] ^= static_cast<char>(frame_.mask_key[(off + i) % 4]);
      }
    } else {
      frame_.payload.append_data(reinterpret_cast<const char*>(*pp), to_copy);
    }
    *pp += to_copy;
    payload_received_ += to_copy;
  }
  if (payload_received_ >= payload_expected_) finish_frame();
  return static_cast<size_t>(*pp - start);
}

void uvcpp_ws_parser::finish_frame() {
  frame_.payload_len = payload_expected_;
  state_ = ws_parser_state::COMPLETE;
  if (frame_.is_control_frame()) {
    if (payload_expected_ > 125) { set_error(-4, "Control frame payload >125"); return; }
    if (!frame_.fin) { set_error(-5, "Fragmented control frame"); return; }
  }
  if (frame_fn_) frame_fn_(&frame_, frame_arg_);
}

void uvcpp_ws_parser::set_error(int code, const char* reason) {
  state_ = ws_parser_state::PARSE_ERROR; last_error_ = code; error_msg_ = reason;
  if (error_fn_) error_fn_(code, reason, error_arg_);
}

bool uvcpp_ws_parser::check_frame_size() {
  if (max_frame_size_ == 0) return true;
  if (payload_expected_ <= max_frame_size_) return true;
  set_error(-6, "Frame payload exceeds configured limit");
  return false;
}

void uvcpp_ws_parser::set_max_frame_size(uint64_t n) { max_frame_size_ = n; }
uint64_t uvcpp_ws_parser::get_max_frame_size() const { return max_frame_size_; }

void uvcpp_ws_parser::set_on_frame(std::function<void(const uvcpp_ws_frame&)> cb) {
  delete static_cast<std::function<void(const uvcpp_ws_frame&)>*>(frame_arg_);
  if (cb) { auto* p = new std::function<void(const uvcpp_ws_frame&)>(std::move(cb)); frame_fn_ = trampoline_frame; frame_arg_ = p; }
  else { frame_fn_ = nullptr; frame_arg_ = nullptr; }
}
void uvcpp_ws_parser::set_on_error(std::function<void(int, const char*)> cb) {
  delete static_cast<std::function<void(int, const char*)>*>(error_arg_);
  if (cb) { auto* p = new std::function<void(int, const char*)>(std::move(cb)); error_fn_ = trampoline_error; error_arg_ = p; }
  else { error_fn_ = nullptr; error_arg_ = nullptr; }
}

ws_parser_state uvcpp_ws_parser::get_state() const { return state_; }
bool uvcpp_ws_parser::is_idle() const { return state_ == ws_parser_state::IDLE; }
const uvcpp_ws_frame& uvcpp_ws_parser::get_current_frame() const { return frame_; }
int uvcpp_ws_parser::get_last_error() const { return last_error_; }

// Frame building
size_t uvcpp_ws_parser::calc_frame_size(const uvcpp_ws_frame& frame) {
  size_t size = 2;
  uint64_t len = frame.payload.size();
  if (len >= 126) size += (len > 0xFFFF) ? 8 : 2;
  if (frame.masked) size += 4;
  return size + static_cast<size_t>(len);
}

size_t uvcpp_ws_parser::build_frame(char* out, const uvcpp_ws_frame& frame) {
  uint8_t* p = reinterpret_cast<uint8_t*>(out);
  uint64_t len = frame.payload.size();
  p[0] = static_cast<uint8_t>(frame.opcode) & 0x0F;
  if (frame.fin) p[0] |= 0x80; if (frame.rsv1) p[0] |= 0x40;
  if (frame.rsv2) p[0] |= 0x20; if (frame.rsv3) p[0] |= 0x10;
  size_t off = 2;
  if (len < 126) p[1] = static_cast<uint8_t>(len);
  else if (len <= 0xFFFF) {
    p[1] = 126; p[2] = static_cast<uint8_t>((len>>8)&0xFF); p[3] = static_cast<uint8_t>(len&0xFF); off = 4;
  } else {
    p[1] = 127;
    for (int i = 0; i < 8; i++) p[2+i] = static_cast<uint8_t>((len>>(56-i*8))&0xFF);
    off = 10;
  }
  if (frame.masked) {
    p[1] |= 0x80;
    for (int i = 0; i < 4; i++) p[off+i] = frame.mask_key[i];
    off += 4;
    if (len > 0) {
      const char* s = frame.payload.get_const_data();
      for (uint64_t i = 0; i < len; i++) p[off+i] = static_cast<uint8_t>(s[i]) ^ frame.mask_key[i%4];
    }
  } else if (len > 0) {
    std::memcpy(p + off, frame.payload.get_const_data(), static_cast<size_t>(len));
  }
  return off + static_cast<size_t>(len);
}

void uvcpp_ws_parser::mask_inplace(char* data, size_t len, const uint8_t key[4], size_t offset) {
  for (size_t i = 0; i < len; i++) data[i] ^= static_cast<char>(key[(offset+i)%4]);
}

#if UVCPP_ZLIB_ENABLE
void uvcpp_ws_parser::enable_compression(bool is_server, bool nc, bool ns, int cbits, int sbits) {
  is_server_ = is_server;
  client_no_context_takeover_ = nc; server_no_context_takeover_ = ns;
  client_max_window_bits_ = cbits; server_max_window_bits_ = sbits;
  compression_enabled_ = true;
}
bool uvcpp_ws_parser::is_compression_enabled() const { return compression_enabled_; }

int uvcpp_ws_parser::own_window_bits() const {
  return is_server_ ? server_max_window_bits_ : client_max_window_bits_;
}
int uvcpp_ws_parser::peer_window_bits() const {
  return is_server_ ? client_max_window_bits_ : server_max_window_bits_;
}
bool uvcpp_ws_parser::own_no_context_takeover() const {
  return is_server_ ? server_no_context_takeover_ : client_no_context_takeover_;
}
bool uvcpp_ws_parser::peer_no_context_takeover() const {
  return is_server_ ? client_no_context_takeover_ : server_no_context_takeover_;
}

int uvcpp_ws_parser::decompress(const uint8_t* in, size_t in_len, uvcpp_buf& out) {
  out.clear();
  if (!inflate_inited_) {
    std::memset(&inflate_ctx_, 0, sizeof(inflate_ctx_));
    if (inflateInit2(&inflate_ctx_, -peer_window_bits()) != Z_OK) return -1;
    inflate_inited_ = true;
  }

  // RFC 7692 §7.2.2：对端发送前把结尾的 00 00 ff ff 剥掉了，收方必须补回去
  // 才能解到同步刷出点。少了这一步，每个真实浏览器发来的消息都会以
  // Z_BUF_ERROR 结束（数据其实已经全出来了），调用方按返回值判断就会把
  // 正常消息当错误丢掉。
  std::string payload(reinterpret_cast<const char*>(in), in_len);
  payload.append("\x00\x00\xff\xff", 4);

  inflate_ctx_.next_in  = reinterpret_cast<Bytef*>(&payload[0]);
  inflate_ctx_.avail_in = static_cast<uInt>(payload.size());

  int ret = Z_OK;
  const size_t C = 4096;
  do {
    unsigned char tmp[C];
    inflate_ctx_.next_out  = tmp;
    inflate_ctx_.avail_out = C;
    // 用 Z_SYNC_FLUSH 而不是 Z_FINISH：这是一个同步刷出点，不是流末尾。
    ret = inflate(&inflate_ctx_, Z_SYNC_FLUSH);
    if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END) {
      // 坏数据：重置上下文，别让错误状态污染后续消息
      inflateReset(&inflate_ctx_);
      out.clear();
      return -1;
    }
    const size_t w = C - inflate_ctx_.avail_out;
    if (w > 0) out.append_data(reinterpret_cast<const char*>(tmp), w);
  } while (inflate_ctx_.avail_out == 0);  // 只有填满输出缓冲才可能有剩余待解

  // 只在协商了本方不再复用上下文时才重置（否则会丢掉对端跨消息依赖的字典）
  if (peer_no_context_takeover()) inflateReset(&inflate_ctx_);
  return 0;
}

int uvcpp_ws_parser::compress(const uint8_t* in, size_t in_len, uvcpp_buf& out) {
  out.clear();
  if (!deflate_inited_) {
    std::memset(&deflate_ctx_, 0, sizeof(deflate_ctx_));
    if (deflateInit2(&deflate_ctx_, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     -own_window_bits(), 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      return -1;
    }
    deflate_inited_ = true;
  }

  deflate_ctx_.next_in  = const_cast<uint8_t*>(in);
  deflate_ctx_.avail_in = static_cast<uInt>(in_len);

  int ret = Z_OK;
  const size_t C = 4096;
  do {
    unsigned char tmp[C];
    deflate_ctx_.next_out  = tmp;
    deflate_ctx_.avail_out = C;
    // RFC 7692 §7.2.3.4：发送方 MUST NOT 输出 BFINAL=1 的块。Z_FINISH 恰恰会
    // 输出它 —— 对端 inflate 立刻拿到 Z_STREAM_END，context takeover 下流就废了。
    ret = deflate(&deflate_ctx_, Z_SYNC_FLUSH);
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      deflateReset(&deflate_ctx_);
      out.clear();
      return -1;
    }
    const size_t w = C - deflate_ctx_.avail_out;
    if (w > 0) out.append_data(reinterpret_cast<const char*>(tmp), w);
  } while (deflate_ctx_.avail_out == 0);

  // 剥掉 Z_SYNC_FLUSH 的尾巴 00 00 ff ff（同样是 §7.2.3.4 的要求）。
  // 空消息在这里得到单字节 0x00 —— 正是 §7.2.3.6 规定的形状。
  static const unsigned char k_tail[4] = {0x00, 0x00, 0xff, 0xff};
  const size_t n = out.size();
  if (n < 4 || std::memcmp(out.get_const_udata() + n - 4, k_tail, 4) != 0) {
    // 不该发生。宁可报错也不放一个形状不对的负载出去 —— 那会把问题推到对端。
    //
    // 这里**必须**一并 reset：deflate 上下文已经吃掉了这条消息的字节，而
    // 这条消息不会发出去了。不 reset 的话，下次 compress 产出的流里若有回溯
    // 引用指到这条"对端从没收到过"的消息，对端会直接解失败 —— 而且是在下一条
    // 消息上失败，排查起来离现场很远。
    deflateReset(&deflate_ctx_);
    out.clear();
    return -1;
  }
  out.resize(n - 4);

  if (own_no_context_takeover()) deflateReset(&deflate_ctx_);
  return 0;
}
std::string uvcpp_ws_parser::get_extension_header() const {
  std::string h="permessage-deflate";
  if(client_no_context_takeover_) h+="; client_no_context_takeover";
  if(server_no_context_takeover_) h+="; server_no_context_takeover";
  if(client_max_window_bits_!=15) h+="; client_max_window_bits="+std::to_string(client_max_window_bits_);
  if(server_max_window_bits_!=15) h+="; server_max_window_bits="+std::to_string(server_max_window_bits_);
  return h;
}
#endif

} // namespace uvcpp
#endif
