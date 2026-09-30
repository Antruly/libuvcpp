/**
 * @file tests/capi/capi_common_func.c
 * @brief C ABI 地基的用例：ABI 自洽、错误码文案、句柄的生死、事件表的 size 规则。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 这份文件是**纯 C**（`.c`），而且刻意不 include 任何 C++ 头 —— 编译它这件事
 * 本身就是一条判据（见 tests/capi/CMakeLists.txt 里那段）。
 *
 * 它量的是五件在 P/Invoke 那一侧真会出事的事情：
 *
 *   1. **头与库是不是一次编出来的**（`uvcpp_c_abi_version()` 与宏不等时就该
 *      当场红，而不是在 C# 里表现为一句毫无线索的访问违例）；
 *   2. **用过的句柄给错误码、不给段错误** —— 包括"释放之后再调一次"这条
 *      （这是本层对 FFI 侧最有价值的一条承诺，所以它必须有用例钉着）；
 *   3. **活句柄数收支平衡**（`uvcpp_c_live_handle_count()`）—— 见下面 3c 那一段
 *      的长注释：这一条**不是** 2. 的重复，因为 2. 量的是分配器的行为，
 *      只有 3. 量得到登记表到底有没有在回收；
 *   4. **事件表按 `size` 逐字段看**（老客户端只填前几格时，后面那几格不许被读到）；
 *   5. 错误码文案表没有抄串格。
 */

#include <capi/uvcpp_c.h>

#include <stdio.h>
#include <string.h>

#include <uv.h>

/* 这一份库里编进了 C 面吗？`uvcpp_c.h` 是按生成的 `uvcpp_config.h` 里的真实
 * 开关拉头的，所以这一条同时也在断言"这个构建的开关与用例的存在性一致"。 */
#if !UVCPP_CAPI_ENABLE
#  error "这个用例只应在 UVCPP_ENABLE_CAPI=ON 的构建里被编译"
#endif

/* 由 CMake 传进来的版本前缀（`uvcpp_version.h` 是 C++ 头，C 这边看不见它）。
 * 两边同源：CMake 也是 configure 期从那个头里抓的。 */
#ifndef UVCPP_TEST_VERSION
#  define UVCPP_TEST_VERSION "0.0.0"
#endif

#define kMsgMax 64

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
 * 1. 地基自洽
 * ------------------------------------------------------------------------- */

static void test_abi_and_version(void) {
  const char* v;

  /* ABI 自洽：库里的那个数与头里的宏必须**逐位相等**。P/Invoke 最常见的故障
   * （头与 .so/.dll 不是一次编出来的）就死在这一点上，而它的症状通常是一句
   * 毫无线索的访问违例 —— 所以这里先把它变成一个能读懂的失败。 */
  CHECK_INT(uvcpp_c_abi_version(), UVCPP_C_ABI_VERSION);
  CHECK(UVCPP_C_ABI_VERSION >= 1);

  v = uvcpp_c_version_string();
  CHECK(v != NULL);
  if (v != NULL) {
    CHECK(strlen(v) > 0);
    /* 前缀相等：库里那份串是 "1.4.2-dev"（开发版带后缀），头里抓出来的是
     * "1.4.2"，所以比前缀而不是全等。 */
    CHECK(strncmp(v, UVCPP_TEST_VERSION, strlen(UVCPP_TEST_VERSION)) == 0);
  }

  /* 刚起来的进程上还没记过任何异常 */
  CHECK(uvcpp_c_last_error_string() == NULL);
}

/* -------------------------------------------------------------------------
 * 2. 错误码文案
 * ------------------------------------------------------------------------- */

