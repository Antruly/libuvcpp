/**
 * @file src/http2/uvcpp_h2_session.cpp
 * @brief h2 会话实现。nghttp2 只出现在这个文件里。
 * @author zhuweiye
 * @version 1.1.0
 */

#include "http2/uvcpp_h2_session.h"

#if UVCPP_NGHTTP2_ENABLE

#include <cstring>
#include <map>
#include <vector>

#include "http2/uvcpp_h2_nghttp2.h"

namespace uvcpp {
namespace {

/// 头名已经按约定是小写存储，但比较仍走大小写无关 —— 依赖"上游确实小写了"
/// 是那种出事后很难查的假设。
bool name_is(const std::string& n, const char* lit) {
  return http_name_equal(n, std::string(lit));
}

/// 连接专属头（RFC 9113 §8.2.2）：h2 里**不该存在**的字段。收到即拒、发出即剥。
bool is_connection_specific(const std::string& n) {
  return name_is(n, "connection") || name_is(n, "keep-alive") ||
         name_is(n, "transfer-encoding") || name_is(n, "upgrade") ||
         name_is(n, "proxy-connection");
}

/// 逐个比对 `http_method_str` 的输出，而不是手写一张字符串表 ——
/// 后者会和枚举一起漂移，而且漂移是静默的（新方法永远解析失败）。
bool method_from_string(const std::string& s, http_method& out) {
  for (int i = 0; i <= static_cast<int>(http_method::HTTP_PRI); ++i) {
    const http_method m = static_cast<http_method>(i);
    if (s == http_method_str(m)) {
      out = m;
      return true;
    }
  }
  return false;
}

/// 这些状态码按协议就不带 body，绝不能因为"body 是空的"去加 `content-length: 0`。
bool status_is_bodyless(int st) {
  return (st >= 100 && st < 200) || st == 204 || st == 304;
}

}  // namespace

// =========================================================================
// impl
// =========================================================================

struct uvcpp_h2_session::impl {
  /// 一条待发 body。
  ///
  /// **必须堆分配**：它的地址会被写进 `nghttp2_data_source.ptr` 并一直用到该流
  /// 发完，而请求那条路上"拿到 stream id"发生在提交**之后** —— 存值就得先有个
  /// 键，存 `std::map` 又会被后续插入之外的删除动作搬走。堆上取址最省事也最稳。
  struct out_body {
    std::string data;
    size_t      offset = 0;
  };

  nghttp2_session*    session     = nullptr;
  bool                server_side = false;
  callbacks           cbs;
  size_t              max_header_list = 64u * 1024u;
  int                 last_error      = 0;

  std::map<int32_t, uvcpp_h2_stream>            streams;
  std::map<int32_t, std::unique_ptr<out_body>>  out_bodies;

  /// 为 @p id 换一块新的 body（旧的丢弃）。返回的指针在流关闭前一直有效。
  out_body* stash_body(int32_t id, std::string body) {
    std::unique_ptr<out_body> ob(new out_body());
    ob->data = std::move(body);
    out_body* raw = ob.get();
    out_bodies[id] = std::move(ob);
    return raw;
  }

  uvcpp_h2_stream& stream_of(int32_t id) {
    auto it = streams.find(id);
    if (it == streams.end()) {
      it = streams.emplace(id, uvcpp_h2_stream()).first;
      it->second.stream_id = id;
      it->second.budget.reset(max_header_list);
    }
    return it->second;
  }

  void reject(int32_t id, uint32_t code) {
    uvcpp_h2_stream& s = stream_of(id);
    if (s.rejected) return;
    s.rejected = true;
    nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, id, code);
  }

  // ---------------------------------------------------------------
  // 收到的头
  // ---------------------------------------------------------------

  void on_begin_headers(const nghttp2_frame* frame) {
    if (frame->hd.type != NGHTTP2_HEADERS) return;  // PUSH_PROMISE 我们不收
    const uint8_t cat = frame->headers.cat;
    // trailer 是**同一条流的第二个头块**：它不该把请求/响应的状态清掉 ——
    // `has_content_length` 一清，DATA 那一侧的走私比对就静默失效了。
    if (cat != NGHTTP2_HCAT_REQUEST && cat != NGHTTP2_HCAT_RESPONSE) return;

    const int32_t id = frame->hd.stream_id;
    uvcpp_h2_stream& s = stream_of(id);
    // CONTINUATION 是**同一个**头部块的续帧，走到这里说明是新的块。
    s.budget.reset(max_header_list);
    s.seen_regular_header = false;
    s.has_content_length  = false;
    s.expected_body       = 0;
    if (cat == NGHTTP2_HCAT_REQUEST) {
      s.request         = uvcpp_http_request();
      s.request.version = uvcpp_http_version::HVER_20;
    } else {
      s.response         = uvcpp_http_response();
      s.response.version = uvcpp_http_version::HVER_20;
    }
  }

