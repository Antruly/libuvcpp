/**
 * @file tests/functional/web_app_service_func.cpp
 * @brief 服务化部署支持（issue #73）的前台模式用例。
 *
 * Windows 服务那条路（SCM）只能在真 Windows 上装/卸服务，本用例不覆盖 ——
 * 它在 `tests/functional/CMakeLists.txt` 里被 `WIN32` 排除。这里覆盖的是三条
 * 真正跨平台、也是"优雅停机"这半边核心的判据：
 *
 * 1. **前台运行 + 信号停机**：子进程以 `uvcpp_service_run()` 跑起来，父进程发
 *    SIGTERM，子进程退出码 0；
 * 2. **优雅排空**：发 SIGTERM 的同时有一条在途的慢请求，它必须正常拿到完整
 *    响应（不是被掐断的半个 body）；
 * 3. **单实例**：同名第二个实例拿到 `UV_EBUSY` 退出，第一个继续服务；
 * 4. **日志落盘 + 轮转**：`--log` 重定向后文件有内容，超过 1 MiB 后出现 `.1`；
 * 5. **子命令边界**：POSIX 上 `install` 返回 `UV_ENOTSUP`，未知选项返回
 *    `UV_EINVAL`。
 *
 * 子进程的角色由 `--child` 打开，其余参数是本文件自己的（不交给服务的命令行
 * 解析器，避免两边语法耦合）。
 */

#include <uvcpp/uvcpp_define.h>

#include <cstdio>  // `#else` 那条 SKIP 分支也要用它

#if UVCPP_WEBAPP_ENABLE && !defined(_WIN32)

#include <handle/uvcpp_timer.h>
#include <webapp/uvcpp_service.h>
#include <webapp/uvcpp_web_app.h>

#include <uv.h>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <utility>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

void sleep_ms(int ms) { ::usleep(static_cast<useconds_t>(ms) * 1000); }

std::string read_file(const std::string& path) {
  std::string out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return out;
  char buf[512];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

bool file_exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

long long file_size(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return -1;
  return static_cast<long long>(st.st_size);
}

std::string self_exe() {
  char buf[4096];
  std::size_t size = sizeof(buf);
  if (uv_exepath(buf, &size) != 0) return std::string();
  return std::string(buf, size);
}

// ---------------------------------------------------------------------------
// 子进程：真服务
// ---------------------------------------------------------------------------

int child_main(int argc, char** argv) {
  std::string name;
  std::string port_file;
  std::string log_file;
  const std::pair<std::string, std::string*> kChildOpts[] = {
      std::make_pair(std::string("name"), &name),
      std::make_pair(std::string("port-file"), &port_file),
      std::make_pair(std::string("log"), &log_file)};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    for (std::size_t k = 0; k < sizeof(kChildOpts) / sizeof(kChildOpts[0]);
         ++k) {
      const std::string opt = "--" + kChildOpts[k].first;
      if (a == opt && i + 1 < argc) {
        *kChildOpts[k].second = argv[++i];
        break;
      }
      if (a.size() > opt.size() + 1 &&
          a.compare(0, opt.size() + 1, opt + "=") == 0) {
        *kChildOpts[k].second = a.substr(opt.size() + 1);
        break;
      }
    }
  }

  uvcpp::uvcpp_web_app app;
  app.set_host("127.0.0.1")
      .set_port(0)
      .set_access_log(false)
      .set_log_level(uvcpp::log_level::WARN);

  app.get("/hello", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                       uvcpp::uvcpp_web_next) {
    resp.text("hello");
    resp.end();
  });

  // 慢请求：300ms 之后才应答。**不用 sleep 卡住循环线程** —— 卡住的话停机
  // 状态机根本没机会在"请求在途"时启动，这条用例就测不到排空。
  app.get("/slow", [&app](uvcpp::uvcpp_web_request&,
                          uvcpp::uvcpp_web_response& resp,
                          uvcpp::uvcpp_web_next next) {
    uvcpp::uvcpp_timer* t = new uvcpp::uvcpp_timer(app.loop());
    t->start(
        [&resp, next, t](uvcpp::uvcpp_timer*) {
          resp.text("slow-done");
          resp.end();
          next();        // 链跑完 → 上下文收场
          t->stop();     // 回调里不 delete 自己；子进程随后退出，这一枚不泄漏
        },
        300, 0);
  });

  app.get("/logburst", [](uvcpp::uvcpp_web_request&, uvcpp::uvcpp_web_response& resp,
                          uvcpp::uvcpp_web_next) {
    static const char kLine[] = "0123456789abcdef0123456789abcdef\n";  // 33 字节
    for (int i = 0; i < 48000; ++i) {  // ~1.58 MiB
      std::fwrite(kLine, 1, sizeof(kLine) - 1, stdout);
    }
    std::fflush(stdout);
    resp.text("burst-done");
    resp.end();
  });

  uvcpp::uvcpp_service_options opts;
  opts.name            = name;
  opts.single_instance = true;
  opts.log_file        = log_file;
  opts.log_max_bytes   = 1024u * 1024u;  // 1 MiB
  opts.log_max_files   = 2;
  opts.stop_grace_ms   = 8000;
  opts.on_started      = [&app, port_file](uvcpp::uvcpp_web_app&) {
    if (port_file.empty()) return;
    FILE* f = std::fopen(port_file.c_str(), "wb");
    if (f == nullptr) return;
    std::fprintf(f, "%d\n", app.bound_port());
    std::fclose(f);
  };

  // 只把程序名交给服务解析器：本文件的 `--child/--port-file` 不是服务选项。
  char* svc_argv[2];
  svc_argv[0] = argv[0];
  svc_argv[1] = nullptr;
  return uvcpp::uvcpp_service_run(app, opts, 1, svc_argv);
}

// ---------------------------------------------------------------------------
// 父进程：socket / 进程小工具
// ---------------------------------------------------------------------------

int tcp_connect(int port, int timeout_ms) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval tv;
  tv.tv_sec  = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
          0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string read_all(int fd) {
  std::string out;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, n);
  return out;
}

bool http_get(int port, const std::string& path, std::string& out,
              bool wait_for_close = true) {
  const int fd = tcp_connect(port, 8000);
  if (fd < 0) return false;
  const std::string req = "GET " + path +
                          " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
  if (::write(fd, req.data(), req.size()) !=
      static_cast<ssize_t>(req.size())) {
    ::close(fd);
    return false;
  }
  if (wait_for_close) {
    out = read_all(fd);
  }
  ::close(fd);
  return wait_for_close ? !out.empty() : true;
}

pid_t spawn_child(const std::string& exe, const std::string& name,
                  const std::string& port_file, const std::string& log_file) {
  const pid_t pid = ::fork();
  if (pid != 0) return pid;
  std::string n = "--name=" + name;
  std::string p = "--port-file=" + port_file;
  std::string l = "--log=" + log_file;
  ::execl(exe.c_str(), exe.c_str(), "--child", n.c_str(), p.c_str(), l.c_str(),
          static_cast<char*>(nullptr));
  ::_exit(127);
}

bool wait_port(const std::string& port_file, int& port, int timeout_ms) {
  const int steps = timeout_ms / 50;
  for (int i = 0; i < steps; ++i) {
    const std::string s = read_file(port_file);
    if (!s.empty()) {
      port = std::atoi(s.c_str());
      if (port > 0) return true;
    }
    sleep_ms(50);
  }
  return false;
}

/// 非阻塞等退出；返回 true 表示等到了，退出码写进 status。
bool wait_exit(pid_t pid, int timeout_ms, int& status) {
  const int steps = timeout_ms / 50;
  for (int i = 0; i < steps; ++i) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) return true;
    sleep_ms(50);
  }
  return false;
}

bool exited_zero(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--child") return child_main(argc, argv);
  }

  std::printf("[service] issue #73（前台模式）\n");
  const std::string exe = self_exe();
  check(!exe.empty(), "拿到自己的可执行文件路径");

  char tmpl[] = "/tmp/uvcpp_service_test_XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  check(dir != nullptr, "建临时目录");
  if (dir == nullptr || exe.empty()) return 1;
  const std::string base = dir;
  const std::string name = "uvcpp-test-" + std::to_string(static_cast<long>(::getpid()));
  const std::string port_file = base + "/port.txt";
  const std::string log_file = base + "/service.log";

  // ---- 启动 A -----------------------------------------------------------
  const pid_t a = spawn_child(exe, name, port_file, log_file);
  check(a > 0, "子进程 A 已拉起");
  int port = 0;
  check(wait_port(port_file, port, 10000), "A 写出实际端口");
  check(port > 0, "端口有效");

  std::string body;
  check(http_get(port, "/hello", body), "A 能应答 /hello");
  check(body.find("200 OK") != std::string::npos, "状态行是 200");
  check(body.find("hello") != std::string::npos, "body = hello");

  // ---- 单实例 -----------------------------------------------------------
  {
    const std::string port_file_b = base + "/port_b.txt";
    const pid_t b = spawn_child(exe, name, port_file_b, base + "/b.log");
    int st = 0;
    const bool done = wait_exit(b, 6000, st);
    check(done, "同名第二个实例自己退出了");
    if (done) {
      check(!exited_zero(st), "第二个实例的退出码非 0（UV_EBUSY）");
    } else {
      ::kill(b, SIGKILL);
      ::waitpid(b, &st, 0);
    }
    check(read_file(port_file_b).empty(), "第二个实例没有写出端口（没开始服务）");

    body.clear();
    check(http_get(port, "/hello", body) &&
              body.find("hello") != std::string::npos,
          "A 在 B 退出之后仍在服务");
  }

  // ---- 日志：先打一轮，再验证轮转 ---------------------------------------
  {
    const int fd = tcp_connect(port, 8000);
    check(fd >= 0, "连上 /logburst");
    if (fd >= 0) {
      static const char kReq[] =
          "GET /logburst HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
      ::write(fd, kReq, sizeof(kReq) - 1);
      const std::string r = read_all(fd);
      ::close(fd);
      check(r.find("burst-done") != std::string::npos, "/logburst 正常完成");
    }
    // 轮转是 1s 一次的看门狗：给两拍。
    for (int i = 0; i < 40 && !file_exists(log_file + ".1"); ++i) sleep_ms(100);
    // 轮转之后当前文件可能是 0 字节（新开的那份还没再写东西）—— 判据是
    // "两代加起来有内容"，不是"当前那份非空"。
    check(file_size(log_file) + file_size(log_file + ".1") > 0,
          "日志落盘且非空");
    check(file_exists(log_file + ".1"), "超过上限后轮转出 .1");
  }

  // ---- 在途请求 + SIGTERM = 优雅排空 ------------------------------------
  {
    const int fd = tcp_connect(port, 8000);
    check(fd >= 0, "连上 /slow");
    if (fd >= 0) {
      static const char kReq[] =
          "GET /slow HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
      ::write(fd, kReq, sizeof(kReq) - 1);
      sleep_ms(80);           // 让请求真的进入在途状态（handler 已开始计时）
      ::kill(a, SIGTERM);     // 停机会等它
      const std::string r = read_all(fd);
      ::close(fd);
      check(r.find("200 OK") != std::string::npos, "在途请求仍拿到 200");
      check(r.find("slow-done") != std::string::npos,
            "在途请求的完整 body 到达（没有被掐断）");
    }
  }

  int st = 0;
  const bool stopped = wait_exit(a, 15000, st);
  check(stopped, "A 收到 SIGTERM 后退出");
  if (stopped) {
    check(exited_zero(st), "退出码 0（优雅停机完成）");
  } else {
    ::kill(a, SIGKILL);
    ::waitpid(a, &st, 0);
  }

  // ---- 子命令边界 --------------------------------------------------------
  {
    uvcpp::uvcpp_web_app app;
    char* install_argv[2];
    install_argv[0] = argv[0];
    install_argv[1] = const_cast<char*>("install");
    // POSIX 上 install 不碰 app，也不开始服务。
    check(uvcpp::uvcpp_service_run(app, 2, install_argv) == UV_ENOTSUP,
          "POSIX 上 install 返回 UV_ENOTSUP");

    char* bad_argv[3];
    bad_argv[0] = argv[0];
    bad_argv[1] = const_cast<char*>("--no-such-option");
    bad_argv[2] = nullptr;
    check(uvcpp::uvcpp_service_run(app, 2, bad_argv) == UV_EINVAL,
          "未知选项返回 UV_EINVAL");
  }

  if (g_fail == 0) {
    std::printf("[service] ALL PASS\n");
    return 0;
  }
  std::printf("[service] %d check(s) failed\n", g_fail);
  return 1;
}

#else  // !(webapp && POSIX)

int main() {
  std::printf("SKIP: webapp 关着或不是 POSIX\n");
  return 3;
}

#endif
