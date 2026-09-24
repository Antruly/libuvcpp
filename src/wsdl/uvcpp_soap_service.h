/**
 * @file src/wsdl/uvcpp_soap_service.h
 * @brief SOAP 的路由半边：Body 元素对上 WSDL 的 operation、该拒的拒、该回的码回对。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `uvcpp_soap_message.h` 的分工
 * --------------------------------
 * 那个头只认**信封**（Envelope/Header/Body/Fault），**不认识 WSDL** —— 想手写一个
 * SOAP 端点的人可以只用它。这个头是另一半：拿一份 WSDL 文档，把 Body 里第一个元素
 * 派给一个 C++ 处理函数，并按规范决定"回什么码、发不发响应"。所以它依赖
 * `uvcpp_wsdl_document.h`。
 *
 * 派发键是 **Body 第一个元素的 QName**，键的形状从 WSDL 里推出来
 * ---------------------------------------------------------------
 * 规范不给"怎么从报文找到 operation"的算法，给的是"binding 声明了什么"：
 * `binding/operation/input/soap:body` 说这一路的正文长什么样。本层按那个推：
 *
 * | 情形                                          | 派发键                                  |
 * |-----------------------------------------------|-----------------------------------------|
 * | input message 的**第一个 part 声明了 `@element`** | 那个 `@element` 的 QName（如 `{urn:calc}Add`）|
 * | 其余（`rpc`，或 part 只声明了 `@type`，或没有 part） | `{input/soap:body/@namespace}` + operation 名的 QName |
 *
 * 一句话：**WSDL 说了 `@element` 就按它，没说就按 `{namespace}操作名`**。这张表只说
 * **派发键**（= 请求这一侧）；**响应包装元素不是它的简单对称**，见下面《响应包装元素
 * 从哪来》。
 * `document` 那一档只取**第一个** part，因为信封层交出来的就是 Body 的**第一个**
 * 元素；多 part 的 document 服务是合法的（1.1 §4.3 的"至多一个元素"只是建议），
 * 多的那几个留给处理函数自己从 `body_xml` 里取。
 *
 * ★ **`rpc` 的命名空间错一个字，派发就不发生。** 键是 QName，命名空间是键的一部分：
 * `{urn:calc}Add` 与 `{urn:other}Add` 是两个不同的键。这就是"rpc 的
 * `soap:body/@namespace` 必须与报文一致"那条规范要求的落地形式 —— 它**不是**一个
 * 单独的检查，而是键本身的形状。于是名字对、命名空间不对，落到的是"没有这个
 * operation"那条 Fault（对一个只认 `urn:calc` 的端点来说，`urn:other` 的 `Add`
 * 确实就是一个不存在的 operation）。
 *
 * 拒绝的九种情形，分属两方
 * -------------------------
 * SOAP 1.2 把 Fault 分成 `Sender`（你的报文/你的请求有问题）与 `Receiver`（我这边
 * 处理不了）。分错的后果是**告警指向错的人**：
 *
 * | 情形                                          | 码（1.1 / 1.2）        | 谁的问题 |
 * |-----------------------------------------------|------------------------|----------|
 * | `Content-Type` 不是 SOAP 的两种之一            | `Client` / `Sender`    | 调用方   |
 * | 报文不可用（空/不是良构/超限/没有 Body/多个/坏 Fault） | `Client` / `Sender`  | 调用方   |
 * | 请求正文本身是一条 `Fault`                      | `Client` / `Sender`    | 调用方   |
 * | 版本策略不收这一版（见 `set_version_policy`）   | `VersionMismatch`      | 调用方   |
 * | 有 `mustUnderstand` 头（见下）                  | `MustUnderstand`       | 调用方   |
 * | 动作值与 WSDL 不一致（见下）                    | `Client` / `Sender`    | 调用方   |
 * | Body 元素的 QName 不是本端点的任何 operation    | `Client` / `Sender`    | 调用方   |
 * | operation 在 WSDL 里、但**没注册处理函数**      | `Server` / `Receiver`  | 服务方   |
 * | 处理函数什么都没给（且这不是单向 operation）    | `Server` / `Receiver`  | 服务方   |
 *
 * 一句话：**报文错是 `Sender`，我方缺东西是 `Receiver`**。解析失败**全部**算
 * `Sender` —— 包括"超过本层的字节/深度上限"那几种：上限是本层的**策略**没错，但触发
 * 它的是对端发来的那份东西；把它算成 `Receiver`，"有人一直在发超大报文"看起来就
 * 成了服务端故障。
 *
 * ## HTTP 状态码：1.1 一律 500，1.2 按 4xx / 5xx 分
 *
 *   * **1.1 只定义了 500**（§6.2 的每个例子都是 500），所以 1.1 下的 Fault 一律发
 *     `500`。**唯一例外是准入失败**：`Content-Type` 不是 SOAP 的两种之一时发
 *     `415 Unsupported Media Type` —— 那是 HTTP 层的判断（"这个媒体类型我不处理"），
 *     用 500 说它等于把"你发错东西了"报成"服务器坏了"。1.2 的正文明确允许 4xx 用于
 *     `Sender` 类错误；1.1 的正文没提，但 415 在这里是有共识的用法。
 *   * **1.2 分两张码**：`Sender` 一族（`Sender` / `MustUnderstand` /
 *     `VersionMismatch`）→ `400`；`Receiver` → `500`。
 *
 * 正常响应一律 `200`；单向 operation 是 `202`（见下）。
 *
 * ## `mustUnderstand`：本层**不理解任何头**，于是"只数不判"变成了"有一条就拒"
 *
 * `uvcpp_soap_message.h` 只**数** `mustUnderstand` 条目（它一个头都不理解，判不了）。
 * 这一层的判断是：**它的处理函数也一个头都不理解** —— 头是给应用看的东西，而本层
 * 交给处理函数的只是一段 `header_xml` 文本。所以这里定成：**数出来大于 0 就回
 * `MustUnderstand` Fault**，reason 里带上条数（不是只报"有"）。
 *
 * ★ 这是一条**已知边界**，不是疏漏。要支持"我认识某个头"，得让处理函数能按**条目**
 * 声明它认识哪个，而今天 `uvcpp_soap_message` 只给整段 `header_xml` 与两个计数，
 * 没有逐条的 QName 视图。在那之前，正确的做法是**不用这一层**：自己在路由里调
 * `uvcpp_soap_parse()`，看完 `header_xml` 再调 `uvcpp_soap_dump_response()` /
 * `uvcpp_soap_dump_fault()` 写那一段 —— 那两个 API 都在 `uvcpp_soap_message.h` 里，
 * 与 WSDL 无关。这也是为什么这一层与那一层分成两个文件。
 *
 * ## 动作值（action）：**提供了且非空**才交叉核对
 *
 * 1.1 的动作在 HTTP 头 `SOAPAction`（规范里**带引号**：`SOAPAction: "urn:calc#Add"`），
 * 1.2 的在 `Content-Type` 的 `action="…"` 参数里。两个版本一套规则：
 *
 *   * WSDL 里这个 `binding/operation/soap:operation/@soapAction` **非空**，且请求提供
 *     了**非空**的动作值 ⇒ 归一后必须相等，否则 `Sender` Fault；
 *   * 请求的动作值是**空串**（`SOAPAction: ""`）⇒ **不核对**。规范说空串的意思是
 *     "看报文体自己"（1.1 §6.1.1），那正是本层的派发依据；
 *   * 请求**根本没提供** ⇒ **不核对**。1.1 要求 HTTP 请求必须带 `SOAPAction`，但真实
 *     客户端会漏；键是 Body 元素，漏了照样派得动，为它回 Fault 只会把能用的调用拒掉。
 *
 * 归一（`uvcpp_soap_normalize_action`）剥空白与**一对**引号 —— 不做这一步两者永远比
 * 不相等（1.1 带引号、1.2 不带）。
 *
 * ## 单向（one-way）operation：`202` + 空正文
 *
 * WSDL 里 `portType/operation` **没有 `output`** 就是单向（模型里记的是 `has_output`
 * 这个"在不在"的布尔，不是"名字是不是空"）。这类调用：
 *
 *   * 处理函数**照常跑**（活还是要干的）；
 *   * 处理函数给了响应 ⇒ 忽略，回 `202 Accepted` + 空正文（规范里单向就是没有响应）；
 *   * 处理函数**什么都没给** ⇒ 也回 `202`。这里**不**套用"没给响应就是 `Receiver`
 *     Fault"那条：单向 operation 的"什么都没给"是**正常**的；
 *   * 处理函数给了 **Fault** ⇒ **发那条 Fault**（带正常的 Fault 状态码）。单向调用没有
 *     别的方式告诉调用方"失败了"，把它吞掉是最坏的选择。
 *
 * ## 响应包装元素从哪来
 *
 * 前半条与派发键同形：output 那一边第一个 part 有 `@element` 就按它（`AddResponse`）。
 * **后半条不同** —— 没有 `@element` 时是 `{output/soap:body/@namespace}` +
 * **操作名 + `Response`**，不是操作名。
 *
 * ★ 为什么这里**必须**与派发键分叉：`document/literal` 的 output part 声明了
 * `@element`，走不到兜底；会走到兜底的是 `rpc/literal` —— 它的 part 只有 `@type`。
 * 而在 rpc 里**包装元素是协议的一部分**：请求是 `<Add>`、响应是 `<AddResponse>`，
 * 两边同名就等于让对端拿着同一个 QName 分不出手上那一段是请求还是响应（与
 * `soap:body/@namespace` 一起，这两个名字是 rpc 报文里唯一能表明方向的东西）。
 * 取操作名的话不是"少一个后缀"，是**响应不可解析**。
 *
 * 处理函数给的是包装元素**里面的东西**（`inner_xml`），逐字节嵌进去（只按当前层补一个
 * 首行缩进，规则与 `uvcpp_soap_dump_response` 一致）。要自己指定包装元素用
 * `uvcpp_soap_reply::set_response_named()`。
 *
 * ★ 本层**不校验正文内容**：没有 schema，也没有"从 WSDL 的 `types` 编译出反序列化器"
 * 这件事（那要一整套 XML Schema）。处理函数拿到的是**整段原样 XML**，取参数用
 * `uvcpp_soap_call::arg()` / `child_xml()`。内容的对错由处理函数自己判 —— 那不是本层
 * 能判的。
 *
 * ## 这一层**不做**的几件事（都是刻意的）
 *
 *   * **不回响应 `Header`**。要发 WS-Security / WS-Addressing 那类头得有对应语义，
 *     不在本轮范围；
 *   * **不做 `types` 到 C++ 的映射**（见上）；
 *   * **不做 MTOM/附件**（`xsd:base64Binary` 会当普通文本把整段搬进内存）；
 *   * **不自动发 WSDL**。GET 那条路是 `uvcpp_wsdl_serve()`（`uvcpp_wsdl_serve.h`）：
 *     两行装配、两套判据，分得开。
 */

