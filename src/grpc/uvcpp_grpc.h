/**
 * @file src/grpc/uvcpp_grpc.h
 * @brief gRPC 的**信封**：5 字节长度前缀帧 + 够用的一小撮 protobuf wire format。
 * @author zhuweiye
 * @version 1.6.10
 *
 * 为什么把这两样放进库里，而不是让每条 gRPC 路由自己写一遍
 * ------------------------------------------------------
 * 因为它们是**协议事实**，不是应用代码：帧格式定在 gRPC 的
 * `PROTOCOL-HTTP2.md` 里，varint 定在 protobuf 的编码规范里，两边都不随调用方
 * 变化。放进库里的直接好处是这条路**有头注释、有用例、有门禁**；散在路由里则
 * 是每个用它的人各写一遍、各错一遍（最容易错的两处：长度前缀是**大端**，
 * 而负的 `int32` 要**符号扩展到 64 位**再编成 10 字节的 varint —— 少这一步，
 * 对端解出来是个正数，而这条路径上没有任何东西会报错）。
 *
 * 边界（写在前面，免得被当成通用 protobuf 运行时）
 * ------------------------------------------------
 * **只做 varint（wire type 0）字段，外加"跳过别的 wire type"。** 不做嵌套消息、
 * 不做 map、不做 packed repeated、不做 group（wire type 3/4 直接判非法）、不做
 * 反射与描述符。
 *
 * 为什么不上 libprotobuf：本库的依赖就是 libuv 加可选的那几个协议库，为两个
 * `int32` 拉进一整套描述符运行时不成比例。需要完整 protobuf 的场合请照常生成
 * 代码，把编码好的字节交给 `grpc_frame_encode()` —— 这一层只认"一段字节"，
 * 与它怎么来的无关，两件事的边界正好切在这里。
 *
 * 这一半（编解码）**不依赖 web 层**，任何配置下都在；服务端那一半在
 * `src/grpc/uvcpp_grpc_server.h`。
 */

#pragma once
#ifndef SRC_GRPC_UVCPP_GRPC_H
#define SRC_GRPC_UVCPP_GRPC_H

#include <uvcpp/uvcpp_config.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>

#include <uv.h>  // UV_EINVAL / UV_ENOENT / UV_EMSGSIZE / UV_ENODATA（兜底见下）
#include <uvcpp/uvcpp_export.h>

// libuv 直到 **1.49.0** 才把 `ENODATA` 放进公开错误码表（`uv.h` 里那条
// `XX(ENODATA, "no data available")`）；1.44~1.48 的发行版没有这个名字
// （Ubuntu 22.04 / Debian 12 是 1.44.2，Ubuntu 24.04 是 1.48）。发行版包按
// **系统** libuv 构建，而本模块的返回值契约里就有它（见下面几个函数的
// @return），所以在这里补一份。取值与 libuv 自己的 `UV__ENODATA` 逐位相同：
// 有 `ENODATA` 的平台是 `-ENODATA`，FreeBSD 用 -9919，其余用 -4024 —— 抄的是
// libuv `uv/errno.h` 里同一段分支，别自己另造一个值。
//
// 判据只能按**版本号**：`UV_ENODATA` 在 libuv 里是枚举常量（`UV_ERRNO_MAP`
// 展开出来的），`defined(UV_ENODATA)` 永远是假 —— 写成 `#ifndef` 会在 1.49+
// 上把这个名字变成宏，把后面的枚举用法顶掉。
#if !defined(UV_ENODATA) && defined(UV_VERSION_HEX) && UV_VERSION_HEX < 0x013100
#  if defined(ENODATA) && !defined(_WIN32)
#    define UV_ENODATA (-(ENODATA))
#  elif defined(__FreeBSD__)
#    define UV_ENODATA (-9919)
#  else
#    define UV_ENODATA (-4024)
#  endif
#endif

