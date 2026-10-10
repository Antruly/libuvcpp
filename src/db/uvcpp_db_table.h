/**
 * @file src/db/uvcpp_db_table.h
 * @brief 结果集：`uvcpp_db_row` + `uvcpp_db_table`，以及 JSON / CSV 导出。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 表操作风格沿用 cyq.data 那一路（`table.rows[i]["列名"].to_int()`），但把
 * 上层那三层指针套娃（`VDataTable` → `VDataRow` → `VDataCell` → `SqlValue*`）
 * 收成两层**值**：表和行都是可拷贝的值类型，行里直接放 `uvcpp_db_value`。
 *
 * 旧结构有三个坑，这一版是冲着它们去的：
 *
 *  1. `VDataCell` 里存的是 `new SqlValue` 出来的裸指针，`VDataRow` 拷贝构造
 *     用的是它、`VDataTable` 的 `Columns[i].Cells` 里还存着**同一批 cell 的副本**
 *     —— 同一格数据在表里存在两份，改一份另一份不动。这里一行就是一份值。
 *  2. `VDataRow` 里存 `VDataTable*`，表一拷贝（`VDataTable::Clone()` 就在拷）
 *     行里的指针就指回原表。这里行只带一份**列名快照**（`shared_ptr`，与表共享、
 *     只读），行可以自由拷贝、脱离表存活。
 *  3. 越界/没有这一列时旧代码 `throw`（`GetColumn`）或返回悬空引用。这里统一
 *     返回一个**静态的 NULL 值**，配套 `has()` / `column_index()` 判断。
 *     数据库读出来的行里出现意外列名不是异常情形，不该逼调用方写 try。
 */

#pragma once
#ifndef SRC_DB_UVCPP_DB_TABLE_H
#define SRC_DB_UVCPP_DB_TABLE_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <db/uvcpp_db_value.h>

namespace uvcpp {

/// 结果集里的一行。值语义，可拷贝、可脱离表存活。
class UVCPP_API uvcpp_db_row {
 public:
  uvcpp_db_row() = default;

  size_t size() const { return cells_.size(); }
  bool empty() const { return cells_.empty(); }

  /// 有没有这一列。`operator[]` 遇到不存在的列名返回 NULL 值，想区分
  /// 「值是 NULL」和「压根没这列」时才需要它。
  bool has(const std::string& column) const;

  /// 按下标 / 按列名取值。越界或没有这一列时返回静态 NULL 值（**不是**
  /// 悬空引用，可以安全地绑到 `const&` 上）。按列名查找是线性扫描。
  const uvcpp_db_value& operator[](size_t index) const;
  const uvcpp_db_value& operator[](const std::string& column) const;

  const std::vector<uvcpp_db_value>& cells() const { return cells_; }

  /// 构造侧：追加一个单元格（驱动填结果集时用）。
  void push(uvcpp_db_value value) { cells_.push_back(std::move(value)); }

 private:
  friend class uvcpp_db_table;

  /// 列名快照。与表共享同一份，只读；构造时由表下发。
  std::shared_ptr<const std::vector<std::string>> columns_;
  std::vector<uvcpp_db_value> cells_;
};

/// 一次查询的结果集。驱动填它，使用者读它。
class UVCPP_API uvcpp_db_table {
 public:
  uvcpp_db_table() = default;

  const std::vector<std::string>& columns() const { return columns_; }
  size_t column_count() const { return columns_.size(); }
  size_t row_count() const { return rows_.size(); }
  bool empty() const { return rows_.empty(); }

  const std::vector<uvcpp_db_row>& rows() const { return rows_; }

  /// 列名 → 下标；没有这一列时返回 `std::string::npos`。
  size_t column_index(const std::string& name) const;

  /// 按 (行, 列) 取值；越界返回静态 NULL 值。
  const uvcpp_db_value& at(size_t row, size_t column) const;
  const uvcpp_db_value& at(size_t row, const std::string& column) const;
  /// 单行查询的糖：`table.value("count")`。空表返回静态 NULL 值。
  const uvcpp_db_value& value(const std::string& column) const;

  /// INSERT/UPDATE/DELETE 的影响行数；SELECT 时等于结果行数。
  int64_t affected_rows() const { return affected_rows_; }
  void set_affected_rows(int64_t n) { affected_rows_ = n; }

  /// 结果集来自哪张表（后端能报就报，报不出是空串）。
  const std::string& table_name() const { return table_name_; }
  void set_table_name(std::string name) { table_name_ = std::move(name); }

  // ---- 构造侧（驱动用）----

  /// 设定列。会清空已有的行。
  void reset_columns(std::vector<std::string> names);
  /// 追加一行，并把当前列名快照发给它。
  uvcpp_db_row& add_row();
  /// 清空列与行，保留名字等元信息。
  void clear();

  // ---- 导出 ----

  /// JSON。数值/布尔按 JSON 的数值/布尔写，NULL 写 `null`，文本按字符串
  /// 转义（引号、反斜杠、控制字符），blob 写 `"[blob N bytes]"`（**有损**，
  /// 要原始字节用 `uvcpp_db_value::bytes()`）。
  std::string to_json() const;

  /// CSV（RFC4180 形状）：字段含逗号/引号/换行时加引号，内部引号翻倍。
  /// NULL 写 `NULL`（不带引号），空串写 `""` —— 两者在文本里必须能分开。
  std::string to_csv() const;

 private:
  std::vector<std::string> columns_;
  std::shared_ptr<const std::vector<std::string>> columns_shared_;
  std::vector<uvcpp_db_row> rows_;
  int64_t affected_rows_ = 0;
  std::string table_name_;
};

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
#endif  // SRC_DB_UVCPP_DB_TABLE_H
