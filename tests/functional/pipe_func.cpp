#include <iostream>
#include <thread>
#include <uv.h>
#include <future>
#include <atomic>
#include <cstring>
#include "handle/uvcpp_loop.h"
#include "handle/uvcpp_pipe.h"
#include "handle/uvcpp_timer.h"
#include "handle/uvcpp_async.h"
#include "req/uvcpp_connect.h"
#include "req/uvcpp_write.h"
#include "req/uvcpp_work.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace uvcpp;

int main() {
  std::cout << "[functional pipe] start\n";
  uvcpp_loop loop;
  loop.init();

  // Two-thread pipe test (server/client) with async readiness and stop signals.
  std::atomic<bool> success(false);
  std::string pipe_name;
#if defined(_WIN32)
  pipe_name = "\\\\.\\pipe\\uvcpp_pipe_test";
#else
  pipe_name = "/tmp/uvcpp_pipe_test";
#endif

  std::promise<void> server_ready;
  auto server_ready_future = server_ready.get_future();
  std::atomic<uvcpp_async*> client_ready_async_ptr(nullptr);

  uvcpp_async server_stop_async;
  uvcpp_async client_ready_async;

  uvcpp_loop server_loop;
  server_loop.init();
  // client loop and its ready async created before server thread so server can notify it
  uvcpp_loop client_loop;
  client_loop.init();

  // init async to stop server (client will call)
  server_stop_async.init([&server_loop](uvcpp_async *a) { 
      server_loop.stop(); 
      },
                         &server_loop);

  client_ready_async.init([&](uvcpp_async* a) {
    // runs on client loop when server signals ready: connect and exchange via pipe
    uvcpp_pipe *client = new uvcpp_pipe(&client_loop, 0);
    uvcpp_connect *conn = new uvcpp_connect();
    client->connect(
        conn, pipe_name.c_str(),
        [conn, client, &success, &client_loop, &server_stop_async](uvcpp_connect *r, int status) {
      if (status == 0) {
        // prepare to read echo
        client->read_start(
          [](uvcpp_handle* h, size_t suggested_size, uv_buf* buf){
            buf->base = (char*)uvcpp::uv_alloc_bytes(suggested_size);
            buf->len = suggested_size;
          },
          [&success, &client_loop, &server_stop_async](
                uvcpp_stream *stream, ssize_t nread,
                                 const uv_buf *buf) {
            if (nread > 0) {
              std::string s(buf->base, (size_t)nread);
              std::cout << "[functional pipe] client recv: " << s << std::endl;
              success.store(true);
              // notify server to stop, then close and stop client
              server_stop_async.send();
              stream->close(
                  [&client_loop](uvcpp_handle *hd) {
                client_loop.stop();
                delete hd;
              });
            }
            if (buf && buf->base) UVCPP_VFREE(((uv_buf*)buf)->base)
          }
        );

        // send message
        const char *msg = "pipe_hello";
        uv_buf *b = uvcpp::uv_alloc<uv_buf>();
        b->base = (char*)uvcpp::uv_alloc_bytes(strlen(msg));
        memcpy(b->base, msg, strlen(msg));
        b->len = (unsigned int)strlen(msg);
        uvcpp_write *w = new uvcpp_write();
        w->set_buf(b, true);
        client->write(w, b, 1, [](uvcpp_write* req, int stat){
          delete req;
        });
      } else {
        std::cout << "[functional pipe] client error status: " << status
                  << " name=" << uv_err_name(status)
                  << " msg=" << uv_strerror(status) << std::endl;
        client_loop.stop();
      }
      delete conn;
    });
  }, &client_loop);
  client_ready_async_ptr.store(&client_ready_async);

  // server thread
  std::thread server_thread([&](){
   
    uvcpp_pipe server(&server_loop, 0);
#if !defined(_WIN32)
    // remove leftover socket file to avoid bind failure when running whole suite
    ::unlink(pipe_name.c_str());
#endif
    int rc = server.bind(pipe_name.c_str());
    if (rc != 0) {
      std::cout << "[functional pipe] server bind failed rc=" << rc
                << " (" << uv_err_name(rc) << ") "
                << uv_strerror(rc) << std::endl;
      server_loop.stop();
      return;
    }
    std::cout << "[functional pipe] server bind/listen setup\n";
    server.listen([&](uvcpp_stream* s, int status){
      if (status < 0) return;
      // accept into a peer handle and hand off to worker via uvcpp_work
      // create a client pipe bound to the worker loop (so accept transfers to worker loop)

      struct new_client_data {
        uvcpp_loop *work_loop;
        uvcpp_pipe *client;
      };

      new_client_data *client_data = new new_client_data();
      client_data->work_loop = new uvcpp_loop();
      client_data->work_loop->init();
      client_data->client = new uvcpp_pipe(client_data->work_loop, 0);
      s->accept(client_data->client);
      std::cout << "[functional pipe] accepted client, queued to work\n";

      uvcpp_work* work = new uvcpp_work();
      work->init();
      work->set_data(client_data);

      work->queue_work(&server_loop,
        // work_cb: runs in worker thread
        [](uvcpp_work* w) {
          std::cout << "[functional pipe] work_cb start\n";
          new_client_data *cd = (new_client_data *)w->get_data();
          uvcpp_pipe *client = cd->client;
          uvcpp_loop *work_loop = cd->work_loop;

          client->read_start(
            [](uvcpp_handle* h, size_t suggested_size, uv_buf* buf){
              buf->base = (char*)uvcpp::uv_alloc_bytes(suggested_size);
              buf->len = suggested_size;
            },
            [work_loop](uvcpp_stream* stream, ssize_t nread, const uv_buf* buf){
              if (nread > 0) {
                std::string s(buf->base, (size_t)nread);
                std::cout << "[worker pipe] recv: " << s << std::endl;
                // echo back
                uv_buf *echo = uvcpp::uv_alloc<uv_buf>();
                echo->base = (char*)uvcpp::uv_alloc_bytes((size_t)nread);
                memcpy(echo->base, buf->base, (size_t)nread);
                echo->len = (unsigned int)nread;
                uvcpp_write *wreq = new uvcpp_write();
                wreq->set_buf(echo, true);
                stream->write(wreq, echo, 1, [](uvcpp_write* req, int stat){
                  delete req;
                });
              } else if (nread == UV_EOF || nread < 0) {
                stream->close([=](uvcpp_handle *hd) {
                  work_loop->stop();
                });
              }
              if (buf && buf->base) UVCPP_VFREE(((uv_buf*)buf)->base)
            }
          );
          // run the worker loop for this client
          work_loop->run(UV_RUN_DEFAULT);
          std::cout << "[functional pipe] work_cb end\n";

        },
        // after_work: runs on server loop thread, cleanup
        [&server_loop](uvcpp_work *w, int status) {
          std::cout << "[functional pipe] after_work start status=" << status << std::endl;
          new_client_data *cd = (new_client_data *)w->get_data();
          uvcpp_pipe *client = cd->client;
          uvcpp_loop *work_loop = cd->work_loop;
          delete client;
          delete work_loop;
          delete cd;
          std::cout << "[functional pipe] after_work cleanup done\n";
          delete w;
        }
      );
    }, 128);

    // start a watchdog timer on server loop to avoid hangs
    uvcpp_timer server_watchdog(&server_loop);
    server_watchdog.start([&server_loop](uvcpp_timer *t){
      std::cout << "[functional pipe] server watchdog timeout, stopping loop\n";
      server_loop.stop();
      t->stop();
    }, 10000, 0);

    std::cout << "[functional pipe] server listen configured, signaling client\n";
    // server ready -> notify client via client's async
    uvcpp_async* client_async = client_ready_async_ptr.load();
    if (client_async) client_async->send();
    server_ready.set_value();
    server_loop.run(UV_RUN_DEFAULT);

  });

  // start a watchdog timer on client loop
  uvcpp_timer *client_watchdog = new uvcpp_timer(&client_loop);
  client_watchdog->start(
      [&client_loop, client_watchdog](uvcpp_timer *t) {
        std::cout
            << "[functional pipe] client watchdog timeout, stopping loop\n";
        client_loop.stop();
        t->stop();
      },
      10000, 0);

  // run client loop in separate thread
  std::thread client_thread([&client_loop]() {
    client_loop.run(UV_RUN_DEFAULT);
  });

  client_thread.join();
  server_thread.join();

  std::cout << "[functional pipe] done success=" << (success.load() ? "true" : "false") << std::endl;
  return success.load() ? 0 : 2;
}


