# net 网络层指南

`src/net/` 是**应用层传输模块**：TCP 服务端/客户端、UDP 服务端/客户端，加一个把读事件
分出语义的 `net_read_result`。它建在 `handle/` 与 `req/` 之上，比那层好用，比
`web/` 层更裸——`web/` 的 HTTP 与 WebSocket 就建在它上面。

- 打开方式：`-DUVCPP_BUILD_NET=ON`（默认开）
- 包含方式：`<net/uvcpp_tcp_server.h>` / `<net/uvcpp_tcp_client.h>` /
  `<net/uvcpp_udp_server.h>` / `<net/uvcpp_udp_client.h>` / `<net/uvcpp_net_read.h>`
- 五个头都用命名空间 `uvcpp`，**没有** `uvcpp_net_read` 这个类——那个是头文件名，
  里面装的是 `net_read_result`

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一层没做"的地方都明确标出来
> ——那些地方比 API 更容易踩。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [最小可运行程序](#2-最小可运行程序)
3. [服务端生命周期](#3-服务端生命周期)
4. [多循环（set_loops(n)）](#4-多循环set_loopsn)
5. [读的三条路](#5-读的三条路)
6. [net_read_result 的事件契约](#6-net_read_result-的事件契约)
7. [连接归谁管](#7-连接归谁管)
8. [写](#8-写)
9. [同步与异步](#9-同步与异步)
10. [UDP](#10-udp)
11. [TLS 接线](#11-tls-接线)
12. [错误处理](#12-错误处理)
13. [典型坑](#13-典型坑)
14. [没做的（如实列出）](#14-没做的如实列出)

---

## 1. 这一层是什么

| 类 | 头 | 角色 |
|---|---|---|
| `uvcpp_tcp_server` | `src/net/uvcpp_tcp_server.h:123` | `bind` / `listen`，**拥有**每一条 accept 出来的连接，给所有连接共用一份读回调 |
| `uvcpp_tcp_client` | `src/net/uvcpp_tcp_client.h:99` | 双模式（异步回调 / 同步 `*_wait`）TCP 连接；服务端交给你的那条连接也是它 |
| `uvcpp_udp_server` | `src/net/uvcpp_udp_server.h:51` | 绑定的 UDP 套接字，交付数据报时带来源 ip/port；没有"连接对象" |
| `uvcpp_udp_client` | `src/net/uvcpp_udp_client.h:72` | UDP 客户端，`bind`/`connect`、异步与同步发送、内部接收缓存 |
| `net_read_result` | `src/net/uvcpp_net_read.h:68` | 一次读事件的载体：数据 / 对端关闭 / 读错误 |

`src/net/uvcpp_tcp_server.h:31` include 了 `net/uvcpp_tcp_client.h`，后者又 include 了
`net/uvcpp_net_read.h`。所以**只写一句 `#include <net/uvcpp_tcp_server.h>` 就够**。

---

## 2. 最小可运行程序

一个回显服务端。

比"读回调里直接 `write` 回去"多了十几行，是因为**写这一侧有一条在途契约**：
一条连接上同时只允许一笔异步写在途，上一笔没完成时再写返回 `UV_EALREADY` 且
**那一笔的字节不会被发出**（§9）。负载一上来（同一连接的两次读先后脚到）就会撞上，
撞上的形状是"回显偶尔少一截"，而单条连接手工测是看不出来的。所以回显自己攒一层。

```cpp
#include <cstdio>
#include <map>
#include <string>

#include <uv.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>

namespace {

// 每条连接一份待发字节。示例用 map 记账；挂在自己的连接结构上等价。
struct Pending {
  std::string bytes;
  bool in_flight = false;
};
std::map<uvcpp::uvcpp_tcp_client*, Pending> g_pending;

void flush(uvcpp::uvcpp_tcp_client& client) {
  Pending& p = g_pending[&client];
  if (p.in_flight || p.bytes.empty()) return;

  std::string out;
  out.swap(p.bytes);            // `write` 返回前会把字节拷进自有缓冲，所以 out 可以就地析构
  p.in_flight = true;

  const int rc = client.write(out.data(), out.size(), [&client](int status) {
    if (status != 0) {
      std::fprintf(stderr, "write failed: %d\n", status);
    }
    // 能在这里接着写：框架是在把在途标志清掉**之后**才调这个回调的。
    // 用 find 不用 []：这一笔在飞的时候对端可能已经关了、待发被扔掉了。
    auto it = g_pending.find(&client);
    if (it == g_pending.end()) return;
    it->second.in_flight = false;
    flush(client);
  });

  if (rc != 0) {                // 非 0 = 这一笔没提交，字节还在手上，放回队首
    Pending& q = g_pending[&client];
    q.in_flight = false;
    q.bytes.insert(0, out);
    std::fprintf(stderr, "write: %d\n", rc);
  }
}

}  // namespace

int main() {
  uvcpp::uvcpp_tcp_server server;

  // 必须在 listen() 之前设好。一份回调，所有连接共用。
  server.set_read_callback(
      [](uvcpp::uvcpp_tcp_client& client, const uvcpp::net_read_result& r) {
        if (r.is_data()) {
          g_pending[&client].bytes.append(r.data, r.size);
          flush(client);
        } else if (r.event == uvcpp::net_read_event::PEER_CLOSED) {
          g_pending.erase(&client);   // 连接没了，它那份待发也扔掉
          std::fprintf(stderr, "peer closed\n");
        } else {
          std::fprintf(stderr, "read error: %d\n", r.error);
        }
      });

  int rc = server.bind("0.0.0.0", 8080);
  if (rc != 0) {
    std::fprintf(stderr, "bind: %d\n", rc);
    return 1;
  }

  rc = server.listen([](uvcpp::uvcpp_tcp_client* client) {
    (void)client;   // 连接已登记、已起读、断开时自动释放，这里通常什么都不用做
  });
  if (rc != 0) {
    std::fprintf(stderr, "listen: %d\n", rc);
    return 1;
  }

  server.run();   // 直到 stop() 关掉监听句柄才返回
  return 0;
}
```

对应的客户端：

```cpp
#include <cstdio>

#include <uv.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>

int main() {
  uvcpp::uvcpp_tcp_client client;

  int rc = client.connect("127.0.0.1", 8080, [&client](int status) {
    if (status != 0) {
      std::fprintf(stderr, "connect: %d\n", status);
      client.stop();
      return;
    }

    // 与 read_start() 互斥，二选一。
    client.read_start_events(
        [](uvcpp::uvcpp_tcp_client& c, const uvcpp::net_read_result& r) {
          if (r.is_data()) {
            std::fwrite(r.data, 1, r.size, stdout);   // 二进制安全
            std::fputc('\n', stdout);
            std::fflush(stdout);
            c.close();
          }
        });

    client.write("hello, echo", 11, [](int ws) {
      std::fprintf(stderr, "write done: %d\n", ws);
    });
  });

  if (rc != 0) {
    std::fprintf(stderr, "connect start: %d\n", rc);
    return 1;
  }

  client.run();
  return 0;
}
```

---

## 3. 服务端生命周期

```cpp
// doc-snippet: fragment — 接口签名摘录。凑成完整 TU 就得造一个假的类定义把这几行
// 包起来，那反而看不出"这就是 uvcpp_tcp_server 上的签名"。
int bind(const char* ip, int port);                              // 自动选 IPv4/IPv6
int bindIpv4(const char* ip, int port);
int bindIpv6(const char* ip, int port);
int listen(std::function<void(uvcpp_tcp_client*)> connection_cb, int backlog = 128);
void stop(std::function<void()> on_stopped = nullptr);
size_t close_all_clients();
```

**连接回调是 `listen()` 的第一个参数**，backlog 第二。`uvcpp_tcp_server` **没有**
`on_connection(...)` 这种 setter——名字像的那个是别的类。`listen(nullptr)` 返回
`UV_EINVAL`。

**`bind` 不监听**，必须先 `bind*` 再 `listen`。

**`stop()` 只关监听句柄，不碰已经建立的连接**。要整个停掉是两步：
`stop()` 然后 `close_all_clients()`。`close_all_clients()` 返回"发起了关闭"的条数
——句柄已经死掉的连接会被回收但不计数。

两个 setter 有**顺序约束**，而且**都没有被强制**：

| setter | 约束 |
|---|---|
| `set_read_callback` | 必须在 loop 线程调用，且应当在 `listen()` 之前设好（`src/net/uvcpp_tcp_server.h:486`） |
| `set_ssl_context` | 必须在 `listen()` 之前设置（`src/net/uvcpp_tcp_server.h:618-619`） |

`set_read_callback` 的实现就是一句赋值（`src/net/uvcpp_tcp_server.cpp:1026-1028`），
**`listen()` 之后调不会报错、也不会生效于已有连接**——只有那之后 accept 的连接才吃得到。
这是个静默的半失效状态，别踩。

> 规矩是**回调里不要用同步的 `write_wait`/`read_wait`**（`src/net/uvcpp_tcp_server.h:92-97`）：
> 它们会把 loop 线程按住，一个慢对端能拖住这条 loop 上的**所有**连接。坑在于这两个函数
> 的完成回调是可选参数，**省略它就等于同步** —— 按 `src/net/uvcpp_tcp_client.h:369-369`，
> `cb == nullptr` **正是** `write_wait(data, len, 30000)`。同文件 `:99` 的官方示例回显
> 现在传的是显式回调（异步）。本页的例子同样传显式回调来避开它。

---

## 4. 多循环（`set_loops(n)`）

```cpp
// doc-snippet: fragment — 接口签名摘录，同上：凑成完整 TU 要造一个假的类定义。
int set_loops(int n);                  // 必须在 listen() 之前调；不调用 = 1 = 今天的行为
int loop_count() const;                // 1 + 工作循环数
size_t client_count_at(int loop_index) const;   // 0 号是接受者；越界返回 0
```

**`set_loops(n)` 把"一条循环"变成 n 条各跑自己循环的线程**，`1..n-1` 号各一条
**专用 `std::thread`**（不是 libuv 线程池里的那种）。**"新连接怎么落到哪一条上"分两种
形状，同一个二进制在不同平台上不一样**（运行时探测；`tcp_server->is_fanout()` 告诉你
这次是哪一种）：

- **内核分流**（平台接受 `UV_TCP_REUSEPORT`，Linux 是）：n 条循环**各自 `bind()` 同一个
  端口**，内核按四元组把新连接分给其中任意一条 —— **0 号自己也收属于它的那一份**。
  哪条落哪条由内核哈希定：**不保证均匀**，只保证逐格之和 == `client_count()`。
- **接受者 + 转手**（Windows，以及那个标志被拒时的回落）：0 号（就是 `get_loop()` 那条、
  由调用者的线程跑）accept 到连接之后把 socket **转手**给 `1 + (i % (n-1))` 号循环 ——
  **显式轮转**决定去向，所以分布是确定的、不会偶发，0 号一条都不留。

**两条腿各有一份正证据**：Windows 上转手是**唯一**形状（libuv 无条件拒绝那个标志），
本机跑的**就是**它 —— 那台机器上 `dup()` 那条腿编不到，跑的是同一函数的 Windows 支；
Linux 上 `UV_TCP_REUSEPORT` 恒绑得上 ⇒ 转手那条腿**本来永远跑不到**。macOS 按本仓自己
那份平台清单同样不支持那个标志 ⇒ 也应当回落，但这一点**没有**被任何一条腿直接见证过，
别写"macOS 验过"。补的测试口子是 `set_handoff_forced(true)`（**必须在 `set_loops()`
之前调**）：它只让 `bind_flags_for_loops()` 返回 0，于是走的是与 Windows **同一条**
回落路径，**不新增运行分支**。判据在 `tests/functional/tcp_multiloop_func.cpp` 的
"转手(强制 n=4)"那一档：回显 16/16、分布落满全部 n−1 条工作循环、**接受者线程一条
连接回调都不跑** —— 连接**能落到别的线程上**这件事，只有 `dup()` 转手这一条路解释
得了，这就是那条腿的正证据。**生产上别用它**：它是把内核分流换成用户态转手，代价更高。

`client_count_at()` 两条路上都看得见分布，但**别把两端写进同一套判据**。

**不调用它、或 `set_loops(1)`，与今天逐字节相同**：不建线程、不碰 fd、不看环境变量。
`n` 取 1..64，越界 `UV_EINVAL`；已经 `listen()` 过、或已经用 `n>1` 装过一遍，都返回
`UV_EBUSY`（装好之后不能再改）。

**工作线程是在 `set_loops()` 里起的，不是 `listen()` 里** —— 所以调用之后（哪怕还没
`listen()`）就已经有 n−1 条线程在跑。这也是它必须在 `listen()` 之前调的原因之一。

### 4.1 装上之后，谁在哪个线程上

| | n=1（今天） | n>1 |
|---|---|---|
| `listen()` 的连接回调、读/写/关闭回调 | 调用者的线程（loop 线程） | 在**那条连接自己那条循环的线程**上 |
| 同一个服务端上不同连接的回调 | 顺序 | **可能并发**（不同工作线程） |
| `set_read_callback` / `set_auto_read` / `set_ssl_context` / `set_tls_handshake_timeout_ms` | loop 线程 | **工作线程读**这些 ⇒ 必须在 `listen()` 之前设好，之后再改是数据竞争（`src/net/uvcpp_tcp_server.h:230-232`） |
| `client_count()` / `owns_client()` / `take_client()` / `return_client()` | loop 线程 | 登记表是**加锁**的，从任何线程调都对 |
| 驱动一条连接（`write` / `read_start` / `close`） | loop 线程 | 仍然**只能在它自己那条循环的线程**上 —— 跨线程直接驱动是未定义行为（`src/net/uvcpp_tcp_server.h:227-229`） |
| `close_all_clients()` | 立即关本循环名下的 | 本循环名下的就地关，**别的循环是异步发起**（返回值只数"发起了"） |

**你的回调要按"可能并发"写**：连接回调、数据回调都可能同时从几条工作线程进来。
每条连接自身的状态不用锁（一条连接永远在同一条线程上），**跨连接的共享状态要自己护**。

### 4.2 Windows：默认开着，代价是 EMULATE 那一档

**这一段要读，而且是 2026-09-22 改过的一段。** Windows 上没有 `REUSEPORT`、
`SO_REUSEADDR` 也当不了它的替代品（依据见[多循环设计](./multiloop-design.md) §2），
Windows 上就只剩这一条形状：**一个接受者 + 把连接转手**。**Linux 侧自 `1.3.5-dev`
起不再走这条** —— 那边 n 条循环各自绑同一个端口、由内核分流（§4.1；`is_fanout()` 可问
是哪一种），本节讲的转手税在 Linux 上不再付。Windows 上的转手只能靠
`WSADuplicateSocketW` + `WSASocketW(FROM_PROTOCOL_INFO)` + `uv_tcp_open`
（`src/net/uvcpp_socket_handoff.h`）。

**"这样造出来的 socket 是 imported 的，所以落在 `libuv/libuv#5282` 上"是错的。**
闸是 `UV_HANDLE_SYNC_BYPASS_IOCP`：它只在 worker 侧那次 `CreateIoCompletionPort`
**成功**时才置（`libuv:win/tcp.c:103-110` 与 `:117-124`），而成功与否取决于**源 socket
有没有被关联过**，不取决于谁造的句柄。两条血统的实测（外部贡献者仪器里的 worker 句柄
标志位，位值见 `libuv:src/uv-common.h:102`/`:104`）：

| 血统（socket 从哪来） | 源 socket | worker 侧关联 | EMULATE | BYPASS | `#5282` 那一族 |
|---|---|---|---|---|---|
| **本库 `set_loops(n>1)`**（被收养句柄） | **已关联** | 失败 `87` | **1** | **0** | **否：没有入口** |
| **本库 `set_loops(1)`**（= 不调用，**默认**；同循环 `uv_accept`） | **已关联** | **没有这一步** | **0** | **1** | **否：不是入口** |
| master 裸 `accept()` 再 dup | 未关联 | 成功 | 0 | **1** | 在（`87 次死亡` 是它的） |

实测 `flags`：`set_loops(n>1)` 那条腿（worker 侧 `uv_tcp_open` 收养之后）**`0x8e088`**
（EMULATE=1 / BYPASS=0）；`set_loops(1)` 在同循环 `uv_accept` 之后 arm 了读时是
**`0x0007f08c`**。**未 arm 时那个 `0x6f08c` 是推得的，不是测的** —— 它是
`0x0007f08c` 减掉 `READ_PENDING`（`0x10000`）那一位算出来的（外部贡献者交这个数时
自己标了）；它等于第 3 行 rig 那条腿**独立测到**的同值，而"同档"这条论断本来就
依赖这个等式成立，所以照旧列在这里，**但量程要跟着写**（他自己的原话：需要的话可以
在 uvcpp arm 读**之前**挂钩子把它变成实测，本仓没有要那台仪器 —— 等式两边已有一边是实测）。
`UV_SUCCEEDED_WITHOUT_IOCP`（`libuv:win/req-inl.h:69`）**只查 BYPASS 这一个位**。

> **"同一档"与"同一个入口"是两件事 —— 别把上面三行压成一行。** 第 1 行与第 2 行是**档不同**
> （第 1 行那位没置上，同步腿根本不触发）；第 2 行与第 3 行是**档相同、入口不同** ——
> `set_loops(1)` 那个 socket 是 libuv 自己 `accept` 出来的（`uv__tcp_accept` 里那次
> `uv__tcp_set_socket(..., 0)`，`imported=0`）⇒ 不是那一族的入口。**默认配置落在第 2 行**，
> 所以"本库默认走进 `#5282`"是错的。
>
> **引 libuv 时只说函数名 + 实参，不给修订行号**（规矩与理由见
> `doc/worker-process-design.md` §9.9 开头那句：外部贡献者那份 `libuv-clean` 与本仓引用的
> 修订行号**不等差**）。本节原先在这里写过 `libuv:win/tcp.c:663` —— 那个号是他那份修订的，
> 在上游 `v1.x` 上不是同一处，2026-09-22 按他的复核改成上面的写法。

**所以 87/55 那两组死亡读数属于「master 裸 `accept()` + 转手」那条血统 ——
多进程那条形状（以及外部贡献者台架上的同名装置），不是本库任何 `set_loops` 档的。**
那一组的读数是：**87 次死亡 / 2 170 885 条连接**；把那个开关关掉（= 预期修复）之后是
**3 462 684 条连接 / 0 / 0**（`doc/worker-process-design.md` §9.8、§9.9）。

**归因别指错地方**（2026-09-22 按外部贡献者的逐参复核改）：那次失败 `87`、以及"失败被静默
吞成 EMULATE"这两个后果，都发生在**转手腿自己**那次 `uv__tcp_set_socket(..., 1)` ——
也就是 `uv_tcp_open`（`libuv:win/tcp.c` 的 `uv__tcp_set_socket` 调用点，`imported=1`）。
第 1 行括注里"源是 `uv_accept`"说的是**源 socket 为什么已经被关联上**（`uv__tcp_accept` 那次
`imported=0`），**不是** 87 发生的地点 ⇒ **EMULATE 这个后果的作者是 `imported=1`**。

> **这里是两组不同的数，别接错。** 上面那一对来自同一台 rig 的两大批。另有**一组单变量 A/B**
> （同一份 `uv.dll`、只翻那一个开关，§9.7）：Release **55 → 0**（581 955 / 578 296 条连接）、
> ASan 报告 **54 → 0**（死亡 27 / 25）。**两组的口径也不同** —— Release 那对是单轮，ASan 两行
> 的连接数与事件数都是**两轮之和**。把 87 直接接到 55 后面，等于把两组读数拼成了一个实验。

**代价是个数，而数要带量程 —— 别只引一个**。2026-09-22 的三臂读数（同一会话、单条工作循环、
8 条并发连接、每请求 64 B 回显）：

| 臂 | 接受者 | 转手 | worker 句柄 | 回显 /s（两轮） |
|---|---|---|---|---|
| raw | 裸 `accept` | 有 | 正常 IOCP | 150 527 / 148 118 |
| inline | libuv `uv_accept` | **无** | 正常 IOCP | 145 534 / 141 893 |
| accept（**本库的形状**） | libuv `uv_accept` | 有 | **EMULATE** | 111 810 / 110 977 |

`inline` 与 `raw` 差 3~4% ⇒ **接受者用哪一套不值钱，转手本身也不贵**；而 `accept` 比 `raw`
低 **25~26%**、比 `inline` 低 **22~23%** —— 它和 `inline` 之间只差"句柄有没有被翻成 EMULATE"
一件事 ⇒ **那 22~23% 是那个翻转的价格，不是转手的价格**（`inline` 这一臂就是为把这两件事
拆开才加的；翻转的独立单价另见 §4.3 的 `l2`）。

端到端的 **2.5% rps** 是**另一个量程**：库在那套端到端台架上只占服务端 CPU 约 13%，
0.25 × 13% ≈ 3%，与 2.5% 同量级、可以对上 —— 所以**不要**把 2.5% 单独读成"这个缺陷的代价很小"。

> **这是性能读数，不是正确性读数。** 三臂都没有错回显、写请求数与写完成回调数逐条相等，
> 但"EMULATE 下会不会真的出现杂包 / 错派发"**仍然是留白** —— 别拿它当缺陷不存在的证据。

**本库这一版在 Windows 上默认就走这条路**（维护者知情并选择默认开：拒绝启动等于把 Windows
用户直接挡在多循环门外，而"静默退化"在这里是更坏的那个选项）。代价不是崩溃风险，而是上面
那条 EMULATE 折扣；要避开它就在 Windows 上 `set_loops(1)` 或干脆不调 —— 单循环那条路
不碰转手。

> **那个代价只在 `n>1` 上付**，这一点现在有读数（不再只是推导）：上游那笔候选修复
> （(乙1)）就是往 TCP 那条守卫 `if (!(handle->flags & UV_HANDLE_EMULATE_IOCP) && !non_ifs_lsp)`
> 后面再补一个 `&& !imported`，而它在 `set_loops(1)` 上**按构造是空操作**
> —— `&& !imported` 碰不到 `imported=0` 的句柄，实测那条路的 flags 与补丁无关（上面第 2 行
> 的 `0x0007f08c` 就是**没打补丁**的树）。⇒ 采纳它**不会**让默认配置变慢；25~26% 那一档
> 只落在主动开多循环的人身上。
>
> ⚠️ **别把 `ProtocolChain.ChainLen == 1` 接到这条 TCP 守卫上**（2026-09-22 按他的复核改正；
> 本节原先就写成 `ChainLen == 1 && !imported`）。TCP 这条里**没有 `ChainLen`**，那两个词
> 说的是两件事：`!(flags & EMULATE_IOCP)` 是**每句柄**的位，`!non_ifs_lsp` 是**进程级一次
> 探测**的结果（`win/winsock.c` 建一个 dummy socket、读 `SO_PROTOCOL_INFOW` 的
> `dwServiceFlags1 & XP1_IFS_HANDLES`，写进两个全局量 `uv_tcp_non_ifs_lsp_ipv4/ipv6`；
> `win/tcp.c` 那里只是**读**那两个全局量，自己不做任何探测）。`ChainLen == 1` 确实存在，
> 但在**另一条路**上、形状也不同：UDP 侧是**每 socket 一次** `getsockopt`，判据就是它
> （`win/udp.c` 的 `uv__udp_set_socket` 一族 —— 那正是他自己另开的那个 UDP 同族 issue）。
> **两者不能互相代替着引**：TCP 是"启动时一次 ⇒ 全局量"，UDP 是"每 socket 一次"。

> **边界："没有入口"是结构判断**（那一族唯一的闸恒假），不等于"这条路已经验过没有别的
> 问题"。`#5282` 的机制本身仍未定（`doc/worker-process-design.md` §9.1、§9.8），
> 把它写成"多循环这条路是安全的"就是拿"没观察到"当"不存在"。

> **判据别读错**：`imported` 是**观测到的相关性，不是已定的机制** —— 上游那段
> `SetFileCompletionNotificationModes` 两种血统都会走到。证据包与复现装置见
> [多进程横向扩展设计](./worker-process-design.md) §9。

> **口径**：多循环**不进** README 那个 **75 k RPS**（流水线档 88 k）的单循环口径，也不与
> hical 的单 acceptor 数并列。扩展性读数只在同一台机器上可比 —— README 的多循环一节讲用法
> 与契约，读数的**装置、门槛与前提**单开在 [benchmark-rig.md](./benchmark-rig.md)，
> 与上面这个口径是两回事。

### 4.3 在本库这套形状上量到的扩展与税（外部装置，2026-09-22）

口径：master `18c7d5a`、net 层直接驱动 `uvcpp_tcp_server`、同一会话、每臂 5 s、回环、
**客户端与服务端在同一个进程里**。同一臂两轮能差 ±9%，所以**只读比值、别读绝对值**。

| 臂 | 谁做 I/O | c=8 回显/s | c=64 回显/s | 相对 `l1` |
|---|---|---|---|---|
| `raw`（libuv 裸 `accept`，不转手） | 1 条 | 142 123 | 145 251 | 1.04× / 1.16× |
| `inline`（libuv `uv_accept`，不转手） | 1 条 | **149 506** | 140 826 | **1.09× / 1.12×** |
| `off`（**根本不调** `set_loops`） | 1 条 | 131 032 | — | 0.96× |
| `l1`（`set_loops(1)`） | 1 条 | 137 064 | 125 654 | 1.00× |
| `l2`（`set_loops(2)`，**每条都转手 + 翻转**） | 1 条 | **97 526** | 95 518 | **0.71× / 0.76×** ← 税 |
| `l4`（`set_loops(4)`） | 3 条 | **199 334** | 205 632 | **1.45× / 1.64×** |
| `l8`（`set_loops(8)`） | 7 条 | 203 050 | **403 383** | 1.48× / **3.21×** |

三条读法：

1. **税单独量出来了**：`l2` 是"同样只有一条循环做 I/O，但每条连接都走转手 + 被翻成 EMULATE"
   ⇒ 它相对 `l1` 的 **0.71~0.76×**（低 24~29%）落在 §4.2 那条翻转折扣的邻域，**在本库这套
   形状上直接量到**，不必再靠推断（与他 libuv 层装置上的 0.735~0.764× 同值）。
2. **收益压过税**：`l4` 在 `inline` 之上（199–206k vs 141–150k，四个交叉比值 1.33~1.46、
   中位 ≈1.4×）⇒ `l4/l1` 的
   1.45~1.64× 里既有 `n−1` 条循环的扩展、也有全部连接那份翻转折扣，**精确归因做不到**
   （两者同时在动），能做的是边界：`l4` 高于"没有翻转税的天花板" ⇒ 收益压过税。
3. `l8` 在 c=64 上 = **3.21× `l1`** —— 与本库早先那对 3.29× / 3.45× 落在同一邻域，
   但**这次是自己的形状、另一台装置**。上面那条"折扣不要乘进 3.29×"照旧。

**边界（照他的原文）**：8 条连接那几档是**延迟受限**的（`l4` ≈ `l8`，多出来的循环买不到东西），
要谈扩展只能看 c=64 那列；c=64 那列里客户端与服务端**同进程抢核**，**只可横着比**（各臂同一个
客户端），不能当"每核扩展性"读；`l4`/`l8` 的读数里**包含接受者做 accept + 转手**的开销。

> **★ 这批臂的标签要重读（2026-09-24）。** 上面 `l1`..`l8` 量的是**转手那个形状**
> （`lN` = 1 条接受者 + N−1 条工作循环，每条连接都转手 + 被翻成 EMULATE）。本版 Linux
> 侧 `set_loops(n)` 已改成内核分流（§4.1）⇒ **转手那道税在 Linux 上不再付**，这批读数
> 描述的是旧形状，**不能直接当新形状的基线**；新形状的读数要单独重量（分流形状量不到
> 内核散列的分布，判据也得跟着换，见[多循环设计](./multiloop-design.md) §6 第 4 条）。

**同一台装置还核了这几条**（都不用拍阈值）：`set_loops(0)` / `(65)` = `UV_EINVAL`，
装过之后再调 / `listen()` 之后再调 = `UV_EBUSY` 且**不改状态**；逐循环条数在**转手形状**下
**逐格**等于显式轮转（8 条连接 `n=4` ⇒ `loop0=0 loop1=3 loop2=3 loop3=2`），接受者循环上恒
0 条 —— **这两条在分流形状下都不成立**（0 号自己也收、分布由内核哈希定），那边只保证
"逐格之和 == 总数"；线程数
`set_loops(4)` 后 7 → 10、`set_loops(8)` 后 7 → 14，而 **`listen()` 之后不变** ⇒ §4.1
"工作线程是在 `set_loops()` 里起的"这一句有读数。

### 4.4 在每条工作循环上跑一次：`set_loop_start_hook()`

```cpp
// doc-snippet: fragment — 接口签名摘录（同 §4 开头那条：凑成完整 TU 要造假的类）。
void set_loop_start_hook(std::function<void(int, uvcpp_loop*)> fn);
```

**给每条工作循环装一个"就绪"钩子**，参数是循环号与那条循环。用途只有一个：属主要在
**每条**循环上建那些**只能建在循环线程上**的句柄（`uv_async`、`uv_timer`），并且要保证
在**任何连接落上来之前**建好 —— 而工作线程是在 `set_loops()` 里起的，那是唯一插得进去
的地方。

- **只对 1..n−1 号调**。0 号是接受者（`get_loop()` 那条），由调用方自己的线程初始化，
  没有"就绪"可言 —— 调用方本来就掌握时机。
- **时序保证**：钩子跑在 worker 放行 `start()` 之前 ⇒ **`set_loops()` 返回时全部已经
  跑完**。所以"先 `set_loops()` 再 `listen()`"这个既有顺序本身就保证了"连接到达时钩子
  已经跑过"，不需要另外同步。
- **必须在 `set_loops()` 之前设**。装晚了不生效 —— 它会往 stderr 说一句（不静默），
  但**不会**返回错误码，所以别把它当可查的失败路径。
- `n == 1`（或不调）时它**一次都不被调用**，也不产生任何开销：没有工作线程可挂。

### 4.5 收尾那两件事：`set_loop_exit_hook()` 与 `rollback_loops()`

```cpp
// doc-snippet: fragment — 接口签名摘录（同上）。
void set_loop_exit_hook(std::function<void(int, uvcpp_loop*)> fn);
int  rollback_loops();                 // 0 = 收干净了；UV_EBUSY = 已经 listen() 过
```

`set_loop_exit_hook()` 是 §4.4 那个钩子的**对称面**：每条工作循环的线程在自己的
`uv_run` 返回之后调一次，参数同样是循环号与那条循环。**必须有它，理由不是"对称好看"** ——
属主在 §4.4 钩子里建的那些句柄（`uv_async`、`uv_timer`）如果没人收，
`uv_loop_close()` 会撞上 `UV_EBUSY`，而本库在那种情况下**把整块循环内存记成泄漏**
（`src/net/uvcpp_loop_worker.cpp` 里那段注释记的就是这一族）。所以属主**必须**在这个
钩子里把建在那条循环上的句柄关掉。

- **只对 1..n−1 号调**；0 号那条是调用方自己的循环，收尾也归调用方。
- **顺序**：先本库自己的清场（`close_clients_of_loop()`），再是你的钩子，最后才
  `uv_loop_close()`。⇒ 钩子跑的时候**循环还活着、内存还没释放**，可以安全 `uv_close()`。
- **钩子里只关句柄**：别停循环、别 `post()`、别阻塞（那是循环自己的线程，堵住它
  就再也退不出来了）。
- **必须在 `set_loops()` 之前设**，装晚了只打一条 stderr（同 §4.4，不静默、也没有返回码）。
- `n == 1` 时同样一次都不调。

`rollback_loops()` 是**给 `listen()` 失败那条路用的**：`set_loops()` 一返回就把
n−1 条工作线程起好了（见 §4），而接着的 `bind()` / `listen()` 还会失败 —— 失败时调它，
把那几条循环连内存一起收掉。**带门、只属于 `listen()` 之前那段窗口**：`listen()` 已经
成功过就返 `UV_EBUSY`（那时工作循环可能已经持有连接，该走 `stop()` / 析构，不能把循环
从底下抽掉）。没有工作循环时它是空操作，所以"`set_loops()` 自己失败时已经收过一次"
那条路再调一遍无害 —— 属主可以把失败路径写成一条。

> **属主要在 `set_loop_exit_hook()` 里收句柄，`rollback_loops()` 只负责停循环。**
> 这两件事分开是刻意的：本库不认识属主的句柄，硬关一遍只能靠 `uv_walk()`，那会把属主
> 的包装对象一起请走（UAF）。

---

## 5. 读的三条路

| 入口 | 在哪 | 回调 | 给你什么 |
|---|---|---|---|
| `read_start(cb)` | 客户端 `:432` | `void(uvcpp_buf*)` | 裸数据，**分不出对端关闭** |
| `read_start(nullptr)` | 客户端 | —— | 只为打开内部接收缓存给 `read_wait()` 用 |
| `read_start_events(cb)` | 客户端 `:446` | `void(uvcpp_tcp_client&, const net_read_result&)` | **带语义的事件** |
| `set_read_callback(cb)` | 服务端 `:232` | 同上 | 一份回调覆盖所有连接 |

**`read_start` 与 `read_start_events` 互斥。** 用过其中一个再用另一个，拿到
`UV_EALREADY`。挡在前面的理由写在实现里（`src/net/uvcpp_tcp_client.cpp:1885-1893`）：
两条路共用同一个底层 stream，同时注册的话底层 `read_start` 会把先注册的那个
**静默覆盖**掉——用户以为两个回调都在收数据，实际只有一个。

错误码要说清，因为三种错法给的东西不一样：

| 调用 | 情况 | 结果 |
|---|---|---|
| `read_start(cb)`，cb 非空 | 已有异步读 | 返回 `UV_EALREADY` |
| `read_start(nullptr)` | 已有异步读 | **抛 `std::runtime_error`** |
| `read_wait(...)` | 已有异步读 | **抛 `std::runtime_error`** |
| `read_start_events(nullptr)` | 任何时候 | 返回 `UV_EINVAL` |

**在服务端上不要自己再注册读。** 设了 `set_read_callback` 之后，每个新连接由框架自动
`read_start_events()`；你在 `listen` 的回调里再 `read_start()` 会拿到 `UV_EALREADY`
（`src/net/uvcpp_tcp_server.h:482-484`）。反过来说，**设它之前**在连接回调里注册的读
优先级更高，会被保留。

---

## 6. net_read_result 的事件契约

```cpp
// doc-snippet: fragment — 结构体定义摘录（这里省掉了默认构造函数和两个判据，正文
// 紧接着就讲它们）。本页要的是成员语义一眼可见，不是能编的最小定义。
struct UVCPP_API net_read_result {
  net_read_event event;
  const char*    data;   // DATA 时有效，其余是 nullptr
  size_t         size;   // DATA 时的字节数（二进制安全，可能含 NUL）
  int            error;  // READ_ERROR 时的 libuv 错误码（负值）

  bool is_data() const;  // event == DATA
  bool is_end() const;   // event != DATA
};
```

只有 `is_data()` 和 `is_end()` 两个判据，**没有** `is_error()` / `is_peer_closed()`，
那两种要比 `event`：

```cpp
#include <cstdio>

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>

void doc_dispatch(uvcpp::uvcpp_tcp_client& client,
                  const uvcpp::net_read_result& r) {
  (void)client;
  if (r.is_data()) {
    std::fwrite(r.data, 1, r.size, stdout);
  } else if (r.event == uvcpp::net_read_event::PEER_CLOSED) {
    // 对端发了干净的 FIN —— 这是一次正常收尾，不是错误
  } else {
    // READ_ERROR：错在 r.error 里
    std::fprintf(stderr, "read error: %d\n", r.error);
  }
}
```

三条必须知道的：

- **`data` 只在本次回调期间有效**（`src/net/uvcpp_net_read.h:64-66`）：缓冲区回调返回后
  就被复用/释放。要留着就自己拷走。
- **回调跑在事件循环线程上**（`:95`）。别在里面做重活，用 `uvcpp_work`。
- **枚举名故意不叫 `ERROR` / `EOF`**（`:49-51`）：`<windows.h>`（经 `<uv.h>`）会
  `#define ERROR 0`，`EOF` 是 `<cstdio>` 的宏。所以是 `READ_ERROR` / `PEER_CLOSED`。

收到 `PEER_CLOSED` / `READ_ERROR` 之后**这个回调不会再被调用**，框架随后的收尾
（关闭回调）照常发生，所以**不要在回调里做释放**（`src/net/uvcpp_tcp_client.h:486-487`）。

---

## 7. 连接归谁管

**默认服务端全包**：每条新连接被登记、并配一个关闭管理器，连接关闭时把它从服务端
摘除并 `delete`。这件事**无条件发生**——你自己 `set_on_close()` 的那个槽位纯观察，
取消不了它（`src/net/uvcpp_tcp_server.h:55-58`）。

所以：

- **不要在关闭回调里 `delete` 客户端**，除非你已经用 `take_client()` 把所有权取走。
  删一个仍归框架管的客户端会让框架随后二次释放（`src/net/uvcpp_tcp_client.h:567-569`）。
- 想自己管，用 `take_client()` 取走、`return_client()` 交回。两个都**必须在 loop
  线程调用**；交回之后**不要再持有那个指针**。
- 关掉一条连接之后也别再留指针：框架可能在完成回调里把它删掉。

**"对端断开了"这件事只有读得见。** `nread < 0` 那条分支是唯一能发现它的地方
（`src/net/uvcpp_tcp_server.h:59-65`）——不读的连接，即使设了 `set_on_close()` 也
**永远不会被触发**，而且这条连接不会被释放，等于每条一个静默泄漏。只想知道死活、
不要数据的话，就用 `read_start_events()` 注册一个忽略数据的回调
（`src/net/uvcpp_tcp_client.h:555-559`）。

服务端有个 `set_auto_read`（默认 **true**）。关掉它只在"你要完全接管读路径、并且
自己负责发现断开"时有意义；关掉又没设回调时，服务端会给每条连接往 stderr 打一行警告
（`src/net/uvcpp_tcp_server.cpp:959-966`）。

---

## 8. 写

客户端的全部写入口（`net/uvcpp_tcp_client.h`）：

```cpp
// doc-snippet: fragment — 重载清单，不是完整翻译单元。这一节的要点就是七个重载
// 并排对比默认值的有无，包成函数体反而看不出来。
int write(const char* data, size_t len, std::function<void(int)> cb = nullptr);
int write(const uvcpp_buf& buf, std::function<void(int)> cb = nullptr);
int write(uvcpp_buf* buf, std::function<void(int)> cb = nullptr);
int write(const char* head, size_t head_len, uvcpp_buf* body,
          std::function<void(int)> cb);          // 这一条 cb 没有默认值

int write_wait(const char* data, size_t len, int timeout_ms = 30000);
int write_wait(const uvcpp_buf& buf, int timeout_ms = 30000);
int write_wait(uvcpp_buf* buf, int timeout_ms = 30000);
```

**缓冲区的所有权按重载分三类**：

| 重载 | 所有权 |
|---|---|
| `const uvcpp_buf&` | **自动拷贝**，调用方保留，原 buf 不变（`:344-348`） |
| `uvcpp_buf*` | **零拷贝、转移负载**：数据被移走，调用后那个 `uvcpp_buf` 是**空的**；对象本身仍归你（`:353-358`） |
| `write(head, head_len, body, cb)` | `body` 被消费；**两种情况下都不许在回调之前改写 `*body`**（`:383-397`） |

传 `uvcpp_buf*` 时别传临时量的地址，指针必须活到写回调触发（异步）或函数返回（同步）。

---

## 9. 同步与异步

**同一个操作上同步与异步不能混用**，混了就抛 `std::runtime_error`
（`src/net/uvcpp_tcp_client.h:76-78`）。具体是：

| 情况 | 结果 |
|---|---|
| 已经有异步写在飞，再调 `write_wait` | 抛 `std::runtime_error` |
| 已经有异步读，再调 `read_wait` / `read_start(nullptr)` | 抛 `std::runtime_error` |
| 已经有异步写在飞，再调异步 `write` | 返回 `UV_EALREADY` |
| 没连上就写 | 返回 `UV_ENOTCONN` |

**异步/同步其实是由 `cb` 是不是空决定的**（`src/net/uvcpp_tcp_client.cpp:1137-1140`）：
`cb` 非空 ⇒ 立即返回 0，完成时回调；`cb` 为空 ⇒ 退化成 `write_wait(data, len, 30000)`。

**这就是为什么回调里必须传 cb。** 事件循环回调里禁止同步等待，而三参数的
`write(data, len)` 省略第三个参数就是 30 秒的同步写。想让"发完不管"，传一个空 lambda。

---

## 10. UDP

UDP 没有"连接对象"：服务端 `recv_start` 交付数据报时带上来源。

```cpp
#include <cstdio>
#include <cstring>

#include <uv.h>
#include <net/uvcpp_udp_server.h>

int main() {
  uvcpp::uvcpp_udp_server server;

  int rc = server.bind("0.0.0.0", 9100);
  if (rc != 0) {
    std::fprintf(stderr, "bind: %d\n", rc);
    return 1;
  }

  rc = server.recv_start([](uvcpp::uvcpp_buf* buf, const char* ip, int port) {
    // buf / ip / port 都只在本次回调期间有效
    if (buf != nullptr && buf->size() > 0) {
      std::printf("from %s:%d, %zu bytes\n", ip, port, buf->size());
    }
  });
  if (rc != 0) {
    std::fprintf(stderr, "recv_start: %d\n", rc);
    return 1;
  }

  server.run();
  return 0;
}
```

- `recv_start` 的回调里那个 `uvcpp_buf*` 是**栈上的临时量**，负载是克隆出来的，
  底层读缓冲回调一返回就释放（`src/net/uvcpp_udp_server.cpp:177-201`）。**不要留那个指针。**
- `send(ip, port, data, len, cb = nullptr)` 有**同一个 nullptr 陷阱**：省略 `cb` 就是
  同步发送。
- 客户端侧另有 `recv_wait(out_buf, timeout_ms)` 和 `set_max_read_cache_size()`，
  同步与异步同样是二选一。

---

## 11. TLS 接线

TLS 不是这一层的功能，是**挂在 `uvcpp_tcp_client` 上的过滤层**——但开关在服务端这边：

```cpp
#include <cstdio>

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

int main() {
  uvcpp::uvcpp_ssl_context ssl_ctx(uvcpp::tls_mode::SERVER,
                                   uvcpp::tls_version::TLS_1_2);
  if (!ssl_ctx.is_ready()) return 1;
  if (!ssl_ctx.generate_self_signed("localhost", 2048)) return 1;
  if (!ssl_ctx.check_private_key()) return 1;

  uvcpp::uvcpp_tcp_server server;

  // 裸指针、非拥有：ssl_ctx 必须活得比整个 server 长
  server.set_ssl_context(&ssl_ctx);

  server.set_read_callback([](uvcpp::uvcpp_tcp_client& c,
                              const uvcpp::net_read_result& r) {
    if (r.is_data()) {
      c.write(r.data, r.size, [](int) {});   // 收到的是明文
    }
  });

  int rc = server.bind("127.0.0.1", 8443);
  if (rc != 0) return 1;
  rc = server.listen([](uvcpp::uvcpp_tcp_client*) {}, 128);
  if (rc != 0) return 1;

  server.run();
  return 0;
}
```

装上之后**对外说的仍然是明文**：`write()` 收明文、读回调交明文，密文只在内部与
socket 之间流动（`src/net/uvcpp_tcp_client.h:193-205`）。所以 `web/` 那层一行都不用改。

三个例外：

- **`uvcpp_buf*` 的零拷贝在 TLS 上不成立**——加密要求明文过 `SSL_write`，它会拷一份，
  所以调用之后那个 `uvcpp_buf` **仍然是满的**，数据仍归它（`src/net/uvcpp_tcp_client.h:399-402`）。
- **握手完成前 `write()` 必然失败**，返回 `UV_ENOTCONN`。
- **握手失败的连接根本不会被交出来**：`on_connection` 一次都不调，只记在
  `last_error_code_` 里（`src/net/uvcpp_tcp_server.h:605-613`）。所以明文直连 TLS 端口时，
  上层"没被通知过"这条连接——这是有意的。

握手期连接不在任何上层登记表里，所以另有 `set_tls_handshake_timeout_ms()`
（默认 10000，`0` = 不设）防"喂 ClientHello 拖死"。

细节（证书怎么装、校验模式、ALPN）见 [TLS 与证书指南](./ssl-guide.md)。

---

## 12. 错误处理

- **`int` 返回：0 成功，失败是 libuv 的负错误码**（`src/net/uvcpp_tcp_server.h:155`）。
  最后一个是粘性的，`get_last_error()` 拿。
- 失败同时会置状态位（`TCP_SERVER_ERROR` / `TCP_CLIENT_ERROR`），`has_status()` 查。
- **同步/异步混用是抛异常**，不是错误码——见 §9。这是本模块唯一会抛的地方。
- `take_client()` 返回 `nullptr` 表示**所有权没变**（不是"出错了"）。

---

## 13. 典型坑

**`write(data, len)` 省掉第三个参数 = 同步写 30 秒。** 回调里这么写就是阻塞事件循环。
要"发完不管"就传一个空的 lambda 当第三个参数（正文里写不出那串字面量：反引号里的
`[]` 紧跟 `(` 会被文档门禁当成一条 markdown 链接）。

**在 `set_read_callback` 之后又在连接回调里 `read_start()`** ⇒ `UV_EALREADY`。

**不读的连接发现不了对端断开**，也不会被释放——一条静默的每连接泄漏。见 §7。

**在关闭回调里 `delete` 服务端管理的客户端** ⇒ 二次释放。

**`set_read_callback` 在 `listen()` 之后调不报错**，只是对已有连接无效。

**`net_read_result::data` 出了回调就没了**，别存那个指针。

**服务端示例里的 `uvcpp_buf*` 回调**（UDP）拿到的是栈上临时量，同样别存。

---

## 14. 没做的（如实列出）

- **没有 `on_connection` / `on_data` 这类 setter**。连接回调是 `listen()` 的第一个
  参数，数据回调是 `set_read_callback()`（服务端）或 `read_start*`（客户端）。
  旧文档里出现过的 `server.on_connection(...)` 在这个类上**不存在**。
- **`net_read_result` 只有两个判据**（`is_data` / `is_end`），没有 `is_error()`。
- **没有内置的背压/写队列上限**。异步写在飞时再写返回 `UV_EALREADY`，怎么排队是
  使用者的事。
- **没有连接池、没有自动重连**。`webapp` 的 WS 客户端有重连，这一层没有。
- **UDP 没有多播的封装**——`uvcpp_udp`（`handle/`）上有，这一层没透出来。
- **TLS 的 `PEER_STRICT` 只在客户端上多做事**：它会拿 `connect()` 收到的那个名字去
  校验对端证书的主机名，`PEER` 不会（见 [TLS 与证书指南](./ssl-guide.md)）。服务端上
  两者仍等价 —— 服务端没有可校验的名字。
- **多循环两端都已落地**：net 层 `uvcpp_tcp_server::set_loops()`（§4）与框架层
  `uvcpp_web_app::set_loops(n)` 都有了，后者配套 `loop_count()` /
  `connection_count_at(int)`；注意 `connections()` 只返回**本循环**那一份。客户端与
  UDP 两侧都没有多循环，细节见[多循环设计](./multiloop-design.md)。

---

相关文档：[低层指南](./lowlevel-guide.md)、[TLS 与证书指南](./ssl-guide.md)、
[webapp 应用框架开发者指南](./webapp-guide.md)、[项目 README](../README.zh.md)。

本页用到的头文件：`<net/uvcpp_net_read.h>`、`<net/uvcpp_tcp_server.h>`、
`<net/uvcpp_tcp_client.h>`、`<net/uvcpp_udp_server.h>`、`<ssl/uvcpp_ssl_context.h>`。
