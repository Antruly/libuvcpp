/**
 * @file src/wsdl/uvcpp_soap_message.h
 * @brief SOAP 信封：解析 `Envelope/Header/Body`、`Fault`，以及**把它写回去**。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层只管**信封**，不管 WSDL
 * ------------------------------
 * 一条 SOAP 报文有两半：**信封**（Envelope/Header/Body/Fault —— 协议层）与
 * **它装的东西**（Body 里那个元素 —— 应用层）。这个头只做前半：
 *
 *   * 认出这是 1.1 还是 1.2（而**不是**按 `Content-Type` 认，见下）；
 *   * 把 `Header` 整段、`Body` 里**第一个元素**整段原样交出来；
 *   * 认 `Fault`（两个版本的字段名完全不同，见表）；
 *   * 把响应信封与 Fault 写出来。
 *
 * "Body 里那个元素的名字怎么对上 WSDL 的 operation、什么时候回 Fault、
 * 单向 operation 回什么状态码"是 `uvcpp_soap_service`（7b 的第二个件）的事。
 * 分成两个文件是因为**这一半不需要 WSDL 文档模型** —— 想手写一个 SOAP 端点
 * 的人可以只用这一层。
 *
 * ## 版本是**从信封自己的命名空间**认出来的，不是从 Content-Type
 *
 * 一条 1.1 请求的 `Content-Type` 是 `text/xml`，1.2 是 `application/soap+xml`，
 * 看着能当判据 —— 但 `text/xml` 也是**任何** XML 的 content-type，所以它
 * 证明不了"这是 SOAP 1.1"。反过来，一条 1.2 报文里那串
 * `xmlns:soap="http://www.w3.org/2003/05/soap-envelope"` 是**报文自己说的**，
 * 没有任何歧义。所以：
 *
 *   * 解析用**命名空间**（`uvcpp_soap_parse`）；
 *   * `Content-Type` 只在入口处做"这个请求归不归我管"的**准入**判断
 *     （`uvcpp_soap_version_of_content_type`），不参与版本判定。
 *
 * ## 两个版本的差别（都是**规范里的**，不是实现选择）
 *
 * | 位置                 | 1.1                                            | 1.2                                        |
 * |----------------------|------------------------------------------------|--------------------------------------------|
 * | 信封命名空间         | `http://schemas.xmlsoap.org/soap/envelope/`    | `http://www.w3.org/2003/05/soap-envelope`  |
 * | HTTP content-type    | `text/xml`                                      | `application/soap+xml`                     |
 * | 动作头               | HTTP 头 `SOAPAction`（可以是 `""`）             | `Content-Type` 的 `action="…"` 参数        |
 * | Fault 的码           | `faultcode`（**不带命名空间**的子元素，值是 QName）| `Code/Value`（在信封命名空间里）        |
 * | Fault 的说明         | `faultstring`                                   | `Reason/Text`（带 `xml:lang`）             |
 * | Fault 的细节         | `detail`                                        | `Detail`                                   |
 * | `Client` / `Server`  | `Client` / `Server`                             | **`Sender` / `Receiver`**（改名了）        |
 * | `DataEncodingUnknown`| **没有这个码**                                   | 有                                          |
 *
 * ★ **`Client`→`Sender` 这一栏是本层最容易被写错的地方**：把 1.1 的 `Client`
 * 原样发进一份 1.2 的 Fault 里，是一个**良构但没人认得**的码。所以本层不接受
 * "直接给一串码"，而是要 `uvcpp_soap_fault_code` 这个**语义**枚举，由
 * `uvcpp_soap_fault_code_local()` 按版本翻成正确的本地名；1.1 里**不存在**的码
 * （`DataEncodingUnknown`）翻出来是**空串**，而不是硬凑一个。
 *
 * ## `mustUnderstand`：本层**数出来**，由上层决定怎么办
 *
 * SOAP 1.1 §4.2.3 / 1.2 §5.2.3 要求：收到一个 `mustUnderstand="1"` 的头而
 * 自己**不认识**它，必须回一个 `MustUnderstand` Fault。而"认不认识"只有应用
 * 知道 —— 本层一个 `Header` 条目都不理解。所以这里只**数**
 * （`header_must_understand()`），判断留给上层；`uvcpp_soap_service` 按规范
 * 回 Fault（见那个头）。`"1"` 与 `"true"` 两种写法都算（1.2 里它是 `xsd:boolean`）。
 *
 * ## `Fault` 的元素名大小写是**故意**不一致的
 *
 * 1.1 的四个子元素是**小写**（`faultcode`/`faultstring`/`faultactor`/`detail`），
 * 1.2 的是**大写**（`Code`/`Reason`/`Node`/`Role`/`Detail`）。写坏这一处不会报错，
 * 只会发一份对端解不出来的报文。
 *
 * ## 存的是**原样文本**，与 `types_xml` 同一个取舍
 *
 * `header_xml` / `body_xml` / `detail` 存的都是**整段元素**（含自己的标签与
 * 自己的 `xmlns` 声明）。拆掉外层标签就会把那一层声明一起丢掉 ——
 * `<tns:Add xmlns:tns="urn:calc">` 里那个 `tns` 是它自己声明的。
 *
 * ★ 但"原样"到这里是**语义级**的，不是字节级：XML 后端的解析标志里**没有**
 * `pugi::parse_ws_pcdata`，所以**元素之间的纯空白文本节点根本不进树** ——
 * `<a>\n  <b/>\n</a>` 取出来的整段是 `<a><b/></a>`，换行与缩进都没了。
 * 混合内容里**带非空白字符**的文本节点不受影响（那是 pcdata，照常进树）。
 * 对 SOAP 不构成问题：1.1 §4.3 / 1.2 §5.1 都把 `Body` 定义为只含元素
 * （element-only content），元素之间的空白按 XML 规范无关紧要。
 * 要**字节级**转发片段得自己开那个标志（本层没开）。
 *
 * ## 序列化的排版规则：**第一行由装它的那层定位，第 2 行起是绝对的**
 *
 * 缩进两空格（可配）、行尾 LF、结尾一个换行，这三条是确定的。嵌套的东西按上面
 * 那条规则接进来：
 *
 *   * 调用方给的**片段**（`header_entries_xml` / `body_entries_xml` / 响应元素的
 *     内容 / `fault.detail`）**一个字节都不动**，只在最前面补一个当前层的缩进
 *     —— 也就是只定位它的第一行，后面几行的排版由写片段的人自己决定；
 *   * 本层**自己生成的块**（响应包装元素、`Fault` 元素）第一行不带缩进，第 2 行
 *     起按绝对层写 —— 所以它们直接塞进 `Body` 就是对的。
 *
 * 为什么第 2 行起不许再加工：片段里行首的空白可能是**文本节点的内容**
 * （`<a>第一行\n  第二行</a>` 里那个换行与空格都是内容），动它就改了语义。
 * 代价是**多行片段的第 2 行起会贴左边** —— 那由写片段的人负责，不由本层猜。
 */

