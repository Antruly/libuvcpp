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
 *  - 实时更新：每次请求**在线程池里**对目标文件做一次 stat，一旦 mtime/大小
 *    变化即触发异步重载，文件改动后下一个请求立刻返回最新内容；
 *  - 缓存：命中且未变化的文件直接从内存返回，零磁盘 IO；
 *  - 不阻塞事件循环：stat 与读盘都通过 uvcpp_work 丢到 libuv 线程池，
 *    读完后在事件循环线程回调里更新缓存并通过 deferred 响应发送。
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
 * 要 Range/206、ETag/304、If-Range、可配 MIME 表、带容量上限的 LRU 缓存，
 * 用 `webapp/uvcpp_web_static.h` 里的 `uvcpp_web_static`。**新代码建议直接用
 * 那个** —— 它跑在框架的连接登记表上，异步完成回调不需要担心连接对象被回收。
 * 本类保留给"只想要一个兜底静态处理器、不想引入整个框架"的场合。
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

  /** @brief 是否启用内存缓存，默认 true。 */
  void set_cache_enabled(bool enable) { cache_enabled_ = enable; }

  /** @brief 当前缓存条目数。 */
  size_t cache_size() const { return cache_.size(); }

  /** @brief 清空缓存。 */
  void clear_cache() { cache_.clear(); }

private:
  struct CacheEntry {
    std::shared_ptr<std::string> data;
    std::string mime;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t size;
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
  void stat_async(std::string fs_path, std::string url_path,
                  std::string spa_fs_path, std::string spa_url_path,
                  int step, const uvcpp_http_reply& reply,
                  uvcpp_http_server* server);

  /** @brief stat 完成后的分派：目录 → 拼 index 再来一次；不存在 → SPA 回退；否则发。 */
  void stat_step(StatTask* task);

  /** @brief 异步读盘：线程池读文件，loop 线程更新缓存 + deferred 发送。 */
  void load_async(const std::string& fs_path, const std::string& cache_key,
                  int64_t msec, int64_t mnsec, int64_t size,
                  const uvcpp_http_reply& reply, uvcpp_http_server* server);

  // 「这条连接还是当初那条吗」那条判据搬进了 `uvcpp_http_reply` 自己
  // （`uvcpp_http_server::reply_alive()`）：它要的那两样 —— 代次号与"在哪张表里
  // 查" —— 正是凭据随身带着的东西，于是本类不必再自己存一遍、也不必再判一次
  // `client == nullptr`（h3 那条腿上它恒为真）。

  static std::string mime_of(const std::string& path);

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

  /**
   * @brief 内存缓存，**受 `mu_` 护**。
   *
   * 从前它是"循环线程只有一条"写的（无锁）。多循环下两个循环线程同时
   * `cache_.find()` 与 `cache_[k] = e`（后者是插入，会动红黑树结构）就是
   * 并发写同一张 `std::map`。
   */
  std::map<std::string, CacheEntry> cache_;

  /**
   * @brief 待回收的 `uvcpp_work`，**按循环分桶**（见 @ref retire）。
   *
   * **表本身**（增删桶）受 `mu_` 护 —— 两条循环同时 `retired_[l]` 就是往同一张
   * `std::map` 上并发写。桶里的 `delete` 在锁外做。
   */
  std::map<uvcpp_loop*, std::vector<uvcpp_work*> > retired_;

  /**
   * @brief 护 `cache_` 与 `retired_` 的表结构。
   *
   * **锁，不是分片**，理由与 `uvcpp_web_static` 那份相同：分片会让每片各有一份
   * 缓存，内存成倍而命中率反而掉（同一条路径的请求落到不同片上互相看不见）。
   * 单循环下这把锁没有可争用的对象，代价是一次未争用的 `lock`/`unlock`。
   *
   * `mutable`：`serve()` 走的是非 const 路径，但 `resolve_url()` 这类只读者是
   * const —— 留成 mutable 免得将来加只读访问器时再回来改一遍。
   */
  mutable std::mutex mu_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_STATIC_SERVER_H
