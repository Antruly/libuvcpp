# QUIC 传输层指南

`src/quic/` 是 **net 层**上的一条**链路协议** —— QUIC（RFC 9000），后端是
[ngtcp2](https://github.com/ngtcp2/ngtcp2)。它的位置与 `src/http2/` 对 HTTP/2 完全
相同：协议栈归上游，本库只做事件循环、缓冲与生命周期的接线。它是 `src/web/` 里将来
那层 HTTP/3 的地基，但**它自己不是 HTTP** —— 这一层不认识请求、响应、流上面的任何
语义。

> ## 1.4.1 起它是一条真能通信的链路协议
>
> 握手、流收发、连接关闭与空闲超时**都通了**：一条 UDP 口上真起得来连接，一条
> 连接上真跑得动多条流，两边真能互发字节。这些不是"设计好了"，是三个功能用例
> 里的断言量出来的（`quic_handshake_func.cpp` 22 条、`quic_stream_func.cpp`
> 33 条、`quic_api_func.cpp` 81 条）。
>
> **没做的**是 HTTP/3（nghttp3 连依赖都没接）、0-RTT、连接迁移、无状态重置、
> datagram（RFC 9221）与 multipath —— 逐条列在下面
> [§8](#8-没做的如实列出)，别在别处另维护一份。
>
> 1.4.1 之前在公开头上挂着的那一片 `@warning` 已经整片摘掉了：它们描述的是
> "返回 `UV_ENOSYS`、回调一次都不跑"的骨架，而那个契约是被
> `tests/functional/quic_api_func.cpp` **钉住**的 —— 实现落地时那些断言确实红了，
> 逼着实现者回来把契约显式改成真的，而不是让"框架写好了"这句话悄悄变成假的。
> 现在它们测的是**实现**：没挂到连接上的壳返回 `UV_ENOTCONN`、没设 TLS 上下文的
> 端点返回 `UV_EINVAL` 且一条回调都不许排。

- 打开方式：`-DUVCPP_ENABLE_QUIC=ON`。**默认 OFF**（`CMakeLists.txt:94`），而且
  下面三种情况会被**强制**置 OFF 并打 warning，而不是留一个"能配置、链不上、
  跑不起来"的组合：
  1. 没开 OpenSSL（`CMakeLists.txt:444`）—— QUIC 建在 TLS 1.3 上，ALPN 是 TLS
     扩展，**没有明文 QUIC** 这回事；
  2. 没开 net 层（`CMakeLists.txt:451`）—— QUIC 是 net 层的一条链路协议，并且复用
     `src/net/uvcpp_net_read.h` 的读事件契约；
  3. OpenSSL 找得到、但**不带 QUIC API**（`CMakeLists.txt:497`）—— 判据见
     [§2](#2-编译期条件一份带-quic-api-的-openssl--32)。
- 包含方式：`<quic/uvcpp_quic_client.h>`、`<quic/uvcpp_quic_server.h>`、
  `<quic/uvcpp_quic_connection.h>`、`<quic/uvcpp_quic_common.h>`。私有的
  `<quic/uvcpp_quic_ngtcp2.h>` 与 `<quic/uvcpp_quic_session.h>` **都不安装**
  （`CMakeLists.txt:1618`）—— 理由见 [§7](#7-典型坑) 第一条。
- 四个公开头**全部**整段套在 `#if UVCPP_QUIC_ENABLE` 里，所以**不开关就一个类都
  看不到**。这与 `web/`、`ssl/`、`http2/`、`wsdl/` 同档。

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一版没做"的地方都明确
> 标出来 —— 那些地方比 API 更容易踩。

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
（`src/quic/uvcpp_quic_common.h:109-137` 有完整理由）。

**它不属于 web 层。** 所以 `CMakeLists.txt:94` 那个 `option()` 刻意**不**放在
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

## 2. 编译期条件：一份带 QUIC API 的 OpenSSL ≥ 3.2

QUIC 的版本协商走 TLS 的 ALPN 扩展，所以 QUIC **必须**有一份支持 QUIC 的 TLS 库。
libngtcp2 支持两个分支，判据写在它自己的 `CMakeLists.txt` 里，顺序是 load-bearing 的：

| 先查到 | 分支 | libngtcp2 建出来的 crypto 目标 |
|---|---|---|
| `SSL_provide_quic_data` | quictls / LibreSSL（`HAVE_QUICTLS`） | `ngtcp2_crypto_quictls_static` |
| 上面那个没有、`SSL_set_quic_tls_cbs` 有 | mainline OpenSSL ≥ 3.2（`HAVE_OSSL`） | **`ngtcp2_crypto_ossl_static`** ← 我们链的是这个 |
| 两个都没有 | — | 一个都不建，configure 期 `FATAL_ERROR` |

本库的探针**复刻**了这个顺序（`CMakeLists.txt:487`、`CMakeLists.txt:490`）。这不是
仪式：只查 `SSL_set_quic_tls_cbs` 的探针会在 quictls 树上"通过"，然后去链一个**从来
没被建出来**的目标。所以探针的结论是"没有 provide、但有 cbs"才放行
（`CMakeLists.txt:497`）；另外两格都强制关闭并出声。

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
ngtcp2 integrated (tag=v1.25.0, static)          # CMakeLists.txt:646
Including quic module in build (ngtcp2 v1.25.0)  # CMakeLists.txt:1072
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
nghttp3 留到真做 HTTP/3 的那一版再接。

---

## 4. 1.4.1 交付了什么

| 东西 | 状态 | 判据在哪 |
|---|---|---|
| CMake 接线（开关、三守卫、QUIC API 探针、FetchContent、PIC 断言） | **真实现** | `check_ci_layout.py` 的 `FEATURE_GATES["quic"]` |
| 依赖接入（`ngtcp2_static` + `ngtcp2_crypto_ossl_static`，静态链入） | **真实现** | `quic_api_func.cpp` 的三条链接证据 |
| 配置契约宏 `UVCPP_QUIC_ENABLE`（生成头 + `_uvcpp_literal01` + PUBLIC 编译定义三处一致） | **真实现** | `check_config_contract.py` |
| **握手**（Initial → TLS 1.3 → ALPN → 1-RTT） | **真实现** | `quic_handshake_func.cpp` |
| **开流、收发、流控、重传、丢包恢复** | **真实现**（ngtcp2 驱动） | `quic_stream_func.cpp` |
| **连接关闭（两端各自的 CONNECTION_CLOSE）与空闲超时** | **真实现** | `quic_api_func.cpp` §5 |
| **服务端的 CID 路由**（一条连接多个 SCID + 对端原始 DCID） | **真实现** | `quic_handshake_func.cpp` + `quic_stream_func.cpp` |
| `uvcpp_quic_server::bind()` 系列**参数校验** | **真实现**（登记；socket 在 `listen()` 里建） | `quic_api_func.cpp` §4 |
| `set_ssl_context()` / `set_alpn_protos()` / `set_alpn_select_protos()` / `set_idle_timeout()` | **真实现** | — |
| `run()` / `stop()` | **真实现**（转发到 loop） | — |
| `quic_ngtcp2_version_string()` / `quic_error_string()` / `quic_crypto_backend_init()` / `_free()` | **真实现**（真调进 ngtcp2） | `quic_api_func.cpp` §1 |
| 对端 reset 一条流（RESET_STREAM）时读侧的收尾事件 | **真实现** | `quic_stream_func.cpp` 第 3 段 |
| 连接迁移、0-RTT、无状态重置、datagram（RFC 9221）、multipath | **没有** | [§8](#8-没做的如实列出) |
| HTTP/3（nghttp3） | **没有**（连依赖都还没接） | [§8](#8-没做的如实列出) |

一句话：**这一层现在是一条能握手、能开流、能收发、能干净收场的链路协议**；它上面
还没有 HTTP/3，所以它还不认识"请求"和"响应"。

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
| `shutdown_stream(stream_id)` | 只关发送方向；队列里没写完的以 `NGTCP2_ERR_STREAM_SHUT_WR` 收场 |
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
- `close(error_code)` 的错误码 0 与非 0 有语义差别：非 0 会被当作**应用错误码**发给
  对端。客户端端点自己的 `close()` 不吃参数 —— 只发一个干净的 CONNECTION_CLOSE。

回调集合 `uvcpp_quic_connection::callbacks` 有**五**个：`on_read`、`on_stream_open`、
`on_write`、`on_alpn`、`on_close`。五个都在 **loop 线程**上跑，而且 `on_read` /
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
   `src/quic/uvcpp_quic_session.h` **两个都不安装**（`CMakeLists.txt:1618`）、打包
   也被排除。理由有两条：一是使用者不该被逼着去配 ngtcp2 的搜索路径才能 include
   一个本库的头；二是 `uvcpp_quic_session.h` 的成员里就有 `ngtcp2_conn*`、`SSL*`、
   `ngtcp2_path_storage`，它的字段布局直接跟着 ngtcp2 的版本走 —— 一旦漏进公开面，
   那个版本就变成了本库的 ABI。这处排除与 `tests/tools/package_release.py` 的
   `PRIVATE_HEADERS` 是**一对**，两处必须一起改。

2. **ngtcp2 的 include 路径是 PRIVATE 进来的。** 静态库上那是 `$<LINK_ONLY:…>`，
   不传播头路径 —— 所以**功能测试不能** `#include <ngtcp2/ngtcp2.h>`。这就是
   `quic_ngtcp2_version_string()` 声明在**公开**头、实现在
   `src/quic/uvcpp_quic_ngtcp2.cpp`（由它 include 私有头）的原因：测试只经公开头
   调用，符号在链接期解析，照样证明 ngtcp2 真被链上。

3. **`src/quic/` 的源文件被 `list(FILTER … EXCLUDE REGEX "src/quic/")` 排除**
   （`CMakeLists.txt:1067`，头文件那一条在 `:1068`），而且"开了 QUIC 但目录是空的"
   会**当场 FATAL**（`CMakeLists.txt:1081`）。后者防的是一棵树同时骗过三道看起来
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
   `check` 的 custom target（`ngtcp2/CMakeLists.txt:160`、`nghttp2/CMakeLists.txt:174`），
   后加进来的那个会直接 configure 失败。所以 `CMakeLists.txt` 里那段把 ngtcp2 的
   源码**复制**进构建目录、在副本上摘掉那一行（`build*/_deps/uvcpp-ngtcp2-src/`）。
   三个推论：

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

- **没有 HTTP/3。** nghttp3 本版**连依赖都没接**。这一层是它的地基，不是它。
- **没有连接迁移、0-RTT、无状态重置、datagram（RFC 9221）、multipath。**
- **对端的 STOP_SENDING 没接。** 收到它意味着"你这条流别再发了"，本层不会自动回
  一个 RESET_STREAM，于是会继续往一条对端已经丢弃的流上填字节，直到流控卡住。
  这是一处**已知的缺口**，不是遗漏 —— 接它需要在回调里排一个延后的 reset 意图
  （与 `close()` 那条同形状），留到需要的时候再做。
- **没有多循环支持。** 见 [§5.2](#52-服务端)。
- **MinGW 的 CI 腿没有开 QUIC。** macOS 与 Windows MSVC 各有一格 `quic`（1.4.1 补的，
  它们用包管理器给的 OpenSSL，只有 ubuntu 那格自建），只有 MSYS2 那条腿还没有 —— 它是
  单个 job，按功能拆它和加 QUIC 格是同一件事，`doc/ci-guide.md` §1 的"未覆盖的格"里有理由。
- **预编译包里 quic 头是惰性的**（`UVCPP_QUIC_ENABLE 0`）。`release.yml` 本版不
  开 QUIC（六条腿都要一份 QUIC-capable OpenSSL），所以与 `http2` 头在非 h2 包里
  的行为一致。
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

下一步只剩 **HTTP/3**（接 nghttp3，把请求/响应那层语义建在这条链路协议上）。
连接迁移、0-RTT 那些不在路线图上 —— 它们要等有真实需求时才谈。
