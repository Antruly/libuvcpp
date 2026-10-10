/**
 * @file src/capi/uvcpp_c_db.h
 * @brief 数据库模块的 C 门面：连接、同步读写、事务、参数、结果集、连接池、异步。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它对应哪些 C++ 能力
 * -------------------
 *   | 这里 | C++ 那侧 |
 *   |------|----------|
 *   | `uvcpp_c_db_client_*` | `uvcpp_db_client` 的同名方法 |
 *   | `uvcpp_c_db_params_*` | `uvcpp_db_params`（`add_*` 逐个对上它的重载）|
 *   | `uvcpp_c_db_table_*` | `uvcpp_db_table` 的读侧 + `to_json()` / `to_csv()` |
 *   | `uvcpp_c_db_value_*` | `uvcpp_db_value` 的读侧（写侧走 `params`）|
 *   | `uvcpp_c_db_pool_*` | `uvcpp_db_pool`（借还 + 代借代还 + 观测）|
 *   | `uvcpp_c_db_async_*` | `uvcpp_db_async`（绑 client）与 `uvcpp_db_pool` 的异步那一组 |
 *
 * 上一批（`1.5.3`）把异步门面与连接池交给了 C++；这一片是它们的 C 面 ——
 * C# / Rust / 任何 P/Invoke 的调用方今天能拿到数据库模块的全部主路径，不需要
 * 自己写 `uv_queue_work` 那段样板，也不需要自己管一组连接。
 *
 * 返回码：**两套编码住在同一个 `int` 上**
 * ---------------------------------------
 * 本片返回 `int` 的函数遵守这条（不写清它，C# 侧一定会把两套搅在一起）：
 *
 *   - **`>= 0` 是 `uvcpp_c_db_status`**（`UVCPP_C_DB_OK = 0`，见下面那张表）。
 *     它**数值与 `uvcpp_db_status` 逐条对齐**（`.cpp` 里有一串 `static_assert`
 *     把这件事钉成编译期的话）—— 于是 C# 侧一张表就够，不需要维护第二份翻译。
 *   - **`< 0` 是 C 层错误**（`uvcpp_c_common.h` 里 `-20001` 起那几个：`E_STALE`
 *     句柄失效、`E_INVALID_ARG`、`E_EXCEPTION`……），**或者 libuv 错误码原样
 *     透传**。两段不会撞：db 状态码全在 `0..11`。
 *
 * "返回个数 / 长度"的函数（`_row_count`、`_to_json`、`_escape`……）用的是同一条
 * 约定的非负侧，别把它们当状态码读。
 *
 * 四类所有权（`uvcpp_c_common.h` 规矩 4 的三类 + 本片自己的两条）
 * --------------------------------------------------------------
 *  1. 入参 `const void*` + `size_t`：**立刻拷贝**，返回后你可以随便释放。
 *  2. 返回的 `const char*`：静态或线程局部，**不要释放**。
 *  3. **`uvcpp_c_db_table*` 是调用方拥有的句柄。** `_query` / `_table_schema`
 *     交出来的、以及异步回调收到的那个，用完都必须 `uvcpp_c_db_table_free()`。
 *     异步回调收到的表**在回调返回之后仍然有效**（它是一份拷贝，不借任何人）。
 *  4. **`uvcpp_c_db_value*` 是借来的视图**：它属于某张表（`_cell`）或某个 client
 *     （`_last_insert_id`），**没有 free 函数**，所有者一没它就失效。`_cell()`
 *     越界时给的是一枚静态 NULL 值视图（**不是 NULL 指针**），所以 C 侧不需要
 *     为越界写第二条分支。
 *  5. **池子借出的 `uvcpp_c_db_client*` 也是借来的**：它的生命周期到
 *     `uvcpp_c_db_pool_release()` / `_discard()` 为止，`uvcpp_c_db_client_free()`
 *     对它返回 `UVCPP_C_E_STATE`（拿它当自己建的 client 释放就是把池子的额度
 *     连同账目一起丢掉）。与 `uvcpp_c_quic.h` 里那枚借来的连接句柄同一类。
 *
 * 线程：**本片是本层唯一不查线程的一片**
 * -------------------------------------
 * `uvcpp_c_common.h` 规矩 5 说"除 `*_post()` 外都只能在循环线程上调用"，本片
 * **不适用**，理由有两条，都不是偷懒：
 *
 *  1. `uvcpp_db_client` 自己就是线程安全的（内部一把递归锁，见 `db/uvcpp_db.h`），
 *     它的 C 面没有任何理由比它更严；
 *  2. 异步那几条的语义**就是**"从任何线程投递" —— `uv_queue_work` 跨线程投递是
 *     libuv 明确支持的用法（`db/uvcpp_db_async.h` 的 future 糖正靠它）。
 *
 * 所以本片的函数**不会**返回 `UVCPP_C_E_WRONG_THREAD`。但并发不等于并行：一个
 * `uvcpp_c_db_client` 同一时刻只有一条 SQL 在跑，要并行用池子。
 *
 * 异步那几条**没有**循环参数（这是与 C++ 那侧最大的一处形状差别）
 * --------------------------------------------------------------
 * C++ 的 `uvcpp_db_async::query(uvcpp_loop* loop, …)` 收的是本库的 `uvcpp_loop*`。
 * C 面拿什么填它？**没有合法的东西可填**：公开头里没有任何函数能造出一个
 * `uvcpp_loop*`，而"把调用方手上的 `uv_loop_t*` 借进来"这条路是**错的** ——
 * `~uvcpp_loop()` 会对自己手上那个指针调 `uv_loop_close()`，成功就连同那块内存
 * 一起释放（`src/handle/uvcpp_loop.cpp` 那段长注释讲的就是这件事），于是"借"
 * 别人的循环 = 归还别人的内存。
 *
 * 给一个**没人填得合法**的参数比不给更糟（它会诱使 C# 侧传一个猜来的指针），
 * 所以本片干脆不给：
 *
 *   - **完成回调一律在门面自带的那条循环线程上被调。** 那条线程**懒起**
 *     （第一次投递时才建），`_async_free()` 里叫停并 join。
 *   - 要把结果搬回**你自己的**循环，就在回调里往那条循环发一次跨线程唤醒
 *     （libuv 上只有 `uv_async_send` 是线程安全的；本库 C 面已经有这条能力 ——
 *     `uvcpp_c_net.h` 里 `*_post()` 那一族就是这么做的）。
 *
 * 顺带一个结论：本头因此**不需要 include `<uv.h>`**，也**不能** include 它 ——
 * 发布包里没有 libuv 的头，而 `check_config_contract` 判据 4 用一个只带本包
 * `-I`、零 `-D` 的**纯 C 消费者**编 `#include <capi/uvcpp_c.h>`。
 *
 * 占位符是**方言差异**，驱动不替换算（照实现写）
 * ----------------------------------------------
 * `?` 是 MySQL / SQLite 的写法，PostgreSQL 要 `$1..$n`。本模块**不做**换算，写错
 * 了由服务端报 `PREPARE_FAILED`。要跨后端就自己看 `_driver_name()`。
 *
 * 不提供（刻意）
 * --------------
 *   - **`set_log()`**：C++ 那侧收的是 `std::function<void(const std::string&)>`。
 *     跨 FFI 传回来的每一行日志都要一份"字符串 + 生命周期"的约定，而它的用途
 *     只是诊断 —— 诊断有 `_last_error()` 与 `_status_name()` 两条路够了。
 *   - **`uvcpp_db_open()` 那枚 unique_ptr**：C 侧 `_new()` + `_open()` 两步就是它，
 *     而且失败时句柄还在手上（能问 `_last_error()`），比 C++ 那枚"失败即销毁"的
 *     糖更适合 FFI。
 *   - **`to_time_t()`**：返回值是 `time_t`，而它在 32/64 位、各平台上是不同的
 *     宽度 —— 跨 FFI 传它就要再定一条"这个数是几字节"的规矩。要时间戳就自己拿
 *     `_to_text()` 出来的 `"YYYY-MM-DD HH:MM:SS"` 去算。
 *   - **流式结果 / 逐行回调**：本片全是"整批到齐"。C++ 侧也没有流式接口可转。
 *   - **`uvcpp_db_driver.h` / `uvcpp_db_factory.cpp` 那套注册机制**：那是给新后端
 *     用的内部面，不是使用面。
 */

