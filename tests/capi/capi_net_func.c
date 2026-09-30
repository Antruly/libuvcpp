/**
 * @file tests/capi/capi_net_func.c
 * @brief net 层的 C ABI 端到端用例：**真起一个 TCP 服务端、真发字节、真收字节**。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这份用例是**纯 C**（`.c`，不 include 任何 C++ 头），跑起来是两个真端点：
 * 主线程是服务端（它自己那条循环），另一条线程是客户端。一次完整的往返：
 *
 *   连接建立 → 服务端发一句 hello → 客户端收下并核对逐字节相等
 *   → 客户端回一句 echo → 服务端收下并核对 → 客户端关闭
 *   → 服务端看见对端关闭 → 停 → 两条循环各自排空、`run()` 各自返回
 *
 * 除了"真收到字节"这条主线，它还钉住四条**只有 C 面才有**的规矩：
 *
 *   1. **服务端交出来的连接句柄不归调用方**：`free()` 返回 `E_STATE`，`run()` /
 *      `stop()` 也一样（它没有自己的循环）；
 *   2. **连接关掉之后那个句柄必须失效**：本层在框架删掉 C++ 对象**之前**把它从
 *      活句柄表里摘掉，所以之后再用它是 `E_STALE`，不是崩溃；
 *   3. **线程规则**：从客户端那条线程上问服务端的句柄，必须拿到
 *      `E_WRONG_THREAD`（这是"一个句柄只在它那条循环的线程上用"那条纪律唯一
 *      能被自动抓到的形态）；
 *   4. **回调里不许 `free()`**：回调期间释放自己的句柄返回 `E_STATE`。
 */

#include <capi/uvcpp_c.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uv.h>

#if !UVCPP_CAPI_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_CAPI=ON 的构建里被编译"
#endif

/* 两边各说一句话，长度不同 —— 这样"谁收到的"不会因为长度一样而看不出来。 */
#define kHello "hello-from-server"
#define kEcho "ping-from-c"
#define kAccumMax 64

static int g_checks = 0;
static int g_failed = 0;

/* 每次判定都记一笔"量了几条"，末尾印一行 `checks=… failures=…` —— 这是本仓
 * 所有用例的统一形状，也是 `tests/tools/` 下那些驱动判断"到底是它红的、还是
 * 被别人的红带下去的"的依据（见 `capi_mutation.py`）。 */
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

/* -------------------------------------------------------------------------
 * 累计器
 * -------------------------------------------------------------------------
 * 读事件**不保证一次给完**（TCP 是一条字节流，不是消息流）：11 个字节在局域
 * 网上通常是一次给完，但"通常"不是判据。所以两边都先攒够再比 —— 这样这条
 * 用例量的是"字节对不对"，而不是"内核这一次切了几刀"。
 */
typedef struct {
  char buf[kAccumMax];
  int len;
} accum;

static void accum_reset(accum* a) { a->len = 0; }

static int accum_push(accum* a, const char* data, size_t size) {
  if (data == NULL) return 0;
  if (a->len + (int)size > kAccumMax) return 0;
  memcpy(a->buf + a->len, data, size);
  a->len += (int)size;
  return 1;
}

static int accum_is(accum* a, const char* want) {
  size_t n = strlen(want);
  return a->len == (int)n && memcmp(a->buf, want, n) == 0;
}

/* -------------------------------------------------------------------------
 * 两侧的状态
 * ------------------------------------------------------------------------- */

typedef struct {
  uvcpp_c_tcp_server* srv;
  int connections;         /* 接受了几条 */
  int client_count_in_cb;  /* 连接回调里问服务端："现在挂着几条" */
  int free_conn_in_cb;     /* 在回调里 free 服务端交出来的连接 -> 应当是 E_STATE */
  int run_conn_in_cb;      /* 对它调 run -> E_STATE（它没有自己的循环） */
  int conn_connected;      /* 连接回调里问它连上了没 -> 1 */
  int conn_ud_ok;          /* 连接回调收到的 user_data 就是装进去的那个 -> 1 */
  int bad_read_ud;         /* 读回调收到的 user_data 不是 NULL（见 main 那张表） */
  int data_events;
  int end_events;
  accum in;                /* 收到的（= 客户端那句 echo） */
  accum echoed;            /* 发出去的（= 服务端那句 hello） */
  int hello_write_status;
  int hello_write_calls;
  uvcpp_c_tcp_client* conn; /* 只记下这个"值"；run() 返回之后先验它已经 E_STALE */
} server_side;

/**
 * 服务端那两个回调挂在**服务端**状态上，但读回调那一格是按"不声明 user_data"
 * 装的（见 main 里那段），所以它收到的 user_data 是 NULL —— 这条用例要的正是
 * 那个 NULL。于是它通过这个文件级指针拿到状态，而不是顺着 ud 走。
 */
static server_side* g_srv = NULL;

typedef struct {
  int port;
  uvcpp_c_tcp_server* srv; /* 只为从客户端线程上问一句"线程不对" */
  int connect_status;
  int connected;
  int data_events;
  int closed;
  int wrong_thread_code;
  int free_in_cb;          /* 在读回调里 free 自己 -> 应当是 E_STATE */
  int connected_after_run; /* run() 返回之后再问一次 -> 应当是 0（已经关了） */
  accum in;                /* 收到的（= 服务端那句 hello） */
  accum sent;              /* 发出去的（= 客户端那句 echo） */
  int echo_write_status;
} client_side;

/* -------------------------------------------------------------------------
 * 服务端（主线程）
 * ------------------------------------------------------------------------- */

static void on_hello_done(void* ud, int status) {
  server_side* ss = (server_side*)ud;
  ss->hello_write_status = status;
}

static void on_conn_read(void* ud, uvcpp_c_tcp_client* conn,
                         const uvcpp_c_read_result* r) {
  server_side* ss = g_srv;

  /* 那份共用的读回调装的时候**没声明** user_data 那一格（见 main），所以这里
   * 必须是 NULL。把这一条写成断言而不是"就当没事"：它就是"本层只读调用方声明
   * 覆盖到了的那几格"这条规矩在**这一层**唯一能被观测到的形态
   * （`tests/tools/capi_mutation.py` 的 M2 打的正是它）。 */
  if (ud != NULL) {
    ++ss->bad_read_ud;
    (void)uvcpp_c_tcp_server_stop(ss->srv);  /* 让它停下来，别挂到超时 */
    return;
  }

  if (r->event == UVCPP_C_READ_DATA) {
    ++ss->data_events;
    /* DATA 这一档的四个字段各有承诺：data 非空、size 是字节数、error 是 0、
     * fin 在 TCP 上恒为 0（FIN 是另一次独立的读事件）。 */
    CHECK(r->data != NULL);
    CHECK(r->size > 0);
    CHECK_INT(r->error, 0);
    CHECK_INT(r->fin, 0);
    CHECK(accum_push(&ss->in, r->data, r->size));
    return;
  }

  /* 对端关闭（或读错误）。两条路都当作"这条连接走完了" —— 本层的服务端没有别的
   * 办法知道一次对谈结束，而循环要有人去停。 */
  ++ss->end_events;
  if (r->event == UVCPP_C_READ_ERROR) {
    printf("[capi net] server read error: %d (%s)\n", r->error,
           uvcpp_c_strerror(r->error));
  }
  CHECK_INT(uvcpp_c_tcp_server_stop(ss->srv), UVCPP_C_OK);
}

