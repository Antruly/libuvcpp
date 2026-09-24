/**
 * @file src/wsdl/uvcpp_soap_message.cpp
 * @brief SOAP 信封的解析与序列化（实现）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 序列化这边的**一条**排版规则（头文件里那条说得很短，这里说清它怎么落地）：
 *
 * > **一个块的"第一行"由装它的那一层定位；从这个块的第二行起，缩进是绝对的。**
 *
 * 两条路都归到这一条上：
 *
 *   * **本文件生成的块**（响应包装元素、`Fault` 元素）第一行写成**不带缩进**，
 *     第 2 行起按绝对层写（`pad()` 按层算）—— 于是它们直接塞进 `Body` 那个
 *     位置就是对的，不需要调用方再加工；
 *   * **调用方给的片段**一个字节都不动，只被补上一个"第一行的缩进"。
 *
 * 为什么第 2 行起不许再加工：片段里行首的空白可能是**文本节点的内容**
 * （`<a>第一行\n  第二行</a>` 里那个换行与空格都是内容），动它就改了语义。
 */

#include <wsdl/uvcpp_soap_message.h>

#if UVCPP_WSDL_ENABLE

#include <wsdl/uvcpp_wsdl_pugixml.h>

#include <string>

namespace uvcpp {

const char* uvcpp_soap_version_name(uvcpp_soap_version v) {
  return (v == uvcpp_soap_version::V1_1) ? "1.1" : "1.2";
}

const char* uvcpp_soap_envelope_ns(uvcpp_soap_version v) {
  return (v == uvcpp_soap_version::V1_1) ? wsdl_ns::soap11_envelope()
                                         : wsdl_ns::soap12_envelope();
}

const char* uvcpp_soap_content_type(uvcpp_soap_version v) {
  return (v == uvcpp_soap_version::V1_1) ? "text/xml; charset=utf-8"
                                         : "application/soap+xml; charset=utf-8";
}

const char* uvcpp_soap_fault_code_local(uvcpp_soap_fault_code c,
                                        uvcpp_soap_version v) {
  switch (c) {
    case uvcpp_soap_fault_code::CLIENT:
      return (v == uvcpp_soap_version::V1_1) ? "Client" : "Sender";
    case uvcpp_soap_fault_code::SERVER:
      return (v == uvcpp_soap_version::V1_1) ? "Server" : "Receiver";
    case uvcpp_soap_fault_code::MUST_UNDERSTAND:
      return "MustUnderstand";
    case uvcpp_soap_fault_code::VERSION_MISMATCH:
      return "VersionMismatch";
    case uvcpp_soap_fault_code::DATA_ENCODING_UNKNOWN:
      // ★ 1.1 **没有**这个码（1.2 才加进来）。空串是刻意的返回值：
      //   硬凑一个名字发出去，对端会把它当成应用自定义码。
      return (v == uvcpp_soap_version::V1_1) ? "" : "DataEncodingUnknown";
  }
  return "";
}

// =========================================================================
// Content-Type / 动作
// =========================================================================

namespace {

char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string trim_ws(const std::string& s) {
  std::string::size_type b = 0;
  std::string::size_type e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
  return s.substr(b, e - b);
}

/** `Type/Subtype`（`;` 之前那一段），小写、去两端空白。 */
std::string media_type_of(const std::string& ct) {
  const std::string::size_type semi = ct.find(';');
  std::string t =
      trim_ws(ct.substr(0, semi == std::string::npos ? ct.size() : semi));
  for (std::string::size_type i = 0; i < t.size(); ++i) t[i] = lower_ascii(t[i]);
  return t;
}

/**
 * @brief 取 `;` 之后的参数里名叫 `name` 的那个**原样**值（键名大小写不敏感）。
 *
 * 值可能是 quoted-string（`action="urn:x;y"`），所以切参数时引号里的 `;`
 * 不算分隔符。返回值可能是带引号的 —— 由调用方决定要不要归一。
 */
std::string content_type_param(const std::string& ct, const char* name) {
  const std::string want = name;
  std::string::size_type i = ct.find(';');
  while (i != std::string::npos) {
    const std::string::size_type start = i + 1;  // 跳过 ';'
    std::string::size_type end = ct.find(';', start);
    const std::string::size_type q1 = ct.find('"', start);
    if (q1 != std::string::npos && (end == std::string::npos || q1 < end)) {
      const std::string::size_type q2 = ct.find('"', q1 + 1);
      if (q2 != std::string::npos) end = ct.find(';', q2 + 1);
    }
    const std::string seg =
        ct.substr(start, (end == std::string::npos ? ct.size() : end) - start);
    const std::string::size_type eq = seg.find('=');
    if (eq != std::string::npos) {
      std::string key = trim_ws(seg.substr(0, eq));
      for (std::string::size_type k = 0; k < key.size(); ++k) {
        key[k] = lower_ascii(key[k]);
      }
      if (key == want) return trim_ws(seg.substr(eq + 1));
    }
    i = end;
  }
  return std::string();
}

}  // namespace

bool uvcpp_soap_version_of_content_type(const std::string& ct,
                                        uvcpp_soap_version& out) {
  const std::string t = media_type_of(ct);
  if (t == "text/xml") {
    out = uvcpp_soap_version::V1_1;
    return true;
  }
  if (t == "application/soap+xml") {
    out = uvcpp_soap_version::V1_2;
    return true;
  }
  return false;
}

std::string uvcpp_soap_action_of_content_type(const std::string& ct) {
  return content_type_param(ct, "action");
}

std::string uvcpp_soap_normalize_action(const std::string& raw) {
  const std::string s = trim_ws(raw);
  if (s.size() >= 2 && s[0] == '"' && s[s.size() - 1] == '"') {
    // ★ 引号**里面**的空白也要剥。少这一步，`" urn:x "` 与 `"urn:x"` 会归一成两个
    //   不同的值，于是同一份 WSDL 对前一种客户端回 200、对后一种回 Fault —— 而
    //   两边看起来一模一样。引号里多出来的空白不可能是动作 URI 的一部分。
    return trim_ws(s.substr(1, s.size() - 2));
  }
  return s;
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

/**
 * @brief 取 `{信封命名空间}` 的限定属性（`mustUnderstand` 那类）。
 *
 * XML 里**不带前缀的属性永远不在默认命名空间里**（默认命名空间只作用于元素名），
 * 所以裸的 `mustUnderstand` 就是"没有命名空间"那个；带前缀的必须解析到**信封
 * 命名空间**才算 —— `foo:mustUnderstand` 而 `foo` 指着别处，那是个同名扩展，
 * 不是 SOAP 的属性。
 */
const char* env_attr(const ns_stack& ns, const pugi::xml_node& n,
                     const char* local, const char* env_uri) {
  for (pugi::xml_attribute a = n.first_attribute(); a; a = a.next_attribute()) {
    const std::string an = a.name();
    const std::string::size_type p = an.find(':');
    if (p == std::string::npos) {
      if (an == local) return a.value();
      continue;
    }
    if (an.substr(p + 1) != local) continue;
    if (ns.resolve(an.substr(0, p)) == env_uri) return a.value();
  }
  return nullptr;
}

/** `xsd:boolean` 的**真**：`1` 或 `true`（大小写不敏感）。其余一律不算真。 */
bool bool_true(const char* v) {
  if (v == nullptr) return false;
  std::string s;
  for (const char* p = v; *p != '\0'; ++p) s += lower_ascii(*p);
  return s == "1" || s == "true";
}

/** 数一个元素的**元素**子节点（注释与文本不算），顺带记下第一个。 */
struct child_scan {
  size_t count;
  pugi::xml_node first;
  child_scan() : count(0) {}
};

child_scan scan_children(const pugi::xml_node& parent, size_t cap,
                         bool* overflow) {
  child_scan sc;
  *overflow = false;
  for (pugi::xml_node c = parent.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (sc.count == 0) sc.first = c;
    ++sc.count;
    if (sc.count > cap) {
      *overflow = true;
      return sc;
    }
  }
  return sc;
}

/** 把一段 QName **文本**按当前作用域解析成 (uri, local) 两段。 */
void split_qname_text(const ns_stack& ns, const std::string& text,
                      std::string* space, std::string* local) {
  const std::string q = ns.resolve_qname(text);
  const std::string::size_type e = q.find('}');
  if (e == std::string::npos) {
    space->clear();
    *local = q;
    return;
  }
  *space = q.substr(1, e - 1);
  *local = q.substr(e + 1);
}

/** 在 `node` 的元素子节点里找 `{env}local`，取它的文本。没有就是空串。 */
std::string env_child_text(ns_stack& ns, const pugi::xml_node& node,
                           const char* env_uri, const char* local) {
  for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    ns.push(c);
    const bool hit = ns.is(c, env_uri, local);
    ns.pop();
    if (hit) return wd::xml_strip_cr(wd::xml_text(c));
  }
  return std::string();
}

/**
 * @brief 解析 `Fault` 的子元素。两个版本**走同一段代码**，按局部名认。
 *
 * 1.1 的四个子元素按规范是**没有命名空间**的（`<faultcode>` 而不是
 * `<soap:faultcode>`），1.2 的全在信封命名空间里。这里两边都收：URI 为空或
 * 等于信封命名空间就算 —— 真实世界里两种写法都有，把它们判死只会让一份能读的
 * 报文读不出来。**认不出的子元素跳过**（扩展元素，与 WSDL 那条宽法一致）。
 */
void parse_fault_children(ns_stack& ns, const pugi::xml_node& fault,
                          const char* env_uri, uvcpp_soap_fault* out) {
  for (pugi::xml_node c = fault.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    ns.push(c);
    const bool ours = ns.uri_of(c).empty() || ns.uri_of(c) == env_uri;
    const std::string local = ns.local_of(c);
    if (ours) {
      if (local == "faultcode") {
        split_qname_text(ns, wd::xml_strip_cr(wd::xml_text(c)),
                         &out->code_space, &out->code);
      } else if (local == "faultstring") {
        out->reason = wd::xml_strip_cr(wd::xml_text(c));
      } else if (local == "faultactor") {
        out->role = wd::xml_strip_cr(wd::xml_text(c));
      } else if (local == "detail") {
        out->detail = wd::xml_raw_element(c);
      } else if (local == "Code") {
        split_qname_text(ns, env_child_text(ns, c, env_uri, "Value"),
                         &out->code_space, &out->code);
        // `Subcode` 的值在**它的**子元素 `Value` 里，不在它自己的文本里 —— 所以
        // 要按元素找一层再取。
        //
        // ★ 这里**不能**先判 `env_child_text(c, "Subcode")` 空不空：`Subcode`
        //   只有元素子节点、没有文本子节点，那个值**永远是空串**，于是整个分支
        //   永远进不去（subcode 静默恒空）。4.14 这条判据就是钉这个的。
        for (pugi::xml_node s = c.first_child(); s; s = s.next_sibling()) {
          if (s.type() != pugi::node_element) continue;
          ns.push(s);
          const bool is_sub = ns.is(s, env_uri, "Subcode");
          ns.pop();
          if (!is_sub) continue;
          split_qname_text(ns, env_child_text(ns, s, env_uri, "Value"),
                           &out->subcode_space, &out->subcode);
          break;
        }
      } else if (local == "Reason") {
        // 规范允许多个 Text（按 xml:lang 区分）。本层只留**第一个** ——
        // 留一个而不是合并，是因为合并出来的句子没有任何语言。
        for (pugi::xml_node t = c.first_child(); t; t = t.next_sibling()) {
          if (t.type() != pugi::node_element) continue;
          ns.push(t);
          const bool is_text = ns.is(t, env_uri, "Text");
          ns.pop();
          if (!is_text) continue;
          out->reason = wd::xml_strip_cr(wd::xml_text(t));
          out->lang = wd::xml_attr(t, "xml:lang", "");
          break;
        }
      } else if (local == "Node") {
        out->node = wd::xml_strip_cr(wd::xml_text(c));
      } else if (local == "Role") {
        out->role = wd::xml_strip_cr(wd::xml_text(c));
      } else if (local == "Detail") {
        out->detail = wd::xml_raw_element(c);
      }
    }
    ns.pop();
  }
}

/** `Body` 的内容：恰好一个元素；那个元素是 `Fault` 的话按 Fault 解出来。 */
wsdl_status parse_body(ns_stack& ns, const pugi::xml_node& body,
                       const char* env_uri, const uvcpp_wsdl_limits& lim,
                       const char** why, uvcpp_soap_message* out) {
  bool overflow = false;
  const child_scan sc = scan_children(body, lim.max_items, &overflow);
  if (overflow) {
    return fail(wsdl_status::TOO_MANY_ITEMS, why, "Body 的子元素太多");
  }
  out->body_children = sc.count;
  if (sc.count == 0) {
    return fail(wsdl_status::SOAP_NO_BODY, why,
                "Body 里一个元素都没有 —— 没有可派发的东西");
  }
  if (sc.count > 1) {
    return fail(wsdl_status::SOAP_TOO_MANY_BODY_ELEMENTS, why,
                "Body 里有多于一个元素 —— SOAP 报文只装一个（1.1 §4.3 / 1.2 §5.1）");
  }

  ns.push(sc.first);
  out->body_local = ns.local_of(sc.first);
  out->body_ns = ns.uri_of(sc.first);
  out->body_xml = wd::xml_raw_element(sc.first);
  const bool is_fault = ns.is(sc.first, env_uri, "Fault");
  ns.pop();

  if (is_fault) {
    // 对端在报错。这**不是**解析失败：报文本身完全合法。
    out->has_fault = true;
    out->fault.version = out->version;
    parse_fault_children(ns, sc.first, env_uri, &out->fault);
    if (out->fault.code.empty()) {
      // `faultcode` / `Code` 两个版本都要求有。与其当成成功、让上层拿到一条
      // 没有码的 Fault（它要回 Fault 时回的必须是**自己**的码），不如在这里断。
      return fail(wsdl_status::SOAP_BAD_FAULT, why,
                  "Body 里是一个没有 faultcode / Code 的 Fault");
    }
  }
  return wsdl_status::OK;
}

}  // namespace

