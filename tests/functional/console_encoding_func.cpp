/**
 * @file tests/functional/console_encoding_func.cpp
 * @brief 控制台代码页设置/恢复（`uvcpp_console`）。
 *
 * Windows 上只有“真的有控制台”时才有意义：
 * - 有控制台：设置目标代码页 → getter 读到新值 → `restore()` 回到旧值（幂等）；
 *   默认不动输入代码页；`enable_utf8()` 在已是 65001 时是 no-op。
 * - 没有控制台（CI 的 runner 服务常见）：先尝试 `AllocConsole()`；仍然没有就
 *   退 3（SKIP），不伪装成通过。
 *
 * POSIX 上这些接口必须全是空操作：getter 返回 0、设置返回 `changed=false`。
 */

#include <uvcpp/uvcpp_define.h>

#include <cstdio>
#include <string>

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

namespace {

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  [ ok ] %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what.c_str());
  }
  std::fflush(stdout);
}

#ifdef _WIN32
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
#endif

}  // namespace

int main() {
  std::printf("[console_encoding] start\n");

#ifdef _WIN32
  bool allocated = false;
  if (!console_attached()) {
    if (AllocConsole()) allocated = true;
    // AllocConsole 之后 std 句柄指向新控制台；stdout 的 CRT 缓冲可能还指着旧
    // 管道，这里只关心 Win32 控制台状态。
  }
  if (!console_attached()) {
    std::printf("SKIP: 没有可用的控制台\n");
    return 3;
  }

  const unsigned old_out = uvcpp::uvcpp_console_output_codepage();
  const unsigned old_in  = uvcpp::uvcpp_console_input_codepage();
  check(old_out != 0, "输出代码页 getter 有值");
  check(old_in != 0, "输入代码页 getter 有值");

  // cp = 0：不改。
  {
    uvcpp::uvcpp_console_encoding s =
        uvcpp::uvcpp_console_set_output_codepage(0);
    check(!s.output_changed && !s.input_changed, "cp=0 是空操作");
  }

  // 设成另一个页再还原。目标刻意避开当前值，保证真的发生改变。
  const unsigned target = (old_out == 65001) ? 437u : 65001u;
  {
    uvcpp::uvcpp_console_encoding s =
        uvcpp::uvcpp_console_set_output_codepage(target);
    check(s.output_changed, "设置目标代码页生效");
    check(uvcpp::uvcpp_console_output_codepage() == target,
          "getter 读回目标代码页");
    check(uvcpp::uvcpp_console_input_codepage() == old_in,
          "默认不动输入代码页");
    s.restore();
    check(uvcpp::uvcpp_console_output_codepage() == old_out,
          "restore 回到旧输出代码页");
    s.restore();
    check(uvcpp::uvcpp_console_output_codepage() == old_out,
          "重复 restore 无副作用");
  }

  // include_input：两个都改，restore 两个都回。
  {
    uvcpp::uvcpp_console_encoding s =
        uvcpp::uvcpp_console_set_output_codepage(target, true);
    check(s.output_changed && s.input_changed, "include_input 同时改两条");
    check(uvcpp::uvcpp_console_input_codepage() == target,
          "输入代码页也变成目标值");
    s.restore();
    check(uvcpp::uvcpp_console_input_codepage() == old_in,
          "restore 还原输入代码页");
  }

  // UTF-8 便捷入口 + RAII。
  {
    uvcpp::uvcpp_console_utf8_scope scope;
    const bool active = scope.active();
    if (old_out == 65001) {
      check(!active, "已是 UTF-8 时 scope 不报告 active");
    } else {
      check(active, "scope 在真控制台下生效");
      check(uvcpp::uvcpp_console_output_codepage() == 65001,
            "scope 期间是 65001");
    }
  }
  check(uvcpp::uvcpp_console_output_codepage() == old_out,
        "scope 析构后还原");

  if (allocated) FreeConsole();
#else
  check(uvcpp::uvcpp_console_output_codepage() == 0, "POSIX 输出 getter 返回 0");
  check(uvcpp::uvcpp_console_input_codepage() == 0, "POSIX 输入 getter 返回 0");
  uvcpp::uvcpp_console_encoding s =
      uvcpp::uvcpp_console_set_output_codepage(65001);
  check(!s.output_changed && !s.input_changed, "POSIX 设置是空操作");
  uvcpp::uvcpp_console_encoding u = uvcpp::uvcpp_console_enable_utf8();
  check(!u.output_changed, "POSIX enable_utf8 是空操作");
  uvcpp::uvcpp_console_utf8_scope scope;
  check(!scope.active(), "POSIX scope 不生效");
#endif

  if (g_fail == 0) {
    std::printf("[console_encoding] ALL PASS\n");
    return 0;
  }
  std::printf("[console_encoding] %d check(s) failed\n", g_fail);
  return 1;
}
