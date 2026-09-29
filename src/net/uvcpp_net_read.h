/**
 * @file src/net/uvcpp_net_read.h
 * @brief 框架层的读事件接口：把原始 read 回调包成有语义的事件。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么要再包一层
 * ----------------
 * 原始的 `uvcpp_tcp_client::read_start(cb)` 给用户的回调是
 * `void(uvcpp_buf*)`，而底层 libuv 给的其实是 `ssize_t nread`
 * —— 中间那层把符号信息**丢了**：
 *
 * | 底层 nread | 原始回调看到的      | 用户能推断出什么         |
 * |------------|---------------------|--------------------------|
 * | `> 0`      | 一个 uvcpp_buf      | 有数据                   |
 * | `== 0`     | 什么都不给          | 什么都没有（EAGAIN）      |
 * | `< 0`      | **什么都不给**      | **不知道，也拿不到错误码** |
 *
 * 最后一行是要命的地方：对端断开和读出错**都不会回调读回调**，只会在另一个
 * 通道（close 回调）上响一声。于是用户写出来的读循环普遍是错的 ——
 * 「数据怎么没了」既可能是对端关了、也可能是网络断了，而这两种要用完全
 * 不同的方式处理（前者正常收尾，后者要重试或上报）。
 *
 * 所以框架这一层把三件事补齐：
 *   1. **明确的语义**（DATA / PEER_CLOSED / READ_ERROR），不用解读符号；
 *   2. **错误码**（READ_ERROR 时给出 libuv 错误码）；
 *   3. **二进制安全的数据**（指针 + 长度，不是 C 字符串）。
 *
 * 顺便：起读/停读（背压）也由框架管，用户不再需要自己调 `read_start()`。
 * 这一点不只是方便 —— 「必须自己起读才会发现断开」曾经是个静默泄漏的坑。
 */

#pragma once
#ifndef SRC_NET_UVCPP_NET_READ_H
#define SRC_NET_UVCPP_NET_READ_H

#include <cstddef>
#include <functional>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

class uvcpp_tcp_client;

/**
 * @brief 一次读事件的性质。
 *
 * **命名注意**：`ERROR` 这个名字不能用 —— Windows 的 `<wingdi.h>`（经
 * `<windows.h>` → `<uv.h>` 被引进来）里有 `#define ERROR 0`，`EOF` 同理
 * 是 `<cstdio>` 的宏。所以是 `READ_ERROR` / `PEER_CLOSED`，别顺手改回去。
 */
enum class net_read_event : int {
  /** 收到了数据，`data`/`size` 有效。 */
  DATA = 0,
  /** 对端**正常**关闭（TCP FIN）。这是一次干净的收尾，不是错误。 */
  PEER_CLOSED,
  /** 读出错（连接被重置、超时等）。`error` 是 libuv 错误码（负值）。 */
  READ_ERROR,
};

/**
 * @brief 一次读事件。
 *
 * `data` 只在 `DATA` 时有效，且**只在本次回调期间有效** —— 缓冲区在这次
 * 回调返回后就会被复用/释放。要留着就得自己拷走。
 */
struct UVCPP_API net_read_result {
  net_read_event event;
  const char*    data;   ///< DATA 时有效，其余为 nullptr
  size_t         size;   ///< DATA 时的字节数（二进制安全，可能含 NUL）
  /**
   * @brief READ_ERROR 时的 libuv 错误码（负值，如 UV_ECONNRESET）。
   *
   * 其余事件为 0。
   */
  int            error;

  /**
   * @brief 这次事件是不是对端**干净地**结束了它在这个方向上的发送（FIN）。
   *
   * 与 `event` 合起来才构成完整语义：
   *
   *   - `DATA`        ：这块数据后面跟着 FIN。TCP 恒为 `false` —— 一条 TCP 字节
   *                     流上"数据"与"结束"是两次独立的 read 回调；而 QUIC 的
   *                     STREAM 帧**可以同时带数据与 FIN 位**，于是"最后一块"与
   *                     "到此为止"是同一个通知里的两件事。
   *   - `PEER_CLOSED` ：`true` 是对端发了 FIN 的正常收尾；`false` 是对端发了
   *                     **应用错误码 0 的 RESET** —— 同样是"读侧到此为止"，
   *                     但不是干净收尾，用这个字段把两种收场分开。
   *   - `READ_ERROR`  ：恒为 `false`。
   *
   * **为什么要它**：分不出 FIN 与 RESET 的调用方没法把"请求体发完了"与"请求被
   * 取消了"分开处理 —— 两者在事件上长得一模一样，而在 HTTP/3 那一层是两条完全
   * 不同的路（前者喂 `fin=1` 给 nghttp3，后者走 `close_stream()`）。
   *
   * @note 老代码不读这个字段时行为**逐字不变**：FIN 与 RESET-0 都仍然只报一次
   *       `PEER_CLOSED`，只是多带了一位信息。
   */
  bool           fin;

  /// 显式构造函数。**不用成员初始化器（NSDMI）** —— 那会让本结构体失去
  /// C++11 的聚合初始化资格。内联定义在这里，免得为这几行单开一个 .cpp。
  net_read_result()
      : event(net_read_event::DATA),
        data(nullptr),
        size(0),
        error(0),
        fin(false) {}

  bool is_data() const { return event == net_read_event::DATA; }
  /** @brief 连接是不是结束了（正常关闭或出错都算）。 */
  bool is_end() const { return event != net_read_event::DATA; }
};

/**
 * @brief 框架层读回调。
 *
 * @param client 事件所属的连接。
 * @param result 事件内容。
 *
 * 在 **loop 线程**上调用。回调返回后 `result.data` 失效。
 */
typedef std::function<void(uvcpp_tcp_client&, const net_read_result&)>
    uvcpp_net_read_cb;

}  // namespace uvcpp

#endif  // SRC_NET_UVCPP_NET_READ_H
