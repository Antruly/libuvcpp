/**
 * @file src/yyjson/uvcpp_json_doc.h
 * @brief JSON 后端：yyjson。解析快、DOM 常驻内存小；构造与写回用它自己的序列化器。
 * @author zhuweiye
 * @version 2.0.0
 *
 * 它解决什么
 * ----------
 * 框架默认的 JSON 解析后端是 nlohmann（`webapp/uvcpp_web_json.h`）。它是 header-only
 * 的、语义完整的、也是**慢**的：每个值都占一个 `std::map` / `std::vector` 节点，
 * 每次解析都伴随成百上千次 `operator new`，DOM 的常驻内存是输入的好几倍。
 * 本模块把这件事整体搬到 yyjson 上 —— **一个模块、两侧**：
 *
 *   解析侧（只读）  `uvcpp_json_doc` 拿 yyjson 解析，之后可以用与 nlohmann 路径
 *                   **同名**的访问器取值（`uvcpp_json_get_string/bool/int/double`）、
 *                   用同一套「查不到就给默认值」的语义遍历（`uvcpp_json_object_iter`
 *                   / `at()`）。想换后端的人，改的是**变量的类型**，不是调用点。
 *
 *   构造侧（可写）  `uvcpp_json_builder` 用 yyjson 的**可变 DOM** 建一棵树，句柄是
 *                   `uvcpp_json_node`；建完用 `uvcpp_json_write` 写出去，写的是
 *                   **yyjson 自己的序列化器**（不是本库手写的 `uvcpp_json_writer`）。
 *                   还能用 `add_copy()` 把解析侧的一棵子树整块搬进来。
 *
 * 两侧合起来才是"完整"：解析是一个方向，构造 + 序列化是另一个方向，yyjson 这一侧
 * 两条都有。**这也是它与 nlohmann 那条路在"合规"上的关键差别** —— 输出的字节由一份
 * 生态里已有的生产级 JSON 库产生，没有一行是为跑分写的手搓序列化。
 *
 * 怎么开
 * ------
 * `-DUVCPP_ENABLE_YYJSON=ON`（CMake 默认 **OFF**）。关掉时整个模块不编译、这个头里的
 * 声明也整段消失（与 `webapp/`、`wsdl/`、`quic/` 那几块同形）——所以**不要**在
 * 关掉的树上 include 它，那不是"降级"，是"这个模块不在库里"。
 *
 * 默认 OFF 说的是**最小源码构建**：它会让 configure 期多一个 FetchContent/系统依赖。
 * **六条发布腿全部显式 `-DUVCPP_ENABLE_YYJSON=ON`**（`release.yml`），所以装机包与
 * 发布包里的 libuvcpp 是**带这一侧的**——"开箱即用"这件事由发布物满足，不由 CMake
 * 的默认值满足。
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
 * `<yyjson.h>` 只出现在本模块的 `.cpp` 里，公开头一个 yyjson 类型都不出现：
 * 三种句柄各有各的装法 —— 值视图与节点句柄是 `const void*`（存
 * `yyjson_val*` / `yyjson_mut_val*`），迭代器那段状态用一个 `alignas` 的字节
 * 数组装着，尺寸由 `.cpp` 里的 `static_assert` 盯着。所以换掉 yyjson 不会动到
 * 使用者的任何一行代码 —— 与 pugixml 在 wsdl 里的位置相同。
 *
 * 与 nlohmann 路径**如实**存在的三处不同
 * --------------------------------------
 * 这三处都是**设计选择**、不是待修的差异；把它们写在这里，是为了让"换后端"
 * 这件事是可预期的：
 *
 *   1. **成员的顺序**：本后端的对象保持**文档里的顺序**（yyjson 按读入次序存，
 *      迭代器也按这个顺序走；构造侧的对象是**插入顺序**）。nlohmann 的对象类型是
 *      `std::map`，`dump()` 出来是**按键排好序**的。要逐字节比两份输出，就得让
 *      两边本来就同序 —— 用例里的语料键一律按升序写，构造侧那条路则在建树时
 *      **按键的升序插入**。键序本身不是数据，`uvcpp_json_get_*` 与 `get(key)`
 *      都不受它影响。
 *   2. **重复键**：两边都**接受**（都报 OK），但取值不同 —— 本后端取**第一个**
 *      （yyjson 的 `obj_get` 语义），nlohmann 取**最后一个**（`std::map` 插入
 *      覆盖）。RFC 8259 里重复键"行为不可移植"，所以这不是谁对谁错；用例把两边
 *      各自的行为都钉住了，而不是假装它们一样。
 *   3. **浮点写回去的形态**：写回走的是 **yyjson 自己的序列化器**
 *      （`yyjson_val_write_buf` / `yyjson_mut_val_write_buf`），不是本库的
 *      `uvcpp_json_writer`。整数、字符串转义、UTF-8 这三块两边**逐字节相同**
 *      （`1.0` 写 `1.0`、`1e2` 写 `100.0`、`\u0000` 是小写十六进制、`中文` 原样
 *      不被转义、`/` 不转义）；差别**只在浮点的科学记数法**，两种形态，都是
 *      实测的（`tests/functional/yyjson_doc_func.cpp` 第 6 组按这张表逐条钉）：
 *
 *         (a) **科学记数法的阈值不同** —— 本后端用的是 ECMAScript 那条线
 *             （`1e-6 ≤ |x| < 1e21` 一律定点）。所以 `1e15` 本后端是
 *             `1000000000000000.0`、nlohmann 是 `1e+15`；`123456789012345678901.0`
 *             本后端是 `123456789012345680000.0`、nlohmann 是
 *             `1.2345678901234568e+20`；反过来在小的那一端，`1e-5` 本后端是
 *             `0.00001`、nlohmann 是 `1e-05`。**值相同、字节不同。**
 *         (b) **指数里的 `+`** —— 两边都落在科学记数法时，nlohmann 写 `1e+300`、
 *             本后端写 `1e300`。负指数没有这个符号，所以 `1e-300` / `5e-324` /
 *             `2.5e-10` / `1.7e-308` 这些两边**逐字节相同**。
 *
 *      另有一条**不对称**：`dump()` 遇到非法 UTF-8 会抛 `type_error.316`，本模块
 *      原样透传那些字节（`YYJSON_WRITE_ALLOW_INVALID_UNICODE`，与老写器一样是
 *      二进制安全的）。所以"两份输出逐字节比"这件事在含非法字节的语料上**没有
 *      对照物** —— 用例里那条是单向的。
 *
 *      **写回不递归**：yyjson 的写器用显式栈（缓冲尾部那段 `yyjson_write_ctx`），
 *      所以"本后端递归、栈是你自己的"这条老差异**没有了**；构造侧与写回侧两边都
 *      不递归，深度只受解析侧预扫描（`<uvcpp/uvcpp_json_scan.h>`，默认 64）和内存
 *      的限制。十万层的文档写得出来 —— 用例里有一条钉着这件事。
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
 * 判据（`tests/functional/yyjson_doc_func.cpp` + `tests/functional/yyjson_build_func.cpp`）
 * -----------------------------------------------------------------------------------
 * 解析侧（`yyjson_doc_func.cpp`）：
 *
 *   * 与 nlohmann 路径的**状态逐条一致**（16 条坏输入 + 边界：空、纯空白、超长、
 *     超深、BOM、非法 UTF-8、重复键）；
 *   * 键有序的语料上，`uvcpp_json_write` 的输出与 nlohmann `dump()` **逐字节相同**
 *     （含全部转义、整数两端、嵌套结构）—— 语料里有一条**单列**出来只判"值相同"：
 *     就是上面第 3 处不同的 (a) 那一格（`1e+300`）；
 *   * 数字语料上**语义相同**（把两边输出都交给 nlohmann 再比 `==`），逐个覆盖
 *     `1.0`/`0.0`/`-0.0`/17 位 double/denormal/超 uint64 的大整数；
 *   * `uvcpp_json_get_*` 两组重载在同名键上返回同一个值，缺失键与类型不符时
 *     返回同一个默认值；
 *   * `memory_bytes()` 在 `reset()` 之后**回到 0**（"没有泄漏"这句话的可测形式），
 *     解析期间 `> 0` 且 `allocation_count() >= 1`。
 *
 * 构造侧（`yyjson_build_func.cpp`）：
 *
 *   * 每种类型建树 → 写出去 → 再解析回来，值逐个相同（往返）；
 *   * **键是拷进去的**：拿临时 `std::string` 当键，`clear()` 掉它之后对象还是对的；
 *     含 `\0` 的键（`uvcpp_json_key(ptr, len)`）是**一等公民**；
 *   * `add_copy()` 搬过来的子树在**源文档析构之后**仍然完整；
 *   * **句柄稳定**：取一个容器句柄，再往里加一万个元素，旧句柄照样能写；
 *   * `memory_bytes()` 的可测形式：空是 0、`reset()` 之后回 0、移动之后不重复计；
 *   * 键序 = **插入序**（与 nlohmann 的排序对照）。
 *
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

/// 构造侧的节点句柄（视图，不拥有数据）。定义在下面。
class UVCPP_API uvcpp_json_node;

/// 构造侧那棵树的所有者。定义在下面。
class UVCPP_API uvcpp_json_builder;

/// 把 DOM 写出去（走 **yyjson 自己的序列化器**）。**这两条前置声明是给 MSVC
/// 用的，不要挪走。**
///
/// 同名函数在两个类里还会各作为 `friend` 出现一次（它们要读私有的 `val_`，见下面
/// 的类定义）。MSVC 要求 `__declspec(dllexport/dllimport)` 出现在**第一条**声明
/// 上：先有 friend、后有带 `UVCPP_API` 的那条，就是
/// `error C2375: 'uvcpp::uvcpp_json_write': redefinition; different linkage`。
/// GCC / clang 对这个顺序不敏感（属性后置也认同一实体），所以本机两条腿都绿、
/// 只有 MSVC 红。放在这里 —— 类定义之前 —— 才能让带属性的那条排第一。
/// 真正的文档写在下面那两条同签名声明上（`UVCPP_YYJSON_ENABLE` 段的末尾）。
UVCPP_API json_write_status uvcpp_json_write(const uvcpp_json_value& v,
                                             std::string& out);
UVCPP_API json_write_status uvcpp_json_write(const uvcpp_json_node& v,
                                             std::string& out);

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

  uvcpp_json_doc(uvcpp_json_doc&& other) UVCPP_NOEXCEPT;
  uvcpp_json_doc& operator=(uvcpp_json_doc&& other) UVCPP_NOEXCEPT;
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
  // 写回是自由函数（它在公共面上的形状与那两个迭代器不同：使用者直接调它），所以
  // 它也要这一条才能读到 `val_`。**别的自由函数不给**：`uvcpp_json_get_*` 全部
  // 走公开的 `get()` / `as_*()`，多给一个朋友就多一处能绕过公开面的地方。
  friend json_write_status uvcpp_json_write(const uvcpp_json_value& v,
                                            std::string& out);
  // 构造侧的 `add_copy()` / `push_copy()` 要从解析侧的句柄深拷一棵子树，所以
  // builder 也要这一条。**一个方向**：解析侧从不碰构造侧的句柄。
  friend class uvcpp_json_builder;
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
// 构造侧：uvcpp_json_key / uvcpp_json_node / uvcpp_json_builder
// =========================================================================
//
// 为什么是**两个新类型**而不是给 `uvcpp_json_doc` 加几个方法：yyjson 的**可变值
// 读不出来**。`yyjson_val` 是 16 字节（tag + uni，子节点是 `ctn + 1` 的连续数组 /
// `uni.ofs` 字节偏移），`yyjson_mut_val` 是 24 字节（多一个 `next`，子节点是**循环
// 链表**）。两套句柄互不通用，迭代器也不通用（`yyjson_arr_iter` vs
// `yyjson_mut_arr_iter`）。所以解析侧（`uvcpp_json_doc` / `uvcpp_json_value`）与
// 构造侧（`uvcpp_json_builder` / `uvcpp_json_node`）是并列的两组，谁也不能冒充谁。
//
// 那"两边怎么互通"？**一个方向**：`uvcpp_json_builder::add_copy()` 把解析侧的
// 一棵子树**深拷**进构造侧（yyjson 的 `yyjson_val_mut_copy`，它是**迭代**的）。
// 反方向（构造侧 → 解析侧）不提供 —— 唯一的 `yyjson_mut_val_mut_copy` 是**递归**
// 的（上游头里就写着 "may cause a stack overflow"），而本模块的卖点之一就是两边
// 都不递归。要从构造侧读，写出去再 `parse()` 回来即可。

/**
 * @brief 对象的键。**视图**（只指，不拷）。
 *
 * 存在的理由是**少一套重载**：`add_int(obj, key, v)` 这样的每个成员函数都要能收
 * `"k"` / `(ptr, len)` / `std::string` 三种写法，没有这个类型就得每个函数写三份、
 * 26 个成员函数变成 78 个。有了它，隐式转换在**参数**上完成，每个操作只有一个
 * 函数（与 `uvcpp_json_writer::key()` 那三个重载等价，只是搬到了构造侧）。
 *
 * **键在写进对象时会被拷进 builder 自己的字符串池** —— 所以拿一个马上要消失的
 * 临时 `std::string` 当键是安全的（用例里有一条专门钉这件事）。
 *
 * `uvcpp_json_key(nullptr)` 是**无效**键：`add_*` / `push_*` 会返回失败，而不是
 * 悄悄建一个空键。空键要用 `""`（`ptr` 非空、`len` 为 0）—— 那是合法 JSON。
 */
