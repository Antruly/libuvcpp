/**
 * @file tests/functional/h2_backpressure_func.cpp
 * @brief h2 收方向背压：暂停一条流只该停**它自己**，不该毒死整条连接。
 *
 * 与 `h2_session_func.cpp` 同源：不开 socket、不用 TLS、不跑事件循环，两条
 * `uvcpp_h2_session` 面对面靠 `drain()`/`recv()` 手动搬字节。理由是这里的观测量是
 * **"收到了多少字节"与"线上有哪些帧"** —— 一旦接上真 socket，内核缓冲会把
 * "对端没发"和"发了但我们没读"搅在一起（`web_ssl_h2_client_func.cpp` 里
 * `peer_pending_out()` 那个 `write_queue_size` 就分不开这两件事）。
 *
 * 本文件钉两条**互相独立**的性质，各用一个场景：
 *
 *   - 场景 1（每流窗口 4096，**远小于**连接窗口 65535）：暂停只停住这条流。
 *     这一档**只**能钉住"流级额度有没有还"，因为总量（16384）远不到连接窗口。
 *   - 场景 2（每流窗口 131072，**大于**连接窗口 65535）：暂停的流不能连带饿死
 *     同一条连接上别的流。这一档**只**能钉住"连接级额度有没有无条件还" ——
 *     而它的全部价值压在 `131072 > 65535` 这个不等式上，理由见场景 2 的注释。
 *
 * 退出码沿用 `h2_flush_reject_func.cpp` 的三值约定：
 *   0 = 判据跑完且全过；1 = 判据红了；**3 = 前提不成立，这条用例没判**。
 * 3 与 1 必须分开 —— 前提不成立时用例在坏实现上**照样绿**，把它报成"红"会让人
 * 去查一个并不存在的缺陷。
 */

#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_NGHTTP2_ENABLE

#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>

#include <http2/uvcpp_h2_session.h>

using namespace uvcpp;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  [ ok ] " << what << "\n";
  } else {
    std::cerr << "  [FAIL] " << what << "\n";
    ++g_failures;
  }
}

// =========================================================================
// 帧嗅探
// =========================================================================

/// 从 @p buf 的 @p i 处解一帧的 9 字节帧头。截断（或越界）返回 false。
///
/// 每个字节都要 `static_cast<unsigned char>` —— `char` 在 MSVC 上有符号，
/// `0x80` 那一档直接变成负数，"帧长算成负数"和"帧根本没发出来"在诊断里
/// 长得一模一样。
bool frame_at(const std::string& buf, size_t i, size_t* len, unsigned char* type,
              unsigned char* flags, int32_t* sid) {
  if (i + 9 > buf.size()) return false;
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(buf.data()) + i;
  *len = (static_cast<size_t>(p[0]) << 16) | (static_cast<size_t>(p[1]) << 8) |
         static_cast<size_t>(p[2]);
  *type  = p[3];
  *flags = p[4];
  *sid   = static_cast<int32_t>((static_cast<uint32_t>(p[5] & 0x7f) << 24) |
                                (static_cast<uint32_t>(p[6]) << 16) |
                                (static_cast<uint32_t>(p[7]) << 8) |
                                static_cast<uint32_t>(p[8]));
  return i + 9 + *len <= buf.size();
}

/// 在 @p buf 里找**指定流**的 WINDOW_UPDATE，返回它的增量；没有返回 -1。
///
/// @param from 起始偏移，可空。命中时把它挪到这条帧**之后** —— 同一串里同一个流
///             完全可能有**好几条** WU（阈值每触发一次发一条），只找第一条会把
///             "还了 4096 又还了 4096"读成"只还了 4096"。
long find_wu(const std::string& buf, int32_t sid, size_t* from = nullptr) {
  size_t i = from ? *from : 0;
  while (i < buf.size()) {
    size_t        len   = 0;
    unsigned char type  = 0;
    unsigned char flags = 0;
    int32_t       fsid  = 0;
    if (!frame_at(buf, i, &len, &type, &flags, &fsid)) return -1;
    const size_t next = i + 9 + len;
    if (type == 0x08 && fsid == sid && len == 4) {
      const unsigned char* p =
          reinterpret_cast<const unsigned char*>(buf.data()) + i + 9;
      const long inc = (static_cast<long>(p[0] & 0x7f) << 24) |
                       (static_cast<long>(p[1]) << 16) |
                       (static_cast<long>(p[2]) << 8) | static_cast<long>(p[3]);
      if (from) *from = next;
      return inc;
    }
    i = next;
  }
  return -1;
}

/// 在 @p buf 里找一条**非 ACK** 的 SETTINGS，返回其中的 `INITIAL_WINDOW_SIZE`
/// （参数 id `0x04`）；没有这个参数、或压根没有这样的帧，都返回 -1。
///
/// **必须排除 ACK**：SETTINGS ACK 的载荷是空的（RFC 9113 §6.5.3），拿它来判
/// "我们宣告了多大的窗口"会恒为 -1，于是下面那条前提判据**永远不成立** ——
/// 而"前提不成立"与"前提成立但判据红"在汇总里只差一个数字。
long settings_initial_window(const std::string& buf) {
  size_t i = 0;
  while (i < buf.size()) {
    size_t        len   = 0;
    unsigned char type  = 0;
    unsigned char flags = 0;
    int32_t       sid   = 0;
    if (!frame_at(buf, i, &len, &type, &flags, &sid)) return -1;
    if (type == 0x04 && (flags & 0x01) == 0) {
      const size_t end = i + 9 + len;
      for (size_t p = i + 9; p + 6 <= end; p += 6) {
        const unsigned char* q =
            reinterpret_cast<const unsigned char*>(buf.data()) + p;
        const unsigned id = (static_cast<unsigned>(q[0]) << 8) | q[1];
        if (id == 0x04) {
          return static_cast<long>((static_cast<long>(q[2]) << 24) |
                                   (static_cast<long>(q[3]) << 16) |
                                   (static_cast<long>(q[4]) << 8) |
                                   static_cast<long>(q[5]));
        }
      }
    }
    i += 9 + len;
  }
  return -1;
}

// =========================================================================
// 一对面对面的会话
// =========================================================================

struct rig {
  uvcpp_h2_session client{false};
  uvcpp_h2_session server{true};

  int    req_count   = 0;
  int    fatal_count = 0;
  int    last_fatal  = 0;
  int    drain_err   = 0;
  int    recv_err    = 0;
  size_t pumped      = 0;
  /// `pump()` 是不是把轮数跑光了（跑光说明它没收敛，"停住了"与"还在搬"分不开）。
  bool   pump_capped = false;
  int    pump_rounds = 0;

  /// 服务端每条流收到的 body 字节数与收尾次数 —— **按流分开**是必须的：
  /// 把两条流拼进同一个计数器，"A 停住了、B 跑完了"和"两条都跑完一半"同形。
  std::map<int32_t, size_t> body_in;
  std::map<int32_t, int>    body_end;

  /// 线缆上**累积**的字节（不随 `pump()` 清空）。方向是"谁发出的"。
  std::string cli_out;
  std::string svr_out;

  /// 用例挂上去的钩子：服务端收到请求头时调一次（参数是流号）。
  std::function<void(uvcpp_h2_session&, int32_t)> on_request_hook;

  /// 服务端**每流**收方向的初始窗口。0 = 不发这一项 SETTINGS（RFC 缺省 65535）。
  uint32_t server_window = 0;

  bool init(uint32_t window) {
    server_window = window;

    uvcpp_h2_session::callbacks sc;
    sc.on_request = [this](uvcpp_h2_session& s, uvcpp_h2_stream& st, bool) {
      ++req_count;
      if (on_request_hook) on_request_hook(s, st.stream_id);
    };
    sc.on_body = [this](uvcpp_h2_session&, uvcpp_h2_stream& st, const char*,
                        size_t n) { body_in[st.stream_id] += n; };
    sc.on_request_end = [this](uvcpp_h2_session&, uvcpp_h2_stream& st) {
      body_end[st.stream_id] += 1;
    };
    sc.on_fatal = [this](uvcpp_h2_session&, int c) {
      ++fatal_count;
      last_fatal = c;
    };
    // 第四个形参才是每流初始窗口。**不在这里发**就退回 RFC 缺省的 65535 ——
    // 那时场景 1 的 "恰好等于窗口" 判据会退化成 "恰好等于 65535"。
    if (server.init(sc, 64u * 1024u, H2_DEFAULT_MAX_CONCURRENT_STREAMS, window) !=
        0) {
      return false;
    }

    uvcpp_h2_session::callbacks cc;
    cc.on_fatal = [this](uvcpp_h2_session&, int c) {
      ++fatal_count;
      last_fatal = c;
    };
    return client.init(cc) == 0;
  }

