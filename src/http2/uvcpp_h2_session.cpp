/**
 * @file src/http2/uvcpp_h2_session.cpp
 * @brief h2 会话实现。nghttp2 只出现在这个文件里。
 * @author zhuweiye
 * @version 1.1.0
 */

#include "http2/uvcpp_h2_session.h"

#if UVCPP_NGHTTP2_ENABLE

#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "http2/uvcpp_h2_nghttp2.h"

namespace uvcpp {

// `uvcpp_h2_common.h` 里那几个具名错误码的保险丝：值必须与 nghttp2 自己的枚举
// 逐一对齐。漂移在这里变成编译错误，而不是"线上悄悄换了语义"。
// 这个 TU 是全仓唯一 include 了 nghttp2 的地方，所以断言只能放这儿。
static_assert(H2_ERR_NO_ERROR == NGHTTP2_NO_ERROR, "h2 error code drift");
static_assert(H2_ERR_REFUSED_STREAM == NGHTTP2_REFUSED_STREAM,
              "h2 error code drift");
static_assert(H2_ERR_CANCEL == NGHTTP2_CANCEL, "h2 error code drift");
static_assert(H2_ERR_ENHANCE_YOUR_CALM == NGHTTP2_ENHANCE_YOUR_CALM,
              "h2 error code drift");

namespace {

/// 单调毫秒。**不用 `uv_now`**：本层刻意不依赖 libuv（`uvcpp_h2_session` 也能
/// 被非 uv 的宿主驱动），而墙钟会被系统对时往回拨 —— 令牌桶按回拨算会凭空
/// 补出一大笔额度，正好把"洪泛"这件事放过去。
uint64_t monotonic_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// 控制帧洪泛的令牌桶（RFC 9113 §10.5）。
///
/// SETTINGS / PING / RST_STREAM / PRIORITY / WINDOW_UPDATE 都不带业务数据，
/// 但每一个都要花 CPU 处理，而 PING 还要我们回一个 ACK —— 攻击者花 1 个包
/// 换我们 1 个包，**是放大器**。一个 64 KiB 的连接窗口对这类帧没有约束，
/// 所以只能自己数。
///
/// 取值：正常客户端在连接建立的头一秒会发两三个 SETTINGS/PING，稳态近乎为零，
/// 所以 64 的突发额度对任何正常客户端都够（包括同时开上百条流的）；32 个/秒
/// 的补充速率则远低于"能让服务端忙起来"的量级。
const double H2_CONTROL_BURST          = 64.0;
const double H2_CONTROL_REFILL_PER_SEC = 32.0;

/// 头名已经按约定是小写存储，但比较仍走大小写无关 —— 依赖"上游确实小写了"
/// 是那种出事后很难查的假设。
bool name_is(const std::string& n, const char* lit) {
  // 别写回 `std::string(lit)`：那是有意的形状，不是笔误的反面。字面量一旦过
  // 15 字符（`"transfer-encoding"` 是 17），这样写就是**每次调用构造一个堆串**，
  // 而 `is_connection_specific` 每个响应头都要过一遍。`http_name_equal` 有
  // `const char*` 重载，直接比，零分配。
  return http_name_equal(n, lit);
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

  /// 单条流待发队列的字节上界（见 `uvcpp_h2_common.h` 那个常量的注释）。
  size_t max_out_stream = H2_DEFAULT_MAX_OUT_STREAM_BYTES;

  /// `nghttp2_session_consume_*` 归还失败过。**不就地吞掉** —— 由 `cb_data_chunk`
  /// 转成 `CALLBACK_FAILURE` 交给 nghttp2，让 `recv()` 按既定的"任何负值都致命"
  /// 收尾。半死的账目比断掉的连接危险得多。
  bool consume_failed = false;

  /// 收尾时要发的 GOAWAY 错误码。正常关闭是 NO_ERROR；被上面那个令牌桶拦下来
  /// 时改成 ENHANCE_YOUR_CALM —— 连接层收尾时读它，否则"为什么关的"这层
  /// 信息会在半路丢掉，对端只看到一个 NO_ERROR，像是我们自己正常退出。
  uint32_t goaway_code = NGHTTP2_NO_ERROR;

  /// 对端发来的 GOAWAY 三个字段。收到过没有单独记一个 bool —— `error_code`
  /// 和 `last_stream_id` 都合法地可以是 0，用它俩当"收到过"的判据会漏。
  bool     peer_goaway      = false;
  uint32_t peer_goaway_code = NGHTTP2_NO_ERROR;
  int32_t  peer_goaway_last = 0;

  // 控制帧令牌桶（见 `monotonic_ms` 上面那段）。
  uint64_t ctl_last_ms    = 0;
  double   ctl_tokens     = H2_CONTROL_BURST;
  bool     flood_tripped  = false;  ///< 已经越界
  bool     flood_reported = false;  ///< 已经通知过连接层（只通知一次）

  /// 致命错误同样只通知一次，理由和洪泛那条**是同一个**：`on_fatal` 的接收方
  /// 会去关连接，而关连接是一次性动作。会话一旦致命就再也回不到可用状态，重复
  /// 通知的收益为零，代价是"调用方还没察觉连接已关"之前塞进来的每一批字节都换
  /// 一次关连接指令。
  bool fatal_reported = false;

  /// 正在 nghttp2 的调用栈里（`mem_recv` 或 `mem_send`），也就是说随时可能有
  /// 回调跑在用户代码上。
  ///
  /// 在**这个窗口里重入 `mem_send` 是 nghttp2 没保证过的用法**：实测完整页堆下
  /// 会在"回调里把 RST_STREAM 冲出去"那条路上读到一块 nghttp2 自己刚释放的
  /// `nghttp2_stream`（裸跑绿、页堆崩）。发方向唯一的收窄点是 `drain()`，所以
  /// 调用方（`uvcpp_h2_connection::flush()`）拿它当推迟的依据。
  bool in_nghttp2 = false;

  /// 收口 `submit_request2` 的返回值。
  ///
  /// 它唯一值得单独对待的失败是**本端流号用完**（客户端每开一条流号 +2，上限
  /// 2^31）。已核：`NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE` 在 nghttp2 源码里
  /// **只**从 `nghttp2_submit.c` 的 `stream_id == -1` 那一支返回 —— 也就是只有
  /// 自己发号的一方（客户端）碰得到，服务端不适用。
  ///
  /// 按 RFC 9113 §5.1.1，这时该发 GOAWAY(NO_ERROR)：告诉对端"这个连接上不会
  /// 再有新流了，要开新流请重连"。不发的话调用方只拿到一个负值，对端什么都
  /// 等不到，只能耗到超时。
  int32_t finish_submit_request(int32_t sid) {
    if (sid == NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE) {
      nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE,
                            nghttp2_session_get_last_proc_stream_id(session),
                            NGHTTP2_NO_ERROR, nullptr, 0);
    }
    return sid;
  }

  /// 控制帧是否还在预算内。返回 false 表示**已经洪泛** —— 本函数只负责置位，
  /// 真正的收尾在 `recv()` 里做：不能在 `mem_recv` 的栈上重入 `drain`。
  bool control_frame_ok() {
    const uint64_t now = monotonic_ms();
    if (ctl_last_ms == 0) ctl_last_ms = now;
    const uint64_t dt = now - ctl_last_ms;
    if (dt > 0) {
      ctl_last_ms = now;
      ctl_tokens += static_cast<double>(dt) * H2_CONTROL_REFILL_PER_SEC / 1000.0;
      if (ctl_tokens > H2_CONTROL_BURST) ctl_tokens = H2_CONTROL_BURST;
    }
    if (ctl_tokens < 1.0) {
      flood_tripped = true;
      return false;
    }
    ctl_tokens -= 1.0;
    return true;
  }

  std::map<int32_t, uvcpp_h2_stream>            streams;
  std::map<int32_t, std::unique_ptr<out_body>>  out_bodies;

  // -------------------------------------------------------------------
  // 流式响应（`submit_headers` + `submit_data`）
  //
  // 与上面那套"一条流一块 body"的区别是**块数不定、到达时间不定**。所以每条
  // 流一个队列，nghttp2 的 data provider 从队首取字节。
  //
  // `frame_pending` 是这套设计的关键：nghttp2 允许一条流上排队多个 data frame，
  // 而它们是**按提交顺序**各自读自己的 provider 的。每块提交一次 frame，就会
  // 有几个 frame 同时指着同一个队列，第二个 frame 会从头再读一遍已经发过的
  // 字节。所以一条流**同时只允许一个** data frame 在飞，队列排空之前不撤，
  // 靠它自己回头再取。
  // -------------------------------------------------------------------
  struct out_chunk {
    std::string              data;
    size_t                   offset     = 0;
    bool                     end_stream = false;
    std::function<void(int)> done;
  };

  struct out_stream {
    std::deque<out_chunk> chunks;
    bool                  frame_pending = false;
    bool                  ended         = false;  ///< 已提交过 end_stream
    /// 队空时 provider 返回过 `NGHTTP2_ERR_DEFERRED`，那条 data frame 现在挂在
    /// nghttp2 的延迟队列里 —— 新数据入队时必须显式 `resume` 才叫得醒。
    bool                  deferred      = false;
    /// Σ 还没交给 nghttp2 的字节（`chunks` 里那些 `data.size() - offset` 之和）。
    /// 维护成运行计数是为了让 `submit_data` 的上界判断是 O(1)，不必遍历队列。
    /// 入队在 `submit_data`、出账在 `cb_read_stream` —— **两处必须成对**。
    size_t                queued_bytes  = 0;
  };

  std::map<int32_t, out_stream> out_streams;

  /// 已上线、等着回调的块（结果码已绑好）。由传输层在写完成之后取走。
  std::vector<std::function<void()>> completed;

  /// 把一块的 `done` 绑上结果码塞进 `completed`。
  ///
  /// **每一块的 `done` 都必须恰好跑一次** —— 包括"流中途没了"的场合。少了
  /// 这一条，`uvcpp_web_response::flush_stream` 的收尾回调永远不回来，那条
  /// 流式响应就永久挂起（这是 `uvcpp_web_stream_sink::stream_write` 写明的
  /// 契约）。
  void retire_chunk(out_stream& os, int code) {
    out_chunk& c = os.chunks.front();
    if (c.done) {
      std::function<void(int)> f = std::move(c.done);
      completed.push_back([f, code]() { f(code); });
    }
    const bool end = c.end_stream;
    os.chunks.pop_front();
    if (end) os.ended = true;
  }

