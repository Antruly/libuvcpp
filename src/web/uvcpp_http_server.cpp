/**
 * @file src/web/uvcpp_http_server.cpp
 * @brief Implementation of uvcpp_http_server — HTTP/1.1 server.
 * @author zhuweiye
 * @version 1.0.0
 */

#include <web/uvcpp_http_server.h>

#if UVCPP_WEB_ENABLE

#include <web/uvcpp_http_common.h>
#include <web/uvcpp_http_parser.h>
#include <web/uvcpp_http_compress.h>
#include <web/uvcpp_http_date.h>
// `uvcpp_loop_index_of_this_thread()`：按循环切容器要问"我在哪条循环上"。
#include <net/uvcpp_loop_worker.h>
// `start_write` 要在连接上留一个复用形态的写请求（`new uvcpp_write()`），
// 那需要完整类型；本层的公开头只把它当指针用，所以没在自己头里带进来。
#include <req/uvcpp_write.h>
#include <cstdio>
#include <cstdlib>  // `std::abort()`：`ctxs_at()` 对多循环下的 -1 直接终止
#include <cstring>
#include <string>

#if UVCPP_NGHTTP2_ENABLE
#include "http2/uvcpp_h2_connection.h"
#include "http2/uvcpp_h2_session.h"
#endif

#if UVCPP_HTTP3_ENABLE
// 与 h2 那两行同一个道理：只有**实现**这一侧引它们的头（公开头里全是前向
// 声明）。`uvcpp_quic_server.h` 自己带上 `uvcpp_quic_connection.h`。
#include "http3/uvcpp_h3_connection.h"
#include "quic/uvcpp_quic_server.h"
#endif

namespace uvcpp {

namespace {

#if UVCPP_NGHTTP2_ENABLE
/**
 * @brief RFC 9113 §3.4 的连接前言，**逐字节**就是这一段。
 *
 * 明文连接的 h2c 判定（`uvcpp_http_server::set_h2c_enabled`）拿它当判据。写成
 * 常量而不是散在代码里比较字符串，是因为它同时是三件事的**唯一**来源：比对的
 * 长度、比对的字节、以及"24"这个数。
 *
 * 末尾那个空字节不参与比较（`kH2cPrefaceLen` 是 `sizeof - 1`），但留着它让这个
 * 常量是一个 C 字符串，接上 `+` 运算之类也能直接用。
 */
const char kH2cPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
const size_t kH2cPrefaceLen = sizeof(kH2cPreface) - 1;
#endif

/**
 * @brief 把一条请求对象**清成空**，但**不还它那几块缓冲**（`clear()` 只改 size）。
 *
 * 为什么不能写 `r = uvcpp_http_request();`：`conn_ctx::request` 与
 * `conn_ctx::stream_request` 都是**连接级**的，头表缓冲正是要靠它们跨请求留下
 * —— 整对象赋值会把容量一起扔掉，`take_headers_into()` 也就退化成每请求一次分配
 * （那正是本笔要省掉的那一笔）。
 */
void reset_request_keep_buffers(uvcpp_http_request& r) {
  r.method    = http_method::HTTP_GET;
  r.url       = "/";  // 与 `uvcpp_http_request()` 的默认值一致（不是清空）
  r.version   = static_cast<uvcpp_http_version>(1);
  r.headers.clear();
  r.body.clear();
  r.stream_id = 0;
}

/**
 * @brief Whether the message being parsed carries a body the server will read.
 *
 * `get_content_length()` returns 0 both for "no body" and for "chunked", so it
 * cannot answer this on its own — the Transfer-Encoding header is what tells
 * the two apart.
 */
bool message_has_body(const uvcpp_http_parser* parser,
                      const http_headers& msg_headers) {
  if (parser->get_content_length() > 0) return true;
  return http_name_equal(http_get_header(msg_headers, "transfer-encoding"),
                         "chunked");
}

/**
 * @brief `Host` 的字段值是否合 `uri-host [ ":" port ]`（RFC 9110 §7.2）。
 *
 * 只做**字符类**判定：不解析 IPv6 字面量、不核端口是不是数字、不核 `%` 后面
 * 那两位是不是十六进制。要挡的是已经被证明会引起路由混淆的那几个字符 ——
 * userinfo 的 `@`、路径的 `/` 与 `\`、列表的 `,` —— 外加空白与控制字符，
 * 而不是去复刻一遍 RFC 3986 的 ABNF。
 *
 * **逗号上故意比 ABNF 严一点**：`reg-name` 的 `sub-delims` 里确实含着逗号，
 * 但 `Host` 不是列表型字段（RFC 9110 §7.2），一个带逗号的值只可能是两个主机名
 * 挤在一起；前端按第一个路由、后端按第二个取，就是缓存投毒 / 虚拟主机绕过。
 */
bool host_value_is_valid(const std::string& raw) {
  // RFC 9110 §5.5：`field-line = field-name ":" OWS field-value OWS`，两头的
  // OWS **不属于**字段值。llhttp 只丢掉冒号后那段前导空白，尾随那段留在值里
  // （实测 `Host: example.com   ` 给出来的值就带三个空格），所以两头自己再修
  // 一次 —— 不修的话，一个合法 Host 只要尾巴上多一个空格就被判成"非法字段值"
  // 回 400，那是**反向**的不合规。
  size_t b = 0;
  size_t e = raw.size();
  while (b < e && (raw[b] == ' ' || raw[b] == '\t')) ++b;
  while (e > b && (raw[e - 1] == ' ' || raw[e - 1] == '\t')) --e;

  if (b == e) return false;   // 空值，或整串都是空白
  for (size_t i = b; i < e; ++i) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z')) {
      continue;
    }
    switch (c) {
      // unreserved 里除字母数字之外的那些
      case '-': case '.': case '_': case '~':
      // pct-encoded 的引导符（不核那两位，宽松）
      case '%':
      // sub-delims 里**除逗号**的那些
      case '!': case '$': case '&': case '\'': case '(': case ')':
      case '*': case '+': case ';': case '=':
      // `:` 分端口，`[` `]` 是 IPv6 字面量
      case ':': case '[': case ']':
        continue;
      default:
        // 空格 / 制表 / CR / LF / `/` / `\` / `@` / `,` / `?` / `#` / 非 ASCII …
        return false;
    }
  }
  return true;
}

/**
 * @brief 两条「畸形报文」判据，一趟扫表判完：Host（RFC 9112 §3.2）与空的
 *        `Transfer-Encoding`（§6.1）。
 *
 * **Host §3.2**：HTTP/1.1 请求必须**恰好一个**、且字段值合法的 `Host`。
 * 缺、多于一行（值相同也一样）、字段值非法，三种都回 400。判据里"多于一行"
 * 与值无关，所以两行同值也算 —— RFC 那句写的是 "more than one Host header
 * field line"，没有给同值开例外。
 * **HTTP/1.0 不受此限**：`Host` 是 1.1 才引入的，1.0 的请求没有它不算违规，
 * 本库照旧服务（探针里 `http10-no-host` 那条把两种处置都算合格，即为此）。
 *
 * **Transfer-Encoding §6.1**：有这一行、但它的值整个是空的。ABNF 是
 * `Transfer-Encoding = #transfer-coding`、`transfer-coding = token`、
 * `token = 1*tchar` —— 空串里一个 `tchar` 都没有，不匹配任何一条。留下一
 * 个框架歧义：前端按"TE 存在"（§6.3：压过 `Content-Length`）还是按"TE 空
 * 所以不存在"（退回 `Content-Length`）处置？两边不一致就是请求走私。
 * 判据只看"所有 TE 行合起来一个元素都没有"：`Transfer-Encoding: ,chunked`
 * 那种前面挂着空列表元素的是合法的（RFC 9110 §5.6.1.2），多行 `TE` 等价于
 * 把它们按逗号接起来，所以只要**有任何一行非空**就不算违规。
 * 注：非空但不认识的编码（`TE: identity`）以及 `TE: chunked` 与 `CL` 同时出现，
 * llhttp 自己就判畸形了，走的是 `has_error()` 那条 400 ＋ 关连接的路；本函数
 * 补的正是它漏掉的那一格：值为空时 llhttp **不把 TE 记成存在**。
 *
 * @return true = 违反其中一条，调用方回 400。
 */
bool message_header_violates(const http_headers& hs, uvcpp_http_version ver) {
  // 两条判据读的是**同一份头表**，原先各扫一遍（`host_header_violates` +
  // `transfer_encoding_is_void`）。合起来一趟就够：名字先按长度分流
  // （`host` 4 / `transfer-encoding` 17），长度不对的头连字节都不用比。
  // 这一段在 h1 的每请求成本里是 438 条（callgrind，`-c 1024 -m 1` 工作点）。
  //
  // 头名在解析器里是**原样保留**的（`ll_on_header_field` 逐字 append，
  // 不小写化），所以比大小写无关的那种比法，和 `message_has_body` 一致。
  const bool check_host = (ver == uvcpp_http_version::HVER_11);
  size_t host_count = 0;
  const http_header* host = nullptr;
  bool te_line = false;        // 有没有 `Transfer-Encoding` 那一行
  bool te_has_content = false; // 其中有没有任何一行带一个非空白字符
  for (size_t i = 0; i < hs.size(); ++i) {
    const http_header& h = hs[i];
    const size_t n = h.name.size();
    if (n == 4) {
      if (check_host && http_name_iequal(h.name.data(), n, "host", 4)) {
        ++host_count;
        // 第一行的值就是下面要验的那个（多余的行本身已经是违规了）。
        if (host == nullptr) host = &h;
      }
      continue;
    }
    if (n == 17 && !te_has_content &&
        http_name_iequal(h.name.data(), n, "transfer-encoding", 17)) {
      te_line = true;
      const std::string& v = h.value;
      for (size_t k = 0; k < v.size(); ++k) {
        if (v[k] != ' ' && v[k] != '\t') {  // 这行有内容
          te_has_content = true;
          break;
        }
      }
    }
  }
  // Host：缺、多于一行（值相同也一样）、字段值非法，三种都算违规。
  if (check_host && (host_count != 1 || !host_value_is_valid(host->value))) {
    return true;
  }
  // Transfer-Encoding：有这一行、但所有行合起来一个元素都没有。
  return te_line && !te_has_content;
}

#if UVCPP_HTTP3_ENABLE
/**
 * @brief h3 请求里的 `:method` 串 → 枚举。
 *
 * 与 `src/http2/uvcpp_h2_session.cpp:77` 那个 `method_from_string` 逐字同源
 * （**逐个比 `http_method_str()` 的输出**，而不是手写一张字符串表 —— 后者会和
 * 枚举一起漂移，而且漂移是静默的）。**故意不共用**：那个是文件局部的，为它开
 * 一个公开的字符串→枚举入口，等于只为下层一处的用处往公开面上添件东西。
 *
 * @return false = 认不出来。调用方按 h2 那边同一套处置：回 400。
 */
bool method_from_string(const std::string& s, http_method& out) {
  for (int i = 0; i <= static_cast<int>(http_method::HTTP_PRI); ++i) {
    const http_method m = static_cast<http_method>(i);
    if (s == http_method_str(m)) {
      out = m;
      return true;
    }
  }
  return false;
}

/**
 * @brief h3 收到的那份请求 → **web 层**的请求对象。
 *
 * 两条腿的请求对象必须逐字同形，处理函数才真的"不知道自己在哪条传输上"：
 * `method` / `url` / `headers` / `body` 一颗不少，`version` 报 HVER_30、
 * `stream_id` 是那条 h3 流（h1 那条路上这一格恒为 0，h3 上是**真流号** ——
 * 包括 0，RFC 9000 §2.1 里客户端发起的双向流就是从 0 起的）。
 *
 * @return false = 这条请求是**畸形**的，调用方回 400（不关连接）。
 */
bool h3_to_web_request(const h3_request& in, uvcpp_http_request& out) {
  http_method m;
  if (!method_from_string(in.method, m)) return false;

  out.method    = m;
  out.url       = in.path;
  out.protocol  = in.protocol;
  out.version   = uvcpp_http_version::HVER_30;
  out.stream_id = static_cast<int32_t>(in.stream_id);

  out.headers.reserve(in.headers.size());
  for (size_t i = 0; i < in.headers.size(); ++i) {
    http_header h;
    h.name  = in.headers[i].name;
    h.value = in.headers[i].value;
    out.headers.push_back(h);
  }
  out.body = uvcpp_buf(in.body.data(), in.body.size());

  out.trailers.reserve(in.trailers.size());
  for (size_t i = 0; i < in.trailers.size(); ++i) {
    http_header h;
    h.name  = in.trailers[i].name;
    h.value = in.trailers[i].value;
    out.trailers.push_back(h);
  }

  // `:authority` 与 `host` 对不上就是**畸形**（RFC 9114 §4.3.1 逐字要求拒掉）：
  // 两个头各说一个主机名是请求走私 / 缓存投毒的经典入口，h2 那边同一条判据
  // （`uvcpp_h2_session.cpp` 里对 `:authority` 那一段）。两个都到齐时必须一致；
  // 只有 `:authority` 时把它落成 `host`，好让 h1/h2/h3 上的处理函数看到同一个
  // 头名（h2 那条腿也是这么做的）。
  if (!in.authority.empty()) {
    const std::string host = http_get_header(out.headers, "host");
    if (!host.empty() && host != in.authority) return false;
    if (host.empty()) out.set_header("host", in.authority);
  }
  return true;
}
#endif

}  // namespace

uvcpp_http_server::uvcpp_http_server() {
  tcp_server_ = new uvcpp_tcp_server();
}

uvcpp_http_server::~uvcpp_http_server() {
#if UVCPP_HTTP3_ENABLE
  // -------------------------------------------------------------------
  // (A) **赶在 `stop_workers()` 之前**把 1..n−1 号循环上的 h3 层与 QUIC 端点
  //     收掉。
  //
  // 为什么必须在这一刻：那两样东西**只能在它们自己那条循环的线程上拆**
  // （`~uvcpp_quic_server` 要关自己的 socket 与定时器，`~uvcpp_h3_connection`
  // 要关它挂在连接上的回调）。`stop_workers()` 一返回，那些线程就 join 掉了 ——
  // 之后再 `delete` 就是往一块已经释放的循环上写 `uv_close`。
  //
  // 这与**上面**那条"先停 worker 再清表"的规则**不冲突，是同一件事的两端**：
  // `contexts_` 那几张表是**共享**的（worker 的退出路径会去 `erase` 同一张表，
  // 所以要先 join 再 clear）；而这里每一格是**它自己循环私有的**，别人不会碰
  // ⇒ 由那条循环自己拆，既安全又必须早。
  //
  // 投递失败的处置（`post_to_loop()` 返回 false，或者有界等待到点还没回来）：
  // **打一行 stderr，然后放任那块内存**。硬删是 UAF —— 那条循环要么已经不在
  // 受理投递了、要么卡在别处，两种情况下我们都没有"它现在不在用这块内存"的
  // 证据。`~uvcpp_loop` 那条"泄漏而不释放"的既有策略就是同一个取舍。
  //
  // **没开 QUIC 就整段跳过**（`quic_servers_` 为空）：那时 1..n−1 号名下既没有
  // h3 层也没有端点，投过去只会空跑一趟 —— 而投递失败时那行 stderr 会把一句
  // 假话写到运维脸上（"h3 层与 QUIC 端点没能收掉"，其实一个都没有）。
  // -------------------------------------------------------------------
  if (tcp_server_ != nullptr && !quic_servers_.empty()) {
    const int n = tcp_server_->loop_count();
    for (int i = 1; i < n; ++i) {
      // `shared_ptr` 而不是栈上的捕获：投递是异步的，而调用方（本析构）**不等
      // 那件任务跑完**（下面 `wait_for` 有墙钟上限）。任务比析构活得久是正常
      // 情形，它捕获的东西就不能是栈上的。
      std::shared_ptr<std::promise<void> > done(new std::promise<void>());
      std::shared_future<void> fut = done->get_future().share();
      const bool queued =
          tcp_server_->post_to_loop(i, [this, i, done]() {
            teardown_quic_loop(static_cast<size_t>(i));
            // 一条循环只投一次，`set_value` 不会重复调（重复调会抛
            // `future_error`，而那是从 worker 线程往外抛的异常 —— 整个进程
            // 会 `std::terminate`）。
            done->set_value();
          });
      if (!queued) {
        std::fprintf(stderr,
                     "[uvcpp_http_server] 析构：第 %d 条循环已经不收投递了，"
                     "它名下的 h3 层与 QUIC 端点没能收掉（放任不释放）\n",
                     i);
        continue;
      }
      // 有界等待，与 `uvcpp_tcp_server::start_worker_listeners()` 同一个数。
      // 收掉一格是"关几个句柄"，正常在微秒级；等不到就说明那条线程不转了，
      // 再等下去只会把析构变成挂死。
      if (fut.wait_for(std::chrono::seconds(5)) !=
          std::future_status::ready) {
        std::fprintf(stderr,
                     "[uvcpp_http_server] 析构：等第 %d 条循环收尾超过 5 秒，"
                     "不再等（那块内存放任不释放）\n",
                     i);
      }
    }
  }
#endif

  // **先让工作循环退场，再动下面那几张按循环切的表。**
  //
  // 上面那条"析构发生在所有循环线程都已经停了之后"是**调用方的责任**，而
  // 调用方**没有任何办法**履行它：能停 worker 的 `stop_workers()` 是私有的，
  // 唯一会调它的地方是本函数末尾那句 `delete tcp_server_` —— 那时代码已经把
  // `contexts_` 逐张清过一遍了。于是 worker 的退出路径（关掉自己名下的连接
  // ⇒ 关闭回调 ⇒ `remove_ctx` ⇒ `tbl.erase`）与这次清表撞在同一张表上：worker
  // 的迭代器指向刚被 `clear()` 释放掉的结点，`erase` 落在
  // `_Rb_tree_rebalance_for_erase` 里读一个已经还回去的结点 ⇒ SIGSEGV。
  //
  // 实测（`tests/functional/web_multiloop_func.cpp` 的 `seq n=4` 一档，4 核、
  // 8 路并发）：2000 次里崩 20 次（约 1%），崩点恒在这一句下面那张
  // `tbl.clear()` 与 worker 的 `remove_ctx` 之间。
  //
  // 单循环没有 worker（`set_loops(1)` 时 `workers_` 是空的），这一句是空操作
  // ⇒ 既有单循环行为逐字节不变。
  if (tcp_server_ != nullptr) tcp_server_->stop_workers();

  // 按循环走一遍。到这儿循环线程**真的**都停了：worker 已经 join，acceptor
  // 那条循环的线程是调用者自己的（`run()` 就在调用者线程上跑，见
  // `uvcpp_tcp_server::run`），它此刻正在这个析构里，不可能同时泵循环。
  for (size_t li = 0; li < contexts_.size(); ++li) {
    std::map<uvcpp_tcp_client*, conn_ctx>& tbl = contexts_[li];
    for (auto& kv : tbl) {
      delete kv.second.parser;
#if UVCPP_NGHTTP2_ENABLE
      // h2 层是连接上下文 new 出来的，反过来**没有任何别的表登记它** ⇒ 少了这
      // 一句就是每条还活着的 h2 连接漏一个 nghttp2 会话和它的缓冲区。
      //
      // 能走到这里说明 `remove_ctx` 没跑过：正常收尾（`close_all_clients()`）
      // 会经由客户端的关闭回调跑它，剩下的是"客户端先于服务端被释放"这种路
      // （`release_client` 只 `delete`，一个回调都不发）。
      //
      // 这里**不**唤醒队列里的 `done`：此刻持有者的上下文（webapp 那套）多半
      // 已经在拆，唤醒了也没有接收方。h1 那半同样是丢下队列直接走的 —— 与它
      // 对齐，而不是单独给 h2 编一套。
      delete kv.second.h2;
      kv.second.h2 = nullptr;
#endif
      // 复用形态那个写请求（如果有）也在这张表里，与 `remove_ctx` 同一句 ——
      // 能走到这里的正是"`remove_ctx` 没跑过"的那种路（客户端先于服务端被释放）。
      if (kv.second.wrecycle != nullptr) {
        uvcpp_tcp_client::release_owned_write(kv.second.wrecycle);
        kv.second.wrecycle = nullptr;
      }
    }
    tbl.clear();
  }
  // 登记簿跟着表一起空掉（"键集 ≡ 各表的键并集"这条不变量在这里也要成立）：
  // 上面那一轮把表清干净了，簿子留着的话，`ctx_gens_` 就会答出"已经不在任何表里"
  // 的代次号 —— 正是这个 API 最不该给的答案（`is_connected()` 会对一条早已注销的
  // 连接报 true）。此刻循环线程都已经停（见上），这一句不是同步，只是维持不变量。
  {
    std::lock_guard<std::mutex> lk(ctx_gen_mu_);
    ctx_gens_.clear();
  }
#if UVCPP_HTTP3_ENABLE
  // -------------------------------------------------------------------
  // (C) 0 号那一格：它就是我们此刻所在这条循环（`run()` 在调用方线程上跑），
  //     所以**就地拆**，不投递。
  //
  // h3 驱动层是 `on_quic_connection()` new 出来的，**只有 `quic_ctxs_` 登记它**
  // ⇒ 少了这一步就是每条还活着的 h3 连接漏一个 h3 层（连同 nghttp3 的会话与它
  // 的缓冲）。能走到这里的正是"`on_disconnect` 没跑过"那种路（客户端先于服务端
  // 被释放），与上面 h2 那一段是同一个形状、同一个理由。1..n−1 号在 (A) 段已
  // 经各自拆完了，所以这里只走一格。
  //
  // `quic_servers_` 在那一段里已经被逐格置空；这里连同两张 vector 一起清掉
  // （`quic_listen_port()` / `quic_fanout()` 之后都报"没有这条腿"）。
  if (!quic_servers_.empty()) {
    teardown_quic_loop(0);
    quic_servers_.clear();
    quic_ctxs_.clear();
    quic_conn_counts_.clear();
  }
#endif
  delete tcp_server_;
  tcp_server_ = nullptr;
}

#if UVCPP_HTTP3_ENABLE

void uvcpp_http_server::teardown_quic_loop(size_t loop_index) {
  if (loop_index >= quic_servers_.size()) return;

  // **先整张摘表、再删对象** —— 与 `remove_h3_ctx()` 同一个次序，而这里是它最
  // 要命的一处：`~uvcpp_h3_connection` 会跑自己的收尾，那一步可能回到
  // `remove_h3_ctx(conn)`；表里那一项要是还指着这个正在析构的对象，那边就会
  // **再删一次**（`t.erase(it); ... delete h3;`）。所以先把每一项的 `h3` 置空、
  // 整张 clear（此后重入进来什么都查不到，与"已经摘过"同一条路），对象攒在
  // 一边最后统一放。
  std::map<uvcpp_quic_connection*, quic_ctx>& tbl = quic_ctxs_[loop_index];
  std::vector<uvcpp_h3_connection*> doomed;
  doomed.reserve(tbl.size());
  for (std::map<uvcpp_quic_connection*, quic_ctx>::iterator it = tbl.begin();
       it != tbl.end(); ++it) {
    if (it->second.h3 != nullptr) doomed.push_back(it->second.h3);
    it->second.h3 = nullptr;
  }
  tbl.clear();
  for (size_t i = 0; i < doomed.size(); ++i) delete doomed[i];

  // 端点在 h3 之后删（理由与今天那段注释同源：h3 持有 QUIC 连接，反向不成立）。
  // 它会连带收掉还在的连接，那些回调进来时表已经空了。
  delete quic_servers_[loop_index];
  quic_servers_[loop_index] = nullptr;

  {
    std::lock_guard<std::mutex> lk(quic_conn_counts_mu_);
    if (loop_index < quic_conn_counts_.size()) {
      quic_conn_counts_[loop_index] = 0;
    }
  }
}

