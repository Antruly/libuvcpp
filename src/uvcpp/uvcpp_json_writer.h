/**
 * @file src/uvcpp/uvcpp_json_writer.h
 * @brief 应用层 JSON 构造器：直写缓冲、不经 DOM、不抛异常、零依赖。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么还要一个 JSON 构造器
 * --------------------------
 * 本仓已有的 JSON 支持在 `webapp/uvcpp_web_json.h`（nlohmann 后端），它做的是
 * **解析**与 **DOM 序列化**两件事。那两层都不适合"服务端把几个字段拼成一段
 * JSON 发出去"这个最常见的场景：
 *
 *   - **DOM 那层要先建树再 dump**，而服务端的数据本来就在手边（几个成员变量、
 *     一次查询的结果），先搬进一棵 `nlohmann::json` 再序列化是白付一遍。
 *   - **它整个挂在 webapp 模块上，而 webapp 默认关掉**（`UVCPP_BUILD_WEBAPP=OFF`，
 *     `CMakeLists.txt:65`），还会拉一个几十 MB 的 nlohmann 进来。写一段 JSON
 *     不该需要这些。
 *
 * 于是今天只能手写转义 —— 仓里就有一处现成的（`uvcpp_web_app.h:728`）：
 *
 *     resp.json_str("{\"size\":" + std::to_string(f->size()) + "}");
 *
 * 这一段里没有一个字符是被转义过的（字段名是硬编码的），所以它"看着对"；但只要
 * 值是**运行时来的字符串**，同一句话就会漏掉转义而产出**能被注入的 JSON**。
 * 本模块要解决的就是这件事：把转义、逗号、括号配对这三件容易写错的事交给库，
 * 使用者只管按顺序描述结构。
 *
 * 形状：流式直写，没有树
 * ----------------------
 * `uvcpp_json_writer` 是一个**状态机**：它按调用顺序把字节直接写进缓冲，不建
 * 任何中间表示。产出就在调用方给的缓冲里（没给就用它自带的那个）。
 *
 *     std::string body;
 *     uvcpp_json_writer w(body);
 *     w.object_begin();
 *     w.key("size");
 *     w.value(static_cast<long long>(f->size()));
 *     w.member("name", user_name);             // key + value 一步；转义由库做
 *     w.key("ids");
 *     w.array_begin();
 *     for (...) w.value(id);
 *     w.array_end();
 *     w.object_end();
 *     if (w.finish() == json_write_status::OK) resp.json_str(body);
 *
 * 刻意**不给链式**（`.key("a").value(1)`）：那要求每个方法返回 `*this`，错误
 * 就只能靠"回头再查 `status()`"，而漏查一次就变成静默的坏产出。返回值就是
 * 状态，是这一层的取舍。
 *
 * 自动的三件事：**逗号**（元素之间）、**转义**（键与字符串值）、**括号配对**
 * （`object_end()` 会核对栈顶是不是对象）。使用者写不出"多余逗号"这种错。
 *
 * 失败是**粘性**的，而且失败之后缓冲里的内容**不再保证是合法 JSON**
 * ------------------------------------------------------------------
 * 所有方法都返回 `json_write_status`，并且**不抛异常**（本仓的铁律：异常不得
 * 穿透 libuv 的回调）。第一次失败之后，后续所有调用都直接返回那个错误、不再
 * 动缓冲 —— 这样错误路径的代码不必层层检查。
 *
 * 但要注意：**一旦返回非 OK，`str()` 里的内容就可能是个半截 JSON**（比如已经
 * 写出去一个逗号，才发现这个位置不该有值）。这是个流式构造器的固有代价：
 * 要么先建树（那正是本模块要避开的），要么允许失败时丢弃缓冲。**调用方在
 * 非 OK 时应当丢弃产出**，而不是把它发出去。`finish()` 是收尾检查，它的
 * `UNCLOSED`（还有容器没关）与 `MISUSE`（一个值都没写过）就是在提醒这件事。
 *
 * 转义决策（这一张表是**契约**，不是实现细节）
 * --------------------------------------------------------------------
 * | 输入字节 | 产出 | 说明 |
 * |---|---|---|
 * | `"` | `\"` | |
 * | `\` | `\\` | |
 * | 0x08 0x09 0x0A 0x0C 0x0D | `\b` `\t` `\n` `\f` `\r` | 五个短形态 |
 * | 其余 0x00–0x1F | `\u00XX` | **小写**十六进制 |
 * | `/` | **原样** | 见下 |
 * | 0x20 及以上（含 UTF-8 多字节） | 原样逐字节 | |
 *
 * **不转义 `/` 是与 hical 的有意分歧**（hical 在 `CompileTimeJson.h:51` 把 `/`
 * 转成 `\/`）。JSON 规范（RFC 8259 §7）**不要求**转义 `/`，转它纯粹是为了
 * HTML 内嵌 JSON 的 `</script>` 那类场景 —— 而本模块的定位是 **HTTP 响应体**
 * （`application/json` 由 JSON 解析器消费，不经 HTML 解析器），在这个上下文里
 * 多转义一个 `/` 只是把 URL、日期、正则里的斜杠统统改写一遍，付出体积和可读性
 * 的代价去防一个此处不存在的风险。真在 HTML 里内嵌 JSON 的使用者应当在**嵌入点**
 * 处理（例如把 `<` 写成 `<`）—— 那需要"这段 JSON 要嵌到哪"的上下文，而
 * 本模块拿不到，所以它不假装能防。
 *
 * **不校验 UTF-8**（诚实边界）：本模块逐字节搬运，不解码也不校验。传进来的是
 * 合法 UTF-8，产出就是合法 JSON；传进来的是非法字节序列，产出**也是非法的**
 * —— 这一点与 nlohmann 不同（它 `dump()` 遇到非法 UTF-8 会抛 `type_error.316`，
 * 本模块**不抛**，所以它只能原样输出）。要校验请在上游做。
 *
 * 与 nlohmann 那侧的关系
 * ----------------------
 * 两边**共存**，各管一段：**本模块产出**（核心模块、永远在编、零依赖），
 * **nlohmann 那侧消费**（解析、DOM 操作、webapp 模块、默认关）。产出接在
 * 响应上的路子就是 `resp.json_str(w.str())` —— 本层不参与 webapp 的编译开关。
 *
 * 线程
 * ----
 * 一个 writer **只属于一个线程**，不是在写中间共享的。跨线程共享要让调用方自己
 * 加锁，本模块不提供任何同步。
 */
#pragma once
#ifndef SRC_UVCPP_UVCPP_JSON_WRITER_H
#define SRC_UVCPP_UVCPP_JSON_WRITER_H

#include <cstddef>
#include <string>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

/**
 * @brief `uvcpp_json_writer` 的返回码。
 *
 * 名前缀是 `json_write_` 而不是 `json_`：解析侧那个 `json_status`
 * （`webapp/uvcpp_web_json.h`）长在 webapp 模块上，本模块是核心模块，**不能**
 * 依赖它（依赖方向反了，而且 webapp 默认关掉）。
 */
enum class json_write_status : int {
  OK = 0,
  MISUSE,     ///< 调用次序不合法（值没配键、键没配值、容器不配对、根值写了两次…）
  TOO_DEEP,   ///< 嵌套超过 `max_depth`
  TOO_LARGE,  ///< 产出超过 `max_bytes`
  UNCLOSED,   ///< `finish()` 时还有容器没关
  NO_MEMORY,  ///< 缓冲扩容失败（`std::bad_alloc`；其它异常也一并归到这里）
};

/** @brief 状态的可读名称，可直接写日志。 */
UVCPP_API const char* json_write_status_name(json_write_status status);

/** @brief 构造器的可调限制。 */
struct UVCPP_API json_write_options {
  size_t max_depth;  ///< 最大嵌套层数，默认 64（与解析侧 `json_parse_options` 一致）
  size_t max_bytes;  ///< 产出字节数上限，默认 8 MiB（超出即 `TOO_LARGE`）

  /// 显式构造函数（成员初始化器会破坏聚合初始化，与 `uvcpp_log_record` 同理）
  explicit json_write_options(size_t depth = 64,
                              size_t bytes = 8u * 1024u * 1024u);
};

/**
 * @brief 流式 JSON 构造器（见文件头）。
 *
 * 栈帧是**定长数组**（`k_max_depth` 项），所以构造器**不分配**；唯一的分配来自
 * 产出缓冲的扩容，以及使用者选的外部缓冲。
 */
class UVCPP_API uvcpp_json_writer {
 public:
  /**
   * @brief 嵌套上限的**编译期**上限。
   *
   * 状态栈是定长数组（不分配是这个类的设计前提），所以 `json_write_options`
   * 里的 `max_depth` 会被夹到 `[0, k_max_depth]`。夹的这个动作**没有**返回值
   * 可报，是刻意的：它是"上限比你想要的小"而不是"写不出来了"，把 max_depth
   * 设成 128 的人得到的仍然是一个能用的 64 层构造器。
   */
  static const size_t k_max_depth = 64;

  /** @brief 自带缓冲。 */
  uvcpp_json_writer();
  /** @brief 自带缓冲 + 自定义限制。 */
  explicit uvcpp_json_writer(const json_write_options& opts);

  /**
   * @brief **直写调用方的缓冲**（本模块的主要用法）。
   *
   * 产出**追加**到 `out` 现有的内容之后 —— 不读、不清空、不要求它是空的。
   * `written()` 数的是**本 writer 自己写进去的字节数**（不是 `out.size()`），
   * `max_bytes` 比的也是它。`clear()` 会把 `out` 截回构造时的长度。
   *
   * `out` 必须比本对象活得久（本类不可拷贝、不可移动，所以这个前提不容易被
   * 意外破坏）。
   */
  explicit uvcpp_json_writer(std::string& out);
  /** @brief 直写调用方缓冲 + 自定义限制。 */
  uvcpp_json_writer(std::string& out, const json_write_options& opts);

  ~uvcpp_json_writer();

  uvcpp_json_writer(const uvcpp_json_writer&) = delete;
  uvcpp_json_writer& operator=(const uvcpp_json_writer&) = delete;

  // -------------------------------------------------------------------
  // 结构
  // -------------------------------------------------------------------

  /// @brief `{`，并开始一个对象。只能在"该有值"的位置调用。
  json_write_status object_begin();
  /// @brief `[`，并开始一个数组。
  json_write_status array_begin();
  /// @brief `}`。要求栈顶**就是**对象，且最后一个键已经配上了值。
  json_write_status object_end();
  /// @brief `]`。要求栈顶**就是**数组。
  json_write_status array_end();

  // -------------------------------------------------------------------
  // 键（只能紧跟着出现在对象里，且一个键只配一个值）
  // -------------------------------------------------------------------

  /** @brief NUL 结尾的键。`nullptr` 是 `MISUSE`（键不能是 null）。 */
  json_write_status key(const char* k);
  /** @brief 带长度的键（二进制安全；可以含 NUL 之外的任何字节，含 `"` 会被转义）。 */
  json_write_status key(const char* k, size_t len);
  json_write_status key(const std::string& k);

  // -------------------------------------------------------------------
  // 值
  // -------------------------------------------------------------------

  /// @brief `null`（也是 `value((const char*)nullptr)` 的语义）。
  json_write_status null_value();

  json_write_status value(bool v);

  json_write_status value(short v);
  json_write_status value(unsigned short v);
  json_write_status value(int v);
  json_write_status value(unsigned int v);
  json_write_status value(long v);
  json_write_status value(unsigned long v);
  json_write_status value(long long v);
  json_write_status value(unsigned long long v);

  /** @brief 双精度；`NaN`/`±Inf` 产出 `null`（JSON 没有这三个字面量）。 */
  json_write_status value(double v);
  /** @brief 单精度；同上，且"最短往返"的判据按 `float` 的精度算。 */
  json_write_status value(float v);

