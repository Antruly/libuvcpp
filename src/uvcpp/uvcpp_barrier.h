/**
 * @file src/uvcpp/uvcpp_barrier.h
 * @brief Wrapper for libuv barriers (uv_barrier_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_BARRIER_H
#define SRC_UVCPP_UVCPP_BARRIER_H

#include <uvcpp/uv_define.h>
#include <uvcpp/uvcpp_define.h>

namespace uvcpp {
class UVCPP_API uvcpp_barrier  {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_barrier)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_barrier)

  /** @brief Initialize barrier for count `ct`. */
  int init(int ct);
  /** @brief Wait on the barrier. */
  int wait();
  /** @brief Destroy the barrier. */
  void destroy();

  /** @brief Get initial count used to create the barrier. */
  int get_count();
  /** @brief Get raw barrier pointer. */
  void* get_barrier() const;

 protected:
 private:
  uv_barrier_t* barrier = nullptr;
  int count = 0;
};
}

#endif // SRC_UVCPP_UVCPP_BARRIER_H
