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

#include <atomic>
#include <cstdio>
#include <cstring>
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

/// 把 \p v 的十进制接到 \p out 的尾部。
///
/// 不用 `out += std::to_string(v)`：那一句先造一个临时串（短数走 SSO，不落堆，
/// 但仍然要多走一次移动赋值），而 `snprintf` 到一个栈缓冲区再 `append` 就是
/// 一次。位数上界：`-9223372036854775808` 是 20 个字符，24 字节够。
void append_i64_dec(std::string& out, int64_t v) {
  char buf[24];
  const int n =
      uvcpp_snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) {
    out.append(buf, static_cast<size_t>(n));
  }
}

/// 无符号整数的十六进制，写进调用方的缓冲区，返回写入长度（不含 NUL）。
///
/// 不用 `std::snprintf("%llx")`：那一个要过一遍格式串解析，而这里要写的是
/// **每个 200 响应**上的那三个数。进位表就只有十六个字符，手写更快也更清楚
/// （`0` 仍然写成 `0`，不是空串）。
size_t u64_to_hex_into(char* out, uint64_t v) {
  char tmp[16];
  size_t n = 0;
  do {
    tmp[n++] = "0123456789abcdef"[v & 0xf];
    v >>= 4;
  } while (v != 0);
  for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  return n;
}

/**
 * @brief 生成 ETag：`"<大小>-<mtime秒>-<mtime纳秒>"`。
 *
 * 和 nginx 一样只用 mtime + size 这两个字段，**不是内容哈希**。取舍的理由：
 * 哈希要读全文，等于把缓存省下的那份开销又原样花掉；而这个校验器的目的是
 * 「客户端手里那份还要不要重下」，mtime 的分辨率（Windows 100ns / Linux ns）
 * 在实践中够用。真需要内容级指纹的场合，应当由使用者在构建期把指纹编进
 * URL（`app.<hash>.js`），那也是更有效的缓存策略。
 *
 * **为什么主路径把它写成 C 串而不是 `std::string`**：这一个值每个 200 响应
 * 都要发一次，而它只是三个十六进制数加两个连字符。`std::string` 那一版是
 * `std::string("\"") + a + "-" + b + "-" + c + "\""` —— 五次 `operator+`，
 * 每一次都产出一个临时串（短的那些走 SSO，一旦拼过 15 字节就落堆）。而这条
 * 路上**真正需要的那一份**只有头表持有的那份。所以 `finish_job` 写进一个
 * 栈缓冲区、用 `set_header(const char*, const char*)` 发出去，只有客户端
 * 真的带了 `If-None-Match` / `If-Range` 时才就地建一个 `std::string` 去比。
 */
size_t format_etag_into(char* out, size_t cap, int64_t size, int64_t mtime_sec,
                        int64_t mtime_nsec) {
  // 三个 int64 各最多 16 个十六进制字符，加两个连字符、两个引号、结尾 NUL
  // ⇒ 上界 53。要 64 是留了余量的整齐上界。
  if (cap < 64) {
    out[0] = '\0';
    return 0;
  }
  size_t n = 0;
  out[n++] = '"';
  n += u64_to_hex_into(out + n, static_cast<uint64_t>(size));
  out[n++] = '-';
  n += u64_to_hex_into(out + n, static_cast<uint64_t>(mtime_sec));
  out[n++] = '-';
  n += u64_to_hex_into(out + n, static_cast<uint64_t>(mtime_nsec));
  out[n++] = '"';
  out[n] = '\0';
  return n;
}

/// `std::string` 那一版。留着是因为它是这个值的**语义入口**（名字说明了它
/// 是什么），而调用方只需要 C 串时走上面那份 —— 见 `finish_job` 里的长注释。
std::string make_etag(int64_t size, int64_t mtime_sec, int64_t mtime_nsec) {
  char buf[64];
  const size_t n =
      format_etag_into(buf, sizeof(buf), size, mtime_sec, mtime_nsec);
  return std::string(buf, n);
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
  bool resolve_memo;                  ///< 见选项：关掉就每请求一次 realpath
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
///
/// `dev`/`ino` 可选（空表示调用方不关心）：它们和别的字段一起在
/// `req_cleanup()` 之前抄出来，所以调用方拿到的是一次 stat 的**同一份**
/// 快照，不存在"抄了 mtime 又抄 inode、两次 stat 之间文件被换掉"。
bool stat_real(uvcpp_loop* loop, const std::string& path, bool& is_dir,
               int64_t& sec, int64_t& nsec, int64_t& size,
               uint64_t* dev = nullptr, uint64_t* ino = nullptr) {
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
      if (dev != nullptr) *dev = static_cast<uint64_t>(st->st_dev);
      if (ino != nullptr) *ino = static_cast<uint64_t>(st->st_ino);
      ok = true;
    }
  }
  fs.req_cleanup();
  return ok;
}

/**
 * @brief 「上一次这个 URL 被完整解析到哪儿」的记忆 —— 见 `probe_leaf` 的快路径。
 *
 * 三个字段缺一不可：`fs_path` 是那次 `web_resolve_within_root()` 的产物（同时
 * 也是检查的**对象**），`(dev, ino)` 是那一刻这个文件在文件系统里的身份。
 * 三者的关系是：**只要 `stat(fs_path)` 还能拿回同一对 `(dev, ino)`，那么
 * `realpath(root + url)` 今天解出来的就还是同一个文件** —— 因为 `stat` 与
 * `realpath` 走的是同一套 VFS 解析，`stat` 跟到哪个 inode，`realpath` 就
 * 规范到哪个路径。
 *
 * 于是那次解析得出的两个结论都还成立：
 *  - 「这个文件在根内」—— 它是同一个 inode，内容一字不差；
 *  - 「它的 mtime/size 是 X」—— 那次是 X，但**今天的值以今天的 stat 为准**，
 *    快路径并不因此跳过尺寸闸门或缓存比对。
 *
 * 为什么 `dev` 必须一起比：inode 号只在**同一个设备内**唯一，跨设备会重号。
 */
struct resolve_hint {
  std::string fs_path;
  uint64_t dev;
  uint64_t ino;

  /**
   * @brief 上一次解析**成不成功**。负数记忆（`false`）同样是钱。
   *
   * 它存在的理由是孪生文件那一圈：`probe_job()` 会依序探 `url + ".br"`、
   * `url + ".gz"`，而语料里**没有**孪生的那些文件两条都要探、两条都失败 ——
   * 只记正数的话，这批请求每一条都要重跑两次完整的 `realpath`（本机量具上
   * 这正是 `realpath`/req 停在 0.98 而不是 0 的原因）。解析失败与解析成功
   * 一样是"查过磁盘得出的结论"，没有理由只记一半。
   *
   * 负数记忆的**校验**与正数不同（没有 inode 可比）：靠一次 `lstat()` 仍然
   * 失败来证明"它还是不在这儿"。两个方向都只要**一次**系统调用，这正是
   * 快路径值得存在的形状。
   */
  bool exists;

  resolve_hint() : dev(0), ino(0), exists(false) {}
};

/**
 * @brief 一次请求要用的全部记忆，**按探测 URL 键**，线性扫。
 *
 * 键取的是 `probe_leaf()` 的 `url` 参数（也就是真正会被拼出来 stat 的那个
 * 字符串），不是最终命中的 URL —— 于是索引文件、SPA 回退、预压缩孪生各自
 * 有自己的一条，三者互不覆盖，`probe_leaf()` 也不必知道"上层最后选了谁"。
 *
 * 用 `vector` 而不是 `map`：候选键的个数是 `(1 + 索引名个数 + spa) × 3`，
 * 正常配置下是个位数 —— 与 `Impl::add_one_probe_locked()` 的去重同一个理由。
 */
typedef std::vector<std::pair<std::string, resolve_hint> > hint_vec;

/// 在 `hints` 里找 `url` 的记忆；没有就返回空指针。
/// **只在快路径上被调一次**，找不到等价于"以前没解析过这个 URL"。
const resolve_hint* find_hint(const hint_vec* hints, const std::string& url) {
  if (hints == nullptr) return nullptr;
  for (size_t i = 0; i < hints->size(); ++i) {
    if ((*hints)[i].first == url) return &(*hints)[i].second;
  }
  return nullptr;
}

/// 记下 `url` 这次的解析结论（幂等：同一个 `url` 只留最后一条）。
void record_hint(hint_vec* out, const std::string& url, const std::string& path,
                 uint64_t dev, uint64_t ino) {
  if (out == nullptr) return;
  resolve_hint h;
  h.fs_path = path;
  h.dev = dev;
  h.ino = ino;
  h.exists = true;
  for (size_t i = 0; i < out->size(); ++i) {
    if ((*out)[i].first == url) {
      (*out)[i].second = h;
      return;
    }
  }
  out->push_back(std::make_pair(url, h));
}

