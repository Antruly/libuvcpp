/**
 * @file src/webapp/uvcpp_log.cpp
 * @brief 日志虚接口的实现。
 */

#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_log_console.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <utility>

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
    "SOAP",  "WSDL",
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

// 默认什么都不做：无缓冲的 sink（直接写 syslog/网络）没有什么可刷的。
// 只有攒了缓冲的实现才需要覆盖它。
void uvcpp_log_sink::flush() {}

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
  // 这里**刻意不 reserve**：那会让每条日志（哪怕只有 "ok" 两个字）先付一次
  // 堆分配，而短消息本来就该落在 std::string 的 SSO 里。长消息靠 std::string
  // 自己的几何增长，多几次扩容只发生在真的写长了的时候。
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
    record.message      = std::move(buf_);  // *this 正在析构，buf_ 不会再被读
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

// 浮点用**最短往返表示**，不用 `%g`：`%g` 只有 6 位有效数字，`0.1 + 0.2` 会被
// 印成 `0.3`，日志里就分不出「本来就是 0.3」和「算错了」。这里从 6 位有效数字
// 往上试，取第一个能原样读回同一 bit 的精度 —— `3.5` 仍是 `3.5`、`0.1` 仍是
// `0.1`，而真需要 15 位的值不被截断。NaN 与任何值都不相等（含它自己），所以
// 循环走到最大精度打印 `nan`/`inf`，有界。
//
// double 与 float 必须分开：把 float 提升成 double 再按 double 往返，`0.1f`
// 会印成 `0.10000000149011612` —— 那是 float→double 的精确值，只是不是 `0.1`
// 的最短 double 表示，反而比 `%g` 更难看。
void append_double(std::string& out, double value) {
  char tmp[40];
  int n = 0;
  for (int digits = 6; digits <= 17; ++digits) {  // 17 对 double 永远够
    n = std::snprintf(tmp, sizeof(tmp), "%.*g", digits, value);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmp)) break;
    if (std::strtod(tmp, nullptr) == value) break;
  }
  // 溢出时**也得追加点东西**（`snprintf` 已在 sizeof-1 处截断并补了 '\0'）：
  // 写成"只在没溢出时才追加"的话，那个数会从日志里**凭空消失** —— 一条
  // 少了字段的日志比一条被截断的难查得多。`%.17g` 最坏约 25 字符，40 够用，
  // 但"够用所以不会发生"不是判据。
  if (n <= 0) return;
  const size_t len = static_cast<size_t>(n) < sizeof(tmp)
                         ? static_cast<size_t>(n)
                         : sizeof(tmp) - 1;
  out.append(tmp, len);
}

void append_float(std::string& out, float value) {
  char tmp[40];
  int n = 0;
  for (int digits = 6; digits <= 9; ++digits) {  // 9 对 float 永远够
    n = std::snprintf(tmp, sizeof(tmp), "%.*g", digits,
                      static_cast<double>(value));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmp)) break;
    if (std::strtof(tmp, nullptr) == value) break;
  }
  // 同 `append_double()`：溢出也要追加，不能静默丢字段。
  if (n <= 0) return;
  const size_t len = static_cast<size_t>(n) < sizeof(tmp)
                         ? static_cast<size_t>(n)
                         : sizeof(tmp) - 1;
  out.append(tmp, len);
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
  append_float(buf_, v);
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(double v) {
  append_double(buf_, v);
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

uvcpp_log_stream& uvcpp_log_stream::operator<<(std::nullptr_t) {
  buf_ += "null";
  return *this;
}

// 等级/模块输出名字而不是数字。这两个是非模板重载，所以在重载解析里压过下面
// 那个枚举模板（精确匹配的非模板优先于同样精确的模板）。
uvcpp_log_stream& uvcpp_log_stream::operator<<(log_level level) {
  buf_ += uvcpp_log_level_name(level);
  return *this;
}

uvcpp_log_stream& uvcpp_log_stream::operator<<(log_category category) {
  buf_ += uvcpp_log_category_name(category);
  return *this;
}

void uvcpp_log_stream::append_enum(long long v) {
  append_integer(buf_, v, "%lld");
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
      global_level_(static_cast<int>(log_level::INFO)) {
  // 原子数组不能在初始化列表里逐个构造，所以在这里填。
  for (int i = 0; i < static_cast<int>(log_category::CATEGORY_COUNT); ++i) {
    // -1 表示「跟随全局等级」，这样 set_level(global) 能立刻影响所有模块，
    // 而某个模块一旦被单独设置过，就不再跟随全局。
    category_levels_[i].store(-1, std::memory_order_relaxed);
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

// 等级读写的内存序一律是 relaxed：这里同步的是「一个 int 的可见性」，不是
// 「哪些内存写在它之前」。打日志的线程不需要因为看到新的阈值就顺带看到别人
// 别的写 —— 它要的只是别读到撕裂的值，而 int 的原子读写本身就保证这一点。
void uvcpp_logger::set_level(log_level level) {
  std::lock_guard<std::mutex> lock(mutex_);
  global_level_.store(static_cast<int>(level), std::memory_order_relaxed);
}

void uvcpp_logger::set_level(log_category category, log_level level) {
  const int idx = static_cast<int>(category);
  if (idx < 0 || idx >= static_cast<int>(log_category::CATEGORY_COUNT)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  category_levels_[idx].store(static_cast<int>(level),
                              std::memory_order_relaxed);
}

void uvcpp_logger::set_all_category_levels(log_level level) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < static_cast<int>(log_category::CATEGORY_COUNT); ++i) {
    category_levels_[i].store(static_cast<int>(level),
                              std::memory_order_relaxed);
  }
}

void uvcpp_logger::clear_category_overrides() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < static_cast<int>(log_category::CATEGORY_COUNT); ++i) {
    category_levels_[i].store(-1, std::memory_order_relaxed);
  }
}

log_level uvcpp_logger::level(log_category category) const {
  const int global = global_level_.load(std::memory_order_relaxed);
  const int idx = static_cast<int>(category);
  if (idx < 0 || idx >= static_cast<int>(log_category::CATEGORY_COUNT)) {
    return static_cast<log_level>(global);
  }
  const int per = category_levels_[idx].load(std::memory_order_relaxed);
  if (per < 0) return static_cast<log_level>(global);
  return static_cast<log_level>(per);
}

log_level uvcpp_logger::global_level() const {
  return static_cast<log_level>(global_level_.load(std::memory_order_relaxed));
}

bool uvcpp_logger::is_enabled(log_level level, log_category category) const {
  // OFF 作为阈值表示「全关」；同时 OFF 本身不是一个可输出的等级。
  if (level == log_level::OFF) return false;

  // 这条路径是**每个被过滤掉的调用点**都要走的，所以它不拿锁：两次 relaxed
  // 原子读就够。早先版本为了读一个 int 去抢全局互斥量，于是「关掉日志」的
  // 代价反而比「打开日志」更集中在锁上。
  int threshold = global_level_.load(std::memory_order_relaxed);
  const int idx = static_cast<int>(category);
  if (idx >= 0 && idx < static_cast<int>(log_category::CATEGORY_COUNT)) {
    const int per = category_levels_[idx].load(std::memory_order_relaxed);
    if (per >= 0) threshold = per;
  }
  if (threshold == static_cast<int>(log_level::OFF)) return false;
  return static_cast<int>(level) >= threshold;
}

void uvcpp_logger::write(const uvcpp_log_record& record) {
  // **先取 sink（锁内）再放锁调用它（锁外）。** 早先版本把整次 sink 调用都
  // 关在锁里，于是控制台 sink 那次阻塞的 fwrite 会压着全局锁 —— 一个慢终端
  // 能把所有打日志的线程一起拖住，而且 sink 里再调 uvcpp_logger 的加锁方法
  // 会自死锁。
  //
  // 代价写在 set_sink() 的 @warning 里：库不再替你排「换 sink」与「正在写」
  // 的先后，所以别在有人打日志的时候销毁旧 sink。
  uvcpp_log_sink* target = sink();
  if (target == nullptr) return;

  // `should_log()` 也在 try 里：它同样是**使用者写的**代码（虚函数），
  // 从锁里挪出来之后更要一视同仁 —— 让它的异常穿透到调用点的析构链上，
  // 比"这条日志没打出来"严重得多。
  try {
    if (target->should_log(record.level, record.category)) {
      target->write(record);
    }
  } catch (...) {
  }
}

void uvcpp_logger::flush() {
  uvcpp_log_sink* target = sink();  // 内部拿锁 + 懒创建，随即放锁
  if (target == nullptr) return;
  try {
    target->flush();
  } catch (...) {
  }
}

// =========================================================================
// printf 风格入口
// =========================================================================

namespace {

/**
 * @brief `uvcpp_logf` / `uvcpp_logf_at` 的共同实现。
 *
 * 两个公开入口只差位置参数，所以把可变参数转发到这里 —— C++11 里没有别的
 * 办法把 `...` 原样转给另一个可变参数函数。
 */
void logf_v(log_level level, log_category category, const char* file, int line,
            const char* function, const char* fmt, va_list ap) {
  if (fmt == nullptr) return;
  // 先问过滤，再格式化：被挡掉的日志不该付 vsnprintf 的钱。
  if (!uvcpp_logger::instance().is_enabled(level, category)) return;

  // **一遍就够。** 直接格式化进栈缓冲：`vsnprintf` 返回「若缓冲区足够大本应
  // 写入的长度」，所以返回值小于缓冲区大小就说明没截断，不必再格式化第二遍。
  // 早先版本先 `vsnprintf(nullptr, 0, ...)` 探一次长度、再格式化一次，于是
  // **每条** printf 风格日志都付两遍格式化 —— 访问日志正好走这条路。
  char stack_buf[512];
  va_list ap_copy;
  va_copy(ap_copy, ap);  // 只有真截断了才用得上
  const int n = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);

  if (n < 0) {  // 格式化本身失败（非法格式串等）
    va_end(ap_copy);
    return;
  }

  uvcpp_log_record record;
  record.level    = level;
  record.category = category;
  record.file     = file;
  record.line     = line;
  record.function = function;

  if (static_cast<size_t>(n) < sizeof(stack_buf)) {
    va_end(ap_copy);
    record.message.assign(stack_buf, static_cast<size_t>(n));
    uvcpp_logger::instance().write(record);
    return;
  }

  // 被 512 字节截断了：这时才走第二遍，写进堆缓冲，长消息的尾巴不丢。
  std::string heap_buf;
  heap_buf.resize(static_cast<size_t>(n) + 1);
  std::vsnprintf(&heap_buf[0], static_cast<size_t>(n) + 1, fmt, ap_copy);
  va_end(ap_copy);
  heap_buf.resize(static_cast<size_t>(n));

  record.message = std::move(heap_buf);
  uvcpp_logger::instance().write(record);
}

}  // namespace

void uvcpp_logf_at(log_level level, log_category category, const char* file,
                   int line, const char* function, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logf_v(level, category, file, line, function, fmt, ap);
  va_end(ap);
}

void uvcpp_logf(log_level level, log_category category, const char* fmt, ...) {
  // 位置留空：老调用点的输出形状（没有 file:line）保持不变。
  va_list ap;
  va_start(ap, fmt);
  logf_v(level, category, nullptr, 0, nullptr, fmt, ap);
  va_end(ap);
}

}  // namespace uvcpp
