/**
 * @file src/redis/uvcpp_redis.h
 * @brief 内置 RESP2/RESP3 的 Redis 客户端：值类型 + 一条连接上的命令 / 管线 / 订阅。
 * @author zhuweiye
 * @version 1.7.5
 *
 * 本模块**不依赖任何第三方 Redis 客户端库**（不用 hiredis）：RESP 的编解码在
 * 私有的 `uvcpp_redis_resp.h` 里，传输建立在 `uvcpp_tcp_client` 上。于是它跟着
 * net 模块走 —— `UVCPP_BUILD_NET=OFF` 时 `UVCPP_REDIS_ENABLE` 也是 0，公开头
 * 整段落在 `#if` 外面。
 *
 * 线程模型与仓库里其它客户端一致：**一个对象只属于一条循环、只属于一条线程**。
 * 所有回调都在那条循环的线程上触发；不要从别的线程调本类的任何方法（唯一的
 * 例外是把对象本身的所有权转给别的线程，且转交之前一条命令都不在飞）。
 *
 * @code
 *   uvcpp_loop loop;
 *   uvcpp_redis_client redis(&loop);
 *   redis.connect("redis://127.0.0.1:6379/0", [](int st) { ... });
 *   redis.command({"SET", "k", "v"}, [](int st, const uvcpp_redis_value& v) {
 *     // v.type() == STATUS, v.as_string() == "OK"
 *   });
 *   loop.run(UV_RUN_DEFAULT);
 * @endcode
 */

#pragma once
#ifndef SRC_REDIS_UVCPP_REDIS_H
#define SRC_REDIS_UVCPP_REDIS_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_REDIS_ENABLE

#include <uvcpp/uvcpp_export.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace uvcpp {

class uvcpp_loop;
class uvcpp_redis_client_impl;
class uvcpp_redis_value_impl;
struct uvcpp_redis_parser;
struct uvcpp_redis_value_factory;

/**
 * @brief RESP 值的类型。
 *
 * RESP2 只会出现前五种（`STATUS` / `ERROR` / `INTEGER` / `BULK` / `ARRAY`）；
 * 后五种来自 RESP3（`HELLO 3` 之后）。`NULL_VALUE` 同时覆盖 RESP2 的
 * `$-1` / `*-1` 与 RESP3 的 `_` —— 两者在语义上是同一个"空"，调用方不该
 * 按协议版本分支。
 *
 * 大整数（RESP3 的 `(`）与逐字字符串（`=`）都归到 `BULK`：它们的内容本来就是
 * 一串字节，多一种类型只会让调用方多写一个 `switch` 分支。
 */
enum class uvcpp_redis_type : std::uint8_t {
  STATUS,      ///< `+OK\r\n`
  /**
   * @brief `-ERR ...\r\n`（错误也是一种应答：回调的 `status` 仍是 0）。
   *
   * 叫 `ERROR_REPLY` 而不是 `ERROR`：Windows 的 `<wingdi.h>` 有
   * `#define ERROR 0`，宏展开不看 `enum class` 作用域，写成 `ERROR`
   * 在 windows.h 之后 include 本头时直接编不过（同 `log_level::ERR`）。
   */
  ERROR_REPLY,
  INTEGER,     ///< `:1000\r\n`
  BULK,        ///< `$5\r\nhello\r\n`（也含 RESP3 的大整数与逐字字符串）
  ARRAY,       ///< `*2\r\n...`（也含 RESP3 的 push `>`）
  MAP,         ///< `%2\r\n...`（RESP3）
  SET,         ///< `~2\r\n...`（RESP3）
  DOUBLE,      ///< `,3.14\r\n`（RESP3）
  BOOLEAN,     ///< `#t\r\n` / `#f\r\n`（RESP3）
  NULL_VALUE   ///< `$-1` / `*-1`（RESP2）或 `_\r\n`（RESP3）
};

/**
 * @brief 一个 RESP 值。
 *
 * 内部是一个**不可变的共享节点**（pimpl）：拷贝只是加一次引用计数，所以
 * 把数组/映射里的元素取出来、传进回调、存进容器都不会有深拷贝的代价。
 * 代价是这些值**只能读**：要构造请求请用 `uvcpp_redis_client::command()`，
 * 本类没有构造侧 API。
 *
 * 越界与类型不符的访问不抛异常（本仓不用异常），返回零值：
 * `as_string()` 给空串、`as_integer()` 给 0、`at()` 给一个空值。这是为了让
 * "服务器暂时返回了别的形状"不至于把进程带走 —— 类型判断请在调用方自己做
 * （`type()` / `is_null()`）。
 */
class UVCPP_API uvcpp_redis_value {
 public:
  /** @brief 一个 `NULL_VALUE`。 */
  uvcpp_redis_value();
  ~uvcpp_redis_value();
  uvcpp_redis_value(const uvcpp_redis_value& other);
  uvcpp_redis_value& operator=(const uvcpp_redis_value& other);
  uvcpp_redis_value(uvcpp_redis_value&& other) noexcept;
  uvcpp_redis_value& operator=(uvcpp_redis_value&& other) noexcept;

  /** @brief 本值的类型。 */
  uvcpp_redis_type type() const;

  /** @brief 是不是 RESP 里的"空"（`$-1` / `*-1` / `_`）。 */
  bool is_null() const;

  /** @brief 是不是错误应答（`-ERR ...`）。错误应答的 status 仍是 0。 */
  bool is_error() const;

  /**
   * @brief `STATUS` / `ERROR` / `BULK` / `DOUBLE` 的文本内容。
   *
   * `INTEGER` / `BOOLEAN` 返回空串（用 `as_integer()` / `as_bool()`）；
   * `ARRAY` / `MAP` / `SET` 也返回空串（用 `size()` / `at()` / `items()`）。
   */
  const std::string& as_string() const;

  /** @brief `INTEGER` 的整数值；`BOOLEAN` 给 1/0；其它类型给 0。 */
  std::int64_t as_integer() const;

  /** @brief `DOUBLE` 的浮点值；`INTEGER` 给它的数值；其它类型给 0。 */
  double as_double() const;

  /** @brief `BOOLEAN` 的真假；`INTEGER` 按非 0 判真；其它类型给 false。 */
  bool as_bool() const;

  /** @brief `ARRAY` / `SET` 的元素个数；`MAP` 的键值对数；其它类型给 0。 */
  std::size_t size() const;

  /**
   * @brief 取 `ARRAY` / `SET` 的第 \p i 个元素。
   *
   * 越界（或对别的类型调用）返回一个静态的空值。`MAP` 请用 `items()`。
   */
  const uvcpp_redis_value& at(std::size_t i) const;

  /**
   * @brief `MAP` 的键值对。
   *
   * 顺序就是服务器给的顺序。非 `MAP` 返回一个静态的空表 —— 返回引用而不是
   * 值是为了让 `for (auto& kv : v.items())` 不产生拷贝。
   */
  const std::vector<std::pair<uvcpp_redis_value, uvcpp_redis_value>>& items()
      const;

  /** @brief 一个静态的空值（`NULL_VALUE`）；越界访问返回的就是它。 */
  static const uvcpp_redis_value& null();

 private:
  friend struct uvcpp_redis_parser;
  friend struct uvcpp_redis_value_factory;
  explicit uvcpp_redis_value(std::shared_ptr<const uvcpp_redis_value_impl> p);

  std::shared_ptr<const uvcpp_redis_value_impl> p_;
};

/**
 * @brief 一条 Redis 连接上的客户端。
 *
 * 连接建立之后，同一条连接上**同一时刻只挂一笔 `write()`**，其余命令进发送
 * 队列；应答按 FIFO 与待决命令一一配对。这是 RESP 的基本约定：没有请求 ID，
 * 同一个连接上按顺序收发。
 *
 * 传输错误（断线、写失败）会让**所有**待决命令的回调以非 0 status 触发一次，
 * 然后连接进入断开状态。Redis 自己返回的错误应答（`-ERR` / `-WRONGTYPE`）
 * **不是**传输错误：`status == 0`、`reply.type() == ERROR`、`reply.as_string()`
 * 是服务器原话 —— 这两类必须分开，否则调用方没法区分"没连上"与"命令写错了"。
 */
class UVCPP_API uvcpp_redis_client {
 public:
  /**
   * @param loop 外部事件循环；**nullptr 表示自建一条**（析构时一起销毁）。
   *             外部循环必须活得比本对象长，`run()` 也由调用方负责。
   */
  explicit uvcpp_redis_client(uvcpp_loop* loop = nullptr);
  ~uvcpp_redis_client();
  uvcpp_redis_client(const uvcpp_redis_client&) = delete;
  uvcpp_redis_client& operator=(const uvcpp_redis_client&) = delete;

  /**
   * @brief 选 RESP 版本：2（默认）或 3。**必须在 `connect()` 之前调用**。
   *
   * RESP3 在连接建立后先发一条 `HELLO 3 [AUTH user pass]`；服务器不支持
   * （Redis 6 之前）时连接失败，`last_error()` 里是服务器原话。
   */
  void set_resp_version(int version);

  /** @brief 连接（含协商）的超时，毫秒；默认 30000。0 = 不超时。 */
  void set_connect_timeout_ms(int ms);

  /**
   * @brief 单条命令的超时，毫秒；默认 **0 = 不超时**。
   *
   * 计的是"从这条命令排到队头、到它的应答到齐"这一段时间；超时后连接被关闭，
   * 所有待决命令以 `UV_ETIMEDOUT` 失败。管线算**一条**：整组应答到齐才算完。
   */
  void set_command_timeout_ms(int ms);

  /**
   * @brief 连接并完成协商（TCP → 可选 TLS → AUTH → SELECT → HELLO）。
   *
   * URL 形状：`redis://[user:pass@]host[:port][/db]`，TLS 用 `rediss://`。
   * 用户名密码按 RFC 3986 做百分号解码；只有密码没有用户名时写
   * `redis://:pass@host`。端口默认 6379，路径为空或 `/` 时选 0 号库。
   *
   * @param cb 完成回调；**status 非 0 时**它是失败原因。协商全部成功之后才
   *           触发 —— 回调里可以立刻发命令。与 `uvcpp_tcp_client::connect()`
   *           同一约定：返回非 0 表示这次都没有发起，此时回调**不会**触发。
   */
  int connect(const std::string& url, std::function<void(int)> cb = nullptr);

  /**
   * @brief 同步版 `connect()`：自己泵这条循环直到完成或超时。
   *
   * @warning 外部循环被别的对象跑着时不要用（与仓库里其它 `*_wait` 相同）。
   * @return 0 成功；超时 `UV_ETIMEDOUT`；其余为失败原因。
   */
  int connect_wait(const std::string& url, int timeout_ms = 30000);

  /** @brief 关掉连接；所有待决命令的回调以 `UV_ECANCELED` 触发。 */
  void close();

  /** @brief 连接是否已建立且协商完成。 */
  bool is_connected() const;

  /**
   * @brief 发一条命令（参数列表，第一项是命令名）。
   *
   * 参数按**字节**发送，不做任何编码/转义；二进制安全（可以塞 `\0`）。
   *
   * @param cb 应答回调；`status` 见类注释。`cb` 不能为空。
   * @return 0 已受理；`UV_ENOTCONN` 连接没就绪；`UV_EINVAL` 参数为空。
   */
  int command(const std::vector<std::string>& args,
              std::function<void(int, const uvcpp_redis_value&)> cb);

  /**
   * @brief 同步版 `command()`：泵循环直到应答或超时。
   *
   * `command_timeout_ms` 为 0 时这里仍按 \p timeout_ms 收尾（**本地**超时，
   * 不会关连接）；超时返回 `UV_ETIMEDOUT`，命令本身可能稍后才到。
   */
  int command_wait(const std::vector<std::string>& args,
                   uvcpp_redis_value& out, int timeout_ms = 30000);

  /**
   * @brief 一次发出多条命令，按顺序收应答。
   *
   * 与逐条 `command()` 的区别在于**一次写出去**（少若干次系统调用），应答仍然
   * 逐条配对。任何一条失败（传输错误）整组以同一个 status 结束；Redis 的
   * `-ERR` 不算失败，逐条落在结果里。
   */
  int pipeline(const std::vector<std::vector<std::string>>& commands,
               std::function<void(int, const std::vector<uvcpp_redis_value>&)>
                   cb);

  /** @brief 同步版 `pipeline()`。 */
  int pipeline_wait(const std::vector<std::vector<std::string>>& commands,
                    std::vector<uvcpp_redis_value>& out,
                    int timeout_ms = 30000);

  /**
   * @brief 订阅若干频道。
   *
   * 订阅成功之后，服务器推来的消息走 `on_message(channel, message)`。RESP2 与
   * RESP3 的差别只在线上编码（RESP2 是普通数组、RESP3 是 push 帧），这里对
   * 调用方是同一件事。
   *
   * @param cb 订阅本身的应答；`status != 0` 表示订阅没成。
   * @note 订阅模式下 Redis 只允许少量命令（`SUBSCRIBE` / `UNSUBSCRIBE` /
   *       `PING` / `QUIT` / `RESET`）；RESP2 的这条限制由服务器执行，本类不拦。
   */
  int subscribe(
      const std::vector<std::string>& channels,
      std::function<void(const std::string& channel,
                         const uvcpp_redis_value& message)> on_message,
      std::function<void(int)> cb = nullptr);

  /**
   * @brief 按模式订阅（`PSUBSCRIBE`）。
   *
   * `on_message` 的 channel 参数给的是**真正的频道名**（不是模式），与
   * `redis-cli` 的 `pmessage` 一致。
   */
  int psubscribe(
      const std::vector<std::string>& patterns,
      std::function<void(const std::string& channel,
                         const uvcpp_redis_value& message)> on_message,
      std::function<void(int)> cb = nullptr);

  /** @brief 退订；之后消息回调不再触发（服务器确认后生效）。 */
  int unsubscribe(const std::vector<std::string>& channels,
                  std::function<void(int)> cb = nullptr);

  /** @brief 最近一次失败的人类可读说明（URL 解析、协商、传输错误）。 */
  const std::string& last_error() const;

 private:
  std::shared_ptr<uvcpp_redis_client_impl> impl_;
};

}  // namespace uvcpp

#endif  // UVCPP_REDIS_ENABLE

#endif  // SRC_REDIS_UVCPP_REDIS_H
