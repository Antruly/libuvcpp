#include <iostream>
#include <cstring>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>
using namespace uvcpp;

static bool test_text_frame() {
  uvcpp_ws_parser p;
  // FIN(1) | opcode=TEXT(1) = 0x81, MASK=0 | len=5 = 0x05, "hello"
  uint8_t raw[] = {0x81, 0x05, 'h', 'e', 'l', 'l', 'o'};
  size_t n = p.execute((const char*)raw, sizeof(raw));
  if (n != sizeof(raw)) return false;
  if (p.get_state() != ws_parser_state::COMPLETE) return false;
  auto& f = p.get_current_frame();
  return f.fin && f.opcode == ws_opcode::TEXT && !f.masked &&
         f.payload.to_string() == "hello";
}

static bool test_masked_frame() {
  uvcpp_ws_parser p;
  // FIN(1) | TEXT(1) = 0x81, MASK(1) | len=3 = 0x83
  // mask key = {0x12, 0x34, 0x56, 0x78}
  // masked payload = "abc" XOR mask key
  uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
  char abc[3] = {'a', 'b', 'c'};
  char masked[3];
  for (int i = 0; i < 3; i++) masked[i] = abc[i] ^ key[i % 4];

  uint8_t raw[] = {0x81, 0x83, key[0], key[1], key[2], key[3],
                    (uint8_t)masked[0], (uint8_t)masked[1], (uint8_t)masked[2]};
  size_t n = p.execute((const char*)raw, sizeof(raw));
  if (n != sizeof(raw)) return false;
  if (p.get_state() != ws_parser_state::COMPLETE) return false;
  auto& f = p.get_current_frame();
  return f.fin && f.masked && f.payload.to_string() == "abc";
}

static bool test_ping_frame() {
  uvcpp_ws_parser p;
  // FIN(1) | PING(9) = 0x89, len=4 = 0x04, "ping"
  uint8_t raw[] = {0x89, 0x04, 'p', 'i', 'n', 'g'};
  size_t n = p.execute((const char*)raw, sizeof(raw));
  if (n != sizeof(raw)) return false;
  auto& f = p.get_current_frame();
  return f.opcode == ws_opcode::PING && f.payload.to_string() == "ping" &&
         f.is_control_frame();
}

static bool test_close_frame() {
  uvcpp_ws_parser p;
  // FIN(1) | CLOSE(8) = 0x88, len=2 = 0x02, close code 1000
  uint8_t raw[] = {0x88, 0x02, 0x03, 0xE8}; // 0x03E8 = 1000
  size_t n = p.execute((const char*)raw, sizeof(raw));
  if (n != sizeof(raw)) return false;
  auto& f = p.get_current_frame();
  return f.opcode == ws_opcode::CLOSE &&
         f.get_close_code() == ws_close_code::NORMAL;
}

static bool test_large_frame() {
  // Build a frame with extended length (126 <= len <= 0xFFFF)
  std::string body(200, 'x');
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::BINARY;
  f.payload.clone_data(body.c_str(), body.size());

  size_t sz = uvcpp_ws_parser::calc_frame_size(f);
  char* buf = new char[sz];
  size_t written = uvcpp_ws_parser::build_frame(buf, f);

  // Parse it back
  uvcpp_ws_parser p;
  size_t n = p.execute(buf, written);
  delete[] buf;

  if (n != written) return false;
  auto& pf = p.get_current_frame();
  return pf.opcode == ws_opcode::BINARY &&
         pf.payload.size() == 200 &&
         pf.payload.to_string() == body;
}

static bool test_frame_roundtrip() {
  // Build → parse → verify
  uvcpp_ws_frame f;
  f.opcode = ws_opcode::TEXT;
  f.masked = true;
  f.mask_key[0] = 0xAA; f.mask_key[1] = 0xBB;
  f.mask_key[2] = 0xCC; f.mask_key[3] = 0xDD;
  f.payload.clone_data("roundtrip", 9);

  size_t sz = uvcpp_ws_parser::calc_frame_size(f);
  char* buf = new char[sz];
  size_t written = uvcpp_ws_parser::build_frame(buf, f);

  uvcpp_ws_parser p;
  p.execute(buf, written);
  delete[] buf;

  auto& pf = p.get_current_frame();
  return pf.opcode == ws_opcode::TEXT && pf.masked &&
         pf.payload.to_string() == "roundtrip";
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"text_frame", test_text_frame},
    {"masked_frame", test_masked_frame},
    {"ping_frame", test_ping_frame},
    {"close_frame", test_close_frame},
    {"large_frame", test_large_frame},
    {"frame_roundtrip", test_frame_roundtrip},
  };
  for (const auto& t : tests) {
    std::cout << "[web_ws_parser] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_ws_parser] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
#else
int main() { return 0; }
#endif
