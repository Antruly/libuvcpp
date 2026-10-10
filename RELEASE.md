# libuvcpp Release Notes

<!-- 这份文件是**归档**，不是任何一版的正文。

     下面每个 `## vX.Y.Z 重点 (Highlights)` 小节记的都是那一版发出去时的样子。
     重发历史 tag（`gh release edit`）时还要按原样取用，所以旧小节**不追改** ——
     要补后来的事实，补进 CHANGELOG.md，不要回头改这里的旧段落。

     发版时的 Release 正文**不在这里**：它由 `release_notes.py` 按 tag 从本文件
     切出该版本那一节（标题里含版本记号的那个 `##`），前面缀一张按平台排的下载表，
     再交给 `gh release --notes-file`。Release 的标题由标签单独给
     （`--title "libuvcpp $TAG"`），所以本文件自己的标题里仍然不写版本号。 -->

## 简介 (Introduction)

**libuvcpp** 是一个基于 libuv 的现代 C++ 封装库，提供简洁的面向对象接口来使用 libuv 的异步 I/O 功能。

## v1.6.0 重点 (Highlights)

`1.5.1` → `1.5.3` 这条开发线收进这一版：**新增数据库模块**（含它在 C ABI 里的第八片）、
QUIC 传输上四处性能与正确性改动、以及发布包的两轮扩容。

### 新增模块：数据库（`src/db/`）— `UVCPP_ENABLE_DB=ON`

一个连接一个 `uvcpp_db_client`，**同一套接口盖住 SQLite / MySQL / PostgreSQL** 三个
后端，结果行按表取（列名或列号）。**默认关**，而且是本库**唯一需要第三方客户端库**
的模块（libsqlite3 / libmysqlclient / libpq）。三个后端一个都没找到时整个模块被强制
关掉并打一条说明怎么修的 warning，而不是留一个"能配置、链不上"的组合。

- **占位符是方言差异，不换算。** 写 `$1` 是 PostgreSQL 的语法，写 `?` 是另外两家的；
  本库**原样透传**给你选的那个后端，不做 `?` → `$n` 的改写。这一条实测过六种组合。
- **事务里不重试。** 失败后的重试会把语句挪出那个事务本身，所以门面不替你重试。
- **`DECIMAL` 有代价**：它既到不了浮点也到不了整型，代价写在 `doc/db-guide.md` 里。
- **异步门面 `uvcpp_db_async`**（回调 + `future`）**不写 `uv_queue_work`** —— 要跑在
  工作线程池上的样板它替你收掉了。
- **连接池**建在同一条连接类型上，把并发从 1 抬到 N；借出的 client 不许 `free`、池子
  被门面绑着时也不许 `free`，这两条都被变异测试钉住（M22–M26）。

### C ABI 第八片：db —— 402 个函数 / 八片

`uvcpp_c_db.h` 是第八片，**81 个入口**（连接 / 同步查询与事务 / 参数 / 结果集与值 /
连接池 / 异步门面），到此合计 **402 个函数**。C#、Rust、Python 的 `ctypes` 今天就能
用数据库模块。

- **`UVCPP_C_ABI_VERSION` 仍是 `1`。** 加一片不 +1 的判据这次**比前几批更直接**：
  `git diff --stat` 在 `src/capi/` 上只有一行（伞头多一段 `#if UVCPP_DB_ENABLE` 的
  include），既有的七份头与七份 `.cpp` 一个字节都没动。
- **C 的异步接口不收循环参数 —— 这是它与 C++ 那侧唯一一处形状差别。** 公开头里造不出
  一个合法的 `uvcpp_loop*`，而"借调用方自己的 `uv_loop_t*`"会让 `~uvcpp_loop()` 去
  `uv_loop_close()` 并释放一块别人的内存。给一个没人填得合法的参数比不给更糟，所以完成
  回调一律在门面自带的那条懒起循环线程上跑；要把结果搬回自己的循环，就在回调里投一次
  `uvcpp_c_net.h` 那族 `*_post()`。
- **C# 绑定仍覆盖前七片**（321 of 402）—— db 那 81 条是它自己 README 里如实列出的缺口，
  不是漏写。

### 发布包：两轮扩容

六条发布腿的预编译包在这一版里**多带两个模块**：

| 版本 | 多了什么 | 断言清单 |
|---|---|---|
| `1.5.2` | WSDL / SOAP | 十个宏 → 十一个 |
| `1.5.3` | 数据库（**只带 SQLite 一个后端**） | 十一个 → 十三个 |

WSDL 那次值得单独说，因为它的失败方式很安静：包里**照样装着** `include/wsdl/*.h`
（那份清单由打包脚本的模块表决定，与开关无关），但生成头里 `UVCPP_WSDL_ENABLE` 是 0，
于是那些头的全部内容落在 `#if` 外面 —— **头在、功能不在**。

db 那次的两个 `OFF`（MySQL / PostgreSQL）是**显式**关的，不是"找不到"：发布 runner 上
装着 `libpq-dev`，`find_package` 会**静默**成功，于是共享库悄悄多一条 `libpq.so.5` 的
`DT_NEEDED` —— 而"装到别的机器上跑不起来"正是发布腿那条 `ldd` 断言要拦的形状。
`PRIVATE` 链接挡不住这件事：`PRIVATE` 关的是头文件与编译定义，共享库上它照样写
`DT_NEEDED`。SQLite 那份也**不链系统的**：走配置期下的一份**哈希钉死**的 amalgamation，
编成静态且强制 PIC 链进去（Ubuntu 24.04 上系统的 `libsqlite3.a` 不是 PIC）。

两轮的判据都**读生成的头**（`<tree>/include/uvcpp/uvcpp_config.h`）而不是
`CMakeCache.txt` —— 模块被强制关闭时走的是普通 `set()`，缓存里照旧写着 `ON` 而编译器
看到 0，只 grep 缓存的断言会**放行一个缺功能的包**。

### QUIC：两处正确性、两处性能

- **大载荷下交付出去的字节是错的（`1.5.1`）** —— 这是本版最重要的一处修复。
  `write_stream()` 把 `std::vector<uint8_t>` 里的一个指针交给 ngtcp2，而那块缓冲会
  **增长**（把地址搬走），丢已确认前缀时又会把尾巴 `memmove` 上来（地址没变、字节变了）；
  ngtcp2 要求在字节被确认前它们"原样留着"。症状是 2 MiB 回显 20 次里崩 3 次
  （`0xC0000005`），而崩只是它**最响**的那种症状：把 HEAD 那份存储换回来重编、跑同一个
  量具，**6 次运行全部**在 15 轮内报 `DATA MISMATCH` —— 旧形状每次吐出的字节都是错的，
  那个基线在 2 MiB 上根本不存在。正式修法是**分块发送队列**：每块出生时预留、此后只写
  到这里为止、只在整块被确认时才丢，两条约束按构造成立。
- **数据报上限钉在 1200，把 ngtcp2 自带的 PMTUD 整条关死（`1.5.1`）**。1200 是"任何
  路径都保证能过"的**下界**，看着最保守，实际让探测表 `{1406, 1342, 1232, 1444}`
  四档全部越界、PMTUD 当场判完成并被关掉，此后每个数据报都被钉死在 1200 字节 ——
  2 MiB 单向要 1748 个数据报。改成 1500（典型以太网 MTU）后，交错跑五轮取最小值，
  接收侧 `on_read(DATA)` 次数**对照组 1743～1766、改完 1392～1446，两组一次都没有
  重叠**（墙钟在那个分辨率上分不出结论，这一行才是结构证据），全尺寸扫描每一档都
  变快（+4.2% ～ +23.8%）。两条 `static_assert` 守着这个缺陷的两个入口，都拿对照臂
  验过（改回 1200 ⇒ 构建失败）。
- **ACK 阈值从 ngtcp2 默认的 2 提到 16（`1.5.1`）**。低 RTT 链路上延迟 ACK 的窗口塌到
  微秒级，阈值成了唯一还在起作用的闸门，接收端几乎每收两个包就回一个 ACK。实测 2 MiB
  单向总数据报 **2445 → 1783（−27%）**、中位耗时 **19.25 → 13.81 ms**。**成本说清楚**：
  ACK 变稀 ⇒ 对端检测丢包、推进 cwnd 的反馈变慢，上界是延迟 ACK 计时器（25 ms）；本机
  回环无丢包，量到的全是收益，别把这个数读成"公网也 +38%"。
