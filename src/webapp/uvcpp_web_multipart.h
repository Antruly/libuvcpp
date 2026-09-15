/**
 * @file src/webapp/uvcpp_web_multipart.h
 * @brief multipart/form-data 的**增量**解析器（RFC 7578 / RFC 2046）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么单独一个模块、而且是**纯**逻辑
 * -----------------------------------
 * 一个流式状态机最难的不是"怎么解析"，而是"跨块时相位怎么保持"—— 边界标记
 * 可能被切成两半、部件头可能跨块、body 里可能出现**近似但不是**边界的字节。
 * 这些 bug 只有在**某个特定的切分偏移**上才暴露，整包喂永远测不出来。
 *
 * 所以本模块**没有 IO、没有 libuv、不碰请求对象**：喂 `const char*, size_t`，
 * 驱动一个 sink 接口。这样它就能被"把同一份报文在每一个字节偏移处切成两块
 * 喂进去、产出必须完全相同"这种测试直接钉死 —— 那是本模块最有价值的一条
 * 用例，也是唯一能系统性覆盖跨块 bug 的办法。
 *
 * 内存上界（这是"流式"两个字的意义）
 * --------------------------------
 * - 保留缓冲有常数上界，见 `uvcpp_web_multipart_max_retain()`：它与**喂进来
 *   多少数据无关**，只随 boundary 长度线性变化（70 字节的 boundary 也就
 *   约 330 字节）。注意它**不是** `boundary.size() + 3`：那只是"没命中"那
 *   一条路径的尾巴；边界行判到一半（还差一个字节）时，缓冲里还要留着
 *   已扫过的填充，所以真正的上界由那个函数给。
 * - 部件头缓冲 ≤ `max_part_header_bytes`
 * - **部件 body 一个字节都不攒** —— 边收边交给 sink
 *
 * 边界匹配（本模块的核心）
 * ----------------------
 * 用两招把它做成线性时间、常数级内存：
 *
 * 1. **虚拟前置 CRLF**：内部保留缓冲初始化为 `"\r\n"`，于是**首边界和后续
 *    边界形状统一**（都找 `\r\n--<boundary>`），不必为首边界特判。
 *    这也顺带解决了"边界前的那个 CRLF 属于分隔符、不属于 body"。
 * 2. **保留尾部**：设 `delim = "\r\n--" + boundary`（长度 `d`）。缓冲区里
 *    找不到 `delim` 时，任何命中都只能从 `len - d + 1` 起 —— 所以
 *    `[0, len - d + 1)` 必然是 body，可以安全交付；留下的尾巴最多
 *    `d - 1 = boundary.size() + 3` 字节。
 *
 * 命中之后**不能只交付"命中点之前的 body"就完事**：还要看整行才能定性，
 * 而那一行可能还没收全。这种"判不出来"的情形必须把已交付的前缀从保留缓冲里
 * 去掉再返回 —— 扫描游标是每次 `feed()` 的局部量，留着这段缓冲的话下一块
 * 数据进来会把它**再交付一遍**（重复的 body 字节）。这个 bug 只在切点正好
 * 落在边界行中间时出现，整包喂永远碰不到。
 *
 * 命中之后还要看**整行**才能判定（RFC 2046 §5.1.1 的语法）：
 *
 * ```
 * delimiter       := CRLF "--" boundary [LWSP] CRLF
 * close-delimiter := CRLF "--" boundary [LWSP] "--" [LWSP] CRLF
 * ```
 *
 * 即 `\r\n` ⇒ 后面还有部件；`--` 后面跟着 CR/LF ⇒ 部件序列结束。
 * **别的字节说明这不是真边界**（是 body 里的近似串），此时只能把第一个字节当
 * body 交付、再往后扫。这一步不能省 —— 少了它，body 里出现 `\r\n--<b>x`
 * 或 `\r\n--<b>--x` 就会把报文**提前截断**，而"提前截断"比"解析失败"更坏：
 * 它看起来是成功的，后面的部件却全都不见了。
 *
 * 两个刻意的取舍，都写在这里免得被当成 bug 改掉：
 * - **`[LWSP]` 有上限**（128 字节），超了按 400 失败。流式解析不能为一行空白
 *   无界地留缓冲；而"超过上限就当 body"会让本端与接受该填充的对端对"部件从
 *   哪儿切"产生分歧 —— 那正是请求走私的形状，宁可明确失败。
 * - **终边界后面的那个 CRLF 可以不发**。缓冲正好停在 `\r\n--<b>--` 上时解析器
 *   判不出来（还差一个字节），由 `finish()` 兜底按正常结束处理。客户端合法地
 *   可以省略它，少了这一支会把一份好报文判成"被截断"。
 *
 * 行尾的明确表态（**不能"碰巧能跑"**）
 * ----------------------------------
 * - **只认 CRLF。** 裸 LF **不接受**为边界或部件头的一部分。
 *   理由不是"RFC 这么写"，而是安全：接受两种行尾的实现，和只接受一种的
 *   中间件/对端，会**对同一段字节得出不同的部件划分** —— 那正是请求走私
 *   （HTTP request smuggling）那一类问题的构造基础。宁可严格。
 *   部件头里出现裸 LF 直接判 400。
 * - `Content-Disposition` 必须是 `form-data`，缺失 `name=` 判 400。
 *
 * 本模块**不做**文件名清洗
 * ----------------------
 * `uvcpp_web_part_info::filename` 是**解码后的原始值**（`filename*` 优先于
 * `filename`，见下），**没有**做过任何清洗。清洗是策略、不是协议，由上传层
 * 调 `web_sanitize_filename()` 做。这样分层的好处是：解析器可以脱离"文件名
 * 安全策略"被单独测试，而安全策略也只需要在一个地方被审。
 *
 * @warning `filename` **永远不能**用来拼路径。落盘名由框架自己生成。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_MULTIPART_H
#define SRC_WEBAPP_UVCPP_WEB_MULTIPART_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

// =========================================================================
// 部件描述
// =========================================================================

/** @brief 一个部件的元数据（在部件 body 开始**之前**交给 sink）。 */
struct UVCPP_API uvcpp_web_part_info {
  /** @brief 字段名（`Content-Disposition: form-data; name="..."`）。 */
  std::string name;

  /**
   * @brief 客户端给的原始文件名（可空）。
   *
   * **解码后、未清洗。** `filename*`（RFC 5987）优先于 `filename`
   * （RFC 6266 §4.3）。普通 `filename=` **不做百分号解码**（RFC 7578 §4.2
   * 明确要求），只有 `filename*` 才解码。
   */
  std::string filename;

  /** @brief 该部件的 `Content-Type` 原始值（可空）。 */
  std::string content_type;

  /** @brief 是否文件部件：有 `filename=` / `filename*=` 即为真。 */
  bool is_file;

  uvcpp_web_part_info() : is_file(false) {}
};

// =========================================================================
// sink：解析器唯一的对外出口
// =========================================================================

/**
 * @brief 解析结果的消费者。
 *
 * 三个回调都**只从 `feed()` 的调用栈里**被调，所以 sink 不需要考虑重入。
 */
class UVCPP_API uvcpp_web_multipart_sink {
 public:
  virtual ~uvcpp_web_multipart_sink() {}

  /**
   * @brief 部件头解析完、body 开始之前。
   * @return false = 中止解析（sink 已自行处理，比如回了 413）。
   */
  virtual bool on_part_begin(const uvcpp_web_part_info& info) = 0;

  /** @return false = 中止解析。 */
  virtual bool on_part_data(const char* data, size_t len) = 0;

  /**
   * @brief 当前部件结束。
   *
   * @param truncated 该部件是否因 `max_file_size` / `max_field_size` 被截断
   *                  （截断只发生在显式配了上限的时候；默认 0 = 不限）。
   *
   * 为什么把 truncated 放在参数里而不是让 sink 反查解析器：截断这件事是
   * **部件级**的事实，落在部件级回调上最自然；反查会把 sink 和解析器绑成
   * 双向依赖，而这个接口的全部意义就是让两者可以分开测。
   */
  virtual void on_part_end(bool truncated) = 0;
};

// =========================================================================
// 状态与结果
// =========================================================================

enum class uvcpp_web_multipart_state {
  PREAMBLE,        ///< 首个边界之前（那段字节按 RFC 属于前导，丢弃）
  PART_HEADERS,    ///< 正在攒部件头
  PART_BODY,       ///< 正在交付部件 body
  PART_BODY_SKIP,  ///< 该部件已超限：**继续扫边界**但不再交付
  EPILOGUE,        ///< 已过终边界，后续字节全部丢弃
  ERROR_STATE      ///< 终态：之后喂什么都不再产出
};

enum class uvcpp_web_multipart_result {
  OK,                        ///< 正常（还没结束，或刚吃完一块还没到终边界）
  DONE,                      ///< 已到达终边界
  ERROR_NO_BOUNDARY,         ///< 配置期：boundary 为空或超过 RFC 2046 的 70 字节
  ERROR_BOUNDARY_NEVER_FOUND,///< finish() 时还没见过终边界
  ERROR_HEADER_TOO_LONG,     ///< 部件头超过 max_part_header_bytes
  ERROR_BAD_HEADER,          ///< 部件头畸形（含裸 LF、行内没有冒号等）
  ERROR_NOT_FORM_DATA,       ///< Content-Disposition 缺失或不是 form-data
  ERROR_MISSING_NAME,        ///< Content-Disposition 里没有 name=
  ERROR_TOO_MANY_FILES,      ///< 文件部件数超过 max_file_count
  ERROR_TOO_MANY_FIELDS,     ///< 字段部件数超过 max_field_count
  ERROR_FIELD_TOO_LARGE,     ///< 字段部件超过 max_field_size（字段不截断，直接拒）
  ERROR_SINK_ABORTED         ///< sink 的某个回调返回了 false
};

/** @brief 结果的稳定英文名（供日志与用例断言用，不要拿来给用户看）。 */
UVCPP_API const char* uvcpp_web_multipart_result_name(
    uvcpp_web_multipart_result r);

