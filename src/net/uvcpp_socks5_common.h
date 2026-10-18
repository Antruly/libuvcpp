/**
 * @file src/net/uvcpp_socks5_common.h
 * @brief **私有**：SOCKS5 线格式的编码、增量解码与错误映射。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层不碰套接字、不碰事件循环、不认识 `uvcpp_tcp_client` —— 它只做
 * "字节串 ↔ 结构体"。TCP 客户端、独立 SOCKS 客户端、UDP 封装与服务端共用
 * 同一份实现，避免三处各写一遍然后各错一处。
 *
 * 所有 `parse_*` 都是**增量**的：喂进来的缓冲不够一帧时返回 `NEED_MORE`，
 * 调用方把字节攒着；`consumed` 只有 `OK` 时才有意义。格式错误一律
 * `ERROR`，绝不越界读。
 */

#pragma once
#ifndef SRC_NET_UVCPP_SOCKS5_COMMON_H
#define SRC_NET_UVCPP_SOCKS5_COMMON_H

#include <net/uvcpp_socks5_types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uvcpp {
namespace socks5_detail {

/** @brief 增量解析的三种结果。 */
enum class parse_status {
  NEED_MORE,  ///< 缓冲里还没有完整的一帧
  OK,         ///< 解析出一帧，`consumed` 是它占的字节数
  ERROR       ///< 格式错误（版本/RSV/长度/ATYP）
};

// 各阶段允许的最大帧长（超过即可判定为恶意/错位，直接断连接）。
const std::size_t kMaxGreetingBytes = 257;  // VER + NMETHODS + 255
const std::size_t kMaxAuthBytes     = 513;  // VER + ULEN + 255 + PLEN + 255
const std::size_t kMaxRequestBytes  = 262;  // VER + CMD + RSV + ATYP + 255 + PORT
const std::size_t kMaxUdpHeaderBytes = 262;

/**
 * @brief 由文本主机名定出 ATYP。
 *
 * 含 `:` ⇒ IPv6；能被 `uv_ip4_addr` 接受 ⇒ IPv4；其余 ⇒ 域名（长度 1..255，
 * 超长返回 false）。**不做 DNS**。
 */
bool classify_host(const std::string& host, uvcpp_socks5_atyp& atyp);

/** @brief 把地址（ATYP + ADDR + PORT）追加到 \p out。 */
bool append_address(std::string& out, const uvcpp_socks5_endpoint& ep);

/**
 * @brief 从 \p p 的 \p off 处解一个地址（ATYP 起头）。
 *
 * 成功时 `off` 前进到地址之后（即端口之后）。任何越界/长度非法返回 false。
 */
bool parse_address(const char* p, std::size_t len, std::size_t& off,
                   uvcpp_socks5_endpoint& out);

// ---------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------

/** @brief 问候：VER=5, NMETHODS, METHODS... */
std::string encode_greeting(const std::vector<std::uint8_t>& methods);

/** @brief RFC 1929 认证请求：VER=1, ULEN, UNAME, PLEN, PASSWD。 */
std::string encode_auth_request(const std::string& user,
                                const std::string& password);

/** @brief 请求：VER=5, CMD, RSV=0, ATYP, ADDR, PORT。 */
std::string encode_request(uvcpp_socks5_command cmd,
                           const uvcpp_socks5_endpoint& target);

/** @brief 应答：VER=5, REP, RSV=0, ATYP, BND.ADDR, BND.PORT。 */
std::string encode_reply(uvcpp_socks5_reply rep,
                         const uvcpp_socks5_endpoint& bound);

/** @brief UDP 头 + 载荷：RSV(2)=0, FRAG=0, ATYP, ADDR, PORT, DATA。 */
bool encode_udp_datagram(std::string& out, const uvcpp_socks5_endpoint& dst,
                         const void* data, std::size_t len);

// ---------------------------------------------------------------------------
// 解码
// ---------------------------------------------------------------------------

parse_status parse_greeting(const char* p, std::size_t len,
                            std::size_t& consumed,
                            std::vector<std::uint8_t>& methods);

parse_status parse_method_selection(const char* p, std::size_t len,
                                    std::size_t& consumed,
                                    uvcpp_socks5_method& method);

parse_status parse_auth_request(const char* p, std::size_t len,
                                std::size_t& consumed, std::string& user,
                                std::string& password);

parse_status parse_auth_response(const char* p, std::size_t len,
                                 std::size_t& consumed, std::uint8_t& status);

parse_status parse_request(const char* p, std::size_t len,
                           std::size_t& consumed, uvcpp_socks5_command& cmd,
                           uvcpp_socks5_endpoint& target);

parse_status parse_reply(const char* p, std::size_t len,
                         std::size_t& consumed, uvcpp_socks5_reply& rep,
                         uvcpp_socks5_endpoint& bound);

/**
 * @brief 解一个 UDP 数据报（头 + 载荷）。
 *
 * `payload_off` 是载荷在 \p p 里的偏移。RSV 非 0 或 FRAG 非 0 返回 `ERROR`
 * （本实现不做分片重组，与 RFC 1928 允许的"不支持 FRAG"一致）。
 */
parse_status decode_udp_datagram(const char* p, std::size_t len,
                                 uvcpp_socks5_endpoint& from,
                                 std::size_t& payload_off);

// ---------------------------------------------------------------------------
// 错误映射
// ---------------------------------------------------------------------------

/** @brief 应答码 → libuv 错误码（`SUCCEEDED` → 0）。 */
int reply_to_error(uvcpp_socks5_reply rep);

/** @brief 应答码 → 人类可读文本（日志与 `last_error()` 用）。 */
const char* reply_string(uvcpp_socks5_reply rep);

/** @brief libuv 错误码 → 应答码（服务端把 errno/UV_* 翻成 REP）。 */
uvcpp_socks5_reply error_to_reply(int uv_error);

}  // namespace socks5_detail
}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_SOCKS5_COMMON_H
