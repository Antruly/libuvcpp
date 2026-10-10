/**
 * @file src/db/uvcpp_db_mysql.cpp
 * @brief MySQL 后端。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 两条通道，不能混：
 *
 *   * **预处理语句**（非空参数）走 `mysql_stmt_*`，参数与结果都按二进制读；
 *   * **裸通道**（空参数）走 `mysql_query`，因为服务端在预处理协议上**拒绝**
 *     `CREATE` / `ALTER` / `DROP`（报 1295），而建表恰恰是本模块要支持的。
 *
 * 时间/日期列在预处理协议里是**二进制** `MYSQL_TIME`，长度报
 * `sizeof(MYSQL_TIME)`；拿通用字符缓冲去接会写穿。所以
 * `is_binary_temporal()` 单独走一条路。
 */
#include "db/uvcpp_db_driver.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_MYSQL_ENABLE

// mysql.h 的位置各家发行版不一样：Debian/Ubuntu 装在 `<mysql/mysql.h>`，
// 但官方 tarball 与 vcpkg 是扁平的 `<mysql.h>`。CMake 那边两个候选目录都挂。
#if defined(__has_include)
#if __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#else
#include <mysql.h>
#endif
#else
#include <mysql.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace uvcpp {
namespace db_detail {

namespace {

/// `MYSQL_BIND::is_null` / `::error` 指向的元素类型：MySQL 8 是 `bool`，
/// MySQL 5.7 与 MariaDB 是 `my_bool`（即 `char`）。写死哪一个都会在另一半
/// 生态上编不过（`cannot convert 'unsigned char*' to 'bool*'`），所以从成员
/// 自己的类型反推一次，下面所有标志缓冲都用这个别名。
typedef typename std::remove_pointer<decltype(MYSQL_BIND::is_null)>::type
    mysql_flag_type;

/// 死锁（`ER_LOCK_DEADLOCK`）。InnoDB 报这个错时会**回滚整个事务**，
/// 所以它不是"这条语句失败了"，而是"你手上一笔交易没了"。
const unsigned int kErrLockDeadlock = 1213;

/// 锁等待超时（`ER_LOCK_WAIT_TIMEOUT`）：这条**不**回滚事务，等一等重试是对的。
const unsigned int kErrLockWaitTimeout = 1205;

/// 预处理协议不支持这条语句（`ER_UNSUPPORTED_PS`）。旧服务端（5.7 及更早）对
/// `CREATE` / `ALTER` / `DROP` 报这个，而建表恰恰是本模块要支持的，所以见到它
/// 要**退到裸通道**去跑，不能当成准备失败交给调用方。
///
/// 8.0.46 上实测：二进制协议**是**能 prepare 并 execute DDL 的（`CREATE TABLE`
/// / `DROP TABLE` 都是 `prepare OK params=0 execute=0`），所以这条回退在
/// 现代服务端上根本不触发 —— 它是给旧服务端和 MariaDB 留的路，本机没有那个
/// 版本，所以**这条路没有实测覆盖**，别把它当成"跑过"。
const unsigned int kErrUnsupportedPs = 1295;

const int kMaxRetries = 3;
const int kRetryBaseDelayMs = 50;

/// 裸通道（`mysql_query`，文本协议）上的失败该报哪个返回码。
///
/// 文本协议是服务端**一步**做完"解析 + 执行"，两种错从同一个出口回来，只能按
/// 错误号分：下面这几号是「服务端根本没接受这条语句」——而返回码表里
/// `PREPARE_FAILED` 那一格的原文正是"SQL 语法错、表/列不存在"，它也明说了
/// "裸通道也会把「没有这个表」报在这里"。剩下的（1062 唯一键冲突、1213 死锁、
/// 1205 锁等待超时、权限不足……）都是语句**被接受了**、执行时才出事，归
/// `EXEC_FAILED`。
///
/// ★ 不这么分的话，同一句写错的 SQL 在 SQLite 上报 `PREPARE_FAILED`、在 MySQL
/// 上报 `EXEC_FAILED`，而调用方没法靠返回码决定"改 SQL 还是重试" —— 换连接串
/// 不该等于换语义。
///
/// 用数字而不是 `ER_*` 宏：`mysqld_error.h` **不在** `mysql.h` 的 include 链上
/// （链上的是 `errmsg.h`，只有客户端那批 `CR_*`），各家发行版与 MariaDB 装在
/// 哪、有没有装都不一定。编不过的宏不如一个带名字注释的数字。
uvcpp_db_status raw_channel_status(unsigned int code) {
  switch (code) {
    case 1049:  // ER_BAD_DB_ERROR —— 库不存在
    case 1051:  // ER_BAD_TABLE_ERROR —— DROP/ALTER 的表不存在
    case 1054:  // ER_BAD_FIELD_ERROR —— 列不存在
    case 1064:  // ER_PARSE_ERROR —— 语法错
    case 1109:  // ER_UNKNOWN_TABLE
    case 1146:  // ER_NO_SUCH_TABLE
      return uvcpp_db_status::PREPARE_FAILED;
    default:
      return uvcpp_db_status::EXEC_FAILED;
  }
}

/// SQL 文本里有没有占位符。**故意不看上下文**（引号、注释、转义都不管）：
/// 这个判据只用来挑通道，假的"有"代价是多走一次二进制协议（服务端数出来的
/// `param_count` 照样是 0），假的"没有"才会让 `?` 落到裸通道上被当成语法错。
/// 宁可多报，不可漏报。
bool sql_has_placeholder(const std::string& sql) {
  return sql.find('?') != std::string::npos;
}

/// `mysql_library_init` 每进程只该调一次，而且它**不是线程安全**的
/// （文档原话：调用它之前不能起别的线程）。用 `std::call_once` 收口。
void ensure_library_initialized() {
  static std::once_flag once;
  std::call_once(once, []() { mysql_library_init(0, nullptr, nullptr); });
}

/// 这些类型驱动按**二进制** `MYSQL_TIME` 写、`length` 报 `sizeof(MYSQL_TIME)`。
/// 用通用字符缓冲接会写穿（DATE/TIME 与 DATETIME 一样，不是只有 DATETIME）。
bool is_binary_temporal(enum_field_types type) {
  return type == MYSQL_TYPE_TIMESTAMP || type == MYSQL_TYPE_DATETIME ||
         type == MYSQL_TYPE_DATE || type == MYSQL_TYPE_TIME;
}

std::string format_time(const MYSQL_TIME& t, enum_field_types type) {
  char buf[48];
  if (type == MYSQL_TYPE_DATE) {
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
                  static_cast<unsigned>(t.year), static_cast<unsigned>(t.month),
                  static_cast<unsigned>(t.day));
  } else if (type == MYSQL_TYPE_TIME) {
    // 负的 TIME 合法（MySQL 的 TIME 是区间量，不只是"一天里的时刻"）。
    std::snprintf(buf, sizeof(buf), "%s%02u:%02u:%02u", t.neg ? "-" : "",
                  static_cast<unsigned>(t.hour),
                  static_cast<unsigned>(t.minute),
                  static_cast<unsigned>(t.second));
  } else {
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
                  static_cast<unsigned>(t.year), static_cast<unsigned>(t.month),
                  static_cast<unsigned>(t.day), static_cast<unsigned>(t.hour),
                  static_cast<unsigned>(t.minute),
                  static_cast<unsigned>(t.second));
  }
  return std::string(buf);
}

/// `MYSQL_TIME` 全零 = 0000-00-00（MySQL 允许的"零日期"，除非开了
/// NO_ZERO_DATE）。它不是 NULL —— 但读出来是一片 0，直接转字符串会得到
/// "0000-00-00"，而使用方往往当成"没值"。这里按 NULL 处理，与旧实现一致。
bool is_zero_time(const MYSQL_TIME& t) {
  return t.year == 0 && t.month == 0 && t.day == 0 && t.hour == 0 &&
         t.minute == 0 && t.second == 0;
}

/// 文本协议（`mysql_store_result`）回来的是**字符串**，按列的声明类型还原。
/// 这是 MySQL 文本协议与二进制协议的区别：二进制协议里 `MYSQL_TIME` 是结构体、
/// 整数是字节，文本协议里一律是十进制/日期字符串。
uvcpp_db_value value_from_text(const char* data, unsigned long len,
                               enum_field_types type) {
  if (!data) return uvcpp_db_value();
  const std::string text(data, static_cast<size_t>(len));
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      // YEAR 在 MySQL 里是 1901..2155 的整数，走整型。
      return uvcpp_db_value(
          static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      // DECIMAL 用 double 装会丢精度（金额就是这么丢的）。本模块没有定点类型，
      // 所以**文本形态反而更准** —— 但调用方拿到的是 double，
      // `doc/db-guide.md` 里写了"金额请用 TEXT/DECIMAL 存字符串"。
      return uvcpp_db_value(std::strtod(text.c_str(), nullptr));
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      return uvcpp_db_value::date(text);
    case MYSQL_TYPE_TIME:
      return uvcpp_db_value::time(text);
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return uvcpp_db_value::datetime(text);
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      // ★ 这四个标签**既是 BLOB 也是 TEXT**：服务端对两者报同一个类型
      // （都是 `MYSQL_TYPE_BLOB` = 252），分它们的是**字符集**，那一步在
      // `is_byte_column()` 里做了。走到这个函数的一律是**非字节列**（字节列
      // 在调用点就被拦下走 `blob()`），所以这里只能按文本还原。
      //
      // 早先这里对 BLOB 家族一律返回 `blob(text)`，于是 `s_val TEXT` 读出来
      // 是个 blob：`to_json()` 打出 `[blob 5 bytes]` 而不是 `"hello"`，
      // 而**不报任何错**。
      return uvcpp_db_value(text);
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_BIT:
      // 这两个没有"文本形态"，不经字符集判断，`is_byte_column()` 也一律返回真
      // —— 保留在这里只是让本函数**单独看**时不会把它们的字节当文本。
      return uvcpp_db_value::blob(text);
    default:
      // STRING / VAR_STRING / VARCHAR / ENUM / SET / JSON / 以及认不出来的：
      // 一律按文本。JSON 特意归这一类（旧实现把它塞进 Blob）—— 它是 utf8 文本，
      // 当二进制抛出去只会让调用方多做一次解码。
      return uvcpp_db_value(text);
  }
}

/// 这一列该不该当**字节**看。两层判据，缺一不可：
///
///  1. **先按类型收窄。** 判据不能写成"`charsetnr == 63` 就是字节"：
///     `DECIMAL` / `INT` 这些**数值**列的 `charsetnr` 同样是 63（binary
///     collation）—— 照那条一刀切的话，金额列会以 blob 的形式抛出来，而调用方
///     拿 `to_double()` 得到的是 0。
///  2. **在"文本系"这几个类型上，字符集才是那根分界线。**
///
/// ★ BLOB 家族（`TINY`/`MEDIUM`/`LONG_BLOB`）**同样要判字符集** —— 当初这里对
/// 它们一律返回 true，于是 `s_val TEXT` 读回来是个 blob、`to_json()` 打出
/// `[blob 5 bytes]`，**且不报任何错**。原因在协议里：`TEXT` 与 `BLOB` 是**同一个
/// 类型标签**，服务端两个都报 `MYSQL_TYPE_BLOB`（252），唯一的区别就是
/// `charsetnr` —— 本机 8.0.46 上实测：
///
///     s_val TEXT  -> type=252 charsetnr=255   （列自己的排序规则 utf8mb4）
///     b_val BLOB  -> type=252 charsetnr=63    （binary collation）
///     v_val VARBINARY(32) -> type=253 charsetnr=63
///
/// 三种走法（裸通道的 `query_raw`、二进制通道的 `read_prepared_result`、
/// 以及 `value_from_column`）都经过这一个函数，所以这一处修完全修好。
bool is_byte_column(const MYSQL_FIELD& field) {
  switch (field.type) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
      // 63 = binary collation。`STRING`/`VAR_STRING`/`VARCHAR` 里它是
      // `BINARY`/`VARBINARY`；BLOB 家族里它就是"这是 BLOB 不是 TEXT"。
      return field.charsetnr == 63;
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_BIT:
      // 这两个没有"文本形态"，不经字符集判断。
      return true;
    default:
      return false;
  }
}

}  // namespace

class mysql_driver : public uvcpp_db_driver {
 public:
  mysql_driver() { ensure_library_initialized(); }
  ~mysql_driver() { close(); }

  const char* name() const { return "mysql"; }
  bool is_open() const { return conn_ != nullptr; }

  uvcpp_db_status open(const uvcpp_db_url& url, std::string* err) {
    close();
    conn_ = mysql_init(nullptr);
    if (!conn_) {
      *err = "mysql_init 失败（内存不够）";
      return uvcpp_db_status::OUT_OF_MEMORY;
    }

    // 超时三件套：连接用固定 10 秒（连接阶段拿不到"语句超时"该有多久），
    // 读/写用调用方设的。**必须在 mysql_real_connect 之前**设 —— 之后设无效。
    unsigned int connect_timeout = 10;
    unsigned int io_timeout = static_cast<unsigned int>(
        timeout_ms_ > 0 ? (timeout_ms_ + 999) / 1000 : 5);
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
    mysql_options(conn_, MYSQL_OPT_READ_TIMEOUT, &io_timeout);
    mysql_options(conn_, MYSQL_OPT_WRITE_TIMEOUT, &io_timeout);

    // CLIENT_MULTI_STATEMENTS：一次 execute() 里能放多条以分号隔开的语句。
    // 开着它是**便利**也是**风险**（拼接的 SQL 能注入第二条语句），所以
    // `doc/db-guide.md` 里明写：拼接 SQL 时必须参数化。
    MYSQL* ok = mysql_real_connect(
        conn_, url.host.empty() ? nullptr : url.host.c_str(),
        url.user.empty() ? nullptr : url.user.c_str(),
        url.password.empty() ? nullptr : url.password.c_str(),
        url.database.empty() ? nullptr : url.database.c_str(),
        url.port ? url.port : 3306, nullptr, CLIENT_MULTI_STATEMENTS);
    if (!ok) {
      *err = std::string("连接 MySQL 失败：") + mysql_error(conn_);
      mysql_close(conn_);
      conn_ = nullptr;
      return uvcpp_db_status::OPEN_FAILED;
    }

    // utf8mb4 优先，失败退 utf8（5.5.3 之前的服务端不认 utf8mb4）。
    // 退不成=utf8 失败才算开不了 —— 用别的字符集连上去，中文会静默变问号。
    if (mysql_set_character_set(conn_, "utf8mb4") != 0 &&
        mysql_set_character_set(conn_, "utf8") != 0) {
      *err = std::string("设置字符集失败（utf8mb4 与 utf8 都不行）：") +
             mysql_error(conn_);
      mysql_close(conn_);
      conn_ = nullptr;
      return uvcpp_db_status::OPEN_FAILED;
    }

    // 锁等待超时压到 3 秒：默认的 50 秒会让一次偶发锁冲突卡住整个调用方。
    // 失败**不算开不了连接**（老服务端/没有 InnoDB 时会失败），所以只记不返回。
    if (mysql_query(conn_, "SET innodb_lock_wait_timeout=3") != 0) {
      last_warning_ = std::string("SET innodb_lock_wait_timeout 失败：") +
                      mysql_error(conn_);
    }

    in_transaction_ = false;
    return uvcpp_db_status::OK;
  }

  void close() {
    if (conn_) {
      mysql_close(conn_);
      conn_ = nullptr;
    }
    in_transaction_ = false;
  }

  uvcpp_db_status ping(std::string* err) {
    if (!conn_) {
      *err = "MySQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (mysql_ping(conn_) != 0) {
      *err = std::string("MySQL ping 失败：") + mysql_error(conn_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status query(const std::string& sql,
                        const std::vector<uvcpp_db_value>& params,
                        uvcpp_db_table* out, std::string* err) {
    if (!conn_) {
      *err = "MySQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (!out) {
      *err = "query 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    // 空参数**不等于**没有占位符。`SELECT ... WHERE id = ?` 只给 0 个参数时走
    // 裸通道，会被服务端当**语法错**报回来（1064，`near '?'`）—— 于是"参数个数
    // 不对"表现成"SQL 写错了"，而 SQLite / PG 那两家在本地就报 `BIND_FAILED`。
    // 见到 `?` 就交给预处理通道，让 `mysql_stmt_param_count` 做权威判据。
    //
    // 字面量里的 `?`（`SELECT '?'`）也会被送过去，代价只是这一次走二进制协议：
    // 那种 SQL 服务端数出来的 `param_count` 就是 0，结果一样。
    if (params.empty() && !sql_has_placeholder(sql)) {
      return query_raw(sql, out, err);
    }
    return query_prepared(sql, params, out, err);
  }

  uvcpp_db_status execute(const std::string& sql,
                          const std::vector<uvcpp_db_value>& params,
                          int64_t* affected, std::string* err) {
    if (affected) *affected = -1;
    if (!conn_) {
      *err = "MySQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    // 与 `query` 同理（上面那段注释）：空参数 + 带 `?` 的 SQL 要走预处理通道，
    // 否则"少给参数"会被报成语法错。无 `?` 的语句（典型就是 DDL）走裸通道，
    // 那条路还顺带支持 `CLIENT_MULTI_STATEMENTS` 的多语句。
    if (params.empty() && !sql_has_placeholder(sql)) {
      return execute_raw(sql, affected, err);
    }
    return execute_prepared(sql, params, affected, err);
  }

  uvcpp_db_status last_insert_id(uvcpp_db_value* out, std::string* err) {
    if (out) out->set_null();
    if (!conn_) {
      *err = "MySQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    // `mysql_insert_id` 是连接级的、由上一次 INSERT 写下的，不用再发一次
    // `SELECT LAST_INSERT_ID()` —— 少一次往返，而且不会与别的语句打架。
    const my_ulonglong id = mysql_insert_id(conn_);
    if (id == 0) {
      *err = "上一次 INSERT 没有产生自增值（没插过 / 表没有 AUTO_INCREMENT）";
      return uvcpp_db_status::UNSUPPORTED;
    }
    out->set_uint64(static_cast<uint64_t>(id));
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status begin(std::string* err) {
    const uvcpp_db_status st = simple_statement("START TRANSACTION", err);
    if (uvcpp_db_ok(st)) in_transaction_ = true;
    return st;
  }

  uvcpp_db_status commit(std::string* err) {
    const uvcpp_db_status st = simple_statement("COMMIT", err);
    // 无论成败都认为事务结束了：COMMIT 失败（比如连接断了）时事务已经不在，
    // 继续把它当"在事务里"只会让后面的语句白等一场。
    in_transaction_ = false;
    return st;
  }

  uvcpp_db_status rollback(std::string* err) {
    const uvcpp_db_status st = simple_statement("ROLLBACK", err);
    in_transaction_ = false;
    return st;
  }

  std::string escape(const std::string& text) {
    if (!conn_) {
      // 没连接时 `mysql_real_escape_string` 没有字符集信息可用。退到"只翻倍
      // 单引号"是**不安全**的（MySQL 还认反斜杠），所以这里干脆不假装能转，
      // 保持与 SQL 标准一致的行为，并在文档里写明"必须先连上"。
      std::string out;
      out.reserve(text.size());
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') out.push_back('\'');
        out.push_back(text[i]);
      }
      return out;
    }
    std::vector<char> buf(text.size() * 2 + 1);
    const unsigned long n = mysql_real_escape_string(
        conn_, buf.data(), text.c_str(), static_cast<unsigned long>(text.size()));
    return std::string(buf.data(), n);
  }

  std::string quote_identifier(const std::string& ident) {
    std::string out = "`";
    for (size_t i = 0; i < ident.size(); ++i) {
      if (ident[i] == '`') out.push_back('`');
      out.push_back(ident[i]);
    }
    out.push_back('`');
    return out;
  }

  uvcpp_db_status table_names(std::vector<std::string>* out, std::string* err) {
    if (!out) {
      *err = "table_names 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    out->clear();
    uvcpp_db_table t;
    const uvcpp_db_status st = query("SHOW FULL TABLES WHERE Table_type = 'BASE TABLE'",
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
    // `SHOW COLUMNS` 的表名不能参数化（它要的是标识符），所以用反引号引起来。
    uvcpp_db_table raw;
    const uvcpp_db_status st =
        query("SHOW COLUMNS FROM " + quote_identifier(table),
              std::vector<uvcpp_db_value>(), &raw, err);
    if (!uvcpp_db_ok(st)) return st;

    out->clear();
    out->reset_columns({"name", "type", "nullable", "key", "default"});
    out->set_table_name(table);
    for (size_t i = 0; i < raw.row_count(); ++i) {
      uvcpp_db_row& row = out->add_row();
      row.push(uvcpp_db_value(raw.at(i, "Field").to_text()));
      row.push(uvcpp_db_value(raw.at(i, "Type").to_text()));
      row.push(uvcpp_db_value(raw.at(i, "Null").to_text()));
      row.push(uvcpp_db_value(raw.at(i, "Key").to_text()));
      row.push(raw.at(i, "Default"));
    }
    return uvcpp_db_status::OK;
  }

  void set_timeout_ms(int ms) {
    timeout_ms_ = ms > 0 ? ms : 0;
    if (!conn_) return;
    // `max_execution_time` 只对**只读** SELECT 生效（MySQL 5.7.8+），且单位是
    // 毫秒。写语句的超时只能靠读/写超时，那个在 open 时定、改不了 ——
    // 所以这里改了只影响后续的 SELECT，文档里写明。
    const std::string sql =
        "SET SESSION max_execution_time = " + std::to_string(timeout_ms_);
    mysql_query(conn_, sql.c_str());
  }

 private:
  // ---- 裸通道（`mysql_query`，文本协议）----
  //
  // 存在的理由有两条，都**不是**"少一次 prepare"：
  //   1. `mysql_stmt_prepare` 不支持 DDL（ALTER/CREATE/... 报 1295
  //      "This command is not supported in the prepared statement protocol yet"）。
  //   2. 无参数的查询走文本协议时，结果直接带列类型回来，不用像二进制协议
  //      那样给每列预备缓冲、再处理截断。
  // 代价是**没有参数绑定**：这条路只走"参数为空"的调用，拼 SQL 的责任在调用方。

  uvcpp_db_status query_raw(const std::string& sql, uvcpp_db_table* out,
                            std::string* err) {
    if (mysql_query(conn_, sql.c_str()) != 0) {
      *err = std::string("MySQL 执行失败：") + mysql_error(conn_);
      // 语法错 / 表列不存在在这里报 `PREPARE_FAILED`（判据见
      // `raw_channel_status`），不是一律 `EXEC_FAILED`。
      return raw_channel_status(mysql_errno(conn_));
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
      // DDL 之类本来就没有结果集 —— 此时 mysql_errno 才是判据。
      if (mysql_errno(conn_) != 0) {
        *err = std::string("MySQL 取结果失败：") + mysql_error(conn_);
        return uvcpp_db_status::EXEC_FAILED;
      }
      return uvcpp_db_status::OK;
    }

    const unsigned int n = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);
    std::vector<std::string> names;
    names.reserve(n);
    for (unsigned int i = 0; i < n; ++i) {
      names.push_back(fields[i].name ? fields[i].name : "");
    }
    out->reset_columns(std::move(names));
    if (n > 0 && fields[0].table && fields[0].table[0] != '\0') {
      out->set_table_name(fields[0].table);
    }

    int64_t rows = 0;
    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(res)) != nullptr) {
      unsigned long* lengths = mysql_fetch_lengths(res);
      uvcpp_db_row& dst = out->add_row();
      for (unsigned int i = 0; i < n; ++i) {
        if (!row[i]) {
          dst.push(uvcpp_db_value());
          continue;
        }
        if (is_byte_column(fields[i])) {
          dst.push(uvcpp_db_value::blob(
              std::string(row[i], static_cast<size_t>(lengths[i]))));
          continue;
        }
        dst.push(value_from_text(row[i], lengths[i], fields[i].type));
      }
      ++rows;
    }
    mysql_free_result(res);
    out->set_affected_rows(rows);
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status execute_raw(const std::string& sql, int64_t* affected,
                              std::string* err) {
    if (mysql_query(conn_, sql.c_str()) != 0) {
      const unsigned int code = mysql_errno(conn_);
      if (!should_retry(code)) {
        *err = std::string("MySQL 执行失败：") + mysql_error(conn_);
        return raw_channel_status(code);
      }
      return retry_execute_raw(sql, affected, err, 1);
    }
    return finish_execute_raw(affected, err);
  }

  uvcpp_db_status retry_execute_raw(const std::string& sql, int64_t* affected,
                                    std::string* err, int attempt) {
    if (attempt >= kMaxRetries) {
      *err = "MySQL 执行失败：重试 " + std::to_string(kMaxRetries) +
             " 次仍拿不到锁：" + mysql_error(conn_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryBaseDelayMs *
                                                          (1 << (attempt - 1))));
    if (mysql_query(conn_, sql.c_str()) != 0) {
      const unsigned int code = mysql_errno(conn_);
      if (!should_retry(code)) {
        *err = std::string("MySQL 重试执行失败：") + mysql_error(conn_);
        return raw_channel_status(code);
      }
      return retry_execute_raw(sql, affected, err, attempt + 1);
    }
    return finish_execute_raw(affected, err);
  }

  uvcpp_db_status finish_execute_raw(int64_t* affected, std::string* err) {
    const my_ulonglong n = mysql_affected_rows(conn_);
    if (affected) {
      // `mysql_affected_rows` 是 `my_ulonglong`，(my_ulonglong)-1 表示"读不出"。
      *affected = (n == static_cast<my_ulonglong>(-1))
                      ? -1
                      : static_cast<int64_t>(n);
    }
    // ★ 结果集必须消费掉：不消费的话，同一条连接上的下一条语句会报
    //   "Commands out of sync; you can't run this command now"。
    //   DDL（ALTER TABLE 之类）同样会返回一个空结果集，不能跳过这一步。
    MYSQL_RES* res = mysql_store_result(conn_);
    if (res) {
      mysql_free_result(res);
    } else if (mysql_errno(conn_) != 0) {
      *err = std::string("MySQL 取结果失败：") + mysql_error(conn_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  /// 该不该重试。**在事务里一律不重试** —— 1213 报出来时 InnoDB 已经把整个
  /// 事务回滚了，重放这一条语句只会写进一个"已经不在事务里"的连接，调用方
  /// 以为还在交易中，实际每条语句都在自动提交。这时候正确的做法是把错误交回
  /// 调用方，让它自己决定要不要重做整笔交易。
  bool should_retry(unsigned int code) const {
    if (in_transaction_) return false;
    return code == kErrLockDeadlock || code == kErrLockWaitTimeout;
  }

  // ---- 预处理通道（`mysql_stmt_*`，二进制协议）----

  uvcpp_db_status query_prepared(const std::string& sql,
                                 const std::vector<uvcpp_db_value>& params,
                                 uvcpp_db_table* out, std::string* err) {
    MYSQL_STMT* stmt = mysql_stmt_init(conn_);
    if (!stmt) {
      *err = std::string("mysql_stmt_init 失败：") + mysql_error(conn_);
      return uvcpp_db_status::OUT_OF_MEMORY;
    }
    // 从这里开始每一条出口都要 `mysql_stmt_close` —— 用一个小的守卫对象收口，
    // 免得某条早退路径漏掉它（旧实现是每处 `throw` 前手写一句，漏一处就漏一个
    // 服务端的 prepared handle）。
    struct stmt_guard {
      MYSQL_STMT* s;
      ~stmt_guard() { mysql_stmt_close(s); }
    } guard = {stmt};

    if (mysql_stmt_prepare(stmt, sql.c_str(),
                           static_cast<unsigned long>(sql.size())) != 0) {
      // 旧服务端在预处理协议上不支持这条语句（`ER_UNSUPPORTED_PS`）时退到裸
      // 通道。只有"本来就没参数"的调用能退 —— 有参数时裸通道没法绑定，
      // 那种情况下 1295 是真的不支持，如实报准备失败。
      if (params.empty() && mysql_stmt_errno(stmt) == kErrUnsupportedPs) {
        return query_raw(sql, out, err);
      }
      *err = std::string("MySQL 准备语句失败：") + mysql_stmt_error(stmt);
      return uvcpp_db_status::PREPARE_FAILED;
    }

    bound_params bound;
    if (!bind_params(stmt, params, &bound, err)) {
      return uvcpp_db_status::BIND_FAILED;
    }

    if (mysql_stmt_execute(stmt) != 0) {
      // 在事务里 `should_retry` 已经返回 false，所以走到重试分支必然不在事务
      // 中，重放这一条语句是安全的。
      const bool retried = should_retry(mysql_stmt_errno(stmt)) &&
                           retry_prepared_execute(stmt, err);
      if (!retried) {
        if (err->empty()) {
          *err = std::string("MySQL 执行失败：") + mysql_stmt_error(stmt);
        }
        return uvcpp_db_status::EXEC_FAILED;
      }
    }

    if (mysql_stmt_store_result(stmt) != 0) {
      *err = std::string("MySQL 取结果失败：") + mysql_stmt_error(stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }
    return read_prepared_result(stmt, out, err);
  }

  bool retry_prepared_execute(MYSQL_STMT* stmt, std::string* err) {
    for (int attempt = 1; attempt < kMaxRetries; ++attempt) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kRetryBaseDelayMs * (1 << (attempt - 1))));
      if (mysql_stmt_execute(stmt) == 0) return true;
      if (!should_retry(mysql_stmt_errno(stmt))) break;
    }
    *err = std::string("MySQL 重试执行失败：") + mysql_stmt_error(stmt);
    return false;
  }

  uvcpp_db_status execute_prepared(const std::string& sql,
                                   const std::vector<uvcpp_db_value>& params,
                                   int64_t* affected, std::string* err) {
    MYSQL_STMT* stmt = mysql_stmt_init(conn_);
    if (!stmt) {
      *err = std::string("mysql_stmt_init 失败：") + mysql_error(conn_);
      return uvcpp_db_status::OUT_OF_MEMORY;
    }
    struct stmt_guard {
      MYSQL_STMT* s;
      ~stmt_guard() { mysql_stmt_close(s); }
    } guard = {stmt};

    if (mysql_stmt_prepare(stmt, sql.c_str(),
                           static_cast<unsigned long>(sql.size())) != 0) {
      // 与 `query_prepared` 同一处回退，理由写在那里。
      if (params.empty() && mysql_stmt_errno(stmt) == kErrUnsupportedPs) {
        return execute_raw(sql, affected, err);
      }
      *err = std::string("MySQL 准备语句失败：") + mysql_stmt_error(stmt);
      return uvcpp_db_status::PREPARE_FAILED;
    }
    bound_params bound;
    if (!bind_params(stmt, params, &bound, err)) {
      return uvcpp_db_status::BIND_FAILED;
    }
    if (mysql_stmt_execute(stmt) != 0) {
      // `retry_prepared_execute` 成功时不动 `err`；失败时它自己写好了原因。
      const bool retried = should_retry(mysql_stmt_errno(stmt)) &&
                           retry_prepared_execute(stmt, err);
      if (!retried) {
        if (err->empty()) {
          *err = std::string("MySQL 执行失败：") + mysql_stmt_error(stmt);
        }
        return uvcpp_db_status::EXEC_FAILED;
      }
    }
    if (affected) {
      *affected = static_cast<int64_t>(mysql_stmt_affected_rows(stmt));
    }
    return uvcpp_db_status::OK;
  }

  /// 参数缓冲的持有者。**必须活到 `mysql_stmt_execute` 返回之后** ——
  /// 绑定数组里存的是裸指针，`mysql_stmt_execute` 那一刻才按它们读数据。
  /// 在绑定函数里就地释放就是 use-after-free，而且是"通常看起来没事"的那种。
  struct bound_params {
    std::vector<MYSQL_BIND> binds;
    std::vector<std::vector<unsigned char> > storage;  // 每个参数的字节缓冲
    std::vector<unsigned long> lengths;
    /// ★ 用裸数组而不是 `vector`：`mysql_flag_type` 在 MySQL 8 上就是 `bool`，
    /// 而 `std::vector<bool>` 是位压缩特化，`&v[i]` 拿到的是个代理对象的地址，
    /// 赋给 `bool*` 编不过 —— 这正是 `MYSQL_BIND` 要求裸指针时最经典的坑。
    std::unique_ptr<mysql_flag_type[]> is_null;
  };

  bool bind_params(MYSQL_STMT* stmt, const std::vector<uvcpp_db_value>& params,
                   bound_params* out, std::string* err) {
    // ★ 个数先对齐，**不能只看 `params.size()` 就往下走**：`mysql_stmt_bind_param`
    // 按服务端报的 `param_count` 读那个绑定数组，多给的元素**被静默忽略** ——
    // `WHERE id = ?` 配两个参数会照常执行、照常返回 OK，调用方以为条件里带了
    // 两个值（1.5.x 上实测到过：套件里"多给参数 -> BIND_FAILED"那条拿到的是
    // OK）。少给则是另一个症状：数组不够长，服务端读的是越界的绑定。
    const size_t want =
        static_cast<size_t>(mysql_stmt_param_count(stmt));
    if (want != params.size()) {
      *err = "MySQL 参数个数对不上：语句要 " + std::to_string(want) + " 个，给了 " +
             std::to_string(params.size()) + " 个";
      return false;
    }
    if (params.empty()) return true;
    const size_t n = params.size();
    out->binds.assign(n, MYSQL_BIND());
    std::memset(out->binds.data(), 0, sizeof(MYSQL_BIND) * n);
    out->lengths.assign(n, 0);
    out->is_null.reset(new mysql_flag_type[n]());
    out->storage.resize(n);

    for (size_t i = 0; i < n; ++i) {
      const uvcpp_db_value& v = params[i];
      MYSQL_BIND& b = out->binds[i];
      b.length = &out->lengths[i];
      b.is_null = &out->is_null[i];

      if (v.is_null()) {
        b.buffer_type = MYSQL_TYPE_NULL;
        continue;
      }
      switch (v.type()) {
        case uvcpp_db_type::INT64: {
          out->storage[i].resize(sizeof(long long));
          long long value = static_cast<long long>(v.to_int64());
          std::memcpy(out->storage[i].data(), &value, sizeof(value));
          b.buffer_type = MYSQL_TYPE_LONGLONG;
          b.buffer = out->storage[i].data();
          b.buffer_length = static_cast<unsigned long>(sizeof(value));
          break;
        }
        case uvcpp_db_type::UINT64: {
          // MySQL 没有无符号绑定类型，unsigned flag 得自己置 —— 不置的话
          // 大于 INT64_MAX 的值会被当负数存进去。
          out->storage[i].resize(sizeof(unsigned long long));
          unsigned long long value =
              static_cast<unsigned long long>(v.to_uint64());
          std::memcpy(out->storage[i].data(), &value, sizeof(value));
          b.buffer_type = MYSQL_TYPE_LONGLONG;
          b.buffer = out->storage[i].data();
          b.buffer_length = static_cast<unsigned long>(sizeof(value));
          b.is_unsigned = 1;
          break;
        }
        case uvcpp_db_type::DOUBLE: {
          out->storage[i].resize(sizeof(double));
          double value = v.to_double();
          std::memcpy(out->storage[i].data(), &value, sizeof(value));
          b.buffer_type = MYSQL_TYPE_DOUBLE;
          b.buffer = out->storage[i].data();
          b.buffer_length = static_cast<unsigned long>(sizeof(value));
          break;
        }
        case uvcpp_db_type::BOOL: {
          out->storage[i].resize(1);
          out->storage[i][0] = v.to_bool() ? 1 : 0;
          b.buffer_type = MYSQL_TYPE_TINY;
          b.buffer = out->storage[i].data();
          b.buffer_length = 1;
          break;
        }
        case uvcpp_db_type::BLOB: {
          // 空 blob 与 NULL 在 MySQL 里是两回事，所以绑一个**长度 0 但非空**
          // 的缓冲，不能退成 MYSQL_TYPE_NULL。缓冲不能是 nullptr —— 参数不是
          // NULL 时驱动会解引用它。
          out->storage[i].assign(v.bytes().begin(), v.bytes().end());
          out->storage[i].push_back(0);
          b.buffer_type = MYSQL_TYPE_BLOB;
          b.buffer = out->storage[i].data();
          b.buffer_length = static_cast<unsigned long>(v.bytes().size());
          break;
        }
        case uvcpp_db_type::DATE:
        case uvcpp_db_type::TIME:
        case uvcpp_db_type::DATETIME:
        case uvcpp_db_type::TEXT:
        default: {
          // 日期时间与文本一样按**字符串**绑：`MYSQL_TYPE_STRING` 会被服务端
          // 按目标列的声明类型转换，所以 `'2024-01-02 03:04:05'` 进 DATETIME
          // 列是准的，不需要在这边先拼一个 MYSQL_TIME。空串同理要留一个字节。
          const std::string& text = v.to_text();
          out->storage[i].assign(text.begin(), text.end());
          out->storage[i].push_back(0);
          b.buffer_type = MYSQL_TYPE_STRING;
          b.buffer = out->storage[i].data();
          b.buffer_length = static_cast<unsigned long>(text.size());
          break;
        }
      }
      out->lengths[i] = b.buffer_length;
    }

    if (mysql_stmt_bind_param(stmt, out->binds.data()) != 0) {
      *err = std::string("绑定参数失败：") + mysql_stmt_error(stmt);
      return false;
    }
    return true;
  }

  /// 读预处理语句的结果集。
  ///
  /// 三处不能省：
  ///  * **每列一个 `is_null` / `error` 标志**，不能共用一个恒假的常量。
  ///    NULL 列的 `length` 不可靠（整型列会留上一行的长度），靠 length 判
  ///    NULL 会把 NULL 读成上一行的值。
  ///  * `DATE`/`TIME`/`DATETIME`/`TIMESTAMP` 必须收进 `MYSQL_TIME` 缓冲，
  ///    驱动对这四种都按二进制结构体写。
  ///  * 缓冲不够长时 `mysql_stmt_fetch` 返回 `MYSQL_DATA_TRUNCATED` 并把该列
  ///    `error` 置位 —— 必须用 `mysql_stmt_fetch_column` 补取，否则长文本
  ///    **静默截断**（旧实现就是直接在 `while (fetch(stmt) == 0)` 里过掉了）。
  uvcpp_db_status read_prepared_result(MYSQL_STMT* stmt, uvcpp_db_table* out,
                                       std::string* err) {
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (!meta) return uvcpp_db_status::OK;  // 没有结果集：DDL / 无返回
    struct res_guard {
      MYSQL_RES* r;
      ~res_guard() { mysql_free_result(r); }
    } rguard = {meta};

    const unsigned int n = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);

    std::vector<std::string> names;
    names.reserve(n);
    for (unsigned int i = 0; i < n; ++i) {
      names.push_back(fields[i].name ? fields[i].name : "");
    }
    out->reset_columns(std::move(names));
    if (n > 0 && fields[0].table && fields[0].table[0] != '\0') {
      out->set_table_name(fields[0].table);
    }
    if (n == 0) return uvcpp_db_status::OK;

    std::vector<std::vector<unsigned char> > buffers(n);
    std::vector<MYSQL_TIME> times(n);
    std::vector<MYSQL_BIND> binds(n);
    std::vector<unsigned long> lengths(n, 0);
    std::unique_ptr<mysql_flag_type[]> is_null(new mysql_flag_type[n]());
    std::unique_ptr<mysql_flag_type[]> error(new mysql_flag_type[n]());
    std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * n);

    for (unsigned int i = 0; i < n; ++i) {
      binds[i].length = &lengths[i];
      binds[i].is_null = &is_null[i];
      binds[i].error = &error[i];
      binds[i].buffer_type = is_binary_temporal(fields[i].type)
                                 ? fields[i].type
                                 : MYSQL_TYPE_STRING;
      if (is_binary_temporal(fields[i].type)) {
        binds[i].buffer = &times[i];
        binds[i].buffer_length = static_cast<unsigned long>(sizeof(MYSQL_TIME));
      } else {
        // 一切非时间类型都按**字符串**收：二进制协议里驱动也认这个（它会在
        // 服务端把数值转成文本再发）。数值类型按本机字节收的话，得为每种宽度
        // 单独开缓冲，还得处理 DECIMAL 这种没有定宽的 —— 收益是省几百字节的
        // 一次转换，代价是每加一种类型就多一处要维护的 switch。
        const unsigned long max_len =
            fields[i].length > 0 ? fields[i].length : 1024;
        buffers[i].resize(static_cast<size_t>(max_len) + 1);
        binds[i].buffer = buffers[i].data();
        binds[i].buffer_length = static_cast<unsigned long>(buffers[i].size());
      }
    }
    if (mysql_stmt_bind_result(stmt, binds.data()) != 0) {
      *err = std::string("绑定结果集失败：") + mysql_stmt_error(stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }

    int64_t rows = 0;
    for (;;) {
      const int rc = mysql_stmt_fetch(stmt);
      if (rc == MYSQL_NO_DATA) break;
      if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
        *err = std::string("MySQL 取结果失败：") + mysql_stmt_error(stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }

      uvcpp_db_row& dst = out->add_row();
      for (unsigned int i = 0; i < n; ++i) {
        if (is_null[i]) {
          dst.push(uvcpp_db_value());
          continue;
        }
        if (rc == MYSQL_DATA_TRUNCATED && error[i]) {
          // 补取这一列：真正长度在 lengths[i] 里（截断时它**仍然**被写成
          // 完整长度，这正是能补取回来的根据）。
          std::vector<unsigned char> full(lengths[i] + 1);
          MYSQL_BIND one;
          std::memset(&one, 0, sizeof(one));
          one.buffer_type = binds[i].buffer_type;
          one.buffer = full.data();
          one.buffer_length = static_cast<unsigned long>(full.size());
          if (mysql_stmt_fetch_column(stmt, &one, i, 0) == 0) {
            dst.push(value_from_column(full.data(), lengths[i], fields[i]));
          } else {
            dst.push(value_from_column(
                static_cast<const unsigned char*>(binds[i].buffer),
                binds[i].buffer_length, fields[i]));
          }
          continue;
        }
        dst.push(value_from_column(
            static_cast<const unsigned char*>(binds[i].buffer), lengths[i],
            fields[i]));
      }
      ++rows;
    }
    out->set_affected_rows(rows);
    return uvcpp_db_status::OK;
  }

  /// 一列的值：时间型从绑定进来的 `MYSQL_TIME` 取，其余从字符串取。
  /// 注意时间型的 `data` 其实是 `&times[i]`，所以这里靠字段类型分派，
  /// 不靠指针内容。
  uvcpp_db_value value_from_column(const unsigned char* data, unsigned long len,
                                   const MYSQL_FIELD& field) {
    if (is_binary_temporal(field.type)) {
      MYSQL_TIME t;
      std::memcpy(&t, data, sizeof(MYSQL_TIME));
      if (is_zero_time(t)) return uvcpp_db_value();
      const std::string text = format_time(t, field.type);
      if (field.type == MYSQL_TYPE_DATE) return uvcpp_db_value::date(text);
      if (field.type == MYSQL_TYPE_TIME) return uvcpp_db_value::time(text);
      return uvcpp_db_value::datetime(text);
    }
    if (is_byte_column(field)) {
      return uvcpp_db_value::blob(std::string(
          reinterpret_cast<const char*>(data), static_cast<size_t>(len)));
    }
    return value_from_text(reinterpret_cast<const char*>(data), len,
                           field.type);
  }

  uvcpp_db_status simple_statement(const char* sql, std::string* err) {
    if (!conn_) {
      *err = "MySQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (mysql_query(conn_, sql) != 0) {
      *err = std::string("MySQL 执行失败（") + sql + "）：" + mysql_error(conn_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (res) mysql_free_result(res);
    return uvcpp_db_status::OK;
  }

  MYSQL* conn_ = nullptr;
  int timeout_ms_ = 5000;
  bool in_transaction_ = false;
  std::string last_warning_;
};

std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_mysql() {
  return std::unique_ptr<uvcpp_db_driver>(new mysql_driver());
}

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_MYSQL_ENABLE
