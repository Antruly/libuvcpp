/**
 * @file src/capi/uvcpp_c_internal.h
 * @brief C ABI 层自己用的件：句柄魔数、边界处的异常收口、版本化结构体的字段可见性。
 * @author zhuweiye
 * @version 1.0.0
 *
 * **这个头不装出去。** CMake 里那句 `list(FILTER CAPI_HEADER_FILES EXCLUDE …)`
 * 与 `tests/tools/package_release.py` 的 `PRIVATE_HEADERS` 是一对，两处必须一起
 * 改 —— 只改一处不会报错，只会让发布包里悄悄多一份内部头（这一点与
 * `uvcpp_quic_ngtcp2.h` / `uvcpp_h3_nghttp3.h` 那两对是同一个形状）。
 *
 * 它不拉任何第三方头（这是它与那两份的区别），但装出去会把"这一层内部长什么样"
 * 摆进公开面：魔数、宏、`uvcpp_c_detail` 里的东西都没有任何稳定性承诺。
 *
 * 三件事在这里收口，各处不许重写
 * ------------------------------
 * 1. **异常边界**（`UVCPP_C_TRY` / `UVCPP_C_CATCH`）。每个导出函数的函数体都得
 *    这么包，理由见 `uvcpp_c_common.h` 规矩 2。
 * 2. **句柄的生死与线程归属**（`alive()` / `thread_ok()`）。魔数就是"这个指针
 *    是不是我们发出去的、而且还没被释放"的唯一判据。
 * 3. **版本化结构体的字段可见性**（`field_present()`）。规矩 3 的那一粒机制。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_INTERNAL_H
#define SRC_CAPI_UVCPP_C_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include <uv.h>

#include "capi/uvcpp_c_common.h"

namespace uvcpp {
class uvcpp_tcp_client;       // 只为 `tcp_client_unwrap()` 的返回类型
class uvcpp_quic_connection;  // 只为 `quic_conn_unwrap()` 的返回类型
}  // namespace uvcpp

namespace uvcpp_c_detail {

// -------------------------------------------------------------------------
// 异常边界
// -------------------------------------------------------------------------
//
// 用法（**每个**导出函数都长这样，一个都不许漏）：
//
//     extern "C" UVCPP_C_API int uvcpp_c_xxx(...) {
//       UVCPP_C_TRY
//         ...
//         return UVCPP_C_OK;
//       UVCPP_C_CATCH(UVCPP_C_E_EXCEPTION)
//     }
//
// 两个宏都是"函数体的一部分"而不是一对独立语句 —— 这是刻意的：合并成一个宏
// 就得把函数体塞进宏参数，那是 `#define` 里带逗号的经典陷阱（参数里有模板
// 逗号、有花括号初始化就炸）。分开写法只有一条纪律：`UVCPP_C_TRY` 与
// `UVCPP_C_CATCH` 必须在同一个函数里成对出现。
//
// **C++ 异常穿过 `extern "C"` 边界是未定义行为**（C 那侧没有 unwind 表），
// 所以这不是"保险"，是契约的一部分。
#define UVCPP_C_TRY try {

#define UVCPP_C_CATCH(ret)                                                     \
  }                                                                            \
  catch (const std::exception& e) {                                            \
    uvcpp_c_detail::set_last_error(e.what());                                  \
    return (ret);                                                              \
  }                                                                            \
  catch (...) {                                                                \
    uvcpp_c_detail::set_last_error("unknown C++ exception at the C ABI boundary"); \
    return (ret);                                                              \
  }

/** @brief 记一句"最近一次失败的话"（线程局部）。 */
void set_last_error(const char* what);

/** @brief 线程局部的 "最近一次失败的话"，没记过就是 nullptr。 */
const char* get_last_error();

/**
 * @brief 把一个 libuv 错误码转成文案（线程局部缓冲，下次调用即失效）。
 *
 * 不在 libuv 那一段里的码（`-errno` / `UV_EOF` / `UV_EAI_*`，最负到
 * `UV_UNKNOWN`）返回 `nullptr` —— 于是"这到底是谁家的码"这件事只在这一个地方
 * 判断。本层自己的码（-20001 起）走的正是这条 `nullptr` 路。
 *
 * 用 `uv_strerror_r()` 而不是 `uv_strerror()`：后者返回每个进程共享的静态缓冲，
 * 多线程下会被别的线程改写，而这一层就是给多线程的 FFI 用的。
 */
