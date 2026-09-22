# 多进程横向扩展设计（`set_worker_processes`）

> **状态：设计稿，未实现。** 本文定的是形状与判据，代码还没有。
> 文里的 `src/...:行号` 引用由 `check_doc_lines.py` 锁着 —— 改代码时门禁会把该一起改的地方顶红。

这一篇讲**打算怎么做**。做成之后怎么用，会进 [webapp 应用框架开发者指南](./webapp-guide.md)。

## 1. 形状

接入成本是 `main()` 里的**一条语句**，外加一个 setter：

```cpp
// doc-snippet: fragment — 目标形状，不是可用代码：uvcpp_worker_process 与
// set_worker_processes() 都还不存在（本页状态是"设计稿，未实现"），handler 也是占位。
#include <webapp/uvcpp_web_app.h>
using namespace uvcpp;

int main(int argc, char** argv) {
  uvcpp_worker_process::split(argc, argv);  // ← 必须是 main 的第一条语句

  uvcpp_web_app app;
  app.set_host("0.0.0.0").set_port(8080)
     .set_worker_processes(4);              // 不调用 = 1 = 今天的行为

  app.get("/hello", handler);

  app.start();                              // worker：接住继承来的监听 fd
  app.join();                               // worker：跑完就退进程，不返回
                                            // master：监督循环
  return 0;                                 // 只有 master 走到这一行
}
```

角色的自动判定发生在两处，用户都不用管：

| | `worker_process::split()` | `start()` / `join()` |
|---|---|---|
| **master** | 认出"我不是 worker"，返回 | bind+listen 持住端口；`join()` 起 worker 并监督 |
| **worker** | 认出"我是第 k 个"，记下身份，返回 | 接住继承来的 fd；`join()` 跑完就 `_exit(0)` |
| **不启用**（默认） | 一个分支都不走 | 今天的行为，逐字节相同 |

`uvcpp_worker_process` 由库提供全局实例。它是全局的，有两个硬理由：身份必须在 `app.start()`
处可见；分裂点必须早于任何用户线程。

**三个名字已定：`uvcpp_worker_process` / `split()` / `index()`，setter 是
`set_worker_processes(n)`。** 两处取舍写在这里，省得以后有人问"为什么不是别的"：

- **不叫 `run()`** —— `uvcpp_web_app::run(uv_run_mode)` 已经有别的意思了，同一个库里两个
  `run` 含义不同会混。`split` 说的是它真正干的事。
- **class 不叫 `guard`** —— 它同时管 master 与 worker 两个角色，不只是"守卫"。
  叫"工作进程类"是按使用者的说法来的。
- **不需要用户自己声明全局变量**：库内部持有那份全局实例。用户声明会多一处可以写错的地方，
  而"必须在 `main()` 第一条语句"这条纪律已经够容易违反了。

还有一条**没定**：多进程横向扩展**进不进 README 的性能口径那一行**。我的判断是**先不进** ——
它是尚未实现的设计，而 README 顶上那个数是和 hical 做对比的口径，扩进来会把"单核比单核"
这个前提弄糊。实现之后单开一节写扩展性，单核那行不动。

## 2. 为什么是重跑 `main()`，不是"在那个方法里把服务跑完"

这一条决定了整套机制的形态，而且它是从"路由注册写在哪"推出来的，不是选出来的：

**路由是在 `worker_process::split()` 那一句**之后**注册的。** 所以 worker 进程必须**把用户的
`main()` 走完**才拿得到路由表 —— `run()` 那一刻它还不知道有哪些路由，没法替用户把服务跑起来。

于是只有两种做法：

| | 做法 | 结论 |
|---|---|---|
| (a) | worker **重跑 main**：POSIX `fork` 从那一句继续往下跑 | **采用** |
| (b) | 把路由注册提到那一句之前（全局构造期注册） | 否 —— 依赖静态初始化顺序，脆弱 |

由此推出两条对用户可见的语义：

1. `worker_process::split()` 在 worker 里**必须返回**（否则后面那段注册代码不跑）。
2. 「worker 跑完就退进程」落在 **`join()`** 上：worker 的 `join()` 永不正常返回，直接退进程。

> 注意是 `join()` 而不是 `start()` —— `start()` 按今天的语义是"起后台线程、阻塞到 bind
> 有结果为止"，它会返回。真正阻塞的是 `join()`，所以语义分叉点在那里。

## 3. POSIX（Linux / macOS）：零通讯

**master 只做一件事：把端口占住。** 它走今天已有的 bind 链
（`src/webapp/uvcpp_web_app.cpp:1742` → `src/web/uvcpp_http_server.cpp:71-72` →
`src/net/uvcpp_tcp_server.cpp:152-153` → `src/handle/uvcpp_tcp.h:59-60`）建出监听 socket，
**然后不跑循环** —— `uv_listen` 会在 `listen(fd, backlog)` 那一步就把端口占住，
而 master 的 loop 从不 `uv_run`，所以它永远不会 accept。于是"master 不接请求"是天然的，
不需要额外机制。

**worker 继承那个 fd，各自 `uv_tcp_open` + `uv_listen`。** 所有 worker 阻塞在**同一个
accept 队列**上，内核叫醒一个 —— worker 之间**零通讯、零共享内存、零路由复制**。

这条路在 libuv 上走得通，我核过三处：

- `uv_tcp_open` 只要求该 fd 不被同一个 loop 重复持有（`libuv:unix/tcp.c:352-365`）；
- `maybe_new_socket` 在句柄已有 fd 时直接返回、**不重建 socket**（`libuv:unix/tcp.c:85-110`）；
- `listen()` 对**已经在监听的 socket 是合法的**，只更新 backlog（`libuv:unix/tcp.c:440`）。

本库这边已经有现成的口子：`src/handle/uvcpp_tcp.cpp:53` 的 `uvcpp_tcp::open` 就是
`uv_tcp_open`。

**分裂用 `fork()`。** 不用 re-exec，三条理由：fd 自动继承（不需要传 fd 号、不需要清
`FD_CLOEXEC`）；写时复制便宜；**用户的全局构造只跑一遍**（子进程直接继承那份已经构造好的
全局状态），而 re-exec 会让全局构造也跑 n+1 遍。

`fork()` 的代价是一条必须写进文档的约束：**fork 点必须在任何用户线程起来之前。**
`worker_process::split()` 已经在 `main` 的第一条语句，处在最安全的位置；但如果用户的**全局
构造器**起了线程或连了库，这条就破了（`fork` 之后子进程里只有调用线程活着，别的线程持有的
锁在子进程里永远不释放）。这是用 `fork` 必须付的账，nginx 也是同一条规矩。

## 4. master 只监督

master 不接请求、不进请求路径。它的 `join()` 是一个监督循环：起 worker、等 worker
（`waitpid`）、死了拉起、收到 `SIGTERM` 时先停 worker 再退。

这条性质是运维侧的前提 —— master 不持有连接状态，才有资格在 worker 之外独立地做重启与
换班。它与 nginx 一致。

## 5. 三条对外约束

### 5.1 不调用 `set_worker_processes()` ＝ 今天逐字节相同

不 fork、不 exec、不碰 fd、不看环境变量。这条要能**变异证明**：把 n=1 那条分支改坏，
现有用例里必须有一个红。没有这条，"默认没开"就只是一句承诺。

### 5.2 用户的 `main()` 会跑 n 遍

worker 从 `worker_process::split()` 那一句继续往下跑（§2），所以用户写在 `main()` 里的一切
**每进程一次**的初始化 —— 连数据库、开日志文件、读配置、建线程池 —— 会**在每个 worker
里各跑一遍**。逃生口是

```cpp
// doc-snippet: fragment — 同样是目标形状：这个静态方法还没实现。
uvcpp_worker_process::index();   // -1 = master，0..n-1 = worker
```

**这是这个形状唯一要用户操心的点**，必须写进使用文档。它也是这条路线相对"单进程多循环"
的**结构性代价**：nginx 能做多进程是因为它**拥有 `main()`**，而本库是被嵌进别人的
`main()` 的库 —— 框架替用户起进程这件事本身逆着库的形状。

（`fork` 让**全局**构造只跑一遍，所以"每进程一次"的重灾区是 `main()` 里那段，不是全局
初始化。这是 §3 选 `fork` 而不是 re-exec 的直接好处。）

### 5.3 worker 退出用 `_exit()`

worker 的 `join()` 跑完循环后直接 `_exit(0)`：**跳过用户的 `atexit` 与静态析构** ——
这正是"不跑后面的代码"要的语义。代价是**框架必须在退出前自己 flush**（日志、统计），
不能指望用户的清理代码。

## 6. Windows：这一版 `n>1` 直接报错

`set_worker_processes(n>1)` 在 Windows 上**返回错误、拒绝启动**，而不是退化成"只有一个
worker 真干活"。

这条是有意的。Windows 上 `fork` 与免通讯的共享端口**都不存在**：`UV_TCP_REUSEPORT`
被 libuv 无条件拒绝（`libuv:win/tcp.c:298-299` 直接 `return ERROR_NOT_SUPPORTED;`），
而 libuv 在 Windows **故意不设** `SO_REUSEADDR`（`libuv:win/tcp.c:277-287`，注释写的是
它"effectively allows 'stealing' a port which is in use by another application"）。

**于是静默退化是这条路唯一危险的失败方式，而它长得和成功一模一样** —— nginx 在 Windows
上就是这个现状（官方已知问题：几个 worker 都能起，但只有一个真的干活）。报错是响的，比
静默好。旁边那条路（把 listener 复制给各 worker、各自 `uv_listen`）也验证过了：两个
worker **恒定为 0 条连接**、rps 单调恶化，而每个 worker 都打 `rc=0`。

Windows 要真正能用，得走 **master 自己 `accept()` + 每连接 `WSADuplicateSocketW` 转手**。
那套的账写在 §7，等两件事查清再上。

## 7. 不做 / 没解决

- **Windows 的转手机制**（master 接受 + `WSADuplicateSocketW` + 管道 + ack）。它的代价是
  **master 变成单接受者**（它死了整个服务就停）、每条连接多一次 IPC 与 ack 往返，
  并且 master 必须**排空 backlog**（一次唤醒 accept 到 `WSAEWOULDBLOCK`）并把转手
  pipelined 化。上它之前要先查清的两件事**已经查完了**，结论在 §8；但这条路上后来又冒出
  **第三件事，而且是拦路的** —— 见 §9。
- **不解决 `contexts_` 那一族。** 多进程形态下它们天然正确：每个 worker 一份，就是今天的
  n=1 语义。`uvcpp_http_server` 的 `contexts_`（`src/web/uvcpp_http_server.h:988`）、
  `uvcpp_web_app` 的 `upgraded_` / `inflight_`（`src/webapp/uvcpp_web_app.h:1433` / `:1489`）、
  `uvcpp_tcp_server` 的 `clients_`（`src/net/uvcpp_tcp_server.h:468`）都不需要切成 per-loop。
  这是选这条路**白拿**的最大一块。
