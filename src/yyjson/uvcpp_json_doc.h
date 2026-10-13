/**
 * @file src/yyjson/uvcpp_json_doc.h
 * @brief 可选的 JSON 解析后端：yyjson。解析快、DOM 常驻内存小、解析器不递归。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它解决什么
 * ----------
 * 框架默认的 JSON 解析后端是 nlohmann（`webapp/uvcpp_web_json.h`）。它是 header-only
 * 的、语义完整的、也是**慢**的：每个值都占一个 `std::map` / `std::vector` 节点，
 * 每次解析都伴随成百上千次 `operator new`，DOM 的常驻内存是输入的好几倍。
 * 本模块给同一件事提供**第二个后端**：`uvcpp_json_doc` 拿 yyjson 解析，之后可以
 *
 *   * 用与 nlohmann 路径**同名**的访问器取值（`uvcpp_json_get_string/bool/int/double`），
 *   * 用同一套「查不到就给默认值」的语义遍历（`uvcpp_json_object_iter` / `at()`），
 *   * 用**同一个**输出器写回去（`uvcpp_json_write` → `uvcpp_json_writer`）。
 *
 * 也就是说：想换后端的人，改的是**变量的类型**，不是成片的调用点。
 *
 * 怎么开
 * ------
 * `-DUVCPP_ENABLE_YYJSON=ON`（默认 **OFF**）。关掉时整个模块不编译、这个头里的
 * 声明也整段消失（与 `webapp/`、`wsdl/`、`quic/` 那几块同形）——所以**不要**在
 * 关掉的树上 include 它，那不是"降级"，是"这个模块不在库里"。
 *
 * 关掉时的判据是**默认配置不受影响**，三条，都实测过（见 `CHANGELOG.md` 那一条）：
 *
 *   1. **导出符号集与 `DT_NEEDED` 逐字相同** —— 默认配置（webapp 开）下
 *      `nm -D --defined-only` 与 `readelf -d` 的 diff 都是**空的**；
 *   2. **编译单元只多一个**：深度预扫描从 `webapp/uvcpp_web_json.cpp` 搬到核心层
 *      `src/uvcpp/uvcpp_json_scan.cpp`（同一个判定要给两个后端用，而 yyjson 那侧
 *      不能依赖 webapp）。搬的是**代码逐行相同**的那一段；`uvcpp_config.h` 之外
 *      的头一个不改、用例表一条不多不少；
 *   3. **`.so` 的字节数会动**（目标文件个数与嵌进去的 `__FILE__` 串都会挪布局），
 *      这**不是**本模块的判据 —— 拿它当判据的话，换个构建目录就会"变红"。
 *      真正的判据是上面两条。
 *
 * 与 nlohmann 路径的关系：**不动它**
 * ---------------------------------
 * `using uvcpp_json = nlohmann::json`（`webapp/uvcpp_web_json.h`）保持原样，
 * `uvcpp_json_parse` / `uvcpp_json_dump` / `uvcpp_json_get_*` 的行为一个字不改。
 * 本模块不是"替换"，是"另一个可选项"：两者可以同时出现在一个工程里，甚至出现在
 * 一个函数里（`uvcpp_json_get_int` 有两组重载，第一参数是 `uvcpp_json` 还是
 * `uvcpp_json_value` 就决定了走哪个后端）。
 *
 * yyjson 是**私有依赖**
 * ---------------------
 * `<yyjson.h>` 只出现在本模块的 `.cpp` 里，公开头一个 yyjson 类型都不出现
 * （迭代器那块用一个 `alignas` 的字节数组装着，`static_assert` 在 `.cpp` 里
 * 盯着大小）。所以换掉 yyjson 不会动到使用者的任何一行代码 —— 与 pugixml 在
 * wsdl 里的位置相同。
 *
 * 与 nlohmann 路径**如实**存在的四处不同
 * --------------------------------------
 * 这四处都是**设计选择**、不是待修的差异；把它们写在这里，是为了让"换后端"
 * 这件事是可预期的：
 *
 *   1. **成员的顺序**：本后端的对象保持**文档里的顺序**（yyjson 按读入次序存，
 *      迭代器也按这个顺序走）。nlohmann 的对象类型是 `std::map`，`dump()` 出来
 *      是**按键排好序**的。要逐字节比两份输出，就得让源文档的键本来就是有序的
 *      （用例就是这么写的）；键序本身不是数据，`uvcpp_json_get_*` 与 `get(key)`
 *      都不受它影响。
 *   2. **重复键**：两边都**接受**（都报 OK），但取值不同 —— 本后端取**第一个**
 *      （yyjson 的 `obj_get` 语义），nlohmann 取**最后一个**（`std::map` 插入
 *      覆盖）。RFC 8259 里重复键"行为不可移植"，所以这不是谁对谁错；用例把两边
 *      各自的行为都钉住了，而不是假装它们一样。
 *   3. **数字写回去的形态**：`uvcpp_json_write` 用的是本库的 `uvcpp_json_writer`，
 *      于是 `1.0` 会写成 `1`、需要 17 位有效数字的 double 会写 17 位（nlohmann 的
 *      `dump()` 走的是它自己的最短往返算法，写 `1.0`）。**值相同、字节可能不同**；
 *      这是 `uvcpp_json_writer` 早就定下的行为（见它文件头「浮点为什么不是一句
 *      `to_string`」），本模块不改它，也不为它背锅。
 *   4. **深度**：两边都用 `<uvcpp/uvcpp_json_scan.h>` 预扫描，所以 `TOO_DEEP`
 *      是同一条线；但 `uvcpp_json_write` 是**递归**的，深度由解析时的 `max_depth`
 *      兜住（默认 64）。把 `max_depth` 设到几千以上、又拿 `uvcpp_json_write` 去写
 *      那份文档，栈是你自己的 —— 这条写在这里，不藏在实现里。
 *
 * 状态与 nlohmann 路径的对照
 * --------------------------
 * `json_doc_status` 是 `json_status` 前五项的同义物（一样的判定顺序、一样的边界）：
 *
 * | 本后端               | nlohmann 路径        | 判定                                   |
 * |----------------------|----------------------|----------------------------------------|
 * | `EMPTY`              | `EMPTY`              | `data == nullptr \|\| len == 0`        |
 * | `TOO_LARGE`          | `TOO_LARGE`          | `len > max_bytes`                      |
 * | `TOO_DEEP`           | `TOO_DEEP`           | `uvcpp_json_max_nesting() > max_depth` |
 * | `SYNTAX`             | `SYNTAX`             | 其余一切解析失败（含纯空白输入）       |
 * | `NO_MEMORY`          | （`bad_alloc`）      | 分配失败：本后端**不抛异常**，报这个   |
 *
 * 不在这张表里的三个（`MISMATCH`/`MISSING`/`UNKNOWN`）来自 webapp 的**反射读入**，
 * 不属于解析，所以不在这里出现。`json_doc_status_name()` 给日志用。
 *
 * 判据（`tests/functional/yyjson_doc_func.cpp`）
 * ------------------------------------------------
 *   * 与 nlohmann 路径的**状态逐条一致**（16 条坏输入 + 边界：空、纯空白、超长、
 *     超深、BOM、非法 UTF-8、重复键）；
 *   * 键有序的语料上，`uvcpp_json_write` 的输出与 nlohmann `dump()` **逐字节相同**
 *     （含全部转义、整数两端、嵌套结构）；
 *   * 数字语料上**语义相同**（把两边输出都交给 nlohmann 再比 `==`），逐个覆盖
 *     `1.0`/`0.0`/`-0.0`/17 位 double/denormal/超 uint64 的大整数；
 *   * `uvcpp_json_get_*` 两组重载在同名键上返回同一个值，缺失键与类型不符时
 *     返回同一个默认值；
 *   * `memory_bytes()` 在 `reset()` 之后**回到 0**（"没有泄漏"这句话的可测形式），
 *     解析期间 `> 0` 且 `allocation_count() >= 1`。
 * 吞吐与常驻内存这两个数（相对 nlohmann 的倍数）是**量出来的**、记在
 * `CHANGELOG.md` 与 `doc/json-guide.md` 里，不作为断言 —— 压测数字进用例就是
 * 造一条会随机变红的门禁。
 */

