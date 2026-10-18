#include "uvcpp_interface_address.h"
#include <uvcpp/uvcpp_alloc.h>
#include <mutex>
namespace uvcpp {
uvcpp_interface_address::uvcpp_interface_address(
    const uvcpp_interface_address &addrs) {
  interface_address = uvcpp::uvcpp_alloc<uv_interface_address_t>();
  memcpy(interface_address, addrs.interface_address, sizeof(uv_interface_address_t));
}
uvcpp_interface_address &
uvcpp_interface_address::operator=(const uvcpp_interface_address &addrs) {
  memcpy(this->interface_address, addrs.interface_address,
         sizeof(uv_interface_address_t));
  return *this;
}
::std::vector<uvcpp_interface_address> &
uvcpp_interface_address::get_all_interface_addresses() {
  // **只建一次**。旧形状是每次调用先 `clear()` 再 `push_back`：两个事件循环
  // 同时问的时候，一个正在 `push_back`、另一个刚把表清空 —— 那不只是数据
  // 竞态，迭代中的调用方还会读到已析构的元素（每个元素持有一块
  // `uv_interface_address_t`）。头里写的本来就是 "cached list"，而接口列表
  // 随**系统配置**变，不是随调用变，所以 `call_once` 是这里诚实的语义。
  //
  // 返回值仍是引用：这是公开签名，改成按值返回会改变调用约定，而返回类型
  // **不进** Itanium 的符号名 —— 用旧头配新 `.so` 会静默按引用去读一个
  // 按值返回的对象。所以约定写在这里：拿到的表**只读**。
  static ::std::vector<uvcpp_interface_address> addrs;
  static ::std::once_flag built;
  ::std::call_once(built, [] {
    uv_interface_address_t *paddrs;
    int count = 0;
    uv_interface_addresses(&paddrs, &count);
    for (int i = 0; i < count; ++i) {
      uvcpp_interface_address vaddrs;
      if (vaddrs.interface_address == nullptr) {
        vaddrs.interface_address = uvcpp::uvcpp_alloc<uv_interface_address_t>();
      }
      memcpy(vaddrs.interface_address, &paddrs[i],
             sizeof(uv_interface_address_t));
      addrs.push_back(vaddrs);
    }
    uv_free_interface_addresses(paddrs, count);
  });
  return addrs;
}
uv_interface_address_t *uvcpp_interface_address::get_interface_address() const {
  return this->interface_address;
}
uvcpp_interface_address::uvcpp_interface_address() {
  this->interface_address = uvcpp::uvcpp_alloc<uv_interface_address_t>();
}
uvcpp_interface_address::~uvcpp_interface_address() {
  UVCPP_VFREE(this->interface_address);
}
int uvcpp_interface_address::init() {
  memset(this->interface_address, 0, sizeof(uv_interface_address_t));
  return 0;
}
::std::string uvcpp_interface_address::get_ipv4_addrs() {
  char buffer[512] = {0};
  if (this->interface_address->address.address4.sin_family == AF_INET) {
    uv_ip4_name(&this->interface_address->address.address4, buffer,
                sizeof(buffer));
  }
  ::std::string strRet(buffer);
  return strRet;
}
::std::string uvcpp_interface_address::get_ipv6_addrs() {
  char buffer[512] = {0};
  if (this->interface_address->address.address4.sin_family == AF_INET6) {
    uv_ip6_name(&this->interface_address->address.address6, buffer,
                sizeof(buffer));
  }
  ::std::string strRet(buffer);
  return strRet;
}
} // namespace uvcpp