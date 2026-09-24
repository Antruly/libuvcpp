# SOAP 指南（下半：信封、派发与响应）

`uvcpp` 的 WSDL 支持分成上下两半。**这一页只讲下半**：一段报文是不是 SOAP、它是不是
发给我的、它是哪个 operation、我该回什么。上半（WSDL 1.1 的文档模型、按名字查它、
把 WSDL 发出去）在 [`doc/wsdl-guide.md`](wsdl-guide.md)。

开关、依赖、编译期条件、以及"模型长什么样"都在那一页（§2 与 §4）：**两半是同一个
模块**（`UVCPP_ENABLE_WSDL`、同一个 pugixml 后端），下面只重复一句最要紧的
——这一页的代码一样要 `-DUVCPP_ENABLE_WSDL=ON`，一样在 `webapp` 之上。

## 目录

1. [这一半解决什么](#1-这一半解决什么)
2. [编译期条件与包含](#2-编译期条件与包含)
3. [五分钟上手](#3-五分钟上手)
4. [派发键怎么从 WSDL 推](#4-派发键怎么从-wsdl-推)
5. [处理函数看到什么](#5-处理函数看到什么)
6. [处理函数给什么](#6-处理函数给什么)
7. [响应包装元素不是派发键的对称](#7-响应包装元素不是派发键的对称)
8. [拒绝与状态码](#8-拒绝与状态码)
9. [版本从信封来](#9-版本从信封来)
10. [动作值](#10-动作值)
11. [单向 operation](#11-单向-operation)
12. [Fault 的形状](#12-fault-的形状)
13. [上限与安全](#13-上限与安全)
14. [判据](#14-判据)
15. [没做的（如实列出）](#15-没做的如实列出)

---

## 1. 这一半解决什么

信封层管的是"这段 XML 是不是 SOAP"，**它管不了"这段报文是不是发给我的、是不是我要的
那个操作"**。这一半管后者，一共三件事：

| 要做的事 | 用哪个 |
|---|---|
| 把绑定到某个 `service/port` 的 operation 收成一张派发表，按报文派给处理函数 | `uvcpp_soap_service`（§3、§4） |
| 解析 / 生成 SOAP 信封与 `Fault`（含 1.1 与 1.2 的差异） | `uvcpp_soap_parse` / `uvcpp_soap_dump_*`（§9、§12） |
| 把它接到 `webapp` 的一条 POST 路由上 | `uvcpp_soap_serve()`（§3） |

一条设计上的取舍先说出来：**这一半与 WSDL 是强耦合的**，因为规范里"这条报文属于哪个
operation"没有运行时依据，只有文档依据 —— 派发键是从 `binding` 推出来的（§4）。所以
"手搓一个 SOAP 端点、不给 WSDL"这条路在本库里不存在。反过来，只想解析一段信封、
不关心 operation，那就直接用 §9 那几个函数（`uvcpp_soap_message.h` 与 WSDL 无关）。

## 2. 编译期条件与包含

与上半同一批（开关、pugixml、`UVCPP_WSDL_ENABLE`、`webapp` 依赖、C++11），见
[`doc/wsdl-guide.md`](wsdl-guide.md) §2。这里只列这一页用到的两个头：

```cpp
#include <wsdl/uvcpp_soap_message.h>   // 信封、Fault、版本
#include <wsdl/uvcpp_soap_service.h>   // 端点：派发、处理函数、响应、路由
```

两个都**不在** `<uvcpp.h>` 里（那个聚合头只收 `uvcpp_define/version/export` 与
`handle/loop/req` 三组）。`uvcpp_soap_service.h` 自己包含 `uvcpp_soap_message.h`，所以
用端点时只写后者也编得过。

## 3. 五分钟上手

```cpp
#include <wsdl/uvcpp_soap_service.h>
#include <wsdl/uvcpp_wsdl_document.h>

#include <cstdlib>
#include <memory>
#include <string>

// 装配一个端点：先解析 WSDL，再挑一条 service/port，然后按 operation 名挂处理函数。
bool doc_soap_wire(uvcpp::uvcpp_web_app& app, const std::string& wsdl_text) {
  uvcpp::uvcpp_wsdl_document doc;
  if (uvcpp::uvcpp_wsdl_parse(wsdl_text, doc) != uvcpp::wsdl_status::OK) {
    return false;   // 上半的 §9：状态码与 why 都在那
  }

  // ★ 收的是**模型**，不是字节 —— 没有"给它一份 WSDL 文本"的构造方式。
  const std::shared_ptr<uvcpp::uvcpp_soap_service> ep =
      std::make_shared<uvcpp::uvcpp_soap_service>(doc, "CalculatorService",
                                                  "CalculatorPort");
  if (!ep->valid()) return false;   // ep->why() 说为什么（静态串）

  ep->operation("Add", [](uvcpp::uvcpp_soap_call& call,
                          uvcpp::uvcpp_soap_reply& reply) {
    const int a = std::atoi(call.arg("a").c_str());
    const int b = std::atoi(call.arg("b").c_str());
    reply.set_response("<result>" + std::to_string(a + b) + "</result>");
  });

  uvcpp::uvcpp_soap_serve(app, "/calc", ep);   // 一条 POST 路由
  return true;
}
```

那个 `WS` 和 `HTTP` 的两条路各管一半，合起来才是"一个能自描述的 SOAP 端点"：

```cpp
#include <wsdl/uvcpp_soap_service.h>
#include <wsdl/uvcpp_wsdl_serve.h>

#include <memory>
#include <string>

// POST /calc 走 SOAP，GET /calc.wsdl 发 WSDL —— 两个路径、两条路由、一个模型。
void doc_soap_full(uvcpp::uvcpp_web_app& app,
                   const uvcpp::uvcpp_wsdl_document& doc,
                   std::shared_ptr<uvcpp::uvcpp_soap_service> ep) {
  uvcpp::uvcpp_wsdl_serve(app, "/calc.wsdl", doc);   // 上半的 §8
  uvcpp::uvcpp_soap_serve(app, "/calc", ep);         // 这一半
}
```

四条最容易踩的：

- **`uvcpp_soap_service` 收的是文档模型**，构造参数是 `(doc, service, port)` 三个
  —— `service` / `port` 是**局部名**，都可以留空（"只有一份 service" /"那个 service
  下只有一个 SOAP port"时自动选）。挑不到**不抛异常**：对象照样构造得出来，但
  `valid()` 是假，请求进来一律回 `Receiver` Fault，原因在 `why()` 里。
- **`operation()` 的名字是 WSDL 里的 operation 局部名**。注册一个 WSDL 里没有的名字
  不会报错，但它**永远不会被调用** —— 装配期用 `has_operation(name)` 自查一遍。
  同一个名字注册两次是**后注册的赢**（同一个端点上的替换语义）。
- **要在 `app.start()` 之前注册完。** 那条路由按值捕获 `shared_ptr`，所以装完之后你
  手上那个可以放掉；但请求处理期间从别的线程调 `operation()` 是数据竞争。
- **两个 `serve()` 都只装一条路由，同一个路径注册两次是静默无效的**（路由表打平时
  先注册的赢）—— 想"重新装载"得换路径或自己写 handler，理由与上半 §8 那两条同形。

## 4. 派发键怎么从 WSDL 推

派发键是 **Body 第一个元素的 QName**；而"这个 QName 应该长什么样"是从 WSDL 的
input message 推出来的：

| WSDL 里长什么样 | 派发键 |
|---|---|
| input message 的**第一个 part 声明了 `@element`** | 那个 `@element` 的 QName（如 `{urn:calc}Add`） |
| 其余（`rpc/literal`，或 part 只声明了 `@type`，或没有 part） | `{input/soap:body/@namespace}` + operation 名 |

一句话：**WSDL 说了 `@element` 就按它，没说就按 `{命名空间}操作名`**。不按 `style`
分叉 —— 那条规则天然覆盖 `document`、`rpc` 与"part 只声明了 `@type`"，分叉只多一处
能写错的地方。多 part 的 `document` 只取**第一个** part（信封层交出来的就是 Body 的
第一个元素），多的那几个留给处理函数自己从 `body_xml` 里取。

★★ **`rpc` 的命名空间错一个字，派发就不发生。** 键是 QName，命名空间是键的一部分：
`{urn:calc}Add` 与 `{urn:other}Add` 是两个不同的键。这不是一个单独的检查，而是键本身
的形状 —— 于是"名字对、命名空间不对"落到的是**"没有这个 operation"**那条 Fault（对
一个只认 `urn:calc` 的端点来说，`urn:other` 的 `Add` 确实就是一个不存在的 operation，
这个报法是对的）。排查时先看这一条，它是最常见的"服务明明在、就是收不到调用"。

**同一份文档里两条 operation 推出同一个键**（坏文档）时**最后一条赢**，另一条永远收
不到调用，而且这条是**静默**的。装配期自查两句：

```cpp
#include <wsdl/uvcpp_soap_service.h>

#include <string>
#include <vector>

// 装配期自查：把键与名字印出来对一遍（重键与"WSDL 里有、我没注册"都会露出来）。
std::vector<std::string> doc_soap_keys(const uvcpp::uvcpp_soap_service& ep,
                                       const std::vector<std::string>& names) {
  std::vector<std::string> out;
  for (size_t i = 0; i < names.size(); ++i) {
    // has_operation 判"WSDL 里有没有"；dispatch_target_of 判"这个键派给谁"。
    out.push_back(names[i] + (ep.has_operation(names[i]) ? " ok" : " MISSING"));
  }
  return out;
}
```

`operation_names()` 给出**能被派发到的**全部名字（按 WSDL 里的顺序），
`dispatch_target_of("{urn:calc}Add")` 给出"这个键派给哪个 operation"（没有就空串），
两者合起来就是上面那句自查。

## 5. 处理函数看到什么

处理函数的签名是 `void(uvcpp_soap_call&, uvcpp_soap_reply&)`（`operation_fn`）。
`uvcpp_soap_call` 是**只读的本次调用**，字段都是公开的：

| 字段 / 方法 | 是什么 |
|---|---|
| `version` | 信封解析出来的版本（§9） |
| `operation` | 派到的 operation 局部名 |
| `action` / `action_present` | 归一后的动作值 / "有一个**非空**的值吗"（§10） |
| `body_local` / `body_ns` / `body_qname()` | Body 第一个元素的局部名 / 命名空间 / `{ns}local` |
| `body_xml` | Body 第一个元素的**整段**原样 XML |
| `header_xml` / `header_entries` | `Header` 整段原样 XML（没有时是空串）/ 它的元素子节点数 |
| `arg(local)` | Body 元素里第一个局部名为 `local` 的**直接子元素**的文本 |
| `child_xml(local)` | 同上，但给的是那个子元素的**整段** XML |

```cpp
#include <wsdl/uvcpp_soap_service.h>

#include <string>

// 三种取参数的粒度：整段正文 / 一个参数的文本 / 一个参数的整段 XML。
std::string doc_soap_introspect(const uvcpp::uvcpp_soap_call& call) {
  const std::string whole = call.body_xml;
  const std::string a = call.arg("a");
  const std::string a_xml = call.child_xml("a");
  return whole + a + a_xml + call.body_qname();
}
```

三条要知道的：

- **`arg()` 只按局部名比，不比命名空间。** 参数的元素名声明在 WSDL 的 `types` 里，而
  本层看不懂 XSD，没有一个权威命名空间可以比。要比命名空间就用 `child_xml()` 拿整段
  自己看。`arg()` 返回空串有**两种**可能（没有这个子元素 / 有但文本是空的），要区分
  就用 `child_xml()`（空 = 确实没有）。
- **`body_xml` 是"语义级原样"，不是字节级。** XML 层没有开 `parse_ws_pcdata`，所以
  元素之间的**纯空白文本节点根本不进树**：`<a>\n  <b/>\n</a>` 取回来是 `<a><b/></a>`。
  对 SOAP 不构成问题（1.1 §4.3 / 1.2 §5.1 都把 `Body` 定义为只含元素），但别拿它做
  逐字节比对。
- **本层不校验正文内容**：没有 schema，也没有"从 `types` 编译出反序列化器"这件事
  （那要一整套 XML Schema）。处理函数拿到的是整段原样 XML，取参数用上面那两个方法，
  内容的对错由处理函数自己判。

## 6. 处理函数给什么

`uvcpp_soap_reply` 是本次调用的产出，三种结局：

```cpp
#include <wsdl/uvcpp_soap_service.h>

#include <string>

void doc_soap_outcomes(bool failed, bool quiet,
                       uvcpp::uvcpp_soap_reply& reply) {
  if (failed) {
    // 语义码 + reason（发给对端的那句是英文，见 §12）+ detail 的**内容**。
    reply.set_fault(uvcpp::uvcpp_soap_fault_code::SERVER, "backend is down",
                    "<retry>later</retry>");
    return;
  }
  if (quiet) return;   // 什么都不给：请求-响应型 -> Receiver Fault；单向 -> 202
  reply.set_response("<result>3</result>");   // 包装元素从 WSDL 推（§7）
}
```

| 结局 | 结果 |
|---|---|
| `set_response(inner_xml)` | `200` + 正常响应。`inner_xml` 进包装元素**里面** |
| `set_response_named(local, ns, inner_xml)` | 同上，但包装元素自己指定 |
| `set_fault(code, reason, detail_inner)` | 按语义码报 Fault（状态码见 §8） |
| `set_fault(const uvcpp_soap_fault&)` | 整条端上来：想设 `role` / `node` / `lang` / 自定义码时用 |
| **什么都不给** | 请求-响应型 ⇒ `Receiver` Fault（"我这边漏了东西"）；单向 ⇒ `202`（§11） |

两条容易踩的：

- **异常不许穿出去。** 处理函数跑在 libuv 的循环线程上，异常从那里逃出去会穿过 C 写
  的 `uv_run` 栈帧 —— 与全库一致，这是未定义行为。同理**不许阻塞**：该走 `uvcpp_fs` /
  `uvcpp_work` 的活自己往那边派（注意它们跑在别的线程上，回来要 `uvcpp_loop::post`
  之类的机制，见 `webapp` 那两页）。
- **`set_fault` 的第一个参数是语义码，不是本地名。** 这时候还不知道要发 1.1 还是 1.2
  （那是信封层解析出来的），所以码先存语义，由这一层按版本翻成 `Client` / `Sender`
  这类本地名。想直接用本地名（或自定义码）就走 `set_fault(uvcpp_soap_fault)` 那条。
- **语义码在那个版本里不存在时，发出去的是 `CLIENT`，理由那句原样保留。** 今天只有
  一种：1.1 里没有 `DATA_ENCODING_UNKNOWN`。不换的话写上线的是**空码**（`soap:`，
  一个非法 QName，对端连码都读不到）—— "换个能解开的码"比"报一个读不出来的码"好，
  所以这一层替调用方兜了这一步。想精确控制就走 `set_fault(uvcpp_soap_fault)`。

## 7. 响应包装元素不是派发键的对称

前半条与派发键同形：output 那一边第一个 part 有 `@element` 就按它（`AddResponse`）。

**后半条不同**：没有 `@element` 时兜底名是 `{output/soap:body/@namespace}` +
**操作名 + `Response`**，不是操作名。

★★ 为什么这里**必须**与派发键分叉：会走到兜底的是 `rpc/literal` —— 它的 part 只有
`@type`，包装元素**只能**由这条规则造出来。而在 rpc 里包装元素是**协议的一部分**：
请求是 `<Add>`、响应是 `<AddResponse>`，两边同名就等于让对端拿着同一个 QName 分不出
手上那一段是请求还是响应（与 `soap:body/@namespace` 一起，这两个名字是 rpc 报文里
唯一能表明方向的东西）。取操作名不是"少一个后缀"，是**响应不可解析**。`document/literal`
走不到兜底（它的 output part 声明了 `@element`），所以这条只对 rpc 生效 —— 换句话说，
**`document` 那一档看不出这个错，rpc 那一档才露出来**。

要自己指定包装元素就用 `set_response_named(local, ns, inner)`：`ns` 给空串是合法的
（"响应元素没有命名空间"），这时不发 `xmlns`。

## 8. 拒绝与状态码

拒绝分两方，分错的后果是**告警指向错的人**：`Sender`/`Client` 是"你的报文/你的请求有
问题"，`Receiver`/`Server` 是"我这边处理不了"。

| 情形 | 码（1.1 / 1.2） | 谁的问题 |
|---|---|---|
| `Content-Type` 不是 SOAP 的两种之一 | `Client` / `Sender` | 对端（**`415`**） |
| 端点没装配好（挑不到 `service/port`） | `Server` / `Receiver` | **我方** |
| 解析失败（空 / 语法 / 超限 / 不是信封 / 没有 Body / Body 多于一个元素 / 那个元素是 Fault 但没码） | `Client` / `Sender` | 对端 |
| 版本不符合策略 | `VersionMismatch` | 对端 |
| 有不认识的 `mustUnderstand` 头 | `MustUnderstand` | 对端 |
| **请求本身是一条 Fault** | `Client` / `Sender` | 对端 |
| Body 元素派不到任何 operation | `Client` / `Sender` | 对端 |
| operation 在 WSDL 里、但没注册处理函数 | `Server` / `Receiver` | **我方** |
| 动作值与 WSDL 里声明的不一致 | `Client` / `Sender` | 对端 |

HTTP 状态码：**1.1 一律 `500`**（规范 §6.2 的每个例子都是 500），只有准入失败那条是
`415`；**1.2 分两张** —— `Sender` 一族 `400`、`Receiver` `500`。

★ **解析失败全部算对端的错**，哪怕触发它的是本层的上限：上限是本层的策略没错，但
"送来这么一份东西"是对端的动作。唯一的例外是端点没装配好 —— 那是**我们的**。

检查顺序（**顺序本身是判据**，因为它决定"同时踩两条时报哪一条"）：

```
准入 -> 装配 -> 解析 -> 版本策略 -> mustUnderstand -> 请求本身是 Fault
     -> 派发 -> 未实现 -> 动作核对 -> 处理函数 -> 三种结局
```

`mustUnderstand` 排在"请求本身是 Fault"**之前**：规范里前者属于"初步处理"（1.2 §5.4）
的一步，而 Body 是后一步 —— 一条**既是 Fault 又带** `mustUnderstand` 的报文报出来的
是 `MustUnderstand`。本层一个头都不理解，所以"数出来大于 0"就是"有不认识的头"；reason
里带条数（`...; count: 2`）。

`reason` / `faultstring` 是**发给对端的**，读它的常常是别人的 SOAP 工具包。所以这一层
发出去的每一句都是英文，本地那句中文诊断（`uvcpp_soap_parse()` 的 `why`）**不塞进去**
—— 塞进去两头不讨好：对端看不懂，我们还以为已经把上下文给出去了。

## 9. 版本从信封来

**版本是信封自己的属性**：1.1 的信封在 `http://schemas.xmlsoap.org/soap/envelope/`，
1.2 的在 `http://www.w3.org/2003/05/soap-envelope`。两个命名空间不同，所以
`uvcpp_soap_parse()` 认出来哪个就是哪个。

`Content-Type` **只做准入**（决定要不要 `415`、以及那个 `action=` 参数，§10）：
`text/xml` 归 1.1，`application/soap+xml` 归 1.2，两者都认、大小写不敏感。它给出来的
版本**只用于**"该回一个哪一版的 Fault"这种准入级判断。

★ 于是有一个非对称：**解析失败时，唯一还在手上的版本线索就是 `Content-Type`**（报文
本身没解出来）。这一层把它当**提示**（`hinted`，没有任何线索时默认 1.1 —— 兼容面最大
的那个形状），于是 **1.2 的客户端发一份坏信封，拿到的是 1.2 形状的 Fault + `400`，不是
`500`**。这一对在判据里是两个方向都钉了的。

版本策略只有三档：

```cpp
#include <wsdl/uvcpp_soap_service.h>

#include <memory>

// 只收 1.2；不符合的请求报 VersionMismatch（默认是 ANY，两版都收）。
void doc_soap_v12_only(std::shared_ptr<uvcpp::uvcpp_soap_service> ep) {
  ep->set_version_policy(uvcpp::uvcpp_soap_version_policy::V1_2_ONLY);
}
```

`VERSION_MISMATCH` 这个码**唯一用得到的地方就是这里**：本层自己不会因为别的原因发它
（信封命名空间不对是**解析失败**，落在上面那张表的第一行里）。

## 10. 动作值

`soapAction`（1.1 是 HTTP 头，1.2 是 `Content-Type` 的 `action=` 参数）是**可选**的，
所以核对规则是三条，缺一条都会把正常客户端拒掉：

1. **只在 WSDL 声明了非空 `soapAction`、且请求提供了非空值时才核对**；
2. **空值不算"提供了"**：`SOAPAction: ""` 与"根本没带这个头"在这一栏都是假。
   前者规范里的意思是"看报文体自己"（那正是本层的派发依据），后者是客户端漏了；
   两种都不该拒，所以这一层不区分它们，只区分"有没有一个**非空**的值可比"；
3. **按信封的版本选读哪个来源** —— 1.1 读 `SOAPAction` 头，1.2 读 `Content-Type` 的
   `action=` 参数。来源选错的后果是**静默的**（读取的地方没人看），所以两个方向在判据
   里都钉了。

比对之前两边都过 `uvcpp_soap_normalize_action()`：剥掉两端空白与**一对**包着的 `"`。
这一步是必须的 —— 1.1 的 `SOAPAction` 按规范带引号（`SOAPAction: "urn:calc#Add"`），
1.2 的 `action` 不带，不归一会永远比不相等。★ 引号**里面**多出来的空白也一起剥
（`" urn:x "` 与 `"urn:x"` 归一成同一个值）：少这一步，同一份 WSDL 会对前一种客户端
回 `200`、对后一种回 Fault，而两边看起来一模一样。

处理函数拿到的是归一后的 `call.action` 与 `call.action_present`（§5）。

## 11. 单向 operation

WSDL 里 `portType/operation` **没有 `output`** 就是单向（模型里记的是 `has_output`
这个"在不在"的布尔，不是"名字是不是空"）。这类调用：

- 处理函数**照常跑**（活还是要干的）；
- 处理函数给了响应 ⇒ **忽略**，回 `202 Accepted` + 空正文（规范里单向就是没有响应）；
- 处理函数**什么都没给** ⇒ 也回 `202`。这里**不**套用"没给响应就是 `Receiver` Fault"
  那条：单向 operation 的"什么都没给"是**正常**的；
- 处理函数给了 **Fault** ⇒ **发那条 Fault**（带正常的 Fault 状态码）。单向调用没有别的
  方式告诉调用方"失败了"，把它吞掉是最坏的选择。

## 12. Fault 的形状

一条 `uvcpp_soap_fault` 的字段与**报文里的位置**一一对应，两个版本共用一份结构：

| 字段 | 1.1 | 1.2 |
|---|---|---|
| `code` | `faultcode` 的本地名（`Client` / `Server`） | `Code/Value` 的本地名（`Sender` / `Receiver` / …） |
| `code_space` | `faultcode` 那个 QName 的命名空间（**只在解析时填**，见下） | 同上（从 `Code/Value` 里拆） |
| `subcode` / `subcode_space` | —— | `Code/Subcode/Value` |
| `reason` | `faultstring` | `Reason/Text` |
| `lang` | —— | `Reason/Text/@xml:lang` |
| `node` | —— | `Node` |
| `role` | `faultactor` | `Role` |
| `detail` | `detail` 元素**整段**原样 XML | `Detail` 同理 |

两个版本的元素名大小写是**故意**不一致的（1.1 全小写、1.2 首字母大写），`uvcpp_soap_fault_code_local(code, version)` 按版本翻本地名；语义码与本地名的对照表在
`uvcpp_soap_message.h` 的文件头。

★ **`code_space` 只在解析时填，生成时不用它**：写出去的码是按**信封前缀**拼出来的
QName（`soap:Client`；信封前缀为空时写裸名 —— 那种写法下默认命名空间正是信封那个，
所以不是将就）。要给 `role` /
`node` / `lang` 或自定义码，就自己填一条 `uvcpp_soap_fault` 交给
`set_fault(const uvcpp_soap_fault&)`；★ 那种情况下 HTTP 状态码是**从本地名反查语义码**
得出的（认不出来按 `Client` 算，1.2 下就是 `400`）—— 自定义码在规范里本来就该挂在标准码
的 `Subcode` 底下，一个裸的自定义顶层码几乎没有别的信息可依据。

## 13. 上限与安全

上限就是上半的 `uvcpp_wsdl_limits`（同一份结构，`set_limits()` 挂到端点上，`max_items`
用在 `Header`/`Body` 的子元素条数上），规矩也一样：**超过就拒**、**解析之前判**、
**不抛异常**。

★ 关于 XXE 与实体炸弹，三条事实（都是 pugixml 的结构决定的，不是本层加的白名单）：

- **不取回任何外部东西。** 本层用 `pugi::parse_default | parse_declaration` 解析，
  **没有** `parse_doctype`。pugixml 里根本没有"按 `SYSTEM`/`PUBLIC` 去读一个文件或发一个
  请求"这条路径 —— 所以那些需要外发请求或读本地文件的 XXE 变体在这一层**不可达**。
- **未声明的实体引用原样留着，不展开、也不报错。** 解析器只认五种预定义实体
  （`&amp;` `&lt;` `&gt;` `&quot;` `&apos;`）与数字字符引用，其余一律**照原样留在文本里**。
  于是"内部实体递归展开"那类炸弹（billion laughs）没有可乘之机：实体表里根本没有条目。
- **深度与元素数是迭代量的**，不是递归遍历出来的：递归遍历本身就是被防的那件事（一万
  层嵌套的文档会让守卫自己爆栈）。同理 pugixml 自己的解析器也是迭代实现 —— 这一条正好
  是我们需要的那条性质。

还有一条不是安全、是**语义**：`Header` 里 `mustUnderstand` 只**数**不判（§8），本层不
理解任何头。要支持 WS-Security / WS-Addressing 那类头得有对应语义，不在本轮范围
（§15）。

## 14. 判据

两条用例，都在 `UVCPP_ENABLE_WSDL=ON` 的树上跑：

| 用例 | 组数 | 钉什么 |
|---|---|---|
| `test_soap_message_func` | 7 | 信封层（与 WSDL 无关）：版本认对、字段读对位置、该拒的拒、写出来的字节逐字节对（1.1 与 1.2 各一套）、`mustUnderstand` 计数与 `Fault` 大小写 |
| `test_web_app_soap_func` | 6 | 端点层：装配（0.x）、派发与响应（1.x）、准入与解析失败（2.x）、版本策略（3.x）、动作值（4.x）、单向（5.x） |

端点那一条需要真的起 TCP 服务，所以它在 `web_app` 那一类里；信封那一条是纯静态调用点
（循环里那几条会跑多次，所以印出来的条数比源码里的 `check_*` 调用点多）。

三条本身也在判据里，值得知道：

- **拒的那九种情形，每一条都配了"只改一处、必须接受"的对照臂**。没有对照的话，"报了
  错"和"报了**对的**错"分不开，而后者才是要判的。
- **动作值与版本那两组，两个方向都钉了**（4.13~4.18：来源选错会怎样；3.8~3.11：
  内容类型与信封不一致时以谁为准）。
- **上限那一条的边界是从报文自己算的**：报文长度**等于**上限要收、**少一字节**要拒。
  写死一个上限数是危险的 —— 那个数一旦比报文还长，这条判据就退化成"发一个正常请求、
  看它正常回"，照样 PASS 而一个字节都没验（第一版就是这么撞上的）。
- **宏关闭时这两条用例不存在**（不是 `[skip]`，是文件名过滤器把它们整个拿掉）：
  `UVCPP_ENABLE_WSDL=OFF` 单独配一棵树实建实跑，实测 100 条用例、名字含 `wsdl|soap`
  的 **0** 条、树里没有 soap 可执行文件、也没 clone pugixml。这一条是刻意的 ——
  `[skip]` 照样 PASS 是本仓已知的坑。

跑法（都在构建树里）：

```
ctest -R soap --output-on-failure
```

## 15. 没做的（如实列出）

- **WS-Security / WS-Addressing 那类头**：不回响应 `Header`，也不认任何头。
  `mustUnderstand` 只是数出来（§8、§13）。
- **不做 `types` 到 C++ 的映射**（§5）：没有 schema 校验，也没有反序列化器。参数取出来
  是**文本**，转换错误由处理函数自己判。
- **不做 MTOM / 附件**：`xsd:base64Binary` 会当普通文本把整段搬进内存。
- **`rpc/encoded` 不实现**：`style` / `use` / `encodingStyle` 都如实读进模型（上半 §4），
  但本层不据此组包 —— `encoded` 已经是被 WS-I 基本剖面淘汰的用法。
- **不解析 `soap:header` / `soap:headerfault`**（上半 §12 同一条）：模型里没有它们，
  所以"Header 里该有什么"本层不知道。
- **不做 `portType` 继承**（上半 §12 同一条）：`binding/operation` 的 `@name` 在它引用的
  portType 里找不到时，本层**照样收下**那条 operation —— 跳过它只会让一个服务方的文档
  错误在上线后表现成"对端发错了"（`Sender` Fault）。真用继承的文档会在这里少几个
  operation，`uvcpp_wsdl_check_references()` 会报出来。
- **不做 SOAP over 别的协议**：只有 HTTP 那一条路（`uvcpp_soap_serve()` 装的是 POST，
  `GET` 不管 —— 要 `?wsdl` 就另外调上半的 `uvcpp_wsdl_serve()`）。