- **日志那条闸门在进程内照样存在。** `src/webapp/uvcpp_log.cpp:338-352` 持锁到
  `target->write(record)` 返回 ⇒ 用户 sink 在全局锁里跑。多进程不改变这一点。
- **`work_limit_` 仍按进程推。** `src/webapp/uvcpp_web_work_limit.cpp:234-238` 是
  `UV_THREADPOOL_SIZE × 4`（下限 16），与进程数无关 ⇒ n 个 worker 下它**仍然是对的数**。
  要写进文档的是另一件事：**n 个 worker × 每进程 4 条池线程 = 4n 条**，调
  `UV_THREADPOOL_SIZE` 时按每进程算。
- **统计要跨进程聚合。** `uvcpp_tcp_client` 的 `reclaim_stats()` / `try_write_stats()`
  （`src/net/uvcpp_tcp_client.h:625` / `:653`）本来就是 `static` 的进程级聚合，
  多进程下变成"每进程一份"，要看总数得自己聚合。
- **那个突发探针不进仓库、不进 CI。** 它要断言的失败模式（"队列满了会拒连接"）在这台机器上
  **基本不出现**（§8），而且"满了之后是被拒还是挂住"**本身随平台变**（POSIX 给 RST、
  Windows 挂住）⇒ 写成用例只会得到一条在 CI 上恒绿、换个平台恒红或恒绿、断言的东西还不是
  被测物的用例。结论留在 §8 的文字里，装置留在仓库外的 `_probe/`。

## 8. 突发拒连接：实测结论

§7 说要先查清两件事：外部贡献者读到的**突发拒连接**，以及**本库自己的 accept 路径在突发下的
表现**。都量完了。

装置：`_probe/burst/burst_probe.cpp`（仓库外，链 `build-h2` 的库）。三臂，同一突发形状、
同一 backlog，**只换"谁在 accept"**：

| 臂 | 是什么 |
|---|---|
| `nodrain` | 裸 `listen(backlog)`，**一声不接** —— 对照组 |
| `rawdrain` | 裸 `listen(backlog)` + 紧循环 `accept()` 立刻关 —— 排空上限 |
| `uvcpp` | `uvcpp_web_app` + `set_backlog(backlog)`，走真实接入路径 |

下面的每个数都来自一次落盘的运行（`_probe/burst/log.txt`，仓库外），不是回忆。

**读数一（对照组成立）：队列填满之后，客户端拿到的是"挂住"，不是"被拒"。**
`nodrain`、backlog 128、4 线程一批压 512 条：**ok=128 / refused=0 / timeout=1920**。
128 恰好等于 backlog —— 队列确实被填满了，而被填满之后多出来的那些**永远停在
SYN_SENT**。⇒ 判据必须把**超时**和**被拒**分开记：只数"拒连接条数"在这台机器上
几乎什么都测不出来。

**读数二：本库的接入路径在突发下不失败。** 10 轮、合计 **35 328 条连接**，
**refused=0、timeout=0**，服务端 `accepted` 与客户端 `ok` 逐轮相等：

| 形状 | 结果 |
|---|---|
| 峰值并发 512（`2×3×256`），backlog 128 | 1536/1536 |
| 峰值并发 2048（`4×3×512`），backlog 128 | 6144/6144 |
| 峰值并发 4096（`4×4×1024`），backlog 128 | 16384/16384 |
| 峰值并发 512，**backlog 8**（队列 8 槽）×3 轮 | 1536/1536 三轮 |
| 峰值并发 512，**backlog 1**（队列 2 槽）×3 轮 | 1536/1536 三轮 |

**backlog=1 那一格是这条结论的承重证据**，因为它是**机制**不是空读数：队列只有 2 槽、
同时压 512 条而全部完成 ⇒ 排空速率远超到达速率，**连最尖的形状都不需要"排空到
`WSAEWOULDBLOCK`"来救**。反过来，"峰值并发 4096 也全过"这一格的说服力**弱于**它看起来的样子
（见读数三的尾巴：溢出在这台机器上本来就多数表现为挂住而不是拒绝）。

**读数三（平台事实）：Windows 上 `listen(n)` 的队列深度是 `n`，n>200 之后封顶在 200。**
`nodrain` 逐档量，`ok` 数即实际队列深度：

| `backlog` | 8 | 32 | 64 | 100 | 128 | 200 | 201 | 256 | 512 | 1024 | 4096 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 实际 | 8 | 32 | 64 | 100 | 128 | 200 | 200 | 200 | 200 | 200 | 200 |

⇒ **`set_backlog(>200)` 在 Windows 上是静默无效的**（默认 128 在限内，所以今天没问题）。
"backlog 给足"这条建议在 Windows 上有硬顶，写进文档要带这个数。

**这条的尾巴：「这台机器不给 RST」不是平台不变量 —— 拒连接出现过，只是不可复现。**
11 档逐档扫的那一轮里，`backlog=4096`（单批深度 4128）那一格是
**ok=200 / refused=534 / timeout=3394**。两臂归因（把"backlog 值"和"单批深度"分开）**都归不到**：

| 归因臂 | refused |
|---|---|
| backlog 128，深度 4128（同深度、小 backlog） | 0 |
| backlog 4096，深度 1056（同 backlog、小深度） | 0 |
| backlog 200，深度 4128 | 0 |
| backlog 4096，深度 4128 —— **原配置复现两次** | **0 / 0** |

⇒ 两个候选变量各自中和之后**都没复现**，原配置重跑两次也**都没复现**。能说的只有：
深突发下偶发，1/15 轮，**与 backlog 值和批深度都不单独对应**（剩下的嫌疑是连跑累积的机器状态，
没验）。**所以 §7 那条设计不能假设"拒连接不会发生"**，只是不必把它当常态失败模式。

### 这批**没有**证明的事（别读成结论）

- **吞吐对比不作数。** 同一形状（`4×1×512`，backlog 128）下 `rawdrain` 是 705.5 ms、
  `uvcpp` 是 203.9 ms；而更早的批次里排序是**反的**。机制是清楚的：`rawdrain` 那条臂是
  **自己空转的 `accept()` 紧循环**，烧掉一个核，而客户端线程与它在**同一个进程**里抢核 ⇒
  烧 CPU 多的那条臂看起来更慢。再加上回环上那笔随负载伸缩的税 ⇒ 这个装置的 `WALL_MS`
  **分辨不了接入路径的开销**。要真比，得把服务端与客户端拆成两个进程、逐轮配对同号。
- **没测每连接的应用层工作量。** 客户端只 connect 就关，**不发请求** ⇒ 量的是接入与关闭，
  `enable_tls`（`src/net/uvcpp_tcp_server.cpp:242`）那次 arm 之后的请求处理没进这条路径。
- **没测"master 自己 accept + IPC 转手"那条路**（§7 的机制本身还没实现）。
  外部贡献者的拒连接读数在他的装置上，本探针**没有**复现它、也**没有**否证它 ——
  只否证了"本库单进程的接入路径在突发下会拒"这个具体担忧。

**由此对 §7 的修正：**「master 必须排空 backlog，否则突发下会拒连接」里**因与果都换了**——
在这台机器上突发**几乎不**拒连接，而排空仍然值得做，理由从"防拒连接"变成**降接入延迟**
（队列深了，客户端的 connect 要等排空才有归宿）。`WSAEWOULDBLOCK` 那条仍是正确写法，
但它保的是延迟不是可用性。

## 9. Windows 转手路径挂着一条 libuv 层的内存安全缺陷

§7 那条「master 接受 + `WSADuplicateSocketW` 每连接转手」有人搭出来跑了：**能跑，但会崩，崩的是
内存安全** —— `0xC0000374` / ASan `heap-use-after-free`。不是延迟、不是吞吐、不是优雅退出。
频率按落盘日志重算是 **每 ~94 480 条 worker 连接一次**（12 次事件 / 1 133 759 条连接，两次完整
跑动）⇒ **不是"CI 恒绿"那种稀有，几轮 churn 就中**。

**读数来自外部贡献者的装置**（本机没有 adopt 路径，复现不了，按他的读数引用）；**下面引的源码
位置是我在 vendored 树里逐条核过的**。注意他最初给的行号取自他加了仪器的树，与干净树有偏移，
这里一律用干净树的号。

### 9.1 机制：libuv 在 Windows 写路径上依赖的一条不变式，在转手 socket 上破了

- `libuv:win/tcp.c:120-125` —— 只有**不是** `UV_HANDLE_EMULATE_IOCP` **且**没有非 IFS LSP 时，
  libuv 才调 `SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)`
  （`:122`）并置 `UV_HANDLE_SYNC_BYPASS_IOCP`（`:124`）；
- `libuv:win/req-inl.h:70` —— `UV_SUCCEEDED_WITHOUT_IOCP(r)` = `r && (flags & SYNC_BYPASS_IOCP)`；
- `libuv:win/tcp.c:921-929` —— `WSASend` 同步成功 + 这个标志 ⇒ libuv 判定"内核不会投包"，于是
  **自己**把 req 插进 pending 队列（注释原话 `Request completed immediately.`，`:924`）。

**不变式 =「标志在 ⇒ skip-on-success 生效 ⇒ 同步成功不会来包」。它在转手过来的 socket 上破了**
⇒ 同一个写请求被派发两次：一次来自 libuv 自己的插入，一次来自内核真的投来的完成包。

**洞的形状**（这一处是拼上他读数之后才清楚的）：`libuv:win/tcp.c:102-111` 里，
`CreateIoCompletionPort` **失败**且句柄是 imported 才置 `UV_HANDLE_EMULATE_IOCP` —— 而那个标志
会正确关掉上面那个 BYPASS 分支。**关联成功的 imported 句柄不置它** ⇒ `:120` 的
`if (!EMULATE_IOCP && !non_ifs_lsp)` 照进 ⇒ BYPASS 照设。而 `uv_tcp_open` 恰恰走 `imported=1`
（`libuv:win/tcp.c:1489`），转手那一步在 `libuv:win/tcp.c:1506-1510` ⇒ **本库的
`uvcpp_tcp::open`（`src/handle/uvcpp_tcp.cpp:53`）正是踩在这一步上的那个入口。**

### 9.2 uvcpp 侧为什么变成 UAF，而不是"多跑一次回调"

`callback_write`（`src/req/uvcpp_write.cpp:88`）取出闭包后走 `invoke_completion`
（`src/req/uvcpp_req.h:156`）——**回调返回之后 `delete self`**。所以重复的那次完成打在已释放对象上。
⇒ 「在自己的完成回调里释放自己」这个形状，**活不过一次重复投递**，与内核为什么多投无关。

