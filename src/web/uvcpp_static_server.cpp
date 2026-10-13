/**
 * @file src/web/uvcpp_static_server.cpp
 * @brief 静态文件服务实现（懒加载 + mtime 校验缓存 + 线程池异步读盘）。
 */

#include <web/uvcpp_static_server.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

// `http_pick_precompressed_variants()` —— 孪生后缀的挑选规则。与技术层的
// `uvcpp_web_static` 共用同一份实现（理由见那个函数上面那段）。
#include <web/uvcpp_http_common.h>

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

/**
 * @brief 给一份取自孪生文件的响应补上 `content-encoding` 与 `vary`。
 *
 * `content-type` **不在这里改**：它取自原文件的路径（`app.js` → `text/javascript`），
 * 而 `app.js.br` 的扩展名是 `.br`，按它推类型会得到 `application/octet-stream`。
 * 调用方传进来的 `mime` 已经是原文件的那一份。
 *
 * `vary` 是必须的：同一条 URL 会因为 `Accept-Encoding` 给出不同的字节，中间缓存
 * 不看到这一行就会把 brotli 的响应发给一个只说 gzip 的客户端（RFC 9110 §12.5.5）。
 * 已经在头表里的不覆盖 —— 与实时压缩那条路（`apply_compression_for`）同一条规矩。
 *
 * 顺带一提：这一对头**同时**把 `uvcpp_http_server` 的实时压缩挡在门外（那边
 * `if (resp.has_header("content-encoding")) return false;`），所以孪生文件不会
 * 被再压一遍。
 */
void apply_variant_headers(uvcpp_http_response& resp,
                           const std::string& variant) {
  if (variant.empty()) return;
  resp.set_header("content-encoding", variant == ".br" ? "br" : "gzip");
  if (!resp.has_header("vary")) resp.set_header("vary", "accept-encoding");
}

}  // namespace

// 异步 stat 任务：worker 线程取元数据，loop 线程决定下一步走向。
struct uvcpp_static_server::StatTask {
  uvcpp_static_server* self;
  uvcpp_http_server* server;
  // 「稍后回话」凭据 —— 代替从前的 `(client, generation)` 两格。它把"回给哪条
  // 连接、哪条流"与"还是不是当初那条"两件事收在一处，而且是**三条传输上同一
  // 个形状**：h3 上根本没有 `uvcpp_tcp_client*` 可留（那条腿上处理函数拿到的是
  // nullptr），从前那条静态路由因此在 h3 上走不通。
  uvcpp_http_reply reply;
  uvcpp_loop* loop;         // `reply.loop()`，在 loop 线程上取好；worker 里别碰 reply

  std::string fs_path;      // 当前要 stat 的本地路径
  std::string url_path;     // 对应的 URL 路径（缓存键）
  std::string spa_fs_path;  // SPA 回退目标（空 = 不回退）
  std::string spa_url_path;

  int step;                 // 0=原始目标 1=目录拼 index 2=SPA 回退
  stat_kind kind;           // worker 产出：FILE / DIR / MISSING
  int64_t mtime_sec;
  int64_t mtime_nsec;
  int64_t size;

  // --- 预压缩孪生（`set_precompressed(true)` 时才有内容） ---
  //
  // `variants` 在 **loop 线程**上按请求头算好（worker 不该碰 `req`），worker 只
  // 负责按序 probe。`variant` 是 worker 的产出：选中的那个后缀，空串 = 发原文件。
  // 选中之后 `fs_path`/`mtime_*`/`size` **全部换成孪生那一份** —— 缓存校验比的是
  // "要发的那个文件变了没有"，拿原文件的 mtime 去校验孪生的字节，改的只是孪生
  // 时会一直发旧字节。
  std::string variants[2];
  size_t variant_count;
  std::string variant;

  // --- 同步档（`uvcpp_static_revalidate::LOOP_STAT`）专用的两格 ---
  //
  // 同步档下这张 task 在**栈**上（循环线程就地走完），响应要**交回调用方的
  // `resp`**，而不是走 `send_reply()`：那条路是给 deferred 用的，这里
  // `resp.deferred` 还是 false，再自己发一次就成了两条响应。所以：
  //   * `sync_out` 非空 = 这是同步档的那张 task；
  //   * `sync_sent` = 这次请求**已经回完了**（命中与 404 都置它）；
  //   * `sync_out` 非空而 `sync_sent` 是假 = 走进了 `load_async()`（缓存未命中，
  //     要读盘）—— 那一支照旧用 deferred 通道自己回话，调用方据此把
  //     `resp.deferred` 立起来。
  uvcpp_http_response* sync_out;
  bool sync_sent;

  // --- 响应**落点** ---
  //
  // 命中与 404 这两条终点都要"造一份响应、再交出去"。造在哪儿有两种可能：
  //   * 同步档：调用方已经给了对象（`*sync_out`），**就往它里面填** —— 中间
  //     再摆一个临时的，等于把整份文件多拷一遍（`uvcpp_buf` 的拷贝是深拷贝，
  //     见下）；
  //   * 池档：没有调用方的对象可填，落在本字段上，`send_reply()` 直接读它。
  //
  // 于是两条路都是"就地填 + 交引用"，谁都不用拷贝。**它按值随 `StatTask` 走**
  // （`stat_dispatch()` 递归时那份栈上拷贝会连它一起拷），空对象是一次默认
  // 构造，没有分配。
  uvcpp_http_response scratch;
};

// 异步加载任务上下文（worker 线程与事件循环线程之间传递）
struct uvcpp_static_server::LoadTask {
  uvcpp_static_server* self;    // 事件循环线程回调用（更新缓存）
  uvcpp_http_server* server;    // deferred 响应发送
  uvcpp_http_reply reply;       // 回给哪条连接哪条流（见 StatTask 里那段说明）
  std::string fs_path;          // 本地文件路径（孪生命中时是孪生那一份）
  std::string cache_key;        // 缓存键（URL 路径 + 孪生后缀）
  std::string mime_key;         // 取 content-type 用的路径：**永远是原文件**
  std::string variant;          // 选中的孪生后缀（""=原文件），决定 content-encoding
  int64_t mtime_sec;            // stat 到的 mtime（孪生命中时是孪生的）
  int64_t mtime_nsec;
  int64_t size;
  bool ok;                      // worker 线程读盘结果
  std::shared_ptr<std::string> data;  // worker 线程填充的文件字节
};

namespace {

/**
 * @brief 把 \p v 的十进制接到 \p out 的尾部。
 *
 * 与 `webapp/uvcpp_web_static.cpp` 里那一份**逐字相同**，是**故意**各留一份：
 * 两个模块互不依赖（连 `uvcpp_static_revalidate` 都各自声明一份，见头文件里
 * 那段说明），而这个函数只服务于 `cache_tag` 的拼装。
 *
 * 不用 `out += std::to_string(v)`：那一句先造一个临时串（短数走 SSO，不落堆，
 * 但仍然要多走一次移动赋值），而 `snprintf` 到一个栈缓冲区再 `append` 就是
 * 一次。位数上界：`-9223372036854775808` 是 20 个字符，24 字节够。
 */
void append_i64_dec(std::string& out, int64_t v) {
  char buf[24];
  const int n =
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
    out.append(buf, static_cast<size_t>(n));
  }
}

