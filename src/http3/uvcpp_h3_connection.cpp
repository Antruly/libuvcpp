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
#include "http3/uvcpp_h3_wt_session.h"
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

// -------------------------------------------------------------------------
// QUIC 变长整数（RFC 9000 §16）—— 只给 HTTP/3 Datagram 的 Quarter Stream ID
// 用（RFC 9297 §2.1）。自己写 20 行而不是借 ngtcp2 的：那两个是内部头，
// 公开面（本文件不在公开头里，但依赖方向仍是 h3 → quic 的公开 API）。
// -------------------------------------------------------------------------

/// @p v 编成最短形式的变长整数，写进 @p out（最多 8 字节），返回字节数。
size_t encode_varint(uint64_t v, uint8_t* out) {
  if (v < 64u) {
    out[0] = static_cast<uint8_t>(v);
    return 1;
  }
  if (v < 16384u) {
    out[0] = static_cast<uint8_t>((v >> 8) | 0x40u);
    out[1] = static_cast<uint8_t>(v & 0xffu);
    return 2;
  }
  if (v < 1073741824u) {
    out[0] = static_cast<uint8_t>((v >> 24) | 0x80u);
    out[1] = static_cast<uint8_t>((v >> 16) & 0xffu);
    out[2] = static_cast<uint8_t>((v >> 8) & 0xffu);
    out[3] = static_cast<uint8_t>(v & 0xffu);
    return 4;
  }
  out[0] = static_cast<uint8_t>((v >> 56) | 0xc0u);
  for (int i = 1; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((v >> (8 * (7 - i))) & 0xffu);
  }
  return 8;
}

/// 解一个变长整数。@return false = 字节不够 / 形状非法。@p used 是消耗的字节数。
bool decode_varint(const uint8_t* p, size_t n, uint64_t& v, size_t& used) {
  if (n == 0) return false;
  const size_t len = static_cast<size_t>(1) << (p[0] >> 6);  // 1 / 2 / 4 / 8
  if (n < len) return false;
  uint64_t x = p[0] & 0x3fu;
  for (size_t i = 1; i < len; ++i) x = (x << 8) | p[i];
  v    = x;
  used = len;
  return true;
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

  // SETTINGS 在 `on_alpn` 里那次 `session_->init()` 才发出去，而 `on_alpn`
  // 可能就在下面的兜底那句里同步跑到 —— 所以这一位必须在它之前落。
  session_->set_h3_datagram_enabled(h3_datagram_enabled_);

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
  // HTTP/3 Datagram（RFC 9297）：QUIC DATAGRAM 的载荷由本层解出四分之一流号。
  qc.on_datagram = [this, life](uvcpp_quic_connection&,
                                const uint8_t* data, size_t datalen) {
    if (!token_alive(life)) return;
    on_datagram(data, datalen);
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
        case h3_header_kind::PROTOCOL:
          s.req.protocol = field.value;
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
    // extended CONNECT（#53）：请求头收全就交给应用 —— 这条流不会读侧收场，
    // 等 `on_end_stream` 等于永远等不到。`req.protocol` 非空即为此类。
    if (server_side_ && !s.req.protocol.empty() && !s.dispatched) {
      s.dispatched  = true;
      s.raw         = true;
      // extended CONNECT 是 RFC 9297 里**免掉 `Datagram-Flow-Id`** 的那一类
      // 请求：datagram 直接按 `stream_id / 4` 关联到这条流。
      s.datagram_ok = true;
      std::function<void(uvcpp_h3_connection&, const h3_request&)> cb;
      if (cbs_.on_connect_request) cb = cbs_.on_connect_request;
      if (cb) cb(*this, s.req);
      return;
    }
    // 头块带 FIN = 这条流没有 body。服务端这边请求已经全了，可以交出去了；
    // 客户端那边**不能**在这里收工 —— 头块结束不代表响应结束的判据要靠
    // `on_end_stream`，两条路都留着，靠 `dispatched` / `completed` 各挡一次。
    if (server_side_ && fin) dispatch_request(stream_id);
  };

  sc.on_begin_trailers = [this, life](uvcpp_h3_session&, int64_t stream_id) {
    if (!token_alive(life)) return;
    stream_of(stream_id);  // 确保账在；trailers 自己不需要新状态
  };

  sc.on_trailer = [this, life](uvcpp_h3_session&, int64_t stream_id,
                               const h3_header& field) {
    if (!token_alive(life)) return;
    stream_rec& s = stream_of(stream_id);
    // 与 `on_header` 分开：第二个头块只进 `trailers`，不混进首部。
    if (server_side_) {
      s.req.trailers.push_back(field);
    } else {
      s.resp.trailers.push_back(field);
    }
  };

  sc.on_end_trailers = [this, life](uvcpp_h3_session&, int64_t) {
    if (!token_alive(life)) return;
    // 不需要动作：trailers 之后紧接着就是 `on_end_stream` / 流的收场，
    // 交付时机由那里统一决定。
  };

  sc.on_body = [this, life](uvcpp_h3_session&, int64_t stream_id,
                            const char* data, size_t len) {
    if (!token_alive(life)) return;
    stream_rec& s = stream_of(stream_id);
    if (s.raw) {
      if (s.sink_data) s.sink_data(data, len);
      return;
    }
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
    if (s.raw) {
      if (s.sink_end) s.sink_end();
      if (!token_alive(life)) return;
      maybe_close_stream(stream_id);
      return;
    }
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

  sc.on_shutdown = [this, life](uvcpp_h3_session&, int64_t id) {
    if (!token_alive(life)) return;
    // `id` 只会不变或变小；notice 是 `NGHTTP3_SHUTDOWN_NOTICE_STREAM_ID`
    // （一个极大的数）—— 先到 notice、后到最终 GOAWAY 是正常顺序。
    peer_goaway_    = true;
    peer_goaway_id_ = id;
    std::function<void(uvcpp_h3_connection&, int64_t)> cb;
    if (cbs_.on_goaway) cb = cbs_.on_goaway;
    if (cb) cb(*this, id);
  };

  sc.on_remote_settings = [this, life](const uvcpp_h3_remote_settings& st) {
    if (!token_alive(life)) return;
    have_remote_settings_ = true;
    remote_settings_      = st;
    std::function<void(uvcpp_h3_connection&, const uvcpp_h3_remote_settings&)>
        cb;
    if (cbs_.on_remote_settings) cb = cbs_.on_remote_settings;
    if (cb) cb(*this, st);
  };

  sc.on_deferred_consume = [this, life](int64_t stream_id, size_t consumed) {
    if (!token_alive(life)) return;
    ++blocked_stream_events_;
    blocked_stream_bytes_ += consumed;
    std::function<void(uvcpp_h3_connection&, int64_t, size_t)> cb;
    if (cbs_.on_deferred_consume) cb = cbs_.on_deferred_consume;
    if (cb) cb(*this, stream_id, consumed);
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

    // WebTransport（draft-ietf-webtrans-http3-13）的第一步：**还没登记的对端流**
    // 要先看首个 varint 是不是 WT 的类型码（双向 `0x41` / 单向 `0x54`）。
    // 这一步必须发生在 `stream_of()` 与 nghttp3 之前 —— 一条 `0x41` 开头的流
    // 不是 HTTP 请求，喂给 nghttp3 就是 H3_FRAME_ERROR，整条连接报错。
    const int pr = route_new_stream(stream_id, r.data, r.size, r.fin);
    if (!token_alive(life)) return;
    if (pr < 0) return;   // 协议错，已经 reset 了那条流
    if (pr == 2) return;  // 字节还不够，已缓冲（等下一段）
    if (pr == 1) {
      // 已经处理完（WT 流，或"普通 HTTP 流且缓冲已喂给 nghttp3"）。
      pump_closes();
      if (!token_alive(life)) return;
      flush();
      return;
    }

    auto sit = streams_.find(stream_id);
    if (sit != streams_.end() && sit->second.wt_session >= 0) {
      // 已登记的 WT 流：DATA 直接交给会话，不碰 nghttp3。
      stream_rec& s = sit->second;
      std::map<int64_t, uvcpp_h3_wt_session*>::iterator ws =
          wt_sessions_.find(s.wt_session);
      const bool first_fin = r.fin && !s.fin_delivered;
      if (r.fin) s.fin_delivered = true;
      if (ws != wt_sessions_.end()) {
        ws->second->on_stream_data(
            stream_id, reinterpret_cast<const uint8_t*>(r.data), r.size);
        if (!token_alive(life)) return;
        if (first_fin) {
          // 回调里可能把会话拆了（它会从 `wt_sessions_` 摘掉自己）—— 重新查，
          // 不拿上面那个迭代器。
          std::map<int64_t, uvcpp_h3_wt_session*>::iterator w2 =
              wt_sessions_.find(s.wt_session);
          if (w2 != wt_sessions_.end()) w2->second->on_stream_end(stream_id);
          if (!token_alive(life)) return;
        }
      }
      flush();
      return;
    }

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
    {
      // WebTransport 流：读方向干净收场 —— 只通知会话，不碰 nghttp3（它根本
      // 不知道有这条流）。
      auto wt = streams_.find(stream_id);
      if (wt != streams_.end() && wt->second.wt_session >= 0) {
        if (!wt->second.fin_delivered) {
          wt->second.fin_delivered = true;
          std::map<int64_t, uvcpp_h3_wt_session*>::iterator ws =
              wt_sessions_.find(wt->second.wt_session);
          if (ws != wt_sessions_.end()) {
            ws->second->on_stream_end(stream_id);
            if (!token_alive(life)) return;
          }
        }
        flush();
        return;
      }
    }
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
  {
    auto wt = streams_.find(stream_id);
    if (wt != streams_.end() && wt->second.wt_session >= 0) {
      std::map<int64_t, uvcpp_h3_wt_session*>::iterator ws =
          wt_sessions_.find(wt->second.wt_session);
      if (!wt->second.fin_delivered) {
        wt->second.fin_delivered = true;
        if (ws != wt_sessions_.end()) {
          ws->second->on_stream_end(stream_id);
          if (!token_alive(life)) return;
        }
      }
      streams_.erase(wt);
      flush();
      return;
    }
  }
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
  // nghttp3 把"无 body 的请求"直接标成写了 END_STREAM（`submit_request()` 里
  // `dr == NULL` 那一支），之后提交 trailers 是 `INVALID_STATE`。请求 trailers
  // 因此要求有 body（gRPC 等真实用法都有）；没有 body 时如实拒绝，别让它变成
  // 一个 -102。
  if (!req.trailers.empty() && req.body.empty()) return UV_EINVAL;

  const int64_t sid = conn_->open_stream(true);
  if (sid < 0) return sid;  // `NGTCP2_ERR_STREAM_ID_BLOCKED` 如实透传

  stream_rec rec;
  rec.local   = true;
  rec.request = true;
  rec.req.stream_id  = sid;
  rec.resp.stream_id = sid;
  // `:protocol` 非空 ⇒ 这是一条 extended CONNECT，RFC 9297 的 datagram 直接
  // 按 `sid / 4` 关联到它（普通请求要走 `Datagram-Flow-Id` 那一套，本层不做）。
  rec.datagram_ok = !req.protocol.empty();
  streams_.insert(std::make_pair(sid, rec));

  // extended CONNECT（#53）：`:protocol` 非空时 `:method` 必须是 CONNECT
  // （RFC 8441 §4）。调用方填错了就直接钉成 CONNECT，而不是发一条对端会以
  // H3_MESSAGE_ERROR 拒掉的请求。
  const bool ext_connect = !req.protocol.empty();
  std::string method = req.method.empty() ? std::string("GET") : req.method;
  if (ext_connect) method = "CONNECT";

  std::vector<h3_header> hs;
  hs.push_back(make_pseudo(":method", method, h3_header_kind::METHOD));
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
  if (ext_connect) {
    hs.push_back(make_pseudo(":protocol", req.protocol, h3_header_kind::PROTOCOL));
  }
  for (size_t i = 0; i < req.headers.size(); ++i) hs.push_back(req.headers[i]);

  const int rc = session_->submit_request(sid, hs, req.body);
  if (rc != 0) {
    streams_.erase(sid);
    return rc;
  }
  if (!req.trailers.empty()) {
    // 必须在 `flush()` **之前**提交：nghttp3 的 trailers 是流帧队列里排在
    // DATA 之后的一帧，而 DATA 的 reader 一旦被抽干、流就被标成写了 END_STREAM，
    // 那时再提交它是 `NGHTTP3_ERR_INVALID_STATE`。
    const int trc = session_->submit_trailers(sid, req.trailers);
    if (trc != 0) return trc;
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
  if (!resp.trailers.empty()) {
    // 同 `send_request()`：排在 DATA 之后、必须在抽干 reader 之前提交。
    const int trc = session_->submit_trailers(resp.stream_id, resp.trailers);
    if (trc != 0) return trc;
  }
  flush();
  return 0;
}

int uvcpp_h3_connection::send_stream_response(
    int64_t stream_id, const std::vector<h3_header>& headers) {
  if (conn_ == nullptr || session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  const int rc = session_->submit_stream_response(stream_id, headers);
  if (rc != 0) return rc;
  flush();
  return 0;
}

int64_t uvcpp_h3_connection::send_stream_request(const h3_request& req) {
  if (conn_ == nullptr || session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  if (server_side_) return UV_EINVAL;
  if (!ready_) return UV_EAGAIN;
  const int64_t sid = conn_->open_stream(true);
  if (sid < 0) return sid;

  stream_rec rec;
  rec.local   = true;
  rec.request = true;
  rec.req.stream_id  = sid;
  rec.resp.stream_id = sid;
  // 这条入口按定义只发 extended CONNECT（`:method` 钉成 CONNECT、带
  // `:protocol`）⇒ datagram 可用（RFC 9297）。
  rec.datagram_ok = true;
  streams_.insert(std::make_pair(sid, rec));

  std::vector<h3_header> hs;
  hs.push_back(make_pseudo(":method", "CONNECT", h3_header_kind::METHOD));
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
  hs.push_back(make_pseudo(":protocol", req.protocol, h3_header_kind::PROTOCOL));
  for (size_t i = 0; i < req.headers.size(); ++i) hs.push_back(req.headers[i]);

  const int rc = session_->submit_stream_request(sid, hs);
  if (rc != 0) {
    streams_.erase(sid);
    return rc;
  }
  flush();
  return sid;
}

// =========================================================================
// HTTP/3 Datagram（RFC 9297）
// =========================================================================

void uvcpp_h3_connection::set_h3_datagram_enabled(bool on) {
  h3_datagram_enabled_ = on;
  // 会话在构造函数里就建好了，`init()` 在 `on_alpn` 里才跑 —— 这一句只是把
  // 意图转交过去；`start()` 里还会再落一次（那时 `started_` 已置，够用）。
  if (session_) session_->set_h3_datagram_enabled(on);
}

bool uvcpp_h3_connection::peer_h3_datagram() const {
  return h3_datagram_enabled_ && have_remote_settings_ &&
         remote_settings_.h3_datagram;
}

size_t uvcpp_h3_connection::max_datagram_size() const {
  if (conn_ == nullptr || !peer_h3_datagram()) return 0;
  return conn_->max_datagram_size();
}

int uvcpp_h3_connection::send_datagram(int64_t stream_id, const void* data,
                                       size_t len) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  // 本端没开 / 对端没宣告 ⇒ RFC 9297 下**不许发**（不是"发了被丢"）。
  if (!peer_h3_datagram()) return UV_ENOTSUP;
  // 请求流是客户端发起的双向流：`stream_id % 4 == 0`（RFC 9000 §2.1）。
  // 四分之一流号的定义就是 `stream_id / 4`，余数非 0 的流没有这个映射。
  if (stream_id < 0 || (stream_id & 0x3) != 0) return UV_EINVAL;
  std::map<int64_t, stream_rec>::iterator it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.datagram_ok) return UV_EINVAL;

  const size_t max_frame = conn_->max_datagram_size();
  if (max_frame == 0) return UV_ENOTSUP;  // 对端 QUIC 层没开 DATAGRAM

  uint8_t      head[8];
  const size_t hlen = encode_varint(static_cast<uint64_t>(stream_id) >> 2, head);
  if (hlen + len > max_frame) return UV_EMSGSIZE;

  std::string frame(reinterpret_cast<const char*>(head), hlen);
  if (data != nullptr && len > 0) {
    frame.append(static_cast<const char*>(data), len);
  }
  return conn_->send_datagram(frame.data(), frame.size());
}

void uvcpp_h3_connection::on_datagram(const uint8_t* data, size_t datalen) {
  if (!peer_h3_datagram()) { ++dropped_datagrams_; return; }
  uint64_t quarter = 0;
  size_t   used    = 0;
  if (!decode_varint(data, datalen, quarter, used) ||
      quarter > (static_cast<uint64_t>(INT64_MAX) >> 2)) {
    ++dropped_datagrams_;
    return;
  }
  const int64_t sid = static_cast<int64_t>(quarter << 2);
  std::map<int64_t, stream_rec>::iterator it = streams_.find(sid);
  if (it == streams_.end() || !it->second.datagram_ok) {
    ++dropped_datagrams_;
    return;
  }
  // WebTransport 会话的 datagram 先交给会话（四分之一流号 = 会话流号 / 4）。
  std::map<int64_t, uvcpp_h3_wt_session*>::iterator ws = wt_sessions_.find(sid);
  if (ws != wt_sessions_.end()) {
    uvcpp_h3_wt_session* session = ws->second;
    session->on_datagram(data + used, datalen - used);
    return;
  }
  // 与其它入口同一条纪律：回调跑的是用户代码，可能把本对象销毁 —— 先拷出来。
  std::function<void(uvcpp_h3_connection&, int64_t, const uint8_t*, size_t)> cb;
  if (cbs_.on_datagram) cb = cbs_.on_datagram;
  if (!cb) return;
  cb(*this, sid, data + used, datalen - used);
}

// =========================================================================
// WebTransport（draft-ietf-webtrans-http3-13）
// =========================================================================

bool uvcpp_h3_connection::is_peer_bidi(int64_t id) const {
  if (id < 0) return false;
  // RFC 9000 §2.1：客户端发起的双向流 0 mod 4，服务端发起的 1 mod 4。
  return server_side_ ? ((id & 0x3) == 0) : ((id & 0x3) == 1);
}

bool uvcpp_h3_connection::is_peer_uni(int64_t id) const {
  if (id < 0) return false;
  return server_side_ ? ((id & 0x3) == 2) : ((id & 0x3) == 3);
}

int uvcpp_h3_connection::route_new_stream(int64_t id, const char* data,
                                          size_t len, bool fin) {
  if (streams_.find(id) != streams_.end()) return 0;  // 已登记：照旧
  const bool pb = is_peer_bidi(id);
  const bool pu = is_peer_uni(id);
  if (!pb && !pu) return 0;  // 关键流/本地流（正常都已经在表里）

  std::map<int64_t, wt_probe>::iterator it = wt_probes_.find(id);
  if (it == wt_probes_.end()) {
    wt_probe p;
    p.bidi = pb;
    if (data != nullptr && len > 0) p.buf.assign(data, len);
    p.fin = fin;
    it = wt_probes_.insert(std::make_pair(id, p)).first;
  } else {
    if (data != nullptr && len > 0) it->second.buf.append(data, len);
    it->second.fin = it->second.fin || fin;
  }

  const uint8_t* p = reinterpret_cast<const uint8_t*>(it->second.buf.data());
  uint64_t       type = 0;
  size_t         used = 0;
  if (!decode_varint(p, it->second.buf.size(), type, used)) {
    if (it->second.fin) {
      // 留一个不完整的类型码：H3_FRAME_ERROR 收场这条流。
      conn_->shutdown_stream(id, 0x0106);
      wt_probes_.erase(it);
      return -1;
    }
    return 2;  // 等更多字节
  }

  const bool is_wt = pb ? (type == 0x41u) : (type == 0x54u);
  if (!is_wt) {
    // 普通 HTTP 流：把攒下的字节（含 FIN）整段喂回 nghttp3。
    //
    // **必须先把 `stream_rec` 建出来**：① 下一段 DATA 不能再进一次探测
    // （`route_new_stream` 以"在不在 `streams_` 里"判新流，漏了就会把 HEADERS
    // 的中间字节当成帧类型，nghttp3 报 H3_FRAME_UNEXPECTED）；② 这一段的 FIN
    // 要记在 `fin_delivered` 上，否则 `PEER_CLOSED && fin` 会再喂一次 FIN。
    std::string payload;
    payload.swap(it->second.buf);
    const bool pfin = it->second.fin;
    wt_probes_.erase(it);
    stream_rec& s = stream_of(id);
    if (pfin) s.fin_delivered = true;
    if (session_->recv_stream_data(id, payload.data(), payload.size(), pfin,
                                   0) < 0) {
      return -1;  // 致命：`on_fatal` 已经报过
    }
    return 1;
  }

  // WT 流：类型码之后是**会话流号**（变长整数）。
  uint64_t session = 0;
  size_t   used2   = 0;
  if (!decode_varint(p + used, it->second.buf.size() - used, session, used2)) {
    if (it->second.fin) {
      conn_->shutdown_stream(id, 0x0106);
      wt_probes_.erase(it);
      return -1;
    }
    return 2;
  }
  const int64_t session_sid = static_cast<int64_t>(session);
  std::string   rest(it->second.buf, used + used2, std::string::npos);
  const bool    pfin = it->second.fin;
  wt_probes_.erase(it);

  // 会话流是**客户端发起的双向流**（0 mod 4）；对不上就是坏号。
  std::map<int64_t, uvcpp_h3_wt_session*>::iterator ws =
      (session_sid >= 0 && (session_sid & 0x3) == 0)
          ? wt_sessions_.find(session_sid)
          : wt_sessions_.end();
  if (ws == wt_sessions_.end()) {
    // draft 说 H3_ID_ERROR。只重置这条流，不拖垮整条连接（见头文件说明）。
    conn_->shutdown_stream(id, 0x0109);
    return -1;
  }

  stream_rec rec;
  rec.local         = false;
  rec.request       = false;
  rec.raw           = true;
  rec.wt_session    = session_sid;
  rec.wt_bidi       = pb;
  rec.fin_delivered = pfin;
  streams_[id] = rec;

  uvcpp_h3_wt_session* session_ptr = ws->second;
  session_ptr->on_peer_stream(id, pb);
  // 用户回调里可能把会话拆掉（析构会从 `wt_sessions_` 摘掉自己）—— 重新查。
  std::map<int64_t, uvcpp_h3_wt_session*>::iterator again =
      wt_sessions_.find(session_sid);
  if (again == wt_sessions_.end() || again->second != session_ptr) return 1;
  if (!rest.empty()) {
    again->second->on_stream_data(
        id, reinterpret_cast<const uint8_t*>(rest.data()), rest.size());
  }
  if (pfin) {
    std::map<int64_t, uvcpp_h3_wt_session*>::iterator a2 =
        wt_sessions_.find(session_sid);
    if (a2 != wt_sessions_.end() && a2->second == session_ptr) {
      a2->second->on_stream_end(id);
    }
  }
  return 1;
}

void uvcpp_h3_connection::attach_wt_session(int64_t session_sid,
                                            uvcpp_h3_wt_session* s) {
  if (s == nullptr) return;
  wt_sessions_[session_sid] = s;
}

void uvcpp_h3_connection::detach_wt_session(int64_t session_sid,
                                            uvcpp_h3_wt_session* s) {
  std::map<int64_t, uvcpp_h3_wt_session*>::iterator it =
      wt_sessions_.find(session_sid);
  if (it != wt_sessions_.end() && it->second == s) wt_sessions_.erase(it);
  // 这条会话下的流一并收掉。**不能只把 `wt_session` 清成 -1**：那样后续
  // DATA 会落到普通的 nghttp3 路径上，而 nghttp3 根本不认识这条流
  // （`H3_STREAM_CREATION_ERROR`，整条连接陪葬）。重置 + 摘记录才是收场。
  std::vector<int64_t> orphan;
  for (std::map<int64_t, stream_rec>::iterator i = streams_.begin();
       i != streams_.end(); ++i) {
    if (i->second.wt_session == session_sid) orphan.push_back(i->first);
  }
  for (size_t i = 0; i < orphan.size(); ++i) {
    if (conn_ != nullptr) conn_->shutdown_stream(orphan[i], 0);
    streams_.erase(orphan[i]);
  }
  flush();
}

int64_t uvcpp_h3_connection::open_wt_stream(int64_t session_sid, bool bidi) {
  if (conn_ == nullptr || session_ == nullptr) return UV_ENOTCONN;
  if (!ready_ || closed_ || fatal_) return UV_ENOTCONN;
  if (wt_sessions_.find(session_sid) == wt_sessions_.end()) return UV_EINVAL;

  const int64_t sid = conn_->open_stream(bidi);
  if (sid < 0) return sid;  // NGTCP2_ERR_STREAM_ID_BLOCKED 如实透传

  stream_rec rec;
  rec.local      = true;
  rec.request    = false;
  rec.raw        = true;
  rec.wt_session = session_sid;
  rec.wt_bidi    = bidi;
  streams_.insert(std::make_pair(sid, rec));

  uint8_t      prefix[16];
  size_t       n = encode_varint(bidi ? 0x41u : 0x54u, prefix);
  n += encode_varint(static_cast<uint64_t>(session_sid), prefix + n);
  const int rc = conn_->write_stream(
      sid, reinterpret_cast<const char*>(prefix), n, false);
  if (rc != 0) {
    streams_.erase(sid);
    return rc;
  }
  flush();
  return sid;
}

int uvcpp_h3_connection::write_wt_stream(int64_t sid, const char* data,
                                         size_t len, bool fin) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  std::map<int64_t, stream_rec>::iterator it = streams_.find(sid);
  if (it == streams_.end() || it->second.wt_session < 0) return UV_EINVAL;
  const int rc = conn_->write_stream(sid, data, len, fin);
  if (rc != 0) return rc;
  flush();
  return 0;
}

int uvcpp_h3_connection::shutdown_wt_stream(int64_t sid,
                                            uint64_t app_error_code) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  const int rc = conn_->shutdown_stream(sid, app_error_code);
  erase_wt_stream(sid);
  flush();
  return rc;
}

int uvcpp_h3_connection::shutdown_wt_read(int64_t sid,
                                          uint64_t app_error_code) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  return conn_->shutdown_stream_read(sid, app_error_code);
}

