/**
 * @file src/webapp/uvcpp_web_file.h
 * @brief 分片异步读 + 有界滑动窗口的文件下发载体。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它解决的问题
 * ------------
 * 3a/3b 把**收**的方向打通了（请求体边收边解析、边落盘），**发**的方向一直是
 * 老样子：任何响应都必须先在内存里攒成一个完整的 body。静态服务今天下发一个
 * N 字节的文件，峰值内存是 **2N** —— worker 整读一份（`read_file()`），响应
 * 再深拷贝一份（`resp.body(...)`）。而 LRU 缓存有 32 MiB 的字节上限，所以
 * **大文件本来也进不了缓存**：整读那条路对大文件什么都没换来，只换来 2N。
 *
 * 本模块把这条路换成：**一片一片地异步读，每片交给 sink 之后就不再持有**，
 * 于是内存占用与文件大小**无关**。
 *
 * 为什么不用 `uv_fs_sendfile`（唯一的零拷贝路径）
 * ---------------------------------------------
 * Windows 的实现是 CRT fd 上的用户态 `_read`/`_write` 循环（写不到 libuv 的
 * 连接上）；Unix 的实现要求 `out_fd` 是**裸 socket fd**，而经过 TLS 过滤的
 * `uvcpp_tcp_client` 不暴露它（开 SSL 时字节还必须走 `SSL_write`）。
 * `uvcpp_web_static.h` 里那段「本平台不可用、分片要单独设计」的注释早就写着
 * 这件事 —— 本模块就是那段设计。
 *
 * 内存上界（**这是本模块存在的全部意义**）
 * --------------------------------------
 * 峰值 = 切片缓冲 + 接收方压着的量，而后者由本模块自己封顶：
 *
 *     窗口 = slice_bytes + backlog          （backlog 是 sink 报的）
 *     每交付完一片就检查：backlog >= high_water ⇒ 停读，等 resume()
 *     交付之前 backlog < high_water，交付加进来的 ≤ slice
 *     ⇒ 峰值 ≤ slice + (high_water - 1 + slice) = high_water + 2 * slice
 *
 * 默认 slice = 256 KiB、high_water = 1 MiB ⇒ **峰值 ≈ 1.5 MiB，与文件大小无关**。
 * 两个数都可配，而且都**导出成静态常量** —— 用例要用它们算上界，而不是另抄
 * 一份（抄一份必然漂移，而"内存不随输入增长"正是这条路径最该守住的唯一不变式）。
 *
 * 为什么"一片缓冲就够"（不需要双片轮换）
 * ------------------------------------
 * `uvcpp_tcp_client::write(const char*, size_t, cb)` 会 `memcpy` 一份 payload
 * （`uvcpp_buf.cpp` 的构造函数是 `resize(sz)` + `memcpy`）。数据交出去之后库那
 * 边自己留了一份，所以本模块**下一笔 `uv_fs_read` 可以立刻覆盖同一块缓冲**。
 * 这一条是读源码核实的，不是假设。
 *
 * **它同时是 sink 的契约**：`on_data` 必须在这一句之内把数据拷走或消耗掉，
 * 因为返回之后下一个 `uv_fs_read` 就会覆盖那块内存。
 *
 * 载体的选择
 * ----------
 * **一个 transfer 一个 `uvcpp_fs`**，串行跑 `open → read → read → … → close`。
 * 与 3b 的落盘路径同一条理由：`uvcpp_fs` 的单笔在途守卫（`async_pending_`）
 * 在这里不是限制，而是恰好合拍的模型。
 *
 * 短读
 * ----
 * `uv_fs_read` **不保证读满**请求的长度。本模块的每一笔都按"还差多少"重新算
 * 请求长度，所以短读只是让下一笔变小，自然补上，不需要额外的内层循环。
 * **如实记**：与 3b 步骤 1 实测过的短写一样，短读在本平台很可能**走不到**
 * （一次 `uv_fs_read` 就把请求的长度读满了）。循环照写，但**不声称它被覆盖**。
 *
 * 生命周期
 * --------
 * 由 `shared_ptr` 持有，且**每一笔在途 fs 操作的完成回调按值捕一份
 * `shared_from_this()`**，所以调用方丢掉最后一个引用也不会打断正在跑的读 ——
 * transfer 自己活到 I/O 跑完。这与 `uvcpp_web_upload` 是同一手法，也同样依赖
 * `uvcpp_fs` 那个「回调先 `swap` 到栈上再调」的契约（见 `uvcpp_web_upload.h`
 * 里那段说明：没有它，链下一笔会析构掉正在执行的闭包）。
 *
 * 断连即释放**不在这里**：本模块不知道 HTTP 的存在。挂到框架的关闭扇出上
 * （`uvcpp_web_app::on_connection_close`）是接线层的事，调的是 `cancel()`。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_FILE_H
#define SRC_WEBAPP_UVCPP_WEB_FILE_H

// `size_t` / `uint64_t` 显式声明，不靠传递包含：本仓的兄弟头
// （`uvcpp_web_upload.h`、`uvcpp_web_response.h`、`uvcpp_web_static.h`）都
// 显式写着这两个，MSVC 上靠 `<string>` 传递进来能编过，但第一次 POSIX 构建
// 就会断在这里。
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

// 前向声明：本头刻意不拉 libuv。`unique_ptr<uvcpp_fs>` 配合在 .cpp 里定义的
// 析构函数即可；fd 用 `int` 存（`uvcpp_web_upload` 同样这么做），调用点再转成
// `uv_file`。
class uvcpp_fs;
class uvcpp_loop;

// =========================================================================
// 接收方
// =========================================================================

/**
 * @brief 分片下发的接收方。
 *
 * 三个成员各回答一个问题，缺一个都构不成完整的契约：
 * - `on_data` —— 数据往哪儿去；
 * - `backlog` —— 你还能不能收（**唯一的背压信号**）；
 * - `on_done` —— 这件事结束了没有、怎么结束的。
 */
