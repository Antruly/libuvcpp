/**
 * @file src/db/uvcpp_db_odbc.cpp
 * @brief ODBC 后端：一份驱动，覆盖所有提供 ODBC 驱动的数据源。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ## 为什么接 SQL Server 这件事落在 ODBC 上
 *
 * 微软**没有**给 SQL Server 一个可以链接的 C 客户端库 —— 它给的就是 ODBC 驱动
 * （`msodbcsql18`，Windows 上随系统或单独安装）。所以"接 SQL Server"在 C 层
 * 就是"接 ODBC"，不存在第二个选择。
 *
 * 而既然路口是通用的，这个后端就**按通用写**：能连 SQL Server，也能连任何装得出
 * ODBC 驱动的数据源（Oracle、DB2、达梦……）。全文件没有一处 `if (是 SQL Server)`。
 * 唯一带"默认值"色彩的是连接串里那个驱动名（见 `build_connection_string`），
 * 而它只是**默认**，`?Driver=` 一写就换掉。
 *
 * ## 坑一：`SQLWCHAR` 的宽度不是固定的
 *
 * 微软的 ODBC 头里 `SQLWCHAR` 是 `unsigned short`（2 字节，UTF-16）；unixODBC
 * 默认是 `wchar_t`（Linux 上 4 字节，UTF-32）。同一份源码在两边"看起来一样、
 * 跑起来不一样"：按 2 字节算字符偏移，在 unixODBC 上会把每个字符的后半截当成
 * 下一个字符的开头，中文表名第一个字就错，而且是**静默**错。
 *
 * 所以下面不假设宽度：转换按 `sizeof(SQLWCHAR)` 分支，2 字节走 UTF-16、4 字节走
 * UTF-32。判据取**编译期的 `sizeof`**，不是平台宏 —— 平台宏会漏掉"Linux 上装了
 * 微软驱动管理器"这种组合，而那正是 CI 与开发机常见的样子。
 *
 * ## 坑二：文本参数不能用 ANSI 形态送
 *
 * 文本参数一律绑 `SQL_C_WCHAR`，不绑 `SQL_C_CHAR`。绑 `SQL_C_CHAR` 时驱动按
 * **连接的 ANSI 代码页**解释那串字节，而本库内部是 UTF-8：在中文 Windows
 * （CP936）上恰好能蒙对，在英文系统（CP1252）上中文条件就变成乱码去匹配，
 * 结果不是报错而是**查不到** —— 比报错难查得多。
 *
 * ## 坑三：不绑定列，逐格 `SQLGetData`
 *
 * `SQLBindCol` 要求给每一列一个**定长**缓冲，而定长这个数本身就是要猜的：猜小了
 * 长文本被静默截断，猜大了每行都白占内存。这里改用 `SQLGetData` 的分块循环，
 * 两头都不猜。代价是每格多一次函数调用 —— 对报表与表浏览这类用途完全划算。
 */

#include <db/uvcpp_db_driver.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_ODBC_ENABLE

#include <sql.h>
#include <sqlext.h>

// unixODBC 的 `sqltypes.h` 第 75 行有一句 `#define BOOL int`（在没有 `<windows.h>`
// 的平台上补齐 ODBC 那套类型表）。它会把这个文件里唯一一处 `uvcpp_db_type::BOOL`
// 展开成 `uvcpp_db_type::int`，报出来的错是 "expected unqualified-id before 'int'"
// —— 离真正的原因隔着一层，第一次遇到很费时间。
//
// 这里直接 `#undef`：ODBC 头已经预处理完，这个宏的使命结束了；本文件从头到尾
// **不用** ODBC 的 `BOOL`（全文件只有上面那一个枚举常量叫 BOOL）。
// 微软那套 `sqltypes.h` 不发这个宏（Windows 上 `BOOL` 是 `<windows.h>` 里的
// typedef，对 `enum class` 的限定名没影响），所以这行只在 unixODBC 上起作用，
// 在 Windows 上是个空操作。
#undef BOOL

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace uvcpp {
namespace db_detail {
namespace {

// ---------------------------------------------------------------------------
// UTF-8 <-> SQLWCHAR
//
// 四个后端里只有 ODBC 需要这一层：SQLite 收 UTF-8 字节串、libpq 收 UTF-8 字节串、
// MySQL 也能收 UTF-8；而 ODBC 的宽接口收的是 `SQLWCHAR`。自己写而不引 ICU —— 本仓
// 的"薄封装"定位（见 doc/db-guide.md）不允许为一次编码转换拖进一个几十 MB 的依赖。
// ---------------------------------------------------------------------------

/// 取下一个 UTF-8 码点。`*i` 是读位置；越界或非法序列返回 false 并把 `*i` 推到
/// 能确认的边界，调用方据此截断。
///
/// 非法序列**不抛也不替代**：截断在这里，调用方拿到的是转换到那一处为止的结果。
/// 与"猜一个字符补上"相比，截断至少不会把编造的数据写进库里。
bool utf8_next(const std::string& s, size_t* i, uint32_t* cp) {
  if (*i >= s.size()) return false;
  const unsigned char c0 = static_cast<unsigned char>(s[*i]);
  size_t extra = 0;
  uint32_t v = 0;
  if (c0 < 0x80) {
    *cp = c0;
    *i += 1;
    return true;
  } else if ((c0 & 0xE0) == 0xC0) {
    extra = 1;
    v = c0 & 0x1Fu;
  } else if ((c0 & 0xF0) == 0xE0) {
    extra = 2;
    v = c0 & 0x0Fu;
  } else if ((c0 & 0xF8) == 0xF0) {
    extra = 3;
    v = c0 & 0x07u;
  } else {
    *i += 1;  // 落单的续字节，或 5 字节以上的老式序列
    return false;
  }
  if (*i + 1 + extra > s.size()) {  // 尾部被截断的多字节序列
    *i = s.size();
    return false;
  }
  for (size_t k = 1; k <= extra; ++k) {
    const unsigned char ck = static_cast<unsigned char>(s[*i + k]);
    if ((ck & 0xC0) != 0x80) {
      *i += 1;
      return false;
    }
    v = (v << 6) | (ck & 0x3Fu);
  }
  *i += 1 + extra;
  *cp = v;
  return true;
}

/// UTF-8 串 -> `SQLWCHAR` 串。
///
/// `sizeof(SQLWCHAR)` 为 2 时写 UTF-16（含代理对），为 4 时直接写 UTF-32。超出
/// U+10FFFF 的码点按替换字符处理 —— 那种码点不存在，写进去只会让驱动端报一个
/// 位置很难定位的错。
std::basic_string<SQLWCHAR> to_sqlwchar(const std::string& utf8) {
  std::basic_string<SQLWCHAR> out;
  out.reserve(utf8.size());
  size_t i = 0;
  uint32_t cp = 0;
  while (utf8_next(utf8, &i, &cp)) {
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (sizeof(SQLWCHAR) == 2) {
      if (cp <= 0xFFFF) {
        out.push_back(static_cast<SQLWCHAR>(cp));
      } else {
        const uint32_t v = cp - 0x10000;
        out.push_back(static_cast<SQLWCHAR>(0xD800 + (v >> 10)));
        out.push_back(static_cast<SQLWCHAR>(0xDC00 + (v & 0x3FF)));
      }
    } else {
      out.push_back(static_cast<SQLWCHAR>(cp));
    }
  }
  return out;
}

/// `SQLWCHAR` 串 -> UTF-8 串。`n` 是**字符个数**，不是字节数 —— ODBC 的长度指示器
/// 一律按字符计（唯一例外是 `SQL_C_WCHAR` 的返回长度，见 `read_text_column`），
/// 把字符数当字节数用会把中文尾部整段截掉。
std::string to_utf8(const SQLWCHAR* p, size_t n) {
  std::string out;
  if (!p) return out;
  out.reserve(n * 3 / 2 + 1);
  for (size_t k = 0; k < n; ++k) {
    uint32_t cp = static_cast<uint32_t>(p[k]);
    if (sizeof(SQLWCHAR) == 2) {
      if (cp >= 0xD800 && cp <= 0xDBFF && k + 1 < n) {
        const uint32_t lo = static_cast<uint32_t>(p[k + 1]);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          ++k;
        }
      } else if (cp >= 0xD800 && cp <= 0xDFFF) {
        cp = 0xFFFD;  // 落单的代理码元
      }
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

/// 一个 `SQLWCHAR` 串 + 它的**字符**长度，喂给 ODBC 用。
///
/// 一律显式传长度、不传 `SQL_NTS`：后者要求串以 0 结尾，而 SQL 文本里出现
/// U+0000 是可能的（字符串字面量里的二进制）。传长度是唯一正确的喂法。
class wstr {
 public:
  explicit wstr(const std::string& utf8) : s_(to_sqlwchar(utf8)) {}
  SQLWCHAR* p() { return s_.empty() ? nullptr : &s_[0]; }
  /// `SQLExecDirectW` / `SQLPrepareW` 的 TextLength 是 `SQLINTEGER`。
  SQLINTEGER len() const { return static_cast<SQLINTEGER>(s_.size()); }
  /// `SQLTablesW` / `SQLColumnsW` / `SQLPrimaryKeysW` 的名字长度是 `SQLSMALLINT`。
  /// 两个都留着而不是只留一个再到处 cast：这两个宽度在 ODBC 里就是不一样的，
  /// 让调用点写成它真正要的那个类型，比在每个调用点写 `static_cast` 更难写错。
  SQLSMALLINT slen() const {
    return static_cast<SQLSMALLINT>(s_.size());
  }

 private:
  std::basic_string<SQLWCHAR> s_;
};

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------

/// 把一个 ODBC 句柄上挂着的**全部**诊断记录拼成一句话。
///
/// 只取第一条是不够的：SQL Server 的报错常常第一条是"语句无效"、第二条才是
/// "对象名 'dbo.检测收费' 无效"，而真正要看的是第二条。
std::string odbc_diag(SQLSMALLINT handle_type, SQLHANDLE handle) {
  std::string out;
  SQLWCHAR state[8];
  SQLWCHAR msg[1024];
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  for (SQLSMALLINT rec = 1;; ++rec) {
    const SQLRETURN rc = SQLGetDiagRecW(
        handle_type, handle, rec, state, &native, msg,
        static_cast<SQLSMALLINT>(sizeof(msg) / sizeof(msg[0]) - 1), &len);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) break;
    if (!out.empty()) out += "；";
    out += "[";
    out += to_utf8(state, 5);
    out += "] ";
    out += to_utf8(msg, static_cast<size_t>(len));
  }
  if (out.empty()) out = "（驱动没给出诊断信息）";
  return out;
}

/// 第一诊断记录的 SQLSTATE，取不到给空串。
std::string odbc_sqlstate(SQLSMALLINT handle_type, SQLHANDLE handle) {
  SQLWCHAR state[8];
  SQLINTEGER native = 0;
  SQLSMALLINT len = 0;
  if (!SQL_SUCCEEDED(SQLGetDiagRecW(handle_type, handle, 1, state, &native,
                                    nullptr, 0, &len))) {
    return std::string();
  }
  return to_utf8(state, 5);
}

constexpr int kDefaultTimeoutMs = 5000;
/// `SQLGetData` 的分块大小（单位是 `SQLWCHAR` / 字节）。取 2048 是个折中：
/// 报表行几十个字段时不会反复扩容，32 KB 的文本也只要 16 次往返。
constexpr size_t kChunk = 2048;
/// 超过这个长度就走 LONG 类型。SQL Server 的 nvarchar 上限是 4000，报成
/// `SQL_WVARCHAR` 送长文本时驱动会报"字符串被截断"。
constexpr size_t kMaxPlainChars = 4000;

/// `SQLDescribeCol` 的结果，够本驱动映射值类型用了。
struct column_desc {
  std::string name;
  SQLSMALLINT data_type = SQL_UNKNOWN_TYPE;
  SQLSMALLINT nullable = SQL_NULLABLE_UNKNOWN;
  SQLULEN size = 0;
};

/// SQL 摘要，进报错信息用。整条 SQL 可能上万字符，日志里留前 120 字足够定位。
std::string sql_brief(const std::string& sql) {
  std::string one;
  one.reserve(sql.size());
  for (size_t i = 0; i < sql.size(); ++i) {
    const char c = sql[i];
    one.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
  }
  if (one.size() <= 120) return one;
  return one.substr(0, 120) + "…";
}

}  // namespace

class odbc_driver : public uvcpp_db_driver {
 public:
  odbc_driver() {}
  ~odbc_driver() { close(); }

  const char* name() const { return "odbc"; }
  bool is_open() const { return dbc_ != nullptr; }

  uvcpp_db_status open(const uvcpp_db_url& url, std::string* err) {
    close();

    // 每个连接一份 ENV。共享一份能省一点，但要引入引用计数和一条"谁最后走谁
    // 释放"的时序 —— 而 `uvcpp_db_client` 本来就是一个连接一个驱动对象，
    // 省下的那点内存换不来这份复杂度。
    const uvcpp_db_status ost = open_env(err);
    if (!uvcpp_db_ok(ost)) return ost;

    SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "分配 ODBC 连接句柄失败：" + odbc_diag(SQL_HANDLE_ENV, env_);
      release_all();
      return uvcpp_db_status::OPEN_FAILED;
    }

    // 只读模式：在**连接之前**设下去。
    //
    // 这不是本模块的护栏（护栏该在应用层），而是"这道连接只用来查"这件事让驱动
    // 也记一份 —— 哪天上层的只读判断漏了，这里还有一道。ODBC 没规定驱动必须实现
    // 它，不支持的驱动会返回错误，所以失败**当警告吞掉**：一个不支持只读模式的
    // 数据源不该因此完全连不上。
    SQLSetConnectAttr(
        dbc_, SQL_ATTR_ACCESS_MODE,
        reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(SQL_MODE_READ_ONLY)),
        0);

    const int login_s = login_timeout_ms_ / 1000;
    if (login_s > 0) {
      SQLSetConnectAttr(
          dbc_, SQL_ATTR_LOGIN_TIMEOUT,
          reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(login_s)), 0);
    }

    const std::string conn = build_connection_string(url);
    wstr wconn(conn);
    SQLWCHAR out_buf[1024];
    SQLSMALLINT out_len = 0;
    rc = SQLDriverConnectW(
        dbc_, nullptr, wconn.p(), wconn.slen(), out_buf,
        static_cast<SQLSMALLINT>(sizeof(out_buf) / sizeof(out_buf[0]) - 1),
        &out_len, SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "ODBC 连接失败：" + odbc_diag(SQL_HANDLE_DBC, dbc_) +
             "（连接串：" + redact(conn) + "）";
      release_all();
      return uvcpp_db_status::OPEN_FAILED;
    }

    quote_ = query_identifier_quote();

    // 自动提交从"开"起步：事务由 begin/commit/rollback 显式开合。不预设成关，
    // 是因为 `uvcpp_db_client` 之外还有人直接用 query()，那种用法下"忘了 commit"
    // 比"没事务"更难查。
    in_transaction_ = false;
    return uvcpp_db_status::OK;
  }

  void close() {
    if (dbc_) {
      if (in_transaction_) {
        // 开关事务的连接被直接关掉时，ODBC 的行为是"回滚"，这里显式结束一次，
        // 不把结果交给实现细节。
        SQLEndTran(SQL_HANDLE_DBC, dbc_, SQL_ROLLBACK);
        in_transaction_ = false;
      }
      SQLDisconnect(dbc_);
    }
    release_all();
  }

  uvcpp_db_status ping(std::string* err) {
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    // 真发一次极轻的往返，而不是只看本地句柄非空 —— 连接被防火墙/NAT 掐掉时
    // 本地句柄照样非空。
    SQLHSTMT stmt = nullptr;
    const uvcpp_db_status ast = alloc_stmt(&stmt, err);
    if (!uvcpp_db_ok(ast)) return ast;
    const SQLRETURN rc = SQLExecDirectW(stmt, const_cast<SQLWCHAR*>(kPing),
                                        SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "ODBC ping 失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }
    const SQLRETURN fr = SQLFetch(stmt);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (!SQL_SUCCEEDED(fr)) {
      *err = "ODBC ping 取不到结果";
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status query(const std::string& sql,
                        const std::vector<uvcpp_db_value>& params,
                        uvcpp_db_table* out, std::string* err) {
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (!out) {
      *err = "query 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    if (sql.empty()) {
      *err = "SQL 是空串";
      return uvcpp_db_status::MISUSE;
    }

    SQLHSTMT stmt = nullptr;
    const uvcpp_db_status ast = alloc_stmt(&stmt, err);
    if (!uvcpp_db_ok(ast)) return ast;
    const uvcpp_db_status est = exec_on(stmt, sql, params, err);
    if (!uvcpp_db_ok(est)) {
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
      return est;
    }
    const uvcpp_db_status rst = fetch_all(stmt, out, err);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return rst;
  }

  uvcpp_db_status execute(const std::string& sql,
                          const std::vector<uvcpp_db_value>& params,
                          int64_t* affected, std::string* err) {
    if (affected) *affected = -1;
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (sql.empty()) {
      *err = "SQL 是空串";
      return uvcpp_db_status::MISUSE;
    }

    SQLHSTMT stmt = nullptr;
    const uvcpp_db_status ast = alloc_stmt(&stmt, err);
    if (!uvcpp_db_ok(ast)) return ast;
    const uvcpp_db_status est = exec_on(stmt, sql, params, err);
    if (!uvcpp_db_ok(est)) {
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
      return est;
    }

    // DDL 与部分 INSERT 报不出行数，ODBC 用 SQL_NO_TOTAL(-1) 表达，而那正好是
    // "报不出"这个契约（头文件写的：后端报不出时写 -1）。
    SQLLEN rows = 0;
    if (affected && SQL_SUCCEEDED(SQLRowCount(stmt, &rows)) && rows >= 0) {
      *affected = static_cast<int64_t>(rows);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status last_insert_id(uvcpp_db_value* out, std::string* err) {
    if (!out) {
      *err = "last_insert_id 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    out->set_null();
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    // SQL Server 的正解是 `SCOPE_IDENTITY()`：只认**本作用域**里刚插的那行。
    // `@@IDENTITY` 会被触发器里的插入顶掉（插 A 表却拿到触发器写进 B 表的 id），
    // 那是这条路上最容易踩的错。别的数据源不认 SCOPE_IDENTITY 时会报错，那时
    // 如实返回 UNSUPPORTED —— 契约里"报不出"就是这个码，不是硬编一个 0。
    SQLHSTMT stmt = nullptr;
    const uvcpp_db_status ast = alloc_stmt(&stmt, err);
    if (!uvcpp_db_ok(ast)) return ast;
    const SQLRETURN rc = SQLExecDirectW(
        stmt, const_cast<SQLWCHAR*>(kScopeIdentity), SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
      *err = "这个数据源报不出自增值（SCOPE_IDENTITY 不被支持）";
      return uvcpp_db_status::UNSUPPORTED;
    }
    bool is_null = false;
    int64_t v = 0;
    const uvcpp_db_status st = read_scalar_int64(stmt, 1, &v, &is_null, err);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (!uvcpp_db_ok(st)) return st;
    if (is_null) {
      *err = "这条连接还没成功 INSERT 过，拿不到自增 id";
      return uvcpp_db_status::UNSUPPORTED;
    }
    out->set_int64(v);
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status begin(std::string* err) {
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    // ODBC 开事务的动作是"关自动提交"，而重复关一次**不报错** —— 所以另三个
    // 后端那种"重复 begin 由后端报错"在这里不会自然发生，得自己拦。
    //
    // 拦在这里是有意的：静默把两层事务折成一层，调用方内层的 rollback 会把外层的
    // 写入一起回滚掉，而它以为自己只撤了内层。
    if (in_transaction_) {
      *err = "事务已经开着，本模块不支持嵌套事务";
      return uvcpp_db_status::EXEC_FAILED;
    }
    const SQLRETURN rc = SQLSetConnectAttr(
        dbc_, SQL_ATTR_AUTOCOMMIT,
        reinterpret_cast<SQLPOINTER>(
            static_cast<intptr_t>(SQL_AUTOCOMMIT_OFF)),
        0);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "开启 ODBC 事务失败：" + odbc_diag(SQL_HANDLE_DBC, dbc_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    in_transaction_ = true;
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status commit(std::string* err) { return end_tran(SQL_COMMIT, err); }
  uvcpp_db_status rollback(std::string* err) {
    return end_tran(SQL_ROLLBACK, err);
  }

  /// 转义**内容**，不含两侧引号。
  ///
  /// ODBC 没有"转义一个字符串字面量"的接口 —— 转义规则取决于下面接的是哪个
  /// 数据源。所以这里按 SQL 标准的做法办：单引号翻倍，反斜杠**不动**。反斜杠是
  /// MySQL 的规矩，在 SQL Server / PostgreSQL 上改它就是**改变数据**（`a\n`
  /// 会存成 `an`）。
  ///
  /// 要绝对稳妥用 `query(sql, params, ...)` 的参数绑定 —— 那条路才是本模块推荐给
  /// 调用方的，`escape()` 只给"拼 DDL、拼 IN 列表"这类绕不开的场合。
  std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\'') out.push_back('\'');
      out.push_back(text[i]);
    }
    return out;
  }

  std::string quote_identifier(const std::string& ident) {
    // 引号字符来自 `SQLGetInfo`，不是写死的 —— 这正是"通用驱动"该有的样子：
    // SQL Server 报 `"`，有的数据源报 `` ` ``。
    //
    // 它是**串**而不是字符（ODBC 的类型就是串），虽然现实中总是单字符。所以
    // 两侧按整串包，内部的转义只在单字符时做 —— 多字符引号没有"翻倍"这个说法，
    // 硬编一种做法反而会在那种数据源上产生语法错的标识符。
    std::string out;
    out.reserve(ident.size() + 2 * quote_.size());
    out += quote_;
    if (quote_.size() == 1) {
      for (size_t i = 0; i < ident.size(); ++i) {
        if (ident[i] == quote_[0]) out += quote_;
        out.push_back(ident[i]);
      }
    } else {
      out += ident;
    }
    out += quote_;
    return out;
  }

  uvcpp_db_status table_names(std::vector<std::string>* out, std::string* err) {
    if (!out) {
      *err = "table_names 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    out->clear();

    SQLHSTMT stmt = nullptr;
    const uvcpp_db_status ast = alloc_stmt(&stmt, err);
    if (!uvcpp_db_ok(ast)) return ast;

    // SQLTables 的四组 (串, 长度) 依序是 目录 / 模式 / 表名 / **类型**。前三个给
    // NULL = 不过滤（当前库、全部模式、全部名字），只有第四组钉住。
    //
    // 顺序在这里是很容易写错的一处：把 "TABLE" 填到**表名**那一格（第三组）会让
    // 这个函数安静地只列出名字叫 "TABLE" 的表 —— 不报错，只是结果是空的。
    static const SQLWCHAR kTableType[] = {'T', 'A', 'B', 'L', 'E', 0};
    const SQLRETURN rc =
        SQLTablesW(stmt, nullptr, 0, nullptr, 0, nullptr, 0,
                   const_cast<SQLWCHAR*>(kTableType), SQL_NTS);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "ODBC 列不出表：" + odbc_diag(SQL_HANDLE_STMT, stmt);
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }

    // 结果集是 TABLE_CAT / TABLE_SCHEM / TABLE_NAME / TABLE_TYPE / REMARKS，
    // 只取**表名**（不拼 `schema.table`），与另三个后端口径一致；套件钉的是
    // "刚建的表能被列出来"，拼上模式名会让它匹配不上。
    std::vector<std::string> names;
    for (;;) {
      const SQLRETURN fr = SQLFetch(stmt);
      if (fr == SQL_NO_DATA) break;
      if (!SQL_SUCCEEDED(fr)) {
        *err = "ODBC 取表名失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }
      bool is_null = false;
      std::string name;
      const uvcpp_db_status st = read_text_column(stmt, 3, &name, &is_null, err);
      if (!uvcpp_db_ok(st)) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return st;
      }
      if (!is_null && !name.empty()) names.push_back(name);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);

    // 排序去重放在本地做：SQLTables 的顺序由驱动决定，而同一个库在 msodbcsql 与
    // FreeTDS 下给出的顺序不一样 —— 那会让"同一份操作在四个后端得到同一份结果"
    // 在表清单这一项上破掉。另外多模式库里同名表会出现多次，也在这里收掉。
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    *out = std::move(names);
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status table_schema(const std::string& table, uvcpp_db_table* out,
                               std::string* err) {
    if (!out) {
      *err = "table_schema 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }

    // 主键列先单独取一份，下面标 key 用 —— SQLColumns 的结果里没有主键信息。
    std::vector<std::string> pk_columns;
    {
      SQLHSTMT pk = nullptr;
      if (uvcpp_db_ok(alloc_stmt(&pk, err))) {
        wstr wt(table);
        if (SQL_SUCCEEDED(SQLPrimaryKeysW(pk, nullptr, 0, nullptr, 0, wt.p(),
                                          wt.slen()))) {
          for (;;) {
            const SQLRETURN fr = SQLFetch(pk);
            if (fr == SQL_NO_DATA) break;
            if (!SQL_SUCCEEDED(fr)) break;
            bool is_null = false;
            std::string name;
            // 结果是 CAT/SCHEM/TABLE/COLUMN_NAME/KEY_SEQ/PK_NAME，列名在第 4 位。
            if (uvcpp_db_ok(read_text_column(pk, 4, &name, &is_null, err)) &&
                !is_null) {
              pk_columns.push_back(name);
            }
          }
        }
        SQLFreeHandle(SQL_HANDLE_STMT, pk);
      }
      // 取不到主键不是错误：视图与无主键的表都取不到，而它们照样有列。
    }

    SQLHSTMT stmt = nullptr;
    const uvcpp_db_status ast = alloc_stmt(&stmt, err);
    if (!uvcpp_db_ok(ast)) return ast;
    wstr wt(table);
    const SQLRETURN rc = SQLColumnsW(stmt, nullptr, 0, nullptr, 0, wt.p(),
                                     wt.slen(), nullptr, 0);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "ODBC 取表结构失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }

    // 归一成四个后端共同的形状：name / type / nullable / key / default。
    // 列序号按 ODBC 3.x 的 SQLColumns 结果集定义（1 起）：
    //   4 COLUMN_NAME / 6 TYPE_NAME / 11 NULLABLE / 13 COLUMN_DEF
    out->clear();
    out->reset_columns({"name", "type", "nullable", "key", "default"});
    out->set_table_name(table);

    for (;;) {
      const SQLRETURN fr = SQLFetch(stmt);
      if (fr == SQL_NO_DATA) break;
      if (!SQL_SUCCEEDED(fr)) {
        *err = "ODBC 取表结构失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }
      bool null_name = false, null_type = false, null_def = false;
      std::string col_name, type_name, column_def;
      uvcpp_db_status st = read_text_column(stmt, 4, &col_name, &null_name, err);
      if (uvcpp_db_ok(st)) {
        st = read_text_column(stmt, 6, &type_name, &null_type, err);
      }
      if (uvcpp_db_ok(st)) {
        st = read_text_column(stmt, 13, &column_def, &null_def, err);
      }
      if (!uvcpp_db_ok(st)) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return st;
      }
      bool null_nullable = false;
      int64_t nullable = 0;
      st = read_scalar_int64(stmt, 11, &nullable, &null_nullable, err);
      if (!uvcpp_db_ok(st)) {
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return st;
      }

      const bool in_pk =
          std::find(pk_columns.begin(), pk_columns.end(), col_name) !=
          pk_columns.end();

      uvcpp_db_row& row = out->add_row();
      row.push(uvcpp_db_value(col_name));
      row.push(uvcpp_db_value(type_name));
      // NULLABLE 是 SQL_NO_NULLS(0) / SQL_NULLABLE(1) / SQL_NULLABLE_UNKNOWN(2)。
      // 未知按"可空"报：报 NO 会让调用方以为这列一定有值，报 YES 最多让人多判
      // 一次空 —— 两种错的代价不对称。
      row.push(uvcpp_db_value(nullable == SQL_NO_NULLS ? "NO" : "YES"));
      row.push(uvcpp_db_value(in_pk ? "PRI" : ""));
      row.push(null_def ? uvcpp_db_value() : uvcpp_db_value(column_def));
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return uvcpp_db_status::OK;
  }

  void set_timeout_ms(int ms) {
    timeout_ms_ = ms > 0 ? ms : 0;
    // 登录超时单独一份：查询超时是每语句的，管不到 TCP 建连那一段。
    login_timeout_ms_ = ms > 0 ? ms : kDefaultTimeoutMs;
  }

 private:
  // -------------------------------------------------------------------------
  // 连接串
  // -------------------------------------------------------------------------

  /// 把本模块的 URL 拼成一条 ODBC 连接串。
  ///
  /// 形状（`odbc://` 后面照旧是 `user:password@host:port/dbname?opts`）：
  ///
  ///     odbc://sa:pw@172.25.183.9:1433/SYSDB_GB38900
  ///     odbc://sa:pw@host/db?Driver=ODBC+Driver+17+for+SQL+Server
  ///     odbc:///DSN=我的数据源;Uid=u;Pwd=p        ← `?` 之后原样进串
  ///     odbc://user@host/db                        ← 不给口令 = Windows 集成认证
  ///
  /// 两条规矩：
  ///   * `?` 之后的原文**原样**进串，所以写 `?DSN=x`、`?Driver=y`，或写一整条
  ///     ODBC 串都行；
  ///   * URL 里给出的 host/port/database/user/password 只在**串里还没有**同名键时
  ///     才补进去 —— 这样 `?Server=别的机器` 能盖掉 URL 里的 host，而不是出现两个
  ///     Server 键（ODBC 对重复键的行为各家不一，别赌）。
  std::string build_connection_string(const uvcpp_db_url& url) const {
    const std::vector<std::string> present = scan_keys(url.options);
    struct has_key {
      const std::vector<std::string>* keys;
      bool operator()(const char* key) const {
        for (size_t i = 0; i < keys->size(); ++i) {
          if (equals_ci((*keys)[i], key)) return true;
        }
        return false;
      }
    };
    const has_key has = {&present};

    std::vector<std::pair<std::string, std::string> > kv;
    if (!has("Server") && !url.host.empty()) {
      // `tcp:host,port` 是 SQL Server 的写法；别的驱动认不出 `,port` 会忽略它或
      // 报错 —— 这是"通用驱动"在连接串这一层的固有代价，写进注释而不是藏起来。
      kv.push_back(std::make_pair(
          std::string("Server"),
          url.port ? "tcp:" + url.host + "," + std::to_string(url.port)
                   : "tcp:" + url.host));
    }
    if (!has("Database") && !url.database.empty()) {
      kv.push_back(std::make_pair(std::string("Database"), url.database));
    }
    if (!has("Uid") && !has("User") && !url.user.empty()) {
      kv.push_back(std::make_pair(std::string("Uid"), url.user));
    }
    if (!has("Pwd") && !has("Password") && !url.password.empty()) {
      kv.push_back(std::make_pair(std::string("Pwd"), url.password));
    }

    const bool no_credentials = url.user.empty() && !has("Uid") && !has("User");
    if (no_credentials && !has("Trusted_Connection")) {
      // 不给账号 = 走 Windows 集成认证。这比"忘了填口令就报错"更好：集成认证本来
      // 就是同域部署下更该用的方式（没有口令进连接串，也就不会进日志）。
      kv.push_back(
          std::make_pair(std::string("Trusted_Connection"), std::string("yes")));
    }

    std::string out;
    for (size_t i = 0; i < kv.size(); ++i) {
      out += kv[i].first;
      out += "=";
      out += kv[i].second;
      out += ";";
    }

    // 默认驱动名。**只是默认**：`?Driver=` 或 `?DSN=` 一写就换掉，换成任何 ODBC
    // 数据源都行。选它当默认是因为本后端的主要用途是 SQL Server，而这个驱动名是
    // 微软在 Windows 与 Linux 上统一的正式名字。
    if (!has("DSN") && !has("Driver") && !has("FILEDSN")) {
      out += "Driver={ODBC Driver 18 for SQL Server};";
      if (!has("Encrypt")) {
        // 18 版默认强制加密，而内网库多半没配证书 —— 不显式关掉的话，使用者会先
        // 撞上一个"证书不受信任"的握手错误，然后去怀疑连接串写错了。
        out += "Encrypt=no;";
      }
    }

    // `?` 之后的原文最后追加。上面的 has() 已经保证这里不会与前面的键重名。
    if (!url.options.empty()) {
      out += url.options;
      if (!out.empty() && out[out.size() - 1] != ';') out += ";";
    }
    return out;
  }

  /// 报错里回显连接串时把口令抹掉 —— 连接串是会进日志的。
  static std::string redact(const std::string& conn) {
    static const char* const kKeys[] = {"Pwd=", "Password=", "PWD="};
    std::string out = conn;
    for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); ++k) {
      size_t pos = 0;
      while ((pos = out.find(kKeys[k], pos)) != std::string::npos) {
        const size_t begin = pos + std::strlen(kKeys[k]);
        size_t end = out.find(';', begin);
        if (end == std::string::npos) end = out.size();
        out.replace(begin, end - begin, "***");
        pos = begin + 3;
      }
    }
    return out;
  }

  static bool equals_ci(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
      char x = a[i], y = b[i];
      if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
      if (x != y) return false;
    }
    return true;
  }

  /// 从 `Key=Value;Key=Value` 里抠出键名。
  static std::vector<std::string> scan_keys(const std::string& options) {
    std::vector<std::string> keys;
    size_t pos = 0;
    while (pos < options.size()) {
      size_t semi = options.find(';', pos);
      if (semi == std::string::npos) semi = options.size();
      const std::string item = options.substr(pos, semi - pos);
      const size_t eq = item.find('=');
      if (eq != std::string::npos && eq > 0) {
        std::string key = item.substr(0, eq);
        while (!key.empty() &&
               (key[key.size() - 1] == ' ' || key[key.size() - 1] == '\t')) {
          key.erase(key.size() - 1);
        }
        if (!key.empty()) keys.push_back(key);
      }
      pos = semi + 1;
    }
    return keys;
  }

  // -------------------------------------------------------------------------
  // 句柄
  // -------------------------------------------------------------------------

  uvcpp_db_status open_env(std::string* err) {
    SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "分配 ODBC 环境句柄失败（装了 ODBC 驱动管理器吗？）";
      return uvcpp_db_status::NO_DRIVER;
    }
    // 声明 ODBC 3.x 而不是驱动管理器的默认值：不声明时拿到的可能是 ODBC 2 的
    // 行为，而 SQLColumns / 诊断记录的形状在 ODBC 2 与 3 下不一样。
    rc = SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION,
                       reinterpret_cast<SQLPOINTER>(
                           static_cast<intptr_t>(SQL_OV_ODBC3)),
                       0);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "这个 ODBC 驱动管理器不接受 ODBC 3.x：" +
             odbc_diag(SQL_HANDLE_ENV, env_);
      release_env();
      return uvcpp_db_status::NO_DRIVER;
    }
    return uvcpp_db_status::OK;
  }

  void release_env() {
    if (env_) {
      SQLFreeHandle(SQL_HANDLE_ENV, env_);
      env_ = nullptr;
    }
  }

  void release_all() {
    if (dbc_) {
      SQLFreeHandle(SQL_HANDLE_DBC, dbc_);
      dbc_ = nullptr;
    }
    release_env();
  }

  uvcpp_db_status alloc_stmt(SQLHSTMT* out, std::string* err) {
    const SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, dbc_, out);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "分配 ODBC 语句句柄失败：" + odbc_diag(SQL_HANDLE_DBC, dbc_);
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (timeout_ms_ > 0) {
      // SQL_ATTR_QUERY_TIMEOUT 的单位是**秒**，且"0 = 不超时"。不足一秒的取值
      // 向上取整成 1 —— 退成 0 会变成永不超时，那不是调用方要的。
      int secs = (timeout_ms_ + 999) / 1000;
      if (secs < 1) secs = 1;
      SQLSetStmtAttr(*out, SQL_ATTR_QUERY_TIMEOUT,
                     reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(secs)), 0);
    }
    return uvcpp_db_status::OK;
  }

  std::string query_identifier_quote() {
    SQLWCHAR buf[16];
    SQLSMALLINT len = 0;
    const SQLRETURN rc = SQLGetInfoW(dbc_, SQL_IDENTIFIER_QUOTE_CHAR, buf,
                                     sizeof(buf), &len);
    if (SQL_SUCCEEDED(rc) && len >= 1) {
      return to_utf8(buf, static_cast<size_t>(len));
    }
    return "\"";  // 主流数据源都是它，报不出来时按标准走
  }

  // -------------------------------------------------------------------------
  // 执行
  // -------------------------------------------------------------------------

  uvcpp_db_status exec_on(SQLHSTMT stmt, const std::string& sql,
                          const std::vector<uvcpp_db_value>& params,
                          std::string* err) {
    if (params.empty()) {
      // 无参数走 SQLExecDirect：省一次 prepare，而且 DDL 在有些驱动上压根
      // prepare 不了（MySQL 那边为同一件事单独留了条裸通道，这里同理）。
      wstr wsql(sql);
      const SQLRETURN rc = SQLExecDirectW(stmt, wsql.p(), wsql.len());
      if (!SQL_SUCCEEDED(rc)) {
        *err = "ODBC 执行失败：" + odbc_diag(SQL_HANDLE_STMT, stmt) +
               "（SQL：" + sql_brief(sql) + "）";
        // 语法错 / 表不存在 / 对象名无效，ODBC 都在 execute 这一步报，而契约要求
        // 它们是 PREPARE_FAILED。判据取 SQLSTATE 前两位：42 类是语法与访问规则错
        // （42S02 表不存在、42S22 列不存在、42000 语法错）。
        const std::string state = odbc_sqlstate(SQL_HANDLE_STMT, stmt);
        return (state.size() >= 2 && state[0] == '4' && state[1] == '2')
                   ? uvcpp_db_status::PREPARE_FAILED
                   : uvcpp_db_status::EXEC_FAILED;
      }
      return uvcpp_db_status::OK;
    }

    wstr wsql(sql);
    SQLRETURN rc = SQLPrepareW(stmt, wsql.p(), wsql.len());
    if (!SQL_SUCCEEDED(rc)) {
      *err = "ODBC 准备语句失败：" + odbc_diag(SQL_HANDLE_STMT, stmt) +
             "（SQL：" + sql_brief(sql) + "）";
      return uvcpp_db_status::PREPARE_FAILED;
    }

    // 占位符个数对不上要在**绑定之前**报出来。这个检查不是多余的：参数少给时有的
    // 驱动会把没绑的占位符当 NULL 用，于是 `WHERE id = ?` 变成 `WHERE id = NULL`，
    // 静默返回 0 行 —— 症状是"数据没了"，而不是"报错了"。
    SQLSMALLINT want = 0;
    rc = SQLNumParams(stmt, &want);
    if (SQL_SUCCEEDED(rc) && static_cast<size_t>(want) != params.size()) {
      *err = "参数个数对不上：SQL 里 " + std::to_string(want) + " 个占位符，传了 " +
             std::to_string(params.size()) + " 个";
      return uvcpp_db_status::BIND_FAILED;
    }

    const uvcpp_db_status bst = bind_all(stmt, params, err);
    if (!uvcpp_db_ok(bst)) return bst;

    rc = SQLExecute(stmt);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "ODBC 执行失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  /// 绑定全部参数。
  ///
  /// 文本一律送 `SQL_C_WCHAR`（理由见文件头坑二）。数值按各自的 C 类型送，让驱动
  /// 端做转换 —— 全送文本也能跑，但那样服务端拿不到类型信息，索引与隐式转换都会
  /// 退化。
  uvcpp_db_status bind_all(SQLHSTMT stmt,
                           const std::vector<uvcpp_db_value>& params,
                           std::string* err) {
    // `SQLBindParameter` 只记指针，数据要活到 `SQLExecute`，所以这些缓冲是**成员**
    // —— 不能用"每次循环一个局部 string 再取 c_str()"的写法，那样 SQLExecute 读到
    // 的是一批已经析构的内存。
    text_bufs_.clear();
    text_bufs_.reserve(params.size());

    for (size_t i = 0; i < params.size(); ++i) {
      const uvcpp_db_value& v = params[i];
      const SQLUSMALLINT index = static_cast<SQLUSMALLINT>(i + 1);
      SQLRETURN rc = SQL_SUCCESS;

      if (v.is_null()) {
        // 空值的两个条件：C 类型随便（不读缓冲），长度指示器必须是 SQL_NULL_DATA。
        rc = SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_CHAR,
                              SQL_VARCHAR, 0, 0, nullptr, 0, &null_ind_);
      } else {
        switch (v.type()) {
          case uvcpp_db_type::INT64:
            int64_buf_ = v.to_int64();
            rc = SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_SBIGINT,
                                  SQL_BIGINT, 0, 0, &int64_buf_, 0, nullptr);
            break;
          case uvcpp_db_type::UINT64:
            uint64_buf_ = v.to_uint64();
            // 超 INT64_MAX 的 uint64 不能塞进 SQL_BIGINT —— 那会在服务端变成负数。
            // 本模块没有"无符号大整数"这种 SQL 参数类型，所以拒绝，而不是截断。
            if (uint64_buf_ > static_cast<uint64_t>(INT64_MAX)) {
              *err = "第 " + std::to_string(i + 1) +
                     " 个参数是无符号数且超过 int64 上限，本模块不支持";
              return uvcpp_db_status::BIND_FAILED;
            }
            rc = SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_UBIGINT,
                                  SQL_BIGINT, 0, 0, &uint64_buf_, 0, nullptr);
            break;
          case uvcpp_db_type::DOUBLE:
            double_buf_ = v.to_double();
            rc = SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_DOUBLE,
                                  SQL_DOUBLE, 0, 0, &double_buf_, 0, nullptr);
            break;
          case uvcpp_db_type::BOOL:
            bit_buf_ = v.to_bool() ? 1 : 0;
            rc = SQLBindParameter(stmt, index, SQL_PARAM_INPUT, SQL_C_BIT,
                                  SQL_BIT, 0, 0, &bit_buf_, 0, nullptr);
            break;
          case uvcpp_db_type::BLOB:
            bin_buf_ = v.bytes();
            bin_len_ = static_cast<SQLLEN>(bin_buf_.size());
            rc = SQLBindParameter(
                stmt, index, SQL_PARAM_INPUT, SQL_C_BINARY,
                bin_buf_.size() > 8000 ? SQL_LONGVARBINARY : SQL_VARBINARY,
                static_cast<SQLULEN>(bin_buf_.size()), 0,
                bin_buf_.empty() ? nullptr : &bin_buf_[0],
                static_cast<SQLLEN>(bin_buf_.size()), &bin_len_);
            break;
          default: {  // TEXT / DATE / TIME / DATETIME
            text_bufs_.push_back(to_sqlwchar(v.bytes()));
            std::basic_string<SQLWCHAR>& w = text_bufs_.back();
            text_len_ = static_cast<SQLLEN>(w.size());
            const bool long_text = w.size() > kMaxPlainChars;
            rc = SQLBindParameter(
                stmt, index, SQL_PARAM_INPUT, SQL_C_WCHAR,
                long_text ? SQL_WLONGVARCHAR : SQL_WVARCHAR,
                static_cast<SQLULEN>(w.size()), 0, w.empty() ? nullptr : &w[0],
                static_cast<SQLLEN>(w.size() * sizeof(SQLWCHAR)), &text_len_);
            break;
          }
        }
      }

      if (!SQL_SUCCEEDED(rc)) {
        *err = "绑定第 " + std::to_string(i + 1) + " 个参数失败：" +
               odbc_diag(SQL_HANDLE_STMT, stmt);
        return uvcpp_db_status::BIND_FAILED;
      }
    }
    return uvcpp_db_status::OK;
  }

  // -------------------------------------------------------------------------
  // 取结果
  // -------------------------------------------------------------------------

  uvcpp_db_status fetch_all(SQLHSTMT stmt, uvcpp_db_table* out, std::string* err) {
    SQLSMALLINT column_count = 0;
    SQLRETURN rc = SQLNumResultCols(stmt, &column_count);
    if (!SQL_SUCCEEDED(rc)) {
      *err = "ODBC 取结果列数失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
      return uvcpp_db_status::EXEC_FAILED;
    }

    // 先把每一列的名字与类型问清楚，再逐行走 SQLGetData（理由见文件头坑三）。
    std::vector<column_desc> cols(static_cast<size_t>(column_count));
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(column_count));

    for (SQLSMALLINT i = 0; i < column_count; ++i) {
      SQLWCHAR name_buf[256];
      SQLSMALLINT name_len = 0;
      SQLSMALLINT data_type = SQL_UNKNOWN_TYPE;
      SQLULEN column_size = 0;
      SQLSMALLINT decimal_digits = 0;
      SQLSMALLINT nullable = SQL_NULLABLE_UNKNOWN;
      rc = SQLDescribeColW(
          stmt, static_cast<SQLUSMALLINT>(i + 1), name_buf,
          static_cast<SQLSMALLINT>(sizeof(name_buf) / sizeof(name_buf[0]) - 1),
          &name_len, &data_type, &column_size, &decimal_digits, &nullable);
      if (!SQL_SUCCEEDED(rc)) {
        *err = "ODBC 取列描述失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }
      column_desc& ci = cols[static_cast<size_t>(i)];
      // name_len 可能超过缓冲（列名超 255 字符），那样拿到的是截断的名字 —— 记在
      // 注释里而不加一段重取逻辑：256 字符的列名在真实库里不存在。
      ci.name = to_utf8(name_buf, static_cast<size_t>(name_len));
      ci.data_type = data_type;
      ci.nullable = nullable;
      ci.size = column_size;
      names.push_back(ci.name);
    }
    out->reset_columns(std::move(names));

    for (;;) {
      rc = SQLFetch(stmt);
      if (rc == SQL_NO_DATA) break;
      if (!SQL_SUCCEEDED(rc)) {
        *err = "ODBC 取行失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }
      uvcpp_db_row& row = out->add_row();
      for (size_t i = 0; i < cols.size(); ++i) {
        const uvcpp_db_status st = cell_value(
            stmt, static_cast<SQLUSMALLINT>(i + 1), cols[i].data_type, &row, err);
        if (!uvcpp_db_ok(st)) return st;
      }
    }

    // 一条语句可能带多个结果集（存储过程里中途再 SELECT 一次就是）。契约只描述
    // "一个结果表"，所以只取第一个，但要把剩下的**丢掉**，否则下一次在同一句柄上
    // 执行会撞上"还有未读结果"。
    while (SQL_SUCCEEDED(SQLMoreResults(stmt))) {
      // 空转，直到没有更多结果集
    }

    // 注意：SELECT 不设 `affected_rows`（与 pgsql 后端的 query 路径一致，那里只有
    // `execute` 的出参才带行数）。套件钉的是同一份操作在四个后端上得到同一份结果，
    // 这里单独多填一项就会破。
    return uvcpp_db_status::OK;
  }

  /// 读一个定长 C 类型。`buf`/`len` 由调用方按目标类型给。
  /// 返回 false 表示这次 SQLGetData 真出错了（不是 NULL）。
  bool read_fixed(SQLHSTMT stmt, SQLUSMALLINT col, SQLSMALLINT c_type, void* buf,
                  size_t len, bool* is_null) {
    SQLLEN ind = 0;
    const SQLRETURN rc = SQLGetData(stmt, col, c_type, buf, len, &ind);
    if (rc == SQL_NO_DATA || ind == SQL_NULL_DATA) {
      *is_null = true;
      return true;
    }
    if (!SQL_SUCCEEDED(rc)) return false;
    *is_null = false;
    return true;
  }

  /// 取一格的 `int64`。给自增 id 与表结构里的 NULLABLE 用 —— 都是单列单值。
  uvcpp_db_status read_scalar_int64(SQLHSTMT stmt, SQLUSMALLINT col, int64_t* out,
                                    bool* is_null, std::string* err) {
    SQLBIGINT v = 0;
    if (read_fixed(stmt, col, SQL_C_SBIGINT, &v, sizeof(v), is_null)) {
      if (!*is_null) *out = static_cast<int64_t>(v);
      return uvcpp_db_status::OK;
    }
    // 撞上"这列不是数值"时退回文本再解析。
    bool text_null = false;
    std::string text;
    const uvcpp_db_status st = read_text_column(stmt, col, &text, &text_null, err);
    if (!uvcpp_db_ok(st)) return st;
    if (text_null) {
      *is_null = true;
      return uvcpp_db_status::OK;
    }
    *is_null = false;
    *out = static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10));
    return uvcpp_db_status::OK;
  }

  /// 取一格的文本。分块循环，所以再长的文本也不会被截断。
  ///
  /// 空串与 NULL 在这里分得很清：NULL 是 `ind == SQL_NULL_DATA`，空串是读到了内容
  /// 而长度为 0。契约里"空串不是 NULL"落到的就是这一行。
  uvcpp_db_status read_text_column(SQLHSTMT stmt, SQLUSMALLINT col,
                                   std::string* out, bool* is_null,
                                   std::string* err) {
    std::basic_string<SQLWCHAR> acc;
    SQLWCHAR buf[kChunk];
    for (;;) {
      SQLLEN ind = 0;
      const SQLRETURN rc =
          SQLGetData(stmt, col, SQL_C_WCHAR, buf, sizeof(buf), &ind);
      if (rc == SQL_NO_DATA) break;
      if (ind == SQL_NULL_DATA) {
        *is_null = true;
        out->clear();
        return uvcpp_db_status::OK;
      }
      if (!SQL_SUCCEEDED(rc) && rc != SQL_SUCCESS_WITH_INFO) {
        *err = "ODBC 取列值失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }
      // SQL_C_WCHAR 时 ind 报的是**字节数**（这是 ODBC 里唯一按字节报长度的
      // 地方），换算成字符个数的锅在这里背 —— 直接把 ind 当字符数用，结果会
      // 少一半，而且中文越长错得越明显。
      size_t chars = static_cast<size_t>(ind) / sizeof(SQLWCHAR);
      if (chars > kChunk) chars = kChunk;
      acc.append(buf, chars);
      if (rc == SQL_SUCCESS) break;
      // SQL_SUCCESS_WITH_INFO = 还有下一块
    }
    *is_null = false;
    *out = to_utf8(acc.empty() ? nullptr : acc.data(), acc.size());
    return uvcpp_db_status::OK;
  }

  /// 取一格的二进制。同样分块。
  uvcpp_db_status read_binary_column(SQLHSTMT stmt, SQLUSMALLINT col,
                                     std::string* out, bool* is_null,
                                     std::string* err) {
    out->clear();
    unsigned char buf[kChunk];
    for (;;) {
      SQLLEN ind = 0;
      const SQLRETURN rc =
          SQLGetData(stmt, col, SQL_C_BINARY, buf, sizeof(buf), &ind);
      if (rc == SQL_NO_DATA) break;
      if (ind == SQL_NULL_DATA) {
        *is_null = true;
        return uvcpp_db_status::OK;
      }
      if (!SQL_SUCCEEDED(rc) && rc != SQL_SUCCESS_WITH_INFO) {
        *err = "ODBC 取二进制列失败：" + odbc_diag(SQL_HANDLE_STMT, stmt);
        return uvcpp_db_status::EXEC_FAILED;
      }
      // 与文本相反：SQL_C_BINARY 时 ind 就是字节数，而且内容可能带 0x00，
      // 所以只能按长度收，不能靠 strlen。
      size_t bytes = static_cast<size_t>(ind);
      if (bytes > kChunk) bytes = kChunk;
      out->append(reinterpret_cast<const char*>(buf), bytes);
      if (rc == SQL_SUCCESS) break;
    }
    *is_null = false;
    return uvcpp_db_status::OK;
  }

  /// 一格的 ODBC 类型 -> `uvcpp_db_value`。分类口径与另三个后端对齐（共用套件钉住
  /// 的就是这件事），只有两处是 ODBC 特有的：
  ///
  /// * **定长（DECIMAL/NUMERIC）读成 double** —— 与 MySQL/PG 两个后端一致，本模块
  ///   没有定点类型。整数形态的 numeric 走 int64（判据与 pgsql 的 `value_from_pg`
  ///   逐字相同：没有 `.`/`e`/`E` 且整段落进 int64）。要精确的钱就把列建成 TEXT，
  ///   见 doc/db-guide.md；
  /// * **日期时间不交给驱动格式化**，拿 `SQL_DATE_STRUCT` 自己拼 `"YYYY-MM-DD"`。
  ///   让驱动格式化的话，同一个 timestamp 在 msodbcsql 与 FreeTDS 下会给出不同的
  ///   小数位与分隔符，而契约要求四个后端的文本形态逐字节相同。
  uvcpp_db_status cell_value(SQLHSTMT stmt, SQLUSMALLINT col,
                             SQLSMALLINT data_type, uvcpp_db_row* row,
                             std::string* err) {
    bool is_null = false;
    switch (data_type) {
      case SQL_BIT: {
        unsigned char b = 0;
        if (read_fixed(stmt, col, SQL_C_BIT, &b, sizeof(b), &is_null)) {
          row->push(is_null ? uvcpp_db_value() : uvcpp_db_value(b != 0));
          return uvcpp_db_status::OK;
        }
        break;
      }
      case SQL_TINYINT:
      case SQL_SMALLINT:
      case SQL_INTEGER:
      case SQL_BIGINT: {
        int64_t v = 0;
        const uvcpp_db_status st = read_scalar_int64(stmt, col, &v, &is_null, err);
        if (!uvcpp_db_ok(st)) return st;
        row->push(is_null ? uvcpp_db_value() : uvcpp_db_value(v));
        return uvcpp_db_status::OK;
      }
      case SQL_REAL:
      case SQL_FLOAT:
      case SQL_DOUBLE: {
        double d = 0.0;
        if (read_fixed(stmt, col, SQL_C_DOUBLE, &d, sizeof(d), &is_null)) {
          row->push(is_null ? uvcpp_db_value() : uvcpp_db_value(d));
          return uvcpp_db_status::OK;
        }
        break;
      }
      case SQL_DECIMAL:
      case SQL_NUMERIC: {
        // 取文本形态再自己判，而不是让驱动直接转 double —— 这里要能区分"整数
        // 形态的 numeric"（走 int64）与"带小数点的"（走 double）。
        bool text_null = false;
        std::string text;
        const uvcpp_db_status st =
            read_text_column(stmt, col, &text, &text_null, err);
        if (!uvcpp_db_ok(st)) return st;
        row->push(text_null ? uvcpp_db_value() : numeric_value(text));
        return uvcpp_db_status::OK;
      }
      case SQL_BINARY:
      case SQL_VARBINARY:
      case SQL_LONGVARBINARY: {
        std::string bytes;
        const uvcpp_db_status st =
            read_binary_column(stmt, col, &bytes, &is_null, err);
        if (!uvcpp_db_ok(st)) return st;
        row->push(is_null ? uvcpp_db_value() : uvcpp_db_value::blob(bytes));
        return uvcpp_db_status::OK;
      }
      case SQL_TYPE_DATE:
      case SQL_DATE: {
        SQL_DATE_STRUCT ds;
        std::memset(&ds, 0, sizeof(ds));
        if (read_fixed(stmt, col, SQL_C_TYPE_DATE, &ds, sizeof(ds), &is_null)) {
          row->push(is_null ? uvcpp_db_value()
                            : uvcpp_db_value::date(format_date(ds)));
          return uvcpp_db_status::OK;
        }
        break;
      }
      case SQL_TYPE_TIME:
      case SQL_TIME: {
        SQL_TIME_STRUCT ts;
        std::memset(&ts, 0, sizeof(ts));
        if (read_fixed(stmt, col, SQL_C_TYPE_TIME, &ts, sizeof(ts), &is_null)) {
          row->push(is_null ? uvcpp_db_value()
                            : uvcpp_db_value::time(format_time(ts)));
          return uvcpp_db_status::OK;
        }
        break;
      }
      case SQL_TYPE_TIMESTAMP:
      case SQL_TIMESTAMP: {
        SQL_TIMESTAMP_STRUCT ts;
        std::memset(&ts, 0, sizeof(ts));
        if (read_fixed(stmt, col, SQL_C_TYPE_TIMESTAMP, &ts, sizeof(ts),
                       &is_null)) {
          row->push(is_null ? uvcpp_db_value()
                            : uvcpp_db_value::datetime(format_timestamp(ts)));
          return uvcpp_db_status::OK;
        }
        break;
      }
      default:
        break;
    }

    // 其余一律按文本收：`uniqueidentifier`、`xml`、各家私有的字符串类型都落在这里，
    // 走 SQL_C_WCHAR 都能拿到人读的形态。
    bool text_null = false;
    std::string text;
    const uvcpp_db_status st = read_text_column(stmt, col, &text, &text_null, err);
    if (!uvcpp_db_ok(st)) return st;
    row->push(text_null ? uvcpp_db_value() : uvcpp_db_value(text));
    return uvcpp_db_status::OK;
  }

  /// 定长文本 -> int64 或 double。判据与 pgsql 的 `value_from_pg` 的 numeric 分支
  /// 逐字相同，这样同一个 DECIMAL 列在两个后端上得到同一个类型标签。
  static uvcpp_db_value numeric_value(const std::string& text) {
    if (text.find_first_of(".eE") == std::string::npos) {
      errno = 0;
      char* end = nullptr;
      const long long v = std::strtoll(text.c_str(), &end, 10);
      if (errno == 0 && end && *end == '\0') {
        return uvcpp_db_value(static_cast<int64_t>(v));
      }
    }
    return uvcpp_db_value(std::strtod(text.c_str(), nullptr));
  }

  static std::string format_date(const SQL_DATE_STRUCT& d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", static_cast<int>(d.year),
                  static_cast<int>(d.month), static_cast<int>(d.day));
    return std::string(buf);
  }

  static std::string format_time(const SQL_TIME_STRUCT& t) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", static_cast<int>(t.hour),
                  static_cast<int>(t.minute), static_cast<int>(t.second));
    return std::string(buf);
  }

  static std::string format_timestamp(const SQL_TIMESTAMP_STRUCT& t) {
    char buf[48];
    // 小数秒**丢掉**：另三个后端的 DATETIME 形态就是秒级
    // （`"YYYY-MM-DD HH:MM:SS"`），这里多带几位小数会让同一个值在四个后端上
    // 文本形态不一致。要小数秒的场合得显式 CAST 成字符串自己取。
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  static_cast<int>(t.year), static_cast<int>(t.month),
                  static_cast<int>(t.day), static_cast<int>(t.hour),
                  static_cast<int>(t.minute), static_cast<int>(t.second));
    return std::string(buf);
  }

  uvcpp_db_status end_tran(SQLSMALLINT how, std::string* err) {
    if (!dbc_) {
      *err = "ODBC 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (!in_transaction_) {
      *err = "没有开着的事务";
      return uvcpp_db_status::EXEC_FAILED;
    }
    const SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, dbc_, how);
    if (!SQL_SUCCEEDED(rc)) {
      *err = std::string("ODBC ") + (how == SQL_COMMIT ? "提交" : "回滚") +
             "失败：" + odbc_diag(SQL_HANDLE_DBC, dbc_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    // 回到自动提交，下一次 begin 才能重新关一次。
    SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                      reinterpret_cast<SQLPOINTER>(
                          static_cast<intptr_t>(SQL_AUTOCOMMIT_ON)),
                      0);
    in_transaction_ = false;
    return uvcpp_db_status::OK;
  }

  /// 这两条常量**每一位单独写**，不能用 `L"SELECT 1"` —— `L"..."` 的宽度是
  /// `wchar_t`（Linux 上 4 字节），而 `SQLWCHAR` 可能是 2 字节，那正好是文件头
  /// 坑一的翻版：在 unixODBC 上编译得过、跑起来全错。
  static const SQLWCHAR kPing[];
  static const SQLWCHAR kScopeIdentity[];

  SQLHENV env_ = nullptr;
  SQLHDBC dbc_ = nullptr;
  bool in_transaction_ = false;
  int timeout_ms_ = kDefaultTimeoutMs;
  int login_timeout_ms_ = kDefaultTimeoutMs;
  /// 标识符引号（`SQLGetInfo(SQL_IDENTIFIER_QUOTE_CHAR)` 的结果）。连上之前
  /// 按标准取 `"`。
  std::string quote_ = "\"";

  // 绑定缓冲。是成员而不是局部变量，因为 SQLBindParameter 只记指针、数据要活到
  // SQLExecute —— 见 bind_all 里的说明。
  std::vector<std::basic_string<SQLWCHAR> > text_bufs_;
  std::string bin_buf_;
  int64_t int64_buf_ = 0;
  uint64_t uint64_buf_ = 0;
  double double_buf_ = 0.0;
  unsigned char bit_buf_ = 0;
  SQLLEN null_ind_ = SQL_NULL_DATA;
  SQLLEN text_len_ = 0;
  SQLLEN bin_len_ = 0;
};

const SQLWCHAR odbc_driver::kPing[] = {'S', 'E', 'L', 'E', 'C', 'T',
                                       ' ', '1', 0};

const SQLWCHAR odbc_driver::kScopeIdentity[] = {
    'S', 'E', 'L', 'E', 'C', 'T', ' ', 'C', 'A', 'S', 'T', '(', 'S', 'C', 'O',
    'P', 'E', '_', 'I', 'D', 'E', 'N', 'T', 'I', 'T', 'Y', '(', ')', ' ', 'A',
    'S', ' ', 'B', 'I', 'G', 'I', 'N', 'T', ')', 0};

std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_odbc() {
  return std::unique_ptr<uvcpp_db_driver>(new odbc_driver());
}

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_ODBC_ENABLE
