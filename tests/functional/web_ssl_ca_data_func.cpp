/**
 * @file tests/functional/web_ssl_ca_data_func.cpp
 * @brief `uvcpp_ssl_context::load_ca_data()` —— 从 **PEM 字符串**装信任 CA（#61）。
 *
 * 判据是**真握手**，不是"函数返回 true"：一张证书装进信任库这件事，只有
 * 「同一个客户端、同一台服务端，装之前握手失败、装之后拿到 200」这一对能说明。
 * 所以每个正向场景都配一条"只差 CA 来源"的负向对照：
 *
 * | # | 客户端信任来源 | 期望 |
 * |---|---|---|
 * | 1 | `load_ca_data(服务端证书 PEM)` | 握手成功、`GET /ping` → 200 `pong` |
 * | 2 | 同上，**装两次**（幂等） | 仍然成功 |
 * | 3 | `load_ca_file(同一份 PEM 写的临时文件)` | 成功（与内存入口对称的对照） |
 * | 4 | **不装**（`PEER` 但没有信任锚） | 握手失败 |
 * | 5 | `load_ca_data("not a pem")` | 返回 false，且不建立任何信任 |
 *
 * 第 2 条钉的是**重复项幂等**：`X509_STORE_add_cert` 对已在哈希表里的证书返回
 * 0 并压一个 `X509_R_CERT_ALREADY_IN_HASH_TABLE`，把它当成错误的话，"同一张
 * CA 装两次"（多张证书的 PEM 重叠、或调用方重复配置）就会变成启动失败。
 *
 * 第 3 条是**对称性对照**：两条入口写的是同一个信任库，行为必须一致；只测
 * 内存入口的话，"它其实什么都没装、握手碰巧靠别的原因过了"分辨不出来 ——
 * 所以第 4 条必须有。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_OPENSSL_ENABLE

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "wait_util.h"

using namespace uvcpp;

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "ok: " << what << std::endl;
  } else {
    ++g_fail;
    std::cerr << "FAIL: " << what << std::endl;
  }
}

/// 把上下文里那张自签证书导出成 PEM 字符串（客户端拿它当信任锚）。
std::string certificate_pem(uvcpp_ssl_context& ctx) {
  std::string out;
  X509* leaf = SSL_CTX_get0_certificate(ctx.raw_ctx());
  if (leaf == nullptr) return out;
  BIO* bio = BIO_new(BIO_s_mem());
  if (bio == nullptr) return out;
  if (PEM_write_bio_X509(bio, leaf) == 1) {
    char*  data = nullptr;
    long   len  = BIO_get_mem_data(bio, &data);
    if (data != nullptr && len > 0) out.assign(data, static_cast<size_t>(len));
  }
  BIO_free(bio);
  return out;
}

struct server_state {
  std::atomic<int>  listen_rc{-1};
  std::atomic<bool> stop{false};
  std::atomic<int>  ping_hits{0};
};

/// 服务端跑在主线程的循环上（客户端在另一条线程里阻塞式发请求）。
void run_server(uvcpp_http_server& server, uvcpp_ssl_context& sctx,
                server_state& st) {
  server.get_tcp_server()->set_ssl_context(&sctx);  // 必须在 listen 之前
  server.get("/ping", [&st](uvcpp_http_request&, uvcpp_http_response& resp,
                            uvcpp_tcp_client*) {
    st.ping_hits.fetch_add(1);
    resp = uvcpp_http_response::ok("pong", 4);
  });
}

int bound_port(uvcpp_http_server& server) {
  struct sockaddr_in name;
  std::memset(&name, 0, sizeof(name));
  int len = static_cast<int>(sizeof(name));
  if (server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<struct sockaddr*>(&name), &len) != 0) {
    return -1;
  }
  return ntohs(name.sin_port);
}

/// 一次同步 HTTPS 请求。@return true = 业务成功（200 + `pong`）。
bool https_ping(int port, uvcpp_ssl_context* client_ctx, int* status_out) {
  uvcpp_http_client client;
  if (client_ctx != nullptr) client.set_ssl_context(client_ctx);

  const int crc = client.connect_wait("127.0.0.1", port, 5000);
  if (crc != 0) return false;

  uvcpp_http_request req = uvcpp_http_request::make_get("/ping");
  uvcpp_http_response resp;
  const int src = client.send_wait(req, resp, 5000);
  if (status_out != nullptr) *status_out = static_cast<int>(resp.status_code);
  return src == 0 && resp.status_code == http_status::OK && resp.body.size() == 4 &&
         std::memcmp(resp.body.get_const_data(), "pong", 4) == 0;
}

}  // namespace

int main() {
  std::cout << "uvcpp_ssl_context::load_ca_data (#61)" << std::endl;

  uvcpp_ssl_context server_ctx(tls_mode::SERVER);
  if (!server_ctx.generate_self_signed("localhost", 2048)) {
    check(false, "服务端自签证书");
    return 1;
  }
  const std::string pem = certificate_pem(server_ctx);
  check(!pem.empty(), "导出服务端证书 PEM（" + std::to_string(pem.size()) + " 字节）");

  // ---- 负向：不是 PEM -------------------------------------------------
  {
    uvcpp_ssl_context c(tls_mode::CLIENT);
    check(!c.load_ca_data("not a pem at all"),
          "load_ca_data(非 PEM) 返回 false");
    check(!c.load_ca_data(std::string()), "load_ca_data(空串) 返回 false");
  }

  // ---- 服务端 ---------------------------------------------------------
  uvcpp_http_server server;
  server_state     st;
  run_server(server, server_ctx, st);

  const int brc = server.bind("127.0.0.1", 0);
  check(brc == 0, "server bind（rc=" + std::to_string(brc) + "）");
  const int lrc = server.listen(128);
  check(lrc == 0, "server listen（rc=" + std::to_string(lrc) + "）");
  const int port = bound_port(server);
  check(port > 0, "端口（" + std::to_string(port) + "）");
  if (brc != 0 || lrc != 0 || port <= 0) return 1;

  std::atomic<bool> client_done{false};
  std::thread client([&] {
    // 1) 内存入口，正向
    {
      uvcpp_ssl_context c(tls_mode::CLIENT);
      check(c.load_ca_data(pem), "load_ca_data(服务端证书 PEM) 返回 true");
      c.set_verify_mode(tls_verify_mode::PEER);
      int status = 0;
      check(https_ping(port, &c, &status),
            "内存 CA：GET /ping → 200 pong（实际 status=" +
                std::to_string(status) + "）");
    }

    // 2) 同一张证书装两次：幂等
    {
      uvcpp_ssl_context c(tls_mode::CLIENT);
      check(c.load_ca_data(pem), "第一次 load_ca_data");
      check(c.load_ca_data(pem), "第二次 load_ca_data（幂等，仍返回 true）");
      c.set_verify_mode(tls_verify_mode::PEER);
      int status = 0;
      check(https_ping(port, &c, &status), "重复装入后仍然握手成功");
    }

    // 3) 文件入口对照：同一份 PEM 写成临时文件
    {
      const char* path = "uvcpp_ca_data_test.pem";
      {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(pem.data(), static_cast<std::streamsize>(pem.size()));
      }
      uvcpp_ssl_context c(tls_mode::CLIENT);
      check(c.load_ca_file(path), "load_ca_file(同一份 PEM) 返回 true");
      std::remove(path);
      c.set_verify_mode(tls_verify_mode::PEER);
      int status = 0;
      check(https_ping(port, &c, &status), "文件 CA：同样拿到 200 pong");
    }

    // 4) 负向对照：PEER 但没有信任锚 —— 握手必须失败
    {
      uvcpp_ssl_context c(tls_mode::CLIENT);
      c.set_verify_mode(tls_verify_mode::PEER);
      int status = -1;
      check(!https_ping(port, &c, &status),
            "没有 CA 时握手失败（PEER 生效）");
    }

    client_done.store(true);
  });

  // 泵服务端循环，直到客户端线程跑完。
  while (!client_done.load()) {
    server.run(UV_RUN_NOWAIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  client.join();

  server.stop();
  for (int i = 0; i < 64; ++i) server.run(UV_RUN_NOWAIT);

  check(st.ping_hits.load() >= 2,
        "服务端真的处理了正向请求（" + std::to_string(st.ping_hits.load()) +
            " 次）");

  if (g_fail != 0) {
    std::cerr << g_fail << " check(s) failed" << std::endl;
    return 1;
  }
  std::cout << "all checks passed" << std::endl;
  return 0;
}

#else  // !web || !openssl

int main() {
  std::cout << "SKIP: web / OpenSSL disabled" << std::endl;
  return 0;
}

#endif
