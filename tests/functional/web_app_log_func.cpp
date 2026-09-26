/**
 * @file tests/functional/web_app_log_func.cpp
 * @brief webapp 日志虚接口的功能测试。
 *
 * 覆盖：
 *   - 自定义 sink 能接管框架日志（这是「使用者可接入自己的日志系统」的验收点）
 *   - 全局等级过滤 / 单模块等级覆盖
 *   - 流式宏的消息拼装（含各类数值类型的 operator<< 重载）
 *   - uvcpp_logf 的 printf 风格入口与长消息不截断
 *   - 从多个线程并发写日志不丢记录、不互相穿插
 *   - 默认 sink 是内置控制台实例
 *   - 用户 sink 在全局锁**外**执行（可在 write() 里回调 logger 的加锁方法）
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// `stream_is_tty()`：颜色那条用例要判前提。
#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <uvcpp/uvcpp_define.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_log_console.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

// 一个把日志收进内存的 sink —— 模拟「使用者接入自己的日志模块」。
class capturing_sink : public uvcpp_log_sink {
public:
  void write(const uvcpp_log_record& record) override {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(record);
  }

  size_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
  }

  // 返回第 idx 条记录的副本；越界返回默认构造的记录。
  uvcpp_log_record at(size_t idx) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (idx >= records_.size()) return uvcpp_log_record();
    return records_[idx];
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::vector<uvcpp_log_record> records_;
};

// 一个永远拒绝所有日志的 sink，用来验证 should_log() 真的被尊重。
class silent_sink : public uvcpp_log_sink {
public:
  void write(const uvcpp_log_record&) override { ++writes; }
  bool should_log(log_level, log_category) const override { return false; }
  std::atomic<int> writes{0};
};

// =========================================================================
// 1. 自定义 sink 接管
// =========================================================================
void test_custom_sink_takes_over(capturing_sink& sink) {
  std::cout << "[log] custom_sink_takes_over" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  sink.clear();

  UVCPP_LOG_INFO(log_category::REQUEST) << "hello " << 42;

  check(sink.count() == 1, "自定义 sink 应当收到 1 条记录");
  if (sink.count() == 1) {
    const uvcpp_log_record r = sink.at(0);
    check(r.level == log_level::INFO, "等级应为 INFO");
    check(r.category == log_category::REQUEST, "模块应为 REQUEST");
    check(r.message == "hello 42", "消息应拼装为 'hello 42'");
    check(r.file != nullptr, "应当带有源码位置");
    check(r.timestamp_ms > 0, "应当有时间戳");
    check(r.thread_id != 0, "应当有线程 id");
  }

  // should_log() 返回 false 的 sink 不能被喂数据
  silent_sink quiet;
  uvcpp_logger::instance().set_sink(&quiet);
  UVCPP_LOG_ERROR(log_category::CORE) << "should not be written";
  check(quiet.writes.load() == 0, "should_log()==false 时 sink 不应被调用");

  uvcpp_logger::instance().set_sink(&sink);
}

// =========================================================================
// 2. 等级过滤
// =========================================================================
void test_level_filtering(capturing_sink& sink) {
  std::cout << "[log] level_filtering" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  uvcpp_logger::instance().clear_category_overrides();
  uvcpp_logger::instance().set_level(log_level::INFO);
  sink.clear();

  UVCPP_LOG_TRACE(log_category::CORE) << "trace";
  UVCPP_LOG_DEBUG(log_category::CORE) << "debug";
  UVCPP_LOG_INFO(log_category::CORE) << "info";
  UVCPP_LOG_WARN(log_category::CORE) << "warn";
  UVCPP_LOG_ERROR(log_category::CORE) << "error";

  check(sink.count() == 3, "INFO 阈值下应只输出 info/warn/error 三条");

  // 全局关掉（没有模块级覆盖时，全局等级对所有模块生效）
  uvcpp_logger::instance().set_level(log_level::OFF);
  sink.clear();
  UVCPP_LOG_FATAL(log_category::CORE) << "nope";
  check(sink.count() == 0, "OFF 阈值下不应有任何输出");

  // 单模块覆盖：全局 ERROR，但 ROUTER 模块放宽到 DEBUG
  uvcpp_logger::instance().set_level(log_level::ERR);
  uvcpp_logger::instance().set_level(log_category::ROUTER, log_level::DEBUG);
  sink.clear();

  UVCPP_LOG_DEBUG(log_category::ROUTER) << "router debug";
  UVCPP_LOG_DEBUG(log_category::CORE) << "core debug";
  UVCPP_LOG_ERROR(log_category::CORE) << "core error";

  check(sink.count() == 2, "模块级覆盖后应输出 router debug + core error");
  if (sink.count() == 2) {
    check(sink.at(0).category == log_category::ROUTER,
          "第一条应来自 ROUTER");
    check(sink.at(1).category == log_category::CORE, "第二条应来自 CORE");
  }

  check(uvcpp_logger::instance().is_enabled(log_level::DEBUG,
                                            log_category::ROUTER),
        "ROUTER 的 DEBUG 应为启用");
  check(!uvcpp_logger::instance().is_enabled(log_level::DEBUG,
                                             log_category::CORE),
        "CORE 的 DEBUG 应为禁用");

  // set_all_category_levels 会把每个模块都显式设一遍，此后全局等级不再影响
  // 它们；clear_category_overrides 才让它们重新跟随全局。
  uvcpp_logger::instance().set_all_category_levels(log_level::WARN);
  uvcpp_logger::instance().set_level(log_level::TRACE);
  check(!uvcpp_logger::instance().is_enabled(log_level::DEBUG,
                                             log_category::ROUTER),
        "模块被显式设置后不应再跟随全局");
  uvcpp_logger::instance().clear_category_overrides();
  check(uvcpp_logger::instance().is_enabled(log_level::DEBUG,
                                            log_category::ROUTER),
        "清掉覆盖后应重新跟随全局");

  uvcpp_logger::instance().clear_category_overrides();
  uvcpp_logger::instance().set_level(log_level::TRACE);
}

// =========================================================================
// 3. 各类型 operator<<
// =========================================================================
void test_stream_formatting(capturing_sink& sink) {
  std::cout << "[log] stream_formatting" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
  sink.clear();

  const int neg = -7;
  const unsigned long long big = 18446744073709551615ULL;
  const void* ptr = reinterpret_cast<const void*>(0x1234);
  const char* null_str = nullptr;

  UVCPP_LOG_INFO(log_category::BODY)
      << "s=" << std::string("str") << " c=" << 'x' << " b=" << true
      << " b2=" << false << " i=" << neg << " u=" << 42u << " ll=" << big
      << " d=" << 3.5 << " p=" << ptr << " null=" << null_str;

  check(sink.count() == 1, "应输出一条");
  if (sink.count() == 1) {
    const std::string m = sink.at(0).message;
    check(m.find("s=str") != std::string::npos, "std::string 应被输出");
    check(m.find("c=x") != std::string::npos, "char 应被输出");
    check(m.find("b=true") != std::string::npos, "true 应输出为 true");
    check(m.find("b2=false") != std::string::npos, "false 应输出为 false");
    check(m.find("i=-7") != std::string::npos, "负数应保留符号");
    check(m.find("u=42") != std::string::npos, "unsigned 应被输出");
    check(m.find("ll=18446744073709551615") != std::string::npos,
          "64 位无符号应完整输出");
    check(m.find("d=3.5") != std::string::npos, "double 应被输出");
    check(m.find("p=0x") != std::string::npos, "指针应输出为十六进制");
    check(m.find("null=") != std::string::npos, "nullptr 字符串不应崩溃");
  }
}

// =========================================================================
// 4. uvcpp_logf
// =========================================================================
void test_logf(capturing_sink& sink) {
  std::cout << "[log] logf" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
  sink.clear();

  uvcpp_logf(log_level::WARN, log_category::HTTP, "code=%d path=%s", 404,
             "/missing");
  check(sink.count() == 1, "logf 应输出一条");
  if (sink.count() == 1) {
    check(sink.at(0).message == "code=404 path=/missing",
          "logf 应正确格式化");
    check(sink.at(0).level == log_level::WARN, "logf 的等级应为 WARN");
  }

  // 超过栈缓冲（512）的长消息不能被截断
  sink.clear();
  std::string filler(2000, 'A');
  uvcpp_logf(log_level::INFO, log_category::CORE, "%s", filler.c_str());
  check(sink.count() == 1, "长消息应输出一条");
  if (sink.count() == 1) {
    check(sink.at(0).message.size() == 2000,
          "2000 字节的消息不应被截断");
  }

  // 等级被过滤时不输出
  sink.clear();
  uvcpp_logger::instance().set_all_category_levels(log_level::ERR);
  uvcpp_logf(log_level::INFO, log_category::CORE, "filtered");
  check(sink.count() == 0, "被过滤的 logf 不应输出");
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
}

// =========================================================================
// 5. 多线程并发
// =========================================================================
void test_thread_safety(capturing_sink& sink) {
  std::cout << "[log] thread_safety" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
  sink.clear();

  const int kThreads = 8;
  const int kPerThread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.push_back(std::thread([t, kPerThread]() {
      for (int i = 0; i < kPerThread; ++i) {
        UVCPP_LOG_DEBUG(log_category::IO) << "t=" << t << " i=" << i;
      }
    }));
  }
  for (size_t i = 0; i < threads.size(); ++i) threads[i].join();

  check(sink.count() == static_cast<size_t>(kThreads * kPerThread),
        "并发写日志不应丢记录");

  // 每条消息都必须是完整的一条（没有两条互相穿插）
  bool all_well_formed = true;
  for (size_t i = 0; i < sink.count(); ++i) {
    const std::string m = sink.at(i).message;
    // 形如 "t=3 i=17"
    if (m.size() < 6 || m.compare(0, 2, "t=") != 0 ||
        m.find(" i=") == std::string::npos) {
      all_well_formed = false;
      break;
    }
  }
  check(all_well_formed, "并发下每条消息应保持完整、不被穿插");
}

// =========================================================================
// 6. 默认 sink
// =========================================================================
void test_default_sink() {
  std::cout << "[log] default_sink" << std::endl;
  uvcpp_logger::instance().set_sink(nullptr);
  uvcpp_log_sink* s = uvcpp_logger::instance().sink();
  check(s != nullptr, "未配置时应回落到内置 sink");

  // 内置实例应当是控制台 sink
  uvcpp_console_log_sink* console = dynamic_cast<uvcpp_console_log_sink*>(s);
  check(console != nullptr, "默认 sink 应当是 uvcpp_console_log_sink");

  // 真的往控制台打一条（人工可见，不作为断言）
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
  UVCPP_LOG_INFO(log_category::CORE)
      << "console sink smoke test (this line is expected)";
}

// =========================================================================
// 7. 名称映射
// =========================================================================
void test_names() {
  std::cout << "[log] names" << std::endl;
  check(std::strcmp(uvcpp_log_level_name(log_level::INFO), "INFO") == 0,
        "INFO 的名称");
  check(std::strcmp(uvcpp_log_level_name(log_level::ERR), "ERROR") == 0,
        "ERROR 的名称");
  check(std::strcmp(uvcpp_log_category_name(log_category::REQUEST),
                    "REQUEST") == 0,
        "REQUEST 的名称");
  check(std::strcmp(uvcpp_log_category_name(log_category::WEBSOCKET),
                    "WEBSOCKET") == 0,
        "WEBSOCKET 的名称");
  check(uvcpp_log_category_count() ==
            static_cast<size_t>(log_category::CATEGORY_COUNT),
        "模块总数应与枚举一致");

  // WSDL/SOAP 这两个模块是随 `src/wsdl/` 一起加进来的：它们必须在**哨兵
  // 之前**，否则 `CATEGORY_COUNT` 数不到它们，上面那条计数校验反而会通过。
  check(std::strcmp(uvcpp_log_category_name(log_category::SOAP), "SOAP") == 0,
        "SOAP 的名称");
  check(std::strcmp(uvcpp_log_category_name(log_category::WSDL), "WSDL") == 0,
        "WSDL 的名称");
  check(static_cast<int>(log_category::SOAP) <
            static_cast<int>(log_category::CATEGORY_COUNT) &&
        static_cast<int>(log_category::WSDL) <
            static_cast<int>(log_category::CATEGORY_COUNT),
        "SOAP/WSDL 必须排在 CATEGORY_COUNT 哨兵之前");

  // 名称表与枚举一一对应：逐个数过去，每个都必须是**非空且不含 '?'**
  // 的串。`uvcpp_log_category_name()` 对越界/未登记的值回 "?"，所以这条
  // 能抓住"枚举加了、名字表忘了加"这类错位 —— 上面的计数校验抓不住它
  // （那边用的是同一个 `CATEGORY_COUNT`，一起错就一起对）。
  bool names_intact = true;
  for (int i = 0; i < static_cast<int>(log_category::CATEGORY_COUNT); ++i) {
    const char* n = uvcpp_log_category_name(static_cast<log_category>(i));
    if (n == nullptr || n[0] == '\0' || std::strcmp(n, "?") == 0) {
      names_intact = false;
      break;
    }
  }
  check(names_intact, "每个 category 都应有登记的名字（不含 '?'）");
}

// =========================================================================
// 8. 重载集：enum / nullptr / 等级名
// =========================================================================

/// 用户定义的有作用域枚举 —— 走 SFINAE 模板重载，输出底层整数。
enum class demo_mode { kOff = 0, kFast = 7, kSlow = 42 };

/// 无作用域枚举 —— 模板重载让它走**精确匹配**而不是整型提升（见头文件里
/// 那条说明）。输出值不变，但绑定变了，所以单独钉一条。
enum legacy_mode { kLegacyA = 3, kLegacyB = 9 };

void test_enum_and_nullptr_streaming(capturing_sink& sink) {
  std::cout << "[log] enum_and_nullptr_streaming" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
  sink.clear();

  UVCPP_LOG_INFO(log_category::CORE) << "e=" << demo_mode::kSlow;
  // 等级/模块输出**名字**而不是数字：这是非模板重载胜出模板的那条路。
  UVCPP_LOG_INFO(log_category::CORE) << "lv=" << log_level::WARN;
  UVCPP_LOG_INFO(log_category::CORE) << "cat=" << log_category::ROUTER;
  // `<< nullptr` 在今天之前是**编译错误**（`const char*` 与 `const void*`
  // 两个候选打平，二义）。这条同时钉住"编得过"与"输出 null"。
  UVCPP_LOG_INFO(log_category::CORE) << "n=" << nullptr;
  UVCPP_LOG_INFO(log_category::CORE) << "u=" << kLegacyB;

  check(sink.count() == 5, "五条都应被输出");
  if (sink.count() == 5) {
    check(sink.at(0).message == "e=42", "enum class 应输出底层整数");
    check(sink.at(1).message == "lv=WARN",
          "log_level 应输出名字（非模板重载优先于模板）");
    check(sink.at(2).message == "cat=ROUTER", "log_category 应输出名字");
    check(sink.at(3).message == "n=null", "nullptr 应输出为 null");
    check(sink.at(4).message == "u=9",
          "无作用域枚举应输出数值（精确匹配改变了绑定，但输出不变）");
  }
}

// =========================================================================
// 9. UVCPP_LOGF 带位置
// =========================================================================
void test_logf_at_carries_location(capturing_sink& sink) {
  std::cout << "[log] logf_at_carries_location" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
  sink.clear();

  UVCPP_LOGF(log_level::INFO, log_category::REQUEST, "a=%d", 1);

  check(sink.count() == 1, "UVCPP_LOGF 应输出一条");
  if (sink.count() == 1) {
    check(sink.at(0).message == "a=1", "UVCPP_LOGF 应正确格式化");
    check(sink.at(0).file != nullptr &&
              std::strstr(sink.at(0).file, "web_app_log_func.cpp") != nullptr,
          "UVCPP_LOGF 的 file 应指向本文件");
    check(sink.at(0).line > 0, "UVCPP_LOGF 的 line 应是 __LINE__ 而不是 0");
    check(sink.at(0).function != nullptr, "UVCPP_LOGF 应带上函数名");
  }

  // 对照组：老的 `uvcpp_logf` **刻意**不带位置（它是源码兼容那条路，
  // 也是今天的行为）。两条路的差别必须真的存在，否则 P1-b 那笔没落地。
  sink.clear();
  uvcpp_logf(log_level::INFO, log_category::REQUEST, "b=%d", 2);
  check(sink.count() == 1, "uvcpp_logf 应输出一条");
  if (sink.count() == 1) {
    check(sink.at(0).file == nullptr,
          "对照组：uvcpp_logf 不带位置（file 应为 nullptr）");
  }
}

// =========================================================================
// 10. flush 转发
// =========================================================================

/// 只数 flush 次数、不关心记录内容的 sink。
class flushing_sink : public uvcpp_log_sink {
public:
  void write(const uvcpp_log_record&) override {}
  void flush() override { ++flushes; }
  std::atomic<int> flushes{0};
};

void test_flush_forwards() {
  std::cout << "[log] flush_forwards" << std::endl;
  flushing_sink fs;
  uvcpp_logger::instance().set_sink(&fs);

  // 前提断言：不写这一条的话，`flushes == 1` 对"根本没转发的实现"也一样
  // 成立不了 —— 但也对"构造时就调过一次"的实现成立，两种都读成绿。
  check(fs.flushes.load() == 0, "前提：还没 flush 过");

  uvcpp_logger::instance().flush();
  check(fs.flushes.load() == 1, "logger::flush() 应转发到 sink");
  uvcpp_logger::instance().flush();
  check(fs.flushes.load() == 2, "第二次也应转发（不是一次性）");

  uvcpp_logger::instance().set_sink(nullptr);
}

// =========================================================================
// 11. 颜色判据按流分别求值
// =========================================================================

/// 这条流是不是终端。与 `uvcpp_log_console.cpp` 里的探测同义。
bool stream_is_tty(std::FILE* f) {
#if defined(_WIN32)
  return _isatty(_fileno(f)) != 0;
#else
  return isatty(fileno(f)) != 0;
#endif
}

int test_color_per_stream() {
  std::cout << "[log] color_per_stream" << std::endl;

  // ⚠️ **前提**：两条流都必须是"非终端"（重定向到文件/管道）。判据问的是
  // "重定向之后还上不上色"，只有在这个状态上才有意义；前提不成立时报 3
  // （没判）而不是红 —— 让一条"什么都没测到"的用例变绿是最糟的结果。
  if (stream_is_tty(stdout) || stream_is_tty(stderr)) {
    std::cout << "[log] SKIP color_per_stream：stdout/stderr 是终端，"
                 "没有可判的状态" << std::endl;
    return 3;
  }

  uvcpp_console_log_sink console;
  check(console.color_enabled(false) == false,
        "stdout 不是终端时不应上色");
  check(console.color_enabled(true) == false,
        "stderr 不是终端时不应上色");
  // 无参那个是 stdout 那条的别名，两者必须一致。
  check(console.color_enabled() == console.color_enabled(false),
        "color_enabled() 应等价于 color_enabled(false)");

  // 对照组：`options.color = false` 时两条流都不上色。这一半与终端能力
  // 无关，所以哪怕前提成立也总能判。
  uvcpp_console_log_options opts;
  opts.color = false;
  uvcpp_console_log_sink off(opts);
  check(off.color_enabled(false) == false, "color=false 时 stdout 不上色");
  check(off.color_enabled(true) == false, "color=false 时 stderr 不上色");

  // ⚠️ **诚实边界**：「stdout 是终端而 stderr 不是」（`./server > out.log`）
  // 这个**原始形状在本机与 CI 都造不出来** —— 两边两条流都不是终端。所以
  // 这条用例能钉住的只是"判据按流分别求值、各自独立参与判断"这件事本身，
  // **不能**证明重定向场景真的对。那一条只能靠读码 + 人工跑一次。
  return 0;
}

// =========================================================================
// 12. 浮点：最短往返表示
// =========================================================================
void test_float_roundtrip(capturing_sink& sink) {
  std::cout << "[log] float_roundtrip" << std::endl;
  uvcpp_logger::instance().set_sink(&sink);
  uvcpp_logger::instance().set_all_category_levels(log_level::TRACE);
  sink.clear();

  const double d1 = 3.5;
  const double d2 = 0.1;
  const double d3 = 3.141592653589793;  // 16 位有效数字，%g 的 6 位装不下
  const float f1 = 0.1f;

  UVCPP_LOG_INFO(log_category::CORE) << "d1=" << d1;
  UVCPP_LOG_INFO(log_category::CORE) << "d2=" << d2;
  UVCPP_LOG_INFO(log_category::CORE) << "d3=" << d3;
  UVCPP_LOG_INFO(log_category::CORE) << "f1=" << f1;

  check(sink.count() == 4, "四条都应被输出");
  if (sink.count() == 4) {
    check(sink.at(0).message == "d1=3.5", "3.5 应打印为 3.5");
    check(sink.at(1).message == "d2=0.1", "0.1 应打印为 0.1");
    // `%g` 默认 6 位有效数字，会把它截成 "3.14159"。这是本次改动的
    // 主要目标：不丢精度的同时也不拖出一串无意义的尾数。
    check(sink.at(2).message == "d3=3.141592653589793",
          "16 位有效数字不应被截断（%g 会给 3.14159）");
    // float 必须走**自己那条**：0.1f 提升成 double 是 0.10000000149011612，
    // 按 double 求最短往返会一字不差地打印出那一长串。选 6 位精度、用
    // strtof 回读，才能得到 "0.1"。
    check(sink.at(3).message == "f1=0.1",
          "float 0.1f 应打印为 0.1（按 double 求会变成 0.10000000149011612）");
  }
}

// =========================================================================
// 13. sink 在锁外被调用（用户 sink 可以回调 logger 的加锁方法）
// =========================================================================

/// 在自己的 `write()` 里回调 `uvcpp_logger` 的**加锁写方法**。
///
/// 探针必须是**加锁**的那个方法：`level()` / `is_enabled()` 在 1.4.0 改成了
/// relaxed 原子读（那正是本笔要消掉的那把锁），拿它们探不出锁在不在。
/// `set_level()` 仍然要拿 `logger::mutex_`。`std::mutex` 不可重入 ⇒ 旧实现
/// （`write()` 持锁调 sink）在这里**自死锁**，新实现（锁外调 sink）正常返回。
///
/// ⚠️ **"+ 自死锁"这个形状在 MSVC 上不会真的挂住**：同线程重锁 `std::mutex`
/// 时 MSVC 的 `_Mtx_lock` 返回 `_EDEADLK`，`std::mutex::lock()` 抛
/// `std::system_error`。那个异常穿透 sink，被 `logger::write()` 的
/// `catch (...)` 吞掉（见 `uvcpp_log.cpp` —— 故意吞的，sink 的异常不该穿到
/// 调用者的析构链上），于是**死锁变成"函数体半途中断"**：下面的
/// `reentered` / `seen_message` / `set_level` 那三条断言照样红，但上面那个
/// 5 秒看门狗那条臂只在 glibc（真死锁）上才走得到。两条路都在，别只留一条。
class reentrant_sink : public uvcpp_log_sink {
public:
  void write(const uvcpp_log_record& record) override {
    ++writes;
    // 设成 `ERR`，**故意与用例预置的 `TRACE` 不同** —— 否则下面那条
    // "set_level() 生效"的断言恒真（早先版本正是这么写的，等于没牙）。
    uvcpp_logger::instance().set_level(log_level::ERR);
    reentered.store(true);
    seen_message = record.message;
  }

  std::atomic<int> writes{0};
  std::atomic<bool> reentered{false};
  std::string seen_message;
};

void test_sink_runs_outside_the_lock() {
  std::cout << "[log] sink_runs_outside_the_lock" << std::endl;

  reentrant_sink rs;
  uvcpp_logger::instance().set_sink(&rs);
  uvcpp_logger::instance().set_level(log_level::TRACE);

  // 在**独立线程**上打这一条，再用带超时的等待把死锁收成一条断言。
  //
  // 为什么不自带超时不行：`test_web_app_log_func` **没有挂 TIMEOUT**（整个
  // tests/functional/CMakeLists.txt 里只有 static 与 multiloop 两个挂了），
  // 死锁在这里等于整个 ctest 卡住。而且 ctest 的超时只会说"超时了"，说不出
  // 是哪条判据 —— 自带一个 5 秒上限能把结论落在具体那句话上。
  std::atomic<bool> done{false};
  std::thread t([&]() {
    UVCPP_LOG_INFO(log_category::REQUEST) << "reentrant";
    done.store(true);
  });

  bool finished = false;
  for (int i = 0; i < 500; ++i) {   // 500 × 10ms = 5s 上限
    if (done.load()) {
      finished = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!finished) {
    // 死锁了。**不能 join、也不能再碰 logger 的任何加锁方法** —— 那会把本条
    // 线程一起挂住，于是"判红了"又变成"挂死了"。detach 掉直接收摊：
    // `_Exit` 跳过析构（那条线程还持着锁，让它跑完析构只会更难读）。
    t.detach();
    check(false,
          "sink 回调 logger 的加锁方法时不应自死锁 —— `logger::write()` 必须在"
          "**锁外**调 sink");
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(2);
  }
  t.join();

  check(rs.writes.load() == 1, "重入的 sink 应收到那条记录");
  check(rs.reentered.load(), "sink 里那次回调应真的跑到了");
  check(rs.seen_message == "reentrant", "重入时消息仍应完整");
  // 这条是"sink 里那次加锁写真的跑完了"的证据：sink 把等级设成 `ERR`，而
  // 上面预置的是 `TRACE`，两者不同 ⇒ 只有那次调用真的生效才会是 `ERR`。
  check(uvcpp_logger::instance().global_level() == log_level::ERR,
        "sink 里那次 set_level() 应当生效（旧实现会在这一步抛/挂，等级留在 TRACE）");

  uvcpp_logger::instance().set_sink(nullptr);
  uvcpp_logger::instance().set_level(log_level::TRACE);  // 免得扰到后面的用例
}

}  // namespace

int main() {
  // 让控制台 sink 在测试里安静一点（真实输出用 set_sink 换掉了）
  uvcpp_logger::instance().set_level(log_level::TRACE);

  capturing_sink sink;

  test_names();
  test_custom_sink_takes_over(sink);
  test_level_filtering(sink);
  test_stream_formatting(sink);
  test_logf(sink);
  test_thread_safety(sink);
  test_default_sink();
  test_enum_and_nullptr_streaming(sink);
  test_logf_at_carries_location(sink);
  test_flush_forwards();
  test_float_roundtrip(sink);
  test_sink_runs_outside_the_lock();
  const int color_rc = test_color_per_stream();

  if (g_failures != 0) {
    std::cout << "[log] FAIL (" << g_failures << " checks failed)" << std::endl;
    return 2;
  }
  // 1（红）压过 3（没判）：上面已经先判过了，走到这里说明没有红的。
  if (color_rc == 3) {
    std::cout << "[log] INCONCLUSIVE: color_per_stream 的前提没满足（见上面那条 "
                 "SKIP），其余用例全过 —— 这不是绿，只是没判" << std::endl;
    return 3;
  }
  std::cout << "[log] ALL PASS" << std::endl;
  return 0;
}
