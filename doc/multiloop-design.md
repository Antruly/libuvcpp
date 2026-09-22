# 多循环横向扩展设计（`set_loops`）

> **状态：对半分。** **net 层已落地**（`uvcpp_tcp_server::set_loops(n)`，1.2.21-dev，
> 见 §1.1）；**webapp 层仍是设计稿**（`uvcpp_web_app::set_loops()` 还不存在，§4 起是它）。
> 文里的 `src/...:行号` 引用由 `check_doc_lines.py` 锁着 —— 改代码时门禁会把该一起改的地方顶红。

这一篇讲**已经落地了什么**（§1.1）与**打算怎么做**（webapp 那一半），以及
**为什么两端用的是同一条路**（§2 —— 那一节的结论**已被本版推翻**，更正块写在节里）。
姊妹篇是 [多进程横向扩展设计](./worker-process-design.md)：两者正交、可叠加，但**次序不同**
（§2 的更正块说清了次序变成了什么）。

## 1. 形状

一条 setter，位置和 `set_worker_processes(n)` 一样：

```cpp
// doc-snippet: fragment — 目标形状，不是可用代码：`uvcpp_web_app::set_loops()` 还不存在。
// （net 层那个**同名**的 API 已经落地了，形状见 §1.1 —— 两者不是一回事。）
#include <webapp/uvcpp_web_app.h>
using namespace uvcpp;

int main() {
  uvcpp_web_app app;
  app.set_host("0.0.0.0").set_port(8080)
     .set_loops(4);            // 不调用 = 1 = 今天的行为

  app.get("/hello", handler);

  app.start();                 // 起 4 条循环线程，各自 bind+listen
  app.join();                  // join 4 条（今天是 1 条）
  return 0;
}
```

**没有"`main()` 第一条语句"那种纪律** —— 不 fork、不 exec、不动 fd，所以它比多进程便宜得多，
也就该先落地。多进程那份纪律来自 `fork` 的约束（见姊妹篇 §3 末段），这里一概没有。

| | `set_loops(n)` | `set_worker_processes(m)` |
|---|---|---|
| 单位 | 一个进程内的**线程**（每循环一条） | 一个 master 下的**进程** |
| 共享监听 | **不是**：一个接受者 + 转手（两端同一条路，§1.1） | 是（`fork` 继承 fd） |
| 平台支持 | **两端都有**；Windows 默认开，代价是 EMULATE 那一档（§2 末；**不是** `#5282`） | 同样 Windows `m>1` 报错 |
| 叠加 | `set_worker_processes(4)` + `set_loops(4)` = 4 进程 × 4 循环，互不干扰 | 同左 |

**名字已定（维护者）**：`set_loops(n)`。备选 `set_worker_loops(n)`（与
`set_worker_processes` 对仗，但"worker"在那个上下文里已经指子进程，容易混）与
`uvcpp_loop_group` 类型都没有采用。

### 1.1 已经落地的那一半：net 层的 `set_loops(n)`

**已实现**（1.2.21-dev）：`uvcpp_tcp_server::set_loops(n)`（`src/net/uvcpp_tcp_server.h:254`）
把"一条循环"变成「**一条接受者 + n−1 条工作循环**」，每条工作循环一条**专用
`std::thread`**（`uvcpp_loop_worker`，`src/net/uvcpp_loop_worker.h:46` —— 不是
`uv_queue_work` 那条线程池）。配套读法：`loop_count()`
（`src/net/uvcpp_tcp_server.h:280-281`）、`client_count_at(i)`
（`src/net/uvcpp_tcp_server.h:290`）。**不调用它或 `set_loops(1)` 与今天逐字节相同**
（`src/net/uvcpp_tcp_server.h:196` 起是完整的对外说明）。

```
acceptor 循环（0 号 = 今天的 loop_，调用者的线程跑它）
  └─ uv_connection_cb
       ├─ n == 1：今天那条路，逐字不动（src/net/uvcpp_tcp_server.cpp:266）
       └─ n > 1 ：accept 进一个**临时句柄**
                  （src/net/uvcpp_tcp_server.cpp:381 accept_and_handoff）
                  → 取出 socket → 平台转手 → post 给 worker[k]，k = i % (n-1)
                  → 立刻关掉自己那份
worker[k] 线程：loop[k].run(UV_RUN_DEFAULT)
  └─ 邮箱回调里排空（src/net/uvcpp_loop_worker.h:110 起）
       └─ 在**这条循环的线程**上：new uvcpp_tcp_client(worker_loop) 装转手来的 socket
          （src/net/uvcpp_tcp_server.cpp:429 on_handoff_task）→ mark_accepted() → 登记
          → setup_client_callbacks() → TLS 块 → deliver_connection()
          （用户的连接回调在这里被调用，src/net/uvcpp_tcp_server.cpp:452 finish_accept）
```

