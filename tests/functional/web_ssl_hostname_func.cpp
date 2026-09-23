/**
 * @file tests/functional/web_ssl_hostname_func.cpp
 * @brief `PEER_STRICT` 的主机名校验（真握手、真证书、两条连接顺序都覆盖）。
 *
 * 这个文件守的是 `tls_verify_mode::PEER_STRICT` 里**多出来的那一半**。
 * 它曾经与 `PEER` 完全等价（两个值映射到同一个 `SSL_VERIFY_PEER` 位），
 * 头注释自己也如实写着"主机名这一半从未实现"。于是 `PEER_STRICT` **挡不住**
 * 「证书链可信、但签发给别的域名」的对端 —— 而那正是中间人最省事的一种做法：
 * 找一张任何可信 CA 签给**它自己域名**的证书，照样握手成功。
 *
 * 判据为什么必须是"握手成不成"，而不是"校验结果对不对"：
 * 主机名对不上时 OpenSSL 把失败报在 `SSL_get_verify_result()` 上，而**报不报
 * 得出来是一回事、中止不中止握手是另一回事** —— `SSL_VERIFY_NONE` 下它报错
 * 但照样把连接建起来。所以只有"连接真的没建起来"才算数。
 *
 * ---------------------------------------------------------------------------
 * 对照表：**每一条"期望失败"都配一条"只差档位"的正向**
 * ---------------------------------------------------------------------------
 * 光看一条失败说明不了问题 —— CA 没装上、名字没解析出来、服务端证书没加载，
 * 都会让握手失败，而红的样子一模一样。所以下面每一行的红都有一条**除档位外
 * 完全相同**的绿垫在下面（同一台服务端、同一张证书、同一个连接名）：
 *
 * | # | 服务端 CN   | 连接名      | 客户端档位     | 装配顺序        | 期望 |
 * |---|-------------|-------------|----------------|-----------------|------|
 * | 1 | localhost   | localhost   | STRICT + CA    | enable→connect  | 成功 |
 * | 2 | localhost   | 127.0.0.1   | STRICT + CA    | enable→connect  | 失败 |
 * | 3 | localhost   | 127.0.0.1   | **PEER** + CA  | enable→connect  | 成功 |  ← 2 的对照
 * | 4 | wrong.test  | localhost   | STRICT + CA    | enable→connect  | 失败 |
 * | 5 | wrong.test  | localhost   | **PEER** + CA  | enable→connect  | 成功 |  ← 4 的对照
 * | 6 | wrong.test  | localhost   | **NONE**       | enable→connect  | 成功 |  ← 既有行为不变
 * | 7 | localhost   | 127.0.0.1   | STRICT + CA    | **connect→enable** | 失败 |
 * | 8 | localhost   | 127.0.0.1   | **PEER** + CA  | **connect→enable** | 成功 |  ← 7 的对照
 *
 * 第 7/8 行不是凑数：主机名是在 `connect()` / `enable_tls()` **两处**补装的，
 * 谁先谁后都合法。只测 `enable_tls → connect` 的话，`enable_tls()` 里那句补装
 * 拿掉也照样全绿。
 *
 * 第 2 行是**反向**的 IP 用例：`generate_self_signed()` 只产 CN、不产任何扩展
 * （没有 `subjectAltName`），所以数字 IP 一定对不上 —— 正向的 IP 用例（证书带
 * `iPAddress` SAN、连接名写 IP、期望成功）需要一张本库造不出来的证书，**未覆盖**，
 * 不为它去扩 `generate_self_signed` 的公开签名。
 */
#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

/// 端口在服务端线程里定、主线程里读 —— 一次性的一格。
std::atomic<int>& port_slot() {
  static std::atomic<int> slot{0};
  return slot;
}

/// 一次连接尝试的结果。
struct probe {
  std::atomic<int>  status{-99};     ///< connect 回调的状态码
  std::atomic<bool> fired{false};    ///< 回调到没到
  std::atomic<bool> hs_done{false};  ///< 回调那一刻握手完成了吗
};

/**
 * @brief 连一次，返回 connect 回调的状态码。
 *
 * @param tls_first `true` 先 `enable_tls()` 再 `connect()`（`uvcpp_http_client`
 *        异步路径的顺序）；`false` 反过来，走延迟装配那条。
 * @return 回调里的状态码；**回调没来返回 -99**（那是判据不成立，不是"失败"——
 *         调用方要分开记）。
 */
