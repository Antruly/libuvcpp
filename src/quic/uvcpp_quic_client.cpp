/**
 * @file src/quic/uvcpp_quic_client.cpp
 * @brief 1.4.1 的客户端壳：配置能存，传输动作一律 `UV_ENOSYS`。
 * @author zhuweiye
 * @version 1.4.1
 */

#include "quic/uvcpp_quic_client.h"

#if UVCPP_QUIC_ENABLE

#include <uv.h>

namespace uvcpp {

struct uvcpp_quic_client::impl {
  uvcpp_loop* loop      = nullptr;
  bool        owns_loop = true;  ///< 自建那条路为真；共享外部循环时为假

  uvcpp_ssl_context* ssl_ctx = nullptr;
  /// 空 = 用 `quic_default_alpn()`。**不在这里预先塞进去** —— 那样
  /// `set_alpn_protos({})`（"一个都不发"）与"没设过"就分不开了，而这两件事在
  /// TLS 层的结果完全不同（前者握手失败，后者用默认值）。一个空 vector 就是
  /// "没设过"的判据。
  std::vector<std::string> alpn_protos;
};

// =========================================================================
// 构造 / 析构
// =========================================================================

uvcpp_quic_client::uvcpp_quic_client() : impl_(new impl()) {
  // 与 `uvcpp_tcp_client()` 同一形状：自建那条路走 `new uvcpp_loop()`，
  // 而 `uvcpp_loop` 的构造函数内部已经调过 `init()`。
  impl_->loop      = new uvcpp_loop();
  impl_->owns_loop = true;
}

uvcpp_quic_client::uvcpp_quic_client(uvcpp_loop* external_loop)
    : impl_(new impl()) {
  impl_->owns_loop = false;
  impl_->loop      = external_loop;
  // `external_loop == nullptr` 不在这里拦：与 `uvcpp_tcp_client(uvcpp_loop*)`
  // 一致 —— 那是个明确的调用方错误，会在 `get_loop()` 的返回值上立刻现形，
  // 而不是被这里悄悄换成一个自建循环（那会让"共享"这条路的语义反过来）。
}

uvcpp_quic_client::~uvcpp_quic_client() {
  // **这里不需要 `uvcpp_tcp_client` 析构里那套"泵到句柄收尾"的循环**：本对象
  // 从来没有在循环上登记过任何句柄（1.4.1 连 socket 都没建），所以队列恒空，
  // `loop_alive()` 恒 0，泵它等于空转。
  //
  // 写成"先按 `loop_alive()` 有界地泵一遍、再 `loop_close()`"，与
  // `uvcpp_tcp_server` 那套保持一致 —— 它现在一次都不进循环体，但等谁给这个类
  // 加第一个句柄时，不必再回来想一遍这件事。`loop_close()` 自己会在队列不干净时
  // 返回 `UV_EBUSY`，而 `~uvcpp_loop` 的既有策略是"泄漏而不释放"（见那边注释），
  // 于是漏掉一次泵的后果是被记下来的一条泄漏，不是一个野指针。
  if (impl_->loop != nullptr && impl_->owns_loop) {
    for (int i = 0; i < 256 && impl_->loop->loop_alive() != 0; ++i) {
      impl_->loop->run(UV_RUN_NOWAIT);
    }
    impl_->loop->loop_close();
    delete impl_->loop;
    impl_->loop = nullptr;
  }
  // 共享那条路上 `delete` 的是**调用方的**循环 —— 绝不能碰。`owns_loop`
  // 就是这条界线，且它只在上面这一处被读。
}

// =========================================================================
// 访问器
// =========================================================================

uvcpp_loop* uvcpp_quic_client::get_loop() { return impl_->loop; }

// =========================================================================
// 配置 —— 真实现（只存值）
// =========================================================================

void uvcpp_quic_client::set_ssl_context(uvcpp_ssl_context* ctx) {
  impl_->ssl_ctx = ctx;
}

void uvcpp_quic_client::set_alpn_protos(const std::vector<std::string>& protos) {
  impl_->alpn_protos = protos;
}

// =========================================================================
// 传输 —— 1.4.1 全部 UV_ENOSYS
// =========================================================================

int uvcpp_quic_client::connect(const char* host, int port,
                               std::function<void(int)> cb) {
  (void)host;
  (void)port;
  (void)cb;
  // **`cb` 一次都不会被调**，而这是成文契约（测试钉了它）。理由不是"懒得写"：
  // 一个"受理了但永远不回调"的接口会让调用方把整条握手超时路径压在这条依赖上，
  // 而它永远不来 —— 那是本仓库里最难查的一类挂死（同 `uvcpp_tcp_server` 里
  // "必须自己起读才会发现断开"那条注释记的教训）。返回 `UV_ENOSYS` 把话
  // 说完整：调用方看到它就知道"这条路没通"，而不是在原地等。
  return UV_ENOSYS;
}

uvcpp_quic_connection* uvcpp_quic_client::connection() {
  // 1.4.1 连不上任何对端，所以本对象名下**没有**连接对象，返回 nullptr。
  // 不预先 new 一个空壳放在这里：那会让 `if (c->connection())` 这种写法
  // 恒真，调用方据此认为自己"连上了"。
  return nullptr;
}

int uvcpp_quic_client::close() { return UV_ENOSYS; }

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE
