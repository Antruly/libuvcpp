#include <web/uvcpp_ws_connection.h>
#if UVCPP_WEB_ENABLE
#include <cstring>

namespace uvcpp {

uvcpp_ws_connection::uvcpp_ws_connection(uvcpp_tcp_client* tcp) : tcp_(tcp) {
  // 存活令牌（见头文件）：写完成回调用它判断"会话还在不在"。
  alive_token_ = std::shared_ptr<char>(new char);
  parser_.set_on_frame([this](const uvcpp_ws_frame& f) { on_ws_frame(f); });
  // 解析器层面的错误（长度越界、控制帧 >125、控制帧被分片、保留操作码、
  // 超过 frame 上限）原先**没人接**：`uvcpp_ws_connection` 只设了 on_frame，
  // 于是错误被静默吞掉、连接就那么哑着。这里先记下原因，由 on_tcp_data 的
  // 循环统一处理（只报一次、只发一次 Close）。
  parser_.set_on_error([this](int, const char* msg) {
    last_parse_error_ = (msg != nullptr) ? msg : "parse error";
  });
  // 单帧上限与消息上限同步：只卡一个都挡不住（分片绕过单帧、单帧绕过消息）。
  parser_.set_max_frame_size(max_message_size_);
}

uvcpp_ws_connection::~uvcpp_ws_connection() {
  // 正常路径上属主会先 terminate()/终结，此时 retired_ 已为真、tcp_ 已清空，
  // 这里什么都不做。走到下面说明有人违反了契约自己 delete 了会话 —— 至少把
  // 挂在客户端上的观察者摘掉，别留一个指向已释放对象的回调。
  if (tcp_ != nullptr && close_observer_id_ != 0) {
    tcp_->remove_close_observer(close_observer_id_);
  }
  close_observer_id_ = 0;
  tcp_ = nullptr;
  alive_token_.reset();   // 同上：还有人持着指向本对象的回调
  // 队列里的帧再也发不出去了：**如实结算**，别让调用方永远等一个不会来的
  // 完成回调。（正常路径上 notify_retired() 已经结算过了，这里是兜底。）
  if (!send_q_.empty()) fail_pending_sends(-1);
}

void uvcpp_ws_connection::start() {
  if (tcp_ == nullptr || started_) return;
  started_ = true;

  // **关闭观察者就是"这条连接没了"的唯一信号。**
  //
  // 读回调（下面那个 read_start）只在**对端**断开时才可能报告 —— 本端主动关
  // 的时候 `uv_close()` 只跑它自己的完成回调，读分支永远不会来。而会话结束
  // 有相当一部分是本端发起的（协议错误、`close()`、服务器停机），更不用说
  // 还有第三只手（http 层的闲置超时）可能把连接关掉。观察者是加法式槽位，
  // 三条路都会通知到（见 uvcpp_tcp_client::add_close_observer）。
  if (close_observer_id_ == 0) {
    close_observer_id_ = tcp_->add_close_observer([this]() { on_transport_closed(); });
  }

  tcp_->read_stop();
  tcp_->read_start([this](uvcpp_buf* buf) { if (buf && buf->size() > 0) on_tcp_data(buf); });
}

// =========================================================================
// 生命周期
// =========================================================================

void uvcpp_ws_connection::set_retire_callback(retire_fn cb) { retire_fn_ = std::move(cb); }

bool uvcpp_ws_connection::is_open() const { return !retired_; }

void uvcpp_ws_connection::on_transport_closed() {
  if (retired_) return;

  // 对端没发 Close 帧就走了：这是 RFC 6455 §7.1.5 的 1006（异常关闭）。
  // 只在**本端还没跟调用方交代过**的时候报 —— 对端规规矩矩发了 Close 帧的
  // 情况已经在 on_ws_frame 里用帧里的码报过了，协议错误也已经在 on_error
  // 里报过，那些时候再补一个 1006 只会是噪声。
  if (!close_notified_) {
    close_notified_ = true;
    if (on_close_) on_close_(ws_close_code::ABNORMAL_CLOSE, std::string());
  }

  notify_retired();
}

void uvcpp_ws_connection::close(ws_close_code code, const std::string& reason) {
  if (retired_) return;
  if (tcp_ == nullptr) { notify_retired(); return; }
  // 本端主动结束：调用方知情，不再补 on_close。
  close_notified_ = true;
  // send_close 的完成回调里会 `tcp_->close()`，随后的关闭观察者把终结补上。
  send_close(code, reason);
}

void uvcpp_ws_connection::terminate() {
  if (retired_) return;
  close_notified_ = true;   // 属主拆台，不是对端走人
  notify_retired();
}

void uvcpp_ws_connection::notify_retired() {
  if (retired_) return;
  retired_ = true;

  // 令牌作废：此后任何"由别人持有的、指向本对象"的闭包（写完成回调）直接
  // 返回。**要排在下面所有用户回调之前** —— 那些回调里有人可能顺手把会话
  // 删掉（属主在 retire_fn 里删是合法用法）。
  alive_token_.reset();

  // 先把观察者摘掉。**顺序要紧**：属主可能在本会话之前就把客户端销毁
  // （服务器析构就是这样），留下的观察者会在那一刻回调到一个已释放的会话。
  if (tcp_ != nullptr && close_observer_id_ != 0) {
    tcp_->remove_close_observer(close_observer_id_);
  }
  close_observer_id_ = 0;

  // 此后不再碰 tcp_：属主的回收是延迟的，但客户端可能在本轮关闭流程里就被
  // 框架释放掉（`fire_close_callbacks` 的最后一个槽会 `delete` 它）。
  tcp_ = nullptr;

  // 队列里的帧发不出去了，逐条结算 —— 不结算调用方会永远等下去。
  if (!send_q_.empty()) fail_pending_sends(-1);

  // 上报属主。回调返回之后本对象随时可能被 delete，所以这必须是最后一步，
  // 并且先把回调搬出来（属主的 delete 会销毁 retire_fn_ 自己）。
  retire_fn cb(std::move(retire_fn_));
  retire_fn_ = nullptr;
  if (cb) cb(this);
}

// =========================================================================
// 接收：驱动解析器 + 消息重组
// =========================================================================

void uvcpp_ws_connection::on_tcp_data(uvcpp_buf* buf) {
  if (!buf || buf->size() == 0) return;

  const char* p = buf->get_const_data();
  size_t      n = buf->size();

  // 解析器**一次只吃一帧**：帧完成后它停在 COMPLETE 并把剩余字节原样留在
  // 缓冲里（uvcpp_ws_parser.cpp 的 `case COMPLETE: return total;`），而且它
  // **不会自己 reset** —— 按设计由调用方负责。连接层原先既没 reset 也没循环，
  // 所以一个连接只处理得了第一帧，之后 `execute()` 永远在第一行进
  // `case COMPLETE: return 0`。这里补上"喂 → 看状态 → reset → 继续喂"。
  while (n > 0 && !protocol_failed_) {
    size_t used = parser_.execute(p, n);
    ws_parser_state st = parser_.get_state();

    if (st == ws_parser_state::COMPLETE) {
      if (used == 0) break;          // 防御：不前进就退出，绝不空转
      parser_.reset();
      p += used;
      n -= used;
      continue;
    }
    if (st == ws_parser_state::PARSE_ERROR) {
      // 帧层面就非法。此时解析器已经废了，连接也不能再用。
      protocol_error(ws_close_code::PROTOCOL_ERROR, last_parse_error_);
      return;
    }
    break;                           // 数据不够一帧，等下一批
  }
}

void uvcpp_ws_connection::on_ws_frame(const uvcpp_ws_frame& frame) {
  if (protocol_failed_) return;

  // ---------------------------------------------------------------------
  // 控制帧：可以穿插在分片消息中间，但**不参与重组**，也不影响 in_message_
  // （RFC 6455 §5.4）。控制帧自身不得分片、负载不得 >125 字节 —— 这两条已经
  // 在 parser 的 finish_frame() 里拦掉了。
  // ---------------------------------------------------------------------
  if (frame.is_control_frame()) {
    switch (frame.opcode) {
      case ws_opcode::PING:
        send_pong(frame.payload.get_const_data(), frame.payload.size());
        if (on_ping_) on_ping_(frame.payload.get_const_udata(), frame.payload.size());
        break;
      case ws_opcode::PONG:
        if (on_pong_) on_pong_(frame.payload.get_const_udata(), frame.payload.size());
        break;
      case ws_opcode::CLOSE: {
        auto cd = frame.get_close_code();
        auto cr = frame.get_close_reason();
        // 对端发起了关闭：把帧里的码原样回过去（RFC 6455 §5.5.1），然后把
        // "对端是怎么走的"告诉调用方。**置位要在回调之前** —— 紧随其后的
        // 传输层关闭不该再补一个 1006。
        close_notified_ = true;
        send_close(cd, cr);
        if (on_close_) on_close_(cd, cr);
        break;
      }
      default: break;
    }
    return;
  }

  // ---------------------------------------------------------------------
  // 数据帧
  // ---------------------------------------------------------------------

  // rsv2/rsv3：没有任何扩展被协商，必须为 0（RFC 6455 §5.2）。
  if (frame.rsv2 || frame.rsv3) {
    protocol_error(ws_close_code::PROTOCOL_ERROR, "RSV2/RSV3 set but no extension negotiated");
    return;
  }

  if (frame.opcode == ws_opcode::CONTINUATION) {
    if (!in_message_) {
      protocol_error(ws_close_code::PROTOCOL_ERROR, "CONTINUATION frame with no message in progress");
      return;
    }
    // RSV1 只允许出现在消息的**首帧**上（RFC 7692 §7.2.2）：中间帧带它就说明
    // 对端把压缩标志放在了错误的帧上，或者分片边界与压缩边界不一致。
    if (frame.rsv1) {
      protocol_error(ws_close_code::PROTOCOL_ERROR, "RSV1 set on a continuation frame");
      return;
    }
  } else {
    // TEXT / BINARY：一条消息还没结束就再来一条数据帧，是协议错误 —— 不能
    // 悄悄把它当成新消息（那会让对端的分片序列和本端的消息边界错位）。
    if (in_message_) {
      protocol_error(ws_close_code::PROTOCOL_ERROR,
                     "new data frame while a fragmented message is in progress");
      return;
    }
    if (frame.rsv1) {
#if UVCPP_ZLIB_ENABLE
      if (!parser_.is_compression_enabled()) {
        protocol_error(ws_close_code::PROTOCOL_ERROR, "RSV1 set but permessage-deflate not negotiated");
        return;
      }
#else
      protocol_error(ws_close_code::PROTOCOL_ERROR, "RSV1 set but permessage-deflate not negotiated");
      return;
#endif
    }
    in_message_        = true;
    message_opcode_    = frame.opcode;
    message_compressed_ = frame.rsv1;
    message_payload_.clear();
  }

  // 聚合上限：**必须在 append 之前**判。等 append 完再判，那一次 append 已经
  // 把内存吃掉了；分片的意义就是让每一片都很小、合起来很大。
  if (max_message_size_ > 0 &&
      message_payload_.size() + frame.payload.size() > max_message_size_) {
    protocol_error(ws_close_code::MESSAGE_TOO_BIG, "aggregated message exceeds configured limit");
    return;
  }

  if (frame.payload.size() > 0) {
    message_payload_.append_data(frame.payload.get_const_data(), frame.payload.size());
  }

  if (!frame.fin) return;   // 还有后续分片
  deliver_message();
}

void uvcpp_ws_connection::deliver_message() {
  const ws_opcode op         = message_opcode_;
  const bool      compressed = message_compressed_;

  in_message_         = false;
  message_opcode_     = ws_opcode::TEXT;
  message_compressed_ = false;

  const uint8_t* data = message_payload_.get_const_udata();
  size_t         len  = message_payload_.size();

#if UVCPP_ZLIB_ENABLE
  // 解压放在**重组之后**（RFC 7692 §7.2.2）：RSV1 只在首帧上，中间帧没有这个
  // 标志可供判断，所以压缩边界和消息边界必须对齐 —— 先重组再解压是唯一顺序。
  uvcpp_buf plain;
  if (compressed) {
    if (parser_.decompress(data, len, plain) != 0) {
      message_payload_.clear();
      protocol_error(ws_close_code::INVALID_PAYLOAD, "permessage-deflate decode failed");
      return;
    }
    data = plain.get_const_udata();
    len  = plain.size();
  }
#endif

  if (op == ws_opcode::TEXT) {
    // 空消息时 data 可能是 nullptr，`std::string(nullptr, 0)` 是 UB，单独走一条。
    if (on_text_) {
      on_text_(len ? std::string(reinterpret_cast<const char*>(data), len) : std::string());
    }
  } else if (op == ws_opcode::BINARY) {
    if (on_bin_) on_bin_(data, len);
  }

  message_payload_.clear();
}

void uvcpp_ws_connection::protocol_error(ws_close_code code, const std::string& reason) {
  if (protocol_failed_) return;      // 只报一次、只发一次 Close
  protocol_failed_ = true;
  in_message_      = false;
  message_payload_.clear();
  // 本端判定对端违规而结束会话：原因已经由 on_error 说清楚了，不该再补一个
  // "对端异常关闭"(1006) —— 那会把"我判的"说成"它跑的"。
  close_notified_ = true;

  if (on_error_) on_error_(static_cast<int>(code), reason);
  // RFC 6455 §7.1.7：先发带状态码的 Close，再关连接。send_frame 的完成回调里
  // 会 tcp_->close()；发送队列保证这一帧不会因为在途写被丢掉。
  send_close(code, reason);
}

// =========================================================================
// 发送：队列化，保证顺序且不丢帧
// =========================================================================

int uvcpp_ws_connection::send_frame(const uvcpp_ws_frame& frame, std::function<void(int)> cb) {
  if (!tcp_) { if (cb) cb(-1); return -1; }
  // 懒启动（客户端侧就是靠这一句：升级完成时不 arm，等第一次发帧再 arm，
  // 免得在活跃的读回调里调 read_stop）。标志由 start() 自己置，不在这里置。
  if (!started_) start();

  // 发送通道已经坏了（之前某次写失败）：立刻结算，别排队等一个不会来的完成回调。
  if (send_error_ != 0) { if (cb) cb(send_error_); return send_error_; }

  pending_send ps;
  size_t sz = uvcpp_ws_parser::calc_frame_size(frame);
  ps.bytes.resize(sz);
  if (sz > 0) uvcpp_ws_parser::build_frame(&ps.bytes[0], frame);
  ps.cb = std::move(cb);
  send_q_.push_back(std::move(ps));

  pump_send();
  return 0;
}

void uvcpp_ws_connection::pump_send() {
  if (write_in_flight_ || send_q_.empty() || !tcp_) return;

  write_in_flight_ = true;

  // tcp_->write() 会先把数据**复制**进自己的 uvcpp_buf（uvcpp_buf.cpp 的
  // `uvcpp_buf(const char*, size_t)` 是 memcpy，且 out_uv_buf() 转移所有权），
  // 所以队首的字节传进去之后就不必再管它 —— 队列本身也因此在写出前可以随便改。
  const std::string& b = send_q_.front().bytes;
  // 捕获存活令牌（C++11 没有 init-capture，weak_ptr 具名捕获即可）。这个闭包
  // 由 TCP 客户端持有，可能**活得比会话久** —— 令牌失效就什么都不碰。
  std::weak_ptr<char> tok = alive_token_;
  int rc = tcp_->write(b.data(), b.size(), [this, tok](int st) {
    if (tok.expired()) return;   // 会话已经终结/回收，别碰任何成员
    on_send_complete(st);
  });

  if (rc != 0) {
    // 没提交成功。`uvcpp_tcp_client::write()` 在返回非 0 时**没有保存回调**，
    // 完成回调不会来，所以队首必须在这里自己结算，否则队列永远卡住。
    // UV_EALREADY 意味着有别的代码在直接写这条连接；本层是升级后唯一的写方，
    // 走到这里说明假设被打破了 —— 如实报告错误，绝不静默丢帧。
    write_in_flight_ = false;
    pending_send ps = std::move(send_q_.front());
    send_q_.pop_front();
    send_error_ = rc;
    if (ps.cb) ps.cb(rc);
    fail_pending_sends(rc);
  }
}

void uvcpp_ws_connection::on_send_complete(int status) {
  write_in_flight_ = false;
  if (send_q_.empty()) return;       // 不该发生（队首只有写出后才出队）

  pending_send ps = std::move(send_q_.front());
  send_q_.pop_front();

  if (status != 0) send_error_ = status;
  // 回调可能重入：send_close 的回调会调 tcp_->close()。所以先出队再回调，
  // 并且回调之后才继续 pump —— 连接已经关了的话 pump 里的 write 会返回
  // UV_ENOTCONN，正好走上面的失败结算。
  if (ps.cb) ps.cb(status);

  if (send_error_ != 0) { fail_pending_sends(send_error_); return; }
  pump_send();
}

void uvcpp_ws_connection::fail_pending_sends(int err) {
  if (send_q_.empty()) return;
  // 先整体摘出来再逐个回调：回调里可能又调用 send_frame，边遍历边被改会出事。
  // （摘出来之后新入队的帧会被 send_error_ 立刻挡掉，不会漏掉也不会递归。）
  std::deque<pending_send> q;
  q.swap(send_q_);
  while (!q.empty()) {
    pending_send ps = std::move(q.front());
    q.pop_front();
    if (ps.cb) ps.cb(err);
  }
}

int uvcpp_ws_connection::send_data(ws_opcode op, const char* d, size_t n,
                                   std::function<void(int)> cb) {
  uvcpp_ws_frame f;
  f.opcode = op;
  if (d && n > 0) {
#if UVCPP_ZLIB_ENABLE
    if (parser_.is_compression_enabled() && n >= compress_min_size_) {
      uvcpp_buf c;
      if (parser_.compress(reinterpret_cast<const uint8_t*>(d), n, c) != 0) {
        // **不能**退回未压缩形态了事：deflate 上下文可能已经吃掉了这条消息，
        // 而对端永远不会收到它 —— context takeover 下后续消息里的回溯引用会
        // 指到对端没有的历史上，对端直接解失败。如实报错，让调用方知道这一帧
        // 没发出去。（parser_.compress 两条失败路径都会 deflateReset，所以
        // 上下文本身是干净的，连接还能继续用。）
        if (cb) cb(-1);
        return -1;
      }
      f.payload.clone_data(reinterpret_cast<const char*>(c.get_const_udata()),
                           c.size());
      // RSV1 只在**消息首帧**上（§7.2.2）。本层消息都是单帧发的，所以就是这里。
      f.rsv1 = true;
    } else {
      f.payload.clone_data(d, n);
    }
#else
    f.payload.clone_data(d, n);
#endif
  }
  return send_frame(f, cb);
}

int uvcpp_ws_connection::send_text(const char* d, size_t n, std::function<void(int)> cb) {
  return send_data(ws_opcode::TEXT, d, n, cb);
}

int uvcpp_ws_connection::send_binary(const char* d, size_t n, std::function<void(int)> cb) {
  return send_data(ws_opcode::BINARY, d, n, cb);
}

#if UVCPP_ZLIB_ENABLE
void uvcpp_ws_connection::enable_compression(bool is_server,
                                             const uvcpp_ws_deflate_params& p) {
  parser_.enable_compression(is_server, p.client_no_context_takeover,
                             p.server_no_context_takeover,
                             p.client_max_window_bits, p.server_max_window_bits);
}

bool uvcpp_ws_connection::is_compression_enabled() const {
  return parser_.is_compression_enabled();
}

void uvcpp_ws_connection::set_compress_min_size(size_t n) { compress_min_size_ = n; }
size_t uvcpp_ws_connection::get_compress_min_size() const { return compress_min_size_; }
#endif

int uvcpp_ws_connection::send_ping(const char* d, size_t n) {
  uvcpp_ws_frame f; f.opcode = ws_opcode::PING;
  if (d && n > 0) f.payload.clone_data(d, n);
  return send_frame(f, nullptr);
}

int uvcpp_ws_connection::send_pong(const char* d, size_t n) {
  uvcpp_ws_frame f; f.opcode = ws_opcode::PONG;
  if (d && n > 0) f.payload.clone_data(d, n);
  return send_frame(f, nullptr);
}

int uvcpp_ws_connection::send_close(ws_close_code cd, const std::string& rs) {
  uvcpp_ws_frame f; f.opcode = ws_opcode::CLOSE;
  // 控制帧负载上限 125 字节（RFC 6455 §5.5）；扣掉 2 字节状态码，原因最多 123。
  // 超了会拼出一个对端**必须拒绝**的帧，所以在这里截断，而不是让它发出去。
  f.set_close_payload(cd, rs.size() > 123 ? rs.substr(0, 123) : rs);
  return send_frame(f, [this](int) {
    // 走 client->close()：直接关句柄会绕过框架的关闭回调，服务端的客户端对象
    // 会被漏在登记表里。关完之后不能再用 tcp_（框架可能已经把它 delete 掉）。
    if (tcp_ && tcp_->get_tcp()) tcp_->close();
  });
}

void uvcpp_ws_connection::on_text(std::function<void(const std::string&)> cb) { on_text_ = std::move(cb); }
void uvcpp_ws_connection::on_binary(std::function<void(const uint8_t*, size_t)> cb) { on_bin_ = std::move(cb); }
void uvcpp_ws_connection::on_ping(std::function<void(const uint8_t*, size_t)> cb) { on_ping_ = std::move(cb); }
void uvcpp_ws_connection::on_pong(std::function<void(const uint8_t*, size_t)> cb) { on_pong_ = std::move(cb); }
void uvcpp_ws_connection::on_close(std::function<void(ws_close_code, const std::string&)> cb) { on_close_ = std::move(cb); }
void uvcpp_ws_connection::on_error(std::function<void(int, const std::string&)> cb) { on_error_ = std::move(cb); }

void uvcpp_ws_connection::set_max_message_size(size_t n) {
  max_message_size_ = n;
  parser_.set_max_frame_size(n);     // 0 原样传下去 = 不限
}
size_t uvcpp_ws_connection::get_max_message_size() const { return max_message_size_; }

uvcpp_tcp_client* uvcpp_ws_connection::get_tcp_client() { return tcp_; }

}  // namespace uvcpp
#endif