namespace uvcpp {

// =========================================================================
// 协议常量
// =========================================================================

/// 帧前缀的字节数：1 字节压缩标志 + 4 字节**大端**长度。
constexpr size_t GRPC_FRAME_HEADER_SIZE = 5u;

/// 单条消息的默认上限，与 gRPC 自己那条默认（4 MiB）同值。
constexpr size_t GRPC_FRAME_MAX_MESSAGE_BYTES = 4u * 1024u * 1024u;

/**
 * @brief gRPC 的状态码，`grpc-status` 尾随头的取值。
 *
 * 与 HTTP 的状态码**不是一回事**，这是 gRPC 里最容易被误解的一处：一条
 * "查无此项"的应答在 HTTP 层是 `200 OK`，真正的结论在这个码上，而它按规范走
 * **尾随头**（`grpc-status`），不走首部。所以只看 HTTP 状态码的客户端永远看不出
 * 失败 —— 名字与数值都取自 gRPC 的 `status_code` 定义，别改。
 */
enum class grpc_status : int {
  OK = 0,                  ///< 成功
  CANCELLED = 1,           ///< 调用方取消
  UNKNOWN = 2,             ///< 服务端未知错误
  INVALID_ARGUMENT = 3,    ///< 请求体不合法（本模块的默认失败码）
  DEADLINE_EXCEEDED = 4,   ///< 超时
  NOT_FOUND = 5,           ///< 找不到
  ALREADY_EXISTS = 6,      ///< 已存在
  PERMISSION_DENIED = 7,   ///< 无权限
  RESOURCE_EXHAUSTED = 8,  ///< 配额/容量用尽（消息超过单条上限时用它）
  FAILED_PRECONDITION = 9, ///< 前置条件不满足
  ABORTED = 10,            ///< 中止
  OUT_OF_RANGE = 11,       ///< 越界
  UNIMPLEMENTED = 12,      ///< 未实现
  INTERNAL = 13,           ///< 内部错误
  UNAVAILABLE = 14,        ///< 不可用
  DATA_LOSS = 15,          ///< 数据丢失
  UNAUTHENTICATED = 16,    ///< 未认证
};

// =========================================================================
// 帧：5 字节前缀 + 载荷
// =========================================================================

/**
 * @brief 把 @p message 编成一帧，**追加**到 @p out 的末尾。
 *
 * 追加而不是覆盖：流式那条路上调用方要的是"往同一个缓冲区里接着放一帧"，
 * 覆盖语义会让每帧都白拷一次。
 *
 * **压缩标志恒 0**：本库不做 gRPC 的消息压缩（`grpc-encoding` 那条路），
 * 写 0 与"没压"是同一件事；标志位本身留在那里，将来要加看得见往哪加。
 *
 * @return 0；`UV_EMSGSIZE`（@p message 超过 @p max_message_bytes，
 *         **此时 @p out 一个字节都不动**）。
 */
UVCPP_API int grpc_frame_encode(
    const std::string& message, std::string& out,
    size_t max_message_bytes = GRPC_FRAME_MAX_MESSAGE_BYTES);

/**
 * @brief 从 @p data 的开头解出一帧（流式用法）。
 *
 * @param consumed 成功时写回**这一帧占用的总字节数**（前缀 + 载荷），调用方据此
 *        推进游标、接着解下一帧；失败时写 0（调用方不必先自己清一遍）。
 * @param err 可选，写一句给人看的说明（不参与判据）。
 *
 * @return 0；
 *   - `UV_ENODATA` —— 字节还不够（长度前缀说要 N 字节，手里只有 M < N）。
 *     **这不是格式错误，是"再读一段"**：流式解码器就是靠这个码决定要不要等；
 *   - `UV_EMSGSIZE` —— 长度前缀报的值超过 @p max_message_bytes。与上一条分开，
 *     是因为它对应对端说"这条消息很大"，而调用方对这个情形的处置（换一个够大的
 *     上限，或者回一个 `RESOURCE_EXHAUSTED`）与"字节坏了"完全不同；
 *   - `UV_EINVAL` —— 字节本身不合法（压缩标志非 0）。
 */
UVCPP_API int grpc_frame_decode(
    const char* data, size_t len, std::string& message, size_t& consumed,
    std::string* err = nullptr,
    size_t max_message_bytes = GRPC_FRAME_MAX_MESSAGE_BYTES);

/**
 * @brief 解**恰好一帧**：整段必须刚好是一帧，多一个字节、少一个字节都算错。
 *
 * unary 的请求体按定义只有一帧（gRPC 的 `PROTOCOL-HTTP2.md`：unary 就是
 * "一问一答"），所以服务端这一侧用"恰好"比用"至少"更严：多出来的字节只可能是
 * 对端把两条请求拼在了一起，或者一条**流式**请求打到了 unary 方法上 ——
 * 两种都不该被当成正常请求照跑（照跑的后果是第二条消息被静默丢弃，而调用方
 * 认为它成功了）。
 */
UVCPP_API int grpc_frame_decode_exact(
    const char* data, size_t len, std::string& message,
    std::string* err = nullptr,
    size_t max_message_bytes = GRPC_FRAME_MAX_MESSAGE_BYTES);

// =========================================================================
// varint：protobuf 的 base-128
// =========================================================================

/**
 * @brief 编一个 varint，**追加**到 @p out 的末尾。
 *
 * 小端组：每字节 7 位，最高位是"后面还有"的续位。
 *
 * @return 恒 0 —— 这个编码没有失败路径（保留返回值只是为了与同层的其它函数
 *         长得一样，调用点不必分叉）。
 */
UVCPP_API int grpc_varint_encode(std::string& out, uint64_t value);

/**
 * @brief 解一个 varint。
 *
 * 合法 varint 最长 **10 字节**：64 位按 7 位一组要 10 组，第 10 个字节只带
 * 最高那一位。所以第 10 个字节要么是 `0x00`、要么是 `0x01`，**而且不能再挂
 * 续位** —— 超了就是格式错，不是"读不完"（一个合法的 64 位 varint 永远读得完）。
 *
 * @param consumed 成功时写回用掉的字节数。
 *
 * @return 0；`UV_ENODATA`（字节不够，多给几个可能就读完了）；
 *         `UV_EINVAL`（第 10 个字节之后仍有续位，或第 10 个字节的值大于 1）。
 */
UVCPP_API int grpc_varint_decode(const char* data, size_t len, uint64_t& value,
                                 size_t& consumed, std::string* err = nullptr);

/**
 * @brief 放一个 varint 字段（wire type 0，tag = `field << 3`），追加到 @p out。
 *
 * **负数按 protobuf 的规矩符号扩展到 64 位再编**：`int32` 的 `-1` 是 10 字节的
 * `FF×9 01`，不是 1 字节的 `01`。这一步是 `int32` 这个类型名的定义的一部分
 * （protobuf 把负的 `int32` 按 64 位 varint 发），漏了它，对端会解出一个正数
 * 而**不是**报错。
 *
 * @return 0；`UV_EINVAL`（@p field 为 0 或超过 `(1 << 29) - 1` —— protobuf 的
 *         字段号范围就是这样，字段号 0 根本不存在）。
 */
UVCPP_API int grpc_field_put_int32(std::string& out, uint32_t field, int32_t value);

/**
 * @brief 在 @p data 这条消息里找出 `field` 号那个 varint 字段。
 *
 * 逐字段走一遍：认得的 wire type 按各自的宽度跳过（0 = varint，1 = 8 字节，
 * 2 = 长度前缀 + 载荷，5 = 4 字节），认不出的（3/4 是已废弃的 group，6/7 保留）
 * 判非法。**同一个字段号出现多次时取最后那个** —— protobuf 对奇异标量字段就是
 * 这么定的（后者覆盖前者），走"取第一个"会在对端重发字段时解出一个过时的值。
 *
 * @return 0（找到，@p value 是解出的值）；`UV_ENOENT`（这条消息里没有这个字段
 *         —— **这不是错误**，缺省值是调用方的事）；`UV_EINVAL`（字节不合法、
 *         字段号为 0、或 wire type 认不出）；`UV_ENODATA`（某个 varint 或某段
 *         载荷被截断）。
 */
UVCPP_API int grpc_field_get_int32(const char* data, size_t len, uint32_t field,
                                   int32_t& value, std::string* err = nullptr);

}  // namespace uvcpp

#endif  // SRC_GRPC_UVCPP_GRPC_H