  /// 伪头必须在常规头之前（RFC 9113 §8.1.2.1），且**按方向白名单**。
  /// 「不认识的伪头一律拒」还不够：请求里的 `:status` 是认识的，但它属于响应 ——
  /// 只按"认识不认识"判，服务端就会把一个 `:status: 200` 的请求收下。
  void handle_pseudo(uvcpp_h2_stream& s, const std::string& n,
                     const std::string& v) {
    const bool want_request = server_side;
    if (n == ":method") {
      if (!want_request || !method_from_string(v, s.request.method)) {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
      }
    } else if (n == ":path") {
      if (!want_request || v.empty()) {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
      } else {
        s.request.url = v;
      }
    } else if (n == ":scheme") {
      // 只做 TLS。接受 `http` 等于给混淆代理（RFC 9113 §8.1.1）开后门。
      if (!want_request || v != "https") {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
      }
    } else if (n == ":authority") {
      if (!want_request) {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
      } else if (s.request.has_header("host") &&
                 s.request.get_header("host") != v) {
        // `:authority` 与 `host` 不一致是请求走私 / 缓存投毒的经典入口。
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
      } else {
        s.request.set_header("host", v);
      }
    } else if (n == ":status") {
      if (want_request) {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
        return;
      }
      int st = 0;
      for (size_t i = 0; i < v.size() && st >= 0; ++i) {
        if (v[i] < '0' || v[i] > '9' || st > 99) {
          st = -1;
          break;
        }
        st = st * 10 + (v[i] - '0');
      }
      if (st < 100 || st > 599) {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
      } else {
        s.response.status_code = static_cast<http_status>(st);
      }
    } else {
      // 没见过的伪头一律拒 —— 包括 RFC 8441 的 `:protocol`（本库不做）。
      reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
    }
  }

