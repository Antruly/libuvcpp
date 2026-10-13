/*
 * 每请求「文件系统系统调用 / 线程原语 / 分配」的**确定性计数器**（LD_PRELOAD 量具）。
 *
 * 为什么要有它
 * ------------
 * 静态腿那几条改动（`1.6.25-dev` 起的 C1–C5）的判据不是 rps，而是
 * 「每请求几次 `realpath`、几次 `stat`、几次投池、几次 `malloc`」——这类数字
 * 是**确定**的，不受压测噪声影响，也因此能在 CI 里当门禁用。但这台机器上
 * `ptrace` 被禁（`/proc/sys/kernel/yama/ptrace_scope`），`strace`/`ltrace` 都
 * 用不了；就算能用，也不该把 `strace` 塞进 CI。所以自己 `LD_PRELOAD` 一个
 * 计数器。
 *
 * 为什么不能只拦 `stat`/`lstat`
 * ----------------------------
 * 三个坑，都是实测踩出来的：
 *
 *   1. **libuv 在 Linux 上走 `statx` 系统调用**，而它是通过 `syscall(SYS_statx,…)`
 *      这个**经 PLT 的包装函数**发出的（`uv__statx`）。所以要拦的是 `syscall()`，
 *      只看 `statx` 这个符号永远是零。
 *   2. **glibc 的 `realpath` 内部用 `__lxstat64`/`__readlink` 这类内部别名**，
 *      不过 PLT —— 一个 5 段路径里的 5 次 `lstat` 一次都拦不到。所以这里的口径
 *      是**「`realpath` 调用次数」**，不是「它内部走了几次 lstat」：前者才是
 *      本库能控制的那一件事（记住解析结果 = 不再调 `realpath`）。
 *   3. **glibc 自己的 `pthread_mutex_lock`/`pthread_cond_wait` 用的是内联 futex
 *      指令**，拦不到；而本仓的 libuv 是**静态链进 `libuvcpp.so`** 的，它提交
 *      线程池任务时调的是 `pthread_cond_signal` —— 那是从 `libuvcpp.so` 出去的
 *      外部调用（`nm -D` 里是 `U`），**经 PLT，拦得到**。于是
 *      `cond_signal` 这一格的语义正是**「每请求投了几次池」**，也就是
 *      `POOL` 与 `LOOP_STAT` 两档唯一的差别。
 *
 *   4. **_FORTIFY_SOURCE 会给 `realpath` 换名字**：缓冲区大小在编译期已知时
 *      （`char buf[4096]; realpath(p, buf);`）glibc 的头把调用改写成
 *      `__realpath_chk()` ——那**不是** `realpath` 这个符号，拦不到，实测那一格
 *      恒为 0（`objdump -dr` 上看得见：定长栈数组那条重定位落在
 *      `__realpath_chk`，而 `realpath(p, NULL)` 那条落在 `realpath`）。所以：
 *      **不要拿定长栈数组去试这一格**，会误以为量具坏了。
 *      本库真正的调用点是 `realpath(path, NULL)`（`uvcpp_web_util.cpp` 的
 *      `real_path_impl()`，让 glibc 自己 malloc）—— 第二个参数是空指针，
 *      没有可比的缓冲大小，走的是**没被改名**的那个符号，这一格如实计到。
 *      反证是 `run_static_fs_gate.py` 的变异 2：在命中路径上加一次
 *      `web_real_path()`，两档读数从 0.000 变成 **1.000**。
 *
 * 分配那三个用 `__libc_malloc` 这一族（glibc 直接导出），不走 `dlsym` ——
 * 于是没有「`dlsym` 自己也要分配」的递归问题。
 *
 * 读数方式有两条，取其一或都用：
 *
 *   - **进程内**：用例 `dlsym(RTLD_DEFAULT, "uvcpp_fscount_read")` 拿到读数函数，
 *     在窗口前后各读一次，差 ÷ 请求数。**零系统调用、零分配**，所以量具自己
 *     不会污染被量的那段。`tests/functional/web_app_static_func.cpp` 的
 *     `fs_count` 用例就是这么读的。
 *   - **进程外**：向被测进程发 `SIGUSR1`，把当前累计值追加进 `$FSCOUNT_OUT`
 *     （`open`/`write`/`close` 都是 async-signal-safe 的）。压测那种"客户端在
 *     另一个进程"的形状只能这么读；注意这条路自己会多出 2~3 次 `openat`/`close`，
 *     好在窗口边界上各一次，除以几千个请求就无所谓了。
 *
 * 编法（`tests/tools/run_static_fs_gate.py` 会自动做）：
 *     cc -shared -fPIC -O2 -o fs_count_shim.so fs_count_shim.c -ldl
 * 用法：
 *     LD_PRELOAD=./fs_count_shim.so FSCOUNT_OUT=/tmp/x.txt ./test_web_app_static_func
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

/* 类别。**只增不改**：序号会随快照文件一起被读到，插在中间会让老读数错位。 */
enum {
  K_STATX = 0,
  K_NEWFSTATAT,
  K_LSTAT,
  K_STAT,
  K_REALPATH,
  K_READLINK,
  K_READLINKAT,
  K_OPENAT,
  K_OPEN,
  K_CLOSE,
  K_FUTEX,
  K_CSIG,
  K_CWAIT,
  K_MLOCK,
  K_MALLOC,
  K_CALLOC,
  K_REALLOC,
  K_FREE,
  K_NKIND
};

