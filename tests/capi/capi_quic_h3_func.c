/**
 * @file tests/capi/capi_quic_h3_func.c
 * @brief quic + http3 两层的 C ABI 端到端用例：**真起一条 QUIC 连接、真跑三次
 *        请求、真收字节**。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这份用例是**纯 C**（`.c`，不 include 任何 C++ 头）。与 `capi_h2_func.c` 的
 * 形状只差一处，而那一处是被 QUIC 的本质逼出来的：
 *
 *   - h2 那用例：TCP，主线程是服务端、另一条线程是客户端，两边各自 `run()`。
 *   - 本用例：**一条线程泵两条循环**（`_run_once()` + `uv_sleep(1)` 轮询）。
 *     理由是 QUIC 两侧都在 UDP 上，用两条线程就要处理"谁先跑"的时序偶然 ——
 *     `tests/functional/http3_request_func.cpp` 那条同形状的 C++ 用例给出了同
 *     一条理由。
 *
 *     代价是**量不到 `E_WRONG_THREAD`**：那是"跨线程用句柄"才有的错误码，一条
 *     线程上根本构造不出来（它由批 2 / 3a 的两条用例去量，那边本来就有第二条
 *     线程）。这里换成两条只有 C 面才有的东西顶上来：**回调表按 `size` 逐格读**
 *     与**每个类型一枚魔数的类型混淆**。
 *
 * 三次请求都是**串行**发的（发一条、等它收场、再发下一条）。这不是图省事：
 * 完成队列是 FIFO 的，两条在飞的时候"`on_stream_close` 响的那一刻队列里恰好
 * 一条"就不再成立，而这一条恰恰是 `uvcpp_c_h3_conn_take_completed()` 那套断言
 * 的核心。串行让那句断言量的是**队列语义**，而不是"这次运气好"。
 *
 *   | 轮 | 客户端 | 服务端怎么回 | 判据 |
 *   |----|--------|--------------|------|
 *   | 1 | `POST /echo` + 13 字节 body + `x-capi` 连设两次 | `_send_response`（200 + `content-type: text/plain` + **把收到的请求体原样回过去**）| 状态码 200、`content-type` 往返、body **逐字节**等于服务端**收到的那一份**；服务端那侧 method / path / scheme / authority / 自定义头 / body 逐条对上；查不到的头是 `E_NOT_FOUND` |
 *   | 2 | `GET /status`（无 body）| `_send_status`（201 + `"made"`）| 状态码 201、body 逐字节 |
 *   | 3 | `GET /empty`（无 body）| `_send_status`（204 + **NULL/0**）| 状态码 204、body **长度 0**（不是"少收了一段"）|
 *
 * 每轮都钉同一组**只有 C 面才有**的规矩：
 *
 *   1. **`on_stream_close` 响的时候，这条流的响应已经在完成队列里了** ——
 *      `_completed_count()` == 1（不是 0，也不是 ≥1）、`_take_completed()` == 1、
 *      取完 == 0、**再取一次还是 0**。四个数一起才有意义：只断言"取到了"的话，
 *      一个"每次都给最后一条"的实现照样绿。
 *   2. **响应挂在它自己那条流上**：`_response_stream_id()` 等于 `_send_request()`
 *      交回来的那个号（三条流的号互不相同、且递增）。
 *   3. **"没有状态码"与"状态码是 0"分得开**：客户端在取之前问
 *      `_response_status()` 是 `E_NOT_FOUND` —— 绝不是替对端编出来的 200。
 *   4. **借来的句柄**（`uvcpp_c_quic_connection`）：客户端那一枚在 `_new()` 时
 *      就存在，于是"先记表、再 connect"这条路要能走；还没连上时
 *      `_conn_state()` 是 `IDLE`、`_open_stream()` 是 `E_STATE`、
 *      `uvcpp_c_h3_connection_new()` 是 **NULL**（底下没有连接，接不上去）。
 *      连接关掉之后一律 `E_STALE`。
 *   5. **回调期句柄出了回调就作废**：`on_request` 里存下来的
 *      `uvcpp_c_h3_request_view*`，回调返回之后再用必须是 `E_STALE`。
 *      同理，"还没定流号"的那枚响应，`_send_response()` 必须用 `UV_EINVAL`
 *      撂倒（流号没设 = 这条响应没归属）。
 *   6. **回调表按 `size` 逐格读**：`size = 3` 的表**整张都不读**，返回
 *      `E_INVALID_ARG`，而且不许把这张表装上去 —— 判据是"冒充的那份 fixture
 *      上一个计数都没有"。这一层有**两张**以 `size` 打头的表（h3 那份与
 *      端点那份 `uvcpp_c_quic_callbacks`），两侧各喂一次：h3 那份在服务端
 *      `on_connection` / 客户端 `_start()` 之前各一次，端点那份在服务端
 *      `on_connection` / 客户端 `_connect()` 之前各一次。四次的返回码都单独
 *      断言 —— 端点那份**只有返回码能当判据**（截断表里那些格本来就不会被填，
 *      "装上了"与"没装上"在行为上长得一样），见那两处的注释。
 *
 * 收尾那一段是**这一批最要紧的一条**（`capi_mutation.py` 的 M18）：借出的
 * QUIC 连接句柄只有在底层连接真没了的时候才反登记，而"真没了"这件事有两个入口
 * —— 裸 QUIC 走本模块 `on_close` 那套跳板，**装上 h3 之后走 h3 的
 * `on_disconnect`**（h3 的 `start()` 会把连接上那张回调表整个换掉，裸 QUIC 那套
 * 一次都不响）。这份用例走的是后者，收尾那句
 * `uvcpp_c_live_handle_count() == 0` 就是它的判据：少一个入口，借出的句柄
 * 就永远留在登记表里，这个数归不了零。
 */

#include <capi/uvcpp_c.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#if !UVCPP_CAPI_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_CAPI=ON 的构建里被编译"
#endif
#if !UVCPP_QUIC_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_QUIC=ON 的构建里被编译"
#endif
#if !UVCPP_HTTP3_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_HTTP3=ON 的构建里被编译"
#endif

static int g_checks = 0;
static int g_failed = 0;

/* 与 `capi_net_func.c` / `capi_h2_func.c` / `capi_webapp_func.c` 同一个形状：
 * 每次判定记一笔，末尾印 `checks=… failures=…`。`tests/tools/` 下那些驱动靠
 * 这一行区分"是它红的"和"被别人的红带下去的"。 */
#define CHECK(cond)                                                      \
  do {                                                                   \
    ++g_checks;                                                          \
    if (!(cond)) {                                                       \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

#define CHECK_INT(got, want)                                             \
  do {                                                                   \
    long long _g = (long long)(got);                                     \
    long long _w = (long long)(want);                                    \
    ++g_checks;                                                          \
    if (_g != _w) {                                                      \
      printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__,    \
             #got, _g, _w);                                              \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

#define CHECK_STR(got, want)                                             \
  do {                                                                   \
    const char* _g = (got);                                              \
    const char* _w = (want);                                             \
    ++g_checks;                                                          \
    if (_g == NULL || strcmp(_g, _w) != 0) {                             \
      printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__,          \
             __LINE__, #got, _g != NULL ? _g : "(null)", _w);            \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

#define CHECK_BYTES(got, got_len, want, want_len)                        \
  do {                                                                   \
    const char* _g  = (got);                                             \
    const char* _w  = (want);                                            \
    size_t      _gl = (size_t)(got_len);                                 \
    size_t      _wl = (size_t)(want_len);                                \
    ++g_checks;                                                          \
    if (_g == NULL || _w == NULL || _gl != _wl ||                        \
        memcmp(_g, _w, _wl) != 0) {                                      \
      printf("FAIL %s:%d: %s = %d 字节, want %d 字节\n", __FILE__,        \
             __LINE__, #got, (int)_gl, (int)_wl);                        \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

/* 三条流各说什么 —— 长度**互不相同**且都不是 2 的幂，这样"哪条流的 body 混进了
 * 哪条"不会因为长度一样而看不出来。 */
#define kCliBody  "capi-quic-h3!" /* 13 字节，第 1 轮的请求体 */
#define kPong1    kCliBody        /* 第 1 轮的响应体：原样回过去 */
#define kMadeBody "made"          /* 4 字节，第 2 轮的响应体 */

#define kRounds     3
#define kDeadlineMs 10000

/* 第 1 轮那个自定义头。**同一个名字设两次**（值不同）—— 服务端必须只看到
 * 后设的那个：C++ 那边是"同名覆盖"，一次追加就会让服务端看到两个同名头，
 * 而"查出来的值是哪一个"就成了实现细节。 */
#define kHdrName  "x-capi"
#define kHdrFirst "1"
#define kHdrLast  "2"

/* =========================================================================
 * fixture
 * =========================================================================
 * 一个结构体装两侧的全部账。**冒充的那一份（`g_bad`）不是摆设**：`size = 3`
 * 那张坏表要是被装了上去，它的回调就会往那份上记数 —— 于是"一个计数都没有"
 * 成了那条 `E_INVALID_ARG` 的**可观测形式**。传一个假指针（`0x5a5a…`）看着
 * 更狠，实际上只是把"装错了"变成一次段错误，反而丢了判据。
 */

typedef struct {
  /* ---- 端点与 TLS（两侧共用一份，因为只有一侧会连）---- */
  uvcpp_c_quic_server* srv;
  uvcpp_c_quic_client* cli;
  uvcpp_c_quic_tls*    tls_srv;
  uvcpp_c_quic_tls*    tls_cli;
  int                  port;

  /* ---- 服务端侧 ---- */
  int                      connections; /* `on_connection` 跑了几次 */
  uvcpp_c_quic_connection* sconn;       /* 借来的：服务端侧的连接句柄 */
  uvcpp_c_h3_connection*   s_h3;
  int                      conn_state_on_connect;
  int                      h3_null;
  int                      bad_size_rc; /* h3 那张 size=3 的表的返回值 */
  int                      q_bad_size_rc; /* 同一张坏表喂给**端点**那一格 */
  int                      start_rc;
  int                      server_side;
  int                      ready_after_start;  /* `_start()` 之后 `_conn_ready()` */
  int                      closed_after_start; /* `_start()` 之后 `_conn_closed()` */
  int                      request_before_ready;
  int                      type_confusion_rc;  /* 拿 h3 句柄当 quic 连接用 */

  int     requests;
  int     method_rc[kRounds];
  char    method[kRounds][16];
  char    path[kRounds][32];
  char    scheme[kRounds][16];
  char    authority[kRounds][32];
  int     hdr_rc[kRounds];
  char    hdr[kRounds][16];
  int     hdr_missing_rc[kRounds];
  size_t  body_size[kRounds];
  int     body_rc[kRounds];
  char    body[kRounds][64];
  int64_t stream_id[kRounds];
  int     send_rc[kRounds];

  const uvcpp_c_h3_request_view* saved_view;
  int                            view_after_probed;

  int      stream_close_size_ok;
  int      stream_closes;
  int      stream_close_errs;
  int      critical_open[3]; /* 三条关键流在 `_start()` 那一刻开出来没有 */
  int      errors;
  int      disconnects;
  int      free_in_cb;
  int      after_free_rc;
  size_t   bytes_in;
  size_t   bytes_out;
  char     alpn[16];
  int      alpn_rc;

  /* ---- 客户端侧 ---- */
  uvcpp_c_quic_connection* cconn;
  uvcpp_c_h3_connection*   c_h3;
  int                      c_h3_null;
  int                      c_connect_calls;
  int                      c_connect_status;
  int                      c_start_rc;
  int                      c_bad_size_rc; /* h3 那张 size=2 的表的返回值 */
  int                      c_q_bad_size_rc; /* 端点那一格，客户端侧那一次 */
  int                      c_server_side;
  int                      c_ready;
  int                      c_state_before;
  int64_t                  c_open_before;
  int                      c_streams_left_before;
  int                      c_alpn_len_before;
  int                      c_send_before_ready;

  int     take_before[kRounds];
  int     take_rc[kRounds];
  int     take_after[kRounds];
  int     take_again[kRounds];
  int     c_status_rc[kRounds];
  int     c_status[kRounds];
  int     c_error[kRounds];
  int64_t c_resp_stream_id[kRounds];
  size_t  c_body_size[kRounds];
  int     c_body_rc[kRounds];
  char    c_body[kRounds][64];
  int     c_ct_rc[kRounds];
  char    c_ct[kRounds][32];
  int     c_missing_hdr_rc[kRounds];
  int64_t c_sent_id[kRounds];

  int      c_stream_closes;
  int      c_errors;
  int      c_disconnects;
  int      c_free_in_cb;
  int      c_after_free_rc;
  size_t   c_bytes_in;
  size_t   c_bytes_out;
  char     c_alpn[16];
  int      c_alpn_rc;

  /* ---- 泵的进度 ---- */
  int want_rounds;
} fixture;

static fixture g;
static fixture g_bad;

static fixture* fx(void* ud) { return (fixture*)ud; }

/* =========================================================================
 * 泵：一条线程驱动两条循环
 * ========================================================================= */

static int pump_both(int (*cond)(void), int deadline_ms) {
  const uint64_t t0 = uv_hrtime();
  while (!cond()) {
    uvcpp_c_quic_client_run_once(g.cli);
    uvcpp_c_quic_server_run_once(g.srv);
    if ((uv_hrtime() - t0) > (uint64_t)deadline_ms * 1000000ULL) return 0;
    uv_sleep(1);
  }
  return 1;
}

static int handshaked(void) { return g.c_connect_calls >= 1; }

static int both_ready(void) {
  return g.s_h3 != NULL && g.c_h3 != NULL &&
         uvcpp_c_h3_conn_ready(g.s_h3) == 1 &&
         uvcpp_c_h3_conn_ready(g.c_h3) == 1;
}

static int round_n_done(void) { return g.c_stream_closes >= g.want_rounds; }

static int closed_both(void) {
  return g.disconnects >= 1 && g.c_disconnects >= 1;
}

/* =========================================================================
 * 服务端的回调
 * ========================================================================= */

static void srv_on_request(void* ud, uvcpp_c_h3_connection* h,
                           const uvcpp_c_h3_request_view* req) {
  fixture*             f = fx(ud);
  const int            i = f->requests;
  uvcpp_c_h3_response* resp;
  char                 buf[64];
  int64_t              sid;

  if (i >= kRounds) return; /* 多了就不记 */
  ++f->requests;

  f->method_rc[i] = uvcpp_c_h3_request_view_method(req, buf, sizeof(buf));
  if (f->method_rc[i] > 0)
    memcpy(f->method[i], buf, (size_t)f->method_rc[i] + 1);
  if (uvcpp_c_h3_request_view_path(req, f->path[i], sizeof(f->path[i])) <= 0)
    f->path[i][0] = '\0';
  if (uvcpp_c_h3_request_view_scheme(req, f->scheme[i],
                                     sizeof(f->scheme[i])) <= 0)
    f->scheme[i][0] = '\0';
  if (uvcpp_c_h3_request_view_authority(req, f->authority[i],
                                        sizeof(f->authority[i])) <= 0)
    f->authority[i][0] = '\0';

  /* 自定义头（头名比较大小写不敏感 —— 这里刻意用小写问） */
  f->hdr_rc[i] = uvcpp_c_h3_request_view_header(req, kHdrName, f->hdr[i],
                                                sizeof(f->hdr[i]));
  if (f->hdr_rc[i] <= 0) f->hdr[i][0] = '\0';
  f->hdr_missing_rc[i] =
      uvcpp_c_h3_request_view_header(req, "x-nope", buf, sizeof(buf));

  f->body_size[i] = uvcpp_c_h3_request_view_body_size(req);
  f->body_rc[i] =
      uvcpp_c_h3_request_view_body(req, f->body[i], sizeof(f->body[i]));
  f->stream_id[i] = uvcpp_c_h3_request_view_stream_id(req);
  sid             = f->stream_id[i];

  /* ★ 视图是**回调期句柄**。这里留着它，出回调之后（main 里）再用一次 ——
   * 那一次必须是 `E_STALE`。留的是指针不是内容：`alive()` 先查登记表、一个
   * 字节都不读调用方那块内存，所以"出栈之后再问它"是安全的（
   * `tests/capi/capi_h2_func.c` 那一枚同款，理由写在那份的文件头）。 */
  f->view_after_probed = 1;
  f->saved_view        = req;

  resp = uvcpp_c_h3_response_new(0);
  if (resp == NULL) return;
  if (i == 0) {
    /* ★ 流号必须**先设**：`_send_response()` 那条路走的正是 `resp` 里带的那个
     * 号（与 C++ 的 `send_response(resp, omit_body)` 同形）。不设就是 `-1`，
     * 那条会拿 `UV_EINVAL` 撂倒 —— 见 main 里那条负向断言。 */
    uvcpp_c_h3_response_set_stream_id(resp, sid);
    uvcpp_c_h3_response_set_status(resp, 200);
    uvcpp_c_h3_response_set_content_type(resp, "text/plain");
    /* **原样回过去**：客户端拿到的 body 必须逐字节等于服务端收到的那个。 */
    uvcpp_c_h3_response_set_body(resp, f->body[i], f->body_size[i]);
    f->send_rc[i] = uvcpp_c_h3_conn_send_response(h, resp, 0);
  } else if (i == 1) {
    f->send_rc[i] =
        uvcpp_c_h3_conn_send_status(h, sid, 201, kMadeBody, strlen(kMadeBody));
  } else {
    /* `body` 为 NULL / 0 是**合法**的（"只发 :status"），头里点着名说过了。 */
    f->send_rc[i] = uvcpp_c_h3_conn_send_status(h, sid, 204, NULL, 0);
  }
  uvcpp_c_h3_response_free(resp);
}

static void srv_on_stream_close(void* ud, uvcpp_c_h3_connection* h,
                                const uvcpp_c_h3_stream_close_info* info) {
  fixture* f = fx(ud);
  (void)h;
  if (info == NULL) return;
  if (info->size == (uint32_t)sizeof(uvcpp_c_h3_stream_close_info))
    ++f->stream_close_size_ok;
  if (info->rx_error != 0 || info->tx_error != 0) ++f->stream_close_errs;
  ++f->stream_closes;
}

static void srv_on_error(void* ud, uvcpp_c_h3_connection* h, int error_code) {
  fixture* f = fx(ud);
  (void)h;
  (void)error_code;
  ++f->errors;
}

static void srv_on_disconnect(void* ud, uvcpp_c_h3_connection* h) {
  fixture* f = fx(ud);
  char     buf[64];
  /* ★ 这是这一层**要求**在这儿做的那件事：h3 对象由持有者销毁（见
   * `uvcpp_c_http3.h` 文件头第 2 条）。free 之后同一个句柄一律 `E_STALE`。 */
  f->bytes_in  = (size_t)uvcpp_c_h3_conn_bytes_in(h);
  f->bytes_out = (size_t)uvcpp_c_h3_conn_bytes_out(h);
  f->alpn_rc   = uvcpp_c_h3_conn_alpn_selected(h, buf, sizeof(buf));
  if (f->alpn_rc > 0) memcpy(f->alpn, buf, (size_t)f->alpn_rc + 1);
  f->free_in_cb    = uvcpp_c_h3_connection_free(h);
  f->after_free_rc = uvcpp_c_h3_conn_ready(h);
  ++f->disconnects;
}

static void srv_on_connection(void* ud, uvcpp_c_quic_connection* c) {
  fixture*               f = fx(ud);
  uvcpp_c_h3_callbacks   cbs;
  uvcpp_c_quic_callbacks qcbs;
  int64_t                ids[3];

  ++f->connections;
  f->sconn                 = c;
  f->conn_state_on_connect = uvcpp_c_quic_conn_state(c);

  /* ★ 借来的连接句柄上那张**端点级**的表也撞一次"`size` 连第一格都没盖住"。
   * 与下面 h3 那一次是**同一份守卫的另一处**（`copy_conn_callbacks`）——
   * 两处各喂一次，"这一侧被喂过"才不是靠传递关系推出来的。
   *
   * 这一格**只有返回码能当判据**：截断表（`size = 3`）里那些格本来就不会被填
   * （`field_present` 管着），所以"装上了"和"没装上"在行为上长得一模一样 ——
   * 把守卫拆掉之后，这里拿到的是 `0` 而不是 `E_INVALID_ARG`，仅此而已。 */
  memset(&qcbs, 0, sizeof(qcbs));
  qcbs.size = 3;
  f->q_bad_size_rc = uvcpp_c_quic_conn_set_callbacks(c, &qcbs, &g_bad);

  /* ★ 借来的句柄在**回调里**是活的：它此刻刚被建出来（首包到达），底层连接
   * 已经在了 —— 于是这一枚能接 h3。 */
  f->s_h3 = uvcpp_c_h3_connection_new(c, 1);
  if (f->s_h3 == NULL) {
    f->h3_null = 1;
    return;
  }

  /* ★ 先撞"`size` 连自己这一格都没盖住"的表：整张表**都不读**，返回
   * `E_INVALID_ARG`。这一句必须在真 `_start()` 之前 —— 它证明的正是"拒绝发生
   * 在任何动作之前"，判据是 `g_bad` 上一个计数都没有（见 main 里那三条）。 */
  memset(&cbs, 0, sizeof(cbs));
  cbs.size          = 3;
  cbs.on_request    = srv_on_request;
  cbs.on_disconnect = srv_on_disconnect;
  f->bad_size_rc    = uvcpp_c_h3_conn_start(f->s_h3, &cbs, &g_bad);

  memset(&cbs, 0, sizeof(cbs));
  cbs.size            = (uint32_t)sizeof(cbs);
  cbs.on_request      = srv_on_request;
  cbs.on_stream_close = srv_on_stream_close;
  cbs.on_error        = srv_on_error;
  cbs.on_disconnect   = srv_on_disconnect;
  f->start_rc         = uvcpp_c_h3_conn_start(f->s_h3, &cbs, f);
  f->server_side      = uvcpp_c_h3_conn_server_side(f->s_h3);
  f->ready_after_start  = uvcpp_c_h3_conn_ready(f->s_h3);
  f->closed_after_start = uvcpp_c_h3_conn_closed(f->s_h3);
  f->request_before_ready =
      (f->requests > 0 && f->ready_after_start == 0) ? 1 : 0;

  /* ★ 类型混淆：拿一个 **h3 连接**句柄去当 **QUIC 连接**用。两者都是本层发出
   * 去的、都登记在表里，只有魔数不同 —— 共用一枚魔数的话这一句会一路走到读错
   * 布局的地方。 */
  f->type_confusion_rc = uvcpp_c_quic_conn_state(
      (const uvcpp_c_quic_connection*)(const void*)f->s_h3);

  /* 三条关键单向流此刻**还没开**（要等 ALPN），所以读出来是 -1 —— 这是"读
   * 访问器在还没准备好时不说谎"的那一格。这一格判的是**同一句话**的另一半：
   * `_start()` 之后 `_conn_ready()` 是 0（见下面 main 里那句断言），而那三个
   * `_stream_id()` 也必须是 -1。谁要是把它们提前开出来，两次断言都会红。 */
  ids[0] = uvcpp_c_h3_conn_control_stream_id(f->s_h3);
  ids[1] = uvcpp_c_h3_conn_qpack_encoder_stream_id(f->s_h3);
  ids[2] = uvcpp_c_h3_conn_qpack_decoder_stream_id(f->s_h3);
  f->critical_open[0] = (ids[0] >= 0);
  f->critical_open[1] = (ids[1] >= 0);
  f->critical_open[2] = (ids[2] >= 0);
}

/* =========================================================================
 * 客户端的回调
 * ========================================================================= */

static void cli_on_connect(void* ud, int status) {
  fixture* f = fx(ud);
  ++f->c_connect_calls;
  f->c_connect_status = status;
}

static void cli_on_stream_close(void* ud, uvcpp_c_h3_connection* h,
                                const uvcpp_c_h3_stream_close_info* info) {
  fixture*             f  = fx(ud);
  uvcpp_c_h3_response* r  = NULL;
  uvcpp_c_h3_response* r2 = NULL;
  char                 buf[64];
  const int            i = f->c_stream_closes;
  (void)info;

  if (i >= kRounds) { /* 多了就只记数 */
    ++f->c_stream_closes;
    return;
  }

  /* ★ 头里那条契约：`on_stream_close` 响的时候，这条流的响应**已经在完成
   * 队列里了**。四个数一起看，才量得出"队列语义"而不是"这次运气好"：
   * 取之前 1 条、取到 1 条、取完 0 条、再取还是 0 条。 */
  f->take_before[i] = uvcpp_c_h3_conn_completed_count(h);

  r = uvcpp_c_h3_response_new(0);
  if (r != NULL) {
    /* 取之前问状态码：一次都没填过 ⇒ `E_NOT_FOUND`，**不是**替对端编的 200。 */
    f->c_status_rc[i]      = uvcpp_c_h3_response_status(r);
    f->take_rc[i]          = uvcpp_c_h3_conn_take_completed(h, r);
    f->take_after[i]       = uvcpp_c_h3_conn_completed_count(h);
    f->c_status[i]         = uvcpp_c_h3_response_status(r);
    f->c_error[i]          = uvcpp_c_h3_response_error(r);
    f->c_resp_stream_id[i] = uvcpp_c_h3_response_stream_id(r);
    f->c_body_size[i]      = uvcpp_c_h3_response_body_size(r);
    f->c_body_rc[i] =
        uvcpp_c_h3_response_body(r, f->c_body[i], sizeof(f->c_body[i]));
    f->c_ct_rc[i] = uvcpp_c_h3_response_header(r, "content-type", f->c_ct[i],
                                               sizeof(f->c_ct[i]));
    if (f->c_ct_rc[i] <= 0) f->c_ct[i][0] = '\0';
    f->c_missing_hdr_rc[i] =
        uvcpp_c_h3_response_header(r, "x-nope", buf, sizeof(buf));
    uvcpp_c_h3_response_free(r);
  }

  /* 再取一次：必须还是 0 —— "一次一条"的后半截。 */
  r2 = uvcpp_c_h3_response_new(0);
  if (r2 != NULL) {
    f->take_again[i] = uvcpp_c_h3_conn_take_completed(h, r2);
    uvcpp_c_h3_response_free(r2);
  }

  ++f->c_stream_closes;
}

static void cli_on_error(void* ud, uvcpp_c_h3_connection* h, int error_code) {
  fixture* f = fx(ud);
  (void)h;
  (void)error_code;
  ++f->c_errors;
}

static void cli_on_disconnect(void* ud, uvcpp_c_h3_connection* h) {
  fixture* f = fx(ud);
  char     buf[64];
  f->c_bytes_in  = (size_t)uvcpp_c_h3_conn_bytes_in(h);
  f->c_bytes_out = (size_t)uvcpp_c_h3_conn_bytes_out(h);
  f->c_alpn_rc   = uvcpp_c_h3_conn_alpn_selected(h, buf, sizeof(buf));
  if (f->c_alpn_rc > 0) memcpy(f->c_alpn, buf, (size_t)f->c_alpn_rc + 1);
  f->c_free_in_cb    = uvcpp_c_h3_connection_free(h);
  f->c_after_free_rc = uvcpp_c_h3_conn_ready(h);
  ++f->c_disconnects;
}

/* =========================================================================
 * 构造器 / 纯函数的边界（全都不碰网络）
 * ========================================================================= */

static void test_constructors(void) {
  uvcpp_c_h3_request*  req;
  uvcpp_c_h3_response* resp;
  uvcpp_c_quic_client* c;
  uvcpp_c_quic_server* s;
  char                 buf[64];
  unsigned char        stack[64];
  const char* const    bad_protos[2] = {"h3", NULL};

  printf("[capi quic+h3] 构造器与版本\n");

  CHECK_INT(uvcpp_c_quic_crypto_init(), 0);
  CHECK_INT(uvcpp_c_quic_ngtcp2_version(buf, sizeof(buf)) > 0, 1);
  CHECK_INT(uvcpp_c_h3_version(buf, sizeof(buf)) > 0, 1);
  /* `0` 是 `NGTCP2_NO_ERROR` 在 ngtcp2 那侧的名字 —— 这一句同时是"真链上了
   * ngtcp2"的证据（`quic_error_string()` 是这个 TU 里第一处真调进它的地方，
   * 少链了 ngtcp2 就在这里以未定义符号红在链接期，而不是运行期）。 */
  CHECK_INT(uvcpp_c_quic_error_string(0, buf, sizeof(buf)), 8);
  CHECK_STR(buf, "NO_ERROR");

  /* 缓冲区约定：够大才写，返回的永远是真实长度（cap=0 也一样）。 */
  CHECK_INT(uvcpp_c_h3_version(NULL, 0) > 0, 1);
  CHECK_INT(uvcpp_c_quic_error_string(0, NULL, 0), 8);

  /* ---- 请求构造器 ---- */
  CHECK(uvcpp_c_h3_request_new(NULL, "/") == NULL);
  CHECK(uvcpp_c_h3_request_new("GET", NULL) == NULL);

  req = uvcpp_c_h3_request_new("PUT", "/x");
  CHECK(req != NULL);
  if (req != NULL) {
    CHECK_INT(uvcpp_c_h3_request_set_scheme(req, "https"), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_request_set_scheme(req, NULL),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_request_set_authority(req, "h:1"), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_request_set_authority(req, NULL),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_request_set_header(req, NULL, "v"),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_request_set_header(req, "k", NULL),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_request_set_body(req, NULL, 4),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_request_set_body(req, NULL, 0), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_request_free(req), UVCPP_C_OK);
    /* 释放之后再用：`E_STALE`（不是崩溃，也不是读到栈上那块被复用的内存） */
    CHECK_INT(uvcpp_c_h3_request_set_scheme(req, "https"), UVCPP_C_E_STALE);
  }
  /* 空指针：不是 `E_STALE`（那是"用过的"），是 `E_INVALID_ARG`（"你给了我个空"） */
  CHECK_INT(uvcpp_c_h3_request_free(NULL), UVCPP_C_E_INVALID_ARG);

  /* ---- 响应容器 ---- */
  resp = uvcpp_c_h3_response_new(0);
  CHECK(resp != NULL);
  if (resp != NULL) {
    /* `0` 是"还没定"：问状态码是 `E_NOT_FOUND`，不是 0、也不是 200。 */
    CHECK_INT(uvcpp_c_h3_response_status(resp), UVCPP_C_E_NOT_FOUND);
    CHECK_INT((int)uvcpp_c_h3_response_body_size(resp), 0);
    CHECK_INT(uvcpp_c_h3_response_error(resp), 0);
    /* 流号没有初值：**不是 0**（QUIC 里 0 是一条真的流），是 -1 */
    CHECK_INT(uvcpp_c_h3_response_stream_id(resp), -1);
    CHECK_INT(uvcpp_c_h3_response_set_stream_id(resp, -1),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_response_set_stream_id(resp, 4), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_response_stream_id(resp), 4);
    CHECK_INT(uvcpp_c_h3_response_set_header(resp, "x", NULL),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_response_set_content_type(resp, NULL),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_response_set_body(resp, NULL, 8),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h3_response_set_body(resp, NULL, 0), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_response_set_status(resp, 404), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_response_status(resp), 404);
    CHECK_INT(uvcpp_c_h3_response_set_body(resp, "abcd", 4), UVCPP_C_OK);
    CHECK_INT((int)uvcpp_c_h3_response_body_size(resp), 4);
    CHECK_INT(uvcpp_c_h3_response_has_header(resp, "x"), 0);
    CHECK_INT(uvcpp_c_h3_response_has_header(resp, NULL),
              UVCPP_C_E_INVALID_ARG);
    /* `_reset()` 把状态码、头表、body **和流号**一起清掉 */
    CHECK_INT(uvcpp_c_h3_response_reset(resp), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_response_status(resp), UVCPP_C_E_NOT_FOUND);
    CHECK_INT((int)uvcpp_c_h3_response_body_size(resp), 0);
    CHECK_INT(uvcpp_c_h3_response_stream_id(resp), -1);
    CHECK_INT(uvcpp_c_h3_response_free(resp), UVCPP_C_OK);
  }
  CHECK_INT(uvcpp_c_h3_response_free(NULL), UVCPP_C_E_INVALID_ARG);

  /* ---- 从来没被交出去的指针：登记表里没有 → 一律 `E_STALE`，一个字节都不读 ---- */
  CHECK_INT(uvcpp_c_h3_request_free((uvcpp_c_h3_request*)(void*)stack),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h3_response_free((uvcpp_c_h3_response*)(void*)stack),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h3_connection_free((uvcpp_c_h3_connection*)(void*)stack),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h3_request_view_method(
                (const uvcpp_c_h3_request_view*)(const void*)stack, buf,
                sizeof(buf)),
            UVCPP_C_E_STALE);
  CHECK_INT((int)uvcpp_c_h3_request_view_body_size(
                (const uvcpp_c_h3_request_view*)(const void*)stack),
            0);
  CHECK_INT(uvcpp_c_h3_request_view_stream_id(
                (const uvcpp_c_h3_request_view*)(const void*)stack),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h3_connection_free(NULL), UVCPP_C_E_INVALID_ARG);
  CHECK(uvcpp_c_h3_connection_new(NULL, 0) == NULL);
  CHECK(uvcpp_c_h3_connection_new((uvcpp_c_quic_connection*)(void*)stack, 0) ==
        NULL);

  /* ---- TLS：三个建法 + 三个改法里能离线判的那几格 ---- */
  CHECK(uvcpp_c_quic_tls_server_new(NULL, "k") == NULL);
  CHECK(uvcpp_c_quic_tls_server_new("c", NULL) == NULL);
  CHECK(uvcpp_c_quic_tls_server_new("/definitely/not/here.pem",
                                    "/definitely/not/here.key") == NULL);
  CHECK_INT(uvcpp_c_quic_tls_free(NULL), UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_tls_set_ca_file(NULL, "x"), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_quic_tls_set_verify(NULL, 0), UVCPP_C_E_STALE);

  /* ---- 端点：传空 / 传坏的那几格 ---- */
  CHECK_INT(uvcpp_c_quic_client_free(NULL), UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_server_free(NULL), UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_client_set_idle_timeout(NULL, 1), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_quic_server_set_idle_timeout(NULL, 1), UVCPP_C_E_STALE);

  c = uvcpp_c_quic_client_new();
  s = uvcpp_c_quic_server_new();
  CHECK(c != NULL && s != NULL);
  if (c != NULL) {
    /* `protos` 里有空元素（不是"数组为空"）是**参数错误** —— 与"给 0 个"
     * 那条恢复默认的规矩分得开。 */
    CHECK_INT(uvcpp_c_quic_client_set_alpn_protos(c, bad_protos, 2),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_quic_client_set_alpn_protos(c, NULL, 0), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_quic_client_free(c), 0);
  }
  if (s != NULL) {
    CHECK_INT(uvcpp_c_quic_server_set_alpn_protos(s, bad_protos, 2),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_quic_server_bind(s, "127.0.0.1", -1),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_quic_server_free(s), 0);
  }
}

/* =========================================================================
 * "还挂着活连接就 free 端点" —— 服务端收尾那条路的回归
 * =========================================================================
 *
 * 这条用例的来历是一枚**必现的 SIGSEGV**：`uvcpp_c_quic_server_free()` 原先边
 * 遍历 `server->conns` 边 `detach_conn()`，而后者会从那张表里 `erase`（那是它
 * "反登记"那一半）—— 当前迭代器失效，`++it` 踩在已回收的桶上。哪怕表里只有
 * 一条连接也照样崩。
 *
 * 为什么仓里原先抓不到：**所有** QUIC 用例（包括本文件的主流程）都是"先关
 * 连接、再释放端点"，于是 `_free()` 那一刻那张表永远是空的，这条路径一次都没
 * 被走过。撞上它的是 C# 例子（`bindings/csharp/examples/QuicEcho`）—— 它是最
 * 早的外部消费者，形状就是"跑完一趟回显，直接把两端释放掉"。
 *
 * 判据三条：两个 `_free()` 都返回 0；执行流活到下一句（崩了就根本没有下一句）；
 * 活句柄数回到进来时的数（借出的连接句柄必须已经被反登记 —— 少了这一条，
 * "毒化"那一半漏掉也测不出来）。
 */
static uvcpp_c_quic_client* fl_cli;
static uvcpp_c_quic_server* fl_srv;
static int                  fl_connected;

static void fl_on_connect(void* ud, int status) {
  (void)ud;
  if (status == 0) fl_connected = 1;
}

/* 什么都不装：本用例量的是收尾，不是数据面。 */
static void fl_on_connection(void* ud, uvcpp_c_quic_connection* c) {
  (void)ud;
  (void)c;
}

static void test_free_with_live_conn(void) {
  const char* const protos[1] = {"echo/1"};
  uvcpp_c_quic_tls* tls_srv;
  uvcpp_c_quic_tls* tls_cli;
  uint64_t          t0;
  int               before;
  int               port;

  printf("[capi quic+h3] 带着活连接释放端点\n");

  before = (int)uvcpp_c_live_handle_count();

  tls_srv = uvcpp_c_quic_tls_server_selfsigned("localhost");
  tls_cli = uvcpp_c_quic_tls_client_new(NULL);
  CHECK(tls_srv != NULL);
  CHECK(tls_cli != NULL);
  if (tls_srv == NULL || tls_cli == NULL) return;

  fl_srv = uvcpp_c_quic_server_new();
  CHECK(fl_srv != NULL);
  if (fl_srv == NULL) return;
  CHECK_INT(uvcpp_c_quic_server_set_tls(fl_srv, tls_srv), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_server_set_alpn_protos(fl_srv, protos, 1), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_server_bind(fl_srv, "127.0.0.1", 0), 0);
  CHECK_INT(uvcpp_c_quic_server_listen(fl_srv, fl_on_connection, NULL), 0);
  port = uvcpp_c_quic_server_configured_port(fl_srv);
  CHECK(port > 0);
  if (port <= 0) return;

  fl_cli = uvcpp_c_quic_client_new();
  CHECK(fl_cli != NULL);
  if (fl_cli == NULL) return;
  CHECK_INT(uvcpp_c_quic_client_set_tls(fl_cli, tls_cli), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_client_set_alpn_protos(fl_cli, protos, 1), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_client_connect(fl_cli, "127.0.0.1", port,
                                        fl_on_connect, NULL),
            0);

  /* 泵到握手完成 —— 这一步是整条用例的**前提**：到这儿服务端那张 `conns`
   * 表里才真的有一枚活句柄。没有它，下面两个 `_free()` 走的是空表那条路，
   * 复现不出迭代器失效。 */
  t0 = uv_hrtime();
  while (!fl_connected && (uv_hrtime() - t0) < 5000ULL * 1000000ULL) {
    uvcpp_c_quic_client_run_once(fl_cli);
    uvcpp_c_quic_server_run_once(fl_srv);
    uv_sleep(1);
  }
  CHECK_INT(fl_connected, 1);

  /* 到这里**不 close、不 stop**，直接释放两端 —— 这就是那条必崩的路。 */
  CHECK_INT(uvcpp_c_quic_client_free(fl_cli), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_server_free(fl_srv), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_tls_free(tls_cli), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_tls_free(tls_srv), UVCPP_C_OK);
  CHECK_INT((int)uvcpp_c_live_handle_count(), before);
}

/* =========================================================================
 * 主流程
 * ========================================================================= */

/** @brief 发一条请求（交出去之后立刻把构造器废掉）。@return 1 = 拿到流号。 */
static int send_round(int i, const char* method, const char* path,
                      const char* body) {
  uvcpp_c_h3_request* req = uvcpp_c_h3_request_new(method, path);
  int64_t             sid;
  if (req == NULL) return 0;
  uvcpp_c_h3_request_set_scheme(req, "https");
  uvcpp_c_h3_request_set_authority(req, "localhost");
  if (i == 0) {
    /* 同名设两次：服务端必须只看到**后设**的那个值（C++ 那边是"同名覆盖"）。 */
    uvcpp_c_h3_request_set_header(req, kHdrName, kHdrFirst);
    uvcpp_c_h3_request_set_header(req, kHdrName, kHdrLast);
  }
  if (body != NULL) uvcpp_c_h3_request_set_body(req, body, strlen(body));
  sid = uvcpp_c_h3_conn_send_request(g.c_h3, req);
  uvcpp_c_h3_request_free(req);
  g.c_sent_id[i] = sid;
  return sid >= 0;
}

int main(void) {
  char                   buf[64];
  int                    i;
  const char* const      protos[1] = {"h3"};
  uvcpp_c_h3_response*   unset;
  uvcpp_c_h3_request*    early;
  uvcpp_c_quic_callbacks qcbs;

  memset(&g, 0, sizeof(g));
  memset(&g_bad, 0, sizeof(g_bad));

  printf("[capi quic+h3] start\n");

  test_constructors();

  /* 第二条：把"释放还在用的端点"这条收尾路径单独走一遍（它有自己的 TLS 对与
   * 端点对，与下面主流程那套互不干扰；crypto 已经在 `test_constructors()` 里
   * 初始化过，这里不再调、也不释放）。 */
  test_free_with_live_conn();

  /* ---- TLS：服务端现场生成自签证书、客户端不校验（默认） ---- */
  g.tls_srv = uvcpp_c_quic_tls_server_selfsigned("localhost");
  CHECK(g.tls_srv != NULL);
  g.tls_cli = uvcpp_c_quic_tls_client_new(NULL);
  CHECK(g.tls_cli != NULL);
  if (g.tls_srv == NULL || g.tls_cli == NULL) return 1;
  /* 三个改法各过一遍。`set_verify(1)` 之后**改回来** —— 装上 PEER 校验却没有
   * 信任库，握手会失败，而那是本用例刻意不测的一格。 */
  CHECK_INT(uvcpp_c_quic_tls_set_ca_file(g.tls_cli, "/definitely/not/here.pem"),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_tls_set_ca_file(g.tls_cli, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_tls_set_verify(g.tls_cli, 1), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_tls_set_verify(g.tls_cli, 0), UVCPP_C_OK);

  /* ---- 服务端 ---- */
  g.srv = uvcpp_c_quic_server_new();
  CHECK(g.srv != NULL);
  if (g.srv == NULL) return 1;
  CHECK_INT(uvcpp_c_quic_server_set_tls(g.srv, g.tls_srv), UVCPP_C_OK);
  /* `set_tls(srv, NULL)` 是**清掉**（头里那句"清空传 NULL"），不是参数错误。
   * 而"没配 TLS"在 QUIC 里是**致命的**（没有明文模式），所以清掉之后
   * `listen()` 必须直接失败 —— 判据就这两句，一正一反。 */
  CHECK_INT(uvcpp_c_quic_server_set_tls(g.srv, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_server_listen(g.srv, srv_on_connection, &g), UV_EINVAL);
  CHECK_INT(uvcpp_c_quic_server_set_tls(
                g.srv, (uvcpp_c_quic_tls*)(void*)protos),
            UVCPP_C_E_STALE); /* 不是一枚 TLS 句柄 */
  CHECK_INT(uvcpp_c_quic_server_set_tls(g.srv, g.tls_srv), UVCPP_C_OK);
  /* 三个 ALPN / 超时配置：`NULL / 0` 是"恢复默认"，显式给一个也对 */
  CHECK_INT(uvcpp_c_quic_server_set_alpn_protos(g.srv, NULL, 0), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_server_set_alpn_protos(g.srv, protos, 1), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_server_set_idle_timeout(g.srv, 5000), UVCPP_C_OK);
  /* `bind()` 只校验地址与端口，**不碰 socket** —— 所以这两条离线可判 */
  CHECK_INT(uvcpp_c_quic_server_bind(g.srv, "999.1.1.1", 0), UV_EINVAL);
  CHECK_INT(uvcpp_c_quic_server_bind(g.srv, NULL, 70000),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_server_bind(g.srv, "127.0.0.1", 0), 0);
  /* 名字之所以叫**configured**_port："记下了"与"这个端口归我了"是两件事 */
  CHECK_INT(uvcpp_c_quic_server_configured_port(g.srv), 0);
  CHECK_INT(uvcpp_c_quic_server_configured_ip(g.srv, buf, sizeof(buf)), 9);
  CHECK_STR(buf, "127.0.0.1");
  CHECK_INT(uvcpp_c_quic_server_listen(g.srv, NULL, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_server_listen(g.srv, srv_on_connection, &g), 0);
  g.port = uvcpp_c_quic_server_configured_port(g.srv);
  CHECK(g.port > 0 && g.port <= 65535);
  CHECK_INT(uvcpp_c_quic_server_listen(g.srv, srv_on_connection, &g),
            UV_EALREADY);
  if (g.port <= 0) return 1;

  /* ---- 客户端：先看"还没有底"那几格 ---- */
  g.cli = uvcpp_c_quic_client_new();
  CHECK(g.cli != NULL);
  if (g.cli == NULL) return 1;
  CHECK_INT(uvcpp_c_quic_client_set_tls(g.cli, g.tls_cli), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_client_set_idle_timeout(g.cli, 5000), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_client_set_alpn_protos(g.cli, NULL, 0), UVCPP_C_OK);
  /* 与 `listen()` 那条对称：清掉 TLS 之后 `_connect()` 也直接失败 */
  CHECK_INT(uvcpp_c_quic_client_set_tls(g.cli, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_client_connect(g.cli, "127.0.0.1", g.port,
                                        cli_on_connect, &g),
            UV_EINVAL);
  CHECK_INT(uvcpp_c_quic_client_set_tls(g.cli, g.tls_cli), UVCPP_C_OK);

  g.cconn = uvcpp_c_quic_client_connection(g.cli);
  CHECK(g.cconn != NULL); /* 借来的那一枚在 `_client_new()` 时就存在 */
  g.c_state_before        = uvcpp_c_quic_conn_state(g.cconn);
  g.c_open_before         = uvcpp_c_quic_conn_open_stream(g.cconn, 1);
  g.c_streams_left_before = (int)uvcpp_c_quic_conn_streams_left(g.cconn, 1);
  g.c_alpn_len_before =
      uvcpp_c_quic_conn_alpn_selected(g.cconn, buf, sizeof(buf));
  /* 底下还没有连接 ⇒ h3 接不上去（拿到的是 NULL，不是一个马上要炸的句柄） */
  g.c_h3_null = (uvcpp_c_h3_connection_new(g.cconn, 0) == NULL) ? 1 : 0;

  /* ★ 端点级那张表的坏表：客户端侧这一次撞在 `_connect()` **之前** ——
   * 那正是头里写的"先记表、再 connect"那个窗口，所以在那里量到的返回码
   * 与后面真的跑起来无关，纯纯是那条守卫。（服务端那一次在
   * `srv_on_connection` 里，两侧都喂过。） */
  memset(&qcbs, 0, sizeof(qcbs));
  qcbs.size = 2;
  g.c_q_bad_size_rc = uvcpp_c_quic_conn_set_callbacks(g.cconn, &qcbs, &g_bad);

  CHECK_INT(uvcpp_c_quic_client_connect(g.cli, NULL, g.port, cli_on_connect,
                                        &g),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_client_connect(g.cli, "127.0.0.1", 70000,
                                        cli_on_connect, &g),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_client_connect(g.cli, "127.0.0.1", g.port, NULL, &g),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_quic_client_connect(g.cli, "127.0.0.1", g.port,
                                        cli_on_connect, &g),
            UVCPP_C_OK);

  /* ---------------------------------------------------------------
   * `_connect()` 返回之后、下一次泵循环之前 —— 头里那条"唯一安全窗口"。
   * --------------------------------------------------------------- */
  g.c_h3 = uvcpp_c_h3_connection_new(g.cconn, 0);
  CHECK(g.c_h3 != NULL);
  if (g.c_h3 == NULL) return 1;
  {
    uvcpp_c_h3_callbacks cbs;
    /* 客户端这张坏表也喂一次：那一格守卫在两侧是**同一份代码**，但"在哪一侧被
     * 喂过"决定了它在哪一侧可红。 */
    memset(&cbs, 0, sizeof(cbs));
    cbs.size          = 2;
    cbs.on_disconnect = cli_on_disconnect;
    g.c_bad_size_rc   = uvcpp_c_h3_conn_start(g.c_h3, &cbs, &g_bad);

    memset(&cbs, 0, sizeof(cbs));
    cbs.size            = (uint32_t)sizeof(cbs);
    cbs.on_stream_close = cli_on_stream_close;
    cbs.on_error        = cli_on_error;
    cbs.on_disconnect   = cli_on_disconnect;
    g.c_start_rc        = uvcpp_c_h3_conn_start(g.c_h3, &cbs, &g);
  }
  g.c_server_side = uvcpp_c_h3_conn_server_side(g.c_h3);
  /* `_start()` 之后 `ready()` 还是 0 —— 三条关键流要等 ALPN（**不是失败**） */
  CHECK_INT(uvcpp_c_h3_conn_ready(g.c_h3), 0);
  CHECK_INT(uvcpp_c_h3_conn_closed(g.c_h3), 0);

  /* 还没握手就发请求：`UV_EAGAIN`（"关键流还没开出来"）—— 那不是本层的错误
   * 码，是 libuv 的码，头里点着名说"这是正常会撞上的"。 */
  early = uvcpp_c_h3_request_new("GET", "/");
  CHECK(early != NULL);
  if (early != NULL) {
    const int64_t rc = uvcpp_c_h3_conn_send_request(g.c_h3, early);
    g.c_send_before_ready = (rc < 0) ? (int)rc : 0;
    CHECK_INT(uvcpp_c_h3_request_free(early), UVCPP_C_OK);
  }

  /* 流号没设的那枚响应：`_send_response()` 必须当场撂倒（"这条响应没归属"） */
  unset = uvcpp_c_h3_response_new(200);
  CHECK(unset != NULL);
  if (unset != NULL) {
    CHECK_INT(uvcpp_c_h3_response_set_body(unset, "x", 1), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h3_conn_send_response(g.c_h3, unset, 0), UV_EINVAL);
    CHECK_INT(uvcpp_c_h3_response_free(unset), UVCPP_C_OK);
  }

  /* ---- 握手 ---- */
  CHECK_INT(pump_both(handshaked, kDeadlineMs), 1);
  CHECK_INT(g.c_connect_calls, 1);
  CHECK_INT(g.c_connect_status, 0);

  /* ---- 两端的关键流都要开出来（ALPN 协商完之后）---- */
  CHECK_INT(pump_both(both_ready, kDeadlineMs), 1);
  CHECK_INT(g.connections, 1);
  CHECK_INT(g.start_rc, 0);
  CHECK_INT(g.c_start_rc, 0);
  CHECK_INT(g.server_side, 1);
  CHECK_INT(g.c_server_side, 0);
  g.c_ready = uvcpp_c_h3_conn_ready(g.c_h3);
  CHECK_INT(g.c_ready, 1);
  CHECK_INT(uvcpp_c_h3_conn_ready(g.s_h3), 1);
  /* ★ 三条关键单向流此刻**都开出来了**，而且是三条**不同**的流。这一句与上面
   * `srv_on_connection` 里那三个"还是 -1"是同一件事的前后两半：`_start()` 那
   * 一刻没开（要等 ALPN），ALPN 之后才开。两边单独看都只在说"某时是某个值"，
   * 合起来才说得清"什么时候开"。 */
  {
    const int64_t a = uvcpp_c_h3_conn_control_stream_id(g.s_h3);
    const int64_t b = uvcpp_c_h3_conn_qpack_encoder_stream_id(g.s_h3);
    const int64_t c = uvcpp_c_h3_conn_qpack_decoder_stream_id(g.s_h3);
    CHECK(a >= 0 && b >= 0 && c >= 0);
    CHECK(a != b && b != c && a != c);
    CHECK_INT(g.critical_open[0] + g.critical_open[1] + g.critical_open[2], 0);
    /* 客户端侧那三条也要开出来（它对服务端侧是另一组流号） */
    CHECK(uvcpp_c_h3_conn_control_stream_id(g.c_h3) >= 0);
    CHECK(uvcpp_c_h3_conn_qpack_encoder_stream_id(g.c_h3) >= 0);
    CHECK(uvcpp_c_h3_conn_qpack_decoder_stream_id(g.c_h3) >= 0);
    CHECK(uvcpp_c_h3_conn_control_stream_id(g.c_h3) != a);
  }

  /* 握手之后两边的 ALPN 都是 `"h3"`（双方给的就是那一个） */
  g.c_alpn_rc = uvcpp_c_h3_conn_alpn_selected(g.c_h3, buf, sizeof(buf));
  CHECK_INT(g.c_alpn_rc, 2);
  CHECK_STR(buf, "h3");
  CHECK_INT(uvcpp_c_h3_conn_alpn_selected(g.s_h3, buf, sizeof(buf)), 2);
  CHECK_STR(buf, "h3");
  /* 两端那两枚**借来的**句柄此刻都是 ESTABLISHED */
  CHECK_INT(uvcpp_c_quic_conn_state(g.sconn), UVCPP_C_QUIC_ESTABLISHED);
  CHECK_INT(uvcpp_c_quic_conn_state(g.cconn), UVCPP_C_QUIC_ESTABLISHED);
  CHECK_INT((int)uvcpp_c_quic_conn_streams_left(g.cconn, 1) > 0, 1);

  /* ---- 三轮串行 ---- */
  for (i = 0; i < kRounds; ++i) {
    g.want_rounds = i + 1;
    if (i == 0) {
      CHECK_INT(send_round(i, "POST", "/echo", kCliBody), 1);
    } else if (i == 1) {
      CHECK_INT(send_round(i, "GET", "/status", NULL), 1);
    } else {
      CHECK_INT(send_round(i, "GET", "/empty", NULL), 1);
    }
    CHECK_INT(pump_both(round_n_done, kDeadlineMs), 1);
  }

  /* ---------------------------------------------------------------
   * 服务端侧：三轮请求逐条对上
   * --------------------------------------------------------------- */
  CHECK_INT(g.requests, kRounds);
  CHECK_INT(g.errors, 0);
  CHECK_INT(g.request_before_ready, 0);
  CHECK_INT(g.bad_size_rc, UVCPP_C_E_INVALID_ARG);
  /* 端点级那张表：两侧各一次（服务端在 `srv_on_connection` 里，客户端在
   * `_connect()` 之前那个窗口里） */
  CHECK_INT(g.q_bad_size_rc, UVCPP_C_E_INVALID_ARG);
  CHECK_INT(g.c_q_bad_size_rc, UVCPP_C_E_INVALID_ARG);
  CHECK_INT(g.type_confusion_rc, UVCPP_C_E_STALE);
  CHECK_INT(g.stream_close_size_ok >= 1, 1);
  CHECK_INT(g.stream_close_errs, 0);
  CHECK_INT(g.closed_after_start, 0);
  CHECK_INT(g.h3_null, 0);
  CHECK_INT(g.c_h3_null, 1); /* connect 之前接不上去 */

  /* ★ 那张坏表**一点都没装上**：冒充的那份 fixture 上一个计数都没有。 */
  CHECK_INT(g_bad.connections, 0);
  CHECK_INT(g_bad.requests, 0);
  CHECK_INT(g_bad.stream_closes, 0);
  CHECK_INT(g_bad.disconnects, 0);
  CHECK_INT(g.c_bad_size_rc, UVCPP_C_E_INVALID_ARG);

  CHECK_INT(g.c_state_before, UVCPP_C_QUIC_IDLE);
  CHECK_INT(g.c_open_before, UVCPP_C_E_STATE);
  CHECK_INT(g.c_streams_left_before, 0);
  CHECK_INT(g.c_alpn_len_before, 0);
  CHECK_INT(g.c_send_before_ready, UV_EAGAIN);

  for (i = 0; i < kRounds; ++i) {
    CHECK_INT(g.send_rc[i], 0);
    CHECK_INT(g.body_rc[i], (int)g.body_size[i]);
    CHECK_INT(g.hdr_missing_rc[i], UVCPP_C_E_NOT_FOUND);
    CHECK_INT(g.c_missing_hdr_rc[i], UVCPP_C_E_NOT_FOUND);
  }

  CHECK_INT(g.method_rc[0], 4);
  CHECK_STR(g.method[0], "POST");
  CHECK_STR(g.path[0], "/echo");
  CHECK_STR(g.scheme[0], "https");
  CHECK_STR(g.authority[0], "localhost");
  CHECK_INT(g.hdr_rc[0], 1);
  CHECK_STR(g.hdr[0], kHdrLast);
  CHECK_BYTES(g.body[0], g.body_size[0], kCliBody, strlen(kCliBody));

  CHECK_STR(g.method[1], "GET");
  CHECK_STR(g.path[1], "/status");
  CHECK_INT((int)g.body_size[1], 0);
  CHECK_INT(g.hdr_rc[1], UVCPP_C_E_NOT_FOUND);

  CHECK_STR(g.method[2], "GET");
  CHECK_STR(g.path[2], "/empty");
  CHECK_INT((int)g.body_size[2], 0);
  CHECK_INT(g.hdr_rc[2], UVCPP_C_E_NOT_FOUND);

  /* ★ 回调期视图出了回调就作废。这里用的是**最后一次**回调留下的那个指针 ——
   * `alive()` 先查登记表、一个字节都不读那块已经出栈的内存（见文件头）。 */
  CHECK_INT(g.view_after_probed, 1);
  CHECK_INT(uvcpp_c_h3_request_view_method(g.saved_view, buf, sizeof(buf)),
            UVCPP_C_E_STALE);
  CHECK_INT((int)uvcpp_c_h3_request_view_body_size(g.saved_view), 0);
  CHECK_INT(uvcpp_c_h3_request_view_header(g.saved_view, "x-nope", buf,
                                           sizeof(buf)),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h3_request_view_stream_id(g.saved_view),
            UVCPP_C_E_STALE);

  /* ---------------------------------------------------------------
   * 客户端侧：三轮响应逐条对上
   * --------------------------------------------------------------- */
  CHECK_INT(g.c_errors, 0);
  CHECK_INT(g.c_stream_closes, kRounds);
  for (i = 0; i < kRounds; ++i) {
    /* 那条契约的四个数 */
    CHECK_INT(g.take_before[i], 1);
    CHECK_INT(g.take_rc[i], 1);
    CHECK_INT(g.take_after[i], 0);
    CHECK_INT(g.take_again[i], 0);
    /* 取之前问过状态码 —— `E_NOT_FOUND`，不是替对端编的 200 */
    CHECK_INT(g.c_status_rc[i], UVCPP_C_E_NOT_FOUND);
    /* 响应挂在它自己那条流上 */
    CHECK_INT(g.c_resp_stream_id[i], g.c_sent_id[i]);
    CHECK_INT(g.c_error[i], 0);
    CHECK_INT(g.c_body_rc[i], (int)g.c_body_size[i]);
  }
  CHECK(g.c_sent_id[0] >= 0 && g.c_sent_id[1] > g.c_sent_id[0] &&
        g.c_sent_id[2] > g.c_sent_id[1]);

  /* 第 1 轮：200 + content-type + body 逐字节 */
  CHECK_INT(g.c_status[0], 200);
  CHECK_INT(g.c_ct_rc[0], 10);
  CHECK_STR(g.c_ct[0], "text/plain");
  CHECK_INT((int)g.c_body_size[0], (int)strlen(kPong1));
  CHECK_BYTES(g.c_body[0], g.c_body_rc[0], kPong1, strlen(kPong1));
  /* 第 2 / 3 轮没设 content-type —— 查出来是 `E_NOT_FOUND`，不是空串 */
  CHECK_INT(g.c_ct_rc[1], UVCPP_C_E_NOT_FOUND);
  CHECK_INT(g.c_ct_rc[2], UVCPP_C_E_NOT_FOUND);

  /* 第 2 轮：201 + body 逐字节 */
  CHECK_INT(g.c_status[1], 201);
  CHECK_INT((int)g.c_body_size[1], (int)strlen(kMadeBody));
  CHECK_BYTES(g.c_body[1], g.c_body_rc[1], kMadeBody, strlen(kMadeBody));

  /* 第 3 轮：204 + **空 body**（不是"少收了一段"） */
  CHECK_INT(g.c_status[2], 204);
  CHECK_INT((int)g.c_body_size[2], 0);
  CHECK_INT(g.c_body_rc[2], 0);

  /* ---------------------------------------------------------------
   * 收尾：close 恰好一次、on_disconnect 恰好一次、借出的句柄被收回去
   * --------------------------------------------------------------- */
  CHECK_INT(uvcpp_c_h3_conn_close(g.c_h3, 0), 0);
  CHECK_INT(pump_both(closed_both, kDeadlineMs), 1);
  /* 再多泵 200ms，确认"恰好一次"的后半截 */
  {
    const uint64_t t0 = uv_hrtime();
    while ((uv_hrtime() - t0) < 200ULL * 1000000ULL) {
      uvcpp_c_quic_client_run_once(g.cli);
      uvcpp_c_quic_server_run_once(g.srv);
      uv_sleep(1);
    }
  }
  CHECK_INT(g.disconnects, 1);
  CHECK_INT(g.c_disconnects, 1);
  CHECK_INT(g.errors, 0);
  CHECK_INT(g.c_errors, 0);
  CHECK_INT(g_bad.disconnects, 0);

  /* h3 对象是在 `on_disconnect` 里 free 的（那一层要求必须做的事） */
  CHECK_INT(g.free_in_cb, UVCPP_C_OK);
  CHECK_INT(g.c_free_in_cb, UVCPP_C_OK);
  CHECK_INT(g.after_free_rc, UVCPP_C_E_STALE);
  CHECK_INT(g.c_after_free_rc, UVCPP_C_E_STALE);

  /* ALPN 与字节数：真的收发了字节，不是空跑一趟 */
  CHECK_INT(g.alpn_rc, 2);
  CHECK_STR(g.alpn, "h3");
  CHECK_INT(g.c_alpn_rc, 2);
  CHECK_STR(g.c_alpn, "h3");
  CHECK(g.c_bytes_out > 0);
  CHECK(g.c_bytes_in > 0);
  CHECK(g.bytes_in > 0);
  CHECK(g.bytes_out > 0);

  /* ★ 借来的 QUIC 连接句柄：连接一没，它就必须已经反登记 + 毒化。
   *
   * 这一条**不是**"顺手多断言一句"：它是这一批唯一量得出 M18 的地方 ——
   * 装上 h3 之后，那条连接上的回调表被 h3 整个换掉，本模块 `on_close` 那套
   * 跳板一次都不响；少一个入口，这一枚句柄就永远留在登记表里，而它的地址
   * （客户端那一枚是嵌在客户端壳里的）要等到端点被释放才消失。
   * 直接证据是下面那句 `uvcpp_c_live_handle_count() == 0`。 */
  CHECK_INT(uvcpp_c_quic_conn_state(g.cconn), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_quic_conn_state(g.sconn), UVCPP_C_E_STALE);
  CHECK_INT((int)uvcpp_c_quic_conn_open_stream(g.cconn, 1), UVCPP_C_E_STALE);
  CHECK_INT((int)uvcpp_c_quic_conn_open_stream(g.sconn, 1), UVCPP_C_E_STALE);

  /* 端点 `_stop()` 各过一遍，且可重复调 */
  CHECK_INT(uvcpp_c_quic_client_stop(g.cli), 0);
  CHECK_INT(uvcpp_c_quic_client_stop(g.cli), 0);
  CHECK_INT(uvcpp_c_quic_server_stop(g.srv), 0);
  CHECK_INT(uvcpp_c_quic_server_stop(g.srv), 0);

  /* ---- 释放顺序：端点在前、TLS 在后（TLS 的生命周期必须盖住连接）---- */
  CHECK_INT(uvcpp_c_quic_client_free(g.cli), 0);
  CHECK_INT(uvcpp_c_quic_client_free(g.cli), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_quic_server_free(g.srv), 0);
  CHECK_INT(uvcpp_c_quic_server_free(g.srv), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_quic_tls_free(g.tls_cli), 0);
  CHECK_INT(uvcpp_c_quic_tls_free(g.tls_cli), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_quic_tls_free(g.tls_srv), 0);
  CHECK_INT(uvcpp_c_quic_crypto_free(), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_quic_crypto_free(), UVCPP_C_OK); /* 可重复调 */

  /* ★ 登记表的收支平衡：一个都不许剩。
   *
   * 跑完这一趟，所有调用方所有的句柄都废过了，所有借出的句柄都该由"底层连接
   * 结束"那两件事（裸 QUIC 的 `on_close` / 装了 h3 时的 `on_disconnect`）反登记
   * 掉。剩一个 —— 不管是哪一个 —— 就是一次泄漏，而且"再用必须 E_STALE"那套
   * 断言**量不出**它（见 `uvcpp_c_common.h` 里 `uvcpp_c_live_handle_count()`
   * 那一段）。 */
  CHECK_INT(uvcpp_c_live_handle_count(), 0);

  printf("[capi quic+h3] checks=%d failures=%d\n", g_checks, g_failed);
  if (g_failed != 0) {
    printf("[capi quic+h3] FAILED: %d\n", g_failed);
    return 1;
  }
  printf("[capi quic+h3] PASS\n");
  return 0;
}