  /// 两边的字节来回搬，直到双方都没东西可发。`cli_out`/`svr_out` 是**累积**的。
  void pump(int max_rounds = 4096) {
    pump_rounds = 0;
    for (int i = 0; i < max_rounds; ++i) {
      pump_rounds = i + 1;
      bool        moved = false;
      std::string b;
      const int   cr = client.drain(b);
      if (cr != 0) drain_err = cr;
      if (cr == 0 && !b.empty()) {
        pumped += b.size();
        cli_out.append(b);
        const int rv = server.recv(b.data(), b.size());
        if (rv != 0 && recv_err == 0) recv_err = rv;
        moved = true;
      }
      b.clear();
      const int sr = server.drain(b);
      if (sr != 0) drain_err = sr;
      if (sr == 0 && !b.empty()) {
        pumped += b.size();
        svr_out.append(b);
        const int rv = client.recv(b.data(), b.size());
        if (rv != 0 && recv_err == 0) recv_err = rv;
        moved = true;
      }
      // 致命之后 `recv()` 会一直回同一个错误，继续搬没有意义。
      if (fatal_count != 0) return;
      if (!moved) return;
    }
    pump_capped = true;   // 跑光轮数 = 没收敛
  }

  void dump(const char* tag) {
    std::cout << "    [dump " << tag << "] req=" << req_count
              << " cli_bytes=" << cli_out.size() << " svr_bytes=" << svr_out.size()
              << " pumped=" << pumped << " rounds=" << pump_rounds
              << " capped=" << (pump_capped ? 1 : 0) << " drains=" << drain_err
              << " recv_err=" << recv_err << " fatals=" << fatal_count
              << " last_fatal=" << last_fatal << "\n";
    for (std::map<int32_t, size_t>::const_iterator it = body_in.begin();
         it != body_in.end(); ++it) {
      std::cout << "      stream " << it->first << ": body_in=" << it->second
                << " end=" << body_end[it->first] << "\n";
    }
  }
};

uvcpp_http_request make_req(const std::string& url, size_t body_len) {
  uvcpp_http_request r;
  r.method = body_len == 0 ? http_method::HTTP_GET : http_method::HTTP_POST;
  r.url    = url;
  r.set_header("host", "example.com");
  if (body_len != 0) {
    char n[32];
    std::snprintf(n, sizeof(n), "%lu", static_cast<unsigned long>(body_len));
    r.set_header("content-length", n);
  }
  return r;
}

// =========================================================================
// 场景 1：暂停只停住这条流（每流窗口 4096 << 连接窗口 65535）
// =========================================================================

int scenario_pause_stalls_its_own_stream() {
  std::cout << "[scenario] pause_stalls_its_own_stream\n";
  const uint32_t kWin  = 4096;
  const size_t   kBody = 16384;

  rig R;
  const int f0 = g_failures;   // 只数本场景新增的红
  if (!R.init(kWin)) return 3;

  // ---- 先把 SETTINGS 换完，再开流 ----
  //
  // **这一步不是"多此一举"，缺了它整条用例会死在另一条路上。** 客户端在收到我们
  // 的 SETTINGS 之前，每流发送窗口是 RFC 缺省的 65535；而服务端只宣告了 4096。
  // 两边同时开跑的话，客户端会先按 65535 把 65535 字节推出去，而服务端的每流
  // 本地窗口只有 4096 ⇒ nghttp2 在**服务端**判 FLOW_CONTROL_ERROR、直接终止
  // **整条会话**。那时 body_in 是 4096、"流停住了"看起来成立，但真正发生的是
  // "对端越界被杀"。与 `h2_session_func.cpp` 里那条"少了前言就
  // BAD_CLIENT_MAGIC(-903)"是同一类陷阱。
  R.pump();

  // ---- 前提 P1：我们真的把 4096 宣告出去了 ----
  //
  // 少了它，下面 `body_in[a] == 4096` 完全可以是"对端本来就只发了 4096 就不发了"
  // ——一个恒真的判据。**对端**看到的是**我们**发的 SETTINGS，所以查 `svr_out`。
  const long declared = settings_initial_window(R.svr_out);
  if (declared != static_cast<long>(kWin)) {
    std::cerr << "  [pre] 前提不成立：服务端宣告的 INITIAL_WINDOW_SIZE=" << declared
              << "，期望 " << kWin << "。装置没把每流窗口设成预期值，这条用例判不了。\n";
    R.dump("p1");
    return 3;
  }
  std::cout << "  [info] 服务端宣告 INITIAL_WINDOW_SIZE=" << declared << "\n";

  const size_t mark = R.svr_out.size();

  R.on_request_hook = [](uvcpp_h2_session& s, int32_t sid) {
    s.pause_stream(sid);   // 请求头一到就暂停：**在第一个 body 字节交付之前**
  };

  const int32_t a = R.client.submit_request(make_req("/a", kBody),
                                            std::string(kBody, 'a'));
  if (a <= 0) return 3;
  R.pump();

  std::cout << "  [info] body_in[a]=" << R.body_in[a]
            << " paused=" << (R.server.find_stream(a) != nullptr &&
                              R.server.find_stream(a)->paused ? 1 : 0)
            << "\n";

  // J1：恰好等于我们宣告的那个窗口 —— 多一字节说明流级额度还在还（M2），
  // 少一字节说明连一个窗口都没搬动（装置或实现有问题）。
  check(R.body_in[a] == kWin, "J1 暂停的流恰好收到一个每流窗口（4096）");

  // J2：再泵三次也不长。
  const size_t before = R.body_in[a];
  R.pump();
  R.pump();
  R.pump();
  check(R.body_in[a] == before, "J2 再泵三次仍然不增长");

  // J3：暂停期间线上**没有**这条流的 WINDOW_UPDATE。
  // （流 0 的可以有 —— 连接级是无条件归还的，见场景 2。）
  check(find_wu(R.svr_out, a, nullptr) < 0,
        "J3 暂停期间没有该流的 WINDOW_UPDATE");

  // J4：欠账记对了 —— 计账点必须跟着字节走。
  uvcpp_h2_stream* st = R.server.find_stream(a);
  check(st != nullptr && st->paused && st->paused_owed == kWin,
        "J4 paused=true 且 paused_owed=4096");

  // J5：恢复 —— 一次还清、余下的到齐、收尾恰好一次。
  //
  // 扫描起点是 `mark`（A 建流之前）**不是** 0，这样 J5a 找的确实是这条流的第一条
  // WU。**不为此单开一条 `return 3` 的前提**：暂停期本来就有 WU 是 M2 的症状，
  // 该报红（J3 已经报了），报成"前提不成立"等于把一个真缺陷说成"没判"。
  if (R.server.resume_stream(a) != 0) return 3;
  R.pump();
  size_t     from     = mark;
  const long wu_after = find_wu(R.svr_out, a, &from);
  check(wu_after == static_cast<long>(kWin),
        "J5a 恢复后出现该流的 WINDOW_UPDATE，增量恰好 4096");
  check(R.body_in[a] == kBody, "J5b 余下 12288 字节全部到齐");
  check(R.body_end[a] == 1, "J5c on_request_end 恰好一次");
  check(R.fatal_count == 0 && R.drain_err == 0 && !R.pump_capped,
        "J5d 全程无致命错误、drain 无错、pump 收敛");

  if (g_failures != f0) R.dump("scenario1");
  return g_failures == f0 ? 0 : 1;
}

// =========================================================================
// 场景 1 的对照组：同参数、**不暂停**，16384 必须全收完
// =========================================================================

int scenario_control_no_pause() {
  std::cout << "[scenario] control_no_pause\n";
  const uint32_t kWin  = 4096;
  const size_t   kBody = 16384;

  rig R;
  const int f0 = g_failures;   // 只数本场景新增的红
  if (!R.init(kWin)) return 3;
  R.pump();
  if (settings_initial_window(R.svr_out) != static_cast<long>(kWin)) return 3;

  const int32_t a = R.client.submit_request(make_req("/a", kBody),
                                            std::string(kBody, 'a'));
  if (a <= 0) return 3;
  R.pump();

  // 这条对照存在的理由：它红了就说明**装置本身**搬不动这么多字节（窗口更新
  // 没回来 / pump 不收敛），那么场景 1 里的"停住"毫无意义 —— 分不清是背压
  // 生效还是装置本来就搬不动。[[gate-must-run-against-a-control]]
  check(R.body_in[a] == kBody, "同参数不暂停时 16384 字节全部收完");
  check(R.body_end[a] == 1, "不暂停时 on_request_end 恰好一次");
  check(R.fatal_count == 0 && R.drain_err == 0 && !R.pump_capped,
        "对照组全程无致命错误、pump 收敛");
  if (g_failures != f0) R.dump("control");
  return g_failures == f0 ? 0 : 1;
}

// =========================================================================
// 场景 2：暂停的流不饿死同一条连接上别的流（每流窗口 131072 > 连接窗口 65535）
// =========================================================================

int scenario_paused_stream_does_not_starve_its_connection() {
  std::cout << "[scenario] paused_stream_does_not_starve_its_connection\n";
  const uint32_t kWin  = 131072;
  const size_t   kBody = 393216;

  // ---- 为什么必须是 131072，不能是"够用就行" ----
  //
  // 连接窗口是**全连接共享**的 65535，而且我们不改它。坏实现（连接级额度也被
  // 暂停的流扣住）下，A 会在**连接窗口**用尽处停住 —— 那一点是 65535，而不是
  // 它自己的每流窗口 131072。若这里取 kWin <= 65535，两个数就撞在一起：
  // body_in[a] 在好实现和坏实现下**都是同一个值**，"连接级有没有无条件归还"
  // 完全看不出来。**这条用例的全部价值都压在 `kWin > 65535` 上。**
  static_assert(131072 > 65535, "场景 2 的前提：每流窗口必须大于连接窗口");

  rig R;
  const int f0 = g_failures;   // 只数本场景新增的红
  if (!R.init(kWin)) return 3;
  R.pump();
  if (settings_initial_window(R.svr_out) != static_cast<long>(kWin)) return 3;

  int32_t first_sid = 0;
  R.on_request_hook = [&first_sid](uvcpp_h2_session& s, int32_t sid) {
    if (first_sid == 0) {
      first_sid = sid;
      s.pause_stream(sid);
    }
  };

  const int32_t a = R.client.submit_request(make_req("/a", kBody),
                                            std::string(kBody, 'a'));
  const int32_t b = R.client.submit_request(make_req("/b", kBody),
                                            std::string(kBody, 'b'));
  if (a <= 0 || b <= 0) return 3;
  R.pump();

  // ---- 前提 P2：两条流的请求头都到过，且被暂停的确实是 a ----
  if (R.req_count != 2 || first_sid != a) {
    std::cerr << "  [pre] 前提不成立：req_count=" << R.req_count
              << " first_sid=" << first_sid << " a=" << a << " b=" << b
              << "。两条流没都开起来，或暂停的不是 a，这条用例判不了。\n";
    R.dump("p2");
    return 3;
  }

  const size_t a_while_paused = R.body_in[a];
  std::cout << "  [info] a_while_paused=" << a_while_paused
            << " b=" << R.body_in[b] << " rounds=" << R.pump_rounds
            << " capped=" << (R.pump_capped ? 1 : 0) << "\n";

  // ---- 反空转：这条才是场景 2 的命门 ----
  //
  // 用 `check` 而**不是** `return 3`：它恰好就是坏实现下的症状，不是"装置没造好"。
  check(a_while_paused > 65535,
        "J6a 暂停的流推进到超过一个连接窗口（说明它停在自己的每流窗口上，"
        "不是被连接级误伤）");

  uvcpp_h2_stream* st = R.server.find_stream(a);
  check(st != nullptr && st->paused, "J6b A 此刻仍然暂停着");

  // J6：B 完全不受影响。
  check(R.body_in[b] == kBody, "J6c B 的 393216 字节全部收完");
  check(R.body_end[b] == 1, "J6d B 的 on_request_end 恰好一次");

  // J7：A 恢复后也收满。
  if (R.server.resume_stream(a) != 0) return 3;
  R.pump();
  check(R.body_in[a] == kBody, "J7a A 恢复后也收满 393216");
  check(R.body_end[a] == 1, "J7b A 的 on_request_end 恰好一次");
  check(R.fatal_count == 0 && R.drain_err == 0 && !R.pump_capped,
        "J7c 全程无致命错误、pump 收敛");

  if (g_failures != f0) R.dump("scenario2");
  return g_failures == f0 ? 0 : 1;
}

