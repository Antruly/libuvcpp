/**
 * @file tests/functional/soap_message_func.cpp
 * @brief SOAP 信封层：两个版本的解析、Fault、以及**写回去**。
 *
 * 这一层能错的地方，按"错了会怎样"分四类，判据就按这四类排：
 *
 * 1. **版本认错**（1.x）：把 1.1 的报文当 1.2 处理，Fault 就会带着 `Client`
 *    发出去 —— 良构、但没人认得。所以版本判定与那张"码的本地名表"各有一组。
 * 2. **字段读错位置**（2.x/4.x）：1.1 的 `faultstring` 与 1.2 的 `Reason/Text`
 *    在完全不同的深度上；1.1 的 `detail` 与 1.2 的 `Detail` 大小写不同。
 * 3. **该拒的没拒**（5.x）：不是信封、没有 Body、Body 里两个元素、
 *    Fault 连码都没有 —— 这些都要在**解析**这一层就断，而不是留给上层。
 * 4. **写出来的字节不对**（6.x/7.x）：序列化是**逐字节**钉的。判据不能只比
 *    "能再解析回来"：那对一份缩进全乱的输出也成立。
 *
 * 另有两条**故意宽容**的行为（4.20-4.26）用判据钉住：两个版本的 Fault 子元素是
 * 按**局部名**认的，所以 1.1 的写法出现在 1.2 的信封里也读得出来。这条不是
 * 漏洞，是刻意的宽法（真实世界的实现两种写法都发），钉住是为了将来"收紧"时
 * 会红一条判据，而不是悄悄改行为。
 */
#include <iostream>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WSDL_ENABLE

#include <wsdl/uvcpp_soap_message.h>

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
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
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

/** 解析并**断言成功**；成功时把 `why` 印出来，失败时判据自己带上原因。 */
bool parse_ok(const std::string& xml, uvcpp_soap_message& out,
              const std::string& what) {
  const char* why = "";
  const wsdl_status st = uvcpp_soap_parse(xml, out, uvcpp_wsdl_limits(), &why);
  if (st != wsdl_status::OK) {
    std::cerr << "  [FAIL] " << what << "：解析失败 " << wsdl_status_name(st)
              << "（" << why << "）" << std::endl;
    ++g_failures;
    return false;
  }
  return true;
}

/** 解析并**断言失败于某个码**。 */
void parse_fails(const std::string& xml, wsdl_status want,
                 const std::string& what,
                 const uvcpp_wsdl_limits& lim = uvcpp_wsdl_limits()) {
  uvcpp_soap_message m;
  const wsdl_status st = uvcpp_soap_parse(xml, m, lim);
  if (st != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: "
              << wsdl_status_name(want) << "\n         实际: "
              << wsdl_status_name(st) << std::endl;
    ++g_failures;
  }
}

const char* k_env11 = "http://schemas.xmlsoap.org/soap/envelope/";
const char* k_env12 = "http://www.w3.org/2003/05/soap-envelope";

/** 一份最普通的 1.1 调用（Body 里一个元素、有 Header、两个头里一个 mustUnderstand）。 */
const char* k_call11 =
    "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " xmlns:tns=\"urn:calc\">\n"
    "  <soap:Header>\n"
    "    <tns:Trace soap:mustUnderstand=\"1\">abc</tns:Trace>\n"
    "    <tns:Hint>quiet</tns:Hint>\n"
    "  </soap:Header>\n"
    "  <soap:Body>\n"
    "    <tns:Add><a>1</a><b>2</b></tns:Add>\n"
    "  </soap:Body>\n"
    "</soap:Envelope>\n";

/** 1.1 的 Fault 响应（四个子元素**不带命名空间**，注意是小写）。 */
const char* k_fault11 =
    "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
    "  <soap:Body>\n"
    "    <soap:Fault>\n"
    "      <faultcode>soap:Client</faultcode>\n"
    "      <faultstring>bad input</faultstring>\n"
    "      <faultactor>urn:me</faultactor>\n"
    "      <detail><err xmlns=\"urn:e\">boom</err></detail>\n"
    "    </soap:Fault>\n"
    "  </soap:Body>\n"
    "</soap:Envelope>\n";

/**
 * 1.2 的 Fault 响应（全在信封命名空间里、**大写**、有 Subcode 与 xml:lang）。
 *
 * 这份文本同时是 7.3 那条字节级往返的基准 —— 所以它必须写成**本层 dump 出来的
 * 样子**（每行一个元素、缩进两空格）。★ `Subcode` 在这里是**展开的三行**而不是
 * 挤成一行：本层的 dump 一定会展开它（`fault_element_block()` 逐行发），挤一行的
 * 原文过不了 7.3。这不是让判据迁就实现 —— "往返"这个判据本来就以**规范形**为
 * 基准，输入也得以规范形写。
 */
const char* k_fault12 =
    "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">\n"
    "  <soap:Body>\n"
    "    <soap:Fault>\n"
    "      <soap:Code>\n"
    "        <soap:Value>soap:Sender</soap:Value>\n"
    "        <soap:Subcode>\n"
    "          <soap:Value>soap:BadArgs</soap:Value>\n"
    "        </soap:Subcode>\n"
    "      </soap:Code>\n"
    "      <soap:Reason>\n"
    "        <soap:Text xml:lang=\"zh\">参数不对</soap:Text>\n"
    "      </soap:Reason>\n"
    "      <soap:Node>urn:me</soap:Node>\n"
    "      <soap:Detail><err xmlns=\"urn:e\">boom</err></soap:Detail>\n"
    "    </soap:Fault>\n"
    "  </soap:Body>\n"
    "</soap:Envelope>\n";

// =========================================================================
// 1. 版本、content-type、动作、码表（都不需要 XML）
// =========================================================================

void test_version_and_admission() {
  check_eq_s(uvcpp_soap_version_name(uvcpp_soap_version::V1_1), "1.1", "1.1 版本名");
  check_eq_s(uvcpp_soap_version_name(uvcpp_soap_version::V1_2), "1.2", "1.2 版本名");

  // 信封常量与 7a 那对是**同一对**（不许出现两处字面量）。
  check_eq_s(uvcpp_soap_envelope_ns(uvcpp_soap_version::V1_1), k_env11,
             "1.2 1.1 信封命名空间");
  check_eq_s(uvcpp_soap_envelope_ns(uvcpp_soap_version::V1_2), k_env12,
             "1.3 1.2 信封命名空间");
  check_eq_s(std::string(wsdl_ns::soap11_envelope()), k_env11,
             "1.4 与 wsdl_ns 那对是同一串");
  check_eq_s(std::string(wsdl_ns::soap12_envelope()), k_env12, "1.5 同上");

  check_eq_s(uvcpp_soap_content_type(uvcpp_soap_version::V1_1),
             "text/xml; charset=utf-8", "1.6 1.1 的 content-type");
  check_eq_s(uvcpp_soap_content_type(uvcpp_soap_version::V1_2),
             "application/soap+xml; charset=utf-8", "1.7 1.2 的 content-type");

  struct ct_case {
    const char* ct;
    bool is_soap;
    uvcpp_soap_version v;
  };
  const ct_case cts[] = {
      {"text/xml", true, uvcpp_soap_version::V1_1},
      {"text/xml; charset=utf-8", true, uvcpp_soap_version::V1_1},
      {"TEXT/XML", true, uvcpp_soap_version::V1_1},          // Type/Subtype 大小写不敏感
      {"text/xml ; charset=utf-8", true, uvcpp_soap_version::V1_1},
      {"application/soap+xml", true, uvcpp_soap_version::V1_2},
      {"application/soap+xml; charset=utf-8; action=\"urn:calc#Add\"", true,
       uvcpp_soap_version::V1_2},
      {"application/soap+xml; action=\"urn:a;b\"", true, uvcpp_soap_version::V1_2},
      {"text/plain", false, uvcpp_soap_version::V1_1},
      {"application/xml", false, uvcpp_soap_version::V1_1},  // 不是 text/xml
      {"", false, uvcpp_soap_version::V1_1},
  };
  for (size_t i = 0; i < sizeof(cts) / sizeof(cts[0]); ++i) {
    uvcpp_soap_version v = uvcpp_soap_version::V1_1;
    const bool got = uvcpp_soap_version_of_content_type(cts[i].ct, v);
    std::string what = std::string("1.8 content-type [") + cts[i].ct + "]";
    check_eq_i(got ? 1 : 0, cts[i].is_soap ? 1 : 0, what + " 判是不是 SOAP");
    if (got) {
      check_eq_i(static_cast<int>(v), static_cast<int>(cts[i].v),
                 what + " 判出来的版本");
    }
  }

  // action 参数：带引号 / 不带引号 / 值里有 `;`（引号里不是分隔符）。
  check_eq_s(uvcpp_soap_action_of_content_type(
                 "application/soap+xml; action=\"urn:calc#Add\""),
             "\"urn:calc#Add\"", "1.9 action 取原样值（带引号）");
  check_eq_s(uvcpp_soap_normalize_action("\"urn:calc#Add\""), "urn:calc#Add",
             "1.10 归一掉引号");
  check_eq_s(uvcpp_soap_normalize_action("  \"urn:x\"  "), "urn:x",
             "1.11 归一掉两端空白");
  check_eq_s(uvcpp_soap_normalize_action("\"\""), "", "1.12 `\"\"` 归一成空串");
  check_eq_s(uvcpp_soap_normalize_action("urn:x"), "urn:x", "1.13 没引号就原样");
  check_eq_s(uvcpp_soap_action_of_content_type(
                 "application/soap+xml; action=urn:x; charset=utf-8"),
             "urn:x", "1.14 不带引号的 action");
  check_eq_s(uvcpp_soap_normalize_action(uvcpp_soap_action_of_content_type(
                 "application/soap+xml; action=\"urn:a;b\"")),
             "urn:a;b", "1.15 值里的 `;` 不算分隔符");
  check_eq_s(uvcpp_soap_action_of_content_type("text/xml"), "",
             "1.16 没有 action 时是空串");

  // 码的本地名表：1.1 → 1.2 那三处改名 / 一处不存在。
  check_eq_s(uvcpp_soap_fault_code_local(uvcpp_soap_fault_code::CLIENT,
                                        uvcpp_soap_version::V1_1),
             "Client", "1.17 1.1 Client");
  check_eq_s(uvcpp_soap_fault_code_local(uvcpp_soap_fault_code::CLIENT,
                                        uvcpp_soap_version::V1_2),
             "Sender", "1.18 1.2 Sender（**改名了**）");
  check_eq_s(uvcpp_soap_fault_code_local(uvcpp_soap_fault_code::SERVER,
                                        uvcpp_soap_version::V1_1),
             "Server", "1.19 1.1 Server");
  check_eq_s(uvcpp_soap_fault_code_local(uvcpp_soap_fault_code::SERVER,
                                        uvcpp_soap_version::V1_2),
             "Receiver", "1.20 1.2 Receiver（**改名了**）");
  check_eq_s(uvcpp_soap_fault_code_local(uvcpp_soap_fault_code::MUST_UNDERSTAND,
                                        uvcpp_soap_version::V1_1),
             "MustUnderstand", "1.21 MustUnderstand 两版同名");
  check_eq_s(uvcpp_soap_fault_code_local(uvcpp_soap_fault_code::VERSION_MISMATCH,
                                        uvcpp_soap_version::V1_2),
             "VersionMismatch", "1.22 VersionMismatch 两版同名");
  check_eq_s(uvcpp_soap_fault_code_local(
                 uvcpp_soap_fault_code::DATA_ENCODING_UNKNOWN,
                 uvcpp_soap_version::V1_2),
             "DataEncodingUnknown", "1.23 1.2 有这个码");
  check_eq_s(uvcpp_soap_fault_code_local(
                 uvcpp_soap_fault_code::DATA_ENCODING_UNKNOWN,
                 uvcpp_soap_version::V1_1),
             "", "1.24 1.1 **没有**这个码 —— 空串是刻意的");

  // 归一的补一条。放在组尾而不是插在 1.11 旁边：判据号是**稳定的引用**（提交信息、
  // 复盘都按号说话），插一条就把后面每一条的号都改了。所以新的往后排。
  check_eq_s(uvcpp_soap_normalize_action("\" urn:x \""), "urn:x",
             "1.25 ★ 引号**里**多出来的空白也归一掉（否则 `\" urn:x \"` 与 "
             "`\"urn:x\"` 是两个值，同一份 WSDL 会对两种写法一个收一个拒）");
}

// =========================================================================
// 2. 解析：1.1 的调用
// =========================================================================

void test_parse_call() {
  uvcpp_soap_message m;
  if (!parse_ok(k_call11, m, "2.0 样例 1.1 调用")) return;

  check_eq_i(static_cast<int>(m.version), static_cast<int>(uvcpp_soap_version::V1_1),
             "2.1 认成 1.1");
  check_eq_s(m.body_local, "Add", "2.2 Body 第一个元素的局部名");
  check_eq_s(m.body_ns, "urn:calc", "2.3 它的命名空间（前缀展开成 URI）");
  check_eq_s(m.body_qname(), "{urn:calc}Add", "2.4 body_qname()");
  check_eq_i(static_cast<long long>(m.body_children), 1, "2.5 Body 的子元素数");
  check(m.is_call(), "2.6 is_call()");
  check(!m.has_fault, "2.7 has_fault 为假");
  check_eq_s(m.body_xml, "<tns:Add><a>1</a><b>2</b></tns:Add>",
             "2.8 body_xml 是**整个元素**的原样文本");

  check_eq_i(static_cast<long long>(m.header_entries), 2, "2.9 Header 的条目数");
  check_eq_i(static_cast<long long>(m.header_must_understand), 1,
             "2.10 mustUnderstand 计数");
  // ★ 2.11 钉住"原样"的**边界**：XML 后端没开 `pugi::parse_ws_pcdata`，元素
  //   之间的**纯空白**文本节点根本不进树，所以整段取回来是**塌掉**的（换行与
  //   缩进都没了）。这不是缺陷，是刻意的（见 .h 里"原样文本"那段）；钉住它是
  //   因为开不开那个标志会**静默**改变所有片段的样子。
  check_eq_s(m.header_xml,
             "<soap:Header><tns:Trace soap:mustUnderstand=\"1\">abc</tns:Trace>"
             "<tns:Hint>quiet</tns:Hint></soap:Header>",
             "2.11 header_xml 是整段（含自己的标签；元素之间的空白已塌掉）");
}

void test_must_understand_forms() {
  // `xsd:boolean` 的两种真写法、以及"带前缀但前缀指别处"的负例。
  const char* forms[] = {
      "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
      "<soap:Header><h soap:mustUnderstand=\"1\"/></soap:Header>"
      "<soap:Body><x/></soap:Body></soap:Envelope>",
      // 裸属性（不带前缀 = 没有命名空间）：也算
      "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
      "<soap:Header><h mustUnderstand=\"true\"/></soap:Header>"
      "<soap:Body><x/></soap:Body></soap:Envelope>",
  };
  for (int i = 0; i < 2; ++i) {
    uvcpp_soap_message m;
    if (!parse_ok(forms[i], m, "2.12 mustUnderstand 的写法")) continue;
    check_eq_i(static_cast<long long>(m.header_must_understand), 1,
               "2.13 mustUnderstand=\"1\"/\"true\" 都算真");
  }

  const char* falses[] = {
      "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
      "<soap:Header><h soap:mustUnderstand=\"0\"/></soap:Header>"
      "<soap:Body><x/></soap:Body></soap:Envelope>",
      "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
      "<soap:Header><h soap:mustUnderstand=\"yes\"/></soap:Header>"
      "<soap:Body><x/></soap:Body></soap:Envelope>",
      // 前缀指着**别处**：那是同名扩展，不是 SOAP 的属性
      "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\""
      " xmlns:o=\"urn:other\">"
      "<soap:Header><h o:mustUnderstand=\"1\"/></soap:Header>"
      "<soap:Body><x/></soap:Body></soap:Envelope>",
  };
  for (int i = 0; i < 3; ++i) {
    uvcpp_soap_message m;
    if (!parse_ok(falses[i], m, "2.14 mustUnderstand 的负例")) continue;
    check_eq_i(static_cast<long long>(m.header_must_understand), 0,
               "2.15 `0`/`yes`/前缀指别处 都不算真");
  }
}