#pragma once
#ifndef SRC_CAPI_UVCPP_C_DB_H
#define SRC_CAPI_UVCPP_C_DB_H

#include <stddef.h>
#include <stdint.h>

#include "capi/uvcpp_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * 状态码
 * ------------------------------------------------------------------------
 * **数值与 `uvcpp_db_status` 逐条对齐**（`UVCPP_C_DB_OK == 0` 起，一个不多一个
 * 不少）—— `.cpp` 里有一串 `static_assert` 钉着这件事。之所以不复用
 * `uvcpp_c_error`：那一段是 -20000 起的**负数**，而 db 状态是**非负**的，两者在
 * 同一个 `int` 上各有各的段，混进一个枚举反而让"哪一段是什么"变得要读注释。
 *
 * 这个枚举里**没有** `E_STALE` 之类：句柄失效是 C 层的错误（负数），不是数据库
 * 的返回码。两者在同一个返回值里出现，靠的就是符号位分界（见文件开头）。
 */
enum uvcpp_c_db_status {
  /** 成功。 */
  UVCPP_C_DB_OK = 0,
  /** 连接串解析不了。 */
  UVCPP_C_DB_BAD_URL = 1,
  /** 这个 scheme 的后端没编进当前这个库。 */
  UVCPP_C_DB_NO_DRIVER = 2,
  /** 打开连接失败：不可达 / 认证失败 / 库不存在 / 超时。 */
  UVCPP_C_DB_OPEN_FAILED = 3,
  /** 连接没打开（**别拿它顶 `NO_CONNECTION`**，见那一格的注释）。 */
  UVCPP_C_DB_NOT_CONNECTED = 4,
  /** 语句准备失败（语法错、表/列不存在）。 */
  UVCPP_C_DB_PREPARE_FAILED = 5,
  /** 语句执行失败（约束冲突、类型不匹配、权限不足）。 */
  UVCPP_C_DB_EXEC_FAILED = 6,
  /** 参数与占位符对不上。 */
  UVCPP_C_DB_BIND_FAILED = 7,
  /** 这个后端不支持该操作。 */
  UVCPP_C_DB_UNSUPPORTED = 8,
  /** 用法错误（空 SQL、空结果集指针）。 */
  UVCPP_C_DB_MISUSE = 9,
  /** 内存不足。 */
  UVCPP_C_DB_OUT_OF_MEMORY = 10,
  /**
   * **连接池借不出连接**：已到上限且全在外借，等到借出超时也没等到。
   *
   * 与 `NOT_CONNECTED` 分开是承重的：后者说的是"这条连接没打开"，拿它顶前者
   * 就是撒谎 —— 池子里每条连接都是好的，只是**没有空闲的**。而两者的处置办法
   * 完全不同：前者该重连，后者该扩容或把借出的还回来。
   */
  UVCPP_C_DB_NO_CONNECTION = 11
};

/**
 * @brief 状态码的稳定短名（`"ok"` / `"no_connection"` / …）。
 *
 * @return **静态**字符串，不要释放。不认识的数返回 `"unknown"`（不是 NULL）。
 */
