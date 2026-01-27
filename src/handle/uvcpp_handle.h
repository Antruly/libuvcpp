/**
 * @file src/handle/uvcpp_handle.h
 * @brief Base C++ wrapper types for libuv handle objects.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides the `uvcpp_handle` base class and helper macros for converting
 * between wrapper objects and raw libuv handle pointers.
 */

#pragma once
#ifndef SRC_HANDLE_UVCPP_HANDLE_H
#define SRC_HANDLE_UVCPP_HANDLE_H

#include <functional>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uv_buf.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uv_alloc.h>

// ================= Handle类型转换宏 =================
#define UVCPP_LOOP_HANDLE      reinterpret_cast<uv_loop_t *>(this->get_handle())
#define UVCPP_IDLE_HANDLE      reinterpret_cast<uv_idle_t *>(this->get_handle())
#define UVCPP_PREPARE_HANDLE   reinterpret_cast<uv_prepare_t *>(this->get_handle())
#define UVCPP_TIMER_HANDLE     reinterpret_cast<uv_timer_t *>(this->get_handle())
#define UVCPP_THREAD_HANDLE    reinterpret_cast<uv_thread_t *>(this->get_handle())
#define UVCPP_PROCESS_HANDLE   reinterpret_cast<uv_process_t *>(this->get_handle())
#define UVCPP_STREAM_HANDLE    reinterpret_cast<uv_stream_t *>(this->get_handle())
#define UVCPP_PIPE_HANDLE      reinterpret_cast<uv_pipe_t *>(this->get_handle())
#define UVCPP_TCP_HANDLE       reinterpret_cast<uv_tcp_t *>(this->get_handle())
#define UVCPP_UDP_HANDLE       reinterpret_cast<uv_udp_t *>(this->get_handle())
#define UVCPP_ASYNC_HANDLE     reinterpret_cast<uv_async_t *>(this->get_handle())
#define UVCPP_CHECK_HANDLE     reinterpret_cast<uv_check_t *>(this->get_handle())
#define UVCPP_FSEVENT_HANDLE   reinterpret_cast<uv_fs_event_t *>(this->get_handle())
#define UVCPP_FSPOLL_HANDLE    reinterpret_cast<uv_fs_poll_t *>(this->get_handle())
#define UVCPP_POLL_HANDLE      reinterpret_cast<uv_poll_t *>(this->get_handle())
#define UVCPP_SIGNAL_HANDLE    reinterpret_cast<uv_signal_t *>(this->get_handle())
#define UVCPP_TTY_HANDLE       reinterpret_cast<uv_tty_t *>(this->get_handle())

#define OBJ_UVCPP_LOOP_HANDLE(obj)      reinterpret_cast<uv_loop_t *>((obj).get_handle())
#define OBJ_UVCPP_IDLE_HANDLE(obj)      reinterpret_cast<uv_idle_t *>((obj).get_handle())
#define OBJ_UVCPP_PREPARE_HANDLE(obj)   reinterpret_cast<uv_prepare_t *>((obj).get_handle())
#define OBJ_UVCPP_TIMER_HANDLE(obj)     reinterpret_cast<uv_timer_t *>((obj).get_handle())
#define OBJ_UVCPP_THREAD_HANDLE(obj)    reinterpret_cast<uv_thread_t *>((obj).get_handle())
#define OBJ_UVCPP_PROCESS_HANDLE(obj)   reinterpret_cast<uv_process_t *>((obj).get_handle())
#define OBJ_UVCPP_STREAM_HANDLE(obj)    reinterpret_cast<uv_stream_t *>((obj).get_handle())
#define OBJ_UVCPP_PIPE_HANDLE(obj)      reinterpret_cast<uv_pipe_t *>((obj).get_handle())
#define OBJ_UVCPP_TCP_HANDLE(obj)       reinterpret_cast<uv_tcp_t *>((obj).get_handle())
#define OBJ_UVCPP_UDP_HANDLE(obj)       reinterpret_cast<uv_udp_t *>((obj).get_handle())
#define OBJ_UVCPP_ASYNC_HANDLE(obj)     reinterpret_cast<uv_async_t *>((obj).get_handle())
#define OBJ_UVCPP_CHECK_HANDLE(obj)     reinterpret_cast<uv_check_t *>((obj).get_handle())
#define OBJ_UVCPP_FSEVENT_HANDLE(obj)   reinterpret_cast<uv_fs_event_t *>((obj).get_handle())
#define OBJ_UVCPP_FSPOLL_HANDLE(obj)    reinterpret_cast<uv_fs_poll_t *>((obj).get_handle())
#define OBJ_UVCPP_POLL_HANDLE(obj)      reinterpret_cast<uv_poll_t *>((obj).get_handle())
#define OBJ_UVCPP_SIGNAL_HANDLE(obj)    reinterpret_cast<uv_signal_t *>((obj).get_handle())
#define OBJ_UVCPP_TTY_HANDLE(obj)       reinterpret_cast<uv_tty_t *>((obj).get_handle())

// ================= 拷贝辅助宏 =================
#define DEFINE_COPY_FUNC_HANDLE_CPP(type, uvname)                              \
  type::type(const type &obj) {                                                \
    if (obj.get_handle() != nullptr) {                                         \
      uvname *hd = uvcpp::uv_alloc<uvname>();                                  \
      memcpy(hd, obj.get_handle(), sizeof(uvname));                            \
      this->set_handle(hd, true);                                              \
    } else {                                                                   \
      this->set_handle(nullptr, false);                                        \
    }                                                                          \
  }                                                                            \
  type &type::operator=(const type &obj) {                                     \
    if (obj.get_handle() != nullptr) {                                         \
      uvname *hd = uvcpp::uv_alloc<uvname>();                                  \
      memcpy(hd, obj.get_handle(), sizeof(uvname));                            \
      this->set_handle(hd, true);                                              \
    } else {                                                                   \
      this->set_handle(nullptr, false);                                        \
    }                                                                          \
    return *this;                                                              \
  }

namespace uvcpp {

/**
 * @brief libuv handle 基础封装
 *
 * Encapsulates a libuv handle and provides lifecycle and utility APIs.
 */
class UVCPP_API uvcpp_handle {
public:
    // 构造/析构/拷贝/赋值
    UVCPP_DEFINE_FUNC(uvcpp_handle)
    UVCPP_DEFINE_COPY_FUNC(uvcpp_handle)

    // 基本操作
    /** @brief Set user data pointer associated with the handle. */
    int set_data(void *pdata);
    /** @brief Get user data pointer associated with the handle. */
    void *get_data();
    /** @brief Increment reference count to keep loop alive. */
    void ref();
    /** @brief Decrement reference count. */
    void unref();
    /** @brief Return whether handle has a reference. */
    int has_ref();
    /** @brief Return whether handle is active. */
    int is_active();
    /** @brief Close the handle asynchronously. */
    void close();
    /** @brief Returns whether the handle is closing. */
    int is_closing();
    /** @brief Close the handle and call \p closeCallback when closed. */
    void close(::std::function<void(uvcpp_handle *)> closeCallback);
    /** @brief Retrieve file descriptor/socket for this handle, if applicable. */
    int fileno(uv_os_sock_t& sock);

    // 属性访问
    /** @brief Return the underlying libuv handle pointer. */
    virtual uv_handle_t *get_handle() const;
    /** @brief Return the size of the underlying handle structure. */
    size_t handle_size();

#if UV_VERSION_MAJOR >= 1 && UV_VERSION_MINOR >= 18
    /** @brief Get the handle type. */
    uv_handle_type handle_get_type();
    /** @brief Get the handle type name. */
    const char *handle_type_name();
    /** @brief Access underlying handle data. */
    void *handle_get_data();
    /** @brief Access the loop associated with this handle. */
    void *handle_get_loop();
    /** @brief Set underlying handle data pointer. */
    void handle_set_data(void *data);
#endif

    // 静态工具
    /** @brief Clone a handle wrapper copying underlying memory. */
    static uvcpp_handle *clone(uvcpp_handle *obj, int memSize);

    static void ref(uvcpp_handle *vhd);
    static void unref(uvcpp_handle *vhd);
    static int has_ref(const uvcpp_handle *vhd);
    static int is_active(const uvcpp_handle *vhd);
    static void close(uvcpp_handle *vhd,
                      ::std::function<void(uvcpp_handle *)> closeCallback);
    static int is_closing(const uvcpp_handle *vhd);
    static int fileno(const uvcpp_handle *vhd, uv_os_sock_t &sock);
    static size_t handle_size(const uvcpp_handle *vhd);

#if UV_VERSION_MAJOR >= 1 && UV_VERSION_MINOR >= 18
    static uv_handle_type handle_get_type(const uvcpp_handle *vhd);
    static const char *handle_type_name(uvcpp_handle *vhd);
    static void *handle_get_data(const uvcpp_handle *vhd);
    static void *handle_get_loop(const uvcpp_handle *vhd);
    static void handle_set_data(uvcpp_handle *vhd, void *data);
#endif
/* old camelCase static accessors removed during rename */

protected:
    /** @brief Update internal handle data storage after underlying changes. */
    void set_handle_data();
    /** @brief Internal allocation callback forwarding to wrapper. */
    static void callback_alloc(uv_handle_t *uvcpp_handle, size_t suggested_size, uv_buf_t *buf);
    /** @brief Internal close callback used to free wrapped resources. */
    static void callback_close(uv_handle_t *uvcpp_handle);
    /** @brief Replace the underlying handle pointer (no ownership flag). */
    virtual void set_handle(void *hd);
    /** @brief Replace the underlying handle pointer with ownership flag. */
    virtual void set_handle(void *hd, bool owns);
    ::std::function<void(uvcpp_handle *)> handle_close_cb;
    ::std::function<void(uvcpp_handle *, size_t, uv_buf *)> handle_alloc_cb;

private:
    /** @brief Free internal handle memory if owned. */
    void free_handle();
    union uvcpp_handle_union {
        uv_handle_t *uvcpp_handle;
        uv_loop_t *loop;
        uv_fs_event_t *fs_event;
        uv_fs_poll_t *fs_poll;
        uv_async_t *async;
        uv_check_t *check;
        uv_idle_t *idle;
        uv_pipe_t *pipe;
        uv_poll_t *poll;
        uv_prepare_t *prepare;
        uv_process_t *process;
        uv_signal_t *signal;
        uv_tty_t *tty;
        uv_udp_t *udp;
        uv_stream_t *stream;
        uv_tcp_t *tcp;
        uv_timer_t *timer;
    };
    uv_handle_t *_handle = nullptr;
    uvcpp_handle_union _handle_union;
    void *_vdata = nullptr;
    bool _owns_handle = false;
};

} // namespace uvcpp

#endif // SRC_HANDLE_UVCPP_HANDLE_H