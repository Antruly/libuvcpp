/**
 * @file tests/capi/capi_h2_func.c
 * @brief http2 层的 C ABI 端到端用例：**真起两条 h2 连接、真发三个请求、真收字节**。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这份用例是**纯 C**（`.c`，不 include 任何 C++ 头）。跑起来是两个真端点：
 * 主线程是服务端（`uvcpp_c_tcp_server` 那条循环，接受一条连接后在这条连接上建
 * **服务端侧**的 `uvcpp_c_h2_connection`），另一条线程是客户端（`uvcpp_c_tcp_client`
 * 自己的循环，建**客户端侧**的 h2 连接）。一次完整的往返是三条流：
 *
 *   | 流 | 客户端 | 服务端怎么回 | 断言 |
 *   |----|--------|--------------|------|
 *   | 1  | `POST /echo` + 14 字节 body（带 `content-length: 14`） | `send_response`（200 + body） | 状态码、`content-type` 往返、body **逐字节**相等；服务端那侧 `expected_body` = **14** |
 *   | 3  | `GET /status`（无 body、无 `content-length`） | `send_status`（201 + body） | 状态码 201、body 逐字节；服务端那侧 `expected_body` = **E_NOT_FOUND** |
 *   | 5  | `GET /stream` | `send_headers` + 两块 `send_data` | body **逐字节**相等、两个 `done` **恰好各跑一次**；服务端那侧 `expected_body` = **E_NOT_FOUND** |
 *
 * ★ `expected_body` 这一列**故意只在服务端那一侧断言** —— 那一句的语义是**不对称**的，
 *   而且这个不对称来自下一层：`uvcpp_h2_stream` 的 `has_content_length` /
 *   `expected_body` 这一对**只在解请求头时填**（`uvcpp_h2_session.cpp:393-414`，
 *   在 `handle_regular()` 里，那条路只有服务端才走），客户端解响应头走的是另一条
 *   分支，那里只把 `content-length` 原样收进 `response.headers`。
 *   于是：客户端在 `expected_body()` 上**永远**拿到 `E_NOT_FOUND`（本用例也照实
 *   断言这一点，三条流都是），客户端要知道响应体长度得读
 *   `uvcpp_c_h2_stream_response_header(st, "content-length", …)` —— 那是**那个头**
 *   本身，两边都收得到。把"永远不成立的那一侧"写进断言，是个假判据；写进注释
 *   说明它为什么不成立，才是判据。
 *
 * 为什么**不开 TLS**：本层（`uvcpp_h2_connection`）只做"字节从哪来、往哪去"，
 * 它拿的是一个已经能在上面读写的 `uvcpp_tcp_client` —— 它内部**没有任何一句**
 * 读 `tls_alpn_selected()`（`src/http2/uvcpp_h2_connection.cpp` 全文没有 alpn /
 * ssl 的字样）。ALPN 是与**真对端**互通的前提，不是这一层的机制。所以这条用例
 * 走明文 TCP：判据落在"转换与交付"上，而不是"OpenSSL 装对了没有"。
 * （接 TLS + ALPN 的那条路由 `tests/functional/web_ssl_h2_*.cpp` 覆盖，
 * 它跑的是 C++ 那一侧；C 侧本批**刻意不给** TLS，见 `uvcpp_c_net.h` 的"不提供"。）
 *
 * 除了"字节对不对"这条主线，它还钉住六条**只有 C 面才有**的规矩：
 *
 *   1. **回调期句柄出了回调就作废**：`on_response_end` 里存下来的
 *      `uvcpp_c_h2_stream*`，回调返回之后再用必须是 `E_STALE`（不是崩溃，也不是
 *      读到栈上那块被复用的内存）；
 *   2. **每个类型一枚魔数**：拿一个 `uvcpp_c_h2_request*` 冒充
 *      `uvcpp_c_h2_response*` 传进去，必须是 `E_STALE` —— 共用一枚的话这会一路
 *      走到读错布局的地方；
 *   3. **回调表按 `size` 逐格读**：`size = 3` 的表**整张都不读**，返回
 *      `E_INVALID_ARG`（而不是"当作空表继续"）；
 *   4. **线程规则**：从服务端那条线程上碰客户端侧的连接句柄，必须是
 *      `E_WRONG_THREAD`；
 *   5. **"没有状态码"与"状态码是 0"分得开**：服务端在请求回调里问
 *      `response_status()` 必须是 `E_NOT_FOUND`（`h2_response_not_received()`
 *      填的是 `HTTP_STATUS_NONE`）—— 绝不能是一个替对端编出来的 200；
 *   6. **"没带这个头"与"带了但值是空的"分得开**：查一个不存在的头是
 *      `E_NOT_FOUND`，而 `host` 查得到。
 *
 * 以及两条收尾纪律：**在 `on_disconnect` 里 free 句柄是允许的**（C++ 侧的契约
 * 就是"持有者在这里销毁本对象"），free 之后同一个句柄一律 `E_STALE`。
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
#if !UVCPP_NGHTTP2_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_NGHTTP2=ON 的构建里被编译"
#endif

static int g_checks = 0;
static int g_failed = 0;

/* 与 `capi_net_func.c` / `capi_webapp_func.c` 同一个形状：每次判定记一笔，
 * 末尾印 `checks=… failures=…`。`tests/tools/` 下那些驱动靠这一行区分
 * "是它红的"和"被别人的红带下去的"。 */
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
    int _g = (int)(got);                                                 \
    int _w = (int)(want);                                                \
    ++g_checks;                                                          \
    if (_g != _w) {                                                      \
      printf("FAIL %s:%d: %s = %d, want %d\n", __FILE__, __LINE__, #got,  \
             _g, _w);                                                    \
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

/* 三条流各说什么 —— 长度**互不相同**且都不是 2 的幂，这样"哪条流的 body
 * 混进了哪条"不会因为长度一样而看不出来。 */
#define kCliBody     "client-says-hi"      /* 14 字节，流 1 的请求体 */
#define kSrvBody     "h2-hello-from-server" /* 20 字节，流 1 的响应体 */
#define kStatusBody  "made"                /* 4 字节，流 3 的响应体 */
#define kChunk1      "chunk-1|"            /* 8 字节 */
#define kChunk2      "chunk-2"             /* 7 字节 */
#define kStreamBody  (kChunk1 kChunk2)     /* 15 字节 */

#define kAccumMax 128
#define kStreams  3

/* -------------------------------------------------------------------------
 * 累计器：TCP 是字节流，h2 的 DATA 帧也不保证一次给完
 * ------------------------------------------------------------------------- */
typedef struct {
  char buf[kAccumMax];
  int  len;
} accum;

static void accum_reset(accum* a) { a->len = 0; }

static void accum_push(accum* a, const char* data, size_t size) {
  if (data == NULL) return;
  if (a->len + (int)size > kAccumMax) return;
  memcpy(a->buf + a->len, data, size);
  a->len += (int)size;
}

static int accum_is(const accum* a, const char* want) {
  size_t n = strlen(want);
  return a->len == (int)n && memcmp(a->buf, want, n) == 0;
}

/* -------------------------------------------------------------------------
 * 两侧共用的"按流号记账"的槽
 * -------------------------------------------------------------------------
 * 三条流在一条连接上并发跑，观测值必须按流号分开记 —— 一个连接级标量会把
 * 三份结果互相盖掉，而那是"看起来有值、其实哪条都不是"的一种。
 */
/**
 * @brief 客户端侧的连接句柄。
 *
 * 客户端线程在跑循环之前把它记在这里，好让**服务端那条线程**在一条流还活着
 * 的时候碰它一下 —— 那是 `E_WRONG_THREAD` 唯一量得出来的时机（等到两边都收工
 * 之后，句柄早就被自己废掉了，那时量到的是 `E_STALE`，两件事长得不一样）。
 * 不加锁：写入发生在客户端发第一个字节**之前**，而读到它必然在服务端收到那些
 * 字节**之后**。
 */
static uvcpp_c_h2_connection* g_client_conn = NULL;

typedef struct {
  int  seen;
  int  status;             /* response_status() 的返回值 */
  int  status_rc;          /* 同上那次调用的返回码（E_NOT_FOUND 之类） */
  int  resp_end;           /* on_response_end 到过几次 */
  /* 这一对名字必须分开，因为它们是**两条不同的路**、返回码也不同：
   * `exp_body_*` 是 `expected_body()`（本层维护的那个数，客户端侧永远是
   * `E_NOT_FOUND`），`cont_len_*` 是 `content-length` **那个头**（客户端侧该走
   * 的路）。合成一个字段的话，"两条路各自对不对"就量不出来了。 */
  int  exp_body_rc;
  int  cont_len_rc;
  size_t cont_len;
  accum body;
} stream_slot;

/* -------------------------------------------------------------------------
 * 服务端
 * ------------------------------------------------------------------------- */

typedef struct {
  uvcpp_c_tcp_server*  srv;
  uvcpp_c_tcp_client*  tcp;         /* 服务端交出来的那条连接 */
  uvcpp_c_h2_connection* h2;
  int                  connections;

  int  requests;                    /* on_request 到过几次 */
  int  request_ends;
  int  request_closes;              /* on_close */
  int  fatals;
  int  disconnects;
  int  free_in_cb;                  /* 在 on_disconnect 里 free 的返回码 */

  /* 每条流的请求侧观测值（按流号 - 1） */
  int  method_name_rc[kStreams];
  char method_name[kStreams][16];
  int  path_rc[kStreams];
  char path[kStreams][64];
  int  host_ok[kStreams];           /* has_header("host") */
  int  host_rc[kStreams];
  char host[kStreams][64];
  int  missing_rc[kStreams];        /* 查一个不存在的头 */
  int  missing_len[kStreams];       /* 同上，cap=0 先问长度那条路 */
  int  req_body_bytes[kStreams];    /* stream_body_bytes() 在 on_request_end 里 */
  /* ★ `expected_body()` 的**主场**在这张表的服务端一侧：请求方向的
   * `content-length` 只有这条路上有人维护（见本文件头部 ★ 那一段）。 */
  int  exp_body_rc[kStreams];
  int  exp_body[kStreams];
  int  req_state[kStreams];
  int  req_paused[kStreams];
  int  resp_status_before[kStreams];/* ★ 请求回调里问响应状态：必须 E_NOT_FOUND */
  int  req_stream_id_by_handle[kStreams];
  accum req_body;                   /* 流 1 的请求体 */

  /* 出的字节数、对端道别与否（在 on_disconnect 里读） */
  size_t bytes_in;
  size_t bytes_out;
  int    peer_goaway;
  uint32_t peer_goaway_code;
  int32_t  peer_last_stream;

  /* `send_data` 的 done：**恰好两次**，各带一个"受理后"的码 */
  int done_calls;
  int done_status[4];

  int goaway_rc;
  int after_free_rc;                /* free 之后再碰这个句柄 */

  /* 跨线程那条：从服务端线程碰客户端侧的连接句柄 */
  int cross_thread_flush_rc;
  int cross_thread_probed;
} server_side;

static server_side g_ss;

/* --- 服务端的请求侧回调 ------------------------------------------------ */

/** 把流号换算成槽下标（客户端发起的流号是 1 / 3 / 5……）。 */
static int slot_of(int32_t sid) {
  int i = (int)(sid / 2);
  if (i < 0 || i >= kStreams) return -1;
  return i;
}

/* `send_data` 的完成回调在下面才定义，但 `srv_reply()` 要用它。 */
static void srv_done_cb(void* ud, int status);

/*
 * 一条流**只许回一次**：`submit_response` 第二次会换掉一块正被 nghttp2 的
 * data provider 按地址引用的缓冲。本用例每条流都在 `on_request_end` 里回一次，
 * 而 `on_request_end` 对一条流只会来一次。
 */
static void srv_reply(server_side* ss, int32_t sid) {
  const int slot = slot_of(sid);
  if (slot < 0) return;

  if (slot == 0) {
    /* 流 1：一份带 body 的完整响应 —— 走构造器那一套。 */
    uvcpp_c_h2_response* resp = uvcpp_c_h2_response_new(200);
    CHECK(resp != NULL);
    if (resp == NULL) return;
    CHECK_INT(uvcpp_c_h2_response_set_content_type(resp, "text/plain"),
              UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_set_header(resp, "x-srv", "one"), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_set_body(resp, kSrvBody, strlen(kSrvBody)),
              UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_conn_send_response(ss->h2, sid, resp, 0), UVCPP_C_OK);
    /* 交出去之后立刻废掉构造器：这正是"入参立刻拷贝"那条规矩的判据 ——
     * 交出去的那一份不受影响（下面客户端收到的 body 会证明这一点）。 */
    CHECK_INT(uvcpp_c_h2_response_free(resp), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_free(resp), UVCPP_C_E_STALE);
  } else if (slot == 1) {
    /* 流 3：最小响应（只有 :status 与 content-length）。 */
    CHECK_INT(uvcpp_c_h2_conn_send_status(ss->h2, sid, 201, kStatusBody,
                                          strlen(kStatusBody)),
              UVCPP_C_OK);
  } else {
    /* 流 5：流式 —— 先头部（不结束流），再两块 body，最后一块 END_STREAM。
     *
     * 这一条路上 `content-length` **一个字节都不写**（`submit_headers()`
     * 明说了"不补"），所以客户端读 `content-length` 这个头必须是
     * `E_NOT_FOUND` 而不是 "0" —— 见本文件头部那张表的第 5 行。 */
    uvcpp_c_h2_response* resp = uvcpp_c_h2_response_new(200);
    CHECK(resp != NULL);
    if (resp == NULL) return;
    CHECK_INT(uvcpp_c_h2_response_set_content_type(resp, "text/plain"),
              UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_conn_send_headers(ss->h2, sid, resp), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_free(resp), UVCPP_C_OK);

    CHECK_INT(uvcpp_c_h2_conn_send_data(ss->h2, sid, kChunk1, strlen(kChunk1),
                                        0, srv_done_cb, ss),
              UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_conn_send_data(ss->h2, sid, kChunk2, strlen(kChunk2),
                                        1, srv_done_cb, ss),
              UVCPP_C_OK);
  }
}

static void srv_on_request(void* ud, uvcpp_c_h2_connection* c,
                           uvcpp_c_h2_stream* st, int end_stream) {
  server_side* ss = (server_side*)ud;
  char buf[64];
  int rc;
  int32_t sid;
  int slot;

  (void)c;
  (void)end_stream;
  ++ss->requests;

  sid  = uvcpp_c_h2_stream_id(st);
  slot = slot_of(sid);
  if (slot < 0) return;

  ss->req_stream_id_by_handle[slot] = (int)sid;
  CHECK_INT(uvcpp_c_h2_stream_state(st), UVCPP_C_H2_OPEN);
  CHECK_INT(uvcpp_c_h2_stream_rejected(st), 0);
  CHECK_INT(uvcpp_c_h2_stream_paused(st), 0);

  /* ★ 跨线程：在服务端这条线程上碰**客户端侧**的连接句柄。改状态的入口必须
   * 一律拒（`E_WRONG_THREAD`）—— 这是"一个句柄只在它那条循环的线程上用"
   * 那条纪律唯一能被自动抓到的形态。只在第一条流上做一次：这是个诊断性的
   * 并发读，做多了没有多出来的信息。 */
  if (slot == 0 && g_client_conn != NULL) {
    ss->cross_thread_flush_rc = uvcpp_c_h2_conn_flush(g_client_conn);
    ss->cross_thread_probed   = 1;
  }

  /* ★ 请求到了、响应还没回：这一层必须给出"没有状态码"，而不是替对端编一个
   * 200 —— `h2_response_not_received()` 把 `status_code` 填成 `HTTP_STATUS_NONE`，
   * 这里读出来必须是那个新错误码 `E_NOT_FOUND`。 */
  ss->resp_status_before[slot] = uvcpp_c_h2_stream_response_status(st);
  CHECK_INT(ss->resp_status_before[slot], UVCPP_C_E_NOT_FOUND);

  /* 方法名 */
  rc = uvcpp_c_h2_stream_request_method_name(st, NULL, 0);
  ss->method_name_rc[slot] = rc;
  if (rc >= 0 && rc < (int)sizeof(ss->method_name[slot])) {
    CHECK_INT(uvcpp_c_h2_stream_request_method_name(
                  st, ss->method_name[slot], (size_t)rc + 1),
              rc);
  }
  /* 路径 */
  rc = uvcpp_c_h2_stream_request_path(st, NULL, 0);
  ss->path_rc[slot] = rc;
  if (rc >= 0 && rc < (int)sizeof(ss->path[slot])) {
    CHECK_INT(uvcpp_c_h2_stream_request_path(st, ss->path[slot], (size_t)rc + 1),
              rc);
  }
  /* 缓冲区不够时**一个字节都不写**：拿一个 1 字节的缓冲区问一条至少 4 字节的
   * 路径，返回值必须是真实长度，而那一格必须原样不动。 */
  buf[0] = '\x5a';
  buf[1] = '\x5a';
  CHECK_INT(uvcpp_c_h2_stream_request_path(st, buf, 1), rc);
  CHECK_INT((unsigned char)buf[0], 0x5a);
  CHECK_INT((unsigned char)buf[1], 0x5a);

  /* 带了的头查得到、没带的头是 E_NOT_FOUND（不是"空串"） */
  ss->host_ok[slot] = uvcpp_c_h2_stream_request_has_header(st, "host");
  CHECK_INT(ss->host_ok[slot], 1);
  rc = uvcpp_c_h2_stream_request_header(st, "host", ss->host[slot],
                                        sizeof(ss->host[slot]));
  ss->host_rc[slot] = rc;
  CHECK(rc >= 1);

  ss->missing_rc[slot] =
      uvcpp_c_h2_stream_request_header(st, "x-not-there", NULL, 0);
  CHECK_INT(ss->missing_rc[slot], UVCPP_C_E_NOT_FOUND);
  CHECK_INT(uvcpp_c_h2_stream_request_has_header(st, "x-not-there"), 0);
  /* 同一个头，第二次问出来还是 E_NOT_FOUND（不是"第一次没找到、第二次给空串"） */
  ss->missing_len[slot] = uvcpp_c_h2_stream_request_header(st, "x-not-there",
                                                           buf, sizeof(buf));
  CHECK_INT(ss->missing_len[slot], UVCPP_C_E_NOT_FOUND);
}

static void srv_on_request_end(void* ud, uvcpp_c_h2_connection* c,
                               uvcpp_c_h2_stream* st) {
  server_side* ss = (server_side*)ud;
  int32_t sid = uvcpp_c_h2_stream_id(st);
  int     slot = slot_of(sid);

  (void)c;
  ++ss->request_ends;
  if (slot < 0) return;

  ss->req_body_bytes[slot] = (int)uvcpp_c_h2_stream_body_bytes(st);
  ss->req_state[slot]      = uvcpp_c_h2_stream_state(st);

  /* ★ 请求方向宣告的 `content-length` —— 这一句的**主场**。流 1 的请求带着
   * `content-length: 14`，流 3 / 5 的 GET 一个字节都没带，所以这里是
   * "14" 与 "E_NOT_FOUND" 两组值，而不是三条一样的。 */
  {
    size_t n = 0x5a5a;
    ss->exp_body_rc[slot] = uvcpp_c_h2_stream_expected_body(st, &n);
    ss->exp_body[slot]    = (ss->exp_body_rc[slot] == UVCPP_C_OK) ? (int)n : -1;
  }

  srv_reply(ss, sid);
}

static void srv_on_body(void* ud, uvcpp_c_h2_connection* c,
                        uvcpp_c_h2_stream* st, const char* data, size_t len) {
  server_side* ss = (server_side*)ud;
  (void)c;
  (void)st;
  accum_push(&ss->req_body, data, len);
}

static void srv_on_close(void* ud, uvcpp_c_h2_connection* c, int32_t stream_id,
                         uint32_t error_code) {
  server_side* ss = (server_side*)ud;
  (void)c;
  (void)stream_id;
  (void)error_code;
  ++ss->request_closes;
}

static void srv_on_fatal(void* ud, uvcpp_c_h2_connection* c,
                         int nghttp2_error) {
  server_side* ss = (server_side*)ud;
  (void)c;
  (void)nghttp2_error;
  ++ss->fatals;
}

/**
 * @brief `send_data` 的完成通知。
 *
 * 判据是**恰好两次**（每个块一次）。参数允许两个值：0（块真的上线了）或
 * `UV_ECANCELED`（连接在它上线之前就没了 —— 那条路上本层保证会把它跑掉，
 * 见 `uvcpp_c_h2_conn_free()` 里那段）。别的值一律算错。
 */
static void srv_done_cb(void* ud, int status) {
  server_side* ss = (server_side*)ud;
  if (ss->done_calls < (int)(sizeof(ss->done_status) / sizeof(int))) {
    ss->done_status[ss->done_calls] = status;
  }
  ++ss->done_calls;
}

static void srv_on_disconnect(void* ud, uvcpp_c_h2_connection* c) {
  server_side* ss = (server_side*)ud;

  ++ss->disconnects;

  /* 诊断读：这些都必须在**销毁之前**问 —— 对象一没就什么都问不到了。 */
  ss->bytes_in  = uvcpp_c_h2_conn_bytes_in(c);
  ss->bytes_out = uvcpp_c_h2_conn_bytes_out(c);
  ss->peer_goaway      = uvcpp_c_h2_conn_peer_goaway_received(c);
  ss->peer_goaway_code = uvcpp_c_h2_conn_peer_goaway_error_code(c);
  ss->peer_last_stream = uvcpp_c_h2_conn_peer_goaway_last_stream_id(c);
  CHECK_INT(uvcpp_c_h2_conn_closed(c), 1);

  /* ★ 在这条回调里销毁是**被允许的** —— 那正是 C++ 侧 `on_disconnect` 的契约
   * （"持有者应当在这里把它销毁"，见 `uvcpp_h2_connection.h:48`）。 */
  ss->free_in_cb = uvcpp_c_h2_conn_free(c);
  CHECK_INT(ss->free_in_cb, UVCPP_C_OK);
  /* 销毁之后这个句柄一律作废 —— 不是崩溃，也不是"还能读到一个旧值"。 */
  ss->after_free_rc = uvcpp_c_h2_conn_flush(c);
  CHECK_INT(ss->after_free_rc, UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h2_conn_free(c), UVCPP_C_E_STALE);

  uvcpp_c_tcp_server_stop(ss->srv);
}

static void srv_on_connection(void* ud, uvcpp_c_tcp_client* client) {
  server_side* ss = (server_side*)ud;

  uvcpp_c_h2_callbacks     h2c;
  uvcpp_c_h2_conn_callbacks cc;

  ++ss->connections;
  ss->tcp = client;

  CHECK_INT(uvcpp_c_tcp_client_is_connected(client), 1);

  ss->h2 = uvcpp_c_h2_connection_new(client, 1);
  CHECK(ss->h2 != NULL);
  if (ss->h2 == NULL) {
    uvcpp_c_tcp_server_stop(ss->srv);
    return;
  }

  /* ★ 先撞一次"`size` 连自己这一格都没盖住"的表：整张表**都不读**，返回
   * `E_INVALID_ARG`（而不是"当作空表继续"）。这一步必须在真 start 之前 ——
   * 它证明的正是"拒绝发生在任何动作之前"。 */
  {
    union {
      uvcpp_c_h2_callbacks t;
      unsigned char raw[sizeof(uvcpp_c_h2_callbacks)];
    } bad;
    memset(&bad, 0, sizeof(bad));
    bad.t.size       = 3;               /* < sizeof(uint32_t) */
    /* 后面几格摆上认得出的哨兵：要是本层读满了，下面那次 start 就会把它们
     * 装上，于是"回调收到的 user_data 是 ss"这条断言会照实报出来。 */
    bad.t.on_request = srv_on_request;
    bad.t.on_close   = srv_on_close;
    CHECK_INT(uvcpp_c_h2_conn_start(ss->h2, &bad.t, NULL,
                                    (void*)(uintptr_t)0x5a5a5a5a5a5a5a5au),
              UVCPP_C_E_INVALID_ARG);
  }

  /* ★ 第二张表也是**独立**的一处检查：这一次 `h2` 表干脆缺席（NULL），坏的
   * 是 `conn` 表。★ 这一块不是凑数 —— 上面那两句只盖住了 `start` 里两条
   * `table_size_ok` 中的一条：`capi_mutation.py` 的 M15 拆的是另一条，第一次
   * 跑的时候**没抓住**（四个用例全绿），追下去才发现用例只喂了 `h2` 表那一条
   * 路。两张表各喂一次，那两条守卫才各有自己的判据。 */
  {
    uvcpp_c_h2_conn_callbacks bad_cc;
    memset(&bad_cc, 0, sizeof(bad_cc));
    bad_cc.size          = 0;
    bad_cc.on_disconnect = srv_on_disconnect;
    CHECK_INT(uvcpp_c_h2_conn_start(ss->h2, NULL, &bad_cc, ss),
              UVCPP_C_E_INVALID_ARG);
  }

  memset(&h2c, 0, sizeof(h2c));
  h2c.size           = (uint32_t)sizeof(h2c);
  h2c.on_request     = srv_on_request;
  h2c.on_request_end = srv_on_request_end;
  h2c.on_body        = srv_on_body;
  h2c.on_close       = srv_on_close;
  h2c.on_fatal       = srv_on_fatal;

  memset(&cc, 0, sizeof(cc));
  cc.size          = (uint32_t)sizeof(cc);
  cc.on_disconnect = srv_on_disconnect;

  CHECK_INT(uvcpp_c_h2_conn_start(ss->h2, &h2c, &cc, ss), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_h2_conn_closed(ss->h2), 0);
  CHECK_INT(uvcpp_c_h2_conn_closing(ss->h2), 0);
  CHECK_INT((int)uvcpp_c_h2_conn_stream_count(ss->h2), 0);
  CHECK_INT(uvcpp_c_h2_conn_last_error(ss->h2), 0);
}

/* --- 客户端 ------------------------------------------------------------ */

typedef struct {
  int  port;
  uvcpp_c_tcp_client*    tcp;
  uvcpp_c_h2_connection* h2;

  int  connect_status;
  int  connect_rc;
  int  start_rc;
  int  sids[3];                 /* submit_request 的返回值 */
  int  free_rc;
  int  req_free_rc;

  int  resp_end_total;
  int  resp_total;
  int  body_events;
  int  closes;
  int  fatals;
  int  disconnects;
  int  free_in_cb;
  int  after_free_rc;

  stream_slot slot[kStreams];

  /* ★ 回调期句柄：出回调再用必须 E_STALE */
  const uvcpp_c_h2_stream* escaped;

  /* 类型冒充：拿请求句柄当响应句柄用 */
  int  type_confusion_rc;

  size_t bytes_in;
  size_t bytes_out;
  int    peer_goaway;

  int  shutdown_rc;
} client_side;

static client_side g_cs;

static int cli_slot_of(int32_t sid) {
  int i = (int)(sid / 2);
  if (i < 0 || i >= kStreams) return -1;
  return i;
}

static void cli_on_response(void* ud, uvcpp_c_h2_connection* c,
                            uvcpp_c_h2_stream* st, int end_stream) {
  client_side* cs = (client_side*)ud;
  int32_t sid = uvcpp_c_h2_stream_id(st);
  int     slot = cli_slot_of(sid);
  int     rc;
  char    buf[64];

  (void)c;
  (void)end_stream;
  ++cs->resp_total;
  if (slot < 0) return;

  cs->slot[slot].seen = 1;
  cs->slot[slot].status = uvcpp_c_h2_stream_response_status(st);
  cs->slot[slot].status_rc = UVCPP_C_OK;

  /* 响应头到了：`content-type` 往返。它由服务端的 `set_content_type()` 写进去，
   * 客户端读出来必须逐字节一致 —— 这条同时量了"我们发出去的头部块"与
   * "对端解出来的头部块"两侧。
   *
   * 流 3 是个例外，而且这个例外本身是判据：那条响应走的是
   * `send_status()`，它按 C++ 侧的约定**只写 `:status` 与 `content-length`**，
   * 一个别的头都不补。所以这里必须查不到 —— 查得到就说明有人替对端多写了一个
   * 它没说的头。 */
  buf[0] = '\0';
  rc = uvcpp_c_h2_stream_response_header(st, "content-type", buf, sizeof(buf));
  if (slot == 1) {
    CHECK_INT(rc, UVCPP_C_E_NOT_FOUND);
  } else {
    CHECK_INT(rc, (int)strlen("text/plain"));
    CHECK_STR(buf, "text/plain");
  }

  /* 流 1 还有一个自定义头 `x-srv`（默认的 `content-type` 打不到这条）。 */
  if (slot == 0) {
    rc = uvcpp_c_h2_stream_response_header(st, "x-srv", buf, sizeof(buf));
    CHECK_INT(rc, (int)strlen("one"));
    CHECK_STR(buf, "one");
  }

  /* ★ 客户端这一侧 `expected_body()` **永远**是 `E_NOT_FOUND` —— 三条流都一样，
   * 而这不是 bug，是这一层的语义本来就只维护**请求方向**的那一对
   * （`has_content_length` / `expected_body` 只在解请求头时填，见本文件头部
   * ★ 那一段）。这里照实断言"永远不成立"，而不是删掉这个断言：
   *   - 删掉 → 一个"客户端上它突然开始给数"的改动（比如有人顺手去响应头那条
   *     路上也填了这一对）不会有人发现；
   *   - 断言"给数" → 那是把假判据写成判据，它会红，而且红得没道理。
   * 那条"out 不被写"的断言也在：返回非 0 时写 out 是 FFI 面最常见的越界写。 */
  {
    size_t n = 0x5a5a;
    rc = uvcpp_c_h2_stream_expected_body(st, &n);
    cs->slot[slot].exp_body_rc = rc;
    CHECK_INT(rc, UVCPP_C_E_NOT_FOUND);
    CHECK_INT((int)n, 0x5a5a);
  }

  /* 客户端**该走的那条路**：读 `content-length` **这个头**，再自己转成数
   * （C# 侧就是 `int.Parse`）。本层不给"响应体长度"那种便利函数 —— 那会多出
   * 一个"头在但值不是数"的分支要定义，而那个分支属于协议违例，不属于这一层
   * 该替调用方做的决定。
   *
   * 三条流在这里正好是三份不同的观测：流 1 是 `send_response`（有 body，C++ 侧
   * 补 `content-length`）、流 3 是 `send_status`（按 C++ 侧约定必写）、流 5 是
   * `send_headers` 起的流式响应（那一步**一个字节都不补**）。最后一条给出的是
   * `E_NOT_FOUND` 而不是 "0" —— "没有这个头"与"这个头说 0"是两件事。 */
  {
    char hb[32];
    rc = uvcpp_c_h2_stream_response_header(st, "content-length", hb, sizeof(hb));
    cs->slot[slot].cont_len_rc = rc;
    if (slot == 2) {
      CHECK_INT(rc, UVCPP_C_E_NOT_FOUND);
      cs->slot[slot].cont_len = 0;
    } else {
      const size_t want = (slot == 0) ? strlen(kSrvBody) : strlen(kStatusBody);
      char         want_s[32];
      CHECK(rc >= 1);
      snprintf(want_s, sizeof(want_s), "%d", (int)want);
      CHECK_STR(hb, want_s);
      cs->slot[slot].cont_len = (size_t)strtoul(hb, NULL, 10);
      CHECK_INT((int)cs->slot[slot].cont_len, (int)want);
    }
  }
}

static void cli_on_response_end(void* ud, uvcpp_c_h2_connection* c,
                                uvcpp_c_h2_stream* st) {
  client_side* cs = (client_side*)ud;
  int32_t sid = uvcpp_c_h2_stream_id(st);
  int     slot = cli_slot_of(sid);
  int     state;

  ++cs->resp_end_total;
  if (slot < 0) return;
  cs->slot[slot].resp_end++;

  state = uvcpp_c_h2_stream_state(st);
  /* **客户端**侧这条流停在 `OPEN` —— 而且这是量出来的，不是推出来的。
   *
   * `uvcpp_h2_session.cpp` 里 `state = …` 一共三处，全在**响应**那两个提交口上
   * （`submit_status` / `submit_response` / `submit_headers`，即 1011 / 1050 /
   * 1167 三行），那几条路只有服务端走。客户端那条路走的是
   * `submit_request()`，它**只**登记 `s.request` 与
   * `s.request.version`（该文件 1291-1293 行），一个字节都不碰 `state`，
   * 所以 `uvcpp_h2_stream` 的默认值原样留到这里：`OPEN`。
   *
   * 这一条一开始写的是 `SENT`，跑出来是 0（`OPEN`），本用例把它照实改了过来
   * —— 断言写"应该是哪个状态"的地方，最容易犯的错就是把**别的那条路**上读到的
   * 一处赋值当成全局规律。`CLOSED` 与 `REJECTED` 同理：整仓没有一处给它们赋过
   * 值（`uvcpp_h2_common.h:170-172` 自己写着），所以它们不该出现在任何断言里。 */
  CHECK_INT(state, UVCPP_C_H2_OPEN);

  /* 回调期句柄可以问；**出了回调就不行** —— 把它记下来，等循环跑完再问一次。 */
  cs->escaped = st;

  if (cs->resp_end_total == kStreams) {
    /* 三条都收完了：走主动关。GOAWAY 先发出去，对端在 PEER_CLOSED 之前一定会
     * 读到它（同一条字节流上的 TCP 顺序），服务端那条 `peer_goaway_received`
     * 断言就是拿这一点做判据的。 */
    cs->shutdown_rc = uvcpp_c_h2_conn_shutdown(c);
    CHECK_INT(cs->shutdown_rc, UVCPP_C_OK);
  }
}

static void cli_on_body(void* ud, uvcpp_c_h2_connection* c,
                        uvcpp_c_h2_stream* st, const char* data, size_t len) {
  client_side* cs = (client_side*)ud;
  int32_t sid = uvcpp_c_h2_stream_id(st);
  int     slot = cli_slot_of(sid);

  (void)c;
  ++cs->body_events;
  if (slot < 0) return;
  accum_push(&cs->slot[slot].body, data, len);
}

static void cli_on_close(void* ud, uvcpp_c_h2_connection* c, int32_t stream_id,
                         uint32_t error_code) {
  client_side* cs = (client_side*)ud;
  (void)c;
  (void)stream_id;
  (void)error_code;
  ++cs->closes;
}

static void cli_on_fatal(void* ud, uvcpp_c_h2_connection* c,
                         int nghttp2_error) {
  client_side* cs = (client_side*)ud;
  (void)c;
  (void)nghttp2_error;
  ++cs->fatals;
}

static void cli_on_disconnect(void* ud, uvcpp_c_h2_connection* c) {
  client_side* cs = (client_side*)ud;

  ++cs->disconnects;
  cs->bytes_in  = uvcpp_c_h2_conn_bytes_in(c);
  cs->bytes_out = uvcpp_c_h2_conn_bytes_out(c);
  cs->peer_goaway = uvcpp_c_h2_conn_peer_goaway_received(c);

  cs->free_in_cb = uvcpp_c_h2_conn_free(c);
  CHECK_INT(cs->free_in_cb, UVCPP_C_OK);
  cs->after_free_rc = uvcpp_c_h2_conn_free(c);
  CHECK_INT(cs->after_free_rc, UVCPP_C_E_STALE);

  uvcpp_c_tcp_client_stop(cs->tcp);
}

static void client_thread(void* arg) {
  client_side* cs = (client_side*)arg;
  uvcpp_c_h2_callbacks     h2c;
  uvcpp_c_h2_conn_callbacks cc;
  uvcpp_c_h2_request*      req;
  int                      i;

  cs->tcp = uvcpp_c_tcp_client_new();
  if (cs->tcp == NULL) return;

  cs->connect_rc = uvcpp_c_tcp_client_connect_wait(cs->tcp, "127.0.0.1",
                                                   cs->port, 5000);
  if (cs->connect_rc != 0) {
    uvcpp_c_tcp_client_free(cs->tcp);
    return;
  }

  cs->h2 = uvcpp_c_h2_connection_new(cs->tcp, 0);
  if (cs->h2 == NULL) {
    uvcpp_c_tcp_client_free(cs->tcp);
    return;
  }
  g_client_conn = cs->h2;

  memset(&h2c, 0, sizeof(h2c));
  h2c.size           = (uint32_t)sizeof(h2c);
  h2c.on_response    = cli_on_response;
  h2c.on_response_end = cli_on_response_end;
  h2c.on_body        = cli_on_body;
  h2c.on_close       = cli_on_close;
  h2c.on_fatal       = cli_on_fatal;

  memset(&cc, 0, sizeof(cc));
  cc.size          = (uint32_t)sizeof(cc);
  cc.on_disconnect = cli_on_disconnect;

  /* ★ 表的大小连自己那一格都没盖住 → 整张表都不读。这一次调用**不许**有任何
   * 副作用（不装回调、不开读、不发 SETTINGS），所以下面那次真 `start` 才是
   * 这条连接第一次开工。 */
  {
    uvcpp_c_h2_callbacks bad;
    memset(&bad, 0, sizeof(bad));
    bad.size           = 0;
    bad.on_response    = cli_on_response;
    CHECK_INT(uvcpp_c_h2_conn_start(cs->h2, &bad, NULL, cs),
              UVCPP_C_E_INVALID_ARG);
  }

  cs->start_rc = uvcpp_c_h2_conn_start(cs->h2, &h2c, &cc, cs);
  if (cs->start_rc != UVCPP_C_OK) {
    uvcpp_c_h2_conn_free(cs->h2);
    uvcpp_c_tcp_client_free(cs->tcp);
    return;
  }

  /* --- 三条请求 ------------------------------------------------------- */

  /* 流 1：POST /echo，带 body 与一个自定义头。
   * ★ `host` 是承重的：h2 的 `:authority` 由它填（本层没有 URL 兜底，
   *   见头里那条★）。 */
  req = uvcpp_c_h2_request_new("POST", "/echo");
  CHECK(req != NULL);
  if (req == NULL) return;
  CHECK_INT(uvcpp_c_h2_request_set_header(req, "host", "127.0.0.1"),
            UVCPP_C_OK);
  CHECK_INT(uvcpp_c_h2_request_set_header(req, "x-probe", "p1"), UVCPP_C_OK);
  /* ★ `content-length` **要自己写**：C++ 侧的 `submit_request()` 只把这个数组
   * 原样搬上线路（`uvcpp_h2_session.cpp:1237-1249`），**不替 body 补**这一格 ——
   * 它连 `body` 是不是空的都不知道。所以"服务端在请求方向看到 content-length"
   * 这件事，判据是这一句真的发出去了，而不是本层顺手加的。
   * 这个数由 `strlen(kCliBody)` 算出来而不是写死：写死的那天 body 改了长度，
   * 服务端会拿到一个与 body 不符的宣告，而那时红的是"expected_body 不对"，
   * 不是"谁改的 body"。 */
  {
    char cl[16];
    snprintf(cl, sizeof(cl), "%d", (int)strlen(kCliBody));
    CHECK_INT(uvcpp_c_h2_request_set_header(req, "content-length", cl),
              UVCPP_C_OK);
  }

  /* ★ 类型冒充：拿请求句柄当响应句柄用。两枚魔数不同，所以必须是 `E_STALE`
   * —— 共用一枚的话这一句会一路走到按响应布局读一块请求对象的地方。 */
  cs->type_confusion_rc =
      uvcpp_c_h2_conn_send_response(cs->h2, 1, (const uvcpp_c_h2_response*)req, 0);
  CHECK_INT(cs->type_confusion_rc, UVCPP_C_E_STALE);

  cs->sids[0] = uvcpp_c_h2_conn_submit_request(cs->h2, req, kCliBody,
                                               strlen(kCliBody));
  CHECK(cs->sids[0] > 0);
  CHECK_INT(cs->sids[0] % 2, 1); /* 客户端发起的流号是奇数 */
  /* 交出去之后立刻废掉：请求体是 `_submit_request()` 的一个参数（立刻拷贝），
   * 所以服务端收到的 body 不受这一句影响。 */
  cs->req_free_rc = uvcpp_c_h2_request_free(req);
  CHECK_INT(cs->req_free_rc, UVCPP_C_OK);
  CHECK_INT(uvcpp_c_h2_request_free(req), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h2_request_set_header(req, "a", "b"), UVCPP_C_E_STALE);

  /* 流 3：GET /status（无 body） */
  req = uvcpp_c_h2_request_new("GET", "/status");
  CHECK(req != NULL);
  if (req != NULL) {
    CHECK_INT(uvcpp_c_h2_request_set_header(req, "host", "127.0.0.1"),
              UVCPP_C_OK);
    cs->sids[1] = uvcpp_c_h2_conn_submit_request(cs->h2, req, NULL, 0);
    CHECK(cs->sids[1] > cs->sids[0]);
    CHECK_INT(uvcpp_c_h2_request_free(req), UVCPP_C_OK);
  }

  /* 流 5：GET /stream（流式响应那条路） */
  req = uvcpp_c_h2_request_new("GET", "/stream");
  CHECK(req != NULL);
  if (req != NULL) {
    CHECK_INT(uvcpp_c_h2_request_set_header(req, "host", "127.0.0.1"),
              UVCPP_C_OK);
    cs->sids[2] = uvcpp_c_h2_conn_submit_request(cs->h2, req, NULL, 0);
    CHECK(cs->sids[2] > cs->sids[1]);
    CHECK_INT(uvcpp_c_h2_request_free(req), UVCPP_C_OK);
  }

  /* 空句柄 / 空指针的边界 */
  CHECK_INT(uvcpp_c_h2_conn_submit_request(NULL, NULL, NULL, 0),
            UVCPP_C_E_STALE);
  CHECK(uvcpp_c_h2_request_new(NULL, "/x") == NULL);
  CHECK(uvcpp_c_h2_request_new("GET", NULL) == NULL);

  (void)uvcpp_c_tcp_client_run(cs->tcp);

  /* 循环回来了：三条流各自收尾。`_free` 已经在 `on_disconnect` 里跑过。 */
  for (i = 0; i < kStreams; ++i) {
    CHECK_INT(cs->slot[i].resp_end, 1);
  }

  /* ★ 回调期句柄：出了回调就是 E_STALE */
  if (cs->escaped != NULL) {
    CHECK_INT(uvcpp_c_h2_stream_id(cs->escaped), UVCPP_C_E_STALE);
    CHECK_INT(uvcpp_c_h2_stream_state(cs->escaped), UVCPP_C_E_STALE);
    CHECK_INT(uvcpp_c_h2_stream_response_status(cs->escaped),
              UVCPP_C_E_STALE);
  }

  CHECK_INT(uvcpp_c_h2_conn_free(cs->h2), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_free(cs->tcp), UVCPP_C_OK);
}

/* -------------------------------------------------------------------------
 * 构造器：不连接、不开循环也能测的那几条
 * ------------------------------------------------------------------------- */

static void test_constructors(void) {
  uvcpp_c_h2_request*  req;
  uvcpp_c_h2_response* resp;
  char                 buf[8];

  printf("[capi h2] 构造器的边界\n");

  req = uvcpp_c_h2_request_new("PUT", "/x");
  CHECK(req != NULL);
  if (req != NULL) {
    CHECK_INT(uvcpp_c_h2_request_set_header(req, NULL, "v"),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h2_request_set_header(req, "k", NULL),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h2_request_set_header(req, "k", "v"), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_request_free(req), UVCPP_C_OK);
  }
  /* 空指针：不是 E_STALE（那是"用过的"），是 E_INVALID_ARG（"你给了我个空"）。 */
  CHECK_INT(uvcpp_c_h2_request_free(NULL), UVCPP_C_E_INVALID_ARG);

  resp = uvcpp_c_h2_response_new(404);
  CHECK(resp != NULL);
  if (resp != NULL) {
    /* 状态码可以事后改（`_new` 那个值不是钉子） */
    CHECK_INT(uvcpp_c_h2_response_set_status(resp, 204), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_set_content_type(resp, NULL),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h2_response_set_header(resp, "x", NULL),
              UVCPP_C_E_INVALID_ARG);
    /* `len > 0` 而指针为空：拒。`len == 0` 而指针为空：合法（清空 body）。 */
    CHECK_INT(uvcpp_c_h2_response_set_body(resp, NULL, 4),
              UVCPP_C_E_INVALID_ARG);
    CHECK_INT(uvcpp_c_h2_response_set_body(resp, NULL, 0), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_set_body(resp, "abcd", 4), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_free(resp), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_h2_response_set_status(resp, 200), UVCPP_C_E_STALE);
  }

  /* 一个从来没被交出去的指针：登记表里没有 → 一律 E_STALE，一个字节都不读。 */
  CHECK_INT(uvcpp_c_h2_stream_id((const uvcpp_c_h2_stream*)(void*)buf),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h2_conn_free((uvcpp_c_h2_connection*)(void*)buf),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_h2_conn_free(NULL), UVCPP_C_E_INVALID_ARG);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  uvcpp_c_tcp_server* srv;
  uvcpp_c_tcp_server_events ev;
  uv_thread_t th;
  int port;
  int i;

  memset(&g_ss, 0, sizeof(g_ss));
  memset(&g_cs, 0, sizeof(g_cs));

  printf("[capi h2] start\n");

  test_constructors();

  srv = uvcpp_c_tcp_server_new();
  CHECK(srv != NULL);
  if (srv == NULL) return 1;
  g_ss.srv = srv;

  memset(&ev, 0, sizeof(ev));
  ev.size             = (uint32_t)sizeof(ev);
  ev.on_connection    = srv_on_connection;
  ev.on_connection_user_data = &g_ss;
  /* **不装 `on_read`**：这条连接上的字节归 h2 那一层读。装了的话两边都会去
   * `read_start_events()`，第二次拿到的是 `UV_EALREADY` —— 那是本层与 C++
   * 侧同一条纪律（"一条连接上只能有一个读者"），用例不去撞它。 */
  CHECK_INT(uvcpp_c_tcp_server_set_events(srv, &ev), UVCPP_C_OK);

  CHECK_INT(uvcpp_c_tcp_server_bind(srv, "127.0.0.1", 0), UVCPP_C_OK);
  port = uvcpp_c_tcp_server_local_port(srv);
  CHECK(port > 0 && port <= 65535);
  if (port <= 0) {
    uvcpp_c_tcp_server_free(srv);
    return 1;
  }
  CHECK_INT(uvcpp_c_tcp_server_listen(srv, 0), UVCPP_C_OK);

  g_cs.port = port;
  CHECK_INT(uv_thread_create(&th, client_thread, &g_cs), 0);

  /* 服务端的循环：客户端走完之后它在 `on_disconnect` 里自己停 */
  CHECK_INT(uvcpp_c_tcp_server_run(srv), 0);
  uv_thread_join(&th);

  /* ---- 服务端侧的判据 ---- */
  CHECK_INT(g_ss.connections, 1);
  CHECK_INT(g_ss.requests, kStreams);
  CHECK_INT(g_ss.request_ends, kStreams);
  CHECK_INT(g_ss.request_closes, kStreams);
  CHECK_INT(g_ss.fatals, 0);
  CHECK_INT(g_ss.disconnects, 1);
  CHECK_INT(g_ss.free_in_cb, UVCPP_C_OK);
  CHECK_INT(g_ss.after_free_rc, UVCPP_C_E_STALE);

  /* 方法名与路径逐条对上（三条流各不同） */
  CHECK_INT(g_ss.method_name_rc[0], 4);
  CHECK_STR(g_ss.method_name[0], "POST");
  CHECK_STR(g_ss.path[0], "/echo");
  CHECK_STR(g_ss.method_name[1], "GET");
  CHECK_STR(g_ss.path[1], "/status");
  CHECK_STR(g_ss.method_name[2], "GET");
  CHECK_STR(g_ss.path[2], "/stream");

  /* `host` 往返（三条流都带了） */
  for (i = 0; i < kStreams; ++i) {
    CHECK_INT(g_ss.host_ok[i], 1);
    CHECK_STR(g_ss.host[i], "127.0.0.1");
    CHECK_INT(g_ss.missing_rc[i], UVCPP_C_E_NOT_FOUND);
    CHECK_INT(g_ss.missing_len[i], UVCPP_C_E_NOT_FOUND);
    CHECK_INT(g_ss.resp_status_before[i], UVCPP_C_E_NOT_FOUND);
    CHECK_INT(g_ss.req_stream_id_by_handle[i], (int)g_cs.sids[i]);
    /* 服务端这条流在 `on_request_end` 那一刻还没回过任何东西 ⇒ 还是 OPEN */
    CHECK_INT(g_ss.req_state[i], UVCPP_C_H2_OPEN);
  }

  /* 请求体只在流 1 上有，且逐字节相等 */
  CHECK(accum_is(&g_ss.req_body, kCliBody));
  CHECK_INT(g_ss.req_body.len, (int)strlen(kCliBody));
  CHECK_INT(g_ss.req_body_bytes[0], (int)strlen(kCliBody));
  CHECK_INT(g_ss.req_body_bytes[1], 0);
  CHECK_INT(g_ss.req_body_bytes[2], 0);

  /* `send_data` 的两个 done：**恰好两次** */
  CHECK_INT(g_ss.done_calls, 2);
  for (i = 0; i < 2; ++i) {
    CHECK(g_ss.done_status[i] == 0 || g_ss.done_status[i] == UV_ECANCELED);
  }

  CHECK(g_ss.bytes_in >= (size_t)strlen(kCliBody));
  CHECK(g_ss.bytes_out > 0);
  /* ★ 客户端的 GOAWAY：TCP 同一条字节流上它一定排在 FIN 之前，所以服务端
   * 处理 `PEER_CLOSED` 时它**已经**被会话解出来了。 */
  CHECK_INT(g_ss.peer_goaway, 1);
  CHECK_INT((int)g_ss.peer_goaway_code, 0); /* NO_ERROR —— 正常道别 */
  CHECK(g_ss.peer_last_stream >= 0);

  /* ---- 客户端侧的判据（都是它那条线程上记下来的） ---- */
  CHECK_INT(g_cs.connect_rc, 0);
  CHECK_INT(g_cs.start_rc, UVCPP_C_OK);
  CHECK_INT(g_cs.type_confusion_rc, UVCPP_C_E_STALE);
  CHECK_INT(g_cs.req_free_rc, UVCPP_C_OK);
  CHECK_INT(g_cs.resp_end_total, kStreams);
  CHECK_INT(g_cs.resp_total, kStreams);
  CHECK(g_cs.body_events >= kStreams); /* 三条流各至少一次 DATA */
  CHECK_INT(g_cs.closes, kStreams);
  CHECK_INT(g_cs.fatals, 0);
  CHECK_INT(g_cs.disconnects, 1);
  CHECK_INT(g_cs.free_in_cb, UVCPP_C_OK);
  CHECK_INT(g_cs.after_free_rc, UVCPP_C_E_STALE);
  CHECK_INT(g_cs.shutdown_rc, UVCPP_C_OK);
  CHECK(g_cs.bytes_in > 0);
  CHECK(g_cs.bytes_out > 0);

  /* 状态码 + body **逐字节**（这一条是整份用例的主判据） */
  CHECK_INT(g_cs.slot[0].status, 200);
  CHECK(accum_is(&g_cs.slot[0].body, kSrvBody));
  CHECK_INT(g_cs.slot[0].body.len, (int)strlen(kSrvBody));

  CHECK_INT(g_cs.slot[1].status, 201);
  CHECK(accum_is(&g_cs.slot[1].body, kStatusBody));
  CHECK_INT(g_cs.slot[1].body.len, (int)strlen(kStatusBody));

  CHECK_INT(g_cs.slot[2].status, 200);
  CHECK(accum_is(&g_cs.slot[2].body, kStreamBody));
  CHECK_INT(g_cs.slot[2].body.len, (int)strlen(kStreamBody));

  /* ---- `expected_body()` 的不对称，两条路各自断言 ----------------------
   *
   * 客户端侧（`exp_body_rc`）：三条流**都**是 `E_NOT_FOUND`，连那条服务端
   * 明明宣告了 `content-length: 20` 的响应也是 —— 因为这一层只维护请求方向
   * 的那一对（本文件头部 ★ 那一段）。这一条**故意写得这么死**：它是一份
   * "客户端这条路现在就是不通"的记录，将来谁在响应头那条路上也把这个数填上，
   * 这一条会红，而那正是该有人来看一眼的时候。
   *
   * 客户端该走的那条路（`cont_len_rc` / `cont_len`）：读 `content-length`
   * 这个头。流 1 / 3 有（分别是 20 与 4），流 5 是流式、没有 → `E_NOT_FOUND`。 */
  CHECK_INT(g_cs.slot[0].exp_body_rc, UVCPP_C_E_NOT_FOUND);
  CHECK_INT(g_cs.slot[1].exp_body_rc, UVCPP_C_E_NOT_FOUND);
  CHECK_INT(g_cs.slot[2].exp_body_rc, UVCPP_C_E_NOT_FOUND);
  CHECK_INT(g_cs.slot[0].cont_len_rc, (int)strlen("20"));
  CHECK_INT((int)g_cs.slot[0].cont_len, (int)strlen(kSrvBody));
  CHECK_INT(g_cs.slot[1].cont_len_rc, 1);
  CHECK_INT((int)g_cs.slot[1].cont_len, (int)strlen(kStatusBody));
  CHECK_INT(g_cs.slot[2].cont_len_rc, UVCPP_C_E_NOT_FOUND);
  CHECK_INT((int)g_cs.slot[2].cont_len, 0);

  /* 服务端侧（请求方向）：这才是这一句的**主场**。流 1 的 POST 带着
   * `content-length: 14`，流 3 / 5 的 GET 什么都没带。两组值不一样，所以
   * 这里量的是"这个数真的从线路上解出来了"，不是"这个函数返回了个常数"。 */
  CHECK_INT(g_ss.exp_body_rc[0], UVCPP_C_OK);
  CHECK_INT(g_ss.exp_body[0], (int)strlen(kCliBody));
  CHECK_INT(g_ss.exp_body_rc[1], UVCPP_C_E_NOT_FOUND);
  CHECK_INT(g_ss.exp_body[1], -1);
  CHECK_INT(g_ss.exp_body_rc[2], UVCPP_C_E_NOT_FOUND);
  CHECK_INT(g_ss.exp_body[2], -1);

  CHECK_INT(uvcpp_c_tcp_server_free(srv), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_tcp_server_free(srv), UVCPP_C_E_STALE);

  /* 跨线程那条：客户端那条线程已经收工，但句柄早就被它自己废掉了 —— 所以
   * 这里只能验"作废之后拿到 E_STALE"，不能验 E_WRONG_THREAD（句柄的先后
   * 顺序决定了后者：这一句跑的时候它已经不在了）。`E_WRONG_THREAD` 那条由
   * 服务端的 `cross_thread_*` 在**连接还活着**的时候量 —— 见下面那段。 */
  if (g_client_conn != NULL) {
    CHECK_INT(uvcpp_c_h2_conn_free(g_client_conn), UVCPP_C_E_STALE);
  }
  CHECK_INT(g_ss.cross_thread_flush_rc, UVCPP_C_E_WRONG_THREAD);
  CHECK_INT(g_ss.cross_thread_probed, 1);

  /* ★ 回调期句柄的收支：一个都不许剩。
   *
   *  `uvcpp_c_h2_stream` 那几枚是**每次回调当场登记、出栈时摘表**的栈对象
   *  （`src/capi/uvcpp_c_http2.cpp` 的 `FrameScope`，`slots_` 只有一格，
   *  意义见那份文件里"最多只有一个"那段）。跑完这一趟它们应该全部摘干净。
   *
   *  为什么非要有这一条：上面那三条"把 stream 带出回调再问它"的 `E_STALE`
   *  断言**量不出"摘表那一句执行了没有"** —— 那块栈在回调返回后立刻被后续代码
   *  复用，魔数早就不是登记时的值了，`alive()` 在"读魔数"那一句就判假。这条
   *  收支平衡是那件事**唯一**的可观测形式（`capi_mutation.py` 的 M8 与 M16
   *  就是这条的代价，记在 `doc/capi-guide.md` §6）。 */
  CHECK_INT((int)uvcpp_c_live_handle_count(), 0);

  printf("[capi h2] checks=%d failures=%d\n", g_checks, g_failed);
  if (g_failed != 0) {
    printf("[capi h2] FAILED: %d\n", g_failed);
    return 1;
  }
  printf("[capi h2] PASS\n");
  return 0;
}
