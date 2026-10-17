#include <iostream>
// 先拿到 `UV_VERSION_HEX`：下面两个头（group / metrics）在 libuv < 1.45 上
// **整个类都不存在**（它们自己的头就是这么守的），这里也得跟着守，否则发行版
// 包那棵按系统 libuv（Ubuntu 22.04 是 1.44.2）编的树会在测试目标上挂掉。
#include <uv.h>
#include "uvcpp/uvcpp_buf.h"
#include "uvcpp/uvcpp_barrier.h"
#include "uvcpp/uvcpp_cpu_info.h"
#include "uvcpp/uvcpp_dir.h"
#include "uvcpp/uvcpp_dirent.h"
#include "uvcpp/uvcpp_env_item.h"
#if UV_VERSION_HEX >= 0x012D00
#include "uvcpp/uvcpp_group.h"
#endif
#include "uvcpp/uvcpp_interface_address.h"
#if UV_VERSION_HEX >= 0x012D00
#include "uvcpp/uvcpp_metrics.h"
#endif
#include "uvcpp/uvcpp_passwd.h"
#include "uvcpp/uvcpp_rwlock.h"
#include "uvcpp/uvcpp_statfs.h"
#include "uvcpp/uvcpp_thread.h"
#include "uvcpp/uvcpp_utsname.h"
#include "uvcpp/uvcpp_version.h"

using namespace uvcpp;

int main() {
  std::cout << "[unit][uvcpp] start\n";
  try {
    uvcpp_buf b; b.init();
    uvcpp_barrier bar; bar.init(1); bar.destroy();
    uvcpp_cpu_info ci; ci.init();
    uvcpp_dirent de; de.init();
    uvcpp_dir d(&de); d.init();
    uvcpp_env_item ei; ei.init();
#if UV_VERSION_HEX >= 0x012D00
    uvcpp_group g; g.init();
#endif
    uvcpp_interface_address ia; ia.init();
#if UV_VERSION_HEX >= 0x012D00
    uvcpp_metrics m; // may require loop; call init if available
    m.init();
#endif
    uvcpp_passwd p; p.init(); p.free_passwd();
    uvcpp_rwlock rw; rw.init(); rw.destroy();
    uvcpp_statfs s; s.init();
    uvcpp_thread th; th.init();
    uvcpp_utsname u; u.init();

    auto *pb = new uvcpp_buf();
    delete pb;

    std::cout << "[unit][uvcpp] done\n";
    return 0;
  } catch(const std::exception &ex) {
    std::cerr << "exception: " << ex.what() << std::endl;
    return 2;
  }
}