#endif  // UVCPP_HTTP3_ENABLE

std::map<uvcpp_tcp_client*, uvcpp_http_server::conn_ctx>&
uvcpp_http_server::ctxs_of(uvcpp_tcp_client* client) {
  // **单循环的快路径**：表只有一张，就不去读 `client` 了。多循环 §7 要求
  // 单循环的热路径不被这套东西改形状，而 `contexts_` 是本层最热的一次查找。
  if (contexts_.size() == 1) return contexts_[0];

  return ctxs_at((client != nullptr) ? client->loop_index() : 0);
}

std::map<uvcpp_tcp_client*, uvcpp_http_server::conn_ctx>&
uvcpp_http_server::ctxs_here() {
  return ctxs_at(uvcpp_loop_index_of_this_thread());
}

const std::map<uvcpp_tcp_client*, uvcpp_http_server::conn_ctx>&
uvcpp_http_server::ctxs_of(uvcpp_tcp_client* client) const {
  if (contexts_.size() == 1) return contexts_[0];
  return ctxs_at((client != nullptr) ? client->loop_index() : 0);
}

const std::map<uvcpp_tcp_client*, uvcpp_http_server::conn_ctx>&
uvcpp_http_server::ctxs_at(int loop_index) const {
  if (loop_index < 0 || static_cast<size_t>(loop_index) >= contexts_.size()) {
    return contexts_[0];
  }
  return contexts_[static_cast<size_t>(loop_index)];
}

std::map<uvcpp_tcp_client*, uvcpp_http_server::conn_ctx>&
uvcpp_http_server::ctxs_at(int loop_index) {
  if (loop_index < 0) {
    // -1 ＝ "不在任何循环线程上"（`uvcpp_loop_index_of_this_thread()` 的初值）
    // —— 也就是在循环线程之外调了 `ctxs_of()` / `ctxs_here()` 这一族。
    //
    // **单循环下这不是错**：表只有一张，夹回 0 号与从前逐字相同，所以留在这里
    // 的正是"单循环下唯一那张表"那条原路（`contexts_.size() == 1`）。
    //
    // **n > 1 下它是错的，而且错得安静**：0 号只是 n 张里的一张，夹回去会让
    // "漏调了一条循环"看着**正常** —— `begin_h2_goaway()` 的契约是每条循环各调
    // 一次（`src/web/uvcpp_http_server.h:207-210`），夹回 0 号时它只向 0 号循环
    // 道了别，却返回一个像样的条数。这比越界更坏：越界至少当场崩。
    //
    // 所以多循环下**直接 abort** —— 与 libuv 对 API 误用的处理一致（先例见
    // `src/handle/uvcpp_handle.cpp:363` 那句注释）。不进热路径：单循环在
    // `ctxs_of()` 里已经提前 return 了，走得到这儿的只有 n > 1。
    if (contexts_.size() > 1) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] ctxs_at(): 不在任何循环线程上"
                   "（loop_index == -1），多循环下这张表是不确定的\n");
      std::abort();
    }
    return contexts_[0];
  }
  if (static_cast<size_t>(loop_index) >= contexts_.size()) {
    // 到不了这儿：`listen()` 之后每条连接的循环号都落在表内（表就是按
    // `loop_count()` 定尺寸的）。留个夹回，是因为它至少是**越界安全**的。
    return contexts_[0];
  }
  return contexts_[static_cast<size_t>(loop_index)];
}

int uvcpp_http_server::set_loops(int n) {
  // 取值校验**先做**：它与状态无关，越界的 n 在任何时刻都是调用方的错。
  if (n < 1 || n > 64) return UV_EINVAL;

  // 这道门是本层独有的，理由见头文件：QUIC 端点按 `loop_count()` 一次建齐，
  // 建完再改 n 就是"表按 1 张定尺寸、而回调报 1..n−1 号"。
  //
  // **只挡 n > 1**：`set_loops(1)` 在下面那条腿的契约里是"幂等的空操作"（它
  // 什么都不建），h3 端点建好之后调它一个字节都不会变，没有理由拒。
#if UVCPP_HTTP3_ENABLE
  if (n > 1 && !quic_servers_.empty()) return UV_EBUSY;
#endif

  // 余下的判据（越界以外的 `UV_EBUSY`、`n == 1` 的幂等）在下面那条腿上，那边
  // 才是权威 —— 这里不重抄一遍，抄出来迟早会与它走岔。
  return tcp_server_->set_loops(n);
}

int uvcpp_http_server::loop_count() const {
  return tcp_server_ != nullptr ? tcp_server_->loop_count() : 1;
}

#if UVCPP_HTTP3_ENABLE

std::map<uvcpp_quic_connection*, uvcpp_http_server::quic_ctx>&
uvcpp_http_server::quic_ctxs_at(int loop_index) {
  // 与 `ctxs_at()` 逐条同源（那边的注释是权威，这里只留本层特有的差别）：
  // **-1 ＝ "不在任何循环线程上"**，单循环下夹回 0 号与从前逐字相同、多循环下
  // 直接 abort（夹回去会让"漏调了一条循环"看着正常）。差别只有一处：h3 的
  // 回调**只可能**从某条循环的线程上进来，所以走到 -1 那条路上的不是"上层忘了
  // 在每条循环上各调一次"，而是"本层在循环线程之外碰了这张表" —— 同样是误用。
  if (loop_index < 0) {
    if (quic_ctxs_.size() > 1) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] quic_ctxs_at(): 不在任何循环线程上"
                   "（loop_index == -1），多循环下这张表是不确定的\n");
      std::abort();
    }
    return quic_ctxs_[0];
  }
  if (static_cast<size_t>(loop_index) >= quic_ctxs_.size()) {
    return quic_ctxs_[0];
  }
  return quic_ctxs_[static_cast<size_t>(loop_index)];
}

const std::map<uvcpp_quic_connection*, uvcpp_http_server::quic_ctx>&
uvcpp_http_server::quic_ctxs_at(int loop_index) const {
  // 与上面那条非 const 版**判据逐条相同**（那边是权威）：越界的两个分支都夹回
  // 0 号，因为 0 号一定存在（`quic_ctxs_` 只在 `listen_quic()` 里非空）。
  //
  // 这里**没有 abort 那条路**，而且不是漏了：-1 只在"不在任何循环线程上"出现，
  // 而这条 const 路唯一的调用方是按凭据里存好的序号查表 —— 那个序号是**造凭据
  // 那一刻**在循环线程上记下来的，不会是 -1。
  if (loop_index < 0 ||
      static_cast<size_t>(loop_index) >= quic_ctxs_.size()) {
    return quic_ctxs_[0];
  }
  return quic_ctxs_[static_cast<size_t>(loop_index)];
}

std::map<uvcpp_quic_connection*, uvcpp_http_server::quic_ctx>&
uvcpp_http_server::quic_ctxs_here() {
  // 单循环的快路径：表只有一张，就不去读线程本地量了 —— 与 `ctxs_of()` 那条
  // 同一个理由（`contexts_` 是本层最热的一次查找，这里是 h3 每次回调的入口）。
  if (quic_ctxs_.size() == 1) return quic_ctxs_[0];
  return quic_ctxs_at(uvcpp_loop_index_of_this_thread());
}

int uvcpp_http_server::quic_loop_index_here() const {
  // 单循环：夹回 0 号（计数表也只有一格），与 `quic_ctxs_here()` 的快路径同源。
  if (quic_ctxs_.size() <= 1) return 0;
  const int li = uvcpp_loop_index_of_this_thread();
  // 多循环下判据与 `quic_ctxs_at()` 逐条相同（那边是权威）。这里不复用那个函数，
  // 是因为它回的是表不是序号 —— 但**判据不能因此走岔**，所以这一段的每一句都与
  // 它对齐：-1 ＝ 不在任何循环线程上 ＝ 误用，越界 ＝ 到不了。
  if (li < 0 || static_cast<size_t>(li) >= quic_ctxs_.size()) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] quic_loop_index_here(): 不在任何循环线程上"
                 "（loop_index == %d），多循环下这个序号是不确定的\n",
                 li);
    std::abort();
  }
  return li;
}

bool uvcpp_http_server::quic_fanout() const {
  // 数**真的建起来了**的端点，不是数数组长度：回滚那条路上（`listen_quic()` 的
  // err 分支）向量可能留着 n 格却一格都不服务 —— 那种时候它必须报 false。
  size_t live = 0;
  for (size_t i = 0; i < quic_servers_.size(); ++i) {
    if (quic_servers_[i] != nullptr) ++live;
  }
  return live > 1;
}

size_t uvcpp_http_server::quic_connection_count_at(int loop_index) const {
  if (loop_index < 0) return 0;
  std::lock_guard<std::mutex> lk(quic_conn_counts_mu_);
  if (static_cast<size_t>(loop_index) >= quic_conn_counts_.size()) return 0;
  const int v = quic_conn_counts_[static_cast<size_t>(loop_index)];
  return v > 0 ? static_cast<size_t>(v) : 0;
}

size_t uvcpp_http_server::quic_connection_count() const {
  size_t sum = 0;
  std::lock_guard<std::mutex> lk(quic_conn_counts_mu_);
  for (size_t i = 0; i < quic_conn_counts_.size(); ++i) {
    if (quic_conn_counts_[i] > 0) sum += static_cast<size_t>(quic_conn_counts_[i]);
  }
  return sum;
}

#endif  // UVCPP_HTTP3_ENABLE

int uvcpp_http_server::bind(const char* ip, int port) {
  return tcp_server_->bind(ip, port);
}

int uvcpp_http_server::bindIpv4(const char* ip, int port) {
  return tcp_server_->bindIpv4(ip, port);
}

int uvcpp_http_server::bindIpv6(const char* ip, int port) {
  return tcp_server_->bindIpv6(ip, port);
}

int uvcpp_http_server::listen(int backlog) {
  // **表要在 acceptor 起来之前就定好尺寸。** 多循环下工作循环的线程是在
  // `uvcpp_tcp_server::set_loops()` 里被放行的（那句 `w->start()`，
  // `src/net/uvcpp_tcp_server.cpp:332`）—— **不是**在 `listen()` 里，`listen()`
  // 只做 bind + `uv_listen()`。那些线程一旦接手连接就会走 `on_tcp_connection()`
  // → `ctxs_of()`；表还是空的话那一步就是越界取表。`set_loops()` 必须在
  // `listen()` 之前调，"几条循环"在这一刻已经是定局，所以问得到。
  //
  // 尺寸此后**只读**：`set_loops()` 在 listen 之后返回 `UV_EBUSY`，循环数不会
  // 再变，扩容也就无从谈起 —— 取表那一路不需要锁，靠的就是这一句。
  contexts_.resize(static_cast<size_t>(tcp_server_->loop_count()));

  int rc = tcp_server_->listen(
      [this](uvcpp_tcp_client* client) { on_tcp_connection(client); },
      backlog);
  if (rc == 0) status_ = HTTP_SERVER_LISTENING;
  return rc;
}

#if UVCPP_HTTP3_ENABLE

void uvcpp_http_server::set_quic_ssl_context(uvcpp_ssl_context* ctx) {
  quic_ssl_ctx_ = ctx;
  // 端点已经起来了就是"装晚了"：**不静默吞掉**，转交给 QUIC 端点让它按自己的
  // 规矩处置（新连接用新凭证，已经建好的那些各持自己那一份 —— QUIC 的凭证是
  // 按连接存的，改不了）。
  //
  // 多循环下 1..n−1 号各自跑在**别的线程**上：这一格是它们读、我们写，所以
  // 必须投过去在它们自己那条线程上写（`uvcpp_quic_server` 的线程约定，见它的
  // 类注释）。0 号就地写。
  for (size_t i = 0; i < quic_servers_.size(); ++i) {
    if (quic_servers_[i] == nullptr) continue;
    if (i == 0) {
      quic_servers_[0]->set_ssl_context(ctx);
      continue;
    }
    uvcpp_quic_server* ep = quic_servers_[i];
    if (!run_on_quic_loop(i, [ep, ctx]() { ep->set_ssl_context(ctx); })) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] set_quic_ssl_context(): 第 %" UVCPP_SIZE_FMT " 条循环"
                   "上的 QUIC 端点没换成凭证\n",
                   i);
    }
  }
}

bool uvcpp_http_server::run_on_quic_loop(size_t loop_index,
                                         std::function<void()> fn) {
  if (loop_index == 0) {
    // 0 号吃不进投递（`post_to_loop()` 契约），就地跑 —— 调用方本来就在那条
    // 线程上（这是本函数的契约，不是推断）。
    fn();
    return true;
  }

  std::shared_ptr<std::promise<void> > done(new std::promise<void>());
  std::shared_future<void> fut = done->get_future().share();
  const bool queued = tcp_server_ != nullptr &&
                      tcp_server_->post_to_loop(static_cast<int>(loop_index),
                                                [fn, done]() {
                                                  fn();
                                                  done->set_value();
                                                });
  if (!queued) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] 第 %" UVCPP_SIZE_FMT " 条循环不收投递了（已经在收尾？）\n",
                 loop_index);
    return false;
  }
  if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] 等第 %" UVCPP_SIZE_FMT " 条循环做完超过 5 秒，不再等\n",
                 loop_index);
    return false;
  }
  return true;
}

uvcpp_quic_server* uvcpp_http_server::start_quic_endpoint(uvcpp_loop* loop,
                                                          const char* ip,
                                                          bool reuseport,
                                                          int* port_io,
                                                          int* rc_out) {
  *rc_out = 0;
  if (loop == nullptr) {
    *rc_out = UV_EINVAL;
    return nullptr;
  }

  uvcpp_quic_server* qs = new uvcpp_quic_server(loop);
  qs->set_ssl_context(quic_ssl_ctx_);
  // ALPN **不覆盖**：用 `uvcpp_quic_server` 自己的默认（`"h3"`）。在 TCP 那份
  // 名单里加 `h3` 才是要避免的事 —— 对端选了它就会把 h3 的二进制流喂给 llhttp。
  if (reuseport) qs->set_reuseport(true);

  // **空串要还原成 nullptr**：`uvcpp_quic_server::bind()` 拿 nullptr 表示"通配"，
  // 而空串在它那里是个**畸形字面量**（`literal_fits(AF_INET, "")` 与
  // `literal_fits(AF_INET6, "")` 都假）⇒ `UV_EINVAL`。`listen_quic()` 为了把地址
  // 按值交给扇出那些任务，先把 nullptr 存成了 `std::string`，到这里必须还原
  // —— 少这一句，"传 ip=nullptr"这条最常用的路会当场起不来。
  const char* bind_ip = (ip != nullptr && ip[0] != '\0') ? ip : nullptr;

  int rc = qs->bind(bind_ip, *port_io);
  if (rc == 0) {
    rc = qs->listen(
        [this](uvcpp_quic_connection* conn) { on_quic_connection(conn); });
  }
  if (rc != 0) {
    // 建到一半失败（端口被占之类）：端点**没有被登记**，当场放掉 —— 否则就是
    // 一次 bind 失败的调用漏一条 UDP socket 加一条循环引用。
    delete qs;
    *rc_out = rc;
    return nullptr;
  }

  // 端口 0 在这里就地解析成真号：**扇出的每个端点必须绑同一个端口**（内核那个
  // REUSEPORT 组是按 `(地址, 端口)` 找的），各绑各的 0 就各挑各的口，组根本
  // 不存在。真号的传播方向是**由 0 号往外**，所以只有这一处需要读回来。
  if (*port_io == 0) *port_io = qs->configured_port();
  return qs;
}

int uvcpp_http_server::listen_quic(int port, const char* ip) {
  // QUIC 没有明文模式（ALPN 是 TLS 扩展）：没有凭证就没有这条腿。在这里先挡
  // 一道，好让调用方在"端口开着、一条连接都建不起来"之前就知道出了什么事
  // —— 那正是 `uvcpp_quic_server::listen()` 自己的判据，这里只是提前报。
  if (quic_ssl_ctx_ == nullptr) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] listen_quic(): 还没装 TLS 上下文，"
                 "先调 set_quic_ssl_context()\n");
    return UV_EINVAL;
  }
  // 非空的 `quic_servers_` 有两种来路：这条腿已经起来了（重复调），或者上一次
  // 建到一半、收尾时那条循环已经不收投递了（下面 err 那一段）。两种都拒：前者
  // 按契约是 `UV_EALREADY`，后者再 `assign` 一次会把可能还排在某条循环邮箱里
  // 的收尾任务指向的容器拆掉 —— 那正是 UAF。第二种来路在 stderr 上已经吼过。
  if (!quic_servers_.empty()) return UV_EALREADY;

  const int n = loop_count();

  // **表和端点数组按循环数一次定死**（理由同 `listen()`：工作循环的线程早在
  // `set_loops()` 里就放行了，1..n−1 号的回调随时可能来）。此后尺寸只读：
  // `set_loops()` 在 `listen_quic()` 之后返 `UV_EBUSY`。
  quic_ctxs_.assign(static_cast<size_t>(n),
                    std::map<uvcpp_quic_connection*, quic_ctx>());
  quic_servers_.assign(static_cast<size_t>(n),
                       static_cast<uvcpp_quic_server*>(nullptr));
  quic_conn_counts_.assign(static_cast<size_t>(n), 0);

  // **地址取一份自己的**：扇出那些任务是在别的线程上跑的，`ip` 的生存期归调用方
  // （`listen_quic()` 返回之后它就可能是野指针了，而任务不保证已经跑完 —— 超时
  // 那条路上它就还挂在邮箱里）。`bind_ip` 按值进每个任务。
  const std::string bind_ip = (ip != nullptr) ? std::string(ip) : std::string();

  // ---- 0 号：建在 acceptor 那条循环上，就地建 ----
  int rc = 0;
  int real_port = port;
  uvcpp_quic_server* ep0 =
      start_quic_endpoint(tcp_server_->get_loop(), bind_ip.c_str(), n > 1,
                          &real_port, &rc);
  if (ep0 == nullptr) {
    quic_servers_.clear();
    quic_ctxs_.clear();
    quic_conn_counts_.clear();
    return rc;
  }
  quic_servers_[0] = ep0;

  // `n == 1`（没调过 `set_loops()`）= 今天的行为，到此为止。
  //
  // **回落也在这一条上**：`n > 1` 但这个平台/这份 libuv 绑不上 REUSEPORT
  // （Windows 无条件拒、macOS 没实现、libuv < 1.50 没这个标志）⇒ 0 号自己已经
  // 把那一次去掉标志重绑过了（`start_quic_endpoint` 之下的
  // `uvcpp_quic_server::start_listen()`），这里**不再建 1..n−1 号**：它们没有
  // REUSEPORT 就抢不到同一个端口，建出来的只会是"最后绑上的那个把前一个踢掉"。
  if (n == 1 || !ep0->is_reuseport()) {
    if (n > 1) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] listen_quic(): 这份 libuv/平台绑不上"
                   "REUSEPORT（UDP 要 1.50+，且只在 Linux/FreeBSD/DragonFly/"
                   "Solaris 11.4/AIX 7.2.5 上有负载均衡语义）：QUIC 只有 0 号"
                   "一个端点，`quic_fanout()` 为 false\n");
    }
    status_ = HTTP_SERVER_LISTENING;
    return 0;
  }

  // ---- 1..n−1 号：各投到对应的工作循环上，在那些线程上建 ----
  int first_err = 0;
  for (int i = 1; i < n; ++i) {
    // 循环指针在这里取就够：`workers_` 在 `set_loops()` 之后不再变，而
    // `post_to_loop()` 马上还会替我们再判一次下标。真正必须在工作线程上做的是
    // **建端点**（`uv_udp_bind()` 要求在本循环的线程上跑），那是下面那个任务。
    uvcpp_loop* worker_loop = tcp_server_->loop_at(i);
    if (worker_loop == nullptr) {
      first_err = UV_EINVAL;
      break;
    }

    // 结果用 `shared_ptr` 带回来，不用栈上的引用：超时那条路上任务还挂在邮箱
    // 里，捕获一个已经出栈的变量就是 UAF。
    std::shared_ptr<int> wrc(new int(0));
    uvcpp_quic_server** slot = &quic_servers_[static_cast<size_t>(i)];
    const bool done = run_on_quic_loop(
        static_cast<size_t>(i),
        [this, bind_ip, real_port, wrc, slot, worker_loop]() {
          int rc2 = 0;
          int p = real_port;
          uvcpp_quic_server* ep = start_quic_endpoint(
              worker_loop, bind_ip.c_str(), true, &p, &rc2);
          if (ep == nullptr) {
            *wrc = (rc2 != 0) ? rc2 : UV_EIO;
            return;
          }
          if (!ep->is_reuseport()) {
            // 0 号带得上、它带不上：这种平台不会走到这里（0 号那次绑定与它
            // 走的是同一个探测），真走到了就只能放弃这一个 —— 留着它的话，
            // 它绑的是同一个端口却没有 REUSEPORT，等于把 0 号挤掉。
            delete ep;
            *wrc = UV_ENOTSUP;
            return;
          }
          *slot = ep;
          *wrc = 0;
        });

    if (!done) {
      if (first_err == 0) first_err = UV_ETIMEDOUT;
      break;
    }
    if (*wrc != 0) {
      first_err = *wrc;
      break;
    }
  }

  if (first_err == 0) {
    // 两种传输共用一个状态字：这里已经真的在服务了，与 `listen()` 那一句同义。
    status_ = HTTP_SERVER_LISTENING;
    return 0;
  }

  // ---- 回滚 ----
  //
  // 已经建起来的那几个（含 0 号）全部拆掉，本对象回到"没有 QUIC 腿"的状态。
  //
  // 拆法照 `uvcpp_tcp_server::start_worker_listeners()` 那条半成品回收：**每个
  // 端点都由它自己那条循环收**（`teardown_quic_loop()`），投递是 FIFO 的，所以
  // 这条一定排在上面那个建端点的任务之后跑 —— 超时那条路上也不会与它并发。
  // 幂等：空的那格直接返回。
  //
  // `quic_servers_` **不在这里清空**，与 `worker_listeners_` 同一个理由：那条
  // 任务可能还挂在某条循环的邮箱里，清掉容器就是让它去读一块已经还回去的内存。
  // 于是"清不清得掉"要看每个 `run_on_quic_loop()` 的返回值：
  bool all_torn = true;
  for (int i = 1; i < n; ++i) {
    const int idx = i;
    if (!run_on_quic_loop(static_cast<size_t>(i),
                          [this, idx]() { teardown_quic_loop(idx); })) {
      all_torn = false;
    }
  }
  teardown_quic_loop(0);  // 0 号就地拆（`run_on_quic_loop(0)` 也是就地）

  if (all_torn) {
    // 都拆干净了 ⇒ 回到"从没 `listen_quic()` 过"，调用方可以修好端口再试一次。
    quic_servers_.clear();
    quic_ctxs_.clear();
    quic_conn_counts_.clear();
  } else {
    // 有循环没能在 5 秒内把活干完。**不放任也不硬来**：容器留着（那块内存由
    // 析构兜底），只把话说清楚 —— 上面已经逐条打过 stderr 了。
    std::fprintf(stderr,
                 "[uvcpp_http_server] listen_quic(): 回滚没能全部完成，"
                 "本对象这次不再受理 QUIC 端点\n");
  }
  return first_err;
}

int uvcpp_http_server::quic_listen_port() const {
  return (!quic_servers_.empty() && quic_servers_[0] != nullptr)
             ? quic_servers_[0]->configured_port()
             : 0;
}

#endif  // UVCPP_HTTP3_ENABLE

void uvcpp_http_server::on_request(http_request_handler handler) {
  default_handler_ = std::move(handler);
}

void uvcpp_http_server::on_upgrade(upgrade_handler_t handler) {
  upgrade_handler_ = std::move(handler);
}

#if UVCPP_NGHTTP2_ENABLE
void uvcpp_http_server::set_h2_extended_connect_handler(
    h2_extended_connect_handler_t handler) {
  h2_extended_connect_handler_ = std::move(handler);
}
#endif

#if UVCPP_HTTP3_ENABLE
void uvcpp_http_server::set_h3_extended_connect_handler(
    h3_extended_connect_handler_t handler) {
  h3_extended_connect_handler_ = std::move(handler);
}
#endif

std::string uvcpp_http_server::take_upgrade_leftover(uvcpp_tcp_client* client) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return std::string();
  std::string out;
  out.swap(it->second.pending);
  it->second.upgrading = false;
  return out;
}

void uvcpp_http_server::get(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_GET, path, std::move(h)});
}
void uvcpp_http_server::post(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_POST, path, std::move(h)});
}
void uvcpp_http_server::post_stream(const std::string& path, http_stream_handler h) {
  routes_.push_back({http_method::HTTP_POST, path, {}, std::move(h)});
}
void uvcpp_http_server::put(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_PUT, path, std::move(h)});
}
void uvcpp_http_server::del(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_DELETE, path, std::move(h)});
}
void uvcpp_http_server::options(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_OPTIONS, path, std::move(h)});
}
void uvcpp_http_server::patch(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_PATCH, path, std::move(h)});
}
void uvcpp_http_server::head(const std::string& path, http_request_handler h) {
  routes_.push_back({http_method::HTTP_HEAD, path, std::move(h)});
}

http_request_handler uvcpp_http_server::find_handler(
    http_method method, const std::string& path) {
  // Route on "method + path" (query string stripped); parsing the query is left
  // to the handler (this layer has no query parser of its own — the webapp
  // layer's is `web_parse_query()`). Without this, "GET /api/x?id=1" would
  // never match a route registered as "/api/x".
  //
  // 剥查询串只算**长度**，不落成 `std::string`：`path` 来自请求行解析，长
  // 度常常超过 SSO 的 15 字节 ⇒ `std::string p = path` 每请求一次堆分配 +
  // 一次 free，再加上 substr 那次。比较用 `compare(0, n, r.path)`，它不构造
  // 子串（先比长度、再 memcmp）。
  const size_t q = path.find('?');
  const size_t n = (q == std::string::npos) ? path.size() : q;
  for (const auto& r : routes_) {
    if (r.method == method && r.path.size() == n &&
        path.compare(0, n, r.path) == 0)
      return r.handler;
  }
  return default_handler_;
}

http_stream_handler uvcpp_http_server::find_stream_handler(
    http_method method, const std::string& path) {
  // Match on "method + path" (query string stripped), like find_handler --
  // 同样只算长度、不构造剥掉查询串的临时串。
  const size_t q = path.find('?');
  const size_t n = (q == std::string::npos) ? path.size() : q;
  for (const auto& r : routes_) {
    if (r.method == method && r.path.size() == n &&
        path.compare(0, n, r.path) == 0 && r.stream_handler)
      return r.stream_handler;
  }
  return http_stream_handler();
}

void uvcpp_http_server::on_tcp_connection(uvcpp_tcp_client* client) {
#if UVCPP_NGHTTP2_ENABLE
  // 分流点。**不需要嗅字节**：`uvcpp_tcp_server` 已经保证"握手没完不投递连接"，
  // 走到这里 ALPN 已是终局。
  if (http2_enabled_ && client != nullptr && client->is_tls() &&
      client->tls_alpn_selected() == "h2") {
    on_tcp_connection_h2(client);
    return;
  }
#endif
  conn_ctx ctx;
  ctx.parser = new uvcpp_http_parser(http_parser_mode::PARSE_REQUEST);
  // 上限装在解析器上，而不是在这里的 `on_data` 里比大小：要挡的是"32 MiB 的
  // 头被完整解析一遍"，那就只能在解析**过程中**停，等头收完再看已经晚了。
  ctx.parser->set_max_header_bytes(max_header_bytes_);
  ctx.parser->set_max_url_bytes(max_url_bytes_);
  // 先自增再赋值：0 要留给"不在登记表里"，不然"第 0 条连接"和"没连接"就分不开了。
  ctx.generation = ++next_generation_;
  ctxs_of(client)[client] = ctx;
  note_ctx_generation(client, ctx.generation);  // 与上一句成对，见它的声明

  // 连接级钩子在**登记之后**才跑：钩子的用途就是"给这条连接做簿记"，而簿记
  // 的第一步是拿到它的身份 —— `connection_generation(client)` 得先有效。
  // （放在登记之前的话它恒返回 0，等于这个钩子拿不到连接标识，与文档说的
  // 「比任何请求都早的簿记点」自相矛盾。）仍然早于该连接上的第一个请求。
  //
  // 失败不能影响服务 —— 钩子是观察者，不是参与者，所以异常就地吞掉。
  if (connection_handler_) {
    try {
      connection_handler_(client);
    } catch (const std::exception& e) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] on_connection callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] on_connection callback threw (unknown)\n");
    }
  }

  conn_ctx* pctx = &ctxs_of(client)[client];

  // 单消息暂存按**消息**清，不按读清。
  //
  // 两者不是一回事：一次读里完全可能有两条消息（流水线，或者只是不等响应就把
  // 下一条写出来），而"读在哪结束"由 TCP 分片决定。按读清的话，同一个读里的
  // 第二条请求会**带着上一条的 body**（`body_buf` 是累加的）与上一条的状态
  // （`rejected` / `expect_continue` …）—— 症状是第二条请求收到上一条的 body，
  // 且随着分片位置变化。
  pctx->parser->set_on_message_begin([pctx]() {
    pctx->msg_done = false;
    pctx->headers_done = false;
    pctx->body_buf.clear();
    pctx->body_bytes = 0;
    pctx->body_overflow = false;
    pctx->accept_encoding.clear();
    pctx->is_head = false;
    pctx->rejected = false;
    pctx->close_after_message = false;
    pctx->defer_close_to_message_end = false;
    pctx->expect_continue = false;
    pctx->stream_view_built = false;
    // 只清内容、不还缓冲：下一条消息的头还要搬进这块表（`take_headers_into()`）。
    reset_request_keep_buffers(pctx->stream_request);
  });

  pctx->parser->set_on_body([this, pctx, client](const char* at, size_t len) {
    if (pctx->stream_handler) {
      pctx->stream_handler(http_stream_event::BODY, at, len,
                           pctx->stream_request, client);
      return;
    }
    // Over the cap: keep feeding the parser so the message can still complete,
    // but stop growing the buffer. The request is answered with 413 and never
    // routed, so a truncated body is fine.
    if (pctx->body_overflow) return;
    pctx->body_bytes += len;
    if (max_body_size_ != 0 && pctx->body_bytes > max_body_size_) {
      pctx->body_overflow = true;
      pctx->body_buf.clear();  // release what was buffered so far
      return;
    }
    pctx->body_buf.append_data(at, len);
  });
  pctx->parser->set_on_headers_complete([this, pctx, client]() {
    pctx->headers_done = true;

    // 两条**必须在认领钩子之前**判的：(1) RFC 9112 §3.2 的三条 MUST-400
    // （缺 / 重复 / 值非法的 Host）；(2) §6.1 那条"Transfer-Encoding 整行为空"。
    //
    // 排在这个位置的原因是框架层装了 `set_stream_claim`（`uvcpp_web_app.cpp`
    // 里那一处）：请求一旦被它认领，本函数在后面就 `return` 了 —— 放后面等于
    // 只有走低层路由的请求受检，而 webapp 才是绝大多数用法。
    //
    // 此刻头表还在解析器上：`take_headers_into()` 是下面那条建视图的路上才
    // 搬的，所以这里读得到全部头。
    const http_headers& h1_headers = pctx->parser->get_headers();
    if (message_header_violates(h1_headers,
                                pctx->parser->get_uvcpp_http_version())) {
      reject_early(*pctx, client, http_status::BAD_REQUEST,
                   /*message_will_complete=*/true);
      return;
    }

    // Resolve a streaming route now that method + url are known; when matched,
    // deliver HEADERS and hand body chunks directly to the handler.
    auto sh = find_stream_handler(pctx->parser->get_method(),
                                  pctx->parser->get_url());

    // Nothing registered for this exact method+path: give the claim hook a
    // chance. The framework installs one to reach its own router, which has
    // path parameters and wildcards that post_stream() cannot express.
    bool built_view = false;
    if (!sh && stream_claim_) {
      // The hook needs a request view, and the hook decides from it — so unlike
      // the stream-route path we must build it before knowing whether it is
      // used. `stream_view_built` lets on_request_complete reuse this one
      // instead of copying the header vector a second time.
      //
      // 头表是**搬**过来的（`take_headers_into()`），不是拷的：一个字符串都不复制，
      // 而且目的地是**连接上**那块表（`stream_request.headers`，容量跨请求留着）
      // ⇒ 连那次向量分配也一并省掉（改前是每请求一次：目的地是本函数新造的）。
      // 这条路上它紧接着还要被搬进 `uvcpp_web_request`。搬走的后果是**解析器上
      // 这条消息的头空了**，所以下面 `check_expect_header` 与 `message_has_body`
      // 都改成读这份视图（见 `msg_headers`）。
      pctx->stream_request.method  = pctx->parser->get_method();
      pctx->stream_request.url     = pctx->parser->get_url();
      pctx->stream_request.version = pctx->parser->get_uvcpp_http_version();
      pctx->stream_request.keep_alive =
          pctx->parser->should_keep_alive() ? 1 : 0;
      pctx->parser->take_headers_into(pctx->stream_request.headers);
      pctx->stream_view_built = true;
      built_view = true;
      try {
        sh = stream_claim_(pctx->stream_request, client);
      } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "[uvcpp_http_server] stream claim hook threw: %s\n",
                     e.what());
        sh = http_stream_handler();
      } catch (...) {
        std::fprintf(stderr,
                     "[uvcpp_http_server] stream claim hook threw (unknown)\n");
        sh = http_stream_handler();
      }
    }

    if (sh) {
      pctx->stream_handler = sh;
      if (!built_view) {
        pctx->stream_request.method  = pctx->parser->get_method();
        pctx->stream_request.url     = pctx->parser->get_url();
        pctx->stream_request.version = pctx->parser->get_uvcpp_http_version();
        pctx->stream_request.keep_alive =
            pctx->parser->should_keep_alive() ? 1 : 0;
        pctx->parser->take_headers_into(pctx->stream_request.headers);
        pctx->stream_view_built = true;
      }
      sh(http_stream_event::HEADERS, nullptr, 0, pctx->stream_request, client);
      return;  // claimed: the body is no longer ours to police
    }

    // --- Nothing claimed it, so the server buffers the body itself and the
    // --- rules below apply. Order matters: a request we are going to reject
    // --- must not also be told to continue.
    //
    // 这两条判据读的都是**这条消息的头**，而头表在上面两条建视图的路上已经
    // 被 `take_headers_into()` 搬进 `stream_request` 了 —— 搬过就只剩那一份。
    const http_headers& msg_headers = pctx->stream_view_built
                                          ? pctx->stream_request.headers
                                          : pctx->parser->get_headers();

    if (!check_expect_header(*pctx, client, msg_headers)) return;  // 417, answered
    if (reject_oversized_declared(*pctx, client)) return;          // early 413

    // Expect: 100-continue with a body worth waiting for — tell the client it
    // may send. Raw bytes, not send_response(): a 1xx carries no Content-Length
    // and send_response() would add one for the empty body.
    if (pctx->expect_continue && message_has_body(pctx->parser, msg_headers)) {
      enqueue_write(*pctx, client, "HTTP/1.1 100 Continue\r\n\r\n");
    }
  });
  pctx->parser->set_on_message_complete([this, client]() {
    on_request_complete(client);
  });

  // ★ 走**框架层**读（`read_start_events`），不走原始那版 `read_start(cb)`。
  //   原始那版要求回调吃一个 `uvcpp_buf*`，于是 `uvcpp_tcp_client` 每次读都得先
  //   `clone_data()` 一份（见它的原始读路径）—— 而本层要的只是
  //   `(const char*, size_t)` 去喂解析器，那份拷贝与随之而来的每请求一次 malloc
  //   是白做的。`uvcpp_net_read.h` 写明 `data` **只在本次回调期间有效**，正是
  //   本层的用法：解析器就地吃字节，升级请求后面的残留当场 `assign` 走。
  //
  //   语义差异只有一处，且对本层是净收益：原始那版在 `nread < 0` 时**什么都不
  //   回调**（对端断开与读错误都只能靠 close 回调猜），框架版给出
  //   DATA / PEER_CLOSED / READ_ERROR；而收尾（close 回调 → `remove_ctx`）照旧，
  //   本层不需要在回调里做任何释放。
  client->read_start_events([this, client](uvcpp_tcp_client&,
                                           const net_read_result& r) {
    if (r.event == net_read_event::DATA && r.data != nullptr && r.size > 0) {
      on_connection_data(client, r.data, r.size);
    }
  });
  client->set_on_close([this, client]() { remove_ctx(client); });
}

void uvcpp_http_server::on_connection_data(uvcpp_tcp_client* client,
                                            const char* data, size_t len) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return;
  conn_ctx& ctx = it->second;

  // Raw-data hook: fires ahead of parsing, so a hook can observe the bytes
  // (return true) or take the connection over (return false). This is the only
  // place outside the parser that sees a connection's raw bytes.
  if (raw_data_hook_) {
    if (!raw_data_hook_(client, data, len)) {
      return;  // consumed by the hook; the parser never sees this chunk
    }
  }

#if UVCPP_NGHTTP2_ENABLE
  // ---- 明文连接的 h2c 判定（只在 `set_h2c_enabled(true)` 之后）----
  //
  // 判据是 RFC 9113 §3.4 的**先验知识**：头 24 个字节与固定前言逐字节相同就当
  // h2，否则当 h1。**不是嗅探** —— 两边都由确定的字节决定，没有"猜"这一档。
  //
  // 位置：必须在 `parser->execute()` **之前**。那 24 个字节一旦喂给 llhttp，
  // 它就已经被当成一条畸形请求行吃掉了（`PRI` 不是方法），而 h2 会话要的恰恰
  // 是**原样**的这 24 个字节（`nghttp2` 自己认它）。
  //
  // 排在 `raw_data_hook_` **之后**是有意的：那个钩子的契约是"能看见原始字节、
  // 也能把连接整个接管走"，接管了就不该再有人替它决定协议。
  if (h2c_enabled() && ctx.parser != nullptr &&
      ctx.h2c == conn_ctx::h2c_probe_state::PROBING && !client->is_tls()) {
    const size_t have = ctx.h2c_probe.size();
    // `matched` 是**本段里**连着的匹配字节数；`ctx.h2c_probe` 存的是**前面几段**
    // 攒下的。分开数是为了下面"岔了"那条路能一次喂对：那时攒下的字节要按原样
    // 补喂，而本段是从下标 0 起整段走常规路径 —— 本段被比过的那些字节还在里面，
    // 单独再喂一次就是重复。
    size_t matched = 0;
    while (have + matched < kH2cPrefaceLen && matched < len &&
           data[matched] == kH2cPreface[have + matched]) {
      ++matched;
    }

    if (have + matched == kH2cPrefaceLen) {
      // 前言齐了 ⇒ 整条连接转 h2。
      //
      // 交出去的必须是**从第 0 个字节起的整条流**，也就是
      // `h2c_probe`（前几段，前言的 `[0, have)`）拼上**本段整段** `data`：
      //
      //   - 本段里被用来比对的 `matched` 个字节**不能跳过**。那 24 字节前言
      //     不是我们替 nghttp2 剥掉的包装 —— 服务端会话**自己就要认它**
      //     （`nghttp2_session_mem_recv2()` 对不上就返回 `-903`
      //     `NGHTTP2_ERR_BAD_CLIENT_MAGIC`，而且是致命的）。第一版在这里写的是
      //     `data + matched`，服务端于是只发得出去自己的 SETTINGS 就再没动静，
      //     连客户端 SETTINGS 的 ACK 都没有。
      //   - 本段 `matched` 之后的字节是前言后面跟的第一批帧（前言与第一帧挤在
      //     同一个 TCP 段里到达是常态），本来就要一起交出去。
      //
      // 于是"前缀在前、本段整段在后"正好拼成一条从字节 0 开始的完整流，既没跳
      // 也没重。
      ctx.h2c = conn_ctx::h2c_probe_state::H2;
      std::string pending = ctx.h2c_probe;
      ctx.h2c_probe.clear();
      pending.append(data, len);
      // **这一句之后 `ctx` 就悬垂了**：`switch_to_h2c()` 会把本节点摘掉、再让
      // h2 那条路插一份新的。所以它是本函数里最后一句用法，后面只剩一个 `return`。
      switch_to_h2c(client, pending);
      return;
    }

    if (matched == len) {
      // 本段整个都是前言的开头，还没比完：攒着等下一段。**解析器一个字节都没
      // 吃到**（这个 `return` 就是"本段到此为止"）。
      ctx.h2c_probe.append(data, len);
      return;
    }

    // 岔了 ⇒ 定案 h1，此后这条连接上与 h2c 有关的代码一行都不再跑。攒下的那几段
    // 是"看过没吃过"的，按原样补喂给解析器；本段整个落到下面的常规路径上去。
    ctx.h2c = conn_ctx::h2c_probe_state::H1;
    if (have > 0) {
      ctx.parser->execute(ctx.h2c_probe.data(), ctx.h2c_probe.size());
      ctx.h2c_probe.clear();
    }
    // 不 return：本段（`data`, `len`）还要走常规路径。
  }
#endif

  // Keep-alive: 上一条消息收完了就重置解析器，好让这次读里的下一条报文能解析。
  //
  // 判据必须是**解析器**收没收到消息尾（`COMPLETE`），不能是"这个连接上处理完过请求
  // 没有"—— 后者是 `msg_done`（在 `on_request_complete` 里置位），它跟"消息停在哪个
  // 字节"无关。坏就坏在两者错位的那一刻：一次读里前几条消息完成后 `msg_done` 已经是
  // 真，而解析器停在**下一条的中间**，于是下个读开头照着 `msg_done` 把半条报文
  // `llhttp_init` 掉 —— 断在头部中间则残尾被当成请求行（400 ＋ 关连接）；断在报文末尾
  // 那个空行上更安静：`llhttp_init` 落到 llhttp 的 `n_start`，那个状态把 `\r`/`\n` 当
  // 空行吞掉，半条报文被抹掉而那条请求不报错、不回、也不进任何表。
  //
  // `msg_done` 本身由 message-begin 那个钩子清（与单消息暂存共用一个清账点）；
  // 这里再清一次只是为了"reset 过就一定干净"，免得哪天 `execute()` 一个字节都
  // 没吃（零长读）时钩子不响，`msg_done` 卡在 true 挡住收尾路径（`:1070`）。
  if (ctx.msg_done && ctx.parser->get_state() == http_parser_state::COMPLETE) {
    ctx.parser->reset();
    ctx.msg_done = false;
  }

  const size_t used = ctx.parser->execute(data, len);

  // 升级请求后面跟着的字节（升级请求和第一帧挤在同一个 TCP 段里到达，真实
  // 客户端几乎总是这样）**不能丢**：它们已经被这次读取走了，重新 read_start
  // 是读不回来的。存下来由升级方补投给新协议
  // （`uvcpp_ws_connection::feed_pending`）。
  //
  // `used` 在这里是可信的：升级请求走的是 `HPE_PAUSED_UPGRADE` 分支，那个
  // 分支按 `llhttp_get_error_pos` 算停下位置（实测"升级请求 156 + 首帧 15"
  // 的 171 字节批次报的就是 156）。**不要**把它推广成通用的"已消费字节数"：
  // `HPE_OK` 分支一律 `return len`（那是"消息吃完了"，不是"字节吃完了"），
  // 流水线的下一条请求就落在那条路径上 —— 与本条无关，不在这里动。
  //
  // 只在升级中才存；不是升级连接的话 `pending` 永远是空的。
  if (ctx.upgrading && used < len) {
    ctx.pending.assign(data + used, len - used);
  }

  // **必须排在 `has_error()` 前面**：撞上限同样会让解析器进 PARSE_ERROR
  // （回调返回非 0 就是这个效果），照 `has_error()` 那条路走会一律回 400，
  // 把"头太大"和"请求畸形"混成一件事，客户端也就无从知道自己该缩哪一样。
  const uvcpp_http_parser::size_limit hit = ctx.parser->limit_hit();
  if (hit != uvcpp_http_parser::size_limit::NONE) {
    // 消息半途被丢下了，永远不会有 `on_request_complete` —— 关闭只能搭在
    // 这次写上（`reject_early` 的默认延迟关闭在这里会等一个不来的事件，连接
    // 就一直挂着：**本层没有闲置清扫**，会来收的是 webapp 层那个，默认 60 s）。
    reject_early(ctx, client,
                 hit == uvcpp_http_parser::size_limit::URL
                     ? http_status::URI_TOO_LONG
                     : http_status::REQUEST_HEADER_FIELDS_TOO_LARGE,
                 /*message_will_complete=*/false);
    return;
  }

  if (ctx.parser->has_error()) {
    // A malformed request must not be answered by hand-rolled bytes: route it
    // through the normal response path so keep-alive/close handling, HEAD
    // suppression and write serialization all apply. `Connection: close` is
    // forced because the parser's state is no longer trustworthy.
    uvcpp_http_response resp = uvcpp_http_response::make(
        http_status::BAD_REQUEST, "Bad Request", 11);
    resp.set_header("connection", "close");
    send_response(client, resp);
  }
}

