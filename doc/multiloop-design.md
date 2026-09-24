# 多循环横向扩展设计（`set_loops`）

> **状态：两端都已落地。`uvcpp_web_app::set_loops(n)` 在 1.2.23-dev 接上了。**
> net 层是 `uvcpp_tcp_server::set_loops(n)`（1.2.21-dev，见 §1.1）；webapp 层分四批：
> （1.2.22-dev，三批，都还是"n 恒为 1"、行为逐字节不变）**§4.1 的 `contexts_` 按循环切 +
> §4.2 的前置**（`uvcpp_tcp_client::loop_index()`、循环号线程本地、`next_generation_`
> 原子化、`begin_h2_goaway()` 收窄到本循环，见 §4.1 的更正块与 §4.2）；
> **W4 / 2a + 2b**：身份与投递那一族（`loop_slot`）、启动拆成"每进程一次 / 每循环一次"、
> 停机进度按格（§4.1.1 甲/丙）；**W4 / 2c**：四张按 id 索引的表
> （`registry` / `inflight`+`flushing` / `upgraded` / `file_transfers`）进格，
> 聚合读数改成跨格求和（§4.1.1 丙，**并带出一条新的并发边界**）。
>
> **（1.2.23-dev，本批）**：`set_loops(n)` 本体 + 停机**逐槽位扇出**（§4.6 那几条规则）
> + `post(fn, loop_index)` + `run(md)` 对 n>1 返 `UV_EINVAL` + 每格原子计数
> （`registry::live_`、`loop_slot::inflight_entries`）+ §4.1.1 那两张表
> （WS 会话按循环分片、静态缓存加锁）。这一批**第一次让 n>1 端到端可达** ——
> 也因此第一次撞出了 `uvcpp_ws_server::shard_snapshot()` 的空洞解引用
> （见 §4.1.1 底下「重过一遍**新发现的两张表**」那一节）。
>
> **同一批里补的"启动失败"那一段（外部复核提出）**：`set_loops(n>1)` **一返回**，
> n−1 条工作循环就已经在跑了，而 `bind()` 这类步骤还会失败 ⇒ 属主必须能把它们
> 收掉，于是 net 层多了一个公开且**带门**的 `uvcpp_tcp_server::rollback_loops()`
> —— 那道门**只认 `listen()` 成功**（不认"`bind()` 成功"那个状态位：`listen()`
> 失败那条路上它已经是真，拿它当门会反过来挡住本该放行的回滚）。同一笔里，
> 工作循环自己 `init_loop_state()` 失败的返回码**不再被丢掉**（收进 `loop_slot::init_rc`、
> 启动即中止），于是 `n > 1` 的启动失败是**终态**：同实例重试返 `UV_EBUSY`
> （`start_terminal_`），要重试就析构重建。`loop_slot::started` / `finished` 是一对
> **对称的按格 CAS** —— "自增几次就减几次"由结构保证，不再依赖
> "`init_loop_state()` 今天恰好在 `note_loop_started()` 前一步失败"这个偶然事实。
> 回滚那条路**不是靠"故意泄漏"收场的**：net 多了一个与 `set_loop_start_hook()` 对称的
> `set_loop_exit_hook()`，属主在**工作循环自己的退出路径**上把建在那条循环上的句柄收掉
> —— 收在循环内存被释放之前、且在那条循环的线程上，所以既不 UAF、也不让
> `uv_loop_close()` 撞 `UV_EBUSY` 漏掉整块循环内存（§4.6（七）记的是第一版为什么落错）。
>
> **还没做的**（诚实记着，别把它们读成"验过了"）：跨线程读登记表那一条（见
> `uvcpp_web_app.h` 里 `reg_of()` 那段 ★）；`chain_cache_` 在 n>1 下的并发读写
> 仍是 UB，本批**只把它写进契约**（`set_loops()` 的 doc block + 类头契约四），
> 没有加锁也没有分片。
> 文里的 `src/...:行号` 引用由 `check_doc_lines.py` 锁着 —— 改代码时门禁会把该一起改的地方顶红。

这一篇讲**已经落地了什么**（§1.1）与**打算怎么做**（webapp 那一半），以及
**为什么两端用的是同一条路**（§2 —— 那一节的结论**已被本版推翻**，更正块写在节里）。
姊妹篇是 [多进程横向扩展设计](./worker-process-design.md)：两者正交、可叠加，但**次序不同**
（§2 的更正块说清了次序变成了什么）。

## 1. 形状

一条 setter，位置和 `set_worker_processes(n)` 一样：

```cpp
// doc-snippet: fragment — 目标形状：`set_loops()` 已落地（1.2.23-dev），
// 但这里面的 `handler` 是占位符，整段编不过，所以标 fragment 而不是真能编的片段。
#include <webapp/uvcpp_web_app.h>
using namespace uvcpp;

int main() {
  uvcpp_web_app app;
  // **不能链式**：`set_loops()` 返的是错误码（`UV_EINVAL` / `UV_EBUSY`），
  // 与 net 层那个同名 API 同形状。链起来写就没有地方报错了。
  app.set_host("0.0.0.0").set_port(8080);
  int rc = app.set_loops(4);   // 不调用 = 1 = 今天的行为
  if (rc != 0) return rc;

  app.get("/hello", handler);

  app.start();                 // 起 4 条循环线程
  app.join();                  // 等 4 条都退出（有界，见 §4.5）
  return 0;
}
```

**"谁 `bind()` + `listen()`"是平台性质**（§1.1、§3 路 (a)）：Linux 上 n 条循环
**各自**绑一个监听句柄抢同一个端口，内核按四元组分流 —— 上面那句注释写的"起 4 条
循环线程"讲的正是这件事。Windows（以及那个标志被拒时的回落）上没有 `SO_REUSEPORT`，
抢不了同一个端口，才退成**一条接受者 + n−1 条工作循环**、由接受者显式轮转着转手
出去（§2 的更正块）。**两种形状共同的不变量只有一条**："每条连接只落在一条循环上"。

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

**已实现**（1.2.21-dev）：`uvcpp_tcp_server::set_loops(n)`（`src/net/uvcpp_tcp_server.h:268`）
把"一条循环"变成 **n 条各跑自己循环的线程**，`1..n-1` 号各一条**专用 `std::thread`**
（`uvcpp_loop_worker`，`src/net/uvcpp_loop_worker.h:66-66` —— 不是 `uv_queue_work`
那条线程池）。**"新连接怎么落到某一条循环上"分两种形状**，而且是**运行时探测**出来的
（同一个二进制在不同平台上不一样）：Linux 上 n 条各自绑同一端口、内核分流（§3 路 (a)，
**本版已落地**）；Windows 与那个标志被拒时的回落才是下面 §2、§3 讲的"一条接受者 +
显式轮转转手"。想知道这次是哪一种：`is_fanout()`。配套读法：`loop_count()`
（`src/net/uvcpp_tcp_server.h:324`）、`client_count_at(i)`
（`src/net/uvcpp_tcp_server.h:407`）。**不调用它或 `set_loops(1)` 与今天逐字节相同**
（`src/net/uvcpp_tcp_server.h:197` 起是完整的对外说明）。

```
acceptor 循环（0 号 = 今天的 loop_，调用者的线程跑它）
  └─ uv_connection_cb
       ├─ n == 1：今天那条路，逐字不动（src/net/uvcpp_tcp_server.cpp:427）
       ├─ n > 1 且**分流**（Linux）：0 号自己那份就地收下，与 n == 1 同形
       │           （src/net/uvcpp_tcp_server.cpp:417 accept_on_loop）
       └─ n > 1 且**转手**（Windows）：accept 进一个**临时句柄**
                  （src/net/uvcpp_tcp_server.cpp:421 accept_and_handoff）
                  → 取出 socket → 平台转手 → post 给 worker[k]，k = i % (n-1)
                  → 立刻关掉自己那份
worker[k] 线程：loop[k].run(UV_RUN_DEFAULT)
  └─ 邮箱回调里排空（src/net/uvcpp_loop_worker.h:141-141 起）
       └─ 在**这条循环的线程**上：new uvcpp_tcp_client(worker_loop) 装转手来的 socket
          （src/net/uvcpp_tcp_server.cpp:687 on_handoff_task）→ mark_accepted() → 登记
          → setup_client_callbacks() → TLS 块 → deliver_connection()
          （用户的连接回调在这里被调用，src/net/uvcpp_tcp_server.cpp:714 finish_accept）
```

**去向是显式轮转，不是内核散列**，所以分布是**确定性**的（第 i 条必然落在
`1 + (i % (n-1))`），也因此可以断言"逐循环条数相等"而不必拍均匀度阈值（§6 第 4 条）。

三条硬约束（前两条是 libuv 的，第三条是实测的）：

- **转手必须在 `mark_accepted()` 之前**，也就是整条尾巴（登记、`setup_client_callbacks`、
  TLS、`deliver_connection`）都必须在**目标循环的线程**上跑 —— `enable_tls()` 会当场在这条
  循环上 arm 一次读（`src/net/uvcpp_tcp_server.cpp:747`）。
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

> **本机只有 Windows，而这条 POSIX `dup()` 腿在 Windows 上根本"编不到"** —— 它在
> `uvcpp_socket_handoff.cpp:65-75` 的 `#else` 里，本机跑的是同一函数的 Windows 支；macOS 腿按同
> 一份平台清单**也应**回落到它（**没被任何一条腿见证过**）。**09-24 补**：`set_handoff_forced(true)`
> （必须在 `set_loops()` 之前调）让 Linux 上也能把它真跑一遍（判据与读数见 [net 指南](./net-guide.md)
> 的多循环那一节）⇒ "两端都验过"这句**在 Linux 上如今也成立**；Windows 支仍只有本机验，别写反。


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

**上限量过了，"值得开着"这件事有数可依。** 这组读数量的是**转手那个形状**（Windows；
下面标签 `N` = 该形状下的**循环总数** = 1 条接受者 + N−1 条工作循环）：单接受者 + 显式
轮转在外部贡献者的机器上实测（探针写死 149 B 响应、并发 100、非 2xx 请求数 0）：
N=1 142 635 / N=2 336 006 / N=4 469 175 / N=8 492 267，即 **3.29× / 3.45×**，每循环
连接数逐轮精确均分、`dropped = 0`；而**转手本身零代价**（带轮转的 N=1 与单循环的差异
落在散布内）。⇒ **转手这个形状**就是"accept + 转手"，**上限就在 3.3~3.5×**，而代价是
那条缺陷的暴露面。
> **★ 这组标签要重读。** 本版 Linux 侧 `set_loops(n)` 走的是 §3 路 (a)（n 条各自绑
> 端口、内核分流），**不是上面那一列的形状** —— 那组数搬不过来：它量不到内核散列的
> 分布，而分流形状的判据正是"看得见分布"（§6 第 4 条）。分流形状的端到端读数需要
> 单独重量。
（绝对 RPS 只在同一台机器上可比，且**不得**进 §6 第 6 条那个单循环口径。）