  /**
   * @brief 字符串值，**带转义**。
   *
   * 值的**裸**字节是刻意不给的（`value_raw`）—— 那等于把转义的责任又还给调用方，
   * 而转义正是本模块存在的理由。要拼已序列化好的片段，请用 `key`/`value` 重新
   * 描述它，或者在**外面**拼（那样拼接点是你自己的责任）。
   *
   * `value((const char*)nullptr)` 产出 `null`（与 `null_value()` 同义）。
   */
  json_write_status value(const char* s);
  json_write_status value(const char* s, size_t len);
  json_write_status value(const std::string& s);

  // -------------------------------------------------------------------
  // 收尾与查询
  // -------------------------------------------------------------------

  /**
   * @brief 收尾检查：栈已空、且至少写过一个根值。
   *
   * 它**不**补任何字节（该关的容器要你自己关）。它只是把"这份产出到底完不完整"
   * 变成一个有返回值的判断，而不是让调用方自己数括号。
   */
  json_write_status finish();

  /// @brief 第一个出错的返回码（粘性）；没出过错就是 `OK`。
  json_write_status status() const;
  /// @brief `status() == OK`。
  bool ok() const;

  /// @brief 当前嵌套层数（根值之前是 0）。
  size_t depth() const;
  /// @brief **本 writer 自己**写进去的字节数（外部缓冲模式下不含缓冲原有内容）。
  size_t written() const;
  /// @brief 预留产出空间（追加模式；`out` 原有内容也在里面）。
  void reserve(size_t bytes);

  /**
   * @brief 回到刚构造完的状态：清空自己写过的那一段、清掉错误、栈清空。
   *
   * 外部缓冲模式下它把缓冲**截回构造时的长度**（不是清空整个串）—— 使用者
   * 把自己的前缀放在缓冲里是合法用法，`clear()` 不该顺手吃掉它。
   */
  void clear();

  /// @brief 产出所在的缓冲（外部缓冲模式下就是调用方那个串，含其原有内容）。
  const std::string& str() const;
  const char* data() const;
  size_t size() const;

  /**
   * @brief 写一个键值对（`key` + `value` 的合并形态）。
   *
   * 模板而不是三个重载：`member("k", 5)` 在 `(const char*, long long)` 与
   * `(const char*, bool)` 之间**是歧义的**（`int` 到这两个的转换同级），而
   * `member("k", true)` 若只留整数版本又会把 JSON 的 `true` 写成 `1`。模板把
   * 参数类型原样交给 `value()` 那套重载，就不会有中间那次"帮倒忙"的转换。
   */
  template <typename T>
  json_write_status member(const char* k, const T& v) {
    json_write_status rc = key(k);
    if (rc != json_write_status::OK) return rc;
    return value(v);
  }

 private:
  /// 一个容器的状态。
  struct frame {
    bool is_object;  ///< true=对象，false=数组
    bool key_open;   ///< 对象里：键已经写出去、它的值还没写
    size_t count;    ///< 已经写进去几个元素（键值对按"值"计）
    frame();
  };

  json_write_status fail(json_write_status s);
  json_write_status append_raw(const char* p, size_t n);
  json_write_status append_escaped(const char* p, size_t len);
  json_write_status check_value() const;
  json_write_status emit_value();
  json_write_status before_value();
  json_write_status push_frame(bool is_object);
  json_write_status end_frame(bool is_object);
  json_write_status value_signed(long long v);
  json_write_status value_unsigned(unsigned long long v);
  json_write_status value_fp(double v, bool is_float);

  std::string owned_;   ///< 自带缓冲模式下的缓冲
  std::string* out_;    ///< 产出写这里（`&owned_`，或调用方给的那个串）
  size_t base_;         ///< 构造时 `*out_` 已有的长度（`clear()` 截回这里）
  size_t written_;      ///< **本 writer 自己**写进去的字节数
  size_t depth_;
  bool root_done_;
  json_write_status status_;
  size_t max_depth_;
  size_t max_bytes_;
  frame stack_[k_max_depth];
};

}  // namespace uvcpp

#endif  // SRC_UVCPP_UVCPP_JSON_WRITER_H
