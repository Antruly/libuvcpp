/**
 * @file src/ssl/uvcpp_ssl_context.h
 * @brief SSL/TLS context — wraps OpenSSL SSL_CTX.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Manages SSL_CTX lifecycle, certificate/key loading, protocol
 * configuration, and verification settings.  One context can be shared
 * across multiple connections.
 */

#pragma once
#ifndef SRC_SSL_UVCPP_SSL_CONTEXT_H
#define SRC_SSL_UVCPP_SSL_CONTEXT_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_OPENSSL_ENABLE

#include <functional>
#include <string>
#include <vector>
#include <uvcpp/uvcpp_define.h>
#include <ssl/uvcpp_ssl_common.h>

// Forward declarations for OpenSSL types (defined in .cpp via <openssl/ssl.h>)
struct ssl_ctx_st;

namespace uvcpp {

class UVCPP_API uvcpp_ssl_context {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_ssl_context)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ssl_context)

  explicit uvcpp_ssl_context(tls_mode mode,
                              tls_version ver = tls_version::TLS_1_2);

  // -------------------------------------------------------------------
  // Certificate configuration
  // -------------------------------------------------------------------

  /**
   * @brief Load certificate chain from PEM file.
   *
   * **调用顺序：先装证书，再装私钥。** 两个 `load_private_key_*` 内部都会调
   * `SSL_CTX_check_private_key`，而 OpenSSL 在「尚无证书」时该调用返回 0 ——
   * 于是先装私钥**会返回 `false`，但私钥其实已经装进上下文了**：报的是配对检查
   * 失败，状态却已经改了。补装证书之后不用重装私钥。
   */
  bool load_certificate_file(const std::string& path);

  /** @brief Load private key from PEM file. **先装证书再调它**，见上。 */
  bool load_private_key_file(const std::string& path);

  /** @brief Load certificate chain from PEM string. */
  bool load_certificate_data(const std::string& pem);

  /** @brief Load private key from PEM string. **先装证书再调它**，见上。 */
  bool load_private_key_data(const std::string& pem);

  /**
   * @brief Generate a self-signed certificate (for testing).
   * @param common_name  CN field (e.g. "localhost").
   * @param bits         RSA key size (default 2048).
   */
  bool generate_self_signed(const std::string& common_name, int bits = 2048);

  /**
   * @brief 检查已装入的私钥与证书是否配对（`SSL_CTX_check_private_key`）。
   *
   * 为什么值得有一个单独的方法：证书和私钥**不配对**（部署时把两套环境的
   * 文件配到一起，是很常见的手误）在 OpenSSL 里不影响 `SSL_CTX_new`、
   * 也不影响两个 `load_*` 的返回值 —— 它只在**每一次握手**时才失败。于是
   * 表现是"服务起来了、端口在听、每条连接建完就被拒"，很难往配置上想。
   * 在配置阶段调一次就能把它变成一个明确的启动错误。
   *
   * @return 配对返回 true；没装证书/私钥，或两者不配对，返回 false。
   */
  bool check_private_key();

  // -------------------------------------------------------------------
  // CA / verification
  // -------------------------------------------------------------------

  /** @brief Load trusted CA certificates from PEM file. */
  bool load_ca_file(const std::string& path);

  /**
   * @brief Set peer certificate verification mode.
   *
   * 默认值：CLIENT 上下文是 `PEER`（并已装入系统信任库），SERVER 是 `NONE`。
   * 回环自签用例要在 CLIENT 上显式设 `NONE` —— 关掉校验必须是一个看得见的
   * 动作。
   */
  void set_verify_mode(tls_verify_mode mode);

  /**
   * @brief 当前的对端证书校验档位。
   *
   * 存在的理由：`PEER_STRICT` 那**多出来的一半**（主机名校验）不落在 `SSL_CTX` 上，
   * 而是每条连接各自钉在它的 `SSL*` 上，所以"要不要钉"得由**建连接的那一层**去问
   * 上下文当前的档位。构造期的默认值也走这条路（CLIENT = `PEER`，SERVER = `NONE`，
   * 与 `set_default_verify()` 一致）。
   */
  tls_verify_mode verify_mode() const { return verify_mode_; }

  // -------------------------------------------------------------------
  // Protocol
  // -------------------------------------------------------------------

  /** @brief Set allowed TLS protocol version range. */
  void set_min_version(tls_version ver);
  void set_max_version(tls_version ver);

  /** @brief Set cipher list (OpenSSL format, e.g. "HIGH:!aNULL"). */
  bool set_cipher_list(const std::string& ciphers);

  // -------------------------------------------------------------------
  // ALPN
  // -------------------------------------------------------------------
  //
  // 两个方向分开，因为 OpenSSL 的接口本来就不一样：客户端是"宣告我支持什么"，
  // 服务端是"从对方给的里面挑一个"。顺序都即优先级。

  /**
   * @brief 客户端：宣告本端支持的协议名，例如 {"h2", "http/1.1"}。
   * @return 列表为空（编码后为空）返回 false；否则 true。
   */
  bool set_alpn_protos(const std::vector<std::string>& protos);

  /**
   * @brief 服务端：给出可选的协议名并安装选择回调。
   *
   * **挑不中时回调返回 `SSL_TLSEXT_ERR_NOACK`，不是 fatal。** 这正是"不带 ALPN
   * 的 HTTP/1.1 客户端照常握手成功、由上层回落 h1"的实现方式；写成 fatal 会让
   * 所有老客户端连握手都完不成。无扩展、有扩展但无交集都走这条路。
   */
  void set_alpn_select_protos(const std::vector<std::string>& protos);

  /**
   * @brief 是否已经装过服务端 ALPN 名单。
   *
   * 给上层用的：`uvcpp_web_app` 默认要替用户宣告 h2，但**不能覆盖用户自己
   * 显式设过的名单**（那是他的协议策略，比框架的默认更权威）。没有这个查询口，
   * 框架只能二选一：要么无条件覆盖，要么一律不设 —— 前者静默改掉用户的配置，
   * 后者让"默认支持 h2"落空。
   */
  bool has_alpn_select() const { return !alpn_select_wire_.empty(); }

  // -------------------------------------------------------------------
  // Status
  // -------------------------------------------------------------------

  bool is_ready() const;
  int get_status() const;
  std::string get_last_error() const;

  // -------------------------------------------------------------------
  // Internal access (for uvcpp_ssl)
  // -------------------------------------------------------------------
  ssl_ctx_st* raw_ctx() const { return ctx_; }
  tls_mode get_mode() const { return mode_; }

 private:
  void init_client();
  void init_server();
  void set_default_verify();
  void clear_error();

  ssl_ctx_st* ctx_ = nullptr;
  tls_mode    mode_;
  tls_version min_version_;
  // 与 `mode_`（CLIENT/SERVER）是两件事，名字相近但别混。`SSL_CTX_set_verify` 只
  // 表达得了一半（`PEER_STRICT` 的主机名那一半在每条连接的 `SSL*` 上），所以这里
  // 记的是**原始档位**，供 `verify_mode()` 查。
  tls_verify_mode verify_mode_ = tls_verify_mode::NONE;
  int status_ = TLS_CTX_NONE;
  std::string last_error_;
  // 选择回调通过 arg 拿到的是这个成员的地址，所以它必须活得和 ctx_ 一样久。
  std::string alpn_select_wire_;
};

}  // namespace uvcpp

#endif  // UVCPP_OPENSSL_ENABLE
#endif  // SRC_SSL_UVCPP_SSL_CONTEXT_H
