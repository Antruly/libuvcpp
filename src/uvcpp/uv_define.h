/**
 * @file src/uvcpp/uv_define.h
 * @brief Project-wide low-level defines and helpers for libuv integration.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UV_DEFINE_H
#define SRC_UVCPP_UV_DEFINE_H

extern "C" {
#include <uv.h>
}

#ifndef NDIS_IF_MAX_STRING_SIZE
#define NDIS_IF_MAX_STRING_SIZE 256
#endif // !NDIS_IF_MAX_STRING_SIZE

#define STD_NO_ZERO_ERROR_SHOW_INT(_ret, _remark)                              \
  if (_ret) {                                                                  \
    printf("%s error %s", _remark, uv_strerror(_ret));                         \
    return _ret;                                                               \
  }
#define STD_NO_ZERO_ERROR_SHOW(_ret, _remark)                                  \
  if (_ret) {                                                                  \
    printf("%s error %s", _remark, uv_strerror(_ret));                         \
    return;                                                                    \
  }
#define STD_G_ZERO_ERROR_SHOW_INT(_ret, _remark)                               \
  if (_ret > 0) {                                                              \
    printf("%s error %s", _remark, uv_strerror(_ret));                         \
    return _ret;                                                               \
  }
#define STD_G_ZERO_ERROR_SHOW(_ret, _remark)                                   \
  if (_ret > 0) {                                                              \
    printf("%s error %s", _remark, uv_strerror(_ret));                         \
    return;                                                                    \
  }
#define STD_L_ZERO_ERROR_SHOW_INT(_ret, _remark)                               \
  if (_ret < 0) {                                                              \
    printf("%s error %s", _remark, uv_strerror(_ret));                         \
    return _ret;                                                               \
  }
#define STD_L_ZERO_ERROR_SHOW(_ret, _remark)                                   \
  if (_ret < 0) {                                                              \
    printf("%s error %s", _remark, uv_strerror(_ret));                         \
    return;                                                                    \
  }

#endif // SRC_UVCPP_UV_DEFINE_H
