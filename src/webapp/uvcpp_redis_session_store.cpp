/**
 * @file src/webapp/uvcpp_redis_session_store.cpp
 * @brief Redis 异步会话存储的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_redis_session_store.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <redis/uvcpp_redis.h>

#include <uv.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace uvcpp {

uvcpp_redis_session_store_options::uvcpp_redis_session_store_options()
    : connect_timeout_ms(5000), reconnect_ms(1000) {
  key_prefix = "uvcpp:sess:";
}

struct uvcpp_redis_session_store::impl
    : std::enable_shared_from_this<uvcpp_redis_session_store::impl> {
  impl(uvcpp_loop* l, const std::string& u,
       const uvcpp_redis_session_store_options& o)
      : loop(l), url(u), opts(o), started(false), attempt(0) {}

  uvcpp_loop*                                 loop;
  std::string                                 url;
  uvcpp_redis_session_store_options           opts;
  bool                                        started;
  uint64_t                                    attempt;
  std::string                                 err;

  std::unique_ptr<uvcpp_redis_client> cli;
  std::unique_ptr<uvcpp_timer>        timer;

  // 退役客户端延迟释放（与 ws_bus 同一个理由：`close()` 的完成回调还在队列
  // 里，包装对象立刻销毁会踩到它）。
  std::vector<std::unique_ptr<uvcpp_redis_client> > retired;
  std::unique_ptr<uvcpp_timer>                      retire_timer;

  ~impl() {
    if (timer) timer->stop();
    if (retire_timer) retire_timer->stop();
    if (!retired.empty() && loop != nullptr && !loop->is_running()) {
      for (int i = 0; i < 32 && !retired.empty(); ++i) {
        loop->run(UV_RUN_NOWAIT);
      }
    }
    retired.clear();
  }

  std::string key(const std::string& id) const {
    return opts.key_prefix + id;
  }

  bool ready() const { return cli && cli->is_connected(); }

  void arm_reconnect() {
    if (!started) return;
    if (!timer) timer.reset(new uvcpp_timer(loop));
    std::weak_ptr<impl> w = shared_from_this();
    timer->start(
        [w](uvcpp_timer*) {
          std::shared_ptr<impl> s = w.lock();
          if (!s) return;
          if (!s->ready()) s->begin_connect();
        },
        static_cast<uint64_t>(opts.reconnect_ms > 0 ? opts.reconnect_ms : 1),
        0);
  }

  void retire(std::unique_ptr<uvcpp_redis_client>& c) {
    if (!c) return;
    c->close();
    retired.push_back(std::move(c));
    if (!retire_timer) retire_timer.reset(new uvcpp_timer(loop));
    std::weak_ptr<impl> w = shared_from_this();
    retire_timer->start(
        [w](uvcpp_timer*) {
          std::shared_ptr<impl> s = w.lock();
          if (!s) return;
          s->retired.clear();
        },
        0, 0);
  }

  void begin_connect() {
    ++attempt;
    const uint64_t gen_now = attempt;
    retire(cli);

    cli.reset(new uvcpp_redis_client(loop));
    cli->set_connect_timeout_ms(opts.connect_timeout_ms);
    std::weak_ptr<impl> w = shared_from_this();
    const int rc = cli->connect(url, [w, gen_now](int status) {
      std::shared_ptr<impl> s = w.lock();
      if (!s || s->attempt != gen_now) return;
      if (status != 0) {
        s->err = "连接 Redis 失败";
        if (s->cli) s->retire(s->cli);
        s->arm_reconnect();
      }
    });
    if (rc != 0) {
      err = "发起 Redis 连接失败";
      retire(cli);
      arm_reconnect();
    }
  }
};

uvcpp_redis_session_store::uvcpp_redis_session_store(
    uvcpp_loop* loop, const std::string& redis_url,
    const uvcpp_redis_session_store_options& opts)
    : impl_(std::make_shared<impl>(loop, redis_url, opts)) {}

uvcpp_redis_session_store::~uvcpp_redis_session_store() {}

int uvcpp_redis_session_store::start(std::string* err) {
  if (impl_->loop == nullptr || impl_->url.empty()) {
    impl_->err = "没有循环或 Redis URL 为空";
    if (err != nullptr) *err = impl_->err;
    return UV_EINVAL;
  }
  if (impl_->started) return 0;
  impl_->started = true;
  impl_->begin_connect();
  return 0;
}

bool uvcpp_redis_session_store::ready() const { return impl_->ready(); }

const std::string& uvcpp_redis_session_store::last_error() const {
  return impl_->err;
}

void uvcpp_redis_session_store::load(const std::string& id,
                                     uvcpp_session& out,
                                     std::function<void(bool)> done) {
  if (id.empty() || !impl_->ready()) {
    if (done) done(false);
    return;
  }
  std::weak_ptr<impl> w = impl_;
  const int rc = impl_->cli->command(
      std::vector<std::string>{"GET", impl_->key(id)},
      [w, &out, done](int status, const uvcpp_redis_value& reply) {
        if (!w.lock()) return;
        if (status != 0 || reply.is_error()) {
          if (done) done(false);
          return;
        }
        const std::string raw = reply.as_string();
        const bool ok = !raw.empty() && out.decode(raw) &&
                        !out.expired(std::time(nullptr));
        if (done) done(ok);
      });
  if (rc != 0 && done) done(false);
}

void uvcpp_redis_session_store::save(const uvcpp_session& s,
                                     std::function<void(bool)> done) {
  if (s.id().empty() || !impl_->ready()) {
    if (done) done(false);
    return;
  }
  long ttl = static_cast<long>(s.expires_at() - std::time(nullptr));
  if (ttl < 1) ttl = 1;
  char ttl_buf[32];
  std::snprintf(ttl_buf, sizeof(ttl_buf), "%ld", ttl);

  const int rc = impl_->cli->command(
      std::vector<std::string>{"SET", impl_->key(s.id()), s.encode(), "EX",
                               ttl_buf},
      [done](int status, const uvcpp_redis_value& reply) {
        if (done) done(status == 0 && !reply.is_error());
      });
  if (rc != 0 && done) done(false);
}

void uvcpp_redis_session_store::destroy(const std::string& id,
                                        std::function<void(bool)> done) {
  if (id.empty() || !impl_->ready()) {
    if (done) done(false);
    return;
  }
  const int rc = impl_->cli->command(
      std::vector<std::string>{"DEL", impl_->key(id)},
      [done](int status, const uvcpp_redis_value& reply) {
        if (done) done(status == 0 && !reply.is_error());
      });
  if (rc != 0 && done) done(false);
}

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE
