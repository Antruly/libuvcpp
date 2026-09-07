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
 *  - 实时更新：每次请求对目标文件做一次同步 stat（微秒级元数据，不读文件内容），
 *    一旦 mtime/大小变化即触发异步重载，文件改动后下一个请求立刻返回最新内容；
 *  - 缓存：命中且未变化的文件直接从内存返回，零磁盘 IO；
 *  - 不阻塞事件循环：真正的文件读盘通过 uvcpp_work 丢到 libuv 线程池，
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
 */

#pragma once
#ifndef SRC_WEB_UVCPP_STATIC_SERVER_H
#define SRC_WEB_UVCPP_STATIC_SERVER_H

#if UVCPP_WEB_ENABLE

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_define.h>
#include <web/uvcpp_http_server.h>

namespace uvcpp {

class uvcpp_loop;

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

  /** @brief 归一化 URL（剥 query、前缀匹配、防穿越、拼 index），成功返回 true。 */
  bool resolve_url(const std::string& raw_url, std::string& url_path) const;

  /** @brief 同步 stat（事件循环线程，微秒级元数据）。 */
  bool stat_file(uvcpp_loop* loop, const std::string& fs_path,
                 int64_t& msec, int64_t& mnsec, int64_t& size) const;

  /** @brief 异步读盘：线程池读文件，loop 线程更新缓存 + deferred 发送。 */
  void load_async(const std::string& fs_path, const std::string& cache_key,
                  int64_t msec, int64_t mnsec, int64_t size,
                  uvcpp_tcp_client* client, uvcpp_http_server* server);

  static std::string mime_of(const std::string& path);

  std::string local_dir_;
  std::string url_root_;
  std::string index_file_;
  bool spa_fallback_ = true;
  bool cache_enabled_ = true;
  std::map<std::string, CacheEntry> cache_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_STATIC_SERVER_H
