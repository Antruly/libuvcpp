/**
 * @file tests/functional/web_app_service_scm_func.cpp
 * @brief Windows 服务（SCM）真装/真启/真停/真卸的用例（issue #73）。
 *
 * 默认不注册：要 `-DUVCPP_TEST_SERVICE_SCM=ON` 才编（CI 只开在 MSVC 的 `web`
 * 那一格）。原因是它会在测试机器上**真的创建、启动、终止、删除一个 Windows
 * 服务** —— 没有管理员权限的机器上它只能退 3（SKIP）。GitHub 的 Windows
 * runner 是管理员，所以那一格量到的是真行为，不是"编过了"。
 *
 * 判据（每一条都对着 issue 的验收项）：
 *
 * 1. `install` → `start` → 服务进程写出实际端口，`/hello` 能被父进程访问；
 * 2. **崩溃后 SCM 拉起**：`TerminateProcess` 杀掉服务进程，等 SCM 按
 *    `SC_ACTION_RESTART` 起一个新的（pid 变化、端口重新可用）；
 * 3. `stop` → 服务进程优雅收场并写出 stopped 标记；
 * 4. `uninstall` 之后 `status` 打不到这个服务（真的删掉了）。
 *
 * 父进程与子进程是**同一个可执行文件**：SCM 用 `"<exe>" run --name ... --config
 * <状态文件>` 拉起它，`main()` 认出 `argv[1] == "run"` 就走服务模式；没有参数
 * 的那一次才是本用例的父进程。
 */

#include <uvcpp/uvcpp_define.h>

#include <cstdio>

#if UVCPP_WEBAPP_ENABLE && defined(_WIN32)

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <uv.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <webapp/uvcpp_service.h>
#include <webapp/uvcpp_web_app.h>

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

void sleep_ms(int ms) { Sleep(static_cast<DWORD>(ms)); }

std::string read_file(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return out;
  char buf[256];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

std::string temp_path(const std::string& leaf) {
  // **不能用 `uv_os_tmpdir()`**：服务进程跑在 LocalSystem 下，它未必能写
  // 当前用户的临时目录。`%WINDIR%\Temp` 两边都写得了。
  char win[MAX_PATH];
  const UINT n = GetWindowsDirectoryA(win, MAX_PATH);
  std::string base = (n > 0) ? std::string(win, n) + "\\Temp" : std::string(".");
  return base + "\\" + leaf;
}

/// 状态文件内容：`<pid> <port>\n`（父进程据此知道"新实例真的起来了"）。
bool parse_state(const std::string& text, unsigned long& pid, int& port) {
  unsigned long p = 0;
  int           q = 0;
  if (std::sscanf(text.c_str(), "%lu %d", &p, &q) != 2) return false;
  if (p == 0 || q <= 0) return false;
  pid  = p;
  port = q;
  return true;
}

bool wait_state(const std::string& path, unsigned long expect_pid, int& port,
                int timeout_ms) {
  const int steps = timeout_ms / 100;
  for (int i = 0; i < steps; ++i) {
    unsigned long pid = 0;
    int           p   = 0;
    if (parse_state(read_file(path), pid, p) &&
        (expect_pid == 0 || pid == expect_pid)) {
      port = p;
      return true;
    }
    sleep_ms(100);
  }
  return false;
}

int http_get(int port, std::string& out) {
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return -1;
  DWORD tv = 8000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
             sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
             sizeof(tv));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(static_cast<u_short>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::closesocket(s);
    return -2;
  }
  static const char kReq[] =
      "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (::send(s, kReq, static_cast<int>(sizeof(kReq) - 1), 0) <= 0) {
    ::closesocket(s);
    return -3;
  }
  out.clear();
  char buf[2048];
  int  n = 0;
  while ((n = ::recv(s, buf, sizeof(buf), 0)) > 0) out.append(buf, n);
  ::closesocket(s);
  return out.empty() ? -4 : 0;
}

bool query_state(SC_HANDLE scm, const std::wstring& name,
                 SERVICE_STATUS_PROCESS& st) {
  SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_STATUS);
  if (svc == nullptr) return false;
  DWORD needed = 0;
  const BOOL ok =
      QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                           reinterpret_cast<LPBYTE>(&st), sizeof(st), &needed);
  CloseServiceHandle(svc);
  return ok != FALSE;
}

bool wait_service_pid(SC_HANDLE scm, const std::wstring& name,
                      unsigned long expect_pid, int timeout_ms,
                      SERVICE_STATUS_PROCESS& st) {
  const int steps = timeout_ms / 200;
  for (int i = 0; i < steps; ++i) {
    SERVICE_STATUS_PROCESS cur;
    std::memset(&cur, 0, sizeof(cur));
    if (query_state(scm, name, cur) && cur.dwCurrentState == SERVICE_RUNNING &&
        (expect_pid == 0 || cur.dwProcessId == expect_pid)) {
      st = cur;
      return true;
    }
    sleep_ms(200);
  }
  return false;
}

std::wstring widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0],
                      n);
  return w;
}

void write_file(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}

/// 子进程（SCM 拉起的那个）：
///   `app.exe run --name N --config <状态文件>`
int service_child_main(int argc, char** argv) {
  uvcpp::uvcpp_web_app app;
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(uvcpp::log_level::WARN);
  app.get("/hello",
          [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
             uvcpp::uvcpp_web_next) {
            resp.text("hello");
            resp.end();
          });

  uvcpp::uvcpp_service_options opts;
  opts.single_instance = true;
  opts.stop_grace_ms   = 8000;
  opts.on_started      = [&app](uvcpp::uvcpp_web_app&) {
    // `--config` 在这个用例里被借用成"状态文件路径"：服务一起来就把自己的
    // pid 和真实端口写进去，父进程据此判断"真的在服务"。
    const std::string stat = uvcpp::uvcpp_service_config_path();
    if (stat.empty()) return;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu %d\n",
                  static_cast<unsigned long>(uv_os_getpid()),
                  app.bound_port());
    write_file(stat, buf);
  };

  const int rc = uvcpp::uvcpp_service_run(app, opts, argc, argv);
  const std::string stat = uvcpp::uvcpp_service_config_path();
  if (!stat.empty()) write_file(stat + ".stopped", "1\n");
  return rc;
}

/// 父进程：所有 SCM 操作都走公开的 `uvcpp_service_run()` 子命令。
int scm_command(const char* command, const std::string& name,
                const std::string& stat, char** argv0) {
  uvcpp::uvcpp_web_app dummy;
  char* argv[8];
  int   argc = 0;
  argv[argc++] = argv0[0];
  argv[argc++] = const_cast<char*>(command);
  argv[argc++] = const_cast<char*>("--name");
  argv[argc++] = const_cast<char*>(name.c_str());
  if (!stat.empty()) {
    argv[argc++] = const_cast<char*>("--config");
    argv[argc++] = const_cast<char*>(stat.c_str());
  }
  argv[argc] = nullptr;
  return uvcpp::uvcpp_service_run(dummy, argc, argv);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "run") == 0) {
    return service_child_main(argc, argv);
  }

  std::printf("[service_scm] issue #73（Windows 服务）\n");

  // 没有管理员权限就没有 SCM 写权限：这是"未判定"，不是"测过了"。
  SC_HANDLE probe = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (probe == nullptr) {
    std::printf("SKIP: 没有管理员权限（OpenSCManager=%lu）\n",
                static_cast<unsigned long>(GetLastError()));
    return 3;
  }
  CloseServiceHandle(probe);

  WSADATA wsa;
  check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup");

  char namebuf[64];
  std::snprintf(namebuf, sizeof(namebuf), "uvcppscm%lu",
                static_cast<unsigned long>(GetCurrentProcessId()));
  const std::string name = namebuf;
  const std::wstring wname = widen(name);
  const std::string stat = temp_path("uvcpp_scm_" + name + ".stat");
  ::DeleteFileA((stat + ".stopped").c_str());
  ::DeleteFileA(stat.c_str());

  check(scm_command("install", name, stat, argv) == 0, "install 成功");

  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
  check(scm != nullptr, "父进程打开 SCM");

  int  port1 = 0;
  bool started = false;
  if (scm != nullptr) {
    check(scm_command("start", name, stat, argv) == 0, "start 成功");
    started = wait_state(stat, 0, port1, 30000);
    check(started, "服务进程写出 pid/端口（真的起来了）");
    if (started) {
      std::string body;
      check(http_get(port1, body) == 0, "访问服务 /hello");
      check(body.find("200 OK") != std::string::npos, "状态行 200");
      check(body.find("hello") != std::string::npos, "body = hello");
    }
  }

  // ---- 崩溃后 SCM 拉起 --------------------------------------------------
  if (scm != nullptr && started) {
    SERVICE_STATUS_PROCESS st;
    std::memset(&st, 0, sizeof(st));
    const bool got = wait_service_pid(scm, wname, 0, 10000, st);
    check(got, "查到服务进程 pid");
    if (got) {
      const unsigned long old_pid = st.dwProcessId;
      HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, st.dwProcessId);
      check(h != nullptr, "打开服务进程");
      if (h != nullptr) {
        check(TerminateProcess(h, 1) != FALSE, "强杀服务进程（模拟崩溃）");
        CloseHandle(h);
      }
      // 失败动作是 5 秒后重启：给足余量。**必须等 pid 变化** —— 崩溃那一瞬
      // 旧实例在 SCM 眼里还可能报着 RUNNING，不能把那个当成"拉起来了"。
      SERVICE_STATUS_PROCESS st2;
      std::memset(&st2, 0, sizeof(st2));
      bool restarted = false;
      for (int i = 0; i < 225; ++i) {  // 45s，200ms 一拍
        SERVICE_STATUS_PROCESS cur;
        std::memset(&cur, 0, sizeof(cur));
        if (query_state(scm, wname, cur) &&
            cur.dwCurrentState == SERVICE_RUNNING &&
            cur.dwProcessId != 0 && cur.dwProcessId != old_pid) {
          st2       = cur;
          restarted = true;
          break;
        }
        sleep_ms(200);
      }
      check(restarted, "SCM 按失败动作重新拉起了服务（pid 变化）");
      if (restarted) {
        int port2 = 0;
        const bool up = wait_state(stat, st2.dwProcessId, port2, 15000);
        check(up, "新实例写出 pid/端口");
        if (up) {
          std::string body;
          check(http_get(port2, body) == 0 && body.find("hello") != std::string::npos,
                "重启后的服务仍能应答 /hello");
        }
      }
    }
  }

  // ---- 优雅停止 ---------------------------------------------------------
  if (scm != nullptr && started) {
    check(scm_command("stop", name, stat, argv) == 0, "stop 成功");
    bool stopped = false;
    for (int i = 0; i < 300; ++i) {
      if (GetFileAttributesA((stat + ".stopped").c_str()) !=
          INVALID_FILE_ATTRIBUTES) {
        stopped = true;
        break;
      }
      sleep_ms(100);
    }
    check(stopped, "服务进程走完优雅停机并写出 stopped 标记");
  }

  // ---- 卸载 -------------------------------------------------------------
  check(scm_command("uninstall", name, stat, argv) == 0, "uninstall 成功");
  check(scm_command("status", name, stat, argv) != 0,
        "卸载之后 status 打不到它（真的删掉了）");

  if (scm != nullptr) CloseServiceHandle(scm);
  DeleteFileA(stat.c_str());
  DeleteFileA((stat + ".stopped").c_str());
  WSACleanup();

  if (g_fail == 0) {
    std::printf("[service_scm] ALL PASS\n");
    return 0;
  }
  std::printf("[service_scm] %d check(s) failed\n", g_fail);
  return 1;
}

#else  // !(webapp && _WIN32)

int main() {
  std::printf("SKIP: webapp 关着或不是 Windows\n");
  return 3;
}

#endif
