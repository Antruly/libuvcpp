#include <iostream>
#include "uvcpp/uvcpp_buf.h"
#include "uvcpp/uvcpp_barrier.h"
#include "uvcpp/uvcpp_cpu_info.h"
#include "uvcpp/uvcpp_dir.h"
#include "uvcpp/uvcpp_dirent.h"
#include "uvcpp/uvcpp_env_item.h"
#include "uvcpp/uvcpp_group.h"
#include "uvcpp/uvcpp_interface_address.h"
#include "uvcpp/uvcpp_metrics.h"
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
    uvcpp_group g; g.init();
    uvcpp_interface_address ia; ia.init();
    uvcpp_metrics m; // may require loop; call init if available
    m.init();
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