/**
 * @brief 把缓存里那份字节**就地**填成一份 200，body 走共享视图（不拷贝字节）。
 *
 * 与 `uvcpp_http_response::ok(data, len, mime)` 逐字段等价，只差 body 从
 * "拷一份"换成"借一份"。长度直接用 `data->size()`：与 `ok()` 一样取**实际读到
 * 的字节数**，不是 stat 报的那个（两者不一致时，短的这份才是真的）。
 *
 * 借出去之后这份内容由**响应**与缓存条目共同保活（`shared_ptr`）：缓存条目
 * 紧接着被淘汰、甚至 `clear_cache()` 清空，也不会让响应手里的指针悬空。
 *
 * \p fs_path / \p size / \p msec / \p mnsec 一起构成 `resp.cache_tag` ——
 * `uvcpp_http_server` 那个**压缩变体缓存**的键（契约在
 * `uvcpp_http_response::cache_tag`）。四个字段与 LRU 校验原始条目用的那一组
 * （`cache_key` 的比对三元组 + 路径）**是同一组**，所以"文件变了"必然导致键变，
 * 不需要任何额外的失效逻辑。
 *
 * **用了孪生文件就不设 tag**：tag 是给**实时压缩**的变体缓存当键的，而这份
 * body 已经是压缩产物 —— `apply_compression_for()` 见到 `content-encoding`
 * 根本不会进来，设了只会白占一份变体缓存的位置。这一条与
 * `uvcpp_web_static.cpp` 里那一处**逐字同策**。
 */
void fill_cached_response(uvcpp_http_response& resp,
                          const std::shared_ptr<std::string>& data,
                          const std::string& mime, const std::string& variant,
                          const std::string& fs_path, int64_t size,
                          int64_t msec, int64_t mnsec) {
  resp.status_code = http_status::OK;
  resp.status_message = "OK";
  if (!mime.empty()) resp.set_content_type(mime);
  resp.body.share(data);
  apply_variant_headers(resp, variant);

  if (variant.empty()) {
    // 就地拼、不造中间串（`a + b + c + d` 那版每加一次都产出一个临时串）。
    // 三个数用不着 `std::to_string`：`int64_t` 的字面量转换对这几个长度本来
    // 就走 SSO，白白多拷一遍。
    std::string& tag = resp.cache_tag;
    tag.clear();
    tag.reserve(fs_path.size() + 48);
    tag.append(fs_path);
    tag += '\x1f';
    append_i64_dec(tag, size);
    tag += '\x1f';
    append_i64_dec(tag, msec);
    tag += '\x1f';
    append_i64_dec(tag, mnsec);
  }
}

/**
 * @brief 就地填成 404。字段与 `uvcpp_http_response::not_found()` 逐字一致。
 *
 * 就地填而不是"造一份再赋值"：赋值会把框架填好的身份字段（`stream_id`、
 * `deferred`、`retryable`）一起冲掉，而它们归框架管 —— h2 上 `stream_id` 决定
 * 这份响应归到哪条流，`send_reply()` 虽然会用凭据里那个覆盖它，但"靠下游兜住"
 * 不该是就地填的代价。
 */
void fill_not_found(uvcpp_http_response& resp) {
  resp.status_code = http_status::NOT_FOUND;
  resp.status_message = "Not Found";
  resp.set_content_type("text/plain");
  resp.body.clear();
}

}  // namespace

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
  drain_all_retired();
}

void uvcpp_static_server::retire(uvcpp_loop* loop, uvcpp_work* w) {
  std::lock_guard<std::mutex> lk(mu_);
  retired_[loop].push_back(w);
}

void uvcpp_static_server::drain_retired(uvcpp_loop* loop) {
  // 取不到循环（用例手工造的请求对象又没给连接）⇒ **一个桶都不排空**。
  // 宁可不删：删错桶是 UAF，不删只是这一批多活一会儿，下次总有一条真循环
  // 的 `serve()` 或析构来收。
  if (loop == nullptr) return;
  std::vector<uvcpp_work*> mine;
  {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<uvcpp_loop*, std::vector<uvcpp_work*> >::iterator it =
        retired_.find(loop);
    if (it == retired_.end()) return;
    mine.swap(it->second);
    retired_.erase(it);
  }
  // 锁外删：`~uvcpp_work` 里会发生什么不该由这把锁兜着。
  for (size_t i = 0; i < mine.size(); ++i) delete mine[i];
}