struct UVCPP_API uvcpp_json_key {
  const char* ptr;  ///< 字节，不必以 NUL 结尾；`nullptr` 表示"没有这个键"
  size_t len;       ///< 字节数

  /// 以 NUL 结尾的串（`nullptr` ⇒ 无效键）。
  uvcpp_json_key(const char* s);
  /// 显式长度 ⇒ **可以含 `\0`**（JSON 的键允许任意字节）。
  uvcpp_json_key(const char* s, size_t len);
  /// 整个 `std::string`（`data()` 在 C++11 起就是非空的，所以空串是合法的空键）。
  uvcpp_json_key(const std::string& s);
};

/**
 * @brief 构造侧的一个节点。**视图**：只指，不拥有；树归 `uvcpp_json_builder`。
 *
 * 取值的那一套与 `uvcpp_json_value` 同形、同语义（不抛异常、不默认转换、类型不符
 * 给默认值），差别只在后端句柄是可变的那一套。**读自己刚建的树**用它；要读一份
 * 解析来的文档用 `uvcpp_json_value`。
 *
 * 与值视图一样，节点是**借来的**：builder 被 `reset()`、被移动走、或者被析构之后，
 * 之前拿到的节点全部悬空。
 *
 * **句柄是稳定的**：yyjson 的可变节点各自独立分配，往容器里追加元素只改参与者的
 * `next` 指针，不搬已经发出去的节点。所以"取一个句柄 → 往里加一万个元素 → 还用
 * 旧句柄"是正确的（用例里有一条钉着）。
 */