static void on_connection(void* ud, uvcpp_c_tcp_client* conn) {
  server_side* ss = g_srv;
  uvcpp_c_tcp_client_events ev;

  /* 这一格是**声明过**的，所以它就是装进去的那个值（不许是 NULL，也不许是
   * 别的东西）—— 与上面读回调那条正好是一对：一格声明过、一格没声明。 */
  if (ud == (void*)g_srv) ss->conn_ud_ok = 1;

  ++ss->connections;
  ss->conn = conn;

  /* 这一刻：连接已经建立、服务端登记表里正好一条 */
  CHECK_INT(uvcpp_c_tcp_client_is_connected(conn), 1);
  ss->conn_connected = uvcpp_c_tcp_client_is_connected(conn);
  ss->client_count_in_cb = uvcpp_c_tcp_server_client_count(ss->srv);

  /* ★ 这条连接**归框架**：释放它、或者拿它去跑循环，都是状态错，不是成功 */
  ss->free_conn_in_cb = uvcpp_c_tcp_client_free(conn);
  ss->run_conn_in_cb = uvcpp_c_tcp_client_run(conn);

  /* 说第一句话。这一步**不在读回调里**是刻意的：一条连接上同时只允许一笔
   * 异步写在途，而在连接回调里写、之后只读不回，就永远不会撞上那条规矩。 */
  ++ss->hello_write_calls;
  CHECK_INT(uvcpp_c_tcp_client_write(conn, kHello, strlen(kHello), on_hello_done,
                                     ss),
            UVCPP_C_OK);

  /* 顺带把"这个句柄能装事件"这件事也走一遍：装一张空表 = 把回调全摘掉，
   * 而服务端那份共用的读回调（下面在 listen 之前装的那份）仍然是兜底 ——
   * 这里**只断言调用成功**，读的归属留给上面那条 on_read 去量。 */
  memset(&ev, 0, sizeof(ev));
  ev.size = (uint32_t)sizeof(ev);
  CHECK_INT(uvcpp_c_tcp_client_set_events(conn, &ev), UVCPP_C_OK);
}

/* -------------------------------------------------------------------------
 * 客户端（另一条线程）
 * ------------------------------------------------------------------------- */

static void on_echo_done(void* ud, int status) {
  client_side* cs = (client_side*)ud;
  cs->echo_write_status = status;
  /* 话说完就关。关闭是异步的，close 回调稍后响。 */
  if (status == 0) {
    /* 拿不到句柄了 —— 写回调只捕获标量。所以这里改用 `on_close` 收尾，
     * 关闭的发起放在下面的读回调里（那时句柄还在手上）。 */
  }
}

static void on_client_read(void* ud, uvcpp_c_tcp_client* c,
                           const uvcpp_c_read_result* r) {
  client_side* cs = (client_side*)ud;

  if (r->event == UVCPP_C_READ_DATA) {
    ++cs->data_events;
    CHECK(r->data != NULL);
    CHECK_INT(r->error, 0);
    CHECK_INT(r->fin, 0);
    CHECK(accum_push(&cs->in, r->data, r->size));

    if (!accum_is(&cs->in, kHello)) return; /* 还没攒够 */

    /* ★ 从**这条**线程上问服务端的句柄：它不是这条循环的线程，必须是
     *   `E_WRONG_THREAD`（而不是一个"看起来对"的条数）。 */
    cs->wrong_thread_code = uvcpp_c_tcp_server_client_count(cs->srv);

    /* ★ 回调里释放自己的句柄：不许（会把这个对象从它自己的回调栈上删掉） */
    cs->free_in_cb = uvcpp_c_tcp_client_free(c);

    /* 回一句，然后就关 */
    CHECK(accum_push(&cs->sent, kEcho, strlen(kEcho)));
    CHECK_INT(uvcpp_c_tcp_client_write(c, kEcho, strlen(kEcho), on_echo_done, cs),
              UVCPP_C_OK);
    CHECK_INT(uvcpp_c_tcp_client_close(c), UVCPP_C_OK);
    return;
  }

  ++cs->data_events; /* 结束事件也记一笔，免得"少了一次读"被静默吞掉 */
}

static void on_client_close(void* ud) {
  client_side* cs = (client_side*)ud;
  cs->closed = 1;
}

static void client_thread(void* arg) {
  client_side* cs = (client_side*)arg;
  uvcpp_c_tcp_client* c;
  uvcpp_c_tcp_client_events ev;

  c = uvcpp_c_tcp_client_new();
  CHECK(c != NULL);
  if (c == NULL) return;

  memset(&ev, 0, sizeof(ev));
  ev.size = (uint32_t)sizeof(ev);
  ev.on_read = on_client_read;
  ev.on_read_user_data = cs;
  ev.on_close = on_client_close;
  ev.on_close_user_data = cs;

  /* ★ 装事件表**在 connect 之前** —— 这是 C# 侧最自然的写法（先把回调配好再
   *   连），而"装读回调"这一步在一条还没连上的流上是非法的（`uv_read_start`
   *   返回 UV_ENOTCONN）。本层把它推迟到连接建立之后补装，这条用例量的就是
   *   那个推迟有效。 */
  CHECK_INT(uvcpp_c_tcp_client_set_events(c, &ev), UVCPP_C_OK);

  cs->connect_status = uvcpp_c_tcp_client_connect_wait(c, "127.0.0.1", cs->port,
                                                       5000);
  CHECK_INT(cs->connect_status, 0);
  if (cs->connect_status != 0) {
    uvcpp_c_tcp_client_free(c);
    return;
  }
  cs->connected = uvcpp_c_tcp_client_is_connected(c);
  CHECK_INT(cs->connected, 1);

  /* 循环跑到没有活跃句柄为止（连接的关闭发生在读回调里，收尾之后自然排空）。 */
  CHECK_INT(uvcpp_c_tcp_client_run(c), 0);

  /* run 回来之后：连接已经关了，句柄还在（归本线程，循环线程也是本线程） */
  cs->connected_after_run = uvcpp_c_tcp_client_is_connected(c);
  CHECK_INT(cs->connected_after_run, 0);
  CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
  /* 释放之后再问一次：E_STALE */
  CHECK_INT(uvcpp_c_tcp_client_is_connected(c), UVCPP_C_E_STALE);
}

/* -------------------------------------------------------------------------
 * C++ 异常**不许**越过 C 边界（规矩 2）
 * -------------------------------------------------------------------------
 * 这一条要的不是"我们很小心的 try/catch"，而是**能被打红**：先让 C++ 侧真的
 * 抛一声，再看调用方拿到的是错误码还是整个进程没了。
 *
 * 触发点挑得刻意：**一条连接上先注册异步 connect、再调同步 `connect_wait`**。
 * C++ 那一层对这件事是**抛 `std::runtime_error`**（`uvcpp_tcp_client.cpp` 里
 * `connect_wait` 开头那句），因为这两种用法混在一起它没法替调用方决定听谁的。
 * 对 C# 侧来说这是一次再普通不过的误用（异步 `Begin` 之后又调了一次同步
 * `Connect`），所以它恰好是"必须给错误码、不许给进程终止"的那一类。
 *
 * 判据三条：
 *   1. 拿到 `UVCPP_C_E_EXCEPTION`（而不是 `UVCPP_C_E_STATE` 之类的近似值 ——
 *      那样就说不清到底是哪一号守卫挡住的）；
 *   2. `uvcpp_c_last_error_string()` 里那句话**是 C++ 那句话**（含 `connect_wait`
 *      这个方法名）—— 异常没丢，只是被转述了；
 *   3. 之后这个句柄还能照常用（`free` 成功），也就是说抛异常的那条路上没有
 *      半成品状态留下来。
 */
static void on_async_connect_status(void* ud, int status) {
  int* got = (int*)ud;
  *got = status;
}

static void test_exception_never_escapes(void) {
  uvcpp_c_tcp_client* c;
  int async_status = 12345;
  int rc;
  const char* what;

  printf("[capi net] C++ 异常不许越过 C 边界\n");

  c = uvcpp_c_tcp_client_new();
  CHECK(c != NULL);
  if (c == NULL) return;

  /* 异步那一条先注册上（端口 1 上不会有东西在听，所以它注定以失败告终 ——
   * 那不影响这条判据：要的是"注册过"，不是"连上了"）。 */
  rc = uvcpp_c_tcp_client_connect(c, "127.0.0.1", 1, on_async_connect_status,
                                  &async_status);
  CHECK_INT(rc, UVCPP_C_OK);

  /* 同步那一条：C++ 侧会抛。 */
  rc = uvcpp_c_tcp_client_connect_wait(c, "127.0.0.1", 1, 1000);
  CHECK_INT(rc, UVCPP_C_E_EXCEPTION);

  what = uvcpp_c_last_error_string();
  CHECK(what != NULL);
  CHECK(what != NULL && strstr(what, "connect_wait") != NULL);

  /* 让那笔异步 connect 走完（它会失败，失败要**原样**报给状态回调 ——
   * "libuv 的码直接透传"这条规矩在这里再钉一次，而且它是在抛过异常之后
   * 仍然成立的）。 */
  (void)uvcpp_c_tcp_client_run(c);
  CHECK(async_status < 0);

  /* 抛过异常的句柄照常能释放：那条路上没有半成品。 */
  CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
}

/* -------------------------------------------------------------------------
 * main：先起服务端，再放客户端出去
 * ------------------------------------------------------------------------- */

