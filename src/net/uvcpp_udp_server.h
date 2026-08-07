/**
 * @file src/net/uvcpp_udp_server.h
 * @brief Higher-level UDP server with bind/receive convenience.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Wraps uvcpp_udp to provide simple bind + recv with per-datagram
 * sender address and send-back capability.  Unlike TCP there are no
 * per-client connections — every datagram is independent.
 */

#pragma once
#ifndef SRC_NET_UVCPP_UDP_SERVER_H
#define SRC_NET_UVCPP_UDP_SERVER_H

#include <functional>
#include <uv.h>
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_udp.h>
#include <uvcpp/uvcpp_buf.h>

namespace uvcpp {

enum uvcpp_udp_server_status : int {
  UDP_SERVER_NONE      = 0x00,  ///< Initial state
  UDP_SERVER_BOUND     = 0x01,  ///< Bound to address, receiving
  UDP_SERVER_STOPPING  = 0x04,  ///< stop() called
  UDP_SERVER_STOPPED   = 0x08,  ///< Fully stopped
  UDP_SERVER_ERROR     = 0x10   ///< Error (see last_error_code)
};

/**
 * @brief Application-layer UDP server wrapping uvcpp_udp.
 *
 * Binds to a local address and delivers incoming datagrams to a
 * user callback along with the sender's IP and port.  To reply,
 * call send() or send_wait() with the sender's address.
 *
 * Usage:
 * @code
 *   uvcpp_udp_server server;
 *   server.bind("0.0.0.0", 9999);
 *   server.recv_start([](uvcpp_buf* buf, const char* ip, int port) {
 *     // echo back
 *     server.send(ip, port, buf->get_data(), buf->size());
 *   });
 *   server.run(UV_RUN_DEFAULT);
 * @endcode
 */
class UVCPP_API uvcpp_udp_server {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_udp_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_udp_server)

  // -----------------------------------------------------------------
  // Accessors
  // -----------------------------------------------------------------

  uvcpp_udp* get_udp();
  uvcpp_loop* get_loop();
  int get_status() const;
  bool has_status(int flags) const;
  int get_last_error() const;

  // -----------------------------------------------------------------
  // Bind
  // -----------------------------------------------------------------

  int bind(const char* ip, int port);
  int bindIpv4(const char* ip, int port);
  int bindIpv6(const char* ip, int port);

  // -----------------------------------------------------------------
  // Receive
  // -----------------------------------------------------------------

  /**
   * @brief Start receiving datagrams.
   * @param cb  Called for every datagram: (buf, sender_ip, sender_port).
   *            The callback runs on the event-loop thread; keep it fast
   *            or offload heavy work via uvcpp_work.
   */
  int recv_start(std::function<void(uvcpp_buf*, const char*, int)> cb);

  /** @brief Stop receiving. */
  int recv_stop();

  // -----------------------------------------------------------------
  // Send (reply to a specific address)
  // -----------------------------------------------------------------

  int send(const char* ip, int port, const char* data, size_t len,
           std::function<void(int)> cb = nullptr);
  int send_wait(const char* ip, int port, const char* data, size_t len,
                int timeout_ms = 30000);
  int send(const char* ip, int port, const uvcpp_buf& buf,
           std::function<void(int)> cb = nullptr);
  int send(const char* ip, int port, uvcpp_buf* buf,
           std::function<void(int)> cb = nullptr);

  // -----------------------------------------------------------------
  // Stop
  // -----------------------------------------------------------------

  void stop(std::function<void()> on_stopped = nullptr);

  // -----------------------------------------------------------------
  // Loop control
  // -----------------------------------------------------------------

  int run(uv_run_mode md = UV_RUN_DEFAULT);
  void stop_loop();

 private:
  void set_status(int flags);
  void clear_status(int flags);

  using recv_callback_t = void(*)(uvcpp_buf* buf, const char* ip,
                                  int port, void* arg);

  uvcpp_loop* loop_ = nullptr;
  uvcpp_udp*  udp_  = nullptr;
  bool stopped_     = false;
  int status_       = UDP_SERVER_NONE;
  int last_error_code_ = 0;

  recv_callback_t recv_fn_  = nullptr;
  void*           recv_arg_ = nullptr;
};

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_UDP_SERVER_H
