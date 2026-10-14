/**
 * @file src/web/uvcpp_http_compress.h
 * @brief HTTP Content-Encoding compression utilities (gzip / deflate / brotli).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides transparent body compression / decompression for the HTTP
 * client and server modules.  gzip and deflate need UVCPP_ZLIB_ENABLE=1
 * (cmake -DUVCPP_ENABLE_ZLIB=ON) and brotli needs UVCPP_BROTLI_ENABLE=1
 * (cmake -DUVCPP_ENABLE_BROTLI=ON); both default to OFF and are
 * independent of each other.  Negotiation follows the encoders that are
 * actually built in — see `parse_accept_encoding()`.
 *
 * Key differences from WebSocket Per-Message Deflate (RFC 7692):
 *  - Uses standard gzip (RFC 1952) and deflate (RFC 1950) wrap,
 *    NOT raw deflate.  This means *positive* window_bits values:
 *      gzip:    15 + 16 = 31
 *      deflate: 15       (zlib header/trailer)
 *  - Every call starts from a **fresh deflate/inflate state** — no context
 *    takeover, so each response is an independent stream.  `compress()`
 *    reaches that state by resetting a per-thread kernel instead of building
 *    a new one per call (a level-6 deflate kernel is ~256 KB, and allocating
 *    that on every response is what used to dominate `json-comp`); the bytes
 *    produced are identical either way.  Safe for concurrent use — the kernel
 *    is thread-local, so no two threads ever share one.
 *  - Auto-detection of incoming encoding via inflateInit2(…, 47).
 *  - The compression **level** is a parameter of `compress()`, defaulting to
 *    `k_compress_level_default` (= zlib's `Z_DEFAULT_COMPRESSION`, which today
 *    means level 6).  The level changes *which bytes* come out, never *whether*
 *    compression happens — so on the server side it belongs in the response's
 *    variant-cache **key** (see `uvcpp_http_server`), not in the "should we
 *    compress" decision.
 */

#pragma once
#ifndef SRC_WEB_UVCPP_HTTP_COMPRESS_H
#define SRC_WEB_UVCPP_HTTP_COMPRESS_H

#include <uvcpp/uvcpp_config.h>

#if UVCPP_WEB_ENABLE

#include <uvcpp/uvcpp_define.h>
#include <uvcpp/uvcpp_buf.h>

#include <string>
#include <vector>