### 9.3 三条候选修法，与先做哪一条

- **(甲) uvcpp 侧：写对象别在自己的完成回调里释放（推迟释放）—— 读完源码后否掉了，见 §9.6。**
  它不是"窗口不够宽"，是**约束根本不落在写对象的生存期上**：重复的那次派发走的是 libuv 自己的
  `uv__process_tcp_write_req`（`libuv:win/tcp.c:1111-1155`），而那个函数里**只有 `req->cb` 一步
  是被守卫的**（`:1133` `if (req->cb)`）；它前面是 `handle->write_queue_size -= req->u.io.queued_bytes`
  （`:1117-1118`），后面是 `handle->stream.conn.write_reqs_pending--`（`:1142`），收尾是
  `DECREASE_PENDING_REQ_COUNT(handle)`（`:1154`）—— 而那个宏第一步就是
  `assert(handle->reqs_pending > 0)`（`libuv:win/handle-inl.h:51-60`）。⇒ 重复派发把**句柄级的两个
  计数器打成负数**：Debug 构建下当场断言，Release 下 `reqs_pending` 再也回不到 0 ⇒
  `uv__tcp_endgame`（`libuv:win/tcp.c:233-239`，四条 assert 之一是 `reqs_pending == 0`）**永远不会
  为这个句柄跑**。**推迟释放救不了，把 `cb` 清空也救不了 —— 两次派发打的是同一份句柄记账。**

  **这一句已被实测修正（§9.7 有完整读数）**：装置里这个句柄上恒挂着一个未完成的零长读
  （取包时 `flags` 含 `UV_HANDLE_READ_PENDING` / `ZERO_READ`），它给 `reqs_pending` 留了余量 ⇒
  第二次派发的减数落到**流级的 `write_reqs_pending`** 上（0 → **−1**），而 `reqs_pending` 只是
  1 → 0、endgame **照常入队**、句柄**照常关**（**没有泄漏**）。但那个 −1 是**静默**的：
  `:1143` 那一整块（含 CLOSING 时的 `closesocket`、以及 `uv__process_tcp_shutdown_req` 的
  **唯一**调用点 `:1148-1151`）不再走；`uv__tcp_try_cancel_reqs` 的 `writing`（`:1399`）为假 ⇒
  `CancelIo`（`:1406`）被跳过（在途写不取消）；`uv__process_tcp_shutdown_req` 里那条
  `assert(write_reqs_pending == 0)`（`:210`）**永远到不了**。
  ⇒ **结论不变，理由更精确**：推迟释放挡掉的是"打在已释放对象上"这一次，挡不掉第二次回调
  与这份记账损伤 —— 换症状，不是防线。
- **(乙) libuv 侧：别让派发重复。** 两个候选形状（读数见 §9.7；都是上游的事，不是本仓的）：
  **(乙1) 对 imported 句柄不拿同步快路径** —— 不调 `SetFileCompletionNotificationModes`
  （`libuv:win/tcp.c:79` / `:122-124`），代价是这类连接每次写多一个 IOCP 往返。**这条已 A/B 实测**
  （§9.7：死亡 55 → 0、ASan 报告 54 → 0，代价 ≈2.5% rps，且它是**修复**不是护栏 —— 关掉 BYPASS 后
  libuv 不再自己插队、只等内核的包，"给一个消费一个"两种情况下账都对）；**(乙2) 真做幂等派发** —— 但"这条包是重复的"只能从 req 内存判断，
  而那份内存应用已经交还（§9.7 的次序读数）⇒ 它不能只加一个 `dispatched` 位，
  得连内存生命周期一起设计。
- **(丙) 结构上别用 master 已经关联过的句柄**：让 worker 走"自己建、自己关联"的接受路径。
  两个形状 —— (a) **监听**句柄在**首次关联之前**就复制给各 worker、各自 `uv_listen`；
  (b) 监听只归 master，但 worker 用**自建**的接受句柄 `AcceptEx` 接进来（§9.6 第三条）。
  §6 实测那条腿恒 0，原因（完成端口关联是**一次性**的，第二次报 `87`）已查清，所以 (a) 今天做不到。
  两个形状都要求 worker 手里那条句柄**还没被谁关联过** ⇒ **它们由同一个判据决定成败**：
  那个洞是 `FROM_PROTOCOL_INFO` 造的句柄特有的，还是所有走 `uv_tcp_open`（`imported=1`）的句柄都这样。
  **这是设计问题，不是补丁问题** —— 而且它正好就是判定实验第三臂要回答的那件事。

**（甲) 一否掉，候选就只剩 (乙) 与 (丙)，两条都不在本仓手里。** §9.3 初稿把 (甲) 排在最前，
理由是"① 缺口在 CI 里复现不出来（库今天没有任何用例走 adopt）⇒ 会是一行没人能证伪的防御代码，
还挂在每次响应写热路径上；② 判定实验决定走 (乙) 还是 (丙)、(甲) 不决定；③ 实验在对方装置上"。
②③ 仍然成立，① 也仍然成立 —— 但它已经不重要了：**(甲) 不是"没证据就先别做"，是做了也没用**（§9.6）。

**新的顺序：判定实验（在对方装置上）→ 假设成立则 (乙) 提上游 / 不成立则回到 (丙)。**