class UVCPP_API uvcpp_json_node {
 public:
  /// 无效节点（默认构造、失败的操作返回的都是它）。
  uvcpp_json_node();

  bool valid() const;

  /// 类型的可读名称（"null"/"bool"/"int"/"double"/"string"/"object"/"array"）。
  const char* type_name() const;

  bool is_null() const;
  bool is_bool() const;
  bool is_int() const;
  bool is_double() const;
  bool is_number() const;
  bool is_string() const;
  bool is_object() const;
  bool is_array() const;

  bool as_bool(bool def = false) const;
  long long as_int(long long def = 0) const;
  unsigned long long as_uint(unsigned long long def = 0) const;
  double as_double(double def = 0.0) const;
  std::string as_string(const std::string& def = std::string()) const;
  const char* as_cstr(size_t* len = nullptr) const;

  size_t size() const;

  /// 对象成员；键不存在、或本节点不是对象时返回无效节点。
  uvcpp_json_node get(const char* key) const;
  uvcpp_json_node get(const char* key, size_t key_len) const;

  /// 数组下标；越界、或本节点不是数组时返回无效节点。
  uvcpp_json_node at(size_t index) const;

 private:
  explicit uvcpp_json_node(const void* handle);
  const void* val_;
  friend class uvcpp_json_builder;
  friend json_write_status uvcpp_json_write(const uvcpp_json_node& v,
                                            std::string& out);
};

