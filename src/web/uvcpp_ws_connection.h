/**
 * @file src/web/uvcpp_ws_connection.h
 * @brief WebSocket connection — wraps an established WS-over-TCP link.
 * @author zhuweiye
 * @version 1.0.0
 *
 * Created from an already-upgraded TCP client.  Provides typed send/receive
 * for text, binary, ping, pong, and close frames via the WS frame parser.
 *
 * ## 生命周期（托管型对象）
 *
 * 本类的实例**不由使用者 `new`/`delete`**，也不自持生命周期：升级成功之后由
 * `uvcpp_ws_server`（或客户端侧的 `uvcpp_ws_client`）创建，并在会话终结时——
 * 对端关闭、协议错误、主动 `close()`、服务器停机——通过
 * `set_retire_callback()` 交还给属主回收。使用者只拿到裸指针，用就行。
 *
 * 为什么不是"会话自己 `delete` 自己"：终结的触发点几乎总在某个回调**内部**
 * （传输层关闭回调、解析器回调、写完成回调），而那时还可能存在指向本对象的
 * 在途引用 —— 最直接的一个是发送队列：`pump_send()` 交给 TCP 层的写完成
 * 回调捕获了本对象的 `this`，它由 TCP 客户端持有。会话在回调里自毁，那个
 * 闭包就会在稍后被调用时写进已释放内存。所以终结与回收**分成两步**：会话在
 * 终结点标注自己并上报，属主在**下一轮循环**（不在任何回调里）真正 `delete`。
 */

#pragma once
#ifndef SRC_WEB_UVCPP_WS_CONNECTION_H
#define SRC_WEB_UVCPP_WS_CONNECTION_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_ws_ext.h>
#include <web/uvcpp_ws_frame.h>
#include <web/uvcpp_ws_parser.h>

namespace uvcpp {

/**
 * @brief 本端在一条 WebSocket 连接上的角色。
 *
 * **为什么要一个枚举而不是 `bool is_server`。** RFC 6455 对两侧的帧要求是
 * **不对称**的：客户端发出的帧 MUST 掩码、服务端发出的 MUST NOT（§5.1）。
 * 既然方向搞反了会直接让连接不可用，这个参数就不该有默认值（见构造函数的
 * 说明）；而"必填"之后，剩下的问题是**光看 `true`/`false` 不知道哪边是哪边** ——
 * 枚举正好补上这一点，而且报错信息里也会直接写出 `ws_role`。
 */
enum class ws_role : int {
  SERVER = 0,   ///< 本端是服务端：发出去的帧**不**掩码，收到的帧**必须**掩码
  CLIENT = 1    ///< 本端是客户端：发出去的帧**必须**掩码，收到的帧**不**该掩码
};

class UVCPP_API uvcpp_ws_connection {
 public:
  // 这个宏同时声明默认构造与析构，两者在 `.cpp` 里都有定义：析构正常清理；默认
  // 构造**不带传输层**（`tcp_` 留空，`role_` 为 `SERVER`），建出来是个合法但惰性
  // 的对象 —— `start()` 见 `tcp_ == nullptr` 直接返回，不会有任何 I/O，本类也没有
  // 之后再挂传输层的接口。要真正收发帧，用下面那个两参构造（`role` 必填）。
  // 注意别把 `is_open()` 当"有传输层"用：它只回 `!retired_`，所以这种对象上它是
  // **true**。
  UVCPP_DEFINE_FUNC(uvcpp_ws_connection)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_ws_connection)

  /**
   * @brief Wrap an already-upgraded TCP client. Call start() to begin.
   *
   * @param role 本端角色。**必填、无默认值** —— RFC 6455 对两侧的帧要求是
   *        **不对称**的：客户端发出的帧 MUST 掩码（§5.1），服务端发出的
   *        MUST NOT（同节）。搞反了任意一侧都会直接不可用，所以让它必须被
   *        写出来，而不是靠一个"猜多半是服务端"的默认值。
   *
   *        **也别给它加默认值来"省一次编译错误"。** 两个默认方向都是静默坏：
   *        默认 SERVER 则客户端漏填会发出未掩码帧（任何合规服务端都会 1002
   *        掉它），默认 CLIENT 则服务端漏填会发出带掩码的帧（合规客户端会关
   *        连接）。换句话说，"搞反了框架自己的往返用例会抓到"这条**只在
   *        框架自测自时成立**，而两侧同错恰好是最容易互相掩盖的一种。
   */
  uvcpp_ws_connection(uvcpp_tcp_client* tcp, ws_role role);

  /**
   * @brief 开始读 WS 帧。握手完成后调**一次**，且调用方已经不在 TCP 读回调里。
   */
  void start();

  /**
   * @brief 补投"跟握手挤在同一批到达"的字节（**升级方专用**）。
   *
   * @param data  WS 字节（升级应答/请求之后剩下的那一截）。
   * @param len   长度；0 或 @p data 为空都不做事。
   *
   * **为什么必须由升级方补投**：握手那一段用的是另一个读回调，它从内核缓冲
   * 里把整批字节一次取走（含第一帧），随后 `start()` 用 `read_stop()` +
   * `read_start()` 换成本会话的读回调 —— 已经被取走的那部分**再也读不回来**。
   * 不补投的表现是"第一帧凭空消失"：连接好好的、后面的帧都收得到，只有第一帧
   * 没了。而它跟"对端压根没发"长得一模一样。
   *
   * 必须**在 `start()` 之后**、并且**在你自己的回调装好之后**调 —— 补投是
   * 同步派发的，装晚了那一帧就派发给了空回调（服务端是在 `on_ready` /
   * `on_connection` 之后补投，客户端是在 `connect` 回调之后：那正是"用户装
   * 回调"的那个点）。
   *
   * 会话在补投之前就结束了（对方断开 / 回调里把手上的会话关了）就**丢掉**这批
   * 字节：连接都不在了，帧派发出去也没有意义。
   */
  void feed_pending(const char* data, size_t len);

  // -------------------------------------------------------------------
  // 生命周期 —— 属主接口
  // -------------------------------------------------------------------

  /** @brief 会话终结回调：`on_retired(conn)`。 */
  typedef std::function<void(uvcpp_ws_connection*)> retire_fn;

  /**
   * @brief 安装会话终结回调（**属主专用**，使用者不要调）。
   *
   * 会话终结时**恰好调用一次**，并且是**最后一次**碰这个对象的机会：回调返回
   * 之后会话不再有任何动作，属主可以随时 `delete` 它（建议在下一轮循环里删，
   * 见类注释）。终结的四种触发：对端断开/读错误、对端 Close 帧、协议错误、
   * 属主调 `terminate()`。
   *
   * 没有装回调时终结仍然发生（会话照旧停止收发、`is_open()` 变 false），
   * 只是没人收到通知 —— 那意味着对象没人回收。所以创建会话的一方**必须**装。
   */
  void set_retire_callback(retire_fn cb);

  /** @brief 会话是否还在（还没被判定终结）。 */
  bool is_open() const;

  /**
   * @brief 优雅关闭：发 Close 帧，发出去之后关掉底层连接。
   *
   * 只是**发起**：真正的终结要等底层连接关闭（可能在同一轮循环、也可能在
   * 几轮之后），届时走终结回调。可以重复调用，只生效一次。
   */
  void close(ws_close_code code = ws_close_code::NORMAL,
             const std::string& reason = std::string());

  /**
   * @brief 立即终结（**不发** Close 帧），并上报属主回收。
   *
   * 给"循环马上要停了、来不及等 Close 帧发完"的场合用：`uvcpp_ws_server`
   * 的析构就是靠它保证每个会话都被回收 —— 那时候关闭观察者已经不会再来了。
   */
  void terminate();

  // -------------------------------------------------------------------
  // Send
  // -------------------------------------------------------------------
  int send_text(const char* data, size_t len, std::function<void(int)> cb = nullptr);
  int send_binary(const char* data, size_t len, std::function<void(int)> cb = nullptr);
  int send_ping(const char* data = nullptr, size_t len = 0);
  int send_pong(const char* data = nullptr, size_t len = 0);
  int send_close(ws_close_code code = ws_close_code::NORMAL,
                 const std::string& reason = "");

  // -------------------------------------------------------------------
  // Receive callbacks
  // -------------------------------------------------------------------
  //
  // 交付语义（RFC 6455 §5.4）：`on_text` / `on_binary` **每条完整消息调用一次**，
  // 不是每帧一次。分片由本层重组，回调拿到的永远是完整消息。`data` 指针只在
  // 回调期间有效（指向重组缓冲区），需要留存请自己拷贝。
  void on_text(std::function<void(const std::string&)> cb);
  void on_binary(std::function<void(const uint8_t*, size_t)> cb);
  void on_ping(std::function<void(const uint8_t*, size_t)> cb);
  void on_pong(std::function<void(const uint8_t*, size_t)> cb);
  /**
   * @brief 会话结束回调：`on_close(code, reason)`。**每条会话最多一次。**
   *
   * 码的含义分两种：
   *  - 对端发了 Close 帧 → 帧里带的码与原因（正常关闭是 1000）；
   *  - 对端**没发** Close 帧就断了（拔网线、进程被杀、TCP 复位）→
   *    `ABNORMAL_CLOSE`(1006)，原因是空串。1006 是 RFC 6455 §7.1.5 规定的
   *    "没有 Close 帧"的表示，它**不能**出现在线上帧里，只在本回调里出现。
   *
   * 本端**自己**发起的结束（协议错误、使用者调 `close()`）不会再触发它 ——
   * 那些情况调用方本来就知情，协议错误另有 `on_error` 报告。也就是说：这个
   * 回调专门用来回答"对端是怎么走的"。
   */
  void on_close(std::function<void(ws_close_code, const std::string&)> cb);

  /**
   * @brief 协议错误回调：`on_error(close_code, reason)`。
   *
   * 收到非法帧（保留位、非法 CONTINUATION、消息中插入新数据帧、超限……）时，
   * 本层**先发对应的 Close 帧再关闭连接**，然后调用它。1002 = 协议错误，
   * 1009 = 消息过大，1007 = 负载非法（解压失败）。
   */
  void on_error(std::function<void(int, const std::string&)> cb);

  // -------------------------------------------------------------------
  // 接收策略（安全）
  // -------------------------------------------------------------------

  /**
   * @brief 重组后**单条消息**的最大字节数，默认 16 MiB。
   *
   * 该值同时作为**单帧**上限下达给解析器（`parser_.set_max_frame_size`）。
   * 两级都卡是必须的：只卡单帧挡不住分片 —— 每一片都合规、合起来无上限，
   * 这正是绕过单帧限制的标准手法。超限发 1009 并关闭连接。
   *
   * @param n 字节数；**0 = 不限**（不建议，等于放弃这一层保护）。
   */
  void set_max_message_size(size_t n);
  size_t get_max_message_size() const;

  // -------------------------------------------------------------------
  // permessage-deflate（RFC 7692）— 仅 UVCPP_ZLIB_ENABLE=1
  // -------------------------------------------------------------------
#if UVCPP_ZLIB_ENABLE
  /**
   * @brief 按握手协商结果启用压缩（收发两侧同时生效）。
   *
   * @param is_server **本端角色**，必填且无默认值：窗口位数与 context
   *                  takeover 都是**按方向**命名的，搞反不会报错，只会解出
   *                  乱码。
   * @param p         `ws_deflate_accept_server()` / `ws_deflate_accept_client()`
   *                  的返回值。`p.accepted == false` 时**不要调用**本函数 ——
   *                  没协商成就该退化成普通 WS。
   */
  void enable_compression(bool is_server, const uvcpp_ws_deflate_params& p);
  bool is_compression_enabled() const;

  /**
   * @brief 小于此长度的消息干脆不压（默认 0 = 都压）。
   *
   * 这是**发送前**的判定，不是"压完发现不划算再退回原文"：后者会让 deflate
   * 上下文已经吞掉这条消息、而对端从没收到过它 —— 在 context takeover 下，
   * 之后消息里的回溯引用就会指到对端没有的历史上，对端直接解失败，而且是在
   * **下一条**消息上失败，离现场很远。所以要么压了就发，要么一开始就不压，
   * 中间没有回头路。（这也正是主流实现只用阈值、不做"压完比大小"的原因。）
   */
  void set_compress_min_size(size_t n);
  size_t get_compress_min_size() const;
#endif

  // -------------------------------------------------------------------
  // Accessors
  // -------------------------------------------------------------------
  /**
   * @brief 底下的 TCP 客户端；**会话终结后返回 nullptr**。
   *
   * 拿到它就等于拿到原始连接（可以在上面挂 `add_close_observer`、读对端
   * 地址、挂原始数据钩子……）。但它只保证在 `is_open()` 为真时有效：终结
   * 之后客户端可能已经被框架释放 —— 这正是本函数改成返回 nullptr 而不是
   * 留一个悬垂指针的原因。
   */
  uvcpp_tcp_client* get_tcp_client();

 private:
  void on_tcp_data(uvcpp_buf* buf);
  /** @brief 传输层关闭的落点（经关闭观察者）。 */
  void on_transport_closed();
  /** @brief 幂等终结：标注自己、摘观察者、清 tcp_、上报属主。 */
  void notify_retired();
  void on_ws_frame(const uvcpp_ws_frame& frame);
  /** @brief TEXT/BINARY 共用的发送路径（含压缩决策）。 */
  int  send_data(ws_opcode op, const char* d, size_t n, std::function<void(int)> cb);
  /** @return 0 已入队/已发出；非 0 表示这一帧没能发出去（错误码）。 */
  int  send_frame(const uvcpp_ws_frame& frame, std::function<void(int)> cb);

  /**
   * @brief 按本端角色给帧盖上掩码（RFC 6455 §5.1：客户端发出的帧 MUST 掩码、
   *        服务端发出的 MUST NOT）。
   *
   * **四个发送出口都要过这一手** —— 数据帧、PING、PONG、CLOSE。控制帧同样是
   * "本端发出的帧"，§5.5 把 §5.1 的要求一并罩住了，只盖数据帧等于漏一半。
   */
  void apply_mask(uvcpp_ws_frame& f) const;

  // --- 发送队列 ---
  // `uvcpp_tcp_client::write()` 同一时刻只允许一个异步写：已有在途写时它直接
  // 返回 UV_EALREADY，**并且在返回之前不保存回调**。原先的 send_frame 对非 0
  // 返回只是释放缓冲就算了 —— 帧静默消失，而返回值恒为 0，调用方看不出来。
  // 队列把"在途"和"待发"分开，帧按序发出，不再有静默丢失。
  struct pending_send {
    std::string              bytes;  // 已序列化的线上字节
    std::function<void(int)> cb;
  };
  void pump_send();
  void on_send_complete(int status);
  /** @brief 把队列里剩下的帧全部以 err 结算掉（连接已坏，别再留着）。 */
  void fail_pending_sends(int err);

  // --- 消息重组（RFC 6455 §5.4）---
  void deliver_message();
  /** @brief 发 Close(code, reason) 并关闭连接；只生效一次。 */
  void protocol_error(ws_close_code code, const std::string& reason);

  uvcpp_tcp_client* tcp_ = nullptr;

  /// 本端角色。决定掩码检查的方向（见构造函数的说明）。
  ws_role role_ = ws_role::SERVER;

  /// 只在本类内部用；语义同 `role_ == ws_role::SERVER`。
  bool is_server() const { return role_ == ws_role::SERVER; }
  uvcpp_ws_parser   parser_;
  bool started_ = false;

  // 生命周期状态
  /// 已经终结（上报过了）。置上之后**绝不再碰 tcp_** —— 它可能已经没了。
  bool retired_ = false;
  /// 关闭观察者的句柄（0 = 没装）。终结时要摘掉，否则会回调到已释放的会话。
  int  close_observer_id_ = 0;
  /// 是否已经通过 on_close_ 告诉过调用方"会话结束"。保证它最多跑一次。
  bool close_notified_ = false;
  retire_fn retire_fn_;

  /**
   * @brief 存活令牌（与 `uvcpp_tcp_client::alive_token_` 同一个手法）。
   *
   * 发送完成回调 `on_send_complete` 是由 TCP 客户端**持有**的一个闭包调用的
   * （`pump_send` 交给 `tcp_->write()` 的那个 lambda）。那个闭包的生命周期跟着
   * 客户端走，**不跟着会话走** —— 服务端还好（关闭时 tcp_server 会把客户端
   * 删掉，闭包随之消失），但客户端侧没有关闭管理器：`uvcpp_ws_client` 的
   * TCP 客户端要活到它自己析构。也就是说"会话已经回收、写完成才到"是有可能
   * 真的发生的（延迟回收只推后一轮循环，并不保证写完成已经过去了）。
   *
   * 所以写完成回调捕获的是它的 weak_ptr：令牌失效就直接返回，什么都不碰。
   * 会话终结/析构时作废令牌。
   */
  std::shared_ptr<char> alive_token_;

  // 发送队列状态
  std::deque<pending_send> send_q_;
  bool write_in_flight_ = false;
  int  send_error_      = 0;   // 非 0 = 发送通道已坏（粘性）

  // 重组状态
  bool      in_message_ = false;
  ws_opcode message_opcode_     = ws_opcode::TEXT;
  bool      message_compressed_ = false;   // 消息首帧的 RSV1
  uvcpp_buf message_payload_;
  size_t    max_message_size_ = 16u * 1024u * 1024u;
  bool      protocol_failed_  = false;
  std::string last_parse_error_;

#if UVCPP_ZLIB_ENABLE
  // 压缩只在**发送时**才需要记住阈值；协商参数全部存在 parser_ 里
  // （它是唯一知道窗口位数与 context takeover 的地方）。
  size_t    compress_min_size_ = 0;
#endif

  std::function<void(const std::string&)>    on_text_;
  std::function<void(const uint8_t*, size_t)> on_bin_;
  std::function<void(const uint8_t*, size_t)> on_ping_;
  std::function<void(const uint8_t*, size_t)> on_pong_;
  std::function<void(ws_close_code, const std::string&)> on_close_;
  std::function<void(int, const std::string&)>           on_error_;
};

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_WS_CONNECTION_H
