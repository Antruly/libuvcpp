/**
 * @file src/net/uvcpp_socks5_common.cpp
 * @brief SOCKS5 线格式编解码（私有）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <net/uvcpp_socks5_common.h>

#include <uv.h>

#include <cstring>

namespace uvcpp {
namespace socks5_detail {

namespace {

void append_port(std::string& out, int port) {
  const std::uint16_t net_port = htons(static_cast<std::uint16_t>(port));
  out.append(reinterpret_cast<const char*>(&net_port), sizeof(net_port));
}

bool read_u8(const char* p, std::size_t len, std::size_t& off,
             std::uint8_t& v) {
  if (off >= len) return false;
  v = static_cast<std::uint8_t>(p[off]);
  ++off;
  return true;
}

bool read_port(const char* p, std::size_t len, std::size_t& off, int& port) {
  if (off + 2 > len) return false;
  std::uint16_t net_port = 0;
  std::memcpy(&net_port, p + off, sizeof(net_port));
  off += 2;
  port = static_cast<int>(ntohs(net_port));
  return true;
}

bool read_bytes(const char* p, std::size_t len, std::size_t& off,
                std::size_t n, std::string& out) {
  if (off + n > len) return false;
  out.assign(p + off, n);
  off += n;
  return true;
}

/**
 * @brief `parse_address` 的三态版本。
 *
 * 公开的 `parse_address` 只答"成了没有"，但上层的增量解析必须分清「字节还没
 * 到齐」与「这帧本身是坏的」—— 前者要攒着等下一次读，后者要断连接。
 */
parse_status parse_address_ex(const char* p, std::size_t len, std::size_t& off,
                              uvcpp_socks5_endpoint& out) {
  std::uint8_t atyp = 0;
  if (!read_u8(p, len, off, atyp)) return parse_status::NEED_MORE;

  switch (static_cast<uvcpp_socks5_atyp>(atyp)) {
    case uvcpp_socks5_atyp::IPV4: {
      if (off + 4 > len) return parse_status::NEED_MORE;
      struct sockaddr_in a4;
      std::memset(&a4, 0, sizeof(a4));
      a4.sin_family = AF_INET;
      std::memcpy(&a4.sin_addr, p + off, 4);
      off += 4;
      char text[INET_ADDRSTRLEN] = {0};
      if (uv_ip4_name(&a4, text, sizeof(text)) != 0) return parse_status::ERROR;
      int port = 0;
      if (!read_port(p, len, off, port)) return parse_status::NEED_MORE;
      out.atyp = uvcpp_socks5_atyp::IPV4;
      out.host = text;
      out.port = port;
      return parse_status::OK;
    }
    case uvcpp_socks5_atyp::IPV6: {
      if (off + 16 > len) return parse_status::NEED_MORE;
      struct sockaddr_in6 a6;
      std::memset(&a6, 0, sizeof(a6));
      a6.sin6_family = AF_INET6;
      std::memcpy(&a6.sin6_addr, p + off, 16);
      off += 16;
      char text[INET6_ADDRSTRLEN] = {0};
      if (uv_ip6_name(&a6, text, sizeof(text)) != 0) return parse_status::ERROR;
      int port = 0;
      if (!read_port(p, len, off, port)) return parse_status::NEED_MORE;
      out.atyp = uvcpp_socks5_atyp::IPV6;
      out.host = text;
      out.port = port;
      return parse_status::OK;
    }
    case uvcpp_socks5_atyp::DOMAIN: {
      std::uint8_t n = 0;
      if (!read_u8(p, len, off, n)) return parse_status::NEED_MORE;
      if (n == 0) return parse_status::ERROR;
      if (off + n > len) return parse_status::NEED_MORE;
      std::string host(p + off, n);
      off += n;
      int port = 0;
      if (!read_port(p, len, off, port)) return parse_status::NEED_MORE;
      out.atyp = uvcpp_socks5_atyp::DOMAIN;
      out.host.swap(host);
      out.port = port;
      return parse_status::OK;
    }
  }
  return parse_status::ERROR;  // 0x00 / 0x02 / 0x05... 都不是合法 ATYP
}

}  // namespace

bool classify_host(const std::string& host, uvcpp_socks5_atyp& atyp) {
  if (host.empty()) return false;
  if (host.find(':') != std::string::npos) {
    struct sockaddr_in6 a6;
    if (uv_ip6_addr(host.c_str(), 0, &a6) != 0) return false;
    atyp = uvcpp_socks5_atyp::IPV6;
    return true;
  }
  struct sockaddr_in a4;
  if (uv_ip4_addr(host.c_str(), 0, &a4) == 0) {
    atyp = uvcpp_socks5_atyp::IPV4;
    return true;
  }
  if (host.size() > 255) return false;
  atyp = uvcpp_socks5_atyp::DOMAIN;
  return true;
}

bool append_address(std::string& out, const uvcpp_socks5_endpoint& ep) {
  switch (ep.atyp) {
    case uvcpp_socks5_atyp::IPV4: {
      struct sockaddr_in a4;
      if (uv_ip4_addr(ep.host.c_str(), ep.port, &a4) != 0) return false;
      out.push_back(static_cast<char>(uvcpp_socks5_atyp::IPV4));
      out.append(reinterpret_cast<const char*>(&a4.sin_addr),
                 sizeof(a4.sin_addr));
      append_port(out, ep.port);
      return true;
    }
    case uvcpp_socks5_atyp::IPV6: {
      struct sockaddr_in6 a6;
      if (uv_ip6_addr(ep.host.c_str(), ep.port, &a6) != 0) return false;
      out.push_back(static_cast<char>(uvcpp_socks5_atyp::IPV6));
      out.append(reinterpret_cast<const char*>(&a6.sin6_addr),
                 sizeof(a6.sin6_addr));
      append_port(out, ep.port);
      return true;
    }
    case uvcpp_socks5_atyp::DOMAIN:
      if (ep.host.empty() || ep.host.size() > 255) return false;
      out.push_back(static_cast<char>(uvcpp_socks5_atyp::DOMAIN));
      out.push_back(static_cast<char>(ep.host.size()));
      out.append(ep.host);
      append_port(out, ep.port);
      return true;
  }
  return false;
}

bool parse_address(const char* p, std::size_t len, std::size_t& off,
                   uvcpp_socks5_endpoint& out) {
  return parse_address_ex(p, len, off, out) == parse_status::OK;
}

std::string encode_greeting(const std::vector<std::uint8_t>& methods) {
  std::string out;
  if (methods.empty() || methods.size() > 255) return out;
  out.push_back(0x05);
  out.push_back(static_cast<char>(methods.size()));
  for (std::size_t i = 0; i < methods.size(); ++i) {
    out.push_back(static_cast<char>(methods[i]));
  }
  return out;
}

std::string encode_auth_request(const std::string& user,
                                const std::string& password) {
  if (user.empty() || user.size() > 255) return std::string();
  if (password.size() > 255) return std::string();
  std::string out;
  out.push_back(0x01);
  out.push_back(static_cast<char>(user.size()));
  out.append(user);
  out.push_back(static_cast<char>(password.size()));
  out.append(password);
  return out;
}

std::string encode_request(uvcpp_socks5_command cmd,
                           const uvcpp_socks5_endpoint& target) {
  std::string out;
  out.push_back(0x05);
  out.push_back(static_cast<char>(cmd));
  out.push_back(0x00);
  if (!append_address(out, target)) return std::string();
  return out;
}

std::string encode_reply(uvcpp_socks5_reply rep,
                         const uvcpp_socks5_endpoint& bound) {
  std::string out;
  out.push_back(0x05);
  out.push_back(static_cast<char>(rep));
  out.push_back(0x00);
  if (!append_address(out, bound)) return std::string();
  return out;
}

bool encode_udp_datagram(std::string& out, const uvcpp_socks5_endpoint& dst,
                         const void* data, std::size_t len) {
  out.clear();
  out.push_back('\0');
  out.push_back('\0');
  out.push_back('\0');  // FRAG = 0，不分片
  if (!append_address(out, dst)) return false;
  if (len > 0) {
    out.append(static_cast<const char*>(data), len);
  }
  return true;
}

