# SOCKS5 指南

`src/net/` 里有一套完整的 SOCKS5（RFC 1928 / RFC 1929）实现：**客户端、服务端、
UDP 数据面都在仓里**，只用 libuv，没有任何第三方代理库。三种命令
（CONNECT / BIND / UDP ASSOCIATE）与两种认证（no-auth、用户名密码）都做了，
IPv4 / IPv6 / 域名三种地址类型都认。

- 打开方式：随 `-DUVCPP_BUILD_NET=ON`（默认开）编进库，**没有单独的开关**
- 包含方式：`<net/uvcpp_socks5_client.h>` / `<net/uvcpp_socks5_server.h>` /
  `<net/uvcpp_socks5_udp.h>` / `<net/uvcpp_socks5_types.h>`
- 生成宏：`UVCPP_NET_ENABLE`（net 关掉的构建里这些类没有符号，头也还是能 include
  —— 与 net 层其它头一样）

> GSSAPI 认证、SOCKS4/SOCKS4a、UDP 分片重组**不在**这套实现里（见 §11）。
> 想给 HTTP / WebSocket 客户端加代理，多数时候不需要本页的类 —— 见 §4。

---

## 目录

1. [三条入口怎么选](#1-三条入口怎么选)
2. [最小可运行程序](#2-最小可运行程序)
3. [地址与常量](#3-地址与常量)
4. [内置 CONNECT 代理（tcp / http / ws）](#4-内置-connect-代理tcp--http--ws)
5. [独立客户端](#5-独立客户端)
6. [服务端](#6-服务端)
7. [UDP 数据面](#7-udp-数据面)
8. [超时、背压与上限](#8-超时背压与上限)
9. [安全：默认配置就是开放代理](#9-安全默认配置就是开放代理)
10. [测试](#10-测试)
11. [明确不做](#11-明确不做)

---

## 1. 三条入口怎么选

| 你要的 | 用什么 | 头 |
|---|---|---|
| 让 HTTP / WebSocket / 普通 TCP 客户端**穿过**代理连目标 | `uvcpp_tcp_client::set_socks5_proxy()`（HTTP/WS 客户端同名转发） | `<net/uvcpp_tcp_client.h>` |
| 自己控制 CONNECT / BIND / UDP ASSOCIATE 的完整流程 | `uvcpp_socks5_client` | `<net/uvcpp_socks5_client.h>` |
| 跑一个 SOCKS5 代理 | `uvcpp_socks5_server` | `<net/uvcpp_socks5_server.h>` |
| 直接用 UDP 关联的数据面（一般不必） | `uvcpp_socks5_udp` | `<net/uvcpp_socks5_udp.h>` |

分工的界线是**形状**：

- `set_socks5_proxy()` 走的是"连上代理之后悄悄把目标换掉"的透明形状 —— 一次
  `connect()`、一次回调，调用方看到的还是"连到了目标"。它只做 CONNECT，
  因为这是 HTTP/WS 需要的全部。
- `uvcpp_socks5_client` 把三种命令都暴露出来。BIND 要两次应答、UDP ASSOCIATE 要
  一条控制连接加一个数据面，这两件事塞不进"一次 connect 一次回调"。

`uvcpp_socks5_client` 内部用的是同一份 `uvcpp_tcp_client` 和同一份线格式编解码
（`src/net/uvcpp_socks5_common.h`，**私有头**，不安装、没有稳定性承诺），不是第二套实现。

---

## 2. 最小可运行程序

### 2.1 客户端：经代理 CONNECT 到 `example.com:80`

```cpp
#include <net/uvcpp_socks5_client.h>
#include <handle/uvcpp_loop.h>

#include <cstdio>

int main() {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_socks5_client c(&loop);

  // RFC 1929 认证；不调这一行就只提供 no-auth 一种方法
  c.set_credentials("user", "pass");

  c.connect("127.0.0.1", 1080, [&](int st) {
    if (st != 0) {
      std::fprintf(stderr, "proxy: %s\n", c.last_error().c_str());
      return;
    }
    c.connect_target("example.com", 80, [&](int st2) {
      if (st2 != 0) {
        std::fprintf(stderr, "connect: %s\n", c.last_error().c_str());
        c.close();
        return;
      }
      c.read_start([](uvcpp::uvcpp_buf* b) {
        // 隧道里回来的字节
        (void)b;
      });
      const char req[] = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
      c.write(req, sizeof(req) - 1, [](int) {});
    });
  });

  loop.run(UV_RUN_DEFAULT);
  return 0;
}
```

`connect()` 只完成"到代理的 TCP + 问候 + 认证"，**不发**命令请求；
`connect_target()` 才是 CONNECT。两者都可以用 `*_wait` 同步版代替 —— 同步版会
**自己泵调用方的循环**，所以不能在循环的回调里调。

### 2.2 服务端：最小代理

```cpp
#include <net/uvcpp_socks5_server.h>

#include <cstdio>

int main() {
  uvcpp::uvcpp_socks5_server srv;   // 自建循环，run() 把它跑起来

  // 只绑回环。绑 0.0.0.0 之前先看 §9。
  if (srv.bind("127.0.0.1", 1080) != 0) {
    std::fprintf(stderr, "%s\n", srv.last_error().c_str());
    return 1;
  }

  srv.listen([](const uvcpp::uvcpp_socks5_session_info& info) {
    // status == 0：会话已按 info.command 建立
    // 非 0：被拒/失败（策略码或 libuv 错误码）
    if (info.status != 0) {
      std::fprintf(stderr, "session rejected: %d\n", info.status);
    }
  });

  std::printf("listening on %d\n", srv.port());
  return srv.run(UV_RUN_DEFAULT);
}
```

> 这个默认配置就是**开放代理**（no-auth + 不限制目标）。给生产环境用之前，
> 至少装 `authenticator` 或 `allow_target`，见 §6.2 与 §9。

---

## 3. 地址与常量

公开头里只到枚举与端点。编解码函数是内部契约，不进安装包。

```cpp
// doc-snippet: fragment — 公开类型清单，不是可编译的翻译单元
namespace uvcpp {

enum class uvcpp_socks5_command : uint8_t { CONNECT = 0x01, BIND = 0x02,
                                            UDP_ASSOCIATE = 0x03 };
enum class uvcpp_socks5_atyp : uint8_t { IPV4 = 0x01, DOMAIN_NAME = 0x03,
                                         IPV6 = 0x04 };
enum class uvcpp_socks5_reply : uint8_t { SUCCEEDED = 0x00, GENERAL_FAILURE,
                                          NOT_ALLOWED, NETWORK_UNREACHABLE,
                                          HOST_UNREACHABLE, CONNECTION_REFUSED,
                                          TTL_EXPIRED, COMMAND_UNSUPPORTED,
                                          ATYP_UNSUPPORTED };
enum class uvcpp_socks5_method : uint8_t { NO_AUTH = 0x00,
                                           USERNAME_PASSWORD = 0x02,
                                           NO_ACCEPTABLE = 0xFF };

struct uvcpp_socks5_endpoint {
  uvcpp_socks5_atyp atyp;
  std::string       host;   // 点分 IPv4 / IPv6 文本（不带方括号）/ 域名
  int               port;   // **主机字节序**

  uvcpp_socks5_endpoint();
  uvcpp_socks5_endpoint(uvcpp_socks5_atyp t, const std::string& h, int p);

  bool is_ipv4() const;
  bool is_ipv6() const;
  bool is_domain() const;
};

}  // namespace uvcpp
```

几条约定：

- **`port` 是主机字节序**。网络序只在线格式编解码那一层出现，出了那一层就不漏出来。
- `host` 是**文本**形式。IPv6 不带方括号（`::1`，不是 `[::1]`）。
- `uvcpp_socks5_reply` 只列到 `ATYP_UNSUPPORTED`（0x08）；9..0xFF 是保留值，
  收到时按 `GENERAL_FAILURE` 处理。
- 域名长度 1..255 字节（`ATYP=DOMAIN` 的长度字段是一个字节）；端口 1..65535。
  这些边界在客户端与服务端**都**检查，坏值不给对端发出去。

---

## 4. 内置 CONNECT 代理（tcp / http / ws）

`uvcpp_tcp_client`、`uvcpp_http_client`、`uvcpp_ws_client`、`uvcpp_web_ws_client`
上都有同一组四个函数：

```cpp
// doc-snippet: fragment — 四个类的签名相同
int  set_socks5_proxy(const std::string& host, int port,
                      const std::string& username = std::string(),
                      const std::string& password = std::string());
void clear_socks5_proxy();
bool socks5_enabled() const;
const std::string& socks5_last_error() const;
```

语义要点：

- **必须在 `connect()` 之前调**。连接已建立或正在建立时返回 `UV_EBUSY`；
  `clear_socks5_proxy()` 同样只能在没有连接时调。
- **打开之后目标主机不在本地做 DNS**：`connect(host, port)` 的 `host` 原样以
  `ATYP=DOMAIN` 交给代理（数字 IPv4/IPv6 则用对应的 ATYP）。这就是
  `curl --socks5-hostname` 的语义 —— 目标域名不出本机。
- **TLS 的顺序固定**：先 SOCKS5 协商，完成之后才在**同一个 socket** 上做 TLS。
  `tls_host_want_` / SNI / 证书校验用的仍然是**目标**主机名，与代理无关。
- `username` 为空 ⇒ 只提供 no-auth 一种方法；非空 ⇒ 只提供 RFC 1929 用户名密码
  一种方法。**不会同时提供两种** —— 那样"服务端选了 no-auth 而使用者以为自己在
  认证"会变成静默降级。用户名非空而密码为空是合法的（RFC 1929 允许空密码）。
- 协商完成之前**不会**触发连接回调。失败时回调拿到非 0：

  | 情况 | 回调 / 返回值 | `get_last_error()` | `socks5_last_error()` |
  |---|---|---|---|
  | 代理连不上 | 那次 TCP 的错误码 | 同一个码 | `uv_strerror` 的原文（没有"代理的话"可引） |
  | 方法不可接受 / 认证失败 | `TCP_CLIENT_ERROR` | `UV_EACCES` | 代理侧原话 |
  | 应答码非 0 / 协议错 | `TCP_CLIENT_ERROR` | `UV_EPROTO` | 应答码文本与目标 |

- **h3 与代理互斥**：`uvcpp_http_client` 开过 `set_http3_enabled(true)` 时再
  `set_socks5_proxy()` 返回 `UV_ENOTSUP`；反过来先设代理再开 h3，`connect()`
  返回 `UV_ENOTSUP`。**绝不静默直连** —— 那是代理泄漏。`uvcpp_ws_client` 没有 h3
  腿，所以不存在这条冲突。

```cpp
// doc-snippet: fragment — 经 SOCKS5 走 wss://
uvcpp::uvcpp_ws_client ws(&loop);
ws.set_socks5_proxy("127.0.0.1", 1080);        // 先代理
ws.connect("wss://example.com/feed", [](int st) {
  // 到这里时 SOCKS5 与 TLS 都已完成；TLS 校验的是 example.com
});
```

---

## 5. 独立客户端

一个对象对应**一条到代理的控制 TCP 连接**。所有回调都在 `get_loop()` 那条循环上触发。

```cpp
// doc-snippet: fragment — 接口清单，不是可编译的翻译单元
class uvcpp_socks5_client {
 public:
  explicit uvcpp_socks5_client(uvcpp_loop* loop);   // 不拥有 loop

  void set_credentials(const std::string& username, const std::string& password);
  void set_connect_timeout_ms(int ms);              // 默认 30000；0 = 不超时

  // 连代理 + 问候 + 认证；不发命令请求
  int connect(const std::string& proxy_host, int proxy_port,
              std::function<void(int)> cb = nullptr);
  int connect_wait(const std::string& proxy_host, int proxy_port,
                   int timeout_ms = 30000);

  // CONNECT
  int connect_target(const std::string& host, int port,
                     std::function<void(int)> cb);
  int connect_target_wait(const std::string& host, int port,
                          int timeout_ms = 30000);

  // BIND：两次应答
  int bind(const std::string& host, int port,
           std::function<void(int, const uvcpp_socks5_endpoint&)> on_reply1,
           std::function<void(int, const uvcpp_socks5_endpoint&)> on_peer = nullptr);
  int bind_wait(const std::string& host, int port,
                uvcpp_socks5_endpoint& out_reply1, int timeout_ms = 120000);
  int wait_for_peer(uvcpp_socks5_endpoint& out_peer, int timeout_ms = 120000);

  // UDP ASSOCIATE
  int udp_associate(std::function<void(int, const uvcpp_socks5_endpoint&)> cb,
                    const std::string& bind_host = "0.0.0.0", int bind_port = 0);
  int udp_associate_wait(uvcpp_socks5_endpoint& relay, int timeout_ms = 30000,
                         const std::string& bind_host = "0.0.0.0",
                         int bind_port = 0);

  // 隧道读写（CONNECT / BIND 成功之后）
  int  read_start(std::function<void(uvcpp_buf*)> cb);
  void clear_read_callback();
  int  write(const char* data, std::size_t len,
             std::function<void(int)> cb = nullptr);
  int  write_wait(const char* data, std::size_t len, int timeout_ms = 30000);

  uvcpp_tcp_client* stream();
  uvcpp_socks5_udp* udp();
  bool is_connected() const;
  void close();
  const std::string& last_error() const;
  int last_error_code() const;
};
```

### 5.1 顺序与状态

`connect()` 成功之后，`connect_target()` / `bind()` / `udp_associate()` **三选一**
（同一时刻只允许一个命令在飞，否则 `UV_EBUSY`）。前一个失败会关掉控制连接，
要重试就重新 `connect()`。

`connect()` 的超时覆盖"连代理 + 问候 + 认证 + 请求 + 第一次应答"整段
（`set_connect_timeout_ms`，默认 30 s）。BIND 的**第二次**应答各自计时：
异步路径没有额外超时（等对端连入），同步路径由 `wait_for_peer()` 的
`timeout_ms` 管（默认 120 s）。

### 5.2 读永远归本对象

与 `uvcpp_tcp_client` 不同，控制连接上的读**始终由 `uvcpp_socks5_client` 持有**：
握手与应用数据共用同一条字节流，谁抢到谁就丢。所以：

- 隧道里来的应用字节走 `read_start()`；
- 直接调 `stream()->read_start()` 会拿到 `UV_EALREADY`；
- `read_start()` 注册**之前**到达的字节会先攒着（上限 **2 MiB**），注册时补投 ——
  "服务端在应答包里夹带应用数据"不会丢；
- `clear_read_callback()` 之后到达的字节**被丢弃**（不再攒）。

`stream()` 仍然可以拿来查地址、`write()`、看状态。

### 5.3 CONNECT

```cpp
// doc-snippet: fragment — 只展示协商顺序；`c` 是前文已建好的客户端
c.connect("127.0.0.1", 1080, [&](int st) {
  if (st != 0) return;
  c.connect_target("example.com", 80, [&](int st2) {
    if (st2 != 0) return;
    // 控制连接此刻已成为隧道
  });
});
```

- 目标**不在本地解析**：域名以 `ATYP=DOMAIN` 发给代理，数字地址用它对应的 ATYP。
- 成功之后 `stream()` 就是隧道：用 `read_start()` 读、`write()` 写。

### 5.4 BIND

```cpp
// doc-snippet: fragment — 只展示两次应答的顺序；`c` 是前文已建好的客户端
uvcpp::uvcpp_socks5_endpoint bound;
uvcpp::uvcpp_socks5_endpoint peer;

if (c.bind_wait("0.0.0.0", 0, bound) == 0) {   // 第一次应答：代理绑定在哪
  // 把 bound 告诉对端（或做别的协调）
  if (c.wait_for_peer(peer) == 0) {            // 第二次应答：对端地址
    // 控制连接此刻就是数据通道
  }
}
```

异步版是 `bind(host, port, on_reply1, on_peer)`：`on_reply1` 拿到代理的绑定地址，
`on_peer` 拿到连进来的对端地址。`on_peer` 可以为空 —— 第二次应答到达时只把内部状态
从 `BIND_WAIT` 推回就绪，不通知任何东西。

`wait_for_peer()` 只对 `bind_wait()` 启动的 BIND 有意义：异步 `bind()` 请用
`on_peer` 回调接第二次应答（它不会泵循环）。已经收到过第二次应答时
`wait_for_peer()` 立即返回它。

### 5.5 UDP ASSOCIATE

`udp_associate()` 先在本地绑一个 UDP socket（`bind_host:bind_port`，端口 0 = 系统选），
把它的地址放进请求。代理回 `BND.ADDR/BND.PORT` 之后：

- 若回的是**通配地址**（`0.0.0.0` / `::`，有的代理就这么回），替换成**代理自己的
  地址**（控制连接的远端地址）—— 不替换的话数据报会发到本机；
- 最终中继地址交给 `udp()`，之后用 `udp()->send_to()` / `udp()->recv_start()`。

**关联的生命周期绑在控制 TCP 上**（RFC 1928 §7）：控制连接一关，本对象自动关掉它
持有的 UDP socket。使用者不需要（也不应该）自己删 `udp()`。

---

## 6. 服务端

```cpp
// doc-snippet: fragment — 接口清单，不是可编译的翻译单元
struct uvcpp_socks5_server_options {
  bool allow_no_auth;                 // 默认 true
  bool allow_username_password;       // 默认 false
  std::function<bool(const std::string&, const std::string&)> authenticator;

  bool allow_connect;                 // 默认 true
  bool allow_bind;                    // 默认 true
  bool allow_udp_associate;           // 默认 true
  std::function<bool(const uvcpp_socks5_endpoint&)> allow_target;  // 空 = 不限

  int         handshake_timeout_ms;           // 30000
  int         upstream_connect_timeout_ms;    // 30000
  int         bind_accept_timeout_ms;         // 120000
  int         udp_idle_timeout_ms;            // 0 = 不空闲超时
  std::size_t max_connections;                // 0 = 不限
};

struct uvcpp_socks5_session_info {
  uvcpp_socks5_command  command;
  uvcpp_socks5_endpoint target;   // 请求里的目标（BIND/UDP 是提示地址）
  uvcpp_socks5_endpoint peer;     // 客户端那条控制连接的远端地址
  int                   status;   // 0 = 已建立；否则错误码
};

class uvcpp_socks5_server {
 public:
  uvcpp_socks5_server();          // 自建循环；run() 把它跑起来
  uvcpp_loop* get_loop();
  int run(uv_run_mode md = UV_RUN_DEFAULT);

  int set_options(const uvcpp_socks5_server_options& opts);  // listen() 前
  int bind(const std::string& host, int port);               // host 含 ':'=IPv6
  int set_loops(int n);                                      // listen() 前
  int listen(std::function<void(const uvcpp_socks5_session_info&)> on_session
                 = nullptr,
             int backlog = 128);

  int port() const;               // bind(0) 用；未绑定时 -1
  std::size_t connection_count() const;
  void close();
  const std::string& last_error() const;
};
```

### 6.1 生命周期与线程

与 `uvcpp_tcp_server` 完全一致：

- `bind()` / `listen()` / `close()` 在服务端自己的循环线程上调；
- 会话的**状态机、上游连接、BIND 监听器、UDP socket 全部建在会话自己那条循环上**，
  所以 `set_loops(n > 1)` 时整条链路天然分片，不需要跨线程碰句柄；
- `on_session` 在每条会话**命令结果确定时**调一次（建立成功 `status == 0`）；
  它跑在会话自己那条循环上；
- `close()` 停掉新连接并关闭**所有**会话（包括工作循环上的）；已建立的会话由各自的
  收尾路径结束，不会被留下悬空的句柄；
- 策略在 `listen()` 时**快照**，之后改 `set_options` 不影响已有连接。

### 6.2 认证与目标策略

- `allow_no_auth = true`（默认）时客户端可以选 no-auth。
- `allow_username_password = true` **必须同时给 `authenticator`**：没有校验函数时
  这个开关不会把 0x02 方法选给客户端（否则等于"任何密码都对"）。
- `authenticator` 返回 `false` ⇒ 回 `REP=NOT_ALLOWED`，控制连接关闭。
- 选择顺序是**服务端自己的偏好**：两种都允许且客户端都提供时选 no-auth；
  否则选用户名密码。两边的交集为空时回 `0xFF`（no acceptable methods）。

  > 客户端一侧（`uvcpp_tcp_client` / `uvcpp_socks5_client`）刻意**只提供一种**
  > 方法，避免静默降级。

- `allow_target` 是可选白名单：返回 `false` ⇒ `REP=NOT_ALLOWED`。域名目标**原样**
  交给你（服务端不替调用方解析），`port` 是主机序。为空 = 不限制。

### 6.3 三种命令的实现要点

- **CONNECT**：在会话循环上解析目标（域名由代理侧解析），用同循环的
  `uvcpp_tcp_client` 连上游；成功回 `REP=0` 且 `BND.ADDR/PORT` 是上游 socket 的
  **本地地址**。失败按错误映射：

  | 上游错误 | 回的 REP |
  |---|---|
  | `ECONNREFUSED` | `CONNECTION_REFUSED` (0x05) |
  | `ENETUNREACH` | `NETWORK_UNREACHABLE` (0x03) |
  | `EHOSTUNREACH` / `ETIMEDOUT` / 域名解析失败 | `HOST_UNREACHABLE` (0x04) |
  | `EACCES` / `EPERM` | `NOT_ALLOWED` (0x02) |
  | `EAFNOSUPPORT` | `ATYP_UNSUPPORTED` (0x08) |
  | `ENOTSUP` | `COMMAND_UNSUPPORTED` (0x07) |
  | 其他 | `GENERAL_FAILURE` (0x01) |

- **BIND**：建一个临时 `uvcpp_tcp_server` 绑 `0.0.0.0:0`（或控制连接本地地址的
  地址族），回第一次 `BND`；**只接受一个**连接，第二次回包带对端地址。
  `bind_accept_timeout_ms`（默认 120 s）到了还没人连进来 ⇒ `GENERAL_FAILURE`
  并关控制连接。
- **UDP ASSOCIATE**：建一个临时 `uvcpp_udp_client` 绑通配临时端口，回 `BND`
  （通配地址替换为控制连接的本地地址，与客户端那条规则对称）。控制连接关闭即销毁
  UDP socket。首个合法数据报**锁定**客户端地址，此后其他来源丢弃；`FRAG ≠ 0` /
  `RSV ≠ 0` 丢弃；目标域名解析结果按 `host:port` 缓存。`udp_idle_timeout_ms > 0`
  时有流量重新计时，超时关闭控制连接。

### 6.4 背压

每个转发方向一条发送队列，同一时刻只挂**一笔** `write()`。队列超过
**1 MiB**（高水位）对源 `read_pause()`，降到 **256 KiB**（低水位）`read_resume()`。
这样既不会把对端灌爆，也不会因为 `write()` 的 `UV_EALREADY` 丢字节 ——
这条队列是本层自己实现的，net 层本身没有。

### 6.5 会话上限

`max_connections` 超了会在 accept 之后**立刻关**（TCP 没有"拒绝握手"的机会）。
`connection_count()` 是当前活跃会话数。

---

## 7. UDP 数据面

RFC 1928 §7：UDP ASSOCIATE 之后，客户端与代理之间的**每一个**数据报都带一个
SOCKS5 头 —— `RSV(2) + FRAG(1) + ATYP + ADDR + PORT`，载荷跟在后面。
`uvcpp_socks5_udp` 就是这个头。

地址有两件事要分清：

- **中继地址**（relay）：数据报的实际目的地，是代理回的 `BND.ADDR/BND.PORT`。
  必须由调用方（或 `uvcpp_socks5_client`）先设好。
- **目标地址**（target）：写在 SOCKS5 头里的真正目的地，域名也行 —— 解析是代理的事。

**中继地址必须是数字地址**（IPv4/IPv6）。`set_relay()` 遇到域名直接 `UV_EINVAL`
—— 热路径上唯一一次地址解析不能放在发送里，否则每个数据报都可能失败一次。

```cpp
// doc-snippet: fragment — 一般通过 client->udp() 拿到已配好的对象
uvcpp::uvcpp_socks5_udp udp(&loop);
udp.bind("0.0.0.0", 0);
udp.set_relay(uvcpp::uvcpp_socks5_endpoint(
    uvcpp::uvcpp_socks5_atyp::IPV4, "127.0.0.1", 1080));

udp.recv_start([](int, const uvcpp::uvcpp_socks5_endpoint& from,
                  const void* data, std::size_t len) {
  // from 是 SOCKS5 头里的来源（代理写的是目标的地址）
  // data 只在本次回调期间有效
  (void)from; (void)data; (void)len;
});

udp.send_to("example.com", 53, query, n);   // 域名可行，代理负责解析
```

约定：

- **不做分片重组**：`FRAG ≠ 0` 的数据报直接丢弃并计入 `dropped_datagrams()`
  （RFC 1928 明确允许不支持分片）。`RSV ≠ 0`、头截断、ATYP 非法同样丢弃。
  `recv_start` 的回调**只在正常包上触发**，`status` 实际上恒为 0（保留参数是为了
  将来"中继不可达"之类的 socket 级错误）。
- **单包上限 65535**（含 SOCKS5 头）：超过返回 `UV_EMSGSIZE`，**不截断**。
- `close()` 之后要重新 `bind()`。`recv_stop()` 只停接收，保留中继与绑定。
- 一般在 `uvcpp_socks5_client::udp_associate()` 成功之后通过 `udp()` 拿到它 ——
  那时中继已经填好、UDP socket 已经绑好，且生命周期归客户端管。

---

## 8. 超时、背压与上限

| 旋钮 | 位置 | 默认 | 说明 |
|---|---|---|---|
| 单个操作总超时 | `uvcpp_socks5_client::set_connect_timeout_ms()` | 30000 | 连代理 + 问候 + 认证 + 请求 + 第一次应答；0 = 不超时 |
| BIND 等对端 | `wait_for_peer()` / `bind()` 的第二次应答 | 120000 | 异步路径没有额外超时（等对端连入） |
| 服务端握手总时限 | `handshake_timeout_ms` | 30000 | 从接受到命令建立 |
| 服务端连上游 | `upstream_connect_timeout_ms` | 30000 | 超时回 `HOST_UNREACHABLE` |
| 服务端等 BIND 对端 | `bind_accept_timeout_ms` | 120000 | 超时回 `GENERAL_FAILURE` |
| UDP 空闲 | `udp_idle_timeout_ms` | 0 | 0 = 不超时（RFC 1928 §7 的推荐做法） |
| 会话上限 | `max_connections` | 0 | 0 = 不限 |
| 转发背压 | 服务端内部 | 1 MiB / 256 KiB | 高水位暂停源读、低水位恢复 |
| 客户端握手后攒字节 | `uvcpp_socks5_client` 内部 | 2 MiB | `read_start()` 之前到达的应用字节；超出部分丢弃 |

所有 `*_wait` 都是"自己泵循环直到完成或超时"。**不要在事件循环的回调里调它们**
（会阻塞那条循环），与仓库里其它 `*_wait` 的规矩相同。

异步写一侧：一条连接上同一时刻只允许一笔 `write()` 在途；再写返回
`UV_EALREADY`。要连发就先攒队列、等上一笔的回调 —— 与 net 层同一条契约。

---

## 9. 安全：默认配置就是开放代理

`uvcpp_socks5_server_options` 的默认值是 `allow_no_auth = true` 且不限制目标。
**绑 `0.0.0.0` 的默认配置等于对全网开放的代理**，会被人拿去做跳板。

生产上至少做两件事之一：

```cpp
// doc-snippet: fragment — 认证 + 目标白名单
uvcpp::uvcpp_socks5_server_options opts;
opts.allow_no_auth = false;
opts.allow_username_password = true;
opts.authenticator = [](const std::string& user, const std::string& pass) {
  return user == "alice" && pass == "s3cret";      // 换成你自己的校验
};
opts.allow_target = [](const uvcpp::uvcpp_socks5_endpoint& ep) {
  // 只放行内网口与 443；域名也回到这里，原样判断
  return ep.port == 443 || ep.port == 80;
};
srv.set_options(opts);
srv.bind("127.0.0.1", 1080);   // 不需要外部访问就只绑回环
```

其它几条：

- `authenticator` 里**不要**做阻塞调用（它在会话循环上跑）。
- `allow_target` 拿到的是**未解析的**目标：域名白名单要按域名判断，不要指望它已经
  是 IP；反过来，按 IP 做的白名单挡不住域名形式的目标（那由代理解析）。
- 服务端不理解、也不检查"目标是不是回环地址"—— 防御 SSRF 是使用者的策略，
  `allow_target` 是唯一的钩子。

---

## 10. 测试

| 用例 | 内容 |
|---|---|
| `socks5_parser_func` | 纯编解码（私有头白盒）：往返、增量（截断前缀必须是 `NEED_MORE`）、坏帧、域名 255/256 边界 |
| `socks5_client_func` | 本仓 server + 本仓 client 的 CONNECT / BIND / UDP ASSOCIATE 回环；no-auth、用户名密码成功/失败、域名目标、IPv6 |
| `socks5_server_func` | 服务端策略与失败路径：REP 映射、目标白名单、握手超时、并发会话上限 |
| `socks5_udp_func` | UDP 关联：中继、首个数据报锁定、FRAG 丢弃、控制连接关闭后的释放、IPv6 |
| `socks5_http_func` | `uvcpp_http_client` 经本仓代理访问本仓服务端：明文、SOCKS 之后接 TLS、代理不可达的反向对照 |

都不依赖外部网络，也不需要本机装任何代理：

```bash
ctest --test-dir build -R socks5 --output-on-failure
```

人工互操作：若本机有 `curl`，可以拿它当外部客户端打本仓服务端
（`curl --socks5-hostname 127.0.0.1:<port> http://<目标>/`）。CI **不依赖** curl
的版本，判据只在上面这五个用例里。

---

## 11. 明确不做

- **GSSAPI 认证**（RFC 1961）：不做。
- **SOCKS4 / SOCKS4a**：不做，只认 SOCKS5 的 `VER=5`。
- **UDP 分片重组**（`FRAG ≠ 0`）：丢弃并计数，RFC 1928 允许。
- **UDP 的域名中继地址**：`set_relay()` 只收数字地址（见 §7）。
- **HTTP CONNECT 代理**（`CONNECT host:port` 那种）：这是 SOCKS5，不是 HTTPS 代理。
- **QUIC / h3 over SOCKS5**：`uvcpp_http_client` 明确拒绝（`UV_ENOTSUP`），
  不静默直连。
- **BIND 的多次接受**：BIND 只接受**一个**对端连接（RFC 1928 的常见用法）。
- **C ABI**：本轮没有 `uvcpp_c_socks5_*` 面。

---

## 相关文档

- [`net-guide.md`](net-guide.md)：TCP / UDP 层本身（服务端生命周期、多循环、读契约）
- [`web-http-guide.md`](web-http-guide.md)：HTTP 客户端（`set_socks5_proxy` 的使用位置）
- [`web-ws-guide.md`](web-ws-guide.md)：WebSocket 客户端（含 `wss://` + 代理的顺序）
- [`ssl-guide.md`](ssl-guide.md)：TLS（SNI 与证书校验为什么必须对目标主机）
