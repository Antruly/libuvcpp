/**
 * @file src/capi/uvcpp_c_db.cpp
 * @brief `uvcpp_c_db.h` 的实现：连接、同步读写、事务、参数、结果集、池、异步。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个文件里五处值得先读
 * ----------------------
 * 1. **`uvcpp_c_db_value` 是一个"空指针类型"，不是结构体。** 它没有定义（头里
 *    也只有前置声明），因为 C 面的值视图是**借来的**：`_cell()` 交出来的那个
 *    指针就是把 `const uvcpp_db_value*` 原样 `reinterpret_cast` 过来的，读它的
 *    每个函数再转回去。零拷贝、零分配，代价是**这个类型永远不能有定义** ——
 *    给它加一个真结构体，这里每一次 `reinterpret_cast` 就都成了类型混淆。
 *    越界那一格也不需要额外分支：`uvcpp_db_table::at()` 本来就返回一枚静态
 *    NULL 值，转出来正好就是"这一格没有"。
 *
 * 2. **句柄的真身都定义在全局作用域**（头里 `typedef struct uvcpp_c_db_xxx` 已经
 *    把标签声明在全局，写进匿名命名空间就是另一个类型了），且 `handle_head` 在
 *    偏移 0 上有一串 `static_assert` 钉着。
 *
 * 3. **一张表句柄拥有一份 `uvcpp_db_table` 的拷贝。** C++ 那侧 `query()` 收的是
 *    出参指针、`uvcpp_db_async` 递的是 `const&`（只在那次回调里有效），而 C 面
 *    两种情况下都承诺"这个表归你、回调返回后仍然有效" —— 所以交付前一律搬一份
 *    进句柄。`_cell()` 的视图因此指向句柄**内部**，句柄一没它就失效（头里那三类
 *    所有权里的第 4 类）。
 *
 * 4. **池子记着它发出去的每一枚 C 句柄。** `_pool_acquire()` 借出的那枚 C 句柄是
 *    **借来的**：`_client_free()` 对它返回 `E_STATE`，而 `_pool_release()` /
 *    `_pool_discard()` / `_pool_free()` 会**反登记并删掉**它 —— 于是"还回去之后
 *    再拿旧句柄用"得到的是 `E_STALE`，不是野指针。为此池子句柄里有一张
 *    `out_`（C 句柄 → 底下的 C++ 连接）。
 *
 * 5. **异步那条自带循环线程，闭包只碰一份 `shared_ptr` 状态，绝不碰 C 句柄。**
 *    于是"回调里 `free()` 门面自己"是安全的（那是库里最自然的用法），而
 *    `free()` 会等在途的活收完（有界：数据库调用自己有超时兜底）。线程的形状
 *    是从 `db/uvcpp_db_async.cpp` 那一段照抄的 —— 尤其是**叫停只投一次唤醒、
 *    绝不在别的线程上调 `uv_stop`** 这一条。
 */

#include "capi/uvcpp_c_db.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_async.h>
#include <db/uvcpp_db_pool.h>
#include <db/uvcpp_db_status.h>
#include <db/uvcpp_db_table.h>
#include <db/uvcpp_db_value.h>
#include <handle/uvcpp_async.h>
#include <handle/uvcpp_loop.h>

#include "capi/uvcpp_c_internal.h"

using uvcpp_c_detail::alive;
using uvcpp_c_detail::copy_out;
using uvcpp_c_detail::field_present;
using uvcpp_c_detail::register_head;
using uvcpp_c_detail::table_size_ok;
using uvcpp_c_detail::unregister_head;

// ---------------------------------------------------------------------------
// 魔数：新的 `ed*` 前缀（`e` = 这一层，`d` = db 那一支），与 eh*/ew*/eq*/eh2*/
// eh3* 那几族不撞。末位仍然按"那个类型叫什么"取。
// ---------------------------------------------------------------------------

#define UVCPP_C_MAGIC_DB_CLIENT UVCPP_C_MAGIC('e', 'd', 'c', '1')
#define UVCPP_C_MAGIC_DB_PARAMS UVCPP_C_MAGIC('e', 'd', 'm', '1')
#define UVCPP_C_MAGIC_DB_TABLE  UVCPP_C_MAGIC('e', 'd', 't', '1')
#define UVCPP_C_MAGIC_DB_POOL   UVCPP_C_MAGIC('e', 'd', 'p', '1')
#define UVCPP_C_MAGIC_DB_ASYNC  UVCPP_C_MAGIC('e', 'd', 'a', '1')

namespace uv = uvcpp;

// ---------------------------------------------------------------------------
// 句柄的真身
// ---------------------------------------------------------------------------

/** @brief 只读的值视图。**借来的**：没有 free，所有者一没它就失效。 */
struct uvcpp_c_db_value;  // 头里已经前置声明；这里**刻意不给定义**，见文件头第 1 条

struct uvcpp_c_db_params {
  uvcpp_c_detail::handle_head head;  ///< 必须在偏移 0

  uv::uvcpp_db_params p;
};

struct uvcpp_c_db_table {
  uvcpp_c_detail::handle_head head;

  /// 一份**拷贝**（不是视图）：C 面承诺"回调返回后它仍然有效"。
  uv::uvcpp_db_table t;
};

struct uvcpp_c_db_client {
  uvcpp_c_detail::handle_head head;

  /// 底下的 C++ 连接。`owned` 为假时它属于池子，这里只是借来指向它。
  uv::uvcpp_db_client* c;
  bool                 owned;
  uv::uvcpp_db_value   last_id;       ///< `_last_insert_id()` 视图的落点
};

struct uvcpp_c_db_pool {
  uvcpp_c_detail::handle_head head;

  uv::uvcpp_db_pool* p;
  /// 建池成功过（`_pool_close()` 与失败的 `init()` 都会把它清掉）。用它把
  /// "池子没 init" 与 "借不出连接" 分开报 —— C++ 那侧把这件事记在 impl 私有的
  /// `acquire_status_` 上，公开面看不到。
  bool inited;

  /// 发出去的借出句柄：C 句柄 → 底下的 C++ 连接。借出的那枚 C 句柄由池子反
  /// 登记并删除（见文件头第 4 条）。
  std::mutex                                          mu;
  std::unordered_map<uvcpp_c_db_client*, uv::uvcpp_db_client*> out;
};

/**
 * @brief 异步门面的**共享状态**：闭包只碰它，**不碰 C 句柄**。
 *
 * 为什么非要多这一层：用户可以在回调里 `delete` 门面（库里最自然的用法），
 * 于是"回调跑起来时 C 句柄还活着"不能靠句柄自己保证。闭包持一份 `shared_ptr`
 * 就没这个问题 —— 句柄没了，状态还在，闭包照样安全地把结果丢掉。
 */
struct db_c_async_state {
  std::mutex              mu;
  std::condition_variable cv;

  /// `_async_free()` 已经开始。置上之后**新的**用户回调不再发生（已排队的活
  /// 把结果丢掉）。置它与 `in_flight` 的加减在同一把锁下，所以"检查 + 计数"
  /// 是一个原子步：free 之后投不进来新的活。
  bool   closed = false;
  size_t in_flight = 0;

  /// 正在跑的用户回调层数（只有循环线程会写它）。
  std::atomic<int> cb_depth{0};

  /// 循环线程是谁 —— 只有"从回调里 free 自己"这一种情形要靠它判（别的线程
  /// 进来时等着就是了，不该报错）。
  bool        loop_started = false;
  uv_thread_t loop_thread = uv_thread_t();
};

struct uvcpp_c_db_async {
  uvcpp_c_detail::handle_head head;

  // ---- 绑谁（二选一，绑上不换）----

  uv::uvcpp_db_client* client = nullptr;  ///< 借来的；`facade` 也绑的是它
  uv::uvcpp_db_pool*   pool = nullptr;    ///< 借来的（池子模式）
  const void*          owner = nullptr;   ///< 借来的那个 C 句柄（反登记用）

  /// 只有 client 模式有：门面替我们管在途账目与投递。
  std::unique_ptr<uv::uvcpp_db_async> facade;

  std::shared_ptr<db_c_async_state> state;

  // ---- 自带的那条循环线程（懒起）----

  std::atomic<uv::uvcpp_loop*>  loop_{nullptr};
  std::atomic<uv::uvcpp_async*> wake_{nullptr};
  std::atomic<bool>             stopping_{false};
  std::mutex                    start_mu;
  std::thread                   thread;
  bool                          started = false;
  int                           start_rc = 0;
};

static_assert(offsetof(uvcpp_c_db_params, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_db_table, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_db_client, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_db_pool, head) == 0, "handle_head 必须在偏移 0");
static_assert(offsetof(uvcpp_c_db_async, head) == 0, "handle_head 必须在偏移 0");

// ---------------------------------------------------------------------------
// 那两张枚举的对齐，钉成编译期的话
// ---------------------------------------------------------------------------
//
// 头里承诺"数值逐条对齐"，而这条承诺的全部价值就在于 C# 侧一张表通吃。散文写
// 在那句话旁边是拦不住漂的 —— 这里逐条钉。db 那边加一格而这边忘了加，是**编译
// 不过**，不是"运行到那一格才发现名字不对"。

#define UVCPP_C_DB_SAME_ENUM(c_name, cpp_name)                                \
  static_assert(static_cast<int>(uv::uvcpp_db_status::cpp_name) == (c_name),   \
                "uvcpp_c_db_status 与 uvcpp_db_status 漂了：" #c_name)

UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_OK, OK);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_BAD_URL, BAD_URL);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_NO_DRIVER, NO_DRIVER);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_OPEN_FAILED, OPEN_FAILED);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_NOT_CONNECTED, NOT_CONNECTED);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_PREPARE_FAILED, PREPARE_FAILED);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_EXEC_FAILED, EXEC_FAILED);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_BIND_FAILED, BIND_FAILED);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_UNSUPPORTED, UNSUPPORTED);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_MISUSE, MISUSE);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_OUT_OF_MEMORY, OUT_OF_MEMORY);
UVCPP_C_DB_SAME_ENUM(UVCPP_C_DB_NO_CONNECTION, NO_CONNECTION);

#define UVCPP_C_DB_SAME_TYPE(c_name, cpp_name)                                \
  static_assert(static_cast<int>(uv::uvcpp_db_type::cpp_name) == (c_name),     \
                "uvcpp_c_db_value_type 与 uvcpp_db_type 漂了：" #c_name)

UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_NIL, NIL);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_INT64, INT64);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_UINT64, UINT64);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_DOUBLE, DOUBLE);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_BOOL, BOOL);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_TEXT, TEXT);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_BLOB, BLOB);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_DATE, DATE);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_TIME, TIME);
UVCPP_C_DB_SAME_TYPE(UVCPP_C_DB_DATETIME, DATETIME);

#undef UVCPP_C_DB_SAME_ENUM
#undef UVCPP_C_DB_SAME_TYPE

namespace {

// ---------------------------------------------------------------------------
// 值视图：`uvcpp_db_value*` ⇄ `uvcpp_c_db_value*`
// ---------------------------------------------------------------------------
//
// 两个方向都只在这一个地方写。`reinterpret_cast` 的往返是标准保证的（转回去
// 一定得到原指针），而这里**永远只会转回去** —— `uvcpp_c_db_value` 没有定义，
// 所以没有任何代码能把它当结构体读。

inline const uv::uvcpp_db_value* unwrap(const uvcpp_c_db_value* v) {
  return reinterpret_cast<const uv::uvcpp_db_value*>(v);
}

inline const uvcpp_c_db_value* wrap(const uv::uvcpp_db_value* v) {
  return reinterpret_cast<const uvcpp_c_db_value*>(v);
}

/// @brief 越界/句柄无效时交出去的那枚静态 NULL 值视图（**不是** NULL 指针）。
const uvcpp_c_db_value* null_view() {
  static const uv::uvcpp_db_value kNull;
  return wrap(&kNull);
}

// ---------------------------------------------------------------------------
// 交付：把一份 C++ 结果搬进一枚**调用方拥有**的表句柄
// ---------------------------------------------------------------------------

uvcpp_c_db_table* wrap_table(uv::uvcpp_db_table&& t) {
  uvcpp_c_db_table* h = new (std::nothrow) uvcpp_c_db_table();
  if (h == nullptr) return nullptr;
  h->t = std::move(t);
  register_head(h, UVCPP_C_MAGIC_DB_TABLE);
  return h;
}

// ---------------------------------------------------------------------------
// "谁绑着异步门面"的登记本
// ---------------------------------------------------------------------------
//
// `uvcpp_c_db_async` **借**一条连接（或一个池子），而 C 面没有 RAII：用户完全
// 可能在异步活还在飞的时候把 client 句柄 free 掉，那一刻工作线程就会去读一块
// 已经还回去的内存。C++ 那侧把这件事写成调用方的义务（`uvcpp_db_async` 的类
// 注释就是这么写的），但 C 面能把义务变成判据 —— 这正是本层句柄登记表存在的
// 同一条理由（"用过的句柄永远给 E_STALE，而不是崩溃"）。
//
// 于是：`_async_new*()` 在这里挂一笔，`_async_free()` 摘一笔；`_client_free()`
// / `_pool_release()` / `_pool_free()` 先问一句"还有门面绑着它吗"，有就
// `E_STATE`。代价是"漏 free 一枚门面"会连带让那个 client 也 free 不掉 ——
// 而那是一次**看得见的**失败（返回码），不是一次内存踩踏。
std::mutex g_bind_mu;
std::unordered_map<const void*, int> g_bind_counts;

void bind_owner(const void* owner) {
  std::lock_guard<std::mutex> lk(g_bind_mu);
  ++g_bind_counts[owner];
}

void unbind_owner(const void* owner) {
  std::lock_guard<std::mutex> lk(g_bind_mu);
  auto it = g_bind_counts.find(owner);
  if (it == g_bind_counts.end()) return;
  if (--it->second <= 0) g_bind_counts.erase(it);
}

bool owner_has_async(const void* owner) {
  std::lock_guard<std::mutex> lk(g_bind_mu);
  auto it = g_bind_counts.find(owner);
  return it != g_bind_counts.end() && it->second > 0;
}

// ---------------------------------------------------------------------------
// 参数解包
// ---------------------------------------------------------------------------

/// @brief `params` 为 NULL 表示"没有参数"（C++ 那侧有两个重载，这里替调用方选）。
const uv::uvcpp_db_params& params_or_empty(const uvcpp_c_db_params* params) {
  static const uv::uvcpp_db_params kEmpty;
  return params != nullptr ? params->p : kEmpty;
}

/// @brief SQL 入参检查。返回 0 表示可以往下走，否则是当场该交回去的错误码。
///
/// 两格的来源不同：`sql == NULL && n > 0` 是**参数不合法**（C 层的 `E_INVALID_ARG`），
/// 而 `n == 0` 是"这条语句是空的"——那是**方言无关的用法错**，与 C++ 那侧
/// `uvcpp_db_status::MISUSE` 是同一件事，所以走 db 那一段编码。
///
/// 写成函数而不是就地一个三元式，是因为两个枚举类型不同：`cond ? E_INVALID_ARG :
/// UVCPP_C_DB_MISUSE` 会被 `-Wenum-compare` 抓（GCC 13 实测），而这条规则在本仓是
/// 编得出来的告警，不是风格偏好。
int check_sql(const char* sql, size_t n) {
  if (sql == nullptr) return UVCPP_C_E_INVALID_ARG;
  if (n == 0) return static_cast<int>(UVCPP_C_DB_MISUSE);
  return 0;
}

}  // namespace

// ===========================================================================
// 状态码与驱动
// ===========================================================================

extern "C" UVCPP_C_API const char* uvcpp_c_db_status_name(int status) {
  UVCPP_C_TRY
    return uv::uvcpp_db_status_name(static_cast<uv::uvcpp_db_status>(status));
  UVCPP_C_CATCH("unknown")
}

extern "C" UVCPP_C_API int uvcpp_c_db_drivers(char* buf, size_t cap) {
  UVCPP_C_TRY
    return copy_out(uv::uvcpp_db_drivers(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 值
// ===========================================================================
//
// 这一组是本片里**唯一不查句柄登记表**的：视图本来就是借来的（没有 free、
// 没有魔数），能做的只有"空指针 → 错误码"。用它之前先确认所有者还在 ——
// 头里那三类所有权里的第 4 类写的就是这条。

extern "C" UVCPP_C_API int uvcpp_c_db_value_type(const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    if (v == nullptr) return UVCPP_C_E_INVALID_ARG;
    return static_cast<int>(v->type());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API const char* uvcpp_c_db_value_type_name(
    const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    return v != nullptr ? v->type_name() : "unknown";
  UVCPP_C_CATCH("unknown")
}

extern "C" UVCPP_C_API int uvcpp_c_db_value_is_null(const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    if (v == nullptr) return UVCPP_C_E_INVALID_ARG;
    return v->is_null() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_value_to_text(const uvcpp_c_db_value* value,
                                                    char* buf, size_t cap) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    if (v == nullptr) return UVCPP_C_E_INVALID_ARG;
    return copy_out(v->to_text(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int64_t uvcpp_c_db_value_to_int64(
    const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    return v != nullptr ? v->to_int64() : 0;
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API uint64_t uvcpp_c_db_value_to_uint64(
    const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    return v != nullptr ? v->to_uint64() : 0;
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API double uvcpp_c_db_value_to_double(
    const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    return v != nullptr ? v->to_double() : 0.0;
  UVCPP_C_CATCH(0.0)
}

extern "C" UVCPP_C_API int uvcpp_c_db_value_to_bool(const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    return (v != nullptr && v->to_bool()) ? 1 : 0;
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_db_value_size(const uvcpp_c_db_value* value) {
  UVCPP_C_TRY
    const uv::uvcpp_db_value* v = unwrap(value);
    if (v == nullptr) return UVCPP_C_E_INVALID_ARG;
    return static_cast<int>(v->size());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_value_bytes(const uvcpp_c_db_value* value,
                                                  const char** data, size_t* len) {
  UVCPP_C_TRY
    if (data == nullptr || len == nullptr) return UVCPP_C_E_INVALID_ARG;
    *data = nullptr;
    *len = 0;
    const uv::uvcpp_db_value* v = unwrap(value);
    if (v == nullptr) return UVCPP_C_E_INVALID_ARG;
    // 数值型没有"原始字节"（`bytes()` 是空串），要文本用 `_to_text()`。
    const std::string& b = v->bytes();
    *data = b.data();
    *len = b.size();
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 参数
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_db_params* uvcpp_c_db_params_new(void) {
  UVCPP_C_TRY
    uvcpp_c_db_params* h = new (std::nothrow) uvcpp_c_db_params();
    if (h == nullptr) return nullptr;
    register_head(h, UVCPP_C_MAGIC_DB_PARAMS);
    return h;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_free(uvcpp_c_db_params* params) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    unregister_head(params);
    delete params;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_add_null(uvcpp_c_db_params* params) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    params->p.add_null();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_add_int64(uvcpp_c_db_params* params,
                                                       int64_t value) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    params->p.add(uv::uvcpp_db_value(value));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_add_uint64(uvcpp_c_db_params* params,
                                                        uint64_t value) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    params->p.add(uv::uvcpp_db_value(value));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_add_double(uvcpp_c_db_params* params,
                                                        double value) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    params->p.add(uv::uvcpp_db_value(value));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_add_bool(uvcpp_c_db_params* params,
                                                      int value) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    params->p.add(uv::uvcpp_db_value(value != 0));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_add_text(uvcpp_c_db_params* params,
                                                      const char* text, size_t n) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    // `text == NULL && n == 0` 是**空串**（不是 NULL）：`std::string(nullptr, 0)`
    // 是未定义行为，所以这一格必须分开走。
    if (text == nullptr || n == 0) {
      params->p.add(uv::uvcpp_db_value(std::string()));
    } else {
      params->p.add(uv::uvcpp_db_value(std::string(text, n)));
    }
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_add_blob(uvcpp_c_db_params* params,
                                                      const void* data, size_t n) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    params->p.add(uv::uvcpp_db_value::blob(data, n));
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_clear(uvcpp_c_db_params* params) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    params->p.clear();
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_params_count(
    const uvcpp_c_db_params* params) {
  UVCPP_C_TRY
    if (!alive(params, UVCPP_C_MAGIC_DB_PARAMS)) return UVCPP_C_E_STALE;
    return static_cast<int>(params->p.size());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 结果集
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_db_table_free(uvcpp_c_db_table* table) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    unregister_head(table);
    delete table;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_table_row_count(
    const uvcpp_c_db_table* table) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    return static_cast<int>(table->t.row_count());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_table_column_count(
    const uvcpp_c_db_table* table) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    return static_cast<int>(table->t.column_count());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_table_column_name(
    const uvcpp_c_db_table* table, size_t index, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    if (index >= table->t.columns().size()) return UVCPP_C_E_INVALID_ARG;
    return copy_out(table->t.columns()[index], buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_table_column_index(
    const uvcpp_c_db_table* table, const char* name) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    const size_t idx = table->t.column_index(name != nullptr ? name : "");
    // "没有这一列"与"第 0 列"必须分得开 —— 0 是个合法下标，拿它当"没有"就是
    // 撒谎（`uvcpp_c_common.h` 里 `E_NOT_FOUND` 那一格讲的是同一件事）。
    if (idx == std::string::npos) return UVCPP_C_E_NOT_FOUND;
    return static_cast<int>(idx);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API const uvcpp_c_db_value* uvcpp_c_db_table_cell(
    const uvcpp_c_db_table* table, size_t row, size_t column) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return null_view();
    // `at()` 越界时返回一枚**静态 NULL 值**（不是悬空引用），于是"这一格没有"
    // 在 C 侧就是 `_is_null()` 为真，不需要第二条分支。
    return wrap(&table->t.at(row, column));
  UVCPP_C_CATCH(null_view())
}

extern "C" UVCPP_C_API int64_t uvcpp_c_db_table_affected_rows(
    const uvcpp_c_db_table* table) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return -1;
    return table->t.affected_rows();
  UVCPP_C_CATCH(-1)
}

extern "C" UVCPP_C_API int uvcpp_c_db_table_name(const uvcpp_c_db_table* table,
                                                 char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    return copy_out(table->t.table_name(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_table_to_json(const uvcpp_c_db_table* table,
                                                    char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    return copy_out(table->t.to_json(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_table_to_csv(const uvcpp_c_db_table* table,
                                                   char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(table, UVCPP_C_MAGIC_DB_TABLE)) return UVCPP_C_E_STALE;
    return copy_out(table->t.to_csv(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 连接
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_db_client* uvcpp_c_db_client_new(void) {
  UVCPP_C_TRY
    uvcpp_c_db_client* h = new (std::nothrow) uvcpp_c_db_client();
    if (h == nullptr) return nullptr;
    h->c = new (std::nothrow) uv::uvcpp_db_client();
    if (h->c == nullptr) {
      delete h;
      return nullptr;
    }
    h->owned = true;
    register_head(h, UVCPP_C_MAGIC_DB_CLIENT);
    return h;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_free(uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT)) return UVCPP_C_E_STALE;
    if (client->c == nullptr) return UVCPP_C_E_STALE;
    // 池子借出的那枚：它属于池子，要用 `_pool_release()` 还。拿它当自己建的
    // 释放就是把池子的额度连同账目一起丢掉。
    if (!client->owned) return UVCPP_C_E_STATE;
    // 还有异步门面绑着它 —— 那门面的工作线程随时可能去读这块内存。
    if (owner_has_async(client)) return UVCPP_C_E_STATE;

    unregister_head(client);
    uv::uvcpp_db_client* c = client->c;
    client->c = nullptr;
    delete c;
    delete client;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_open(uvcpp_c_db_client* client,
                                                  const char* url) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    if (url == nullptr) return UVCPP_C_E_INVALID_ARG;
    return static_cast<int>(client->c->open(std::string(url)));
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_close(uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    client->c->close();
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_is_open(
    const uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return client->c->is_open() ? 1 : 0;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_reconnect(uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(client->c->reconnect());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_ping(uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(client->c->ping());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_driver_name(
    const uvcpp_c_db_client* client, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    const char* name = client->c->driver_name();
    return copy_out(name != nullptr ? std::string(name) : std::string(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_url(const uvcpp_c_db_client* client,
                                                 char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return copy_out(client->c->url(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_last_error(
    const uvcpp_c_db_client* client, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return copy_out(client->c->last_error(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_set_timeout_ms(
    uvcpp_c_db_client* client, int timeout_ms) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    client->c->set_timeout_ms(timeout_ms);
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 同步读写
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_db_client_query(uvcpp_c_db_client* client,
                                                   const char* sql, size_t n,
                                                   const uvcpp_c_db_params* params,
                                                   uvcpp_c_db_table** out) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    if (out == nullptr) return UVCPP_C_E_INVALID_ARG;
    // `n == 0` 是空 SQL（头里写死的 `MISUSE`）；`sql == NULL && n > 0` 是参数
    // 不合法。两者都不是数据库的返回码，但第一格是**方言无关的**"这条语句是
    // 空的"，所以走 db 状态那一段（与 C++ 那侧一致）。
    const int sql_rc = check_sql(sql, n);
    if (sql_rc != 0) return sql_rc;

    // 句柄**先**建：于是"失败时 `out` 也是一张空表"这条承诺在所有非内存不足
    // 的情形下都成立（C 面没有 RAII，"拿到了就必须 free" 越少例外越好）。
    uvcpp_c_db_table* h = wrap_table(uv::uvcpp_db_table());
    if (h == nullptr) return UVCPP_C_E_NO_MEMORY;

    const uv::uvcpp_db_status st =
        params != nullptr
            ? client->c->query(std::string(sql, n), params->p, &h->t)
            : client->c->query(std::string(sql, n), &h->t);
    *out = h;
    return static_cast<int>(st);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_execute(
    uvcpp_c_db_client* client, const char* sql, size_t n,
    const uvcpp_c_db_params* params, int64_t* affected) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    const int sql_rc = check_sql(sql, n);
    if (sql_rc != 0) return sql_rc;
    int64_t n_affected = -1;
    const uv::uvcpp_db_status st =
        params != nullptr
            ? client->c->execute(std::string(sql, n), params->p, &n_affected)
            : client->c->execute(std::string(sql, n), &n_affected);
    if (affected != nullptr) *affected = n_affected;
    return static_cast<int>(st);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_insert(
    uvcpp_c_db_client* client, const char* sql, size_t n,
    const uvcpp_c_db_params* params, int64_t* out_id, int* out_has_id) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    const int sql_rc = check_sql(sql, n);
    if (sql_rc != 0) return sql_rc;
    // 顺带落进句柄里：`_last_insert_id()` 的视图指的就是这一格。
    const uv::uvcpp_db_status st =
        params != nullptr
            ? client->c->insert(std::string(sql, n), params->p, &client->last_id)
            : client->c->insert(std::string(sql, n), uv::uvcpp_db_params(),
                                &client->last_id);
    // "插进去了"与"拿不到 id"是两件事：报不出自增时返回码仍是 OK，这一格给 0。
    const bool has_id = !client->last_id.is_null();
    if (out_has_id != nullptr) *out_has_id = has_id ? 1 : 0;
    if (out_id != nullptr) *out_id = has_id ? client->last_id.to_int64() : 0;
    return static_cast<int>(st);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_last_insert_id(
    uvcpp_c_db_client* client, const uvcpp_c_db_value** out) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    if (out == nullptr) return UVCPP_C_E_INVALID_ARG;
    const uv::uvcpp_db_status st = client->c->last_insert_id(&client->last_id);
    if (st != uv::uvcpp_db_status::OK) {
      *out = nullptr;
      return static_cast<int>(st);
    }
    *out = wrap(&client->last_id);
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 事务
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_db_client_begin(uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(client->c->begin());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_commit(uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(client->c->commit());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_rollback(uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(client->c->rollback());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 元信息与转义
// ===========================================================================

extern "C" UVCPP_C_API int uvcpp_c_db_client_table_names(uvcpp_c_db_client* client,
                                                         char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    std::vector<std::string> names;
    const uv::uvcpp_db_status st = client->c->table_names(&names);
    if (st != uv::uvcpp_db_status::OK) return static_cast<int>(st);
    // `'\n'` 连接：表名里不可能有换行，于是这个编码不需要任何额外的长度/生命
    // 周期约定（头里写死了这一条）。
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i != 0) joined.push_back('\n');
      joined += names[i];
    }
    return copy_out(joined, buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_table_schema(
    uvcpp_c_db_client* client, const char* table, uvcpp_c_db_table** out) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    if (table == nullptr || out == nullptr) return UVCPP_C_E_INVALID_ARG;

    uvcpp_c_db_table* h = wrap_table(uv::uvcpp_db_table());
    if (h == nullptr) return UVCPP_C_E_NO_MEMORY;
    const uv::uvcpp_db_status st =
        client->c->table_schema(std::string(table), &h->t);
    *out = h;
    return static_cast<int>(st);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_escape(uvcpp_c_db_client* client,
                                                    const char* text, size_t n,
                                                    char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    const std::string in = (text == nullptr || n == 0) ? std::string()
                                                       : std::string(text, n);
    return copy_out(client->c->escape(in), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_client_escape_identifier(
    uvcpp_c_db_client* client, const char* text, size_t n, char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return UVCPP_C_E_STALE;
    const std::string in = (text == nullptr || n == 0) ? std::string()
                                                       : std::string(text, n);
    return copy_out(client->c->escape_identifier(in), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 连接池
// ===========================================================================

extern "C" UVCPP_C_API uvcpp_c_db_pool* uvcpp_c_db_pool_new(void) {
  UVCPP_C_TRY
    uvcpp_c_db_pool* h = new (std::nothrow) uvcpp_c_db_pool();
    if (h == nullptr) return nullptr;
    h->p = new (std::nothrow) uv::uvcpp_db_pool();
    if (h->p == nullptr) {
      delete h;
      return nullptr;
    }
    h->inited = false;
    register_head(h, UVCPP_C_MAGIC_DB_POOL);
    return h;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_free(uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    // 还有异步门面绑着它：那门面在池线程里借还连接，池子一没它就在读已释放的
    // 内存（这是本文件里唯一一处**必须**报错而不是"尽力而为"的次序约束）。
    if (owner_has_async(pool)) return UVCPP_C_E_STATE;
    // 在外的借用：连同它们的 C 句柄一起收掉。**句柄先反登记再删**，于是用户手上
    // 那枚旧指针之后一律是 `E_STALE`，不是野指针。
    std::unordered_map<uvcpp_c_db_client*, uv::uvcpp_db_client*> drop;
    {
      std::lock_guard<std::mutex> lk(pool->mu);
      drop.swap(pool->out);
    }
    for (auto& kv : drop) {
      unregister_head(kv.first);
      delete kv.first;
    }
    unregister_head(pool);
    uv::uvcpp_db_pool* p = pool->p;
    pool->p = nullptr;
    delete p;
    delete pool;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_init(uvcpp_c_db_pool* pool,
                                                const char* url, size_t min,
                                                size_t max) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    if (url == nullptr) return UVCPP_C_E_INVALID_ARG;
    const uv::uvcpp_db_status st =
        pool->p->init(std::string(url), min, max);
    // 只有真的成了才算"已 init"：失败的 init 之后池子是空的、还能再 init 一次。
    pool->inited = (st == uv::uvcpp_db_status::OK);
    return static_cast<int>(st);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_close(uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    pool->p->close();
    pool->inited = false;
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_acquire(uvcpp_c_db_pool* pool,
                                                   int timeout_ms,
                                                   uvcpp_c_db_client** out) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    if (out == nullptr) return UVCPP_C_E_INVALID_ARG;
    *out = nullptr;

    // 照调一次：借不出时 C++ 那侧会把原因写进池子的 `last_error()`，而那是
    // 调用方唯一能看到"到底是满了还是连不上"的地方。
    uv::uvcpp_db_client* c = pool->p->acquire(timeout_ms);
    if (c == nullptr) {
      // "没 init" 与 "借不出" 是两件不同的事（前者是用法错，后者是该扩容），
      // 而 C++ 那侧把这个区分记在私有的 `acquire_status_` 上 —— 这里用自己那份
      // `inited` 复现它。
      return pool->inited ? UVCPP_C_DB_NO_CONNECTION : UVCPP_C_DB_MISUSE;
    }

    uvcpp_c_db_client* h = new (std::nothrow) uvcpp_c_db_client();
    if (h == nullptr) {
      // 借到手了但记不下来：还回去，别把这条连接漏在外头。
      pool->p->release(c);
      return UVCPP_C_E_NO_MEMORY;
    }
    h->c = c;
    h->owned = false;  // 借来的：`_client_free()` 对它返回 E_STATE
    register_head(h, UVCPP_C_MAGIC_DB_CLIENT);
    {
      std::lock_guard<std::mutex> lk(pool->mu);
      pool->out.emplace(h, c);
    }
    *out = h;
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_release(uvcpp_c_db_pool* pool,
                                                   uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    if (client == nullptr) return UVCPP_C_E_INVALID_ARG;
    // 已释放过的句柄在这里就分得出来（登记表不问那块内存，见 `alive()`）。
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT)) return UVCPP_C_E_STALE;
    if (owner_has_async(client)) return UVCPP_C_E_STATE;

    uv::uvcpp_db_client* c = nullptr;
    {
      std::lock_guard<std::mutex> lk(pool->mu);
      auto it = pool->out.find(client);
      if (it == pool->out.end()) return UVCPP_C_E_INVALID_ARG;  // 不是本池借出的
      c = it->second;
      pool->out.erase(it);
    }
    unregister_head(client);
    delete client;
    pool->p->release(c);
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_discard(uvcpp_c_db_pool* pool,
                                                   uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    if (client == nullptr) return UVCPP_C_E_INVALID_ARG;
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT)) return UVCPP_C_E_STALE;
    if (owner_has_async(client)) return UVCPP_C_E_STATE;

    uv::uvcpp_db_client* c = nullptr;
    {
      std::lock_guard<std::mutex> lk(pool->mu);
      auto it = pool->out.find(client);
      if (it == pool->out.end()) return UVCPP_C_E_INVALID_ARG;
      c = it->second;
      pool->out.erase(it);
    }
    unregister_head(client);
    delete client;
    pool->p->discard(c);
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_query(uvcpp_c_db_pool* pool,
                                                 const char* sql, size_t n,
                                                 const uvcpp_c_db_params* params,
                                                 uvcpp_c_db_table** out) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    if (out == nullptr) return UVCPP_C_E_INVALID_ARG;
    const int sql_rc = check_sql(sql, n);
    if (sql_rc != 0) return sql_rc;
    // 与 `_client_query()` 同一条：句柄先建，于是交付的形状在对错两种情形下
    // 完全一样（拿到了就必须 free，没有例外）。
    uvcpp_c_db_table* h = wrap_table(uv::uvcpp_db_table());
    if (h == nullptr) return UVCPP_C_E_NO_MEMORY;
    const uv::uvcpp_db_status st =
        params != nullptr
            ? pool->p->query(std::string(sql, n), params->p, &h->t)
            : pool->p->query(std::string(sql, n), &h->t);
    *out = h;
    return static_cast<int>(st);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_execute(
    uvcpp_c_db_pool* pool, const char* sql, size_t n,
    const uvcpp_c_db_params* params, int64_t* affected) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    const int sql_rc = check_sql(sql, n);
    if (sql_rc != 0) return sql_rc;
    int64_t n_affected = -1;
    const uv::uvcpp_db_status st =
        params != nullptr
            ? pool->p->execute(std::string(sql, n), params->p, &n_affected)
            : pool->p->execute(std::string(sql, n), uv::uvcpp_db_params(),
                               &n_affected);
    if (affected != nullptr) *affected = n_affected;
    return static_cast<int>(st);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_ping(uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(pool->p->ping());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_size(const uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(pool->p->size());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_in_use(const uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(pool->p->in_use());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_idle(const uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(pool->p->idle());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_max_size(const uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(pool->p->max_size());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_min_size(const uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(pool->p->min_size());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_created_total(
    const uvcpp_c_db_pool* pool, uint64_t* out) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    if (out == nullptr) return UVCPP_C_E_INVALID_ARG;
    *out = pool->p->created_total();
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_reused_total(
    const uvcpp_c_db_pool* pool, uint64_t* out) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    if (out == nullptr) return UVCPP_C_E_INVALID_ARG;
    *out = pool->p->reused_total();
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_close_idle(uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    return static_cast<int>(pool->p->close_idle());
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_set_acquire_timeout_ms(
    uvcpp_c_db_pool* pool, int ms) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    pool->p->set_acquire_timeout_ms(ms);
    return UVCPP_C_DB_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_pool_last_error(const uvcpp_c_db_pool* pool,
                                                      char* buf, size_t cap) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr)
      return UVCPP_C_E_STALE;
    // C++ 那侧返回的是**拷贝**（池子是多线程的），这里正好接得住。
    return copy_out(pool->p->last_error(), buf, cap);
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

// ===========================================================================
// 异步
// ===========================================================================
//
// 形状：投递 → 池线程里跑阻塞查询 → 完成回调在**门面自带的循环线程**上交付。
// 头里解释了为什么这里没有循环参数（C 面没有一个合法的 `uvcpp_loop*` 可填）。
//
// 三件容易错的事由这一段守住：
//   1. 回调一定在循环线程上一次；
//   2. 投递失败时回调不会被调（返回值就是全部）；
//   3. `free()` 之后不再有**新的**用户回调，而在途的活会被等完。

namespace {

/// @brief 进入/退出用户回调时的计数（RAII：回调里提前 return 也算数）。
class CbDepth {
 public:
  explicit CbDepth(const std::shared_ptr<db_c_async_state>& s) : s_(s) {
    s_->cb_depth.fetch_add(1);
  }
  ~CbDepth() { s_->cb_depth.fetch_sub(1); }
  CbDepth(const CbDepth&) = delete;
  CbDepth& operator=(const CbDepth&) = delete;

 private:
  std::shared_ptr<db_c_async_state> s_;
};

/// @brief 一笔在途活的收尾：`in_flight` 减一 + 叫醒在等的 `_async_free()`。
void finish_one(const std::shared_ptr<db_c_async_state>& s) {
  {
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->in_flight > 0) --s->in_flight;
  }
  s->cv.notify_all();
}

/// @brief 这次投递还该不该交付给用户？（`free()` 之后的活把结果丢掉）
bool still_open(const std::shared_ptr<db_c_async_state>& s) {
  std::lock_guard<std::mutex> lk(s->mu);
  return !s->closed;
}

/// @brief 查询的交付：把表**拷一份**成调用方拥有的句柄，再调用户回调。
void deliver_table(const std::shared_ptr<db_c_async_state>& s, int status,
                   const uv::uvcpp_db_table& t, const uvcpp_c_db_query_events& ev,
                   void* user_data) {
  if (still_open(s)) {
    CbDepth depth(s);
    uvcpp_c_db_table* out = nullptr;
    // 失败时 `t` 本来就是空表（C++ 侧的约定），而头里说 `t` 为 NULL 表示"这次
    // 不是查询"或"交付不出来" —— 换句说：没表可交，就别造一个空的糊弄人。
    if (status == UVCPP_C_DB_OK) out = wrap_table(uv::uvcpp_db_table(t));
    try {
      ev.on_table(user_data, status, out);
    } catch (...) {
      // C 函数指针不该抛，但这一层是 C++ 编译的 —— 让异常跑进 libuv 的
      // after_work 是未定义行为，这里就地吃掉。
    }
  }
  finish_one(s);
}

/// @brief 增删改的交付。
void deliver_executed(const std::shared_ptr<db_c_async_state>& s, int status,
                      int64_t affected, const uvcpp_c_db_query_events& ev,
                      void* user_data) {
  if (still_open(s)) {
    CbDepth depth(s);
    try {
      ev.on_executed(user_data, status, affected);
    } catch (...) {
    }
  }
  finish_one(s);
}

/// @brief 回调表的校验。返回负数是该当场拒掉的错误码。
int check_events(const uvcpp_c_db_query_events* ev, bool want_table) {
  if (ev == nullptr) return UVCPP_C_E_INVALID_ARG;
  if (!table_size_ok(ev->size)) return UVCPP_C_E_INVALID_ARG;
  if (want_table) {
    if (!field_present(ev->size, &uvcpp_c_db_query_events::on_table) ||
        ev->on_table == nullptr) {
      // 让它白跑不是"用法之一"，是漏了 —— 这一笔的交付通道没给。
      return UVCPP_C_E_INVALID_ARG;
    }
  } else {
    if (!field_present(ev->size, &uvcpp_c_db_query_events::on_executed) ||
        ev->on_executed == nullptr) {
      return UVCPP_C_E_INVALID_ARG;
    }
  }
  return UVCPP_C_OK;
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// 门面的循环线程。形状照 `db/uvcpp_db_async.cpp` 的 `impl::thread_main` /
// `stop_private_loop` —— 那里有这段代码为什么长这样的完整理由，这里只重复最
// 要紧的两条：
//
//   1. 那条 async **既撑住循环又叫停**。没有它，两次投递之间的空档里循环没有
//      活跃句柄，`UV_RUN_DEFAULT` 当场返回、线程就退了；叫停也只能靠往它上面
//      发一次唤醒（`uv_stop` 既非线程安全，也叫不醒正 parked 在 epoll 里的
//      循环）。
//   2. 收尾顺序不能换：先摘指针 → 删 async（**必须在 `uv_run` 返回之后**，
//      在它自己的回调里删就是析构正在执行的 std::function）→ 泵掉挂起的关闭
//      回调 → 才关得掉循环。
// ---------------------------------------------------------------------------

void db_c_loop_main(uvcpp_c_db_async* a, std::promise<int>* ready) {
  uv::uvcpp_loop* lp = nullptr;
  uv::uvcpp_async* wk = nullptr;
  try {
    lp = new uv::uvcpp_loop();
    wk = new uv::uvcpp_async();
  } catch (...) {
    delete wk;
    delete lp;
    ready->set_value(UV_ENOMEM);
    return;
  }

  const int rc = wk->init(
      [a](uv::uvcpp_async*) {
        if (a->stopping_.load()) {
          uv::uvcpp_loop* l = a->loop_.load();
          if (l != nullptr) l->stop();
        }
      },
      lp);
  if (rc != 0) {
    delete wk;
    lp->loop_close();
    delete lp;
    ready->set_value(rc);
    return;
  }

  a->wake_.store(wk);
  a->loop_.store(lp);
  {
    // 记下"循环线程是谁"。写在这里（`set_value` 之前）是为了让它对
    // `ensure_loop()` 的调用方**必然可见** —— 那对 promise/future 就是一条
    // release/acquire 边。
    std::lock_guard<std::mutex> lk(a->state->mu);
    a->state->loop_thread = uv_thread_self();
    a->state->loop_started = true;
  }
  ready->set_value(0);

  lp->run(UV_RUN_DEFAULT);

  a->wake_.store(nullptr);
  a->loop_.store(nullptr);
  delete wk;
  for (int i = 0; i < 256 && lp->loop_alive() != 0; ++i) {
    lp->run(UV_RUN_NOWAIT);
  }
  lp->loop_close();
  delete lp;
}

/// @brief 懒起那条循环线程。返回 0 或错误码。
int ensure_loop(uvcpp_c_db_async* a) {
  std::lock_guard<std::mutex> lk(a->start_mu);
  if (a->started) return a->start_rc;

  std::promise<int> ready;
  std::future<int> fut = ready.get_future();
  std::thread t(db_c_loop_main, a, &ready);
  const int rc = fut.get();
  a->thread = std::move(t);
  a->started = true;
  a->start_rc = rc;
  return rc;
}

/// @brief 叫停并 join。**只投一次唤醒**，绝不在别的线程上调 `uv_stop`。
void stop_loop(uvcpp_c_db_async* a) {
  std::thread t;
  {
    std::lock_guard<std::mutex> lk(a->start_mu);
    if (!a->thread.joinable()) return;
    t = std::move(a->thread);
  }
  a->stopping_.store(true);
  uv::uvcpp_async* w = a->wake_.load();
  if (w != nullptr) w->send();
  t.join();
}

}  // namespace

extern "C" UVCPP_C_API uvcpp_c_db_async* uvcpp_c_db_async_new(
    uvcpp_c_db_client* client) {
  UVCPP_C_TRY
    if (!alive(client, UVCPP_C_MAGIC_DB_CLIENT) || client->c == nullptr)
      return nullptr;
    uvcpp_c_db_async* a = new (std::nothrow) uvcpp_c_db_async();
    if (a == nullptr) return nullptr;
    try {
      a->facade.reset(new uv::uvcpp_db_async(client->c));
      a->state = std::make_shared<db_c_async_state>();
    } catch (...) {
      delete a;
      return nullptr;
    }
    a->client = client->c;
    a->owner = client;
    register_head(a, UVCPP_C_MAGIC_DB_ASYNC);
    // 挂上这一笔之后，client 在那枚门面 free 掉之前不会被放走（见登记本那段）。
    bind_owner(client);
    return a;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API uvcpp_c_db_async* uvcpp_c_db_async_new_pool(
    uvcpp_c_db_pool* pool) {
  UVCPP_C_TRY
    if (!alive(pool, UVCPP_C_MAGIC_DB_POOL) || pool->p == nullptr) return nullptr;
    uvcpp_c_db_async* a = new (std::nothrow) uvcpp_c_db_async();
    if (a == nullptr) return nullptr;
    try {
      a->state = std::make_shared<db_c_async_state>();
    } catch (...) {
      delete a;
      return nullptr;
    }
    a->pool = pool->p;
    a->owner = pool;
    register_head(a, UVCPP_C_MAGIC_DB_ASYNC);
    bind_owner(pool);
    return a;
  UVCPP_C_CATCH(nullptr)
}

extern "C" UVCPP_C_API int uvcpp_c_db_async_free(uvcpp_c_db_async* async) {
  UVCPP_C_TRY
    if (!alive(async, UVCPP_C_MAGIC_DB_ASYNC)) return UVCPP_C_E_STALE;
    const std::shared_ptr<db_c_async_state> s = async->state;

    // "从回调里 free 自己"：同一条线程、且正有一层回调在跑。这时**不能**往下走
    // —— 下面要等在途的活收完，而当前这一笔就是我们自己（等自己 = 死等）。
    // 别的线程进来时不该报错：它等一等就对了。
    {
      std::lock_guard<std::mutex> lk(s->mu);
      if (s->cb_depth.load() > 0 && s->loop_started) {
        uv_thread_t cur = uv_thread_self();
        if (uv_thread_equal(&s->loop_thread, &cur) != 0) {
          return UVCPP_C_E_STATE;
        }
      }
    }

    // 置上 closed（此后不会再投进来新的活），然后等在途的清零。等待是有界的：
    // 那些活得先跑完，而数据库调用自己有超时兜底。
    {
      std::unique_lock<std::mutex> lk(s->mu);
      s->closed = true;
      s->cv.wait(lk, [&] { return s->in_flight == 0; });
    }

    unbind_owner(async->owner);
    unregister_head(async);
    stop_loop(async);
    // 到这里在途的活已经清零，facade 手上没有挂着的 job 了。
    async->facade.reset();
    delete async;
    return UVCPP_C_OK;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API size_t uvcpp_c_db_async_in_flight(
    const uvcpp_c_db_async* async) {
  UVCPP_C_TRY
    if (!alive(async, UVCPP_C_MAGIC_DB_ASYNC)) return 0;
    std::lock_guard<std::mutex> lk(async->state->mu);
    return async->state->in_flight;
  UVCPP_C_CATCH(0)
}

extern "C" UVCPP_C_API int uvcpp_c_db_async_query(
    uvcpp_c_db_async* async, const char* sql, size_t n,
    const uvcpp_c_db_params* params, const uvcpp_c_db_query_events* ev,
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(async, UVCPP_C_MAGIC_DB_ASYNC)) return UVCPP_C_E_STALE;
    const int sql_rc = check_sql(sql, n);
    if (sql_rc != 0) return sql_rc;
    const int ev_rc = check_events(ev, /*want_table=*/true);
    if (ev_rc != UVCPP_C_OK) return ev_rc;

    const int loop_rc = ensure_loop(async);
    if (loop_rc != 0) return loop_rc;
    uv::uvcpp_loop* loop = async->loop_.load();
    if (loop == nullptr) return UVCPP_C_E_STATE;

    // "检查 closed + 计数"在同一把锁下 —— 否则 `free()` 可能刚好在两步之间
    // 观察到 0 就把循环拆了，而这一笔随后投了上去。
    const std::shared_ptr<db_c_async_state> s = async->state;
    {
      std::lock_guard<std::mutex> lk(s->mu);
      if (s->closed) return UVCPP_C_E_STALE;
      ++s->in_flight;
    }

    const std::string sql_s(sql, n);
    const uv::uvcpp_db_params& ps = params_or_empty(params);
    const uvcpp_c_db_query_events ev_copy = *ev;
    const int rc = async->facade
                       ? async->facade->query(
                             loop, sql_s, ps,
                             [s, ev_copy, user_data](uv::uvcpp_db_status st,
                                                     const uv::uvcpp_db_table& t) {
                               deliver_table(s, static_cast<int>(st), t, ev_copy,
                                             user_data);
                             })
                       : async->pool->query(
                             loop, sql_s, ps,
                             [s, ev_copy, user_data](uv::uvcpp_db_status st,
                                                     const uv::uvcpp_db_table& t) {
                               deliver_table(s, static_cast<int>(st), t, ev_copy,
                                             user_data);
                             });
    // 没投出去：libuv 两个回调都不会跑，账目在这里自己平掉。
    if (rc != 0) finish_one(s);
    return rc;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}

extern "C" UVCPP_C_API int uvcpp_c_db_async_execute(
    uvcpp_c_db_async* async, const char* sql, size_t n,
    const uvcpp_c_db_params* params, const uvcpp_c_db_query_events* ev,
    void* user_data) {
  UVCPP_C_TRY
    if (!alive(async, UVCPP_C_MAGIC_DB_ASYNC)) return UVCPP_C_E_STALE;
    const int sql_rc = check_sql(sql, n);
    if (sql_rc != 0) return sql_rc;
    const int ev_rc = check_events(ev, /*want_table=*/false);
    if (ev_rc != UVCPP_C_OK) return ev_rc;

    const int loop_rc = ensure_loop(async);
    if (loop_rc != 0) return loop_rc;
    uv::uvcpp_loop* loop = async->loop_.load();
    if (loop == nullptr) return UVCPP_C_E_STATE;

    const std::shared_ptr<db_c_async_state> s = async->state;
    {
      std::lock_guard<std::mutex> lk(s->mu);
      if (s->closed) return UVCPP_C_E_STALE;
      ++s->in_flight;
    }

    const std::string sql_s(sql, n);
    const uv::uvcpp_db_params& ps = params_or_empty(params);
    const uvcpp_c_db_query_events ev_copy = *ev;
    const int rc = async->facade
                       ? async->facade->execute(
                             loop, sql_s, ps,
                             [s, ev_copy, user_data](uv::uvcpp_db_status st,
                                                     int64_t affected) {
                               deliver_executed(s, static_cast<int>(st), affected,
                                                ev_copy, user_data);
                             })
                       : async->pool->execute(
                             loop, sql_s, ps,
                             [s, ev_copy, user_data](uv::uvcpp_db_status st,
                                                     int64_t affected) {
                               deliver_executed(s, static_cast<int>(st), affected,
                                                ev_copy, user_data);
                             });
    if (rc != 0) finish_one(s);
    return rc;
  UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
}
