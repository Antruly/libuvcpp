/**
 * @file src/redis/uvcpp_redis_resp.h
 * @brief RESP2/RESP3 的增量解析与编码 —— **私有头**，不是 API。
 * @author zhuweiye
 * @version 1.7.5
 *
 * 与 `src/net/uvcpp_socks5_common.h` 同一个定位：协议编解码是本模块的内部
 * 契约，公开面只到 `uvcpp_redis_value` 为止。安装规则与
 * `tests/tools/package_release.py` 的 `PRIVATE_HEADERS` 两处都要排除本文件。
 *
 * 解析器是**增量**的：数据按到达顺序喂进来，`next()` 要么给出一个完整值、
 * 要么说"还不够"。不够时它**不动**偏移量，所以调用方可以原样保留缓冲、等下一段
 * 字节到了再试 —— 不需要自己维护"解析到哪一步"的状态。
 *
 * 分片到达时同一段前缀会被重复解析（简单、不会错）：Redis 的单个应答很少大到
 * 值得为它做状态机，而多解析几百字节的代价远小于一份写错的状态恢复逻辑。
 */

#pragma once
#ifndef SRC_REDIS_UVCPP_REDIS_RESP_H
#define SRC_REDIS_UVCPP_REDIS_RESP_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_REDIS_ENABLE

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <redis/uvcpp_redis.h>

namespace uvcpp {

/**
 * @brief `uvcpp_redis_value` 的不可变节点。
 *
 * 只在本目录的两个 .cpp 之间存在（公开头里只有它的前置声明）。`pairs` 只给
 * `MAP` 用，`elems` 给 `ARRAY` / `SET` 用 —— 不合并成一个"扁平列表"，是因为
 * 合并之后 `at()` 对 MAP 的语义会变得含糊。
 */
struct uvcpp_redis_value_impl {
  uvcpp_redis_value_impl() : type(uvcpp_redis_type::NULL_VALUE), integer(0),
                             number(0.0), boolean(false) {}

  uvcpp_redis_type type;
  std::string      text;     ///< STATUS / ERROR / BULK / DOUBLE 的文本
  std::int64_t     integer;  ///< INTEGER
  double           number;   ///< DOUBLE
  bool             boolean;  ///< BOOLEAN
  std::vector<uvcpp_redis_value> elems;  ///< ARRAY / SET
  std::vector<std::pair<uvcpp_redis_value, uvcpp_redis_value>> pairs;  ///< MAP
};

/**
 * @brief 构造 `uvcpp_redis_value` 的唯一入口（解析器与值类之间的桥）。
 *
 * `uvcpp_redis_value` 的构造是私有的；它是本模块内部唯一被允许建值的地方。
 * 放在这里而不是公开头，是因为"谁能造值"本身就是内部契约的一部分。
 */
struct uvcpp_redis_value_factory {
  static uvcpp_redis_value make(
      const std::shared_ptr<const uvcpp_redis_value_impl>& p) {
    return uvcpp_redis_value(p);
  }
};

/**
 * @brief RESP 增量解析器。
 *
 * 用法：
 * @code
 *   uvcpp_redis_parser parser;
 *   std::size_t off = 0;
 *   bool push = false;
 *   uvcpp_redis_value v;
 *   const auto r = parser.next(buf.data(), buf.size(), off, v, &push);
 *   if (r == uvcpp_redis_parser::DONE) { 消费 off 之前的字节，v 是一个值 }
 *   else if (r == uvcpp_redis_parser::NEED_MORE) { 等更多字节 }
 *   else { 协议错误：parser.last_error() / last_error_text() }
 * @endcode
 */
struct UVCPP_API uvcpp_redis_parser {
  enum result {
    NEED_MORE = 0,  ///< 数据不够，`off` 未动，可以再来一次
    DONE      = 1,  ///< 解析出一个完整值，`off` 已前移
    BAD       = 2   ///< 协议错误，连接应当断开
  };

  uvcpp_redis_parser();

  /// 单个 bulk 的上限（默认 512 MiB）；超过按协议错误处理。
  std::size_t max_bulk;
  /// 单个聚合（数组/映射/集合/push）的元素上限（默认 1,048,576）。
  std::size_t max_elements;
  /// 嵌套层数上限（默认 64）。
  int max_depth;

  /**
   * @brief 解析 `buf[off]` 开始的一个完整值。
   *
   * @param buf      数据
   * @param len      数据长度
   * @param off      入口：起点；出口：DONE 时是值的下一个字节（NEED_MORE / BAD
   *                 时保持不变）
   * @param out      解析结果
   * @param is_push  可选；值为真表示这一帧是 RESP3 的 push（`>`）。RESP2 下恒为假
   * @return 见 `result`
   */
  result next(const char* buf, std::size_t len, std::size_t& off,
              uvcpp_redis_value& out, bool* is_push = nullptr);

  /** @brief 最近一次 `BAD` 的错误码（libuv 错误码，通常是 `UV_EPROTO`）。 */
  int last_error() const { return last_error_; }

  /** @brief 最近一次 `BAD` 的人类可读说明。 */
  const std::string& last_error_text() const { return last_error_text_; }

  /** @brief 记录一次协议错误并返回 `BAD`（实现内部用）。 */
  result bad(int code, const std::string& text);

  /** @brief 把一条命令编码成 RESP 请求（数组 + bulk 字符串）。 */
  static void encode_command(const std::vector<std::string>& args,
                             std::string& out);

  /** @brief 把多条命令按顺序编码进同一个缓冲（管线）。 */
  static void encode_commands(
      const std::vector<std::vector<std::string>>& commands, std::string& out);

 private:
  int         last_error_;
  std::string last_error_text_;
};

}  // namespace uvcpp

#endif  // UVCPP_REDIS_ENABLE

#endif  // SRC_REDIS_UVCPP_REDIS_RESP_H
