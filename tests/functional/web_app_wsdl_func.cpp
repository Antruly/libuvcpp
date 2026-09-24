/**
 * @file tests/functional/web_app_wsdl_func.cpp
 * @brief 把 WSDL 发出去：真端口、真连接、真客户端。
 *
 * 这一层只有三件事可能错，所以三件事各有一组判据：
 *
 * 1. **字节对不对**：客户端拿到的正文必须与 `uvcpp_wsdl_dump()` **逐字节**
 *    相同，content-type 必须是 `text/xml; charset=utf-8`。
 * 2. **生命周期对不对**：路由捕获的是**注册那一刻**的共享指针。所以
 *    "源对象早就析构了"必须照发不误，而"注册之后再改源"必须**不发**新内容。
 * 3. **语义是不是真的像文档写的那样**：这一组里最要紧的一条是
 *    **重复注册同一个路径** —— 路由表在特异度打平时**先注册的赢**
 *    （`uvcpp_web_router.cpp` 里那句「compare_specificity 是全序，所以结果与
 *    注册顺序无关」指的是不同模式之间；**同模式**打平就只剩注册序了）。
 *    所以第二次 `uvcpp_wsdl_serve()` 是**静默无效**的，这一点必须有用例钉住，
 *    否则 `serve.h` 的文件头注释将来会被人按"后注册的覆盖前一条"改回去 ——
 *    那样改出来的是一个"以为换掉了、其实没换"的静默错。
 *
 * 与之配对的是**正对照**：一个**新的**路径拿同一个源，必须发出新内容。少了
 * 这一条，"发的是旧内容"就可能被解释成"新内容根本没生效"。
 *
 * 刻意**不做**的一件事：启动之后再改源。那不是"没测"，是**按 C++ 的规则本来
 * 就是数据竞争**（源里的 `shared_ptr` 被主线程写、被循环线程读），所以本用例
 * 把所有的改动都放在 `start_background()` 之前 —— 判据只覆盖文档承诺的那条
 * 语义，不把竞争行为固化成"约定"。
 */
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WSDL_ENABLE

#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <wsdl/uvcpp_wsdl_document.h>
#include <wsdl/uvcpp_wsdl_serve.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

/** 测试用的 App 骨架（与 `web_app_app_func.cpp` 同一套）。 */
void configure_for_test(uvcpp_web_app& app) {
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(log_level::WARN);
}

/** 同步往返一次（沿用 `web_app_app_func.cpp` 的写法与重试理由）。 */
bool get(int port, const std::string& path, uvcpp_http_response& resp) {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) == 0 &&
        client.send_wait(uvcpp_http_request::make_get(path), resp, 3000) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

