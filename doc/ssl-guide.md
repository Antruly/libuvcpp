# TLS 与证书指南

`src/ssl/` 是**最小的一个模块**：三个头、三个公开类型。使用者真正要构造的只有一个
——`uvcpp_ssl_context`，它包一个 `SSL_CTX`（证书、私钥、CA、协议版本、校验模式、
ALPN 名单）。另一个 `uvcpp_ssl` 是**每连接**的包装，由 `uvcpp_tcp_client` 内部持有，
不是给一般使用者用的门面。

TLS 在这库里是**过滤层**，不是独立的传输实现：装到 `uvcpp_tcp_client` 之后，
`write()` 收明文、读回调交明文，密文只在内部与 socket 之间流动
（`src/net/uvcpp_tcp_client.h:161-173`）。所以 `web/` / `webapp/` 那条线一行都不用改。

- 打开方式：`-DUVCPP_ENABLE_OPENSSL=ON`
- 包含方式：`<ssl/uvcpp_ssl_context.h>`（构造上下文必须）；只声明变量的话
  `<net/uvcpp_tcp_server.h>` 就够，它只前置声明
- **不需要 OpenSSL 头**：三个头一律前置声明 + PIMPL（`src/ssl/uvcpp_ssl_context.h:26`
  那句 `<openssl/…>` 在**注释里**）

> 本指南里的签名、默认值、行为都对着当前源码核过。凡是"这一层没做"、或者
> **头注释与实现不一致**的地方都明确标出来——后者是这份文档里最值得看的部分。

---

## 目录

1. [这一层是什么](#1-这一层是什么)
2. [打开方式与包含](#2-打开方式与包含)
3. [最小可运行程序](#3-最小可运行程序)
4. [证书与私钥](#4-证书与私钥)
5. [校验模式](#5-校验模式)
6. [ALPN](#6-alpn)
7. [生命周期](#7-生命周期)
8. [错误处理](#8-错误处理)
9. [典型坑](#9-典型坑)
10. [没做的（如实列出）](#10-没做的如实列出)

---

## 1. 这一层是什么

| 类型 | 头 | 角色 |
|---|---|---|
| `uvcpp_ssl_context` | `src/ssl/uvcpp_ssl_context.h:31` | **主类型**。一个上下文可以共享给多条连接（`:8-9`） |
| `uvcpp_ssl` | `src/ssl/uvcpp_ssl.h:33` | 每连接包装（一个 `SSL*`），`uvcpp_tcp_client` 内部持有 |
| `ssl_detail::alpn_wire_format` | `src/ssl/uvcpp_ssl_common.h:84` | 把 ALPN 名单编成线格式的自由函数，实现细节 |

值类型在 `ssl/uvcpp_ssl_common.h`：`tls_version`、`tls_mode`、`tls_verify_mode`、
`tls_ctx_status`、`tls_cert_info`。

**没有 `uvcpp_ssl_server` 这种类。** 服务端 TLS 的落点是
`uvcpp_tcp_server::set_ssl_context()`。

三个头**整段**套在 `#if UVCPP_OPENSSL_ENABLE` 里（`src/ssl/uvcpp_ssl.h:17`、
`src/ssl/uvcpp_ssl_common.h:14`、`src/ssl/uvcpp_ssl_context.h:18`）。

---

## 2. 打开方式与包含

`UVCPP_OPENSSL_ENABLE` 由 CMake 选项 `UVCPP_ENABLE_OPENSSL` 决定，最终值写进生成头
`uvcpp/uvcpp_config.h`。

**不要自己 define 这个宏。** 外部定义且与本次构建不一致的值是硬 `#error`
（`cmake/uvcpp_config.h.in:56-61`）——理由是类布局会变，而不一致的布局**没有任何编译
或链接错误**，只会在运行时读到错位的垃圾。

关掉这个选项时：`src/ssl/` 会被从源文件与头文件列表里排除，**而且根本不安装 ssl 头**
（`CMakeLists.txt:1066-1070`）——使用者连 `#include <ssl/uvcpp_ssl_context.h>` 都会
找不到文件。同理，`uvcpp_tcp_server::set_ssl_context()` 在关掉时**连声明都不存在**。

运行时"关掉 TLS"不是宏，是 `set_ssl_context(nullptr)`。

---

## 3. 最小可运行程序

```cpp
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <net/uvcpp_tcp_server.h>
#include <ssl/uvcpp_ssl_context.h>

int main() {
  // 一个服务端一个上下文。TLS 版本显式给：默认值是 1.2，而枚举注释里写的是 1.3
  // （见 §10），别靠默认值。
  uvcpp::uvcpp_ssl_context ssl_ctx(uvcpp::tls_mode::SERVER,
                                   uvcpp::tls_version::TLS_1_2);
  if (!ssl_ctx.is_ready()) return 1;

  // 测试/内网用自签；真证书换成 §4 那两行
  if (!ssl_ctx.generate_self_signed("localhost", 2048)) return 1;
  if (!ssl_ctx.check_private_key()) return 1;

  uvcpp::uvcpp_tcp_server server;

  // 必须在 listen() 之前；裸指针、非拥有，ssl_ctx 必须活得更久
  server.set_ssl_context(&ssl_ctx);

  server.set_read_callback([](uvcpp::uvcpp_tcp_client& c,
                              const uvcpp::net_read_result& r) {
    if (r.is_data()) {
      // r.data / r.size 收到的已经是明文
      c.write(r.data, r.size, [](int) {});
    }
  });

  int rc = server.bind("127.0.0.1", 8443);
  if (rc != 0) return 1;

  // 连接回调只在**握手成功之后**才被调用
  rc = server.listen([](uvcpp::uvcpp_tcp_client* client) {
    const char kHello[] = "hello over TLS\n";
    client->write(kHello, sizeof(kHello) - 1, [](int) {});
  }, 128);
  if (rc != 0) return 1;

  server.run();
  return 0;
}
```

客户端侧要能连自签证书，必须**显式**关掉校验——这是有意的设计，让"关掉校验"在调用点
看得见（`src/ssl/uvcpp_ssl_context.cpp:110-112`）：

```cpp
#include <memory>

#include <ssl/uvcpp_ssl_context.h>

std::unique_ptr<uvcpp::uvcpp_ssl_context> doc_make_loopback_client() {
  // 上下文删了拷贝构造（src/ssl/uvcpp_ssl_context.h:34），而用户声明了拷贝构造就会
  // 抑制移动构造 —— 所以它既不能拷贝也不能按值返回，交给 unique_ptr 持有。
  std::unique_ptr<uvcpp::uvcpp_ssl_context> ctx(
      new uvcpp::uvcpp_ssl_context(uvcpp::tls_mode::CLIENT,
                                   uvcpp::tls_version::TLS_1_2));
  // 自签证书要连得上就必须关校验。让"关掉校验"在调用点看得见，是有意的设计。
  ctx->set_verify_mode(uvcpp::tls_verify_mode::NONE);
  return ctx;
}
```

---

## 4. 证书与私钥

| 方法 | 返回 | 要点 |
|---|---|---|
| `load_certificate_file(path)` | `bool` | 内部用 `SSL_CTX_use_certificate_chain_file` —— **必须是链**，否则中间证书不发，只信根 CA 的客户端握手失败（`src/ssl/uvcpp_ssl_context.cpp:133-147`） |
| `load_private_key_file(path)` | `bool` | 见下面的**顺序陷阱** |
| `load_certificate_data(pem)` | `bool` | 支持多证书 PEM：叶子 + 逐个 `extra_chain_cert` |
| `load_private_key_data(pem)` | `bool` | 同样附带配对检查 |
| `load_ca_file(path)` | `bool` | `SSL_CTX_load_verify_locations` |
| `generate_self_signed(cn, bits = 2048)` | `bool` | 现场生成 RSA + 自签 X509，有效期 365 天 |
| `check_private_key()` | `bool` | 显式配对检查 |

**顺序陷阱：先装证书，再装私钥。** `load_private_key_file` / `load_private_key_data`
**内部都会调 `SSL_CTX_check_private_key`**（`src/ssl/uvcpp_ssl_context.cpp:159`、`:212`）。
OpenSSL 在"尚无证书"时该调用返回 0，于是**先装私钥会返回 `false`**——而私钥其实已经
装进上下文了，属于"报了错但状态已改"。头注释只写了 "Load … from PEM"，没提这回事。

为什么值得单独调 `check_private_key()`：证书和私钥不配对**在 OpenSSL 里既不影响
`SSL_CTX_new`、也不影响两个 `load_*` 的返回值**——它只在**每一次握手**时才失败。
表现是"服务起来了、端口在听、每条连接建完就被拒"，很难往配置上想
（`src/ssl/uvcpp_ssl_context.h:66-69`）。

```cpp
#include <ssl/uvcpp_ssl_context.h>

bool doc_load_server_cert(uvcpp::uvcpp_ssl_context& ctx,
                          const char* cert_chain_pem,
                          const char* private_key_pem) {
  // 顺序不能反：私钥装载内部会做配对检查，那时证书还没进去就会返回 false
  if (!ctx.load_certificate_file(cert_chain_pem)) return false;
  if (!ctx.load_private_key_file(private_key_pem)) return false;
  // 把"不配对"提前到启动期报错，而不是留到每次握手
  return ctx.check_private_key();
}
```

`generate_self_signed` **只用于测试和受控内网**（`src/webapp/uvcpp_web_app.h:441-444`）。

---

## 5. 校验模式

```cpp
// doc-snippet: fragment — 枚举定义摘录，本页要展示的是"注释说校验主机名、实现不校验"
// 这个对照，注释本身就是内容，不能挪走。
enum class tls_verify_mode : uint8_t {
  NONE        = 0,   // 不校验对端证书
  PEER        = 1,   // 校验对端证书
  PEER_STRICT = 2,   // 注释写的是"校验对端 + 主机名" —— 但实现里没有主机名校验
};
```

**`PEER` 与 `PEER_STRICT` 行为完全相同。** `set_verify_mode` 把两个值都映射成
`SSL_VERIFY_PEER`（`src/ssl/uvcpp_ssl_context.cpp:278-279`），全仓 grep
`SSL_set1_host` / `X509_VERIFY_PARAM_set1_host` / `X509_VERIFY_PARAM_set1_ip`
**零命中**。枚举注释里那句 "Verify peer + hostname" 是**空承诺**——写了 `PEER_STRICT`
不会让主机名被校验。

默认值（`src/ssl/uvcpp_ssl_context.cpp:109-125`）：

| 模式 | 默认 |
|---|---|
| `CLIENT` | `SSL_VERIFY_PEER` + `SSL_CTX_set_default_verify_paths()`（装系统信任库） |
| `SERVER` | `SSL_VERIFY_NONE` |

**服务端默认没有信任库。** `set_default_verify_paths()` 只在 CLIENT 分支调
（`:116`）。所以服务端想做客户端证书认证时，必须自己 `load_ca_file(...)`。而且代码
只设 `SSL_VERIFY_PEER`，**从不设 `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`**
（`:277-280`）——服务端是"请求但非强制"要证书，要强制得在应用层用
`uvcpp_ssl::verify_peer()` 兜。

---

## 6. ALPN

```cpp
// doc-snippet: fragment — 接口签名摘录，与 §4、§7 那几张表同类的东西。
bool set_alpn_protos(const std::vector<std::string>& protos);       // 上下文级
void set_alpn_select_protos(const std::vector<std::string>& protos); // 服务端
bool has_alpn_select() const;
```

**这里有一个反过来的返回值约定：`SSL_CTX_set_alpn_protos` 成功返回 0**，与 OpenSSL
大多数接口相反（`src/ssl/uvcpp_ssl_context.cpp:338` 专门写了注释）。本库的包装把它
翻正成 `bool`，但如果绕到 `raw_ctx()` 自己调，记住这条。

其他要点：

- 名单里**空串或长度超过 255 字节的项会被静默丢掉**，不报错；全部丢完导致编码为空时
  `set_alpn_protos` 返回 `false`（`src/ssl/uvcpp_ssl_common.h:78-81`）。
- 服务端**挑不中时返回 `SSL_TLSEXT_ERR_NOACK`，不是 fatal**
  （`src/ssl/uvcpp_ssl_context.h:118-120`）——写成 fatal 会让所有老客户端连握手都完不成。
- `has_alpn_select()` 存在的理由是框架默认值与用户策略的冲突：`uvcpp_web_app` 默认要
  替使用者宣告 h2，但**不会覆盖已经显式设过的名单**（`:126-130`）。
- 每连接的覆盖在 `uvcpp_ssl::set_alpn_protos`，**必须在握手前**调。

---

## 7. 生命周期

**上下文是裸指针、非拥有**，每个消费点都这么写：

| 落点 | 声明 | 所有权说明 |
|---|---|---|
| `uvcpp_tcp_server::set_ssl_context` | `src/net/uvcpp_tcp_server.h:349` | "生命周期必须覆盖**整个服务端**，本服务端不持有它的所有权，也不负责释放"（`:342-344`） |
| `uvcpp_tcp_client::enable_tls` | `src/net/uvcpp_tcp_client.h:189` | "生命周期必须覆盖**整条连接**"（`:185`） |
| `uvcpp_web_app` | `src/webapp/uvcpp_web_app.cpp:1725` | 全库唯一"有人拥有"的一处：`std::shared_ptr<uvcpp_ssl_context>` |

顺序：`set_ssl_context` **必须在 `listen()` 之前**（`:346-347`），清空用
`set_ssl_context(nullptr)`。而且它是**在 loop 线程调用**的。

上下文对象本身的地址被 OpenSSL 长期持有——ALPN 选择回调通过 `arg` 拿到的就是成员
`alpn_select_wire_` 的地址（`src/ssl/uvcpp_ssl_context.h:159`，实现
`SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_cb, &alpn_select_wire_)`）。
**所以不要移动它、不要提前析构它。**

`uvcpp_ssl::use_memory_bio()` **必须在任何握手/IO 之前**调（`src/ssl/uvcpp_ssl.h:100`）。

---

## 8. 错误处理

装载类方法（`load_*` / `set_cipher_list` / `set_alpn_protos` / `check_private_key`）
返回 `bool`，失败原因在 `get_last_error()`（里面先把错误队列清空再把 `ERR_get_error()`
串起来，`"; "` 分隔）。`set_verify_mode` / `set_min_version` / `set_max_version` /
`set_alpn_select_protos` 返回 **`void`**——没有错误通道，只可能静默无效。

**握手返回值契约（最易踩，`src/ssl/uvcpp_ssl.h:90-98` 原文）：**

```
handshake(): 1 = 握手完成，0 = 需要更多 I/O，< 0 = 真出错
read/write(): > 0 = 处理的字节数，0 = 需要更多 I/O，< 0 = 真出错
```

所以**判失败要用 `< 0`，不能用 `!= 1`**：`0` 是"还没完"，是非阻塞下的常态。
这不是假想 bug——`tests/functional/web_ssl_func.cpp:152-156` 记录了
`uvcpp_ws_client` 的 `wss://` 路径曾经就是这么写错的，"于是它在非阻塞 socket 上
永远握不上手"。

另外两条同类的：

- `uvcpp_ssl::read()` 对 `SSL_ERROR_ZERO_RETURN`（对端干净关闭）**也返回 0**，与
  "需要更多数据"同值——只看返回值分不出来，要配 `last_ssl_error()`。
- `uvcpp_ssl::write(data, 0)` 返回 **-1**：零长度写在别处常是合法 no-op，这里算错误。

**握手失败怎么知道：**

- 服务端：握手成功才 `deliver_connection()`；失败**只记账**，`on_connection`
  **一次都不被调用**（`src/net/uvcpp_tcp_server.h:333-341`）。
- 客户端：`set_tls_ready_callback(cb)`，`status == 0` 成功；**只触发一次**，失败时在
  关闭回调**之前**触发（那时对象还活着）。
- 握手期连接不在上层登记表里，`idle_timeout_ms` 覆盖不到——所以有
  `set_tls_handshake_timeout_ms()`，默认 **10000**，`0` = 不设。

---

## 9. 典型坑

**`uvcpp_ssl_context ctx;` 编得过、链接不过。** `UVCPP_DEFINE_FUNC` 会声明一个默认
构造函数，但 `.cpp` 里**从未定义它**——唯一定义的是
`uvcpp_ssl_context(tls_mode, tls_version)`（`src/ssl/uvcpp_ssl_context.cpp:54`）。
必须传 `tls_mode`。`uvcpp_ssl` 同理。

**`is_ready()` 不反映证书装载失败。** `status_` **只在构造时**赋值
（`src/ssl/uvcpp_ssl_context.cpp:68,70,75,77`），之后任何 `load_*` 失败都**不改**它。而
`uvcpp_tcp_client::enable_tls()` 恰好只查 `ctx->is_ready()`
（`src/net/uvcpp_tcp_client.cpp:2109`）——所以**必须看各 `load_*` 的返回值**，
不能只看 `is_ready()`。

**先装私钥后装证书会返回 `false`**，见 §4。

**`PEER_STRICT` 不校验主机名**，见 §5。

**TLS 上 `uvcpp_buf*` 的零拷贝不成立**：调用之后那个 buf **仍然是满的**，
（`src/net/uvcpp_tcp_client.h:360-363`）。

**握手完成前写必然失败**（`UV_ENOTCONN`），`SSL_write` 要求握手已完成。

**同步/异步混用返回 `UV_ENOTSUP`**，而不是把明文写进一条已加密的连接
（`src/web/uvcpp_http_client.h:209-218`）。

**怎么确认过滤器真的接上了：** 在连接回调里断言 `is_tls_handshake_done()`，并对收到的
字节按预期**明文**内容比对。测试就是这么做的——`tests/functional/web_ssl_server_func.cpp:131-135`
的注释写着"收到的是明文才作数：过滤器没接上的话这里会是密文，逐字节比对会失败"。

---

## 10. 没做的（如实列出）

- **没有主机名校验。** `PEER_STRICT` 是空承诺，见 §5。要校验主机名得自己做。
- **完全没有 SNI。** 全仓 grep `SSL_set_tlsext_host_name` / `servername` **零命中**：
  客户端不发 SNI，服务端也不读。一个 IP 上挂多张证书的虚拟主机式 TLS 服务端**服务
  不了**，服务端也无法据 SNI 选证书。
- **服务端默认没有信任库**，且**不强制**客户端证书，见 §5。
- **没有 OCSP、没有 session ticket 配置、没有会话复用开关。**
- **默认 TLS 版本的头注释与实现对不上**：枚举把 `TLS_1_3` 标成 `(default)`
  （`src/ssl/uvcpp_ssl_common.h:30`），但构造函数默认值是 `tls_version::TLS_1_2`
  （`src/ssl/uvcpp_ssl_context.h:37`），而它**只被当作下限**（`min_proto_version`），
  上限不设。实际是"min = TLS1.2，max = OpenSSL 默认"。**建议显式传版本。**
- **线程安全没有承诺。** 上下文可以共享给多条连接（`SSL_CTX` 本身线程安全），但
  setter 的线程安全头里**没有说明**。按"配置阶段调完再 listen/connect"写。
- **`uvcpp_ssl` 不是给使用者的门面。** 它的 `handshake()`/`feed_ciphertext()` 那一套
  只在手写 TLS（内存 BIO）时需要，正常用法走 `uvcpp_tcp_client` 的 TLS 过滤层。

---

相关文档：[net 网络层指南](./net-guide.md)、[低层指南](./lowlevel-guide.md)、
[webapp 应用框架开发者指南](./webapp-guide.md)、[项目 README](../README.zh.md)。

本页用到的头文件：`<ssl/uvcpp_ssl_context.h>`、`<ssl/uvcpp_ssl.h>`、
`<ssl/uvcpp_ssl_common.h>`、`<net/uvcpp_tcp_server.h>`、`<net/uvcpp_tcp_client.h>`、
`<net/uvcpp_net_read.h>`。
