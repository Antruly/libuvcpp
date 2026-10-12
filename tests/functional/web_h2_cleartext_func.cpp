/**
 * @file tests/functional/web_h2_cleartext_func.cpp
 * @brief 明文 HTTP/2（RFC 9113 §3.4「先验知识」，下称 h2c）：判据全部落在
 *        **确定的字节**上，并且必须把**分界**钉住。
 *
 * 为什么单开一条：`web_ssl_h2_server_func` 把 h2 服务端测得很细，但那条路上
 * 连接是靠 **TLS + ALPN** 认出来的 —— 判据落在 `SSL_get0_alpn_selected()` 上。
 * 明文这条腿没有任何可借用的信号，只能比字节：头 24 个字节是不是
 * `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`。既然判据是字节，本用例的落点也必须
 * 是字节，而且要覆盖这个判定的**两侧与边界**：
 *
 *   1. 判对了 —— 前言完整时服务端真的按 h2 回帧、路由真的跑到（第一、四、六轮）；
 *   2. 判错了 —— 不是前言时一个字节都不能因此丢掉或重复（第二、五轮）；
 *   3. 前言**跨读边界**时不能只看当前这一段（第三、四轮，这是本层唯一的新状态）；
 *   4. 开关关掉时，同一个字节流必须**逐字节**走今天那条 h1 的路（第七轮）；
 *   5. 第八轮与前言无关，钉的是**这条腿上才有的第二段头** —— 明文 h2 的尾随头
 *      （`grpc-status` 那类，1.6.9）。它挂在这个文件里，是因为尾随头只有在
 *      "一条连接能承载多条流"的协议上才存在，h1 那条腿**没有对应物**：同一份
 *      字节流走到 `send_trailers()` 必须拿到 `UV_ENOTSUP`，而不是被当成普通
 *      响应头混进首部（混进去调用方看着"发出去了"，对端收到的却是另一份报文）。
 *      这一条同样落在第八轮里，用的是同一个 h2c 端口的 h1 那一侧。
 *   6. 第九、十轮判的是**明文这条腿上的 `:scheme`**：RFC 9113 §8.1.1 要求它
 *      与实际 URI 一致，于是明文只能是 `http`、TLS 只能是 `https`，**不符就拒**
 *      （拒掉的那一格拦的是"明文连接自称 https"这种混淆代理入口）。这两轮是
 *      1.6.12 修掉的那个缺陷的回归：服务端曾经把 `https` 钉死，明文 h2c 每一条
 *      流都被 RST_STREAM(PROTOCOL_ERROR)，而前八轮**一个字节都没走到它** ——
 *      那些轮的字节是客户端会话 `drain()` 出来的，而客户端当时也钉死 `https`，
 *      两边一起错、恒等抵消。所以第九轮的头块是**手拼的**（`raw_h2_get()`），
 *      造字节的手与被检验的手不是同一只；第十轮再拿真会话的字节回头打给服务端，
 *      把"两端各自独立地按传输层取值"这一条闭合成环。
 *
 * 装置与"为什么这样能把住关"：
 *
 *   - 服务端跑在**后台线程**、绑端口 0，客户端用**阻塞** `uvcpp_tcp_client`
 *     按需分片写、累积读。分片之间留墙钟间隔，于是"服务端确实分段读过"不再
 *     是推断：`set_raw_data_hook`（解析器**之前**、每段原始字节都经过一次）
 *     把每段长度记下来，第三/四/五轮直接断言**段数 ≥ 2**。没有这条自检的话，
 *     "分片前缀"那几轮在最坏情况下退化成"一次性收全"，于是它们判的东西
 *     （累积状态）一个字节都没被走到，而用例照样绿。
 *   - 第一、四、六轮里喂给服务端的字节**不是手写的常量**，是客户端会话
 *     （`uvcpp_h2_session(false)`）自己 `drain()` 出来的真字节，并且断言它
 *     以那 24 个字节开头 —— 装置自检。手写字节能过，不代表线上真客户端能过。
 *   - 判"服务端说了 h2"用两处互不依赖的证据：第四/六轮是响应体 `world`
 *     出现在 DATA 帧的载荷里（h2 的 DATA 载荷是明文，不是 HPACK），
 *     第一轮则是客户端会话按流收到了 200 + `world` —— 只认字节的话，
 *     一个把 h1 响应恰好拼成帧形状的实现也能骗过第一处。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <uv.h>

#include <uvcpp/uvcpp_define.h>

#if UVCPP_WEB_ENABLE && UVCPP_NGHTTP2_ENABLE

#include <net/uvcpp_tcp_client.h>
#include <req/uvcpp_work.h>
#include <uvcpp/uvcpp_buf.h>
#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_request.h>
#include <web/uvcpp_http_response.h>
#include <web/uvcpp_http_server.h>

#include <http2/uvcpp_h2_session.h>

using namespace uvcpp;

namespace {

// =========================================================================
// 判据
// =========================================================================

/// 头列表里**有没有**这个名字。
///
/// 与 `http_get_header(...) != ""` 的差别就是空值字段：那个对"没发"与"发了个空值"
/// 给同一个答案，而尾随头这一路恰好有 `grpc-message: ""` 这种正常情形。
bool has_named(const http_headers& hs, const std::string& name) {
  for (size_t i = 0; i < hs.size(); ++i) {
    if (hs[i].name == name) return true;
  }
  return false;
}

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  [ ok ] " << what << std::endl;
  } else {
    std::cerr << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void info(const std::string& what) {
  std::cout << "  [info] " << what << std::endl;
}

/// 一段字节的前 16 个字节的十六进制，排错时看"服务端到底回了什么"用。
std::string hex16(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string       out;
  const size_t      n = s.size() < 16 ? s.size() : 16;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    out += kHex[c >> 4];
    out += kHex[c & 0x0F];
    out += ' ';
  }
  if (s.size() > n) out += "...";
  return out;
}

/// 把一段 h2 字节按帧头走一遍，打印 `长度/类型/标志/流号`。
///
/// 判据落在字节上（"服务端说了 h2"）时，光说"收到了 44 字节"没有诊断价值：
/// SETTINGS、ACK、PING、HEADERS、DATA 都是 9 字节帧头起头的，只有把它们摊开
/// 才看得出"服务端停在哪一步"—— 只发了自己的 SETTINGS 就再没动静，
/// 与服务端已经回了 HEADERS/DATA 是两件完全不同的事。
std::string frame_dump(const std::string& s) {
  std::string out;
  size_t      off = 0;
  int         n   = 0;
  while (off + 9 <= s.size() && n < 12) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data()) + off;
    const uint32_t len =
        (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) |
        static_cast<uint32_t>(p[2]);
    const uint32_t type = p[3];
    const uint32_t flg  = p[4];
    if (off + 9 + len > s.size()) {
      out += "截断";
      break;
    }
    if (!out.empty()) out += " ";
    out += "len=" + std::to_string(len) + ",type=" + std::to_string(type) +
           ",flag=" + std::to_string(flg);
    off += 9 + len;
    ++n;
  }
  if (out.empty()) out = "没有完整的帧";
  return out;
}

/// RFC 9113 §3.4 的固定前言：一条 h2 客户端连接的**第一段**。
const char  kPreface[]    = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
const size_t kPrefaceLen  = sizeof(kPreface) - 1;  // 24

/// h2 响应体的明文一定出现在 DATA 帧的载荷里（h2 的 DATA 不编码）。
const char kBody[] = "world";

/// 手拼一条 h2 帧（RFC 9113 §4.1：9 字节定长头 + 载荷）。
///
/// 第 9/10 轮**故意**不用本库的客户端会话：那正是这个缺陷藏了这么久的原因。
/// 缺陷在 `:scheme` 上，而客户端会话过去把它钉死成 `https` —— 同源的用例
/// 两边一起错，恒等抵消（与 h3 那个 CID 长度缺陷同一形状）。判据落在字节上，
/// 造字节的手就不能是那支正在被检验的手。
std::string h2_frame(unsigned type, unsigned flags, uint32_t sid,
                     const std::string& payload) {
  std::string out;
  const uint32_t n = static_cast<uint32_t>(payload.size());
  out += static_cast<char>((n >> 16) & 0xFFu);
  out += static_cast<char>((n >> 8) & 0xFFu);
  out += static_cast<char>(n & 0xFFu);
  out += static_cast<char>(type);
  out += static_cast<char>(flags);
  out += static_cast<char>((sid >> 24) & 0x7Fu);
  out += static_cast<char>((sid >> 16) & 0xFFu);
  out += static_cast<char>((sid >> 8) & 0xFFu);
  out += static_cast<char>(sid & 0xFFu);
  out += payload;
  return out;
}

/// HPACK 里"字面量 + 不做索引 + 名字也是字面量"那一种（RFC 7541 §6.2.2）。
///
/// 首字节 `0x00` = 4 位前缀的值 0（"新名字"），后面两个字符串各自是
/// 「1 位 Huffman 标志(0) + 7 位长度」。名字与值都短于 128 字节，所以长度
/// 一字节装得下，不需要 §5.1 的整数编码。
std::string hpack_literal(const std::string& name, const std::string& value) {
  std::string out;
  out += '\x00';
  out += static_cast<char>(name.size());
  out += name;
  out += static_cast<char>(value.size());
  out += value;
  return out;
}

/// 手工拼一条 `GET <path>` 的完整开场：前言 + 空 SETTINGS + 一条带头块的流。
/// **`scheme` 是唯一的变量** —— 第 9/10 两轮的对照全靠它。
std::string raw_h2_get(const std::string& scheme, const std::string& path) {
  std::string block;
  block += hpack_literal(":method", "GET");
  block += hpack_literal(":scheme", scheme);
  block += hpack_literal(":authority", "example.com");
  block += hpack_literal(":path", path);
  return std::string(kPreface, kPrefaceLen) +
         h2_frame(0x4u, 0x0u, 0u, std::string()) +  // SETTINGS（空：全套缺省）
         h2_frame(0x1u, 0x4u | 0x1u, 1u, block);    // HEADERS + END_HEADERS|END_STREAM
}

/// 收到的字节里有没有**某一类**帧。
///
/// 摊开成帧表而不是 `find()` 一两字节：`\x03`（RST_STREAM 的类型号）在载荷里
/// 随手就会出现（HPACK 或 body 里），拿它当判据会随 body 内容忽真忽假。
bool has_frame_type(const std::string& s, unsigned type) {
  size_t off = 0;
  while (off + 9 <= s.size()) {
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(s.data()) + off;
    const uint32_t len = (static_cast<uint32_t>(p[0]) << 16) |
                         (static_cast<uint32_t>(p[1]) << 8) |
                         static_cast<uint32_t>(p[2]);
    if (off + 9 + len > s.size()) break;  // 截断：后面都不是完整的帧
    if (p[3] == type) return true;
    off += 9 + len;
  }
  return false;
}

uvcpp_http_request make_req(http_method m, const std::string& url,
                            const std::string& host = "example.com") {
  uvcpp_http_request r;
  r.method = m;
  r.url    = url;
  r.set_header("host", host);
  return r;
}

// =========================================================================
// 服务端：后台线程 + 端口 0
// =========================================================================

struct TestServer {
  std::thread         th;
  std::atomic<bool>   stop{false};
  std::promise<int>   port_promise;
  std::mutex          mtx;
  std::vector<size_t> chunks;  // raw_data_hook_ 看到的每段长度

  /**
   * @param h2c true 同时开 h2 与 h2c 两块开关；false 两块都不开（今天的默认）。
   *
   * 两块都要开是刻意的：`h2c_enabled()` 的判据是
   * 「h2 开着 **且** h2c 开着」—— h2c 是 h2 的一条腿，不是独立协议。
   */
  int start(bool h2c) {
    th = std::thread([this, h2c] {
      uvcpp_http_server server;

      // 解析器**之前**的观测点：每段原始字节到这里一次。只看不接管（返回 true）。
      server.set_raw_data_hook(
          [this](uvcpp_tcp_client*, const char*, size_t len) {
            std::lock_guard<std::mutex> g(mtx);
            chunks.push_back(len);
            return true;
          });

      server.set_http2_enabled(h2c);
      server.set_h2c_enabled(h2c);

      auto plain = [](uvcpp_http_request&, uvcpp_http_response& resp,
                      uvcpp_tcp_client*) {
        resp = uvcpp_http_response::ok(kBody, 5, "text/plain");
      };
      server.get("/plain", plain);
      server.post("/plain", plain);
      server.get("/hello", [](uvcpp_http_request&, uvcpp_http_response& resp,
                              uvcpp_tcp_client*) {
        resp = uvcpp_http_response::ok("hello", 5, "text/plain");
      });

      // 推迟应答（1.6.7）：凭据从 `req.reply` 拿，兑现入口是 `send_reply()`。
      // 这一条钉的是凭据里的**流号**真的落到了 `resp.stream_id` 上 —— h2 的
      // 发送按流号归位，少了那一句，响应会被 `send_h2_response(client, 0, resp)`
      // 丢在 `h2_streams.find(0)` 那一步，而且**一处错误都不打**（客户端看到
      // 的只是超时）。`uvcpp_static_server` 那条路以前正是这个症状。
      //
      // 捕获 `&server` 是安全的：处理函数挂在这个对象上，它长不过 server。
      // 尾随头（1.6.9）：流式回一块 body，等它的 `done` 回来之后再发尾随头。
      //
      // **顺序是这条路线的全部**：尾随头是一个自带 END_STREAM 的头块，抢在
      // body 前面出去就是"END_STREAM 在前、字节在后"。所以必须在**最后一块的
      // `done` 里**发 —— 那个回调是那批字节写完之后才跑的（`write_stream` 的
      // 注释写了这条契约），会话层也据此只认"队列空"（见 `submit_trailer`）。
      server.get("/trailers", [&server](uvcpp_http_request& req,
                                        uvcpp_http_response& resp,
                                        uvcpp_tcp_client* client) {
        resp.deferred = true;  // 这条响应由处理函数自己收尾
        uvcpp_http_response head;
        head.status_code = http_status::OK;
        head.set_header("content-type", "application/grpc");
        server.begin_stream(client, req.stream_id, head);
        const int32_t sid = req.stream_id;
        server.write_stream(client, sid, std::string(kBody),
                            [&server, client, sid](int rc) {
                              if (rc != 0) return;
                              http_headers tr;
                              http_header  h;
                              h.name  = "grpc-status";
                              h.value = "0";
                              tr.push_back(h);
                              h.name  = "grpc-message";
                              h.value = "";
                              tr.push_back(h);
                              server.send_trailers(client, sid, tr);
                            });
      });

      // h1 那一侧的尾随头：**必须明着拒掉**。判据只能是返回值 —— 处理函数跑在
      // 服务端线程上，它自己断言失败时客户端看到的是超时，什么都说明不了。所以
      // 这里把 rc 编进 body，让客户端那一侧能读到（见第 8 轮）。
      server.get("/trailers-h1", [&server](uvcpp_http_request& req,
                                           uvcpp_http_response& resp,
                                           uvcpp_tcp_client* client) {
        http_headers tr;
        http_header  h;
        h.name  = "grpc-status";
        h.value = "0";
        tr.push_back(h);
        const int         rc = server.send_trailers(client, req.stream_id, tr);
        const std::string s  = std::to_string(rc);
        resp = uvcpp_http_response::ok(s.c_str(), s.size(), "text/plain");
      });

      server.get("/later", [&server](uvcpp_http_request& req,
                                     uvcpp_http_response& resp,
                                     uvcpp_tcp_client*) {
        const uvcpp_http_reply reply = req.reply;  // 按值留一份
        resp.deferred = true;  // 声明"这条响应由我负责"
        uvcpp_work* w = new uvcpp_work();
        w->queue_work(reply.loop(), [](uvcpp_work*) {},
                      [&server, reply](uvcpp_work* self, int) {
                        delete self;
                        if (!server.reply_alive(reply)) return;
                        uvcpp_http_response out =
                            uvcpp_http_response::ok("later", 5, "text/plain");
                        server.send_reply(reply, out);
                      });
      });

      server.bind("127.0.0.1", 0);
      sockaddr_in name;
      int         namelen = sizeof(name);
      server.get_tcp_server()->get_tcp()->getsockname(
          reinterpret_cast<sockaddr*>(&name), &namelen);
      const int port = ntohs(name.sin_port);

      // **先 listen，再发布端口**：反过来的话调用方立刻 connect 会拿到
      // ECONNREFUSED（本用例的客户端没有重试）。
      server.listen();
      port_promise.set_value(port);

      while (!stop.load()) {
        server.run(UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return port_promise.get_future().get();
  }

  int chunk_count() {
    std::lock_guard<std::mutex> g(mtx);
    return static_cast<int>(chunks.size());
  }

  std::string chunk_sizes() {
    std::lock_guard<std::mutex> g(mtx);
    std::string s;
    for (size_t i = 0; i < chunks.size(); ++i) {
      if (i != 0) s += ",";
      s += std::to_string(chunks[i]);
    }
    return s;
  }

  void reset_chunks() {
    std::lock_guard<std::mutex> g(mtx);
    chunks.clear();
  }

  void shutdown() {
    stop.store(true);
    if (th.joinable()) th.join();
  }
};

// =========================================================================
// 客户端（阻塞）
// =========================================================================

long long ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

/**
 * @brief 依次写出 @p pieces（段与段之间停 @p gap_ms 墙钟），然后**累积读**
 *        直到 @p pred 为真或超过 @p deadline_ms。
 *
 * @return \p pred 最终是否为真。
 *
 * 必须累积读：服务端的 h2 首批字节（我们的 SETTINGS 帧）与响应帧**可能分两次
 * 到达**，而 `read_wait()` 一有数据就返回。只读一次的写法在"响应恰好与前一批
 * 挤在同一段"时绿、否则红 —— 那是把 TCP 分段写进了判据。
 */
bool write_then_read(int port, const std::vector<std::string>& pieces, int gap_ms,
                     std::string& got,
                     const std::function<bool(const std::string&)>& pred,
                     int deadline_ms) {
  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  for (size_t i = 0; i < pieces.size(); ++i) {
    if (!pieces[i].empty() &&
        c.write_wait(pieces[i].data(), pieces[i].size(), 3000) != 0) {
      return false;
    }
    if (gap_ms > 0 && i + 1 < pieces.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
    }
  }

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    if (pred(got)) return true;
    if (ms_since(t0) >= deadline_ms) return false;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        got.append(b.get_const_data(), b.size());
      }
    } else if (rv != UV_ETIMEDOUT) {
      return false;  // 连接没了；后面 pred 也不可能再变真
    }
  }
}

/// 回复是不是一条 **HTTP/1.x** 报文：状态行开头。用来判"这条连接走了 llhttp"。
bool looks_like_h1(const std::string& got) {
  return got.size() >= 9 && got.compare(0, 9, "HTTP/1.1 ") == 0;
}

/**
 * @brief 用客户端会话自己产生一次 h2 客户端连接的**开场字节**：
 *        前言 + SETTINGS + 一条 `GET /plain` 的 HEADERS。
 *
 * 这是装置自检的锚点：第四/五/六轮喂给服务端的必须**就是**这些字节
 * （真客户端会发的那些），不是手写的近似物。
 */
std::string h2_client_hello() {
  uvcpp_h2_session            cli(false);
  uvcpp_h2_session::callbacks cbs;
  // **必须显式声明这是明文连接。** 会话的 `:scheme` 由它定（TLS 上 `https`、
  // 明文上 `http`），缺省是 TLS —— 不声明的话这里 `drain()` 出来的是
  // `:scheme: https`，而服务端在明文这条腿上（正确地）只认 `http`，于是
  // "真客户端的字节"这个锚点本身就把服务端判死了。构造出来的会话默认 TLS，
  // 是因为库内唯一的构造者 `uvcpp_h2_connection` 总会按 `client->is_tls()`
  // 覆盖掉这个值；手工搭会话的用例是唯一需要自己说清楚的场景。
  cli.set_tls(false);
  cbs.on_fatal = [](uvcpp_h2_session&, int) {};
  if (cli.init(cbs) != 0) return std::string();
  if (cli.submit_request(make_req(http_method::HTTP_GET, "/plain"), "") <= 0) {
    return std::string();
  }
  std::string out;
  if (cli.drain(out) != 0) return std::string();
  return out;
}

/**
 * @brief 第一轮：明文 socket 上一次真正的 h2 往返。
 *
 * 客户端会话（不是 `uvcpp_h2_connection`，本层不需要那条连接对象）产生字节，
 * 经真 socket 打到服务端，服务端回来的字节再喂回会话。判据落在**会话**上：
 * 按流收到了 200 与 `world` —— 这一步同时穿过了前言判定、llhttp 的让位、
 * 与会话的重新武装，任何一处没接上都会表现为超时。
 */
bool h2_roundtrip(int port, int& status, std::string& body, std::string& sent,
                  std::string& raw, const std::string& url = "/plain") {
  uvcpp_h2_session            cli(false);
  uvcpp_h2_session::callbacks cbs;
  cli.set_tls(false);  // 明文连接（理由见 h2_client_hello 那一处）
  bool     done  = false;
  bool     fatal = false;
  int      st    = 0;
  std::string bd;

  cbs.on_response = [&](uvcpp_h2_session&, uvcpp_h2_stream& stream, bool) {
    st = static_cast<int>(stream.response.status_code);
  };
  cbs.on_body = [&](uvcpp_h2_session&, uvcpp_h2_stream&, const char* d, size_t n) {
    bd.append(d, n);
  };
  cbs.on_response_end = [&](uvcpp_h2_session&, uvcpp_h2_stream&) { done = true; };
  cbs.on_fatal        = [&](uvcpp_h2_session&, int) { fatal = true; };

  if (cli.init(cbs) != 0) return false;
  if (cli.submit_request(make_req(http_method::HTTP_GET, url), "") <= 0) {
    return false;
  }

  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  bool first_batch = true;

  for (;;) {
    std::string out;
    if (cli.drain(out) != 0) break;
    if (!out.empty()) {
      if (first_batch) {
        sent   = out;
        first_batch = false;
      }
      if (c.write_wait(out.data(), out.size(), 3000) != 0) break;
    }
    if (done || fatal) break;
    if (ms_since(t0) >= 4000) break;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        raw.append(b.get_const_data(), b.size());
        if (cli.recv(b.get_const_data(), b.size()) != 0) break;
      }
    } else if (rv != UV_ETIMEDOUT) {
      break;
    }
  }

  status = st;
  body   = bd;
  return done;
}

// =========================================================================
// 第 1 轮：明文 h2 往返（真 socket、真路由、真会话）
// =========================================================================

void test_h2c_roundtrip() {
  std::cout << "[1] 明文连接上的 h2 往返" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  check(port > 0, "服务端起来了（端口 0 由系统分配）");
  if (port <= 0) return;

  int         status = 0;
  std::string body;
  std::string sent;
  std::string raw;
  const bool  ok = h2_roundtrip(port, status, body, sent, raw);

  // 装置自检：真客户端会话发出来的第一段**必须**以那 24 个字节开头。
  // 不是的话本用例判的不是 h2c —— 它判的是"我们喂了什么"。
  check(sent.size() >= kPrefaceLen &&
            std::memcmp(sent.data(), kPreface, kPrefaceLen) == 0,
        "装置自检：客户端会话发出来的第一段就是 h2 前言");

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  // 服务端回了什么，按帧摊开打出来。判据是"服务端说了 h2"时，光说"收到 44
  // 字节"没有诊断价值 —— 只有摊开才看得出服务端停在哪一步（只发了自己的
  // SETTINGS 就再没动静，与已经回了 HEADERS/DATA 是两件完全不同的事）。
  info("发 " + std::to_string(sent.size()) + " 字节，收 " +
       std::to_string(raw.size()) + " 字节" +
       (raw.empty() ? std::string(" —— 服务端一个字节都没回")
                    : std::string("；帧：" + frame_dump(raw) + "；头 16 字节 " +
                                  hex16(raw))));

  check(ok, "明文连接上收到了完整的 h2 响应");
  check(status == 200, "状态码是 200（实收 " + std::to_string(status) + "）");
  check(body == kBody, "响应体是 world（实收 \"" + body + "\"）");

  srv.shutdown();
}

// =========================================================================
// 第 1 轮之二：明文 h2 上的**推迟应答**（凭据 + send_reply）
// =========================================================================

/**
 * @brief 同一台明文服务端上，走一趟 `resp.deferred` + `req.reply` 的应答。
 *
 * 这一条与第 1 轮打的是同一条腿、同一份前言分流，差别只在处理函数**不在这一次
 * 调用里回话**：它把凭据按值留下、把结果投回循环线程，回来时进度函数早返回了。
 *
 * 判据是"完整的 200 + 那个体"，而**静默丢包正是它的反例**：h2 的发送按
 * `resp.stream_id` 归位，凭据里那个流号要是没搬上去，`send_h2_response()` 会在
 * `h2_streams.find(0)` 上失败并返回 `UV_EINVAL` —— 客户端那边只是一直等到超时，
 * 服务端一处错误都不打。所以这条用例的价值全在"它答出来了"。
 */
void test_h2c_deferred_reply() {
  std::cout << "[1b] 明文 h2 上的推迟应答（req.reply + send_reply）"
            << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  check(port > 0, "服务端起来了（端口 0 由系统分配）");
  if (port <= 0) return;

  int         status = 0;
  std::string body;
  std::string sent;
  std::string raw;
  const bool  ok = h2_roundtrip(port, status, body, sent, raw, "/later");

  info("发 " + std::to_string(sent.size()) + " 字节，收 " +
       std::to_string(raw.size()) + " 字节" +
       (raw.empty() ? std::string(" —— 服务端一个字节都没回")
                    : std::string("；帧：" + frame_dump(raw))));

  check(ok, "推迟那条也收到了完整的 h2 响应（不是静默超时）");
  check(status == 200, "推迟那条的状态码是 200（实收 " +
                           std::to_string(status) + "）");
  check(body == "later", "推迟那条的响应体是 later（实收 \"" + body + "\"）");

  srv.shutdown();
}

// =========================================================================
// 第 2 轮：同一个端口上的 h1 逐字节不变
// =========================================================================

/**
 * @brief 打 `/trailers`：判据落在**客户端会话收到的第二个头块**上。
 *
 * 与 `h2_roundtrip` 的差别只有一处观测点，但那处是关键：尾随头字段收下之后折进
 * 同一个 `response.headers`，而 `on_response` 只在**首部块**上跑（尾随头块的
 * category 是 `HCAT_HEADERS`）—— 所以要在 `on_response_end` 那一刻抓。
 * 只断言"收到 END_STREAM"是不够的：`end_stream()` 那条路带的也是 END_STREAM。
 */
bool h2_trailers_roundtrip(int port, int& status, std::string& body,
                           http_headers& end_headers, std::string& raw) {
  uvcpp_h2_session            cli(false);
  uvcpp_h2_session::callbacks cbs;
  cli.set_tls(false);  // 明文连接（理由见 h2_client_hello 那一处）
  bool         done  = false;
  bool         fatal = false;
  int          st    = 0;
  http_headers first_headers;
  std::string  bd;

  cbs.on_response = [&](uvcpp_h2_session&, uvcpp_h2_stream& stream, bool) {
    st            = static_cast<int>(stream.response.status_code);
    first_headers = stream.response.headers;
  };
  cbs.on_body = [&](uvcpp_h2_session&, uvcpp_h2_stream&, const char* d, size_t n) {
    bd.append(d, n);
  };
  cbs.on_response_end = [&](uvcpp_h2_session&, uvcpp_h2_stream& stream) {
    done        = true;
    end_headers = stream.response.headers;
  };
  cbs.on_fatal = [&](uvcpp_h2_session&, int) { fatal = true; };

  if (cli.init(cbs) != 0) return false;
  if (cli.submit_request(make_req(http_method::HTTP_GET, "/trailers"), "") <= 0) {
    return false;
  }

  uvcpp_tcp_client c;
  if (c.connect_wait("127.0.0.1", port, 3000) != 0) return false;

  const std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  for (;;) {
    std::string out;
    if (cli.drain(out) != 0) break;
    if (!out.empty() && c.write_wait(out.data(), out.size(), 3000) != 0) break;
    if (done || fatal) break;
    if (ms_since(t0) >= 4000) break;

    uvcpp_buf b;
    const int rv = c.read_wait(b, 30);
    if (rv == 0) {
      if (b.size() > 0 && b.get_const_data() != nullptr) {
        raw.append(b.get_const_data(), b.size());
        if (cli.recv(b.get_const_data(), b.size()) != 0) break;
      }
    } else if (rv != UV_ETIMEDOUT) {
      break;
    }
  }

  status = st;
  body   = bd;
  // 首部块里不许提前出现尾随头字段 —— 两个头块必须真是两块。
  for (size_t i = 0; i < first_headers.size(); ++i) {
    if (first_headers[i].name == "grpc-status") return false;
  }
  return done;
}

