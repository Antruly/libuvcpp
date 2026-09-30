/**
 * @file tests/capi/capi_webapp_func.c
 * @brief webapp 层的 C ABI 端到端用例：**纯 C** 起一个真服务器、用 C 写的
 *        客户端打自己，逐字节核对。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这份用例是**纯 C**（`.c`，不 include 任何 C++ 头）。一条进程里两个真线程：
 *
 *   - **后台线程**：`uvcpp_c_app_start_background()` 起的 webapp（它自己那条
 *     循环），端口给 0 让内核挑，挑中的用 `uvcpp_c_app_bound_port()` 问回来；
 *   - **主线程**：先跑 `uvcpp_c_http_client` 那条循环，打上面那个端口；跑完
 *     再跑 `uvcpp_c_ws_client` 的那条。
 *
 * 它钉住的七件事（每一条都对应 C 面一处只有 C 面才有的形状）：
 *
 *   1. **真收字节、逐字节相等**：`/text` 的 body、`/json` 的 body、`/echo` 回
 *      来的 body（POST 上去什么就回来什么）都按长度 + `memcmp` 比，不是"看起
 *      来对"。
 *   2. **中间件真的在路由之前跑**：中间件在响应上写 `X-MW: yes`，`/mw` 的那个
 *      处理器**读这个头**并据此决定自己写什么 —— 于是客户端的断言
 *      （头在 + body 是那句只可能由"读到了头"得出的值）同时量了两件事。
 *   3. **延迟应答"晚到但如期"**：`/slow` 的处理器 `uvcpp_c_defer(next)` 之后
 *      立刻返回，真正写响应的是 `uvcpp_c_deferred_post()` 投回来的那次。
 *      判据是收到它的时候服务端那个"已经答完"的标志必须是 1（= 处理器返回
 *      之后才发生），且 body 内容对得上。
 *   4. **静态目录**：`/static/hello.txt` 从磁盘上一个真目录里取，内容逐字节比。
 *   5. **404 是 404**：一条没注册过的路径必须给出 404，而不是 200 或者挂住。
 *   6. **WebSocket 真往返**：C 写的 ws 客户端发一句，服务端回显，客户端核对。
 *   7. **回调期句柄在回调外失效**：把 `uvcpp_c_req` / `uvcpp_c_resp` 存进静态
 *      变量，回调返回之后再用，必须拿到 `UVCPP_C_E_STALE`（**不是崩溃**）。
 *
 * 另有两条"量了但没当判据"的自检写在最后：`uvcpp_c_live_handle_count()`
 * 必须回到起点（登记表收支平衡），以及中间件至少跑了每一条请求那么多趟。
 */

#include <capi/uvcpp_c.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !UVCPP_CAPI_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_CAPI=ON 的构建里被编译"
#endif

/* 建/删一个目录（Windows 与 POSIX 的两个名字，不引第三方）。 */
#ifdef _WIN32
#  include <direct.h>
#  define UVCPP_T_MKDIR(p) _mkdir(p)
#  define UVCPP_T_RMDIR(p) _rmdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define UVCPP_T_MKDIR(p) mkdir((p), 0755)
#  define UVCPP_T_RMDIR(p) rmdir(p)
#endif

/* -------------------------------------------------------------------------
 * 判定宏（与 `capi_common_func.c` / `capi_net_func.c` 同一形状）
 * ------------------------------------------------------------------------- */

