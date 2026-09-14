/**
 * @file src/webapp/uvcpp_web_context.h
 * @brief 每请求的上下文：持有请求/响应、跑中间件链、管异步续跑与生命周期。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这个类解决三件事
 * ----------------
 * **一、异步 handler 怎么恢复。** C++11 没有协程，所以「中间件调 `next()`
 * 放行」不能靠递归调用栈实现 —— 那既会吃掉栈（50 层中间件就够呛），也没法
 * 从线程池回调里恢复。本类用的是**索引 + 重入标志**（`advance()`）：
 *
 *   - 同步调 `next()`：`advancing_` 已经是真，只置一个 `next_pending_` 就返回，
 *     由外层循环接着跑下一环 —— **不递归**；
 *   - 异步调 `next()`（从线程池完成回调里）：`advancing_` 是假，直接进循环，
 *     从 `chain_index_` 接着跑。
 *
 * 两者走的是同一条路，所以「同步链」和「异步链」在框架里是同一个东西。
 *
 * **二、链什么时候算结束。** 一个 handler 返回之后，框架问它一句：还能不能
 * 恢复？判断依据是它**有没有把 `next` 留起来**（留了副本就说明它打算稍后
 * 自己调）。三种情况：
 *
 * | handler 的行为                     | 框架的处理                 |
 * |------------------------------------|----------------------------|
 * | 同步调了 `next()`                  | 立刻跑下一环               |
 * | 留起了 `next`（异步续跑）           | **挂起**，等它自己调       |
 * | 既没调也没留                       | 链到此为止，收尾           |
 *
 * 第三行是「handler 忘了结束响应」的兜底：框架会打一条 WARN 然后**按现状把
 * 响应发出去**，而不是让请求永远挂着。
 *
 * @warning 「留没留 next」是靠 `next` 闭包持有上下文引用这一点判断的（见
 *          `advance()` 里的注释）。所以**留副本**这个动作必须发生在 handler
 *          返回之前 —— 把 `next` 塞进一个更晚才创建的对象里是没用的。
 *
 * @warning 反过来也要小心：**留了副本却永远不调它**，链就一直挂着，这个请求
 *          永远不会有响应（要等闲置超时才收场）。留了 `next` 就是承诺"我会
 *          调它"。
 *
 * **三、异步期间对象还在不在。** 见下面「生命周期」。
 *
 * 生命周期
 * --------
 * `uvcpp_web_context` 必须由 `create()` 造出来（内部走 `std::shared_ptr`），
 * 因为它要能**自己把自己续住**：
 *
 *   - 框架在派发期间持有一份（`App` 的 in-flight 表）；
 *   - `next` 闭包里也捕获了一份 —— 所以 handler 只要留着 `next`，上下文就
 *     不会被销毁，哪怕连接已经断了、App 已经把它摘了；
 *   - `advance()` 进循环前自己再取一份，于是 `finish()` 里把最后一份还回去
 *     （host 摘除）也不会用到已析构的对象。
 *
 * 请求/响应对象由上下文**按值持有**（`uvcpp_web_request` / `uvcpp_web_response`
 * 都是不可拷贝不可移动的，只有这一种持有方式可行）。这也顺手消掉了一类
 * 悬垂：两者的生存期和上下文完全一致，不需要调用方操心。
 *
 * 连接
 * ----
 * 上下文只拿一个 `uvcpp_web_conn_id`，**从不持有 `uvcpp_tcp_client*`**。
 * 要写原始 socket 得显式走 `raw_client()`，而它会先查登记表：连接已经断开
 * 就返回 `nullptr`。这就是「异步回调拿着旧指针写到别人连接上」那个缺陷的
 * 结构性解法。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_CONTEXT_H
#define SRC_WEBAPP_UVCPP_WEB_CONTEXT_H

#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_web_connection.h>
#include <webapp/uvcpp_web_handler.h>
#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>

namespace uvcpp {

class uvcpp_web_context;

/**
 * @brief 上下文与外界（EventLoop / 连接登记表 / HTTP 层）之间的接口。
 *
 * 计划里这一层叫 `uvcpp_web_loop_dispatcher`，但写下来发现它必然还要管
 * 「按 id 找活连接」和「把响应发出去」—— 因为这三件事在实现上是同一段代码
 * （发送前必须先在登记表里查连接），拆成两个接口只会让实现方多写一层转发。
 * 于是合并成一个，名字按实际职责取。
 *
 * `uvcpp_web_app` 实现它；测试里可以拿一个假实现把链的控制流单独测干净
 * （`tests/functional/web_app_context_func.cpp` 就是这么做的）。
 *
 * @warning 所有方法都在 **loop 线程**上调用，`post()` 除外（它是唯一要求
 *          线程安全的）。
 */
class UVCPP_API uvcpp_web_context_host {
 public:
  virtual ~uvcpp_web_context_host();

  /** @brief 当前线程是不是 loop 线程。 */
  virtual bool on_loop_thread() const = 0;

