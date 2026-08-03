#include <iostream>
#include <thread>
#include <uv.h>
#include <future>
#include <atomic>
#include <cstring>
#include <string>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_pipe.h"
#include "handle/uvcpp_timer.h"
#include "handle/uvcpp_async.h"
#include "req/uvcpp_connect.h"
#include "req/uvcpp_write.h"
#include "uvcpp/uvcpp_buf.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace uvcpp;

int main() {
  std::cout << "[functional pipe] start\n";

  std::atomic<bool> success(false);
  std::string pipe_name;
#if defined(_WIN32)
  pipe_name = "\\\\.\\pipe\\uvcpp_pipe_test_" + std::to_string(GetCurrentProcessId());
#else
  pipe_name = "/tmp/uvcpp_pipe_test_" + std::to_string(getpid());
#endif

  std::promise<void> server_ready;
  auto server_ready_future = server_ready.get_future();
  std::promise<void> client_loop_ready;
  auto client_loop_ready_future = client_loop_ready.get_future();

  // server thread: loop created and run in the same thread
  std::thread server_thread([&](){
    uvcpp_loop server_loop;
    server_loop.init();

    uvcpp_async server_stop_async;
    server_stop_async.init([&server_loop](uvcpp_async*) {
      server_loop.stop();
    }, &server_loop);

    uvcpp_pipe server(&server_loop, 0);
#if !defined(_WIN32)
    ::unlink(pipe_name.c_str());
#endif
    int rc = server.bind(pipe_name.c_str());
    if (rc != 0) {
      std::cout << "[functional pipe] server bind failed rc=" << rc
                << " (" << uv_err_name(rc) << ") "
                << uv_strerror(rc) << std::endl;
      exit(rc);
      return;
    }
    std::cout << "[functional pipe] server bind/listen setup\n";

    server.listen([&](uvcpp_stream* s, int status){
      if (status < 0) return;
      uvcpp_pipe* peer = new uvcpp_pipe(&server_loop, 0);
      int rc = s->accept(peer);
      if (rc != 0) {
        std::cout << "[functional pipe] server accept failed rc=" << rc << " ("
                  << uv_err_name(rc) << ") " << uv_strerror(rc) << std::endl;
        delete peer;
        return;
      }

      // Pipe is a stream, use read_start directly (no poll needed)
      peer->read_start(
        [](uvcpp_handle*, size_t sz, uv_buf_t* buf) {
          uvcpp_buf::alloc_buf(buf, sz > 0 ? sz : 4096);
        },
        [peer, &server_loop](uvcpp_stream* stream, ssize_t nread, const uv_buf_t* buf) {
          if (nread > 0) {
            std::string s(buf->base, (size_t)nread);
            std::cout << "[functional pipe] server recv: " << s << std::endl;
            // Echo back
            std::unique_ptr<uvcpp_buf> bufcpp(new uvcpp_buf(buf->base, nread));
            uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));

            uvcpp_write* w = new uvcpp_write();
            w->set_uv_buf(bufcpp->out_uv_buf(), true);
            stream->write(w, w->get_uv_buf(), 1, [w, bufcpp = bufcpp.release()](uvcpp_write* req, int stat){
              delete bufcpp;
              delete req;
            });
          } else {
            uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
            stream->close([&server_loop, peer](uvcpp_handle*){
              server_loop.stop();
              delete peer;
            });
          }
        }
      );
    }, 128);

    // Watchdog timer
    uvcpp_timer server_watchdog(&server_loop);
    server_watchdog.start([&server_loop](uvcpp_timer *t){
      std::cout << "[functional pipe] server watchdog timeout, stopping loop\n";
      server_loop.stop();
      t->stop();
    }, 5000, 0);

    // Wait for client loop to be ready, then signal
    client_loop_ready_future.wait();
    std::cout << "[functional pipe] server listen configured, signaling client\n";
    server_ready.set_value();
    server_loop.run(UV_RUN_DEFAULT);
  });

  // client thread: loop created and run in the same thread
  std::thread client_thread([&](){
    uvcpp_loop client_loop;
    client_loop.init();

    // Client async: triggered when server is ready
    uvcpp_async client_start_async;
    client_start_async.init([&](uvcpp_async*) {
      uvcpp_pipe *client = new uvcpp_pipe(&client_loop, 1);  // ipc=1 for IPC mode
      uvcpp_connect *conn = new uvcpp_connect();
      client->connect(
          conn, pipe_name.c_str(),
          [conn, client, &success, &client_loop](uvcpp_connect *r, int status) {
        if (status == 0) {
          client->read_start(
            [](uvcpp_handle* h, size_t suggested_size, uv_buf_t* buf){
              uvcpp_buf::alloc_buf(buf, suggested_size > 0 ? suggested_size : 4096);
            },
            [client, &success, &client_loop](uvcpp_stream *stream, ssize_t nread, const uv_buf_t *buf) {
              if (nread > 0) {
                std::string s(buf->base, (size_t)nread);
                std::cout << "[functional pipe] client recv: " << s << std::endl;
                success.store(true);
              }
              uvcpp_buf::free_buf(const_cast<uv_buf_t*>(buf));
              stream->close([&client_loop, client](uvcpp_handle*) {
                client_loop.stop();
                delete client;
              });
            }
          );

          const char *msg = "pipe_hello";
          std::unique_ptr<uvcpp_buf> bufcpp(new uvcpp_buf(msg));
          uvcpp_write *w = new uvcpp_write();
          w->set_uv_buf(bufcpp->out_uv_buf(), true);
          client->write(w, w->get_uv_buf(), 1, [w, bufcpp = bufcpp.release()](uvcpp_write* req, int stat){
            delete bufcpp;
            delete req;
          });
        } else {
          std::cout << "[functional pipe] client error status: " << status
                    << " name=" << uv_err_name(status)
                    << " msg=" << uv_strerror(status) << std::endl;
          client_loop.stop();
          delete client;
        }
        delete conn;
      });
    }, &client_loop);

    // Signal that client loop is ready
    client_loop_ready.set_value();

    // Wait for server to be ready, then trigger client
    server_ready_future.wait();

    // Send async to start client connection
    client_start_async.send();

    // Watchdog timer
    uvcpp_timer client_watchdog(&client_loop);
    client_watchdog.start([&client_loop](uvcpp_timer *t) {
      std::cout << "[functional pipe] client watchdog timeout, stopping loop\n";
      client_loop.stop();
      t->stop();
    }, 5000, 0);

    client_loop.run(UV_RUN_DEFAULT);
  });

  client_thread.join();
  server_thread.join();

  std::cout << "[functional pipe] done success=" << (success.load() ? "true" : "false") << std::endl;
  return success.load() ? 0 : 2;
}
