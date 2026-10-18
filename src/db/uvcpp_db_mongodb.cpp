/**
 * @file src/db/uvcpp_db_mongodb.cpp
 * @brief `uvcpp_mongo_client` 的实现：真实后端（运行时加载）+ 缺后端时的桩。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件有**两条互斥的编译路径**，由 `UVCPP_DB_MONGODB_ENABLE` 选：
 *
 *  * `=1`：配置期找到了 `mongoc.h` 与 `bson.h`，后端真的编进来了。
 *    公开类照常工作；客户端库在**第一次 `open()`** 时才加载。
 *  * `=0`：没找到头（或使用者显式关了 `UVCPP_ENABLE_DB_MONGODB`）。公开类
 *    **仍然存在**，每个方法返回 `NO_DRIVER`。这是本模块的契约：MongoDB 的
 *    客户端库本仓既不随包分发、也不要求使用者为了用别的功能去装它 ——
 *    "这个包没带 MongoDB" 必须表现为一次可读的运行期返回码，而不是链接错误。
 *
 * 为什么 `UVCPP_DB_MONGODB_ENABLE` 与 `UVCPP_ENABLE_DB_MONGODB` 是两个值：
 * 前者是**结果**（找到头没有），后者是**意图**（要不要找）。与 MySQL / PG /
 * ODBC 那三个后端同一个形状。
 */

#include "db/uvcpp_mongo.h"

#include <uvcpp/uvcpp_config.h>

#if UVCPP_DB_ENABLE

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <db/uvcpp_db_status.h>
#include <req/uvcpp_work.h>

#if UVCPP_DB_MONGODB_ENABLE
#include <db/uvcpp_db_mongodb_api.h>

#if UVCPP_DB_RUNTIME_LOAD
#include <db/uvcpp_db_runtime_load.h>
#endif
#endif  // UVCPP_DB_MONGODB_ENABLE

// `namespace uvcpp` 在**两条分支之外**：`#else` 里那个桩也要定义
// `uvcpp_mongo_client` 的成员，把它留在真后端那一侧会让桩落在全局命名空间里
// （症状是文件末尾一句 "expected declaration before '}'"）。
//
// 两个 `#include` 在上面的 `#if` 里、**不能**挪进命名空间：mongoc 的头里
// 有 `extern "C"` 与全局声明，套进 `namespace uvcpp` 会把它们全改名字。
namespace uvcpp {

#if UVCPP_DB_MONGODB_ENABLE

namespace {

using db_detail::bson_api;
using db_detail::mongoc_api;

// ---------------------------------------------------------------------------
// 库加载：与另外三个后端同形，只是这里要**两份**库
// ---------------------------------------------------------------------------

#if UVCPP_DB_RUNTIME_LOAD

const char* const kMongocLibraryNames[] = {
#if defined(_WIN32)
    "mongoc-1.0.dll", "libmongoc-1.0.dll", nullptr
#elif defined(__APPLE__)
    "libmongoc-1.0.0.dylib", "libmongoc-1.0.dylib", nullptr
#else
    "libmongoc-1.0.so.0", "libmongoc-1.0.so", nullptr
#endif
};

const char* const kBsonLibraryNames[] = {
#if defined(_WIN32)
    "bson-1.0.dll", "libbson-1.0.dll", nullptr
#elif defined(__APPLE__)
    "libbson-1.0.0.dylib", "libbson-1.0.dylib", nullptr
#else
    "libbson-1.0.so.0", "libbson-1.0.so", nullptr
#endif
};

/// 逐个取符号，缺一个就整张表作废（理由见 `uvcpp_db_pgsql.cpp` 里同一处）。
bool bind_bson_api(void* handle, bson_api* t, const char** missing) {
#define UVCPP_DB_BSON_BIND(name)                          \
  t->name = reinterpret_cast<decltype(t->name)>(          \
      db_detail::uvcpp_db_lookup_symbol(handle, #name));  \
  if (t->name == nullptr) {                               \
    *missing = #name;                                     \
    return false;                                         \
  }
  UVCPP_DB_BSON_SYMBOLS(UVCPP_DB_BSON_BIND)
#undef UVCPP_DB_BSON_BIND
  return true;
}

bool bind_mongoc_api(void* handle, mongoc_api* t, const char** missing) {
#define UVCPP_DB_MONGOC_BIND(name)                        \
  t->name = reinterpret_cast<decltype(t->name)>(          \
      db_detail::uvcpp_db_lookup_symbol(handle, #name));  \
  if (t->name == nullptr) {                               \
    *missing = #name;                                     \
    return false;                                         \
  }
  UVCPP_DB_MONGOC_SYMBOLS(UVCPP_DB_MONGOC_BIND)
#undef UVCPP_DB_MONGOC_BIND
  return true;
}

struct api_state {
  bson_api bson;
  mongoc_api mongoc;
  bool ok;
  std::string error;

  api_state() : bson(), mongoc(), ok(false), error() {}
};

/// 打开两份库并取符号。**两份都要**：
///
///  * `libmongoc-1.0` 自己 `DT_NEEDED` 了 `libbson-1.0`，所以只开 mongoc 时
///    `dlsym(mongoc_handle, "bson_…")` 在 glibc 上**可能**也能找到（句柄的
///    依赖作用域）。但那是 `dlsym` 的实现细节，Windows 的 `GetProcAddress`
///    完全不认这套 —— 而这份代码两个平台都要跑。所以两份各开各的、各取各的
///    符号。
///  * 两份各有自己的环境变量（`UVCPP_DB_MONGOC_LIBRARY` /
///    `UVCPP_DB_BSON_LIBRARY`）：使用者把 mongo-c-driver 装在非标准前缀下时，
///    显式指路是唯一确定的路。
///
/// 任何一份打不开都不影响进程：返回 `NO_DRIVER` + 一段列出试过哪些库名的
/// 错误串。这就是"缺库不报错"落地的位置。
api_state load_apis() {
  api_state st;

  std::string tried;
  std::string loaded;
  void* mongoc_handle = db_detail::uvcpp_db_open_client_library(
      kMongocLibraryNames, "UVCPP_DB_MONGOC_LIBRARY", &tried, &loaded);
  if (mongoc_handle == nullptr) {
    st.error =
        "加载 MongoDB 客户端库失败，试过这些名字都没打开：" + tried +
        "。装一份 mongo-c-driver（Debian/Ubuntu 上是 libmongoc-1.0-0 与 "
        "libbson-1.0-0，RHEL 系是 mongo-c-driver，macOS 上是 mongo-c-driver），"
        "或者把环境变量 UVCPP_DB_MONGOC_LIBRARY 指到 libmongoc 的完整路径。"
        "不用 MongoDB 的话不用管这一条。";
    return st;
  }
  const std::string mongoc_name = loaded;

  void* bson_handle = db_detail::uvcpp_db_open_client_library(
      kBsonLibraryNames, "UVCPP_DB_BSON_LIBRARY", &tried, &loaded);
  if (bson_handle == nullptr) {
    st.error =
        "加载 libbson 失败，试过这些名字都没打开：" + tried +
        "（libmongoc 已经从 " + mongoc_name +
        " 打开了）。通常它与 libmongoc 装在同一个前缀下；也可以把环境变量 "
        "UVCPP_DB_BSON_LIBRARY 指到 libbson 的完整路径。";
    return st;
  }
  const std::string bson_name = loaded;

  const char* missing = nullptr;
  if (!bind_bson_api(bson_handle, &st.bson, &missing)) {
    st.error = "libbson（" + bson_name + "）里缺少符号 " + missing +
               " —— 版本太旧或不是 libbson。";
    return st;
  }
  if (!bind_mongoc_api(mongoc_handle, &st.mongoc, &missing)) {
    st.error = "libmongoc（" + mongoc_name + "）里缺少符号 " + missing +
               " —— 版本太旧或不是 libmongoc。";
    return st;
  }

  // 每进程一次。`mongoc_cleanup` 有意不调：与另外三份"句柄永不 dlclose"
  // 是同一条理由 —— 进程退出由内核回收，而手工清理要求"再也没有活连接"，
  // 那个条件在本库里无法可靠判定。
  st.mongoc.mongoc_init();
  st.ok = true;
  return st;
}

api_state& api_state_ref() {
  // 第一次真要用 MongoDB 时才加载，不是加载这份库的时候。理由同 MySQL 那边：
  // 一份从不连 MongoDB 的进程不该因为缺一份客户端库而多出一行错误。
  static api_state st = load_apis();
  return st;
}

#else  // 直接链接

#define UVCPP_DB_BSON_ADDR(name) &::name,
const bson_api kBsonApi = {UVCPP_DB_BSON_SYMBOLS(UVCPP_DB_BSON_ADDR)};
#undef UVCPP_DB_BSON_ADDR

#define UVCPP_DB_MONGOC_ADDR(name) &::name,
const mongoc_api kMongocApi = {UVCPP_DB_MONGOC_SYMBOLS(UVCPP_DB_MONGOC_ADDR)};
#undef UVCPP_DB_MONGOC_ADDR

/// 直接链接形态下"加载成功"是编译期的事实，唯一要做的是 `mongoc_init()`。
bool backend_loaded() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, []() {
    kMongocApi.mongoc_init();
    ok = true;
  });
  return ok;
}

#endif  // UVCPP_DB_RUNTIME_LOAD

const bson_api& bson_a() { return db_detail::uvcpp_db_bson_api(); }
const mongoc_api& mongoc_a() { return db_detail::uvcpp_db_mongoc_api(); }

/// `bson_error_t` → 一句人话。`message` 是 libmongoc 自己给的那句（认证失败、
/// 主机不可达、超时……），domain/code 是它的机器可读那一半 —— 两半都留着，
/// 因为前者给人看、后者给日志过滤用。
std::string error_text(const bson_error_t& e) {
  std::string text = e.message;
  if (text.empty()) text = "libmongoc 没有给出错误文本";
  text += "（domain=" + std::to_string(e.domain) +
          ", code=" + std::to_string(e.code) + "）";
  return text;
}

void clear_error(bson_error_t* e) { std::memset(e, 0, sizeof(*e)); }

/// 把一段 Extended JSON 解析成堆上的 `bson_t`。空串按 `{}` 处理（`find` 的
/// filter/options 允许省略）。失败返回 `nullptr`，原因在 `e` 里。
bson_t* parse_doc(const std::string& text, bson_error_t* e) {
  clear_error(e);
  const std::string body = text.empty() ? std::string("{}") : text;
  return bson_a().bson_new_from_json(
      reinterpret_cast<const uint8_t*>(body.data()),
      static_cast<ssize_t>(body.size()), e);
}

/// 堆上的 `bson_t` → canonical Extended JSON 文本。失败返回空串。
std::string to_json(const bson_t* doc) {
  size_t len = 0;
  char* s = bson_a().bson_as_canonical_extended_json(doc, &len);
  if (s == nullptr) return std::string();
  std::string out(s, len);
  bson_a().bson_free(s);
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// db_detail：两张函数表的正式取法（头里声明的那三个）
// ---------------------------------------------------------------------------

namespace db_detail {

#if UVCPP_DB_RUNTIME_LOAD

const bson_api& uvcpp_db_bson_api() { return api_state_ref().bson; }
const mongoc_api& uvcpp_db_mongoc_api() { return api_state_ref().mongoc; }
const std::string& uvcpp_db_mongodb_load_error() {
  return api_state_ref().error;
}

#else  // 直接链接

const bson_api& uvcpp_db_bson_api() { return kBsonApi; }
const mongoc_api& uvcpp_db_mongoc_api() { return kMongocApi; }

#endif  // UVCPP_DB_RUNTIME_LOAD

}  // namespace db_detail

// ---------------------------------------------------------------------------
// impl
// ---------------------------------------------------------------------------

namespace {

/// 运行时加载形态下"加载成功没有 + 失败原因"；直接链接形态下永远成功。
bool backend_ready(std::string* why) {
#if UVCPP_DB_RUNTIME_LOAD
  const api_state& st = api_state_ref();
  if (!st.ok && why != nullptr) *why = st.error;
  return st.ok;
#else
  (void)why;
  return backend_loaded();
#endif
}

}  // namespace

struct uvcpp_mongo_client::impl : std::enable_shared_from_this<impl> {
  struct job;

  std::recursive_mutex mu;
  mongoc_client_t* client = nullptr;
  mongoc_uri_t* uri = nullptr;
  std::string uri_text;
  std::string error;

  ~impl() {
    std::lock_guard<std::recursive_mutex> lk(mu);
    close_locked();
  }

  void close_locked() {
    if (client != nullptr) {
      mongoc_a().mongoc_client_destroy(client);
      client = nullptr;
    }
    if (uri != nullptr) {
      mongoc_a().mongoc_uri_destroy(uri);
      uri = nullptr;
    }
  }

  /// 连接没打开时把原因写进 `error` 并返回 `NOT_CONNECTED`。
  uvcpp_db_status require_open_locked() {
    if (client != nullptr) return uvcpp_db_status::OK;
    error = "连接没有打开（先调用 open()）";
    return uvcpp_db_status::NOT_CONNECTED;
  }

  uvcpp_db_status open_locked(const std::string& text);

  uvcpp_db_status insert_locked(const std::string& db,
                                const std::string& collection,
                                const std::string& document);
  uvcpp_db_status update_locked(const std::string& db,
                                const std::string& collection,
                                const std::string& filter,
                                const std::string& update, bool upsert);
  uvcpp_db_status delete_locked(const std::string& db,
                                const std::string& collection,
                                const std::string& filter);
  uvcpp_db_status find_locked(const std::string& db,
                              const std::string& collection,
                              const std::string& filter,
                              const std::string& options,
                              std::vector<std::string>* out);
  uvcpp_db_status command_locked(const std::string& db,
                                 const std::string& command,
                                 std::string* out_json);
  uvcpp_db_status ping_locked();

  std::shared_ptr<job> new_job(int kind);
  int launch(uvcpp_loop* loop, const std::shared_ptr<job>& j);

  int insert_async(uvcpp_loop* loop, const std::string& db,
                   const std::string& collection, const std::string& document,
                   uvcpp_mongo_client::simple_cb cb);
  int update_async(uvcpp_loop* loop, const std::string& db,
                   const std::string& collection, const std::string& filter,
                   const std::string& update, bool upsert,
                   uvcpp_mongo_client::simple_cb cb);
  int delete_async(uvcpp_loop* loop, const std::string& db,
                   const std::string& collection, const std::string& filter,
                   uvcpp_mongo_client::simple_cb cb);
  int find_async(uvcpp_loop* loop, const std::string& db,
                 const std::string& collection, const std::string& filter,
                 const std::string& options, uvcpp_mongo_client::find_cb cb);
  int command_async(uvcpp_loop* loop, const std::string& db,
                    const std::string& command,
                    uvcpp_mongo_client::simple_cb cb);
  int ping_async(uvcpp_loop* loop, uvcpp_mongo_client::simple_cb cb);
};

/// 一次异步操作的全部状态。
///
/// 它自己拿住 `shared_ptr<impl>`：公开对象在回调里被析构时，在飞的这一笔
/// 仍然有一份完整的内部状态（含 `mongoc_client_t`），不会踩到释放后的内存。
struct uvcpp_mongo_client::impl::job {
  enum kind_t { K_INSERT, K_UPDATE, K_DELETE, K_FIND, K_COMMAND, K_PING };

  int kind = K_INSERT;
  std::shared_ptr<impl> im;
  std::string db;
  std::string coll;
  std::string first;
  std::string second;
  bool flag = false;

  // ---- 结果：池线程写，循环线程读 ----
  uvcpp_db_status status = uvcpp_db_status::MISUSE;
  std::string message;
  std::vector<std::string> docs;

  // ---- 交付 ----
  uvcpp_mongo_client::simple_cb simple;
  uvcpp_mongo_client::find_cb find;

  /// 池线程。整段持锁 —— 与同步调用争的就是这把锁，而"一条连接上同一时刻
  /// 只跑一条操作"正是本类对外承诺的那条。
  void run() {
    std::lock_guard<std::recursive_mutex> lk(im->mu);
    switch (kind) {
      case K_INSERT:
        status = im->insert_locked(db, coll, first);
        break;
      case K_UPDATE:
        status = im->update_locked(db, coll, first, second, flag);
        break;
      case K_DELETE:
        status = im->delete_locked(db, coll, first);
        break;
      case K_FIND:
        status = im->find_locked(db, coll, first, second, &docs);
        break;
      case K_COMMAND:
        status = im->command_locked(db, first, nullptr);
        break;
      case K_PING:
        status = im->ping_locked();
        break;
    }
    if (status != uvcpp_db_status::OK) message = im->error;
  }

  /// 循环线程。**恰好调一次**（投递成功的前提下）。
  void deliver() {
    if (find) {
      find(status, docs, message);
    } else if (simple) {
      simple(status, message);
    }
  }
};

std::shared_ptr<uvcpp_mongo_client::impl::job>
uvcpp_mongo_client::impl::new_job(int kind) {
  std::shared_ptr<job> j(new job());
  j->kind = kind;
  j->im = shared_from_this();
  return j;
}

int uvcpp_mongo_client::impl::launch(uvcpp_loop* loop,
                                     const std::shared_ptr<job>& j) {
  uvcpp_work* w = nullptr;
  try {
    w = new uvcpp_work();
  } catch (...) {
    return UV_ENOMEM;
  }
  w->set_self_free(true);

  int rc = 0;
  try {
    rc = w->queue_work(
        loop, [j](uvcpp_work*) { j->run(); },
        [j](uvcpp_work*, int status) {
          if (status != 0) {
            // 只可能是 `uv_cancel`，本类从不取消 —— 但空着的话回调会带着
            // 默认的 MISUSE 和空文案出去，比说实话更难查。
            j->status = uvcpp_db_status::MISUSE;
            j->message = std::string("异步工作被 libuv 取消（status=") +
                         std::to_string(status) + "）";
          }
          j->deliver();
        });
  } catch (...) {
    // `uvcpp_work::queue_work` 抛的是裸 `const char*`（loop 为空）。
    rc = UV_EINVAL;
  }

  if (rc != 0) {
    // 没投出去：libuv 两个回调都不会跑，work 得自己收。先摘掉 self_free，
    // 免得将来某条路上被第二次删除。
    w->set_self_free(false);
    delete w;
  }
  return rc;
}

uvcpp_db_status uvcpp_mongo_client::impl::open_locked(const std::string& text) {
  close_locked();
  uri_text.clear();

  std::string why;
  if (!backend_ready(&why)) {
    error = why.empty() ? std::string("这份构建没有可用的 MongoDB 后端") : why;
    return uvcpp_db_status::NO_DRIVER;
  }

  bson_error_t e;
  clear_error(&e);
  mongoc_uri_t* u = mongoc_a().mongoc_uri_new_with_error(text.c_str(), &e);
  if (u == nullptr) {
    error = "MongoDB 连接串解析失败：" + error_text(e) +
            "。期望形状是 mongodb://[user:pass@]host[:port][/db][?options] "
            "或 mongodb+srv://…";
    return uvcpp_db_status::BAD_URL;
  }

  mongoc_client_t* c = mongoc_a().mongoc_client_new_from_uri(u);
  if (c == nullptr) {
    mongoc_a().mongoc_uri_destroy(u);
    error = "libmongoc 没能从连接串建出客户端（内存不足？）";
    return uvcpp_db_status::OPEN_FAILED;
  }

  // **真的连一次**：`mongoc_client_new_from_uri` 只做拓扑发现，不等服务器。
  // 不 ping 的话 `open()` 会在"语法对、服务器不在"时返回 OK，而那条连接
  // 直到第一次操作才失败 —— 与 `uvcpp_db_client::open()` 的语义不一致。
  //
  // 等待上限由 URI 的 `serverSelectionTimeoutMS` 决定（libmongoc 默认 30 秒）。
  bson_t reply = BSON_INITIALIZER;
  bson_t* ping_cmd = parse_doc("{\"ping\": 1}", &e);
  bool ok = false;
  if (ping_cmd != nullptr) {
    ok = mongoc_a().mongoc_client_command_simple(c, "admin", ping_cmd, nullptr,
                                                 &reply, &e);
    bson_a().bson_destroy(ping_cmd);
  }
  bson_a().bson_destroy(&reply);

  if (!ok) {
    mongoc_a().mongoc_client_destroy(c);
    mongoc_a().mongoc_uri_destroy(u);
    error = "连接 MongoDB 失败：" + error_text(e) +
            "。连不上时请检查服务是否启动、以及连接串里的 "
            "serverSelectionTimeoutMS（libmongoc 默认 30000 毫秒）";
    return uvcpp_db_status::OPEN_FAILED;
  }

  client = c;
  uri = u;
  uri_text = text;
  error.clear();
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_mongo_client::impl::insert_locked(
    const std::string& db, const std::string& collection,
    const std::string& document) {
  uvcpp_db_status st = require_open_locked();
  if (st != uvcpp_db_status::OK) return st;

  bson_error_t e;
  bson_t* doc = parse_doc(document, &e);
  if (doc == nullptr) {
    error = "insert_one 的 document 不是合法的 Extended JSON：" +
            error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }

  mongoc_collection_t* c = mongoc_a().mongoc_client_get_collection(
      client, db.c_str(), collection.c_str());
  if (c == nullptr) {
    bson_a().bson_destroy(doc);
    error = "拿不到 collection 句柄（内存不足？）";
    return uvcpp_db_status::EXEC_FAILED;
  }

  bson_t reply = BSON_INITIALIZER;
  clear_error(&e);
  const bool ok =
      mongoc_a().mongoc_collection_insert_one(c, doc, nullptr, &reply, &e);
  bson_a().bson_destroy(&reply);
  bson_a().bson_destroy(doc);
  mongoc_a().mongoc_collection_destroy(c);

  if (!ok) {
    error = "insert_one 失败：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_mongo_client::impl::update_locked(
    const std::string& db, const std::string& collection,
    const std::string& filter, const std::string& update, bool upsert) {
  uvcpp_db_status st = require_open_locked();
  if (st != uvcpp_db_status::OK) return st;

  bson_error_t e;
  bson_t* selector = parse_doc(filter, &e);
  if (selector == nullptr) {
    error = "update_one 的 filter 不是合法的 Extended JSON：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  bson_t* upd = parse_doc(update, &e);
  if (upd == nullptr) {
    bson_a().bson_destroy(selector);
    error = "update_one 的 update 不是合法的 Extended JSON：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  bson_t* opts = nullptr;
  if (upsert) {
    opts = parse_doc("{\"upsert\": true}", &e);
    if (opts == nullptr) {
      bson_a().bson_destroy(selector);
      bson_a().bson_destroy(upd);
      error = "构造 upsert 选项失败：" + error_text(e);
      return uvcpp_db_status::EXEC_FAILED;
    }
  }

  mongoc_collection_t* c = mongoc_a().mongoc_client_get_collection(
      client, db.c_str(), collection.c_str());
  if (c == nullptr) {
    bson_a().bson_destroy(selector);
    bson_a().bson_destroy(upd);
    if (opts != nullptr) bson_a().bson_destroy(opts);
    error = "拿不到 collection 句柄（内存不足？）";
    return uvcpp_db_status::EXEC_FAILED;
  }

  bson_t reply = BSON_INITIALIZER;
  clear_error(&e);
  const bool ok = mongoc_a().mongoc_collection_update_one(
      c, selector, upd, opts, &reply, &e);
  bson_a().bson_destroy(&reply);
  bson_a().bson_destroy(selector);
  bson_a().bson_destroy(upd);
  if (opts != nullptr) bson_a().bson_destroy(opts);
  mongoc_a().mongoc_collection_destroy(c);

  if (!ok) {
    error = "update_one 失败：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_mongo_client::impl::delete_locked(
    const std::string& db, const std::string& collection,
    const std::string& filter) {
  uvcpp_db_status st = require_open_locked();
  if (st != uvcpp_db_status::OK) return st;

  bson_error_t e;
  bson_t* selector = parse_doc(filter, &e);
  if (selector == nullptr) {
    error = "delete_one 的 filter 不是合法的 Extended JSON：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }

  mongoc_collection_t* c = mongoc_a().mongoc_client_get_collection(
      client, db.c_str(), collection.c_str());
  if (c == nullptr) {
    bson_a().bson_destroy(selector);
    error = "拿不到 collection 句柄（内存不足？）";
    return uvcpp_db_status::EXEC_FAILED;
  }

  bson_t reply = BSON_INITIALIZER;
  clear_error(&e);
  const bool ok =
      mongoc_a().mongoc_collection_delete_one(c, selector, nullptr, &reply, &e);
  bson_a().bson_destroy(&reply);
  bson_a().bson_destroy(selector);
  mongoc_a().mongoc_collection_destroy(c);

  if (!ok) {
    error = "delete_one 失败：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_mongo_client::impl::find_locked(
    const std::string& db, const std::string& collection,
    const std::string& filter, const std::string& options,
    std::vector<std::string>* out) {
  if (out == nullptr) {
    error = "find() 的 out 是空的";
    return uvcpp_db_status::MISUSE;
  }
  out->clear();

  uvcpp_db_status st = require_open_locked();
  if (st != uvcpp_db_status::OK) return st;

  bson_error_t e;
  bson_t* filter_doc = parse_doc(filter, &e);
  if (filter_doc == nullptr) {
    error = "find 的 filter 不是合法的 Extended JSON：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  bson_t* opts_doc = nullptr;
  if (!options.empty()) {
    opts_doc = parse_doc(options, &e);
    if (opts_doc == nullptr) {
      bson_a().bson_destroy(filter_doc);
      error = "find 的 options 不是合法的 Extended JSON：" + error_text(e);
      return uvcpp_db_status::EXEC_FAILED;
    }
  }

  mongoc_collection_t* c = mongoc_a().mongoc_client_get_collection(
      client, db.c_str(), collection.c_str());
  if (c == nullptr) {
    bson_a().bson_destroy(filter_doc);
    if (opts_doc != nullptr) bson_a().bson_destroy(opts_doc);
    error = "拿不到 collection 句柄（内存不足？）";
    return uvcpp_db_status::EXEC_FAILED;
  }

  mongoc_cursor_t* cursor = mongoc_a().mongoc_collection_find_with_opts(
      c, filter_doc, opts_doc, nullptr);
  bson_a().bson_destroy(filter_doc);
  if (opts_doc != nullptr) bson_a().bson_destroy(opts_doc);
  mongoc_a().mongoc_collection_destroy(c);

  if (cursor == nullptr) {
    error = "find 没能建出游标（内存不足？）";
    return uvcpp_db_status::EXEC_FAILED;
  }

  const bson_t* doc = nullptr;
  while (mongoc_a().mongoc_cursor_next(cursor, &doc)) {
    std::string text = to_json(doc);
    if (!text.empty()) out->push_back(text);
  }

  // `mongoc_cursor_next` 返回 false 有两种：取完了，或者出错了。不区分的话，
  // "连接断了"会被报成"一条都没有" —— 那正是本仓最忌讳的那种绿。
  clear_error(&e);
  const bool failed = mongoc_a().mongoc_cursor_error(cursor, &e);
  mongoc_a().mongoc_cursor_destroy(cursor);
  if (failed) {
    out->clear();
    error = "find 取结果时失败：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_mongo_client::impl::command_locked(
    const std::string& db, const std::string& command, std::string* out_json) {
  if (out_json != nullptr) out_json->clear();

  uvcpp_db_status st = require_open_locked();
  if (st != uvcpp_db_status::OK) return st;

  bson_error_t e;
  bson_t* cmd = parse_doc(command, &e);
  if (cmd == nullptr) {
    error = "command 不是合法的 Extended JSON：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }

  bson_t reply = BSON_INITIALIZER;
  clear_error(&e);
  const bool ok = mongoc_a().mongoc_client_command_simple(
      client, db.c_str(), cmd, nullptr, &reply, &e);
  bson_a().bson_destroy(cmd);

  if (!ok) {
    bson_a().bson_destroy(&reply);
    error = "command 失败：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  if (out_json != nullptr) *out_json = to_json(&reply);
  bson_a().bson_destroy(&reply);
  return uvcpp_db_status::OK;
}

uvcpp_db_status uvcpp_mongo_client::impl::ping_locked() {
  uvcpp_db_status st = require_open_locked();
  if (st != uvcpp_db_status::OK) return st;

  bson_error_t e;
  bson_t* cmd = parse_doc("{\"ping\": 1}", &e);
  if (cmd == nullptr) {
    error = "构造 ping 失败：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  bson_t reply = BSON_INITIALIZER;
  clear_error(&e);
  const bool ok = mongoc_a().mongoc_client_command_simple(
      client, "admin", cmd, nullptr, &reply, &e);
  bson_a().bson_destroy(cmd);
  bson_a().bson_destroy(&reply);
  if (!ok) {
    error = "ping 失败：" + error_text(e);
    return uvcpp_db_status::EXEC_FAILED;
  }
  return uvcpp_db_status::OK;
}

// -- 异步的投递：每个 impl 方法只做"装 job + 投" --

int uvcpp_mongo_client::impl::insert_async(
    uvcpp_loop* loop, const std::string& db, const std::string& collection,
    const std::string& document, uvcpp_mongo_client::simple_cb cb) {
  std::shared_ptr<job> j;
  try {
    j = new_job(job::K_INSERT);
  } catch (...) {
    return UV_ENOMEM;
  }
  j->db = db;
  j->coll = collection;
  j->first = document;
  j->simple = cb;
  return launch(loop, j);
}

int uvcpp_mongo_client::impl::update_async(
    uvcpp_loop* loop, const std::string& db, const std::string& collection,
    const std::string& filter, const std::string& update, bool upsert,
    uvcpp_mongo_client::simple_cb cb) {
  std::shared_ptr<job> j;
  try {
    j = new_job(job::K_UPDATE);
  } catch (...) {
    return UV_ENOMEM;
  }
  j->db = db;
  j->coll = collection;
  j->first = filter;
  j->second = update;
  j->flag = upsert;
  j->simple = cb;
  return launch(loop, j);
}

int uvcpp_mongo_client::impl::delete_async(
    uvcpp_loop* loop, const std::string& db, const std::string& collection,
    const std::string& filter, uvcpp_mongo_client::simple_cb cb) {
  std::shared_ptr<job> j;
  try {
    j = new_job(job::K_DELETE);
  } catch (...) {
    return UV_ENOMEM;
  }
  j->db = db;
  j->coll = collection;
  j->first = filter;
  j->simple = cb;
  return launch(loop, j);
}

int uvcpp_mongo_client::impl::find_async(
    uvcpp_loop* loop, const std::string& db, const std::string& collection,
    const std::string& filter, const std::string& options,
    uvcpp_mongo_client::find_cb cb) {
  std::shared_ptr<job> j;
  try {
    j = new_job(job::K_FIND);
  } catch (...) {
    return UV_ENOMEM;
  }
  j->db = db;
  j->coll = collection;
  j->first = filter;
  j->second = options;
  j->find = cb;
  return launch(loop, j);
}

int uvcpp_mongo_client::impl::command_async(
    uvcpp_loop* loop, const std::string& db, const std::string& command,
    uvcpp_mongo_client::simple_cb cb) {
  std::shared_ptr<job> j;
  try {
    j = new_job(job::K_COMMAND);
  } catch (...) {
    return UV_ENOMEM;
  }
  j->db = db;
  j->first = command;
  j->simple = cb;
  return launch(loop, j);
}

int uvcpp_mongo_client::impl::ping_async(uvcpp_loop* loop,
                                         uvcpp_mongo_client::simple_cb cb) {
  std::shared_ptr<job> j;
  try {
    j = new_job(job::K_PING);
  } catch (...) {
    return UV_ENOMEM;
  }
  j->simple = cb;
  return launch(loop, j);
}

// ---------------------------------------------------------------------------
// uvcpp_mongo_client：公开面
// ---------------------------------------------------------------------------

uvcpp_mongo_client::uvcpp_mongo_client() : impl_(new impl()) {}

uvcpp_mongo_client::~uvcpp_mongo_client() {}

uvcpp_db_status uvcpp_mongo_client::open(const std::string& uri) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->open_locked(uri);
}

void uvcpp_mongo_client::close() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  impl_->close_locked();
}

bool uvcpp_mongo_client::is_open() const {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->client != nullptr;
}

uvcpp_db_status uvcpp_mongo_client::ping() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->ping_locked();
}

uvcpp_db_status uvcpp_mongo_client::insert_one(const std::string& db,
                                               const std::string& collection,
                                               const std::string& document) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->insert_locked(db, collection, document);
}

uvcpp_db_status uvcpp_mongo_client::update_one(
    const std::string& db, const std::string& collection,
    const std::string& filter, const std::string& update, bool upsert) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->update_locked(db, collection, filter, update, upsert);
}

uvcpp_db_status uvcpp_mongo_client::delete_one(
    const std::string& db, const std::string& collection,
    const std::string& filter) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->delete_locked(db, collection, filter);
}

uvcpp_db_status uvcpp_mongo_client::find(const std::string& db,
                                         const std::string& collection,
                                         const std::string& filter,
                                         const std::string& options,
                                         std::vector<std::string>* out) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->find_locked(db, collection, filter, options, out);
}

uvcpp_db_status uvcpp_mongo_client::command(const std::string& db,
                                            const std::string& command,
                                            std::string* out_json) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  return impl_->command_locked(db, command, out_json);
}

const std::string& uvcpp_mongo_client::last_error() const {
  return impl_->error;
}

const std::string& uvcpp_mongo_client::uri() const { return impl_->uri_text; }

bool uvcpp_mongo_client::backend_available() { return true; }

int uvcpp_mongo_client::insert_one_async(uvcpp_loop* loop,
                                         const std::string& db,
                                         const std::string& collection,
                                         const std::string& document,
                                         simple_cb cb) {
  return impl_->insert_async(loop, db, collection, document, cb);
}

int uvcpp_mongo_client::update_one_async(uvcpp_loop* loop,
                                         const std::string& db,
                                         const std::string& collection,
                                         const std::string& filter,
                                         const std::string& update, bool upsert,
                                         simple_cb cb) {
  return impl_->update_async(loop, db, collection, filter, update, upsert, cb);
}

int uvcpp_mongo_client::delete_one_async(uvcpp_loop* loop,
                                         const std::string& db,
                                         const std::string& collection,
                                         const std::string& filter,
                                         simple_cb cb) {
  return impl_->delete_async(loop, db, collection, filter, cb);
}

int uvcpp_mongo_client::find_async(uvcpp_loop* loop, const std::string& db,
                                   const std::string& collection,
                                   const std::string& filter,
                                   const std::string& options, find_cb cb) {
  return impl_->find_async(loop, db, collection, filter, options, cb);
}

int uvcpp_mongo_client::command_async(uvcpp_loop* loop, const std::string& db,
                                      const std::string& command,
                                      simple_cb cb) {
  return impl_->command_async(loop, db, command, cb);
}

int uvcpp_mongo_client::ping_async(uvcpp_loop* loop, simple_cb cb) {
  return impl_->ping_async(loop, cb);
}

// ===========================================================================
// 桩：这份构建没有编 MongoDB 后端
// ===========================================================================

#else  // !UVCPP_DB_MONGODB_ENABLE

namespace {

const char* const kNoBackend =
    "这份构建没有编 MongoDB 后端（配置期没找到 mongoc.h / bson.h，或者显式关了 "
    "UVCPP_ENABLE_DB_MONGODB）。MongoDB 的客户端库本仓不随包分发：要么装一份 "
    "mongo-c-driver 的开发头再重新构建，要么改用别的存储。这一条只影响 "
    "MongoDB，进程与其它模块照常。";

}  // namespace

struct uvcpp_mongo_client::impl {
  std::string error;
};

uvcpp_mongo_client::uvcpp_mongo_client() : impl_(new impl()) {}

uvcpp_mongo_client::~uvcpp_mongo_client() {}

uvcpp_db_status uvcpp_mongo_client::open(const std::string& uri) {
  (void)uri;
  impl_->error = kNoBackend;
  return uvcpp_db_status::NO_DRIVER;
}

void uvcpp_mongo_client::close() {}

bool uvcpp_mongo_client::is_open() const { return false; }

uvcpp_db_status uvcpp_mongo_client::ping() {
  impl_->error = kNoBackend;
  return uvcpp_db_status::NO_DRIVER;
}

uvcpp_db_status uvcpp_mongo_client::insert_one(const std::string& db,
                                               const std::string& collection,
                                               const std::string& document) {
  (void)db;
  (void)collection;
  (void)document;
  impl_->error = kNoBackend;
  return uvcpp_db_status::NO_DRIVER;
}

uvcpp_db_status uvcpp_mongo_client::update_one(
    const std::string& db, const std::string& collection,
    const std::string& filter, const std::string& update, bool upsert) {
  (void)db;
  (void)collection;
  (void)filter;
  (void)update;
  (void)upsert;
  impl_->error = kNoBackend;
  return uvcpp_db_status::NO_DRIVER;
}

uvcpp_db_status uvcpp_mongo_client::delete_one(
    const std::string& db, const std::string& collection,
    const std::string& filter) {
  (void)db;
  (void)collection;
  (void)filter;
  impl_->error = kNoBackend;
  return uvcpp_db_status::NO_DRIVER;
}

uvcpp_db_status uvcpp_mongo_client::find(const std::string& db,
                                         const std::string& collection,
                                         const std::string& filter,
                                         const std::string& options,
                                         std::vector<std::string>* out) {
  (void)db;
  (void)collection;
  (void)filter;
  (void)options;
  if (out != nullptr) out->clear();
  impl_->error = kNoBackend;
  return uvcpp_db_status::NO_DRIVER;
}

uvcpp_db_status uvcpp_mongo_client::command(const std::string& db,
                                            const std::string& command,
                                            std::string* out_json) {
  (void)db;
  (void)command;
  if (out_json != nullptr) out_json->clear();
  impl_->error = kNoBackend;
  return uvcpp_db_status::NO_DRIVER;
}

const std::string& uvcpp_mongo_client::last_error() const {
  return impl_->error;
}

const std::string& uvcpp_mongo_client::uri() const {
  static const std::string kEmpty;
  return kEmpty;
}

bool uvcpp_mongo_client::backend_available() { return false; }

// 桩里的异步：同步地兑现一个"没有后端"的结果，但**投递语义不变** ——
// 返回值 0（已受理）并且回调在**这条循环的线程**上被调一次。用 `uv_queue_work`
// 而不是当场调，是因为当场调会让调用方拿到与真后端不同的重入形状（回调嵌在
// 调用栈里），那正好是本仓在别处反复避免的"两种路径两种时序"。
//
// 真后端那条路投递失败时回调不来；这里也照做：`loop` 为空返回 `UV_EINVAL`。

namespace {

/// 桩上的一个异步任务：没有真的活要干，只把结果送回去。
struct stub_job {
  uvcpp_db_status status = uvcpp_db_status::NO_DRIVER;
  std::string message;
  std::vector<std::string> docs;
  uvcpp_mongo_client::simple_cb simple;
  uvcpp_mongo_client::find_cb find;
};

int stub_launch(uvcpp_loop* loop,
                const std::shared_ptr<stub_job>& j,
                const char* message) {
  j->status = uvcpp_db_status::NO_DRIVER;
  j->message = message;
  uvcpp_work* w = nullptr;
  try {
    w = new uvcpp_work();
  } catch (...) {
    return UV_ENOMEM;
  }
  w->set_self_free(true);
  int rc = 0;
  try {
    rc = w->queue_work(
        loop, [j](uvcpp_work*) {},
        [j](uvcpp_work*, int status) {
          if (status != 0) {
            j->status = uvcpp_db_status::MISUSE;
            j->message = std::string("异步工作被 libuv 取消（status=") +
                         std::to_string(status) + "）";
          }
          if (j->find) {
            j->find(j->status, j->docs, j->message);
          } else if (j->simple) {
            j->simple(j->status, j->message);
          }
        });
  } catch (...) {
    rc = UV_EINVAL;
  }
  if (rc != 0) {
    w->set_self_free(false);
    delete w;
  }
  return rc;
}

}  // namespace

int uvcpp_mongo_client::insert_one_async(uvcpp_loop* loop,
                                         const std::string& db,
                                         const std::string& collection,
                                         const std::string& document,
                                         simple_cb cb) {
  (void)db;
  (void)collection;
  (void)document;
  std::shared_ptr<stub_job> j(new (std::nothrow) stub_job());
  if (!j) return UV_ENOMEM;
  j->simple = cb;
  return stub_launch(loop, j, kNoBackend);
}

int uvcpp_mongo_client::update_one_async(uvcpp_loop* loop,
                                         const std::string& db,
                                         const std::string& collection,
                                         const std::string& filter,
                                         const std::string& update, bool upsert,
                                         simple_cb cb) {
  (void)db;
  (void)collection;
  (void)filter;
  (void)update;
  (void)upsert;
  std::shared_ptr<stub_job> j(new (std::nothrow) stub_job());
  if (!j) return UV_ENOMEM;
  j->simple = cb;
  return stub_launch(loop, j, kNoBackend);
}

int uvcpp_mongo_client::delete_one_async(uvcpp_loop* loop,
                                         const std::string& db,
                                         const std::string& collection,
                                         const std::string& filter,
                                         simple_cb cb) {
  (void)db;
  (void)collection;
  (void)filter;
  std::shared_ptr<stub_job> j(new (std::nothrow) stub_job());
  if (!j) return UV_ENOMEM;
  j->simple = cb;
  return stub_launch(loop, j, kNoBackend);
}

int uvcpp_mongo_client::find_async(uvcpp_loop* loop, const std::string& db,
                                   const std::string& collection,
                                   const std::string& filter,
                                   const std::string& options, find_cb cb) {
  (void)db;
  (void)collection;
  (void)filter;
  (void)options;
  std::shared_ptr<stub_job> j(new (std::nothrow) stub_job());
  if (!j) return UV_ENOMEM;
  j->find = cb;
  return stub_launch(loop, j, kNoBackend);
}

int uvcpp_mongo_client::command_async(uvcpp_loop* loop, const std::string& db,
                                      const std::string& command,
                                      simple_cb cb) {
  (void)db;
  (void)command;
  std::shared_ptr<stub_job> j(new (std::nothrow) stub_job());
  if (!j) return UV_ENOMEM;
  j->simple = cb;
  return stub_launch(loop, j, kNoBackend);
}

int uvcpp_mongo_client::ping_async(uvcpp_loop* loop, simple_cb cb) {
  std::shared_ptr<stub_job> j(new (std::nothrow) stub_job());
  if (!j) return UV_ENOMEM;
  j->simple = cb;
  return stub_launch(loop, j, kNoBackend);
}

#endif  // UVCPP_DB_MONGODB_ENABLE

}  // namespace uvcpp

#endif  // UVCPP_DB_ENABLE
