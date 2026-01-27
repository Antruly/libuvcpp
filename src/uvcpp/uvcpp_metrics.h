/**
 * @file src/uvcpp/uvcpp_metrics.h
 * @brief Wrapper for libuv metrics APIs (uv_metrics_t) when available.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_METRICS_H
#define SRC_UVCPP_UVCPP_METRICS_H

#include <uvcpp/uv_define.h>

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 45
#include <uvcpp/uvcpp_define.h>
#include <handle/uvcpp_loop.h>
namespace uvcpp {
class UVCPP_API uvcpp_metrics {
public:
  UVCPP_DEFINE_FUNC(uvcpp_metrics)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_metrics)

  /** @brief Initialize metrics structures. */
  int init();
  /** @brief Populate metrics information for \p loop. */
  int info(uvcpp_loop *loop);
  /** @brief Return idle time for the given loop. */
  static uint64_t idle_time(uvcpp_loop *loop);
  /** @brief Access underlying uv_metrics_t. */
  uv_metrics_t *get_metrics() const;

private:
  uv_metrics_t *metrics = nullptr;
};
} // namespace uvcpp
#endif
#endif

#endif // SRC_UVCPP_UVCPP_METRICS_H