## 3. POSIX 那两条免转手的路：**(a) 本版已走，(b) 以后再说**

> **本版走了 (a)，没走 (b)。** 写这一节时的判断是"两条都留到以后"；实现时探明：**(a)
> 不必等平台分叉** —— `UV_TCP_REUSEPORT` 接受与否**在同一个二进制里就能试出来**
> （`bind()` 带标志失败就把同一个句柄重来一次不带标志，回落时 `is_fanout()` 为假），
> 所以它直接落了地（§1.1；Windows 上 libuv 无条件拒那个标志，自然走回落）。**(b)
> （macOS 的 `dup()` + 共享 accept 队列）本版没走**，留在本节作候选。两条省掉的都是
> 转手本身（连同 EMULATE 那一档折扣；`#5282` 不在本版这条路上，见 §2 末），所以 POSIX
> 侧值钱；前提仍然是本节那条硬约束。

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
**不保证均匀**，客户数少时可能明显偏 —— 所以走它们时，"看得见分布"那条判据（§6 第 4 条）
必须跟着上。**(a) 已经落地，所以这条性质现在就在线上**（Linux 侧）。**反过来，转手那条
路没有这个性质**：去向是显式轮转、逐条均分，分布是确定的（§1.1）—— 两条形状的判据
因此不能共用，§6 第 4 条已按 `is_fanout()` 拆成两支。

## 4. 真正的成本：把 per-app 容器切成 per-loop

这一节是这份设计里**唯一有分量的活**，也是它比"多起几个线程"贵的地方。

**判据是"容器是不是 per-object"，不是"状态是不是 per-object"。** `std::map`/`std::list`/
`std::deque` **并发插不同 key 也是 UB** —— 装每请求状态的容器是共享的，那就不算 per-object。
按这个判据扫全仓，闸门是这几张表：

| 容器 | 位置 | 触碰频率 |
|---|---|---|
| `uvcpp_http_server::contexts_` | `src/web/uvcpp_http_server.h:1009` | **每请求**（`.cpp` 里 52 处引用）—— **已切开**（§4.1） |
| ~~`uvcpp_web_app::inflight_`~~ → `loop_slot::inflight` | `src/webapp/uvcpp_web_app.h:1925-1925`（**已切开**，W4 / 2c） | **每请求** |
| ~~`uvcpp_web_app::upgraded_`~~ → `loop_slot::upgraded` | `src/webapp/uvcpp_web_app.h:1957-1957`（**已切开**，W4 / 2c） | 每次 WS 升级 |
| `uvcpp_tcp_server::clients_` | `src/net/uvcpp_tcp_server.h:865` | 接受 / 关闭 / 计数 |

**比上面几条都靠前的一条：`post()` 本身是单循环的。** 它原来只有一份
`loop_tid_`/`post_queue_`，投递入口判据是 `loop_tid_ == std::this_thread::get_id()`。
而 `post()` 正是"把活儿挪到循环线程上"的**原语** —— 框架自己到处在用它，
**它得先变成 per-loop**，别的一切才好谈。

> **已落地（W4 / 2a，2026-09-22）**：这一族成员现在装在 `loop_slot`
> （`src/webapp/uvcpp_web_app.h:1842-2028`）里，`loops_`（`src/webapp/uvcpp_web_app.h:2061-2061`）
> 是"每循环一格"的向量，`slot_here()`（`src/webapp/uvcpp_web_app.cpp:2350-2366`）
> 按**调用线程**取格 —— 认不出是哪条循环的线程时给 0 号（§5.2）。
> `n == 1` 走快路直接回 `loops_[0]`，**一次锁都不多加**，与今天逐字相同。
> `post()` 现在是 `src/webapp/uvcpp_web_app.cpp:2299-2311`，`loop()` 是
> `src/webapp/uvcpp_web_app.cpp:2342-2348`，`on_loop_thread()` 是
> `src/webapp/uvcpp_web_app.cpp:2293-2297`。

### 4.1 好消息：大部分切分是"多建几个对象"，不是"给容器加锁"

`uvcpp_http_server` 在自己的构造函数里 `new uvcpp_tcp_server`（`src/web/uvcpp_http_server.cpp:48-50`），
而 `uvcpp_tcp_server` 在自己的构造函数里 `new uvcpp_loop`（`src/net/uvcpp_tcp_server.cpp:81-85`）。
⇒ **n 个 `uvcpp_http_server` 实例 = n 份 `contexts_` + n 份 `clients_` + n 条循环/线程。**

> **更正（2026-09-22，net 批落地后）：上面这条"多建几个实例"的路没有被采用。**
> 落地的是**一个实例、内部按循环切表**：`contexts_` 从一张
> `std::map<uvcpp_tcp_client*, conn_ctx>` 变成
> `std::vector<std::map<uvcpp_tcp_client*, conn_ctx> >`
> （`src/web/uvcpp_http_server.h:1009`），索引 = `uvcpp_tcp_client::loop_index()`；
> 取表只有三个入口 —— `ctxs_of(client)`（有连接时）、`ctxs_here()`（没有连接、
> 靠线程本地的循环号，`src/net/uvcpp_loop_worker.h` 那对
> `uvcpp_loop_index_of_this_thread()`）、`ctxs_at(i)`；`n == 1` 时
> `ctxs_of()` 直接回 `contexts_[0]`，不读 `client`。
> **理由是 §4.2 那条**：连接 id 由登记表自己发，切成 n 份之后"是不是我发的号"
> 这条判据会对别的循环的号返回真 —— 一个实例内部切表才能让 id 的高位编码循环号
> 这条修法（`e538ea5`）成立。n 个实例那条路要求每个实例自己一套 id 空间，
> 而 id 是**服务全局**的。
> 连带的一处：`next_generation_` 变成 `std::atomic<uint64_t>`（代次号也是全局序列，
> 不按循环切），`listen()` 里在 `tcp_server_->listen()` **之前**
> `contexts_.resize(loop_count())` —— 尺寸定晚了，工作线程一接手连接就会走
> `on_tcp_connection()` → `ctxs_of()`。

> **更正（2026-09-22，外部复核）：上面"worker 线程是在那个调用里放行的"是错的。**
> 放行点是 `set_loops()` **自己的** `w->start()`（`src/net/uvcpp_tcp_server.cpp:557`），
> 比 `uvcpp_tcp_server::listen()` **更早**；`listen()` 里那句 `contexts_.resize()`
> 之所以还赶得上，靠的是另一条事实 —— **监听还没起来 ⇒ 一条连接都进不来**
> （`uvcpp_tcp_server::listen()` 是绑端口 + `uv_listen()` 的地方），**不是**靠
> "放行发生在这里"。
>
> 这个更正有实际后果：按旧说法，只要 resize 在 `listen()` 之前就安全；按事实，
> `contexts_` 必须在 **`set_loops()` 返回之前**就定好尺寸，而"一条连接都进不来"
> 只是把那段时间窗遮住了。次序本身由别的两句锁着：`set_loops()` 在已经在听时
> 返回 `UV_EBUSY`（`src/net/uvcpp_tcp_server.cpp:515`），而 accept 回调分叉看的
> 是 `!workers_.empty()`（`src/net/uvcpp_tcp_server.cpp:413`）—— 两句都预设
> "`set_loops()` 先跑完、线程已经在跑"。

**但"接受者就是这个循环"在那条路上不再成立 —— 这正是 net 层要转手的原因。**
`:266` 的 `new uvcpp_tcp_client(loop_)` 与 `:269` 的 `s->accept(...)` 今天是 **n==1 那条分支**；
n>1 时先把连接 accept 进一个**临时句柄**（`src/net/uvcpp_tcp_server.cpp:639` 的 `accept_and_handoff`），
客户端对象是在**目标工作循环的线程**上建的（`src/net/uvcpp_tcp_server.cpp:687` 的 `on_handoff_task`）。
⇒ webapp 层要接的话，**每个 `uvcpp_http_server` 的 `tcp_server` 各自 `set_loops(n)`**
仍然成立（每个 app 一份循环组），但"接受者即本循环"这条直觉要换成 §1.1 那张图。

于是形状是一句话：

> **一个 app = n 份 per-loop 工作状态 + 一份注册之后冻结的共享表。**

per-loop 那份装：`loop`、循环线程 id、`post_queue_`、两个定时器与停机进度（**这七样
已进 `loop_slot`**，见 §4.1.1 丙）、`http_`（连同它里面的 `contexts_`
与 `clients_`）、`inflight_`、`upgraded_`、连接登记表、文件传输登记表。

共享冻结的那份装：`router_`（`src/webapp/uvcpp_web_app.h:1391-1391`）、`middlewares_`（`src/webapp/uvcpp_web_app.h:1392-1392`）、
`ws_router_`/`ws_handlers_`（`src/webapp/uvcpp_web_app.h:1442-1443`）、`stream_router_`（`src/webapp/uvcpp_web_app.h:1458-1458`）、`upload_routes_`
（`src/webapp/uvcpp_web_app.h:1474-1474`）。冻结期之后多线程只读，**不需要锁**。

> **理由更正（2026-09-22，外部复核）：这条的理由**不是**"写点都在 `start()` 里"。**
> `chain_storage_.push_back`（`src/webapp/uvcpp_web_app.cpp:3219-3219`）与
> `chain_cache_[...]`（`src/webapp/uvcpp_web_app.cpp:3221-3221`）确实在 `start()` 里，但**那两个函数在请求路径上可达**：
> `sync_chains()`（`src/webapp/uvcpp_web_app.cpp:3171-3171`）被 `src/webapp/uvcpp_web_app.cpp:1252-1252`（流式那条路）与 `src/webapp/uvcpp_web_app.cpp:3104-3104`（派发前）调用，
> `build_chain()`（`src/webapp/uvcpp_web_app.cpp:3197-3197`）被 `src/webapp/uvcpp_web_app.cpp:1254-1254` 与 `src/webapp/uvcpp_web_app.cpp:3126-3126` 调用。让这些写**离开**请求路径的是
> 两件事叠起来：① `start()` 之后缓存已热；② `src/webapp/uvcpp_web_app.h:91` 那条
> **成文契约**「**四、注册路由要在 `start()` 之前**」。按"写点在 `start()` 里"写，
> 会被读成"运行中注册也无害" —— 而那正是契约里说的**安全网，不是用法**。
>
> **n>1 之后同一个违反的后果变了，这一条要抄进 `set_loops()` 的 doc block：
> 今天它退化成的正是那张安全网（同一线程重建 + 路由数量校验挡住指针复用），最坏是
> 跑错业务这一类*结果*错；n 条循环之后它变成对 `chain_cache_`（`std::map`）的并发
> `clear`/`find`、对 `chain_storage_`（`std::deque`）的并发 `push_back` ⇒ **UB
> （内存安全）**，不是结果错。**（deque 那条"旧元素地址不变"仍成立：在途请求手里
> 的 `const std::vector<...>*` 不会因 `push_back` 悬垂，形状是对的。）

> **更正二（2026-09-22，外部复核）：上面这句"只在注册期与 `start()` 期间写"对
> `middlewares_` 不成立 —— 它是"每循环一次"，而"每循环一次"本身就是错的。**
> `init_on_loop_thread()`（`src/webapp/uvcpp_web_app.cpp:2151-2151`）是**循环线程的入口**，
> 而访问日志那句 `middlewares_.insert(middlewares_.begin(), web_middleware_access_log())`
> （`src/webapp/uvcpp_web_app.cpp:1876-1876`）与 `++middleware_gen_`
> （`src/webapp/uvcpp_web_app.cpp:1877-1877`）**当天就在这个入口里**（今天已经搬走，见下面的已落地）。
> n>1 时这个函数**每条循环各跑一次** ⇒
> ① 插进 **n 份**访问日志中间件（每请求记 n 次。本库对这一档有读数：`off → info`
> 是 +17~19% rps、每请求 19 → 25 次分配）；
> ② 更要紧的是**竞态** —— `middlewares_` / `middleware_gen_` 在请求路径上被**读**
> （`src/webapp/uvcpp_web_app.cpp:3186-3186`、`src/webapp/uvcpp_web_app.cpp:3210-3212`、`src/webapp/uvcpp_web_app.cpp:3231-3231`/`src/webapp/uvcpp_web_app.cpp:3246-3246`/`src/webapp/uvcpp_web_app.cpp:3265-3265`），而 n 条循环**不同时起跑**：
> A 循环已经在服务（首命中某路由 ⇒ `build_chain()` 读 `middlewares_`）时，B 循环的
> 初始化正在 `insert` ⇒ 对 `std::vector<uvcpp_web_middleware>` 与那个 `size_t` 的
> **未同步读写**。
>
> ⇒ **`init_on_loop_thread()` 要拆成两半**：「**每进程一次**、且必须在放行任何循环
> **之前**做完」（路由 / 中间件 / 日志级别）与「**每循环一次**」（`loop_tid_`、钩子、
> `http_->run()`）。今天这两半在同一个函数里，而那个函数恰好是循环线程入口 ——
> 上面那份 per-loop 清单是**按"在不在 `start()` 里"分的类，那个分类标准是错的**，
> 得按**"每进程一次"还是"每循环一次"**重过一遍。同族里另外几笔（`uvcpp_logger::
> set_level`、`router_.set_auto_options` / `set_head_as_get`）是**同值重写**，值层面
> 无害，但"每进程一份的状态放在每循环一次的初始化里"这个形状本身要一起收口
> —— 哪天各循环各 bind，`bound_port_` / `loop_started_` 就是后写覆盖。

**已落地（2026-09-22）**：这一半已经拆开了 —— 每进程那半是
`init_process_once()`（`src/webapp/uvcpp_web_app.cpp:1775-1775`），每循环那半仍是
`init_on_loop_thread()`（`src/webapp/uvcpp_web_app.cpp:2151-2151`）。两处调用点都按
**先每进程、后每循环**的次序调它们（`src/webapp/uvcpp_web_app.cpp:1593-1659` 与
`src/webapp/uvcpp_web_app.cpp:1734-1773`，理由见 §4.1.1 甲）。上面 ①② 两条后果随之消失：
那两句现在**只跑一次**，而且跑在放行任何工作循环**之前** —— 放行点是 `set_loops()`
自己的 `w->start()`（`src/net/uvcpp_tcp_server.cpp:557`），所以这份配置在放行那
一刻已经冻结。（**不是**"从 `listen()` 里面放行"—— 那处旧说法已在上面的更正块里
改掉，两处引的是同一次外部复核。）

> **上面那条更正本身留着**：它记的是"按**在不在 `start()` 里**分类"这个标准为什么错，
> 而 §4.1.1 就是照它重过一遍的产物。删掉它，下次还会有人按"写点在 `start()` 里"分类。

#### 4.1.1 按「每进程一次 / 每循环一次」重过一遍（2026-09-22）

`uvcpp_web_app` 的**全部成员**逐个过了一遍（判据是"这个状态该有几份"，不是"今天在哪个函数里写"）。
四类。**下面的引用一律写全路径** —— 不是啰嗦：裸 `:NNN` 只在"同一段落里另有一条完整引用"
时才被门禁解析，整张表里一条完整引用都没有的话，**表里每一条引用都会被静默忽略**。

**甲、每进程一份的配置（注册期或启动前写，冻结后只读 ⇒ 必须在放行任何循环之前做完）**

| 成员 | 位置 |
|---|---|
| `router_` | `src/webapp/uvcpp_web_app.h:1391-1391` |
| `middlewares_` + `middleware_gen_` | `src/webapp/uvcpp_web_app.h:1392-1392` / `src/webapp/uvcpp_web_app.h:1759-1759` |
| `ws_router_` / `ws_handlers_` | `src/webapp/uvcpp_web_app.h:1442-1442` / `src/webapp/uvcpp_web_app.h:1443-1443` |
| `stream_router_` / `stream_routes_seen_` | `src/webapp/uvcpp_web_app.h:1458-1458` / `src/webapp/uvcpp_web_app.h:1461-1461` |
| `upload_routes_` | `src/webapp/uvcpp_web_app.h:1474-1474` |
| `upload_dir_` / `upload_dir_real_` / `upload_dir_unsafe_` | `src/webapp/uvcpp_web_app.h:1480-1480` / `src/webapp/uvcpp_web_app.h:1489-1489` / `src/webapp/uvcpp_web_app.h:1500-1500` |
| `static_roots_real_` | `src/webapp/uvcpp_web_app.h:1497-1497` |
| 上传上限族（6 个） | `src/webapp/uvcpp_web_app.h:1520-1525` |
| 连接钩子族（4 个） | `src/webapp/uvcpp_web_app.h:1770-1776` |
| `work_limit_` | `src/webapp/uvcpp_web_app.h:1401-1401` |

**其中 `middlewares_` 原本是这一格里唯一在错误的位置被写的**：写点
（`src/webapp/uvcpp_web_app.cpp:1876-1876` 与 `src/webapp/uvcpp_web_app.cpp:1877-1877`）
当时落在循环线程的入口里，而所有者是**进程**（上面那条更正二）。
**2026-09-22 已落地**：这两句所属的函数现在是 `init_process_once()`
（`src/webapp/uvcpp_web_app.cpp:1775-1775`）—— **行号没动，换的是函数**。甲这一格其余各笔
（`uvcpp_logger::set_level`、`router_.set_auto_options` / `set_head_as_get`）也一并搬了过来。

**乙、每进程一份的可变量**

| 成员 | 位置 | 为什么是进程级 |
|---|---|---|
| `bound_port_` | `src/webapp/uvcpp_web_app.h:2063-2063` | 一个 app 一个监听端口（`bind()` 只调一次）。 |
| `started_once_` | `src/webapp/uvcpp_web_app.h:1791-1791` | "这个 app 起过了"，与几条循环无关。 |
| `stopping_` | `src/webapp/uvcpp_web_app.h:1807-1807` | 它答的是"这次停机请求受理过了没有"（幂等用 `exchange`，`src/webapp/uvcpp_web_app.cpp:2176-2176`）。`shutdown_phase_` 才是"我这条循环走到第几步" —— **两者今天挨着写，很容易被一起搬**。 |
| `running_` / `loop_started_` | `src/webapp/uvcpp_web_app.h:1806-1806` / `src/webapp/uvcpp_web_app.h:1805-1805` | 它们是**聚合量**："有没有任何一条循环在跑"。`loop_started_` 的语义在 W4 里正式改成"**至少还有一条循环在跑**"（由 `std::atomic<int> loops_running_` 记数）。⚠️ **`stop()` 的早退判据不是它**：看的是 `started_once_`（`src/webapp/uvcpp_web_app.cpp:2172-2172`）—— 工作循环在 `init_process_once()` 里就被放行，会在 0 号还没建好自己的 `async` 时就把 `loop_started_` 变真，那时 `stop()` 就在一条没有 `async` 的槽位上干活了。 |
| `thread_` / `thread_started_` / `threading_` | `src/webapp/uvcpp_web_app.h:1779-1781` | 单数 ⇒ 向量。`join()`（`src/webapp/uvcpp_web_app.cpp:2210-2277`）、`start_background()`（`src/webapp/uvcpp_web_app.cpp:1593-1659`）、析构三处都按"一个线程"写。 |

**丙、每循环一份 —— 今天**全部**是单数成员**

前两条（身份与投递）与两个定时器那一族**已搬进 `loop_slot`**
（`src/webapp/uvcpp_web_app.h:1842-2028`，W4 / 2a + 2b 已落地）。注意成员名去掉了
尾下划线 —— 它们在结构体里，不再是类的直接成员：

| 成员 | 位置 |
|---|---|
| `loop_tid` / `tid_known` / `tid_mutex` | `src/webapp/uvcpp_web_app.h:1860-1860` / `src/webapp/uvcpp_web_app.h:1861-1861` / `src/webapp/uvcpp_web_app.h:1859-1859` |
| `async` | `src/webapp/uvcpp_web_app.h:1864-1864` —— **一个 `uv_async` 只能服务它注册的那条循环**，这条最硬 |
| `post_queue` / `post_mutex` | `src/webapp/uvcpp_web_app.h:1866-1866` / `src/webapp/uvcpp_web_app.h:1865-1865` |
| `loop` / `index` | `src/webapp/uvcpp_web_app.h:1856-1856` / `src/webapp/uvcpp_web_app.h:1852-1852` |
| `idle_timer` / `shutdown_timer` | `src/webapp/uvcpp_web_app.h:1875-1875` / `src/webapp/uvcpp_web_app.h:1876-1876` —— `uv_timer` 与 `async` 同一条理由：**循环亲和** |
| `shutdown_deadline_ms` / `shutdown_phase` | `src/webapp/uvcpp_web_app.h:1894-1894` / `src/webapp/uvcpp_web_app.h:1895-1895` |

> **最后两条为什么必须按循环切**（2b 一并搬的理由）：`shutdown_step()` 是**这条
> 循环自己的**看门狗回调，它等的是这条循环上的在途请求。合成一份的话，先推完
> 的那条循环会把还没轮到的那条的进度直接跳过去 —— 那不是"慢一点"，是**少走几拍**。
> `stopping_` 不同：它答的是"这次停机受理过了没有"（幂等），所以它仍留作单数
> （见上面乙表）。

下面这四张表**已搬进 `loop_slot`（W4 / 2c，2026-09-22）**：

| 成员 | 新位置 |
|---|---|
| `inflight` / `flushing` | `src/webapp/uvcpp_web_app.h:1925-1925` / `src/webapp/uvcpp_web_app.h:1953-1953` |
| `upgraded` | `src/webapp/uvcpp_web_app.h:1957-1957` |
| `registry` | `src/webapp/uvcpp_web_app.h:1908-1908` |
| `file_transfers` | `src/webapp/uvcpp_web_app.h:1960-1961` |

**2c 定下的那条钥匙（"按 id 定位"与"按本线程定位"的分工）**：手上是
`uvcpp_tcp_client*`、或者正在**分配** id（`id_of` / `add` / `remove_by_client`），
以及"本循环自己遍历"（`ids`）⇒ **本循环那一份**（`reg_here()` / `slot_here()`）；
手上是**一个 id**（`client` / `find` / `note_*` / `mark_streaming` / `is_streaming` /
`activity_since` / `touch`，连同 `inflight` / `upgraded` / `file_transfers` 的读写）
⇒ **按 id 高段的循环号定位那一格**（`slot_of(id)` / `reg_of(id)`，
`src/webapp/uvcpp_web_app.cpp:2380-2400`）。理由是后者那些入口的调用者**不保证**在
连接自己那条循环的线程上（`uvcpp_web_stream_sink` 持 id 转发、`abort_request()` 可能
从异步处理器线程进来）——用 id 定位之后"哪条循环"由 id 决定，与调用线程无关。
`n == 1` 下所有 id 的高段都是 0 ⇒ 两条路落在同一格，与拆分前逐字节相同。

> **"找哪一份"不是"能不能读"（外部复核 2026-09-22，本批只做了前者）。**
> `slot_of(id)` / `reg_of(id)` 只解决**定位**，不解决**跨线程访问** —— 那一格里的
> `std::map` 仍然不是线程安全的（`uvcpp_web_connection.h` 里那个类自己的 `@warning`），
> 而这一条**拆分前后一样**：拆前也只有同一个 `std::map`、同一条 `@warning`。
>
> **但聚合量把它加重了一格，这是新增的。** `connection_count()`
> （`src/webapp/uvcpp_web_app.cpp:2445-2445`）、`connection_count_at()`
> （`src/webapp/uvcpp_web_app.cpp:2451-2451`）、`inflight_total()`
> （`src/webapp/uvcpp_web_app.cpp:823-823`）现在要**遍历每一格**的表 —— 即使在 0 号
> 循环的线程上调用，也会读到 1 号循环正在改的 map。拆前只有一张表，所以从前不存在
> 这一条。三条公开声明上已经加了 `@warning` 写明边界（本进程只有一条循环、或所有
> 循环都已停下之后才安全）。
>
> **本批**没有**顺手改成"每格一个原子计数"**（net 层 `live_clients_` 就是那个形状）：
> 它现在**没有判据** —— n>1 的用例要等公开 `set_loops(n)` 那一批才存在，现在改就是
> "n==1 等价 ⇒ 变异恒绿"那一格。step 3 落 `set_loops` 时把"每格原子计数"与"循环跑着
> 的时候读聚合量"那条用例**同一笔**落地。
>
> 另一条要一起交代的：`uvcpp_web_app` 今天自己没有 `set_loops()`，但 `tcp_server()`
> 是公开的（`src/webapp/uvcpp_web_app.h:1067-1067`）⇒ `tcp_server()->set_loops(n)`
> 能绕过这一批进多循环，而上面这些边界在那条路上**全部成立**。在 step 3 正式接上
> 之前别那么用。

**丁、一份、但内部已经按循环切好了**：`http_`（`src/webapp/uvcpp_web_app.h:1379-1379`）
—— `contexts_` 已切成 `vector<map>`（§4.1），net 层的 `clients_` 已按 worker 切。

**戊、只读常量，不分类**：`empty_chain_`（`src/webapp/uvcpp_web_app.h:1767-1767`）。

**己、三个"每循环"的访问器**（不是成员，所以上面几张表都装不下，但同一类）：
`on_loop_thread()`（`src/webapp/uvcpp_web_app.cpp:2293-2293`）、`post()`（`src/webapp/uvcpp_web_app.cpp:2299-2299`）、
`loop()`（`src/webapp/uvcpp_web_app.cpp:2342-2342`）—— 三个都在 `uvcpp_web_context_host` 接口上
（`src/webapp/uvcpp_web_context.h:101-142`），今天都用**唯一那条**循环作答。其中
`loop()` 有个不在显然处的调用点：`serve_static()` 的 handler 在**请求时**才要它
（`src/webapp/uvcpp_web_app.cpp:1342-1342` 那行 `st->serve(req, resp, next, self->loop())`，
注释就写着"循环要到请求时才取"）⇒ n>1 之后它必须回答**这条连接所在的那条**循环。

> **答错循环的后果不是"等"，是数据竞争**（外部复核 2026-09-22 更正）。`work->queue_work()`
> 拿的就是这个 `task->loop`（`src/web/uvcpp_static_server.cpp:265-266`），它的 **after-work
> 回调在 `task->loop` 的线程上**跑 `stat_step()`（`src/web/uvcpp_static_server.cpp:272-280`），
> 而 `stat_step()` 里的 `send_response(task->client, resp)` 走的是 `ctxs_of(client)` ——
> **这条连接自己那张表**。于是 `loop()` 答错循环时，是在**别的循环的线程上改这张连接的
> ctx 与它下面的句柄**；同一段代码里那句"worker 线程：只做元数据，不碰 client / server"
> （`src/web/uvcpp_static_server.cpp:267`）能成立，前提正是 `task->loop` == 连接的循环。

`post()` 的"发起者循环"怎么定，见 §5。

##### 重过一遍**新发现的两张表**：§4 那张闸门表漏了它们

按"容器是不是 per-object"这个判据（§4 的判据）扫 `src/web/*.h` + `src/webapp/*.h` 的成员容器，
**除 §4 已列的四条外还有两张**，都在**请求路径上被写**：

| 容器 | 位置 | 写点 | 谁持有 |
|---|---|---|---|
| `uvcpp_ws_sessions::sessions_` / `retired_` | `src/web/uvcpp_ws_sessions.h:193-194` | 每次 WS 接管 `push_back`、每次终结搬进 `retired_`、async 回调里 `delete` | `uvcpp_ws_server::shards_`（`src/web/uvcpp_ws_server.h:272`，按连接的循环号分片） |
| `uvcpp_web_static::Impl` 的 `cache` / `lru` / `bytes` / `hits` / `misses` / `rejected` / `retired` | `src/webapp/uvcpp_web_static.cpp:569-575` 与 `src/webapp/uvcpp_web_static.cpp:596` | 命中改 `lru` 与计数、未命中插入并可能 LRU 淘汰；`retire()` 在 after_work 里 `push_back`（`src/webapp/uvcpp_web_static.cpp:604`）、`drain_retired()` 在 `serve()` 开头 `delete`（`src/webapp/uvcpp_web_static.cpp:610`） | `uvcpp_web_static`，由 `uvcpp_web_app::serve_static()` 造一份（`src/webapp/uvcpp_web_app.cpp:1306-1309`）—— **请求路径上真正活着的那个容器** |
| `uvcpp_static_server::cache_` / `retired_` | `src/web/uvcpp_static_server.h:192-193` | 未命中时 `self->cache_[task->cache_key] = e`（`src/web/uvcpp_static_server.cpp:394`），读在 `src/web/uvcpp_static_server.cpp:302-303`；`retired_` 由 `drain_retired()` 清扫（`src/web/uvcpp_static_server.cpp:194-197`） | `uvcpp_static_server` 自己 —— **公开 API 但本仓零生产调用点**（只有用例构造它：`tests/functional/web_static_server_func.cpp:172`），仍然要修，优先级低于上一行 |

> **上一张表原先写错了（2026-09-22 按源码核正）**：`uvcpp_static_server::cache_` 的持有者
> 被写成「`uvcpp_web_static`（`serve_static()` 一份）」，**这两个是不同的类、互不包含**。
> `serve_static()`（`src/webapp/uvcpp_web_app.cpp:1306-1309`）造的是 `uvcpp_web_static`，
> 它有自己的 `Impl`（定义在 `src/webapp/uvcpp_web_static.cpp:456` —— **在 .cpp 里**，
> 所以动它不动 ABI）；
> 而 `uvcpp_static_server` 那份另算。⇒ 请求路径上真正活着的容器**原先根本没进这张表**，
> 现已补成独立一行。

**`ws_sessions` 那一张比"并发 `vector::push_back`"更重**：它自己还带一个 `drain_async_`
（`src/web/uvcpp_ws_sessions.h:195`），而那个 async 是 `set_loop(http_server_->get_tcp_server()->get_loop())`
装上的（`src/web/uvcpp_ws_server.cpp:233`）—— **原先装的是接受者那条循环**（不管连接落在哪）。
W4 之后改成了**连接自己那条**（`client->get_loop()`），分片用的也是这个键。n>1 之后 WS 连接落在
工作循环上 ⇒ 这一张不能只加锁，要么按循环切、要么把回收挪回连接自己那条循环。

**理由要写准（2026-09-22 按源码核过，原先那句不成立）**：不能说成"在**别的循环的线程**上
`delete` 一个持有那条循环句柄的 `uvcpp_ws_connection` 在 libuv 层面不成立" ——
`notify_retired()`（`src/web/uvcpp_ws_connection.cpp:115-143`）在对象进 `retired_` **之前**
就跑完了：它清零 `close_observer_id_`、摘掉关闭观察者、把 `tcp_` 置空
（`src/web/uvcpp_ws_connection.cpp:133`）；而 `~uvcpp_ws_connection`
（`src/web/uvcpp_ws_connection.cpp:35-48`）在 `tcp_ == nullptr` 时
**不碰任何循环句柄**（只剩清 token 与结算未发的帧，都在本进程内存里）。所以
`drain()` 里那句 `delete`（`src/web/uvcpp_ws_sessions.cpp:123`）**不是**循环亲和的。

真正循环亲和的是另外三组：

1. **打在还活着的会话上的动作**：`close_all()` 的 `live[i]->close(code)`
   （`src/web/uvcpp_ws_sessions.cpp:138`）与 `recycle_all()` 的 `live[i]->terminate()`
   （`src/web/uvcpp_ws_sessions.cpp:151`）—— 那两条会碰 `tcp_`（摘观察者、排一个写）。
   **停机走的正是这条**：`~uvcpp_ws_server`（`src/web/uvcpp_ws_server.cpp:138-139`：逐片
   快照后对每片 `shutdown()`）→ `recycle_all()`。
2. **`uv_ref()` / `uv_unref()` 从别的线程调**（`src/web/uvcpp_ws_sessions.cpp:110` 与
   `src/web/uvcpp_ws_sessions.cpp:131`）—— 它改的是 `loop->active_handles`，
   libuv 层面不是线程安全的。
3. `sessions_` / `retired_`（`src/web/uvcpp_ws_sessions.h:193-194`）是裸 `std::vector`，
   `recycled_`（`src/web/uvcpp_ws_sessions.h:209`）曾是裸 `size_t`，n 条线程同时
   `push_back` 就是数据竞争（UB，不只是"改错线程"）。W4 的修法是**在 server 侧按连接的
   循环号分片**（每一片只归一条循环的线程），而不是给这三样加锁。

