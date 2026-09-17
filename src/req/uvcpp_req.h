/**
 * @file src/req/uvcpp_req.h
 * @brief Base wrappers for libuv request types (uv_req_t and friends).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_REQ_UVCPP_REQ_H
#define SRC_REQ_UVCPP_REQ_H

#include <functional>
#include <utility>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>

namespace uvcpp {
#define UVCPP_REQ_REQ reinterpret_cast<uv_req_t *>(this->get_req())
#define UVCPP_CONNECT_REQ reinterpret_cast<uv_connect_t *>(this->get_req())
#define UVCPP_FS_REQ reinterpret_cast<uv_fs_t *>(this->get_req())
#define UVCPP_GETADDRINFO_REQ                                                  \
  reinterpret_cast<uv_getaddrinfo_t *>(this->get_req())
#define UVCPP_GETNAMEINFO_REQ                                                  \
  reinterpret_cast<uv_getnameinfo_t *>(this->get_req())
#define UVCPP_RANDOM_REQ reinterpret_cast<uv_random_t *>(this->get_req())
#define UVCPP_SHUTDOWN_REQ reinterpret_cast<uv_shutdown_t *>(this->get_req())
#define UVCPP_UDP_SEND_REQ reinterpret_cast<uv_udp_send_t *>(this->get_req())
#define UVCPP_WORK_REQ reinterpret_cast<uv_work_t *>(this->get_req())
#define UVCPP_WRITE_REQ reinterpret_cast<uv_write_t *>(this->get_req())

#define OBJ_UVCPP_REQ_REQ(obj) reinterpret_cast<uv_req_t *>((obj).get_req())
#define OBJ_UVCPP_CONNECT_REQ(obj)                                             \
  reinterpret_cast<uv_connect_t *>((obj).get_req())
#define OBJ_UVCPP_FS_REQ(obj) reinterpret_cast<uv_fs_t *>((obj).get_req())
#define OBJ_UVCPP_GETADDRINFO_REQ(obj)                                         \
  reinterpret_cast<uv_getaddrinfo_t *>((obj).get_req())
#define OBJ_UVCPP_GETNAMEINFO_REQ(obj)                                         \
  reinterpret_cast<uv_getnameinfo_t *>((obj).get_req())
#define OBJ_UVCPP_RANDOM_REQ(obj)                                              \
  reinterpret_cast<uv_random_t *>((obj).get_req())
#define OBJ_UVCPP_SHUTDOWN_REQ(obj)                                            \
  reinterpret_cast<uv_shutdown_t *>((obj).get_req())
#define OBJ_UVCPP_UDP_SEND_REQ(obj)                                            \
  reinterpret_cast<uv_udp_send_t *>((obj).get_req())
#define OBJ_UVCPP_WORK_REQ(obj) reinterpret_cast<uv_work_t *>((obj).get_req())
#define OBJ_UVCPP_WRITE_REQ(obj) reinterpret_cast<uv_write_t *>((obj).get_req())

#define DEFINE_FUNC_REQ_CPP(type, uvname)                                      \
  type::type() : uvcpp_req(nullptr) {                                          \
    uvname *r = uvcpp::uv_alloc<uvname>();                                     \
    this->set_req(r);                                                          \
    this->init();                                                              \
  }                                                                            \
  type::type() : uvcpp_req(nullptr) {}                                         \
  type::~type() {}

#define DEFINE_COPY_FUNC_REQ_CPP(type, uvname)                                 \
  type::type(const type &obj) {                                                \
    if (obj.get_req() != nullptr) {                                            \
      uvname *hd = uvcpp::uv_alloc<uvname>();                                  \
      memcpy(hd, obj.get_req(), sizeof(uvname));                               \
      this->set_req(hd);                                                       \
    } else {                                                                   \
      this->set_req(nullptr);                                                  \
    }                                                                          \
  }                                                                            \
  type &type::operator=(const type &obj) {                                     \
    if (obj.get_req() != nullptr) {                                            \
      uvname *hd = uvcpp::uv_alloc<uvname>();                                  \
      memcpy(hd, obj.get_req(), sizeof(uvname));                               \
      this->set_req(hd);                                                       \
    } else {                                                                   \
      this->set_req(nullptr);                                                  \
    }                                                                          \
    return *this;                                                              \
  }

class UVCPP_API uvcpp_req {
public:
  UVCPP_DEFINE_FUNC(uvcpp_req)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_req)

  int set_data(void *pdata);
  void *get_data();
  size_t req_size();
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
  void *req_get_data();
  void req_set_data(void *data);
  const char *req_type_name();
#endif
#endif

  uv_req_type req_get_type();
  int cancel();
  static size_t req_size(uvcpp_req *vReq);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
  static void *req_get_data(const uvcpp_req *vReq);
  static void req_set_data(uvcpp_req *vReq, void *data);
  static const char *req_type_name(uvcpp_req *vReq);
#endif
#endif

  static uv_req_type req_get_type(const uvcpp_req *vReq);

  static int cancel(uvcpp_req *vReq);
  uvcpp_req *clone(uvcpp_req *obj, int memSize);

  virtual uv_req_t *get_req() const;

  /**
   * @brief 置位后，本请求对象在自己的**完成回调返回之后**由跳板自行 `delete`；
   *        置位者就不必（也不能）再在回调里自己删。默认关闭。
   * @note 只对"排他持有请求对象"的用法有意义：置位后使用者不再拥有它。
   */
  void set_self_free(bool on) { self_free_ = on; }
  bool is_self_free() const { return self_free_; }