static const char* k_name[K_NKIND] = {
    "statx",      "newfstatat", "lstat",  "stat",   "realpath", "readlink",
    "readlinkat", "openat",     "open",   "close",  "futex",    "cond_signal",
    "cond_wait",  "mutex_lock", "malloc", "calloc", "realloc",  "free",
};

static unsigned long long g_cnt[K_NKIND];

static inline void bump(int k) {
  __atomic_fetch_add(&g_cnt[k], 1, __ATOMIC_RELAXED);
}

/* ---- 进程内读数接口（用例 dlsym 这几个） -------------------------------- */

int uvcpp_fscount_nkinds(void) { return K_NKIND; }

const char* uvcpp_fscount_kind_name(int i) {
  return (i >= 0 && i < K_NKIND) ? k_name[i] : "";
}

int uvcpp_fscount_kind_index(const char* name) {
  if (name == NULL) return -1;
  for (int i = 0; i < K_NKIND; ++i) {
    const char* a = k_name[i];
    const char* b = name;
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*a == '\0' && *b == '\0') return i;
  }
  return -1;
}

/// 把前 n 个类别的累计值写进 out，返回实际写了几格。
int uvcpp_fscount_read(unsigned long long* out, int n) {
  if (out == NULL || n <= 0) return 0;
  const int m = (n < K_NKIND) ? n : K_NKIND;
  for (int i = 0; i < m; ++i) {
    out[i] = __atomic_load_n(&g_cnt[i], __ATOMIC_RELAXED);
  }
  return m;
}

/* ---- 系统调用 ------------------------------------------------------------ */

static long (*real_syscall)(long, ...) = NULL;

long syscall(long n, ...) {
  if (real_syscall == NULL) {
    real_syscall = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
  }
  /* 前 6 个整型参数原样转发；关心的这几个都不带浮点参数。 */
  long a1, a2, a3, a4, a5, a6;
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  a1 = __builtin_va_arg(ap, long);
  a2 = __builtin_va_arg(ap, long);
  a3 = __builtin_va_arg(ap, long);
  a4 = __builtin_va_arg(ap, long);
  a5 = __builtin_va_arg(ap, long);
  a6 = __builtin_va_arg(ap, long);
  __builtin_va_end(ap);

  switch (n) {
    case SYS_statx:      bump(K_STATX); break;
    case SYS_newfstatat: bump(K_NEWFSTATAT); break;
    case SYS_openat:     bump(K_OPENAT); break;
    case SYS_open:       bump(K_OPEN); break;
    case SYS_readlinkat: bump(K_READLINKAT); break;
    case SYS_close:      bump(K_CLOSE); break;
    /* libuv 的线程池同步有走 `uv_futex()` 的一支（它就是
       `syscall(SYS_futex,…)`），所以这一格数得到；glibc 自己的那几个
       拦不到，见文件头。 */
    case SYS_futex:      bump(K_FUTEX); break;
    default: break;
  }
  return real_syscall(n, a1, a2, a3, a4, a5, a6);
}

/* ---- 文件系统包装函数 ---------------------------------------------------- */

static int (*real_lstat64)(const char*, void*) = NULL;
static int (*real_stat64)(const char*, void*) = NULL;
static ssize_t (*real_readlink)(const char*, char*, size_t) = NULL;
static ssize_t (*real_readlinkat)(int, const char*, char*, size_t) = NULL;
static char* (*real_realpath)(const char*, char*) = NULL;

int lstat64(const char* p, void* b) {
  if (real_lstat64 == NULL) {
    real_lstat64 = (int (*)(const char*, void*))dlsym(RTLD_NEXT, "lstat64");
  }
  bump(K_LSTAT);
  return real_lstat64(p, b);
}

/* 现代 glibc 只导出 `lstat`/`stat`（`struct stat` 就是 64 位的），老的发行版
   两个名字都在。两个都定义，谁被调到就记谁。 */
int lstat(const char* p, void* b) {
  if (real_lstat64 == NULL) {
    real_lstat64 = (int (*)(const char*, void*))dlsym(RTLD_NEXT, "lstat64");
  }
  bump(K_LSTAT);
  return real_lstat64(p, b);
}

int stat64(const char* p, void* b) {
  if (real_stat64 == NULL) {
    real_stat64 = (int (*)(const char*, void*))dlsym(RTLD_NEXT, "stat64");
  }
  bump(K_STAT);
  return real_stat64(p, b);
}

int stat(const char* p, void* b) {
  if (real_stat64 == NULL) {
    real_stat64 = (int (*)(const char*, void*))dlsym(RTLD_NEXT, "stat64");
  }
  bump(K_STAT);
  return real_stat64(p, b);
}

