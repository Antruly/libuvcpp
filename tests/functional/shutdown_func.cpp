#include <iostream>
#include <thread>
#include <future>
#include <atomic>
#include <cstring>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_timer.h"
#include "req/uvcpp_connect.h"
#include "req/uvcpp_write.h"
#include "req/uvcpp_shutdown.h"
#include "uvcpp/uvcpp_buf.h"

using namespace uvcpp;

int main() {
  std::cout << "[functional shutdown] start\n";

  std::atomic<bool> eof_received(false);
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
    port_promise.set_value(port);

    server.listen([&](uvcpp_stream* s, int status) {
      if (status < 0) return;
      uvcpp_tcp* peer = new uvcpp_tcp(&server_loop);
      s->accept(peer);

      peer->read_start(
        [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
          uvcpp_buf::alloc_buf(buf, sz);
        },
        [&, peer](uvcpp_stream* stream, ssize_t nread, const uv_buf_t* buf) {
          if (nread == UV_EOF) {
            eof_received.store(true);
            std::cout << "[functional shutdown] server received EOF\n";
            uvcpp_free_bytes(buf->base);
            stream->close([&, peer](uvcpp_handle*) {
              server_loop.stop();
              delete peer;
            });
          } else if (nread > 0) {
            std::cout << "[functional shutdown] server recv: "
                      << std::string(buf->base, (size_t)nread) << std::endl;
            uvcpp_free_bytes(buf->base);
            // Don't close yet — wait for EOF from client shutdown
          } else {
            uvcpp_free_bytes(buf->base);
          }
        }
      );
    }, 128);

    uvcpp_timer watchdog(&server_loop);
    watchdog.start([&server_loop](uvcpp_timer* t) {
      std::cout << "[functional shutdown] server watchdog\n";
      server_loop.stop();
    }, 5000, 0);

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

    client.connect(conn, (const sockaddr*)&addr, [&](uvcpp_connect* r, int status) {
      if (status != 0) {
        std::cout << "[functional shutdown] connect failed: "
                  << uv_strerror(status) << std::endl;
        client_loop.stop();
        delete r;
        return;
      }
      delete r;

      // Write data, then shutdown after a brief delay to let server process it
      const char* msg = "hello";
      uvcpp_buf bufcpp(msg);
      uv_buf_t* b = bufcpp.out_uv_buf();
      uvcpp_write* w = new uvcpp_write();
      w->set_uv_buf(b, true);
      client.write(w, b, 1, [&client, &client_loop](uvcpp_write* wr, int ws) {
        delete wr;

        // Delay shutdown briefly to ensure server has processed the write
        uvcpp_timer* delay = new uvcpp_timer(&client_loop);
        delay->start([delay, &client, &client_loop](uvcpp_timer* t) {
          delete delay;

          uvcpp_shutdown* sd = new uvcpp_shutdown();
          sd->init();
          sd->shutdown(&client,
            [sd, &client, &client_loop](uvcpp_shutdown* s, int ss) {
              std::cout << "[functional shutdown] shutdown complete status="
                        << ss << std::endl;
              delete s;
              client.close([&client_loop](uvcpp_handle*) {
                client_loop.stop();
              });
            });
        }, 100, 0);
      });
    });

    uvcpp_timer watchdog(&client_loop);
    watchdog.start([&client_loop](uvcpp_timer* t) {
      std::cout << "[functional shutdown] client watchdog\n";
      client_loop.stop();
    }, 5000, 0);

    client_loop.run(UV_RUN_DEFAULT);
  });

  client_thread.join();
  server_thread.join();

  std::cout << "[functional shutdown] done success="
            << (eof_received.load() ? "true" : "false") << std::endl;
  return eof_received.load() ? 0 : 2;
}