#pragma once
#ifndef SRC_WSDL_UVCPP_SOAP_SERVICE_H
#define SRC_WSDL_UVCPP_SOAP_SERVICE_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WSDL_ENABLE

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_response.h>

#include <wsdl/uvcpp_soap_message.h>
#include <wsdl/uvcpp_wsdl_document.h>

namespace uvcpp {

// =========================================================================
// 版本策略
// =========================================================================

/**
 * @brief 这个端点收哪些版本的 SOAP 信封。
 *
 * 这是 `VersionMismatch` 这个码**唯一**用得到的地方：本模块两个版本都实现，默认
 * （`ANY`）就没有"版本不对"这回事；只有显式只收一版时，另一版才成为一个错误。
 * 它是个真实存在的部署形状 —— 老客户端只发 1.1，服务方愿意明确地拒掉 1.2 而不是
 * 半支持。
 */
enum class uvcpp_soap_version_policy : int {
  /** 两个版本都收（默认）。 */
  ANY = 0,
  /** 只收 1.1；1.2 的信封回 `VersionMismatch`。 */
  V1_1_ONLY,
  /** 只收 1.2；1.1 的信封回 `VersionMismatch`。 */
  V1_2_ONLY
};

/** @return `"any"` / `"1.1-only"` / `"1.2-only"`。 */
const char* uvcpp_soap_version_policy_name(uvcpp_soap_version_policy p);

// =========================================================================
// 一次调用（交给处理函数的那些东西）
// =========================================================================

/**
 * @brief 一次派发出去的调用。字段是**公开**的（与 `uvcpp_wsdl_document` 同一个取舍：
 *        它是数据，不是有不变量的句柄）。
 *
 * 这里**没有** HTTP 请求的指针：要 peer、路径、任意头，就在你自己的路由 lambda 里
 * 先取走再调处理函数。本层只把 SOAP 相关的东西搬过来。
 */
struct uvcpp_soap_call {
  uvcpp_soap_version version = uvcpp_soap_version::V1_1;

