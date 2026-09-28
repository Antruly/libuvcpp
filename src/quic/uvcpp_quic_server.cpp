/**
 * @file src/quic/uvcpp_quic_server.cpp
 * @brief 1.4.1 的服务端壳：地址与配置能登记并校验，`listen()` 一律 `UV_ENOSYS`。
 * @author zhuweiye
 * @version 1.4.1
 */

#include "quic/uvcpp_quic_server.h"

#if UVCPP_QUIC_ENABLE

#include <uv.h>

namespace uvcpp {

namespace {

/// 地址字面量的最大长度：`INET6_ADDRSTRLEN` 是 46，IPv6 带 `%scope` 还可以更长。
/// 取 128 是余量，不是精度 —— 这里只是一块给 `uv_inet_pton` 落结果的暂存区。
const size_t kAddrLen = 128;

/**
 * @brief 把一个字面量按指定地址族校验一遍。
 * @return 字面量属于 \p family 返回 true。
 *
 * 只做校验，结果丢掉 —— 这一版没有 sockaddr 要填。写成函数而不是在三个
 * `bind*` 里各写一遍，是为了让"三个入口用的是同一把尺子"这件事一眼可查。
 */
bool literal_fits(int family, const char* ip) {
  if (ip == nullptr) return false;
  char buf[kAddrLen];
  return uv_inet_pton(family, ip, buf) == 0;
}

}  // namespace

struct uvcpp_quic_server::impl {
  uvcpp_loop* loop      = nullptr;
  bool        owns_loop = true;

  uvcpp_ssl_context* ssl_ctx = nullptr;
  /// 空 = 用 `quic_default_alpn()`。理由见 `uvcpp_quic_client.cpp` 里同名成员。
  std::vector<std::string> alpn_protos;

  /// 登记下来的地址。**不是**"已绑定的 socket"—— 见头文件 `bind()` 的
  /// `@warning`，以及这两个访问器为什么叫 `configured_*`。
  std::string ip;
  int         port = 0;
};

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_quic_server::uvcpp_quic_server() : impl_(new impl()) {
  impl_->loop      = new uvcpp_loop();
  impl_->owns_loop = true;
}

uvcpp_quic_server::uvcpp_quic_server(uvcpp_loop* external_loop)
    : impl_(new impl()) {
  impl_->owns_loop = false;
  impl_->loop      = external_loop;
}

uvcpp_quic_server::~uvcpp_quic_server() {
  // 与 `~uvcpp_quic_client` 逐字同理（包括"为什么这里现在一次都不进循环体"）——
  // 见那边的注释，别在两处各维护一份理由。
  if (impl_->loop != nullptr && impl_->owns_loop) {
    for (int i = 0; i < 256 && impl_->loop->loop_alive() != 0; ++i) {
      impl_->loop->run(UV_RUN_NOWAIT);
    }
    impl_->loop->loop_close();
    delete impl_->loop;
    impl_->loop = nullptr;
  }
}

// =========================================================================
// 访问器
// =========================================================================

uvcpp_loop* uvcpp_quic_server::get_loop() { return impl_->loop; }

std::string uvcpp_quic_server::configured_ip() const { return impl_->ip; }

int uvcpp_quic_server::configured_port() const { return impl_->port; }

// =========================================================================
// Bind —— 真实现：校验 + 登记
// =========================================================================

int uvcpp_quic_server::bind(const char* ip, int port) {
  // **先全部校验、再一次性赋值**：中间任何一步失败都不许留下半套状态。
  // 一半"新地址 + 旧端口"是最难查的那种：调用方拿到负的返回码，以为没生效，
  // 而 `configured_*()` 报出来的东西已经变了。
  if (port < 0 || port > 65535) return UV_EINVAL;

  std::string addr = (ip != nullptr) ? std::string(ip) : std::string("0.0.0.0");

  // 自动判断族：先当 IPv4 试，不行再当 IPv6 试。**两边都不行才是错** ——
  // 不能只用"含不含冒号"来分，那会把 `"::ffff:1.2.3.4"` 这类写法和
  // `"1.2.3.4"` 之外的所有畸形输入混进同一个分支。
  if (!literal_fits(AF_INET, addr.c_str()) &&
      !literal_fits(AF_INET6, addr.c_str())) {
    return UV_EINVAL;
  }

  impl_->ip   = addr;
  impl_->port = port;
  return 0;
}

int uvcpp_quic_server::bindIpv4(const char* ip, int port) {
  if (port < 0 || port > 65535) return UV_EINVAL;
  const char* addr = (ip != nullptr) ? ip : "0.0.0.0";
  if (!literal_fits(AF_INET, addr)) return UV_EINVAL;
  impl_->ip   = addr;
  impl_->port = port;
  return 0;
}

int uvcpp_quic_server::bindIpv6(const char* ip, int port) {
  if (port < 0 || port > 65535) return UV_EINVAL;
  const char* addr = (ip != nullptr) ? ip : "::";
  if (!literal_fits(AF_INET6, addr)) return UV_EINVAL;
  impl_->ip   = addr;
  impl_->port = port;
  return 0;
}

// =========================================================================
// 配置 —— 真实现（只存值）
// =========================================================================

void uvcpp_quic_server::set_ssl_context(uvcpp_ssl_context* ctx) {
  impl_->ssl_ctx = ctx;
}

void uvcpp_quic_server::set_alpn_select_protos(
    const std::vector<std::string>& protos) {
  impl_->alpn_protos = protos;
}

// =========================================================================
// Listen / Run
// =========================================================================

int uvcpp_quic_server::listen(
    std::function<void(uvcpp_quic_connection*)> connection_cb) {
  (void)connection_cb;
  // `connection_cb` 一次都不会被调 —— 理由与 `uvcpp_quic_client::connect()`
  // 里那段逐字相同（"受理了但永远不回调"是最难查的一类挂死）。见那边。
  return UV_ENOSYS;
}

int uvcpp_quic_server::run(uv_run_mode md) {
  // 这一条是**真实现**：`run` 就是"拨循环"，而循环是真的。
  // 本版循环上什么都没有，所以 `UV_RUN_DEFAULT` 会立刻返回 0（`uv_run`
  // 在"没有活跃句柄"时第一轮就退出），这是正确行为，不是吞掉。
  if (impl_->loop == nullptr) return UV_EINVAL;
  return impl_->loop->run(md);
}

void uvcpp_quic_server::stop() {
  if (impl_->loop != nullptr) impl_->loop->stop();
}

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
