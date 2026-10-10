/**
 * @file tests/capi/capi_db_func.c
 * @brief C ABI 的 db 片：连接 / 参数 / 结果集 / 值 / 事务 / 池 / 异步。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这份文件是**纯 C**（`.c`），刻意不 include 任何 C++ 头 —— 编译它这件事本身
 * 就是一条判据（见 tests/capi/CMakeLists.txt 里那段）。它量的七件事，每一件都是
 * "P/Invoke 那一侧一定会踩"的：
 *
 *   1. **句柄失效是一条错误码，不是崩溃。** 二次 `free`、陌生指针、池子借出的
 *      句柄拿去 `_client_free()` —— 全都必须是 `E_STALE` / `E_STATE`。
 *      这一条是整个 C 面的立身之本，所以它必须有用例钉着。
 *   2. **所有权只有三种形状**（头文件开头那段）：`_query()` 交出来的表归调用方、
 *      `_cell()` 交出来的值视图是借的、池子借出的连接是借的。这里逐条量。
 *   3. **参数化真的生效**：`?`（SQLite/MySQL）与 `$n`（PostgreSQL）**由调用方
 *      按方言给**，驱动不换算 —— 所以本用例自己带方言表，三个后端一份代码。
 *   4. **事务钉在一条连接上**：`begin` / `rollback` 之后查不到、`begin` /
 *      `commit` 之后查得到。
 *   5. **池子是借还，不是"每次新开"**：`created_total()` 停在 `max` 上而不是
 *      随调用次数上涨；借到上限之后再借是 `NO_CONNECTION`（**不是挂住**）。
 *   6. **异步的回调在门面自带的那条循环线程上被调**，而且 `_in_flight()` 在回调
 *      里是 1（这一笔自己还没收）—— 后一条量的是"账目没错位"。
 *   7. **活句柄收支平衡**（`uvcpp_c_live_handle_count()`）：这一条**不是** 1 的
 *      重复，1 量的是分配器，只有 7 量得到登记表到底有没有在回收。
 *
 * 后端从哪儿来
 * ------------
 * **不写死一个后端**：本用例在启动时把"这台机器上能用的连接串"列一张表，然后
 * 对**每一个**都跑一遍整套断言。
 *
 *   - `UVCPP_DB_TEST_PGSQL_URL` 有值 ⇒ 加一条 PostgreSQL；
 *   - `UVCPP_DB_TEST_MYSQL_URL` 有值 ⇒ 加一条 MySQL；
 *   - 编了 SQLite 后端 ⇒ 再加一条 SQLite（临时文件，跑完删掉）。
 *
 * 一个都没有 ⇒ 打印一行说明并**退出码 3（未判定）**；设了
 * `UVCPP_DB_TEST_REQUIRE=1` 则是**红**（与 `db_mysql_func.cpp` / `db_pgsql_func.cpp`
 * 同一条规矩：CI 上库都起了还判定不了，那是红，不是"跳过"）。
 *
 * 于是本用例在 `db-servers` 那条腿上会**三个后端各跑一遍**，而在只开 SQLite 的
 * `capi` 那条腿上就是 SQLite 一遍 —— 覆盖面跟着连接串走，不跟着编译开关走。
 */

#include <capi/uvcpp_c.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

/* 这一份库里编进了 C 面吗？`uvcpp_c.h` 是按生成的 `uvcpp_config.h` 里的真实
 * 开关拉头的，所以这两条同时也在断言"这个构建的开关与用例的存在性一致"。 */
#if !UVCPP_CAPI_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_CAPI=ON 的构建里被编译"
#endif
#if !UVCPP_DB_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_DB=ON 的构建里被编译"
#endif