**去向是显式轮转，不是内核散列**，所以分布是**确定性**的（第 i 条必然落在
`1 + (i % (n-1))`），也因此可以断言"逐循环条数相等"而不必拍均匀度阈值（§6 第 4 条）。

三条硬约束（前两条是 libuv 的，第三条是实测的）：

- **转手必须在 `mark_accepted()` 之前**，也就是整条尾巴（登记、`setup_client_callbacks`、
  TLS、`deliver_connection`）都必须在**目标循环的线程**上跑 —— `enable_tls()` 会当场在这条
  循环上 arm 一次读（`src/net/uvcpp_tcp_server.cpp:485`）。
- **POSIX 的 `uv_accept` 硬断言同循环**（`libuv:unix/stream.c:539`），所以转手靠
  `dup()` + `uv_tcp_open`（后者"已存在"的检查是**按循环**做的，跨循环共用一个 fd 不会被拒、
  只会 double close）。
- **Windows 上接受者必须立刻关掉自己那份副本**：重复句柄的 FIN 只在"最后一个句柄关闭"时
  才发 —— 这不是优化，是这条机制的工作条件。转手原语在
  `src/net/uvcpp_socket_handoff.h:73`（`UV_TCP_REUSEPORT` 那条 POSIX 免转手路**没有**用上，
  降级成以后 Linux 侧的优化，§3）。

**验收状态（如实）**：全量构建 + ctest 97/97 绿 + 页面堆门禁过；用例
`tests/functional/tcp_multiloop_func.cpp` 六条判据。**变异证明**做在两条上：把轮转钉死到
worker 0 ⇒ 分布判据红（`[n=4] 分布 0 0 16`）；绕开转手（让 acceptor 自己收下）⇒
「接受者循环留了 16 条连接」+「接受者线程上跑了连接回调」两条同时红。worker 循环的关闭
（`loop_close` 的 `rc` 与残留句柄数）**由自报读数证**（5 次 `rc=0 alive=0`），因为
`tests/tools/run_loop_leak_probe.py` 自己的锚点过期、跑不起来 —— **"探针没跑"不等于"没泄漏"**，
这条边界要带着看。

> **本机只有 Windows。** 上面那条 POSIX `dup()` 腿在本机**编得到、跑不到**（走的是
> `#if` 的 Windows 分支），证据只能来自 CI 的 ubuntu/macOS 腿。所以本页任何"两端都验过"
> 的写法都是错的，只能写"Windows 本机验过、POSIX 由 CI 验"。


## 2. 平台事实：Windows 上"挪一条监听句柄"是死路

> **更正块（本版推翻的结论）。** 本节原来的结论是「**这一版 `set_loops(n>1)` 在 Windows 上
> 返回错误、拒绝启动**，与多进程同一条政策」，并把 Windows 支排到 `#5282` 之后。
> **维护者拍板反过来**：两端同时上、**Windows 默认开**，理由是"静默退化是唯一危险的失败
> 方式"这条政策在这里被反过来用 —— 拒绝启动等于把 Windows 用户直接挡在多循环门外，
> 而那条已知缺陷的暴露面（见下）小于"多数用户被静默限制在单循环"的代价。
> **下一节起仍成立的是平台事实那一半**（关联一次性、没有 `REUSEPORT`、`SO_REUSEADDR`
> 不是替代品、唯一剩下的形状是"接受者 + 转手"）；**不再成立的是由它推出的取舍**。
> 落地形状见 §1.1，风险与代价见 §2 末与 `doc/net-guide.md` 的多循环一节。

**结论：多循环的 Windows 那一支，和多进程的 Windows 那一支，挡的是同一件东西。**
不是"多循环更简单所以先做"，而是它在 Windows 上**没有第二条路**。

Windows 上一条监听句柄**绑死在一个循环的完成端口上**，而且压着 32 个预投的 AcceptEx
⇒ 想把它挪给别的循环**必被拒**。原因不是"还压着在途 I/O"，而是**关联本身是一次性的**：
一个 socket 对象只能属于一个完成端口，与句柄上有没有在途 I/O、那个端口是否还活着、
完成键是什么**都无关** —— **不是优先级问题，是身份问题**。四种情形（外部贡献者的探针，
本机实测）都返回 `ERROR_INVALID_PARAMETER`（87）：

1. 句柄上**一动 I/O 都没有**时，再关联一次；
2. 有**挂起的 `WSARecv`** 时再关联（这一条与"压着预投 `AcceptEx`"无关，只是同一现象的
   另一种妆扮）；
3. **关联回同一个端口、只换完成键**；
4. **先把旧端口关掉**，再关联新端口。

第 3、4 条各堵死一条退路 —— 文件句柄上"重设完成键"是允许的，**socket 上不是**；
"先把旧循环的端口销毁、再交给新循环"也不行。⇒ 于是"**先取消掉在途的 `AcceptEx`、
再重新关联**"这类设计**根本不存在**：**转手必须在首次关联之前**。剩下的口子也一条条封着：

- `UV_TCP_REUSEPORT` 在 Windows 上被 libuv **无条件拒绝**（`libuv:win/tcp.c:298-299`）；
- libuv 在 Windows **故意不设** `SO_REUSEADDR`（`libuv:win/tcp.c:277-287`，注释写的是它
  "effectively allows 'stealing' a port which is in use by another application"）；
- 实测（本机标准库探针，A–E 矩阵）：两边都设 `SO_REUSEADDR` 时**两个进程能共存**，
  但连接 **100% 归先绑者**（分布 `[8,0]`，把 accept 顺序反过来仍是 `[8,0]`），
  原主一关才全接手 ⇒ **它不是 `SO_REUSEPORT` 的替代品**。

⇒ Windows 上唯一剩下的形状是「**一个接受者 accept + 把连接交给别的循环**」。

**落地的是这一条**（§1.1），而且**默认开着** —— 例外只有 `set_loops(1)`。但它的暴露面
**不是**上游那条缺陷。`WSADuplicateSocketW` 造出来的 socket 在 libuv 眼里是 imported 的，
而 **imported 本身不是闸**：闸是 `UV_HANDLE_SYNC_BYPASS_IOCP`，只在 worker 侧那次
`CreateIoCompletionPort` **成功**时才置（`libuv:win/tcp.c:103-110`、`:117-124`），
而那次能不能成功取决于**源 socket 有没有被关联过**：

- **本库 `set_loops`**：源是 `uv_accept` 出来的（**已关联**）⇒ worker 侧关联失败 `87`
  ⇒ EMULATE=1 / BYPASS=0（实测 `flags=0x8e088`）⇒ `UV_SUCCEEDED_WITHOUT_IOCP`
  （`libuv:win/req-inl.h:69`，只查 BYPASS 这一个位）恒假 ⇒ 上游那一族**没有入口**。
- **master 裸 `accept()` 再 dup**（多进程那条形状，也是外部贡献者台架的形状）：源从未关联
  ⇒ worker 侧关联成功 ⇒ BYPASS=1（实测 `flags=0x6f08c`）⇒ **在那一族上**：装置上
  **87 次死亡 / 2 170 885 次请求**，把那个开关关掉之后是 **3 462 684 条 / 0 / 0**
  （姊妹篇 §9.8；另有**单变量 A/B** 那组的 **55 → 0**，§9.7）。

⇒ **那两组死亡读数是多进程那条血统的，不是 `set_loops` 的**（2026-09-22 按"源 socket
关联过没有"这条判据分臂实测确认，归口见姊妹篇 §9.9）。本版 `set_loops` 的代价换成 EMULATE
那一档：**库侧约 25~27%**（单循环 / 8 连接），端到端台架上只值约 2.5% ——
**两个数量程不同，见 `doc/net-guide.md` §4.2，别单独引任何一个**。
这是**维护者知情后的选择**，不是"没看见"。

**次序不是"多循环能替代多进程"。** 多循环省掉的是 IPC 那一层（管道、ack、master 监督、
重跑 `main()`）；**多进程那一支仍然排在 `#5282` 之后** —— 这次实测把"两条腿共用同一个洞"
这个前提拆掉了：挡住多进程的是那条缺陷，挡住多循环的是 EMULATE 的折扣。

> **边界**："没有入口"是**结构**判断（那一族的闸恒假），不等于"这条路已经验过没别的问题"。
> `#5282` 的机制仍未定。

**上限量过了，"值得开着"这件事有数可依。** 单接受者 + 显式轮转在外部贡献者的机器上实测
（探针写死 149 B 响应、并发 100、非 2xx 请求数 0）：N=1 142 635 / N=2 336 006 /
N=4 469 175 / N=8 492 267，即 **3.29× / 3.45×**，每循环连接数逐轮精确均分、
`dropped = 0`；而**转手本身零代价**（带轮转的 N=1 与单循环的差异落在散布内）。⇒ 多循环
的形状就是"accept + 转手"，**上限就在 3.3~3.5×**，而代价是那条缺陷的暴露面。
（绝对 RPS 只在同一台机器上可比，且**不得**进 §6 第 6 条那个单循环口径。）

## 3. POSIX 那两条免转手的路：以后 Linux 侧的优化（**本版没走**）

> **本版没走这两条。** 落地的 `set_loops(n)` 在**两个平台上是同一条形状**：一个接受者 +
> `dup()`/`WSADuplicateSocketW` 转手（§1.1）。这一节留下的是"**以后可以把 POSIX 那半边
> 的转手省掉**"的候选 —— 它省掉的是转手本身（连同 EMULATE 那一档折扣；`#5282` 不在本版
> 这条路上，见 §2 末），所以 POSIX 侧值钱；
> 前提仍然是本节那条硬约束。

两条路都满足同一条硬约束 —— **接受者就是这个循环自己**，所以完全不碰 libuv 的
`server and client must be on the same loop`（`libuv:docs/src/stream.rst:133-134`；
Unix 侧是硬断言 `libuv:unix/stream.c:539`）。这不是"顺手满足"，而是选这两条路的**理由**：
"单接受者 + 转手"是 Windows 的形状，在 POSIX 上是白付通讯与延迟。

**(a) Linux / FreeBSD / DragonFly / Solaris / AIX：`UV_TCP_REUSEPORT`。**
每个循环自己 `bind` + `listen` 同一个端口，内核按四元组散列。零共享、零转手、不需要额外 fd。

**(b) macOS：`dup()` 监听 fd + 共享 accept 队列。**
macOS 上 `UV_TCP_REUSEPORT` 是 `UV_ENOTSUP`（`UV_TCP_REUSEPORT` 只在 Linux/FreeBSD/
DragonFly/Solaris/AIX 生效）。改法是：监听 fd 建一次，`dup()` n 份，每份在**它自己那个循环**
上 `uv_tcp_open` + `uv_listen`；所有循环阻塞在**同一个 accept 队列**上，内核叫醒一个。
这条路在 libuv 上走得通，姊妹篇 §3 已经核过三处（`libuv:unix/tcp.c:352-365` 的
`uv_tcp_open` 只要求该 fd 不被**同一个** loop 重复持有、`libuv:unix/tcp.c:85-110` 的
`maybe_new_socket` 在已有 fd 时不重建 socket、`libuv:unix/tcp.c:440` 的 `listen()` 对
已在监听的 socket 合法、只更新 backlog）。本库这边的口子是
`src/handle/uvcpp_tcp.cpp:53` 的 `uvcpp_tcp::open`。

> **诚实边界：路 (b) 本机验不了。** 本机是 Windows，`dup()`+共享队列这一支在这里连编都
> 编不到。它必须由 CI 的 macOS 腿带着自己的用例来证；在 CI 给出读数之前，本文里 (b) 只是
> **读源码得出的可行**，不是实测结论。
>
> **核验状态**：本节 §3 的六处 libuv 引文（`REUSEPORT` 被无条件拒、`SO_REUSEADDR` 那段
> 注释、"`uv_tcp_open` 只禁止同一个 loop 重复持有"、"`maybe_new_socket` 在已有 fd 时不
> 重建"、"`listen()` 对已在监听的 socket 只更新 backlog"、Unix 侧 same-loop 硬断言）已由
> 外部贡献者对着本机 `5152db2c` 逐条核过。**但这只核了引文，没有核可行性** —— (b) 仍是
> 读源码得出的可行。

> **路 (b) 有一类要盯的失败形状（Windows 上量到过，机制不同）。** 外部贡献者在本机实测过
> "共享同一个端点的多份句柄、各循环各自去接"这种形状：**8 个循环里 5 个拿到 0 条连接**，
> N=2 时 101 条**全落 loop1**（该档 ≈ 单循环，等于零扩展），换接受顺序或换线程布置只把
> 饥饿挪到别的循环。**机制不同，不做等同** —— Windows 那是 `WSADuplicateSocketW` 出来的
> **两个 socket 对象共用一个端点**，macOS 是 `dup()` 出来的**同一个 socket 对象的多份
> fd**，"内核叫醒一个"在 macOS 上更像有个裁决者。但它落在**同一类**可观测失败上：
> **"谁接到"不由你决定**。