void uvcpp_h3_connection::erase_wt_stream(int64_t sid) {
  std::map<int64_t, stream_rec>::iterator it = streams_.find(sid);
  if (it != streams_.end() && it->second.wt_session >= 0) streams_.erase(it);
}

int uvcpp_h3_connection::write_stream_data(int64_t stream_id,
                                           const char* data, size_t len) {
  if (session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  const int rc = session_->write_stream_data(stream_id, data, len, false);
  if (rc != 0) return rc;
  flush();
  return 0;
}

int uvcpp_h3_connection::end_stream_data(int64_t stream_id) {
  if (session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  const int rc = session_->write_stream_data(stream_id, nullptr, 0, true);
  if (rc != 0) return rc;
  flush();
  return 0;
}

void uvcpp_h3_connection::set_stream_sink(
    int64_t stream_id, std::function<void(const char*, size_t)> on_data,
    std::function<void()> on_end) {
  stream_rec& s = stream_of(stream_id);
  s.raw        = true;
  s.sink_data  = std::move(on_data);
  s.sink_end   = std::move(on_end);
}

int uvcpp_h3_connection::send_trailers(
    int64_t stream_id, const std::vector<h3_header>& trailers) {
  if (conn_ == nullptr || session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  if (streams_.find(stream_id) == streams_.end()) return UV_EINVAL;
  const int rc = session_->submit_trailers(stream_id, trailers);
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
  // 最多两趟：第一趟把当前待发清空；如果这一趟的 nghttp3 回调里调了
  // `shutdown_notice()` / `shutdown()`（意图被推迟了），退栈之后再开一趟
  // 把 GOAWAY 编出去。回调里再调一次只会再置位，不会无限循环（第二趟开头
  // 就清掉了）。
  for (int pass = 0; pass < 2; ++pass) {
    if (!self->in_nghttp3()) {
      if (shutdown_notice_pending_) {
        shutdown_notice_pending_ = false;
        const int rv = self->shutdown_notice();
        if (rv < 0) { rc = rv; break; }
      }
      if (shutdown_pending_) {
        shutdown_pending_ = false;
        const int rv = self->shutdown();
        if (rv < 0) { rc = rv; break; }
      }
    }
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
    // 还在 nghttp3 的栈里：外层调用者（`on_read` / `recv_stream_data`）会再冲
    // 一轮，这里不能替它继续。
    if (self->in_nghttp3()) break;
    if (!shutdown_notice_pending_ && !shutdown_pending_) break;
  }

  pump_closes();
  return rc;
}

// =========================================================================
// 平滑退场（GOAWAY，#53）
// =========================================================================

int uvcpp_h3_connection::shutdown_notice() {
  if (session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  // 在 nghttp3 的栈里（`on_request` / `on_stream_close` 都是）：它的写函数
  // 不许重入，记下意图，交给退栈之后那一轮 `flush()`。
  if (session_->in_nghttp3()) {
    shutdown_notice_pending_ = true;
    return 0;
  }
  const int rv = session_->shutdown_notice();
  if (rv != 0) return rv;
  flush();
  return 0;
}

int uvcpp_h3_connection::shutdown() {
  if (session_ == nullptr) return UV_ENOTCONN;
  if (closed_ || fatal_) return UV_ENOTCONN;
  if (session_->in_nghttp3()) {
    shutdown_pending_ = true;
    return 0;
  }
  const int rv = session_->shutdown();
  if (rv != 0) return rv;
  flush();
  return 0;
}

bool uvcpp_h3_connection::is_drained() const {
  return session_ != nullptr && session_->is_drained();
}

bool uvcpp_h3_connection::peer_goaway_received() const { return peer_goaway_; }

int64_t uvcpp_h3_connection::peer_goaway_last_stream_id() const {
  return peer_goaway_id_;
}

bool uvcpp_h3_connection::has_remote_settings() const {
  return have_remote_settings_;
}

uvcpp_h3_remote_settings uvcpp_h3_connection::remote_settings() const {
  return remote_settings_;
}

uint64_t uvcpp_h3_connection::blocked_stream_events() const {
  return blocked_stream_events_;
}

uint64_t uvcpp_h3_connection::blocked_stream_bytes() const {
  return blocked_stream_bytes_;
}

int uvcpp_h3_connection::close(int error_code) {
  if (conn_ == nullptr) return UV_ENOTCONN;
  return conn_->close(error_code);
}

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
