/**
 * @file src/quic/uvcpp_quic_connection.cpp
 * @brief 1.4.1 的连接壳：元数据能读，动作一律 `UV_ENOSYS`。
 * @author zhuweiye
 * @version 1.4.1
 */

#include "quic/uvcpp_quic_connection.h"

#if UVCPP_QUIC_ENABLE

#include <uv.h>

namespace uvcpp {

struct uvcpp_quic_connection::impl {
  callbacks cbs;
  // 状态机还没有，所以这两个值是**恒定的**：`state()` 返回下面这个初值，
  // `alpn_selected()` 返回空串。这正是头文件里那两条 `@warning` 说的事 ——
  // 写在这里是为了让"为什么恒返回它"在一处就能看完，而不是靠 grep。
  quic_connection_state state = quic_connection_state::IDLE;
  std::string          alpn;
};

uvcpp_quic_connection::uvcpp_quic_connection() : impl_(new impl()) {}

uvcpp_quic_connection::~uvcpp_quic_connection() = default;

void uvcpp_quic_connection::set_callbacks(const callbacks& cbs) {
  impl_->cbs = cbs;
}

quic_connection_state uvcpp_quic_connection::state() const {
  return impl_->state;
}

std::string uvcpp_quic_connection::alpn_selected() const {
  return impl_->alpn;
}

// =========================================================================
// 动作 —— 1.4.1 全部 UV_ENOSYS
// =========================================================================
//
// `UV_ENOSYS`（"这个功能没有实现"）而不是 `UV_EINVAL` / `UV_EPERM` 之类：
// 参数是好是坏在这里根本无从判断（没有状态机），报一个**关于参数**的错就是在
// 撒谎，而调用方会照着那个错误去改参数。`UV_ENOSYS` 是唯一一句实话。
//
// 返回值类型照签名走：`open_stream` 的签名是 `int64_t`，所以是
// `(int64_t)UV_ENOSYS` 而不是 `UV_ENOSYS` —— 后者是 `int`，靠隐式转换也能过，
// 但那正好把"这个返回类型为什么是 int64_t"（QUIC 流号到 2^62-1）那件事
// 从代码里抹掉了。显式写出来，下次有人想把它改回 `int` 时会先看到这一行。

int64_t uvcpp_quic_connection::open_stream(bool bidi) {
  (void)bidi;
  return static_cast<int64_t>(UV_ENOSYS);
}

int uvcpp_quic_connection::write_stream(int64_t stream_id, const char* data,
                                        size_t len, bool end_stream) {
  (void)stream_id;
  (void)data;
  (void)len;
  (void)end_stream;
  return UV_ENOSYS;
}

int uvcpp_quic_connection::shutdown_stream(int64_t stream_id) {
  (void)stream_id;
  return UV_ENOSYS;
}

int uvcpp_quic_connection::close(int error_code) {
  (void)error_code;
  return UV_ENOSYS;
}

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
