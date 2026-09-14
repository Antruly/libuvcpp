/**
 * @file src/web/uvcpp_static_server.cpp
 * @brief 静态文件服务实现（懒加载 + mtime 校验缓存 + 线程池异步读盘）。
 */

#include <web/uvcpp_static_server.h>

#include <fstream>
#include <sstream>
#include <vector>

#include <req/uvcpp_fs.h>
#include <req/uvcpp_work.h>

namespace uvcpp {

namespace {

/**
 * @brief 段级归一化：丢掉空段与 `.`，`..` 弹栈（**栈空 = 越界，直接失败**）。
 *
 * 为什么不是 `url.find("..")`：那个写法两头都不对。
 *   - **误杀**：`/js/lib..min.js` 是个完全合法的文件名，子串匹配会拒掉它；
 *   - **说不清边界**：它匹配的是"出现过两个点"，而不是"爬到根外面去了"。
 *     段级归一化匹配的是后者。
 *
 * 越界时**不做静默截断** —— 截断会把一次攻击变成一个看起来正常的请求，
 * 日志里再也看不出来。
 *
 * 同时挡掉三类在 Windows 上会被当成路径语法的字节：
 *   - `\`：它和 `/` 一样是分隔符，`..\..\` 由它绕过；
 *   - `:`：盘符（`C:`）与 NTFS 数据流（`file:stream`）；
 *   - NUL 与控制字符：NUL 会在任何 C 字符串 API 处截断。
 *
 * @note 本类**不解码百分号**（`req.url` 是解析器原样给出的），所以
 *       `%2e%2e%2f` 到这里仍是一个普通文件名，它不构成穿越。这一层是纯文本
 *       检查，不碰文件系统 —— 符号链接不在它的能力范围内。
 */
bool normalize_url_path(std::string& url) {
  for (size_t i = 0; i < url.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(url[i]);
    if (c == '\0' || c < 0x20 || c == 0x7f) return false;
    if (c == '\\' || c == ':') return false;
  }

  std::vector<std::string> stack;
  size_t i = 0;
  while (i <= url.size()) {
    size_t j = url.find('/', i);
    if (j == std::string::npos) j = url.size();
    const std::string seg = url.substr(i, j - i);
    i = j + 1;

    if (seg.empty() || seg == ".") {
      if (j >= url.size()) break;
      continue;
    }
    if (seg == "..") {
      if (stack.empty()) return false;  // 爬出根了
      stack.pop_back();
      if (j >= url.size()) break;
      continue;
    }
    stack.push_back(seg);
    if (j >= url.size()) break;
  }

  std::string out;
  for (size_t k = 0; k < stack.size(); ++k) {
    out += '/';
    out += stack[k];
  }
  if (out.empty()) out = "/";
  url = out;
  return true;
}

// worker 线程专用：二进制读文件（阻塞磁盘 IO 放在线程池，不卡事件循环）
bool read_file_binary(const std::string& path, std::string& out) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if (!f.is_open()) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

/**
 * @brief worker 线程专用：取元数据。
 *
 * 用的是 `uv_fs_stat` 的**同步**形态（回调传 NULL）。它不碰 loop 的任何
 * 内部状态 —— libuv 在 `cb == NULL` 时直接就地跑 `uv__fs_work`，不注册
 * request、不提交线程池 —— 所以从工作线程调用是安全的，也正是我们要的：
 * 阻塞发生在线程池里，事件循环那边一无所知。
 *
 * 三态而不是两态，因为"目录"和"不存在"要走不同的后续：目录要拼 index 文件
 * （`/sub/` → `/sub/index.html`），不存在才轮到 SPA 回退。混成一个 false
 * 会让每个 404 都白跑一次 `xxx/index.html` 的 stat。
 */
enum class stat_kind { MISSING, FILE, DIR };

stat_kind stat_path(uvcpp_loop* loop, const std::string& path,
                    int64_t& mtime_sec, int64_t& mtime_nsec, int64_t& size) {
  uvcpp_fs fs;
  if (fs.stat(loop, path.c_str()) != 0) {
    fs.req_cleanup();
    return stat_kind::MISSING;
  }
  uv_stat_t* st = fs.get_statbuf();
  // statbuf 只在 req_cleanup() 之前有效，所以这里必须先把值取出来。
  stat_kind kind = stat_kind::MISSING;
  if (st != nullptr) {
    if ((st->st_mode & S_IFMT) == S_IFDIR) {
      kind = stat_kind::DIR;
    } else if ((st->st_mode & S_IFMT) == S_IFREG) {
      kind = stat_kind::FILE;
      mtime_sec = static_cast<int64_t>(st->st_mtim.tv_sec);
      mtime_nsec = static_cast<int64_t>(st->st_mtim.tv_nsec);
      size = static_cast<int64_t>(st->st_size);
    }
  }
  fs.req_cleanup();
  return kind;
}

}  // namespace

// 异步 stat 任务：worker 线程取元数据，loop 线程决定下一步走向。
struct uvcpp_static_server::StatTask {
  uvcpp_static_server* self;
  uvcpp_http_server* server;
  uvcpp_tcp_client* client;
  uint64_t generation;      // 发起时的连接代次（回来时要对得上）
  uvcpp_loop* loop;         // 从 loop 线程记下来的，worker 里不能再碰 client

  std::string fs_path;      // 当前要 stat 的本地路径
  std::string url_path;     // 对应的 URL 路径（缓存键）
  std::string spa_fs_path;  // SPA 回退目标（空 = 不回退）
  std::string spa_url_path;

  int step;                 // 0=原始目标 1=目录拼 index 2=SPA 回退
  stat_kind kind;           // worker 产出：FILE / DIR / MISSING
  int64_t mtime_sec;
  int64_t mtime_nsec;
  int64_t size;
};

// 异步加载任务上下文（worker 线程与事件循环线程之间传递）
struct uvcpp_static_server::LoadTask {
  uvcpp_static_server* self;    // 事件循环线程回调用（更新缓存）
  uvcpp_http_server* server;    // deferred 响应发送
  uvcpp_tcp_client* client;     // 客户端连接
  uint64_t generation;          // 发起时的连接代次
  std::string fs_path;          // 本地文件路径
  std::string cache_key;        // 缓存键（URL 路径）
  int64_t mtime_sec;            // serve 阶段 stat 到的 mtime
  int64_t mtime_nsec;
  int64_t size;
  bool ok;                      // worker 线程读盘结果
  std::shared_ptr<std::string> data;  // worker 线程填充的文件字节
};

uvcpp_static_server::uvcpp_static_server() {}

uvcpp_static_server::uvcpp_static_server(const std::string& local_dir,
                                         const std::string& url_root,
                                         const std::string& index_file)
    : local_dir_(local_dir), url_root_(url_root), index_file_(index_file) {
  // local_dir 去尾部斜杠
  while (!local_dir_.empty() &&
         (local_dir_[local_dir_.size() - 1] == '/' ||
          local_dir_[local_dir_.size() - 1] == '\\')) {
    local_dir_.resize(local_dir_.size() - 1);
  }
  // url_root 规范化：保证以 / 开头，非根时去掉尾部 /
  if (url_root_.empty()) url_root_ = "/";
  if (url_root_[0] != '/') url_root_ = "/" + url_root_;
  if (url_root_.size() > 1 && url_root_[url_root_.size() - 1] == '/') {
    url_root_.resize(url_root_.size() - 1);
  }
  // index_file 去前导 /
  while (!index_file_.empty() && index_file_[0] == '/') index_file_.erase(0, 1);
}

uvcpp_static_server::~uvcpp_static_server() {
  // 析构前把挂起的 `uvcpp_work` 释放掉，否则它们再没人管了。
  //
  // 注意这**不能**保证"析构之后没有回调再来" —— 在途任务捕获的 `self` 仍是
  // 本对象。那条约定是 `handler()` 文档里写的「本对象生命周期必须不短于
  // server」，属于调用方的责任，这里只是把已经退役的那批还掉。
  drain_retired();
}

void uvcpp_static_server::drain_retired() {
  for (size_t i = 0; i < retired_.size(); ++i) delete retired_[i];
  retired_.clear();
}

http_request_handler uvcpp_static_server::handler(uvcpp_http_server* server) {
  uvcpp_static_server* self = this;
  return [self, server](uvcpp_http_request& req, uvcpp_http_response& resp,
                        uvcpp_tcp_client* client) {
    self->serve(req, resp, client, server);
  };
}

bool uvcpp_static_server::resolve_url(const std::string& raw_url,
                                      std::string& url_path) const {
  // 剥离 query
  std::string url = raw_url;
  size_t q = url.find('?');
  if (q != std::string::npos) url = url.substr(0, q);

  // **先剥挂载前缀，再判"是不是根"** —— 顺序反了 `/static` 和 `/static/`
  // 都进不了索引分支：前者被剥成空串、后者被剥成 "/"，两个都会被直接拿去拼
  // 文件系统路径（得到 `dist/` 或 `dist/`），于是 Windows 上 404、
  // Linux 上 stat 成功但把目录当文件读 → 一个空 body 的 200。
  if (url_root_.size() > 1) {
    if (url.compare(0, url_root_.size(), url_root_) != 0) return false;
    // 挂载点是**路径段**，不是字符串前缀：`/staticfoo` 不该被当成
    // "/static" + "foo"（那会跑到 dist/foo，等于从挂载点里溜出去）。
    if (url.size() > url_root_.size() && url[url_root_.size()] != '/') {
      return false;
    }
    url = url.substr(url_root_.size());
  }

  // 空或根 → 索引文件
  if (url.empty() || url == "/") {
    url_path = "/" + index_file_;
    return true;
  }

  if (!normalize_url_path(url)) return false;

  url_path = url;
  return true;
}

void uvcpp_static_server::stat_async(std::string fs_path, std::string url_path,
                                     std::string spa_fs_path,
                                     std::string spa_url_path, int step,
                                     uint64_t generation,
                                     uvcpp_tcp_client* client,
                                     uvcpp_http_server* server) {
  StatTask* task = new StatTask();
  task->self = this;
  task->server = server;
  task->client = client;
  task->generation = generation;
  // **在 loop 线程上把 loop 指针记下来**。worker 里绝不能再通过 client 去取
  // 它 —— 那时连接可能已经被回收，而 loop 的生命周期覆盖整个服务器。
  task->loop = client != nullptr ? client->get_loop() : nullptr;
  task->fs_path = fs_path;
  task->url_path = url_path;
  task->spa_fs_path = spa_fs_path;
  task->spa_url_path = spa_url_path;
  task->step = step;
  task->kind = stat_kind::MISSING;
  task->mtime_sec = 0;
  task->mtime_nsec = 0;
  task->size = 0;

  uvcpp_work* work = new uvcpp_work();
  int rc = work->queue_work(
      task->loop,
      // worker 线程：只做元数据，不碰 client / server
      [task](uvcpp_work*) {
        task->kind = stat_path(task->loop, task->fs_path, task->mtime_sec,
                               task->mtime_nsec, task->size);
      },
      [task, work](uvcpp_work*, int /*status*/) {
        uvcpp_static_server* self = task->self;
        self->stat_step(task);
        // `work` 交给 `serve()` 回收 —— 详见 `retire()` 的注释。**这里之后
        // 绝不能再碰 `work`**；`task` 是另一个堆对象，`work` 还没被释放，
        // 所以读捕获变量 `task` 仍然是安全的。
        self->retire(work);
        delete task;
      });

  if (rc != 0) {
    // 线程池排队失败（极端情况）：当成"文件不存在"，走目录 index / SPA 回退。
    task->kind = stat_kind::MISSING;
    stat_step(task);
    delete task;
    delete work;
  }
}

void uvcpp_static_server::stat_step(StatTask* task) {
  // 连接已经换人了（或者断了）：**什么都别写**。地址可能已经被新连接复用，
  // 写下去就是把 A 的响应发给 B。
  if (!connection_matches(task->server, task->client, task->generation)) {
    return;
  }

  if (task->kind == stat_kind::FILE) {
    // 缓存命中且文件没变 → 内存直出（零磁盘 IO）
    if (cache_enabled_) {
      std::map<std::string, CacheEntry>::iterator it =
          cache_.find(task->url_path);
      if (it != cache_.end() && it->second.mtime_sec == task->mtime_sec &&
          it->second.mtime_nsec == task->mtime_nsec &&
          it->second.size == task->size) {
        const CacheEntry& e = it->second;
        uvcpp_http_response resp =
            uvcpp_http_response::ok(e.data->data(), e.data->size(), e.mime);
        task->server->send_response(task->client, resp);
        return;
      }
    }
    // 未命中或文件已变更 → 异步读盘（线程池），deferred 响应
    load_async(task->fs_path, task->url_path, task->mtime_sec,
               task->mtime_nsec, task->size, task->generation, task->client,
               task->server);
    return;
  }

  // step 0 且目标是目录：`/sub` 与 `/sub/` 都要落到 `/sub/index.html`。
  if (task->step == 0 && task->kind == stat_kind::DIR) {
    std::string dir = task->fs_path;
    if (!dir.empty() && dir[dir.size() - 1] != '/') dir += '/';
    dir += index_file_;

    std::string url = task->url_path;
    if (!url.empty() && url[url.size() - 1] != '/') url += '/';
    url += index_file_;

    stat_async(dir, url, task->spa_fs_path, task->spa_url_path, 1,
               task->generation, task->client, task->server);
    return;
  }

  // 还没试过 SPA 回退，且回退目标和刚 stat 的不是同一个 → 试一次。
  if (task->step < 2 && !task->spa_fs_path.empty() &&
      task->spa_fs_path != task->fs_path) {
    stat_async(task->spa_fs_path, task->spa_url_path, std::string(),
               std::string(), 2, task->generation, task->client, task->server);
    return;
  }

  uvcpp_http_response resp = uvcpp_http_response::not_found();
  task->server->send_response(task->client, resp);
}

void uvcpp_static_server::load_async(const std::string& fs_path,
                                     const std::string& cache_key, int64_t msec,
                                     int64_t mnsec, int64_t size,
                                     uint64_t generation,
                                     uvcpp_tcp_client* client,
                                     uvcpp_http_server* server) {
  LoadTask* task = new LoadTask();
  task->self = this;
  task->server = server;
  task->client = client;
  task->generation = generation;
  task->fs_path = fs_path;
  task->cache_key = cache_key;
  task->mtime_sec = msec;
  task->mtime_nsec = mnsec;
  task->size = size;
  task->ok = false;

  uvcpp_work* work = new uvcpp_work();
  int rc = work->queue_work(
      client->get_loop(),
      // worker 线程：只做磁盘 IO
      [task](uvcpp_work*) {
        task->data = std::make_shared<std::string>();
        task->ok = read_file_binary(task->fs_path, *task->data);
      },
      // 事件循环线程：更新缓存 + deferred 发送
      [task, work](uvcpp_work*, int /*status*/) {
        uvcpp_static_server* self = task->self;
        // 先把 `work` 挂起来（只是入队，不释放任何东西），之后**再也不碰它**。
        // 放在最前面是为了让"两条 return 路径都别漏了回收"这件事不可能写错。
        self->retire(work);

        uvcpp_http_response resp;
        // 连接换人了就别写 —— 见 `connection_matches()`。
        if (!connection_matches(task->server, task->client,
                                task->generation)) {
          delete task;
          return;
        }
        if (task->ok) {
          CacheEntry e;
          e.data = task->data;
          e.mime = uvcpp_static_server::mime_of(task->cache_key);
          e.mtime_sec = task->mtime_sec;
          e.mtime_nsec = task->mtime_nsec;
          e.size = task->size;
          self->cache_[task->cache_key] = e;
          resp = uvcpp_http_response::ok(e.data->data(), e.data->size(), e.mime);
        } else {
          resp = uvcpp_http_response::not_found();
        }
        task->server->send_response(task->client, resp);
        delete task;
      });

  // 极端情况：线程池排队失败，直接兜底 404 并回收
  if (rc != 0) {
    if (connection_matches(server, client, generation)) {
      uvcpp_http_response resp = uvcpp_http_response::not_found();
      server->send_response(client, resp);
    }
    delete task;
    delete work;
  }
}

bool uvcpp_static_server::connection_matches(uvcpp_http_server* server,
                                             uvcpp_tcp_client* client,
                                             uint64_t generation) {
  if (server == nullptr || client == nullptr) return false;
  return server->connection_generation(client) == generation;
}

void uvcpp_static_server::serve(uvcpp_http_request& req, uvcpp_http_response& resp,
                                uvcpp_tcp_client* client,
                                uvcpp_http_server* server) {
  // 先把上一批用完的 `uvcpp_work` 真正删掉。这里是**安全的删除点**：`serve()`
  // 由请求驱动，必然跑在 loop 线程上，而且此时不在任何 libuv 完成回调里 ——
  // 与在 after_work 回调里 delete 完全是两回事（详见 `retire()`）。
  drain_retired();

  // 仅 GET / HEAD
  if (req.method != http_method::HTTP_GET &&
      req.method != http_method::HTTP_HEAD) {
    resp = uvcpp_http_response::not_found();
    return;
  }

  std::string url_path;
  if (!resolve_url(req.url, url_path)) {
    resp = uvcpp_http_response::not_found();
    return;
  }

  // 记下这条连接此刻的代次。异步跑完回来时要靠它确认"还是同一条连接"。
  const uint64_t generation = server->connection_generation(client);
  if (generation == 0) {
    // 连接已经不在服务器上了 —— 写它没有意义，而且指针可能已被复用。
    resp = uvcpp_http_response::not_found();
    return;
  }

  std::string fs_path = local_dir_ + url_path;

  // SPA 回退目标：根索引文件。请求本身就是索引文件时不必回退（否则会出现
  // "index.html 不存在 → 回退到 index.html" 这种自我循环）。
  std::string spa_fs_path;
  std::string spa_url_path;
  const std::string root_index = "/" + index_file_;
  if (spa_fallback_ && url_path != root_index) {
    spa_url_path = root_index;
    spa_fs_path = local_dir_ + root_index;
  }

  // 元数据也要异步取：sync stat 在事件循环线程上是阻塞 IO，
  // 本地盘上微秒级、网络盘上是毫秒级 —— 属于"通常很快但不是保证"的那类。
  resp.deferred = true;
  stat_async(fs_path, url_path, spa_fs_path, spa_url_path, 0, generation, client,
             server);
}

std::string uvcpp_static_server::mime_of(const std::string& path) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return "application/octet-stream";

  std::string ext = path.substr(dot + 1);
  for (size_t i = 0; i < ext.size(); ++i) {
    char c = ext[i];
    if (c >= 'A' && c <= 'Z') ext[i] = static_cast<char>(c - 'A' + 'a');
  }

  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "js" || ext == "mjs") return "application/javascript; charset=utf-8";
  if (ext == "css")  return "text/css; charset=utf-8";
  if (ext == "json") return "application/json; charset=utf-8";
  if (ext == "map")  return "application/json";
  if (ext == "svg")  return "image/svg+xml";
  if (ext == "png")  return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif")  return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "ico")  return "image/x-icon";
  if (ext == "woff2") return "font/woff2";
  if (ext == "woff") return "font/woff";
  if (ext == "ttf")  return "font/ttf";
  if (ext == "otf")  return "font/otf";
  if (ext == "eot")  return "application/vnd.ms-fontobject";
  if (ext == "txt")  return "text/plain; charset=utf-8";
  if (ext == "xml")  return "application/xml";
  if (ext == "mp4")  return "video/mp4";
  if (ext == "webm") return "video/webm";
  if (ext == "mp3")  return "audio/mpeg";
  if (ext == "wav")  return "audio/wav";
  return "application/octet-stream";
}

}  // namespace uvcpp