void uvcpp_static_server::drain_all_retired() {
  std::map<uvcpp_loop*, std::vector<uvcpp_work*> > all;
  {
    std::lock_guard<std::mutex> lk(mu_);
    all.swap(retired_);
  }
  for (std::map<uvcpp_loop*, std::vector<uvcpp_work*> >::iterator it =
           all.begin();
       it != all.end(); ++it) {
    for (size_t i = 0; i < it->second.size(); ++i) delete it->second[i];
  }
}

// ---- 缓存记账（三处，全在 `mu_` 里）----------------------------------------
//
// 这三处的形状与 `uvcpp_web_static::Impl` 的 `put()` / `evict_locked()` /
// `clear_cache()` 是同一套（同一个"有界的东西该拿什么当尺子"的取舍），差别只在
// 这里没有解析记忆那一份表。

void uvcpp_static_server::put_cache_locked(
    const std::string& key, const std::shared_ptr<std::string>& data,
    const std::string& mime, const std::string& variant,
    const std::string& fs_path, int64_t msec, int64_t mnsec, int64_t size) {
  if (!data) return;
  // 单份文件就超过总预算 ⇒ 干脆不缓存。否则它一进来就会把整个缓存冲空，然后
  // 下一次仍然装不下，退化成"每次请求都把缓存冲一遍"。
  // （`cache_max_bytes_ == 0` 是"不限字节"，不是"预算为零"。）
  if (cache_max_bytes_ > 0 && data->size() > cache_max_bytes_) return;

  std::map<std::string, CacheEntry>::iterator old = cache_.find(key);
  if (old != cache_.end()) {
    cache_bytes_ -= old->second.data->size();
    lru_.erase(old->second.pos);
    cache_.erase(old);
  }

  lru_.push_front(key);
  CacheEntry e;
  e.data = data;
  e.mime = mime;
  e.variant = variant;
  e.mtime_sec = msec;
  e.mtime_nsec = mnsec;
  e.size = size;
  e.fs_path = fs_path;
  e.pos = lru_.begin();
  cache_[key] = e;
  cache_bytes_ += data->size();

  evict_locked();
}

void uvcpp_static_server::evict_locked() {
  while (!lru_.empty() &&
         (cache_.size() > cache_max_entries_ ||
          (cache_max_bytes_ > 0 && cache_bytes_ > cache_max_bytes_))) {
    const std::string& victim = lru_.back();
    std::map<std::string, CacheEntry>::iterator it = cache_.find(victim);
    if (it != cache_.end()) {
      cache_bytes_ -= it->second.data->size();
      cache_.erase(it);
    }
    lru_.pop_back();
  }
}

size_t uvcpp_static_server::cache_size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return cache_.size();
}

size_t uvcpp_static_server::cache_bytes() const {
  std::lock_guard<std::mutex> lk(mu_);
  return cache_bytes_;
}

void uvcpp_static_server::clear_cache() {
  std::lock_guard<std::mutex> lk(mu_);
  lru_.clear();
  cache_.clear();
  cache_bytes_ = 0;
}