void test_no_header_and_default_ns() {
  // 没有 Header：三个字段都是空的，但一切都正常。
  uvcpp_soap_message m;
  if (!parse_ok("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><x/></s:Body></s:Envelope>",
                m, "2.16 没有 Header")) {
    return;
  }
  check(m.header_xml.empty() && m.header_entries == 0 &&
            m.header_must_understand == 0,
        "2.17 没有 Header 时三处都是 0/空");
  check_eq_s(m.body_local, "x", "2.18 一个元素也认");
  check(m.body_ns.empty(), "2.19 它没有命名空间（不是默认命名空间，是**没有**）");

  // 前缀叫什么无所谓；信封与 Body 用**默认命名空间**也认。
  uvcpp_soap_message d;
  if (!parse_ok("<Envelope xmlns=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<Body><Add xmlns=\"urn:calc\"/></Body></Envelope>",
                d, "2.20 默认命名空间写法")) {
    return;
  }
  check_eq_i(static_cast<int>(d.version), static_cast<int>(uvcpp_soap_version::V1_1),
             "2.21 默认命名空间的信封也认得出 1.1");
  check_eq_s(d.body_ns, "urn:calc", "2.22 Body 子元素自己的默认命名空间");
}

// =========================================================================
// 3. 解析：1.2
// =========================================================================

void test_parse_12() {
  const std::string call =
      "<env:Envelope xmlns:env=\"http://www.w3.org/2003/05/soap-envelope\""
      " xmlns:tns=\"urn:calc\">"
      "<env:Body><tns:Add/></env:Body></env:Envelope>";
  uvcpp_soap_message m;
  if (!parse_ok(call, m, "3.0 1.2 调用")) return;
  check_eq_i(static_cast<int>(m.version), static_cast<int>(uvcpp_soap_version::V1_2),
             "3.1 认成 1.2（前缀叫 env 也一样）");
  check_eq_s(m.body_local, "Add", "3.2 局部名");
  check_eq_s(m.body_ns, "urn:calc", "3.3 命名空间");

  // 1.2 的信封里用 1.2 的 Header 名（`env:Header`）+ 一个 mustUnderstand。
  const std::string hdr =
      "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">"
      "<soap:Header><h soap:mustUnderstand=\"true\"/></soap:Header>"
      "<soap:Body><x/></soap:Body></soap:Envelope>";
  uvcpp_soap_message h;
  if (!parse_ok(hdr, h, "3.4 1.2 的 Header")) return;
  check_eq_i(static_cast<long long>(h.header_entries), 1, "3.5 Header 条目数");
  check_eq_i(static_cast<long long>(h.header_must_understand), 1,
             "3.6 1.2 的 mustUnderstand");
}

// =========================================================================
// 4. Fault（两个版本 + 两条故意宽容的）
// =========================================================================

void test_fault11() {
  uvcpp_soap_message m;
  if (!parse_ok(k_fault11, m, "4.0 1.1 Fault")) return;
  check(m.has_fault, "4.1 has_fault");
  check(!m.is_call(), "4.2 is_call() 为假（对端在报错）");
  check_eq_i(static_cast<int>(m.fault.version), static_cast<int>(uvcpp_soap_version::V1_1),
             "4.3 Fault 的版本跟着信封");
  check_eq_s(m.fault.code, "Client", "4.4 faultcode 的局部名");
  check_eq_s(m.fault.code_space, k_env11, "4.5 faultcode 的命名空间（`soap:` 翻出来了）");
  check_eq_s(m.fault.reason, "bad input", "4.6 faultstring");
  check_eq_s(m.fault.role, "urn:me", "4.7 faultactor -> role");
  check(m.fault.node.empty() && m.fault.subcode.empty() && m.fault.lang.empty(),
        "4.8 1.1 里没有的字段是空的");
  check_eq_s(m.fault.detail, "<detail><err xmlns=\"urn:e\">boom</err></detail>",
             "4.9 detail 是**整个元素**");
}

void test_fault12() {
  uvcpp_soap_message m;
  if (!parse_ok(k_fault12, m, "4.10 1.2 Fault")) return;
  check(m.has_fault, "4.11 has_fault");
  check_eq_s(m.fault.code, "Sender", "4.12 Code/Value 的局部名");
  check_eq_s(m.fault.code_space, k_env12, "4.13 它的命名空间");
  check_eq_s(m.fault.subcode, "BadArgs", "4.14 Code/Subcode/Value");
  check_eq_s(m.fault.reason, "参数不对", "4.15 Reason/Text");
  check_eq_s(m.fault.lang, "zh", "4.16 Reason/Text 的 xml:lang");
  check_eq_s(m.fault.node, "urn:me", "4.17 Node");
  check(m.fault.role.empty(), "4.18 这条里没有 Role");
  check_eq_s(m.fault.detail, "<soap:Detail><err xmlns=\"urn:e\">boom</err></soap:Detail>",
             "4.19 1.2 的 Detail（**大写**）也是整段");
}

void test_fault_leniency() {
  // 4.20/4.21：两个版本的 Fault 子元素按**局部名**认，所以混写也读得出来。
  // 这两条钉的是**刻意的宽法**（见文件头），不是"碰巧能过"。
  const std::string v11_names_in_12 =
      "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">"
      "<soap:Body><soap:Fault><faultcode>soap:Sender</faultcode>"
      "<faultstring>x</faultstring></soap:Fault></soap:Body></soap:Envelope>";
  uvcpp_soap_message a;
  if (parse_ok(v11_names_in_12, a, "4.20 1.1 写法的 Fault 出现在 1.2 的信封里")) {
    check_eq_s(a.fault.code, "Sender", "4.21 照样读出码");
    check_eq_s(a.fault.reason, "x", "4.22 照样读出说明");
    check_eq_i(static_cast<int>(a.fault.version),
               static_cast<int>(uvcpp_soap_version::V1_2),
               "4.23 Fault 的版本仍跟着**信封**（不是跟着子元素怎么写的）");
  }

  const std::string v12_names_in_11 =
      "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
      "<soap:Body><soap:Fault><soap:Code><soap:Value>soap:Client</soap:Value>"
      "</soap:Code><soap:Reason><soap:Text>y</soap:Text></soap:Reason>"
      "</soap:Fault></soap:Body></soap:Envelope>";
  uvcpp_soap_message b;
  if (parse_ok(v12_names_in_11, b, "4.24 1.2 写法的 Fault 出现在 1.1 的信封里")) {
    check_eq_s(b.fault.code, "Client", "4.25 照样读出码");
    check_eq_s(b.fault.reason, "y", "4.26 照样读出说明");
  }

  // 没有 lang 的 Text：留空（**不**替它猜一个）。
  const std::string no_lang =
      "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">"
      "<soap:Body><soap:Fault><soap:Code><soap:Value>soap:Sender</soap:Value>"
      "</soap:Code><soap:Reason><soap:Text>z</soap:Text></soap:Reason>"
      "</soap:Fault></soap:Body></soap:Envelope>";
  uvcpp_soap_message c;
  if (parse_ok(no_lang, c, "4.27 没有 xml:lang 的 Text")) {
    check(c.fault.lang.empty(), "4.28 读不到 lang 就留空");
  }
}

// =========================================================================
// 5. 该拒的
// =========================================================================

void test_rejections() {
  parse_fails(std::string(), wsdl_status::EMPTY, "5.1 空输入");

  parse_fails("<soap:Envelope", wsdl_status::SYNTAX, "5.2 不是良构的 XML");
  parse_fails("这不是 XML", wsdl_status::SYNTAX, "5.3 根本不是 XML");

  // 一份 WSDL 文档不是 SOAP 信封 —— 两个模块的边界在这里各就各位。
  parse_fails("<definitions xmlns=\"http://schemas.xmlsoap.org/wsdl/\""
              " targetNamespace=\"urn:x\"/>",
              wsdl_status::SOAP_NOT_ENVELOPE, "5.4 根不是 Envelope");

  // 命名空间对、局部名不对（或反过来）：都不是信封。
  parse_fails("<soap:EnvelopeX xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\"/>",
              wsdl_status::SOAP_NOT_ENVELOPE, "5.5 局部名不对");
  parse_fails("<soap:Envelope xmlns:soap=\"urn:not-soap\"/>",
              wsdl_status::SOAP_NOT_ENVELOPE, "5.6 命名空间不对");

  parse_fails("<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\"/>",
              wsdl_status::SOAP_NO_BODY, "5.7 没有 Body");
  parse_fails("<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
              "<soap:Body/></soap:Envelope>",
              wsdl_status::SOAP_NO_BODY, "5.8 Body 是空的");
  parse_fails("<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
              "<soap:Body><a/><b/></soap:Body></soap:Envelope>",
              wsdl_status::SOAP_TOO_MANY_BODY_ELEMENTS, "5.9 Body 里两个元素");

  parse_fails("<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
              "<soap:Body><soap:Fault><faultstring>no code</faultstring>"
              "</soap:Fault></soap:Body></soap:Envelope>",
              wsdl_status::SOAP_BAD_FAULT, "5.10 Fault 连码都没有");
  parse_fails("<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">"
              "<soap:Body><soap:Fault><soap:Reason><soap:Text>x</soap:Text>"
              "</soap:Reason></soap:Fault></soap:Body></soap:Envelope>",
              wsdl_status::SOAP_BAD_FAULT, "5.11 1.2 的 Fault 没有 Code");

  // 上限（三个上限各一条；`max_items` 那条挂在 Header 上）。
  parse_fails(std::string(k_call11), wsdl_status::TOO_LARGE, "5.12 超字节数",
              uvcpp_wsdl_limits(16u));
  std::string deep = "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                     "<soap:Body>";
  for (int i = 0; i < 80; ++i) deep += "<a>";
  for (int i = 0; i < 80; ++i) deep += "</a>";
  deep += "</soap:Body></soap:Envelope>";
  parse_fails(deep, wsdl_status::TOO_DEEP, "5.13 超深度");
  parse_fails(k_call11, wsdl_status::TOO_MANY_ITEMS, "5.14 Header 条目超上限",
              uvcpp_wsdl_limits(4u * 1024u * 1024u, 64u, 65536u, 1u));

  // 失败之后 `out` 必须是干净的（不留半份模型给上层）。
  uvcpp_soap_message m;
  uvcpp_soap_parse(std::string(k_call11), m);
  const wsdl_status st = uvcpp_soap_parse("<soap:Envelope xmlns:soap=\"urn:x\"/>", m);
  check(st == wsdl_status::SOAP_NOT_ENVELOPE && m.body_local.empty() &&
            m.header_entries == 0 && !m.has_fault,
        "5.15 失败时 out 被清干净");
}

// =========================================================================
// 6. 序列化（逐字节）
// =========================================================================

void test_dump_envelope() {
  const std::string e = uvcpp_soap_dump_envelope(
      uvcpp_soap_version::V1_1, std::string(), "<x/>");
  check_eq_s(e,
             "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
             "  <soap:Body>\n"
             "    <x/>\n"
             "  </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.1 只有 Body 的信封");

  check_eq_s(uvcpp_soap_dump_envelope(uvcpp_soap_version::V1_2, std::string(),
                                      std::string()),
             "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">\n"
             "  <soap:Body>\n"
             "  </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.2 1.2 的空 Body（空 Body 是**能组出来**的，判它该不该发不是这一层的事）");

  check_eq_s(uvcpp_soap_dump_envelope(
                 uvcpp_soap_version::V1_1, "<h1/>", "<x/>",
                 uvcpp_soap_dump_options("env", 2u)),
             "<env:Envelope xmlns:env=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
             "  <env:Header>\n"
             "    <h1/>\n"
             "  </env:Header>\n"
             "  <env:Body>\n"
             "    <x/>\n"
             "  </env:Body>\n"
             "</env:Envelope>\n",
             "6.3 换前缀 + 带 Header");

  // 前缀为空 ⇒ 根上写 `xmlns=`（默认命名空间）。这条同时钉住"前缀不是写死的"。
  check_eq_s(uvcpp_soap_dump_envelope(uvcpp_soap_version::V1_1, std::string(),
                                      "<x/>", uvcpp_soap_dump_options("")),
             "<Envelope xmlns=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
             "  <Body>\n"
             "    <x/>\n"
             "  </Body>\n"
             "</Envelope>\n",
             "6.4 前缀为空 -> 默认命名空间");

  // 缩进宽度可配；定长两处（一层与两层）都要跟着变。
  check_eq_s(uvcpp_soap_dump_envelope(uvcpp_soap_version::V1_1, std::string(),
                                      "<x/>", uvcpp_soap_dump_options("soap", 4u)),
             "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
             "    <soap:Body>\n"
             "        <x/>\n"
             "    </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.5 缩进宽度可配");
}

void test_dump_response() {
  check_eq_s(uvcpp_soap_dump_response(uvcpp_soap_version::V1_1, "AddResponse",
                                      "urn:calc", "<result>3</result>"),
             "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
             "  <soap:Body>\n"
             "    <AddResponse xmlns=\"urn:calc\">\n"
             "      <result>3</result>\n"
             "    </AddResponse>\n"
             "  </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.6 1.1 响应逐字节");

  // 没有命名空间：**不发** xmlns（合法：document/literal 的服务可以不声明）。
  check_eq_s(uvcpp_soap_dump_response(uvcpp_soap_version::V1_1, "Ping", "",
                                      std::string()),
             "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
             "  <soap:Body>\n"
             "    <Ping/>\n"
             "  </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.7 无命名空间、无内容 -> 自闭合");

  // 命名空间里的 `&` 要转义（属性值）。
  check_eq_s(uvcpp_soap_dump_response(uvcpp_soap_version::V1_2, "R", "urn:a&b",
                                      "<v/>"),
             "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">\n"
             "  <soap:Body>\n"
             "    <R xmlns=\"urn:a&amp;b\">\n"
             "      <v/>\n"
             "    </R>\n"
             "  </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.8 属性值里的 `&` 被转义");
}