namespace uvcpp {

// =========================================================================
// Compression method
// =========================================================================

/** @brief Content-Encoding values supported by this module.
 *
 *  `BROTLI` 这个枚举值**无条件存在**（它是协议层的东西：`content-encoding: br`
 *  在盘上的孪生文件那条路上一直是可以发出去的，见 `uvcpp_http_common.h` 的
 *  `http_pick_precompressed_variants()`），能不能**现场编**出 brotli 才由
 *  `UVCPP_BROTLI_ENABLE` 决定 —— `parse_accept_encoding()` 在那个宏为 0 时
 *  不会返回它（理由见那里的注释：一个发不出去的编码不能进协商结果）。
 */
enum class http_compress_method : int {
  NONE    = 0,   ///< No compression / identity
  GZIP    = 1,   ///< gzip (RFC 1952)
  DEFLATE = 2,   ///< deflate (RFC 1950, zlib format)
  BROTLI  = 3,   ///< brotli (RFC 7932)
};

// =========================================================================
// Compression result
// =========================================================================

/** @brief Result of a compress or decompress operation.
 *         Uses uvcpp_buf (not std::string) — safe for binary payloads. */
struct http_compress_result {
  bool                success = false;  ///< true if operation completed
  uvcpp_buf           data;             ///< Result payload (empty on failure)
  http_compress_method method = http_compress_method::NONE;  ///< Encoding used
};

// =========================================================================
// Compression utility functions (free functions, no state)
// =========================================================================

namespace http_compress {

/**
 * @brief 默认压缩档位。
 *
 * 值就是 zlib 的 `Z_DEFAULT_COMPRESSION`（今天等价第 6 档）。写成字面量而不是
 * 包含 `<zlib.h>`：这个头文件是公开头，不该把 zlib 的类型与宏带进每一个
 * include 它的编译单元。
 */
static const int k_compress_level_default = -1;

/** 档位取值范围的闭区间：`k_compress_level_default`(-1) 与 0..9。 */
static const int k_compress_level_min = -1;
static const int k_compress_level_max = 9;

/**
 * @brief 把任意整数收进 `[k_compress_level_min, k_compress_level_max]`。
 *
 * 夹紧而不是报错：档位错了最坏是"压得多一点或少一点"，没有任何一条路径会因为
 * 它变得不正确，所以没有理由让调用方在运行时处理一个失败返回值。越界值一律
 * 落到最近的端点上（`-5` → `-1`，`99` → `9`）。
 *
 * 服务器侧 `set_compress_level()` 存的就是这里夹过的值 —— 键里的档位字符
 * （见 `uvcpp_http_server.cpp`）依赖"两个不同的档位不会映射到同一个字符"，
 * 而未夹紧的 15 与 5 会撞在一起。
 */
UVCPP_API int clamp_compress_level(int level);

/**
 * @brief 默认 brotli 质量档。
 *
 * 5 是算出来的，不是随手取的。
 *
 * ## 两张表（本机，三份真语料 = `/json/25|40|50` 的响应体）
 *
 * 线上字节（gzip 的 +12 B 包装已含在 deflate 那一列里；brotli 自带包装）：
 *
 * | 语料 | deflate L6 | q4 | **q5** | q6 | q9 | q11 |
 * |---|---|---|---|---|---|---|
 * | 4 190 B | 970 | 923 | **856** | 853 | 852 | 740 |
 * | 6 707 B | 1 329 | 1 281 | **1 182** | 1 181 | 1 176 | 1 018 |
 * | 8 397 B | 1 566 | 1 518 | **1 389** | 1 380 | 1 370 | 1 206 |
 *
 * 单次耗时（中位数：5 批 × 200 次，**按本库的形状**量 —— deflate 那一列用的是
 * `deflateReset()` 复用内核，brotli 每一列都是每次新建状态，因为 API 里没有
 * reset，见 `uvcpp_http_compress.cpp` 的 `deflate_slot` 那段）：
 *
 * | 语料 | deflate L6 | q4 | **q5** | q6 | q9 | q11 |
 * |---|---|---|---|---|---|---|
 * | 4 190 B | 21.0 µs | 36.9 | **41.6** | 42.5 | 50.8 | 3 744 |
 * | 6 707 B | 31.3 µs | 45.2 | **64.5** | 70.0 | 88.5 | 6 047 |
 * | 8 397 B | 43.0 µs | 55.2 | **83.7** | 92.1 | 113.3 | 8 111 |
 *
 * ## 为什么停在 5
 *
 * 判据不是"哪个更小"，也不是"哪个更快"，而是 `json-comp` 那条 profile 的分数
 * —— 它对字节数是**平方**计价的（`effectiveRps = rps × (minBpr / myBpr)²`），
 * 而 CPU 只按一阶折 rps。平台基线（本轮实测读数：bpr 1430、rps 191 970、
 * 每请求 324 µs、占满 6400% 里的 97%）代进去：
 *
 * | 档位 | 混合字节（三份等权） | 比 L6 多花 | 字节那一项 | 净 |
 * |---|---|---|---|---|
 * | q4 | 1 240.7 | +14.0 µs | ×1.078 | **×1.034** |
 * | **q5** | **1 142.3** | **+31.5 µs** | **×1.274** | **×1.161** |
 * | q6 | 1 138.0 | +36.4 µs | ×1.282 | ×1.152 |
 *
 * 也就是：q4 省下的字节几乎全被它的 CPU 吃回去了；q6 比 q5 只小 0.4% 的字节、
 * 却贵 5 µs，于是**净收益反而更低**。q5 是这张表上的极大点，q9 之后（q9→q11
 * 有一条静态字典的台阶）是给离线用的。
 *
 * 注意这个默认值**只在 `json-comp` 这一类"客户端明说收 br"的档位上有意义**；
 * `json-tls` / `json-h2c` 不发 `Accept-Encoding`（已逐条核过三处 raw 文件），
 * 那里根本走不到 brotli 这一支。
 */
static const int k_brotli_quality_default = 5;

/** 质量取值范围：0（最快）.. 11（最小，慢两个数量级）。 */
static const int k_brotli_quality_min = 0;
static const int k_brotli_quality_max = 11;

/**
 * @brief 把任意整数收成合法的 brotli 质量档。
 *
 * 负数 → `k_brotli_quality_default`（**哨兵**，不是"很小的质量"）；`0..11` 原样
 * 通过；`>11` 落到 11。
 *
 * 与 `clamp_compress_level()` 同一条理由：档位错了最坏是"压得多一点或少一点"，
 * 没有哪条路径会因为它变得不正确 —— 所以夹紧而不是报错。两个函数**不能合并**
 * 成"夹进 `[a, b]`"一把尺子：deflate 的上限是 9、brotli 是 11，同一把尺子会把
 * brotli 的 10 悄悄降成 9；而唯一共享的那半条规则（"负数 = 本方法自己的默认档"）
 * 在两侧恰好退化成不同的值（deflate 的 `k_compress_level_default` 本身就是 -1）。
 *
 * 服务器侧 `set_compress_level()` 存的值会**原样**流到这里（`apply_compression_for()`
 * 对 method 无知），所以默认档这条路径必须在这里兜住 —— 见 `.cpp` 里的注释。
 */
UVCPP_API int clamp_brotli_quality(int quality);

#if UVCPP_ZLIB_ENABLE || UVCPP_BROTLI_ENABLE

/**
 * @brief Compress input data.
 * @param data   Input buffer.
 * @param len    Input length in bytes.
 * @param method Target encoding (GZIP / DEFLATE / BROTLI).
 * @return Compressed result (check result.success).  Uses
 *         `k_compress_level_default` / `k_brotli_quality_default`.
 */
UVCPP_API http_compress_result compress(const char* data, size_t len,
                                         http_compress_method method);

/**
 * @brief Compress input data at an explicit level.
 * @param data   Input buffer.
 * @param len    Input length in bytes.
 * @param method Target encoding (GZIP / DEFLATE / BROTLI).
 * @param level  Deflate level（`clamp_compress_level()`）**或** brotli 质量
 *               （`clamp_brotli_quality()`）—— 按 `method` 走哪一把尺子。
 *               越低越快越大；**负数一律表示"该 method 自己的默认档"**
 *               （deflate → `k_compress_level_default`，brotli →
 *               `k_brotli_quality_default`），因为调用方未必知道自己在压什么。
 * @return Compressed result (check result.success).
 *
 * 与三参数那份的**唯一**区别就是档位，产出的字节因此**不同**（同一份输入、
 * 同一个档位，则与三参数那份逐字节相同 —— 每调用一次都是全新状态）。
 *
 * `method` 是 `BROTLI` 而这一份构建没开 `UVCPP_BROTLI_ENABLE` 时返回
 * `success == false`：编不出来就是编不出来，`parse_accept_encoding()` 也已经
 * 不会把这个编码交过来（见那里），所以这条只可能是调用方自己指定的。
 */
UVCPP_API http_compress_result compress(const char* data, size_t len,
                                         http_compress_method method,
                                         int level);

/**
 * @brief Decompress input data.
 *        Auto-detects gzip / deflate / brotli from the data header.
 * @param data  Compressed input.
 * @param len   Input length.
 * @return Decompressed result.
 *
 * brotli 那一支只在 `UVCPP_BROTLI_ENABLE=1` 时能认；否则一份 brotli 字节会走
 * 完整条 gzip 回退链后返回失败（而不是返回一份**看起来成功**的错字节）。
 */
UVCPP_API http_compress_result decompress(const char* data, size_t len);

#endif  // UVCPP_ZLIB_ENABLE || UVCPP_BROTLI_ENABLE

/**
 * @brief Parse Accept-Encoding header and return the best supported method.
 *
 * Quality values (q=…) are respected per RFC 7231 Section 5.3.4.
 * Priority when equal: brotli > gzip > deflate（与静态腿选孪生文件的那套
 * `http_pick_precompressed_variants()` 同序，见 `uvcpp_http_common.h`）。
 *
 * 只会返回**这一份构建编得出来**的编码：`UVCPP_BROTLI_ENABLE=0` 时 `br` 当作
 * 未知编码处理（`Accept-Encoding: br` 单独出现时返回 `NONE` ⇒ 发原文，而不是
 * 发一个客户端刚说过它收得到的编码的替代品）。
 *
 * @param header  Value of the Accept-Encoding request header.
 * @return        Best method, or NONE if no supported encoding is acceptable.
 */
UVCPP_API http_compress_method parse_accept_encoding(const std::string& header);

/**
 * @brief Determine whether a response should be compressed.
 * @param content_type   The response Content-Type value (may be empty).
 * @param excluded_types List of MIME types to skip.
 *                       Supports wildcard: "image/*" matches "image/png", etc.
 * @return true if compression is appropriate.
 */
UVCPP_API bool should_compress(const std::string& content_type,
                                const std::vector<std::string>& excluded_types);

/**
 * @brief Default MIME types excluded from HTTP compression.
 *
 * These types are generally already compressed or are binary formats
 * that gain little from gzip.  Callers can use this as a base and
 * customise via set_compress_excluded_types() on the server.
 */
UVCPP_API const std::vector<std::string>& default_excluded_mime_types();

}  // namespace http_compress
}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
#endif  // SRC_WEB_UVCPP_HTTP_COMPRESS_H
