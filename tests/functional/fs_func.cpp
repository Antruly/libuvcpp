#include <iostream>
#include <cstring>
#include <cstdio>
#include "handle/uvcpp_loop.h"
#include "req/uvcpp_fs.h"
#include "uvcpp/uvcpp_buf.h"
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
      // prepare write buffer using uvcpp_buf
      const char *txt = "hello functional fs";
      uvcpp_buf bufcpp(txt);
      uv_buf_t* b = bufcpp.out_uv_buf();
      // write async with callback; pass the underlying uv_buf_t from uvcpp_buf
      fs.write(&loop, fd, b, 1, 0, [&fs, fd, &done_promise, &loop](uvcpp_fs* wreq){
        if (wreq->get_result() < 0) {
          int err = (int)wreq->get_result();
          std::cerr << "[functional fs] write failed: " << err << " "
                    << uv_err_name(err) << " - " << uv_strerror(err) << std::endl;
          fs.close(&loop, fd);
          done_promise.set_value(3);
          return;
        }
        // close async
        fs.close(&loop, fd, [&done_promise](uvcpp_fs* creq){
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
    // allocate read buffer using uvcpp_buf
    uvcpp_buf rb;
    rb.resize(4096);
    uv_buf_t* read_buf = rb.out_uv_buf();

    fs2.read(&loop, fd, read_buf, 1, 0, [&fs2, &loop, &cleanup_promise, fname, fd, read_buf](uvcpp_fs* rreq) {
      if (rreq->get_result() < 0) {
        int err = (int)rreq->get_result();
        std::cerr << "[functional fs] read failed: " << err << " "
                  << uv_err_name(err) << " - " << uv_strerror(err) << std::endl;
        // attempt close then remove via C
        fs2.close(&loop, fd, [&cleanup_promise, fname](uvcpp_fs* creq){
          std::remove(fname);
          cleanup_promise.set_value(6);
        });
        return;
      }
      ssize_t nread = rreq->get_result();
      std::string read_back;
      if (nread > 0 && read_buf->base)
        read_back.assign(read_buf->base, (size_t)nread);
      std::cout << "[functional fs] read: " << read_back << std::endl;
      // close file then unlink
        fs2.close(&loop, fd, [&cleanup_promise, fname](uvcpp_fs* creq){
                  cleanup_promise.set_value(7);
      });
    });
  });
  if (rc2 < 0) {
    std::cerr << "[functional fs] open(read) scheduling failed: " << rc2 << std::endl;
    std::remove(fname);
    return 8;
  }
  // run loop until read done
  loop.run(UV_RUN_DEFAULT);

  // create a separate promise for the unlink step to avoid re-using the previous one
  std::promise<int> unlink_promise;
  auto unlink_future = unlink_promise.get_future();

  uvcpp_fs fs3;
  fs3.init();
  // after close, unlink file
  int rc3 = fs3.unlink(&loop, fname,
             [&unlink_promise, &fs3, &loop, fname](uvcpp_fs *ureq) {
               if (ureq->get_result() < 0) {
                 int err = (int)ureq->get_result();
                 std::cerr << "[functional fs] unlink failed: " << err << " "
                           << uv_err_name(err) << " - " << uv_strerror(err)
                           << std::endl;
                 // fallback to C remove
                 std::remove(fname);
                 unlink_promise.set_value(10);
               } else {
                 unlink_promise.set_value(0);
               }
             });
  if (rc3 < 0) {
    std::cerr << "[functional fs] unlink(remove) scheduling failed: " << rc3
              << std::endl;
    std::remove(fname);
    return 9;
  }

  // run loop until unlink done
  loop.run(UV_RUN_DEFAULT);
  int final_rc = unlink_future.get();
  std::cout << "[functional fs] done\n";
  return final_rc == 0 ? 0 : final_rc;
}