  /** WSDL 里的 operation 名（**局部名**，派发的落点）。 */
  std::string operation;

  /**
   * 归一后的动作值（`uvcpp_soap_normalize_action` 的输出）。可能是空串。
   * @note 1.1 读的是 HTTP 头 `SOAPAction`，1.2 读的是 `Content-Type` 的 `action=`
   *       参数 —— **按信封解析出来的版本决定读哪一个**（与"版本从信封认"是同一条
   *       规则，`Content-Type` 只提供准入与那个参数）。
   */
  std::string action;

  /**
   * 提供了**非空**的动作值吗？
   *
   * 它是"要不要与 WSDL 交叉核对"的开关（见文件头）：`SOAPAction: ""` 与"根本没带
   * 这个头"在这一栏**都是假**。前者规范里的意思是"看报文体自己"（那正是本层的派发
   * 依据），后者是客户端漏了；两种都不该拒，所以这里不区分它们 —— 只区分"有没有一个
   * **非空**的值可比"。
   */
  bool action_present = false;

  /** `Header` 整段原样 XML（没有 Header 时是空串）。 */
  std::string header_xml;
  /** `Header` 的元素子节点数。 */
  size_t header_entries = 0;

  /** `Body` 里第一个元素：局部名 / 命名空间 URI / 整段原样 XML。 */
  std::string body_local;
  std::string body_ns;
  std::string body_xml;