static void test_strerror(void) {
  /* 每一格的文案：非空、且**互不相同**（抄串格时通常就是两格变成一样，
   * 而"两格一样"在调用方看来是"两个不同的错给了同一句话"）。 */
  static const int kCodes[] = {
      UVCPP_C_E_INVALID_ARG, UVCPP_C_E_STALE,       UVCPP_C_E_EXCEPTION,
      UVCPP_C_E_STATE,       UVCPP_C_E_NO_MEMORY,   UVCPP_C_E_UNSUPPORTED,
      UVCPP_C_E_NOT_BUILT,   UVCPP_C_E_WRONG_THREAD,
      UVCPP_C_E_BUFFER_TOO_SMALL};
  const int n = (int)(sizeof(kCodes) / sizeof(kCodes[0]));
  int i;
  int j;

  CHECK(strcmp(uvcpp_c_strerror(UVCPP_C_OK), "ok") == 0);

  for (i = 0; i < n; ++i) {
    const char* a = uvcpp_c_strerror(kCodes[i]);
    CHECK(a != NULL);
    CHECK(a != NULL && strlen(a) > 0);
    for (j = i + 1; j < n; ++j) {
      const char* b = uvcpp_c_strerror(kCodes[j]);
      CHECK(a != NULL && b != NULL && strcmp(a, b) != 0);
    }
    /* 一段本层的码不许被当成 libuv 的码去翻译 */
    CHECK(a != NULL && strcmp(a, "unknown error code") != 0);
  }

  /* libuv 的码**原样透传**：这一条是"两种码混在同一条 int 上"那条设计的判据
   * —— -20000 那一段与 libuv 的 -errno 不可能撞，所以这里能拿到 libuv 自己
   * 的文案，而不用先判断"这是谁家的码"。 */
  CHECK(uvcpp_c_strerror(UV_ECONNREFUSED) != NULL);
  CHECK(strcmp(uvcpp_c_strerror(UV_ECONNREFUSED), "unknown error code") != 0);
  CHECK(strcmp(uvcpp_c_strerror(UV_ECONNREFUSED), "ok") != 0);

  /* 谁都不是的码 */
  CHECK(strcmp(uvcpp_c_strerror(-99999), "unknown error code") == 0);
}

/* -------------------------------------------------------------------------
 * 3. 句柄的生死
 * ------------------------------------------------------------------------- */