static int g_checks = 0;
static int g_failed = 0;

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
    long _g = (long)(got);                                               \
    long _w = (long)(want);                                              \
    ++g_checks;                                                          \
    if (_g != _w) {                                                      \
      printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__,     \
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
      printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__,         \
             __LINE__, #got, _g != NULL ? _g : "(null)", _w);            \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

/**
 * @brief 「调用方给缓冲区」那条约定：函数名 + 它除了 `(buf, cap)` 之外的参数。
 *
 * 形如 `CHECK_BUF_EQ("GET", uvcpp_c_req_method_name, req)` —— 宏自己造缓冲区并
 * 把 `_b, sizeof(_b)` 接到参数表尾。**缓冲区必须由宏持有**：写成"调用方把自己
 * 的 buf 塞进表达式"那种形状，宏就没法检查它到底写了没有（它连那个 buf 的
 * 名字都不知道），于是"缓冲区够不够"这条约定根本量不到。
 *
 * 返回值那一半单独量：负数报错误码、非负时与 `strlen(want)` 比对长度，最后比
 * 内容 —— 三条分开报，是为了让失败信息直接说出是"没查到"、"长度不对"还是
 * "内容不对"。
 */
#define CHECK_BUF_EQ(want, fn, ...)                                      \
  do {                                                                   \
    char _b[256];                                                        \
    _b[0] = '\0';                                                        \
    const size_t _w = strlen(want);                                      \
    int _n = fn(__VA_ARGS__, _b, sizeof(_b));                            \
    ++g_checks;                                                          \
    if (_n < 0) {                                                        \
      printf("FAIL %s:%d: %s 返回错误码 %d (%s)\n", __FILE__, __LINE__,  \
             #fn, _n, uvcpp_c_strerror(_n));                             \
      ++g_failed;                                                        \
    } else if ((size_t)_n != _w) {                                       \
      printf("FAIL %s:%d: %s 长度 = %d, want %d\n", __FILE__, __LINE__,  \
             #fn, _n, (int)_w);                                          \
      ++g_failed;                                                        \
    } else if (strcmp(_b, want) != 0) {                                  \
      printf("FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__,         \
             __LINE__, #fn, _b, want);                                   \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

/* -------------------------------------------------------------------------
 * 服务端状态
 * ------------------------------------------------------------------------- */

#define kTextBody "hello-from-capi"
#define kJsonBody "{\"n\":42}"
#define kSlowBody "late-but-on-time"
#define kStaticBody "static-file-content\n"
#define kEchoBody "the-body-came-back"
#define kMwBody "mw-saw-the-header"
#define kStaticDir "capi_static_tmp"
#define kStaticName "hello.txt"
#define kStaticPath "/static/" kStaticName
#define kWsPath "/ws"
#define kWsClosePath "/ws-close"
#define kWsText "ping-over-websocket"

static struct {
  int mw_hits;       /**< 中间件跑了几趟 */
  int slow_done;     /**< `/slow` 那条"延迟回调真的跑过了"的标志 */
  int slow_posted;   /**< 处理器里 `deferred_post` 成功投出去了几次 */
  int ws_upgrades;   /**< WS 升级回调跑了几次 */
  int ws_echoes;     /**< WS 回显发了几次 */
  int ws_closes;     /**< WS 关闭回调响了几次 */
  void* escaped_req;  /**< 回调期句柄越界使用的取样（规矩 7） */
  void* escaped_resp;
} g_srv;

/** 全体共享的 app 句柄。回调里要用它（`uvcpp_c_req_*` 不带 app）。 */
static uvcpp_c_app* g_app = NULL;

/* -------------------------------------------------------------------------
 * 服务端路由
 * ------------------------------------------------------------------------- */

/**
 * @brief `/text`：最普通的一条。顺手钉三件事。
 *
 * 1. 请求行的读数：`method_name` / `path` 与客户端发的一致（用"先问长度再取"
 *    那条约定取，`cap = 0` 时不该写缓冲区）。
 * 2. 响应头往返。
 * 3. **回调期句柄越界使用**：把 req / resp 两枚存进全局，回调返回后再用，
 *    必须拿到 `E_STALE`。
 */
static void on_text(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
                    uvcpp_c_next* next) {
  (void)ud;
  (void)next;
  char buf[64];

  CHECK_BUF_EQ("GET", uvcpp_c_req_method_name, req);
  CHECK_BUF_EQ("/text", uvcpp_c_req_path, req);

  /* `cap = 0` 只问长度、一个字节都不写。 */
  {
    char untouched = 'Z';
    const int n = uvcpp_c_req_path(req, &untouched, 0);
    CHECK_INT(n, (int)strlen("/text"));
    CHECK(untouched == 'Z');
  }

  /* 缓冲区刚好差一个字节 → 仍然只回答长度，不写（不许截断）。 */
  {
    char small[4] = {0, 0, 0, 0};
    const int n = uvcpp_c_req_path(req, small, sizeof(small));
    CHECK_INT(n, (int)strlen("/text"));
    CHECK(small[0] == '\0');
  }

  CHECK_INT(uvcpp_c_req_header(req, "X-Does-Not-Exist", buf, sizeof(buf)),
            UVCPP_C_E_NOT_FOUND);
  CHECK_INT(uvcpp_c_req_is_keep_alive(req), 1);

  /* 本层"查不到"与"查到空串"是两件事：这里查一个不存在的 query 参数。 */
  CHECK_INT(uvcpp_c_req_query(req, "nope", buf, sizeof(buf)),
            UVCPP_C_E_NOT_FOUND);

  uvcpp_c_resp_set_header(resp, "X-Capi", "1");
  uvcpp_c_resp_text(resp, kTextBody);
  CHECK_INT(uvcpp_c_resp_status_code(resp), 200);
  CHECK_INT(uvcpp_c_resp_end(resp), UVCPP_C_OK);
  /* 恰好一次：第二次 `end` 必须是 `E_STATE`，不是静默重复。 */
  CHECK_INT(uvcpp_c_resp_end(resp), UVCPP_C_E_STATE);
  CHECK_INT(uvcpp_c_resp_ended(resp), 1);

  g_srv.escaped_req  = req;
  g_srv.escaped_resp = resp;
}

/** @brief `/json`：JSON 只在 C 面出现**一个字符串**（规矩：`nlohmann` 不进 C 头）。 */
static void on_json(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
                    uvcpp_c_next* next) {
  (void)ud;
  (void)req;
  (void)next;
  char buf[64];
  uvcpp_c_resp_json_str(resp, kJsonBody);
  CHECK_BUF_EQ("application/json; charset=utf-8", uvcpp_c_resp_content_type,
               resp);
  uvcpp_c_resp_end(resp);
}

/**
 * @brief `/echo`：POST 上来的 body 原样回去。
 *
 * 这是"入参立刻被拷走"那条所有权的判据：`uvcpp_c_req_body()` 给的指针只在
 * 本次回调里有效，而 `uvcpp_c_resp_body()` 把它拷进去 —— 于是回显是对的。
 */
static void on_echo(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
                    uvcpp_c_next* next) {
  (void)ud;
  (void)next;
  const char* data = NULL;
  size_t len = 0;

  CHECK_BUF_EQ("POST", uvcpp_c_req_method_name, req);
  CHECK_INT(uvcpp_c_req_body(req, &data, &len), UVCPP_C_OK);
  CHECK(data != NULL);
  CHECK_INT(len, (int)strlen(kEchoBody));
  CHECK(len == strlen(kEchoBody) &&
        memcmp(data, kEchoBody, strlen(kEchoBody)) == 0);
  CHECK_INT(uvcpp_c_req_body_empty(req), 0);
  CHECK_INT((int)uvcpp_c_req_content_length(req), (int)strlen(kEchoBody));

  uvcpp_c_resp_body(resp, data, len, "application/octet-stream");
  uvcpp_c_resp_end(resp);
}

/** @brief `/slow` 的后半段：`deferred_post` 投回来的那次，在这里真正写响应。 */
static void on_slow_ready(void* ud, uvcpp_c_deferred* d) {
  (void)ud;
  uvcpp_c_req* req = uvcpp_c_deferred_req(d);
  uvcpp_c_resp* resp = uvcpp_c_deferred_resp(d);
  char buf[64];

  /* 延迟期间那两枚句柄必须还是活的（这就是 `deferred` 存在的意义）。 */
  CHECK(req != NULL);
  CHECK(resp != NULL);
  if (req != NULL) {
    CHECK_BUF_EQ("/slow", uvcpp_c_req_path, req);
  }
  if (resp != NULL) {
    /* 处理器返回时什么都没写：这一趟之前状态码还是默认的 200、body 是空的。 */
    CHECK_INT((int)uvcpp_c_resp_body_size(resp), 0);
    uvcpp_c_resp_set_header(resp, "X-Slow", "1");
    uvcpp_c_resp_text(resp, kSlowBody);
  }
  g_srv.slow_done = 1;
  CHECK_INT(uvcpp_c_deferred_resume(d), UVCPP_C_OK);
  /* 恰好一次：第二次 resume 是 `E_STATE`。 */
  CHECK_INT(uvcpp_c_deferred_resume(d), UVCPP_C_E_STATE);
  CHECK_INT(uvcpp_c_deferred_free(d), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_deferred_free(d), UVCPP_C_E_STALE);
}

/**
 * @brief `/slow` 的前半段：把链挂起，立刻返回。
 *
 * 判据在于：**这条处理器返回时响应还没写**（`uvcpp_c_resp_ended()` 为 0），
 * 而客户端最终收到的那条是 `on_slow_ready()` 写的。
 */
static void on_slow(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
                    uvcpp_c_next* next) {
  (void)ud;
  (void)req;
  CHECK_INT(uvcpp_c_resp_ended(resp), 0);
  CHECK_INT(g_srv.slow_done, 0);  /* 还没到"晚到"那一刻 */

  uvcpp_c_deferred* d = uvcpp_c_defer(next);
  CHECK(d != NULL);
  if (d == NULL) {
    uvcpp_c_resp_server_error(resp, "defer failed");
    uvcpp_c_resp_end(resp);
    return;
  }
  CHECK_INT(uvcpp_c_deferred_post(d, on_slow_ready, NULL), UVCPP_C_OK);
  ++g_srv.slow_posted;
  /* 处理器**不写响应、也不 `resume`** —— 两件事都在投回来那次里做。 */
}

/**
 * @brief 中间件：给每条响应盖一个头，然后放行。
 *
 * 它同时是"`next` 只能用一次"的判据（第二次 `run` 是 `E_STATE`）。
 */
static void on_middleware(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
                          uvcpp_c_next* next) {
  (void)ud;
  (void)req;
  ++g_srv.mw_hits;
  uvcpp_c_resp_set_header(resp, "X-MW", "yes");
  CHECK_INT(uvcpp_c_next_run(next), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_next_run(next), UVCPP_C_E_STATE);
}

/**
 * @brief `/mw`：**读中间件留下的那个头**再决定写什么。
 *
 * 这是"中间件真的在路由之前跑"唯一的硬证据 —— 它与客户端的断言连起来看：
 * 客户端看到 `X-MW: yes` 且 body 是 `mw-saw-the-header`，而后者只可能由
 * "读到了 `X-MW` 且值是 `yes`" 得出。
 */
static void on_mw_route(void* ud, uvcpp_c_req* req, uvcpp_c_resp* resp,
                        uvcpp_c_next* next) {
  (void)ud;
  (void)req;
  (void)next;
  char buf[16];
  if (uvcpp_c_resp_get_header(resp, "X-MW", buf, sizeof(buf)) > 0 &&
      strcmp(buf, "yes") == 0) {
    uvcpp_c_resp_text(resp, kMwBody);
  } else {
    /* 中间件没跑（或跑在路由之后）—— 写一句明显不同的，让客户端的断言红。 */
    uvcpp_c_resp_text(resp, "middleware-missing");
  }
  uvcpp_c_resp_end(resp);
}

/* -------------------------------------------------------------------------
 * 服务端 WebSocket
 * ------------------------------------------------------------------------- */

static void on_ws_text(void* ud, uvcpp_c_ws_conn* conn, const char* data,
                       size_t len) {
  (void)ud;
  ++g_srv.ws_echoes;
  /* 原样回显：文本帧的载荷**二进制安全**，按 `len` 走。 */
  CHECK_INT(uvcpp_c_ws_conn_is_open(conn), 1);
  CHECK_INT(uvcpp_c_ws_conn_send_text(conn, data, len) >= 0, 1);
}

static void on_ws_close(void* ud, uvcpp_c_ws_conn* conn, int code,
                        const char* reason, size_t reason_len) {
  (void)ud;
  (void)conn;
  (void)reason;
  (void)reason_len;
  ++g_srv.ws_closes;
  /* 客户端是照 1000 正常收的。 */
  CHECK_INT(code, 1000);
}

/** @brief 升级回调：把回显装上去，然后返回（= 接受升级）。 */
static void on_ws_upgrade(void* ud, uvcpp_c_ws_req* wreq) {
  (void)ud;
  ++g_srv.ws_upgrades;

  CHECK_BUF_EQ(kWsPath, uvcpp_c_ws_req_path, wreq);
  CHECK_BUF_EQ(kWsPath, uvcpp_c_ws_req_route, wreq);

  uvcpp_c_ws_conn* conn = uvcpp_c_ws_req_conn(wreq);
  CHECK(conn != NULL);
  if (conn == NULL) return;

  uvcpp_c_ws_events ev = {0};
  ev.size = (uint32_t)sizeof(ev);
  ev.on_text = on_ws_text;
  ev.on_close = on_ws_close;
  CHECK_INT(uvcpp_c_ws_conn_set_events(conn, &ev, NULL), UVCPP_C_OK);
}

/**
 * @brief `/ws-close`：升级之后就等第一帧文本，收到就把**这条连接关掉**。
 *
 * 这条路由存在的理由只有一个：**它是客户端能确定性地看到 `on_close` 的唯一
 * 形状**。由客户端发起的关闭，框架按设计"调用方知情、不再补 on_close"
 * （`uvcpp_ws_connection::close()` 那句 `close_notified_ = true`），而"服务端
 * 的回 Close 帧"和"本端已经开始的拆连接"之间是一场竞态 —— 客户端那边报不报
 * 是随机的。服务端关就没有这个歧义：对端发起的关闭一定走到
 * `on_ws_frame` 的 CLOSE 分支，那里**一定**把码交给调用方。
 */
static void on_ws_close_route_text(void* ud, uvcpp_c_ws_conn* conn,
                                   const char* data, size_t len) {
  (void)ud;
  (void)data;
  (void)len;
  ++g_srv.ws_echoes;
  uvcpp_c_ws_conn_close(conn, UVCPP_C_WS_NORMAL, "server-bye", 10);
}

static void on_ws_close_route_upgrade(void* ud, uvcpp_c_ws_req* wreq) {
  (void)ud;
  ++g_srv.ws_upgrades;
  uvcpp_c_ws_conn* conn = uvcpp_c_ws_req_conn(wreq);
  if (conn == NULL) return;
  uvcpp_c_ws_events ev = {0};
  ev.size = (uint32_t)sizeof(ev);
  ev.on_text = on_ws_close_route_text;
  CHECK_INT(uvcpp_c_ws_conn_set_events(conn, &ev, NULL), UVCPP_C_OK);
}

/* -------------------------------------------------------------------------
 * 客户端：HTTP
 * -------------------------------------------------------------------------
 * **驱动方式是本文件里最承重的一个决定，先说清楚。**
 *
 * 直觉写法是"在响应回调里发下一条请求"（异步客户端最自然的形状），但它在
 * `uvcpp_http_client` 上不成立：`send()` 会 `parser_->reset()` 并重新注册读
 * 回调，而它是从 `on_tcp_data → parser_->execute() → on_message_complete`
 * 这条栈里被调进来的 —— 也就是在 llhttp 自己的 `execute` 里重新初始化它，
 * 于是**第二条响应永远解析不出来**，调用方要等到对端 FIN 才拿到
 * `UV_ECONNRESET`。（本用例第一版就是这么写的，症状是"服务端日志里两条 200
 * 都在，客户端第二条却等到 60 秒空闲超时"。）
 *
 * 所以这里改成**主线程驱动的一步一条**：
 *   1. 主线程发第 N 条；
 *   2. `uvcpp_c_http_client_run()` 跑这条循环，响应回调把结果记下来再
 *      `uvcpp_c_http_client_stop()`，于是 `run()` 当场返回；
 *   3. 主线程断言第 N 条，然后发第 N+1 条。
 *
 * 这仍然是**同一条 keep-alive 连接**上的七条请求（连接复用照旧被测到），只是
 * "什么时候发下一条"这件事由主线程在回调之外决定 —— 与仓里 C++ 那侧的用法
 * 一致（`tests/functional/web_http_client_keepalive_func.cpp` 也是这么发的）。
 *
 * 顺带说明 `run()` 的返回值：它是 `uv_run()` 的原值，**keep-alive 连接还开着
 * 的时候停止循环返回的是非 0**（"还有活句柄"）。所以断言写 `>= 0`，不写 `== 0`
 * —— 写 `== 0` 就把"连接被复用"这件事测反了。
 */

/**
 * @brief 一条请求的读数（由响应回调填，主线程断言）。
 *
 * 响应句柄是**回调期句柄**，所以 body 必须在回调里就拷出来 —— 这张结构体就是
 * 那一次拷贝的落点。
 */
typedef struct {
  int  status;
  char status_msg[32];
  char x_capi[32];
  char x_mw[32];
  char x_slow[32];
  char content_type[64];
  char body[256];
  size_t body_len;
  int  seen;    /**< 回调真的跑过了（`run()` 返回时必须为 1） */
  void* handle; /**< 故意带出回调的那枚句柄，见 `main()` 里的失效断言 */
} http_result;

static struct {
  uvcpp_c_http_client* cli;
  int   step;
  int   failed;
  http_result r;
} g_http;

#define HTTP_EXPECT(cond)                                                \
  do {                                                                   \
    ++g_checks;                                                          \
    if (!(cond)) {                                                       \
      printf("FAIL %s:%d: 第 %d 条: %s\n", __FILE__, __LINE__,           \
             g_http.step, #cond);                                        \
      ++g_failed;                                                        \
    }                                                                    \
  } while (0)

/** @brief 把一条响应拷进 `g_http.r`（回调里唯一允许做的事）。 */
static void http_capture(uvcpp_c_http_response* resp) {
  char buf[64];

  memset(&g_http.r, 0, sizeof(g_http.r));
  g_http.r.seen   = 1;
  g_http.r.handle = resp;

  g_http.r.status = uvcpp_c_http_response_status_code(resp);
  {
    char msg[64];
    const int n = uvcpp_c_http_response_status_message(resp, msg, sizeof(msg));
    ++g_checks;
    if (n < 0) {
      printf("FAIL %s:%d: status_message 返回 %d (%s)\n", __FILE__, __LINE__,
             n, uvcpp_c_strerror(n));
      ++g_failed;
    } else {
      strncpy(g_http.r.status_msg, msg, sizeof(g_http.r.status_msg) - 1);
    }
  }
  if (uvcpp_c_http_response_header(resp, "X-Capi", buf, sizeof(buf)) > 0)
    strncpy(g_http.r.x_capi, buf, sizeof(g_http.r.x_capi) - 1);
  if (uvcpp_c_http_response_header(resp, "X-MW", buf, sizeof(buf)) > 0)
    strncpy(g_http.r.x_mw, buf, sizeof(g_http.r.x_mw) - 1);
  if (uvcpp_c_http_response_header(resp, "X-Slow", buf, sizeof(buf)) > 0)
    strncpy(g_http.r.x_slow, buf, sizeof(g_http.r.x_slow) - 1);
  uvcpp_c_http_response_content_type(resp, g_http.r.content_type,
                                     sizeof(g_http.r.content_type));

  /* 查不到的头与"查到空串"是两件事：这里两条都要量。 */
  CHECK_INT(uvcpp_c_http_response_header(resp, "X-Nope", buf, sizeof(buf)),
            UVCPP_C_E_NOT_FOUND);
  CHECK_INT(uvcpp_c_http_response_has_header(resp, "X-Nope"), 0);

  {
    const char* data = NULL;
    size_t len = 0;
    if (uvcpp_c_http_response_body(resp, &data, &len) == UVCPP_C_OK && data) {
      if (len >= sizeof(g_http.r.body)) len = sizeof(g_http.r.body) - 1;
      memcpy(g_http.r.body, data, len);
      g_http.r.body[len] = '\0';
      g_http.r.body_len = len;
    }
  }
}

/**
 * @brief 响应回调：记下来、停循环，**到这里为止**。
 *
 * 不发下一条请求（理由见上面那一大段）；断言全在主线程做。
 */
static void on_http_response(void* ud, uvcpp_c_http_response* resp, int error) {
  (void)ud;
  if (error != 0) {
    printf("FAIL: HTTP 第 %d 步失败，error = %d (%s)\n", g_http.step, error,
           uvcpp_c_strerror(error));
    ++g_failed;
    g_http.failed = 1;
  } else {
    http_capture(resp);
  }
  uvcpp_c_http_client_stop(g_http.cli);
}

/** @brief 发第 `step` 条请求。 */
static int http_issue(int step) {
  g_http.step   = step;
  g_http.r.seen = 0;
  switch (step) {
    case 0: return uvcpp_c_http_client_get(g_http.cli, "/text",
                                           on_http_response, NULL);
    case 1: return uvcpp_c_http_client_get(g_http.cli, "/json",
                                           on_http_response, NULL);
    case 2: return uvcpp_c_http_client_get(g_http.cli, "/mw",
                                           on_http_response, NULL);
    case 3: return uvcpp_c_http_client_get(g_http.cli, kStaticPath,
                                           on_http_response, NULL);
    case 4: return uvcpp_c_http_client_get(g_http.cli, "/slow",
                                           on_http_response, NULL);
    case 5: return uvcpp_c_http_client_post(g_http.cli, "/echo", kEchoBody,
                                            strlen(kEchoBody), "text/plain",
                                            on_http_response, NULL);
    case 6: return uvcpp_c_http_client_get(g_http.cli, "/nope",
                                           on_http_response, NULL);
    default: return UVCPP_C_E_INVALID_ARG;
  }
}

/** @brief 连上之后立刻发第 0 条（连接回调在循环线程上，发请求是允许的）。 */
static void on_http_connected(void* ud, int status) {
  (void)ud;
  if (status != 0) {
    printf("FAIL: HTTP 连接失败，status = %d (%s)\n", status,
           uvcpp_c_strerror(status));
    ++g_failed;
    g_http.failed = 1;
    uvcpp_c_http_client_stop(g_http.cli);
    return;
  }
  CHECK_INT(uvcpp_c_http_client_is_connected(g_http.cli), 1);
  CHECK_INT(http_issue(0), UVCPP_C_OK);
}

/** @brief 第 `step` 条收回来的东西该长什么样。 */
static void http_check(int step) {
  switch (step) {
    case 0:
      HTTP_EXPECT(g_http.r.status == 200);
      HTTP_EXPECT(strcmp(g_http.r.status_msg, "OK") == 0);
      HTTP_EXPECT(strcmp(g_http.r.body, kTextBody) == 0);
      HTTP_EXPECT(g_http.r.body_len == strlen(kTextBody));
      HTTP_EXPECT(strcmp(g_http.r.x_capi, "1") == 0);
      HTTP_EXPECT(strstr(g_http.r.content_type, "text/plain") != NULL);
      break;
    case 1:
      HTTP_EXPECT(g_http.r.status == 200);
      HTTP_EXPECT(strcmp(g_http.r.body, kJsonBody) == 0);
      HTTP_EXPECT(strstr(g_http.r.content_type, "application/json") != NULL);
      break;
    case 2:
      HTTP_EXPECT(g_http.r.status == 200);
      HTTP_EXPECT(strcmp(g_http.r.x_mw, "yes") == 0);
      HTTP_EXPECT(strcmp(g_http.r.body, kMwBody) == 0);
      break;
    case 3:
      HTTP_EXPECT(g_http.r.status == 200);
      HTTP_EXPECT(strcmp(g_http.r.body, kStaticBody) == 0);
      break;
    case 4:
      /* "晚到但如期"：body 是处理器**没写**、由投回来那次写的。 */
      HTTP_EXPECT(g_http.r.status == 200);
      HTTP_EXPECT(strcmp(g_http.r.body, kSlowBody) == 0);
      HTTP_EXPECT(strcmp(g_http.r.x_slow, "1") == 0);
      break;
    case 5:
      HTTP_EXPECT(g_http.r.status == 200);
      HTTP_EXPECT(strcmp(g_http.r.body, kEchoBody) == 0);
      HTTP_EXPECT(g_http.r.body_len == strlen(kEchoBody));
      /* `on_echo` 显式给了 `application/octet-stream` —— C 面这一格的默认值
       * 就是它（与 C++ 侧一致），传什么就回什么，不是框架替我挑的。 */
      HTTP_EXPECT(strstr(g_http.r.content_type,
                         "application/octet-stream") != NULL);
      break;
    case 6:
      HTTP_EXPECT(g_http.r.status == 404);
      HTTP_EXPECT(strcmp(g_http.r.status_msg, "Not Found") == 0);
      break;
    default:
      HTTP_EXPECT(0);
      break;
  }
}

/* -------------------------------------------------------------------------
 * 客户端：WebSocket
 * -------------------------------------------------------------------------
 * 同样由主线程驱动：`connect()` 之后 `run()` 一次把这一场会话跑完（握手、发、
 * 收、关都在里面），事件回调负责 `stop()`。
 *
 * 两种结束方式都要跑，因为它们能观测到的东西**不一样**：
 *   - `/ws`（**客户端发起关闭**）：能观测到的是**服务端**收到了 Close 帧并报出
 *     码 1000。客户端自己那枚 `on_close` 是竞态 —— 框架对"本端发起"的设计就是
 *     "调用方知情，不再补 on_close"，而"服务端回帧"与本端拆连接谁先谁后不
 *     确定。所以这一场**不断言**客户端有没有 on_close。
 *   - `/ws-close`（**服务端发起关闭**）：对端发的 Close 帧一定走到
 *     `on_ws_frame` 的 CLOSE 分支，那里一定回调 —— 所以这一场**断言**客户端
 *     收到 1000 与原因串。
 *
 * **关闭帧必须单独再 `run()` 一次才发得出去**：`close()` 只是把 Close 帧排进
 * 写队列（写完成的回调里才 `tcp_->close()`），而 `stop()` 会让 `uv_run` 在
 * 当前这一轮末尾就返回 —— 排进去的写还没落地。所以"发完就停"那一版里服务端
 * 一个 Close 帧都收不到。这里按仓里 C++ 用例的写法收场：先在 `on_text` 里
 * `stop()`，回到主线程再 `close()` + `run()` 一次，让关闭握手走完。
 */

/** 一场 WS 会话的落点。 */
static struct {
  uvcpp_c_ws_client* cli;
  const char* send_text;
  int  send_len;
  int  expect_peer_close; /**< 1 = 这一场由服务端关，客户端必须收到 on_close */
  int  connected;
  int  got_text;
  int  closed;
  int  close_code;
  char echo[64];
  char reason[64];
} g_ws;

static void on_ws_client_text(void* ud, const char* data, size_t len) {
  (void)ud;
  g_ws.got_text = 1;
  if (len >= sizeof(g_ws.echo)) len = sizeof(g_ws.echo) - 1;
  memcpy(g_ws.echo, data, len);
  g_ws.echo[len] = '\0';
  /* 只停循环；关闭握手留给主线程（理由见上面那段）。 */
  uvcpp_c_ws_client_stop(g_ws.cli);
}

static void on_ws_client_close(void* ud, int code, const char* reason,
                               size_t reason_len) {
  (void)ud;
  g_ws.closed     = 1;
  g_ws.close_code = code;
  if (reason != NULL && reason_len > 0) {
    size_t n = reason_len < sizeof(g_ws.reason) - 1 ? reason_len
                                                    : sizeof(g_ws.reason) - 1;
    memcpy(g_ws.reason, reason, n);
    g_ws.reason[n] = '\0';
  }
  uvcpp_c_ws_client_stop(g_ws.cli);
}

static void on_ws_client_connected(void* ud, int status) {
  (void)ud;
  g_ws.connected = 1;
  if (status != 0) {
    printf("FAIL: ws 握手失败，status = %d (%s)\n", status,
           uvcpp_c_strerror(status));
    ++g_failed;
    uvcpp_c_ws_client_stop(g_ws.cli);
    return;
  }
  CHECK_INT(uvcpp_c_ws_client_is_open(g_ws.cli), 1);
  CHECK_INT(uvcpp_c_ws_client_session_count(g_ws.cli), 1);
  CHECK_INT(uvcpp_c_ws_client_send_text(g_ws.cli, g_ws.send_text,
                                        (size_t)g_ws.send_len),
            0);
}

/**
 * @brief 跑完一场 WS 会话（主线程调）。
 *
 * @param expect_echo        这一场服务端会回显（`/ws-close` 那条不回，它直接把
 *                           连接关掉）。
 * @param expect_peer_close  这一场由**服务端**发起关闭 → 客户端必须收到
 *                           `on_close`，且码与原因要对得上。
 */
static void ws_run_session(int port, const char* path, const char* text,
                           int expect_echo, int expect_peer_close) {
  char url[160];
  int rc;

  memset(&g_ws, 0, sizeof(g_ws));
  g_ws.send_text         = text;
  g_ws.send_len          = (int)strlen(text);
  g_ws.expect_peer_close = expect_peer_close;

  g_ws.cli = uvcpp_c_ws_client_new();
  CHECK(g_ws.cli != NULL);
  if (g_ws.cli == NULL) return;

  {
    uvcpp_c_ws_client_events ev = {0};
    ev.size     = (uint32_t)sizeof(ev);
    ev.on_text  = on_ws_client_text;
    ev.on_close = on_ws_client_close;
    /* 事件表在 `connect()` **之前**装 —— 异步客户端的常规形状。 */
    CHECK_INT(uvcpp_c_ws_client_set_events(g_ws.cli, &ev, NULL), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_ws_client_session_count(g_ws.cli), 0);
    CHECK_INT(uvcpp_c_ws_client_is_open(g_ws.cli), 0);
  }

  snprintf(url, sizeof(url), "ws://127.0.0.1:%d%s", port, path);
  rc = uvcpp_c_ws_client_connect(g_ws.cli, url, on_ws_client_connected, NULL);
  CHECK_INT(rc, UVCPP_C_OK);

  rc = uvcpp_c_ws_client_run(g_ws.cli);
  CHECK(rc >= 0);

  CHECK_INT(g_ws.connected, 1);
  if (expect_echo) {
    CHECK_INT(g_ws.got_text, 1);
    CHECK_STR(g_ws.echo, text);
  }
  if (expect_peer_close) {
    CHECK_INT(g_ws.closed, 1);
    CHECK_INT(g_ws.close_code, (int)UVCPP_C_WS_NORMAL);
    CHECK_STR(g_ws.reason, "server-bye");
  } else {
    /* 本端发起：`close()` 只是**开始**关 —— 发掉 Close 帧、把关闭握手走完，
     * 会话才真的没了。所以下面那两条读数必须排在**第二次 `run()` 之后**
     * （`close()` 里那两句断言过：它只把状态推到 CLOSING，会话还挂着）。 */
    CHECK_INT(uvcpp_c_ws_client_close(g_ws.cli, UVCPP_C_WS_NORMAL, "bye"),
              UVCPP_C_OK);
    rc = uvcpp_c_ws_client_run(g_ws.cli);
    CHECK(rc >= 0);
    CHECK_INT(uvcpp_c_ws_client_is_open(g_ws.cli), 0);
    CHECK_INT(uvcpp_c_ws_client_session_count(g_ws.cli), 0);
  }

  CHECK_INT(uvcpp_c_ws_client_free(g_ws.cli), UVCPP_C_OK);
  /* 第二次 free 同一个句柄 = `E_STALE`。 */
  CHECK_INT(uvcpp_c_ws_client_free(g_ws.cli), UVCPP_C_E_STALE);
}

/* -------------------------------------------------------------------------
 * 静态目录的造与拆
 * ------------------------------------------------------------------------- */

static int make_static_dir(void) {
  char path[512];
  FILE* f;
  UVCPP_T_MKDIR(kStaticDir);
  snprintf(path, sizeof(path), "%s/%s", kStaticDir, kStaticName);
  f = fopen(path, "wb");
  if (f == NULL) return -1;
  fwrite(kStaticBody, 1, strlen(kStaticBody), f);
  fclose(f);
  return 0;
}

static void remove_static_dir(void) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", kStaticDir, kStaticName);
  remove(path);
  UVCPP_T_RMDIR(kStaticDir);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
  uvcpp_c_app* app = NULL;
  int rc;
  int port;
  int step;

  /* 头与库必须是同一次 configure 出来的：P/Invoke 最常犯的错就是这个。 */
  CHECK_INT((int)uvcpp_c_abi_version(), (int)UVCPP_C_ABI_VERSION);
  CHECK_INT(g_failed, 0);

  if (make_static_dir() != 0) {
    printf("SKIP: 造不出静态目录 %s（这个用例需要可写的当前目录）\n",
           kStaticDir);
    return 0;
  }

  app = uvcpp_c_app_new();
  CHECK(app != NULL);
  if (app == NULL) {
    remove_static_dir();
    return 1;
  }
  g_app = app; /* 回调里要用（`uvcpp_c_req_*` 不带 app 句柄）。 */

  CHECK_INT(uvcpp_c_app_set_host(app, "127.0.0.1"), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_set_port(app, 0), UVCPP_C_OK); /* 0 = 让内核挑 */
  CHECK_INT(uvcpp_c_app_set_loops(app, 1), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_loop_count(app), 1);
  CHECK_INT(uvcpp_c_app_set_server_header(app, "uvcpp-capi-test"), UVCPP_C_OK);

  CHECK_INT(uvcpp_c_app_get(app, "/text", on_text, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_get(app, "/json", on_json, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_post(app, "/echo", on_echo, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_get(app, "/slow", on_slow, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_get(app, "/mw", on_mw_route, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_use(app, on_middleware, NULL), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_serve_static(app, "/static", kStaticDir, NULL),
            UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_websocket(app, kWsPath, on_ws_upgrade, NULL),
            UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_websocket(app, kWsClosePath, on_ws_close_route_upgrade,
                                  NULL),
            UVCPP_C_OK);

  /* 参数校验：空 pattern / 空回调必须是 `E_INVALID_ARG`，不是"注册成功、运行时
   * 才炸"。 */
  CHECK_INT(uvcpp_c_app_get(app, NULL, on_text, NULL), UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_app_get(app, "/x", NULL, NULL), UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_app_websocket(app, NULL, on_ws_upgrade, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_app_serve_static(app, "/s2", NULL, NULL),
            UVCPP_C_E_INVALID_ARG);

  rc = uvcpp_c_app_start_background(app);
  CHECK_INT(rc, 0);
  if (rc != 0) {
    printf("FAIL: start_background 返回 %d (%s)\n", rc, uvcpp_c_strerror(rc));
    ++g_failed;
    uvcpp_c_app_free(app);
    remove_static_dir();
    return 1;
  }
  CHECK_INT(uvcpp_c_app_running(app), 1);

  port = uvcpp_c_app_bound_port(app);
  CHECK(port > 0);

  /* ---------------------------------------------------------------- HTTP */
  memset(&g_http, 0, sizeof(g_http));
  g_http.cli = uvcpp_c_http_client_new();
  CHECK(g_http.cli != NULL);
  if (g_http.cli != NULL && port > 0) {
    CHECK_INT(uvcpp_c_http_client_last_error(g_http.cli), 0);
    /* keep-alive 开着（其实也是默认）：七条请求共用一条 TCP 连接。 */
    CHECK_INT(uvcpp_c_http_client_set_keep_alive(g_http.cli, 1), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_http_client_connect(g_http.cli, "127.0.0.1", port,
                                          on_http_connected, NULL),
              UVCPP_C_OK);

    /* 第 0 条在连接回调里发出去，所以这一趟把"连上"与"第 0 条"一起跑掉。 */
    rc = uvcpp_c_http_client_run(g_http.cli);
    CHECK(rc >= 0);
    CHECK_INT(g_http.failed, 0);
    if (!g_http.failed) {
      CHECK_INT(g_http.r.seen, 1);
      http_check(0);

      for (step = 1; step <= 6 && !g_http.failed; ++step) {
        CHECK_INT(http_issue(step), UVCPP_C_OK);
        rc = uvcpp_c_http_client_run(g_http.cli);
        CHECK(rc >= 0);
        if (g_http.failed) break;
        CHECK_INT(g_http.r.seen, 1);
        http_check(step);
      }
      CHECK_INT(g_http.failed, 0);
    }

    /* 收摊：先断连接再停循环（keep-alive 开着时连接不会自己断）。 */
    CHECK_INT(uvcpp_c_http_client_close(g_http.cli), UVCPP_C_OK);
    uvcpp_c_http_client_run(g_http.cli);
    /* 关完之后必须**不再**自称已连上。这一条只有真去问底下那条 TCP 连接才答得
     * 对 —— `uvcpp_http_client::has_status()` 是按位或，`CONNECTED` 一旦置上就
     * 再也不会清（它连 `CLOSED` 都不置）。 */
    CHECK_INT(uvcpp_c_http_client_is_connected(g_http.cli), 0);

    /* 那枚响应句柄是**回调期句柄**：拿回调之外再问它，必须是 `E_STALE`（先查
     * 登记表、一个字节都不读那个地址）。 */
    CHECK(g_http.r.handle != NULL);
    CHECK_INT(uvcpp_c_http_response_status_code(
                  (uvcpp_c_http_response*)g_http.r.handle),
              UVCPP_C_E_STALE);

    CHECK_INT(uvcpp_c_http_client_free(g_http.cli), UVCPP_C_OK);
    /* 第二次 free 同一个句柄 = `E_STALE`。 */
    CHECK_INT(uvcpp_c_http_client_free(g_http.cli), UVCPP_C_E_STALE);
  }

  /* ------------------------------------------------------------ WebSocket */
  if (port > 0) {
    /* `/ws`：回显 → 本端关（服务端报 1000；客户端那枚 on_close 是竞态，
     * 不断言）。 */
    ws_run_session(port, kWsPath, kWsText, 1, 0);
    /* `/ws-close`：服务端关（客户端一定收到 on_close，码与原因逐字对）。 */
    ws_run_session(port, kWsClosePath, kWsText, 0, 1);

    /* 非法 URL：当场拒绝，而且要**可辨认**（不是"连不上"）。 */
    {
      uvcpp_c_ws_client* c = uvcpp_c_ws_client_new();
      CHECK(c != NULL);
      if (c != NULL) {
        CHECK_INT(uvcpp_c_ws_client_connect(c, "http://127.0.0.1/",
                                            on_ws_client_connected, NULL),
                  UVCPP_C_E_INVALID_ARG);
        CHECK_INT(uvcpp_c_ws_client_free(c), UVCPP_C_OK);
      }
    }
  }

  /* ----------------------------------------------------- 回调期句柄越界
   *
   * **这两条必须在 `stop()` / `join()` 之前问**，这是量出来的，不是讲究。
   *
   * 第一版放在 `join()` 之后，看起来更"时序确定"，其实什么也没量到：服务端那
   * 条线程一退出，它那块栈（回调期句柄就住在上面）被 glibc 收回去重新映射成零
   * 页，`alive()` 里那句读魔数读到的是 0 —— 断言照样过，过的是"那块内存被清零
   * 了"，不是"这道守卫把它挡住了"；而且那一刻那块内存**已经不属于这个进程**，
   * 读它本身就是潜伏的 UB。
   *
   * 挪到 `join()` 之前以后，读的至少是一条活着的线程的栈：合法，且语义清楚。
   *
   * **但要如实说一句：这两条量的是"契约"，不是"机制"。** 变异表 M8（把
   * `FrameScope` 析构里那句"全部摘表"拆掉）跑下来，这两条**照样过** —— 我原本
   * 预期它会红在 `E_WRONG_THREAD` 上，实测不是。原因是 `creq` / `cresp` 是
   * `route_trampoline` 的栈上局部量，回调返回之后那块栈立刻被后续的循环代码重
   * 用，魔数被无关的写入盖掉了，于是 `alive()` 在"魔数"那一句就判假、根本走不
   * 到线程检查。也就是说：**只要句柄在栈上，魔数这个判据就区分不出"被毒化"和
   * "被栈复用盖掉"** —— M8 只有 `uvcpp_c_live_handle_count()` 量得出来。
   *
   * 这就是本层为什么**先查登记表、再读魔数**（`uvcpp_c_internal.h` 里 `alive()`
   * 那段）的实证：M8 下登记表里那枚地址还在，于是代码真去读了那块栈；基线里它
   * 被登记表那一句挡在读内存之前。两者对外都给 `E_STALE`，差别在"有没有读一块
   * 不该读的内存"上 —— 这正是那张表的代价买回来的东西。 */
  CHECK(g_srv.escaped_req != NULL);
  CHECK(g_srv.escaped_resp != NULL);
  CHECK_INT(uvcpp_c_req_path((uvcpp_c_req*)g_srv.escaped_req, NULL, 0),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_resp_ended((uvcpp_c_resp*)g_srv.escaped_resp),
            UVCPP_C_E_STALE);
  /* 顺带：这时候 app 句柄还好好活着，说明上面两条 `E_STALE` 不是"整个 app 连
   * 句柄都没了"那种误判。 */
  CHECK_INT(uvcpp_c_app_running(app), 1);

  /* 停机之后那些计数才是**最终值**（`join()` 回来那条循环上所有回调都跑完
   * 了，这里的每一个数不依赖任何时序）。 */
  CHECK_INT(uvcpp_c_app_stop(app), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_join(app), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_app_running(app), 0);

  /* -------------------------------------------------------------- 服务端 */
  CHECK_INT(g_srv.slow_posted, 1);
  CHECK_INT(g_srv.slow_done, 1);
  /* 七条 HTTP 请求都过中间件（含那条 404 —— 中间件在路由之前）。 */
  CHECK(g_srv.mw_hits >= 7);
  /* 两次升级；两条文本帧（`/ws` 的回显 + `/ws-close` 那条触发关闭的）。 */
  CHECK_INT(g_srv.ws_upgrades, 2);
  CHECK_INT(g_srv.ws_echoes, 2);
  /* `/ws` 那一场是**客户端**发起关闭 —— 服务端一定报出来，且必须报 1000
   * （断言在 `on_ws_close` 里，这里只量次数）。 */
  CHECK_INT(g_srv.ws_closes, 1);

  CHECK_INT(uvcpp_c_app_free(app), UVCPP_C_OK);
  /* 第二次 free 同一个句柄 = `E_STALE`。 */
  CHECK_INT(uvcpp_c_app_free(app), UVCPP_C_E_STALE);

  remove_static_dir();

  /* 登记表收支平衡：所有句柄都还回去了。这是"释放时忘了摘表"那类漏**唯一**可
   * 观测的形式（见 `uvcpp_c_common.h` 里 `uvcpp_c_live_handle_count()` 那段 ——
   * 光靠"释放后再用必须是 E_STALE"量不出来）。 */
  CHECK_INT((int)uvcpp_c_live_handle_count(), 0);

  printf("checks=%d failures=%d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
