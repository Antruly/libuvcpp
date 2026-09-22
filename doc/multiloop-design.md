# 多循环横向扩展设计（`set_loops`）

> **状态：设计稿，未实现。** 本文定的是形状与判据，代码还没有。
> 文里的 `src/...:行号` 引用由 `check_doc_lines.py` 锁着 —— 改代码时门禁会把该一起改的地方顶红。

这一篇讲**打算怎么做**，以及**为什么先在 POSIX 落地**。姊妹篇是
[多进程横向扩展设计](./worker-process-design.md)：两者正交、可叠加，但**次序不同**
（§2 说清为什么）。

## 1. 形状

一条 setter，位置和 `set_worker_processes(n)` 一样：

```cpp
// doc-snippet: fragment — 目标形状，不是可用代码：set_loops() 还不存在。
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
| 共享监听 | 是（内核散列，或共享 accept 队列） | 是（`fork` 继承 fd） |
| 平台支持 | POSIX 有；**Windows 这一版 `n>1` 报错**（§2） | 同样 Windows `m>1` 报错 |
| 叠加 | `set_worker_processes(4)` + `set_loops(4)` = 4 进程 × 4 循环，互不干扰 | 同左 |

**名字待定（归维护者）**：`set_loops(n)` 是本文的用法。备选 `set_worker_loops(n)`
（与 `set_worker_processes` 对仗，但"worker"在那个上下文里已经指子进程，容易混）或
引入一个 `uvcpp_loop_group` 类型（多一个类型、收益不明）。我不替这件事拍板。

## 2. 为什么先在 POSIX 落地：Windows 挡在同一把锁上

**结论：多循环的 Windows 那一支，和多进程的 Windows 那一支，挡的是同一件东西。**
不是"多循环更简单所以先做"，而是它在 Windows 上**根本无路可走**。

Windows 上一条监听句柄**绑死在一个循环的完成端口上**，而且压着 32 个预投的 AcceptEx
⇒ 想把它挪给别的循环**必被拒**（重新关联只在句柄没有在途 I/O 时才允许）。于是：

- `UV_TCP_REUSEPORT` 在 Windows 上被 libuv **无条件拒绝**（`libuv:win/tcp.c:298-299`）；
- libuv 在 Windows **故意不设** `SO_REUSEADDR`（`libuv:win/tcp.c:277-287`，注释写的是它
  "effectively allows 'stealing' a port which is in use by another application"）；
- 实测（本机标准库探针，A–E 矩阵）：两边都设 `SO_REUSEADDR` 时**两个进程能共存**，
  但连接 **100% 归先绑者**（分布 `[8,0]`，把 accept 顺序反过来仍是 `[8,0]`），
  原主一关才全接手 ⇒ **它不是 `SO_REUSEPORT` 的替代品**。

⇒ Windows 上唯一剩下的形状是「**一个接受者 accept + 把连接交给别的循环**」，而那条路今天
挂着一条 libuv 层的内存安全缺陷（上游 issue **libuv/libuv#5282**，机制与判据见姊妹篇 §9）。

**所以这一版 `set_loops(n>1)` 在 Windows 上返回错误、拒绝启动**，与 `set_worker_processes`
同一条政策：**静默退化是唯一危险的失败方式，而它长得和成功一模一样**（几个 worker 都起来了、
只有一个真干活，每个都打 `rc=0`）。报错是响的，比静默好。

**次序不是"多循环能替代多进程"。** 多循环省掉的是 IPC 那一层（管道、ack、master 监督、
重跑 `main()`），**省不掉转手那一条** —— 只要还在"把接受的连接交给另一个循环"，Windows 上
就是同一个洞。所以：**多循环先在 POSIX 落地，Windows 支与多进程的 Windows 支一起排
#5282 之后。**

## 3. POSIX 的两条路：内核散列，或共享 accept 队列

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

**两条路都有一条要写进使用文档的性质**：连接落在哪个循环上由**内核散列**决定，
**不保证均匀**，客户数少时可能明显偏。所以验收里必须有一条"看得见分布"的判据（§6），
不能假设它是平的。

## 4. 真正的成本：把 per-app 容器切成 per-loop

这一节是这份设计里**唯一有分量的活**，也是它比"多起几个线程"贵的地方。

**判据是"容器是不是 per-object"，不是"状态是不是 per-object"。** `std::map`/`std::list`/
`std::deque` **并发插不同 key 也是 UB** —— 装每请求状态的容器是共享的，那就不算 per-object。
按这个判据扫全仓，闸门是这几张表：

| 容器 | 位置 | 触碰频率 |
|---|---|---|
| `uvcpp_http_server::contexts_` | `src/web/uvcpp_http_server.h:988` | **每请求**（`.cpp` 里 52 处引用） |
| `uvcpp_web_app::inflight_` | `src/webapp/uvcpp_web_app.h:1489` | **每请求** |
| `uvcpp_web_app::upgraded_` | `src/webapp/uvcpp_web_app.h:1433` | 每次 WS 升级 |
| `uvcpp_tcp_server::clients_` | `src/net/uvcpp_tcp_server.h:468` | 接受 / 关闭 / 计数 |

**比上面几条都靠前的一条：`post()` 本身是单循环的。** `src/webapp/uvcpp_web_app.cpp:1842-1862`
里只有一份 `loop_tid_`/`post_queue_`（成员在 `src/webapp/uvcpp_web_app.h:1548-1556`），
投递入口判据是 `loop_tid_ == std::this_thread::get_id()`。而 `post()` 正是"把活儿挪到循环
线程上"的**原语** —— 框架自己到处在用它，**它得先变成 per-loop**，别的一切才好谈。

### 4.1 好消息：大部分切分是"多建几个对象"，不是"给容器加锁"

`uvcpp_http_server` 在自己的构造函数里 `new uvcpp_tcp_server`（`src/web/uvcpp_http_server.cpp:44-46`），
而 `uvcpp_tcp_server` 在自己的构造函数里 `new uvcpp_loop`（`src/net/uvcpp_tcp_server.cpp:37-41`）。
⇒ **n 个 `uvcpp_http_server` 实例 = n 份 `contexts_` + n 份 `clients_` + n 条循环/线程。**
`uvcpp_tcp_server.cpp:200` 的 `new uvcpp_tcp_client(loop_)` 与 `:203` 的 `s->accept(...)`
用的都是**这个 server 自己的** loop，所以那两行天然正确 —— 接受者就是这个循环（§3）。

于是形状是一句话：

> **一个 app = n 份 per-loop 工作状态 + 一份注册之后冻结的共享表。**

per-loop 那份装：`loop`、循环线程 id、`post_queue_`、`http_`（连同它里面的 `contexts_`
与 `clients_`）、`inflight_`、`upgraded_`、连接登记表、文件传输登记表、`idle_timer_`、
`shutdown_timer_`。

共享冻结的那份装：`router_`（`src/webapp/uvcpp_web_app.h:1203`）、`middlewares_`（`:1204`）、
`ws_router_`/`ws_handlers_`（`:1243-1244`）、`stream_router_`（`:1259`）、`upload_routes_`
（`:1275`）、压缩变体表。它们**只在注册期与 `start()` 期间写** —— `chain_storage_` 的全部
写点集中在 `src/webapp/uvcpp_web_app.cpp:2592-2652`，而它就在 `start()` 里。冻结期之后
多线程只读，**不需要锁**。

### 4.2 一处必须跟着改的判据：连接 id 的"是不是我发的号"

`uvcpp_web_conn_id` 是 `uint64_t`（`src/webapp/uvcpp_web_connection.h:57`），由**登记表自己的**
计数器发（`src/webapp/uvcpp_web_connection.h:268` 的 `next_id_`，
`src/webapp/uvcpp_web_connection.cpp:47` 的 `next_id_++`），而 `issued()` 的判据是

```cpp
// doc-snippet: fragment — 引述现状，不是待实现的形状。
return id != UVCPP_WEB_INVALID_CONN_ID && id < next_id_;   // uvcpp_web_connection.cpp:192
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

