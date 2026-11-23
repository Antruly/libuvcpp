/**
 * @file src/webapp/uvcpp_ws_bus.h
 * @brief 跨进程 WebSocket 广播桥（Redis pub/sub，issue #74）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * `uvcpp_ws_rooms` 只认识**本进程**的连接；多 worker / 多实例部署时，同一条
 * 广播要发给别的进程里的客户端，就得有一个进程间通道。本类把它收成一层：
 *
 * ```cpp
 * uvcpp::uvcpp_ws_bus bus(&loop, uvcpp::uvcpp_ws_bus_options());
 * bus.attach(&rooms);                    // 收到远端消息 → 本进程按房间广播
 * bus.start("redis://127.0.0.1:6379/0");
 *
 * // 发布：先本地广播，再经 Redis 发给别的进程
 * bus.broadcast(rooms, "room:42", data, len);
 * ```
 *
 * ## 语义
 *
 * - **不回环**：消息带发布节点的 `node_id`；默认收到自己发的消息**不再回调**
 *   （`deliver_loopback=true` 可以打开，诊断用）。
 * - **降级不是失败**：Redis 不可用/断线时 `publish()` 仍然会**排队**（有界，
 *   `max_pending` 条），`broadcast()` 的本地那一半照常完成 —— 单进程广播不受
 *   影响。队列满时返回 `UV_ENOBUFS` 并把 `dropped` 加一。
 * - **断线重连**：定时探测连接；断开后按 `reconnect_ms` 起、每次翻倍到
 *   `reconnect_max_ms` 重连，成功后**重新订阅**并补发排队中的消息。
 * - **消息格式由本层定义**（二进制安全）：`node_len(4, BE) | node | seq(8, BE) |
 *   payload`；频道名是 `channel_prefix + 房间名`。业务只看见房间名与 payload。
 * - **慢订阅者**：本层不把消息攒在内存里 —— 回调是同步的，处理不过来是调用方
 *   的事；唯一的队列是"未连接时的待发队列"，它有界。
 *
 * ## 线程
 *
 * 所有成员（含 `publish` / `broadcast` / 回调）都必须在**构造时给的那条循环**
 * 的线程上调用。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WS_BUS_H
#define SRC_WEBAPP_UVCPP_WS_BUS_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_define.h>
#include <webapp/uvcpp_ws_rooms.h>

namespace uvcpp {

class uvcpp_loop;

/// `uvcpp_ws_bus` 的构造参数（显式构造函数，C++11 聚合初始化资格）。
struct UVCPP_API uvcpp_ws_bus_options {
  uvcpp_ws_bus_options();

  /// 本节点 id。空 = 自动生成（pid + 高精度时间）。
  std::string node_id;
  /// 频道前缀；房间 r 对应频道 `<prefix><r>`。
  std::string channel_prefix;
  /// 收到**本节点**发的消息时是否也回调（默认 false：不回环）。
  bool deliver_loopback;
  /// 首次重连间隔与上限（毫秒）。成功后回到首次间隔。
  int reconnect_ms;
  int reconnect_max_ms;
  /// 未连接时最多排队多少条待发消息；满了 `publish()` 返 `UV_ENOBUFS`。
  size_t max_pending;
  /// 单条消息上限（字节）；超过 `publish()` 返 `UV_EMSGSIZE`。
  size_t max_payload_bytes;
};

/// 运行观测快照。
struct UVCPP_API uvcpp_ws_bus_stats {
  uvcpp_ws_bus_stats();

  uint64_t published;         ///< 交给 Redis（或进了待发队列）的条数
  uint64_t received;          ///< 从 Redis 收到的条数（含自己的回环）
  uint64_t delivered;         ///< 真正交给回调的条数
  uint64_t loopback_skipped;  ///< 因为是本节点发的而跳过
  uint64_t dropped;           ///< 队列满 / 格式非法 / 超限丢弃
  uint64_t send_failures;     ///< Redis 命令层失败
  uint64_t reconnects;        ///< 成功重连次数（含首次成功？不含）
  bool     connected;         ///< 当前是否已连接并订阅
  bool     degraded;          ///< 连接不在（消息在排队或已丢弃）
  std::string last_error;     ///< 最近一次连接/命令失败原因
};

/**
 * @brief Redis pub/sub 的 WebSocket 广播桥。见文件头。
 */
class UVCPP_API uvcpp_ws_bus {
 public:
  explicit uvcpp_ws_bus(uvcpp_loop* loop,
                        const uvcpp_ws_bus_options& opts =
                            uvcpp_ws_bus_options());
  ~uvcpp_ws_bus();

  uvcpp_ws_bus(const uvcpp_ws_bus&) = delete;
  uvcpp_ws_bus& operator=(const uvcpp_ws_bus&) = delete;

  /**
   * @brief 连上 Redis 并按模式订阅 `<prefix>*`。
   *
   * 异步：返回 0 只表示"开始连了"；连接/订阅失败会走重连退避，状态看
   * `snapshot()`。重复调用是幂等的（已经在跑就返回 0）。
   */
  int start(const std::string& redis_url);

  /**
   * @brief 发布一条消息到房间。
   *
   * 已连接 → 立刻 `PUBLISH`；未连接 → 进待发队列（满则 `UV_ENOBUFS`）。
   * 消息本层会加上节点 id 与序号；**payload 原样保留**（二进制安全）。
   *
   * @return 0 已发出或已排队；`UV_EMSGSIZE` 超过 `max_payload_bytes`；
   *         `UV_ENOBUFS` 待发队列满；`UV_EINVAL` 房间为空。
   */
  int publish(const std::string& room, const std::string& payload);

  /** @brief 本进程收到的**远端**消息（房间名, payload）。空 = 不回调。 */
  void set_on_message(
      std::function<void(const std::string& room, const std::string& payload)>
          cb);

  /**
   * @brief 把收到的远端消息直接转给本进程的房间表（`broadcast_text`）。
   *
   * 与 `set_on_message()` 可以同时用（先回调、再转发）。传 `nullptr` 解除。
   */
  void attach(uvcpp_ws_rooms* rooms);

  /**
   * @brief 本地广播 + 跨进程发布，一次做完（与 #68 房间 API 的组合口）。
   *
   * 本地那一半**总是**执行（Redis 挂了也照常）；返回值是**本地**投递到的连接
   * 数（与 `uvcpp_ws_rooms::broadcast_text()` 一致）。`publish()` 的失败只记进
   * 统计，不改变返回值 —— 远端投递本来就不保证（pub/sub 也没有确认）。
   */
  size_t broadcast(uvcpp_ws_rooms& rooms, const std::string& room,
                   const char* data, size_t len,
                   const uvcpp_ws_broadcast_options& opts =
                       uvcpp_ws_broadcast_options());

  /** @brief 停止：断开连接、停掉定时器、清空待发队列。可再 `start()`。 */
  void stop();

  /**
   * @brief 强制断开当前连接（测试/运维用）。下一次探测会按退避重连。
   *
   * 不给应用层一个"静默断线"的入口：真正的断线路径由定时器探测 `is_connected()`
   * 发现，这条只是让那条路径可测。
   */
  void drop_connection();

  bool connected() const;
  uvcpp_ws_bus_stats snapshot() const;

  /// 本节点 id（构造时生成或配置给的那个）。
  const std::string& node_id() const;

  /// 构造时给的那条循环。
  uvcpp_loop* loop() const;

 private:
  struct impl;
  std::shared_ptr<impl> p_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEBAPP_ENABLE && UVCPP_REDIS_ENABLE
#endif  // SRC_WEBAPP_UVCPP_WS_BUS_H