protected:
  /**
   * @brief 完成回调的统一跳板（`callback_write` / `callback_udp_send` /
   *        `callback_connect` / `callback_random` / `callback_getaddrinfo` /
   *        `callback_getnameinfo` 共用）。末尾的可变参数原样转发给闭包，
   *        给"回调还带额外参数"的那几个用（`buf`/`res`/`hostname` …）。
   *
   * 两件事，顺序都不能反：
   *  1. **先把闭包搬出 `slot` 再调用**。回调里常见最后一句 `delete self`，
   *     而那个闭包就存在 `slot` 里 —— 不搬走的话，删掉的是**此刻正在执行**的
   *     这个 `std::function`，连同它的捕获一起，是未定义行为（只在"删完不再
   *     读捕获"时才不表现为故障）；
   *  2. 闭包返回**之后**按 `self_free_` 决定是否 `delete self`。这个标志必须在
   *     调用**之前**读 —— 回调拿到 `self`，有权把它删掉。
   */
  template <typename TCb, typename TReq, typename... TArgs>
  static void invoke_completion(TCb &slot, TReq *self, int status,
                                TArgs &&...args) {
    TCb cb = ::std::move(slot);
    // **搬完显式清空源。** 移动之后源对象只是"有效但未指定"，标准不保证它空：
    // libc++（macOS）在目标落进小对象缓冲时是**克隆**一份、源仍非空 ——
    // `libcxx/include/__functional/function.h` 的 `__value_func` 移动构造里
    // `__f.__f_ == &__f.__buf_` 那一支不动源的 `__f_`，只有堆上分配的才置空。
    // 于是"搬出去之后槽就空了"这条不变量在 macOS 上不成立（libstdc++ 与 MSVC
    // 恰好一律置空，所以两边看不出来）。上面第 1 条正是本函数存在的理由，
    // 那就自己把它立起来，别借实现自由。
    slot = TCb();
    const bool self_free = self->is_self_free();
    if (cb) {
      cb(self, status, ::std::forward<TArgs>(args)...);
    }
    if (self_free) {
      delete self;
    }
  }

  virtual void set_req(void *r);
  void set_req_data();

private:
  void free_req();

protected:
private:
  uv_req_t *req = nullptr;
  void *vdata = nullptr;
  bool self_free_ = false;  ///< 完成回调之后自我释放（见 set_self_free）
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_REQ_H