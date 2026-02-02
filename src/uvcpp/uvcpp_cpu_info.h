/**
 * @file src/uvcpp/uvcpp_cpu_info.h
 * @brief Wrapper for uv_cpu_info_t to query CPU topology and usage.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_CPU_INFO_H
#define SRC_UVCPP_UVCPP_CPU_INFO_H

#include <uvcpp/uvcpp_define.h>

namespace uvcpp {
class UVCPP_API uvcpp_cpu_info {
public:
  UVCPP_DEFINE_FUNC(uvcpp_cpu_info)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_cpu_info)

  /** @brief Initialize CPU info retrieval. */
  int init();

private:
  uv_cpu_info_t *cpu_info = nullptr;
};

} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_CPU_INFO_H