/**
 * @brief 用 yyjson 的**可变 DOM** 建一棵 JSON 树。
 *
 * 不可拷贝、可移动（与 `uvcpp_json_doc` 同形）。**不是线程安全的**：一棵树属于
 * 一个线程，与另外两个 DOM 相同；不同线程各建各的完全并行。
 *
 * 典型用法：
 *
 *     uvcpp_json_builder b;
 *     uvcpp_json_node root = b.make_object();
 *     b.set_root(root);
 *     b.add_int(root, "count", 3);
 *     uvcpp_json_node arr = b.add_array(root, "items");
 *     b.push_string(arr, "a");
 *     b.push_string(arr, "b");
 *     std::string out;
 *     if (uvcpp_json_write(root, out) == json_write_status::OK) { ... }
 *
 * **分配**：整棵树都从 builder 自己的字符串池 / 值池里出，池按 2 倍长块 ⇒ 建一棵
 * 几十个节点的树通常是**个位数次** `malloc`（对照：同样的树走 nlohmann 的
 * `std::map` DOM 是**四位数**次）。`reset()` 之后全部还回去。
 *
 * **键与字符串值都是拷进来的**，所以传临时对象、传栈上的缓冲区都安全。
 */
class UVCPP_API uvcpp_json_builder {
 public:
  uvcpp_json_builder();
  ~uvcpp_json_builder();

