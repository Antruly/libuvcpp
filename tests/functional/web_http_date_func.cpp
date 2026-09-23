/**
 * @file tests/functional/web_http_date_func.cpp
 * @brief `Date` 响应头：IMF-fixdate 格式化、**按秒缓存**、以及服务端出报文收口上的注入。
 *
 * 这一组里有三条判据是为了"证伪自己"而存在的，不是装饰：
 *
 *   - `date_is_locale_immune` 先**证明本机的 `LC_TIME` 真的换了**（`strftime("%a")`
 *     必须吐不出 `Sun`），再去断言格式化结果没变。少了前半截，这条在任何没有该
 *     区域的环境里都会**恒绿**，而它想证的那件事一次都没被验过。
 *   - `date_cache_is_per_second` 先验"新线程第一次调用**确实**让计数器 +1"，
 *     再验"同一秒内 1001 次调用让计数器 +0"。少了前半截，后半截在"计数器坏了、
 *     恒返回 0"的实现下照样绿。
 *   - wire 那条把**响应头行的值**拿去和 `http_date(±1s)` 比，而不是只查
 *     `find("date:") != npos` —— 后者在"日期停在编译期某一天"的实现下也是绿的。
 *
 * 第四处注入（h2 流式头部）的判据不在这里：那条路只在 h2 上可达，见
 * `h2_session_func.cpp` 与 `web_ssl_app_h2_func.cpp`。h1 流式那条（第三处注入）
 * 的判据在 `web_stream_response_func.cpp` —— 那里已经有整套流式装置，不重复搭。
 */
#include <iostream>
#include <cstring>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_http_date.h>
#include <web/uvcpp_http_server.h>
#include <net/uvcpp_tcp_client.h>

#include <atomic>
#include <chrono>
#include <clocale>
#include <ctime>
#include <functional>
#include <future>
#include <thread>
using namespace uvcpp;

// -------------------------------------------------------------------------
// TestServer —— 与 web_http_server_func.cpp 里那个同形（起独立线程跑循环、
// listen 之后再发布端口、stop 后 join）。
// -------------------------------------------------------------------------
class TestServer {
 public:
  using SetupFn = std::function<void(uvcpp_http_server&)>;

