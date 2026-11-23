/**
 * @file src/webapp/uvcpp_session.cpp
 * @brief 会话存储与中间件的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "webapp/uvcpp_session.h"

#if UVCPP_WEBAPP_ENABLE

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <utility>

#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

namespace uvcpp {

namespace {

const char* kAttrKey = "uvcpp.session";

std::string new_session_id() {
  // 128 位：两次 random_device 拼起来。不是"密码学保证"的承诺 —— 但 Linux 上
  // random_device 走 /dev/urandom，Windows 上走 rand_s，够会话 id 用。
  // 带一个进程内计数器，避免随机源被同一毫秒内重复播种时撞车。
  static std::mutex         mu;
  static std::random_device rd;
  static uint64_t           counter = 0;
  std::lock_guard<std::mutex> lk(mu);
  const uint64_t a = (static_cast<uint64_t>(rd()) << 32) ^ rd();
  const uint64_t b = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^
                     static_cast<uint64_t>(
                         std::chrono::steady_clock::now().time_since_epoch()
                             .count()) ^
                     (++counter);
  char buf[33];
  std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                static_cast<unsigned long long>(a),
                static_cast<unsigned long long>(b));
  return std::string(buf);
}

}  // namespace

// ===========================================================================
// uvcpp_session
// ===========================================================================

uvcpp_session::uvcpp_session()
    : created_at_(0), expires_at_(0) {}


namespace {

void put_u32(std::string& out, std::uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void put_u64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
  }
}

bool get_u32(const std::string& in, std::size_t& pos, std::uint32_t& v) {
  if (pos + 4 > in.size()) return false;
  v = static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos])) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos + 1]))
       << 8) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos + 2]))
       << 16) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos + 3]))
       << 24);
  pos += 4;
  return true;
}

bool get_u64(const std::string& in, std::size_t& pos, std::uint64_t& v) {
  if (pos + 8 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(
             static_cast<unsigned char>(in[pos + static_cast<std::size_t>(i)]))
         << (i * 8);
  }
  pos += 8;
  return true;
}

bool get_blob(const std::string& in, std::size_t& pos, std::string& out) {
  std::uint32_t n = 0;
  if (!get_u32(in, pos, n)) return false;
  if (n > 1024u * 1024u || pos + n > in.size()) return false;
  out.assign(in, pos, n);
  pos += n;
  return true;
}

}  // namespace

std::string uvcpp_session::encode() const {
  std::string out;
  put_u32(out, static_cast<std::uint32_t>(id_.size()));
  out += id_;
  put_u64(out, static_cast<std::uint64_t>(created_at_));
  put_u64(out, static_cast<std::uint64_t>(expires_at_));
  put_u32(out, static_cast<std::uint32_t>(data_.size()));
  for (std::map<std::string, std::string>::const_iterator it = data_.begin();
       it != data_.end(); ++it) {
    put_u32(out, static_cast<std::uint32_t>(it->first.size()));
    out += it->first;
    put_u32(out, static_cast<std::uint32_t>(it->second.size()));
    out += it->second;
  }
  return out;
}

bool uvcpp_session::decode(const std::string& in) {
  std::size_t   pos = 0;
  std::string   id;
  std::uint64_t created = 0, expires = 0;
  std::uint32_t count = 0;
  if (!get_blob(in, pos, id)) return false;
  if (!get_u64(in, pos, created) || !get_u64(in, pos, expires)) return false;
  if (!get_u32(in, pos, count)) return false;
  if (count > 100000u) return false;
  std::map<std::string, std::string> data;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string k, v;
    if (!get_blob(in, pos, k) || !get_blob(in, pos, v)) return false;
    data[k] = v;
  }
  if (pos != in.size()) return false;

  id_         = id;
  created_at_ = static_cast<std::time_t>(created);
  expires_at_ = static_cast<std::time_t>(expires);
  data_.swap(data);
  return true;
}

bool uvcpp_session::has(const std::string& key) const {
  return data_.find(key) != data_.end();
}

std::string uvcpp_session::get(const std::string& key,
                               const std::string& def) const {
  std::map<std::string, std::string>::const_iterator it = data_.find(key);
  return it == data_.end() ? def : it->second;
}

void uvcpp_session::set(const std::string& key, const std::string& value) {
  data_[key] = value;
}

void uvcpp_session::erase(const std::string& key) { data_.erase(key); }

void uvcpp_session::clear() { data_.clear(); }

void uvcpp_session::regenerate() {
  id_ = new_session_id();
  // 有效期跟着"这一次续期"走；调用方随后应当用
  // `uvcpp_session_set_cookie()` 把新 id 发给客户端。
}

// ===========================================================================
// 内存存储
// ===========================================================================

struct uvcpp_memory_session_store::impl {
  explicit impl(size_t max) : max_sessions(max) {}

  mutable std::mutex                                  mu;
  std::map<std::string, uvcpp_session>                sessions;
  size_t                                              max_sessions;

  void sweep_expired_locked(std::time_t now) {
    for (std::map<std::string, uvcpp_session>::iterator it = sessions.begin();
         it != sessions.end();) {
      if (it->second.expired(now)) {
        it = sessions.erase(it);
      } else {
        ++it;
      }
    }
  }
};

uvcpp_memory_session_store::uvcpp_memory_session_store(size_t max_sessions)
    : impl_(new impl(max_sessions == 0 ? 1 : max_sessions)) {}

uvcpp_memory_session_store::~uvcpp_memory_session_store() {}

bool uvcpp_memory_session_store::load(const std::string& id,
                                      uvcpp_session& out) {
  if (id.empty()) return false;
  std::lock_guard<std::mutex> lk(impl_->mu);
  std::map<std::string, uvcpp_session>::iterator it =
      impl_->sessions.find(id);
  if (it == impl_->sessions.end()) return false;
  if (it->second.expired(std::time(nullptr))) {
    impl_->sessions.erase(it);
    return false;
  }
  out = it->second;
  return true;
}

void uvcpp_memory_session_store::save(const uvcpp_session& s) {
  if (s.id().empty()) return;
  std::lock_guard<std::mutex> lk(impl_->mu);
  const std::time_t now = std::time(nullptr);
  if (s.expired(now)) {
    impl_->sessions.erase(s.id());
    return;
  }
  if (impl_->sessions.find(s.id()) == impl_->sessions.end() &&
      impl_->sessions.size() >= impl_->max_sessions) {
    // 满了：先清过期，再按"最早过期"逐出一个。O(n) 只在真满时发生，
    // 比每次 save 都排序便宜。
    impl_->sweep_expired_locked(now);
    while (impl_->sessions.size() >= impl_->max_sessions) {
      std::map<std::string, uvcpp_session>::iterator victim =
          impl_->sessions.begin();
      for (std::map<std::string, uvcpp_session>::iterator it =
               impl_->sessions.begin();
           it != impl_->sessions.end(); ++it) {
        if (it->second.expires_at() < victim->second.expires_at()) victim = it;
      }
      impl_->sessions.erase(victim);
    }
  }
  impl_->sessions[s.id()] = s;
}

void uvcpp_memory_session_store::destroy(const std::string& id) {
  if (id.empty()) return;
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->sessions.erase(id);
}

size_t uvcpp_memory_session_store::size() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->sessions.size();
}

// ===========================================================================
// 选项 / 中间件
// ===========================================================================

uvcpp_session_options::uvcpp_session_options()
    : cookie_name("sid"),
      path("/"),
      same_site("Lax"),
      ttl_seconds(86400),
      secure(false),
      http_only(true),
      sliding(true) {}

void uvcpp_session_set_cookie(uvcpp_web_response& resp,
                              const uvcpp_session& s,
                              const uvcpp_session_options& opts) {
  if (s.empty()) return;
  const long max_age =
      static_cast<long>(std::max<std::time_t>(
          1, s.expires_at() - std::time(nullptr)));
  resp.set_cookie(opts.cookie_name, s.id(), opts.path, max_age,
                  opts.http_only, opts.secure, opts.same_site);
}

uvcpp_session* uvcpp_session_of(uvcpp_web_request& req) {
  return req.attribute_as<uvcpp_session>(kAttrKey).get();
}

uvcpp_web_middleware uvcpp_session_middleware(
    std::shared_ptr<uvcpp_session_store> store,
    const uvcpp_session_options&         opts) {
  if (!store) {
    return [](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next next) {
      next();
    };
  }
  return [store, opts](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    std::shared_ptr<uvcpp_session> s(new uvcpp_session());
    const std::string*             sid = req.cookie(opts.cookie_name);
    const bool loaded = sid != nullptr && !sid->empty() &&
                        store->load(*sid, *s);
    const std::time_t now = std::time(nullptr);
    if (!loaded) {
      s->id_         = new_session_id();
      s->created_at_ = now;
      s->expires_at_ = now + opts.ttl_seconds;
      store->save(*s);
      uvcpp_session_set_cookie(resp, *s, opts);
    } else if (opts.sliding) {
      s->expires_at_ = now + opts.ttl_seconds;
      store->save(*s);
      uvcpp_session_set_cookie(resp, *s, opts);
    }

    const std::string id_before = s->id();
    req.set_attribute(kAttrKey, std::static_pointer_cast<void>(s));

    // **落库必须挂在 `on_sent` 上，不能写在 `next()` 之后。**
    // 中间件链的 `next()` 只是"安排续跑"：实测它先返回，下游 handler 之后才
    // 执行 —— 写在 next 后面的 save 拿到的是**处理前**的会话，处理器里 set()
    // 的东西根本没落库（登录后下一条请求读不到 uid 的实测形态）。
    // `on_sent` 在响应真正发出后触发，那时 handler 已经跑完。
    resp.on_sent([store, s, id_before](const uvcpp_web_sent_info&) {
      if (s->empty()) {
        if (!id_before.empty()) store->destroy(id_before);
        return;
      }
      if (s->id() != id_before && !id_before.empty()) {
        store->destroy(id_before);  // regenerate：旧 id 立即失效
      }
      store->save(*s);
    });

    next();
  };
}

void uvcpp_session_logout(uvcpp_web_request& req, uvcpp_web_response& resp,
                          uvcpp_session_store& store,
                          const uvcpp_session_options& opts) {
  uvcpp_session* s = uvcpp_session_of(req);
  if (s != nullptr && !s->empty()) {
    store.destroy(s->id());
    s->invalidate();
  }
  resp.clear_cookie(opts.cookie_name, opts.path);
}

// ===========================================================================
// 异步会话中间件（共享存储）
// ===========================================================================

uvcpp_web_middleware uvcpp_async_session_middleware(
    std::shared_ptr<uvcpp_async_session_store> store,
    const uvcpp_session_options&               opts) {
  if (!store) {
    return [](uvcpp_web_request&, uvcpp_web_response&, uvcpp_web_next next) {
      next();
    };
  }
  return [store, opts](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    std::shared_ptr<uvcpp_session> s(new uvcpp_session());
    const std::string* sidp = req.cookie(opts.cookie_name);
    const std::string  sid  = sidp != nullptr ? *sidp : std::string();

    // `cont` 可能在 load 回调里晚一拍跑；`next` 的副本保住上下文，req/resp
    // 仍然有效。它自己也被拷贝进 std::function，所有捕获都是值。
    std::function<void(bool)> cont =
        [store, opts, &req, &resp, next, s](bool loaded) mutable {
          const std::time_t now = std::time(nullptr);
          if (!loaded) {
            s->id_         = new_session_id();
            s->created_at_ = now;
            s->expires_at_ = now + opts.ttl_seconds;
            uvcpp_session_set_cookie(resp, *s, opts);
          } else if (opts.sliding) {
            s->expires_at_ = now + opts.ttl_seconds;
            uvcpp_session_set_cookie(resp, *s, opts);
          }

          const std::string id_before = s->id();
          req.set_attribute(kAttrKey, std::static_pointer_cast<void>(s));

          // 与同步版同一个理由：落库挂 `on_sent`，不能写在 `next()` 之后。
          resp.on_sent([store, s, id_before](const uvcpp_web_sent_info&) {
            if (s->empty()) {
              if (!id_before.empty()) store->destroy(id_before);
              return;
            }
            if (s->id() != id_before && !id_before.empty()) {
              store->destroy(id_before);
            }
            store->save(*s);
          });

          next();
        };

    if (!sid.empty() && store->ready()) {
      store->load(sid, *s, cont);
      return;
    }
    cont(false);
  };
}

void uvcpp_async_session_logout(uvcpp_web_request& req,
                                uvcpp_web_response& resp,
                                uvcpp_async_session_store& store,
                                const uvcpp_session_options& opts) {
  uvcpp_session* s = uvcpp_session_of(req);
  if (s != nullptr && !s->empty()) {
    store.destroy(s->id());
    s->invalidate();
  }
  resp.clear_cookie(opts.cookie_name, opts.path);
}

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
