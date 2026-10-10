# QUIC 传输层指南

`src/quic/` 是 **net 层**上的一条**链路协议** —— QUIC（RFC 9000），后端是
[ngtcp2](https://github.com/ngtcp2/ngtcp2)。它的位置与 `src/http2/` 对 HTTP/2 完全
相同：协议栈归上游，本库只做事件循环、缓冲与生命周期的接线。它是 `src/http3/` 的
地基 —— **HTTP/3 那一层已经落地**（1.4.1，见 [`doc/http3-guide.md`](./http3-guide.md)），
但**它自己不是 HTTP**：这一层不认识请求、响应、流上面的任何语义。

> ## 1.4.1 起它是一条真能通信的链路协议
>
> 握手、流收发、连接关闭与空闲超时**都通了**：一条 UDP 口上真起得来连接，一条
> 连接上真跑得动多条流，两边真能互发字节。这些不是"设计好了"，是三个功能用例
> 里的断言量出来的（`quic_handshake_func.cpp` 22 条、`quic_stream_func.cpp`
> 33 条、`quic_api_func.cpp` 81 条）。
>
> **没做的**是 0-RTT、连接迁移、无状态重置、datagram（RFC 9221）与 multipath
> —— 逐条列在下面 [§8](#8-没做的如实列出)，别在别处另维护一份。
>
> 1.4.1 顺带把三样只对上层有意义的东西从内核接了出来（FIN 与 RESET 分开报、
> 流额度事件、STOP_SENDING）—— 它们的调用方是 HTTP/3，所以**形状的说明在
> [`doc/http3-guide.md`](./http3-guide.md) §2**，本页只记"接出来了"这件事与它在
> 哪条断言上（[§4](#4-141-交付了什么)）。
>
> 1.4.1 之前在公开头上挂着的那一片 `@warning` 已经整片摘掉了：它们描述的是
> "返回 `UV_ENOSYS`、回调一次都不跑"的骨架，而那个契约是被
> `tests/functional/quic_api_func.cpp` **钉住**的 —— 实现落地时那些断言确实红了，
> 逼着实现者回来把契约显式改成真的，而不是让"框架写好了"这句话悄悄变成假的。
> 现在它们测的是**实现**：没挂到连接上的壳返回 `UV_ENOTCONN`、没设 TLS 上下文的
> 端点返回 `UV_EINVAL` 且一条回调都不许排。

- 打开方式：`-DUVCPP_ENABLE_QUIC=ON`。**默认 OFF**（`CMakeLists.txt:95`），而且
  下面三种情况会被**强制**置 OFF 并打 warning，而不是留一个"能配置、链不上、
  跑不起来"的组合：
  1. 没开 OpenSSL（`CMakeLists.txt:513-513`）—— QUIC 建在 TLS 1.3 上，ALPN 是 TLS
     扩展，**没有明文 QUIC** 这回事；
  2. 没开 net 层（`CMakeLists.txt:520-520`）—— QUIC 是 net 层的一条链路协议，并且复用
     `src/net/uvcpp_net_read.h` 的读事件契约；
  3. OpenSSL 找得到、但**不带 QUIC API**（`CMakeLists.txt:566-566`）—— 判据见
     [§2](#2-编译期条件一份带-quic-api-的-openssl--32)。
- 包含方式：`<quic/uvcpp_quic_client.h>`、`<quic/uvcpp_quic_server.h>`、
  `<quic/uvcpp_quic_connection.h>`、`<quic/uvcpp_quic_common.h>`。私有的
  `<quic/uvcpp_quic_ngtcp2.h>` 与 `<quic/uvcpp_quic_session.h>` **都不安装**
  （`CMakeLists.txt:2299-2299`）—— 理由见 [§7](#7-典型坑) 第一条。
- 四个公开头**全部**整段套在 `#if UVCPP_QUIC_ENABLE` 里，所以**不开关就一个类都
  看不到**。这与 `web/`、`ssl/`、`http2/`、`http3/`、`wsdl/` 同档。

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一版没做"的地方都明确
> 标出来 —— 那些地方比 API 更容易踩。

> **吞吐不在这一页。** 这里讲契约，不讲速率。大载荷的**量具、读数与前提**单开在
> [benchmark-rig.md](./benchmark-rig.md) 的「QUIC 吞吐量具」一节；同处还记着 issue #34
> 那次大载荷崩溃的根因（ngtcp2 要求交给它的那段数据"原样留着"，而当时的发送缓冲
> 既会扩容搬地址、也会丢前缀搬字节）与修法（分块发送队列）。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [编译期条件：一份带 QUIC API 的 OpenSSL ≥ 3.2](#2-编译期条件一份带-quic-api-的-openssl--32)
3. [怎么拿到那份 OpenSSL](#3-怎么拿到那份-openssl)
4. [1.4.1 交付了什么](#4-141-交付了什么)
5. [公开 API 形状](#5-公开-api-形状)
6. [后端探针与错误码](#6-后端探针与错误码)
7. [典型坑](#7-典型坑)
8. [没做的（如实列出）](#8-没做的如实列出)

---

## 1. 这一层是什么

| 类 | 头 | 角色 |
|---|---|---|
| `uvcpp_quic_client` | `src/quic/uvcpp_quic_client.h:57` | 客户端**端点**：一条 UDP 口 + 它名下的那条连接 |
| `uvcpp_quic_server` | `src/quic/uvcpp_quic_server.h:57` | 服务端**端点**：一条（或多条）UDP 口 + 它上面**所有**连接 |
| `uvcpp_quic_connection` | `src/quic/uvcpp_quic_connection.h:58` | 一条 QUIC 连接：状态机、流、CID、ALPN |
| `net_read_result` / `net_read_event` | `src/net/uvcpp_net_read.h:68` | **复用 net 层**的读事件语义（不是本模块新造的） |

**为什么是三个类而不是一个。** `uvcpp_tcp_client` 一个对象既是 socket 又是连接，
因为 TCP 的一条连接从头到尾就是一条 socket。QUIC 不是：客户端的首包之前连对端地址
都还不知道，服务端的**一条** UDP 口背后可以同时有成千上万条连接（QUIC 的连接由
**CID** 标识，不由四元组标识），而且握手完成后连接还可能因为路径迁移换 socket。
所以"端点"（管 socket 与监听）与"连接"（管状态机、流、CID）在这里必须分开 ——
这不是命名习惯问题。

**读事件为什么复用 net 层那套。** QUIC 的一条流在应用层看到的形状与一条 TCP 连接
**逐字相同**：有数据 / 对端收了 / 读出错，连"`nread < 0` 时原始回调什么都不给、
于是用户写的读循环普遍是错的"这个坑都一模一样。再造一套平行的事件枚举，只会让上层
为两套名字相同的语义各写一遍分支，而且两套迟早漂移。复用落在**语义**那一层
（`net_read_result` / `net_read_event`）；回调的**第一个参数**按本层自己的类型走，
因为 `uvcpp_net_read_cb` 那个 typedef 写死了 `uvcpp_tcp_client&`
（`src/quic/uvcpp_quic_common.h:110-138` 有完整理由）。

**它不属于 web 层。** 所以 `CMakeLists.txt:95` 那个 `option()` 刻意**不**放在
"Web 子开关"那一组里 —— 那组的标题写着"仅在 `UVCPP_BUILD_WEB=ON` 时有效"，而
QUIC 在 web 关闭时照样要能用。这正是下面 [§3](#3-怎么拿到那份-openssl) 要单独说
一遍的原因：QUIC 逼出来的那个组合是"**net 打开 + SSL 打开 + web 关闭**"，而在
1.4.1 之前，这个组合是坏的。

### 1.1 这个组合以前是坏的（本版修掉了）

OpenSSL 的发现块（`find_package(OpenSSL)` → `UVCPP_SSL_LIBS`）原先**嵌在
`if(UVCPP_BUILD_WEB)` 里面**。于是 `-DUVCPP_ENABLE_OPENSSL=ON
-DUVCPP_BUILD_WEB=OFF` 会把 `src/ssl/` 编进去，却从不定义 `UVCPP_SSL_LIBS` ——
`UVCPP_SSL_LIBS` 为空时那条 `target_link_libraries()` 会展开成一个**静默的空操作**，
于是症状不是配置期报错，而是**链接失败**（受害面还不止 QUIC：net 层自己的 TLS
客户端路径一起坏）。

1.4.1 把那段发现块**平移到 web 块之外**，净行长不变。QUIC 正好住在这个组合里，
所以这个 bug 是被 QUIC 逼出来的；四个平台文件里的 `quic` 格（ubuntu / macOS /
Windows MSVC 各一格）是**唯一**覆盖它的那几格 —— 每一格都是 SSL 开、web 关。

---

### 1.2 Windows 上的发送侧 UDP GSO/USO（1.5.2）

**这笔账。** Windows 上 libuv 发一个 UDP 数据报就是一次 `WSASendTo`
（`src/win/udp.c` 里 `uv__udp_try_send2()` 是个纯 `for` 循环），于是 QUIC 的每个
数据报都自带一次系统调用。Linux 上这笔账**上游已经塌缩掉了**：`src/unix/udp.c` 的
`uv__udp_sendmsg()` 每批 20 个请求折成一次 `sendmmsg()`，而触发它的正是本库 QUIC
那种连发形状 —— 所以这不是"libuv 慢"，是"Windows 那一侧少一条路"。

**做法。** 一次 `WSASendTo` 交一批**等长**数据报给协议栈，由它按
`UDP_SEND_MSG_SIZE`（选项号 2，`IPPROTO_UDP`）切开送出去。切在内核里做，所以
**对端看到的还是一个个正常数据报** —— 变的只有系统调用次数。这条机制本身在原生
UDP 地板上量过（`doc/benchmark-rig.md`）：`--pkt=1434` 时 11.19 µs/包 →
2.99 µs/包，而同期线上数据报数 1462 → 1473，**一个没少**。

**本库上的实测（同会话交错 A/B，各 4 轮 × 15 rounds，钉核 mask=1）。** 同一台机上交错
跑 `UVCPP_ENABLE_UDP_GSO=OFF` 与 `=ON` 两个二进制，**每一轮把两条臂的先后顺序翻过来**
（装置在仓库外，`_probe/gso_ab.py` 的 `interleave()`）—— 固定"先 off 后 on"的话，任何"第二个跑的那个
更暖"的效应（频率爬升、页缓存、CPU boost 掉回去）都**恒定偏向 ON**，而它长得和真实收益
一模一样；取各臂最小值削不掉系统性偏置，翻顺序是免费的。

| 轴 | 尺寸 | 墙钟 OFF/ON | cyc/B | 线上数据报数 |
|---|---|---|---|---|
| push（单向） | 2 MiB | **1.345×** | 1.332× | 噪声内 |
| push（单向） | 32 MiB | **1.332×** | 1.343× | 噪声内 |
| echo（双向单流） | 2 MiB | **1.341×** | 1.340× | 噪声内 |
| echo（双向单流） | 32 MiB | **1.342×** | 1.331× | 噪声内 |

墙钟与 `cyc/B`（每字节周期数，bench 自报）是两条独立读数，四个格子上给出的倍数一致，
所以这不是某一条量具的偏置。**"线上数据报数没变"用的是 bench 自报的 `recv_calls`**
（接收侧调用次数，起 GSO 完全不影响它）：两边**逐轮自己的跨度比组间差还大**（32 MiB
push 一例：本臂跨度 67、组间差 42），所以判据落在噪声里 —— 注意这里没写成"两个数必须
逐字相等"，那样写会自己变红（`recv_calls` 本身逐轮就飘 ~0.2%）。

**同会话的对照臂是裸 UDP 地板**（`--mode=udpfloor`，一个数据报一次 `uv_udp_try_send`、
不开 USO）：ON 的 QUIC 只要它的 **0.650×**（2 MiB）/ **0.689×**（32 MiB）。QUIC 干的活
**严格多于**裸 UDP，比地板还快只能是发送机制本身换了 —— 这是"系统调用真的塌了"的自证，
也是本笔唯一不依赖"墙钟差异"的独立证据（读数是 `_probe/gso_ab/run4.txt`）。

**开关。** `UVCPP_ENABLE_UDP_GSO`（宏 `UVCPP_UDP_GSO_ENABLE`）。默认值是**派生**的：
Windows 且 `UVCPP_ENABLE_QUIC=ON` → `ON`，其余一律 `OFF`。非 Windows 上还有一层
**降级**：显式传 `-DUVCPP_ENABLE_UDP_GSO=ON` 也会被按回 `OFF`（配置期报一行 note），
所以这个宏在非 Windows 上**恒为 0、整段代码不编进去** —— 是"不生效"，不是"编进来
但是死的"。派生值必须算在 QUIC 的**静默降级链之后**（`CMakeLists.txt:577-577` 之后），
否则一条"QUIC 已被降级掉"的配置会把 GSO 打开。

**两层闸门。**

1. **编译期** `UVCPP_UDP_GSO_ENABLE`：为 0 时 `do_flush()` 走的分支与今天逐字节
   相同 —— 「关闭 == 今天」是结构性的，不靠 review。
2. **运行期** `quic_session::gso_`：`init_client()`/`init_server()` 末尾（也就是
   `bind()` 之后）拿 `kDatagramBufLen`(1500) 探一次 —— 那个值**大于** PMTUD 探测表
   最大的一档 1444，所以探测本身不会切开任何一个包，只为回答"这台机器支不支持"；
   真正发的时候按**本次 agg2 回填的 `*pgsolen`** 重设（见下），**值变了才去撞
   系统调用**，所以一次大批量传输里通常只设一两次。重设失败就把 `gso_` **永久**
   置假，并把这一批按 `pgsolen` 切开逐段发 —— **绝不**在选项没设对时把聚合缓冲
   当一个大包发出去。

**分段尺寸为什么只能取 `*pgsolen`。** `UDP_SEND_MSG_SIZE` 是"每段多少字节"，而
PMTUD 会把 path MTU 从 1200 抬到 1444：设大了协议栈**切在包中间**，设小了把完整的
包切碎。而 agg2 的源码保证了"一旦继续聚合，所有非末包长度都**等于** `*pgsolen`"，
且它 == `ngtcp2_conn_get_path_max_tx_udp_payload_size2()`（`ngtcp2:lib/ngtcp2_conn.c:14360`
那条 `if`）。所以"值 = 本次回填的 `*pgsolen`"是唯一无假设的正确做法。

**聚合缓冲按上限开，不按此刻量到的值。** `ngtcp2_conn_get_send_quantum2()` 在握手
刚开始时报的是 `10 × max_udp_payload_size` = 12000（`ngtcp2:lib/ngtcp2_cc.c:53` 覆盖掉了
`ngtcp2:lib/ngtcp2_conn.c:923` 那个 64 KiB），然后随 PMTUD 上抬。缓冲要是按**当前值**开，
聚合就被压在 8 个包上下（本机实测过这个形状）。开成 64 KiB 上限（`kGsoBufLen`，
`uvcpp_quic_session.cpp:133`），让 ngtcp2 自己那条 `ngtcp2_min(buflen,
send_quantum)`（`ngtcp2:lib/ngtcp2_conn.c:14304`）去限才是对的形状。下限仍是 `kDatagramBufLen`：
agg2 里有 `assert(buflen >= path_max_udp_payloadlen)`。

**只做发送侧一半，而且这是有意的。** 收方向要 `UDP_RECV_MAX_COALESCED_SIZE` +
`WSARecvMsg` + `UDP_COALESCED_INFO`，而 `uv_udp_recv_start` 够不着控制消息。
**半开比不开更糟**：对端把几个数据报粘在一起，libuv 会把它们当成**一次**读交给
应用 ⇒ 静默的内容损坏。要做得自己实现整条收包路径，那是另一笔活（本版不做）。

**坑一：`uv_fileno` 要 `bind()` 之后才有 socket。** libuv 的 UDP 句柄是**惰性**
建 socket 的 —— `uv_udp_init`（AF_UNSPEC）之后 `handle->socket` 还是
`INVALID_SOCKET`，这时 `uv_fileno` 返回 `UV_EBADF`，`setsockopt` 无从谈起。本机实测：
bind 前 `fileno rc=-4083 sock=-1`，bind 后 `rc=0 sock=628`。所以探测点落在
`init_client()`/`init_server()` 的末尾（`setup_gso()`，`uvcpp_quic_session.cpp:923`），
不是建对象的时候。

**坑二：`setsockopt` 成功 ≠ 协议栈真会切。** 本机实测 512…65483 **全都返回成功**，
包括远超 MTU 的值 —— 所以那个返回值只能当"这台机器认得这个选项号"，不能当"它真的
在切"。这条残余风险靠用例挡，不靠探测：`tests/functional/quic_gso_func.cpp` 的
第 1 部分在**裸 UDP** 上单独量这条机制（对照组是同一个 `setsockopt` 从不设的
socket），第 2 部分跑一次 192 KiB 的 QUIC 传输逐字节比对。第 1 部分是**环境**判据
（这一档该切却没切就红），第 2 部分加变异对照才是库侧的判据。

---

## 2. 编译期条件：一份带 QUIC API 的 OpenSSL ≥ 3.2

QUIC 的版本协商走 TLS 的 ALPN 扩展，所以 QUIC **必须**有一份支持 QUIC 的 TLS 库。
libngtcp2 支持两个分支，判据写在它自己的 `CMakeLists.txt` 里，顺序是 load-bearing 的：

| 先查到 | 分支 | libngtcp2 建出来的 crypto 目标 |
|---|---|---|
| `SSL_provide_quic_data` | quictls / LibreSSL（`HAVE_QUICTLS`） | `ngtcp2_crypto_quictls_static` |
| 上面那个没有、`SSL_set_quic_tls_cbs` 有 | mainline OpenSSL ≥ 3.2（`HAVE_OSSL`） | **`ngtcp2_crypto_ossl_static`** ← 我们链的是这个 |
| 两个都没有 | — | 一个都不建，configure 期 `FATAL_ERROR` |

本库的探针**复刻**了这个顺序（`CMakeLists.txt:556-556`、`CMakeLists.txt:559-559`）。这不是
仪式：只查 `SSL_set_quic_tls_cbs` 的探针会在 quictls 树上"通过"，然后去链一个**从来
没被建出来**的目标。所以探针的结论是"没有 provide、但有 cbs"才放行
（`CMakeLists.txt:566-566`）；另外两格都强制关闭并出声。

> **Ubuntu 24.04 自带的 3.0.13 不行。** 3.0/3.1 没有那两个符号中的任何一个。这不是
> 保守估计 —— 对着本机那份 `libssl.so.3` 量过：两个符号数都是 0。

---

## 3. 怎么拿到那份 OpenSSL

一份 3.2+（本仓 CI 用的是 3.5.0）的 mainline OpenSSL，编到某个前缀：

```bash
curl -sSL -o openssl-3.5.tar.gz \
  https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/openssl-3.5.0
mkdir -p _local_deps/openssl-3.5 && tar xzf openssl-3.5.tar.gz \
  -C _local_deps/openssl-3.5 --strip-components=1
cd _local_deps/openssl-3.5 && ./Configure --prefix=$PWD/../openssl-3.5-inst \
  --libdir=lib shared && make -j"$(nproc)" && make install_sw
```

然后把它指给 CMake：

```bash
cmake -S . -B build-quic \
  -DUVCPP_BUILD_TESTS=ON -DUVCPP_BUILD_NET=ON -DUVCPP_BUILD_WEB=OFF \
  -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_QUIC=ON \
  -DOPENSSL_ROOT_DIR=$PWD/_local_deps/openssl-3.5-inst
```

`_local_deps/` 在 `.gitignore` 里（`_local_*/`），**不入库** —— 与 libuv / llhttp /
nghttp2 / zlib / pugixml 的用法一致。

配置成功的标志是两行 `message(STATUS)`，CI 也正是 grep 这两行：

```
ngtcp2 integrated (tag=v1.25.0, static)          # CMakeLists.txt:734-734
Including quic module in build (ngtcp2 v1.25.0)  # CMakeLists.txt:1505-1505
```

> **只 grep `CMakeCache.txt` 是不够的。** 三条降级用的都是**普通变量**
> `set(UVCPP_ENABLE_QUIC OFF)`，不是 cache 写，所以降级之后 cache 里仍然是
> `UVCPP_ENABLE_QUIC:BOOL=ON`。一个"被降级了"的配置和一个"真开了"的配置，在 cache
> 那一层长得一模一样。这就是上面这两行、以及"`ctest -N` 里真有
> `test_quic_api_func`"三者必须一起看的原因。

**离线/本地可复现**：ngtcp2 也走 FetchContent，本地可以用
`-DFETCHCONTENT_SOURCE_DIR_NGTCP2=<一份已下好的 ngtcp2 源码>` 指过去绕开下载。

**只压住 ngtcp2 那一批开关就够**：`ENABLE_LIB_ONLY=ON` 让 ngtcp2 跳过它自己的
`examples/` 与 `tests/`，`third-party/` 整段挂在 `if(LIBEV_FOUND AND
LIBNGHTTP3_FOUND)` 下不会建。所以 **libngtcp2 既不需要 nghttp3，也不需要 libev** ——
nghttp3 由 HTTP/3 那一层自己接（`CMakeLists.txt` 的 http3 依赖块），QUIC 这条路
不需要它，也**不该**为了 QUIC 去接它。

---

## 4. 1.4.1 交付了什么

| 东西 | 状态 | 判据在哪 |
|---|---|---|
| CMake 接线（开关、三守卫、QUIC API 探针、FetchContent、PIC 断言） | **真实现** | `check_ci_layout.py` 的 `FEATURE_GATES["quic"]` |
| 依赖接入（`ngtcp2_static` + `ngtcp2_crypto_ossl_static`，静态链入） | **真实现** | `quic_api_func.cpp` 的三条链接证据 |
| 配置契约宏 `UVCPP_QUIC_ENABLE`（生成头 + `_uvcpp_literal01` + PUBLIC 编译定义三处一致） | **真实现** | `check_config_contract.py` |
| **握手**（Initial → TLS 1.3 → ALPN → 1-RTT） | **真实现** | `quic_handshake_func.cpp` |
| **开流、收发**（流控、重传、丢包恢复由 ngtcp2 驱动，本层不自己实现） | **真实现** | `quic_stream_func.cpp` |
| **连接关闭（两端各自的 CONNECTION_CLOSE）与空闲超时** | **真实现** | `quic_api_func.cpp` §5 |
| **服务端的 CID 路由**（握手要跨好几个包，路由不对就握不上） | **真实现** | `quic_handshake_func.cpp` + `quic_stream_func.cpp` |
| `uvcpp_quic_server::bind()` 系列**参数校验** | **真实现**（登记；socket 在 `listen()` 里建） | `quic_api_func.cpp` §4 |
| `set_ssl_context()` / `set_alpn_protos()` / `set_alpn_select_protos()` | **真实现** | `quic_handshake_func.cpp`（ALPN 真协商成 `"h3"`）、`quic_api_func.cpp` §4（缺上下文 → `UV_EINVAL`） |
| `set_idle_timeout()` | **真实现** | `quic_api_func.cpp` §5（300ms 到期，两侧 `NGTCP2_ERR_IDLE_CLOSE`） |
| `run()` / `stop()` | **真实现**（转发到 loop） | `quic_api_func.cpp` §6（自建循环那一节） |
| `quic_ngtcp2_version_string()` / `quic_error_string()` / `quic_crypto_backend_init()` / `_free()` | **真实现**（真调进 ngtcp2） | `quic_api_func.cpp` §1 |
| 对端 reset 一条流（RESET_STREAM）时读侧的收尾事件 | **真实现** | `quic_stream_func.cpp` 第 3 段 |
| **FIN 与 RESET 分开报**（`net_read_result::fin`，连同 TCP 那四处构造点一起是真话） | **真实现** | `quic_api_func.cpp`（"DATA 的 fin == true"、"RESET-0 带 fin == false"） |
| **流额度事件** `on_streams_available` + `streams_left(bidi)` | **真实现** | `quic_api_func.cpp`（"没有内核时 streams_left==0"、"服务端的 streams_left(双向) > 0"） |
| **STOP_SENDING**（`on_stop_sending`）与 `shutdown_stream_read()` | **真实现**（1.4.1 补上的，原是 [§8](#8-没做的如实列出) 那条缺口） | `quic_api_func.cpp`（"客户端收到了对端的 STOP_SENDING"、"应用错误码原样传到"） |
| 连接迁移、0-RTT、无状态重置、datagram（RFC 9221）、multipath | **没有** | [§8](#8-没做的如实列出) |
| HTTP/3（nghttp3） | **不在本层**（1.4.1 落在 `src/http3/`） | [`doc/http3-guide.md`](./http3-guide.md) |

**"判据在哪"这一列不是装饰。** 上面每一格都指得到一条会因为它坏掉而变红的断言；
指不到的地方**不写**，而是列在下面。这一版**没有**判据的有两处，写出来免得被当成
已经量过：

- **丢包重传与丢包恢复没有被量过。** 那是 ngtcp2 的职责，本层只是把它的定时器与
  重发路径接起来，而三条用例里**没有一条真丢过包** —— "它真做了"这件事本仓没有
  证据，只有"它没坏到让我们看出来"。
- **CID 没有被换过。** 路由那一格量的是"跨多个包还能找到同一条连接"，而一个真的
  换 CID 的场景（对端按 `preferred_address` 换、或迁移）属于 [§8](#8-没做的如实列出)
  的迁移那一档，不存在。

一句话：**这一层现在是一条能握手、能开流、能收发、能干净收场的链路协议**；它上面
那层 HTTP/3 已经落地（[`doc/http3-guide.md`](./http3-guide.md)），但**它自己不
认识"请求"和"响应"** —— 那是上面那一层的活。

---

## 5. 公开 API 形状

### 5.1 客户端

```cpp
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_context.h>

#include <string>
#include <vector>

using namespace uvcpp;

// 配置 + 连接。**回调要在 connect() 返回之后、下一次循环迭代之前装上去** ——
// 理由见下面那条 warning。
int start_quic_client(uvcpp_ssl_context& tls, uvcpp_quic_client& cli) {
  cli.set_ssl_context(&tls);   // 生命周期必须覆盖整条连接，本对象不持有它
  cli.set_alpn_protos(std::vector<std::string>{"h3"});
  cli.set_idle_timeout(30000);  // 毫秒；0 = 不超时

  const int rc = cli.connect("127.0.0.1", 4433, [](int) {
    // 那个 0 是**握手完成**，不是"受理了"：这一刻起才谈得上发数据。
  });
  if (rc != 0) return rc;   // 负值 = 连开始都没开始，回调一次都不会跑

  uvcpp_quic_connection::callbacks cbs;
  cbs.on_alpn = [](uvcpp_quic_connection&, const std::string&) {
    // 进来的字符串就是对端选定的协议名；到这一刻 state() 才到 ESTABLISHED。
  };
  cbs.on_read = [](uvcpp_quic_connection&, int64_t,
                   const net_read_result&) {
    // 一条连接上有很多条流 —— 所以流号是回调参数，不是从别处查的。
  };
  cli.connection()->set_callbacks(cbs);
  return 0;
}
```

- 两个构造函数：`uvcpp_quic_client()`（**自建** loop，析构时关掉它）与
  `uvcpp_quic_client(uvcpp_loop*)`（**共享**外部 loop，析构时**不会**关它）——
  与 `uvcpp_tcp_client` 那条约定逐字相同。
- `connect()` 的**完成回调报的是握手完成**，不是"地址解析好了"：`cb(0)` 与
  `on_alpn` / `state()==ESTABLISHED` 是同一时刻。`connect()` 本身并不发包 ——
  第一个 Initial 包要等循环转起来才出去，所以"返回 0"只表示"这条路开始了"。
- **回调必须在 `connect()` 返回后、下一次循环迭代前装上。** 这既是安全的
  （那时还没有任何包进来），也是**唯一**的窗口：等 `cb(0)` 再装，握手期间发生的事
  （比如服务端侧那一刻的 `on_stream_open`）已经过去了。
- `set_alpn_protos({})`（空列表）**不等于回到默认**：默认是
  `quic_default_alpn()`，也就是 `"h3"`；空列表的含义是"一个候选都不发"，通常直接
  握手失败。
- `set_ssl_context(nullptr)` 是"清空"，而**不设就是没配**：QUIC 没有明文模式，
  所以缺上下文时 `connect()` 返回 `UV_EINVAL`，而不是退化成一个不加密的连接。
- `set_idle_timeout(ms)` 只在 `connect()` **之前**调有效 —— 握手一开始传输参数就
  发出去了，之后改只是改一个再也不会被读到的字段。

### 5.2 服务端

```cpp
#include <quic/uvcpp_quic_server.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_context.h>

#include <cstdio>

using namespace uvcpp;

// 端口用 0 让内核挑一个 —— 这是测试里发现端口的标准写法。
int start_quic_server(uvcpp_ssl_context& tls, uvcpp_quic_server& srv) {
  srv.set_ssl_context(&tls);
  srv.set_alpn_select_protos(std::vector<std::string>{"h3"});

  if (srv.bind("127.0.0.1", 0) != 0) return 1;   // 0 = 登记成功（真校验，但不建 socket）
  if (srv.listen([](uvcpp_quic_connection* c) {
        // 连接**刚建出来**（首包到达）就跑，不是握手完成时 —— 回调要在这一格里装。
        uvcpp_quic_connection::callbacks cbs;
        cbs.on_alpn = [](uvcpp_quic_connection&, const std::string&) {
          // 到这一刻起才谈得上收发。
        };
        c->set_callbacks(cbs);
      }) != 0) {
    return 2;
  }

  // listen() 之后它报的是**内核实际给的那个端口**，不再是登记的 0。
  std::printf("%s:%d\n", srv.configured_ip().c_str(), srv.configured_port());
  return 0;
}
```

- `bind()` / `bindIpv4()` / `bindIpv6()` **真的**校验：地址过一遍
  `uv_inet_pton()`，端口范围 `0..65535`；**先校验、后赋值**，所以失败时已经登记好的
  地址不变。域名**不收** —— UDP 的绑定要一个 `sockaddr`，"解析成本机接口地址"是另
  一件事（解析出多个地址选哪个？），不在这里猜。
- **socket 是在 `listen()` 里建的，不是 `bind()` 里。** 所以 `bind()` 返回 0 仍然
  **不等于**"这个端口归我了"—— 端口冲突要到 `listen()` 才暴露。访问器叫
  `configured_ip()` / `configured_port()` 而不是 `bound_*`，正是为了名字上不撒谎。
- 但 `configured_port()` 在 `listen()` **之后**报的是**内核实际分配**的端口：
  写 `bind("127.0.0.1", 0)` 让内核挑一个，然后拿这个访问器问出是哪一号 ——
  测试里没它就没法发现端口。
- `listen()` 在**首包到达、连接对象刚建出来**的那一刻跑 `connection_cb`，
  **不是**握手完成时。这与客户端的 `connect()` 相反，理由只有一个：`on_alpn` /
  `on_read` 这些回调要在它的下一行装上去，装晚了握手期间的事件全丢。
  要"能开始收发"这个点，看 `on_alpn` 或 `state()`。
- 服务端是 ALPN 的**选择方**：对端候选里没有一个落在 `set_alpn_select_protos()`
  列表里时，按 RFC 7301 应当以 `no_application_protocol` 告警结束握手，而**不是**
  挑一个双方都没承诺的协议名。
- **没有** `uvcpp_tcp_server` 那套多循环扇出（`set_loops()`）。QUIC 的多循环切分是
  按 CID 而不是按连接亲和度做的，形状不同 —— 这不是"暂时没写"，是**不打算照搬**。

### 5.3 连接

```cpp
#include <quic/uvcpp_quic_connection.h>

#include <cstdint>

using namespace uvcpp;

// 一次往返：开流、写、只关发送方向。
int one_round_trip(uvcpp_quic_connection& conn) {
  const int64_t id = conn.open_stream(/*bidi=*/true);
  if (id < 0) return 1;   // 负值就是错误码；NGTCP2_ERR_STREAM_ID_BLOCKED 是正常的一种

  if (conn.write_stream(id, "hello", 5, /*end_stream=*/true) != 0) return 2;
  // write_stream() 是**受理**：返回 0 只表示字节进了发送队列。
  // 真正"对端确认了"由 on_write(conn, id, 0) 报，一次调用对一次回调。

  // 只关发送方向（QUIC 的流是两个方向各关一次的）；读方向照常收。
  return conn.shutdown_stream(id) == 0 ? 0 : 3;
}
```

`uvcpp_quic_connection` 的公开面：

| 方法 | 1.4.1 的行为 |
|---|---|
| `set_callbacks(const callbacks&)` | 真实现（存值，随时可换） |
| `state()` | 真状态机：`IDLE → HANDSHAKING → ESTABLISHED → CLOSING → CLOSED` |
| `alpn_selected()` | 握手完成后是对端选定的协议名；之前是空串 |
| `open_stream(bool bidi = true)` | 真开流，返回流号；失败返负的错误码 |
| `write_stream(stream_id, data, len, end_stream = false)` | 受理进发送队列，`on_write` 报完成 |
| `shutdown_stream(stream_id, app_error_code = 0)` | 只关发送方向；队列里没写完的以 `NGTCP2_ERR_STREAM_SHUT_WR` 收场 |
| `shutdown_stream_read(stream_id, app_error_code = 0)` | 只关**读**方向：给对端发一个 STOP_SENDING（"别再发了"） |
| `streams_left(bool bidi)` | 现在还能开几条本地流（**累计**上限，不是增量）；没内核时是 0 |
| `close(int error_code = 0)` | 发 CONNECTION_CLOSE，`state()` 到 `CLOSING` |

四件签名与语义上的讲究：

- `open_stream()` 返回 `int64_t` 而不是 `int`。QUIC 的流号是 62 位无符号（RFC 9000
  §2.1 允许到 2^62−1），用一个会截断的返回类型，就是把一个"永远到不了"的假设焊进
  签名里。
- **`write_stream()` 是"受理"不是"发出去了"。** 没有完成通知，调用方就只能靠猜来
  决定什么时候能重用缓冲区 —— 这是 `uvcpp_tcp_client::write()` 早就定下的同一条
  约定，`on_write` 是它在流上的对应物，且**一次调用恰好对应一次回调**。
- `shutdown_stream()` 与 `close()` 是**两个**动作，因为 QUIC 的流是**两个方向各关
  一次**的。想要 TCP 那种"两边一起关"，得 `shutdown_stream()` 之后再等对端也关。
- **两个方向的关闭各有自己的入口**：`shutdown_stream()` 关的是**写**方向（"我说完
  了"），`shutdown_stream_read()` 关的是**读**方向（"你别再发了"，线上是一个
  STOP_SENDING 帧）。两者都收应用错误码，默认 0。**对端**下的 STOP_SENDING 不在
  这两个入口上，它由 `on_stop_sending` 报 —— 方向相反的两件事，别混。
- `close(error_code)` 的错误码 0 与非 0 有语义差别：非 0 会被当作**应用错误码**发给
  对端。客户端端点自己的 `close()` 不吃参数 —— 只发一个干净的 CONNECTION_CLOSE。

回调集合 `uvcpp_quic_connection::callbacks` 有**七**个：`on_read`、`on_stream_open`、
`on_write`、`on_alpn`、`on_close`、`on_streams_available`、`on_stop_sending`。
七个都在 **loop 线程**上跑，而且 `on_read` /
`on_stream_open` / `on_write` 会在某个内部调用**还没返回**的时候就同步跑用户代码 ——
于是用户代码可以在回调里 `write_stream()` / `close()` 掉这条连接（这两件事都被受理，
真正的发包推到回调退栈之后），但**回调返回后不要再碰本对象**，尤其是 `on_close`
之后：那时端点已经在准备销毁它了。

`on_close` 的 `error_code` 按符号分三种意思：`> 0` 是对端的**应用**错误码，`= 0`
是干净关闭，`< 0` 是传输层原因（`NGTCP2_ERR_DRAINING` 表示对端关了我们、
`NGTCP2_ERR_IDLE_CLOSE` 表示空闲超时），拿 `quic_error_string()` 问它的意思。
无论多少条路通向终结，它**恰好跑一次**。

### 5.4 读事件

```cpp
#include <quic/uvcpp_quic_common.h>
#include <quic/uvcpp_quic_connection.h>

#include <cstdint>

using namespace uvcpp;

// 读回调拿到的 result 与 net 层**同一套语义**（net_read_result / net_read_event），
// 只是多了一个流号：一条连接上有很多条流。
void install_reader(uvcpp_quic_connection& conn) {
  uvcpp_quic_connection::callbacks cbs;
  cbs.on_read = [](uvcpp_quic_connection&, int64_t,
                   const net_read_result& r) {
    if (r.event == net_read_event::DATA) {
      // r.data 只在本次回调期间有效（r.size 是字节数，二进制安全，可能含 NUL）；
      // r.error 只有 event == READ_ERROR 时才是 libuv 错误码。
      return;
    }
    // PEER_CLOSED 是一次**干净收尾**，不是错误；READ_ERROR 才是。
    // 两者都**按流**报：某条流收到 FIN 或 RESET_STREAM 只结束那一条流。
  };
  conn.set_callbacks(cbs);
}
```

读侧的收尾**按流**到达，而且**两个方向各报各的**：对端发 FIN 由 `on_read` 报
`PEER_CLOSED`；对端发 RESET_STREAM（它不要这条流了）也由 `on_read` 报，只是错误码
非 0 时报 `READ_ERROR`、为 0 时报 `PEER_CLOSED` —— 错误码是应用自己定的，0 按约定
就是"没有错误"，报成 `READ_ERROR(0)` 会让调用方去做无意义的错误处理。

**但"谁报的"分不出那是一件事还是两件事**，所以 1.4.1 给结果加了一格
`net_read_result::fin`（`src/net/uvcpp_net_read.h:100`）：`PEER_CLOSED && fin` 是
对端发了 FIN 的正常收尾，`PEER_CLOSED && !fin` 是对端发了**应用错误码 0 的
RESET_STREAM**。两者都报 `PEER_CLOSED` 是有意的 —— 对上层来说"读侧到此为止"是
同一件事 —— 而 `fin` 才是让 HTTP/3 分得开"请求体发完了"与"请求被取消了"的那一位
（`nghttp3_conn_read_stream2()` 要的就是它）。TCP 那四处构造点一律置 `true`，因为
TCP 的 `PEER_CLOSED` 本来就是对端 FIN —— 于是这个字段对 TCP 也是真话。

另外两格是 1.4.1 新接出来的：

```cpp
// doc-snippet: fragment — 两格回调的装法摘录（`cbs` 来自上文），不是完整翻译单元
  // 本端现在可以多开 max_streams 条**累计**上限的本地流（不是增量）。
  // 握手刚完时对端的 initial_max_streams_* 可能还没到，想开流就得等这一格。
  cbs.on_streams_available = [](uvcpp_quic_connection& c, bool bidi,
                                uint64_t max_streams) {
    (void)c; (void)bidi; (void)max_streams;
    // c.streams_left(bidi) 问"现在还能开几条"。
  };
  // 对端下发了 STOP_SENDING："这条流你别再发了"。与 on_read 的收尾是两件事 ——
  // 那个报"对端不发了"，这个报"对端不要我发了"。
  cbs.on_stop_sending = [](uvcpp_quic_connection& c, int64_t stream_id,
                           uint64_t app_error_code) {
    (void)app_error_code;
    c.shutdown_stream(stream_id, /*app_error_code=*/0);  // 通常在这里认账
  };
```

> **本层收到 STOP_SENDING 时不会自己回一个 RESET_STREAM。** 以前那句"对端的
> STOP_SENDING 没接"描述的是"这个事件根本到不了应用"；现在它到得了，而**默认的
> 动作留给上层**：该不该认账、用什么错误码认，是协议层的事（HTTP/3 那一层要按
> nghttp3 的指示走），链路层替它决定只会在"对端已经不要这条流了"和"这条流本来就
> 读完了"这两件事之间猜错。所以这一格不是"缺口已补"，是**契约改了**：能看见，
> 拍板的是上层。

**`on_stream_close` 那一格是故意留空的。** 它要等**两个**方向都收场才跑，那时读侧
早就没有新信息了，拿它再报一次只会让调用方对同一条流收两次尾。本层把读侧的信号
放在**它自己的到达时刻**上，写侧的收尾则由 `on_write` 报。

---

## 6. 后端探针与错误码

`src/quic/uvcpp_quic_common.h` 上有四个函数，它们既是给使用者用的，也是
**"依赖真被用到了"的证据**：

| 函数 | 真调进哪里 | 证明什么 |
|---|---|---|
| `quic_ngtcp2_version_string()` | 读 `NGTCP2_VERSION` **宏** | 头路径到得了（**不**证明链上了） |
| `quic_error_string(int)` | `ngtcp2_strerror()` | `ngtcp2_static` 真被链上 |
| `quic_crypto_backend_init()` | `ngtcp2_crypto_ossl_init()` | `ngtcp2_crypto_ossl_static` 真被链上 |
| `quic_crypto_backend_free()` | `ngtcp2_crypto_ossl_free()` | 与上一条配对 |

第一条单独拎出来说：**ngtcp2 没有 `ngtcp2_version()` 这样的函数**，版本只以宏的形式
存在。所以"没调那个函数"不是遗漏，是**它不存在**。也正因为它只读一个宏，一个"头
路径骗到了、libngtcp2 一个字节都没链上"的树照样能把它编出来并跑绿 —— 真正的链接
证据在后两行，它们少链就是**链接期未定义符号**，用例根本编不出可执行文件。

`quic_crypto_backend_init()` 那一条还额外覆盖了本模块最容易坏的一半：
`ngtcp2_static` 是协议状态机，`ngtcp2_crypto_ossl_static` 才是把它接到 OpenSSL 上的
那一半，而两个目标里**只有后者**取决于"那份 OpenSSL 是不是带 QUIC API 的 mainline
≥ 3.2"（树不对时它**根本不会被建出来**）。所以链接到它，等于在链接期把 §2 那段
配置期预检又证了一遍。

行为上的两条注意：

- `quic_error_string(0)` 返回 `"NO_ERROR"`；认不出的码返回 `"(unknown)"` —— 两个
  都是**上游的原文**，别顺手改大小写。
- `quic_crypto_backend_init()` **不要调两次**。上游不查重：第二次会把同一批 EVP
  对象再 `fetch` 一遍、覆盖掉第一次的指针，第一次那批引用计数就此丢掉。要再来一次
  就先 `quic_crypto_backend_free()`。它是**可重复调用**的（内部每释放一个就置
  NULL），而 `init`/`free` 必须配对 —— 本库**不**在静态析构里替你调它。

---

## 7. 典型坑

1. **别指望从公开头里看到 ngtcp2。** `<ngtcp2/ngtcp2.h>` 只出现在私有的
   `src/quic/uvcpp_quic_ngtcp2.h` 里，而它和持有 ngtcp2 句柄的
   `src/quic/uvcpp_quic_session.h` **两个都不安装**（`CMakeLists.txt:2299-2299`）、打包
   也被排除。理由有两条：一是使用者不该被逼着去配 ngtcp2 的搜索路径才能 include
   一个本库的头；二是 `uvcpp_quic_session.h` 的成员里就有 `ngtcp2_conn*`、`SSL*`、
   `ngtcp2_path_storage`，它的字段布局直接跟着 ngtcp2 的版本走 —— 一旦漏进公开面，
   那个版本就变成了本库的 ABI。这处排除与 `tests/tools/package_release.py` 的
   `PRIVATE_HEADERS` 是**一对**，两处必须一起改。

2. **ngtcp2 的 include 路径是 PRIVATE 进来的。** 静态库上那是 `$<LINK_ONLY:…>`，
   不传播头路径 —— 功能测试**光靠链接**拿不到那份头（真要 include，得照 `web_ssl_*` 自己挂 OpenSSL 的写法补一句 `target_include_directories(... $<TARGET_PROPERTY:ngtcp2_static,INTERFACE_INCLUDE_DIRECTORIES>)`，`tests/functional/CMakeLists.txt` 末尾有一条注释记着这个写法）。**但那条路也别走**：本机 PATH 上有 MSYS2 那份**版本不同**的 ngtcp2，`-I` 没排到它前面就会静默拿到它（报错长得像本库的私有头写错了）；而且库里那些私有 helper **没有导出** —— 白盒用例就算编得过，链接期也是 `undefined reference`。这就是
   `quic_ngtcp2_version_string()` 声明在**公开**头、实现在
   `src/quic/uvcpp_quic_ngtcp2.cpp`（由它 include 私有头）的原因：测试只经公开头
   调用，符号在链接期解析，照样证明 ngtcp2 真被链上。

3. **`src/quic/` 的源文件被 `list(FILTER … EXCLUDE REGEX "src/quic/")` 排除**
   （`CMakeLists.txt:1500-1500`，头文件那一条在 `:1252`），而且"开了 QUIC 但目录是空的"
   会**当场 FATAL**（`CMakeLists.txt:1514-1514`）。后者防的是一棵树同时骗过三道看起来
   很像门禁的东西：cache 里 `QUIC=ON`、日志里有 `ngtcp2 integrated`、编译也过 ——
   而 `src/quic/` 一个 `.cpp` 都没有，**零行 QUIC 代码被编译过**。"没测"必须表现为
   **失败**，不是表现为**通过**。

4. **不要从公开 quic 头里 `#include` ngtcp2。** 除了上面第 1 条那个 ABI 理由，
   `tests/tools/check_doc_snippets.py` 的 `NOT_BUNDLED` 里记着 `ngtcp2/`：公开头
   一旦拉它，文档片段会被判成"环境缺口"（退 3）而不是编不过（退 1）。两种都不该
   发生，但前者的提示是错的 —— 问题不在文档，在那个头。

5. **别把本模块的读事件当成"另一套"来写。** 事件是 `net_read_event`，结果是
   `net_read_result`，与 `net/` 层逐字相同。只有回调的**第一个参数**不同
   （`uvcpp_quic_connection&` 而不是 `uvcpp_tcp_client&`），因为 QUIC 连接不是、也
   不该被伪装成一个 TCP 客户端 —— 它根本没有 `uv_stream_t`，继承不了。

6. **`UVCPP_QUIC_ENABLE` 别自己定义。** 包的 `uvcpp/uvcpp_config.h` 里带的是本次
   构建实际用的值，自己定义一个不一致的（比如从别处拷了一份 `-D`）是**硬
   `#error`**，而不是静默的 ABI 错配。

7. **`UVCPP_ENABLE_QUIC=ON` 与 `UVCPP_ENABLE_NGHTTP2=ON` 同时开，本版是能配的 ——
   但它要先把 ngtcp2 的源码改一行。** ngtcp2 与 nghttp2 **都**无条件地建一个名叫
   `check` 的 custom target（`ngtcp2/CMakeLists.txt:181`、`nghttp2/CMakeLists.txt:195`），
   后加进来的那个会直接 configure 失败。所以 `CMakeLists.txt` 里那段把 ngtcp2 的
   源码**复制**进构建目录、在副本上摘掉那一行（`build*/_deps/uvcpp-ngtcp2-src/`）。
   （**nghttp3 是同一个坑的第三个来源**，1.4.1 的 HTTP/3 照同一套处置 ——
   见 [`doc/http3-guide.md`](./http3-guide.md) §5。三个来源里两个同时开就撞上，
   所以别把这个组合当成"只有一个补丁要打"。）本文下面那三条推论：

   - **`-DFETCHCONTENT_SOURCE_DIR_NGTCP2=<本地检出>` 之后改了那份检出，要删掉
     `build*/_deps/uvcpp-ngtcp2-src/` 再 configure** —— 副本只在第一次 configure 时
     生成，不删就不会跟着变。这是"复制而不是原地改"换来的代价：原地改会弄脏
     使用者自己的工作树。
   - **那道补丁配了一条断言**：找不到那一行就 FATAL，而不是静默跳过。上游改了
     写法时你会看到"补丁没打上"，而不是"配置莫名其妙的撞名错"。
   - **别把这个组合当成被支持的组合去用。** CI 里没有任何一条腿同时开这两个
     （`h2` 关 quic，`quic` 关 web），所以这条路径**没有门禁看着** —— 它能配出来
     只证明构建契约成立，不证明那棵树被跑过。

   CMake 自己的逃生口 `CMP0002=OLD`（"逻辑目标名必须全局唯一"）在这里**救不了**：
   策略只在没有被子目录覆盖时继承，而 ngtcp2 的 `cmake_minimum_required` 会把
   继承下来的 OLD 重置回 NEW。试过的三种设法（父目录 `cmake_policy(SET)`、函数
   作用域、`CMAKE_POLICY_DEFAULT_CMP0002`）**都实测失败**，别再走一遍。

8. **`close(42)` 之后，值 42 只有对端收得到 —— 你自己那一侧的 `on_close` 收到的是
   0。** 这不是漏报：本端一旦发出 CONNECTION_CLOSE 就进了 `CLOSING` 期，而 RFC 9000
   §10.2.1 规定这个状态下不再处理收到的包，于是对端的回应被 ngtcp2 一律以
   `NGTCP2_ERR_CLOSING` 丢掉。所以"本端先关的那一侧拿不到对端的错误码"是**协议
   行为**，不是本层的取舍 —— `tests/functional/quic_api_func.cpp` §5 把它量成了
   两条断言（一侧 42、另一侧 0），免得后来人把它当 bug 修掉。

---

## 8. 没做的（如实列出）

按仓库惯例，这一节必须老实写。以下都是**这一版真的没有**，不是"文档没写"：

- **本层里没有 HTTP/3，也不该有。** 1.4.1 把它落在了 `src/http3/`
  （[`doc/http3-guide.md`](./http3-guide.md)）—— 这一层是它的地基，不是它。
- **没有连接迁移、0-RTT、无状态重置、datagram（RFC 9221）、multipath。**
- **没有多循环支持。** 见 [§5.2](#52-服务端)。
- **MinGW 的 CI 腿没有开 QUIC。** macOS 与 Windows MSVC 各有一格 `quic`（1.4.1 补的，
  它们用包管理器给的 OpenSSL，只有 ubuntu 那格自建），只有 MSYS2 那条腿还没有 —— 它是
  单个 job，按功能拆它和加 QUIC 格是同一件事，`doc/ci-guide.md` §1 的"未覆盖的格"里有理由。
- **预编译包（`1.5.0` 起）里这个头是活的**（`UVCPP_QUIC_ENABLE 1`）：六条腿的发布档
  与调试档都开了 QUIC —— 为此每条腿都得有一份带 QUIC API 的 OpenSSL ≥ 3.2，逐条的来路
  与判据（读生成的头，不是 `CMakeCache.txt`）见 `release.yml` 文件头那张表。**在 1.5.0
  之前这条不成立**：那时候的发布包不带 QUIC，这个头是惰性的（与 `http2` 头在非 h2 包里
  的行为一致）。**从源码构建的人不受影响**：`UVCPP_ENABLE_QUIC` 默认仍是 `OFF`，前提
  也照旧要自己满足。
- **没有 `uvcpp_web` 那侧的接线。** `uvcpp_http_server` 不会因为 QUIC 打开就多出
  什么 —— 它连 `UVCPP_QUIC_ENABLE` 都不看。
- **没有流状态枚举，也没有自研 varint 编解码。** 这两样都属于"没有调用方的名字"：
  流已经是真实现了，但它的公开面仍然只是 `stream_id` 本身加一组事件回调，没有任何
  访问器会返回"这条流的读方向还在不在"之类的状态，所以 `quic_stream_state` 那样的
  枚举**依然**一个消费者都没有；varint 的包解析/序列化归 ngtcp2，自己写一份就是
  死代码。本仓库对这类名字是不发的 —— 尤其是公开头里的，它会进 `include/quic/`、
  进 API 兼容面，被下一个读代码的人当成承重结构。对照：`quic_connection_state`
  **留着**，因为 `state()` 是它的消费者，测试也断言在它上面。
- **没有连接级的统计/指标**（丢包数、RTT、拥塞窗口）。想读这些得上 ngtcp2 的
  `ngtcp2_conn_get_*`，而那是私有头里的类型，公开面暂时不接。
- **C 面（1.4.4 起）给的是整条"一条连接多股流"的面**：`include/capi/uvcpp_c_quic.h`
  （TLS 上下文 / 客户端 / 服务端 / 连接 / 流收发 / 那几枚计数器），另外把读事件的形状
  与 net 那份 **复用同一个结构体**（`uvcpp_c_read_result`）而不是另造一个长得一样的。
  它有一条别处没有的规矩：连接句柄是**借来的**（只有端点建连接、只有端点关连接），
  所以它没有 `_free()`，而"它什么时候不算数了"由知道这件事的那一侧毒化它 —— 见
  [C ABI 指南](./capi-guide.md) §3.3 与 §4。

- **GSO 只做了发送侧。** 收方向（`UDP_RECV_MAX_COALESCED_SIZE` + `WSARecvMsg` + `UDP_COALESCED_INFO`）没做，理由与代价见上面 [§1.2](#12-windows-上的发送侧-udp-gsouso152)：`uv_udp_recv_start` 够不着控制消息，半开会让粘在一起的数据报被当成一次读。另外这个开关**只在 Windows 上生效** —— Linux 上 libuv 那条 `sendmmsg` 批量（每批 20 个）已经在那儿了，不需要本库再做一遍。

下一步是把 h3 接进 web 层的**流式路由**与 **GOAWAY 的平滑退场**（见
[`doc/http3-guide.md`](./http3-guide.md) §8 的收尾）。连接迁移、0-RTT 那些仍然不在
路线图上 —— 它们要等有真实需求时才谈，本层也**不该**为了它们先动形状。