**判定实验（假设，可证伪）**：`SetFileCompletionNotificationModes` 在一个由
`WSASocketW(FROM_PROTOCOL_INFO)` 从 `WSADuplicateSocketW` blob 造出来的句柄上**返回 TRUE 但不生效**。
三臂（第三臂见 §9.6）：走 adopt（甲臂）vs 同一 socket 显式补一次该调用（乙臂）
vs **worker 侧自建接受句柄**（丙臂），看"已派发又插包"是否只在甲臂出现。

### 9.4 一处我先读错的读数：`req-socket=INVALID_SOCKET`

我上一版据此推过"重复包是在连接关掉之后才被取走的，所以释放得推迟到 close callback 之后"。
**这个推论不成立 —— 源码里有更简单的一种解释。**

那个 `INVALID_SOCKET` 是**第一次**派发自己置上的：`uv__process_tcp_write_req` 里
`write_reqs_pending--` 归零 + `UV_HANDLE_CLOSING` ⇒ `closesocket(handle->socket); handle->socket =
INVALID_SOCKET;`（`libuv:win/tcp.c:1143-1147`）。**第二次**派发随后才读到它，于是打包成完成包
的 `req-socket` 字段送出。⇒ **这条读数只能说明包"迟到"**（连接已经关了才被取走），
**说明不了它迟到到 `uv__tcp_endgame`（`:233-239`）之后** —— 次序恰好相反：endgame 要等
`reqs_pending == 0`，而重复派发把那个数打负了（§9.6），所以 endgame 根本不会跑。

**读数仍然值得留**（上游报告里"包迟到"是要给的一份证据），**但它不再是判据** —— (甲) 的存废
由 §9.6 的句柄记账决定，与"取走时刻"无关。

### 9.5 这件事对本设计的意义

- §7 那条转手是**这条路上 Windows 唯一的一条**，而它现在挂着一条内存安全缺陷 ⇒ **§6 那条
  「Windows `n>1` 直接报错」的决定不变，而且更站得住了**：既然唯一那条真能用的路有拦路的缺陷，
  "报错"就不是保守，是当前唯一诚实的默认。
- **别和 §6 那条恒 0 的机制混起来**：那条是"完成端口关联只能做一次"，这条是"BYPASS 标志在
  adopted 句柄上不生效"（**假设，待判定**；§9.6 第一条把它的竞争解释挤到只剩它一个）。
- 顺带把 §6 那条恒 0 的**根因**补上，并说清两条为什么是同一族的：两个 worker 各对同一个继承
  socket 发 `uv_listen`，而关联**只在句柄还有在途 I/O 时**被拒（第二次 `CreateIoCompletionPort`
  报 `87`）—— 监听句柄有 32 个预投的 AcceptEx ⇒ 必然被拒 ⇒ 第二个 worker 预投的包**永远回不来**，
  `uv_listen` 照样返回 0。反过来，**一条刚接受、还没有在途 I/O 的连接 socket 是可以被重新关联的**
  —— 这正是"每连接转手"这套东西之所以还能跑起来的原因。**"静默"（§6）与"崩溃"（§9）是同一个
  约束的两面：一条句柄的完成端口，只在它闲着的时候归你。**
- 顺带记一条：**"把 master 那条监听句柄直接转给 worker"在 Windows 上不是"还没设计"，是今天做不到**
  —— 它已经被 master 关联过，而关联**只在句柄还有在途 I/O 时**被拒（上一条）⇒ 监听句柄恒有在途 I/O
  ⇒ 一律被拒（这就是 §6 那条恒 0）。要让它成立，worker 得在**首次关联之前**拿到它，或者改用自建的
  接受句柄（§9.6 第三条）。

### 9.6 源码级定位：读干净树得到的三件

三件都是**读出来的**（vendored 树，行号为干净树），不是猜的。

1. **"标志在 ⇒ 那次调用返回过 TRUE"是代码保证的。** `uv__tcp_set_socket` 里
   `if (!SetFileCompletionNotificationModes(...)) return GetLastError();`（`libuv:win/tcp.c:122-123`），
   **置标志在 `:124`** —— 调用失败就直接返回错误、根本走不到置标志那一步。他读到的
   `flags=6f08c`（BYPASS=1）与"同步成功"并存，**"返回真但不生效"是唯一幸存的解释**。
2. **取包循环零过滤。** `libuv:win/core.c:489-495`：每个出队条目只做两件事 ——
   `req = uv__overlapped_to_req(overlappeds[i].lpOverlapped);` 然后
   `uv__insert_pending_req(loop, req);`。**不看 `lpCompletionKey`、不看句柄状态、不看这个 req
   是不是已经被派发过。** ⇒ libuv 在 Windows 上**没有任何一层**能吸收一个多余的完成包；
   唯一的不变式就是"内核不会投"。**这同时说明 (甲) 为什么救不了**（§9.3）：包会先被写进
   `req->next_req` —— 而本库的 `uv_req_t` 是**独立分配**的（`src/req/uvcpp_req.h:184` 那个成员），
   `delete self` 经 `~uvcpp_req()`（`src/req/uvcpp_req.cpp:16`）走 `free_req()`（`:96`）把它一起还掉
   ⇒ **那一写就已经落在已释放内存上，比派发更早**。

   **顺带把 (甲) 的成色说准**（这是我第一版说糊的地方）：推迟释放**能**挡掉"打在已释放对象上"
   这一次，**挡不掉**随后那次派发对句柄记账的破坏 —— 于是崩溃被换成了"Debug 断言 + Release 下
   这个句柄永远走不到 endgame（连接泄漏）"。**那是换个症状，不是防线。**
