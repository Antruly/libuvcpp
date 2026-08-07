#include <iostream>
#include <cstring>
#include <string>
#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE
#include <web/uvcpp_http_server.h>
using namespace uvcpp;

// Test 1: Server construction, bind, listen, status
static bool test_server_lifecycle() {
  uvcpp_http_server server;
  if (server.get_status() != HTTP_SERVER_NONE) return false;
  server.bind("127.0.0.1", 20001);
  server.listen();
  if (!server.has_status(HTTP_SERVER_LISTENING)) return false;
  return true;
}

// Test 2: Route registration (no network)
static bool test_route_registration() {
  uvcpp_http_server server;
  server.bind("127.0.0.1", 20002);

  bool get_called = false;
  server.get("/hello", [&](uvcpp_http_request&, uvcpp_http_response& resp, uvcpp_tcp_client*) {
    get_called = true;
    resp = uvcpp_http_response::ok("hi", 2);
  });

  server.post("/data", [&](uvcpp_http_request&, uvcpp_http_response& resp, uvcpp_tcp_client*) {
    resp = uvcpp_http_response::ok("posted", 6);
  });

  // Routes are stored — can't easily test without real connection,
  // but at least verify no crash during registration
  (void)get_called;
  return true;
}

// Test 3: HTTP response serialization
static bool test_response_format() {
  auto resp = uvcpp_http_response::ok("body", 4);
  resp.set_header("x-test", "value");

  std::string wire = resp.to_string();
  if (wire.find("HTTP/1.1 200") == std::string::npos) return false;
  if (wire.find("content-length: 4") == std::string::npos) return false;
  if (wire.find("x-test: value") == std::string::npos) return false;
  if (wire.find("\r\n\r\nbody") == std::string::npos) return false;
  return true;
}

// Test 4: Status code responses
static bool test_status_codes() {
  auto ok = uvcpp_http_response::ok("ok", 2);
  if (ok.status_code != http_status::OK) return false;

  auto nf = uvcpp_http_response::not_found();
  if (nf.status_code != http_status::NOT_FOUND) return false;

  auto se = uvcpp_http_response::server_error();
  if (se.status_code != http_status::INTERNAL_SERVER_ERROR) return false;

  return true;
}

int main() {
  bool ok = true;
  struct { const char* name; bool (*fn)(); } tests[] = {
    {"server_lifecycle", test_server_lifecycle},
    {"route_registration", test_route_registration},
    {"response_format", test_response_format},
    {"status_codes", test_status_codes},
  };
  for (const auto& t : tests) {
    std::cout << "[web_http_server] " << t.name << "\n";
    bool r = t.fn();
    std::cout << "  -> " << (r ? "PASS" : "FAIL") << "\n";
    ok = r && ok;
  }
  std::cout << "[web_http_server] " << (ok ? "ALL PASS" : "FAIL") << "\n";
  return ok ? 0 : 2;
}
#else
int main() { return 0; }
#endif
