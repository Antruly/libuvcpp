/**
 * @file src/webapp/uvcpp_web_static.cpp
 * @brief `uvcpp_web_static` 的实现。设计取舍见头文件。
 *
 * 线程模型（读代码前先看这段）
 * ----------------------------
 * 一次请求跨越两个线程，边界只有一处：`uvcpp_work` 的完成回调。
 *
 *   [loop 线程] 归一化 URL、dotfile 判定、组 job、投递
 *        ↓  （只传值：字符串、shared_ptr、快照）
 *   [worker]    拼路径、符号链接检查、stat、索引/SPA 回退、尺寸闸门、
 *               缓存校验、（必要时）读盘
 *        ↓
 *   [loop 线程] 条件请求/范围判定、填响应、更新 LRU
 *
 * 两条纪律：
 *   1. **worker 不碰 libuv 的循环状态**，只调用 `uv_fs_*` 的同步形态
 *      （回调传 NULL），它在调用线程就地执行，不注册 request、不进线程池。
 *      唯一的例外是那个 `uvcpp_loop*` 本身 —— 它只是同步 fs 调用的入参。
 *   2. **worker 不碰 `req`/`resp`**，只写 job 里的产出字段。
 *
 * 于是「连接在读盘期间断开」这件事不需要任何额外处理：响应最后是 loop 线程
 * 按**连接 id** 找活连接发的，找不到就丢弃（框架层保证），中间层没有裸指针。
 *
 * 多循环下要补一条：上面那个「loop 线程」不再只有一条。同一个 `uvcpp_web_static`
 * 对象被所有循环共用，于是**落在它身上的共享状态**（LRU/计数/待回收的
 * `uvcpp_work`）成了跨线程的。处理见 `Impl::mu_` 与 `Impl::retired` ——
 * 前者是把整份缓存锁起来（不分片），后者按循环分桶。
 */

#include "uvcpp_web_static.h"

#include <cstdio>
#include <mutex>

#include <fcntl.h>
#include <sys/stat.h>

#include <req/uvcpp_fs.h>
#include <req/uvcpp_work.h>
#include <web/uvcpp_http_common.h>
#include <webapp/uvcpp_log.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <webapp/uvcpp_web_util.h>

namespace uvcpp {

namespace {

// =========================================================================
// 小工具
// =========================================================================

/// 单次读盘的分片上限。Windows 上 `uv_buf_t::len` 是 ULONG，而且一次
/// `ReadFile` 读满 64 MiB 会把工作线程钉住很久 —— 分片才有机会让排在后面的
/// 任务插进来。1 MiB 是「系统调用次数」和「单次阻塞时长」的折中。
const size_t k_read_chunk = 1024u * 1024u;

std::string i64_to_hex(int64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llx", (unsigned long long)v);
  return std::string(buf);
}

/**
 * @brief 生成 ETag：`"<大小>-<mtime秒>-<mtime纳秒>"`。
 *
 * 和 nginx 一样只用 mtime + size 这两个字段，**不是内容哈希**。取舍的理由：
 * 哈希要读全文，等于把缓存省下的那份开销又原样花掉；而这个校验器的目的是
 * 「客户端手里那份还要不要重下」，mtime 的分辨率（Windows 100ns / Linux ns）
 * 在实践中够用。真需要内容级指纹的场合，应当由使用者在构建期把指纹编进
 * URL（`app.<hash>.js`），那也是更有效的缓存策略。
 */
std::string make_etag(int64_t size, int64_t mtime_sec, int64_t mtime_nsec) {
  return std::string("\"") + i64_to_hex(size) + "-" + i64_to_hex(mtime_sec) +
         "-" + i64_to_hex(mtime_nsec) + "\"";
}

/// 去掉 ETag 两端的引号（客户端漏引号时仍然要能比对得上）。
std::string strip_quotes(const std::string& s) {
  if (s.size() >= 2 && s[0] == '"' && s[s.size() - 1] == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

/**
 * @brief `If-None-Match` 的值是否命中我们的 ETag。
 *
 * 按 RFC 7232 §3.2：`*` 匹配任何当前表示；列表用逗号分隔；这个头做的是
 * **弱比较**，所以 `W/"x"` 与 `"x"` 视为同一个。
 */
bool etag_list_matches(const std::string& header_value,
                       const std::string& etag) {
  size_t pos = 0;
  while (pos <= header_value.size()) {
    size_t end = header_value.find(',', pos);
    if (end == std::string::npos) end = header_value.size();

    std::string item = web_trim(header_value.substr(pos, end - pos));
    if (!item.empty()) {
      if (item == "*") return true;
      if (web_starts_with_ci(item, "W/")) item = web_trim(item.substr(2));
      if (strip_quotes(item) == strip_quotes(etag)) return true;
    }

    if (end == header_value.size()) break;
    pos = end + 1;
  }
  return false;
}

/// 严格的十进制解析：非数字、超长（>18 位）一律失败。
///
/// 不用 `strtoll` 是因为它接受 `+`、前导空白、以及 `0x`，而 Range 里出现
/// 这些东西只可能是畸形输入 —— 与其依赖 `strtoll` 的宽容再把它们显式拒掉，
/// 不如从一开始就只认数字。
bool parse_i64(const std::string& s, int64_t& out) {
  if (s.empty() || s.size() > 18) return false;
  int64_t v = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  out = v;
  return true;
}

/// @note 中间那个值叫 `SKIP` 而不是 `IGNORE` —— 理由见
///       `uvcpp_web_dotfile_policy::HIDE` 的注释（`IGNORE` 是 Windows 的宏）。
enum class range_parse { OK, SKIP, UNSATISFIABLE };

/**
 * @brief 解析 `Range: bytes=...`，**只支持单个区间**。
 *
 * 多区间（`bytes=0-99,200-299`）要回 `multipart/byteranges`，本模块不做。
 * 按 RFC 7233 §3.1 服务端**可以**忽略 `Range`，所以多区间回 200 全量是合规
 * 的 —— 这比回一个拼错的 multipart 好得多（后者会让客户端拿到损坏的数据，
 * 而且它多半解析失败后不会重试）。
 *
 * `IGNORE` 与 `UNSATISFIABLE` 的区别是有意义的：前者回 200，后者回 416。
 * 语法正确但越界的范围（`bytes=9999-`）必须回 416，否则「拖动进度条到末尾」
 * 这类客户端会以为服务端不支持断点续传。
 */
range_parse parse_range(const std::string& value, int64_t size, int64_t& first,
                        int64_t& last) {
  if (!web_starts_with_ci(value, "bytes=")) return range_parse::SKIP;

  const std::string spec = web_trim(value.substr(6));
  if (spec.empty() || spec.find(',') != std::string::npos)
    return range_parse::SKIP;

  const size_t dash = spec.find('-');
  if (dash == std::string::npos) return range_parse::SKIP;

  const std::string a = web_trim(spec.substr(0, dash));
  const std::string b = web_trim(spec.substr(dash + 1));

  if (a.empty()) {
    // `-N`：最后 N 字节
    if (b.empty()) return range_parse::SKIP;
    int64_t n = 0;
    if (!parse_i64(b, n)) return range_parse::SKIP;
    if (n <= 0 || size <= 0) return range_parse::UNSATISFIABLE;
    if (n > size) n = size;
    first = size - n;
    last = size - 1;
    return range_parse::OK;
  }

  int64_t s = 0;
  if (!parse_i64(a, s)) return range_parse::SKIP;
  if (s >= size) return range_parse::UNSATISFIABLE;  // 起点越界

  if (b.empty()) {
    first = s;
    last = size - 1;
    return range_parse::OK;
  }

  int64_t e = 0;
  if (!parse_i64(b, e)) return range_parse::SKIP;
  if (e < s) return range_parse::UNSATISFIABLE;
  if (e >= size) e = size - 1;  // 右端超出就截到末尾，这是规范允许的
  first = s;
  last = e;
  return range_parse::OK;
}

/**
 * @brief 归一化路径里是否含以 `.` 开头的段。
 *
 * 只看**段首**：`lib..min.js` 不是 dotfile，`a/.b/c` 是。`.well-known` 也
 * 落在这一类里 —— 需要放行它的使用者请把 dotfiles 设为 ALLOW，或者把文档根
 * 挪到别处（不要指望框架猜哪些点文件是"好"的）。
 */
bool path_has_dotfile_segment(const std::string& p) {
  size_t i = 0;
  while (i < p.size()) {
    while (i < p.size() && p[i] == '/') ++i;
    if (i >= p.size()) break;
    if (p[i] == '.') return true;
    while (i < p.size() && p[i] != '/') ++i;
  }
  return false;
}

/// URL 路径拼接。存在的意义是**只有一份实现**：loop 线程拿它给缓存建候选键，
/// worker 拿它算索引文件的实际 URL —— 两边算出来的键必须逐字节相同，否则
/// 缓存永远命不中（而且不会报错，只是白读盘）。
std::string join_url(const std::string& base, const std::string& name) {
  if (base.empty() || base == "/") return "/" + name;
  return base + "/" + name;
}

// =========================================================================
// worker 侧的只读配置
// =========================================================================

/**
 * @brief 构造后不再变的那部分配置。
 *
 * 单独拎出来用 `shared_ptr<const>` 持有，是为了 job 能安全地跨线程带上它：
 * 即使 `uvcpp_web_static` 在工作途中被销毁，worker 读到的这份配置依然有效
 * （路径字符串和选项值都是自己的）。真正会失效的只有 `Impl*`，而它只在
 * loop 线程的完成回调里用。
 */
struct static_config {
  std::string root_real;              ///< 已归一化的文档根（空 = 配置错误）
  std::vector<std::string> index_names;  ///< 单段文件名，构造时已校验
  std::string spa_url;                ///< 已归一化的 SPA 入口 URL
  bool spa_fallback;
  size_t max_file_size;
  size_t max_cached_file_size;        ///< 超过它就改走分片读（见 options）
  bool follow_symlinks;
};

enum class probe_status {
  OK,
  NOT_FOUND,   ///< 不存在（含父目录不存在）
  FORBIDDEN,   ///< 存在但被策略挡下（穿越 / 符号链接）
  TOO_LARGE,   ///< 超过 max_file_size
  IO_ERROR,    ///< 打开/读取出错
  /**
   * 体积超过分流阈值 ⇒ **worker 一个字节都不读**，交给 loop 线程用
   * `resp.send_file_range()` 分片下发。
   *
   * 它是**最像 OK 的一个**：所有元数据（final_url / fs_path / size /
   * mtime）都齐了，ETag / Last-Modified / 条件请求 / Range 判定全部照常
   * 走，差别只在"产出 body 的那一步"换了实现。所以 `finish_job()` 里它跟
   * `OK` 走同一条大路，只在最后那个 if/else 链上分开。
   */
  STREAM,
  /**
   * 工作池名额没抢到 ⇒ 503。
   *
   * 与其余几种的差别在于：它是**唯一一种在 worker 里才判出来的状态**。
   * 名额过去是在 `serve()` 里（投递之前）取的，于是"命中 LRU 缓存、一次
   * 磁盘读都不需要"的请求也要先抢名额，抢不到就 503 —— 而那正是绝大多数
   * 请求。现在名额只在**确实要读盘**的两条支路上取（见 `run_job`），所以
   * 它的判定点跟着搬到了 worker 里。
   */
  REJECTED,
};

struct probe_result {
  probe_status status;
  bool policy_rejected;  ///< 被策略挡下 —— SPA 回退**不得**生效
  bool is_dir;
  std::string final_url;  ///< 实际命中的 URL（可能是索引文件或 SPA 入口）
  std::string fs_path;    ///< 真实路径
  int64_t mtime_sec;
  int64_t mtime_nsec;
  int64_t size;

  probe_result()
      : status(probe_status::NOT_FOUND),
        policy_rejected(false),
        is_dir(false),
        mtime_sec(0),
        mtime_nsec(0),
        size(0) {}
};

/// stat 到的是文件还是目录。任何错误（含"是设备文件/管道"）都返回 false，
/// 让调用方按"不存在"处理 —— 文档根里出现 FIFO 只可能是有人放进去的。
bool stat_real(uvcpp_loop* loop, const std::string& path, bool& is_dir,
               int64_t& sec, int64_t& nsec, int64_t& size) {
  uvcpp_fs fs;
  if (fs.stat(loop, path.c_str()) != 0) {
    fs.req_cleanup();
    return false;
  }

  bool ok = false;
  uv_stat_t* st = fs.get_statbuf();
  if (st != nullptr) {
    // statbuf 只在 req_cleanup() 之前有效，所以先把值抄出来。
    const uint64_t fmt = static_cast<uint64_t>(st->st_mode) & S_IFMT;
    if (fmt == S_IFDIR || fmt == S_IFREG) {
      is_dir = (fmt == S_IFDIR);
      sec = static_cast<int64_t>(st->st_mtim.tv_sec);
      nsec = static_cast<int64_t>(st->st_mtim.tv_nsec);
      size = static_cast<int64_t>(st->st_size);
      ok = true;
    }
  }
  fs.req_cleanup();
  return ok;
}

/// 最后一段是不是符号链接。用 `lstat`：它只对**最后一段**不跟随，中间段
/// 照样跟随 —— 而中间段指向根外的情况由 `web_resolve_within_root()` 兜住，
/// 两者合起来才是完整的。
bool lstat_is_symlink(uvcpp_loop* loop, const std::string& path) {
  uvcpp_fs fs;
  if (fs.lstat(loop, path.c_str()) != 0) {
    fs.req_cleanup();
    return false;
  }
  bool link = false;
  uv_stat_t* st = fs.get_statbuf();
  if (st != nullptr) {
    link = ((static_cast<uint64_t>(st->st_mode) & S_IFMT) == S_IFLNK);
  }
  fs.req_cleanup();
  return link;
}

/**
 * @brief 把 URL 解析成一个确定的文件（**不处理索引文件**，也不做 SPA 回退）。
 *
 * 「不处理索引」是刻意的：索引只对目录有意义，把它留在上层，递归深度就天然
 * 封顶在 1 —— 不用去证明「index.html 指向一个目录」这种情况不会转圈。
 */
probe_status probe_leaf(uvcpp_loop* loop, const static_config& cfg,
                        const std::string& url, probe_result& out) {
  out = probe_result();
  out.final_url = url;

  // (1) 符号链接策略。查的是**未解析**的路径，所以这里自己拼一次 ——
  //     `web_resolve_within_root()` 返回的是展开后的真实路径，那时候再判断
  //     「是不是链接」已经没有意义了。
  if (!cfg.follow_symlinks) {
    const std::string joined = web_join_root(cfg.root_real, url);
    if (lstat_is_symlink(loop, joined)) {
      out.status = probe_status::FORBIDDEN;
      out.policy_rejected = true;
      return out.status;
    }
  }

  // (2) 真实路径的包含判断 —— 这才是安全边界，第 (1) 步不是。
  std::string real;
  const web_path_status ws = web_resolve_within_root(cfg.root_real, url, real);
  if (ws != web_path_status::OK) {
    const bool traversal = (ws == web_path_status::TRAVERSAL);
    out.status = traversal ? probe_status::FORBIDDEN : probe_status::NOT_FOUND;
    out.policy_rejected = traversal;
    return out.status;
  }

  // (3) stat
  if (!stat_real(loop, real, out.is_dir, out.mtime_sec, out.mtime_nsec,
                 out.size)) {
    out.status = probe_status::NOT_FOUND;
    return out.status;
  }

  out.fs_path = real;
  out.status = probe_status::OK;
  return out.status;
}

/**
 * @brief `probe_leaf` + 目录索引。
 *
 * 目录里任何一个索引文件被**策略**拒绝（理论上只可能发生在 follow_symlinks
 * 关闭且索引是个链接时），整个解析就以拒绝收场 —— 而不是继续试下一个。
 * 理由是"拒绝"必须比"不存在"优先：否则关掉符号链接策略的人会看到自己的
 * index.html 链接被静默跳过、然后回落到 SPA，问题彻底看不见。
 */
probe_status probe_with_index(uvcpp_loop* loop, const static_config& cfg,
                              const std::string& url, probe_result& out) {
  const probe_status s = probe_leaf(loop, cfg, url, out);
  if (s != probe_status::OK || !out.is_dir) return s;

  for (size_t i = 0; i < cfg.index_names.size(); ++i) {
    probe_result sub;
    const std::string idx_url = join_url(url, cfg.index_names[i]);
    const probe_status ss = probe_leaf(loop, cfg, idx_url, sub);
    if (ss == probe_status::OK && !sub.is_dir) {
      out = sub;
      return ss;
    }
    if (ss == probe_status::FORBIDDEN) {
      out = sub;
      return ss;
    }
  }

  // 目录但没有可用索引：按"不存在"处理，**不是**策略拒绝 ——
  // 所以 SPA 回退仍然适用（这正是 SPA 该生效的场合之一）。
  const std::string dir_url = url;
  out = probe_result();
  out.status = probe_status::NOT_FOUND;
  out.final_url = dir_url;
  return out.status;
}

/**
 * @brief 读整个文件。
 *
 * 用 libuv 的**同步** fs 接口而不是 `fopen`：Windows 上 `fopen` 吃的是当前
 * ANSI 代码页，UTF-8 路径（中文目录名）会直接打不开；`uv_fs_open` 内部做
 * UTF-8 → UTF-16 转换，和 `web_real_path()` 给出的路径才是同一套编码。
 */
bool read_file(uvcpp_loop* loop, const std::string& path, int64_t size,
               std::string& out) {
  uvcpp_fs fs;
  const int fd = fs.open(loop, path.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    fs.req_cleanup();
    return false;
  }

  out.resize(static_cast<size_t>(size));
  int64_t off = 0;
  bool ok = true;
  while (off < size) {
    size_t want = static_cast<size_t>(size - off);
    if (want > k_read_chunk) want = k_read_chunk;
    uv_buf_t b = uv_buf_init(&out[static_cast<size_t>(off)],
                             static_cast<unsigned int>(want));
    const int n = fs.read(loop, fd, &b, 1, off);
    if (n <= 0) {
      ok = false;  // 被截断了（或出错）—— 宁可不发，也不发半个文件
      break;
    }
    off += n;
  }

  fs.close(loop, fd);
  fs.req_cleanup();
  if (!ok) out.clear();
  return ok;
}

}  // namespace

// =========================================================================
// Impl
// =========================================================================

struct uvcpp_web_static::Impl {
  /// 缓存候选：loop 线程查表后带进 job 的**期望快照**。
  ///
  /// 为什么要把数据本身（`data`）也带进去：worker 校验通过后要直接告诉
  /// 调用方"用你手里那份"，所以那份数据必须在 worker 可见的地方活着。
  /// shared_ptr 的引用计数让"正好在校验通过和回调之间被淘汰"也不会 UAF。
  struct cache_probe {
    std::string key;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t size;
    std::shared_ptr<std::string> data;
  };

  struct cache_entry {
    std::shared_ptr<std::string> data;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t size;
    std::list<std::string>::iterator pos;
  };

  /// 一次跨越线程的静态服务任务。
  struct job {
    Impl* self;                              ///< **只在 loop 线程用**
    std::shared_ptr<const static_config> cfg;
    uvcpp_loop* loop;                        ///< 仅用于同步 fs 调用

    uvcpp_web_request* req;                  ///< **只在 loop 线程用**
    uvcpp_web_response* resp;                ///< **只在 loop 线程用**

    /**
     * @brief 留住的 `next` —— 整个异步挂起的**唯一**凭据。
     *
     * 框架判断"这一环会不会异步恢复"的办法是量 `next` 闭包里的上下文引用数
     * 有没有变多（`uvcpp_web_context.cpp` 里那段 `kept_next`）。把它存下来
     * 就是那份"变多"，于是链会挂起等我们。**不存它的话框架会认为这一环已经
     * 结束**，立刻按当前状态发一个空响应出去，之后 worker 回来再往一个已经
     * 销毁的响应里写 —— 这正是本模块第一次跑起来时的崩溃。
     */
    uvcpp_web_next next;

    std::string url;                         ///< 已归一化的请求 URL
    bool is_head;
    std::vector<cache_probe> probes;

    /**
     * @brief 本 job 当前**持有一个工作池名额**吗。
     *
     * 名额从 `serve()` 搬进 `run_job()` 之后，"拿过"不再是一条必走的路
     * （命中缓存、404 都不拿），所以归还侧必须有个凭据 —— 否则
     * `release()` 会把别人的名额还掉，闸门自己就成了漏洞。
     */
    bool slot_held;

    // worker 产出
    probe_status status;
    bool policy_rejected;
    bool served_from_cache;
    std::string final_url;
    std::string fs_path;  ///< 只用于取 MIME 与日志，不再参与路径判断
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t size;
    std::shared_ptr<std::string> data;

    job()
        : self(nullptr),
          loop(nullptr),
          req(nullptr),
          resp(nullptr),
          next(),
          is_head(false),
          slot_held(false),
          status(probe_status::NOT_FOUND),
          policy_rejected(false),
          served_from_cache(false),
          mtime_sec(0),
          mtime_nsec(0),
          size(0) {}
  };

  uvcpp_web_static_options opts;
  std::shared_ptr<static_config> cfg;

  /**
   * @brief 工作池在途上限（可空 = 不限）。
   *
   * 共享持有而不是裸指针：App 把**自己的**那个传进来，而静态服务是
   * `shared_ptr` 交给用户的 —— 用户完全可能拿着它活过 App。裸指针会在那种
   * 用法下悬垂，`shared_ptr` 让"谁先死"都不成问题。
   */
  std::shared_ptr<uvcpp_web_work_limit> work_limit;

  /**
   * @brief 护 `lru`/`cache`/`bytes`/`hits`/`misses`/`rejected`，外加
   *        `retired` 那张**表本身**（不含表里的指针指向的东西）。
   *
   * 为什么要锁：多循环下 `serve()`（读缓存）与 `finish_job()`（写缓存、
   * 计数）分别跑在**不同的循环线程**上，而这几个成员从前是按"loop 线程只有
   * 一条"写的（`rejected` 的旧注释就写着"只在 loop 线程加减"）。
   *
   * **锁，不是分片。** 分片会让每片各有一份 LRU：内存成倍、命中率还掉
   * （同一条路径的请求落到不同片上就互相看不见）。这正是 `compress_mu_`
   * 那条先例里的取舍（`src/web/uvcpp_http_server.h:1098-1110`：「这里要的是
   * 互斥，不是分片」）。单循环下这把锁没有可争用的对象，代价是一次未争用的
   * `lock`/`unlock`。
   *
   * `mutable`：`add_probe()`/只读访问器都是 const 成员函数。
   */
  mutable std::mutex mu_;

  /** @brief 被 503 挡掉的请求数（观测用；受 `mu_` 护）。 */
  unsigned long long rejected;

  std::list<std::string> lru;  ///< 队首 = 最近使用（受 `mu_` 护）
  std::map<std::string, cache_entry> cache;  ///< 受 `mu_` 护
  size_t bytes;                 ///< 受 `mu_` 护
  unsigned long long hits;      ///< 受 `mu_` 护
  unsigned long long misses;    ///< 受 `mu_` 护

  /**
   * @brief 用完待回收的 `uvcpp_work`，**按循环分桶**。
   *
   * 为什么不是用完就地 `delete`：`uvcpp_work` 把 after_work 回调**存在自己
   * 身上**，所以在那个回调里删掉它就等于把当前正在执行的那一帧的捕获变量
   * 还回堆 —— 详见 `serve()` 里 `retire()` 处的长注释。攒到这里，等下次
   * `serve()` 进来（早已退出所有回调）再统一删。
   *
   * **为什么多循环下必须分桶**：`delete w` 的时机是"这条循环下次进 `serve()`"，
   * 而 `w` 的 after_work 排在**投递它的那条循环**上。合成一个桶的话，循环 0
   * 的 `serve()` 会把循环 1 还没回调进来的 `w` 一起删掉 —— 循环 1 随后在那个
   * 已释放的对象上执行回调（正是上面那段长注释记的同一类危险，只是换了个
   * 触发者）。分桶之后每条循环只删自己投出去的。
   *
   * **表本身**（增删桶）受 `mu_` 护 —— 两条循环同时 `retired[l]` 就是往
   * 同一个 `std::map` 上并发写。桶里的 `delete` 在锁外做。
   *
   * 数量级：两次请求之间完成的活儿，通常就是 1 个。
   */
  std::map<uvcpp_loop*, std::vector<uvcpp_work*> > retired;

  Impl() : bytes(0), hits(0), misses(0), rejected(0) {}

  ~Impl() { drain_all_retired(); }

  /// 记下一个用完的 work 请求（**只能在 after_work 回调里调**，传**投递它的
  /// 那条循环**）。
  void retire(uvcpp_loop* l, uvcpp_work* w) {
    std::lock_guard<std::mutex> lk(mu_);
    retired[l].push_back(w);
  }

  /// 真正释放 \p l 那一条循环攒下的。**绝不能在任何回调里调。**
  void drain_retired(uvcpp_loop* l) {
    std::vector<uvcpp_work*> mine;
    {
      std::lock_guard<std::mutex> lk(mu_);
      const std::map<uvcpp_loop*, std::vector<uvcpp_work*> >::iterator it =
          retired.find(l);
      if (it == retired.end()) return;
      mine.swap(it->second);
      retired.erase(it);
    }
    // 锁外删：`~uvcpp_work` 里会发生什么不该由这把锁兜着。
    for (size_t i = 0; i < mine.size(); ++i) delete mine[i];
  }

  /// 析构用：所有桶一起排空。到这里已经没有别的线程会往里放了。
  void drain_all_retired() {
    std::map<uvcpp_loop*, std::vector<uvcpp_work*> > all;
    {
      std::lock_guard<std::mutex> lk(mu_);
      all.swap(retired);
    }
    for (std::map<uvcpp_loop*, std::vector<uvcpp_work*> >::iterator it =
             all.begin();
         it != all.end(); ++it) {
      for (size_t i = 0; i < it->second.size(); ++i) delete it->second[i];
    }
  }

  bool cache_enabled() const { return opts.cache_max_entries > 0; }

  const uvcpp_web_mime_map& mime_map() const {
    return opts.mime != nullptr ? *opts.mime : uvcpp_web_mime_map::default_map();
  }

  std::string content_type_for(const std::string& fs_path) const {
    const char* t = mime_map().lookup(fs_path);
    std::string ct = (t != nullptr) ? t : "application/octet-stream";
    if (opts.add_charset && ct.find("charset") == std::string::npos &&
        uvcpp_web_mime_map::is_text(ct)) {
      ct += "; charset=utf-8";
    }
    return ct;
  }

  // ---- LRU（共享一份，受 mu_ 护；`evict()` 例外，见它自己的注释）----
  void collect_probes(const std::string& url,
                      std::vector<cache_probe>& out) const {
    add_probe(url, out);
    for (size_t i = 0; i < cfg->index_names.size(); ++i) {
      add_probe(join_url(url, cfg->index_names[i]), out);
    }
    if (cfg->spa_fallback && !cfg->spa_url.empty()) add_probe(cfg->spa_url, out);
  }

  void add_probe(const std::string& key, std::vector<cache_probe>& out) const {
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i].key == key) return;  // 去重：候选键可能撞车
    }
    // 整段都在锁里：探针是**快照**（拷的是 mtime/size 和一个 shared_ptr 引用
    // 计数），拷完这一次就与 `cache` 无关了，worker 拿它去和磁盘上的
    // mtime/size 比对 —— 语义与从前逐字相同。
    std::lock_guard<std::mutex> lk(mu_);
    const std::map<std::string, cache_entry>::const_iterator it =
        cache.find(key);
    if (it == cache.end()) return;

    cache_probe p;
    p.key = key;
    p.mtime_sec = it->second.mtime_sec;
    p.mtime_nsec = it->second.mtime_nsec;
    p.size = it->second.size;
    p.data = it->second.data;
    out.push_back(p);
  }

  /// 写入并提升到队首。命中与未命中走的是同一条路 —— 命中时它顺带把快照
  /// 刷新成 worker 刚验证过的那一份。
  void put(const std::string& key, const std::shared_ptr<std::string>& data,
           int64_t sec, int64_t nsec, int64_t size) {
    if (!cache_enabled() || !data) return;
    // 单个文件就超过总预算时干脆不缓存 —— 否则它一进来就会把整个缓存清空，
    // 然后下一次仍然装不下，退化成"每次请求都把缓存冲一遍"。
    if (opts.cache_max_bytes > 0 &&
        static_cast<size_t>(size) > opts.cache_max_bytes) {
      return;
    }

    std::lock_guard<std::mutex> lk(mu_);

    const std::map<std::string, cache_entry>::iterator old = cache.find(key);
    if (old != cache.end()) {
      bytes -= old->second.data->size();
      lru.erase(old->second.pos);
      cache.erase(old);
    }

    lru.push_front(key);
    cache_entry e;
    e.data = data;
    e.mtime_sec = sec;
    e.mtime_nsec = nsec;
    e.size = size;
    e.pos = lru.begin();
    cache[key] = e;
    bytes += data->size();

    evict_locked();
  }

  /// **要求已持有 `mu_`**（只从 `put()` 里调）。名字带后缀是为了让"光看调用点
  /// 就知道它不自己拿锁"这件事成立 —— 从前它叫 `evict()`，是私有的、单线程的。
  void evict_locked() {
    while (!lru.empty() &&
           (cache.size() > opts.cache_max_entries ||
            (opts.cache_max_bytes > 0 && bytes > opts.cache_max_bytes))) {
      const std::string& victim = lru.back();
      const std::map<std::string, cache_entry>::iterator it =
          cache.find(victim);
      if (it != cache.end()) {
        bytes -= it->second.data->size();
        cache.erase(it);
      }
      lru.pop_back();
    }
  }

  void clear_cache() {
    std::lock_guard<std::mutex> lk(mu_);
    lru.clear();
    cache.clear();
    bytes = 0;
  }

  // ---- 计数（受 mu_ 护；`finish_job()` / `finish_job()` 的 REJECTED 分支
  //      都在循环线程上，但多循环下不止一条）----
  void note_hit() {
    std::lock_guard<std::mutex> lk(mu_);
    ++hits;
  }

  void note_miss() {
    std::lock_guard<std::mutex> lk(mu_);
    ++misses;
  }

  void note_rejected() {
    std::lock_guard<std::mutex> lk(mu_);
    ++rejected;
  }

  // ---- 只读访问器（外面那几个 public 的就转发到这里）----

  size_t entries() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cache.size();
  }

  size_t bytes_used() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bytes;
  }

  unsigned long long hit_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return hits;
  }

  unsigned long long miss_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return misses;
  }

  unsigned long long rejected_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rejected;
  }

  // ---- 跨线程两侧 ----

  void run_job(job* j);     ///< worker 线程
  void finish_job(job* j);  ///< loop 线程

  /**
   * @brief 为一次**确实要读盘的** job 取工作池名额。
   *
   * @return true  = 可以继续（要么没设闸门，要么名额已到手，`j->slot_held` 为真）；
   *         false = 没抢到，`j->status` 已写成 `REJECTED`，调用方立即返回。
   *
   * 取的时机从 `serve()`（投递之前）搬到了这里 —— **这是本模块唯一被搬过
   * 位置的东西**，取名额这件事本身没变。原因见 `run_job()` 里缓存校验那一段。
   */
  bool acquire_slot(job* j);
};

// =========================================================================
// worker 线程
// =========================================================================

bool uvcpp_web_static::Impl::acquire_slot(job* j) {
  if (!work_limit) return true;  // 没设闸门 = 一律放行

  if (!work_limit->acquire()) {
    // `rejected` 是**循环线程**那一侧的计数（`note_rejected()` 在 `finish_job()`
    // 里），而这里在 worker 上，所以计数不在这里加 —— 这一支只写 job 自己的
    // 状态，不碰任何共享量。（`mu_` 能护住它，但那会让 worker 去抢一把本该
    // 只属于循环线程的锁；把写点留在业主那侧更省事，也少一处跨线程的锁。）
    j->status = probe_status::REJECTED;
    return false;
  }
  j->slot_held = true;
  return true;
}

void uvcpp_web_static::Impl::run_job(job* j) {
  probe_result pr;
  probe_status s = probe_with_index(j->loop, *j->cfg, j->url, pr);

  // SPA 回退：**只对"路径不存在"生效**。被策略挡下的路径绝不能回落到 SPA ——
  // 否则 `/.env` 会拿到一个 200 的 index.html，一次探测就变成了正常响应，
  // 安全问题再也看不见。
  if (s == probe_status::NOT_FOUND && !pr.policy_rejected &&
      j->cfg->spa_fallback && !j->cfg->spa_url.empty()) {
    probe_result sp;
    const probe_status ss =
        probe_with_index(j->loop, *j->cfg, j->cfg->spa_url, sp);
    if (ss == probe_status::OK && !sp.is_dir) {
      pr = sp;
      s = ss;
    }
  }

  j->status = s;
  j->policy_rejected = pr.policy_rejected;
  j->final_url = pr.final_url;
  j->fs_path = pr.fs_path;
  j->mtime_sec = pr.mtime_sec;
  j->mtime_nsec = pr.mtime_nsec;
  j->size = pr.size;
  j->served_from_cache = false;

  if (s != probe_status::OK || pr.is_dir) return;

  // 尺寸闸门放在读盘**之前**：超限的请求一个字节都不读，直接 413。
  // 这是"读进内存再一次写出去"这个实现的必要配套，没有它，一个 2 GiB 的
  // 文件请求就能把进程内存打爆。
  if (j->cfg->max_file_size > 0 &&
      static_cast<uint64_t>(pr.size) > j->cfg->max_file_size) {
    j->status = probe_status::TOO_LARGE;
    return;
  }

  // ---- 阈值分流：大文件不整读 ----
  //
  // **这里才是判定的正确位置，不能在 `serve()` 里判。** 文件多大只有 worker
  // stat 完之后才知道 —— 在 `serve()` 里判等于要提前 stat 一次，那正是本模块
  // 一直在避免的"每个请求多一次系统调用"。
  //
  // 判据用 `>` 而不是 `>=`：**恰好等于阈值仍然整读**。理由是两条路的峰值
  // 在阈值处本来就相等（2 × 阈值），取哪边都不违反上界，而"≤ 阈值"这个
  // 说法在文档、选项注释和用例里都好写、不用解释半天。
  //
  // 阈值 0 表示"一律走流式"（见 options 的说明），所以这里**不能**写成
  // `thr > 0 && ...` —— 那样 0 就变成了"从不流式"，与文档相反。
  const size_t thr = j->cfg->max_cached_file_size;
  if (static_cast<uint64_t>(pr.size) > static_cast<uint64_t>(thr)) {
    // 一个字节都不读。元数据已经全部带在 job 上了（上面那一段赋过值），
    // 所以 ETag/Last-Modified/条件请求/Range 在 loop 线程那侧照常判定。
    //
    // **流式也要占名额**：它不整读，但确实在读盘 —— 闸门在这条路上的意义
    // 与整读路径完全一样（见 acquire_slot）。
    if (!acquire_slot(j)) return;
    j->status = probe_status::STREAM;
    return;
  }

  // 缓存校验：拿 loop 线程给的期望快照和刚 stat 到的真实值比。
  // 对上了就**不读盘** —— 热文件的成本是一次 stat。
  //
  // **命中路径不占名额**，这是本模块最关键的一条：命中时 worker 只做了一次
  // `stat`（几微秒），一个字节的磁盘读都没有，内存面也早就有 `max_cached_file_size`
  // 封着。让这样一次请求去抢名额，代价是**并发一过闸门就大面积 503**，而收益
  // 是零 —— 实测并发 64 时 71.7% 的请求被拒，其中 99.97% 本来能命中缓存。
  for (size_t i = 0; i < j->probes.size(); ++i) {
    const cache_probe& p = j->probes[i];
    if (p.key == pr.final_url && p.size == pr.size &&
        p.mtime_sec == pr.mtime_sec && p.mtime_nsec == pr.mtime_nsec) {
      j->data = p.data;
      j->served_from_cache = true;
      return;
    }
  }

  // 走到这里只剩一条路：**把文件整读进内存**。这才是闸门要卡的那件事。
  if (!acquire_slot(j)) return;

  std::shared_ptr<std::string> buf(new std::string());
  if (!read_file(j->loop, pr.fs_path, pr.size, *buf)) {
    UVCPP_LOG_ERROR(log_category::IO)
        << "静态文件读取失败: " << pr.fs_path << "（" << pr.size << " 字节）";
    j->status = probe_status::IO_ERROR;
    return;
  }
  j->data = buf;
}

// =========================================================================
// loop 线程
// =========================================================================

void uvcpp_web_static::Impl::finish_job(job* j) {
  uvcpp_web_request& req = *j->req;
  uvcpp_web_response& resp = *j->resp;

  switch (j->status) {
    case probe_status::NOT_FOUND:
      // 404 而不是 403/400：路径归一化失败、穿越、父目录不存在……全部
      // 收敛成同一个答案，探测者拿不到任何"这个路径有蹊跷"的信号。
      resp.not_found();
      return;
    case probe_status::FORBIDDEN:
      UVCPP_LOG_WARN(log_category::STATIC)
          << "静态请求被策略拒绝: " << req.path();
      resp.forbidden();
      return;
    case probe_status::TOO_LARGE:
      UVCPP_LOG_WARN(log_category::STATIC)
          << "静态文件超过尺寸上限（" << j->size << " > " << opts.max_file_size
          << "）: " << j->final_url;
      resp.payload_too_large();
      return;
    case probe_status::IO_ERROR:
      resp.server_error();
      return;
    case probe_status::REJECTED:
      // 名额没抢到。形状与原先 `serve()` 里那一支**逐字一致** —— 只是判定的
      // 位置从"投递之前"挪到了"真要读盘之前"（见 `run_job`）。
      note_rejected();  // 观测用计数；多循环下不止一条循环线程会加减，故走锁
      UVCPP_LOG_WARN(log_category::STATIC)
          << "工作池已满（在途 " << (work_limit ? work_limit->in_flight() : 0)
          << " / " << (work_limit ? work_limit->limit() : 0)
          << "），拒绝静态请求 " << j->final_url;
      resp.service_unavailable();
      // `Retry-After` 是 503 的配套语义（RFC 9110 §10.2.3）：告诉对端"等 1 秒
      // 再来"。**必须给这个头** —— 没有它，客户端只能靠猜，实测最常见的反应
      // 是立刻重试，正好把已经饱和的池子压得更死。
      resp.set_header("retry-after", "1");
      return;
    case probe_status::OK:
    case probe_status::STREAM:
      break;  // STREAM 只换产出 body 的方式，元数据与 OK 完全一样
  }

  const std::string ct = content_type_for(j->fs_path);
  const std::string etag =
      opts.etag ? make_etag(j->size, j->mtime_sec, j->mtime_nsec)
                : std::string();
  const std::string last_modified =
      opts.last_modified ? web_http_date(static_cast<time_t>(j->mtime_sec))
                         : std::string();

  // 这几个头在 200/206/304 上都要有，所以先发，后面不再重复。
  if (opts.etag && !etag.empty()) resp.set_header("etag", etag);
  if (opts.last_modified && !last_modified.empty()) {
    resp.set_header("last-modified", last_modified);
  }
  if (!opts.cache_control.empty()) {
    resp.set_header("cache-control", opts.cache_control);
  }
  resp.set_content_type(ct);
  const bool ranged_ok = opts.range;
  if (ranged_ok) resp.set_header("accept-ranges", "bytes");

  // ---- 条件请求 ----
  //
  // 顺序不能反：RFC 7232 §3.3 规定 `If-None-Match` 存在时
  // `If-Modified-Since` **必须被忽略**。两个都看会让"客户端手里那份的
  // ETag 不匹配、但时间戳却在文件之前"这种情况错误地回 304，客户端于是
  // 一直用着旧内容 —— 而且它自己永远发现不了。
  bool not_modified = false;
  if (opts.etag && !etag.empty() && req.has_header("if-none-match")) {
    not_modified =
        etag_list_matches(req.header("if-none-match"), etag);
  } else if (opts.last_modified && req.has_header("if-modified-since")) {
    const time_t t = web_parse_http_date(req.header("if-modified-since"));
    if (t != static_cast<time_t>(-1)) {
      // 只比到秒：HTTP 日期本身就只有秒精度，拿纳秒去比会比客户端永远
      // "更新"一点，导致 304 永不生效。
      not_modified = (j->mtime_sec <= static_cast<int64_t>(t));
    }
  }

  if (not_modified) {
    UVCPP_LOG_DEBUG(log_category::STATIC) << "304 " << j->final_url;
    resp.not_modified();
    return;
  }

  // ---- 范围请求 ----
  int64_t first = 0;
  int64_t last = (j->size > 0) ? (j->size - 1) : 0;
  bool partial = false;

  if (ranged_ok && req.has_header("range")) {
    // `If-Range` 不匹配时必须**忽略 Range 回全量**，而不是回 412：
    // 客户端的意思是"要是没变就给我续传的那一段，变了就给我完整的"，
    // 它并没有把请求设成条件的。
    bool range_allowed = true;
    if (req.has_header("if-range")) {
      const std::string v = web_trim(req.header("if-range"));
      if (!v.empty()) {
        if (v[0] == '"' || web_starts_with_ci(v, "W/")) {
          range_allowed = !etag.empty() && (strip_quotes(v) == strip_quotes(etag));
        } else {
          const time_t t = web_parse_http_date(v);
          range_allowed =
              (t != static_cast<time_t>(-1)) &&
              (j->mtime_sec <= static_cast<int64_t>(t));
        }
      }
    }

    if (range_allowed) {
      int64_t rf = 0;
      int64_t rl = 0;
      const range_parse rp = parse_range(req.header("range"), j->size, rf, rl);
      if (rp == range_parse::UNSATISFIABLE) {
        UVCPP_LOG_DEBUG(log_category::STATIC)
            << "416 " << j->final_url << "（Range: "
            << req.header("range") << "，共 " << j->size << " 字节）";
        // `range_not_satisfiable()` 只在 total > 0 时才发 Content-Range ——
        // 在那个重载里 0 表示"长度未知，没法写"。可这里长度是**已知的 0**
        // （空文件），而 RFC 7233 §4.4 要求 416 带上当前长度，`bytes */0`
        // 才是对的。所以空文件这条自己发；下面那次调用只写状态码，
        // 不会把这个头覆盖掉。
        if (j->size == 0) resp.set_header("content-range", "bytes */0");
        resp.range_not_satisfiable(static_cast<unsigned long long>(j->size));
        return;
      }
      if (rp == range_parse::OK) {
        first = rf;
        last = rl;
        partial = true;
      }
    }
  }

  const int64_t count = partial ? (last - first + 1) : j->size;

  if (partial) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "bytes %lld-%lld/%lld",
                  static_cast<long long>(first), static_cast<long long>(last),
                  static_cast<long long>(j->size));
    resp.status(206);
    resp.set_header("content-range", buf);
  }

  if (j->status == probe_status::STREAM) {
    // 分片读下发：`count` == last-first+1，与 send_file_range 自己设的
    // Content-Length 一致，所以 206/`Content-Range` 那两条照旧成立。
    //
    // HEAD 也走这一支：`start_file_transfer()` 见到 `head_only_` 会自己短路成
    // "只发头、不读盘"，而 `arm_file_transfer()` 设的长度与 GET 那份逐字节相同。
    resp.send_file_range(j->fs_path, static_cast<uint64_t>(first),
                         static_cast<uint64_t>(last));
  } else if (j->data) {
    // **整份文件走共享**：`j->data` 本来就是一份 `shared_ptr<std::string>`，
    // 把引用递给响应体 = 一次引用计数加一，**一个字节都不拷**。
    //
    // 这一份拷贝原先是真的白付：命中压缩变体缓存时，`uvcpp_http_server` 拿到
    // 响应后下一句就是 `resp.body.clear()` + 换成压缩产物（见那边命中那一支），
    // 中间没有任何一处读过这份拷贝。
    //
    // 为什么不是 `body_move()`：那份内容归**静态层的 LRU** 所有，之后还要靠它
    // 服务重复请求。move 会把条目掏空，等于每次缓存命中都要重读一次盘 —— 那是
    // 用一次拷贝换一次磁盘 IO，方向反了。
    //
    // 为什么 Range 不能共享：`partial` 发的是文件中间的一段，而共享视图指的是
    // **整份**内容，共享出去就会把整个文件当切片发（头还声明着那一段）。这里
    // 保持原来的拷贝语义。
    //
    // 长度对不上也回退到拷贝：一旦 `j->data` 比 `j->size` 短，共享会让
    // `content-length` 声明得比实际发出去的字节多 —— "少发一段"会变成"对端一直
    // 等"，宁可多拷这一次。
    const bool share_whole =
        !partial && j->data->size() == static_cast<size_t>(j->size);
    if (share_whole) {
      // 引用递出去之后，这份内容由**响应**（以及写出队列）一起保着 ——
      // 静态层的 LRU 此刻把它淘汰掉，也不会让响应手里的指针悬空。
      resp.body_share(j->data);
    } else {
      resp.body(j->data->data() + static_cast<size_t>(first),
                static_cast<size_t>(count), std::string());
    }

    // 压缩变体缓存的键（见 `uvcpp_http_response::cache_tag`）。
    //
    // 用**文件身份**而不是 URL：两个 URL 落到同一个文件（`/` 与 `/index.html`）
    // 应当共用同一份压缩结果。这四个字段就是 LRU 校验原始条目用的那一组
    // （`:780-781` 的 `key/size/mtime_sec/mtime_nsec`），所以"文件变了"必然
    // 导致键变，不需要任何额外的失效逻辑。
    //
    // **`partial` 一律不设 tag。** 这四个字段标识的是**整个文件**，而 body 是
    // `first` 起的 `count` 个字节 —— 同一个文件的全量请求与任意一段 Range 会
    // 算出**同一个键**，body 却完全不同。两种次序都错、都不报错：先全量后 Range
    // 时 206 会拿到整个文件的 gzip（头声明 206、体是整个文件）；先 Range 后全量
    // 更贵 —— 一个正常的 200 会静默收到那段切片的 gzip，调用方无从分辨。
    //
    // 也可以把 `first`/`count` 编进键、让每段 Range 各自成条，但那样 Range 就能
    // 用任意多的键把"整个文件的变体"挤出去（淘汰只看最近使用），而换来的只是
    // "同一段 Range 重复请求"这一个边角收益。不值。
    if (!partial) {
      resp.raw().cache_tag = j->fs_path + "\x1f" + std::to_string(j->size) +
                             "\x1f" + std::to_string(j->mtime_sec) + "\x1f" +
                             std::to_string(j->mtime_nsec);
    }
  }

  // **HEAD 的次序是"先把 body 装好，再让序列化把 body 丢掉"。**
  //
  // 装进来的这份 body 不是发给对端的，是**给 HTTP 层的压缩器算长度用的**：
  // `content-length` 与 `content-encoding` 都是压缩决策的结果，不给真 body
  // 就会被 `apply_compression()` 的尺寸门直接挡掉，HEAD 于是报**未压缩**的
  // 长度、GET 报压缩后的长度 —— RFC 9110 §9.3.2 要的"HEAD 与 GET 相同的头
  // 字段"当场不成立，而**拿 HEAD 探长度、再按那个长度读满的客户端会一直等到
  // 超时**（本层原来就是这个行为，`resp.set_header("content-length", size)`
  // 显式钉的是文件原始长度）。
  //
  // 代价：HEAD 与 GET 一样要读一次盘、一样进缓存、一样计命中/未命中。超过
  // `max_cached_file_size` 的大文件两边都不读，头本来就一致，不受影响。
  //
  // 丢掉 body 分别发生在 `to_string(include_body=false)`（h1）与
  // `send_h2_response` 的 `omit_body=true`（h2）—— 见 `sync_meta()` 里那段。
  if (j->is_head) resp.set_head_only(true);

  // 缓存只在真的读到了字节时更新。HEAD 现在与 GET 同路，所以它也进缓存、
  // 也计进命中/未命中。
  if (j->data) {
    if (j->served_from_cache) {
      note_hit();
    } else {
      note_miss();
      put(j->final_url, j->data, j->mtime_sec, j->mtime_nsec, j->size);
    }
  }
}

