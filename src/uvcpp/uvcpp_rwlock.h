/**
 * @file src/uvcpp/uvcpp_rwlock.h
 * @brief Reader-writer lock wrapper using libuv's rwlock.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_RWLOCK_H
#define SRC_UVCPP_UVCPP_RWLOCK_H

#include <uvcpp/uvcpp_define.h>

namespace uvcpp {
class UVCPP_API uvcpp_rwlock {
public:
  UVCPP_DEFINE_FUNC(uvcpp_rwlock)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_rwlock)

  /** @brief Initialize the rwlock. */
  int init();
  /** @brief Acquire read lock. */
  void rdlock();
  /** @brief Try acquire read lock without blocking. */
  void tryrdlock();
  /** @brief Acquire write lock. */
  void wrlock();
  /** @brief Try acquire write lock without blocking. */
  void trywrlock();

  /** @brief Release read lock. */
  void rdunlock();
  /** @brief Release write lock. */
  void wrunlock();

  /** @brief Destroy the rwlock. */
  void destroy();

  /** @brief Get raw rwlock pointer. */
  void *get_rwlock() const;

protected:
private:
  uv_rwlock_t *rwlock = nullptr;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_RWLOCK_H