## 5. 对外约束

### 5.1 `set_loops(1)`（或不调用）＝ 今天逐字节相同

不建线程、不碰 fd、不看环境变量、不改变任何现有语义。这条要能**变异证明**：把 n=1 那条
分支改坏（比如强行走多循环初始化），现有用例里必须有一个红。没有这条，"默认没变"只是一句
承诺 —— 与姊妹篇 §5.1 同一条纪律。

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

1. **`n=1` 逐字节相同 + 变异证明**（§5.1）。
2. **聚合量仍然是聚合量**：`inflight_count()` 必须对**所有**循环的每个连接求和 —— 头文件里
   那条注释已经写明这一点（`src/webapp/uvcpp_web_app.h:1398-1399`），切分时别改成读某一份。
3. **id 不乱**：n 个循环下 id 全局唯一、`issued()`/`alive()` 对**别的循环**的 id 不误判为真
   （§4.2）—— 这条要专门的用例，因为它今天恒真。
4. **看得见分布**：n 条循环各自的连接数与在途请求数要能被读到。理由是两个方向的失败长得
   一样 —— "只有一个循环真干活"（§2 那种静默退化）和"散列不均"。**分布不是判据，能看见
   分布才是判据**；判"平不平"要按 §3 那条性质（内核散列、少量客户端时会偏）来定，别拍一个
   均匀度阈值。
5. **停机**：n 条循环全部退出、没有句柄泄漏（走既有的页面堆门禁）。
6. **不进 README 的性能口径。** 15 万 RPS 那个数是**单循环**口径，多循环不得混进去，
   也不得与 hical 的单 acceptor 数并列。扩展性实现之后**单开一节**写，单核那行不动。

## 7. 不做

- **Windows 的 `n>1`** —— 排 `libuv/libuv#5282` 之后（§2）。
- **不把 `post()` 改成全进程队列** —— 那等于把今天每请求的锁换成一把全局锁，方向反了。
- **不给共享表加锁** —— 冻结之后只读，加锁是白付（§4.1）。
- **不动 `uvcpp_web_work_limit` 的取数**（§4.3）。
- **不动多进程那一套**：worker 重跑 `main()`、`fork` 那条纪律、master 只监督，全部照旧。
- **不改单循环的热路径**：n=1 时不许出现任何多循环的残留（没有残余的 atomic、没有空转的
  表遍历）。

## 8. 与其他文档的关系

- 多进程（正交、可叠加）：[多进程横向扩展设计](./worker-process-design.md) —— 含 §9 那条
  上游缺陷的证据包。
- 平台事实（`REUSEPORT` 支持矩阵、`SO_REUSEADDR` 实测矩阵）：多进程篇 §6 与本篇 §2 引的是
  同一批读数。
- 用法（实现之后）：[webapp 应用框架开发者指南](./webapp-guide.md)。