**若 macOS 的分布果然偏，退路是"显式接受者轮转"**：每循环照旧各持自己 `dup()` 的那份 fd，
但**一次只有一个循环在 `accept`**（比如每接 k 条换下一个）。它满足本节那条硬约束
（接受者就是本循环，不碰 libuv 的 same-loop 约束），**不需要任何转手**，所以省 IPC 那个
好处全留着。实现的次序上**先按"所有循环都 accept"做、把分布打出来**，别一上来就上轮转 ——
要不要轮转、k 取多少，等 macOS CI 给出分布读数再定（§6 第 4 条）。

**这两条路都有一条要写进使用文档的性质**：连接落在哪个循环上由**内核散列**决定，
**不保证均匀**，客户数少时可能明显偏 —— 所以真走它们时，"看得见分布"那条判据（§6 第 4 条）
必须跟着上。**反过来，已经落地的转手路没有这个性质**：去向是显式轮转、逐条均分，
分布是确定的（§1.1），这也是 §6 第 4 条今天能用"逐循环条数相等"这种确定性断言的原因。

## 4. 真正的成本：把 per-app 容器切成 per-loop

这一节是这份设计里**唯一有分量的活**，也是它比"多起几个线程"贵的地方。

**判据是"容器是不是 per-object"，不是"状态是不是 per-object"。** `std::map`/`std::list`/
`std::deque` **并发插不同 key 也是 UB** —— 装每请求状态的容器是共享的，那就不算 per-object。
按这个判据扫全仓，闸门是这几张表：

| 容器 | 位置 | 触碰频率 |
|---|---|---|
| `uvcpp_http_server::contexts_` | `src/web/uvcpp_http_server.h:989` | **每请求**（`.cpp` 里 52 处引用） |
| `uvcpp_web_app::inflight_` | `src/webapp/uvcpp_web_app.h:1489` | **每请求** |
| `uvcpp_web_app::upgraded_` | `src/webapp/uvcpp_web_app.h:1433` | 每次 WS 升级 |
| `uvcpp_tcp_server::clients_` | `src/net/uvcpp_tcp_server.h:670` | 接受 / 关闭 / 计数 |

**比上面几条都靠前的一条：`post()` 本身是单循环的。** `src/webapp/uvcpp_web_app.cpp:1842-1862`
里只有一份 `loop_tid_`/`post_queue_`（成员在 `src/webapp/uvcpp_web_app.h:1548-1556`），
投递入口判据是 `loop_tid_ == std::this_thread::get_id()`。而 `post()` 正是"把活儿挪到循环
线程上"的**原语** —— 框架自己到处在用它，**它得先变成 per-loop**，别的一切才好谈。

### 4.1 好消息：大部分切分是"多建几个对象"，不是"给容器加锁"

`uvcpp_http_server` 在自己的构造函数里 `new uvcpp_tcp_server`（`src/web/uvcpp_http_server.cpp:44-46`），
而 `uvcpp_tcp_server` 在自己的构造函数里 `new uvcpp_loop`（`src/net/uvcpp_tcp_server.cpp:79-83`）。
⇒ **n 个 `uvcpp_http_server` 实例 = n 份 `contexts_` + n 份 `clients_` + n 条循环/线程。**

**但"接受者就是这个循环"在那条路上不再成立 —— 这正是 net 层要转手的原因。**
`:266` 的 `new uvcpp_tcp_client(loop_)` 与 `:269` 的 `s->accept(...)` 今天是 **n==1 那条分支**；
n>1 时先把连接 accept 进一个**临时句柄**（`src/net/uvcpp_tcp_server.cpp:381` 的 `accept_and_handoff`），
客户端对象是在**目标工作循环的线程**上建的（`src/net/uvcpp_tcp_server.cpp:429` 的 `on_handoff_task`）。
⇒ webapp 层要接的话，**每个 `uvcpp_http_server` 的 `tcp_server` 各自 `set_loops(n)`**
仍然成立（每个 app 一份循环组），但"接受者即本循环"这条直觉要换成 §1.1 那张图。

于是形状是一句话：

> **一个 app = n 份 per-loop 工作状态 + 一份注册之后冻结的共享表。**

per-loop 那份装：`loop`、循环线程 id、`post_queue_`、`http_`（连同它里面的 `contexts_`
与 `clients_`）、`inflight_`、`upgraded_`、连接登记表、文件传输登记表、`idle_timer_`、
`shutdown_timer_`。

共享冻结的那份装：`router_`（`src/webapp/uvcpp_web_app.h:1203`）、`middlewares_`（`:1204`）、
`ws_router_`/`ws_handlers_`（`:1243-1244`）、`stream_router_`（`:1259`）、`upload_routes_`
（`:1275`）。它们**只在注册期与 `start()` 期间写** —— `chain_storage_` 的全部
写点集中在 `src/webapp/uvcpp_web_app.cpp:2592-2652`，而它就在 `start()` 里。冻结期之后
多线程只读，**不需要锁**。

> **一处例外（2026-09-22 修正）：压缩变体表不属于这一份。** 它早先被列在上面，是错的
> —— `uvcpp_http_server::compress_variants_`（`src/web/uvcpp_http_server.h:1023`）是
> **请求期惰性写**的缓存：命中时改 `last_used` / `compress_variant_clock_` 并计数
> （`src/web/uvcpp_http_server.cpp:1017`），未命中时插入并可能触发 LRU 淘汰
> （`src/web/uvcpp_http_server.cpp:1070`、`src/web/uvcpp_http_server.cpp:872`）。
> 多循环下这些写来自**多条循环线程** ⇒ 它和 `compress_variant_clock_` / `_hits_` /
> `_misses_` / `_stored_`（`src/web/uvcpp_http_server.h:1024-1027`）一起加锁；
> **按循环切不成立** —— 它本来就是跨循环共用的缓存，切了就退回每循环各自 deflate。
>
> **已落地**：`mutable std::mutex compress_mu_`（`src/web/uvcpp_http_server.h:1041`）
> 一把**非递归**锁护住那张表与四个计数。加锁点只有三个**外层入口** —— 命中
> （`src/web/uvcpp_http_server.cpp:1017`）、存入（含淘汰，**同一次临界区**：淘汰那句要读
> 整张表的字节总量，拆成两次加锁会让别的循环插在中间按一个已不成立的总量做决定）、
> `compress_variant_stats()`。两个帮手改名成 `..._locked()`
> （`src/web/uvcpp_http_server.h:1052` / `:1056`），意思就是"调用方已持锁" ——
> 淘汰要算字节总量，所以这两个互相调用，同一条非递归锁不能进两次。
> 临界区里只碰表与计数：命中那条路把共享句柄**拷出锁外**再 `share()`，
> `finish_headers()` 也在锁外（它改的是响应，不碰表）。
>
> 说不改单循环热路径是**假话**：`n == 1` 时这把锁也照加。理由是"只在你真开了多循环
> 时才加的开关"今天根本不存在（webapp 还没有 `set_loops`，见 §5），写一个恒假的分支
> 就是死代码；等 §5 落地时那个开关自然有了，再回来按它分流。当前价格是**一次无竞争
> 的 lock/unlock**，在这条路上（字符串键的 map 查找 + `shared_ptr` 计数）本就量不出来。

### 4.2 一处必须跟着改的判据：连接 id 的"是不是我发的号"

`uvcpp_web_conn_id` 是 `uint64_t`（`src/webapp/uvcpp_web_connection.h:57`），由**登记表自己的**
计数器发（`src/webapp/uvcpp_web_connection.h:333` 的 `next_id_`，
`src/webapp/uvcpp_web_connection.cpp:52` 的 `next_id_++`），而 `issued()` 的判据是

```cpp
// doc-snippet: fragment — 已落地的判据（§4.2 的结论就是 `loop_of(id)` 那半）。
return id != UVCPP_WEB_INVALID_CONN_ID && loop_of(id) == loop_index_ &&
       seq_of(id) < next_id_;   // uvcpp_web_connection.cpp:200-201
```

把登记表切成 n 份之后，**这条判据会对别的循环发的号返回真**。所以不能只是"多建 n 份登记表"：

> **id 的高位编码循环号**，低段仍是该登记表自己的递增号。于是"按 id 找循环"是一次位运算，
> `issued()` 加一条"高段与我相同"即可，`alive()`/`find()` 也不用全 app 搜索。

这一条是**设计里最容易漏的地方**，所以单列。

### 4.3 进程级那族：今天同核、之后变跨核，会变贵

不是闸门（它们本来就是 atomic / 带锁），但值得单独收口，因为**今天它们是同核命中、
n 个循环之后变成跨核缓存行打架**：

- **日志的全局锁最重**：`src/webapp/uvcpp_log.cpp:338-352` 拿 `mutex_` 之后**持有到
  `target->write(record)` 返回** ⇒ **用户的 sink 在全局锁里执行**；默认 console sink 还有
  第二把锁，且锁内做 `std::string` 拼接与 `fwrite`。多循环下这是唯一一处**每条请求都可能**
  撞上的进程级锁。
- 三条 atomic：请求 id（`src/webapp/uvcpp_web_middleware.cpp:63-65`，每请求）、
  reclaim / try_write 计数器（`src/net/uvcpp_tcp_client.cpp:66-68`、`:82-85`）、
  `share_discard_count_`（`src/uvcpp/uvcpp_buf.cpp:216`）。注意后两条的形状会骗人：
  读取口 `reclaim_stats()` / `try_write_stats()` 的签名像实例指标，实际是**全进程聚合**。

**不用动的一条**：`work_limit_`。它按 `UV_THREADPOOL_SIZE × 4`（下限 16，
`src/webapp/uvcpp_web_work_limit.cpp:234-238`）推导，**从来不看循环数**；而那个线程池本来就是
进程级的，n 个循环下 16 仍然是对的。`limit_`/`in_flight_` 已经是 `std::atomic`。

### 4.4 TLS：CTX 共享、SSL 对象不共享

`SSL_CTX` 是 per-app 的，`start()` 里交给每个 tcp_server（`src/webapp/uvcpp_web_app.cpp:1738`）。
注册期建好、之后只读 ⇒ n 个循环共享同一份是对的；但**每个循环的 `SSL` 对象必须是这个循环
自己的**，跨循环复用 SSL 对象是 UB。这条要按"新加 I/O 入口逐个重载核"的老规矩走一遍
（本库已经在 TLS 上漏接过两次）。

**核验状态**：§4 的行号引用与"只在注册期与 `start()` 期间写"这个判断，目前只有本文作者
核过（§4.2 那条"最容易漏"的尤其值得第二次）。§3 那六处引文已由外部贡献者独立核过，
§4 这几张表还没有。**行号那一半在 net 批里被门禁逼着复核过一遍**：`clients_` 那张表的
引用跟着声明从 `std::list` 变成 `std::vector` + 互斥（§4.1 的切分判据因此更该重看 ——
今天它**不是** per-loop，而是**一把锁护一个全局表**）。

## 5. 对外约束

> **net 层已经把这节的四条落进头文件了**（`set_loops()` 的 doc block，
> `src/net/uvcpp_tcp_server.h:196` 起）：`n=1` 逐字节相同、`on_connection` 可能并发、
> `close_all_clients()` 在 `n>1` 时是异步发起、`pause_read()` 只在自己循环的线程上调。
> **面向使用者的版本在 [net 层指南](./net-guide.md) 的多循环那一节**
> （含 Windows 那条风险的完整交代）。下面 §5.2/§5.3 是**webapp 层**还没定的部分。

### 5.1 `set_loops(1)`（或不调用）＝ 今天逐字节相同

不建线程、不碰 fd、不看环境变量、不改变任何现有语义。这条要能**变异证明**：把 n=1 那条
分支改坏（比如强行走多循环初始化），现有用例里必须有一个红。没有这条，"默认没变"只是一句
承诺 —— 与姊妹篇 §5.1 同一条纪律。

**net 层这一半已经做过了**：那条分支被抽成 `finish_accept()` 后由两个调用点共用
（`src/net/uvcpp_tcp_server.cpp:266` 是 n==1、`src/net/uvcpp_tcp_server.cpp:259` 是 n>1），变异"绕开转手"
（`accept_and_handoff` 那条分支改成 `false &&`）让 n=4 档红在"接受者循环留了 16 条连接"
与"接受者线程上跑了连接回调"两条判据上（§1.1）。

### 5.2 `post()` 投给谁

今天只有一条循环，`post()` 的含义没有歧义；n 条之后必须**写死一条**，否则就是一个新的
未定义面。我的建议：**投给发起者所在的那个循环**；从**非循环线程**投递且未指定循环时投给
0 号循环，并把"能不能指定循环"留成一个显式参数（`post(fn, loop_index)`）。
这条是**语义决定，不是实现细节**，需要维护者拍板。

### 5.3 `stop()` / `join()` 从"一条"变成"n 条"

`stop()`（`src/webapp/uvcpp_web_app.cpp:1796`）与 `join()`（`:1813`）今天按一条线程写。
n 条之后：`stop()` 必须**幂等且覆盖全部**，`join()` 必须 join n 条；停机看门狗的推进
（`shutdown_step()`）要按**循环**而不是按 app 记账，否则"某一个循环卡住"会被别的循环的
进展掩盖掉。

**待定（`run()` 的语义）**：`run(uv_run_mode)`（`:1568`）是阻塞式跑循环。n 条循环时它应当
是"在当前线程跑 0 号、其余各起一条线程、全部退出才返回"，还是"`n>1` 时拒绝"，我倾向后者
（阻塞式 API 与多循环本来就不搭，`start()`/`join()` 才是正路），但这是 API 语义，归维护者。
`loop()`（`:1864-1866`，今天返回那唯一一条）同理。

