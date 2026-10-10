/**
 * @file src/db/uvcpp_db_sqlite.cpp
 * @brief SQLite 后端。
 * @author zhuweiye
 * @version 1.0.0
 *
 * SQLite 的整数是**有符号 64 位**：`uvcpp_db_value` 里的 `UINT64` 超出
 * `INT64_MAX` 时这里直接返回 `BIND_FAILED`，而不是截断成负数静默存进去。
 */
#include "db/uvcpp_db_driver.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_SQLITE_ENABLE

#include <sqlite3.h>

#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace uvcpp {
namespace db_detail {

namespace {

/// 逐级建目录。C++11 的树（`CMAKE_CXX_STANDARD 11`），所以没有
/// `std::filesystem::create_directories` 可用；这一段就是它的手写替身。
///
/// 为什么非要建：`sqlite3_open_v2` 在父目录不存在时只报 `SQLITE_CANTOPEN`
/// （"unable to open database file"），不说少的是哪一级 —— 而 `sqlite:///tmp/a/b/c.db`
/// 这条路上最常见的原因就是目录没建。
bool make_parent_dirs(const std::string& path, std::string* err) {
  size_t cut = path.find_last_of("/\\");
  if (cut == std::string::npos || cut == 0) return true;  // 无父目录 / 根
  const std::string parent = path.substr(0, cut);

  std::string prefix;
  size_t i = 0;
  // 保留 Windows 盘符（C:\...）与 POSIX 前导 '/'。
  if (parent.size() >= 2 && parent[1] == ':') {
    prefix = parent.substr(0, 2);
    i = 2;
  } else if (parent[0] == '/') {
    prefix = "/";
    i = 1;
  }
  while (i <= parent.size()) {
    size_t slash = parent.find_first_of("/\\", i);
    const std::string component =
        parent.substr(i, slash == std::string::npos ? std::string::npos
                                                    : slash - i);
    if (!component.empty()) {
      if (!prefix.empty() && prefix[prefix.size() - 1] != '/' &&
          prefix[prefix.size() - 1] != '\\') {
        prefix.push_back('/');
      }
      prefix.append(component);
#if defined(_WIN32)
      if (!CreateDirectoryA(prefix.c_str(), nullptr)) {
        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) {
          *err = "建目录失败（" + std::to_string(code) + "）：" + prefix;
          return false;
        }
      }
#else
      if (::mkdir(prefix.c_str(), 0755) != 0 && errno != EEXIST) {
        *err = "建目录失败（" + std::string(std::strerror(errno)) +
               "）：" + prefix;
        return false;
      }
#endif
    }
    if (slash == std::string::npos) break;
    i = slash + 1;
  }
  return true;
}

/// SQLite 的标识符引号是双引号（与 SQL 标准、PG 一致；MySQL 是反引号）。
std::string double_quote(const std::string& ident) {
  std::string out = "\"";
  for (size_t i = 0; i < ident.size(); ++i) {
    if (ident[i] == '"') out.push_back('"');
    out.push_back(ident[i]);
  }
  out.push_back('"');
  return out;
}

}  // namespace

class sqlite_driver : public uvcpp_db_driver {
 public:
  sqlite_driver() {}
  ~sqlite_driver() { close(); }

  const char* name() const { return "sqlite"; }
  bool is_open() const { return db_ != nullptr; }

  uvcpp_db_status open(const uvcpp_db_url& url, std::string* err) {
    close();
    const std::string path = url.database.empty() ? ":memory:" : url.database;
    if (path != ":memory:" && path.find(':') != 0) {
      if (!make_parent_dirs(path, err)) return uvcpp_db_status::OPEN_FAILED;
    }

    // FULLMUTEX：即使 `uvcpp_db_client` 那把递归锁在，也把 sqlite 自己那把
    // 打开 —— 代价是一次用不上的互斥，换来的是"哪天有人绕开 client 直接用
    // 驱动"时不会静默踩线程问题。这条连接是本库独占的，serialized 模式下
    // 也不会有别人来抢。
    const int rc = sqlite3_open_v2(
        path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK) {
      *err = "打开 SQLite 失败：" +
             std::string(db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc)) +
             "（" + path + "）";
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      return uvcpp_db_status::OPEN_FAILED;
    }

    sqlite3_busy_timeout(db_, timeout_ms_);
    path_ = path;

    // 外键默认是**关**的（为了兼容 3.6 之前的行为）。不打开的话
    // `REFERENCES` 只是一句注释，插一条悬空外键照样成功。
    std::string pragma_err;
    if (!uvcpp_db_ok(exec_raw("PRAGMA foreign_keys = ON", &pragma_err))) {
      *err = pragma_err;
      close();
      return uvcpp_db_status::OPEN_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  void close() {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
    path_.clear();
  }

  uvcpp_db_status ping(std::string* err) {
    if (!db_) {
      *err = "SQLite 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db_, "SELECT 1", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
      *err = std::string("SQLite ping 失败：") + sqlite3_errmsg(db_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    const int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW) {
      *err = std::string("SQLite ping 失败：") + sqlite3_errmsg(db_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status query(const std::string& sql,
                        const std::vector<uvcpp_db_value>& params,
                        uvcpp_db_table* out, std::string* err) {
    if (!db_) {
      *err = "SQLite 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (!out) {
      *err = "query 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }

    sqlite3_stmt* stmt = nullptr;
    // SQLite 的 prepare 本来就吃 DDL，也吃多条语句（只准备第一条），
    // 所以这里**不需要** MySQL 那边那条"裸通道"。
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(),
                                      static_cast<int>(sql.size()), &stmt,
                                      nullptr);
    if (rc != SQLITE_OK) {
      *err = std::string("SQLite 准备语句失败：") + sqlite3_errmsg(db_);
      return uvcpp_db_status::PREPARE_FAILED;
    }
    if (!stmt) {  // 空语句 / 纯注释
      *err = "SQLite 准备出来是空语句（只有注释或空串？）";
      return uvcpp_db_status::PREPARE_FAILED;
    }
    const uvcpp_db_status bst = bind_all(stmt, params, err);
    if (!uvcpp_db_ok(bst)) {
      sqlite3_finalize(stmt);
      return bst;
    }

    // 列名在 prepare 之后就能拿（它来自语句的结果描述，不需要先取到一行）。
    // 零行的 SELECT 照样有列名 —— 那样 `to_json()` 才能给出 `[]` 而不是
    // 一个连列都没有的空壳。这一段必须放在 finalize **之前**。
    const int column_count = sqlite3_column_count(stmt);
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(column_count));
    for (int i = 0; i < column_count; ++i) {
      const char* n = sqlite3_column_name(stmt, i);
      names.push_back(n ? n : "");
    }
    out->reset_columns(std::move(names));

    int64_t rows = 0;
    for (;;) {
      const int step = sqlite3_step(stmt);
      if (step == SQLITE_ROW) {
        uvcpp_db_row& row = out->add_row();
        for (int i = 0; i < column_count; ++i) {
          row.push(column_value(stmt, i));
        }
        ++rows;
      } else if (step == SQLITE_DONE) {
        break;
      } else {
        *err = std::string("SQLite 取结果失败：") + sqlite3_errmsg(db_);
        sqlite3_finalize(stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }
    }
    sqlite3_finalize(stmt);

    out->set_affected_rows(rows);
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status execute(const std::string& sql,
                          const std::vector<uvcpp_db_value>& params,
                          int64_t* affected, std::string* err) {
    if (affected) *affected = -1;
    if (!db_) {
      *err = "SQLite 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(),
                                      static_cast<int>(sql.size()), &stmt,
                                      nullptr);
    if (rc != SQLITE_OK) {
      *err = std::string("SQLite 准备语句失败：") + sqlite3_errmsg(db_);
      return uvcpp_db_status::PREPARE_FAILED;
    }
    if (!stmt) {
      *err = "SQLite 准备出来是空语句（只有注释或空串？）";
      return uvcpp_db_status::PREPARE_FAILED;
    }
    const uvcpp_db_status bst = bind_all(stmt, params, err);
    if (!uvcpp_db_ok(bst)) {
      sqlite3_finalize(stmt);
      return bst;
    }

    const int step = sqlite3_step(stmt);
    if (step != SQLITE_DONE && step != SQLITE_ROW) {
      *err = std::string("SQLite 执行失败：") + sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }
    sqlite3_finalize(stmt);
    if (affected) *affected = static_cast<int64_t>(sqlite3_changes(db_));
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status last_insert_id(uvcpp_db_value* out, std::string* err) {
    if (out) out->set_null();
    if (!db_) {
      *err = "SQLite 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    // `sqlite3_last_insert_rowid` 在这条连接上还没成功插过行时返回 0。
    // 用 0 当"报不出"的标记：显式插 rowid=0 的行虽然合法，但那是把 rowid
    // 当数据用，与"自增主键"是两件事 —— 头文件里已经写明这条。
    const sqlite3_int64 id = sqlite3_last_insert_rowid(db_);
    if (id == 0) {
      *err = "这条连接还没成功 INSERT 过，拿不到自增 id";
      return uvcpp_db_status::UNSUPPORTED;
    }
    out->set_int64(static_cast<int64_t>(id));
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status begin(std::string* err) { return exec_raw("BEGIN", err); }
  uvcpp_db_status commit(std::string* err) { return exec_raw("COMMIT", err); }
  uvcpp_db_status rollback(std::string* err) {
    return exec_raw("ROLLBACK", err);
  }

  /// SQLite **不**把反斜杠当转义符（那是 MySQL 的规矩），所以只翻倍单引号。
  /// 多余的 `\` 处理在这里不是"更安全"，而是**改变数据**。
  std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\'') out.push_back('\'');
      out.push_back(text[i]);
    }
    return out;
  }

  std::string quote_identifier(const std::string& ident) {
    return double_quote(ident);
  }

  uvcpp_db_status table_names(std::vector<std::string>* out, std::string* err) {
    if (!out) {
      *err = "table_names 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    out->clear();
    // 排掉 `sqlite_%`：那些是 internal schema（sqlite_sequence 之类），
    // 列出来只会让"库里有哪些表"这个问题多出几个没人建过的名字。
    uvcpp_db_table t;
    const uvcpp_db_status st = query(
        "SELECT name FROM sqlite_master WHERE type = 'table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name",
        std::vector<uvcpp_db_value>(), &t, err);
    if (!uvcpp_db_ok(st)) return st;
    for (size_t i = 0; i < t.row_count(); ++i) {
      out->push_back(t.at(i, static_cast<size_t>(0)).to_text());
    }
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status table_schema(const std::string& table, uvcpp_db_table* out,
                               std::string* err) {
    if (!out) {
      *err = "table_schema 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    // PRAGMA table_info 吃不了绑定参数（它不是函数调用，是语法），所以表名
    // 只能拼进去 —— 因此这里**必须**自己转义。用双引号引起来当标识符，
    // 这是 PRAGMA 认的写法。
    uvcpp_db_table raw;
    const uvcpp_db_status st =
        query("PRAGMA table_info(" + double_quote(table) + ")",
              std::vector<uvcpp_db_value>(), &raw, err);
    if (!uvcpp_db_ok(st)) return st;

    // 归一成三家的共同形状：name / type / nullable / key / default。
    // SQLite 侧「PRI」只有单列主键能报得准（复合主键每一列的 pk 都是一个
    // 序号，>0 就说明它在这条主键里）。
    out->clear();
    out->reset_columns({"name", "type", "nullable", "key", "default"});
    out->set_table_name(table);
    for (size_t i = 0; i < raw.row_count(); ++i) {
      const bool not_null = raw.at(i, "notnull").to_int64() != 0;
      const bool in_pk = raw.at(i, "pk").to_int64() != 0;
      uvcpp_db_row& row = out->add_row();
      row.push(uvcpp_db_value(raw.at(i, "name").to_text()));
      row.push(uvcpp_db_value(raw.at(i, "type").to_text()));
      row.push(uvcpp_db_value(not_null ? "NO" : "YES"));
      row.push(uvcpp_db_value(in_pk ? "PRI" : ""));
      const uvcpp_db_value& dflt = raw.at(i, "dflt_value");
      row.push(dflt.is_null() ? uvcpp_db_value() : uvcpp_db_value(dflt.to_text()));
    }
    return uvcpp_db_status::OK;
  }

  void set_timeout_ms(int ms) {
    timeout_ms_ = ms > 0 ? ms : 0;
    if (db_) sqlite3_busy_timeout(db_, timeout_ms_);
  }

 private:
  /// 一条语句直接丢给 sqlite3_exec。给事务三件套和 PRAGMA 用 —— 它们都是
  /// "没有结果集、没有参数"的语句，走 prepare 只是多两行。
  uvcpp_db_status exec_raw(const char* sql, std::string* err) {
    if (!db_) {
      *err = "SQLite 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
      *err = std::string("SQLite 执行失败：") +
             (message ? message : sqlite3_errmsg(db_));
      if (message) sqlite3_free(message);
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status bind_all(sqlite3_stmt* stmt,
                           const std::vector<uvcpp_db_value>& params,
                           std::string* err) {
    const int want = sqlite3_bind_parameter_count(stmt);
    if (static_cast<int>(params.size()) != want) {
      *err = "参数个数对不上：SQL 里 " + std::to_string(want) + " 个占位符，传了 " +
             std::to_string(params.size()) + " 个";
      return uvcpp_db_status::BIND_FAILED;
    }
    for (size_t i = 0; i < params.size(); ++i) {
      const uvcpp_db_value& v = params[i];
      const int index = static_cast<int>(i) + 1;
      int rc = SQLITE_OK;
      if (v.is_null()) {
        rc = sqlite3_bind_null(stmt, index);
      } else {
        switch (v.type()) {
          case uvcpp_db_type::INT64:
            rc = sqlite3_bind_int64(stmt, index, v.to_int64());
            break;
          case uvcpp_db_type::UINT64:
            // SQLite 的整数是**有符号** 64 位。超过 INT64_MAX 的 uint64 只能
            // 落成 REAL，会丢精度 —— 所以不静默转，直接拒。要存这么大的值
            // 就存成文本/blob。
            if (v.to_uint64() > static_cast<uint64_t>(INT64_MAX)) {
              *err = "第 " + std::to_string(i + 1) +
                     " 个参数是无符号 64 位且超过 INT64_MAX，SQLite 存不下";
              return uvcpp_db_status::BIND_FAILED;
            }
            rc = sqlite3_bind_int64(stmt, index,
                                    static_cast<sqlite3_int64>(v.to_uint64()));
            break;
          case uvcpp_db_type::DOUBLE:
            rc = sqlite3_bind_double(stmt, index, v.to_double());
            break;
          case uvcpp_db_type::BOOL:
            rc = sqlite3_bind_int(stmt, index, v.to_bool() ? 1 : 0);
            break;
          case uvcpp_db_type::TEXT:
          case uvcpp_db_type::DATE:
          case uvcpp_db_type::TIME:
          case uvcpp_db_type::DATETIME:
            // SQLITE_TRANSIENT：让 sqlite 自己拷一份。绑的是 `v.bytes()` 的
            // 内部缓冲，它的生命周期只到本次调用返回为止 —— 用 SQLITE_STATIC
            // 的话 sqlite3_step 时读到的就是别人的内存。
            rc = sqlite3_bind_text(stmt, index, v.bytes().c_str(),
                                   static_cast<int>(v.bytes().size()),
                                   SQLITE_TRANSIENT);
            break;
          case uvcpp_db_type::BLOB:
            rc = sqlite3_bind_blob(
                stmt, index, v.bytes().empty() ? nullptr : v.bytes().data(),
                static_cast<int>(v.bytes().size()), SQLITE_TRANSIENT);
            break;
          default:
            rc = sqlite3_bind_null(stmt, index);
            break;
        }
      }
      if (rc != SQLITE_OK) {
        *err = "绑定第 " + std::to_string(i + 1) + " 个参数失败：" +
               sqlite3_errmsg(db_);
        return uvcpp_db_status::BIND_FAILED;
      }
    }
    return uvcpp_db_status::OK;
  }

  /// 按**声明类型**取值，不是按 `sqlite3_column_text` 一律取文本。
  /// SQLite 是动态类型的：一列里这行是整数、下行是文本是合法的。声明类型
  /// （`sqlite3_column_decltype`）只能当提示，真正可信的是**这一格**的
  /// `sqlite3_column_type` —— 所以按格判断。
  static uvcpp_db_value column_value(sqlite3_stmt* stmt, int column) {
    switch (sqlite3_column_type(stmt, column)) {
      case SQLITE_INTEGER:
        return uvcpp_db_value(
            static_cast<int64_t>(sqlite3_column_int64(stmt, column)));
      case SQLITE_FLOAT:
        return uvcpp_db_value(sqlite3_column_double(stmt, column));
      case SQLITE_TEXT: {
        const unsigned char* text = sqlite3_column_text(stmt, column);
        const int bytes = sqlite3_column_bytes(stmt, column);
        if (!text) return uvcpp_db_value();
        return uvcpp_db_value(std::string(reinterpret_cast<const char*>(text),
                                          static_cast<size_t>(bytes)));
      }
      case SQLITE_BLOB: {
        const void* data = sqlite3_column_blob(stmt, column);
        const int bytes = sqlite3_column_bytes(stmt, column);
        if (!data) return uvcpp_db_value();
        return uvcpp_db_value::blob(data, static_cast<size_t>(bytes));
      }
      case SQLITE_NULL:
      default:
        return uvcpp_db_value();
    }
  }

  sqlite3* db_ = nullptr;
  std::string path_;
  int timeout_ms_ = 5000;
};

std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_sqlite() {
  return std::unique_ptr<uvcpp_db_driver>(new sqlite_driver());
}

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_SQLITE_ENABLE
