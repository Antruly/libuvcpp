/**
 * @file src/http3/uvcpp_h3_connection.cpp
 * @brief h3 连接的驱动实现：三条关键流、写序列化、按方向的流收场记账。
 * @author zhuweiye
 * @version 1.4.1
 */

#include "http3/uvcpp_h3_connection.h"

#if UVCPP_HTTP3_ENABLE

#include <utility>

#include "http3/uvcpp_h3_nghttp3.h"
#include "http3/uvcpp_h3_session.h"
#include "net/uvcpp_net_read.h"
#include "quic/uvcpp_quic_connection.h"

namespace uvcpp {

namespace {

/// 拼一个伪头。四个 `:xxx` 的构造只差这三个值，抽出来免得四段抄写里错一格。
h3_header make_pseudo(const char* name, const std::string& value,
                      h3_header_kind kind) {
  h3_header h;
  h.name  = name;
  h.value = value;
  h.kind  = kind;
  return h;
}

/// `:status` 的值 → 整数。**只认十进制正整数**：h3 的 `:status` 是一个三位
/// 数字串，别的形状（空、带符号、带尾随字符）一律当 0（"没拿到"）—— 悄悄按
/// `atoi` 的脾气把 `"20 0"` 读成 20 会让调用方拿一个从没被对端发过的状态码。
int parse_status(const std::string& v) {
  if (v.empty() || v.size() > 3) return 0;
  int n = 0;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] < '0' || v[i] > '9') return 0;
    n = n * 10 + (v[i] - '0');
  }
  return n;
}

}  // namespace

// =========================================================================
// 生 / 死
// =========================================================================

uvcpp_h3_connection::uvcpp_h3_connection(uvcpp_quic_connection* conn,
                                         bool server_side)
    : conn_(conn),
      server_side_(server_side),
      session_(new uvcpp_h3_session(server_side)) {}

uvcpp_h3_connection::~uvcpp_h3_connection() {
  if (alive_token_) *alive_token_ = 1;  // 先置位，再拆成员
  // **不动 QUIC 的回调集合**（`conn_->set_callbacks(...)` 这一句不要加）。
  // 看着像"拆干净"，实则是本文件最容易造出来的一个 use-after-free：本对象
  // 完全可能在某个 QUIC 回调里被析构（`on_read` → 用户代码 → 销毁本对象），
  // 那样 `set_callbacks()` 赋值的就是**此刻正在执行的那个 `std::function`
  // 对象**，等于在它自己的栈帧里把调用体销毁掉。
  //
  // 存活令牌已经把这条路封死了：那个 lambda 捕了一份 `life`，本析构一置位，
  // 它之后每一次触发都在碰 `this` 之前先返回。
}

std::shared_ptr<char> uvcpp_h3_connection::alive_token() {
  if (!alive_token_) alive_token_.reset(new char(0));
  return alive_token_;
}

bool uvcpp_h3_connection::token_alive(const std::shared_ptr<char>& token) {
  return token && *token == 0;
}

std::string uvcpp_h3_connection::nghttp3_version() {
  return std::string(h3_detail::version_literal());
}

// =========================================================================
// 起
// =========================================================================

int uvcpp_h3_connection::start(const callbacks& cbs) {
  if (conn_ == nullptr) return UV_EINVAL;
  if (started_) return UV_EINVAL;
  started_ = true;
  cbs_     = cbs;

  wire_session_callbacks();

  // **回调要在这里装。** 服务端的 `connection_cb` 是"连接刚建出来"那一刻、
  // 客户端的 `connect()` 返回之后也还没转起循环 —— 两处都在 ALPN 之前，而
  // 握手期间的事件（`on_streams_available` 之类）只在这段窗口之后才可能来。
  const std::shared_ptr<char> life = alive_token();
  uvcpp_quic_connection::callbacks qc;

  qc.on_alpn = [this, life](uvcpp_quic_connection& c, const std::string& alpn) {
    if (!token_alive(life)) return;
    on_alpn(c, alpn);
  };
  qc.on_read = [this, life](uvcpp_quic_connection& c, int64_t stream_id,
                            const net_read_result& r) {
    if (!token_alive(life)) return;
    on_read(c, stream_id, r);
  };
  qc.on_write = [this, life](uvcpp_quic_connection& c, int64_t stream_id,
                             int status) {
    if (!token_alive(life)) return;
    on_quic_write(c, stream_id, status);
  };
  qc.on_close = [this, life](uvcpp_quic_connection& c, int error_code) {
    if (!token_alive(life)) return;
    on_quic_close(c, error_code);
  };
  qc.on_streams_available = [this, life](uvcpp_quic_connection& c, bool bidi,
                                         uint64_t max_streams) {
    if (!token_alive(life)) return;
    on_streams_available(c, bidi, max_streams);
  };
  qc.on_stop_sending = [this, life](uvcpp_quic_connection& c, int64_t stream_id,
                                    uint64_t app_error_code) {
    if (!token_alive(life)) return;
    on_stop_sending(c, stream_id, app_error_code);
  };
  conn_->set_callbacks(qc);

  // 兜底：已经协商完 ALPN 的（理论上不会走到 —— 两个调用点都在握手之前）
  // 就别干等着那个回调了。
  const std::string alpn = conn_->alpn_selected();
  if (!alpn.empty()) on_alpn(*conn_, alpn);

  return 0;
}

