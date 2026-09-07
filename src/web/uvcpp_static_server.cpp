/**
 * @file src/web/uvcpp_static_server.cpp
 * @brief 静态文件服务实现（懒加载 + mtime 校验缓存 + 线程池异步读盘）。
 */

#include <web/uvcpp_static_server.h>

#include <fstream>
#include <sstream>

#include <req/uvcpp_fs.h>
#include <req/uvcpp_work.h>

namespace uvcpp {

// 异步加载任务上下文（worker 线程与事件循环线程之间传递）
struct uvcpp_static_server::LoadTask {
  uvcpp_static_server* self;    // 事件循环线程回调用（更新缓存）
  uvcpp_http_server* server;    // deferred 响应发送
  uvcpp_tcp_client* client;     // 客户端连接
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

uvcpp_static_server::~uvcpp_static_server() {}

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

  // 空或根 → 索引文件
  if (url.empty() || url == "/") {
    url_path = "/" + index_file_;
    return true;
  }

  // URL 前缀匹配（url_root 非 "/" 时）
  if (url_root_.size() > 1) {
    if (url.compare(0, url_root_.size(), url_root_) != 0) return false;
    url = url.substr(url_root_.size());
    if (url.empty()) url = "/";
    if (url[0] != '/') url = "/" + url;
  }

  // 防目录穿越
  if (url.find("..") != std::string::npos) return false;

  url_path = url;
  return true;
}

bool uvcpp_static_server::stat_file(uvcpp_loop* loop, const std::string& fs_path,
                                    int64_t& msec, int64_t& mnsec,
                                    int64_t& size) const {
  uvcpp_fs fs;
  if (fs.stat(loop, fs_path.c_str()) != 0) {
    fs.req_cleanup();
    return false;
  }
  uv_stat_t* st = fs.get_statbuf();
  if (st == nullptr) {
    fs.req_cleanup();
    return false;
  }
  msec = static_cast<int64_t>(st->st_mtim.tv_sec);
  mnsec = static_cast<int64_t>(st->st_mtim.tv_nsec);
  size = static_cast<int64_t>(st->st_size);
  fs.req_cleanup();
  return true;
}

namespace {
// worker 线程专用：二进制读文件（阻塞磁盘 IO 放在线程池，不卡事件循环）
bool read_file_binary(const std::string& path, std::string& out) {
  std::ifstream f(path.c_str(), std::ios::binary);
  if (!f.is_open()) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
}  // namespace

void uvcpp_static_server::load_async(const std::string& fs_path,
                                     const std::string& cache_key, int64_t msec,
                                     int64_t mnsec, int64_t size,
                                     uvcpp_tcp_client* client,
                                     uvcpp_http_server* server) {
  LoadTask* task = new LoadTask();
  task->self = this;
  task->server = server;
  task->client = client;
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
        uvcpp_http_response resp;
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
        delete work;
      });

  // 极端情况：线程池排队失败，直接兜底 404 并回收
  if (rc != 0) {
    uvcpp_http_response resp = uvcpp_http_response::not_found();
    server->send_response(client, resp);
    delete task;
    delete work;
  }
}

void uvcpp_static_server::serve(uvcpp_http_request& req, uvcpp_http_response& resp,
                                uvcpp_tcp_client* client,
                                uvcpp_http_server* server) {
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

  std::string fs_path = local_dir_ + url_path;
  std::string cache_key = url_path;

  uvcpp_loop* loop = client->get_loop();

  int64_t msec = 0, mnsec = 0, size = 0;
  bool exists = stat_file(loop, fs_path, msec, mnsec, size);

  // 目标文件不存在 → SPA 回退到索引文件
  if (!exists) {
    std::string index_path = "/" + index_file_;
    if (spa_fallback_ && url_path != index_path) {
      url_path = index_path;
      fs_path = local_dir_ + url_path;
      cache_key = url_path;
      exists = stat_file(loop, fs_path, msec, mnsec, size);
    }
    if (!exists) {
      resp = uvcpp_http_response::not_found();
      return;
    }
  }

  // 缓存命中且未变化 → 内存直出（零磁盘 IO）
  if (cache_enabled_) {
    std::map<std::string, CacheEntry>::iterator it = cache_.find(cache_key);
    if (it != cache_.end() && it->second.mtime_sec == msec &&
        it->second.mtime_nsec == mnsec && it->second.size == size) {
      const CacheEntry& e = it->second;
      resp = uvcpp_http_response::ok(e.data->data(), e.data->size(), e.mime);
      return;
    }
  }

  // 未命中或文件已变更 → 异步读盘（线程池），deferred 响应
  resp.deferred = true;
  load_async(fs_path, cache_key, msec, mnsec, size, client, server);
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
