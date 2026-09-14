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

#if UVCPP_WEB_ENABLE

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <web/uvcpp_http_server.h>

namespace uvcpp {

class uvcpp_loop;
class uvcpp_work;

/**
 * @brief 静态文件服务：懒加载 + mtime 校验缓存 + 线程池异步读盘。
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
   */
  http_request_handler handler(uvcpp_http_server* server);

  /** @brief 手动处理一个请求（handler() 内部即调用本方法）。 */
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
   */
  void stat_async(std::string fs_path, std::string url_path,
                  std::string spa_fs_path, std::string spa_url_path,
                  int step, uint64_t generation, uvcpp_tcp_client* client,
                  uvcpp_http_server* server);

  /** @brief stat 完成后的分派：目录 → 拼 index 再来一次；不存在 → SPA 回退；否则发。 */
  void stat_step(StatTask* task);

  /** @brief 异步读盘：线程池读文件，loop 线程更新缓存 + deferred 发送。 */
  void load_async(const std::string& fs_path, const std::string& cache_key,
                  int64_t msec, int64_t mnsec, int64_t size,
                  uint64_t generation, uvcpp_tcp_client* client,
                  uvcpp_http_server* server);

  /**
   * @brief 这条连接还是当初那条吗？不是就别写。
   *
   * 光比指针不够：连接关掉后 `uvcpp_tcp_client` 会被 delete，新连接完全可能
   * 落在同一个地址上。代次号单调递增不复用，所以地址被复用时对不上号。
   */
  static bool connection_matches(uvcpp_http_server* server,
                                 uvcpp_tcp_client* client, uint64_t generation);

  static std::string mime_of(const std::string& path);

  /**
   * @brief 把用完的 `uvcpp_work` 挂起来，**下次 `serve()` 时**才真正 delete。
   *
   * 为什么不能在 after_work 回调里直接 delete：`uvcpp_work::m_after_work_cb`
   * 就是**正在执行的那个闭包本身**（`callback_after_work()` 里是
   * `work->m_after_work_cb(work, status)`），释放 `work` 等于在闭包执行到
   * 一半时抽掉它自己的存储 —— 之后任何一次对捕获变量的重新加载都是在读
   * 已释放内存。这个坑在 `webapp/uvcpp_web_static.cpp` 上实测是约 50% 概率的
   * 段错误（只在寄存器分配迫使重新加载时才炸，所以时灵时不灵）。
   *
   * 本文件两处回调都把 `delete work` 放在**最后一句**，今天恰好没炸；但那是
   * 运气而不是保证 —— 编译器换个内联策略、或者谁在末尾再加一行读捕获的
   * 代码，就变成同一个 bug。所以统一改成"挂起来、下次进 `serve()` 再删"，
   * 那时早已不在任何回调里。
   */
  void retire(uvcpp_work* w) { retired_.push_back(w); }

  /** @brief 真正释放挂起的 `uvcpp_work`。**只能在 loop 线程调**。 */
  void drain_retired();

  std::string local_dir_;
  std::string url_root_;
  std::string index_file_;
  bool spa_fallback_ = true;
  bool cache_enabled_ = true;
  std::map<std::string, CacheEntry> cache_;
  std::vector<uvcpp_work*> retired_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_STATIC_SERVER_H