  uvcpp_json_builder(uvcpp_json_builder&& other) UVCPP_NOEXCEPT;
  uvcpp_json_builder& operator=(uvcpp_json_builder&& other) UVCPP_NOEXCEPT;
  uvcpp_json_builder(const uvcpp_json_builder&) = delete;
  uvcpp_json_builder& operator=(const uvcpp_json_builder&) = delete;

  /**
   * @brief 丢掉整棵树。`memory_bytes()` 回到 0，之前发出去的节点全部悬空。
   *
   * 树是**就地**重建的（下一次 `make_object()` 才真正分配），所以"reset 之后
   * `memory_bytes() == 0`"是一条能断言的话，不是形容词。
   */
  void reset();

  /**
   * @brief 给两个内存池一个初始尺寸的提示。**建树之前调**（池只影响**以后**分配
   *        的块，已经发出去的块不回收）。
   * @param values      预计的节点个数（普通值、容器、键都算一个节点）
   * @param string_bytes 预计的字符串与键的总字节数
   *
   * 不调也正确 —— 池的默认起点是 16 个节点 / 256 字节，按 2 倍长块自动往上长。
   * 调它是为了把"长块"那几次 `malloc` 提前省掉。
   */
  void reserve(size_t values, size_t string_bytes);

  /// 一个**游离**的空对象（还没挂上树）。用完 `add_val()` / `set_root()` 挂上去。
  uvcpp_json_node make_object();
  /// 一个**游离**的空数组。语义同上。
  uvcpp_json_node make_array();

  /**
   * @brief 把 `n` 挂成本树的根。
   * @return 已经有根了返回 false（**不覆盖**；要换先 `reset()`）。
   *
   * 也可以不调它：`root()` 会返回 `yyjson_mut_doc` 记住的那个根。挂着根不影响继续
   * 往里加东西 —— 建一棵树通常是"先挂根、再把根当对象/数组往里填"。
   */
  bool set_root(uvcpp_json_node n);

  /// 当前的根；没挂过根时返回无效节点。
  uvcpp_json_node root() const;

  // ── 对象侧 ──────────────────────────────────────────────────────────
  //
  // 这一族与下面那族**同形**：参数是"要往哪个容器加"，失败（`obj` 不是对象、
  // `key` 无效、`n` 无效、分配失败）一律返回 false / 无效节点，**不抛异常**，
  // 也不改动已经建好的部分。同名键**允许**（JSON 不禁止，本模块也不假装它禁止）；
  // 键序 = **插入序**（第 1 处不同：nlohmann 是按键排序的）。
  //
  // `add_int` / `add_uint` / `add_double` 是**三个不同的名字**，不是同一个名字的
  // 三个重载 —— 那样 `add(…, 1)` 会在 `long long` 与 `double` 之间二义
  // （两条都是"转换"级，谁也不比谁强）。分开之后各写各的，代价是 `add_int(…, 1l)`
  // 与 `add_uint(…, 1u)` 要自己挑一个；**负值传给 `add_uint` 会绕回**（与
  // `uvcpp_json_writer::value(unsigned long long)` 同形）。

