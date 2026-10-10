/**
 * @file src/db/uvcpp_db_pgsql.cpp
 * @brief PostgreSQL 后端。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 取值的形状由列的 **OID** 决定（`PQftype`），不看文字内容 —— 与另外两个后端
 * 保持一致：换连接串不该等于换数据库语义。参数一律以文本形式交给
 * `PQexecParams`（`paramFormats` 全 0），省掉一层二进制编码的来回。
 */
#include "db/uvcpp_db_driver.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE && UVCPP_DB_PGSQL_ENABLE

#include <libpq-fe.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace uvcpp {
namespace db_detail {

namespace {

// PostgreSQL 的内置类型 OID。这些是**稳定值**，写死在 `pg_type.dat` 里、
// 从 PG 7 至今没变过，所以不需要带一份 `pg_type_d.h`（那是服务端头文件，
// 装 libpq 的机器上没有）。认不出来的 OID 一律当文本 —— libpq 本来就
// 以文本返回结果，所以"不认识"退化成"原样给字符串"永远是安全的。
const unsigned int kOidBool = 16;
const unsigned int kOidBytea = 17;
const unsigned int kOidInt8 = 20;
const unsigned int kOidInt2 = 21;
const unsigned int kOidInt4 = 23;
const unsigned int kOidOid = 26;
const unsigned int kOidFloat4 = 700;
const unsigned int kOidFloat8 = 701;
const unsigned int kOidNumeric = 1700;
const unsigned int kOidDate = 1082;
const unsigned int kOidTime = 1083;
const unsigned int kOidTimestamp = 1114;
const unsigned int kOidTimestamptz = 1184;

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// 把一段字节编成 bytea 的**文本**形态，给 `PQexecParams` 当参数用。
///
/// 这一步不做的话，`uvcpp_db_value::blob("a\0b")` 会被当成裸文本发出去，
/// 服务端报 `invalid input syntax for type bytea` —— 也就是"三个后端里只有
/// PG 存不进二进制"。我们自己 `SET bytea_output = 'hex'`，所以坚持用 hex 形态
/// （十六进制是 `bytea_output` 的**输出**设置，但它同时是**输入**永远接受的一种
/// 形态，与那个设置无关）。
std::string encode_bytea(const std::string& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2 + 2);
  out.append("\\x");
  for (size_t i = 0; i < bytes.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(bytes[i]);
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

/// 反过来：把 libpq 给的 bytea 文本还原成字节。
///
/// **这一步是必须的**，而且是最容易漏的一处：驱动在 `open()` 里设了
/// `bytea_output = 'hex'`，于是文本协议下 BYTEA 读回来的是 `\x0001feff`
/// 这**十个 ASCII 字符**。把它们当字节交出去，调用方拿到的 blob 长度会是 10、
/// 内容是那串字符 —— 一条"不报错、数据全错"的路。
///
/// 两种形态都认：`\x` + 十六进制（我们自己设的），以及老式的 escape 形态
/// （`\\` 表示一个反斜杠、`\ooo` 表示八进制字节）。后者只在别人把
/// `bytea_output` 改成 `escape` 之后才会出现，但认它只要十来行，而不认它的
/// 后果是静默错数据。两样都不像时**原样返回**：宁可交出一段看得出来的 ASCII，
/// 也不在这里猜字节。
std::string decode_bytea(const std::string& text) {
  if (text.size() >= 2 && text[0] == '\\' && text[1] == 'x') {
    std::string out;
    out.reserve((text.size() - 2) / 2);
    for (size_t i = 2; i + 1 < text.size(); i += 2) {
      const int hi = hex_digit(text[i]);
      const int lo = hex_digit(text[i + 1]);
      if (hi < 0 || lo < 0) return text;  // 不是合法十六进制：原样返回
      out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
  }
  if (text.find('\\') == std::string::npos) return text;

  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 >= text.size()) {
      out.push_back(text[i]);
      continue;
    }
    if (text[i + 1] == '\\') {  // `\\` 是一个反斜杠
      out.push_back('\\');
      ++i;
      continue;
    }
    if (i + 3 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '3' &&
        text[i + 2] >= '0' && text[i + 2] <= '7' && text[i + 3] >= '0' &&
        text[i + 3] <= '7') {
      const int v = (text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 +
                    (text[i + 3] - '0');
      out.push_back(static_cast<char>(v));
      i += 3;
      continue;
    }
    out.push_back(text[i]);  // 认不出来的转义：原样保留那个反斜杠
  }
  return out;
}

/// 按 OID 把**文本**（libpq 的返回形态）还原成合适的值。
///
/// 为什么不学旧实现"一律当 String"：本模块的三个后端必须是同一种手感 ——
/// 同一句 `SELECT age FROM t` 在 MySQL 上给出 int64、在 PG 上给出 text，
/// 调用方就得为每种后端写一遍分支，这正好抵消掉"换连接串就能换库"的意义。
/// 所以这里按 OID 做与 MySQL/SQLite 一致的分类。
///
/// `numeric`（DECIMAL）是**唯一**会丢精度的：它有 128 位有效数字，而本模块
/// 没有定点类型。整数形态的 numeric 走 int64，带小数点/指数的走 double ——
/// 要精确的金额请把列建成 text（`doc/db-guide.md` 里写了）。
uvcpp_db_value value_from_pg(unsigned int oid, const char* data, int len) {
  if (!data) return uvcpp_db_value();
  const std::string text(data, static_cast<size_t>(len));
  switch (oid) {
    case kOidBool:
      // PG 的布尔文本是 't' / 'f'（也有 'true'/'false'/'1'/'0' 的输入形态）。
      return uvcpp_db_value(text == "t" || text == "true" || text == "1");
    case kOidInt2:
    case kOidInt4:
    case kOidInt8:
    case kOidOid:
      return uvcpp_db_value(
          static_cast<int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
    case kOidFloat4:
    case kOidFloat8:
      return uvcpp_db_value(std::strtod(text.c_str(), nullptr));
    case kOidNumeric: {
      // 没有 '.' / 'e' / 'E' 且能整段落进 int64 的，当整数；否则 double。
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
    case kOidBytea:
      // `decode_bytea` 的注释里写了为什么不能直接把 text 当字节 ——
      // 这里拿到的就是 `\x0001feff` 这串字符。
      return uvcpp_db_value::blob(decode_bytea(text));
    case kOidDate:
      return uvcpp_db_value::date(text);
    case kOidTime:
      return uvcpp_db_value::time(text);
    case kOidTimestamp:
    case kOidTimestamptz:
      // TIMESTAMPTZ 的文本带时区后缀（`+08`），原样保留 —— 本模块不替调用方
      // 做时区换算（那需要一张时区表，而这是一个薄封装）。
      return uvcpp_db_value::datetime(text);
    default:
      return uvcpp_db_value(text);
  }
}

/// `PQresult` 的 RAII。libpq 的每一个 `PGresult*` 都要 `PQclear`，而下面
/// 每一条分支（成功/失败/早退）都得清 —— 手写就会漏。
class pg_result {
 public:
  explicit pg_result(PGresult* r) : r_(r) {}
  ~pg_result() {
    if (r_) PQclear(r_);
  }
  PGresult* get() const { return r_; }
  bool ok() const { return r_ != nullptr; }
  ExecStatusType status() const {
    return r_ ? PQresultStatus(r_) : PGRES_FATAL_ERROR;
  }

 private:
  pg_result(const pg_result&);
  pg_result& operator=(const pg_result&);
  PGresult* r_;
};

/// `PGresult` 上的 SQLSTATE 翻成本模块的返回码。
///
/// 分界线是「服务端有没有接受这条语句」——返回码表里 `PREPARE_FAILED` 那一格
/// 的原文就是"SQL 语法错、表/列不存在"，而且明写了"裸通道也会把「没有这个表」
/// 报在这里"。PG 把这件事做得很干净：**42 类**（`syntax_error_or_access_rule_
/// violation`）就是这个意思，本机 16.15 上实测：
///
///     SELCT 1                        -> 42601  syntax_error
///     SELECT * FROM 不存在的表        -> 42P01  undefined_table
///     SELECT 不存在的列               -> 42703  undefined_column
///
/// 三个例外，都是"看着像 42 类、其实不是那个意思"：
///
///   * **42P02 `undefined_parameter`**：SQL 里引用了 `$1`，而调用方一个参数都
///     没给。这是**参数个数**不对，不是 SQL 写错 —— 归 `BIND_FAILED`。不摘出来
///     的话，"少给参数"会报成"SQL 有语法错"。
///   * **08P01 `protocol_violation`**：`PQexecParams` 是 Parse + Bind 两步，
///     参数个数与服务端数出来的对不上时，失败在 Bind 那一侧，报的就是它。
///     实测两种触发都是个数不对（2 个给 1 个占位、1 个给 0 个占位）。libpq
///     自己拼协议消息、不会发出格式错误的 Bind，所以这一号落到这条路上的含义
///     就是个数对不上 —— 而 PG 在这一点上**比 MySQL 强**：它是响亮地报错，
///     不是把多余的参数静默忽略。
///   * **42501 `insufficient_privilege`**：权限不足是执行时才判的（语句本身没
///     问题，是这个人不能跑它），返回码表把"权限不足"列在 `EXEC_FAILED` 那一格。
///
/// 其余的（22 类数据异常、23 类完整性约束、40 类事务回滚、55 类对象状态……）
/// 都是语句被接受了、跑起来才出事 —— 实测文本进 int 列 → 22P02、除零 → 22012。
uvcpp_db_status sqlstate_status(const char* sqlstate) {
  if (sqlstate == nullptr || std::strlen(sqlstate) < 2) {
    return uvcpp_db_status::EXEC_FAILED;
  }
  if (std::strcmp(sqlstate, "42P02") == 0 || std::strcmp(sqlstate, "08P01") == 0) {
    return uvcpp_db_status::BIND_FAILED;
  }
  if (std::strcmp(sqlstate, "42501") == 0) {
    return uvcpp_db_status::EXEC_FAILED;
  }
  // 42 类：`syntax_error_or_access_rule_violation`（SQL 标准里的类，不是
  // PG 私有的号段）。
  if (sqlstate[0] == '4' && sqlstate[1] == '2') {
    return uvcpp_db_status::PREPARE_FAILED;
  }
  return uvcpp_db_status::EXEC_FAILED;
}

}  // namespace

class pgsql_driver : public uvcpp_db_driver {
 public:
  pgsql_driver() {}
  ~pgsql_driver() { close(); }

  const char* name() const { return "postgres"; }
  bool is_open() const { return conn_ != nullptr; }

  uvcpp_db_status open(const uvcpp_db_url& url, std::string* err) {
    close();

    // ★ 用 `PQconnectdbParams`，**不**拼 `"host=... password=..."` 那种
    //   conninfo 字符串。拼字符串有两个洞，而且第二个是安全洞：
    //   1. 口令里有空格 / 单引号就散架（`p ass` 之后那截会被当成新关键字）；
    //   2. 调用方能通过连接串往 conninfo 里**注入关键字**（比如把口令写成
    //      `x host=evil`），等于绕过本模块给的参数。
    //   关键字/值数组这条路没有转义问题：值整份就是一个值，不解析。
    std::vector<const char*> keys;
    std::vector<const char*> values;
    keys.push_back("host");
    values.push_back(url.host.empty() ? "127.0.0.1" : url.host.c_str());
    keys.push_back("port");
    const std::string port = std::to_string(url.port ? url.port : 5432);
    values.push_back(port.c_str());
    keys.push_back("dbname");
    values.push_back(url.database.c_str());
    keys.push_back("user");
    values.push_back(url.user.c_str());
    keys.push_back("password");
    values.push_back(url.password.c_str());
    keys.push_back("connect_timeout");
    values.push_back("10");
    keys.push_back("client_encoding");
    values.push_back("UTF8");
    keys.push_back(nullptr);
    values.push_back(nullptr);

    conn_ = PQconnectdbParams(&keys[0], &values[0], 0);
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
      *err = std::string("连接 PostgreSQL 失败：") +
             (conn_ ? PQerrorMessage(conn_) : "PQconnectdbParams 返回空");
      close();
      return uvcpp_db_status::OPEN_FAILED;
    }

    // statement_timeout 要开连接之后才设得上（它是会话参数）。
    apply_timeout();
    // bytea 用**十六进制**文本格式返回（`\x...`）而不是老的 `escape` 格式；
    // 不开的话 `PQunescapeBytea` 才能解，而本模块按原始字节收。
    exec_simple("SET bytea_output = 'hex'", nullptr);
    return uvcpp_db_status::OK;
  }

  void close() {
    if (conn_) {
      PQfinish(conn_);
      conn_ = nullptr;
    }
  }

  uvcpp_db_status ping(std::string* err) {
    if (!conn_) {
      *err = "PostgreSQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (PQstatus(conn_) != CONNECTION_OK) {
      *err = std::string("PostgreSQL 连接已失效：") + PQerrorMessage(conn_);
      return uvcpp_db_status::EXEC_FAILED;
    }
    // PQstatus 只看得到**本地**已知的状态（对端悄悄消失时它照样是 OK），
    // 所以要真发一次极轻的往返。
    pg_result res(PQexec(conn_, "SELECT 1"));
    if (res.status() != PGRES_TUPLES_OK) {
      *err = std::string("PostgreSQL ping 失败：") + pq_error(res.get());
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status query(const std::string& sql,
                        const std::vector<uvcpp_db_value>& params,
                        uvcpp_db_table* out, std::string* err) {
    if (!conn_) {
      *err = "PostgreSQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (!out) {
      *err = "query 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    if (params.empty()) {
      pg_result res(PQexec(conn_, sql.c_str()));
      return fill_table(res, out, err);
    }
    return exec_params(sql, params, out, nullptr, err);
  }

  uvcpp_db_status execute(const std::string& sql,
                          const std::vector<uvcpp_db_value>& params,
                          int64_t* affected, std::string* err) {
    if (affected) *affected = -1;
    if (!conn_) {
      *err = "PostgreSQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    if (params.empty()) {
      pg_result res(PQexec(conn_, sql.c_str()));
      return fill_table(res, nullptr, err, affected);
    }
    return exec_params(sql, params, nullptr, affected, err);
  }

  uvcpp_db_status last_insert_id(uvcpp_db_value* out, std::string* err) {
    if (out) out->set_null();
    if (!conn_) {
      *err = "PostgreSQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    // `lastval()` 是"本会话最近一次 nextval()"，语义上最接近 MySQL 的
    // LAST_INSERT_ID()。会话里没取过序列时它报 55000 —— 那不是错误，是
    // "这张表没有序列"，所以映射成 UNSUPPORTED 而不是 EXEC_FAILED。
    pg_result res(PQexec(conn_, "SELECT lastval()"));
    if (res.status() != PGRES_TUPLES_OK) {
      *err = "这个会话还没用过序列（表没有 SERIAL/IDENTITY 列？）";
      return uvcpp_db_status::UNSUPPORTED;
    }
    if (PQntuples(res.get()) < 1 || PQgetisnull(res.get(), 0, 0)) {
      *err = "lastval() 没有返回行";
      return uvcpp_db_status::UNSUPPORTED;
    }
    out->set_int64(static_cast<int64_t>(
        std::strtoll(PQgetvalue(res.get(), 0, 0), nullptr, 10)));
    return uvcpp_db_status::OK;
  }

  uvcpp_db_status begin(std::string* err) { return exec_simple("BEGIN", err); }
  uvcpp_db_status commit(std::string* err) { return exec_simple("COMMIT", err); }
  uvcpp_db_status rollback(std::string* err) {
    return exec_simple("ROLLBACK", err);
  }

  std::string escape(const std::string& text) {
    if (!conn_) {
      std::string out;
      out.reserve(text.size());
      for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\'') out.push_back('\'');
        out.push_back(text[i]);
      }
      return out;
    }
    std::vector<char> buf(text.size() * 2 + 1);
    int error = 0;
    const size_t n = PQescapeStringConn(conn_, buf.data(), text.c_str(),
                                        text.size(), &error);
    if (error != 0) {
      // 走到这里说明连接里存的编码信息不对（服务端字符集与 client_encoding
      // 对不上）。返回空串比返回**没转义**的原串好：后者会被当成安全的拼进
      // SQL，而它恰恰是没转义的。
      return std::string();
    }
    return std::string(buf.data(), n);
  }

  std::string quote_identifier(const std::string& ident) {
    if (!conn_) {
      // 没连接时的退路：双引号 + 内部双引号翻倍。这是 SQL 标准的写法，
      // 不需要连接里的编码信息。
      std::string out = "\"";
      for (size_t i = 0; i < ident.size(); ++i) {
        if (ident[i] == '"') out.push_back('"');
        out.push_back(ident[i]);
      }
      out.push_back('"');
      return out;
    }
    char* quoted = PQescapeIdentifier(conn_, ident.c_str(), ident.size());
    if (!quoted) return std::string();
    const std::string out(quoted);
    PQfreemem(quoted);
    return out;
  }

  uvcpp_db_status table_names(std::vector<std::string>* out, std::string* err) {
    if (!out) {
      *err = "table_names 的 out 是空的";
      return uvcpp_db_status::MISUSE;
    }
    out->clear();
    uvcpp_db_table t;
    const uvcpp_db_status st = query(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema = current_schema() AND table_type = 'BASE TABLE' "
        "ORDER BY table_name",
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
    // 表名走参数（$1），不当标识符拼 —— information_schema 里表名就是一个
    // 普通文本列，所以这里能参数化，比 identity 转义更稳。
    //
    // `key` 那一列要 join 主键约束才拿得到：`information_schema.columns`
    // 本身不说哪一列是主键。
    uvcpp_db_table raw;
    const std::vector<uvcpp_db_value> args(1, uvcpp_db_value(table));
    const uvcpp_db_status st = query(
        "SELECT c.column_name, c.data_type, c.is_nullable, c.column_default, "
        "       CASE WHEN pk.column_name IS NULL THEN '' ELSE 'PRI' END AS key "
        "FROM information_schema.columns c "
        "LEFT JOIN ("
        "  SELECT kcu.column_name "
        "  FROM information_schema.table_constraints tc "
        "  JOIN information_schema.key_column_usage kcu "
        "    ON kcu.constraint_name = tc.constraint_name "
        "   AND kcu.table_schema = tc.table_schema "
        "  WHERE tc.constraint_type = 'PRIMARY KEY' "
        "    AND tc.table_schema = current_schema() AND tc.table_name = $1"
        ") pk ON pk.column_name = c.column_name "
        "WHERE c.table_schema = current_schema() AND c.table_name = $1 "
        "ORDER BY c.ordinal_position",
        args, &raw, err);
    if (!uvcpp_db_ok(st)) return st;

    out->clear();
    out->reset_columns({"name", "type", "nullable", "key", "default"});
    out->set_table_name(table);
    for (size_t i = 0; i < raw.row_count(); ++i) {
      uvcpp_db_row& row = out->add_row();
      row.push(uvcpp_db_value(raw.at(i, "column_name").to_text()));
      row.push(uvcpp_db_value(raw.at(i, "data_type").to_text()));
      // PG 的 `is_nullable` 直接就是 'YES'/'NO'（MySQL 的 `Null` 是同一个
      // 形状），所以三家的 nullable 列能对齐。
      row.push(uvcpp_db_value(raw.at(i, "is_nullable").to_text()));
      row.push(uvcpp_db_value(raw.at(i, "key").to_text()));
      row.push(raw.at(i, "column_default"));
    }
    return uvcpp_db_status::OK;
  }

  void set_timeout_ms(int ms) {
    timeout_ms_ = ms > 0 ? ms : 0;
    apply_timeout();
  }

 private:
  const char* pq_error(PGresult* res) const {
    if (res) {
      const char* m = PQresultErrorMessage(res);
      if (m && m[0]) return m;
    }
    return conn_ ? PQerrorMessage(conn_) : "连接已关闭";
  }

  /// `PQexecParams`。`out` 与 `affected` 至少有一个非空（调用方保证）。
  ///
  /// 参数一律以**文本**形式传（`paramFormats` 传 nullptr ⇒ 全文本），类型
  /// 也让服务端从上下文推断（`paramTypes` 传 nullptr）：文本形态下
  /// `'123'` 与 `123` 在 PG 眼里是同一个值，所以 int64/double/bool 都能原样
  /// 走这条路，不需要为每种类型预备二进制编码。
  uvcpp_db_status exec_params(const std::string& sql,
                              const std::vector<uvcpp_db_value>& params,
                              uvcpp_db_table* out, int64_t* affected,
                              std::string* err) {
    std::vector<std::string> storage(params.size());
    std::vector<const char*> values(params.size(), nullptr);
    for (size_t i = 0; i < params.size(); ++i) {
      if (params[i].is_null()) continue;  // nullptr = SQL NULL
      // BLOB 必须编成 bytea 的文本形态（`\x...`）再发；其余类型 `to_text()`
      // 就是服务端要的形态。
      storage[i] = params[i].type() == uvcpp_db_type::BLOB
                       ? encode_bytea(params[i].bytes())
                       : params[i].to_text();
      values[i] = storage[i].c_str();
    }
    pg_result res(PQexecParams(conn_, sql.c_str(),
                               static_cast<int>(params.size()), nullptr,
                               values.empty() ? nullptr : &values[0], nullptr,
                               nullptr, 0));
    return fill_table(res, out, err, affected);
  }

  /// `PGresult` → `uvcpp_db_table` / 影响行数 / 错误码。
  uvcpp_db_status fill_table(const pg_result& res, uvcpp_db_table* out,
                             std::string* err, int64_t* affected = nullptr) {
    if (affected) *affected = -1;
    if (!res.ok()) {
      // 连 `PGresult` 都没有：连接断了或协议乱了，没有 SQLSTATE 可看。
      *err = std::string("PostgreSQL 执行失败：") + pq_error(nullptr);
      return uvcpp_db_status::EXEC_FAILED;
    }
    const ExecStatusType st = res.status();
    if (st == PGRES_COMMAND_OK) {
      if (affected) {
        const char* tuples = PQcmdTuples(res.get());
        *affected = (tuples && tuples[0]) ? std::strtoll(tuples, nullptr, 10) : 0;
      }
      if (out) out->set_affected_rows(0);
      return uvcpp_db_status::OK;
    }
    if (st != PGRES_TUPLES_OK) {
      *err = std::string("PostgreSQL 执行失败：") + pq_error(res.get());
      return sqlstate_status(PQresultErrorField(res.get(), PG_DIAG_SQLSTATE));
    }
    if (out) fill_rows(res.get(), out);
    if (affected) *affected = PQntuples(res.get());
    return uvcpp_db_status::OK;
  }

  void fill_rows(PGresult* res, uvcpp_db_table* out) {
    const int ncols = PQnfields(res);
    const int nrows = PQntuples(res);
    std::vector<std::string> names;
    names.reserve(static_cast<size_t>(ncols));
    for (int c = 0; c < ncols; ++c) {
      const char* n = PQfname(res, c);
      names.push_back(n ? n : "");
    }
    out->reset_columns(std::move(names));

    for (int r = 0; r < nrows; ++r) {
      uvcpp_db_row& row = out->add_row();
      for (int c = 0; c < ncols; ++c) {
        if (PQgetisnull(res, r, c)) {
          row.push(uvcpp_db_value());
          continue;
        }
        row.push(value_from_pg(PQftype(res, c), PQgetvalue(res, r, c),
                               PQgetlength(res, r, c)));
      }
    }
    out->set_affected_rows(nrows);
  }

  /// `BEGIN` / `COMMIT` / `ROLLBACK` / `SET ...` 这类没有返回值也没有参数的。
  uvcpp_db_status exec_simple(const char* sql, std::string* err) {
    if (!conn_) {
      if (err) *err = "PostgreSQL 连接没打开";
      return uvcpp_db_status::NOT_CONNECTED;
    }
    pg_result res(PQexec(conn_, sql));
    if (res.status() != PGRES_COMMAND_OK) {
      if (err) {
        *err = std::string("PostgreSQL 执行失败（") + sql + "）：" +
               pq_error(res.get());
      }
      return uvcpp_db_status::EXEC_FAILED;
    }
    return uvcpp_db_status::OK;
  }

  void apply_timeout() {
    if (!conn_) return;
    // 0 = 不限。`statement_timeout` 是**服务端**的取消机制 —— 客户端等着，
    // 到点了服务端把语句掐掉并回一条错误，连接本身还在。
    const std::string sql =
        "SET statement_timeout = " + std::to_string(timeout_ms_);
    exec_simple(sql.c_str(), nullptr);
  }

  PGconn* conn_ = nullptr;
  int timeout_ms_ = 5000;
};

std::unique_ptr<uvcpp_db_driver> uvcpp_db_make_pgsql() {
  return std::unique_ptr<uvcpp_db_driver>(new pgsql_driver());
}

}  // namespace db_detail
}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE && UVCPP_DB_PGSQL_ENABLE
