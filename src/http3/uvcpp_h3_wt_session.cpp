/**
 * @file src/http3/uvcpp_h3_wt_session.cpp
 * @brief WebTransport over HTTP/3（draft-ietf-webtrans-http3-13）。
 */

#include "http3/uvcpp_h3_wt_session.h"

#if UVCPP_HTTP3_ENABLE

#include <cstring>
#include <utility>
#include <vector>

#include "http3/uvcpp_h3_connection.h"

namespace uvcpp {

namespace {

// RFC 9000 §16 的变长整数。与 `uvcpp_h3_connection.cpp` 里那份同形 —— 两处
// 各留一份是为了不让公开的 wt 头去依赖那个 .cpp 的内部符号（它们都在匿名
// 命名空间里）。
size_t wt_encode_varint(uint64_t v, uint8_t* out) {
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

bool wt_decode_varint(const uint8_t* p, size_t n, uint64_t& v, size_t& used) {
  if (n == 0) return false;
  const size_t len = static_cast<size_t>(1) << (p[0] >> 6);
  if (n < len) return false;
  uint64_t x = p[0] & 0x3fu;
  for (size_t i = 1; i < len; ++i) x = (x << 8) | p[i];
  v    = x;
  used = len;
  return true;
}

const uint64_t kWtCloseSession = 0x2843;  // WT_CLOSE_SESSION
const uint64_t kWtDrainSession = 0x78ae;  // WT_DRAIN_SESSION
const uint64_t kWtSessionGone  = 0x170d7b68;

}  // namespace

// =========================================================================
// uvcpp_h3_wt_stream
// =========================================================================

uvcpp_h3_wt_stream::~uvcpp_h3_wt_stream() {}

int uvcpp_h3_wt_stream::write(const void* data, size_t len) {
  if (!open_) return UV_ENOTCONN;
  // 会话还在等 2xx：先排队，建立后由 `flush_pending()` 一次冲出去。
  if (sid_ < 0) {
    if (data != nullptr && len > 0) {
      pending_out_.append(static_cast<const char*>(data), len);
    }
    return 0;
  }
  if (conn_ == nullptr) return UV_ENOTCONN;
  return conn_->write_wt_stream(sid_, static_cast<const char*>(data), len,
                                false);
}

int uvcpp_h3_wt_stream::end() {
  if (!open_) return UV_ENOTCONN;
  if (sid_ < 0) {
    pending_end_ = true;
    open_        = false;
    return 0;
  }
  if (conn_ == nullptr) return UV_ENOTCONN;
  const int rc = conn_->write_wt_stream(sid_, nullptr, 0, true);
  if (rc == 0) open_ = false;
  return rc;
}

int uvcpp_h3_wt_stream::reset(uint32_t app_error_code) {
  if (!open_) return UV_ENOTCONN;
  // 还没开出去的流：作废即可（没有 QUIC 流可重置）。
  if (sid_ < 0) {
    open_ = false;
    pending_out_.clear();
    return 0;
  }
  if (conn_ == nullptr) return UV_ENOTCONN;
  const int rc = conn_->shutdown_wt_stream(sid_, app_error_code);
  open_ = false;
  return rc;
}

void uvcpp_h3_wt_stream::deliver_data(const uint8_t* data, size_t len) {
  if (on_data_) on_data_(data, len);
}

void uvcpp_h3_wt_stream::deliver_end() {
  if (on_end_) on_end_();
}

// =========================================================================
// uvcpp_h3_wt_session
// =========================================================================

uvcpp_h3_wt_session::uvcpp_h3_wt_session(uvcpp_h3_connection* h3, int64_t sid,
                                         callbacks cbs)
    : h3_(h3), sid_(sid), cbs_(std::move(cbs)) {}

uvcpp_h3_wt_session::~uvcpp_h3_wt_session() {
  if (alive_) *alive_ = 1;  // 先置位：此后迟到的回调都直接返回
  // 再摘登记：h3 连接不再往这里分派任何 datagram / 流，也不再兑现
  // 那条响应 watcher（它捕的是 this）。
  if (h3_ != nullptr) {
    h3_->watch_wt_response(sid_, nullptr);
    h3_->detach_wt_session(sid_, this);
  }
  // 会话流上的落点也要摘掉（h3 连接里那两个 lambda 捕的是 this）。
  if (h3_ != nullptr) h3_->set_stream_sink(sid_, nullptr, nullptr);
  streams_.clear();
}

std::unique_ptr<uvcpp_h3_wt_session> uvcpp_h3_wt_session::accept(
    uvcpp_h3_connection& h3, const h3_request& req, callbacks cbs) {
  // draft-13 的 `:protocol` 取值是 `webtransport`；draft-16 改成了
  // `webtransport-h3`。两个都收，方便与不同版本的对端互通。
  if (req.protocol != "webtransport" && req.protocol != "webtransport-h3") {
    return std::unique_ptr<uvcpp_h3_wt_session>();
  }
  if (req.stream_id < 0) return std::unique_ptr<uvcpp_h3_wt_session>();

  std::vector<h3_header> hs;
  h3_header st;
  st.name  = ":status";
  st.value = "200";
  st.kind  = h3_header_kind::STATUS;
  hs.push_back(st);
  if (h3.send_stream_response(req.stream_id, hs) != 0) {
    return std::unique_ptr<uvcpp_h3_wt_session>();
  }

  std::unique_ptr<uvcpp_h3_wt_session> s(
      new uvcpp_h3_wt_session(&h3, req.stream_id, std::move(cbs)));
  uvcpp_h3_wt_session* raw = s.get();
  h3.attach_wt_session(req.stream_id, raw);
  h3.set_stream_sink(req.stream_id,
                     [raw](const char* d, size_t n) {
                       raw->on_capsule_data(d, n);
                     },
                     [raw]() { raw->on_capsule_end(); });
  return s;
}

std::unique_ptr<uvcpp_h3_wt_session> uvcpp_h3_wt_session::connect(
    uvcpp_h3_connection& h3, const std::string& path,
    const std::string& authority, callbacks cbs) {
  h3_request req;
  req.method    = "CONNECT";
  req.protocol  = "webtransport";
  req.authority = authority;
  req.path      = path.empty() ? std::string("/") : path;
  const int64_t sid = h3.send_stream_request(req);
  if (sid < 0) return std::unique_ptr<uvcpp_h3_wt_session>();

  std::unique_ptr<uvcpp_h3_wt_session> s(
      new uvcpp_h3_wt_session(&h3, sid, std::move(cbs)));
  uvcpp_h3_wt_session* raw = s.get();
  // 客户端：**等 2xx** 才算建立（draft 要求）。之前开的流/datagram 先排队。
  raw->open_    = false;
  raw->pending_ = true;
  h3.attach_wt_session(sid, raw);
  h3.set_stream_sink(sid,
                     [raw](const char* d, size_t n) {
                       raw->on_capsule_data(d, n);
                     },
                     [raw]() { raw->on_capsule_end(); });
  h3.watch_wt_response(sid, [raw](int status, int error) {
    raw->on_connect_response(status, error);
  });
  return s;
}

uvcpp_h3_wt_stream* uvcpp_h3_wt_session::open_bidi_stream() {
  if (h3_ == nullptr || terminated_) return nullptr;
  if (pending_) return make_stream(next_pending_id_--, true);
  return open_ ? make_stream(h3_->open_wt_stream(sid_, true), true) : nullptr;
}

uvcpp_h3_wt_stream* uvcpp_h3_wt_session::open_uni_stream() {
  if (h3_ == nullptr || terminated_) return nullptr;
  if (pending_) return make_stream(next_pending_id_--, false);
  return open_ ? make_stream(h3_->open_wt_stream(sid_, false), false) : nullptr;
}

uvcpp_h3_wt_stream* uvcpp_h3_wt_session::make_stream(int64_t sid, bool bidi) {
  // 负 id 是"还没开出去"的占位（等 2xx 期间），不是错误。
  if (sid == 0) return nullptr;
  std::unique_ptr<uvcpp_h3_wt_stream> p(new uvcpp_h3_wt_stream(h3_, sid, bidi));
  uvcpp_h3_wt_stream* raw = p.get();
  streams_[sid] = std::move(p);
  return raw;
}

int uvcpp_h3_wt_session::send_datagram(const void* data, size_t len) {
  if (h3_ == nullptr || terminated_) return UV_ENOTCONN;
  if (pending_) {
    // 等 2xx 期间排队（有界：64 KiB / 64 条）。「早发的 datagram 静默丢」
    // 是 1.8.10 的行为，这里改成显式排队或显式 ENOBUFS。
    if (pending_dgrams_.size() >= 64 ||
        pending_dgram_bytes_ + len > 64u * 1024u) {
      return UV_ENOBUFS;
    }
    pending_dgrams_.push_back(std::string(
        data != nullptr && len > 0 ? static_cast<const char*>(data) : "", len));
    pending_dgram_bytes_ += len;
    return 0;
  }
  if (!open_) return UV_ENOTCONN;
  return h3_->send_datagram(sid_, data, len);
}

int uvcpp_h3_wt_session::send_capsule(uint64_t type, const std::string& body) {
  if (h3_ == nullptr || !open_) return UV_ENOTCONN;
  uint8_t      head[16];
  size_t       n = wt_encode_varint(type, head);
  n += wt_encode_varint(static_cast<uint64_t>(body.size()), head + n);
  std::string out(reinterpret_cast<const char*>(head), n);
  out.append(body);
  return h3_->write_stream_data(sid_, out.data(), out.size());
}

int uvcpp_h3_wt_session::close(uint32_t code, const std::string& reason) {
  if (h3_ == nullptr) return UV_ENOTCONN;
  if (terminated_) return UV_EALREADY;
  if (reason.size() > 1024) return UV_EINVAL;  // draft 的上限

  if (pending_) {
    // 还没建立：不发 capsule（对端可能还没认下会话），只把 CONNECT 流收掉。
    h3_->end_stream_data(sid_);
    terminated_ = true;
    open_       = false;
    pending_    = false;
    pending_dgrams_.clear();
    pending_dgram_bytes_ = 0;
    streams_.clear();
    return 0;
  }
  if (!open_) return UV_ENOTCONN;

  // draft：终止时把关联流按 `WT_SESSION_GONE` 重置（本端主动关也是终止）。
  for (std::map<int64_t, std::unique_ptr<uvcpp_h3_wt_stream> >::iterator it =
           streams_.begin();
       it != streams_.end(); ++it) {
    if (it->second->sid_ >= 0) {
      h3_->shutdown_wt_stream(it->second->sid_, kWtSessionGone);
    }
  }
  streams_.clear();

  std::string body;
  body.push_back(static_cast<char>((code >> 24) & 0xffu));
  body.push_back(static_cast<char>((code >> 16) & 0xffu));
  body.push_back(static_cast<char>((code >> 8) & 0xffu));
  body.push_back(static_cast<char>(code & 0xffu));
  body.append(reason);

  const int rc = send_capsule(kWtCloseSession, body);
  close_sent_ = true;
  // 发完 capsule 立刻 FIN 会话流（draft 要求）。
  if (h3_ != nullptr) h3_->end_stream_data(sid_);
  // 本端主动关：与 `uvcpp_ws_connection::close()` 同一条语义 —— 调用方自己
  // 发起的，不再补一次 on_close。
  terminated_ = true;
  open_       = false;
  return rc;
}

int uvcpp_h3_wt_session::drain() {
  if (!open_) return UV_ENOTCONN;
  return send_capsule(kWtDrainSession, std::string());
}

void uvcpp_h3_wt_session::on_capsule_data(const char* data, size_t len) {
  if (terminated_) return;
  if (len > 0) capsule_buf_.append(data, len);
  for (;;) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(capsule_buf_.data());
    uint64_t type = 0, clen = 0;
    size_t   u1 = 0, u2 = 0;
    if (!wt_decode_varint(p, capsule_buf_.size(), type, u1)) break;
    if (!wt_decode_varint(p + u1, capsule_buf_.size() - u1, clen, u2)) break;
    if (capsule_buf_.size() - u1 - u2 < clen) break;
    const size_t off = u1 + u2;
    if (type == kWtCloseSession) {
      uint32_t code = 0;
      if (clen >= 4) {
        code = (static_cast<uint32_t>(static_cast<uint8_t>(p[off])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(p[off + 1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(p[off + 2])) << 8) |
               static_cast<uint32_t>(static_cast<uint8_t>(p[off + 3]));
      }
      const std::string reason(
          capsule_buf_.data() + off + (clen >= 4 ? 4 : clen),
          clen >= 4 ? clen - 4 : 0);
      capsule_buf_.erase(0, off + clen);
      terminate(code, reason);
      return;
    }
    if (type == kWtDrainSession) {
      capsule_buf_.erase(0, off + clen);
      // 只是通知：会话仍然可用（draft 明确允许继续用），所以不改 open_。
      std::function<void(uvcpp_h3_wt_session&)> cb = cbs_.on_drain;
      if (cb) {
        const std::shared_ptr<char> life = alive_;
        cb(*this);
        if (!life || *life != 0) return;
      }
      continue;
    }
    // 未知 capsule：draft 允许忽略，消费掉继续。
    capsule_buf_.erase(0, off + clen);
  }
  // 防一把：非法 capsule 长度把缓冲撑爆之前先兜住。
  if (capsule_buf_.size() > 64u * 1024u) capsule_buf_.clear();
}

void uvcpp_h3_wt_session::on_capsule_end() {
  // 会话流干净收尾（没有 WT_CLOSE_SESSION 也等价于 code 0 / 空说明）。
  terminate(0, std::string());
}

void uvcpp_h3_wt_session::on_peer_stream(int64_t stream_id, bool bidi) {
  if (terminated_) return;
  uvcpp_h3_wt_stream* s = make_stream(stream_id, bidi);
  if (s == nullptr) return;
  if (cbs_.on_stream) cbs_.on_stream(*this, *s, bidi);
}

void uvcpp_h3_wt_session::on_stream_data(int64_t stream_id,
                                         const uint8_t* data, size_t len) {
  std::map<int64_t, std::unique_ptr<uvcpp_h3_wt_stream> >::iterator it =
      streams_.find(stream_id);
  if (it == streams_.end()) return;
  it->second->deliver_data(data, len);
}

void uvcpp_h3_wt_session::on_stream_end(int64_t stream_id) {
  std::map<int64_t, std::unique_ptr<uvcpp_h3_wt_stream> >::iterator it =
      streams_.find(stream_id);
  if (it == streams_.end()) return;
  // 单向流读侧收场就是它的全部生命：回调之后再没人能写它，从表里摘掉。
  // 回调期间用局部 `keep` 保住对象（用户的 `on_end` 里可能还会读它）。
  std::unique_ptr<uvcpp_h3_wt_stream> keep;
  if (!it->second->bidi_) {
    keep = std::move(it->second);
    streams_.erase(it);
  } else {
    keep = nullptr;
  }
  if (keep) {
    keep->deliver_end();
    return;
  }
  it->second->deliver_end();
}

void uvcpp_h3_wt_session::on_datagram(const uint8_t* data, size_t len) {
  if (!open_ || terminated_) {
    ++dropped_datagrams_;
    return;
  }
  if (cbs_.on_datagram) cbs_.on_datagram(*this, data, len);
}

void uvcpp_h3_wt_session::on_connect_response(int status, int error) {
  if (terminated_ || !pending_) return;
  if (error != 0) {
    reject(error, "CONNECT stream closed before a 2xx response");
    return;
  }
  if (status < 200 || status >= 300) {
    reject(status, "WebTransport CONNECT rejected (status " +
                       std::to_string(status) + ")");
    return;
  }

  pending_ = false;
  open_    = true;
  flush_pending();
  if (terminated_) return;  // flush 里开流失败会 reject

  // 用户回调可能销毁本会话 —— 拷出来调，之后不再碰成员。
  std::function<void(uvcpp_h3_wt_session&)> cb = cbs_.on_open;
  if (cb) cb(*this);
}

void uvcpp_h3_wt_session::flush_pending() {
  if (h3_ == nullptr || !open_ || terminated_) return;

  // 1) 排队开流。先收键再动表：过程中要 erase/insert。
  std::vector<int64_t> ids;
  for (std::map<int64_t, std::unique_ptr<uvcpp_h3_wt_stream> >::iterator it =
           streams_.begin();
       it != streams_.end(); ++it) {
    if (it->second->sid_ < 0) ids.push_back(it->first);
  }
  for (size_t i = 0; i < ids.size(); ++i) {
    std::map<int64_t, std::unique_ptr<uvcpp_h3_wt_stream> >::iterator it =
        streams_.find(ids[i]);
    if (it == streams_.end()) continue;
    std::unique_ptr<uvcpp_h3_wt_stream> st = std::move(it->second);
    streams_.erase(it);

    const int64_t sid = h3_->open_wt_stream(sid_, st->bidi_);
    if (sid < 0) {
      reject(static_cast<int>(sid), "open WebTransport stream failed");
      return;
    }
    st->sid_ = sid;
    if (!st->pending_out_.empty() || st->pending_end_) {
      const int rc = h3_->write_wt_stream(sid, st->pending_out_.data(),
                                          st->pending_out_.size(),
                                          st->pending_end_);
      st->pending_out_.clear();
      if (rc != 0) {
        reject(rc, "flush pending WebTransport stream failed");
        return;
      }
    }
    streams_[sid] = std::move(st);
  }

  // 2) 排队的数据报。
  while (!pending_dgrams_.empty()) {
    std::string d;
    d.swap(pending_dgrams_.front());
    pending_dgrams_.pop_front();
    pending_dgram_bytes_ -= d.size();
    if (h3_->send_datagram(sid_, d.data(), d.size()) != 0) {
      ++dropped_datagrams_;
    }
  }
}

void uvcpp_h3_wt_session::reject(int code, const std::string& why) {
  if (terminated_) return;
  pending_ = false;
  open_    = false;

  // `on_error` 跑的是用户代码，可能把本会话销毁 —— 拿存活令牌兜住。
  std::function<void(uvcpp_h3_wt_session&, int, const std::string&)> ecb =
      cbs_.on_error;
  if (ecb) {
    const std::shared_ptr<char> life = alive_;
    ecb(*this, code, why);
    if (!life || *life != 0) return;
  }
  terminate(0, why);
}

void uvcpp_h3_wt_session::terminate(uint32_t code, const std::string& reason) {
  if (terminated_) return;
  terminated_ = true;
  open_       = false;
  pending_    = false;
  pending_dgrams_.clear();
  pending_dgram_bytes_ = 0;
  if (h3_ != nullptr) h3_->watch_wt_response(sid_, nullptr);

  // 会话没了：把还在的流按 draft 重置（WT_SESSION_GONE）。
  for (std::map<int64_t, std::unique_ptr<uvcpp_h3_wt_stream> >::iterator it =
           streams_.begin();
       it != streams_.end(); ++it) {
    if (h3_ != nullptr && it->second->sid_ >= 0) {
      h3_->shutdown_wt_stream(it->second->sid_, kWtSessionGone);
    }
  }
  streams_.clear();

  // 回调可能销毁会话本身 —— 先拷贝出来，调用之后不再碰成员。
  std::function<void(uvcpp_h3_wt_session&, uint32_t, const std::string&)> cb =
      cbs_.on_close;
  if (cb) cb(*this, code, reason);
}

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
