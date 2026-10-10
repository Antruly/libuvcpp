/**
 * @file src/db/uvcpp_db_table.cpp
 * @brief `uvcpp_db_row` / `uvcpp_db_table` 的实现，含 JSON / CSV 导出。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "db/uvcpp_db_table.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <cstdio>

namespace uvcpp {

namespace {

/// 越界、没有这一列、空表 —— 三种「没取到」共用的返回值。
/// 静态 + 不可变：可以安全地把它的引用交出去，调用方读到的永远是 NULL。
const uvcpp_db_value& null_value() {
  static const uvcpp_db_value kNull;
  return kNull;
}

void json_escape_into(std::string* out, const std::string& s) {
  out->push_back('"');
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\b': out->append("\\b"); break;
      case '\f': out->append("\\f"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out->append(buf);
        } else {
          // 0x80 以上按原字节直出：本库全线按 UTF-8 处理，不替调用方重编码。
          out->push_back(static_cast<char>(c));
        }
        break;
    }
  }
  out->push_back('"');
}

void csv_field_into(std::string* out, const std::string& s) {
  const bool need_quote = s.empty() || s.find_first_of(",\"\r\n") != std::string::npos;
  if (!need_quote) {
    out->append(s);
    return;
  }
  out->push_back('"');
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '"') out->push_back('"');
    out->push_back(s[i]);
  }
  out->push_back('"');
}

}  // namespace

// ---------------------------------------------------------------- row

bool uvcpp_db_row::has(const std::string& column) const {
  if (!columns_) return false;
  for (size_t i = 0; i < columns_->size(); ++i) {
    if ((*columns_)[i] == column) return true;
  }
  return false;
}

const uvcpp_db_value& uvcpp_db_row::operator[](size_t index) const {
  if (index >= cells_.size()) return null_value();
  return cells_[index];
}

const uvcpp_db_value& uvcpp_db_row::operator[](const std::string& column) const {
  if (!columns_) return null_value();
  for (size_t i = 0; i < columns_->size(); ++i) {
    if ((*columns_)[i] == column) {
      return i < cells_.size() ? cells_[i] : null_value();
    }
  }
  return null_value();
}

// -------------------------------------------------------------- table

size_t uvcpp_db_table::column_index(const std::string& name) const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i] == name) return i;
  }
  return std::string::npos;
}

const uvcpp_db_value& uvcpp_db_table::at(size_t row, size_t column) const {
  if (row >= rows_.size()) return null_value();
  return rows_[row][column];
}

const uvcpp_db_value& uvcpp_db_table::at(size_t row,
                                         const std::string& column) const {
  if (row >= rows_.size()) return null_value();
  return rows_[row][column];
}

const uvcpp_db_value& uvcpp_db_table::value(const std::string& column) const {
  if (rows_.empty()) return null_value();
  return rows_[0][column];
}

void uvcpp_db_table::reset_columns(std::vector<std::string> names) {
  columns_ = std::move(names);
  columns_shared_ = std::make_shared<const std::vector<std::string>>(columns_);
  rows_.clear();
}

uvcpp_db_row& uvcpp_db_table::add_row() {
  rows_.emplace_back();
  rows_.back().columns_ = columns_shared_;
  return rows_.back();
}

void uvcpp_db_table::clear() {
  rows_.clear();
  columns_.clear();
  columns_shared_.reset();
  affected_rows_ = 0;
  table_name_.clear();
}

std::string uvcpp_db_table::to_json() const {
  std::string out;
  out.reserve(256 + rows_.size() * columns_.size() * 16);
  out.append("{\n  \"table\": ");
  json_escape_into(&out, table_name_);
  out.append(",\n  \"columns\": [");
  for (size_t c = 0; c < columns_.size(); ++c) {
    if (c > 0) out.push_back(',');
    out.append("\n    ");
    json_escape_into(&out, columns_[c]);
  }
  out.append(columns_.empty() ? "],\n" : "\n  ],\n");
  out.append("  \"rows\": [");
  for (size_t r = 0; r < rows_.size(); ++r) {
    if (r > 0) out.push_back(',');
    out.append("\n    {");
    const uvcpp_db_row& row = rows_[r];
    for (size_t c = 0; c < columns_.size(); ++c) {
      if (c > 0) out.push_back(',');
      out.append("\n      ");
      json_escape_into(&out, columns_[c]);
      out.append(": ");
      const uvcpp_db_value& v = row[c];
      switch (v.type()) {
        case uvcpp_db_type::NIL:
          out.append("null");
          break;
        case uvcpp_db_type::BOOL:
          out.append(v.to_bool() ? "true" : "false");
          break;
        case uvcpp_db_type::INT64:
        case uvcpp_db_type::DOUBLE:
          out.append(v.to_text());
          break;
        case uvcpp_db_type::UINT64:
          out.append(v.to_text());
          break;
        case uvcpp_db_type::BLOB: {
          char buf[48];
          std::snprintf(buf, sizeof(buf), "\"[blob %lu bytes]\"",
                        static_cast<unsigned long>(v.size()));
          out.append(buf);
          break;
        }
        default:
          json_escape_into(&out, v.to_text());
          break;
      }
    }
    out.append(columns_.empty() ? "}" : "\n    }");
  }
  out.append(rows_.empty() ? "]\n}" : "\n  ]\n}");
  return out;
}

std::string uvcpp_db_table::to_csv() const {
  std::string out;
  for (size_t c = 0; c < columns_.size(); ++c) {
    if (c > 0) out.push_back(',');
    csv_field_into(&out, columns_[c]);
  }
  out.push_back('\n');
  for (size_t r = 0; r < rows_.size(); ++r) {
    for (size_t c = 0; c < columns_.size(); ++c) {
      if (c > 0) out.push_back(',');
      const uvcpp_db_value& v = rows_[r][c];
      if (v.is_null()) {
        out.append("NULL");
      } else {
        csv_field_into(&out, v.to_text());
      }
    }
    out.push_back('\n');
  }
  return out;
}

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
