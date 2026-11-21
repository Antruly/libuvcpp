/**
 * @file src/webapp/uvcpp_ws_rooms.h
 * @brief WebSocket 房间与广播：join/leave、按房间群发、慢客户端策略。
 * @author zhuweiye
 * @version 1.0.0
 *
 * `uvcpp_web_app` 之前只有单条会话的 `send_text()`；群发要业务自己维护
 * `room → 会话` 的表，而"会话什么时候没了"没有可靠的摘除点（`on_close` 是
 * 单槽、且只在对端走人时报告）。本类把这三件事收在一起：
 *
 * ```cpp
 * uvcpp::uvcpp_ws_rooms rooms;          // 归 app 所有，活得比所有会话久
 *
 * app.websocket("/chat/:room", [&](uvcpp::uvcpp_web_ws_request& ws_req) {
 *   uvcpp::uvcpp_ws_connection* c = ws_req.connection();
 *   rooms.join(c, *ws_req.param("room"));
 *   c->on_text([&rooms](const std::string& msg) {
 *     rooms.broadcast_text("chat", msg.data(), msg.size());  // 示例：按房间广播
 *   });
 * });
 * ```
 *
 * ## 三条语义
 *
 * 1. **成员资格随会话终结自动摘除。** 本类在 `join()` 时给会话挂一个
 *    **终结观察者**（`uvcpp_ws_connection::add_close_observer()`，可叠加，
 *    任何一条终结路径都恰好触发一次）——对端 Close、异常断开、协议错误、
 *    本端 `close()`、服务器停机，都会把这条会话从它加入过的所有房间摘掉。
 * 2. **广播不保证送达。** WebSocket 是可靠流，但本类的慢客户端策略可能
 *    **主动丢弃**一条消息或**关闭**那条连接：一个不读数据、队列涨到上限的
 *    客户端不该拖着整场广播一起慢。
 * 3. **只在 app 的循环线程上调用。** 房间表没有锁；WS 回调本来就在那条线程
 *    上。要从别的线程广播，用 `app.post()` 投回来。
 *
 * ## 不做的事（如实列出）
 *
 * - **跨进程**：只看得见本进程本对象里的会话（跨进程用 Redis pub/sub 另接，
 *   见 issue #74）；
 * - **h2/h3 extended CONNECT 的会话**：那两条腿的会话类型是
 *   `uvcpp_web_ws_session`，目前还没有同形的终结观察者接口，本类只接 h1；
 * - **心跳 / ping-pong 保活**：`uvcpp_ws_connection` 已会自动回 PONG，主动探测
 *   留给业务（一个定时器即可）；
 * - **升级鉴权**：在 `websocket()` 的处理器里先判 Cookie / 头再做升级流程之外
 *   的事，不在本类里。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WS_ROOMS_H
#define SRC_WEBAPP_UVCPP_WS_ROOMS_H

#include <uvcpp/uvcpp_config.h>
#include <uvcpp/uvcpp_export.h>

#if UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE

#include <cstddef>
#include <memory>
#include <string>

namespace uvcpp {

class uvcpp_ws_connection;

/// 慢客户端的处置方式（队列超过 `max_queue_bytes` 时）。
enum class uvcpp_ws_slow_client : int {
  /// 跳过这一条消息（其他成员照常收）。默认 —— 最不具破坏性。
  DROP = 0,
  /// 关掉这条连接（1001 GOING_AWAY）。适合"消息必须完整"的场合。
  CLOSE = 1,
};

/// 一次广播的旋钮。显式构造函数（C++11 聚合初始化资格）。
struct UVCPP_API uvcpp_ws_broadcast_options {
  uvcpp_ws_broadcast_options();

  /**
   * @brief 单条连接的发送积压上限（字节，含在途那批）。默认 **1 MiB**。
   *
   * 超过它才触发 `policy`。设 0 = 不检查（**不建议**：慢客户端会把内存拖到
   * 无限，本类的意义就没了）。
   */
  size_t max_queue_bytes;

  uvcpp_ws_slow_client policy;
};

/// 累计观测（无锁快照；只在循环线程上读，没有并发问题）。
struct UVCPP_API uvcpp_ws_room_stats {
  uint64_t broadcasts = 0;     ///< 调用广播的次数
  uint64_t delivered = 0;      ///< 成功交给会话的次数（一条消息 × 一个成员）
  uint64_t dropped = 0;        ///< 因慢客户端 / 已关闭而跳过的次数
  uint64_t closed_slow = 0;    ///< 因慢客户端被关掉的连接数
  size_t   rooms = 0;          ///< 当前房间数
  size_t   members = 0;        ///< 当前成员连接数（去重）
};

/**
 * @brief 房间表 + 广播。见文件头。
 *
 * **生命周期**：本对象必须活得比所有加入过它的会话久（析构时会摘掉自己挂的
 * 终结观察者；那时连接仍在也可以安全析构）。
 */
class UVCPP_API uvcpp_ws_rooms {
 public:
  uvcpp_ws_rooms();
  ~uvcpp_ws_rooms();

  uvcpp_ws_rooms(const uvcpp_ws_rooms&) = delete;
  uvcpp_ws_rooms& operator=(const uvcpp_ws_rooms&) = delete;

  // ---- 房间 ----

  /// 加入房间。重复加入是幂等成功。空房间名 / 空连接 → `UV_EINVAL`。
  int join(uvcpp_ws_connection* c, const std::string& room);

  /// 离开房间（不在里面也是成功）。
  int leave(uvcpp_ws_connection* c, const std::string& room);

  /// 离开所有房间，返回摘掉的房间数。
  size_t leave_all(uvcpp_ws_connection* c);

  size_t room_size(const std::string& room) const;
  size_t room_count() const;
  /// 成员连接数（同一条连接加入多个房间只算一次）。
  size_t member_count() const;

  // ---- 广播 ----

  size_t broadcast_text(const std::string& room, const char* data, size_t len,
                        const uvcpp_ws_broadcast_options& opts =
                            uvcpp_ws_broadcast_options(),
                        size_t* dropped_out = nullptr);

  size_t broadcast_binary(const std::string& room, const void* data,
                          size_t len,
                          const uvcpp_ws_broadcast_options& opts =
                              uvcpp_ws_broadcast_options(),
                          size_t* dropped_out = nullptr);

  /// 向所有成员广播（不区分房间）。
  size_t broadcast_all_text(const char* data, size_t len,
                            const uvcpp_ws_broadcast_options& opts =
                                uvcpp_ws_broadcast_options(),
                            size_t* dropped_out = nullptr);

  uvcpp_ws_room_stats stats() const;

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE
#endif  // SRC_WEBAPP_UVCPP_WS_ROOMS_H
