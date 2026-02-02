#include "uvcpp_req.h"
#include <uvcpp/uvcpp_alloc.h>
namespace uvcpp {
uvcpp_req::uvcpp_req(): req(nullptr) ,vdata(nullptr){
  req = uvcpp::uvcpp_alloc<uv_req_t>();
  this->set_req_data();
}

int uvcpp_req::set_data(void *pdata) {
  vdata = pdata;
  return 0;
}

void *uvcpp_req::get_data() { return vdata; }

uvcpp_req::~uvcpp_req() { free_req(); }
uvcpp_req::uvcpp_req(const uvcpp_req &obj) {
  if (obj.req != nullptr) {
    size_t sz = uv_req_size(obj.req->type);
    req = (uv_req_t *)uvcpp::uvcpp_alloc_bytes(sz);
    if (req == nullptr)
      throw std::bad_alloc();
    memcpy(req, obj.req, sz);
    this->set_req_data();
  } else {
    req = nullptr;
  }
}

uvcpp_req &uvcpp_req::operator=(const uvcpp_req &obj) {
  this->free_req();
  if (obj.req != nullptr) {
    size_t sz = uv_req_size(obj.req->type);
    req = (uv_req_t *)uvcpp::uvcpp_alloc_bytes(sz);
    if (req == nullptr)
      throw std::bad_alloc();
    memcpy(req, obj.req, sz);
    this->set_req_data();
  } else {
    req = nullptr;
  }
  return *this;
}

// non-static member req_size — returns size of underlying libuv req type
size_t uvcpp_req::req_size() { return req ? uv_req_size(req->type) : 0; }

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
void *uvcpp_req::req_get_data() { return uv_req_get_data(req); }

void uvcpp_req::req_set_data(void *data) { uv_req_set_data(req, data); }

const char *uvcpp_req::req_type_name() { return uv_req_type_name(req->type); }
#endif
#endif

uv_req_type uvcpp_req::req_get_type() { return req->type; }

int uvcpp_req::cancel() { return uv_cancel(req); }

size_t uvcpp_req::req_size(uvcpp_req *vReq) {
  return uv_req_size(vReq->req->type);
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
void *uvcpp_req::req_get_data(const uvcpp_req *vReq) {
  return uv_req_get_data(vReq->req);
}

void uvcpp_req::req_set_data(uvcpp_req *vReq, void *data) {
  uv_req_set_data(vReq->req, data);
}
const char *uvcpp_req::req_type_name(uvcpp_req *vReq) {
  return uv_req_type_name(vReq->req->type);
}
#endif
#endif

uv_req_type uvcpp_req::req_get_type(const uvcpp_req *vReq) {
  return vReq->req->type;
}

uv_req_t *uvcpp_req::get_req() const { return req; }

void uvcpp_req::set_req(void *r) {
  free_req();
  req = (uv_req_t *)r;
  this->set_req_data();
  return;
}

void uvcpp_req::set_req_data() { req->data = this; }

void uvcpp_req::free_req() {
  if (req != nullptr) {
    UVCPP_VFREE(req)
  }
}

int uvcpp_req::cancel(uvcpp_req *vReq) { return uv_cancel(vReq->req); }

uvcpp_req *uvcpp_req::clone(uvcpp_req *obj, int memSize) {
  uvcpp_req *newObj = (uvcpp_req *)new char[memSize];
  memcpy(newObj, obj, memSize);
  return newObj;
}
} // namespace uvcpp