3. **一条还没被排除的结构性出路（线索，不是结论）。** 把"关联"从**监听**句柄挪到**接受**句柄：
   监听句柄**不关联任何 IOCP**，各 worker 自建接受句柄、各自关联到自己的端口。按文档，`AcceptEx`
   的完成包投给**接受句柄所属的端口**（监听句柄没有端口时）—— **这一条我是从文档读的，没在
   本机量过**。若成立，worker 走的就是 libuv 在 Windows 上天天走的标准路径（自己 `WSASocket`
   建的句柄、自己关联），那个"skip-on-success 不生效"的洞**可能压根不适用**。
   **它成不成立只取决于一件事**：那个洞是 `WSASocketW(FROM_PROTOCOL_INFO)` 造出来的句柄特有的，
   还是**所有走 `uv_tcp_open`（`imported=1`）的句柄都这样**。
   ⇒ 判定实验要加**第三臂**：worker 侧 `WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
   WSA_FLAG_OVERLAPPED)` 自建接受句柄，`AcceptEx` + `SO_UPDATE_ACCEPT_CONTEXT` 接进来，再
   `uv_tcp_open`。**只有这一臂决定 Windows 上还剩不剩一条既免转手、也免得改上游的路。**

### 9.7 §5 判定实验的收尾读数（外部装置，2026-09-22）

**装置臂：17 次命中 / ~3.3×10⁶ 条成功连接 ⇒ ≈1/1.9×10⁵**，而 17 次的计数**逐字同形**
（`sync == sends == procs`、`pend=0 fail=0 bad=0`、`insW = sends+1`、`iSync = sends`、
`iPkt=1`、`iAfter=1`、`iRedun=0`）。读法：

- `pend = 0` ⇒ 那条包**不是**"它自己 pending 写的合法完成"（否则 `pend ≥ 1`）；
- `insW = sends+1` 且 `iAfter = 1` ⇒ 同一条 req 被插了两次，第二次发生在**它已经被派发过之后**；
- 派发侧独立计数 `procs = sends + 1`（`uv__process_tcp_write_req` 入口）⇒ **不只是插进队列，
  应用的回调真的被调了第二次**。
- 包的字段这次取对了：`status=0` + `bytes=117` ⇒ **一次成功的发送完成**，不是取消
  （取消会是 `0xC0000120`、bytes=0）。（他上一版那条"包字段"取自 req 自己的 overlapped，
  已自行更正。）

**微观臂（进程内 adopt 同形探针，4×10⁶ 轮）：0 命中**，上界 1/1.3×10⁶（rule of three）
—— 比装置里的速率还低 6 倍 ⇒ **光"adopt"这一件事解释不了它**。阳性对照是有牙的：去掉 modes
的 2000 轮 = 2000/2000，且包在 `WSASend` 返回时**已经躺在队列里**（所以"返回后立刻零超时取一次"
是完备探测，不是抽样）。

> **他同时撤回了自己上一轮那句"假设被证伪"**：当时那 1600 轮阴性只排除快于 1/533 的速率，
> 而装置里的速率是 1/2×10⁵ —— **没有功率的阴性结果不是反证**。这条单记，它正是
> "判据要有牙"的一个实例（本仓的规矩：判据要对着对照组跑一遍）。

**次序读数（§9.4 缺的那一半，他重做了）**：他原来的事件环**按 socket 号认人，而 socket 号会回收**
（64 K 事件窗口里约 10 代）⇒ 上一版那个"取走时刻"取错了人。改成按 req/handle 认人后两次命中同形：

```
S 提交 → D 派发进入 → C uv__tcp_close 进入 → Z 应用写回调返回
      → W write_reqs_pending-- → X closesocket() → P 重复包出队
```

⇒ **P 在 Z 之后**（应用已经把写对象交还），**P 在 X 之后而 endgame 还没跑**。
这与 §9.4 的更正一致（`INVALID_SOCKET` 是第一次派发自己置的），并且补上了那一半：
包确实**迟到**，迟到到"对象已释放"之后。

**我那条预测，实测下来是"机制对、计数器错"**（他按上面的判据读了）：`reqs_pending` 是 **1 → 0**，
**没有变负**；变负的是**流级的 `write_reqs_pending`**（0 → **−1**）。原因写在 flags 里：那一刻
句柄上**还挂着一个未完成的零长读**（`0x10000` / `0x40000`）⇒ 它给 `reqs_pending` 提供了余量，
而 `write_reqs_pending` 只被写 req 加减、没有余量。所以：

- **"句柄永远停在 CLOSING"不成立**：`flags` 从 `0x72089` 变成 `0x720a9`，多出来的正是
  `UV_HANDLE_ENDGAME_QUEUED`（`0x20`，`libuv:src/uv-common.h:87`）⇒ **endgame 被正常排队、
  句柄照常关**，没有泄漏。
- **但 −1 是真的，而且是静默的**：它跳过 `libuv:win/tcp.c:1143` 那一整块
  （块内含 CLOSING 时的 `closesocket`，以及 `uv__process_tcp_shutdown_req` 的**唯一**调用点
  `:1148-1151`）、让 `uv__tcp_try_cancel_reqs` 的 `writing`（`:1399`）为假从而跳过
  `CancelIo`（`:1406`）、并使 `uv__process_tcp_shutdown_req` 里那条
  `assert(write_reqs_pending == 0)`（`:210`）**永远到不了** —— 也就是说
  **"Debug 下当场断言"不会以预期的形式出现**。
