#include <iostream>
#include <cstring>
#include <thread>
#include <future>
#include <atomic>
#include <uv.h>

#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_tcp.h"
#include "handle/uvcpp_async.h"
#include "req/uvcpp_connect.h"
#include "req/uvcpp_write.h"
#include "uvcpp/uvcpp_buf.h"
#include "loop_drain.h"


using namespace uvcpp;

static const char *kMsg = "hello_uvcpp";

int main() {
  std::cout << "[functional tcp] start\n";

  std::atomic<bool> success(false);
  std::promise<int> port_promise;
  auto port_future = port_promise.get_future();

  // client loop + async: client loop must be running to receive async
  uvcpp_loop client_loop;

  uvcpp_test::loop_drain drain_client_loop(&client_loop);
  client_loop.init();
  uvcpp_async start_async;
  start_async.init(
      [&](uvcpp_async *a) {
        // this runs in client loop thread: perform connect -> write -> read for echo
        int port = port_future.get();

        uvcpp_tcp *client = new uvcpp_tcp(&client_loop);
        uvcpp_connect *conn = new uvcpp_connect();

        sockaddr_in addr;
        uv_ip4_addr("127.0.0.1", port, &addr);

        client->connect(conn, (const sockaddr*)&addr, [client, conn, &client_loop, &success](uvcpp_connect* r, int status){
          if (status == 0) {
            // start read to get echo
        client->read_start(
          [](uvcpp_handle *h, size_t suggested_size, uv_buf_t* buf) {
            uvcpp_buf::alloc_buf(buf, suggested_size);
          },
          [client, &client_loop, &success](uvcpp_stream *stream, ssize_t nread, const uv_buf_t* buf) {
            if (nread > 0) {
              std::string received(buf->base, (size_t)nread);
              std::cout << "[functional tcp client] recv: " << received << std::endl;
              if (received == std::string(kMsg)) success.store(true);
            } else if (nread == UV_EOF) {
              // peer closed
            }
            uvcpp_free_bytes(buf->base);
            // close client and stop loop
            stream->close([&client_loop, client](uvcpp_handle*){
              client_loop.stop();
              delete client;
            });
          }
        );

            // write message using uvcpp_buf
            uvcpp_buf bufcpp(kMsg);
            uv_buf_t* b = bufcpp.out_uv_buf();
            uvcpp_write *w = new uvcpp_write();
            w->set_uv_buf(b, true);
            client->write(w, b, 1, [](uvcpp_write* req, int stat){
              // free write req (will free buffer if owner)
              if (stat < 0) {
                std::cout << "[functional tcp] client write error : "
                          << uv_strerror(stat) << std::endl;
              }
              delete req;
            });
            
          } else {
            client_loop.stop();
            delete client;
          }
          delete conn;
        });
      },
      &client_loop);

  // start client loop thread
  std::thread client_thread([&client_loop](){
    client_loop.run(UV_RUN_DEFAULT);
  });

  // Server thread: own loop, bind/listen, accept and echo
  std::thread server_thread([&start_async, &port_promise, &success](){
    uvcpp_loop server_loop;

    uvcpp_test::loop_drain drain_server_loop(&server_loop);
    server_loop.init();

    uvcpp_tcp server(&server_loop);

    int r = server.bindIpv4("127.0.0.1", 0);
    (void)r;

    sockaddr_in name;
    int namelen = sizeof(name);
    server.getsockname((sockaddr*)&name, &namelen);
    int port = ntohs(name.sin_port);
    if (port == 0) {
      int fallback = 8000;
      int rb = server.bindIpv4("127.0.0.1", fallback);
      if (rb == 0) {
        server.getsockname((sockaddr*)&name, &namelen);
        port = ntohs(name.sin_port);
      } else {
        std::cerr << "[functional tcp] bind failed rb=" << rb << std::endl;
      }
    }
    std::cout << "[functional tcp] server listening on port " << port << std::endl;
    port_promise.set_value(port);

    r = server.listen(
        [&server, & server_loop, &success, &start_async](uvcpp_stream *s,
                                                        int status) {
      if (status < 0) return;
      // **被接受的句柄必须建在"接受它的那条循环"上。**
      //
      // `uv_accept()` 要求 `server->loop == client->loop`：libuv 在
      // `unix/stream.c` 里有这条断言，Windows 的 `uv__accept` 没有 —— 所以
      // "把连接接进另一条循环"这个错误只在 POSIX 上现形。原先这里是
      // `new uvcpp_loop()` 造出第二条循环 `srv_loop`、再把 `srv_client` 建在它
      // 上面，于是 Ubuntu CI 每轮都是
      // `uv_accept: Assertion 'server->loop == client->loop' failed`。
      //
      // 句柄归哪条循环，它的读/写就只能在**那条循环的线程**上做：所以回显放回
      // server_loop，不再交给工作线程（那条路本来也是跨线程操作别人的句柄）。
      uvcpp_tcp *srv_client = new uvcpp_tcp(&server_loop);
      s->accept(srv_client);

      srv_client->read_start(
          [](uvcpp_handle *h, size_t suggested_size, uv_buf_t *buf) {
            uvcpp_buf::alloc_buf(buf, suggested_size);
          },
          [srv_client, &server, &server_loop](uvcpp_stream *stream, ssize_t nread,
                                              const uv_buf_t *buf) {
            if (nread > 0) {
              std::string received(buf->base, (size_t)nread);
              std::cout << "[functional tcp] server recv: " << received
                        << std::endl;
              // echo back
              uvcpp_buf bufcpp(buf->base, nread);
              uv_buf_t *echo = bufcpp.out_uv_buf();
              uvcpp_write *wreq = new uvcpp_write();
              wreq->set_uv_buf(echo, true);
              stream->write(wreq, echo, 1,
                            [](uvcpp_write *req, int stat) { delete req; });
            } else if (nread == UV_EOF || nread < 0) {
              // 对端收到回显就关了，收工：先关被接受的句柄，再关服务器、停循环
              // （顺序与 `~uvcpp_tcp_server` 一致）。
              stream->close([srv_client](uvcpp_handle *) { delete srv_client; });
              server.close([&server_loop](uvcpp_handle *) { server_loop.stop(); });
            }
            uvcpp_free_bytes(buf->base);
          });
    }, 128);
    // notify client loop to start (after listen set up)
    start_async.send();
    server_loop.run(UV_RUN_DEFAULT);
  });

  client_thread.join();
  server_thread.join();

  std::cout << "[functional tcp] done success=" << (success.load() ? "true" : "false") << std::endl;
  return success.load() ? 0 : 2;
}