parse_status parse_greeting(const char* p, std::size_t len,
                            std::size_t& consumed,
                            std::vector<std::uint8_t>& methods) {
  consumed = 0;
  if (len > kMaxGreetingBytes) return parse_status::ERROR;
  if (len < 2) return parse_status::NEED_MORE;
  if (static_cast<std::uint8_t>(p[0]) != 0x05) return parse_status::ERROR;
  const std::size_t n = static_cast<std::uint8_t>(p[1]);
  if (n == 0) return parse_status::ERROR;
  const std::size_t total = 2 + n;
  if (len < total) return parse_status::NEED_MORE;
  methods.assign(p + 2, p + total);
  consumed = total;
  return parse_status::OK;
}

parse_status parse_method_selection(const char* p, std::size_t len,
                                    std::size_t& consumed,
                                    uvcpp_socks5_method& method) {
  consumed = 0;
  if (len < 2) return parse_status::NEED_MORE;
  if (static_cast<std::uint8_t>(p[0]) != 0x05) return parse_status::ERROR;
  method = static_cast<uvcpp_socks5_method>(static_cast<std::uint8_t>(p[1]));
  consumed = 2;
  return parse_status::OK;
}

parse_status parse_auth_request(const char* p, std::size_t len,
                                std::size_t& consumed, std::string& user,
                                std::string& password) {
  consumed = 0;
  if (len > kMaxAuthBytes) return parse_status::ERROR;
  std::size_t off = 0;
  std::uint8_t ver = 0;
  std::uint8_t ulen = 0;
  if (!read_u8(p, len, off, ver)) return parse_status::NEED_MORE;
  if (ver != 0x01) return parse_status::ERROR;
  if (!read_u8(p, len, off, ulen)) return parse_status::NEED_MORE;
  std::string u;
  if (!read_bytes(p, len, off, ulen, u)) return parse_status::NEED_MORE;
  std::uint8_t plen = 0;
  if (!read_u8(p, len, off, plen)) return parse_status::NEED_MORE;
  std::string pw;
  if (!read_bytes(p, len, off, plen, pw)) return parse_status::NEED_MORE;
  user.swap(u);
  password.swap(pw);
  consumed = off;
  return parse_status::OK;
}

parse_status parse_auth_response(const char* p, std::size_t len,
                                 std::size_t& consumed, std::uint8_t& status) {
  consumed = 0;
  if (len < 2) return parse_status::NEED_MORE;
  if (static_cast<std::uint8_t>(p[0]) != 0x01) return parse_status::ERROR;
  status = static_cast<std::uint8_t>(p[1]);
  consumed = 2;
  return parse_status::OK;
}

parse_status parse_request(const char* p, std::size_t len,
                           std::size_t& consumed, uvcpp_socks5_command& cmd,
                           uvcpp_socks5_endpoint& target) {
  consumed = 0;
  if (len > kMaxRequestBytes) return parse_status::ERROR;
  if (len < 4) return parse_status::NEED_MORE;
  if (static_cast<std::uint8_t>(p[0]) != 0x05) return parse_status::ERROR;
  if (static_cast<std::uint8_t>(p[2]) != 0x00) return parse_status::ERROR;  // RSV
  cmd = static_cast<uvcpp_socks5_command>(static_cast<std::uint8_t>(p[1]));
  std::size_t off = 3;
  const parse_status st = parse_address_ex(p, len, off, target);
  if (st != parse_status::OK) return st;
  consumed = off;
  return parse_status::OK;
}

parse_status parse_reply(const char* p, std::size_t len,
                         std::size_t& consumed, uvcpp_socks5_reply& rep,
                         uvcpp_socks5_endpoint& bound) {
  consumed = 0;
  if (len < 4) return parse_status::NEED_MORE;
  if (static_cast<std::uint8_t>(p[0]) != 0x05) return parse_status::ERROR;
  if (static_cast<std::uint8_t>(p[2]) != 0x00) return parse_status::ERROR;
  rep = static_cast<uvcpp_socks5_reply>(static_cast<std::uint8_t>(p[1]));
  std::size_t off = 3;
  const parse_status st = parse_address_ex(p, len, off, bound);
  if (st != parse_status::OK) return st;
  consumed = off;
  return parse_status::OK;
}

