/**
 * @file src/wsdl/uvcpp_soap_service.cpp
 * @brief SOAP 路由那半边的实现（装配、派发、Fault、状态码）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <wsdl/uvcpp_soap_service.h>

#if UVCPP_WSDL_ENABLE

#include <string>

#include <webapp/uvcpp_log.h>
#include <wsdl/uvcpp_wsdl_pugixml.h>

namespace uvcpp {

namespace {

namespace det = wsdl_detail;

// =========================================================================
// 发给对端的那句话：一律英文
// =========================================================================
//
// `reason` / `faultstring` 是**发给对端**的，读它的常常是别人的 SOAP 工具包；而且
// 它是"这次调用为什么失败"的诊断，不是给我们自己日志看的东西。所以这一层发出去的
// 每一句都是英文，本地那句中文诊断（`uvcpp_soap_parse` 的 `why`）**不塞进去** ——
// 塞进去两头不讨好：对端看不懂，我们还以为已经把上下文给出去了。要看那句就自己
// 在派发之前调一次 `uvcpp_soap_parse()`（`uvcpp_soap_message.h`，与 WSDL 无关）。
const char* k_reason_bad_ct = "Content-Type is not a SOAP media type";
const char* k_reason_bad_envelope = "SOAP envelope is not usable: ";
const char* k_reason_invalid_endpoint =
    "this endpoint was not assembled from the WSDL";
const char* k_reason_request_is_fault =
    "the request body is a SOAP Fault, not a call";
const char* k_reason_must_understand =
    "a header with mustUnderstand was not understood; count: ";
const char* k_reason_no_such_op = "unrecognized SOAP body element: ";
const char* k_reason_not_implemented =
    "operation is declared in the WSDL but not implemented: ";
const char* k_reason_action_mismatch = "action does not match the WSDL; declared: ";
const char* k_reason_no_response = "the handler produced no response";

const char* k_why_ok = "ok";
const char* k_why_no_service_name = "no service with that name in this WSDL";
const char* k_why_no_service = "the WSDL declares no service";
const char* k_why_many_services = "the WSDL declares several services; name one";
const char* k_why_no_port_name = "no port with that name in that service";
const char* k_why_no_soap_port =
    "that service has no port whose binding is a SOAP binding";
const char* k_why_many_soap_ports = "that service has several SOAP ports; name one";
const char* k_why_no_binding =
    "the port refers to a binding that is not in this WSDL";
const char* k_why_binding_not_soap = "that port's binding is not a SOAP binding";
const char* k_why_no_port_type =
    "the binding refers to a portType that is not in this WSDL";

// =========================================================================
// 模型里按 QName 找一条
// =========================================================================

/**
 * @brief 在 `messages` / `port_types` / `bindings` 里按 QName 找一条同名的。
 *
 * 三处的 `@name` 都叫 `name`，所以一套模板够用。
 *
 * ★ 不用 `uvcpp_wsdl_document::find_message()` 那一组，因为那组的 `{uri}local` 写法
 * **要求 uri 逐字节等于 targetNamespace**，而这里要判的是"这个引用是不是指向本文档"。
 * 规则：`q.space` 非空且不等于 targetNamespace ⇒ 返回 nullptr —— 那种引用指向**别的
 * 文档**（`wsdl:import`），而模型只**记账**、不取回（见 `uvcpp_wsdl_document.h`），
 * 所以它确实不在本文档里。拿局部名硬凑一条同名的出来，会把"引用了没取回的文档"
 * 静默变成"就是本文档里那条"。
 */
template <typename T>
const T* find_named(const std::vector<T>& all, const uvcpp_qname& q,
                    const std::string& tns) {
  if (q.local.empty()) return nullptr;
  if (!q.space.empty() && q.space != tns) return nullptr;
  for (size_t i = 0; i < all.size(); ++i) {
    if (all[i].name == q.local) return &all[i];
  }
  return nullptr;
}