#pragma once
#ifndef SRC_YYJSON_UVCPP_JSON_DOC_H
#define SRC_YYJSON_UVCPP_JSON_DOC_H

#include <cstddef>
#include <string>

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>
#include <uvcpp/uvcpp_json_writer.h>

#if UVCPP_YYJSON_ENABLE

namespace uvcpp {

// =========================================================================
// 解析
// =========================================================================

/** @brief 解析结果。前五项与 `webapp/uvcpp_web_json.h` 的 `json_status` 同义。 */
enum class json_doc_status : int {
  OK = 0,
  EMPTY,      ///< 输入为空
  SYNTAX,     ///< 语法错误（含纯空白输入：它不是"空"，是"不是 JSON"）
  TOO_DEEP,   ///< 嵌套超过 max_depth（防爆栈）
  TOO_LARGE,  ///< 超过 max_bytes
  NO_MEMORY,  ///< 分配失败（本后端不抛异常，报这个）
};

/** @brief 状态的可读名称，可直接写日志。 */
UVCPP_API const char* json_doc_status_name(json_doc_status status);

/**
 * @brief 解析的可调限制。
 *
 * 默认值与 webapp 的 `json_parse_options` **相同**（深度 64、8 MiB），但两者是
 * 各自独立的类型 —— 本模块不依赖 webapp，所以不能共用那一个。
 */
struct UVCPP_API json_doc_options {
  size_t max_depth;  ///< 最大嵌套层数，默认 64
  size_t max_bytes;  ///< 最大输入字节数，默认 8 MiB