UVCPP_C_API const char* uvcpp_c_db_status_name(int status);

/**
 * @brief 这个库里编进来了哪些后端，逗号分隔（例 `"sqlite,mysql,postgres"`）。
 *
 * 走"调用方给缓冲区"那套约定。预编译包缺哪个后端时，这一行是第一个该看的地方
 * —— 拿到 `NO_DRIVER` 时先打印它。
 */
UVCPP_C_API int uvcpp_c_db_drivers(char* buf, size_t cap);

/* ------------------------------------------------------------------------
 * 句柄
 * ------------------------------------------------------------------------ */

/**
 * @brief 一条数据库连接。定义在 `.cpp` 里。
 *
 * 两种活法：`_client_new()` 建的是**你自己的**；池子 `_acquire()` 借出的也是这个
 * 类型，但它是**借来的** —— 那种情况下 `_client_free()` 返回 `E_STATE`，要还就
 * `uvcpp_c_db_pool_release()`。
 */
typedef struct uvcpp_c_db_client uvcpp_c_db_client;

/** @brief 一组绑定参数。`_params_new()` 建、`_params_free()` 废。 */
typedef struct uvcpp_c_db_params uvcpp_c_db_params;

/** @brief 一张结果集。**归调用方**，用完必须 `_table_free()`。 */
typedef struct uvcpp_c_db_table uvcpp_c_db_table;

/**
 * @brief 一个单元格的只读视图。**借来的**：没有 free 函数，所有者一没它就失效。
 */
typedef struct uvcpp_c_db_value uvcpp_c_db_value;

/** @brief 连接池。`_pool_new()` 建、`_pool_free()` 废（连带它的连接）。 */
typedef struct uvcpp_c_db_pool uvcpp_c_db_pool;

/**
 * @brief 异步门面：绑一条连接，或绑一个池子。
 *
 * 二选一（`_async_new()` / `_async_new_pool()`），绑上就不能换 —— C++ 那侧
 * `bind()` 能换连接，C 面刻意不给：FFI 侧"这条在途的活用的是哪条连接"必须一眼
 * 看得出，换来的灵活性不值那个排查成本。
 */
typedef struct uvcpp_c_db_async uvcpp_c_db_async;

/* ------------------------------------------------------------------------
 * 值
 * ------------------------------------------------------------------------ */

/** @brief 值的类型标签。**数值与 `uvcpp_db_type` 逐条对齐**。 */
enum uvcpp_c_db_value_type {
  /** SQL NULL。 */
  UVCPP_C_DB_NIL = 0,
  UVCPP_C_DB_INT64 = 1,
  UVCPP_C_DB_UINT64 = 2,
  UVCPP_C_DB_DOUBLE = 3,
  UVCPP_C_DB_BOOL = 4,
  UVCPP_C_DB_TEXT = 5,
  UVCPP_C_DB_BLOB = 6,
  UVCPP_C_DB_DATE = 7,
  UVCPP_C_DB_TIME = 8,
  UVCPP_C_DB_DATETIME = 9
};

/** @brief 类型标签（`UVCPP_C_DB_NIL` …）。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_value_type(const uvcpp_c_db_value* value);

/** @brief 类型的稳定短名（`"nil"` / `"text"` / …）。静态字符串，不要释放。 */
UVCPP_C_API const char* uvcpp_c_db_value_type_name(const uvcpp_c_db_value* value);

/**
 * @brief 是不是 SQL NULL（1/0）。
 *
 * 与"空串"是两回事：`_type()` 是 `TEXT` 而 `_size()` 是 0 时，`_is_null()` 为假。
 * （C++ 侧的旧实现把空串折叠成 NULL，那一版被特意修掉了。）
 */
UVCPP_C_API int uvcpp_c_db_value_is_null(const uvcpp_c_db_value* value);

/**
 * @brief 文本形态，走"调用方给缓冲区"那套约定。
 *
 * 数值/布尔给它们的字符串形态；**文本/blob/日期时间给原始字节**（可能含 0，
 * 要完整的就按返回长度取，别拿 `strlen` 量）。NULL 给空串（长度 0）—— 于是
 * "值是 NULL"和"值是空串"在文本形态上是同一个结果，分它要靠 `_is_null()`。
 */
UVCPP_C_API int uvcpp_c_db_value_to_text(const uvcpp_c_db_value* value, char* buf,
                                         size_t cap);

/** @brief 按 `int64_t` 读。类型不合适（或 NULL）给 0 —— **不抛、不炸**。 */
UVCPP_C_API int64_t uvcpp_c_db_value_to_int64(const uvcpp_c_db_value* value);

/** @brief 按 `uint64_t` 读。语义同上。 */
UVCPP_C_API uint64_t uvcpp_c_db_value_to_uint64(const uvcpp_c_db_value* value);

/** @brief 按 `double` 读。语义同上。 */
UVCPP_C_API double uvcpp_c_db_value_to_double(const uvcpp_c_db_value* value);

/** @brief 按布尔读（1/0）。语义同上。 */
UVCPP_C_API int uvcpp_c_db_value_to_bool(const uvcpp_c_db_value* value);

/** @brief 原始字节长度（文本/blob/日期时间）。数值型给 0。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_value_size(const uvcpp_c_db_value* value);

/**
 * @brief 原始字节（**二进制安全**，可能含 NUL）。
 *
 * @param data [出] 指向值内部字节的指针。**借来的**：值视图失效它就没了。
 * @param len  [出] 字节数。
 * @return `UVCPP_C_DB_OK`；负数 = 错误码。NULL 与数值型给长度为 0 的指针。
 */
UVCPP_C_API int uvcpp_c_db_value_bytes(const uvcpp_c_db_value* value,
                                       const char** data, size_t* len);

/* ------------------------------------------------------------------------
 * 参数
 * ------------------------------------------------------------------------
 * 一次查询的绑定参数（位置参数）。**值一律参数化**：拼 SQL 只解决"看起来对"，
 * 而 `?` 与 `$n` 的差别、以及"无参数走另一条转义路径"那类事故（本仓有过一次）
 * 都躲不过参数化。
 */

/** @brief 建一个空参数集。失败返回 NULL（内存不足）。 */
UVCPP_C_API uvcpp_c_db_params* uvcpp_c_db_params_new(void);

/** @brief 废掉它（连带里面那些值）。第二次 `free` 返回 `UVCPP_C_E_STALE`。 */
UVCPP_C_API int uvcpp_c_db_params_free(uvcpp_c_db_params* params);

/** @brief 追加一个 NULL。 */
UVCPP_C_API int uvcpp_c_db_params_add_null(uvcpp_c_db_params* params);

/** @brief 追加一个有符号整数。 */
UVCPP_C_API int uvcpp_c_db_params_add_int64(uvcpp_c_db_params* params,
                                            int64_t value);

/** @brief 追加一个无符号整数（`uint64_t` 全宽，BIGINT UNSIGNED 用得着）。 */
UVCPP_C_API int uvcpp_c_db_params_add_uint64(uvcpp_c_db_params* params,
                                             uint64_t value);

/** @brief 追加一个浮点数。 */
UVCPP_C_API int uvcpp_c_db_params_add_double(uvcpp_c_db_params* params,
                                             double value);

/** @brief 追加一个布尔（非 0 = 真）。 */
UVCPP_C_API int uvcpp_c_db_params_add_bool(uvcpp_c_db_params* params, int value);

/**
 * @brief 追加一个文本值。**立刻拷贝**，可以含 0 字节（按 `n` 走，不当 C 串）。
 *
 * `text == NULL && n == 0` 追加的是**空串**，不是 NULL —— 要 NULL 用
 * `_add_null()`。这条分界与结果集那边是同一条（`db/uvcpp_db_value.h` 契约 1）。
 */
UVCPP_C_API int uvcpp_c_db_params_add_text(uvcpp_c_db_params* params,
                                           const char* text, size_t n);

/** @brief 追加一个二进制值。语义与 `_add_text` 相同，只是类型标签是 BLOB。 */
UVCPP_C_API int uvcpp_c_db_params_add_blob(uvcpp_c_db_params* params,
                                           const void* data, size_t n);

/**
 * @brief 清空（保留句柄，接着还能用）。
 *
 * 存在的理由：位置参数与占位符**逐个对应**，一套参数循环复用是最常见的写法；
 * 每次都 new/free 一个句柄只会多出一次分配。
 */
UVCPP_C_API int uvcpp_c_db_params_clear(uvcpp_c_db_params* params);

/** @brief 现在有几个参数。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_params_count(const uvcpp_c_db_params* params);

/* ------------------------------------------------------------------------
 * 结果集
 * ------------------------------------------------------------------------ */

/** @brief 废掉一张表。第二次 `free` 返回 `UVCPP_C_E_STALE`。 */
UVCPP_C_API int uvcpp_c_db_table_free(uvcpp_c_db_table* table);

/** @brief 行数。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_table_row_count(const uvcpp_c_db_table* table);

/** @brief 列数。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_table_column_count(const uvcpp_c_db_table* table);

/** @brief 第 `index` 列的名字，走"调用方给缓冲区"那套约定。 */
UVCPP_C_API int uvcpp_c_db_table_column_name(const uvcpp_c_db_table* table,
                                             size_t index, char* buf,
                                             size_t cap);

/**
 * @brief 列名 → 下标。
 *
 * @return `>= 0` 是下标；**`UVCPP_C_E_NOT_FOUND` = 没有这一列**（与"第 0 列"
 *         分得开：0 是个合法的下标，拿 0 表示"没有"就是撒谎）。
 */
UVCPP_C_API int uvcpp_c_db_table_column_index(const uvcpp_c_db_table* table,
                                              const char* name);

/**
 * @brief 取一格。**借来的视图**（规矩 4 第 4 类）。
 *
 * 越界（行或列超范围）返回一枚**静态 NULL 值视图**，不是 NULL 指针 —— 于是
 * "这一格没有"在 C 侧就是 `_is_null()` 为真，不需要第二条分支。表句柄被 free
 * 之后再用它返回的指针是未定义行为（它没有魔数、不在活句柄表里）。
 */
UVCPP_C_API const uvcpp_c_db_value* uvcpp_c_db_table_cell(
    const uvcpp_c_db_table* table, size_t row, size_t column);

/**
 * @brief 影响行数。SELECT 时等于结果行数，失败时是 -1。
 */
UVCPP_C_API int64_t uvcpp_c_db_table_affected_rows(
    const uvcpp_c_db_table* table);

/** @brief 这张表来自哪儿（后端报得出就报）。走"调用方给缓冲区"那套约定。 */
UVCPP_C_API int uvcpp_c_db_table_name(const uvcpp_c_db_table* table, char* buf,
                                      size_t cap);

/**
 * @brief 整张表的 JSON。
 *
 * 这是本片最值钱的一条：C# 侧一行拿到 `System.Text.Json` 直接吃的字符串，
 * 比逐格跨 P/Invoke 快一个数量级，也不容易在类型转换上错。NULL 写 `null`，
 * blob 写成 `"[blob N bytes]"`（**有损**，要原始字节用 `_cell()` + `_bytes()`）。
 */
UVCPP_C_API int uvcpp_c_db_table_to_json(const uvcpp_c_db_table* table, char* buf,
                                         size_t cap);

/** @brief 整张表的 CSV（RFC4180：含逗号/引号/换行的字段加引号，内部引号翻倍）。 */
UVCPP_C_API int uvcpp_c_db_table_to_csv(const uvcpp_c_db_table* table, char* buf,
                                        size_t cap);

/* ------------------------------------------------------------------------
 * 连接
 * ------------------------------------------------------------------------ */

/** @brief 建一个**空的**连接句柄（还没连）。失败返回 NULL。 */
UVCPP_C_API uvcpp_c_db_client* uvcpp_c_db_client_new(void);

/**
 * @brief 废掉它（连接一并关掉）。
 *
 * 第二次 `free` 返回 `UVCPP_C_E_STALE`。两种 `E_STATE` 不是用法错，而是"现在不能
 * 从这儿放"：
 *
 *  - **池子借出的句柄** —— 它属于池子（要用 `uvcpp_c_db_pool_release()` 还、
 *    或者 `_pool_discard()` 丢掉）；
 *  - **还有异步门面绑着它** —— 那门面的工作线程随时会去读这条连接，先
 *    `_async_free()` 掉门面再回来。
 *
 * 第二种是**本片特有的**：C# 那种语言里两个对象互指是常态，而在 C 面这里，
 * "门面借着一个 client"这件事必须有一个判据，否则就是一次 use-after-free。
 */
UVCPP_C_API int uvcpp_c_db_client_free(uvcpp_c_db_client* client);

/**
 * @brief 打开连接。
 *
 * 连接串形状：`sqlite:///path/app.db`、`mysql://user:pass@host:3306/db`、
 * `postgres://user:pass@host:5432/db`（`postgresql://` 也认）。
 *
 * 已经打开时**先关掉旧的再开新的**，而且失败**不会**退回旧连接（见
 * `db/uvcpp_db.h` 那条注释）—— 所以别在事务里 open。
 */
UVCPP_C_API int uvcpp_c_db_client_open(uvcpp_c_db_client* client,
                                       const char* url);

/** @brief 关掉连接（句柄还能再 open）。没开过是**无操作**，不是错误。 */
UVCPP_C_API int uvcpp_c_db_client_close(uvcpp_c_db_client* client);

/**
 * @brief 手上有没有连接（1/0）。**不代表它还能用** —— 那个要 `_ping()`。
 */
UVCPP_C_API int uvcpp_c_db_client_is_open(const uvcpp_c_db_client* client);

/**
 * @brief 用最后一次 `open()` 的连接串重连。
 *
 * **不自动重连**是这一层的设计（`db/uvcpp_db.h`）：连接什么时候接由调用方决定，
 * 因为它才知道自己在不在事务里。在事务里调它 = 放弃那个事务。
 */
UVCPP_C_API int uvcpp_c_db_client_reconnect(uvcpp_c_db_client* client);

/** @brief 连接是否还活着（一次极轻的服务端往返；SQLite 是文件句柄检查）。 */
UVCPP_C_API int uvcpp_c_db_client_ping(uvcpp_c_db_client* client);

/** @brief `"sqlite"` / `"mysql"` / `"postgres"`；没打开时是空串。 */
UVCPP_C_API int uvcpp_c_db_client_driver_name(const uvcpp_c_db_client* client,
                                              char* buf, size_t cap);

/** @brief 最后一次 `open()` 用的连接串。 */
UVCPP_C_API int uvcpp_c_db_client_url(const uvcpp_c_db_client* client, char* buf,
                                      size_t cap);

/**
 * @brief 最近一次失败的人读原因。
 *
 * 成功**不清理**它（失败现场比成功更值得留着），所以"这句话是不是这一次的"
 * 只能靠"先看返回码、再看它"来判断。
 */
UVCPP_C_API int uvcpp_c_db_client_last_error(const uvcpp_c_db_client* client,
                                             char* buf, size_t cap);

/**
 * @brief 语句/连接超时（毫秒）。
 *
 * 必须在 `open()` **之前**设置才影响连接阶段；打开之后设置会尽量应用到当前
 * 连接（SQLite 是 busy timeout，MySQL 是读写超时，PG 是 `statement_timeout`）。
 */
UVCPP_C_API int uvcpp_c_db_client_set_timeout_ms(uvcpp_c_db_client* client,
                                                 int timeout_ms);

/* ------------------------------------------------------------------------
 * 同步读写
 * ------------------------------------------------------------------------
 * 这些**会阻塞调用线程**（数据库客户端库全是阻塞 API）。在事件循环线程上调它们
 * 就是把循环卡住 —— 要异步用下面的 `uvcpp_c_db_async_*`，要并发用池子。
 *
 * `sql` + `n`：SQL 的字节数（**不含结尾 NUL**），`n == 0` 视为空 SQL（`MISUSE`）。
 * 这样 C# 侧可以直接把手上的字节数组递进来，不必先凑一个 C 串。
 *
 * `params` 可以为 NULL（无参数）。
 */

/**
 * @brief 查询。
 *
 * @param out [出] **归调用方**的结果集句柄；失败时也会被填上一张空表（不是 NULL，
 *            也不是"上次的"），照样要 `_table_free()`。
 */
UVCPP_C_API int uvcpp_c_db_client_query(uvcpp_c_db_client* client,
                                        const char* sql, size_t n,
                                        const uvcpp_c_db_params* params,
                                        uvcpp_c_db_table** out);

/**
 * @brief 增删改。`affected` 可为 NULL。
 *
 * SQL 里**不要**带尾分号（各家处理不一），本模块不替你去掉。
 */
UVCPP_C_API int uvcpp_c_db_client_execute(uvcpp_c_db_client* client,
                                          const char* sql, size_t n,
                                          const uvcpp_c_db_params* params,
                                          int64_t* affected);

/**
 * @brief `execute` + 自增值，给"插入后要新主键"这条最常见的路。
 *
 * `out_id` 与 `out_has_id` 都可为 NULL。**"插进去了"与"拿不到 id"是两件事**：
 * 后端报不出自增时返回码仍是 `OK`，而 `out_has_id` 给 0（此时 `out_id` 是 0）。
 */
UVCPP_C_API int uvcpp_c_db_client_insert(uvcpp_c_db_client* client,
                                         const char* sql, size_t n,
                                         const uvcpp_c_db_params* params,
                                         int64_t* out_id, int* out_has_id);

/**
 * @brief 最近一次 INSERT 的自增值。**借来的视图**，落在句柄自己身上。
 *
 * 有效到下一次在**同一个句柄**上调它、或者 `_free()` 为止。
 */
UVCPP_C_API int uvcpp_c_db_client_last_insert_id(uvcpp_c_db_client* client,
                                                 const uvcpp_c_db_value** out);

/* ------------------------------------------------------------------------
 * 事务
 * ------------------------------------------------------------------------
 * 事务钉在**一条连接**上，所以：
 *   - 自己建的 `uvcpp_c_db_client`：直接 begin/commit/rollback；
 *   - 池子：`_pool_acquire()` 借一条出来做，做完 `_pool_release()`。
 *     池子的代借代还那组（`_pool_query` 等）**每个语句一条连接**，
 *     **事务在上面根本不成立** —— 池子因此不提供 `_pool_begin()`。
 */

/** @brief 开事务（SQLite 上是 `BEGIN`，MySQL/PG 上是 `START TRANSACTION` / `BEGIN`）。 */
UVCPP_C_API int uvcpp_c_db_client_begin(uvcpp_c_db_client* client);

/** @brief 提交。 */
UVCPP_C_API int uvcpp_c_db_client_commit(uvcpp_c_db_client* client);

/** @brief 回滚。 */
UVCPP_C_API int uvcpp_c_db_client_rollback(uvcpp_c_db_client* client);

/* ------------------------------------------------------------------------
 * 元信息与转义
 * ------------------------------------------------------------------------ */

/**
 * @brief 库里有哪些表。名字用 `'\n'` 连接（**文档写死这个编码**）。
 *
 * 为什么不是"回调每张表一次"或"数组出参"：那两样都要跨 FFI 定一套长度/生命
 * 周期的规矩，而表名里不可能有换行 —— 用 `'\n'` 连接是一个不需要任何额外约定
 * 的编码。走"调用方给缓冲区"那套（先 `cap = 0` 问长度）。
 */
UVCPP_C_API int uvcpp_c_db_client_table_names(uvcpp_c_db_client* client,
                                              char* buf, size_t cap);

/** @brief 列结构（`name` / `type` / `nullable` / `key` / `default` 那几列）。 */
UVCPP_C_API int uvcpp_c_db_client_table_schema(uvcpp_c_db_client* client,
                                               const char* table,
                                               uvcpp_c_db_table** out);

/**
 * @brief 字面量转义，**不含**两侧引号：`escape("a'b")` → `a''b`。
 *
 * 拼 SQL 时才需要；**能参数化就参数化**。
 */
UVCPP_C_API int uvcpp_c_db_client_escape(uvcpp_c_db_client* client,
                                         const char* text, size_t n, char* buf,
                                         size_t cap);

/**
 * @brief 标识符转义，**含**两侧引号（MySQL 反引号、PG/SQLite 双引号）。
 *
 * 它防的是"名字里有特殊字符"，**不是**"名字是恶意的"：表名来自外部输入时
 * 仍然要对着白名单查一遍。
 */
UVCPP_C_API int uvcpp_c_db_client_escape_identifier(uvcpp_c_db_client* client,
                                                    const char* text, size_t n,
                                                    char* buf, size_t cap);

/* ------------------------------------------------------------------------
 * 连接池
 * ------------------------------------------------------------------------
 * 池子买两件事：连接**复用**（不再每次付 open 的 1.9~2.4 ms）、**多条**连接
 * （锁不再互相等）。单线程串行跑的场景它一分钱都省不下来，那就别用。
 *
 * **`_acquire()` 会阻塞**（默认等到 5 s 超时），所以别在事件循环线程上调它；
 * 循环上要取连接就用异步那组（借还在池线程里做）。
 */

/** @brief 建一个还没 `init` 的池子。失败返回 NULL。 */
UVCPP_C_API uvcpp_c_db_pool* uvcpp_c_db_pool_new(void);

/**
 * @brief 废掉它（连带它开的连接）。
 *
 * 没有在外的借用时随便调。有的话它们会被一起释放，那些 C 句柄当场变成失效句柄
 * （再用它返回 `E_STALE`，不是崩溃）—— 但**在这之前**请先 `_async_free()` 掉绑着
 * 这个池子的异步门面：门面在池线程里借还连接，池子没了它就是在读已释放的内存。
 * 还有门面活着时这一条返回 `E_STATE`。
 */
UVCPP_C_API int uvcpp_c_db_pool_free(uvcpp_c_db_pool* pool);

/**
 * @brief 建池：**立刻**开 `min` 条，失败当场报出来（`OPEN_FAILED` / `BAD_URL` /
 *        `NO_DRIVER`），此时池子是空的、修好问题再 `init` 一次即可。
 *
 * `min > max` 会被夹到 `max`；`max == 0` 返回 `MISUSE`。
 */
UVCPP_C_API int uvcpp_c_db_pool_init(uvcpp_c_db_pool* pool, const char* url,
                                     size_t min, size_t max);

