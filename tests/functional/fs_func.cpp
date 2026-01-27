#include <iostream>
#include <cstring>
#include <cstdio>
#include "handle/uvcpp_loop.h"
#include "req/uvcpp_fs.h"
#include <future>
#include <uv.h>

using namespace uvcpp;

int main() {
  std::cout << "[functional fs] start\n";
  uvcpp_loop loop;
  loop.init();

  uvcpp_fs fs;
  fs.init();

  const char* fname = "uvcpp_fn_test.txt";
  // Use asynchronous callbacks and run the loop to completion.
  std::promise<int> done_promise;
  auto done_future = done_promise.get_future();

  // open with callback
  int rc = fs.open(&loop, fname, O_CREAT | O_WRONLY, 0644,
    // open callback
    [&fs, &loop, &done_promise](uvcpp_fs* req){
      if (req->get_result() < 0) {
        std::cerr << "[functional fs] open failed: " << req->get_result() << std::endl;
        done_promise.set_value(2);
        return;
      }
      uv_file fd = (uv_file)req->get_result();
      // prepare write buffer (allocate on heap because uv_fs_write is asynchronous
      // and buffers must remain valid until the request completes)
      const char *txt = "hello functional fs";
      uvcpp::uv_buf *b = uvcpp::uv_alloc<uvcpp::uv_buf>();
      b->base = (char*)uvcpp::uv_alloc_bytes(strlen(txt));
      memcpy(b->base, txt, strlen(txt));
      b->len = (unsigned int)strlen(txt);
      // write async with callback
      fs.write(&loop, fd, b, 1, 0, [&fs, fd, &done_promise, &loop, b](uvcpp_fs* wreq){
        if (wreq->get_result() < 0) {
          int err = (int)wreq->get_result();
          std::cerr << "[functional fs] write failed: " << err << " "
                    << uv_err_name(err) << " - " << uv_strerror(err) << std::endl;
          fs.close(&loop, fd);
          // free buffer
          if (b && b->base) uvcpp::uv_free_bytes(b->base);
          delete b;
          done_promise.set_value(3);
          return;
        }
        // close async
        fs.close(&loop, fd, [&done_promise, b](uvcpp_fs* creq){
          // free buffer
          if (b && b->base) uvcpp::uv_free_bytes(b->base);
          delete b;
          // completion
          done_promise.set_value(0);
        });
      });
    });
  if (rc < 0) {
    std::cerr << "[functional fs] open scheduling failed: " << rc << std::endl;
    return 2;
  }

  // run loop until done
  loop.run(UV_RUN_DEFAULT);
  int result = done_future.get();

  // read back using uvcpp_fs (async) and then unlink using uvcpp_fs
  if (result != 0) {
    std::cerr << "[functional fs] failed with code " << result << std::endl;
    // try cleanup via C remove just in case
    std::remove(fname);
    return result;
  }

  std::promise<int> cleanup_promise;
  auto cleanup_future = cleanup_promise.get_future();

  uvcpp_fs fs2;
  fs2.init();

  // open for read asynchronously
  int rc2 = fs2.open(&loop, fname, O_RDONLY, 0, [&fs2, &loop, &cleanup_promise, fname](uvcpp_fs* oreq) {
    if (oreq->get_result() < 0) {
      int err = (int)oreq->get_result();
      std::cerr << "[functional fs] open(read) failed: " << err << " "
                << uv_err_name(err) << " - " << uv_strerror(err) << std::endl;
      // fallback to C remove
      std::remove(fname);
      cleanup_promise.set_value(5);
      return;
    }
    uv_file fd = (uv_file)oreq->get_result();
    // allocate read buffer
    uv_buf_t* rb = new uv_buf_t();
    rb->base = (char*)uvcpp::uv_alloc_bytes(4096);
    rb->len = 4096;

    fs2.read(&loop, fd, reinterpret_cast<const uv_buf*>(rb), 1, 0, [&fs2, &loop, &cleanup_promise, fname, fd, rb](uvcpp_fs* rreq) {
      if (rreq->get_result() < 0) {
        int err = (int)rreq->get_result();
        std::cerr << "[functional fs] read failed: " << err << " "
                  << uv_err_name(err) << " - " << uv_strerror(err) << std::endl;
        // attempt close then remove via C
        fs2.close(&loop, fd, [&cleanup_promise, fname](uvcpp_fs* creq){
          std::remove(fname);
          cleanup_promise.set_value(6);
        });
        if (rb && rb->base) uvcpp::uv_free_bytes(rb->base);
        delete rb;
        return;
      }
      ssize_t nread = rreq->get_result();
      std::string read_back;
      if (nread > 0 && rb && rb->base) read_back.assign(rb->base, (size_t)nread);
      std::cout << "[functional fs] read: " << read_back << std::endl;
      // close file then unlink
      fs2.close(&loop, fd, [&fs2, &loop, &cleanup_promise, fname, rb](uvcpp_fs* creq){
        // after close, unlink file
        fs2.unlink(&loop, fname, [&cleanup_promise, &fs2, &loop, fname, rb](uvcpp_fs* ureq){
          if (ureq->get_result() < 0) {
            int err = (int)ureq->get_result();
            std::cerr << "[functional fs] unlink failed: " << err << " "
                      << uv_err_name(err) << " - " << uv_strerror(err) << std::endl;
            // fallback to C remove
            std::remove(fname);
            cleanup_promise.set_value(7);
          } else {
            cleanup_promise.set_value(0);
          }
          if (rb && rb->base) uvcpp::uv_free_bytes(rb->base);
          delete rb;
        });
      });
    });
  });
  if (rc2 < 0) {
    std::cerr << "[functional fs] open(read) scheduling failed: " << rc2 << std::endl;
    std::remove(fname);
    return 8;
  }

  // run loop until unlink done
  loop.run(UV_RUN_DEFAULT);
  int final_rc = cleanup_future.get();
  std::cout << "[functional fs] done\n";
  return final_rc == 0 ? 0 : final_rc;
}


