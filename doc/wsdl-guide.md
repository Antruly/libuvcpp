# WSDL / SOAP 指南（上半：文档与发布）

`uvcpp` 的 WSDL 支持分成上下两半。**这一页只讲上半**：把一份 WSDL 1.1 文档
解析成模型、按名字查它、原样发出去，以及**从模型生成**一份 WSDL。下半（SOAP
信封、`soap:Fault`、按 operation 名派发、响应序列化）在
[`doc/soap-guide.md`](soap-guide.md) —— 两半是同一个模块、同一个开关，读哪一页
取决于你要解决的是"这份文档怎么读"还是"这条报文怎么处理"。

它建在 `webapp` 之上：唯一的开关 `UVCPP_ENABLE_WSDL` 默认 **OFF**，XML 后端是
**pugixml**（静态链入，`PUGIXML_VERSION` 可钉，默认 `v1.14`）。

## 目录

1. [这一半解决什么](#1-这一半解决什么)
2. [编译期条件与包含](#2-编译期条件与包含)
3. [五分钟上手](#3-五分钟上手)
4. [模型](#4-模型)
5. [命名空间：按 (URI, 局部名) 判](#5-命名空间按-uri-局部名-判)
6. [QName 引用怎么查](#6-qname-引用怎么查)
7. [`<types>` 原样存，dump 不是字节往返](#7-types-原样存dump-不是字节往返)
8. [把 WSDL 发出去](#8-把-wsdl-发出去)
9. [上限与错误码](#9-上限与错误码)
10. [引用闭合检查](#10-引用闭合检查)
11. [判据](#11-判据)
12. [没做的（如实列出）](#12-没做的如实列出)

---

## 1. 这一半解决什么

服务端要发 WSDL，只有两条路，两条都被这一半覆盖：

| 你要做的事 | 用哪个 |
|---|---|
| 手上已经有一份 `.wsdl` 文件（自己写的、或别的工具生成的） | 直接发它的字节：`uvcpp_wsdl_source(src)` + `uvcpp_wsdl_serve()`（§8） |
| 服务端**就是**那套接口的定义处，WSDL 应当从代码里的接口长出来 | 填模型 + `uvcpp_wsdl_dump()`（§4、§7） |

两条路之外还多一件事，是**解析**：你要按 operation 名做点什么（哪怕只是启动时
自检一句"这份 WSDL 里真的有 `Add` 吗"），就得先把文档读成模型。所以这一半的
核心是**模型 + 两个方向的转换**（parse / dump），发布那一层只是把它的产出接到
HTTP 上。

**为什么用 pugixml 而不是手写 XML 解析器**：这个库的做法一贯是"能借的借、借来的
关在一个私有头里"（nghttp2、nlohmann 都是这么进来的）。XML 的良构性检查里有一堆
真实的坑 —— 实体引用、CDATA、注释里不能出现 `--`、属性的引号配对、元素名里的
冒号 —— 手写一遍要几百行才勉强对，而 pugixml 是三个源文件、MIT、零传递依赖，
且**它自己的解析器就是迭代实现**（不递归，不会因为一万层嵌套而爆栈），这一条
正好是我们需要的那条性质（§9）。

## 2. 编译期条件与包含

- 开关：`-DUVCPP_ENABLE_WSDL=ON`，默认 `OFF`。
- **它需要 webapp**：`UVCPP_BUILD_WEBAPP=OFF` 时 `UVCPP_ENABLE_WSDL` 会被**强制
  关掉**并给一条 warning（`CMakeLists.txt`，与 `UVCPP_ENABLE_NGHTTP2` 在
  OpenSSL 关掉时那条同形）—— 留一个"开着但建不出来"的配置比关掉更糟。
- **pugixml 是私有依赖**：`<pugixml.hpp>` 只出现在 `src/wsdl/uvcpp_wsdl_pugixml.h`
  这一个**不装出去**的头里，公开头一个 pugi 类型都没有。连带效果有三条：使用者
  既不需要 pugixml 的头也不需要它的库；发布包里**不需要**为它加拷贝项；Windows
  的 DLL 清单也一行都不用改（它是静态链进来的）。
- 版本可钉：`-DPUGIXML_VERSION=v1.14`（`FetchContent` 拉的 tag）。关掉这个模块时
  一次网络请求都不会发。
- 宏由生成头给出：`UVCPP_WSDL_ENABLE`（含在这个模块的两个公开头里判）。
- 包含方式：`#include <wsdl/uvcpp_wsdl_document.h>` 与
  `#include <wsdl/uvcpp_wsdl_serve.h>`。**它们不在 `<uvcpp.h>` 里** —— 那个聚合头
  只收 `uvcpp_define/version/export` 与 `handle/loop/req` 三组。
- C++11（本仓没有 per-target 覆盖）。没有 `std::string_view`、没有 `if constexpr`。

导出的东西：文档模型 `uvcpp_wsdl_document` 与它下面那组结构（§4）、`wsdl_status`、
`uvcpp_wsdl_limits`、两个方向的转换（`uvcpp_wsdl_parse` / `uvcpp_wsdl_dump`）、
`uvcpp_wsdl_check_references`、两个转义函数、发布那一层的
`uvcpp_wsdl_source` / `uvcpp_wsdl_send` / `uvcpp_wsdl_serve` / `uvcpp_wsdl_content_type`。

## 3. 五分钟上手

```cpp
#include <wsdl/uvcpp_wsdl_document.h>

#include <string>

// 「这份 WSDL 里 Add 这个 operation 的 input message 叫什么」——任一步缺失就
// 返回一句说明（不是抛异常，见 §9）。
std::string doc_wsdl_add_input(const std::string& wsdl_text) {
  uvcpp::uvcpp_wsdl_document doc;
  const char* why = nullptr;
  const uvcpp::wsdl_status st =
      uvcpp::uvcpp_wsdl_parse(wsdl_text, doc, uvcpp::uvcpp_wsdl_limits(), &why);
  if (st != uvcpp::wsdl_status::OK) {
    return why ? std::string(why) : std::string(uvcpp::wsdl_status_name(st));
  }
  if (doc.port_types.empty()) return std::string();
  const uvcpp::uvcpp_wsdl_operation* op = doc.port_types[0].find_operation("Add");
  if (op == nullptr) return std::string();
  const uvcpp::uvcpp_wsdl_message* m = doc.find_message(op->input.message.str());
  return m ? m->name : std::string();
}
```

输入长这样（节选自测试里那份 Calculator WSDL；`wsdl:` 前缀也能写成 `s:` 或干脆
不写，见 §5）：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<wsdl:definitions name="Calculator"
                  targetNamespace="urn:calc"
                  xmlns:wsdl="http://schemas.xmlsoap.org/wsdl/"
                  xmlns:tns="urn:calc"
                  xmlns:soap="http://schemas.xmlsoap.org/wsdl/soap/"
                  xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <wsdl:message name="AddRequest">
    <wsdl:part name="a" type="xs:int"/>
    <wsdl:part name="b" type="xs:int"/>
  </wsdl:message>
  <wsdl:portType name="CalculatorPortType">
    <wsdl:operation name="Add" parameterOrder="a b">
      <wsdl:input message="tns:AddRequest"/>
      <wsdl:output message="tns:AddResponse"/>
    </wsdl:operation>
  </wsdl:portType>
</wsdl:definitions>
```

两条最容易踩的：

- **`find_message(op->input.message.str())` 那一格**：`op->input.message` 是
  `uvcpp_qname`，`.str()` 给出 `{uri}local` 形状；`find_*` 认三种写法（§6）。
  直接把 `uvcpp_qname` 喂进 `find_message` 是不行的 —— 它收的是**引用文本**。
- **`port_types[0]` 只是取其一的写法**，不是"默认 portType"。真实文档里可能有
  好几个；按名字取用 `doc.find_port_type("tns:CalculatorPortType")`。

## 4. 模型

模型是**公开的数据结构**，不是有不变量的句柄：字段就是 `public` 成员，按模型
生成 WSDL 那条路（§7）因此可以像填表单一样写。`find_*` 那组只提供"按键查一条"，
不提供修改。

| 结构 | 对应元素 | 主要的字段 |
|---|---|---|
| `uvcpp_wsdl_document` | `wsdl:definitions` | `target_namespace`、`name`、`documentation`、`types_xml`、`root_namespaces`、`imports`、`messages`、`port_types`、`bindings`、`services` |
| `uvcpp_wsdl_message` | `message` | `name`、`parts`、`find_part()` |
| `uvcpp_wsdl_part` | `part` | `name`、`element`、`type`（至多一个非空）、`documentation` |
| `uvcpp_wsdl_port_type` | `portType` | `name`、`operations`、`find_operation()` |
| `uvcpp_wsdl_operation` | `portType/operation` | `name`、`has_input`/`has_output`、`input`/`output`/`faults`、`parameter_order` |
| `uvcpp_wsdl_op_ref` | `input` / `output` / `fault` | `name`、`message`（QName）、`documentation` |
| `uvcpp_wsdl_binding` | `binding` | `name`、`port_type`、`soap`、`operations`、`find_operation()` |
| `uvcpp_wsdl_soap_binding` | `soap:binding` / `soap12:binding` | `present`、`is_soap12`、`style`、`transport` |
| `uvcpp_wsdl_binding_operation` | `binding/operation` | `name`、`soap_action`、`style`、`input_use`/`output_use`、`input_body_parts`/`output_body_parts`、`effective_style()` |
| `uvcpp_wsdl_service` | `service` | `name`、`ports`、`find_port()` |
| `uvcpp_wsdl_port` | `port` | `name`、`binding`、`address`、`is_soap12` |
| `uvcpp_wsdl_import` | `import` | `namespace_uri`、`location` |

三处值得单独说的：

**`has_input` / `has_output` 是单独的 bool，不是"`input.message` 是不是空"。**
单向（one-way）operation **合法地**没有 `output`；而"`output` 在、`@message`
忘了写"是另一回事（坏文档）。只看 `output.message.empty()` 区分不出这两者，
于是要么把单向操作误判成坏文档，要么把坏文档放过去 —— 而 SOAP 派发正要按这个
决定回不回响应。

**`soap:binding` 与 `soap12:binding` 共用一份结构**，用 `is_soap12` 区分：两者
元素名一样、只差命名空间。`transport` 只有 1.1 写（1.2 的 `soap:binding` 不带它）。

**`input_body_parts` 为空 = 没写 `@parts` = message 的**全部** part 都进 body**
（WSDL 1.1 的默认语义，不是"一个都不进"）。这条反直觉，所以模型里存的是"切好的
列表 + 空表示未写"，而不是塞一个默认全集进去。

认不出的元素**一律跳过，不报错**：WSDL 允许扩展元素（`wsdl:documentation`、
厂商私有的 `wsaw:`、`wsp:` 策略都在这一档），为它们报错会把合法文档拒掉。
扩展**属性**同理。

## 5. 命名空间：按 (URI, 局部名) 判

`soap:` / `s:` / `SOAP:` 三种前缀写法都认，**默认命名空间下的 WSDL 也认**
（`<definitions xmlns="http://schemas.xmlsoap.org/wsdl/">` 是合法且常见的写法）。

这不是宽容，是唯一正确的做法：前缀只是个**局部别名**，跨文档没有任何意义。
机制在私有层：pugixml 1.14 没有公开的命名空间解析 API（`namespace_uri()` 只在它
内部给 XPath 用），`node.name()` 返回的是**带前缀的限定名**。所以本模块自己维护
一个**随遍历进出的作用域栈**，匹配元素时按 (URI, local) 比。

栈里那个**空串前缀**是默认命名空间（`xmlns=...`），它作用于不带前缀的元素 ——
只认带前缀的写法会漏掉整类文档。子元素可以**自己声明前缀**
（`<w:message xmlns:w="…wsdl/">`），所以"这个子元素是不是 `{wsdl}message`"这句
话只有在把子元素自己的 `xmlns*` 压进栈之后才成立。

**负向的一面同样重要**：把 `soap:binding` 绑到 `http://example.com/not-soap/`
的文档**不会被认成 SOAP 绑定**（判据里有一条"只把 URI 改掉"的对照臂，见 §11），
而一份**根本没声明命名空间**的 WSDL 会以 `NOT_WSDL` 报出去，而不是被"宽容地"
收下 —— 那种宽容会把真正的拼写错误藏起来。

模型里那些 URI 常量在 `uvcpp::wsdl_ns`（`wsdl()`、`soap_binding()`、
`soap12_binding()`、`xsd()`、`soap11_envelope()`、`soap12_envelope()`、`wsdl20()`）。
写成函数而不是全局 `const char*`：后者在动态库上是一条跨模块符号引用，而这些
是编译期字面量，没有理由让它们有地址。

## 6. QName 引用怎么查

`find_message` / `find_port_type` / `find_binding` / `find_service` 收的是**引用
文本**，认三种写法：

| 写法 | 判据 |
|---|---|
| `"Add"` | 按**局部名**查，不挑命名空间 |
| `"{urn:calc}Add"` | 要求 `urn:calc` 逐字节等于本文档的 `targetNamespace` |
| `"tns:Add"` | **用文档根上的 `xmlns*` 声明表把前缀翻成 URI 再比** |

第三种是**必须**的：真实 WSDL 里引用写的是前缀
（`binding/@type="tns:CalculatorPortType"`），而不是把那串 URI 抄一遍。所以
"文档自己声明的 `tns:Add` 查得到、而一个没声明过（或指向别处）的前缀查不到"，
就是这一格的判据。

```cpp
#include <wsdl/uvcpp_wsdl_document.h>

#include <string>

bool doc_wsdl_lookup_forms(const uvcpp::uvcpp_wsdl_document& doc) {
  // 三种写法查同一条 message（文档里 tns == targetNamespace == urn:calc）。
  return doc.find_message("AddRequest") != nullptr &&
         doc.find_message("tns:AddRequest") != nullptr &&
         doc.find_message("{urn:calc}AddRequest") != nullptr;
}
```

`uvcpp_qname_local(text)` 是这一格的拆解工具：把一个 QName 文本的**局部名**取出来
（依次剥掉 `{uri}Foo` 与 `tns:Foo` 两种形状）。`uvcpp_qname` 自己带 `str()`，
给出 `{uri}local` 形状。

## 7. `<types>` 原样存，dump 不是字节往返

**`types_xml` 存的是整个 `<types>` 元素的原样文本，不是它的内容。** 理由：原文档
里那个元素可能**自带 `xmlns` 声明**（`<types xmlns:xs="…XMLSchema">` 很常见），
拆掉外层标签就把它自己的声明一起丢了 —— 重新发出去的文档里那些前缀就没人定义。

**要自己生成 XSD，就写 `"<types>…</types>"` 整段**，内层用哪个前缀由你定，只要
保证它在根上能解析到（见下）。

`uvcpp_wsdl_dump` 的命名空间策略：

- `tns` **保留**给 `targetNamespace`（WSDL 文档里的惯例写法）；
- 原文档根上的全部 `xmlns*` **照搬**到输出根上 —— 因为原样存进来的 `types` 片段
  可能正用着它们（`<wsdl:types>` 就要求根上有 `xmlns:wsdl`）；
- 模型里出现的、上面两类都没覆盖到的 URI，按**偏好前缀**分配（`wsdl` / `soap` /
  `soap12` / `xsd`，被占了就 `ns1`、`ns2`…）。

产出是**确定性的**：同一个模型永远逐字节相同（缩进固定两空格、行尾 LF、结尾一个
换行）。所以它可以直接当缓存键、也可以直接进版本控制。

**dump 不是格式化器，也不是字节级往返。** 它输出的是**模型投影**：模型不认识的
属性、注释、处理指令、以及 `wsdl:import` 指到的**别的文档的内容**都不会出现。
判据因此是**语义往返**（parse → dump → parse，两次的模型相等，见 §11），不是
"照着原文件 diff 为空"。文档里的文本节点在读进来时会**剥掉 CR** —— 否则一份
CRLF 的输入 dump 出来会是 LF，"再 parse 回去相等"这条判据就会因为行尾而假红。

```cpp
#include <wsdl/uvcpp_wsdl_document.h>

#include <string>

// 从零拼一份最小的 WSDL：一个 message、一个 part（`xs:int`）。
std::string doc_wsdl_build() {
  uvcpp::uvcpp_wsdl_document doc;
  doc.target_namespace = "urn:doc:calc";
  doc.name = "DocCalculator";

  uvcpp::uvcpp_wsdl_message m;
  m.name = "AddRequest";
  uvcpp::uvcpp_wsdl_part p;
  p.name = "a";
  p.type = uvcpp::uvcpp_qname(uvcpp::wsdl_ns::xsd(), "int");
  m.parts.push_back(p);
  doc.messages.push_back(m);

  return uvcpp::uvcpp_wsdl_dump(doc);   // 根上会自动出现 xmlns:tns 与 xmlns:xsd
}
```

## 8. 把 WSDL 发出去

这一层只有两件事：**序列化一次、发多次**，和**一条 GET 路由**。

`uvcpp_wsdl_source` 持有一个 `shared_ptr<const std::string>`，`uvcpp_wsdl_send`
把它**按引用**交给响应（`body_share`），所以 N 个客户端拿到的是**同一块字节**，
不是 N 份拷贝。WSDL 是典型的"启动时定好、之后只读"的资源，这条正是它该走的路。

发出去的是 **200 + `text/xml; charset=utf-8`**，正文就是 `text()` 里的字节；
**不做** 404/500 之类的代替决定（源是空的就是空正文）。这一层不替调用方判断
"这份 WSDL 该不该给人看"。

```cpp
#include <wsdl/uvcpp_wsdl_serve.h>

#include <string>

void doc_wsdl_publish(uvcpp::uvcpp_web_app& app, const std::string& wsdl_text) {
  const uvcpp::uvcpp_wsdl_source src(wsdl_text);   // 收下已经序列化好的文本
  uvcpp::uvcpp_wsdl_serve(app, "/calc.wsdl", src);
}
```

另一条重载直接收模型（内部先 dump 一次）：
`uvcpp_wsdl_serve(app, path, doc)`。

**两条与"换内容"有关的语义，都必须知道**：

1. **路由拿的是注册那一刻的共享指针**（按值捕获）。所以注册完 `uvcpp_wsdl_source`
   那个对象**可以立刻析构**，字节仍活着；反过来，注册**之后**再
   `set_from_document()` / `set_from_text()` 改它，**已经装好的那条路由不会跟着
   变**。这条是刻意的：一条运行时被换掉 body 的路由，行为取决于"请求到达时是
   哪一版"，那种不确定性比"要么两版都能工作、要么都不工作"糟得多。
2. **想换内容，别指望"再 `uvcpp_wsdl_serve()` 一次"。** 路由表里**同一个模式
   注册两次是静默无效的**：`match()` 在特异度打平时只在一种情况下换手（候选是
   真方法匹配、而当前最优是 HEAD 回退），两条同方法的同模式路由之间没有这种
   换手 —— 于是**先注册的那条一直赢**。正当路子有两条：

   - **换路径**（`/svc.wsdl` → `/svc2.wsdl`），然后在服务端做重定向；
   - **自己写 handler**，在里头调 `uvcpp_wsdl_send(src, resp)` —— 那条路在**每次
     请求时**读源，所以 `set_from_*()` 是对它生效的。代价是：源被循环线程读的
     同时你在别的线程写它，那是**数据竞争**，要改就得先停机或在自己的同步下改。

```cpp
#include <wsdl/uvcpp_wsdl_serve.h>

#include <memory>
#include <string>

// 第二条路：handler 每次请求读源，所以 set_from_* 生效（要自己管同步）。
void doc_wsdl_dynamic(uvcpp::uvcpp_web_app& app, const std::string& path) {
  const std::shared_ptr<uvcpp::uvcpp_wsdl_source> live =
      std::make_shared<uvcpp::uvcpp_wsdl_source>();
  app.get(path, [live](uvcpp::uvcpp_web_request& req,
                       uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next next) {
    (void)req;
    (void)next;
    uvcpp::uvcpp_wsdl_send(*live, resp);
  });
}
```

## 9. 上限与错误码

三条规矩与 `src/webapp/uvcpp_web_json.h` 那套同形：**不抛异常**、**解析前有上限**、
**后端隔离**。

唯一的例外是 `std::bad_alloc`：pugixml 只在分配失败时抛，与本库走 nlohmann 那条
暴露面相同（调用点全在 libuv 的回调里，异常从那里逃出去会穿过 C 写的 `uv_run`
栈帧，是未定义行为 —— 所以每个入口都只返回状态码，如实记着这一个例外）。

上限是"**超过就拒**"，不是"截断"。四个数都在 `uvcpp_wsdl_limits` 里，且**在建模
型之前**生效：先卡输入字节数，再**迭代地**量一遍深度与节点数。这里一定不能递归
—— 递归遍历本身就是被防的那件事（一万层嵌套的文档会让守卫自己爆栈）。

| `wsdl_status` | 含义 |
|---|---|
| `OK` | 成功 |
| `EMPTY` | 输入长度为 0（与"语法错"分开，调用方才能区分 400 与 415） |
| `SYNTAX` | 不是良构的 XML（**全是空白**也算，理由是它没有文档元素） |
| `TOO_LARGE` | 超过 `max_bytes`（默认 4 MiB） |
| `TOO_DEEP` | 超过 `max_depth`（默认 64） |
| `TOO_MANY_NODES` | 超过 `max_nodes`（默认 65536） |
| `TOO_MANY_ITEMS` | 超过 `max_items`（默认 4096，**每一类**元素各自算） |
| `NOT_WSDL` | 良构，但根元素不是 `{wsdl}definitions` |
| `UNSUPPORTED_VERSION` | 根是 `{http://www.w3.org/ns/wsdl}description`，即 WSDL **2.0** |
| `NO_TARGET_NAMESPACE` | 缺 `targetNamespace`（WSDL 1.1 §3.1 要求它存在） |

顺序是：空 → 超字节 → 不是良构 → 超深度/元素数 → 根不对（其中 2.0 单独报）→
缺 `targetNamespace`。`wsdl_status_name()` 给一句话（判据里直接印它，比印数字好查）。

```cpp
#include <wsdl/uvcpp_wsdl_document.h>

#include <string>

uvcpp::wsdl_status doc_wsdl_tight(const std::string& text) {
  // 只改字节上限，其余三个用默认值。超了返回 TOO_LARGE，不是截断。
  const uvcpp::uvcpp_wsdl_limits lim(16u * 1024u);
  uvcpp::uvcpp_wsdl_document doc;
  const char* why = nullptr;
  const uvcpp::wsdl_status st = uvcpp::uvcpp_wsdl_parse(text, doc, lim, &why);
  return st;   // 非 OK 时 why 里有一句说明（来自本层或 XML 层）
}
```

`why` 指向的是**静态字符串或常量串**，不接收时不写；返回 `SYNTAX`/`TOO_*` 时那句
说明来自 XML 层。

## 10. 引用闭合检查

**解析时不做引用闭合检查**：不因为"引用指到别的文档去了"而拒 —— 那种严法会把
合法文档拒掉。要判就问一句：

```cpp
#include <wsdl/uvcpp_wsdl_document.h>

#include <string>

bool doc_wsdl_refs_ok(const uvcpp::uvcpp_wsdl_document& doc, std::string* why) {
  return uvcpp::uvcpp_wsdl_check_references(doc, why);
}
```

查四件事：`binding/@type` 指得到 portType、`service/port/@binding` 指得到 binding、
`portType/operation` 的 `input`/`output`/`fault/@message` 指得到 message、以及每个
`binding/operation/@name` 在它引用的 portType 里存在（SOAP 派发要按这个名字查，
缺了就是运行时才发现）。

**反方向不查**：不要求 binding 覆盖 portType 的全部 operation —— 真实 WSDL 里
有只绑一部分的，那条合法的"部分绑定"在判据里是**正例**。

**有 `wsdl:import` 时本判据依然只查本文档内。** `wsdl:import` 是**只记账、不取回**
（跨文档取回以及随之而来的循环 import 超出本模块范围）。所以你知道引用指向
import 进来的文档时，就别调它 —— 或者说别拿它的失败当"文档坏了"。

## 11. 判据

这一半有两条 ctest 用例，都在 `UVCPP_ENABLE_WSDL=ON` 的树上跑（整个模块一共四条；
下半那两条 —— `soap_message_func` 与 `web_app_soap_func` —— 在
[`doc/soap-guide.md`](soap-guide.md) §14）：

| 用例 | 组数 | 钉什么 |
|---|---|---|
| `test_wsdl_document_func` | 6 | 逐字段解析（含 `find_*` 三种写法）、四种前缀写法的**指纹相等** + 假 URI 负对照 + 固定 URI 正对照、语义往返、错误码阶梯（9 条**各配一条"只改一处、必须接受"的对照臂**）、引用闭合的正反例、两个转义函数 |
| `test_web_app_wsdl_func` | 2 | 发布层：200 + `text/xml; charset=utf-8` + 正文逐字节相同 + 能重新解析回来 + 未注册路径 404；以及 §8 那两条"换内容"语义（含一条"新路径拿同一个源必须发新内容"的正对照） |

两条判据本身也在判据里，值得知道：

- **指纹**是 `wsdl_document_func.cpp` 里一个把所有模型字段平展开的本地函数
  （漏 `root_namespaces`/`source_bytes` 这两个"来源相关"的字段），`\n`/`\r` 在
  拼进去之前被转义，所以多行值不会把指纹撞平。用它判"四种前缀写法的模型相同"，
  比逐个字段比更不容易漏 —— 而漏一个字段就正好是这个模块最容易骗过自己的地方。
- **宏关闭时这四条用例都不存在**（不是 `[skip]`，是文件名过滤器把它们整个拿掉 ——
  上下两半一样，`ctest -N` 出来的名字里一条都找不到）：
  实测 `UVCPP_ENABLE_WSDL=OFF` 的三棵树分别 100/100、107/107、46/46，且用例名里
  含 `wsdl|soap` 的 0 条。★ 数的时候要先滤掉 `ctest -N` 的**头一行**
  （`Test project <构建目录>`）：构建目录名里带 `wsdl` 时，直接
  `ctest -N | grep -ci wsdl` 会把那一行数进去、把 0 报成 1，看着像"关掉了还有一条
  用例剩着" —— 判据自己带进来一个与被测对象无关的匹配源。用
  `ctest -N | grep -E '^ *Test +#' | grep -ciE 'wsdl|soap'`。这一条是刻意的 ——
  `[skip]` 照样 PASS 是本仓已知的坑，所以"关掉模块时用例仍在"等于没有判据。
- **错误码阶梯每一条都配对照臂**：只改一处、其余不变的那份**必须接受**。没有
  对照的话，"报了错"和"报了**对的**错"分不开，而后者才是要判的。

## 12. 没做的（如实列出）

**SOAP 那一半不在这里**：`Envelope`/`Header`/`Body` 的解析、`soap:Fault`、
SOAP 1.1 与 1.2 的命名空间差异、按 operation 名把 Body 里第一个元素派发给处理函数、
SOAP 响应与 Fault 的序列化 —— 全在 [`doc/soap-guide.md`](soap-guide.md)，那一页也有
它自己"没做的"清单（那页的 §15）。派发要用的那几个**文档属性**（`soapAction`、
`style`、`use`、1.1/1.2 之分）**收在模型里**，因为它们属于文档、不属于运行时。

**这一半自己没做的**：

- **`wsdl:import` 不取回**（§10）。只记 `namespace_uri` + `location`。
- **WSDL 2.0 不支持**，只把它**认出来**并报 `UNSUPPORTED_VERSION`。
- **不校验 `types` 里的 XSD**：`<types>` 整段原样存、原样发（§7）。所以"WSDL 里
  的 schema 说 `a` 是 `int`，而 SOAP 消息里给的是字符串"这类问题，本模块不判。
- **不做 XSD 到 C++ 的代码生成**（gSOAP / `wsdl2h` 那一类）。
- **不解析 `soap:header` / `soap:headerfault`**：模型里没有它们（1.1 的
  `soap:header` 对应 Header 块的静态声明，属于下半，见
  [`doc/soap-guide.md`](soap-guide.md) §13/§15）。
- **`rpc`/`encoded` 只记录不实现**：`style`、`use`、`encodingStyle`、`namespace`
  都如实读进模型，但本模块不据此组包 —— 组包是下半的事，且 `encoded` 已经是被
  WS-I 基本剖面淘汰的用法（下半同样不实现它，那页的 §15）。
- **不做 `portType` 继承**：`portType/@extends` 是个扩展属性，本模块只读这个
  `portType` **自己**的 `operation`，不把继承来的那些合进来 —— 而 SOAP 派发正是
  要按 operation 名查，所以真用继承的文档会在这里少几个 operation。这是刻意的
  取舍：跟进去就要处理继承环，而那个检查现在没有。
