/**
 * @file tests/functional/wsdl_document_func.cpp
 * @brief WSDL 1.1 文档模型：解析、命名空间、往返、拒绝档、引用闭合。
 *
 * ## 判据的形状：每一条"必须拒"都配一条"只差一处、必须收"
 *
 * 只写"坏文档被拒"是没有分辨力的 —— 一个对什么都报错的解析器也能全绿。所以
 * 第 4 组（拒绝档）里每一条都是**一对**：坏的那份必须给出**那个**状态码，
 * 好的那份只改一处、必须 `OK`。两条一起才说得上"这个是判据，不是装饰"。
 *
 * 同理，第 2 组（命名空间）里最要紧的不是"四种前缀写法都能解析"，而是那条
 * **负对照**：把 `soap:binding` 绑到一个**假 URI** 上，它就不能被当成 SOAP
 * 绑定收下。少了它，"解析器根本没看命名空间"这个错也能让前三份全绿。
 *
 * ## 第 3 组的往返是**语义**往返，不是字节往返
 *
 * `uvcpp_wsdl_dump` 输出的是模型投影（见 `uvcpp_wsdl_document.h` 文件头），
 * 所以判据是 `指纹(解析(序列化(模型))) == 指纹(模型)` —— 指纹把模型里每一个
 * 字段都摊平成一串文本（见 `fingerprint()`），漏一个字段就会在往返里露出来。
 * 另外两条是**序列化器自己**的性质：同样输入两次产出逐字节相同；产出里不许
 * 有 CR。
 *
 * ## 一个具体的历史坑被钉在这里
 *
 * `<wsdl:types>` 是**原样**存的（它可能自带 `xmlns` 声明），而序列化时如果
 * **不把原文档根上那个 `wsdl` 前缀照搬下来**，重新解析那份产出时这个 `<types>`
 * 会被整段跳过（前缀解析不出来 ⇒ URI 是空串 ⇒ 不是 `{wsdl}types`）。第 3.4 条
 * 就是钉它的：产出里必须有 `xmlns:wsdl=`。
 */
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WSDL_ENABLE

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

void check_st(wsdl_status got, wsdl_status want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: "
              << wsdl_status_name(want) << "\n         实际: "
              << wsdl_status_name(got) << std::endl;
    ++g_failures;
  }
}

/// 解析**必须成功**；失败时报红并返回一个空模型。
uvcpp_wsdl_document must_parse(const std::string& xml, const std::string& what) {
  uvcpp_wsdl_document d;
  const char* why = nullptr;
  const wsdl_status st = uvcpp_wsdl_parse(xml, d, uvcpp_wsdl_limits(), &why);
  if (st != wsdl_status::OK) {
    std::cerr << "  [FAIL] " << what << "：解析失败 " << wsdl_status_name(st)
              << "（" << (why ? why : "") << "）" << std::endl;
    ++g_failures;
    return uvcpp_wsdl_document();
  }
  return d;
}

/// 把模型里**每一个字段**摊平成一串文本。漏字段 = 往返判据漏判。
std::string oneline(const std::string& s) {
  std::string out;
  for (std::string::size_type i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') out += "\\n";
    else if (s[i] == '\r') out += "\\r";
    else out += s[i];
  }
  return out;
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (std::string::size_type i = 0; i < v.size(); ++i) {
    if (i != 0) out += ",";
    out += v[i];
  }
  return out;
}

void ref_fp(std::ostringstream& o, const char* tag, const uvcpp_wsdl_op_ref& r) {
  o << "      " << tag << "=" << r.name << "|" << r.message.str() << "|"
    << oneline(r.documentation) << "\n";
}

std::string fingerprint(const uvcpp_wsdl_document& d) {
  std::ostringstream o;
  o << "tns=" << d.target_namespace << "\n";
  o << "name=" << d.name << "\n";
  o << "doc=" << oneline(d.documentation) << "\n";
  o << "types=" << oneline(d.types_xml) << "\n";
  o << "imports=" << d.imports.size() << "\n";
  for (std::string::size_type i = 0; i < d.imports.size(); ++i) {
    o << "  imp=" << d.imports[i].namespace_uri << "|" << d.imports[i].location
      << "\n";
  }
  o << "messages=" << d.messages.size() << "\n";
  for (std::string::size_type i = 0; i < d.messages.size(); ++i) {
    const uvcpp_wsdl_message& m = d.messages[i];
    o << "  m=" << m.name << "|" << oneline(m.documentation) << "\n";
    for (std::string::size_type j = 0; j < m.parts.size(); ++j) {
      o << "    p=" << m.parts[j].name << "|" << m.parts[j].element.str() << "|"
        << m.parts[j].type.str() << "|" << oneline(m.parts[j].documentation)
        << "\n";
    }
  }
  o << "portTypes=" << d.port_types.size() << "\n";
  for (std::string::size_type i = 0; i < d.port_types.size(); ++i) {
    const uvcpp_wsdl_port_type& pt = d.port_types[i];
    o << "  pt=" << pt.name << "|" << oneline(pt.documentation) << "\n";
    for (std::string::size_type j = 0; j < pt.operations.size(); ++j) {
      const uvcpp_wsdl_operation& op = pt.operations[j];
      o << "    op=" << op.name << "|" << (op.has_input ? 1 : 0)
        << (op.has_output ? 1 : 0) << "|" << join(op.parameter_order) << "|"
        << oneline(op.documentation) << "\n";
      ref_fp(o, "in", op.input);
      ref_fp(o, "out", op.output);
      for (std::string::size_type k = 0; k < op.faults.size(); ++k) {
        ref_fp(o, "fault", op.faults[k]);
      }
    }
  }
  o << "bindings=" << d.bindings.size() << "\n";
  for (std::string::size_type i = 0; i < d.bindings.size(); ++i) {
    const uvcpp_wsdl_binding& b = d.bindings[i];
    o << "  b=" << b.name << "|" << b.port_type.str() << "|soap="
      << (b.soap.present ? 1 : 0) << (b.soap.is_soap12 ? 1 : 0) << "|"
      << b.soap.style << "|" << b.soap.transport << "|"
      << oneline(b.documentation) << "\n";
    for (std::string::size_type j = 0; j < b.operations.size(); ++j) {
      const uvcpp_wsdl_binding_operation& ob = b.operations[j];
      o << "    ob=" << ob.name << "|" << ob.soap_action << "|" << ob.style
        << "|" << (ob.is_soap12 ? 1 : 0) << "|" << oneline(ob.documentation)
        << "\n";
      o << "      in=" << ob.input_use << "|" << ob.input_encoding_style << "|"
        << ob.input_namespace << "|" << join(ob.input_body_parts) << "\n";
      o << "      out=" << ob.output_use << "|" << ob.output_encoding_style
        << "|" << ob.output_namespace << "|" << join(ob.output_body_parts)
        << "\n";
    }
  }
  o << "services=" << d.services.size() << "\n";
  for (std::string::size_type i = 0; i < d.services.size(); ++i) {
    const uvcpp_wsdl_service& s = d.services[i];
    o << "  s=" << s.name << "|" << oneline(s.documentation) << "\n";
    for (std::string::size_type j = 0; j < s.ports.size(); ++j) {
      const uvcpp_wsdl_port& p = s.ports[j];
      o << "    port=" << p.name << "|" << p.binding.str() << "|" << p.address
        << "|" << (p.is_soap12 ? 1 : 0) << "|" << oneline(p.documentation)
        << "\n";
    }
  }
  return o.str();
}

// =========================================================================
// 样例文档
// =========================================================================

/// 一份把 1.1 的每种元素都覆盖到的文档（含一个单向 operation 与一个 fault）。
const char* k_calc =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<wsdl:definitions name=\"Calculator\"\n"
    "                  targetNamespace=\"http://example.com/calc\"\n"
    "                  xmlns:wsdl=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "                  xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "                  xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\"\n"
    "                  xmlns:tns=\"http://example.com/calc\">\n"
    "  <wsdl:documentation>计算器服务</wsdl:documentation>\n"
    "  <wsdl:types>\n"
    "    <xsd:schema targetNamespace=\"http://example.com/calc\">\n"
    "      <xsd:element name=\"Add\" type=\"xsd:int\"/>\n"
    "    </xsd:schema>\n"
    "  </wsdl:types>\n"
    "  <wsdl:message name=\"AddRequest\">\n"
    "    <wsdl:part name=\"parameters\" element=\"tns:Add\"/>\n"
    "  </wsdl:message>\n"
    "  <wsdl:message name=\"AddResponse\">\n"
    "    <wsdl:part name=\"result\" type=\"xsd:int\"/>\n"
    "  </wsdl:message>\n"
    "  <wsdl:message name=\"PingRequest\">\n"
    "    <wsdl:part name=\"text\" type=\"xsd:string\"/>\n"
    "  </wsdl:message>\n"
    "  <wsdl:message name=\"CalcFault\">\n"
    "    <wsdl:part name=\"reason\" type=\"xsd:string\"/>\n"
    "  </wsdl:message>\n"
    "  <wsdl:portType name=\"CalculatorPortType\">\n"
    "    <wsdl:operation name=\"Add\" parameterOrder=\"a b\">\n"
    "      <wsdl:documentation>两数相加</wsdl:documentation>\n"
    "      <wsdl:input message=\"tns:AddRequest\"/>\n"
    "      <wsdl:output message=\"tns:AddResponse\"/>\n"
    "      <wsdl:fault name=\"CalcFault\" message=\"tns:CalcFault\"/>\n"
    "    </wsdl:operation>\n"
    "    <wsdl:operation name=\"Ping\">\n"
    "      <wsdl:input name=\"in\" message=\"tns:PingRequest\"/>\n"
    "    </wsdl:operation>\n"
    "  </wsdl:portType>\n"
    "  <wsdl:binding name=\"CalculatorSoapBinding\" type=\"tns:CalculatorPortType\">\n"
    "    <soap:binding style=\"document\"\n"
    "                  transport=\"http://schemas.xmlsoap.org/soap/http\"/>\n"
    "    <wsdl:operation name=\"Add\">\n"
    "      <soap:operation soapAction=\"http://example.com/calc/Add\" style=\"document\"/>\n"
    "      <wsdl:input><soap:body use=\"literal\"/></wsdl:input>\n"
    "      <wsdl:output><soap:body use=\"literal\"/></wsdl:output>\n"
    "    </wsdl:operation>\n"
    "    <wsdl:operation name=\"Ping\">\n"
    "      <soap:operation soapAction=\"http://example.com/calc/Ping\"/>\n"
    "      <wsdl:input><soap:body use=\"literal\" parts=\"text\"/></wsdl:input>\n"
    "    </wsdl:operation>\n"
    "  </wsdl:binding>\n"
    "  <wsdl:service name=\"CalculatorService\">\n"
    "    <wsdl:port name=\"CalculatorPort\" binding=\"tns:CalculatorSoapBinding\">\n"
    "      <soap:address location=\"http://example.com/calc\"/>\n"
    "    </wsdl:port>\n"
    "  </wsdl:service>\n"
    "</wsdl:definitions>\n";

/// 第 2 组用的小文档：只差**前缀约定**，语义完全相同。
const char* k_ns_a =  // 全部用 wsdl: / soap: 前缀
    "<wsdl:definitions xmlns:wsdl=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "                  xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "                  xmlns:tns=\"urn:mini\" targetNamespace=\"urn:mini\">\n"
    "  <wsdl:message name=\"M\"><wsdl:part name=\"p\" type=\"tns:T\"/></wsdl:message>\n"
    "  <wsdl:portType name=\"P\">\n"
    "    <wsdl:operation name=\"Op\"><wsdl:input message=\"tns:M\"/></wsdl:operation>\n"
    "  </wsdl:portType>\n"
    "  <wsdl:binding name=\"B\" type=\"tns:P\">\n"
    "    <soap:binding style=\"document\"/>\n"
    "    <wsdl:operation name=\"Op\"><wsdl:input><soap:body use=\"literal\"/></wsdl:input></wsdl:operation>\n"
    "  </wsdl:binding>\n"
    "  <wsdl:service name=\"S\">\n"
    "    <wsdl:port name=\"Pt\" binding=\"tns:B\"><soap:address location=\"urn:mini\"/></wsdl:port>\n"
    "  </wsdl:service>\n"
    "</wsdl:definitions>\n";

const char* k_ns_b =  // WSDL 走**默认命名空间**，SOAP 仍带前缀
    "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "             xmlns:soap=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "             xmlns:tns=\"urn:mini\" targetNamespace=\"urn:mini\">\n"
    "  <message name=\"M\"><part name=\"p\" type=\"tns:T\"/></message>\n"
    "  <portType name=\"P\">\n"
    "    <operation name=\"Op\"><input message=\"tns:M\"/></operation>\n"
    "  </portType>\n"
    "  <binding name=\"B\" type=\"tns:P\">\n"
    "    <soap:binding style=\"document\"/>\n"
    "    <operation name=\"Op\"><input><soap:body use=\"literal\"/></input></operation>\n"
    "  </binding>\n"
    "  <service name=\"S\">\n"
    "    <port name=\"Pt\" binding=\"tns:B\"><soap:address location=\"urn:mini\"/></port>\n"
    "  </service>\n"
    "</definitions>\n";

const char* k_ns_c =  // 前缀**全大写**（XML 前缀区分大小写，URI 不区分）
    "<WSDL:definitions xmlns:WSDL=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "                  xmlns:SOAP=\"http://schemas.xmlsoap.org/wsdl/soap/\"\n"
    "                  xmlns:tns=\"urn:mini\" targetNamespace=\"urn:mini\">\n"
    "  <WSDL:message name=\"M\"><WSDL:part name=\"p\" type=\"tns:T\"/></WSDL:message>\n"
    "  <WSDL:portType name=\"P\">\n"
    "    <WSDL:operation name=\"Op\"><WSDL:input message=\"tns:M\"/></WSDL:operation>\n"
    "  </WSDL:portType>\n"
    "  <WSDL:binding name=\"B\" type=\"tns:P\">\n"
    "    <SOAP:binding style=\"document\"/>\n"
    "    <WSDL:operation name=\"Op\"><WSDL:input><SOAP:body use=\"literal\"/></WSDL:input></WSDL:operation>\n"
    "  </WSDL:binding>\n"
    "  <WSDL:service name=\"S\">\n"
    "    <WSDL:port name=\"Pt\" binding=\"tns:B\"><SOAP:address location=\"urn:mini\"/></WSDL:port>\n"
    "  </WSDL:service>\n"
    "</WSDL:definitions>\n";

const char* k_ns_d =  // 每个元素**自己声明**自己的前缀（最刻薄的一种写法）
    "<a:definitions xmlns:a=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "               xmlns:tns=\"urn:mini\" targetNamespace=\"urn:mini\">\n"
    "  <b:message xmlns:b=\"http://schemas.xmlsoap.org/wsdl/\" name=\"M\">\n"
    "    <c:part xmlns:c=\"http://schemas.xmlsoap.org/wsdl/\" name=\"p\" type=\"tns:T\"/>\n"
    "  </b:message>\n"
    "  <d:portType xmlns:d=\"http://schemas.xmlsoap.org/wsdl/\" name=\"P\">\n"
    "    <e:operation xmlns:e=\"http://schemas.xmlsoap.org/wsdl/\" name=\"Op\">\n"
    "      <f:input xmlns:f=\"http://schemas.xmlsoap.org/wsdl/\" message=\"tns:M\"/>\n"
    "    </e:operation>\n"
    "  </d:portType>\n"
    "  <g:binding xmlns:g=\"http://schemas.xmlsoap.org/wsdl/\" name=\"B\" type=\"tns:P\">\n"
    "    <h:binding xmlns:h=\"http://schemas.xmlsoap.org/wsdl/soap/\" style=\"document\"/>\n"
    "    <i:operation xmlns:i=\"http://schemas.xmlsoap.org/wsdl/\" name=\"Op\">\n"
    "      <j:input xmlns:j=\"http://schemas.xmlsoap.org/wsdl/\">\n"
    "        <k:body xmlns:k=\"http://schemas.xmlsoap.org/wsdl/soap/\" use=\"literal\"/>\n"
    "      </j:input>\n"
    "    </i:operation>\n"
    "  </g:binding>\n"
    "  <l:service xmlns:l=\"http://schemas.xmlsoap.org/wsdl/\" name=\"S\">\n"
    "    <m:port xmlns:m=\"http://schemas.xmlsoap.org/wsdl/\" name=\"Pt\" binding=\"tns:B\">\n"
    "      <n:address xmlns:n=\"http://schemas.xmlsoap.org/wsdl/soap/\" location=\"urn:mini\"/>\n"
    "    </m:port>\n"
    "  </l:service>\n"
    "</a:definitions>\n";

/// 负对照：`soap:binding` 绑到的是**假 URI**，不能被当成 SOAP 绑定。
const char* k_ns_e_fake_soap =
    "<wsdl:definitions xmlns:wsdl=\"http://schemas.xmlsoap.org/wsdl/\"\n"
    "                  xmlns:soap=\"http://example.com/not-soap/\"\n"
    "                  xmlns:tns=\"urn:mini\" targetNamespace=\"urn:mini\">\n"
    "  <wsdl:binding name=\"B\" type=\"tns:P\">\n"
    "    <soap:binding style=\"document\"/>\n"
    "    <wsdl:operation name=\"Op\"><wsdl:input><soap:body use=\"literal\"/></wsdl:input></wsdl:operation>\n"
    "  </wsdl:binding>\n"
    "</wsdl:definitions>\n";

// =========================================================================
// 第 1 组：解析一份完整文档
// =========================================================================

void group_parse() {
  const uvcpp_wsdl_document d = must_parse(k_calc, "1.0 完整文档");
  if (g_failures != 0) return;

  check_eq_s(d.target_namespace, "http://example.com/calc", "1.1 targetNamespace");
  check_eq_s(d.name, "Calculator", "1.2 definitions/@name");
  check_eq_s(d.documentation, "计算器服务", "1.3 definitions 的 documentation");
  check_eq_i(static_cast<long long>(d.source_bytes), static_cast<long long>(std::string(k_calc).size()),
             "1.4 source_bytes 记的是输入长度");

  check_eq_i(static_cast<long long>(d.messages.size()), 4, "1.5 message 条数");
  const uvcpp_wsdl_message* req = d.find_message("AddRequest");
  check(req != nullptr, "1.6 find_message(\"AddRequest\")");
  if (req != nullptr) {
    check_eq_i(static_cast<long long>(req->parts.size()), 1, "1.7 part 条数");
    check_eq_s(req->parts[0].element.str(),
               "{http://example.com/calc}Add", "1.8 part/@element 解析成 QName");
    check(req->parts[0].type.empty(), "1.9 element 与 type 不会同时有值");
  }
  // `tns:` 写法与 `{URI}` 写法必须查到同一条；别人的命名空间必须**查不到**。
  // `tns:` 那一档是**前缀**，靠文档根上的 `xmlns:tns` 翻成 URI —— 少了那一步
  // 它就会被当成一个字面量 URI，与 targetNamespace 比出"不等"。
  check(d.find_message("tns:AddRequest") == req, "1.10 tns: 前缀写法查到同一条");
  check(d.find_message("{http://example.com/calc}AddRequest") == req,
        "1.11 {URI} 写法查到同一条");
  check(d.find_message("{http://other/}AddRequest") == nullptr,
        "1.12 别的命名空间的同名 message 查不到");
  // 1.12 的对照：**没声明过**的前缀也必须查不到（否则"前缀一律认"这个错
  // 也能让 1.10 绿）。
  check(d.find_message("nope:AddRequest") == nullptr,
        "1.12a 没声明过的前缀查不到");
  // 而一个声明了、但指向**别处**的前缀同样查不到。
  check(d.find_message("xsd:AddRequest") == nullptr,
        "1.12b 指向别处的前缀（xsd:）也查不到");
  check_eq_s(d.find_message("AddResponse")->parts[0].type.str(),
             "{http://www.w3.org/2001/XMLSchema}int", "1.13 part/@type 解析");

  const uvcpp_wsdl_port_type* pt = d.find_port_type("tns:CalculatorPortType");
  check(pt != nullptr, "1.14 find_port_type");
  if (pt != nullptr) {
    check_eq_i(static_cast<long long>(pt->operations.size()), 2, "1.15 operation 条数");
    const uvcpp_wsdl_operation* add = pt->find_operation("Add");
    check(add != nullptr, "1.16 find_operation(\"Add\")");
    if (add != nullptr) {
      check(add->has_input && add->has_output, "1.17 Add 有 input 与 output");
      check_eq_s(join(add->parameter_order), "a,b", "1.18 parameterOrder 切开了");
      check_eq_s(add->input.message.str(), "{http://example.com/calc}AddRequest",
                 "1.19 input/@message");
      check_eq_i(static_cast<long long>(add->faults.size()), 1, "1.20 fault 条数");
      check_eq_s(add->faults[0].name, "CalcFault", "1.21 fault/@name");
      check_eq_s(add->documentation, "两数相加", "1.22 operation 的 documentation");
    }
    const uvcpp_wsdl_operation* ping = pt->find_operation("Ping");
    check(ping != nullptr, "1.23 find_operation(\"Ping\")");
    if (ping != nullptr) {
      // 单向 operation：有 input、**没有** output —— 这两件事必须分得开。
      check(ping->has_input, "1.24 Ping 有 input");
      check(!ping->has_output, "1.25 Ping 没有 output（单向）");
      check_eq_s(ping->input.name, "in", "1.26 input/@name");
    }
  }

  const uvcpp_wsdl_binding* b = d.find_binding("B");
  check(b == nullptr, "1.27 不存在的 binding 查到的是 nullptr");
  b = d.find_binding("tns:CalculatorSoapBinding");
  check(b != nullptr, "1.28 find_binding");
  if (b != nullptr) {
    check_eq_s(b->port_type.str(), "{http://example.com/calc}CalculatorPortType",
               "1.29 binding/@type");
    check(b->soap.present && !b->soap.is_soap12, "1.30 soap:binding 是 1.1");
    check_eq_s(b->soap.style, "document", "1.31 soap:binding/@style");
    check_eq_s(b->soap.transport, "http://schemas.xmlsoap.org/soap/http",
               "1.32 soap:binding/@transport");
    check_eq_i(static_cast<long long>(b->operations.size()), 2, "1.33 binding operation 条数");
    const uvcpp_wsdl_binding_operation* ob = b->find_operation("Add");
    check(ob != nullptr, "1.34 find_operation(\"Add\")");
    if (ob != nullptr) {
      check_eq_s(ob->soap_action, "http://example.com/calc/Add", "1.35 soapAction");
      check_eq_s(ob->style, "document", "1.36 soap:operation/@style");
      check_eq_s(ob->effective_style("rpc"), "document", "1.37 effective_style 用自己那份");
      check_eq_s(ob->input_use, "literal", "1.38 soap:body/@use");
      check(ob->input_body_parts.empty(), "1.39 没写 parts = 空（= message 全部 part）");
      check_eq_s(ob->output_use, "literal", "1.40 output 的 soap:body/@use");
    }
    const uvcpp_wsdl_binding_operation* pb = b->find_operation("Ping");
    check(pb != nullptr && pb->style.empty(), "1.41 没写 style 的 operation");
    if (pb != nullptr) {
      check_eq_s(pb->effective_style("document"), "document", "1.42 effective_style 回落到 binding");
      check_eq_s(join(pb->input_body_parts), "text", "1.43 soap:body/@parts 切开了");
    }
  }

  const uvcpp_wsdl_service* s = d.find_service("S");
  check(s == nullptr, "1.44 不存在的 service 查到的是 nullptr");
  s = d.find_service("CalculatorService");
  check(s != nullptr, "1.45 find_service");
  if (s != nullptr && s->ports.size() == 1) {
    check_eq_s(s->ports[0].name, "CalculatorPort", "1.46 port/@name");
    check_eq_s(s->ports[0].binding.str(), "{http://example.com/calc}CalculatorSoapBinding",
               "1.47 port/@binding");
    check_eq_s(s->ports[0].address, "http://example.com/calc", "1.48 soap:address/@location");
    check(!s->ports[0].is_soap12, "1.49 address 来自 soap: 而不是 soap12:");
  } else {
    check(false, "1.50 service 的 port 条数不是 1");
  }

  check(d.types_xml.find("<xsd:schema") != std::string::npos,
        "1.51 types_xml 是整段原样（含它自己的前缀）");
  check(d.root_namespaces.size() >= 4, "1.52 根上的 xmlns 声明被记账");

  check(uvcpp_wsdl_check_references(d, nullptr), "1.53 这份文档的引用是闭合的");
  std::string why;
  check(uvcpp_wsdl_check_references(d, &why) && why.empty(),
        "1.54 通过时不写 why");
}

// =========================================================================
// 第 2 组：命名空间
// =========================================================================

void group_namespaces() {
  const std::string a = fingerprint(must_parse(k_ns_a, "2.0 前缀写法 A"));
  const std::string b = fingerprint(must_parse(k_ns_b, "2.0 默认命名空间写法 B"));
  const std::string c = fingerprint(must_parse(k_ns_c, "2.0 大写前缀写法 C"));
  const std::string d = fingerprint(must_parse(k_ns_d, "2.0 逐元素自声明写法 D"));
  if (g_failures != 0) return;

  check_eq_s(b, a, "2.1 默认命名空间与 wsdl: 前缀给出**同一个模型**");
  check_eq_s(c, a, "2.2 大写前缀与 wsdl: 前缀给出**同一个模型**");
  check_eq_s(d, a, "2.3 每个元素自声明前缀给出**同一个模型**");

  // 负对照：假 URI 上的 soap:binding / soap:body 不能被收下。
  const uvcpp_wsdl_document e = must_parse(k_ns_e_fake_soap, "2.4 假 URI 文档");
  const uvcpp_wsdl_binding* eb = e.find_binding("B");
  check(eb != nullptr, "2.5 假 URI 文档里 binding 本身还在");
  if (eb != nullptr) {
    check(!eb->soap.present, "2.6 假 URI 上的 soap:binding **没有**被当成本库的 SOAP 绑定");
    check(eb->operations.size() == 1 && eb->operations[0].input_use.empty(),
          "2.7 假 URI 上的 soap:body 也没有被收下");
  }

  // 反过来：同一份文档把 URI 改对，就该被收下（"只差一处"的对照）。
  std::string fixed(k_ns_e_fake_soap);
  const std::string fake = "http://example.com/not-soap/";
  fixed.replace(fixed.find(fake), fake.size(),
                "http://schemas.xmlsoap.org/wsdl/soap/");
  const uvcpp_wsdl_document f = must_parse(fixed, "2.8 把 URI 改对");
  const uvcpp_wsdl_binding* fb = f.find_binding("B");
  check(fb != nullptr && fb->soap.present,
        "2.9 URI 改对之后 soap:binding 被收下（2.6 因此不是「什么都没收」）");
  check(fb != nullptr && fb->operations.size() == 1 &&
            fb->operations[0].input_use == "literal",
        "2.10 URI 改对之后 soap:body 也被收下");
}

// =========================================================================
// 第 3 组：序列化
// =========================================================================

void group_roundtrip() {
  const uvcpp_wsdl_document d = must_parse(k_calc, "3.0 完整文档");
  const std::string xml = uvcpp_wsdl_dump(d);
  if (g_failures != 0) return;

  check(xml.find('\r') == std::string::npos, "3.1 产出里没有 CR");
  check(xml.compare(xml.size() - 15, 15, "</definitions>\n") == 0,
        "3.2 产出以 </definitions> 收尾");
  check_eq_s(uvcpp_wsdl_dump(d), xml, "3.3 同一份模型两次 dump 逐字节相同");
  check(xml.find("xmlns:wsdl=") != std::string::npos,
        "3.4 原文档根上的 wsdl 前缀被照搬（否则 types 里的 wsdl: 会没人定义）");

  const uvcpp_wsdl_document back = must_parse(xml, "3.5 重新解析自己的产出");
  if (g_failures != 0) return;
  check_eq_s(fingerprint(back), fingerprint(d), "3.6 语义往返：模型逐字段相等");
  check_eq_s(uvcpp_wsdl_dump(back), xml, "3.7 往返之后 dump 仍然逐字节相同（幂等）");

  // 手写模型（完全不经过 pugixml 那条路）—— "按模型生成 WSDL" 那一档。
  uvcpp_wsdl_document made;
  made.target_namespace = "urn:made";
  made.name = "Made";
  uvcpp_wsdl_message m;
  m.name = "Req";
  uvcpp_wsdl_part p;
  p.name = "body";
  p.element = uvcpp_qname("urn:made", "Req");
  m.parts.push_back(p);
  made.messages.push_back(m);
  uvcpp_wsdl_message m2;
  m2.name = "Rep";
  uvcpp_wsdl_part p2;
  p2.name = "body";
  // 一个**没人声明过**的命名空间：序列化时必须自己给它分配一个前缀出来。
  // 放在 part/@element 上（`check_references` 不查 schema 元素，所以这份文档
  // 的引用仍然是闭合的 —— 想把它放 @message 上就得再补一个 message）。
  p2.element = uvcpp_qname("urn:elsewhere", "RepEl");
  m2.parts.push_back(p2);
  made.messages.push_back(m2);
  uvcpp_wsdl_port_type pt;
  pt.name = "Pt";
  uvcpp_wsdl_operation op;
  op.name = "Go";
  op.has_input = true;
  op.input.message = uvcpp_qname("urn:made", "Req");
  op.output.message = uvcpp_qname("urn:made", "Rep");
  op.has_output = true;
  pt.operations.push_back(op);
  made.port_types.push_back(pt);
  uvcpp_wsdl_binding bd;
  bd.name = "Bd";
  bd.port_type = uvcpp_qname("urn:made", "Pt");
  bd.soap.present = true;
  bd.soap.style = "document";
  uvcpp_wsdl_binding_operation bo;
  bo.name = "Go";
  bo.input_use = "literal";
  bd.operations.push_back(bo);
  made.bindings.push_back(bd);
  uvcpp_wsdl_service sv;
  sv.name = "Sv";
  uvcpp_wsdl_port po;
  po.name = "Po";
  po.binding = uvcpp_qname("urn:made", "Bd");
  po.address = "http://example.com/made";
  sv.ports.push_back(po);
  made.services.push_back(sv);

  const std::string made_xml = uvcpp_wsdl_dump(made);
  check(made_xml.find("xmlns:tns=\"urn:made\"") != std::string::npos,
        "3.8 tns 是保留给 targetNamespace 的");
  check(made_xml.find("urn:elsewhere") != std::string::npos,
        "3.9 没声明过的命名空间也出现在输出里");
  check(made_xml.find("<soap:binding") != std::string::npos,
        "3.10 soap 前缀按需声明");

  const uvcpp_wsdl_document made_back = must_parse(made_xml, "3.11 解析手写模型的产出");
  if (g_failures == 0) {
    check_eq_s(fingerprint(made_back), fingerprint(made),
               "3.12 手写模型的语义往返");
    check(made_back.find_message("{urn:made}Req") != nullptr,
          "3.13 手写模型的 message 查得到");
    check(made_back.find_message("{urn:elsewhere}Req") == nullptr,
          "3.14 urn:elsewhere 里没有被凭空造出 message（3.9 的对照）");
    check(uvcpp_wsdl_check_references(made_back, nullptr),
          "3.15 手写模型的引用闭合（urn:elsewhere 只出现在 part/@element，"
          "而 schema 元素不在引用闭合的检查范围内）");
  }
}

// =========================================================================
// 第 4 组：拒绝档（每条都配一条"只差一处、必须收"）
// =========================================================================

wsdl_status parse_st(const std::string& xml, const char** why = nullptr) {
  uvcpp_wsdl_document d;
  return uvcpp_wsdl_parse(xml, d, uvcpp_wsdl_limits(), why);
}

void group_errors() {
  const char* why = nullptr;
  check_st(parse_st(std::string(), &why), wsdl_status::EMPTY, "4.1 空输入");
  check(why != nullptr && *why != '\0', "4.2 空输入带了说明");

  // 只有空白：**没有根元素** ⇒ 按 XML 的规矩就是不良构，所以是 SYNTAX 而不是
  // NOT_WSDL。（`EMPTY` 留给"一个字节都没有"那一档 —— 那两件事对调用方的意思
  // 完全不同：一个是"你没给东西"，一个是"你给的东西坏了"。）
  check_st(parse_st("   "), wsdl_status::SYNTAX, "4.3 只有空白：没有根元素");
  check_st(parse_st("<definitions"), wsdl_status::SYNTAX, "4.4 标签没闭合");
  check_st(parse_st("<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
                    " targetNamespace=\"urn:x\"/>"),
           wsdl_status::OK, "4.5 只差「闭合」这一处：必须收");

  check_st(parse_st("<other/>"), wsdl_status::NOT_WSDL, "4.6 根元素不对");
  check_st(parse_st("<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\"/>"),
           wsdl_status::NO_TARGET_NAMESPACE, "4.7 缺 targetNamespace");
  check_st(parse_st("<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
                    " targetNamespace=\"\"/>"),
           wsdl_status::NO_TARGET_NAMESPACE, "4.8 targetNamespace 是空串也算缺");
  check_st(parse_st("<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
                    " targetNamespace=\"urn:x\"/>"),
           wsdl_status::OK, "4.9 只差 targetNamespace 这一处：必须收");

  // WSDL 2.0 的根：**先认出版本不对**再说别的 —— 所以哪怕它也缺
  // targetNamespace，报出来的仍然是"这个版本我们不支持"，而不是"缺属性"。
  check_st(parse_st("<description xmlns=\"http://www.w3.org/ns/wsdl\""
                    " targetNamespace=\"urn:x\"/>"),
           wsdl_status::UNSUPPORTED_VERSION, "4.10 WSDL 2.0 的根");
  check_st(parse_st("<description xmlns=\"http://www.w3.org/ns/wsdl\"/>"),
           wsdl_status::UNSUPPORTED_VERSION,
           "4.11 2.0 的根即使缺 targetNamespace，报的也是版本不对");
  check_st(parse_st("<description xmlns=\"http://example.com/nope\""
                    " targetNamespace=\"urn:x\"/>"),
           wsdl_status::NOT_WSDL,
           "4.11a 只差命名空间这一处：挂着 description 这个名字但不是那个 URI，仍是 NOT_WSDL");

  // 字节上限 / 条数上限：同一份**生成**的文档，每次只换一个上限。
  //
  // ★ 这里**不能**靠"把 k_calc 拼两遍把字节数堆上去"来造大文档：解析器只看
  //   根元素**之后**就停（后面多出来的那份是尾巴垃圾，连元素都不算），于是
  //   条目数根本没变多 —— 那样写出来的 4.14 会是一条**永远绿不起来的假判据**
  //   （第一版就是这么写的，跑出来是 `ok`）。
  std::string many =
      "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
      " targetNamespace=\"urn:x\">";
  for (int i = 0; i < 40; ++i) {
    many += "<message name=\"M";
    many += static_cast<char>('0' + i / 10);
    many += static_cast<char>('0' + i % 10);
    many += "\"><part name=\"p\"/></message>";
  }
  many += "</definitions>";
  {
    uvcpp_wsdl_document d;
    check_st(uvcpp_wsdl_parse(many, d, uvcpp_wsdl_limits(1024, 64, 65536, 4096)),
             wsdl_status::TOO_LARGE, "4.12 超过字节上限");
    check_st(uvcpp_wsdl_parse(many, d, uvcpp_wsdl_limits()), wsdl_status::OK,
             "4.13 只差字节上限这一处：同样字节必须收");
    // 正对照的前置：40 条 message 确实被解析出来了（否则 4.13 成了"什么都没收
    // 也算 OK"，4.14 也就无从谈起）。上面那次解析的 `d` 还在。
    check_eq_i(static_cast<long long>(d.messages.size()), 40,
               "4.13a（前置）40 条 message 一条不少");
    check_st(uvcpp_wsdl_parse(many, d, uvcpp_wsdl_limits(1024 * 1024, 64, 65536, 8)),
             wsdl_status::TOO_MANY_ITEMS, "4.14 超过每类条数上限");
    check_st(uvcpp_wsdl_parse(many, d, uvcpp_wsdl_limits(1024 * 1024, 64, 65536, 64)),
             wsdl_status::OK, "4.15 只差条数上限这一处：必须收");
  }

  // 深度上限：1.3 万层嵌套。**这一条同时是"测量不递归"的判据** —— 递归实现
  // 在这种输入上是自己把栈爆掉，而不是返回 TOO_DEEP。
  {
    std::string deep =
        "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
        " targetNamespace=\"urn:x\">";
    for (int i = 0; i < 13000; ++i) deep += "<a>";
    for (int i = 0; i < 13000; ++i) deep += "</a>";
    deep += "</definitions>";
    uvcpp_wsdl_document d;
    check_st(uvcpp_wsdl_parse(deep, d, uvcpp_wsdl_limits()),
             wsdl_status::TOO_DEEP, "4.16 超深文档被拒（而不是崩）");
    check_st(uvcpp_wsdl_parse(deep, d,
                              uvcpp_wsdl_limits(4u * 1024u * 1024u, 20000,
                                                65536u, 4096u)),
             wsdl_status::OK, "4.17 只差深度上限这一处：必须收");
  }

  // 元素总数上限。
  {
    std::string many =
        "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
        " targetNamespace=\"urn:x\">";
    for (int i = 0; i < 40; ++i) many += "<a></a>";
    many += "</definitions>";
    uvcpp_wsdl_document d;
    check_st(uvcpp_wsdl_parse(many, d, uvcpp_wsdl_limits(4u * 1024u * 1024u, 64, 30, 4096)),
             wsdl_status::TOO_MANY_NODES, "4.18 超元素总数上限");
    check_st(uvcpp_wsdl_parse(many, d, uvcpp_wsdl_limits()),
             wsdl_status::OK, "4.19 只差元素总数上限这一处：必须收");
  }

  check_eq_s(wsdl_status_name(wsdl_status::TOO_MANY_ITEMS), "too_many_items",
             "4.20 状态名（判据里印它）");
}

// =========================================================================
// 第 5 组：引用闭合（每条坏文档都配一条好文档）
// =========================================================================

