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

#include <mutex>
#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_log.h>

namespace uvcpp {

/**
 * @brief 控制台输出的外观选项。
 *
 * 用显式构造函数而不是成员初始化器 —— C++11 下 NSDMI 会破坏聚合初始化，
 * `uvcpp_console_log_options{false, true}` 这种写法会编译不过。
 */
struct uvcpp_console_log_options {
  bool color;           ///< 用 ANSI 颜色区分等级（默认按终端能力自动判断）
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
 *   2026-09-13 12:34:56.789 [INFO ] [REQUEST] GET /index.html  (server.cpp:88)
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

  /** @brief sink 自己的附加下限（在 logger 的过滤之上再收一道）。 */
  void set_min_level(log_level level);
  log_level min_level() const;

  uvcpp_console_log_options& options();
  const uvcpp_console_log_options& options() const;

  /** @brief 当前输出是否真的会上色（取决于终端能力与 NO_COLOR 环境变量）。 */
  bool color_enabled() const;

private:
  mutable std::mutex mutex_;
  uvcpp_console_log_options options_;
  log_level min_level_;
  bool color_supported_;

  uvcpp_console_log_sink(const uvcpp_console_log_sink&) = delete;
  uvcpp_console_log_sink& operator=(const uvcpp_console_log_sink&) = delete;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_LOG_CONSOLE_H
