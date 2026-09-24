/**
 * @file src/wsdl/uvcpp_wsdl_document.cpp
 * @brief WSDL 1.1 文档模型：解析与序列化（实现）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * ## 遍历的形状：名字判定必须在**子元素自己的作用域**里做
 *
 * 一个子元素可以**自己声明前缀**：`<w:message xmlns:w="…wsdl/" …>`。所以
 * "这个子元素是不是 `{wsdl}message`"这句话，只有在把子元素自己的 `xmlns*` 压进
 * 作用域之后问才对 —— 拿父元素的作用域去判，会把上面那种写法判成"不是"。
 * 全文件因此只有一条遍历路径（`for_each_child`：**先进后出地压/弹每个子元素**），
 * 没有"先看名字再决定要不要进"的写法。
 *
 * ## 序列化为什么**先写正文、后拼根标签**
 *
 * 根标签上要把用到的 `xmlns*` 全部声明出来，而"用到了哪些"是**写正文时才知道**
 * 的（`ns_table::ensure_named` 用到才分配）。所以先把正文攒进 `body_`，再拿攒好
 * 的声明表拼根标签 —— 一遍遍历，不需要"先扫一遍收集命名空间"那种两趟。
 */

#include <wsdl/uvcpp_wsdl_document.h>

#if UVCPP_WSDL_ENABLE

#include <wsdl/uvcpp_wsdl_pugixml.h>

#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace uvcpp {

// =========================================================================
// 状态名 / 小工具
// =========================================================================

const char* wsdl_status_name(wsdl_status s) {
  switch (s) {
    case wsdl_status::OK:                 return "ok";
    case wsdl_status::EMPTY:              return "empty";
    case wsdl_status::SYNTAX:             return "syntax";
    case wsdl_status::TOO_LARGE:          return "too_large";
    case wsdl_status::TOO_DEEP:           return "too_deep";
    case wsdl_status::TOO_MANY_NODES:     return "too_many_nodes";
    case wsdl_status::TOO_MANY_ITEMS:     return "too_many_items";
    case wsdl_status::NOT_WSDL:           return "not_wsdl";
    case wsdl_status::UNSUPPORTED_VERSION:return "unsupported_version";
    case wsdl_status::NO_TARGET_NAMESPACE:return "no_target_namespace";
    case wsdl_status::SOAP_NOT_ENVELOPE:  return "soap_not_envelope";
    case wsdl_status::SOAP_NO_BODY:       return "soap_no_body";
    case wsdl_status::SOAP_TOO_MANY_BODY_ELEMENTS:
                                          return "soap_too_many_body_elements";
    case wsdl_status::SOAP_BAD_FAULT:     return "soap_bad_fault";
  }
  return "?";
}

namespace {

/**
 * @brief 把引用文本拆成 (uri, local)。
 *
 * `{uri}local`、`prefix:local`、`local` 三种都认。注意 `prefix:local` 拆出来的
 * **是前缀、不是 URI** —— 前缀得靠文档自己的作用域才翻得成 URI，那一步在
 * `ref_matches()` 里做（本函数手上没有作用域）。
 */
void split_ref(const std::string& ref, std::string* uri, std::string* local) {
  uri->clear();
  if (!ref.empty() && ref[0] == '{') {
    const std::string::size_type e = ref.find('}');
    if (e != std::string::npos) {
      *uri = ref.substr(1, e - 1);
      *local = ref.substr(e + 1);
      return;
    }
  }
  const std::string::size_type p = ref.find(':');
  if (p != std::string::npos) {
    *uri = ref.substr(0, p);
    *local = ref.substr(p + 1);
    return;
  }
  *local = ref;
}

/**
 * @brief 引用能不能指到本文档里的一条。
 *
 * 规则：局部名必须相等，且写出来的那一部分必须**就是本文档的目标命名空间**：
 *
 *   * 没写（`"Add"`）→ 认；
 *   * `"{uri}Add"` → uri 与 targetNamespace 逐字节相等才认；
 *   * `"tns:Add"` → `tns` 是**前缀**，先拿根上的 `xmlns*` 声明表翻成 URI 再比。
 *
 * 最后那条是必需的：引用是从文档里读出来的，写的自然是文档自己的前缀
 * （`binding/@type="tns:CalculatorPortType"`），而不是把那串 URI 抄一遍。
 * 一个**没声明过**或指向别处的前缀因此翻不出目标命名空间，就正确地指不到。
 */
bool ref_matches(const std::string& ref, const std::string& name,
                 const std::string& target_ns,
                 const std::vector<std::pair<std::string, std::string> >& decls) {
  std::string uri, local;
  split_ref(ref, &uri, &local);
  if (local != name) return false;
  if (uri.empty()) return true;
  if (uri == target_ns) return true;
  for (std::string::size_type i = 0; i < decls.size(); ++i) {
    if (decls[i].first == uri) return decls[i].second == target_ns;
  }
  return false;
}

std::string escape_xml(const std::string& s, bool attr) {
  std::string out;
  out.reserve(s.size() + 8);
  for (std::string::size_type i = 0; i < s.size(); ++i) {
    const char c = s[i];
    switch (c) {
      case '&':  out += "&amp;"; break;
      case '<':  out += "&lt;";  break;
      case '>':  out += "&gt;";  break;
      case '"':  out += (attr ? "&quot;" : "\""); break;
      default:   out += c;       break;
    }
  }
  return out;
}

}  // namespace

std::string uvcpp_qname_local(const std::string& text) {
  std::string uri, local;
  split_ref(text, &uri, &local);
  return local;
}

std::string uvcpp_xml_escape_text(const std::string& s) {
  return escape_xml(s, false);
}

std::string uvcpp_xml_escape_attr(const std::string& s) {
  return escape_xml(s, true);
}

// =========================================================================
// 查
// =========================================================================

const uvcpp_wsdl_part* uvcpp_wsdl_message::find_part(
    const std::string& name) const {
  for (std::string::size_type i = 0; i < parts.size(); ++i) {
    if (parts[i].name == name) return &parts[i];
  }
  return nullptr;
}

const uvcpp_wsdl_operation* uvcpp_wsdl_port_type::find_operation(
    const std::string& name) const {
  for (std::string::size_type i = 0; i < operations.size(); ++i) {
    if (operations[i].name == name) return &operations[i];
  }
  return nullptr;
}

const uvcpp_wsdl_binding_operation* uvcpp_wsdl_binding::find_operation(
    const std::string& name) const {
  for (std::string::size_type i = 0; i < operations.size(); ++i) {
    if (operations[i].name == name) return &operations[i];
  }
  return nullptr;
}

const uvcpp_wsdl_port* uvcpp_wsdl_service::find_port(
    const std::string& name) const {
  for (std::string::size_type i = 0; i < ports.size(); ++i) {
    if (ports[i].name == name) return &ports[i];
  }
  return nullptr;
}

const uvcpp_wsdl_message* uvcpp_wsdl_document::find_message(
    const std::string& ref) const {
  for (std::string::size_type i = 0; i < messages.size(); ++i) {
    if (ref_matches(ref, messages[i].name, target_namespace,
                    root_namespaces)) {
      return &messages[i];
    }
  }
  return nullptr;
}

const uvcpp_wsdl_port_type* uvcpp_wsdl_document::find_port_type(
    const std::string& ref) const {
  for (std::string::size_type i = 0; i < port_types.size(); ++i) {
    if (ref_matches(ref, port_types[i].name, target_namespace,
                    root_namespaces)) {
      return &port_types[i];
    }
  }
  return nullptr;
}

const uvcpp_wsdl_binding* uvcpp_wsdl_document::find_binding(
    const std::string& ref) const {
  for (std::string::size_type i = 0; i < bindings.size(); ++i) {
    if (ref_matches(ref, bindings[i].name, target_namespace,
                    root_namespaces)) {
      return &bindings[i];
    }
  }
  return nullptr;
}

const uvcpp_wsdl_service* uvcpp_wsdl_document::find_service(
    const std::string& ref) const {
  for (std::string::size_type i = 0; i < services.size(); ++i) {
    if (ref_matches(ref, services[i].name, target_namespace,
                    root_namespaces)) {
      return &services[i];
    }
  }
  return nullptr;
}

// =========================================================================
// 解析
// =========================================================================

namespace {

namespace wd = wsdl_detail;

typedef wd::xml_ns_stack ns_stack;

wsdl_status fail(wsdl_status st, const char** why, const char* msg) {
  if (why != nullptr) *why = msg;
  return st;
}

bool has_room(size_t n, const uvcpp_wsdl_limits& lim) {
  return n < lim.max_items;
}

/** 属性文本 -> 已解析的 QName（`{uri}local` 的形状）。 */
uvcpp_qname qname_of(const ns_stack& ns, const char* text) {
  if (text == nullptr || *text == '\0') return uvcpp_qname();
  const std::string s = ns.resolve_qname(std::string(text));
  const std::string::size_type e = s.find('}');
  if (e == std::string::npos) return uvcpp_qname(std::string(), s);
  return uvcpp_qname(s.substr(1, e - 1), s.substr(e + 1));
}

std::vector<std::string> split_ws(const char* text) {
  std::vector<std::string> out;
  if (text == nullptr) return out;
  std::string cur;
  for (const char* p = text; *p != '\0'; ++p) {
    const char c = *p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

/**
 * @brief 遍历子元素，**每个子元素先把自己的 `xmlns*` 压进作用域**再交给访问器。
 *
 * 这是全文件唯一的一条遍历路径（见文件头）。`fn` 一旦把状态置成非 OK 就不再干活，
 * 但循环还会走完 —— 数量由父元素的子节点数决定，不是由文档大小决定。
 */
template <typename Fn>
void for_each_child(ns_stack& ns, const pugi::xml_node& parent, Fn fn) {
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    ns.push(c);
    fn(c);
    ns.pop();
  }
}

bool is_wsdl(const ns_stack& ns, const pugi::xml_node& n, const char* local) {
  return ns.is(n, wsdl_ns::wsdl(), local);
}

// --- 各个元素 ---------------------------------------------------------

wsdl_status parse_part(ns_stack& ns, const pugi::xml_node& n,
                       uvcpp_wsdl_part* out) {
  out->name = wd::xml_attr(n, "name", "");
  out->element = qname_of(ns, wd::xml_attr_raw(n, "element"));
  out->type = qname_of(ns, wd::xml_attr_raw(n, "type"));
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (is_wsdl(ns, c, "documentation")) out->documentation = wd::xml_text(c);
  });
  return wsdl_status::OK;
}

