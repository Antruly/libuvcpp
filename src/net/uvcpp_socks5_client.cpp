/**
 * @file src/net/uvcpp_socks5_client.cpp
 * @brief 独立 SOCKS5 客户端（CONNECT / BIND / UDP ASSOCIATE）的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "net/uvcpp_socks5_client.h"

#include <net/uvcpp_socks5_common.h>
#include <net/uvcpp_socks5_udp.h>
#include <net/uvcpp_tcp_client.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <uvcpp/uvcpp_buf.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace uvcpp {

namespace {

/** @brief BIND 第二次应答（对端连入）的等待上限：120 s（与常见实现一致）。 */
const int kBindPeerTimeoutMs = 120000;

/** @brief 握手之后、消费者出现之前最多攒多少应用字节。 */
const std::size_t kMaxStashBytes = 2 * 1024 * 1024;

bool wildcard_host(const std::string& host) {
  return host.empty() || host == "0.0.0.0" || host == "::";
}

}  // namespace

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_socks5_client::uvcpp_socks5_client()
    : tcp_(nullptr),
      udp_(nullptr),
      loop_(new uvcpp_loop()),
      owns_loop_(true),
      tcp_closed_(false),
      timeout_timer_(nullptr),
      timeout_in_cb_(false),
      close_observer_id_(0),
      connect_timeout_ms_(30000),
      phase_(phase::IDLE),
      op_(op::NONE),
      sync_(false),
      op_done_(false),
      op_status_(0),
      peer_done_(false),
      peer_status_(0),
      peer_sync_(false),
      last_error_code_(0) {
  tcp_ = new uvcpp_tcp_client(loop_);
  close_observer_id_ =
      tcp_->add_close_observer([this]() { on_control_closed(); });
}

uvcpp_socks5_client::uvcpp_socks5_client(uvcpp_loop* loop)
    : tcp_(nullptr),
      udp_(nullptr),
      loop_(loop),
      owns_loop_(false),
      tcp_closed_(false),
      timeout_timer_(nullptr),
      timeout_in_cb_(false),
      close_observer_id_(0),
      connect_timeout_ms_(30000),
      phase_(phase::IDLE),
      op_(op::NONE),
      sync_(false),
      op_done_(false),
      op_status_(0),
      peer_done_(false),
      peer_status_(0),
      peer_sync_(false),
      last_error_code_(0) {
  tcp_ = new uvcpp_tcp_client(loop_);
  close_observer_id_ =
      tcp_->add_close_observer([this]() { on_control_closed(); });
}

uvcpp_socks5_client::~uvcpp_socks5_client() {
  if (alive_token_) *alive_token_ = 1;

  if (tcp_ != nullptr && close_observer_id_ != 0) {
    tcp_->remove_close_observer(close_observer_id_);
    close_observer_id_ = 0;
  }
  destroy_timer();
  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }
  if (tcp_ != nullptr) {
    delete tcp_;
    tcp_ = nullptr;
  }
  // 自建循环这条路：句柄全关完之后才可能退干净（与 udp/tcp 客户端同一套）。
  if (owns_loop_ && loop_ != nullptr) {
    for (int i = 0; i < 256 && loop_->loop_alive() != 0; ++i) {
      loop_->run(UV_RUN_NOWAIT);
    }
    loop_->loop_close();
    delete loop_;
    loop_ = nullptr;
  }
}

std::shared_ptr<char> uvcpp_socks5_client::alive_token() {
  if (!alive_token_) alive_token_.reset(new char(0));
  return alive_token_;
}

bool uvcpp_socks5_client::token_alive(const std::shared_ptr<char>& token) {
  return token && *token == 0;
}

uvcpp_loop* uvcpp_socks5_client::get_loop() { return loop_; }
uvcpp_tcp_client* uvcpp_socks5_client::stream() { return tcp_; }
uvcpp_socks5_udp* uvcpp_socks5_client::udp() { return udp_; }

