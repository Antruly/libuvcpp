#include <iostream>
#include <cstring>
#include <thread>
#include <future>
#include <atomic>
#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_udp.h"
#include "req/uvcpp_udp_send.h"
#include "handle/uvcpp_timer.h"
#include "handle/uvcpp_async.h"
#include <chrono>

using namespace uvcpp;

int main() {
  std::cout << "[functional udp] start\n";
  std::atomic<bool> success(false);
  std::promise<int> port_promise;
  auto port_future = port_promise.get_future();
  uvcpp_async server_stop_async;
  std::atomic<uvcpp_async*> server_ready_async_ptr(nullptr);
  std::promise<void> server_ready_created;
  auto server_ready_created_future = server_ready_created.get_future();

  uvcpp_loop client_loop;
  client_loop.init();
  uvcpp_udp client(&client_loop);
  uvcpp_async client_stop_async;
  client_stop_async.init([&client_loop](uvcpp_async* a){
    client_loop.stop();
  }, &client_loop);

  // create async on client loop which server will send to when ready
  uvcpp_async server_ready_async;
  server_ready_async.init(
      [&client, &port_future, &server_stop_async, &client_loop,
       &success](uvcpp_async *a) {
        int port = port_future.get();
        // runs in client loop: start recv and periodic send
        std::cout << "[functional udp] client ready async callback invoked\n";
        sockaddr_in dest;
        uv_ip4_addr("127.0.0.1", port, &dest);
        const char *msg = "udp_hello";
        uv_buf* b = new uv_buf();
        b->base = (char *)uvcpp::uv_alloc_bytes(strlen(msg));
        b->len = (unsigned int)strlen(msg);
        memcpy(b->base, msg, b->len);

        // create timer pointer first so recv callback can stop/delete it
        uvcpp_timer *timer = new uvcpp_timer(&client_loop);
        timer->set_data(b);

        client.recv_start(
            [](uvcpp_handle *h, size_t suggested_size, uv_buf *buf) {
              buf->base = (char *)uvcpp::uv_alloc_bytes(suggested_size);
              buf->len = suggested_size;
            },
            [&server_stop_async, &client_loop,
             &success, b, timer](uvcpp_udp *u, ssize_t nread,
                                 const uv_buf *buf, const sockaddr *addr,
                                 unsigned flags) {
              if (nread > 0) {
                std::string s(buf->base, (size_t)nread);
                std::cout << "[functional udp] client recv: " << s << std::endl;
                // received echo -- notify server to stop and exit
                u->recv_stop();
                server_stop_async.send();
                // stop and delete timer before deleting b
                if (timer) {
                  timer->stop();
                  delete timer;
                }
                client_loop.stop();
                delete b;
                success.store(true);
              }
              if (buf && buf->base)
                UVCPP_VFREE(((uv_buf *)buf)->base)
            });

        // start periodic timer to send until echo received
        timer->start([dest, &client](uvcpp_timer *t) {
          uv_buf *b = (uv_buf *)t->get_data();
          uvcpp_udp_send *sreq = new uvcpp_udp_send();
          sreq->init();
          client.send(sreq, b, 1, (const sockaddr *)&dest,
                      [&](uvcpp_udp_send *r, int status) {
                        if (status < 0) {
                          std::cout << "[functional udp] client send error : "
                                    << uv_strerror(status) << std::endl;
                        }
                        delete r;
                      });
        },
                     100, 100);
      },
      &client_loop);

  // expose the async pointer to server and notify server it's ready
  server_ready_async_ptr.store(&server_ready_async);
  std::cout
      << "[functional udp] client registered ready async, notifying server\n";
  server_ready_created.set_value();

  // Server thread
  std::thread server_thread([&](){
    uvcpp_loop server_loop;
    server_loop.init();

    uvcpp_udp server(&server_loop);
    sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", 0, &addr);
    server.bind((const sockaddr*)&addr, 0);

    // init async used to stop the server loop
    server_stop_async.init([&server_loop](uvcpp_async* a){
      server_loop.stop();
    }, &server_loop);

    // get assigned port
    sockaddr_in name;
    int namelen = sizeof(name);
    server.getsockname((sockaddr*)&name, &namelen);
    int port = ntohs(name.sin_port);
    std::cout << "[functional udp] server bound port=" << port << std::endl;

    server.recv_start(
      [](uvcpp_handle* h, size_t suggested_size, uv_buf* buf){
        buf->base = (char*)uvcpp::uv_alloc_bytes(suggested_size);
        buf->len = suggested_size;
      },
      [&](uvcpp_udp* u, ssize_t nread, const uv_buf* buf, const sockaddr* addr, unsigned flags){
        if (nread > 0) {
          std::string s(buf->base, (size_t)nread);
          std::cout << "[functional udp] server recv: " << s << std::endl;
          // echo back using udp_send request
          uvcpp_udp_send *req = new uvcpp_udp_send();
          req->init();
          uv_buf* echo = new uv_buf();
          echo->base = (char*)uvcpp::uv_alloc_bytes((size_t)nread);
          memcpy(echo->base, buf->base, (size_t)nread);
          echo->len = (unsigned int)nread;
          req->set_data(echo);
          u->send(req, echo, 1, addr, [&](uvcpp_udp_send* r, int status){
            if (status < 0) {
              std::cout << "[functional udp] server echo send error : " << uv_strerror(status) << std::endl;
            }
            std::cout << "[functional udp] server echo send status=" << status << std::endl;
            uv_buf *echo = (uv_buf *)r->get_data();
            uvcpp::uv_free_bytes(echo->base);
            delete echo;
            delete r;
          });

        }
        if (buf && buf->base) UVCPP_VFREE(((uv_buf*)buf)->base)
      }
    );

    // now that recv_start is registered, publish port to client
    server.getsockname((sockaddr*)&name, &namelen);
    int port_after = ntohs(name.sin_port);
    port_promise.set_value(port_after);
    std::cout << "[functional udp] server listening (recv started) port=" << port_after << std::endl;

    // wait until client has created its ready-async, then notify client to start
    std::cout << "[functional udp] server waiting for client ready async\n";
    server_ready_created_future.get();
    std::cout << "[functional udp] server detected client ready async\n";
    uvcpp_async* ready_async = server_ready_async_ptr.load();
    if (ready_async) {
      std::cout << "[functional udp] server sending ready async\n";
      ready_async->send();
    } else {
      std::cout << "[functional udp] server ready async ptr is null\n";
    }

    server_loop.run(UV_RUN_DEFAULT);
  });

  // Client thread
  std::thread client_thread([&client_loop]() {
    client_loop.run(UV_RUN_DEFAULT);
  });

  // watchdog: stop test after timeout (ms)
  std::atomic<bool> watchdog_stopped(false);
  const int kTimeoutMs = 5000;
  std::thread watchdog([&](){
    using namespace std::chrono;
    auto start = steady_clock::now();
    while(!success.load()) {
      if (duration_cast<milliseconds>(steady_clock::now() - start).count() > kTimeoutMs) {
        std::cout << "[functional udp] watchdog timeout, stopping loops\n";
        // notify both loops to stop
        server_stop_async.send();
        client_stop_async.send();
        break;
      }
      std::this_thread::sleep_for(milliseconds(50));
    }
    watchdog_stopped.store(true);
  });

  client_thread.join();
  server_thread.join();
  if (!watchdog_stopped.load()) {
    // give watchdog a chance to finish
    watchdog.join();
  } else {
    watchdog.join();
  }

  std::cout << "[functional udp] done received=" << (success.load() ? "true" : "false") << std::endl;
  return success.load() ? 0 : 2;
}
 