  /** @brief 把 fn 送到 loop 线程上执行。**必须线程安全**。 */
  virtual void post(std::function<void()> fn) = 0;

  /** @brief 底层事件循环（做定时器/调度用）。 */
  virtual uvcpp_loop* loop() const = 0;

  /** @brief 按 id 取活连接；已断开或 id 不存在返回 nullptr。 */
  virtual uvcpp_tcp_client* connection(uvcpp_web_conn_id id) = 0;

  /**
   * @brief 响应就绪，发出去。
   *
   * 实现方应当**先查登记表**再写：连接已经断了就别写（指针可能已被复用），
   * 记一条警告然后计数丢弃即可。
   */
  virtual void send_response(uvcpp_web_context& ctx) = 0;

  /**
   * @brief 请求被 `abort()` 掉了：不要发响应，把连接关掉。
   *
   * 不发响应是有意的 —— abort 的语义就是"这个请求我不管了"。连接留着会让
   * 客户端一直等，所以实现方应当关闭它。
   */
  virtual void abort_request(uvcpp_web_context& ctx) = 0;

  /**
   * @brief 上下文可以销毁了（链跑完、且 `hold()` 都还回去了）。
   *
   * @warning **实现必须把 ctx 的 `shared_ptr` 从所有容器里摘掉。** 这是
   *          「框架松手」的唯一动作；如果摘掉之后没有任何一份引用留存，
   *          `ctx` 会在这次调用返回前析构。实现返回后**不得再碰 ctx**，
   *          上下文自身返回后也不会再碰自己的成员。
   */
  virtual void context_finished(uvcpp_web_context& ctx) = 0;
};

/**
 * @brief 一次请求的全部状态。
 */
class UVCPP_API uvcpp_web_context
    : public std::enable_shared_from_this<uvcpp_web_context> {
 public:
  /**
   * @brief 造一个上下文。
   *
   * **只能这样造** —— 直接 `new` 或者放栈上会让 `shared_from_this()`
   * 抛异常（`advance()` 依赖它把自己续住），构造函数因此是私有的。
   */
  static std::shared_ptr<uvcpp_web_context> create(uvcpp_web_context_host& host,
                                                   uvcpp_web_conn_id conn_id);

  // -----------------------------------------------------------------
  // 连接
  // -----------------------------------------------------------------

  uvcpp_web_conn_id connection_id() const { return conn_id_; }

  /** @brief 连接现在是否还活着（查登记表，不是查缓存）。 */
  bool connection_alive() const;

  /**
   * @brief 拿到底层连接 —— **显式逃生口**。
   *
   * 正常路径不需要它：发响应走 `host.send_response()`，框架自己会查连接。
   * 只有要动原始 socket（升级协议、直接写字节）时才用。
   *
   * @return 连接**活着**时返回指针；已断开返回 `nullptr`。返回的指针只在
   *         本次 loop 迭代内可信 —— 别存进异步回调。
   */
  uvcpp_tcp_client* raw_client();

  // -----------------------------------------------------------------
  // 线程
  // -----------------------------------------------------------------

  bool on_loop_thread() const;

  /**
   * @brief 把 fn 送回 loop 线程执行（线程安全）。
   *
   * 已经在 loop 线程上时**就地执行**（省一次投递），这一点值得注意：就地
   * 执行意味着 `post()` 可能同步跑到 fn 里面去，fn 里不要假设自己在"下一个
   * 循环迭代"。
   *
   * 在别的线程上调用时它只是把 fn 塞进队列，所以**必须自己保证 ctx 活着**
   * —— 要么留一份 `next`，要么先 `hold()`。
   *
   * @note 一般的异步 handler 用不着它：把手里的 `next` 留起来、稍后直接调，
   *       框架会自己投回来（见 `uvcpp_web_next` 的说明）。要在这里 `post()`
   *       的场合是"不是想续跑链，而是想回到 loop 线程干点别的"—— 比如往
   *       别的连接上写东西。
   */
  void post(const std::function<void()>& fn);

  uvcpp_loop* loop() const;

  // -----------------------------------------------------------------
  // 生命周期
  // -----------------------------------------------------------------

  /**
   * @brief 钉住上下文，别在链跑完之后销毁。
   *
   * 什么时候需要：handler 打算**用 `post()` 而不是留 `next`** 来恢复，或者
   * 要在一个不属于本链的异步回调里访问本上下文。留了 `next` 的情况不需要。
   *
   * @warning 每次 `hold()` 都要有对应的 `release()`，否则上下文泄漏。
   */
  void hold();

  /** @brief 松开一次 `hold()`。计数归零且链已跑完 → 立刻销毁。 */
  void release();

  int hold_count() const { return hold_count_; }

  // -----------------------------------------------------------------
  // 用户数据
  // -----------------------------------------------------------------

  /**
   * @brief 挂一份自己的数据（跨中间件/异步阶段传递状态）。
   *
   * 典型的用法是解析登录态之类的**每请求**数据。用 `shared_ptr` 是因为
   * C++11 的 `std::any` 还不存在；取的时候用 `user_data_as<T>()`。
   *
   * `T` 记在 `typeid` 里，所以 `user_data_as<T>()` 用**错的类型**取会返回空，
   * 而不是给你一个 reinterpret 过的野指针。
   */
  template <typename T>
  void set_user_data(const std::shared_ptr<T>& d) {
    user_data_      = d;
    user_data_type_ = (d == nullptr) ? nullptr : &typeid(T);
  }

  /** @brief 清掉挂的数据。 */
  void clear_user_data() {
    user_data_.reset();
    user_data_type_ = nullptr;
  }

  std::shared_ptr<void> user_data() const { return user_data_; }

  /**
   * @brief 按类型取回 `set_user_data()` 存的数据。
   *
   * @return 存过**且类型对得上**才返回；没存过、或者类型不对都返回空。
   *
   * 类型比较用 `type_info::operator==`（比名字，不是比指针），所以跨 DLL
   * 也是可靠的 —— 指针相等在不同模块里对同一个类型可能不成立。
   */
  template <typename T>
  std::shared_ptr<T> user_data_as() const {
    if (user_data_ == nullptr || user_data_type_ == nullptr) {
      return std::shared_ptr<T>();
    }
    if (!(*user_data_type_ == typeid(T))) return std::shared_ptr<T>();
    return std::static_pointer_cast<T>(user_data_);
  }

  // -----------------------------------------------------------------
  // 请求 / 响应
  // -----------------------------------------------------------------

  uvcpp_web_request& request() { return req_; }
  const uvcpp_web_request& request() const { return req_; }

  uvcpp_web_response& response() { return resp_; }
  const uvcpp_web_response& response() const { return resp_; }

  // -----------------------------------------------------------------
  // 控制流
  // -----------------------------------------------------------------

  /**
   * @brief 跑一条链。**每个上下文只能调一次。**
   *
   * @param chain 处理器链，按顺序执行。**上下文只存指针**，所以这张表必须
   *              在上下文存活期内保持有效且不被修改 —— `App` 把它缓存在
   *              路由项上（与 `web_route_match::handler` 同一个约定）。
   *              这样每个请求就不用把中间件（含它们捕获的配置）再拷一遍。
   */
  void run(const std::vector<uvcpp_web_handler>& chain);

  /**
   * @brief 放行到链上的下一个处理器。
   *
   * 由 `next()` 调用，也可以直接调（比如 `post()` 回来之后）。看上面的表：
   * 同步调会置 `next_pending_`，异步调会直接续跑。链已经结束时是**安全空操作**。
   */
  void advance();

  /**
   * @brief 放弃这个请求：不发响应，让 host 关掉连接。
   *
   * 用在「这个连接上的数据已经不可信了」的场合 —— 比如协议解析出错、或者
   * 鉴权失败到不想给对方任何回显。链会立刻停止。
   */
  void abort();

  bool aborted() const { return aborted_; }

  /** @brief 链是否已经跑完并收尾（响应已发 / 已 abort）。 */
  bool finished() const { return finished_; }

  /** @brief 下一个要跑的处理器下标（诊断用）。 */
  size_t chain_index() const { return chain_index_; }
  size_t chain_size() const { return chain_ != nullptr ? chain_->size() : 0; }

 private:
  uvcpp_web_context(uvcpp_web_context_host& host, uvcpp_web_conn_id conn_id);
  uvcpp_web_context(const uvcpp_web_context&);
  uvcpp_web_context& operator=(const uvcpp_web_context&);

  /**
   * @brief 链上抛异常时的统一出口。
   *
   * 优先交给响应上装的错误处理器（`web_middleware_error_handler` 装的），
   * 没有就自己记一条 `ERROR(REQUEST)` 并回 500。
   */
  void fail(const std::string& what);

  /** @brief 收尾：发送 / abort / 通知 host。幂等。 */
  void finish();

  uvcpp_web_context_host& host_;
  uvcpp_web_conn_id       conn_id_;

  // 按值持有：两者都不可拷贝不可移动，而且这样它们的生存期天然等于上下文的。
  uvcpp_web_request  req_;
  uvcpp_web_response resp_;

  const std::vector<uvcpp_web_handler>* chain_;
  size_t chain_index_;

  /// 正在 `advance()` 的循环里（同步调 next() 靠它改成"记账"而不是递归）。
  bool advancing_;
  /// 同步调了 next()：本轮返回后由外层循环继续。
  bool next_pending_;
  /// 链已经跑完（下次 `advance()` 是空操作）。
  bool chain_finished_;
  /// `finish()` 已经跑过（幂等保护，防止重复发送）。
  bool finished_;

  bool aborted_;

  int hold_count_;
  /// `hold()` 还挂着的时候链跑完了：等 release() 再通知 host。
  bool pending_release_;

  std::shared_ptr<void> user_data_;
  /// `user_data_` 的静态类型（`set_user_data<T>()` 记下来的），供取回时校验。
  const std::type_info* user_data_type_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_CONTEXT_H
