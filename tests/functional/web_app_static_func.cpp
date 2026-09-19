/**
 * @file tests/functional/web_app_static_func.cpp
 * @brief `uvcpp_web_static` 的功能测试 —— 真端口、真文件、真客户端。
 *
 * 这个模块的价值集中在三件**只有跑起来才看得见**的事情上，用例也就围着它们：
 *
 *   1. **断点续传那一整套**（Range/206/416/Content-Range/If-Range）。
 *      它的正确性不看状态码，看的是**字节**：206 回的必须是那一段，
 *      偏移错了状态码照样是 206。
 *   2. **缓存真的会失效**。"命中计数变大了"证明不了任何事 —— 一个永远
 *      命中、内容永远是旧的缓存也能让计数很好看。所以这里改完文件**必须
 *      拿到新内容**才算过。
 *   3. **安全边界**。断言的不是状态码，而是**响应体里没有那个秘密**：
 *      穿透成功时状态码可能仍然是 200/404，只有比对内容才骗不了人。
 *
 * 所有挂载都在**一个 App** 上，用不同前缀配不同选项（`/deny`、`/spa`、
 * `/capped`……）。这样一次 `start_background()` 就能覆盖全部选项组合，
 * 也顺带验证了"多个静态挂载互不干扰"。
 *
 * HEAD 走裸客户端：`uvcpp_http_client` 的 `make_head()` 是能跑 HEAD 的，但
 * 这里要验的是"响应里真的一个 body 字节都没有"，裸路径能逐字看到线上字节，
 * 比隔着客户端猜更确定。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#include "wait_util.h"

#if UVCPP_WEBAPP_ENABLE

#include "net/uvcpp_net_read.h"
#include "net/uvcpp_tcp_client.h"
#include <web/uvcpp_http_client.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_util.h>  // web_real_path（搭链接时要把目标转绝对）

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want,
              const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cerr << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_ne(const std::string& got, const std::string& unwanted,
              const std::string& what) {
  if (got == unwanted) {
    std::cerr << "  [FAIL] " << what << " —— 不该等于 [" << unwanted << "]"
              << std::endl;
    ++g_failures;
  }
}

/** 断言响应体里**不含**某个秘密。安全用例只看状态码是不够的。 */
void check_hides(const std::string& body, const std::string& secret,
                 const std::string& what) {
  if (body.find(secret) != std::string::npos) {
    std::cerr << "  [FAIL] " << what << " —— 响应体里出现了 [" << secret
              << "]" << std::endl;
    ++g_failures;
  }
}

// =========================================================================
// 测试用文档根
// =========================================================================

// 放在工作目录下（ctest 的工作目录就是构建目录），跑完删掉。
const char* k_root = "uvcpp_static_test_root";
const char* k_secret = "uvcpp_static_test_secret.txt";
const char* k_secret_text = "TOP-SECRET-OUTSIDE-ROOT";

/**
 * 根外面的一个**目录**，里面再放一个秘密 —— 链接逃逸用例的靶子。
 *
 * 用独立的目录而不是直接链接到 `.`：后者一旦遇上"误递归删除"，删掉的就是
 * 构建目录本身。省三行代码不值得冒这个险。
 */
const char* k_outside_dir = "uvcpp_static_test_outside";
const char* k_outside_secret = "uvcpp_static_test_outside/secret2.txt";
const char* k_outside_text = "SECOND-SECRET-VIA-LINK";

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define TEST_MKDIR(p) _mkdir(p)
#define TEST_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>

#define TEST_MKDIR(p) mkdir((p), 0755)
#define TEST_RMDIR(p) rmdir(p)
#endif

bool dir_exists(const std::string& p) {
#ifdef _WIN32
  struct _stat st;
  if (_stat(p.c_str(), &st) != 0) return false;
  return (st.st_mode & _S_IFDIR) != 0;
#else
  struct stat st;
  if (stat(p.c_str(), &st) != 0) return false;
  return S_ISDIR(st.st_mode);
#endif
}

/**
 * 建目录，**已经存在也算成功**。
 *
 * 这一条是为崩溃准备的：用例跑挂了就不会走到 `cleanup_root()`，下一次运行
 * 看到的就是一个还留着内容的旧目录 —— `mkdir` 失败、`rmdir` 又因为非空而
 * 失败，于是测试从第二次起永远起不来。而"起不来"和"真的挂了"在 ctest 输出
 * 里长得一模一样。
 */
bool ensure_dir(const std::string& p) {
  if (TEST_MKDIR(p.c_str()) == 0) return true;
  return dir_exists(p);
}

bool write_file(const std::string& path, const std::string& data) {
  std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  if (!data.empty()) f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return f.good();
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if (!f) return false;
  out.assign((std::istreambuf_iterator<char>(f)),
             std::istreambuf_iterator<char>());
  return true;
}

void remove_file(const std::string& path) { std::remove(path.c_str()); }

/**
 * @brief 在 `link` 处建一个指向 `target` 的**目录链接**。
 *
 * Windows 上用目录 **junction**（`mklink /J`）而不是符号链接：创建 symlink
 * 需要管理员权限或开发者模式，CI 上大概率没有，那样这条用例就会静默地
 * "跳过" —— 而它会跳过的方式是断言永远不成立，看起来和通过一样。junction
 * 不需要提权，同样是 reparse point，`realpath` 照常解析，对
 * "真实路径包含判断"的考验与 symlink 完全等价。
 *
 * 用 `cmd /c` 是因为 Win32 的 `CreateSymbolicLink` 对 junction 不适用，
 * 而 `DeviceIoControl` 手搓 reparse point 不值得为一个测试写。
 */
bool make_dir_link(const std::string& link, const std::string& target) {
  // 目标先转成绝对路径，**这一步是必需的**：`mklink /J` 把相对目标按当前目录
  // 解析、落盘成绝对路径，而 POSIX 的 `symlink()` 把目标字符串原样存下来、
  // 解析时相对于**链接所在目录**（且不检查目标是否存在，照样返回 0）。
  // 传相对名在 POSIX 上会建出一个悬空链接 —— 本机实测 mklink 的 Target 字段
  // 是绝对的，所以这条差异只在 POSIX 上现形，本机复现不了。
  std::string abs;
  if (!web_real_path(target, abs, /*allow_missing=*/false)) return false;
#ifdef _WIN32
  const std::string cmd =
      "cmd /c mklink /J \"" + link + "\" \"" + abs + "\" >NUL 2>&1";
  return std::system(cmd.c_str()) == 0;
#else
  return ::symlink(abs.c_str(), link.c_str()) == 0;
#endif
}

/** 删链接：Windows 上 junction 是目录（`rmdir` 只摘掉链接本身，不动目标）；POSIX 上 symlink 是文件。 */
void remove_dir_link(const std::string& p) {
#ifdef _WIN32
  TEST_RMDIR(p.c_str());
#else
  ::unlink(p.c_str());
#endif
}

void cleanup_root();  // 定义在下面，build_root() 要先拿它清残骸

std::string big_content() { return std::string(256 * 1024, 'x'); }

/**
 * 搭出文档根。**秘密文件放在根的旁边**（兄弟路径）—— 穿越用例断言"读不到
 * 它"，前提是它确实存在、而且确实在根外面，所以这个文件必须在。
 */
bool build_root() {
  // 先把上一次留下的残骸清掉（删不掉也无所谓，下面每个文件都会重写）。
  cleanup_root();

  if (!ensure_dir(k_root)) {
    std::cerr << "  [FAIL] 无法创建测试文档根 " << k_root << std::endl;
    return false;
  }
  const std::string r(k_root);

  bool ok = true;
  ok = write_file(r + "/index.html", "<h1>ROOT-INDEX</h1>") && ok;
  ok = write_file(r + "/hello.txt", "hello static") && ok;
  ok = write_file(r + "/app.js", "var x=1;") && ok;
  ok = write_file(r + "/noext", "no extension here") && ok;
  ok = write_file(r + "/empty.txt", "") && ok;
  ok = write_file(r + "/big.bin", big_content()) && ok;
  ok = write_file(r + "/.env", "SECRET=in-dotfile") && ok;
  ok = write_file(r + "/.hash.txt", "d41d8cd9") && ok;
  ok = write_file(r + "/a..b.txt", "double dot is fine") && ok;

  ok = ensure_dir(r + "/sub") && ok;
  ok = write_file(r + "/sub/index.html", "<h1>SUB-INDEX</h1>") && ok;
  ok = write_file(r + "/sub/a.txt", "sub a") && ok;

  ok = ensure_dir(r + "/deep") && ok;
  ok = ensure_dir(r + "/deep/inner") && ok;
  ok = write_file(r + "/deep/inner/f.txt", "deep file") && ok;

  // 根外面的秘密（穿越用例的靶子）
  ok = write_file(k_secret, k_secret_text) && ok;
  ok = ensure_dir(k_outside_dir) && ok;
  ok = write_file(k_outside_secret, k_outside_text) && ok;

  // 链接逃逸用的两个链接。**两条缺一不可**：
  //   - `escape` 指向根**外**，它必须被挡住 —— 这是第 2 道边界
  //     （`web_resolve_within_root`）唯一的用武之地：路径文本
  //     `/escape/secret2.txt` 完全合法，第 1 道（`web_sanitize_path`）
  //     看不出任何问题。
  //   - `inside` 指向根**内**，它必须照常工作。没有这条对照，"把所有链接
  //     一律拒绝"也能让逃逸断言通过 —— 那测的就不是包含判断了。
  ok = make_dir_link(r + "/escape", k_outside_dir) && ok;
  ok = make_dir_link(r + "/inside", r + "/sub") && ok;

  // 「建成了」不等于「通」：`symlink()` 对悬空目标同样返回 0。链接不通的话
  // 下面那些断言会以最难归因的方式失效 —— `/inside` 那组会红（还算诚实），
  // 而 `/escape` 那组的**逃逸**断言会因为 404 页面里本来就没有秘密而
  // **假装通过**，于是"安全边界"这件事在整套用例里再没有任何东西在测。
  // 所以在这里先钉死，别让后面那组断言承担它不该承担的静默。
  if (ok && !(dir_exists(r + "/escape") && dir_exists(r + "/inside"))) {
    std::cerr << "  [FAIL] 目录链接建成但无法解析（悬空链接）" << std::endl;
    ok = false;
  }

  return ok;
}

void cleanup_root() {
  // 链接要在 `rmdir(root)` **之前**摘掉，否则根目录非空、删不掉。
  remove_dir_link(std::string(k_root) + "/escape");
  remove_dir_link(std::string(k_root) + "/inside");
  remove_file(std::string(k_root) + "/sub/index.html");
  remove_file(std::string(k_root) + "/sub/a.txt");
  remove_file(std::string(k_root) + "/deep/inner/f.txt");
  remove_file(std::string(k_root) + "/index.html");
  remove_file(std::string(k_root) + "/hello.txt");
  remove_file(std::string(k_root) + "/app.js");
  remove_file(std::string(k_root) + "/noext");
  remove_file(std::string(k_root) + "/empty.txt");
  remove_file(std::string(k_root) + "/big.bin");
  remove_file(std::string(k_root) + "/.env");
  remove_file(std::string(k_root) + "/.hash.txt");
  remove_file(std::string(k_root) + "/a..b.txt");
  remove_file(std::string(k_root) + "/home.html");
  TEST_RMDIR((std::string(k_root) + "/sub").c_str());
  TEST_RMDIR((std::string(k_root) + "/deep/inner").c_str());
  TEST_RMDIR((std::string(k_root) + "/deep").c_str());
  TEST_RMDIR(k_root);
  remove_file(k_secret);
  remove_file(k_outside_secret);
  TEST_RMDIR(k_outside_dir);
}

// =========================================================================
// 客户端侧小工具（沿用 web_app_app_func.cpp 的写法）
// =========================================================================

bool roundtrip(int port, const uvcpp_http_request& req,
               uvcpp_http_response& resp) {
  for (int i = 0; i < 40; ++i) {
    uvcpp_http_client client;
    if (client.connect_wait("127.0.0.1", port, 2000) == 0 &&
        client.send_wait(req, resp, 3000) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

/** 带自定义头的一次往返。 */
bool get_with(int port, const std::string& path,
              const std::vector<std::pair<std::string, std::string> >& headers,
              uvcpp_http_response& resp) {
  uvcpp_http_request req = uvcpp_http_request::make_get(path);
  for (size_t i = 0; i < headers.size(); ++i) {
    req.set_header(headers[i].first, headers[i].second);
  }
  return roundtrip(port, req, resp);
}

bool get(int port, const std::string& path, uvcpp_http_response& resp) {
  return roundtrip(port, uvcpp_http_request::make_get(path), resp);
}

std::string body_of(const uvcpp_http_response& r) {
  if (r.body.size() == 0) return std::string();
  return std::string(r.body.get_const_data(), r.body.size());
}

int status_of(const uvcpp_http_response& r) {
  return static_cast<int>(r.status_code);
}

std::string header_of(const uvcpp_http_response& r, const char* name) {
  return http_get_header(r.headers, name);
}

// ---- 裸交换（HEAD 用）----

struct raw_result {
  bool        ok;
  std::string raw;
  raw_result() : ok(false) {}
};

/**
 * 连上去、把请求原样写出去、读到对端关闭为止，返回**全部原始字节**。
 *
 * 请求里必须带 `Connection: close`，否则服务端不关连接，这里会等到超时
 * （那样也能拿到响应，但每次用例要白等一整个 wait_ms）。
 */
raw_result raw_exchange(int port, const std::string& request_bytes,
                        int wait_ms) {
  raw_result out;
  uvcpp_tcp_client client;
  std::atomic<bool> connected(false);
  std::atomic<bool> ended(false);

  int rc = client.connect("127.0.0.1", port, [&](int st) {
    if (st != 0) return;
    connected.store(true);
    client.read_start_events([&](uvcpp_tcp_client&, const net_read_result& r) {
      if (r.is_data()) {
        out.raw.append(r.data, r.size);
      } else {
        ended.store(true);
      }
    });
    client.write(request_bytes.data(), request_bytes.size(), [](int) {});
  });
  if (rc != 0) return out;

  uvcpp_loop* loop = client.get_loop();
  uvcpp_test::wait_until(loop, [&] { return connected.load(); }, uvcpp_test::kWaitMs);
  if (!connected.load()) {
    if (client.get_tcp() != nullptr) client.get_tcp()->close([](uvcpp_handle*) {});
    return out;
  }

  const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  while (!ended.load()) {
    loop->run(UV_RUN_NOWAIT);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    if (ms >= wait_ms) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::atomic<bool> closed(false);
  if (client.get_tcp() != nullptr) {
    client.get_tcp()->close([&](uvcpp_handle*) { closed.store(true); });
  }
  uvcpp_test::wait_until(loop, [&] { return closed.load(); }, uvcpp_test::kWaitMs);
  for (int i = 0; i < 30; ++i) loop->run(UV_RUN_NOWAIT);

  out.ok = !out.raw.empty();
  return out;
}

/// 把 `/x HTTP/1.1` 拼成一条完整的头 `Connection: close` 的请求。
std::string raw_head_request(const std::string& method, const std::string& path) {
  return method + " " + path +
         " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
}

std::string raw_body(const std::string& raw) {
  const size_t p = raw.find("\r\n\r\n");
  return (p == std::string::npos) ? std::string() : raw.substr(p + 4);
}

std::string raw_header(const std::string& raw, const std::string& name) {
  const size_t head_end = raw.find("\r\n\r\n");
  const std::string head =
      (head_end == std::string::npos) ? raw : raw.substr(0, head_end);

  const std::string want = web_to_lower(name) + ":";
  size_t pos = 0;
  while (pos < head.size()) {
    size_t end = head.find("\r\n", pos);
    if (end == std::string::npos) end = head.size();
    const std::string line = head.substr(pos, end - pos);
    const std::string lower = web_to_lower(line);
    if (web_starts_with(lower, want)) {
      return web_trim(line.substr(want.size()));
    }
    if (end == head.size()) break;
    pos = end + 2;
  }
  return std::string();
}

int raw_status(const std::string& raw) {
  const size_t sp = raw.find(' ');
  if (sp == std::string::npos) return 0;
  return std::atoi(raw.c_str() + sp + 1);
}

// =========================================================================
// 用例
// =========================================================================

int g_port = 0;

// ---- 1. 基本响应与 MIME ----

void test_basic_and_mime() {
  std::cout << "[static] basic_and_mime" << std::endl;
  uvcpp_http_response r;

  check(get(g_port, "/hello.txt", r), "/hello.txt 应当有响应");
  check_eq_i(status_of(r), 200, "/hello.txt 状态码");
  check_eq(body_of(r), "hello static", "/hello.txt 内容");
  check_eq(header_of(r, "content-type"), "text/plain; charset=utf-8",
           "文本类型要补 charset");

  check(get(g_port, "/app.js", r), "/app.js 应当有响应");
  check_eq(header_of(r, "content-type"), "application/javascript; charset=utf-8",
           ".js 的类型");

  check(get(g_port, "/index.html", r), "/index.html 应当有响应");
  check_eq(header_of(r, "content-type"), "text/html; charset=utf-8",
           ".html 的类型");

  // 未知类型：兜底 + **不补 charset**（octet-stream 不是文本）
  check(get(g_port, "/noext", r), "/noext 应当有响应");
  check_eq(body_of(r), "no extension here", "/noext 内容");
  check_eq(header_of(r, "content-type"), "application/octet-stream",
           "未知扩展名不该补 charset");

  // 空文件：CL:0 必须有，否则客户端会一直等
  check(get(g_port, "/empty.txt", r), "/empty.txt 应当有响应");
  check_eq_i(status_of(r), 200, "空文件状态码");
  check_eq_i(static_cast<long long>(r.body.size()), 0, "空文件 body 应为空");
  check_eq(header_of(r, "content-length"), "0", "空文件必须显式给 Content-Length: 0");

  // 名字里带连续的点不该被当成穿越
  check(get(g_port, "/a..b.txt", r), "a..b.txt 应当有响应");
  check_eq_i(status_of(r), 200, "a..b.txt 应当 200（不是 403/404）");
}

// ---- 2. 索引文件与挂载前缀 ----

void test_index_and_prefix() {
  std::cout << "[static] index_and_prefix" << std::endl;
  uvcpp_http_response r;

  check(get(g_port, "/", r), "根路径应当有响应");
  check_eq(body_of(r), "<h1>ROOT-INDEX</h1>", "根路径应当给索引文件");

  check(get(g_port, "/sub", r), "/sub 应当有响应");
  check_eq(body_of(r), "<h1>SUB-INDEX</h1>", "/sub 应当给子目录索引");

  // **这两个必须等价** —— 旧实现正是在这里挂的（只注册了通配路由，
  // 于是 `/static` 取不到索引、`/static/` 拿到空 200）。
  check(get(g_port, "/sub/", r), "/sub/ 应当有响应");
  check_eq(body_of(r), "<h1>SUB-INDEX</h1>", "/sub/ 与 /sub 必须等价");

  check(get(g_port, "/static", r), "挂载前缀本身应当有响应");
  check_eq(body_of(r), "<h1>ROOT-INDEX</h1>", "/static 应当给索引文件");

  check(get(g_port, "/static/", r), "/static/ 应当有响应");
  check_eq(body_of(r), "<h1>ROOT-INDEX</h1>", "/static/ 与 /static 必须等价");

  check(get(g_port, "/static/hello.txt", r), "/static/hello.txt 应当有响应");
  check_eq(body_of(r), "hello static", "带前缀的文件内容");

  check(get(g_port, "/static/sub/a.txt", r), "带前缀的深路径应当有响应");
  check_eq(body_of(r), "sub a", "带前缀的深路径内容");

  // 多段通配
  check(get(g_port, "/deep/inner/f.txt", r), "深层文件应当有响应");
  check_eq(body_of(r), "deep file", "深层文件内容");

  // 目录没有索引 → 404（不是空 200）
  check(get(g_port, "/deep/inner", r), "无索引目录应当有响应");
  check_eq_i(status_of(r), 404, "没有索引文件的目录应当 404");

  // 只注册了 GET/HEAD → POST 是 405，且必须带 Allow
  uvcpp_http_request post =
      uvcpp_http_request::make_post("/hello.txt", "x", 1, "text/plain");
  check(roundtrip(g_port, post, r), "POST 应当有响应");
  check_eq_i(status_of(r), 405, "静态路由不接受 POST");
  const std::string allow = header_of(r, "allow");
  check(allow.find("GET") != std::string::npos, "405 必须带 Allow: GET（实际 [" + allow + "]）");
}

// ---- 3. ETag / 304 ----

void test_etag_and_304() {
  std::cout << "[static] etag_and_304" << std::endl;
  uvcpp_http_response r;

  check(get(g_port, "/hello.txt", r), "取 ETag 的第一次请求");
  const std::string etag = header_of(r, "etag");
  check(!etag.empty(), "200 应当带 ETag");
  check(etag.size() > 2 && etag[0] == '"', "ETag 必须带引号（实际 [" + etag + "]）");

  const std::string lm = header_of(r, "last-modified");
  check(!lm.empty(), "200 应当带 Last-Modified");

  // 命中 → 304 且无 body
  std::vector<std::pair<std::string, std::string> > h;
  h.push_back(std::make_pair(std::string("If-None-Match"), etag));
  check(get_with(g_port, "/hello.txt", h, r), "带 If-None-Match 的请求");
  check_eq_i(status_of(r), 304, "ETag 命中应当 304");
  check_eq_i(static_cast<long long>(r.body.size()), 0, "304 不该有 body");
  check_eq(header_of(r, "etag"), etag, "304 应当回显 ETag");

  // `*` 也命中
  h.clear();
  h.push_back(std::make_pair(std::string("If-None-Match"), std::string("*")));
  check(get_with(g_port, "/hello.txt", h, r), "If-None-Match: * 的请求");
  check_eq_i(status_of(r), 304, "If-None-Match: * 应当 304");

  // 弱比较：W/ 前缀照样命中
  h.clear();
  h.push_back(std::make_pair(std::string("If-None-Match"), std::string("W/") + etag));
  check(get_with(g_port, "/hello.txt", h, r), "带 W/ 的 If-None-Match");
  check_eq_i(status_of(r), 304, "W/ 前缀应当按弱比较命中");

  // 不匹配 → 200 全量
  h.clear();
  h.push_back(std::make_pair(std::string("If-None-Match"), std::string("\"nope\"")));
  check(get_with(g_port, "/hello.txt", h, r), "错误 ETag 的请求");
  check_eq_i(status_of(r), 200, "ETag 不匹配应当 200");
  check_eq(body_of(r), "hello static", "不匹配时要回全量内容");

  // If-Modified-Since：用响应里的 Last-Modified 回问 → 304
  h.clear();
  h.push_back(std::make_pair(std::string("If-Modified-Since"), lm));
  check(get_with(g_port, "/hello.txt", h, r), "带 If-Modified-Since 的请求");
  check_eq_i(status_of(r), 304, "If-Modified-Since 命中应当 304");

  // 早于文件的时间 → 200
  h.clear();
  h.push_back(std::make_pair(std::string("If-Modified-Since"),
                             std::string("Sun, 06 Nov 1994 08:49:37 GMT")));
  check(get_with(g_port, "/hello.txt", h, r), "旧日期的 If-Modified-Since");
  check_eq_i(status_of(r), 200, "早于文件的日期应当 200");

  // **优先级**：If-None-Match 存在时 If-Modified-Since 必须被忽略。
  // 这里给一个"命中时间"的日期 + 一个不匹配的 ETag，正确行为是 200 ——
  // 反过来（回 304）会让客户端永远用着旧内容。
  h.clear();
  h.push_back(std::make_pair(std::string("If-None-Match"), std::string("\"nope\"")));
  h.push_back(std::make_pair(std::string("If-Modified-Since"), lm));
  check(get_with(g_port, "/hello.txt", h, r), "两个条件头同时给的请求");
  check_eq_i(status_of(r), 200,
             "If-None-Match 存在时 If-Modified-Since 必须被忽略（RFC 7232 §3.3）");

  // 关了 etag/last_modified 的挂载：两个头都不该出现，条件请求也不该 304
  check(get(g_port, "/plain/hello.txt", r), "/plain 的请求");
  check_eq(header_of(r, "etag"), "", "etag=false 时不该发 ETag");
  check_eq(header_of(r, "last-modified"), "", "last_modified=false 时不该发 Last-Modified");
  check_eq(header_of(r, "accept-ranges"), "", "range=false 时不该发 Accept-Ranges");

  h.clear();
  h.push_back(std::make_pair(std::string("If-None-Match"), etag));
  check(get_with(g_port, "/plain/hello.txt", h, r), "/plain 带 If-None-Match");
  check_eq_i(status_of(r), 200, "关掉 etag 之后不该再回 304");
}

// ---- 4. Range / 206 / 416 ----

void test_range() {
  std::cout << "[static] range" << std::endl;
  uvcpp_http_response r;
  const std::string whole = big_content();
  const long long total = static_cast<long long>(whole.size());

  // 全量：应当声明支持 Range
  check(get(g_port, "/big.bin", r), "/big.bin 全量请求");
  check_eq_i(status_of(r), 200, "无 Range 时应当 200");
  check_eq(header_of(r, "accept-ranges"), "bytes", "应当声明 Accept-Ranges: bytes");
  check_eq_i(static_cast<long long>(r.body.size()), total, "全量 body 长度");

  // 前缀区间
  std::vector<std::pair<std::string, std::string> > h;
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-4")));
  check(get_with(g_port, "/big.bin", h, r), "bytes=0-4");
  check_eq_i(status_of(r), 206, "Range 命中应当 206");
  check_eq(body_of(r), whole.substr(0, 5), "bytes=0-4 的内容");
  check_eq(header_of(r, "content-range"),
           "bytes 0-4/" + std::to_string(total), "Content-Range（前缀区间）");

  // 开区间：从中间到末尾
  h.clear();
  h.push_back(std::make_pair(std::string("Range"),
                             std::string("bytes=") + std::to_string(total - 3) + "-"));
  check(get_with(g_port, "/big.bin", h, r), "bytes=N-");
  check_eq_i(status_of(r), 206, "开区间应当 206");
  check_eq(header_of(r, "content-range"),
           "bytes " + std::to_string(total - 3) + "-" + std::to_string(total - 1) +
               "/" + std::to_string(total),
           "Content-Range（开区间）");
  check_eq_i(static_cast<long long>(r.body.size()), 3, "开区间长度");

  // 后缀区间：最后 N 字节
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=-5")));
  check(get_with(g_port, "/big.bin", h, r), "bytes=-5");
  check_eq_i(status_of(r), 206, "后缀区间应当 206");
  check_eq(header_of(r, "content-range"),
           "bytes " + std::to_string(total - 5) + "-" + std::to_string(total - 1) +
               "/" + std::to_string(total),
           "Content-Range（后缀区间）");

  // 右端越界 → 截到末尾（规范允许）
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=100-999999999")));
  check(get_with(g_port, "/big.bin", h, r), "右端越界");
  check_eq_i(status_of(r), 206, "右端越界应当 206（截到末尾）");
  check_eq_i(static_cast<long long>(r.body.size()), total - 100, "右端越界时的长度");

  // 起点越界 → 416 且带 `bytes */总长`
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=999999999-")));
  check(get_with(g_port, "/big.bin", h, r), "起点越界");
  check_eq_i(status_of(r), 416, "起点越界应当 416（不是 200）");
  check_eq(header_of(r, "content-range"), "bytes */" + std::to_string(total),
           "416 必须带 Content-Range: bytes */总长");

  // 多区间：本模块不做 multipart，按规范**允许忽略** → 200 全量
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-4,10-14")));
  check(get_with(g_port, "/big.bin", h, r), "多区间");
  check_eq_i(status_of(r), 200, "多区间应当回 200 全量（RFC 7233 §3.1 允许忽略）");
  check_eq_i(static_cast<long long>(r.body.size()), total, "多区间时给全量");

  // 语法正确但右边比左边小 → 416
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=10-5")));
  check(get_with(g_port, "/big.bin", h, r), "bytes=10-5");
  check_eq_i(status_of(r), 416, "左大于右应当 416");

  // 完全看不懂的 Range → 忽略，回 200
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("items=0-5")));
  check(get_with(g_port, "/big.bin", h, r), "items=0-5");
  check_eq_i(status_of(r), 200, "非 bytes 单位应当忽略 Range");

  // 空文件上的任何 Range 都是不可满足的
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-0")));
  check(get_with(g_port, "/empty.txt", h, r), "空文件的 Range");
  check_eq_i(status_of(r), 416, "空文件上的 Range 应当 416");
  check_eq(header_of(r, "content-range"), "bytes */0", "空文件的 416 Content-Range");

  // 关掉 range 的挂载：Range 被忽略、不发 Accept-Ranges
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-4")));
  check(get_with(g_port, "/plain/big.bin", h, r), "/plain 带 Range");
  check_eq_i(status_of(r), 200, "range=false 时应当忽略 Range 回 200");
  check_eq_i(static_cast<long long>(r.body.size()), total, "range=false 时要回全量");
}

// ---- 5. If-Range ----

void test_if_range() {
  std::cout << "[static] if_range" << std::endl;
  uvcpp_http_response r;

  check(get(g_port, "/big.bin", r), "取 /big.bin 的 ETag");
  const std::string etag = header_of(r, "etag");
  const std::string lm = header_of(r, "last-modified");
  check(!etag.empty() && !lm.empty(), "需要 ETag 与 Last-Modified 才能测 If-Range");

  // ETag 对得上 → 按 Range 回 206
  std::vector<std::pair<std::string, std::string> > h;
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-4")));
  h.push_back(std::make_pair(std::string("If-Range"), etag));
  check(get_with(g_port, "/big.bin", h, r), "If-Range 命中");
  check_eq_i(status_of(r), 206, "If-Range 命中时应当按 Range 回 206");

  // ETag 对不上 → **忽略 Range，回 200 全量**（不是 412）
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-4")));
  h.push_back(std::make_pair(std::string("If-Range"), std::string("\"stale\"")));
  check(get_with(g_port, "/big.bin", h, r), "If-Range 不匹配");
  check_eq_i(status_of(r), 200, "If-Range 不匹配应当回 200 全量（不是 412）");
  check_eq_i(static_cast<long long>(r.body.size()), static_cast<long long>(big_content().size()),
             "If-Range 不匹配时给的是完整内容");

  // 日期形式：命中 → 206；过期 → 200
  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-4")));
  h.push_back(std::make_pair(std::string("If-Range"), lm));
  check(get_with(g_port, "/big.bin", h, r), "If-Range 用日期（命中）");
  check_eq_i(status_of(r), 206, "日期命中时应当 206");

  h.clear();
  h.push_back(std::make_pair(std::string("Range"), std::string("bytes=0-4")));
  h.push_back(std::make_pair(std::string("If-Range"),
                             std::string("Sun, 06 Nov 1994 08:49:37 GMT")));
  check(get_with(g_port, "/big.bin", h, r), "If-Range 用旧日期");
  check_eq_i(status_of(r), 200, "旧日期应当回 200 全量");
}

// ---- 6. HEAD ----

void test_head() {
  std::cout << "[static] head" << std::endl;

  raw_result rr = raw_exchange(g_port, raw_head_request("HEAD", "/big.bin"), 3000);
  check(rr.ok, "HEAD /big.bin 应当有响应");
  check_eq_i(raw_status(rr.raw), 200, "HEAD 的状态码");
  check_eq_i(static_cast<long long>(raw_body(rr.raw).size()), 0,
             "HEAD **一个 body 字节都不能有**");
  check_eq(raw_header(rr.raw, "content-length"),
           std::to_string(big_content().size()),
           "HEAD 的 Content-Length 必须和 GET 一致");
  check_eq(raw_header(rr.raw, "content-type"), "application/octet-stream",
           "HEAD 应当带 Content-Type");
  check(!raw_header(rr.raw, "etag").empty(), "HEAD 应当带 ETag");

  // 不存在的文件：HEAD 也要 404（不能因为不读 body 就跳过存在性判断）
  rr = raw_exchange(g_port, raw_head_request("HEAD", "/nope.txt"), 3000);
  check(rr.ok, "HEAD /nope.txt 应当有响应");
  check_eq_i(raw_status(rr.raw), 404, "HEAD 一个不存在的文件应当 404");

  // HEAD + Range：206 与 Content-Range 都要有，仍然没有 body
  rr = raw_exchange(
      g_port,
      "HEAD /big.bin HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
      "Range: bytes=0-4\r\n\r\n",
      3000);
  check(rr.ok, "HEAD + Range 应当有响应");
  check_eq_i(raw_status(rr.raw), 206, "HEAD + Range 应当 206");
  check_eq_i(static_cast<long long>(raw_body(rr.raw).size()), 0,
             "HEAD + Range 也不能有 body");
  check_eq(raw_header(rr.raw, "content-length"), "5",
           "HEAD + Range 的 Content-Length 应当是区间长度");

  // HEAD 命中 ETag → 304，仍然没有 body
  check(true, "占位");
  uvcpp_http_response r;
  check(get(g_port, "/hello.txt", r), "取 /hello.txt 的 ETag");
  const std::string etag = header_of(r, "etag");
  rr = raw_exchange(g_port,
                    "HEAD /hello.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Connection: close\r\nIf-None-Match: " + etag + "\r\n\r\n",
                    3000);
  check(rr.ok, "HEAD + If-None-Match 应当有响应");
  check_eq_i(raw_status(rr.raw), 304, "HEAD 的条件请求命中应当 304");
  check_eq_i(static_cast<long long>(raw_body(rr.raw).size()), 0, "304 不能有 body");
}

// ---- 7. 尺寸闸门 ----

void test_size_gate() {
  std::cout << "[static] size_gate" << std::endl;
  uvcpp_http_response r;

  // `/capped` 的上限是 1 KiB，big.bin 是 256 KiB
  check(get(g_port, "/capped/big.bin", r), "/capped/big.bin 应当有响应");
  check_eq_i(status_of(r), 413, "超过尺寸上限应当 413");
  check_eq_i(static_cast<long long>(r.body.size()) > 0 ? 1 : 0, 1,
             "413 应当有一个可读的说明 body");

  // 上限之内照常
  check(get(g_port, "/capped/hello.txt", r), "/capped/hello.txt 应当有响应");
  check_eq_i(status_of(r), 200, "上限之内的文件应当正常");
  check_eq(body_of(r), "hello static", "上限之内的内容");
}

// ---- 8. dotfile 策略 ----

void test_dotfiles() {
  std::cout << "[static] dotfiles" << std::endl;
  uvcpp_http_response r;

  // 默认 HIDE：404，且**绝不泄漏内容**
  check(get(g_port, "/.env", r), "/.env 应当有响应");
  check_eq_i(status_of(r), 404, "默认策略下 dotfile 应当 404（不是 403）");
  check_hides(body_of(r), "SECRET=in-dotfile", "dotfile 的内容不能出现在响应里");

  check(get(g_port, "/.hash.txt", r), "/.hash.txt 应当有响应");
  check_eq_i(status_of(r), 404, "默认策略下 .hash.txt 也应当 404");

  // DENY：403（运维要看得出有人在扫）
  check(get(g_port, "/deny/.env", r), "/deny/.env 应当有响应");
  check_eq_i(status_of(r), 403, "DENY 策略下应当 403");
  check_hides(body_of(r), "SECRET=in-dotfile", "403 也不能泄漏内容");

  // ALLOW：照常提供
  check(get(g_port, "/allow/.env", r), "/allow/.env 应当有响应");
  check_eq_i(status_of(r), 200, "ALLOW 策略下应当 200");
  check_eq(body_of(r), "SECRET=in-dotfile", "ALLOW 策略下应当是文件内容");

  // 非 dotfile 不受影响
  check(get(g_port, "/deny/hello.txt", r), "DENY 挂载下的普通文件");
  check_eq_i(status_of(r), 200, "dotfile 策略不该影响普通文件");
}

// ---- 9. 路径穿越（安全核心）----

void test_traversal() {
  std::cout << "[static] traversal" << std::endl;
  uvcpp_http_response r;

  // 前提：秘密文件确实存在且在根外 —— 否则这组用例全是假绿。
  std::string secret;
  check(read_file(k_secret, secret) && secret == k_secret_text,
        "靶子文件必须存在且在文档根之外（否则穿越用例没有意义）");

  // 各种编码与写法。**每一条都同时验状态码和内容**。
  const char* attacks[] = {
      "/../uvcpp_static_test_secret.txt",
      "/..%2fuvcpp_static_test_secret.txt",
      "/%2e%2e/uvcpp_static_test_secret.txt",
      "/%2e%2e%2fuvcpp_static_test_secret.txt",
      "/sub/../../uvcpp_static_test_secret.txt",
      "/sub/..%2f..%2fuvcpp_static_test_secret.txt",
      "/./../uvcpp_static_test_secret.txt",
      "/deep/inner/../../../uvcpp_static_test_secret.txt",
      "/....//uvcpp_static_test_secret.txt",
      "/%252e%252e/uvcpp_static_test_secret.txt",
      "/C:/Windows/win.ini",
      "/..\\uvcpp_static_test_secret.txt",
  };
  const size_t n = sizeof(attacks) / sizeof(attacks[0]);
  for (size_t i = 0; i < n; ++i) {
    const std::string path(attacks[i]);
    if (!get(g_port, path, r)) {
      check(false, "穿越请求 " + path + " 没有拿到响应");
      continue;
    }
    const int st = status_of(r);
    if (st == 200) {
      check(false, "穿越请求 " + path + " 回了 200");
    }
    check_hides(body_of(r), k_secret_text, "穿越请求 " + path);
    check_hides(body_of(r), "[extensions]", "穿越请求 " + path + " 不该读到 win.ini");
  }

  // 开了 SPA 回退之后，穿越**仍然**要挡住（回退只对"不存在"生效）
  check(get(g_port, "/spa/../uvcpp_static_test_secret.txt", r), "SPA 挂载下的穿越");
  const int st = status_of(r);
  check_hides(body_of(r), k_secret_text, "SPA 回退不得把穿越变成一个 200");
  check(st != 200 || body_of(r).find("ROOT-INDEX") != std::string::npos,
        "SPA 挂载下的穿越即使有响应也不能是秘密文件（状态 " +
            std::to_string(st) + "）");
}

// ---- 9b. 链接逃逸（第 2 道安全边界）----

/**
 * @brief 一条**文本上完全合法**、却指向根外的路径。
 *
 * 上面那组穿越用例打的是第 1 道边界（`web_sanitize_path` 的文本归一化）：
 * `..%2f`、`C:`、反斜杠……每一条都能在字符串层面被认出来。这一组打的是
 * 第 2 道（`web_resolve_within_root` 的真实路径包含判断）—— `/escape/x.txt`
 * 里没有 `..`、没有编码、没有反斜杠，**第 1 道无从下手**，只有把链接展开成
 * 真实路径再比一次前缀才挡得住。
 *
 * 所以这组用例是第 2 道边界**唯一**的覆盖：它挂掉时上面那组穿越用例会
 * 全绿。
 */
void test_symlink_escape() {
  std::cout << "[static] symlink_escape" << std::endl;
  uvcpp_http_response r;

  // 前提：链接真的建起来了、目标真的在根外。否则这组用例会**静默地**变成
  // "什么都没测" —— 一条拿不到响应的断言在只看状态码时和通过没区别。
  std::string outside;
  check(read_file(k_outside_secret, outside) && outside == k_outside_text,
        "链接目标必须存在且在文档根之外（否则逃逸用例没有意义）");

  // (1) 逃逸必须被挡住。状态码不钉死（404/403 都算合理），**内容**钉死。
  check(get(g_port, "/escape/secret2.txt", r), "/escape/secret2.txt 应当有响应");
  check_hides(body_of(r), k_outside_text,
              "经链接读到根外文件 —— 第 2 道边界没挡住");
  check(status_of(r) != 200, "经链接取根外文件不该回 200");

  // (2) 链接本身（当目录访问）也不能把根外目录的索引漏出来
  check(get(g_port, "/escape/", r), "/escape/ 应当有响应");
  check_hides(body_of(r), k_outside_text, "经链接列根外目录");

  // (3) **对照**：指向根**内**的链接必须照常工作。
  //     没有这一条，一个"见到链接就拒绝"的实现也能让上面两条通过。
  check(get(g_port, "/inside/a.txt", r), "/inside/a.txt 应当有响应");
  check_eq_i(status_of(r), 200, "指向根内的链接应当照常服务");
  check_eq(body_of(r), "sub a", "指向根内的链接应当给出目标文件的内容");
}

// ---- 10. SPA 回退 ----

void test_spa_fallback() {
  std::cout << "[static] spa_fallback" << std::endl;
  uvcpp_http_response r;

  // 关着的时候：不存在就是 404
  check(get(g_port, "/user/42", r), "/user/42 应当有响应");
  check_eq_i(status_of(r), 404, "没开 SPA 时前端路由应当 404");

  // 开着的时候：回落到入口文件
  check(get(g_port, "/spa/user/42", r), "/spa/user/42 应当有响应");
  check_eq_i(status_of(r), 200, "开了 SPA 时应当 200");
  check_eq(body_of(r), "<h1>ROOT-INDEX</h1>", "SPA 回落应当给入口文件的内容");

  // 真实存在的文件优先，不能一律回落到入口
  check(get(g_port, "/spa/hello.txt", r), "/spa/hello.txt 应当有响应");
  check_eq(body_of(r), "hello static", "存在的文件必须优先于 SPA 回落");
  check_eq_i(status_of(r), 200, "存在的文件状态码");

  // 深层不存在路径也回落
  check(get(g_port, "/spa/a/b/c/d", r), "/spa/a/b/c/d 应当有响应");
  check_eq(body_of(r), "<h1>ROOT-INDEX</h1>", "深层不存在路径也应当回落");
}

// ---- 11. 缓存：命中、失效 ----

void test_cache() {
  std::cout << "[static] cache" << std::endl;
  uvcpp_http_response r;

  check(get(g_port, "/cache/hello.txt", r), "第一次取 /cache/hello.txt");
  check_eq(body_of(r), "hello static", "第一次的内容");

  check(get(g_port, "/cache/hello.txt", r), "第二次取 /cache/hello.txt");
  check_eq(body_of(r), "hello static", "第二次的内容");

  // **关键的失效用例**：改写文件（长度也变了，所以不可能撞上旧快照），
  // 下一次请求必须拿到**新内容**。只比命中计数的话，一个永远返回旧内容的
  // 缓存也能让计数很好看。
  check(write_file(std::string(k_root) + "/hello.txt", "hello REWRITTEN longer"),
        "改写文档根里的文件");
  check(get(g_port, "/cache/hello.txt", r), "改写后取 /cache/hello.txt");
  check_eq(body_of(r), "hello REWRITTEN longer",
           "文件改了之后缓存必须失效（拿到新内容）");
  check_ne(header_of(r, "content-length"), "12", "改写后长度头也要跟着变");

  // 改回来，免得影响后面的用例（后面的用例读的是同一个文件）
  check(write_file(std::string(k_root) + "/hello.txt", "hello static"), "还原文件");
  check(get(g_port, "/cache/hello.txt", r), "还原后再取一次");
  check_eq(body_of(r), "hello static", "还原后应当拿到还原的内容");

  // 失效之后必须能**重新命中** —— 只测"改了会变"是不够的，一个每次
  // 重读、从不写回缓存的实现也能通过前面那些断言，而它等于没有缓存。
  check(get(g_port, "/cache/hello.txt", r), "还原后再取第二次");
  check_eq(body_of(r), "hello static", "第三次读同一份内容");
}

// ---- 12. 各种选项 ----

void test_options() {
  std::cout << "[static] options" << std::endl;
  uvcpp_http_response r;

  // `Cache-Control` 原样发出
  check(get(g_port, "/cc/hello.txt", r), "/cc/hello.txt 应当有响应");
  check_eq(header_of(r, "cache-control"), "public, max-age=3600",
           "Cache-Control 应当原样发出");
  check(get(g_port, "/hello.txt", r), "没配 Cache-Control 的挂载");
  check_eq(header_of(r, "cache-control"), "", "没配就不该发 Cache-Control");

  // add_charset=false
  check(get(g_port, "/plain/index.html", r), "/plain/index.html 应当有响应");
  check_eq(header_of(r, "content-type"), "text/html",
           "add_charset=false 时不该补 charset");

  // 自定义 index 顺序：/custom 挂载只认 home.html
  check(get(g_port, "/custom", r), "/custom 应当有响应");
  check_eq(body_of(r), "<h1>HOME</h1>", "自定义索引文件名应当生效");
}

}  // namespace

// =========================================================================
// main
// =========================================================================

/**
 * 只跑某一组用例，`UVCPP_STATIC_TEST_ONLY=range` 这样。
 *
 * 用途是把"这套用例里有一处偶发崩溃"缩到一组之内 —— 全量跑一次要两秒，
 * 而崩溃只有一半概率出现，不缩范围的话根本分不清是哪一组。
 */
bool want(const char* name) {
  const char* only = std::getenv("UVCPP_STATIC_TEST_ONLY");
  if (only == nullptr || only[0] == '\0') return true;
  return std::strcmp(only, name) == 0;
}

int main() {
  std::cout << std::unitbuf;

  if (!build_root()) {
    std::cout << "[static] 无法搭建测试文档根，跳过" << std::endl;
    return 2;
  }

  int rc = 0;
  {
    uvcpp_web_app app;
    app.set_port(0);
    app.set_log_level(log_level::WARN);

    // 默认选项那一份 —— 目录就是测试根
    std::shared_ptr<uvcpp_web_static> dflt = app.serve_static("/", k_root);
    std::shared_ptr<uvcpp_web_static> named =
        app.serve_static("/static", k_root);

    // 各选项组合
    uvcpp_web_static_options plain;
    plain.etag = false;
    plain.last_modified = false;
    plain.range = false;
    plain.add_charset = false;
    plain.cache_max_entries = 0;  // 顺带覆盖"关缓存"这条路
    app.serve_static("/plain", k_root, plain);

    uvcpp_web_static_options deny;
    deny.dotfiles = uvcpp_web_dotfile_policy::DENY;
    app.serve_static("/deny", k_root, deny);

    uvcpp_web_static_options allow;
    allow.dotfiles = uvcpp_web_dotfile_policy::ALLOW;
    app.serve_static("/allow", k_root, allow);

    uvcpp_web_static_options spa;
    spa.spa_fallback = true;
    app.serve_static("/spa", k_root, spa);

    uvcpp_web_static_options capped(1024);
    app.serve_static("/capped", k_root, capped);

    uvcpp_web_static_options cc;
    cc.cache_control = "public, max-age=3600";
    std::shared_ptr<uvcpp_web_static> cc_only = app.serve_static("/cc", k_root, cc);

    uvcpp_web_static_options custom;
    custom.index_files.clear();
    custom.index_files.push_back("home.html");
    custom.index_files.push_back("index.html");
    app.serve_static("/custom", k_root, custom);

    std::shared_ptr<uvcpp_web_static> cached = app.serve_static("/cache", k_root);

    // 自定义索引用例要的文件，得在 App 起来之前放进文档根
    write_file(std::string(k_root) + "/home.html", "<h1>HOME</h1>");

    if (app.start_background() != 0) {
      std::cerr << "[static] 服务启动失败" << std::endl;
      cleanup_root();
      return 2;
    }
    g_port = app.bound_port();
    if (g_port <= 0) {
      std::cerr << "[static] 端口无效" << std::endl;
      app.stop();
      app.join();
      cleanup_root();
      return 2;
    }

    if (want("basic")) test_basic_and_mime();
    if (want("index")) test_index_and_prefix();
    if (want("etag")) test_etag_and_304();
    if (want("range")) test_range();
    if (want("ifrange")) test_if_range();
    if (want("head")) test_head();
    if (want("size")) test_size_gate();
    if (want("dotfiles")) test_dotfiles();
    if (want("traversal")) test_traversal();
    if (want("symlink")) test_symlink_escape();
    if (want("spa")) test_spa_fallback();
    if (want("cache")) test_cache();
    if (want("options")) test_options();

    app.stop();
    app.join();

    if (!want("cache")) { /* 单组模式下没跑缓存用例，计数对不上是应该的 */ }
    else {

    // 计数只能在 App 停干净之后读 —— loop 线程已经退出，不再有并发写。
    std::cout << "[static] cache: hits=" << cached->cache_hits()
              << " misses=" << cached->cache_misses()
              << " entries=" << cached->cache_entries()
              << " bytes=" << cached->cache_bytes() << std::endl;

    // /cache 上一共 5 次请求：初次 miss、二次 hit、改写后 miss、还原后 miss、
    // 最后那次 hit。计数是这套设计里唯一"看得见"的东西，所以钉死具体数字，
    // 而不是 `hits > 0` 这种怎么都能过的断言。
    check_eq_i(static_cast<long long>(cached->cache_misses()), 3,
               "cache：初次 + 改写后 + 还原后 = 3 次未命中");
    check_eq_i(static_cast<long long>(cached->cache_hits()), 2,
               "cache：两次重复请求 = 2 次命中");
    check(cached->cache_entries() >= 1, "cache：条目数应当 >= 1");
    check(cached->cache_bytes() > 0, "cache：字节数应当 > 0");

    // 每个挂载是**独立**的缓存实例，不该互相串味：`/cc` 只被取过一次，
    // 它的计数必须干干净净，不能沾上 /cache 那一串。
    check_eq_i(static_cast<long long>(cc_only->cache_hits()), 0,
               "/cc 不该有命中（它只被取过一次）");
    check_eq_i(static_cast<long long>(cc_only->cache_misses()), 1,
               "/cc 应当恰好未命中一次");

    // clear_cache 之后计数保留、条目清零
    cached->clear_cache();
    check_eq_i(static_cast<long long>(cached->cache_entries()), 0,
               "clear_cache 之后条目应当清零");
    check_eq_i(static_cast<long long>(cached->cache_bytes()), 0,
               "clear_cache 之后字节应当清零");
    check_eq_i(static_cast<long long>(cached->cache_hits()), 2,
               "clear_cache 不该重置命中统计（那是历史，不是状态）");

    }  // want("cache")

    (void)dflt;
  }

  cleanup_root();

  if (g_failures == 0) {
    std::cout << "[static] ALL PASS" << std::endl;
    return 0;
  }
  rc = 2;
  std::cout << "[static] FAIL (" << g_failures << " checks failed)" << std::endl;
  return rc;
}

#endif  // UVCPP_WEBAPP_ENABLE
