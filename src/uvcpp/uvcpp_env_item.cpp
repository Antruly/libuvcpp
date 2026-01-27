#include "uvcpp_env_item.h"
#include <uvcpp/uv_alloc.h>
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 30
namespace uvcpp {
uvcpp_env_item::uvcpp_env_item() {
  this->env_item = uvcpp::uv_alloc<uv_env_item_t>();
  this->init();
}

uvcpp_env_item::~uvcpp_env_item() { UVCPP_VFREE(this->env_item); }

uvcpp_env_item::uvcpp_env_item(const uvcpp_env_item& obj) {
  if (this->env_item != nullptr) {
    uv_env_item_t* hd = uvcpp::uv_alloc<uv_env_item_t>();
    memcpy(hd, this->env_item, sizeof(uv_env_item_t));
    this->env_item = hd;
  } else {
    this->env_item = nullptr;
  }
}
uvcpp_env_item& uvcpp_env_item::operator=(const uvcpp_env_item& obj) {
  if (this->env_item != nullptr) {
    uv_env_item_t* hd = uvcpp::uv_alloc<uv_env_item_t>();
    memcpy(hd, this->env_item, sizeof(uv_env_item_t));
    this->env_item = hd;
  } else {
    this->env_item = nullptr;
  }
  return *this;
}

int uvcpp_env_item::init() {
  memset(this->env_item, 0, sizeof(uv_env_item_t));
  return 0;
}
int uvcpp_env_item::os_environ(int *count) {
  return uv_os_environ(&this->env_item, count);
}
void uvcpp_env_item::free_environ(int count) {
  uv_os_free_environ(this->env_item, count);
}
int uvcpp_env_item::getenv(const char *name, char *buffer, size_t *size) {
  return uv_os_getenv(name, buffer, size);
}
int uvcpp_env_item::setenv(const char *name, const char *value) {
  return uv_os_setenv(name, value);
}
int uvcpp_env_item::unsetenv(const char *name) { return uv_os_unsetenv(name); }
} // namespace uvcpp
#endif
#endif