  /// 显式构造函数（成员初始化器会破坏聚合初始化，与 `json_parse_options` 同理）
  explicit json_doc_options(size_t depth = 64,
                            size_t bytes = 8u * 1024u * 1024u);
};

/// DOM 里的一个值（视图，不拥有数据）。定义在下面。
class UVCPP_API uvcpp_json_value;

/**
 * @brief 一份解析好的 JSON 文档（**拥有**整棵 DOM）。
 *
 * 不可拷贝、可移动。移动之后源对象变成"没有文档"（`valid() == false`），
 * 与 `std::unique_ptr` 的语义一致 —— DOM 的所有权始终只有一份。
 *
 * **不是线程安全的**：一份文档属于一个线程，与 nlohmann 那边的 DOM 相同。
 * 解析本身没有全局状态，所以不同线程各持有各自的文档是完全并行的。
 *
 * 值（`uvcpp_json_value`）是**借来的**：文档被 `reset()`、被重新 `parse()`、
 * 或者被析构之后，之前拿到的值全部悬空。这条与 `std::string_view` 同形。
 */
class UVCPP_API uvcpp_json_doc {
 public:
  uvcpp_json_doc();
  ~uvcpp_json_doc();

  uvcpp_json_doc(uvcpp_json_doc&& other) noexcept;
  uvcpp_json_doc& operator=(uvcpp_json_doc&& other) noexcept;
  uvcpp_json_doc(const uvcpp_json_doc&) = delete;
  uvcpp_json_doc& operator=(const uvcpp_json_doc&) = delete;

  /**
   * @brief 解析 JSON。
   *
   * @param data 输入（可为二进制，不要求 NUL 结尾；解析期间**不修改**它）
   * @param len  字节数
   * @param opts 限制
   * @return 见 `json_doc_status`
   *
   * **失败时不改动本对象**：原来那份文档（连同它的值视图）继续有效。这条与
   * nlohmann 路径的 `uvcpp_json_parse(data, len, out)` 逐字一致 —— 调用方常见
   * 的写法是"解析失败就沿用上一次的"，两者必须给出同样的行为。
   */
  json_doc_status parse(const char* data, size_t len);
  json_doc_status parse(const char* data, size_t len,
                        const json_doc_options& opts);
  json_doc_status parse(const std::string& text);
  json_doc_status parse(const std::string& text,
                        const json_doc_options& opts);

  /// 有一份解析成功的文档吗。
  bool valid() const;

  /// 根值；`valid()` 为假时返回**无效视图**（`is_null()` 也为假的那种空）。
  uvcpp_json_value root() const;

  /// 丢掉当前文档。之后 `valid()` 为假、`memory_bytes()` 为 0；值视图全部悬空。
  void reset();

  /// 当前 DOM 占用的字节数（借来的分配器**实测**，含本库加的对齐垫片）。空文档是 0。
  size_t memory_bytes() const;

  /// 最近一次**成功**解析里 `memory_bytes()` 的峰值。
  size_t peak_bytes() const;

  /// 最近一次**成功**解析里分配器被调用的次数（yyjson 一份文档通常是几个块）。
  size_t allocation_count() const;

  /**
   * @brief 最近一次失败的原因原文（后端的原话，调试用）。
   * @return 一直有效的常量串；没失败过或已 `reset()` 时是 `""`
   */
  const char* last_error() const;