#pragma once
#ifndef SRC_WSDL_UVCPP_SOAP_MESSAGE_H
#define SRC_WSDL_UVCPP_SOAP_MESSAGE_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WSDL_ENABLE

#include <cstddef>
#include <string>

#include <wsdl/uvcpp_wsdl_document.h>

namespace uvcpp {

// =========================================================================
// 版本与常量
// =========================================================================

/** @brief SOAP 的两代。本模块**两个都支持**，并且不假定只有一个。 */
enum class uvcpp_soap_version : int {
  V1_1 = 0,
  V1_2 = 1
};

/** @return `"1.1"` / `"1.2"`。 */
const char* uvcpp_soap_version_name(uvcpp_soap_version v);

/**
 * @brief 信封元素的命名空间 URI。
 *
 * 与 `wsdl_ns::soap11_envelope()` / `soap12_envelope()` 是**同一对常量**
 * —— 那两个在 7a 就放进去了，理由也写在那边（不让同一串字面量出现两次）。
 */
const char* uvcpp_soap_envelope_ns(uvcpp_soap_version v);

/** @return `"text/xml; charset=utf-8"`（1.1）/ `"application/soap+xml; charset=utf-8"`（1.2）。 */
const char* uvcpp_soap_content_type(uvcpp_soap_version v);

/**
 * @brief 这个 `Content-Type` 归不归本层管？是的话给出**它指的是哪个版本**。
 *
 * 两种都认：`text/xml`（1.1）与 `application/soap+xml`（1.2）。参数
 * （`; charset=…` / `; action="…"`）不参与判断，`Type/Subtype` 之外的都被忽略。
 * 大小写不敏感（HTTP 的 Type/Subtype 是大小写不敏感的）。
 *
 * @return 不是 SOAP 的时候返回 false，`out` 不动。
 * @note 这里给出来的版本**只用于**"该回一个哪一版的 Fault"这种准入级判断；
 *       报文真正的版本一律以 `uvcpp_soap_parse()` 解析出来的为准。
 */
bool uvcpp_soap_version_of_content_type(const std::string& ct,
                                        uvcpp_soap_version& out);

/**
 * @brief 取 `Content-Type` 的 `action="…"` 参数（SOAP 1.2 的动作）。
 *
 * 也认不带引号的写法（`action=urn:x`）—— 那是**非法**的 HTTP 参数语法，
 * 但真实客户端会写。找不到返回空串。
 */
std::string uvcpp_soap_action_of_content_type(const std::string& ct);

/**
 * @brief 归一一个动作值：剥掉两端的空白与**一对**包着的 `"`。
 *
 * 1.1 的 `SOAPAction` 头按规范是带引号的（`SOAPAction: "urn:calc#Add"`），
 * 而 1.2 的 `action` 参数不带 —— 两处都要归一后再比，否则永远比不相等。
 * `SOAPAction: ""` 归一成空串（**这是个有意义的值**：规范里它表示"看报文体
 * 自己，别用动作头"，所以它**不**算"和 WSDL 里的 soapAction 不一致"）。
 */
std::string uvcpp_soap_normalize_action(const std::string& raw);

// =========================================================================
// Fault
// =========================================================================

/**
 * @brief Fault 的**语义**码 —— 按版本翻成正确的本地名（见文件头那张表）。
 *
 * 这五个是 SOAP 自己定义的；应用自定义的码不走这里（把本地名直接填进
 * `uvcpp_soap_fault::code`）。
 */
enum class uvcpp_soap_fault_code : int {
  /** 报文本身有问题（1.1 `Client` / 1.2 `Sender`）。 */
  CLIENT = 0,
  /** 服务端处理失败（1.1 `Server` / 1.2 `Receiver`）。 */
  SERVER = 1,
  /** 有一个不认识的 `mustUnderstand` 头。 */
  MUST_UNDERSTAND = 2,
  /** 信封命名空间不是本层认识的那两个之一 —— **本层不会自己发这个码**（见下）。 */
  VERSION_MISMATCH = 3,
  /** 1.1 **没有**这个码，翻出来是空串。 */
  DATA_ENCODING_UNKNOWN = 4
};

/**
 * @return 这个语义码在该版本里的**本地名**；该版本里不存在时返回**空串**。
 * @note 空串是个**有意义的返回值**，不是失败：`DATA_ENCODING_UNKNOWN` 在
 *       1.1 里根本不存在，硬凑一个名字发出去只会让对端把它当成应用自定义码。
 */
const char* uvcpp_soap_fault_code_local(uvcpp_soap_fault_code c,
                                        uvcpp_soap_version v);

/**
 * @brief 一条 Fault。字段与**报文里的位置**一一对应，两个版本共用一份结构。
 *
 * 哪些字段在哪个版本里有值，见文件头那张表：1.1 只填 `code`/`reason`/`role`/
 * `detail`，1.2 还会填 `subcode`/`lang`/`node`。`code_space` 是解析时读到的
 * 那个 URI（1.1 的 `faultcode` 常写成 `soap:Client`，此时 `code_space` 就是
 * 信封命名空间）；由本层**生成**时它总是信封命名空间。
 */
struct uvcpp_soap_fault {
  uvcpp_soap_version version = uvcpp_soap_version::V1_1;
  std::string code;        ///< 本地名（`Client` / `Sender` / …）
  std::string code_space;  ///< `code` 的命名空间 URI（可空）
  std::string subcode;     ///< 1.2 `Code/Subcode/Value`
  std::string subcode_space;
  std::string reason;      ///< 1.1 `faultstring` / 1.2 `Reason/Text`
  std::string lang;        ///< 1.2 `Reason/Text/@xml:lang`（可空）
  std::string node;        ///< 1.2 `Node`
  std::string role;        ///< 1.1 `faultactor` / 1.2 `Role`
  /** `detail` / `Detail` 元素**整段**的原样 XML（没有就是空串）。 */
  std::string detail;