uvcpp_loop* uvcpp_static_server::loop_of(uvcpp_tcp_client* client,
                                         const uvcpp_http_reply& reply) {
  if (client != nullptr) return client->get_loop();
  if (reply.valid()) return reply.loop();
  return nullptr;
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

void uvcpp_static_server::run_stat_inline(StatTask* task) const {
  task->kind = stat_path(task->loop, task->fs_path, task->mtime_sec,
                         task->mtime_nsec, task->size);
  // 孪生探查与上面这一次必须在**同一趟**里做完 —— 分成两趟的话，两次 stat
  // 之间文件可能被换掉，选出来的孪生与刚校验过的 mtime 对不上。
  if (task->kind != stat_kind::FILE || task->variant_count == 0) return;
  for (size_t i = 0; i < task->variant_count; ++i) {
    const std::string cand = task->fs_path + task->variants[i];
    int64_t s = 0, ns = 0, sz = 0;
    if (stat_path(task->loop, cand, s, ns, sz) != stat_kind::FILE) {
      continue;  // 这一种盘上没有，试下一种（有序表的意义就在这）
    }
    task->variant = task->variants[i];
    task->fs_path = cand;
    task->mtime_sec = s;
    task->mtime_nsec = ns;
    task->size = sz;
    return;
  }
}

void uvcpp_static_server::stat_dispatch(StatTask* task, const std::string& fs_path,
                                        const std::string& url_path,
                                        const std::string& spa_fs_path,
                                        const std::string& spa_url_path, int step) {
  if (revalidate_ == uvcpp_static_revalidate::LOOP_STAT) {
    // 同步档：目录 index（step 1）与 SPA 回退（step 2）也在这条线程上一路走完
    // ——`stat_step()` 的递归最深三层，全在栈上，没有池、没有堆。换路径要拷一份
    // task（候选表与凭据原样带下去），这也是同步档唯一的额外代价。
    StatTask local = *task;
    local.fs_path = fs_path;
    local.url_path = url_path;
    local.spa_fs_path = spa_fs_path;
    local.spa_url_path = spa_url_path;
    local.step = step;
    local.variant.clear();  // 换文件了，上一步选中的孪生后缀不再适用
    run_stat_inline(&local);
    stat_step(&local);
    // `local` 是栈上那份，它的 `sync_sent` 得**传回去**：`serve()` 看的是最初
    // 那张 task 上的这一格，决定还要不要设 `resp.deferred`。`sync_out` 是同一个
    // 指针（拷贝过来的），所以"发过没有"这一格是唯一需要搬的。
    task->sync_sent = local.sync_sent;
    return;
  }
  stat_async(fs_path, url_path, spa_fs_path, spa_url_path, step, task->reply,
             task->server, task->variants, task->variant_count);
}

void uvcpp_static_server::stat_async(std::string fs_path, std::string url_path,
                                     std::string spa_fs_path,
                                     std::string spa_url_path, int step,
                                     const uvcpp_http_reply& reply,
                                     uvcpp_http_server* server,
                                     const std::string* variants,
                                     size_t variant_count) {
  StatTask* task = new StatTask();
  task->sync_out = nullptr;
  task->sync_sent = false;
  task->self = this;
  task->server = server;
  task->reply = reply;
  // **在 loop 线程上把 loop 指针记下来**。worker 里绝不能再通过 reply 去取
  // 它 —— 那时连接可能已经被回收，而 loop 的生命周期覆盖整个服务器。
  task->loop = reply.loop();
  task->fs_path = fs_path;
  task->url_path = url_path;
  task->spa_fs_path = spa_fs_path;
  task->spa_url_path = spa_url_path;
  task->step = step;
  task->kind = stat_kind::MISSING;
  task->mtime_sec = 0;
  task->mtime_nsec = 0;
  task->size = 0;
  task->variant_count = variant_count > 2 ? 2 : variant_count;
  for (size_t i = 0; i < task->variant_count; ++i) {
    task->variants[i] = variants[i];
  }

  uvcpp_work* work = new uvcpp_work();
  int rc = work->queue_work(
      task->loop,
      // worker 线程：只做元数据，不碰 reply / server。函数体与同步档**共用**
      // 同一份（`run_stat_inline()`）—— 两条路走的必须是同一套 stat，否则
      // "选了哪个孪生、比的是谁的 mtime" 会在两档之间悄悄分家。
      [task](uvcpp_work*) { task->self->run_stat_inline(task); },
      [task, work](uvcpp_work*, int /*status*/) {
        uvcpp_static_server* self = task->self;
        self->stat_step(task);
        // `work` 交给**这条循环**下次进 `serve()` 时回收 —— 详见 `retire()`
        // 的注释。**这里之后绝不能再碰 `work`**；`task` 是另一个堆对象，
        // `work` 还没被释放，所以读捕获变量 `task` 仍然是安全的。
        self->retire(task->loop, work);
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
  // 写下去就是把 A 的响应发给 B —— 凭据里的代次号就是为这一句存在的。
  if (!task->server->reply_alive(task->reply)) {
    return;
  }

  if (task->kind == stat_kind::FILE) {
    // 缓存键带孪生后缀：同一条 URL 在 `Accept-Encoding` 不同时取的是**两份不同
    // 的字节**（`app.js` 与 `app.js.br`），合成一个键就会把 brotli 的字节发给
    // 一个只说 gzip 的客户端。后缀是 `.br`/`.gz`，都是 `url_path` 里不可能出现
    // 的结尾（合法的静态文件名里带点也会带扩展名，而键的比对是整串相等）。
    const std::string cache_key = task->url_path + task->variant;
    // 缓存命中且文件没变 → 内存直出（零磁盘 IO）
    if (cache_enabled_) {
      // 查、取、**提升 LRU** 都在锁内，**用**在锁外：命中时把条目**拷一份**
      // 出来（`CacheEntry` 里那块数据是 `shared_ptr`，拷贝是几次指针/整数
      // 赋值），这样 `send_reply()` —— 它会一路走到写 socket —— 不在这把锁里跑。
      //
      // 提升用 `splice`：O(1)、不搬元素、别的迭代器一个都不失效（所以队列里
      // 其它条目的 `pos` 仍然有效）。这一步把"命中算一次使用"钉住 —— 不提升的
      // 话，被反复请求的热文件会被一次性的冷文件按插入次序挤出去。
      CacheEntry hit;
      bool found = false;
      {
        std::lock_guard<std::mutex> lk(mu_);
        std::map<std::string, CacheEntry>::iterator it =
            cache_.find(cache_key);
        if (it != cache_.end() && it->second.mtime_sec == task->mtime_sec &&
            it->second.mtime_nsec == task->mtime_nsec &&
            it->second.size == task->size) {
          lru_.splice(lru_.begin(), lru_, it->second.pos);
          hit = it->second;
          found = true;
        }
      }
      if (found) {
        // 就地填进落点：同步档是调用方那个对象，池档是本 task 自带的 —— 两条
        // 路都不经过临时的 `uvcpp_http_response`（一次深拷贝的 body），body 也
        // 是借缓存那份字节（`shared_ptr`）而不是拷一份。
        uvcpp_http_response& out =
            task->sync_out != nullptr ? *task->sync_out : task->scratch;
        fill_cached_response(out, hit.data, hit.mime, hit.variant, hit.fs_path,
                             hit.size, hit.mtime_sec, hit.mtime_nsec);
        deliver(task, out);
        return;
      }
    }
    // 未命中或文件已变更 → 异步读盘（线程池），deferred 响应
    load_async(task->fs_path, cache_key, task->url_path, task->variant,
               task->mtime_sec, task->mtime_nsec, task->size, task->reply,
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

    stat_dispatch(task, dir, url, task->spa_fs_path, task->spa_url_path, 1);
    return;
  }

  // 还没试过 SPA 回退，且回退目标和刚 stat 的不是同一个 → 试一次。
  if (task->step < 2 && !task->spa_fs_path.empty() &&
      task->spa_fs_path != task->fs_path) {
    stat_dispatch(task, task->spa_fs_path, task->spa_url_path, std::string(),
                  std::string(), 2);
    return;
  }

  uvcpp_http_response& out =
      task->sync_out != nullptr ? *task->sync_out : task->scratch;
  fill_not_found(out);
  deliver(task, out);
}

void uvcpp_static_server::deliver(StatTask* task, uvcpp_http_response& resp) {
  if (task->sync_out == &resp) {
    // 同步档：`resp` **就是**调用方那个对象（落点由上面那两句选的），响应已经
    // 就地填好了，这里只记账 —— `serve()` 还等着往下走，别自己发。
    // 这里不设 `deferred` —— 那个标志归 `serve()` 管（见那里的注释）。
    task->sync_sent = true;
    return;
  }
  task->server->send_reply(task->reply, resp);
}

void uvcpp_static_server::load_async(const std::string& fs_path,
                                     const std::string& cache_key,
                                     const std::string& mime_key,
                                     const std::string& variant, int64_t msec,
                                     int64_t mnsec, int64_t size,
                                     const uvcpp_http_reply& reply,
                                     uvcpp_http_server* server) {
  LoadTask* task = new LoadTask();
  task->self = this;
  task->server = server;
  task->reply = reply;
  task->fs_path = fs_path;
  task->cache_key = cache_key;
  task->mime_key = mime_key;
  task->variant = variant;
  task->mtime_sec = msec;
  task->mtime_nsec = mnsec;
  task->size = size;
  task->ok = false;

  uvcpp_work* work = new uvcpp_work();
  int rc = work->queue_work(
      reply.loop(),
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
        // 桶键取**投递它的那条循环**（`queue_work()` 的第一实参就是它）。
        self->retire(task->reply.loop(), work);

        uvcpp_http_response resp;
        // 连接换人了就别写 —— 凭据里的代次对不上就不发（见 `reply_alive()`）。
        if (!task->server->reply_alive(task->reply)) {
          delete task;
          return;
        }
        if (task->ok) {
          const std::string mime = uvcpp_static_server::mime_of(task->mime_key);
          // **`cache_enabled_` 在这里也要判一次。** 从前只在命中那一支判，
          // 于是 `set_cache_enabled(false)` 关掉的是"读"、关不掉"写"：表照样
          // 只增不减（`cache_size()` 也会跟着涨）—— 那是个货真价实的漏洞，
          // 不是省一次判断。`1.6.44-dev` 起与命中那一支用同一个开关。
          if (self->cache_enabled_) {
            // 插入会动红黑树结构、还会动 LRU 队列，而别的循环线程可能正在这
            // 两处上查/提升 —— 见头文件里 `cache_` 的说明。`mime_of()` 是文件
            // 系统之外的一次纯计算，留在锁外。
            std::lock_guard<std::mutex> lk(self->mu_);
            self->put_cache_locked(task->cache_key, task->data, mime,
                                   task->variant, task->fs_path, task->mtime_sec,
                                   task->mtime_nsec, task->size);
          }
          // 就地填，body 直接借刚读进来的那份（存进缓存的是同一个
          // `shared_ptr`）—— `ok()` 那一支会把整份文件再拷一遍。
          fill_cached_response(resp, task->data, mime, task->variant,
                               task->fs_path, task->size, task->mtime_sec,
                               task->mtime_nsec);
        } else {
          // 读盘失败：几字节的常量 body，走 `not_found()` 就好（它带的那几个
          // 字段正是这里要的）。
          resp = uvcpp_http_response::not_found();
        }
        task->server->send_reply(task->reply, resp);
        delete task;
      });

  // 极端情况：线程池排队失败，直接兜底 404 并回收
  if (rc != 0) {
    if (server->reply_alive(reply)) {
      uvcpp_http_response resp = uvcpp_http_response::not_found();
      server->send_reply(reply, resp);
    }
    delete task;
    delete work;
  }
}

void uvcpp_static_server::serve(uvcpp_http_request& req, uvcpp_http_response& resp,
                                uvcpp_tcp_client* client,
                                uvcpp_http_server* server) {
  // 先把**本条循环**上一批用完的 `uvcpp_work` 真正删掉。这里是**安全的删除点**：
  // `serve()` 由请求驱动，必然跑在 loop 线程上，而且此时不在任何 libuv 完成回调里
  // —— 与在 after_work 回调里 delete 完全是两回事（详见 `retire()`）。
  //
  // 只看自己这一桶：别的循环攒下的`work`，其 after_work 回调**排在别的循环上**，
  // 可能还没跑完；在这里顺手删掉就是让那条循环在一个已释放的对象上继续执行回调。
  drain_retired(loop_of(client, req.reply));

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

  // 「稍后回话」凭据：异步跑完回来时**全靠它**（它同时回答了"回给哪条连接哪条
  // 流"与"还是不是当初那条"）。
  //
  // 优先取框架填好的那枚 `req.reply` —— **三条传输上都有**，于是本类不必知道
  // 自己在 h1 / h2 / h3 哪条腿上（从前它只认 `client`，而 h3 那条腿上处理函数
  // 拿到的是 nullptr，静态路由因此在 h3 上直接不可用）。取不到才退回按 `client`
  // 现造一枚：那是**手工构造的请求对象**走的老路（用例里这么干），只存在于 h1/h2。
  uvcpp_http_reply reply = req.reply;
  if (!reply.valid() && client != nullptr) {
    reply = server->reply_of(client, resp.stream_id);
  }
  // 造不出有效凭据（连接已经不在服务器上了）⇒ 写它没有意义，而且指针可能已被
  // 复用。回一个 404 而不是推迟 —— 推迟出去也没有入口能把它送出去。
  if (!server->reply_alive(reply)) {
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

  // 预压缩孪生的候选表，在**这条 loop 线程**上按请求头算好：worker 里没有
  // `req`，也不该有 —— 它只该做磁盘 IO。两道关卡：
  //
  //   * 本类没开 `precompressed`（默认）⇒ 一个候选都不给；
  //   * 请求带 `Range` ⇒ 同样不给。范围说的是**原文件**的字节坐标，换一份表示
  //     之后那个坐标就不成立了（与 `uvcpp_web_static` 同策，那边也有一句
  //     `!req.has_header("range")`）。本类今天不做 Range，这一条是为了将来
  //     加它的时候不会漏。
  //
  // 候选表按引用传下去的话，`std::string variants[2]` 得活到 worker 跑完 ——
  // 所以 `stat_async()` 按值拷进 `StatTask`，这里这个数组只活到函数返回。
  std::string variants[2];
  size_t variant_count = 0;
  if (precompressed_ && !req.has_header("range")) {
    variant_count =
        http_pick_precompressed_variants(req.get_header("accept-encoding"),
                                         variants);
  }

  if (revalidate_ == uvcpp_static_revalidate::LOOP_STAT) {
    // 就地 stat 那一档：命中与 404 都在这里回完，只有**未命中**（要读盘）才落到
    // `load_async()` 去、走 deferred。栈上这张 task 借 `sync_out` 把响应写回
    // `resp`；`variants` 那份候选表也照 `stat_async` 的做法按值拷进去 —— 递归到
    // 目录 index / SPA 回退时，`stat_dispatch()` 拷的就是这份拷贝。
    StatTask task;
    task.sync_out = &resp;
    task.sync_sent = false;
    task.self = this;
    task.server = server;
    task.reply = reply;
    task.loop = reply.loop();
    task.fs_path = fs_path;
    task.url_path = url_path;
    task.spa_fs_path = spa_fs_path;
    task.spa_url_path = spa_url_path;
    task.step = 0;
    task.kind = stat_kind::MISSING;
    task.mtime_sec = 0;
    task.mtime_nsec = 0;
    task.size = 0;
    task.variant_count = variant_count > 2 ? 2 : variant_count;
    for (size_t i = 0; i < task.variant_count; ++i) {
      task.variants[i] = variants[i];
    }

    run_stat_inline(&task);
    stat_step(&task);
    // 落到读盘那条路时（缓存未命中）它自己会异步回，这里不能抢先设 deferred：
    // `resp` 此刻还是空的，设了就等着读盘回调把它填上 —— 与池档逐字同策。
    if (!task.sync_sent) resp.deferred = true;
    return;
  }

  // 元数据也要异步取：sync stat 在事件循环线程上是阻塞 IO，
  // 本地盘上微秒级、网络盘上是毫秒级 —— 属于"通常很快但不是保证"的那类。
  resp.deferred = true;
  stat_async(fs_path, url_path, spa_fs_path, spa_url_path, 0, reply, server,
             variants, variant_count);
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