  /// 流没了：把还没上线的块全部按 @p code 结掉。
  void cancel_stream_out(int32_t id, int code) {
    auto it = out_streams.find(id);
    if (it == out_streams.end()) return;
    while (it->second.chunks.size() > 0) retire_chunk(it->second, code);
    out_streams.erase(it);
  }

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
      s.response = h2_response_not_received();
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
      http_reserve_headers(s.response.headers);
      s.response.headers.push_back(http_header{n, v});
    }
  }

  // ---------------------------------------------------------------
  // 收到的帧
  // ---------------------------------------------------------------

  void on_frame_recv(const nghttp2_frame* frame) {
    // 收到 GOAWAY 是**连接级**的事件：对端宣告"这个连接上不会再处理新流了"。
    // 不记下来的话，我们照样把永远发不出去的流号还给调用方（`submit_request`
    // 那条守卫读的就是它）。记在查表之前，理由和下面那个 switch 一样。
    if (frame->hd.type == NGHTTP2_GOAWAY) {
      peer_goaway      = true;
      peer_goaway_code = frame->goaway.error_code;
      peer_goaway_last = frame->goaway.last_stream_id;
    }

    // 计数必须在按流号查表**之前** —— 这些帧的 `stream_id` 恒为 0，
    // 落在下面那个 `streams.end()` 的早返回上，一个都数不到。
    switch (frame->hd.type) {
      case NGHTTP2_SETTINGS:
      case NGHTTP2_PING:
      case NGHTTP2_RST_STREAM:
      case NGHTTP2_PRIORITY:
      case NGHTTP2_WINDOW_UPDATE:
        if (!control_frame_ok()) return;
        break;
      default:
        break;
    }

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

  /// 收方向 DATA 的**唯一**入口（进出站调试时先看这里）。两条额度的规则是：
  ///   连接级 —— **无条件、且在任何早退之前**（连接窗口是全连接共享的）
  ///   流级   —— 只有"这条流还在正常收"才还；暂停则记账欠着，这就是背压
  ///
  /// 为什么"只有这一条路要还"：nghttp2 自己会把**到不了本回调**的字节在连接级
  /// 消费掉（被忽略的 DATA `nghttp2_session.c:6948-6950`、messaging 判违规的 DATA
  /// `:6854-6857`、Pad Length `:6726`），而本回调只在 `data_readlen > 0` 时触发
  /// （`:6890`）；连接窗口在回调**之前**就被扣了（`:6804`）。这个开关打开之后
  /// nghttp2 的自动 WINDOW_UPDATE 被抑制（`:5100`/`:5124`），于是**只有**下面这两
  /// 次 `consume` 能把 `recv_window_size` 降回来 —— 降不回来会走
  /// `nghttp2_session_terminate_session(FLOW_CONTROL_ERROR)`（`:5121`），
  /// 那是**整条会话**死掉，不是"上传停住"。
  void on_data_chunk(int32_t id, const uint8_t* data, size_t len) {
    // ① 连接级：位置是判据，不是风格。一条暂停（或刚被 RST）的流如果把连接级也
    //    扣住，攒够一个连接窗口（65535）就能把同一条连接上**别的流**一起饿死 ——
    //    那是连接级误伤，不是背压。
    if (nghttp2_session_consume_connection(session, len) != 0) {
      consume_failed = true;
      return;
    }

    auto it = streams.find(id);
    if (it == streams.end()) return;  // 连接级已还；没有流对象可还流级
    uvcpp_h2_stream& s = it->second;
    if (s.rejected) return;           // 流要没了：欠额随流消失，不再还

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

    // ② 流级：这才是背压本身。暂停期间**不**归还，攒着 —— 对端把当前剩余的窗口
    //    发完就自己停下来（它无法再发），于是欠额天然有界（≤ 一个每流窗口）。
    if (s.paused) {
      s.paused_owed += len;
    } else if (nghttp2_session_consume_stream(session, id, len) != 0) {
      consume_failed = true;
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
    // 流式那条路的收尾：还在排队的块这辈子都发不出去了，但它们的 `done`
    // **必须**跑 —— 否则发起方（框架的流式响应）永远等不到结算。
    cancel_stream_out(id, UV_ECANCELED);
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
    impl* self = self_of(ud);
    self->on_data_chunk(stream_id, data, len);
    // 额度归还失败**不吞**：账目坏掉的会话继续跑，症状正好是"某条流在 64 KiB 处
    // 永久停住" —— 本笔要消灭的那个形状。实际只有 NOMEM 那一格够得着（开着这个
    // 开关时 INVALID_STATE 不可能）。必须返回 CALLBACK_FAILURE 本身：
    // `nghttp2_session.c:6894` 只在 `nghttp2_is_fatal(rv)` 时才转成它，
    // 返回 INVALID_STATE 那种非致命码等于什么也没说。
    return self->consume_failed ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
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

  /// 流式响应的 data provider。`source->ptr` 是那条流的 `out_stream*`
  /// （`out_streams` 是 `std::map`，节点地址稳定，插入别的流不会把它搬走）。
  ///
  /// **队列空但还没 END_STREAM 时返回 `NGHTTP2_ERR_DEFERRED`** —— 这是"以后
  /// 还会有"的**唯一**表达方式（nghttp2.h 的 provider 契约原话：postpone 靠
  /// 返回 DEFERRED）。返回 0 不是"稍后再来"，而是"这一帧就是 0 字节"：
  /// `nghttp2_session_pack_data` 会照 0 的长度组一个空 DATA 帧，而 `eof` 仍是
  /// 0 ⇒ 同一帧被反复重打包，`drain()` 的循环再也退不出来（这是实打实撞过的：
  /// 一次卡死里 60 秒刷了 1370 万行，`out_` 无上限增长）。
  ///
  /// 代价是醒来必须由我们负责：`submit_data` 入队新块时叫一次
  /// `nghttp2_session_resume_data`。延迟标记记在 `out_stream::deferred` 上。
  static nghttp2_ssize cb_read_stream(nghttp2_session*, int32_t, uint8_t* buf,
                                      size_t length, uint32_t* data_flags,
                                      nghttp2_data_source* source, void* ud) {
    impl* self = self_of(ud);
    auto* os   = static_cast<out_stream*>(source->ptr);
    size_t n = 0;
    while (os->chunks.size() > 0 && n < length) {
      out_chunk&   c    = os->chunks.front();
      const size_t left = c.data.size() - c.offset;
      const size_t take = (left < length - n) ? left : (length - n);
      if (take > 0) {
        std::memcpy(buf + n, c.data.data() + c.offset, take);
        c.offset += take;
        n += take;
        // 记账点**必须**跟着字节走：`out_stream::queued_bytes` 是 `submit_data`
        // 那道上界的输入，只增不减的话跑久了会把一条正常的流误拒。
        os->queued_bytes -= take;
      }
      if (c.offset >= c.data.size()) {
        // 这块整块交出去了。END_STREAM 的那一块发完就到此为止，后面的块
        // （如果有）不该存在 —— `submit_data` 已经挡住了。
        const bool end = c.end_stream;
        self->retire_chunk(*os, 0);
        if (end) {
          *data_flags |= NGHTTP2_DATA_FLAG_EOF;
          break;
        }
      } else {
        break;  // 缓冲区满了，这块还没发完
      }
    }
    if (n == 0 && (*data_flags & NGHTTP2_DATA_FLAG_EOF) == 0) {
      os->deferred = true;
      return NGHTTP2_ERR_DEFERRED;
    }
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

  nghttp2_option* opt = nullptr;
  if (nghttp2_option_new(&opt) != 0) {
    nghttp2_session_callbacks_del(ncb);
    return UV_ENOMEM;
  }
  // 发方向的上限显式设一次。nghttp2 的默认值（`NGHTTP2_MAX_HEADERSLEN`）恰好也
  // 是 65536，但"恰好相等"不是一份契约 —— `header_block_fits()` 是按这个常量
  // 在拦的，两边必须是同一个数。
  nghttp2_option_set_max_send_header_block_length(opt, H2_MAX_SEND_HEADER_BLOCK);
  // 收方向背压的前提：nghttp2 不再自己补窗口，改由我们在 `on_data_chunk` 里按
  // `paused` 归还。**无条件对所有会话打开**，不做"第二个模式" —— 两个模式的账
  // 就是半套，而半套在这里的下场是整条连接在 65535 字节处被 FLOW_CONTROL_ERROR
  // 终止（`nghttp2_session.c:5121`），不是"慢一点"。
  //
  // 不暂停时与开关关掉**帧集合与阈值逐帧相同**（两边用的是同一个
  // `nghttp2_should_send_window_update`，只是驱动量从"收到量"换成"消费量"），
  // 只有同一批 `drain` 之内的**次序**可能不同 —— WINDOW_UPDATE 从 `on_body`
  // **之前**入队变成**之后**。RFC 9113 §6.9 对它与其他帧没有次序约束。
  nghttp2_option_set_no_auto_window_update(opt, 1);
  const int rv =
      impl_->server_side
          ? nghttp2_session_server_new2(&impl_->session, ncb, impl_.get(), opt)
          : nghttp2_session_client_new2(&impl_->session, ncb, impl_.get(), opt);
  nghttp2_option_del(opt);
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
  // 已经致命之后**不再进 nghttp2**：`mem_recv` 一旦返回致命错误，会话就停在
  // 未定义状态上，官方契约是不许再用。原样把它当时给的那个错误回给调用方。
  // 顺带这也是"只通知一次"能成立的前提 —— 不在这里拦住的话，每次 recv 都会
  // 在 nghttp2 里重新走一遍错误路径。
  if (impl_->fatal_reported) return impl_->last_error != 0 ? impl_->last_error : -1;
  impl_->in_nghttp2 = true;
  const nghttp2_ssize rv = nghttp2_session_mem_recv2(
      impl_->session, reinterpret_cast<const uint8_t*>(data), len);
  // 在派发 `on_fatal` **之前**就收掉：那两条路是特意放在 `mem_recv` 之外的，
  // 接收方会走 `shutdown()` → `flush()` → `drain()`，那一步必须能真的发出去。
  impl_->in_nghttp2 = false;
  if (rv < 0) {
    impl_->last_error = static_cast<int>(rv);
    // 这里之后**不许再碰 impl_** —— on_fatal 的接收方可以关掉连接，
    // 而连接一关就可能把这个会话一起放掉。
    if (!impl_->fatal_reported) {
      impl_->fatal_reported = true;
      if (impl_->cbs.on_fatal) impl_->cbs.on_fatal(*this, static_cast<int>(rv));
    }
    return static_cast<int>(rv);
  }
  // 输入必须被完整消费。没吃完而返回非负值是 nghttp2 的"pause"语义，
  // 我们没开 pause，走到这里说明状态机不对 —— 当致命处理。
  if (rv != static_cast<nghttp2_ssize>(len)) {
    impl_->last_error = -1;
    if (!impl_->fatal_reported) {
      impl_->fatal_reported = true;
      if (impl_->cbs.on_fatal) impl_->cbs.on_fatal(*this, -1);
    }
    return -1;
  }

  // 控制帧洪泛。收尾**必须**放在 `mem_recv` 之外，理由与上面那条 fatal 相同：
  // `on_fatal` 的接收方会走 `shutdown()`，那一步会 `drain()`（= `mem_send`），
  // 而在 `mem_recv` 的栈上重入 `mem_send` 是 nghttp2 没保证过的用法。
  if (impl_->flood_tripped && !impl_->flood_reported) {
    impl_->flood_reported = true;
    impl_->goaway_code    = NGHTTP2_ENHANCE_YOUR_CALM;
    // 从这里往下**不许再碰 `impl_`**：接收方可以关掉连接，而连接一关就可能
    // 把这个会话一起放掉。`on_fatal` 也按值先取出来。
    if (impl_->cbs.on_fatal) impl_->cbs.on_fatal(*this, NGHTTP2_ENHANCE_YOUR_CALM);
    return UV_ECANCELED;
  }
  return 0;
}

uint32_t uvcpp_h2_session::goaway_code() const { return impl_->goaway_code; }

bool uvcpp_h2_session::in_nghttp2() const { return impl_->in_nghttp2; }

int uvcpp_h2_session::drain(std::string& out) {
  if (!impl_->session) return 0;

  // 冲出去的帧会**同步**回调回来（`session_after_frame_sent1` 撞上 RST_STREAM
  // 就地关流），那些回调一路跑到用户代码上 —— 用户代码接着提交并再冲一次是
  // 完全正常的写法。那一下要挡住，理由见 `impl::in_nghttp2`。用 RAII 而不是
  // 手动置位：`out.append` 抛 `bad_alloc` 时也不能把标记留在里面。
  struct in_nghttp2_scope {
    bool* flag;
    explicit in_nghttp2_scope(bool* f) : flag(f) { *flag = true; }
    ~in_nghttp2_scope() { *flag = false; }
  } scope(&impl_->in_nghttp2);

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
  const int status = static_cast<int>(resp.status_code);

  store.clear();
  store.reserve(resp.headers.size() + 2);
  store.push_back(std::to_string(status));

  // `content-length` 由这里统一发**恰好一份**：`resp.headers` 里那份只提供值，
  // 不直接上线。原写法是"循环里照发、末尾再补一份"，于是同一份长度发了两遍 ——
  // nghttp2 收到重复的 `content-length` 一律判 -531（Invalid HTTP header field）
  // 并 RST 整条流，客户端表现为**一条响应都收不到**。这个洞只在框架侧暴露：
  // 服务端自己的用例响应从不带 `content-length`，而框架总会设它。
  std::string cl_value;
  bool        has_cl = false;

  // 名字与值**成对**攒：`names[i]` 对应 `store[i]`，一次循环同时推进。
  //
  // 别改成"先攒值、再拿 `resp.headers` 平行走一遍取名字"：那两遍的过滤条件
  // （连接专属头、content-length）必须**逐字相同**，改一处忘另一处就会整体
  // 错位一格 —— 之前这里就错位成了 `content-length: uvcpp` / `server: 17`，
  // 客户端拿到一份合法但内容错乱的头部。单遍成对攒没有这个失败模式。
  //
  // 名字指向 `resp.headers` 或字面量，**都比本次调用活得久**；`nghttp2_submit_*`
  // 是在本函数返回之后才跑的，指向局部量的名字会当场变成悬垂。
  std::vector<const char*> names;
  names.reserve(resp.headers.size() + 2);  // :status + 可能补的 content-length
  names.push_back(":status");

  for (size_t i = 0; i < resp.headers.size(); ++i) {
    const http_header& h = resp.headers[i];
    if (is_connection_specific(h.name)) continue;  // 发出时一律剥掉
    if (name_is(h.name, "content-length")) {
      cl_value = h.value;
      has_cl   = true;
      continue;  // 值已记下，末尾统一发
    }
    if (!nghttp2_check_header_name(
            reinterpret_cast<const uint8_t*>(h.name.data()), h.name.size()) ||
        !nghttp2_check_header_value_rfc9113(
            reinterpret_cast<const uint8_t*>(h.value.data()), h.value.size())) {
      return false;
    }
    names.push_back(h.name.c_str());
    store.push_back(h.value);
  }

  // body 非空却没有 `content-length`：补一个。h2 里它只是参考值，
  // 但缺了它下游的"边收边判"就没依据。
  //
  // 无实体状态不用单独判：`submit_response` 已经把它们的 `omit_body` 强制成
  // true（见那里的注释），所以下面这个 `!omit_body` 就是那个判据。
  if (!has_cl && !omit_body && resp.body.size() > 0) {
    cl_value = std::to_string(resp.body.size());
    has_cl   = true;
  }
  // nghttp2 自己就会拒收：1xx 一律不许有 `content-length`，204 只认 "0"。
  // 与其让它把整条流 RST 掉，不如在这里按同一套规则裁掉。
  if (status / 100 == 1 || (status == 204 && cl_value != "0")) has_cl = false;

  if (has_cl) {
    names.push_back("content-length");
    store.push_back(cl_value);
  }

  nv.clear();
  nv.reserve(names.size());
  for (size_t i = 0; i < names.size(); ++i) {
    nghttp2_nv e;
    e.name     = reinterpret_cast<uint8_t*>(const_cast<char*>(names[i]));
    e.value    = reinterpret_cast<uint8_t*>(const_cast<char*>(store[i].data()));
    e.namelen  = std::strlen(names[i]);
    e.valuelen = store[i].size();
    e.flags    = NGHTTP2_NV_FLAG_NONE;
    nv.push_back(e);
  }
  return true;
}

/**
 * @brief 这个头部块还发得出去吗。
 *
 * nghttp2 在发送前会拿 `nghttp2_hd_deflate_bound()` 估一个上界，超过
 * `max_send_header_block_length` 就 `return NGHTTP2_ERR_FRAME_SIZE_ERROR`
 * （`nghttp2_session.c:2101`）。**而这个错误码是 `is_non_fatal` 的**，于是它
 * 在上层的 `OB_POP_ITEM` 分支里被这样处理（`nghttp2_session.c:2853-2925`）：
 * 丢掉这一帧、把这条流按 `REFUSED_STREAM` 关掉、然后 `break` 继续跑 ——
 * 既不通知我们（`on_frame_not_send_callback` 没注册，那个 `if` 整块被跳过），
 * 也不发 RST_STREAM（对端根本不知道这条流存在过）。而且这是在**发**的时候才
 * 发生的，那时 `submit_*` 早就把一个正的流号还给调用方了。
 *
 * 净效果：调用方拿到一个成功的返回值，然后永远等不到任何东西，对端也一样。
 * 这是"回调永远不来"那一类里最难查的一种 —— 线上一个字节都没有，没有任何
 * 出错的地方可以看。所以本层按同一个公式先算一遍，把它变成一个**同步**的
 * `UV_EMSGSIZE`，而且**在动流状态之前**退掉。
 *
 * 公式与 `nghttp2_hd_deflate_bound()`（`nghttp2_hd.c:1596-1622`）逐字一致：
 * 那个函数 `(void)deflater`、不看动态表状态，是 nv 数组的纯函数，所以复刻它
 * 不会随连接状态漂。`+ 5` 是 `NGHTTP2_PRIORITY_SPECLEN`（`nghttp2_frame.h:64`），
 * 即 nghttp2 那处传的 `additional`。**升级 nghttp2 时这三处要一起核。**
 */
bool header_block_fits(const std::vector<nghttp2_nv>& nv) {
  size_t bound = 12;         // 至多两次动态表尺寸变更，每次 6 字节
  bound += 12 * nv.size();   // 每个字段的名字与值各按 7 位前缀的最长编码计
  for (size_t i = 0; i < nv.size(); ++i) {
    bound += nv[i].namelen + nv[i].valuelen;
  }
  return bound + 5 <= H2_MAX_SEND_HEADER_BLOCK;
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
  // 一次提交 = 一条流只发一套响应。`HEADERS_SENT`（流式那条路的头部已经发过）
  // 也必须挡在这里，否则会往同一条流上再发一套 HEADERS。
  if (sit->second.state != h2_stream_state::OPEN) return UV_EALREADY;

  // 204/304/1xx 按协议就不带 body，调用方就算忘了 omit_body 也不能发出 DATA。
  if (status_is_bodyless(static_cast<int>(resp.status_code))) omit_body = true;

  std::vector<std::string>  store;
  std::vector<nghttp2_nv>   nv;
  if (!build_response_nv(resp, omit_body, store, nv)) return UV_EINVAL;
  // 两条退路都必须在**置 SENT 之前**：置了再退，这条流就停在"说过要发、其实
  // 一个字节都没发"的状态上，而 nghttp2 那边连流都没建 —— 对端只会一直等。
  if (!header_block_fits(nv)) return UV_EMSGSIZE;
  sit->second.state = h2_stream_state::SENT;

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

int uvcpp_h2_session::submit_headers(int32_t stream_id,
                                     const uvcpp_http_response& resp) {
  if (!impl_->session) return UV_EINVAL;
  auto sit = impl_->streams.find(stream_id);
  if (sit == impl_->streams.end()) return UV_EINVAL;
  // `SENT` 那格留给"整条响应一次发完"。这里用 `HEADERS_SENT` 与它区分开：
  // 走错路（先 `submit_headers` 又 `submit_response`）必须当场被挡住，否则
  // 两条路会各自往同一条流上发一套 HEADERS。
  if (sit->second.state != h2_stream_state::OPEN) return UV_EALREADY;

  std::vector<std::string> store;
  std::vector<nghttp2_nv>  nv;
  // `omit_body = true` 走的是"不补 content-length"那一支，正是流式要的：
  // 长度此刻不知道，补一个就是在说谎。`resp` 里若已写明就原样发出。
  if (!build_response_nv(resp, /*omit_body=*/true, store, nv)) return UV_EINVAL;
  // 同上：置位之前退，否则流停在"头部发过了"而实际没发。
  if (!header_block_fits(nv)) return UV_EMSGSIZE;
  sit->second.state = h2_stream_state::HEADERS_SENT;

  impl_->out_streams[stream_id];  // 占位，让 provider 的指针立刻有效
  return nghttp2_submit_headers(impl_->session, NGHTTP2_FLAG_NONE, stream_id,
                                nullptr, nv.data(), nv.size(), nullptr);
}

int uvcpp_h2_session::submit_data(int32_t stream_id, const char* data,
                                  size_t len, bool end_stream,
                                  std::function<void(int)> done) {
  if (!impl_->session) return UV_EINVAL;
  auto sit = impl_->streams.find(stream_id);
  if (sit == impl_->streams.end()) return UV_EINVAL;
  if (sit->second.state != h2_stream_state::HEADERS_SENT) return UV_EALREADY;

  impl::out_stream& os = impl_->out_streams[stream_id];
  // 已经宣告过 END_STREAM，不能再补字节。
  //
  // 判据必须**同时**看"收尾那块还在队里"这一条：`os.ended` 是 `retire_chunk`
  // 里落的，也就是那块**发出去之后**才为真。只看它的话，在"收尾块已入队、还没
  // 发出去"这个窗口里再提交一块是允许的 —— 而 provider 取到收尾块就置 EOF 收工，
  // 后面那块永远没人来读，它的 `done` 也就永远不跑。调用方把整条流的收尾挂在
  // 那个 `done` 上（见 `retire_chunk` 的注释），于是流式响应永久挂起。
  if (os.ended) return UV_EALREADY;
  if (!os.chunks.empty() && os.chunks.back().end_stream) return UV_EALREADY;

  // 待发队列的字节上界。**它是最后一道拒绝，不是水位** —— 框架那套 1 MiB 的软水位
  // （`uvcpp_web_response::pending_bytes_`）先起作用，走到这里说明调用方压根没看
  // `done`。零字节的提交（终止块）**永不**因它被拒，否则一条流永远收不了尾。
  //
  // 位置必须在 `nghttp2_submit_data2` **之前**：契约是"非 0 ⇒ 什么都没发生"。
  // 一旦 frame 提了而块没入队，那条 data frame 就会去读一个空队列并把 provider
  // 挂成 DEFERRED —— 正是下面那段注释在防的事。
  if (len > 0 && os.queued_bytes + len > impl_->max_out_stream) return UV_ENOBUFS;

  // **先提交 frame，再入队。** 反过来的话，提交失败就把一块（连同它的 `done`）
  // 留在了队首 —— 没有 frame 会来读它，那个 `done` 于是永远不跑，而契约是
  // **恰好一次**（`stream_write` 的调用方把整条流的收尾挂在它上面）。在这里
  // 当场结算也不行：本函数的调用方（`uvcpp_http_server::write_stream`）拿到
  // 非 0 会**自己**按失败结算那一块，两边都结就是同一个 `done` 跑两次。
  //
  // 所以让"块在队列里"与"frame 在飞"严格同生共死：提交不接受，块压根不入队，
  // 形参 `done` 随本函数返回一起析构，调用方那边的返回值语义（非 0 = 未受理，
  // `done` 不会被调）原样成立。
  //
  // 这个顺序是安全的：`nghttp2_submit_data2` 只是把出站项排进队列，provider
  // 要等到 `mem_send`（也就是随后那次 `flush()`）才会被调，中间没有人读队列。
  if (!os.frame_pending) {
    nghttp2_data_provider2 prd;
    prd.source.ptr    = &os;
    prd.read_callback = &impl::cb_read_stream;
    // `NGHTTP2_FLAG_END_STREAM` 必须在这里给（nghttp2.h：给了它，"**最后一个**
    // DATA 帧"才带 END_STREAM）。挂在这里而不是提交时按需给，是因为提交发生在
    // 第一块数据到的时候 —— 那时根本还不知道这条流会不会收尾。
    //
    // 给了它不会让中间那些帧也带上 END_STREAM：`nghttp2_session_pack_data` 只在
    // provider 置了 EOF 的那一次才把它写进 `frame->hd.flags`。所以这个 flag 的
    // 语义是"允许收尾"，真正收尾仍由 `end_stream` 那一块（`cb_read_stream` 里
    // 置 EOF）决定。
    //
    // 少了它症状很隐蔽：帧一个不少地在线上跑，客户端也照常收到 body，只是
    // **永远等不到 END_STREAM** —— 表现为流式响应挂到超时，而报文肉眼看不出问题。
    const int rv = nghttp2_submit_data2(impl_->session, NGHTTP2_FLAG_END_STREAM,
                                        stream_id, &prd);
    if (rv != 0) return rv;
    os.frame_pending = true;
  }

  // 队空过 ⇒ provider 已把这条 data frame 挂进 nghttp2 的延迟队列，得显式叫醒
  // （`cb_read_stream` 的注释里写了为什么只能用这个机制）。
  //
  // 放在入队**之前**：`resume` 只是把出站项重新排进队列，provider 要等到下一次
  // `mem_send` 才被调，中间没人读队列 —— 所以"先叫醒、再放数据"是安全的；反过来
  // 一旦 `resume` 失败，这块就留在了一条没有 frame 来读它的队里，那个 `done`
  // 永远不跑，正好是上面那段注释在防的事。
  if (os.deferred) {
    const int rrv = nghttp2_session_resume_data(impl_->session, stream_id);
    if (rrv != 0) return rrv;
    os.deferred = false;
  }

  impl::out_chunk c;
  if (data != nullptr && len > 0) c.data.assign(data, len);
  c.end_stream = end_stream;
  c.done       = std::move(done);
  // 入账与 `cb_read_stream` 里的出账**必须成对** —— 这是那道上界的唯一输入。
  os.queued_bytes += c.data.size();
  os.chunks.push_back(std::move(c));
  return 0;  // 有 frame 在飞（刚提的，或者之前那个），它会回头把这块取走
}

void uvcpp_h2_session::take_completed(std::vector<std::function<void()>>& out) {
  out.swap(impl_->completed);
}

void uvcpp_h2_session::cancel_pending_out() {
  if (!impl_) return;
  // 先收键再逐条作废：`cancel_stream_out()` 会 erase，边遍历边删是另一回事。
  std::vector<int32_t> ids;
  ids.reserve(impl_->out_streams.size());
  for (auto it = impl_->out_streams.begin(); it != impl_->out_streams.end();
       ++it) {
    ids.push_back(it->first);
  }
  for (size_t i = 0; i < ids.size(); ++i) {
    impl_->cancel_stream_out(ids[i], UV_ECANCELED);
  }
}

int uvcpp_h2_session::submit_status(int32_t stream_id, int status,
                                    const std::string& body) {
  if (!impl_->session) return UV_EINVAL;
  auto sit = impl_->streams.find(stream_id);
  if (sit == impl_->streams.end()) return UV_EINVAL;
  // 一次提交 = 一条流只发一套响应。`HEADERS_SENT`（流式那条路的头部已经发过）
  // 也必须挡在这里，否则会往同一条流上再发一套 HEADERS。
  if (sit->second.state != h2_stream_state::OPEN) return UV_EALREADY;
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

  // 对端已经发过 GOAWAY（或本端流号用尽、会话已在收口）时**不能**再开流。
  //
  // 不做这道检查的后果不是"提交失败"，而是**提交成功**：nghttp2 会照发一个
  // 流号（实测返回 3），把这条 HEADERS 推进 `ob_syn`，然后在打帧那一步才发现
  // 开不了流 —— 于是帧被丢掉、流被**以 `REFUSED_STREAM` 关掉**，调用方拿到的
  // 是一个正数流号、零个字节上线、外加一个事后才到的 `on_close`。
  // `nghttp2_session_check_request_allowed()` 的文档把两条路都写明，这是那条
  // "提前问"的路。`UV_ENOTCONN` 与 `UV_EINVAL`/`UV_EMSGSIZE` 同属"同步拒绝、
  // 什么都没发生"。
  if (!nghttp2_session_check_request_allowed(impl_->session)) return UV_ENOTCONN;

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

  // 发不出去的头部块必须在这里退掉，理由见 `header_block_fits()`：nghttp2 会
  // 静默丢掉这一帧，而那时我们已经把一个正的流号还给调用方了。
  if (!header_block_fits(nv)) return UV_EMSGSIZE;

  // data_prd 必须在**这一次**提交里给出：`submit_request2` 收到 NULL 时
  // HEADERS 自带 END_STREAM，之后再 `submit_data2` 就是对一条已结束的流发数据。
  // 而 provider 又需要一个活到发完的地址，所以先在堆上备好，拿到 id 再登记。
  std::unique_ptr<impl::out_body> pending;
  nghttp2_data_provider2          prd;
  const nghttp2_data_provider2*   prd_ptr = nullptr;
  if (!body.empty()) {
    pending.reset(new impl::out_body());
    pending->data     = std::move(body);
    prd.source.ptr    = pending.get();
    prd.read_callback = &impl::cb_read_body;
    prd_ptr           = &prd;
  }

  const int32_t sid = impl_->finish_submit_request(nghttp2_submit_request2(
      impl_->session, nullptr, nv.data(), nv.size(), prd_ptr, nullptr));
  if (sid < 0) return sid;
  if (pending.get() != nullptr) impl_->out_bodies[sid] = std::move(pending);

  // **两条路都登记**这条流。不登记的话它后续的 `on_stream_close` 会在自己的
  // `streams.find()` 那一步静默返回，`cbs.on_close` 一声不吭 —— 而对端一个
  // RST_STREAM 打过来时，`uvcpp_http_client` 正是靠这个回调给挂起的请求结算
  // （`on_h2_stream_close`）。空 body 那条路原先就是漏的，而它恰恰是 GET 的
  // 常态：服务端拒掉一条 GET，调用方的回调就永远不来，两边一起等。
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
  // `last_stream_id` 是"我可能已经处理过的最后一条流"，不是随便填的 0。
  // 填 0 等于告诉对端"我一条都没处理"，于是对端把**所有**在飞的流都当成可
  // 重试的 —— 包括我们这边副作用已经跑过的那些，重试就是重复副作用。
  // `nghttp2_session_get_last_proc_stream_id()` 就是干这个的（文档原话：这个
  // 返回值可以直接当 `nghttp2_submit_goaway()` 的 `last_stream_id`）。
  return nghttp2_submit_goaway(
      impl_->session, NGHTTP2_FLAG_NONE,
      nghttp2_session_get_last_proc_stream_id(impl_->session), error_code,
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

bool uvcpp_h2_session::peer_goaway_received() const {
  return impl_->peer_goaway;
}

uint32_t uvcpp_h2_session::peer_goaway_error_code() const {
  return impl_->peer_goaway_code;
}

int32_t uvcpp_h2_session::peer_goaway_last_stream_id() const {
  return impl_->peer_goaway_last;
}

uvcpp_h2_stream* uvcpp_h2_session::find_stream(int32_t stream_id) {
  auto it = impl_->streams.find(stream_id);
  return it == impl_->streams.end() ? nullptr : &it->second;
}

size_t uvcpp_h2_session::stream_count() const { return impl_->streams.size(); }

int uvcpp_h2_session::last_error() const { return impl_->last_error; }

// =========================================================================
// 收方向背压
// =========================================================================

int uvcpp_h2_session::pause_stream(int32_t stream_id) {
  auto it = impl_->streams.find(stream_id);
  if (it == impl_->streams.end()) return UV_EINVAL;
  // 幂等：已在暂停中的流再暂停一次**不是**错误 —— 它只是继续欠账。
  it->second.paused = true;
  return 0;
}

int uvcpp_h2_session::resume_stream(int32_t stream_id) {
  auto it = impl_->streams.find(stream_id);
  if (it == impl_->streams.end()) return UV_EINVAL;
  uvcpp_h2_stream& s = it->second;
  s.paused           = false;

  const size_t owed = s.paused_owed;
  s.paused_owed     = 0;
  if (owed == 0 || !impl_->session) return 0;

  // 已经决定要扔掉的流**不还** —— 还了等于告诉对端"继续发"，紧接着我们就要 RST
  // 它。这是 `on_data_chunk` 顶部那条不变式的第三处落地（另两处是它的两条早退）：
  // 进 `on_data_chunk` 的每个字节，连接级恰好还一次，流级只在"这条流还在正常收"
  // 时才还。
  if (s.rejected) return 0;

  // 一次还清。`consume_stream` 对**已经关掉的**流是静默空操作（返回 0，见
  // `nghttp2_session.c:8005-8007`），所以这里不必先查流还在不在。非 0 只可能是
  // NOMEM / 流号 0 这类调用方错误；原样透传，不在这里吞。
  return nghttp2_session_consume_stream(impl_->session, stream_id, owed);
}

void uvcpp_h2_session::set_max_out_stream_bytes(size_t n) { impl_->max_out_stream = n; }

size_t uvcpp_h2_session::max_out_stream_bytes() const { return impl_->max_out_stream; }

int32_t uvcpp_h2_session::peer_window_size(int32_t stream_id) const {
  if (!impl_->session) return 0;
  const int32_t w =
      nghttp2_session_get_stream_remote_window_size(impl_->session, stream_id);
  // nghttp2 用 -1 表示"没这条流"。本访问器的口径是 0 —— 于是"没有这条流"与
  // "窗口恰好用尽"同形，想区分先用 `find_stream()`。
  return w < 0 ? 0 : w;
}

}  // namespace uvcpp

#endif  // UVCPP_NGHTTP2_ENABLE
