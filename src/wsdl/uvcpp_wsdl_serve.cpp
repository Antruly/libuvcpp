/**
 * @file src/wsdl/uvcpp_wsdl_serve.cpp
 * @brief WSDL 发出的那一层（实现）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <wsdl/uvcpp_wsdl_serve.h>

#if UVCPP_WSDL_ENABLE

namespace uvcpp {

const char* uvcpp_wsdl_content_type() { return "text/xml; charset=utf-8"; }

const std::string& uvcpp_wsdl_source::text() const {
  // 空源也要能返回一个可解引用的引用（调用方不该被迫先判空）。
  static const std::string k_empty;
  return text_ ? *text_ : k_empty;
}

namespace {

/**
 * @brief 真正干活的那一手：把一份共享字节交给响应。
 *
 * 单独拆出来是为了让"按共享指针捕获"那条路（路由）与"按源引用"那条路
 * （直接发）走**同一段**代码 —— 两条路各写一遍，早晚只有一条跟上改动。
 */
void send_shared(const std::shared_ptr<const std::string>& body,
                 uvcpp_web_response& resp) {
  // `set_content_type(const char*)` 那一档：省掉按值传 `const std::string&`
  // 时在调用点建的临时串（这是每个响应一条的路径）。
  resp.set_content_type(uvcpp_wsdl_content_type());
  if (body) {
    resp.body_share(body);
  } else {
    resp.body(std::string());  // 空源：明确发一个空正文，而不是留个空指针形状
  }
}

}  // namespace

void uvcpp_wsdl_send(const uvcpp_wsdl_source& src, uvcpp_web_response& resp) {
  send_shared(src.shared(), resp);
}

void uvcpp_wsdl_serve(uvcpp_web_app& app, const std::string& path,
                      const uvcpp_wsdl_source& src) {
  // ★ 按值捕获共享指针（不是捕获 src 的引用）—— 见头文件里那条语义说明。
  const std::shared_ptr<const std::string> held = src.shared();
  app.get(path, [held](uvcpp_web_request& req, uvcpp_web_response& resp,
                       uvcpp_web_next next) {
    (void)req;
    (void)next;
    send_shared(held, resp);
  });
}

void uvcpp_wsdl_serve(uvcpp_web_app& app, const std::string& path,
                      const uvcpp_wsdl_document& doc) {
  const uvcpp_wsdl_source src(doc);
  uvcpp_wsdl_serve(app, path, src);
}

}  // namespace uvcpp

#endif  // UVCPP_WSDL_ENABLE