ssize_t readlink(const char* p, char* b, size_t n) {
  if (real_readlink == NULL) {
    real_readlink =
        (ssize_t(*)(const char*, char*, size_t))dlsym(RTLD_NEXT, "readlink");
  }
  bump(K_READLINK);
  return real_readlink(p, b, n);
}

ssize_t readlinkat(int dfd, const char* p, char* b, size_t n) {
  if (real_readlinkat == NULL) {
    real_readlinkat = (ssize_t(*)(int, const char*, char*, size_t))dlsym(
        RTLD_NEXT, "readlinkat");
  }
  bump(K_READLINKAT);
  return real_readlinkat(dfd, p, b, n);
}

char* realpath(const char* p, char* out) {
  if (real_realpath == NULL) {
    real_realpath =
        (char* (*)(const char*, char*))dlsym(RTLD_NEXT, "realpath");
  }
  bump(K_REALPATH);
  return real_realpath(p, out);
}

/* ---- 线程原语（"投了几次池"那一格） ------------------------------------- */

static int (*real_csig)(pthread_cond_t*) = NULL;
static int (*real_cwait)(pthread_cond_t*, pthread_mutex_t*) = NULL;
static int (*real_mlock)(pthread_mutex_t*) = NULL;

int pthread_cond_signal(pthread_cond_t* c) {
  if (real_csig == NULL) {
    real_csig =
        (int (*)(pthread_cond_t*))dlsym(RTLD_NEXT, "pthread_cond_signal");
  }
  bump(K_CSIG);
  return real_csig(c);
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
  if (real_cwait == NULL) {
    real_cwait = (int (*)(pthread_cond_t*, pthread_mutex_t*))dlsym(
        RTLD_NEXT, "pthread_cond_wait");
  }
  bump(K_CWAIT);
  return real_cwait(c, m);
}

int pthread_mutex_lock(pthread_mutex_t* m) {
  if (real_mlock == NULL) {
    real_mlock =
        (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_lock");
  }
  bump(K_MLOCK);
  return real_mlock(m);
}

/* ---- 分配 ---------------------------------------------------------------- */

static void account_alloc(void) {
  __atomic_fetch_add(&g_cnt[K_MALLOC], 1, __ATOMIC_RELAXED);
}

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void __libc_free(void*);

void* malloc(size_t n) {
  account_alloc();
  return __libc_malloc(n);
}

void* calloc(size_t a, size_t b) {
  __atomic_fetch_add(&g_cnt[K_CALLOC], 1, __ATOMIC_RELAXED);
  return __libc_calloc(a, b);
}

void* realloc(void* p, size_t n) {
  __atomic_fetch_add(&g_cnt[K_REALLOC], 1, __ATOMIC_RELAXED);
  return __libc_realloc(p, n);
}

void free(void* p) {
  __atomic_fetch_add(&g_cnt[K_FREE], 1, __ATOMIC_RELAXED);
  __libc_free(p);
}

/* ---- 进程外读数（SIGUSR1 → $FSCOUNT_OUT） -------------------------------- */

static const char* g_out = NULL;

/* 只用手写的十进制转换：信号处理函数里 `snprintf`/`fprintf` 都不保证可重入，
   而 `open`/`write`/`close` 都是 async-signal-safe 的。 */
static int fmt_u64(char* dst, unsigned long long v) {
  char tmp[24];
  int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v > 0) {
    tmp[n++] = (char)('0' + (int)(v % 10));
    v /= 10;
  }
  for (int i = 0; i < n; ++i) dst[i] = tmp[n - 1 - i];
  return n;
}

static void snapshot(const char* why) {
  if (g_out == NULL) return;
  const int fd = open(g_out, O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) return;

  char buf[96 * K_NKIND];
  int n = 0;
  /* 一行头 + 每类一行，与 `/tmp/c1` 那套量具的输出同形，好让解析脚本通用。 */
  for (const char* p = "SNAP "; *p != '\0'; ++p) buf[n++] = *p;
  for (const char* p = why; *p != '\0'; ++p) buf[n++] = *p;
  buf[n++] = '\n';
  for (int i = 0; i < K_NKIND; ++i) {
    if (i > 0) buf[n++] = '\n';
    buf[n++] = ' ';
    buf[n++] = ' ';
    for (const char* p = k_name[i]; *p != '\0'; ++p) buf[n++] = *p;
    buf[n++] = ' ';
    n += fmt_u64(buf + n, __atomic_load_n(&g_cnt[i], __ATOMIC_RELAXED));
  }
  buf[n++] = '\n';
  const ssize_t r = write(fd, buf, (size_t)n);
  (void)r;
  close(fd);
}

static void on_usr1(int sig) {
  (void)sig;
  snapshot("usr1");
}

__attribute__((constructor)) static void fsc_init(void) {
  g_out = getenv("FSCOUNT_OUT");
  if (g_out != NULL) signal(SIGUSR1, on_usr1);
}