bool uvcpp_socks5_client::is_connected() const {
  return phase_ == phase::READY;
}

const std::string& uvcpp_socks5_client::last_error() const {
  return last_error_;
}

int uvcpp_socks5_client::last_error_code() const { return last_error_code_; }

void uvcpp_socks5_client::set_credentials(const std::string& username,
                                          const std::string& password) {
  username_ = username;
  password_ = password;
}

void uvcpp_socks5_client::set_connect_timeout_ms(int ms) {
  connect_timeout_ms_ = ms;
}

// =========================================================================
// 定时器
// =========================================================================

void uvcpp_socks5_client::arm_timeout(int ms) {
  disarm_timeout();
  if (ms <= 0 || loop_ == nullptr) return;
  if (timeout_timer_ == nullptr) timeout_timer_ = new uvcpp_timer(loop_);

  // 令牌按值捕进来：定时器回调跑的时候对象可能已经没了（用户在上一个回调
  // 里删了客户端，而这个定时器属于同一轮）。对象没了就只停，不碰 `this`。
  std::shared_ptr<char> life = alive_token();
  timeout_timer_->start(
      [this, life](uvcpp_timer* t) {
        t->stop();
        if (!token_alive(life)) return;
        on_timeout();
      },
      static_cast<uint64_t>(ms), 0);
}

void uvcpp_socks5_client::disarm_timeout() {
  if (timeout_timer_ != nullptr) timeout_timer_->stop();
}

void uvcpp_socks5_client::destroy_timer() {
  if (timeout_timer_ == nullptr) return;
  timeout_timer_->stop();
  // **绝不在定时器自己的回调里删它** —— 那等于析构正在执行的那个闭包。
  // 这条路上（超时回调 → 用户回调 → 删客户端）只能留下一个定时器对象，
  // 与 tcp 客户端里 `tls_hs_timer_` 的处置同一条规则。
  if (!timeout_in_cb_) {
    delete timeout_timer_;
  }
  timeout_timer_ = nullptr;
}

void uvcpp_socks5_client::on_timeout() {
  timeout_in_cb_ = true;
  fail(UV_ETIMEDOUT, "SOCKS5 operation timed out", true);
  timeout_in_cb_ = false;
}

// =========================================================================
// 连接
// =========================================================================

void uvcpp_socks5_client::recreate_tcp() {
  if (tcp_ != nullptr) {
    if (close_observer_id_ != 0) {
      tcp_->remove_close_observer(close_observer_id_);
      close_observer_id_ = 0;
    }
    delete tcp_;
  }
  tcp_ = new uvcpp_tcp_client(loop_);
  close_observer_id_ =
      tcp_->add_close_observer([this]() { on_control_closed(); });
  tcp_closed_ = false;
}

void uvcpp_socks5_client::reset_for_new_connect() {
  disarm_timeout();
  in_.clear();
  stash_.clear();
  op_          = op::NONE;
  op_done_     = false;
  op_status_   = 0;
  op_cb_       = nullptr;
  op_ep_cb_    = nullptr;
  peer_cb_     = nullptr;
  peer_done_   = false;
  peer_status_ = 0;
  peer_sync_   = false;
  phase_       = phase::IDLE;
  last_error_.clear();
  last_error_code_ = 0;
  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }
  if (tcp_closed_ || tcp_ == nullptr) recreate_tcp();
}

