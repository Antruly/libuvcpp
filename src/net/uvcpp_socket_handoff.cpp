/**
 * @file src/net/uvcpp_socket_handoff.cpp
 * @brief Implementation of the cross-loop socket handoff.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_socket_handoff.h>

#include <handle/uvcpp_tcp.h>

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace uvcpp {

uv_os_sock_t uvcpp_handoff_invalid_socket() {
  return static_cast<uv_os_sock_t>(-1);
}

int uvcpp_handoff_extract(uvcpp_tcp* accepted, uv_os_sock_t& out_sock) {
  const uv_os_sock_t kInvalid = uvcpp_handoff_invalid_socket();
  out_sock = kInvalid;

  if (accepted == nullptr) return UV_EINVAL;

  uv_os_sock_t raw = kInvalid;
  const int frc = accepted->fileno(raw);
  if (frc != 0) return frc;
  if (raw == kInvalid) return UV_EBADF;

#if defined(_WIN32)
  // 同一个进程里转手，所以"目标进程"就是自己。这一步只产出协议信息（纯数据）。
  WSAPROTOCOL_INFOW info;
  std::memset(&info, 0, sizeof(info));
  if (::WSADuplicateSocketW(raw, ::GetCurrentProcessId(), &info) == SOCKET_ERROR) {
    return uv_translate_sys_error(::WSAGetLastError());
  }

  // 拿协议信息造一个**新的 socket 对象**。此时它还没有绑定任何完成端口 ——
  // 绑定发生在目标循环上那次 `uv_tcp_open` 里（这才是"转手必须在首次关联
  // 之前"能成立的原因）。
  SOCKET s = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                          FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_OVERLAPPED);
  if (s == INVALID_SOCKET) {
    return uv_translate_sys_error(::WSAGetLastError());
  }
  out_sock = static_cast<uv_os_sock_t>(s);
#else
  const int fd = ::dup(static_cast<int>(raw));
  if (fd < 0) {
    return uv_translate_sys_error(errno);
  }
  // `dup()` 不继承 `FD_CLOEXEC`（原件是 libuv 用 `accept4(SOCK_CLOEXEC)` 接的），
  // 不补上的话 fork+exec 出去的子进程会白白继承一个连着对端的 socket。
  // Windows 那边不用管：`uv_tcp_open` 会自己 `SetHandleInformation(...INHERIT, 0)`。
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  out_sock = static_cast<uv_os_sock_t>(fd);
#endif

  return 0;
}

void uvcpp_handoff_close_raw(uv_os_sock_t sock) {
  if (sock == uvcpp_handoff_invalid_socket()) return;
#if defined(_WIN32)
  ::closesocket(sock);
#else
  ::close(static_cast<int>(sock));
#endif
}

}  // namespace uvcpp
