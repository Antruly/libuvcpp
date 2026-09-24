/**
 * @file src/wsdl/uvcpp_wsdl_pugixml.h
 * @brief 私有头：把 pugixml 关在这一个文件里（**不装出去**）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么它不装出去
 * ----------------
 * 与 `src/http2/uvcpp_h2_nghttp2.h` 同一个理由：这个头把 `<pugixml.hpp>` 拉进来，
 * 正是**为了让别的头不拉**。`src/wsdl/` 里其余的头（文档模型、发布）一个 pugi
 * 类型都不出现，所以使用者既不需要它的头，也不需要它的库 —— 连带地，
 * `package_release.py` 不用为 pugixml 加任何拷贝项，Windows 的 DLL 清单也一行
 * 都不用改（它是静态链进来的）。两处必须一起改：
 * `CMakeLists.txt` 的 install 段与 `tests/tools/package_release.py` 的
 * `PRIVATE_HEADERS`。
 *
 * 这层要做的三件事
 * ----------------
 * 1. **不抛异常。** pugixml 的解析本身就用返回值（`xml_parse_result`），只在
 *    **分配失败**时抛 `std::bad_alloc`。而我们的调用点全在 libuv 的回调里，
 *    异常从那里逃出去会穿过 C 写的 `uv_run` 栈帧 —— 未定义行为。所以本层的
 *    每一个入口都只返回状态码；分配失败是**唯一**还能逃出去的东西，与 nlohmann
 *    那条暴露面相同（见 `src/webapp/uvcpp_web_json.h` 顶部），如实记在这里。
 *
 * 2. **上限。** pugixml 的解析器是**迭代**的（不爆栈），但节点数没有上限 ——
 *    一个 8 MiB 的 `<a/><a/><a/>...` 能建出百万个节点。所以这里先卡输入字节数，
 *    再在**建模型之前**迭代量一遍深度与节点数。上限的作用是把最坏情况变成
 *    "输入大小 × 常数"，而不是"输入的形状决定内存"。
 *
 *    **量的时候一定不能递归**：递归遍历本身就是被防的那件事（一万层嵌套的
 *    文档会让守卫自己爆栈）。所以本文件里没有一处递归。
 *
 * 3. **命名空间。** pugixml 1.14 **没有公开的命名空间解析 API**
 *    （`namespace_uri()` 只在它内部给 XPath 用），`node.name()` 返回的是**带前缀
 *    的限定名**。所以前缀到 URI 的解析由本层自己维护：一个随遍历进出的作用域栈。
 *    这不是绕路 —— 它换来一件事：匹配元素时按 **(URI, local)** 比，而不是按
 *    字面前缀比，于是 `soap:` / `s:` / `SOAP:` 三种写法都认，而**默认命名空间
 *    下的 WSDL**（`<definitions xmlns="http://schemas.xmlsoap.org/wsdl/">`，
 *    一个很常见的写法）也认。
 */

#pragma once
#ifndef SRC_WSDL_UVCPP_WSDL_PUGIXML_H
#define SRC_WSDL_UVCPP_WSDL_PUGIXML_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WSDL_ENABLE

#include <pugixml.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace uvcpp {
namespace wsdl_detail {

// =========================================================================
// 上限
// =========================================================================

/** @brief 一份 XML 的解析上限。三个数都是"超过就拒"，不是"截断"。 */
struct xml_limits {
  /** 输入字节数上限，**解析之前**判。默认 4 MiB（一份 WSDL 通常几 KB）。 */
  size_t max_bytes;
  /** 元素嵌套深度上限。默认 64 —— 与 JSON 那条同值，理由也相同：真文档不会更深。 */
  size_t max_depth;
  /** 元素总数上限。默认 65536。 */
  size_t max_nodes;

  explicit xml_limits(size_t bytes = 4u * 1024u * 1024u, size_t depth = 64,
                      size_t nodes = 65536u)
      : max_bytes(bytes), max_depth(depth), max_nodes(nodes) {}
};

/** @brief 本层的解析结果。取值是**故意**与 `json_status` 分开的一套。 */
enum class xml_result : int {
  OK = 0,
  /** 输入为空（长度为 0）。与"语法错"分开，调用方才能区分 400 与 415。 */
  EMPTY,
  /** 不是良构的 XML。 */
  SYNTAX,
  /** 超过 `max_bytes`。 */
  TOO_LARGE,
  /** 超过 `max_depth`。 */
  TOO_DEEP,
  /** 超过 `max_nodes`。 */
  TOO_MANY_NODES
};

/** @brief 给使用者看的一句话（判据里直接印它，比印数字好查）。 */
const char* xml_result_name(xml_result r);

// =========================================================================
// 解析
// =========================================================================

/**
 * @brief 把一段字节解析成 pugixml 文档。
 *
 * 顺序（与 `uvcpp_json_parse` 同形）：空 -> EMPTY；超字节数 -> TOO_LARGE；
 * 解析失败 -> SYNTAX；超深度/节点数 -> TOO_DEEP / TOO_MANY_NODES。
 * `out` 只在成功时被写。
 *
 * @param why 非空时接收一句**指向静态字符串**的说明（不接收时不写）。
 */
xml_result xml_parse(const char* data, size_t len, pugi::xml_document& out,
                     const xml_limits& lim, const char** why);

/**
 * @brief 迭代地量一棵树的深度与元素数（**不递归**，见文件头）。
 *
 * @param root 从哪个节点往下量（含自身）。
 */
xml_result xml_measure(const pugi::xml_node& root, const xml_limits& lim,
                       const char** why);

// =========================================================================
// 命名空间作用域栈
// =========================================================================

/**
 * @brief 前缀 -> URI 的作用域栈。**必须与遍历同进同出**（`push` 进一个元素、
 *        处理完它的子树后 `pop`）。
 *
 * 空串那个键是**默认命名空间**（`xmlns="..."`），它作用于**不带前缀的元素**。
 * 这是 XML 的语义，不是本库的发明：`<definitions xmlns="...wsdl/">` 是完全
 * 合法的 WSDL，只认带前缀的写法会漏掉它。
 */
class xml_ns_stack {
 public:
  xml_ns_stack();

  /** 收下本元素上的 `xmlns` / `xmlns:foo` 声明，然后压层。 */
  void push(const pugi::xml_node& node);
  void pop();

  /** @return 前缀绑定的 URI；没声明过返回空串。`prefix` 为空 = 默认命名空间。 */
  const std::string& resolve(const std::string& prefix) const;

  /** @brief 元素自身的限定名（`node.name()` 的**原样**）。 */
  static std::string raw_name(const pugi::xml_node& node);

  /** @brief 去掉前缀之后的局部名。 */
  std::string local_of(const pugi::xml_node& node) const;

  /** @brief 元素所在命名空间的 URI（按当前作用域解析；解析不出来是空串）。 */
  std::string uri_of(const pugi::xml_node& node) const;

  /**
   * @brief 这个元素是不是 `{uri}local`。
   *
   * **按 (URI, local) 比，不按字面前缀比** —— 见文件头。要求 `uri` 非空：
   * 一份**根本没声明命名空间**的文档在这里一律不匹配，会以 NOT_WSDL 报出去，
   * 而不是被"宽容地"当成 WSDL 收下（那种宽容会把真正的拼写错误藏起来）。
   */
  bool is(const pugi::xml_node& node, const char* uri, const char* local) const;

  /** @brief 把 QName（`tns:Foo`）按当前作用域解析成 `{uri}Foo`。 */
  std::string resolve_qname(const std::string& qname) const;

 private:
  std::vector<std::map<std::string, std::string> > scopes_;
};

/** @brief 取元素上的属性（`name` 是**限定名原样**）。不存在返回 nullptr。 */
const char* xml_attr_raw(const pugi::xml_node& node, const char* name);

/** @brief 取属性的 C 串，不存在时返回 `fallback`。 */
const char* xml_attr(const pugi::xml_node& node, const char* name,
                     const char* fallback);

/** @brief 取节点的文本（元素取子 pcdata 的拼接）。 */
std::string xml_text(const pugi::xml_node& node);

}  // namespace wsdl_detail
}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
#endif  // SRC_WSDL_UVCPP_WSDL_PUGIXML_H