const char* uv_error_text(int err);

// -------------------------------------------------------------------------
// 活句柄登记表
// -------------------------------------------------------------------------
//
// 为什么要一张表，而不是只查魔数
// ------------------------------
// 只查魔数有一个说不出口的洞：**那时对象可能已经被 `delete` 了**。"读一块已
// 释放的内存，看那里的第一格是不是某个常数"是未定义行为 —— Release 下它经常
// 恰好还是旧值（于是"防住了"），也经常被分配器改写成别的（于是"没防住"），
// 而两种表现的差别不由我们决定。
//
// 所以判据改成两步，**顺序是承重的**：
//   1. 先问登记表"这个地址现在是不是一个活着的句柄"—— 这一步**一个字节都不读**
//      调用方给的地址，所以地址指向已释放内存时它是安全的；
//   2. 表里有，才去读魔数，确认类型对得上（TCP 客户端的句柄不能当服务端用）。
//
// 代价是每次调用一次哈希 + 一把锁（多循环时真的有别的线程在并发调）。这是
// 明确买下的：换到的是"用过的句柄永远给 `UVCPP_C_E_STALE`，而不是崩溃"这条
// 对 FFI 侧最有价值的承诺。
//
// 线程纪律没有因此放松：一个句柄仍然只能在它那条循环的线程上用（规矩 5）。
// 锁保护的是"另一个线程正在释放"这一瞬间，不是"你可以跨线程用同一个句柄"。
void registry_add(const void* handle);
void registry_remove(const void* handle);
bool registry_has(const void* handle);

/**
 * @brief 登记表当前的条目数。`uvcpp_c_live_handle_count()` 就是它。
 *
 * 它存在的理由不是"方便调试"，而是**收支平衡这件事从公开面上量不出来**：
 * 把 `*_free()` 的摘表拆掉之后，`alive()` 照样给出正确的 `E_STALE`（因为它要读
 * 的那 4 个字节被 glibc 的 tcache 顺手改掉了，见 `uvcpp_c_common.h` 那段）。
 * 这个数是那条不变式**唯一**可观测的形式，`capi_mutation.py` 的 M2/M3 靠它咬人。
 */
size_t registry_size();

// -------------------------------------------------------------------------
// 句柄
// -------------------------------------------------------------------------
//
// 每个不透明句柄的第一个成员都是它：`magic` 是"这是我这一层发出去的、还没被
// 释放"的类型凭据，`abi` 是造它的时候库的 ABI 版本（跨版本混用时不至于把
// 一个新句柄当成旧布局去读）。
//
// **`magic` 必须放在偏移 0**：`alive()` 会把任意一个 `uvcpp_c_xxx*` 当作
// `handle_head*` 去读。下一层的每个句柄结构体都有一句 `static_assert` 钉住
// 这一条（省掉它，将来有人在前面加个字段就是一次静默的内存踩踏）。
struct handle_head {
  uint32_t magic;
  uint32_t abi;

  /// 事件循环线程。`loop_armed` 为 0 时不做线程检查（还没跑起来的对象）。
  uv_thread_t loop_thread;
  int         loop_armed;
};

/**
 * @brief 本层的句柄魔数。四字节，小端写出来正好是那四个 ASCII 字符，
 *        于是 core dump 里那四个字节人是能读的（"ehc1"）。
 *
 * 用**字符字面量**拼而不是 `0x##a##u` 那种记号粘贴：后者要拼出 `0xeu` 这种
 * 非法 pp-number（`u` 只能进后缀，不能进十六进制数字），编不过。
 */
#define UVCPP_C_MAGIC(a, b, c, d)                                            \
  ((uint32_t)(unsigned char)(a) | ((uint32_t)(unsigned char)(b) << 8) |       \
   ((uint32_t)(unsigned char)(c) << 16) | ((uint32_t)(unsigned char)(d) << 24))

#define UVCPP_C_MAGIC_TCP_CLIENT UVCPP_C_MAGIC('e', 'h', 'c', '1')
#define UVCPP_C_MAGIC_TCP_SERVER UVCPP_C_MAGIC('e', 'h', 's', '1')

/* 批 3 的四种。命名继续按"那个类型叫什么"取末位：`c` = connection、`s` =
 * stream、`q` = request、`r` = response。**每个类型一枚**而不是所有句柄共用一
 * 枚：`alive()` 的第二段就是拿它认出"这不是我这一类句柄"，共用一枚等于把类型
 * 检查整段撤掉（拿一个 h2 流句柄去当响应构造器用，会一路走到读错布局的地方）。 */
#define UVCPP_C_MAGIC_H2_CONN UVCPP_C_MAGIC('e', 'h', '2', 'c')
#define UVCPP_C_MAGIC_H2_STREAM UVCPP_C_MAGIC('e', 'h', '2', 's')
#define UVCPP_C_MAGIC_H2_REQUEST UVCPP_C_MAGIC('e', 'h', '2', 'q')
#define UVCPP_C_MAGIC_H2_RESPONSE UVCPP_C_MAGIC('e', 'h', '2', 'r')

/* 批 3b 的八种。四字节的取法同上（读出来是 `"eqc1"` 那一类），但**换了一个头
 * 字母**（`e` → `q` / `h`）：
 *
 *   - `eq*1`：QUIC 那一族（client / server / tls / connection）；
 *   - `eh3*`：h3 那一族（connection / request / response / request-view），
 *     与 `eh2*` 排成一对，一眼能看出"这是哪一层的句柄"。
 *
 * 每个类型**仍然各有一枚**，理由同上（类型检查是 `alive()` 的第二段，共用一枚
 * 等于把那一整段撤掉）。`uvcpp_c_quic_connection` 那一枚是**借来的**句柄用的：
 * 它在底层连接被关掉时反登记 + 毒化，而它从来不由调用方 free（那个类型上没有
 * free 函数）—— 见 `uvcpp_c_quic.h` 文件头。 */
#define UVCPP_C_MAGIC_QUIC_CLIENT UVCPP_C_MAGIC('e', 'q', 'c', '1')
#define UVCPP_C_MAGIC_QUIC_SERVER UVCPP_C_MAGIC('e', 'q', 's', '1')
#define UVCPP_C_MAGIC_QUIC_TLS UVCPP_C_MAGIC('e', 'q', 't', '1')
#define UVCPP_C_MAGIC_QUIC_CONN UVCPP_C_MAGIC('e', 'q', 'n', '1')
#define UVCPP_C_MAGIC_H3_CONN UVCPP_C_MAGIC('e', 'h', '3', 'c')
#define UVCPP_C_MAGIC_H3_REQUEST UVCPP_C_MAGIC('e', 'h', '3', 'q')
#define UVCPP_C_MAGIC_H3_RESPONSE UVCPP_C_MAGIC('e', 'h', '3', 'r')
#define UVCPP_C_MAGIC_H3_REQ_VIEW UVCPP_C_MAGIC('e', 'h', '3', 'v')

/* 字节顺序钉在这里：`tests/capi/capi_common_func.c` 造"冒充的句柄"时用的是
 * **写死的** `0x31636865u`。两边同时改才可能漂，而这是一句编译期的话。 */
static_assert(UVCPP_C_MAGIC('e', 'h', 'c', '1') == 0x31636865u,
              "handle magic byte order changed");

/** @brief 给一个句柄头初始化：魔数、ABI 版本、线程位先清空。 */
inline void init_head(handle_head& h, uint32_t magic) {
  h.magic       = magic;
  h.abi         = static_cast<uint32_t>(UVCPP_C_ABI_VERSION);
  h.loop_armed  = 0;
  h.loop_thread = uv_thread_t();
}

/** @brief 毒化：`*_free()` 的全部内容就是这一行。之后任何 `alive()` 都为假。 */
inline void poison_head(handle_head& h) {
  h.magic = 0;
}

/**
 * @brief 这个指针是不是一个活的、本层发出去的、类型也对得上的句柄？
 *
 * 三段顺序是承重的：空指针 → **先问登记表**（不读内存）→ 才读魔数。把后两段
 * 调过来就是"读一块可能已经释放的内存"，见上面登记表那一段。
 *
 * 返回假的三种情况（空指针、已释放、类型不对）**都返回 `UVCPP_C_E_STALE`**，
 * 不区分 —— 对调用方来说，"我给了你个空指针"和"我给了你用过的指针"要做的事
 * 完全一样：别用，报错。
 */
template <typename H>
inline bool alive(const H* h, uint32_t magic) {
  if (h == nullptr) return false;
  if (!registry_has(h)) return false;
  return reinterpret_cast<const handle_head*>(h)->magic == magic;
}

/** @brief 给句柄头初始化 + 登记。**建句柄时用这个**，别只调 `init_head`。 */
template <typename H>
inline void register_head(H* h, uint32_t magic) {
  init_head(reinterpret_cast<handle_head&>(*h), magic);
  registry_add(h);
}

/** @brief 反登记 + 毒化。**释放前调它**。 */
template <typename H>
inline void unregister_head(H* h) {
  registry_remove(h);
  poison_head(reinterpret_cast<handle_head&>(*h));
}

/**
 * @brief 线程归属检查（规矩 5）。
 *
 * "循环线程"是**跑这条循环的那个线程**，不是"建对象的那个线程" —— 所以它由
 * `*_run()` 记下（`run()` 之前不检查，因为那一刻还没有"循环线程"可言）。
 * 对象被同一个循环服务的多个对象共享时（服务端的连接就是），它们共享同一份
 * 记录：指向同一个 `handle_head`，见 `uvcpp_c_net.cpp` 里 server 那一段。
 *
 * @return 1 = 可以继续，0 = 调用方应当返回 `UVCPP_C_E_WRONG_THREAD`。
 */
inline bool thread_ok_head(const handle_head* head) {
  if (!head->loop_armed) return true;
  uv_thread_t cur = uv_thread_self();
  return uv_thread_equal(&head->loop_thread, &cur) != 0;
}

/** @brief 上面那一个的句柄版：问的就是这个句柄自己的记录。 */
template <typename H>
inline bool thread_ok(const H* h) {
  return thread_ok_head(reinterpret_cast<const handle_head*>(h));
}

/** @brief 把某个句柄记为"循环线程 = 当前线程"。`run()` 的第一件事。 */
template <typename H>
inline void arm_loop_thread(H* h) {
  handle_head* head = reinterpret_cast<handle_head*>(h);
  head->loop_thread = uv_thread_self();
  head->loop_armed  = 1;
}

// -------------------------------------------------------------------------
// 版本化结构体：字段可见性
// -------------------------------------------------------------------------
//
// 规矩 3 的机制。调用方在 `size` 里写"我这份结构体有多大"，我们**逐字段**问
// "你这一格覆盖到了吗" —— 而不是把整个结构体一次读满。差别在于**老客户端**：
// 1.4.2 编出来的表只有 3 格，将来 1.5.0 的表有 5 格，那个老客户端传进来的
// `size` 只到第 3 格；"读满"会去读它**根本没有的字节**（那是调用方栈上的别的
// 东西，取值随机），"逐字段"则干净地当"第 4、5 格没给"。
//
// `size` 比本层知道的结构体还大也合法（那是"你比我新"），多余的部分忽略。
template <typename T, typename M>
inline bool field_present(uint32_t size, M T::*field) {
  // 偏移量的算法：拿一个静态的零对象取该成员的地址再相减。**不用 `offsetof`**
  // —— 它只对标准布局类型有定义，而这里的结构体里放着函数指针（合法，但
  // `offsetof` 在 C++ 里对非标准布局是条件支持）。`static T probe;` 是 POD，
  // 零初始化，且我们只取地址、不读写成员的值。
  static T probe;
  const std::size_t off = static_cast<std::size_t>(
      reinterpret_cast<const char*>(&(probe.*field)) -
      reinterpret_cast<const char*>(&probe));
  return static_cast<std::size_t>(size) >= off + sizeof(M);
}

/**
 * @brief 检查调用方给的 `size` 至少覆盖到 `size` 自己这一格。
 *
 * 一张回调表连自己有多大都没说清（`size < sizeof(uint32_t)`）就是一次误用：
 * 这时唯一正确的反应是整张表都不读。返回假时调用方应当报
 * `UVCPP_C_E_INVALID_ARG`，而不是"当作空表继续"。
 */
inline bool table_size_ok(uint32_t size) {
  return size >= static_cast<uint32_t>(sizeof(uint32_t));
}

// -------------------------------------------------------------------------
// 字符串出口：调用方给缓冲区
// -------------------------------------------------------------------------

/**
 * @brief `uvcpp_c_common.h` 里"调用方给缓冲区"那条约定的**唯一实现**。
 *
 * 写进 `uvcpp_c_detail` 而不是各文件各写一份：这条约定的全部风险都集中在
 * "返回长度"与"写不写结尾 NUL"这一处算术上，抄成两份就是两份会各自漂的算术。
 * `tests/capi/capi_webapp_func.c` 里那几条 `cap = 0` 先问长度、再按长度取值的
 * 断言，量的就是它。
 *
 * @return 字符串真实长度（不含结尾 NUL）；负数 = 错误码。
 */