/// 记下「`url` 这次解析**没成**」。
void record_miss_hint(hint_vec* out, const std::string& url) {
  if (out == nullptr) return;
  resolve_hint h;
  h.exists = false;
  for (size_t i = 0; i < out->size(); ++i) {
    if ((*out)[i].first == url) {
      (*out)[i].second = h;
      return;
    }
  }
  out->push_back(std::make_pair(url, h));
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

/// `lstat` 成功了吗 —— 也就是"这条路径的**最后一段**在不在"（不跟随它）。
///
/// 与 `lstat_is_symlink()` 只差**报告什么**：那条报"是不是链接"（出错也报
/// false），这条报"在不在"。负数记忆的校验用的就是这一条，见 `probe_leaf()`。
bool lstat_exists(uvcpp_loop* loop, const std::string& path) {
  uvcpp_fs fs;
  const bool ok = (fs.lstat(loop, path.c_str()) == 0);
  fs.req_cleanup();
  return ok;
}

/**
 * @brief 把 URL 解析成一个确定的文件（**不处理索引文件**，也不做 SPA 回退）。
 *
 * 「不处理索引」是刻意的：索引只对目录有意义，把它留在上层，递归深度就天然
 * 封顶在 1 —— 不用去证明「index.html 指向一个目录」这种情况不会转圈。
 *
 * 第 (2) 步是**唯一**有快慢两档的地方，其余三步逐字不变。快路径的来历：
 * `realpath()` 是按分量走的 —— 一个 5 段路径就是 5 次 `lstat`，而且它们是
 * glibc 内部调用（`__lstat64`），既过不了 LD_PRELOAD 也进不了 profiling 的
 * 符号表，只能看着它在 `stat` 之外凭空多出一倍的 VFS 往返。本机量具上静态腿
 * 每请求 `realpath` 2.73 次、`statx` 2.25 次（`/tmp/c1/count.sh`）—— 前者纯粹
 * 是在重复回答一个**昨天就已经回答过、且答案没变**的问题。
 *
 * 快路径凭什么敢跳过安全边界：`stat()` 与 `realpath()` 走的是**同一套 VFS
 * 解析**（都逐分量跟符号链接）。所以「`stat(fs_path)` 还拿回同一对
 * `(dev, ino)`」这句话的意思就是「`realpath(root + url)` 今天仍然解到同一个
 * 文件」，而那个文件在上一次解析时**已经**通过了根内包含判断。inode 相同 ⇒
 * 内容一字不差 ⇒ 那次判断继续成立。
 *
 * `hints` 空、或里面没有这个 URL、或指纹对不上，一律落回完整的
 * `web_resolve_within_root()` —— **回退是默认行为，快路径是例外**。
 * 于是「文件被换掉」（`mv -f`、删除后重建、甚至是同尺寸的替换）必然因为
 * inode 变化而回退，行为与从前逐字相同。
 *
 * 已知的**唯一**一处行为差异：文件被移出根、原位置留一个指回去的符号链接时，
 * inode 没变，快路径会继续供这份文件，而完整解析会判 `TRAVERSAL` ⇒ 403。
 * 供出去的字节仍然只是那次已经授权过的那一份，所以不是越权读取，是「管理员
 * 移走它」这个意图没被立刻执行 —— 删掉（而不是移走）就会立刻生效。详见
 * `doc/web-http-guide.md`。
 */
probe_status probe_leaf(uvcpp_loop* loop, const static_config& cfg,
                        const std::string& url, probe_result& out,
                        const hint_vec* hints = nullptr,
                        hint_vec* record = nullptr) {
  out = probe_result();
  out.final_url = url;

  // (1) 符号链接策略。查的是**未解析**的路径，所以这里自己拼一次 ——
  //     `web_resolve_within_root()` 返回的是展开后的真实路径，那时候再判断
  //     「是不是链接」已经没有意义了。
  //
  //     **快路径不跳过这一步**：它判的是"这次请求要走的那个 URL 上有没有
  //     链接"，与"上次解析到哪儿"是两件事。少跳过一步，就少一类需要在
  //     "链接被新加上去"时才能发现的差异。
  if (!cfg.follow_symlinks) {
    const std::string joined = web_join_root(cfg.root_real, url);
    if (lstat_is_symlink(loop, joined)) {
      out.status = probe_status::FORBIDDEN;
      out.policy_rejected = true;
      return out.status;
    }
  }

  std::string real;

  // (2a) 快路径 —— 只在记忆命中时走。**一次系统调用，零次 `realpath`。**
  //
  //      正数记忆：stat 的对象是**上次解析出来的那个路径**，不是
  //      `root + url` —— 只有前者才与"上次的结论"可比。第 (1) 步已经保证
  //      了策略面的事，包含面的事由指纹保证。
  //      负数记忆：反过来查**没解析过**的那条路径（`root + url`），
  //      "它现在还是不在"就是"上次的失败今天照样成立"。
  const resolve_hint* h = find_hint(cfg.resolve_memo ? hints : nullptr, url);
  if (h != nullptr) {
    if (h->exists) {
      uint64_t dev = 0;
      uint64_t ino = 0;
      if (stat_real(loop, h->fs_path, out.is_dir, out.mtime_sec, out.mtime_nsec,
                    out.size, &dev, &ino) &&
          dev == h->dev && ino == h->ino) {
        out.fs_path = h->fs_path;
        out.status = probe_status::OK;
        return out.status;
      }
    } else if (!lstat_exists(loop, web_join_root(cfg.root_real, url))) {
      // `lstat` 失败 ⇒ 这条路径上确实没有东西 ⇒ 完整解析也只会得出
      // NOT_FOUND（`realpath` 会在同一个分量上以同一个 errno 失败）。
      out.fs_path.clear();
      out.status = probe_status::NOT_FOUND;
      return out.status;
    }
    // 对不上（换文件了 / 路径没了 / 变成别的类型了 / 那个东西又出现了）：
    // 落下去做完整解析。**先把手里的 `out` 清干净** —— 上面那次 `stat_real`
    // 可能已经往 `out.is_dir/mtime/size` 里写过值了。
    out = probe_result();
    out.final_url = url;
  }

  // (2b) 真实路径的包含判断 —— 这才是安全边界，第 (1) 步不是。
  //      关掉 `resolve_memo` 时**连记账也不做**：不读的人不该付写的钱，
  //      而且"关掉"这一档的判据正是"系统调用序列与不做记忆时逐字节相同"。
  hint_vec* const rec = cfg.resolve_memo ? record : nullptr;
  const web_path_status ws = web_resolve_within_root(cfg.root_real, url, real);
  if (ws != web_path_status::OK) {
    const bool traversal = (ws == web_path_status::TRAVERSAL);
    out.status = traversal ? probe_status::FORBIDDEN : probe_status::NOT_FOUND;
    out.policy_rejected = traversal;
    // `TRAVERSAL` 不记负数记忆：它的校验需要重做包含判断，而"路径被换向"
    // 恰恰是记忆最容易过期的那一类，留它走完整解析更划算。
    if (!traversal) record_miss_hint(rec, url);
    return out.status;
  }

  // (3) stat。`dev`/`ino` 与别的字段取自**同一次** stat，所以记下去的
  //     指纹与记下去的 mtime/size 描述的是同一个瞬间的文件。
  uint64_t dev = 0;
  uint64_t ino = 0;
  if (!stat_real(loop, real, out.is_dir, out.mtime_sec, out.mtime_nsec,
                 out.size, &dev, &ino)) {
    out.status = probe_status::NOT_FOUND;
    // 落在这里的常见情形是"`realpath` 与 `stat` 之间有东西删掉了它"，
    // 也可能解析到的是 FIFO/设备这类不被接受的类型。两种都按"现在没有"
    // 记，下次由 `lstat_exists()` 复核 —— 那时东西要是又在了，自然落回
    // 完整解析。
    record_miss_hint(rec, url);
    return out.status;
  }

  out.fs_path = real;
  out.status = probe_status::OK;
  record_hint(rec, url, real, dev, ino);
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
                              const std::string& url, probe_result& out,
                              const hint_vec* hints = nullptr,
                              hint_vec* record = nullptr) {
  const probe_status s = probe_leaf(loop, cfg, url, out, hints, record);
  if (s != probe_status::OK || !out.is_dir) return s;

  for (size_t i = 0; i < cfg.index_names.size(); ++i) {
    probe_result sub;
    const std::string idx_url = join_url(url, cfg.index_names[i]);
    const probe_status ss = probe_leaf(loop, cfg, idx_url, sub, hints, record);
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
     * 框架判断「这一环会不会异步恢复」的办法是量 `next` 副本里的上下文引用数
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
     * @brief 解析记忆的**输入**：循环线程在取探针的那一次锁里一起拷出来的。
     *
     * 与 `probes` 同一个时机、同一把锁、同一份候选键集合 —— 但**互不依赖**：
     * `probes` 只在缓存里**有**这个键时才有一条，而记忆可以在"没缓存但解析过"
     * 时也存在（走分片下发的大文件就是这种）。所以它是独立的一份列表，
     * 不是 `cache_probe` 的一个字段。
     */
    hint_vec hints;

    /**
     * @brief 解析记忆的**输出**：worker 这次完整解析出来的结论，攒着一次性写回。
     *
     * 攒起来是为了**只取一次锁**：`probe_leaf()` 一次请求最多被调 6 次
     * （索引名 × 孪生后缀），每次都自己取锁就把 miss 路径的锁次数乘了六倍。
     * worker 一次请求至多解析成功 3 条（原文件 + 两个孪生），所以这里通常
     * 只有 1~2 条。
     */
    hint_vec fresh_hints;

    /**
     * @brief 客户端**可接受**的预压缩后缀，按 q 降序，至多两个；空串 = 没这一位。
     *
     * 在 `serve()`（循环线程）里按 `Accept-Encoding` 定下来，之后不再变。
     * worker 按序探 `final_url + try_variants[i]`，**取第一个存在的**。
     */
    std::string try_variants[2];

    /**
     * @brief **已经 stat 通过**并被采用的那一个后缀，空 = 用原文件。
     *
     * 它与 `try_variants[0]` 不一定相同：首选缺席（比如盘上只有 `.gz`）时取的是
     * 后面的候选。**全都没探到就自己清空**（见 `run_job`），所以 worker 结束时
     * 它为空就等价于"这次没用孪生文件"。
     */
    std::string variant;

    /**
     * @brief 本 job 当前**持有一个工作池名额**吗。
     *
     * 名额从 `serve()` 搬进 `run_job()` 之后，"拿过"不再是一条必走的路
     * （命中缓存、404 都不拿），所以归还侧必须有个凭据 —— 否则
     * `release()` 会把别人的名额还掉，闸门自己就成了漏洞。
     *
     * **名额在 `run_job()` 末尾就还掉了**（worker 上，走
     * `release_deferred()`），所以这个标志在 `after_work` 里**恒为 false** ——
     * 它只在 worker 那一段活着。留着它是因为"拿过没有"这件事本身要在 worker
     * 上判两次（读盘前取、读完后还）。
     */
    bool slot_held;

    /**
     * @brief 名额是在 worker 上还的，而那次还名额**欠一次唤醒补派**。
     *
     * `release_deferred()` 只记账、不派发（唤醒回调必须跑在 loop 线程上），
     * 于是由本 job 自己的 `after_work` —— 一个保证会跑的 loop 线程回调 —— 来
     * 补这一刀 `dispatch_wakeups()`。见 `uvcpp_web_work_limit::release_deferred()`。
     */
    bool slot_deferred;

    /**
     * @brief **循环线程已经把 `probe_job()` 整个做完了** —— worker 只许读盘。
     *
     * 只在 `revalidate == LOOP_STAT` 时为真（见 `serve()`）。worker 看到它就跳过
     * `probe_job()`，于是**不会有第二次 `stat`**：这次请求的磁盘身份是被循环
     * 线程那条线程看到的，worker 只负责把已经认定的那个文件读进内存。
     */
    bool probed_on_loop;

    // worker 产出
    probe_status status;
    bool policy_rejected;
    bool served_from_cache;
    std::string final_url;
    std::string fs_path;  ///< 只用于取 MIME 与日志，不再参与路径判断
    /**
     * @brief 取 MIME 用的路径 —— **永远是原文件**那一份。
     *
     * 用了孪生文件时 `fs_path` 会变成 `app.js.br`，而 `.br` 在后缀表里只能
     * 查成 `application/octet-stream`。类型要按**原文件名**报，压缩只是同一份
     * 表示的另一种编码（靠 `content-encoding` 声明），不是另一种类型。
     */
    std::string mime_path;
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
          slot_deferred(false),
          probed_on_loop(false),
          status(probe_status::NOT_FOUND),
          policy_rejected(false),
          served_from_cache(false),
          mtime_sec(0),
          mtime_nsec(0),
          size(0) {}

    /**
     * @brief 放回回收站之前把自己清干净 —— **容量留着，内容丢掉**。
     *
     * 这是 `job` 走回收站的**全部要点**：`url` / `final_url` / `fs_path` /
     * `mime_path` / `probes` / `hints` / `fresh_hints` 在下一次请求里会被
     * `operator=` / `push_back()` 复用，只要容量够就**不再分配**。所以这里
     * 必须是 `clear()`，不能是 `job().swap(*this)` 或 `shrink_to_fit()` ——
     * 那两种会把容量还回去，等于每请求照旧分配。
     *
     * 松引用的时刻与 `~job` **逐条一致**：`next`（那份让链挂起的上下文引用）
     * 与 `data`（响应体）都在这里松开，而且调用点也就是原来 `delete j` 的
     * 那三处。这一点是必须对齐的 —— 这两份引用按着的是上下文与响应体，
     * "晚一拍松"就会把已经结束的连接资源多拖一会儿。
     */
    void reset() {
      self = nullptr;
      loop = nullptr;
      req = nullptr;
      resp = nullptr;
      next = uvcpp_web_next();
      url.clear();
      is_head = false;
      probes.clear();
      hints.clear();
      fresh_hints.clear();
      try_variants[0].clear();
      try_variants[1].clear();
      variant.clear();
      slot_held = false;
      slot_deferred = false;
      probed_on_loop = false;
      status = probe_status::NOT_FOUND;
      policy_rejected = false;
      served_from_cache = false;
      final_url.clear();
      fs_path.clear();
      mime_path.clear();
      mtime_sec = 0;
      mtime_nsec = 0;
      size = 0;
      data.reset();
      cfg.reset();
    }
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
   * @brief 护 `lru`/`cache`/`bytes`，外加 `retired` 那张**表本身**（不含
   *        表里的指针指向的东西，也不含桶里的 `vector`）。
   *
   * 为什么要锁：多循环下 `serve()`（读缓存）与 `finish_job()`（写缓存）分别
   * 跑在**不同的循环线程**上，而这两个成员从前是按"loop 线程只有一条"写的。
   *
   * **锁，不是分片。** 分片会让每片各有一份 LRU：内存成倍、命中率还掉
   * （同一条路径的请求落到不同片上就互相看不见）。这正是 `compress_mu_`
   * 那条先例里的取舍（`src/web/uvcpp_http_server.h:1098-1110`：「这里要的是
   * 互斥，不是分片」）。单循环下这把锁没有可争用的对象，代价是一次未争用的
   * `lock`/`unlock`。
   *
   * **唯一的例外是解析记忆**（`retire_bucket::memo`）：每条循环各存一份答案
   * 副本，热路径上一次锁都不取。它与上面那段不冲突，因为两者不是一回事 ——
   * 那是**纯记忆**的副本（丢了只多走一次 `realpath`，不参与任何判定），而
   * `cache`/`lru` 分片会同时改掉内存、命中率与淘汰次序。理由见那个成员的
   * 注释。
   *
   * **它离热路径又远了一步**（每次取锁在 `-m 32` 那档下都是一次潜在的
   * futex，每请求 8 次的账就是这么来的）：三个计数改成了 `std::atomic`
   * （它们只被加减，从不参与决策）；循环线程取探针整段只取**一次**锁
   * （`collect_probes()`）；回收桶空了就一次锁都不取（`bucket_for()`）；
   * `LOOP_STAT` 档的解析记忆走前台副本（`memo_lookup()`）。落到每请求上的
   * 取锁次数因此是 1 —— `POOL` 档是 `collect_probes()` 那一次，`LOOP_STAT`
   * 档是 `lookup_cache()` 那一次（缓存比对本身仍要查全局表，见它的注释）。
   *
   * `mutable`：`collect_probes()`/只读访问器都是 const 成员函数。
   */
  mutable std::mutex mu_;

  /** @brief 被 503 挡掉的请求数（观测用；跨线程，无锁读改写）。 */
  std::atomic<unsigned long long> rejected;

  std::list<std::string> lru;  ///< 队首 = 最近使用（受 `mu_` 护）
  std::map<std::string, cache_entry> cache;  ///< 受 `mu_` 护
  size_t bytes;                 ///< 受 `mu_` 护

  /**
   * @brief 「这个被探过的 URL 上次解析到哪个文件」—— 见 `probe_leaf()` 的快路径。
   *
   * **受 `mu_` 护**，与 `cache` 并列但独立：条目缓存存的是**字节**，这份存的是
   * **解析结论**，两者的键空间与失效条件都不同（字节按 mtime/size 失效，结论
   * 按 inode 失效）。`cache_max_entries == 0`（关缓存）时这份表也不建 —— 它
   * 的上限就是借 `cache_max_entries` 定的。
   *
   * 它**不参与任何正确性判定**：丢掉/清空只会让下一次多走一次
   * `web_resolve_within_root()`，响应逐字节不变。所以它可以在任何时刻被
   * 整表清掉（`put_hints()` 满了就是这么干的），`clear_cache()` 也顺手清它。
   *
   * **`LOOP_STAT` 档的热路径不读这张表**：那条路上每条循环有一份前台副本
   * （`retire_bucket::memo`，零取锁），只有冷启动、以及前台那份被作废/没答上来
   * 的那一次请求才会落到这里。这张表在多循环下仍然是**唯一**权威的那份 ——
   * 前台是从它这儿拷出去、也随时可以丢掉的。
   */
  std::map<std::string, resolve_hint> hints_;

  /** @brief 命中/未命中（观测用；跨线程，无锁读改写）。 */
  std::atomic<unsigned long long> hits;
  std::atomic<unsigned long long> misses;

  /**
   * @brief 前台副本的**代次**：`clear_cache()` 每喊一次加一。
   *
   * 每条循环那份前台副本（`retire_bucket::memo` 与 `retire_bucket::body`）
   * 里的条目都记着自己是哪一代的。对不上就**当作没有**（不读也不删，等下一次
   * 回写就地换掉）。
   *
   * 为什么 `clear_cache()` 必须让它们作废：那份函数的调用方（换语料、跑合规
   * 探针之前）要的是一条**可以断言的性质** —— "清缓存之后第一次请求必定走
   * 完整解析"（全局表那边是 `hints_.clear()` 给的，内容那边是 `cache.clear()`
   * 给的）。前台副本如果活得比它们长，这条断言就不成立了：请求会拿一份
   * "本该被忘掉"的记忆或字节直接命中。
   *
   * **容量性的清空不算**（`put_hints()` 满了那一下）：那是全局表自己的容量
   * 策略，不是"结论变了"，而前台那份仍然是**有效的记忆**（它每次被用都要
   * 当场校验，见 `memo_lookup()`）。
   *
   * `acquire`/`release` 配对：发布方在锁内 `fetch_add(release)`，读方在
   * `load(acquire)` —— 要的是"看见新代次"这件事本身，而不是别的字段的可见性。
   */
  std::atomic<unsigned long long> front_gen_;

  /**
   * @brief 前台解析记忆的一条**备选答案**：一份 `hint_vec` + 它是在哪对后缀下取的。
   *
   * 为什么要带后缀：`collect_hints()` 的候选键是
   * `(URL | URL+索引名 | SPA) × ("", v0, v1)` 展开出来的，而 `variants` 来自
   * **每个请求自己的** `Accept-Encoding`（带 `Range` 的请求还会把这一对整对
   * 清空，见 `serve()`）。所以同一个 URL 在不同请求上答案不同 —— 一张
   * `URL → 答案` 的表必须把"这份答案是哪对后缀下的"一起记住，否则一个混着
   * 两种 `Accept-Encoding` 的客户端就会拿到张冠李戴的记忆。
   *
   * 表里**至多两条**（`k_alt_max`）：真实的取值空间就是这个量级（带后缀的
   * 一对，以及不带后缀的空对）。再多就说明"这个 URL 的答案一直在变"，那时
   * 每次都落回全局表 —— 也就是不做前台时的行为，没有正确性代价。
   */
  struct memo_alt {
    std::string v0;
    std::string v1;
    hint_vec hints;
  };
  static const size_t k_alt_max = 2;

  /// 前台记忆里的一条：`gen` 那一代下，某个 `job::url` 的答案（队首 = 最近用过）。
  struct memo_entry {
    unsigned long long gen;
    std::vector<memo_alt> alts;
    memo_entry() : gen(0) {}
  };

  /**
   * @brief 前台**内容**副本里的一条：对全局条目那份字节的**弱**引用 + 它当时的身份。
   *
   * 与 `cache_entry` 的差别只有这一处 —— **弱引用**。这条副本不按着字节，它
   * 只是"全局表里那份还在不在"的一个旁证：全局表把它淘汰掉（LRU/超预算），
   * 这里当场就 `expired()`，落回全局那条路重新读盘。
   *
   * 那正是它与"分片"的分野，也是上面 `mu_` 那段"锁，不是分片"仍然成立的原因：
   * **分片各存一份字节（内存成倍、命中率还掉），这里连一个字节都不存** ——
   * 每条循环多的只是几个键、三个整数和一个控制块的弱计数，`bytes` 的账一个
   * 字节都不动。内容只存一份、立刻都还在全局表里。
   *
   * 身份的三个字段与全局表判命中用的**完全相同**（大小、mtime 秒、mtime 纳秒），
   * 所以"这份字节还算不算数"在两条路上仍然是同一个问题、同一个答案。
   */
  struct body_entry {
    unsigned long long gen;
    int64_t size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    std::weak_ptr<std::string> data;
    body_entry() : gen(0), size(0), mtime_sec(0), mtime_nsec(0) {}
  };

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
   * **表本身**（插入新桶）受 `mu_` 护 —— 两条循环同时 `retired[l]` 就是往
   * 同一个 `std::map` 上并发写。桶里的 `delete` 在锁外做。
   *
   * **桶一旦建出来就再不 `erase()`**（旧写法每轮 `serve()` 都删一遍键）。
   * 于是 `std::map` 节点的地址恒定（重平衡只改指针、不搬节点），
   * `bucket_for()` 就能把指针缓存在本线程上，之后这条循环上的
   * `retire()`/`drain_retired()` **一次锁都不取**。桶的 `vector` 也因此只被
   * 一条线程碰（见 `retire_bucket`）。
   *
   * 数量级：两次请求之间完成的活儿，通常就是 1 个。
   */
  struct retire_bucket {
    /**
     * @brief 待删的 work 请求。
     *
     * **只被那一条循环自己的线程碰**，所以它不需要 `mu_`：`retire()` 在
     * `uvcpp_work` 的 after_work 回调里调（libuv 保证那个回调跑在**投递它的
     * 那条循环**上），`drain_retired()` 在 `serve()` 开头调（同一条循环的
     * 线程）。这正是分桶的第二个好处 —— 第一个是"只删自己投出去的"。
     */
    std::vector<uvcpp_work*> pending;

    /**
     * @brief **回收站**：已经清干净、留着复用的对象。
     *
     * 与 `pending` 分开是因为两者语义不同：`pending` 是"刚用完、还没处理"，
     * 回收站里的是"清干净了、随时能发出去"。`take_job()` 从这里取，取不到
     * 才 `new`；`release_job()` 往这里放。
     *
     * **这里只有 `job`，没有 `uvcpp_work`** —— 后者这一版做不了，理由是硬的，
     * 写在这里免得下次又有人试：`uvcpp_work::queue_work()` 在 `loop != nullptr`
     * 时**抛异常**（`src/req/uvcpp_work.cpp` 的 `"error,local loop had value"`），
     * 而那个 `loop` 是私有成员、缓存之后没有任何公开路径能清掉。复用一个用过
     * 的 `uvcpp_work` 就会在 `queue_work()` 上抛出去（实测：静态用例整组 500）。
     * 要做就得先给那个核心类加一条"可以再次 `queue_work()`"的契约 —— 那是另一
     * 处改动、另一套判据，不塞进这一版。
     *
     * **有上限**（`k_free_cap`）：稳态下回收站的深度就是这条循环上的在途峰值，
     * 峰值本身不会因为回收站而变大；但没有上限的话，一次尖峰之后那份深度会被
     * 一直留着。超上限的这一批照旧 `delete` —— 退化成今天的行为，不是泄漏。
     */
    std::vector<job*> free_jobs;

    /**
     * @brief 这条循环自己的**前台解析记忆**：`job::url` → 那次的答案。
     *
     * 与全局那张 `hints_` 是**同一份信息的两处存放**，取舍完全不同：全局那份
     * 每读一次要取一次 `mu_`（`LOOP_STAT` 档下那是每请求的第二次取锁），
     * 这份一次锁都不取，代价是每条循环各存一份。
     *
     * **只被那一条循环自己的线程碰** —— 与 `pending`/`free_jobs` 同一条约束，
     * 而且比它们更严：`LOOP_STAT` 档下 `collect_hints()`（读）与
     * `probe_job()` 的回写（写）都在循环线程上，`POOL` 档则**一次都不碰**
     * （见 `memo_store()` 的入口条件）。所以它不需要锁，也不需要分片。
     *
     * **上限借用 `cache_max_entries`** —— 与 `put_hints()` 同一把尺子、
     * 同一个理由（配置一个有界的东西不该需要第二个旋钮）：满了整表清掉，
     * 丢掉任何一条都只意味着"下次多走一次 `realpath`"。
     */
    std::map<std::string, memo_entry> memo;

    /**
     * @brief 这条循环自己的**前台内容副本**：`job::final_url` → 那份字节的弱引用。
     *
     * 与 `memo` 同一条约束（只被这条循环自己的线程碰，`LOOP_STAT` 档专有，
     * 见 `body_store()`），也是同一个目的：`LOOP_STAT` 档的缓存比对
     * （`lookup_cache()`）每请求要取一次 `mu_`，而实测那一次持有所花的
     * 544~627 ns 里大头同样是取锁本身，后面还跟着 1.3~2.4 µs 的**等锁**。
     * 这一份把那笔账也整段去掉。
     *
     * **它不存字节**（弱引用，见 `body_entry`），所以它与 `cache` 不构成
     * 两份缓存：这里只是"那条路不必再问全局表"的一个抄本。
     *
     * 上限同样借 `cache_max_entries`（与 `put_hints()` 同一把尺子）。
     */
    std::map<std::string, body_entry> body;
  };

  /// 每条循环的回收站上限（个）。见 `retire_bucket::free_jobs`。
  static const size_t k_free_cap = 512;

  std::map<uvcpp_loop*, retire_bucket> retired;

  /**
   * @brief 本线程上 `(this, 循环)` 对应的桶的缓存，见 `bucket_for()`。
   *
   * `ticket` 是防"`Impl` 析构后在同一地址又建了一个、而循环指针也复用了
   * 同一个值"的：票号单调递增、永不复用，所以过期的缓存一定对不上。
   *
   * **这个类型必须一直保持"平凡析构"（纯 POD）**：MinGW-w64 下带非平凡析构的
   * `thread_local` 对象走 `__cxa_thread_atexit`，而 emutls 在那之前就把 TLS 块
   * 还回堆了 —— 析构读到的是已复用的内存（那个坑的完整现场见
   * `src/expand/uvcpp_page_heap.cpp:556-574`）。只放指针和整数就没有这个问题：
   * 编译器根本不登记析构回调。
   */
  struct bucket_cache {
    const Impl* owner;
    unsigned long long ticket;
    uvcpp_loop* loop;
    retire_bucket* bucket;
  };

  /** @brief 本实例的票号。见 `bucket_cache`。 */
  unsigned long long instance_id_;

  static unsigned long long next_instance_id() {
    static std::atomic<unsigned long long> n(0);
    return n.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  /**
   * @brief 取 \p l 的回收桶；没有就建一个。**只在没缓存时才取一次锁。**
   *
   * 为什么可以免锁：桶建出来之后地址就不动了（`std::map` 的节点地址恒定，
   * 而且这张表再没有 `erase()`），而桶里的东西只有这条循环的线程会碰。缓存
   * 拿 `(this, ticket, loop)` 三重校验，所以"另一个 `Impl` 的桶""另一条循环
   * 的桶"都命中不了。
   */
  retire_bucket* bucket_for(uvcpp_loop* l) {
    static UVCPP_THREAD_LOCAL bucket_cache c = {nullptr, 0, nullptr, nullptr};
    if (c.owner == this && c.ticket == instance_id_ && c.loop == l) {
      return c.bucket;
    }
    std::lock_guard<std::mutex> lk(mu_);
    retire_bucket& b = retired[l];
    c.owner = this;
    c.ticket = instance_id_;
    c.loop = l;
    c.bucket = &b;
    return &b;
  }

  Impl()
      : rejected(0),
        bytes(0),
        hits(0),
        misses(0),
        front_gen_(0),
        instance_id_(next_instance_id()) {}

  ~Impl() { drain_all_retired(); }

  /// 记下一个用完的 work 请求（**只能在 after_work 回调里调**，传**投递它的
  /// 那条循环**）。
  void retire(uvcpp_loop* l, uvcpp_work* w) {
    bucket_for(l)->pending.push_back(w);
  }

  /// 真正释放 \p l 那一条循环攒下的 work。**绝不能在任何回调里调。**
  ///
  /// `uvcpp_work` 这一版**不复用**（理由见 `retire_bucket`），所以这里仍然是
  /// "删"，只是 `job` 那一份不再走这条路。
  void drain_retired(uvcpp_loop* l) {
    retire_bucket* b = bucket_for(l);
    if (b->pending.empty()) return;  // 常态：连锁带 map 查找全省掉
    std::vector<uvcpp_work*> mine;
    mine.swap(b->pending);
    // 锁外删：`~uvcpp_work` 里会发生什么不该由这把锁兜着。
    for (size_t i = 0; i < mine.size(); ++i) delete mine[i];
  }

  /// 取一个 `job`：回收站里有就复用，没有才 `new`。返回的一定是**干净**的
  /// （进回收站之前 `reset()` 过），所以调用方看到的与 `new job()` 逐字段相同。
  job* take_job(uvcpp_loop* l) {
    retire_bucket* b = bucket_for(l);
    if (b->free_jobs.empty()) return new job();
    job* j = b->free_jobs.back();
    b->free_jobs.pop_back();
    return j;
  }

  /**
   * @brief 归还一个 `job`：就地 `reset()` 再放进回收站。
   *
   * **就地**是刻意的，不是图省事：`reset()` 会松开这份 `job` 按着的 `next`
   * （上下文引用）与 `data`（响应体），而调用它的三处正是原来 `delete j` 的
   * 那三处。清理时机一个字节都没变，只是内存不再还给堆。
   *
   * 回调里（`uvcpp_work` 的 after_work）调用也安全 —— 现状就是在那里
   * `delete j`。**与 `retire()` 那条约束的区别**：那边不能删是因为被删的
   * `w` 自己身上存着**正在执行的闭包**；`job` 是另一个对象，身上没有闭包。
   */
  void release_job(uvcpp_loop* l, job* j) {
    retire_bucket* b = bucket_for(l);
    j->reset();
    if (b->free_jobs.size() < k_free_cap) {
      b->free_jobs.push_back(j);
    } else {
      delete j;
    }
  }

  /**
   * @brief 析构用：所有桶一起排空。
   *
   * **前提是"已经没有别的线程会往里放了"**（App 先停、循环先退出），旧写法
   * 也是同一个前提 —— 它那把锁护住的只是这张表，`erase()` 掉一个桶之后
   * 再有 `retire()` 进来就是往一个没人看的桶里塞（泄漏，不是崩溃）。这里
   * 连表一起清掉，语义不变。
   */
  void drain_all_retired() {
    std::map<uvcpp_loop*, retire_bucket> all;
    {
      std::lock_guard<std::mutex> lk(mu_);
      all.swap(retired);
    }
    for (std::map<uvcpp_loop*, retire_bucket>::iterator it = all.begin();
         it != all.end(); ++it) {
      // 两份都要：**刚用完还没处理的** work，以及回收站里的 `job`。
      retire_bucket& b = it->second;
      for (size_t i = 0; i < b.pending.size(); ++i) delete b.pending[i];
      for (size_t i = 0; i < b.free_jobs.size(); ++i) delete b.free_jobs[i];
    }
  }

  bool cache_enabled() const { return opts.cache_max_entries > 0; }

  /// 校验（以及一切 `stat`）是否搬到了循环线程上。见 `uvcpp_web_revalidate`。
  bool loop_revalidate() const {
    return opts.revalidate == uvcpp_web_revalidate::LOOP_STAT;
  }

  const uvcpp_web_mime_map& mime_map() const {
    return opts.mime != nullptr ? *opts.mime : uvcpp_web_mime_map::default_map();
  }

  /**
   * @brief 取 Content-Type。
   *
   * 返回值那一版对**每个文本类型**至少分配两次：`std::string ct = t` 一次、
   * `ct += "; charset=utf-8"` 再一次（`ct` 的容量正好等于长度，追加重分配）。
   * 而调用方下一句就是 `set_content_type()` —— 真正需要的那一份是**头表持有
   * 的那份**，中间这份纯白付。
   *
   * 改成一次：要补后缀的那条路在栈上拼好再构造一次；不补的那条路直接返回
   * 类型表里的静态串（短于 SSO 上界的类型连构造都不落堆）。
   *
   * `strstr` 而不是 `find`：`find` 会为这个 `const char*` 造一个临时
   * `std::string`，正好是这里要省掉的那一次。
   */
  std::string content_type_for(const std::string& fs_path) const {
    const char* t = mime_map().lookup(fs_path);
    const char* base = (t != nullptr) ? t : "application/octet-stream";
    if (!opts.add_charset) return std::string(base);
    if (std::strstr(base, "charset") != nullptr) return std::string(base);
    if (!uvcpp_web_mime_map::is_text(base)) return std::string(base);
    // `; charset=utf-8` 是 15 字节；64 够装下任何一个真实的类型名加它。
    char buf[64];
    const size_t bl = std::strlen(base);
    if (bl + 15 >= sizeof(buf)) return std::string(base);  // 兜底：不补
    std::memcpy(buf, base, bl);
    std::memcpy(buf + bl, "; charset=utf-8", 15);
    return std::string(buf, bl + 15);
  }

  // ---- LRU（共享一份，受 mu_ 护；`evict()` 例外，见它自己的注释）----
  /**
   * @brief 给每个候选键**各探一次**：原文件，以及每一个可接受后缀的孪生键。
   *
   * 为什么要两个都探：循环线程不知道盘上有没有孪生文件 —— 那正是 worker
   * 要去 stat 的事。多探一条只是多一次内存里的 map 查找；少探一条则会让
   * "孪生文件已缓存"的那次命中退化成一次读盘。
   *
   * 键就是**拼好后缀的 URL 本身**（`/static/app.js.br`），跟 `put()` 写进去的
   * 那一份对得上，也天然把 `.br` / `.gz` / 原文件分成三条互不覆盖的条目。
   */
  void collect_probes(const std::string& url, const std::string variants[2],
                      std::vector<cache_probe>& out,
                      hint_vec* hints = nullptr) const {
    // **整段只取一次锁。** 从前是每个候选键各取一次（1 个索引名 + 2 个后缀
    // ⇒ 条目用的那种配置下每请求最多 6 次），而 `-m 32` 那种形状下这把锁是
    // 真有争用的 —— 每取一次都是一次潜在的 futex。候选键的**全集**不依赖锁
    // 里的任何状态（拼的是 URL 与配置），所以把整段查找包进来不改变语义：
    // 每个条目仍然是在锁内拷出来的快照，worker 照旧拿它去和磁盘上的
    // mtime/size 比对。
    //
    // 解析记忆（`hints`）**搭这同一次锁的车**：它的候选键与探针完全同一批，
    // 而且它本来就是受 `mu_` 护的。于是 C2 的快路径对 POOL 档是**零新增取锁**
    // —— 这是它没有变成"省下 3 µs 的 realpath、付掉 20 ns 的锁"那种半吊子
    // 优化的原因。
    std::lock_guard<std::mutex> lk(mu_);
    add_probe_locked(url, variants, out, hints);
    for (size_t i = 0; i < cfg->index_names.size(); ++i) {
      add_probe_locked(join_url(url, cfg->index_names[i]), variants, out, hints);
    }
    if (cfg->spa_fallback && !cfg->spa_url.empty()) {
      add_probe_locked(cfg->spa_url, variants, out, hints);
    }
  }

  /**
   * @brief `LOOP_STAT` 档专用的取记忆：那条路上 `serve()` 不取探针（用不上），
   *        所以记忆得自己取。
   *
   * **先问本循环那份前台副本**（`memo_lookup()`），命中就一次锁都不取；没命中
   * 才落到全局表上取**一次**锁，并把答案顺手拷进前台（`memo_store()`）。
   *
   * \p l 是这条循环 —— 只有它能把前台表取出来（`bucket_for()`）。从前这个
   * 函数没有这个参数，因为那时它只需要全局表。
   *
   * **不是 `const` 了**：前台表是懒建的（第一次 `bucket_for()` 要建桶）。
   */
  void collect_hints(uvcpp_loop* l, const std::string& url,
                     const std::string variants[2], hint_vec& out) {
    // 关掉记忆时连取都不取：`probe_leaf()` 那边本来也不会用，白拿一次锁
    // 会让"关掉之后一次多余的系统调用/加锁都没有"这条判据不成立。
    if (!cfg->resolve_memo) return;
    if (memo_lookup(l, url, variants, out)) return;
    std::lock_guard<std::mutex> lk(mu_);
    add_hints_locked(url, variants, &out);
    for (size_t i = 0; i < cfg->index_names.size(); ++i) {
      add_hints_locked(join_url(url, cfg->index_names[i]), variants, &out);
    }
    if (cfg->spa_fallback && !cfg->spa_url.empty()) {
      add_hints_locked(cfg->spa_url, variants, &out);
    }
  }

  /**
   * @brief 前台解析记忆的**读**侧：这条循环自己那份 `URL → 答案`，**零取锁**。
   *
   * 为什么可以有一份副本而不改变任何判定：全局那张 `hints_` 是**纯记忆** ——
   * 它自己不参与正确性（丢了只多走一次 `realpath`，见 `hints_` 的注释），
   * 而每一条记忆在被用的时候都要**当场校验**（`probe_leaf()` 的 (2a)：正数
   * 比对 `dev`/`ino`，负数再 `lstat` 一次）。所以一份副本即使过期，也只会在
   * 那一步落回完整解析，再由 `fresh_hints` 改成新的 —— **前台表因此与它所
   * 复制的那张表站在同一条船上**：不参与任何判定，只决定"要不要省下这一次
   * `realpath`"。
   *
   * 为什么只在 `LOOP_STAT` 档有：这张表没有锁，它成立的前提是"只有这条循环
   * 自己的线程碰它"。`LOOP_STAT` 档下读（本函数所在的 `collect_hints()`）与
   * 写（`probe_job()` 的回写）都在循环线程上 —— 这是那条路把 `stat` 搬到
   * 循环线程换来的副产品。`POOL` 档的回写在 worker 上，那时这张表**一次都
   * 不碰**（`probe_job()` 里由 `probed_on_loop` 分岔），行为与不做前台时
   * 逐字节相同。
   *
   * 为什么值得：`LOOP_STAT` 档的读侧从前每请求取一次 `mu_`，而实测那一次
   * 的持有时长里**大部分是取锁本身**（争用下的 `futex` 唤醒与持锁线程被
   * 抢占），不是临界区里的那点活。前台表把这笔账整段去掉：热路径上一把
   * 共享的锁都不碰，也就没有可争用的缓存行。
   */
  bool memo_lookup(uvcpp_loop* l, const std::string& url,
                   const std::string variants[2], hint_vec& out) {
    if (!cache_enabled()) return false;  // 全局表那条路也不会给答案
    std::map<std::string, memo_entry>& tab = bucket_for(l)->memo;
    const std::map<std::string, memo_entry>::iterator it = tab.find(url);
    if (it == tab.end()) return false;
    if (it->second.gen != front_gen_.load(std::memory_order_acquire)) {
      return false;  // 上一代（`clear_cache()` 之后）：不读，也不在这里删
    }
    for (size_t i = 0; i < it->second.alts.size(); ++i) {
      if (it->second.alts[i].v0 != variants[0] ||
          it->second.alts[i].v1 != variants[1]) {
        continue;
      }
      // 队首 = 最近用过。两条的容量，`swap` 一次就是完整的一次 LRU 更新。
      if (i != 0) std::swap(it->second.alts[0], it->second.alts[i]);
      out = it->second.alts[0].hints;
      return true;
    }
    return false;
  }

  /**
   * @brief 前台解析记忆的**写**侧：`probe_job()` 探完之后的回写。**零取锁。**
   *
   * 两条互斥的出路，判据是"这次解析动没动结论"：
   *
   *  - `fresh` 非空 = 这次**完整解析过**（记忆没答上来，或者答上来的那条当场
   *    校验没通过）。此时全局表刚被 `put_hints()` 合并进新结论，而手里这份
   *    `hints` 里混着旧的 —— **丢掉这条**比留一条旧的安全：下一条请求会重新
   *    从全局表拷一份（那时它拿到的就是合并后的那份）。
   *  - `fresh` 空 = 记忆**全答上来了**。这时 `hints` 就是全局表对这个 URL 的
   *    完整答案，逐字节可拷 —— 这才是前台表会被填满的那条路（也正是热路径：
   *    一条 URL 被探过、被记下之后，之后每次请求都从这里出）。
   *
   * 只在 `LOOP_STAT` 档被调（`probed_on_loop`），所以这张表**只被它自己那条
   * 循环的线程碰**，不需要锁 —— 这是 `retire_bucket::memo` 能免锁的全部前提。
   */
  void memo_store(uvcpp_loop* l, const std::string& url,
                  const std::string variants[2], const hint_vec& hints,
                  const hint_vec& fresh) {
    if (!cache_enabled()) return;
    std::map<std::string, memo_entry>& tab = bucket_for(l)->memo;
    const std::map<std::string, memo_entry>::iterator it = tab.find(url);
    if (!fresh.empty()) {
      if (it != tab.end()) tab.erase(it);
      return;
    }
    if (hints.empty()) return;  // 全局表里本来就没有这个 URL 的记忆

    const unsigned long long gen = front_gen_.load(std::memory_order_acquire);
    memo_entry* e = nullptr;
    if (it == tab.end()) {
      // 上限与 `put_hints()` 同款：满了整表清掉，不做逐条淘汰。
      if (tab.size() >= opts.cache_max_entries) tab.clear();
      e = &tab[url];
    } else {
      e = &it->second;
      if (e->gen != gen) {
        e->alts.clear();  // 上一代的后缀集合可能已经不成立了
      }
    }
    e->gen = gen;

    for (size_t i = 0; i < e->alts.size(); ++i) {
      if (e->alts[i].v0 == variants[0] && e->alts[i].v1 == variants[1]) {
        e->alts[i].hints = hints;  // 就地替换（容量复用），并提到队首
        if (i != 0) std::swap(e->alts[0], e->alts[i]);
        return;
      }
    }
    if (e->alts.size() >= k_alt_max) e->alts.pop_back();  // 顶掉最久没用过的
    e->alts.push_back(memo_alt());
    memo_alt& a = e->alts.back();
    a.v0 = variants[0];
    a.v1 = variants[1];
    a.hints = hints;
  }

  /// **要求已持有 `mu_`**（只从 `collect_probes()` 里调）。
  void add_probe_locked(const std::string& key, const std::string variants[2],
                        std::vector<cache_probe>& out,
                        hint_vec* hints) const {
    add_one_probe_locked(key, out);
    add_one_hint_locked(key, hints);
    for (size_t i = 0; i < 2; ++i) {
      if (!variants[i].empty()) {
        add_one_probe_locked(key + variants[i], out);
        add_one_hint_locked(key + variants[i], hints);
      }
    }
  }

  /**
   * @brief 把 `key` 及其孪生后缀的记忆拷进 `out`。**要求已持有 `mu_`。**
   *
   * 单独有一个"从原 URL 进"的入口（而不是只从 `add_probe_locked()` 进），
   * 是为了让 `LOOP_STAT` 档的 `collect_hints()` 也能复用 —— 那条路上
   * `serve()` 根本不取探针，用不上 `cache_probe`。
   */
  void add_hints_locked(const std::string& key, const std::string variants[2],
                        hint_vec* out) const {
    if (out == nullptr || !cache_enabled()) return;
    add_one_hint_locked(key, out);
    for (size_t i = 0; i < 2; ++i) {
      if (!variants[i].empty()) add_one_hint_locked(key + variants[i], out);
    }
  }

  /// 记忆的**单键**版本：`variants` 已经在上层展开过了。
  ///
  /// 为什么记忆不跟 `cache` 共用一份表：两者的键空间不同 —— 缓存条目的键是
  /// **最终命中**的那个 URL（索引文件、SPA 入口都算），而记忆的键是
  /// **被探过**的那个 URL（`probe_leaf()` 收到的参数）。索引文件那一条上两者
  /// 恰好不同，合并就得在探之前知道"最后会选中谁"，那正是探测要回答的问题。
  void add_one_hint_locked(const std::string& key, hint_vec* out) const {
    // 唯一一道闸：`cfg->resolve_memo` 关掉时这份表**永远是空的** ——
    // `probe_leaf()` 那边也不再记账，所以"关掉 = 逐字节回到不做记忆时"
    // 这条判据不依赖任何一个调用点记得住这件事。
    if (out == nullptr || !cfg->resolve_memo) return;
    for (size_t i = 0; i < out->size(); ++i) {
      if ((*out)[i].first == key) return;  // 去重：候选键可能撞车
    }
    const std::map<std::string, resolve_hint>::const_iterator it =
        hints_.find(key);
    if (it == hints_.end()) return;
    out->push_back(*it);
  }

  /// worker 侧：把这次完整解析出来的结论写回（**一次锁**）。
  ///
  /// 满了就整表清空而不是逐条淘汰：这份表是纯记忆，丢掉任何一条都只意味着
  /// "下次多走一次 `realpath`"，正确性不受影响；而按 LRU 淘汰要额外养一条
  /// 链表和一堆迭代器，换来的只是"冷热混杂时少失效几条"。上限取
  /// `cache_max_entries` —— 与内容缓存同一把尺子，配置一个有界的东西不该
  /// 需要第二个旋钮。
  void put_hints(const hint_vec& fresh) {
    if (!cache_enabled() || fresh.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (hints_.size() + fresh.size() > opts.cache_max_entries) hints_.clear();
    for (size_t i = 0; i < fresh.size(); ++i) {
      hints_[fresh[i].first] = fresh[i].second;
    }
  }

  /**
   * @brief 把 `key` 的探针快照拷进 `out`；`key` 不在缓存里就什么都不做。
   *
   * **要求已持有 `mu_`。**
   *
   * 去重是对 `out` 的线性扫描（候选键确实会撞车：`spa_url` 可能就是某个
   * 索引名）。**故意没换成 `std::set`**：候选键的个数是
   * `(1 + 索引名个数 + spa) × 3`，正常配置下是个位数，十几次字符串比较比给
   * 每个键建一个红黑树节点（每次都要分配）划算。
   */
  void add_one_probe_locked(const std::string& key,
                            std::vector<cache_probe>& out) const {
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i].key == key) return;  // 去重：候选键可能撞车
    }
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

  /**
   * @brief `LOOP_STAT` 那一档的缓存比对：**查 `key` 一个键**，判据与探针一致。
   *
   * 判据和 `POOL` 那一支的三个字段逐个相同（大小、mtime 秒、mtime 纳秒）——
   * 也就是"这份快照还算不算数"在两条路上是**同一个问题**，只是快照的来源
   * 不同：`POOL` 是 `serve()` 提前拷的，这里是**当场读**的（因为 stat 刚刚在
   * 同一条线程上做完，中间没有别的东西能插进来）。
   *
   * 不提升 LRU：`POOL` 那一支也只是把快照拷进 job，从不碰 `lru`。两条路在
   * "命中算不算一次使用"上的行为必须一致，否则 `LOOP_STAT` 会悄悄改掉淘汰次序。
   *
   * **先问本循环那份前台副本**（`retire_bucket::body`，零取锁），没命中才落到
   * 全局表上取**一次**锁；全局那边命中之后顺手把结果登记进前台（见
   * `body_store()`）。为什么这笔账值得单独记一次：实测这一次取锁的持有是
   * 544~627 ns、后面还跟着 1.3~2.4 µs 的等锁（本机 9 条循环、`-m 32`），
   * 而它下面真正干的活只是**一次 `std::map` 查找加一次 `shared_ptr` 拷贝**。
   *
   * **不是 `const` 了**：前台表是懒建/懒填的。
   */
  bool lookup_cache(uvcpp_loop* l, const std::string& key, int64_t size,
                    int64_t sec, int64_t nsec,
                    std::shared_ptr<std::string>& out) {
    // 桶**只在这里取一次**（`bucket_for()` 可能会取 `mu_`），下面登记时把它
    // 传下去 —— `body_store()` 不再自己取桶。理由不是省一次查找：这里往下
    // 那个 `lock_guard` 是**非递归**的，`body_store()` 若在锁里去取一个新桶，
    // 就是自死锁（`bucket_for()` 的线程本地缓存会把这件事盖住，直到某天有人
    // 把登记挪到别的线程上 —— 那时它不会再报错，只会挂住）。
    retire_bucket* const b = cache_enabled() ? bucket_for(l) : nullptr;
    if (b != nullptr) {
      std::map<std::string, body_entry>& tab = b->body;
      const std::map<std::string, body_entry>::iterator fit = tab.find(key);
      if (fit != tab.end() &&
          fit->second.gen == front_gen_.load(std::memory_order_acquire) &&
          fit->second.size == size && fit->second.mtime_sec == sec &&
          fit->second.mtime_nsec == nsec) {
        out = fit->second.data.lock();
        if (out) return true;
        // 全局表那边已经把它淘汰了（弱引用是唯一还能看见这件事的地方）。
        // 擦掉这条再落下去问全局 —— 这次会走一次完整的全局查找，而不是
        // 拿一份已经不存在的字节去应答。
        tab.erase(fit);
      }
    }

    std::lock_guard<std::mutex> lk(mu_);
    const std::map<std::string, cache_entry>::const_iterator it = cache.find(key);
    if (it == cache.end()) return false;
    if (it->second.size != size || it->second.mtime_sec != sec ||
        it->second.mtime_nsec != nsec) {
      return false;
    }
    out = it->second.data;
    // 顺手登记一份前台。**登记在这里而不是 `put()` 里**：`put()` 只知道
    // "刚读进来那一份"，而"全局表里这份还算数"是**这一次查找**得出的结论，
    // 也是前台下一次能命中的唯一依据。代价是每条循环每个键多取一次锁
    // （登记的这一下），之后同一条循环上全是零取锁。
    body_store(b, key, out, size, sec, nsec);
    return true;
  }

  /**
   * @brief 把一份**确实还算数**的字节登记进本循环的前台副本。
   *
   * 只从 `lookup_cache()` 里调（那里是 `LOOP_STAT` 档的循环线程，也正是这张表
   * 唯一的写者）。登记的是**弱引用**：这条副本按不住字节，也就不会让
   * `bytes` 的账与全局表脱节（见 `body_entry`）。
   *
   * \p b 由调用方传进来，**它们都不会为 nullptr**：调用点已经确认了
   * `cache_enabled()`、并且那次 `bucket_for()` 与这里同线程同循环。所以本函数
   * **不自己取桶** —— 它是在持有 `mu_` 的情况下被调的，再去 `bucket_for()`
   * 一个新桶就是自死锁（非递归锁）。
   *
   * 上限与 `put_hints()` 同款：满了整表清掉，不做逐条淘汰。丢掉任何一条只
   * 意味着"下次多取一次锁"，没有正确性代价。
   */
  void body_store(retire_bucket* b, const std::string& key,
                  const std::shared_ptr<std::string>& data, int64_t size,
                  int64_t sec, int64_t nsec) {
    if (b == nullptr || !data) return;
    std::map<std::string, body_entry>& tab = b->body;
    if (tab.size() >= opts.cache_max_entries && tab.find(key) == tab.end()) {
      tab.clear();
    }
    body_entry& e = tab[key];
    e.gen = front_gen_.load(std::memory_order_acquire);
    e.size = size;
    e.mtime_sec = sec;
    e.mtime_nsec = nsec;
    e.data = data;
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
    // 解析记忆跟着一起清：它记的是"这个 URL 的磁盘身份"，而调用方喊
    // `clear_cache()` 的场合（换语料、跑合规探针之前）正是"磁盘那边可能
    // 全变了"的场合。不清它也**不会错**（inode 变了自然会回退），但清掉能
    // 让"清缓存之后第一次请求必定走完整解析"成为一条可以断言的性质。
    hints_.clear();
    // **每条循环那份前台副本也要一起作废**，否则上面那条性质只对全局表成立：
    // 请求会从 `retire_bucket::memo` 里拿到一份"本该被忘掉"的记忆直接命中。
    // 加代次而不是去遍历每条循环的表 —— 那些表**不能被这条线程碰**（它们没有
    // 锁，见 `retire_bucket::memo`），一个整数就能让它们全部失效。
    front_gen_.fetch_add(1, std::memory_order_release);
  }

  // ---- 计数（`std::atomic`，不取锁）。
  //
  // 这三个数各自只被加减、从不参与任何决策，从前跟着邻居一起取 `mu_` 纯属
  // 历史遗留。但**不能**因此退化成裸 `unsigned long long`：多循环下它们是真
  // 有并发写的（`finish_job()` 跑在各自的循环线程上），那就是数据竞争。
  // 计数本身是观测量，不需要与别的字段构成任何不变量，所以用 relaxed。
  void note_hit() { hits.fetch_add(1, std::memory_order_relaxed); }

  void note_miss() { misses.fetch_add(1, std::memory_order_relaxed); }

  void note_rejected() { rejected.fetch_add(1, std::memory_order_relaxed); }

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
    return hits.load(std::memory_order_relaxed);
  }

  unsigned long long miss_count() const {
    return misses.load(std::memory_order_relaxed);
  }

  unsigned long long rejected_count() const {
    return rejected.load(std::memory_order_relaxed);
  }

  // ---- 跨线程两侧 ----

  /**
   * @brief 探路径 + 缓存比对：把 job 填成"知道该做什么"的状态。**不读盘**。
   *
   * 在 `POOL` 档里跑在 worker 上（那正是本模块一直以来的样子）；在 `LOOP_STAT`
   * 档里由 `serve()` 在**循环线程**上直接调一次 —— 这是两档唯一的实现差别，
   * 函数体一行都不变。
   */
  void probe_job(job* j);

  /// `probe_job()` 的实体（`probe_job()` 只负责取/放解析记忆那两件外围事）。
  void probe_job_inner(job* j);

  /**
   * @brief 把 `probe_job()` 认定要读的那份文件读进内存。会阻塞，**要占名额**。
   *
   * 只有 `needs_read()` 为真时才该调 —— 它只从 `run_job()` 里被调。
   */
  void read_job(job* j);

  /// 探完（可能已在循环线程上探过）之后，这次请求**还要不要读盘**。
  ///
  /// 判据只有一条：探到了文件、且手上没有它的字节。`STREAM`/`TOO_LARGE` 是
  /// 终结态（前者自己按块读、后者一个字节都不读），`NOT_FOUND`/`FORBIDDEN`
  /// 更没有；命中缓存则已经有了 `data`。
  bool needs_read(const job* j) const {
    return j->status == probe_status::OK && !j->data;
  }

  void run_job(job* j) {  ///< worker 线程（`LOOP_STAT` 下只做后半段）
    if (!j->probed_on_loop) probe_job(j);
    if (needs_read(j)) read_job(j);

    // **读完了，名额就在这里还** —— 不等 `after_work`。这是 2026-09-24 那次
    // 分片路改动（见 `read_job` 里 `STREAM` 那一支的注释）在整读路上的同一条：
    // 闸门想封的是"并发的磁盘读"，而 `after_work` 跑在**循环线程**上 —— 循环
    // 线程一忙，`after_work` 排不上队，名额就被"一个字节都读完了、只是在等
    // 循环线程"的请求攥着。后果不是慢，是**误判**：在途数看起来到了 256（= 池
    // 线程 64 × 4），真正在读盘的却只有 ≤ 64 个，于是后来者被 503 回绝 ——
    // 而池子其实还有余量。实测（本机 8 核、`taskset -c 0-7`、1024 连接 ×
    // `-m 32`）：改动前 3,642 个 503，日志就是 `工作池已满（在途 256 / 256）`。
    //
    // 走 `release_deferred()` 而不是 `release()`：唤醒回调必须跑在注册它的
    // 那根线程上（分片传输与上传都注册在 loop 线程上），从 worker 里同步派发
    // 会让它们跑错线程。记账立刻发生（名额早一拍变松），派发由下面那个
    // `after_work` 补 —— 那是本 job 一定会走到的、属于 loop 线程的回调。
    if (j->slot_held) {
      j->slot_held = false;
      j->slot_deferred = j->self->work_limit->release_deferred();
    }
  }
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
    // 状态，不碰任何共享量。把写点留在业主那侧更省事，也少一处跨线程的写
    // （那个计数今天已经是原子的，但"由业主记自己的账"这条分工没变）。
    j->status = probe_status::REJECTED;
    return false;
  }
  j->slot_held = true;
  return true;
}