parse_status decode_udp_datagram(const char* p, std::size_t len,
                                 uvcpp_socks5_endpoint& from,
                                 std::size_t& payload_off) {
  payload_off = 0;
  if (len < 4) return parse_status::NEED_MORE;
  if (p[0] != '\0' || p[1] != '\0') return parse_status::ERROR;  // RSV
  if (p[2] != '\0') return parse_status::ERROR;                  // FRAG != 0
  std::size_t off = 3;
  const parse_status st = parse_address_ex(p, len, off, from);
  if (st != parse_status::OK) return st;
  payload_off = off;
  return parse_status::OK;
}

int reply_to_error(uvcpp_socks5_reply rep) {
  switch (rep) {
    case uvcpp_socks5_reply::SUCCEEDED:
      return 0;
    case uvcpp_socks5_reply::GENERAL_FAILURE:
      return UV_EIO;
    case uvcpp_socks5_reply::NOT_ALLOWED:
      return UV_EACCES;
    case uvcpp_socks5_reply::NETWORK_UNREACHABLE:
      return UV_ENETUNREACH;
    case uvcpp_socks5_reply::HOST_UNREACHABLE:
      return UV_EHOSTUNREACH;
    case uvcpp_socks5_reply::CONNECTION_REFUSED:
      return UV_ECONNREFUSED;
    case uvcpp_socks5_reply::TTL_EXPIRED:
      return UV_ETIMEDOUT;
    case uvcpp_socks5_reply::COMMAND_UNSUPPORTED:
      return UV_ENOTSUP;
    case uvcpp_socks5_reply::ATYP_UNSUPPORTED:
      return UV_EAFNOSUPPORT;
  }
  return UV_EPROTO;
}

const char* reply_string(uvcpp_socks5_reply rep) {
  switch (rep) {
    case uvcpp_socks5_reply::SUCCEEDED:
      return "succeeded";
    case uvcpp_socks5_reply::GENERAL_FAILURE:
      return "general SOCKS server failure";
    case uvcpp_socks5_reply::NOT_ALLOWED:
      return "connection not allowed by ruleset";
    case uvcpp_socks5_reply::NETWORK_UNREACHABLE:
      return "network unreachable";
    case uvcpp_socks5_reply::HOST_UNREACHABLE:
      return "host unreachable";
    case uvcpp_socks5_reply::CONNECTION_REFUSED:
      return "connection refused";
    case uvcpp_socks5_reply::TTL_EXPIRED:
      return "TTL expired";
    case uvcpp_socks5_reply::COMMAND_UNSUPPORTED:
      return "command not supported";
    case uvcpp_socks5_reply::ATYP_UNSUPPORTED:
      return "address type not supported";
  }
  return "unknown reply";
}

uvcpp_socks5_reply error_to_reply(int uv_error) {
  switch (uv_error) {
    case 0:
      return uvcpp_socks5_reply::SUCCEEDED;
    case UV_ECONNREFUSED:
      return uvcpp_socks5_reply::CONNECTION_REFUSED;
    case UV_ENETUNREACH:
      return uvcpp_socks5_reply::NETWORK_UNREACHABLE;
    case UV_EHOSTUNREACH:
    case UV_ETIMEDOUT:
#ifdef UV_EAI_NONAME
    case UV_EAI_NONAME:
#endif
#ifdef UV_EAI_AGAIN
    case UV_EAI_AGAIN:
#endif
      return uvcpp_socks5_reply::HOST_UNREACHABLE;
    case UV_EACCES:
    case UV_EPERM:
      return uvcpp_socks5_reply::NOT_ALLOWED;
    case UV_EAFNOSUPPORT:
      return uvcpp_socks5_reply::ATYP_UNSUPPORTED;
    case UV_ENOTSUP:
      return uvcpp_socks5_reply::COMMAND_UNSUPPORTED;
    default:
      return uvcpp_socks5_reply::GENERAL_FAILURE;
  }
}

}  // namespace socks5_detail
}  // namespace uvcpp