wsdl_status parse_message(ns_stack& ns, const pugi::xml_node& n,
                          const uvcpp_wsdl_limits& lim,
                          uvcpp_wsdl_message* out, const char** why) {
  out->name = wd::xml_attr(n, "name", "");
  wsdl_status st = wsdl_status::OK;
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (st != wsdl_status::OK) return;
    if (is_wsdl(ns, c, "documentation")) {
      out->documentation = wd::xml_text(c);
      return;
    }
    if (is_wsdl(ns, c, "part")) {
      if (!has_room(out->parts.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "message 的 part 太多");
        return;
      }
      out->parts.push_back(uvcpp_wsdl_part());
      st = parse_part(ns, c, &out->parts.back());
    }
  });
  return st;
}

wsdl_status parse_op_ref(ns_stack& ns, const pugi::xml_node& n,
                         uvcpp_wsdl_op_ref* out) {
  out->name = wd::xml_attr(n, "name", "");
  out->message = qname_of(ns, wd::xml_attr_raw(n, "message"));
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (is_wsdl(ns, c, "documentation")) out->documentation = wd::xml_text(c);
  });
  return wsdl_status::OK;
}

wsdl_status parse_port_type_op(ns_stack& ns, const pugi::xml_node& n,
                               const uvcpp_wsdl_limits& lim,
                               uvcpp_wsdl_operation* out, const char** why) {
  out->name = wd::xml_attr(n, "name", "");
  out->parameter_order = split_ws(wd::xml_attr_raw(n, "parameterOrder"));
  wsdl_status st = wsdl_status::OK;
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (st != wsdl_status::OK) return;
    if (is_wsdl(ns, c, "documentation")) {
      out->documentation = wd::xml_text(c);
      return;
    }
    if (is_wsdl(ns, c, "input")) {
      out->has_input = true;
      st = parse_op_ref(ns, c, &out->input);
      return;
    }
    if (is_wsdl(ns, c, "output")) {
      out->has_output = true;
      st = parse_op_ref(ns, c, &out->output);
      return;
    }
    if (is_wsdl(ns, c, "fault")) {
      if (!has_room(out->faults.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "operation 的 fault 太多");
        return;
      }
      out->faults.push_back(uvcpp_wsdl_op_ref());
      st = parse_op_ref(ns, c, &out->faults.back());
    }
  });
  return st;
}

wsdl_status parse_port_type(ns_stack& ns, const pugi::xml_node& n,
                            const uvcpp_wsdl_limits& lim,
                            uvcpp_wsdl_port_type* out, const char** why) {
  out->name = wd::xml_attr(n, "name", "");
  wsdl_status st = wsdl_status::OK;
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (st != wsdl_status::OK) return;
    if (is_wsdl(ns, c, "documentation")) {
      out->documentation = wd::xml_text(c);
      return;
    }
    if (is_wsdl(ns, c, "operation")) {
      if (!has_room(out->operations.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "portType 的 operation 太多");
        return;
      }
      out->operations.push_back(uvcpp_wsdl_operation());
      st = parse_port_type_op(ns, c, lim, &out->operations.back(), why);
    }
  });
  return st;
}

/** `soap:body` / `soap12:body`：use + encodingStyle + namespace + parts。 */
void parse_soap_body(ns_stack& ns, const pugi::xml_node& n, std::string* use,
                     std::string* enc, std::string* space,
                     std::vector<std::string>* parts) {
  (void)ns;
  *use = wd::xml_attr(n, "use", "");
  *enc = wd::xml_attr(n, "encodingStyle", "");
  *space = wd::xml_attr(n, "namespace", "");
  *parts = split_ws(wd::xml_attr_raw(n, "parts"));
}

/** `input` / `output` 下面那一个 `soap:body`。 */
void parse_soap_body_of(ns_stack& ns, const pugi::xml_node& n,
                        std::string* use, std::string* enc, std::string* space,
                        std::vector<std::string>* parts) {
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (!use->empty() || !enc->empty() || !space->empty() || !parts->empty()) {
      return;  // 已经收过了
    }
    if (ns.is(c, wsdl_ns::soap_binding(), "body") ||
        ns.is(c, wsdl_ns::soap12_binding(), "body")) {
      parse_soap_body(ns, c, use, enc, space, parts);
    }
  });
}

wsdl_status parse_binding_op(ns_stack& ns, const pugi::xml_node& n,
                             uvcpp_wsdl_binding_operation* out) {
  out->name = wd::xml_attr(n, "name", "");
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (is_wsdl(ns, c, "documentation")) {
      out->documentation = wd::xml_text(c);
      return;
    }
    if (ns.is(c, wsdl_ns::soap_binding(), "operation")) {
      out->is_soap12 = false;
      out->soap_action = wd::xml_attr(c, "soapAction", "");
      out->style = wd::xml_attr(c, "style", "");
      return;
    }
    if (ns.is(c, wsdl_ns::soap12_binding(), "operation")) {
      out->is_soap12 = true;
      out->soap_action = wd::xml_attr(c, "soapAction", "");
      out->style = wd::xml_attr(c, "style", "");
      return;
    }
    if (is_wsdl(ns, c, "input")) {
      parse_soap_body_of(ns, c, &out->input_use, &out->input_encoding_style,
                         &out->input_namespace, &out->input_body_parts);
      return;
    }
    if (is_wsdl(ns, c, "output")) {
      parse_soap_body_of(ns, c, &out->output_use, &out->output_encoding_style,
                         &out->output_namespace, &out->output_body_parts);
    }
  });
  return wsdl_status::OK;
}

wsdl_status parse_binding(ns_stack& ns, const pugi::xml_node& n,
                          const uvcpp_wsdl_limits& lim, uvcpp_wsdl_binding* out,
                          const char** why) {
  out->name = wd::xml_attr(n, "name", "");
  out->port_type = qname_of(ns, wd::xml_attr_raw(n, "type"));
  wsdl_status st = wsdl_status::OK;
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (st != wsdl_status::OK) return;
    if (is_wsdl(ns, c, "documentation")) {
      out->documentation = wd::xml_text(c);
      return;
    }
    if (ns.is(c, wsdl_ns::soap_binding(), "binding") ||
        ns.is(c, wsdl_ns::soap12_binding(), "binding")) {
      if (out->soap.present) return;  // 取第一个
      out->soap.present = true;
      out->soap.is_soap12 = ns.is(c, wsdl_ns::soap12_binding(), "binding");
      out->soap.style = wd::xml_attr(c, "style", "");
      out->soap.transport = wd::xml_attr(c, "transport", "");
      return;
    }
    if (is_wsdl(ns, c, "operation")) {
      if (!has_room(out->operations.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "binding 的 operation 太多");
        return;
      }
      out->operations.push_back(uvcpp_wsdl_binding_operation());
      st = parse_binding_op(ns, c, &out->operations.back());
    }
  });
  return st;
}

wsdl_status parse_port(ns_stack& ns, const pugi::xml_node& n,
                       uvcpp_wsdl_port* out) {
  out->name = wd::xml_attr(n, "name", "");
  out->binding = qname_of(ns, wd::xml_attr_raw(n, "binding"));
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (is_wsdl(ns, c, "documentation")) {
      out->documentation = wd::xml_text(c);
      return;
    }
    if (ns.is(c, wsdl_ns::soap_binding(), "address")) {
      out->address = wd::xml_attr(c, "location", "");
      out->is_soap12 = false;
      return;
    }
    if (ns.is(c, wsdl_ns::soap12_binding(), "address")) {
      out->address = wd::xml_attr(c, "location", "");
      out->is_soap12 = true;
    }
  });
  return wsdl_status::OK;
}

wsdl_status parse_service(ns_stack& ns, const pugi::xml_node& n,
                          const uvcpp_wsdl_limits& lim,
                          uvcpp_wsdl_service* out, const char** why) {
  out->name = wd::xml_attr(n, "name", "");
  wsdl_status st = wsdl_status::OK;
  for_each_child(ns, n, [&](const pugi::xml_node& c) {
    if (st != wsdl_status::OK) return;
    if (is_wsdl(ns, c, "documentation")) {
      out->documentation = wd::xml_text(c);
      return;
    }
    if (is_wsdl(ns, c, "port")) {
      if (!has_room(out->ports.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "service 的 port 太多");
        return;
      }
      out->ports.push_back(uvcpp_wsdl_port());
      st = parse_port(ns, c, &out->ports.back());
    }
  });
  return st;
}

}  // namespace