int try_connect(int port, const char* host, uvcpp_ssl_context* cctx,
                bool tls_first, probe& p) {
  uvcpp_tcp_client client;

  auto on_cb = [&p, &client](int status) {
    p.status.store(status);
    p.hs_done.store(client.is_tls_handshake_done());
    p.fired.store(true);
  };

  int rc = 0;
  if (tls_first) {
    rc = client.enable_tls(cctx);
    if (rc != 0) return rc;
    rc = client.connect(host, port, on_cb);
  } else {
    rc = client.connect(host, port, on_cb);
    if (rc != 0) return rc;
    rc = client.enable_tls(cctx);
  }
  if (rc != 0) return rc;  // 连接都没发起，回调不会来

  uvcpp_loop* loop = client.get_loop();
  uvcpp_test::wait_until(loop, [&p] { return p.fired.load(); },
                         uvcpp_test::kWaitMs);
  client.close();
  uvcpp_test::pump_for(loop, 50);

  return p.fired.load() ? p.status.load() : -99;
}

/**
 * @brief 一条判据：连一次，比对状态码，并断言"回调真的来过"。
 *
 * 回调没来（-99）单独记一条 —— 那是用例自己没跑起来，与"对端拒绝了我们"
 * 是两件事，混在一起会把装置故障读成被测行为。
 */
void expect(int port, const char* host, uvcpp_ssl_context* cctx, bool tls_first,
            bool want_ok, const std::string& label) {
  probe p;
  const int st = try_connect(port, host, cctx, tls_first, p);

  check(p.fired.load(), label + "：connect 回调始终没来（装置问题，不是被测行为）");
  if (!p.fired.load()) return;

  if (want_ok) {
    check(st == 0, label + "：期望握手成功，实际 status=" + std::to_string(st));
    // 反面也钉一下：成功时**必须**是握手真的走完了，而不是"连上就算"。
    check(p.hs_done.load(), label + "：回调到了但握手没完成");
  } else {
    check(st != 0, label + "：期望握手**失败**，实际 status=0（名字对不上却连上了）");
    check(!p.hs_done.load(), label + "：报了错却把握手标成已完成");
  }
}

/// 把上下文里那张自签证书导成 PEM，供客户端 `load_ca_file()` 当信任锚。
bool write_cert_pem(const char* path, uvcpp_ssl_context& ctx) {
  X509* leaf = SSL_CTX_get0_certificate(ctx.raw_ctx());
  if (leaf == nullptr) return false;
  BIO* out = BIO_new_file(path, "w");
  if (out == nullptr) return false;
  const bool ok = PEM_write_bio_X509(out, leaf) == 1;
  BIO_free(out);
  return ok;
}

/// 把客户端上下文收在一个地方，活得比所有场景久（`uvcpp_ssl_context` 不可拷贝）。
std::vector<std::unique_ptr<uvcpp_ssl_context>>& ctx_pool() {
  static std::vector<std::unique_ptr<uvcpp_ssl_context>> pool;
  return pool;
}

/**
 * @brief 造一个客户端上下文。
 * @param ca_path 非空时装它当信任锚；`NONE` 那条场景传 nullptr —— 那一条要证的
 *        正是"什么都不设也照旧能连"。
 */
uvcpp_ssl_context* make_client_ctx(tls_verify_mode mode, const char* ca_path,
                                   const std::string& label) {
  ctx_pool().push_back(std::unique_ptr<uvcpp_ssl_context>(
      new uvcpp_ssl_context(tls_mode::CLIENT, tls_version::TLS_1_2)));
  uvcpp_ssl_context* c = ctx_pool().back().get();

  check(c->is_ready(), label + "：客户端 context 没建起来");
  c->set_verify_mode(mode);
  // 前置断言：下面那些判据全靠"档位真的设进去了"，设丢了就全无意义。
  check(c->verify_mode() == mode, label + "：verify_mode 设了没记住");

  if (ca_path != nullptr) {
    // 前置断言，而且是**这条用例的软肋**：`load_ca_file` 失败的话，正向场景
    // 也会红 —— 但那是"CA 没装上"，不是"主机名校验错了"。分成两条记。
    check(c->load_ca_file(ca_path),
          label + "：load_ca_file 失败（装置问题，不是被测行为）");
  }
  return c;
}

/// 服务端一侧的计数（判"到底有没有人连上来"）。
struct server_state {
  std::atomic<int> accepted{0};
  std::atomic<int> tls_ok{0};
};

/**
 * @brief 起一台 TLS 服务端，跑 \p body，然后收掉。
 *
 * 服务端的证书用 `generate_self_signed(cn)` 现造 —— 只有 CN、没有任何扩展。
 * 这一点对上面的表很关键：没有 `subjectAltName` 时 OpenSSL 才回落到 CN 比对
 * （有 dNSName SAN 的话 CN 直接被忽略）。
 */
