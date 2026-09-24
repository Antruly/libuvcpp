/**
 * @file tests/functional/web_app_soap_func.cpp
 * @brief SOAP 路由层：真端口、真连接、真客户端、真 WSDL。
 *
 * 与 `soap_message_func.cpp` 的分工：那边钉**信封**的字段与字节，这边钉
 * **WSDL 与报文怎么对上**、以及"该回哪个码"。判据分六组：
 *
 * 1. **装配**（0.x）：从一份 WSDL 挑出 service/port，收 operation，推出派发键。
 *    最要紧的两条是 **0.21**（rpc 的命名空间是键的一部分）与 **0.43**（同一个键
 *    出现两次时最后一条赢）—— 两者都是"错了不报、只是收不到调用"的形状。
 * 2. **派发与响应**（1.x）：Body 元素对上 operation、响应包装元素从 WSDL 推、
 *    处理函数的三种结局各回什么。含 **1.35~1.38**（同一个语义码在两个版本下写成
 *    不同的名字，状态码也不同）。
 * 3. **准入与解析失败**（2.x）：解析失败**全部**算 `Sender`（除了端点没装配好）。
 *    含 **2.14**（解析失败时版本只能靠 Content-Type 判 ⇒ 1.2 的客户端拿到 400）。
 * 4. **版本策略**（3.x）：`VersionMismatch` 这条码唯一用得到的地方，以及**内容类型
 *    与信封不一致**时以谁为准（3.8~3.11）。
 * 5. **动作值**（4.x）：三条规则（声明了非空值才核对 / 只认非空值 / 按版本选读哪个
 *    来源），两边都钉了"来源选错会怎样"（4.13~4.18）。
 * 6. **单向 operation**（5.x）。
 *
 * 期望字节怎么来：信封那几层的逐字节基准在 `soap_message_func.cpp` 里钉着，所以这里
 * 大多数判据的期望值写成 `uvcpp_soap_dump_*(版本, 元素名, 命名空间, 内容)` —— 参数是
 * **独立写出来的**，所以它照样验得了"这一层有没有把对的参数传下去"，而不用把同一份
 * 信封抄第 N 遍。另外三条主路径（1.1 响应 / 1.1 Fault / 1.2 Fault）各有一条**整段
 * 字面量**的判据，让这个文件自己也能证明线上的字节长什么样。
 */
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WSDL_ENABLE

#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <wsdl/uvcpp_soap_message.h>
#include <wsdl/uvcpp_soap_service.h>
#include <wsdl/uvcpp_wsdl_document.h>

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

/** 两个值"必须不一样"时用它 —— 否则一条恒真的判据看起来也像在钉东西。 */
void check_ne_s(const std::string& a, const std::string& b,
                const std::string& what) {
  if (a == b) {
    std::cerr << "  [FAIL] " << what << "（两边相同：" << a << "）" << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 样例 WSDL 与样例报文
// =========================================================================

const char* k_env11 = "http://schemas.xmlsoap.org/soap/envelope/";
const char* k_env12 = "http://www.w3.org/2003/05/soap-envelope";

const char* k_ct11 = "text/xml; charset=utf-8";
const char* k_ct12 = "application/soap+xml; charset=utf-8";

const char* k_path = "/calc";

/** document/literal：四条 portType operation，其中 `Ghost` **没有** binding 那条。 */
const char* k_doc =
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "             xmlns:tns=\"urn:calc\" targetNamespace=\"urn:calc\" name=\"Calc\">\n"
    "  <message name=\"AddIn\"><part name=\"p\" element=\"tns:Add\"/></message>\n"
    "  <message name=\"AddOut\"><part name=\"p\" element=\"tns:AddResponse\"/></message>\n"
    "  <message name=\"PingIn\"><part name=\"p\" element=\"tns:Ping\"/></message>\n"
    "  <message name=\"PingOut\"><part name=\"p\" element=\"tns:PingResponse\"/></message>\n"
    "  <message name=\"NotifyIn\"><part name=\"p\" element=\"tns:Notify\"/></message>\n"
    "  <message name=\"GhostIn\"><part name=\"p\" element=\"tns:Ghost\"/></message>\n"
    "  <portType name=\"CalcPort\">\n"
    "    <operation name=\"Add\"><input message=\"tns:AddIn\"/><output message=\"tns:AddOut\"/></operation>\n"
    "    <operation name=\"Ping\"><input message=\"tns:PingIn\"/><output message=\"tns:PingOut\"/></operation>\n"
    "    <operation name=\"Notify\"><input message=\"tns:NotifyIn\"/></operation>\n"
    "    <operation name=\"Ghost\"><input message=\"tns:GhostIn\"/><output message=\"tns:AddOut\"/></operation>\n"
    "  </portType>\n"
    "  <binding name=\"CalcBinding\" type=\"tns:CalcPort\">\n"
    "    <soap:binding style=\"document\" transport=\"http://schemas.xmlsoap.org/soap/http\"/>\n"
    "    <operation name=\"Add\">\n"
    "      <soap:operation soapAction=\"urn:calc#Add\"/>\n"
    "      <input><soap:body use=\"literal\"/></input>\n"
    "      <output><soap:body use=\"literal\"/></output>\n"
    "    </operation>\n"
    "    <operation name=\"Ping\">\n"
    "      <soap:operation soapAction=\"urn:calc#Ping\"/>\n"
    "      <input><soap:body use=\"literal\"/></input>\n"
    "      <output><soap:body use=\"literal\"/></output>\n"
    "    </operation>\n"
    "    <operation name=\"Notify\">\n"
    "      <input><soap:body use=\"literal\"/></input>\n"
    "    </operation>\n"
    "  </binding>\n"
    "  <service name=\"CalcService\">\n"
    "    <port name=\"CalcPort1\" binding=\"tns:CalcBinding\">\n"
    "      <soap:address location=\"urn:calc\"/>\n"
    "    </port>\n"
    "  </service>\n"
    "</definitions>\n";

/** rpc/literal：part 只有 `@type`，所以派发键是 `{soap:body/@namespace}操作名`。 */
const char* k_rpc =
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "             xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"\n"
    "             xmlns:tns=\"urn:calc\" targetNamespace=\"urn:calc\" name=\"Rpc\">\n"
    "  <message name=\"MulIn\"><part name=\"a\" type=\"xsd:int\"/><part name=\"b\" type=\"xsd:int\"/></message>\n"
    "  <message name=\"MulOut\"><part name=\"result\" type=\"xsd:int\"/></message>\n"
    "  <portType name=\"RpcPort\">\n"
    "    <operation name=\"Mul\"><input message=\"tns:MulIn\"/><output message=\"tns:MulOut\"/></operation>\n"
    "  </portType>\n"
    "  <binding name=\"RpcBinding\" type=\"tns:RpcPort\">\n"
    "    <soap:binding style=\"rpc\" transport=\"http://schemas.xmlsoap.org/soap/http\"/>\n"
    "    <operation name=\"Mul\">\n"
    "      <soap:operation soapAction=\"urn:calc#Mul\"/>\n"
    "      <input><soap:body use=\"literal\" namespace=\"urn:calc\"/></input>\n"
    "      <output><soap:body use=\"literal\" namespace=\"urn:calc\"/></output>\n"
    "    </operation>\n"
    "  </binding>\n"
    "  <service name=\"RpcService\">\n"
    "    <port name=\"RpcPort1\" binding=\"tns:RpcBinding\"><soap:address location=\"urn:calc\"/></port>\n"
    "  </service>\n"
    "</definitions>\n";

/** 两份 service；第一份底下**两个** SOAP port（用来分别钉"点名"的两条错法）。 */
const char* k_multi =
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "             xmlns:tns=\"urn:multi\" targetNamespace=\"urn:multi\">\n"
    "  <message name=\"M\"><part name=\"p\" element=\"tns:Add\"/></message>\n"
    "  <portType name=\"P\">\n"
    "    <operation name=\"Add\"><input message=\"tns:M\"/><output message=\"tns:M\"/></operation>\n"
    "  </portType>\n"
    "  <binding name=\"B\" type=\"tns:P\">\n"
    "    <soap:binding style=\"document\"/>\n"
    "    <operation name=\"Add\"><input><soap:body use=\"literal\"/></input>\n"
    "      <output><soap:body use=\"literal\"/></output></operation>\n"
    "  </binding>\n"
    "  <service name=\"Svc1\">\n"
    "    <port name=\"P1a\" binding=\"tns:B\"><soap:address location=\"urn:multi\"/></port>\n"
    "    <port name=\"P1b\" binding=\"tns:B\"><soap:address location=\"urn:multi\"/></port>\n"
    "  </service>\n"
    "  <service name=\"Svc2\">\n"
    "    <port name=\"P2\" binding=\"tns:B\"><soap:address location=\"urn:multi\"/></port>\n"
    "  </service>\n"
    "</definitions>\n";

/** 有 portType、有 binding、**没有 soap:binding** —— 一个纯 HTTP 的 port。 */
const char* k_http_only =
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:tns=\"urn:plain\" targetNamespace=\"urn:plain\">\n"
    "  <message name=\"M\"><part name=\"p\" element=\"tns:Add\"/></message>\n"
    "  <portType name=\"P\">\n"
    "    <operation name=\"Add\"><input message=\"tns:M\"/><output message=\"tns:M\"/></operation>\n"
    "  </portType>\n"
    "  <binding name=\"B\" type=\"tns:P\">\n"
    "    <operation name=\"Add\"><input/><output/></operation>\n"
    "  </binding>\n"
    "  <service name=\"S\">\n"
    "    <port name=\"P1\" binding=\"tns:B\"/>\n"
    "  </service>\n"
    "</definitions>\n";

/** 两个坏引用：P1 指不到 binding，P2 指的 binding 指不到 portType。 */
const char* k_bad_refs =
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "             xmlns:tns=\"urn:bad\" targetNamespace=\"urn:bad\">\n"
    "  <binding name=\"B\" type=\"tns:Nope\">\n"
    "    <soap:binding style=\"document\"/>\n"
    "  </binding>\n"
    "  <service name=\"S\">\n"
    "    <port name=\"P1\" binding=\"tns:Nope\"><soap:address location=\"urn:bad\"/></port>\n"
    "    <port name=\"P2\" binding=\"tns:B\"><soap:address location=\"urn:bad\"/></port>\n"
    "  </service>\n"
    "</definitions>\n";

/** 两条 operation 推出**同一个**派发键（坏文档，但 XML 完全合法）。 */
const char* k_dup_key =
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "             xmlns:tns=\"urn:dup\" targetNamespace=\"urn:dup\">\n"
    "  <message name=\"M1\"><part name=\"p\" element=\"tns:Add\"/></message>\n"
    "  <message name=\"M2\"><part name=\"p\" element=\"tns:Add\"/></message>\n"
    "  <portType name=\"P\">\n"
    "    <operation name=\"Add\"><input message=\"tns:M1\"/><output message=\"tns:M1\"/></operation>\n"
    "    <operation name=\"Plus\"><input message=\"tns:M2\"/><output message=\"tns:M2\"/></operation>\n"
    "  </portType>\n"
    "  <binding name=\"B\" type=\"tns:P\">\n"
    "    <soap:binding style=\"document\"/>\n"
    "    <operation name=\"Add\"><input><soap:body use=\"literal\"/></input>\n"
    "      <output><soap:body use=\"literal\"/></output></operation>\n"
    "    <operation name=\"Plus\"><input><soap:body use=\"literal\"/></input>\n"
    "      <output><soap:body use=\"literal\"/></output></operation>\n"
    "  </binding>\n"
    "  <service name=\"S\">\n"
    "    <port name=\"P1\" binding=\"tns:B\"><soap:address location=\"urn:dup\"/></port>\n"
    "  </service>\n"
    "</definitions>\n";

std::string env11(const std::string& inner) {
  return std::string("<soap:Envelope xmlns:soap=\"") + k_env11 +
         "\"><soap:Body>" + inner + "</soap:Body></soap:Envelope>";
}

std::string env12(const std::string& inner) {
  return std::string("<soap:Envelope xmlns:soap=\"") + k_env12 +
         "\"><soap:Body>" + inner + "</soap:Body></soap:Envelope>";
}

/** 1.1 带 Header 的信封（`header` 是 Header 的**内容**）。 */
std::string env11h(const std::string& header, const std::string& inner) {
  return std::string("<soap:Envelope xmlns:soap=\"") + k_env11 +
         "\"><soap:Header>" + header + "</soap:Header><soap:Body>" + inner +
         "</soap:Body></soap:Envelope>";
}

/** `{urn:calc}Add` 那个包装元素，带两个参数。 */
const char* k_add = "<tns:Add xmlns:tns=\"urn:calc\"><a>1</a><b>2</b></tns:Add>";
/** `{urn:calc}Notify`（单向那条）的 Body。 */
const char* k_notify =
    "<tns:Notify xmlns:tns=\"urn:calc\"><m>hi</m></tns:Notify>";

// =========================================================================
// 处理函数与它看到的东西
// =========================================================================

/** 处理函数要演的那几种结局。 */
enum class behavior {
  REPLY,              ///< 给一个正常响应
  REPLY_NAMED,        ///< 给一个响应，但包装元素自己指定
  SILENT,             ///< 什么都不给
  FAULT_SERVER,       ///< 报 `SERVER`（1.2 下是 `Receiver`）
  FAULT_CLIENT,       ///< 报 `CLIENT`（1.2 下是 `Sender`）
  FAULT_CUSTOM,       ///< 端一条整的、码是自定义的
  FAULT_BAD_ENCODING  ///< 报 `DATA_ENCODING_UNKNOWN`（1.1 里不存在）
};

behavior g_behavior = behavior::REPLY;

const char* k_reply_inner = "<result>3</result>";
const char* k_fault_reason = "boom";

struct call_log {
  int calls = 0;
  uvcpp_soap_version version = uvcpp_soap_version::V1_1;
  std::string operation;
  std::string action;
  bool action_present = false;
  std::string body_local;
  std::string body_ns;
  std::string body_xml;
  std::string header_xml;
  size_t header_entries = 0;
  // 只给 `probe` 那一支用：`arg()` / `child_xml()`
  std::string probe_arg_a;
  std::string probe_arg_missing;
  std::string probe_raw_a;
  std::string probe_raw_missing;
};

call_log g_log;

void reset_log() { g_log = call_log(); }

/** 默认的处理函数：把看到的东西记下来，再按 `g_behavior` 演一种结局。 */
void handler(uvcpp_soap_call& c, uvcpp_soap_reply& r) {
  ++g_log.calls;
  g_log.version = c.version;
  g_log.operation = c.operation;
  g_log.action = c.action;
  g_log.action_present = c.action_present;
  g_log.body_local = c.body_local;
  g_log.body_ns = c.body_ns;
  g_log.body_xml = c.body_xml;
  g_log.header_xml = c.header_xml;
  g_log.header_entries = c.header_entries;

  switch (g_behavior) {
    case behavior::REPLY:
      r.set_response(k_reply_inner);
      return;
    case behavior::REPLY_NAMED:
      r.set_response_named("Custom", "urn:x", "<v/>");
      return;
    case behavior::SILENT:
      return;
    case behavior::FAULT_SERVER:
      r.set_fault(uvcpp_soap_fault_code::SERVER, k_fault_reason);
      return;
    case behavior::FAULT_CLIENT:
      r.set_fault(uvcpp_soap_fault_code::CLIENT, k_fault_reason);
      return;
    case behavior::FAULT_CUSTOM: {
      uvcpp_soap_fault f;
      f.code = "BadArgs";
      f.reason = k_fault_reason;
      r.set_fault(f);
      return;
    }
    case behavior::FAULT_BAD_ENCODING:
      r.set_fault(uvcpp_soap_fault_code::DATA_ENCODING_UNKNOWN, k_fault_reason);
      return;
  }
}

/** 只记 `arg()` / `child_xml()` 的结果，回一个空内容。 */
void probe(uvcpp_soap_call& c, uvcpp_soap_reply& r) {
  ++g_log.calls;
  g_log.operation = c.operation;
  g_log.probe_arg_a = c.arg("a");
  g_log.probe_arg_missing = c.arg("zzz");
  g_log.probe_raw_a = c.child_xml("a");
  g_log.probe_raw_missing = c.child_xml("zzz");
  r.set_response(std::string());
}

// =========================================================================
// 装置
// =========================================================================

struct rig {
  uvcpp_web_app app;
  std::shared_ptr<uvcpp_soap_service> ep;
  int port = 0;

  /** 解析 + 装配（不起服务），给每个 operation 装默认处理函数。
   *  @return 样例 WSDL 解析成功。 */
  bool load(const char* wsdl, const std::vector<std::string>& ops,
            const std::string& service = std::string(),
            const std::string& port_name = std::string()) {
    uvcpp_wsdl_document doc;
    const wsdl_status st = uvcpp_wsdl_parse(std::string(wsdl), doc);
    if (st != wsdl_status::OK) {
      std::cerr << "  [FAIL] 样例 WSDL 解析失败：" << wsdl_status_name(st)
                << std::endl;
      ++g_failures;
      return false;
    }
    ep = std::make_shared<uvcpp_soap_service>(doc, service, port_name);
    for (size_t i = 0; i < ops.size(); ++i) ep->operation(ops[i], handler);
    return true;
  }

  /** 装路由 + 起服务。 */
  bool serve(const std::string& path = std::string(k_path)) {
    uvcpp_soap_serve(app, path, ep);
    app.set_host("127.0.0.1")
        .set_port(0)
        .set_access_log(false)
        .set_log_level(log_level::WARN);
    if (app.start_background() != 0) return false;
    port = app.bound_port();
    return port > 0;
  }

  void down() {
    app.stop();
    app.join();
  }
};

/** 同步往返一次 POST（沿用 `web_app_wsdl_func.cpp` 的重试写法与理由）。 */
bool post(int port, const std::string& path, const std::string& body,
          const std::string& ct, const char* soap_action,
          uvcpp_http_response& resp) {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) == 0) {
      uvcpp_http_request req =
          uvcpp_http_request::make_post(path, body.data(), body.size(), ct);
      if (soap_action != nullptr) req.set_header("SOAPAction", soap_action);
      if (client.send_wait(req, resp, 3000) == 0) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

/** 1.1 的一次 POST（不带 `SOAPAction` 头）。 */
bool p11(int port, const std::string& body, uvcpp_http_response& resp) {
  return post(port, k_path, body, k_ct11, nullptr, resp);
}

/** 1.1 的一次 POST，带 `SOAPAction` 头。 */
bool p11a(int port, const std::string& body, const char* action,
          uvcpp_http_response& resp) {
  return post(port, k_path, body, k_ct11, action, resp);
}

/** 1.2 的一次 POST（不带 `action=` 参数）。 */
bool p12(int port, const std::string& body, uvcpp_http_response& resp) {
  return post(port, k_path, body, k_ct12, nullptr, resp);
}

std::string body_of(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

int status_of(const uvcpp_http_response& r) {
  return static_cast<int>(r.status_code);
}

std::string ct_of(const uvcpp_http_response& r) {
  return http_get_header(r.headers, "content-type");
}

/** 期望的一条 Fault 响应（整段信封）。 */
std::string fault_bytes(uvcpp_soap_version v, uvcpp_soap_fault_code code,
                        const std::string& reason) {
  return uvcpp_soap_dump_fault(uvcpp_soap_make_fault(v, code, reason));
}

/** 期望的一条正常响应（整段信封）。 */
std::string response_bytes(uvcpp_soap_version v, const std::string& local,
                           const std::string& ns, const std::string& inner) {
  return uvcpp_soap_dump_response(v, local, ns, inner);
}

/** `Add` / `Ping` / `Notify` 三条（`k_doc` 上能派发的全部）。 */
const std::vector<std::string>& ops3() {
  static const std::vector<std::string> v = {"Add", "Ping", "Notify"};
  return v;
}

// =========================================================================
// 0. 装配（不联网）
// =========================================================================

void test_assembly() {
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    check(r.ep->valid(), "0.1 不点名也能装配（一份 service、一个 SOAP port）");
    check_eq_s(r.ep->service_name(), "CalcService", "0.2 service 名");
    check_eq_s(r.ep->port_name(), "CalcPort1", "0.3 port 名");
    check_eq_s(r.ep->binding_name(), "CalcBinding", "0.4 binding 名");
    check_eq_s(r.ep->port_type_name(), "CalcPort", "0.5 portType 名");
    check_eq_s(r.ep->target_namespace(), "urn:calc", "0.6 targetNamespace");
    check_eq_s(std::string(r.ep->why()), "ok", "0.7 可用的端点 why() 是 ok");
  }

  // operation 表从 **binding** 收，顺序按 WSDL。
  {
    rig r;
    if (!r.load(k_doc, std::vector<std::string>())) return;
    check_eq_i(static_cast<long long>(r.ep->operation_count()), 3,
               "0.8 只收 binding 里的三条（portType 里有四条）");
    const std::vector<std::string> names = r.ep->operation_names();
    check_eq_i(static_cast<long long>(names.size()), 3, "0.9 operation_names 长度");
    if (names.size() == 3) {
      check_eq_s(names[0], "Add", "0.10 顺序 1/3");
      check_eq_s(names[1], "Ping", "0.11 顺序 2/3");
      check_eq_s(names[2], "Notify", "0.12 顺序 3/3");
    }
    check(!r.ep->has_operation("Ghost"),
          "0.13 portType 里那条没有 binding 的 operation 收不进来");
    check(r.ep->has_operation("Add"), "0.14 has_operation 认名字");
    check(!r.ep->has_operation("Nope"), "0.15 不认识的名字是假");
  }

  // 派发键（document：part 的 `@element`）。
  {
    rig r;
    if (!r.load(k_doc, std::vector<std::string>())) return;
    check_eq_s(r.ep->dispatch_target_of("{urn:calc}Add"), "Add",
               "0.16 document 的键是 part 的 @element");
    check_eq_s(r.ep->dispatch_target_of("{urn:calc}AddResponse"), "",
               "0.17 输出那个元素不是键");
    check_eq_s(r.ep->dispatch_target_of("{}Add"), "",
               "0.18 少了命名空间就不是同一个键");
  }

  // 派发键（rpc：`{soap:body/@namespace}` + 操作名）。
  {
    rig r;
    if (!r.load(k_rpc, std::vector<std::string>())) return;
    check(r.ep->valid(), "0.19 rpc 的样例装配得上");
    check_eq_s(r.ep->dispatch_target_of("{urn:calc}Mul"), "Mul",
               "0.20 rpc 的键是 soap:body/@namespace + 操作名");
    // ★ 名字对、命名空间不对 —— 键是 QName，命名空间是键的一部分。
    check_eq_s(r.ep->dispatch_target_of("{urn:other}Mul"), "",
               "0.21 ★ 命名空间不对就不是这个端点认识的 operation");
  }

  // 挑不到的那些情形，各自有自己的一句话。
  {
    rig r;
    if (!r.load(k_doc, std::vector<std::string>(), "Nope")) return;
    check(!r.ep->valid(), "0.22 点了一个不存在的 service 名字");
    check_eq_s(std::string(r.ep->why()),
               "no service with that name in this WSDL", "0.23 它说了为什么");
  }
  {
    rig r;
    if (!r.load(k_doc, std::vector<std::string>(), "CalcService", "Nope")) return;
    check(!r.ep->valid(), "0.24 点了一个不存在的 port 名字");
    check_eq_s(std::string(r.ep->why()),
               "no port with that name in that service", "0.25 它说了为什么");
  }
  {
    rig r;
    if (!r.load(k_multi, std::vector<std::string>())) return;
    check(!r.ep->valid(), "0.26 两份 service 而不点名");
    check_eq_s(std::string(r.ep->why()),
               "the WSDL declares several services; name one", "0.27 它说了为什么");
  }
  {
    rig r;
    if (!r.load(k_multi, std::vector<std::string>(), "Svc1")) return;
    check(!r.ep->valid(), "0.28 一个 service 下两个 SOAP port 而不点名");
    check_eq_s(std::string(r.ep->why()),
               "that service has several SOAP ports; name one", "0.29 它说了为什么");
  }
  {
    rig r;
    if (!r.load(k_multi, std::vector<std::string>(), "Svc1", "P1a")) return;
    check(r.ep->valid(), "0.30 点名了就装配得上（正对照）");
    check_eq_s(r.ep->port_name(), "P1a", "0.31 取的是点名那个 port");
  }
  {
    rig r;
    if (!r.load(k_multi, std::vector<std::string>(), "Svc2")) return;
    check(r.ep->valid(), "0.32 另一份 service 只有一个 SOAP port，不点名也行");
  }
  {
    rig r;
    if (!r.load(k_http_only, std::vector<std::string>())) return;
    check(!r.ep->valid(), "0.33 整个 service 没有一个绑 SOAP 的 port");
    check_eq_s(std::string(r.ep->why()),
               "that service has no port whose binding is a SOAP binding",
               "0.34 它说了为什么");
  }
  {
    rig r;
    if (!r.load(k_http_only, std::vector<std::string>(), std::string(), "P1"))
      return;
    check(!r.ep->valid(), "0.35 点名的那个 port 的 binding 不是 SOAP");
    check_eq_s(std::string(r.ep->why()),
               "that port's binding is not a SOAP binding", "0.36 它说了为什么");
  }
  {
    rig r;
    if (!r.load(k_bad_refs, std::vector<std::string>(), std::string(), "P1"))
      return;
    check(!r.ep->valid(), "0.37 port 指不到 binding");
    check_eq_s(std::string(r.ep->why()),
               "the port refers to a binding that is not in this WSDL",
               "0.38 它说了为什么");
  }
  {
    rig r;
    if (!r.load(k_bad_refs, std::vector<std::string>(), std::string(), "P2"))
      return;
    check(!r.ep->valid(), "0.39 binding 指不到 portType");
    check_eq_s(std::string(r.ep->why()),
               "the binding refers to a portType that is not in this WSDL",
               "0.40 它说了为什么");
  }

  // 同一个派发键出现两次：最后一条赢（头文件里记着这条是**静默**的）。
  {
    rig r;
    if (!r.load(k_dup_key, std::vector<std::string>())) return;
    check(r.ep->valid(), "0.41 这份坏文档也装配得起来");
    check_eq_i(static_cast<long long>(r.ep->operation_count()), 2,
               "0.42 两条 operation 都收进来了");
    check_eq_s(r.ep->dispatch_target_of("{urn:dup}Add"), "Plus",
               "0.43 ★ 同一个键出现两次：最后一条赢");
  }
}

// =========================================================================
// 1. 派发与响应
// =========================================================================

void test_dispatch() {
  // (a) document/literal 的一次成功调用（响应逐字节）
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    reset_log();
    if (!r.serve()) {
      check(false, "1.1 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11a(r.port, env11(k_add), "\"urn:calc#Add\"", resp)) {
      check_eq_i(status_of(resp), 200, "1.1 状态码");
      check_eq_s(ct_of(resp), "text/xml; charset=utf-8",
                 "1.2 content-type 是 1.1 那一版");
      check_eq_s(body_of(resp),
                 "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
                 "  <soap:Body>\n"
                 "    <AddResponse xmlns=\"urn:calc\">\n"
                 "      <result>3</result>\n"
                 "    </AddResponse>\n"
                 "  </soap:Body>\n"
                 "</soap:Envelope>\n",
                 "1.3 响应逐字节（包装元素是从 WSDL 的 output part 推出来的）");
      check_eq_i(static_cast<long long>(g_log.calls), 1, "1.4 处理函数被调了一次");
      check_eq_s(g_log.operation, "Add", "1.5 派发到的 operation 名");
      check(g_log.version == uvcpp_soap_version::V1_1, "1.6 版本是 1.1");
      check_eq_s(g_log.body_local, "Add", "1.7 Body 元素的局部名");
      check_eq_s(g_log.body_ns, "urn:calc", "1.8 Body 元素的命名空间");
      check_eq_s(g_log.body_xml, k_add, "1.9 Body 元素整段原样交给处理函数");
      check_eq_s(g_log.action, "urn:calc#Add", "1.10 动作值归一后（少了引号）");
      check(g_log.action_present, "1.11 动作值被标成「提供了」");
      check_eq_i(static_cast<long long>(g_log.header_entries), 0, "1.12 没有 Header");
      check_eq_s(g_log.header_xml, std::string(), "1.13 header_xml 是空串");
    } else {
      check(false, "1.1 请求没回来");
    }
    r.down();
  }

  // (b) `arg()` / `child_xml()` —— 把 Add 换成一个只记录参数的函数。顺带钉住
  //     "同名再注册一次会把前一个换掉"（头文件说 operation 表是共享的）。
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    r.ep->operation("Add", probe);
    reset_log();
    if (!r.serve()) {
      check(false, "1.14 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_add), resp)) {
      check_eq_i(status_of(resp), 200, "1.14 换过处理函数之后照样派发");
      check_eq_s(g_log.probe_arg_a, "1", "1.15 arg() 取到第一个参数的文本");
      check_eq_s(g_log.probe_raw_a, "<a>1</a>", "1.16 child_xml() 取到整段");
      check_eq_s(g_log.probe_arg_missing, std::string(),
                 "1.17 没有那个参数 -> 空串");
      check_eq_s(g_log.probe_raw_missing, std::string(),
                 "1.18 整段也是空（两种「没有」在这里长得一样，头文件写着）");
    } else {
      check(false, "1.14 请求没回来");
    }
    r.down();
  }

  // (c) rpc：包装元素从 `soap:body/@namespace` 推
  {
    rig r;
    if (!r.load(k_rpc, std::vector<std::string>(1, "Mul"))) return;
    r.ep->operation("Mul", probe);
    reset_log();
    if (!r.serve()) {
      check(false, "1.19 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    const std::string body =
        env11("<tns:Mul xmlns:tns=\"urn:calc\"><a>3</a><b>4</b></tns:Mul>");
    if (p11a(r.port, body, "\"urn:calc#Mul\"", resp)) {
      check_eq_i(status_of(resp), 200, "1.19 rpc 调用成功");
      check_eq_s(g_log.operation, "Mul", "1.20 派发到 Mul");
      check_eq_s(g_log.probe_arg_a, "3", "1.21 rpc 的参数照样取得出来");
      check_eq_s(body_of(resp),
                 response_bytes(uvcpp_soap_version::V1_1, "MulResponse",
                                "urn:calc", std::string()),
                 "1.22 响应包装元素是 {urn:calc}MulResponse");
    } else {
      check(false, "1.19 请求没回来");
    }

    // ★ 名字对、命名空间不对：落到"没有这个 operation"上。
    reset_log();
    const std::string wrong =
        env11("<tns:Mul xmlns:tns=\"urn:other\"><a>3</a><b>4</b></tns:Mul>");
    if (p11a(r.port, wrong, "\"urn:calc#Mul\"", resp)) {
      check_eq_i(status_of(resp), 500,
                 "1.23 ★ rpc 命名空间不对 -> 1.1 的 Fault 排 500");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::CLIENT,
                             "unrecognized SOAP body element: {urn:other}Mul"),
                 "1.24 它被当成一条不存在的 operation");
      check_eq_i(static_cast<long long>(g_log.calls), 0,
                 "1.25 处理函数一次都没被调（对照 1.20）");
    } else {
      check(false, "1.23 请求没回来");
    }
    r.down();
  }

  // (d) 未知 operation / 声明了但没实现 —— 两条 Fault 的**码不同**
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    if (!r.serve()) {
      check(false, "1.26 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11("<tns:Nope xmlns:tns=\"urn:calc\"/>"), resp)) {
      check_eq_i(status_of(resp), 500, "1.26 未知 operation");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::CLIENT,
                             "unrecognized SOAP body element: {urn:calc}Nope"),
                 "1.27 报的是 Client（对端把名字拼错了）");
    } else {
      check(false, "1.26 请求没回来");
    }
    r.down();
  }
  {
    // 只装 Add：Ping 在 WSDL 里，但没有处理函数。
    rig r;
    if (!r.load(k_doc, std::vector<std::string>(1, "Add"))) return;
    if (!r.serve()) {
      check(false, "1.28 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11("<tns:Ping xmlns:tns=\"urn:calc\"/>"), resp)) {
      check_eq_i(status_of(resp), 500, "1.28 声明了但没实现");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::SERVER,
                             "operation is declared in the WSDL but not "
                             "implemented: Ping"),
                 "1.29 ★ 报的是 Server（缺的是我方的东西）");
      // 前一条判据要成立，"两个码不同"必须是**真的**不同。
      check_ne_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::CLIENT,
                             "operation is declared in the WSDL but not "
                             "implemented: Ping"),
                 "1.30 （前提）Client 与 Server 在 1.1 下写出来的字节确实不同");
    } else {
      check(false, "1.28 请求没回来");
    }
    r.down();
  }

  // (e) 处理函数的三种结局
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    g_behavior = behavior::SILENT;
    if (!r.serve()) {
      check(false, "1.31 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_add), resp)) {
      check_eq_i(status_of(resp), 500, "1.31 处理函数什么都没给");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::SERVER,
                             "the handler produced no response"),
                 "1.32 报 Server（请求-响应型里这是我们的错）");
    } else {
      check(false, "1.31 请求没回来");
    }
    r.down();
    g_behavior = behavior::REPLY;
  }
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    g_behavior = behavior::FAULT_SERVER;
    if (!r.serve()) {
      check(false, "1.33 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_add), resp)) {
      check_eq_i(status_of(resp), 500, "1.33 处理函数报 SERVER（1.1 的请求）");
      check_eq_s(body_of(resp),
                 "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
                 "  <soap:Body>\n"
                 "    <soap:Fault>\n"
                 "      <faultcode>soap:Server</faultcode>\n"
                 "      <faultstring>boom</faultstring>\n"
                 "    </soap:Fault>\n"
                 "  </soap:Body>\n"
                 "</soap:Envelope>\n",
                 "1.34 1.1 的 Fault 逐字节（子元素**裸名**、小写）");
    } else {
      check(false, "1.33 请求没回来");
    }
    r.down();
    g_behavior = behavior::REPLY;
  }
  {
    // ★ 同一个语义码（CLIENT）在 1.2 下必须写成 `soap:Sender`，状态码也要换。
    rig r;
    if (!r.load(k_doc, ops3())) return;
    g_behavior = behavior::FAULT_CLIENT;
    if (!r.serve()) {
      check(false, "1.35 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p12(r.port, env12(k_add), resp)) {
      check_eq_i(status_of(resp), 400, "1.35 ★ 1.2 的 Sender 类 Fault 排 400");
      check_eq_s(ct_of(resp), "application/soap+xml; charset=utf-8",
                 "1.36 Fault 的 content-type 跟着版本走");
      check_eq_s(body_of(resp),
                 "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">\n"
                 "  <soap:Body>\n"
                 "    <soap:Fault>\n"
                 "      <soap:Code>\n"
                 "        <soap:Value>soap:Sender</soap:Value>\n"
                 "      </soap:Code>\n"
                 "      <soap:Reason>\n"
                 "        <soap:Text xml:lang=\"en\">boom</soap:Text>\n"
                 "      </soap:Reason>\n"
                 "    </soap:Fault>\n"
                 "  </soap:Body>\n"
                 "</soap:Envelope>\n",
                 "1.37 ★ 同一个 CLIENT 语义码在 1.2 下是 Sender（逐字节）");
      check_ne_s(fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::CLIENT, k_fault_reason),
                 body_of(resp),
                 "1.38 （前提）两版发出来的字节确实不同");
    } else {
      check(false, "1.35 请求没回来");
    }
    r.down();
    g_behavior = behavior::REPLY;
  }
  {
    // 自定义码（裸的局部名）落在信封命名空间里；1.2 下按 Sender 定 HTTP 码。
    rig r;
    if (!r.load(k_doc, ops3())) return;
    g_behavior = behavior::FAULT_CUSTOM;
    if (!r.serve()) {
      check(false, "1.39 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p12(r.port, env12(k_add), resp)) {
      check_eq_i(status_of(resp), 400,
                 "1.39 认不出的自定义码按 Sender 定 HTTP 码");
      check(body_of(resp).find("<soap:Value>soap:BadArgs</soap:Value>") !=
                std::string::npos,
            "1.40 自定义码写出来在信封命名空间里（裸名，头文件里有说明）");
    } else {
      check(false, "1.39 请求没回来");
    }
    r.down();
    g_behavior = behavior::REPLY;
  }
  {
    // ★ 1.1 里不存在的码：**不能**发成空码（空码写出来是 `soap:`，非法 QName）。
    rig r;
    if (!r.load(k_doc, ops3())) return;
    g_behavior = behavior::FAULT_BAD_ENCODING;
    if (!r.serve()) {
      check(false, "1.41 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_add), resp)) {
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::CLIENT, k_fault_reason),
                 "1.41 ★ 1.1 里没有 DataEncodingUnknown -> 落到 Client");
      check_eq_i(status_of(resp), 500, "1.42 状态码照旧");
    } else {
      check(false, "1.41 请求没回来");
    }
    // 同一个码在 1.2 下是**合法**的，所以原样发出去（正对照）。
    if (p12(r.port, env12(k_add), resp)) {
      check(body_of(resp).find(
                "<soap:Value>soap:DataEncodingUnknown</soap:Value>") !=
                std::string::npos,
            "1.43 同一个码在 1.2 下原样发");
      check_eq_i(status_of(resp), 400, "1.44 它是 Sender 一族");
    } else {
      check(false, "1.43 请求没回来");
    }
    r.down();
    g_behavior = behavior::REPLY;
  }
  {
    // 包装元素自己指定
    rig r;
    if (!r.load(k_doc, ops3())) return;
    g_behavior = behavior::REPLY_NAMED;
    if (!r.serve()) {
      check(false, "1.45 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_add), resp)) {
      check_eq_s(body_of(resp),
                 response_bytes(uvcpp_soap_version::V1_1, "Custom", "urn:x",
                                "<v/>"),
                 "1.45 set_response_named 覆盖了从 WSDL 推出来的包装元素");
    } else {
      check(false, "1.45 请求没回来");
    }
    r.down();
    g_behavior = behavior::REPLY;
  }
}

// =========================================================================
// 2. 准入与解析失败
// =========================================================================

void test_admission() {
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    if (!r.serve()) {
      check(false, "2.1 服务起不来");
      return;
    }

    // 准入：Content-Type 不是 SOAP 的两种之一 -> 415（不是 500）
    {
      uvcpp_http_response resp;
      if (post(r.port, k_path, env11(k_add), "application/json", nullptr, resp)) {
        check_eq_i(status_of(resp), 415, "2.1 ★ 媒体类型被拒是 415，不是 500");
        check_eq_s(body_of(resp),
                   fault_bytes(uvcpp_soap_version::V1_1,
                               uvcpp_soap_fault_code::CLIENT,
                               "Content-Type is not a SOAP media type"),
                   "2.2 正文仍是 1.1 形状的 Fault（还没有版本线索时的回落）");
      } else {
        check(false, "2.1 请求没回来");
      }
    }

    // 解析失败：条条都算 Sender，reason 里带得出本层的状态名
    const char* cases[] = {
        "",  // 空
        "<not-xml",  // 不是良构
        "<foo/>",  // 良构，但根不是信封
        "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\"/>",
        "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<soap:Body><a/><b/></soap:Body></soap:Envelope>"};
    const char* statuses[] = {"empty", "syntax", "soap_not_envelope",
                             "soap_no_body", "soap_too_many_body_elements"};
    const char* labels[] = {"2.3 空正文", "2.5 不是良构 XML", "2.7 根不是信封",
                            "2.9 没有 Body", "2.11 Body 里两个元素"};
    for (int i = 0; i < 5; ++i) {
      uvcpp_http_response resp;
      if (p11(r.port, cases[i], resp)) {
        check_eq_i(status_of(resp), 500,
                   std::string(labels[i]) + " 的 1.1 状态码");
        check_eq_s(body_of(resp),
                   fault_bytes(uvcpp_soap_version::V1_1,
                               uvcpp_soap_fault_code::CLIENT,
                               std::string("SOAP envelope is not usable: ") +
                                   statuses[i]),
                   std::string(labels[i]) + " 的 reason");
      } else {
        check(false, std::string(labels[i]) + " 请求没回来");
      }
    }

    // 超字节上限：`set_limits` 真的接上了吗（对照 1.1：上限够大时它是好的）
    //
    // ★ 上限**从这份报文自己算**，不写死一个数。原来的 200 就比这份报文长（这份
    // `env11(k_add)` 是 164 字节），于是这条判据悄悄退化成"发一个正常请求、看它正常
    // 回"——照样 PASS，一个字节都没验。两臂正好卡在边界两侧：判据是
    // `len > max_bytes`，所以**等于上限要收**（2.12）、**少一字节要拒**（2.13）。
    {
      const std::string body = env11(k_add);
      rig fits;
      if (!fits.load(k_doc, ops3())) return;
      fits.ep->set_limits(uvcpp_wsdl_limits(body.size()));
      if (!fits.serve()) {
        check(false, "2.12 服务起不来");
        return;
      }
      uvcpp_http_response ok;
      if (p11(fits.port, body, ok)) {
        check_eq_i(status_of(ok), 200,
                   "2.12 ★ 报文长度==上限：收（「超过就拒」，不是「到了就拒」）");
      } else {
        check(false, "2.12 请求没回来");
      }
      fits.down();

      rig tight;
      if (!tight.load(k_doc, ops3())) return;
      tight.ep->set_limits(uvcpp_wsdl_limits(body.size() - 1));
      if (!tight.serve()) {
        check(false, "2.13 服务起不来");
        return;
      }
      uvcpp_http_response resp;
      if (p11(tight.port, body, resp)) {
        check_eq_s(body_of(resp),
                   fault_bytes(uvcpp_soap_version::V1_1,
                               uvcpp_soap_fault_code::CLIENT,
                               "SOAP envelope is not usable: too_large"),
                   "2.13 ★ 少一字节：拒，而且报的是 too_large");
      } else {
        check(false, "2.13 请求没回来");
      }
      tight.down();
    }

    // ★ 解析失败时版本只能靠 Content-Type 判：1.2 的客户端拿到 400 而不是 500
    {
      uvcpp_http_response resp;
      if (p12(r.port, "<foo/>", resp)) {
        check_eq_i(status_of(resp), 400,
                   "2.14 ★ 1.2 的客户端发坏报文 -> 400（按 Content-Type 定版本）");
        check_eq_s(body_of(resp),
                   fault_bytes(uvcpp_soap_version::V1_2,
                               uvcpp_soap_fault_code::CLIENT,
                               "SOAP envelope is not usable: soap_not_envelope"),
                   "2.15 而且是 1.2 形状的 Fault");
      } else {
        check(false, "2.14 请求没回来");
      }
    }

    // 请求正文本身是一条 Fault（解析层当成功，这一层得拒）
    {
      uvcpp_http_response resp;
      const std::string f =
          std::string("<soap:Envelope xmlns:soap=\"") + k_env11 +
          "\"><soap:Body><soap:Fault><faultcode>soap:Client</faultcode>"
          "<faultstring>x</faultstring></soap:Fault></soap:Body></soap:Envelope>";
      if (p11(r.port, f, resp)) {
        check_eq_s(body_of(resp),
                   fault_bytes(uvcpp_soap_version::V1_1,
                               uvcpp_soap_fault_code::CLIENT,
                               "the request body is a SOAP Fault, not a call"),
                   "2.16 请求本身是 Fault");
      } else {
        check(false, "2.16 请求没回来");
      }
    }

    // mustUnderstand：数出来就拒，reason 里带条数
    {
      reset_log();
      uvcpp_http_response resp;
      const std::string body =
          env11h("<tns:Trace xmlns:tns=\"urn:calc\" soap:mustUnderstand=\"1\">a"
                 "</tns:Trace><tns:Hint xmlns:tns=\"urn:calc\">q</tns:Hint>",
                 k_add);
      if (p11(r.port, body, resp)) {
        check_eq_i(status_of(resp), 500, "2.17 mustUnderstand");
        check_eq_s(body_of(resp),
                   fault_bytes(uvcpp_soap_version::V1_1,
                               uvcpp_soap_fault_code::MUST_UNDERSTAND,
                               "a header with mustUnderstand was not understood; "
                               "count: 1"),
                   "2.18 报 MustUnderstand，reason 里带条数");
        check_eq_s(r.ep->dispatch_target_of("{urn:calc}Add"), "Add",
                   "2.19 （前提）这条报文的 Body 本来派得到 Add");
        check_eq_i(static_cast<long long>(g_log.calls), 0,
                   "2.20 ★ 它排在派发之前（处理函数一次都没被调）");
      } else {
        check(false, "2.17 请求没回来");
      }
    }

    // 顺序：mustUnderstand 排在 Body 之前 —— 一条**既是 Fault 又带** mustUnderstand
    // 的报文，报出来的是 MustUnderstand（`true` 也算数）。
    {
      uvcpp_http_response resp;
      const std::string f =
          std::string("<soap:Envelope xmlns:soap=\"") + k_env11 +
          "\"><soap:Header><t xmlns=\"urn:calc\" soap:mustUnderstand=\"true\"/>"
          "</soap:Header><soap:Body><soap:Fault>"
          "<faultcode>soap:Client</faultcode><faultstring>x</faultstring>"
          "</soap:Fault></soap:Body></soap:Envelope>";
      if (p11(r.port, f, resp)) {
        check_eq_s(body_of(resp),
                   fault_bytes(uvcpp_soap_version::V1_1,
                               uvcpp_soap_fault_code::MUST_UNDERSTAND,
                               "a header with mustUnderstand was not understood; "
                               "count: 1"),
                   "2.21 ★ mustUnderstand 先于 Body 判");
      } else {
        check(false, "2.21 请求没回来");
      }
    }

    r.down();
  }

  // 端点没装配好：那是我方的问题
  {
    rig bad;
    if (!bad.load(k_doc, ops3(), "Nope")) return;
    if (!bad.serve()) {
      check(false, "2.22 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(bad.port, env11(k_add), resp)) {
      check_eq_i(status_of(resp), 500, "2.22 端点没装配好（1.1）");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::SERVER,
                             "this endpoint was not assembled from the WSDL"),
                 "2.23 报的是 Server，不是 Client");
    } else {
      check(false, "2.22 请求没回来");
    }
    // 1.2 下它是 Receiver -> 仍 500（**不是** 400）—— 与 2.14 正好是一对。
    if (p12(bad.port, env12(k_add), resp)) {
      check_eq_i(status_of(resp), 500,
                 "2.24 ★ 1.2 下 Receiver 也是 500（4xx 只给 Sender 一族）");
    } else {
      check(false, "2.24 请求没回来");
    }
    bad.down();
  }
}