/** @brief 关掉空闲连接；在外的借用仍然归池子所有。之后 `_acquire()` 一律借不出。 */
UVCPP_C_API int uvcpp_c_db_pool_close(uvcpp_c_db_pool* pool);

/**
 * @brief 借一条连接。
 *
 * @param timeout_ms 等多久。`< 0` 表示一直等（**不建议**：见下）。
 * @param out        [出] 借到的句柄。**借来的**（规矩 4 第 5 类）：
 *                   `_client_free()` 对它返回 `E_STATE`，要用 `_pool_release()`
 *                   还、或者 `_pool_discard()` 丢掉。
 * @return `UVCPP_C_DB_OK`；借不到返回 `UVCPP_C_DB_NO_CONNECTION`（`out` 写
 *         NULL），具体原因在 `uvcpp_c_last_error_string()` 与 `_pool_last_error()`。
 *
 * 借到还之间这条连接**归你独占**，事务只能这么写。
 */
UVCPP_C_API int uvcpp_c_db_pool_acquire(uvcpp_c_db_pool* pool, int timeout_ms,
                                        uvcpp_c_db_client** out);

/**
 * @brief 还回去。
 *
 * @return `UVCPP_C_DB_OK`；`E_STALE` 是"这枚句柄已经不在了"（还过两次、或者它本来
 *         就不是本片发出来的），`E_INVALID_ARG` 是"它是个好句柄，但不是**这个**
 *         池子借出去的"，`E_STATE` 是"还有异步门面绑着它"。
 *
 * 三种都不是崩溃：查的是句柄登记表，不问那块内存（见 `uvcpp_c_internal.h` 里
 * `alive()` 那一段的顺序）。所以传个陌生指针进来是安全的错误码，不是野指针读。
 */
UVCPP_C_API int uvcpp_c_db_pool_release(uvcpp_c_db_pool* pool,
                                        uvcpp_c_db_client* client);

/**
 * @brief 这条连接坏了（比如刚返回 `NOT_CONNECTED`）：**关掉它、不还回池子**，
 *        把额度让给新开的。
 *
 * 与 `_release()` 一样解除借用关系，返回码也逐条相同（`E_STALE` / `E_INVALID_ARG`
 * / `E_STATE`）。**不自动重试**：写操作重试就是重复写。
 */
UVCPP_C_API int uvcpp_c_db_pool_discard(uvcpp_c_db_pool* pool,
                                        uvcpp_c_db_client* client);

/**
 * @brief 代借代还：一次调用借一条、用完立刻还。**事务别用这组**（见上面那段）。
 *
 * `params` 可为 NULL。`out` 与 `_client_query()` **同一条规矩**：它总是被填上一枚
 * 归调用方的句柄（拿不到连接时是一张空表），拿到就要 `uvcpp_c_db_table_free()`。
 * 之所以不在这里"失败写 NULL"：C 面没有 RAII，"什么时候要 free"多一种形状就是多
 * 一个漏点，而 C# 侧 wrapper 的 `finally` 也就能只有一句。
 */
UVCPP_C_API int uvcpp_c_db_pool_query(uvcpp_c_db_pool* pool, const char* sql,
                                      size_t n,
                                      const uvcpp_c_db_params* params,
                                      uvcpp_c_db_table** out);

/** @brief 代借代还的执行。`affected` 可为 NULL。 */
UVCPP_C_API int uvcpp_c_db_pool_execute(uvcpp_c_db_pool* pool, const char* sql,
                                        size_t n,
                                        const uvcpp_c_db_params* params,
                                        int64_t* affected);

/** @brief 借一条出来 ping 一下再还。 */
UVCPP_C_API int uvcpp_c_db_pool_ping(uvcpp_c_db_pool* pool);

/** @brief 现存连接数（含在借的）。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_pool_size(const uvcpp_c_db_pool* pool);

/** @brief 在外借的数。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_pool_in_use(const uvcpp_c_db_pool* pool);

/** @brief 池子里歇着的数。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_pool_idle(const uvcpp_c_db_pool* pool);

/** @brief 上限。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_pool_max_size(const uvcpp_c_db_pool* pool);

/** @brief 保底（`min`）。负数 = 错误码。 */
UVCPP_C_API int uvcpp_c_db_pool_min_size(const uvcpp_c_db_pool* pool);

/**
 * @brief 累计开过多少条连接 —— **连接是复用的见证**：它不随调用次数涨。
 *
 * 用出参而不是直接返回它：计数是 `uint64_t`，装不进一个 `int`（几千次查询不
 * 稀奇，而一个"看起来对"的窄化是这类接口最经典的静默错）。
 */
UVCPP_C_API int uvcpp_c_db_pool_created_total(const uvcpp_c_db_pool* pool,
                                              uint64_t* out);

/** @brief 累计借出过多少次是复用来的。语义与上面那条相同（出参的理由也相同）。 */
UVCPP_C_API int uvcpp_c_db_pool_reused_total(const uvcpp_c_db_pool* pool,
                                             uint64_t* out);

/** @brief 立刻把空闲连接收到 `min` 条，返回关掉几条。在借的一条都不动。 */
UVCPP_C_API int uvcpp_c_db_pool_close_idle(uvcpp_c_db_pool* pool);

/**
 * @brief 借不到连接时等多久，默认 5000 ms。`0` = 不等待（借不到立刻返回）。
 *
 * **这是池子唯一的防挂保险**：任何一条等着的池线程都必然会醒。别设成 `< 0`。
 */
UVCPP_C_API int uvcpp_c_db_pool_set_acquire_timeout_ms(uvcpp_c_db_pool* pool,
                                                       int ms);

