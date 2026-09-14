/**
 * @file src/webapp/uvcpp_log.cpp
 * @brief 日志虚接口的实现。
 */

#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_log_console.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#  include <process.h>  // _getpid
#else
#  include <unistd.h>   // getpid
#endif

namespace uvcpp {

// =========================================================================
// 名称表
// =========================================================================

namespace {

const char* const k_level_names[] = {"TRACE", "DEBUG", "INFO",
                                     "WARN",  "ERROR", "FATAL",
                                     "OFF"};
const size_t k_level_count = sizeof(k_level_names) / sizeof(k_level_names[0]);

// 顺序必须与 log_category 一一对应；两者长度都由 CATEGORY_COUNT 保证。
const char* const k_category_names[] = {
    "CORE",  "HTTP",    "REQUEST", "RESPONSE", "HEADER",
    "BODY",  "ROUTER",  "STATIC",  "WEBSOCKET", "SSL",
    "UPLOAD", "DOWNLOAD", "IO",    "RAW",      "JSON",
};

const size_t k_category_count =
    sizeof(k_category_names) / sizeof(k_category_names[0]);

// 编译期自检：枚举与名称表长度必须一致，加模块时忘了改名表会在这里断掉。
static_assert(k_category_count ==
                  static_cast<size_t>(log_category::CATEGORY_COUNT),
              "log_category 与 k_category_names 长度不一致："
              "新增模块时请同步补上名字");

uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

uint64_t current_thread_id() {
  // std::thread::id 是可哈希的、可比较的；转成一个数字只是为了日志好看。
  std::hash<std::thread::id> hasher;
  return static_cast<uint64_t>(hasher(std::this_thread::get_id()));
}

}  // namespace

const char* uvcpp_log_level_name(log_level level) {
  const int idx = static_cast<int>(level);
  if (idx < 0 || static_cast<size_t>(idx) >= k_level_count) return "?";
  return k_level_names[idx];
}

const char* uvcpp_log_category_name(log_category category) {
  const int idx = static_cast<int>(category);
  if (idx < 0 || static_cast<size_t>(idx) >= k_category_count) return "?";
  return k_category_names[idx];
}

size_t uvcpp_log_category_count() { return k_category_count; }

// =========================================================================
// log_record
// =========================================================================

uvcpp_log_record::uvcpp_log_record()
    : level(log_level::INFO),
      category(log_category::CORE),
      file(nullptr),
      line(0),
      function(nullptr),
      message(),
      timestamp_ms(now_ms()),
      thread_id(current_thread_id()) {}

// =========================================================================
// sink 基类
// =========================================================================

uvcpp_log_sink::~uvcpp_log_sink() {}

bool uvcpp_log_sink::should_log(log_level /*level*/,
                                log_category /*category*/) const {
  return true;
}

// =========================================================================
// 流对象
// =========================================================================

uvcpp_log_stream::uvcpp_log_stream(log_level level, log_category category,
                                   const char* file, int line,
                                   const char* function)
    : buf_(),
      level_(level),
      category_(category),
      file_(file),
      line_(line),
      function_(function) {
  // 预留一点空间，避免最常见的短消息走一次重新分配。
  buf_.reserve(128);
}

uvcpp_log_stream::~uvcpp_log_stream() {
  // 析构里绝不能抛异常（否则 std::terminate）。logger::write 自己吞异常，
  // 这里再加一层防线。
  try {
    uvcpp_log_record record;
    record.level        = level_;
    record.category     = category_;
    record.file         = file_;
    record.line         = line_;
    record.function     = function_;
    record.message      = buf_;
    uvcpp_logger::instance().write(record);
  } catch (...) {
    // 日志失败绝不能影响业务
  }
}

namespace {

// 数值转字符串：不用 std::ostringstream（比 snprintf 慢一个数量级，而日志
// 可能出现在每请求路径上）。
template <typename T>
void append_integer(std::string& out, T value, const char* fmt) {
  char tmp[32];
  const int n = std::snprintf(tmp, sizeof(tmp), fmt, value);
  if (n > 0) out.append(tmp, static_cast<size_t>(n));
}

}  // namespace

uvcpp_log_stream& uvcpp_log_stream::operator<<(const char* s) {
  if (s != nullptr) buf_ += s;
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(const std::string& s) {
  buf_ += s;
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(char c) {
  buf_ += c;
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(bool b) {
  buf_ += (b ? "true" : "false");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(short v) {
  append_integer(buf_, v, "%d");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(unsigned short v) {
  append_integer(buf_, v, "%u");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(int v) {
  append_integer(buf_, v, "%d");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(unsigned int v) {
  append_integer(buf_, v, "%u");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(long v) {
  append_integer(buf_, v, "%ld");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(unsigned long v) {
  append_integer(buf_, v, "%lu");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(long long v) {
  append_integer(buf_, v, "%lld");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(unsigned long long v) {
  append_integer(buf_, v, "%llu");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(float v) {
  append_integer(buf_, static_cast<double>(v), "%g");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(double v) {
  append_integer(buf_, v, "%g");
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(const void* p) {
  // 不用 "%p"：MSVC 输出的是不带 0x 前缀、且补满位宽的大写十六进制
  // （0000000000001234），和 glibc 的 0x1234 不一致，日志格式会随平台变。
  // 统一成 0x + 小写十六进制。
  char tmp[32];
  const int n = std::snprintf(tmp, sizeof(tmp), "0x%llx",
                              static_cast<unsigned long long>(
                                  reinterpret_cast<uintptr_t>(p)));
  if (n > 0) buf_.append(tmp, static_cast<size_t>(n));
  return *this;
}

// =========================================================================
// logger
// =========================================================================

uvcpp_logger& uvcpp_logger::instance() {
  // C++11 起，函数内静态变量的初始化是线程安全的（magic static）。
  static uvcpp_logger inst;
  return inst;
}

uvcpp_logger::uvcpp_logger()
    : mutex_(),
      sink_(nullptr),
      default_sink_(nullptr),
      global_level_(log_level::INFO) {
  for (int i = 0; i < static_cast<int>(log_category::CATEGORY_COUNT); ++i) {
    // -1 表示「跟随全局等级」，这样 set_level(global) 能立刻影响所有模块，
    // 而某个模块一旦被单独设置过，就不再跟随全局。
    category_levels_[i] = -1;
  }
}

uvcpp_logger::~uvcpp_logger() {
  delete default_sink_;
  default_sink_ = nullptr;
  sink_ = nullptr;
}

void uvcpp_logger::set_sink(uvcpp_log_sink* sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_ = sink;  // 允许 nullptr，sink() 会回落到内置控制台
}

uvcpp_log_sink* uvcpp_logger::sink() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sink_ != nullptr) return sink_;
  // 延迟创建内置控制台 sink：这样「只是链接了本库但从不打日志」的程序不会
  // 在启动时就碰控制台。
  if (default_sink_ == nullptr) {
    default_sink_ = new uvcpp_console_log_sink();
  }
  return default_sink_;
}

void uvcpp_logger::set_level(log_level level) {
  std::lock_guard<std::mutex> lock(mutex_);
  global_level_ = level;
}

void uvcpp_logger::set_level(log_category category, log_level level) {
  const int idx = static_cast<int>(category);
  if (idx < 0 || idx >= static_cast<int>(log_category::CATEGORY_COUNT)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  category_levels_[idx] = static_cast<int>(level);
}

void uvcpp_logger::set_all_category_levels(log_level level) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < static_cast<int>(log_category::CATEGORY_COUNT); ++i) {
    category_levels_[i] = static_cast<int>(level);
  }
}

void uvcpp_logger::clear_category_overrides() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < static_cast<int>(log_category::CATEGORY_COUNT); ++i) {
    category_levels_[i] = -1;
  }
}

log_level uvcpp_logger::level(log_category category) const {
  const int idx = static_cast<int>(category);
  if (idx < 0 || idx >= static_cast<int>(log_category::CATEGORY_COUNT)) {
    return global_level_;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (category_levels_[idx] < 0) return global_level_;
  return static_cast<log_level>(category_levels_[idx]);
}

log_level uvcpp_logger::global_level() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return global_level_;
}

bool uvcpp_logger::is_enabled(log_level level, log_category category) const {
  // OFF 作为阈值表示「全关」；同时 OFF 本身不是一个可输出的等级。
  if (level == log_level::OFF) return false;

  log_level threshold = global_level_;
  {
    const int idx = static_cast<int>(category);
    std::lock_guard<std::mutex> lock(mutex_);
    if (idx >= 0 && idx < static_cast<int>(log_category::CATEGORY_COUNT) &&
        category_levels_[idx] >= 0) {
      threshold = static_cast<log_level>(category_levels_[idx]);
    }
  }
  if (threshold == log_level::OFF) return false;
  return static_cast<int>(level) >= static_cast<int>(threshold);
}

void uvcpp_logger::write(const uvcpp_log_record& record) {
  // 拿锁再取 sink：这样 set_sink 不会在 write 进行到一半时换掉目标。
  std::lock_guard<std::mutex> lock(mutex_);
  uvcpp_log_sink* target = sink_;
  if (target == nullptr) {
    if (default_sink_ == nullptr) {
      default_sink_ = new uvcpp_console_log_sink();
    }
    target = default_sink_;
  }
  if (target == nullptr) return;
  if (!target->should_log(record.level, record.category)) return;

  // sink 是使用者代码，不能让它抛异常穿透到业务路径上（尤其不能穿透
  // 析构函数）。
  try {
    target->write(record);
  } catch (...) {
  }
}

// =========================================================================
// printf 风格入口
// =========================================================================

void uvcpp_logf(log_level level, log_category category, const char* fmt, ...) {
  if (fmt == nullptr) return;
  if (!uvcpp_logger::instance().is_enabled(level, category)) return;

  // vsnprintf 返回「若缓冲区足够大本应写入的长度」，所以先探长度再分配，
  // 保证长消息不会被截断。
  va_list ap;
  va_start(ap, fmt);
  va_list ap_copy;
  va_copy(ap_copy, ap);
  const int needed = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);

  if (needed < 0) {
    va_end(ap_copy);
    return;
  }

  // 常见情况走栈缓冲，避免为了短消息做一次堆分配。
  char stack_buf[512];
  const size_t need = static_cast<size_t>(needed) + 1;
  if (need <= sizeof(stack_buf)) {
    std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap_copy);
    va_end(ap_copy);

    uvcpp_log_record record;
    record.level    = level;
    record.category = category;
    record.message.assign(stack_buf, static_cast<size_t>(needed));
    uvcpp_logger::instance().write(record);
    return;
  }

  std::string heap_buf;
  heap_buf.resize(need);
  std::vsnprintf(&heap_buf[0], need, fmt, ap_copy);
  va_end(ap_copy);
  heap_buf.resize(static_cast<size_t>(needed));

  uvcpp_log_record record;
  record.level    = level;
  record.category = category;
  record.message  = heap_buf;
  uvcpp_logger::instance().write(record);
}

}  // namespace uvcpp
