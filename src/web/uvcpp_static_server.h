/**
 * @file src/web/uvcpp_static_server.h
 * @brief 静态文件服务（内存缓存 + mtime 校验 + 线程池异步读盘）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 提供可复用的静态文件托管能力，供 uvcpp_http_server 直接挂载为兜底 handler。
 *
 * 设计要点（区别于「启动时一次性预载进内存后不再更新」的旧方案）：
 *  - 懒加载：首次请求某文件时才读盘并写入内存缓存；
 *  - 实时更新：每次请求对目标文件做一次 stat（默认在**线程池**里，见
 *    @ref set_revalidate），一旦 mtime/大小变化即触发异步重载，文件改动后
 *    下一个请求立刻返回最新内容；
 *  - 缓存：命中且未变化的文件直接从内存返回，零磁盘 IO；
 *  - 不阻塞事件循环：默认下 stat 与读盘都通过 uvcpp_work 丢到 libuv 线程池，
 *    读完后在事件循环线程回调里更新缓存并通过 deferred 响应发送。选了
 *    `uvcpp_static_revalidate::LOOP_STAT` 时**只有 stat 那一步**回到循环线程上
 *    就地做（读盘仍然走池），取舍见 @ref uvcpp_static_revalidate。
 *
 * 用法：
 * @code
 *   uvcpp_http_server server;
 *   uvcpp_static_server static_svr("frontend/dist");
 *   server.bind("0.0.0.0", 8000);
 *   // ... 注册 API 路由 ...
 *   server.on_request(static_svr.handler(&server));   // 兜底：托管静态文件
 *   server.listen();
 *   server.run(UV_RUN_DEFAULT);
 * @endcode
 *
 * 与 `uvcpp_web_static`（`src/webapp/`）的分工
 * ------------------------------------------
 * 本类是**web 层**的静态托管：直接挂在 `uvcpp_http_server` 上，只做
 * 「路径 → 文件 → 200/404」。它**不做** Range/断点续传、ETag/304、
 * 目录索引以外的协商、也不做百分号解码（`%20` 会被当成三个字面字符去找
 * 文件名 —— 这是能力边界，不是漏洞）。
 *
 * 要 Range/206、ETag/304、If-Range、可配 MIME 表，用
 * `webapp/uvcpp_web_static.h` 里的 `uvcpp_web_static`。**新代码建议直接用
 * 那个** —— 它跑在框架的连接登记表上，异步完成回调不需要担心连接对象被回收。
 * 本类保留给"只想要一个兜底静态处理器、不想引入整个框架"的场合。
 *
 * **`1.6.44-dev` 起本类也补齐了两样**（从前正是上面那句劝人换类的理由）：
 * 内存缓存**有上限**（条目数 + 字节数双重预算，默认 256 条 / 32 MiB，LRU 淘汰，
 * 见 @ref set_cache_max_entries）、并且**填 `resp.cache_tag`**（于是
 * `uvcpp_http_server` 那个压缩变体缓存对本类也生效，不再每请求在循环线程上重压
 * 一遍整份文件）。两样都只是把**已有**的机制接上 —— 契约在
 * `uvcpp_http_response::cache_tag` 那一处，本类要做的只有"把文件身份填进去"。
 *
 * 本类不做 URL 解码，所以 `%2e%2e%2f` 这类编码绕过在这里天然无效（它会变成
 * 一个名字里带百分号的目录，找不到就是 404）。防穿越靠的是**段级归一化**
 * （见 `resolve_url()`），而不是子串匹配 —— 后者既会误杀 `lib..min.js`
 * 这种合法文件名，也说不清自己到底挡住了什么。
 */

#pragma once
#ifndef SRC_WEB_UVCPP_STATIC_SERVER_H
#define SRC_WEB_UVCPP_STATIC_SERVER_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <web/uvcpp_http_server.h>

namespace uvcpp {

class uvcpp_loop;
class uvcpp_work;

/**
 * @brief 缓存校验（那次 `stat`）跑在哪条线程上。
 *
 * 与 `webapp/uvcpp_web_static.h` 里的 `uvcpp_web_revalidate` **是同一件事、
 * 同一组取舍**（那两个模块互不依赖，所以各自声明一份，行为必须一致）。
 * 语义在两种取值下**逐字相同**：每个请求都重新校验一次，文件一改，**下一个
 * 请求**就带新字节。变的只是那一次 `stat` 在哪条线程上做。
 *
 * | 档 | 校验在哪 | 缓存命中的代价 |
 * |---|---|---|
 * | `POOL` | 线程池 worker（`uvcpp_work`） | 一次池往返 + 一次 `stat` |
 * | `LOOP_STAT` | 循环线程就地 `stat` | 只有一次 `stat` |
 *
 * `LOOP_STAT` 省掉的正是池往返：实测在 h1/h2 那条腿（`uvcpp_web_static`，同一
 * 台机器、同一个二进制，只换一个环境变量）上，平台形状（`-c 256 -m 32 -D 5s`，
 * 三次取中位数）每请求用户态 CPU 从 84.571 µs 掉到 64.671 µs（−23.5%），
 * 而 `strace -f -c` 下 `statx`/req 一格不差（1.00 → 1.00，校验照样做）、
 * `futex`/req 从 4.51 掉到 0.23。读数与判据见
 * [`doc/benchmark.md`](../../doc/benchmark.md) §4.7。
 *
 * @note **只在本地盘上合算**：循环线程上那次 `stat` 是**阻塞**的 —— ext4 上
 *       1.35 µs，FUSE / 网络盘上是 46 µs 量级，后者会把同一条循环上的其它连接
 *       （h3 上还有 QUIC 自己的定时器与丢包重传）一起拖住：尾延迟涨而 rps 看着
 *       还行。盘型不明就用默认的 `POOL`。
 * @note 默认 `POOL`，即 1.6.25 之前唯一的行为。
 */
enum class uvcpp_static_revalidate : int {
  /** 校验在线程池 worker 里做。**默认**，也是 1.6.25 之前唯一的行为。 */
  POOL = 0,
  /** 校验在循环线程上直接 `stat`。本地盘上的高吞吐场景。 */
  LOOP_STAT,
};

/**
 * @brief 静态文件服务：懒加载 + mtime 校验缓存 + 线程池异步读盘。
 *
 * **多循环（`n > 1`）下可用**（1.6.8 起）。这一份的 `cache_` / `retired_` 从前
 * 与 `uvcpp_web_static` 修好之前是同一个形状：按"循环线程只有一条"写的，既没有
 * 锁也没有按循环分桶。那时的判断是"本类在本仓库里没有生产调用点（只有用例构造
 * 它），改它就是改一段没人跑、也验不了的代码"。
 *
 * 那个前提在 h3 上不成立了：`uvcpp_http_server` 没有 `serve_static()` 那种高层
 * 入口（`uvcpp_web_app` 才有），**h3 那条腿要挂静态文件只能用本类** —— 而 h3 的
 * 端点默认就扇在 `set_loops(n)` 条循环上。于是两处按 `uvcpp_web_static.cpp` 里
 * `Impl::mu_` 与 `Impl::retired` 的注释补齐。
 *
 * 判据是**入口层（h2load-h3 打 `/static/*`，同一份条目、同一台机器）的 A/B**，
 * 不是"看着加了锁"：
 *
 * | 并发 | 修前 | 修后 |
 * |---|---|---|
 * | `-c 4` | 立刻死，`0 done`（段错误落在 `drain_retired()` 的 `delete` 上） | 全 2xx |
 * | `-c 16` | 立刻死 | 16,690 / 16,690 全 2xx |
 * | `-c 64` | 立刻死 | 17,431 完成、2 失败，进程存活 |
 *
 * **下面那条功能用例不是「修前必红」的那种**，别把它当成锁还在的证据：
 * `tests/functional/web_static_server_func.cpp::multiloop_concurrency` 钉的是
 * **多循环下逐条响应的正确性**（状态码 + body 长度逐条比对）与进程存活。
 * 实测把本类的 5 处加锁再摘掉（`retire` / `drain_retired` / `drain_all_retired` /
 * 缓存写 / 缓存读）重建后重跑，4 条循环 × 8 路 × 120 与 4 条循环 × 16 路 × 300
 * **都仍然全绿**（各跑 3 次）—— 这里的并发写窗口窄，而且是 UB，不保证以段错误的
 * 形式现形。所以：**这条用例绿，说明不了锁还在**；钉住这个修复的是上表那条 A/B。
 */
class UVCPP_API uvcpp_static_server {
public:
  UVCPP_DEFINE_FUNC(uvcpp_static_server)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_static_server)

  /**
   * @param local_dir  本地静态根目录（如 "frontend/dist"）
   * @param url_root   URL 前缀（默认 "/"；如 "/static" 则只服务 /static/** 请求）
   * @param index_file 根路径/目录的索引文件（默认 "index.html"）
   */
  uvcpp_static_server(const std::string& local_dir,
                      const std::string& url_root = "/",
                      const std::string& index_file = "index.html");

  /**
   * @brief 生成兜底 handler，供 server.on_request() 直接挂载。
   * @param server 所属 HTTP 服务器（deferred 响应发送需要）。
   * @note 本对象生命周期必须不短于 server（handler 捕获了 this 指针）。
   * @note 挂在 `on_request()` 上就等于三条传输（h1 / h2 / h3）都归它管：
   *       推迟应答走的是 `req.reply` + `send_reply()`，不再依赖 `client`。
   */
  http_request_handler handler(uvcpp_http_server* server);

  /**
   * @brief 手动处理一个请求（handler() 内部即调用本方法）。
   *
   * @param client 这条请求的 TCP 连接，**h3 上传 nullptr**。本方法只在
   *        `req.reply` 无效时才用它现造一枚凭据（手工构造的请求对象走的那条
   *        老路），所以 h3 上传 nullptr 不影响它工作。
   */
  void serve(uvcpp_http_request& req, uvcpp_http_response& resp,
             uvcpp_tcp_client* client, uvcpp_http_server* server);

  /** @brief 是否启用 SPA 回退（未命中时回退 index.html），默认 true。 */
  void set_spa_fallback(bool enable) { spa_fallback_ = enable; }

  /** @brief 是否启用内存缓存，默认 true。见 @ref set_cache_max_entries。 */
  void set_cache_enabled(bool enable) { cache_enabled_ = enable; }

  /**
   * @brief 内存缓存的**条目上限**，默认 256；传 0 等于关缓存。
   *
   * `1.6.44-dev` 之前这张表是**无上限也无淘汰**的：`std::map` 只增不减，一个
   * 深目录树的站点能把整份语料驻留到进程结束。现在与 `uvcpp_web_static` 的
   * `cache_max_entries` / `cache_max_bytes` 是同一套记账（同一把尺子量有界的
   * 东西），淘汰是 LRU：**命中会提升**到队首，逐出从队尾取。
   *
   * `set_cache_enabled(false)` 与 `set_cache_max_entries(0)` 的差别只在读法：
   * 前者是"把缓存关掉"，后者是"预算为零"。两者都让本类**不查也不写**。
   */
  void set_cache_max_entries(size_t n) { cache_max_entries_ = n; }

  /** @brief 当前条目上限。 */
  size_t cache_max_entries() const { return cache_max_entries_; }

  /**
   * @brief 内存缓存的**字节上限**，默认 32 MiB；**0 = 不限字节**（只按条目数封）。
   *
   * 单份文件本身就超过这个预算时**干脆不缓存它** —— 否则它一进来就会把整个
   * 缓存冲空，然后下一次仍然装不下，退化成"每次请求都把缓存冲一遍"（这一条与
   * `uvcpp_web_static` 同策）。字节数按 \ref cache_bytes 记。
   */
  void set_cache_max_bytes(size_t n) { cache_max_bytes_ = n; }

  /** @brief 当前字节上限。 */
  size_t cache_max_bytes() const { return cache_max_bytes_; }

  /**
   * @brief 是否发盘上的预压缩孪生文件（`foo.br` / `foo.gz`），默认 **false**。
   *
   * 开了之后，一次 `/static/app.js` 在 `Accept-Encoding: br, gzip` 下取的是
   * `app.js.br`：**字节更少**，而且**一次 deflate 都不用做**。
   *
   * 为什么这件事必须在这里做、而不是靠 `uvcpp_http_server` 的实时压缩：那个
   * 是**开着**的（`compress_enabled_` 默认 true），但它按 `resp.cache_tag`
   * 缓存压好的变体 —— 从前全仓只有 `uvcpp_web_static` 填 `cache_tag`
   * （`uvcpp_web_static.cpp` 里那一行）。**`1.6.44-dev` 起本类也填**
   * （文件身份：`fs_path` + `size` + `mtime` 秒 + 纳秒），所以走本类的腿不再
   * 每个请求重压一遍整份文件 —— 一次压、之后按 `cache_tag` 查表。
   *
   * 实测（探针 `probe.cpp`，HttpArena 的 `data/static` 20 个 URI 轮转、8 条循环、
   * `h2load -p http/1.1 -c 64 -m 2 -n 8000`、
   * `Accept-Encoding: br;q=1, gzip;q=0.8`；每臂 3 次取中位数，2026-10-14 于
   * `1.6.44-dev`；同一份语料的 ext4 拷贝，理由见下）：
   *
   * | 本选项 | 填 `resp.cache_tag` | rps（中位） | 8000 请求送出字节 |
   * |---|---|---|---|
   * | 关 | 否（`1.6.44-dev` **之前**的形状） | 977.00（8.19 s） | 173,843,446 |
   * | 关 | 是（`1.6.44-dev` 起） | 66,554.63（120.20 ms） | 173,843,446 |
   * | 开（发盘上孪生） | 是 | **89,791.80**（89.10 ms） | 131,194,904 |
   *
   * 第一行是**消融臂**：同一份源码，只把 `fill_cached_response()` 里填 tag 的那
   * 一段关掉、单独链一份 `libuvcpp.so`，其余一个字节不改。有了它这张表才读得动
   * ——"关"那一栏从 977 到 66 554 这一跳（**68.1×**）全部归因于 `cache_tag`
   * （每个文件压一次、之后命中变体缓存），而**本选项自己**的贡献是
   * 66 554 → 89 792（**+34.9%**）。本类填 tag 之前，这条腿每请求都在循环线程上
   * 把整份文件 deflate 一遍：20 个文件平均 63.6 KB、最大的 `app.js` 204 800 B。
   *
   * 旧版这一节记的是「933.15 → 82 451.28（**88×**）」，那个倍率是"关**且不填**
   * tag"对"开"的比值（今天在同一装置上量是 91.9× = 68.1 × 1.349），其中大头是
   * 补上 `cache_tag`、不是本选项。**旧版的绝对值也别与上表比**：同一台机器今天
   * 整体慢得多（把 `1.6.18` 那份发布包挂上同一装置、跑同一条孪生命令 —— 那条路
   * 两版逐字节同策 —— 得 78,174 ~ 79,411 rps，而当时那一臂记的是 82 451.28），
   * 只有**同一次会话内**的比值可比。
   *
   * 字节那一栏与文件系统无关，且**不全是 brotli 省下来的**：盘上 20 个文件共
   * 1,271,603 B，其中 15 个有孪生（br 224,029 B / gz 262,407 B，br 比 gz 再少
   * 14.6%），另外 5 个 webp/woff2 没有孪生、按原样发（101,376 B）—— 即盘上
   * 「br + 原样」325,405 B 对「gz + 原样」363,783 B，br 少 10.5%。而关掉本选项时
   * 发出去的不是原文件、是**实时 deflate 出来的 gzip**：上表"关"那两行每圈
   * 20 个 URI 送 431,160 B（= 172,463,836 ÷ 400 圈），比盘上那套 `.gz` 孪生
   * （363,783 B）还大 —— 所以开这一档同时省掉「压一次」和「一份比预压更差的
   * 压缩」。
   *
   * **rps 那一栏要连同语料所在文件系统一起读**：本机 `data/static` 的原路径挂在
   * ntfs-3g（FUSE）上，一次 `stat` 45.9 µs；同一份 50 个文件原样拷到 ext4 上是
   * 1.35 µs，**34 倍**。开了本选项之后每个请求要多 stat 一次候选结尾名，于是在
   * ntfs-3g 上这条腿被 `stat` 拖垮（2026-10-13 那次只量到 32,448 rps），在 ext4
   * 上才跑出上表。上表因此取 ext4 那份拷贝；量具页 `doc/benchmark-rig.md` 记了
   * 这条。
   *
   * 挑选规则（只认 `br`/`gzip`、`q=0` 是拒绝、同 q 时 `br` 在前、`*` 兜底）与
   * 技术层的 `uvcpp_web_static` 共用同一份实现 ——
   * `http_pick_precompressed_variants()`，见 `web/uvcpp_http_common.h`。
   *
   * 默认关（与 `uvcpp_web_static_options::precompressed` 一致）：它是**对盘的
   * 一个约定**（同名 `.br`/`.gz` 是那份资源的另一份表示），不该在调用方没说要
   * 的时候悄悄生效。
   *
   * 带 `Range` 的请求**一律不取孪生**（与 `uvcpp_web_static` 同策）：范围说的是
   * 原文件的字节坐标，换一份表示之后那个坐标就不成立了。本类今天不做 Range，
   * 这一条是为了将来加它时不会漏。
   */
  void set_precompressed(bool enable) { precompressed_ = enable; }

  /** @brief 是否发预压缩孪生文件，默认 false。见 @ref set_precompressed。 */
  bool precompressed() const { return precompressed_; }

  /**
   * @brief 缓存校验（`stat`）跑在哪条线程上，默认 `POOL`。
   *
   * 两档的**语义逐字相同**（每个请求都重新校验，文件一改下一个请求就带新字节），
   * 变的只是那一次 `stat` 在哪条线程上做 —— 取舍与实测见
   * @ref uvcpp_static_revalidate，与 `uvcpp_web_static_options::revalidate` 是
   * 同一个开关。`LOOP_STAT` 要显式开，因为它把**阻塞**的一次 `stat` 放回了
   * 循环线程：本地盘上划算，FUSE / 网络盘上不划算。
   *
   * 只影响**校验**那一步：缓存未命中时的读盘仍然走线程池（读一整份文件是
   * 毫秒级的阻塞，那一条不在这里赌）。
   */
  void set_revalidate(uvcpp_static_revalidate mode) { revalidate_ = mode; }

  /** @brief 当前校验档，默认 `uvcpp_static_revalidate::POOL`。 */
  uvcpp_static_revalidate revalidate() const { return revalidate_; }

  /**
   * @brief 当前缓存条目数。
   *
   * 走 `mu_` 读（`cache_` 是一份被多循环共享的 `std::map`，裸读 `size()` 与
   * 别的循环上的插入是数据竞争）—— 语义不变，只是把那次读也放进了锁里。
   */
  size_t cache_size() const;

  /**
   * @brief 当前缓存持有的**字节数**（就是各条目那份 `std::string` 的容量之外
   *        的**长度**之和，不含 `CacheEntry` 自己的元数据）。
   *
   * 与 @ref cache_max_bytes 是同一把尺子的两端。
   */
  size_t cache_bytes() const;

  /** @brief 清空缓存（条目、LRU 次序、字节账一起归零）。 */
  void clear_cache();

private:
  struct CacheEntry {
    std::shared_ptr<std::string> data;
    std::string mime;
    /// 这份字节是哪个孪生文件来的（`""` / `".br"` / `".gz"`）。
    /// 命中那一支要靠它补 `content-encoding` —— 键里虽然有后缀，但键是用来
    /// **查**的，发响应时手上只有条目本身。
    std::string variant;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t size;
    /// 这份文件**在盘上的路径**（孪生命中时是孪生那一份）。
    ///
    /// 只为一件事存在：`fill_cached_response()` 要拿它当 `resp.cache_tag` 的
    /// 身份成分。用**文件路径**而不是 URL 是因为两个 URL 会落到同一个文件
    /// （`/` 与 `/index.html` 都归到 `<root>/index.html`），它们应当共用同一份
    /// 压缩变体 —— 键里放 URL 就会各存一份压好的字节。
    std::string fs_path;
    /// 在 @ref lru_ 里的位置（队首 = 最近使用）。**受 `mu_` 护**。
    std::list<std::string>::iterator pos;
  };

  struct LoadTask;
  struct StatTask;

  /** @brief 归一化 URL（剥 query、前缀匹配、防穿越、拼 index），成功返回 true。 */
  bool resolve_url(const std::string& raw_url, std::string& url_path) const;

  /**
   * @brief 异步取元数据：**线程池**里做 stat，loop 线程里决定下一步。
   *
   * 为什么不是同步 stat：整个库的硬性约定是「事件循环线程上不做 IO」。
   * 本地 SSD 上一次 stat 是微秒级，但网络盘/机械盘上是毫秒级 —— 而
   * "通常很快"不是保证。所以元数据和读盘走同一条路（线程池）。
   *
   * @param reply 这条请求的「稍后回话」凭据（见 @ref uvcpp_http_reply）。
   *        从前这里传的是 `(generation, client)` 两格 —— 换成凭据之后 h3
   *        那条腿也走得通了（那里没有 `uvcpp_tcp_client*` 可传）。
   */
  /// @param variants 预压缩孪生的候选后缀（有序，最多 2 个），可为 nullptr。
  ///        在 **loop 线程**上按请求头算好传进来 —— worker 不该碰 `req`。
  /// @param variant_count 候选个数，0 表示这次请求不取孪生（关卡见
  ///        @ref set_precompressed）。
  void stat_async(std::string fs_path, std::string url_path,
                  std::string spa_fs_path, std::string spa_url_path,
                  int step, const uvcpp_http_reply& reply,
                  uvcpp_http_server* server, const std::string* variants,
                  size_t variant_count);

  /** @brief stat 完成后的分派：目录 → 拼 index 再来一次；不存在 → SPA 回退；否则发。 */
  void stat_step(StatTask* task);

  /**
   * @brief 一次 stat 的**全部动作**（`stat_path` + 孪生逐级探测）。
   *
   * 只碰 \p task 自己的字段，不碰任何与"跑在哪条线程"有关的东西 —— 池里的
   * worker 与 `LOOP_STAT` 档都在跑它，两条路必须是同一套语义（否则两种档
   * 会选出不同的孪生）。函数体是从前 worker 闭包里的那段，逐字搬出来。
   */
  void run_stat_inline(StatTask* task) const;

  /**
   * @brief 按 @ref revalidate_ 把一次 stat 投出去：`POOL` 走 @ref stat_async
   *        （线程池），`LOOP_STAT` 就地做完后立刻 `stat_step()`。
   *
   * `LOOP_STAT` 那一支在**栈上**复制一份 task 再跑：真正的 `StatTask` 是
   * `stat_async` 里 `new` 出来、由 after_work 回调 `delete` 的，同步档没有
   * 那一步，用局部副本就不会有人去 delete 一块栈内存。
   */
  void stat_dispatch(StatTask* task, const std::string& fs_path,
                     const std::string& url_path,
                     const std::string& spa_fs_path,
                     const std::string& spa_url_path, int step);

  /**
   * @brief 把一份**已经填好**的响应交出去：池档发给 `task->server`，同步档只记账。
   *
   * 响应是就地填在落点上的（同步档 = 调用方那个 `resp`，池档 = task 自带的
   * `scratch`，见 `uvcpp_static_server.cpp` 里 `response_slot` 那一处的说明），
   * 所以这里**只判发不发、不搬字节**：搬一次就是每请求把整份文件白拷一遍
   * （`uvcpp_buf` 的拷贝是深拷贝）。
   *
   * 两种档**只有这一步不同**，所以收到一处来。同步档还要置 `sync_sent`：
   * `stat_step()` 走到 `load_async()` 那一支时是**不回话**的（改由池里的读盘
   * 回调回），调用者要靠这一格分辨"已经回完了"与"落到了读盘那条路"。
   */
  void deliver(StatTask* task, uvcpp_http_response& resp);

  /**
   * @brief 异步读盘：线程池读文件，loop 线程更新缓存 + deferred 发送。
   *
   * @param mime_key 取 `content-type` 用的路径 —— **永远是原文件**，不是孪生
   *        （`app.js.br` 按扩展名推出来的是 `application/octet-stream`）。
   * @param variant  选中的孪生后缀（`""` = 原文件），决定 `content-encoding`。
   */
  void load_async(const std::string& fs_path, const std::string& cache_key,
                  const std::string& mime_key, const std::string& variant,
                  int64_t msec, int64_t mnsec, int64_t size,
                  const uvcpp_http_reply& reply, uvcpp_http_server* server);

  // 「这条连接还是当初那条吗」那条判据搬进了 `uvcpp_http_reply` 自己
  // （`uvcpp_http_server::reply_alive()`）：它要的那两样 —— 代次号与"在哪张表里
  // 查" —— 正是凭据随身带着的东西，于是本类不必再自己存一遍、也不必再判一次
  // `client == nullptr`（h3 那条腿上它恒为真）。

  static std::string mime_of(const std::string& path);

  /**
   * @brief 写入一条缓存条目并提升到队首，然后按预算淘汰。**要求已持有 `mu_`。**
   *
   * 名字带 `_locked` 后缀是为了让"光看调用点就知道它不自己拿锁"这件事成立
   * （与 `uvcpp_web_static` 里 `evict_locked()` 同一个约定）。
   *
   * 三件与 `uvcpp_web_static::put()` 逐字相同的事：单份文件超过字节预算时
   * **干脆不缓存**（否则它一进来就冲空整个缓存，下一次仍然装不下）；同一个键
   * 覆盖写要把旧条目的字节与 LRU 位置一起撤掉；淘汰从**队尾**取，直到条目数与
   * 字节数都回到预算内。
   */
  void put_cache_locked(const std::string& key,
                        const std::shared_ptr<std::string>& data,
                        const std::string& mime, const std::string& variant,
                        const std::string& fs_path, int64_t msec, int64_t mnsec,
                        int64_t size);

  /** @brief 按预算从队尾淘汰。**要求已持有 `mu_`**（只从 `put_cache_locked()` 调）。 */
  void evict_locked();

  /**
   * @brief 把用完的 `uvcpp_work` 挂起来，**下次同一循环进 `serve()` 时**才真正
   *        delete。
   *
   * 为什么不能在 after_work 回调里直接 delete：`uvcpp_work::m_after_work_cb`
   * 就是**正在执行的那个闭包本身**（`callback_after_work()` 里是
   * `work->m_after_work_cb(work, status)`），释放 `work` 等于在闭包执行到
   * 一半时抽掉它自己的存储 —— 之后任何一次对捕获变量的重新加载都是在读
   * 已释放内存。这个坑在 `webapp/uvcpp_web_static.cpp` 上实测是约 50% 概率的
   * 段错误（只在寄存器分配迫使重新加载时才炸，所以时灵时不灵）。
   *
   * @param loop **投递这个 `work` 的那条循环**（`queue_work()` 的第一个实参）。
   *        分桶的理由与 `uvcpp_web_static` 里 `Impl::retired` 那份逐字相同：
   *        `delete` 的时机是"这条循环下次进 `serve()`"，合成一个桶的话，循环 0
   *        的 `serve()` 会把循环 1 上还没跑完回调的 `work` 一起删掉 —— 循环 1
   *        随后在那个已释放的对象上执行回调。分桶之后每条循环只删自己投出去的，
   *        而"同一条循环正在跑 after_work"与"同一条循环正在跑 `serve()`"互斥。
   */
  void retire(uvcpp_loop* loop, uvcpp_work* w);

  /** @brief 真正释放 \p loop 那一条循环攒下的。**只能在该循环线程上调。** */
  void drain_retired(uvcpp_loop* loop);

  /** @brief 析构用：所有桶一起排空（此后没有别的线程会往里放）。 */
  void drain_all_retired();

  /**
   * @brief 当前代码跑在哪条循环上 —— `drain_retired()` 的桶键。
   *
   * `client` 优先（h1/h2 上它就是这条请求的连接，而连接属于哪条循环是确定的）；
   * h3 上 `client` 恒为 nullptr，取凭据里存好的那一个（h3 的回调只可能在它自己
   * 那条端点的循环线程上跑）。两者都取不到（用例手工造的请求对象、又没给
   * 连接）时返回 nullptr，那时**不排空任何桶**——宁可不删，也不删错。
   */
  static uvcpp_loop* loop_of(uvcpp_tcp_client* client,
                             const uvcpp_http_reply& reply);

  std::string local_dir_;
  std::string url_root_;
  std::string index_file_;
  bool spa_fallback_ = true;
  bool cache_enabled_ = true;
  bool precompressed_ = false;  ///< 见 @ref set_precompressed，默认关
  /// 校验跑在哪条线程上，见 @ref set_revalidate，默认 `POOL`（池往返那一档）。
  uvcpp_static_revalidate revalidate_ = uvcpp_static_revalidate::POOL;

  /// 缓存预算，见 @ref set_cache_max_entries / @ref set_cache_max_bytes。
  /// 默认与 `uvcpp_web_static_options` 的两个默认值相同（256 条 / 32 MiB）。
  size_t cache_max_entries_ = 256;
  size_t cache_max_bytes_ = 32u * 1024u * 1024u;

  /**
   * @brief 内存缓存，**受 `mu_` 护**。
   *
   * 从前它是"循环线程只有一条"写的（无锁）。多循环下两个循环线程同时
   * `cache_.find()` 与 `cache_[k] = e`（后者是插入，会动红黑树结构）就是
   * 并发写同一张 `std::map`。
   */
  std::map<std::string, CacheEntry> cache_;

  /**
   * @brief LRU 次序，**队首 = 最近使用**，**受 `mu_` 护**。
   *
   * 反向索引在 `CacheEntry::pos` 里，于是"提升"是一次 `splice`（O(1)、不搬
   * 元素、失效不了别的迭代器）、"淘汰"是一次 `back()` + `pop_back()`。用
   * `std::list` 而不是"给每条记一个时间戳再扫描"：后者每次淘汰是 O(n) 且要
   * 时钟（单调时钟读一次在热路径上不是免费的）。
   */
  std::list<std::string> lru_;

  /// 当前缓存的字节数（`cache_` 里各条目 `data->size()` 之和），**受 `mu_` 护**。
  size_t cache_bytes_ = 0;

  /**
   * @brief 待回收的 `uvcpp_work`，**按循环分桶**（见 @ref retire）。
   *
   * **表本身**（增删桶）受 `mu_` 护 —— 两条循环同时 `retired_[l]` 就是往同一张
   * `std::map` 上并发写。桶里的 `delete` 在锁外做。
   */
  std::map<uvcpp_loop*, std::vector<uvcpp_work*> > retired_;

  /**
   * @brief 护 `cache_` + `lru_` + `cache_bytes_` 与 `retired_` 的表结构。
   *
   * **锁，不是分片**，理由与 `uvcpp_web_static` 那份相同：分片会让每片各有一份
   * 缓存，内存成倍而命中率反而掉（同一条路径的请求落到不同片上互相看不见）。
   * 单循环下这把锁没有可争用的对象，代价是一次未争用的 `lock`/`unlock`。
   *
   * `mutable`：`serve()` 走的是非 const 路径，但 `resolve_url()` 这类只读者是
   * const —— 留成 mutable 免得将来加只读访问器时再回来改一遍。`clear_cache()`
   * 与 `cache_bytes()` 也吃这一条（两个都是 `const` 的公开访问器）。
   */
  mutable std::mutex mu_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_STATIC_SERVER_H
