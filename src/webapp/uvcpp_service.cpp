/**
 * @file src/webapp/uvcpp_service.cpp
 * @brief 服务化部署支持（issue #73）：Windows 服务封装 + 前台运行。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 三条路共用同一段"启动顺序"（`start_application`）：
 *
 *   工作目录 → 环境变量 → 日志重定向 → 单实例锁 → configure → set_grace
 *   → `app.start()` → `on_started`
 *
 * 差别只在"谁来告诉我们停机"：前台模式是一个跑信号的 `uvcpp_loop`，Windows
 * 服务模式是 SCM 的 STOP/SHUTDOWN 控制（一个 `HANDLE` 事件）。两边的收尾都是
 * `app.stop()` + `app.join()` —— 优雅停机本身在 `uvcpp_web_app` 里，本文件
 * 不重复实现。
 */

#include <webapp/uvcpp_service.h>

#if UVCPP_WEBAPP_ENABLE

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_signal.h>
#include <handle/uvcpp_timer.h>
#include <uvcpp/uvcpp_define.h>
#include <webapp/uvcpp_web_app.h>

#include <uv.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace uvcpp {

namespace {

std::string g_config_path;
bool        g_is_service = false;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string exe_path() {
  char buf[4096];
  std::size_t size = sizeof(buf);
  if (uv_exepath(buf, &size) != 0 || size == 0) return std::string();
  return std::string(buf, size);
}

std::string exe_basename() {
  std::string p = exe_path();
  if (p.empty()) return "uvcpp_service";

  const std::size_t slash = p.find_last_of("/\\");
  if (slash != std::string::npos) p = p.substr(slash + 1);
  const std::size_t dot = p.find_last_of('.');
  if (dot != std::string::npos && dot > 0) p = p.substr(0, dot);
  return p.empty() ? std::string("uvcpp_service") : p;
}

std::string effective_name(const uvcpp_service_options& o) {
  return o.name.empty() ? exe_basename() : o.name;
}

std::string to_lower(std::string s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = static_cast<char>(s[i] + 32);
  }
  return s;
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      &w[0], n);
  return w;
}

std::string narrow(const wchar_t* s) {
  if (s == nullptr) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr,
                                    nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
  return out;
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// 日志重定向 + 轮转
// ---------------------------------------------------------------------------

class log_sink {
 public:
  log_sink() : fd_(-1), max_(0), files_(0) {}
  ~log_sink() { close(); }

  bool open(const uvcpp_service_options& o) {
    path_  = o.log_file;
    max_   = o.log_max_bytes;
    files_ = o.log_max_files;
    if (path_.empty()) return true;
    // 上次退出时已经写满：先轮转再接管，别让第一行就顶破上限。
    if (max_ > 0 && files_ > 0 && file_size(path_) >= (long long)max_) {
      shift();
    }
    return reopen();
  }

  void rotate_if_needed() {
    if (path_.empty() || max_ == 0 || files_ <= 0 || fd_ < 0) return;
    if (file_size(path_) < (long long)max_) return;
    std::fflush(stdout);
    std::fflush(stderr);
    close_fd_only();
    shift();
    reopen();
  }

  void close() { close_fd_only(); }

 private:
  static std::string numbered(const std::string& p, int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), ".%d", i);
    return p + buf;
  }

  bool reopen() {
    int fd = -1;
#ifdef _WIN32
    const std::wstring wp = widen(path_);
    fd = _wopen(wp.c_str(), _O_CREAT | _O_WRONLY | _O_APPEND | _O_BINARY,
                _S_IREAD | _S_IWRITE);
#else
    fd = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
#endif
    if (fd < 0) return false;
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    _dup2(fd, 1);
    _dup2(fd, 2);
#else
    ::dup2(fd, 1);
    ::dup2(fd, 2);
#endif
    fd_ = fd;  // 1/2 是 dup 出来的，原 fd 留着方便整份关掉/统计
    return true;
  }

  void shift() {
    // `x.log.1` 是最新的一份，最老的一份被挤掉。
    for (int i = files_; i >= 1; --i) {
      const std::string from =
          (i == 1) ? path_ : numbered(path_, i - 1);
      const std::string to = numbered(path_, i);
      if (file_size(from) < 0) continue;
#ifdef _WIN32
      MoveFileExW(widen(from).c_str(), widen(to).c_str(),
                  MOVEFILE_REPLACE_EXISTING);
#else
      ::rename(from.c_str(), to.c_str());
#endif
    }
  }

  static long long file_size(const std::string& p) {
#ifdef _WIN32
    struct _stat64 st;
    if (_wstat64(widen(p).c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
#endif
  }

  void close_fd_only() {
    if (fd_ < 0) return;
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    _close(fd_);
#else
    ::close(fd_);
#endif
    fd_ = -1;
  }

  std::string path_;
  int         fd_;
  std::size_t max_;
  int         files_;
};

// ---------------------------------------------------------------------------
// 单实例锁
// ---------------------------------------------------------------------------

class instance_lock {
 public:
#ifdef _WIN32
  instance_lock() : handle_(nullptr) {}
#else
  instance_lock() : fd_(-1) {}
#endif

  ~instance_lock() { release(); }

  int acquire(const std::string& name) {
#ifdef _WIN32
    const std::wstring wname =
        widen(std::string("Local\\uvcpp_service_") + name);
    handle_ = CreateMutexW(nullptr, TRUE, wname.c_str());
    if (handle_ == nullptr) return uv_translate_sys_error(GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(handle_);
      handle_ = nullptr;
      return UV_EBUSY;
    }
    return 0;
#else
    char tmp[1024];
    std::size_t size = sizeof(tmp);
    std::string dir = "/tmp";
    if (uv_os_tmpdir(tmp, &size) == 0) dir = std::string(tmp, size);
    path_ = dir + "/uvcpp_service_" + name + ".lock";
    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd_ < 0) return uv_translate_sys_error(errno);
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const int e = errno;
      ::close(fd_);
      fd_ = -1;
      return e == EWOULDBLOCK ? UV_EBUSY : uv_translate_sys_error(e);
    }
    char pid[32];
    const int n = std::snprintf(pid, sizeof(pid), "%d\n",
                                static_cast<int>(uv_os_getpid()));
    if (::ftruncate(fd_, 0) == 0 && n > 0) {
      ssize_t ignored = ::write(fd_, pid, static_cast<std::size_t>(n));
      (void)ignored;
    }
    return 0;
#endif
  }

  void release() {
#ifdef _WIN32
    if (handle_ != nullptr) {
      ReleaseMutex(handle_);
      CloseHandle(handle_);
      handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

 private:
  instance_lock(const instance_lock&);
  instance_lock& operator=(const instance_lock&);

#ifdef _WIN32
  HANDLE handle_;
#else
  int         fd_;
  std::string path_;
#endif
};

// ---------------------------------------------------------------------------
// 启动顺序（两条路共用）
// ---------------------------------------------------------------------------

bool apply_working_dir(const uvcpp_service_options& o) {
  if (o.working_dir.empty()) return true;
  const int rc = uv_chdir(o.working_dir.c_str());
  if (rc != 0) {
    std::fprintf(stderr, "[uvcpp_service] 切工作目录失败：%s（%s）\n",
                 o.working_dir.c_str(), uv_strerror(rc));
    return false;
  }
  return true;
}

bool apply_env(const uvcpp_service_options& o) {
  for (std::size_t i = 0; i < o.env.size(); ++i) {
    const int rc = uv_os_setenv(o.env[i].first.c_str(),
                                o.env[i].second.c_str());
    if (rc != 0) {
      std::fprintf(stderr, "[uvcpp_service] 设置环境变量 %s 失败：%s\n",
                   o.env[i].first.c_str(), uv_strerror(rc));
      return false;
    }
  }
  return true;
}

/**
 * @brief 工作目录 → 环境变量 → 日志 → 单实例 → configure → start。
 *
 * 返回 0 表示 `app` 已经在跑；非 0 时 `lock`/`log` 由调用方的析构收尾。
 */
int start_application(uvcpp_web_app& app, const uvcpp_service_options& o,
                      log_sink& log, instance_lock& lock) {
  if (!apply_working_dir(o)) return UV_EINVAL;
  if (!apply_env(o)) return UV_EINVAL;
  if (!log.open(o)) {
    std::fprintf(stderr, "[uvcpp_service] 打开日志文件失败：%s\n",
                 o.log_file.c_str());
    return UV_EIO;
  }

  if (o.single_instance) {
    const std::string name = effective_name(o);
    const int rc = lock.acquire(name);
    if (rc != 0) {
      std::fprintf(stderr,
                   "[uvcpp_service] 单实例锁被占用（name=%s）：另一个实例还在跑，"
                   "本次退出（%s）\n",
                   name.c_str(), uv_strerror(rc));
      return rc;
    }
  }

  if (o.configure) {
    const int rc = o.configure(app, g_config_path);
    if (rc != 0) {
      std::fprintf(stderr, "[uvcpp_service] configure 回调失败：%d\n", rc);
      return rc;
    }
  }

  if (o.stop_grace_ms > 0) app.set_shutdown_grace_ms(o.stop_grace_ms);

  const int rc = app.start();
  if (rc != 0) {
    std::fprintf(stderr, "[uvcpp_service] 启动失败：%s（%d）\n",
                 uv_strerror(rc), rc);
    return rc;
  }

  if (o.on_started) o.on_started(app);
  return 0;
}

void stop_application(uvcpp_web_app& app) {
  app.stop();
  app.join();
}

// ---------------------------------------------------------------------------
// 前台模式
// ---------------------------------------------------------------------------

int run_foreground(uvcpp_web_app& app, const uvcpp_service_options& o) {
  log_sink     log;
  instance_lock lock;
  int rc = start_application(app, o, log, lock);
  if (rc != 0) return rc;

  uvcpp_loop loop;

  uvcpp_signal sig_int(&loop);
  uvcpp_signal sig_term(&loop);
  bool stopping = false;
  std::function<void(uvcpp_signal*, int)> on_signal =
      [&](uvcpp_signal*, int signum) {
        if (stopping) return;
        stopping = true;
        std::fprintf(stderr, "[uvcpp_service] 收到信号 %d，开始优雅停机\n",
                     signum);
        app.stop();   // 线程安全；真正的排空由 app 自己的停机状态机完成
        loop.stop();  // 让 run() 返回，下面 join 等后台线程退干净
      };
  sig_int.start(on_signal, SIGINT);
#ifndef _WIN32
  sig_term.start(on_signal, SIGTERM);
#endif

  uvcpp_timer rotate(&loop);
  if (!o.log_file.empty() && o.log_max_bytes > 0 && o.log_max_files > 0) {
    rotate.start([&log](uvcpp_timer*) { log.rotate_if_needed(); }, 1000, 1000);
  }

  loop.run(UV_RUN_DEFAULT);

  if (!stopping) {
    // 不是信号停的：循环自己空了（理论上不该发生，但别让 join 永远等下去）。
    app.stop();
  }
  stop_application(app);
  return 0;
}

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Windows 服务
// ---------------------------------------------------------------------------

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS        g_status;
HANDLE                g_stop_event = nullptr;
uvcpp_web_app*        g_service_app = nullptr;
uvcpp_service_options g_service_opts;
std::wstring          g_service_name_w;
log_sink*             g_service_log = nullptr;

void report_status(DWORD state, DWORD exit_code, DWORD wait_hint) {
  if (g_status_handle == nullptr) return;
  g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState            = state;
  g_status.dwWin32ExitCode           = exit_code;
  g_status.dwServiceSpecificExitCode = 0;
  g_status.dwCheckPoint              = 0;
  g_status.dwWaitHint                = wait_hint;
  // 不接暂停/继续：应用层没有"暂停"语义，别让 SCM 的按钮骗人。
  g_status.dwControlsAccepted =
      (state == SERVICE_RUNNING)
          ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)
          : 0;
  SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI service_ctrl_handler(DWORD ctrl, DWORD, LPVOID, LPVOID) {
  switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      report_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
      if (g_stop_event != nullptr) SetEvent(g_stop_event);
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      // 不重新报状态就会被 SCM 当成"没响应"。
      SetServiceStatus(g_status_handle, &g_status);
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_is_service = true;
  g_status_handle =
      RegisterServiceCtrlHandlerExW(g_service_name_w.c_str(),
                                    service_ctrl_handler, nullptr);
  if (g_status_handle == nullptr) return;

  report_status(SERVICE_START_PENDING, NO_ERROR, 30000);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stop_event == nullptr) {
    report_status(SERVICE_STOPPED, GetLastError(), 0);
    return;
  }

  log_sink     log;
  instance_lock lock;
  g_service_log = &log;
  const int rc = start_application(*g_service_app, g_service_opts, log, lock);
  if (rc != 0) {
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    g_service_log = nullptr;
    report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, 0);
    return;
  }

  report_status(SERVICE_RUNNING, NO_ERROR, 0);

  while (true) {
    const DWORD w = WaitForSingleObject(g_stop_event, 1000);
    log.rotate_if_needed();
    if (w != WAIT_TIMEOUT) break;
    if (!g_service_app->running()) break;  // 应用自己停了（启动失败之外的路径）
  }

  report_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
  stop_application(*g_service_app);
  CloseHandle(g_stop_event);
  g_stop_event = nullptr;
  g_service_log = nullptr;
  report_status(SERVICE_STOPPED, NO_ERROR, 0);
}

int run_as_service(uvcpp_web_app& app, const uvcpp_service_options& o) {
  g_service_app  = &app;
  g_service_opts = o;
  g_service_name_w = widen(effective_name(o));

  SERVICE_TABLE_ENTRYW table[2];
  table[0].lpServiceName = &g_service_name_w[0];
  table[0].lpServiceProc = service_main;
  table[1].lpServiceName = nullptr;
  table[1].lpServiceProc = nullptr;

  if (StartServiceCtrlDispatcherW(table)) return 0;
  const DWORD err = GetLastError();
  if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
    // 不是从 SCM 拉起的（控制台里直接跑）：走前台模式。
    return run_foreground(app, o);
  }
  std::fprintf(stderr,
               "[uvcpp_service] StartServiceCtrlDispatcher 失败：%lu\n",
               static_cast<unsigned long>(err));
  return uv_translate_sys_error(static_cast<int>(err));
}

// ---- SCM 辅助 --------------------------------------------------------------

int scm_error(const char* what) {
  const DWORD err = GetLastError();
  std::fprintf(stderr, "[uvcpp_service] %s 失败：错误 %lu\n", what,
               static_cast<unsigned long>(err));
  if (err == ERROR_ACCESS_DENIED) {
    std::fprintf(stderr, "[uvcpp_service] 需要管理员权限（请用提升后的终端）\n");
  }
  return uv_translate_sys_error(static_cast<int>(err));
}

int cmd_install(const uvcpp_service_options& o) {
  const std::string name = effective_name(o);
  const std::wstring wname = widen(name);

  std::wstring cmd = L"\"" + widen(exe_path()) + L"\" run";
  if (!o.config_path.empty()) {
    cmd += L" --config \"" + widen(o.config_path) + L"\"";
  }
  if (!o.log_file.empty()) {
    cmd += L" --log \"" + widen(o.log_file) + L"\"";
    if (o.log_max_bytes > 0) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), " --log-max-mb %llu",
                    static_cast<unsigned long long>(o.log_max_bytes /
                                                    (1024ull * 1024ull)));
      cmd += widen(buf);
    }
    if (o.log_max_files > 0) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), " --log-files %d", o.log_max_files);
      cmd += widen(buf);
    }
  }
  if (!o.working_dir.empty()) {
    cmd += L" --workdir \"" + widen(o.working_dir) + L"\"";
  }
  if (o.stop_grace_ms > 0) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), " --grace-ms %d", o.stop_grace_ms);
    cmd += widen(buf);
  }
  if (!o.single_instance) cmd += L" --no-single-instance";

  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (scm == nullptr) return scm_error("打开服务控制管理器");

  const std::wstring wdisplay =
      widen(o.display_name.empty() ? name : o.display_name);
  SC_HANDLE svc = CreateServiceW(
      scm, wname.c_str(), wdisplay.c_str(), SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
      cmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
  if (svc == nullptr) {
    const int rc = scm_error("创建服务");
    CloseServiceHandle(scm);
    return rc;
  }

  if (!o.description.empty()) {
    SERVICE_DESCRIPTIONW desc;
    std::wstring wdesc = widen(o.description);
    desc.lpDescription = &wdesc[0];
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
  }

  // 崩溃后由 SCM 拉起：5 秒后重启，最多 3 次；一天之后计数复位。
  // 干净停机不会触发它（SCM 只在服务"失败"时用这张表）。
  SC_ACTION actions[3];
  actions[0].Type = SC_ACTION_RESTART;
  actions[0].Delay = 5000;
  actions[1] = actions[0];
  actions[2] = actions[0];
  SERVICE_FAILURE_ACTIONSW failures;
  std::memset(&failures, 0, sizeof(failures));
  failures.dwResetPeriod = 86400;
  failures.cActions      = 3;
  failures.lpsaActions   = actions;
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failures);

  std::printf("[uvcpp_service] 已安装：%s\n", name.c_str());
  std::printf("[uvcpp_service] 命令行：%s\n", narrow(cmd.c_str()).c_str());
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return 0;
}

SC_HANDLE open_service(SC_HANDLE scm, const std::wstring& wname,
                       DWORD access) {
  SC_HANDLE svc = OpenServiceW(scm, wname.c_str(), access);
  if (svc == nullptr) scm_error("打开服务");
  return svc;
}

int cmd_uninstall(const uvcpp_service_options& o) {
  const std::string name = effective_name(o);
  const std::wstring wname = widen(name);
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) return scm_error("打开服务控制管理器");
  // `SERVICE_DELETE` 不是访问位：`DeleteService()` 要的是标准权限 `DELETE`。
  SC_HANDLE svc = open_service(scm, wname,
                               SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
  if (svc == nullptr) {
    CloseServiceHandle(scm);
    return uv_translate_sys_error(static_cast<int>(GetLastError()));
  }

  SERVICE_STATUS st;
  if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
    for (int i = 0; i < 50; ++i) {
      if (!QueryServiceStatus(svc, &st)) break;
      if (st.dwCurrentState == SERVICE_STOPPED) break;
      Sleep(200);
    }
  }
  const BOOL ok = DeleteService(svc);
  const int rc = ok ? 0 : scm_error("删除服务");
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  if (ok) std::printf("[uvcpp_service] 已卸载：%s\n", name.c_str());
  return rc;
}

int cmd_start_stop(const uvcpp_service_options& o, bool start) {
  const std::string name = effective_name(o);
  const std::wstring wname = widen(name);
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) return scm_error("打开服务控制管理器");
  SC_HANDLE svc = open_service(scm, wname,
                               start ? (SERVICE_START | SERVICE_QUERY_STATUS)
                                     : (SERVICE_STOP | SERVICE_QUERY_STATUS));
  if (svc == nullptr) {
    CloseServiceHandle(scm);
    return uv_translate_sys_error(static_cast<int>(GetLastError()));
  }
  int rc = 0;
  if (start) {
    if (!StartServiceW(svc, 0, nullptr)) {
      const DWORD err = GetLastError();
      if (err != ERROR_SERVICE_ALREADY_RUNNING) {
        rc = uv_translate_sys_error(static_cast<int>(err));
      }
    }
  } else {
    SERVICE_STATUS st;
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
      rc = uv_translate_sys_error(static_cast<int>(GetLastError()));
    }
  }
  if (rc == 0) {
    std::printf("[uvcpp_service] 已%s：%s\n", start ? "启动" : "停止",
                name.c_str());
  }
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return rc;
}

int cmd_status(const uvcpp_service_options& o) {
  const std::string name = effective_name(o);
  const std::wstring wname = widen(name);
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (scm == nullptr) return scm_error("打开服务控制管理器");
  SC_HANDLE svc = open_service(scm, wname, SERVICE_QUERY_STATUS);
  if (svc == nullptr) {
    CloseServiceHandle(scm);
    return uv_translate_sys_error(static_cast<int>(GetLastError()));
  }
  // 要 pid 只能走 `QueryServiceStatusEx`：`QueryServiceStatus` 填的是
  // `SERVICE_STATUS`，它根本没有 `dwProcessId` 这个成员。
  SERVICE_STATUS_PROCESS st;
  DWORD needed = 0;
  std::memset(&st, 0, sizeof(st));
  const BOOL ok = QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                       reinterpret_cast<LPBYTE>(&st),
                                       sizeof(st), &needed);
  const int rc = ok ? 0 : scm_error("查询服务状态");
  if (ok) {
    const char* state = "未知";
    switch (st.dwCurrentState) {
      case SERVICE_STOPPED: state = "STOPPED"; break;
      case SERVICE_START_PENDING: state = "START_PENDING"; break;
      case SERVICE_STOP_PENDING: state = "STOP_PENDING"; break;
      case SERVICE_RUNNING: state = "RUNNING"; break;
      case SERVICE_PAUSED: state = "PAUSED"; break;
      default: break;
    }
    std::printf("[uvcpp_service] %s：%s（pid %lu）\n", name.c_str(), state,
                static_cast<unsigned long>(st.dwProcessId));
  }
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return rc;
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------

void print_usage() {
  std::printf(
      "用法：<app> [run|install|uninstall|start|stop|status] [选项]\n"
      "  run        运行（默认；Windows 下由 SCM 拉起走服务模式，否则前台）\n"
      "  install    注册成 Windows 服务（自动启动 + 崩溃后自动重启）\n"
      "  uninstall  停止并注销 Windows 服务\n"
      "  start/stop/status  操作已注册的 Windows 服务\n"
      "\n"
      "选项：\n"
      "  --name <名> --display-name <名> --description <文字>\n"
      "  --config <路径> --log <路径> --log-max-mb <n> --log-files <n>\n"
      "  --workdir <目录> --env K=V（可重复） --grace-ms <n>\n"
      "  --foreground --no-single-instance --help\n");
}

struct parsed_cli {
  parsed_cli() : foreground(false), help(false) {}
  std::string command;
  bool        foreground;
  bool        help;
};

int read_opt(int argc, char** argv, int i, const char* name, std::string& out,
             bool& matched) {
  matched = false;
  const std::string opt = std::string("--") + name;
  const std::string arg = argv[i];
  if (arg == opt) {
    matched = true;
    if (i + 1 >= argc) {
      std::fprintf(stderr, "[uvcpp_service] 选项 %s 缺少取值\n", opt.c_str());
      return UV_EINVAL;
    }
    out = argv[i + 1];
    return 0;
  }
  if (arg.size() > opt.size() + 1 &&
      arg.compare(0, opt.size() + 1, opt + "=") == 0) {
    matched = true;
    out = arg.substr(opt.size() + 1);
    return 0;
  }
  return 0;
}

int parse_args(int argc, char** argv, const uvcpp_service_options& base,
               parsed_cli& pc, uvcpp_service_options& out) {
  out = base;
  pc.command = "run";
  bool command_seen = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      pc.help = true;
      return 0;
    }
    if (arg == "--foreground") {
      pc.foreground = true;
      continue;
    }
    if (arg == "--single-instance") {
      out.single_instance = true;
      continue;
    }
    if (arg == "--no-single-instance") {
      out.single_instance = false;
      continue;
    }

    bool  matched = false;
    std::string value;
    int rc = read_opt(argc, argv, i, "name", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.name = value;
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "display-name", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.display_name = value;
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "description", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.description = value;
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "config", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.config_path = value;
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "log", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.log_file = value;
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "log-max-mb", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.log_max_bytes = static_cast<std::size_t>(std::strtoull(
                              value.c_str(), nullptr, 10)) *
                          1024u * 1024u;
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "log-files", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.log_max_files = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "workdir", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.working_dir = value;
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "grace-ms", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      out.stop_grace_ms = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }
    rc = read_opt(argc, argv, i, "env", value, matched);
    if (rc != 0) return rc;
    if (matched) {
      const std::size_t eq = value.find('=');
      if (eq == std::string::npos || eq == 0) {
        std::fprintf(stderr, "[uvcpp_service] --env 要 K=V 形状：%s\n",
                     value.c_str());
        return UV_EINVAL;
      }
      out.env.push_back(std::make_pair(value.substr(0, eq), value.substr(eq + 1)));
      if (arg.find('=') == std::string::npos) ++i;
      continue;
    }

    if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "[uvcpp_service] 未知选项：%s\n", arg.c_str());
      return UV_EINVAL;
    }
    if (!command_seen) {
      pc.command = arg;
      command_seen = true;
      continue;
    }
    std::fprintf(stderr, "[uvcpp_service] 多余的参数：%s\n", arg.c_str());
    return UV_EINVAL;
  }

  const std::string c = to_lower(pc.command);
  if (c != "run" && c != "install" && c != "uninstall" && c != "start" &&
      c != "stop" && c != "status") {
    std::fprintf(stderr, "[uvcpp_service] 未知子命令：%s\n", pc.command.c_str());
    return UV_EINVAL;
  }
  pc.command = c;
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// 公开面
// ---------------------------------------------------------------------------

uvcpp_service_options::uvcpp_service_options()
    : log_max_bytes(0),
      log_max_files(0),
      stop_grace_ms(0),
      single_instance(true) {}

int uvcpp_service_run(uvcpp_web_app& app, int argc, char** argv) {
  return uvcpp_service_run(app, uvcpp_service_options(), argc, argv);
}

int uvcpp_service_run(uvcpp_web_app& app, const uvcpp_service_options& opts,
                      int argc, char** argv) {
  parsed_cli           pc;
  uvcpp_service_options o;
  const int prc = parse_args(argc, argv, opts, pc, o);
  if (prc != 0) return prc;
  if (pc.help) {
    print_usage();
    return 0;
  }
  g_config_path = o.config_path;

  if (pc.command == "run") {
#ifdef _WIN32
    if (pc.foreground) return run_foreground(app, o);
    return run_as_service(app, o);
#else
    return run_foreground(app, o);
#endif
  }

#ifdef _WIN32
  if (pc.command == "install") return cmd_install(o);
  if (pc.command == "uninstall") return cmd_uninstall(o);
  if (pc.command == "start") return cmd_start_stop(o, true);
  if (pc.command == "stop") return cmd_start_stop(o, false);
  if (pc.command == "status") return cmd_status(o);
#else
  std::fprintf(stderr,
               "[uvcpp_service] 子命令 %s 只在 Windows 上有意义；POSIX 上请用"
               "系统自己的服务管理器（systemd / launchd）调用 `run`\n",
               pc.command.c_str());
  return UV_ENOTSUP;
#endif
  return UV_EINVAL;
}

const std::string& uvcpp_service_config_path() { return g_config_path; }

bool uvcpp_service_is_service() { return g_is_service; }

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE
