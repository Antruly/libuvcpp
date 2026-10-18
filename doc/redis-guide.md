# Redis 客户端指南

`src/redis/` 是**内置的 Redis 客户端**：RESP2 / RESP3 的编解码与解析都在本仓里，
传输直接建在 `uvcpp_tcp_client` 上。**它不依赖 hiredis，也不依赖任何第三方客户端库**
—— 所以这个模块默认就开着，装好 libuv 就能用。

- 打开方式：`-DUVCPP_ENABLE_REDIS=ON`（默认开）
- 包含方式：`<redis/uvcpp_redis.h>`
- 依赖：`UVCPP_BUILD_NET=ON`。net 关掉时本模块被**强制关闭**并打一条 WARNING
  （没有传输层，客户端无处落脚）
- 生成宏：`UVCPP_REDIS_ENABLE`（0/1）。模块关掉时公开头整段落在 `#if` 外面，
  这与 db / capi / yyjson 的写法一致

> 随包策略见 [`db-driver-policy.md`](db-driver-policy.md)：Redis 属于"仓里自己实现、
> 产物不需要额外客户端库"的那一档，与 SQLite 同类。

---

## 目录

1. [最小可运行程序](#1-最小可运行程序)
2. [连接 URL](#2-连接-url)
3. [值类型](#3-值类型)
4. [客户端 API](#4-客户端-api)
5. [协议契约](#5-协议契约)
6. [发布订阅](#6-发布订阅)
7. [TLS（rediss://）](#7-tlsrediss)
8. [生命周期与线程](#8-生命周期与线程)
9. [解析上限](#9-解析上限)
10. [测试](#10-测试)
11. [明确不做](#11-明确不做)

---

## 1. 最小可运行程序

```cpp
#include <redis/uvcpp_redis.h>
#include <handle/uvcpp_loop.h>

int main() {
  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_redis_client redis(&loop);

  redis.connect("redis://127.0.0.1:6379/0", [&](int st) {
    if (st != 0) {
      std::printf("connect failed: %s\n", redis.last_error().c_str());
      return;
    }
    std::vector<std::string> cmd;
    cmd.push_back("SET");
    cmd.push_back("k");
    cmd.push_back("v");
    redis.command(cmd, [&](int st2, const uvcpp::uvcpp_redis_value& r) {
      if (st2 == 0 && !r.is_error()) std::printf("SET: %s\n",
                                                 r.as_string().c_str());
      redis.close();
    });
  });

  loop.run(UV_RUN_DEFAULT);
  return 0;
}
```

不在循环上的调用方可以用同步版（自己泵循环，直到完成或超时）：

```cpp
// doc-snippet: fragment — 接着上一段用同一个 `loop`，不是独立的翻译单元
uvcpp::uvcpp_redis_client redis(&loop);
if (redis.connect_wait("redis://127.0.0.1:6379/0", 5000) == 0) {
  uvcpp::uvcpp_redis_value v;
  if (redis.command_wait({"PING"}, v, 5000) == 0) {
    std::puts(v.as_string().c_str());  // PONG
  }
}
```

> `connect_wait` / `command_wait` / `pipeline_wait` 会**自己跑调用方的循环**。循环
> 已经被别的对象跑着时不要用它们（与仓库里其它 `*_wait` 的规矩相同）。

---

## 2. 连接 URL

```
redis://[user:pass@]host[:port][/db]
rediss://[user:pass@]host[:port][/db]
```

| 部分 | 规则 |
|---|---|
| scheme | 只认 `redis` 与 `rediss`（大小写不敏感）。别的 scheme 返回 `UV_ENOTSUP` |
| 用户信息 | 最后一个 `@` 分隔；`user:pass` 两段，只有 `pass` 时写 `redis://:pass@host`。两段都按 RFC 3986 做 `%XX` 解码，非法转义返回 `UV_EINVAL` |
| 主机 | 域名、数字 IPv4，或方括号里的 IPv6（`redis://[::1]:6379/0`） |
| 端口 | 缺省 6379；非数字 / 0 / >65535 返回 `UV_EINVAL` |
| 路径 | `/N` 选 N 号库（只认十进制数字）；为空或 `/` 时**不发** `SELECT` |
| 查询串 | 忽略 —— Redis 的 URL 没有标准化过的查询参数，猜一个比不看更坏 |

URL 解析失败时 `connect()` **直接返回错误码且不触发回调**（与
`uvcpp_tcp_client::connect()` 同一约定），原因在 `last_error()` 里。

---

## 3. 值类型

`uvcpp_redis_value` 是**只读**的不可变值：内部是共享节点，拷贝只加引用计数。
没有构造侧 API —— 要构造请求，用 `uvcpp_redis_client::command()`。

| `uvcpp_redis_type` | 线上编码 | 取值方式 |
|---|---|---|
| `STATUS` | `+OK\r\n` | `as_string()` |
| `ERROR_REPLY` | `-ERR ...\r\n` / `!<len>` | `as_string()`；`is_error()` 为真 |
| `INTEGER` | `:1000\r\n` | `as_integer()` |
| `BULK` | `$5\r\nhello\r\n` | `as_string()` |
| `ARRAY` | `*2\r\n...` | `size()` / `at(i)` |
| `MAP` | `%2\r\n...`（RESP3） | `items()`（键值对，顺序即服务器顺序） |
| `SET` | `~2\r\n...`（RESP3） | `size()` / `at(i)` |
| `DOUBLE` | `,3.14\r\n`（RESP3） | `as_double()`，文本在 `as_string()` |
| `BOOLEAN` | `#t\r\n` / `#f\r\n`（RESP3） | `as_bool()`；`as_integer()` 给 1/0 |
| `NULL_VALUE` | `$-1` / `*-1` / `_\r\n` | `is_null()` |

几处刻意的归并：

- **大整数（`(`）与逐字字符串（`=`）都归到 `BULK`。** 它们的内容本来就是字节串；
  逐字字符串的 `txt:` 格式标签在解析时被剥掉（`as_string()` 只给负载）。
- **属性帧（`|`）对调用方不可见。** 它只是元数据，解析器读掉之后继续解析真正的值
  —— 所以一次 `command()` 永远只见到一个值。
- **越界与类型不符不抛异常**（本仓不用异常）：`as_string()` 给空串、`as_integer()`
  给 0、`at(i)` 越界返回静态空值。类型判断请调用方自己做（`type()` / `is_null()`）。

---

## 4. 客户端 API

```cpp
// doc-snippet: fragment — 接口清单，不是可编译的翻译单元
class uvcpp_redis_client {
 public:
  explicit uvcpp_redis_client(uvcpp_loop* loop = nullptr);  // nullptr = 自建循环
  ~uvcpp_redis_client();

  void set_resp_version(int version);     // 2（默认）/ 3，必须在 connect 之前
  void set_connect_timeout_ms(int ms);    // 默认 30000；0 = 不超时
  void set_command_timeout_ms(int ms);    // 默认 0 = 不超时（含管线）

  int connect(const std::string& url, std::function<void(int)> cb = nullptr);
  int connect_wait(const std::string& url, int timeout_ms = 30000);
  void close();
  bool is_connected() const;

  int command(const std::vector<std::string>& args,
              std::function<void(int, const uvcpp_redis_value&)> cb);
  int command_wait(const std::vector<std::string>& args,
                   uvcpp_redis_value& out, int timeout_ms = 30000);

  int pipeline(const std::vector<std::vector<std::string>>& commands,
               std::function<void(int, const std::vector<uvcpp_redis_value>&)> cb);
  int pipeline_wait(const std::vector<std::vector<std::string>>& commands,
                    std::vector<uvcpp_redis_value>& out, int timeout_ms = 30000);

  int subscribe(const std::vector<std::string>& channels,
                std::function<void(const std::string&, const uvcpp_redis_value&)> on_message,
                std::function<void(int)> cb = nullptr);
  int psubscribe(const std::vector<std::string>& patterns, /* 同上 */);
  int unsubscribe(const std::vector<std::string>& channels,
                  std::function<void(int)> cb = nullptr);

  const std::string& last_error() const;
};
```

参数按**字节**发送，不做编码或转义，二进制安全（可以塞 `\0`）。命令名不区分大小写
是**服务器**的规矩，本类原样发送。

`command()` 的返回值只说明"受理没受理"：

| 返回 | 含义 |
|---|---|
| `0` | 已排队 |
| `UV_ENOTCONN` | 连接没就绪（还没连上、或已经断了） |
| `UV_EINVAL` | 参数为空，或回调为空 |
| `UV_EALREADY` | 上一次 `connect()` 还没结束 |

---

## 5. 协议契约

### 5.1 `connect()` 的完成语义

回调触发时，**TCP →（可选 TLS）→ AUTH → SELECT → HELLO 全部完成**。顺序是：

1. RESP3（`set_resp_version(3)`）先发 `HELLO 3`；URL 带凭据时是
   `HELLO 3 AUTH <user|default> <pass>`，**只发这一条**（不再单独发 `AUTH`）。
2. RESP2 且 URL 带凭据：`AUTH <pass>` 或 `AUTH <user> <pass>`。
3. 库号非 0：`SELECT <n>`。

任何一步收到错误应答，`connect` 回调带非 0 status 触发，`last_error()` 是服务器原话：

| 失败的步骤 | status |
|---|---|
| `AUTH` | `UV_EACCES` |
| `HELLO` | `UV_ENOTSUP`（服务器太老，或 HELLO 的参数被拒） |
| `SELECT` | `UV_EPROTO` |
| 传输 / 超时 | libuv 错误码 / `UV_ETIMEDOUT` |

### 5.2 Redis 的错误应答不是传输错误

`-ERR` / `-WRONGTYPE` 这类应答**是一种应答**：回调的 `status == 0`，
`reply.type() == ERROR_REPLY`、`reply.is_error()` 为真、`reply.as_string()` 是服务器原话。
传输错误（断线、写失败、解析失败）才让 `status` 非 0。

这两类必须分开：前者说明"命令写错了"，重连没有意义；后者说明"连接没了"。

传输错误发生时会**把所有待决命令的回调各触发一次**（同一 status），然后连接进入
断开状态。

### 5.3 一条连接：一次一笔写，应答按 FIFO 配对

同一条连接上同一时刻只挂一笔 `write()`，其余命令进发送队列。RESP 没有请求 ID，
同一个连接上就是按顺序收发 —— `pipeline()` 只是把多条命令**编码进同一个缓冲**
（少若干次系统调用），配对规则不变。

管线的任何一条遇到传输错误，整组以同一个 status 结束；其中的 `-ERR` 不算失败，
逐条落在结果里。

### 5.4 超时

`set_command_timeout_ms(n)` 计的是"从命令排到**队头**、到它的应答到齐"这一段；
管线算**一条**（整组应答到齐才算完）。超时之后**连接被关闭**（不是只丢掉这一条），
所有待决命令以 `UV_ETIMEDOUT` 失败 —— 因为一条超时之后，后面哪条应答对应哪条
命令已经无法确定。

`command_wait()` 的 `timeout_ms` 是**本地**超时，不关连接，命令本身可能稍后才到。

---

## 6. 发布订阅

```cpp
// doc-snippet: fragment — 接着 §1 的 `redis` 对象，不是独立的翻译单元
redis.subscribe({"news"}, [](const std::string& channel,
                             const uvcpp::uvcpp_redis_value& message) {
  std::printf("[%s] %s\n", channel.c_str(), message.as_string().c_str());
});
```

- `subscribe(channels, on_message, cb)`：`cb` 是**订阅本身的应答**；
  `on_message` 之后收到的每条消息都从它进来。
- `psubscribe` 的回调拿到的是**真正的频道名**（不是模式），与 `redis-cli` 的
  `pmessage` 一致。
- `unsubscribe` 之后消息回调不再触发（服务器确认后生效）。
- 订阅消息与命令应答的分流规则：**先查待决命令队列，再查订阅**。RESP2 下两者是
  同一种编码（普通数组），顺序反了会把命令应答当消息吞掉。RESP3 的 push 帧（`>`）
  天然可分，直接路由给订阅；`message` / `pmessage` 之外、又没有待决命令的 push
  被丢弃（计数在内部，不对外）。

> **RESP2 的订阅模式有服务器侧限制**：进入订阅模式后只允许 `SUBSCRIBE` /
> `UNSUBSCRIBE` / `PSUBSCRIBE` / `PUNSUBSCRIBE` / `PING` / `QUIT` / `RESET`。
> 本类不拦这条 —— 服务器会回错误。要同时跑普通命令，开第二条连接（或切 RESP3）。

---

## 7. TLS（rediss://）

`rediss://` 会建一个 `tls_mode::CLIENT` 的 `uvcpp_ssl_context` 并给连接装上 TLS 过滤层：

- **校验档位是 `PEER`，信任库取自系统**（`uvcpp_ssl_context` 的 CLIENT 默认值），
  与 `redis-cli --tls` 一致。
- **本模块不提供"跳过校验"的开关**。自签证书的场景请把 CA 放进系统信任库，
  或者在你的程序里直接对 `uvcpp_ssl_context` 做设置后使用 `uvcpp_tcp_client`。
- `UVCPP_ENABLE_OPENSSL=OFF` 的构建里 `rediss://` 返回 `UV_ENOTSUP`。

---

## 8. 生命周期与线程

- **一个对象只属于一条循环、只属于一条线程。** 所有回调都在那条循环的线程上触发；
  不要从别的线程调本类的任何方法（唯一例外是把对象所有权转给别的线程，且转交前
  一条命令都不在飞）。
- `uvcpp_redis_client(uvcpp_loop* loop)`：
  - 传 `nullptr` ⇒ 自建一条循环，析构时一起销毁。自建循环由**调用方**跑
    （`run()`），客户端自己不会起线程。
  - 传外部循环 ⇒ 本对象不拥有它，也不负责跑它；它必须活得比本对象长。
- `close()`：所有待决命令的回调以 `UV_ECANCELED` 触发一次。在回调里删掉客户端
  是合法的（内部回调都拿 `weak_ptr`，每次入口才锁一份强引用）。
- 析构里**不回调**待决命令：对象已经交给析构了，再跑调用方的闭包没有意义，
  而且多半就在它自己的栈帧里。
- 重新 `connect()`（先 `close()` 或等它失败）会重建传输；上一代连接发出的
  读/写/连接完成回调全部作废（内部用代际计数区分），不会污染新连接的状态。

---

## 9. 解析上限

| 上限 | 默认值 | 超过时 |
|---|---|---|
| 单个 bulk | 512 MiB | 连接关闭，`last_error()` 说明原因 |
| 单个聚合的元素数（数组 / 集合 / push / 映射的**键值对×2**） | 1,048,576 | 同上 |
| 嵌套层数 | 64 | 同上 |

走到"解析失败"时本模块**不尝试恢复**：RESP 没有帧校验，一旦边界错了，后面的字节
无法再对齐。服务器给你的值如果真能超过这些上限，请在协议层之外解决（例如换
`STREAM` / 分块读取），而不是调大这三个数 —— 它们只在私有解析器上，不是公开 API。

---

## 10. 测试

| 用例 | 内容 | 是否需要外部服务 |
|---|---|---|
| `redis_resp_func` | 编解码与增量解析：RESP2/RESP3 全部类型、逐字节拆包、粘包、坏输入与上限 | 否 |
| `redis_client_func` | 进程内假 Redis 服务端（`uvcpp_tcp_server` + 同一个解析器）测 AUTH/SELECT/HELLO 3、pipeline 顺序、错误应答、分段到达、订阅、命令超时、URL 错误 | 否 |
| `redis_real_func` | 对真实 `redis-server`：SET/GET/INCR/LPUSH/LRANGE/管线/发布订阅 | 是 |

真实用例的环境变量：

- `UVCPP_REDIS_TEST_URL`：默认 `redis://127.0.0.1:6379/0`。
- 连不上时返回 **3**，ctest 把它报成 `***Skipped`（`SKIP_RETURN_CODE 3`）。
- `UVCPP_REDIS_TEST_REQUIRE=1`：连不上是**失败**。CI 的腿上起了 `redis-server`
  并设这一条 —— 服务都起了还跳过，说明配置写错了。
- 用例只碰带 `uvcpp:test:<时间戳>:` 前缀的键，末尾统一 `DEL`。

```bash
redis-server --port 6399 --save '' --appendonly no --daemonize yes
UVCPP_REDIS_TEST_URL=redis://127.0.0.1:6399/0 \
  ctest --test-dir build -R redis --output-on-failure
```

---

## 11. 明确不做

- **Cluster / Sentinel**：连接串里的主从与分片策略不在本类里。
- **连接池**：一个对象一条连接。要并发就开 N 个对象，或者按 db 模块的
  `uvcpp_db_pool` 那个形状自己包一层。
- **Lua 专用 API**：`command({"EVAL", script, "0", ...})` 本来就能发。
- **hiredis 兼容层**：没有，也不打算有 —— 本模块的公开面是 `uvcpp_redis_value`。
- **`CLIENT TRACKING` / 客户端缓存**：用 `command()` 发原始命令可以做，但没有
  专用的失效通知回调。

---

## 相关文档

- [`db-driver-policy.md`](db-driver-policy.md)：什么随包、什么不随包、缺库时什么现象
- [`net-guide.md`](net-guide.md)：`uvcpp_tcp_client` 的读/写/关闭契约（本模块的传输层）
- [`db-guide.md`](db-guide.md)：SQL 后端；与 Redis 没有共用代码，只有"客户端怎么分片"
  这一点思路相通