/**
 * @brief 一条 binding operation 的包装元素 QName（派发键 / 响应元素共用这一条规则）。
 *
 * **第一个 part 声明了 `@element` 就按它，否则 `{soap:body/@namespace}` + `fallback_name`** ——
 * 见头文件那张表。不按 `style` 分叉：那条规则天然覆盖 `document`、`rpc` 与
 * "part 只声明了 `@type`"，分叉只多一处能写错的地方。
 *
 * `fallback_name` 由调用方给：**请求侧是操作名，响应侧是操作名 + `Response`**。
 * 这不是对称的 —— `rpc/literal` 的 part 只声明 `@type`（没有 `@element`），于是两个
 * 方向的包装元素**只能**靠这条兜底规则造出来；两边都取操作名的话请求与响应就是**同一个
 * QName**，对端分不出手上这段 XML 是请求还是响应。`document/literal` 走不到兜底
 * （它的 output part 声明了 `@element = ...Response`），所以这条只对 rpc 生效。
 */
uvcpp_qname wrapper_of(const uvcpp_wsdl_document& d, const uvcpp_qname& msg_ref,
                       const std::string& ns_fallback,
                       const std::string& fallback_name) {
  const uvcpp_wsdl_message* m = find_named(d.messages, msg_ref, d.target_namespace);
  if (m != nullptr && !m->parts.empty() && !m->parts[0].element.empty()) {
    return m->parts[0].element;
  }
  return uvcpp_qname(ns_fallback, fallback_name);
}

// =========================================================================
// 版本、Fault 的码与 HTTP 状态码
// =========================================================================

bool accepts(uvcpp_soap_version_policy p, uvcpp_soap_version v) {
  switch (p) {
    case uvcpp_soap_version_policy::ANY:
      return true;
    case uvcpp_soap_version_policy::V1_1_ONLY:
      return v == uvcpp_soap_version::V1_1;
    case uvcpp_soap_version_policy::V1_2_ONLY:
      return v == uvcpp_soap_version::V1_2;
  }
  // 到不了这里：没有 `default:` 是为了让漏掉新枚举值这件事由 `-Wswitch` 报出来。
  return true;
}

/** @brief 语义码 -> HTTP 状态码（见头文件那张码表）。 */
int fault_status(uvcpp_soap_fault_code c, uvcpp_soap_version v) {
  // 1.1 只定义了 500（§6.2 的每个例子都是 500）；准入失败那条 415 不走这里。
  if (v == uvcpp_soap_version::V1_1) return 500;
  // 1.2 分两张：Sender 一族 4xx、Receiver 5xx。
  return c == uvcpp_soap_fault_code::SERVER ? 500 : 400;
}

/**
 * @brief 从一条 Fault 的**本地名**反查语义码。
 *
 * 处理函数可以端一条整的 `uvcpp_soap_fault` 上来（那时它手里只有本地名，比如
 * `"Sender"`），而 HTTP 状态码要看的是语义。反查一遍比让调用方再填一个字段省事。
 *
 * 认不出来（应用自定义码）时按 `CLIENT` 算 —— 1.2 下就是 400。自定义码在规范里
 * 本来就该挂在标准码的 `Subcode` 底下，一个裸的自定义顶层码几乎没有别的信息可依据，
 * 而"这个码多半是说请求有问题"是更好的默认（头文件里记着这条）。
 */
uvcpp_soap_fault_code semantic_code_of(const uvcpp_soap_fault& f) {
  const uvcpp_soap_fault_code all[] = {
      uvcpp_soap_fault_code::CLIENT, uvcpp_soap_fault_code::SERVER,
      uvcpp_soap_fault_code::MUST_UNDERSTAND,
      uvcpp_soap_fault_code::VERSION_MISMATCH,
      uvcpp_soap_fault_code::DATA_ENCODING_UNKNOWN};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    const char* local = uvcpp_soap_fault_code_local(all[i], f.version);
    if (local != nullptr && local[0] != '\0' && f.code == local) return all[i];
  }
  return uvcpp_soap_fault_code::CLIENT;
}

void send_fault(uvcpp_web_response& resp, const uvcpp_soap_fault& f, int status,
                const uvcpp_soap_dump_options& opt) {
  resp.status(status).body(uvcpp_soap_dump_fault(f, opt),
                           uvcpp_soap_content_type(f.version));
}

/** @brief 单向 operation 的正常结局：202 + 空正文。 */
void send_accepted(uvcpp_web_response& resp) {
  resp.status(202).body(std::string());
}

/**
 * @brief 这个请求提供的动作值（归一后）与"提供了非空的值吗"。
 *
 * 1.1 看 HTTP 头，1.2 看 `Content-Type` 的 `action=` 参数 —— 读哪一个由**信封解析
 * 出来的版本**决定（与"版本从信封认"同一条规则）。两个版本共用一个 `present`
 * 定义：有**非空**的值才算提供了（见头文件里那三条规则）。
 */
std::string action_of(const uvcpp_web_request& req, uvcpp_soap_version v,
                      bool* present) {
  std::string raw;
  if (v == uvcpp_soap_version::V1_2) {
    raw = uvcpp_soap_action_of_content_type(req.content_type());
  } else {
    raw = req.header("SOAPAction", std::string());
  }
  const std::string norm = uvcpp_soap_normalize_action(raw);
  *present = !norm.empty();
  return norm;
}

// =========================================================================
// 片段解析（`uvcpp_soap_call::arg()` 那两条用）
// =========================================================================

/** @brief 把一个片段解析成一棵树；失败时给一个空节点（不抛、不报）。 */
pugi::xml_node parse_fragment(const std::string& xml, pugi::xml_document& doc) {
  if (xml.empty()) return pugi::xml_node();
  const det::xml_result r =
      det::xml_parse(xml.data(), xml.size(), doc, det::xml_limits(), nullptr);
  if (r != det::xml_result::OK) return pugi::xml_node();
  return doc.document_element();
}

/** @brief 第一个**局部名**是 `local` 的直接子元素（文本/注释节点跳过）。 */
pugi::xml_node first_child_named(const pugi::xml_node& root,
                                 const std::string& local) {
  if (root.empty()) return pugi::xml_node();
  // `local_of()` 只看名字里有没有 `':'`、不用作用域栈，但它是成员函数。
  const det::xml_ns_stack ns;
  for (pugi::xml_node c = root.first_child(); !c.empty(); c = c.next_sibling()) {
    if (c.type() != pugi::node_element) continue;
    if (ns.local_of(c) == local) return c;
  }
  return pugi::xml_node();
}

}  // namespace

// =========================================================================
// 版本策略
// =========================================================================

const char* uvcpp_soap_version_policy_name(uvcpp_soap_version_policy p) {
  switch (p) {
    case uvcpp_soap_version_policy::ANY:
      return "any";
    case uvcpp_soap_version_policy::V1_1_ONLY:
      return "1.1-only";
    case uvcpp_soap_version_policy::V1_2_ONLY:
      return "1.2-only";
  }
  return "?";
}

// =========================================================================
// 一次调用
// =========================================================================

std::string uvcpp_soap_call::arg(const std::string& local) const {
  pugi::xml_document d;
  const pugi::xml_node c = first_child_named(parse_fragment(body_xml, d), local);
  return c.empty() ? std::string() : det::xml_text(c);
}

std::string uvcpp_soap_call::child_xml(const std::string& local) const {
  pugi::xml_document d;
  const pugi::xml_node c = first_child_named(parse_fragment(body_xml, d), local);
  return c.empty() ? std::string() : det::xml_raw_element(c);
}

// =========================================================================
// 一次回复
// =========================================================================

void uvcpp_soap_reply::set_response(const std::string& inner_xml) {
  kind = kind_t::RESPONSE;
  named = false;
  local.clear();
  ns.clear();
  inner = inner_xml;
}

void uvcpp_soap_reply::set_response_named(const std::string& local_in,
                                          const std::string& ns_in,
                                          const std::string& inner_xml) {
  kind = kind_t::RESPONSE;
  named = true;
  local = local_in;
  ns = ns_in;
  inner = inner_xml;
}

