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
 */
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

  if (g_failures == 0) {
    std::cout << "[log] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[log] FAIL (" << g_failures << " checks failed)" << std::endl;
  return 2;
}