  bool empty() const {
    return code.empty() && reason.empty() && node.empty() && role.empty() &&
           detail.empty();
  }
};

/** @brief 按语义码造一条 Fault（`version` 一起定下来）。 */
uvcpp_soap_fault uvcpp_soap_make_fault(uvcpp_soap_version v,
                                       uvcpp_soap_fault_code code,
                                       const std::string& reason,
                                       const std::string& detail_inner_xml =
                                           std::string());

// =========================================================================
// 信封
// =========================================================================

/**
 * @brief 解析出来的一个 SOAP 信封。
 *
 * 字段是**公开**的（与 `uvcpp_wsdl_document` 同一个取舍）：它是数据，不是有
 * 不变量的句柄。
 */
struct uvcpp_soap_message {
  uvcpp_soap_version version = uvcpp_soap_version::V1_1;

  /** `Header` 元素**整段**的原样 XML；没有 `Header` 时是空串。 */
  std::string header_xml;
  /** `Header` 的元素子节点数。 */
  size_t header_entries = 0;
  /** `Header` 里 `mustUnderstand` 为真的条目数（见文件头）。 */
  size_t header_must_understand = 0;

  /** `Body` 里**第一个**元素子节点的局部名 / 命名空间 URI / 整段原样 XML。 */
  std::string body_local;
  std::string body_ns;
  std::string body_xml;
  /** `Body` 的元素子节点数（规范要求恰好 1，多于 1 是 `SOAP_TOO_MANY_BODY_ELEMENTS`）。 */
  size_t body_children = 0;