  /// 最近一次失败的字节偏移（后端给的 `pos`）。没失败过时是 0。
  size_t last_error_position() const;

 private:
  void* doc_;  ///< `yyjson_doc*`；nullptr 表示没有文档（公开头里不出现 yyjson 类型）
  struct alloc_stats* stats_;  ///< 分配器计数，与 `doc_` 同生共死
  const char* last_error_;     ///< 指向常量串
  size_t last_error_pos_;
};

// =========================================================================
// 值（视图）
// =========================================================================

/**
 * @brief DOM 里的一个值。**视图**：只指，不拥有。
 *
 * 取值函数一律**不抛异常**、也一律**不默认转换**：类型不符就给默认值
 * （`as_string()` 拿到数字给 `def`，而不是把数字写成字符串）。这条与
 * `uvcpp_json_get_*` 的理由相同：这些调用点在 libuv 回调里，抛异常会穿到 C 栈，
 * 而"猜一个值"比"给默认值"更危险。
 *
 * 唯一一处**有意的**宽松是 `as_int()` / `as_double()` 对数字的处理：
 * `3.0` 能当 `3` 读（JSON 的数字不分整浮），`18446744073709551615` 走
 * `as_uint()`、走 `as_int()` 会得到截断后的值 —— 后者与 nlohmann 路径的
 * `uvcpp_json_get_int` 行为一致（它也是 `static_cast<long long>`）。
 */
class UVCPP_API uvcpp_json_value {
 public:
  /// 无效视图（默认构造、越界 `at()`、缺失的 `get()` 都是它）。
  uvcpp_json_value();

  bool valid() const;

  /// 类型的可读名称（"null"/"bool"/"int"/"double"/"string"/"object"/"array"）。
  const char* type_name() const;

  bool is_null() const;
  bool is_bool() const;
  bool is_int() const;     ///< 整数（有符号或无符号都算）
  bool is_double() const;  ///< IEEE 浮点（放不下 uint64/int64 的大整数也读成它）
  bool is_number() const;  ///< `is_int() || is_double()`
  bool is_string() const;
  bool is_object() const;
  bool is_array() const;

  bool as_bool(bool def = false) const;
  long long as_int(long long def = 0) const;              ///< 浮点截断；非数字给 def
  unsigned long long as_uint(unsigned long long def = 0) const;
  double as_double(double def = 0.0) const;               ///< 整数也接受
  std::string as_string(const std::string& def = std::string()) const;

  /**
   * @brief 零拷贝取字符串。
   * @param len 非空时写入字节数
   * @return 指向 DOM 内部缓冲的指针（**不**以 NUL 结尾）；不是字符串时 `nullptr`
   */
  const char* as_cstr(size_t* len = nullptr) const;

  /// 数组 / 对象的元素个数；其它类型是 0。
  size_t size() const;

  /// 对象成员；键不存在、或本值不是对象时返回无效视图。
  uvcpp_json_value get(const char* key) const;
  uvcpp_json_value get(const char* key, size_t key_len) const;

  /// 数组下标；越界、或本值不是数组时返回无效视图。
  uvcpp_json_value at(size_t index) const;

 private:
  explicit uvcpp_json_value(const void* handle);
  const void* val_;
  friend class uvcpp_json_doc;
  friend class uvcpp_json_object_iter;
  friend class uvcpp_json_array_iter;
  // 桥是自由函数（它在公共面上的形状与那两个迭代器不同：使用者直接调它），所以
  // 它也要这一条才能读到 `val_`。**别的自由函数不给**：`uvcpp_json_get_*` 全部
  // 走公开的 `get()` / `as_*()`，多给一个朋友就多一处能绕过公开面的地方。
  friend json_write_status uvcpp_json_write(const uvcpp_json_value& v,
                                            uvcpp_json_writer& w);
};

/**
 * @brief 迭代一个对象的成员，**按文档里的顺序**（见文件头第 1 条不同）。
 *
 * 用法：
 *
 *     uvcpp_json_object_iter it(obj);
 *     const char* key; size_t key_len; uvcpp_json_value v;
 *     while (it.next(&key, &key_len, v)) { ... }
 *
 * 迭代器**借** DOM 的内存（内部就是后端那个迭代器的状态），所以与值视图一样：
 * 文档没了就别再用它。
 */
class UVCPP_API uvcpp_json_object_iter {
 public:
  explicit uvcpp_json_object_iter(const uvcpp_json_value& obj);

