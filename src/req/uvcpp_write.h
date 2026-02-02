/**
 * @file src/req/uvcpp_write.h
 * @brief Wrapper for uv_write_t to perform asynchronous stream writes.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_WRITE_H
#define SRC_REQ_UVCPP_WRITE_H

#include <req/uvcpp_req.h>
#include <uvcpp/uvcpp_buf.h>

namespace uvcpp {
/**
 * @brief Write request wrapper for stream write operations.
 *
 * Use `set_buf` / `set_src_buf` to configure buffers. Provide `m_write_cb`
 * to receive completion notifications.
 */
class UVCPP_API uvcpp_write : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_write)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_write)

  /** @brief Initialize the write request. */
  int init();

  /** @brief Set the main buffer to write; if owner=true wrapper will free it. */
  void set_uv_buf(uv_buf_t *bf, bool owner = false);
  /** @brief Get the buffer previously set. */
  uv_buf_t *get_uv_buf();
  /** @brief Set a source buffer (used for send_handle scenarios). */
  void set_src_buf(const uvcpp_buf *bf, bool owner = false);
  /** @brief Get the source buffer previously set. */
  const uvcpp_buf *get_src_buf();
  ::std::function<void(uvcpp_write*,int)> m_write_cb;
 public:

  /** @brief libuv uv_write_cb forwarded to m_write_cb. */
  static void callback_write(uv_write_t *req, int status);

  private:
  uv_buf_t *uv_buf = nullptr;
  const uvcpp_buf *src_buf = nullptr;
  bool uv_buf_owner = false;
  bool src_buf_owner = false;
};
} // namespace uvcpp

#endif // SRC_REQ_UVCPP_WRITE_H