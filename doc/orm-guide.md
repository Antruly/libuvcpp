# 单表 ORM 指南（1.12.0 草稿）

> 这一版只覆盖模型声明（#89）、SQL 构造（#90）与同步单表 CRUD（#91）。
> 事务、异步、关联、迁移、复合主键都**没有**实现。

## 1. 打开方式

ORM 不是独立模块：它复用 db 模块的 `uvcpp_db_value` / `uvcpp_db_client`，
所以必须同时打开 DB。没有 DB 时配置期会把 ORM 强制关掉。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ENABLE_DB=ON -DUVCPP_ENABLE_DB_SQLITE=ON -DUVCPP_BUILD_ORM=ON
```

公开头在 `include/orm/`：`uvcpp_orm_value.h`（`nullable<T>` 与类型映射）、
`uvcpp_orm_model.h`（模型元数据/宏）、`uvcpp_orm_error.h`（返回码）、
`uvcpp_orm_query.h`（SQL 构造）、`uvcpp_orm_repository.h`（执行与结果映射）。

## 2. 声明模型

宏必须写在**全局作用域**；模型是公开成员、无基类、无虚函数的聚合。

```cpp
struct user {
  std::int64_t id;
  std::string name;
  uvcpp::nullable<std::int32_t> age;
  double score;
  bool active;
};

UVCPP_ORM_MODEL(user, "users")
UVCPP_ORM_FIELD(user, id, "id",
                UVCPP_ORM_PRIMARY_KEY | UVCPP_ORM_AUTO_INCREMENT)
UVCPP_ORM_FIELD(user, name, "name", UVCPP_ORM_NOT_NULL)
UVCPP_ORM_FIELD(user, age, "age", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(user, score, "score", UVCPP_ORM_NONE)
UVCPP_ORM_FIELD(user, active, "active", UVCPP_ORM_NONE)
UVCPP_ORM_MODEL_END()
```

`nullable<T>` 的默认构造是 NULL；从 `T` 构造（**包括** `T()` 和空串）是有值。
空串与 NULL 是两件事。

## 3. CRUD

所有接口返回 `uvcpp_orm_status`，不抛异常；人读原因写进 `std::string* error`。
`db_status` 是可选出参，只有数据库执行失败（`DB_ERROR`）时才写后端原始返回码。

```cpp
uvcpp::uvcpp_db_client db;
db.open("sqlite:///tmp/app.db");

user u;
u.name = "Alice";
u.score = 1.5;
u.active = true;

// INSERT：跳过自增主键；给了 new_id 就回填 last_insert_id。
uvcpp::uvcpp_db_value new_id;
std::string error;
uvcpp::uvcpp_orm_status st =
    uvcpp::insert(db, u, &new_id, &error);

// 按主键查。没找到是 NOT_FOUND（不是 DB_ERROR），out 不被修改。
user loaded;
st = uvcpp::find_by_pk(db, new_id, &loaded, &error);

// 条件查 + 可选总数（COUNT 从同一个 WHERE 生成，不受 LIMIT/OFFSET 影响）。
std::vector<user> page;
int64_t total = 0;
st = uvcpp::find_all(
    db,
    uvcpp::select<user>(uvcpp::uvcpp_orm_dialect::sqlite)
        .where_ge(&user::age, std::int32_t(18))
        .order_by(&user::id, uvcpp::uvcpp_orm_order::asc)
        .limit(20),
    &page, &error, &total);

// 按主键更新：SET 不含主键（也不含 read_only 列）。
int64_t affected = -1;
st = uvcpp::update(db, loaded, &affected, &error);
// affected == 0 表示没有这一行，不是执行失败。

// 按主键删除：affected == 0 同样不是失败。
// T 不在参数列表里出现，所以要显式写 remove<user>。
st = uvcpp::remove<user>(db, new_id, &affected, &error);
```

已经构造好的 SQL 也可以直接执行：

```cpp
uvcpp::uvcpp_orm_sql_result sql =
    uvcpp::select<user>(uvcpp::uvcpp_orm_dialect::sqlite)
        .where_eq(&user::name, std::string("Alice"))
        .build();
st = uvcpp::find_all(db, sql, &page, &error);
```

## 4. 映射规则

| 结果集 | 模型字段 | 结果 |
|---|---|---|
| 列名精确匹配 | 同类型 | 映射 |
| 多余列 | — | 忽略 |
| 缺列 | `nullable<T>` | 写空态 |
| 缺列 | 非空字段 | `MISSING_COLUMN` |
| SQL NULL | `nullable<T>` | 空态 |
| SQL NULL | 非空字段 | `NULL_VIOLATION` |
| TEXT 值 | 整数/浮点字段 | `TYPE_MISMATCH` |
| 整数越界 | 窄整数/无符号字段 | `TYPE_MISMATCH` |

映射按**列名**而不是列位置，所以结果列顺序可以不同。一行先在栈上的临时
模型里映射完，全部成功才写进 `out`；失败不会留下半个对象。

支持 #89 的全部字段类型：`bool`、`int8_t`…`int64_t`、`uint8_t`…`uint64_t`、
`float`、`double`、`std::string`、`std::vector<std::uint8_t>`（BLOB），以及
它们各自的 `nullable<T>` 版本。

## 5. 返回码

| 返回码 | 含义 |
|---|---|
| `OK` | 成功（UPDATE/DELETE 0 行也是 OK） |
| `NOT_FOUND` | `find_by_pk` 没找到 |
| `MISSING_COLUMN` | 结果集缺非空字段的列 |
| `TYPE_MISMATCH` | 类型不接受，或整数越界 |
| `NULL_VIOLATION` | SQL NULL 落到非空字段 |
| `MAPPING_FAILED` | 其他映射失败（例如 COUNT 没返回整数） |
| `DB_ERROR` | 数据库执行失败；看 `db_status` 与 `db.last_error()` |
| `INVALID_ARGUMENT` | 空 out、NULL 主键、SQL 方言与连接不匹配等 |

`insert` 的 `new_id` 在失败时会清成 NULL。`update` / `remove` 失败时
`affected` 保持 -1，成功时写实际影响行数（可以为 0）。

## 6. 已知边界

* `find_all(result, ..., total)` 需要从 SELECT 推导 COUNT：只支持构造器生成的
  单表 SELECT。`raw()` 的 SELECT 列表里若也有占位符，会返回
  `INVALID_ARGUMENT`；改用 `find_all(builder, ..., total)` 或显式 `count<T>()`。
* 模型必须可默认构造（`column_for<T>()` 与映射都依赖这一点）。
* 字段偏移来自 `offsetof`；与 #89 一样，模型必须是公开成员、无基类、无虚函数
  的聚合形状。
* SQLite 对长度 0 的 BLOB 会按 NULL 绑定（db 模块现有行为），所以非空 BLOB
  列不要存空 BLOB。空串 TEXT 不受影响。