⇒ 结论「不能只加锁」仍然成立，但依据是上面这三条。

**落地的形状（1.2.23-dev）与一个只有 n>1 才撞得到的缺陷。** 分片落在
`uvcpp_ws_server` 上（`shards_` + `shards_mu_`，键取 `client->loop_index()`），
两个帮手 `shard_for(int)`（没有就建）与 `shard_if_exists(int)`（**只查不建** ——
聚合量与停机收窄都不许凭空 materialize 出一格）。

建第一条 WS 会话时**当场挂了**（rc=139，cdb 定位到
`uvcpp_ws_sessions::size` 里 `mov rax,[rcx+38h]`、`rcx = 0`，调用者是
`uvcpp_ws_server::session_count()`）。根因是 `shard_for(i)` 的实现
`shards_.resize(i + 1)`：**建 1 号那一格会顺带把 0 号那一格也造出来，留在
`nullptr`**。而多循环下 0 号是接受者、一条连接都不留（§1），所以 0 号那一格
**永远**是 `nullptr` —— `shard_snapshot()` 把它原样返回，第一个读聚合量的人就踩空。

⇒ **`shard_snapshot()` 必须把空洞滤掉**，不能因为"下标连续"就当成"每格都有"。
这一条值得当成模板：**按需 `resize()` 的稀疏表，它的"长度"不等于"元素个数"。**
它是本批唯一一个**确定性崩溃**型缺陷，而且**单循环下永远不可达** ——
正是"没有 `set_loops(n)` 就没法验"这句话最直接的证据。
判据：`tests/functional/web_app_multiloop_func.cpp` 的 WS 那条（n=2）。

`uvcpp_static_server` 那一张形状简单（一个 `std::map` 缓存 + 一个 `retired_` 向量，都只该由持有它的
循环动），但它今天**是 app 一份、被所有循环共用** —— 加锁或按循环切都行，得选一个。

**这两条是这次重过一遍的产出，之前设计稿与外部复核都没提到它们。**

> **一处例外（2026-09-22 修正）：压缩变体表不属于这一份。** 它早先被列在上面，是错的
> —— `uvcpp_http_server::compress_variants_`（`src/web/uvcpp_http_server.h:1092-1092`）是
> **请求期惰性写**的缓存：命中时改 `last_used` / `compress_variant_clock_` 并计数
> （`src/web/uvcpp_http_server.cpp:1130-1130`），未命中时插入并可能触发 LRU 淘汰
> （`src/web/uvcpp_http_server.cpp:1183-1183`、`src/web/uvcpp_http_server.cpp:985-985`）。
> 多循环下这些写来自**多条循环线程** ⇒ 它和 `compress_variant_clock_` / `_hits_` /
> `_misses_` / `_stored_`（`src/web/uvcpp_http_server.h:1093-1096`）一起加锁；
> **按循环切不成立** —— 它本来就是跨循环共用的缓存，切了就退回每循环各自 deflate。
>
> **已落地**：`mutable std::mutex compress_mu_`（`src/web/uvcpp_http_server.h:1110-1110`）
> 一把**非递归**锁护住那张表与四个计数。加锁点只有三个**外层入口** —— 命中
> （`src/web/uvcpp_http_server.cpp:1130-1130`）、存入（含淘汰，**同一次临界区**：淘汰那句要读
> 整张表的字节总量，拆成两次加锁会让别的循环插在中间按一个已不成立的总量做决定）、
> `compress_variant_stats()`。两个帮手改名成 `..._locked()`
> （`src/web/uvcpp_http_server.h:1121-1121` / `src/web/uvcpp_http_server.h:1125-1125`），意思就是"调用方已持锁" ——
> 淘汰要算字节总量，所以这两个互相调用，同一条非递归锁不能进两次。
> 临界区里只碰表与计数：命中那条路把共享句柄**拷出锁外**再 `share()`，
> `finish_headers()` 也在锁外（它改的是响应，不碰表）。
>
> 说不改单循环热路径是**假话**：`n == 1` 时这把锁也照加。写这段话时（1.2.22-dev）的
> 理由是"只在你真开了多循环时才加的开关根本不存在"（webapp 还没有 `set_loops`），
> 写一个恒假的分支就是死代码。
>
> **那个理由在 1.2.23-dev 已经不成立了** —— `set_loops(n)` 落地之后 `n == 1` 是个
> **运行时**判据，"按它分流"随时可写。本批仍然**不加**那个开关，理由换成了成本判断：
> 当前价格只是**一次无竞争的 lock/unlock**，在这条路上（字符串键的 map 查找 +
> `shared_ptr` 计数）本就量不出来，而分叉会让"单循环热路径"与"多循环热路径"
> 从此是两段代码。**回看的触发条件**：等压测靶场量出 `n == 1` 的锁开销可见时再议
> （不是"等有了 set_loops 再议" —— 那一条已经满足了）。

### 4.2 一处必须跟着改的判据：连接 id 的"是不是我发的号"

`uvcpp_web_conn_id` 是 `uint64_t`（`src/webapp/uvcpp_web_connection.h:59`），由**登记表自己的**
计数器发（`src/webapp/uvcpp_web_connection.h:344` 的 `next_id_`，
`src/webapp/uvcpp_web_connection.cpp:53` 的 `next_id_++`），而 `issued()` 的判据是

```cpp
// doc-snippet: fragment — 已落地的判据（§4.2 的结论就是 `loop_of(id)` 那半）。
return id != UVCPP_WEB_INVALID_CONN_ID && loop_of(id) == loop_index_ &&
       seq_of(id) < next_id_;   // uvcpp_web_connection.cpp:206-207
```

把登记表切成 n 份之后，**这条判据会对别的循环发的号返回真**。所以不能只是"多建 n 份登记表"：

> **id 的高位编码循环号**，低段仍是该登记表自己的递增号。于是"按 id 找循环"是一次位运算，
> `issued()` 加一条"高段与我相同"即可，`alive()`/`find()` 也不用全 app 搜索。

这一条是**设计里最容易漏的地方**，所以单列。

> **核验状态（2026-09-22，外部复核）：这一节已经从设计变成落地，而且有判据。**
> 位布局 20/44 在 `src/webapp/uvcpp_web_connection.h:64-82`（`UVCPP_WEB_CONN_LOOP_SHIFT`
> 在 `:80`、`UVCPP_WEB_CONN_SEQ_MASK` 在 `:82`），
> `make_id`（`:175`）/ `loop_of`（`:165`）/ `seq_of`（`:170`）互为逆运算，
> 发号点是 `make_id(loop_index_, next_id_++)`（`src/webapp/uvcpp_web_connection.cpp:53`）；
> `issued()` 上面那段 `doc-snippet` 与代码**逐字一致**。
> **n=1 逐字节不变**也有判据：`loop_index_ == 0` 时 `make_id` 是恒等变换，
> 用例直接断言 `s1 == 1 && s2 == 2`。
> 判据在 `tests/functional/web_app_context_func.cpp:920` 起（`[18d]`）：
> 两条循环低段相同、`r0.issued(ib)` 为假、`r1.issued(ia)` 为假、跨表
> `client`/`alive`/`find` 全空，连 `-1` 与 `1<<20` 的夹回都断言了。
> ⇒ 这一节**不用再审**。

> **剩一个口子，留给下一批（外部复核提的）**：`uvcpp_web_connection_registry(int loop_index = 0)`
> （`src/webapp/uvcpp_web_connection.h:151`）**有默认实参**，而 `loop_slot::registry`
> （`src/webapp/uvcpp_web_app.h:1908-1908`）今天正是默认构造的。n>1 那天，哪条循环忘了传
> 自己的号就会拿到 `loop_index_ == 0` —— 而**越界夹回的目标也是 0**
> （`src/webapp/uvcpp_web_connection.cpp:29`），也就是那个会**撞号**的值：
> 两份表都发 `(0,1) (0,2) …`，于是本节要防的误判**原样回来，而且是静默的**
> （`loop_of(id) == loop_index_` 恰好成立）。**下一批加一条廉价的一致性断言**：
> 每循环初始化时断言 `registry_.loop_index() == 本循环号`（这个读口已经有了）
> —— 比去掉默认实参省事，还能同时盖住"夹回 0"那条路。这条与上面那条
> `init_on_loop_thread()` 拆分一起，是 webapp 那一批的**入口条件**。

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
`src/webapp/uvcpp_web_work_limit.cpp:206-210`）推导，**从来不看循环数**；而那个线程池本来就是
进程级的，n 个循环下 16 仍然是对的。`limit_`/`in_flight_` 已经是 `std::atomic`。

### 4.4 TLS：CTX 共享、SSL 对象不共享

`SSL_CTX` 是 per-app 的，`start()` 里交给每个 tcp_server（`src/webapp/uvcpp_web_app.cpp:1947-1947`）。
注册期建好、之后只读 ⇒ n 个循环共享同一份是对的；但**每个循环的 `SSL` 对象必须是这个循环
自己的**，跨循环复用 SSL 对象是 UB。这条要按"新加 I/O 入口逐个重载核"的老规矩走一遍
（本库已经在 TLS 上漏接过两次）。

**核验状态**：§4 的行号引用与"只在注册期与 `start()` 期间写"这个判断，目前只有本文作者
核过（§4.2 那条"最容易漏"的尤其值得第二次）。§3 那六处引文已由外部贡献者独立核过，
§4 这几张表还没有。**行号那一半在 net 批里被门禁逼着复核过一遍**：`clients_` 那张表的
引用跟着声明从 `std::list` 变成 `std::vector` + 互斥（§4.1 的切分判据因此更该重看 ——
今天它**不是** per-loop，而是**一把锁护一个全局表**）。

### 4.5 取表那一族的纪律：`find()` 跟谁比、`-1` 怎么办（2026-09-22）

`contexts_` 切成 n 张之后，"取表"这一步本身多出两条必须写下来的纪律。两条都不是新 bug，
而是**已经写在树上的形状**在多循环下换了含义。

**（一）`find()` 的结果必须拿这张表自己的 `end()` 比。** 树上有 19 处原来是

```cpp
// doc-snippet: fragment — 改前形态的两句摘录，正文讲的就是这两句的错处；凑成完整
// 翻译单元得先造一个带 ctxs_of()/ctxs_here() 的假类，反而把要看的形状淹了。
auto it = ctxs_of(client).find(client);
if (it == ctxs_here().end()) return;   // 两个容器的 end() 相比
```