int copy_out(const std::string& s, char* buf, size_t cap);

// -------------------------------------------------------------------------
// 跨文件的句柄解包
// -------------------------------------------------------------------------
//
// C 面**按模块分文件**，而有些能力要拿另一个模块的句柄当输入：h2 的驱动层是
// 建在一条已经握手过的 TCP 连接上的（`uvcpp_c_h2_connection_new(client, …)`），
// 而 `uvcpp_c_tcp_client` 的真身（`struct uvcpp_c_tcp_client`）只写在
// `uvcpp_c_net.cpp` 的文件作用域里 —— 别的 `.cpp` 拿不到它。
//
// **不把那个结构体搬进头里的理由**：搬进来就是"句柄的布局有一个第二份定义"，
// 将来给 TCP 句柄加一个字段、改了顺序，读它的地方不会有任何编译期提醒，只会
// 静默读错偏移。所以出口是**一个函数**：谁想知道"这个 C 句柄底下是谁"，都得
// 问 net 那一侧，判据（亡故 / 已被回收 / 底下是不是 nullptr）只有一处。
//
// 参数用 `const void*` 而不是 `uvcpp_c_tcp_client*`：这个头**不拉** `uvcpp_c_net.h`
// （那会把这层的地基和某个模块绑在一起，而 QUIC 那一侧将来也要解它自己的句柄）。
// 句柄本来就不是给本层读的，退化成一个地址正合适。
//
// @return 底下的 C++ 连接；句柄无效（空、已释放、类型不对、底下已经没了）返回
//         `nullptr` —— **不区分**，与 `alive()` 同一条：调用方要做的事一样。
uvcpp::uvcpp_tcp_client* tcp_client_unwrap(const void* handle);

/**
 * @brief 上面那一条的 QUIC 版（批 3b 加的，那个文件顶上早写着"QUIC 那一侧将来
 *        也要解它自己的句柄"）。
 *
 * 消费者只有一个：`uvcpp_c_http3.cpp` 建 `uvcpp_h3_connection` 时要那条
 * `uvcpp_quic_connection`，而"借来的句柄"的真身（`struct uvcpp_c_quic_connection`）
 * 只写在 `uvcpp_c_quic.cpp` 的文件作用域里。
 *
 * **"底下还没连上"也返回 `nullptr`**：借来的句柄从 `_client_new()` 那一刻就存在
 * （那时它是一枚空壳），而 h3 拿到一条还没有连接的连接是没意义的 —— 判据放在
 * 这里，`uvcpp_c_http3.cpp` 那一侧只需要判一次 null。
 */
uvcpp::uvcpp_quic_connection* quic_conn_unwrap(const void* handle);

/**
 * @brief "这条连接没了"：把一枚**借来的**连接句柄毒化 + 反登记。
 *
 * 上面那一条是**读**句柄，这一条是**改**它的死期，两者成对 —— 借来的句柄没有
 * `_free()`，所以"它什么时候不算数了"必须由知道这件事的那一侧说出来。
 *
 * 消费者同样只有一个：`uvcpp_c_http3.cpp`。理由在 `uvcpp_c_quic.cpp` 的文件头
 * 第 2 条，这里只留结论：h3 的 `start()` 会把 C++ 连接上那张回调表**整个换成
 * 它自己的**（h3 要吃 `on_read` / `on_alpn`），于是 `uvcpp_c_quic.cpp` 自己那套
 * 跳板（本来由它的 `on_close` 负责收尾）从此一次都不响。所以：
 *
 *   - 裸 QUIC（没装 h3）：QUIC 那一侧的 `on_close` 跳板收；
 *   - 装了 h3：h3 那一侧在它的 `on_disconnect` 里叫这一声。
 *
 * 两边都收不到的症状是同一个 —— 活句柄数收不回来（`capi_mutation.py` 的 M18）。
 *
 * @param handle 任何指针都安全：不是一枚活的 QUIC 连接句柄时什么都不做
 *               （先查登记表，不读调用方那块内存）。
 */
void quic_conn_detach(const void* handle);

}  // namespace uvcpp_c_detail

#endif  /* SRC_CAPI_UVCPP_C_INTERNAL_H */