std::string body_of(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

int status_of(const uvcpp_http_response& r) {
  return static_cast<int>(r.status_code);
}

/** 一份最小的合法 WSDL（够 dump 出一个像样的文档就行）。 */
const char* k_svc =
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "             xmlns:tns=\"urn:svc\" targetNamespace=\"urn:svc\" name=\"Svc\">\n"
    "  <message name=\"PingReq\"><part name=\"t\" type=\"tns:T\"/></message>\n"
    "  <portType name=\"P\">\n"
    "    <operation name=\"Ping\"><input message=\"tns:PingReq\"/></operation>\n"
    "  </portType>\n"
    "  <binding name=\"B\" type=\"tns:P\">\n"
    "    <soap:binding style=\"document\"/>\n"
    "    <operation name=\"Ping\"><input><soap:body use=\"literal\"/></input></operation>\n"
    "  </binding>\n"
    "  <service name=\"S\">\n"
    "    <port name=\"P1\" binding=\"tns:B\"><soap:address location=\"urn:svc\"/></port>\n"
    "  </service>\n"
    "</definitions>\n";

uvcpp_wsdl_document parse_svc() {
  uvcpp_wsdl_document d;
  const wsdl_status st = uvcpp_wsdl_parse(std::string(k_svc), d);
  if (st != wsdl_status::OK) {
    std::cerr << "  [FAIL] 样例 WSDL 解析失败：" << wsdl_status_name(st)
              << std::endl;
    ++g_failures;
  }
  return d;
}

// =========================================================================
// 1. 装配 + 三种发法
// =========================================================================

void test_serve_and_send() {
  uvcpp_web_app app;
  configure_for_test(app);

  const uvcpp_wsdl_document doc = parse_svc();
  const std::string bytes = uvcpp_wsdl_dump(doc);
  if (g_failures != 0) return;

  // (a) 便利入口：注册一条 GET 路由。
  uvcpp_wsdl_serve(app, "/svc.wsdl", doc);

  // (b) 源对象**放在作用域里**，出了作用域就析构 —— 路由必须照发不误
  //     （这就是"按值捕获共享指针"那条语义的判据）。
  {
    uvcpp_wsdl_source scoped(doc);
    uvcpp_wsdl_serve(app, "/scoped.wsdl", scoped);
    check_eq_i(static_cast<long long>(scoped.size()),
               static_cast<long long>(bytes.size()), "1.1 source::size()");
    check(!scoped.empty() && scoped.text() == bytes, "1.2 source::text()");
  }

  // (c) 自己写 handler、直接发（运行时可控的那条路）。
  const uvcpp_wsdl_source live(doc);
  app.get("/live.wsdl", [live](uvcpp_web_request&, uvcpp_web_response& resp,
                               uvcpp_web_next next) {
    (void)next;
    uvcpp_wsdl_send(live, resp);
    resp.end();
  });

  // (d) 空源：不崩，发一个空正文 + 正确的 content-type。
  uvcpp_wsdl_source empty;
  check(empty.empty() && empty.size() == 0, "1.3 空源的 empty()/size()");
  check(empty.text().empty(), "1.4 空源的 text() 是空串（不是 nullptr）");
  check(!empty.shared(), "1.5 空源的 shared() 是空指针");
  app.get("/empty.wsdl", [empty](uvcpp_web_request&, uvcpp_web_response& resp,
                                 uvcpp_web_next next) {
    (void)next;
    uvcpp_wsdl_send(empty, resp);
    resp.end();
  });

  check_eq_s(std::string(uvcpp_wsdl_content_type()), "text/xml; charset=utf-8",
             "1.6 content-type 常量");

  check(app.start_background() == 0, "1.7 start_background()");
  const int port = app.bound_port();
  if (port <= 0) {
    check(false, "1.8 端口没起来");
    return;
  }

  const char* paths[] = {"/svc.wsdl", "/scoped.wsdl", "/live.wsdl"};
  const char* names[] = {"1.9 /svc.wsdl", "1.10 /scoped.wsdl（源已析构）",
                         "1.11 /live.wsdl（handler 里直发）"};
  for (int i = 0; i < 3; ++i) {
    uvcpp_http_response r;
    if (!get(port, paths[i], r)) {
      check(false, std::string(names[i]) + "：请求没回来");
      continue;
    }
    check_eq_i(status_of(r), 200, std::string(names[i]) + " 状态码");
    check_eq_s(http_get_header(r.headers, "content-type"),
               "text/xml; charset=utf-8", std::string(names[i]) + " content-type");
    check_eq_i(static_cast<long long>(body_of(r).size()),
               static_cast<long long>(bytes.size()),
               std::string(names[i]) + " 正文长度");
    check_eq_s(body_of(r), bytes, std::string(names[i]) + " 正文逐字节相同");
  }

  // 发出去的字节必须能**被自己解析回来**（否则等于发了一份没人能用的 WSDL）。
  {
    uvcpp_http_response r;
    if (get(port, "/svc.wsdl", r)) {
      uvcpp_wsdl_document back;
      const wsdl_status st = uvcpp_wsdl_parse(body_of(r), back);
      check(st == wsdl_status::OK, "1.12 发出去的字节能重新解析");
      check_eq_s(back.find_service("S") != nullptr ? "S" : "<null>", "S",
                 "1.13 重新解析出的模型里有那个 service");
      check_eq_s(uvcpp_wsdl_dump(back), bytes, "1.14 再 dump 一次仍然相同");
    } else {
      check(false, "1.12 取正文失败");
    }
  }

  // 空源：200 + 空正文。
  {
    uvcpp_http_response r;
    if (get(port, "/empty.wsdl", r)) {
      check_eq_i(status_of(r), 200, "1.15 空源的状态码");
      check_eq_s(http_get_header(r.headers, "content-type"),
                 "text/xml; charset=utf-8", "1.16 空源的 content-type");
      check_eq_s(body_of(r), std::string(), "1.17 空源的正文是空的");
      check_eq_s(http_get_header(r.headers, "content-length"), "0",
                 "1.18 空源的长度是 0（不是没这个头）");
    } else {
      check(false, "1.15 空源请求没回来");
    }
  }

  // 没注册过的路径：404（对照：上面那几条都是 200，所以这条不是"什么都 404"）。
  {
    uvcpp_http_response r;
    if (get(port, "/nope.wsdl", r)) {
      check_eq_i(status_of(r), 404, "1.19 没注册的路径 404");
    } else {
      check(false, "1.19 404 请求没回来");
    }
  }

  app.stop();
  app.join();
}

// =========================================================================
// 2. 注册之后改源 —— 行为与文档必须逐条对齐
// =========================================================================

void test_source_semantics() {
  uvcpp_web_app app;
  configure_for_test(app);

  const uvcpp_wsdl_document doc = parse_svc();
  const std::string old_bytes = uvcpp_wsdl_dump(doc);
  if (g_failures != 0) return;

  uvcpp_wsdl_source src(doc);

  // 先注册 A 版。
  uvcpp_wsdl_serve(app, "/fixed.wsdl", src);

  // 注册**之后**把源换成 B 版。
  const std::string new_bytes =
      "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
      " targetNamespace=\"urn:other\"/>";
  src.set_from_text(new_bytes);
  check_eq_s(src.text(), new_bytes, "2.1 set_from_text 换掉了源自己的内容");

  // 同一个路径**再注册一次**：静默无效（路由此刻仍指向 A 版那块字节）。
  uvcpp_wsdl_serve(app, "/fixed.wsdl", src);

  // 正对照：一个**新**路径拿同一个源 —— 必须发 B 版。
  uvcpp_wsdl_serve(app, "/fresh.wsdl", src);

  // 直接发那条路也读同一个源。
  app.get("/live2.wsdl", [src](uvcpp_web_request&, uvcpp_web_response& resp,
                               uvcpp_web_next next) {
    (void)next;
    uvcpp_wsdl_send(src, resp);
    resp.end();
  });

  check(app.start_background() == 0, "2.2 start_background()");
  const int port = app.bound_port();
  if (port <= 0) {
    check(false, "2.3 端口没起来");
    return;
  }

  {
    uvcpp_http_response r;
    if (get(port, "/fixed.wsdl", r)) {
      check_eq_i(status_of(r), 200, "2.4 /fixed.wsdl 状态码");
      // ★ 这一条同时钉两件事：注册时按值捕获了共享指针；重复注册不生效。
      check_eq_s(body_of(r), old_bytes,
                 "2.5 注册后再改源、再注册同一路径，发的仍是**注册那一刻**那份");
      check(old_bytes != new_bytes, "2.6（前提）两份字节确实不同");
    } else {
      check(false, "2.4 /fixed.wsdl 请求没回来");
    }
  }
  {
    uvcpp_http_response r;
    if (get(port, "/fresh.wsdl", r)) {
      check_eq_s(body_of(r), new_bytes,
                 "2.7 正对照：**新**路径拿同一个源，发的是新内容");
    } else {
      check(false, "2.7 /fresh.wsdl 请求没回来");
    }
  }
  {
    uvcpp_http_response r;
    if (get(port, "/live2.wsdl", r)) {
      check_eq_s(body_of(r), new_bytes,
                 "2.8 自己写 handler 直发那条路读到的是源的当前内容");
    } else {
      check(false, "2.8 /live2.wsdl 请求没回来");
    }
  }

  app.stop();
  app.join();
}

}  // namespace

int main() {
  std::cout << "[web_app_wsdl] " << std::flush;
  test_serve_and_send();
  test_source_semantics();

  if (g_failures == 0) {
    std::cout << "ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "FAIL (" << g_failures << ")" << std::endl;
  return 2;
}

#else  // UVCPP_WSDL_ENABLE

// 关掉 wsdl 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它）。这里返回非零：真编到了就是一个必须修的配置错。
int main() {
  std::cerr << "[web_app_wsdl] UVCPP_WSDL_ENABLE=0 —— 这个测试文件不该被"
               "编译进来" << std::endl;
  return 2;
}

#endif  // UVCPP_WSDL_ENABLE
