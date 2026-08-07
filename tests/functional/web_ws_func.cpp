#include <iostream>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_ws_server.h>
#include <web/uvcpp_ws_client.h>
using namespace uvcpp;

static int get_port(uvcpp_ws_server& s) {
  struct sockaddr_in n; int l=sizeof(n);
  s.get_http_server()->get_tcp_server()->get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&n),&l);
  return ntohs(n.sin_port);
}

int main() {
  std::cout << "[web_ws] echo" << std::endl;
  // NOTE: uses raw new/leak to avoid dual-loop destructor issues
  auto* server = new uvcpp_ws_server();
  server->bind("127.0.0.1", 0);
  int port = get_port(*server);
  std::atomic<bool> ok{false};

  server->on_connection([&](uvcpp_ws_connection* c) {
    c->on_text([c](const std::string& m) { c->send_text(m.c_str(), m.size()); });
  });
  server->listen();

  auto* client = new uvcpp_ws_client();
  client->connect("ws://127.0.0.1:" + std::to_string(port) + "/chat",
    [&](uvcpp_ws_connection* c, int) {
      c->on_text([&](const std::string& m) { if (m == "hello") ok.store(true); });
      c->send_text("hello", 5);
    });

  auto t = std::chrono::steady_clock::now();
  while (!ok.load()) {
    server->run(UV_RUN_NOWAIT);
    client->run(UV_RUN_NOWAIT);
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t).count() >= 5000)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  bool pass = ok.load();
  std::cout << "  -> " << (pass ? "PASS" : "FAIL") << std::endl;
  // Skip cleanup (known dual-loop issue), exit fast
  ::_exit(pass ? 0 : 2);
}
#else
int main() { return 0; }
#endif
