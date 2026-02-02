/**
 * @file src/uvcpp/uvcpp_interface_address.h
 * @brief Helper representing a network interface address (wrapping uv_interface_address_t).
 * @author zhuweiye
 * @version 1.0.0
 */

#pragma once
#ifndef SRC_UVCPP_UVCPP_INTERFACE_ADDRESS_H
#define SRC_UVCPP_UVCPP_INTERFACE_ADDRESS_H

#include <uvcpp/uvcpp_define.h>
#include <string>
#include <vector>

namespace uvcpp {
class UVCPP_API uvcpp_interface_address {
public:
  UVCPP_DEFINE_FUNC(uvcpp_interface_address)
  UVCPP_DEFINE_COPY_FUNC(uvcpp_interface_address)

  /** @brief Initialize internal representation. */
  int init();

  /** @brief Get IPv4 addresses as a comma-separated string. */
  ::std::string get_ipv4_addrs();
  /** @brief Get IPv6 addresses as a comma-separated string. */
  ::std::string get_ipv6_addrs();

  /** @brief Return cached list of all interface addresses. */
  static ::std::vector<uvcpp_interface_address> &get_all_interface_addresses();

  /** @brief Access underlying uv_interface_address_t pointer. */
  uv_interface_address_t *get_interface_address() const;

protected:
private:
  uv_interface_address_t *interface_address = nullptr;
};
} // namespace uvcpp

#endif // SRC_UVCPP_UVCPP_INTERFACE_ADDRESS_H