#include "uvcpp_interface_address.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_interface_address::uvcpp_interface_address(
    const uvcpp_interface_address &addrs) {
  interface_address = uvcpp::uv_alloc<uv_interface_address_t>();
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
  static ::std::vector<uvcpp_interface_address> addrs;
  addrs.clear();
  uv_interface_address_t *paddrs;
  int count = 0;
  uv_interface_addresses(&paddrs, &count);
  for (int i = 0; i < count; ++i) {
    uvcpp_interface_address vaddrs;
    if (vaddrs.interface_address == nullptr) {
      vaddrs.interface_address = uvcpp::uv_alloc<uv_interface_address_t>();
    }
    memcpy(vaddrs.interface_address, &paddrs[i],
           sizeof(uv_interface_address_t));
    addrs.push_back(vaddrs);
  }
  uv_free_interface_addresses(paddrs, count);
  return addrs;
}
uv_interface_address_t *uvcpp_interface_address::get_interface_address() const {
  return this->interface_address;
}
uvcpp_interface_address::uvcpp_interface_address() {
  this->interface_address = uvcpp::uv_alloc<uv_interface_address_t>();
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