/**
 * @brief 第 8 轮：`send_trailers()` 真的把尾随头发上线了吗。
 *
 * 判据三样，缺一样都说明不了这条路通：
 *   1. 客户端按流收到 200 与 body `world`（首部与 DATA 都对）；
 *   2. **`on_response_end` 那一刻的头列表里有 `grpc-status: 0`** —— 这是尾随头
 *      真的到了的**唯一**证据（它不是首部、也不是 body）；
 *   3. 首部块（`on_response` 那一刻）里**没有**它。
 * 外加同端口的 h1 那一侧：`send_trailers()` 返 `UV_ENOTSUP`，且报文里看不到
 * `grpc-status`（既没有被塞进首部，也没有别的什么"装作发成功了"的路径）。
 *
 * 反例的形状值得写下来：只断言"客户端收到了 END_STREAM"是**假绿** ——
 * 把处理函数里的 `send_trailers()` 换成 `end_stream(client, sid, false)`
 * 那一条照样绿（两者都是 END_STREAM），而 gRPC 那条路要的正是"END_STREAM
 * 捎带一段字段"。
 */
void test_h2c_trailers() {
  std::cout << "[8] 明文 h2 上的尾随头（send_trailers）" << std::endl;
  TestServer  srv;
  const int   port = srv.start(/*h2c=*/true);
  check(port > 0, "服务端起来了");
  if (port <= 0) return;

  int          status = 0;
  std::string  body, raw;
  http_headers end_headers;
  const bool ok = h2_trailers_roundtrip(port, status, body, end_headers, raw);

  // 同一个端口的 **h1 那一侧**：同样的调用必须拿到 `UV_ENOTSUP`，而且要能从
  // 报文上看出尾随头**没有**被塞进首部。
  const std::string want = std::to_string(UV_ENOTSUP);
  std::string       h1_got;
  const bool        h1_ok = write_then_read(
      port, {"GET /trailers-h1 HTTP/1.1\r\nHost: h\r\n\r\n"}, 0, h1_got,
      [&want](const std::string& s) { return s.find(want) != std::string::npos; },
      3000);
  srv.shutdown();

  check(ok, "整条响应收完（含 END_STREAM）");
  check(status == 200, "状态码 200");
  check(body == kBody, "body 与处理函数发的一致");
  check(http_get_header(end_headers, "grpc-status") == "0",
        "尾随头 grpc-status: 0 到了客户端");
  // 空值的字段也要**在**：`grpc-message` 在成功路径上就是空的，而"这个字段没发"
  // 与"发了个空值"在 `http_get_header() == ""` 这一格上长得一模一样 ——
  // 所以这一条查的是**存在性**，不是值。
  check(has_named(end_headers, "grpc-message"),
        "空值尾随头 grpc-message 也在头列表里（存在性，不是值）");
  check(raw.find("world") != std::string::npos,
        "DATA 载荷里能看到 body 的明文（确认走的是 h2）");
  info("h1 上 send_trailers() 返回 " + want + "（UV_ENOTSUP）");
  check(h1_ok, "h1 上 send_trailers() 返回 UV_ENOTSUP（body 里读到的）");
  check(h1_got.find("grpc-status") == std::string::npos,
        "尾随头没有被当成普通响应头混进 h1 的首部");
}

void test_h1_unchanged_on_same_port() {
  std::cout << "[2] 开了 h2c 的同一个端口上，h1 照旧" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  std::string got;
  const bool  ok = write_then_read(
      port, {"GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"}, 0, got,
      [](const std::string& s) { return s.find("hello") != std::string::npos; },
      3000);

  info("回复 " + std::to_string(got.size()) + " 字节");
  check(ok, "h1 请求拿到了响应");
  check(looks_like_h1(got), "回复是一条 HTTP/1.1 报文（不是 h2 帧）");
  check(got.find("200") != std::string::npos, "状态码是 200");
  check(got.find("hello") != std::string::npos, "响应体是 hello");

  srv.shutdown();
}

// =========================================================================
// 第 3 轮：前言一个字一个字地到 —— 不能只看当前这一段
// =========================================================================

