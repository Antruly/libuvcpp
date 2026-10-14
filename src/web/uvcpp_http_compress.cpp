/**
 * @file src/web/uvcpp_http_compress.cpp
 * @brief HTTP Content-Encoding compression implementation.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_compress.h>

#if UVCPP_WEB_ENABLE

#include <algorithm>
#include <cstring>
#include <sstream>
#if UVCPP_ZLIB_ENABLE
#include <zlib.h>
#endif
#if UVCPP_BROTLI_ENABLE
#include <brotli/encode.h>   /* BrotliEncoderCompressStream 等 */
#include <brotli/decode.h>   /* BrotliDecoderDecompressStream 等 */
#endif


namespace uvcpp {

// =========================================================================
// 同 q 时的偏好序
// =========================================================================

/**
 * 同 q 值时的偏好序：**小的赢**。`br` < `gzip` < `deflate`，与静态腿选孪生
 * 文件那套 `http_pick_precompressed_variants()`（`uvcpp_http_common.h`）逐字同序 ——
 * 同一个 `Accept-Encoding` 在两处必须挑到同一种编码，否则 `/static/x.js` 走孪生
 * 发 br、而同一份字节走现场压缩发 gzip。
 *
 * `NONE` 排在最后只是因为"不上场的不该赢"，实际上它到不了这里：调用点要求
 * `method != NONE`，而 `best` 的初值 q=0 会被任何 q>0 的候选顶掉。
 */
static inline int rank_of(http_compress_method m) {
  switch (m) {
    case http_compress_method::BROTLI:  return 0;
    case http_compress_method::GZIP:    return 1;
    case http_compress_method::DEFLATE: return 2;
    default:                            return 3;
  }
}

// =========================================================================
// parse_accept_encoding
// =========================================================================

http_compress_method http_compress::parse_accept_encoding(
    const std::string& header) {
  if (header.empty()) return http_compress_method::NONE;

  // Parse comma-separated encodings with optional q= weights
  struct candidate { http_compress_method method; double q; };
  candidate best = { http_compress_method::NONE, 0.0 };

  // 三种"这个编码到底能不能发"的依据，缺一不可：
  //   `*_ok`     —— 没被 `q=0` 显式拒过（**不等于**被接受了 —— 没有 `*` 时
  //                 未被列出的编码默认不可接受，RFC 9110 §12.5.3）；
  //   `*_accept` —— 显式列出且 q>0；
  //   `wildcard` —— `*` 出现过（它覆盖的就是"没被显式列出"的那些）。
  // `*_ok` 单独用会把"没被拒"当成"被接受"，于是给一个只列了 gzip 的客户端
  // 发 `content-encoding: deflate` —— 那正是这个函数要修的那一类错误。
  //
  // 每一格都只在**这份构建编得出来**的时候才参与 —— `br` 由 `UVCPP_BROTLI_ENABLE`
  // 说了算，gzip / deflate 由 `UVCPP_ZLIB_ENABLE` 说了算，两个开关各自独立
  // （下面 `zlib_built` 那段的注释讲了后者为什么以前是恒真的）。
  //
  // 关掉的那一格落进"认不出的编码"分支（既不 `accept` 也不 `ok`），于是
  // `Accept-Encoding: br` 单独出现在一份没编 brotli 的构建里，结果就是 `NONE`
  // —— 发原文。**不能**退到 gzip：客户端只列了 br，说明它认的就只有 br
  // （RFC 9110 §12.5.3：没有 `*` 时未被列出的编码默认不可接受），发 gzip 与发
  // 一个它没要的编码是一回事。反过来 `Accept-Encoding: gzip` 出现在一份只编了
  // brotli 的构建里同理 —— 也只有这一条能让「协商说得出、`compress()` 却编不出来」
  // 那对矛盾不出现。
  const bool brotli_built = (UVCPP_BROTLI_ENABLE != 0);
  // gzip / deflate 同一条规矩：**这一份构建编得出来**才进协商结果。在此之前
  // 这一格是恒真的 —— 那时 `UVCPP_ZLIB_ENABLE=0` 意味着整个 http 正文压缩器都
  // 不存在，没人会调到这个函数，于是"只列了 gzip 的客户端被选中 GZIP、而
  // `compress(…, GZIP)` 立刻失败"这条路径不可达。brotli 进来之后它可达了
  // （只编 brotli 的构建里，服务端照样会来协商），所以把 zlib 也收进同一个开关。
  const bool zlib_built   = (UVCPP_ZLIB_ENABLE != 0);
  bool gzip_ok = true, deflate_ok = true, br_ok = true;
  bool gzip_accept = false, deflate_accept = false, br_accept = false;
  bool wildcard = false;

  std::string tok;
  std::istringstream ss(header);
  while (std::getline(ss, tok, ',')) {
    // Trim whitespace
    size_t s = 0, e = tok.size();
    while (s < e && (tok[s] == ' ' || tok[s] == '\t')) s++;
    while (e > s && (tok[e-1] == ' ' || tok[e-1] == '\t')) e--;
    std::string enc = tok.substr(s, e - s);

    // Lower-case for matching（`http_lower_ascii` 是同一张表，见
    // `uvcpp_http_common.h` —— 全模块只剩这一个 ASCII 小写原语）
    for (auto& c : enc) c = static_cast<char>(http_lower_ascii(static_cast<unsigned char>(c)));

    // Parse quality value
    double q = 1.0;
    size_t qpos = enc.find(";q=");
    if (qpos != std::string::npos) {
      try { q = std::stod(enc.substr(qpos + 3)); } catch (...) { q = 0.0; }
      enc = enc.substr(0, qpos);
      // Re-trim
      while (!enc.empty() && enc.back() == ' ') enc.pop_back();
    }

    if (q == 0.0) {
      // "not acceptable" —— 记下来，别只是跳过这一项。
      if (zlib_built && (enc == "gzip" || enc == "x-gzip")) gzip_ok = false;
      else if (zlib_built && (enc == "deflate" || enc == "x-deflate")) deflate_ok = false;
      else if (brotli_built && enc == "br") br_ok = false;
      continue;
    }

    http_compress_method method = http_compress_method::NONE;
    if (zlib_built && (enc == "gzip" || enc == "x-gzip")) {
      gzip_accept = true;
      method      = http_compress_method::GZIP;
    } else if (zlib_built && (enc == "deflate" || enc == "x-deflate")) {
      deflate_accept = true;
      method         = http_compress_method::DEFLATE;
    } else if (brotli_built && enc == "br") {
      br_accept = true;
      method    = http_compress_method::BROTLI;
    } else if (enc == "*") {
      // RFC 9110 §12.5.3：`*` 只匹配**没有显式列出**的编码。所以选择时要把
      // 被 `q=0` 显式拒掉的排掉 —— 否则 `*, gzip;q=0` 会选回 gzip，服务端
      // 于是发了一个客户端刚刚声明过收不了的编码。
      //
      // `*` 的兜底顺序（br → gzip → deflate）与静态腿选孪生文件那套
      // `http_pick_precompressed_variants()` 逐字同序：同一个 `Accept-Encoding`
      // 在两处必须挑到同一种编码，否则 `/static/x.js` 走孪生发 br、而同一份
      // 字节走现场压缩发 gzip，一个客户端会拿到两种 `content-encoding`。
      wildcard = true;
      method   = (brotli_built && br_ok)
                     ? http_compress_method::BROTLI
                     : ((zlib_built && gzip_ok)
                            ? http_compress_method::GZIP
                            : ((zlib_built && deflate_ok)
                                   ? http_compress_method::DEFLATE
                                   : http_compress_method::NONE));
    }

    if (method != http_compress_method::NONE) {
      if (q > best.q || (q == best.q && rank_of(method) < rank_of(best.method))) {
        best.method = method;
        best.q = q;
      }
    }
  }

  // 收尾一道：上面按 q 值取最大值，而 `q=0` 那支是"直接跳过"，所以
  // `gzip;q=0, gzip;q=0.5` 这种自相矛盾的输入仍会把 gzip 选回来。
  // 退回去的那个必须**本身可接受** —— 于是先算"这个编码今天到底能不能发"。
  const bool gzip_usable    = zlib_built && gzip_ok && (gzip_accept || wildcard);
  const bool deflate_usable = zlib_built && deflate_ok && (deflate_accept || wildcard);
  const bool br_usable      = brotli_built && br_ok && (br_accept || wildcard);
  if (best.method == http_compress_method::BROTLI && !br_usable) {
    best.method = gzip_usable ? http_compress_method::GZIP
                              : (deflate_usable ? http_compress_method::DEFLATE
                                                : http_compress_method::NONE);
  } else if (best.method == http_compress_method::GZIP && !gzip_usable) {
    best.method = br_usable ? http_compress_method::BROTLI
                            : (deflate_usable ? http_compress_method::DEFLATE
                                              : http_compress_method::NONE);
  } else if (best.method == http_compress_method::DEFLATE && !deflate_usable) {
    best.method = br_usable ? http_compress_method::BROTLI
                            : (gzip_usable ? http_compress_method::GZIP
                                           : http_compress_method::NONE);
  }

  return best.method;
}

// =========================================================================
// should_compress
// =========================================================================

bool http_compress::should_compress(const std::string& content_type,
                                     const std::vector<std::string>& excluded_types) {
  if (content_type.empty()) return true;  // no content-type → compress

  // Lower-case for matching
  std::string ct = content_type;
  for (auto& c : ct) c = static_cast<char>(http_lower_ascii(static_cast<unsigned char>(c)));

  for (const auto& excl : excluded_types) {
    std::string excl_lower = excl;
    for (auto& c : excl_lower)
      c = static_cast<char>(http_lower_ascii(static_cast<unsigned char>(c)));

    if (!excl_lower.empty() && excl_lower.back() == '/') {
      // Prefix match: "image/" matches "image/png", "image/jpeg", etc.
      if (ct.compare(0, excl_lower.size(), excl_lower) == 0)
        return false;
    } else {
      // Exact match
      if (ct == excl_lower) return false;
    }
  }
  return true;
}

// =========================================================================
// default_excluded_mime_types
// =========================================================================

const std::vector<std::string>& http_compress::default_excluded_mime_types() {
  static const std::vector<std::string> types = {
    "image/",
    "video/",
    "audio/",
    "application/zip",
    "application/gzip",
    "application/x-gzip",
    "application/x-rar-compressed",
    "application/x-7z-compressed",
    "application/x-bzip2",
    "application/x-xz",
    "application/x-compress",
    "application/x-compressed",
    "application/zstd",
    "application/pdf",
    "application/octet-stream",
    // 字体：**只排 woff/woff2，不排整个 `font/`**。这两个容器里装的本来就是
    // 压缩流（WOFF 是 gzip 过的 sfnt，WOFF2 是 brotli 过的），再 deflate 一遍
    // 不但白烧 CPU，产物还**比原文件大** —— 本仓语料上 22 528 → 22 556、
    // 18 432 → 18 460 字节。而 `font/ttf` / `font/otf` / `font/collection` 是
    // 未压缩的轮廓数据，deflate 有实打实的收益，所以前缀匹配在这里是**错的**。
    // 这与 Caddy `encode` 的跳过表一致（它也是逐个列 `font/woff` / `font/woff2`，
    // 不是整个 `font/`）。
    "font/woff",
    "font/woff2",
  };
  return types;
}

// =========================================================================
// 档位夹紧
//
// 两个函数都在 `UVCPP_ZLIB_ENABLE` / `UVCPP_BROTLI_ENABLE` 之外：它们是纯整数
// 夹取，不碰任何第三方类型，而声明在头文件里是无条件的 —— 定义跟着无条件，
// 符号表才不会随"这份构建开了哪个编码器"而变（`check_capi_symbols` 量的就是
// 这件事）。
// =========================================================================

int http_compress::clamp_compress_level(int level) {
  if (level < k_compress_level_min) return k_compress_level_min;
  if (level > k_compress_level_max) return k_compress_level_max;
  return level;
}

int http_compress::clamp_brotli_quality(int quality) {
  // 负数不是"很小的质量"，是**哨兵**：`k_compress_level_default`(-1) 的同一个
  // 意思 —— "用这个编码器自己的默认档"。服务器侧的 `set_compress_level()` 存的
  // 就是这枚哨兵（默认值），而 `apply_compression_for()` 把它**原样**喂给本函数
  // （它对 method 是无知的）。少了这一行，`json-comp` 那条路上 brotli 会以
  // quality 0 编码：本机 32 820 B 的语料 q0 = 1 784 B，而 q5 = 620 B —— 比
  // gzip 的 1 118 B 还大 60%，即"开了 br 反而扣分"。
  if (quality < 0) return k_brotli_quality_default;
  if (quality < k_brotli_quality_min) return k_brotli_quality_min;
  if (quality > k_brotli_quality_max) return k_brotli_quality_max;
  return quality;
}

// =========================================================================
// deflate（zlib）
// =========================================================================

#if UVCPP_ZLIB_ENABLE

namespace {

// 每个线程留一份 deflate 内核，用完 `deflateReset()` 复用，而不是每请求
// `deflateInit2()` / `deflateEnd()` 一遍。
//
// `deflateInit2()` 要为内核要一大块内存 —— level 6、windowBits 15 时约 256 KB
// （两个 32 KB 窗口，加上 32 K 条目的 hash 表与 prev 链）。而这条路在**每个
// 被压缩的响应**上都要走一遍，那块内存在请求之间不携带任何信息：`deflateReset()`
// 之后产出的字节与刚 init 完的一份**逐字节相同**（它就是这个语义），参数一个
// 都没变。于是每请求一次 256 KB 的分配/释放，换不来任何东西。
//
// 单线程下那一次分配只是几十微秒，但在**每条循环线程各来一次**的服务器上不是：
// 256 KB 在默认的 mmap 阈值（128 KB）之上，glibc 走的是 `mmap`/`munmap`，而那条
// 路要拿内核的地址空间锁、并且每释放一次就让其他核上的 TLB 失效一次。实测
// `json-comp` 那条腿上，压缩把服务端吞吐从约 42 000 rps 压到约 15 800 rps ——
// 差值远大于 deflate 本身的 55 µs（见下），那一大块就是这里的内核争用。
//
// 线程局部而不是全局：压缩跑在各条循环线程上，一份共享内核就得加锁，把省下来
// 的有还回去。代价是调用过压缩的线程各常驻一份内核 —— 它只在真被用到时才分配。
//
// `deflateReset()` 换不了任何**创建内核时定下的**参数。这一类参数今天有两个：
// 窗口（gzip 是 31、zlib 是 15）与档位。所以槽里要记着上一回是哪一组，两者
// 有一个对不上就老老实实 end 掉再 init 一次 —— 只比窗口的话，从 6 档换到 1 档
// 会**静默地**继续用 6 档的内核（`deflateReset()` 不会把它调低），产出的字节
// 与记录的档位不符，而变体缓存是**按档位分键**的：于是 1 档那个键下面存着的
// 是 6 档的字节，一路错到底且每一层看着都对。
//
// **这一套在 brotli 上做不到**，不是没试：brotli 根本没有 `BrotliEncoderReset()`
// 这个函数（`brotli/encode.h` 里只有 `BrotliEncoderCreateInstance` /
// `BrotliEncoderCompress` / `BrotliEncoderCompressStream` 那一族，没有任何 reset
// 入口；`decode.h` 同理）。所以 brotli 那一支每次都得新建状态 —— 头文件里
// 那张 q5 = 62.4 µs 的表**已经含**这份状态churn，"能不能靠复用内核把 brotli
// 拉回来"这条路在 API 上就是封死的。
struct deflate_slot {
  z_stream strm;
  bool     inited;
  int      window_bits;
  int      level;

