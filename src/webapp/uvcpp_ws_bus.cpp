/**
 * @file src/webapp/uvcpp_ws_bus.cpp
 * @brief 跨进程 WebSocket 广播桥的实现（Redis pub/sub，issue #74）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_ws_bus.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <redis/uvcpp_redis.h>
#include <webapp/uvcpp_ws_rooms.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <utility>

namespace uvcpp {

uvcpp_ws_bus_options::uvcpp_ws_bus_options()
    : deliver_loopback(false),
      reconnect_ms(1000),
      reconnect_max_ms(30000),
      max_pending(4096),
      max_payload_bytes(8u * 1024u * 1024u) {
  channel_prefix = "uvcpp:ws:";
}

uvcpp_ws_bus_stats::uvcpp_ws_bus_stats()
    : published(0),
      received(0),
      delivered(0),
      loopback_skipped(0),
      dropped(0),
      send_failures(0),
      reconnects(0),
      connected(false),
      degraded(false) {}

namespace {

/// 信封：`node_len(4, BE) | node | seq(8, BE) | payload`。
std::string encode_envelope(const std::string& node, uint64_t seq,
                            const std::string& payload) {
  std::string out;
  out.reserve(4 + node.size() + 8 + payload.size());
  const uint32_t n = static_cast<uint32_t>(node.size());
  out.push_back(static_cast<char>((n >> 24) & 0xFF));
  out.push_back(static_cast<char>((n >> 16) & 0xFF));
  out.push_back(static_cast<char>((n >> 8) & 0xFF));
  out.push_back(static_cast<char>(n & 0xFF));
  out.append(node);
  for (int i = 7; i >= 0; --i) {
    out.push_back(static_cast<char>((seq >> (i * 8)) & 0xFF));
  }
  out.append(payload);
  return out;
}

bool decode_envelope(const std::string& in, std::string& node, uint64_t& seq,
                     std::string& payload) {
  if (in.size() < 12) return false;
  const uint32_t n = (static_cast<uint32_t>(
                         static_cast<unsigned char>(in[0]))
                      << 24) |
                     (static_cast<uint32_t>(
                          static_cast<unsigned char>(in[1]))
                      << 16) |
                     (static_cast<uint32_t>(
                          static_cast<unsigned char>(in[2]))
                      << 8) |
                     static_cast<uint32_t>(static_cast<unsigned char>(in[3]));
  if (in.size() < 4u + n + 8u) return false;
  node.assign(in, 4, n);
  seq = 0;
  for (size_t i = 0; i < 8; ++i) {
    seq = (seq << 8) | static_cast<unsigned char>(in[4 + n + i]);
  }
  payload.assign(in, 4 + n + 8, std::string::npos);
  return true;
}

std::string make_node_id() {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "n%ld-%llx",
                static_cast<long>(uv_os_getpid()),
                static_cast<unsigned long long>(uv_hrtime() & 0xFFFFFFFFull));
  return std::string(buf);
}

}  // namespace

// =========================================================================
// impl
// =========================================================================

struct uvcpp_ws_bus::impl : std::enable_shared_from_this<uvcpp_ws_bus::impl> {
  enum state_t { IDLE, WAITING, CONNECTING, SUBSCRIBING, READY };

  impl(uvcpp_loop* l, const uvcpp_ws_bus_options& o)
      : loop(l),
        opts(o),
        node_id(o.node_id.empty() ? make_node_id() : o.node_id),
        state(IDLE),
        started(false),
        backoff(o.reconnect_ms > 0 ? o.reconnect_ms : 1),
        seq(0),
        ever_connected(false),
        attempt(0),
        pub_ready(false),
        sub_ready(false),
        rooms(nullptr) {}

  ~impl() {
    if (timer) timer->stop();
    if (retire_timer) retire_timer->stop();
    if (!retired.empty() && loop != nullptr && !loop->is_running()) {
      // 循环不在跑（析构发生在普通代码里）：自己把关闭收尾拨完，否则那些
      // `uv_close` 的完成回调永远不会来，而包装对象马上就被释放。
      for (int i = 0; i < 32 && !retired.empty(); ++i) {
        loop->run(UV_RUN_NOWAIT);
      }
    }
    retired.clear();
  }

  uvcpp_loop*                       loop;
  uvcpp_ws_bus_options              opts;
  std::string                       node_id;
  std::string                       url;
  state_t                           state;
  bool                              started;
  int                               backoff;
  uint64_t                          seq;
  bool                              ever_connected;
  /// 每次 `begin_connect()` 加一。异步回调带着自己那一代进来，对不上就丢弃 ——
  /// 否则"发布连接失败、订阅连接随后成功"会让状态机卡在 SUBSCRIBING 里等一个
  /// 永远不会来的 `maybe_ready()`（那一半已经被退役了）。
  uint64_t                          attempt;

  // **订阅与发布必须用两条连接**：RESP2 下一条连接一旦进入订阅模式，Redis 只
  // 接受 (P)SUBSCRIBE/UNSUBSCRIBE/PING/QUIT/RESET，PUBLISH 会被回一条 `-ERR`。
  // 用同一条连接的两面派做法在redis-cli里看得见，但在本层只会表现为"消息发不
  // 出去、对端收不到"。
  std::unique_ptr<uvcpp_redis_client> sub_cli;
  std::unique_ptr<uvcpp_redis_client> pub_cli;
  bool                                pub_ready;
  bool                                sub_ready;
  std::unique_ptr<uvcpp_timer>        timer;

  // **退役不是立刻 delete**：`uvcpp_redis_client::close()` 只发起 `uv_close`，
  // 完成回调要等到本轮 `uv_run` 的收尾阶段；而 `uvcpp_tcp_client` 的析构里
  // 那条"句柄正在关"的分支会告警并把包装对象留在循环里，而 `tcp_client::close`
  // 注册的完成回调捕获的是 **tcp_client 的 this** —— 包装对象一释放就是悬垂。
  // 所以 close() 之后先把客户端挪进 `retired`，由下一轮循环里的一次性定时器
  // 释放；那时关闭完成回调已经跑过。
  std::vector<std::unique_ptr<uvcpp_redis_client> > retired;
  std::unique_ptr<uvcpp_timer>                      retire_timer;

  std::deque<std::pair<std::string, std::string> > pending;  // room, payload

  std::function<void(const std::string&, const std::string&)> on_message;
  uvcpp_ws_rooms*                                             rooms;

  mutable uvcpp_ws_bus_stats st;

  // ---- 定时器 --------------------------------------------------------

  void arm_timer(int ms) {
    if (!started) return;
    if (ms <= 0) ms = 1;
    if (!timer) timer.reset(new uvcpp_timer(loop));
    std::weak_ptr<impl> w = shared_from_this();
    timer->start(
        [w](uvcpp_timer*) {
          std::shared_ptr<impl> s = w.lock();
          if (!s) return;
          s->on_tick();
        },
        static_cast<uint64_t>(ms), 0);
  }

  void on_tick() {
    if (!started) return;
    if (state == READY) {
      if (pub_cli && sub_cli && pub_cli->is_connected() &&
          sub_cli->is_connected()) {
        arm_timer(opts.reconnect_ms);
      } else {
        connected_lost("与 Redis 的连接已断开");
      }
      return;
    }
    if (state == WAITING) {
      begin_connect();
      return;
    }
    // CONNECTING / SUBSCRIBING：连接与订阅都有自己的超时，这里只是看门狗。
    arm_timer(opts.reconnect_ms);
  }

  // ---- 连接 / 订阅 ---------------------------------------------------

  void maybe_ready() {
    if (state != SUBSCRIBING || !pub_ready || !sub_ready) return;
    state = READY;
    st.connected = true;
    st.degraded  = false;
    st.last_error.clear();
    if (ever_connected) {
      ++st.reconnects;
    } else {
      ever_connected = true;
    }
    backoff = opts.reconnect_ms > 0 ? opts.reconnect_ms : 1;
    flush_pending();
    arm_timer(opts.reconnect_ms);
  }

  void begin_connect() {
    state     = CONNECTING;
    ++attempt;
    const uint64_t gen_now = attempt;
    pub_ready = false;
    sub_ready = false;
    pub_cli.reset(new uvcpp_redis_client(loop));
    sub_cli.reset(new uvcpp_redis_client(loop));

    std::weak_ptr<impl> w = shared_from_this();
    const int prc = pub_cli->connect(url, [w, gen_now](int status) {
      std::shared_ptr<impl> s = w.lock();
      if (!s || !s->started || s->attempt != gen_now) return;
      if (s->state != CONNECTING && s->state != SUBSCRIBING) return;
      if (status != 0) {
        s->fail_backoff("连接 Redis（发布连接）失败");
        return;
      }
      s->pub_ready = true;
      s->maybe_ready();
    });
    if (prc != 0) {
      fail_backoff("连接 Redis（发布连接）失败");
      return;
    }

    const int src = sub_cli->connect(url, [w, gen_now](int status) {
      std::shared_ptr<impl> s = w.lock();
      if (!s || !s->started || s->attempt != gen_now) return;
      if (s->state != CONNECTING) return;
      if (status != 0) {
        s->fail_backoff("连接 Redis（订阅连接）失败");
        return;
      }
      s->state = SUBSCRIBING;
      std::weak_ptr<impl> w2 = s;
      const std::vector<std::string> patterns(1, s->opts.channel_prefix + "*");
      const int pc = s->sub_cli->psubscribe(
          patterns,
          [w2, gen_now](const std::string& channel,
                        const uvcpp_redis_value& message) {
            std::shared_ptr<impl> s2 = w2.lock();
            if (!s2 || !s2->started || s2->attempt != gen_now) return;
            s2->on_remote(channel, message);
          },
          [w2, gen_now](int sub_status) {
            std::shared_ptr<impl> s2 = w2.lock();
            if (!s2 || !s2->started || s2->attempt != gen_now) return;
            if (s2->state != SUBSCRIBING || !s2->sub_cli) return;
            if (sub_status != 0) {
              s2->fail_backoff("订阅 Redis 频道失败");
              return;
            }
            s2->sub_ready = true;
            s2->maybe_ready();
          });
      if (pc != 0) s->fail_backoff("订阅 Redis 频道失败");
    });
    if (src != 0) fail_backoff("连接 Redis（订阅连接）失败");
  }

  void retire(std::unique_ptr<uvcpp_redis_client>& c) {
    if (!c) return;
    c->close();
    retired.push_back(std::move(c));
  }

  void arm_retire_timer() {
    if (retired.empty()) return;
    if (!retire_timer) retire_timer.reset(new uvcpp_timer(loop));
    std::weak_ptr<impl> w = shared_from_this();
    retire_timer->start(
        [w](uvcpp_timer*) {
          std::shared_ptr<impl> s = w.lock();
          if (!s) return;
          // 上一轮 `uv_run` 的收尾阶段已经跑完所有 `uv_close` 的完成回调，
          // 现在释放包装对象是安全的。
          s->retired.clear();
        },
        0, 0);
  }

  void connected_lost(const char* why) {
    st.connected = false;
    st.degraded = true;
    st.last_error = why;
    state = WAITING;
    pub_ready = false;
    sub_ready = false;
    retire(sub_cli);
    retire(pub_cli);
    arm_retire_timer();
    arm_timer(backoff);
    // 括号不能省：`<windows.h>` 把 `min`/`max` 定义成宏，`std::min(` 会被
    // 展开成一段非法记号（MSVC C2589/C2059），GCC/MinGW 那条腿恰好没走到这个
    // 宏定义上。`(std::min)(...)` 让预处理器看不见函数名后面的 `(`。
    backoff = (std::min)(backoff * 2, (std::max)(opts.reconnect_max_ms, 1));
  }

  void fail_backoff(const char* why) {
    connected_lost(why);
    if (st.last_error.empty()) st.last_error = why;
  }

  // ---- 收发 ----------------------------------------------------------

  bool publish_now(const std::string& room, const std::string& payload) {
    if (!pub_cli || !pub_cli->is_connected()) return false;
    const std::string env = encode_envelope(node_id, ++seq, payload);
    const std::string ch  = opts.channel_prefix + room;
    std::weak_ptr<impl> w = shared_from_this();
    const int rc = pub_cli->command(
        std::vector<std::string>{"PUBLISH", ch, env},
        [w](int status, const uvcpp_redis_value& reply) {
          std::shared_ptr<impl> s = w.lock();
          if (!s) return;
          // **服务器错误也是 status == 0**（见 `uvcpp_redis_client` 的说明），
          // 必须再看一眼 reply 是不是 ERROR —— PUBLISH 在订阅连接上被拒就是
          // 这一类，漏掉它会表现为"发出去了、没人收到"。
          if (status != 0 || reply.is_error()) {
            ++s->st.send_failures;
            ++s->st.dropped;  // PUBLISH 没有确认路径：失败即丢
            if (reply.is_error() && s->st.last_error.empty()) {
              s->st.last_error = reply.as_string();
            }
          }
        });
    if (rc != 0) {
      ++st.send_failures;
      return false;
    }
    return true;
  }

  void flush_pending() {
    while (!pending.empty() && state == READY && pub_cli &&
           pub_cli->is_connected()) {
      std::pair<std::string, std::string> item = std::move(pending.front());
      if (!publish_now(item.first, item.second)) break;
      pending.pop_front();
    }
  }

  void on_remote(const std::string& channel, const uvcpp_redis_value& msg) {
    ++st.received;
    const std::string& prefix = opts.channel_prefix;
    if (channel.size() <= prefix.size() ||
        channel.compare(0, prefix.size(), prefix) != 0) {
      ++st.dropped;
      return;
    }
    const std::string room = channel.substr(prefix.size());

    std::string node, payload;
    uint64_t    seq_in = 0;
    const std::string raw = msg.as_string();
    if (!decode_envelope(raw, node, seq_in, payload)) {
      ++st.dropped;
      return;
    }
    if (node == node_id && !opts.deliver_loopback) {
      ++st.loopback_skipped;
      return;
    }
    ++st.delivered;
    if (on_message) on_message(room, payload);
    if (rooms != nullptr) {
      rooms->broadcast_text(room, payload.data(), payload.size());
    }
  }

  // ---- 生命周期 ------------------------------------------------------

  int start(const std::string& redis_url) {
    if (started) return 0;
    started = true;
    url      = redis_url;
    backoff  = opts.reconnect_ms > 0 ? opts.reconnect_ms : 1;
    st.degraded = true;
    begin_connect();
    return 0;
  }

  void stop() {
    started = false;
    state   = IDLE;
    if (timer) timer->stop();
    retire(sub_cli);
    retire(pub_cli);
    arm_retire_timer();
    pending.clear();
    rooms = nullptr;
    on_message = nullptr;
    st.connected = false;
    st.degraded  = false;
  }
};

// =========================================================================
// 公开面
// =========================================================================

uvcpp_ws_bus::uvcpp_ws_bus(uvcpp_loop* loop,
                           const uvcpp_ws_bus_options& opts)
    : p_(new impl(loop, opts)) {}

uvcpp_ws_bus::~uvcpp_ws_bus() {
  if (p_) p_->stop();
}

int uvcpp_ws_bus::start(const std::string& redis_url) {
  return p_->start(redis_url);
}

int uvcpp_ws_bus::publish(const std::string& room,
                          const std::string& payload) {
  if (room.empty()) return UV_EINVAL;
  if (p_->opts.max_payload_bytes > 0 &&
      payload.size() > p_->opts.max_payload_bytes) {
    ++p_->st.dropped;
    return UV_EMSGSIZE;
  }
  if (p_->state == impl::READY && p_->pub_cli &&
      p_->pub_cli->is_connected()) {
    if (p_->publish_now(room, payload)) {
      ++p_->st.published;
      return 0;
    }
  }
  // 降级：先排队，连上之后补发。
  if (p_->pending.size() >= p_->opts.max_pending) {
    ++p_->st.dropped;
    return UV_ENOBUFS;
  }
  p_->pending.push_back(std::make_pair(room, payload));
  ++p_->st.published;
  p_->st.degraded = true;
  return 0;
}

void uvcpp_ws_bus::set_on_message(
    std::function<void(const std::string&, const std::string&)> cb) {
  p_->on_message = std::move(cb);
}

void uvcpp_ws_bus::attach(uvcpp_ws_rooms* rooms) { p_->rooms = rooms; }

size_t uvcpp_ws_bus::broadcast(uvcpp_ws_rooms& rooms, const std::string& room,
                               const char* data, size_t len,
                               const uvcpp_ws_broadcast_options& opts) {
  const size_t local = rooms.broadcast_text(room, data, len, opts);
  // 远端投递是 best-effort（pub/sub 没有确认），失败只进统计。
  publish(room, std::string(data != nullptr && len > 0 ? data : "", len));
  return local;
}

void uvcpp_ws_bus::stop() { p_->stop(); }

void uvcpp_ws_bus::drop_connection() {
  if (!p_->started) return;
  p_->connected_lost("测试/运维触发的断开");
}

bool uvcpp_ws_bus::connected() const {
  return p_->state == impl::READY && p_->pub_cli && p_->sub_cli &&
         p_->pub_cli->is_connected() && p_->sub_cli->is_connected();
}

uvcpp_ws_bus_stats uvcpp_ws_bus::snapshot() const {
  uvcpp_ws_bus_stats out = p_->st;
  out.connected = connected();
  out.degraded  = !out.connected;
  return out;
}

const std::string& uvcpp_ws_bus::node_id() const { return p_->node_id; }

uvcpp_loop* uvcpp_ws_bus::loop() const { return p_->loop; }

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE
