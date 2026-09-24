/**
 * @file src/wsdl/uvcpp_wsdl_pugixml.cpp
 * @brief 私有 XML 层：解析上限与命名空间作用域栈（实现）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <wsdl/uvcpp_wsdl_pugixml.h>

#if UVCPP_WSDL_ENABLE

#include <utility>
#include <vector>

namespace uvcpp {
namespace wsdl_detail {

const char* xml_result_name(xml_result r) {
  switch (r) {
    case xml_result::OK:            return "ok";
    case xml_result::EMPTY:         return "empty";
    case xml_result::SYNTAX:        return "syntax";
    case xml_result::TOO_LARGE:     return "too_large";
    case xml_result::TOO_DEEP:      return "too_deep";
    case xml_result::TOO_MANY_NODES:return "too_many_nodes";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

xml_result xml_measure(const pugi::xml_node& root, const xml_limits& lim,
                       const char** why) {
  if (!root) {
    if (why != nullptr) *why = "文档里没有根元素";
    // 空文档不算超限：调用方自己会看出"没有根元素"这件事（NOT_WSDL）。
    return xml_result::OK;
  }

  // **显式栈**：递归遍历本身就是被防的那件事（一万层嵌套能让守卫自己爆栈）。
  // 每个元素只进栈一次，出栈时判定 —— 深度是"出栈时那个节点所在的层"，
  // 因为 DFS 见过的最大层号就是树的最大深度。
  std::vector<std::pair<pugi::xml_node, size_t> > stack;
  stack.push_back(std::make_pair(root, size_t(1)));
  size_t nodes = 0;

  while (!stack.empty()) {
    const std::pair<pugi::xml_node, size_t> cur = stack.back();
    stack.pop_back();

    ++nodes;  // 只数**元素**：属性与文本不占节点名额（一个元素带几个属性是常数级）
    if (cur.second > lim.max_depth) {
      if (why != nullptr) *why = "元素嵌套太深";
      return xml_result::TOO_DEEP;
    }
    if (nodes > lim.max_nodes) {
      if (why != nullptr) *why = "元素总数太多";
      return xml_result::TOO_MANY_NODES;
    }

    for (pugi::xml_node c = cur.first.first_child(); c; c = c.next_sibling()) {
      if (c.type() == pugi::node_element) {
        stack.push_back(std::make_pair(c, cur.second + 1));
      }
    }
  }
  return xml_result::OK;
}

xml_result xml_parse(const char* data, size_t len, pugi::xml_document& out,
                     const xml_limits& lim, const char** why) {
  if (why != nullptr) *why = "";
  if (data == nullptr || len == 0) {
    if (why != nullptr) *why = "空输入";
    return xml_result::EMPTY;
  }
  if (len > lim.max_bytes) {
    if (why != nullptr) *why = "超过输入字节上限";
    return xml_result::TOO_LARGE;
  }

  out.reset();
  // parse_declaration：认 `<?xml version="1.0" encoding="UTF-8"?>`（没有它，
  // 带声明的文档会解析失败 —— 而绝大多数 WSDL 的第一行就是它）。
  // encoding_auto：按声明/前几个字节判编码（BOM、UTF-8、UTF-16 都认）。
  const pugi::xml_parse_result pr = out.load_buffer(
      data, len, pugi::parse_default | pugi::parse_declaration, pugi::encoding_auto);
  if (!pr) {
    // pr.description() 指向 pugixml 自己的静态串，生命周期足够。
    if (why != nullptr) *why = pr.description();
    return xml_result::SYNTAX;
  }

  const xml_result mr = xml_measure(out.document_element(), lim, why);
  if (mr != xml_result::OK) {
    out.reset();
    return mr;
  }
  return xml_result::OK;
}

// ---------------------------------------------------------------------------
// 命名空间作用域栈
// ---------------------------------------------------------------------------

xml_ns_stack::xml_ns_stack() {
  // 第 0 层是"根元素之外"：一份没声明任何命名空间的文档在这里就该解析不出来。
  scopes_.resize(1);
}

void xml_ns_stack::push(const pugi::xml_node& node) {
  std::map<std::string, std::string> scope = scopes_.back();  // 继承上一层
  for (pugi::xml_attribute a = node.first_attribute(); a; a = a.next_attribute()) {
    const std::string n = a.name();
    if (n == "xmlns") {
      scope[""] = a.value();  // 默认命名空间（作用于不带前缀的元素）
    } else if (n.size() > 6 && n.compare(0, 6, "xmlns:") == 0) {
      scope[n.substr(6)] = a.value();
    }
  }
  scopes_.push_back(scope);
}

void xml_ns_stack::pop() {
  if (scopes_.size() > 1) scopes_.pop_back();
}

const std::string& xml_ns_stack::resolve(const std::string& prefix) const {
  static const std::string k_empty;
  const std::map<std::string, std::string>& top = scopes_.back();
  const std::map<std::string, std::string>::const_iterator it = top.find(prefix);
  return it == top.end() ? k_empty : it->second;
}

std::string xml_ns_stack::raw_name(const pugi::xml_node& node) {
  return std::string(node.name());
}

std::string xml_ns_stack::local_of(const pugi::xml_node& node) const {
  const std::string s = raw_name(node);
  const std::string::size_type p = s.find(':');
  return p == std::string::npos ? s : s.substr(p + 1);
}

std::string xml_ns_stack::uri_of(const pugi::xml_node& node) const {
  const std::string s = raw_name(node);
  const std::string::size_type p = s.find(':');
  if (p == std::string::npos) {
    return resolve("");  // 不带前缀 -> 默认命名空间
  }
  return resolve(s.substr(0, p));
}

bool xml_ns_stack::is(const pugi::xml_node& node, const char* uri,
                      const char* local) const {
  if (uri == nullptr || local == nullptr) return false;
  return uri_of(node) == uri && local_of(node) == local;
}

std::string xml_ns_stack::resolve_qname(const std::string& qname) const {
  const std::string::size_type p = qname.find(':');
  const std::string prefix = (p == std::string::npos) ? std::string() : qname.substr(0, p);
  const std::string local = (p == std::string::npos) ? qname : qname.substr(p + 1);
  // 不带前缀的 QName 用的是**默认命名空间**（XML Schema 的 QName 规则，
  // 与"元素名不带前缀"同一条），不是"没有命名空间"。
  return "{" + resolve(prefix) + "}" + local;
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

const char* xml_attr_raw(const pugi::xml_node& node, const char* name) {
  const pugi::xml_attribute a = node.attribute(name);
  return a ? a.value() : nullptr;
}

const char* xml_attr(const pugi::xml_node& node, const char* name,
                     const char* fallback) {
  const char* v = xml_attr_raw(node, name);
  return (v != nullptr && *v != '\0') ? v : fallback;
}

std::string xml_text(const pugi::xml_node& node) {
  std::string out;
  for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
    if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) {
      out += c.value();
    }
  }
  return out;
}

}  // namespace wsdl_detail
}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
