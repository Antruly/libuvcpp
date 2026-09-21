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
4. [读的三条路](#4-读的三条路)
5. [net_read_result 的事件契约](#5-net_read_result-的事件契约)
6. [连接归谁管](#6-连接归谁管)
7. [写](#7-写)
8. [同步与异步](#8-同步与异步)
9. [UDP](#9-udp)
10. [TLS 接线](#10-tls-接线)
11. [错误处理](#11-错误处理)
12. [典型坑](#12-典型坑)
13. [没做的（如实列出）](#13-没做的如实列出)

---

## 1. 这一层是什么

| 类 | 头 | 角色 |
|---|---|---|
| `uvcpp_tcp_server` | `src/net/uvcpp_tcp_server.h:114` | `bind` / `listen`，**拥有**每一条 accept 出来的连接，给所有连接共用一份读回调 |
| `uvcpp_tcp_client` | `src/net/uvcpp_tcp_client.h:99` | 双模式（异步回调 / 同步 `*_wait`）TCP 连接；服务端交给你的那条连接也是它 |
| `uvcpp_udp_server` | `src/net/uvcpp_udp_server.h:51` | 绑定的 UDP 套接字，交付数据报时带来源 ip/port；没有"连接对象" |
| `uvcpp_udp_client` | `src/net/uvcpp_udp_client.h:72` | UDP 客户端，`bind`/`connect`、异步与同步发送、内部接收缓存 |
| `net_read_result` | `src/net/uvcpp_net_read.h:68` | 一次读事件的载体：数据 / 对端关闭 / 读错误 |

`src/net/uvcpp_tcp_server.h:24` include 了 `net/uvcpp_tcp_client.h`，后者又 include 了
`net/uvcpp_net_read.h`。所以**只写一句 `#include <net/uvcpp_tcp_server.h>` 就够**。

---

## 2. 最小可运行程序

一个回显服务端：

```cpp
#include <cstdio>

#include <uv.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>

int main() {
  uvcpp::uvcpp_tcp_server server;

  // 必须在 listen() 之前设好。一份回调，所有连接共用。
  server.set_read_callback(
      [](uvcpp::uvcpp_tcp_client& client, const uvcpp::net_read_result& r) {
        if (r.is_data()) {
          // 注意第三个参数不是可省的：cb == nullptr 会退化成同步 write_wait
          // （30 秒超时），而回调里禁止同步写。
          client.write(r.data, r.size, [](int status) {
            if (status != 0) {
              std::fprintf(stderr, "write failed: %d\n", status);
            }
          });
        } else if (r.event == uvcpp::net_read_event::PEER_CLOSED) {
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
| `set_read_callback` | 必须在 loop 线程调用，且应当在 `listen()` 之前设好（`src/net/uvcpp_tcp_server.h:234`） |
| `set_ssl_context` | 必须在 `listen()` 之前设置（`src/net/uvcpp_tcp_server.h:350-351`） |

`set_read_callback` 的实现就是一句赋值（`src/net/uvcpp_tcp_server.cpp:466-468`），
**`listen()` 之后调不会报错、也不会生效于已有连接**——只有那之后 accept 的连接才吃得到。
这是个静默的半失效状态，别踩。

> 规矩是**回调里不要用同步的 `write_wait`/`read_wait`**（`src/net/uvcpp_tcp_server.h:83-88`）：
> 它们会把 loop 线程按住，一个慢对端能拖住这条 loop 上的**所有**连接。坑在于这两个函数
> 的完成回调是可选参数，**省略它就等于同步** —— 按 `src/net/uvcpp_tcp_client.h:337`，
> `cb == nullptr` **正是** `write_wait(data, len, 30000)`。同文件 `:99` 的官方示例回显
> 现在传的是显式回调（异步）。本页的例子同样传显式回调来避开它。

---

## 4. 读的三条路

| 入口 | 在哪 | 回调 | 给你什么 |
|---|---|---|---|
| `read_start(cb)` | 客户端 `:432` | `void(uvcpp_buf*)` | 裸数据，**分不出对端关闭** |
| `read_start(nullptr)` | 客户端 | —— | 只为打开内部接收缓存给 `read_wait()` 用 |
| `read_start_events(cb)` | 客户端 `:446` | `void(uvcpp_tcp_client&, const net_read_result&)` | **带语义的事件** |
| `set_read_callback(cb)` | 服务端 `:232` | 同上 | 一份回调覆盖所有连接 |

**`read_start` 与 `read_start_events` 互斥。** 用过其中一个再用另一个，拿到
`UV_EALREADY`。挡在前面的理由写在实现里（`src/net/uvcpp_tcp_client.cpp:1827-1835`）：
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
（`src/net/uvcpp_tcp_server.h:230-232`）。反过来说，**设它之前**在连接回调里注册的读
优先级更高，会被保留。

---

## 5. net_read_result 的事件契约

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
（关闭回调）照常发生，所以**不要在回调里做释放**（`src/net/uvcpp_tcp_client.h:443-444`）。

---

## 6. 连接归谁管

**默认服务端全包**：每条新连接被登记、并配一个关闭管理器，连接关闭时把它从服务端
摘除并 `delete`。这件事**无条件发生**——你自己 `set_on_close()` 的那个槽位纯观察，
取消不了它（`src/net/uvcpp_tcp_server.h:46-49`）。

所以：

- **不要在关闭回调里 `delete` 客户端**，除非你已经用 `take_client()` 把所有权取走。
  删一个仍归框架管的客户端会让框架随后二次释放（`src/net/uvcpp_tcp_client.h:524-526`）。
- 想自己管，用 `take_client()` 取走、`return_client()` 交回。两个都**必须在 loop
  线程调用**；交回之后**不要再持有那个指针**。
- 关掉一条连接之后也别再留指针：框架可能在完成回调里把它删掉。

**"对端断开了"这件事只有读得见。** `nread < 0` 那条分支是唯一能发现它的地方
（`src/net/uvcpp_tcp_server.h:50-56`）——不读的连接，即使设了 `set_on_close()` 也
**永远不会被触发**，而且这条连接不会被释放，等于每条一个静默泄漏。只想知道死活、
不要数据的话，就用 `read_start_events()` 注册一个忽略数据的回调
（`src/net/uvcpp_tcp_client.h:512-516`）。

服务端有个 `set_auto_read`（默认 **true**）。关掉它只在"你要完全接管读路径、并且
自己负责发现断开"时有意义；关掉又没设回调时，服务端会给每条连接往 stderr 打一行警告
（`src/net/uvcpp_tcp_server.cpp:399-406`）。

---

## 7. 写

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

## 8. 同步与异步

**同一个操作上同步与异步不能混用**，混了就抛 `std::runtime_error`
（`src/net/uvcpp_tcp_client.h:76-78`）。具体是：

| 情况 | 结果 |
|---|---|
| 已经有异步写在飞，再调 `write_wait` | 抛 `std::runtime_error` |
| 已经有异步读，再调 `read_wait` / `read_start(nullptr)` | 抛 `std::runtime_error` |
| 已经有异步写在飞，再调异步 `write` | 返回 `UV_EALREADY` |
| 没连上就写 | 返回 `UV_ENOTCONN` |

**异步/同步其实是由 `cb` 是不是空决定的**（`src/net/uvcpp_tcp_client.cpp:1086-1089`）：
`cb` 非空 ⇒ 立即返回 0，完成时回调；`cb` 为空 ⇒ 退化成 `write_wait(data, len, 30000)`。

**这就是为什么回调里必须传 cb。** 事件循环回调里禁止同步等待，而三参数的
`write(data, len)` 省略第三个参数就是 30 秒的同步写。想让"发完不管"，传一个空 lambda。

---

## 9. UDP

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

## 10. TLS 接线

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
socket 之间流动（`src/net/uvcpp_tcp_client.h:161-173`）。所以 `web/` 那层一行都不用改。

三个例外：

- **`uvcpp_buf*` 的零拷贝在 TLS 上不成立**——加密要求明文过 `SSL_write`，它会拷一份，
  所以调用之后那个 `uvcpp_buf` **仍然是满的**，数据仍归它（`src/net/uvcpp_tcp_client.h:360-363`）。
- **握手完成前 `write()` 必然失败**，返回 `UV_ENOTCONN`。
- **握手失败的连接根本不会被交出来**：`on_connection` 一次都不调，只记在
  `last_error_code_` 里（`src/net/uvcpp_tcp_server.h:337-345`）。所以明文直连 TLS 端口时，
  上层"没被通知过"这条连接——这是有意的。

握手期连接不在任何上层登记表里，所以另有 `set_tls_handshake_timeout_ms()`
（默认 10000，`0` = 不设）防"喂 ClientHello 拖死"。

细节（证书怎么装、校验模式、ALPN）见 [TLS 与证书指南](./ssl-guide.md)。

---

## 11. 错误处理

- **`int` 返回：0 成功，失败是 libuv 的负错误码**（`src/net/uvcpp_tcp_server.h:146-147`）。
  最后一个是粘性的，`get_last_error()` 拿。
- 失败同时会置状态位（`TCP_SERVER_ERROR` / `TCP_CLIENT_ERROR`），`has_status()` 查。
- **同步/异步混用是抛异常**，不是错误码——见 §8。这是本模块唯一会抛的地方。
- `take_client()` 返回 `nullptr` 表示**所有权没变**（不是"出错了"）。

---

## 12. 典型坑

**`write(data, len)` 省掉第三个参数 = 同步写 30 秒。** 回调里这么写就是阻塞事件循环。
要"发完不管"就传一个空的 lambda 当第三个参数（正文里写不出那串字面量：反引号里的
`[]` 紧跟 `(` 会被文档门禁当成一条 markdown 链接）。

**在 `set_read_callback` 之后又在连接回调里 `read_start()`** ⇒ `UV_EALREADY`。

**不读的连接发现不了对端断开**，也不会被释放——一条静默的每连接泄漏。见 §6。

**在关闭回调里 `delete` 服务端管理的客户端** ⇒ 二次释放。

**`set_read_callback` 在 `listen()` 之后调不报错**，只是对已有连接无效。

**`net_read_result::data` 出了回调就没了**，别存那个指针。

**服务端示例里的 `uvcpp_buf*` 回调**（UDP）拿到的是栈上临时量，同样别存。

---

## 13. 没做的（如实列出）

- **没有 `on_connection` / `on_data` 这类 setter**。连接回调是 `listen()` 的第一个
  参数，数据回调是 `set_read_callback()`（服务端）或 `read_start*`（客户端）。
  旧文档里出现过的 `server.on_connection(...)` 在这个类上**不存在**。
- **`net_read_result` 只有两个判据**（`is_data` / `is_end`），没有 `is_error()`。
- **没有内置的背压/写队列上限**。异步写在飞时再写返回 `UV_EALREADY`，怎么排队是
  使用者的事。
- **没有连接池、没有自动重连**。`webapp` 的 WS 客户端有重连，这一层没有。
- **UDP 没有多播的封装**——`uvcpp_udp`（`handle/`）上有，这一层没透出来。
- **TLS 的 `PEER_STRICT` 与 `PEER` 行为完全相同**（见 [TLS 与证书指南](./ssl-guide.md)）。

---

相关文档：[低层指南](./lowlevel-guide.md)、[TLS 与证书指南](./ssl-guide.md)、
[webapp 应用框架开发者指南](./webapp-guide.md)、[项目 README](../README.zh.md)。

本页用到的头文件：`<net/uvcpp_net_read.h>`、`<net/uvcpp_tcp_server.h>`、
`<net/uvcpp_tcp_client.h>`、`<net/uvcpp_udp_server.h>`、`<ssl/uvcpp_ssl_context.h>`。
