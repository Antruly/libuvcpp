/**
 * @file tests/functional/h2_backpressure_flush_func.cpp
 * @brief 背压恢复的**传输侧**那一半：`resume_stream()` 必须把 WINDOW_UPDATE 冲出网。
 *
 * 这个用例只为**一条**判据存在：`uvcpp_h2_connection::resume_stream()` 里那句
 * `flush()`。会话层的 `resume_stream()` 只把 WINDOW_UPDATE 排进 nghttp2 的出站
 * 队列；而应用侧最自然的调用时机（自己的定时器里、下游腾空之后）**没有任何人
 * 会替你冲一次** —— 漏掉它的表现是"对端永远等不到窗口、那条流挂死"，并且
 * **不报任何错**。把 `conn->resume_stream(1)` 换成 `conn->session().resume_stream(1)`
 * 必须让本用例变红；如果不变红，这条判据就是摆设。
 *
 * 为什么非得上真 socket（`h2_backpressure_func.cpp` 那个面对面内存装置做不到）：
 * 内存装置里"排进队列"和"冲出去"是同一个动作（`drain()` 就是在搬队列），
 * 两者的差别在那套装置里**结构上不存在**。
 *
 * 与 `h2_flush_reject_func.cpp` 同一个立论：**不上 TLS**。判据落在"字节有没有
 * 出网"上，掺进 OpenSSL 只会把"没冲"和"握手没成"混在一起。
 *
 * 对端是**手搓帧**的裸 TCP 连接，不是另一个 `uvcpp_h2_session` —— 因为要观测的
 * 恰好是"我们发出去的那几个帧"，用一个真会话去收会把它们消化掉。
 *
 * 退出码沿用三值约定：0 判过 / 1 判红 / **3 前提不成立＝没判**。
 */

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_NGHTTP2_ENABLE

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <web/uvcpp_http_request.h>

#include <http2/uvcpp_h2_connection.h>

#include "loop_drain.h"
#include "wait_util.h"

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
// 常量
// =========================================================================

const int32_t kStream = 1;

/// 对端一次灌满**一个连接窗口**的 DATA。
const size_t kData = 65535;

/// 恢复之后对端补发的那些（够越过一次 WINDOW_UPDATE 阈值就行）。
const size_t kMore = 32768;

/// 对端**发**的帧也必须守我们宣告的 `SETTINGS_MAX_FRAME_SIZE`（RFC 9113 §4.2
/// 的缺省值 16384）。一帧 65535 是 FRAME_SIZE_ERROR —— 连接当场死，而症状
/// （`on_fatal` 而不是超时）与"背压没生效"完全不同。
const size_t kMaxFrame = 16384;

const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// =========================================================================
// 观测
// =========================================================================

/// 对端（裸 TCP 那一侧）收到的**全部**字节 —— 也就是"我们发出去的东西"。
std::string g_peer_got;
size_t      g_body_total      = 0;
bool        g_paused          = false;
int         g_response_status = 0;
int         g_resp_end        = 0;
int         g_fatal           = 0;
int         g_last_fatal      = 0;
bool        g_responded       = false;

// =========================================================================
// 手搓帧
// =========================================================================

void put_frame(std::string& out, unsigned char type, unsigned char flags,
               int32_t sid, const std::string& payload) {
  const size_t n = payload.size();
  out.push_back(static_cast<char>((n >> 16) & 0xff));
  out.push_back(static_cast<char>((n >> 8) & 0xff));
  out.push_back(static_cast<char>(n & 0xff));
  out.push_back(static_cast<char>(type));
  out.push_back(static_cast<char>(flags));
  out.push_back(static_cast<char>((static_cast<uint32_t>(sid) >> 24) & 0x7f));
  out.push_back(static_cast<char>((static_cast<uint32_t>(sid) >> 16) & 0xff));
  out.push_back(static_cast<char>((static_cast<uint32_t>(sid) >> 8) & 0xff));
  out.push_back(static_cast<char>(static_cast<uint32_t>(sid) & 0xff));
  out.append(payload);
}

/// 空 SETTINGS（9 字节帧头 + 0 字节载荷）。
std::string mk_settings() {
  std::string s;
  put_frame(s, 0x04, 0x00, 0, std::string());
  return s;
}

/// `:status: 200`。**一个字节**：HPACK 的"索引头字段"编码 `0x80 | 8`，静态表
/// 第 8 项正好是 `:status: 200` —— 不用为此写一个 HPACK 编码器。
/// 带 END_HEADERS(0x04)，**刻意不带 END_STREAM**：body 还要跟着来。
std::string mk_headers_200() {
  std::string s;
  put_frame(s, 0x01, 0x04, kStream, std::string(1, static_cast<char>(0x88)));
  return s;
}

/// 把 @p total 字节按 `kMaxFrame` 切成若干 DATA 帧。@p end 时最后一帧带
/// END_STREAM(0x01)。
std::string mk_data(size_t total, bool end) {
  std::string s;
  size_t      left = total;
  while (left > 0) {
    const size_t n   = left > kMaxFrame ? kMaxFrame : left;
    left -= n;
    const bool last = end && left == 0;
    put_frame(s, 0x00, last ? 0x01 : 0x00, kStream, std::string(n, 'x'));
  }
  return s;
}

/// 在 @p buf 里找**指定流**的 WINDOW_UPDATE（type 0x08）并返回它的增量；没有
/// 返回 -1。@p from 是起始偏移（可空），命中时挪到这条帧之后。
///
/// 起点由调用方给：前面 24 字节是客户端前言（RFC 9113 §3.4），拿它当帧解析
/// 只会得到垃圾 —— 而"扫出垃圾"与"真的没有 WU"在只看 `>= 0` 的判据里同形。
long find_wu(const std::string& buf, size_t from, int32_t sid, size_t* next) {
  size_t i = from;
  while (i + 9 <= buf.size()) {
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(buf.data()) + i;
    const size_t len = (static_cast<size_t>(p[0]) << 16) |
                       (static_cast<size_t>(p[1]) << 8) |
                       static_cast<size_t>(p[2]);
    if (i + 9 + len > buf.size()) return -1;
    const int32_t fsid = static_cast<int32_t>(
        (static_cast<uint32_t>(p[5] & 0x7f) << 24) |
        (static_cast<uint32_t>(p[6]) << 16) | (static_cast<uint32_t>(p[7]) << 8) |
        static_cast<uint32_t>(p[8]));
    if (p[3] == 0x08 && fsid == sid && len == 4) {
      const unsigned char* q = p + 9;
      const long inc = (static_cast<long>(q[0] & 0x7f) << 24) |
                       (static_cast<long>(q[1]) << 16) |
                       (static_cast<long>(q[2]) << 8) | static_cast<long>(q[3]);
      if (next) *next = i + 9 + len;
      return inc;
    }
    i += 9 + len;
  }
  return -1;
}

/// 从头扫，数所有 WU 的**条数**（按流号分）。
int count_wu(const std::string& buf, size_t from, int32_t sid) {
  int    n = 0;
  size_t i = from;
  for (;;) {
    size_t next = i;
    if (find_wu(buf, i, sid, &next) < 0) return n;
    ++n;
    i = next;
  }
}

/// @p buf 里（从 @p from 起）有没有 type == @p type 的帧。
bool has_frame_type(const std::string& buf, size_t from, unsigned char type) {
  size_t i = from;
  while (i + 9 <= buf.size()) {
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(buf.data()) + i;
    const size_t len = (static_cast<size_t>(p[0]) << 16) |
                       (static_cast<size_t>(p[1]) << 8) |
                       static_cast<size_t>(p[2]);
    if (i + 9 + len > buf.size()) return false;
    if (p[3] == type) return true;
    i += 9 + len;
  }
  return false;
}

}  // namespace

int main() {
  std::cout << "[functional h2_backpressure_flush] start\n";

  // 服务端用具名类型 `uvcpp_tcp_server`（它把每条连接包成 `uvcpp_tcp_client*`，
  // 于是写字节就是一句 `write(data, len, cb)`）；循环也在它里面，h2 客户端挂到
  // **同一条**循环上，`wait_until` 才能一次泵完两边。
  uvcpp_tcp_server      srv;
  uvcpp_loop*           loop = srv.get_loop();
  uvcpp_test::loop_drain drain_guard(loop);

  if (srv.bindIpv4("127.0.0.1", 0) != 0) {
    std::cerr << "[functional h2_backpressure_flush] bind failed\n";
    return 3;
  }
  sockaddr_in name;
  int         namelen = sizeof(name);
  srv.get_tcp()->getsockname(reinterpret_cast<sockaddr*>(&name), &namelen);
  const int port = ntohs(name.sin_port);

  uvcpp_tcp_client* peer = nullptr;

  // 裸对端的写：把一串字节交给传输层。`write(const char*, size_t, cb)` 是
  // **拷贝**语义（内部 `uvcpp_buf` 收下那一份），所以传临时串是安全的。
  auto peer_send = [&peer](const std::string& bytes, const char* tag) {
    if (peer == nullptr) {
      std::cerr << "  [note] peer 还没建立，" << tag << " 发不出去\n";
      return;
    }
    const int rc = peer->write(bytes.data(), bytes.size(), [tag](int st) {
      if (st != 0) std::cerr << "  [note] peer write(" << tag << ") failed " << st << "\n";
    });
    if (rc != 0) {
      std::cerr << "  [note] peer write(" << tag << ") rejected " << rc << "\n";
    }
  };

  srv.set_read_callback([&](uvcpp_tcp_client& c, const net_read_result& r) {
    if (!r.is_data()) return;
    g_peer_got.append(r.data, r.size);

    // 客户端把自己的请求发出来之后才能回它 —— 服务端在客户端**没开过的**
    // 流上发 HEADERS 是 PROTOCOL_ERROR（那不是响应，是非法推流）。
    // 触发点就取"收到第一个 HEADERS"，不含糊。
    if (!g_responded && has_frame_type(g_peer_got, sizeof(kPreface) - 1, 0x01)) {
      g_responded = true;
      peer = &c;
      std::string batch = mk_settings() + mk_headers_200() + mk_data(kData, false);
      const int   rc    = c.write(batch.data(), batch.size(), [](int st) {
        if (st != 0) std::cerr << "  [note] peer batch write failed " << st << "\n";
      });
      if (rc != 0) {
        std::cerr << "  [note] peer batch rejected " << rc << "\n";
      }
      std::cout << "  [info] 对端回帧 " << batch.size() << " 字节（SETTINGS+HEADERS+"
                << kData << " DATA）\n";
    }
  });

  if (srv.listen([](uvcpp_tcp_client* c) { (void)c; }, 128) != 0) {
    std::cerr << "[functional h2_backpressure_flush] listen failed\n";
    return 3;
  }

  // 声明次序是有意的（析构逆序）：`conn` 必须在 `client` **之前**声明 —— 它在
  // `start()` 里把自己的读回调挂在 `client` 上，客户端还没拆干净就先删 h2 层的话，
  // 那之后的任何一次读都会进已释放的对象。
  std::unique_ptr<uvcpp_h2_connection> conn;
  uvcpp_tcp_client                     client(loop);

  bool connected  = false;
  int  connect_rc = 0;
  int  start_rc   = 0;
  int  submit_rc  = 0;

  const int crc = client.connect("127.0.0.1", port, [&](int status) {
    connect_rc = status;
    connected  = status == 0;
    if (status != 0) return;

    conn.reset(new uvcpp_h2_connection(&client, /*server_side=*/false));

    uvcpp_h2_session::callbacks h2c;
    h2c.on_response = [&](uvcpp_h2_session& s, uvcpp_h2_stream& st, bool) {
      g_response_status = static_cast<int>(st.response.status_code);
      // 暂停点选在这里：请求头刚解出来、body 一个字节都还没交付。暂停之后
      // **最多**还会有一个每流窗口的 `on_body` 到来（已经解出来的收不回去），
      // 所以 J8 期望的正是"恰好一个连接窗口"。
      if (s.pause_stream(kStream) == 0) g_paused = true;
    };
    h2c.on_body = [](uvcpp_h2_session&, uvcpp_h2_stream&, const char*,
                     size_t n) { g_body_total += n; };
    h2c.on_response_end = [](uvcpp_h2_session&, uvcpp_h2_stream&) {
      ++g_resp_end;
    };
    h2c.on_fatal = [](uvcpp_h2_session&, int code) {
      ++g_fatal;
      g_last_fatal = code;
    };

    uvcpp_h2_connection::callbacks cc;
    cc.on_disconnect = [](uvcpp_h2_connection&) {};

    start_rc = conn->start(h2c, cc);
    if (start_rc != 0) return;

    uvcpp_http_request req;
    req.method = http_method::HTTP_GET;
    req.url    = "/bp";
    req.set_header("host", "example.com");
    const int32_t sid = conn->session().submit_request(req, std::string());
    if (sid < 0) {
      submit_rc = sid;
      return;
    }
    const int frv = conn->flush();
    if (frv != 0) submit_rc = frv;
  });

  if (crc != 0) {
    std::cerr << "[functional h2_backpressure_flush] connect() start failed " << crc
              << "\n";
    return 3;
  }

  // -----------------------------------------------------------------
  // 前提 P3：响应头到过、流确实被暂停了、连接还活着
  //
  // **`on_body` 收到的字节数不在这里。** 它恰恰是被测物：把"收到了一整窗"写进
  // 前提，等于拿被测物的产出去判"装置造好了没有" —— 一个**把暂停期间的字节丢掉**
  // 的实现（变异 M2b）会让这里读成"前提不成立"，于是真缺陷被报成**没判**。
  // 交付量归 J8 判，那才是判据该在的地方。
  //
  // 真装置坏了仍然拦得住：对端那批帧要是被 nghttp2 拒了（帧长越界之类），
  // 会走 `on_fatal`；`status != 200` 说明 HEADERS 根本没解出来。
  // -----------------------------------------------------------------
  const bool ok_head = uvcpp_test::wait_until(
      loop, [] { return g_response_status != 0; }, uvcpp_test::kWaitMs);

  std::cout << "  [info] connect_rc=" << connect_rc << " start_rc=" << start_rc
            << " submit_rc=" << submit_rc << " status=" << g_response_status
            << " body=" << g_body_total << " paused=" << (g_paused ? 1 : 0)
            << " fatal=" << g_fatal << "\n";

  if (start_rc != 0 || submit_rc != 0 || g_fatal != 0 || !ok_head ||
      g_response_status != 200 || !g_paused) {
    std::cerr << "[functional h2_backpressure_flush] 前提不成立：装置没造出"
                 "「响应头到过且流已暂停」，这条用例判不了。\n";
    return 3;
  }
  // 客户端前言必须在最前面 —— 下面的 WU 扫描是**从第 25 个字节**起算的，
  // 起点错了就把前言的字节当帧解析。
  if (g_peer_got.size() < sizeof(kPreface) - 1 ||
      std::memcmp(g_peer_got.data(), kPreface, sizeof(kPreface) - 1) != 0) {
    std::cerr << "[functional h2_backpressure_flush] 前提不成立：对端收到的开头"
                 "不是 h2 客户端前言。\n";
    return 3;
  }

  const size_t kFramesFrom = sizeof(kPreface) - 1;

  // J8：暂停之后**最多**还有一个窗口 —— 端到端钉成精确值。
  check(g_body_total == kData,
        "J8 on_body 累计恰好 65535（暂停后不再多收一个字节）");

  // 先空转一段，让连接级的 WINDOW_UPDATE 落地（它在 on_read 的那次 flush 里
  // 出去，时机不在我们手上）；再空转一段，确认**这会儿本来就没人会冲一次**。
  // 第二段是 M4 那条变异能不能被抓住的前提：若还有别的 flush 在途，把
  // `conn->resume_stream()` 换成只排队的 `session().resume_stream()` 也照样绿。
  uvcpp_test::pump_for(loop, 200);
  const size_t peer_mark = g_peer_got.size();
  uvcpp_test::pump_for(loop, 300);
  check(g_peer_got.size() == peer_mark,
        "空转期间线上没有新字节（这会儿没人会替我们冲一次）");

  // J9：连接级无条件归还，流级一点都不还。
  const int wu_conn = count_wu(g_peer_got, kFramesFrom, 0);
  const int wu_str  = count_wu(g_peer_got, kFramesFrom, kStream);
  std::cout << "  [info] 暂停期 WU：流 0 × " << wu_conn << "，流 " << kStream << " × "
            << wu_str << "\n";
  check(wu_conn >= 1,
        "J9a 暂停期间对端收到流 0 的 WINDOW_UPDATE（连接级无条件归还）");
  check(wu_str == 0, "J9b 暂停期间对端一个该流的 WINDOW_UPDATE 都没收到");

  // -----------------------------------------------------------------
  // J10：恢复必须**真的把字节冲出去**
  //
  // 这一句是本文件存在的理由。**不要**把它挪进 `on_response` 之类的回调里 ——
  // 那时 `in_nghttp2()` 为真，`flush()` 本来就会早退，而 `on_read()` 紧接着会
  // 替你冲一次：变异体于是照样绿，判据失去全部牙。这里在主线程、`run()` 之外，
  // 谁都不会替你冲。
  // -----------------------------------------------------------------
  const int rrc = conn->resume_stream(kStream);
  std::cout << "  [info] resume_stream rc=" << rrc << "\n";
  check(rrc == 0, "J10a resume_stream 返回 0");

  const bool ok_wu = uvcpp_test::wait_until(
      loop, [&]() { return count_wu(g_peer_got, kFramesFrom, kStream) >= 1; },
      uvcpp_test::kWaitMs);
  if (!ok_wu) {
    std::cerr << "  [diag] 恢复之后 " << uvcpp_test::kWaitMs
              << " ms 内对端一个该流的 WINDOW_UPDATE 都没收到"
                 "（线上共 " << g_peer_got.size() << " 字节）—— 恢复只排了队、"
                 "没人冲。\n";
  }
  check(ok_wu, "J10b 恢复之后对端收到该流的 WINDOW_UPDATE");

  size_t     wu_from = kFramesFrom;
  const long inc     = find_wu(g_peer_got, wu_from, kStream, &wu_from);
  check(inc == static_cast<long>(kData),
        "J10c 第一个该流的 WINDOW_UPDATE 增量恰好 65535（欠账一次还清）");

  // 对端据此补发剩下的 + END_STREAM ⇒ 流收尾。
  peer_send(mk_data(kMore, true), "more");
  const bool ok_end = uvcpp_test::wait_until(loop, [] { return g_resp_end > 0; },
                                             uvcpp_test::kWaitMs);
  check(ok_end, "J10d 恢复后余下的字节到齐、on_response_end 恰好一次");
  check(g_resp_end == 1, "J10e on_response_end 只跑了一次");
  check(g_fatal == 0, "J10f 全程没有致命错误");

  // -----------------------------------------------------------------
  // 收摊：句柄先关、循环后关
  // -----------------------------------------------------------------
  conn.reset();
  client.close([] {});
  srv.close_all_clients();
  srv.stop();
  uvcpp_test::pump_for(loop, 100);

  std::cout << "[functional h2_backpressure_flush] done failures=" << g_failures
            << " fatal=" << g_fatal << " last_fatal=" << g_last_fatal << "\n";
  return g_failures == 0 ? 0 : 1;
}

#else  // UVCPP_NGHTTP2_ENABLE

int main() {
  std::cout << "[functional h2_backpressure_flush] [skip] nghttp2 disabled\n";
  return 0;
}

#endif  // UVCPP_NGHTTP2_ENABLE