// =========================================================================
// 选项
// =========================================================================

uvcpp_web_static_options::uvcpp_web_static_options(size_t max_file)
    : spa_fallback(false),
      spa_file("index.html"),
      max_file_size(max_file),
      etag(true),
      last_modified(true),
      range(true),
      cache_control(),
      cache_max_entries(256),
      cache_max_bytes(32u * 1024u * 1024u),
      max_cached_file_size(1024u * 1024u),
      dotfiles(uvcpp_web_dotfile_policy::HIDE),
      follow_symlinks(true),
      add_charset(true),
      mime(nullptr) {
  index_files.push_back("index.html");
}

// =========================================================================
// uvcpp_web_static
// =========================================================================

namespace {

/**
 * @brief 索引文件名是否合法：必须是**单段**的普通文件名。
 *
 * 配置里出现 `../x` 或 `a/b` 一律丢掉并告警 —— 索引名是唯一一处会绕过
 * URL 归一化、直接参与拼路径的配置值，必须在这里钉死，不能指望调用方守规矩。
 */
bool valid_index_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  if (name[0] == '/' || name[0] == '\\') return false;
  // 点开头的索引名会**绕过 dotfile 策略**（策略检查的是请求 URL，而索引是
  // 由它拼出来的）。索引文件本来也没有理由叫 `.index.html`，直接判非法。
  if (name[0] == '.') return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '/' || c == '\\' || c == ':' || c == '\0') return false;
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return false;
  }
  return true;
}

}  // namespace

uvcpp_web_static::uvcpp_web_static(const std::string& root_dir,
                                   const uvcpp_web_static_options& opts)
    : impl_(new Impl()) {
  impl_->opts = opts;

  std::shared_ptr<static_config> cfg(new static_config());
  cfg->spa_fallback = opts.spa_fallback;
  cfg->max_file_size = opts.max_file_size;
  cfg->max_cached_file_size = opts.max_cached_file_size;
  cfg->follow_symlinks = opts.follow_symlinks;

  // 文档根：构造时归一化一次并缓存。之后每个请求都要拿它做包含判断，
  // 每请求算一遍 realpath 是纯浪费（而且那是个系统调用）。
  std::string real;
  if (!web_real_path(root_dir, real, /*allow_missing=*/false)) {
    UVCPP_LOG_ERROR(log_category::STATIC)
        << "静态文档根解析失败（目录不存在或不可访问），所有请求将回 500: "
        << root_dir;
  } else {
    cfg->root_real = real;
    UVCPP_LOG_INFO(log_category::STATIC) << "静态服务文档根: " << real;
  }

  for (size_t i = 0; i < opts.index_files.size(); ++i) {
    if (valid_index_name(opts.index_files[i])) {
      cfg->index_names.push_back(opts.index_files[i]);
    } else {
      UVCPP_LOG_WARN(log_category::STATIC)
          << "索引文件名非法，已忽略: [" << opts.index_files[i] << "]";
    }
  }

  if (opts.spa_fallback) {
    std::string spa_url;
    const web_path_status ws = web_sanitize_path(
        opts.spa_file.empty() ? std::string("/index.html")
                              : ("/" + opts.spa_file),
        spa_url, web_path_options(4096, 64, /*decode=*/false));
    if (ws != web_path_status::OK || spa_url == "/") {
      UVCPP_LOG_WARN(log_category::STATIC)
          << "SPA 入口非法，已关闭回退: [" << opts.spa_file << "]";
    } else {
      cfg->spa_url = spa_url;
    }
  }

  impl_->cfg = cfg;
}

uvcpp_web_static::~uvcpp_web_static() { delete impl_; }

const std::string& uvcpp_web_static::root_real() const {
  return impl_->cfg->root_real;
}

void uvcpp_web_static::clear_cache() { impl_->clear_cache(); }

size_t uvcpp_web_static::cache_entries() const { return impl_->entries(); }

size_t uvcpp_web_static::cache_bytes() const { return impl_->bytes_used(); }

unsigned long long uvcpp_web_static::cache_hits() const {
  return impl_->hit_count();
}

unsigned long long uvcpp_web_static::cache_misses() const {
  return impl_->miss_count();
}

void uvcpp_web_static::set_work_limit(
    const std::shared_ptr<uvcpp_web_work_limit>& limit) {
  impl_->work_limit = limit;
}

std::shared_ptr<uvcpp_web_work_limit> uvcpp_web_static::work_limit() const {
  return impl_->work_limit;
}

unsigned long long uvcpp_web_static::rejected_count() const {
  return impl_->rejected_count();
}

