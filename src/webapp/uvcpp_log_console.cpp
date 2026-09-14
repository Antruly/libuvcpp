/**
 * @file src/webapp/uvcpp_log_console.cpp
 * @brief 内置控制台 sink 的实现。
 */

#include <webapp/uvcpp_log_console.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#if defined(_WIN32)
// 只在本 .cpp 里包含 windows.h —— 它把 `small` 之类的常见词定义成宏
// （rpcndr.h 里有 `#define small char`），放进公开头文件会污染使用者代码。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  define UVCPP_ISATTY(fd) _isatty(fd)
#  define UVCPP_FILENO(f)  _fileno(f)
#else
#  include <unistd.h>
#  define UVCPP_ISATTY(fd) isatty(fd)
#  define UVCPP_FILENO(f)  fileno(f)
#endif

namespace uvcpp {

namespace {

// 各等级的 ANSI 颜色。索引与 log_level 一一对应（含 OFF 占位）。
const char* const k_level_colors[] = {
    "\x1b[90m",  // TRACE  亮黑/灰
    "\x1b[36m",  // DEBUG  青
    "\x1b[32m",  // INFO   绿
    "\x1b[33m",  // WARN   黄
    "\x1b[31m",  // ERROR  红
    "\x1b[1;31m",// FATAL  加粗红
    "\x1b[0m",   // OFF    占位
};
const char* const k_color_reset = "\x1b[0m";

const size_t k_level_color_count =
    sizeof(k_level_colors) / sizeof(k_level_colors[0]);

const char* level_color(log_level level) {
  const int idx = static_cast<int>(level);
  if (idx < 0 || static_cast<size_t>(idx) >= k_level_color_count) {
    return k_color_reset;
  }
  return k_level_colors[idx];
}

/**
 * @brief 检查终端是否支持 ANSI 转义序列。
 *
 * Windows 10 之前的 conhost 默认不解释 ANSI，需要显式打开
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING。这里顺手打开；打不开就不上色，
 * 免得日志里全是乱码的转义字符。
 */
bool detect_color_support() {
  // 尊重 NO_COLOR 约定（https://no-color.org/）：只要设了就不上色。
  const char* no_color = std::getenv("NO_COLOR");
  if (no_color != nullptr && no_color[0] != '\0') return false;

  // 重定向到文件/管道时不上色 —— 否则日志文件里会混入转义序列。
  if (!UVCPP_ISATTY(UVCPP_FILENO(stdout))) return false;

#if defined(_WIN32)
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
  DWORD mode = 0;
  if (!GetConsoleMode(h, &mode)) return false;  // 不是控制台
  if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) return true;
  return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
  return true;
#endif
}

/** @brief 把 epoch 毫秒格式化成本地时间 "YYYY-MM-DD HH:MM:SS.mmm"。 */
void format_timestamp(uint64_t epoch_ms, char* out, size_t out_size) {
  const time_t secs = static_cast<time_t>(epoch_ms / 1000);
  const unsigned millis = static_cast<unsigned>(epoch_ms % 1000);

  struct tm tmv;
  std::memset(&tmv, 0, sizeof(tmv));
#if defined(_WIN32)
  if (localtime_s(&tmv, &secs) != 0) {
    std::snprintf(out, out_size, "-----------------------");
    return;
  }
#else
  if (localtime_r(&secs, &tmv) == nullptr) {
    std::snprintf(out, out_size, "-----------------------");
    return;
  }
#endif
  std::snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d.%03u",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec, millis);
}

/** @brief 取文件名的最后一段，避免日志里出现一长串构建路径。 */
const char* basename_of(const char* path) {
  if (path == nullptr) return "";
  const char* last_slash = std::strrchr(path, '/');
  const char* last_bslash = std::strrchr(path, '\\');
  const char* best = path;
  if (last_slash != nullptr && last_slash + 1 > best) best = last_slash + 1;
  if (last_bslash != nullptr && last_bslash + 1 > best) best = last_bslash + 1;
  return best;
}

}  // namespace

uvcpp_console_log_options::uvcpp_console_log_options()
    : color(true),
      show_timestamp(true),
      show_thread(true),
      show_category(true),
      show_location(true),
      split_streams(true) {}

uvcpp_console_log_sink::uvcpp_console_log_sink()
    : mutex_(),
      options_(),
      min_level_(log_level::TRACE),
      color_supported_(detect_color_support()) {}

uvcpp_console_log_sink::uvcpp_console_log_sink(
    const uvcpp_console_log_options& options)
    : mutex_(),
      options_(options),
      min_level_(log_level::TRACE),
      color_supported_(detect_color_support()) {}

uvcpp_console_log_sink::~uvcpp_console_log_sink() {}

bool uvcpp_console_log_sink::should_log(log_level level,
                                        log_category /*category*/) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (min_level_ == log_level::OFF) return false;
  return static_cast<int>(level) >= static_cast<int>(min_level_);
}

void uvcpp_console_log_sink::set_min_level(log_level level) {
  std::lock_guard<std::mutex> lock(mutex_);
  min_level_ = level;
}

log_level uvcpp_console_log_sink::min_level() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return min_level_;
}

uvcpp_console_log_options& uvcpp_console_log_sink::options() {
  return options_;
}

const uvcpp_console_log_options& uvcpp_console_log_sink::options() const {
  return options_;
}

bool uvcpp_console_log_sink::color_enabled() const {
  return color_supported_ && options_.color;
}

void uvcpp_console_log_sink::write(const uvcpp_log_record& record) {
  std::lock_guard<std::mutex> lock(mutex_);

  const bool use_color = color_supported_ && options_.color;

  // 先在本地把整行拼好，再一次性 fwrite —— 分多次 flockfile 输出会让并发
  // 日志互相穿插，而且行缓冲下每次 fputc 都可能触发刷盘。
  std::string line;
  line.reserve(record.message.size() + 96);

  if (options_.show_timestamp) {
    char ts[32];
    format_timestamp(record.timestamp_ms, ts, sizeof(ts));
    line += ts;
    line += ' ';
  }

  if (use_color) line += level_color(record.level);

  // 等级固定宽度 5，让后续列对齐（INFO 补一个空格）。
  line += '[';
  const char* lv = uvcpp_log_level_name(record.level);
  line += lv;
  for (size_t i = std::strlen(lv); i < 5; ++i) line += ' ';
  line += ']';

  if (use_color) line += k_color_reset;

  if (options_.show_category) {
    line += " [";
    line += uvcpp_log_category_name(record.category);
    line += ']';
  }

  if (options_.show_thread) {
    char tid[32];
    std::snprintf(tid, sizeof(tid), " (tid:%llu)",
                  static_cast<unsigned long long>(record.thread_id));
    line += tid;
  }

  line += ' ';
  line += record.message;

  if (options_.show_location && record.file != nullptr) {
    char loc[64];
    std::snprintf(loc, sizeof(loc), "  (%s:%d)", basename_of(record.file),
                  record.line);
    line += loc;
  }

  line += '\n';

  const bool to_stderr =
      options_.split_streams &&
      static_cast<int>(record.level) >= static_cast<int>(log_level::WARN);
  std::FILE* out = to_stderr ? stderr : stdout;

  std::fwrite(line.data(), 1, line.size(), out);
  // WARN 以上立刻刷出去：进程崩溃/被 kill 时最需要看到的正是这些。
  if (to_stderr) std::fflush(out);
}

}  // namespace uvcpp