// =========================================================================
// 3. 版本策略与"内容类型 / 信封不一致"
// =========================================================================

void test_version_policy() {
  check_eq_s(std::string(uvcpp_soap_version_policy_name(
                 uvcpp_soap_version_policy::ANY)),
             "any", "3.1 策略名 1/3");
  check_eq_s(std::string(uvcpp_soap_version_policy_name(
                 uvcpp_soap_version_policy::V1_1_ONLY)),
             "1.1-only", "3.2 策略名 2/3");
  check_eq_s(std::string(uvcpp_soap_version_policy_name(
                 uvcpp_soap_version_policy::V1_2_ONLY)),
             "1.2-only", "3.3 策略名 3/3");

  // 默认 ANY：两个版本都收
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    reset_log();
    if (!r.serve()) {
      check(false, "3.4 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p12(r.port, env12(k_add), resp)) {
      check_eq_i(status_of(resp), 200, "3.4 1.2 的调用被收下");
      check_eq_s(body_of(resp),
                 response_bytes(uvcpp_soap_version::V1_2, "AddResponse",
                                "urn:calc", k_reply_inner),
                 "3.5 响应是 1.2 的信封");
      check_eq_s(ct_of(resp), "application/soap+xml; charset=utf-8",
                 "3.6 1.2 的 content-type");
      check(g_log.version == uvcpp_soap_version::V1_2, "3.7 处理函数看到 1.2");
    } else {
      check(false, "3.4 请求没回来");
    }

    // ★ 内容类型与信封不一致：**以信封为准**（Content-Type 只做准入）
    {
      uvcpp_http_response r12;
      if (post(r.port, k_path, env12(k_add), k_ct11, nullptr, r12)) {
        check_eq_i(status_of(r12), 200, "3.8 ★ 1.2 信封 + text/xml 照样收下");
        check_eq_s(body_of(r12),
                   response_bytes(uvcpp_soap_version::V1_2, "AddResponse",
                                  "urn:calc", k_reply_inner),
                   "3.9 ★ 回的是 1.2 的信封（版本以信封为准）");
      } else {
        check(false, "3.8 请求没回来");
      }
    }
    {
      uvcpp_http_response r11;
      if (post(r.port, k_path, env11(k_add), k_ct12, nullptr, r11)) {
        check_eq_i(status_of(r11), 200, "3.10 ★ 1.1 信封 + application/soap+xml");
        check_eq_s(body_of(r11),
                   response_bytes(uvcpp_soap_version::V1_1, "AddResponse",
                                  "urn:calc", k_reply_inner),
                   "3.11 ★ 回的是 1.1 的信封");
      } else {
        check(false, "3.10 请求没回来");
      }
    }
    r.down();
  }

  // V1_1_ONLY
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    r.ep->set_version_policy(uvcpp_soap_version_policy::V1_1_ONLY);
    if (!r.serve()) {
      check(false, "3.12 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_add), resp)) {
      check_eq_i(status_of(resp), 200, "3.12 1.1 的请求照收");
    } else {
      check(false, "3.12 请求没回来");
    }
    if (p12(r.port, env12(k_add), resp)) {
      check_eq_i(status_of(resp), 400, "3.13 1.2 的请求被拒（Sender 一族）");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_2,
                             uvcpp_soap_fault_code::VERSION_MISMATCH,
                             "this endpoint accepts SOAP 1.1 only"),
                 "3.14 ★ 报 VersionMismatch，按**信封**那一版发出去");
    } else {
      check(false, "3.13 请求没回来");
    }
    r.down();
  }

  // V1_2_ONLY：同一个语义码，1.1 下是另一个名字、另一个状态码
  {
    rig r;
    if (!r.load(k_doc, ops3())) return;
    r.ep->set_version_policy(uvcpp_soap_version_policy::V1_2_ONLY);
    if (!r.serve()) {
      check(false, "3.15 服务起不来");
      return;
    }
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_add), resp)) {
      check_eq_i(status_of(resp), 500, "3.15 1.1 的请求被拒（1.1 只有 500）");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::VERSION_MISMATCH,
                             "this endpoint accepts SOAP 1.2 only"),
                 "3.16 ★ 1.1 形状（faultcode 裸名 + 500）");
    } else {
      check(false, "3.15 请求没回来");
    }
    if (p12(r.port, env12(k_add), resp)) {
      check_eq_i(status_of(resp), 200, "3.17 1.2 的请求照收（正对照）");
    } else {
      check(false, "3.17 请求没回来");
    }
    r.down();
  }
}

