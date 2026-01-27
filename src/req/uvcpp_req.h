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

protected:
  virtual void set_req(void *r);
  void set_req_data();

private:
  void free_req();

protected:
private:
  uv_req_t *req = nullptr;
  void *vdata = nullptr;
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_REQ_H