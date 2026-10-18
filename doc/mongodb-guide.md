# MongoDB 客户端指南

`<db/uvcpp_mongo.h>` 是 **MongoDB 的客户端**：JSON 进、JSON 出，同步为核心，
异步建立在 libuv 线程池上。它属于 `db` 模块，但**与 `uvcpp_db_client` 不是同一条路**
—— 后者是 SQL 驱动的工厂，前者是一个独立的类，API 形状完全不同。

- 打开方式：`-DUVCPP_ENABLE_DB=ON`（必需）+ `-DUVCPP_ENABLE_DB_MONGODB=ON`（默认开）
- 包含方式：`<db/uvcpp_mongo.h>`
- 生成宏：`UVCPP_DB_MONGODB_ENABLE`（0/1）
- 客户端库：**不随包分发**。`libmongoc-1.0` / `libbson-1.0` 在第一次 `open()` 时
  按需加载；缺库不是错误，是 `NO_DRIVER`

> 随包策略的全文见 [`db-driver-policy.md`](db-driver-policy.md)。MongoDB 是
> 「动态接入」那一档：**仓里只有后端代码与符号表，产物里没有这两份客户端库**。

---

## 目录

1. [最小可运行程序](#1-最小可运行程序)
2. [构建与运行期加载](#2-构建与运行期加载)
3. [连接串](#3-连接串)
4. [API](#4-api)
5. [JSON 的形状](#5-json-的形状)
6. [并发与生命周期](#6-并发与生命周期)
7. [错误码对照](#7-错误码对照)
8. [排障](#8-排障)
9. [测试](#9-测试)
10. [明确不做](#10-明确不做)

---

## 1. 最小可运行程序

```cpp
#include <db/uvcpp_mongo.h>
#include <cstdio>

int main() {
  uvcpp::uvcpp_mongo_client mongo;
  if (mongo.open("mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=2000") !=
      uvcpp::uvcpp_db_status::OK) {
    std::fprintf(stderr, "%s\n", mongo.last_error().c_str());
    return 1;
  }

  mongo.insert_one("app", "users", "{\"name\": \"ada\", \"age\": 36}");

  std::vector<std::string> docs;
  mongo.find("app", "users", "{\"age\": {\"$gte\": 18}}",
             "{\"limit\": 10, \"sort\": {\"age\": -1}}", &docs);
  for (size_t i = 0; i < docs.size(); ++i) {
    std::printf("%s\n", docs[i].c_str());
  }
  mongo.close();
  return 0;
}
```

在事件循环上用异步版（**回调在 `loop` 的线程上**）：

```cpp
// doc-snippet: fragment — 接着上一段，不是独立的翻译单元
uvcpp::uvcpp_loop loop;
mongo.find_async(&loop, "app", "users", "{}", "{\"limit\": 1}",
                 [&](uvcpp::uvcpp_db_status st, const std::vector<std::string>& docs,
                     const std::string& err) {
                   if (st == uvcpp::uvcpp_db_status::OK) {
                     // docs[0] 是 canonical Extended JSON
                   } else {
                     std::fprintf(stderr, "%s\n", err.c_str());
                   }
                   mongo.close();
                 });
loop.run(UV_RUN_DEFAULT);
```

---

## 2. 构建与运行期加载

### 2.1 两个开关，两件事

| 开关 | 作用 | 缺省 |
|---|---|---|
| `UVCPP_ENABLE_DB` | db 模块总开关 | **OFF** |
| `UVCPP_ENABLE_DB_MONGODB` | 这个后端要不要找头、编不编进来 | ON（主开关打开时） |
| `UVCPP_DB_MONGODB_INCLUDE_DIR` | 可选：`libmongoc-1.0/` 与 `libbson-1.0/` 的公共上层目录 | 空（按常规路径找） |
| `UVCPP_DB_RUNTIME_LOAD` | 客户端库链接进来，还是运行时打开 | OFF（**发布包是 ON**） |

配置期找两份头：

```
<prefix>/include/libmongoc-1.0/mongoc/mongoc.h
<prefix>/include/libbson-1.0/bson/bson.h
```

`-DUVCPP_DB_MONGODB_INCLUDE_DIR=/usr/include` 这种写法指向它们的**公共上层**；
也可以直接给解包出来的 `/tmp/mongoc-root/usr/include`（开发机临时用）。

找不到头 → 后端不编进来，配置期打一条 **WARNING**，`UVCPP_DB_MONGODB_ENABLE=0`。
这时公开类**仍然存在**（这是刻意的）：`open()` 一律返回 `NO_DRIVER`，
`backend_available()` 是 `false`，进程、HTTP 服务和别的模块一切照常。

### 2.2 运行期加载什么

`UVCPP_DB_RUNTIME_LOAD=ON` 时，链接行上**没有** mongoc/bson。第一次 `open()`
才按下面的顺序找：

| 库 | 环境变量（显式路径，优先） | 候选名（Linux，1.x 先试） |
|---|---|---|
| libmongoc | `UVCPP_DB_MONGOC_LIBRARY` | `libmongoc-1.0.so.0`、`libmongoc-1.0.so`、`libmongoc2.so.2`、`libmongoc2.so` |
| libbson | `UVCPP_DB_BSON_LIBRARY` | `libbson-1.0.so.0`、`libbson-1.0.so`、`libbson2.so.2`、`libbson2.so` |

**两组名字是因为 mongo-c-driver 2.0 改了 SONAME**：1.x 是 `libmongoc-1.0.so.0`，
2.0 起是 `libmongoc2.so.2`（Windows 上 `mongoc-1.0.dll` → `libmongoc2.dll`）。
两组都试，1.x 在前 —— Debian/Ubuntu 到 24.04 装的都还是 1.x。

macOS 是 `libmongoc-1.0.0.dylib` / `libmongoc-1.0.dylib` / `libmongoc2.dylib`（bson
同理），Windows 是 `mongoc-1.0.dll` / `libmongoc-1.0.dll` / `mongoc2.dll` /
`libmongoc2.dll`（bson 同理）。**两份都要**：缺一份就是
`NO_DRIVER`，错误串列出试过的名字与失败原因。

句柄**永不关闭**（与 MySQL / PostgreSQL / ODBC 同一条约定：进程退出由内核回收，
手工 `dlclose` 要求"再也没有活连接"，那个条件无法可靠判定）。因此加载只发生一次，
同进程里第二次 `open()` 拿的是同一份状态。

### 2.3 装哪个包

| 系统 | 运行库 | 开发头 |
|---|---|---|
| Debian / Ubuntu | `libmongoc-1.0-0`（或 `libmongoc-1.0-0t64`）+ `libbson-1.0-0`；2.x 的发行版是 `libmongoc2-2` + `libbson2-2` | `libmongoc-dev`（带 `libbson-dev`） |
| RHEL / Fedora | `mongo-c-driver` | `mongo-c-driver-devel` |
| macOS | `brew install mongo-c-driver` | 同一个 formula |
| Windows | mongo-c-driver 的官方 DLL（1.x 是 `mongoc-1.0.dll`，2.x 是 `libmongoc2.dll`） | 随开发包的头 |

> Ubuntu 24.04 起运行时包带 `t64` 后缀（`time_t` 迁移），这是**包名**的差异，
> 不影响库名。`libmongoc` 自己还依赖 `libmongocrypt.so.0`（包 `libmongocrypt0`）
> —— 少了它 `dlopen` 会失败，错误串里原样带出那句 `cannot open shared object file`。

---

## 3. 连接串

原样交给 libmongoc（`mongoc_uri_new_with_error`）：

```
mongodb://[user:pass@]host[:port][/db][?options]
mongodb+srv://[user:pass@]host[/db][?options]
```

本类不重写、不补全连接串。两个最常调的选项：

| 选项 | 含义 | 本库相关的注意 |
|---|---|---|
| `serverSelectionTimeoutMS` | 选服务器的上限 | **`open()` 真的 ping 一次**，上限就是它（默认 30000）。本机/局域网建议写小，例如 `?serverSelectionTimeoutMS=2000` |
| `connectTimeoutMS` | 单条 TCP 连接的上限 | 与上面那个分开 |

`open()` 的语义是「**服务器确实应答了**」：它 `mongoc_client_new_from_uri` 之后
还会在 `admin` 上发一条 `{"ping": 1}`。所以：

- 语法不对 → `BAD_URL`（`last_error()` 是 libbson 原话）；
- 语法对、服务器不在 / 认证失败 / 选不出可用节点 → `OPEN_FAILED`。

已经打开时再 `open()` 会先关掉旧连接再开新的；**失败时不会退回旧连接**
（与 `uvcpp_db_client::open()` 同一条）。

---

## 4. API

```cpp
// doc-snippet: fragment — 接口清单，不是可编译的翻译单元
namespace uvcpp {
class uvcpp_mongo_client {
 public:
  uvcpp_mongo_client();
  ~uvcpp_mongo_client();
  // 拷贝/赋值已删除 —— 一条连接一个对象

  uvcpp_db_status open(const std::string& uri);
  void close();
  bool is_open() const;
  uvcpp_db_status ping();
  const std::string& last_error() const;
  const std::string& uri() const;
  static bool backend_available();

  uvcpp_db_status insert_one(const std::string& db,
                             const std::string& collection,
                             const std::string& document_json);
  uvcpp_db_status update_one(const std::string& db,
                             const std::string& collection,
                             const std::string& filter_json,
                             const std::string& update_json, bool upsert);
  uvcpp_db_status delete_one(const std::string& db,
                             const std::string& collection,
                             const std::string& filter_json);
  uvcpp_db_status find(const std::string& db, const std::string& collection,
                       const std::string& filter_json,
                       const std::string& options_json,
                       std::vector<std::string>* out);
  uvcpp_db_status command(const std::string& db,
                          const std::string& command_json,
                          std::string* out_json);

  using simple_cb = std::function<void(uvcpp_db_status, const std::string&)>;
  using find_cb = std::function<void(uvcpp_db_status,
                                     const std::vector<std::string>&,
                                     const std::string&)>;
  int insert_one_async(uvcpp_loop*, db, coll, document_json, simple_cb = nullptr);
  int update_one_async(uvcpp_loop*, db, coll, filter, update, bool upsert,
                       simple_cb = nullptr);
  int delete_one_async(uvcpp_loop*, db, coll, filter, simple_cb = nullptr);
  int find_async(uvcpp_loop*, db, coll, filter, options, find_cb = nullptr);
  int command_async(uvcpp_loop*, db, command_json, simple_cb = nullptr);
  int ping_async(uvcpp_loop*, simple_cb = nullptr);
};
}  // namespace uvcpp
```

几处约定：

- **`find` 的 `out` 不能为空**，每次调用**先清空**它；`filter_json` 为空串等价于
  `{}`，`options_json` 为空串等价于不给选项。
- `options_json` 原样交给 `mongoc_collection_find_with_opts` —— `limit` / `skip` /
  `sort` / `projection` 都是驱动认的那套写法，本库不解释它。
- `update_one` 的 `update_json` 是**更新操作符**文档（`$set` / `$inc` …），不是
  整条替换；替换用 `command()` 发 `replaceOne`。
- `command()` 的 `db` 是命令的执行库（`"admin"` 或业务库名）。
- `backend_available()` 说的是**编译期**事实（这份构建有没有编后端），
  `open()` 返回的 `NO_DRIVER` 才可能是运行期缺库。两者一起看才知道该"装包"还是
  "换构建"。

---

## 5. JSON 的形状

公开面里**一个 BSON 类型都不出现**，这是有意的（使用者不需要把
`<bson/bson.h>` 装进自己的构建 —— 那份头正是本模块不随包分发的那一份）。

| 方向 | 函数 | 说明 |
|---|---|---|
| 文本 → BSON | `bson_new_from_json` | **Extended JSON**：`{"$oid": "…"}`、`{"$date": "…"}`、`{"$numberLong": "…"}` 都能进 |
| BSON → 文本 | `bson_as_canonical_extended_json` | **canonical** 形状：类型信息显式写在标签里（例如 `{"$numberInt": "36"}`） |

所以 `find` 回来的文档与写进去的那段文本不保证逐字节相同 —— 它保证的是
**同一份文档**。要在代码里读结果，请按 Extended JSON 解析（本仓的
`<uvcpp/uvcpp_json_scan.h>` 或你自己的 JSON 库都行），不要按位置截字符串。

`insert_one` / `update_one` 的文档解析失败返回 `EXEC_FAILED`，`last_error()` 是
libbson 的原话；`BAD_URL` 只留给连接串。

---

## 6. 并发与生命周期

- **一个对象同一时刻只跑一条操作**（内部一把递归锁）。libmongoc 的一条
  `mongoc_client_t` 自带连接池，从外面再叠"同一连接并发多条"只会把排队藏起来。
  要并行就开多个 `uvcpp_mongo_client`。
- 同步方法在**调用线程**上阻塞（libmongoc 是阻塞 API，本类不假装它不是）。
- `*_async` 用 `uv_queue_work` 把同样的调用丢进 libuv 线程池，完成回调**在
  `loop` 的线程上**触发。返回值只说明"受理没受理"：

  | 返回 | 含义 |
  |---|---|
  | `0` | 已投递，回调**一定**会被调一次 |
  | 负的 libuv 码（`UV_EINVAL` / `UV_ENOMEM`） | 当场没投出去，**回调不会被调** |

- 异步回调里**可以析构本对象**：在飞的活自己拿住了内部状态（`shared_ptr`），
  析构不会把它的脚砍掉。
- 异步回调里**不要**对同一个对象发同步调用 —— 那会在循环线程上等池线程持有的锁。
  要串联就再投一次异步。
- 析构会等手上的同步操作跑完（同一把锁）；在飞的异步操作也会自然结束，不会半路被砍。

---

## 7. 错误码对照

| 码 | 什么时候 | 下一步 |
|---|---|---|
| `OK` | 成功 | —— |
| `NO_DRIVER` | 这份构建没编后端，或运行时打不开 libmongoc / libbson | 看 `backend_available()`：`false` 换构建，`true` 装包/指路 |
| `BAD_URL` | 连接串解析失败 | 看 `last_error()`（libbson 原话） |
| `OPEN_FAILED` | 连不上、认证失败、选不出节点 | 检查服务、凭据、`serverSelectionTimeoutMS` |
| `NOT_CONNECTED` | 连接没打开就调了操作（包括 `open()` 失败之后） | 先 `open()` |
| `EXEC_FAILED` | JSON 解析失败、驱动报错、取结果出错 | 看 `last_error()` |
| `MISUSE` | 参数不合法（例如 `find` 的 `out` 为空） | 改调用 |

`last_error()` 在成功时**不清理**旧的失败原因 —— 失败现场比成功更值得留着。
异步回调拿到的消息是失败当时的快照。

---

## 8. 排障

**`open()` 回 `NO_DRIVER`，`backend_available()==true`。**
读 `last_error()`：

- 句子里是 `UVCPP_DB_MONGOC_LIBRARY=/path（…）` → 这个路径打不开（常见是路径写错、
  或者它依赖的 `libmongocrypt.so.0` 不在 `LD_LIBRARY_PATH` 上）；
- 句子里列出一串候选名（`libmongoc-1.0.so.0（…）、…、libmongoc2.so（…）`）→ 全都
  没找到，装运行库包；
- 句子里是 `libbson（…）里缺少符号 …` → 版本太旧，或者那份 `.so` 根本不是 libbson。

**`open()` 回 `OPEN_FAILED`，错误里是 `No suitable servers found`。**
服务器不在、端口不对、认证被拒，或者 `serverSelectionTimeoutMS` 太小。错误原文里
会带上它试过的主机与失败原因。

**`backend_available()==false`。**
这份库在配置期没找到 `mongoc.h` / `bson.h`。装开发包后**重新构建** ——
换一份运行时库不能把一个没编进来的后端变出来。

---

## 9. 测试

| 用例 | 内容 | 需要什么 |
|---|---|---|
| `db_mongodb_nodrv_func` | 客户端库/后端不在时的返回码与错误串；同一进程里 SQLite 照常可用；异步入口的回调次数 | 无（前提由 ctest 的 `ENVIRONMENT` 给） |
| `db_mongodb_func` | 对真实 mongod：ping、insert/find/update/upsert/delete/command、异步、坏 JSON 与坏连接串 | **真的 mongod** |

真实用例的环境变量：

- `UVCPP_DB_TEST_MONGODB_URL`：例如
  `mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000`。没设时返回 **3**，
  ctest 报成 `***Skipped`（`SKIP_RETURN_CODE 3`）。
- `UVCPP_DB_TEST_REQUIRE=1`：没 URL **或**客户端库加载失败都是**红**。CI 的腿上
  起了 mongod 并设这一条 —— 服务都起了还跳过，说明配置写错了。
- 用例只碰自己那个带前缀的 collection，末尾 `drop`。

```bash
mongod --dbpath /tmp/uvcpp-mongo-db --port 27017 --bind_ip 127.0.0.1 &
UVCPP_DB_TEST_MONGODB_URL='mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000' \
UVCPP_DB_TEST_REQUIRE=1 \
  ctest --test-dir build -R db_mongodb --output-on-failure
```

`db_mongodb_nodrv_func` 不设 `SKIP_RETURN_CODE`：它的前提完全在测试这边（把两个
环境变量指到不存在的路径），不成立就该红。

---

## 10. 明确不做

- **事务 / Change Streams / GridFS / 客户端字段级加密**：用 `command()` 发原始
  命令可以做一部分，但没有专用 API。
- **聚合管道构造器**：`command()` 发 `aggregate` 就行；本类不重复发明一套 builder。
- **游标 / 流式结果**：`find` 一次性取回全部。大数据集请用 `options_json` 的
  `limit` / `batchSize` 自己分页。
- **连接池**：一个对象一条 `mongoc_client_t`（它内部有自己的连接池）。要并行就开
  多个对象。
- **把 `libmongoc` / `libbson` 打进发布包**：不打包、不链接，只运行时引用。
- **C ABI**：本轮没有 `uvcpp_c_mongo_*`。

---

## 相关文档

- [`db-driver-policy.md`](db-driver-policy.md)：什么随包、什么不随包、缺库时什么现象
- [`db-guide.md`](db-guide.md)：SQL 后端（`uvcpp_db_client`）；与本类没有共用代码
- [`redis-guide.md`](redis-guide.md)：另一条"零外部依赖、默认编进库"的客户端路线