  void handle_regular(uvcpp_h2_stream& s, const std::string& n,
                      const std::string& v) {
    s.seen_regular_header = true;
    if (is_connection_specific(n)) {
      reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
      return;
    }
    if (n == "cookie") {
      // RFC 9113 §8.2.3：cookie 允许拆成多份，语义上要用 "; " 拼回去。
      // 拼不回原样就会让「同一份 cookie 两种解释」变成走私面。
      if (s.request.has_header("cookie")) {
        s.request.set_header("cookie",
                             s.request.get_header("cookie") + "; " + v);
        return;
      }
    }
    if (name_is(n, "host")) {
      // `:authority` 已经落进 `host` 了。这里再出现一个 `host` 是合法的
      // （RFC 9113 允许），但**必须与 `:authority` 一致** —— 不一致就是
      // 那套"代理按一个看、源站按另一个看"的老把戏。
      if (s.request.has_header("host")) {
        if (s.request.get_header("host") != v) {
          reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
        }
        return;
      }
    }
    if (n == "content-length") {
      size_t cl = 0;
      bool   ok = !v.empty();
      for (size_t i = 0; i < v.size() && ok; ++i) {
        if (v[i] < '0' || v[i] > '9') {
          ok = false;
          break;
        }
        cl = cl * 10 + static_cast<size_t>(v[i] - '0');
        if (cl > (size_t)1 << 40) ok = false;  // 荒谬值直接当非法
      }
      if (!ok) {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);
        return;
      }
      if (s.has_content_length && s.expected_body != cl) {
        reject(s.stream_id, NGHTTP2_PROTOCOL_ERROR);  // 重复且不一致
        return;
      }
      s.has_content_length = true;
      s.expected_body      = cl;
    }
    s.request.headers.push_back(http_header{n, v});
  }

  void on_header(const nghttp2_frame* frame, const uint8_t* name, size_t namelen,
                 const uint8_t* value, size_t valuelen) {
    const int32_t id = frame->hd.stream_id;
    uvcpp_h2_stream& s = stream_of(id);
    if (s.rejected) return;  // 已经 RST 了，剩下的头块不再累积

    // nghttp2 自己也校验，但那是"它愿意校验多少"的问题 —— 这条路径是我们
    // 唯一能保证的地方，所以自己再走一遍，不把它当第二保险。
    if (!nghttp2_check_header_name(name, namelen) ||
        !nghttp2_check_header_value_rfc9113(value, valuelen)) {
      reject(id, NGHTTP2_PROTOCOL_ERROR);
      return;
    }
    // 超预算必须**在这里**断开。解完再看等于把 HPACK bomb 解进了内存。
    if (!s.budget.add(namelen, valuelen)) {
      reject(id, NGHTTP2_ENHANCE_YOUR_CALM);
      return;
    }

    const std::string n(reinterpret_cast<const char*>(name), namelen);
    const std::string v(reinterpret_cast<const char*>(value), valuelen);

    const bool pseudo = !n.empty() && n[0] == ':';
    if (pseudo) {
      if (s.seen_regular_header) {
        reject(id, NGHTTP2_PROTOCOL_ERROR);
        return;
      }
      handle_pseudo(s, n, v);
    } else if (server_side) {
      handle_regular(s, n, v);
    } else {
      if (is_connection_specific(n)) {
        reject(id, NGHTTP2_PROTOCOL_ERROR);  // 响应里也不许有
        return;
      }
      s.response.headers.push_back(http_header{n, v});
    }
  }

  // ---------------------------------------------------------------
  // 收到的帧
  // ---------------------------------------------------------------

  void on_frame_recv(const nghttp2_frame* frame) {
    const int32_t id = frame->hd.stream_id;
    auto it = streams.find(id);
    if (it == streams.end()) return;
    uvcpp_h2_stream& s = it->second;
    const bool end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;

    if (frame->hd.type == NGHTTP2_HEADERS) {
      if (s.rejected) return;
      if (frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        // `on_request` 是"头收全了"，`on_request_end` 是"整条请求完了"。
        // 无 body 时两者背靠背，有 body 时后者要等 DATA（或 trailer）。
        if (cbs.on_request) cbs.on_request(*owner, s, end_stream);
        if (end_stream && cbs.on_request_end) cbs.on_request_end(*owner, s);
      } else if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        if (cbs.on_response) cbs.on_response(*owner, s, end_stream);
        // 头自带 END_STREAM ⇒ 这条响应就是头本身，没有 DATA 可等。
        if (end_stream && cbs.on_response_end) cbs.on_response_end(*owner, s);
      } else if (end_stream) {
        // trailer 收完：这是"有 trailer 的请求"的结束信号。不认它，
        // 那条流在业务层就永远等不到收尾。
        finish_body(id, s);
      }
      return;
    }

    if (frame->hd.type == NGHTTP2_DATA && end_stream) {
      if (!s.rejected) finish_body(id, s);
      return;
    }
  }

  /// body 收完的收尾：先对 `content-length`，再通知。
  void finish_body(int32_t id, uvcpp_h2_stream& s) {
    // `content-length` 与实际 DATA 长度不符是 h2→h1 走私的标准手法。
    if (s.has_content_length && s.body_bytes != s.expected_body) {
      reject(id, NGHTTP2_PROTOCOL_ERROR);
      return;
    }
    if (server_side) {
      if (cbs.on_request_end) cbs.on_request_end(*owner, s);
    } else if (cbs.on_response_end) {
      cbs.on_response_end(*owner, s);
    }
  }

  void on_data_chunk(int32_t id, const uint8_t* data, size_t len) {
    auto it = streams.find(id);
    if (it == streams.end()) return;
    uvcpp_h2_stream& s = it->second;
    if (s.rejected) return;

    s.body_bytes += len;
    if (s.body_bytes > H2_DEFAULT_MAX_BODY_BYTES) {
      reject(id, NGHTTP2_ENHANCE_YOUR_CALM);
      return;
    }
    if (s.has_content_length && s.body_bytes > s.expected_body) {
      // 早一点拒：不必等 END_STREAM 才发现对不上。
      reject(id, NGHTTP2_PROTOCOL_ERROR);
      return;
    }
    if (cbs.on_body) {
      cbs.on_body(*owner, s, reinterpret_cast<const char*>(data), len);
    }
  }

  void on_stream_close(int32_t id, uint32_t error_code) {
    auto it = streams.find(id);
    if (it == streams.end()) return;
    if (cbs.on_close) cbs.on_close(*owner, id, error_code);
    // 回调之后流引用即失效，所以放最后。
    out_bodies.erase(id);
    streams.erase(it);
  }

  uvcpp_h2_session* owner = nullptr;

  // ---------------------------------------------------------------
  // nghttp2 回调转发
  //
  // 做成 `impl` 的**静态成员**而不是匿名命名空间里的自由函数：`impl` 是私有的
  // 嵌套类型，外面那些函数连这个名字都写不出来（要用就得把 impl 提成公开声明，
  // 那等于为了让转发函数好写而放宽封装）。静态成员天然有这个访问权。
  // ---------------------------------------------------------------

  static impl* self_of(void* ud) { return static_cast<impl*>(ud); }

  static int cb_begin_headers(nghttp2_session*, const nghttp2_frame* frame,
                              void* ud) {
    self_of(ud)->on_begin_headers(frame);
    return 0;
  }

  static int cb_header(nghttp2_session*, const nghttp2_frame* frame,
                       const uint8_t* name, size_t namelen,
                       const uint8_t* value, size_t valuelen, uint8_t,
                       void* ud) {
    self_of(ud)->on_header(frame, name, namelen, value, valuelen);
    return 0;
  }

  static int cb_frame_recv(nghttp2_session*, const nghttp2_frame* frame,
                           void* ud) {
    self_of(ud)->on_frame_recv(frame);
    return 0;
  }

  static int cb_data_chunk(nghttp2_session*, uint8_t, int32_t stream_id,
                           const uint8_t* data, size_t len, void* ud) {
    self_of(ud)->on_data_chunk(stream_id, data, len);
    return 0;
  }

  static int cb_stream_close(nghttp2_session*, int32_t stream_id,
                             uint32_t error_code, void* ud) {
    self_of(ud)->on_stream_close(stream_id, error_code);
    return 0;
  }

  /// 返回 `nghttp2_ssize`（= `ptrdiff_t`）而不是 `ssize_t` —— 后者的定义在我们
  /// 自己的 shim 里被临时改写过，用它当返回类型等于把 ABI 押在那个宏上。
  static nghttp2_ssize cb_read_body(nghttp2_session*, int32_t, uint8_t* buf,
                                    size_t length, uint32_t* data_flags,
                                    nghttp2_data_source* source, void*) {
    auto* ob   = static_cast<out_body*>(source->ptr);
    const size_t left = ob->data.size() - ob->offset;
    const size_t n    = left < length ? left : length;
    if (n > 0) {
      std::memcpy(buf, ob->data.data() + ob->offset, n);
      ob->offset += n;
    }
    if (ob->offset >= ob->data.size()) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(n);
  }
};