void uvcpp_http_server::on_request_complete(uvcpp_tcp_client* client) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return;
  conn_ctx& ctx = it->second;
  ctx.msg_done = true;

  // Already answered during the headers callback (417 unknown Expect, 413 by
  // declared Content-Length). Everything below would answer a second time.
  // The close was deferred to here on purpose: the client has now stopped
  // sending, so closing no longer risks an RST that would destroy the response
  // sitting in its receive buffer.
  if (ctx.rejected) {
    if (ctx.close_after_message) {
      ctx.close_requested = true;
      // 这一句与上面那次 `find` 之间没有用户代码，直接用 `ctx`。
      pump_write(ctx, client);  // closes now, or after the pending write completes
    }
    return;
  }

  // Streaming route: deliver END; the handler responds asynchronously.
  if (ctx.stream_handler) {
    ctx.stream_handler(http_stream_event::END, nullptr, 0, ctx.stream_request, client);
    ctx.stream_handler = nullptr;
    // 同上：只清内容。这一块表下一条消息还要接着当搬移目的地。
    reset_request_keep_buffers(ctx.stream_request);

    // The handler answered from a BODY callback and asked for the connection to
    // close. That close was parked by pump_write() because the peer was still
    // sending; it is safe now.
    if (ctx.defer_close_to_message_end) {
      ctx.defer_close_to_message_end = false;
      ctx.close_requested = true;
      pump_write(client);
    }
    return;
  }

  // Body over the cap: the buffer was truncated, so there is nothing sane to
  // route. Reject and close.
  if (ctx.body_overflow) {
    // 长度**数一遍再写**：这里从前写的是 23，而 "413 Payload Too Large" 只有 21
    // 个字符 —— 每走一次这条 413 分支都会从这个字面量后面多读 2 个字节
    // （ASan：`global-buffer-overflow ... READ of size 23`）。越过去的那 2 个字节
    // 落在紧挨着的下一个字面量上（rodata 里字面量是挨着排的），所以读了几年也
    // 没人发现：不越页、不越段，只有 ASan 的全局变量影子内存看得见。
    // 写成 `sizeof(字面量) - 1` 让编译器去数，字面量改了长度跟着改。
    uvcpp_http_response resp = uvcpp_http_response::make(
        http_status::PAYLOAD_TOO_LARGE, "413 Payload Too Large",
        sizeof("413 Payload Too Large") - 1);
    resp.set_header("connection", "close");
    ctx.body_buf.clear();
    send_response(client, resp);
    return;
  }

  // 交给处理函数的是**连接上**那一个请求对象（`ctx.request` / `ctx.stream_request`），
  // 不再是每请求一个局部。理由只有一个、但很硬：头表的**搬移目的地**必须活得比
  // 请求长，那块 vector 的容量才能跨请求留下（`take_headers_into()` 里的 `clear()`
  // 只改 size、不还容量）。
  //
  // 改前这里是 `req = std::move(ctx.stream_request)` —— 视图建好后再整份搬一次：
  // 那一次搬不但多余，还把连接上那块缓冲的容量一起搬进了局部对象、随函数返回
  // 一起没了。
  //
  // 对处理函数来说形状没变（还是一个 `uvcpp_http_request&`，方法/URL/头/体就地
  // 取用），变的是它的**寿命**：改前那个局部对象在 `on_request_complete` 返回时
  // 就死了（再读是悬垂），现在它活到下一条请求在原地重填（再读**读到的是下一条
  // 请求**）。「处理函数返回之后不得再读它」这条约束两版一样，只是违反后的症状
  // 从崩变成静默读错 —— 所以这里点名写出来。
  uvcpp_http_request& req =
      ctx.stream_view_built ? ctx.stream_request : ctx.request;
  if (!ctx.stream_view_built) {
    req.method    = ctx.parser->get_method();
    req.url       = ctx.parser->get_url();
    req.version   = ctx.parser->get_uvcpp_http_version();
    req.stream_id = 0;  // h1 路径恒为 0（h2 走 `dispatch_h2_request`，不经过这里）
    // keep-alive 的结论解析器**已经算好了**（`extract_metadata()` 里那句
    // `llhttp_should_keep_alive()`）。带下去，web 层就不用每请求再扫一遍头表：
    // 那边 `is_keep_alive()` 的懒计算正是"没拿到就自己按 Connection 算"。
    req.keep_alive = ctx.parser->should_keep_alive() ? 1 : 0;
    // 与上面那两条建视图的路**同一个形状**：头表是**搬**过来的，不是拷的。
    // 拷一份 = 一次向量分配**外加每个非 SSO 的值各一次**（libstdc++ 的界是
    // 15 字节，`Host: 127.0.0.1:8081` 这种就超了）；搬 = 一个字符串都不分配
    // —— 契约在 `uvcpp_http_parser.h` 的 `take_headers_into()`。
    //
    // 与改前只差一句：**目的地是连接上这块**（`ctx.request.headers`，容量跨请求
    // 留着），不再是每请求新造的那个空 vector —— 那一次 `_M_allocate` 正是本笔要
    // 省掉的（站点普查里 `take_headers` 那一笔 1.00 次/请求）。
    //
    // 搬走的后果是**解析器上这条消息的头空了**。这一条在本函数里已经吃透过：
    // 下面读 `accept-encoding` 与 `is_head` 用的都是 `req`，不是解析器；而延迟
    // 应答那条路更不可能回头读解析器 —— 它发响应时解析器早被复位了，所以那两样
    // 必须在下面缓存（见紧随其后的注释）。整条消息对解析器头表的读，到这一行为止。
    ctx.parser->take_headers_into(req.headers);
  }
  ctx.stream_view_built = false;
  req.body.clone(ctx.body_buf);

  // Capture what send_response() needs later: for a deferred response the
  // parser is reset (and its headers replaced) before the response is sent,
  // so these cannot be read back off the parser at send time.
  http_get_header_into(req.headers, "accept-encoding", ctx.accept_encoding);
  ctx.is_head = (req.method == http_method::HTTP_HEAD);

  // 「稍后回话」凭据（见 `uvcpp_http_reply`）：h1 上它背后只有这条 TCP 连接，
  // 流号恒为 0 —— 与 h2 那条腿同一个形状，处理函数因此不必分辨自己在哪条腿上。
  fill_reply(req.reply, client, ctx, 0);

  uvcpp_http_response resp;

  // 头表的缓冲按**连接**留下来：认领必须在处理函数跑之前（下面那句
  // `handler(req, resp, client)` 会往表里塞头，认领晚了那些头就落在一块
  // 马上要被丢掉的小缓冲上）。清空由 `adopt_tables()` 自己负责 —— 处理函数
  // 拿到的一定是一张干净的表，看不见上一条请求的头。
  resp.adopt_tables(ctx.resp_hdr_recycle);
  ctx.resp_hdr_owner = &resp;

  // ★ 领了就得**有且只有一个**配对，而配对点必须是"这条响应真的发完了"那一刻。
  //   本函数有三条出口（升级 / deferred / 正常 `send_response()`），后两条才到
  //   得了正常归还点，所以拿一个 RAII 兜底 —— 靠人工在三处各补一句 `yield` 是
  //   会漏的，而**漏了的症状只是"下次从 0 长"**（静默，只有读数看得见）。
  //
  //   为什么认的是"领走缓冲的那个对象"而不是"本函数的局部对象"：见
  //   `conn_ctx::resp_hdr_owner` 的注释 —— `send_response()` 收到的引用可能是
  //   webapp 手上那个对象，无条件还就会把它的表换走、塞给它一块空的。
  struct resp_hdr_guard {
    conn_ctx& c;
    uvcpp_http_response& r;
    ~resp_hdr_guard() {
      if (c.resp_hdr_owner == &r) {
        r.yield_tables(c.resp_hdr_recycle);
        c.resp_hdr_owner = nullptr;
      }
    }
  } resp_hdr_guard_inst{ctx, resp};

  // Check for WebSocket upgrade BEFORE routing
  if (upgrade_handler_) {
    std::string up = http_get_header(req.headers, "upgrade");
    if (!up.empty()) {
      bool is_ws = (up.size() == 9);
      for (size_t i = 0; is_ws && i < 9; i++)
        if (http_lower_ascii(static_cast<unsigned char>(up[i])) !=
            static_cast<unsigned char>("websocket"[i]))
          is_ws = false;
      if (is_ws) {
        // 升级之后，**同一次读**里剩下的字节属于新协议（WS 帧），不属于 HTTP。
        // 记下"这条连接正在升级"，等 `execute()` 返回到手了再把这批字节存起来
        // —— 升级回调是在 `execute()` **里面**同步调的，此刻还不知道剩下多少。
        ctx.upgrading = true;
        upgrade_handler_(req, client);
        return;
      }
    }
  }

  auto handler = find_handler(req.method, req.url);
  if (handler) {
    handler(req, resp, client);
  } else {
    resp = uvcpp_http_response::not_found();
  }

  // Compression is applied inside send_response(), NOT here: a deferred
  // response returns below and is sent later, so compressing at this point
  // would silently skip every async handler's response.

  // Deferred response: handler set deferred; it calls send_response later.
  if (resp.deferred) return;

  send_response(client, resp);
}

bool uvcpp_http_server::check_expect_header(conn_ctx& ctx,
                                            uvcpp_tcp_client* client,
                                            const http_headers& msg_headers) {
  // Expect is an HTTP/1.1 mechanism (RFC 7231 §5.1.1): in a 1.0 message the
  // header has no defined meaning, so it is ignored rather than failed. That
  // matters because clients from that era send things like "Expect: 100-continue"
  // through proxies that do not understand it.
  if (ctx.parser->get_uvcpp_http_version() != uvcpp_http_version::HVER_11)
    return true;

  std::string expect = http_get_header(msg_headers, "expect");
  if (expect.empty()) return true;

  if (http_name_equal(expect, "100-continue")) {
    ctx.expect_continue = true;
    return true;
  }

  // RFC 7231 §5.1.1: an expectation the server cannot meet must be answered
  // 417 rather than silently ignored — the client is waiting for a signal that
  // a naive implementation would never send, and would hang until its own
  // timeout.
  reject_early(ctx, client, http_status::EXPECTATION_FAILED);
  return false;
}

bool uvcpp_http_server::reject_oversized_declared(conn_ctx& ctx,
                                                  uvcpp_tcp_client* client) {
  if (max_body_size_ == 0) return false;
  const uint64_t declared = ctx.parser->get_content_length();
  if (declared == 0 || declared <= max_body_size_) return false;

  // Refuse on the declared length, before the body arrives. Waiting for the
  // message to complete (the old path) means buffering up to the cap and then
  // reading the rest to nowhere — a client streaming 10 GB at a 1 MB limit is
  // answered immediately instead of after it has sent 10 GB.
  reject_early(ctx, client, http_status::PAYLOAD_TOO_LARGE);
  return true;
}

void uvcpp_http_server::reject_early(conn_ctx& ctx, uvcpp_tcp_client* client,
                                     http_status status,
                                     bool message_will_complete) {
  const char* reason = http_status_reason(status);
  const std::string body =
      std::to_string(static_cast<int>(status)) + " " + reason;

  uvcpp_http_response resp =
      uvcpp_http_response::make(status, body.c_str(), body.size());
  resp.set_header("connection", "close");

  ctx.rejected = true;
  // The body is going to arrive no matter what; don't accumulate a byte of it.
  ctx.body_overflow = true;
  ctx.body_buf.clear();

  if (message_will_complete) {
    // Not now. The client is still sending the body, and closing a socket with
    // unread data in its receive buffer makes Windows send an RST — which throws
    // away bytes the peer has buffered but not yet read, including this response.
    // on_request_complete closes once the message has actually ended.
    ctx.close_after_message = true;
  }

  // 消息半途被丢下时反过来：等 `on_request_complete` 就是永远不等。
  // 队列写完由 `pump_write()` 收尾 —— 它认得这个形状（没有 stream handler
  // 且消息没完成 ⇒ 当场关），所以这里只要让 `close_requested` 立起来。
  send_response(client, resp, /*close_after_write=*/!message_will_complete);
}

/**
 * @brief 这个状态码的响应**不许**自动补 `content-length`。
 *
 * RFC 9110 §8.6：1xx 与 204 一律不许带；304 只允许回显原始长度，而本层没有
 * 原始长度可回显。空体响应那一支补 `content-length: 0` 时先把这几种放行 ——
 * `uvcpp_web_response::sync_meta()` 早就是这条规矩，但那条只影响它自己算出来
 * 的头；这里补的是 HTTP 层最后那一下。
 */
static bool status_forbids_content_length(http_status s) {
  const int c = static_cast<int>(s);
  return (c >= 100 && c < 200) || c == 204 || c == 304;
}

size_t uvcpp_http_server::send_response(uvcpp_tcp_client* client,
                                        uvcpp_http_response& resp,
                                        bool close_after_write) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) {
    // The connection is already gone. Callers reach this by responding after
    // the client disconnected, or twice for one request — a real bug worth
    // hearing about rather than a silently vanished response.
    std::fprintf(stderr,
                 "[uvcpp_http_server] Warning: response dropped, connection "
                 "is no longer tracked (already closed?)\n");
    return 0;
  }
  conn_ctx& ctx = it->second;

  // `Date` 是源服务器**必须**发的（RFC 9110 §6.6.1：有可靠的钟就必须发），
  // 而这里是 h1 整包响应与"延迟应答回到这里"那一支的唯一收口。
  // 必须补在 h2 分支**之前** —— 延迟应答也从下面那一句拐走。
  http_ensure_date(resp);

#if UVCPP_NGHTTP2_ENABLE
  // h2 连接的延迟应答也回到这里 —— 发起它的处理函数手里只有 client 和 resp，
  // 没有流 id，所以流 id 是**随 resp 一起传下来**的（见 resp.stream_id）。
  if (ctx.h2 != nullptr) {
    send_h2_response(client, resp.stream_id, resp);
    // h2 那条路上体一个字节都没动（帧由 h2 会话自己切），所以这里如实报长度。
    return resp.body.size();
  }
#endif

  // Server -> client compression. Done here so deferred responses are covered.
  apply_compression(ctx, resp);

  // ★ 一次扫表拿齐四件事：`connection` 在不在、它的值是不是 `close`、
  //   有没有 `content-length` / `transfer-encoding`。改前这里是四次
  //   `has_header()` 加一次 `get_header()`（后者还造一个 `std::string` 再丢掉），
  //   每次都是从表头扫到表尾 —— 响应头常态就五六个，但这五次扫描在 callgrind
  //   上是每请求 412 条，而它们换来的信息量就是这四个布尔。
  //
  //   长度先分流（10/14/17）再逐字节小写比较：名字长度不等的头连比较都省了，
  //   `date`(4)/`server`(6)/`content-type`(12) 这些一趟就跳过去。
  //   `connection` 与 `content-length` 可能有重复项（`set_header` 会覆盖，
  //   但处理函数直接 `headers.push_back` 就能塞第二份）—— 取**第一个**匹配项，
  //   与 `http_get_header()` / `http_has_header()` 的语义一致。
  bool has_conn = false;
  bool conn_close = false;
  bool has_content_length = false;
  bool has_transfer_encoding = false;
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    const http_header& h = resp.headers[i];
    const size_t n = h.name.size();
    if (n == 10) {
      if (!has_conn && http_name_iequal(h.name.data(), n, "connection", 10)) {
        has_conn = true;
        // 字段值不要求保留大小写，所以按名字比较那条路（大小写不敏感）比。
        conn_close = http_name_equal(h.value, "close");
      }
    } else if (n == 14) {
      if (!has_content_length &&
          http_name_iequal(h.name.data(), n, "content-length", 14)) {
        has_content_length = true;
      }
    } else if (n == 17) {
      if (!has_transfer_encoding &&
          http_name_iequal(h.name.data(), n, "transfer-encoding", 17)) {
        has_transfer_encoding = true;
      }
    }
  }

  // Keep-Alive (RFC 7230 Section 6.3):
  // HTTP/1.1 defaults to keep-alive unless Connection: close
  // HTTP/1.0 defaults to close unless Connection: keep-alive
  bool keep_alive;
  if (has_conn) {
    // Respect an explicit header set by the handler (e.g. streaming endpoints
    // force close).
    keep_alive = !conn_close;
  } else {
    // Only consult the parser when the response does not decide for itself:
    // after a parse error its keep-alive state is not meaningful.
    keep_alive = ctx.parser->should_keep_alive();
    resp.set_header("connection", keep_alive ? "keep-alive" : "close");
  }

  if (ctx.is_head) {
    // A HEAD response carries the headers a GET would, including its
    // Content-Length, but no body. Pin Content-Length before dropping the
    // body, otherwise it would be computed as 0.
    if (!has_content_length && resp.body.size() > 0) {
      resp.set_header("content-length",
                      std::to_string(static_cast<unsigned long long>(
                          resp.body.size())));
    }
    resp.body.clear();
  } else if (!has_content_length && !has_transfer_encoding &&
             resp.body.size() == 0 &&
             !status_forbids_content_length(resp.status_code)) {
    // An empty body still needs an explicit Content-Length: 0, otherwise a
    // keep-alive client cannot tell where this response ends.
    //
    // **1xx / 204 / 304 例外**（RFC 9110 §8.6）：前两种根本不许带
    // `content-length`，304 只允许回显原始长度（这里没有原始长度可回显）。
    // 补一个 `content-length: 0` 就是一条协议违例 —— 而它以前就在这里，
    // 因为 `to_string()` 本身不加，单测直接看序列化结果时看不出来。
    resp.set_header("content-length", "0");
  }

  // 体是否**单独**作为第二块写出去（#17 的 `nbufs = 2`）。
  //
  // 判据是一条**逐字节相同**的等价关系：`to_string(false)` 接上 `resp.body`
  // 必须与 `to_string(true)` 一字不差。什么时候成立，看
  // `uvcpp_http_response::to_string` 就清楚 —— 非 chunked（chunked 会补十六进制
  // 帧与终止块，体不是裸字节接在头部后面）且体非空。
  //
  // 这里用 `has_header("transfer-encoding")` 而不是逐字照搬那边"扫到 chunked"
  // 那句：本库只发 chunked 这一种 transfer-encoding，而这个判据只可能**更保守**
  // （判错时退回合并那条路，多一次拷贝），不会悄悄走成"第二块不是那条报文的后半
  // 段"。方向选对了，两边就不会因为将来加一种编码而分叉。
  const bool two_bufs = !ctx.is_head && resp.body.size() > 0 &&
                        !has_transfer_encoding;

  // HEAD 走「只序列化头部」那一支，**必须显式传 false**：清空 body 并不足以
  // 让报文正确 —— 一个声明了 `transfer-encoding: chunked` 的 HEAD 响应在
  // `to_string()` 里会照样补出终止块 `0\r\n\r\n`（那是 chunked 的**帧**，
  // 与 body 是否为空无关），于是 HEAD 凭空多出一段 body。这条与
  // `uvcpp_http_response.cpp` 里「空 body 也要发终止块」是同一个改动的两面：
  // 只改那边不改这里，就是修完一个缺陷立刻制造一个新缺陷。
  //
  // 两块那条路上序列化的正是同一个头部块（`to_string(false)` 返回的就是
  // `to_string(true)` 去掉末尾体之后的部分），所以 `two_bufs` 与 HEAD 这一条
  // 不冲突：HEAD 永远走"只序列化头部"。
  resp.to_string_into(ctx.wire_recycle, /*include_body=*/!ctx.is_head && !two_bufs);

  // close_after_write=false lets a streaming handler keep the connection open
  // to write the body after the header; it must close the connection itself.
  if (!keep_alive && close_after_write) ctx.close_requested = true;

  // **体的长度必须在这里取走。** 两块那条路会把 `resp.body` 移动出去（移动的
  // 是句柄不是字节），之后 `resp.body.size()` 是 0 —— 而"这次上线了多少 body"
  // 正是调用方（`uvcpp_web_app::send_response` 的访问日志）要记的那个数。以前
  // 它是**在这之后**回头读 `resp.body` 拿到的，那条耦合到这里断了，所以改成
  // 由本函数返回。返回值就是取代那次回头读的东西。
  const size_t body_bytes = resp.body.size();

  if (two_bufs) {
    enqueue_write(ctx, client, ctx.wire_recycle, std::move(resp.body));
  } else {
    enqueue_write(ctx, client, ctx.wire_recycle);
  }

  // 归还要放在**最后**：头表一直要用到上面那句 `to_string_into()`（报文就是
  // 从表里拼出来的），而 `enqueue_write` 拿走的是序列化后的字节与 `resp.body`，
  // 不再碰 `headers`。放在这里之后，这一段里再新增"读头"的代码都得往回挪。
  //
  // ★ 一次点名：交还之后这个响应对象的头表就空了 ⇒ 对**同一个对象**再调一次
  //   `send_response()`，第二次发的是没有头的报文（改前是第二份完整报文）。
  //   一个请求发两份响应在 HTTP/1.1 上本来就是帧序错乱，而且本函数自己就会
  //   `set_header`，两次调用从来不幂等 —— 所以这是一条如实写出来的行为变化，
  //   不是"无变化"。
  // ★ 只有**领走那块缓冲的那个对象**才还 —— 见 `conn_ctx::resp_hdr_owner`。
  if (ctx.resp_hdr_owner == &resp) {
    resp.yield_tables(ctx.resp_hdr_recycle);
    ctx.resp_hdr_owner = nullptr;
  }
  return body_bytes;
}

void uvcpp_http_server::begin_stream(uvcpp_tcp_client* client,
                                     uvcpp_http_response& resp) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] Warning: stream dropped, connection is "
                 "no longer tracked (already closed?)\n");
    return;
  }
  conn_ctx& ctx = it->second;

#if UVCPP_NGHTTP2_ENABLE
  // h2 上这条入口不该被走到：调用方手上没有"这条连接唯一的那条流"这个前提，
  // 必须走带 `stream_id` 的重载。真走到了说明调用方还在按 h1 的形状调，此时
  // 下面第一句就会解引用 `ctx.parser`（h2 连接上恒为 nullptr）。
  //
  // 用 stderr 而不是静默返回：调用方在 h1 上走得好好的，切到 h2 忽然少一条
  // 响应、还没有任何提示，是最难查的一类。
  if (ctx.h2 != nullptr) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] begin_stream(client, resp) 在 HTTP/2 连接"
                 "上不能用（分不清是哪条流），请改用带 stream_id 的重载\n");
    return;
  }
#endif

  // keep-alive 的判定与 `send_response` 同一套（RFC 7230 §6.3）：handler 自己
  // 设了 `connection` 就听它的，否则看这条请求的解析结果。这里**不**决定
  // 关不关 —— 那由 end_stream(close_after) 定，否则头部写完那一刻队列是空的，
  // 非 keep-alive 的流会在第一块 body 之前就被关掉。
  bool keep_alive;
  if (resp.has_header("connection")) {
    keep_alive = !http_name_equal(resp.get_header("connection"), "close");
  } else {
    keep_alive = ctx.parser->should_keep_alive();
    resp.set_header("connection", keep_alive ? "keep-alive" : "close");
  }

  // **不**走 apply_compression：body 还不存在（见头文件里的说明）。
  //
  // **不**补 `content-length: 0`：长度语义由调用方选（已知长度就设
  // content-length，未知就设 transfer-encoding: chunked），本层不猜。

  // HEAD 走的就是"只序列化头部"那一支：头部与 GET 逐字节相同，body 一个
  // 字节都不发。`out_streaming` 仍然置位，好让 write_stream 的调用方拿到
  // 一致的语义（HEAD 上框架层会把 write_chunk 变成空操作，见 uvcpp_web_response）。
  // 流式响应的头部不走 `send_response`（它一次性拼整条报文，而流式的 body
  // 此刻还不存在），`Date` 在这条路上得单独补 —— 且必须在 `to_string()`
  // **之前**：序列化就发生在那一句里。
  http_ensure_date(resp);

  ctx.out_streaming = true;

  enqueue_write(ctx, client, resp.to_string(/*include_body=*/false));
}

void uvcpp_http_server::begin_stream(uvcpp_tcp_client* client, int32_t stream_id,
                                     uvcpp_http_response& resp) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] Warning: stream dropped, connection is "
                 "no longer tracked (already closed?)\n");
    return;
  }
  conn_ctx& ctx = it->second;

#if UVCPP_NGHTTP2_ENABLE
  // h2 上没有"连接级流式"这回事，也没有 chunked 组帧 —— 头部折成 HEADERS、
  // body 一块块折成 DATA，都由会话层做。长度靠 END_STREAM 划界，所以这里
  // **不**像 h1 那样要求调用方设 `transfer-encoding`。
  if (ctx.h2 != nullptr) {
    if (stream_id == 0) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] begin_stream() 在 HTTP/2 连接上收到 "
                   "stream_id=0 —— 分不清是哪条流，本响应已丢弃\n");
      return;
    }
    resp.stream_id = stream_id;
    // h2 的流式头部走 `send_headers()`，与整包那条（`send_h2_response`）是
    // 两个不同的出口 —— 这里也得补。
    http_ensure_date(resp);
    ctx.h2->send_headers(stream_id, resp);
    return;
  }
#endif  // UVCPP_NGHTTP2_ENABLE

  // 走到这里说明这是一条 h1 连接，而调用方给了一个非 0 的流号 —— 那是 h2 的
  // 身份，在 h1 上没有意义。按 h1 继续会把 `stream_id` 静默丢掉，响应看起来
  // 正常发出、其实挂在了错误的身份上，所以宁可在这里说清楚。
  if (stream_id != 0) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] begin_stream() 收到 stream_id=%d，但这条"
                 "连接是 HTTP/1.1（h2 未启用或 ALPN 没协商出 h2）\n",
                 static_cast<int>(stream_id));
    return;
  }

  begin_stream(client, resp);
}

int uvcpp_http_server::write_stream(uvcpp_tcp_client* client, std::string bytes,
                                    std::function<void(int)> done) {
  return write_stream(client, 0, std::move(bytes), std::move(done));
}

int uvcpp_http_server::write_stream(uvcpp_tcp_client* client, int32_t stream_id,
                                    std::string bytes,
                                    std::function<void(int)> done) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end() || it->second.closing) return UV_ECANCELED;
  conn_ctx& ctx = it->second;

#if UVCPP_NGHTTP2_ENABLE
  if (ctx.h2 != nullptr) {
    if (stream_id == 0) return UV_EINVAL;  // h2 上没有"这条连接那条流"这回事
    // 会话层收下了就一定会回调 `done`（正常上线是 0，流中途没了是
    // UV_ECANCELED），所以这里返回 0 是**有保证**的那个 0 —— 与 h1 分支
    // 返回值那行契约逐条对齐。
    const int rv = ctx.h2->send_data(stream_id, bytes.data(), bytes.size(),
                                     /*end_stream=*/false, std::move(done));
    if (rv != 0) return UV_ECANCELED;  // 未受理，`done` 不会被调
    return 0;
  }
#endif  // UVCPP_NGHTTP2_ENABLE

  enqueue_write(ctx, client, bytes, std::move(done));
  return 0;
}

void uvcpp_http_server::end_stream(uvcpp_tcp_client* client, bool close_after) {
  end_stream(client, 0, close_after);
}

void uvcpp_http_server::end_stream(uvcpp_tcp_client* client, int32_t stream_id,
                                   bool close_after) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return;
  conn_ctx& ctx = it->second;
  if (ctx.closing) return;

#if UVCPP_NGHTTP2_ENABLE
  if (ctx.h2 != nullptr) {
    if (stream_id == 0) return;
    // 空块 + END_STREAM：h2 里"发完了"就是一个零长度的 DATA 帧带 END_STREAM
    // （没有 h1 那种终止块）。`close_after` 在 h2 上**不关连接** —— 见头文件。
    ctx.h2->send_data(stream_id, nullptr, 0, /*end_stream=*/true,
                      [](int) {});
    return;
  }
#endif  // UVCPP_NGHTTP2_ENABLE

  ctx.out_streaming = false;
  if (close_after) ctx.close_requested = true;

  // 队列可能已经空了（最后一块写完了），所以必须主动泵一次：pump_write 会在
  // 空队列 + close_requested 两条同时成立时收尾。
  pump_write(ctx, client);
}

int uvcpp_http_server::send_trailers(uvcpp_tcp_client* client, int32_t stream_id,
                                     const http_headers& trailers) {
  if (client == nullptr) return UV_ECANCELED;
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  // 与 `write_stream()` 同一套口径：连接不在表里或正在关，都当"这条流已经没了"。
  if (it == tbl.end() || it->second.closing) return UV_ECANCELED;
  conn_ctx& ctx = it->second;

#if UVCPP_NGHTTP2_ENABLE
  if (ctx.h2 != nullptr) {
    if (stream_id == 0) return UV_EINVAL;  // h2 上没有"这条连接那条流"这回事
    return ctx.h2->send_trailers(stream_id, trailers);
  }
#endif  // UVCPP_NGHTTP2_ENABLE

  // ---- h1：chunked 的末块带字段（RFC 9112 §7.1.2）----------------------
  //
  // 只对**正在流式响应**的连接有效（`begin_stream()` 已经发过带
  // `transfer-encoding: chunked` 的头部）：尾随头在 h1 上没有别的落点 ——
  // 它在报文结构里就是终止块之后那段字段，头部那一块此刻早已上线。
  if (stream_id != 0) return UV_EINVAL;  // h1 上只有"这条连接唯一那条流"
  // HEAD 的响应没有 body，也就没有"末块"可挂（RFC 9112 §7.1.2）。`begin_stream`
  // 在 HEAD 上同样会置 `out_streaming`（好让 write_stream 的调用方拿到一致的
  // 语义），所以这一条得单独挡。
  if (ctx.is_head) return UV_EINVAL;
  if (!ctx.out_streaming) return UV_EINVAL;

  // 字段名必须是 token、值里不许有 CR/LF —— 否则就是响应拆分注入。
  std::string out;
  out.append("0\r\n");
  for (size_t i = 0; i < trailers.size(); ++i) {
    const std::string& n = trailers[i].name;
    const std::string& v = trailers[i].value;
    if (n.empty()) return UV_EINVAL;
    for (size_t k = 0; k < n.size(); ++k) {
      const unsigned char c = static_cast<unsigned char>(n[k]);
      if (c <= 0x20 || c >= 0x7f || c == ':') return UV_EINVAL;
    }
    for (size_t k = 0; k < v.size(); ++k) {
      const unsigned char c = static_cast<unsigned char>(v[k]);
      if (c == '\r' || c == '\n') return UV_EINVAL;
    }
    out.append(n);
    out.append(": ");
    out.append(v);
    out.append("\r\n");
  }
  out.append("\r\n");
  // 这一块就是整条流的收尾：与 `end_stream()` 等价（不再发单独的终止块）。
  ctx.out_streaming = false;
  enqueue_write(ctx, client, out);
  return 0;
}

// 压缩变体表的总字节上限。#11 第 3 条量出来的是"重复 deflate"这一项，
// 而 deflate 的产物通常比原文小，所以按**压缩后**字节记账真正占的内存只会更少。
// 这一整块（变体缓存的上下界、淘汰、记账）与 `apply_compression_for` 收在
// `UVCPP_HTTP_COMPRESS_ENABLE` 下 —— **不是** `UVCPP_ZLIB_ENABLE`：缓存存的是
// "某个编码 + 某个档位 + 某个 tag 压出来的字节"，而编码现在有两个来源。
#if UVCPP_HTTP_COMPRESS_ENABLE
static UVCPP_CONSTEXPR size_t kCompressVariantMaxBytes = 32u * 1024u * 1024u;

// 单条上限。没有它，一个大文件（静态路径可以到 MiB 级）的变体进来就能把整张表
// 冲干净 —— 于是"缓存"在最需要它的大文件上反而永远命中不了，典型的最坏组合。
static UVCPP_CONSTEXPR size_t kCompressVariantMaxEntry = 4u * 1024u * 1024u;

// 条数上限。和上面那条**不是一回事**：字节那条只按 body 字节算，而每条另外还挂着
// 一个键串（含着整条路径）和一个 map 节点 —— 一个满是**小文件**的静态根能造出大量
// "压完只剩几十字节"的条目，字节数永远涨不到上限、开销却全在键和节点上。两个上限
// 各封一件事：字节封大文件，条数封碎片。
static UVCPP_CONSTEXPR size_t kCompressVariantMaxEntries = 1024;

// 档位在键里占**一个字符**：`-1`（`k_compress_level_default`，即
// `Z_DEFAULT_COMPRESSION`）→ `'#'`，0..9 → `'0'+n`。
//
// 定长是有意的。变长的十进制串会与 `cache_tag` 粘成同一个键：档位 1 配 tag
// "2x" 与档位 12 配 tag "x" 拼出来一模一样 —— 而"12 档不存在"只是**今天**的
// 巧合（`clamp_compress_level` 的上限是 9），不是不变量。定长前缀使
// `(编码, 档位, tag) -> 键` 是单射，与编码字符同一个理由（见 `.h` 里那段）。
//
// 单射还依赖"拿到的档位已经夹紧"：`set_compress_level()` 存的就是夹过的值，
// 而 `-1` 与 `0..9` 正好 11 个取值 → 11 个不同字符。未夹紧的 15 会落到 `'?'`，
// 不会与 5 撞，但那是运气不是设计 —— 夹紧在入口那一次做完。
static char compress_level_key_char(int level) {
  return level < 0 ? '#' : static_cast<char>('0' + level);
}

// 编码字符。**必须与 `http_pick_precompressed_variants()`（`uvcpp_http_common.h`）
// 的优先级同序**：那边 br > gzip > deflate 是"盘上有孪生文件时挑哪个"的次序，
// 这边是"现场编出来的是哪个"的次序 —— 同一份请求里只有一个编码会被选中，但
// 两处一旦不同序，同一次协商在两条路上会得到不同的答案（一条发 `.br`、一条
// 现编 gzip），线上就是**同一份资源两种字节**。
//
// 字符本身只要求"三个编码互不相同、都不是 `'0'..'9'` 或 `'#'`"（否则会与紧随
// 其后的档位字符粘成同一个键）：`'b'`/`'g'`/`'d'` 满足，且与文件后缀同名，
// 排查报文时一眼能对上是哪一支。
//
// 兜底走 `'d'`：`best` 到不了这里 —— `parse_accept_encoding()` 只会返回这三种
// 之一（`NONE` 在上面就 early-return 了），所以这个分支只为"以后又加了编码却
// 忘了改这里"时**不产生重复字符**，宁可多打一次压缩。
static char compress_method_key_char(http_compress_method m) {
  switch (m) {
    case http_compress_method::BROTLI:  return 'b';
    case http_compress_method::GZIP:    return 'g';
    case http_compress_method::DEFLATE: return 'd';
    default:                            return 'd';
  }
}

// `content-encoding` 头值，与上面那个字符一一对应（同一个 switch 的两半，
// 分开写是因为一处要字符、一处要字符串；`http_compress_method_to_string()`
// 不在公共头里，且这里的三个值就是 RFC 里那三个字面量）。
static const char* compress_method_header_value(http_compress_method m) {
  switch (m) {
    case http_compress_method::BROTLI:  return "br";
    case http_compress_method::GZIP:    return "gzip";
    case http_compress_method::DEFLATE: return "deflate";
    default:                            return "deflate";
  }
}

size_t uvcpp_http_server::compress_variant_total_bytes_locked() const {
  size_t n = 0;
  for (std::map<std::string, compress_variant>::const_iterator it =
           compress_variants_.begin();
       it != compress_variants_.end(); ++it) {
    n += it->second.bytes();
  }
  return n;
}

void uvcpp_http_server::compress_variant_evict_locked() {
  while ((compress_variant_total_bytes_locked() > kCompressVariantMaxBytes ||
          compress_variants_.size() > kCompressVariantMaxEntries) &&
         !compress_variants_.empty()) {
    // 线性找最久未用的那次够用，且不必维护第二个容器与随之而来的迭代器失效问题。
    // 条数上限是 1024，所以最坏情况下这一趟是 O(n²)（每轮 O(n) 找 LRU，而字节那条
    // 腿可能要求删掉上千条才降到线下）—— 但触发它需要"几条接近单条上限的大条目 +
    // 上千条几十字节的小条目"这种混合表，且只在**存入**时进这个循环（deflate 之后
    // 的冷路径），不在请求路径上。
    auto victim = compress_variants_.begin();
    for (auto it = compress_variants_.begin(); it != compress_variants_.end();
         ++it) {
      if (it->second.last_used < victim->second.last_used) victim = it;
    }
    compress_variants_.erase(victim);
  }
}
#endif  // UVCPP_HTTP_COMPRESS_ENABLE

uvcpp_http_server::compress_variant_stat
uvcpp_http_server::compress_variant_stats() const {
#if UVCPP_HTTP_COMPRESS_ENABLE
  // 三条外层入口之一：这里独占地读四个计数 + 整张表（`entries` 与 `bytes` 都从表里
  // 算），读一半就有别的循环写进去的话，`entries`/`bytes` 会与计数对不上。
  std::lock_guard<std::mutex> lk(compress_mu_);
  compress_variant_stat s;
  s.hits    = compress_variant_hits_;
  s.misses  = compress_variant_misses_;
  s.stored  = compress_variant_stored_;
  s.entries = compress_variants_.size();
  s.bytes   = compress_variant_total_bytes_locked();
  return s;
#else
  // 头文件里明写了"压缩器关掉时返回全零、调用方不必跟着条件编译"，而成员本身
  // 在 `#if` 里 —— 少了这条分支，两个编码器都关掉时整个文件就编不过。本机默认树
  // 恰好是开的（CMakeLists 默认 OFF，webapp 树一律 ON），所以只在别的树/CI 上看得见。
  return compress_variant_stat();
#endif
}

bool uvcpp_http_server::apply_compression(conn_ctx& ctx,
                                          uvcpp_http_response& resp) {
  // 转手给按编码串取的那一份。整个决策只用到连接上的**一个**字段，而三条传输
  // 拿这个字段的方式各不相同（h1 在连接级单槽、h2 临时借进那个槽、h3 压根没有
  // 槽）—— 实现只能有一份，否则同一个处理函数在不同传输上的响应形状会分叉。
  return apply_compression_for(ctx.accept_encoding, resp);
}

bool uvcpp_http_server::apply_compression_for(
    const std::string& accept_encoding, uvcpp_http_response& resp) {
#if UVCPP_HTTP_COMPRESS_ENABLE
  if (!compress_enabled_) return false;

  // 流式响应（`transfer-encoding` 已经在头部里声明了）**不压缩**：body 还不
  // 存在，此刻按 `content_type()` 判定并打上 `content-encoding: gzip` 是
  // **声明了一件没发生的事**；而真要压一个流，得每块一个 deflate 上下文，
  // 那是另一个特性。
  //
  // 注意这是**第二道**门。第一道是结构性的：`begin_stream()` 这条入口压根
  // 不调用本函数。所以一个"忘了在 begin_stream 里设 transfer-encoding"的
  // 调用方会让这一句静默失效 —— 但那时它也没在走流式协议。
  if (resp.has_header("transfer-encoding")) return false;

  if (resp.body.size() < compress_min_body_) return false;
  // 这里**不排除 HEAD**：RFC 9110 §9.3.2 要求 HEAD 发与 GET 相同的头字段，
  // 而 `content-length` 与 `content-encoding` 正是压缩决策的结果。对 HEAD 压
  // 一遍只是为了**算出那个长度**，压出来的字节不会被发出去 —— 上游
  // `send_response` 用 `to_string(include_body=false)` 序列化，body 在那里丢掉。
  //
  // 这里曾经有一句 `if (ctx.is_head) return false;`。它当时**够不着**（webapp
  // 层的 `sync_meta()` 已经把 body 清空，上面那道尺寸门先返回），所以删掉它
  // 看不出任何变化 —— 而 `sync_meta()` 那处一改，它就立刻变成真正的拦截点。
  // 两处是同一个缺陷的两条腿，必须一起改。
  if (resp.has_header("content-encoding")) return false;
  // 206 一律不编码。一旦带上 `content-encoding`，`content-range` 的字节坐标
  // 就该按**编码后**的流来算（RFC 9110 §8.4 把内容编码算作表示的一部分），
  // 而今天那条范围是在**原文件**上切的 —— 于是
  // `content-range: bytes 0-4095/253952` 与 `content-length: 1305` 描述的是
  // 两个不同的东西，续传方按前者推后者会接不上。206 **必须**带
  // `content-range`（§15.3.7），躲不开；要自洽就只能先压整个文件再切片，
  // 那正好把范围请求的意义抵消掉。所以与 nginx 同策：**有范围就不编码**。
  // 附带的好处是请求方拿到的正是它要的原始字节（续传到文件、seek 都要原字节，
  // 半路插进来的 gzip 帧只会让"先 200 后 206"的续传拼出坏文件）。
  //
  // 这也把两条路对齐了：流式那条（`send_file_range` → `begin_stream()`）压根
  // 不调用本函数，大文件的 206 本来就不压 —— 同一个状态码在两条路上行为不同
  // 本身就是缺陷，这一句让两边都变成"206 不编码"。
  switch (resp.status_code) {
    case http_status::NO_CONTENT:
    case http_status::NOT_MODIFIED:
    case http_status::PARTIAL_CONTENT:
      return false;
    default:
      break;
  }
  if (static_cast<int>(resp.status_code) < 200) return false;

  http_compress_method best =
      http_compress::parse_accept_encoding(accept_encoding);
  if (best == http_compress_method::NONE) return false;

  const std::vector<std::string>& excluded =
      compress_excluded_types_.empty() ? http_compress::default_excluded_mime_types()
                                       : compress_excluded_types_;
  if (!http_compress::should_compress(resp.content_type(), excluded)) return false;

  // 到这里"要压"已经定了。缓存只在**这之后**介入 —— 它记的是"同一份输入算过
  // 没有"，不参与上面任何一个判断。
  //
  // 这也是四个运行时旋钮（`set_compression_enabled` / `set_compress_min_body_size`
  // / `set_compress_excluded_types` / `add_compress_excluded_type`）天然安全的原因：
  // 配置一变，要么在门前就 early-return（压根不查表），要么查不到（miss）——
  // 不存在"配置改了、表里还发旧结果"这回事。**谁要把查表提到门前，这条就破了。**
  //
  // 第五个旋钮 `set_compress_level` 走的是**第三条**路：它不改"要不要压"（那
  // 四个判的才是这个），只改"压成什么字节"，所以它既不在门前返回、也不指望
  // miss —— 它**进键**（见下）。于是它一变就必然 miss，同样不可能发出旧结果。
  //
  // 另外半句（这一条是量出来的）：**提前查表也换不到你要的东西。** 热路径上曾经
  // 有过一次**纯白付**的拷贝 —— 静态层先把整份文件拷进 `resp.body`（248 KB 那
  // 档是 253952 B/响应），而命中那一支下一句 `clear()` 就把它丢掉了。消掉它的
  // 方向是让身体**不拷贝地就位**（`uvcpp_buf` 的共享手柄，见 #17 的 ①），不是把
  // 查表提前：查表再早，只要身体已经拷进来，那次拷贝照样白付。而且它**早不了**：
  // 键里的 `cache_tag` 与门槛要看的 `body.size()` 都是身体就位之后才有的。
  //
  // 键 = `编码字符 + 档位字符 + cache_tag`：两个**定长**前缀都在前、`cache_tag`
  // 在尾（理由见 `.h` 里那段，以及 `compress_level_key_char` 上面那段）。
  //
  // 三样都是"这一条变体是谁"的必要成分。少一样就会把另一档压出来的字节当成这一
  // 档的发给对端，而**每一层看着都对**：编码对、`content-length` 与实际的体自洽、
  // 解压出来也逐字节相同 —— 只有线上字节数与档位不符，那正是"省掉的 CPU"与
  // "多花的带宽"这笔账算错的方向。
  //
  // 这里**不**在 `set_compress_level()` 里顺手清表：清表与进键是两条互相顶替的
  // 机制，两条都在的话"档位入不入键"就再也观测不到了 —— 清完表里只剩当前档位，
  // 把档位从键里删掉照样全绿（`web_app_app_func.cpp` 那条客户端用例刚吃过一次
  // 同形的亏：整条链上一处都没压，`body == 原文` 照样成立 —— 于是那里补了"线上
  // 字节以 `1f 8b` 开头"这类前提断言）。只留进键这一条，于是"换档后必须重新压"
  // 可以直接量到。
  // 代价是改档位后上一代那条会滞留到被 LRU 淘汰（另一档的字节还在表里占着）。
  // 档位通常只在启动期设一次，那时表本来就是空的；跑着改也正确，只是会多留一条。
  const bool cacheable = !resp.cache_tag.empty() &&
                         resp.body.size() <= kCompressVariantMaxEntry;
  const int level = compress_level();
  const std::string vkey =
      cacheable ? std::string(1, compress_method_key_char(best)) +
                      compress_level_key_char(level) + resp.cache_tag
                : std::string();

  // 头这三件事命中与未命中都要做，且**必须逐字一样** —— 命中路径与现算是同一个
  // 响应形状，差别只在字节从哪来。
  const auto finish_headers = [&resp, best]() {
    resp.set_header("content-encoding", compress_method_header_value(best));
    // Remove any stale Content-Length — to_string() re-adds it from the new
    // (compressed) body size.
    resp.remove_header("content-length");
    // Vary for CDN/proxy cache correctness (RFC 7231 §7.1.4). Set whenever we
    // made an encoding decision, so a cache never serves a compressed body to a
    // client that cannot decode it.
    if (!resp.has_header("vary")) resp.set_header("vary", "accept-encoding");
  };

  if (cacheable) {
    // 三条外层入口之一。锁里**只做查表与记账**：`finish_headers()` 改的是响应、
    // 不碰表，所以留在锁外 —— 临界区不许嵌进任何"会改动别的共享状态"的代码。
    ::std::shared_ptr<const ::std::string> hit;
    bool found = false;
    {
      std::lock_guard<std::mutex> lk(compress_mu_);
      auto it = compress_variants_.find(vkey);
      if (it != compress_variants_.end()) {
        ++compress_variant_hits_;
        it->second.last_used = ++compress_variant_clock_;
        // 递句柄，一个字节都不拷（第 ④ 步）。`share()` 顺手把旧的那份放掉 ——
        // 静态层借来的那份就这么还回去，既不物化、也不多一次拷贝。
        //
        // 句柄在锁里拷出来、`share()` 在锁外调：`share()` 要放掉响应原来那份，
        // 那是 `uvcpp_buf` 的事，与本表无关，没有理由把它圈进临界区（那条路
        // 一旦沾上用户侧的 `uvcpp_buf`，临界区就开始随别处的改动而变长）。
        hit = it->second.data;
        found = true;
      } else {
        ++compress_variant_misses_;
      }
    }
    if (found) {
      resp.body.share(hit);
      finish_headers();
      return true;
    }
  }

  const size_t src_size = resp.body.size();
  auto result = http_compress::compress(resp.body.get_const_data(),
                                        resp.body.size(), best, level);
  if (!result.success) return false;

  // 压出来的那份字节**只存在一份**：响应与表共用同一个句柄（第 ④ 步）。
  // 这一份拷贝仍在 —— 它是 deflate 的产出落进那个共享串（冷路径一次性的事）；
  // ④ 去掉的是另外两份：存表那份，以及每次命中拷回来的那份。
  const size_t out_n = result.data.size();
  const char *out_p = result.data.get_const_data();
  ::std::shared_ptr<const ::std::string> made =
      (out_p != nullptr && out_n > 0)
          ? ::std::make_shared<const ::std::string>(out_p, out_n)
          : ::std::make_shared<const ::std::string>();
  resp.body.share(made);
  finish_headers();

  // 存进表**不再拷**：存的就是上面那个 `made` —— 响应与表共用同一个句柄。
  // 以前这里要 clone 一份，理由是"响应自己那份还要发出去，不能 move 走"；
  // 句柄把"不能 move"换成了"不必拷"：`shared_ptr<const std::string>` 让两边
  // 同时活着，各持一个引用计数而已。这一份拷贝换来的是之后每次重复请求都省掉
  // 整个 deflate（两个数量级的差价），而现在连这一份也不用付了。
  //
  // 压完不比原文小就不存：那种 body 本来就压不动，存了也只是占地方。
  if (cacheable && resp.body.size() < src_size) {
    // 三条外层入口之二：存入 + 淘汰**同一次临界区**。淘汰那句要读整张表的字节
    // 总量，与这次存入是同一个"表的状态" —— 拆成两次加锁会让别的循环插在中间，
    // 淘汰就可能按一个已经不成立的总量做决定（多删或少删）。
    std::lock_guard<std::mutex> lk(compress_mu_);
    compress_variant v;
    v.data = made;
    v.last_used = ++compress_variant_clock_;
    compress_variants_[vkey] = std::move(v);
    ++compress_variant_stored_;
    compress_variant_evict_locked();
  }
  return true;
#else
  (void)accept_encoding;
  (void)resp;
  return false;
#endif
}

void uvcpp_http_server::enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client,
                                      const std::string& wire,
                                      std::function<void(int)> done) {
  // **空闲时直接发，不绕队列。** 这是每个响应都走的那条路：一次 `queued_write`
  // 构造、一次入队、一次出队、两次移动、两次析构，换来的只是"下一条才轮到它"
  // 这个此时并不存在的约束。判据与 `pump_write` 的早返回逐字相同 —— 它见到
  // 这两个标志也是直接返回。见 `start_write` 的前置条件说明。
  if (!ctx.write_pending && !ctx.closing) {
    start_write(ctx, client, wire, nullptr, false, std::move(done));
    return;
  }

  queued_write qw;
  qw.bytes = wire;  // 拷一份：这块要活到冲刷时，而 wire 那块得留着容量
  qw.done = std::move(done);
  ctx.write_queue.push_back(std::move(qw));
  pump_write(ctx, client);
}

void uvcpp_http_server::enqueue_write(conn_ctx& ctx, uvcpp_tcp_client* client,
                                      const std::string& head, uvcpp_buf body,
                                      std::function<void(int)> done) {
  if (!ctx.write_pending && !ctx.closing) {
    start_write(ctx, client, head, &body, true, std::move(done));
    return;
  }

  queued_write qw;
  qw.bytes = head;  // 拷一份（同上：head 可能是连接上那块复用缓冲）
  // 移动的是**句柄**（共享视图转引用计数、自有块转所有权），不是字节 ——
  // 这条队列本身就是"零拷贝"能不能成立的地方：这里如果拷了，后面两块写得再
  // 干净也白搭。见 `uvcpp_buf` 里"必须显式写移动"那段。
  qw.body     = std::move(body);
  qw.has_body = true;
  qw.done     = std::move(done);
  ctx.write_queue.push_back(std::move(qw));
  pump_write(ctx, client);
}

void uvcpp_http_server::fire_write_done(const std::shared_ptr<write_done>& d,
                                        int status) {
  if (!d || d->fired) return;
  d->fired = true;

  // 先换到栈上再调 —— 本仓已经踩过四次"正在执行的闭包被自己析构"（`delete
  // wr`、`delete w`、`~uvcpp_ws_client` 泵循环、`uvcpp_fs` 的 `operator=`）。
  // 这里的 `fn` 归 `d` 所有，而 `d` 是 `shared_ptr`，调用方手上那一份保证了
  // 它在回调期间活着；但"恰好一次"这个语义本身就该由这一行显式表达出来。
  std::function<void(int)> fn;
  fn.swap(d->fn);
  if (fn) fn(status);
}

// 结算这次写。**身体是从 `start_write` 里那个 `on_written` 闭包整段搬过来的**
// （连注释一起），所以"一块那条路"与"两块那条路"结算的仍然是同一份逻辑；搬出来
// 的另一个理由是复用形态那条路也要用它，而那条路的完成回调是个函数指针、捕不了
// 任何东西。见 `start_write` 里那段「重入次序铁律」。
void uvcpp_http_server::settle_write(uvcpp_tcp_client* client,
                                     const std::shared_ptr<write_done>& wd,
                                     uvcpp_write* recycle, int status) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto c = tbl.find(client);
  if (c == tbl.end()) {
    // 连接已经从表里消失；这一块的下场仍然是"不会写出去了"。
    //
    // 复用形态还要多做一件事：那个写请求本来该在下面的 `cc.wrecycle` 格里等下
    // 一笔，而那张表项已经没了 —— 除了本函数没有别人认它（库里只在完成回调这
    // 一刻认），所以在这里放掉。**必须在 `fire_write_done` 之前**：闭包最自然
    // 的动作就是再写一块，那一刻它既不该看见一个已经没人要的请求，也不该因为
    // 这一格而被牵着走。
    if (recycle != nullptr) uvcpp_tcp_client::release_owned_write(recycle);
    fire_write_done(wd, status != 0 ? status : UV_ECANCELED);
    return;
  }

  conn_ctx& cc = c->second;
  cc.write_pending = false;
  // **先把在途这一块摘下来，再关连接。** 反过来的话
  // close_connection 会把它当成"还没结算的在途块"用 UV_ECANCELED
  // 唤醒，而这里明明拿到了真实结果（ECONNRESET 之类）—— 调用方
  // 就再也分不清"对端在写的时候走了"和"框架自己取消的"。
  cc.inflight.reset();
  // 复用形态：请求就地还回按连接的槽，下一笔 `start_write` 直接取用。
  // **必须在 `close_connection` 之前**：那个函数可能经 `remove_ctx` 把这张表项
  // 抹掉，抹掉之后这一格连同里面的请求一起消失（`remove_ctx` 会放掉它）。
  if (recycle != nullptr) cc.wrecycle = recycle;
  if (status != 0) {
    // The peer is gone: nothing still queued can be delivered, and
    // the connection needs retiring. Any `done` still parked in the
    // queue is woken by close_connection() with UV_ECANCELED.
    close_connection(client);
    fire_write_done(wd, status);
    return;  // `cc` 已经被 close_connection 抹掉，后面一个字都不能碰
  }

  fire_write_done(wd, 0);

  // **`wd` 为空时上面那句什么用户代码都没跑**（`fire_write_done` 第一句就是
  // `if (!d || d->fired) return;`），`contexts_` 必然还是刚才那个样子 —— 所以
  // `c` 仍然有效，这一次 find 直接省掉。有 `wd` 时才必须重查：闭包最自然的
  // 动作就是关连接或再写一块，两者都可能把表项摘掉。普通响应走的正是省掉
  // 这一查的那条（`enqueue_write` 的五个调用点里四个不传 `done`）。
  if (wd) {
    c = tbl.find(client);
    if (c == tbl.end()) return;
  }
  if (c->second.closing) return;

  // **不在这里判 `close_requested`。** 这里是"某一块写完了"，而
  // `close_requested` 的语义是"**我排的队写完**之后关"，不是"下一个写
  // 完成时关"。一个 handler 完全可能（而且流式响应里就是常态）在**头部
  // 还在途**的时候就把整条响应写完并调 `end_stream(close_after=true)`：
  // 那时队列里还压着 body 的每一块，而本次写完成的是**头部**。早先在这里
  // 判它，等于把整个 body 当成"写完头部之后的余量"丢掉 —— 线上症状是
  // 只剩一个头部块、连接随即关闭（`web_stream_response_func` 的
  // `stream_roundtrip` 就是这么红的）。
  //
  // 交给 pump_write 的空队列分支去收尾：它只在**队列真的空了**时才看
  // `close_requested`，而且那条分支还带着"对端还在发 → 把关闭推迟到
  // 消息结束"的判定（上传路径需要），在这里重写一遍必然漏掉那个判定。
  pump_write(c->second, client);
}

void uvcpp_http_server::recycle_write_done(uvcpp_write* w,
                                           uvcpp_tcp_client* client,
                                           int status, void* arg) {
  // 无捕获 ⇒ 转成 `std::function` 时走小对象缓冲，不落堆 —— 这一笔与请求对象、
  // 头部缓冲那两笔一样，都是从这条路上省下来的。这里一转就回到那条共用结算路径。
  static_cast<uvcpp_http_server*>(arg)->settle_write(
      client, std::shared_ptr<write_done>(), w, status);
}

void uvcpp_http_server::pump_write(uvcpp_tcp_client* client) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return;
  pump_write(it->second, client);
}

// 一次写最多带走多少字节。**这个上限是给"攒批会拷贝"留的后路**，不是给系统调用
// 留的 —— 攒批的理由恰恰是字节不值钱、系统调用才值钱：
//
//   回环上一次 send 的内核开销是**每报文**的固定成本。改前实测（25 连接 × 流水线
//   10）服务端 23.6 µs/请求，其中约 74% 在内核里；而一个响应才 100 多字节 —— 按字
//   节摊就是 100 ns/B 上下，那个数不可能是拷贝成本。所以把 10 个响应攒成一次
//   `WSASend` 省下的是 9 次固定成本，代价是这 1000 多字节的一次 memcpy，量级差三
//   个数量级：同一装置上改完 15.8–16.8 µs/请求，段/请求 3.43 → 1.99。
//
// 但**体是零拷贝交下来的**（`uvcpp_buf` 要么是共享视图要么是自有块，见
// `enqueue_write` 那段"这里如果拷了，后面两块写得再干净也白搭"）。攒批会把体
// 拷进这条串里，所以大块不能攒：那等于用一整份 memcpy 去换一次系统调用，方向就
// 反了。上限之外的那些照旧单独走（`batch_n == 1` 那条路），行为与改动前一致。
static UVCPP_CONSTEXPR size_t kCoalesceMaxBytes = 64u * 1024u;

void uvcpp_http_server::pump_write(conn_ctx& ctx, uvcpp_tcp_client* client) {
  // uvcpp_tcp_client holds at most one async write at a time; the queue is
  // what makes back-to-back keep-alive responses safe.
  if (ctx.write_pending || ctx.closing) return;

  if (ctx.write_queue.empty()) {
    // 流式响应里"队列空了"是常态（头部写完、下一块还没来），不是收尾信号。
    if (ctx.out_streaming) return;
    if (ctx.close_requested) {
      // The response that requested the close may have been written while the
      // peer was still sending its body (a claimed stream stopping an upload
      // early). Closing here would RST, and the RST discards that response from
      // the peer's receive buffer. Hand the close to the end of the message
      // instead — but only when there is a stream handler that will still be
      // alive to receive it. A malformed message never completes, and there the
      // close must happen now: **this layer has no idle sweep of its own**, so a
      // bare uvcpp_http_server (nothing above it) would hang for good — the
      // sweep that gets named elsewhere belongs to the webapp layer (60 s).
      if (ctx.stream_handler && !ctx.msg_done) {
        ctx.defer_close_to_message_end = true;
        ctx.close_requested = false;
      } else {
        close_connection(client);
      }
    }
    return;
  }

  // 队首那一块永远收下（`batch_n > 0` 这个前提），所以它自己就超限时是**单独
  // 发**，不会被饿死。
  size_t batch_bytes = 0;
  size_t batch_n     = 0;
  for (std::deque<queued_write>::const_iterator it = ctx.write_queue.begin();
       it != ctx.write_queue.end(); ++it) {
    const size_t one =
        it->bytes.size() + (it->has_body ? it->body.size() : 0);
    if (batch_n > 0 && batch_bytes + one > kCoalesceMaxBytes) break;
    batch_bytes += one;
    ++batch_n;
  }

  if (batch_n == 1) {
    queued_write qw = std::move(ctx.write_queue.front());
    ctx.write_queue.pop_front();
    start_write(ctx, client, qw.bytes, &qw.body, qw.has_body,
                std::move(qw.done));
    return;
  }

  // 攒批：把前 `batch_n` 项**按队列次序**拼进一条串，体也拷进来 —— 这一份拷贝
  // 就是上面那个上限要挡的东西。各块的 `done` 收进一个结算器，整批完成时按次序
  // 一起唤醒（今天只有 `write_stream` 那一个调用点传 `done`）。
  //
  // 整批只算**一次**在途块：`ctx.inflight` 只有一个槽位，而 `start_write` 这次
  // 只发一次 `uv_write` —— 两边的"一次"是对齐的。所以对端在中途断开时
  // `close_connection` 把这一批的 done 一起唤醒成 `UV_ECANCELED`，与逐块发时
  // "在途那块拿真实错误、排队那些拿 ECANCELED"结算的是**同一组**闭包。
  std::string wire;
  wire.reserve(batch_bytes);
  std::vector<std::function<void(int)> > dones;
  for (size_t i = 0; i < batch_n; ++i) {
    queued_write qw = std::move(ctx.write_queue.front());
    ctx.write_queue.pop_front();
    wire.append(qw.bytes);
    if (qw.has_body && qw.body.size() > 0) {
      wire.append(qw.body.get_const_data(), qw.body.size());
    }
    if (qw.done) dones.push_back(std::move(qw.done));
  }

  std::function<void(int)> done;
  if (dones.size() == 1) {
    done = std::move(dones[0]);
  } else if (dones.size() > 1) {
    // C++11 有意：`[ds = std::move(dones)]` 是 C++14 的 init-capture，而本仓是
    // `CMAKE_CXX_STANDARD 11` + `REQUIRED ON`（见 `uvcpp_memory_pool.h` 里那处
    // 同族的修法）。这里多一个 `shared_ptr` 是因为闭包必须是可拷贝的。
    std::shared_ptr<std::vector<std::function<void(int)> > > ds(
        new std::vector<std::function<void(int)> >(std::move(dones)));
    done = [ds](int status) {
      for (size_t i = 0; i < ds->size(); ++i) {
        if ((*ds)[i]) (*ds)[i](status);
      }
    };
  }

  start_write(ctx, client, wire, nullptr, false, std::move(done));
}

void uvcpp_http_server::start_write(conn_ctx& ctx, uvcpp_tcp_client* client,
                                    const std::string& wire, uvcpp_buf* body,
                                    bool has_body,
                                    std::function<void(int)> done) {
  ctx.write_pending = true;

  // 在途这一块的 `done` 装进一次性结算器，并且 **ctx 也留一份**：网络层的完成
  // 回调在"客户端已被析构"时会直接早返回（`if (!token_alive(life))`），而服务端
  // 连接恰恰是被 tcp_server 的 close manager 删掉的 —— 没有 ctx 这一份兜底，
  // 对端在写入途中断开时这一块的 `done` 就永远不响了。见 write_done 的注释。
  // **`done` 为空时一个堆分配都不做。** `enqueue_write` 的五个调用点里有四个不传
  // `done`，而普通响应走的正是其中那条 —— 结算器的**全部**作用就是把那个闭包唤醒
  // 一次，而空闭包唤醒了什么都不做。`fire_write_done` 第一句是
  // `if (!d || d->fired) return;`，所以 `wd` 与 `ctx.inflight` 双双留空时，
  // 完成回调、`close_connection`、`remove_ctx` 三条路都走同一条"什么都不做"的早
  // 返回 —— 语义逐条相同。省下的是**每个响应两个堆分配**（`new write_done` 一个、
  // `shared_ptr` 控制块一个，这里没走 `make_shared`）外加两次原子引用计数。
  std::shared_ptr<write_done> wd;
  if (done) {
    wd.reset(new write_done());
    wd->fn = std::move(done);
  }
  ctx.inflight = wd;

  // 1 块时那次写会把字节拷进自己的缓冲，所以 `wire` 不必活过这一句。
  // **2 块时不是**：那边体不拷，写请求持有它（共享视图接引用计数、自有块接
  // 所有权）直到完成回调 —— 所以 `*body` 在 `write()` 返回之后即便析构也安全，
  // 前提是它的内容已经被交出去了（见 `uvcpp_tcp_client::write` 的消费语义）。
  // 队列那条路上这个指针指着 `pump_write` 的局部 `qw.body`；`start_write` 的
  // 直发那条路上指着 `enqueue_write` 的按值参数 —— 两条一样，都在调用方的栈上。
  //
  // ---------------------------------------------------------------------
  // 重入次序铁律（本仓已踩过三次同族：`delete wr`、`delete w`、
  // `~uvcpp_ws_client` 里泵循环）。这个闭包持着 `conn_ctx&` —— 一个指向
  // `contexts_` 内部的引用 —— 而 `done` 里最自然的动作恰恰是**再写一块**
  // （重入 pump_write）或**关连接**（close_connection）。所以次序只能是：
  //
  //   ① 结算这次写（**只在这一步碰 contexts_**，出块前把要用的值拷出来）
  //   ② 在**不持有任何引用**的那一刻调用 done
  //   ③ 重新 find 再决定下一步（done 可能已经关了连接、也可能又写了新块）
  //
  // 回调之后**不许再碰 cc**。
  // ---------------------------------------------------------------------
  // 回调提到两个重载外面来：两块那条路与一块那条路是**同一个**结算逻辑，
  // 抄成两份就是给"以后只改了一份"留口子（本仓的 ws 那族就是这么来的）。
  auto on_written = [this, client, wd](int status) {
    settle_write(client, wd, nullptr, status);
  };

  // 复用形态那条路带出来的请求（一块/两块都可能有），失败时还回槽用。
  uvcpp_write* recycle_w = nullptr;
  int rc = 0;
  if (wd) {
    rc = has_body ? client->write(wire.c_str(), wire.size(), body, on_written)
                  : client->write(wire.c_str(), wire.size(), on_written);
  } else {
    // ---- 复用形态 ----
    // 没有 `done` 的调用点（`enqueue_write` 五个里占四个，普通响应正在其中）
    // 走这条：请求对象连同它的 `uv_write_t`、头部缓冲都按连接复用，完成回调是
    // 个函数指针 ⇒ 从请求对象到回调兑现**一次堆分配都没有**。
    //
    // 有 `done` 的那些必须留在原地：结算器的全部作用就是把那个闭包唤醒一次，
    // 而闭包的去向只有 `wd` 知道（见上面那段"空闭包"的说明）—— 把 `done` 的
    // 所有权搬到另一个容器里是另一件事，不在这笔里做。
    if (ctx.wrecycle == nullptr) ctx.wrecycle = new uvcpp_write();
    recycle_w = ctx.wrecycle;
    ctx.wrecycle = nullptr;  // 出去的那一刻就不在这格里了（见 conn_ctx::wrecycle）
    rc = client->write_owned(recycle_w, wire.c_str(), wire.size(),
                             has_body ? body : nullptr,
                             &uvcpp_http_server::recycle_write_done, this);
  }

  if (rc != 0) {
    // The write never started, so its completion callback will never fire.
    // Drop the queue and retire the connection instead of leaking it.
    // close_connection() 会把队列里每块的 done —— 以及在途那一块 —— 唤醒成
    // UV_ECANCELED。**但这一块的下场是本次真实的 rc**，所以先把它从 ctx 上摘
    // 下来（`fire_write_done` 的幂等因此由"谁先拿到"决定，而不是由时序碰运气），
    // 关完连接再用真实 rc 结算。
    ctx.write_pending = false;
    ctx.inflight.reset();
    // 复用形态：交不出去的请求还回槽 —— 下面 `close_connection` 必然经由
    // `remove_ctx`（或延迟到关闭收尾的那次）走过 `ctx.wrecycle`，由它连同头部
    // 缓冲一起放掉。**不能在这里自己 delete**：那时 `body` 的所有权可能已经交给
    // 这个请求了（`adopt_body`），而调用方栈上那份 `uvcpp_buf` 还指着同一块 ——
    // 还回槽里让同一个出口收尾，就不存在两条释放路径。
    if (recycle_w != nullptr) ctx.wrecycle = recycle_w;
    close_connection(client);
    fire_write_done(wd, rc);
  }
}

void uvcpp_http_server::close_connection(uvcpp_tcp_client* client) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return;
  conn_ctx& ctx = it->second;
  if (ctx.closing) return;  // close is issued exactly once
  ctx.closing = true;

  // 队列里还没发出去的块，它们的 done **永远不会**被触发（连接要关了）。
  // 先整批搬出来，再等关闭动作做完之后逐个唤醒 —— 不能在这里边遍历边调：
  // done 里可能又往队列里写（那时 closing 已为真，pump_write 直接返回，
  // 不会递归回来），也可能让 close_connection 走 remove_ctx 那条路把 ctx 抹掉。
  std::deque<queued_write> dropped;
  dropped.swap(ctx.write_queue);

  // **在途那一块也要一起摘。** 它不在队列里 —— 它的 done 已经交给网络层的
  // 写完成回调了，而那条回调有一条早返回（`if (!token_alive(life)) return;`）：
  // 服务端连接是被 tcp_server 的 close manager 删掉的，于是"对端在写入途中
  // 断开"这条最普通的路会让它**永远不响**。同样必须在这里搬出来再唤醒，理由
  // 与 dropped 一样：erase(it) 之后 ctx 不可再用。
  std::shared_ptr<write_done> inflight;
  inflight.swap(ctx.inflight);

  const bool no_tcp = (client->get_tcp() == nullptr);

  if (no_tcp) {
    remove_ctx(client);  // 会 erase(it)，之后 ctx 不可再用
  }

  // 到这里 ctx 可能已经失效，所以下面**只读 dropped 与 inflight**。
  for (size_t i = 0; i < dropped.size(); ++i) {
    if (dropped[i].done) dropped[i].done(UV_ECANCELED);
  }
  fire_write_done(inflight, UV_ECANCELED);

  if (no_tcp) return;
  // 走 `client->close()`，**不是** `client->get_tcp()->close(...)`。
  //
  // 后者只调 libuv 的关闭，读回调的 `nread < 0` 分支永远不会来 —— 而那是
  // 原来唯一会通知框架的地方。结果是：服务端主动关的连接，客户端对象既不被
  // 摘除也不被释放，永远留在 tcp_server 的登记表里（每个 `Connection: close`
  // 的响应漏一个，越跑越大）。`client->close()` 在关闭完成时补上这个通知。
  //
  // remove_ctx 不用在这里传：它已经挂在客户端的 on_close 上（见
  // on_tcp_connection），主动关闭和对端断开两条路都会经由 fire_close_callbacks
  // 恰好跑到它一次。传进来反而会让 on_connection_close 触发两次。
  client->close();
}

void uvcpp_http_server::note_ctx_generation(uvcpp_tcp_client* client,
                                            uint64_t generation) {
  if (client == nullptr) return;
  std::lock_guard<std::mutex> lk(ctx_gen_mu_);
  ctx_gens_[client] = generation;
}

void uvcpp_http_server::forget_ctx_generation(uvcpp_tcp_client* client) {
  if (client == nullptr) return;
  std::lock_guard<std::mutex> lk(ctx_gen_mu_);
  ctx_gens_.erase(client);
}

uint64_t uvcpp_http_server::connection_generation(
    uvcpp_tcp_client* client) const {
  if (client == nullptr) return 0;
  // **查登记簿，一张按循环切的表都不碰、`client` 指向的内存一个字节都不读。**
  //
  // 本 API 的文档用法（见头文件那条的 @code 段）就是"把开始时记下的那个指针、
  // 过一会儿在异步完成回调里再问一次"——而那时连接**可能已经 delete 掉了**，
  // 手里这个指针是悬垂的。`contexts_` 对这个问题的两个答法都走不通：
  //
  //   * 走 `ctxs_of()`：多循环下它必须先读 `client->loop_index()` 才知道查哪张
  //     表，而那就是对被释放对象的解引用（单循环下它有一条
  //     `contexts_.size() == 1` 的快路径，读完不读 —— 所以缺陷只在
  //     `set_loops(n>1)` 下存在）。实测 1.6.19：
  //     `tests/functional/web_static_server_func.cpp` 的 `multiloop_concurrency`
  //     跑完 `set_loops(4)` 之后，正是拿一批已经释放的 `uvcpp_tcp_client*` 来
  //     问代次号（用例注释："当 key 用、不解引用"）。Linux + ASan **5/5 次**
  //     报 `heap-buffer-overflow ... READ of size 8`；MinGW 那条 CI 腿上就是
  //     `***Exception: SegFault`（Windows 的堆把释放掉的页还给系统，读它是访问
  //     违例；Linux/macOS 上那块内存通常还映射着，所以只有 ASan 看得见）。
  //   * 逐张表扫一遍（本轮先试过这一版）：那是**跨循环读别人的表**，与那条循环
  //     自己的 insert/erase 是数据竞争。头文件里 `contexts_` 那条注释写着"一张
  //     全局表在两条循环上并发 insert/erase 是 UB，不是'慢一点'"——把一张表换成
  //     四张并不改变这件事。实测同一条用例当场变成
  //     `heap-use-after-free`：`_Rb_tree::find` ← 这里 ← `on_connection` 回调，
  //     被读的那个结点是另一条循环的线程刚在 `remove_ctx` 里 erase 掉的。
  //
  // 于是另立一张只讲"这地址上现在是哪条连接"的表（`ctx_gens_`）：写入只在连接
  // **登记**与**摘除**那四处，与 `contexts_` 的增删成对（`note_ctx_generation()` /
  // `forget_ctx_generation()`），读侧只有本函数与 `is_connected()`，**没有每请求
  // 的调用者**（派发那条路走 `ctxs_here()` / 手上已有的 `ctx`）。
  //
  // 地址被新连接复用时，这里答出的是**新连接**的代次号 —— 与调用方记下的那个
  // 对不上，正是文档要的语义；一条连接都没有就如实返回 0，此时不该往那个地址
  // 写任何字节。
  std::lock_guard<std::mutex> lk(ctx_gen_mu_);
  std::map<uvcpp_tcp_client*, uint64_t>::const_iterator it =
      ctx_gens_.find(client);
  return it == ctx_gens_.end() ? 0 : it->second;
}

bool uvcpp_http_server::is_connected(uvcpp_tcp_client* client) const {
  return connection_generation(client) != 0;
}

// =========================================================================
// 稍后回话：凭据的造与用
// =========================================================================

void uvcpp_http_server::fill_reply(uvcpp_http_reply& r, uvcpp_tcp_client* client,
                                   const conn_ctx& ctx, int32_t stream_id) const {
  // 就地写字段，**不查表**：这是每请求的热路，`ctx` 已经在手上了。
  r.server_     = const_cast<uvcpp_http_server*>(this);
  r.client_     = client;
  r.loop_       = client->get_loop();
  r.loop_index_ = client->loop_index();
  r.stream_id_  = stream_id;
  r.generation_ = ctx.generation;
  r.version_    = uvcpp_http_version::HVER_11;
#if UVCPP_NGHTTP2_ENABLE
  // 版本由**连接**决定，不由流号决定：h2 连接上 `stream_id` 恒非 0，而 h1 上
  // 它恒为 0 —— 判据取哪一个都行，取连接这一个少一次分支歧义。
  if (ctx.h2 != nullptr) r.version_ = uvcpp_http_version::HVER_20;
#endif
}

uvcpp_http_reply uvcpp_http_server::reply_of(uvcpp_tcp_client* client,
                                             int32_t stream_id) const {
  uvcpp_http_reply r;
  if (client == nullptr) return r;
  // 这一个入口**必须**查表：调用方手里只有 `client`，而"代次号"与"这条连接是
  // 不是 h2"两件事都只有 `conn_ctx` 知道。派发那两处不走这里（它们有 `ctx`）。
  const std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  std::map<uvcpp_tcp_client*, conn_ctx>::const_iterator it = tbl.find(client);
  if (it == tbl.end()) return r;  // 不在登记表里 ⇒ 无效凭据（`valid()` 为假）
  fill_reply(r, client, it->second, stream_id);
  return r;
}

#if UVCPP_HTTP3_ENABLE
void uvcpp_http_server::fill_h3_reply(uvcpp_http_reply& r,
                                      uvcpp_quic_connection* conn,
                                      const quic_ctx& qc,
                                      int64_t stream_id) const {
  r.server_     = const_cast<uvcpp_http_server*>(this);
  r.quic_       = conn;
  r.loop_       = qc.loop;
  r.loop_index_ = quic_loop_index_here();
  r.stream_id_  = static_cast<int32_t>(stream_id);
  r.generation_ = qc.generation;
  r.version_    = uvcpp_http_version::HVER_30;
}
#endif

bool uvcpp_http_server::reply_alive(const uvcpp_http_reply& reply) const {
  // `server_ != this` 这一条不是多余的：一枚凭据可以被传给别人、也可以在服务器
  // 析构之后被拿来用，两种情况下它都不是"我们这张表里的一条连接"。
  if (!reply.valid() || reply.server_ != this) return false;
  if (reply.generation_ == 0) return false;  // 从未登记
#if UVCPP_HTTP3_ENABLE
  if (reply.quic_ != nullptr) {
    // h3 的表按循环切，凭据里存的那一号就是它自己那条循环。**不用
    // `quic_ctxs_here()`**：那个入口读的是"当前线程在几号循环"，而本函数
    // 允许在别的线程上被问（问一个布尔值不需要在循环线程上，见头文件的说明）。
    const std::map<uvcpp_quic_connection*, quic_ctx>& t =
        quic_ctxs_at(reply.loop_index_);
    std::map<uvcpp_quic_connection*, quic_ctx>::const_iterator it =
        t.find(reply.quic_);
    return it != t.end() && it->second.generation == reply.generation_;
  }
#endif
  if (reply.client_ == nullptr) return false;
  // **用凭据自己带的循环号选表**，不要去读 `reply.client_` 指向的内存 ——
  // 凭据存在的全部意义就是"连接已经消失之后还能拿它来问"，那时
  // `reply.client_` 是悬垂的（`ctxs_of()` 会读 `client->loop_index()`，那是
  // UAF；单循环下它有快路径不读，所以这也只在 `set_loops(n>1)` 下现形）。
  // 与上面 h3 那条支路同源：那边也是用 `reply.loop_index_` 选表。
  //
  // `ctxs_at()` 的 const 重载对表外的号夹回 0 号，找不到就返回 false ——
  // 宁可不发，也不往一条可能已经换人的连接上写。
  const std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_at(reply.loop_index_);
  std::map<uvcpp_tcp_client*, conn_ctx>::const_iterator it =
      tbl.find(reply.client_);
  return it != tbl.end() && it->second.generation == reply.generation_;
}

int uvcpp_http_server::send_reply(const uvcpp_http_reply& reply,
                                  uvcpp_http_response& resp) {
  if (!reply.valid() || reply.server_ != this) return UV_EINVAL;
  // 连接换人了就**一个字节都不发**。这不是"保险"：地址被新连接复用时，那枚
  // 凭据里存的指针指向的是**另一条连接**，写下去就是把 A 的响应发给 B。
  if (!reply_alive(reply)) return UV_ENOENT;
#if UVCPP_HTTP3_ENABLE
  if (reply.quic_ != nullptr) {
    return send_h3_response(reply.quic_, reply.stream_id_, resp);
  }
#endif
  if (reply.client_ == nullptr) return UV_EINVAL;
  // h2 的流号在 `resp` 上（会话层按它归位），而调用方造出来的响应通常是"新"的、
  // 那一格是 0 —— 凭据里带的那一个才是权威。h1 上这一句是写 0，等于没写。
  resp.stream_id = reply.stream_id_;
  return static_cast<int>(send_response(reply.client_, resp));
}

void uvcpp_http_server::remove_ctx(uvcpp_tcp_client* client) {
  // 对端断开走的是**这条路**（`set_on_close` → 这里），不是 close_connection。
  // 所以队列里那几块的 done 必须在这里唤醒 —— 少了这一步，它们永远不响，而
  // 等它们的人（`send_file` 的传输对象、流式响应的调用方）就**永久挂起**。
  // 这不是理论风险：一个下载到一半的对端断开，服务端此刻队列里通常还压着
  // 好几块。
  //
  // 次序与 close_connection 一致：**先把 ctx 摘掉，再唤醒**。这样 done 里再写
  // （`write_stream` 找不到 ctx，如实返回非 0）或再关（`close_connection` 找
  // 不到 ctx，直接返回）都安全，不会踩到正在被销毁的 `conn_ctx`；而
  // `close_handler_`（框架的断连通知）排在最后，与"先唤醒、后通知框架"的既有
  // 次序一致 —— 否则 done 可能在框架已经拆掉这条连接的上下文之后才跑。
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it != tbl.end()) {
    std::deque<queued_write> dropped;
    dropped.swap(it->second.write_queue);
    // 在途那一块跟着一起摘（理由见 write_done 的注释）：网络层的写完成回调
    // 在"客户端已被析构"时会早返回，而这正是对端断开时发生的事。
    std::shared_ptr<write_done> inflight;
    inflight.swap(it->second.inflight);
    // 复用形态那个写请求也在这一格里（见 conn_ctx::wrecycle）—— 它与上面两块
    // 一样是"只有这张表登记着"的东西：库里只在完成回调那一刻认它，而这条路正是
    // "回调可能永远不来"的那条（对端断开时网络层的写回调早返回）。先取到局部、
    // 抹掉槽，erase 之后再放 —— 与上面两块同一个次序。
    uvcpp_write* wrecycle = it->second.wrecycle;
    it->second.wrecycle = nullptr;
    delete it->second.parser;
#if UVCPP_NGHTTP2_ENABLE
    // h2 层**不拥有** client（那是 tcp_server 的 close manager 的责任），
    // 反过来 client 的上下文拥有 h2 层。对端断开与主动关闭两条路都会汇到这里。
    //
    // h2 这边还没上线的块也要跟上面的 h1 队列一样被唤醒，但**不能就地唤醒**：
    // `done` 跑的是框架流式响应的收尾，它可能再补一笔写，而此刻 ctx 还在、
    // `h2` 却已经没了 ⇒ `write_stream` 会掉进 h1 那条分支，把这笔写挂到一条
    // 根本不是 h1 的连接上（`stream_id` 被静默丢掉）。所以先取走，等 `erase`
    // 之后再跑 —— 那时 `write_stream` 找不到 ctx，如实返回 `UV_ECANCELED`。
    std::vector<std::function<void()>> h2_dropped;
    if (it->second.h2 != nullptr) {
      it->second.h2->take_cancelled_dones(h2_dropped);
    }
    delete it->second.h2;
    it->second.h2 = nullptr;
#endif
    tbl.erase(it);
    forget_ctx_generation(client);  // 与上一句成对，见它的声明

    // 放掉复用形态那个写请求（连同它的头部缓冲）。**放在唤醒 `done` 之前**：
    // 那些闭包最自然的动作就是再写一块，而那一刻本表项已经没有了。
    if (wrecycle != nullptr) uvcpp_tcp_client::release_owned_write(wrecycle);

    for (size_t i = 0; i < dropped.size(); ++i) {
      if (dropped[i].done) dropped[i].done(UV_ECANCELED);
    }
    fire_write_done(inflight, UV_ECANCELED);
#if UVCPP_NGHTTP2_ENABLE
    for (size_t i = 0; i < h2_dropped.size(); ++i) h2_dropped[i]();
#endif
  }

  if (close_handler_) close_handler_(client);
}

size_t uvcpp_http_server::begin_h2_goaway() {
#if UVCPP_NGHTTP2_ENABLE
  // 这里**不关连接**，只打招呼。关是立刻生效的（`uv_close`），而 GOAWAY 得先
  // 排进队列再等几轮循环才出网 —— 两件事挤在一起做，对端拿到的就是"连接断了"，
  // 正好是这一句想避免的那个歧义。
  //
  // **只翻本循环那张表**（多循环）：表是按循环切的，从一条循环上去翻另一条
  // 循环的表就是数据竞争。所以调用方要在**每条**循环上各道一次别（webapp 的
  // 停机正是按循环推进的）；单循环下"本循环"就是"全部"，行为与从前逐字相同。
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_here();
  size_t n = 0;
  for (auto it = tbl.begin(); it != tbl.end(); ++it) {
    uvcpp_h2_connection* h2 = it->second.h2;
    if (h2 == nullptr || h2->closed()) continue;
    if (h2->begin_goaway() == 0) ++n;
  }
  return n;
#else
  return 0;
#endif
}

#if UVCPP_NGHTTP2_ENABLE

void uvcpp_http_server::on_tcp_connection_h2(uvcpp_tcp_client* client,
                                             bool run_connection_hook) {
  conn_ctx ctx;
  ctx.parser = nullptr;  // 与 h2 互斥：h2 连接上没有 llhttp
  ctx.generation = ++next_generation_;
  ctxs_of(client)[client] = ctx;
  note_ctx_generation(client, ctx.generation);  // 与上一句成对，见它的声明

  // 与本层 h1 那条路**次序一致**：登记之后才跑连接钩子，钩子里的
  // `connection_generation(client)` 才拿得到身份。
  //
  // `run_connection_hook` 只有"明文连接改判成 h2"那条路会传 false：那条连接
  // 已经按 h1 登记过一次、钩子也跑过了（见 `switch_to_h2c()`）。改判是**同一条
  // 连接换了协议**，不是新连接，钩子跑第二次等于把调用方的每连接簿记翻倍。
  if (run_connection_hook && connection_handler_) {
    try {
      connection_handler_(client);
    } catch (const std::exception& e) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] on_connection callback threw: %s\n",
                   e.what());
    } catch (...) {
      std::fprintf(stderr,
                   "[uvcpp_http_server] on_connection callback threw (unknown)\n");
    }
  }

  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return;  // 钩子里就把连接关了
  uvcpp_h2_connection* h2 = new uvcpp_h2_connection(client, /*server_side=*/true);
  it->second.h2 = h2;

  // 回调里**一律重新查表**，不缓存 `conn_ctx*`：会话的回调会同步跑用户处理
  // 函数，用户函数可以关掉连接，而 `remove_ctx` 会把那个节点连同 h2 层一起删掉。
  uvcpp_h2_session::callbacks h2c;
  h2c.on_body = [this, client](uvcpp_h2_session&, uvcpp_h2_stream& st,
                               const char* d, size_t n) {
    std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
    auto cit = tbl.find(client);
    if (cit == tbl.end()) return;
    conn_ctx::h2_stream_state& ss = cit->second.h2_streams[st.stream_id];
    if (ss.overflow) return;  // 已经判过超限：后面的块直接丢，别再填回去
    if (max_body_size_ != 0 && ss.body.size() + n > max_body_size_) {
      // 超限就停止累积，但**继续收** —— 让这条流走到 on_request_end 才能
      // 回一个 413。中途 RST 会让对端拿到连接错误而不是一个明确的答复。
      ss.body.clear();
      ss.overflow = true;
      return;
    }
    ss.body.append(d, n);
  };
  h2c.on_request = [this, client, h2](uvcpp_h2_session&, uvcpp_h2_stream& st,
                                      bool) {
    std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
    auto cit = tbl.find(client);
    if (cit == tbl.end()) return;
    // 流 id 只增不复用，正常这里本来就是空的；复位是为了万一有残留状态，也不会
    // 把上一条流的判定带给新流。**不能 erase** —— 无 body 的请求 `on_request` 和
    // `on_request_end` 是背靠背的（同上），擦掉之后那条请求就没人派发了。
    conn_ctx::h2_stream_state& ss = cit->second.h2_streams[st.stream_id];
    ss = conn_ctx::h2_stream_state();

    // extended CONNECT（RFC 8441，#58）：这条流的语义是**长连接**，不能走
    // 普通请求那套"等 body 收尾再派发"—— 它永远收不了尾。头块收全这一刻
    // 是唯一的接管点，此时流上还没有 DATA 到达，装落点不会漏字节。
    if (!st.request.protocol.empty()) {
      ss.extended_connect = true;
      if (h2_extended_connect_handler_ &&
          h2_extended_connect_handler_(*h2, st.stream_id, st.request)) {
        return;  // 已接管：响应与流的生命周期都归调用方
      }
      // 没有接管（或钩子返回 false）：明确拒掉。501 让客户端知道这条路
      // 走不通，可以回落到 h1 的 `Upgrade`。
      uvcpp_http_response resp =
          uvcpp_http_response::make(http_status::NOT_IMPLEMENTED);
      send_h2_response(client, st.stream_id, resp);
      return;
    }
  };
  h2c.on_request_end = [this, client](uvcpp_h2_session&, uvcpp_h2_stream& st) {
    std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
    auto cit = tbl.find(client);
    if (cit == tbl.end()) return;
    conn_ctx& c = cit->second;

    auto sit = c.h2_streams.find(st.stream_id);
    if (sit == c.h2_streams.end()) return;
    conn_ctx::h2_stream_state& ss = sit->second;
    // 活流由接管方处置：它没有"请求收尾"这回事，别当成普通请求再派发一遍。
    if (ss.extended_connect) return;

    std::string body;
    body.swap(ss.body);
    const bool overflow = ss.overflow;
    // 条目**留着**：延迟应答还要回来读 `is_head` / `accept_encoding`。

    if (overflow) {
      // 长度同 h1 那条 413：`sizeof(字面量) - 1`，别手写（理由见那里的注释）。
      uvcpp_http_response resp =
          uvcpp_http_response::make(http_status::PAYLOAD_TOO_LARGE,
                                    "413 Payload Too Large",
                                    sizeof("413 Payload Too Large") - 1);
      send_h2_response(client, st.stream_id, resp);
      return;
    }

    uvcpp_http_request req = st.request;
    req.stream_id = st.stream_id;
    if (!body.empty()) req.body = uvcpp_buf(body.data(), body.size());
    dispatch_h2_request(client, st.stream_id, req);
  };
  h2c.on_close = [this, client](uvcpp_h2_session&, int32_t sid, uint32_t) {
    std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
    auto cit = tbl.find(client);
    if (cit != tbl.end()) cit->second.h2_streams.erase(sid);
  };

  uvcpp_h2_connection::callbacks cc;
  cc.on_disconnect = [](uvcpp_h2_connection&) {};

  // 对端断开 → 框架的关闭回调 → remove_ctx；主动关走 h2 层的 close()，同样
  // 汇到那里。两条路都负责把 h2 层删掉。
  client->set_on_close([this, client]() { remove_ctx(client); });

  const int rv = h2->start(h2c, cc);
  if (rv != 0) {
    std::fprintf(stderr, "[uvcpp_http_server] h2 start failed: %d\n", rv);
    h2->close_now();
  }
}

void uvcpp_http_server::switch_to_h2c(uvcpp_tcp_client* client,
                                      const std::string& pending) {
  // 一条明文连接在**已经按 h1 登记、已经起了 h1 的读**之后改判成 h2。三件事必须
  // 按这个次序做，缺一件都留下一个静默的坏状态：
  //
  // 1) 摘掉 h1 那份 `conn_ctx`（含 llhttp 解析器）。`on_tcp_connection_h2()` 会
  //    自己登记一份新的，不先摘就是**把它盖掉** —— 那份解析器当场泄漏，而它每个
  //    连接一个，跑上一天就是稳定的内存增长。
  // 2) 卸掉 h1 那条读回调。**必须排在重建之前**：`read_start_events()` 对"已经
  //    注册过读"的连接返回 `UV_EALREADY`（判据在 `read_start()` 里），而 h2 层的
  //    `start()` 正是靠它登记的 —— 不卸的话 h2 层起不来，连接静默变成"收了字节
  //    什么都不做"。
  //    这里**丢不掉字节**：我们此刻正跑在那个读回调里（调用链是
  //    `on_connection_data` ← 它），同步地卸掉再装上，中间没有一次循环迭代；
  //    而本段字节已经在 `pending` 里了。
  // 3) 建 h2 会话，再把 `pending`（前言 + 同段跟来的帧）喂给它。喂在 `start()`
  //    **之后**：那时读已经被重新 arm 上，但新的一次读回调只可能在下一轮循环里
  //    跑，而这里是同步的 —— 所以"先喂 pending、再收到新字节"这个次序是确定的，
  //    不是靠运气。
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  std::map<uvcpp_tcp_client*, conn_ctx>::iterator it = tbl.find(client);
  if (it == tbl.end()) return;  // 原始数据钩子里已经把连接关了
  uvcpp_http_parser* parser = it->second.parser;
  tbl.erase(it);
  // 摘了旧的、下面 `on_tcp_connection_h2()` 立刻登记新的（同一句同步代码里），
  // 中间没有任何回调能观察到这个空隙 —— 与表这边的次序逐字一致。
  forget_ctx_generation(client);
  delete parser;

  client->read_stop();
  // 钩子**不重跑**：这条连接按 h1 登记时已经跑过一次（见那里的注释）。
  on_tcp_connection_h2(client, /*run_connection_hook=*/false);

  std::map<uvcpp_tcp_client*, conn_ctx>& tbl2 = ctxs_of(client);
  std::map<uvcpp_tcp_client*, conn_ctx>::iterator it2 = tbl2.find(client);
  if (it2 == tbl2.end()) return;  // 钩子里/start 失败时已经把连接关了
  uvcpp_h2_connection* h2 = it2->second.h2;
  if (h2 == nullptr || pending.empty()) return;
  h2->feed(pending.data(), pending.size());
}

void uvcpp_http_server::dispatch_h2_request(uvcpp_tcp_client* client,
                                            int32_t stream_id,
                                            uvcpp_http_request& req) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end()) return;
  conn_ctx& ctx = it->second;

  auto sit = ctx.h2_streams.find(stream_id);
  if (sit == ctx.h2_streams.end()) return;
  // 按**这条流**记，不写连接级字段 —— 理由见 h2_stream_state 的说明。
  sit->second.accept_encoding = http_get_header(req.headers, "accept-encoding");
  sit->second.is_head = (req.method == http_method::HTTP_HEAD);

  // 「稍后回话」凭据（见 `uvcpp_http_reply`）：h2 上它比 h1 多一样东西 ——
  // 流号。一条 h2 连接上并发跑着多条流，处理函数推迟应答时"我替哪条流回话"
  // 全靠它，而推迟那条路上 `resp.stream_id` 可能已经被整对象替换冲掉了。
  fill_reply(req.reply, client, ctx, stream_id);

  uvcpp_http_response resp;
  resp.stream_id = stream_id;

  // 与 h1 那条路同一个路由表、同一个兜底处理函数 —— h2 换的是**传输**，
  // 不是"用哪个处理函数"。webapp 就是靠这个兜底处理函数接进来的。
  auto handler = find_handler(req.method, req.url);
  if (handler) {
    handler(req, resp, client);
  } else {
    resp = uvcpp_http_response::not_found();
    resp.stream_id = stream_id;
  }

  if (resp.deferred) return;  // 处理函数稍后自己调 send_response
  send_h2_response(client, stream_id, resp);
}

int uvcpp_http_server::send_h2_response(uvcpp_tcp_client* client,
                                        int32_t stream_id,
                                        uvcpp_http_response& resp) {
  std::map<uvcpp_tcp_client*, conn_ctx>& tbl = ctxs_of(client);
  auto it = tbl.find(client);
  if (it == tbl.end() || it->second.h2 == nullptr) return UV_EINVAL;
  conn_ctx& ctx = it->second;

  // h2 的**主派发路径**（`on_request_end` 里那句 `send_h2_response`）与 413
  // 那一条都直接调本函数、整条绕过 `send_response`，所以这一处不是保险而是必需。
  // 重复调用是空操作 —— `http_ensure_date` 先查有没有。
  http_ensure_date(resp);

  auto sit = ctx.h2_streams.find(stream_id);
  if (sit == ctx.h2_streams.end()) return UV_EINVAL;
  // 先取到局部量：`send_response` 会同步跑回调，回调可以关掉流甚至关掉连接，
  // 那之后 `sit` 就是悬垂引用了。
  const bool head = sit->second.is_head;

  // 压缩是**协议无关**的：h1 那条路做的事这里一件不能少，否则同一个处理函数
  // 在 h2 下会拿到未压缩的响应体。
  //
  // 但 `apply_compression` 读的是**连接级**的 `is_head` / `accept_encoding`
  // （那是 h1 的形状：一条连接同一时刻只有一个请求）。h2 上并发流各说各话，
  // 所以把这条流的值临时借过去 —— 本函数同步跑完，中途没有第二方读这两个字段。
  ctx.is_head         = head;
  ctx.accept_encoding = sit->second.accept_encoding;
  apply_compression(ctx, resp);

  if (head) {
    // HEAD 在 h2 里是"HEADERS 带 END_STREAM、一个 DATA 帧都不发"，但
    // `content-length` 仍按 GET 会有多长写 —— 所以先钉长度再抑制 body。
    if (!resp.has_header("content-length") && resp.body.size() > 0) {
      resp.set_header("content-length",
                      std::to_string(static_cast<unsigned long long>(
                          resp.body.size())));
    }
  } else if (!resp.has_header("content-length") && resp.body.size() == 0) {
    // h2 没有 chunked，空 body 靠 content-length: 0 说明边界。
    resp.set_header("content-length", "0");
  }

  // 连接专属头（含 keep-alive 需要的那条 `connection`）由会话层统一剥掉 ——
  // 这里**不调** h1 那套 keep-alive 判定：h2 的连接是长命的，没有"这条消息
  // 之后要不要关连接"这回事。
  return ctx.h2->send_response(stream_id, resp, /*omit_body=*/head);
}

#endif  // UVCPP_NGHTTP2_ENABLE

#if UVCPP_HTTP3_ENABLE

void uvcpp_http_server::on_quic_connection(uvcpp_quic_connection* conn) {
  if (conn == nullptr) return;
  // 一条 QUIC 连接只建一次 h3 层。这个回调是"**首包到达**"那一刻跑的（握手
  // 还没完）—— 正是 `uvcpp_quic_server::listen()` 那条约定的时机：等握手完成
  // 再装，握手期间到达的流数据就全丢了（首包之后紧接着就是流帧）。
  //
  // **表按当前线程取**：这个回调是某条 QUIC 端点的 `listen` 回调，端点是按循环
  // 建的（`listen_quic()`），所以它只会在**那条端点自己的循环线程**上跑 —— 与
  // 连接以后全部 h3 回调同一个线程。取表的迭代器只与这张表自己的 `end()` 比。
  std::map<uvcpp_quic_connection*, quic_ctx>& tbl = quic_ctxs_here();
  if (tbl.find(conn) != tbl.end()) return;
  const int li = quic_loop_index_here();

  uvcpp_h3_connection* h3 = new uvcpp_h3_connection(conn, /*server_side=*/true);
  // **先登记、再 start**：`start()` 会同步冲一轮待发字节（三条关键流），那一轮
  // 随时可能把对端触发的回调跑起来 —— 那时表里必须已经有这一项，否则一个立刻
  // 到达的请求会在一张空表上查不到连接（被静默丢掉）。
  quic_ctx& qc = tbl[conn];
  qc.h3 = h3;
  // 代次用**同一个**计数器（`conn_ctx::generation` 那个）：两条腿各一套号会让
  // "这个号是哪条腿的"变成一个新的歧义，而它要回答的问题一模一样。
  qc.generation = ++next_generation_;
  // 循环在 `listen_quic` 建端点时定下来，这里只是取回来 —— h3 的回调只可能在
  // 它自己那条端点的循环线程上跑，所以这一句读到的就是对的（见 `quic_ctx::loop`）。
  if (static_cast<size_t>(li) < quic_servers_.size() &&
      quic_servers_[static_cast<size_t>(li)] != nullptr) {
    qc.loop = quic_servers_[static_cast<size_t>(li)]->get_loop();
  }
  // 计数与登记同步（同一条线程，同一个临界区之外也没有并发读者会看到"登记了但
  // 没计数"——读数那两格只是给人看的近似值，见头文件）。
  {
    std::lock_guard<std::mutex> lk(quic_conn_counts_mu_);
    if (static_cast<size_t>(li) < quic_conn_counts_.size()) {
      ++quic_conn_counts_[static_cast<size_t>(li)];
    }
  }

  uvcpp_h3_connection::callbacks cbs;
  cbs.on_request = [this, conn](uvcpp_h3_connection&, const h3_request& h3req) {
    // 回调**先查表**：上一条请求的处理过程中可能已经把连接收了尾
    // （`on_disconnect` 里表项就摘掉了），此后这个闭包再进来必须什么都不做。
    // 表还是按当前线程取 —— 与登记那一句同一条线程、同一条端点。
    std::map<uvcpp_quic_connection*, quic_ctx>& t = quic_ctxs_here();
    std::map<uvcpp_quic_connection*, quic_ctx>::iterator it = t.find(conn);
    if (it == t.end() || it->second.h3 == nullptr) return;

    // 转成 web 层那份请求对象。转换在这一层做，为的是让 `h3_request` 不出现在
    // `uvcpp_http_server` 的声明里（公开头只认 web 自己的类型）。
    uvcpp_http_request req;
    if (!h3_to_web_request(h3req, req)) {
      // 畸形请求（`method` 认不出来，或 `:authority` 与 `host` 对不上）：
      // **在它自己那条流上**回 400、连接留着 —— 与 h1 那条路上 llhttp 判畸形
      // 时同一种处置。这里**不打 stderr**：那是客户端的错，不是本层的缺口
      // （下面 `dispatch_h3_request` 里那几处才是）。
      uvcpp_http_response bad =
          uvcpp_http_response::make(http_status::BAD_REQUEST);
      send_h3_response(conn, h3req.stream_id, bad);
      return;
    }
    dispatch_h3_request(conn, req);
  };
  // extended CONNECT（RFC 9220，#58）。h3 层把这类请求从 `on_request` 里摘出来
  // 单独交付 —— 它的读方向永远不会收尾，等"完整请求"就是永远等不到。
  cbs.on_connect_request = [this, conn](uvcpp_h3_connection& h3,
                                        const h3_request& h3req) {
    std::map<uvcpp_quic_connection*, quic_ctx>& t = quic_ctxs_here();
    std::map<uvcpp_quic_connection*, quic_ctx>::iterator it = t.find(conn);
    if (it == t.end() || it->second.h3 == nullptr) return;

    uvcpp_http_request req;
    if (!h3_to_web_request(h3req, req)) {
      uvcpp_http_response bad =
          uvcpp_http_response::make(http_status::BAD_REQUEST);
      send_h3_response(conn, h3req.stream_id, bad);
      return;
    }
    if (h3_extended_connect_handler_ &&
        h3_extended_connect_handler_(h3, req)) {
      return;  // 已接管
    }
    uvcpp_http_response resp =
        uvcpp_http_response::make(http_status::NOT_IMPLEMENTED);
    send_h3_response(conn, h3req.stream_id, resp);
  };
  cbs.on_stream_close = [this, conn](uvcpp_h3_connection&,
                                     const h3_stream_close_info& info) {
    // 流收场 ⇒ 它的那格状态也收场。**按流**：一条连接上同时跑着多条流，删错了
    // 就是把另一条流的 `is_head` 抹掉。连接已经摘表了（`on_disconnect` 先跑）
    // 就什么都不做 —— 那一整格连同表项一起没了。
    std::map<uvcpp_quic_connection*, quic_ctx>& t = quic_ctxs_here();
    std::map<uvcpp_quic_connection*, quic_ctx>::iterator it = t.find(conn);
    if (it != t.end()) it->second.h3_streams.erase(info.stream_id);
  };
  cbs.on_error = [](uvcpp_h3_connection&, int code) {
    // h3 层收到致命错误之后**会自己把 QUIC 连接关掉**（那是唯一正确的动作，
    // 见 `callbacks::on_error` 的说明），所以这里只记日志 —— 不留一条线索引，
    // 运维看到的就是"连接莫名其妙没了"。
    std::fprintf(stderr,
                 "[uvcpp_http_server] h3 致命错误：%d（连接已由 h3 层收尾）\n",
                 code);
  };
  cbs.on_disconnect = [this, conn](uvcpp_h3_connection&) { remove_h3_ctx(conn); };

  const int rv = h3->start(cbs);
  if (rv != 0) {
    std::fprintf(stderr, "[uvcpp_http_server] h3 start() 失败：%d\n", rv);
    // 这一层已经用不了了。**次序**：先摘表（那一步把 h3 层删掉），再关 QUIC
    // 连接 —— 反了的话会在"还认得这条连接"的表里留下一个悬垂的 `h3`。
    remove_h3_ctx(conn);
    conn->close(static_cast<int>(H3_INTERNAL_ERROR));
  }
}

void uvcpp_http_server::dispatch_h3_request(uvcpp_quic_connection* conn,
                                            uvcpp_http_request& req) {
  // 流号在这一句取好：处理函数**整对象替换** `resp` 时那一格会被冲成 0
  // （h2 那边同一个理由），而本函数一路都要用它。
  const int64_t stream_id = req.stream_id;

  // 这条流的状态就在这一句建立：**推迟应答**要在响应发出去那一刻回来读它，而
  // 那时手上只有连接与流号（见 `quic_ctx::h3_streams`）。建立点必须在所有会
  // 发响应的分支**之前**，否则推迟那一条自己就成了唯一读不到状态的人。
  std::map<uvcpp_quic_connection*, quic_ctx>& tbl = quic_ctxs_here();
  std::map<uvcpp_quic_connection*, quic_ctx>::iterator cit = tbl.find(conn);
  if (cit == tbl.end() || cit->second.h3 == nullptr) return;  // 连接已收尾
  {
    quic_ctx::h3_stream_state& ss = cit->second.h3_streams[stream_id];
    ss.accept_encoding = http_get_header(req.headers, "accept-encoding");
    ss.is_head = (req.method == http_method::HTTP_HEAD);
  }
  // 「稍后回话」的凭据 —— h3 上它是处理函数**唯一**能拿到的那一枚（那条腿上
  // 第三个形参恒为 nullptr）。在跑处理函数之前填好。
  fill_h3_reply(req.reply, conn, cit->second, stream_id);

  // 这一批没做的那两样，**显式拒绝**而不是静默错路由。两条都写在分派**之前**：
  // 它们是服务端**配置**层面的条件，撞上时用户的处理函数根本不该被跑起来 ——
  // 跑起来它就会收到一个自己没打算接的请求，然后回一个"看着正常"的响应。
  const bool refuse_claim = has_stream_claim();
  const bool refuse_stream =
      !refuse_claim &&
      static_cast<bool>(find_stream_handler(req.method, req.url));
  if (refuse_claim || refuse_stream) {
    std::fprintf(stderr,
                 "[uvcpp_http_server] h3 上不支持 %s：这条腿上只有"
                 "「整份请求收全之后交给处理函数」那一套\n",
                 refuse_claim ? "set_stream_claim() 装的认领钩子"
                              : "post_stream() 注册的流式路由");
    uvcpp_http_response gap =
        uvcpp_http_response::make(http_status::INTERNAL_SERVER_ERROR);
    send_h3_response(conn, stream_id, gap);
    return;
  }

  uvcpp_http_response resp;
  // 与 `dispatch_h2_request` 逐字同形：**只**带流号，不带版本号。版本不在这条
  // 腿上传递 —— 客户端看到的 HVER_30 是它自己的 h3 层写的（`h3_response_not_received`），
  // 本函数写下的任何版本都到不了网上。流号是另一回事：处理函数靠它对号入座。
  resp.stream_id = static_cast<int32_t>(stream_id);

  // 与 h1/h2 **同一张**路由表、同一个兜底处理函数：换的是传输，不是"用哪个
  // 处理函数"。webapp 就是靠兜底处理函数接进来的。
  auto handler = find_handler(req.method, req.url);
  if (handler) {
    // 第三个参数 `uvcpp_tcp_client*` 在 h3 上**没有对应物** ⇒ 传 nullptr。
    // 这条腿上的连接句柄是 `req.reply`（推迟应答要用的那一枚），不是它 —— 需要
    // 连接级动作（关闭、原始写）的处理函数在 h3 下拿不到 `client`，只能按
    // `req.reply.version()` 分辨自己在哪条腿上。
    handler(req, resp, nullptr);
  } else {
    resp = uvcpp_http_response::not_found();
    // `not_found()` 造出来的是 h1 形状的（版本 HVER_11、流号 0），而**这里不用
    // 补**：上面那句已经带了本条流的真流号，而版本根本不在这条腿上传递（见上）。
    // 曾经在这里把两格都钉过一遍 —— 实测是死代码（反空转表 P5-5：拿掉之后
    // 用例一条都不红），于是删掉，只留下这条注释说明它为什么不需要在。
  }

  if (resp.deferred) {
    // 推迟应答：处理函数**自己**负责把响应送出去，入口是 `req.reply` 与
    // `send_reply()`（h1/h2 上是 `client` 与 `send_response()`）。这里必须
    // 什么都不做 —— 发了就是同一个请求两份响应。
    return;
  }

  send_h3_response(conn, stream_id, resp);
}

int uvcpp_http_server::send_h3_response(uvcpp_quic_connection* conn,
                                        int64_t stream_id,
                                        uvcpp_http_response& resp) {
  std::map<uvcpp_quic_connection*, quic_ctx>& t = quic_ctxs_here();
  std::map<uvcpp_quic_connection*, quic_ctx>::iterator it = t.find(conn);
  if (it == t.end() || it->second.h3 == nullptr) return UV_EINVAL;
  uvcpp_h3_connection* h3 = it->second.h3;

  // `accept_encoding` / `is_head` 是**按流**取的，取自派发那一刻登记的那一格
  // （`quic_ctx::h3_streams`）。取不到（畸形请求的 400、流已经收场）就按
  // "没有压缩、不是 HEAD" 处理 —— 那正是这两样的默认值。
  std::string accept_encoding;
  bool is_head = false;
  {
    std::map<int64_t, quic_ctx::h3_stream_state>::iterator sit =
        it->second.h3_streams.find(stream_id);
    if (sit != it->second.h3_streams.end()) {
      accept_encoding = sit->second.accept_encoding;
      is_head = sit->second.is_head;
    }
  }

  // 与 `send_h2_response` 逐条相同：h3 也是一条**绕过** `send_response` 的独立
  // 应答路径（h1 那个入口要 `uvcpp_tcp_client*`），所以下面这几件一件都不能少
  // —— 少了 `date` 就是三条传输上三种响应形状。
  http_ensure_date(resp);

  // 压缩是协议无关的，而且在这条腿上是**按流取值**：h3 一条连接上同样并发跑着
  // 多条流，编码串在分派那一刻就从这条请求自己的头里取出来、随流登记下来了
  // （h2 那边是临时借进连接级单槽，h3 连可借的槽都没有 —— 于是有了
  // `apply_compression_for`）。
  apply_compression_for(accept_encoding, resp);

  if (is_head) {
    // HEAD 在 h3 里是"头块带 END_STREAM、一个 DATA 帧都不发"，但
    // `content-length` 仍按 GET 会有多长写 —— 先钉长度、再抑制 body。
    if (!resp.has_header("content-length") && resp.body.size() > 0) {
      resp.set_header("content-length",
                      std::to_string(static_cast<unsigned long long>(
                          resp.body.size())));
    }
  } else if (!resp.has_header("content-length") && resp.body.size() == 0) {
    // 空体靠 `content-length: 0` 说明边界（h3 里没有 chunked 这回事）。
    resp.set_header("content-length", "0");
  }

  h3_response out;
  // 身份在**网上**走的那一格是它（`h3_response::stream_id`），不是 `resp` 上
  // 那些—— `resp` 到这里已经读完了，谁都不会再看它一眼。`send_h2_response`
  // 同样不在 `resp` 上钉这两格，理由一致。
  out.stream_id = stream_id;
  out.status    = static_cast<int>(resp.status_code);
  out.headers.reserve(resp.headers.size());
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    h3_header h;
    h.name  = resp.headers[i].name;
    h.value = resp.headers[i].value;
    out.headers.push_back(h);
  }
  out.body = resp.body.to_string();

  out.trailers.reserve(resp.trailers.size());
  for (size_t i = 0; i < resp.trailers.size(); ++i) {
    h3_header h;
    h.name  = resp.trailers[i].name;
    h.value = resp.trailers[i].value;
    out.trailers.push_back(h);
  }

  // 连接专属头（含 h1 那套 keep-alive 要用的 `connection`）由**会话层**统一
  // 剥掉 —— 这里不判 keep-alive：h3 的连接是整个 QUIC 连接，没有"这条消息之后
  // 要不要关连接"这回事。
  // `out.trailers` 由 `send_response()` 在抽干 body reader **之前**提交 ——
  // 那里是唯一还来得及的时机（见它的注释）。
  return h3->send_response(out, /*omit_body=*/is_head);
}

void uvcpp_http_server::remove_h3_ctx(uvcpp_quic_connection* conn) {
  std::map<uvcpp_quic_connection*, quic_ctx>& t = quic_ctxs_here();
  std::map<uvcpp_quic_connection*, quic_ctx>::iterator it = t.find(conn);
  if (it == t.end()) return;  // 幂等：on_disconnect 与析构都可能会来
  // 序号在**确认有这一项之后**才求：它要走 abort 那条判据（多循环 + 不在循环
  // 线程上），而一次"已经摘过了"的幂等重入不该因此崩掉。
  const int li = quic_loop_index_here();
  uvcpp_h3_connection* h3 = it->second.h3;
  it->second.h3 = nullptr;
  // **先摘表、再删对象**（与 `remove_ctx` 同一个次序）：删对象会跑它自己的
  // 收尾，而那一步可能回到本函数；表还在的话那次重入就会再删一次。
  t.erase(it);
  {
    std::lock_guard<std::mutex> lk(quic_conn_counts_mu_);
    if (static_cast<size_t>(li) < quic_conn_counts_.size() &&
        quic_conn_counts_[static_cast<size_t>(li)] > 0) {
      --quic_conn_counts_[static_cast<size_t>(li)];
    }
  }
  delete h3;
}

#endif  // UVCPP_HTTP3_ENABLE

void uvcpp_http_server::set_max_body_size(size_t max_bytes) {
  max_body_size_ = max_bytes;
}

size_t uvcpp_http_server::max_body_size() const { return max_body_size_; }

void uvcpp_http_server::set_max_header_bytes(size_t max_bytes) {
  max_header_bytes_ = max_bytes;
}

size_t uvcpp_http_server::max_header_bytes() const { return max_header_bytes_; }

void uvcpp_http_server::set_max_url_bytes(size_t max_bytes) {
  max_url_bytes_ = max_bytes;
}

size_t uvcpp_http_server::max_url_bytes() const { return max_url_bytes_; }

void uvcpp_http_server::set_raw_data_hook(http_raw_data_hook hook) {
  raw_data_hook_ = std::move(hook);
}

void uvcpp_http_server::clear_raw_data_hook() { raw_data_hook_ = nullptr; }

bool uvcpp_http_server::has_raw_data_hook() const {
  return static_cast<bool>(raw_data_hook_);
}

void uvcpp_http_server::set_stream_claim(http_stream_claim hook) {
  stream_claim_ = std::move(hook);
}

void uvcpp_http_server::clear_stream_claim() { stream_claim_ = nullptr; }

bool uvcpp_http_server::has_stream_claim() const {
  return static_cast<bool>(stream_claim_);
}

void uvcpp_http_server::on_connection_close(
    std::function<void(uvcpp_tcp_client*)> cb) {
  close_handler_ = std::move(cb);
}

void uvcpp_http_server::on_connection(
    std::function<void(uvcpp_tcp_client*)> cb) {
  connection_handler_ = std::move(cb);
}

int uvcpp_http_server::run(uv_run_mode md) { return tcp_server_->run(md); }
void uvcpp_http_server::stop(std::function<void()> on_stopped) {
  status_ = HTTP_SERVER_STOPPING;
  tcp_server_->stop([this, on_stopped]() {
    status_ = HTTP_SERVER_STOPPED;
#if UVCPP_HTTP3_ENABLE
    // TCP 与 0 号 QUIC 端点**共用 0 号循环**（见 `listen_quic()`），所以这里收完
    // TCP 那条腿再停循环。次序反了的话，TCP 那侧异步关闭的收尾回调再也不会跑
    // —— 而它正是我们这个 lambda 本身。
    //
    // 这一句对"**只**开了 h3"的服务端是**必需**的：`uvcpp_tcp_server::stop()`
    // 在没在 listen 时是同步早返回的（调用方还没 `listen()` 就是这个形状），
    // 于是不补这一句就没人去停循环，`run()` 永远不回来。
    //
    // **只停 0 号**：1..n−1 号端点是各自那条工作循环上的 listener，它们的循环由
    // `uvcpp_tcp_server::stop_workers()` 停 —— `uvcpp_quic_server::stop()` 是
    // `loop->stop()`，在工作循环上调它会让 `uv_run` 提前返回、绕过那边
    // `stopping_` 的闸（`uvcpp_quic_server::stop()` 的注释是权威）。所以这一段
    // 与扇出与否无关，一个字都不用跟着 n 变。
    if (!quic_servers_.empty() && quic_servers_[0] != nullptr) {
      quic_servers_[0]->stop();
    }
#endif
    if (on_stopped) on_stopped();
  });
}
int uvcpp_http_server::get_status() const { return status_; }
bool uvcpp_http_server::has_status(int flags) const { return (status_ & flags) == flags; }
uvcpp_tcp_server* uvcpp_http_server::get_tcp_server() { return tcp_server_; }

#if UVCPP_HTTP_COMPRESS_ENABLE
void uvcpp_http_server::set_compression_enabled(bool enable) {
  compress_enabled_ = enable;
}
bool uvcpp_http_server::is_compression_enabled() const {
  return compress_enabled_;
}
void uvcpp_http_server::set_compress_min_body_size(size_t min_size) {
  compress_min_body_ = min_size;
}
void uvcpp_http_server::set_compress_excluded_types(
    const std::vector<std::string>& types) {
  compress_excluded_types_ = types;
}
void uvcpp_http_server::add_compress_excluded_type(const std::string& mime_type) {
  compress_excluded_types_.push_back(mime_type);
}
void uvcpp_http_server::set_compress_level(int level) {
  // 存**夹过的**值：键里那个档位字符靠"不同档位不同字符"才成立（见下面
  // `compress_level_key_char`），而未夹紧的 15 与 5 会落到同一个字符上。
  compress_level_.store(http_compress::clamp_compress_level(level),
                        std::memory_order_relaxed);
}
int uvcpp_http_server::compress_level() const {
  return compress_level_.load(std::memory_order_relaxed);
}
#endif

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE
