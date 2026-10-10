/**
 * @file tests/functional/db_pgsql_func.cpp
 * @brief 数据库模块 · PostgreSQL 后端：跑共用套件（`db_suite.h`）。
 *
 * ## 这一条要一个**真的 PostgreSQL 服务端**
 *
 * 连接串从环境变量 `UVCPP_DB_TEST_PGSQL_URL` 取，例：
 *
 *     UVCPP_DB_TEST_PGSQL_URL='postgresql://uvcpp:pw@127.0.0.1:5432/uvcpp_test'
 *
 * 缺 URL 的两种结局（退出码 3 = 未判定 / 设了 `UVCPP_DB_TEST_REQUIRE` 就是红）
 * 与 `db_mysql_func.cpp` 完全一样，理由写在那个文件的开头。
 *
 * ## PostgreSQL 在这条路上最容易错的两处（都在驱动里，断言在套件里）
 *
 *   1. **BYTEA 的文本形态。** 驱动 `open()` 里设了 `bytea_output = 'hex'`，
 *      于是 `SELECT b_val` 回来的**文本**是 `\x0001feff` 这十个 ASCII 字符。
 *      直接把它们当字节交出去，调用方会拿到长度 10 的"blob"而**不报任何错**。
 *      套件里那条"BLOB 逐字节往返（含 0x00 / 0xFF）"就是它的见证 —— 含 0x00
 *      这一枚尤其要紧，它是"当 C 字符串处理"会当场截断的字节。
 *   2. **参数占位符是 `$1` 不是 `?`。** 这是套件里唯一一个必须由方言提供的
 *      差异（`dialect::ph()`）。
 */

#include <cstdlib>
#include <iostream>
#include <string>

#include <db/uvcpp_db.h>

#include "db_suite.h"

namespace {

using namespace uvcpp_db_test;

class pgsql_dialect : public dialect {
 public:
  const char* name() const { return "postgres"; }

  /// `$1` 起数（**不是** `$0`）。
  std::string ph(size_t n) const {
    return "$" + std::to_string(n);
  }

  std::string serial_pk_type() const { return "BIGSERIAL PRIMARY KEY"; }

  std::string create_types_table(const std::string& t) const {
    // `flag_val BOOLEAN`：三家里面**只有 PG** 有真布尔类型，于是也只有它会
    // 给出 `uvcpp_db_type::BOOL`。共用套件因此一律判 `to_bool()` 的值 ——
    // 判 `type()` 的断言在 MySQL / SQLite 上必然红，而那红的不是被测代码。
    //
    // `e_val` 的 `NOT NULL DEFAULT ''` 与另外两家同理：把空串折成 NULL 的
    // 实现会撞在非空约束上，而不是悄悄存个 NULL。
    return "CREATE TABLE " + t +
           " (id BIGSERIAL PRIMARY KEY,"
           " i_val INTEGER, d_val DOUBLE PRECISION, s_val TEXT, b_val BYTEA,"
           " n_val TEXT, e_val TEXT NOT NULL DEFAULT '', flag_val BOOLEAN,"
           " dt_val TIMESTAMP, dd_val DATE, tt_val TIME, " +
           decimal_column_decl() + ")";
  }

  std::string create_serial_table(const std::string& t) const {
    return "CREATE TABLE " + t + " (id " + serial_pk_type() + ", name TEXT)";
  }

  /// 原生 `NUMERIC`。本库把它读成 **double**（`value_from_pg` 按 OID 1700
  /// 分派），所以带小数点的金额在这一步丢精度 —— 与 MySQL 的 DECIMAL 一样，
  /// 而 SQLite 的 TEXT 列反而是三家里面唯一能逐字节往返的。
  std::string decimal_column_decl() const { return "dec_val NUMERIC(18,2)"; }

  bool decimal_is_native() const { return true; }
};

}  // namespace

int main() {
  const char* url = std::getenv("UVCPP_DB_TEST_PGSQL_URL");
  const char* require = std::getenv("UVCPP_DB_TEST_REQUIRE");

  if (url == nullptr || *url == '\0') {
    if (require != nullptr && *require != '\0') {
      std::cerr << "UVCPP_DB_TEST_REQUIRE 已设，但 UVCPP_DB_TEST_PGSQL_URL 是空的"
                << " —— 这是失败，不是跳过（CI 上服务端应该已经起来了）"
                << std::endl;
      return 1;
    }
    std::cout << "SKIP: 没给 UVCPP_DB_TEST_PGSQL_URL，PostgreSQL 后端这一条未判定"
              << std::endl;
    return 3;  // ctest 的 SKIP_RETURN_CODE
  }

  pgsql_dialect d;

  if (uvcpp::uvcpp_db_drivers().find("postgres") == std::string::npos) {
    std::cerr << "这份构建没有 PostgreSQL 后端（已编入："
              << uvcpp::uvcpp_db_drivers() << "）—— 这一条用例不该被注册"
              << std::endl;
    return 1;
  }

  std::cout << "-- PostgreSQL (" << url << ") --" << std::endl;
  return run_suite(url, d);
}
