/**
 * @file src/webapp/uvcpp_web_ws.cpp
 * @brief WebSocket 请求视图的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_ws.h>

namespace uvcpp {

uvcpp_web_ws_request::uvcpp_web_ws_request(uvcpp_http_request& raw)
    : conn_(nullptr) {
  // **搬走的是 body，不是整个请求。** 升级请求按 RFC 6455 §4.1 是 GET，
  // 正常没有 body，所以这一句在实践中不搬任何字节；留着它是为了万一有
  // body 时不留一份拷贝在已经没人读的地方。
  req_.take_from(raw);
}

uvcpp_web_ws_request::~uvcpp_web_ws_request() {}

// 纯虚接口的析构：必须有**一个**带外键的锚点，否则每个用到本头的 TU 都会
// 各自生成一份 vtable/typeinfo，链接期还得靠 COMDAT 去重（跨 DSO 时不保证）。
uvcpp_web_ws_session::~uvcpp_web_ws_session() {}

}  // namespace uvcpp
