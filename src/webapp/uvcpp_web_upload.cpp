/**
 * @file src/webapp/uvcpp_web_upload.cpp
 * @brief `uvcpp_web_upload` 的实现：串行异步落盘的状态机。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 全部并发都在**一个 loop 线程**上，所以本文件没有任何锁，也没有原子量；
 * `uvcpp_fs` 的单笔在途守卫（`async_pending_`）保证了同一时刻只有一笔 fs
 * 操作，本文件只需要一条 `queue_` 把部件按顺序排好。
 */

#include <webapp/uvcpp_web_upload.h>

#include <cstdio>
#include <ctime>
#include <mutex>
#include <random>

#include <webapp/uvcpp_web_util.h>
#include <webapp/uvcpp_web_work_limit.h>
#include <web/uvcpp_http_common.h>

#include <fcntl.h>

// uvcpp_fs.h 自己会把 <uv.h> 拉进来。
#include <req/uvcpp_fs.h>

#include <uv.h>

// POSIX 上没有 O_BINARY（那是 Windows 的"不做 CRLF 翻译"标志）。
#ifndef O_BINARY
#define O_BINARY 0
#endif

namespace uvcpp {

namespace {

/// 落盘时用的打开标志。
///
/// **`O_EXCL` 是这里的安全控制本身**，不是防重名的顺手一笔：没有它，`O_CREAT`
/// 会**顺着一个已存在的符号链接写出去** —— 攻击者只要预先把 `upload_dir` 里
/// 某个名字种成指向别处的链接（他对目录没有写权限也没关系，只要有一处能影响
/// 到那个路径），本模块就会替他写他想写的文件。有了 `O_EXCL`，`open` 在那个
/// 路径上**失败**（`UV_EEXIST`），会话重摇一个名字。
///
/// **没有 `O_TRUNC`**：`O_EXCL` 已经保证 open 不会命中已存在的文件，`O_TRUNC`
/// 在这里是死代码，留着只会让读代码的人以为"这里可能会截断一个已有文件"。
const int k_open_flags = O_WRONLY | O_CREAT | O_EXCL | O_BINARY;
const int k_open_mode = 0600;

/// `UV_EEXIST` 之后重摇名字的次数上限。
///
/// 16 位十六进制撞名的概率是 2^-64 量级，重试一次就够；给到 4 是留给"注入的
/// 生成器故意一直给同一个名字"这种形状 —— 那种情况下要**确定地失败**，而不是
/// 无限重摇（生成器是用户代码，不能假设它良善）。
const size_t k_open_retries = 4;

/// 扩展名保留的字节数上限（白名单过滤**之后**算）。
///
/// 为什么要有：扩展名是从客户端给的名字里取出来的，虽然只留 `[A-Za-z0-9]`，
/// 但长度不受控的话一个 `aaaa...` 能让落盘名长得离谱（路径长度是有系统上限的）。
/// 10 足够覆盖任何真实扩展名（`.tar.gz` 那种多段形式只保留最后一段，见下）。
const size_t k_max_ext_bytes = 10;

/**
 * @brief 从**已清洗**的客户端文件名里取一个安全的扩展名（含 `.`，可空）。
 *
 * 三条规则，每条都有理由：
 *   - **只取最后一个 `.` 之后的部分**：`a.tar.gz` 保留 `gz`。多段形式对落盘名
 *     没有意义 —— 落盘名不需要"看起来像原名"，它只需要一个有界的后缀。
 *   - **白名单 `[A-Za-z0-9]`**：黑名单无论怎么列都会漏（跨平台的怪字符、编码、
 *     以及"现在没想到的"），白名单漏无可漏。扩展名本来就没有非字母数字的正当
 *     用法。
 *   - **过滤后为空就整个丢掉**：宁可不带扩展名，也不要一个 `.` 结尾的名字
 *     （Windows 会静默截掉结尾的点，那正是"展示名与落盘名不一致"的来源）。
 */
std::string pick_extension(const std::string& sanitized) {
  const size_t dot = sanitized.rfind('.');
  // `dot + 1 == size()` 是"以点结尾"（清洗后不该出现，但真出现也不该取一个
  // 空扩展名）；`dot == 0` 是隐藏文件（`.bashrc`），那不是扩展名。
  if (dot == std::string::npos || dot == 0 || dot + 1 >= sanitized.size()) {
    return std::string();
  }

  std::string ext;
  for (size_t i = dot + 1; i < sanitized.size() && ext.size() < k_max_ext_bytes;
       ++i) {
    const char c = sanitized[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9');
    if (!ok) break;
    ext.push_back(c);
  }
  if (ext.empty()) return std::string();
  return "." + ext;
}

/**
 * @brief 叶子名校验：必须能安全地 `join_path` 到目录后面。
 *
 * 拒绝的东西与理由（这是一条**安全**判据，不是"格式检查"）：
 *   - 空 —— `join_path(dir, "")` 得到目录本身，`open` 一个目录会失败但语义
 *     完全错位；
 *   - 含 `/` 或 `\` —— 它能改变落盘**位置**，那就不是叶子名了；
 *   - 含 `:` —— Windows 的盘符/ADS（`a.txt:evil`）都靠它，落盘名里没有任何
 *     正当理由需要冒号；
 *   - `.` / `..` —— 目录别名。
 *
 * 注意**没有**拒绝 `*`/`?`/`"` 这类：它们在 Windows 上确实不能出现在文件名里，
 * 但那种输入下 `open` 自己会失败，会话按 open 失败收场，行为是确定的。把它们
 * 也列进来只会让这条判据看起来像"合法文件名检查"，而它的职责是**边界**。
 */
bool is_safe_leaf(const std::string& leaf) {
  if (leaf.empty() || leaf == "." || leaf == "..") return false;
  if (leaf.find('/') != std::string::npos) return false;
  if (leaf.find('\\') != std::string::npos) return false;
  if (leaf.find(':') != std::string::npos) return false;
  return true;
}

/**
 * @brief 在途写期间允许攒下的字节数，默认 1 MiB。
 *
 * 量级怎么定的：单次 TCP read 交付的块在 64 KiB 上下（libuv 的读缓冲量级），
 * 而 `pause()` 到真正停读之间还会再交付若干块。1 MiB 给的是**十几块**的余量
 * —— 慢磁盘（写一块要几十毫秒）下足够吸收正常抖动，同时又小到"就算真被灌满
 * 也只是 1 MiB 内存"，与上传总长（可能是几个 GB）完全解耦。
 */
const size_t k_max_pending_bytes = 1024 * 1024;

/// 一次播种的 64 位随机源。
///
/// 为什么不用 `rand()`：MSVC 上它只有 15 位、而且是全局状态（谁调一下就把
/// 序列推进了）。为什么用 `random_device` 播种而不是每次直接取：`random_device`
/// 每次调用都要一次系统熵源，而这里只需要在首次摇名字时播一次种。
uint64_t seed_once() {
  std::random_device rd;
  uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
  s ^= static_cast<uint64_t>(std::time(nullptr)) << 17;
  return s;
}

/**
 * @brief 摇一个落盘名（16 位十六进制）。
 *
 * **与客户端给的 `filename` 完全无关** —— 这是本模块最基本的安全前提：
 * 客户端决定不了自己落在盘上的名字。名字可预测本身不是主要威胁（攻击者对
 * `upload_dir` 没有写权限），但"不用客户端给的名字"是。
 *
 * 锁只防"有人在别的线程建会话"（正常用法是整个上传都在 loop 线程上）。
 * `std::mt19937_64` 不是线程安全的，一条 mutex 换掉一整类难查的问题。
 */
std::string make_temp_name() {
  static std::mutex mu;
  static std::mt19937_64 rng(seed_once());
  std::lock_guard<std::mutex> lk(mu);
  const uint64_t v = rng();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(v));
  return std::string(buf, 16);
}

/**
 * @brief 默认的叶子名生成器：16 位随机十六进制 + 白名单过滤后的扩展名。
 *
 * 扩展名来自**已清洗**的客户端文件名 —— 这是客户端的信息里唯一还留在落盘名
 * 上的东西，而且它过了白名单、有长度上限。文件名本身一个字节都不留。
 */
std::string default_leaf_name(const std::string& sanitized) {
  return make_temp_name() + pick_extension(sanitized);
}

/// 拼目录与文件名，容忍调用方给的目录带不带尾部分隔符。
std::string join_path(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir[dir.size() - 1];
  if (last == '/' || last == '\\') return dir + name;
  return dir + "/" + name;
}

uv_file as_uv_file(int fd) { return static_cast<uv_file>(fd); }

}  // namespace

// =========================================================================
// 元数据类型
// =========================================================================

uvcpp_web_upload_file::uvcpp_web_upload_file() : size_(0), truncated_(false) {}

uvcpp_web_upload_field::uvcpp_web_upload_field() {}

uvcpp_web_upload_result::uvcpp_web_upload_result() {}

const uvcpp_web_upload_file* uvcpp_web_upload_result::file(
    const std::string& field) const {
  for (size_t i = 0; i < files_.size(); ++i) {
    if (files_[i].field_name() == field) return &files_[i];
  }
  return nullptr;
}

const std::string* uvcpp_web_upload_result::field(
    const std::string& name) const {
  for (size_t i = 0; i < fields_.size(); ++i) {
    if (fields_[i].name() == name) return &fields_[i].value();
  }
  return nullptr;
}

void uvcpp_web_upload_result::clear() {
  files_.clear();
  fields_.clear();
}

// =========================================================================
// 会话
// =========================================================================

uvcpp_web_upload::upload_job::upload_job()
    : fd(-1),
      fd_valid(false),
      end_seen(false),
      closed(false),
      removed(false),
      kept(false),
      failed(false),
      written(0),
      open_attempts(0) {}

uvcpp_web_upload::uvcpp_web_upload(uvcpp_loop* loop, const std::string& dir_real)
    : loop_(loop),
      upload_dir_real_(dir_real),
      current_(no_job()),
      received_(0),
      busy_(false),
      parse_done_(false),
      discarding_(false),
      done_(false),
      fsync_(true),
      flow_(),
      max_pending_(k_max_pending_bytes),
      work_limit_(nullptr),
      wakeup_id_(uvcpp_web_work_limit::INVALID_WAKEUP),
      slot_held_(false),
      waiting_slot_(false),
      flow_paused_(false),
      pause_events_(0),
      overflow_events_(0) {
  fs_.reset(new uvcpp_fs());
  name_gen_ = &default_leaf_name;
}

std::shared_ptr<uvcpp_web_upload> uvcpp_web_upload::create(
    uvcpp_loop* loop, const std::string& upload_dir) {
  // 目录的真实路径在这里解析一次，之后就不再碰文件系统 —— 落盘路径的包含判断
  // （`resolve_leaf()` 里那道）**每部件**都要用基准，不能每次都去 realpath。
  // 见头文件里"三道防线"那一节：这是第 3 道的基准，不是安全控制本身。
  //
  // `allow_missing = true`：目录还不存在时解析父目录再拼叶子，得到的是
  // "目录建起来之后会是"的那个路径 —— 对包含判断来说正是想要的。解析失败
  // （例如调用方给的是一个不含分隔符的相对名）不致命，回退成原始串。
  std::string real;
  if (!web_real_path(upload_dir, real, /*allow_missing=*/true) || real.empty()) {
    real = upload_dir;
  }
  return create(loop, upload_dir, real);
}

std::shared_ptr<uvcpp_web_upload> uvcpp_web_upload::create(
    uvcpp_loop* loop, const std::string& upload_dir,
    const std::string& upload_dir_real) {
  // 调用方没给已解析的路径时，用原始串兜底 —— 这时两者是同一个字符串，
  // 会话内部的包含判断退化成纯字面前缀（仍然成立，只是失去了"展开符号链接"
  // 这一层，而那一层在会话里本来也不是安全控制）。
  const std::string real = upload_dir_real.empty() ? upload_dir : upload_dir_real;

  // 一个连落点都没有的会话**在创建时就失败**，不放进流里。理由：`resolve_leaf()`
  // 拿空基准做包含判断必然为假，于是一个"目录没配好"的配置错误会以
  // **每个部件**一条"落盘名生成器返回了不安全的叶子名"的形式冒出来 ——
  // 指向生成器，而真正的问题在别处。返回 `nullptr` 让调用方当场报 500
  // （App 那条路就是这么用的）。
  if (real.empty()) return nullptr;

  // 构造函数是私有的：`shared_from_this()` 要求对象**从一开始**就被 shared_ptr
  // 持有，把入口收成一个函数比在文档里叮嘱一遍可靠。
  return std::shared_ptr<uvcpp_web_upload>(new uvcpp_web_upload(loop, real));
}

uvcpp_web_upload::~uvcpp_web_upload() {
  // 还挂在 `work_limit_` 上的唤醒必须摘掉。回调捕的是 `weak_ptr`（见
  // `acquire_slot()`），所以**不会**替本会话续命、也不会造成引用环；但留着
  // 一个永远不会再被用到的条目，等于让唤醒表随历史会话数单调增长。
  clear_wakeup();

  // 最后一道兜底。正常路径下 `pump()` 已经把每个 fd 关掉、该删的也删了；
  // 能带着"还开着的 fd / 还没删的文件"走到这里的只有一种情形 —— 调用方在
  // I/O 还没跑完时就把最后一份 `shared_ptr` 丢了。
  //
  // 这里用**同步** `close`/`unlink`：两者都是有界的元数据操作（不是密集 IO），
  // 而放着一个 fd 加一个半截文件不管才是真问题 —— 客户端可以反复触发它。
  // 会话的生命周期契约是"先于 loop 析构"，所以 loop_ 必然还有效。
  if (!fs_ || loop_ == nullptr) return;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    upload_job& j = jobs_[i];
    // 成功路径上已经交给调用方的，一根手指都不许碰。而 `discarding_` 为真时
    // 连它们也要删 —— 那正是 `fail()` 的契约（见那里）。
    if (j.kept && !discarding_) continue;
    if (j.fd_valid && !j.closed) {
      fs_->close(loop_, as_uv_file(j.fd));
      j.closed = true;
    }
    // 只有 open 成功过才可能已经有文件落在盘上。
    if (j.fd_valid && !j.removed) {
      fs_->unlink(loop_, j.meta.path_.c_str());
      j.removed = true;
    }
  }
}

size_t uvcpp_web_upload::open_file_count() const {
  size_t n = 0;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].fd_valid && !jobs_[i].closed) ++n;
  }
  return n;
}

// -------------------------------------------------------------------------
// sink 接口
// -------------------------------------------------------------------------

bool uvcpp_web_upload::on_part_begin(const uvcpp_web_part_info& info) {
  current_ = no_job();

  // 已经在丢弃模式：继续把字节吃下去（返回 true），让解析器自己走到终边界 ——
  // 在这里返回 false 只是提前中止，而我们无论如何都要把 body 读完才能复用
  // 连接。真正要"掐断"的场景（413）走的是流层的 abort，不是这里。
  if (discarding_) return true;

  if (!info.is_file) {
    field_name_ = info.name;
    field_value_.clear();
    return true;
  }

  upload_job j;
  j.meta.field_name_ = info.name;
  // **清洗**：客户端给的名字只走到"可安全展示的元数据"这一步为止，绝不参与
  // 落盘名的构成。见 `original_filename()` 的 @warning 与 `pick_extension()`。
  j.meta.original_filename_ = web_sanitize_filename(info.filename);
  j.meta.content_type_ = info.content_type;

  // 落盘名由生成器给。生成器返回的东西**必须**是叶子，这里查一次 —— 它不是
  // "生成器一定对"的假设，而是一道真判据：`set_name_generator()` 是给用户的
  // 接缝，用户代码可以返回任何东西，而返回值决定文件落在哪。
  //
  // 非法叶子按**会话失败**处理（不是丢这一个部件就完事）：一个会返回 `../x`
  // 的生成器是编程错误，继续跑下去只会产生一串落在目录外的文件。
  const std::string leaf = name_gen_ ? name_gen_(j.meta.original_filename_)
                                     : std::string();
  std::string path;
  if (!resolve_leaf(leaf, path)) {
    fail("落盘名生成器返回了不安全的叶子名");
    return false;
  }
  j.meta.path_ = path;

  // `std::deque` 的 push_back 不会让已有元素的引用失效，所以 `queue_` 里存下标
  // 是安全的（`std::vector` 就不行 —— 它会搬家，而每个异步回调都持有下标）。
  jobs_.push_back(j);
  current_ = jobs_.size() - 1;
  queue_.push_back(current_);

  // 立刻试着开文件。这里**不**同步 open：交给线程池，回调里再往下走。
  pump();
  return true;
}

