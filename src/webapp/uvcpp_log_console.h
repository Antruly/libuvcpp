/**
 * @file src/webapp/uvcpp_log_console.h
 * @brief 内置的控制台日志输出实例。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这是框架的默认 sink —— 不配置任何东西时日志就打到控制台。
 * 想换成自己的日志系统，实现 `uvcpp_log_sink` 再
 * `uvcpp_logger::instance().set_sink(&my_sink)` 即可。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_LOG_CONSOLE_H
#define SRC_WEBAPP_UVCPP_LOG_CONSOLE_H

#include <atomic>
#include <mutex>
#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_log.h>

namespace uvcpp {

/**
 * @brief 控制台输出的外观选项。
 *
 * 用显式构造函数而不是聚合初始化：C++11 下**只要有用户声明的构造函数，这个类型就
 * 不再是聚合体**，`uvcpp_console_log_options{false, true}` 这种写法编译不过。
 */
struct UVCPP_API uvcpp_console_log_options {
  bool color;           ///< 用 ANSI 颜色区分等级。**本字段默认 `true`**；最终上不上色
                        ///< 还要看终端能力，两个都为真才上色 —— 而终端能力只在
                        ///< **sink 构造时**探测一次，中途换终端不会重新判断。
                        ///< 探测是**分两条流**做的（见 `color_enabled(bool)`）：
                        ///< WARN 以上走 stderr，而 `./app > out.log` 这种重定向
                        ///< 只让 stdout 失去终端能力，stderr 还在屏幕上
  bool show_timestamp;  ///< 输出时间戳（含毫秒）
  bool show_thread;     ///< 输出线程 id
  bool show_category;   ///< 输出模块标签，如 [REQUEST]
  bool show_location;   ///< 输出 文件:行号
  bool split_streams;   ///< WARN 及以上走 stderr，其余走 stdout

  uvcpp_console_log_options();
};

/**
 * @brief 把日志写到 stdout/stderr 的控制台 sink。
 *
 * 格式：
 * @code
 *   2026-09-13 12:34:56.789 [INFO ] [REQUEST] (tid:14028) GET /index.html  (server.cpp:88)
 * @endcode
 *
 * 线程安全：`write()` 内部持锁，来自工作线程池的日志不会互相穿插。
 */
class UVCPP_API uvcpp_console_log_sink : public uvcpp_log_sink {
public:
  uvcpp_console_log_sink();
  explicit uvcpp_console_log_sink(const uvcpp_console_log_options& options);
  ~uvcpp_console_log_sink() override;

  void write(const uvcpp_log_record& record) override;
  bool should_log(log_level level, log_category category) const override;
  /// @brief `fflush(stdout) + fflush(stderr)`。stdout 重定向到文件时是块缓冲的，
  /// 进程崩溃或被 kill 会丢掉最后一段日志（见 `uvcpp_log_sink::flush()`）。
  void flush() override;

  /** @brief sink 自己的附加下限（在 logger 的过滤之上再收一道）。 */
  void set_min_level(log_level level);
  log_level min_level() const;

  uvcpp_console_log_options& options();
  const uvcpp_console_log_options& options() const;

  /** @brief **stdout** 那条流当前是否真的会上色。等价于 `color_enabled(false)`。 */
  bool color_enabled() const;

  /**
   * @brief 指定那条流当前是否真的会上色。
   * @param to_stderr true 问 stderr（WARN 及以上的去处），false 问 stdout。
   *
   * 两条流分别探测，因为它们的终端能力可以不同：`./server > access.log` 只
   * 让 stdout 变成文件，stderr 还在终端上，此时 INFO 不该上色而 WARN 该上。
   */
  bool color_enabled(bool to_stderr) const;

private:
  /// @brief 第 0 条 = stdout，第 1 条 = stderr。
  bool color_supported_for(bool to_stderr) const {
    return color_supported_[to_stderr ? 1 : 0];
  }

  mutable std::mutex mutex_;
  uvcpp_console_log_options options_;
  /// 原子：`should_log()` 在每个被放行的记录上都要读它，而紧随其后的
  /// `write()` 又要拿同一把互斥量 —— 让读侧免锁，少一次抢锁。
  std::atomic<int> min_level_;
  bool color_supported_[2];

  uvcpp_console_log_sink(const uvcpp_console_log_sink&) = delete;
  uvcpp_console_log_sink& operator=(const uvcpp_console_log_sink&) = delete;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_LOG_CONSOLE_H