  bool add_null(uvcpp_json_node obj, uvcpp_json_key key);
  bool add_bool(uvcpp_json_node obj, uvcpp_json_key key, bool v);
  bool add_int(uvcpp_json_node obj, uvcpp_json_key key, long long v);
  bool add_uint(uvcpp_json_node obj, uvcpp_json_key key, unsigned long long v);
  bool add_double(uvcpp_json_node obj, uvcpp_json_key key, double v);
  bool add_string(uvcpp_json_node obj, uvcpp_json_key key, const char* s);
  bool add_string(uvcpp_json_node obj, uvcpp_json_key key, const char* s,
                  size_t len);
  bool add_string(uvcpp_json_node obj, uvcpp_json_key key,
                  const std::string& s);

  /// 挂一个**空的**子对象并把它返回（没有子对象可挂时返回无效节点）。
  uvcpp_json_node add_object(uvcpp_json_node obj, uvcpp_json_key key);
  /// 挂一个**空的**子数组并把它返回。
  uvcpp_json_node add_array(uvcpp_json_node obj, uvcpp_json_key key);

  /// 挂一个已经建好的节点（`make_object()` / `make_array()` 的产物）。
  bool add_val(uvcpp_json_node obj, uvcpp_json_key key, uvcpp_json_node v);

  /**
   * @brief 把**解析侧**的一棵子树整块深拷进来。
   *
   * 拷完与源文档**再无关系**（字符串进本 builder 的池），源 `uvcpp_json_doc`
   * 可以立刻析构 —— 用例里有一条钉着。这是两个 DOM 之间**唯一**的通道。
   *
   * `src` 是无效视图时挂 `null`（与写回对无效视图的处理一致：JSON 里"没有这个值"
   * 有确切表示，不该让整棵树的构造失败）。
   */
  bool add_copy(uvcpp_json_node obj, uvcpp_json_key key,
                const uvcpp_json_value& src);

  // ── 数组侧 ──────────────────────────────────────────────────────────
  //
  // 与上面那族一一对应，只是"往哪个容器加"变成了"往哪个数组追加"，没有键。
  // 语义（失败返回 false、不抛、不改动、顺序 = 追加序）完全相同。

  bool push_null(uvcpp_json_node arr);
  bool push_bool(uvcpp_json_node arr, bool v);
  bool push_int(uvcpp_json_node arr, long long v);
  bool push_uint(uvcpp_json_node arr, unsigned long long v);
  bool push_double(uvcpp_json_node arr, double v);
  bool push_string(uvcpp_json_node arr, const char* s);
  bool push_string(uvcpp_json_node arr, const char* s, size_t len);
  bool push_string(uvcpp_json_node arr, const std::string& s);

  uvcpp_json_node push_object(uvcpp_json_node arr);
  uvcpp_json_node push_array(uvcpp_json_node arr);

  bool push_val(uvcpp_json_node arr, uvcpp_json_node v);
  bool push_copy(uvcpp_json_node arr, const uvcpp_json_value& src);

  // ── 记账 ────────────────────────────────────────────────────────────

  /// 当前树占用的字节数（借来的分配器**实测**，含本库加的对齐垫片）。空树是 0。
  size_t memory_bytes() const;
  /// `memory_bytes()` 的历史峰值。`reset()` 之后回到 0。
  size_t peak_bytes() const;
  /// 分配器被调用过的次数（yyjson 一棵树通常是几个块）。
  size_t allocation_count() const;

  /**
   * @brief 最近一次失败的原因。**本模块自己写的一句话**（yyjson 的可变 API 只
   *        返回 `bool` / `NULL`，不带错误信息），成功之后清空。
   */
  const char* last_error() const;

 private:
  /// 惰性建 yyjson 的可变文档 —— 第一次真正要分配时才建（所以空 builder 的
  /// `memory_bytes()` 是 0，而不是"一个还没用上的文档结构"）。
  bool ensure_doc();
  /// 记一句失败原因；传 `nullptr` 就是清空。
  void note(const char* why);

  void* doc_;  ///< `yyjson_mut_doc*`；nullptr 表示"还没建过、或刚 reset"
  struct alloc_stats* stats_;  ///< 分配器计数，与 `doc_` 同生共死
  const char* last_error_;
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
 * @brief 把一棵 DOM 序列化成 JSON 文本。**用的是 yyjson 自己的写器。**
 *
 * @param v   要写的值 / 节点。无效视图（或无效节点）写成 `null`，不报错 ——
 *            "没有这个值"在 JSON 里有确切表示，不该让整份响应构造失败。
 * @param out 产出。**契约**：调用前它的内容会被丢掉；成功时它就是完整的 JSON
 *            文本（**不含**结尾 NUL）；失败时它**是空的**。
 * @return 见下。
 *
 * 与老的那道桥（`uvcpp_json_write(value, uvcpp_json_writer&)`，1.7.2 已删除）
 * 相比，这是**换了一个序列化器**，不是换了一层皮：产出的字节由 yyjson 决定
 * （与 nlohmann 的差别见文件头第 3 处不同），失败的原因是 yyjson 的错误码而不是
 * 写者的状态。**API 是破坏性变更**，`CHANGELOG.md` 里写明了。
 *
 * 状态映射（`json_write_status`）：
 *
 * | 本模块             | yyjson 的错误码                    | 什么时候                          |
 * |--------------------|------------------------------------|-----------------------------------|
 * | `OK`               | `SUCCESS`                          | 正常                              |
 * | `MISUSE`           | `INVALID_PARAMETER` / `INVALID_VALUE_TYPE` | 传了空缓冲、根是空文档      |
 * | `NO_MEMORY`        | `MEMORY_ALLOCATION`                | 分配失败（见下）                  |
 *
 * 另外两个**选定的写标志让它们到不了**，如实写在这里而不是留白：
 * `NAN_OR_INF` 到不了（开了 `INF_AND_NAN_AS_NULL`，NaN/Inf 写成 `null`，与
 * `uvcpp_json_writer` 的既有行为一致）；`INVALID_STRING` 到不了（开了
 * `ALLOW_INVALID_UNICODE`，非法 UTF-8 原样透传）。`DEPTH` 也到不了 ——
 * `YYJSON_WRITER_DEPTH_LIMIT` 是编译期的 **0**，yyjson 在这一版里**不递归**
 * （显式栈）。`TOO_LARGE` / `UNCLOSED` 是流写器的状态，这条路一个都不产生。
 *
 * **性能契约**（可断言的那部分）：写回**不递归**；在一棵尺寸稳定的树上反复写，
 * 稳态是**零 `malloc`** —— 产出直接写进 `out` 已有的缓冲，只有当它装不下时才
 * 让它长大（每次翻倍，上限 64 MiB；再大改走"先分配、再搬进 `out`"那条路，多一
 * 次拷贝，任何尺寸都能成功）。
 *
 * 那条"装不下"比直觉大得多，**别按"缓冲等于产出长度"去改**：写器把它的
 * `yyjson_write_ctx` 栈放在缓冲**尾部**，而且每写一段之前按**最坏展开量**预留
 * （字符串那条是 `str_len * 6 + 16`）⇒ 刚好装下**一定**失败（实测 2 692 B 的产出
 * 要 > 2 820 B）。`out` 长大那几步可能抛 `std::bad_alloc`，本模块不抛异常，
 * 收成 `NO_MEMORY`。
 */
UVCPP_API json_write_status uvcpp_json_write(const uvcpp_json_value& v,
                                             std::string& out);
UVCPP_API json_write_status uvcpp_json_write(const uvcpp_json_node& v,
                                             std::string& out);

}  // namespace uvcpp

#endif  // UVCPP_YYJSON_ENABLE

#endif  // SRC_YYJSON_UVCPP_JSON_DOC_H