wsdl_status uvcpp_soap_parse(const char* data, size_t len,
                             uvcpp_soap_message& out,
                             const uvcpp_wsdl_limits& lim, const char** why) {
  out.clear();
  if (why != nullptr) *why = "";

  const wd::xml_limits xlim(lim.max_bytes, lim.max_depth, lim.max_nodes);
  pugi::xml_document pd;
  const wd::xml_result xr = wd::xml_parse(data, len, pd, xlim, why);
  if (xr != wd::xml_result::OK) return wd::from_xml_result(xr, why);

  pugi::xml_node root = pd.document_element();
  if (!root) {
    return fail(wsdl_status::SOAP_NOT_ENVELOPE, why, "文档里没有根元素");
  }

  ns_stack ns;
  ns.push(root);

  // 版本认的是**信封自己声明的那个 URI**（见 .h 那张表）。两代都支持，所以
  // 这里不会出现"版本不认识"以外的情形 —— `VersionMismatch` 也因此在入口处
  // 不会产生（那头有一条同样的说明）。
  const std::string uri = ns.uri_of(root);
  const bool is_env = (ns.local_of(root) == "Envelope") &&
                      (uri == wsdl_ns::soap11_envelope() ||
                       uri == wsdl_ns::soap12_envelope());
  if (!is_env) {
    ns.pop();
    return fail(wsdl_status::SOAP_NOT_ENVELOPE, why,
                "根元素不是 {…schemas.xmlsoap.org/soap/envelope/}Envelope，"
                "也不是 {…www.w3.org/2003/05/soap-envelope}Envelope");
  }
  out.version = (uri == wsdl_ns::soap11_envelope()) ? uvcpp_soap_version::V1_1
                                                    : uvcpp_soap_version::V1_2;
  const char* env_uri = uvcpp_soap_envelope_ns(out.version);

  wsdl_status st = wsdl_status::OK;
  bool seen_body = false;
  bool seen_header = false;
  for (pugi::xml_node c = root.first_child(); c; c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    ns.push(c);
    const bool is_header = ns.is(c, env_uri, "Header");
    const bool is_body = ns.is(c, env_uri, "Body");
    if (is_header && !seen_header && !seen_body) {
      seen_header = true;
      out.header_xml = wd::xml_raw_element(c);
      bool overflow = false;
      const child_scan sc = scan_children(c, lim.max_items, &overflow);
      if (overflow) {
        st = fail(wsdl_status::TOO_MANY_ITEMS, why, "Header 的子元素太多");
      } else {
        out.header_entries = sc.count;
        // mustUnderstand 只**数**不判："认不认识"只有应用知道（见 .h）。
        for (pugi::xml_node h = c.first_child(); h; h = h.next_sibling()) {
          if (h.type() != pugi::node_element) continue;
          ns.push(h);
          if (bool_true(env_attr(ns, h, "mustUnderstand", env_uri))) {
            ++out.header_must_understand;
          }
          ns.pop();
        }
      }
    } else if (is_body && !seen_body) {
      seen_body = true;
      st = parse_body(ns, c, env_uri, lim, why, &out);
    }
    // 其余的（第二个 Header/Body、以及 Envelope 下认不出的元素）一律跳过：
    // 宽法与 WSDL 那条一致（认不出的元素跳过），顺序也不校验 —— 把 Body 排在
    // Header 前面判成坏报文，只会让一份读得懂的消息读不出来。
    ns.pop();
    if (st != wsdl_status::OK) break;
  }
  ns.pop();

  if (st == wsdl_status::OK && !seen_body) {
    st = fail(wsdl_status::SOAP_NO_BODY, why, "没有 Body 元素");
  }
  if (st != wsdl_status::OK) out.clear();
  return st;
}