void uvcpp_h3_connection::wire_session_callbacks() {
  const std::shared_ptr<char> life = alive_token();
  uvcpp_h3_session::callbacks sc;

  sc.on_begin_headers = [this, life](uvcpp_h3_session&, int64_t stream_id) {
    if (!token_alive(life)) return;
    // 建一条流的账（第一次见到它时）。**在头块开始的这一刻建**而不是等第一个
    // 头字段：一个空头块（对端什么都没发）也得有条记录才说得上"收场"。
    stream_of(stream_id);
  };

  sc.on_header = [this, life](uvcpp_h3_session&, int64_t stream_id,
                              const h3_header& field) {
    if (!token_alive(life)) return;
    stream_rec& s = stream_of(stream_id);
    // 第二个头块就是 trailers —— 本批不做（见 doc/http3-guide.md 的"没做的"
    // 那张表），丢掉而不是往请求里接着塞：塞进去会得到一个"头和体混在一起"
    // 的请求，那比丢掉更难查。`headers_done` 正是"已经收过一个头块"。
    if (s.headers_done) return;

    if (server_side_) {
      switch (field.kind) {
        case h3_header_kind::METHOD:
          s.req.method = field.value;
          return;
        case h3_header_kind::SCHEME:
          s.req.scheme = field.value;
          return;
        case h3_header_kind::PATH:
          s.req.path = field.value;
          return;
        case h3_header_kind::AUTHORITY:
          s.req.authority = field.value;
          return;
        default:
          // `:status` 出现在请求里是协议错（nghttp3 自己会拦），落到这里说明
          // 上游放行了 —— 当普通头处理，不另造分支。
          s.req.headers.push_back(field);
          return;
      }
    }

    if (field.kind == h3_header_kind::STATUS) {
      s.resp.status = parse_status(field.value);
      return;
    }
    s.resp.headers.push_back(field);
  };

  sc.on_end_headers = [this, life](uvcpp_h3_session&, int64_t stream_id,
                                   bool fin) {
    if (!token_alive(life)) return;
    stream_rec& s = stream_of(stream_id);
    s.headers_done = true;
    // 头块带 FIN = 这条流没有 body。服务端这边请求已经全了，可以交出去了；
    // 客户端那边**不能**在这里收工 —— 头块结束不代表响应结束的判据要靠
    // `on_end_stream`，两条路都留着，靠 `dispatched` / `completed` 各挡一次。
    if (server_side_ && fin) dispatch_request(stream_id);
  };

  sc.on_body = [this, life](uvcpp_h3_session&, int64_t stream_id,
                            const char* data, size_t len) {
    if (!token_alive(life)) return;
    stream_rec& s = stream_of(stream_id);
    std::string& body = server_side_ ? s.req.body : s.resp.body;
    if (s.overflowed) return;
    if (body.size() + len > H3_DEFAULT_MAX_BODY_BYTES) {
      // **上限的兑现方式是当场收场，不是"攒不完就默默继续攒"。** 一旦这里
      // 放行，`body` 就成了对端任意长的内存 —— 而"上限"这个词就没意义了。
      s.overflowed = true;
      body.clear();
      body.shrink_to_fit();
      // 读方向掐掉："这条流我不要了"。nghttp3 据此会要求我们发 STOP_SENDING
      // （`on_stop_sending` 那条会话回调），线上那个帧由 QUIC 层发。
      if (session_) session_->shutdown_stream_read(stream_id);
      return;
    }
    body.append(data, len);
  };

  sc.on_end_stream = [this, life](uvcpp_h3_session&, int64_t stream_id) {
    if (!token_alive(life)) return;
    stream_rec& s = stream_of(stream_id);
    s.rx_done = true;
    if (server_side_) {
      dispatch_request(stream_id);
    } else if (s.local && s.request) {
      // 响应收全了。**这里是"收全"的唯一判据** —— `on_end_headers` 的 `fin`
      // 说的是"头块之后没有 body"，而一条带 body 的响应要靠这一格。
      complete_response(s, 0);
    }
    maybe_close_stream(stream_id);
  };

  sc.on_stream_close = [this, life](uvcpp_h3_session&,
                                    const h3_stream_close_info& info) {
    if (!token_alive(life)) return;
    std::map<int64_t, stream_rec>::iterator it =
        streams_.find(info.stream_id);
    if (it != streams_.end()) {
      stream_rec& s = it->second;
      if (!server_side_ && s.local && s.request && !s.completed) {
        // 走到这里说明读侧没干净收场（reset / STOP_SENDING / 连接断的一半），
        // 但**还是要给一次**：一条请求的完成队列里恰好一条，是 `take_completed`
        // 那条契约的全部内容。
        complete_response(s, UV_ECANCELED);
      }
    }

    // 用户回调**先拷一份再调**：它里面很可能就把本对象销毁了，而成员里那个
    // `std::function` 正是此刻在执行的调用体本身。
    std::function<void(uvcpp_h3_connection&, const h3_stream_close_info&)> cb;
    if (cbs_.on_stream_close) cb = cbs_.on_stream_close;
    if (cb) {
      const h3_stream_close_info copy = info;
      cb(*this, copy);
    }
    if (!token_alive(life)) return;

    // 记账到此为止。**在用户回调之后才擦**：它完全可能去读 `streams_` 里
    // 那条流的账（比如拿 `stream_state()` 对一下）。
    streams_.erase(info.stream_id);
    pending_.erase(info.stream_id);
  };

  sc.on_stop_sending = [this, life](uvcpp_h3_session&, int64_t stream_id,
                                    uint64_t app_error_code) {
    if (!token_alive(life)) return;
    // nghttp3 说"给对端发 STOP_SENDING"。**本层不碰 QUIC 的收/发，只传话** ——
    // 真正把那个帧排上的是 QUIC 层，而它是唯一能做的事。
    if (conn_) conn_->shutdown_stream_read(stream_id, app_error_code);
  };

  sc.on_reset_stream = [this, life](uvcpp_h3_session&, int64_t stream_id,
                                    uint64_t app_error_code) {
    if (!token_alive(life)) return;
    if (conn_) conn_->shutdown_stream(stream_id, app_error_code);
  };

  sc.on_acked = [](uvcpp_h3_session&, int64_t, uint64_t) {};

  sc.on_header_budget_exceeded = [this, life](uvcpp_h3_session&,
                                              int64_t stream_id, size_t) {
    if (!token_alive(life)) return;
    // 预算超了：这条请求**当成不存在**。会话层已经不再往上交了，这里做的是
    // 把读方向掐掉 —— 于是 `on_stop_sending` 会带着 `H3_EXCESSIVE_LOAD` 来，
    // 线上真的发出那个帧。
    std::map<int64_t, stream_rec>::iterator it = streams_.find(stream_id);
    if (it != streams_.end()) {
      it->second.overflowed = true;
      it->second.req.body.clear();
      it->second.req.headers.clear();
    }
    if (session_) session_->shutdown_stream_read(stream_id);
  };

  sc.on_fatal = [this, life](uvcpp_h3_session&, int nghttp3_error) {
    (void)nghttp3_error;
    if (!token_alive(life)) return;
    fatal_ = true;
    // 要发给对端的是 nghttp3 自己翻译出来的那个 QUIC 应用错误码 —— 不是原始
    // 的 nghttp3 负值（那个数是本库的私有词汇，对端看不懂）。
    const int code = static_cast<int>(session_->quic_app_error_code());
    std::function<void(uvcpp_h3_connection&, int)> cb;
    if (cbs_.on_error) cb = cbs_.on_error;
    if (cb) cb(*this, code);
    if (!token_alive(life)) return;
    if (conn_) conn_->close(code);
  };

  if (session_) {
    session_->init(sc, H3_DEFAULT_MAX_FIELD_SECTION_SIZE,
                   H3_DEFAULT_QPACK_MAX_DTABLE_CAPACITY,
                   H3_DEFAULT_QPACK_BLOCKED_STREAMS);
  }
}

// =========================================================================
// QUIC 回调
// =========================================================================

void uvcpp_h3_connection::on_alpn(uvcpp_quic_connection& c,
                                  const std::string& alpn) {
  (void)c;
  if (closed_ || !session_) return;
  if (ready_) return;
  alpn_ = alpn;
  try_open_critical_streams();
  if (!ready_) return;  // 额度还没到 —— 等 `on_streams_available` 那一格。
  // 顺序有讲究：控制流必须先绑（SETTINGS 在它上面），而"我们容许对端开多少条
  // 双向流"要在收到第一个请求之前告诉 nghttp3。
  if (server_side_) session_->set_max_client_streams_bidi(conn_->streams_left(true));
  flush();
}

void uvcpp_h3_connection::try_open_critical_streams() {
  if (ready_ || conn_ == nullptr || session_ == nullptr) return;

  while (critical_left_ > 0) {
    // 先问额度再开。**不拿 `open_stream()` 的 `STREAM_ID_BLOCKED` 当控制流**：
    // 那个负值要判它是不是"额度还没到"，就得把 ngtcp2 的私有错误码拉进本文件，
    // 而 h3 这一层刻意不认识 ngtcp2。`streams_left()` 是公开面上同一个数的
    // 只读版本，用它表达"还没到"不需要认识任何私有词汇。
    if (conn_->streams_left(false) == 0) return;

    const int64_t sid = conn_->open_stream(false);
    if (sid < 0) {
      // 到这里就是真错了（上面那一句已经排除了"额度没到"那一种）。报出去，
      // 不让连接静悄悄地卡在"三条关键流永远开不出来"上。
      fatal_ = true;
      std::function<void(uvcpp_h3_connection&, int)> cb;
      if (cbs_.on_error) cb = cbs_.on_error;
      if (cb) cb(*this, static_cast<int>(sid));
      conn_->close(static_cast<int>(H3_INTERNAL_ERROR));
      return;
    }

    stream_rec rec;
    rec.local   = true;
    rec.request = false;  // 关键流是单向的，不是请求流
    streams_.insert(std::make_pair(sid, rec));

    if (ctl_sid_ < 0) {
      ctl_sid_ = sid;
    } else if (qenc_sid_ < 0) {
      qenc_sid_ = sid;
    } else {
      qdec_sid_ = sid;
    }
    --critical_left_;
  }

  int rc = session_->bind_control_stream(ctl_sid_);
  if (rc == 0) rc = session_->bind_qpack_streams(qenc_sid_, qdec_sid_);
  if (rc != 0) {
    fatal_ = true;
    std::function<void(uvcpp_h3_connection&, int)> cb;
    if (cbs_.on_error) cb = cbs_.on_error;
    if (cb) cb(*this, rc);
    conn_->close(static_cast<int>(H3_INTERNAL_ERROR));
    return;
  }
  ready_ = true;
}

