/**
 * @file src/webapp/uvcpp_log.h
 * @brief Web 应用框架的日志虚接口。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 设计要点
 * --------
 * 两层结构：
 *   1. **等级**（log_level）—— 这条日志有多严重：TRACE/DEBUG/INFO/WARN/ERROR/FATAL。
 *   2. **模块**（log_category）—— 这条日志来自哪个功能模块：REQUEST/RESPONSE/
 *      HEADER/BODY/ROUTER/...。
 *
 * 「按模块自动分到 info/warn/error」就是这样落地的：**模块决定 category，
 * 事件性质决定 level**。例如 HTTP 请求进来是 `UVCPP_LOG_INFO(REQUEST)`，
 * 报文头不合法是 `UVCPP_LOG_WARN(HEADER)`，body 超过上限是
 * `UVCPP_LOG_ERROR(BODY)`。使用者可以按模块单独设等级
 * （`set_level(log_category::HEADER, log_level::WARN)`），做到「只看某个模块的
 * 警告以上」。
 *
 * 使用方式（流式，不是 printf 变参）：
 * @code
 *   UVCPP_LOG_INFO(log_category::ROUTER) << "matched " << path << " -> " << name;
 * @endcode
 *
 * 之所以不用 `printf` 风格的可变参数宏：`__VA_OPT__` 是 C++20，而
 * `##__VA_ARGS__` 只是 MSVC/GNU 扩展。流式宏在 C++11 下可移植，而且被过滤掉
 * 时**零开销** —— 连消息拼装都不会发生（这是相对于「先 sprintf 再判断要不要
 * 输出」的关键差别，在高频路径上很重要）。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_LOG_H
#define SRC_WEBAPP_UVCPP_LOG_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

// =========================================================================
// 等级与模块
// =========================================================================

/**
 * @brief 日志等级。数值越小越详细。
 */
enum class log_level : int {
  TRACE = 0,
  DEBUG = 1,
  INFO  = 2,
  WARN  = 3,
  /**
   * @brief 错误。
   *
   * **名字是 ERR 而不是 ERROR，这是刻意的，不要"顺手改回去"。**
   * Windows 的 `<wingdi.h>`（经 `<windows.h>` → `<uv.h>` 被引进来）里有
   * `#define ERROR 0`。宏展开不看 `enum class` 的作用域，所以写成 `ERROR`
   * 时，任何在 windows.h 之后 include 本头文件的翻译单元都会把它展开成
   * `0`，直接编译失败。spdlog 出于同样的原因也把这个等级命名为 `err`。
   * 对外展示用 `uvcpp_log_level_name()`，它返回的仍然是 "ERROR"。
   */
  ERR   = 4,
  FATAL = 5,
  OFF   = 6,  ///< 只作为「阈值」使用，不会出现在日志记录里
};

/**
 * @brief 日志模块分类。
 *
 * 新增模块时只需在 CATEGORY_COUNT 之前插入，`uvcpp_log_category_name()` 与
 * 等级表会自动跟上（两者都以 CATEGORY_COUNT 为数组长度）。
 */
enum class log_category : int {
  CORE = 0,   ///< 框架自身（生命周期、配置）
  HTTP,       ///< HTTP 协议层
  REQUEST,    ///< 请求
  RESPONSE,   ///< 响应
  HEADER,     ///< 报头
  BODY,       ///< 报文主体
  ROUTER,     ///< 路由
  STATIC,     ///< 静态文件服务
  WEBSOCKET,  ///< WebSocket
  SSL,        ///< TLS/证书
  UPLOAD,     ///< 上传
  DOWNLOAD,   ///< 下载
  IO,         ///< 文件/线程池相关 IO
  RAW,        ///< 原始 TCP 数据钩子
  JSON,       ///< JSON 解析与序列化
  CATEGORY_COUNT,  ///< 哨兵：模块总数
};

/** @brief 等级的可读名称（"INFO" 等），未知值返回 "?"。 */
UVCPP_API const char* uvcpp_log_level_name(log_level level);

/** @brief 模块的可读名称（"REQUEST" 等），未知值返回 "?"。 */
UVCPP_API const char* uvcpp_log_category_name(log_category category);

/** @brief 模块总数，方便按模块遍历配置。 */
UVCPP_API size_t uvcpp_log_category_count();

// =========================================================================
// 日志记录
// =========================================================================

/**
 * @brief 一条待输出的日志。
 *
 * 由 `uvcpp_log_stream` 在析构时填好并交给 sink。sink 实现**不得**长期持有
 * 这个对象的引用 —— 它只在下游 `write()` 调用期间有效。
 */
struct UVCPP_API uvcpp_log_record {
  log_level    level;
  log_category category;
  const char*  file;      ///< 源码文件（可能为 nullptr）
  int          line;
  const char*  function;  ///< 函数名（可能为 nullptr）
  std::string  message;   ///< 已拼装好的消息正文
  uint64_t     timestamp_ms;  ///< Unix 纪元毫秒
  uint64_t     thread_id;     ///< 输出这条日志的线程

  /// 显式构造函数。**不用成员初始化器（NSDMI）** —— 那会让本结构体失去
  /// 聚合初始化资格（C++11 下 `uvcpp_log_record{...}` 会编译失败）。
  uvcpp_log_record();
};

// =========================================================================
// 虚接口：sink
// =========================================================================

/**
 * @brief 日志输出目标虚接口。
 *
 * 使用者实现这个接口并 `uvcpp_logger::instance().set_sink(&my_sink)` 就能把
 * 框架日志接入自己的日志系统（文件、syslog、ELK、自研模块……）。
 *
 * **线程安全要求**：`write()` 可能从 libuv 的工作线程池线程调用，实现必须
 * 自己保证线程安全。logger 会在调用期间持有自己的锁来串行化 sink 调用，
 * 但 sink 也可能被使用者从别处直接调用，所以不要依赖那把锁。
 */
class UVCPP_API uvcpp_log_sink {
public:
  virtual ~uvcpp_log_sink();

  /** @brief 输出一条日志。实现必须线程安全。 */
  virtual void write(const uvcpp_log_record& record) = 0;

  /**
   * @brief sink 自己的附加过滤。
   *
   * logger 已经按等级过滤过一遍了；这里给 sink 一个补一层的机会（例如控制台
   * sink 想比全局更安静）。默认返回 true。
   */
  virtual bool should_log(log_level level, log_category category) const;
};

// =========================================================================
// 流式日志对象
// =========================================================================

/**
 * @brief 由 `UVCPP_LOG_*` 宏构造的临时流对象，析构时交给 sink。
 *
 * 不要自己构造它 —— 用宏。它只需要活在一个完整表达式内。
 */
class UVCPP_API uvcpp_log_stream {
public:
  uvcpp_log_stream(log_level level, log_category category, const char* file,
                   int line, const char* function);
  ~uvcpp_log_stream();

  uvcpp_log_stream& operator<<(const char* s);
  uvcpp_log_stream& operator<<(const std::string& s);
  uvcpp_log_stream& operator<<(char c);
  uvcpp_log_stream& operator<<(bool b);
  uvcpp_log_stream& operator<<(short v);
  uvcpp_log_stream& operator<<(unsigned short v);
  uvcpp_log_stream& operator<<(int v);
  uvcpp_log_stream& operator<<(unsigned int v);
  uvcpp_log_stream& operator<<(long v);
  uvcpp_log_stream& operator<<(unsigned long v);
  uvcpp_log_stream& operator<<(long long v);
  uvcpp_log_stream& operator<<(unsigned long long v);
  uvcpp_log_stream& operator<<(float v);
  uvcpp_log_stream& operator<<(double v);
  uvcpp_log_stream& operator<<(const void* p);

  // 不能被拷贝/移动：它就是个临时的拼装缓冲
  uvcpp_log_stream(const uvcpp_log_stream&) = delete;
  uvcpp_log_stream& operator=(const uvcpp_log_stream&) = delete;

private:
  std::string  buf_;
  log_level    level_;
  log_category category_;
  const char*  file_;
  int          line_;
  const char*  function_;
};

// =========================================================================
// 门面：logger
// =========================================================================

/**
 * @brief 全局日志门面（单例）。
 *
 * 默认 sink 是内置的控制台实现（`uvcpp_console_log_sink`），即「本项目的默认
 * 控制台实例」。不设置任何东西就能直接用。
 */
class UVCPP_API uvcpp_logger {
public:
  static uvcpp_logger& instance();

  /**
   * @brief 替换输出目标。
   * @param sink 新的 sink；传 nullptr 恢复内置控制台 sink。
   *             **不接管所有权** —— 调用方负责让 sink 活得比这块用法久
   *             （通常用静态对象或长生命周期成员）。
   */
  void set_sink(uvcpp_log_sink* sink);
  uvcpp_log_sink* sink() const;

  /** @brief 设置全局等级阈值（低于它的日志全部丢弃）。 */
  void set_level(log_level level);
  /** @brief 只给某个模块设置等级阈值。 */
  void set_level(log_category category, log_level level);
  /** @brief 给所有模块统一设置等级阈值（这会覆盖掉每个模块各自的设置）。 */
  void set_all_category_levels(log_level level);
  /**
   * @brief 清掉所有模块级覆盖，让每个模块重新跟随全局等级。
   *
   * 注意 `set_all_category_levels()` 是「把每个模块都显式设一遍」，设完之后
   * 全局等级就不再影响它们了；想回到「全部跟随全局」要用这个函数。
   */
  void clear_category_overrides();
  /** @brief 读取某个模块当前的等级阈值。 */
  log_level level(log_category category) const;
  /** @brief 读取全局等级阈值。 */
  log_level global_level() const;

  /** @brief 这条日志会不会被输出。用于手动守护昂贵的拼装。 */
  bool is_enabled(log_level level, log_category category) const;

  /** @brief 输出一条记录（线程安全）。一般由 `uvcpp_log_stream` 调用。 */
  void write(const uvcpp_log_record& record);

private:
  uvcpp_logger();
  ~uvcpp_logger();
  uvcpp_logger(const uvcpp_logger&) = delete;
  uvcpp_logger& operator=(const uvcpp_logger&) = delete;

  mutable std::mutex mutex_;
  uvcpp_log_sink*    sink_;          ///< 当前生效的 sink（nullptr = 用内置的）
  /// 内置控制台 sink，首次使用时**延迟创建**。声明为 mutable 是因为
  /// `sink()` / `write()` 是 const 或需要从 const 语境里懒创建它。
  mutable uvcpp_log_sink* default_sink_;
  log_level          global_level_;
  /// 每个模块的阈值。用 int 存而不是 log_level，避免枚举数组的初始化麻烦。
  int                category_levels_[static_cast<int>(log_category::CATEGORY_COUNT)];
};

// =========================================================================
// 宏
// =========================================================================

/** @brief 判断某等级/模块当前是否会输出。 */
#define UVCPP_LOG_ENABLED(level, category) \
  (::uvcpp::uvcpp_logger::instance().is_enabled((level), (category)))

#ifndef UVCPP_LOG_FUNC
#  if defined(_MSC_VER)
#    define UVCPP_LOG_FUNC __FUNCTION__
#  else
#    define UVCPP_LOG_FUNC __func__
#  endif
#endif

/**
 * @brief 构造流式日志对象的底层宏。
 *
 * 用 `for` 而不是 `if` 是为了避开 dangling-else：`if (x) LOG << a;` 展开后如果
 * 使用者后面跟了 `else`，会绑到内层 `if` 上。`for` 没有这个歧义。
 * 条件为假时循环体一次都不执行，且**不构造流对象** —— 零开销。
 */
#define UVCPP_LOG_STREAM(level, category)                                     \
  for (bool uvcpp_log_guard_ = UVCPP_LOG_ENABLED((level), (category));         \
       uvcpp_log_guard_; uvcpp_log_guard_ = false)                            \
    ::uvcpp::uvcpp_log_stream((level), (category), __FILE__, __LINE__,         \
                              UVCPP_LOG_FUNC)

#define UVCPP_LOG_TRACE(category) \
  UVCPP_LOG_STREAM(::uvcpp::log_level::TRACE, (category))
#define UVCPP_LOG_DEBUG(category) \
  UVCPP_LOG_STREAM(::uvcpp::log_level::DEBUG, (category))
#define UVCPP_LOG_INFO(category) \
  UVCPP_LOG_STREAM(::uvcpp::log_level::INFO, (category))
#define UVCPP_LOG_WARN(category) \
  UVCPP_LOG_STREAM(::uvcpp::log_level::WARN, (category))
#define UVCPP_LOG_ERROR(category) \
  UVCPP_LOG_STREAM(::uvcpp::log_level::ERR, (category))
#define UVCPP_LOG_FATAL(category) \
  UVCPP_LOG_STREAM(::uvcpp::log_level::FATAL, (category))

/**
 * @brief printf 风格的格式化日志，供确实需要动态格式串的场合。
 *
 * 注意：即使等级被过滤掉，参数也已经被求值了（这是 printf 风格的固有代价）。
 * 高频路径请用 `UVCPP_LOG_*` 流式宏。
 */
UVCPP_API void uvcpp_logf(log_level level, log_category category,
                          const char* fmt, ...);

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_LOG_H
