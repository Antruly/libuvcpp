/**
 * @file src/webapp/uvcpp_redis_session_store.h
 * @brief Redis 异步会话存储（issue #70 的共享存储）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ```cpp
 * auto store = std::make_shared<uvcpp::uvcpp_redis_session_store>(
 *     &loop, "redis://127.0.0.1:6379/0");
 * std::string err;
 * if (store->start(&err) != 0) { ... }
 * app.use(uvcpp::uvcpp_async_session_middleware(store, opts));
 * ```
 *
 * - 一个 store 一条连接，**只在构造时给的那条循环的线程上**调用。
 * - 连接断了按 `reconnect_ms` 定时重连；未就绪时 `load` 按未命中回调
 *   （中间件会新建会话），`save/destroy` 直接回调 false。
 * - 值的格式由 `uvcpp_session::encode()` 定义；TTL 用 Redis 的 `EX`，与
 *   `expires_at` 对齐（最小 1 秒）。
 * - 记录键：`<key_prefix><session id>`，默认前缀 `uvcpp:sess:`。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_REDIS_SESSION_STORE_H
#define SRC_WEBAPP_UVCPP_REDIS_SESSION_STORE_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE

#include <memory>
#include <string>

#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_session.h>

namespace uvcpp {

class uvcpp_loop;

struct UVCPP_API uvcpp_redis_session_store_options {
  uvcpp_redis_session_store_options();
  std::string key_prefix;         ///< 默认 `uvcpp:sess:`
  int         connect_timeout_ms; ///< 默认 5000
  int         reconnect_ms;       ///< 默认 1000
};

class UVCPP_API uvcpp_redis_session_store
    : public uvcpp_async_session_store {
 public:
  uvcpp_redis_session_store(uvcpp_loop* loop, const std::string& redis_url,
                            const uvcpp_redis_session_store_options& opts =
                                uvcpp_redis_session_store_options());
  ~uvcpp_redis_session_store() override;

  uvcpp_redis_session_store(const uvcpp_redis_session_store&) = delete;
  uvcpp_redis_session_store& operator=(const uvcpp_redis_session_store&) =
      delete;

  /// 开始连接（异步、幂等）。参数错误返回非 0 并写 `err`。
  int start(std::string* err = nullptr);

  bool ready() const override;

  void load(const std::string& id, uvcpp_session& out,
            std::function<void(bool found)> done) override;
  void save(const uvcpp_session& s,
            std::function<void(bool ok)> done = nullptr) override;
  void destroy(const std::string& id,
               std::function<void(bool ok)> done = nullptr) override;

  const std::string& last_error() const;

 private:
  struct impl;
  std::shared_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE
#endif  // SRC_WEBAPP_UVCPP_REDIS_SESSION_STORE_H