void test_dump_fault() {
  const uvcpp_soap_fault f11 =
      uvcpp_soap_make_fault(uvcpp_soap_version::V1_1, uvcpp_soap_fault_code::CLIENT,
                            "bad input");
  check_eq_s(uvcpp_soap_dump_fault(f11),
             "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">\n"
             "  <soap:Body>\n"
             "    <soap:Fault>\n"
             "      <faultcode>soap:Client</faultcode>\n"
             "      <faultstring>bad input</faultstring>\n"
             "    </soap:Fault>\n"
             "  </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.9 1.1 Fault 逐字节（子元素**裸名**、小写）");

  const uvcpp_soap_fault f12 = uvcpp_soap_make_fault(
      uvcpp_soap_version::V1_2, uvcpp_soap_fault_code::CLIENT, "bad input");
  check_eq_s(uvcpp_soap_dump_fault(f12),
             "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\">\n"
             "  <soap:Body>\n"
             "    <soap:Fault>\n"
             "      <soap:Code>\n"
             "        <soap:Value>soap:Sender</soap:Value>\n"
             "      </soap:Code>\n"
             "      <soap:Reason>\n"
             "        <soap:Text xml:lang=\"en\">bad input</soap:Text>\n"
             "      </soap:Reason>\n"
             "    </soap:Fault>\n"
             "  </soap:Body>\n"
             "</soap:Envelope>\n",
             "6.10 1.2 Fault 逐字节（**大写**、全在信封命名空间里、`Sender`）");

  // ★ 同一句 reason、同一个语义码，两个版本发出来的**码不一样** —— 这正是
  //   那张表要防的错。
  check(uvcpp_soap_dump_fault(f11).find("<faultcode>soap:Client</faultcode>") !=
            std::string::npos &&
            uvcpp_soap_dump_fault(f12).find("<soap:Value>soap:Sender</soap:Value>") !=
                std::string::npos,
        "6.11 语义码按版本翻成 Client / Sender");

  // `DATA_ENCODING_UNKNOWN` 在 1.1 里不存在 -> 码是空串（而不是硬凑一个）。
  const uvcpp_soap_fault bad_code = uvcpp_soap_make_fault(
      uvcpp_soap_version::V1_1, uvcpp_soap_fault_code::DATA_ENCODING_UNKNOWN, "x");
  check(bad_code.code.empty(), "6.12 1.1 里不存在的码是空串");
  // 空码写出来就是 `soap:`（一个非法 QName）—— 这里**如实钉住**这个结果，而不是
  // 替它编一个名字。保证这种 Fault 永远不会被发出去是 `uvcpp_soap_service` 的事：
  // 它只用规范里定义的那几个码造 Fault。
  check_eq_s(uvcpp_soap_dump_fault_element(bad_code, "soap"),
             "<soap:Fault>\n"
             "  <faultcode>soap:</faultcode>\n"
             "  <faultstring>x</faultstring>\n"
             "</soap:Fault>",
             "6.13 空码就是空码（不替它猜）");

  // Subcode / Node / Role / reason 里的 `<` 与 `&`；`detail` 那行**原样**嵌。
  uvcpp_soap_fault rich;
  rich.version = uvcpp_soap_version::V1_2;
  rich.code = "Receiver";
  rich.subcode = "Timeout";
  rich.reason = "a<b & c";
  rich.lang = "zh";
  rich.node = "urn:n";
  rich.role = "urn:r";
  rich.detail = "<d xmlns=\"urn:e\">1</d>";
  check_eq_s(uvcpp_soap_dump_fault_element(rich, "soap"),
             "<soap:Fault>\n"
             "  <soap:Code>\n"
             "    <soap:Value>soap:Receiver</soap:Value>\n"
             "    <soap:Subcode>\n"
             "      <soap:Value>soap:Timeout</soap:Value>\n"
             "    </soap:Subcode>\n"
             "  </soap:Code>\n"
             "  <soap:Reason>\n"
             "    <soap:Text xml:lang=\"zh\">a&lt;b &amp; c</soap:Text>\n"
             "  </soap:Reason>\n"
             "  <soap:Node>urn:n</soap:Node>\n"
             "  <soap:Role>urn:r</soap:Role>\n"
             "  <d xmlns=\"urn:e\">1</d>\n"
             "</soap:Fault>",
             "6.14 全字段 + 文本转义（`detail` 是它自己存的那整段）");

  // `make_fault()` 把 detail 的**内容**包成整个元素（1.1 小写 / 1.2 大写），
  // 与解析出来那份"整段原样"是同一个约定 —— 两条路径在这里合流。
  const uvcpp_soap_fault made11 = uvcpp_soap_make_fault(
      uvcpp_soap_version::V1_1, uvcpp_soap_fault_code::SERVER, "oops", "<e/>");
  check_eq_s(made11.detail, "<detail><e/></detail>",
             "6.15 make_fault 把内容包成 1.1 的 detail");
  const uvcpp_soap_fault made12 = uvcpp_soap_make_fault(
      uvcpp_soap_version::V1_2, uvcpp_soap_fault_code::SERVER, "oops", "<e/>");
  check_eq_s(made12.detail, "<Detail><e/></Detail>",
             "6.16 1.2 包成 **Detail**（大写）");

  // `dump_fault_element` 以自己为第 0 层，`dump_fault` 里那份在信封的第 2 层 ——
  // 同一个 Fault 两处的子元素缩进**差两层**（头文件里写着这条）。
  check(uvcpp_soap_dump_fault(f11).find("\n      <faultcode>") !=
                std::string::npos &&
            uvcpp_soap_dump_fault_element(f11, "soap")
                    .find("\n  <faultcode>") != std::string::npos,
        "6.17 包在信封里与单独一份的缩进差两层");
}

// =========================================================================
// 7. 往返：写出来、再读回来，字段必须一样
// =========================================================================