/**
 * @brief 保留缓冲的**上界**（字节）—— 只与 boundary 长度有关，与喂进来多少
 *        数据**无关**。"流式"这两个字的全部意义就在这个数上。
 *
 * 两部分之中的较大者：
 *  - **未命中**时留下的那半个标记：`boundary_len + 3`；
 *  - 边界行**还没判完**时留在缓冲里的：`delim` + 至多两段 `[LWSP]` 填充
 *    + 至多两个待判字节。
 *
 * 导出成一个函数、而不是只在注释里写一句，是为了让它同时被**实现**和
 * **用例**引用 —— 两边各写一份必然会漂移，而"内存不随输入增长"正是本模块
 * 最该守住的那条不变式。
 */
UVCPP_API size_t uvcpp_web_multipart_max_retain(size_t boundary_len);

// =========================================================================
// 解析器
// =========================================================================

class UVCPP_API uvcpp_web_multipart {
 public:
  uvcpp_web_multipart();
  ~uvcpp_web_multipart();

  // 不可拷贝：内含保留缓冲与 sink 指针，拷贝没有意义。
  uvcpp_web_multipart(const uvcpp_web_multipart&) = delete;
  uvcpp_web_multipart& operator=(const uvcpp_web_multipart&) = delete;

  // -----------------------------------------------------------------------
  // 配置（必须在第一次 feed() 之前做完）
  // -----------------------------------------------------------------------

  void set_sink(uvcpp_web_multipart_sink* sink);

  /**
   * @brief 设置 boundary（**不带**前导 `--`，就是 `Content-Type` 参数里的值）。
   * @return false = 不合法（空，或长度 > 70 —— RFC 2046 的硬上限）。
   *
   * 超长/为空的 boundary 必须**拒绝**而不是截断：截断会让本端与对端对
   * "边界是什么"产生不同理解，那正是切错报文的起点。
   */
  bool set_boundary(const std::string& boundary);

  void set_max_part_header_bytes(size_t n);
  void set_max_file_count(size_t n);
  void set_max_field_count(size_t n);
  /** @brief 0 = 不限。超限则**截断**该部件并置 truncated。 */
  void set_max_file_size(uint64_t n);
  /** @brief 0 = 不限。超限则**拒绝**（字段不截断 —— 半截的字段值是错的）。 */
  void set_max_field_size(uint64_t n);

  // -----------------------------------------------------------------------
  // 驱动
  // -----------------------------------------------------------------------

  /**
   * @brief 喂一块 body。
   *
   * 到达终边界之后（或进入 ERROR_STATE 之后）再喂是**无害的空操作**，
   * 返回值保持不变 —— 调用方不必在每次 feed 之前查状态。
   */
  uvcpp_web_multipart_result feed(const char* data, size_t len);

  /**
   * @brief body 收完了。
   *
   * @return `DONE`（见过终边界）；否则 `ERROR_BOUNDARY_NEVER_FOUND`
   *         （报文中途截断 —— 这是**必须**判出来的，不然一个被掐断的上传
   *         会被当成"正常结束、只是文件小"，而半截文件已经落盘了）。
   */
  uvcpp_web_multipart_result finish();

  // -----------------------------------------------------------------------
  // 读状态
  // -----------------------------------------------------------------------

  const std::string& boundary() const { return boundary_; }
  uvcpp_web_multipart_state state() const { return state_; }
  uvcpp_web_multipart_result result() const { return result_; }
  uint64_t received() const { return received_; }
  /** @brief 当前正在处理的部件序号（从 1 开始；还没有则为 0）。 */
  size_t part_index() const { return part_index_; }
  size_t file_count() const { return file_count_; }
  size_t field_count() const { return field_count_; }
  /** @brief 已交付给 sink 的 body 字节数（不含部件头/边界）。 */
  uint64_t body_bytes() const { return body_bytes_; }
  /** @brief 保留缓冲当前占用（用例用它钉"内存上界"这个不变式）。 */
  size_t retained_bytes() const { return retain_.size(); }
  bool current_truncated() const { return truncated_; }
  /** @brief 人类可读的错误说明（含出错的部件序号）。 */
  const std::string& error_text() const { return error_text_; }

 private:
  std::string boundary_;
  std::string delim_;
  std::string retain_;
  std::string header_block_;
  std::string error_text_;

  uvcpp_web_part_info part_;
  uvcpp_web_multipart_sink* sink_;

  uvcpp_web_multipart_state state_;
  uvcpp_web_multipart_result result_;

  uint64_t received_;
  uint64_t body_bytes_;
  uint64_t part_bytes_;
  size_t header_scan_from_;  ///< 部件头扫描游标（避免一次一字节喂时 O(头长²)）
  uint64_t max_file_size_;
  uint64_t max_field_size_;
  size_t max_part_header_bytes_;
  size_t max_file_count_;
  size_t max_field_count_;
  size_t part_index_;
  size_t file_count_;
  size_t field_count_;
  bool truncated_;

  // 内部步骤
  void fail(uvcpp_web_multipart_result r, const std::string& detail);
  bool deliver(const char* data, size_t len);
  bool finish_part();
  bool begin_part_from_block();
  void scan();
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_MULTIPART_H
