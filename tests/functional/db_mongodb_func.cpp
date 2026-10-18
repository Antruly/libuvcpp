/**
 * @file tests/functional/db_mongodb_func.cpp
 * @brief 数据库模块 · MongoDB 后端：对**真的 mongod** 跑一遍增删改查与命令。
 *
 * ## 这一条要一个真的 MongoDB 服务端
 *
 * 连接串从环境变量 `UVCPP_DB_TEST_MONGODB_URL` 取，例：
 *
 *     UVCPP_DB_TEST_MONGODB_URL='mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000'
 *
 * 没给 URL 时：
 *
 *   * 没设 `UVCPP_DB_TEST_REQUIRE` → 打印一行说明、**退出码 3**。ctest 那边用
 *     `SKIP_RETURN_CODE 3` 把它报成 `***Skipped`，与 `Passed` 是两个词。
 *   * 设了 `UVCPP_DB_TEST_REQUIRE=1` → **退出码 1（红）**。CI 上 mongod 都起了
 *     还跳过，说明是配置写错了。
 *
 * ## 它只碰自己那一个 collection
 *
 * 名字带进程号与时间戳，最后 `drop` 掉。不用固定的库名 —— 这条用例可能被别人
 * 指到一个共用的测试库上。
 *
 * ## 为什么文件名里是 `db_mongodb`
 *
 * `tests/functional/CMakeLists.txt` 按文件名摘用例：含 `db` 的需要 db 模块、
 * 含 `db_mongodb` 的需要 MongoDB 后端。桩构建里这个文件**根本不会被编译**
 * （`NOT UVCPP_ENABLE_DB_MONGODB` 那条 FILTER），因为那时一个操作也跑不了 ——
 * 而"没编进来"应该表现为"用例不存在"，不是"用例失败"。桩构建里仍然在跑的
 * 是 `db_mongodb_nodrv_func.cpp`。
 */

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include <db/uvcpp_mongo.h>
#include <handle/uvcpp_loop.h>

namespace {

using uvcpp::uvcpp_db_status;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  [ok]   " << what << std::endl;
  } else {
    std::cout << "  [FAIL] " << what << std::endl;
    ++failures;
  }
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

const char* const kDb = "uvcpp_test";

}  // namespace