void uvcpp_web_static::Impl::probe_job(job* j) {
  // `LOOP_STAT` 档的 `serve()` 不取探针（那条路用不上 `cache_probe`），于是
  // 解析记忆得在这里自己取。`POOL` 档的记忆是搭 `serve()` 里
  // `collect_probes()` 那一次锁的车来的，这里什么都不做。
  //
  // 为什么值得为 `LOOP_STAT` 专门取一次记忆：这一档的 `stat` 与 `realpath` 都
  // 跑在**循环线程**上，而选项注释里写明它的代价正是"这个阻塞会拖住同一条
  // 循环上的其它连接"。C2 恰恰是把那里的 5 次 `lstat` 换成 1 次 `stat`，
  // 对这条线程的价值比对 worker 大。
  //
  // **这笔交换里贵的那一半后来换了地方**：起初量的是"未争用的取锁 20 ns 换
  // 3~5 µs 的 VFS 往返"，但那 20 ns 只在单循环下成立 —— 多循环打满时这一次
  // 取锁是**真的有争用**的，实测持有 1534 ns（本机 9 条循环、`-m 32`），其中
  // 大头是取锁本身而不是临界区里的活。所以现在的读侧先问本循环那份前台副本
  // （`memo_lookup()`，零取锁），只有没命中才落到全局表上。
  if (j->probed_on_loop) {
    collect_hints(j->loop, j->url, j->try_variants, j->hints);
  }

  probe_job_inner(j);

  // 这次完整解析出来的结论攒在 `fresh_hints` 里，到这里一次性写回。
  // 攒的原因见 `job::fresh_hints`；空表时 `put_hints()` 提前返回，**一次锁
  // 都不取** —— 于是"记忆已经在表里"的那些请求（也就是热路径）不付这份钱。
  if (!j->fresh_hints.empty()) put_hints(j->fresh_hints);

  // 前台副本的回写。与 `put_hints()` 同一个分岔条件（`probed_on_loop`），
  // 所以 `POOL` 档一次都不碰这张表。顺序要紧：先 `put_hints()` 把新结论并进
  // 全局表，再让 `memo_store()` 决定"拷一份"还是"丢掉一条"（判据是
  // `fresh_hints` 空不空，见它的注释）。
  if (j->probed_on_loop) {
    memo_store(j->loop, j->url, j->try_variants, j->hints, j->fresh_hints);
  }
}

void uvcpp_web_static::Impl::probe_job_inner(job* j) {
  probe_result pr;
  probe_status s = probe_with_index(j->loop, *j->cfg, j->url, pr, &j->hints,
                                    &j->fresh_hints);

  // SPA 回退：**只对"路径不存在"生效**。被策略挡下的路径绝不能回落到 SPA ——
  // 否则 `/.env` 会拿到一个 200 的 index.html，一次探测就变成了正常响应，
  // 安全问题再也看不见。
  if (s == probe_status::NOT_FOUND && !pr.policy_rejected &&
      j->cfg->spa_fallback && !j->cfg->spa_url.empty()) {
    probe_result sp;
    const probe_status ss = probe_with_index(j->loop, *j->cfg, j->cfg->spa_url,
                                             sp, &j->hints, &j->fresh_hints);
    if (ss == probe_status::OK && !sp.is_dir) {
      pr = sp;
      s = ss;
    }
  }

  j->status = s;
  j->policy_rejected = pr.policy_rejected;
  j->final_url = pr.final_url;
  j->fs_path = pr.fs_path;
  j->mime_path = pr.fs_path;
  j->mtime_sec = pr.mtime_sec;
  j->mtime_nsec = pr.mtime_nsec;
  j->size = pr.size;
  j->served_from_cache = false;

  if (s != probe_status::OK || pr.is_dir) return;

  // ---- 预压缩孪生文件 ----
  //
  // 位置在尺寸闸门**之前**：闸门该量的是"这次真要读进内存的那份"有多大，
  // 而孪生文件比原文件小得多（本仓语料上平均小 75%）—— 拿原文件的大小去
  // 拒绝一个只读 12 KB 的请求，是拿一条与实际字节无关的数当判据。
  //
  // 只走**整读**那条路：超过 `max_cached_file_size` 的走分片下发，而分片
  // 下发不经过响应层，`content-encoding` 这个头挂不上去（见选项说明）。
  // 所以孪生文件自己也超阈值时一样退回原文件。
  //
  // 探不到（404/403/是个目录）就清空 `variant`，让后面全部退回原文件 ——
  // 于是"没预压过的资源"与关掉本选项时逐字节相同。
  //
  // 这一步用 `probe_leaf` 而不是 `probe_with_index`：孪生文件只能是**文件**
  // 本身，`/static/app.js.br/index.html` 这种东西不存在也不该被找。
  for (size_t i = 0; i < 2; ++i) {
    if (j->try_variants[i].empty()) break;
    probe_result vp;
    const probe_status vs =
        probe_leaf(j->loop, *j->cfg, pr.final_url + j->try_variants[i], vp,
                   &j->hints, &j->fresh_hints);
    if (vs == probe_status::OK && !vp.is_dir &&
        static_cast<uint64_t>(vp.size) <=
            static_cast<uint64_t>(j->cfg->max_cached_file_size)) {
      j->variant = j->try_variants[i];
      j->final_url = vp.final_url;
      j->fs_path = vp.fs_path;
      j->mtime_sec = vp.mtime_sec;
      j->mtime_nsec = vp.mtime_nsec;
      j->size = vp.size;
      break;
    }
    // 首选缺席（或它自己超阈值）就试下一个 —— nginx 与 Caddy 都是这个次序。
    // 全都没探到则 `j->variant` 保持为空，退回原文件。
  }

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
  if (static_cast<uint64_t>(j->size) > static_cast<uint64_t>(thr)) {
    // 一个字节都不读。元数据已经全部带在 job 上了（上面那一段赋过值），
    // 所以 ETag/Last-Modified/条件请求/Range 在 loop 线程那侧照常判定。
    //
    // **这条路上不取名额**（2026-09-24 改）。原先在这里 `acquire_slot`，
    // 而名额是到 `after_work` 才还的 —— after_work 跑在**循环线程**上，于是
    // 1000 条并发大文件时循环线程搬运不过来、after_work 排不上队，名额就被
    // 攥在"一个字节都还没读"的请求手里。实测 `ab -c 1000` 打 8 MiB 文件：
    // 4000 个请求里 267 个 503（约 6.7%），日志是
    // `工作池已满（在途 0 / 16），拒绝静态请求 /big.bin` —— **在途 0 却拒绝**，
    // 就是"名额被没在读盘的请求占着"的现场。
    //
    // 现在名额改由**传输自己按块借还**（见 `uvcpp_web_file_transfer::
    // set_chunk_gate`）：读一块之前拿、读完立刻还，拿不到就停在块边界上等
    // 唤醒。于是名额封的是"真正并行的读盘数"，而 1000 条传输可以全都在跑。
    j->status = probe_status::STREAM;
    return;
  }

  // 缓存校验：拿"期望快照"和刚 stat 到的真实值比。对上了就**不读盘** ——
  // 热文件的成本是一次 stat。
  //
  // **命中路径不占名额**，这是本模块最关键的一条：命中时只做了一次 `stat`
  // （几微秒），一个字节的磁盘读都没有，内存面也早就有 `max_cached_file_size`
  // 封着。让这样一次请求去抢名额，代价是**并发一过闸门就大面积 503**，而收益
  // 是零 —— 实测并发 64 时 71.7% 的请求被拒，其中 99.97% 本来能命中缓存。
  //
  // 两档在这一步分岔，**判据完全相同**（键、大小、mtime 秒、mtime 纳秒）：
  //  - `POOL`：快照是 `serve()` 提前在缓存里拷的（`j->probes`）。stat 在 worker
  //    上，两件事不在同一刻，只能"先拍快照、后比对"。
  //  - `LOOP_STAT`：`probe_job()` 就是在循环线程上被调的那一次，stat 与查表
  //    **一刻完成**，快照这个概念不存在了 —— 直接查 `final_url` 一个键。
  //    **为什么只查一个键还等价**：上面那一支的循环体里，真正能命中的也只有
  //    `p.key == j->final_url` 那一条，索引名/SPA 回退/孪生后缀这些候选键在
  //    这个等式上都会被筛掉。候选列表的意义是"把数据提前拿到手里"，而这里
  //    数据就地在缓存里，拿到手的方式就是这把锁下面这一次查找。
  if (j->probed_on_loop) {
    if (cache_enabled() &&
        lookup_cache(j->loop, j->final_url, j->size, j->mtime_sec,
                     j->mtime_nsec, j->data)) {
      j->served_from_cache = true;
    }
    return;
  }
  for (size_t i = 0; i < j->probes.size(); ++i) {
    const cache_probe& p = j->probes[i];
    // 比的是**这一份**（可能已经是孪生文件）的身份：键、大小、mtime。键里
    // 已经带上了后缀，所以 `.br` 的命中不会把原文件的条目顶掉。
    if (p.key == j->final_url && p.size == j->size &&
        p.mtime_sec == j->mtime_sec && p.mtime_nsec == j->mtime_nsec) {
      j->data = p.data;
      j->served_from_cache = true;
      return;
    }
  }
}