void uvcpp_h3_connection::on_read(uvcpp_quic_connection& c, int64_t stream_id,
                                  const net_read_result& r) {
  (void)c;
  if (closed_ || fatal_) return;
  const std::shared_ptr<char>             life = alive_token();
  const std::shared_ptr<uvcpp_h3_session> self = session_;
  uvcpp_quic_connection* const            qc   = conn_;
  if (!self || qc == nullptr) return;
  if (!ready_) return;  // 关键流还没绑，协议还没开始跑

  if (r.event == net_read_event::DATA) {
    bytes_in_ += r.size;
    stream_rec& s = stream_of(stream_id);
    if (r.fin) s.fin_delivered = true;
    const int64_t n = self->recv_stream_data(
        stream_id, r.data, r.size, r.fin, 0);
    if (!token_alive(life)) return;
    if (n < 0) return;  // 致命，`on_fatal` 已经报过
    pump_closes();
    if (!token_alive(life)) return;
    flush();
    return;
  }

  if (r.event == net_read_event::PEER_CLOSED && r.fin) {
    stream_rec& s = stream_of(stream_id);
    // 读侧的最后一个通知到了 —— 到这一刻为止，这条流**不会**再有读事件。
    // 关闭（会把它从 nghttp3 里删掉）因此才安全，见 `stream_rec::rx_settled`。
    s.rx_settled = true;
    if (!s.fin_delivered) {
      // 一个字节数据都没带的 FIN（对端把 FIN 单独发了一个 STREAM 帧）。
      // QUIC 层那条路报两次：先 DATA 再 PEER_CLOSED —— 那次 DATA 只在有字节时
      // 才来，所以这里可能见到一个**没被那一次带过 FIN** 的 PEER_CLOSED。
      // 这一位 `fin_delivered` 就是"别把同一个 FIN 交两遍"。
      s.fin_delivered = true;
      const int64_t n = self->recv_stream_data(stream_id, nullptr, 0, true, 0);
      if (!token_alive(life)) return;
      if (n < 0) return;
    }
    // **这一句是承重的，不能省。** `rx_settled` 刚刚才置上，而 `rx_done` 早在
    // 上一条通知（`DATA && fin`）里就由 `on_end_stream` 置好了 —— 那一次
    // `maybe_close_stream()` 看到的 `rx_settled` 还是假，于是**空手而回**，
    // 之后不会再有人替它排队。少了这里，只有"END_STREAM 恰好由这一个零长度
    // FIN 带进来"的流会关掉，其余全留在账上。
    maybe_close_stream(stream_id);
    pump_closes();
    if (!token_alive(life)) return;
    flush();
    return;
  }

  // 剩下的两种：`PEER_CLOSED && !fin`（对端发了 RESET，应用错误码 0）与
  // `READ_ERROR`。语义是同一个 —— **读方向到此为止，但不是干净收尾**。
  stream_rec& s = stream_of(stream_id);
  s.rx_settled = true;  // 同上：RESET / 读错误本身就是读侧的最后一个通知
  if (!s.rx_done) {
    s.rx_done  = true;
    s.rx_error = true;
    s.rx_code  = static_cast<uint64_t>(r.error < 0 ? 0 : r.error);
    // 告诉 nghttp3 读侧没了：待收的数据全部作废。**这不是"关掉这条流"** ——
    // 写方向照走（RFC 9114 §4.1 明写两个方向独立）。
    self->shutdown_stream_read(stream_id);
    if (!token_alive(life)) return;
  }
  maybe_close_stream(stream_id);
  pump_closes();
  if (!token_alive(life)) return;
  flush();
}

void uvcpp_h3_connection::on_quic_write(uvcpp_quic_connection& c,
                                        int64_t stream_id, int status) {
  (void)c;
  if (closed_) return;
  const std::shared_ptr<char>             life = alive_token();
  const std::shared_ptr<uvcpp_h3_session> self = session_;
  if (!self) return;

  std::map<int64_t, std::deque<pending_write>>::iterator it =
      pending_.find(stream_id);
  if (it == pending_.end() || it->second.empty()) {
    // **契约被破坏的那一格。** 按 `uvcpp_quic_connection::on_write` 的说明，一次
    // `write_stream()` 对应一次本回调；唯一的例外是"流被 `shutdown_stream()`
    // 丢掉时队列条目被 `do_flush()` 顺手删掉"那条路（`send_q_.erase`），那时
    // 剩下的完成通知不会再来。既然对不上账，**宁可漏记一笔，也不要把一笔字节
    // 记到另一笔头上** —— 后者会让 QPACK 的记账错位，而那是静默的。
    return;
  }
  const pending_write w = it->second.front();
  it->second.pop_front();

  if (status == 0) {
    self->add_ack_offset(stream_id, w.bytes);
    if (!token_alive(life)) return;
    if (w.fin) {
      // 带 FIN 的那一笔被确认 = 这条流的**写方向**收场了。
      std::map<int64_t, stream_rec>::iterator sit = streams_.find(stream_id);
      if (sit != streams_.end()) sit->second.tx_done = true;
    }
  } else {
    std::map<int64_t, stream_rec>::iterator sit = streams_.find(stream_id);
    if (sit != streams_.end()) {
      sit->second.tx_done  = true;
      sit->second.tx_error = true;
      sit->second.tx_code  = static_cast<uint64_t>(status < 0 ? 0 : status);
    }
    // 这些字节永远不会到对端了。告诉 nghttp3 写方向到此为止 ——
    // `nghttp3_conn_shutdown_stream_write()` 正是那个意思。
    self->shutdown_stream_write(stream_id);
    if (!token_alive(life)) return;
  }

  maybe_close_stream(stream_id);
  pump_closes();
}

