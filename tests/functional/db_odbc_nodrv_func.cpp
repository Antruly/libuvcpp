/**
 * @file tests/functional/db_odbc_nodrv_func.cpp
 * @brief 数据库模块 · ODBC 后端：**这台机器上没有驱动管理器**时的行为。
 *
 * ## 这条用例在验什么
 *
 * 预编译包里带着 db 的四个后端，而 ODBC 那一支要的不是"某一个客户端库"，是**驱动
 * 管理器**（Linux 的 `libodbc.so.2`、Windows 的 `odbc32.dll`、macOS 的
 * `libodbc.2.dylib`）。管理器是运行期 `dlopen` / `LoadLibraryA` 找的
 * （`UVCPP_DB_RUNTIME_LOAD=ON`，所以它**不在** `DT_NEEDED` / 导入表里）—— 于是
 * "使用者机器上没装 unixODBC"是**预期之内**的部署形态，不是错误。这条用例钉住这个
 * 形态下的三件事：
 *
 *   1. `open("odbc://…")` 回 **`NO_DRIVER`** —— 不崩、不 abort，也不是"连接失败"
 *      那种含糊的码（`OPEN_FAILED` 在这个模块里的意思是"管理器在、数据源连不上"，
 *      两个码混起来，排障第一步就会走到"换个包"上去）；
 *   2. `last_error()` 是一句**人话**，而且**说得出它试过什么**（带上
 *      `UVCPP_DB_ODBC_LIBRARY` 那个值和解析出的路径）—— 现场第一步就是看这句话；
 *   3. **同一个进程里别的后端照常可用**（下面真跑一遍 SQLite 的建表/插入/查询/
 *      读回）—— "少一份 ODBC 动态库"不该波及整份库，这正是"一份二进制带全部功能、
 *      要 ODBC 的人只多带一份动态库"那句话的后半句。
 *
 * ## 怎么把"没装管理器"确定性地做出来
 *
 * 不去真的卸载 unixODBC，而是把 `UVCPP_DB_ODBC_LIBRARY` 指到一个**不存在的路径**上。
 * 这不是取巧，是这个旋钮写在文档里的用途（`src/db/uvcpp_db_runtime_load.h`：指到
 * 不存在的路径即可确定性地复现"这台机器没装客户端库"，所以测试不依赖机器上装没装
 * 东西）；它同时还是"显式指定就该是显式的"那条约定的必然结果 —— 环境变量一设，
 * 候选名一个都不再试。
 *
 * **必须在自己的进程里跑**，所以这是独立目标、而不是并进 `db_odbc_func.cpp`：
 * 符号加载走的是 magic static，进程内一旦成功加载过 ODBC 管理器，之后再设环境变量
 * 也不生效（那是"句柄永不释放"的直接后果，理由在 `uvcpp_db_runtime_load.h` 里）。
 * 换句话说：**这条用例只有在进程开头的第一次 ODBC 接触之前设好环境变量才有意义**，
 * 而"独立目标 + ctest 的 ENVIRONMENT 属性"恰好就是唯一能保证这件事的办法。
 *
 * ## 前提由 ctest 给（`tests/functional/CMakeLists.txt` 里那条 ENVIRONMENT）
 *
 * 这一条的前提取自环境变量，而那份环境变量是**我们自己**在 CMake 里设的 —— 机器上
 * 装了什么、没装什么，都不影响它（本机装没装 unixODBC 都跑得动）。所以：
 *
 *   * 没设 `UVCPP_DB_ODBC_LIBRARY`，或它指到一个**真实存在**的文件 → **红**（退出码
 *     1），不是跳过。这不是"未判定"，是"这条用例没按它的前提跑"。与 mysql/pgsql 那
 *     两条"CI 上服务端都起了还跳过就必须红"同一条规矩，只是这里更强：前提完全在
 *     我们手里，没有任何"机器没配好"可以当借口。
 *   * 想手动跑：
 *     `UVCPP_DB_ODBC_LIBRARY=/nonexistent/libodbc.so.2 ./test_db_odbc_nodrv_func`
 *     （Windows 上用 `Z:/nonexistent/odbc32.dll` 这种肯定不存在的路径）。
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include <db/uvcpp_db.h>

#include "db_suite.h"

namespace {

// 与 `db_odbc_func.cpp` / `db_sqlite_func.cpp` 同一个形状：断言类型全在
// `uvcpp_db_test` 里，下面这些辅助函数跟着它一起在匿名命名空间里，`main()`
// 再靠这个 using 指令从全局找得到 `harness` / `uvcpp_db_client`。
using namespace uvcpp_db_test;

/// 路径存在吗。用 `std::fopen` 而不是 `<filesystem>`：本仓的 C++ 标准是 11
/// （根 `CMakeLists.txt` 的 `CMAKE_CXX_STANDARD`），而这里要判的是"这个文件在不在"，
/// 一个 `fopen` 足够，不值得为它抬标准或者挂一份 `-lstdc++fs`。
bool path_exists(const std::string& p) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

/// 前提不成立时的统一出口：**红**，并把"该怎么跑"说出来。
int premise_broken(const std::string& why) {
  std::cerr << "这一条用例的前提没满足：" << why << "\n"
            << "它必须在 UVCPP_DB_ODBC_LIBRARY 指向一个**不存在**的路径时跑"
               "（ctest 那边由 tests/functional/CMakeLists.txt 的 ENVIRONMENT 属性"
               "设好）。手动跑的话：\n"
            << "  UVCPP_DB_ODBC_LIBRARY=/nonexistent/libodbc.so.2 "
               "./test_db_odbc_nodrv_func\n"
            << "（Windows 上用 Z:/nonexistent/odbc32.dll 这种肯定不存在的路径）"
            << std::endl;
  return 1;
}

}  // namespace

int main() {
  const char* override_path = std::getenv("UVCPP_DB_ODBC_LIBRARY");
  if (override_path == nullptr || *override_path == '\0') {
    return premise_broken("UVCPP_DB_ODBC_LIBRARY 没设");
  }
  if (path_exists(override_path)) {
    return premise_broken(std::string("UVCPP_DB_ODBC_LIBRARY 指的 ") + override_path +
                          " 真的存在 —— 那这一条就变成了「管理器在、连不上数据源」，"
                          "测的不是同一件事");
  }

  // 先确认这份构建真的带了 ODBC 后端：CMake 那边已经把"没编进来"的用例摘掉了
  // （`NOT UVCPP_ENABLE_DB_ODBC` 那条 FILTER），所以走到这里还说没有就是配置错了
  // —— 报错，不跳过。下面那条"管理器不在 ⇒ NO_DRIVER"的判据只有在后端**编进来了**
  // 的时候才有意义：没编进来时 `open("odbc://…")` 也会回 NO_DRIVER，但那是
  // "没有这个后端"，与"后端在、管理器不在"完全是两件事。
  if (uvcpp::uvcpp_db_drivers().find("odbc") == std::string::npos) {
    std::cerr << "这份构建没有 ODBC 后端（已编入：" << uvcpp::uvcpp_db_drivers()
              << "）—— 这一条用例不该被注册" << std::endl;
    return 1;
  }

  harness h;
  int failures = 0;

  std::cout << "-- ODBC 后端在、驱动管理器不在 --" << std::endl;

  uvcpp_db_client odbc;
  h.db = &odbc;
  const uvcpp_db_status st = odbc.open("odbc://u:p@127.0.0.1:1/uvcpp_nodrv");
  h.eq_status(st, uvcpp_db_status::NO_DRIVER,
              "open() 回 NO_DRIVER（不是 OPEN_FAILED、更不是崩）");
  h.check(!odbc.is_open(), "打不开之后 is_open() 必须是 false");

  const std::string err = odbc.last_error();
  h.check(err.find("加载 ODBC 驱动管理器失败") != std::string::npos,
          "last_error 说得清是**管理器**没加载上（而不是「连接失败」）");
  h.check(err.find("UVCPP_DB_ODBC_LIBRARY=") != std::string::npos,
          "last_error 里带上了试过的那个环境变量名");
  h.check(err.find(override_path) != std::string::npos,
          std::string("last_error 里带上了解析出的路径本身（") + override_path + "）");
  h.check(err.find("只用别的后端时不用管这一条") != std::string::npos,
          "last_error 里说清了「只是不用 ODBC 的话不受影响」");
  if (h.failed != 0) {
    // 错误信息本身不对时，别接着跑 SQLite 那一段 —— 那会把"报错写得不清楚"淹没在
    // 一串无关的通过里。
    std::cerr << "last_error 原文：" << err << std::endl;
  }

  // 同一进程里再开一次 ODBC：加载失败这件事被 magic static 记着，第二次必须还是
  // NO_DRIVER —— 而不是"第一次失败、第二次换条路"。这条看着多余，但它正是"加载
  // 只发生一次、且失败可重复"那两条约定的见证：谁哪天把 `manager_api_state()` 改成
  // 每次重试（或者反过来，第一次失败后置成不可恢复的硬错），这里会先响。
  uvcpp_db_client odbc_again;
  h.db = &odbc_again;
  h.eq_status(odbc_again.open("odbc://u:p@127.0.0.1:1/uvcpp_nodrv"),
              uvcpp_db_status::NO_DRIVER, "同一进程里第二次 open 同样回 NO_DRIVER");
  h.check(!odbc_again.is_open(), "第二次也没打开");

  // 其他后端照常 —— 这才是"一份二进制带全部功能、要 ODBC 的只多带一份动态库"的后半句。
  // 用 `sqlite::memory:`（`doc/db-guide.md` 里记的写法）：不落文件、不用临时目录，
  // 也就没有"上一轮跑剩下的库文件"这种偶发。
  std::cout << "-- 同一进程里 SQLite 照常可用 --" << std::endl;
  uvcpp_db_client lite;
  h.db = &lite;
  h.ok(lite.open("sqlite::memory:"), "SQLite 后端照常打开");

  h.ok(lite.execute("CREATE TABLE uvcpp_nodrv (k INTEGER PRIMARY KEY, v TEXT)"),
       "建表");
  h.ok(lite.execute("INSERT INTO uvcpp_nodrv (k, v) VALUES (?, ?)",
                    {int64_t(7), std::string("仍然可用")}),
       "插入一行（带参数）");

  uvcpp_db_table t;
  h.ok(lite.query("SELECT k, v FROM uvcpp_nodrv", static_cast<uvcpp_db_table*>(&t)),
       "查回来");
  h.eq_int(static_cast<int64_t>(t.row_count()), 1, "一行");
  if (t.row_count() == 1) {
    h.eq_int(t.at(0, 0).to_int64(), 7, "k 对得上");
    h.eq_text(t.at(0, 1).to_text(), "仍然可用", "v 逐字节对得上（UTF-8 中文）");
  }
  lite.close();
  h.check(!lite.is_open(), "close() 之后 is_open() 是 false");

  std::cout << "-- 结果：" << h.passed << " 通过 / " << h.failed << " 失败 --"
            << std::endl;
  failures += h.failed;
  std::cout << (failures == 0 ? "db_odbc_nodrv_func: 全部通过"
                              : "db_odbc_nodrv_func: 有失败")
            << std::endl;
  return failures == 0 ? 0 : 1;
}