// =========================================================================
// 生命周期
// =========================================================================

uvcpp_h2_session::uvcpp_h2_session(bool server_side) : impl_(new impl()) {
  impl_->server_side = server_side;
  impl_->owner       = this;
}

uvcpp_h2_session::~uvcpp_h2_session() {
  if (impl_ && impl_->session) nghttp2_session_del(impl_->session);
}

int uvcpp_h2_session::init(const callbacks& cbs, size_t max_header_list_size,
                           uint32_t max_concurrent_streams,
                           uint32_t initial_window_size) {
  impl_->cbs             = cbs;
  impl_->max_header_list = max_header_list_size;

  nghttp2_session_callbacks* ncb = nullptr;
  if (nghttp2_session_callbacks_new(&ncb) != 0) return UV_ENOMEM;
  nghttp2_session_callbacks_set_on_begin_headers_callback(ncb,
                                                          &impl::cb_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(ncb, &impl::cb_header);
  nghttp2_session_callbacks_set_on_frame_recv_callback(ncb, &impl::cb_frame_recv);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(ncb,
                                                            &impl::cb_data_chunk);
  nghttp2_session_callbacks_set_on_stream_close_callback(ncb,
                                                         &impl::cb_stream_close);

  const int rv = impl_->server_side
                     ? nghttp2_session_server_new(&impl_->session, ncb, impl_.get())
                     : nghttp2_session_client_new(&impl_->session, ncb, impl_.get());
  nghttp2_session_callbacks_del(ncb);
  if (rv != 0) return rv;

  std::vector<nghttp2_settings_entry> iv;
  iv.push_back({NGHTTP2_SETTINGS_ENABLE_PUSH, 0});  // 我们不收也绝不发 PUSH_PROMISE
  iv.push_back({NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, max_concurrent_streams});
  iv.push_back({NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
                static_cast<uint32_t>(max_header_list_size)});
  if (initial_window_size != 0) {
    iv.push_back({NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, initial_window_size});
  }
  return nghttp2_submit_settings(impl_->session, NGHTTP2_FLAG_NONE, iv.data(),
                                 iv.size());
}

// =========================================================================
// 收发
// =========================================================================

int uvcpp_h2_session::recv(const char* data, size_t len) {
  if (!impl_->session || len == 0) return 0;
  const nghttp2_ssize rv = nghttp2_session_mem_recv2(
      impl_->session, reinterpret_cast<const uint8_t*>(data), len);
  if (rv < 0) {
    impl_->last_error = static_cast<int>(rv);
    // 这里之后**不许再碰 impl_** —— on_fatal 的接收方可以关掉连接，
    // 而连接一关就可能把这个会话一起放掉。
    if (impl_->cbs.on_fatal) impl_->cbs.on_fatal(*this, static_cast<int>(rv));
    return static_cast<int>(rv);
  }
  // 输入必须被完整消费。没吃完而返回非负值是 nghttp2 的"pause"语义，
  // 我们没开 pause，走到这里说明状态机不对 —— 当致命处理。
  if (rv != static_cast<nghttp2_ssize>(len)) {
    impl_->last_error = -1;
    if (impl_->cbs.on_fatal) impl_->cbs.on_fatal(*this, -1);
    return -1;
  }
  return 0;
}

int uvcpp_h2_session::drain(std::string& out) {
  if (!impl_->session) return 0;
  for (;;) {
    const uint8_t* data = nullptr;
    const nghttp2_ssize n = nghttp2_session_mem_send2(impl_->session, &data);
    if (n < 0) return static_cast<int>(n);
    if (n == 0) return 0;
    out.append(reinterpret_cast<const char*>(data), static_cast<size_t>(n));
  }
}

// =========================================================================
// 提交：响应
// =========================================================================

namespace {

/**
 * @brief 把 `uvcpp_http_response` 折成 nghttp2 的头部数组。
 *
 * @return false 表示有不能进 h2 的字段（连接专属头、非法名字/值）。
 *
 * `nv` 里的指针指向 `store` 里的字符串，而 `nghttp2_nv` 的 flags 保持
 * `NGHTTP2_NV_FLAG_NONE`（**默认拷贝** name/value）。所以 `store` 只要活到
 * `nghttp2_submit_*` 调用结束就行。后来者若想加 `NGHTTP2_NV_FLAG_NO_COPY_NAME`
 * 之类的优化，必须先把 `store` 的生命周期延到该帧真的写出去为止 —— 那和现在
 * 这个"提交完就扔"的形状是冲突的。
 */
bool build_response_nv(const uvcpp_http_response& resp, bool omit_body,
                       std::vector<std::string>& store,
                       std::vector<nghttp2_nv>&  nv) {
  store.reserve(resp.headers.size() + 2);
  store.push_back(std::to_string(static_cast<int>(resp.status_code)));

  const size_t body_len = resp.body.size();
  bool         has_cl   = resp.has_header("content-length");

  for (size_t i = 0; i < resp.headers.size(); ++i) {
    const http_header& h = resp.headers[i];
    if (is_connection_specific(h.name)) continue;  // 发出时一律剥掉
    if (!nghttp2_check_header_name(
            reinterpret_cast<const uint8_t*>(h.name.data()), h.name.size()) ||
        !nghttp2_check_header_value_rfc9113(
            reinterpret_cast<const uint8_t*>(h.value.data()), h.value.size())) {
      return false;
    }
    store.push_back(h.value);
    // `content-length` 由下面统一补，避免出现两份。
    if (name_is(h.name, "content-length")) has_cl = false;
  }

  // body 非空却没有 `content-length`：补一个。h2 里它只是参考值，
  // 但缺了它下游的"边收边判"就没依据。
  if (!omit_body && !has_cl && body_len > 0 &&
      !status_is_bodyless(static_cast<int>(resp.status_code))) {
    store.push_back(std::to_string(body_len));
  }

  nv.clear();
  nv.reserve(store.size());
  auto add = [&nv](const char* k, const std::string& v) {
    nghttp2_nv e;
    e.name     = reinterpret_cast<uint8_t*>(const_cast<char*>(k));
    e.value    = reinterpret_cast<uint8_t*>(const_cast<char*>(v.data()));
    e.namelen  = std::strlen(k);
    e.valuelen = v.size();
    e.flags    = NGHTTP2_NV_FLAG_NONE;
    nv.push_back(e);
  };

  add(":status", store[0]);
  // store[0] 是 status，之后是各值（顺序与 push 顺序一致）。这里重建顺序：
  size_t si = 1;
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    const http_header& h = resp.headers[i];
    if (is_connection_specific(h.name)) continue;
    add(h.name.c_str(), store[si++]);
  }
  if (si < store.size()) add("content-length", store[si]);  // 上面补的那个
  return true;
}

}  // namespace

int uvcpp_h2_session::submit_response(int32_t stream_id,
                                      const uvcpp_http_response& resp,
                                      bool omit_body) {
  if (!impl_->session) return UV_EINVAL;
  auto sit = impl_->streams.find(stream_id);
  if (sit == impl_->streams.end()) return UV_EINVAL;
  // 一条流只提交一次完整响应。放过去的话 `stash_body` 会**换掉**那块正在被
  // nghttp2 的 data provider 引用的缓冲（它按地址记着），于是一次应用层的手误
  // 就变成 use-after-free。宁可在这里返回错误。
  if (sit->second.state == h2_stream_state::SENT) return UV_EALREADY;
  sit->second.state = h2_stream_state::SENT;

  // 204/304/1xx 按协议就不带 body，调用方就算忘了 omit_body 也不能发出 DATA。
  if (status_is_bodyless(static_cast<int>(resp.status_code))) omit_body = true;

  std::vector<std::string>  store;
  std::vector<nghttp2_nv>   nv;
  if (!build_response_nv(resp, omit_body, store, nv)) return UV_EINVAL;

  if (omit_body) {
    // data_prd 传 nullptr ⇒ HEADERS 自带 END_STREAM，一个 DATA 帧都不发。
    return nghttp2_submit_response2(impl_->session, stream_id, nv.data(),
                                    nv.size(), nullptr);
  }

  if (resp.body.size() == 0) {
    return nghttp2_submit_response2(impl_->session, stream_id, nv.data(),
                                    nv.size(), nullptr);
  }
  impl::out_body* ob = impl_->stash_body(
      stream_id, std::string(resp.body.get_const_data(), resp.body.size()));

  nghttp2_data_provider2 prd;
  prd.source.ptr    = ob;
  prd.read_callback = &impl::cb_read_body;
  return nghttp2_submit_response2(impl_->session, stream_id, nv.data(), nv.size(),
                                  &prd);
}