- 对"句柄上当时没有别的 req"的情形，我原来那版才成立（第一派发就把 `reqs_pending` 减到 0 ⇒
  `DECREASE_PENDING_REQ_COUNT` 的 `assert(reqs_pending > 0)` 在 Debug 下会断）。**本 rig 不满足这个前提**
  —— 服务端连接恒挂零长读。（这一条我写在这里，是因为它是"把条件写全"的实例：我漏了那个并存的读请求。）
- 同一批还有一条形状读数：**大多数进程生命是"两次打印之间就没了"**（`ENTRY=1, EXIT=0`），
  而那两次之间只有 `req->cb()` ⇒ Release 下**每次事件几乎等于一次死亡**；ASan 臂死得更早
  —— 死在我预测的那一处，`uv__insert_pending_req` 里那 8 字节的写。

**次序那个怀疑也降级了**（他补了我没列的一个维度）：他那个进程内探针的句柄是 **`FROM_PROTOCOL_INFO`
血统**（`WSADuplicateSocketW` 同进程 + `WSASocketW(FROM_PROTOCOL_INFO,…)`），不是我猜的自建句柄；
而**源副本什么时候关**这个维度他用三臂扫了（关在 modes 后 / 关在关联前 / 关在两者之间，
每臂 **1 000 000 轮**：**0 / 0 / 0**；同一次跑动的正对照关掉 modes = 3000/3000 全中）⇒
**次序不是触发条件**（进程内）。把它和"进程内怎么摆都是 0"拼起来，剩下的差别只有**真的跨了进程**。

**⇒ (乙1) 已有 A/B 实测，而且它是修复不是护栏**（同一份 uv.dll，只翻一个开关）：

| 臂 | 连接 | worker 退出 | ASan 报告 |
|---|---|---|---|
| A（现状）Release | 581 955 | 55（1/10 582） | — |
| **B（imported 不设 `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`）** Release | 578 296 | **0** | — |
| A ASan | 429 107 | 27 / 25 | **28 / 26** |
| **B ASan** | 582 236 | **0 / 0** | **0 / 0** |

B 两档都是 0，上界 1/19 万（rule of three）⇒ ≥18×（Release）/ ≥24×（ASan）。**为什么是修复**：
BYPASS 在时，同步成功走 `libuv:win/tcp.c:923` 那条分支 —— **libuv 自己把 req 插进队列**
（`:929`），**它根本不期待完成包**；内核多给的那一个只能撞到同一个 req 上。BYPASS 关掉后走
`:930` 那条（`UV_SUCCEEDED_WITH_IOCP`）—— **libuv 不插、等内核的包** ⇒ 内核给一个消费一个，
**两种情况下账都对**：B 不再依赖那条不变式。

**代价**（真栈 QPS，4 worker / 100 并发 / 10 s，四档服务端 CPU 都 ≈4.00 核）：
A 351 814 / 342 707 rps，B 341 820 / 335 431 rps ⇒ **≈2.5% rps、延迟 +8 µs**。
那条装置里**每条连接都是转手来的** ⇒ 这是"adopt 用户付全额"的**上界**，混合部署按 imported
句柄占比摊。（单次对比看不出来：A 自己两次就差 2.6%。）

**一条对 §7 那套转手有约束的装置侧发现**：他想用"推迟 master 关副本"来验上面那条嫌疑，
结果 `MP_CLOSE_DELAY_MS=1000` 直接把吞吐打到 **63 连接/s** —— 原因是**重复句柄的 FIN 只在
"最后一个句柄关闭"时才发**，而 churn 客户端是 FIN 定界（`Connection: close` + 读到 EOF 才重连）
⇒ **在这个设计里，连接的收尾其实是由 master 关掉自己那份副本促成的。**
任何推迟这个关闭的实验都会改掉连接寿命形状（同一份重建 DLL 在 delay=0 下复现基线
1/8 800 ⇒ 重建无污染，塌的是形状）。⇒ 写进 §7 那条：master 必须**及时**关掉自己那份副本，
否则连接不会收尾 —— 这不是优化，是这条机制的工作条件之一。

**他的头号假设（未证）**：差异在"**真的跨进程**"那一步 —— blob 过管道交给另一个进程打开、
且 master 那份副本在 worker 调 modes 时**还开着**。验证实验（子进程 + blob 走管道，其余不变）
他下一轮能做。

**⇒ 第三臂（§9.6 第三条）的次序，因此也定了：先走 (乙1)，第三臂只在 (乙1) 被上游拒了才做。**

理由：进程内那条 0/10⁶ 说明这个洞**需要"真的跨进程"的形状**；而"`uvcpp_tcp::open` 对**同进程**
adopt 用户安不安全"这个问题**在进程内装置上答不了**（两种血统、三种次序全是 0/10⁶ ⇒ 要答它
必须把自建接受句柄搬进真 rig）。**注意 0/10⁶ 是上界，不是"证明干净"。**

于是两条路的定位是：

1. **(乙1) 便宜、已实测有效、但需要上游**：(乙1) 落地后，"设计上还留不留一条免转手的路"这个问题
   就**不必再答**；
2. **第三臂（自建接受句柄）是唯一不需要改上游的路**，代价是架构级改动 ⇒ 只在 (乙1) 走不通时才做。
   它如果要成立，靠的正是"**那条句柄从来没跨过进程**"这一点与上面"需要跨进程"的读数同一方向 ——
   但那是推论，**没量过**。

相关文档：[webapp 应用框架开发者指南](./webapp-guide.md)、[压测靶场](./benchmark-rig.md)。
