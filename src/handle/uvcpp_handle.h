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
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_alloc.h>

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
// 本宏此前**编不过**：分配器写成 `uvcpp::uv_alloc<T>()`，而项目里叫
// `uvcpp_alloc<T>()`（见 `uvcpp/uvcpp_alloc.h`）—— `uv_alloc` 这个名字全仓不存在。
// 全仓零展开点，所以一直没暴露（与 `uvcpp_req.h` 的 `DEFINE_FUNC_REQ_CPP` 同一类，
// 那个已修）。以同形状的 `DEFINE_COPY_FUNC_REQ_CPP` 为准。
#define DEFINE_COPY_FUNC_HANDLE_CPP(type, uvname)                              \
  type::type(const type &obj) {                                                \
    if (obj.get_handle() != nullptr) {                                         \
      uvname *hd = uvcpp::uvcpp_alloc<uvname>();                               \
      memcpy(hd, obj.get_handle(), sizeof(uvname));                            \
      this->set_handle(hd, true);                                              \
    } else {                                                                   \
      this->set_handle(nullptr, false);                                        \
    }                                                                          \
  }                                                                            \
  type &type::operator=(const type &obj) {                                     \
    if (obj.get_handle() != nullptr) {                                         \
      uvname *hd = uvcpp::uvcpp_alloc<uvname>();                               \
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
    // 构造/析构/拷贝/赋值（拷贝 **删除** —— 理由见 doc/lowlevel-guide.md §10）
    UVCPP_DEFINE_FUNC(uvcpp_handle)
    UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_handle)

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
    /**
     * @brief Close the handle asynchronously.
     *
     * @warning **`close()` 之后不要再调子类的 `stop()`。** 这两者不对称：`close()`
     *          自己判了空（`_handle == nullptr` 直接返回），而七个"只有 stop"的
     *          子类 —— `uvcpp_timer` / `uvcpp_check` / `uvcpp_prepare` /
     *          `uvcpp_signal` / `uvcpp_poll` / `uvcpp_fs_event` / `uvcpp_fs_poll`
     *          —— 的 `stop()` 都是**直接把 `_handle` 递给 libuv**，一个守卫都没有。
     *          关闭完成时 `callback_close` 会把 `_handle` 置空，此后 `stop()` 就是
     *          把空指针交给 `uv_*_stop()` ⇒ 空指针解引用。
     *          （`uvcpp_idle::stop()` 是例外：它走 `is_closing()` 判断后转调
     *          `close()`。）关之前也不必先 `stop()` —— `uv_close()` 自己会停掉
     *          活动，之后不再有回调。
     */
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
    /** @brief 按字节克隆一个句柄包装对象 —— **已删除（`= delete`）**。
     *
     *  这个操作不可能有正确实现，所以留着声明是为了让任何调用变成**编译错误**，
     *  而不是留一个只在运行期咬人的陷阱：`memcpy` 出的那份外壳与源对象共享同一个
     *  `_handle`，而 `_owns_handle` 也按字节复制 ⇒ **两个所有者、一块内存**，
     *  两边析构时 `free_handle()` 会去还同一块 —— 双重释放。没有任何赋值方式能
     *  把这两者分开，因为「要不要还」这件事本身就该是单数的。
     */
    static uvcpp_handle *clone(uvcpp_handle *obj, int memSize) = delete;

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
    /** @brief 无参 init() 里的"清零 + 挂 data"步骤，整合到一处。
     *
     *  **句柄已经被 libuv 接管时这一步是空操作**，这是本函数的全部意义所在。
     *  `uv_*_init` 会把 `handle_queue` 的前后指针和 `loop` 反向指针写进这块
     *  内存；在它之后再 memset 等于把这些链接抹掉 —— 循环那头的
     *  `handle_queue` 仍然指向这块内存，而 `free_handle()` 读到 `loop == nullptr`
     *  就会判定"从没 uv_*_init 过"，走最后一条分支直接 `UVCPP_VFREE`。
     *  于是把一块**仍挂在队列上**的内存还给了分配器，下一次 `uv_*_init` 往
     *  队尾插入时写进已释放内存：page heap 下必崩（`tests/unit/handle_unit.cpp`
     *  正是这么被抓住的），普通堆下只是静默破坏队列、`uv_loop_close` 从此
     *  再也关不掉那个循环。
     *
     *  判据可信：底层内存来自 `uvcpp_alloc`，出生即全零（见 uvcpp_alloc.h），
     *  所以 `loop` 非空 <=> libuv 确实初始化过它，不存在"读到垃圾值"的风险。
     */
    void reset_handle_state(void *handle, size_t size);
    /** @brief Internal allocation callback forwarding to wrapper. */
    static void callback_alloc(uv_handle_t *uvcpp_handle, size_t suggested_size, uv_buf_t *buf);
    /** @brief Internal close callback used to free wrapped resources. */
    static void callback_close(uv_handle_t *uvcpp_handle);
    /** @brief Replace the underlying handle pointer (no ownership flag). */
    virtual void set_handle(void *hd);
    /** @brief Replace the underlying handle pointer with ownership flag. */
    virtual void set_handle(void *hd, bool owns);
    /** @brief Detach the underlying handle pointer without running teardown.
     *  For subclasses (uvcpp_loop) that wrap a non-handle payload (uv_loop_t)
     *  and free it themselves, so the base free_handle() sees a null handle. */
    void detach_handle() {
      _handle = nullptr;
      _owns_handle = false;
    }
    ::std::function<void(uvcpp_handle *)> handle_close_cb;
    ::std::function<void(uvcpp_handle *, size_t, uv_buf_t*)> handle_alloc_cb;

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