int uvcpp_h2_session::submit_status(int32_t stream_id, int status,
                                    const std::string& body) {
  if (!impl_->session) return UV_EINVAL;
  auto sit = impl_->streams.find(stream_id);
  if (sit == impl_->streams.end()) return UV_EINVAL;
  if (sit->second.state == h2_stream_state::SENT) return UV_EALREADY;
  sit->second.state = h2_stream_state::SENT;

  const std::string code = std::to_string(status);
  const std::string len  = std::to_string(body.size());
  const bool        bodyless = status_is_bodyless(status) || body.empty();

  std::vector<std::string> store;
  store.push_back(code);
  store.push_back(len);
  store.push_back(body);

  std::vector<nghttp2_nv> nv;
  auto add = [&nv](const char* k, const std::string& v) {
    nghttp2_nv e;
    e.name     = reinterpret_cast<uint8_t*>(const_cast<char*>(k));
    e.value    = reinterpret_cast<uint8_t*>(const_cast<char*>(v.data()));
    e.namelen  = std::strlen(k);
    e.valuelen = v.size();
    e.flags    = NGHTTP2_NV_FLAG_NONE;
    nv.push_back(e);
  };
  add(":status", store[0]);
  add("content-length", store[1]);

  if (bodyless) {
    return nghttp2_submit_response2(impl_->session, stream_id, nv.data(),
                                    nv.size(), nullptr);
  }
  impl::out_body* ob = impl_->stash_body(stream_id, body);
  nghttp2_data_provider2 prd;
  prd.source.ptr    = ob;
  prd.read_callback = &impl::cb_read_body;
  return nghttp2_submit_response2(impl_->session, stream_id, nv.data(), nv.size(),
                                  &prd);
}

