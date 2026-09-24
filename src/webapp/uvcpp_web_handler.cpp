/**
 * @file src/webapp/uvcpp_web_handler.cpp
 * @brief `uvcpp_web_next::operator()` 的实现。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么单独一个 TU：`operator()` 要调 `uvcpp_web_context::next_resume_chain()`，
 * 那需要上下文的完整定义；而 `uvcpp_web_context.h` 反过来包含本头（handler 的
 * 签名是上下文的一部分）⇒ 放头里就是循环包含。放 .cpp 里包含顺序就无所谓了。
 */

#include <webapp/uvcpp_web_handler.h>

#include <functional>

#include <webapp/uvcpp_web_context.h>

namespace uvcpp {

void uvcpp_web_next::operator()() const {
  // 框架形态：续跑链（loop 线程上就地跑、跨线程投回去，见 next_resume_chain()）。
  if (ctx_ != nullptr) {
    ctx_->next_resume_chain();
    return;
  }

  // 手造形态。空 next 的抛法与原来的 `std::function<void()>` 逐字一致。
  if (!fn_) throw std::bad_function_call();
  fn_();
}

}  // namespace uvcpp