static void test_handle_lifecycle(void) {
  uvcpp_c_tcp_client* c;
  uvcpp_c_tcp_server* s;

  /* 空指针：一律 E_STALE，不区分（对调用方来说要做的事都一样：别用，报错） */
  CHECK_INT(uvcpp_c_tcp_client_free(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_is_connected(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_run(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_close(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_alpn_selected(NULL, NULL, 0),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_server_free(NULL), UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_server_client_count(NULL), UVCPP_C_E_STALE);

  /* 一个**不是本层发出去的**指针：它可能长得像，但表里没有 => E_STALE。
   * 这是"先查活句柄表、再读魔数"那个顺序的判据：只查魔数的话，这一格要靠
   * 运气（栈上恰好不是那个值）。 */
  {
    struct foreign {
      unsigned int magic;
      unsigned int abi;
    } f;
    f.magic = 0x31636865u; /* "ehc1" 的小端读法，就是想蒙对 */
    f.abi = 1u;
    CHECK_INT(uvcpp_c_tcp_client_is_connected((uvcpp_c_tcp_client*)&f),
              UVCPP_C_E_STALE);
    CHECK_INT(uvcpp_c_tcp_server_client_count((uvcpp_c_tcp_server*)&f),
              UVCPP_C_E_STALE);
  }

  /* 真句柄：建得出来、问得出状态 */
  c = uvcpp_c_tcp_client_new();
  CHECK(c != NULL);
  if (c != NULL) {
    CHECK_INT(uvcpp_c_tcp_client_is_connected(c), 0); /* 0 = 还没连上（不是错误） */
    CHECK_INT(uvcpp_c_tcp_client_last_error(c), 0);
    CHECK_INT(uvcpp_c_tcp_client_is_tls(c), 0);
    /* 没协商出 ALPN 时长度是 0；cap=0 这一遍**一个字节都不写** */
    CHECK_INT(uvcpp_c_tcp_client_alpn_selected(c, NULL, 0), 0);

    /* 还没跑过循环 => 线程检查不做（那一刻还没有"循环线程"可言） */
    CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
    /* 双重释放 */
    CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_E_STALE);
    /* ★ 释放之后再调别的：**必须**是错误码，不是崩溃。上面那次 free 已经把
     *   这块内存还给分配器了，本层连一个字节都不读它（登记表先挡住）。 */
    CHECK_INT(uvcpp_c_tcp_client_is_connected(c), UVCPP_C_E_STALE);
    CHECK_INT(uvcpp_c_tcp_client_close(c), UVCPP_C_E_STALE);
  }

  s = uvcpp_c_tcp_server_new();
  CHECK(s != NULL);
  if (s != NULL) {
    CHECK_INT(uvcpp_c_tcp_server_client_count(s), 0);
    CHECK_INT(uvcpp_c_tcp_server_loop_count(s), 1);
    /* 没绑上时 local_port 是"状态不对"，不是 0 —— 0 是个合法端口号 */
    CHECK_INT(uvcpp_c_tcp_server_local_port(s), UVCPP_C_E_STATE);
    CHECK_INT(uvcpp_c_tcp_server_free(s), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_tcp_server_free(s), UVCPP_C_E_STALE);
    CHECK_INT(uvcpp_c_tcp_server_client_count(s), UVCPP_C_E_STALE);
  }
}

/* -------------------------------------------------------------------------
 * 3b. 句柄的类型也得对得上（两种句柄都是 `void*` 的时候最容易犯）
 * ------------------------------------------------------------------------- */
static void test_handle_type_is_checked(void) {
  uvcpp_c_tcp_client* c = uvcpp_c_tcp_client_new();
  uvcpp_c_tcp_server* s = uvcpp_c_tcp_server_new();

  CHECK(c != NULL);
  CHECK(s != NULL);
  if (c == NULL || s == NULL) return;

  /* 两个句柄都是**真的**（在活句柄表里），但类型不对。C# 侧的 `IntPtr` 分不清
   * 它们，所以把服务端句柄传给客户端那一族函数是一次很普通的误用 —— 本层必须
   * 给 `UVCPP_C_E_STALE`，而不是拿着服务端那块内存当客户端去读。
   *
   * `alive()` 的三段里，这一条打的是**第三段**（魔数）：光看登记表的话，这两句
   * 都会"通过"，然后 `((uvcpp_c_tcp_client*)s)->cli` 读到的是服务端结构体里
   * 对应偏移上的另一个指针。 */
  CHECK_INT(uvcpp_c_tcp_client_is_connected((uvcpp_c_tcp_client*)s),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_client_free((uvcpp_c_tcp_client*)s),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_server_client_count((uvcpp_c_tcp_server*)c),
            UVCPP_C_E_STALE);
  CHECK_INT(uvcpp_c_tcp_server_free((uvcpp_c_tcp_server*)c), UVCPP_C_E_STALE);

  /* 反过来再确认一次：被"错用"的那两个句柄本身**没被弄坏** */
  CHECK_INT(uvcpp_c_tcp_client_is_connected(c), 0);
  CHECK_INT(uvcpp_c_tcp_server_client_count(s), 0);

  CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
  CHECK_INT(uvcpp_c_tcp_server_free(s), UVCPP_C_OK);
}

/* -------------------------------------------------------------------------
 * 4. 事件表：调用方给的 size 说了算
 * ------------------------------------------------------------------------- */

static void on_dummy_read(void* ud, uvcpp_c_tcp_client* c,
                          const uvcpp_c_read_result* r) {
  (void)ud;
  (void)c;
  (void)r;
}

/* -------------------------------------------------------------------------
 * 3c. 活句柄数必须收支平衡（**这是本文件里唯一一条能量出登记表在回收的判据**）
 * -------------------------------------------------------------------------
 * 为什么这条断言非要在：把 `*_free()` 里的摘表拆掉之后，"释放后再用必须给
 * E_STALE"那几条**照样全绿** —— 因为 `alive()` 接下来要去读的那 4 个字节
 * （空闲块的第 0 字节）在 glibc 上正好被 tcache 的 `next` 指针改写了，于是
 * "读魔数"那一句给出的仍然是一个不等于魔数的值。也就是说，那几条断言量的是
 * **分配器的行为**，不是本层的守卫。
 *
 * 这一个数换了一条完全不同的路径：它直接问登记表有多大，不依赖内存里剩下什么。
 * `tests/tools/capi_mutation.py` 的 M2（拆摘表、留毒化）与 M3（两道一起拆）就是
 * 靠它咬人的 —— 没有它，那两条变异**在本机上是量不出来的**。
 */
static void test_live_handle_count_balances(void) {
  size_t before = uvcpp_c_live_handle_count();
  uvcpp_c_tcp_client* c;
  uvcpp_c_tcp_server* s;
  int i;

  /* 一轮"建了又废"之后必须回到原来的数（这一句是**相对**的，所以前面那些
   * 用例有没有把句柄清干净不影响它）。 */
  for (i = 0; i < 3; ++i) {
    c = uvcpp_c_tcp_client_new();
    s = uvcpp_c_tcp_server_new();
    CHECK(c != NULL);
    CHECK(s != NULL);
    if (c == NULL || s == NULL) return;
    CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_tcp_server_free(s), UVCPP_C_OK);
  }
  CHECK(uvcpp_c_live_handle_count() == before);

  /* 再确认一次方向：**活着**的句柄真的被数进去了（否则上面那条断言在一个
   * 恒返回 0 的实现上也是绿的 —— 那就是一次空转）。 */
  c = uvcpp_c_tcp_client_new();
  CHECK(c != NULL);
  if (c != NULL) {
    CHECK(uvcpp_c_live_handle_count() == before + 1);
    CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
    CHECK(uvcpp_c_live_handle_count() == before);
  }

  /* 双重释放不许把计数弄少（`registry_remove` 对不在表里的地址必须是无害的 ——
   * 它现在确实无害：`erase()` 一个不存在的键什么也不做）。 */
  c = uvcpp_c_tcp_client_new();
  CHECK(c != NULL);
  if (c != NULL) {
    CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
    CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_E_STALE);
    CHECK(uvcpp_c_live_handle_count() == before);
  }
}

static void on_dummy_close(void* ud) { (void)ud; }

static void on_dummy_status(void* ud, int status) {
  (void)ud;
  (void)status;
}

static void on_dummy_connection(void* ud, uvcpp_c_tcp_client* c) {
  (void)ud;
  (void)c;
}

static void test_events_table_size(void) {
  uvcpp_c_tcp_client* c = uvcpp_c_tcp_client_new();
  uvcpp_c_tcp_client_events ev;

  CHECK(c != NULL);
  if (c == NULL) return;

  /* 完整表 */
  memset(&ev, 0, sizeof(ev));
  ev.size = (uint32_t)sizeof(ev);
  ev.on_read = on_dummy_read;
  ev.on_close = on_dummy_close;
  CHECK_INT(uvcpp_c_tcp_client_set_events(c, &ev), UVCPP_C_OK);

  /* NULL = 全部摘掉 */
  CHECK_INT(uvcpp_c_tcp_client_set_events(c, NULL), UVCPP_C_OK);

  /* ★ 模拟一个"老客户端"：它那个版本的 `uvcpp_c_tcp_client_events` 只有前两格
   *   （`size` 与 `on_read`），所以它报上来的 size 就到这里。本层必须**只读
   *   它声明覆盖的那几格**，而不是把整个结构体读满 —— 读满就是读调用方栈上
   *   别人的字节（那是随机的，于是"有时对有时错"）。
   *
   *   这里那个"老结构体"就摆在**同一个栈帧**上，后面紧跟着的是别的变量：
   *   读满的话读到的正是它们的字节。 */
  {
    struct old_client_events {
      uint32_t size;
      uvcpp_c_read_cb on_read;
    } old_ev;
    old_ev.size = (uint32_t)sizeof(old_ev);
    old_ev.on_read = on_dummy_read;
    CHECK_INT(uvcpp_c_tcp_client_set_events(
                  c, (const uvcpp_c_tcp_client_events*)&old_ev),
              UVCPP_C_OK);
  }

  /* size 只说清了自己这一格（>= 4）：合法，但每一格都"没覆盖到" => 全部当
   * 不关心。这不是错误。 */
  {
    struct size_only {
      uint32_t size;
    } so;
    so.size = (uint32_t)sizeof(so);
    CHECK_INT(uvcpp_c_tcp_client_set_events(
                  c, (const uvcpp_c_tcp_client_events*)&so),
              UVCPP_C_OK);
  }

  /* size 说出的数**比它自己那一格还小**：一张连自己多大都说不清的表，一格都
   * 不可信 —— 这是参数错，当场拒绝。
   *
   * 注意那个字段的类型是 `uint32_t`，与公开头里逐字相同：`size` 是**约定好的
   * 第一格**（偏移 0、四字节），它的类型是 ABI 的一部分。这里不能用 `uint16_t`
   * 去"声明一个更小的表"—— 那样本层去读 `table->size` 时读的是调用方没声明过的
   * 两个字节，正是这套 `size` 规则要防的事；反过来本层也不可能读两个字节就猜出
   * 调用方想说什么。所以"表太小"这件事只能由那个 `uint32_t` 自己说出来。 */
  {
    struct too_small {
      uint32_t size;
    } ts;
    ts.size = 2u; /* < sizeof(uint32_t) */
    CHECK_INT(uvcpp_c_tcp_client_set_events(
                  c, (const uvcpp_c_tcp_client_events*)&ts),
              UVCPP_C_E_INVALID_ARG);
  }

  /* 参数安检：这一批的调用都发生在"还没连上"的时候，所以它们判的都是参数，
   * 不会去碰 socket。 */
  CHECK_INT(uvcpp_c_tcp_client_connect(c, NULL, 80, on_dummy_status, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_tcp_client_connect(c, "127.0.0.1", 0, on_dummy_status, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_tcp_client_connect(c, "127.0.0.1", 70000, on_dummy_status,
                                       NULL),
            UVCPP_C_E_INVALID_ARG);
  /* 没给完成回调：C 侧的 `connect` 是异步的那一条，同步等要显式用
   * `connect_wait`（否则一个不经意的 NULL 就变成一次阻塞调用） */
  CHECK_INT(uvcpp_c_tcp_client_connect(c, "127.0.0.1", 80, NULL, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_tcp_client_write(c, "x", 1, NULL, NULL),
            UVCPP_C_E_INVALID_ARG);
  CHECK_INT(uvcpp_c_tcp_client_write(c, NULL, 1, on_dummy_status, NULL),
            UVCPP_C_E_INVALID_ARG);
  /* NULL + 0 是合法的（零长度写）——但它没连上，所以是 ENOTCONN 那条路，
   * 不是参数错。这里只断言"不是 INVALID_ARG"。 */
  CHECK(uvcpp_c_tcp_client_write(c, NULL, 0, on_dummy_status, NULL) !=
        UVCPP_C_E_INVALID_ARG);

  /* 服务端那一侧的形状相同 */
  {
    uvcpp_c_tcp_server* s = uvcpp_c_tcp_server_new();
    uvcpp_c_tcp_server_events sev;
    struct server_too_small {
      uint32_t size;
    } ts;
    CHECK(s != NULL);
    if (s != NULL) {
      memset(&sev, 0, sizeof(sev));
      sev.size = (uint32_t)sizeof(sev);
      sev.on_connection = on_dummy_connection;
      sev.on_read = on_dummy_read;
      CHECK_INT(uvcpp_c_tcp_server_set_events(s, &sev), UVCPP_C_OK);
      CHECK_INT(uvcpp_c_tcp_server_set_events(s, NULL), UVCPP_C_OK);

      ts.size = 2u; /* 同上：表太小只能由那个 uint32_t 自己说出来 */
      CHECK_INT(uvcpp_c_tcp_server_set_events(
                    s, (const uvcpp_c_tcp_server_events*)&ts),
                UVCPP_C_E_INVALID_ARG);

      CHECK_INT(uvcpp_c_tcp_server_bind(s, NULL, 0), UVCPP_C_E_INVALID_ARG);
      CHECK_INT(uvcpp_c_tcp_server_bind(s, "127.0.0.1", -1),
                UVCPP_C_E_INVALID_ARG);
      CHECK_INT(uvcpp_c_tcp_server_bind(s, "127.0.0.1", 70000),
                UVCPP_C_E_INVALID_ARG);
      CHECK_INT(uvcpp_c_tcp_server_set_loops(s, 0), UVCPP_C_E_INVALID_ARG);
      CHECK_INT(uvcpp_c_tcp_server_set_loops(s, 1), UVCPP_C_OK);
      CHECK_INT(uvcpp_c_tcp_server_loop_count(s), 1);

      CHECK_INT(uvcpp_c_tcp_server_free(s), UVCPP_C_OK);
    }
  }

  CHECK_INT(uvcpp_c_tcp_client_free(c), UVCPP_C_OK);
}

int main(void) {
  printf("[capi common] start\n");

  test_abi_and_version();
  test_strerror();
  test_handle_lifecycle();
  test_handle_type_is_checked();
  test_live_handle_count_balances();
  test_events_table_size();

  printf("[capi common] checks=%d failures=%d\n", g_checks, g_failed);
  if (g_failed != 0) {
    printf("[capi common] FAILED: %d\n", g_failed);
    return 1;
  }
  printf("[capi common] PASS\n");
  return 0;
}