wsdl_status uvcpp_wsdl_parse(const char* data, size_t len,
                             uvcpp_wsdl_document& out,
                             const uvcpp_wsdl_limits& lim, const char** why) {
  out.clear();
  if (why != nullptr) *why = "";

  const wd::xml_limits xlim(lim.max_bytes, lim.max_depth, lim.max_nodes);
  pugi::xml_document pd;
  const wd::xml_result xr = wd::xml_parse(data, len, pd, xlim, why);
  if (xr != wd::xml_result::OK) return wd::from_xml_result(xr, why);

  out.source_bytes = len;

  pugi::xml_node root = pd.document_element();
  if (!root) {
    return fail(wsdl_status::NOT_WSDL, why, "文档里没有根元素");
  }

  ns_stack ns;
  ns.push(root);
  if (!is_wsdl(ns, root, "definitions")) {
    if (ns.is(root, wsdl_ns::wsdl20(), "description")) {
      return fail(wsdl_status::UNSUPPORTED_VERSION, why,
                  "这是 WSDL 2.0 的 description —— 本模块只支持 1.1");
    }
    return fail(wsdl_status::NOT_WSDL, why,
                "根元素不是 {http://schemas.xmlsoap.org/wsdl/}definitions");
  }

  for (pugi::xml_attribute a = root.first_attribute(); a;
       a = a.next_attribute()) {
    const std::string n = a.name();
    if (n == "xmlns") {
      out.root_namespaces.push_back(
          std::make_pair(std::string(), std::string(a.value())));
    } else if (n.size() > 6 && n.compare(0, 6, "xmlns:") == 0) {
      out.root_namespaces.push_back(
          std::make_pair(n.substr(6), std::string(a.value())));
    }
  }

  out.target_namespace = wd::xml_attr(root, "targetNamespace", "");
  if (out.target_namespace.empty()) {
    return fail(wsdl_status::NO_TARGET_NAMESPACE, why,
                "definitions 上没有 targetNamespace（WSDL 1.1 §3.1 要求它）");
  }
  out.name = wd::xml_attr(root, "name", "");

  wsdl_status st = wsdl_status::OK;
  for_each_child(ns, root, [&](const pugi::xml_node& c) {
    if (st != wsdl_status::OK) return;
    if (is_wsdl(ns, c, "documentation")) {
      if (out.documentation.empty()) {
        out.documentation = wd::xml_strip_cr(wd::xml_text(c));
      }
      return;
    }
    if (is_wsdl(ns, c, "import")) {
      if (!has_room(out.imports.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "import 太多");
        return;
      }
      uvcpp_wsdl_import im;
      im.namespace_uri = wd::xml_attr(c, "namespace", "");
      im.location = wd::xml_attr(c, "location", "");
      out.imports.push_back(im);
      return;
    }
    if (is_wsdl(ns, c, "types")) {
      // 第二个 <types> 忽略（规范只允许一个）。
      if (out.types_xml.empty()) out.types_xml = wd::xml_raw_element(c);
      return;
    }
    if (is_wsdl(ns, c, "message")) {
      if (!has_room(out.messages.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "message 太多");
        return;
      }
      out.messages.push_back(uvcpp_wsdl_message());
      st = parse_message(ns, c, lim, &out.messages.back(), why);
      return;
    }
    if (is_wsdl(ns, c, "portType")) {
      if (!has_room(out.port_types.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "portType 太多");
        return;
      }
      out.port_types.push_back(uvcpp_wsdl_port_type());
      st = parse_port_type(ns, c, lim, &out.port_types.back(), why);
      return;
    }
    if (is_wsdl(ns, c, "binding")) {
      if (!has_room(out.bindings.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "binding 太多");
        return;
      }
      out.bindings.push_back(uvcpp_wsdl_binding());
      st = parse_binding(ns, c, lim, &out.bindings.back(), why);
      return;
    }
    if (is_wsdl(ns, c, "service")) {
      if (!has_room(out.services.size(), lim)) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "service 太多");
        return;
      }
      out.services.push_back(uvcpp_wsdl_service());
      st = parse_service(ns, c, lim, &out.services.back(), why);
      return;
    }
    // 其余元素一律跳过：WSDL 明确允许扩展元素，为它们报错会把合法文档拒掉。
  });
  ns.pop();

  if (st != wsdl_status::OK) out.clear();
  return st;
}

wsdl_status uvcpp_wsdl_parse(const std::string& xml, uvcpp_wsdl_document& out,
                             const uvcpp_wsdl_limits& lim, const char** why) {
  return uvcpp_wsdl_parse(xml.data(), xml.size(), out, lim, why);
}

// =========================================================================
// 序列化
// =========================================================================

namespace {

/**
 * @brief 输出的命名空间表。
 *
 * **默认命名空间与具名前缀分开记**：WSDL 元素一律走默认命名空间，而 QName 的
 * *值* 必须是 `前缀:局部名` 的形状（XML Schema 的 QName 规则里，不带前缀的属性
 * 值是"用默认命名空间"，在 WSDL 里没人这么写，照做会产出没人认识的东西）。
 * 所以 `prefix_of(kWsdl)` **必须**回来一个具名前缀，不能是空串。
 */
struct ns_table {
  std::string default_uri;
  std::vector<std::pair<std::string, std::string> > decls;  // (前缀, URI)
  std::map<std::string, std::string> by_uri;
  std::map<std::string, std::string> by_prefix;
  int invented;

  ns_table() : invented(0) {}

  void set_default(const std::string& uri) {
    if (!default_uri.empty() || uri.empty()) return;
    default_uri = uri;
    decls.push_back(std::make_pair(std::string(), uri));
  }

  bool add_named(const std::string& prefix, const std::string& uri) {
    if (uri.empty() || prefix.empty()) return false;
    if (by_uri.count(uri)) return false;
    if (by_prefix.count(prefix)) return false;
    decls.push_back(std::make_pair(prefix, uri));
    by_uri[uri] = prefix;
    by_prefix[prefix] = uri;
    return true;
  }

  /** 顶掉一个前缀的既有绑定（只用来占 `tns` —— 见 `collect`）。 */
  void override_named(const std::string& prefix, const std::string& uri) {
    if (prefix.empty() || uri.empty()) return;
    std::map<std::string, std::string>::iterator it = by_prefix.find(prefix);
    if (it != by_prefix.end()) {
      if (it->second == uri) return;
      by_uri.erase(it->second);
      it->second = uri;
    } else {
      by_prefix[prefix] = uri;
      decls.push_back(std::make_pair(prefix, uri));
    }
    by_uri[uri] = prefix;
  }