void group_references() {
  const std::string head =
      "<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
      " xmlns:tns=\"urn:x\" targetNamespace=\"urn:x\">";
  const std::string msg = "<message name=\"M\"><part name=\"p\" type=\"tns:T\"/></message>";
  const std::string pt = "<portType name=\"P\"><operation name=\"Op\">"
                         "<input message=\"tns:M\"/></operation></portType>";
  const std::string good = head + msg + pt + "</definitions>";
  const std::string bad_msg = head + pt + "</definitions>";
  const std::string bad_type = head + msg +
                               "<portType name=\"P\"><operation name=\"Op\">"
                               "<input message=\"tns:M\"/></operation></portType>"
                               "<binding name=\"B\" type=\"tns:Nope\"/>"
                               "</definitions>";
  const std::string bad_port = head + "<binding name=\"B\"/>"
                                      "<service name=\"S\"><port name=\"Pt\""
                                      " binding=\"tns:Nope\"/></service>"
                                      "</definitions>";
  const std::string bad_op = head + msg + pt +
                             "<binding name=\"B\" type=\"tns:P\">"
                             "<operation name=\"Nope\"/></binding>"
                             "</definitions>";
  // 反方向**不查**：binding 少绑一个 portType 里的 operation 是合法的。
  const std::string partial = head + msg + pt +
                              "<binding name=\"B\" type=\"tns:P\">"
                              "<operation name=\"Op\"/></binding>"
                              "</definitions>";

  std::string why;
  const uvcpp_wsdl_document g = must_parse(good, "5.0 闭合文档");
  check(uvcpp_wsdl_check_references(g, &why), "5.1 闭合文档通过");
  check(why.empty(), "5.2 通过时不写 why");

  const uvcpp_wsdl_document bm = must_parse(bad_msg, "5.0 缺 message");
  why.clear();
  check(!uvcpp_wsdl_check_references(bm, &why), "5.3 input/@message 指不到 message 时报错");
  check(why.find("M") != std::string::npos, "5.4 说明里点出了那个名字");

  const uvcpp_wsdl_document bt = must_parse(bad_type, "5.0 坏 @type");
  why.clear();
  check(!uvcpp_wsdl_check_references(bt, &why), "5.5 binding/@type 指不到 portType 时报错");

  const uvcpp_wsdl_document bp = must_parse(bad_port, "5.0 坏 @binding");
  why.clear();
  check(!uvcpp_wsdl_check_references(bp, &why), "5.6 port/@binding 指不到 binding 时报错");

  const uvcpp_wsdl_document bo = must_parse(bad_op, "5.0 坏 operation 名");
  why.clear();
  check(!uvcpp_wsdl_check_references(bo, &why),
        "5.7 binding 里的 operation 名在 portType 里不存在时报错");

  const uvcpp_wsdl_document pa = must_parse(partial, "5.0 只绑一部分");
  why.clear();
  check(uvcpp_wsdl_check_references(pa, &why),
        "5.8 binding 只绑 portType 的一部分是**合法**的（反方向不查）");
}

// =========================================================================
// 第 6 组：QName 与转义
// =========================================================================

void group_qname_escape() {
  check_eq_s(uvcpp_qname_local("Add"), "Add", "6.1 无前缀");
  check_eq_s(uvcpp_qname_local("tns:Add"), "Add", "6.2 带前缀");
  check_eq_s(uvcpp_qname_local("{urn:x}Add"), "Add", "6.3 {uri} 形状");
  check_eq_s(uvcpp_qname_local("{urn:x}"), "", "6.4 只有命名空间");
  check_eq_s(uvcpp_qname_local("a:b:c"), "b:c", "6.5 第二个冒号之后都算局部名");

  check_eq_s(uvcpp_qname("urn:x", "Add").str(), "{urn:x}Add", "6.6 qname::str");
  check(uvcpp_qname() == uvcpp_qname("", ""), "6.7 空 qname 相等");
  check(uvcpp_qname("", "Add") != uvcpp_qname("urn:x", "Add"), "6.8 命名空间不同则不相等");

  check_eq_s(uvcpp_xml_escape_text("a<b>&c"), "a&lt;b&gt;&amp;c", "6.9 文本转义");
  check_eq_s(uvcpp_xml_escape_attr("\"q\"&<"), "&quot;q&quot;&amp;&lt;", "6.10 属性转义");
  check_eq_s(uvcpp_xml_escape_text("引号\"不用转\""), "引号\"不用转\"",
             "6.11 文本里的双引号不必须转（也就不转）");
  check_eq_s(uvcpp_xml_escape_attr("'"), "'", "6.12 单引号不转（属性用双引号括）");

  // 转义必须真的承重：把带转义字符的文本塞进模型，dump 出来再解析回来要相同。
  uvcpp_wsdl_document d;
  d.target_namespace = "urn:x";
  d.documentation = "a<b>&\"c\"";
  const std::string xml = uvcpp_wsdl_dump(d);
  check(xml.find("a&lt;b&gt;&amp;\"c\"") != std::string::npos,
        "6.13 dump 里那段是转义过的");
  const uvcpp_wsdl_document back = must_parse(xml, "6.14 解析带转义的产出");
  check_eq_s(back.documentation, d.documentation, "6.15 转义往返相等");
}

}  // namespace

int main() {
  group_parse();
  group_namespaces();
  group_roundtrip();
  group_errors();
  group_references();
  group_qname_escape();

  if (g_failures == 0) {
    std::cout << "[wsdl_document] ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "[wsdl_document] FAIL (" << g_failures << ")" << std::endl;
  return 2;
}

#else  // UVCPP_WSDL_ENABLE

// 关掉 wsdl 模块时**这个文件不该被编译**（`tests/functional/CMakeLists.txt` 的
// 过滤器负责把它摘掉）。所以这里的 main 不打印 SKIP —— 打印 SKIP 的话，
// "过滤器失效"会表现为"通过"，而那正是本仓反复记着的坑（见那个 CMakeLists 里
// 关于 `h2_` 前缀的注释）。这里返回非零：真的编到了就是一个必须修的错。
int main() {
  std::cout << "[wsdl_document] 本用例不该在 UVCPP_WSDL_ENABLE=0 的树里被编译"
            << std::endl;
  return 2;
}

#endif  // UVCPP_WSDL_ENABLE