## 6. 判据（验收）

> 标 **[net 已过]** 的是 net 层这一批已经跑出来的；其余是 webapp 层的待验收项。

1. **`n=1` 逐字节相同 + 变异证明**（§5.1）。**[net 已过]**（§1.1 那两条变异）
2. **聚合量仍然是聚合量**：`inflight_count()` 必须对**所有**循环的每个连接求和 —— 头文件里
   那条注释已经写明这一点（`src/webapp/uvcpp_web_app.h:1398-1399`），切分时别改成读某一份。
3. **id 不乱**：n 个循环下 id 全局唯一、`issued()`/`alive()` 对**别的循环**的 id 不误判为真
   （§4.2）—— 这条要专门的用例，因为它今天恒真。
4. **看得见分布**：n 条循环各自的连接数与在途请求数要能被读到。理由是两个方向的失败长得
   一样 —— "只有一个循环真干活"（§2 那种静默退化）和"散列不均"。**分布不是判据，能看见
   分布才是判据**；判"平不平"要按 §3 那条性质（内核散列、少量客户端时会偏）来定，别拍一个
   均匀度阈值。
   **[net 已过，而且比设计时更强]**：`client_count_at(i)` 读得见逐循环条数，而转手是
   **显式轮转**（不是内核散列）⇒ 分布确定、可以断言**逐循环条数相等**，不必退到"没有哪个
   循环拿到 0 条"这种弱判据。**但要避开一个空转档**：逐循环相等这条只在 `nconn` 能被
   `n−1` 整除时才有内容，而 `n=2` 那档 `n−1 = 1` —— 每条连接都归 0 号工作循环，
   于是它**证明不了**"全投 0 号"这种坏实现（变异时确认过：`[n=4] 分布 0 0 16` 是 n=4 那档
   抓到的）。
   **macOS 那条腿（§3 路 (b)）若真走了，仍要那条"能红"的判据**：n 个循环、足够多的连接、
   逐循环计数，断言**没有任何一个循环拿到 0 条**。理由：那一格的失败形状正是**能跑、能响应、
   非 2xx 请求数 0、只有一个循环真干活** —— 与成功逐字相同。
5. **停机**：n 条循环全部退出、没有句柄泄漏（走既有的页面堆门禁）。**[net 已过]**：
   门禁通过，worker 循环的 `loop_close` 由自报读数证（§1.1）。
6. **不进 README 的性能口径。** 15 万 RPS 那个数是**单循环**口径，多循环不得混进去，
   也不得与 hical 的单 acceptor 数并列。扩展性实现之后**单开一节**写，单核那行不动。

## 7. 不做

- **(已撤销)** ~~**Windows 的 `n>1`** —— 排 `libuv/libuv#5282` 之后（§2）。~~
  见 §2 更正块：**两端都做，Windows 默认开**。
- **不走 §3 那两条免转手的 POSIX 路**（`REUSEPORT` / `dup()` 共享 accept 队列）——
  本版两端同一条转手路；那两条留给以后 Linux 侧的优化。
- **不把 `post()` 改成全进程队列** —— 那等于把今天每请求的锁换成一把全局锁，方向反了。
- **不给共享表加锁** —— 冻结之后只读，加锁是白付（§4.1）。
- **不动 `uvcpp_web_work_limit` 的取数**（§4.3）。
- **不动多进程那一套**：worker 重跑 `main()`、`fork` 那条纪律、master 只监督，全部照旧。
- **不改单循环的热路径**：n=1 时不许出现任何多循环的残留（没有残余的 atomic、没有空转的
  表遍历）。

## 8. 与其他文档的关系

- **net 层的用法（已实现，看这篇）**：[net 网络层指南](./net-guide.md) 的多循环那一节 ——
  含 Windows 那条风险的**面向使用者**的完整交代（默认开、读数、代价、怎么关掉）。
- 多进程（正交、可叠加）：[多进程横向扩展设计](./worker-process-design.md) —— 含 §9 那条
  上游缺陷的证据包。
- 平台事实：多进程篇 §6 与本篇 §2 引的是同一批读数。**但 `SO_REUSEADDR` 那组（分布
  `[8,0]`）是在 Windows 上量的，只在 Windows 成立** —— 别拿它去论证 Linux/macOS 的行为；
  §3 路 (a) 的依据是 libuv 的 `REUSEPORT` 支持矩阵与 Linux CI 的读数，不是它。
- 用法（webapp 层实现之后）：[webapp 应用框架开发者指南](./webapp-guide.md)。