  bool has_fault = false;
  uvcpp_soap_fault fault;

  /** @return `Body` 第一个子元素的 QName 文本 `{ns}local`（没有 Body 时是 `{}`）。 */
  std::string body_qname() const { return "{" + body_ns + "}" + body_local; }

  /** @return Body 里恰好一个元素、且它不是 `Fault`（即"这是一次调用"）。 */
  bool is_call() const {
    return body_children == 1 && !has_fault && !body_local.empty();
  }

  void clear() { *this = uvcpp_soap_message(); }
};

/**
 * @brief 解析一条 SOAP 报文。
 *
 * 顺序：空 -> `EMPTY`；超字节数 -> `TOO_LARGE`；不是良构 XML -> `SYNTAX`；
 * 超深度/节点数 -> `TOO_DEEP` / `TOO_MANY_NODES`；根不是本层认识的
 * `{信封命名空间}Envelope` -> `SOAP_NOT_ENVELOPE`；没有 `Body` 元素或它没有
 * 元素子节点 -> `SOAP_NO_BODY`；多于一个 -> `SOAP_TOO_MANY_BODY_ELEMENTS`；
 * 唯一那个是 `Fault` -> **成功**，`has_fault` 为真（对端在报错，那不是解析
 * 失败）、`fault` 填好；但那个 `Fault` **自己没有码**（1.1 的 `faultcode` /
 * 1.2 的 `Code` 都没有）时是 `SOAP_BAD_FAULT` —— 规范两个版本都要求它必须有，
 * 而一条没有码的 Fault 转发出去只会把对面的问题变成我们的。
 *
 * `Header` / `Body` 的子元素条数超过 `lim.max_items` 时是 `TOO_MANY_ITEMS`。
 *
 * @param why 非空时接收一句**指向静态串**的说明（不接收时不写）。
 */
wsdl_status uvcpp_soap_parse(const char* data, size_t len,
                             uvcpp_soap_message& out,
                             const uvcpp_wsdl_limits& lim = uvcpp_wsdl_limits(),
                             const char** why = nullptr);

/** @copydoc uvcpp_soap_parse(const char*, size_t, uvcpp_soap_message&, const uvcpp_wsdl_limits&, const char**) */
wsdl_status uvcpp_soap_parse(const std::string& xml, uvcpp_soap_message& out,
                             const uvcpp_wsdl_limits& lim = uvcpp_wsdl_limits(),
                             const char** why = nullptr);

// =========================================================================
// 序列化
// =========================================================================

/** @brief 信封序列化的选项。目前只有前缀，将来要缩进宽度也是加在这里。 */
struct uvcpp_soap_dump_options {
  /** 信封命名空间用的前缀。默认 `"soap"`。 */
  std::string env_prefix;
  /** 缩进宽度（空格数）。默认 2。 */
  size_t indent;

  explicit uvcpp_soap_dump_options(const std::string& prefix = "soap",
                                   size_t width = 2u)
      : env_prefix(prefix), indent(width) {}
};

/**
 * @brief 只组信封：`Header` 与 `Body` 的**内容**由调用方给。
 *
 * `header_entries_xml` 为空时不发 `Header`（SOAP 允许没有 Header）。
 * `body_entries_xml` 按原样嵌进 `Body`（只把第一行按当前层缩进，见文件头）。
 *
 * 产出是**确定的**：同一个入参永远产出逐字节相同的文本。
 */
std::string uvcpp_soap_dump_envelope(uvcpp_soap_version v,
                                     const std::string& header_entries_xml,
                                     const std::string& body_entries_xml,
                                     const uvcpp_soap_dump_options& opt =
                                         uvcpp_soap_dump_options());

/**
 * @brief 一次调用的响应：`<op_local xmlns="op_ns">` 包住 `inner_xml` 再进 Body。
 *
 * `op_ns` 为空时不发 `xmlns`（"响应元素没有命名空间"是**合法**的 ——
 * 一个 document/literal 的服务完全可以不声明它）。
 *
 * **不做** rpc 风格那套 `part` 包装：包装元素里放什么由 `inner_xml` 决定，
 * 因为那要按 `message` 的 `part` 表来组，而本层不认识 WSDL。
 */
std::string uvcpp_soap_dump_response(uvcpp_soap_version v,
                                     const std::string& op_local,
                                     const std::string& op_ns,
                                     const std::string& inner_xml,
                                     const uvcpp_soap_dump_options& opt =
                                         uvcpp_soap_dump_options());

/** @brief 一条 Fault 的**整个信封**（Fault 进 Body）。 */
std::string uvcpp_soap_dump_fault(const uvcpp_soap_fault& f,
                                  const uvcpp_soap_dump_options& opt =
                                      uvcpp_soap_dump_options());

/**
 * @brief Fault 元素**本身**（不含信封）—— 用例与"只想看一眼"的调用点用得到。
 *
 * 缩进以**自己**为第 0 层（第一行不缩、子元素缩一层）。这与
 * `uvcpp_soap_dump_fault()` 里那一份**缩进不同**：那边它是信封的第 2 层，
 * 子元素按绝对层缩。判据要逐字节比整封时用前者，比元素本身时用这个。
 */
std::string uvcpp_soap_dump_fault_element(const uvcpp_soap_fault& f,
                                          const std::string& env_prefix);

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
#endif  // SRC_WSDL_UVCPP_SOAP_MESSAGE_H