void uvcpp_web_static::serve(uvcpp_web_request& req, uvcpp_web_response& resp,
                             uvcpp_web_next next, uvcpp_loop* loop) {
  Impl* im = impl_;

  // 先把**本条循环**上一批用完的 `uvcpp_work` 真正删掉。**这里才是安全的
  // 删除点**：`serve()` 是 loop 线程上的普通调用，不在任何 libuv 回调里面
  // （尤其不在 `uvcpp_work` 的 after_work 里），所以没有"删掉正在执行的那个
  // 闭包"的问题。详见 Impl::retired 的注释 —— 传 `loop` 是因为多循环下只能
  // 删自己投出去的那些。
  im->drain_retired(loop);

  // 文档根没解析出来是部署错误，不是"文件不存在"。500 让它立刻可见 ——
  // 静默 404 只会让人以为是自己 URL 写错了。
  //
  // 下面几条**同步**路径（以及后面的 dotfile / 路径非法）都是"填好响应就
  // 返回"，没有留 next —— 于是框架会自己收尾并把响应发出去。这里显式
  // `end()` 是为了把链正常终结掉：不写的话每次都要多打一条"处理器链结束但
  // 没有结束响应"的 WARN，而那条 WARN 是留给真忘了 end 的人看的。
  if (im->cfg->root_real.empty()) {
    resp.server_error();
    resp.end();
    return;
  }
  if (loop == nullptr) {
    UVCPP_LOG_ERROR(log_category::STATIC) << "serve() 没拿到循环，回 500";
    resp.server_error();
    resp.end();
    return;
  }

  // 通配段。`/static`（无通配）那条路由取不到参数，按空前缀处理 ——
  // 于是 `/static` 与 `/static/` 走的是同一条路，旧实现里"挂载前缀取不到
  // index"的毛病在这里结构性地不存在。
  const std::string* p = req.param("path");
  std::string raw = "/";
  if (p != nullptr && !p->empty()) {
    raw = ((*p)[0] == '/') ? *p : ("/" + *p);
  }

  // **不再解码**。`req.path()` 在请求层已经解过一次，这里再解一次会把文件名
  // 里字面的 `%2e` 变成 `.`、`a%2fb` 变成目录分隔符 —— 那是凭空造出一个
  // 新的路径，而且是往攻击者想要的方向变。
  std::string url;
  const web_path_status ws =
      web_sanitize_path(raw, url, web_path_options(4096, 64, /*decode=*/false));
  if (ws != web_path_status::OK) {
    UVCPP_LOG_WARN(log_category::STATIC)
        << "拒绝非法静态路径 [" << raw << "]: " << web_path_status_name(ws);
    resp.not_found();
    resp.end();
    return;
  }

  // dotfile 判定在**投递之前**做：探测 `/.env` 的成本是一次字符串扫描，
  // 一个线程池任务都不该花。
  if (path_has_dotfile_segment(url)) {
    if (im->opts.dotfiles == uvcpp_web_dotfile_policy::DENY) {
      UVCPP_LOG_WARN(log_category::STATIC) << "拒绝 dotfile 访问: " << url;
      resp.forbidden();
      resp.end();
      return;
    }
    if (im->opts.dotfiles == uvcpp_web_dotfile_policy::HIDE) {
      resp.not_found();
      resp.end();
      return;
    }
    // ALLOW：照常往下走
  }

  // **工作池名额不在这里取**（曾经在这里）。闸门想限制的是"并发的磁盘读"，
  // 而它的计数单位过去是"请求" —— 这两者在缓存未命中时重合、在命中时不重合，
  // 而命中是绝大多数。于是并发一过闸门，连"只做了一次 stat、一个字节都没读"
  // 的请求也一起被 503 掉。取名额的时机因此搬到了 `run_job()` 里真正要读盘
  // 那一句之前，那里才是"确实要占一份 IO"的判定点。
  //
  // 随之改变的只有 503 的产出位置：从这里的同步回绝变成 `finish_job()` 里的
  // 一个 `probe_status::REJECTED` 分支，响应内容（503 + Retry-After）不变。
  Impl::job* j = new Impl::job();
  j->self = im;
  j->cfg = im->cfg;
  j->loop = loop;
  j->req = &req;
  j->resp = &resp;
  j->url = url;
  j->is_head = (req.method() == http_method::HTTP_HEAD);

  // 留住 next。**赋值必须发生在 `serve()` 返回之前**（也就是 handler 调用
  // 返回之前）—— 框架是在 handler 返回的那一刻量引用数来判断"留没留"的。
  j->next = next;

  if (im->cache_enabled()) im->collect_probes(url, j->probes);

  uvcpp_work* work = new uvcpp_work();
  const int rc = work->queue_work(
      loop,
      [j](uvcpp_work*) { j->self->run_job(j); },
      [j](uvcpp_work* w, int) {
        // 这个回调**本身就是"worker 已经返回"**，所以名额在这里就该还 ——
        // 早还一拍，排在后面的请求就早一拍被受理。
        //
        // **只能还自己拿过的那一份**：名额搬进 worker 之后，"拿过"不再是必走
        // 的路（命中缓存 / 404 都不拿），无条件 release 会把别人的名额
        // 还掉 —— 闸门自己就成了漏洞。凭据是 `j->slot_held`。
        //
        // 与 `serve()` 里的 `rc != 0` 分支**互斥**（libuv 的约定：
        // `uv_queue_work` 返回 0 才会调 after_work），而且两条路都会 `delete
        // j`，所以不存在还两次的可能。
        if (j->slot_held) j->self->work_limit->release();

        // **绝对不能在回调里 `delete w`。**
        //
        // `w->m_after_work_cb` 就是**正在执行的这个闭包本身** —— 它的存储
        // 就在 `w` 里（闭包小到内联时）或由 `w` 的析构负责释放（大到上堆
        // 时）。在回调里删掉 `w`，等于把当前这一帧赖以取捕获变量的内存还回
        // 堆，之后每一次读 `j` 都可能读到 0xFEEEFEEE。
        //
        // 这不是理论问题：本模块第一版就是这么写的，表现是约 50% 的用例组
        // 随机段错误，`cdb` 里的现场是
        // `callback_after_work` → 本文件 → 读 `ds:feeefeee` —— 而它**只在
        // 寄存器分配恰好要重新装载 `j` 时才崩**，所以时快时慢、换一组用例
        // 就换个位置，看着完全不像同一个 bug。
        //
        // 交给 `serve()` 下次进来时统一回收：那时早已不在任何回调里。
        // 键取 `j->loop` —— 就是投递这次 work 的那条循环，也就是**正在跑本
        // 回调**的这一条；下次它自己进来 `serve()` 时才会删掉。
        j->self->retire(j->loop, w);

        j->self->finish_job(j);

        // 响应填好了，**现在才**结束它并把链放行到收尾 —— 收尾那一步
        // （`uvcpp_web_context::finish()`）才会真的把响应发出去。
        j->resp->end();

        // 先把 next 拷出来再删 job：`resume()` 一路走到发送，可能正好把
        // 上下文（连同 `j->resp` 指向的对象）销毁掉，而那份 next 副本
        // 本身就持有上下文，所以它是安全的最后一步。
        const uvcpp_web_next resume = j->next;
        delete j;
        if (resume) resume();
      });
  if (rc != 0) {
    UVCPP_LOG_ERROR(log_category::STATIC)
        << "投递静态读盘任务失败（" << rc << "）";
    delete work;
    // 任务从来没进过池子 —— 而名额现在是在 worker 里才取的（见 `run_job`），
    // 所以这一支**根本没有名额可还**。原先那句 `release()` 在今天会凭空把
    // 别人的名额还掉。
    // 删掉 job 同时也就丢掉了 next 的那份副本，于是框架照常收尾、
    // 把下面这个 500 发出去。
    delete j;
    resp.server_error();
    resp.end();
  }
}

}  // namespace uvcpp