  int start(SetupFn setup) {
    thread_ = std::thread([this, setup]() {
      uvcpp_http_server server;
      if (setup) setup(server);
      server.bind("127.0.0.1", 0);

      sockaddr_in name;
      int namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      const int port = ntohs(name.sin_port);

      // 先 listen、再发布端口：反过来的话调用方立刻 connect 会拿到 ECONNREFUSED。
      server.listen();
      port_promise_.set_value(port);
      while (!stop_.load()) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return port_promise_.get_future().get();
  }

  void shutdown() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::promise<int> port_promise_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// 发原始字节、读回响应。返回 false = 连接/写/读失败。
static bool raw_exchange(int port, const std::string& send, std::string& received,
                         int read_timeout_ms = 3000) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;
  if (c.write_wait(send.c_str(), send.size(), 3000) != 0) return false;
  uvcpp_buf out;
  if (c.read_wait(out, read_timeout_ms) != 0) return false;
  received.assign(out.get_const_data() ? out.get_const_data() : "", out.size());
  return true;
}

// 在 `s` 里找 `\r\n` + name + `: `，返回值的起始下标（找不到给 npos）。
// 锚在行首是为了不匹配到别的头里恰好含 "date" 的片段。
static size_t find_header_value(const std::string& s, const std::string& name) {
  const std::string needle = "\r\n" + name + ": ";
  const size_t p = s.find(needle);
  if (p == std::string::npos) return std::string::npos;
  // 让下标落在值的第一个字节上（补上被 needle 吃掉的那两个 `\r\n`）
  return p + 2 + name.size() + 2;
}

/// IMF-fixdate 的**形态**校验：`Xxx, DD Mon YYYY HH:MM:SS GMT`（29 字节）。
///
/// 位次（`"Sun, 06 Nov 1994 08:49:37 GMT"`）：
///   0-2 星期  3 `,`  4 ` `  5-6 日  7 ` `  8-10 月  11 ` `  12-15 年
///   16 ` `  17-18 时  19 `:`  20-21 分  22 `:`  23-24 秒  25 ` `  26-28 `GMT`
///
/// 星期/月份名对着**自己的一张表**比（不是拿 `http_date` 的输出比，那会绕回去），
/// 数字位逐位判、再按数值域判范围 —— 位数错、`25:00:00` 这种越界都拦得住。
static bool looks_like_imf(const std::string& v) {
  static const char* const k_wd[] = {"Sun", "Mon", "Tue", "Wed",
                                     "Thu", "Fri", "Sat"};
  static const char* const k_mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const int k_digits[] = {5, 6, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24};
  static const int k_fixed[] = {3, 4, 7, 11, 16, 19, 22, 25};
  static const char k_want[] = {',', ' ', ' ', ' ', ' ', ':', ':', ' '};

  if (v.size() != 29) return false;
  bool wd_ok = false;
  for (const char* w : k_wd) {
    if (v.compare(0, 3, w) == 0) {
      wd_ok = true;
      break;
    }
  }
  if (!wd_ok) return false;
  bool mo_ok = false;
  for (const char* m : k_mo) {
    if (v.compare(8, 3, m) == 0) {
      mo_ok = true;
      break;
    }
  }
  if (!mo_ok) return false;
  for (size_t i = 0; i < sizeof(k_fixed) / sizeof(k_fixed[0]); ++i) {
    if (v[k_fixed[i]] != k_want[i]) return false;
  }
  for (int i : k_digits) {
    if (v[i] < '0' || v[i] > '9') return false;
  }
  if (v.compare(26, 3, "GMT") != 0) return false;

  const int day = (v[5] - '0') * 10 + (v[6] - '0');
  const int hh = (v[17] - '0') * 10 + (v[18] - '0');
  const int mi = (v[20] - '0') * 10 + (v[21] - '0');
  const int ss = (v[23] - '0') * 10 + (v[24] - '0');
  if (day < 1 || day > 31) return false;
  if (hh > 23 || mi > 59 || ss > 59) return false;
  return true;
}

// -------------------------------------------------------------------------
// Test: 定点值的格式化（含 RFC 9110 §5.6.7 自己那个例子）
//
// 期望串是**逐字段**核过的（数字段用 `date -u -d @<t>` 的本地化输出对过：
// 本机区域吐 `日, 06 11月 1994`，数字与顺序与英文串逐个一致，只有名字不同）。
// -------------------------------------------------------------------------
static bool test_date_format_known_values() {
  struct { time_t t; const char* want; } cases[] = {
      {0, "Thu, 01 Jan 1970 00:00:00 GMT"},
      {1, "Thu, 01 Jan 1970 00:00:01 GMT"},
      // RFC 9110 §5.6.7 用的就是这一条
      {784111777, "Sun, 06 Nov 1994 08:49:37 GMT"},
      // 闰日：2000 是闰年（能被 400 整除）
      {951782400, "Tue, 29 Feb 2000 00:00:00 GMT"},
      {1700000000, "Tue, 14 Nov 2023 22:13:20 GMT"},
      // 32 位 time_t 的上界
      {2147483647, "Tue, 19 Jan 2038 03:14:07 GMT"},
      // 超出 32 位
      {4102444800LL, "Fri, 01 Jan 2100 00:00:00 GMT"},
  };
  for (const auto& c : cases) {
    const std::string got = http_date(c.t);
    if (got != c.want) {
      std::cerr << "  [date] t=" << static_cast<long long>(c.t) << " 得到 " << got
                << "，期望 " << c.want << std::endl;
      return false;
    }
    if (!looks_like_imf(got)) return false;
    // 绝不能带 CR/LF：它是头行的值，混进去就是头注入。
    if (got.find('\r') != std::string::npos ||
        got.find('\n') != std::string::npos) {
      return false;
    }
  }
  return true;
}

// -------------------------------------------------------------------------
// Test: 格式化不受 `LC_TIME` 影响
//
// 这一条是"为什么写死 ASCII 星期/月份表"那个设计的判据。`strftime("%a")` / `%b`
// 走区域，任何 `setlocale(LC_TIME, "")` 之后同一台机器上就吐中文 —— 那不是
// "格式不同"，是对端解析不出来（缓存静默失效）。
// -------------------------------------------------------------------------
static bool test_date_is_locale_immune() {
  // 存下原值：`setlocale` 返回的指针会被下一次调用作废，所以必须先拷成 string。
  const char* cur = std::setlocale(LC_TIME, nullptr);
  const std::string saved = (cur != nullptr) ? cur : "C";

  // 本机区域（VM 上是 zh_CN.UTF-8）。拿不到就如实报"这条判不了"。
  if (std::setlocale(LC_TIME, "") == nullptr) {
    std::cerr << "  [date] 环境里没有可用的区域，locale 那条判不了" << std::endl;
    std::setlocale(LC_TIME, saved.c_str());
    return true;
  }

  // **反假绿的前半截**：证明区域真的换了。同一个区域下 `strftime("%a")` 必须
  // 吐不出英文 `Sun`，否则下面那句断言就是在"C 区域"下自证。
  std::time_t t = 784111777;
  struct tm tmv;
#if defined(_WIN32)
  gmtime_s(&tmv, &t);
#else
  gmtime_r(&t, &tmv);
#endif
  char probe[64] = {0};
  std::strftime(probe, sizeof(probe), "%a", &tmv);
  if (std::strcmp(probe, "Sun") == 0) {
    std::cerr << "  [date] 区域没换成功（strftime(\"%a\") 仍是 Sun），"
                 "这条判不了" << std::endl;
    std::setlocale(LC_TIME, saved.c_str());
    return true;
  }

  const std::string got = http_date(784111777);
  std::setlocale(LC_TIME, saved.c_str());  // 无论下面怎么返回，先把进程状态还回去

  if (got != "Sun, 06 Nov 1994 08:49:37 GMT") {
    std::cerr << "  [date] 区域是 " << probe << "，格式化结果被带跑了：" << got
              << std::endl;
    return false;
  }
  return true;
}

// -------------------------------------------------------------------------
// Test: 按秒缓存 —— 同一秒内 1001 次调用做 **0** 次格式化
//
// 缓存是线程本地的，所以先在本线程烤热槽位，再清计数器。只比两次返回值相等
// 是证不出来的（同一秒内本来就相等）；能证的是"真正做格式化的次数"。
// -------------------------------------------------------------------------
static bool test_date_cache_is_per_second() {
  // 前半截：新线程的第一次调用**必须**让计数器 +1。少了它，下面那个 0 在
  // "计数器恒为 0" 的实现下同样会绿。
  http_date_reset_format_count();
  std::atomic<size_t> first_delta{0};
  std::thread fresh([&first_delta]() {
    const size_t before = http_date_format_count();
    (void)http_date_now();
    first_delta.store(http_date_format_count() - before);
  });
  fresh.join();
  if (first_delta.load() != 1) {
    std::cerr << "  [date] 新线程第一次调用让计数器 +" << first_delta.load()
              << "，期望 1 —— 计数器本身就不对，下面那条判据没有意义" << std::endl;
    return false;
  }

  // 后半截：本线程烤热之后，同一秒内 1001 次调用一次都不该格式化。
  (void)http_date_now();
  http_date_reset_format_count();
  const time_t t0 = ::time(NULL);
  const std::string first = http_date_now();
  std::string last;
  for (int i = 0; i < 1000; ++i) last = http_date_now();
  const time_t t1 = ::time(NULL);
  const size_t n = http_date_format_count();
  if (t0 != t1) {
    // 这一跑跨了秒，判不了。如实说，然后跳过（不是"绿"）。
    std::cerr << "  [date] 这一跑跨了一秒，缓存那条跳过" << std::endl;
    return true;
  }
  if (n != 0) {
    std::cerr << "  [date] 同一秒内 1001 次调用格式化了 " << n << " 次，期望 0"
              << std::endl;
    return false;
  }
  if (first != last) return false;
  return true;
}

// -------------------------------------------------------------------------
// Test: `http_ensure_date()` 的三条语义
//   (1) 没有就补上，且补的是"现在"（允许 ±1 秒，跨秒是正常的）
//   (2) 已经有了就不覆盖（处理函数自己设的说了算）
//   (3) `to_string()` 是纯序列化 —— 它**不**自己往上加 `Date`
// -------------------------------------------------------------------------
static bool test_ensure_date_semantics() {
  // (3) 先验纯序列化：没调 ensure 之前，序列化结果里不许出现 date 头。
  {
    auto resp = uvcpp_http_response::ok("body", 4, "text/plain");
    if (resp.has_header("date")) return false;
    const std::string wire = resp.to_string();
    if (wire.find("\r\ndate: ") != std::string::npos) return false;
  }

  // (1) 没有就补上。
  {
    auto resp = uvcpp_http_response::ok("body", 4, "text/plain");
    const time_t before = ::time(NULL);
    http_ensure_date(resp);
    const time_t after = ::time(NULL);
    if (!resp.has_header("date")) return false;
    const std::string v = resp.get_header("date", "");
    if (!looks_like_imf(v)) {
      std::cerr << "  [date] ensure 补出来的值不是 IMF-fixdate：" << v << std::endl;
      return false;
    }
    // 值必须落在 [before-1, after+1] 里。往前放宽 1 秒是因为缓存可能刚好
    // 停在上一秒；往后放宽同理。
    bool matched = false;
    for (time_t t = before - 1; t <= after + 1; ++t) {
      if (v == http_date(t)) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      std::cerr << "  [date] ensure 补出来的值 " << v << " 与当前时刻对不上"
                << std::endl;
      return false;
    }
    // 必须真的序列化出去。
    if (resp.to_string().find("\r\ndate: ") == std::string::npos) return false;
  }

  // (2) 已经有了就不覆盖。
  {
    auto resp = uvcpp_http_response::ok("body", 4, "text/plain");
    const std::string mine = "Mon, 01 Jan 2001 00:00:00 GMT";
    resp.set_header("date", mine.c_str());
    http_ensure_date(resp);
    if (resp.get_header("date", "") != mine) {
      std::cerr << "  [date] 处理函数设的 date 被覆盖成了 "
                << resp.get_header("date", "") << std::endl;
      return false;
    }
    // 重复调用同样是空操作（同一个 resp 会经过不止一条出口）。
    http_ensure_date(resp);
    if (resp.get_header("date", "") != mine) return false;
  }
  return true;
}

// -------------------------------------------------------------------------
// Test: 线上真的带了 `Date`（第一处注入：h1 整包响应）
// -------------------------------------------------------------------------
static bool test_date_on_the_wire_h1() {
  TestServer srv;
  const int port = srv.start([](uvcpp_http_server& s) {
    s.get("/d", [](uvcpp_http_request&, uvcpp_http_response& resp,
                   uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("hi", 2, "text/plain");
    });
    // 处理函数自己设过 date 的路由：值必须原样出现在线上。
    s.get("/mine", [](uvcpp_http_request&, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
      resp = uvcpp_http_response::ok("hi", 2, "text/plain");
      resp.set_header("date", "Mon, 01 Jan 2001 00:00:00 GMT");
    });
  });
  if (port <= 0) return false;

  std::string raw;
  const bool sent = raw_exchange(
      port, "GET /d HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", raw);
  std::string raw_mine;
  const bool sent_mine = raw_exchange(
      port, "GET /mine HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", raw_mine);
  srv.shutdown();
  if (!sent || !sent_mine) return false;

  // 头部块必须完整（不然下面的"找不到"会是假绿）。
  if (raw.find("\r\n\r\n") == std::string::npos) return false;
  if (raw.find("200") == std::string::npos) return false;

  const size_t p = find_header_value(raw, "date");
  if (p == std::string::npos) {
    std::cerr << "  [date] 线上没有 date 头。响应：\n" << raw << std::endl;
    return false;
  }
  const std::string v = raw.substr(p, 29);
  if (!looks_like_imf(v)) {
    std::cerr << "  [date] 线上的 date 值不成形态：" << v << std::endl;
    return false;
  }
  // 必须是"现在"，不是某个固定串（固定串在"日期被冻住"的实现下也是绿的）。
  const time_t now = ::time(NULL);
  if (v != http_date(now) && v != http_date(now - 1) && v != http_date(now + 1)) {
    std::cerr << "  [date] 线上的 date 值 " << v << " 不是当前时刻" << std::endl;
    return false;
  }
  // 紧跟着必须是 CRLF（值恰好 29 字节，没有被多写几个字符）。
  if (raw.compare(p + 29, 2, "\r\n") != 0) return false;

  // 自己设过的那条路：值必须逐字节原样。
  const size_t q = find_header_value(raw_mine, "date");
  if (q == std::string::npos) return false;
  if (raw_mine.compare(q, 29, "Mon, 01 Jan 2001 00:00:00 GMT") != 0) {
    std::cerr << "  [date] 处理函数设的 date 没原样上线："
              << raw_mine.substr(q, 29) << std::endl;
    return false;
  }
  return true;
}

// -------------------------------------------------------------------------
int main(int argc, char** argv) {
  const std::string filter = (argc > 1) ? argv[1] : std::string();
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
      {"date_format_known_values", test_date_format_known_values},
      {"date_is_locale_immune", test_date_is_locale_immune},
      {"date_cache_is_per_second", test_date_cache_is_per_second},
      {"ensure_date_semantics", test_ensure_date_semantics},
      {"date_on_the_wire_h1", test_date_on_the_wire_h1},
  };
  for (const auto& t : tests) {
    if (!filter.empty() &&
        std::string(t.name).find(filter) == std::string::npos) {
      continue;
    }
    std::cout << "[web_http_date] " << t.name << std::endl;
    const bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << std::endl;
    ok = r && ok;
  }
  std::cout << "[web_http_date] " << (ok ? "ALL PASS" : "FAIL") << std::endl;
  return ok ? 0 : 2;
}

#else
int main() {
  std::cout << "[web_http_date] SKIP (UVCPP_WEB_ENABLE=0)" << std::endl;
  return 0;
}
#endif