void uvcpp_soap_reply::set_fault(uvcpp_soap_fault_code code_in,
                                 const std::string& reason_in,
                                 const std::string& detail_inner_xml) {
  kind = kind_t::FAULT;
  use_full = false;
  code = code_in;
  reason = reason_in;
  detail_inner = detail_inner_xml;
  full = uvcpp_soap_fault();
}

void uvcpp_soap_reply::set_fault(const uvcpp_soap_fault& f) {
  kind = kind_t::FAULT;
  use_full = true;
  full = f;
}

uvcpp_soap_fault uvcpp_soap_reply::fault_for(uvcpp_soap_version v) const {
  if (!use_full) {
    const uvcpp_soap_fault f = uvcpp_soap_make_fault(v, code, reason, detail_inner);
    // ★ 该版本里不存在的码（今天只有 1.1 的 `DATA_ENCODING_UNKNOWN`）发出来是**空码**，
    //   写到线上就是 `soap:` —— 一个非法 QName，对端连码都读不到。
    //   `tests/functional/soap_message_func.cpp` 把"保证这种 Fault 不会发出去"记成了
    //   **这一层**的责任，所以落到 `CLIENT`：理由那句原样保留，换个能解开的码。
    if (f.code.empty()) {
      return uvcpp_soap_make_fault(v, uvcpp_soap_fault_code::CLIENT, reason,
                                   detail_inner);
    }
    return f;
  }
  // 整条带来的那条：版本以**请求**的为准 —— 一条 1.2 的请求收不下 1.1 形状的 Fault
  // （对端按 1.2 解，只会看到一堆它不认识的孩子）。其余字段原样。
  uvcpp_soap_fault f = full;
  f.version = v;
  return f;
}

// =========================================================================
// 端点：装配
// =========================================================================

uvcpp_soap_service::uvcpp_soap_service(const uvcpp_wsdl_document& doc,
                                       const std::string& service,
                                       const std::string& port)
    : doc_(doc) {
  build(service, port);

  // 装配只在这一处收口：`build()` 内部有九条失败分支，每条 `why_` 都不同，
  // 但**都**是"这个端点不可用"这一件事。在这里记一条，既盖住了全部九条，
  // 也把"到底哪一条"交给 `why_`（它是精确的静态串，比在九个地方各写一句
  // 更容易保持同步）。头文件 `uvcpp_soap_service.h` 里那句「判据与**启动
  // 日志**都该看它」指的就是这里 —— 构造函数跑在循环起来之前，正是启动期。
  if (!valid_) {
    UVCPP_LOG_ERROR(log_category::SOAP)
        << "SOAP 端点装配失败：" << why_ << "（service=\"" << service
        << "\", port=\"" << port
        << "\"）—— 这个端点会对每个请求回 Server Fault";
  }
}

void uvcpp_soap_service::build(const std::string& service,
                               const std::string& port) {
  // ---- 1. 挑 service ----
  const uvcpp_wsdl_service* svc = nullptr;
  if (!service.empty()) {
    svc = doc_.find_service(service);
    if (svc == nullptr) {
      why_ = k_why_no_service_name;
      return;
    }
  } else if (doc_.services.empty()) {
    why_ = k_why_no_service;
    return;
  } else if (doc_.services.size() > 1) {
    why_ = k_why_many_services;
    return;
  } else {
    svc = &doc_.services[0];
  }
  service_name_ = svc->name;

  // ---- 2. 挑 port ----
  // 不点名时只数**绑到 SOAP 的**那些 port：一个 service 底下常同时挂 SOAP 与
  // HTTP-GET 两个 port，把那个也算进去就永远"有好几个，请点名"了。
  const uvcpp_wsdl_port* p = nullptr;
  if (!port.empty()) {
    p = svc->find_port(port);
    if (p == nullptr) {
      why_ = k_why_no_port_name;
      return;
    }
  } else {
    const uvcpp_wsdl_port* only = nullptr;
    size_t n = 0;
    for (size_t i = 0; i < svc->ports.size(); ++i) {
      const uvcpp_wsdl_binding* b =
          find_named(doc_.bindings, svc->ports[i].binding, doc_.target_namespace);
      if (b == nullptr || !b->soap.present) continue;
      only = &svc->ports[i];
      ++n;
    }
    if (n == 0) {
      why_ = k_why_no_soap_port;
      return;
    }
    if (n > 1) {
      why_ = k_why_many_soap_ports;
      return;
    }
    p = only;
  }
  port_name_ = p->name;

  // ---- 3. binding / portType ----
  const uvcpp_wsdl_binding* b =
      find_named(doc_.bindings, p->binding, doc_.target_namespace);
  if (b == nullptr) {
    why_ = k_why_no_binding;
    return;
  }
  if (!b->soap.present) {
    why_ = k_why_binding_not_soap;
    return;
  }
  binding_name_ = b->name;

  const uvcpp_wsdl_port_type* pt =
      find_named(doc_.port_types, b->port_type, doc_.target_namespace);
  if (pt == nullptr) {
    why_ = k_why_no_port_type;
    return;
  }
  port_type_name_ = pt->name;

  // ---- 4. 收 operation ----
  // 从 **binding** 收（不是 portType）：binding 才是"这条路走 SOAP 时暴露了什么"。
  // portType 里有一条而 binding 里没有的 operation，在这个端点上**到不了**。
  for (size_t i = 0; i < b->operations.size(); ++i) {
    const uvcpp_wsdl_binding_operation& bo = b->operations[i];
    const uvcpp_wsdl_operation* po = pt->find_operation(bo.name);
    // `po` 为空 = binding 里有一条 portType 里没有的 operation（坏文档，
    // `uvcpp_wsdl_check_references()` 会报它）。**照样收下**：跳过它只会让一个
    // 服务方的文档错误在上线后表现成"对端发错了"（`Sender` Fault）。
    op_entry e;
    e.name = bo.name;
    e.in_wrapper =
        wrapper_of(doc_, po != nullptr ? po->input.message : uvcpp_qname(),
                   bo.input_namespace, bo.name);
    // 响应侧兜底名是 `操作名 + "Response"`（不是操作名）：`rpc/literal` 的 part 只有
    // `@type`，包装元素**只能**由这条规则造，两边同名就等于请求与响应用同一个 QName。
    e.out_wrapper =
        wrapper_of(doc_, po != nullptr ? po->output.message : uvcpp_qname(),
                   bo.output_namespace, bo.name + "Response");
    e.soap_action = bo.soap_action;
    // 认不出 portType 那条时按**请求-响应**算：单向是"没有 output"这个**在不在**
    // 的事实，不是默认值 —— 猜"单向"会让缺响应变成一个静默的 202。
    e.has_output = po != nullptr ? po->has_output : true;
    ops_.push_back(e);
  }
  // 索引最后建：同一个键出现两次时**最后一条赢**（头文件里记着这条是静默的）。
  // 记一条 WARN ——"静默"说的是**行为**（不报错、不抛），不是"没人该知道"：
  // 派发键撞车意味着前面那条 operation 永远轮不到，多半是 WSDL 写重了。
  for (size_t i = 0; i < ops_.size(); ++i) {
    const std::string key = ops_[i].in_wrapper.str();
    if (index_.find(key) != index_.end()) {
      UVCPP_LOG_WARN(log_category::SOAP)
          << "派发键重复：\"" << key << "\"（" << ops_[index_[key]].name
          << " 被 " << ops_[i].name << " 覆盖，后者生效）";
    }
    index_[key] = i;
  }

  valid_ = true;
  why_ = k_why_ok;
}

const char* uvcpp_soap_service::why() const { return why_; }

std::vector<std::string> uvcpp_soap_service::operation_names() const {
  std::vector<std::string> out;
  out.reserve(ops_.size());
  for (size_t i = 0; i < ops_.size(); ++i) out.push_back(ops_[i].name);
  return out;
}

bool uvcpp_soap_service::has_operation(const std::string& name) const {
  for (size_t i = 0; i < ops_.size(); ++i) {
    if (ops_[i].name == name) return true;
  }
  return false;
}

std::string uvcpp_soap_service::dispatch_target_of(
    const std::string& body_qname) const {
  const std::map<std::string, size_t>::const_iterator it = index_.find(body_qname);
  if (it == index_.end()) return std::string();
  return ops_[it->second].name;
}

uvcpp_soap_service& uvcpp_soap_service::operation(const std::string& name,
                                                  const operation_fn& fn) {
  handlers_[name] = fn;  // 替换语义：同名后注册的赢
  return *this;
}

uvcpp_soap_service& uvcpp_soap_service::set_version_policy(
    uvcpp_soap_version_policy p) {
  policy_ = p;
  return *this;
}

uvcpp_soap_service& uvcpp_soap_service::set_limits(const uvcpp_wsdl_limits& lim) {
  limits_ = lim;
  return *this;
}

uvcpp_soap_service& uvcpp_soap_service::set_dump_options(
    const uvcpp_soap_dump_options& opt) {
  dump_ = opt;
  return *this;
}

// =========================================================================
// 端点：处理一个请求
// =========================================================================

void uvcpp_soap_service::handle(uvcpp_web_request& req,
                               uvcpp_web_response& resp) const {
  // ---- 1. 准入 ----
  // 这一步只会用上"是不是 SOAP 的两种之一"：解析失败的 Fault 要发成哪一版，只能
  // 靠 `Content-Type` 给的那一版（报文本身还没解出来）。所以它必须排在解析之前。
  // 准入失败时 `hinted` 保持 1.1 —— 没有任何版本线索时 1.1 是兼容面最大的形状。
  uvcpp_soap_version hinted = uvcpp_soap_version::V1_1;
  if (!uvcpp_soap_version_of_content_type(req.content_type(), hinted)) {
    // DEBUG 而不是 WARN：这条路由挂在业务自己的路径上，而浏览器/探针/误配的
    // 反代都会往那儿送普通请求 —— 它是噪声，不是信号。
    UVCPP_LOG_DEBUG(log_category::SOAP)
        << "Content-Type 不是 SOAP 媒体类型：\"" << req.content_type()
        << "\"，回 415";
    send_fault(resp,
               uvcpp_soap_make_fault(uvcpp_soap_version::V1_1,
                                     uvcpp_soap_fault_code::CLIENT,
                                     k_reason_bad_ct),
               415, dump_);
    return;
  }

  // 装配失败是**我方的**问题（挑不到 service/port），不是请求的问题。
  if (!valid_) {
    // 构造函数已经记过一次启动期的那条；这里再记一次是因为**每次请求**都在
    // 提醒同一件事，而装配失败的服务今天是"每个请求静默回 500"。
    UVCPP_LOG_ERROR(log_category::SOAP)
        << "端点不可用却收到了请求（" << why_ << "）：" << req.method_name()
        << ' ' << req.path();
    send_fault(resp,
               uvcpp_soap_make_fault(hinted, uvcpp_soap_fault_code::SERVER,
                                     k_reason_invalid_endpoint),
               fault_status(uvcpp_soap_fault_code::SERVER, hinted), dump_);
    return;
  }

  // ---- 2. 解析 ----
  // 解析失败**全部**算 `Sender`：上限是本层的策略没错，但触发它的是对端那份东西。
  uvcpp_soap_message msg;
  const wsdl_status st = uvcpp_soap_parse(req.body_data(), req.body_size(), msg,
                                          limits_, nullptr);
  if (st != wsdl_status::OK) {
    UVCPP_LOG_WARN(log_category::SOAP)
        << "SOAP 报文解析失败：" << wsdl_status_name(st)
        << "（body " << req.body_size() << " 字节）";
    send_fault(resp,
               uvcpp_soap_make_fault(hinted, uvcpp_soap_fault_code::CLIENT,
                                     std::string(k_reason_bad_envelope) +
                                         wsdl_status_name(st)),
               fault_status(uvcpp_soap_fault_code::CLIENT, hinted), dump_);
    return;
  }

  // ---- 3. 版本策略 ----
  if (!accepts(policy_, msg.version)) {
    const char* r = policy_ == uvcpp_soap_version_policy::V1_1_ONLY
                        ? "this endpoint accepts SOAP 1.1 only"
                        : "this endpoint accepts SOAP 1.2 only";
    send_fault(resp,
               uvcpp_soap_make_fault(msg.version,
                                     uvcpp_soap_fault_code::VERSION_MISMATCH, r),
               fault_status(uvcpp_soap_fault_code::VERSION_MISMATCH, msg.version),
               dump_);
    return;
  }

  // ---- 4. mustUnderstand ----
  // 顺序在 Body 之前：规范里这是"初步处理"（1.2 §5.4）里的一步，而 Body 是后一步。
  // 本层一个头都不理解，所以"数出来大于 0"就是"有不认识的头"。
  if (msg.header_must_understand > 0) {
    send_fault(resp,
               uvcpp_soap_make_fault(
                   msg.version, uvcpp_soap_fault_code::MUST_UNDERSTAND,
                   std::string(k_reason_must_understand) +
                       std::to_string(msg.header_must_understand)),
               fault_status(uvcpp_soap_fault_code::MUST_UNDERSTAND, msg.version),
               dump_);
    return;
  }

  // ---- 5. 请求本身是一条 Fault ----
  // 解析层把它当**成功**（对端在报错，那不是解析失败），但派发不了：Fault 不是调用。
  if (msg.has_fault) {
    send_fault(resp,
               uvcpp_soap_make_fault(msg.version, uvcpp_soap_fault_code::CLIENT,
                                     k_reason_request_is_fault),
               fault_status(uvcpp_soap_fault_code::CLIENT, msg.version), dump_);
    return;
  }

  // ---- 6. 派发 ----
  const std::string key = msg.body_qname();
  const std::map<std::string, size_t>::const_iterator found = index_.find(key);
  if (found == index_.end()) {
    // WARN：这个端点只由 WSDL 决定它认哪些 operation，所以走到这里要么是调用方
    // 拼错了名字，要么是有人在拿别的服务的报文探我们。两种都值得被看见。
    UVCPP_LOG_WARN(log_category::SOAP)
        << "Body 的 QName 不是本端点的 operation：\"" << key
        << "\"（本端点认 " << index_.size() << " 个）";
    send_fault(resp,
               uvcpp_soap_make_fault(msg.version, uvcpp_soap_fault_code::CLIENT,
                                     std::string(k_reason_no_such_op) + key),
               fault_status(uvcpp_soap_fault_code::CLIENT, msg.version), dump_);
    return;
  }
  const op_entry& e = ops_[found->second];

  // 在 WSDL 里、但没注册处理函数 —— 缺的是**我方**的东西（`Receiver`）。
  // 这就是为什么"没有这个 operation"与"这个 operation 没实现"必须分开报：
  // 前者是调用方拼错了名字，后者是我们没上线。
  const std::map<std::string, operation_fn>::const_iterator h =
      handlers_.find(e.name);
  if (h == handlers_.end()) {
    // WARN 而不是 ERR：**告警指向的是服务方**——WSDL 里声明了这个 operation，
    // 代码里却没注册处理函数。这是部署漏了一半，不是运行时故障。
    UVCPP_LOG_WARN(log_category::SOAP)
        << "WSDL 里声明了 operation \"" << e.name
        << "\" 但没有注册处理函数，回 Server Fault";
    send_fault(resp,
               uvcpp_soap_make_fault(
                   msg.version, uvcpp_soap_fault_code::SERVER,
                   std::string(k_reason_not_implemented) + e.name),
               fault_status(uvcpp_soap_fault_code::SERVER, msg.version), dump_);
    return;
  }

  // ---- 7. 动作值交叉核对 ----
  bool action_present = false;
  const std::string action = action_of(req, msg.version, &action_present);
  const std::string declared = uvcpp_soap_normalize_action(e.soap_action);
  if (!declared.empty() && action_present && action != declared) {
    send_fault(resp,
               uvcpp_soap_make_fault(msg.version, uvcpp_soap_fault_code::CLIENT,
                                     std::string(k_reason_action_mismatch) +
                                         declared + ", got: " + action),
               fault_status(uvcpp_soap_fault_code::CLIENT, msg.version), dump_);
    return;
  }

  // ---- 8. 交给处理函数 ----
  uvcpp_soap_call call;
  call.version = msg.version;
  call.operation = e.name;
  call.action = action;
  call.action_present = action_present;
  call.header_xml = msg.header_xml;
  call.header_entries = msg.header_entries;
  call.body_local = msg.body_local;
  call.body_ns = msg.body_ns;
  call.body_xml = msg.body_xml;

  uvcpp_soap_reply reply;
  h->second(call, reply);

  // ---- 9. 三种结局 ----
  if (reply.kind == uvcpp_soap_reply::kind_t::FAULT) {
    const uvcpp_soap_fault f = reply.fault_for(msg.version);
    send_fault(resp, f, fault_status(semantic_code_of(f), msg.version), dump_);
    return;
  }
  if (reply.kind == uvcpp_soap_reply::kind_t::NONE) {
    // 单向 operation 的"什么都没给"是**正常**的（规范里单向就是没有响应）；
    // 请求-响应型才是我方漏了东西。
    if (e.has_output) {
      send_fault(resp,
                 uvcpp_soap_make_fault(msg.version, uvcpp_soap_fault_code::SERVER,
                                       k_reason_no_response),
                 fault_status(uvcpp_soap_fault_code::SERVER, msg.version), dump_);
      return;
    }
    send_accepted(resp);
    return;
  }

  // kind == RESPONSE
  if (!e.has_output) {
    // 单向：处理函数的响应**忽略**掉（规范里这条报文不存在）。它要是想报告失败，
    // 那条路是 Fault —— 上面那一支已经处理过了。
    send_accepted(resp);
    return;
  }

  const std::string local = reply.named ? reply.local : e.out_wrapper.local;
  const std::string ns = reply.named ? reply.ns : e.out_wrapper.space;
  resp.status(200).body(
      uvcpp_soap_dump_response(msg.version, local, ns, reply.inner, dump_),
      uvcpp_soap_content_type(msg.version));
}

// =========================================================================
// 装路由
// =========================================================================

void uvcpp_soap_serve(uvcpp_web_app& app, const std::string& path,
                      std::shared_ptr<uvcpp_soap_service> ep) {
  if (!ep) {
    // 装配期的错（空指针），不该到循环线程上变成一次解引用崩溃。
    // 配上一条日志：否则这里只是安静地装一条恒返 500 的路由，而调用方
    // 从 `uvcpp_soap_serve` 的签名上完全看不出自己传了个空指针。
    UVCPP_LOG_ERROR(log_category::SOAP)
        << "uvcpp_soap_serve(\"" << path
        << "\") 收到空端点，已改为注册一条恒回 500 的路由";
    app.post(path, [](uvcpp_web_request&, uvcpp_web_response& resp,
                      uvcpp_web_next next) {
      (void)next;
      resp.status(500).body(std::string("uvcpp_soap_serve: null endpoint"),
                            "text/plain; charset=utf-8");
      resp.end();
    });
    return;
  }
  // 按值捕获共享指针：注册完调用方手上那个可以放掉，端点仍活着（`uvcpp_wsdl_serve`
  // 同一个形状）。operation 表是**共享的**，所以这之后 `operation()` 仍生效 ——
  // 但要在 `start()` 之前注册完（见头文件）。
  app.post(path, [ep](uvcpp_web_request& req, uvcpp_web_response& resp,
                      uvcpp_web_next next) {
    (void)next;
    ep->handle(req, resp);
    resp.end();
  });
}

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
