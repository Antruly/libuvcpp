/**
 * @file src/uvcpp/uvcpp_console.cpp
 * @brief 控制台代码页设置的实现（Windows 真操作，POSIX 空操作）。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <uvcpp/uvcpp_console.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace uvcpp {

uvcpp_console_encoding::uvcpp_console_encoding()
    : output_changed(false),
      input_changed(false),
      old_output_cp(0),
      old_input_cp(0) {}

#ifdef _WIN32

namespace {

bool console_attached() {
  const DWORD streams[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (int i = 0; i < 2; ++i) {
    HANDLE h = GetStdHandle(streams[i]);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) continue;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) return true;
  }
  return false;
}

}  // namespace

void uvcpp_console_encoding::restore() {
  if (output_changed && old_output_cp != 0) {
    SetConsoleOutputCP(old_output_cp);
    output_changed = false;
  }
  if (input_changed && old_input_cp != 0) {
    SetConsoleCP(old_input_cp);
    input_changed = false;
  }
}

uvcpp_console_encoding uvcpp_console_set_output_codepage(
    unsigned cp, bool include_input) {
  uvcpp_console_encoding out;
  if (cp == 0 || !console_attached()) return out;

  const unsigned cur_out = static_cast<unsigned>(GetConsoleOutputCP());
  if (cur_out != cp) {
    if (SetConsoleOutputCP(cp) != 0) {
      out.old_output_cp  = cur_out;
      out.output_changed = true;
    }
  }

  if (include_input) {
    const unsigned cur_in = static_cast<unsigned>(GetConsoleCP());
    if (cur_in != cp) {
      if (SetConsoleCP(cp) != 0) {
        out.old_input_cp  = cur_in;
        out.input_changed = true;
      }
    }
  }
  return out;
}

unsigned uvcpp_console_output_codepage() {
  return static_cast<unsigned>(GetConsoleOutputCP());
}

unsigned uvcpp_console_input_codepage() {
  return static_cast<unsigned>(GetConsoleCP());
}

#else  // !_WIN32

void uvcpp_console_encoding::restore() {
  output_changed = false;
  input_changed  = false;
}

uvcpp_console_encoding uvcpp_console_set_output_codepage(unsigned, bool) {
  // 终端编码由 locale / 终端本身决定；库不碰进程全局 locale。
  return uvcpp_console_encoding();
}

unsigned uvcpp_console_output_codepage() { return 0; }
unsigned uvcpp_console_input_codepage() { return 0; }

#endif  // _WIN32

uvcpp_console_encoding uvcpp_console_enable_utf8(bool include_input) {
  return uvcpp_console_set_output_codepage(65001, include_input);
}

uvcpp_console_utf8_scope::uvcpp_console_utf8_scope(bool include_input)
    : state_(uvcpp_console_enable_utf8(include_input)) {}

uvcpp_console_utf8_scope::~uvcpp_console_utf8_scope() { state_.restore(); }

}  // namespace uvcpp