`n == 1` 时这两张是同一条表，行为对；但它依赖一条**没写出来的**不变量：当前线程就是这条
连接所属的那条循环。§4.1.1 己 那条（工作项投错循环）与"不在任何循环线程上"恰好破坏它 ——
那时比较的是**两个不同容器**的迭代器（标准上是 UB；`std::map` 的 `end()` 是各自表头节点的
地址，于是"找不到"这条早退**恒不成立**），后面紧跟着的 `it->second` 就解引用了 `end()`。
⇒ 19 处全部改成"表取一次、拿它自己的 `end()` 比"；`src/web/uvcpp_http_server.cpp:1600-1600`
那处 `ctxs_here()` 是**有意的**（`begin_h2_goaway()` 要的就是本循环那张表），不动。

**（二）`ctxs_at(-1)` 在多循环下直接终止，不再夹回 0 号。** `-1` 不是越界，它的含义是
"不在任何循环线程上"（`uvcpp_loop_index_of_this_thread()` 的初值）。单循环下夹回 0 号
与从前逐字相同，那条路原样保留；n > 1 时 0 号只是 n 张里的一张，夹回去会让"漏调了一条循环"
看着正常 —— `begin_h2_goaway()` 的契约是每条循环各调一次（`src/web/uvcpp_http_server.h:207-210`），
夹回 0 号时它只向 0 号循环道别，却返回一个像样的条数。错得比崩安静，所以选崩：libuv 对
API 误用也是直接终止（先例见 `src/handle/uvcpp_handle.cpp:363` 引的那句）。

**这两条都没有新用例**，理由是同一个：要走到它们，得在**不是**这条连接的循环的线程上调
这一族访问器，而树上没有这样的调用点（§4.1.1 己 那条今天也够不着 —— webapp 层还没有
`set_loops`）。按"先证明缺口是真的再补用例"的老规矩，这里记的是**形状与决策**，
不是一条可红的判据。

这一句在 step 3（1.2.23-dev）之后不再成立：`set_loops(n)` 落地后 §4.1.1 己 那条够得着了，
不过§4.5 这两条**仍然**没有专门用例 —— 它们是"访问器被调错线程"的形状，而框架自己的
调用点都在对的线程上。如实记着，别把"没测"写成"验过"。

### 4.6 停机那一族的纪律（2026-09-23）

step 3（`uvcpp_web_app::set_loops(n)`，1.2.23-dev）落地时核出来的七条。前三条是**必须**
这么写，第四条是**推翻了本批计划里原先写的那条规则**，后三条是外部复核之后补的
（（五）(六) 来自 sercebr 在 `5dc9f21` 上的复核，（七）是第一版回滚**落错了**、
被泄漏探针逼出来的）。

**（一）`stop()` 必须逐槽位扇出，不能只投 0 号。** 这是本批的主判据。从前的形状是
`stop()` → `post([this]{ begin_shutdown(); })`，而 `post(fn)` 投给 `slot_here()`，
从**非循环线程**调时 `slot_here()` 答 0 号（全仓那条"不在循环线程上就答 0 号"的回落）
⇒ `begin_shutdown()` 只在接受者那条循环上跑，工作循环**既不进停机状态机、也不会被停**
⇒ 那几格里 `async` 与两个定时器没人删 ⇒ `uv_loop_close` 撞上未关句柄返 `UV_EBUSY`
⇒ **整块循环内存泄漏**（正是 net 层 `uvcpp_loop_worker.cpp:153-154` 与
`uvcpp_tcp_server.cpp:103-119` 记着的那一族）。

落地形状：逐个槽位 `post([this]{ begin_shutdown(); }, i)`，**发起者那条循环就地跑**
（与 n==1 同形，不为多循环多绕一次异步）。"就地"的判据是 `on_loop_thread()` **加**指针
相等 —— 不能只看指针，因为从非循环线程调时 `slot_here()` 也答 0 号那一格，而
`begin_shutdown()` 绝不能在别人的线程上跑。

**（二）"要不要开始停机"必须是**本格**的判据，不能拿全局结果当门。** 这条是本批最隐蔽
的一处，外部复核（sercebr，`0bf090e` 上读出来的）先于落地指出来的：

- 旧门是 `if (!loop_started_.load()) return;`，而收尾那句正是把 `loop_started_` 置假；
- ⇒ 只要**有一格先跑完收尾**，还没进停机的格子再进 `begin_shutdown()` 就**直接返回** ——
  既不道别（WS Close / h2 GOAWAY 都不发）也不收尾。n==1 只有一格，看不见；n>1 是
  **"最快那一格把最慢那一格吃掉"** 的 S 形。

落地的形状不是"每格投一条'收完'给 0 号、0 号数到 n 再做全进程那几件"（那样 0 号要做的事
还是要跨线程触别的循环），而是把两件事拆开：
`loop_slot::finished`（`compare_exchange_strong`，**每格只记一次**）当本格的门，
`loop_started_` 降级成"**至少还有一条循环在跑**"、由 `std::atomic<int> loops_running_`
推出（`note_loop_stopped()` 里 `fetch_sub(1) == 1` 才置假）。于是它不再是门，
只是"全停完了吗"这个结果，S 形不成立。

**（三）逐循环停**自己**那条循环。** 收尾那句原来是 `http_->get_tcp_server()->stop_loop()`
（逐字就是 `loop_->stop()`，那是**接受者**的循环）。n==1 时 `slot.loop` 与它是同一个指针
⇒ 逐字节相同；n>1 时它只停 0 号，工作循环永远回不到 `uv_run` 之外。
`slot_here().loop->stop()` 是它的精确推广，**不需要动 net 层**。

**（四）"每进程动作只在 0 号发"这条规则**不是**普遍成立的 —— 它只对**句柄**成立，
对**连接**恰好反过来。** 这一格是本批里唯一一处"计划与设计稿互相矛盾、最后设计稿
是对的"，值得整张表写下来：

| 动作 | 谁做 | 为什么 |
|---|---|---|
| `tcp->stop()`（关**监听**句柄） | **只在 0 号** | `uvcpp_handle::close()` 是**直接** `uv_close()`，不投递给属主 ⇒ 工作循环调它就是跨循环 `uv_close` |
| `close_sessions_of_loop(slot.index, GOING_AWAY)` | **逐格** | Close 帧要排进**连接自己那条循环**的写队列 |
| `begin_h2_goaway()` | **逐格** | 按 `ctxs_here()` 只翻本循环那张表，契约本来就是"每条循环各调一次" |
| `close_clients_on_loop(slot.loop)` | **逐格** | 循环亲和；**不能**用会自己扇出的 `close_all_clients()`（见下） |
| "开始停机"INFO / "停机完成"INFO | 只在 0 号 | 纯粹是日志：每格各说一轮，读者会以为停机发生了 n 次 |
| "循环 N 即将退出"DEBUG | 逐格（**带号**） | 它正是"逐循环停自己那条循环"唯一的每循环可观测面（§5.1 那条例外） |

两张方向相反的规则各自的原因是同一条：**句柄挂在哪条循环上，就得在哪条循环上动它**。
- **监听句柄**只有一条、挂在 0 号 ⇒ 只有 0 号能动 ⇒ 这是"只在 0 号发"；
- **连接句柄**散布在各条循环上 ⇒ 每条循环自己那份自己动 ⇒ 这是"逐格发"。

原先（本批计划里）把这条写成"`tcp->stop()` 会被调 n 次 —— 它有状态位早退，幂等，无害，
把那条 INFO 用 `slot.index == 0` 收成一条"。**那半句是错的**，核源码后推翻：
`has_status(TCP_SERVER_LISTENING)` 那条早退确实让结果幂等，但它是"**第一个到的人**执行
`uv_close`"，不是"0 号执行" —— 扇出之后第一个到的完全可能是工作循环，那一下就踩线了。
早退挡的是"重复发"，挡不住"发的人是谁"。设计稿 §5 那条 blockquote 早就写对了这一点，
计划里的括号是漏读。**这一处没有确定性红**（与静态缓存那把锁同一类，见上）。

另一边的理由是 §4.1.1 那一批把 `close_all_sessions()` 改成委托给按分片的那条之后，
**从 0 号发只覆盖 0 号那一片** —— 别的循环上的会话再也收不到 Close 帧。那是**丢道别**
（对端只看到连接被断，1006），不是"少发一轮"。所以这一格的门不但不该加，加了就是缺陷。
判据落在 `tests/functional/web_app_multiloop_func.cpp` 的 WS 那条上：n=2、会话在 1 号循环上，
停机后对端必须收到 **1 条** Close 帧、码是 1001。

`close_all_clients()` 顺带记一笔：它**自己就会扇出**（就地关接受者那份 + 把其余
`post` 给各 worker），所以从工作循环上调它是隔着线程动接受者的连接 —— 那是数据竞争，
不只是"多投了几次"。要用按循环的收窄入口。

**（五）`join()` 的等待是**有界**的，所以"扇出没落地"要从日志上判。** `join()` 等各槽位的
`finished`，超时（`shutdown_grace_ms` + 5 s）打一条 ERROR 然后返回。⇒
"`join()` 返回了"**不能**当成"工作循环被停了"的判据：坏实现 8 秒就返回，照样落在用例给的
15 秒里，是一条**恒绿**的断言。有牙的是那条 ERROR
（「join() 已等 N ms，仍有 M 条循环没退出」）—— 用例靠一个日志 sink 抓它。

这条是**先写错、再核出来**的：本条判据一开始就是按"坏实现会挂住"写的（本批计划里也这么
写），核 `join()` 的实现才发现它自己有界。**按错的机理写出来的断言是恒绿的**，这一条
值得当成模板记住：写"确定性红"之前，先去读那条路到底怎么失败。

**（六）"自增几次就减几次"要由**结构**保证，不能靠"唯一失败点恰好在自增前一句"。**
`loops_running_` 的两个增减点今天配对成立，靠的是一个**偶然**事实：`init_loop_state()`
里唯一可能失败的一步（`slot.async->init()`）恰好在 `note_loop_started(index)` 的**前一句**
⇒ "没自增"与"`async == nullptr`"是同一件事，而 `async == nullptr` 的格子**进不了停机
状态机**（它的 `post()` 是丢弃 + WARN，不是就地执行）⇒ 它也不会替自己多减一次。以后谁在
那两行之间加一句**可能失败**的调用，配对就断了：那一格会"没自增却会减"，`fetch_sub(1) == 1`
提前命中 ⇒ `loop_started_` 提前变假 ⇒ 另一条循环迟到的 `begin_shutdown()` 在上面的门那里
早退 —— 正是（二）刚消掉的 S 形，只是换了个门进来。

落地形状：`loop_slot::started` 与 `finished` 一对**对称的按格 CAS**（各只成功一次）——
自增侧"记过就不再自增"、自减侧"没记过就不许减" ⇒ 配对由结构保证，`init_loop_state()` 里
再加几句可能失败的调用也改不动它。**注释说不服后来的人，CAS 说服得了。**
（这一条是外部复核 sercebr 在 `5dc9f21` 上指出来的，与本批同落。）

