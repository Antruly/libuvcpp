/**
 * @file tests/functional/db_mongodb_nodrv_func.cpp
 * @brief 数据库模块 · MongoDB 后端：**客户端库不在**时的行为。
 *
 * ## 这条用例在验什么
 *
 * MongoDB 的客户端库（`libmongoc-1.0` + `libbson-1.0`）**不随包分发**，缺库是
 * 预期之内的部署形态，不是错误。这条用例钉住这个形态下的四件事：
 *
 *   1. `open()` 回 **`NO_DRIVER`**，不是 `OPEN_FAILED`、更不是崩；
 *   2. `last_error()` 说得出**它试过什么**（试过的库名，或者
 *      `UVCPP_DB_MONGOC_LIBRARY` 那个显式路径）；
 *   3. 同一进程里**别的后端照常可用**（下面真跑一遍 SQLite）——
 *      "不用 MongoDB" 不该波及整份库；
 *   4. 异步入口在同样的前提下也给出可读的结果，而且回调**恰好一次**。
 *
 * ## 两种构建形态都要跑
 *
 * 这条用例**在 `UVCPP_DB_MONGODB_ENABLE=0` 的桩构建里也注册**（CMake 那边只摘
 * `db_mongodb_func.cpp`，见 `tests/functional/CMakeLists.txt`）。两种形态下
 * `open()` 都回 `NO_DRIVER`，区别只在 `last_error()` 说的是哪一件事：
 *
 *   * 桩构建：`backend_available() == false`，"这份构建没有编 MongoDB 后端"；
 *   * 真后端 + 库不在：`backend_available() == true`，"加载 MongoDB 客户端库失败，
 *     试过这些名字……"。
 *
 * 把两种形态合成一条用例，是因为**调用方看到的是同一个返回码**；分不清的只是
 * 下一步该做什么（装库 vs 换构建），而那正是 `last_error()` 与
 * `backend_available()` 的分工。
 *
 * ## 前提由 ctest 给
 *
 * 真后端形态下，`UVCPP_DB_MONGOC_LIBRARY` / `UVCPP_DB_BSON_LIBRARY` 由
 * `tests/functional/CMakeLists.txt` 的 `ENVIRONMENT` 指到**不存在的路径**。
 * 没设、或指到一个真实存在的文件 → **红**，不是跳过：前提完全在我们手里，
 * 没有"机器没配好"可以当借口（与 `db_odbc_nodrv_func` 同一条规矩）。
 *
 * 手动跑：
 *   * 真后端构建：
 *     `UVCPP_DB_MONGOC_LIBRARY=/nonexistent/libmongoc-1.0.so.0 \
 *      UVCPP_DB_BSON_LIBRARY=/nonexistent/libbson-1.0.so.0 \
 *      ./test_db_mongodb_nodrv_func`
 *   * 桩构建（没有 mongoc.h 的那份）：直接跑即可。
 */

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_mongo.h>
#include <handle/uvcpp_loop.h>

namespace {

using uvcpp::uvcpp_db_ok;
using uvcpp::uvcpp_db_status;

/// 路径存在吗（与 ODBC 那条用例同一个实现：C++11，不挂 `-lstdc++fs`）。
bool path_exists(const std::string& p) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ok]   " << what << std::endl;
  } else {
    std::cout << "  [FAIL] " << what << std::endl;
    ++failures;
  }
}

int premise_broken(const std::string& why) {
  std::cerr << "这一条用例的前提没满足：" << why << "\n"
            << "真后端构建必须在 UVCPP_DB_MONGOC_LIBRARY / "
               "UVCPP_DB_BSON_LIBRARY 都指向**不存在**的路径时跑"
               "（ctest 那边由 tests/functional/CMakeLists.txt 的 ENVIRONMENT "
               "属性设好）。手动跑的话：\n"
            << "  UVCPP_DB_MONGOC_LIBRARY=/nonexistent/libmongoc-1.0.so.0 \\\n"
            << "  UVCPP_DB_BSON_LIBRARY=/nonexistent/libbson-1.0.so.0 \\\n"
            << "  ./test_db_mongodb_nodrv_func\n"
            << std::endl;
  return 1;
}

