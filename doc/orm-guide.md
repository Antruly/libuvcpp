# 单表 ORM 指南（1.12.0）

`src/orm/` 是建在现有 `db` 模块之上的**单表 ORM 核心**：用 C++11 的模型类型
声明表、列与单列主键，然后完成参数化的单表增删改查。它**不重新实现**连接、
协议、连接池或事务 —— 同步执行走 `uvcpp_db_client` / `uvcpp_db_pool` /
`uvcpp_db_tx`，异步执行复用 `uvcpp_db_pool` 已有的异步路径。

> **一句话说清它是什么**：一个把 `uvcpp_db_table` 与业务模型互相映射的薄层，
> 加上一个只生成单表 SQL 的构造器。SQL 里的值永远走 `uvcpp_db_params`，不拼字符串；
> 表名/列名只能来自模型声明。它**不是**完整 ORM：没有 JOIN、关联、迁移、复合主键、
> 代码生成或协程。

## 1. 打开方式与公开头

ORM 不是独立模块。它的值层复用 `uvcpp_db_value`，所以必须同时打开 DB：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ENABLE_DB=ON -DUVCPP_ENABLE_DB_SQLITE=ON \
  -DUVCPP_BUILD_ORM=ON
```

`UVCPP_BUILD_ORM=ON` 而 `UVCPP_ENABLE_DB=OFF` 时，配置期会打一条警告并**强制
关掉 ORM**（与 db 后端“找不到就降级并出声”同形），不会留下一个头文件存在、
类型却编不出来的组合。

公开头（安装到 `include/orm/`）：

| 头 | 内容 |
|---|---|
| `<orm/uvcpp_orm_value.h>` | `nullable<T>`、字段类型标签 `uvcpp_orm_type`、值转换 |
| `<orm/uvcpp_orm_model.h>` | `uvcpp_orm_column`、`model_traits<T>`、`UVCPP_ORM_MODEL` / `UVCPP_ORM_FIELD`、`validate_model<T>()` |
| `<orm/uvcpp_orm_dialect.h>` | `uvcpp_orm_dialect`、占位符与标识符转义 |
| `<orm/uvcpp_orm_error.h>` | `uvcpp_orm_status` 与返回码短名 |
| `<orm/uvcpp_orm_query.h>` | `select<T>()` / `insert<T>()` / `update<T>()` / `delete_from<T>()` 构造器与 `raw()` |
| `<orm/uvcpp_orm_repository.h>` | 同步 CRUD、COUNT、结果映射 |
| `<orm/uvcpp_orm_async.h>` | 回调式异步 CRUD（复用 `uvcpp_db_pool`） |

1.12.0 **不新增 C ABI 符号**。C 面保持 `uvcpp_c_*` 原有函数集合不变。

## 2. 声明模型

模型是**公开成员、无基类、无虚函数**的聚合形状。`UVCPP_ORM_MODEL` /
`UVCPP_ORM_FIELD` 必须写在**全局作用域**：

```cpp
#include <cstdint>
#include <string>

#include <orm/uvcpp_orm_model.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

bool check_model(std::string* error) {
  return uvcpp::validate_model<account>(error) ==
         uvcpp::uvcpp_orm_status::OK;
}
```

标志位：

| 标志 | 含义 |
|---|---|
| `UVCPP_ORM_PRIMARY_KEY` | 单列主键；1.12.0 必须有且只有一个 |
| `UVCPP_ORM_AUTO_INCREMENT` | 自增；只能标在整数主键上 |
| `UVCPP_ORM_NOT_NULL` | 非空字段；映射到 SQL NULL 时是 `NULL_VIOLATION` |
| `UVCPP_ORM_UNIQUE` | 元数据标记；ORM **不建表**，唯一约束由你的 DDL 负责 |
| `UVCPP_ORM_READ_ONLY` | 不进入 `insert`，也不进入 `update ... SET` |
| `UVCPP_ORM_NULLABLE` | 元数据标记；`nullable<T>` 字段会自动补上这一位 |
| `UVCPP_ORM_NONE` | 没有额外标志 |

支持的字段类型是**精确类型**，不做“差不多”的猜测：

| C++ 类型 | 数据库值标签 |
|---|---|
| `bool` | `BOOL` |
| `std::int8_t` … `std::int64_t` | `INT64` |
| `std::uint8_t` … `std::uint64_t` | `UINT64` |
| `float` / `double` | `DOUBLE` |
| `std::string` | `TEXT` |
| `std::vector<std::uint8_t>` | `BLOB` |
| `nullable<T>`（`T` 为以上任意一种） | 同 `T`，空态是 SQL NULL |

`char`、`long double`、自定义结构体都是 `UNKNOWN`，`validate_model<T>()` 会以
`UNSUPPORTED_TYPE` 拒绝。模型必须可默认构造 —— 结果映射先在一个栈上临时对象里
填字段，全部成功才写进 `out`。

## 3. 列名映射与标识符

映射按**列名**而不是列位置：结果集的列顺序可以与 `SELECT` 列表不同，模型字段的
声明顺序也不影响取值。字段名与列名可以不同：

```cpp
#include <cstdint>
#include <string>

#include <orm/uvcpp_orm_model.h>

struct profile {
  std::int64_t id;
  std::string display_name;
};

UVCPP_ORM_MODEL(profile, "user_profiles")
UVCPP_ORM_FIELD(profile, id, "profile_id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(profile, display_name, "display_name", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

const char* column_of_display_name() {
  const uvcpp::uvcpp_orm_column* column =
      uvcpp::model_traits<profile>::column_by_field_name("display_name");
  return column != nullptr ? column->column_name() : "";
}
```

* 表名与列名只来自 `model_traits<T>`。查询构造器不接受任意字符串当表名。
* 标识符会按方言加引号：SQLite/PostgreSQL 用双引号，MySQL 用反引号；名字里
  与引号字符相同的字符会翻倍。
* 引用一个没有在模型里声明的列名或成员指针是 `UNKNOWN_COLUMN`。
* 多余的结果列被忽略；缺列时，`nullable<T>` 字段写空态，非空字段是
  `MISSING_COLUMN`。

## 4. SQL 构造

四个构造器只生成**单表** SQL，并把每个值放进 `uvcpp_db_params`：

```cpp
#include <cstdint>
#include <iostream>
#include <string>

#include <orm/uvcpp_orm_query.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

void build_queries() {
  const uvcpp::uvcpp_orm_dialect dialect = uvcpp::uvcpp_orm_dialect::sqlite;

  // 分页查询：只选两列，WHERE 的 AND/OR 分组由构造器保证。
  const uvcpp::uvcpp_orm_sql_result page =
      uvcpp::select<account>(dialect)
          .column(&account::id)
          .column(&account::email)
          .where_ge(&account::age, std::int32_t(18))
          .where_is_not_null(&account::email)
          .or_where_eq(&account::active, true)
          .order_by(&account::id, uvcpp::uvcpp_orm_order::desc)
          .limit(20)
          .offset(0)
          .build();

  if (!page.ok()) {
    std::cerr << page.error << std::endl;
    return;
  }
  std::cout << page.sql << " (params=" << page.params.size() << ")\n";

  // COUNT 只保留 WHERE，不带列选择 / ORDER BY / LIMIT / OFFSET。
  const uvcpp::uvcpp_orm_sql_result total =
      uvcpp::select<account>(dialect)
          .where_ge(&account::age, std::int32_t(18))
          .build_count();

  // INSERT / UPDATE / DELETE 同样只接受模型里声明过的列。
  const uvcpp::uvcpp_orm_sql_result inserted =
      uvcpp::insert<account>(dialect)
          .column(&account::email, uvcpp::uvcpp_db_value("a@example.com"))
          .column(&account::active, uvcpp::uvcpp_db_value(true))
          .build();

  const uvcpp::uvcpp_orm_sql_result updated =
      uvcpp::update<account>(dialect)
          .set(&account::score, uvcpp::uvcpp_db_value(9.5))
          .where_eq(&account::id, std::int64_t(1))
          .build();

  const uvcpp::uvcpp_orm_sql_result removed =
      uvcpp::delete_from<account>(dialect)
          .where_in(&account::id, {std::int64_t(1), std::int64_t(2)})
          .build();

  (void)total;
  (void)inserted;
  (void)updated;
  (void)removed;
}
```

构造规则与拒绝项：

* WHERE 支持 `where_eq` / `where_ne` / `where_lt` / `where_le` / `where_gt` /
  `where_ge` / `where_like`、`where_in`、`where_is_null` /
  `where_is_not_null`，以及对应的 `or_where_*`。一个组内是 AND，多个组之间是 OR
  并自动加括号。
* `order_by(列, uvcpp_orm_order::asc|desc)`；`limit(n)` / `offset(n)` 的值也走
  参数绑定。只给 `offset` 不给 `limit` 是 `OFFSET_WITHOUT_LIMIT`。
* `update` / `delete_from` **没有 WHERE 时拒绝生成**（`EMPTY_WHERE`），不会
  生成全表写操作；`update` 没有 SET 列是 `EMPTY_SET`。
* SELECT 一个列都没有是 `EMPTY_COLUMN_LIST`；`where_in` 的空集合是
  `EMPTY_IN_VALUES`。
* `count()` 是“把这个 SELECT 变成 COUNT 查询”，不能和列选择 / ORDER BY /
  LIMIT / OFFSET 组合；`build_count()` 是“从同一个 WHERE 再生成一条 COUNT”，
  组合进来的那几项按定义被忽略。

## 5. 方言与占位符

方言由调用方**显式**传入，不从“当前连接”猜：

| 方言 | 占位符 | 标识符引号 |
|---|---|---|
| `uvcpp_orm_dialect::sqlite` | `?` | `"` |
| `uvcpp_orm_dialect::mysql` | `?` | `` ` `` |
| `uvcpp_orm_dialect::postgresql` | `$1` … `$n` | `"` |

SQLite 与 MySQL 的占位符相同，但不要因此把方言合并：SQLite 会把 `$1` 当**命名
参数**收下，写错的 SQL 在 SQLite 上可能“能跑”，换到 PostgreSQL 才暴露。构造器
和 `raw()` 都按方言生成/校验占位符；执行时方言与连接的驱动不匹配是
`INVALID_ARGUMENT`，不会在底层被悄悄改写。

## 6. 同步 CRUD

同步接口有三种执行器：`uvcpp_db_client&`（直接连接）、`uvcpp_db_tx&`（事务体）、
`uvcpp_db_pool&`（同步借还一条连接；`acquire()` 会阻塞，**不要在事件循环线程上
用**）。三者共用同一套 SQL 构造与结果映射。

```cpp
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_repository.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

bool fail(const char* what, uvcpp::uvcpp_orm_status status,
          const std::string& error, uvcpp::uvcpp_db_status db_status) {
  std::cerr << what << ": " << uvcpp::uvcpp_orm_status_name(status)
            << " [" << error << "]"
            << " db=" << uvcpp::uvcpp_db_status_name(db_status) << std::endl;
  return false;
}

bool run_crud() {
  uvcpp::uvcpp_db_client db;
  if (!uvcpp::uvcpp_db_ok(db.open("sqlite:///tmp/orm-guide.db"))) {
    std::cerr << db.last_error() << std::endl;
    return false;
  }

  std::string error;
  uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;

  account row;
  row.email = "alice@example.com";
  row.age = 30;                  // nullable<T> 从 T 构造 = 有值
  row.score = 1.5;
  row.active = true;

  // INSERT：自增列不进入 SQL；new_id 回填 last_insert_id。
  uvcpp::uvcpp_db_value new_id;
  uvcpp::uvcpp_orm_status status =
      uvcpp::insert(db, row, &new_id, &error, &db_status);
  if (status != uvcpp::uvcpp_orm_status::OK) {
    return fail("insert", status, error, db_status);
  }

  // 按主键查。没找到是 NOT_FOUND（不是 DB_ERROR），out 不被修改。
  account loaded;
  status = uvcpp::find_by_pk(db, new_id, &loaded, &error, &db_status);
  if (status != uvcpp::uvcpp_orm_status::OK) {
    return fail("find_by_pk", status, error, db_status);
  }

  // 条件查 + 可选总数。COUNT 从同一个 WHERE 生成，不受 LIMIT/OFFSET 影响。
  std::vector<account> page;
  int64_t total = 0;
  status = uvcpp::find_all(
      db,
      uvcpp::select<account>(uvcpp::uvcpp_orm_dialect::sqlite)
          .where_ge(&account::age, std::int32_t(18))
          .order_by(&account::id, uvcpp::uvcpp_orm_order::asc)
          .limit(20),
      &page, &error, &total, &db_status);
  if (status != uvcpp::uvcpp_orm_status::OK) {
    return fail("find_all", status, error, db_status);
  }

  // 显式 COUNT（total 必须非空）。
  int64_t active_total = 0;
  status = uvcpp::count(
      db,
      uvcpp::select<account>(uvcpp::uvcpp_orm_dialect::sqlite)
          .where_eq(&account::active, true),
      &active_total, &error, &db_status);
  if (status != uvcpp::uvcpp_orm_status::OK) {
    return fail("count", status, error, db_status);
  }

  // 按主键更新。SET 不含主键，也不含 read_only 列。
  loaded.score = 2.5;
  int64_t affected = -1;
  status = uvcpp::update(db, loaded, &affected, &error, &db_status);
  if (status != uvcpp::uvcpp_orm_status::OK) {
    return fail("update", status, error, db_status);
  }
  // affected == 0 表示没有这一行，不是执行失败。

  // 按主键删除。T 不在参数列表里出现，所以要显式写 remove<account>。
  affected = -1;
  status = uvcpp::remove<account>(db, new_id, &affected, &error, &db_status);
  if (status != uvcpp::uvcpp_orm_status::OK) {
    return fail("remove", status, error, db_status);
  }

  account gone;
  status = uvcpp::find_by_pk(db, new_id, &gone, &error, &db_status);
  return status == uvcpp::uvcpp_orm_status::NOT_FOUND;
}
```

约定：

* **`insert`**：`auto_increment` 与 `read_only` 列不进入 INSERT。`new_id` 非空时
  回填自增主键；后端报不出自增值时写 NULL 值，但返回码仍是 `OK`。失败时
  `new_id` 被清成 NULL。
* **`update`**：WHERE 是主键等值，SET 不含主键与 `read_only` 列。主键为 NULL
  时拒绝构造（`INVALID_ARGUMENT`）。执行失败时 `affected` 保持 -1，成功时写实际
  影响行数（可以为 0）。
* **`remove<T>`**：主键为 NULL 是 `INVALID_ARGUMENT`，SQL 生成 `DELETE ... WHERE
  pk = ?`。受影响 0 行不是错误。
* **`find_by_pk`**：一行都没有是 `NOT_FOUND`，`out` 保持原值。
* **`find_all`**：传入构造器时，`total` 非空会额外执行一条 `SELECT COUNT(*)`；
  传入已构造的 `uvcpp_orm_sql_result` 时，COUNT 从这条 SELECT 推导，只支持构造器
  生成的单表 SELECT（见 §10）。
* **`count`**：`total` 必须非空；查询没有返回值或返回 NULL 是 `MAPPING_FAILED`。

## 7. 在事务内使用

ORM 不自己 `begin` / `commit` / `rollback`。把它放进
`uvcpp_db_pool::transaction()` 的事务体里，提交、回滚、超时与“第一条失败后不再
发语句”的语义全部由 `uvcpp_db_tx` 保证：

```cpp
#include <cstdint>
#include <string>

#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_transaction.h>
#include <handle/uvcpp_loop.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_repository.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

void create_account(uvcpp::uvcpp_db_pool& pool) {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_db_tx_options options;
  options.timeout_ms = 5000;     // 整个事务的总预算；0 = 不限

  const int rc = pool.transaction(
      &loop,
      [](uvcpp::uvcpp_db_tx& tx) {
        std::string error;
        uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;

        account row;
        row.email = "bob@example.com";
        row.age = 40;
        row.score = 0.0;
        row.active = true;

        uvcpp::uvcpp_db_value new_id;
        uvcpp::uvcpp_orm_status status =
            uvcpp::insert(tx, row, &new_id, &error, &db_status);
        if (status != uvcpp::uvcpp_orm_status::OK) {
          tx.rollback();         // 也可以直接 return，框架会自动回滚
          return;
        }

        // 事务内查询走同一条连接，能看到自己尚未提交的写入。
        account loaded;
        status = uvcpp::find_by_pk(tx, new_id, &loaded, &error, &db_status);
        if (status != uvcpp::uvcpp_orm_status::OK) {
          tx.rollback();
          return;
        }

        loaded.score = 1.0;
        int64_t affected = -1;
        (void)uvcpp::update(tx, loaded, &affected, &error, &db_status);
      },
      [](uvcpp::uvcpp_db_status status, const std::string& error) {
        // 循环线程上恰好一次；投递失败时不会来（看 transaction() 返回值）。
        (void)status;
        (void)error;
      },
      options);

  if (rc != 0) {
    return;                      // 没投出去，回调不会来
  }
  loop.run();
}
```

事务内使用要点：

* 事务体跑在 worker 线程上；`uvcpp_db_tx` 只在事务体执行期间有效，别保存指针。
* ORM 的每条语句都经过 `tx` 自己的门：第一条失败的语句会被记住，后续 ORM 调用
  直接返回同一个错误。
* ORM 写操作与直接 `tx.execute()` / `tx.query()` 可以混用，因为它们走
  `tx.client()` 的同一条连接。
* 业务代码**不要自己构造** `uvcpp_db_tx`；它由 `uvcpp_db_pool::transaction()`
  内部创建。
* 同步的 `db.begin()` / `db.commit()` 仍可用于 `uvcpp_db_client` 上的手写事务，
  但 ORM 的 `uvcpp_db_tx&` 重载只接事务体里的那个句柄。

## 8. 异步回调

`<orm/uvcpp_orm_async.h>` 提供回调式异步 CRUD。它**复用**
`uvcpp_db_pool` 已有的异步路径，不新开线程、不新开连接池、不重写事务：

```cpp
#include <cstdint>
#include <string>
#include <vector>

#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_value.h>
#include <handle/uvcpp_loop.h>
#include <orm/uvcpp_orm_async.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_query.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

void start_async(uvcpp::uvcpp_db_pool& pool, uvcpp::uvcpp_loop& loop) {
  const uvcpp::uvcpp_orm_dialect dialect = uvcpp::uvcpp_orm_dialect::sqlite;
  std::string submit_error;

  // 按主键查。返回值 0 = 已投递；负值 = 没投出去，回调不会来。
  const int rc = uvcpp::find_by_pk_async<account>(
      pool, &loop, dialect, uvcpp::uvcpp_db_value(std::int64_t(1)),
      [](uvcpp::uvcpp_orm_status status, const account& value,
         const std::string& error) {
        if (status != uvcpp::uvcpp_orm_status::OK) {
          (void)error;
          return;
        }
        (void)value;
      },
      &submit_error);
  if (rc != 0) {
    return;
  }

  // 已构造的 SELECT 或构造器都可以；这个入口没有 total 参数。
  (void)uvcpp::find_all_async<account>(
      pool, &loop,
      uvcpp::select<account>(dialect)
          .where_ge(&account::age, std::int32_t(18))
          .order_by(&account::id, uvcpp::uvcpp_orm_order::asc),
      [](uvcpp::uvcpp_orm_status status, const std::vector<account>& rows,
         const std::string& error) {
        (void)status;
        (void)rows;
        (void)error;
      },
      &submit_error);

  account row;
  row.email = "carol@example.com";
  row.age = 25;
  row.score = 3.0;
  row.active = true;

  (void)uvcpp::insert_async(
      pool, &loop, dialect, row,
      [](uvcpp::uvcpp_orm_status status, const uvcpp::uvcpp_db_value& new_id,
         const std::string& error) {
        (void)status;
        (void)new_id;
        (void)error;
      },
      &submit_error);

  (void)uvcpp::update_async(
      pool, &loop, dialect, row,
      [](uvcpp::uvcpp_orm_status status, int64_t affected,
         const std::string& error) {
        (void)status;
        (void)affected;
        (void)error;
      },
      &submit_error);

  (void)uvcpp::remove_async<account>(
      pool, &loop, dialect, uvcpp::uvcpp_db_value(std::int64_t(1)),
      [](uvcpp::uvcpp_orm_status status, int64_t affected,
         const std::string& error) {
        (void)status;
        (void)affected;
        (void)error;
      },
      &submit_error);

  loop.run();
}
```

异步语义：

* 回调在 `loop` 的线程上**恰好一次**（成功、执行失败、映射失败都算一次）。
  投递失败返回负的 libuv 码，回调**不会**来；`submit_error` 写一句人读原因。
* “投递失败”与“执行失败”是两件事：判“这一次有没有投出去”看返回值，别等回调。
* `find_by_pk_async` / `insert_async` / `update_async` / `remove_async` 要求
  调用方显式传方言，因为 `uvcpp_db_pool` 不暴露池里连接的驱动；传错会在执行时由
  数据库报错，而不是被悄悄改写。`find_all_async` 从 SQL/构造器自己的 `dialect`
  取值。
* **异步只保证回调的线程与次数，不保证“真异步”**。PostgreSQL 上池查询走已有的
  真异步路径（`uv_poll` 盯连接套接字，不占池线程）；SQLite / MySQL / ODBC 仍走
  `uv_queue_work` 线程池。`insert_async` 走 `pool.transaction()` + 一条 INSERT
  来取回 `last_insert_id`，那本来就是两次阻塞往返，不属于真异步路径。
* `find_all_async` 没有 `total`：总数需要第二条 SQL，而这个接口只承诺“一次投递、
  一次回调”。需要总数时在回调里再投一条 COUNT，或使用同步路径。
* ORM 不新增线程模型。需要 future 糖时，用 db 层自己的 `uvcpp_db_async`；
  ORM 的异步面是回调式的。

## 9. 错误码、`db_status` 与出参

所有 ORM 接口返回 `uvcpp_orm_status`，不抛异常；人读原因写进可选的
`std::string* error`。`uvcpp_db_status* db_status` 是可选的后端原始返回码出参：
**只有 `DB_ERROR` 会写它**，其余失败把它留在 `OK`。

| 返回码 | 含义 |
|---|---|
| `OK` | 成功（UPDATE/DELETE 影响 0 行也是 OK） |
| `NO_MODEL` | 这个类型没有声明模型 |
| `NO_COLUMNS` / `NO_PRIMARY_KEY` / `MULTIPLE_PRIMARY_KEYS` | 模型元数据不成立 |
| `UNSUPPORTED_TYPE` | 字段类型没有映射 |
| `UNKNOWN_COLUMN` | 列名/成员指针不在模型里 |
| `NOT_FOUND` | `find_by_pk` 没找到 |
| `MISSING_COLUMN` | 结果集缺非空字段的列 |
| `TYPE_MISMATCH` | 类型不接受，或整数越界 |
| `NULL_VIOLATION` | SQL NULL 落到非空字段 |
| `MAPPING_FAILED` | 其他映射失败（例如 COUNT 查询没有返回值或返回 NULL） |
| `DB_ERROR` | 数据库执行失败；看 `db_status` 与执行器的 `last_error()` |
| `INVALID_ARGUMENT` | 空 out、NULL 主键、方言与连接不匹配等用法错误 |
| `INVALID_DIALECT` / `INVALID_LIMIT` / `INVALID_OFFSET` 等 | 构造期错误，见 `uvcpp_orm_error.h` |

`uvcpp_orm_status_name()` 给出稳定短名，适合日志与测试断言。区分
`NOT_FOUND` / `NULL_VIOLATION` / `TYPE_MISMATCH` / `DB_ERROR` 是有意的：主键没找到
与连接断开不是同一种处置；唯一键冲突这类后端错误应保留 `db_status` 与原文，而不是
压成一个字符串。

## 10. COUNT、LIMIT 与分页

有两种取得总数的写法：

1. **`find_all(builder, ..., total)`**：用构造器的 WHERE 生成一条
   `SELECT COUNT(*)`，**忽略** LIMIT/OFFSET/ORDER BY/列选择。分页查询与总数用
   同一个构造器，不会出现“总数也被 LIMIT 截断”的错误。
2. **显式 `count(db, builder, &total, ...)`**：先构造一个只带 WHERE 的 SELECT
   构造器（`.count()` 也可以），再执行。`total` 必须非空。

`find_all` 也接受已构造好的 `uvcpp_orm_sql_result`。这条路径的总数从 SELECT
文本推导，只支持构造器生成的单表 SELECT；如果 SELECT 列表里也有占位符，
`derive_count_query()` 会返回 `INVALID_ARGUMENT`（它无法安全地把 SELECT 列表
换成 `COUNT(*)`）。需要总数时优先走构造器重载。

```cpp
#include <cstdint>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_repository.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

bool load_page(uvcpp::uvcpp_db_client& db, int page_index) {
  std::vector<account> rows;
  int64_t total = 0;
  std::string error;
  uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;

  const int64_t page_size = 20;
  const uvcpp::uvcpp_orm_status status = uvcpp::find_all(
      db,
      uvcpp::select<account>(uvcpp::uvcpp_orm_dialect::sqlite)
          .where_ge(&account::age, std::int32_t(18))
          .order_by(&account::id, uvcpp::uvcpp_orm_order::asc)
          .limit(page_size)
          .offset(page_index * page_size),
      &rows, &error, &total, &db_status);

  if (status != uvcpp::uvcpp_orm_status::OK) {
    (void)error;
    (void)db_status;
    return false;
  }
  return static_cast<int64_t>(rows.size()) <= total;
}
```

## 11. NULL、空串、BLOB 与类型边界

`nullable<T>` 是显式的两态：默认构造是 NULL；从 `T` 构造（**包括** `T()` 和
空串）是有值。空串与 NULL 是两件事：

* SQL NULL → `nullable<T>` 是成功的空态；→ 非空字段是 `NULL_VIOLATION`。
* 结果集缺列 → `nullable<T>` 写空态；→ 非空字段是 `MISSING_COLUMN`。
* `nullable<std::string>` 的 `has_value() == true` 且 `value().empty()` 是一个长度
  为 0 的文本值，不是 NULL。
* 整数越界（例如把超出 `std::int32_t` 的值映射到该字段）是 `TYPE_MISMATCH`，
  不截断。
* **BLOB 空值限制**：`std::vector<std::uint8_t>` 映射到 `uvcpp_db_value::blob`。
  SQLite 后端对长度为 0 的 BLOB 按 **NULL** 绑定（这是 db 模块的现有行为），所以
  非空 BLOB 列不要存空 BLOB；`nullable<std::vector<std::uint8_t>>` 也无法用空
  vector 表示“有值但长度为 0”，它读回来会是空态。空串 TEXT 不受影响。
* **日期时间**：模型里没有自动映射到 `DATE` / `TIME` / `DATETIME` 的 C++ 类型。
  `std::string` 一律是 `TEXT`；SQLite 上按文本原样存取，不做时区换算。
* **DECIMAL**：沿用 db 模块的已知限制，ORM 不把它偷偷改成定点类型；需要精确
  十进制时用 `std::string` 存格式化的十进制串。
* **枚举/自定义类型**：不支持（`UNSUPPORTED_TYPE`）。需要时在模型里用底层整数或
  文本字段，业务层自己做转换。

## 12. `raw()` 逃生舱

`raw()` 原样收下你的 SQL，只做两件事：检查方言合法、检查占位符数量与参数数量
一致。它**不**做标识符白名单、不改写值、也不是完整的 SQL 解析器（它不懂
PostgreSQL 的 `$tag$...$tag$` 字符串，也不懂 MySQL 的反斜杠转义）：

```cpp
#include <cstdint>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>
#include <orm/uvcpp_orm_model.h>
#include <orm/uvcpp_orm_query.h>
#include <orm/uvcpp_orm_repository.h>

struct account {
  std::int64_t id;
  std::string email;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(account, "accounts")
UVCPP_ORM_FIELD(account, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(account, email, "email",
                UVCPP_ORM_NOT_NULL | UVCPP_ORM_UNIQUE)
UVCPP_ORM_FIELD(account, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(account, active, "active", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_MODEL_END()

bool raw_query(uvcpp::uvcpp_db_client& db) {
  const uvcpp::uvcpp_orm_sql_result sql = uvcpp::raw(
      uvcpp::uvcpp_orm_dialect::sqlite,
      "SELECT id, email, age, score, active FROM accounts "
      "WHERE age >= ? AND active = ?",
      {std::int32_t(18), true});

  if (!sql.ok()) {
    return false;
  }

  std::vector<account> rows;
  std::string error;
  uvcpp::uvcpp_db_status db_status = uvcpp::uvcpp_db_status::OK;
  const uvcpp::uvcpp_orm_status status =
      uvcpp::find_all(db, sql, &rows, &error, nullptr, &db_status);
  return status == uvcpp::uvcpp_orm_status::OK;
}
```

风险与规矩：

* **能不用就不要用。** 普通单表查询用 `select<T>()` / `insert<T>()` /
  `update<T>()` / `delete_from<T>()`，它们保证表名/列名来自模型、值走参数。
* `raw()` 的 SQL 必须是**常量结构**；所有外部输入都放进 `params`，不要做字符串
  拼接。占位符数量对不上会返回 `PARAM_COUNT_MISMATCH`，但这只是最低限度的护栏。
* PostgreSQL 的 `$1..$n` 必须是连续序列；`$2` 单独出现会被拒绝。
* 给 `find_all(sql_result, ..., total)` 用时，SELECT 列表里不要再有占位符，否则
  总数推导返回 `INVALID_ARGUMENT`。

## 13. 1.12.0 明确不做

下面这些**不在 1.12.0 的范围里**，不要按它们写业务代码：

* **JOIN / 关联 / 预加载 / 懒加载** —— 只有单表。多表查询请用 db 层手写 SQL。
* **自动迁移 / schema sync / 反向建表** —— 建表、索引、外键、唯一约束都由你的
  DDL 负责；`UVCPP_ORM_UNIQUE` 只是元数据标记。
* **复合主键** —— 只支持单列主键；多个主键在 `validate_model<T>()` 是
  `MULTIPLE_PRIMARY_KEYS`。
* **代码生成器**（`uvcpp_ctl`）—— 模型靠手写宏。
* **MongoDB ORM** —— MongoDB 仍是 db 模块旁边的独立客户端，不在 ORM 里。
* **C ABI 暴露 ORM** —— C 面函数集合不变。
* **C++20 协程 / concepts** —— 公开头在 C++11 下编译。
* **Unit of Work / identity map / 脏字段跟踪 / 批量 Insert**。
* **预处理语句缓存、流式结果、批量 COPY**。
* **自动重试、自动建表**。

## 14. 支持矩阵与验证边界

| 能力 | SQLite | MySQL | PostgreSQL |
|---|---|---|---|
| 模型声明与 `validate_model` | 已测 | 已测（同一套元数据，不连库） | 已测（同一套元数据，不连库） |
| SQL 构造与占位符/标识符转义 | 已测 | 已测（golden，不连库） | 已测（golden，不连库） |
| 同步 CRUD / 结果映射 | 已测（真库） | 已测（真库，`orm_mysql_func`） | 已测（真库，`orm_pgsql_func`） |
| 事务内使用 | 已测（真库） | 未在 ORM 用例里真库验证 | 未在 ORM 用例里真库验证 |
| 异步回调 | 已测（线程池路径） | 未在 ORM 用例里真库验证 | 未在 ORM 用例里真库验证 |

“已测”指仓库里的 `orm_*` 用例实际跑过（`orm_model_func`、`orm_sql_func`、
`orm_sqlite_func`、`orm_mysql_func`、`orm_tx_async_func`）。MySQL 与 PostgreSQL 的同步 CRUD
在真 8.0 / 16 服务端上跑过；事务内使用与异步回调仍只在 SQLite 上跑，ODBC
尚未在 ORM 用例里真库验证。“未验证”不是“不能用”：ORM 的执行层
复用 db 模块的 `uvcpp_db_client` / `uvcpp_db_pool` / `uvcpp_db_tx`，db 层自己的
用例是绿的；但不要把 db 层的结论借给 ORM。

**异步真相**：回调的线程与次数有保证；真正的非阻塞只到 PostgreSQL 这一条腿。
SQLite / MySQL / ODBC 的异步仍是 `uv_queue_work` 线程池，`insert_async` 也走
事务体 + INSERT 的两次往返。不要把它当成“所有后端都不占线程”。

**打包**：公开头安装到 `include/orm/`，`find_package(uvcpp)` 后
`#include <orm/...>` 可用；`UVCPP_BUILD_ORM=OFF` 时 ORM 头和符号不参与构建。
`package_release.py` 已按模块目录收集 `src/orm/*.h`，没有 ORM 私有头需要排除。
