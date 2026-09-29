/**
 * @file src/http3/uvcpp_h3_session.cpp
 * @brief h3 会话实现。nghttp3 只出现在这个文件里（这一层不碰 socket/libuv/QUIC）。
 * @author zhuweiye
 * @version 1.4.1
 */

#include "http3/uvcpp_h3_session.h"

#if UVCPP_HTTP3_ENABLE

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "http3/uvcpp_h3_nghttp3.h"

namespace uvcpp {

// `uvcpp_h3_common.h` 里那几个具名错误码的保险丝：值必须与 nghttp3 自己的宏
// 逐一对齐。这个 TU 是全仓唯一 include 了 nghttp3 的地方，所以断言只能放这儿。
//
// 这两条断言就是"不造平行表"那个决定的**执行机构**：`H3_NO_ERROR` 与
// `H3_EXCESSIVE_LOAD` 之所以敢在公开头里写成裸数字，是因为它们在这里被钉死在
// 上游的取数上。往 `uvcpp_h3_common.h` 里再加一个 h3 错误码而不在这里加一条
// 断言，等于把平行表从明处搬到暗处。
static_assert(H3_NO_ERROR == NGHTTP3_H3_NO_ERROR, "h3 error code drift");
static_assert(H3_EXCESSIVE_LOAD == NGHTTP3_H3_EXCESSIVE_LOAD,
              "h3 error code drift");

namespace {

/**
 * @brief nghttp3 调用栈的深度守卫。
 *
 * 规矩只有一条：**`nghttp3_conn_read_stream2()` 与 `nghttp3_conn_writev_stream()`
 * 不许在 nghttp3 自己的回调里被调用**（上游文档原话是"calling nghttp3 API other
 * than nghttp3_conn_del causes undefined behavior"，而这两个正是被点名的）。
 * 而我们的回调会一路跑进用户代码，用户代码里"收到请求就答复"又天然会碰发方向 ——
 * 所以必须有东西替它挡住。
 *
 * 挡法：进 nghttp3 之前 `++`、出来 `--`；深度 > 0 时 `drain()` 直接返回 0。
 * 与 `uvcpp_h2_session` 那个 `in_nghttp2` 标志同形，差别只在**计数**而不是布尔：
 * h3 的回调会嵌套（`add_ack_offset` → `acked_stream_data` → 用户代码 → `submit_*`
 * → nghttp3 内部 → 又一次回调），布尔量会在内层退出时把外层也放行。
 *
 * 用 RAII 而不是手工置位：`submit_*()` 里那几次 `std::string` 分配会抛
 * `bad_alloc`，抛出时标记不能留在里面。
 */
struct cb_guard {
  int* depth;
  explicit cb_guard(int* d) : depth(d) { ++*depth; }
  ~cb_guard() { --*depth; }
  cb_guard(const cb_guard&)            = delete;
  cb_guard& operator=(const cb_guard&) = delete;
};

/// QPACK token → 具名种类。
///
/// **判据是 nghttp3 的具名常量，不是数字。** `__AUTHORITY` / `__METHOD` /
/// `__PATH` / `__SCHEME` / `__STATUS` 这五个在 v1.18.0 里是 0 / 1 / 8 / 9 / 11，
/// 但那是一张会随上游漂移的表（`nghttp3.h` 的 `nghttp3_qpack_token` 里还有
/// 四十来个静态表条目，看得见它在长）。写数字的话，上游插一个 token 进来就静默
/// 错位 —— 而错位的症状恰好是"`:method` 变成了普通头"，一条用例绿灯通过。
h3_header_kind kind_of_token(int32_t token) {
  switch (token) {
    case NGHTTP3_QPACK_TOKEN__METHOD:    return h3_header_kind::METHOD;
    case NGHTTP3_QPACK_TOKEN__SCHEME:    return h3_header_kind::SCHEME;
    case NGHTTP3_QPACK_TOKEN__PATH:      return h3_header_kind::PATH;
    case NGHTTP3_QPACK_TOKEN__STATUS:    return h3_header_kind::STATUS;
    case NGHTTP3_QPACK_TOKEN__AUTHORITY: return h3_header_kind::AUTHORITY;
    default:                             return h3_header_kind::REGULAR;
  }
}

/// `nghttp3_rcbuf` → `std::string`，**当场拷**。
///
/// 上游契约写得很死：`recv_header` 拿到的两个 `nghttp3_rcbuf*` 只在该回调期间
/// 有效，想留就调 `nghttp3_rcbuf_incref` 自己管引用计数。我们选择拷一份而不是
/// incref —— 交出去的 `h3_header` 因此可以在回调返回之后照样用，调用方（web 层
/// 的 handler）也就不需要知道 nghttp3 的引用计数存在过。头字段都小，一次分配
/// 换掉的是一整套跨回调的生命周期纪律。
std::string rcbuf_to_string(const nghttp3_rcbuf* b) {
  const nghttp3_vec v = nghttp3_rcbuf_get_buf(b);
  return std::string(reinterpret_cast<const char*>(v.base), v.len);
}

}  // namespace

// =========================================================================
// 构造 / 析构 / 初始化
// =========================================================================

uvcpp_h3_session::uvcpp_h3_session(bool server_side)
    : server_side_(server_side) {}

uvcpp_h3_session::~uvcpp_h3_session() {
  if (conn_ != nullptr) nghttp3_conn_del(conn_);
}

int uvcpp_h3_session::init(const callbacks& cbs,
                           uint64_t max_field_section_size,
                           size_t qpack_max_dtable_capacity,
                           size_t qpack_blocked_streams) {
  if (conn_ != nullptr) return UV_EALREADY;

  cbs_                    = cbs;
  max_field_section_size_ = max_field_section_size;

  // 回调表**按字段名逐格赋值**。不用 `nghttp3_callbacks{...}` 的位置初始化：
  // 上游每加一个回调都是**追加在尾部**的（`nghttp3.h` 里 `/* The following
  // fields have been added since NGHTTP3_CALLBACKS_Vn. */` 那几段就是证据），
  // 而位置初始化在追加之后会静默把后面的每一条都挪错一格 —— 编译期一声不响，
  // 运行期表现为"某个回调跑的是另一个回调的代码"。逐格赋值最坏只是"新加的回调
  // 我们没填"，那是一个**看得见**的缺失。
  //
  // `= {}` 先把整张表清零：没填的格子必须是 NULL（nghttp3 对可选回调就是这么
  // 判的），而不是栈上的垃圾。
  nghttp3_callbacks ncb = {};
  ncb.acked_stream_data = &uvcpp_h3_session::cb_acked_stream_data;
  ncb.recv_data         = &uvcpp_h3_session::cb_recv_data;
  ncb.begin_headers     = &uvcpp_h3_session::cb_begin_headers;
  ncb.recv_header       = &uvcpp_h3_session::cb_recv_header;
  ncb.end_headers       = &uvcpp_h3_session::cb_end_headers;
  ncb.stop_sending      = &uvcpp_h3_session::cb_stop_sending;
  ncb.end_stream        = &uvcpp_h3_session::cb_end_stream;
  ncb.reset_stream      = &uvcpp_h3_session::cb_reset_stream;
  ncb.stream_close2     = &uvcpp_h3_session::cb_stream_close2;

  // 刻意**留空**的几格，都是有名字的取舍，不是忘了填：
  //
  // - `stream_close`：V4 之前的老形状，只带一个错误码，分不出两个方向。
  //   填了它反而会在上游把 `stream_close2` 也设为非空时出现"两个都调"的疑虑；
  //   我们只要 `stream_close2`。
  // - `begin_trailers` / `recv_trailer` / `end_trailers`：**本批不处理 trailers**。
  //   留空而不是填一个"收到就忽略"的实现：忽略会让带 trailers 的响应在
  //   `end_stream` 之前多一个空的头块途经我们的路由判断（那份代码按"一次
  //   `end_headers` = 一次完整请求"写的），静默走错比显式不做更糟。
  // - `shutdown`：对端发 GOAWAY 的通知。本批不发也不收 GOAWAY（见 doc 里
  //   "没做的"那张表），所以这一格没有消费者。
  // - `recv_settings` / `recv_settings2`：本层不因对端的 SETTINGS 改自己的行为。
  //   唯一想读的是对端的 `max_field_section_size`，而那个值 nghttp3 压根不给
  //   应用看（可参照的是我们在**发**方向的自觉上限，见 `H3_MAX_SEND_HEADER_BLOCK`）。
  // - `deferred_consume`：只在"流之间互相阻塞、后来解开了"时报账。我们的 QUIC
  //   层在收到字节时就当场还过流控了（`cb_recv_stream_data`），这份账在我们这里
  //   没有下家（理由见 `recv_stream_data()` 的 `@warning`）。
  // - `recv_origin` / `end_origin`：ORIGIN 帧（RFC 9412）本批不做。
  // - `rand`：上游建议填（拿去加固对可疑对端的随机化）。**这一格留空是有代价的**，
  //   如实记在 doc 的"没做的"表里，不是"忘了"。
  ncb.deferred_consume = nullptr;

  nghttp3_settings settings;
  nghttp3_settings_default(&settings);
  // 只改这三个。其余保持上游默认 —— 抄一整份字面量会把 1.18.0 的默认值冻死，
  // 上游改默认值时我们这边静默跟着变的是**行为**，而我们读的却是**常量**。
  settings.max_field_section_size = max_field_section_size;
  settings.qpack_max_dtable_capacity = qpack_max_dtable_capacity;
  settings.qpack_blocked_streams     = qpack_blocked_streams;

  const int rv =
      server_side_
          ? nghttp3_conn_server_new(&conn_, &ncb, &settings,
                                    nghttp3_mem_default(), this)
          : nghttp3_conn_client_new(&conn_, &ncb, &settings,
                                    nghttp3_mem_default(), this);
  if (rv != 0) {
    conn_ = nullptr;
    return rv;
  }
  return 0;
}

// =========================================================================
// 收
// =========================================================================

int64_t uvcpp_h3_session::recv_stream_data(int64_t stream_id, const char* data,
                                           size_t len, bool fin, uint64_t ts) {
  if (conn_ == nullptr) return 0;
  // 已经致命之后**不再进 nghttp3**：`read_stream2` 一旦返回致命错误，按上游
  // 契约这条连接就只剩 `nghttp3_conn_del` 能调了。原样把它当时给的那个错误
  // 回给调用方，顺带这也是"只通知一次"能成立的前提。
  if (fatal_) return last_error_ != 0 ? last_error_ : -1;

  nghttp3_ssize rv;
  {
    cb_guard g(&in_callback_);
    rv = nghttp3_conn_read_stream2(
        conn_, stream_id, reinterpret_cast<const uint8_t*>(data), len,
        fin ? 1 : 0, static_cast<nghttp3_tstamp>(ts));
  }
  // 守卫**必须在报错之前就退掉**：`on_fatal` 的接收方要关连接，那一步会
  // `drain()`，而它正是被这个守卫挡着的那个函数。
  if (rv < 0) return mark_fatal(static_cast<int>(rv));
  return static_cast<int64_t>(rv);
}

// =========================================================================
// 发
// =========================================================================

int uvcpp_h3_session::drain(h3_out_chunk& out) {
  out.stream_id = -1;
  out.fin       = false;
  out.veccnt    = 0;
  out.total     = 0;

  if (conn_ == nullptr) return 0;
  // 重入保护。理由与"深度要计数而不是置位"见 `cb_guard`。返回 0（"暂时没得发"）
  // 而不是错误码：调用方**本来就要**在每次 `recv_stream_data()` 返回之后再冲
  // 一轮，所以这一句的语义是"这轮先到这儿"，不是失败。
  if (in_callback_ > 0) return 0;
  if (fatal_) return last_error_ != 0 ? last_error_ : -1;

  nghttp3_vec     vec[H3_MAX_WRITE_VECS];
  int64_t         sid = -1;
  int             fin = 0;
  nghttp3_ssize   ncnt;
  {
    cb_guard g(&in_callback_);
    ncnt = nghttp3_conn_writev_stream(conn_, &sid, &fin, vec,
                                      H3_MAX_WRITE_VECS);
  }
  if (ncnt < 0) return mark_fatal(static_cast<int>(ncnt));

  // 两种"抽干净"要分开：
  //   - `ncnt == 0 && !fin`：真的没得发（`writev_stream` 的文档原话是"returns 0,
  //     and -1 is assigned to *pstream_id"）。回 0。
  //   - `ncnt == 0 && fin && sid != -1`：**只有 FIN 的块**（文档原话："This
  //     function may return 0, and *pstream_id is not -1, and *pfin is nonzero"）。
  //     那是一次合法的待发 —— 不把它报出去，这条流的发送方向就永远关不上。
  if (ncnt == 0 && !fin) return 0;

  out.stream_id = sid;
  out.fin       = fin != 0;
  out.veccnt    = static_cast<size_t>(ncnt);
  for (size_t i = 0; i < out.veccnt; ++i) {
    out.base[i] = vec[i].base;
    out.len[i]  = vec[i].len;
    out.total += vec[i].len;
  }
  return 1;
}

int uvcpp_h3_session::add_write_offset(int64_t stream_id, size_t n) {
  if (conn_ == nullptr || fatal_) return 0;
  const int rv = nghttp3_conn_add_write_offset(conn_, stream_id, n);
  if (rv != 0) return mark_fatal(rv);
  return 0;
}

int uvcpp_h3_session::add_ack_offset(int64_t stream_id, uint64_t n) {
  if (conn_ == nullptr || fatal_) return 0;
  int rv;
  {
    // 这一句会**同步**回调进 `cb_acked_stream_data`（上游 `add_ack_offset` 里
    // 直接调过去），而那条路会跑到用户代码上 —— 用户代码里再叫一次 `drain()`
    // 同样得被挡住。所以它也套守卫。
    cb_guard g(&in_callback_);
    rv = nghttp3_conn_add_ack_offset(conn_, stream_id, n);
  }
  if (rv != 0) return mark_fatal(rv);
  return 0;
}

// =========================================================================
// 三条关键单向流与设置
// =========================================================================

int uvcpp_h3_session::bind_control_stream(int64_t stream_id) {
  if (conn_ == nullptr) return UV_EINVAL;
  if (fatal_) return last_error_ != 0 ? last_error_ : -1;
  return nghttp3_conn_bind_control_stream(conn_, stream_id);
}

int uvcpp_h3_session::bind_qpack_streams(int64_t qenc_stream_id,
                                         int64_t qdec_stream_id) {
  if (conn_ == nullptr) return UV_EINVAL;
  if (fatal_) return last_error_ != 0 ? last_error_ : -1;
  return nghttp3_conn_bind_qpack_streams(conn_, qenc_stream_id, qdec_stream_id);
}

void uvcpp_h3_session::set_max_client_streams_bidi(uint64_t max_streams) {
  if (conn_ == nullptr || fatal_) return;
  nghttp3_conn_set_max_client_streams_bidi(conn_, max_streams);
}

void uvcpp_h3_session::set_max_concurrent_streams(size_t n) {
  if (conn_ == nullptr || fatal_) return;
  nghttp3_conn_set_max_concurrent_streams(conn_, n);
}

// =========================================================================
// 提出请求 / 提交响应
// =========================================================================

int uvcpp_h3_session::submit(const std::vector<h3_header>& headers,
                             std::string& body, bool omit_body,
                             int64_t stream_id, bool request_side) {
  if (conn_ == nullptr) return UV_EINVAL;
  if (fatal_) return last_error_ != 0 ? last_error_ : -1;

  // 尺寸与合法性在**碰 nghttp3 之前**判完。理由与 h2 那边逐字相同：这是同步
  // 拒绝，流状态一个字都还没动，调用方拿到 `UV_EMSGSIZE` 时可以把这条流当没提过。
  size_t total = 0;
  std::vector<nghttp3_nv> nva;
  nva.reserve(headers.size());
  for (size_t i = 0; i < headers.size(); ++i) {
    const h3_header& h = headers[i];
    if (h.name.empty()) return UV_EINVAL;
    // RFC 9114 §4.2.2 的计法：`namelen + valuelen + 32`。与收方向的
    // `h3_header_budget` 是同一个数、同一个算法，这一点必须一致 —— 两边不一样
    // 就会出现"我们肯收 64 KiB 却只肯发 32 KiB"这种没法解释的不对称。
    total += h.name.size() + h.value.size() + 32;
    if (total > H3_MAX_SEND_HEADER_BLOCK) return UV_EMSGSIZE;

    nghttp3_nv nv;
    nv.name     = reinterpret_cast<const uint8_t*>(h.name.data());
    nv.value    = reinterpret_cast<const uint8_t*>(h.value.data());
    nv.namelen  = h.name.size();
    nv.valuelen = h.value.size();
    nv.flags    = NGHTTP3_NV_FLAG_NONE;
    nva.push_back(nv);
  }
  if (nva.empty()) return UV_EINVAL;

  // `submit_*` 会调 `nghttp3_nva_copy()` 把整个 nva **拷一份**（上游
  // `nghttp3_conn.c` 的 `conn_submit_headers_data` 第一件事就是它），所以 `nv`
  // 里那些指向 `headers` 的指针只要活到这次调用返回就够了 —— 传进来的
  // `const std::vector<h3_header>&` 满足这一条，不需要我们再复制一份字符串。
  //
  // `dr` 是**按值**存进 nghttp3 的帧队列的（同一个函数里的 `fr->data.dr = *dr`），
  // 所以这个栈上的 `dr` 出了作用域也没关系；真正承重的是那个函数指针，而它靠
  // `stream_id` 在我们自己的 `streams_` 里找回 body。
  nghttp3_data_reader dr;
  dr.read_data = &uvcpp_h3_session::cb_read_data;

  // 没有 body 时传 NULL 而不是"一个永远返回 0 + EOF 的 reader"：上游文档明写
  // `dr == NULL` 就等于"写方向到此为止"，那正是 HEAD / 204 / 304 要的语义
  // （只有头块，没有 DATA，发送方向随即关闭）。用一个空 reader 去表达同一件事，
  // 会在线上多出一个零长 DATA 帧的机会窗口，而那件事没有任何好处。
  const bool has_body = !omit_body && !body.empty();

  const int rv =
      request_side
          ? nghttp3_conn_submit_request(conn_, stream_id, nva.data(), nva.size(),
                                        has_body ? &dr : nullptr, nullptr)
          : nghttp3_conn_submit_response(conn_, stream_id, nva.data(), nva.size(),
                                        has_body ? &dr : nullptr);
  if (rv != 0) return rv;

  // 记在 nghttp3 收下之后：提交失败时不该在 `streams_` 里留下一条我们根本没提过的流。
  stream_rec& s = stream_of(stream_id);
  if (has_body) {
    s.out_body   = std::move(body);
    s.out_offset = 0;
  } else {
    s.out_body.clear();
    s.out_offset = 0;
  }
  if (s.state == h3_stream_state::OPEN) s.state = h3_stream_state::HEADERS_DONE;
  return 0;
}

int uvcpp_h3_session::submit_request(int64_t stream_id,
                                     const std::vector<h3_header>& headers,
                                     std::string body) {
  return submit(headers, body, false, stream_id, true);
}

int uvcpp_h3_session::submit_response(int64_t stream_id,
                                      const std::vector<h3_header>& headers,
                                      std::string body, bool omit_body) {
  return submit(headers, body, omit_body, stream_id, false);
}

int uvcpp_h3_session::submit_status(int64_t stream_id, int status,
                                    std::string body) {
  std::vector<h3_header> hs;
  h3_header h;
  h.name  = ":status";
  h.value = std::to_string(status);
  h.kind  = h3_header_kind::STATUS;
  hs.push_back(h);
  return submit_response(stream_id, hs, std::move(body), false);
}

// =========================================================================
// 收场
// =========================================================================

int uvcpp_h3_session::shutdown_stream_read(int64_t stream_id) {
  if (conn_ == nullptr || fatal_) return 0;
  const int rv = nghttp3_conn_shutdown_stream_read(conn_, stream_id);
  if (rv != 0) return mark_fatal(rv);
  return 0;
}

void uvcpp_h3_session::shutdown_stream_write(int64_t stream_id) {
  if (conn_ == nullptr || fatal_) return;
  nghttp3_conn_shutdown_stream_write(conn_, stream_id);
}

int uvcpp_h3_session::close_stream(const h3_stream_close_info& info) {
  if (conn_ == nullptr || fatal_) return 0;

  // 两个方向分开说，靠的是 nghttp3 自己的两个 flag —— 上游文档原话："If
  // NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET is set in flags,
  // rx_app_error_code is the QUIC application error code that shut down the
  // receiving side of the stream."
  //
  // 所以"那一侧是干净的"（对端发了 FIN、我们把要发的发完了）必须表达成
  // **不置位**，而不是"错误码写成 0"。这两件事的区别是有后果的：`stream_close2`
  // 回调把置位与否原样报回来，我们的 `h3_stream_close_info::rx_error` 就是它，
  // 而上面那张 web 层的表按那个布尔决定"请求收全了"还是"对端取消了"。
  uint32_t flags = NGHTTP3_STREAM_CLOSE_FLAG_NONE;
  if (info.rx_error) flags |= NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET;
  if (info.tx_error) flags |= NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET;

  int rv;
  {
    cb_guard g(&in_callback_);
    rv = nghttp3_conn_close_stream2(conn_, flags, info.stream_id,
                                    info.rx_app_error_code,
                                    info.tx_app_error_code);
  }

  // 流不在 nghttp3 那边了 —— 那**不是**错误。QUIC 的收尾通知可能比 nghttp3
  // 自己的收尾晚到（两个方向的 FIN/RESET 各来一次，第二次时 nghttp3 早已把
  // 这条流清了），所以这一格单独给一个码，让调用方能把它和"关错了流"分开。
  if (rv == NGHTTP3_ERR_STREAM_NOT_FOUND) {
    streams_.erase(info.stream_id);
    return UV_ENOENT;
  }
  if (rv != 0) return mark_fatal(rv);
  return 0;
}

// =========================================================================
// 查询
// =========================================================================

bool uvcpp_h3_session::in_nghttp3() const { return in_callback_ > 0; }

bool uvcpp_h3_session::fatal() const { return fatal_; }

int uvcpp_h3_session::last_error() const { return last_error_; }

uint64_t uvcpp_h3_session::quic_app_error_code() const {
  if (last_error_ == 0) return H3_NO_ERROR;
  return nghttp3_err_infer_quic_app_error_code(last_error_);
}

std::string uvcpp_h3_session::nghttp3_version() const {
  return std::string(h3_detail::version_literal());
}

std::string uvcpp_h3_session::error_string(int code) {
  // 这一句是本模块唯一**直接调进 libnghttp3 的函数**（其余调用都收在会话类里）。
  // 它的第二个用处是链接证据：少链了 `nghttp3_static` 时，缺的是这个符号，
  // 在链接期就红，而不是等某个错误路径跑起来才发现。
  const char* s = nghttp3_strerror(code);
  return s != nullptr ? std::string(s) : std::string();
}

h3_stream_state uvcpp_h3_session::stream_state(int64_t stream_id) const {
  std::map<int64_t, stream_rec>::const_iterator it = streams_.find(stream_id);
  if (it == streams_.end()) return h3_stream_state::CLOSED;
  return it->second.state;
}

size_t uvcpp_h3_session::stream_count() const { return streams_.size(); }

// =========================================================================
// 内部
// =========================================================================

uvcpp_h3_session::stream_rec& uvcpp_h3_session::stream_of(int64_t stream_id) {
  // `std::map` 的节点地址在插入别的键时不变，所以返回的引用不会因为后面又开了
  // 一条流而失效 —— 这一点是被依赖的（回调里拿到的引用会跨过用户代码）。
  return streams_[stream_id];
}

int uvcpp_h3_session::mark_fatal(int nghttp3_error) {
  last_error_ = nghttp3_error;
  if (!fatal_) {
    fatal_ = true;
    // 从这里往下**不许再碰任何成员**：接收方（h3 连接的 `on_fatal`）会去关掉
    // QUIC 连接，而那条路可能把本对象一起安排掉。`on_fatal` 也按值先取出来。
    std::function<void(uvcpp_h3_session&, int)> cb = cbs_.on_fatal;
    if (cb) cb(*this, nghttp3_error);
  }
  return nghttp3_error;
}

// =========================================================================
// 跳板
//
// 全部是 `static` 成员 + `conn_user_data → this`。做成**静态成员**而不是匿名
// 命名空间里的自由函数：`stream_rec` / `streams_` 都是私有的，外面那些函数连
// 名字都写不出来（要用就得把它们提成公开声明，那等于为了让转发函数好写而放宽
// 封装）。静态成员天然有这个访问权。
//
// 每个跳板第一件事都是套 `cb_guard` —— 包括那些"看起来不会回调回来"的。
// 判据不是"这个函数会不会回调"，而是"它是不是在 nghttp3 的栈上"，因为守卫的
// 另一边（`drain()`）判的正是这个。
// =========================================================================

int uvcpp_h3_session::cb_begin_headers(nghttp3_conn*, int64_t stream_id,
                                       void* ud, void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  stream_rec& s = self->stream_of(stream_id);
  s.state       = h3_stream_state::OPEN;
  // 每个头块都重置预算。trailers 会是"同一条流上的第二个头块"，那时也要重置 ——
  // 本批不处理 trailers（回调表里那三格留空），所以这条规矩此刻只有一个消费者，
  // 但写法上不留"第二个头块沿用第一个账"的坑。
  s.budget.reset(static_cast<size_t>(self->max_field_section_size_));
  s.budget_reported = false;

  if (self->cbs_.on_begin_headers) self->cbs_.on_begin_headers(*self, stream_id);
  return 0;
}

int uvcpp_h3_session::cb_recv_header(nghttp3_conn*, int64_t stream_id,
                                     int32_t token, nghttp3_rcbuf* name,
                                     nghttp3_rcbuf* value, uint8_t, void* ud,
                                     void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  // **先拷再判**。`rcbuf_to_string` 依赖的"缓冲区只在本回调期间有效"这件事，
  // 意味着拷贝必须在这里发生；等到累积判完再拷的话，超预算那一支就永远拿不到
  // 已经解出来的字符串了（诊断信息只剩一个字节数）。
  h3_header h;
  h.name  = rcbuf_to_string(name);
  h.value = rcbuf_to_string(value);
  h.kind  = kind_of_token(token);

  stream_rec& s = self->stream_of(stream_id);
  // 这条流已经报过超限了：不再累积、不再往上交。**不是**"继续解完再一起丢" ——
  // 那正是 QPACK bomb 想要的效果（我们替它扛完整张内存）。
  if (s.budget_reported) return 0;

  if (!s.budget.add(h.name.size(), h.value.size())) {
    s.budget_reported = true;
    if (self->cbs_.on_header_budget_exceeded) {
      self->cbs_.on_header_budget_exceeded(*self, stream_id, s.budget.used());
    }
    return 0;
  }

  if (self->cbs_.on_header) self->cbs_.on_header(*self, stream_id, h);
  return 0;
}

int uvcpp_h3_session::cb_end_headers(nghttp3_conn*, int64_t stream_id, int fin,
                                     void* ud, void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  stream_rec& s = self->stream_of(stream_id);
  if (s.state == h3_stream_state::OPEN) s.state = h3_stream_state::HEADERS_DONE;

  // 超限的流连"头块收全"都不报：调用方已经收到 `on_header_budget_exceeded`，
  // 那条通知的意思就是"这条请求当它不存在"。再报一次收全，等于给调用方两条
  // 互相矛盾的话（"作废"和"收好了"），而它会按后到的那条走。
  if (s.budget_reported) return 0;

  if (self->cbs_.on_end_headers) {
    self->cbs_.on_end_headers(*self, stream_id, fin != 0);
  }
  return 0;
}

int uvcpp_h3_session::cb_recv_data(nghttp3_conn*, int64_t stream_id,
                                   const uint8_t* data, size_t datalen, void* ud,
                                   void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  stream_rec& s = self->stream_of(stream_id);
  if (s.budget_reported) return 0;

  if (self->cbs_.on_body) {
    self->cbs_.on_body(*self, stream_id,
                       reinterpret_cast<const char*>(data), datalen);
  }
  return 0;
}

int uvcpp_h3_session::cb_end_stream(nghttp3_conn*, int64_t stream_id, void* ud,
                                    void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  stream_rec& s = self->stream_of(stream_id);
  if (s.state != h3_stream_state::CLOSED) s.state = h3_stream_state::RX_ENDED;
  if (s.budget_reported) return 0;

  if (self->cbs_.on_end_stream) self->cbs_.on_end_stream(*self, stream_id);
  return 0;
}

int uvcpp_h3_session::cb_stream_close2(nghttp3_conn*, uint32_t flags,
                                       int64_t stream_id,
                                       uint64_t rx_app_error_code,
                                       uint64_t tx_app_error_code, void* ud,
                                       void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  h3_stream_close_info info;
  info.stream_id = stream_id;
  info.rx_error =
      (flags & NGHTTP3_STREAM_CLOSE_FLAG_RX_APP_ERROR_CODE_SET) != 0;
  info.tx_error =
      (flags & NGHTTP3_STREAM_CLOSE_FLAG_TX_APP_ERROR_CODE_SET) != 0;
  info.rx_app_error_code = rx_app_error_code;
  info.tx_app_error_code = tx_app_error_code;

  // **记录先删再报**。nghttp3 不会再碰这条流了，而我们这边的账要在
  // `on_stream_close` 的接收方看来是"到此为止"：它可能在回调里就按流号去查
  // （`stream_state()` 得给 `CLOSED` 而不是残留的 `RX_ENDED`），也可能当场
  // 再 `close_stream()` 一次（那次会走到 `UV_ENOENT` 那一格，而不是把一个已经
  // 删掉的记录再删一遍）。
  self->streams_.erase(stream_id);

  if (self->cbs_.on_stream_close) {
    self->cbs_.on_stream_close(*self, info);
  }
  return 0;
}

int uvcpp_h3_session::cb_stop_sending(nghttp3_conn*, int64_t stream_id,
                                      uint64_t app_error_code, void* ud, void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  // 上游这一格是"**请你**替我发 STOP_SENDING"：nghttp3 自己发不出去，它只跟
  // 内存说话。真正把它送到线上的是 QUIC 的 `shutdown_stream_read()`，所以这里
  // 只是把请求转给调用方 —— 本层一个 QUIC 类型都不认识，这是分层的代价，也是
  // 分层的意义。
  if (self->cbs_.on_stop_sending) {
    self->cbs_.on_stop_sending(*self, stream_id, app_error_code);
  }
  return 0;
}

int uvcpp_h3_session::cb_reset_stream(nghttp3_conn*, int64_t stream_id,
                                      uint64_t app_error_code, void* ud, void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  if (self->cbs_.on_reset_stream) {
    self->cbs_.on_reset_stream(*self, stream_id, app_error_code);
  }
  return 0;
}

int uvcpp_h3_session::cb_acked_stream_data(nghttp3_conn*, int64_t stream_id,
                                           uint64_t datalen, void* ud, void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  if (self->cbs_.on_acked) self->cbs_.on_acked(*self, stream_id, datalen);
  return 0;
}

nghttp3_ssize uvcpp_h3_session::cb_read_data(nghttp3_conn*, int64_t stream_id,
                                             nghttp3_vec* vec, size_t veccnt,
                                             uint32_t* pflags, void* ud, void*) {
  uvcpp_h3_session* self = static_cast<uvcpp_h3_session*>(ud);
  cb_guard          g(&self->in_callback_);

  if (veccnt == 0) return 0;

  std::map<int64_t, stream_rec>::iterator it = self->streams_.find(stream_id);
  // 找不到流记录 = 我们自己把账弄丢了，那是**本层的 bug**，不是对端行为。
  // 回 `CALLBACK_FAILURE` 让它一路上到 `writev_stream` 的负值，被当成连接级
  // 致命处理 —— 比"返回 0 + EOF"好：那会静默把这条流的 body 变成空，而线上
  // 看起来一切正常（对端只会看到一个 content-length 对不上的响应）。
  if (it == self->streams_.end()) return NGHTTP3_ERR_CALLBACK_FAILURE;

  stream_rec&  s    = it->second;
  const size_t left = s.out_body.size() - s.out_offset;
  if (left == 0) {
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 0;
  }

  // 一次把剩下的全交出去，并置 EOF —— 于是上游**只会调我们一次**：置了 EOF 的
  // DATA 帧会从帧队列里弹掉（`nghttp3_stream_fill_outq` 只在 `data_eof` 时
  // `ringbuf_pop_front`）。
  //
  // 交多了也不要紧：上游把这几段包成 outq 里的 alien buf，`writev_stream` 每轮
  // 只吐出 QUIC 这一轮吃得下的部分，剩下的靠 `add_write_offset(实际收下的字节)`
  // 记账，下一轮从同一批 alien buf 接着吐。**"交出去"和"送出去"是两件事**，
  // 这正是上游要 `add_write_offset` 的原因。
  //
  // 前提是那些字节活到上游不再引用它们为止 —— 所以 `out_body` 由 `stream_rec`
  // 持有到这条流关闭，而不是本次调用结束。
  vec[0].base = reinterpret_cast<uint8_t*>(&s.out_body[s.out_offset]);
  vec[0].len  = left;
  s.out_offset = s.out_body.size();
  *pflags |= NGHTTP3_DATA_FLAG_EOF;
  return 1;
}

}  // namespace uvcpp

#endif  // UVCPP_HTTP3_ENABLE