void test_preface_byte_at_a_time() {
  std::cout << "[3] 前言按 1 字节分片到达" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  const std::string hello = h2_client_hello();
  check(hello.size() > kPrefaceLen && hello.compare(0, kPrefaceLen, kPreface) == 0,
        "装置自检：客户端会话给出了真字节（前言 + 帧）");
  if (hello.size() <= kPrefaceLen) {
    srv.shutdown();
    return;
  }

  // 24 个 1 字节的段，段间 10 ms。服务端循环 1 ms 一轮 —— 要让这些段全被
  // 合成一次读，得服务端线程连着 23 次被剥夺 10 ms 以上，实际上不可能。
  std::vector<std::string> pieces;
  for (size_t i = 0; i < kPrefaceLen; ++i) pieces.push_back(hello.substr(i, 1));
  pieces.push_back(hello.substr(kPrefaceLen));

  std::string got;
  const bool  ok = write_then_read(
      port, pieces, 10, got,
      [](const std::string& s) { return s.find(kBody) != std::string::npos; },
      4000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  // **判据的前提**：分片真的发生了。否则这一轮判的与第 6 轮无异。
  check(srv.chunk_count() >= 2, "前提：前言确实跨了多个读段（否则本轮的判据是空的）");
  check(ok, "分片前言仍然被判成 h2 —— 响应体 world 出现在回包里");

  srv.shutdown();
}

// =========================================================================
// 第 4 轮：边界 —— 分片正好落在第 23/24 字节之间
// =========================================================================

void test_preface_split_at_last_byte() {
  std::cout << "[4] 前言在第 23/24 字节之间被切开" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  const std::string hello = h2_client_hello();
  if (hello.size() <= kPrefaceLen) {
    check(false, "装置自检：客户端会话给出了真字节");
    srv.shutdown();
    return;
  }

  // 攒了 23 个字节（**没比完**）之后，最后那 1 个字节单独到。这一格是
  // `have + matched == kH2cPrefaceLen` 的边界：`have > 0` 且 `matched > 0`，
  // 两处「还没比完就 return」的条件都不能成立。
  std::string got;
  const bool  ok = write_then_read(
      port,
      {hello.substr(0, kPrefaceLen - 1), hello.substr(kPrefaceLen - 1)},
      /*gap_ms=*/20, got,
      [](const std::string& s) { return s.find(kBody) != std::string::npos; },
      4000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  check(srv.chunk_count() >= 2, "前提：前言确实跨了多个读段");
  check(ok, "跨在第 23/24 字节之间的前言仍然判成 h2");

  srv.shutdown();
}

// =========================================================================
// 第 5 轮：攒下的前缀不许丢（判岔了要走 h1，且字节一个不少）
// =========================================================================

void test_buffered_prefix_replayed_on_mismatch() {
  std::cout << "[5] 攒下的近似前缀必须原样补喂给 h1 解析器" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  // "P" 与 `PRI...` 的第 1 个字节相同 ⇒ 会被攒进 `h2c_probe` 而**不进解析器**。
  // 第二个段岔开 ⇒ 判定为 h1，这时那个攒下的字节必须**按原样补喂**。
  //
  // 判据为什么是 200：`P` 丢了的话，llhttp 看到的是 `OST /plain HTTP/1.1`，
  // 请求行不是任何已知方法 ⇒ 400（而不是 200）。反过来若把本段也重复喂一次，
  // 请求行就成了 `PPOST ...`，同样不是 200。所以这一格同时钉住"丢了"和"重了"。
  std::string got;
  const bool  ok = write_then_read(
      port,
      {std::string("P"),
       std::string("OST /plain HTTP/1.1\r\nHost: h\r\ncontent-length: 0\r\n\r\n")},
      /*gap_ms=*/20, got,
      [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; },
      3000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  check(srv.chunk_count() >= 2, "前提：两段确实分开读了");
  info("回复首行 " + got.substr(0, got.find("\r\n") == std::string::npos
                                        ? got.size()
                                        : got.find("\r\n")));
  check(looks_like_h1(got), "岔开之后走的是 h1 那条路");
  check(got.compare(0, 12, "HTTP/1.1 200") == 0,
        "状态行是 200 —— 攒下的 \"P\" 被原样补喂，既没丢也没重");

  srv.shutdown();
}

// =========================================================================
// 第 6 轮：岔在第 24 个字节上
// =========================================================================

void test_preface_diverges_at_last_byte() {
  std::cout << "[6] 前 23 个字节对、第 24 个字节岔开" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  // 与第 4 轮同一格边界，但这次第 24 个字节是岔的：`have = 23`、`matched = 0`，
  // 既没比完也不该攒 —— 定案 h1，并把那 23 个字节补喂给 llhttp。
  std::string got;
  const bool  ok = write_then_read(
      port,
      {std::string(kPreface, kPrefaceLen - 1),
       std::string("XGET /hello HTTP/1.1\r\nHost: h\r\n\r\n")},
      /*gap_ms=*/20, got,
      [](const std::string& s) { return s.find("\r\n\r\n") != std::string::npos; },
      3000);

  info("服务端按 " + std::to_string(srv.chunk_count()) + " 段读入，长度 " +
       srv.chunk_sizes());
  check(srv.chunk_count() >= 2, "前提：两段确实分开读了");
  check(looks_like_h1(got), "岔在第 24 个字节上 ⇒ 走 h1 那条路");
  check(got.find(kBody) == std::string::npos,
        "没有按 h2 派发（响应里不该出现路由的 body）");
  check(ok, "服务端给了回复（没有静默丢连接）");

  srv.shutdown();
}

// =========================================================================
// 第 7 轮：对照组 —— 关掉开关时**逐字节**走今天那条路
// =========================================================================

void test_h2c_disabled_control() {
  std::cout << "[7] 对照组：h2c 关掉，同一个字节流仍走 h1" << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/false);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  const std::string hello = h2_client_hello();
  check(hello.size() > kPrefaceLen, "装置自检：客户端会话给出了真字节");

  // 把**完整**的 h2 开场字节一次性打到一个没开 h2c 的服务端上。它必须原样
  // 落到 llhttp 上：请求行 `PRI * HTTP/2.0` 不是任何已知方法 ⇒ 400。
  // 这一轮是前六轮的对照：没有它，"服务端说了 h2"证明不了是那道开关干的。
  std::string got;
  const bool  ok = write_then_read(
      port, {hello}, 0, got,
      [](const std::string& s) { return s.find("\r\n") != std::string::npos; },
      3000);

  info("回复 " + std::to_string(got.size()) + " 字节，首行 " +
       got.substr(0, got.find("\r\n") == std::string::npos ? got.size()
                                                          : got.find("\r\n")));
  check(ok, "关掉开关时服务端仍然回了话");
  check(looks_like_h1(got), "回复是一条 HTTP/1.1 报文 —— 前言被当成畸形请求行");
  check(got.find(kBody) == std::string::npos, "没有按 h2 派发（没有路由的 body）");

  srv.shutdown();
}

// =========================================================================
// 第 9 轮：`:scheme` 必须与**实际传输层**一致 —— 明文侧（字节级的对照组）
// =========================================================================

/**
 * @brief 两条只差 `:scheme` 一个字的连接，必须得到**两种**结果。
 *
 * 这一轮的全部价值在"造字节的手不是被检验的那只手"：包里的 HEADERS 是手拼的
 * （`raw_h2_get()`），不经过 `uvcpp_h2_session`，也不经过 `uvcpp_tcp_client`
 * 之外的任何本库代码。原来的缺陷（服务端把 `https` 钉死）在本文件前八轮里
 * **一个字节都没被走到**，因为那些轮的字节全是客户端会话 `drain()` 出来的，
 * 而它当时也钉死 `https` —— 两边一起错，恒等抵消。
 */
void test_scheme_must_match_cleartext() {
  std::cout << "[9] 明文连接上 `:scheme` 只能是 http（手拼字节，不经过客户端会话）"
            << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  // (a) `http` —— 合规客户端（curl/h2load 的先验知识 h2c）会发的值：必须被派发。
  {
    std::string got;
    const bool  ok = write_then_read(
        port, {raw_h2_get("http", "/plain")}, 0, got,
        [](const std::string& s) { return s.find(kBody) != std::string::npos; },
        3000);

    info("(a) `:scheme: http` 回复 " + std::to_string(got.size()) + " 字节：" +
         frame_dump(got));
    check(ok, "(a) `:scheme: http` 被判成 h2 并派发（响应体 world 出现在 DATA 里）");
    check(!has_frame_type(got, 0x3u), "(a) 没有 RST_STREAM");
  }

  // (b) `https` —— 同一条连接、同一个端口、**只改这一个字**：必须被拒。
  //
  // 为什么"再宽松一点，两个都收"不成立：明文连接自称 `https`，中间那个终结
  // TLS 的代理/缓存就会按 https 的语义去处理它（缓存键、Cookie 的 Secure、
  // HSTS）。RFC 9113 §8.1.1 要拦的正是这个，所以这一格判的不是"服务端挑剔"，
  // 是"服务端没有帮混淆代理开门"。
  {
    std::string got;
    const bool  ok = write_then_read(
        port, {raw_h2_get("https", "/plain")}, 0, got,
        [](const std::string& s) { return has_frame_type(s, 0x3u); }, 3000);

    info("(b) `:scheme: https` 回复 " + std::to_string(got.size()) + " 字节：" +
         frame_dump(got));
    check(ok, "(b) `:scheme: https` 被 RST_STREAM 拒掉");
    check(got.find(kBody) == std::string::npos,
          "(b) 没有派发（响应体一个字节都不该出现）");
    check(srv.chunk_count() >= 1, "前提：服务端确实读到了字节");
  }

  srv.shutdown();
}

// =========================================================================
// 第 10 轮：同一个对照组，但走本库的客户端会话（两处事实必须同源）
// =========================================================================

/**
 * @brief 第 9 轮的镜像：客户端会话**发**出来的字节，按它的 `set_tls()` 取值。
 *
 * 判据为什么不是"在字节里找 `http`/`https` 这几个字符"：`submit_request` 的帧
 * 是 nghttp2 编出来的，头块里伪头的**名字**大概率走静态表索引（`:scheme` 是
 * 索引 6），值也可能被 Huffman 编码 —— 字面量搜索会随编码器策略忽真忽假。
 * 所以这一轮让**服务端**当判据：同样的字节打给明文服务端，第 9 轮已经用
 * 手拼字节钉死了它的口径（`http` 派发、`https` 拒）。于是
 *   - 明文会话的字节被派发 ⇒ 它发的是 `http`；
 *   - TLS 会话的字节被 RST ⇒ 它发的是 `https`。
 * 这条链上没有任何一处是"两端同源抵消"：两端的取值由两个**独立**的事实决定
 * （各自那条连接的 TLS 与否），而判据（哪一侧该被拒）由手拼字节单独钉住。
 */
void test_client_session_scheme_follows_transport() {
  std::cout << "[10] 客户端会话按 set_tls() 填 `:scheme`（服务端当判据）"
            << std::endl;
  TestServer srv;
  const int  port = srv.start(/*h2c=*/true);
  if (port <= 0) {
    check(false, "服务端起来了");
    return;
  }

  auto drained = [](bool tls) {
    uvcpp_h2_session            cli(false);
    uvcpp_h2_session::callbacks cbs;
    cbs.on_fatal = [](uvcpp_h2_session&, int) {};
    cli.set_tls(tls);
    if (cli.init(cbs) != 0) return std::string();
    if (cli.submit_request(make_req(http_method::HTTP_GET, "/plain"), "") <= 0) {
      return std::string();
    }
    std::string out;
    if (cli.drain(out) != 0) return std::string();
    return out;
  };

  const std::string plain   = drained(/*tls=*/false);
  const std::string via_tls = drained(/*tls=*/true);
  check(plain.size() > kPrefaceLen, "装置自检：明文会话给出了字节");
  check(via_tls.size() > kPrefaceLen, "装置自检：TLS 会话给出了字节");

  {
    std::string got;
    const bool  ok = write_then_read(
        port, {plain}, 0, got,
        [](const std::string& s) { return s.find(kBody) != std::string::npos; },
        3000);
    info("明文会话的字节回复 " + std::to_string(got.size()) + " 字节：" +
         frame_dump(got));
    check(ok, "明文会话发的 `:scheme: http` 被服务端接受并派发");
  }

  {
    std::string got;
    const bool  ok = write_then_read(
        port, {via_tls}, 0, got,
        [](const std::string& s) { return has_frame_type(s, 0x3u); }, 3000);
    info("TLS 会话的字节回复 " + std::to_string(got.size()) + " 字节：" +
         frame_dump(got));
    check(ok, "TLS 会话发的 `:scheme: https` 在明文口上被 RST_STREAM 拒掉");
    check(got.find(kBody) == std::string::npos, "TLS 会话那份没有被派发");
  }

  srv.shutdown();
}

}  // namespace

int main() {
  std::cout << "[functional web_h2_cleartext] 明文 HTTP/2（先验知识 h2c）"
            << std::endl;

  test_h2c_roundtrip();
  test_h2c_deferred_reply();
  test_h2c_trailers();
  test_h1_unchanged_on_same_port();
  test_preface_byte_at_a_time();
  test_preface_split_at_last_byte();
  test_buffered_prefix_replayed_on_mismatch();
  test_preface_diverges_at_last_byte();
  test_h2c_disabled_control();
  test_scheme_must_match_cleartext();
  test_client_session_scheme_follows_transport();

  std::cout << "[functional web_h2_cleartext] done failures=" << g_failures
            << std::endl;
  return g_failures == 0 ? 0 : 1;
}

#else

int main() {
  std::cout << "[functional web_h2_cleartext] [skip] web/nghttp2 disabled"
            << std::endl;
  return 0;
}

#endif  // UVCPP_WEB_ENABLE && UVCPP_NGHTTP2_ENABLE