bool uvcpp_web_upload::on_part_data(const char* data, size_t len) {
  received_ += len;
  if (len == 0) return true;

  if (discarding_) return true;

  if (current_ == no_job()) {
    // 字段部件（或解析器还没给过部件头 —— 后者不可能，防御性处理）。
    if (!field_name_.empty()) field_value_.append(data, len);
    return true;
  }

  upload_job& j = jobs_[current_];
  if (j.failed) return true;

  // **攒进 `incoming` 而不是 `pending`。**
  //
  // `uv_fs_write` 只 memcpy 那个 `uv_buf_t` **数组**，从不拷贝数据本身 —— 正在
  // 写的那段内存必须活到回调。而 `on_part_data` 完全可能在写完成之前又被调一次，
  // 若直接往 `pending` 上 append，`std::string` 一旦扩容就会把**正在被内核读的
  // 那块缓冲**释放掉。单槽双缓冲把这件事变成结构性的：在途的那份永不改动。
  //
  // 而 `incoming` 是**有上限**的：`pause()` 不是瞬时的（暂停之前已经在解析器
  // 缓冲里的字节还会继续交付），对端也完全可以无视背压继续灌，所以"暂停了还在
  // 灌"必然发生一部分 —— 但绝不能无界。上限是 `set_max_pending_bytes()`。
  //
  // 判据有两个条件，**缺任何一个都是错的**：
  //
  // - `busy_ || waiting_slot_` —— 只有"这一块真的会被攒下来"时才谈越界。没有
  //   在途写、也不在等名额时，下面的 `pump()` 会立刻把它挪进 `pending` 发出去，
  //   它根本不是"攒"。少了这个条件，**一次比上限大的单块交付**（一次 TCP read
  //   的量级，几 KB 到几十 KB）就会把一次完全正常的上传判成越界 —— 而头文件
  //   写得很清楚：单块不会被切开，峰值允许是 `max_pending + 一块`。
  // - 判的是**已经攒下的量**（`incoming.size() >= max_pending_`），不是"加上这
  //   一块会不会超"。后者与前一条自相矛盾（它等于要求单块不得大于上限），前者
  //   才给出头文件承诺的那个峰值的真正上界：攒到上限之后，**再来的那一块**才
  //   被拒。
  if (max_pending_ > 0 && (busy_ || waiting_slot_) &&
      j.incoming.size() >= max_pending_) {
    ++overflow_events_;
    // 顺序很讲究：**先把回调拷到栈上**。`abort` 会同步走到流层的 `on_abort`
    // → `notify_parse_done(false)` → `fail()`，也就是在本函数的栈上重入
    // 会话；`flow_` 是会话的成员，到那时我们已经不该再碰它。
    ::std::function<void(int)> ab = flow_.abort;
    if (ab) {
      // 剩下的 body 没人要了：回 413 并关连接。这条路径**不该**发生 ——
      // 正常的背压会让对端先停下来 —— 所以它一旦发生就说明背压失效了，
      // 继续收只会把内存吃光。
      ab(static_cast<int>(http_status::PAYLOAD_TOO_LARGE));
      return false;  // 最后一句，之后不再碰任何成员
    }
    // 没有流可掐（脱开服务器的用法）：就地按失败收场，返回 false 让解析器
    // 停止交付。
    fail("上传缓冲超出上限（没有可用的背压通道）");
    return false;
  }

  j.incoming.append(data, len);
  // `pump()` 末尾会 `sync_flow()`：如果这一块让"还有字节没写出去"从假变真，
  // 流层的 `pause()` 就在这一刻下发。
  pump();
  return true;
}

void uvcpp_web_upload::on_part_end(bool truncated) {
  if (current_ == no_job()) {
    if (!discarding_ && !field_name_.empty()) {
      uvcpp_web_upload_field f;
      f.name_ = field_name_;
      f.value_ = field_value_;
      result_.fields_.push_back(f);
    }
    field_name_.clear();
    field_value_.clear();
    return;
  }

  upload_job& j = jobs_[current_];
  j.end_seen = true;
  j.meta.truncated_ = truncated;
  current_ = no_job();

  // 最后一块数据可能还在 `incoming` 里，也可能还有在途写 —— `pump()` 会一路
  // 推到 fsync/close。**这里没有任何同步等待**。
  pump();
}

void uvcpp_web_upload::notify_parse_done(bool ok) {
  if (parse_done_) return;  // 幂等：重复喊不该重复收尾

  // 全程持有一份自引用：`pump()` 可能**当场**把完成回调发出去，而那回调有权
  // 丢掉调用方手里那份 `shared_ptr` —— 没有这一份，会话会在自己的成员函数
  // 栈上被析构。
  std::shared_ptr<uvcpp_web_upload> self = shared_from_this();

  parse_done_ = true;
  if (!ok && error_.empty()) fail("报文不完整或解析失败");
  pump();
}

// -------------------------------------------------------------------------
// 失败与收尾
// -------------------------------------------------------------------------

void uvcpp_web_upload::fail(const std::string& what) {
  if (error_.empty()) error_ = what;  // 首个错误胜出
  if (discarding_) return;
  discarding_ = true;

  // 已经在等名额的话，先把等待撤掉：失败之后要跑的是"关 fd + 删文件"那几笔，
  // 它们会自己重新 acquire（拿不到就重新注册）。留着一个陈旧的注册只会让唤醒
  // 表里多一条永远不会被正确消费的条目。
  clear_wakeup();

  // 结果清零：失败时调用方拿到的必须是"什么都没有"，而不是一份指向即将被删掉
  // 的文件的列表。
  result_.clear();

  // **已经把文件交进 `result_` 的那些，必须重新排回丢弃队列。**
  //
  // 它们在成功路径上走的是 `kept` 那一支 —— 也就是**已经 `pop_front()` 出队**
  // 了。不排回来的话，下面的丢弃分支永远看不到它们，结果是：文件留在盘上，
  // 而 `result_` 刚被清空，调用方**连路径都拿不到**。那是一个没有任何人知道
  // 它存在的孤儿文件，比"删掉"和"留着并告知"都坏。
  //
  // 契约是"失败时本次创建的文件全删"（见头文件「所有权交接点」），这里就是
  // 兑现它的地方。`kept` 不在这里清 —— 它同时是析构兜底判断的依据，而
  // `removed` 置位之后析构那一支本来也不会重复动手。
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].kept) queue_.push_back(i);
  }
}

void uvcpp_web_upload::complete_if_ready() {
  if (done_ || !parse_done_ || busy_ || !queue_.empty()) return;

  // 调用方（`notify_parse_done` 或某个 fs 回调的闭包）必须持有一份自引用，
  // 见 `notify_parse_done` 里的说明。
  done_ = true;

  // 会话做完了 ⇒ 它不再需要背压，**在跑完成回调之前**松开：完成回调会
  // `release_end()` → 用户的 `on_end` → 填响应 → 收场，之后上下文可能当场被
  // 摘掉，那时再想按 id 找流对象就找不到了（`live_stream()` 会给 nullptr）。
  // 松不开也不致命 —— `uvcpp_web_app::context_finished()` 有一条兜底的不变式。
  sync_flow();

  invoke_done_callback();
}

void uvcpp_web_upload::invoke_done_callback() {
  if (!done_cb_) return;
  // **先换到本地再调** —— 与 `uvcpp_fs` 的 36 处内联回调同一手法：回调里若
  // 重新装了 `done_cb_`，给成员赋值会析构掉正在执行的那个闭包。
  done_callback cb;
  cb.swap(done_cb_);
  cb(result_, error_.empty(), error_);
}

// -------------------------------------------------------------------------
// 背压与工作池名额
// -------------------------------------------------------------------------

size_t uvcpp_web_upload::buffered_bytes() const {
  size_t n = 0;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    n += jobs_[i].pending.size() + jobs_[i].incoming.size();
  }
  return n;
}

bool uvcpp_web_upload::backpressure_needed() const {
  // 收完了就该把读放开：失败路径上连接本来就要关（`abort()` 设了
  // `connection: close`），而"会话终态还要求暂停"只会把收尾拖住。
  if (done_) return false;
  if (waiting_slot_) return true;
  if (busy_) return true;
  // 字节攒着没能写出去 ⇒ 就是我们跟不上，别再往里灌了。
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (!jobs_[i].pending.empty() || !jobs_[i].incoming.empty()) return true;
  }
  return false;
}

void uvcpp_web_upload::sync_flow() {
  const bool want = backpressure_needed();
  // **只在跳变时下发。** 每块数据都调一次 `pause()` 不只是浪费：`uvcpp_tcp_client`
  // 的 `read_pause`/`read_resume` 各自都要走一次 libuv 调用，而"暂停中再暂停"
  // 这种状态本身没有意义 —— 真正需要的是"现在要停"和"现在可以继续了"两个事件。
  if (want == flow_paused_) return;
  flow_paused_ = want;

  // 拷到栈上再调：回调会进流层，而流层有可能一路走到本会话的收尾（比如
  // `abort`），那时 `flow_` 已经不是我们能指望的成员了。
  ::std::function<void()> fn = want ? flow_.pause : flow_.resume;
  if (!fn) return;  // 没注入流控：没有背压，只有 `max_pending_` 这道兜底

  if (want) ++pause_events_;
  fn();
}

bool uvcpp_web_upload::acquire_slot() {
  if (work_limit_ == nullptr) return true;
  if (slot_held_) return true;  // 已经持有（不该发生，防御性）

  if (work_limit_->acquire()) {
    slot_held_ = true;
    waiting_slot_ = false;
    return true;
  }

  // 拿不到名额。**这里不排队**：排队意味着把任务攒在本会话里，而线程池
  // 已经满了 —— 攒下去只是把压力换个地方堆。能退的压力就退回去。
  waiting_slot_ = true;
  if (wakeup_id_ == uvcpp_web_work_limit::INVALID_WAKEUP) {
    // 捕 `weak_ptr` 而不是 `shared_ptr`：唤醒表由 `work_limit_` 持有，捕强引用
    // 就成了 `work_limit → 回调 → 会话`，而会话要等 I/O 跑完才析构 —— 一个
    // 一直等不到名额的会话会永远留在表里（也正是最可能发生的那种）。
    ::std::weak_ptr<uvcpp_web_upload> w(shared_from_this());
    wakeup_id_ = work_limit_->add_wakeup([w]() {
      ::std::shared_ptr<uvcpp_web_upload> self = w.lock();
      if (self) self->on_slot_available();
    });
  }
  return false;
}

void uvcpp_web_upload::release_slot() {
  if (!slot_held_) return;
  slot_held_ = false;
  if (work_limit_ != nullptr) {
    // `release()` 里可能**同步**回调我们的 `on_slot_available()`（那就是唤醒
    // 的整个意义）。所以这一句之后不能再碰成员 —— 见 `.h` 里 `flow_` 的警告。
    work_limit_->release();
  }
}

void uvcpp_web_upload::clear_wakeup() {
  if (work_limit_ != nullptr &&
      wakeup_id_ != uvcpp_web_work_limit::INVALID_WAKEUP) {
    work_limit_->remove_wakeup(wakeup_id_);
  }
  wakeup_id_ = uvcpp_web_work_limit::INVALID_WAKEUP;
  waiting_slot_ = false;
}

void uvcpp_web_upload::on_slot_available() {
  // 结算上一步的等待状态。条目本身**已经**被 `work_limit_` 摘掉了（唤醒是
  // 一次性的），所以这里只需清自己的账 —— 若下面这一次仍拿不到名额，
  // `acquire_slot()` 会重新注册。
  wakeup_id_ = uvcpp_web_work_limit::INVALID_WAKEUP;
  waiting_slot_ = false;
  pump();
}

// -------------------------------------------------------------------------
// 串行调度
// -------------------------------------------------------------------------

void uvcpp_web_upload::pump() {
  // `pump_step()` 返回 true = "队列动了，再跑一圈"。
  while (pump_step()) {
  }
  // **唯一**的背压出口，见头文件里的说明。
  sync_flow();
}

bool uvcpp_web_upload::pump_step() {
  // 一次只允许一笔在途：`uvcpp_fs` 只有一个 `uv_fs_t`，二次提交会拿到
  // `UV_EALREADY` 且**不会**有回调，那是永久挂起。
  if (busy_) return false;

  if (queue_.empty()) {
    complete_if_ready();
    return false;
  }

  const size_t idx = queue_.front();
  upload_job& j = jobs_[idx];

  if (discarding_) {
    // 只关和删，不再写。已经写完的（包括 `kept` 的）同样要删 —— 失败是整次
    // 上传的失败，`fail()` 已经把 `result_` 清空了，那份文件已经没有任何人
    // 知道它的存在。
    //
    // 收尾的这几笔同样要走工作池名额：它们也是线程池任务，不占名额就等于在
    // 池子已经满的时候还能无限地插队（而失败收尾恰恰是最容易成批发生的）。
    if (j.fd_valid && !j.closed) {
      if (!acquire_slot()) return false;
      submit_close(idx);
      return false;
    }
    if (j.fd_valid && !j.removed) {
      if (!acquire_slot()) return false;
      submit_unlink(idx);
      return false;
    }
    queue_.pop_front();
    return true;
  }

  // open 失败过的 job：不再重试。正常走不到这里（`fail()` 同时置了
  // `discarding_`），这是防御性的第二道 —— 少了它，open 失败会变成
  // "重新 open、再失败、再 open"的死循环。
  if (j.failed) {
    queue_.pop_front();
    return true;
  }
  if (!j.fd_valid) {
    if (!acquire_slot()) return false;
    submit_open(idx);
    return false;
  }
  if (!j.pending.empty()) {
    if (!acquire_slot()) return false;
    submit_write(idx);
    return false;
  }
  if (!j.incoming.empty()) {
    j.pending.swap(j.incoming);
    j.incoming.clear();
    if (!acquire_slot()) return false;
    submit_write(idx);
    return false;
  }
  if (j.end_seen && !j.closed) {
    // fsync 关掉时这一步直接落到 close。
    if (!acquire_slot()) return false;
    submit_fsync(idx);
    return false;
  }
  if (j.closed) {
    j.kept = true;
    result_.files_.push_back(j.meta);
    queue_.pop_front();
    return true;
  }
  // 部件还没结束 —— 等下一块数据。
  return false;
}

// -------------------------------------------------------------------------
// 提交
//
// 每个 submit 都先置 `busy_`，再按值捕一份 `shared_from_this()` 进完成回调。
//
// **同步失败必须管**：`uv_fs_*` 的异步形态在参数非法时**同步**返回非零，此时
// libuv 根本没有排队、回调永远不来 —— 只置 `busy_` 而不看返回值，会话就永久
// 挂起（完成回调再也不发），这是最坏的一种失败。所以每个 submit 都检查返回值，
// 非零就当作"这笔操作失败了"，直接走完成路径。
//
// 代价如实记：`uvcpp_fs` 在同步失败时**已经把回调存进成员**了（见
// `uvcpp_fs::open` 里 `if (rc != 0) async_pending_ = false;` 那一支 ——
// `fs_open_cb` 留在原地），而那份闭包按值捕了 `shared_from_this()`。所以若这次
// 提交恰好是**最后一次**提交，就留下一个断不开的自引用环（内存不回收）。
// 不在这里 `fs_.reset()` 去清它，是因为那些回调可能正跑在 `uvcpp_fs` 自己的
// `callback_*(uv_fs_t* req)` 栈上，而 `req` 就是会被释放的那个 `uv_fs_t` ——
// 用一个内存泄漏换一次 use-after-free 不划算。libuv 1.51 的 `uv_fs_*` 异步形态
// 实际只在 `INIT` 宏的参数校验上返回非零，走到这里需要参数非法，正常路径不可达。
// -------------------------------------------------------------------------

void uvcpp_web_upload::submit_open(size_t idx) {
  upload_job& j = jobs_[idx];
  busy_ = true;
  std::shared_ptr<uvcpp_web_upload> self = shared_from_this();
  // `path_` 是 job 的成员，活到会话结束；libuv 自己也会复制一份路径。
  const int rc = fs_->open(loop_, j.meta.path_.c_str(), k_open_flags, k_open_mode,
                           [self, idx](uvcpp_fs* f) {
                             self->on_open_done(
                                 idx, static_cast<long long>(f->get_result()));
                           });
  if (rc != 0) on_open_done(idx, rc);
}

void uvcpp_web_upload::submit_write(size_t idx) {
  upload_job& j = jobs_[idx];
  busy_ = true;
  // `&s[0]` 而不是 `s.data()`：C++11 下只有前者在形式上保证可写（`data()`
  // 返回 `const char*`，用 `const_cast` 剥掉才可写，那是靠 C++11 的连续存储
  // 保证在撑，不如直接用带下标的写法）。调用点保证 `pending` 非空。
  uv_buf_t b = uv_buf_init(&j.pending[0],
                           static_cast<unsigned int>(j.pending.size()));
  std::shared_ptr<uvcpp_web_upload> self = shared_from_this();
  // `-1` = 用当前文件偏移（追加语义）。不用显式偏移是因为本会话对每个文件
  // 只有一条串行的写链，偏移天然就是"已写入的字节数"。
  const int rc = fs_->write(loop_, as_uv_file(j.fd), &b, 1, -1,
                            [self, idx](uvcpp_fs* f) {
                              self->on_write_done(
                                  idx, static_cast<long long>(f->get_result()));
                            });
  if (rc != 0) on_write_done(idx, rc);
}

void uvcpp_web_upload::submit_fsync(size_t idx) {
  upload_job& j = jobs_[idx];
  if (!fsync_) {
    submit_close(idx);
    return;
  }
  busy_ = true;
  std::shared_ptr<uvcpp_web_upload> self = shared_from_this();
  const int rc = fs_->fsync(loop_, as_uv_file(j.fd), [self, idx](uvcpp_fs* f) {
    self->on_fsync_done(idx, static_cast<long long>(f->get_result()));
  });
  if (rc != 0) on_fsync_done(idx, rc);
}

void uvcpp_web_upload::submit_close(size_t idx) {
  upload_job& j = jobs_[idx];
  busy_ = true;
  std::shared_ptr<uvcpp_web_upload> self = shared_from_this();
  const int rc = fs_->close(loop_, as_uv_file(j.fd), [self, idx](uvcpp_fs* f) {
    self->on_close_done(idx, static_cast<long long>(f->get_result()));
  });
  if (rc != 0) on_close_done(idx, rc);
}

void uvcpp_web_upload::submit_unlink(size_t idx) {
  upload_job& j = jobs_[idx];
  busy_ = true;
  // 按值取路径：`pump()` 之后不能再碰成员，而路径本来就要在回调期间活着。
  const std::string path = j.meta.path_;
  std::shared_ptr<uvcpp_web_upload> self = shared_from_this();
  const int rc = fs_->unlink(loop_, path.c_str(), [self, idx](uvcpp_fs* f) {
    self->on_unlink_done(idx, static_cast<long long>(f->get_result()));
  });
  if (rc != 0) on_unlink_done(idx, rc);
}

// -------------------------------------------------------------------------
// 完成回调
//
// 每一个都遵守同一条纪律：**最后一句是 `pump()`，之后不再碰任何成员**。
// `pump()` 可能把完成回调发出去，而那个回调有权丢掉最后一份 `shared_ptr` ——
// 也就是说会话可能就析构在 `pump()` 里面。（闭包自己持有的那份引用保证它活到
// 本函数返回，但"返回之后"没有任何保证。）
// -------------------------------------------------------------------------

void uvcpp_web_upload::on_open_done(size_t idx, long long rc) {
  busy_ = false;
  release_slot();
  upload_job& j = jobs_[idx];

  // `O_EXCL` 撞名 ⇒ **重摇名字再开一次**。
  //
  // 这一支必须**不能**置 `j.failed`：`pump_step()` 里有一条防御性的
  // `if (j.failed) { queue_.pop_front(); return true; }`（它挡的是"open 失败 →
  // 重试 → 再失败"的死循环），置了就再也开不了第二次。重摇之后 `fd_valid`
  // 仍为假，正好让 `pump_step()` 走"提交一次 open"那条正常分支 —— 这就是
  // 下面直接 `pump()` 而什么都不用额外标注的原因。
  //
  // 重试上限是必需的：`name_gen_` 是**用户代码**，一个恒定返回同一个名字的
  // 生成器会让这里无限循环。超限就按 open 失败收场（而不是继续摇）。
  if (rc == UV_EEXIST && j.open_attempts < k_open_retries) {
    ++j.open_attempts;
    if (!reroll_path(j)) {
      j.failed = true;
      fail("落盘名生成器返回了不安全的叶子名");
      pump();
      return;
    }
    pump();
    return;
  }

  if (rc < 0) {
    j.failed = true;
    fail(std::string("打开上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
  } else {
    j.fd = static_cast<int>(rc);
    j.fd_valid = true;
  }
  pump();
}

bool uvcpp_web_upload::reroll_path(upload_job& j) {
  const std::string leaf = name_gen_ ? name_gen_(j.meta.original_filename_)
                                     : std::string();
  std::string path;
  if (!resolve_leaf(leaf, path)) return false;
  j.meta.path_ = path;
  return true;
}

bool uvcpp_web_upload::resolve_leaf(const std::string& leaf,
                                    std::string& out) const {
  if (!is_safe_leaf(leaf)) return false;

  // **两侧必须是同一个基准。** `upload_dir_` 是调用方写的那串字符，可以是相对
  // 路径（头文件里的例子就是 `"./uploads"`），而 `upload_dir_real_` 是它的真实
  // 绝对路径。拿前者拼出的候选去跟后者比前缀**必然不相等**，于是每一次上传都
  // 被判成"生成器返回了不安全的叶子名"—— 一个长得像安全告警、实际是路径基准
  // 不一致的假失败（这个坑是本步第一次跑用例时撞出来的：`single_chunk` 全红，
  // 而报的是安全错误）。
  //
  // 基底用 `real` 同时也是**缺陷面更小**的那一侧：它在这里解析过一次就不再变
  // （`create()` 的注释写了为什么不能每部件 realpath），而相对路径的解析结果
  // 依赖进程当前工作目录 —— 那是个随时会变的量，不该参与安全判断。
  //
  // 这条包含判断**逻辑上永真**（叶子由 `is_safe_leaf()` 保证不含任何分隔符，
  // 所以拼出来的一定是 `real` 的直接子项）。写成判断不是因为它可能为假，而是
  // 为了让这条不变式显式、可测 —— 头文件里"三道防线"那一节就是这么记的。
  const std::string path = join_path(upload_dir_real_, leaf);
  if (!web_is_within_root(upload_dir_real_, path)) return false;
  out = path;
  return true;
}

void uvcpp_web_upload::on_write_done(size_t idx, long long rc) {
  busy_ = false;
  release_slot();
  upload_job& j = jobs_[idx];
  if (rc < 0) {
    j.failed = true;
    fail(std::string("写入上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
  } else {
    const size_t n = static_cast<size_t>(rc);
    j.written += n;
    j.meta.size_ = j.written;
    // 短写：`uv_fs_write` 不保证写满，剩下的下一次接着写。本平台上 4 MiB 一次
    // 提交就写完了（`fs_async_write_func.cpp` 的 `large_payload` 有实测记录），
    // 所以这条路径**没有被本平台的用例覆盖** —— 但契约要求它存在。
    j.pending.erase(0, n);
    if (j.pending.empty() && !j.incoming.empty()) {
      j.pending.swap(j.incoming);
      j.incoming.clear();
    }
  }
  pump();
}

void uvcpp_web_upload::on_fsync_done(size_t idx, long long rc) {
  busy_ = false;
  upload_job& j = jobs_[idx];
  if (rc < 0) {
    release_slot();
    j.failed = true;
    fail(std::string("fsync 上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
    pump();
    return;
  }

  // **这一支刻意不归还名额。** 名额的记账单位是"一个 `busy_` 窗口"，而
  // fsync→close 是同一个窗口：`submit_fsync()` 在 `fsync_` 关掉时会直接落到
  // `submit_close()`，两笔共用一个在途标志。若在这里归还，紧接着的 close 就是
  // "没有 acquire 却 release"—— 名额会凭空变多，闸门被悄悄关小，而唯一的症状
  // 是上传比预期更早地开始暂停。归还落在 `on_close_done()` 里，一次窗口一次。
  //
  // `j` 的引用由 `submit_close` 重新取，这里不必再碰。
  submit_close(idx);
}

void uvcpp_web_upload::on_close_done(size_t idx, long long rc) {
  busy_ = false;
  release_slot();
  upload_job& j = jobs_[idx];
  j.closed = true;
  if (rc < 0 && !discarding_) {
    // close 失败时 fd 已经不由我们掌握了，`closed` 仍然置位（否则析构会
    // 再关一次同一个 fd）。文件按失败处理：删。
    j.failed = true;
    fail(std::string("关闭上传文件失败: ") + uv_strerror(static_cast<int>(rc)));
  }
  pump();
}

void uvcpp_web_upload::on_unlink_done(size_t idx, long long rc) {
  busy_ = false;
  release_slot();
  upload_job& j = jobs_[idx];
  // 删不掉**不**改变失败原因 —— 会话早就因为别的原因失败了，用"删除失败"
  // 覆盖掉真正的病因只会让排查变难。这里只记账。
  (void)rc;
  j.removed = true;
  pump();
}

}  // namespace uvcpp
