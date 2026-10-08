/**
 * @file src/quic/uvcpp_quic_common.h
 * @brief QUIC 传输层的公开小件：ALPN 默认值、状态枚举、读事件回调、后端探针。
 * @author zhuweiye
 * @version 1.5.1
 *
 * **1.4.1 的 QUIC 是一条真能通信的链路协议**：握手、流收发、连接关闭与空闲超时
 * 都通了，一条连接上的多条流共用同一条 UDP 口。**没做的**是 HTTP/3（nghttp3 连
 * 依赖都没接）、0-RTT、连接迁移、datagram（RFC 9221）与 multipath —— 这份清单
 * 在 `doc/quic-guide.md` §8 里逐条列着，别在别处另维护一份。
 *
 * 本头**不包含** `<ngtcp2/ngtcp2.h>`（理由见 `uvcpp_quic_ngtcp2.h`）。
 */

#ifndef SRC_QUIC_UVCPP_QUIC_COMMON_H
#define SRC_QUIC_UVCPP_QUIC_COMMON_H

#include <uvcpp/uvcpp_config.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <uvcpp/uvcpp_export.h>

// QUIC 是 net 层的一条链路协议，它**复用 net 层那套"带语义的读事件"**：
// `net_read_event::DATA / PEER_CLOSED / READ_ERROR` 加错误码。
//
// 复用是有意的，不是偷懒：QUIC 的流在应用层看到的形状与一条 TCP 连接**逐字相同**
// （有数据 / 对端收了 / read 出错），连"`nread < 0` 时原始回调什么都不给、于是
// 用户写的读循环普遍是错的"这个坑都一模一样。再造一套平行的事件枚举，只会让
// 上层为两套名字相同的语义各写一遍分支，而且两套迟早漂移。
//
// 这条依赖也是构建期的一条守卫：`CMakeLists.txt` 里 `UVCPP_ENABLE_QUIC=ON` 要求
// `UVCPP_BUILD_NET=ON`，就是它逼出来的。
#include "net/uvcpp_net_read.h"

#if UVCPP_QUIC_ENABLE

namespace uvcpp {

class uvcpp_quic_connection;  // 见 src/quic/uvcpp_quic_connection.h

// =========================================================================
// ALPN
// =========================================================================

/**
 * @brief 本层默认的 ALPN 协议名。
 *
 * `"h3"`（RFC 9114 §3.1）。QUIC 的版本协商走 TLS 的 ALPN 扩展，所以**没有明文
 * QUIC** —— 这正是 `UVCPP_ENABLE_QUIC=ON` 必须 `UVCPP_ENABLE_OPENSSL=ON` 的原因，
 * 而且那份 OpenSSL 得带 QUIC API（≥ 3.2）。
 *
 * 返回 `const char*` 而不是 `std::string`：这是一个**字面量**，不是一个算出来的
 * 值，调用方要拷就自己拷。也**不加** `std::vector<std::string>` 那份重载 ——
 * 那会让每个调用点都多一次堆分配，只为了包一个长度已知的 C 串。
 *
 * `uvcpp_quic_client::set_alpn_protos()` 与 `uvcpp_quic_server::set_alpn_select_protos()`
 * 不设时，交给 TLS 的就是它。
 */
inline const char* quic_default_alpn() {
  return "h3";
}

// =========================================================================
// 连接 / 流状态
// =========================================================================

/**
 * @brief 一条 QUIC 连接在本层的生命周期（RFC 9000 §10）。
 *
 * 由 `uvcpp_quic_connection::state()` 报出来，也是
 * `quic_detail::quic_session` 内部那个状态机的对外投影。
 *
 * 与 `uvcpp_h2_common.h` 里 `h2_stream_state` 同一套做法：只命名**有具名语义**
 * 的几个，其余一律以 ngtcp2 的原始返回值透传 —— **不在这里造一张平行表**，
 * 那种表会和上游一起漂移，而且漂移是静默的。
 *
 * @note 没有"`ESTABLISHED` 之前不能开流"这条限制：QUIC 允许在握手完成前就把流
 *       开出去（`ngtcp2_conn_open_bidi_stream` 的说明里写着可以在握手前调），
 *       那些字节排在 0-RTT 队列里，等密钥齐了一起走。本库不做 0-RTT，所以
 *       实践中第一个应用数据字节总在握手之后 —— 但那是**时序**，不是这里的
 *       一条规则，用例不该拿 `state()` 当"能不能开流"的判据。
 */
enum class quic_connection_state {
  IDLE,         ///< 还没有对端。客户端在此状态可以去 `connect()`。
  HANDSHAKING,  ///< 初始包已发/已收，握手进行中（此时已有 1-RTT 之外的空间可用）
  ESTABLISHED,  ///< 握手完成，可以开流。**这是唯一能收发应用数据的稳态**
  CLOSING,      ///< 已发出 CONNECTION_CLOSE，等对端确认（RFC 9000 §10.2.1）
  DRAINING,     ///< 收到对端的 CONNECTION_CLOSE，只等计时器到期（§10.2.2）
  CLOSED,       ///< 连接已终结，对象可以销毁
};

// 这里**故意没有** `quic_stream_state` —— 别补。
//
// `quic_connection_state` 上面有一个消费者：`uvcpp_quic_connection::state()`，
// 并且测试断言在它上面。流这边没有对应的东西：1.4.1 也没有 `uvcpp_quic_stream`
// 这个类，流的公开面就是 `stream_id` 本身，**没有任何访问器会返回一个流状态**，
// 于是那样一个枚举一个消费者都没有。
//
// 本仓库对"没有调用方的名字"是不发的（自研 varint 编解码当初也是这么砍掉的：
// 包解析归 ngtcp2，自己写一份就是死代码）。公开头里的死名字比私有死代码更坏 ——
// 它会进 `include/quic/`、进 API 兼容面，被下一个读代码的人当成承重结构。
// 将来真做流的时候再建：那时它会有访问器做消费者，形状也该由那时的实现来定，
// 而不是由今天这个还没有实现的猜测来定。

// =========================================================================
// 读事件
// =========================================================================

/**
 * @brief 框架层 QUIC 读回调。
 *
 * @param conn      事件所属的连接。
 * @param stream_id **事件属于哪条流**（QUIC 流号，`int64_t`）。
 * @param result    事件内容（**复用 net 层那套语义**，见本文件开头）。
 *
 * 在 **loop 线程**上调用。回调返回后 `result.data` 失效。
 *
 * @note 这里**没有**直接复用 `uvcpp_net_read_cb`，尽管两者的 `result` 参数逐字
 *       相同。区别只在第一个参数：那个 typedef 写死了 `uvcpp_tcp_client&`，
 *       而一条 QUIC 连接既不是、也不该被伪装成一个 TCP 客户端 —— 硬套上去要么
 *       让 `uvcpp_quic_connection` 去继承 `uvcpp_tcp_client`（它根本没有
 *       `uv_stream_t`，继承不了），要么就得在回调里伪造一个 TCP 客户端引用。
 *       所以复用落在**语义**那一层（`net_read_result` / `net_read_event`），
 *       那是真正共用的部分；第一个参数按本层自己的类型走。
 *
 * @note 加 `stream_id` 是 QUIC 与 TCP 的**本质**差别，不是灵活性：一条 QUIC
 *       连接上并行跑着很多条流，而 TCP 上只有一条字节流。不加这个参数，调用方
 *       根本没法说清"收到的这半句话是哪一问的回答"。`src/http2/` 那边的
 *       `uvcpp_h2_session` 回调同样是 `(session&, stream_id, …)` 的形状 ——
 *       多路复用层都长这样，不是巧合。
 */
typedef std::function<void(uvcpp_quic_connection&, int64_t stream_id,
                           const net_read_result&)>
    uvcpp_quic_read_cb;

// =========================================================================
// 后端探针
// =========================================================================

/**
 * @brief 编进来的 ngtcp2 是哪个版本（形如 `"1.25.0"`）。
 *
 * 取值来自 ngtcp2 自己生成的 `version.h` 里的 `NGTCP2_VERSION` 宏。
 *
 * @note 这是一个**编译期**字符串 —— 它证明"ngtcp2 的头路径到得了"，**不证明
 *       链上了**。要证明链接，用下面的 `quic_error_string()`（真调进
 *       `ngtcp2_static`）与 `quic_crypto_backend_init()`（真调进
 *       `ngtcp2_crypto_ossl_static`）。三条各占一条断言，见
 *       `tests/functional/quic_api_func.cpp`。
 *
 * ngtcp2 没有 `ngtcp2_version()` 这样的函数（版本只以宏的形式存在），所以这里
 * 不是"没调那个函数"，而是**它不存在**。
 */
UVCPP_API std::string quic_ngtcp2_version_string();

/**
 * @brief 把 ngtcp2 的错误码翻译成它自己的文本（`ngtcp2_strerror()` 的返回值）。
 *
 * 存在的理由有两条，第二条才是主要的：
 *
 * 1. 上层拿到一个负的 ngtcp2 错误码时需要有地方问"这是什么意思"。这张表是
 *    **上游的**，我们只转发 —— 本库不自己维护一份，理由同 `uvcpp_h2_common.h`
 *    里那段"不造平行表"。
 *
 * 2. **它是一条真实的链接证据。** `quic_ngtcp2_version_string()` 只读一个宏，
 *    一个"头路径骗到了、libngtcp2 一个字节都没链上"的树照样能把它编出来并跑绿。
 *    这个函数真调进 libngtcp2 的符号，少链就是链接期未定义符号 —— 用例**根本
 *    编不出可执行文件**。所以测试里必须断言它。
 *
 * `ngtcp2_strerror(0)` 返回 `"NO_ERROR"`，是个稳定字面量，正好拿来当断言。
 *
 * 认不出的码返回 `"(unknown)"`（**上游的原文**，不是我们编的 —— 别顺手改成大写，
 * 测试拿它当判据）。
 */
UVCPP_API std::string quic_error_string(int code);

/**
 * @brief 初始化 ngtcp2 的 OpenSSL crypto 后端。
 *
 * **这是本库唯一一处真正调进 `ngtcp2_crypto_ossl_static` 的地方，而那个目标恰好
 * 是本模块里最容易坏的一半**：`ngtcp2_static` 是协议状态机，
 * `ngtcp2_crypto_ossl_static` 才是把它接到 OpenSSL 上的那一半；两个目标里只有
 * 后者取决于"那份 OpenSSL 是不是带 QUIC API 的 mainline ≥ 3.2" —— 树不对时它
 * **根本不会被建出来**（上游那时只建 `ngtcp2_crypto_quictls_static`）。
 * 所以链接到它，就等于在运行期把 `CMakeLists.txt` 里那段 QUIC API 预检又证了一遍。
 *
 * **调用时机**：进程内调一次即可，在任何 `uvcpp_quic_client` / `uvcpp_quic_server`
 * 之前。它内部按名字 `EVP_*_fetch` 一批算法实现（AES-GCM / CCM、ChaCha20-Poly1305、
 * SHA-256/384、HKDF），存进一批**进程级**静态变量。
 *
 * @return 0 成功。上游的实现**从不失败**（fetch 不到也只是留 NULL，它自己说"我们
 *         不管预取成不成功"），所以非 0 只可能来自将来更严的版本 —— 调用方照样
 *         该看返回值，别写 `assert(rv == 0)`。
 *
 * @warning **不要调两次。** 上游不查重：第二次会把同一批对象再 `fetch` 一遍、
 *          覆盖掉第一次的指针，第一次那批引用计数就此丢掉。要"调了再调"就先
 *          `quic_crypto_backend_free()`。幂等性不是它承诺的东西。
 *
 * @note 与 `quic_crypto_backend_free()` **必须配对**：`fetch` 回来的对象是带
 *       引用计数的，不还就是进程退出前一直持有。给测试用的那种"只验一次"的
 *       往返（`init(); free();`）是安全且无残留的 —— 两个函数在同一个实现文件里，
 *       看 `uvcpp_quic_ngtcp2.cpp`。
 */
UVCPP_API int quic_crypto_backend_init();

/**
 * @brief 释放 `quic_crypto_backend_init()` 抓回来的那批 EVP 对象。
 *
 * 可重复调用（内部每个指针释放后都置 NULL）。本库**不**在静态析构里替你调它 ——
 * 谁 `init` 谁 `free`，与库里其它成对的资源一个规矩。
 */
UVCPP_API void quic_crypto_backend_free();

}  // namespace uvcpp

#endif  // UVCPP_QUIC_ENABLE

#endif  // SRC_QUIC_UVCPP_QUIC_COMMON_H