/* -------------------------------------------------------------------------
 * 记账
 * -------------------------------------------------------------------------
 * 与 `capi_common_func.c` 同一个形状：每次判定记一笔，末尾印
 * `checks=… failures=…`（`tests/tools/` 下那些驱动靠这一行判断"是它红的，还是
 * 被别人的红带下去的"）。
 *
 * ★ 这两个计数器只在**主线程**上被写：异步回调不直接 CHECK，它把结果记进
 *   `async_ctx`，主线程 `uv_sem_wait()` 醒来之后再判 —— 那是一次真正的
 *   happens-before，所以既没有数据竞争，也不需要原子量。
 * ------------------------------------------------------------------------- */

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                      \
  do {                                                                   \
    ++g_checks;                                                          \
    if (!(cond)) {                                                       \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

#define CHECK_INT(got, want)                                             \
  do {                                                                   \
    int _g = (int)(got);                                                 \
    int _w = (int)(want);                                                \
    ++g_checks;                                                          \
    if (_g != _w) {                                                      \
      printf("FAIL %s:%d: %s = %d, want %d\n", __FILE__, __LINE__, #got,  \
             _g, _w);                                                     \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

#define CHECK_STR(got, want)                                             \
  do {                                                                   \
    const char* _g = (got);                                              \
    const char* _w = (want);                                             \
    ++g_checks;                                                          \
    if (_g == NULL || strcmp(_g, _w) != 0) {                             \
      printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__,         \
             __LINE__, #got, _g != NULL ? _g : "(null)", _w);            \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

/* 缓冲区的约定是"返回真实长度，装得下才写"（`uvcpp_c_common.h` 那一段）。
 * 所以 `rc >= 0 && (size_t)rc < cap && strcmp(...)` 这一条同时也在量
 * "它说的是真长度" —— 只比内容的话，一个永远返回 0 的实现也能骗过去。
 *
 * ★ `call` 里的缓冲区必须写成 `_buf` / `sizeof(_buf)`（宏自己声明的那一份）：
 *   写外面某个 `buf` 的话，这里比的就是**没被碰过的那块内存**，而 `_rc` 仍然
 *   是真的 —— 于是它变成一条只会误报的断言。 */
#define CHECK_TEXT(call, want)                                           \
  do {                                                                   \
    char _buf[256];                                                      \
    int _rc = (call);                                                    \
    ++g_checks;                                                          \
    if (_rc < 0 || (size_t)_rc >= sizeof(_buf) ||                        \
        strcmp(_buf, (want)) != 0) {                                     \
      printf("FAIL %s:%d: %s → rc=%d buf=\"%s\", want \"%s\"\n",          \
             __FILE__, __LINE__, #call, _rc,                             \
             (_rc >= 0 && (size_t)_rc < sizeof(_buf)) ? _buf : "(?)",     \
             (want));                                                    \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

/* -------------------------------------------------------------------------
 * 方言
 * -------------------------------------------------------------------------
 * 三个后端只差四件事：自增主键怎么写、blob 叫什么、占位符长什么样、标识符
 * 用什么括起来。别的都是同一套 SQL。
 * ------------------------------------------------------------------------- */

typedef struct db_dialect {
  const char* name;      /* 驱动名，与 `_driver_name()` 比 */
  const char* ddl;       /* 建表（同步那一节用的那张） */
  int         dollar_ph; /* 占位符是 `$n`（PostgreSQL）还是 `?` */
  char        id_quote;  /* 标识符的引号：`"` 或 `` ` `` */
} db_dialect;

static const db_dialect k_sqlite = {
  "sqlite",
  "CREATE TABLE IF NOT EXISTS capi_t ("
  "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, n INTEGER, d REAL, b BLOB)",
  0,
  '"'
};

static const db_dialect k_mysql = {
  "mysql",
  "CREATE TABLE IF NOT EXISTS capi_t ("
  "id BIGINT AUTO_INCREMENT PRIMARY KEY, name TEXT, n BIGINT, d DOUBLE, b BLOB)",
  0,
  '`'
};

static const db_dialect k_postgres = {
  "postgres",
  "CREATE TABLE IF NOT EXISTS capi_t ("
  "id BIGSERIAL PRIMARY KEY, name TEXT, n BIGINT, d DOUBLE PRECISION, b BYTEA)",
  1,
  '"'
};

/** @brief 第 n 个（1 起）占位符，写进 `out`。 */
static void ph(const db_dialect* d, char* out, size_t cap, int n) {
  if (d->dollar_ph) {
    snprintf(out, cap, "$%d", n);
  } else {
    snprintf(out, cap, "?");
  }
}

/** @brief 在一个块里声明 `_p1` …：`PH_BUF(1)` 给出第 1 个占位符的字符串。 */
#define PH_BUF(d, n)                                                     \
  char _p##n[16];                                                        \
  ph((d), _p##n, sizeof(_p##n), (n))

/** @brief 一张两列的池子/异步表，三个后端的自增主键写法。 */
static void mk_two_col_ddl(const db_dialect* d, const char* name, char* out,
                           size_t cap) {
  if (d->dollar_ph) {
    snprintf(out, cap,
             "CREATE TABLE %s (id BIGSERIAL PRIMARY KEY, name TEXT)", name);
  } else if (strcmp(d->name, "mysql") == 0) {
    snprintf(out, cap,
             "CREATE TABLE %s (id BIGINT AUTO_INCREMENT PRIMARY KEY, name TEXT)",
             name);
  } else {
    snprintf(out, cap,
             "CREATE TABLE %s (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)",
             name);
  }
}

/* -------------------------------------------------------------------------
 * 临时目录
 * -------------------------------------------------------------------------
 * CI 的三个平台都有 TMPDIR / TEMP / TMP 之一，兜底给 /tmp（与
 * `tests/functional/db_sqlite_func.cpp` 的 `temp_dir()` 同一条）。
 * ------------------------------------------------------------------------- */

static const char* temp_dir(void) {
  static const char* keys[] = {"TMPDIR", "TEMP", "TMP"};
  size_t i;
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    const char* v = getenv(keys[i]);
    if (v != NULL && v[0] != '\0') return v;
  }
  return "/tmp";
}

/* -------------------------------------------------------------------------
 * 1. 地基：状态码、驱动清单、句柄生死
 * ------------------------------------------------------------------------- */

static void test_status_and_drivers(void) {
  char buf[512];
  int rc;

  /* 状态码文案：这几格都是"手抄会抄错"的那种，所以它在用例里。 */
  CHECK_STR(uvcpp_c_db_status_name(UVCPP_C_DB_OK), "ok");
  CHECK_STR(uvcpp_c_db_status_name(UVCPP_C_DB_NO_CONNECTION), "no_connection");
  CHECK_STR(uvcpp_c_db_status_name(UVCPP_C_DB_NOT_CONNECTED), "not_connected");
  /* 不认识的数给 "unknown"，**不是 NULL** —— C# 侧 `Marshal.PtrToStringAnsi`
   * 拿到 NULL 会得到一个空串或者抛，两者都比这里该发生的更糟。 */
  CHECK_STR(uvcpp_c_db_status_name(9999), "unknown");
  CHECK_STR(uvcpp_c_db_value_type_name(NULL), "unknown");

  /* `_drivers()` 是这个构建编进了哪些后端的清单，至少得有其中一个。 */
  rc = uvcpp_c_db_drivers(buf, sizeof(buf));
  CHECK(rc > 0);
  CHECK(strstr(buf, "sqlite") != NULL || strstr(buf, "mysql") != NULL ||
        strstr(buf, "postgres") != NULL);

  /* 缓冲区约定：cap = 0 时它仍然报出真实长度（装不下，但不撒谎）。 */
  CHECK_INT(uvcpp_c_db_drivers(NULL, 0), rc);

  /* ---- 句柄生死：全是错误码，不是崩溃 ---- */

  /* 陌生指针：查的是登记表，不问那块内存（`uvcpp_c_internal.h` 里 `alive()`
   * 那一段的顺序），所以下面这几条**不会**段错误。 */
  CHECK_INT(uvcpp_c_db_client_free((uvcpp_c_db_client*)(void*)&g_checks),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_params_free((uvcpp_c_db_params*)(void*)&g_checks),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_table_free((uvcpp_c_db_table*)(void*)&g_checks),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_pool_free((uvcpp_c_db_pool*)(void*)&g_checks),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_async_free((uvcpp_c_db_async*)(void*)&g_checks),
            UVCPP_C_E_STALE);

  /* NULL 也是"失效句柄"，不是崩溃。 */
  CHECK_INT(uvcpp_c_db_client_free(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_params_count(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_table_row_count(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_table_column_count(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_pool_size(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_db_pool_last_error(NULL, buf, sizeof(buf)),
            UVCPP_C_E_STALE);

  /* 值的读函数是唯一一组**不查登记表**的（值视图本来就是借来的，没有魔数），
   * 所以它们的 NULL 行为单独钉一遍。 */
  CHECK_INT(uvcpp_c_db_value_type(NULL), UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_db_value_is_null(NULL), UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_db_value_size(NULL), UVCPP_C_E_INVALID_ARG);
  /* 这一组里"值本身是空的"与"指针是空的"给的东西不一样：前者是 0 / 空串，
   * 后者是错误码。混起来的话调用方就没法把"这一格没有值"与"我传错了"分开。 */
  CHECK_INT(uvcpp_c_db_value_to_int64(NULL), 0);
  CHECK_INT(uvcpp_c_db_value_to_bool(NULL), 0);
  CHECK_INT(uvcpp_c_db_value_to_double(NULL), 0.0);
  CHECK_INT(uvcpp_c_db_value_to_text(NULL, buf, sizeof(buf)),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_db_value_bytes(NULL, NULL, NULL), UVCPP_C_E_INVALID_ARG);
  /* 越界的那一格交出来的**不是 NULL**，是一枚静态 NULL 值视图 —— 于是
   * "这一格没有"在 C 侧就是 `_is_null()` 为真，不需要第二条分支。连句柄
   * 失效时也一样（没有表可查，但仍然给得出一枚可读的视图）。 */
  CHECK(uvcpp_c_db_table_cell(NULL, 0, 0) != NULL);
  CHECK_INT(uvcpp_c_db_value_is_null(uvcpp_c_db_table_cell(NULL, 0, 0)), 1);
}

/* -------------------------------------------------------------------------
 * 2. 参数集
 * ------------------------------------------------------------------------- */

static void test_params(void) {
  uvcpp_c_db_params* p = uvcpp_c_db_params_new();
  const char blob[] = {0x00, 0x01, (char)0xff};
  CHECK(p != NULL);
  if (p == NULL) return;

  CHECK_INT(uvcpp_c_db_params_count(p), 0);

  CHECK_INT(uvcpp_c_db_params_add_null(p), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_int64(p, -42), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_uint64(p, 42u), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_double(p, 1.5), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_bool(p, 1), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_text(p, "bob", 3), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_blob(p, blob, sizeof(blob)), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_count(p), 7);

  /* `NULL + 0` 是**空串**（不是 NULL）—— `std::string(NULL, 0)` 是未定义行为，
   * 所以这一格必须是分开走的一条路，而它的行为要在这里钉住。 */
  CHECK_INT(uvcpp_c_db_params_add_text(p, NULL, 0), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_count(p), 8);

  CHECK_INT(uvcpp_c_db_params_clear(p), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_count(p), 0);
  /* 清空之后还能再加（`clear()` 不是 `free()`）。 */
  CHECK_INT(uvcpp_c_db_params_add_int64(p, 1), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_count(p), 1);

  CHECK_INT(uvcpp_c_db_params_free(p), UVCPP_C_OK);
  /* 二次 free：错误码，不是崩溃。 */
  CHECK_INT(uvcpp_c_db_params_free(p), UVCPP_C_E_STALE);
}

/* -------------------------------------------------------------------------
 * 3. 连接 + 同步读写 + 事务 + 元信息
 * ------------------------------------------------------------------------- */

static void test_sync(const char* url, const db_dialect* d) {
  char buf[1024];
  char sql[512];
  uvcpp_c_db_client* c = uvcpp_c_db_client_new();
  uvcpp_c_db_params* ps = NULL;
  int rc;

  printf("  --- 同步：%s\n", d->name);
  CHECK(c != NULL);
  if (c == NULL) return;

  /* ---- 打开 ---- */

  CHECK_INT(uvcpp_c_db_client_is_open(c), 0);
  rc = uvcpp_c_db_client_open(c, url);
  if (rc != UVCPP_C_DB_OK) {
    printf("FAIL %s:%d: open(%s) → %d（%s）\n", __FILE__, __LINE__, url, rc,
           uvcpp_c_db_status_name(rc));
    ++g_failed;
    ++g_checks;
    uvcpp_c_db_client_free(c);
    return;
  }
  CHECK_INT(uvcpp_c_db_client_is_open(c), 1);
  /* 名字与连接串是"写进缓冲区 + 返回长度"的形状，不是返回指针 —— 这一片里
   * 凡是要交出字符串的都走这一套（C 侧没有"借来的 std::string"这回事）。 */
  CHECK_INT(uvcpp_c_db_client_driver_name(c, buf, sizeof(buf)),
            (int)strlen(d->name));
  CHECK_STR(buf, d->name);
  /* 驱动名是**静态**字符串：给 NULL 缓冲区时它照样报长度，不要释放。 */
  CHECK_INT(uvcpp_c_db_client_driver_name(c, NULL, 0), (int)strlen(d->name));
  CHECK_INT(uvcpp_c_db_client_url(c, buf, sizeof(buf)), (int)strlen(url));
  CHECK_STR(buf, url);
  CHECK_INT(uvcpp_c_db_client_ping(c), UVCPP_C_DB_OK);

  /* 打不开的连接串：`BAD_URL` 与 `NO_DRIVER` 要分得开 —— 前者是连接串写错了，
   * 后者是"这个后端存在，但这份包没把它编进来"，换份包就好。 */
  {
    uvcpp_c_db_client* bad = uvcpp_c_db_client_new();
    char avail[256];
    CHECK_INT(uvcpp_c_db_client_open(bad, "this-is-not-a-url"),
              UVCPP_C_DB_BAD_URL);
    CHECK_INT(uvcpp_c_db_client_open(bad, "nosuchdriver://x/y"),
              UVCPP_C_DB_BAD_URL);

    /* `NO_DRIVER` 只能拿"**认识、但这份包里没有**"的后端来量。写死一个后端名
     * 就会让每条腿的期望不一样（全开的树上它反而开得成），所以先问清楚这份库
     * 编进了哪些，再挑一个缺的。三个都编进来了就跳过这一格 —— 那时没有合法的
     * `NO_DRIVER` 输入，跳过是如实的，不是掩盖。 */
    CHECK(uvcpp_c_db_drivers(avail, sizeof(avail)) > 0);
    {
      const char* probe = NULL;
      if (strstr(avail, "postgres") == NULL) {
        probe = "postgres://u@127.0.0.1:5432/capi_probe";
      } else if (strstr(avail, "mysql") == NULL) {
        probe = "mysql://root@127.0.0.1:3306/capi_probe";
      } else if (strstr(avail, "sqlite") == NULL) {
        probe = "sqlite:///tmp/capi_probe.sqlite";
      }
      if (probe != NULL) {
        CHECK_INT(uvcpp_c_db_client_open(bad, probe), UVCPP_C_DB_NO_DRIVER);
      } else {
        printf("  （这份库三个后端都编进来了，`NO_DRIVER` 没有合法输入可量）\n");
      }
    }

    /* 失败之后句柄还在手上，而且能问出原因 —— 这是本片相对 C++ 那枚
     * `uvcpp_db_open()`（失败即销毁）唯一一处刻意的形状差别。 */
    CHECK(uvcpp_c_db_client_last_error(bad, buf, sizeof(buf)) > 0);
    CHECK_INT(uvcpp_c_db_client_free(bad), UVCPP_C_OK);
  }

  /* ---- DDL ---- */

  snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS capi_t");
  CHECK_INT(uvcpp_c_db_client_execute(c, sql, strlen(sql), NULL, NULL),
            UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_client_execute(c, d->ddl, strlen(d->ddl), NULL, NULL),
            UVCPP_C_DB_OK);

  /* 空 SQL 是 MISUSE（`n == 0`）；`sql == NULL && n > 0` 是参数不合法。这两格
   * **必须分得开** —— 前者是"这条语句是空的"，后者是调用方把长度传错了，处置
   * 办法完全不同。 */
  CHECK_INT(uvcpp_c_db_client_execute(c, "x", 0, NULL, NULL),
            UVCPP_C_DB_MISUSE);
  CHECK_INT(uvcpp_c_db_client_execute(c, NULL, 5, NULL, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_db_client_query(c, "SELECT 1", 8, NULL, NULL),
            UVCPP_C_E_INVALID_ARG);

  /* ---- 参数化 INSERT + 自增 ---- */

  ps = uvcpp_c_db_params_new();
  CHECK_INT(uvcpp_c_db_params_add_text(ps, "bob", 3), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_int64(ps, 42), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_double(ps, 1.5), UVCPP_C_DB_OK);
  {
    const char blob[] = {0x00, 0x01, 0x02};
    CHECK_INT(uvcpp_c_db_params_add_blob(ps, blob, sizeof(blob)), UVCPP_C_DB_OK);
  }

  {
    PH_BUF(d, 1);
    PH_BUF(d, 2);
    PH_BUF(d, 3);
    PH_BUF(d, 4);
    snprintf(sql, sizeof(sql),
             "INSERT INTO capi_t (name, n, d, b) VALUES (%s, %s, %s, %s)", _p1,
             _p2, _p3, _p4);
  }
  {
    int64_t id = -1;
    int has_id = -1;
    rc = uvcpp_c_db_client_insert(c, sql, strlen(sql), ps, &id, &has_id);
    CHECK_INT(rc, UVCPP_C_DB_OK);
    /* **"插进去了"与"拿不到 id"是两件事**：后端报不出自增时返回码仍是 OK，
     * 而 `has_id` 给 0（此时 `id` 是 0）。 */
    if (has_id == 1) {
      CHECK(id > 0);
    } else {
      printf("  （%s 这次没报出自增 id —— 那是允许的，返回码仍是 OK）\n",
             d->name);
    }
    /* 返回值与 `_last_insert_id()` 说的必须是同一件事。 */
    {
      const uvcpp_c_db_value* v = NULL;
      rc = uvcpp_c_db_client_last_insert_id(c, &v);
      if (rc == UVCPP_C_DB_OK) {
        CHECK(v != NULL);
        CHECK_INT(uvcpp_c_db_value_is_null(v), has_id == 1 ? 0 : 1);
      } else {
        CHECK_INT(rc, UVCPP_C_DB_UNSUPPORTED);
      }
    }
  }

  /* ---- SELECT + 逐格读 ---- */

  CHECK_INT(uvcpp_c_db_params_clear(ps), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_params_add_text(ps, "bob", 3), UVCPP_C_DB_OK);
  {
    PH_BUF(d, 1);
    snprintf(sql, sizeof(sql),
             "SELECT id, name, n, d, b FROM capi_t WHERE name = %s", _p1);
  }

  {
    uvcpp_c_db_table* t = NULL;
    rc = uvcpp_c_db_client_query(c, sql, strlen(sql), ps, &t);
    CHECK_INT(rc, UVCPP_C_DB_OK);
    CHECK(t != NULL);
    if (t != NULL) {
      CHECK_INT(uvcpp_c_db_table_row_count(t), 1);
      CHECK_INT(uvcpp_c_db_table_column_count(t), 5);

      /* 列名与列下标。**"没有这一列"与"第 0 列"分得开**：前者是 -20010，
       * 后者是 0 —— 拿 0 表示"没有"就是撒谎。 */
      CHECK_TEXT(uvcpp_c_db_table_column_name(t, 1, _buf, sizeof(_buf)), "name");
      CHECK_INT(uvcpp_c_db_table_column_index(t, "name"), 1);
      CHECK_INT(uvcpp_c_db_table_column_index(t, "n"), 2);
      CHECK_INT(uvcpp_c_db_table_column_index(t, "no_such_column"),
                UVCPP_C_E_NOT_FOUND);
      /* 列下标越界是参数不合法，不是崩溃。 */
      CHECK_INT(uvcpp_c_db_table_column_name(t, 99, buf, sizeof(buf)),
                UVCPP_C_E_INVALID_ARG);

      /* 文本列 */
      {
        const uvcpp_c_db_value* v = uvcpp_c_db_table_cell(t, 0, 1);
        CHECK_INT(uvcpp_c_db_value_type(v), UVCPP_C_DB_TEXT);
        CHECK_STR(uvcpp_c_db_value_type_name(v), "text");
        CHECK_INT(uvcpp_c_db_value_is_null(v), 0);
        CHECK_INT(uvcpp_c_db_value_size(v), 3);
        CHECK_TEXT(uvcpp_c_db_value_to_text(v, _buf, sizeof(_buf)), "bob");
        CHECK_INT(uvcpp_c_db_value_to_int64(v), 0); /* 类型不合适给 0，不炸 */
      }
      /* 整型列 */
      {
        const uvcpp_c_db_value* v = uvcpp_c_db_table_cell(t, 0, 2);
        CHECK_INT(uvcpp_c_db_value_to_int64(v), 42);
        CHECK_INT(uvcpp_c_db_value_to_uint64(v), 42);
        CHECK_INT(uvcpp_c_db_value_to_bool(v), 1);
        /* 数值型没有"原始字节"（`set_int64` 会清掉 text_）—— 要文本用
         * `_to_text()`。这一条区分了"数值 42"与"文本 \"42\""。 */
        CHECK_INT(uvcpp_c_db_value_size(v), 0);
        CHECK_TEXT(uvcpp_c_db_value_to_text(v, _buf, sizeof(_buf)), "42");
      }
      /* 浮点列 */
      {
        const uvcpp_c_db_value* v = uvcpp_c_db_table_cell(t, 0, 3);
        double got = uvcpp_c_db_value_to_double(v);
        CHECK(got > 1.49 && got < 1.51);
        CHECK_INT(uvcpp_c_db_value_to_bool(v), 1); /* 非零为真 */
      }
      /* blob 列：**二进制安全**，三个字节里有一个是 0 —— 用 `strlen` 量就会
       * 得到 0。所以这里按 `len` 取。 */
      {
        const uvcpp_c_db_value* v = uvcpp_c_db_table_cell(t, 0, 4);
        const char* bytes = NULL;
        size_t len = 0;
        CHECK_INT(uvcpp_c_db_value_type(v), UVCPP_C_DB_BLOB);
        CHECK_INT(uvcpp_c_db_value_size(v), 3);
        CHECK_INT(uvcpp_c_db_value_bytes(v, &bytes, &len), UVCPP_C_DB_OK);
        CHECK_INT((int)len, 3);
        if (bytes != NULL && len == 3) {
          CHECK_INT((unsigned char)bytes[0], 0x00);
          CHECK_INT((unsigned char)bytes[1], 0x01);
          CHECK_INT((unsigned char)bytes[2], 0x02);
        }
        /* 出参指针为 NULL 是参数不合法。 */
        CHECK_INT(uvcpp_c_db_value_bytes(v, NULL, &len),
                  UVCPP_C_E_INVALID_ARG);
      }
      /* 越界的那一格：静态 NULL 视图，`_is_null()` 为真。 */
      {
        const uvcpp_c_db_value* v = uvcpp_c_db_table_cell(t, 99, 99);
        CHECK(v != NULL);
        CHECK_INT(uvcpp_c_db_value_is_null(v), 1);
        CHECK_INT(uvcpp_c_db_value_type(v), UVCPP_C_DB_NIL);
        /* 值是空的、但类型名照样有（"nil"）—— "没有名字"是**句柄**失效/空指针
         * 才给的东西（见 test_status_and_drivers 那一条）。 */
        CHECK_STR(uvcpp_c_db_value_type_name(v), "nil");
        CHECK_INT(uvcpp_c_db_value_size(v), 0);
        /* 借用的是**同一枚**静态值：两次拿到的地址一样。 */
        CHECK(uvcpp_c_db_table_cell(t, 99, 99) == v);
      }

      /* 整张表的两条序列化出口。C# 侧一行拿到 JSON 比逐格 P/Invoke 快一个
       * 数量级，所以它值得一条断言（这里只量"里面有那一格"，格式细节归 C++
       * 那侧的用例）。 */
      {
        int need = uvcpp_c_db_table_to_json(t, buf, sizeof(buf));
        CHECK(need > 0);
        CHECK(strstr(buf, "bob") != NULL);
        /* 装不下时报的仍然是真实长度 —— 调用方据此扩容，一次到位。 */
        CHECK_INT(uvcpp_c_db_table_to_json(t, NULL, 0), need);
      }
      {
        int need = uvcpp_c_db_table_to_csv(t, buf, sizeof(buf));
        CHECK(need > 0);
        CHECK(strstr(buf, "bob") != NULL);
      }

      CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_OK);
      /* 二次 free：错误码，不是崩溃。 */
      CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_E_STALE);
    }
  }

  /* ---- 无参数的那条重载（`params == NULL`）---- */

  {
    uvcpp_c_db_table* t = NULL;
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM capi_t");
    rc = uvcpp_c_db_client_query(c, sql, strlen(sql), NULL, &t);
    CHECK_INT(rc, UVCPP_C_DB_OK);
    if (t != NULL) {
      CHECK_INT(uvcpp_c_db_table_row_count(t), 1);
      CHECK_INT(uvcpp_c_db_table_column_count(t), 1);
      /* SELECT 的影响行数就是结果行数。 */
      CHECK_INT((int)uvcpp_c_db_table_affected_rows(t), 1);
      CHECK(uvcpp_c_db_value_to_int64(uvcpp_c_db_table_cell(t, 0, 0)) >= 1);
      CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_OK);
    }
  }

  /* ---- 查询失败时 `out` **也是一张表**（不是 NULL）----
   * C 面没有 RAII，"什么时候要 free"多一种形状就是多一个漏点：所以拿到
   * `out` 就一律要 free，对错两种情况完全一样。 */
  {
    uvcpp_c_db_table* t = NULL;
    snprintf(sql, sizeof(sql), "SELECT * FROM no_such_table_xyz");
    rc = uvcpp_c_db_client_query(c, sql, strlen(sql), NULL, &t);
    CHECK(rc != UVCPP_C_DB_OK);
    CHECK(t != NULL); /* ← 承重的那一条 */
    if (t != NULL) {
      CHECK_INT(uvcpp_c_db_table_row_count(t), 0);
      CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_OK);
    }
    /* 失败原因问得出来。 */
    CHECK(uvcpp_c_db_client_last_error(c, buf, sizeof(buf)) > 0);
  }

  /* ---- 事务：rollback 之后查不到 ---- */

  {
    uvcpp_c_db_table* t = NULL;
    int64_t n_affected = 0;
    CHECK_INT(uvcpp_c_db_client_begin(c), UVCPP_C_DB_OK);
    snprintf(sql, sizeof(sql), "INSERT INTO capi_t (name) VALUES ('tmp_roll')");
    CHECK_INT(uvcpp_c_db_client_execute(c, sql, strlen(sql), NULL, &n_affected),
              UVCPP_C_DB_OK);
    CHECK_INT((int)n_affected, 1);
    CHECK_INT(uvcpp_c_db_client_rollback(c), UVCPP_C_DB_OK);

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM capi_t WHERE name = 'tmp_roll'");
    CHECK_INT(uvcpp_c_db_client_query(c, sql, strlen(sql), NULL, &t),
              UVCPP_C_DB_OK);
    if (t != NULL) {
      CHECK_INT((int)uvcpp_c_db_value_to_int64(uvcpp_c_db_table_cell(t, 0, 0)),
                0);
      CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_OK);
    }
  }

  /* ---- 事务：commit 之后查得到 ---- */

  {
    uvcpp_c_db_table* t = NULL;
    CHECK_INT(uvcpp_c_db_client_begin(c), UVCPP_C_DB_OK);
    snprintf(sql, sizeof(sql), "INSERT INTO capi_t (name) VALUES ('tmp_keep')");
    CHECK_INT(uvcpp_c_db_client_execute(c, sql, strlen(sql), NULL, NULL),
              UVCPP_C_DB_OK);
    CHECK_INT(uvcpp_c_db_client_commit(c), UVCPP_C_DB_OK);

    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM capi_t WHERE name = 'tmp_keep'");
    CHECK_INT(uvcpp_c_db_client_query(c, sql, strlen(sql), NULL, &t),
              UVCPP_C_DB_OK);
    if (t != NULL) {
      CHECK_INT((int)uvcpp_c_db_value_to_int64(uvcpp_c_db_table_cell(t, 0, 0)),
                1);
      CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_OK);
    }
  }

  /* ---- 元信息：表清单、表结构、转义 ---- */

  {
    int need = uvcpp_c_db_client_table_names(c, buf, sizeof(buf));
    CHECK(need > 0);
    CHECK(strstr(buf, "capi_t") != NULL);
  }
  {
    /* 表结构是一张普通的结果集 —— 同一套读函数。 */
    uvcpp_c_db_table* s = NULL;
    CHECK_INT(uvcpp_c_db_client_table_schema(c, "capi_t", &s), UVCPP_C_DB_OK);
    CHECK(s != NULL);
    if (s != NULL) {
      CHECK(uvcpp_c_db_table_row_count(s) >= 5); /* 五列 */
      CHECK_INT(uvcpp_c_db_table_free(s), UVCPP_C_OK);
    }
    CHECK_INT(uvcpp_c_db_client_table_schema(c, NULL, &s),
              UVCPP_C_E_INVALID_ARG);
  }
  /* 转义：只量"它真的动了手"，不量具体形状 —— 三家后端的转义法不一样
   * （PG 会加 `E''` 前缀、SQLite/MySQL 翻倍引号），钉死其中一种就是拿 SQLite
   * 的习惯去要求另外两个。承重的是"拼出来的东西和原串不一样、且原文还在里面"。 */
  {
    int need = uvcpp_c_db_client_escape(c, "o'brien", 7, buf, sizeof(buf));
    CHECK(need >= 7);
    CHECK(strcmp(buf, "o'brien") != 0);
    CHECK(strstr(buf, "brien") != NULL);
    /* 引号一个都没漏过去：原文里 1 个 `'`，转义之后不该还是 1 个。 */
    CHECK(buf[0] != '\'');
  }
  {
    int need =
        uvcpp_c_db_client_escape_identifier(c, "capi_t", 6, buf, sizeof(buf));
    CHECK(need >= 6);
    CHECK(strstr(buf, "capi_t") != NULL);
    CHECK(strcmp(buf, "capi_t") != 0); /* 括起来了 */
    CHECK(buf[0] == d->id_quote);      /* 而且用的是这个方言的引号 */
  }

  /* ---- 收尾：关掉 ---- */

  CHECK_INT(uvcpp_c_db_client_close(c), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_client_is_open(c), 0);
  CHECK_INT(uvcpp_c_db_client_ping(c), UVCPP_C_DB_NOT_CONNECTED);
  /* 关掉之后还能再开（`open()` 会重建驱动）。 */
  CHECK_INT(uvcpp_c_db_client_open(c, url), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_client_is_open(c), 1);
  CHECK_INT(uvcpp_c_db_client_reconnect(c), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_client_set_timeout_ms(c, 7000), UVCPP_C_DB_OK);

  CHECK_INT(uvcpp_c_db_params_free(ps), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_db_client_free(c), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_db_client_free(c), UVCPP_C_E_STALE);
}

/* -------------------------------------------------------------------------
 * 4. 连接池
 * ------------------------------------------------------------------------- */

static void test_pool(const char* url, const db_dialect* d) {
  char buf[512];
  char sql[512];
  char ddl[256];
  uvcpp_c_db_pool* pool = uvcpp_c_db_pool_new();
  uvcpp_c_db_client* a = NULL;
  uvcpp_c_db_client* b = NULL;
  uvcpp_c_db_table* t = NULL;
  uint64_t created = 0;
  int rc;

  printf("  --- 池子：%s\n", d->name);
  CHECK(pool != NULL);
  if (pool == NULL) return;

  /* 还没 `init` 就借：`MISUSE`（用法错），**不是** `NO_CONNECTION`（那是"该
   * 扩容"）—— 这两格分开报是承重的，池子的 `last_error()` 也只有这时候有用。 */
  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM capi_t");
  CHECK_INT(uvcpp_c_db_pool_query(pool, sql, strlen(sql), NULL, &t),
            UVCPP_C_DB_MISUSE);
  CHECK(t != NULL); /* `out` 永远是一枚要 free 的句柄，对错两种情况一样 */
  CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_OK);
  t = NULL;

  /* min=1 / max=2：`init()` **立刻**开 min 条（失败当场报出来，不留到第一次
   * 查询上 —— `open` 一次 1.9~2.4 ms，把它藏起来不等于消掉它）。 */
  rc = uvcpp_c_db_pool_init(pool, url, 1, 2);
  if (rc != UVCPP_C_DB_OK) {
    printf("FAIL %s:%d: pool init(%s) → %d（%s）\n", __FILE__, __LINE__, url, rc,
           uvcpp_c_db_status_name(rc));
    ++g_failed;
    ++g_checks;
    uvcpp_c_db_pool_free(pool);
    return;
  }
  CHECK_INT(uvcpp_c_db_pool_min_size(pool), 1);
  CHECK_INT(uvcpp_c_db_pool_max_size(pool), 2);
  CHECK_INT(uvcpp_c_db_pool_size(pool), 1);
  CHECK_INT(uvcpp_c_db_pool_in_use(pool), 0);
  CHECK_INT(uvcpp_c_db_pool_idle(pool), 1);
  CHECK_INT(uvcpp_c_db_pool_ping(pool), UVCPP_C_DB_OK);

  /* 代借代还：建表 + 插 + 查（每个语句一条连接，用完立刻还）。 */
  snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS capi_pool_t");
  CHECK_INT(uvcpp_c_db_pool_execute(pool, sql, strlen(sql), NULL, NULL),
            UVCPP_C_DB_OK);
  mk_two_col_ddl(d, "capi_pool_t", ddl, sizeof(ddl));
  CHECK_INT(uvcpp_c_db_pool_execute(pool, ddl, strlen(ddl), NULL, NULL),
            UVCPP_C_DB_OK);
  snprintf(sql, sizeof(sql), "INSERT INTO capi_pool_t (name) VALUES ('pooled')");
  CHECK_INT(uvcpp_c_db_pool_execute(pool, sql, strlen(sql), NULL, NULL),
            UVCPP_C_DB_OK);
  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM capi_pool_t");
  t = NULL;
  CHECK_INT(uvcpp_c_db_pool_query(pool, sql, strlen(sql), NULL, &t),
            UVCPP_C_DB_OK);
  if (t != NULL) {
    CHECK_INT((int)uvcpp_c_db_value_to_int64(uvcpp_c_db_table_cell(t, 0, 0)), 1);
    CHECK_INT(uvcpp_c_db_table_free(t), UVCPP_C_OK);
  }
  /* 上面四件活共用了一条连接（max 没被顶到），所以池子仍然只有 1 条。 */
  CHECK_INT(uvcpp_c_db_pool_size(pool), 1);

  /* 借还：借到还之间那条连接归你独占 —— 事务只能这么写。 */
  CHECK_INT(uvcpp_c_db_pool_acquire(pool, 1000, &a), UVCPP_C_DB_OK);
  CHECK(a != NULL);
  if (a != NULL) {
    uvcpp_c_db_table* tt = NULL;
    CHECK_INT(uvcpp_c_db_pool_in_use(pool), 1);
    /* **借来的句柄不能拿去 `_client_free()`** —— 它属于池子。 */
    CHECK_INT(uvcpp_c_db_client_free(a), UVCPP_C_E_STATE);
    /* 但 `acquire` 出来的那条**就是一条普通连接**：事务、查询都能用。 */
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM capi_pool_t");
    CHECK_INT(uvcpp_c_db_client_query(a, sql, strlen(sql), NULL, &tt),
              UVCPP_C_DB_OK);
    if (tt != NULL) CHECK_INT(uvcpp_c_db_table_free(tt), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_db_client_begin(a), UVCPP_C_DB_OK);
    CHECK_INT(uvcpp_c_db_client_rollback(a), UVCPP_C_DB_OK);
  }

  /* 借到上限（max=2）之后再借：等到超时，拿 `NO_CONNECTION`（**不是挂住**）。
   * 这一条是"池线程不会互相等死"那层防线在**同步**侧的见证。 */
  CHECK_INT(uvcpp_c_db_pool_acquire(pool, 1000, &b), UVCPP_C_DB_OK);
  CHECK(b != NULL);
  CHECK_INT(uvcpp_c_db_pool_size(pool), 2);
  CHECK_INT(uvcpp_c_db_pool_in_use(pool), 2);
  CHECK_INT(uvcpp_c_db_pool_idle(pool), 0);
  {
    uvcpp_c_db_client* c3 = NULL;
    /* 100 ms 就够判定：池子满了，等多久都是满的，这里量的只是"不会挂死"。 */
    rc = uvcpp_c_db_pool_acquire(pool, 100, &c3);
    CHECK_INT(rc, UVCPP_C_DB_NO_CONNECTION);
    CHECK(c3 == NULL);
    /* 借不出时原因问得出来（`last_error()` 返回**拷贝**：池子是多线程的，
     * 交出一枚内部引用就是给别人一个随时会变的指针）。 */
    CHECK(uvcpp_c_db_pool_last_error(pool, buf, sizeof(buf)) >= 0);
  }

  /* 观测：`created_total` 停在 2（= max）上，而不是随调用次数上涨 —— 这就是
   * "连接是**复用**的，不是每次新开"的见证。 */
  CHECK_INT(uvcpp_c_db_pool_created_total(pool, &created), UVCPP_C_DB_OK);
  CHECK_INT((int)created, 2);
  {
    uint64_t reused = 0;
    CHECK_INT(uvcpp_c_db_pool_reused_total(pool, &reused), UVCPP_C_DB_OK);
    CHECK(reused >= 1);
  }

  /* 还回去：`created_total` 不变，`in_use` 减一。 */
  CHECK_INT(uvcpp_c_db_pool_release(pool, a), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_pool_in_use(pool), 1);
  /* 还过之后那枚句柄是**失效**的（池子反登记并删掉了它）—— 再用它是错误码，
   * 不是野指针。 */
  CHECK_INT(uvcpp_c_db_pool_release(pool, a), UVCPP_C_E_STALE);

  /* 不是这个池子借出的句柄：`E_INVALID_ARG`，而且**不会**把它错扔掉。 */
  {
    uvcpp_c_db_client* own = uvcpp_c_db_client_new();
    CHECK_INT(uvcpp_c_db_pool_release(pool, own), UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_db_pool_discard(pool, own), UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_db_client_free(own), UVCPP_C_OK); /* 它还活着 */
    CHECK_INT(uvcpp_c_db_pool_release(pool, NULL), UVCPP_C_E_INVALID_ARG);
  }

  /* `discard`：坏掉的那条不回池子（额度让给新开的）。b 是好的，但接口语义
   * 一样 —— 借出的额度减一，池子里的连接数减一。 */
  CHECK_INT(uvcpp_c_db_pool_discard(pool, b), UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_pool_in_use(pool), 0);
  CHECK_INT(uvcpp_c_db_pool_size(pool), 1); /* 被丢掉了，没还回去 */
  CHECK_INT(uvcpp_c_db_pool_discard(pool, b), UVCPP_C_E_STALE);

  /* 参数校验：`max == 0` 是 MISUSE；`min > max` 被夹到 max；url 是 NULL 是
   * 参数不合法。 */
  {
    uvcpp_c_db_pool* p2 = uvcpp_c_db_pool_new();
    CHECK_INT(uvcpp_c_db_pool_init(p2, url, 0, 0), UVCPP_C_DB_MISUSE);
    CHECK_INT(uvcpp_c_db_pool_init(p2, NULL, 1, 2), UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_db_pool_init(p2, url, 9, 3), UVCPP_C_DB_OK);
    CHECK_INT(uvcpp_c_db_pool_max_size(p2), 3);
    CHECK_INT(uvcpp_c_db_pool_min_size(p2), 3); /* 夹过之后 min == max == 3 */
    CHECK_INT(uvcpp_c_db_pool_size(p2), 3);
    /* `close_idle()` 收到 min 就停手 —— p2 是 min == max，一条都不该关。 */
    CHECK_INT((int)uvcpp_c_db_pool_close_idle(p2), 0);
    CHECK_INT(uvcpp_c_db_pool_size(p2), 3);
    CHECK_INT(uvcpp_c_db_pool_set_acquire_timeout_ms(p2, 250), UVCPP_C_DB_OK);
    CHECK_INT(uvcpp_c_db_pool_free(p2), UVCPP_C_OK);
  }

  /* 关掉池子，再借就一条都借不出（`MISUSE`：池子没在用了）。 */
  CHECK_INT(uvcpp_c_db_pool_close(pool), UVCPP_C_DB_OK);
  {
    uvcpp_c_db_client* c4 = NULL;
    CHECK_INT(uvcpp_c_db_pool_acquire(pool, 100, &c4), UVCPP_C_DB_MISUSE);
    CHECK(c4 == NULL);
  }
  CHECK_INT(uvcpp_c_db_pool_free(pool), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_db_pool_free(pool), UVCPP_C_E_STALE);

  /* 清场：池子那一节自己的表自己收拾（`capi_t` 留着给异步那一节用）。 */
  {
    uvcpp_c_db_client* c5 = uvcpp_c_db_client_new();
    if (uvcpp_c_db_client_open(c5, url) == UVCPP_C_DB_OK) {
      snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS capi_pool_t");
      (void)uvcpp_c_db_client_execute(c5, sql, strlen(sql), NULL, NULL);
    }
    CHECK_INT(uvcpp_c_db_client_free(c5), UVCPP_C_OK);
  }
}

/* -------------------------------------------------------------------------
 * 5. 异步
 * -------------------------------------------------------------------------
 * 交付是在**门面自带的那条循环线程**上发生的，所以主线程只能等。这里用
 * `uv_sem_t`：它在三个平台上都有（Windows 上是 `HANDLE`，`uv_thread_self()`
 * 把当前线程的句柄缓在 TLS 里，所以 `uv_thread_equal()` 在那边也成立），而
 * `uv_sem_wait()` 给的是一次真正的 happens-before —— 于是回调写进 `ctx` 的东西
 * 在主线程上读是安全的（不需要原子量，也不需要锁）。
 *
 * 门面的循环是**懒起**的：一条活都没投过就不起线程（所以 `_new()` 之后
 * `in_flight()` 是 0，而"起线程"这件事本身发生在第一次投递里）。
 * ------------------------------------------------------------------------- */

typedef struct async_ctx {
  uv_sem_t sem;
  int      called;
  int      status;
  int      rows;
  int      cols;
  int      table_free_rc;
  int64_t  affected;
  size_t   in_flight_in_cb; /* 回调里读到 1 ⇒ 这一笔自己还没收（账目没错位） */
  int      on_main_thread;  /* 回调**不该**在主线程上 —— 这一条量的是承诺 */
  int      free_in_cb_rc;   /* 回调里 `free()` 自己：应当是 E_STATE */
  int      got_null_table;  /* 失败时 `t` 是不是 NULL */
} async_ctx;

static uv_thread_t g_main_thread;
static uvcpp_c_db_async* g_facade = NULL;

static void ctx_init(async_ctx* c) {
  memset(c, 0, sizeof(*c));
  uv_sem_init(&c->sem, 0);
}

static void on_table_cb(void* ud, int status, uvcpp_c_db_table* t) {
  async_ctx* c = (async_ctx*)ud;
  uv_thread_t self = uv_thread_self();

  c->called += 1;
  c->status = status;
  c->got_null_table = (t == NULL) ? 1 : 0;
  c->on_main_thread = (uv_thread_equal(&g_main_thread, &self) != 0) ? 1 : 0;
  c->in_flight_in_cb = uvcpp_c_db_async_in_flight(g_facade);
  if (t != NULL) {
    c->rows = (int)uvcpp_c_db_table_row_count(t);
    c->cols = (int)uvcpp_c_db_table_column_count(t);
    /* 回调返回之后这张表仍然有效（**归调用方**），所以这里顺手 free 掉 ——
     * 这正是 C# 侧 wrapper 会写的那一行。 */
    c->table_free_rc = uvcpp_c_db_table_free(t);
  }
  /* 在回调里 `free()` 自己：E_STATE，不是崩溃、也不是把自己当场拆了。 */
  c->free_in_cb_rc = uvcpp_c_db_async_free(g_facade);

  uv_sem_post(&c->sem);
}

static void on_executed_cb(void* ud, int status, int64_t affected) {
  async_ctx* c = (async_ctx*)ud;
  uv_thread_t self = uv_thread_self();

  c->called += 1;
  c->status = status;
  c->affected = affected;
  c->on_main_thread = (uv_thread_equal(&g_main_thread, &self) != 0) ? 1 : 0;
  c->in_flight_in_cb = uvcpp_c_db_async_in_flight(g_facade);
  uv_sem_post(&c->sem);
}

static void test_async(const char* url, const db_dialect* d) {
  char sql[512];
  char ddl[256];
  async_ctx ctx;
  uvcpp_c_db_query_events ev;
  uvcpp_c_db_client* c = uvcpp_c_db_client_new();
  uvcpp_c_db_async* a = NULL;
  int rc;

  printf("  --- 异步：%s\n", d->name);
  CHECK(c != NULL);
  if (c == NULL) return;

  rc = uvcpp_c_db_client_open(c, url);
  if (rc != UVCPP_C_DB_OK) {
    printf("FAIL %s:%d: open(%s) → %d\n", __FILE__, __LINE__, url, rc);
    ++g_failed;
    ++g_checks;
    uvcpp_c_db_client_free(c);
    return;
  }

  /* 建表 + 两行数据：全在**建门面之前**用同步接口做完（门面与同步接口不要
   * 混着用来"省时间"—— 那是头里写死的禁令，这里自己先照做）。 */
  snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS capi_async_t");
  CHECK_INT(uvcpp_c_db_client_execute(c, sql, strlen(sql), NULL, NULL),
            UVCPP_C_DB_OK);
  mk_two_col_ddl(d, "capi_async_t", ddl, sizeof(ddl));
  CHECK_INT(uvcpp_c_db_client_execute(c, ddl, strlen(ddl), NULL, NULL),
            UVCPP_C_DB_OK);
  snprintf(sql, sizeof(sql), "INSERT INTO capi_async_t (name) VALUES ('a')");
  CHECK_INT(uvcpp_c_db_client_execute(c, sql, strlen(sql), NULL, NULL),
            UVCPP_C_DB_OK);
  snprintf(sql, sizeof(sql), "INSERT INTO capi_async_t (name) VALUES ('b')");
  CHECK_INT(uvcpp_c_db_client_execute(c, sql, strlen(sql), NULL, NULL),
            UVCPP_C_DB_OK);

  /* ---- 建门面 ---- */

  g_facade = uvcpp_c_db_async_new(c);
  CHECK(g_facade != NULL);
  if (g_facade == NULL) {
    uvcpp_c_db_client_free(c);
    return;
  }
  a = g_facade;
  /* 懒起：一条活都没投过，线程还没起，在途是 0。 */
  CHECK_INT((int)uvcpp_c_db_async_in_flight(a), 0);
  /* **门面借着的 client free 不掉** —— 那门面的工作线程随时会去读这条连接。
   * 这是 C 面把"调用方的义务"变成一条判据的地方（C++ 那侧只能写在注释里）。 */
  CHECK_INT(uvcpp_c_db_client_free(c), UVCPP_C_E_STATE);

  memset(&ev, 0, sizeof(ev));
  ev.size = sizeof(ev);
  ev.on_table = on_table_cb;
  ev.on_executed = on_executed_cb;

  /* ---- 参数校验：当场拒掉的几种，回调**一次都不该**被调 ---- */

  ctx_init(&ctx);
  {
    uvcpp_c_db_query_events bad;
    memset(&bad, 0, sizeof(bad));
    bad.size = sizeof(bad);
    bad.on_table = NULL; /* 这一笔的交付通道没给 */
    CHECK_INT(uvcpp_c_db_async_query(a, "SELECT 1", 8, NULL, &bad, &ctx),
              UVCPP_C_E_INVALID_ARG);

    bad.on_table = on_table_cb;
    bad.size = 2u; /* 表太小，`on_table` 那一格读不到 */
    CHECK_INT(uvcpp_c_db_async_query(a, "SELECT 1", 8, NULL, &bad, &ctx),
              UVCPP_C_E_INVALID_ARG);

    /* `on_executed` 那条通道也一样。 */
    bad.size = offsetof(uvcpp_c_db_query_events, on_executed);
    bad.on_table = on_table_cb;
    CHECK_INT(uvcpp_c_db_async_execute(a, "SELECT 1", 8, NULL, &bad, &ctx),
              UVCPP_C_E_INVALID_ARG);

    CHECK_INT(uvcpp_c_db_async_query(a, NULL, 8, NULL, &ev, &ctx),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_db_async_query(a, "x", 0, NULL, &ev, &ctx),
              UVCPP_C_DB_MISUSE);
    CHECK_INT(uvcpp_c_db_async_execute(a, NULL, 8, NULL, &ev, &ctx),
              UVCPP_C_E_INVALID_ARG);

    /* 陌生句柄。 */
    CHECK_INT(uvcpp_c_db_async_query((uvcpp_c_db_async*)(void*)&g_checks,
                                     "SELECT 1", 8, NULL, &ev, &ctx),
              UVCPP_C_E_STALE);

    /* 以上全都没投出去：在途仍然是 0，回调一次都没发生。 */
    CHECK_INT((int)uvcpp_c_db_async_in_flight(a), 0);
    CHECK_INT(ctx.called, 0);
  }
  uv_sem_destroy(&ctx.sem);

  /* ---- 两条查询同时投出去（账目要平） ---- */

  ctx_init(&ctx);
  snprintf(sql, sizeof(sql), "SELECT id, name FROM capi_async_t ORDER BY id");
  CHECK_INT(uvcpp_c_db_async_query(a, sql, strlen(sql), NULL, &ev, &ctx), 0);
  CHECK_INT(uvcpp_c_db_async_query(a, sql, strlen(sql), NULL, &ev, &ctx), 0);
  /* 投出去之后在途至少是 1（可能已经跑完一条了，所以不比 2）。 */
  CHECK(uvcpp_c_db_async_in_flight(a) >= 1);
  uv_sem_wait(&ctx.sem);
  uv_sem_wait(&ctx.sem);
  uv_sem_destroy(&ctx.sem);

  CHECK_INT(ctx.called, 2);
  CHECK_INT(ctx.status, UVCPP_C_DB_OK);
  CHECK_INT(ctx.rows, 2);
  CHECK_INT(ctx.cols, 2);
  CHECK_INT(ctx.table_free_rc, UVCPP_C_OK);
  /* 回调**不在**主线程上（它在门面自带的循环线程上）—— 这是本片异步形状的
   * 全部承诺，所以它有用例钉着。 */
  CHECK_INT(ctx.on_main_thread, 0);
  /* 回调里读到 1：这一笔自己还没收（`free()` 之后才会等它清零）。 */
  CHECK_INT((int)ctx.in_flight_in_cb, 1);
  /* 在回调里 `free()` 自己：E_STATE。 */
  CHECK_INT(ctx.free_in_cb_rc, UVCPP_C_E_STATE);
  /* 活收完了，在途回到 0。 */
  CHECK_INT((int)uvcpp_c_db_async_in_flight(a), 0);

  /* ---- 带参数的那条重载（`params != NULL`）：占位符仍然由调用方按方言给 ---- */

  {
    uvcpp_c_db_params* ps = uvcpp_c_db_params_new();
    PH_BUF(d, 1);
    CHECK_INT(uvcpp_c_db_params_add_text(ps, "a", 1), UVCPP_C_DB_OK);
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM capi_async_t WHERE name = %s", _p1);
    ctx_init(&ctx);
    CHECK_INT(uvcpp_c_db_async_query(a, sql, strlen(sql), ps, &ev, &ctx), 0);
    uv_sem_wait(&ctx.sem);
    uv_sem_destroy(&ctx.sem);
    CHECK_INT(ctx.called, 1);
    CHECK_INT(ctx.status, UVCPP_C_DB_OK);
    CHECK_INT(ctx.rows, 1);
    CHECK_INT(ctx.cols, 1);
    /* 参数集是**投递时拷走**的，但这里仍然等回调收完才 free —— 交付一旦发生，
     * "什么时候能放手"就没有第二种答案了。 */
    CHECK_INT(uvcpp_c_db_params_free(ps), UVCPP_C_OK);
  }

  /* ---- 一条增删改：`on_executed` 拿到影响行数 ---- */

  ctx_init(&ctx);
  snprintf(sql, sizeof(sql),
           "UPDATE capi_async_t SET name = 'c' WHERE name = 'b'");
  CHECK_INT(uvcpp_c_db_async_execute(a, sql, strlen(sql), NULL, &ev, &ctx), 0);
  uv_sem_wait(&ctx.sem);
  uv_sem_destroy(&ctx.sem);
  CHECK_INT(ctx.called, 1);
  CHECK_INT(ctx.status, UVCPP_C_DB_OK);
  CHECK_INT((int)ctx.affected, 1);
  CHECK_INT(ctx.on_main_thread, 0);

  /* ---- 一条**失败**的查询：回调照调，status 非 0、表是 NULL ---- */

  ctx_init(&ctx);
  snprintf(sql, sizeof(sql), "SELECT * FROM no_such_table_xyz");
  /* "投没投出去"看返回值，"跑得怎么样"看回调 —— 这两件事不能混。 */
  CHECK_INT(uvcpp_c_db_async_query(a, sql, strlen(sql), NULL, &ev, &ctx), 0);
  uv_sem_wait(&ctx.sem);
  uv_sem_destroy(&ctx.sem);
  CHECK_INT(ctx.called, 1);
  CHECK(ctx.status != UVCPP_C_DB_OK);
  CHECK_INT(ctx.got_null_table, 1); /* 没表可交，就不造一张空的糊弄人 */

  /* ---- 拆掉门面 ---- */

  CHECK_INT(uvcpp_c_db_async_free(a), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_db_async_free(a), UVCPP_C_E_STALE);
  g_facade = NULL;
  /* 拆掉之后 client 就归我们了（借用关系解除了）。 */
  CHECK_INT(uvcpp_c_db_client_is_open(c), 1);

  /* ---- 池子那条路：门面绑池子，借还在池线程里做 ---- */

  {
    uvcpp_c_db_pool* pool = uvcpp_c_db_pool_new();
    uvcpp_c_db_async* pa = NULL;
    CHECK(pool != NULL);
    rc = uvcpp_c_db_pool_init(pool, url, 1, 2);
    CHECK_INT(rc, UVCPP_C_DB_OK);
    if (rc == UVCPP_C_DB_OK) {
      pa = uvcpp_c_db_async_new_pool(pool);
      CHECK(pa != NULL);
      if (pa != NULL) {
        g_facade = pa;
        /* **池子被门面绑着时不能 free** —— 那门面在池线程里借还连接，池子一没
         * 它就是在读已释放的内存。这是本片唯一一处"次序约束"必须报错而不是
         * 尽力而为的地方。 */
        CHECK_INT(uvcpp_c_db_pool_free(pool), UVCPP_C_E_STATE);

        ctx_init(&ctx);
        /* `capi_t` 是同步那一节建的（同一个库，池子的连接看得见它）。 */
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM capi_t");
        CHECK_INT(uvcpp_c_db_async_query(pa, sql, strlen(sql), NULL, &ev, &ctx),
                  0);
        uv_sem_wait(&ctx.sem);
        uv_sem_destroy(&ctx.sem);
        CHECK_INT(ctx.called, 1);
        CHECK_INT(ctx.status, UVCPP_C_DB_OK);
        CHECK_INT(ctx.rows, 1);
        CHECK_INT(ctx.table_free_rc, UVCPP_C_OK);
        CHECK_INT(ctx.on_main_thread, 0);
        CHECK_INT(ctx.free_in_cb_rc, UVCPP_C_E_STATE);

        /* 拆掉池子门面：池子又能 free 了。 */
        CHECK_INT(uvcpp_c_db_async_free(pa), UVCPP_C_OK);
        g_facade = NULL;
      }
      CHECK_INT(uvcpp_c_db_pool_free(pool), UVCPP_C_OK);
    } else {
      printf("FAIL %s:%d: pool init(%s) → %d\n", __FILE__, __LINE__, url, rc);
      ++g_failed;
      ++g_checks;
      uvcpp_c_db_pool_free(pool);
    }
  }

  /* ---- 收尾 ---- */

  snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS capi_async_t");
  CHECK_INT(uvcpp_c_db_client_execute(c, sql, strlen(sql), NULL, NULL),
            UVCPP_C_DB_OK);
  CHECK_INT(uvcpp_c_db_client_free(c), UVCPP_C_OK);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  char sqlite_url[600];
  char sqlite_path[512];
  const char* require = getenv("UVCPP_DB_TEST_REQUIRE");
  const char* mysql_url = getenv("UVCPP_DB_TEST_MYSQL_URL");
  const char* pgsql_url = getenv("UVCPP_DB_TEST_PGSQL_URL");
  const char* tmp = temp_dir();
  size_t tmp_len = strlen(tmp);
  size_t live_before;
  size_t live_after;

  g_main_thread = uv_thread_self();
  live_before = uvcpp_c_live_handle_count();

  printf("[capi db] start\n");

  /* 1. 地基（不连任何后端） */
  test_status_and_drivers();
  test_params();

  /* 2. 几个后端就几遍历整套断言 —— 覆盖面跟着**连接串**走，不跟着编译开关走。 */
  if (pgsql_url != NULL && pgsql_url[0] != '\0') {
    printf("[capi db] 后端：PostgreSQL（UVCPP_DB_TEST_PGSQL_URL）\n");
    test_sync(pgsql_url, &k_postgres);
    test_pool(pgsql_url, &k_postgres);
    test_async(pgsql_url, &k_postgres);
  }
  if (mysql_url != NULL && mysql_url[0] != '\0') {
    printf("[capi db] 后端：MySQL（UVCPP_DB_TEST_MYSQL_URL）\n");
    test_sync(mysql_url, &k_mysql);
    test_pool(mysql_url, &k_mysql);
    test_async(mysql_url, &k_mysql);
  }
#if UVCPP_DB_SQLITE_ENABLE
  {
    /* SQLite 用临时**文件**，不是 `:memory:`：后者每开一条连接就是另一个空库，
     * 池子与异步那两节会立刻垮掉（池子里每条连接都看不见别人建的表）。 */
    if (tmp_len > 0 && (tmp[tmp_len - 1] == '/' || tmp[tmp_len - 1] == '\\')) {
      snprintf(sqlite_path, sizeof(sqlite_path), "%suvcpp-capi-db-func.sqlite",
               tmp);
    } else {
      snprintf(sqlite_path, sizeof(sqlite_path), "%s/uvcpp-capi-db-func.sqlite",
               tmp);
    }
    snprintf(sqlite_url, sizeof(sqlite_url), "sqlite://%s", sqlite_path);
    remove(sqlite_path);
    printf("[capi db] 后端：SQLite（%s）\n", sqlite_url);
    test_sync(sqlite_url, &k_sqlite);
    test_pool(sqlite_url, &k_sqlite);
    test_async(sqlite_url, &k_sqlite);
    remove(sqlite_path);
  }
#endif

  /* 3. 活句柄收支平衡。这一条**不是**"二次 free 给错误码"的重复：那条量的是
   *    分配器的行为，只有这一条量得到**登记表**到底有没有在回收。 */
  live_after = uvcpp_c_live_handle_count();
  CHECK_INT((int)live_after, (int)live_before);

  printf("[capi db] checks=%d failures=%d\n", g_checks, g_failed);
  if (g_failed != 0) {
    printf("[capi db] FAILED: %d\n", g_failed);
    return 1;
  }
  printf("[capi db] PASS\n");
  return 0;
}
