#include <iostream>
#include <cstring>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_poll.h"
#include "req/uvcpp_connect.h"
#include "req/uvcpp_write.h"
#include "uvcpp/uvcpp_buf.h"

using namespace uvcpp;

#include <atomic>
#include <thread>
#include <future>
#include <string>
#include <memory>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cassert>
int main() {
  std::cout << "[functional poll] start\n";

  // Two-thread TCP poll test:
  // - server thread: listens, accepts, polls accepted socket for readability and echoes data
  // - client thread: connects, sends a message, waits for echo
  std::atomic<bool> success(false);
  std::promise<int> port_promise;
  auto port_future = port_promise.get_future();

  // Server thread
  std::thread server_thread([&]() {
    uvcpp_loop server_loop;
    server_loop.init();

    uvcpp_tcp server(&server_loop);
    server.bindIpv4("127.0.0.1", 0);
    sockaddr_in name;
    int namelen = sizeof(name);
    server.getsockname((sockaddr*)&name, &namelen);
    int port = ntohs(name.sin_port);
    if (port == 0) {
      server.bindIpv4("127.0.0.1", 8003);
      server.getsockname((sockaddr*)&name, &namelen);
      port = ntohs(name.sin_port);
    }

    // publish port so client can connect
    port_promise.set_value(port);

    server.listen([&](uvcpp_stream* s, int status) {
      if (status < 0) return;
      uvcpp_tcp* peer = new uvcpp_tcp(&server_loop);
      s->accept(peer);

      // obtain native socket for poll
      uv_os_sock_t sock;
      peer->fileno(sock);

      uvcpp_poll* poller = new uvcpp_poll(&server_loop, sock);
      poller->start(UV_READABLE, [peer, poller, &server_loop, &success](uvcpp_poll* p, int status, int events) {
        std::cout << "[functional poll] server poll callback events=" << events << " status=" << status << std::endl;
        // start libuv read on the peer to consume data and echo it back
        peer->read_start(
          [](uvcpp_handle* h, size_t suggested_size, uv_buf_t* buf) {
            uvcpp_buf::alloc_buf(buf, suggested_size);
          },
          [peer, poller, p, &server_loop, &success](uvcpp_stream* stream, ssize_t nread, const uv_buf_t* buf) {
            if (nread > 0) {
              std::string s(buf->base, (size_t)nread);
              std::cout << "[functional poll] server read: " << s << std::endl;
              // echo back
              uvcpp_buf bufcpp(buf->base, nread);
              uv_buf_t* b = bufcpp.out_uv_buf();
              uvcpp_write* w = new uvcpp_write();
              w->set_uv_buf(b, true);
              stream->write(w, b, 1, [w](uvcpp_write* req, int stat){
                delete req;
              });
            } else {
              std::cout << "[functional poll] server read nread=" << nread << std::endl;
            }
            uvcpp_free_bytes(buf->base);
            // on EOF or error, cleanup
            if (nread <= 0) {
              p->stop();
              p->close();
              stream->close([&server_loop, peer](uvcpp_handle*){
                server_loop.stop();
                delete peer;
              });
            }
          }
        );
      });
    }, 128);

    server_loop.run(UV_RUN_DEFAULT);
  });

  // Client thread
  std::thread client_thread([&]() {
    int port = port_future.get();
    uvcpp_loop client_loop;
    client_loop.init();
    uvcpp_tcp client(&client_loop);

    uvcpp_connect* conn = new uvcpp_connect();
    sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", port, &addr);
    client.connect(conn, (const sockaddr*)&addr, [conn, &client, &client_loop, &success](uvcpp_connect* r, int status) {
      if (status == 0) {
        // start read to receive echo
        client.read_start(
          [](uvcpp_handle* h, size_t suggested_size, uv_buf_t* buf) {
            uvcpp_buf::alloc_buf(buf, suggested_size);
          },
          [&client_loop, &success](uvcpp_stream* stream, ssize_t nread, const uv_buf_t* buf) {
            if (nread > 0) {
              std::string s(buf->base, (size_t)nread);
              std::cout << "[functional poll] client recv: " << s << std::endl;
              success.store(true);
              stream->close([&client_loop](uvcpp_handle*){
                client_loop.stop();
              });
            }
            uvcpp_free_bytes(buf->base);
          }
        );

        // send message
        const char* msg = "poll_ping";
        uvcpp_buf bufcpp(msg);
        uv_buf_t* b = bufcpp.out_uv_buf();
        uvcpp_write* w = new uvcpp_write();
        w->set_uv_buf(b, true);
        client.write(w, b, 1, [w](uvcpp_write* req, int stat){
          delete req;
        });
      } else {
        std::cout << "[functional poll] client connect failed status=" << status << std::endl;
        client_loop.stop();
      }
      delete conn;
    });

    client_loop.run(UV_RUN_DEFAULT);
  });

  client_thread.join();
  server_thread.join();

  std::cout << "[functional poll] done success=" << (success.load() ? "true" : "false") << std::endl;
  return success.load() ? 0 : 2;
}