  /// 拷一份键（方便）。`key` 是 UTF-8 文本，可能含 `\0`，所以别拿它当 C 串。
  bool next(std::string& key, uvcpp_json_value& value);

  /// 零拷贝的键（`*key` 不以 NUL 结尾）。给热路径用。
  bool next(const char** key, size_t* key_len, uvcpp_json_value& value);

 private:
  // 后端迭代器的状态（yyjson 的 `obj_iter`）。公开头里不能出现 yyjson 类型，
  // 所以按"能装下"来定尺寸，.cpp 里有 `static_assert` 盯着这件事。
  static const size_t k_storage_bytes = sizeof(void*) * 6;
  alignas(std::max_align_t) unsigned char storage_[k_storage_bytes];
};

/** @brief 迭代一个数组的元素，按顺序。用法与 `uvcpp_json_object_iter` 同形。 */
class UVCPP_API uvcpp_json_array_iter {
 public:
  explicit uvcpp_json_array_iter(const uvcpp_json_value& arr);
  bool next(uvcpp_json_value& value);

 private:
  static const size_t k_storage_bytes = sizeof(void*) * 6;
  alignas(std::max_align_t) unsigned char storage_[k_storage_bytes];
};

// =========================================================================
// 与 nlohmann 路径同名的入口
// =========================================================================
//
// 下面这些函数与 `webapp/uvcpp_web_json.h` 里的**同名**，靠第一/第三参数的
// 类型区分重载 —— 想换后端的人改的是变量的类型，调用点照抄。

/**
 * @brief 解析 JSON，**不抛异常**。形状与 nlohmann 路径的同名函数一致。
 * @param out 成功时写入；**失败时不改动**
 */
UVCPP_API json_doc_status uvcpp_json_parse(const char* data, size_t len,
                                          uvcpp_json_doc& out);
UVCPP_API json_doc_status uvcpp_json_parse(const char* data, size_t len,
                                          uvcpp_json_doc& out,
                                          const json_doc_options& opts);
UVCPP_API json_doc_status uvcpp_json_parse(const std::string& s,
                                          uvcpp_json_doc& out);

/** @brief 取字符串；键不存在、类型不是字符串、或 `obj` 不是对象时返回 `def`。 */
UVCPP_API std::string uvcpp_json_get_string(const uvcpp_json_value& obj,
                                            const char* key,
                                            const std::string& def);
/** @brief 取布尔；同上。 */
UVCPP_API bool uvcpp_json_get_bool(const uvcpp_json_value& obj, const char* key,
                                   bool def);
/**
 * @brief 取整数。
 * @return 取到了返回 true 并写入 `out`；否则返回 false 且不写 `out`。
 *         数值是浮点时按截断处理（`3.9` → `3`），与 nlohmann 路径一致。
 */
UVCPP_API bool uvcpp_json_get_int(const uvcpp_json_value& obj, const char* key,
                                  long long& out);
/** @brief 取浮点；整数也接受。取不到返回 false。 */
UVCPP_API bool uvcpp_json_get_double(const uvcpp_json_value& obj,
                                     const char* key, double& out);

/**
 * @brief 把 DOM 写进 `uvcpp_json_writer`——本后端与"输出"之间的那道桥。
 *
 * @param v 要写的值（无效视图写成 `null`，不报错：与 `uvcpp_json_writer` 对
 *          `value((const char*)nullptr)` 的处理同一个理由 —— "没有这个值"在
 *          JSON 里有确切表示，不该让整份响应构造失败）
 * @param w 目标写者；状态由写者自己维护（它失败是粘性的）
 * @return 写者的状态（失败时是它当时的状态，本函数不吞）
 *
 * 递归深度由**解析时**的 `max_depth` 兜住（默认 64，见文件头第 4 条不同）。
 * 数字走 `uvcpp_json_writer` 的形态（第 3 条不同），对象按键的**文档顺序**写
 * （第 1 条不同）。
 */
UVCPP_API json_write_status uvcpp_json_write(const uvcpp_json_value& v,
                                             uvcpp_json_writer& w);

}  // namespace uvcpp

#endif  // UVCPP_YYJSON_ENABLE

#endif  // SRC_YYJSON_UVCPP_JSON_DOC_H