void uvcpp_h3_connection::on_streams_available(uvcpp_quic_connection& c,
                                               bool bidi,
                                               uint64_t max_streams) {
  (void)c;
  if (closed_ || !session_) return;

  if (!ready_) {
    // 关键流正等着额度 —— 这条回调存在的理由就是这一格。
    try_open_critical_streams();
    if (!ready_) return;
    if (server_side_) {
      session_->set_max_client_streams_bidi(conn_->streams_left(true));
    }
    flush();
    return;
  }

  if (server_side_ && bidi) {
    // 对端放开了双向流额度 —— 服务端拿它当 nghttp3 那边的并发上界。
    // `max_streams` 是**累计**值（与 ngtcp2 的语义同），所以直接盖掉旧值是对的。
    session_->set_max_client_streams_bidi(max_streams);
  }
}

void uvcpp_h3_connection::on_stop_sending(uvcpp_quic_connection& c,
                                          int64_t stream_id,
                                          uint64_t app_error_code) {
  (void)c;
  if (closed_ || !session_) return;
  // 对端说"这条流你别再发了"。**线上回应的 RESET_STREAM 由 ngtcp2 自己发掉**
  // （见 `uvcpp_quic_connection::callbacks::on_stop_sending` 的说明），这里要
  // 做的是把应用侧这条流的发送方向也收掉，否则 nghttp3 会一直等着写它。
  session_->shutdown_stream_write(stream_id);
  std::map<int64_t, stream_rec>::iterator sit = streams_.find(stream_id);
  if (sit != streams_.end()) {
    sit->second.tx_done  = true;
    sit->second.tx_error = true;
    sit->second.tx_code  = app_error_code;
  }
  maybe_close_stream(stream_id);
  pump_closes();
}

void uvcpp_h3_connection::on_quic_close(uvcpp_quic_connection& c,
                                        int error_code) {
  (void)c;
  if (closed_) return;
  closed_ = true;

  const std::shared_ptr<char> life = alive_token();

  // 还没回来的请求全部以失败收场。**这一句是"不挂死"的全部内容**：连接没了，
  // 那些响应永远不会到，而调用方拿不到它们就没法把那批请求结掉。
  if (!server_side_) {
    for (std::map<int64_t, stream_rec>::iterator it = streams_.begin();
         it != streams_.end(); ++it) {
      stream_rec& s = it->second;
      if (s.local && s.request && !s.completed) {
        complete_response(s, static_cast<int>(UV_ECANCELED));
      }
    }
  }

  // 先拷一份再调：用户在 `on_disconnect` 里迟早会销毁本对象，而成员里那个
  // `std::function` 正是此刻在跑的调用体本身。
  std::function<void(uvcpp_h3_connection&)> cb;
  if (cbs_.on_disconnect) cb = cbs_.on_disconnect;
  if (cb) cb(*this);
  (void)life;
}

// =========================================================================
// 账
// =========================================================================

uvcpp_h3_connection::stream_rec& uvcpp_h3_connection::stream_of(
    int64_t stream_id) {
  std::map<int64_t, stream_rec>::iterator it = streams_.find(stream_id);
  if (it != streams_.end()) return it->second;

  stream_rec rec;
  // 走到这里说明这条流不是本端开的（本端开的都在 `send_request()` 与
  // `try_open_critical_streams()` 里先建了记录）。对端发起的**双向**流在 h3 里
  // 只有一种：一条请求流。
  rec.local        = false;
  rec.request      = is_bidi(stream_id);
  rec.req.stream_id  = stream_id;
  rec.resp.stream_id = stream_id;
  std::pair<std::map<int64_t, stream_rec>::iterator, bool> r =
      streams_.insert(std::make_pair(stream_id, rec));
  return r.first->second;
}

void uvcpp_h3_connection::dispatch_request(int64_t stream_id) {
  std::map<int64_t, stream_rec>::iterator it = streams_.find(stream_id);
  if (it == streams_.end()) return;
  stream_rec& s = it->second;
  if (s.dispatched || !s.request || s.local) return;
  if (s.overflowed) {
    // 超了预算的那条请求**当成不存在**：不交出去，但流照样要收场。
    s.dispatched = true;
    return;
  }
  s.dispatched = true;

  std::function<void(uvcpp_h3_connection&, const h3_request&)> cb;
  if (cbs_.on_request) cb = cbs_.on_request;
  if (!cb) return;

  const std::shared_ptr<char> life = alive_token();
  // 拷一份交出去：用户回调里完全可能把本对象销毁，而 `streams_` 是成员。
  const h3_request req = s.req;
  cb(*this, req);
  (void)life;
}

void uvcpp_h3_connection::complete_response(stream_rec& s, int error) {
  if (s.completed) return;
  s.completed = true;
  h3_response out = s.resp;
  if (error != 0) {
    // 起头就被掐断的请求可能一个头字段都没收到，那时 `status` 是 0（"没拿到"）。
    // 如实留 0 —— 编一个 5xx 出来就是替对端说话。
    out.body.clear();
  }
  out.error = error;
  completed_.push_back(std::move(out));
}

void uvcpp_h3_connection::maybe_close_stream(int64_t stream_id) {
  std::map<int64_t, stream_rec>::iterator it = streams_.find(stream_id);
  if (it == streams_.end()) return;
  const stream_rec& s = it->second;
  if (!s.request) return;
  if (!s.rx_done || !s.tx_done || !s.rx_settled) {
    return;
  }
  for (size_t i = 0; i < close_pending_.size(); ++i) {
    if (close_pending_[i] == stream_id) return;  // 已经排上了
  }
  close_pending_.push_back(stream_id);
}

void uvcpp_h3_connection::pump_closes() {
  if (session_ == nullptr) return;
  // **只在 nghttp3 的栈外兑现。** 这个判断不是保险，是承重的：读方向那个入口
  // 就在 `cb_end_stream` 里面，在那里调 `close_stream()` 会把正在被处理的流
  // 删掉 —— 上游文档对"在回调里调它自己的 API"给的用词是 undefined behavior。
  if (session_->in_nghttp3()) return;

  const std::shared_ptr<char>             life = alive_token();
  const std::shared_ptr<uvcpp_h3_session> self = session_;

  while (!close_pending_.empty()) {
    if (!token_alive(life)) return;
    const int64_t stream_id = close_pending_.front();
    close_pending_.pop_front();

    std::map<int64_t, stream_rec>::iterator it = streams_.find(stream_id);
    if (it == streams_.end()) continue;
    stream_rec& s = it->second;
    if (!s.rx_done || !s.tx_done || !s.rx_settled) {
      continue;
    }

    h3_stream_close_info info;
    info.stream_id         = stream_id;
    info.rx_error          = s.rx_error;
    info.tx_error          = s.tx_error;
    info.rx_app_error_code = s.rx_code;
    info.tx_app_error_code = s.tx_code;
    // **干净收尾表达成"不置位"，不是"错误码写 0"**：0 在 h3 里压根不是合法应用
    // 错误码（合法值从 0x0100 起），拿它当"没有"是在借一个不存在的编码。
    // `uvcpp_h3_session::close_stream()` 会把这两位翻译成 nghttp3 的那个 flag。
    //
    // 它会**同步**回到 `sc.on_stream_close`（那是本层自己回声报出来的收场，
    // 不是另一条独立信号），所以记录是在那一格里擦的。
    self->close_stream(info);
    if (!token_alive(life)) return;
  }
}

// =========================================================================
// 提问 / 答复
// =========================================================================

int64_t uvcpp_h3_connection::send_request(const h3_request& req) {
  if (conn_ == nullptr || session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  if (server_side_) return UV_EINVAL;  // 服务端不提请求（本批没有客户端推送）
  if (!ready_) return UV_EAGAIN;       // 关键流还没开出来，协议还没开始跑

  const int64_t sid = conn_->open_stream(true);
  if (sid < 0) return sid;  // `NGTCP2_ERR_STREAM_ID_BLOCKED` 如实透传

  stream_rec rec;
  rec.local   = true;
  rec.request = true;
  rec.req.stream_id  = sid;
  rec.resp.stream_id = sid;
  streams_.insert(std::make_pair(sid, rec));

  std::vector<h3_header> hs;
  hs.push_back(make_pseudo(":method",
                           req.method.empty() ? std::string("GET") : req.method,
                           h3_header_kind::METHOD));
  // `:scheme` 缺省成 `https` 不是"猜的"：h3 只跑在 QUIC 上，而 QUIC 没有明文
  // 那一档（ALPN 是 TLS 扩展），所以这一层唯一可能的取值就是它。
  hs.push_back(make_pseudo(":scheme",
                           req.scheme.empty() ? std::string("https")
                                              : req.scheme,
                           h3_header_kind::SCHEME));
  if (!req.authority.empty()) {
    hs.push_back(
        make_pseudo(":authority", req.authority, h3_header_kind::AUTHORITY));
  }
  hs.push_back(make_pseudo(
      ":path", req.path.empty() ? std::string("/") : req.path,
      h3_header_kind::PATH));
  for (size_t i = 0; i < req.headers.size(); ++i) hs.push_back(req.headers[i]);

  const int rc = session_->submit_request(sid, hs, req.body);
  if (rc != 0) {
    streams_.erase(sid);
    return rc;
  }
  flush();
  return sid;
}

bool uvcpp_h3_connection::take_completed(h3_response& out) {
  if (completed_.empty()) return false;
  out = std::move(completed_.front());
  completed_.pop_front();
  return true;
}

int uvcpp_h3_connection::send_response(const h3_response& resp,
                                       bool omit_body) {
  if (conn_ == nullptr || session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  if (resp.stream_id < 0) return UV_EINVAL;

  std::vector<h3_header> hs;
  hs.push_back(make_pseudo(":status", std::to_string(resp.status),
                           h3_header_kind::STATUS));
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    // 伪头由上面那一行负责。调用方往 `headers` 里塞了 `:xxx` 也**不拦** ——
    // 那是一个真错误，但它是"发出去之后对端以 H3_MESSAGE_ERROR 关掉这条流"，
    // 在这里拦掉只会让两个不同的错看起来像同一个。
    hs.push_back(resp.headers[i]);
  }

  const int rc =
      session_->submit_response(resp.stream_id, hs, resp.body, omit_body);
  if (rc != 0) return rc;
  flush();
  return 0;
}

int uvcpp_h3_connection::send_status(int64_t stream_id, int status,
                                     const std::string& body) {
  if (conn_ == nullptr || session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  const int rc = session_->submit_status(stream_id, status, body);
  if (rc != 0) return rc;
  flush();
  return 0;
}

int uvcpp_h3_connection::flush() {
  if (conn_ == nullptr || session_ == nullptr) return 0;
  if (closed_ || fatal_) return 0;
  if (!ready_) return 0;  // 关键流还没绑，nghttp3 那边没得发

  const std::shared_ptr<char>             life = alive_token();
  const std::shared_ptr<uvcpp_h3_session> self = session_;
  uvcpp_quic_connection* const            qc   = conn_;

  int         rc = 0;
  h3_out_chunk chunk;
  for (;;) {
    const int dr = self->drain(chunk);
    if (!token_alive(life)) return rc;
    if (dr < 0) {
      rc = dr;  // 致命，`on_fatal` 已经报过
      break;
    }
    if (dr == 0) break;

    const int64_t sid = chunk.stream_id;
    if (sid < 0) break;

    // 段指针指向 nghttp3 的内部缓冲，**只在本次 `drain()` 之后有效** —— 所以
    // 这一轮里必须把它们交出去。单段时直接把那一段交给 QUIC（少一次拷贝）：
    // `write_stream()` 本来就会拷一份进它自己的发送队列，多拼一次是白拼。
    const char* data = nullptr;
    size_t      len  = chunk.total;
    if (chunk.veccnt == 1) {
      data = reinterpret_cast<const char*>(chunk.base[0]);
      len  = chunk.len[0];
    } else if (chunk.veccnt > 1) {
      wbuf_.clear();
      wbuf_.reserve(chunk.total);
      for (size_t i = 0; i < chunk.veccnt; ++i) {
        wbuf_.insert(wbuf_.end(), chunk.base[i], chunk.base[i] + chunk.len[i]);
      }
      data = wbuf_.empty() ? nullptr : &wbuf_[0];
    }
    // `veccnt == 0 && fin` 走到这里：`data == nullptr, len == 0` —— 那就是
    // "只发一个 FIN"。**它是一次合法的待发**，一个字节不少。

    const int wr = qc->write_stream(sid, data, len, chunk.fin);
    if (!token_alive(life)) return rc;
    if (wr != 0) {
      // 这一笔没被受理 —— 一个字节都没交出去，所以**一个字都不能记**。
      // 记账必须在"真的交出去了"之后，反过来的话 FIFO 会从此整体错位一格。
      rc = wr;
      break;
    }
    pending_write pw;
    pw.bytes = len;
    pw.fin   = chunk.fin;
    pending_[sid].push_back(pw);
    bytes_out_ += len;

    // **无论多少字节都要调。** nghttp3 的文档把这条写死了："It is important to
    // call this function even if n is 0 in this case." —— 少了这一句，只有 FIN
    // 没有数据的那个块永远发不出去，而 nghttp3 那边看起来一切正常。
    const int ar = self->add_write_offset(sid, len);
    if (!token_alive(life)) return rc;
    if (ar < 0) {
      rc = ar;
      break;
    }
  }

  pump_closes();
  return rc;
}

int uvcpp_h3_connection::close(int error_code) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  return conn_->close(error_code);
}

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