wsdl_status uvcpp_soap_parse(const std::string& xml, uvcpp_soap_message& out,
                             const uvcpp_wsdl_limits& lim, const char** why) {
  return uvcpp_soap_parse(xml.data(), xml.size(), out, lim, why);
}

// =========================================================================
// 序列化
// =========================================================================

namespace {

/** 第 lvl 层的缩进（lvl 从 0 起）。 */
std::string pad(size_t lvl, const uvcpp_soap_dump_options& opt) {
  return std::string(lvl * opt.indent, ' ');
}

/** `<name>text</name>` 一行（`text` 已经转义好）。 */
void elem_text(std::string& out, size_t lvl, const std::string& name,
               const std::string& text, const uvcpp_soap_dump_options& opt) {
  out += pad(lvl, opt);
  out += "<" + name + ">" + text + "</" + name + ">\n";
}

/** 元素名的限定形式：前缀为空就是裸名（那时命名空间靠根上的默认声明）。 */
std::string q(const std::string& prefix, const char* local) {
  if (prefix.empty()) return std::string(local);
  return prefix + ":" + local;
}

/**
 * @brief `Fault` 元素，**第一行不带缩进**、`base` 是它在信封里的绝对层。
 *
 * 为什么拆成"按绝对层生成"而不是"生成好再嵌"：片段规则只缩**第一行**，一个
 * 多行的块按那条规则嵌进去，第 2 行起会全部贴左边。而这个块自己是知道该缩多少的
 * —— 它在信封里的位置是固定的（`Envelope` 0 / `Body` 1 / `Fault` 2）。
 */
std::string fault_element_block(const uvcpp_soap_fault& f,
                                const std::string& env_prefix, size_t width,
                                size_t base) {
  const uvcpp_soap_dump_options opt(env_prefix, width);
  const std::string fault_elem = q(env_prefix, "Fault");
  // `faultcode` 的值是个 **QName**：有前缀时写成 `soap:Client`；前缀为空时
  // 裸的 `Client` **也是对的** —— 按 XML Schema 的规则，QName 里不带前缀的名字
  // 解析到**默认命名空间**，而那个默认命名空间正是信封那个（`dump_envelope`
  // 在前缀为空时写的就是 `xmlns="…"`）。所以这条不是将就。
  const std::string code_qname =
      env_prefix.empty() ? f.code : env_prefix + ":" + f.code;
  const std::string reason = uvcpp_xml_escape_text(f.reason);

  std::string out;
  out += "<" + fault_elem + ">\n";
  if (f.version == uvcpp_soap_version::V1_1) {
    // ★ 1.1 的四个子元素**没有命名空间**（裸名），1.2 的全在信封命名空间里；
    //   大小写也不同（`faultcode` vs `Code`）—— 见 .h 那张表。
    elem_text(out, base + 1, "faultcode", code_qname, opt);
    elem_text(out, base + 1, "faultstring", reason, opt);
    if (!f.role.empty()) {
      elem_text(out, base + 1, "faultactor", uvcpp_xml_escape_text(f.role), opt);
    }
    if (!f.detail.empty()) {
      out += pad(base + 1, opt) + f.detail + "\n";  // 整段原样（见 .h）
    }
  } else {
    const std::string code_el = q(env_prefix, "Code");
    const std::string val_el = q(env_prefix, "Value");
    const std::string sub_el = q(env_prefix, "Subcode");
    out += pad(base + 1, opt) + "<" + code_el + ">\n";
    elem_text(out, base + 2, val_el, code_qname, opt);
    if (!f.subcode.empty()) {
      out += pad(base + 2, opt) + "<" + sub_el + ">\n";
      elem_text(out, base + 3, val_el,
                env_prefix.empty() ? f.subcode : env_prefix + ":" + f.subcode,
                opt);
      out += pad(base + 2, opt) + "</" + sub_el + ">\n";
    }
    out += pad(base + 1, opt) + "</" + code_el + ">\n";

    const std::string reason_el = q(env_prefix, "Reason");
    const std::string text_el = q(env_prefix, "Text");
    out += pad(base + 1, opt) + "<" + reason_el + ">\n";
    // 1.2 的 `Reason/Text` 带 `xml:lang`（规范要求它必须有）—— 单独拼这一行，
    // 因为 `elem_text` 只发文本、发不出属性。缺省 `en`：规范没规定缺省语言，
    // 但一条没有 lang 的 Text 是不合规的，写一个总比不写强。
    out += pad(base + 2, opt) + "<" + text_el + " xml:lang=\"" +
           uvcpp_xml_escape_attr(f.lang.empty() ? "en" : f.lang) + "\">" +
           reason + "</" + text_el + ">\n";
    out += pad(base + 1, opt) + "</" + reason_el + ">\n";

    if (!f.node.empty()) {
      elem_text(out, base + 1, q(env_prefix, "Node"),
                uvcpp_xml_escape_text(f.node), opt);
    }
    if (!f.role.empty()) {
      elem_text(out, base + 1, q(env_prefix, "Role"),
                uvcpp_xml_escape_text(f.role), opt);
    }
    if (!f.detail.empty()) {
      out += pad(base + 1, opt) + f.detail + "\n";
    }
  }
  out += pad(base, opt) + "</" + fault_elem + ">";
  return out;
}

/**
 * @brief 响应元素，**第一行不带缩进**（`base` 只用于它下面那几行）。
 *
 * 包装元素只做两件事：把 `op_ns` 作为默认命名空间带上、把 `inner_xml` 放进去。
 * **不做** rpc 那套 `part` 包装 —— 那要按 `message` 的 `part` 表来组，而这一层
 * 不认识 WSDL（在 `uvcpp_soap_service` 里）。
 */
std::string response_wrapper_block(const std::string& op_local,
                                   const std::string& op_ns,
                                   const std::string& inner_xml, size_t width,
                                   size_t base) {
  const uvcpp_soap_dump_options opt(std::string(), width);
  std::string out;
  out += "<" + op_local;
  if (!op_ns.empty()) {
    out += " xmlns=\"" + uvcpp_xml_escape_attr(op_ns) + "\"";
  }
  if (inner_xml.empty()) {
    out += "/>";
    return out;  // 无内容：自闭合（`<Op/>`），比 `<Op></Op>` 干净
  }
  out += ">\n";
  out += pad(base + 1, opt) + inner_xml + "\n";  // 片段：只缩第一行
  out += pad(base, opt) + "</" + op_local + ">";
  return out;
}

}  // namespace

std::string uvcpp_soap_dump_envelope(uvcpp_soap_version v,
                                     const std::string& header_entries_xml,
                                     const std::string& body_entries_xml,
                                     const uvcpp_soap_dump_options& opt) {
  const char* ns = uvcpp_soap_envelope_ns(v);
  const std::string env = q(opt.env_prefix, "Envelope");
  const std::string body = q(opt.env_prefix, "Body");

  std::string out;
  out += "<" + env;
  if (opt.env_prefix.empty()) {
    out += " xmlns=\"";
  } else {
    out += " xmlns:" + opt.env_prefix + "=\"";
  }
  out += ns;
  out += "\">\n";

  if (!header_entries_xml.empty()) {
    const std::string hdr = q(opt.env_prefix, "Header");
    out += pad(1, opt) + "<" + hdr + ">\n";
    out += pad(2, opt) + header_entries_xml + "\n";  // 片段：只缩第一行
    out += pad(1, opt) + "</" + hdr + ">\n";
  }

  out += pad(1, opt) + "<" + body + ">\n";
  if (!body_entries_xml.empty()) {
    out += pad(2, opt) + body_entries_xml + "\n";  // 同上
  }
  out += pad(1, opt) + "</" + body + ">\n";
  out += "</" + env + ">\n";
  return out;
}

std::string uvcpp_soap_dump_response(uvcpp_soap_version v,
                                     const std::string& op_local,
                                     const std::string& op_ns,
                                     const std::string& inner_xml,
                                     const uvcpp_soap_dump_options& opt) {
  // 包装元素在信封里的位置是固定的（Envelope 0 / Body 1 / 包装 2），所以按那个
  // 绝对层生成，再作为 Body 的"片段"交给 dump_envelope —— 那边只给第一行补
  // 一个 Body 内容层的缩进，块自己的第 2 行起已经是绝对缩进了（见文件头）。
  return uvcpp_soap_dump_envelope(
      v, std::string(),
      response_wrapper_block(op_local, op_ns, inner_xml, opt.indent, 2u), opt);
}

std::string uvcpp_soap_dump_fault_element(const uvcpp_soap_fault& f,
                                          const std::string& env_prefix) {
  // 以**自己**为第 0 层缩进（`dump_fault` 嵌进 Body 时用的是绝对层，所以两者
  // 的缩进不同 —— 这个入口是给用例与"只想看一眼"的调用点的）。
  return fault_element_block(f, env_prefix, 2u, 0u);
}

std::string uvcpp_soap_dump_fault(const uvcpp_soap_fault& f,
                                  const uvcpp_soap_dump_options& opt) {
  return uvcpp_soap_dump_envelope(
      f.version, std::string(),
      fault_element_block(f, opt.env_prefix, opt.indent, 2u), opt);
}

uvcpp_soap_fault uvcpp_soap_make_fault(uvcpp_soap_version v,
                                       uvcpp_soap_fault_code code,
                                       const std::string& reason,
                                       const std::string& detail_inner_xml) {
  uvcpp_soap_fault f;
  f.version = v;
  const char* local = uvcpp_soap_fault_code_local(code, v);
  f.code = (local != nullptr) ? local : "";
  f.reason = reason;
  if (!detail_inner_xml.empty()) {
    // `detail` 存的是**整个元素**（与 `types_xml` 同一个取舍：它可能自带
    // xmlns 声明）。所以这里把标签一起拼进来，而不是只存内容。
    const char* elem = (v == uvcpp_soap_version::V1_1) ? "detail" : "Detail";
    f.detail = "<" + std::string(elem) + ">" + detail_inner_xml + "</" +
               elem + ">";
  }
  return f;
}

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