/// 真后端形态下的前提：两个覆盖变量都在、且都指向不存在的文件。
int check_override_premise() {
  const char* mongoc = std::getenv("UVCPP_DB_MONGOC_LIBRARY");
  const char* bson = std::getenv("UVCPP_DB_BSON_LIBRARY");
  if (mongoc == nullptr || *mongoc == '\0') {
    return premise_broken("UVCPP_DB_MONGOC_LIBRARY 没设");
  }
  if (bson == nullptr || *bson == '\0') {
    return premise_broken("UVCPP_DB_BSON_LIBRARY 没设");
  }
  if (path_exists(mongoc)) {
    return premise_broken(std::string("UVCPP_DB_MONGOC_LIBRARY 指的 ") + mongoc +
                          " 真的存在 —— 测的就不是「库不在」了");
  }
  if (path_exists(bson)) {
    return premise_broken(std::string("UVCPP_DB_BSON_LIBRARY 指的 ") + bson +
                          " 真的存在 —— 测的就不是「库不在」了");
  }
  return 0;
}

}  // namespace

int main() {
  const bool backend = uvcpp::uvcpp_mongo_client::backend_available();

  std::cout << "-- MongoDB 后端" << (backend ? "在、客户端库不在" : "没编进这份构建")
            << " --" << std::endl;

  if (backend) {
    const int rc = check_override_premise();
    if (rc != 0) return rc;
  }

  // ---- 同步：open() 必须是一个可读的 NO_DRIVER ----
  uvcpp::uvcpp_mongo_client mongo;
  check(mongo.open("mongodb://127.0.0.1:1/uvcpp_nodrv?serverSelectionTimeoutMS=500") ==
            uvcpp_db_status::NO_DRIVER,
        "open() 回 NO_DRIVER（不是 OPEN_FAILED、更不是崩）");
  check(!mongo.is_open(), "打不开之后 is_open() 必须是 false");
  check(mongo.uri().empty(), "打不开时 uri() 是空串（它只记真正打开的那一次）");

  const std::string err = mongo.last_error();
  if (backend) {
    // 真后端：错误串来自加载器，必须带上**试过的那个路径**与它的环境变量名。
    check(err.find("加载 MongoDB 客户端库失败") != std::string::npos,
          "last_error 说得清是**客户端库**没加载上（而不是「连接失败」）");
    check(err.find("UVCPP_DB_MONGOC_LIBRARY=") != std::string::npos,
          "last_error 里带上了 UVCPP_DB_MONGOC_LIBRARY 这个变量名");
    check(err.find(std::getenv("UVCPP_DB_MONGOC_LIBRARY")) != std::string::npos,
          "last_error 里带上了那个路径本身");
  } else {
    // 桩：说的是另一件事 —— 这份构建没编后端。两句话不能混。
    check(err.find("没有编 MongoDB 后端") != std::string::npos,
          "last_error 说得清是**这份构建**没编 MongoDB 后端");
    check(err.find("不随包分发") != std::string::npos ||
              err.find("mongo-c-driver") != std::string::npos,
          "last_error 里给出了出路（装 mongo-c-driver 或换构建）");
  }
  if (failures != 0) {
    // 错误信息本身不对时，别再往下跑 —— 那会把"报错写得不清楚"淹没在
    // 一串无关的通过里。
    std::cerr << "last_error 原文：" << err << std::endl;
  }

  // 同一进程里第二次：加载只发生一次，结果必须可重复。
  uvcpp::uvcpp_mongo_client mongo_again;
  check(mongo_again.open("mongodb://127.0.0.1:1/uvcpp_nodrv") ==
            uvcpp_db_status::NO_DRIVER,
        "同一进程里第二次 open() 同样回 NO_DRIVER");

  // 写/读操作在没有连接时也必须是同一个可读的码，而不是空指针。
  //
  // 真后端形态下 `open()` 已经失败过，所以对象的状态是"没有连接"——
  // `NOT_CONNECTED`，与 `uvcpp_db_client` 在同样情形下的返回值一致。桩形态下
  // 连"后端"这个概念都没有，回的是 `NO_DRIVER`。两个码分开，是因为调用方要
  // 做的事不同：前者先看 `open()` 为什么失败，后者只需换构建或装库。
  const uvcpp_db_status op_st = backend ? uvcpp_db_status::NOT_CONNECTED
                                        : uvcpp_db_status::NO_DRIVER;
  std::vector<std::string> docs;
  check(mongo.insert_one("uvcpp_nodrv", "c", "{\"k\": 1}") == op_st,
        backend ? "未连接时 insert_one() 回 NOT_CONNECTED"
                : "桩构建里 insert_one() 回 NO_DRIVER");
  check(mongo.find("uvcpp_nodrv", "c", "", "", &docs) == op_st,
        backend ? "未连接时 find() 回 NOT_CONNECTED"
                : "桩构建里 find() 回 NO_DRIVER");
  check(docs.empty(), "find() 失败时 out 是空的");

  // ---- 异步：回调恰好一次，结果同样可读 ----
  {
    uvcpp::uvcpp_loop loop;
    int calls = 0;
    uvcpp_db_status got = uvcpp_db_status::OK;
    std::string message;
    const int rc = mongo.ping_async(&loop, [&](uvcpp_db_status st,
                                               const std::string& msg) {
      ++calls;
      got = st;
      message = msg;
    });
    check(rc == 0, "ping_async() 返回 0（已投递）");
    loop.run(UV_RUN_DEFAULT);
    check(calls == 1, "完成回调恰好被调一次");
    // 桩构建：操作本身就知道没有后端 ⇒ NO_DRIVER。
    // 真后端但库不在：对象没打开 ⇒ NOT_CONNECTED（与同步 ping() 一致）。
    check(got == (backend ? uvcpp_db_status::NOT_CONNECTED
                          : uvcpp_db_status::NO_DRIVER),
          backend ? "真后端形态下异步 ping 回 NOT_CONNECTED（没连接）"
                  : "桩形态下异步 ping 回 NO_DRIVER");
    check(!message.empty(), "失败消息不是空的");
  }

  // ---- 别的后端照常可用 ----
  //
  // 这才是"一份二进制带全部功能、要 MongoDB 的人自己多带一份库"的后半句。
  // 这份构建如果恰好没有 SQLite（理论上可能，实际上发布腿一定有），就明说
  // 没测这一半 —— 但不把它算成失败，也不假装测过。
  if (uvcpp::uvcpp_db_drivers().find("sqlite") != std::string::npos) {
    std::cout << "-- 同一进程里 SQLite 照常可用 --" << std::endl;
    uvcpp::uvcpp_db_client lite;
    check(uvcpp_db_ok(lite.open("sqlite::memory:")), "SQLite 后端照常打开");
    check(uvcpp_db_ok(lite.execute(
              "CREATE TABLE uvcpp_mongo_nodrv (k INTEGER PRIMARY KEY, v TEXT)")),
          "建表");
    check(uvcpp_db_ok(lite.execute(
              "INSERT INTO uvcpp_mongo_nodrv (k, v) VALUES (?, ?)",
              {int64_t(7), std::string("仍然可用")})),
          "插入一行（带参数）");
    uvcpp::uvcpp_db_table t;
    check(uvcpp_db_ok(lite.query("SELECT k, v FROM uvcpp_mongo_nodrv", &t)),
          "查回来");
    check(t.row_count() == 1, "一行");
    if (t.row_count() == 1) {
      check(t.at(0, 0).to_int64() == 7, "k 对得上");
      check(t.at(0, 1).to_text() == "仍然可用", "v 逐字节对得上（UTF-8 中文）");
    }
    lite.close();
    check(!lite.is_open(), "close() 之后 is_open() 是 false");
  } else {
    std::cout << "-- 这份构建没有 SQLite 后端，跳过「别的后端照常」那一段 --"
              << std::endl;
  }

  std::cout << "-- 结果：" << (failures == 0 ? "全部通过" : "有失败") << "（"
            << failures << " 个失败）--" << std::endl;
  return failures == 0 ? 0 : 1;
}