// =========================================================================
// 4. 动作值
// =========================================================================

void test_action() {
  rig r;
  if (!r.load(k_doc, ops3())) return;
  if (!r.serve()) {
    check(false, "4.1 服务起不来");
    return;
  }

  // 一致（规范里 1.1 的写法就是带引号）
  {
    reset_log();
    uvcpp_http_response resp;
    if (p11a(r.port, env11(k_add), "\"urn:calc#Add\"", resp)) {
      check_eq_i(status_of(resp), 200, "4.1 SOAPAction 与 WSDL 一致");
      check_eq_s(g_log.action, "urn:calc#Add", "4.2 处理函数拿到归一后的值");
    } else {
      check(false, "4.1 请求没回来");
    }
  }
  // 不一致
  {
    reset_log();
    uvcpp_http_response resp;
    if (p11a(r.port, env11(k_add), "\"urn:calc#Nope\"", resp)) {
      check_eq_i(status_of(resp), 500, "4.3 SOAPAction 与 WSDL 不一致");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::CLIENT,
                             "action does not match the WSDL; declared: "
                             "urn:calc#Add, got: urn:calc#Nope"),
                 "4.4 reason 里两个值都在");
      check_eq_i(static_cast<long long>(g_log.calls), 0, "4.5 处理函数没被调");
    } else {
      check(false, "4.3 请求没回来");
    }
  }
  // 空串：规范里它表示"看报文体自己"，所以**不核对**
  {
    reset_log();
    uvcpp_http_response resp;
    if (p11a(r.port, env11(k_add), "\"\"", resp)) {
      check_eq_i(status_of(resp), 200, "4.6 ★ SOAPAction 是空串时不核对");
      check(!g_log.action_present, "4.7 它不算「提供了动作值」");
    } else {
      check(false, "4.6 请求没回来");
    }
  }
  // 引号里的空白：归一之后仍然相等
  {
    reset_log();
    uvcpp_http_response resp;
    if (p11a(r.port, env11(k_add), "\" urn:calc#Add \"", resp)) {
      check_eq_i(status_of(resp), 200, "4.8 ★ 归一（剥空白与一对引号，含引号里）");
    } else {
      check(false, "4.8 请求没回来");
    }
  }
  // 1.2 的动作在 Content-Type 的 `action=` 参数里
  {
    reset_log();
    uvcpp_http_response resp;
    const std::string ct = std::string(k_ct12) + "; action=\"urn:calc#Add\"";
    if (post(r.port, k_path, env12(k_add), ct, nullptr, resp)) {
      check_eq_i(status_of(resp), 200, "4.9 1.2 的 action 一致");
      check_eq_s(g_log.action, "urn:calc#Add", "4.10 读到的是 action= 那个参数");
    } else {
      check(false, "4.9 请求没回来");
    }
  }
  {
    reset_log();
    uvcpp_http_response resp;
    const std::string ct = std::string(k_ct12) + "; action=\"urn:nope\"";
    if (post(r.port, k_path, env12(k_add), ct, nullptr, resp)) {
      check_eq_i(status_of(resp), 400, "4.11 1.2 的 action 不一致 -> 400");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_2,
                             uvcpp_soap_fault_code::CLIENT,
                             "action does not match the WSDL; declared: "
                             "urn:calc#Add, got: urn:nope"),
                 "4.12 reason 里两个值都在（1.2 形状）");
    } else {
      check(false, "4.11 请求没回来");
    }
  }

  // ★ 下面两条各钉"来源选错会怎样"的一半：
  //   4.13 一个 1.2 的请求**带** `SOAPAction` 头（值是错的）而 Content-Type 里没有
  //        `action=` —— 收下，证明那个头被忽略了（真按它核就会拒）。
  //   4.14 同一个 1.2 的请求，头是**对的**、`action=` 是**错的** —— 拒，且 reason 里
  //        带的是参数那个值。两条合起来把"1.2 只读参数"钉死。
  {
    uvcpp_http_response resp;
    if (post(r.port, k_path, env12(k_add), k_ct12, "\"urn:calc#Nope\"", resp)) {
      check_eq_i(status_of(resp), 200,
                 "4.13 ★ 1.2 的请求不看 `SOAPAction` 头（没有 action= 就不核）");
    } else {
      check(false, "4.13 请求没回来");
    }
  }
  {
    uvcpp_http_response resp;
    const std::string ct = std::string(k_ct12) + "; action=\"urn:nope\"";
    if (post(r.port, k_path, env12(k_add), ct, "\"urn:calc#Add\"", resp)) {
      check_eq_i(status_of(resp), 400, "4.14 1.2 下拒的是参数那个值");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_2,
                             uvcpp_soap_fault_code::CLIENT,
                             "action does not match the WSDL; declared: "
                             "urn:calc#Add, got: urn:nope"),
                 "4.15 ★ 头是对的也没用 —— 1.2 读的是参数");
    } else {
      check(false, "4.14 请求没回来");
    }
  }

  // 反向：1.1 只看 `SOAPAction` 头
  {
    uvcpp_http_response resp;
    const std::string ct = std::string(k_ct11) + "; action=\"urn:nope\"";
    if (post(r.port, k_path, env11(k_add), ct, nullptr, resp)) {
      check_eq_i(status_of(resp), 200,
                 "4.16 ★ 1.1 的请求不看 `action=` 参数（它只看 `SOAPAction` 头）");
    } else {
      check(false, "4.16 请求没回来");
    }
  }
  {
    uvcpp_http_response resp;
    const std::string ct = std::string(k_ct11) + "; action=\"urn:calc#Add\"";
    if (post(r.port, k_path, env11(k_add), ct, "\"urn:calc#Nope\"", resp)) {
      check_eq_i(status_of(resp), 500, "4.17 1.1 下拒的是头那个值");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::CLIENT,
                             "action does not match the WSDL; declared: "
                             "urn:calc#Add, got: urn:calc#Nope"),
                 "4.18 ★ 参数是对的也没用 —— 1.1 读的是头");
    } else {
      check(false, "4.17 请求没回来");
    }
  }

  r.down();
}

// =========================================================================
// 5. 单向 operation
// =========================================================================

void test_oneway() {
  // `Notify` 在 WSDL 里没有 output。
  rig r;
  if (!r.load(k_doc, ops3())) return;
  reset_log();
  if (!r.serve()) {
    check(false, "5.1 服务起不来");
    return;
  }

  // 处理函数给了响应 -> 忽略，202 + 空正文
  {
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_notify), resp)) {
      check_eq_i(status_of(resp), 202, "5.1 单向 operation 回 202");
      check_eq_s(body_of(resp), std::string(), "5.2 正文是空的");
      check_eq_s(http_get_header(resp.headers, "content-length"), "0",
                 "5.3 长度是 0（不是没这个头）");
      check_eq_s(g_log.operation, "Notify", "5.4 它确实被派发出去了");
    } else {
      check(false, "5.1 请求没回来");
    }
  }

  // 处理函数什么都没给 -> **也是** 202（与 1.31 那条正好相反）
  {
    g_behavior = behavior::SILENT;
    reset_log();
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_notify), resp)) {
      check_eq_i(status_of(resp), 202,
                 "5.5 ★ 单向的「什么都没给」是正常的，不是 Server Fault");
      check_eq_s(body_of(resp), std::string(), "5.6 正文空");
    } else {
      check(false, "5.5 请求没回来");
    }
    g_behavior = behavior::REPLY;
  }

  // 处理函数报 Fault -> 发那条 Fault（这是单向唯一能报告失败的渠道）
  {
    g_behavior = behavior::FAULT_SERVER;
    uvcpp_http_response resp;
    if (p11(r.port, env11(k_notify), resp)) {
      check_eq_i(status_of(resp), 500, "5.7 单向 + Fault -> 发 Fault（不是 202）");
      check_eq_s(body_of(resp),
                 fault_bytes(uvcpp_soap_version::V1_1,
                             uvcpp_soap_fault_code::SERVER, k_fault_reason),
                 "5.8 Fault 的形状照旧");
    } else {
      check(false, "5.7 请求没回来");
    }
    g_behavior = behavior::REPLY;
  }

  // WSDL 里没声明动作值 -> 请求带什么动作都不核对（对照 4.3）
  {
    uvcpp_http_response resp;
    if (p11a(r.port, env11(k_notify), "\"urn:whatever\"", resp)) {
      check_eq_i(status_of(resp), 202,
                 "5.9 ★ WSDL 没声明 soapAction -> 请求带什么都不核对");
    } else {
      check(false, "5.9 请求没回来");
    }
  }

  r.down();
}

}  // namespace

int main() {
  std::cout << "[web_app_soap] " << std::flush;
  test_assembly();
  test_dispatch();
  test_admission();
  test_version_policy();
  test_action();
  test_oneway();

  if (g_failures == 0) {
    std::cout << "ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "FAIL (" << g_failures << ")" << std::endl;
  return 2;
}

#else  // UVCPP_WSDL_ENABLE

// 关掉 wsdl 模块时这个文件不该被编进来（`tests/functional/CMakeLists.txt` 的过滤器
// 按文件名摘掉它）。这里返回非零：真编到了就是一个必须修的配置错。
int main() {
  std::cerr << "[web_app_soap] UVCPP_WSDL_ENABLE=0 —— 这个测试文件不该被编译进来"
            << std::endl;
  return 2;
}

#endif  // UVCPP_WSDL_ENABLE
