#include <iostream>
#include <cstring>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_poll.h"
#include "handle/uvcpp_timer.h"
#include "handle/uvcpp_async.h"
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

  std::atomic<bool> success(false);
  std::promise<int> port_promise;
  auto port_future = port_promise.get_future();

  // Server thread
  std::thread server_thread([&]() {
    uvcpp_loop loop;
    loop.init();

    uvcpp_tcp server(&loop);
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

    port_promise.set_value(port);

    server.listen([&](uvcpp_stream* s, int status) {
      if (status < 0) return;
      uvcpp_tcp* peer = new uvcpp_tcp(&loop);
      s->accept(peer);

      uv_os_sock_t sock;
      peer->fileno(sock);

      uvcpp_poll* poller = new uvcpp_poll(&loop, sock);

      // Lambda to restart poll
      auto restart_poll = [&]() {
        poller->start(UV_READABLE, [peer, poller, &loop](uvcpp_poll* p, int status, int events) {
          std::cout << "[functional poll] server poll callback events=" << events << std::endl;
          p->stop();  // Stop poll immediately to prevent repeated triggers

          peer->read_start(
            [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
              uvcpp_buf::alloc_buf(buf, sz);
            },
            [peer, poller, &loop](uvcpp_stream* stream, ssize_t nread, const uv_buf_t* buf) {
              if (nread > 0) {
                std::string s(buf->base, (size_t)nread);
                std::cout << "[functional poll] server read: " << s << std::endl;

                // Echo back - use unique_ptr for automatic cleanup
                std::unique_ptr<uvcpp_buf> bufcpp(new uvcpp_buf(buf->base, nread));
                uvcpp_write* w = new uvcpp_write();
                w->set_uv_buf(bufcpp->out_uv_buf(), true);
                stream->write(w, w->get_uv_buf(), 1, [w, bufcpp = bufcpp.release()](uvcpp_write*, int) {
                  delete bufcpp;
                  delete w;
                });
              }

              if (nread <= 0) {
                poller->stop();
                poller->close();
                stream->close([&loop, peer](uvcpp_handle*){
                  loop.stop();
                  delete peer;
                });
              } else {
                // Restart poll after read completes
                poller->start(UV_READABLE, [peer, poller, &loop](uvcpp_poll* p, int s, int e) {
                  std::cout << "[functional poll] server poll callback events=" << e << std::endl;
                  p->stop();

                  peer->read_start(
                    [](uvcpp_handle*, size_t sz, uv_buf_t* b) { uvcpp_buf::alloc_buf(b, sz); },
                    [peer, poller, &loop](uvcpp_stream* st, ssize_t nr, const uv_buf_t* bb) {
                      
                      if (nr > 0) {
                        std::string ss(bb->base, (size_t)nr);
                        std::cout << "[functional poll] server read: " << ss << std::endl;
                        std::unique_ptr<uvcpp_buf> bcpp(new uvcpp_buf(bb->base, nr));
                        uvcpp_write* ww = new uvcpp_write();
                        ww->set_uv_buf(bcpp->out_uv_buf(), true);
                        st->write(ww, ww->get_uv_buf(), 1, [ww, bcpp = bcpp.release()](uvcpp_write*, int) {
                          delete bcpp; delete ww;
                        });
                        poller->start(UV_READABLE, [peer, poller, &loop](uvcpp_poll* pp, int ss, int ee) {
                          pp->stop();
                          peer->read_start(
                            [](uvcpp_handle*, size_t, uv_buf_t* b) { uvcpp_buf::alloc_buf(b, 4096); },
                            [peer, poller, &loop](uvcpp_stream* stt, ssize_t nrr, const uv_buf_t* bbb) {
                              if (nrr <= 0) {
                                poller->stop(); poller->close();
                                stt->close([&loop, peer](uvcpp_handle*){ loop.stop(); delete peer; });
                              }
                              uvcpp_buf::free_buf(const_cast<uv_buf_t*>(bbb));
                            }
                          );
                        });
                      } else if (nr <= 0) {
                        poller->stop(); poller->close();
                        st->close([&loop, peer](uvcpp_handle*){ loop.stop(); delete peer; });
                      }
                      uvcpp_buf::free_buf(const_cast<uv_buf_t *>(bb));
                    }
                  );
                });
              }
              uvcpp_buf::free_buf(const_cast<uv_buf_t *>(buf));
            }
          );
        });
      };

      restart_poll();
    }, 128);

    // Watchdog timer
    uvcpp_timer watchdog(&loop);
    watchdog.start([&loop](uvcpp_timer* t) {
      std::cout << "[functional poll] server watchdog timeout, stopping loop\n";
      loop.stop();
      t->stop();
    }, 5000, 0);

    loop.run(UV_RUN_DEFAULT);
  });

  // Client thread
  std::thread client_thread([&]() {
    int port = port_future.get();
    uvcpp_loop client_loop;
    client_loop.init();
    uvcpp_tcp client(&client_loop);

    uvcpp_async client_stop_async;
    client_stop_async.init([&client_loop](uvcpp_async*) { client_loop.stop(); }, &client_loop);

    uvcpp_connect* conn = new uvcpp_connect();
    sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", port, &addr);
    client.connect(conn, (const sockaddr*)&addr, [&](uvcpp_connect*, int status) {
      if (status == 0) {
        client.read_start(
          [](uvcpp_handle*, size_t sz, uv_buf_t* buf) { uvcpp_buf::alloc_buf(buf, sz); },
          [&](uvcpp_stream* stream, ssize_t nread, const uv_buf_t* buf) {
            if (nread > 0) {
              std::string s(buf->base, (size_t)nread);
              std::cout << "[functional poll] client recv: " << s << std::endl;
              success.store(true);
              stream->close([&](uvcpp_handle*){ client_stop_async.send(); });
            }
            uvcpp_buf::free_buf(const_cast<uv_buf_t *>(buf));
          }
        );

        const char* msg = "poll_ping";
        std::unique_ptr<uvcpp_buf> bufcpp(new uvcpp_buf(msg));
        uvcpp_write* w = new uvcpp_write();
        w->set_uv_buf(bufcpp->out_uv_buf(), true);
        client.write(w, w->get_uv_buf(), 1, [w, bufcpp = bufcpp.release()](uvcpp_write*, int) {
          delete bufcpp;
          delete w;
        });
      } else {
        std::cout << "[functional poll] client connect failed status=" << status << std::endl;
        client_loop.stop();
      }
      delete conn;
    });

    uvcpp_timer client_watchdog(&client_loop);
    client_watchdog.start([&client_loop, &client_watchdog](uvcpp_timer* t) {
      std::cout << "[functional poll] client watchdog timeout, stopping loop\n";
      client_loop.stop();
      t->stop();
    }, 5000, 0);

    client_loop.run(UV_RUN_DEFAULT);
  });

  client_thread.join();
  server_thread.join();

  std::cout << "[functional poll] done success=" << (success.load() ? "true" : "false") << std::endl;
  return success.load() ? 0 : 2;
}