  std::string prefix_of(const std::string& uri) const {
    if (uri.empty()) return std::string();
    const std::map<std::string, std::string>::const_iterator it = by_uri.find(uri);
    return it == by_uri.end() ? std::string() : it->second;
  }

  /** 要一个**具名**前缀；没有就按 `preferred`（被占了就 `ns1`、`ns2`…）造一个。 */
  std::string ensure_named(const std::string& uri,
                           const std::string& preferred) {
    if (uri.empty()) return std::string();
    std::string p = prefix_of(uri);
    if (!p.empty()) return p;
    if (!preferred.empty() && !by_prefix.count(preferred)) {
      add_named(preferred, uri);
      return preferred;
    }
    for (;;) {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "ns%d", ++invented);
      const std::string cand(buf);
      if (!by_prefix.count(cand)) {
        add_named(cand, uri);
        return cand;
      }
    }
  }
};

/** 有名字的 URI 用哪个前缀 —— 只是**倾向**，被占了就自动换。 */
const char* preferred_prefix(const std::string& uri) {
  if (uri == wsdl_ns::wsdl()) return "wsdl";
  if (uri == wsdl_ns::soap_binding()) return "soap";
  if (uri == wsdl_ns::soap12_binding()) return "soap12";
  if (uri == wsdl_ns::xsd()) return "xsd";
  return "";
}

struct dumper {
  const uvcpp_wsdl_document& doc;
  ns_table tbl;
  std::string body;

  explicit dumper(const uvcpp_wsdl_document& d) : doc(d) {}

  std::string pad(int lvl) const { return std::string(static_cast<size_t>(lvl) * 2, ' '); }

  void line(int lvl, const std::string& s) {
    body += pad(lvl);
    body += s;
    body += '\n';
  }

  /** QName 值：`前缀:局部名`（没有命名空间时就是局部名）。 */
  std::string qname_text(const uvcpp_qname& q) {
    if (q.local.empty()) return std::string();
    if (q.space.empty()) return q.local;
    const std::string p = tbl.ensure_named(q.space, preferred_prefix(q.space));
    return p + ":" + q.local;
  }

  /** 没有命名空间的常见情形：属性值本来就不是 QName（名字、URI、style…）。 */
  static std::string attr(const std::string& name, const std::string& value) {
    if (value.empty()) return std::string();
    return " " + name + "=\"" + uvcpp_xml_escape_attr(value) + "\"";
  }

  std::string soap_prefix(bool is12) {
    const std::string uri = is12 ? wsdl_ns::soap12_binding()
                                 : wsdl_ns::soap_binding();
    return tbl.ensure_named(uri, is12 ? "soap12" : "soap");
  }

  void documentation(int lvl, const std::string& text) {
    if (text.empty()) return;
    line(lvl, "<documentation>" + uvcpp_xml_escape_text(text) + "</documentation>");
  }

  /** 属性集合里一个不为空的都不写时，属性串就是空 —— 用个小帮手拼。 */
  static void join(std::string* out, const std::string& piece) {
    if (!piece.empty()) *out += piece;
  }

  void op_ref(int lvl, const char* tag, const uvcpp_wsdl_op_ref& r) {
    std::string a;
    join(&a, attr("name", r.name));
    join(&a, attr("message", qname_text(r.message)));
    if (r.documentation.empty()) {
      line(lvl, std::string("<") + tag + a + "/>");
    } else {
      line(lvl, std::string("<") + tag + a + ">");
      documentation(lvl + 1, r.documentation);
      line(lvl, std::string("</") + tag + ">");
    }
  }

  void soap_body(int lvl, const char* tag, const std::string& use,
                 const std::string& enc, const std::string& space,
                 const std::vector<std::string>& parts, bool is12) {
    if (use.empty() && enc.empty() && space.empty() && parts.empty()) return;
    const std::string p = soap_prefix(is12);
    std::string a;
    join(&a, attr("use", use));
    join(&a, attr("encodingStyle", enc));
    join(&a, attr("namespace", space));
    if (!parts.empty()) {
      std::string joined;
      for (std::string::size_type i = 0; i < parts.size(); ++i) {
        if (i != 0) joined += " ";
        joined += parts[i];
      }
      join(&a, attr("parts", joined));
    }
    line(lvl, std::string("<") + tag + ">");
    line(lvl + 1, "<" + p + ":body" + a + "/>");
    line(lvl, std::string("</") + tag + ">");
  }

  void run() {
    // 1) 默认命名空间 = WSDL；tns 保留给 targetNamespace；原文档根上的声明照搬。
    tbl.set_default(wsdl_ns::wsdl());
    if (!doc.target_namespace.empty()) {
      tbl.override_named("tns", doc.target_namespace);
    }
    for (std::string::size_type i = 0; i < doc.root_namespaces.size(); ++i) {
      const std::string& p = doc.root_namespaces[i].first;
      const std::string& u = doc.root_namespaces[i].second;
      if (p.empty()) continue;   // 默认命名空间由我们自己定（必须是 WSDL）
      if (p == "tns") continue;  // tns 由我们自己占
      // ★ **连 WSDL 自己的那个前缀也照搬**（原文档写 `wsdl:types` 时就是它）。
      //   少了这一条，`types_xml` 里原样搬过来的 `<wsdl:types>` 就成了"前缀没人
      //   定义"——那份文档**重新解析时那个 <types> 会被整段跳过**（前缀解析不出来
      //   ⇒ URI 是空串 ⇒ 不是 {wsdl}types）。同一 URI 上多一条声明是合法的，
      //   而"丢掉声明"是不可逆的。
      tbl.add_named(p, u);
    }

    // 2) 正文（写正文的过程会把用到的命名空间登记进表里）。
    emit_root_children();
  }