// =========================================================================
// 提交：请求
// =========================================================================

int32_t uvcpp_h2_session::submit_request(const uvcpp_http_request& req,
                                         std::string body) {
  if (!impl_->session) return UV_EINVAL;

  std::vector<std::string> store;
  std::vector<nghttp2_nv>  nv;
  std::vector<std::string> names;

  const std::string method(http_method_str(req.method));
  const std::string authority = req.get_header("host");
  store.push_back(method);
  names.push_back(":method");
  store.push_back("https");
  names.push_back(":scheme");
  store.push_back(authority);
  names.push_back(":authority");
  store.push_back(req.url.empty() ? std::string("/") : req.url);
  names.push_back(":path");

  for (size_t i = 0; i < req.headers.size(); ++i) {
    const http_header& h = req.headers[i];
    if (name_is(h.name, "host")) continue;  // 已经进了 `:authority`
    if (is_connection_specific(h.name)) continue;
    if (!nghttp2_check_header_name(
            reinterpret_cast<const uint8_t*>(h.name.data()), h.name.size()) ||
        !nghttp2_check_header_value_rfc9113(
            reinterpret_cast<const uint8_t*>(h.value.data()), h.value.size())) {
      return UV_EINVAL;
    }
    names.push_back(h.name);
    store.push_back(h.value);
  }

  nv.reserve(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    nghttp2_nv e;
    e.name     = reinterpret_cast<uint8_t*>(const_cast<char*>(names[i].data()));
    e.value    = reinterpret_cast<uint8_t*>(const_cast<char*>(store[i].data()));
    e.namelen  = names[i].size();
    e.valuelen = store[i].size();
    e.flags    = NGHTTP2_NV_FLAG_NONE;
    nv.push_back(e);
  }

  if (body.empty()) {
    return nghttp2_submit_request2(impl_->session, nullptr, nv.data(), nv.size(),
                                   nullptr, nullptr);
  }

  // data_prd 必须在**这一次**提交里给出：`submit_request2` 收到 NULL 时
  // HEADERS 自带 END_STREAM，之后再 `submit_data2` 就是对一条已结束的流发数据。
  // 而 provider 又需要一个活到发完的地址，所以先在堆上备好，拿到 id 再登记。
  std::unique_ptr<impl::out_body> pending(new impl::out_body());
  pending->data = std::move(body);
  nghttp2_data_provider2 prd;
  prd.source.ptr    = pending.get();
  prd.read_callback = &impl::cb_read_body;

  const int32_t sid = nghttp2_submit_request2(
      impl_->session, nullptr, nv.data(), nv.size(), &prd, nullptr);
  if (sid < 0) return sid;

  impl_->out_bodies[sid] = std::move(pending);
  uvcpp_h2_stream& s = impl_->stream_of(sid);
  s.request          = req;
  s.request.version  = uvcpp_http_version::HVER_20;
  return sid;
}

// =========================================================================
// 通用
// =========================================================================

int uvcpp_h2_session::submit_rst(int32_t stream_id, uint32_t error_code) {
  if (!impl_->session) return UV_EINVAL;
  impl_->reject(stream_id, error_code);
  return 0;
}

int uvcpp_h2_session::submit_goaway(uint32_t error_code,
                                    const std::string& debug) {
  if (!impl_->session) return UV_EINVAL;
  return nghttp2_submit_goaway(
      impl_->session, NGHTTP2_FLAG_NONE, 0, error_code,
      reinterpret_cast<const uint8_t*>(debug.empty() ? nullptr : debug.data()),
      debug.size());
}

bool uvcpp_h2_session::want_read() const {
  return impl_->session && nghttp2_session_want_read(impl_->session) != 0;
}

bool uvcpp_h2_session::want_write() const {
  return impl_->session && nghttp2_session_want_write(impl_->session) != 0;
}

uint32_t uvcpp_h2_session::peer_max_concurrent_streams() const {
  if (!impl_->session) return 0;
  return nghttp2_session_get_remote_settings(
      impl_->session, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
}

uvcpp_h2_stream* uvcpp_h2_session::find_stream(int32_t stream_id) {
  auto it = impl_->streams.find(stream_id);
  return it == impl_->streams.end() ? nullptr : &it->second;
}

size_t uvcpp_h2_session::stream_count() const { return impl_->streams.size(); }

int uvcpp_h2_session::last_error() const { return impl_->last_error; }

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE
