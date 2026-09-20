#include <iostream>
#include <cstring>
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

// `DEFINE_FUNC_REQ_CPP` / `DEFINE_COPY_FUNC_REQ_CPP` 的唯一展开点。
//
// 这两个宏在仓库里零使用，而且**从来没有编过**，三处都是硬错误：同一个默认构造
// 被定义了两次（redefinition）、基类写成不存在的 `uvcpp_req(nullptr)`、分配器写成
// 不存在的 `uvcpp::uv_alloc<T>()`。下面这个探针按 `uvcpp_connect` / `uvcpp_fs`
// 那几个手写类的形状写 —— 它编得过、链得过，就说明这三个缺陷没有回来。
// 动这两个宏的时候，这里会先响。
namespace uvcpp {
class probe_req_macro : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(probe_req_macro)
  // `DEFINE_COPY_FUNC_REQ_CPP` 是在类外**定义**拷贝构造与赋值，所以类里必须先
  // 声明它们（`UVCPP_DEFINE_COPY_FUNC` 就是干这个的；`uvcpp_req` 自己也是这么写的）。
  UVCPP_DEFINE_COPY_FUNC(probe_req_macro)
  int init();
};

DEFINE_FUNC_REQ_CPP(probe_req_macro, uv_timer_t)
DEFINE_COPY_FUNC_REQ_CPP(probe_req_macro, uv_timer_t)

int probe_req_macro::init() {
  memset(this->get_req(), 0, sizeof(uv_timer_t));
  this->set_req_data();
  return 0;
}
}  // namespace uvcpp

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

    // 两个生成宏的展开点（见上面那个探针类）：建、初始化、析构都要走得通。
    probe_req_macro prm;
    prm.init();

    std::cout << "[unit][req] ok\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "exception: " << e.what() << std::endl;
    return 3;
  }
}