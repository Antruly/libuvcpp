/**
 * @file src/webapp/uvcpp_web_connection.h
 * @brief 连接的身份与登记表：把「裸 client 指针」换成「永不复用的 id」。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么要有这一层
 * ----------------
 * 异步 handler 的基本形态是「把活交给线程池，回调里再写响应」。写响应需要
 * 连接，而连接是个 `uvcpp_tcp_client*` —— **那个指针在异步期间可能已经失效**：
 * 对端断开，服务端把它 delete 掉，地址随后被一条新连接复用。这时回调拿着
 * 旧指针去写，数据就写到了别人的连接上。这不是理论风险：现有的
 * `uvcpp_static_server` 异步读盘那条路径就是裸指针，注释里也承认了。
 *
 * 判空挡不住它 —— 指针非空，只是指向了另一个对象。**换成 id 才挡得住**：
 *
 * @code
 *   uvcpp_web_conn_id id = registry.add(client, ip, port);   // 1, 2, 3... 绝不重复
 *   // ... 异步 ...
 *   uvcpp_tcp_client* c = registry.client(id);   // 断开过 → nullptr
 *   if (c == nullptr) { drop_response_and_log(); return; }
 * @endcode
 *
 * id 是单调递增且**永不复用**的，所以「连接 7 已经死了」这个事实不会因为
 * 新连接拿到 7 而变成谎话。登记表内部的 `by_client_` 反向索引则保证同一个
 * client 只会有一条记录。
 *
 * 线程约束
 * --------
 * 登记表**只能在 loop 线程上操作**（accept / close 都发生在那里）。异步
 * 回调要从别的线程查，得先 `context::post()` 回到 loop 线程。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_CONNECTION_H
#define SRC_WEBAPP_UVCPP_WEB_CONNECTION_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

class uvcpp_loop;
class uvcpp_tcp_client;

/**
 * @brief 一条连接的身份。
 *
 * 从 1 开始（0 是无效值），单调递增，**永不复用**。
 */
typedef uint64_t uvcpp_web_conn_id;

/** @brief 无效 / 未知的连接 id。 */
const uvcpp_web_conn_id UVCPP_WEB_INVALID_CONN_ID = 0;

/**
 * @brief 登记表里的一条连接记录。
 *
 * 只在 `uvcpp_web_connection_registry` 存活期内有效，且 `client` 只在
 * `alive` 为真时可用。
 */
struct UVCPP_API uvcpp_web_connection {
  uvcpp_web_conn_id id;
  /** @brief 底层连接。**登记表从不解引用它**，只当作身份用。 */
  uvcpp_tcp_client* client;
  std::string       peer_ip;
  int               peer_port;

  /**
   * @brief 最近一次**收到字节**的时刻（毫秒，`uv_now` 基准）。
   *
   * `0` 表示还不知道 —— 登记时没给时间戳就会这样。扫描器第一次看到这种
   * 连接时会补一个基准，而不是当成"从时间原点起就没动过"直接杀掉（那会让
   * 每一条刚建立的连接都活不过第一拍）。
   */
  int64_t last_read_ms;

  /**
   * @brief 当前这个「半截请求」的第一个字节到达的时刻；`0` = 当前没有半截请求。
   *
   * 为什么要有它：只用 `last_read_ms` 挡不住慢速攻击 —— 攻击者每 30 秒发一个
   * 字节，空闲计时就永远是新鲜的。改成「**整个请求**的预算」之后，无论中间
   * 发不发字节，这个请求都必须在超时之内收完。
   *
   * 由 `note_read()` 在这条连接上"没有半截请求"时置上，由
   * `note_request_done()` 清零（响应发出、请求结束）。
   */
  int64_t request_start_ms;

  /**
   * @brief 这条连接当前是不是**流式收体中**（body 正在按块交付给 handler）。
   *
   * 它改变的是「活动基准取哪一个」这一件事，但**必须存在**，因为两种计时方式
   * 对流式请求都不成立：
   *
   * - 沿用 `request_start_ms`（整段预算）→ 一个 2 GB 的上传即使一直在推进，
   *   也会在上限那一刻被当成慢速攻击杀掉；
   * - 不豁免（按 `last_read_ms`）→ 这正是要的，但 `idle_sweep` 对在途请求
   *   是**整体豁免**的，于是流式上传变成永远不超时。
   *
   * 所以流式期间的语义是「**停顿保护**」：只要还在收字节就不动它，停止推进超过
   * `idle_timeout_ms` 就关。由框架在认领时置真、在消息结束/中止时清掉；
   * `note_request_done()` 也会清（它是"这个请求结束了"的唯一收口）。
   */
  bool streaming;

  /// 显式构造函数，**不用 NSDMI**（那会破坏 C++11 的聚合初始化）。
  uvcpp_web_connection();
};

/**
 * @brief 连接登记表：id ↔ 活连接的映射。
 *
 * @warning 只在 loop 线程上用。内部的 `std::map` 不是线程安全的。
 */
class UVCPP_API uvcpp_web_connection_registry {
 public:
  uvcpp_web_connection_registry();
  ~uvcpp_web_connection_registry();

  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_web_connection_registry)

  /**
   * @brief 登记一条新连接，返回它的 id。
   *
   * 同一个 `client` 重复登记是**调用方的 bug**，这里不会静默吞掉：旧记录会
   * 被先摘掉（否则 `by_client_` 会指着一条失效记录），再建新的。id 因此可能
   * 出现「同一个 client 有两个 id，只有后一个有效」的情况 —— 这正是重复登记
   * 该有的表现。
   *
   * @return 新分配的 id，永不为 `UVCPP_WEB_INVALID_CONN_ID`。
   */
  uvcpp_web_conn_id add(uvcpp_tcp_client* client,
                        const std::string& peer_ip = std::string(),
                        int peer_port = 0,
                        int64_t now_ms = 0);

  // -----------------------------------------------------------------
  // 活动时间（闲置超时的依据）
  // -----------------------------------------------------------------

  /**
   * @brief 记一次「收到字节」。
   *
   * 顺便把「半截请求」的起点定下来：**如果当前没有半截请求，这次收到的字节
   * 就是新请求的第一个字节**，于是 `request_start_ms` 置为 @p now_ms。
   * 登记表不认识 HTTP 分帧，但「上一个请求答完之后的第一块字节」正好就是
   * 新请求的开头 —— 而 `note_request_done()` 已经把上一个请求清零了。
   *
   * @param now_ms `uv_now` 基准的毫秒时刻。
   * @return 记录存在并已更新。
   */
  bool note_read(uvcpp_web_conn_id id, int64_t now_ms);

  /**
   * @brief 记一次「这个请求已经答完」，`request_start_ms` 归零。
   *
   * 下一次 `note_read()` 收到的字节就属于新请求，拿一个全新的预算。
   * keep-alive 连接的第二个请求因此能重新计时。
   */
  bool note_request_done(uvcpp_web_conn_id id);

  /**
   * @brief 标记/清除这条连接的「流式收体中」状态。
   *
   * 置真之后 `activity_since()` 改用 `last_read_ms`，于是超时从「整个请求的
   * 预算」变成「**停顿**多久没进展」。`note_request_done()` 会无条件清掉，
   * 所以即使调用方漏了清，它也不会漏到下一个请求上去。
   *
   * @return 记录存在。
   */
  bool mark_streaming(uvcpp_web_conn_id id, bool streaming);

  /** @brief 这条连接是否处于「流式收体中」。记录不存在时为 false。 */
  bool is_streaming(uvcpp_web_conn_id id) const;

  /**
   * @brief 这条连接的**活动基准时刻** —— 闲置超时该从哪一刻算起。
   *
   * - 正在流式收体 → `last_read_ms`（只要还在收字节就不算超时，停下来才超时）；
   * - 正在收一个半截请求 → `request_start_ms`（按整个请求的预算算，慢速
   *   滴字节拖不过去）；
   * - 否则 → `last_read_ms`（连接之间的空闲按最后一次收字节算）。
   *
   * @return `uv_now` 基准的毫秒时刻；记录不存在、或者还没有基准时为 `0`。
   */
  int64_t activity_since(uvcpp_web_conn_id id) const;

  /**
   * @brief 给一条还不知道活动时刻的连接补基准。
   *
   * **只在 `last_read_ms` 为 0 时写**，所以可以每拍都无脑调。存在的意义
   * 是「登记时没给时间戳」（默认参数 0）的那些记录：第一次扫描时补一个
   * 基准，让它从这一刻开始算，而不是被当成上古连接当场杀掉。
   *
   * @return 记录存在。
   */
  bool touch(uvcpp_web_conn_id id, int64_t now_ms);

  /** @brief 摘掉一条记录。@return 记录存在并已摘除。 */
  bool remove(uvcpp_web_conn_id id);

  /**
   * @brief 按 client 指针摘除（对端断开时只知道指针，不知道 id）。
   *
   * @param[out] out_id 若非空，收到被摘除的 id（供日志用）。
   * @return 找到并摘除了记录。
   */
  bool remove_by_client(uvcpp_tcp_client* client,
                        uvcpp_web_conn_id* out_id = nullptr);

  /**
   * @brief id → 活连接。
   *
   * @return 连接**当前活着**时返回它的指针；已经断开、或者这个 id 压根没
   *         发出过，都返回 `nullptr`。
   *
   * **不要把这个指针存到异步回调里** —— 那正是本文件要解决的问题。要用
   * 就在 loop 线程上现查现用。
   */
  uvcpp_tcp_client* client(uvcpp_web_conn_id id) const;

  /**
   * @brief client 指针 → id（`client()` 的反向查询）。
   *
   * 给 HTTP 层的入口用：请求到达时手上只有 `uvcpp_tcp_client*`（那是 HTTP
   * 服务器给的），而框架想用 id 记账。查不到返回 0 —— 0 永远不是有效的
   * 连接 id（id 从 1 开始发）。
   */
  uvcpp_web_conn_id id_of(uvcpp_tcp_client* client) const;

  /** @brief 这条连接现在是否还活着。 */
  bool alive(uvcpp_web_conn_id id) const;

  /**
   * @brief 这个 id 是否**曾经**发出过（不管现在是不是还活着）。
   *
   * 用来区分两种 `client() == nullptr`：「连接已经断了」（正常，记一条
   * debug 就够了）和「这个 id 根本不存在」（框架自己的 bug，要报 error）。
   * 靠 id 单调递增实现，不需要额外存一张"死 id 表"。
   */
  bool issued(uvcpp_web_conn_id id) const;

  /** @brief 取一条记录的副本。不存在返回 nullptr。 */
  const uvcpp_web_connection* find(uvcpp_web_conn_id id) const;

  /** @brief 当前活连接数。 */
  size_t size() const;

  /** @brief 当前所有活连接的 id（顺序按 id 升序）。 */
  std::vector<uvcpp_web_conn_id> ids() const;

  /** @brief 历史上分配过的 id 总数（含已断开的）。 */
  uint64_t issued_count() const;

  /** @brief 清空登记表。**已经发出去的 id 不会被回收**，下一个 id 接着涨。 */
  void clear();

 private:
  std::map<uvcpp_web_conn_id, uvcpp_web_connection> by_id_;
  std::map<uvcpp_tcp_client*, uvcpp_web_conn_id>    by_client_;
  uvcpp_web_conn_id next_id_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_CONNECTION_H
