/**
 * @file src/wsdl/uvcpp_wsdl_document.h
 * @brief WSDL 1.1 文档模型：解析、查询，以及**不需要 pugixml** 的重新序列化。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一档做什么
 * ------------
 * `definitions` / `import` / `types` / `message` / `portType`(+`operation`) /
 * `binding`(+`soap:binding`、`soap:operation`、`soap:body`、`soap:address`) /
 * `service`(+`port`)，外加 `targetNamespace` 与 QName 解析。**SOAP envelope、
 * `soap:Fault`、按 operation 名派发**是下一档（7b），不在这个头里 —— 但
 * `binding` 里那几样派发要用的东西（`soapAction`、`style`、`use`、1.1/1.2 之分）
 * 已经收在这里，因为它们是**文档的属性**，不是运行时的属性。
 *
 * 三条规矩（与 `src/webapp/uvcpp_web_json.h` 同形）
 * ------------------------------------------------
 * 1. **不抛异常。** 每个入口都返回状态码。唯一还能逃出去的是 `std::bad_alloc`
 *    —— 与 nlohmann 那条暴露面相同（pugixml 的解析本身也用返回值，只在分配
 *    失败时抛）。
 * 2. **解析前有上限。** 字节数、嵌套深度、元素总数、每一类元素的条数，全是
 *    "超过就拒"，不是"截断"。上限在**建模型之前**生效（见私有层的 `xml_measure`）。
 * 3. **后端隔离。** `<pugixml.hpp>` 只出现在 `src/wsdl/uvcpp_wsdl_pugixml.h`
 *    这一个**不装出去**的头里。这个头里一个 pugi 类型都没有，所以使用者既不
 *    需要它的头、也不需要它的库。
 *
 * ## 命名空间按 (URI, 局部名) 判，不按字面前缀
 *
 * `soap:` / `s:` / `SOAP:` 三种前缀写法都认，**默认命名空间下的 WSDL** 也认
 * （`<definitions xmlns="http://schemas.xmlsoap.org/wsdl/">` 是合法且常见的写法）。
 * 这不是宽容，是唯一正确的做法：前缀只是个**局部别名**，跨文档没有任何意义。
 * 机制在私有层（pugixml 1.14 **没有公开的命名空间解析 API**，`node.name()`
 * 返回的是带前缀的限定名，所以前缀到 URI 的解析由我们自己维护一个作用域栈）。
 *
 * ## `types_xml` 存的是**整个 `<types>` 元素**，不是它的内容
 *
 * 理由：原文档里那个 `<types>` 元素**可能自带 `xmlns` 声明**
 * （`<types xmlns:xs="http://www.w3.org/2001/XMLSchema">` 很常见）。拆掉外层标签
 * 就把它自己的声明一起丢了，重新发出去的文档里那些前缀就没人定义。所以这里
 * 原样存整段。要**自己生成**一份 XSD，就写 `"<types>…</types>"` 整段（内层用哪
 * 个前缀由你定，但要保证它在根上能解析到 —— 见下面 dump 的命名空间策略）。
 *
 * ## `uvcpp_wsdl_dump` 的命名空间策略
 *
 * * `tns` **保留**给 `targetNamespace`（这是 WSDL 文档里的惯例写法）；
 * * 原文档根上的全部 `xmlns*` **照搬**到输出根上 —— 因为原样存进来的 `types`
 *   片段可能正用着它们（`<wsdl:types>` 就要求根上有 `xmlns:wsdl`）；
 * * 模型里出现的、上面两类都没覆盖到的 URI，依次分配 `ns1`、`ns2`…。
 *
 * ## 说清楚 dump **不是**字节级往返
 *
 * 它输出的是**模型投影**：模型不认识的属性、注释、处理指令、以及 `wsdl:import`
 * 指到的**别的文档的内容**都不会出现。判据因此是**语义往返**（parse → dump →
 * parse，两次的模型相等），不是"照着原文件 diff 为空"。别把它当格式化器用。
 */

#pragma once
#ifndef SRC_WSDL_UVCPP_WSDL_DOCUMENT_H
#define SRC_WSDL_UVCPP_WSDL_DOCUMENT_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WSDL_ENABLE

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace uvcpp {

// =========================================================================
// 命名空间 URI 常量
// =========================================================================
//
// 写成函数而不是 `extern const char* const` 的全局量：后者在**动态库**里是
// 一条跨模块的符号引用，Windows 上还要走 import 表；而这些都是**编译期字面量**，
// 没有理由让它们有地址。调用点在比较时也不需要取地址（见 `ns_is`）。

/** @brief 本模块认得的那些命名空间 URI。 */
namespace wsdl_ns {

/** `wsdl:` —— WSDL 1.1 的元素命名空间。 */
inline const char* wsdl() { return "http://schemas.xmlsoap.org/wsdl/"; }
/** `soap:` —— WSDL 1.1 的 SOAP 绑定扩展。 */
inline const char* soap_binding() {
  return "http://schemas.xmlsoap.org/wsdl/soap/";
}
/** `soap12:` —— WSDL 1.1 的 SOAP 1.2 绑定扩展。 */
inline const char* soap12_binding() {
  return "http://schemas.xmlsoap.org/wsdl/soap12/";
}
/** `xsd:` —— XML Schema。 */
inline const char* xsd() { return "http://www.w3.org/2001/XMLSchema"; }
/** SOAP 1.1 信封（7b 用；这里先给出常量，免得两处各写一遍字面量）。 */
inline const char* soap11_envelope() {
  return "http://schemas.xmlsoap.org/soap/envelope/";
}
/** SOAP 1.2 信封（7b 用）。 */
inline const char* soap12_envelope() {
  return "http://www.w3.org/2003/05/soap-envelope";
}
/** WSDL 2.0 的元素命名空间 —— **只用来把"这是 2.0"认出来**，不支持。 */
inline const char* wsdl20() { return "http://www.w3.org/ns/wsdl"; }

}  // namespace wsdl_ns

// =========================================================================
// 状态码
// =========================================================================

/** @brief 解析结果。取值与 `json_status` / `xml_result` **故意**分开。 */
enum class wsdl_status : int {
  OK = 0,
  /** 输入为空（长度为 0）。与"语法错"分开，调用方才能区分 400 与 415。 */
  EMPTY,
  /** 不是良构的 XML。 */
  SYNTAX,
  /** 超过输入字节上限。 */
  TOO_LARGE,
  /** 超过嵌套深度上限。 */
  TOO_DEEP,
  /** 超过元素总数上限。 */
  TOO_MANY_NODES,
  /** 超过"每一类元素的条数"上限（`max_items`）。 */
  TOO_MANY_ITEMS,
  /** 良构，但根元素不是 `{wsdl}definitions`（也不是 2.0 的 description）。 */
  NOT_WSDL,
  /** 根是 `{http://www.w3.org/ns/wsdl}description`：这是 WSDL **2.0**，本模块不支持。 */
  UNSUPPORTED_VERSION,
  /** 缺 `targetNamespace`（WSDL 1.1 §3.1 要求它存在）。 */
  NO_TARGET_NAMESPACE,
  // ---- 以下是 SOAP 那一半（`uvcpp_soap_message.h`）用的 ----
  //
  // 加在**尾部**：枚举值是按位置编码的，插在中间会改掉已有值的数值。
  /** 根元素不是 SOAP 信封（也不是本模块认识的任何一个版本的信封）。 */
  SOAP_NOT_ENVELOPE,
  /** 没有 `Body` 元素，或 `Body` 里一个元素子节点都没有。 */
  SOAP_NO_BODY,
  /** `Body` 里的元素子节点多于一个（两个版本都只允许一个）。 */
  SOAP_TOO_MANY_BODY_ELEMENTS,
  /** `Body` 里是一个**连码都没有**的 `Fault`（两个版本都要求它必须有）。 */
  SOAP_BAD_FAULT
};

/** @brief 给使用者看的一句话（判据里直接印它，比印数字好查）。 */
const char* wsdl_status_name(wsdl_status s);

// =========================================================================
// 模型
// =========================================================================

/**
 * @brief XML QName：`{命名空间 URI}局部名`。
 *
 * `space` 为空 = **没有命名空间**（不是"默认命名空间"—— 解析时默认命名空间
 * 已经被解析成具体的 URI 填进来了）。
 */
struct uvcpp_qname {
  std::string space;
  std::string local;

  uvcpp_qname() {}
  uvcpp_qname(const std::string& s, const std::string& l)
      : space(s), local(l) {}

  bool empty() const { return space.empty() && local.empty(); }

  /** @return `{uri}local`；没有命名空间时是 `{}local`。 */
  std::string str() const { return "{" + space + "}" + local; }

  bool operator==(const uvcpp_qname& o) const {
    return space == o.space && local == o.local;
  }
  bool operator!=(const uvcpp_qname& o) const { return !(*this == o); }
};

/**
 * @brief 取一个 QName **文本**的局部名。
 *
 * 依次剥掉两种写法：`{http://x}Foo`（`str()` 的形状）与 `tns:Foo`（XML 里的
 * 形状）。`find_*` 用它，所以传 `"Add"`、`"tns:Add"`、`"{…}Add"` 都查得到。
 */
std::string uvcpp_qname_local(const std::string& text);

/** @brief `wsdl:part`。`element` 与 `type` 至多有一个非空（规范如此）。 */
struct uvcpp_wsdl_part {
  std::string name;
  uvcpp_qname element;  ///< `@element`（一个**元素**声明）
  uvcpp_qname type;     ///< `@type`（一个**类型**）
  std::string documentation;
};

/** @brief `wsdl:message`。 */
struct uvcpp_wsdl_message {
  std::string name;
  std::vector<uvcpp_wsdl_part> parts;
  std::string documentation;

  const uvcpp_wsdl_part* find_part(const std::string& name) const;
};

/** @brief `portType/operation` 里 `input` / `output` / `fault` 的一个引用。 */
struct uvcpp_wsdl_op_ref {
  std::string name;     ///< `@name`（可空；WSDL 允许匿名）
  uvcpp_qname message;  ///< `@message`
  std::string documentation;
};

/** @brief `wsdl:portType/wsdl:operation`。 */
struct uvcpp_wsdl_operation {
  std::string name;
  /**
   * `input` / `output` **在不在**（而不是"名字是不是空"）。
   *
   * 这条必须单独记：**单向（one-way）operation 合法地没有 `output`**，而
   * "`output` 在、`@message` 忘了写"是另一回事（后者是坏文档）。只看
   * `output.message.empty()` 区分不出这两者，于是要么把单向操作误判成坏文档，
   * 要么把坏文档放过去（SOAP 派发要按这个决定回不回响应）。
   */
  bool has_input = false;
  bool has_output = false;
  uvcpp_wsdl_op_ref input;
  uvcpp_wsdl_op_ref output;
  std::vector<uvcpp_wsdl_op_ref> faults;
  /** `@parameterOrder`（按空白切好的；没写就是空）。 */
  std::vector<std::string> parameter_order;
  std::string documentation;
};

/** @brief `wsdl:portType`。 */
struct uvcpp_wsdl_port_type {
  std::string name;
  std::vector<uvcpp_wsdl_operation> operations;
  std::string documentation;

  const uvcpp_wsdl_operation* find_operation(const std::string& name) const;
};

/**
 * @brief `soap:binding` / `soap12:binding` 扩展。
 *
 * 1.1 与 1.2 的差别在**命名空间**上，元素名一样，所以这一份结构两边共用，
 * 用 `is_soap12` 区分。`transport` 只有 1.1 写（1.2 的 `soap:binding` 不带它）。
 */
struct uvcpp_wsdl_soap_binding {
  bool present = false;
  bool is_soap12 = false;
  std::string style;      ///< `"document"` / `"rpc"`（空 = 没写）
  std::string transport;  ///< 1.1 常见值 `http://schemas.xmlsoap.org/soap/http`
};

/** @brief `wsdl:binding/wsdl:operation`，含它那一对 `soap:operation`/`soap:body`。 */
struct uvcpp_wsdl_binding_operation {
  std::string name;
  std::string soap_action;  ///< `soap:operation/@soapAction`（1.1）；1.2 里可空
  std::string style;        ///< `soap:operation/@style`（空 = 继承 binding 的）
  bool is_soap12 = false;

  std::string input_use;             ///< `input/soap:body/@use`：`literal` / `encoded`
  std::string input_encoding_style;  ///< `@encodingStyle`（`encoded` 时才有意义）
  std::string input_namespace;       ///< `@namespace`（`rpc` 时才有意义）
  /** `input/soap:body/@parts`（按空白切好）。空 = 没写 = **message 的全部 part
   *  都进 body**（这是 1.1 的默认，不是"一个都不进"）。派发/组包要按它挑 part。 */
  std::vector<std::string> input_body_parts;
  std::string output_use;
  std::string output_encoding_style;
  std::string output_namespace;
  std::vector<std::string> output_body_parts;
  std::string documentation;

  /** @return `style` 为空时回落到 binding 的 `style`。 */
  std::string effective_style(const std::string& binding_style) const {
    return style.empty() ? binding_style : style;
  }
};

/** @brief `wsdl:binding`。 */
struct uvcpp_wsdl_binding {
  std::string name;
  uvcpp_qname port_type;  ///< `@type`
  uvcpp_wsdl_soap_binding soap;
  std::vector<uvcpp_wsdl_binding_operation> operations;
  std::string documentation;

  const uvcpp_wsdl_binding_operation* find_operation(
      const std::string& name) const;
};

/** @brief `wsdl:service/wsdl:port`。 */
struct uvcpp_wsdl_port {
  std::string name;
  uvcpp_qname binding;  ///< `@binding`
  std::string address;  ///< `soap:address` / `soap12:address` 的 `@location`
  bool is_soap12 = false;
  std::string documentation;
};

/** @brief `wsdl:service`。 */
struct uvcpp_wsdl_service {
  std::string name;
  std::vector<uvcpp_wsdl_port> ports;
  std::string documentation;

  const uvcpp_wsdl_port* find_port(const std::string& name) const;
};

/**
 * @brief `wsdl:import` —— **只记账，不取回**。
 *
 * 跨文档取回（以及随之而来的循环 import）超出本模块范围。解析时不因为
 * "引用指到别的文档去了"而拒（那种严法会把合法文档拒掉），要判引用是否闭合
 * 就显式调 `uvcpp_wsdl_check_references`，并知道它**只查本文档内**。
 */
struct uvcpp_wsdl_import {
  std::string namespace_uri;  ///< `@namespace`
  std::string location;       ///< `@location`
};

// =========================================================================
// 上限
// =========================================================================

/** @brief 一份 WSDL 的解析上限。四个数都是"超过就拒"，不是"截断"。 */
struct uvcpp_wsdl_limits {
  /** 输入字节数上限，**解析之前**判。默认 4 MiB（一份 WSDL 通常几 KB）。 */
  size_t max_bytes;
  /** 元素嵌套深度上限。默认 64。 */
  size_t max_depth;
  /** 元素总数上限。默认 65536。 */
  size_t max_nodes;
  /** **每一类**元素（message / portType / binding / service / operation / part /
   *  port …）各自的条数上限。默认 4096。 */
  size_t max_items;

  explicit uvcpp_wsdl_limits(size_t bytes = 4u * 1024u * 1024u, size_t depth = 64,
                             size_t nodes = 65536u, size_t items = 4096u)
      : max_bytes(bytes), max_depth(depth), max_nodes(nodes), max_items(items) {}
};

// =========================================================================
// 文档
// =========================================================================

/**
 * @brief 一份 WSDL 1.1 文档的模型。
 *
 * 字段是**公开**的：它是个数据模型，不是有不变量的句柄 —— 包装一层
 * getter/setter 只会让"按模型生成 WSDL"那条路（见 `uvcpp_wsdl_dump`）难写。
 * `find_*` 那组只提供"按键查一条"，不提供修改。
 */
struct uvcpp_wsdl_document {
  std::string target_namespace;
  std::string name;              ///< `definitions/@name`（可选）
  std::string documentation;     ///< `definitions` 下第一个 `wsdl:documentation`
  /** `<types>` **整个元素**的原样文本（见文件头）。没有就是空串。 */
  std::string types_xml;
  /** 原文档**根元素上**的 `xmlns*` 声明（前缀, URI），按出现顺序。 */
  std::vector<std::pair<std::string, std::string> > root_namespaces;
  /** dump 之后重新 parse 回来的字节数记账用；解析时填的是**输入**长度。 */
  size_t source_bytes = 0;

  std::vector<uvcpp_wsdl_import> imports;
  std::vector<uvcpp_wsdl_message> messages;
  std::vector<uvcpp_wsdl_port_type> port_types;
  std::vector<uvcpp_wsdl_binding> bindings;
  std::vector<uvcpp_wsdl_service> services;

  void clear() { *this = uvcpp_wsdl_document(); }

  // 查：`ref` 可以是局部名（`"Add"`）或 QName 文本（`"tns:Add"` / `"{…}Add"`）。
  // 局部名那一档按名字查（不挑命名空间）；`{uri}` 那一档要求 uri 逐字节等于
  // targetNamespace；`prefix:` 那一档**用文档根上的 `xmlns*` 声明表把前缀翻成
  // URI 再比** —— 所以文档自己声明的 `tns:Add` 查得到，而一个没声明过（或指向
  // 别处）的前缀查不到。三种写法的判据在 `tests/functional/wsdl_document_func.cpp`
  // 第 1.10-1.12 条。
  const uvcpp_wsdl_message* find_message(const std::string& ref) const;
  const uvcpp_wsdl_port_type* find_port_type(const std::string& ref) const;
  const uvcpp_wsdl_binding* find_binding(const std::string& ref) const;
  const uvcpp_wsdl_service* find_service(const std::string& ref) const;
};

// =========================================================================
// 解析 / 序列化
// =========================================================================

/**
 * @brief 解析一份 WSDL。
 *
 * 顺序：空 -> `EMPTY`；超字节数 -> `TOO_LARGE`；不是良构 XML -> `SYNTAX`；
 * 超深度/元素数 -> `TOO_DEEP` / `TOO_MANY_NODES`；根不是 `{wsdl}definitions`
 * -> `NOT_WSDL`（若是 WSDL 2.0 的 `{...wsdl}description` 则 `UNSUPPORTED_VERSION`）；
 * 缺 `targetNamespace` -> `NO_TARGET_NAMESPACE`。
 *
 * **不做引用闭合检查**（见 `uvcpp_wsdl_check_references`）。模型里认不出的元素
 * 一律跳过 —— WSDL 允许扩展元素，为它们报错会把合法文档拒掉。
 *
 * @param why 非空时接收一句**指向静态缓冲或常量串**的说明（不接收时不写）。
 *            返回 `SYNTAX`/`TOO_*` 时那句说明来自 XML 层。
 */
wsdl_status uvcpp_wsdl_parse(const char* data, size_t len,
                             uvcpp_wsdl_document& out,
                             const uvcpp_wsdl_limits& lim = uvcpp_wsdl_limits(),
                             const char** why = nullptr);

/** @copydoc uvcpp_wsdl_parse(const char*, size_t, uvcpp_wsdl_document&, const uvcpp_wsdl_limits&, const char**) */
wsdl_status uvcpp_wsdl_parse(const std::string& xml, uvcpp_wsdl_document& out,
                             const uvcpp_wsdl_limits& lim = uvcpp_wsdl_limits(),
                             const char** why = nullptr);

/**
 * @brief 把模型序列化成一份 WSDL 1.1 文档。**不需要 pugixml**（纯字符串拼接）。
 *
 * 命名空间策略与"不是字节级往返"这两条写在文件头。产出是**确定性的**：同一个
 * 模型永远产出逐字节相同的文本（缩进固定两空格、行尾 LF、结尾一个换行）。
 */
std::string uvcpp_wsdl_dump(const uvcpp_wsdl_document& doc);

/** @copydoc uvcpp_wsdl_dump */
void uvcpp_wsdl_dump_to(const uvcpp_wsdl_document& doc, std::string& out);

/**
 * @brief 检查**文档内**的引用是否闭合。
 *
 * 查四件事：`binding/@type` 指得到 portType、`service/port/@binding` 指得到
 * binding、`portType/operation` 的 `input/output/fault/@message` 指得到 message、
 * 以及每个 `binding/operation/@name` 在它引用的 portType 里存在（SOAP 派发要按
 * 这个名字查，缺了就是运行时才发现）。
 *
 * **反方向不查**：不要求 binding 覆盖 portType 的全部 operation（真实 WSDL 里
 * 有只绑一部分的）。有 `wsdl:import` 时本判据依然只查本文档内 —— 如果你知道
 * 引用指向 import 进来的文档，就别调它，或者说别拿它的失败当"文档坏了"。
 *
 * @param why 失败时接收一句说明（静态串）。
 */
bool uvcpp_wsdl_check_references(const uvcpp_wsdl_document& doc,
                                std::string* why);

// =========================================================================
// XML 转义（dump 与 7b 的响应序列化共用）
// =========================================================================

/** @brief 文本节点转义：`&` `<` `>`（引号在文本里不必转，但转了无害）。 */
std::string uvcpp_xml_escape_text(const std::string& s);

/** @brief 属性值转义：在上面基础上再加 `"`。 */
std::string uvcpp_xml_escape_attr(const std::string& s);

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
#endif  // SRC_WSDL_UVCPP_WSDL_DOCUMENT_H
