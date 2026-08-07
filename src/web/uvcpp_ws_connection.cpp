#include <web/uvcpp_ws_connection.h>
#if UVCPP_WEB_ENABLE
#include <cstring>
namespace uvcpp {
uvcpp_ws_connection::uvcpp_ws_connection(uvcpp_tcp_client* tcp) : tcp_(tcp) {
  parser_.set_on_frame([this](const uvcpp_ws_frame& f) { on_ws_frame(f); });
}
uvcpp_ws_connection::~uvcpp_ws_connection() {}
void uvcpp_ws_connection::start() {
  if (!tcp_) return;
  tcp_->read_stop();
  tcp_->read_start([this](uvcpp_buf* buf) { if (buf && buf->size() > 0) on_tcp_data(buf); });
}
void uvcpp_ws_connection::on_tcp_data(uvcpp_buf* buf) {
  parser_.execute(buf->get_const_data(), buf->size());
}
void uvcpp_ws_connection::on_ws_frame(const uvcpp_ws_frame& frame) {
  switch (frame.opcode) {
    case ws_opcode::TEXT: if (on_text_) on_text_(frame.payload.to_string()); break;
    case ws_opcode::BINARY: if (on_bin_) on_bin_(frame.payload.get_const_udata(), frame.payload.size()); break;
    case ws_opcode::PING: send_pong(frame.payload.get_const_data(), frame.payload.size()); if (on_ping_) on_ping_(frame.payload.get_const_udata(), frame.payload.size()); break;
    case ws_opcode::PONG: if (on_pong_) on_pong_(frame.payload.get_const_udata(), frame.payload.size()); break;
    case ws_opcode::CLOSE: { auto cd=frame.get_close_code(); auto cr=frame.get_close_reason(); send_close(cd,cr); if(on_close_) on_close_(cd,cr); break; }
    default: break;
  }
}
void uvcpp_ws_connection::send_frame(const uvcpp_ws_frame& frame, std::function<void(int)> cb) {
  if (!tcp_) { if (cb) cb(-1); return; }
  if (!started_) { started_=true; start(); }
  size_t sz=uvcpp_ws_parser::calc_frame_size(frame);
  char* buf=new char[sz];
  uvcpp_ws_parser::build_frame(buf,frame);
  int wrc=tcp_->write(buf,sz,[buf,cb](int st){ delete[] buf; if(cb) cb(st); });
  if(wrc!=0){ delete[] buf; if(cb) cb(wrc); }
}
int uvcpp_ws_connection::send_text(const char* d, size_t n, std::function<void(int)> cb){ uvcpp_ws_frame f; f.opcode=ws_opcode::TEXT; f.payload.clone_data(d,n); send_frame(f,cb); return 0; }
int uvcpp_ws_connection::send_binary(const char* d, size_t n, std::function<void(int)> cb){ uvcpp_ws_frame f; f.opcode=ws_opcode::BINARY; f.payload.clone_data(d,n); send_frame(f,cb); return 0; }
int uvcpp_ws_connection::send_ping(const char* d, size_t n){ uvcpp_ws_frame f; f.opcode=ws_opcode::PING; if(d&&n>0) f.payload.clone_data(d,n); send_frame(f,nullptr); return 0; }
int uvcpp_ws_connection::send_pong(const char* d, size_t n){ uvcpp_ws_frame f; f.opcode=ws_opcode::PONG; if(d&&n>0) f.payload.clone_data(d,n); send_frame(f,nullptr); return 0; }
int uvcpp_ws_connection::send_close(ws_close_code cd, const std::string& rs){ uvcpp_ws_frame f; f.opcode=ws_opcode::CLOSE; f.set_close_payload(cd,rs); send_frame(f,[this](int){ if(tcp_&&tcp_->get_tcp()) tcp_->get_tcp()->close([](uvcpp_handle*){}); }); return 0; }
void uvcpp_ws_connection::on_text(std::function<void(const std::string&)> cb){ on_text_=std::move(cb); }
void uvcpp_ws_connection::on_binary(std::function<void(const uint8_t*,size_t)> cb){ on_bin_=std::move(cb); }
void uvcpp_ws_connection::on_ping(std::function<void(const uint8_t*,size_t)> cb){ on_ping_=std::move(cb); }
void uvcpp_ws_connection::on_pong(std::function<void(const uint8_t*,size_t)> cb){ on_pong_=std::move(cb); }
void uvcpp_ws_connection::on_close(std::function<void(ws_close_code,const std::string&)> cb){ on_close_=std::move(cb); }
uvcpp_tcp_client* uvcpp_ws_connection::get_tcp_client(){ return tcp_; }
} // namespace uvcpp
#endif