/** @brief 最近一次失败的人读原因（借不出、开连接失败……）。 */
UVCPP_C_API int uvcpp_c_db_pool_last_error(const uvcpp_c_db_pool* pool,
                                           char* buf, size_t cap);

/* ------------------------------------------------------------------------
 * 异步
 * ------------------------------------------------------------------------
 * 门面替你写掉的那段样板是：`queue_work` → 在工作线程里跑阻塞查询 → 完成回调
 * 在**循环线程**上交付。三件容易错的事由它守住：回调一定在循环线程上一次、投递
 * 失败时回调不会被调、回调里 `_free()` 自己会被拒。
 *
 * **用的是全库唯一那条 libuv 默认线程池**（默认 4 条线程，与 `uv_fs_*` / DNS
 * 共用）。一条 30 秒的慢查询会把文件读写和 DNS 一起堵住 —— 这是已知代价，要变
 * 就调池子大小。本层不做背压。
 *
 * 交付形状：表是**拷贝**（C++ 侧递的是 `const&`，而 C 面要求它活过回调），
 * 归调用方、用完 `_table_free()`。完成回调在**门面自带的循环线程**上被调
 * （为什么没有循环参数，见文件开头那一段）。
 */

/**
 * @brief 绑定参数表的两种形状，`size` 打头、逐字段看（`uvcpp_c_common.h` 规矩 3）。
 *
 * 一张表同时给 `_query` 与 `_execute` 用：`_query` 只看 `on_table`、
 * `_execute` 只看 `on_executed`，另一半忽略。两者都**在回调里**才有效。
 *
 * `status` 是 `uvcpp_c_db_status`（非负）。`t` 为 NULL 有两种情形：这次投的是
 * `_execute`（根本没有表），或者交付不出来（`status != 0`）。
 */
typedef struct uvcpp_c_db_query_events {
  /** @brief **必须**是 `sizeof(uvcpp_c_db_query_events)` 或你那份结构体的字节数。 */
  uint32_t size;

  /**
   * @brief 查询到齐了。`t` **归调用方**，回调返回后仍然有效，用完
   *        `uvcpp_c_db_table_free()`。
   */
  void (*on_table)(void* user_data, int status, uvcpp_c_db_table* t);

  /** @brief 执行完了。`affected` 拿不到时是 -1。 */
  void (*on_executed)(void* user_data, int status, int64_t affected);
} uvcpp_c_db_query_events;

/**
 * @brief 建一个绑**某条连接**的异步门面。
 *
 * **不持有**那条连接：调用方必须保证 client 活得比所有在途操作久（工作回调跑在
 * 池线程上，那时 client 没了就是野指针）。这个门面可以跨线程用。
 *
 * 自带的那条循环线程**懒起**：一条 `_async_query` 都没投过就不起线程。
 */
UVCPP_C_API uvcpp_c_db_async* uvcpp_c_db_async_new(uvcpp_c_db_client* client);

/**
 * @brief 建一个绑**池子**的异步门面：每条活在池线程里自己借还一条连接。
 *
 * 事务**不能用它**（事务要独占一条连接直到 commit，而这里的借还在回调之前就
 * 结束了）—— 事务走 `_pool_acquire()`。
 */
UVCPP_C_API uvcpp_c_db_async* uvcpp_c_db_async_new_pool(uvcpp_c_db_pool* pool);

/**
 * @brief 废掉它。
 *
 * 两条承重语义（都是"FFI 侧一定会踩"的那种）：
 *
 *  1. **`free()` 返回之后不再有新的用户回调。** 还在排队没跑的活会把结果丢掉；
 *     而**已经在跑的那一次会跑完** —— `free()` 等它（闭包手里是共享状态，不是
 *     这枚句柄，所以这一等不会踩到已释放的东西）。
 *  2. **在回调里 `free()` 自己返回 `E_STATE`**（正在跑的那一层回调结束后再调）。
 *
 * 在途的活会被等完（有界：数据库调用自己有超时兜底）。之所以等得起，正是因为
 * 循环是门面自己的 —— 门面知道它一定在跑；换成调用方的循环就变成死等（那条循环
 * 此刻可能压根没在 run）。
 */
UVCPP_C_API int uvcpp_c_db_async_free(uvcpp_c_db_async* async);

/**
 * @brief 投一次查询。
 *
 * 回调在**门面自带的那条循环线程**上被调（懒起；为什么这里没有循环参数，见文件
 * 开头那一段）。这个函数**可以在任何线程上调**。
 *
 * @param ev 回调表。`on_table` 是这一笔的交付通道，为 NULL 当场
 *           `UVCPP_C_E_INVALID_ARG`（让它白跑不是"用法之一"，是漏了）。
 * @return `0` 投出去了；**负值是当场没投出去**（libuv 码 / C 层错误码），
 *         此时**回调不会被调** —— 判"这一次行不行"一律看返回值，别等回调。
 */
UVCPP_C_API int uvcpp_c_db_async_query(uvcpp_c_db_async* async,
                                       const char* sql, size_t n,
                                       const uvcpp_c_db_params* params,
                                       const uvcpp_c_db_query_events* ev,
                                       void* user_data);

/** @brief 投一次增删改。语义与上面那条相同，走 `ev->on_executed`。 */
UVCPP_C_API int uvcpp_c_db_async_execute(uvcpp_c_db_async* async,
                                         const char* sql, size_t n,
                                         const uvcpp_c_db_params* params,
                                         const uvcpp_c_db_query_events* ev,
                                         void* user_data);

/** @brief 在途条数（跨线程读是安全的）。它不回到 0 之前 `free()` 会等。 */
UVCPP_C_API size_t uvcpp_c_db_async_in_flight(const uvcpp_c_db_async* async);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SRC_CAPI_UVCPP_C_DB_H */