// =========================================================================
// 暂停对不存在的流 / 幂等
// =========================================================================

int scenario_pause_api_edges() {
  std::cout << "[scenario] pause_api_edges\n";
  rig R;
  const int f0 = g_failures;   // 只数本场景新增的红
  if (!R.init(65535)) return 3;
  R.pump();

  // 不存在的流：`UV_EINVAL`，且**一次性**属性（暂停 / 恢复两半对称）。
  check(R.server.pause_stream(99) == UV_EINVAL, "暂停不存在的流返回 UV_EINVAL");
  check(R.server.resume_stream(99) == UV_EINVAL, "恢复不存在的流返回 UV_EINVAL");

  const int32_t a = R.client.submit_request(make_req("/a", 0), std::string());
  if (a <= 0) return 3;
  R.pump();

  uvcpp_h2_stream* st = R.server.find_stream(a);
  if (st == nullptr) {
    // GET 无 body ⇒ HEADERS 自带 END_STREAM，收完即关，流可能已经没了。
    std::cout << "  [info] 无 body 的流已经关闭，跳过幂等检查\n";
    return g_failures == f0 ? 0 : 1;
  }
  check(R.server.pause_stream(a) == 0 && R.server.pause_stream(a) == 0,
        "重复暂停是幂等的（两次都返回 0）");
  check(R.server.resume_stream(a) == 0, "恢复返回 0");
  check(R.server.resume_stream(a) == 0, "未暂停的流再恢复是空操作（返回 0）");

  if (g_failures != f0) R.dump("edges");
  return g_failures == f0 ? 0 : 1;
}

}  // namespace

int main() {
  struct { const char* name; int (*fn)(); } tests[] = {
      {"pause_stalls_its_own_stream", scenario_pause_stalls_its_own_stream},
      {"control_no_pause", scenario_control_no_pause},
      {"paused_stream_does_not_starve_its_connection",
       scenario_paused_stream_does_not_starve_its_connection},
      {"pause_api_edges", scenario_pause_api_edges},
  };

  // `precondition` 只记账，**不立刻返回** —— 一旦返回，"前面某个场景已经判红"
  // 就会被"某个场景前提不成立"盖掉，红的事实在报告里变成"没判"。真红优先：
  // 只有**一条都没红**时，"没判"才是这句话的结论。
  bool precondition = false;

  for (const auto& t : tests) {
    const int before = g_failures;
    const int rc     = t.fn();
    std::cout << "  -> " << (rc == 0 ? "PASS" : rc == 3 ? "PRECONDITION" : "FAIL")
              << " (" << (g_failures - before) << " 条红)\n";
    if (rc == 3) precondition = true;
  }

  if (g_failures == 0 && precondition) {
    std::cerr << "[h2_backpressure] 前提不成立，本用例**没判**\n";
    return 3;
  }
  std::cout << "[h2_backpressure] " << (g_failures == 0 ? "ALL PASS" : "FAIL")
            << "\n";
  return g_failures == 0 ? 0 : 1;
}

#else  // UVCPP_NGHTTP2_ENABLE

int main() {
  std::cout << "[h2_backpressure] SKIP (nghttp2 disabled)\n";
  return 0;
}

#endif  // UVCPP_NGHTTP2_ENABLE
