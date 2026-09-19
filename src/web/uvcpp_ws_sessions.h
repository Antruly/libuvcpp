/**
 * @file src/web/uvcpp_ws_sessions.h
 * @brief WebSocket 会话的归属表：谁持有会话、什么时候回收。
 * @author zhuweiye
 * @version 1.0.0
 *
 * `uvcpp_ws_connection` 是**托管型**对象：它不自持生命周期，终结时把自身交回
 * 属主（见 `uvcpp_ws_connection::set_retire_callback`）。本类就是服务端与
 * 客户端共用的那个"属主"：接管会话所有权、在会话终结时**延迟**回收。
 *
 * ## 为什么要延迟，不就地 `delete`
 *
 * 终结的触发点几乎总在某个回调内部（传输层关闭回调、解析器回调、写完成
 * 回调）。就地 `delete` 会在这一瞬间留下指向已释放对象的在途引用，最直接的
 * 一个来自发送队列：`uvcpp_ws_connection::pump_send()` 交给 TCP 层的写完成
 * 回调捕获了会话的 `this`，而那个闭包由 TCP 客户端持有 —— 写完成晚于关闭
 * 到达时就会写进已释放内存。所以回收统一推迟到**下一轮循环**：那时候既不在
 * 任何回调里，TCP 客户端也已经随关闭流程销毁了。
 *
 * 驱动延迟回收的是 `uvcpp_async`（libuv 里唯一线程安全的唤醒入口，仓库里
 * `uvcpp_web_app::post()` 用的是同一个东西）。**注意 `delete` 那个 async
 * 句柄绝不能在自己的回调里做**（那会析构正在执行的 `std::function`），所以
 * `recycle_all()` 只负责删会话，句柄由 `~uvcpp_ws_sessions` 在循环停掉之后
 * 处理 —— 与 `uvcpp_web_app` 的收尾顺序一致。
 *
 * ## 一处如实说明的边界
 *
 * 会话终结靠 TCP 客户端的**关闭观察者**通知。如果客户端**根本不来关闭回调**
 * 就被删掉（框架层的某个析构直接 `delete` 客户端），会话就收不到通知 ——
 * 它会一直留在 `sessions_` 里，直到本对象析构时被 `recycle_all()` 兜底清掉。
 * 也就是说：最坏情况是"活到属主析构"，不是泄漏到进程结束。属主析构前会
 * 逐个 `terminate()`，所以这条兜底路径是有测试覆盖的（服务器停机用例）。
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_SESSIONS_H
#define SRC_WEB_UVCPP_WS_SESSIONS_H

#if UVCPP_WEB_ENABLE

#include <cstddef>
#include <vector>
#include <uvcpp/uvcpp_define.h>
#include <handle/uvcpp_loop.h>
#include <web/uvcpp_ws_connection.h>

namespace uvcpp {

class uvcpp_async;

/**
 * @brief 一组 WebSocket 会话的归属表（不可拷贝）。
 *
 * 典型用法（服务器、客户端都一样）：
 * @code
 *   uvcpp_ws_connection* c = new uvcpp_ws_connection(tcp, ws_role::SERVER);
 *   sessions_.adopt(c);          // 接管所有权 + 装终结回调
 *   c->start();
 * @endcode
 */
class UVCPP_API uvcpp_ws_sessions {
 public:
  uvcpp_ws_sessions();
  ~uvcpp_ws_sessions();

  /**
   * @brief 指定驱动延迟回收的循环。必须在**该循环的线程上**、且循环正在跑
   *        的时候调用（`uv_async_init` 不是线程安全的）。
   *
   * 传 nullptr 表示"暂时没有循环可用"：延迟回收退化成"等下一次
   * `recycle_all()`"，会话不会丢，但回收会推迟。
   *
   * 建出来的句柄是**按需保活**的：**退休表空着时 unref、有东西要删时 ref**。
   * 它只负责"醒来把待回收的会话删掉"，不是"进程还在等的事"，所以不该让一个
   * 没事可做的循环被它撑着（`uv_run` 的存活判据就是 `active_handles > 0`）；
   * 但反过来，有东西要删时它**必须**算数 —— 否则 `uv_run` 会在进 while 体之前
   * 就因为存活为 0 直接返回，挂起的唤醒请求没人取，回收永远不发生。
   * 两边的实测都写在 `set_loop()` / `on_retired()` 的实现注释里。
   *
   * 属主的循环通常另有保活句柄（服务器的监听、客户端的连接与重连定时器）；
   * 这条规则管的是"别的都没了"那一刻。
   */
  void set_loop(uvcpp_loop* loop);

  /** @brief 接管一个会话（装终结回调）。空指针忽略。 */
  void adopt(uvcpp_ws_connection* c);

  /**
   * @brief 装一个「某个会话终结了」的观察者（属主可选）。
   *
   * 在会话**已经离开活动表、但还没被 `delete`** 的时候调用：`all()` 里不再有
   * 它、`size()` 已经减掉，`pending()` 里能找到它。属主用它跟自己的状态对账
   * （客户端把「当前会话」指针清空、状态置 CLOSED 之类）。
   *
   * **不要在这里 `delete` 会话**：回收由本类的延迟回收负责（理由见类注释），
   * 在这里删会与 `drain()` 撞车（同一指针删两次）。也**不要**在这里发起耗时
   * 操作 —— 它是在终结它的那个回调里**同步**调用的。
   *
   * 只有一个槽位（后装的覆盖先装的）。没有装的时候终结照常发生，只是没人收到
   * 通知 —— 属主需要知道「会话没了」就必须装。
   */
  void set_retire_observer(std::function<void(uvcpp_ws_connection*)> cb);

  /**
   * @brief 优雅关闭并交出全部会话（服务器 `stop()` 用）。
   *
   * 逐个 `close()` —— **发起**关闭、发 Close 帧，真正的回收仍由终结回调
   * 送回。循环如果随后就停了，那些会话留在表里，等 `recycle_all()` 兜底。
   *
   * @param code 发出去的 Close 帧状态码。默认 1000 NORMAL 适合"客户端主动
   *             离开"；**属主自己要停机**时应当传 1001 `GOING_AWAY`
   *             （RFC 6455 §7.4.1：1001 的定义就是"端点即将消失，服务端要关了
   *             或客户端要离开了"）—— 对端据此就能把"服务器在维护"和
   *             "对方只是正常告别"分开，而不是收到一个含义反了的 1000。
   */
  void close_all(ws_close_code code = ws_close_code::NORMAL);

  /**
   * @brief 立即终结并回收全部会话（属主析构用）。
   *
   * 不发 Close 帧：这一步的存在理由是"循环马上要停了，等不到帧发出去"。
   */
  void recycle_all();

  /**
   * @brief 回收全部会话**并释放延迟回收用的 async 句柄**。
   *
   * 属主应当在**关闭 loop 之前**（`loop_close()` / 删掉 loop 的所有者之前）
   * 调它。理由：`delete` 那个句柄要走 `uv_close`，在一个已经关掉的 loop 上
   * 做这件事是未定义行为；而作为成员在析构里兜底时，循环往往已经没了。
   *
   * 调过之后本对象仍然可用（下次 `set_loop()` 会重新建句柄）。
   */
  void shutdown();

  /**
   * @brief 交出全部会话，**一个都不删**（只把表清空），此后本对象不再管它们。
   *
   * 只给一条路用：属主在**自己的某个回调里**析构（`~uvcpp_ws_client` 那一处）。
   * 那一刻会话的某个回调正压在栈上，而它多半就地执行着会话自己的
   * `std::function`（`uvcpp_ws_connection::deliver_message` 是就地调用）——
   * 删会话就是删掉正在执行的那个闭包，`shutdown()` 里那句"绝不能在自己的
   * 回调里做"说的正是这件事，只是那条路是属主从外面调进来的，这里是从里面。
   *
   * 会话对象连同它引用的 TCP 客户端一起留给循环。属主已经没了，所以这是
   * **有意的泄漏**，换掉一个必然发生的 use-after-free（与 `~uvcpp_loop` 里
   * "`uv_loop_close()` 关不掉就不释放那块内存"同一条策略）。
   *
   * 调过之后 `shutdown()` 变成空操作。
   */
  void abandon();

  /** @brief 当前持有的会话数（已终结待回收的**不算**）。 */
  size_t size() const;

  /** @brief 已经终结、等待下一轮循环回收的会话数。 */
  size_t pending() const;

  /**
   * @brief 累计回收（真正 `delete`）过的会话数。**单调递增**。
   *
   * 这是"会话到底有没有被回收"的唯一直接证据：`size()` 归零加
   * `pending()` 归零只说明表里空了，而会话可能是被漏掉、永远没终结
   * （那就还留在表里）。计数对上才说明对象确实被销毁过。
   */
  size_t recycled() const;

  /** @brief 表里的会话（按接管顺序）。仅供观察，不要留存指针。 */
  const std::vector<uvcpp_ws_connection*>& all() const;

 private:
  uvcpp_ws_sessions(const uvcpp_ws_sessions&);
  uvcpp_ws_sessions& operator=(const uvcpp_ws_sessions&);

  /** @brief 会话终结的落点（`retire_fn`）。 */
  void on_retired(uvcpp_ws_connection* c);
  /** @brief async 回调：把待回收的会话真正删掉。 */
  void drain();

  std::vector<uvcpp_ws_connection*> sessions_;
  std::vector<uvcpp_ws_connection*> retired_;
  uvcpp_async* drain_async_ = nullptr;
  size_t       recycled_    = 0;
  std::function<void(uvcpp_ws_connection*)> retire_observer_;
  /** @brief `abandon()` 过了：`shutdown()` 从此是空操作，别再碰那些会话。 */
  bool abandoned_ = false;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_SESSIONS_H