**（七）属主建在循环上的句柄，由那条循环**自己的**退出钩子收。** 启动失败那条路
（`set_loops` 返非 0 / 每循环钩子初始化失败 / `bind()` / `listen()`）上，n−1 条工作循环
**已经在跑**，而槽位里的 `async` 与两个定时器已经建在它们上面；回滚只能 `rollback_loops()`
（停线程 + 释放循环）。于是这几格句柄掉进两难：在析构里 `delete` 是往一块**已经被释放**的
循环上写 `uv_close`（UAF）；一个都不删则 `uv_loop_close()` 撞 `UV_EBUSY`、**整块循环内存
泄漏**（`tests/tools/run_loop_leak_probe.py` 数的就是这一条；本仓在这一族上吃过一次大的，
占过全部泄漏循环的 57%）。

第一版落的是"**故意泄漏**"（把那几格标 `orphaned`、析构时跳过），理由是"泄漏好过 UAF"。
那条取舍本身没错 —— 错在把它当成**唯一**的出路：泄漏探针当场把这条路的账报了出来
（`test_web_app_multiloop_func 泄漏 1`，正好是那一格的三件套：`async` + 两个 timer），
而干净的路一直存在：**在句柄自己那条循环的线程上、循环内存还在的时候收** —— 那正是 net
工作线程退出路径里 `on_exit` 那一段（`uv_run` 已经返回、挂起的关闭回调还没泵完）。

落地形状：net 加 `set_loop_exit_hook(fn)`，与 `set_loop_start_hook()` 对称（同样只对
1..n-1 号调、同样必须在 `set_loops()` 之前装）；net 自己那份收尾（关本循环的连接）**先跑**，
属主的钩子后跑，顺序是语义的一部分。webapp 把 `finish_shutdown()` 里收句柄的那段尾巴抽成
`release_loop_handles()`，**正常停机与循环退出共用**（靠"删完置空"幂等，第二次进来看到的
是空表）。`teardown_failed_start()` 于是不再需要标 `orphaned` —— 那个标记只剩 `join()` 超时
那条路在用（那条路上循环**没走完**退出路径，钩子不会响，句柄只能真的留着）。

**不做"投一条任务过去、等它关完再回滚"**：那要在 `start()` 里等一个每格的闩，而等待是这条
路径上唯一能变成挂死的地方（一条 worker 卡住，`start()` 就跟着不回）。退出钩子不需要
**额外**的等待 —— 那份等待已经在 `stop_workers()` 的 join 里付过了。

## 5. 对外约束

> **net 层已经把这节的四条落进头文件了**（`set_loops()` 的 doc block，
> `src/net/uvcpp_tcp_server.h:198` 起）：`n=1` 逐字节相同、`on_connection` 可能并发、
> `close_all_clients()` 在 `n>1` 时是异步发起、`pause_read()` 只在自己循环的线程上调。
> **面向使用者的版本在 [net 层指南](./net-guide.md) 的多循环那一节**
> （含 Windows 那条风险的完整交代）。下面 §5.2/§5.3 是**webapp 层**还没定的部分。
>
> **webapp 层已经定了的一条（2026-09-22，外部复核）**：`uvcpp_web_app.h:91` 那条
> 「注册路由要在 `start()` 之前」的契约，在 `n>1` 之后从"打乱链缓存（安全网兜底、
> 最坏是跑错业务）"**升级成对 `chain_cache_` / `chain_storage_` 的并发读写 ⇒ UB**。
> 这条要写进 `uvcpp_web_app::set_loops()` 的 doc block（理由与出处见 §4.1 的理由更正块）。
>
> **停机那一族另有一条硬约束（2026-09-22，外部复核 §1）**：`begin_h2_goaway()` 只翻
> **本循环**那张表（`src/web/uvcpp_http_server.cpp:1600-1600` 的 `ctxs_here()`），而
> `ctxs_at(-1)` 在 `contexts_.size() > 1` 且调用者不在任何循环线程上时直接
> `std::abort()`（`src/web/uvcpp_http_server.cpp:125-130`）⇒ **停机那一族必须"每条
> 循环各跑一次、且每次都在该循环自己的线程上"**。顺着这条往回看，今天的 `stop()`
> （`src/webapp/uvcpp_web_app.cpp:2165-2165`）在 n>1 下是坏的：非循环线程进来时它走
> `post(...)`，而 `post()` 的落点是 0 号（§5.2）⇒ 1..n−1 的停机状态机永远不会被推进，
> 那些循环上的 WS/h2 道别与连接关闭也没人做。**step 3 的 `stop()` 要按循环扇出。**
>
> 扇出时还有一处必须**只在 0 号**发：`begin_shutdown()` 里那句 `tcp->stop()`
> （`src/webapp/uvcpp_web_app.cpp:3359-3359`）关的是**接受者的 listener 句柄**
> （`uvcpp_tcp_server::stop()` 里的 `tcp_->close(...)`）——扇出之后"谁第一个进来谁执行
> 那句 close"，而第一个进来的可能是工作循环 ⇒ **跨循环 `uv_close`**（libuv 里那不是
> 线程安全的）。它本身是**真幂等**（早退判据 `!has_status(TCP_SERVER_LISTENING)`
> 在 `src/net/uvcpp_tcp_server.cpp:870`，而第一次进来就在
> `src/net/uvcpp_tcp_server.cpp:876` `clear_status`），
> 所以"只让 0 号发"与"每条循环都发"在结果上等价，但前者才是对的形状。
>
> **这一段在 1.2.23-dev 已落地，而且它是本批唯一一处"计划写错、设计稿写对"的地方**
> —— 计划里把它记成"`tcp->stop()` 会被调 n 次，幂等，无害，把那句 INFO 收成一条"，
> 那半句是漏读了上面这个跨循环论证（详见 §4.6（四）那张表）。落地形状是
> `if (tcp != nullptr && slot.index == 0) tcp->stop();`。
### 5.1 `set_loops(1)`（或不调用）＝ 今天逐字节相同

不建线程、不碰 fd、不看环境变量、不改变任何现有语义。这条要能**变异证明**：把 n=1 那条
分支改坏（比如强行走多循环初始化），现有用例里必须有一个红。没有这条，"默认没变"只是一句
承诺 —— 与姊妹篇 §5.1 同一条纪律。

**net 层这一半已经做过了**：连接回调那段收尾被抽成 `finish_accept()`，由
`accept_on_loop()` 共用 —— `src/net/uvcpp_tcp_server.cpp:427`（n==1）与
`src/net/uvcpp_tcp_server.cpp:417`（分流时 0 号自己那一条）走的是**同一个**调用；转手那条
是 `src/net/uvcpp_tcp_server.cpp:421` 的 `accept_and_handoff()`。变异"绕开转手"
（`accept_and_handoff` 那条分支改成 `false &&`）让 n=4 档红在"接受者循环留了 16 条连接"
与"接受者线程上跑了连接回调"两条判据上（§1.1）。

> **★ 那条变异只在转手形状上有牙（2026-09-24）。** Linux 侧 `set_loops(n)` 现在走内核
> 分流（`is_fanout()`），`accept_and_handoff()` 那条分支**根本不会被走到** ⇒ 把它改成
> `false &&` 在 Linux 上是一条**空转变异**（判据不会红，而门禁照样绿 —— 是 ①"最坏的一种
> 绿"）。分流形状的分辨力改由**监听 socket 普查**负责（§6 第 4 条）；
> `tests/tools/multiloop_mutation.py` 里这批变异的**适用形状**要一并复核。

**webapp 层这一半（1.2.23-dev）有一处例外，必须写明白：DEBUG 日志的文本不再是逐字节相同。**

`finish_shutdown()` 收尾处新加了一句**无条件**的
`UVCPP_LOG_DEBUG "停机完成：循环 " << slot.index << " 即将退出"`。n==1 时 `slot.index` 恒为 0，
于是默认档（`log_level` 高于 DEBUG）下**一个字节都不变**，但把 `set_level(log_level::DEBUG)`
打开就会多出这一行。原有那句 INFO（"停机完成，事件循环即将退出"）保持原样、且只由 0 号发，
所以 INFO 及以上完全没动。

**为什么不去掉那句 DEBUG**：它正是"逐循环停自己那条循环"这条改动**唯一**的每循环可观测面
—— 多循环下你没法从别的地方看出"1 号循环走到了收尾"。为了保住"逐字节相同"这句承诺而删掉它，
等于把(三)那条规则搞成不可观测。所以这一处是**明知而写下的例外**，不是疏漏。

**行为面仍然逐字节相同**：不加线程、不碰 fd、不看环境变量、句柄的创建/关闭次序不变、
响应字节不变、`stop()`/`join()` 的返回值与耗时不变。§5.1 那句承诺改读成
"**行为与默认档日志逐字节相同**"。

### 5.2 `post()` 投给谁

今天只有一条循环，`post()` 的含义没有歧义；n 条之后必须**写死一条**，否则就是一个新的
未定义面。**已定并落地（1.2.23-dev）**，就是本节原先的建议：

- `post(fn)` = **投给发起者所在的那个循环**；从**非循环线程**投递时投给 0 号
  （`slot_here()` 那条"不在循环线程上就答 0 号"的回落）。
- 新增 `post(fn, loop_index)`（`src/webapp/uvcpp_web_app.h:1175-1175`）显式指定。

**"发起者循环"这个默认值在停机那条路上是陷阱**：`stop()` 从非循环线程进来时它会投给
0 号 —— 于是工作循环既不进停机状态机也不会被停。这就是 §4.6（一）那条，`stop()` 改成
逐槽位扇出之后才对。

### 5.3 `stop()` / `join()` 从"一条"变成"n 条"

**已落地（1.2.23-dev）**，形状与本节原先的判断一致，另有两处落地时才定下来的细节：

- `stop()` **幂等且覆盖全部**：逐槽位扇出 `begin_shutdown()`，发起者那条循环**就地跑**。
- `join()` join n 条，但**等待是有界的**：预算 = `shutdown_grace_ms`（默认 3000）+
  `kJoinSlackMs`（5000），超时打一条 ERROR（「join() 已等 N ms，仍有 M 条循环没退出」）
  然后返回。**这条有界是判据设计的关键**：它让"扇出没落地"这条缺陷的观测面从
  "挂住"变成"一条日志"，用例（和变异脚本里的 M1）抓的是那条日志，不是墙钟 ——
  详见 §4.6（五）。
- 停机看门狗的推进（`shutdown_step()`）按**循环**记账（每槽位自己的
  `shutdown_deadline_ms` / `shutdown_timer` / `finished`），不是按 app —— 与本节
  原先担心的"某一个循环卡住会被别的循环的进展掩盖掉"正是同一条。

**`run()` 的语义定了**（本节原先倾向的那个）：`run(uv_run_mode)` 在 **n>1 时返
`UV_EINVAL`** —— 它是在调用者线程上就地跑 0 号循环，那 n−1 条工作循环的归属不成立。
多循环只走 `start()` + `join()`。
`loop()`（`src/webapp/uvcpp_web_app.cpp:2342-2348`）**跟着改了**，而且是必须改的那一个：
它现在答**本线程那条循环**（`slot_here().loop`），不是"唯一那条"。§4.1.1 己 那条依赖
就是它 —— `serve_static()` 的 handler 在**请求时**才取 `loop()`（`src/webapp/uvcpp_web_app.cpp:1342-1342`），
而那个值会被 `queue_work()` 当成 `task->loop`，决定 after-work 回调在哪条线程上改这张
连接的 ctx。答错循环就是**数据竞争**（§4.1.1 己 那条 blockquote），不是"等"。
不在循环线程上时 `slot_here()` 给 0 号 —— 与单循环逐字相同。

## 6. 判据（验收）

> 标 **[net 已过]** 的是 net 层这一批已经跑出来的；标 **[已过]** 的是 webapp 层
> 1.2.23-dev 这一批跑出来的。用例在 `tests/functional/web_app_multiloop_func.cpp`，
> 变异脚本在 `tests/tools/multiloop_mutation.py`（M1..M6 + M0 对照组）。

1. **`n=1` 逐字节相同 + 变异证明**（§5.1）。**[net 已过]**（§1.1 那两条变异）
   **[webapp 已过，带一处明文例外]**：n==1 的对照组在
   `test_control_group_n1`；例外是 DEBUG 档多一行带循环号的日志（§5.1 那段）。
2. **聚合量仍然是聚合量**：`inflight_count()` 必须对**所有**循环的每个连接求和 —— 头文件里
   那条注释已经写明这一点（`src/webapp/uvcpp_web_app.h:1120-1120`），切分时别改成读某一份。
   **[2c 已做]**：求和那一半在 `inflight_total()`（`src/webapp/uvcpp_web_app.cpp:823-823`）
   与 `connection_count()`（`src/webapp/uvcpp_web_app.cpp:2445-2445`）里。
   **求和那条路带来的新边界已经收口（1.2.23-dev）**：每格改成**原子计数**
   （`uvcpp_web_connection_registry::live_`、`loop_slot::inflight_entries`），所以聚合量
   可以**在循环跑着的时候**从任何线程读，不再是遍历 n 个 `std::map`。
   判据 = `test_aggregates_while_running`（n=2、8 个请求在途时从主线程读），
   变异 M4（`connection_count_at(i)` 不看下标）在它上面红 ——
   红点是「逐格之和 == `connection_count()`（0+0+0 vs 1）」。
3. **id 不乱**：n 个循环下 id 全局唯一、`issued()`/`alive()` 对**别的循环**的 id 不误判为真
   （§4.2）—— 这条要专门的用例，因为它今天恒真。
   **[已过]**：`test_loop_identity_and_ids`（n=3）与 `test_id_high_bits_from_loop`。
   前者的判据从"handler 自报循环号"改成了**线程身份表** —— 因为
   `uvcpp_loop_index_of_this_thread()` 那条路在 webapp 侧没有导出（链接期就挂了），
   现在用 `ask_loop_threads()` 往每格投一个"报出你的 thread::id"的任务建表，再断言
   三格的线程两两不同、`conn_tid[idx] == slot_tid[idx]`。变异 M3（WS 分片键钉成 0 号）
   在 WS 那条上红。
4. **看得见分布**：n 条循环各自的连接数与在途请求数要能被读到。理由是两个方向的失败长得
   一样 —— "只有一个循环真干活"（§2 那种静默退化）和"散列不均"。**分布不是判据，能看见
   分布才是判据**；判"平不平"要按 §3 那条性质（内核散列、少量客户端时会偏）来定，别拍一个
   均匀度阈值。
   **[net 已过，而且比设计时更强]**：`client_count_at(i)` 读得见逐循环条数，**按形状分
   两支**（`is_fanout()`，2026-09-24）：
   - **转手**那条路上去向是**显式轮转**（不是内核散列）⇒ 分布确定、可以断言**逐循环条数
     相等**，不必退到"没有哪个循环拿到 0 条"这种弱判据。**但要避开一个空转档**：逐循环
     相等这条只在 `nconn` 能被 `n−1` 整除时才有内容，而 `n=2` 那档 `n−1 = 1` —— 每条
     连接都归 0 号工作循环，于是它**证明不了**"全投 0 号"这种坏实现（变异时确认过：
     `[n=4] 分布 0 0 16` 是 n=4 那档抓到的）。
   - **分流**那条路上分布由内核哈希定 ⇒ 那一支只断言**守恒**（逐格之和 == 总数）；
     线程身份那条本体判据（"回调就在该连接自己那条循环的线程上"）两条形状都保留。

   **★ 分流支因此丢掉了转手支的分辨力** —— "每格都回落 0 号"这类坏实现在 0 号自己也收
   的时候抓不到（守恒照样成立）。那条改由**监听 socket 普查**负责，装置是
   `bench/bench_server.cpp` 的 `--port N --loops N` 加 `/stats`（构建选项
   `UVCPP_BUILD_BENCH`，默认 OFF）：`--loops 4` 那档在端口上数得到 **4 个 LISTEN 句柄**
   （属主 pid == 被读的那个进程），`--loops 1` 对照臂 **1 个**。实测 2026-09-24：
   4 个 / 1 个，16 条连接 `[7,3,4,3]`（脚本 `t1/fanout_census.py`，两臂一次跑完）。
   **macOS 那条腿（§3 路 (b)）若真走了，仍要那条"能红"的判据**：n 个循环、足够多的连接、
   逐循环计数，断言**没有任何一个循环拿到 0 条**。理由：那一格的失败形状正是**能跑、能响应、
   非 2xx 请求数 0、只有一个循环真干活** —— 与成功逐字相同。
   **[webapp 已过]**：`connection_count_at(i)` 落地，上面那个空转档由
   `test_loop_identity_and_ids`（n=3、16 条连接 ⇒ `{8,8}`，0 号那格恒 0）绕开了。
5. **停机**：n 条循环全部退出、没有句柄泄漏（走既有的页面堆门禁）。**[net 已过]**：
   门禁通过，worker 循环的 `loop_close` 由自报读数证（§1.1）。
   **[webapp 已过]**：`test_stop_fans_out_to_workers`（主判据，红法是那条
   「join() 已等 N ms，仍有 M 条循环没退出」的 ERROR，变异 M1 实测抓住 7 条）与
   `test_ws_session_and_close_frame`（n=2、会话在 1 号循环上，停机后对端必须收到
   **1 条** Close 帧、码 1001；变异 M2 抓住 3 条）。
   **量具是 `tests/tools/run_loop_leak_probe.py`，而它管的是"整块循环内存"**：
   `uv_loop_close()` 撞上没收的句柄返 `UV_EBUSY` 时，`~uvcpp_loop` 会把整块循环内存
   记成泄漏（§4.6（七））。所以**失败的那条启动路径也要收句柄**，判据就是同一支探针
   跑 `test_web_app_multiloop_func` 时必须报 `泄漏 0` —— 第一版落地时它报的是
   `泄漏 1`（三个句柄 = 一个 `loop_slot`），这条判据是**探针逼出来的、不是设计时想到的**。
6. **不进 README 的性能口径。** **75 k RPS**（流水线档 88 k）那个数是**单循环**口径，
   多循环不得混进去，也不得与 hical 的单 acceptor 数并列。扩展性实现之后**单开一节**写，
   单核那行不动。
   **[已落地 2026-09-23]**：README 两版各加了「Multi-loop scaling」一节（用法、契约、坑），
   单核那行一个字节没动。端到端读数的**装置与前提**写在
   [benchmark-rig.md](./benchmark-rig.md) —— **本机目前取不到这条读数**：这台机器
   推不动 4 条循环的服务端到顶（回环路径与生成器容量两头都够不着），照实记在那里，
   没有编一个数出来。这一条原先写的是「15 万 RPS」—— 那是**指错了路**：
   全仓没有任何一处出现过这个数，读者按它找不到对应的那一行。

## 7. 不做

- **(已撤销)** ~~**Windows 的 `n>1`** —— 排 `libuv/libuv#5282` 之后（§2）。~~
  见 §2 更正块：**两端都做，Windows 默认开**。
- **不走 §3 那两条免转手的 POSIX 路**（`REUSEPORT` / `dup()` 共享 accept 队列）——
  本版两端同一条转手路；那两条留给以后 Linux 侧的优化。
- **不把 `post()` 改成全进程队列** —— 那等于把今天每请求的锁换成一把全局锁，方向反了。
- **不给共享表加锁** —— 冻结之后只读，加锁是白付（§4.1）。
  **(已被 1.2.23-dev 收窄)**：这条原本是一句概括，落地时发现它只对**冻结后只读**的表
  成立。请求路径上**会写**的两张共享表都违了它，而且都不是可选的：静态缓存与压缩变体表
  是**跨循环共用的缓存**，切了会成倍吃内存还掉命中率（§4.1.1 那两处）⇒ 加锁。
  判据是"冻结之后还写不写"，不是"是不是共享"。
- **不动 `uvcpp_web_work_limit` 的取数**（§4.3）。
- **不动多进程那一套**：worker 重跑 `main()`、`fork` 那条纪律、master 只监督，全部照旧。
- **不改单循环的热路径**：n=1 时不许出现任何多循环的**代价**。判据是三件事，不是
  "不许出现 atomic"：① 不额外**加锁**（`slot_here()` 的 `loops_.size() == 1` 早退就是
  这一条）；② 不额外**遍历**（聚合量从"遍历 n 个 map"改成读 n 个原子计数，n=1 时是
  一次 relaxed load）；③ 不额外**起线程**。
  **(措辞更正)**：原文写的是"没有残余的 atomic"。每格的原子计数（`registry::live_`、
  `loop_slot::inflight_entries`、`finished`）在 n=1 时**照样存在、照样被读写** ——
  它们不是"多循环的残留"，而是聚合量读数与停机进度**本来就该有的**形状（单循环下
  也保证"跑着读聚合量"不是数据竞争）。按原措辞验收会得出"本批违规"这个错结论。

## 8. 与其他文档的关系

- **net 层的用法（已实现，看这篇）**：[net 网络层指南](./net-guide.md) 的多循环那一节 ——
  含 Windows 那条风险的**面向使用者**的完整交代（默认开、读数、代价、怎么关掉）。
- 多进程（正交、可叠加）：[多进程横向扩展设计](./worker-process-design.md) —— 含 §9 那条
  上游缺陷的证据包。
- 平台事实：多进程篇 §6 与本篇 §2 引的是同一批读数。**但 `SO_REUSEADDR` 那组（分布
  `[8,0]`）是在 Windows 上量的，只在 Windows 成立** —— 别拿它去论证 Linux/macOS 的行为；
  §3 路 (a) 的依据是 libuv 的 `REUSEPORT` 支持矩阵与 Linux CI 的读数，不是它。
- 用法（webapp 层实现之后）：[webapp 应用框架开发者指南](./webapp-guide.md)。