  deflate_slot() : inited(false), window_bits(0), level(0) {
    std::memset(&strm, 0, sizeof(strm));
  }
  ~deflate_slot() {
    if (inited) deflateEnd(&strm);
  }
};

deflate_slot& deflate_slot_for_this_thread() {
  static thread_local deflate_slot slot;
  return slot;
}

}  // namespace

#endif  // UVCPP_ZLIB_ENABLE

// =========================================================================
// brotli
// =========================================================================

#if UVCPP_BROTLI_ENABLE

namespace {

/**
 * 输入长度对应的 `lgwin`。
 *
 * 窗口大小属于"建状态时定下、后面改不了"的那一类参数（就是下面 deflate
 * 那段说的那种，只是 brotli 连 reset 都没有），所以每次按体长现算。
 * 取 `ceil(log2(len))`，夹在 brotli 自己的合法区间里
 * （`BROTLI_MIN_WINDOW_BITS`=10 .. `BROTLI_MAX_WINDOW_BITS`=24，`encode.h:23-34`）。
 *
 * 窗口只要**装得下整个输入**，往后就没有任何长途匹配可以多找 —— 所以按体长取
 * 够用的最小窗口，而不是像一次性 API 那样一律开满默认的 22。本机在
 * 4 190 / 6 707 / 8 397 B 三份语料上比过常用档位：`lgwin` 取 `ceil(log2(len))`
 * 与取 22 的字节数差 **0~3 B，方向两边都有**（8 397 B 那份 q5 是 1 389 vs 1 391），
 * 而窗口每小一档，状态里那几张表就小一截。
 */
int brotli_lgwin_for(size_t len) {
  int lg = BROTLI_MIN_WINDOW_BITS;
  while (lg < BROTLI_MAX_WINDOW_BITS &&
         (static_cast<size_t>(1) << lg) < len) {
    ++lg;
  }
  return lg;
}

}  // namespace

#endif  // UVCPP_BROTLI_ENABLE

// =========================================================================
// compress / decompress
//
// 两份实现都在这一节里，因为两个重载的**分支结构**是共用的：`method` 决定走
// zlib 还是 brotli，缺哪一份就把那一支判成失败（而不是悄悄换成另一种编码）。
// =========================================================================

#if UVCPP_ZLIB_ENABLE || UVCPP_BROTLI_ENABLE

http_compress_result http_compress::compress(const char* data, size_t len,
                                               http_compress_method method) {
  // 两个档位的默认值各走各的尺子：deflate 的默认是 `-1`（= zlib 的
  // `Z_DEFAULT_COMPRESSION`），brotli 的是 5。传错尺子的下限会直接被
  // `BrotliEncoderCompress` 判失败（它不收 -1），所以这里不能混。
  return compress(data, len, method,
                  method == http_compress_method::BROTLI
                      ? k_brotli_quality_default
                      : k_compress_level_default);
}

http_compress_result http_compress::compress(const char* data, size_t len,
                                               http_compress_method method,
                                               int level) {
  http_compress_result result;
  result.method = method;
  if (!data || len == 0) { result.success = true; return result; }

  if (method == http_compress_method::BROTLI) {
#if UVCPP_BROTLI_ENABLE
    // `level` 可能是 deflate 的哨兵 -1（`set_compress_level()` 的默认值，
    // 见 uvcpp_http_server.cpp 的 `apply_compression_for()`）—— clamp 把负数
    // 收成 `k_brotli_quality_default`，**不是** 0
    const int q     = clamp_brotli_quality(level);
    const int lgwin = brotli_lgwin_for(len);

    BrotliEncoderState* st = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (st == NULL) {
      result.success = false;
      return result;
    }
    // 三个参数值都是**创建后、开工前**设：`SIZE_HINT` 让编码器一开始就知道
    // 一共多少字节（它据此挑内部参数，不设只是白白保守一点）；`MODE` 用
    // GENERIC —— 本机在 json 语料上量过 TEXT 与 GENERIC 的差别落在噪声里，
    // 而 TEXT 对真的二进制（图片/字体）是错的。
    BrotliEncoderSetParameter(st, BROTLI_PARAM_QUALITY, (uint32_t)q);
    BrotliEncoderSetParameter(st, BROTLI_PARAM_LGWIN, (uint32_t)lgwin);
    BrotliEncoderSetParameter(st, BROTLI_PARAM_SIZE_HINT,
                             (uint32_t)((len > 0xFFFFFFFFu) ? 0xFFFFFFFFu : len));

    size_t avail_in = len;
    const uint8_t* next_in = reinterpret_cast<const uint8_t*>(data);
    // 输出块 4 KiB、走栈 —— 与 deflate 那一支同形：这条路不为输出另要一块
    // 与响应体同量级的内存，哪怕输入是一整个大文件。
    const size_t C = 4096;
    unsigned char tmp[C];
    size_t avail_out = C;
    uint8_t* next_out = tmp;
    bool ok = true;

    // 循环形状取自 `encode.h:395-410` 的说明：FINISH 下要一直喂到
    // `BrotliEncoderIsFinished()` 为真，中途 `avail_out` 见底就把这一块收走、
    // 换一块新的接着转。
    //
    // **没有"收尾块"**：判定在调用**之前**，所以结束流的那一次调用的输出已经
    // 在循环体里被收走了；再多收一次就是把它收两遍（本机实测：多收一次产出的
    // 恰好是 2 倍长度）。反过来说，漏收也不会短 —— `IsFinished()` 只有在最后
    // 一个字节都交出去之后才为真。
    //
    // 这一支与 `BrotliEncoderCompress()` 的产出**逐字节相同**，本机在
    // q4/5/6/9/11 × 三份语料（4 190 / 6 707 / 8 397 B）共 15 组上比过。
    // 那才是这条路的判据：字节不差，才谈得上"和一次性 API 一样快"。
    while (!BrotliEncoderIsFinished(st)) {
      if (!BrotliEncoderCompressStream(st, BROTLI_OPERATION_FINISH,
                                       &avail_in, &next_in,
                                       &avail_out, &next_out, NULL)) {
        ok = false;
        break;
      }
      const size_t written = C - avail_out;
      if (written > 0)
        result.data.append_data(reinterpret_cast<const char*>(tmp), written);
      if (avail_out == 0) { avail_out = C; next_out = tmp; }
    }
    BrotliEncoderDestroyInstance(st);
    result.success = ok;
    return result;
#else
    // 这份构建没编 brotli。**不退化**成 gzip：`method` 是调用方指定的编码，
    // 换成另一种就是在响应头上写 `content-encoding: gzip` 而调用方以为发的是
    // br（`parse_accept_encoding()` 也根本不会把 BROTLI 交过来）。
    result.success = false;
    return result;
#endif
  }

#if UVCPP_ZLIB_ENABLE
  int window_bits;
  switch (method) {
    case http_compress_method::GZIP:    window_bits = 15 + 16; break;  // gzip wrap
    case http_compress_method::DEFLATE: window_bits = 15;       break;  // zlib wrap
    default: result.success = false; return result;
  }

  // 先夹紧再进内核：`deflateInit2` 对越界档位返回 `Z_STREAM_ERROR`，而调用方
  // 没有理由为"档位写大了"去处理一个失败 —— 见头文件里那条。
  const int lv = clamp_compress_level(level);

  deflate_slot& slot = deflate_slot_for_this_thread();
  int rv;
  if (slot.inited && slot.window_bits == window_bits && slot.level == lv) {
    rv = deflateReset(&slot.strm);
  } else {
    if (slot.inited) {
      deflateEnd(&slot.strm);
      slot.inited = false;
    }
    std::memset(&slot.strm, 0, sizeof(slot.strm));
    rv = deflateInit2(&slot.strm, lv, Z_DEFLATED,
                      window_bits, 8, Z_DEFAULT_STRATEGY);
    if (rv == Z_OK) {
      slot.inited = true;
      slot.window_bits = window_bits;
      slot.level = lv;
    }
  }
  if (rv != Z_OK) {
    result.success = false;
    return result;
  }

  z_stream& strm = slot.strm;
  strm.next_in   = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
  strm.avail_in  = static_cast<uInt>(len);

  int ret;
  const size_t C = 4096;
  do {
    unsigned char tmp[C];
    strm.next_out  = tmp;
    strm.avail_out = C;
    ret = deflate(&strm, Z_FINISH);
    size_t written = C - strm.avail_out;
    if (written > 0)
      result.data.append_data(reinterpret_cast<const char*>(tmp), written);
  } while (ret == Z_OK);

  result.success = (ret == Z_STREAM_END);
  // 不 `deflateEnd()` —— 内核留给这个线程的下一次调用（见上面 `deflate_slot`）。
  return result;
#else
  // 没编 zlib，而 `method` 也不是 BROTLI（上面那支已经返回了）—— 只剩
  // GZIP/DEFLATE，两种都编不出来。
  result.success = false;
  return result;
#endif
}

http_compress_result http_compress::decompress(const char* data, size_t len) {
  http_compress_result result;
  if (!data || len == 0) { result.success = true; return result; }

#if UVCPP_ZLIB_ENABLE
  // Auto-detect: try with window_bits=47 (32+15) which handles gzip, zlib, and raw
  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));

  bool have_zlib = true;
  if (inflateInit2(&strm, 15 + 32) != Z_OK) {
    // Fallback: try zlib-wrap only
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 15) != Z_OK) {
      have_zlib = false;
    }
  }

  if (have_zlib) {
    strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    strm.avail_in = static_cast<uInt>(len);

    int ret;
    const size_t C = 4096;
    // `Z_NO_FLUSH` 不是随手写的，**Z_FINISH 在这里是个缺陷**：输出缓冲被填满时，
    // `inflate()` 对 `Z_FINISH` 的约定是返回 `Z_BUF_ERROR`(-5)，而不是 `Z_OK` ——
    // 于是 `while (ret == Z_OK)` 在**第一块**就退出、`ret != Z_STREAM_END`，整个
    // 解压被判成失败，再掉进下面那条 raw deflate 回退（同样失败）。症状是
    // **解压后超过 4 KiB 的东西一律解不开**，4 KiB 以内的全好（一次就
    // `Z_STREAM_END`）—— 所以小体的往返用例一直是绿的，这也是它一直没被发现的原因。
    // `Z_NO_FLUSH` 才是"输入还没完、我还要接着输出"的那个参数：填满返回 `Z_OK`。
    //
    // 退出条件也从"只看返回值"补成"返回值 + 缓冲真的被填满了"：`Z_NO_FLUSH` 下
    // `Z_OK` 而 `avail_out != 0` 只剩一个含义 —— 输入用完了，没有下一块了。
    // 少了后半句，一个恰好整块结束的流会在最后那次调用上白转（更坏的情况是
    // "返回 Z_OK 但毫无进展"时原地死循环）。
    do {
      unsigned char tmp[C];
      strm.next_out  = tmp;
      strm.avail_out = C;
      ret = inflate(&strm, Z_NO_FLUSH);
      size_t written = C - strm.avail_out;
      if (written > 0)
        result.data.append_data(reinterpret_cast<const char*>(tmp), written);
    } while (ret == Z_OK && strm.avail_out == 0);

    inflateEnd(&strm);
    if (ret == Z_STREAM_END) {
      result.success = true;
      result.method = http_compress_method::GZIP;
      return result;
    }
    // 不是 gzip/zlib：清掉半路的输出，让下面两支从零开始判（留着的话，
    // brotli 一支成功时会带回一段 inflate 吐出来的垃圾前缀）。
    result.data.clear();
  }
#else
  (void)data; (void)len;
#endif  // UVCPP_ZLIB_ENABLE

#if UVCPP_BROTLI_ENABLE
  // brotli 排在**裸 deflate 之前**。两个理由：
  //   1. 裸 deflate（`inflateInit2(-15)`）对**任意**字节都可能"成功" —— 它没有
  //      校验和，也没有头部，任何一段能凑成合法 Huffman 块的东西都能解出点什么。
  //      把 brotli 放在它后面，一份真的 br 字节有机会先被 inflate 当成裸 deflate
  //      解成垃圾并**返回成功**，那比失败更坏（调用方拿到的是错字节）。
  //   2. 裸 deflate 在今天已经不是任何服务端会发的东西（本仓那条回退是给"某台
  //      老服务器"留的），而 brotli 是 RFC 7932，`Accept-Encoding: br` 满世界都在发。
  // brotli 没有魔数，所以只能"试"：解不出就是解不出，落回下面那条裸 deflate。
  {
    BrotliDecoderState* st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (st != NULL) {
      size_t avail_in = len;
      const uint8_t* next_in = reinterpret_cast<const uint8_t*>(data);
      const size_t C = 4096;
      BrotliDecoderResult r = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
      while (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
             r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
        unsigned char tmp[C];
        size_t avail_out = C;
        uint8_t* next_out = tmp;
        r = BrotliDecoderDecompressStream(st, &avail_in, &next_in,
                                          &avail_out, &next_out, NULL);
        const size_t written = C - avail_out;
        if (written > 0)
          result.data.append_data(reinterpret_cast<const char*>(tmp), written);
        // 输入耗尽又没要更多输出 ⇒ 流没结束，别再原地转（`NEEDS_MORE_INPUT`
        // 会一直返回同一个值，这里没有新输入可喂）。
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in == 0) break;
      }
      BrotliDecoderDestroyInstance(st);
      if (r == BROTLI_DECODER_RESULT_SUCCESS) {
        result.success = true;
        result.method = http_compress_method::BROTLI;
        return result;
      }
      result.data.clear();
    }
  }
#endif  // UVCPP_BROTLI_ENABLE

#if UVCPP_ZLIB_ENABLE
  // Try raw deflate as fallback (some servers send bare deflate)
  {
    z_stream raw;
    std::memset(&raw, 0, sizeof(raw));
    if (inflateInit2(&raw, -15) != Z_OK) {
      result.success = false;
      return result;
    }
    raw.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    raw.avail_in = static_cast<uInt>(len);
    int ret;
    const size_t C = 4096;
    // 同上面那条：`Z_NO_FLUSH` + "缓冲真的填满了"，理由见那段说明。
    do {
      unsigned char tmp[C];
      raw.next_out  = tmp;
      raw.avail_out = C;
      ret = inflate(&raw, Z_NO_FLUSH);
      size_t written = C - raw.avail_out;
      if (written > 0)
        result.data.append_data(reinterpret_cast<const char*>(tmp), written);
    } while (ret == Z_OK && raw.avail_out == 0);
    result.success = (ret == Z_STREAM_END);
    if (result.success)
      result.method = http_compress_method::DEFLATE;
    else
      result.data.clear();
    inflateEnd(&raw);
  }
  return result;
#else
  result.success = false;
  result.data.clear();
  return result;
#endif  // UVCPP_ZLIB_ENABLE
}

#endif  // UVCPP_ZLIB_ENABLE || UVCPP_BROTLI_ENABLE


}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
