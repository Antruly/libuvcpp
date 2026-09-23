/**
 * @file src/web/uvcpp_http_date.cpp
 * @brief HTTP 日期（IMF-fixdate）格式化与 `Date` 头的按秒缓存 —— 实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_date.h>

#if UVCPP_WEB_ENABLE

#include <atomic>
#include <cstdio>
#include <cstring>

namespace uvcpp {

namespace {

// 星期名与月份名**写死 ASCII**。头文件里那条说明是这块的全部理由：`strftime`
// 的 `%a` / `%b` 走 locale，任何 `setlocale(LC_TIME, "")` 之后会吐中文，
// 而那在 HTTP 日期上不是"格式不同"，是**对端解析不出来**。
const char* const k_wday_names[] = {"Sun", "Mon", "Tue", "Wed",
                                    "Thu", "Fri", "Sat"};
const char* const k_mon_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/// IMF-fixdate 的长度，`sizeof("Sun, 06 Nov 1994 08:49:37 GMT") - 1`。
const size_t k_imf_len = 29;

/// `http_date_now()` 的**线程本地**槽位。
///
/// 只放**平凡可析构**的东西 —— `time_t` + `bool` + `char[30]`，一个非平凡成员
/// 都没有。带析构的 thread_local 会注册 TLS 析构回调，而 MinGW 下那回调里
/// `delete` 用的不是当初分配的那块堆（hical 的 `ReadBufferPool::PoolSlots`
/// 就是为这个套了 `#ifndef __MINGW32__`）。这里根本没有回调可注册，所以是安全的。
///
/// 用线程本地而不是一把全局锁：`set_loops(n)` 之后有 n 条循环线程各自出响应，
/// 共享一把锁就变成跨核抢同一条缓存行；而"这个秒的串"本来就该按线程各存一份。
struct date_slot {
  time_t sec;
  bool   valid;
  char   text[k_imf_len + 1];  // NUL 结尾，好直接喂 `set_header(const char*, ..)`
};

date_slot& slot_here() {
  static thread_local date_slot s = {0, false, {0}};
  return s;
}

/// 观测计数：真正做格式化的次数。进程一份（用例要跨线程看它，所以不能是
/// thread_local —— 那正是被测对象的行为，判据不能用被测对象的形状来记）。
std::atomic<size_t>& format_counter() {
  static std::atomic<size_t> c(0);
  return c;
}

/// 把"现在"填进本线程的槽位，返回槽位里那个 NUL 结尾的串；失败返回 `nullptr`。
///
/// 这是**内部**形态（不对外）：`http_ensure_date()` 需要的是一个 C 串 —— 走
/// `set_header(const char*, const char*)` 时头的名与值各自就地构造，值只分配一次，
/// 而先造一个 `std::string` 再传 `const std::string&` 会多一次临时分配。
const char* now_cstr_in_slot() {
  const time_t now = ::time(NULL);
  date_slot& s = slot_here();
  if (s.valid && s.sec == now) return s.text;

  const std::string text = http_date(now);
  format_counter().fetch_add(1, std::memory_order_relaxed);
  // 长度必须是 29：`http_date()` 在 `gmtime` 越界时返回空串，而一个**被截断的**
  // 日期串比没有更坏（对端会把它当成有效的日期解析出乱七八糟的年月日），所以
  // 只接受恰好 29 字节的那一种形态。
  if (text.size() != k_imf_len) {
    s.valid = false;
    return nullptr;
  }
  std::memcpy(s.text, text.data(), k_imf_len);
  s.text[k_imf_len] = '\0';
  s.sec   = now;
  s.valid = true;
  return s.text;
}

}  // namespace

std::string http_date(time_t t) {
  struct tm tmv;
#if defined(_WIN32)
  if (::gmtime_s(&tmv, &t) != 0) return std::string();
#else
  if (::gmtime_r(&t, &tmv) == NULL) return std::string();
#endif

  // 30 字节足够装 29 + NUL —— 前提是年份恰好 4 位。
  char buf[k_imf_len + 1];
  const int n = std::snprintf(buf, sizeof(buf),
                              "%s, %02d %s %04d %02d:%02d:%02d GMT",
                              k_wday_names[tmv.tm_wday % 7], tmv.tm_mday,
                              k_mon_names[tmv.tm_mon % 12], tmv.tm_year + 1900,
                              tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

  // `snprintf` 返回的是**本该写多少**，可以 `>= sizeof(buf)`。年份超出 4 位
  // （或为负）时就会撞上这一条 —— 那时候按返回值构造 `std::string` 是**越界读**，
  // 所以上界必须自己判，只判 `n <= 0` 是不够的。
  if (n <= 0 || static_cast<size_t>(n) != k_imf_len) return std::string();
  return std::string(buf, k_imf_len);
}

std::string http_date_now() {
  const char* p = now_cstr_in_slot();
  return p != nullptr ? std::string(p) : std::string();
}

void http_ensure_date(uvcpp_http_response& resp) {
  // 处理函数自己设过就不覆盖 —— 与 webapp 层那条 `Server` 同一个约定。
  // 这里也顺带让重复调用（同一个 resp 被两条出口各过一遍）成为空操作。
  //
  // 用响应自己的 `has_header()`（它就是 `http_has_header(headers, key)` 的一行
  // 转发），而不是直接摸 `headers` 成员：少一处对"那个成员是公开的"的依赖。
  if (resp.has_header("date")) return;
  const char* text = now_cstr_in_slot();
  if (text == nullptr) return;  // 没有可靠的钟就不发：RFC 9110 §6.6.1 允许
  resp.set_header("date", text);
}

size_t http_date_format_count() {
  return format_counter().load(std::memory_order_relaxed);
}

void http_date_reset_format_count() {
  format_counter().store(0, std::memory_order_relaxed);
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