int uvcpp_socks5_client::start_connect(const std::string& host, int port) {
  if (host.empty() || port <= 0 || port > 65535) {
    last_error_      = "invalid SOCKS5 proxy address";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  phase_ = phase::CONNECTING;
  op_    = op::PROXY;
  arm_timeout(connect_timeout_ms_);

  const int rc = tcp_->connect(host.c_str(), port, [this](int status) {
    on_tcp_connected(status);
  });
  if (rc != 0) {
    phase_ = phase::IDLE;
    op_    = op::NONE;
    disarm_timeout();
    last_error_      = uv_strerror(rc);
    last_error_code_ = rc;
  }
  return rc;
}

int uvcpp_socks5_client::connect(const std::string& proxy_host, int proxy_port,
                                 std::function<void(int)> cb) {
  reset_for_new_connect();
  if (cb == nullptr) return connect_wait(proxy_host, proxy_port);

  op_cb_ = std::move(cb);
  sync_  = false;
  const int rc = start_connect(proxy_host, proxy_port);
  if (rc != 0) op_cb_ = nullptr;
  return rc;
}

int uvcpp_socks5_client::connect_wait(const std::string& proxy_host,
                                      int proxy_port, int timeout_ms) {
  reset_for_new_connect();
  op_done_ = false;
  sync_    = true;
  const int rc = start_connect(proxy_host, proxy_port);
  if (rc != 0) {
    sync_ = false;
    return rc;
  }
  // 显式传入的 timeout_ms 覆盖 `set_connect_timeout_ms()` 那个默认值。
  arm_timeout(timeout_ms);
  return run_sync_op(timeout_ms);
}

void uvcpp_socks5_client::on_tcp_connected(int status) {
  if (phase_ != phase::CONNECTING) return;
  if (status != 0) {
    fail(status, uv_strerror(status), true);
    return;
  }

  const int rc = tcp_->read_start([this](uvcpp_buf* b) { on_read(b); });
  if (rc != 0) {
    fail(rc, "cannot start reading from the SOCKS5 control connection", true);
    return;
  }

  phase_ = phase::GREETING;
  // 只提供**一种**方法（与 tcp 客户端同一条规则，见那里对"静默降级"的说明）。
  std::vector<std::uint8_t> methods;
  methods.push_back(static_cast<std::uint8_t>(
      username_.empty() ? uvcpp_socks5_method::NO_AUTH
                        : uvcpp_socks5_method::USERNAME_PASSWORD));
  const std::string greeting = socks5_detail::encode_greeting(methods);
  if (greeting.empty()) {
    fail(UV_EINVAL, "cannot encode SOCKS5 greeting", true);
    return;
  }
  const int src = send_handshake(greeting);
  if (src != 0) fail(src, uv_strerror(src), true);
}

int uvcpp_socks5_client::send_handshake(const std::string& bytes) {
  if (bytes.empty()) return UV_EINVAL;
  return tcp_->write(bytes.data(), bytes.size(), [this](int status) {
    if (status != 0) fail(status, uv_strerror(status), true);
  });
}

// =========================================================================
// 读与状态机
// =========================================================================

void uvcpp_socks5_client::on_read(uvcpp_buf* buf) {
  if (buf == nullptr) return;
  if (phase_ == phase::READY) {
    deliver_app(buf->get_const_data(), buf->size());
    return;
  }
  if (phase_ == phase::IDLE || phase_ == phase::FAILED) return;

  in_.append(buf->get_const_data(), buf->size());
  drive();
}

void uvcpp_socks5_client::deliver_app(const char* data, std::size_t len) {
  if (len == 0) return;
  if (!read_cb_) {
    // 消费者还没出现：先攒着（有上限，别让代理把内存灌满）。
    if (stash_.size() + len <= kMaxStashBytes) {
      stash_.append(data, len);
    }
    return;
  }
  uvcpp_buf b(data, len);
  read_cb_(&b);
}

void uvcpp_socks5_client::drive() {
  // 用户回调可能在本函数栈里把 `this` 删掉（"操作完成就把客户端扔掉"是
  // 完全合法的用法）。令牌在堆上，读它不碰已释放的对象 —— 与 tcp 客户端
  // 的 `alive_token_` 同一套。
  std::shared_ptr<char> life = alive_token();

  for (;;) {
    std::size_t used = 0;

    if (phase_ == phase::GREETING) {
      uvcpp_socks5_method method = uvcpp_socks5_method::NO_ACCEPTABLE;
      const socks5_detail::parse_status st =
          socks5_detail::parse_method_selection(in_.data(), in_.size(), used,
                                                method);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::ERROR) {
        fail(UV_EPROTO, "malformed method-selection reply", true);
        return;
      }
      in_.erase(0, used);

      if (method == uvcpp_socks5_method::NO_ACCEPTABLE) {
        fail(UV_EACCES, "proxy: no acceptable authentication method", true);
        return;
      }
      if (method == uvcpp_socks5_method::USERNAME_PASSWORD) {
        if (username_.empty()) {
          fail(UV_EACCES,
               "proxy requires username/password, none was configured", true);
          return;
        }
        phase_ = phase::AUTH;
        const std::string auth = socks5_detail::encode_auth_request(
            username_, password_);
        if (auth.empty()) {
          fail(UV_EINVAL, "invalid username/password", true);
          return;
        }
        const int rc = send_handshake(auth);
        if (rc != 0) {
          fail(rc, uv_strerror(rc), true);
          return;
        }
        continue;
      }
      if (method == uvcpp_socks5_method::NO_AUTH) {
        if (!username_.empty()) {
          fail(UV_EPROTO,
               "proxy selected no-auth although only username/password was "
               "offered",
               true);
          return;
        }
        phase_ = phase::READY;
        complete_op(0, uvcpp_socks5_endpoint());
        return;
      }
      fail(UV_EPROTO, "unknown authentication method from proxy", true);
      return;
    }

    if (phase_ == phase::AUTH) {
      std::uint8_t status = 0xFF;
      const socks5_detail::parse_status st =
          socks5_detail::parse_auth_response(in_.data(), in_.size(), used,
                                             status);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::ERROR) {
        fail(UV_EPROTO, "malformed authentication reply", true);
        return;
      }
      in_.erase(0, used);
      if (status != 0) {
        fail(UV_EACCES, "username/password authentication failed", true);
        return;
      }
      phase_ = phase::READY;
      complete_op(0, uvcpp_socks5_endpoint());
      return;
    }

    if (phase_ == phase::REQUEST) {
      uvcpp_socks5_reply rep = uvcpp_socks5_reply::GENERAL_FAILURE;
      uvcpp_socks5_endpoint bound;
      const socks5_detail::parse_status st = socks5_detail::parse_reply(
          in_.data(), in_.size(), used, rep, bound);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::ERROR) {
        fail(UV_EPROTO, "malformed SOCKS5 reply", true);
        return;
      }
      in_.erase(0, used);
      if (rep != uvcpp_socks5_reply::SUCCEEDED) {
        fail(socks5_detail::reply_to_error(rep),
             socks5_detail::reply_string(rep), true);
        return;
      }

      const op kind = op_;
      if (kind == op::BIND) {
        // 第一次应答：绑定完成。控制连接继续等第二次应答（对端连入）。
        phase_     = phase::BIND_WAIT;
        peer_done_ = false;
        peer_status_ = 0;
        complete_op(0, bound);
        if (!token_alive(life)) return;
        // 第二次应答可能已经躺在 in_ 里（同一个 TCP 段）。
        arm_timeout(kBindPeerTimeoutMs);
        continue;
      }
      if (kind == op::UDP) {
        // 通配地址要换成代理自己的地址，否则数据报会发到本机。
        uvcpp_socks5_endpoint relay = bound;
        if (wildcard_host(relay.host) || relay.port == 0) {
          std::string proxy_ip;
          int proxy_port = 0;
          tcp_->getPeerAddrs(proxy_ip, proxy_port);
          if (!proxy_ip.empty()) {
            uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::IPV4;
            if (socks5_detail::classify_host(proxy_ip, atyp)) {
              const int use_port = relay.port != 0 ? relay.port : proxy_port;
              relay = uvcpp_socks5_endpoint(atyp, proxy_ip, use_port);
            }
          }
        }
        if (udp_ == nullptr) udp_ = new uvcpp_socks5_udp(loop_);
        const int rrc = udp_->set_relay(relay);
        if (rrc != 0) {
          fail(UV_EPROTO,
               "SOCKS5 server returned a non-numeric UDP relay address", true);
          return;
        }
        phase_ = phase::READY;
        complete_op(0, relay);
        return;
      }

      // CONNECT：隧道通了。同一个读里可能已经跟着目标端的数据，先攒着。
      phase_ = phase::READY;
      if (!in_.empty()) {
        if (stash_.size() + in_.size() <= kMaxStashBytes) {
          stash_.append(in_);
        }
        in_.clear();
      }
      complete_op(0, bound);
      return;
    }

    if (phase_ == phase::BIND_WAIT) {
      uvcpp_socks5_reply rep = uvcpp_socks5_reply::GENERAL_FAILURE;
      uvcpp_socks5_endpoint peer;
      const socks5_detail::parse_status st = socks5_detail::parse_reply(
          in_.data(), in_.size(), used, rep, peer);
      if (st == socks5_detail::parse_status::NEED_MORE) return;
      if (st == socks5_detail::parse_status::ERROR) {
        fail(UV_EPROTO, "malformed BIND reply", true);
        return;
      }
      in_.erase(0, used);

      peer_done_ = true;
      phase_     = phase::READY;
      disarm_timeout();
      if (rep != uvcpp_socks5_reply::SUCCEEDED) {
        peer_status_ = socks5_detail::reply_to_error(rep);
        peer_ep_     = peer;
        last_error_  = socks5_detail::reply_string(rep);
        last_error_code_ = peer_status_;
      } else {
        peer_status_ = 0;
        peer_ep_     = peer;
        if (!in_.empty()) {
          if (stash_.size() + in_.size() <= kMaxStashBytes) {
            stash_.append(in_);
          }
          in_.clear();
        }
      }

      const bool deliver = !peer_sync_;
      std::function<void(int, const uvcpp_socks5_endpoint&)> pcb;
      if (deliver) {
        pcb = std::move(peer_cb_);
        peer_cb_ = nullptr;
      }
      if (pcb) pcb(peer_status_, peer_ep_);
      return;
    }

    return;
  }
}