int main(void) {
  server_side ss;
  client_side cs;
  uvcpp_c_tcp_server* srv;
  uv_thread_t th;
  int port;

  memset(&ss, 0, sizeof(ss));
  memset(&cs, 0, sizeof(cs));

  printf("[capi net] start\n");

  srv = uvcpp_c_tcp_server_new();
  CHECK(srv != NULL);
  if (srv == NULL) return 1;
  ss.srv = srv;
  g_srv = &ss;

  /* ★ 服务端这张表装成 **1.4.1 之前的"老客户端"** 的形状：它声明的 `size` 只
   * 到 `on_read` 那一格为止，`on_read_user_data` 不在其中 —— 而那一格里摆着
   * 一个**认得出是谁的值**（0x5a5a…）。本层若按整张结构体读满，就会把它当成
   * user_data 装进去，于是读回调收到的就不是 NULL 而是那个哨兵（`on_conn_read`
   * 里那条断言会照实报出来）。
   *
   * 正确的行为：没声明的那几格一律"不关心"、清成 NULL —— 所以读回调收到的
   * user_data 必须是 NULL。一条断言同时钉住两件事：声明之外一个字节都不读，
   * 以及"没声明 = NULL"这条可预期性（C# 侧靠它决定要不要在每张表里塞满格子）。
   *
   * `on_connection` 那一格**在声明范围之内**，所以它照常拿到 &ss —— 一格声明过、
   * 一格没声明，两条断言合起来才说明"逐格"这件事是真的在逐格。 */
  {
    union {
      uvcpp_c_tcp_server_events full;
      unsigned char raw[sizeof(uvcpp_c_tcp_server_events)];
    } t;
    memset(&t, 0, sizeof(t));
    t.full.size = (uint32_t)(offsetof(uvcpp_c_tcp_server_events, on_read) +
                             sizeof(t.full.on_read));
    t.full.on_connection = on_connection;
    t.full.on_connection_user_data = &ss;
    t.full.on_read = on_conn_read;
    t.full.on_read_user_data = (void*)(uintptr_t)0x5a5a5a5a5a5a5a5au;
    CHECK_INT(uvcpp_c_tcp_server_set_events(srv, &t.full), UVCPP_C_OK);
  }

  /* 绑 0 让系统挑端口，再用 local_port 问回来 —— 这一条既是"能绑"的判据，
   * 也是客户端要知道往哪连的唯一途径。 */
  CHECK_INT(uvcpp_c_tcp_server_bind(srv, "127.0.0.1", 0), UVCPP_C_OK);
  port = uvcpp_c_tcp_server_local_port(srv);
  CHECK(port > 0 && port <= 65535);
  if (port <= 0) {
    uvcpp_c_tcp_server_free(srv);
    return 1;
  }

  /* backlog <= 0 用 C++ 侧的默认值（128） */
  CHECK_INT(uvcpp_c_tcp_server_listen(srv, 0), UVCPP_C_OK);

  cs.port = port;
  cs.srv = srv;
  CHECK_INT(uv_thread_create(&th, client_thread, &cs), 0);

  /* 服务端的循环：客户端走完之后它自己停（读回调里的 end 分支） */
  CHECK_INT(uvcpp_c_tcp_server_run(srv), 0);
  uv_thread_join(&th);

  /* --- 服务端侧的判据 --- */
  CHECK_INT(ss.connections, 1);
  CHECK_INT(ss.client_count_in_cb, 1);
  CHECK_INT(ss.conn_connected, 1);
  CHECK_INT(ss.hello_write_calls, 1);
  CHECK_INT(ss.hello_write_status, 0);
  CHECK_INT(ss.conn_ud_ok, 1);  /* 声明过的那一格：就是装进去的那个值 */
  CHECK_INT(ss.bad_read_ud, 0); /* ★ 没声明的那一格：没被读过，回调收到 NULL */
  CHECK(ss.data_events >= 1);
  CHECK_INT(ss.end_events, 1);
  CHECK(accum_is(&ss.in, kEcho)); /* ★ 逐字节相等 */
  CHECK_INT(ss.in.len, (int)strlen(kEcho));
  CHECK_INT(uvcpp_c_tcp_server_client_count(srv), 0); /* 连接已经收干净 */

  /* ★ 服务端交出来的句柄：不归调用方；连接关掉之后作废 */
  CHECK_INT(ss.free_conn_in_cb, UVCPP_C_E_STATE);
  CHECK_INT(ss.run_conn_in_cb, UVCPP_C_E_STATE);
  CHECK(ss.conn != NULL);
  CHECK_INT(uvcpp_c_tcp_client_is_connected(ss.conn), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_close(ss.conn), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_free(ss.conn), UVCPP_C_E_STALE);

  /* --- 客户端侧的判据（都是那条线程上记下来的） --- */
  CHECK_INT(cs.connect_status, 0);
  CHECK_INT(cs.connected, 1);
  CHECK_INT(cs.echo_write_status, 0);
  CHECK_INT(cs.closed, 1);
  CHECK(cs.data_events >= 1);
  CHECK(accum_is(&cs.in, kHello));
  CHECK(accum_is(&cs.sent, kEcho));
  CHECK_INT(cs.wrong_thread_code, UVCPP_C_E_WRONG_THREAD);
  CHECK_INT(cs.free_in_cb, UVCPP_C_E_STATE);
  CHECK_INT(cs.connected_after_run, 0);

  /* 服务端释放之后 */
  CHECK_INT(uvcpp_c_tcp_server_free(srv), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_tcp_server_free(srv), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_server_client_count(srv), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_server_local_port(srv), UVCPP_C_E_STALE);

  /* 顺带一条真实的失败路：监听已经关了，再连这个端口应当被拒（libuv 的码
   * **原样透传**，所以判据是"负数"，不必假定是哪一个负数 —— Windows 与
   * Linux 上被拒的码不是同一个）。 */
  {
    uvcpp_c_tcp_client* c2 = uvcpp_c_tcp_client_new();
    int rc;
    CHECK(c2 != NULL);
    if (c2 != NULL) {
      rc = uvcpp_c_tcp_client_connect_wait(c2, "127.0.0.1", port, 3000);
      CHECK(rc < 0);
      CHECK(uvcpp_c_strerror(rc) != NULL);
      CHECK_INT(uvcpp_c_tcp_client_free(c2), UVCPP_C_OK);
    }
  }

  test_exception_never_escapes();

  printf("[capi net] checks=%d failures=%d\n", g_checks, g_failed);
  if (g_failed != 0) {
    printf("[capi net] FAILED: %d\n", g_failed);
    return 1;
  }
  printf("[capi net] PASS\n");
  return 0;
}