void uvcpp_web_static::Impl::read_job(job* j) {
  // 走到这里只剩一条路：**把文件整读进内存**。这才是闸门要卡的那件事。
  if (!acquire_slot(j)) return;

  std::shared_ptr<std::string> buf(new std::string());
  if (!read_file(j->loop, j->fs_path, j->size, *buf)) {
    UVCPP_LOG_ERROR(log_category::IO)
        << "静态文件读取失败: " << j->fs_path << "（" << j->size << " 字节）";
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
      note_rejected();  // 观测用计数；多循环下不止一条循环线程会加减，故用原子
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

  // MIME 取**原文件**那一份（`mime_path`）：孪生文件是同一份表示的另一种
  // 编码，靠 `content-encoding` 声明，类型不该跟着后缀变成 octet-stream。
  const std::string ct = content_type_for(j->mime_path);
  //
  // ETag 先写成**栈上的 C 串**：这个值每个 200 响应都要发一次，而需要的只有
  // 头表持有的那一份 —— 先建 `std::string` 再传 `const std::string&` 是白付
  // （那条拼接链实测五次 `operator+`）。下面两处真的要比对它的时候
  // （客户端带了 `If-None-Match` / `If-Range`）才就地建串，那两条路在一次
  // 压测里是 0 次。
  char etag_buf[64];
  size_t etag_len = 0;
  if (opts.etag) {
    etag_len = format_etag_into(etag_buf, sizeof(etag_buf), j->size,
                                j->mtime_sec, j->mtime_nsec);
  }

  // 这几个头在 200/206/304 上都要有，所以先发，后面不再重复。
  if (etag_len > 0) resp.set_header("etag", etag_buf);
  if (opts.last_modified) {
    // 这一份仍然要一个 `std::string`：`web_http_date()` 的返回值就是它，
    // 而且它的长度（29）必然超过 SSO 上界。**没有**为它再加一条 C 串形态的
    // 日期格式化 —— 那会把 IMF-fixdate 的拼法复制成两份，而这里只省一次
    // 短命分配。
    const std::string last_modified =
        web_http_date(static_cast<time_t>(j->mtime_sec));
    if (!last_modified.empty()) resp.set_header("last-modified", last_modified);
  }
  if (!opts.cache_control.empty()) {
    resp.set_header("cache-control", opts.cache_control);
  }
  resp.set_content_type(ct);

  // 预压缩孪生文件：body 就是盘上那份压缩产物，声明它的编码，并挂上
  // `vary` —— 同一个 URL 从此有两种表示，任何共享缓存都必须按
  // `Accept-Encoding` 分桶，否则会把 brotli 的字节发给只认 gzip 的客户端。
  //
  // 声明之后 `apply_compression()` 会**自己让路**：它见到响应已有
  // `content-encoding` 就原样放行，不会再压第二遍（压第二遍还会把已经压过的
  // 字节再"压大"一次）。
  if (!j->variant.empty()) {
    resp.set_header("content-encoding",
                    j->variant == ".br" ? "br" : "gzip");
    resp.set_header("vary", "accept-encoding");
  }

  const bool ranged_ok = opts.range;
  if (ranged_ok) resp.set_header("accept-ranges", "bytes");

  // ---- 条件请求 ----
  //
  // 顺序不能反：RFC 7232 §3.3 规定 `If-None-Match` 存在时
  // `If-Modified-Since` **必须被忽略**。两个都看会让"客户端手里那份的
  // ETag 不匹配、但时间戳却在文件之前"这种情况错误地回 304，客户端于是
  // 一直用着旧内容 —— 而且它自己永远发现不了。
  bool not_modified = false;
  if (etag_len > 0 && req.has_header("if-none-match")) {
    const std::string etag(etag_buf, etag_len);
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
          const std::string etag(etag_buf, etag_len);
          range_allowed =
              (etag_len > 0) && (strip_quotes(v) == strip_quotes(etag));
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
    uvcpp_snprintf(buf, sizeof(buf), "bytes %lld-%lld/%lld",
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
    // 闸门在**发起之前**交给响应（传输对象是响应自己 `new` 出来的，调用方
    // 手上没有它），由响应转交给那个传输。`.get()` 是**借用**：`work_limit`
    // 的所有者是本 `Impl`，而请求属于 App，App 活着它就在 —— 与下面
    // `after_work` 里 `j->self->work_limit->release()` 借的是同一个生命周期。
    // 为 `nullptr` 时与从前逐字节相同。
    resp.set_file_chunk_gate(work_limit.get());
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
    // **用了孪生文件就不设 tag**：tag 是给**实时压缩**的变体缓存当键的，
    // 而这份 body 已经是压缩产物，`apply_compression()` 见到
    // `content-encoding` 根本不会进来。设了它只会白占一份变体缓存的位置。
    if (!partial && j->variant.empty()) {
      //
      // **就地拼，不造中间串。** `a + b + c + d` 那一版每加一次都产出一个
      // 临时串（实测四次分配），而需要的只有目标那一份。`clear()` 之后再
      // `reserve()` 一次到位，后面几次追加就都在容量里 —— 总共一次分配。
      // 三个数用不着 `std::to_string`：它返回的串对这几个长度本来就走 SSO，
      // 白白多拷一遍。
      std::string& tag = resp.raw().cache_tag;
      tag.clear();
      tag.reserve(j->fs_path.size() + 32);
      tag.append(j->fs_path);
      tag += '\x1f';
      append_i64_dec(tag, j->size);
      tag += '\x1f';
      append_i64_dec(tag, j->mtime_sec);
      tag += '\x1f';
      append_i64_dec(tag, j->mtime_nsec);
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
      precompressed(false),
      revalidate(uvcpp_web_revalidate::POOL),
      resolve_memo(true),
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

// 挑选规则（只认 br/gzip、`q=0` 是拒绝、同 q 时 br 在前、`*` 兜底、返回有序
// 候选表）**不在这份文件里** —— 它是 `http_pick_precompressed_variants()`，
// 与协议层的 `uvcpp_static_server` 共用同一份实现。理由见那个函数上面那段：
// 同一条 `Accept-Encoding` 在 :8443/udp 与 :8443/tcp 上选出不同的表示，是
// 那种"看日志永远看不出来"的偏差，所以规则只能有一处。
size_t pick_precompressed_variants(const std::string& header,
                                   std::string out[2]) {
  return http_pick_precompressed_variants(header, out);
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
  cfg->resolve_memo = opts.resolve_memo;

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
  // 从本循环的回收站里取一个（没有才 `new`）。取到的那个是干净的，下面这些
  // 赋值**逐条与 `new job()` 之后完全一样**；回收站带来的差别是那些成员串的
  // 容量还在，于是这些赋值多数不再落堆 —— 见 `Impl::job::reset()`。
  Impl::job* j = im->take_job(loop);
  j->self = im;
  j->cfg = im->cfg;
  j->loop = loop;
  j->req = &req;
  j->resp = &resp;
  j->url = url;
  j->is_head = (req.method() == http_method::HTTP_HEAD);

  // 预压缩孪生文件的后缀在这里定，**只按请求头算一次**。
  //
  // 带 `Range` 的请求一律不用：分段要切的是**原文件**的字节，对孪生文件切段
  // 会切出一段没有意义的压缩流。这与响应层那条"206 一律不编码"是同一条规则
  // （对孪生文件来说，206 与 `content-encoding` 一起出现本身就是矛盾的：
  // `Content-Range` 会拿原文件的坐标系去描述压缩流里的偏移）。
  if (im->opts.precompressed && !req.has_header("range")) {
    pick_precompressed_variants(req.header("accept-encoding"),
                                j->try_variants);
  }

  // 留住 next。**赋值必须发生在 `serve()` 返回之前**（也就是 handler 调用
  // 返回之前）—— 框架是在 handler 返回的那一刻量引用数来判断"留没留"的。
  j->next = next;

  if (im->loop_revalidate()) {
    // ---- LOOP_STAT：探路径与缓存校验**就在这条线程上做完** ----
    //
    // 这里是整个选项的全部实现：把 worker 那前半段提前搬过来同步跑一次。
    // 探到了、而且缓存里那份还算数（或者已经是终结态），这次请求就**一个池
    // 任务都不用投** —— 省掉的是"提交 + 唤醒 + after_work 回来"这三次跨线程
    // 往返。实测在 8 核打满时，本模块每请求的 CPU 里有 12~19 µs 花在
    // `futex`/`pthread_cond_signal`/`__lll_lock_*` 上，而那一段**全部**来自
    // 这次往返（`futex`/req ≈ 8 次，命中路径本身一次锁都不该拿）。
    //
    // 代价写在选项的注释里：这个 `stat` 会阻塞同一条循环上的其它连接，
    // 本地盘几微秒、FUSE/网络盘几十微秒到毫秒。**所以默认是 `POOL`。**
    j->probed_on_loop = true;
    im->probe_job(j);
    if (!im->needs_read(j)) {
      // 命中或终结态。走的是**同一个** `finish_job()`，填出来的响应与 worker
      // 那条路逐字节相同 —— 区别只在"谁来填"和"投不投池任务"。
      im->finish_job(j);
      // 与上面几条同步路径同构：填好、`end()`、不留 next，框架自己收尾。
      // 删 `j` 就是丢掉那份 next 副本（引用计数回到原样），所以框架在
      // handler 返回时量到的仍然是"这一环没挂起"。
      resp.end();
      im->release_job(loop, j);
      return;
    }
    // 走到这里只有一种情形：探到了、要读盘。下面照常投池，worker 只做读盘
    // 那一段（`probed_on_loop` 为真，它不会再 stat 第二次）。
  } else if (im->cache_enabled()) {
    im->collect_probes(url, j->try_variants, j->probes, &j->hints);
  }

  uvcpp_work* work = new uvcpp_work();
  const int rc = work->queue_work(
      loop,
      [j](uvcpp_work*) { j->self->run_job(j); },
      [j](uvcpp_work* w, int) {
        // 名额**已经还过了**，就在 `run_job()` 末尾（见那里的长注释）——
        // 还一拍都不该等到这里：这个回调跑在**循环线程**上，而循环线程正是
        // 高负载下最忙的那一根，等到这里还等于让名额被"读完在等循环线程"的
        // 请求攥着。这里只补它欠下的那一刀唤醒派发：`release_deferred()` 只
        // 记账不派发（唤醒回调必须跑在注册它的那根线程上，分片传输与上传都
        // 注册在 loop 线程上），而本回调恰恰是 loop 线程上那个保证会跑的点。
        //
        // 与 `serve()` 里的 `rc != 0` 分支**互斥**（libuv 的约定：
        // `uv_queue_work` 返回 0 才会调 after_work），而且两条路都会 `delete
        // j`，所以不存在补两次的可能。
        if (j->slot_deferred) {
          j->slot_deferred = false;
          j->self->work_limit->dispatch_wakeups();
        }

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
        // 先归还再 `resume()` —— 与原先"先 `delete j` 再 `resume()`"同一个
        // 顺序，所以 `next` 那份上下文引用松开的时刻也一模一样。
        j->self->release_job(j->loop, j);
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
    im->release_job(loop, j);
    resp.server_error();
    resp.end();
  }
}

}  // namespace uvcpp
