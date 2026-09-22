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
  pipelined 化。上它之前要先查清的两件事**已经查完了**，结论在 §8。
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

## 8. 突发拒连接：实测结论（本库自己的接入路径不拒、也不挂）

§7 说要先查清两件事：外部贡献者读到的**突发拒连接**，以及**本库自己的 accept 路径在突发下的
表现**。都量完了，都是**否定结果** —— 而且否定结果正好把 §7 那条设计约束削掉一半。

装置：`_probe/burst/burst_probe.cpp`（仓库外，链 `build-h2` 的库）。三臂，同一突发形状、
同一 backlog，**只换"谁在 accept"**：

| 臂 | 是什么 |
|---|---|
| `nodrain` | 裸 `listen(backlog)`，**一声不接** —— 对照组 |
| `rawdrain` | 裸 `listen(backlog)` + 紧循环 `accept()` 立刻关 —— 排空上限 |
| `uvcpp` | `uvcpp_web_app` + `set_backlog(backlog)`，走真实接入路径 |

**读数一：这台机器上"被拒"根本不出现，超出的连接是挂住。** `nodrain` 在 `backlog=128`
下是 **ok=129 / refused=0 / timeout=1407**。129 = backlog+1，队列确实被填满了 ——
**所以对照组成立**（判据看得见"完成"与"挂住"的区别），而被填满之后客户端拿到的**不是 RST**，
是**永远停在 SYN_SENT**。⇒ 拿"拒连接条数"当判据在这台机器上**什么也测不出来**，
必须把"超时"和"被拒"分开记。

**读数二：本库的接入路径在突发下不失败。** 12 轮、合计 **37 888 条连接**，
**refused=0、timeout=0**，服务端 `accepted` 与客户端 `ok` 逐轮相等：

| 形状 | 结果 |
|---|---|
| 峰值并发 512（`2×3×256`），backlog 128 | 1536/1536 |
| 峰值并发 2048（`4×3×512`），backlog 128 | 6144/6144 |
| 峰值并发 4096（`4×4×1024`），backlog 128 | 16384/16384 |
| 峰值并发 512，**backlog 8**（队列只有 9 槽）×3 轮 | 1536/1536 三轮 |
| 峰值并发 512，**backlog 1**（队列只有 2 槽）×3 轮 | 1536/1536 三轮 |

**backlog=1 这一格是这条结论的承重证据**：队列只有 2 槽、同时压 512 条，全部完成
⇒ 排空速率远超到达速率，**连最尖的形状都不需要"排空到 `WSAEWOULDBLOCK`"来救**。

**读数三（意外收获，与横向扩展无关，但它是一条平台事实）：Windows 上 `listen(n)` 的
backlog 在 n>200 之后**不再增长**。** `nodrain` 逐档量：

| `backlog` | 实际完成数 |
|---|---|
| 8 / 32 / 64 / 100 / 128 / 200 | 8 / 33 / 64 / 100 / 128 / 200 |
| 201 / 256 / 512 / 1024 / 4096 | 201 / 200 / 200 / 201 / 201 |

⇒ **`set_backlog(>200)` 在 Windows 上是静默无效的**（默认 128 在限内，所以现在没问题）。
"backlog 给足"这条建议在 Windows 上有硬顶，写到文档里要带这个数。

### 这批**没有**证明的事（别读成结论）

- **吞吐对比不作数。** `uvcpp` 与 `rawdrain` 的墙钟排序**在不同批次之间翻面**
  （一批里 rawdrain 快 2.7×，另一批里 uvcpp 快 1.6×）。两者都与客户端同进程抢核，
  再加上回环上那笔随负载伸缩的税 ⇒ 这个装置的 `WALL_MS` **分辨不了接入路径的开销**。
  要真比，得把服务端与客户端拆成两个进程、逐轮配对同号。
- **没测每连接的应用层工作量。** 客户端只 connect 就关，**不发请求** ⇒ 量的是接入与关闭，
  `enable_tls`（`src/net/uvcpp_tcp_server.cpp:242`）那次 arm 之后的请求处理没进这条路径。
- **没测"master 自己 accept + IPC 转手"那条路**（§7 的机制本身还没实现）。
  外部贡献者的拒连接读数在他的装置上，本探针**没有**复现它、也**没有**否证它 ——
  只否证了"本库单进程的接入路径在突发下会拒"这个具体担忧。

**由此对 §7 的修正：**「master 必须排空 backlog，否则突发下会拒连接」里**因与果都换了**——
在这台机器上突发**不会**拒连接，而排空仍然值得做，理由从"防拒连接"变成**降接入延迟**
（队列深了，客户端的 connect 要等排空才有归宿）。`WSAEWOULDBLOCK` 那条仍是正确写法，
但它保的是延迟不是可用性。

相关文档：[webapp 应用框架开发者指南](./webapp-guide.md)、[压测靶场](./benchmark-rig.md)。