class UVCPP_API uvcpp_web_file_sink {
 public:
  virtual ~uvcpp_web_file_sink() {}

  /**
   * @brief 交付一块数据（`len` 恒大于 0）。
   *
   * **必须在这一句之内把数据拷走或消耗掉**：返回之后下一个 `uv_fs_read` 就会
   * 覆盖同一块缓冲。也就是说不接受"先记下指针，回头再处理"。
   *
   * 这个方法**不会**被重入 —— transfer 只在 fs 完成回调里调它，而同一时刻只有
   * 一笔 fs 操作在途。但**它可以在这个方法内部调 `cancel()`**（对端在这一句里
   * 断了是常态），transfer 会在 `on_data` 返回之后立刻看到并走关闭路径。
   */
  virtual void on_data(const char* data, size_t len) = 0;

  /**
   * @brief 当前还压着多少字节没有送出去。
   *
   * 这是**唯一**的背压信号：transfer 每交付完一块问一次，达到高水位就停止提交
   * 下一笔读，直到 `resume()` 被调用。所以一个"永远返回 0"的 sink 会让窗口只剩
   * 切片缓冲那一份 —— 那是允许的，只是背压不起作用。
   */
  virtual size_t backlog() const = 0;

  /**
   * @brief 结束通知，**恰好一次**。
   *
   * `status == 0` 表示 `[first, last]` 全部交付完毕；非 0 表示出错或被取消
   * （`UV_ECANCELED`），`bytes_sent` 是**已经交付出去**的字节数。
   *
   * 走到这里时 fd 已经关完（本方法由 `uv_fs_close` 的完成回调调用），所以在这一
   * 句里销毁 transfer 是安全的。
   *
   * **这一句之后不得再碰 transfer 的任何成员** —— 最后一次 `shared_ptr` 很可能
   * 就在这里释放，成员已经不存在了。
   */
  virtual void on_done(int status, uint64_t bytes_sent) = 0;
};

// =========================================================================
// 传输
// =========================================================================