  /** @return `{ns}local` —— 与派发键同一个格式。 */
  std::string body_qname() const { return "{" + body_ns + "}" + body_local; }

  /**
   * @brief 取 `Body` 元素里**第一个**局部名为 `local` 的直接子元素的文本。
   *
   * **只按局部名比，不比命名空间**：参数的元素名声明在 WSDL 的 `types` 里，而本层
   * 看不懂 XSD，没有一个权威命名空间可以比。要比命名空间就在处理函数里用
   * `child_xml()` 拿到整段自己看。
   *
   * @note 返回空串有**两种**可能：没有这个子元素，或者有但文本是空的。要区分就
   *       用 `child_xml()`（它空 = 确实没有这个子元素）。
   * @note 每次调用都重新解析一遍 `body_xml`（片段很小，且这样 `uvcpp_soap_call` 里
   *       不用藏一个 XML 文档对象）。要取很多个参数又在意开销，就自己解析一次。
   */
  std::string arg(const std::string& local) const;

  /** @brief 取 `Body` 元素里第一个局部名为 `local` 的直接子元素的**整段**原样 XML。 */
  std::string child_xml(const std::string& local) const;
};

// =========================================================================
// 一次回复（处理函数要填的东西）
// =========================================================================

/**
 * @brief 处理函数的产出。三种结局：什么都不给 / 给一个响应 / 给一条 Fault。
 *
 * 字段是公开的，但正常用法是那四个 `set_*`。
 */
struct uvcpp_soap_reply {
  enum class kind_t : int {
    /** 什么都没给。请求-响应型算 `Receiver` Fault；单向 operation 算正常（202）。 */
    NONE = 0,
    /** 一个正常响应。 */
    RESPONSE,
    /** 一条 Fault。 */
    FAULT
  };

  kind_t kind = kind_t::NONE;

  // ---- kind == RESPONSE ----
  /** 包装元素局部名。`named` 为假时这一对由 WSDL 推（见文件头）。 */
  std::string local;
  std::string ns;
  /** 调用方是否显式指定了包装元素（`set_response_named`）。 */
  bool named = false;
  /** 包装元素**里面**的东西，原样嵌入。 */
  std::string inner;

  // ---- kind == FAULT ----
  uvcpp_soap_fault_code code = uvcpp_soap_fault_code::SERVER;
  std::string reason;
  std::string detail_inner;
  /** `set_fault(const uvcpp_soap_fault&)` 那条路整条带走的东西。 */
  uvcpp_soap_fault full;
  bool use_full = false;

  /** @brief 正常响应：`inner_xml` 进由 WSDL 推出来的包装元素。 */
  void set_response(const std::string& inner_xml);

  /**
   * @brief 正常响应，但包装元素自己指定。
   * @param ns 空串 = 不写 `xmlns`（"响应元素没有命名空间"是合法的）。
   */
  void set_response_named(const std::string& local, const std::string& ns,
                          const std::string& inner_xml);