  void emit_root_children();
};

}  // namespace

namespace {

// `emit_root_children` 拆在下面：它用到 `dumper` 的全部成员，写在结构体里太长。
void dumper::emit_root_children() {
  documentation(1, doc.documentation);

  for (std::string::size_type i = 0; i < doc.imports.size(); ++i) {
    std::string a;
    join(&a, attr("namespace", doc.imports[i].namespace_uri));
    join(&a, attr("location", doc.imports[i].location));
    line(1, "<import" + a + "/>");
  }

  // types 整段原样（可能自带 xmlns 声明，所以不重新缩进 —— 见头文件）。
  if (!doc.types_xml.empty()) {
    body += pad(1);
    body += doc.types_xml;
    body += '\n';
  }

  for (std::string::size_type i = 0; i < doc.messages.size(); ++i) {
    const uvcpp_wsdl_message& m = doc.messages[i];
    line(1, "<message" + attr("name", m.name) + ">");
    documentation(2, m.documentation);
    for (std::string::size_type j = 0; j < m.parts.size(); ++j) {
      const uvcpp_wsdl_part& p = m.parts[j];
      std::string a = attr("name", p.name);
      if (!p.element.empty()) {
        join(&a, attr("element", qname_text(p.element)));
      } else if (!p.type.empty()) {
        join(&a, attr("type", qname_text(p.type)));
      }
      line(2, "<part" + a + "/>");
    }
    line(1, "</message>");
  }

  for (std::string::size_type i = 0; i < doc.port_types.size(); ++i) {
    const uvcpp_wsdl_port_type& pt = doc.port_types[i];
    line(1, "<portType" + attr("name", pt.name) + ">");
    documentation(2, pt.documentation);
    for (std::string::size_type j = 0; j < pt.operations.size(); ++j) {
      const uvcpp_wsdl_operation& op = pt.operations[j];
      std::string a = attr("name", op.name);
      if (!op.parameter_order.empty()) {
        std::string joined;
        for (std::string::size_type k = 0; k < op.parameter_order.size(); ++k) {
          if (k != 0) joined += " ";
          joined += op.parameter_order[k];
        }
        join(&a, attr("parameterOrder", joined));
      }
      line(2, "<operation" + a + ">");
      documentation(3, op.documentation);
      if (op.has_input) op_ref(3, "input", op.input);
      if (op.has_output) op_ref(3, "output", op.output);
      for (std::string::size_type k = 0; k < op.faults.size(); ++k) {
        op_ref(3, "fault", op.faults[k]);
      }
      line(2, "</operation>");
    }
    line(1, "</portType>");
  }

  for (std::string::size_type i = 0; i < doc.bindings.size(); ++i) {
    const uvcpp_wsdl_binding& b = doc.bindings[i];
    std::string a = attr("name", b.name);
    join(&a, attr("type", qname_text(b.port_type)));
    line(1, "<binding" + a + ">");
    documentation(2, b.documentation);
    if (b.soap.present) {
      const std::string p = soap_prefix(b.soap.is_soap12);
      std::string sa;
      join(&sa, attr("style", b.soap.style));
      join(&sa, attr("transport", b.soap.transport));
      line(2, "<" + p + ":binding" + sa + "/>");
    }
    for (std::string::size_type j = 0; j < b.operations.size(); ++j) {
      const uvcpp_wsdl_binding_operation& op = b.operations[j];
      line(2, "<operation" + attr("name", op.name) + ">");
      documentation(3, op.documentation);
      // `is_soap12` 也算"有话要说"：一个空的 `soap12:operation` 与一个空的
      // `soap:operation` 在模型里不是同一件事。
      const bool soap12 = op.is_soap12 || b.soap.is_soap12;
      if (!op.soap_action.empty() || !op.style.empty() || op.is_soap12) {
        const std::string p = soap_prefix(soap12);
        std::string sa;
        join(&sa, attr("soapAction", op.soap_action));
        join(&sa, attr("style", op.style));
        line(3, "<" + p + ":operation" + sa + "/>");
      }
      soap_body(3, "input", op.input_use, op.input_encoding_style,
                op.input_namespace, op.input_body_parts, soap12);
      soap_body(3, "output", op.output_use, op.output_encoding_style,
                op.output_namespace, op.output_body_parts, soap12);
      line(2, "</operation>");
    }
    line(1, "</binding>");
  }

  for (std::string::size_type i = 0; i < doc.services.size(); ++i) {
    const uvcpp_wsdl_service& s = doc.services[i];
    line(1, "<service" + attr("name", s.name) + ">");
    documentation(2, s.documentation);
    for (std::string::size_type j = 0; j < s.ports.size(); ++j) {
      const uvcpp_wsdl_port& p = s.ports[j];
      std::string a = attr("name", p.name);
      join(&a, attr("binding", qname_text(p.binding)));
      if (p.address.empty() && p.documentation.empty()) {
        line(2, "<port" + a + "/>");
        continue;
      }
      line(2, "<port" + a + ">");
      documentation(3, p.documentation);
      if (!p.address.empty()) {
        const std::string pre = soap_prefix(p.is_soap12);
        line(3, "<" + pre + ":address" + attr("location", p.address) + "/>");
      }
      line(2, "</port>");
    }
    line(1, "</service>");
  }
}

}  // namespace