int main() {
  const char* url = std::getenv("UVCPP_DB_TEST_MONGODB_URL");
  const char* require = std::getenv("UVCPP_DB_TEST_REQUIRE");

  if (url == nullptr || *url == '\0') {
    if (require != nullptr && *require != '\0' && std::string(require) != "0") {
      std::cerr << "UVCPP_DB_TEST_REQUIRE 已设，但 UVCPP_DB_TEST_MONGODB_URL 是空的"
                << std::endl;
      return 1;
    }
    std::cout << "未设置 UVCPP_DB_TEST_MONGODB_URL，跳过（退出码 3 = 未判定）"
              << std::endl;
    return 3;  // ctest 的 SKIP_RETURN_CODE
  }

  if (!uvcpp::uvcpp_mongo_client::backend_available()) {
    std::cerr << "这份构建没有 MongoDB 后端（缺 mongoc.h/bson.h）—— 这一条用例"
                 "不该被注册"
              << std::endl;
    return 1;
  }

  // 只碰自己的 collection。
  char suffix[64];
  std::time_t now = std::time(nullptr);
  std::snprintf(suffix, sizeof(suffix), "uvcpp_test_%ld_%d",
                static_cast<long>(now), static_cast<int>(sizeof(void*)));
  const std::string coll = suffix;

  std::cout << "-- 对真实 mongod 跑一遍（collection=" << coll << "）--"
            << std::endl;

  // ---- 连接串错误必须是 BAD_URL，不是 EXEC_FAILED ----
  {
    uvcpp::uvcpp_mongo_client bad;
    check(bad.open("这不是一个连接串") == uvcpp_db_status::BAD_URL,
          "坏的连接串回 BAD_URL");
    check(!bad.is_open(), "BAD_URL 之后 is_open() 是 false");
    check(!bad.last_error().empty(), "BAD_URL 有可读原因");
  }

  uvcpp::uvcpp_mongo_client m;
  const uvcpp_db_status open_st = m.open(url);
  if (open_st == uvcpp_db_status::NO_DRIVER) {
    // 头在、客户端库不在：这台机器本身跑不了这条用例。CI 上会设
    // UVCPP_DB_TEST_REQUIRE=1，那时这是**红**（配置错了，不是未判定）。
    if (require != nullptr && *require != '\0' && std::string(require) != "0") {
      std::cerr << "要求真跑，但 MongoDB 客户端库加载失败：" << m.last_error()
                << std::endl;
      return 1;
    }
    std::cout << "MongoDB 客户端库不在，跳过（退出码 3 = 未判定）："
              << m.last_error() << std::endl;
    return 3;
  }
  check(open_st == uvcpp_db_status::OK, "open() 成功（它真的 ping 过服务器）");
  if (open_st != uvcpp_db_status::OK) {
    std::cerr << "open() 失败：" << m.last_error() << std::endl;
    return 1;
  }
  check(m.is_open(), "is_open() 是 true");
  check(m.uri() == url, "uri() 就是传进去的那一条");
  check(m.ping() == uvcpp_db_status::OK, "ping() 成功");

  // ---- 插入 ----
  check(m.insert_one(kDb, coll, "{\"name\": \"ada\", \"age\": 36}") ==
            uvcpp_db_status::OK,
        "insert_one 插入 ada");
  check(m.insert_one(kDb, coll, "{\"name\": \"bob\", \"age\": 20}") ==
            uvcpp_db_status::OK,
        "insert_one 插入 bob");

  // 坏的 Extended JSON：必须是 EXEC_FAILED（BAD_URL 只留给连接串）。
  check(m.insert_one(kDb, coll, "{这不是 JSON") == uvcpp_db_status::EXEC_FAILED,
        "坏的文档回 EXEC_FAILED");
  check(!m.last_error().empty(), "EXEC_FAILED 有可读原因");

  // ---- 查询：filter + options（sort/limit）真的到了驱动手里 ----
  {
    std::vector<std::string> docs;
    check(m.find(kDb, coll, "{\"age\": {\"$gte\": 18}}",
                 "{\"limit\": 10, \"sort\": {\"age\": -1}}", &docs) ==
              uvcpp_db_status::OK,
          "find 成功");
    check(docs.size() == 2, "两条都查到了（年龄都 >= 18）");
    if (docs.size() == 2) {
      // sort 是降序：ada(36) 在前，bob(20) 在后。canonical Extended JSON 的
      // 形状是 `"name" : "ada"`（冒号两边有空格），所以只判子串。
      check(contains(docs[0], "ada"), "降序：第一条是 ada");
      check(contains(docs[1], "bob"), "降序：第二条是 bob");
      check(contains(docs[0], "36"), "文档里的数字原样回来");
    }
    docs.clear();
    check(m.find(kDb, coll, "{\"name\": \"nobody\"}", "", &docs) ==
              uvcpp_db_status::OK,
          "查不到不是失败");
    check(docs.empty(), "查不到时 out 为空");
  }

  // ---- 更新 ----
  check(m.update_one(kDb, coll, "{\"name\": \"ada\"}",
                     "{\"$set\": {\"age\": 37}}", false) ==
            uvcpp_db_status::OK,
        "update_one $set 成功");
  {
    std::vector<std::string> docs;
    check(m.find(kDb, coll, "{\"name\": \"ada\"}", "", &docs) ==
              uvcpp_db_status::OK,
          "改完再查成功");
    check(docs.size() == 1 && contains(docs[0], "37"), "age 变成 37");
  }

  // upsert：没有匹配就插入。
  check(m.update_one(kDb, coll, "{\"name\": \"grace\"}",
                     "{\"$set\": {\"age\": 45}}", true) ==
            uvcpp_db_status::OK,
        "upsert 成功");
  {
    std::vector<std::string> docs;
    check(m.find(kDb, coll, "{\"name\": \"grace\"}", "", &docs) ==
              uvcpp_db_status::OK,
          "upsert 之后查得到");
    check(docs.size() == 1, "upsert 插入了一条");
  }

  // ---- 删除 ----
  check(m.delete_one(kDb, coll, "{\"name\": \"grace\"}") == uvcpp_db_status::OK,
        "delete_one 成功");
  {
    std::vector<std::string> docs;
    check(m.find(kDb, coll, "{\"name\": \"grace\"}", "", &docs) ==
              uvcpp_db_status::OK,
          "删完再查成功");
    check(docs.empty(), "删掉的那条不在了");
  }

  // ---- 原始命令 ----
  {
    std::string reply;
    check(m.command(kDb, "{\"count\": \"" + coll + "\"}", &reply) ==
              uvcpp_db_status::OK,
          "command(count) 成功");
    check(contains(reply, "\"n\""), "应答里有 n");
    check(contains(reply, "2"), "n 等于 2（ada + bob）");
  }

  // ---- 异步：投递成功、回调在循环线程上恰好一次 ----
  {
    uvcpp::uvcpp_loop loop;
    int calls = 0;
    uvcpp_db_status got = uvcpp_db_status::MISUSE;
    std::string message;
    const int rc = m.insert_one_async(
        &loop, kDb, coll, "{\"name\": \"carol\", \"age\": 28}",
        [&](uvcpp_db_status st, const std::string& msg) {
          ++calls;
          got = st;
          message = msg;
        });
    check(rc == 0, "insert_one_async 返回 0（已投递）");
    loop.run(UV_RUN_DEFAULT);
    check(calls == 1, "异步插入回调恰好一次");
    check(got == uvcpp_db_status::OK, "异步插入成功");
    check(message.empty(), "成功时消息是空串");

    std::vector<std::string> docs;
    int find_calls = 0;
    got = uvcpp_db_status::MISUSE;
    const int rc2 = m.find_async(
        &loop, kDb, coll, "{\"name\": \"carol\"}", "",
        [&](uvcpp_db_status st, const std::vector<std::string>& out,
            const std::string& msg) {
          ++find_calls;
          got = st;
          docs = out;
          message = msg;
        });
    check(rc2 == 0, "find_async 返回 0（已投递）");
    loop.run(UV_RUN_DEFAULT);
    check(find_calls == 1, "异步查询回调恰好一次");
    check(got == uvcpp_db_status::OK, "异步查询成功");
    check(docs.size() == 1 && contains(docs[0], "carol"), "异步查询拿到 carol");
  }

  // 投递失败（loop 为空）时返回负码、回调不来 —— 与 db_async 同一条约定。
  {
    bool called = false;
    const int rc = m.ping_async(nullptr, [&](uvcpp_db_status, const std::string&) {
      called = true;
    });
    check(rc < 0, "ping_async(nullptr) 返回负的 libuv 码");
    check(!called, "没投出去时回调不会被调");
  }

  // ---- 收尾：删掉自己的 collection ----
  {
    std::string reply;
    const uvcpp_db_status st = m.command(
        kDb, "{\"drop\": \"" + coll + "\"}", &reply);
    check(st == uvcpp_db_status::OK, "drop 自己的 collection 成功");
  }

  m.close();
  check(!m.is_open(), "close() 之后 is_open() 是 false");

  std::cout << "-- 结果：" << (failures == 0 ? "全部通过" : "有失败") << "（"
            << failures << " 个失败）--" << std::endl;
  return failures == 0 ? 0 : 1;
}
