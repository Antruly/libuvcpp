/**
 * @file tests/functional/db_mysql_func.cpp
 * @brief 数据库模块 · MySQL 后端：跑共用套件（`db_suite.h`）。
 *
 * ## 这一条要一个**真的 MySQL 服务端**
 *
 * 连接串从环境变量 `UVCPP_DB_TEST_MYSQL_URL` 取，例：
 *
 *     UVCPP_DB_TEST_MYSQL_URL='mysql://root:secret@127.0.0.1:3306/uvcpp_test'
 *
 * 没给 URL 时：
 *
 *   * 没设 `UVCPP_DB_TEST_REQUIRE` → 打印一行说明、**退出码 3**。ctest 那边用
 *     `SKIP_RETURN_CODE 3` 把它报成 `***Skipped`，与 `Passed` 是两个词。
 *   * 设了 `UVCPP_DB_TEST_REQUIRE=1` → **退出码 1（红）**。CI 上库都起了还
 *     跳过，说明是配置写错了，这种时候"绿"是最坏的结果。
 *
 * 退出码 3 是照本仓 gate 脚本的规矩来的：**「未判定」不是「通过」**。
 *
 * ## 这个库会被**改写**
 *
 * 套件里有 `DROP TABLE` / `CREATE TABLE`，所以 URL 必须指向一个**一次性的**
 * 库。用例自己的表名都是 `uvcpp_db_*` 前缀，但别拿生产库试。
 *
 * ## 为什么文件名里是 `db_mysql`
 *
 * `tests/functional/CMakeLists.txt` 按文件名摘用例：含 `db` 的需要 db 模块、
 * 含 `db_mysql` 的需要 MySQL 后端。后端没编进来时这个文件**根本不会被编译**
 * —— 编了的话它会去调一个不存在的后端，而"没编进来"应该表现为"用例不存在"，
 * 不是"用例失败"。
 */

#include <cstdlib>
#include <iostream>
#include <string>

#include <db/uvcpp_db.h>

#include "db_suite.h"

namespace {

using namespace uvcpp_db_test;

class mysql_dialect : public dialect {
 public:
  const char* name() const { return "mysql"; }

  std::string ph(size_t) const { return "?"; }

  std::string serial_pk_type() const { return "BIGINT AUTO_INCREMENT PRIMARY KEY"; }

  std::string create_types_table(const std::string& t) const {
    // `flag_val TINYINT(1)`：MySQL 的 `BOOL`/`BOOLEAN` 就是它的同义词，
    // 而本库把它读成**整数**（`MYSQL_TYPE_TINY`）—— 所以共用套件判的是
    // `to_bool()` 的值，不是类型标签。三家在这一点上统一不了：SQLite 连
    // 声明的地方都没有布尔，见 `doc/db-guide.md` 里那张对照表。
    //
    // `e_val` 特意带 `NOT NULL DEFAULT ''`：把空串折成 NULL 的实现会在这里
    // 撞上非空约束，失败得**响亮**。
    return "CREATE TABLE " + t +
           " (id BIGINT AUTO_INCREMENT PRIMARY KEY,"
           " i_val INT, d_val DOUBLE, s_val TEXT, b_val BLOB,"
           " n_val TEXT, e_val VARCHAR(64) NOT NULL DEFAULT '',"
           " flag_val TINYINT(1),"
           " dt_val DATETIME, dd_val DATE, tt_val TIME, " +
           decimal_column_decl() + ")";
  }

  std::string create_serial_table(const std::string& t) const {
    return "CREATE TABLE " + t + " (id " + serial_pk_type() + ", name VARCHAR(64))";
  }

  /// 原生 `DECIMAL`。本库把它读成 **double** —— 金额在这一步会丢精度，
  /// 所以 `doc/db-guide.md` 的建议是"要精确就用 TEXT 存"。这条断言就是那句话
  /// 的见证：类型确实变成 DOUBLE 了，不是"我们以为会"。
  std::string decimal_column_decl() const { return "dec_val DECIMAL(18,2)"; }

  bool decimal_is_native() const { return true; }
};

}  // namespace

int main() {
  const char* url = std::getenv("UVCPP_DB_TEST_MYSQL_URL");
  const char* require = std::getenv("UVCPP_DB_TEST_REQUIRE");

  if (url == nullptr || *url == '\0') {
    if (require != nullptr && *require != '\0') {
      std::cerr << "UVCPP_DB_TEST_REQUIRE 已设，但 UVCPP_DB_TEST_MYSQL_URL 是空的"
                << " —— 这是失败，不是跳过（CI 上服务端应该已经起来了）"
                << std::endl;
      return 1;
    }
    std::cout << "SKIP: 没给 UVCPP_DB_TEST_MYSQL_URL，MySQL 后端这一条未判定"
              << std::endl;
    return 3;  // ctest 的 SKIP_RETURN_CODE
  }

  mysql_dialect d;

  // 先确认这个库真的带了 MySQL 后端。CMake 那边已经把"没编进来"的用例摘掉了，
  // 所以走到这里还说没有，就是配置错了 —— 报错，不跳过。
  if (uvcpp::uvcpp_db_drivers().find("mysql") == std::string::npos) {
    std::cerr << "这份构建没有 MySQL 后端（已编入：" << uvcpp::uvcpp_db_drivers()
              << "）—— 这一条用例不该被注册" << std::endl;
    return 1;
  }

  std::cout << "-- MySQL (" << url << ") --" << std::endl;
  return run_suite(url, d);
}