void uvcpp_wsdl_dump_to(const uvcpp_wsdl_document& doc, std::string& out) {
  dumper d(doc);
  d.run();

  std::string head = "<definitions";
  for (std::string::size_type i = 0; i < d.tbl.decls.size(); ++i) {
    const std::string& p = d.tbl.decls[i].first;
    const std::string& u = d.tbl.decls[i].second;
    head += "\n  xmlns";
    if (!p.empty()) head += ":" + p;
    head += "=\"" + uvcpp_xml_escape_attr(u) + "\"";
  }
  head += "\n  targetNamespace=\"" +
          uvcpp_xml_escape_attr(doc.target_namespace) + "\"";
  if (!doc.name.empty()) {
    head += "\n  name=\"" + uvcpp_xml_escape_attr(doc.name) + "\"";
  }
  head += ">";

  out.clear();
  out.reserve(head.size() + d.body.size() + 24);
  out += head;
  out += "\n";
  out += d.body;
  out += "</definitions>\n";
}

std::string uvcpp_wsdl_dump(const uvcpp_wsdl_document& doc) {
  std::string out;
  uvcpp_wsdl_dump_to(doc, out);
  return out;
}

// =========================================================================
// 引用闭合
// =========================================================================

bool uvcpp_wsdl_check_references(const uvcpp_wsdl_document& doc,
                                 std::string* why) {
  for (std::string::size_type i = 0; i < doc.bindings.size(); ++i) {
    const uvcpp_wsdl_binding& b = doc.bindings[i];
    if (b.port_type.empty()) {
      if (why) *why = "binding \"" + b.name + "\" 没有 @type";
      return false;
    }
    const uvcpp_wsdl_port_type* pt = doc.find_port_type(b.port_type.str());
    if (pt == nullptr) {
      if (why) {
        *why = "binding \"" + b.name + "\" 的 @type=" + b.port_type.str() +
               " 指不到任何 portType";
      }
      return false;
    }
    for (std::string::size_type j = 0; j < b.operations.size(); ++j) {
      const uvcpp_wsdl_binding_operation& ob = b.operations[j];
      if (pt->find_operation(ob.name) == nullptr) {
        if (why) {
          *why = "binding \"" + b.name + "\" 的 operation \"" + ob.name +
                 "\" 在它引用的 portType \"" + pt->name + "\" 里不存在";
        }
        return false;
      }
    }
  }

  for (std::string::size_type i = 0; i < doc.port_types.size(); ++i) {
    const uvcpp_wsdl_port_type& pt = doc.port_types[i];
    for (std::string::size_type j = 0; j < pt.operations.size(); ++j) {
      const uvcpp_wsdl_operation& op = pt.operations[j];
      if (op.has_input && !op.input.message.empty() &&
          doc.find_message(op.input.message.str()) == nullptr) {
        if (why) {
          *why = "operation \"" + op.name + "\" 的 input/@message=" +
                 op.input.message.str() + " 指不到任何 message";
        }
        return false;
      }
      if (op.has_output && !op.output.message.empty() &&
          doc.find_message(op.output.message.str()) == nullptr) {
        if (why) {
          *why = "operation \"" + op.name + "\" 的 output/@message=" +
                 op.output.message.str() + " 指不到任何 message";
        }
        return false;
      }
      for (std::string::size_type k = 0; k < op.faults.size(); ++k) {
        if (!op.faults[k].message.empty() &&
            doc.find_message(op.faults[k].message.str()) == nullptr) {
          if (why) {
            *why = "operation \"" + op.name + "\" 的 fault/@message=" +
                   op.faults[k].message.str() + " 指不到任何 message";
          }
          return false;
        }
      }
    }
  }

  for (std::string::size_type i = 0; i < doc.services.size(); ++i) {
    const uvcpp_wsdl_service& s = doc.services[i];
    for (std::string::size_type j = 0; j < s.ports.size(); ++j) {
      const uvcpp_wsdl_port& p = s.ports[j];
      if (p.binding.empty()) {
        if (why) *why = "service \"" + s.name + "\" 的 port \"" + p.name +
                        "\" 没有 @binding";
        return false;
      }
      if (doc.find_binding(p.binding.str()) == nullptr) {
        if (why) {
          *why = "port \"" + p.name + "\" 的 @binding=" + p.binding.str() +
                 " 指不到任何 binding";
        }
        return false;
      }
    }
  }

  return true;
}

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