- **Windows 发送侧批量交数据报（`1.5.2`）**。`UVCPP_ENABLE_UDP_GSO` 把一次聚合出来的
  一批等长数据报用一次 `WSASendTo` 交给协议栈，而不是每个数据报一次（libuv 在 Windows
  上是一个数据报一次 `WSASendTo`）。分段尺寸取的是本次聚合写回填的值、不是常量，所以
  线上跑的还是原来那些数据报。**Windows only** —— Linux 上游已经把每批折成一次
  `sendmmsg()`，这个开关在那里整段编掉。收方向**故意没做**（半开会让 libuv 把粘在一起
  的数据报当成一次读交给应用）。

### 换二进制之前

- **C++ API 没有破坏性改动**；**C ABI 版本仍是 `1`**，本版**新增**了 db 那一片
  （81 个 `uvcpp_c_db_*`），**没有删除或改名**任何既有符号。
- **发布包的模块集合变了**：换上 `1.6.0` 的包之后，`UVCPP_WSDL_ENABLE` 与
  `UVCPP_DB_ENABLE` 由 `0` 变 `1`。**从源码构建的人不受影响** —— 两个开关的默认值仍是
  `OFF`。
- 包里 `uvcpp_db_drivers()` 只打印 `sqlite`；要 MySQL / PostgreSQL 就从源码编
  （默认三个后端都开）。

逐条的「这一版没做什么」在 [`doc/db-guide.md`](doc/db-guide.md)、
[`doc/quic-guide.md`](doc/quic-guide.md) 与 [`doc/http3-guide.md`](doc/http3-guide.md)；
按主题汇总的清单在这个 tag 对应的
[CHANGELOG.md](https://github.com/Antruly/libuvcpp/blob/master/CHANGELOG.md)。

## v1.5.0 重点 (Highlights)

`1.4.1` → `1.4.4` 这条开发线（HTTP/2、QUIC、HTTP/3 的 C 面，共 321 个函数）连同上
面那几件 `1.5.0` 自己的改动，一起收进这一版。

### 发布包：六条腿的预编译二进制从此都是全功能的

这一版**最大的一处用户可见变化**在这里。此前的发布包**不带 QUIC / HTTP/3**（那两个
开关默认 OFF，发布腿也不传）；`1.5.0` 起六条腿（linux-x64 / linux-arm64 / mingw-x64 /
mingw-arm64 / msvc-x64 / msvc-arm64）的**发布档与调试档都带上 QUIC + HTTP/3 + C ABI**。

判据**不是**"我们传了 `-D`"，而是**读生成的头**：每条腿各加一步断言，读
`<tree>/include/uvcpp/uvcpp_config.h` 里那十个宏（`UVCPP_NET_ENABLE`、
`UVCPP_WEB_ENABLE`、`UVCPP_WEBAPP_ENABLE`、`UVCPP_ENABLE_MEMORY_POOL`、
`UVCPP_ZLIB_ENABLE`、`UVCPP_OPENSSL_ENABLE`、`UVCPP_NGHTTP2_ENABLE`、
`UVCPP_CAPI_ENABLE`、`UVCPP_QUIC_ENABLE`、`UVCPP_HTTP3_ENABLE`）。**为什么不能 grep
`CMakeCache.txt`**：QUIC / HTTP3 缺依赖时走的是 `message(WARNING)` + **普通变量**
`set(... OFF)`，于是缓存里照旧写着 `=ON` 而编译器看到的是 0 —— 只 grep 缓存的断言会
**放行一个缺功能的包**，而"发出去的包缺功能"正是它要拦的那件事。生成的头是编译器真正
读到的那个数。（名字映射也不是按名字猜的：第一版把 `UVCPP_BUILD_EXPAND` 写成
`UVCPP_WSDL_ENABLE`，拿真生成头实测时当场红了一条。）

QUIC 的前提是一份**带 QUIC API 的 OpenSSL ≥ 3.2**，而六条腿的来路各不相同（逐条记在
`release.yml` 文件头那张表里，改任何一条之前先看它）：

| 腿 | 这份 OpenSSL 从哪来 |
|---|---|
| linux-x64 / linux-arm64 | **不装** Ubuntu 22.04 的 `libssl-dev`（3.0.2，没有 QUIC API —— 装上只是给 `find_package` 添一份不合用的候选），job 里自建 **OpenSSL 3.5.0**（`no-shared no-tests -fPIC`；`-fPIC` **不是保险是承重的**，本机量过同一份源码两种命令行生成的 `CFLAGS`）。编完当场 `nm --defined-only libssl.a` 查 `SSL_set_quic_tls_cbs` **有定义** |
| mingw-x64 / mingw-arm64 | MSYS2 包的 `openssl`（今天 3.6.x，≥ 3.2） |
| msvc-x64 | runner 脚本给的那份（`.github/scripts/win-openssl-deps.sh`）；不够 3.2 就会被预检静默关掉 QUIC，从而被断言判红 |
| msvc-arm64 | 镜像上没有 arm64 的，这条腿自己拿 `VC-WIN64-ARM no-asm` 编 3.5.8；另外把"这份 OpenSSL 确实带 QUIC API"从配置期探测结果（缓存变量 `UVCPP_OPENSSL_HAS_QUIC_CBS`）里读回来，好把"OpenSSL 不对"与"开关没传"两类红分开报 |

依赖仍然**全静态**：libuv / llhttp / zlib / OpenSSL / nghttp2 / **ngtcp2** / **nghttp3**。
Linux 上 `ldd libuvcpp.so` 只剩 `libc` 与 `ld-linux`（那条自包含断言 `1.5.0` 起把
ngtcp2 / nghttp3 也写进名单 —— 它们今天被本仓以 `ENABLE_SHARED_LIB=OFF` 压成静态、
**不可能**出现在 `ldd` 里，写进去是为了让"哪天有人把上游那个开关改成 ON"变成一条当场
看得见的红）。

### C# 绑定（`bindings/csharp/`）

`321` 条 `DllImport`（`UvcppNative.cs` 183 + `UvcppNative.Protocols.cs` 138，同一个
partial 类的两半）+ 一个能跑的 **QUIC 回显例子**（`dotnet run` 默认做一次回环：服务端
收、回显、客户端收到同样的字节，逐字节比，收尾断言活句柄数回到 0）。**判据是"一条不
多、一条不少"**：与 `tests/tools/capi_symbols.lock` 里 `321` 个 `uvcpp_c_*` 符号**双向
对账**，两个方向的差集都为空 —— 一段纯文本脚本就能核，不需要库、不需要编译。用法、
动态库名字那一栏（Windows 上 MinGW 包与 MSVC 包**不一样**）与"别手写声明"的理由都在
[`bindings/csharp/README.md`](bindings/csharp/README.md)。

例子的价值不只是一份示例：它第一次跑就撞出库里的一个必现 SIGSEGV（带着活连接
`uvcpp_c_quic_server_free()`），见下面「修复」。

### 修复

- **QUIC 端点带着活连接释放会 SIGSEGV**（1.5.0）。`uvcpp_c_quic_server_free()` 边遍历
  `server->conns` 边 `detach_conn()`，而后者的"反登记"那一半会从这张表里 `erase` ——
  `unordered_map::erase` 把**当前迭代器**弄失效，`++it` 踩在已回收的桶上；表里只有一条
  连接也照样崩。修复是先把句柄抄进一个局部数组，再统一 detach。回归用例
  `test_free_with_live_conn()`（自签 TLS + 一次真握手，**不 close 也不 stop** 直接 free
  两端 + 两个 TLS 上下文，再断言登记表回到进场时的数）与变异 M21 一起进 —— 反空转是
  量出来的：把修复拿掉重跑，这条用例 SIGSEGV。
- **`uvcpp_c_quic.h` 补上一处会让人写错两遍的文档**（1.5.0）：QUIC 的 `on_read` 收尾是
  **两条**回调 —— 一个"带数据和 FIN 的 STREAM 帧"先报一条 `DATA`（**那条 `DATA` 的
  `fin` 也是非 0**），紧接着再报一条 `PEER_CLOSED`。所以**流的结束判据是
  `PEER_CLOSED`，不是 `fin`**。例子第一版拿 `fin` 当结束判据，于是回显了两次，第二次写
  方向已经关了（ngtcp2 给 -219），异常从 native 调进来的回调里逃出去 → abort（本机实测
  退出码 134）。

### 换二进制之前

- **C++ API 没有破坏性改动**；**C ABI 版本仍是 `1`**（`uvcpp_c_abi_version()` 返回 1），
  这一版**没有**新增、删除或改名任何 `uvcpp_c_*` 符号 —— 符号面仍是 321 条。
- 但**发布包的模块集合变了**：拿 1.5.0 的包接上去之后，`UVCPP_QUIC_ENABLE` /
  `UVCPP_HTTP3_ENABLE` / `UVCPP_CAPI_ENABLE` 由 `0` 变 `1`，链接期会多出这三批符号与
  头。**从源码构建的人不受影响** —— 三个开关的默认值仍是 `OFF`，`full` 那条 CI 格也
  没有替谁改默认。
- 拿新包但**不**想用这几片的人什么都不用做：多出来的头是惰性的，模块宏由包里的
  `include/uvcpp/uvcpp_config.h` 给出。

## v1.4.0 重点 (Highlights)

`1.3.1` → `1.3.33` 这 33 个开发档全部收进这一版。**这一版有破坏性改动**：日志模块
动了一处 vtable、一处对象布局和四个重载集（逐条见本节末「换二进制之前」）。

### 新增

| 新增 | 说明 | 档 |
|---|---|---|
| **SOAP 1.1/1.2 + WSDL 1.1** | 新模块 `src/wsdl/`（`UVCPP_ENABLE_WSDL`，默认 OFF）：WSDL 文档模型与发布层、SOAP 信封解析 / Fault / 序列化、operation 派发层（派发键、九种拒绝、响应包装）。依赖 pugixml，随包静态链接 | `1.3.7-dev`、`1.3.8-dev` |
| **应用层 JSON** | `uvcpp_json` 构造器与字段表反射（`UVCPP_JSON_REFLECT`），不用再手写 `to_string` | `1.3.3-dev`、`1.3.4-dev` |
| **日志模块完善** | 见下节 | `1.4.0` |
| 多循环的 Linux 分流 | `set_loops(n > 1)` 在 Linux/BSD 上改为**内核分流**（n 条循环各自 `UV_TCP_REUSEPORT` 绑同一端口），Windows 仍是转手；`is_fanout()` 可问是哪一种 | `1.3.5-dev` |
| `Date` 响应头 | 按秒缓存，不再每响应 `strftime` | `1.3.1-dev` |
| 进程内设 libuv 线程池 | `uvcpp_set_threadpool_size()`，上限来源收口成一个常量 | `1.3.2-dev` |
| 静态分片下发的名额闸门 | 按块借还工作池名额，拿不到名额时不再回 503 | `1.3.6-dev` |

### 日志模块（`1.4.0`）

- **看得到位置了**：新增 `UVCPP_LOGF(level, category, fmt, ...)`，注入
  `__FILE__` / `__LINE__` / 函数名。访问日志原先**全表唯一**没有位置的一条，现在带
  `(uvcpp_web_middleware.cpp:108)`。
- **`<<` 能打的东西变多了**：`enum class`（底层整数）、`log_level` / `log_category`
  （**名字**）、`nullptr`（输出 `null` —— 此前 `<< nullptr` 是**编译错误**，`const char*`
  与 `const void*` 两个候选打平）。
- **浮点不再丢精度**：改最短往返表示，`0.1` 仍是 `0.1`，而 16 位有效数字不会被 `%g`
  截成 6 位。`float` 与 `double` 分开处理。
- **两个新模块标签** `SOAP` / `WSDL`，并给 `src/wsdl/` 补上 9 处日志点
  （`uvcpp_soap_service.cpp` 8 处：1 `DEBUG` / 3 `ERR` / 4 `WARN`，
  `uvcpp_wsdl_serve.cpp` 1 处 `WARN`）；
  `log_category::JSON` 此前**全仓 0 处使用**，现在序列化失败会记一条 ——
  那个失败此前与"空 JSON 文档"完全不可区分（`resp.json(j)` 会发出 200 + 零字节正文）。
- **`flush()`**：`uvcpp_logger::flush()` / `uvcpp_log_sink::flush()`。stdout 重定向到
  文件时是块缓冲的，崩溃或被 `_exit()` 会丢掉最后一段日志。
- **少几把锁、少几次分配**：等级改 `relaxed` 原子读 ⇒ **被过滤掉的调用点不再抢全局
  互斥量，也不再分配、不再拼装**；`logger::write()` 把 sink 调用移出锁（锁内只快照
  sink 指针）⇒ 每条**真的打出去**的记录少持一次锁，顺带解掉「sink 里再打日志会自死锁」；
  控制台 sink 的 `min_level_` 同样改原子 ⇒ 每条的 `should_log()` 免锁；
  `uvcpp_logf` 不再永远格式化两遍（先试 512 B 栈缓冲，只有真截断才走堆）；
  删掉 `buf_.reserve(128)` ⇒ 每条日志不再先来一次堆分配；`record.message` 两处改移动。

  **量程要说准**：控制台 sink **自己那把锁与那次 `fwrite`** 仍在（消掉它要动异步设计），
  所以别把这笔读成"日志开销归零"。

### 修复

- **一个从来没有链过的公开类型** —— `uvcpp_console_log_options` 自 `1.2.0` 起就缺
  `UVCPP_API`，于是共享库版（`UVCPP_BUILD_SHARED=ON`）下它的构造函数**不在导出表里**：
  指南 §9 教的 `uvcpp_console_log_options opt;` 只要真的写进使用者的 TU 就是
  `LNK2019`。它能躲过所有门禁有两个原因 —— 片段门禁**只编不链**
  （`tests/tools/check_doc_snippets.py` 传给编译器的是 `-c`），而库自己的 TU 带
  `UVCPP_EXPORTS`，只有**吃 import lib 的另一个 TU** 才会撞上；仓内此前也确实没有
  一处构造过它（要自定义选项的人都走 `sink.options()`）。现已补上 `UVCPP_API`，
  并有了第一条真的链接它的用例。

- **启动钩子抛异常时，属主建的句柄没有任何回收点**（外部贡献者
  [@sercebr](https://github.com/sercebr) 在
  [#32](https://github.com/Antruly/libuvcpp/issues/32) 报的）。`uvcpp_loop_worker`
  的必填钩子 `set_on_start()` 抛异常时，那条分支既没跑 `set_on_exit()`、也没把循环
  泵干，于是属主在钩子里建的那些句柄一直挂着。触发条件是**内存分配失败**
  （`std::bad_alloc`），所以它不是常见路径，但一旦踩到就是每次启动漏一块。
  修法是那一支补上 `set_on_exit()`，并把「摘句柄 → 泵 → 关循环」抽成正常路径与
  失败路径共用的一段，免得两处各自演化。

  这一条报的是**悬垂写**（use-after-free）。实测下来后果是**泄漏**而不是悬垂：
  `uvcpp_loop::loop_close()` 只在 `uv_loop_close()` 返回 0 时才置关闭位，钩子建的
  句柄还开着就返回 `UV_EBUSY`，于是析构走的是"故意漏掉这一整块、换掉一个悬垂"
  那条分支 —— 那块内存是**泄漏但有效**的。两道互相独立的防线都指向同一结论。
  三臂实测（真·修前 / 只补钩子不泵 / 修好）确认**补钩子与泵两步都不可或缺**：
  少任何一步，泄漏都还在。

### 实测数据

本版**不发布吞吐数字**。日志这一笔改的是"少抢几次锁、少几次分配"这类结构性开销，
用本机这个回环靶场去量，前后差落在噪声里（该靶场上服务端 CPU 绝大部分花在内核，
库自身占比很小），拿它当结论等于把噪声当信号。要自己量：`-DUVCPP_BUILD_BENCH=ON`
后跑 `uvcpp_bench_server`，它带 `--access-log` 开关可以整开整关；同机多轮取最小值
再比，**绝对值不跨机可比**。

功能与回归侧有实测：

- **回归**：`build-log`（VS 2022 x64 Release，WEBAPP + OPENSSL + WSDL + BENCH 全开）
  **113/113 通过**；`build-h2`（同生成器，WSDL=OFF）**109/109 通过**。两棵都是全量
  重编，`error C` 计数 0。
- **变异**：日志改动配的 **10 条变异体全部被现有用例抓住**，没有一条是靠超时兜底
  蒙混过去的。其中"等级该不该被配置覆盖"是**一对反向用例** —— 单跑任何一条，
  "永远覆盖"或"永远不覆盖"都能骗过去，两条一起才把那个开关钉住。

### 换二进制之前

相对上一次发布（`v1.3.0`），这一版动过的东西分两类。

**一、日志模块的 ABI 变化 —— 旧二进制必须重编**

| 变化 | 为什么 |
|---|---|
| `uvcpp_log_sink` 新增**虚函数** `flush()` | **vtable 布局变了**。给了默认空实现，所以**源码**兼容；但任何继承它的既有二进制 sink 必须重编 |
| `log_category` 在哨兵前插入 `SOAP` / `WSDL` | `CATEGORY_COUNT` 由 15 变 17，`uvcpp_logger::category_levels_` 变大 ⇒ **`uvcpp_logger` 的对象布局变了**。硬编码过 15、或缓存过某个 category 数值的代码会**静默错位** |
| 新增 `UVCPP_API` 符号 | `uvcpp_logf_at(...)`、`uvcpp_logger::flush()`，以及**补导出的** `uvcpp_console_log_options()`（见上节「修复」）。前两个是新增功能，第三个是修复 —— 三条都是导出面**增加**，不强制旧二进制重编 |
| `uvcpp_log_stream` 的重载集变了 | 新增模板 `operator<<` 与 `nullptr_t` 重载 ⇒ **无作用域枚举**实参的绑定从整型提升变成精确匹配。输出值不变，但重载解析变了 |

**二、一条行为变更（不改代码也能观察到）**

`uvcpp_logger::set_level()` 在 `app.start()` **之前**调用的，此前会被
`init_process_once()` **无条件顶回** `cfg_.min_log_level`（默认 `INFO`）且不给任何提示。
现在**只有**显式调用过 `app.set_log_level()` 才会套用配置值：

- 调过 `app.set_log_level(X)` ⇒ `X` 生效，后调用的那个赢；
- 没调过 ⇒ 你在 `start()` 之前设的全局等级**原样保留**。

本仓的靶场正踩在这个坑里：`bench/bench_server --log-level WARN` 的等级被顶成 `INFO`
（`TRACE`/`DEBUG` 则被压成 `INFO`），而自报那行印的是**请求值**，读数整个是假的。
修好之后自报值改印生效值。

## v1.3.0 重点 (Highlights)

`1.2.1` → `1.2.25` 这 25 个开发档全部收进这一版。**这一版有破坏性改动**：3 个公开符号
被删除，另有 7 处布局或导出符号变过 —— **必须重编，不能只换二进制**（逐条见本节末
「换二进制之前」）。这也是 `1.2.x` 这条线的收口。

### 新增

| 新增 | 说明 |
|---|---|
| **多循环横向扩展** | `uvcpp_tcp_server::set_loops(n)`（`1.2.21`）与 `uvcpp_web_app::set_loops(n)`（`1.2.23`）：**一个接受者 + n−1 条专用线程的工作循环**，配套两个新公开头（`net/uvcpp_loop_worker.h`、`net/uvcpp_socket_handoff.h`）与 socket 转手机制。**新连接落哪条循环分两种形状**（运行时探测，`is_fanout()` 可问）：Linux 侧自 `1.3.5-dev` 起 n 条循环**各自绑同一端口、内核分流**（0 号自己也承载连接），Windows 上没有 `SO_REUSEPORT`，走的是「单接受者 + 无锁转手」，**默认开着**（只有 `set_loops(1)` 例外）；那条路上有一笔量过的吞吐代价，口径与数字见 [`doc/multiloop-design.md`](doc/multiloop-design.md)。 |
| **HTTP/2 流级背压** | 收方向的 `pause_stream()` / `resume_stream()`、发方向单流待发队列的上界（默认 4 MiB）、以及 `peer_window_size()`（`1.2.25`）。**这是协议层机件，仓内没有应用层调用方** —— 框架侧不驱动它，所以 h2 上「边收边给」的流式请求体**仍不可达**，见 [`doc/http2-status.md`](doc/http2-status.md)。 |
| **TLS 主机名校验** | `uvcpp_ssl::set_verify_hostname()`（`1.2.24`）—— 见下面「修复」的第一条。 |

### 修复

`1.2.x` 修掉的问题里，三类值得单列：

- **安全** —— `tls_verify_mode::PEER_STRICT` 此前与 `PEER` **完全等价**：两个值映射到
  同一个 `SSL_VERIFY_PEER` 位，头注释自己也如实写着"主机名那一半从未实现"。于是它
  挡不住「证书链可信、但签发给别的域名」的对端 —— 而那正是中间人最省事的一种做法。
  现在客户端把 `connect()` 收到的那个名字钉给证书（数字 IP 字面量走
  `X509_VERIFY_PARAM_set1_ip_asc()`，其余走 `set1_host()`），名字对不上的对端
  **建立不起来**（`1.2.24`）。服务端仍与 `PEER` 等价：本库的服务端不发 SNI、也不要求
  客户端证书，没有可校验的名字。
- **内存** —— 内存池四条释放路径的缺陷（SUPER 静默泄漏、**三处 use-after-free**、
  两块内存两套释放器混用）；`max_total_memory` 从假限额改成**按实占字节记账**；
  池的元数据不再走全局 `operator new`（在那之前，接上池会**卡死**）。
- **公开契约** —— 三个**从来没有编过、也从来没有链过**的公开 API 修好并各自配上用例
  （`DEFINE_FUNC_REQ_CPP` 宏里一处重复定义、两处写错的名字）；`uvcpp_handle` 的拷贝
  构造、拷贝赋值与 `clone()` 从「静默泄漏 / 破坏活句柄」改成**编译期拒绝**。

### 实测数据

`1.2.x` 期间有三笔性能改动，各自带读数。**三组来自不同装置与不同轮次，不能互相加减，
也不能与 [`doc/benchmark.md`](doc/benchmark.md) 那页的 75 k 单循环口径混引**：

- 队列里攒着的响应**合成一次写** —— 段/请求从 **3.43 降到 2.0**，同装置配对跑
  **RPS +46%**（对照 38 839 RPS / 服务端 23.57 µs 每请求，其中约 74% 花在内核）；
- 每请求分配次数 **16.00 → 13.00**（2 848.2 B → 1 984.2 B）；
- 读缓冲不再清零 —— 此前**每请求白清 128 KiB**；交错 A/B 读数 QPS 101 251 → 115 085。

### 换二进制之前

相对上一次发布（`v1.2.0`），这一版动过的东西分两类。

**一、导出符号被删除 —— 旧代码会编不过**（删的理由见
[`doc/lowlevel-guide.md`](doc/lowlevel-guide.md) §10；删的两个 `clone()` 全仓零调用点，
而拷贝句柄本来就是泄漏或破坏活句柄）

| 删掉的符号 | 档 |
|---|---|
| `uvcpp_handle` 的**拷贝构造**与**拷贝赋值** | `1.2.5` |
| `uvcpp_handle::clone()` | `1.2.5` |
| `uvcpp_req::clone()`（`uvcpp_req` 的拷贝本身**保留**） | `1.2.5` |

还有一处**只是导出符号变了、布局没变**：`uvcpp_web_context` 的构造函数多了一个前置的
凭证参数（`1.2.20`）。它不构成实际破坏 —— 那个构造函数在此之前是**私有**的，类外既没有
也不可能有调用点 —— 写在这里只是让这份清单完整。

**二、布局变了 —— 旧头文件配新库会读到错的偏移**

| 类 | 变的是什么 | 档 |
|---|---|---|
| `uvcpp_h2_stream` | 新增 `paused` / `paused_owed` 两个成员，`sizeof` 变了 | `1.2.25` |
| `uvcpp_ssl_context` | 新增私有成员 `verify_mode_` | `1.2.24` |
| `uvcpp_memory_pool` | 私有成员换过（`destroying_` → `held_bytes_`） | `1.2.x` |
| `uvcpp_http_server`、`uvcpp_ws_server`、`uvcpp_ws_sessions`、`uvcpp_tcp_server`、`uvcpp_web_app` | 私有布局重排（每循环一格容器、状态位改原子量），多循环那一批 | `1.2.21`–`1.2.23` |
| `uvcpp_web_file_transfer`、`uvcpp_web_response` | 各新增私有成员（静态分片下发那一支的**块级名额闸门**：`gate_` / `gate_held_` / `gate_wakeup_` / `gate_waits_` 与 `file_gate_`），`sizeof` 变了 | `1.3.6-dev` |

**三、还有一条编得过、单循环下也对，只有多循环才会读到错的数据**

`uvcpp_web_app::connections()` 与 `connection_count()` **同名同签名，语义变了**：
`connections()` 现在返回**本循环**那一份登记表，`connection_count()` 变成**所有循环求和**，
逐循环要用 `connection_count_at(int)`。单循环（`n == 1`）下行为与旧版等价，
只有 `set_loops(n > 1)` 时才不同 —— 旧代码会**编过**，然后读到错的那一份。

**另有两处行为变化，不改代码也能观察到**：`uvcpp_h2_session` 的 `on_request_end` 现在
**只要 `end_stream` 为真就触发**（裸 GET 也来，且紧跟 `on_request`），原先只在有 body
时触发；`submit_data()` 新增返回码 `UV_ENOBUFS`（待发队列超上界）。

## v1.2.0 重点 (Highlights)

`1.1.1` → `1.1.35` 这 35 个开发档全部收进这一版。**无破坏性改动**，但有两处 ABI
变化（见本节末）。

### 新增

| 新增 | 说明 |
|---|---|
| **HTTP/2** | nghttp2 集成、ALPN、h2 会话与连接层。`uvcpp_web_app` **零配置自动协商**（ALPN 挑 h2 / h1.1）；低层的 `uvcpp_http_client` / `uvcpp_http_server` 仍默认 HTTP/1.1，要 h2 得显式 `set_http2_enabled()`。明文上**不做 h2c**：没有 ALPN 就没有协商，自动"升级"只能是猜。 |
| **发布包里的调试档** | 每个 zip 同时有发布档与调试档（`uvcppd.dll` / `libuvcppd.dll` / `libuvcppd.so`），MSVC 那份另带 `uvcppd.pdb`。**它是用来调试的，不是用来发布的** —— 前提见下面「调试档」一节。 |
| **性能** | 静态响应的压缩变体缓存（命中时不再重复 deflate）；`write()` 先试 `uv_try_write()`，装得下的部分不进待写缓冲；响应体零拷贝、与响应头合成两个 write block（`nbufs = 2`）一次发出。 |

### 实测数据

[`doc/benchmark.md`](doc/benchmark.md) 是在**用本库构建出的一个真实 webapp**（静态文件 +
动态路由 + 上传下载 + WebSocket + TLS）上跑出来的读数，不是估算：

- **每条空闲连接 4.62 KiB**（4 734 B）—— 八档最小二乘、**R² = 0.9999869**、除原点外每点
  残差 ≤ 0.51%，外推 100 万连接 ≈ **4.42 GiB**；
- 单事件循环 **75 k RPS**（流水线 10–50 深可到 88 k）；静态 **44 k RPS / 107 MB/s**；
- 10 分钟长跑 **3 840 万请求 / 0 错误**，RSS 漂移 0.2%。

那一页还把这个数与 [Hical](https://github.com/Hical61/Hical) 的自报数据做了对照：单位口径
是**查实**的（Hical 标 `KB`/`GB`，但它那两个数只在二进读法下自洽 —— `17.44 × 1024 × 10⁶ B
= 16.63 GiB` 正好对上它自己写的 `~16.6 GB` —— 两边本来就同口径，不必折算），剩下的口径
问题（内核 TCP 缓冲计不计入、平台不同）逐条写明，**没有单选对本库有利的读法**；归因未
闭合的那组（TLS 稳态代价）同样说明了为什么不放进去。读数测于 `1.1.33`，该页 §1 逐个
commit 说明为什么它对 1.2.0 仍然成立。

### 修复

四类问题一并收掉。逐条的一档号在
[CHANGELOG.md](https://github.com/Antruly/libuvcpp/blob/master/CHANGELOG.md)
里 —— 那边是唯一的清单，这边不抄第二份（两份手写的清单正是本仓已经栽过的形状）。

- **协议正确性** —— 错误路径伪造状态码、中途断流送出假的 `200`、`206` 被压缩
  （`Content-Range` 与 `Content-Encoding` 本就互斥）、`HEAD` 与 `GET` 响应头不一致、
  `Accept-Encoding` 里显式的 `q=0` 被 `*` 覆盖、跨读边界的请求被吞、请求头/URL
  超长在接收时就按 `431` / `414` 拒掉；WebSocket 侧服务端强制客户端掩码并校验
  文本帧 UTF-8，客户端也真的掩码了。
- **内存** —— 大块分配重新计入 `span->in_use`（此前每块漏掉一整个 span）、
  `uvcpp_write` 第二个写槽被顶替时泄漏、内存池在用块数不跟分配走、压缩变体表的
  字节账与表内容脱节。
- **生命周期** —— h2 拆解的两处 use-after-free 与一处泄漏、TLS 握手留下孤儿定时器、
  从自己的回调里销毁 `tcp_client` 累积包装对象、对端断开时 HTTP 客户端的关闭
  观察者永不触发。
- **打包与消费方契约** —— 模块使能宏随包发布，宏集合与 dll 不一致从"静默读错偏移"
  改成**编译期硬失败**；22 个公开头补 UTF-8 BOM，消费方不传 `/utf-8` 不再级联报错；
  Linux 包的 `libuvcpp.so` 从 `bin/` 挪到 `lib/`；六个打包 job 漏传
  `UVCPP_ENABLE_NGHTTP2` 导致 h2 包缺模块。
- **发布门槛（本版新加）** —— 包里的两档库各自**自报家门**（Windows 看导入表与
  `RSDS`，MinGW/Linux 看调试节里的本库源文件名），staging 把两档装反时**拒绝出包**，
  而不是发一份文件名全对、内容错了的包。

### 换二进制之前

两处 **ABI 变化**：`uvcpp_buf`（`1.1.28`）与 `uvcpp_http_server`（`1.1.34`）的布局
变了。**要重编，不能只换二进制** —— 旧头文件配新库会读到错的偏移。

（`1.2.x` 期间追加的 ABI 变化不记在这里，它们属于下一版 —— 见上面
`v1.3.0` 的「换二进制之前」。）

## 新增模块 (New in v1.1.0)

v1.1.0 在 v1.0.0 的 libuv 封装之上，**新增了网络层、HTTP/1.1 与 WebSocket、TLS、
以及一套 Web 应用框架**，并首次提供预编译的动态库。全部为向后兼容的增量。

### net — 面向对象的网络层
- `uvcpp_tcp_server` / `uvcpp_tcp_client` — TCP 服务端与客户端
- `uvcpp_udp_server` / `uvcpp_udp_client` — UDP 服务端与客户端
- `uvcpp_net_read` — 统一的读回调

### web — HTTP/1.1 与 WebSocket
- `uvcpp_http_server` / `uvcpp_http_client` — HTTP 服务端与客户端（含流水线）
- `uvcpp_http_parser` / `uvcpp_http_request` / `uvcpp_http_response` — 基于 llhttp 的解析与报文构造
- `uvcpp_http_compress` — gzip / deflate 压缩（zlib）
- `uvcpp_static_server` — 静态文件服务
- `uvcpp_ws_server` / `uvcpp_ws_client` / `uvcpp_ws_parser` / `uvcpp_ws_frame` / `uvcpp_ws_sessions` — WebSocket（RFC 6455）

### ssl — TLS
- `uvcpp_ssl_context` / `uvcpp_ssl` — TLS 上下文与连接（OpenSSL）

### webapp — Web 应用框架
- `uvcpp_web_app` + `uvcpp_web_router` + `uvcpp_web_handler` — 应用与路由
- `uvcpp_web_middleware` — 中间件
- `uvcpp_web_static` / `uvcpp_web_mime` / `uvcpp_web_file` — 静态资源与文件下发
- `uvcpp_web_stream` — 流式响应
- `uvcpp_web_multipart` / `uvcpp_web_upload` — multipart 上传
- `uvcpp_web_json` — JSON（nlohmann/json）
- `uvcpp_web_ws` / `uvcpp_web_ws_client` — 接入 webapp 的 WebSocket
- `uvcpp_log` / `uvcpp_log_console` — 日志

## 主要特性 (Key Features)

低层（`uvcpp_loop` / `uvcpp_tcp` / `uvcpp_pipe` / `uvcpp_udp` / `uvcpp_timer` /
`uvcpp_signal` / `uvcpp_process` / `uvcpp_fs*` 这些句柄，与 `uvcpp_write` /
`uvcpp_connect` / `uvcpp_fs` / `uvcpp_work` / `uvcpp_getaddrinfo` 这些请求）之外，
本库另有 net / web / webapp / ssl / db 五个高层模块，与 expand / wsdl / capi 三个。
**逐个类的清单在 [README.md 的功能特性一节](README.md#features)** —— 那边是唯一的
清单，此处不抄第二份。

## 构建要求 (Requirements)

C++11 或更高、CMake 3.20+、libuv 1.0.0+，支持 Windows / Linux / macOS。可选依赖：
OpenSSL（TLS；QUIC 与 HTTP/3 还要 ≥ 3.2）、zlib（压缩）、llhttp 与 nlohmann/json
（HTTP / Web 框架，可由 FetchContent 自动获取）、pugixml（WSDL/SOAP）、ngtcp2 与
nghttp3（QUIC / HTTP/3）、libsqlite3 / libmysqlclient / libpq（数据库，至少一个）。
各平台依赖与全部开关在 [doc/build-guide.md](doc/build-guide.md)。

## 使用示例 (Example)

从 clone 到跑绿的那条路，以及可以照抄的完整例子（**每一个都被 CI 逐条编译过**）在
[README.md 的快速入门](README.md#quick-start)；`examples/` 下另有可运行示例。

## 变更日志 (Changelog)

**按主题汇总的清单在 [CHANGELOG.md](./CHANGELOG.md)** —— 那一段是唯一的清单，
这边不抄一份（两份手写的清单正是本仓已经栽过的形状）。逐版本的**发布叙事**就是
上面那些 `## vX.Y.Z 重点` 小节，而每个已发布 tag 的清单条目在 CHANGELOG.md 末尾的
「逐个版本」里。

开发版线是怎么折叠的：`1.1.1` → `1.1.35` 收进 `v1.2.0`，`1.2.1` → `1.2.25` 收进
`v1.3.0`，`1.3.1` → `1.3.33` 收进 `v1.4.0`，`1.4.1` → `1.4.4` 收进 `v1.5.0`，
`1.5.1` → `1.5.3` 收进 `v1.6.0`。逐条的「这一版没做什么」在
[`doc/db-guide.md`](doc/db-guide.md)、[`doc/quic-guide.md`](doc/quic-guide.md) 与
[`doc/http3-guide.md`](doc/http3-guide.md)。

---

## 以下为归档

这一分界线以下是**归档**段落。它们记的是跨版本仍然成立的东西，不是某一版的新增，
所以 `release_notes.py` 按 tag 切出的 Release 正文里**没有**这几节 —— 要看它们，
来这份文件；要按版本看，翻上面的 `## vX.Y.Z 重点`。

## 预编译产物 (Prebuilt binaries)

`1.5.0` 起**每一档的发布档与调试档都是全功能的**：QUIC + HTTP/3 + C ABI
（`UVCPP_QUIC_ENABLE` / `UVCPP_HTTP3_ENABLE` / `UVCPP_CAPI_ENABLE` 都是 1，此前的发布包
这三项是 0 —— 逐条的来路与判据见上面「`v1.5.0` 重点」）。**数据库模块自 `1.5.3` 起也进了
六条腿**（`UVCPP_DB_ENABLE` 从 0 变 1），但**只带 SQLite 一个后端** —— 见下面那一段。

**包里的数据库模块只有 SQLite。** 六条腿各传四个 `-D`：

```
-DUVCPP_ENABLE_DB=ON -DUVCPP_DB_SQLITE_FROM_SOURCE=ON \
-DUVCPP_ENABLE_DB_MYSQL=OFF -DUVCPP_ENABLE_DB_PGSQL=OFF
```

两个 `OFF` 是**显式**关的，不是"找不到"：发布腿的 runner 上装着 `libpq-dev`，
`find_package(PostgreSQL QUIET)` 会**静默**成功，于是 `libuvcpp.so` 悄悄多一条
`libpq.so.5` 的 `DT_NEEDED` —— 而"装到别的机器上跑不起来"正是发布腿那条 `ldd` 断言要拦的
形状（名单里 `sqlite3` / `mysqlclient` / `pq` 三枚都在）。SQLite 那份也**不链系统的**：
`UVCPP_DB_SQLITE_FROM_SOURCE=ON` 让配置期下一份**钉死哈希**的 amalgamation、编成静态且带
PIC 的 `uvcpp_sqlite3` 链进去 —— 链系统的 `libsqlite3.so` 会多一条 `DT_NEEDED`，而 Ubuntu
24.04 上系统的 `libsqlite3.a` 不是 PIC。所以预编译包里 `uvcpp_db_drivers()` 只打印
`sqlite`；要 MySQL / PostgreSQL 就**从源码编**（默认三个后端都开，那时它们按各自那份
客户端库正常链接）。两条理由的实测数据在 [`doc/db-guide.md`](doc/db-guide.md)。

本版本提供 **6 个平台**（x64 与 arm64 × MinGW-w64 / MSVC / GCC）的预编译动态库，
**每档都含发布版与调试版两份**（`v1.1.0` 起就是 6 份，此前这里只列了 3 个 x64 ——
是这段写漏了，不是少发了包）：

| 平台 | 工具链 | 发布档 | 调试档 |
|---|---|---|---|
| Windows x64 | MinGW-w64 (GCC) | `libuvcpp.dll` + `libuvcpp.dll.a` | `libuvcppd.dll` + `libuvcppd.dll.a` |
| Windows arm64 | MinGW-w64 (GCC) | 同上 | 同上 |
| Windows x64 | MSVC (VS2022) | `uvcpp.dll` + `uvcpp.lib` | `uvcppd.dll` + `uvcppd.lib` + `uvcppd.pdb` |
| Windows arm64 | MSVC (VS2022) | 同上 | 同上 |
| Linux x64 | GCC | `libuvcpp.so` | `libuvcppd.so` |
| Linux arm64 | GCC | 同上 | 同上 |

每个 zip 内含 `bin/`（动态库）、`lib/`（导入库）、`include/`（公开头，含 libuv、
nlohmann/json、zlib 的头）、`lib/pkgconfig/`（`uvcpp.pc` 与 `uvcpp-debug.pc`）、
`bindings/csharp/`（C# 绑定与示例）与文档。

| 文件 | 说明 |
|---|---|
| `bin/libuvcpp.dll` | 动态库。**libuv / llhttp / zlib / OpenSSL / nghttp2 / ngtcp2 / nghttp3 / SQLite 以及 MinGW 运行时均已静态链接进去** |
| `lib/libuvcpp.dll.a` | 导入库（供 MinGW/GCC 链接，`-luvcpp`） |
| `bin/libuvcppd.dll` | **调试档**动态库（MSVC 那份叫 `uvcppd.dll`）。用法与前提见下面「调试档」一节 |
| `lib/libuvcppd.dll.a` | 调试档导入库（`-luvcppd`）；MSVC 那份是 `uvcppd.lib` |
| `bin/uvcppd.pdb` | **仅 MSVC**：调试档的符号文件，与 `uvcppd.dll` 同目录 |
| `include/` | 公开头文件，含 `expand/`（内存池）与 `db/`（数据库 — 公开头里**不出现** `sqlite3.h`，三个后端都是 pimpl，所以用包里的 db 模块不需要装任何客户端库的头） |
| `include/uvcpp/uvcpp_config.h` | **生成的**模块使能宏。每个公开头自己包含它，使用者**不必再传任何 `-D`**（见下） |
| `bindings/csharp/` | C# 绑定：`UvcppNative.cs` + `UvcppNative.Protocols.cs`（321 条 `DllImport`）与 `examples/QuicEcho/`（一个能跑的回环例子）。放在包里是为了让 C# 侧"这两份声明 + 这份动态库"就能开工，不必再回仓里捞 |

> 命名遵循各工具链的惯例：**MinGW/GCC 产出 `libuvcpp.dll`**，MSVC 产出 `uvcpp.dll`。

MinGW 版 `libuvcpp.dll` 的依赖只有 Windows 自带系统库（`KERNEL32` / `WS2_32` / `CRYPT32` /
`ADVAPI32` / `USER32` / `SHELL32` / `IPHLPAPI` / `USERENV` / `ole32` / `dbghelp` / `msvcrt`），
**不需要安装 MSYS2 或 MinGW 运行时**，任意 64 位 Windows 直接可用。

MSVC 版 `uvcpp.dll` 用 `/MD` 构建，因此 `bin/` 里一并带了
`msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll`，
**不需要预装 VC++ 可再发行组件**。若构建机上只有动态版 OpenSSL，`bin/` 里还会多出
`libcrypto-<主版本>-<架构>.dll` 与 `libssl-<主版本>-<架构>.dll` —— **两个占位都随构建机上
那份 OpenSSL 变**，所以不同平台包里的名字不同：已发布的包里 x64 是 `libcrypto-4-x64.dll` /
`libssl-4-x64.dll`，arm64 是 `libcrypto-3-arm64.dll` / `libssl-3-arm64.dll`。
（本机构建用的是静态版，因此本地这份包没有这两份。）

打包脚本不靠手写的依赖清单，而是**读产物自己的导入表**：凡是不是 Windows 自带的
模块，包里必须有，找不到就拒绝出包（`tests/tools/package_release.py`）。早先那张手写
清单是猜的 —— MSYS2 同时装了 `libssl.a` 和 `libssl.dll.a`，`find_library` 默认挑
`.dll.a`，产物会凭空多一个 `libssl-<主版本>-x64.dll` 而清单里没有。

它也不靠文件名认那两份库：文件名是给人看的，staging 把调试档拷成发布档的名字（或
反过来）时，**所有**基于名字的判据都会全绿而包是错的。所以两档各自**要自报家门**，
而且**两个方向都判**：Windows 上比对导入表里有没有 `/MDd` 那几份调试版 CRT、以及
PE 里有没有 `RSDS` 指向自己的 `.pdb`；MinGW / Linux 上比对调试节里有没有本库自己的
源文件名（发布档按 `-O3 -DNDEBUG` 编，一个都不该有 —— 而"有没有 `.debug_*` 节"
**不是**判据：静态链进去的依赖本身就带着 355 KB 调试节）。

`.pdb` 还会与 `.dll` **对源**：比 PE 里 CodeView 记录的 GUID+age 与 PDB 里 stream 1
的 GUID+age，对不上就拒绝出包 —— 一个配错符号的 `.pdb` 比没有 `.pdb` 更坏，它会让
调试器停在一份不相干的源码上。

> ⚠️ 两个 Windows 版互为替代、不可混用：由 MinGW-w64 编译的动态库**不能被 MSVC
> 链接**，反之亦然（C++ ABI 不同）。用哪套工具链就用哪个 zip。

### 调试档（Debug 版）

每个 zip 里除了发布档，还有一份**调试档**：文件名多一个 `d`（`libuvcppd.dll` /
`uvcppd.dll` / `libuvcppd.so`），与发布档同放在 `bin/`（Linux 是 `lib/`），导入库同在
`lib/`，`.pc` 是 `uvcpp-debug`（`-luvcppd`）。MSVC 那份还带 `bin/uvcppd.pdb`。

**它是用来调试的，不是用来发布的**，四条前提：

1. **MSVC 的调试档用 `/MDd` 链**，所以包里**没有**、也不会带 `msvcp140d.dll` /
   `vcruntime140d.dll` / `ucrtbased.dll` —— 微软的调试版运行库**不可再分发**，
   只在 VS 安装树里。要跑调试档，机器上得装了 Visual Studio，或者把那几个目录挂到
   `PATH` 上（`…\VC\Redist\MSVC\<版本>\debug_nonredist\x64\Microsoft.VC143.DebugCRT\`
   与 `…\Windows Kits\10\bin\<版本>\x64\ucrt\`）。**不挂的话每个程序都以
   `0xc0000135`（找不到 DLL）退出，看起来像库坏了**，实际是缺那三份不可再分发的运行库。
   打包脚本刻意不发它们，并且按**成品**再扫一遍确认没混进去。
2. **两档不能混链**：调试档只能链进 `/MDd`（MSVC）或带 `-g` 的程序，发布档只能链进
   `/MD` 的程序。混着链是未定义行为，不是"能跑但慢一点"。
3. **不要拿调试档做性能测量**（`-O0`，且带各种调试期检查）。
4. **MinGW / Linux 没有独立的符号文件**：调试信息（DWARF）**内嵌在动态库里**，
   包里没有、也不会有 `.pdb` 之类的东西。发布档不带 `-g`，要调试就用调试档。

### 使用方式

**使用者不需要传任何 `-D`。** 模块使能宏由包里的
`include/uvcpp/uvcpp_config.h` 给出，每个公开头都会在自己第一个模块守卫**之前**
包含它，所以只要 `-I` 指对，宏就自动与这个 dll 一致：

```bash
export PKG_CONFIG_PATH=/path/to/libuvcpp-1.5.0-mingw-x64/lib/pkgconfig
g++ -std=c++11 $(pkg-config --cflags uvcpp) your_app.cpp $(pkg-config --libs uvcpp) -o your_app.exe
```

> ⚠️ `--cflags` 与 `--libs` **要分开写、`--libs` 要放在源文件之后**。
> 合成一条 `$(pkg-config --cflags --libs uvcpp) your_app.cpp` 会把 `-luvcpp`
> 排到源文件前面，`ld` 不会回头再看一遍归档文件 ⇒ 28 个 `undefined reference`
> （库本身没问题，纯粹是命令行顺序）。

不用 pkg-config 时，等价的命令行是：

```bash
g++ -std=c++11 -I include your_app.cpp -L lib -luvcpp -o your_app.exe
```

要**链调试档**就把包名与库名都换成带 `d` 的那个（`-luvcppd`）；头文件是同一份，
一个字节都不差：

```bash
g++ -std=c++11 -g -I include your_app.cpp -L lib -luvcppd -o your_app.exe
```

（上面两条命令都在包的根目录下执行；`-L lib` 是库所在处。跑的时候
`bin/libuvcpp.dll` 要在 `PATH` 上，或直接拷到 exe 旁边；Linux 那份 `.so` 就在
`lib/` 里，用 `LD_LIBRARY_PATH=$(pkg-config --variable=libdir uvcpp)` 或 rpath 指过去。）

> Linux 包里的 `libuvcpp.so` **从 1.1.28 起放在 `lib/`**。在那之前它放在 `bin/`，
> 而 `.pc` 里写的是 `-L${libdir} -luvcpp`、`lib/` 是空的 —— 也就是说照本文档在
> Linux 上**根本链不上**（`ld: cannot find -luvcpp`）。Windows 两套不受影响，
> 仍是 `bin/*.dll` 配 `lib/` 里的导入库。

> MSVC 消费者**不需要额外传 `/utf-8`**。仓内头文件里含中文注释的那批本来靠
> `add_compile_options(/utf-8)` 兜着，而那个开关是**目录作用域**的 —— 既不进导出集，
> 也到不了预编译包的消费者。按系统代码页（936）读这些头时，中文注释的末字节会吞掉
> 换行、把 `*/` 吃掉，注释不闭合，报错却落在 `<algorithm>` 里（`C4819` 是唯一的线索）。
> 因此 1.1.27 起，**打包脚本给所有含非 ASCII 的头补了 UTF-8 BOM**，MSVC 会据此自动
> 按 UTF-8 读，什么开关都不用加。包里的头因此与源码树里的**不逐字节相同**（多一个 BOM），
> gcc/clang 前导 BOM 一样接受，各平台包保持同一份字节。

> ⚠️ **不要自己定义这些宏。** 如果你显式传了一个与包**不一致**的值
> （`-DUVCPP_OPENSSL_ENABLE=0` 拿到一个开着 OpenSSL 编的包），
> `uvcpp_config.h` 会**直接 `#error` 停编译**，并指出包里那个值。
> 这是刻意的：宏不一致时公开头里的**成员布局**会与 dll 不同，
> 内联访问器按错误偏移读成员，拿到的是一个天文数字，
> **没有编译错误、没有链接错误、没有运行时告警** —— 比编不过危险得多。
> 需要别的宏集就重新构建 uvcpp，不要从这一侧改。
>
> 1.1.26 及更早的包需要你手工传一串 `-D`，且那份清单是**写死**的
> （与包实际怎么编无关）。升级到本版时把那些 `-D` **删掉**即可；
> 留着它们只有在与包冲突时才会报错，值相同时无害。

> ⚠️ **换 dll 而不重编消费者会崩 —— 本条自 1.1.28 起生效，且有两次实例。**
> 拿新的 `uvcpp.dll` 配旧的 exe / obj，会在启动或首次用到那个类时崩：
> `0xC0000409`（栈缓冲溢出）或 `0xC0000374`（堆损坏），在 ctest 里表现为
> **0.01 秒、一行输出都没有**。这个形状最容易被当成环境问题查很久。
>
> - **1.1.28**：`uvcpp_buf` 加了共享视图（多一个 `shared_ptr` 成员）与移动
>   构造/移动赋值。
> - **1.1.34**：`uvcpp_http_server` 去掉了一个私有成员（压缩变体表的字节
>   计数器 —— 改成从表里算），`sizeof` 因此变了。
>
> 两次都是**加/删私有成员**：消费者**源码一行都不用改**，但必须**重新编译**。
> 一句话：**换了 dll 就必须重编，别只换二进制。**
>
> 1.1.28 同版还有两条**行为**变化（不崩，但要知道）：
>
> - `uvcpp_buf` 从前**没有**移动构造（本类有用户声明的拷贝构造与析构，编译器
>   因此不会隐式生成），`uvcpp_buf b = std::move(a);` 一直是静默的深拷贝、
>   `a` 原封不动；现在 `a` 会被搬空。对"move 之后接着用 `a`"的调用点，
>   这是行为变化（虽然那几乎肯定是写错了）。
> - `uvcpp_http_server::send_response` 的返回类型从 `void` 变成 `size_t`
>   （实际发出去的字节数）。返回类型不进名字修饰，**不会**造成链接错误。

头文件的入口是包根 `include/uvcpp.h`（聚合头，含 loop / handle / req）。
`net` / `web` / `webapp` / `ssl` 的类**不在聚合头里**，按模块显式 include，例如
`#include "handle/uvcpp_tcp.h"`、`#include "web/uvcpp_http_server.h"`。

---

## 已知问题 (Known Issues)

### 内存池在 v1.1.0 里修好了；CMake 默认仍是 OFF

早先 MinGW-w64 打开 `UVCPP_BUILD_EXPAND=ON` 会让 `ctest` 80 项里 **7 项**崩溃
（`0xc0000374` 堆损坏或 SegFault），而同一份源码用 MSVC 构建全绿。
**根因已定位并修复**，两个平台现在都是 80/80。

根因不属于内存池的数据结构，而在于**线程缓存由谁销毁**：

- MinGW-w64 下 DLL 里的 `thread_local` 走的是 **emutls**（libgcc 在堆上按线程
  分配的数组），带非平凡析构的 `thread_local` 对象则经 `__cxa_thread_atexit`
  登记析构。线程退出时 emutls **先**把那个数组还给了堆，登记的回调**才**被调用
  —— 于是析构函数 walk 的是已释放、已被复用的内存：读出来的 `span` 是野指针，
  紧接着在 `central_cache::push` 里对它做 CAS。写进只读页就是访问违例，写进
  可写页就是堆损坏；崩在哪个地址全看那块内存被谁捡走了。
- MSVC 没有 emutls，所以一直是对的。这解释了全部既有现象：只在 MinGW 复现、
  只在开池时复现、崩溃点在"线程退出"、改成共享运行时会更糟（libgcc 的释放
  时机变了）。

修法是**不再依赖 `thread_local` 对象自己的析构函数**：线程缓存改由 OS 的线程
存储槽管理（Windows `FlsAlloc` / POSIX `pthread_key_create`），槽的析构回调
**把缓存指针当参数收进来**，回调自身一个字节的 TLS 都不读，因此不存在
"读一个已经被释放的 TLS 槽"这回事。

因此**本版发布的两个 Windows 动态库都带内存池**（`UVCPP_BUILD_EXPAND=ON`，
产物里 `UVCPP_ENABLE_MEMORY_POOL=1`，由 `include/uvcpp/uvcpp_config.h` 带出来）。

但 **CMake 的默认值仍然是 OFF** —— 从源码构建要用池，得显式传
`-DUVCPP_BUILD_EXPAND=ON`。

这里原来给的理由是「默认开的话，使用者的 TU 忘了定义 `UVCPP_ENABLE_MEMORY_POOL`，
宏求值为 0 ⇒ 使用者侧走 `std::malloc`、dll 侧走池，库会拿池去 free 一个 `malloc`
的指针，**静默**堆损坏」。**这条与现在的实际行为对不上**：`uvcpp_config.h` 生成之后
它经 `uvcpp_alloc.h` 等公开头带进来，规则是「外部定义过且与本包不同 ⇒ `#error`；
**没定义 ⇒ 取本包的值**」。所以"忘了定义"拿到的是**包的值**，两边一致；定义成别的
值是编译期硬失败。默认关因此只是"从源码构建时默认拿到哪套分配器"的选择，不再是
安全守卫 —— 用预编译包则**什么都不用传**。

### 其他

- `cmake --install` 在当前树上是坏的：libuv 由 `FetchContent_MakeAvailable` 引入，
  它登记的 install 规则引用了一个从未构建的 `libuv.dll`，且它的规则排在本项目的
  规则之前 —— 一失败就整体中止，本项目的头文件与库一个都装不出来。
  本次的 zip 绕过它、照 `CMakeLists.txt:2238-2485` 的规则手工组装，与之有两处
  刻意的差异：**libuv 的头放在 `include/` 顶层**（本库的公开头写的是
  `#include <uv.h>`，放进 `include/libuv/` 会找不到），以及**补上了 `zlib.h` /
  `zconf.h`**（`web/uvcpp_ws_parser.h` 在 `UVCPP_ZLIB_ENABLE=1` 时要 include 它，
  但 install 规则里没有这一条）。

### MSVC 从源码树 / 安装树取头时要加 `/utf-8` —— 头文件侧已修（1.2.0）

**这条曾经是个真问题，现在只剩一半。** 预编译包的消费者从来不受影响
（打包脚本给含非 ASCII 的头补了 BOM），受影响的一直是 `find_package(uvcpp)`
与直接把 `src/` 加进 `-I` 的那条路。

**当时**：`src/` 下有一批公开头是**无 BOM 的 UTF-8**。MSVC 按系统代码页（936/GBK）解码，
中文注释的末字节吞掉换行、把 `*/` 吃掉，注释不闭合，报错却落在 `<algorithm>` 里；
`C4819` 是唯一的线索（`warning C4819: 该文件包含不能在当前代码页(936)中表示的字符`）。
仓内编译看不出来，有两个原因叠加：顶层 `CMakeLists.txt` 里
`if(MSVC) add_compile_options(/utf-8) endif()` 是**目录作用域**的 —— 导出集里
`INTERFACE_COMPILE_OPTIONS` 是**空**的，`find_package` 的消费者拿不到它；而
`install(FILES ...)` 是把源码树那几个头**原样**拷出去，BOM 不会凭空多出来。

**现在**（2026-09-21 实测）：`src/` 下 99 个 `.h` 里 **97 个带 BOM**，剩下 2 个
（`expand/uvcpp_memory_pool_span.h`、`expand/uvcpp_page_allocator.h`）**是纯 ASCII**、
不含非 ASCII ⇒ **没有一个头还会踩这个坑**。`package_release.py` 里那道"含非 ASCII
又没 BOM 就补一个"的下限仍在，但它现在不会命中任何东西。

**仍未修的那一半**：`add_compile_options(/utf-8)` 依旧不进导出集，
`INTERFACE_COMPILE_OPTIONS` 依然是空的。所以**你自己写的头**若含非 ASCII 又没 BOM，
照样会中招 —— 给自己的目标加 `/utf-8`（或 `/source-charset:utf-8`），
或者把源文件存成带 BOM 的 UTF-8（这也是本仓的约定）。

## 感谢 (Credits)

感谢所有为这个项目做出贡献的人！

## 许可证 (License)

MIT License
