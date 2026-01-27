#include <iostream>
#include "req/uvcpp_req.h"
#include "req/uvcpp_write.h"
#include "req/uvcpp_connect.h"
#include "req/uvcpp_fs.h"
#include "req/uvcpp_getaddrinfo.h"
#include "req/uvcpp_getnameinfo.h"
#include "req/uvcpp_random.h"
#include "req/uvcpp_shutdown.h"
#include "req/uvcpp_udp_send.h"
#include "req/uvcpp_work.h"

using namespace uvcpp;

int main() {
  try {
    uvcpp_req r;
    r.req_size();

    uvcpp_write *w = new uvcpp_write();
    w->init();
    delete w;

    uvcpp_connect c;
    c.init();

    uvcpp_fs f;
    f.init();

    uvcpp_getaddrinfo gai;
    gai.init();

    uvcpp_getnameinfo gni;
    gni.init();

    uvcpp_random rnd;
    // init may be guarded by libuv version
    rnd.init();

    uvcpp_shutdown sd;
    sd.init();

    uvcpp_udp_send us;
    us.init();

    uvcpp_work work;
    work.init();

    std::cout << "[unit][req] ok\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "exception: " << e.what() << std::endl;
    return 3;
  }
}