void with_server(const char* cn, const char* cert_path,
                 const std::function<void(int port, const char* cert_path)>& body) {
  uvcpp_ssl_context sctx(tls_mode::SERVER, tls_version::TLS_1_2);
  check(sctx.is_ready(), std::string("服务端 context 没建起来（CN=") + cn + "）");
  check(sctx.generate_self_signed(cn, 2048),
        std::string("generate_self_signed 失败（CN=") + cn + "）");
  check(write_cert_pem(cert_path, sctx),
        std::string("导出自签证书 PEM 失败（CN=") + cn + "）");

  server_state      st;
  std::atomic<bool> stop{false};
  std::thread th([&] {
    uvcpp_tcp_server server;
    server.set_read_callback(
        [](uvcpp_tcp_client&, const net_read_result&) { /* 握手用的字节走 TLS 层 */ });
    if (server.bindIpv4("127.0.0.1", 0) != 0) return;
    sockaddr_in name;
    int         namelen = sizeof(name);
    server.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
    const int port = ntohs(name.sin_port);
    if (server.listen(
            [&st, &sctx](uvcpp_tcp_client* client) {
              st.accepted.fetch_add(1);
              if (client->enable_tls(&sctx) == 0) st.tls_ok.fetch_add(1);
            },
            128) != 0) {
      return;
    }
    // 端口写进 body 能看见的地方 —— 用 promise 反而要处理"没 listen 成功"那条路。
    port_slot().store(port);
    uvcpp_loop* loop = server.get_loop();
    while (!stop.load()) {
      loop->run(UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    uvcpp_test::pump_for(loop, 200);
  });

  // `listen()` 成功之后端口才放行（"已 bind、未 listen"的 socket 是**拒连**的，
  // 不是排队等 listen —— `web_ssl_client_func.cpp` 踩过一次，那里有详细记录）。
  const int port = uvcpp_test::wait_flag(
      [] { return port_slot().load() != 0; }, uvcpp_test::kWaitMs)
                       ? port_slot().load()
                       : 0;
  check(port > 0, std::string("服务端没起来（CN=") + cn + "）");

  if (port > 0) body(port, cert_path);

  stop.store(true);
  th.join();
  std::remove(cert_path);
}

}  // namespace

int main() {
  std::cout << "[web_ssl_hostname] OpenSSL "
            << OpenSSL_version(OPENSSL_VERSION_STRING) << std::endl;

  const char* kCertA = "uvcpp_ssl_hostname_a.pem";  // CN = localhost
  const char* kCertB = "uvcpp_ssl_hostname_b.pem";  // CN = wrong.test

  // ---- 服务端 A：名字就是 localhost ----------------------------------
  port_slot().store(0);
  with_server("localhost", kCertA, [](int port, const char* cert) {
    expect(port, "localhost", make_client_ctx(tls_verify_mode::PEER_STRICT, cert,
                                              "1 STRICT+同名"),
           true, true, "1 STRICT + 名字相同（localhost）必须连得上");

    expect(port, "127.0.0.1",
           make_client_ctx(tls_verify_mode::PEER_STRICT, cert, "2 STRICT+IP"),
           true, false, "2 STRICT + 数字 IP 必须连不上（证书没有 iPAddress SAN）");

    expect(port, "127.0.0.1",
           make_client_ctx(tls_verify_mode::PEER, cert, "3 PEER+IP(对照)"),
           true, true, "3 PEER + 同一个数字 IP 必须连得上（2 的对照）");

    expect(port, "127.0.0.1",
           make_client_ctx(tls_verify_mode::PEER_STRICT, cert,
                           "7 STRICT+IP(延迟装配)"),
           false, false,
           "7 connect→enable 顺序下 STRICT + 数字 IP 也必须连不上");

    expect(port, "127.0.0.1",
           make_client_ctx(tls_verify_mode::PEER, cert, "8 PEER+IP(延迟装配对照)"),
           false, true,
           "8 connect→enable 顺序下 PEER + 同一个数字 IP 必须连得上（7 的对照）");
  });

  // ---- 服务端 B：证书签给了别的名字 ----------------------------------
  port_slot().store(0);
  with_server("wrong.test", kCertB, [](int port, const char* cert) {
    expect(port, "localhost",
           make_client_ctx(tls_verify_mode::PEER_STRICT, cert, "4 STRICT+异名"),
           true, false,
           "4 STRICT + 链可信但名字不符必须连不上（这正是 PEER 挡不住的那一类）");

    expect(port, "localhost",
           make_client_ctx(tls_verify_mode::PEER, cert, "5 PEER+异名(对照)"),
           true, true,
           "5 PEER + 同一张证书、同一个名字必须连得上（4 的对照：红的是主机名，"
           "不是 CA / 解析 / 证书加载）");

    expect(port, "localhost",
           make_client_ctx(tls_verify_mode::NONE, nullptr, "6 NONE"),
           true, true, "6 NONE + 什么都不设必须照旧连得上（既有行为不变）");
  });

  std::cout << "[web_ssl_hostname] done failures=" << g_failures << std::endl;
  return g_failures == 0 ? 0 : 1;
}

#else

#include <iostream>
int main() {
  std::cout << "[skip] web_ssl_hostname —— 需要 WEB + OpenSSL" << std::endl;
  return 0;
}

#endif
