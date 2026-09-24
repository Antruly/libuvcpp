/**
 * @file src/wsdl/uvcpp_wsdl_serve.h
 * @brief 把 WSDL 发出去：一份**可共享**的文本 + 一条 GET 路由。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这一层只有两件事
 * ----------------
 * 1. **序列化一次、发多次。** `uvcpp_wsdl_source` 持有一个
 *    `shared_ptr<const std::string>`，`uvcpp_wsdl_send` 把它**按引用**交给响应
 *    （`body_share`），所以 N 个客户端拿到的是**同一块字节**，不是 N 份拷贝。
 *    WSDL 是典型的"启动时定好、之后只读"的资源，这条正是它该走的路。
 * 2. **一条 GET 路由。** `uvcpp_wsdl_serve` 在 app 上装路由。
 *
 * ★ **路由拿的是注册那一刻的共享指针**（按值捕获）。所以：
 *
 *   * 注册完 `uvcpp_wsdl_source` 那个对象**可以立刻析构**，字节仍活着；
 *   * 反过来，注册**之后**再 `set_from_document()` / `set_from_text()` 改它，
 *     **已经装好的那条路由不会跟着变**。这条是刻意的：一条运行时被换掉 body
 *     的路由，行为取决于"请求到达时是哪一版"，那种不确定性比"要么两版都能
 *     工作、要么都不工作"糟得多。
 *
 * ★ **想换内容，别指望"再 `uvcpp_wsdl_serve()` 一次"** —— 路由表里**同一个
 * 模式注册两次是静默无效的**：`uvcpp_web_router::match()` 在特异度打平时
 * 保留**先注册**的那条（`src/webapp/uvcpp_web_router.cpp` 里"compare_
 * specificity 是全序、所以结果与注册顺序无关"说的是**不同模式之间**；同一个
 * 模式打平之后只剩注册序）。换内容的正当路子有两条：
 *
 *   1. **换路径**（`/svc.wsdl` → `/svc2.wsdl`），然后在服务端做重定向；
 *   2. **自己写 handler**，在里头调 `uvcpp_wsdl_send(src, resp)` —— 那条路在
 *      **每次请求时**读源，所以 `set_from_*()` 是对它生效的。代价是：源被
 *      循环线程读的同时你在别的线程写它，那是**数据竞争**，要改就得先停机
 *      或在自己的同步下改。
 *
 * `tests/functional/web_app_wsdl_func.cpp` 第 2 组把这两条都钉着（含一条
 * "新路径拿同一个源必须发新内容"的正对照）。
 *
 * 状态码与 content-type
 * ----------------------
 * 发出去的是 **200 + `text/xml; charset=utf-8`**，正文就是 `text()` 里的字节；
 * **不做** 404/500 之类的代替决定（源是空的就发空正文）。这一层不替调用方
 * 判断"这份 WSDL 该不该给人看"。
 */

#pragma once
#ifndef SRC_WSDL_UVCPP_WSDL_SERVE_H
#define SRC_WSDL_UVCPP_WSDL_SERVE_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WSDL_ENABLE

#include <cstddef>
#include <memory>
#include <string>

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_response.h>

#include <wsdl/uvcpp_wsdl_document.h>

namespace uvcpp {

/** @return `"text/xml; charset=utf-8"` —— WSDL 的老约定，兼容面最大。 */
const char* uvcpp_wsdl_content_type();

/**
 * @brief 一份**已序列化**的 WSDL 文本。
 *
 * 拷贝这个对象不拷贝字节（内部是 `shared_ptr`）。
 */
class uvcpp_wsdl_source {
 public:
  /** 空源（还没装内容）。 */
  uvcpp_wsdl_source() {}

  /** 立刻按模型序列化一份（`uvcpp_wsdl_dump`）。 */
  explicit uvcpp_wsdl_source(const uvcpp_wsdl_document& doc) {
    set_from_document(doc);
  }

  /** 直接收下已经序列化好的文本（"手上就是一份 .wsdl 文件"那条路）。 */
  explicit uvcpp_wsdl_source(const std::string& xml) { set_from_text(xml); }

  bool empty() const { return !text_ || text_->empty(); }

  size_t size() const { return text_ ? text_->size() : 0u; }

  /** @return 文本；空源时是一块空的静态串（不是 nullptr）。 */
  const std::string& text() const;

  /** @return 可以直接交给 `body_share()` 的共享指针；空源时是空的。 */
  const std::shared_ptr<const std::string>& shared() const { return text_; }

  /** 重新 dump 一遍（换掉内容）。 */
  void set_from_document(const uvcpp_wsdl_document& doc) {
    text_ = std::make_shared<const std::string>(uvcpp_wsdl_dump(doc));
  }

  /** 换掉内容。 */
  void set_from_text(const std::string& xml) {
    text_ = std::make_shared<const std::string>(xml);
  }

 private:
  std::shared_ptr<const std::string> text_;
};

/** @brief 把这个 WSDL 作为响应发出去：200 + `text/xml; charset=utf-8`。 */
void uvcpp_wsdl_send(const uvcpp_wsdl_source& src, uvcpp_web_response& resp);

/**
 * @brief 在 app 上装一条 GET 路由。
 *
 * @param path 精确路径（路由自己的匹配规则由 `uvcpp_web_app` 决定）。
 * @note **重复注册同一路径不会生效**（路由表打平时先注册的赢，见文件头那一段）
 *       —— 所以这个函数是"一次性的"，别拿它当"重新装载"用。
 */
void uvcpp_wsdl_serve(uvcpp_web_app& app, const std::string& path,
                      const uvcpp_wsdl_source& src);

/** @brief 同上，但先把模型 dump 一次再装（省掉显式造 source 的那一步）。 */
void uvcpp_wsdl_serve(uvcpp_web_app& app, const std::string& path,
                      const uvcpp_wsdl_document& doc);

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
#endif  // SRC_WSDL_UVCPP_WSDL_SERVE_H
