# 数据库驱动随包策略

这一页回答的是**"这个仓库带什么、不带什么、不带的时候会发生什么"**，不是用法。
用法见 [`db-guide.md`](db-guide.md)。

一句话：**「支持直接项目集成」说的是仓里有后端代码，不是随包分发客户端的
`.so` / `.dll`。** 发布包里没有 `libmysqlclient.so`、没有 `libpq.so`，也没有
`libmongoc-1.0.so`；这些库由使用者的机器自己装，本库在运行期去找它们。

## 一张表

| 数据库 | 仓里有什么 | 产物里带客户端库吗 | 缺库时 |
|---|---|---|---|
| **SQLite** | 内建后端，另有一份钉死哈希的 amalgamation 源码（`UVCPP_DB_SQLITE_FROM_SOURCE=ON`） | 不需要 —— 从源码编进 `libuvcpp` | —— |
| **MySQL** | 内建 SQL 后端（`src/db/uvcpp_db_mysql.cpp`） | **不带**。发布配置 `UVCPP_DB_RUNTIME_LOAD=ON`，运行期 `dlopen` 使用者那份 `libmysqlclient` / `libmariadb` | `open()` 返回 `uvcpp_db_status::NO_DRIVER`，进程照常 |
| **PostgreSQL** | 内建 SQL 后端（`src/db/uvcpp_db_pgsql.cpp`，目前唯一有原生异步查询路径的后端） | **不带**，同上 | 同上 |
| **Redis** | 本仓自己实现的 RESP2 / RESP3 客户端（`redis` 模块），没有 hiredis、没有别的 Redis 客户端库 | 不需要 | —— |
| **MongoDB** | 后端代码与运行期符号表（`db` 模块里的 MongoDB 后端） | **不带**。`libmongoc-1.0` / `libbson-1.0` 只被 `dlopen` / `LoadLibrary` 引用 | `open()` 返回 `NO_DRIVER`，进程照常 |
| **SQL Server / Oracle / 达梦 / 其他** | **不提供官方驱动**；保留 ODBC 后端作为接入面（`src/db/uvcpp_db_odbc.cpp`） | 不带 ODBC 驱动，也不带驱动管理器本身以外的任何东西 | `open()` 返回驱动管理器的原话（找不到数据源 / 找不到驱动） |
| **ODBC 驱动本身** | 不提供、不打包 | —— | 使用者自己装 `msodbcsql18`、Oracle Instant Client ODBC、达梦 ODBC 等 |

## 三条口径

### 1. 「开源数据库」只指 SQLite / MySQL / PostgreSQL

这三个是**仓里有后端代码、可以直接对着编译**的库：SQLite 连源码一起钉在仓库里；
MySQL / PostgreSQL 在配置期只要头（`mysql.h` / `libpq-fe.h`），因为 `MYSQL_BIND`、
`PQresult` 这些类型要按真实布局用，不能靠手抄结构体赌 ABI。

**但"编进来"不等于"打包带走"。** MySQL / PostgreSQL 的符号取自运行期加载的客户端库，
发布产物里**没有**这两个库的 `DT_NEEDED`（Windows 上是导入表项）。理由在包的
使用者身上：`DT_NEEDED` 是**加载期**解析的，多一条 `libpq.so.5`，没装过 libpq 的
机器连 `libuvcpp.so` 都加载不了 —— 哪怕那个程序只跑 HTTP、一行数据库代码都不碰。

### 2. MongoDB 属于「动态接入」，且客户端库也不随包分发

按本项目的口径，MongoDB **不是**这里说的「开源数据库」（它的许可与发行方式都不是
SQLite / MySQL / PostgreSQL 那一路）。因此：

- 仓里有后端代码、有稳定的运行期符号表，**没有** `libmongoc` / `libbson` 的副本；
- 配置期只要有 `mongoc/mongoc.h` 与 `bson/bson.h`（`UVCPP_ENABLE_DB_MONGODB=ON`，
  默认 `ON`）；**找不到头就只关掉这一个后端**，不报错、不影响别的模块；
- 运行期用 `dlopen(RTLD_NOW | RTLD_LOCAL)` / `LoadLibraryA` 找使用者安装的那一份；
- **缺库不是进程级错误**：`uvcpp_mongo_client::open()` 返回 `NO_DRIVER`，
  `last_error()` 列出试过的库名与环境变量；HTTP / WebSocket / 其他模块照常工作。

### 3. 其他数据库：只有 ODBC 这一个接入面

SQL Server、Oracle、达梦等**没有本仓提供的官方驱动**。要连它们，走 ODBC 后端：

- 本仓带的是**接入代码**（ODBC 后端）与对**驱动管理器**的运行时引用，不带
  `msodbcsql18`、Oracle Instant Client、达梦 ODBC 这些具体驱动；
- 连接串里的 `Driver=` / `DSN=` 决定连谁、用哪种方言，本库不替使用者猜；
- 驱动装没装、DSN 配没配，只有 `open()` 知道；失败时返回的是驱动管理器的原话。

## 缺库时的统一行为

所有运行时加载的后端（MySQL / PostgreSQL / ODBC / MongoDB）遵守同一条契约：

1. 配置期能找到头 ⇒ 后端**编进库**；找不到 ⇒ 只关掉这一个并出声（`WARNING`），
   其余后端与模块照常。
2. 运行期能找到客户端库 ⇒ 正常连接；找不到 ⇒ `open()` 返回 `NO_DRIVER`。
3. `NO_DRIVER` **不是**"库坏了"：它是 `uvcpp_db_status` 的一个普通返回值，
   进程可以继续跑，下一次装好库再 `open()` 就行（库句柄在进程内缓存，重开无需重启）。
4. 每个后端都有环境变量覆盖搜索路径：

   | 后端 | 环境变量 |
   |---|---|
   | MySQL | `UVCPP_DB_MYSQL_LIBRARY` |
   | PostgreSQL | `UVCPP_DB_PGSQL_LIBRARY` |
   | ODBC（驱动管理器） | `UVCPP_DB_ODBC_LIBRARY` |
   | MongoDB | `UVCPP_DB_MONGOC_LIBRARY`、`UVCPP_DB_BSON_LIBRARY` |

   环境变量是**独占式覆盖**：设了它就只试它，不会再退回去找候选名。

## 发布产物里有什么

| 产物 | 数据库相关内容 |
|---|---|
| 六个 `libuvcpp-<ver>-<leg>.zip` | SQLite（源码编入）、MySQL / PostgreSQL / ODBC / MongoDB 后端代码；**没有任何客户端库** |
| `.deb` / `.rpm` | 同上；`libuvcpp1` 的 `Depends` 只有系统 libuv / zlib 等运行期依赖，不依赖数据库客户端库 |
| Windows 安装程序 | 同上 |

判断依据不是读 README，是发布流水线里的三条断言：`ldd` 黑名单、`nm -D -u` 的未定义
符号检查、Windows 上的 `dumpbin /DEPENDENTS`。它们同时挡住"链接行被改回去"和
"符号漏改一处调用点"两种形状。

## 与「自包含」的关系

「自包含」在发布包的语境里**只指除 libc / C++ 运行时之外不依赖第三方库**这一条，
而且有明确例外：Linux 的发行版包链系统的 `libuv.so.1` / `libz.so.1`（理由与公开头
有关，见 [`packaging/README.md`](../packaging/README.md)）。数据库客户端库从来不在
"自包含"的承诺里 —— 它们从 1.6.54 起就只走运行期加载，1.7.0 起六条发布腿全部如此。