// =========================================================================
// 命令请求
// =========================================================================

int uvcpp_socks5_client::begin_request(uvcpp_socks5_command cmd,
                                       const uvcpp_socks5_endpoint& target,
                                       op kind) {
  if (phase_ != phase::READY) {
    last_error_      = "SOCKS5 control connection is not ready";
    last_error_code_ = UV_EBUSY;
    return UV_EBUSY;
  }
  if (op_ != op::NONE) {
    last_error_      = "another SOCKS5 command is already in flight";
    last_error_code_ = UV_EBUSY;
    return UV_EBUSY;
  }

  const std::string req = socks5_detail::encode_request(cmd, target);
  if (req.empty()) {
    last_error_      = "cannot encode SOCKS5 request";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }

  op_       = kind;
  op_done_  = false;
  op_status_ = 0;
  phase_    = phase::REQUEST;
  arm_timeout(connect_timeout_ms_);

  const int rc = send_handshake(req);
  if (rc != 0) {
    op_    = op::NONE;
    phase_ = phase::READY;
    disarm_timeout();
    last_error_      = uv_strerror(rc);
    last_error_code_ = rc;
  }
  return rc;
}

int uvcpp_socks5_client::connect_target(const std::string& host, int port,
                                        std::function<void(int)> cb) {
  if (cb == nullptr) return connect_target_wait(host, port);

  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::DOMAIN;
  if (!socks5_detail::classify_host(host, atyp) || port <= 0 || port > 65535) {
    last_error_      = "invalid SOCKS5 target";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  op_cb_ = std::move(cb);
  sync_  = false;
  const int rc = begin_request(uvcpp_socks5_command::CONNECT,
                               uvcpp_socks5_endpoint(atyp, host, port),
                               op::TARGET);
  if (rc != 0) op_cb_ = nullptr;
  return rc;
}

int uvcpp_socks5_client::connect_target_wait(const std::string& host, int port,
                                             int timeout_ms) {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::DOMAIN;
  if (!socks5_detail::classify_host(host, atyp) || port <= 0 || port > 65535) {
    last_error_      = "invalid SOCKS5 target";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  sync_ = true;
  const int rc = begin_request(uvcpp_socks5_command::CONNECT,
                               uvcpp_socks5_endpoint(atyp, host, port),
                               op::TARGET);
  if (rc != 0) {
    sync_ = false;
    return rc;
  }
  arm_timeout(timeout_ms);
  return run_sync_op(timeout_ms);
}

int uvcpp_socks5_client::bind(
    const std::string& host, int port,
    std::function<void(int, const uvcpp_socks5_endpoint&)> on_reply1,
    std::function<void(int, const uvcpp_socks5_endpoint&)> on_peer) {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::IPV4;
  if (!socks5_detail::classify_host(host, atyp) || port < 0 || port > 65535) {
    last_error_      = "invalid SOCKS5 BIND address";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  op_ep_cb_    = std::move(on_reply1);
  peer_cb_     = std::move(on_peer);
  peer_done_   = false;
  peer_status_ = 0;
  sync_        = false;
  return begin_request(uvcpp_socks5_command::BIND,
                       uvcpp_socks5_endpoint(atyp, host, port), op::BIND);
}

int uvcpp_socks5_client::bind_wait(const std::string& host, int port,
                                   uvcpp_socks5_endpoint& out_reply1,
                                   int timeout_ms) {
  uvcpp_socks5_atyp atyp = uvcpp_socks5_atyp::IPV4;
  if (!socks5_detail::classify_host(host, atyp) || port < 0 || port > 65535) {
    last_error_      = "invalid SOCKS5 BIND address";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  op_ep_cb_    = nullptr;
  peer_cb_     = nullptr;  // 同步路径由 wait_for_peer() 接第二次应答
  peer_done_   = false;
  peer_status_ = 0;
  sync_        = true;
  const int rc = begin_request(uvcpp_socks5_command::BIND,
                               uvcpp_socks5_endpoint(atyp, host, port),
                               op::BIND);
  if (rc != 0) {
    sync_ = false;
    return rc;
  }
  arm_timeout(timeout_ms);
  const int st = run_sync_op(timeout_ms);
  if (st == 0) out_reply1 = op_ep_;
  return st;
}

int uvcpp_socks5_client::wait_for_peer(uvcpp_socks5_endpoint& out_peer,
                                       int timeout_ms) {
  if (!peer_done_ && phase_ != phase::BIND_WAIT) {
    last_error_      = "no BIND is waiting for a peer";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  peer_sync_ = true;
  const bool ok = pump_until([this]() { return peer_done_; }, timeout_ms);
  peer_sync_ = false;
  if (!ok) {
    last_error_      = "timed out waiting for the BIND peer";
    last_error_code_ = UV_ETIMEDOUT;
    return UV_ETIMEDOUT;
  }
  if (peer_status_ == 0) out_peer = peer_ep_;
  return peer_status_;
}

int uvcpp_socks5_client::udp_associate(
    std::function<void(int, const uvcpp_socks5_endpoint&)> cb,
    const std::string& bind_host, int bind_port) {
  if (cb == nullptr) {
    uvcpp_socks5_endpoint relay;
    return udp_associate_wait(relay, 30000, bind_host, bind_port);
  }

  if (phase_ != phase::READY) {
    last_error_      = "SOCKS5 control connection is not ready";
    last_error_code_ = UV_EBUSY;
    return UV_EBUSY;
  }
  if (op_ != op::NONE) {
    last_error_      = "another SOCKS5 command is already in flight";
    last_error_code_ = UV_EBUSY;
    return UV_EBUSY;
  }

  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }
  udp_ = new uvcpp_socks5_udp(loop_);
  int rc = udp_->bind(bind_host, bind_port);
  if (rc != 0) {
    last_error_      = udp_->last_error();
    last_error_code_ = rc;
    delete udp_;
    udp_ = nullptr;
    return rc;
  }

  op_ep_cb_ = std::move(cb);
  sync_     = false;
  rc        = begin_request(uvcpp_socks5_command::UDP_ASSOCIATE,
                            udp_->local_endpoint(), op::UDP);
  if (rc != 0) {
    delete udp_;
    udp_ = nullptr;
  }
  return rc;
}

int uvcpp_socks5_client::udp_associate_wait(uvcpp_socks5_endpoint& relay,
                                            int timeout_ms,
                                            const std::string& bind_host,
                                            int bind_port) {
  if (phase_ != phase::READY) {
    last_error_      = "SOCKS5 control connection is not ready";
    last_error_code_ = UV_EBUSY;
    return UV_EBUSY;
  }
  if (op_ != op::NONE) {
    last_error_      = "another SOCKS5 command is already in flight";
    last_error_code_ = UV_EBUSY;
    return UV_EBUSY;
  }

  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }
  udp_ = new uvcpp_socks5_udp(loop_);
  int rc = udp_->bind(bind_host, bind_port);
  if (rc != 0) {
    last_error_      = udp_->last_error();
    last_error_code_ = rc;
    delete udp_;
    udp_ = nullptr;
    return rc;
  }

  op_ep_cb_ = nullptr;
  sync_     = true;
  rc        = begin_request(uvcpp_socks5_command::UDP_ASSOCIATE,
                            udp_->local_endpoint(), op::UDP);
  if (rc != 0) {
    sync_ = false;
    delete udp_;
    udp_ = nullptr;
    return rc;
  }
  arm_timeout(timeout_ms);
  const int st = run_sync_op(timeout_ms);
  if (st == 0) relay = op_ep_;
  return st;
}

// =========================================================================
// 完成 / 失败
// =========================================================================

void uvcpp_socks5_client::complete_op(int status,
                                      const uvcpp_socks5_endpoint& ep) {
  op_done_   = true;
  op_status_ = status;
  op_ep_     = ep;
  if (status == 0) disarm_timeout();
  // `op_` 必须在同步分支之前清掉：同步调用靠 `op_done_` 返回，"在这条连接
  // 上还有一条命令在飞"这个判据也得跟着复位，否则 connect_wait() 之后的
  // 第一个命令会拿到 UV_EBUSY。
  op_ = op::NONE;
  if (sync_) return;  // 同步调用由泵循环取 op_done_/op_status_

  std::function<void(int)> fn = std::move(op_cb_);
  op_cb_ = nullptr;
  std::function<void(int, const uvcpp_socks5_endpoint&)> fn_ep =
      std::move(op_ep_cb_);
  op_ep_cb_ = nullptr;

  // 用户回调可能是最后一句话 —— 之后不许再碰成员（见 drive() 的说明）。
  if (fn) {
    fn(status);
  } else if (fn_ep) {
    fn_ep(status, ep);
  }
}

void uvcpp_socks5_client::fail(int code, const std::string& message,
                               bool close_control) {
  if (phase_ == phase::FAILED) return;

  const bool was_bind_wait = (phase_ == phase::BIND_WAIT);
  const bool was_sync      = sync_;

  const int err = code != 0 ? code : UV_EPROTO;
  last_error_code_ = err;
  last_error_ = !message.empty() ? message : uv_strerror(err);
  phase_      = phase::FAILED;
  op_         = op::NONE;
  disarm_timeout();

  op_done_   = true;
  op_status_ = err;
  op_ep_     = uvcpp_socks5_endpoint();

  std::function<void(int)> fn;
  std::function<void(int, const uvcpp_socks5_endpoint&)> fn_ep;
  std::function<void(int, const uvcpp_socks5_endpoint&)> pcb;
  if (!was_sync) {
    fn    = std::move(op_cb_);
    fn_ep = std::move(op_ep_cb_);
    if (was_bind_wait && !peer_sync_) {
      pcb = std::move(peer_cb_);
    }
  }
  op_cb_    = nullptr;
  op_ep_cb_ = nullptr;
  peer_cb_  = nullptr;

  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }

  if (close_control && tcp_ != nullptr) {
    tcp_closed_ = true;
    tcp_->close(nullptr);
  }

  if (!was_sync) {
    if (pcb) pcb(err, uvcpp_socks5_endpoint());
    if (fn) fn(err);
    else if (fn_ep) fn_ep(err, uvcpp_socks5_endpoint());
  }
}

void uvcpp_socks5_client::on_control_closed() {
  if (phase_ == phase::IDLE || phase_ == phase::FAILED) return;
  tcp_closed_ = true;

  const bool was_ready = (phase_ == phase::READY);
  if (udp_ != nullptr) {
    // RFC 1928 §7：UDP 关联的生命周期绑在控制 TCP 上。
    delete udp_;
    udp_ = nullptr;
  }

  if (was_ready) {
    phase_           = phase::IDLE;
    last_error_      = "SOCKS5 control connection closed";
    last_error_code_ = UV_ECONNRESET;
    return;
  }
  fail(UV_ECONNRESET, "SOCKS5 control connection closed", false);
}

// =========================================================================
// 同步泵
// =========================================================================

bool uvcpp_socks5_client::pump_until(std::function<bool()> done,
                                     int timeout_ms) {
  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    if (done()) return true;
    if (loop_ != nullptr) loop_->run(UV_RUN_NOWAIT);
    if (done()) return true;
    if (timeout_ms > 0) {
      const long long elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();
      if (elapsed >= timeout_ms) return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

int uvcpp_socks5_client::run_sync_op(int timeout_ms) {
  const bool ok = pump_until([this]() { return op_done_; }, timeout_ms);
  sync_ = false;
  if (!ok) {
    fail(UV_ETIMEDOUT, "SOCKS5 operation timed out", true);
    sync_ = false;
    return UV_ETIMEDOUT;
  }
  return op_status_;
}

// =========================================================================
// 隧道读写 / 收尾
// =========================================================================

int uvcpp_socks5_client::read_start(std::function<void(uvcpp_buf*)> cb) {
  if (cb == nullptr) {
    last_error_      = "read_start requires a callback";
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  if (read_cb_) {
    last_error_      = "a read callback is already installed";
    last_error_code_ = UV_EALREADY;
    return UV_EALREADY;
  }
  read_cb_ = std::move(cb);

  if (!stash_.empty()) {
    std::string first;
    first.swap(stash_);
    uvcpp_buf b(first.data(), first.size());
    read_cb_(&b);
  }
  return 0;
}

void uvcpp_socks5_client::clear_read_callback() { read_cb_ = nullptr; }

int uvcpp_socks5_client::write(const char* data, std::size_t len,
                               std::function<void(int)> cb) {
  if (tcp_ == nullptr) {
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  return tcp_->write(data, len, std::move(cb));
}

int uvcpp_socks5_client::write_wait(const char* data, std::size_t len,
                                    int timeout_ms) {
  if (tcp_ == nullptr) {
    last_error_code_ = UV_EINVAL;
    return UV_EINVAL;
  }
  return tcp_->write_wait(data, len, timeout_ms);
}

void uvcpp_socks5_client::close() {
  disarm_timeout();
  if (udp_ != nullptr) {
    delete udp_;
    udp_ = nullptr;
  }
  phase_ = phase::IDLE;
  op_    = op::NONE;
  if (tcp_ != nullptr) {
    tcp_closed_ = true;
    tcp_->close(nullptr);
  }
}

}  // namespace uvcpp
