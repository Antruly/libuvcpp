/**
 * @file src/webapp/uvcpp_web_static.h
 * @brief 框架原生的静态文件服务：Range/断点续传、ETag/304、LRU 缓存、安全边界。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 与 `src/web/uvcpp_static_server.{h,cpp}` 的关系
 * ----------------------------------------------
 * 那个是协议层的旧实现，本模块**不基于它**，也不打算替换它（它还有自己的
 * 使用者）。两者解决的是同一件事，但旧实现有几个结构性缺陷，本模块从设计上
 * 就不存在：
 *
 * | 旧实现 | 本模块 |
 * |---|---|
 * | 异步读盘回调持有裸 `uvcpp_tcp_client*`，地址被新连接复用就会把 A 的响应发给 B | 框架按 **连接 id** 取活连接，取不到就丢弃响应 —— 身份不是指针，"复用"这个问题不存在 |
 * | 同步 `uv_fs_stat` 在循环线程上 | 全部走 `uvcpp_work`，循环线程只做组包 |
 * | 挂载前缀 `/static` 与 `/static/` 取不到 index | 前缀与 `前缀/*path` 两条路由都注册，等价处理 |
 * | HEAD 返回 body | HEAD 与 GET 走**同一条路**（一样读盘、一样进缓存），装好 body 后由 `set_head_only(true)` 在序列化时丢掉 —— 因为 `Content-Length` 与 `Content-Encoding` 都是 HTTP 层压缩决策的结果，不给真 body 就算不出 GET 会发的长度 |
 *
 * 异步与线程模型
 * --------------
 * 每次请求**只投一次 `uvcpp_work`**，worker 线程里把「解析候选 → stat →
 * （必要时）读盘」一口气做完，用的都是标准文件 IO，**不碰任何 libuv 对象**
 * —— 这是刻意的：`uvcpp_fs` 的 `uv_fs_t` 是对象级共享的，同一个对象上不能
 * 有两个在途操作，在"每请求一次 IO"的场景里用它，要么每请求 new 一个对象，
 * 要么就得小心翼翼串行化，两条路都比直接用标准 IO 更绕。
 *
 * 读盘结果是 `std::shared_ptr<std::string>`，完成回调（跑在 loop 线程上）里
 * **按值**捕获这个 shared_ptr，所以请求上下文的生存期与它无关 —— 连接断了、
 * 请求被丢弃，数据也只是正常析构。
 *
 * 缓存
 * ----
 * LRU，同时受「条目数」和「总字节数」两个上限约束（只限条目数的话，256 个
 * 大文件就能把内存吃光）。缓存**只在 loop 线程上访问**，因此内部没有锁。
 *
 * 校验方式是把缓存条目的 `(mtime, size)` 作为"期望值"快照带进 work job：
 * worker stat 之后先比这个快照 —— 对上了就**不读盘**，直接告诉调用方"用你
 * 手里那份"；对不上才真读。所以热文件是**一次 stat、零次读盘**。
 * 代价是缓存命中也要过一次线程池，换来的是"校验一定发生在文件系统上"，
 * 而不是靠时间戳猜。
 *
 * 安全边界
 * --------
 * 两道，缺一不可：
 *   1. `web_sanitize_path()` —— 纯文本归一化，挡掉所有编码绕过
 *      （`%2e%2e%2f`、`..\\`、`C:`、NUL……）；
 *   2. `web_resolve_within_root()` —— 展开符号链接后做**真实路径**的包含
 *      判断，挡住"根目录里有个指向 C:\\Windows 的链接"。
 *
 * 只有第 1 道是**不够的**：纯文本层面 `/link/system32/x` 完全合法。
 *
 * 大文件与流式下发
 * ----------------
 * **小文件**（≤ `max_cached_file_size`，默认 1 MiB）走原路：worker 整读进
 * 内存 + LRU + `resp.body(...)` 一份拷贝。**大文件**改走
 * `uvcpp_web_file_transfer` 的分片异步读（`resp.send_file_range()`），
 * 每片交给 sink 之后就不再持有，峰值与文件大小**无关**。
 *
 * 这样做之前，任何大小的文件都是整读，于是峰值是 **2N** —— 而 LRU 的字节
 * 上限（32 MiB）意味着大文件本来也进不了缓存，整读对它没换来任何收益。
 *
 * 分片读要的是"能一片一片读出来"的载体，不是 `uv_fs_sendfile` ——
 * 后者在 Windows 上是 CRT fd 上的用户态 `_read`/`_write` 循环（写不到 libuv
 * 的连接上），在 Unix 上要求 `out_fd` 是**裸 socket fd**（TLS 过滤后的
 * `uvcpp_tcp_client` 不暴露它，且开 SSL 时字节必须走 `SSL_write`）。所以那
 * 条零拷贝路径在两个平台上都用不了，详见 `uvcpp_web_file.h` 的文件头。
 *
 * `max_file_size`（默认 64 MiB）仍然是**独立的**一道闸门：它管的是"最大的
 * 那个文件有多大"，超限回 413 且一个字节都不读；阈值管的是"从哪个大小开始
 * 不整读"。两者的判据不同，不要合并。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_STATIC_H
#define SRC_WEBAPP_UVCPP_WEB_STATIC_H

#include <cstddef>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_mime.h>
#include <webapp/uvcpp_web_work_limit.h>

namespace uvcpp {

class uvcpp_web_app;
class uvcpp_web_request;
class uvcpp_web_response;
class uvcpp_loop;

// =========================================================================
// 选项
// =========================================================================

/**
 * @brief 以 `.` 开头的路径段（`.git`、`.env`）怎么处理。
 *
 * @note 第一个枚举量叫 `HIDE` 而不是 `IGNORE`：`IGNORE` 是 Windows 头文件
 *       （`winnt.h`）里的宏，`#define IGNORE 0`，写出来会被预处理成 `0 = 0`
 *       这种语法错误 —— 而且报错位置在枚举上，看着和 Windows 毫无关系。
 *       枚举类的作用域**挡不住**宏。
 */
enum class uvcpp_web_dotfile_policy : int {
  /**
   * 当作不存在（404）。**默认**。
   *
   * 404 而不是 403 是刻意的：403 等于确认"这个文件存在，只是不给你"，
   * 对 `.env`、`.git/config` 这类探测来说，那就已经泄漏了信息。
   */
  HIDE = 0,
  /** 明确拒绝（403）。想让运维一眼看出"有人在扫"时用。 */
  DENY,
  /** 照常提供。只在文档根只放公开内容、且确实需要 `.well-known` 之类时用。 */
  ALLOW,
};

/** @brief `.well-known` 这类目录即使策略是 `HIDE` 也应当放行的白名单。 */
struct UVCPP_API uvcpp_web_static_options {
  /** @brief 目录请求时按顺序尝试的索引文件名。默认 `{"index.html"}`。 */
  std::vector<std::string> index_files;

  /**
   * @brief 找不到文件时是否回落到单页应用入口（默认 false）。
   *
   * 开了之后 `/user/42` 这种前端路由会拿到 `spa_file`（默认 `index.html`）
   * 而不是 404。**注意它只对"路径不存在"生效**：路径存在但被安全策略拒绝
   * （穿越、dotfile）时仍然按拒绝处理 —— 否则 SPA 回落会把一次探测变成一个
   * 200，安全问题就再也看不见了。
   */
  bool spa_fallback;
  std::string spa_file;

  /**
   * @brief 单个文件的大小上限，超过回 413 且**不读盘**。默认 64 MiB。
   *
   * 这是"读进内存"这个实现的必要配套：没有它，一个 2 GiB 的文件请求就能
   * 把进程内存打爆。0 = 不限（**别这么用**，除非你确定文档根里都是小文件）。
   */
  size_t max_file_size;

  /** @brief 是否发 `ETag` 并处理 `If-None-Match`。默认 true。 */
  bool etag;
  /** @brief 是否发 `Last-Modified` 并处理 `If-Modified-Since`。默认 true。 */
  bool last_modified;
  /**
   * @brief 是否支持 `Range`（断点续传 / 拖动进度条）。默认 true。
   *
   * 关掉之后不回 `Accept-Ranges`，`Range` 头被忽略（回 200 全量）。
   */
  bool range;

  /**
   * @brief `Cache-Control` 的值。空 = 不发这个头。
   *
   * 带指纹的静态资源建议 `"public, max-age=31536000, immutable"`；
   * HTML 入口建议 `"no-cache"`。**框架不替你猜** —— 它不知道哪个文件带指纹。
   */
  std::string cache_control;

  /** @brief 缓存条目数上限。0 = 关掉缓存。默认 256。 */
  size_t cache_max_entries;
  /** @brief 缓存总字节上限。0 = 不按字节限制。默认 32 MiB。 */
  size_t cache_max_bytes;

  /**
   * @brief 分流阈值：文件**大于**它就走分片读下发（`send_file_range`），
   *        不再整读进内存。默认 1 MiB。
   *
   * 两条路径的峰值内存差得很远，这是本选项存在的全部理由：
   *
   * | 大小 | 路径 | 峰值 |
   * |---|---|---|
   * | ≤ 阈值 | worker 整读 + LRU + `resp.body(...)` 一份拷贝 | ≤ 2 × 阈值 |
   * | > 阈值 | 分片异步读 + 有界滑动窗口 | ≈ 切片 + 高水位 ≈ 1.5 MiB |
   *
   * 阈值不是随手取的：LRU 的字节上限默认 32 MiB，所以**大文件今天本来就
   * 进不了缓存**（`put()` 会把上限撑爆）。也就是说整读那条路对大文件**没
   * 换来任何缓存收益**，只换来 2N 的峰值 —— 而小资源（热 CSS/JS/图标）才
   * 是缓存真正划算的地方，它们的第二份拷贝正好被阈值卡住。
   *
   * **0 = 一律走流式**（"没有文件小到值得缓存"），与 `cache_max_entries = 0`
   * （关掉缓存）是同一种读法。注意这跟本仓多数 setter 的"0 = 用默认值"
   * **相反** —— 阈值没有"未配置"这个状态，不配就是 1 MiB。
   *
   * **超过阈值不只是"缓存够不着"，是整条路都不压。** 大文件走
   * `send_file_range()` → `begin_stream()`，那条入口压根不调用
   * `apply_compression()`，所以它既没有 `content-encoding` 也没有 `vary` ——
   * 与"客户端根本没发 `Accept-Encoding`"**逐字节相同**。想让大文件也压，
   * 得改成流式 deflate（每块一个上下文），那是另一个特性。
   *
   * 真要做，**先考虑的不是流式 deflate**：构建期压好同名兄弟文件（`x.js.gz`，
   * nginx `gzip_static` 那种形状）把流式要付的三项成本一次绕开 —— 块边界不
   * 存在（整份文件）、`content-length` 已知（兄弟文件的大小）、与
   * `transfer-encoding` 没有交互。它换不来"任意大文件"（得先压好），但静态
   * 资源的可压缩主体（js / css / wasm / json）本来就是打包器产出的，那一步
   * 天然已经做过。代价是那条路**自己**得声明 `vary: accept-encoding` —— 它
   * 同样不经过 `apply_compression()`，拿不到那处声明，上面"逐字节相同"这句
   * 届时也就不再成立。
   *
   * 所以本选项实际的含义是"**从哪个大小起不压缩**"。调大它买到的是"这个
   * 尺寸以下的资源走整读 + 变体缓存"，**买不到**大文件的压缩率：调多大都
   * 不会让 2 MiB 的那个文件变小。
   */
  size_t max_cached_file_size;

  /** @brief dotfile 策略。默认 `HIDE`（404）。 */
  uvcpp_web_dotfile_policy dotfiles;

  /**
   * @brief 是否允许通过符号链接提供文件。默认 true。
   *
   * @warning 关掉它**只检查最后一段**：`root/a.link/file.txt` 里 `a.link`
   *          是指向根外的链接时，挡住它的是
   *          `web_resolve_within_root()` 的包含判断（无论本选项如何），
   *          而不是本选项。本选项管的是"目标文件本身就是个链接"。
   */
  bool follow_symlinks;

  /** @brief 文本类型是否追加 `; charset=utf-8`。默认 true。 */
  bool add_charset;

  /**
   * @brief 自定义 MIME 表。nullptr = 用 `uvcpp_web_mime_map::default_map()`。
   *
   * **不持有所有权**，调用方必须保证它活得比本选项久（通常是个静态量或
   * App 的成员）。
   */
  const uvcpp_web_mime_map* mime;

  /** @brief 显式构造函数（不用 NSDMI —— 那会破坏 C++11 聚合初始化）。 */
  explicit uvcpp_web_static_options(size_t max_file = 64u * 1024u * 1024u);
};

// =========================================================================
// 静态服务
// =========================================================================

/**
 * @brief 静态文件服务。
 *
 * 通常不直接构造，而是走 `uvcpp_web_app::serve_static()` —— 它负责建对象、
 * 注册路由、并把 `shared_ptr` 交回给你（想手动清缓存时要用）。
 *
 * 生命周期：对象被路由里的 handler **按值捕获 shared_ptr** 持有，所以只要
 * App 活着它就活着，不需要额外管理。
 */
class UVCPP_API uvcpp_web_static {
 public:
  /**
   * @param root_dir 文档根。构造时就做一次 `web_real_path()` 归一化并缓存
   *                 （每请求再算一遍是纯浪费），之后**不再回头看配置**。
   *                 解析失败（目录不存在）时不会抛异常，而是让之后每个请求
   *                 都回 500 —— 这是部署错误，用 500 让它立刻可见，
   *                 比静默 404 却发现不了要强。
   */
  uvcpp_web_static(const std::string& root_dir,
                   const uvcpp_web_static_options& opts);
  ~uvcpp_web_static();

  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_web_static)

  const uvcpp_web_static_options& options() const;

  /**
   * @brief 构造函数解析并缓存的**真实**文档根。
   *
   * 空串 = 配置错误（构造时 `web_real_path()` 失败，已打过 `ERROR`）。拿它做
   * "别的目录是不是落在本根里"这类判断 —— 典型是 `uvcpp_web_app` 拒绝把上传
   * 目录设在文档根里。**返回的是构造时那一次解析的结果，不是重新解析**：
   * 调用方本就需要一个稳定基准，再算一遍既浪费又可能因为目录被换掉而不一致。
   */
  const std::string& root_real() const;

  /** @brief 清空 LRU 缓存（例如部署了新版本之后）。**只能在 loop 线程调**。 */
  void clear_cache();

  size_t cache_entries() const;
  size_t cache_bytes() const;

  /** @brief 缓存命中/未命中计数，便于确认缓存真的在工作。 */
  unsigned long long cache_hits() const;
  unsigned long long cache_misses() const;

  /**
   * @brief 服务一个请求。**框架内部接口**，使用者请用
   *        `uvcpp_web_app::serve_static()`。
   *
   * @param loop 投递读盘任务用的循环，**必须是请求所在的那个循环**。
   *
   * 为什么要显式传 loop：handler 的签名是 `(req, resp, next)`，里面既没有
   * 上下文也没有 App，**拿不到循环**。而循环要到 `start()` 之后才存在，
   * 注册期（handler 被创建时）也拿不到 —— 所以只能在每次请求时由调用方
   * （也就是 App）从自己身上取。
   *
   * 路径从 `req.param("path")` 取通配段；没有该参数时按空路径处理
   * （挂载前缀本身 → 走索引文件）。
   */
  void serve(uvcpp_web_request& req, uvcpp_web_response& resp,
             uvcpp_web_next next, uvcpp_loop* loop);

  /**
   * @brief 装上工作池在途上限。
   *
   * 装上之后，worker 在**真正要读盘的那一步之前**先要一个名额；拿不到就回
   * **503 + `Retry-After`**。
   *
   * 取名额的位置是 `run_job()` 里那条读盘语句之前，**不是**投递任务之前 ——
   * "要不要读盘"只有 stat 完才知道（命中缓存、404 都不用读），在投递前判会
   * 把那些请求一起 503 掉。于是 503 的产出位置是 `finish_job()` 里的一个
   * `REJECTED` 分支（细节见 `uvcpp_web_static.cpp`）。
   *
   * 为什么静态路径只能拒绝、不能背压：走到这里时请求**已经整包读完**了
   * （静态路由是普通路由，body 早收完），字节不在网上，没有"让对端慢点发"
   * 这种退路。上传路径相反 —— 那里字节还在读，所以它的分工是
   * `stream->pause()`，见 `uvcpp_web_work_limit` 的说明。
   *
   * @param limit 共享持有（App 传自己的那个）；**传空 = 不限**，也就是本类
   *              原本的行为。直接构造 `uvcpp_web_static` 的使用者不调它，
   *              行为与从前完全一致。
   */
  void set_work_limit(const std::shared_ptr<uvcpp_web_work_limit>& limit);

  /** @brief 当前装着的上限对象；没装时为**空**。 */
  std::shared_ptr<uvcpp_web_work_limit> work_limit() const;

  /** @brief 因为工作池满而被回绝（503）的请求数。 */
  unsigned long long rejected_count() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_STATIC_H
