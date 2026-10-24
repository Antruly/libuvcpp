#include <web/uvcpp_ws_connection.h>

#include <random>
#if UVCPP_WEB_ENABLE
#include <cstring>

namespace uvcpp {

// 默认构造：转交给两参构造，只是不带传输层（`tcp_` 留空）。
// 类成员本来就有默认值（`tcp_ = nullptr`、`role_ = ws_role::SERVER`），`start()`
// 也早就写了 `if (tcp_ == nullptr || started_) return;`，所以这样建出来的对象是
// **合法且惰性的**：可以安全地构造、析构、调 `start()`，只是不会有任何 I/O。
// 声明来自 `UVCPP_DEFINE_FUNC`（它同时声明默认构造与析构），而此前只有析构有
// 定义 ⇒ `uvcpp_ws_connection c;` 编得过、链接不过。转交是这里唯一不丢初始化
// 的写法：`alive_token_` 的分配与解析器回调的挂接都在两参构造体里。
uvcpp_ws_connection::uvcpp_ws_connection()
    : uvcpp_ws_connection(nullptr, ws_role::SERVER) {}

uvcpp_ws_connection::uvcpp_ws_connection(uvcpp_tcp_client* tcp, ws_role role)
    : tcp_(tcp), role_(role) {
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

void uvcpp_ws_connection::feed_pending(const char* data, size_t len) {
  // 还没 start()（读都没 arm，谈何"补投"）、或者会话已经结束了（对方断开、
  // 或者升级方在自己的回调里就把会话关了）—— 这批字节直接丢掉。
  if (!started_ || retired_ || data == nullptr || len == 0) return;
  uvcpp_buf buf(data, len);   // 拷贝构造：来源是升级方的临时缓冲
  on_tcp_data(&buf);
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
  //
  // **整批帧处理完才泵一次写**（`in_read_dispatch_`）。一次读回调里常常躺着
  // 十几帧（对端是批发的），逐帧 `pump_send` 会让**第一帧**立刻占住在途位、
  // 单独成段，后续各帧才凑成第二批 —— 而且**只有本批合并**，跨批不合并。
  // 合成的效果是一次读一段，段数正好是 `echo-ws-pipeline` 那一档量的东西
  // （"Write coalescing and syscall reduction on the send path"），实测见
  // `pump_send` 上面那段注释。
  //
  // 守卫是**析构**里收尾，不是为了好看：下面 `PARSE_ERROR` 那条路要
  // `return`，手写清除会漏。
  //
  // **`protocol_failed_` 时不跳过泵**：`protocol_error()` 正是先置这个标志、
  // 再 `send_close()` 把 Close 帧排进队列的（`uvcpp_ws_connection.cpp`
  // `protocol_error`），跳过就等于把 Close 帧永远留在队列里 —— 对端收不到
  // 状态码，本端的 `tcp_->close()` 回调也永远不会被触发。只有 `retired_`
  // （会话已终结/回收）才真的不该再碰 `tcp_`。
  struct DispatchGuard {
    uvcpp_ws_connection* self;
    ~DispatchGuard() {
      self->in_read_dispatch_ = false;
      if (!self->retired_) self->pump_send();
    }
  };
  // **置位必须在守卫构造之前**：这一句漏掉的话 `send_frame` 里的 `if
  // (!in_read_dispatch_)` 恒为真，整个延后就是死代码 —— 而它不报错、不崩，
  // 只是段数停在"首帧单独一段 + 其余合并"（实测 0.125 段/帧，而不是应有的
  // 0.0625）。`close_delivered_on_protocol_error` 那条用例盯着守卫这一侧。
  in_read_dispatch_ = true;
  DispatchGuard guard{this};

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

  // 逐帧观察（#50）：**放在所有自动处理之前**，让调试器看到线上原样
  // （含随后会被判协议错误的帧）。回调里可能 send/close，甚至触发协议错误，
  // 所以回来之后要重新看一眼状态。
  if (on_frame_) on_frame_(frame);
  if (protocol_failed_) return;

  // ---------------------------------------------------------------------
  // 掩码的方向性要求（RFC 6455 §5.1）。
  //
  // 这一条对**所有帧**都成立，控制帧也不例外（§5.5），所以放在控制帧/数据帧
  // 分叉之前 —— 放进去就只能盖住一半。
  //
  // 规范原文是两侧各一条 MUST："The server MUST close the connection upon
  // receiving a frame that is not masked" 与客户端侧的镜像要求；两个方向都用
  // 1002。掩码本来的用途是防中间代理缓存投毒（§10.3），服务端不强制等于把
  // 那条防护让掉。
  // ---------------------------------------------------------------------
  if (is_server() && !frame.masked) {
    protocol_error(ws_close_code::PROTOCOL_ERROR, "client frame must be masked");
    return;
  }
  if (!is_server() && frame.masked) {
    protocol_error(ws_close_code::PROTOCOL_ERROR, "server frame must not be masked");
    return;
  }

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

namespace {

/**
 * @brief UTF-8 合法性校验（RFC 3629）。
 *
 * RFC 6455 §8.1：文本帧的负载必须是合法 UTF-8，不是就 MUST Fail the
 * WebSocket Connection。库此前**一处校验都没有** —— 非法字节会被原样交给
 * `on_text`，一个把它转给浏览器的应用（聊天室这类）会在客户端那边才炸。
 *
 * 覆盖的不只是"形状不对"，还有三处**语法合法但语义非法**的：
 *   - overlong 编码（`0xC0 0x80` 这种"用两个字节写 ASCII"）；
 *   - UTF-16 代理区 U+D800–U+DFFF（CESU-8 那种编码不该出现在 UTF-8 里）；
 *   - 超出 U+10FFFF 的范围（`0xF4 0x90` 以上）。
 * 只判首字节与续字节的形状会让这三类漏过去 —— 而它们正是"能过校验却仍不是
 * 合法 UTF-8"的全部来源。
 */
bool ws_is_valid_utf8(const uint8_t* p, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t c = p[i];
    if (c <= 0x7F) { ++i; continue; }            // ASCII

    size_t need;
    if (c >= 0xC2 && c <= 0xDF)      need = 1;   // 2 字节（0xC0/0xC1 是 overlong）
    else if (c >= 0xE0 && c <= 0xEF) need = 2;   // 3 字节
    else if (c >= 0xF0 && c <= 0xF4) need = 3;   // 4 字节（>0xF4 超出 U+10FFFF）
    else return false;                           // 孤立续字节 / 非法首字节

    if (n - i < need + 1) return false;          // 尾部不够
    for (size_t k = 1; k <= need; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) return false;   // 续字节必须是 10xxxxxx
    }
    if (need == 2) {
      if (c == 0xE0 && p[i + 1] < 0xA0) return false;   // overlong
      if (c == 0xED && p[i + 1] > 0x9F) return false;   // 代理区
    } else if (need == 3) {
      if (c == 0xF0 && p[i + 1] < 0x90) return false;   // overlong
      if (c == 0xF4 && p[i + 1] > 0x8F) return false;   // > U+10FFFF
    }
    i += need + 1;
  }
  return true;
}

}  // namespace

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
    // RFC 6455 §8.1：文本消息必须是合法 UTF-8，否则以 1007 失败。
    //
    // **位置在解压之后**：应用看到的是解压后的字节，校验就必须落在同一份字节
    // 上 —— 在上游验压缩过的数据既无意义（那是二进制），也会让带 permessage-
    // deflate 的连接永远通不过。
    //
    // `data` 在空消息时可能是 nullptr，校验器对 (nullptr, 0) 返回 true。
    if (!ws_is_valid_utf8(data, len)) {
      message_payload_.clear();
      protocol_error(ws_close_code::INVALID_PAYLOAD,
                     "text message is not valid UTF-8");
      return;
    }
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

int uvcpp_ws_connection::send_frame(const uvcpp_ws_frame& frame,
                                    std::function<void(int)> cb) {
  // payload 深拷贝（`uvcpp_buf` 的拷贝构造会克隆字节），调用方在返回后可以
  // 立刻释放自己的缓冲。掩码按本端角色盖，调用方填的 `masked` / 密钥不算数。
  uvcpp_ws_frame f = frame;
  apply_mask(f);
  return send_frame_impl(f, std::move(cb));
}

int uvcpp_ws_connection::send_frame_impl(const uvcpp_ws_frame& frame, std::function<void(int)> cb) {
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

  // 正在一次读回调里逐帧派发（见 `on_tcp_data`）：先攒着，等整批派发完由
  // 守卫统一泵一次。其余场合（定时器、用户直接调用）立即泵，一个字都不变。
  if (!in_read_dispatch_) pump_send();
  return 0;
}

namespace {
/// 单次 `uv_write` 最多合并多少字节。
///
/// 合并的收益**不在省系统调用**，而在段的大小：一批 16 个 7 字节的回声帧逐帧
/// 写出去，就是一串十几字节的段，对端每收一个都要回一个 ACK。veth + 8 连接
/// ×16 深度这一格上量得出来的对照（`server_outsegs` 直接读
/// `/proc/net/snmp`，帧数取客户端的完成帧；三组各跑两轮取区间）：
///
/// | 发送形状 | 段/帧 | 服务端 µs/帧 | 帧/秒 |
/// |---|---|---|---|
/// | 逐帧 `uv_write` + NODELAY | 1.0000 | 3.8 ~ 4.5 | 524k ~ 859k |
/// | 逐帧 `uv_write` + Nagle | 0.62 ~ 0.81 | 3.0 ~ 3.8 | 817k ~ 1,240k |
/// | **本批合并 + 派发延后（本次）** | **0.0625** | **0.70 ~ 0.88** | **4,111k ~ 5,215k** |
///
/// 中间那一行是 v1.6.1 的行为：段数是**内核替我们**压下来的（Nagle 攒到 ACK
/// 才发），代价是延迟不可控，而且只压得到 0.7 左右；`TCP_NODELAY` 一开就回到
/// 1.0000，段数立刻由本端的调用形状决定。所以合并不能指望内核，要在这里做。
/// 底下一行是"一次读回调发一段"：16 帧进来是一段 112 字节出去，段数正好是
/// 1/16 —— 比 Nagle 还低一个数量级，且没有内核那点延迟代价。
///
/// 上限只是"这一批装多少"，装不下的帧留在队列里、写完这批接着泵，所以帧序与
/// 逐个写完全一致。64 KiB 取在 `UVCPP_TRY_WRITE_MIN_BYTES`(32 KiB) 之上，
/// 大帧批量还能顺带吃到 `uv_try_write` 那条快路径；小帧批量（ws 的常见形状）
/// 远够不到这个上限。
UVCPP_CONSTEXPR size_t kSendBatchMaxBytes = 64 * 1024;
}  // namespace

void uvcpp_ws_connection::pump_send() {
  if (write_in_flight_ || send_q_.empty() || !tcp_) return;

  // 把队列里装得下的帧拼成一份连续字节，一次交出去。
  //
  // tcp_->write() 会先把数据**复制**进自己的 uvcpp_buf（uvcpp_buf.cpp 的
  // `uvcpp_buf(const char*, size_t)` 是 memcpy，且 out_uv_buf() 转移所有权），
  // 所以 `send_batch_` 在 write() 返回之后就可以随便重用 —— 队列同时也可以改。
  send_batch_.clear();
  size_t n = 0;
  for (const pending_send& ps : send_q_) {
    // 至少收一帧：单帧就超过上限时也得发得出去，否则队列永远卡住。
    if (n > 0 && send_batch_.size() + ps.bytes.size() > kSendBatchMaxBytes) break;
    send_batch_ += ps.bytes;
    ++n;
  }
  if (n == 0) return;                // 不可能：队首在空缓冲上不会被截断

  in_flight_frames_ = n;
  write_in_flight_ = true;

  // 捕获存活令牌（C++11 没有 init-capture，weak_ptr 具名捕获即可）。这个闭包
  // 由 TCP 客户端持有，可能**活得比会话久** —— 令牌失效就什么都不碰。
  std::weak_ptr<char> tok = alive_token_;
  int rc = tcp_->write(send_batch_.data(), send_batch_.size(), [this, tok](int st) {
    if (tok.expired()) return;   // 会话已经终结/回收，别碰任何成员
    on_send_complete(st);
  });

  if (rc != 0) {
    // 没提交成功。`uvcpp_tcp_client::write()` 在返回非 0 时**没有保存回调**，
    // 完成回调不会来，所以这一批必须在这里自己结算，否则队列永远卡住。
    // UV_EALREADY 意味着有别的代码在直接写这条连接；本层是升级后唯一的写方，
    // 走到这里说明假设被打破了 —— 如实报告错误，绝不静默丢帧。
    //
    // 结算整批（而不是只结算队首）：交出去的是 n 帧的字节，n 帧一起失败。
    // 标志按"结算期间仍算在途"处理，与完成回调那条路同一套次序（下面
    // on_send_complete 有说明）。
    send_error_ = rc;
    size_t settled = in_flight_frames_;
    in_flight_frames_ = 0;
    settle_frames(settled, rc);
    write_in_flight_ = false;
    fail_pending_sends(rc);
  }
}

void uvcpp_ws_connection::on_send_complete(int status) {
  if (!write_in_flight_) return;     // 不该发生（没有在途写就不会有完成回调）

  if (status != 0) send_error_ = status;

  size_t n = in_flight_frames_;
  in_flight_frames_ = 0;

  // **结算期间 `write_in_flight_` 必须仍然是 true**：回调可能重入
  // `send_frame_impl()`（`send_close` 的回调会调 `tcp_->close()`，别的调用方也会
  // 接着发下一帧），而 `send_frame` 末尾就是 `pump_send()`。标志若先清了，
  // 新入队的帧会被这里紧跟着的一次 `uv_write` **抢走**——同一批在途写有两个
  // 完成回调，队列被结算两次。
  //
  // 先出队再回调同理：回调里看到的是"这批已经发完了"的状态，而不是"自己还在
  // 队列里"。回调之后才继续泵 —— 连接已经关了的话 pump 里的 write 会返回
  // UV_ENOTCONN，正好走上面的失败结算。
  settle_frames(n, status);
  write_in_flight_ = false;

  if (send_error_ != 0) { fail_pending_sends(send_error_); return; }
  pump_send();
}

void uvcpp_ws_connection::settle_frames(size_t count, int status) {
  for (size_t i = 0; i < count && !send_q_.empty(); ++i) {
    pending_send ps = std::move(send_q_.front());
    send_q_.pop_front();
    if (ps.cb) ps.cb(status);
  }
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
  apply_mask(f);
  return send_frame_impl(f, cb);
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

void uvcpp_ws_connection::apply_mask(uvcpp_ws_frame& f) const {
  if (is_server()) {
    // 服务端发的帧**必须不掩码**（§5.1）。显式清掉而不是不管：帧是调用方
    // 造的，万一哪天有谁填了 masked，服务端发出去就成了协议错误。
    f.masked = false;
    return;
  }
  f.masked = true;
  // 掩码密钥要"来自强熵源"（§5.3）—— 它防的是中间代理按**可预测**的字节模式
  // 缓存投毒，用 rand()/时间戳这类源等于把这条防护让掉。
  //
  // 实现取「线程本地的 mt19937_64，种子来自 random_device」：一次种子来自强熵源，
  // 之后由 PRNG 展开。**这不是密码学强度**，但对"不可预测"这个要求是够的
  // （每帧的密钥不会被同一连接内的前序字节推出来），而且避免了在每帧的发送路径上
  // 做 4 次系统调用 —— 逐帧调 random_device 在 Linux 上就是每帧 4 次读 /dev/urandom。
  static UVCPP_THREAD_LOCAL std::mt19937_64* rngp = nullptr;  if (!rngp) rngp = new std::mt19937_64(std::random_device{}());  // v120: TLS 只收标量
  const uint64_t r = (*rngp)();
  for (int i = 0; i < 4; ++i) {
    f.mask_key[i] = static_cast<uint8_t>((r >> (i * 8)) & 0xFF);
  }
}

int uvcpp_ws_connection::send_ping(const char* d, size_t n) {
  uvcpp_ws_frame f; f.opcode = ws_opcode::PING;
  if (d && n > 0) f.payload.clone_data(d, n);
  apply_mask(f);
  return send_frame_impl(f, nullptr);
}

int uvcpp_ws_connection::send_pong(const char* d, size_t n) {
  uvcpp_ws_frame f; f.opcode = ws_opcode::PONG;
  if (d && n > 0) f.payload.clone_data(d, n);
  apply_mask(f);
  return send_frame_impl(f, nullptr);
}

int uvcpp_ws_connection::send_close(ws_close_code cd, const std::string& rs) {
  uvcpp_ws_frame f; f.opcode = ws_opcode::CLOSE;
  // 控制帧负载上限 125 字节（RFC 6455 §5.5）；扣掉 2 字节状态码，原因最多 123。
  // 超了会拼出一个对端**必须拒绝**的帧，所以在这里截断，而不是让它发出去。
  f.set_close_payload(cd, rs.size() > 123 ? rs.substr(0, 123) : rs);
  apply_mask(f);
  return send_frame_impl(f, [this](int) {
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

void uvcpp_ws_connection::on_frame(std::function<void(const uvcpp_ws_frame&)> cb) {
  on_frame_ = std::move(cb);
}

void uvcpp_ws_connection::set_max_message_size(size_t n) {
  max_message_size_ = n;
  parser_.set_max_frame_size(n);     // 0 原样传下去 = 不限
}
size_t uvcpp_ws_connection::get_max_message_size() const { return max_message_size_; }

uvcpp_tcp_client* uvcpp_ws_connection::get_tcp_client() { return tcp_; }

}  // namespace uvcpp
#endif