  /**
   * @brief 按语义码报 Fault。
   *
   * **不给版本** —— 这时候还不知道要发 1.1 还是 1.2（那是信封层解析出来的），所以
   * 码先存**语义**，由 `fault_for()` 按版本翻成本地名。这也正是
   * `uvcpp_soap_fault_code` 存在的理由（见 `uvcpp_soap_message.h`）。
   *
   * @param detail_inner_xml 进 `detail`/`Detail` 的**内容**（整段元素由本层包）。
   * @note 该版本里**不存在**的码（今天只有一种：1.1 的 `DATA_ENCODING_UNKNOWN`）
   *       会落到 `CLIENT` 上。`uvcpp_soap_message.h` 的判据里写着"保证这种 Fault
   *       不会发出去是这一层的责任" —— 空码写出来是 `soap:`，一个非法 QName，
   *       对端连码都读不到。理由那句话原样保留，只是换一个能解开的码。
   */
  void set_fault(uvcpp_soap_fault_code code, const std::string& reason,
                 const std::string& detail_inner_xml = std::string());

  /** @brief 整条 Fault 自己端上来（想设 `role`/`node`/`lang`/自定义码时用）。 */
  void set_fault(const uvcpp_soap_fault& f);

  /**
   * @brief 把上面那条 Fault 按版本补全成一条完整的 `uvcpp_soap_fault`。
   *
   * `handle()` 自己调它；处理函数一般不用。公开是因为"判据要能直接看那条 Fault"。
   */
  uvcpp_soap_fault fault_for(uvcpp_soap_version v) const;
};

// =========================================================================
// 端点
// =========================================================================

/**
 * @brief 一个 SOAP 端点：一份 WSDL 模型 + 一个 operation 表 + 一套派发规则。
 *
 * **不可拷贝**，用 `shared_ptr` 持有（`uvcpp_soap_serve()` 就收这个）—— 它与
 * `uvcpp_wsdl_source` 是同一个形状：装配期造、之后只读、被路由按值捕获。
 *
 * operation 表**就是共享的那张**（不是注册时快照）：`uvcpp_soap_serve()` 之后
 * `operation()` 仍会生效。**但要在 `start()` 之前注册完** —— 请求处理期间从别的
 * 线程改它是一次数据竞争，这条与 `uvcpp_web_app::use()` 那条"中间件请在 start()
 * 之前注册完"是同一类要求。
 *
 * ★ **同一个派发键出现两次**（坏文档：两条 binding operation 推出的 QName 一样）时
 * **最后一条赢**，另一条永远收不到调用。这条是**静默**的，所以装配期该用
 * `operation_names()` 与 `dispatch_target_of()` 自查一遍。
 */
class uvcpp_soap_service {
 public:
  /**
   * @brief 处理函数：一次调用 -> 一次回复。
   *
   * 契约：**不许让异常穿出去**（与全库一致 —— 它跑在 libuv 的循环线程上）；
   * 不许阻塞（该走 `uvcpp_fs` / `uvcpp_work` 的活自己往那边派）。
   */
  typedef std::function<void(uvcpp_soap_call&, uvcpp_soap_reply&)> operation_fn;

  /**
   * @brief 从一份 WSDL 里挑一个 port，把它底下 binding 的 operation 收进来。
   *
   * @param service `service/@name` 的**局部名**；空串 = "就一份 service 时用那份"。
   * @param port `port/@name` 的局部名；空串 = "那个 service 下只有一个 SOAP port 时
   *             用那个"。
   *
   * 挑不到时对象仍然构造得出来，但 `valid()` 是假、`handle()` 一律回
   * `Server` Fault，`why()` 说明原因（**静态串**）。判据与启动日志都该看它。
   */
  uvcpp_soap_service(const uvcpp_wsdl_document& doc,
                     const std::string& service = std::string(),
                     const std::string& port = std::string());

  /** @return 这个端点是可用的（挑到了 service/port，且它有 SOAP binding）。 */
  bool valid() const { return valid_; }

  /** @return 不可用时为什么（静态串）；可用时是 `"ok"`。 */
  const char* why() const;

  const std::string& service_name() const { return service_name_; }
  const std::string& port_name() const { return port_name_; }
  const std::string& binding_name() const { return binding_name_; }
  const std::string& port_type_name() const { return port_type_name_; }
  /** @return 文档的 `targetNamespace`（`uvcpp_wsdl_document` 那份）。 */
  const std::string& target_namespace() const { return doc_.target_namespace; }

  /** @return binding 里声明的 operation 条数（= 这个端点能被派发到的全部名字）。 */
  size_t operation_count() const { return ops_.size(); }

  /** @return 能被派发到的 operation 名，按 WSDL 里的顺序。 */
  std::vector<std::string> operation_names() const;

  /**
   * @brief 这个名字在 WSDL 里、能被派发到吗？
   *
   * `operation()` 注册一个 WSDL 里没有的名字**不会报错**（文档可能只是不全），
   * 但它永远不会被调用 —— 装配期用这个自查。
   */
  bool has_operation(const std::string& name) const;

  /**
   * @brief 注册一个处理函数。
   *
   * 重复注册同一个名字：**后注册的赢**（与路由表"先注册的赢"相反 —— 这里是同一个
   * 端点上的替换，替换语义才符合直觉）。
   *
   * @note 名字是 WSDL 里的 operation **局部名**。
   */
  uvcpp_soap_service& operation(const std::string& name,
                                const operation_fn& fn);

  uvcpp_soap_service& set_version_policy(uvcpp_soap_version_policy p);

  /** @brief 信封的解析上限（`max_items` 用在 Header/Body 的子元素条数上）。 */
  uvcpp_soap_service& set_limits(const uvcpp_wsdl_limits& lim);

  /** @brief 序列化选项（信封前缀、缩进宽度）。 */
  uvcpp_soap_service& set_dump_options(const uvcpp_soap_dump_options& opt);

  /**
   * @brief 这条 `{ns}local` 派给哪个 operation？没有就返回空串。
   *
   * 键的格式与 `uvcpp_soap_call::body_qname()` 完全一样。装配期自查 / 判据 / 日志用。
   */
  std::string dispatch_target_of(const std::string& body_qname) const;

  /**
   * @brief 处理一个请求：准入、解析、核对、派发、序列化、定状态码。
   *
   * 装了 `uvcpp_soap_serve()` 的路由就是直接调它。**不抛异常**（内部全走状态码）。
   * 响应会被自己 `end()` 掉吗？—— 不会：本函数只管写好 `resp`，`end()` 由路由那层
   * （`uvcpp_soap_serve` 装的 lambda）调。
   */
  void handle(uvcpp_web_request& req, uvcpp_web_response& resp) const;

  uvcpp_soap_service(const uvcpp_soap_service&) = delete;
  uvcpp_soap_service& operator=(const uvcpp_soap_service&) = delete;

 private:
  /** binding 里一条 operation，连同它推出来的那两个 QName。 */
  struct op_entry {
    std::string name;        ///< portType 里的 operation 名（局部名）
    uvcpp_qname in_wrapper;  ///< 派发键
    uvcpp_qname out_wrapper; ///< 响应包装元素
    std::string soap_action; ///< `soap:operation/@soapAction`（可能为空）
    bool has_output = false; ///< 单向还是请求-响应（`portType` 里那个"在不在"）
  };

  /** @brief 构造期的装配：挑 service/port、收 operation、建索引。 */
  void build(const std::string& service, const std::string& port);

  uvcpp_wsdl_document doc_;
  std::string service_name_;
  std::string port_name_;
  std::string binding_name_;
  std::string port_type_name_;
  bool valid_ = false;
  const char* why_ = "";
  uvcpp_soap_version_policy policy_ = uvcpp_soap_version_policy::ANY;
  uvcpp_wsdl_limits limits_;
  uvcpp_soap_dump_options dump_;

  std::vector<op_entry> ops_;
  std::map<std::string, size_t> index_;             ///< 派发键 -> ops_ 下标
  std::map<std::string, operation_fn> handlers_;    ///< operation 名 -> 处理函数
};

/**
 * @brief 在 app 上装一条 POST 路由。
 *
 * @param ep 按值捕获（共享指针），所以注册完你手上那个 `shared_ptr` 可以放掉，
 *           端点仍活着；反过来，`operation()` 之后注册的**看得见**（表是共享的，
 *           见类注释里那条"要在 start() 之前注册完"）。
 *
 * @note 与 `uvcpp_wsdl_serve()` 同一类要求：**同一个路径注册两次是静默无效的**
 *       （路由表打平时先注册的赢）。
 * @note GET 那条路它不管 —— 要 `?wsdl` 就另外调 `uvcpp_wsdl_serve()`。
 */
void uvcpp_soap_serve(uvcpp_web_app& app, const std::string& path,
                      std::shared_ptr<uvcpp_soap_service> ep);

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
#endif  // SRC_WSDL_UVCPP_SOAP_SERVICE_H
