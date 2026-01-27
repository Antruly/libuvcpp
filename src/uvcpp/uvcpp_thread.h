/**
 * @file src/uvcpp/uvcpp_thread.h
 * @brief Thread helpers wrapping libuv thread API.
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_THREAD_H
#define SRC_UVCPP_UVCPP_THREAD_H

#include <uvcpp/uv_define.h>
#include <uvcpp/uvcpp_define.h>
#include <functional>

namespace uvcpp {
/**
 * @brief Lightweight wrapper around libuv thread primitives.
 */
class UVCPP_API uvcpp_thread {
public:
  UVCPP_DEFINE_FUNC(uvcpp_thread)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_thread)

  /**
   * @brief Start the thread, invoking start_cb with arg on the new thread.
   */
  int start(::std::function<void(void *)> start_cb, void *arg);

  /** @brief Initialize internal thread state. */
  int init();
  /** @brief Join the thread. */
  int join();
  /** @brief Compare with another wrapper. */
  int equal(const uvcpp_thread *t);
  /** @brief Compare with a raw uv_thread_t. */
  int equal(const uv_thread_t *t);

  /** @brief Access the underlying uv_thread_t. */
  uv_thread_t *get_thread() const;

  static int equal(const uvcpp_thread *t1, const uvcpp_thread *t2);
  static int equal(const uv_thread_t *t1, const uv_thread_t *t2);
  static uv_thread_t self();

protected:
  ::std::function<void(void *)> thread_start_cb;

private:
  static void callback_start(void *pdata) {
    if (reinterpret_cast<uvcpp_thread *>(pdata)->thread_start_cb)
      reinterpret_cast<uvcpp_thread *>(pdata)->thread_start_cb(
          reinterpret_cast<uvcpp_thread *>(pdata));
  }

private:
  uv_thread_t *thread = nullptr;
  void *vdata = nullptr;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_THREAD_H