void test_roundtrip() {
  // (a) 1.1：用**样例文档本身**当往返的基准 —— 解析回来的模型再写出去，
  //     必须与原文**逐字节**相同（原文就是按本层的排版写的）。
  uvcpp_soap_message a;
  if (parse_ok(k_fault11, a, "7.0 解析样例 1.1 Fault")) {
    check_eq_s(uvcpp_soap_dump_fault(a.fault), std::string(k_fault11),
               "7.1 1.1 Fault 的字节级往返");
  }
  uvcpp_soap_message b;
  if (parse_ok(k_fault12, b, "7.2 解析样例 1.2 Fault")) {
    check_eq_s(uvcpp_soap_dump_fault(b.fault), std::string(k_fault12),
               "7.3 1.2 Fault 的字节级往返");
  }

  // (b) 造一条带 detail 的 Fault，写出去再读回来：字段逐条相等。
  const uvcpp_soap_fault made = uvcpp_soap_make_fault(
      uvcpp_soap_version::V1_1, uvcpp_soap_fault_code::SERVER, "oops",
      "<err:x xmlns:err=\"urn:e\">1 &amp; 2</err:x>");
  const std::string bytes = uvcpp_soap_dump_fault(made);
  uvcpp_soap_message back;
  if (parse_ok(bytes, back, "7.4 解析自己写出来的 Fault")) {
    check(back.has_fault, "7.5 读回来是一个 Fault");
    check_eq_s(back.fault.code, made.code, "7.6 码相等");
    // ★ 这里**不能**比 `made.code_space`：`make_fault()` 不填它（发的时候前缀由
    //   dump 自己发，用不着它），而解析出来的那份是从 `faultcode` 的 QName 文本
    //   里读到的 —— 两条路径的"码"一样，"码的命名空间"只有解析这一侧有值。
    check(made.code_space.empty() &&
              back.fault.code_space == uvcpp_soap_envelope_ns(made.version),
          "7.7 码的命名空间：造的那条是空的，解析的那条是信封命名空间");
    check_eq_s(back.fault.reason, made.reason, "7.8 说明相等");
    check_eq_s(back.fault.detail, made.detail, "7.9 detail 逐字节相等");
    check_eq_s(uvcpp_soap_dump_fault(back.fault), bytes, "7.10 再写一次仍然相同");
  }

  // (c) 响应的往返：把发出去的响应**当请求**读回来，Body 里那个元素的
  //     局部名与命名空间必须就是我们包装时给的那两个。
  const std::string resp = uvcpp_soap_dump_response(
      uvcpp_soap_version::V1_2, "AddResponse", "urn:calc", "<result>3</result>");
  uvcpp_soap_message rm;
  if (parse_ok(resp, rm, "7.11 解析自己写出来的响应")) {
    check_eq_s(rm.body_local, "AddResponse", "7.12 包装元素的局部名");
    check_eq_s(rm.body_ns, "urn:calc", "7.13 包装元素的命名空间");
    check_eq_s(rm.body_xml,
               "<AddResponse xmlns=\"urn:calc\"><result>3</result></AddResponse>",
               "7.14 body_xml（整段；写出去时那些换行与缩进不在里面，见 2.11）");
  }

  // (d) 前缀为空时写出来的信封，也读得回来（默认命名空间那条路的往返）。
  const std::string plain = uvcpp_soap_dump_response(
      uvcpp_soap_version::V1_1, "Ping", "urn:calc", "<v>1</v>",
      uvcpp_soap_dump_options(""));
  check(plain.find("<Envelope xmlns=\"http://schemas.xmlsoap.org/soap/envelope/\">") == 0,
        "7.15 前缀为空时根上写的是默认命名空间");
  uvcpp_soap_message pm;
  if (parse_ok(plain, pm, "7.16 解析前缀为空的响应")) {
    check_eq_s(pm.body_local, "Ping", "7.17 局部名（默认命名空间下的解析）");
    check_eq_s(pm.body_ns, "urn:calc", "7.18 命名空间");
  }
}

// =========================================================================
// 8. 上限参数真的透传（防空转）
// =========================================================================

void test_limits_are_honoured() {
  // 这两条与 5.12-5.14 是一对：那边证明"该拒的拒了"，这边证明**拒的理由对**
  // —— 把上限放宽之后同一份输入必须过。
  uvcpp_soap_message m;
  const char* why = "";
  check(uvcpp_soap_parse(std::string(k_call11), m,
                         uvcpp_wsdl_limits(4u * 1024u * 1024u, 64u, 65536u, 4096u),
                         &why) == wsdl_status::OK,
        "8.1 默认上限下这份输入是过的（对照 5.12/5.14）");

  // 深度上限：把上限提到 200 之后那条 80 层的输入必须过。
  std::string deep = "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                     "<soap:Body>";
  for (int i = 0; i < 80; ++i) deep += "<a>";
  for (int i = 0; i < 80; ++i) deep += "</a>";
  deep += "</soap:Body></soap:Envelope>";
  check(uvcpp_soap_parse(deep, m, uvcpp_wsdl_limits(4u * 1024u * 1024u, 200u)) ==
            wsdl_status::OK,
        "8.2 上限放宽后同一条输入就过了（证明 5.13 拒的是**深度**）");
}

}  // namespace

int main() {
  std::cout << "[soap_message] " << std::flush;
  test_version_and_admission();
  test_parse_call();
  test_must_understand_forms();
  test_no_header_and_default_ns();
  test_parse_12();
  test_fault11();
  test_fault12();
  test_fault_leniency();
  test_rejections();
  test_dump_envelope();
  test_dump_response();
  test_dump_fault();
  test_roundtrip();
  test_limits_are_honoured();

  if (g_failures == 0) {
    std::cout << "ALL PASS" << std::endl;
    return 0;
  }
  std::cout << "FAIL (" << g_failures << ")" << std::endl;
  return 2;
}

#else  // UVCPP_WSDL_ENABLE

// 关掉 wsdl 模块时这个文件不该被编译（`tests/functional/CMakeLists.txt` 的
// 过滤器按文件名摘掉它 —— 规则是 `wsdl|soap`）。这里返回非零：真编到了就是
// 一个必须修的配置错。
int main() {
  std::cerr << "[soap_message] UVCPP_WSDL_ENABLE=0 —— 这个测试文件不该被"
               "编译进来" << std::endl;
  return 2;
}

#endif  // UVCPP_WSDL_ENABLE
