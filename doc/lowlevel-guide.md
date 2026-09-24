# 低层指南：循环、句柄与请求

`src/handle/`、`src/req/`、`src/uvcpp/` 是这一层：libuv 的薄封装。**handle** 是长期挂在
循环上的对象（定时器、信号、流、进程），**req** 是一次性的操作（写、连、DNS、文件）。
`net/`、`web/`、`webapp/` 都建在它上面。

- 打开方式：**没有开关**。这三个目录总是编进库里，不需要任何 `-D`
- 包含方式：`#include <uvcpp.h>` 只给**基类**——`uvcpp_loop`、`uvcpp_handle`、
  `uvcpp_req` 加几个宏头（`src/uvcpp.h:22-24`）。`uvcpp_timer`、`uvcpp_write`、`uvcpp_fs`
  这些**具体类型全都要单独 include**，别以为聚合头把它们带进来了

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一层没做"或者"注释与实现对
> 不上"的地方都明确标出来——那些地方比 API 更容易踩。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [最小可运行程序](#2-最小可运行程序)
3. [循环](#3-循环)
4. [句柄](#4-句柄)
5. [请求](#5-请求)
6. [线程与工作池](#6-线程与工作池)
7. [缓冲区](#7-缓冲区)
8. [错误与异常](#8-错误与异常)
9. [典型坑](#9-典型坑)
10. [没做的（如实列出）](#10-没做的如实列出)

---

## 1. 这一层是什么

| 目录 | 里面是什么 | 一个例子 |
|---|---|---|
| `src/handle/` | 长期对象，挂在循环上产生事件 | `uvcpp_timer`、`uvcpp_signal`、`uvcpp_tcp`、`uvcpp_process` |
| `src/req/` | 一次性操作，提交后回调一次就结束 | `uvcpp_write`、`uvcpp_connect`、`uvcpp_getaddrinfo`、`uvcpp_fs` |
| `src/uvcpp/` | 值类型与工具 | `uvcpp_buf`、`uvcpp_alloc`、`uvcpp_thread`、`uvcpp_metrics` |

**handle 有生命周期**（create → start → stop → close），**req 没有**：`uvcpp_req` 上
根本没有 `start`/`stop`/`close`，每个派生类只有 `init()` 和完成回调。req 的"结束"就是
回调跑完。

`<uvcpp.h>` 只聚合这七个头（`src/uvcpp.h:22-24`）：

```
uvcpp/uvcpp_config.h    ← 构建期生成，不在源码树里
uvcpp/uvcpp_define.h
uvcpp/uvcpp_version.h
uvcpp/uvcpp_export.h
handle/uvcpp_handle.h
handle/uvcpp_loop.h
req/uvcpp_req.h
```

所以它给你循环、两个基类、版本宏，**一个具体 watcher 和具体 req 都不给**。
`net/`、`web/`、`webapp/`、`ssl/`、`http2/`、`expand/` 里的东西一个都不在里面——
那些是各自独立的模块指南的事。

---

## 2. 最小可运行程序

一个重复三次就自己收工的定时器：

```cpp
#include <cstdio>

#include <uvcpp.h>
#include <handle/uvcpp_timer.h>

int main() {
  uvcpp::uvcpp_loop loop;           // 构造函数里已经 init() 过，不用再调
  uvcpp::uvcpp_timer timer(&loop);  // 必须给 loop —— 默认构造的定时器不挂在任何循环上

  int ticks = 0;
  timer.start([&ticks](uvcpp::uvcpp_timer* t) {
    std::printf("tick %d\n", ++ticks);
    if (ticks >= 3) {
      t->stop();    // 只是解除定时，句柄还在
      t->close();   // 真正释放；close 回调在 run() 里跑完
    }
  }, 200, 200);     // 首次 200ms，之后每 200ms

  loop.run();          // UV_RUN_DEFAULT：没有活跃句柄时自己返回
  loop.loop_close();   // 句柄队列已空，这里会成功
  return 0;
}
```

三个从例子里就能看出来的约定：

- **`uvcpp_loop` 的构造函数自己调 `init()`**（`src/handle/uvcpp_loop.cpp:57-63`）。
  再手写一句 `loop.init()` 是多余的。
- **句柄的 loop 是构造参数**，不是默认挂在 `default_loop()` 上。`uvcpp_timer t;` 之后
  必须 `t.init(&loop)` 才可用——**无参的 `init()` 不接循环**，它只把句柄状态清零
  （`src/handle/uvcpp_timer.cpp:17-21`）。唯一的例外还是 `uvcpp_loop`。
- **`stop()` 和 `close()` 是两件事**。定时器 `stop()` 之后句柄仍然占着循环，
  `uv_run` 不会因为没有"活跃"定时器就退出——必须 `close()`。

---

## 3. 循环

`uvcpp_loop`（`src/handle/uvcpp_loop.h:20`）包一个 `uv_loop_t`。它**派生自
`uvcpp_handle`**，但 `uv_loop_t` 在 libuv 里并不是 `uv_handle_t`——继承关系只在包装层。

| 方法 | 签名 | 要点 |
|---|---|---|
| `run` | `int run(uv_run_mode md = UV_RUN_DEFAULT)` | 阻塞跑 |
| `stop` | `void stop()` | 让 `uv_run` 返回 |
| `loop_close` | `int loop_close()` | 幂等；第二次直接返回 0 |
| `loop_alive` | `int loop_alive()` | 还有活跃句柄/请求吗 |
| `is_running` | `bool is_running() const` | 此刻正在 `uv_run` 里面吗 |
| `default_loop` | `static uvcpp_loop* default_loop()` | 进程级单例 |

**`uv_run_mode` 是 libuv 的枚举，不是本库的。** 直接用 `UV_RUN_DEFAULT` /
`UV_RUN_ONCE` / `UV_RUN_NOWAIT`，别写 `uvcpp::uv_run_mode`。

**`run()` 不可重入。** 头里写得很直白（`src/handle/uvcpp_loop.h:40-50`）：库里有几条
析构路径要在收尾时拨几轮循环放掉挂起的关闭回调，那些路径如果是在**本循环自己的回调里**
被调到，就既不能再拨一次、也不能把循环关掉。判据就是 `is_running()`：

```cpp
#include <uvcpp.h>
#include <handle/uvcpp_idle.h>

int main() {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_idle idle(&loop);

  idle.start([&loop](uvcpp::uvcpp_idle* self) {
    // 在回调里：loop.run() 绝对不能调 —— 会重入 uv_run
    if (loop.is_running()) {
      self->close();   // 换个做法：把自己关掉，让外层 uv_run 自然返回
    }
  });

  return loop.run();
}
```

**`loop_close()` 之后那块内存不能碰。** 成功之后 libuv 已经把 `uv_loop_t` 收掉，
debug 构建里还会把整块内存填成 `-1`（`src/handle/uvcpp_loop.h:61-67`）——连
`uv_loop_alive()` 都是读垃圾。包装层有几处保护（`run()` 返回 0、`loop_alive()` 返回 0、
`loop_close()` 返回 0），但别指望它兜住一切。

**`stop()` 可能什么都不做。** 它只在 `loop_alive()` 非零时才调 `uv_stop`
（`src/handle/uvcpp_loop.cpp:159-164`）。没有活跃句柄时循环本来就会自己退出，
所以这不影响正确性——但别把 `stop()` 当成"一定能打断阻塞的 `uv_run`"。

**`default_loop()` 每次调用都重跑 `init()`**（`src/handle/uvcpp_loop.cpp:136-140`），
也就是对同一个 `uv_loop_t` 反复 `uv_loop_init`。libuv 没有定义重复初始化，所以
**不要把它当成可以随便反复调的东西**——拿一次指针存下来用。

---

## 4. 句柄

基类是 `uvcpp_handle`（`src/handle/uvcpp_handle.h:90`）：

```
uvcpp_handle
├── uvcpp_loop                 ← 特殊：uv_loop_t 不是 uv_handle_t
├── uvcpp_timer  uvcpp_idle  uvcpp_prepare  uvcpp_check
├── uvcpp_async  uvcpp_poll  uvcpp_signal
├── uvcpp_fs_event  uvcpp_fs_poll
├── uvcpp_stream
│   ├── uvcpp_tcp
│   └── uvcpp_pipe
├── uvcpp_udp  uvcpp_tty  uvcpp_process
```

基类里 `virtual` 的只有 `get_handle()`、`set_handle()` 和析构；**`close()` 不是虚函数**。
`uvcpp_loop::close()` 是**隐藏**基类那个同名函数，不是覆盖——通过 `uvcpp_handle*` 调
`close()` 会走到基类版本，而两个 `close()` 返回类型还不一样（`void` vs `int`）。

关闭只有两个入口（`src/handle/uvcpp_handle.h:123,127`）：

```cpp
// doc-snippet: fragment — 重载形状的摘录，不是完整翻译单元；这里要展示的是"两个
// close 返回 void 但语义不同"，凑成能编的 TU 反而要编出连调两次 close 的错代码。
void close();                                                     // 不关心收尾
void close(::std::function<void(uvcpp_handle*)> closeCallback);   // 收尾时回调
```

两个都返回 `void`，两个都容忍"句柄已经被释放"（内部有 `_handle == nullptr` 判断）。
但**在关闭回调跑之前连调两次 `close()` 会调两次 `uv_close`**——头里没有防这个，
要防就自己看 `is_closing()`。

**在句柄自己的回调里 `delete` 它是合法的**。库先把 `std::function` 搬出来再调用
（`src/handle/uvcpp_handle.cpp:149-154`），所以回调返回之后它不会再碰 wrapper。
常见写法是挂在 `close()` 的回调上：

```cpp
#include <uvcpp.h>
#include <handle/uvcpp_timer.h>

int main() {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_timer* t = new uvcpp::uvcpp_timer(&loop);

  t->start([](uvcpp::uvcpp_timer* self) {
    self->stop();
    self->close([](uvcpp::uvcpp_handle* h) {
      delete static_cast<uvcpp::uvcpp_timer*>(h);   // 关闭回调里释放，安全
    });
  }, 100, 0);

  loop.run();
  loop.loop_close();
  return 0;
}
```

**`stop()` 的行为每个类都不一样**，这是句柄层最大的坑：

| 类 | `stop()` 干什么 |
|---|---|
| `uvcpp_timer` `uvcpp_check` `uvcpp_prepare` `uvcpp_poll` `uvcpp_signal` `uvcpp_fs_event` `uvcpp_fs_poll` | 只调 `uv_*_stop()`，句柄还能再 `start()` |
| `uvcpp_idle` | **调 `close()`** —— 句柄被消费掉，不能重启（`src/handle/uvcpp_idle.cpp:38-43`） |

而且那七个"只 stop"的实现**都不判空指针**（`src/handle/uvcpp_timer.cpp:36`、`src/handle/uvcpp_check.cpp:37`
等），展开就是 `reinterpret_cast<...>(this->get_handle())` 直接交给 `uv_*_stop`。
libuv 那边会解引用。所以**关闭回调跑完之后再 `stop()` 是空指针解引用**，而同样情况下
`close()` 是安全的。这个不对称头文件里没写。

---

## 5. 请求

`uvcpp_req`（`src/req/uvcpp_req.h:88`）是所有 req 的基类。**回调签名没有统一约定**，
每个类自己的成员：

| 类 | 回调签名 |
|---|---|
| `uvcpp_write` | `void(uvcpp_write*, int status)` |
| `uvcpp_connect` | `void(uvcpp_connect*, int status)` |
| `uvcpp_udp_send` | `void(uvcpp_udp_send*, int status)` |
| `uvcpp_getaddrinfo` | `void(uvcpp_getaddrinfo*, int status, struct addrinfo*)` |
| `uvcpp_getnameinfo` | `void(uvcpp_getnameinfo*, int status, const char* host, const char* service)` |
| `uvcpp_random` | `void(uvcpp_random*, int status, void* buf, size_t buflen)` |
| `uvcpp_fs` | `void(uvcpp_fs*)` —— **status 不是参数**，事后 `get_result()` |
| `uvcpp_work` | 工作线程 `void(uvcpp_work*)`，循环线程 `void(uvcpp_work*, int status)` |

规律是"**先自己，再 status**"，`uvcpp_fs` 是唯一的例外。

**`set_self_free(true)`**（`src/req/uvcpp_req.h:137`）让 req 在完成回调返回后自己
`delete`，默认关。开了之后调用方不能再删。

**为什么能在完成回调里 `delete` 自己**：`invoke_completion()`（`src/req/uvcpp_req.h:174-193`）
**先把闭包从槽位里 move 出来、再清空源槽、然后才调用**。顺序反过来的话，删掉的就是
"此刻正在执行的那个 `std::function`"，连同它的捕获一起——是未定义行为。
`src/req/uvcpp_req.h:178-185` 还专门写了"为什么一定要显式清空源"：libc++ 的小对象
move 不会把源置空（libstdc++/MSVC 会），所以这个 bug 只在 macOS 上显形。

**`uvcpp_random` 在旧 libuv 上不存在**——它整段套在 `#if UV_VERSION_MINOR >= 33` 里。

---

## 6. 线程与工作池

**`uvcpp_work` 的两个回调跑在不同的线程上。** 工作回调在**工作线程池**里，
完成回调（after_work）在**循环线程**里。所以：

- 工作回调里**不能**碰循环、不能碰其他句柄、不能 `delete` 这个 work 对象
- 原因写在工作回调的注释里（`src/req/uvcpp_work.h:51-54`）：它故意**不走
  `invoke_completion`**，因为那个函数会按 `self_free_` 删对象，而对象必须活到 after_work

```cpp
#include <cstdio>

#include <uvcpp.h>
#include <req/uvcpp_work.h>

int main() {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_loop* lp = &loop;
  uvcpp::uvcpp_work* w = new uvcpp::uvcpp_work();

  w->queue_work(&loop,
                [](uvcpp::uvcpp_work*) {
                  // 工作线程：这里干重活。不要碰 loop，不要 delete self。
                },
                [lp](uvcpp::uvcpp_work* self, int status) {
                  // 循环线程：这里可以安全地碰循环和句柄
                  std::printf("status=%d\n", status);
                  delete self;   // 先删，再别再解引用 self
                  lp->stop();
                });

  loop.run();
  loop.loop_close();
  return 0;
}
```

**`queue_work` 是一次性的**：第二次调会抛（`src/req/uvcpp_work.cpp:24-27`），因为
`loop` 已经设过了。

**跨线程唤醒循环用 `uvcpp_async`**。`send()` 就是 `uv_async_send`，头里明说它用于
"从另一个线程给循环发通知"（`src/handle/uvcpp_async.h:34`）。**除此之外这一层没有
任何线程安全承诺**——头文件里没有一句话说 `init`/`start`/`close` 可以从别的线程调。

---

## 7. 缓冲区

`uvcpp_buf`（`src/uvcpp/uvcpp_buf.h:20`）是可增长、可共享的字节缓冲，能和 `uv_buf_t`
互转。和它相关的契约都是**生命周期**：

- **libuv 要求写缓冲活到完成回调**，它不会替你拷一份（`src/req/uvcpp_write.h:36-46`）。
  `uv_write` 会把 `uv_buf_t` **数组本身**拷走，但**不拷数据**（`:122-128`）。
- `append_uv_buf_view()` **必须在 `set_uv_buf()` 之后**调（`src/req/uvcpp_write.h:56`），
  而且第 2 块**只有一个**：再调一次（两种入口混着调也算）会把上一个占用者换掉。
- `append_uv_buf_owned()` 传的必须是 `uvcpp_buf::out_uv_buf()` 返回的**原指针**
  （`:67-70`）——释放用的就是它。别先给 `base` 加偏移再传进来。
- `uvcpp_stream::try_write()` 只是**借用**那次调用的入参，不拷贝不保留
  （`src/handle/uvcpp_stream.h:63`）。

---

## 8. 错误与异常

- **`int` 返回值：0 成功，失败是 libuv 的负错误码**（`UV_EINVAL`、`UV_EALREADY`…）。
  最后一个是粘性的，`get_last_error()` 拿得到。
- **内存不足抛 `std::bad_alloc`**：`src/uvcpp/uvcpp_alloc.h:121-126`、`uvcpp_loop` 的构造
  （`src/handle/uvcpp_loop.cpp:59-60`）、`uvcpp_req` 的拷贝构造与赋值。（`uvcpp_handle`
  的拷贝构造/赋值曾经也在这一列，但它们已删 —— 见 §10。）
- **`queue_work` 抛的是 `const char*`，不是 `std::exception`**（`src/req/uvcpp_work.cpp:24`）。
  单元测试统一的 `catch (const std::exception&)` **接不住它**。
- 这一层除此之外基本不抛异常——错误都走返回值。

---

## 9. 典型坑

**`uvcpp_idle::stop()` 会把句柄关掉。** 它不是"暂停"而是"销毁"：`stop()` 之后不能再
`start()`（`src/handle/uvcpp_idle.cpp:38-43`）。`uvcpp_check` / `uvcpp_prepare` 的
`stop()` 才是真的只停。

**关闭之后再 `stop()` 是空指针解引用。** 那七个 `stop()` 实现都不判空，而 `close()`
判。要"关了之后还能安全再调一次"的场合，用 `close()`。

**无参的 `start()` 在 `uvcpp_idle` / `uvcpp_check` 上会返回 `UV_EINVAL`。**
它把 `nullptr` 当回调传下去，libuv 1.51 的 `uv_idle_start` 对空回调返回 `-22`
（`src/handle/uvcpp_idle.cpp:30`）。要判返回值，不要以为会崩。

**在句柄自己的回调里调 `loop.run()`。** 不可重入，用 `is_running()` 判。

**抓 `std::exception` 抓不住 `queue_work` 的异常**，它是字符串字面量。

**Unix 上构造函数会忽略 `SIGPIPE`。** `uvcpp_loop` 的构造函数在进程内装一次
`SIG_IGN`，但**只在当前处置是 `SIG_DFL` 时才动**（`src/handle/uvcpp_loop.cpp:26`）。
想自己处理 `SIGPIPE` 就要在造循环**之前**装好。

---

## 10. 没做的（如实列出）

- **没有逐成员的 API 参考。** 全仓没有 Doxygen 生成步骤，本页讲的是"典型流程 + 坑"。
- **不支持拷贝句柄，也不支持"克隆" —— 而且是编译期拒绝，不是文档警告。**
  基类 `uvcpp_handle` 与 16 个派生类的拷贝构造 / 拷贝赋值全部是 `= delete`
  （`src/handle/uvcpp_handle.h:94`），`uvcpp_handle::clone()` 与 `uvcpp_req::clone()`
  同样 `= delete`，任一调用都是编译错误。理由是这几个操作**不可能有正确实现**：
  `uv_handle_t` 不是一个值，它是挂在某个 `uv_loop_t` 的 `handle_queue` 上的一个
  节点。`memcpy` 一个**活着的**它，拷出来的那份会带走源的 `loop` 指针与队列邻居，
  却从来没被插进任何队列 —— 于是"那块内存要不要还"怎么答都不对：不还就是纯泄漏
  （原先的实况），还了则会走 `uv_close`，顺着**源句柄邻居**的地址把**还活着的源
  句柄**从它自己的队列上静默摘掉。判据是编译期断言，见 `tests/unit/handle_unit.cpp`。
  `uvcpp_req` 的**拷贝**是另一回事，不受这批影响：它没有所有权标志，拷出来的那块
  在析构里会被 `free_req()` 还掉。
- **线程契约没有成文。** 除了 `uvcpp_async::send()`，头文件里没有任何一句话说哪个
  线程能调哪个函数。实现注释暗示是循环线程，但那是从代码推的，不是承诺。
- **`default_loop()` 的重复 `init()` 行为未定义。** 见 §3。

---

相关文档：[net 网络层指南](./net-guide.md)、[TLS 与证书指南](./ssl-guide.md)、
[项目 README](../README.zh.md)。

本页用到的头文件：`<uvcpp.h>`、`<handle/uvcpp_loop.h>`、`<handle/uvcpp_handle.h>`、
`<handle/uvcpp_timer.h>`、`<handle/uvcpp_idle.h>`、`<handle/uvcpp_async.h>`、
`<handle/uvcpp_stream.h>`、`<req/uvcpp_req.h>`、`<req/uvcpp_work.h>`、
`<req/uvcpp_write.h>`、`<uvcpp/uvcpp_buf.h>`。