/**
 * @brief 把一个文件的 `[first, last]`（**闭区间**）分片读出来交给 sink。
 *
 * 用法：
 * @code
 *   std::shared_ptr<uvcpp_web_file_transfer> t(
 *       new uvcpp_web_file_transfer(loop, &my_sink));
 *   if (t->start(path, 0, size - 1) != 0) { ... }   // 提交失败，不会有回调
 *   // 对端断开时：t->cancel();
 * @endcode
 *
 * **必须由 `shared_ptr` 持有**（内部用 `shared_from_this()`）。
 */
class UVCPP_API uvcpp_web_file_transfer
    : public std::enable_shared_from_this<uvcpp_web_file_transfer> {
 public:
  /// 默认切片：256 KiB。
  ///
  /// 这个数不是随手取的：它要**明显大于** TCP 单次写的合理批量（否则每片都要
  /// 一次 `uv_fs_read` 的往返，吞吐被线程池调度粒度拖住），又要**明显小于**高
  /// 水位（否则"高水位"还没到就已经堆了两片，上界失去意义）。
  static const size_t k_default_slice_bytes = 256u * 1024u;

  /// 默认高水位：1 MiB。
  ///
  /// 与 `uvcpp_web_response::set_max_stream_buffer_bytes()` 的默认值一致 ——
  /// 两边不是巧合地相同：接收方那个缓冲就是这个 sink 的 `backlog()`，所以高水位
  /// 超过它没有意义（transfer 会停在一个接收方根本到不了的值上）。
  static const size_t k_default_high_water_bytes = 1024u * 1024u;

  uvcpp_web_file_transfer(uvcpp_loop* loop, uvcpp_web_file_sink* sink);
  ~uvcpp_web_file_transfer();

  // 不可拷贝：内部有 fd、缓冲与在途操作。
  uvcpp_web_file_transfer(const uvcpp_web_file_transfer&) = delete;
  uvcpp_web_file_transfer& operator=(const uvcpp_web_file_transfer&) = delete;

  /// 切片大小；0 表示用默认值。
  void set_slice_bytes(size_t n);
  /// 高水位；0 表示用默认值。
  void set_high_water_bytes(size_t n);
  size_t slice_bytes() const;
  size_t high_water_bytes() const;

  /**
   * @brief 读到文件尾就算正常结束（默认 **关**）。
   *
   * **关**（默认）：`[first, last]` 是调用方**声明过**的区间。读到 EOF 而区间
   * 还没走完，说明文件比声明时短了（被截断、或被换掉了）—— 那是错误
   * （`UV_EOF`），不是"正常发完了"。上层已经把 `Content-Length` 送上线了，
   * 悄悄少发几个字节等于交给对端一个长度对不上的 body，而它要读到尾才发现。
   *
   * **开**：**没有任何长度被声明过**，于是 EOF 就是这条数据流的天然终点
   * （`on_done` 以 0 结束）。给 `uvcpp_web_response::send_file(path)` 这种
   * "长度未知 ⇒ 走 chunked"的下发用。
   *
   * 开的时候调用方传的 `last` 应当是 `UINT64_MAX - 1`，**不是** `UINT64_MAX`
   * —— `submit_read()` 里的 `remain = last_ - offset_ + 1u` 在后者的第一个
   * 切片上就溢出成 0，于是当场收尾、一个字节都不读。
   *
   * **必须在 `start()` 之前设** —— 状态机一开始跑，这个标志就只是给已经
   * 提交出去的那些读当判据用了。
   */
  void set_stop_at_eof(bool on);
  bool stop_at_eof() const;

  /**
   * @brief 开始下发 `[first, last]`（闭区间，`0 <= first <= last` 由调用方保证
   *        —— 静态服务本来就要 stat 来判 ETag/Range，结果直接传进来即可）。
   *
   * `first > last` 是合法的"空区间"：**仍然会走去 open**（所以 ENOENT 之类的
   * 错误照常上报），只是不读任何字节就结束。这样做的唯一目的是保住下面那条
   * 契约。
   *
   * **契约：`start()` 绝不会同步回调。** 每一次 `on_done` 都来自某笔 fs 操作的
   * 完成回调，所以调用方可以放心地在 `start()` 返回之后继续设置自己的状态。
   *
   * @return 0 = 已提交；非 0 = **提交失败**（参数非法，或 libuv 同步拒绝了这笔
   *         操作），**此时不会有任何回调** —— 与 libuv「同步失败不排队」的约定
   *         一致，免得同一个失败被通知两次。调用方只需丢掉自己的 `shared_ptr`。
   */
  int start(const std::string& path, uint64_t first, uint64_t last);

  /**
   * @brief 接收方腾出空间了，恢复被背压停下的读。
   *
   * 只在"因背压停下"时有意义，其余情况是空操作（幂等）。被 `cancel()` 之后
   * 也是空操作 —— 已经决定不发了，再读就是白读。
   */
  void resume();

  /**
   * @brief 取消这次传输（对端断开 / 停机）。幂等。
   *
   * 语义是"**已经交出去的字节就那样了，别再读新的**"：当前在途的那一笔读会
   * 跑完（`uv_fs_read` 已经开始，停不下来），然后走关闭路径，`on_done` 以
   * `UV_ECANCELED` 被调用一次。
   *
   * `start()` 之前调用是空操作 —— 还没有任何东西需要取消，`on_done` 也不会
   * 因此被触发（它只对"已经开始过的传输"有承诺）。
   */
  void cancel();

  /// `on_done` 是否已经跑过。
  bool done() const;
  /// 是否被 `cancel()` 过。
  bool cancelled() const;
  /// 已经交付给 sink 的字节数。
  uint64_t bytes_sent() const;

  /// 当前占用字节数的上界：切片缓冲 + 接收方压着的量。
  size_t window_bytes() const;
  /// 上述值的峰值。`mem_bounded` 那一组断言的就是它。
  size_t peak_window_bytes() const;
  /// 提交过多少笔 `uv_fs_read`（观测口：短读会让它比"片数"多）。
  size_t read_submits() const;
  /// 因为背压停读的次数（观测口：钉"背压真的起过作用"）。
  size_t pause_events() const;

 private:
  enum state {
    st_idle,     ///< `start()` 之前
    st_opening,  ///< `uv_fs_open` 在途
    st_reading,  ///< `uv_fs_read` 在途
    st_paused,   ///< 因背压停读，没有 fs 操作在途
    st_closing,  ///< `uv_fs_close` 在途
    st_done      ///< `on_done` 已经跑过
  };

  void submit_read();
  /// 走关闭路径：有 fd 就异步关，`on_close_done` 里再 `finish()`。
  /// **必须是所在函数的最后一句** —— 同步失败分支会当场把 `on_done` 发出去，
  /// 而 `on_done` 可能销毁本对象。
  void submit_close(int status);
  /// 没有 fd 可关时直接收尾（open 失败）。同样是"最后一句"。
  void finish(int status);

  void on_open_done(long long rc);
  void on_read_done(long long nread);
  void on_close_done(int status);

  /// 采样窗口占用（每次状态跳变时调用）。
  void note_window();

  uvcpp_loop* loop_;
  uvcpp_web_file_sink* sink_;
  std::unique_ptr<uvcpp_fs> fs_;

  std::string path_;
  /// 复用同一块切片缓冲（见文件头的"一片缓冲就够"）。
  std::string slice_buf_;
  size_t slice_bytes_;
  size_t high_water_bytes_;
  /// 读到 EOF 即正常结束（见 `set_stop_at_eof`）。
  bool stop_at_eof_;

  uint64_t first_;
  uint64_t last_;
  uint64_t offset_;      ///< 下一个要读的绝对文件偏移
  uint64_t bytes_sent_;

  size_t peak_window_;
  size_t read_submits_;
  size_t pause_events_;

  int state_;
  bool abort_requested_;
  /// 关闭完成时要回报的状态：`0` = 正常发完，其余 = 出错码。
  int close_status_;
  /// 打开成功后的 fd；负数表示没有可关的。
  int fd_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_FILE_H
