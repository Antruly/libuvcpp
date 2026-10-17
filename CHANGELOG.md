# 变更日志

本文件是 libuvcpp 的**唯一**变更清单。两版 README 只留一个指针指向这里；
`RELEASE.md` 是逐版本的发布叙事（每版「重点」与「破坏性」那两段），但**它本身也不是
任何一版的 Release 正文** —— 正文由 `tests/tools/release_notes.py` 按 tag 现切：
一张六平台的下载表 + 那一版的「重点」小节 + 一个指回本文件的链接，整份归档不再被
当成正文贴出去。想按版本读发布叙事，翻 `RELEASE.md`；想按主题读逐条改动，读本文件。

读法：**先看主题清单**。它按模块分组，每条括号里写的是这处改动**首次出现**的开发档号
（`1.1.7`、`1.3.5-dev` 这种）。开发档不单独发布 —— 它们按线折叠进稳定版：
`1.1.x` → `v1.2.0`、`1.2.x` → `v1.3.0`、`1.3.x` → `v1.4.0`、`1.4.1`–`1.4.4` → `v1.5.0`、
`1.5.1`–`1.5.3` → `v1.6.0`；`1.6.1`–`1.6.4` 是它之后的补丁版，不折叠、直接发布；
`1.6.5`–`1.6.61` → `v1.7.0`。想按时间线读，翻到最后的「逐个版本」。

当前源码树是 **1.7.2-dev**（开发版） —— 即 `src/uvcpp/uvcpp_version.h` 里 `UVCPP_VERSION_STRING`
报告的那个串。已打过的 tag：`v1.0.0`、`v1.1.0`、`v1.2.0`、`v1.3.0`、`v1.4.0`、`v1.5.0`、`v1.6.0`、`v1.6.1`、`v1.6.2`、`v1.6.3`、`v1.6.4`、`v1.7.0`。
其中若干条来自本仓第一位外部贡献者 [@sercebr](https://github.com/sercebr) 报的 issue。

## 目录

- [主题清单](#主题清单)
  - [C ABI（`uvcpp_c_*`）](#c-abiuvcpp_c_)
  - [C# 绑定（`bindings/csharp/`）](#c-绑定bindingscsharp)
  - [QUIC 传输（net 层）](#quic-传输net-层)
  - [HTTP/3（web 层）](#http3web-层)
  - [HTTP/2](#http2)
  - [gRPC（`src/grpc/`）](#grpcsrcgrpc)
  - [数据库（`src/db/`）](#数据库srcdb)
  - [横向扩展（多事件循环）](#横向扩展多事件循环)
  - [HTTP 与 WebSocket 语义](#http-与-websocket-语义)
  - [TLS 与网络](#tls-与网络)
  - [内存与缓冲](#内存与缓冲)
  - [公开契约与安全](#公开契约与安全)
  - [性能](#性能)
  - [JSON](#json)
  - [配置、打包与 CI](#配置打包与-ci)
- [逐个版本](#逐个版本)
  - [v1.7.0 (2026-10-14)](#v170-2026-10-14)
  - [v1.6.4 (2026-10-12)](#v164-2026-10-12)
  - [v1.6.3 (2026-10-12)](#v163-2026-10-12)
  - [v1.6.2 (2026-10-11)](#v162-2026-10-11)
  - [v1.6.1 (2026-10-11)](#v161-2026-10-11)
  - [v1.6.0 (2026-10-11)](#v160-2026-10-11)
  - [v1.5.0 (2026-10-05)](#v150-2026-10-05)
  - [v1.4.0 (2026-09-26)](#v140-2026-09-26)
  - [v1.3.0 (2026-09-23)](#v130-2026-09-23)
  - [v1.2.0 (2026-09-20)](#v120-2026-09-20)
  - [v1.1.0 (2026-09-19)](#v110-2026-09-19)
  - [v1.0.0 (2026-02-02)](#v100-2026-02-02)

## 主题清单

### C ABI（`uvcpp_c_*`）

> **两个数字各自属于一个时点，别互相覆盖。** `v1.5.0` 发布时这一层是前七片
> （地基 + net + webapp/web + HTTP/2 + QUIC + HTTP/3）共 **321** 个函数 —— RELEASE.md 里
> `v1.5.0` 那段写的就是这个数，作为**当时**的记录它是对的。第八片 db 在 `1.5.3` 落地，
> 合计 **402 个 / 八片**，也就是 `tests/tools/capi_symbols.lock` 与两版 README 门面表上的
> 当前值。下面各条按落地顺序写，所以会先读到 321、再读到 402。

- **`extern "C"` 这一层有了**，藏在 `UVCPP_ENABLE_CAPI` 后面（**默认关**，但每一个发布
  配置与 `full` 那条 CI 格都打开）。`src/capi/` 装到 `include/capi/`，**编进现有
  `uvcpp` 库** —— 不多一个产物、不多一处 DLL 拷贝、不新增第三方依赖、不需要 `.def`
  （`1.4.2`）
- **它是精选门面，不是 C++ API 的逐方法镜像。** 逐方法镜像会到 1500+ 个入口
  （`uvcpp_web_app` 一个类就 ≈107 个公开方法），那不是有人会手写的绑定面；发出来的是
  C# / P-Invoke 调用方真正要用的那一片。JSON 绝不进 C 头（`uvcpp_json` 就是
  `nlohmann::json`），libuv 类型同样不进（`1.4.2`）
- **五条承重规矩各自有一条用例，不是一句"我们很小心"**：句柄是不透明指针 + 魔数 +
  **登记表**（释放后再用给 `UVCPP_C_E_STALE`，不是 UB）；C++ 异常绝不过边界；回调表以
  `uint32_t size` 打头并**逐格**判，所以往尾部加一格**不破坏**已编好的客户端；每个函数
  写明自己属于三类所有权里的哪一类；线程规则与 C++ 一致，且**真查**（`1.4.2`）
- **`uvcpp_c_abi_version()` 让 P/Invoke 最常见的故障当场响** —— 头与 `.so` / `.dll`
  不是一次编出来的。它与库版本号**刻意是两条线**，用例断言它等于头里的宏（`1.4.2`）
- **1.4.2 交付的是地基 + net**：35 个入口（common 5、`tcp_client` 17、`tcp_server` 13）。
  web / webapp / HTTP/2 / QUIC / HTTP3 的 C 面**还没做**，`doc/capi-guide.md` 的 §7
  如实列出缺什么而不是暗示全量；具体到这一批，没有 TLS **参数**入口、没有 UDP、
  没有 DNS（`1.4.2`）
- **变异表第一次跑就挖出一处假绿**，这是最值得记的那件事。七条故意弄坏，预期先写下
  再跑；三条"句柄失效"变异**都没被抓住** —— 因为那几条 use-after-free 断言读的是空闲
  块的头 4 个字节，而 glibc 的 tcache 早已把 `next` 指针写在那儿，它们量的是分配器。
  补上 `uvcpp_c_live_handle_count()`（登记表收支平衡唯一可被外部量到的形式）才真的红；
  仍有一条如设计存活，如实写在文档里（`1.4.2`）
- **1.4.3 补上 webapp 与 web 两片**：新增 `uvcpp_c_webapp.h`（119 个入口）与
  `uvcpp_c_web.h`（29 个入口），到此共 **183 个函数**。app / 路由 / 中间件 / 静态目录 /
  上传 / WebSocket 路由 / `req` / `resp` / `next` / 延迟应答，以及 `http_client`、
  `http_server`、`ws_server` / `ws_connection` / `ws_client`。**JSON 依旧不进 C 头** ——
  响应侧只有 `uvcpp_c_resp_json_str()`（一个字符串），请求侧只有 body 原始字节
  （`1.4.3`）
- **回调期句柄是这一层最容易被误用的一处，1.4.3 把它变成机制而不是一句提醒**：
  `uvcpp_c_req` / `resp` / `next` / `ws_req` / `ws_conn` / `http_response` 都在**栈上**
  造，进回调登记、出回调摘表（`FrameScope` 的析构，所以用户回调抛异常那条路径也漏不
  掉）。回调返回之后再用它一律 `UVCPP_C_E_STALE`，**永远不是 UB**；唯一能带出回调的
  是 `uvcpp_c_deferred`（它真的持有 `ctx`），延迟应答、从别的线程投回来都走它（`1.4.3`）
- **这一批一个新错误码、零个 ABI 破坏**：`UVCPP_C_E_NOT_FOUND`（-20010）加在枚举**尾
  部**，所以按本层自己的规矩它不算 ABI 变更；`UVCPP_C_ABI_VERSION` 因此仍是 **1**，
  判据不是"我觉得没破坏"，而是 `git diff` 里批 1 那 35 个符号**一个签名都没动**（`1.4.3`）
- **第三个纯 C 用例，这次是"C 写的客户端打 C 写的服务端"**：`test_capi_webapp_func`
  （210 条断言）在一个进程里起一个纯 C 的 app，再用纯 C 的 HTTP / WebSocket 客户端打
  自己 —— 七条请求共用一条 keep-alive 连接、body 逐字节比、header 往返、中间件次序、
  静态目录、跨线程的延迟应答"晚到但如期"、两种 WebSocket 关闭方式，收尾核对登记表
  收支平衡（`1.4.3`）
- **批 3a 把 HTTP/2 的 C 面接上**：新增 `uvcpp_c_http2.h`（46 个入口），到此共
  **229 个函数**。C 面只有**驱动层**一个句柄（`uvcpp_c_h2_connection`）—— 会话层
  不单独给：那会要求 C 侧自己写一遍 socket 驱动，而且两个句柄指向同一份内部状态时，
  "先 free 哪个"就成了第二份真相。请求 / 响应是**构造器**（调用方建、调用方废），
  `uvcpp_c_h2_stream` 是**回调期句柄**。**不给**优先级 / 依赖 / push 与逐帧回调 ——
  `uvcpp_h2_session` 本来就没有前几项。第四个纯 C 用例 `test_capi_h2_func`（244 条
  断言）在**一条**连接上真跑三条流（POST 带 body / GET / 流式 GET），状态码与 body
  逐字节比，收尾核对登记表收支平衡（`1.4.3`）
- **符号面锁在这一批变成分片的**（`#@ module <名>` 一行开一片），因为一条腿只开得起
  一部分模块：CI 的 `capi` 格**没有** NGHTTP2，h2 那一片在它那棵树上本来就不该导出，
  平的锁会把"这一片这棵树没有"报成"这个符号被删了"——一个很有说服力的假红。门禁现在
  按**每片自己的开关**（从这棵树的 `uvcpp_config.h` 里读）逐片判：开着 → 与导出面
  逐条相等；关着 → 这一片**一个都不许出现**，并如实印一行「未判」。CI 里 `capi` 格与
  `h2` 格合起来才覆盖锁里每一片，所以 `h2` 格从这一批起也带 `-DUVCPP_ENABLE_CAPI=ON`
  （`1.4.3`）
- **变异表第二次挖出假绿，而这一次的结论更窄**：`1.4.3` 加了 M8–M12 五条，"回调期
  句柄带出回调之后必须给 `E_STALE`"那两条断言**没能抓住 M8**（拆掉 `FrameScope` 的摘
  表）。查下来是两件事：位置原先排在 `app_join()` **之后**，那时服务端线程的栈已被
  glibc 收回、魔数读成 0，断言过的是"内存没了"，而且读它本身就是那条断言号称要排除的
  UB（已挪到 `join()` 之前）；挪完 M8 依旧不红 —— 句柄住在栈上，回调一返回那块栈就被
  复用、魔数被无关写入盖掉，**魔数这个判据区分不出"被毒化"和"被栈复用盖掉"**，M8 因
  此只有 `uvcpp_c_live_handle_count()` 量得出来。那两条量的是**契约**，不是**机制**：
  两者都该有，错在拿前者当后者的证据（`1.4.3`）
- **批 3b 把 QUIC 与 HTTP/3 的 C 面接上，这一层到此做完**：新增 `uvcpp_c_quic.h`
  （42 个入口）与 `uvcpp_c_http3.h`（50 个入口），共 **321 个函数 / 七片**。QUIC 那
  一片给的是"一条连接上很多条流"的完整面（TLS 上下文、ALPN、idle timeout、流开关、
  字节计数），HTTP/3 那一片接在一枚**借来的** QUIC 连接句柄上 —— 所以 h3 的连接句柄
  必须在 `on_disconnect` 里由持有者 `free`，那条纪律写在头里、用例里也钉着（`1.4.4`）
- **写这一批时踩出来的一个接口缺口**：`uvcpp_c_h3_conn_send_response()` 本来是**没
  法用**的 —— C++ 的 `send_response()` 在 `stream_id < 0` 时给 `UV_EINVAL`，而 C 侧
  那枚响应容器的流号（内部 `-1`）从来没有设置入口。补的是
  `uvcpp_c_h3_response_set_stream_id()`，负数**当场** `E_INVALID_ARG`：`-1` 是"还没
  有归属"的日子值不是一条流，而 QUIC 里 **0 是一条真的流**，所以流号的初值不能是 0
  （`1.4.4`）
- **借来的连接句柄由谁反登记，是这一批最要紧的一处**：装上 h3 之后，连接上那张回调
  表被 h3 **整个换掉**，裸 QUIC 的 `on_close` 跳板一次都不响 —— 于是"这条连接没了"
  只剩 h3 的 `on_disconnect` 一个入口。少这一个入口，借出的 QUIC 连接句柄就永远留在
  登记表里，而"再用它必须 `E_STALE`"那一套断言**量不出**这件事（句柄还活着、魔数还
  在）。唯一量得出它的是收尾那句 `uvcpp_c_live_handle_count() == 0` —— 也就是变异
  M18（`1.4.4`）
- **第五个纯 C 用例 `test_capi_quic_h3_func`（280 条断言）**：与 h2 那一份只差一处，
  而那一处被 QUIC 的本质逼出来 —— 它用**一条线程泵两条循环**（两侧都在 UDP 上，两条
  线程就要处理"谁先跑"的时序偶然），握手（ALPN `h3`）之后**串行**发三条请求（POST
  带 body + 同名头设两次 / GET + `_send_status` / GET + 204 空体）。代价是量不到
  `E_WRONG_THREAD`，换上来的是两条**只有 C 面才有**的东西：回调表按 `size` 逐格读、
  每个类型一枚魔数的类型混淆（拿 h3 句柄当 QUIC 连接用必须是 `E_STALE`）（`1.4.4`）
- **变异表第三次扩容，而且"整表要哪棵树"这件事变成了机制**：M17–M20 是 quic + h3 的
  四条（端点回调表的 `size`、上面那条反登记、h3 的 `FrameScope` 摘表、`_set_stream_id`
  拦负数）。这一批还改了两处驱动本身：**两张表各喂一次坏表**（h3 那份与端点那份
  `uvcpp_c_quic_callbacks`，因为"同一句守卫有两个分支、只喂一个"正是批 3a 的 M15 挖
  出来的病），以及**用例缺一个就退 3 并点名是哪几条变异没有判据** —— 本机那三棵
  CI 同构的树各缺一块（`build-capi` 没 SSL/h2/quic、`build-capi-h3` 没 webapp），所以
  整表要一棵全开的 `build-capi-all`（`1.4.4`）
- **符号面锁在 CI 上挂第三条腿**：`http3` 那格从这一批起也带
  `-DUVCPP_ENABLE_CAPI=ON`。没有哪一条腿开得起全部七片（`capi` 格没 SSL/h2/quic、
  `http3` 格没 webapp），所以是三条腿**合起来**把七片都盖上：`capi` 判四片、
  `h2` 判五片、`http3` 判六片（它那棵树里 `webapp` 那一片按"关着的模块一个都不许
  导出"判，如实印「未判」）。哪一片都不许靠"另一条腿会判"蒙过去（`1.4.4`）
- **顺带修掉一个批 3a 留下的头文件缺陷，而它只有"逐份头单独喂 C 编译器"量得出来**：
  `uvcpp_c_http2.h` 的参数表里写着 `struct uvcpp_c_tcp_client*`，却没有 include 那个
  类型的所有者 `capi/uvcpp_c_net.h`。**伞头里编得过**（`uvcpp_c.h` 先 include 了 net
  那份），**单独 include 本头就红** —— 参数表里第一次出现的 tag，C 会新造一个只属于
  这条原型的类型，`-Werror` 下一条红。修法是补那一份 include（形状照
  `uvcpp_c_quic.h`）。教训是伞头的 include 顺序会把这类毛病整个盖住，而"只 include
  我要的那一份"是完全正当的用法（`1.4.4`）
- **1.5.3 把数据库模块的 C 面接上**：新增 `uvcpp_c_db.h`（**81 个入口**），到此共
  **402 个函数 / 八片**。连接 / 同步查询与事务 / 参数 / 结果集与值 / **连接池** /
  **异步门面** —— C#、Rust、Python 的 `ctypes` 今天就能用数据库模块，不用自己写
  `uv_queue_work` 那段样板，也不用自己管一组连接。**这一片有自己的模块开关**
  （`UVCPP_DB_ENABLE`）：db 不接在 net / web 上，所以 `CAPI=ON, DB=OFF` 是合法组合；
  三个后端（SQLite / MySQL / PostgreSQL）一个都没编进来时整个模块被强制关掉。后端
  那一级开关与**符号面**无关 —— 它只决定 `uvcpp_c_db_drivers()` 报出哪几个名字，
  所以符号锁只判 `UVCPP_DB_ENABLE`，不判任何一个后端（`1.5.3`）
- **"加一片不 +1"第四次，而这一次的判据比前几批更直接**：`UVCPP_C_ABI_VERSION` 仍是
  **1**，因为 `git diff --stat origin/master -- src/capi/` 的输出**只有一行**
  （`src/capi/uvcpp_c.h | 12 ++++++++++++`，伞头多一段 `#if UVCPP_DB_ENABLE` 的
  include），既有的七份头与七份 `.cpp` **一个字节都没动**（`1.5.3`）
- **C 的异步接口没有循环参数 —— 这是它与 C++ 那侧唯一一处形状差别**：C++ 的
  `uvcpp_db_async::query(uvcpp_loop* loop, …)` 收的是本库的 `uvcpp_loop*`，而 C 面
  **没有合法的东西可填**：公开头里造不出 `uvcpp_loop*`，而"借调用方自己的
  `uv_loop_t*`"会让 `~uvcpp_loop()` 去 `uv_loop_close()` 并释放一块**别人的**内存。
  给一个没人填得合法的参数比不给更糟（它会诱使 C# 侧传一个猜来的指针），所以
  `uvcpp_c_db_async_*` 干脆不收：完成回调一律在门面自带的那条**懒起**的循环线程上
  被调，`_async_free()` 里叫停并 join；要把结果搬回自己的循环，就在回调里投一次
  `uvcpp_c_net.h` 那族 `*_post()`（`1.5.3`）
- **第六个纯 C 用例，这次是逐后端的**：`test_capi_db_func` 把建表 / 插入 / 查询 /
  事务 / 池 / 异步逐项断言一遍（本机 `build-capi-all` 上 **296 条**，三个后端都给了
  连接串时 **792 条**），承重的几条是"借走唯一一条连接之后第二个 `acquire` 在超时后
  拿到 `NO_CONNECTION` 而**不是挂住**""在回调里 `_async_free` 拿 `E_STATE`""越界列号
  返回静态 NULL 视图"。一个后端都没有时它退 3（**未判定**，不是通过）。CI 的 `capi`
  三格从这一批起都带 `-DUVCPP_ENABLE_DB=ON -DUVCPP_ENABLE_DB_SQLITE=ON`，`db-servers`
  那一格也把 C 面打开，让 C 写的用例对着**真的** MySQL / PostgreSQL 各跑一遍（`1.5.3`）
- **变异表第四次扩容**：M22–M26 是 db 的五条（结果集列号越界、借出的 client 不许
  `free`、池子被门面绑着时不许 `free`、回调表 `on_table` 缺席），仍然是**先写预期
  再跑**，五条全部被抓住；其中三条的形态是**崩溃**（一次 `SIGABRT`、两次 `SIGSEGV`，
  没有 `checks=` 行），照实记在表里而不是读成一句 FAIL（`1.5.3`）
- **符号面锁的 db 那一片只在一条腿上真判，所以那条腿多了一道门禁**：`capi` 那格是
  唯一同时开着 CAPI 与 DB 的腿，而 `UVCPP_ENABLE_DB` 被强制关掉时**锁不会红** ——
  它只在 db 那一行如实印「未判」，然后退 0。也就是说那条腿能从"唯一判 db 的腿"退化
  成"没在判 db 的腿"而全程无声。所以那一格新增一步：配置日志里必须有
  `db: SQLite 后端开` 与 `Including db module in build`，`ctest -N` 里必须有
  `test_capi_db_func`（`1.5.3`）

### C# 绑定（`bindings/csharp/`）

- **这一层有了第一个真消费方，而且随仓一起发**：`bindings/csharp/` 两份 `.cs`
  （`UvcppNative.cs` 183 条 + `UvcppNative.Protocols.cs` 138 条，同一个 partial 类的两半），
  外加一个能跑的 QUIC 回显例子（`dotnet run` 默认做一次回环：服务端收、回显、客户端收到
  同样的字节，逐字节比，收尾断言 `uvcpp_c_live_handle_count()` 回到 0）。**判据是"一条不
  多、一条不少"**：321 条 `DllImport` 与 `tests/tools/capi_symbols.lock` 的 321 个
  `uvcpp_c_*` 符号**双向对账**，两个方向的差集都为空；核法是一段纯文本脚本，不需要库、
  不需要编译（`1.5.0`）
- **它第一次跑就撞出库里的一个必现 SIGSEGV，这是"绑定是 C 面第一个真消费方"的直接价值**：
  例子跑通回显之后**不关连接**、直接 `uvcpp_c_quic_server_free()` → 崩在"边遍历
  `server->conns` 边 `detach_conn()`"（后者的"反登记"那一半会从这张表里 `erase`，而
  `unordered_map::erase` 把**当前迭代器**弄失效，`++it` 踩在已回收的桶上；表里只有一条
  连接也照样崩）。修复是先把句柄抄进一个局部数组再统一 detach；回归用例
  `test_free_with_live_conn()` 与变异 M21 一起进（`1.5.0`）
- **同一版还暴露了一处文档缺口，代价是"回显了两次"**：QUIC 的 `on_read` 收尾是**两条**
  回调 —— 一个"带数据和 FIN 的 STREAM 帧"先报一条 `DATA`（**那条 `DATA` 的 `fin` 也是
  非 0**，它是"这块就是最后一块"的信息位），紧接着再报一条 `PEER_CLOSED`。所以**流的
  结束判据是 `PEER_CLOSED`，不是 `fin`**。例子第一版拿 `fin` 当结束判据，第二次回显时写
  方向已经关了（ngtcp2 给 -219），异常从被 native 调进来的回调里逃出去 → abort
  （本机实测退出码 134）。这条现在写在 `uvcpp_c_quic.h` 的字段注释与绑定 README 里（`1.5.0`）
- **编译门槛是量出来的，不是猜的**：`net8.0` 编过且真跑过；`netstandard2.1` + `LangVersion 10`
  编过（只编过，没跑过）；`netstandard2.0` **编不过**（参考程序集里 `UnmanagedType` 没有
  `LPUTF8Str`）；C# 9 及更早**编不过**（文件范围命名空间要 C# 10）—— 于是"Unity / Mono 也
  编得过"这句话**是错的**（Unity 2022 还是 C# 9），`UvcppNative.Protocols.cs` 里那句自述按
  实测改掉了（`1.5.0`）
- **"手写一份 P/Invoke 声明"会怎么坏，逐条对着真实头文件量过**（绑定 README §八 那张表）：
  最狠的一格是 `bool uvcpp_c_app_running(app)` —— 托管 `bool` 默认按 Win32 `BOOL` 封送
  （非 0 即 `true`），拿一个已经 `_free()` 掉的句柄问它，`int` 声明读到 `-20002`
  （`E_STALE`）、`bool` 声明读到 **`True`**，"出错了"被读成"正在运行"；另有"凭空多一个
  回调参数"（`uvcpp_c_tcp_server_listen` 实际是 `(server, int backlog)`，写成一个回调指针
  的话 C 侧把指针的低 32 位当 backlog 用，编得过、跑得动、语义全错）与"以为它借出一枚
  字符串指针"（`uvcpp_c_req_path` 实际是"你给缓冲区、它还把长度"，写成 `IntPtr f(req)`
  就是少两个参数、C 侧往栈上垃圾地址写）。**这张表就是这一层必须有那份定义类的理由**（`1.5.0`）
- **db 那一片（81 个）这一批没有绑，这是如实列出的缺口**：绑定是手写 + 逐条核过的
  产物，跟着一批新片一起做才能保持"条条都有人读过"，所以它不是自动生成的。锁今天
  402 条，`bindings/csharp/README.md` §七 那个纯文本对账脚本会打印
  `db 0/81 ← 没绑`，其余七片 321 条在**两个方向**上的差集仍为空（`1.5.3`）

### QUIC 传输（net 层）

- **`uvcpp_quic_server::set_reuseport()`：让一次 `listen()` 带着 `UV_UDP_REUSEPORT` 去绑**
  （`1.6.6-dev`）—— 好让 n 个端点抢同一个端口（扇出的那一半在
  `uvcpp_http_server::set_loops(n)`）。**`is_reuseport()` 报的是「这次绑定真的带上标志了
  没有」，不是「调用方请求了没有」**；`set_reuseport()` 在**已经收包之后**调用只打一行
  诊断（标志是 `bind()` 那一刻用的，之后改不了）。`UV_UDP_REUSEPORT` 是 libuv **1.50**
  才有的成员、且是**枚举量不是宏**，所以按 `UV_VERSION_HEX` 分叉出
  `UVCPP_UDP_REUSEPORT_FLAG`（`handle/uvcpp_udp.h`）—— 不是纸面上的讲究：CI 的 ubuntu 腿
  装的是系统 libuv 1.48，直接用那个名字在那里**编不过**。
  `listen()` 失败会把 `reuseport` 回滚成 `false`。`bind("127.0.0.1", 0)` 时**必须先由
  0 号解析出真端口再建工作端点**，否则各挑各的端口、内核分流永不发生
- **`uvcpp_quic_server::set_scid_len(n)` / `scid_len()`（取 `8..20`）**（`1.6.6-dev`）——
  `745608c` 那条缺陷的**回归旋钮**。短包头不把 DCID 长度写在包上（RFC 9000 §17.3.1），
  收方只能按**自己**选的长度反推；而本库两端默认都选 18 ⇒ 「拿**对端** SCID 长度去解
  **本端** DCID」这类错误**恒等抵消**，既有用例一条都不会红（那条缺陷就是这么在平台上
  才现形的 —— 上游 `h2load` 选的是 8）。这个口子把两端拉开，让没有 `h2load` 的 CI 自己
  复现：用例见 `tests/functional/quic_stream_func.cpp` 的
  `test_streams_with_mismatched_cid_len()`
- 把 [ngtcp2](https://github.com/ngtcp2/ngtcp2) 接进构建系统，藏在
  `UVCPP_ENABLE_QUIC` 后面（**默认关**），静态链入，走的是 nghttp2 给 HTTP/2 用的
  同一套 FetchContent + 私有头模式（`1.4.1`）
- **1.4.1 把骨架填成了一条真能通信的链路协议**：握手、流收发、连接关闭与空闲超时
  端到端都通了。骨架期那条 `UV_ENOSYS` 契约由 `tests/functional/quic_api_func.cpp`
  钉住而不是靠散文承诺，所以实现落地时它**真的红了**，逼着契约被显式改写 —— 而不是
  让"框架写好了"这句话悄悄过期。现在 `quic_api_func.cpp` 钉的是**实现**：81 条断言
  覆盖端点契约、关闭路径与空闲超时。新增 `quic_handshake_func.cpp`（22 条）与
  `quic_stream_func.cpp`（33 条），在一条循环上真起服务端与客户端（`1.4.1`）
- **流事件按流报。** `on_read` 多了 `int64_t stream_id` 参数，并新增了
  `on_write` 完成回调 —— 一次 `write_stream()` 对应一次 `on_write`，与
  `uvcpp_tcp_client::write()` 那条"受理不是发出去了"是同一条契约。**不**新增
  `uvcpp_quic_stream` 类：风格照 `src/http2/uvcpp_h2_session.h`，回调自己带流号
  （`1.4.1`）
- **读侧的收尾按流、在它发生的时刻报**：对端 FIN 由 `on_read` 报 `PEER_CLOSED`；
  对端 `RESET_STREAM` 也由 `on_read` 报（应用错误码 0 → `PEER_CLOSED`，非 0 →
  `READ_ERROR`）。`on_stream_close` 是**故意**不接的 —— 它要等两个方向都收场才跑，
  那时读侧已经没有新信息，拿它再报一次会让调用方对同一条流收两次尾（`1.4.1`）
- **修掉一条只在静下来的连接上才现形的丢数据 bug**：从**回调外面**调
  `write_stream()`（刚 `connect()` 完，或用户自己的定时器里）会把字节排进队列，然后
  指望下一个进来的包把它们推出去 —— 而在一条已经静下来的连接上那个包永远不会来，
  等到的是空闲超时。根因是 flush 循环的第一轮跳过了挑流，而静连接上没有待发的非流
  帧，于是 `writev_stream` 返回 0、被当成收工。由 `quic_stream_func.cpp` 第 3 段
  故意"从回调外面写"的用例抓到（`1.4.1`）
- 打开它需要同时满足 `UVCPP_ENABLE_OPENSSL=ON`、一份带 QUIC API 的 OpenSSL ≥ 3.2
  （`SSL_set_quic_tls_cbs`）、以及 `UVCPP_BUILD_NET=ON`；缺任何一条都**强制关闭**并
  打一条说明怎么修的 warning，而不是留一个"能配置、链不上"的组合（`1.4.1`）
- 真实现的部分就是今天能被验证的部分：构建契约、`bind()` 的参数校验
  （`uv_inet_pton` 当场拒掉非法地址，而不是等到 `listen()`）、`UVCPP_QUIC_ENABLE`
  配置宏，以及三个真调进 `libngtcp2` 与 `ngtcp2_crypto_ossl_static` 的后端探针
  （`1.4.1`）
- 顺带修了一个它正好撞上的既有缺陷：OpenSSL 的发现块原先**嵌在
  `if(UVCPP_BUILD_WEB)` 里面**，于是
  `-DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_BUILD_WEB=OFF` 会把 `src/ssl/` 编进去、
  却从不定义 `UVCPP_SSL_LIBS`；而空的 `UVCPP_SSL_LIBS` 展开出来是个**静默的空
  操作** `target_link_libraries()` —— 症状是**链接失败**，而且把 net 层自己的
  TLS 客户端路径一起带坏了（`1.4.1`）
- **还没有的**：0-RTT、连接迁移、无状态重置、datagram（RFC 9221）、multipath、
  多循环支持。这条里原先另外两条都在 1.4.1 关掉了：HTTP/3 现在架在这条传输上
  （下一节），对端的 `STOP_SENDING` 也已经送到应用面前（`on_stop_sending`）。
  传输层自己仍然**不**回一个 —— 这是刻意的（拍板的是上面那层协议），所以
  `doc/quic-guide.md` §8 的措辞是"契约改了"，不是"缺口补了"（`1.4.1`）
- **那一对私有头。** `uvcpp_quic_session.h` 里是 `ngtcp2_conn*`、`SSL*` 与
  `ngtcp2_path_storage`，字段布局跟着 ngtcp2 的版本走 —— 它是第二个私有头，与
  `uvcpp_quic_ngtcp2.h` 并列。两个都不安装（`CMakeLists.txt:3118-3118`）、打包也排除
  （`tests/tools/package_release.py` 的 `PRIVATE_HEADERS`）；量过：
  `cmake --install build-quic --prefix /tmp/inst` 落进 `include/quic/` 的正好是那
  四个公开头（`1.4.1`）
- 顺带补上一条**门禁**的洞：`doc/quic-guide.md` 新加的 4 条片段让
  `tests/tools/check_doc_snippets.py` 在**发布包**上永远退 3（那个包里 QUIC 是
  结构性关着的，片段必然 `[跳]`），而 CI 那一步是 `exit "$rc"` —— 于是不管代码多
  正确，config-contract job 都会永久红。现在那个工具的 `DEFAULT_OFF` 表把这类
  "发布配置里结构性关着"的模块记下来：照样打 `[跳·默认关]`，但不抬退出码。
  **代价写在那张表旁边**（这些片段在 CI 里不会被编，要一份开了该模块的包才判得到），
  防真空转规则也在那里 —— 预期缺席要是把候选全吸收了，照样退 3（`1.4.1`）
- **发送路径上那处大载荷崩溃已定位并修掉。** `write_stream()` 把 `std::vector<uint8_t>`
  里的一个指针交给了 ngtcp2，而那块缓冲会**增长**。ngtcp2 存的是应用给的**指针**
  （重传帧链就拿着它们），而 `ngtcp2.h` 要求被覆盖的那段字节**原样留着**
  （"in tact"），直到 `acked_stream_data_offset` 说它被确认 —— 于是那块缓冲在
  **两个方向**上违反了契约：扩容把地址搬走；丢掉已确认的前缀时把尾巴 `memmove` 上来，
  地址没变、字节变了。症状：`--mode=echo --sizes=2097152` 20 次里崩 3 次
  （`0xC0000005`），出错的那次读在 `ngtcp2_cpymem` ← `ngtcp2_pkt_encode_stream_frame`
  里，目标区域是 `MEM_RESERVE`（越过了某个堆块已提交区的末尾）。先把"地址与字节都不许动"
  临时焊死，**20 次 0 崩**，而未修的对照是 3/20；正式修法是**分块发送队列** —— 每块出生时
  `reserve(64 KiB)`、此后只写到这里为止、只在**整块**都被确认时才丢 —— 两条约束都按构造
  成立，丢块 O(1) 且不搬字节。**"代价百分之几"这句话量不出来，而原因本身是个发现**：
  把 HEAD 那份存储换回来重编、跑同一个量具，**6 次运行（3 次独立构建 × push/echo）全部**
  在 15 轮内报 `DATA MISMATCH`，一行耗时都不打印，首个不符的流偏移在 63 605 ～ 1 811 177
  之间浮动 —— 旧形状**每次**吐出的字节都是错的，不是"慢一点"，所以那个基线在 2 MiB 上
  根本不存在，拿它当分母没有意义；崩（3/20）只是它最响的那种症状。分块大小则是量出来的
  （2 MiB 单向：16 KiB 25.410 ms / 64 KiB 21.907 / 256 KiB 21.913，拐点在 64 KiB）。
  `quic_stream_func.cpp` 加了第四段，回显 64 KiB 与
  3 × 64 KiB + 1234 B 并**逐字节**比对整条流 —— 计数式判据恰好对这类错位免疫：一个
  **对调两个等长块内容**的变异体（长度、记账、送达与两边收到的**字节数**全都不变）
  只有逐字节判据抓得住，且它报出的第一个不同偏移正是块边界（`1.5.1`）
- **QUIC 的数据报路径不再每包拷一次。** 两个端点现在先试 `uv_udp_try_send()` —— 它同步，
  返回时 `data` 已死，不必拷 —— 只有不是"已发出"才落回原来的异步拷贝入队那条路。
  2 MB 一笔就是约 1700 次 `new char[]` + `memcpy` + `delete[]` 与约 1700 次多余的循环轮转。
  只有成功才走快路，这不是保守而是必需的：在失败处 `return` 等于**静默把这个包扔了**，
  而 QUIC 只会把它当丢包去重传（症状是"能跑但慢"）。libuv 在已有异步发送排队时返回
  `UV_EAGAIN`，所以快路的包不会插到慢路的包前面（`1.5.1`）
- **QUIC 的数据报上限从 1200 抬到 1500，把 ngtcp2 自带的 PMTUD 从「被整条关死」
  救回来。** `settings.max_tx_udp_payload_size` 原先逐字写着
  `NGTCP2_MAX_UDP_PAYLOAD_SIZE`(1200) —— 那是"任何路径都保证能过"的**下界**，
  看着最保守，实际是硬上限：`conn_start_pmtud()` 拿它当上限，ngtcp2 自带探测表
  `{1406, 1342, 1232, 1444}` **四档全部越界**，那个 `for` 一路 `continue` 过去、
  `ngtcp2_pmtud_finished()` 立刻为真，此后每个数据报都被钉死在 1200 字节，
  **全程无声**。2 MiB 单向因此要 1748 个数据报，本机每包约 10 µs 的内核税一分
  不打折。改法由常量 `kDatagramBufLen` 1200 → 1500 一处收口（落点缓冲、两条 init
  路径的 settings、以及**进 Initial 包**的 `transport_params.max_udp_payload_size`），
  `kRecvBufLen` 的注释同时订正 —— 4096 的依据是我们对外宣布的那一档，不是 1200
  那个下界。**判据用结构证据而不是墙钟**：接收侧 `on_read(DATA)` 次数对照臂
  1743/1761/1766/1761/1747、改完 1444/1407/1446/1442/1392，**两组一次都没有重叠**
  —— 同一份二进制的最小值在 20.7～24.9 ms 之间跳，单看耗时分不出结论；push 最小
  21.603 → 17.539 ms、echo 39.866 → 35.459 ms，全尺寸扫描（1024 B … 2 MiB）每一档
  都变快，没有一档变慢。两条 `static_assert` 守着这个缺陷的**两个入口**，都用对照臂
  验过：钉回 1200，两条同时报出各自的理由、**构建立刻失败**。**一处线上一档行为
  变更**：我们宣布的 `max_udp_payload_size` 由 ngtcp2 默认的 65527 收到 1500，
  与对端是不是本库无关了（以前撞不上，只因为对端也被同一个常量钉着）—— 换一个照
  RFC 走的第三方实现，旧的样子会在内核那层被截断、AEAD 校验失败、当丢包重传，是一处
  只在跨实现时出现的静默性能悬崖。1444 是探测表的最大一档，想再往上要自己加探测档。
  地板那一列也因此**换了口径**：`bench_quic` 新增 `--pkt=N`（默认 1444）并在输出里
  自报 `pkt=` —— 量的是哪一档由它说，不靠默认值（`1.5.1`）
- **ACK 阈值从 ngtcp2 默认的 2 提到 16，回环/局域网上少发 27% 的数据报。**
  `ack_thresh` 的默认值直译自 RFC 9000 §13.2.1 那句 SHOULD（"收到两个 ack-eliciting
  包就回一个 ACK"），在**低 RTT 链路**上代价很大，原因是一道乘法：延迟 ACK 的到期
  时刻是 `first_unacked_ts + min(max_ack_delay, max(srtt/8, 1ns))`，回环上 `srtt`
  只有几十微秒 ⇒ 那个窗口塌到微秒级，`ack_thresh` 成了**唯一**还在起作用的闸门，
  接收端几乎每收两个包就回一个 ACK，而每个 ACK 都是一次独立的 `sendto`。实测
  （MinGW Release、2 MiB push、同机同轮交错 A/B × 4 组，内核实发数据报数当独立
  旁证）：服务端控制包/轮 **821 → 161**、客户端载荷包/轮 1623 不变、总数据报/轮
  **2445 → 1783（−27%）**、中位耗时 19.25 → 13.81 ms。五档尺寸 × 两种模式全部同向
  改善，唯一例外是 1 KiB push 的 −1.5%，落在 0.014 ms 的分辨率噪声里。再往上加到
  32 / 64 / 128 没有系统差 ⇒ 取**到达平台的最小值**，离那句 SHOULD 的偏离最小。
  **成本说清楚**：ACK 变稀 ⇒ 对端检测丢包、推进 cwnd 的反馈变慢，上界是延迟 ACK
  计时器（`max_ack_delay` = 25 ms，协议允许），有丢包的链路上这是一笔真实取舍。
  本机回环无丢包，量到的全是收益 —— **别把这个数读成"公网也 +38%"**：广域网上
  `srtt/8` 是毫秒量级（30 ms RTT ⇒ 3.75 ms），远大于包间隔，延迟 ACK 计时器才是
  那个闸门，这一档取多少都一样出去（`1.5.1`）
- **QUIC 的发送路径在 Windows 上把数据报批量交出去。** `UVCPP_ENABLE_UDP_GSO`（默认值
  是派生的：Windows 且带 QUIC 时为开）把一次聚合出来的**一批等长数据报**用
  `UDP_SEND_MSG_SIZE` 在**一次** `WSASendTo` 里交给协议栈。Windows 上 libuv 是一个数据报
  一次 `WSASendTo`（`uv__udp_try_send2()` 就是个 `for` 循环）—— MsQuic 对照里那笔
  "每数据报成本"正是在这里；Linux 那边上游已经把每批 20 个折成一次 `sendmmsg()`，所以
  这个开关在那里整段编掉，显式打开也不生效。分段尺寸取的是**本次聚合写回填的值**、
  不是常量，所以线上跑的还是原来那些数据报；收方向**故意没做** —— 半开会让 libuv 把
  粘在一起的数据报当成**一次**读交给应用（`1.5.2`）


- **流关闭时把额度还给对端**。服务端从不补发 `MAX_STREAMS` —— `ngtcp2.h` 写着它不自动
  补，而本库注册了 `stream_open` 回调、使那个例外不成立，所以得自己发。补上之前一条
  连接终生只服务 100 个请求；判据是阶梯实测 64 连接 × 64 流 = 6400 = 64 × 100，且
  服务端只烧 0.02 核。（`1.6.1`）

- **短包头的 DCID 按「本端发出去的 CID 有多长」解，不按对端的 SCID 长度解。** 短包头
  不把 DCID 长度写在包上（RFC 9000 §17.3.1），收方只能按**自己**选的规则反推；而两端
  各自选自己的长度（RFC 9000 §5.1）。本库服务端发 18 字节 SCID，上游 `h2load` 用 8 字节
  —— 拿 8 去解一条 18 字节 CID 的短包头，解出来的 key 一个都命中不了路由表，于是
  握手之后**每一个 1-RTT 包都被当成陌生 CID 丢掉**，双方只能靠 PTO（初始约 1 s）重传
  往前挪。表象极具迷惑性：服务端 CPU≈0（它在等）、每连接每 5 秒只出几个响应、0 个 5xx。
  判据（本机上游 `h2load`，平台那条一模一样的命令 `-c 64 -m 64 -t 64 -D 5s`，三轮）：
  改前 **315 / 255 / 150** 个请求、63.0 / 51.0 / 30.0 req/s、每连接 ~4.9 个响应；
  改后 **429 553 / 412 692 / 399 236**、85 910.6 / 82 538.4 / 79 847.2 req/s、
  每连接 ~6 700 个响应，0 个 5xx。单连接那一格更干净：`-c 1 -m 1 -t 1 -D 5s` 改前
  5 秒只出 **1** 个请求（且失败），改后 **4 386** 个。
  `quic_session::scid_len()` 在这个改动之前**一个调用者都没有**：契约（含 RFC 引用）
  写在 `uvcpp_quic_session.h` 里，而调用点自己发明了一个 —— 拿对端 SCID 长度顶上。
  两端同源的用例永远看不见它（本库客户端也选 18 字节），换一个对端才现形。（`1.6.5-dev`）

- **一个 QUIC 数据报装多条流的 STREAM 帧 —— `sendmsg` 从 1.03 次/请求降到 0.05**
  （`1.7.2-dev`）。`quic_session::do_flush()` 现在每轮都带
  `NGTCP2_WRITE_STREAM_FLAG_MORE` 调 `ngtcp2_conn_writev_stream()`。不带这个位走的是
  ngtcp2 的默认行为 —— 它自己的文档原话是 "by default, application can only send
  1 STREAM frame in one QUIC packet"（`ngtcp2.h`）—— 于是收侧一个数据报能装 28 个请求、
  **发侧一个应答一个数据报**，每个包都单独走一遍包头构造、AEAD 与 `ngtcp2_conn_write_pkt`。
  读数（本机 h3 档 `-c 64 -m 64 -t 9 -D 5s`，A B B A，确定性判据）：`sendmsg`/请求
  **1.0316 / 1.0360 → 0.0428 / 0.0561**（19~24×），系统态微秒/请求
  **3.498 / 5.322 → 0.404 / 0.630**，合计微秒/请求 **16.299 / 24.428 → 12.750 / 15.651**。
  每请求字节量不变（13.28 → 13.25）。

  这里有个坑留给下一个人：**"包还开着"不能用返回值的正负判**。`ngtcp2_conn.c` 在返回
  `NGTCP2_ERR_STREAM_DATA_BLOCKED` **之前**就置上了 `NGTCP2_CONN_FLAG_PPE_PENDING`，
  所以判据是 `more = (flags & MORE) != 0 && nwrite < 0`，而且
  `NGTCP2_ERR_WRITE_MORE`（同样是负数）必须单独认出来、不能落进"连接级错误"那一支。
  判错的症状不是"慢一点"：带着 `PPE_PENDING` 去调 `ngtcp2_conn_read_pkt()` 会撞上
  `ngtcp2_conn.c` 里的 `assert(!(conn->flags & NGTCP2_CONN_FLAG_PPE_PENDING))`。
  **触发条件是连接级流控** —— 突发超过对端广告的 `MAX_DATA` 才会走到那条返回上，而一个
  一个地发请求永远到不了那个水位，所以这条缺陷在既有用例下**一条都不会红**。新增
  `tests/functional/http3_large_body_func.cpp` 专打它（9 条请求、合计约 1.8 MiB，在
  `connect` 回调里一次发出，逐字节比对），在新码上 3/3 绿、在旧码上 3/3 以那条断言中止。

- **发送队列每块的容量按「这次要写多少」定 —— h3 每请求的分配字节 −89.6%、峰值常驻内存
  −40 MiB**（`1.7.2-dev`）。`stream_send` 的块此前一律 `reserve(kChunkSize)`（64 KiB），
  而那个值是按 2 MiB 单流的大块传输量出来的，HTTP/3 的常态却是"每条流只发几百字节"——
  于是每块都是 64 KiB 里只用掉最前面一点。本机 h3 量具（`-c 64 -m 64`）上的账：
  **每请求分配 65 778 字节里 65 536 是这一个 `reserve`，占全部分配字节的 91.6%**；而
  64 连接 × 64 条并发流 × 64 KiB = 256 MiB 的活块，与平台 `baseline-h3-64` 报的
  238 MiB 对得上 —— 它不只是一个显示数字，每块都要被写一遍、再被 ngtcp2 读一遍，
  4096 块散在堆上，每次访问都是冷页。

  改成在 `[kMinChunkSize, kChunkSize]` 之间按这次写入量取容量之后：分配字节/请求
  **72 374.8 → 7 507.5**（−89.6%，A B B A 两序各一次），峰值 RSS
  **91 / 90 MiB → 51 / 50 MiB**；而分配**次数**几乎不动（53.3891 → 53.3875）——
  这一条省的是"每块占多大"，不是"分配几次"。`utime+stime` 在服务端受限的形状上是
  12.03 → 11.91 µs/请求，落在散布内（无回归；本机端到端本来就分辨不了库内改动）。

  代价是 `next_sendable_stream()` 里"第几块 + 块内偏移"不能再一次除法算出来，改成从
  前往后累加地跳过 `sent_off - base_off` 个字节。**`settle()` 不用改** —— 它本来就是拿
  `chunks.front().size()` 判整块确认的，与每块多大无关。

### HTTP/3（web 层）

- **h3 上的「稍后应答」通了 —— `static-h3` 那一档从此订阅得上**（`1.6.8-dev`）：
  h3 派发从前对 `resp.deferred` 一律回 500 + 写 stderr（那条腿上交给处理函数的
  `uvcpp_tcp_client*` 是 `nullptr`，没有任何东西能"稍后回话"）。补的是
  `uvcpp_http_reply` —— 一枚**与传输无关的**「稍后回话」凭据（见下面「HTTP 与
  WebSocket 语义」那条）；h3 上它由 `quic_ctx` 的按流表兜住，在
  `on_stream_close` 回调里注销。于是 `uvcpp_static_server` / `uvcpp_web_static`
  那条 deferred 的路在 h3 上原样走通。**判据两半**：①
  `tests/functional/http3_web_func.cpp` 里那条「不支持的路径显式拒绝」改成
  「被支持」—— `deferred` 从 500 变 200，`set_stream_claim` 与 `post_stream`
  仍是 500（它们要的是另一套能力），并加了一条"三个条件全关时仍 200"的对照；
  ② 平台 `scripts/validate.sh` 里那条断言
  `h2load --alpn-list=h3 -n 4 -c 1 -t 1 https://localhost:$H2PORT/static/reset.css`
  本机实测 **4/4 个 2xx**（修前 0/4）。同一份条目按 `static-h3` 的形状压
  （`-c 64 -m 64 -t 8 -D 3s`）：**17,433 完成 / 17,505 个 2xx，约 5,811 rps、
  120 MB/s**
- **`uvcpp_static_server` 的多循环竞态**（`1.6.8-dev`）：本类的 `cache_` 与
  `retired_` 从前是"循环线程只有一条"写的（两张表都没有锁），而 h3 那条腿
  **只有本类可用**（`uvcpp_http_server` 没有 `uvcpp_web_app` 那种 `serve_static()`），
  h3 端点又默认扇在 `set_loops(n)` 条循环上 —— 它就这样从"没有生产调用点的代码"
  变成了线上路径。按 `uvcpp_web_static` 修好时那份形状补齐：`mu_` 护两张表、
  `retired_` **按循环分桶**（`delete` 只能在该循环线程上做；合成一个桶会把别的
  循环还在跑回调的 `uvcpp_work` 删掉，是 UAF）。**判据是入口层的 A/B**（同一份
  条目、同一台机器，h2load-h3 打 `/static/*`）：修前 `-c 4` 起立刻死、`0 done`
  （段错误落在 `drain_retired()` 的 `delete` 上）；修后 `-c 16` →
  **16,690 / 16,690 全 2xx**、`-c 64` → **17,431 完成 / 2 失败**，进程存活。
  新增用例 `tests/functional/web_static_server_func.cpp::multiloop_concurrency`
  （4 条工作循环 × 8 路 × 120 次，逐条比状态码与 body 长度）钉的是**多循环下的
  数据正确性**；它**不是**"修前必红"的那种 —— 实测把本类 5 处加锁全摘掉重建后
  它照样全绿（4×8×120 与 4×16×300 各跑 3 次），所以它绿**不能**当成锁还在的证据
- **`listen_quic()` 会把 QUIC 端点扇到工作循环上**（`1.6.6-dev`）：`set_loops(n)` 之后
  0 号端点建在 acceptor 线程、1..n−1 号各投递到一条工作循环上建（有界等待，任一失败
  整体回滚）。`quic_fanout()` 报成没成、`quic_connection_count_at(i)` 报每端点的连接数
  （合计等于 `quic_connection_count()`）。web 层那三张按连接索引的表跟着切成
  per-loop，取表用 `uvcpp_loop_index_of_this_thread()` —— **连接所属循环 ≡ 当前线程
  所在循环**。**`listen_quic()` 之后再 `set_loops(n>1)` 返 `UV_EBUSY`**：纯 h3 的服务端
  TCP 从未 `listen()`，不补这道门就会出现"端点 1 个、表 1 张、工作线程报 1..n−1 号"
  ⇒ 跨线程写同一张 map。析构三段式：**先**在 `stop_workers()` 之前把工作端点连它们的
  h3 连接投递到各自循环上删掉（有界等待），**再** `stop_workers()`，**最后**收 0 号 ——
  顺序反了就是 UAF
- [nghttp3](https://github.com/ngtcp2/nghttp3) 被接进构建，挂在 `UVCPP_ENABLE_HTTP3`
  后面（**默认关**）、静态链入、位置在 **web 层**，而且它需要下面的 QUIC 传输
  （`UVCPP_ENABLE_QUIC=ON`）—— h3 **就是** QUIC，没有 h3-over-TCP 这回事（`1.4.1`）
- **1.4.1 把它做成端到端可用的传输，不是一个骨架。** `uvcpp_http_client` 多了
  `set_http3_enabled(true)`，`uvcpp_http_server` 多了 `set_quic_ssl_context()` +
  `listen_quic(port)`，而两者走的是**同一张**路由表、同一个处理函数签名、同一个
  `uvcpp_http_response` —— 处理函数不知道自己在答哪条传输（`1.4.1`）
- **`"h3"` 绝不进 TCP 的 ALPN 名单。** 在 TCP 的 ClientHello 里报 `h3` 是无意义的：
  对端选了我们就会把 h3 的二进制分帧喂给 HTTP/1.1 解析器，症状是"连上了、写得出去、
  响应永远不来、哪里都不报错"。客户端那份 TCP ALPN 名单**一个字没动**，h3 走
  `uvcpp_quic_client`，ALPN 由它自己带（`1.4.1`）
- **HTTP/1.1 根本不在 h3 这条路上** —— 这是本批的硬要求，而且是**量出来的**、不是
  承诺的：`-DUVCPP_ENABLE_HTTP3=OFF` 时 `uvcpp_http_server.cpp`、
  `uvcpp_http_client.cpp`、`uvcpp_tcp_client.cpp` 三者的编译器命令行与上一版**逐字节
  相同**，`nm --print-size --size-sort` 给出的 h1 热路函数尺寸也相同
  （`on_tcp_connection`、`on_connection_data`、`on_request_complete`）。h3 用的是
  另一张上下文表（h1 的 `conn_ctx` **一个字段都没加**）和 h1 从不碰的 UDP socket
  （`1.4.1`）
- **它向 QUIC 要了三样东西，三样现在都是公开 API**：FIN 与 `RESET_STREAM` 能分开
  （`net_read_result::fin`）、流额度事件（`on_streams_available` + `streams_left()`，
  让那三条关键单向流在额度到达时开出来，而不是轮询）、以及 `STOP_SENDING`
  （`on_stop_sending` + `shutdown_stream_read()`）。`shutdown_stream()` 也不再写死
  错误码 0 —— 取消一条流和把它发完不是一回事（`1.4.1`）
- **分层照 `src/http2/` 抄**：`uvcpp_h3_session` 是纯的"收字节吐字节"引擎（不知道
  socket 是什么），`uvcpp_h3_connection` 驱动它跑在一条 `uvcpp_quic_connection` 上。
  只有 `uvcpp_h3_common.h` 与 `uvcpp_h3_connection.h` 发出去；`uvcpp_h3_nghttp3.h`
  （唯一 include nghttp3 的地方）与 `uvcpp_h3_session.h` 是私有头，与 quic 那一对
  同形（`1.4.1`）
- **两条功能用例钉着它**，钉的是值不是"发生过什么"：`http3_request_func.cpp`
  （95 条检查 —— 握手、真处理函数答的真 GET、`:method`/`:path`/`:status` 的 token
  映射、body 恰好等于 `hello-h3`、恰好一次 的完成队列、close）与
  `http3_web_func.cpp`（92 条检查 —— 同一件事走 web 层，外加**同一个** server 对象
  在另一条 socket 上照旧答一次 h1 请求）（`1.4.1`）
- **变异表里有两行红 0 条，如实记着**：在 nghttp3 自己的回调里重入它的写函数，只有
  评审或 sanitizer 抓得到；另一条 `add_write_offset()` 的变异**设计上就不可达**
  （本层永远把 EOF 与最后一块数据一起置上）。两条都写进 `doc/http3-guide.md`，没有
  算成"已覆盖"（`1.4.1`）
- 哪些真、哪些不真：`POST` 请求体可以，但请求/响应 trailers、GOAWAY、Server Push、
  extended CONNECT、h3 上的流式路由、多循环 h3 都**没做** —— 逐条列在
  `doc/http3-guide.md` §4 与 §8，而且不支持的路径是**显式拒绝**，不是静默路由错
  （`1.4.1`）
- QUIC 那批片段捅出来的门禁洞现在有了第二条：`doc/http3-guide.md` 的 3 条片段对着
  一份开了 h3 的包判（3/3 编过），而在发布包上它们打 `[跳·默认关]`、不抬
  `check_doc_snippets.py` 的退出码 —— 那张表和代价写在工具里、也写在指南 §5
  （`1.4.1`）
- Linux 那条 h3 格还带一步专门的门禁：**h2 + quic + http3 同时配置**，并断言三条
  集成消息都在。理由是 nghttp2 / ngtcp2 / nghttp3 各自都无条件建一个名叫 `check`
  的目标，第三个进来的会让 `cmake` 撞名失败、而报错里既没有库名也没有文件名。放在
  Linux 那条腿上是因为那是**本来就要一次 `cmake` 把这仨全配起来**的那条腿 ——
  撞名本身与平台无关（`1.4.1`）

### HTTP/2

实现现状 —— 强制了什么、默认值是什么、哪些是刻意不支持的 —— 见
[`doc/http2-status.md`](doc/http2-status.md)。

- 接入 [nghttp2](https://github.com/nghttp2/nghttp2)、ALPN 基础设施、h2 会话层与连接层
  （`1.1.1`），并接进请求层与 Web 应用框架（`1.1.2`）
- `uvcpp_http_client` 与 `uvcpp_http_server` 默认仍是 HTTP/1.1，要显式
  `set_http2_enabled()`；`uvcpp_web_app` 自己协商版本（`1.1.3`）
- 不再把 GOAWAY 当没发生；停机时先道别再拆连接（`1.1.6`、`1.1.7`）
- 发方向超出对端公布的上限时不再静默丢帧（`1.1.5`）
- 拆连接路上的两个 use-after-free 与一处泄漏（`1.1.7`），回调栈里不再重入
  `mem_send()`（`1.1.9`）
- 流级背压：收方向的 `pause_stream()` / `resume_stream()`、发方向的单流待发队列上界、
  以及 `peer_window_size()`（`1.2.25`）。这是**协议层机件，暂无应用层调用方** ——
  框架侧没有任何调用点，h2 上"边收边给"的流式请求体因此仍不可达


- **`WINDOW_UPDATE` 不计进控制帧预算**。那张单子是"回一帧 = 分配一帧"的帧，`PING` /
  `SETTINGS` / `RST_STREAM` / `PRIORITY` 都在其中；`WINDOW_UPDATE` 不在，理由是它的
  正常速率**与我们已经发出去的字节数成正比**，任何帧/秒的上限都会变成吞吐上限 ——
  32 帧/秒 × 32 KiB = 1 MiB/s，一条连接只要跑得比这快，就会被自己人掐断。判据：默认
  64 KiB 窗口的客户端，一条连接稳定在 7 个 200 KiB 响应（≈1.4 MiB）后开始 `RST_STREAM`
  且与并发流数无关，移出后 64/64 全通。（`1.6.1`）

- **明文 h2（先验知识 h2c）有了，默认关**（`1.6.7-dev`）。`set_h2c_enabled(true)`
  之后，非 TLS 连接的**头 24 个字节**与 RFC 9113 §3.4 的连接前言
  （`PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n`）**逐字节**比一次：全中 ⇒ 这条连接交给 h2，
  否则 ⇒ 照旧给 llhttp，而比过的那几个字节一个不少地**补喂回去**。两件事必须一起
  说清楚：这不是嗅探（判据是固定串的逐字节比较，两边都由确定的字节决定，没有"看着像
  就按它处理"这一档），也**不做** `Upgrade: h2c` 协商（RFC 9113 §3.1 已经删掉它）。
  合法 HTTP/1.1 请求里没有哪一条会长成前言的样子（`PRI` 不是一个方法），所以这里
  没有"误判"这一类风险。代价只在**打开之后**每条明文连接的头 24 个字节上，且定案
  即停；默认（关）比都不比。两条腿的口径不同，所以它和 `set_http2_enabled()` 是两个
  开关且**都要开**：那个管 TLS 上宣告 h2（默认开，ALPN 协商不成自动回落、零风险），
  这个管不加密的连接上认不认前言（默认关，它改变的是明文连接的协议判定）。
  `h2c_enabled()` 如实回报"真能开吗"而不是"你设过什么"
- **改判那条路上最贵的一句是"别跳过已经比过的字节"**（`1.6.7-dev`）。第一版把移交
  给会话的字节写成 `data + matched` —— "前言反正我们已经认过了，剥掉"。症状是服务端
  **只发得出去自己的 SETTINGS 就再没动静**（连客户端 SETTINGS 的 ACK 都没有），
  量到的返回码是 `-903` = `NGHTTP2_ERR_BAD_CLIENT_MAGIC`：服务端会话**自己就要认**
  那 24 个字节，它不是我们替 nghttp2 剥的包装。改成把 `h2c_probe`（前几段）拼上
  `data` **整段**：本段被比对过的 `matched` 个字节留在里面（前缀在前、本段整段在后，
  正好是从字节 0 起的完整流，既不跳也不重），前言之后跟来的第一批帧也因此同批交出去
  （前言与第一帧挤在同一个 TCP 段里到达是常态）
- **一条明文连接按 h1 登记之后再改判成 h2，三件事有次序，缺一件留一个静默的坏状态**
  （`1.6.7-dev`）：① 先摘掉 h1 那份 `conn_ctx`（含 llhttp 解析器）—— 不摘就是被新
  登记的那份**盖掉**，那个解析器每连接一个、当场泄漏；② 再 `read_stop()` 卸掉 h1 的
  读回调 —— `read_start_events()` 对"已经注册过读"的连接返回 `UV_EALREADY`，不卸
  h2 层就起不来，连接静默变成"收了字节什么都不做"；③ 最后建会话、把探测期攒下的
  字节喂进去（喂在 `start()` 之后：新的一次读回调只可能在下一轮循环里跑，而这里是
  同步的，所以"先喂 pending、再收新字节"是确定的）。**丢不掉字节**：改判正跑在那个
  读回调里面，中间没有一次循环迭代。`on_connection` 钩子在那条连接上**只跑一次**
  （按 h1 登记的那次），不因为改判再跑一次 —— 否则调用方的每连接计数翻倍
- **判据**（`tests/functional/web_h2_cleartext_func.cpp`，八轮，`1.6.7-dev`）：① 真
  h2 客户端会话打一次往返（断言首批发出的 24 字节就是前言、回 200 与正确的体）；
  ② 同一端口上 h1 逐字节不变；③ 前言**一次一个字节**写 24 次（间隔 10 ms），这一轮
  先断言 `raw_data_hook` 收到过 **≥2 段**（否则整轮退化成"一次整读"，什么都没证明），
  再断言回的是 h2；④ 在**最后一字节之前**切开；⑤ 前缀 `"P"` + 余下 `"OST …"` 两段
  ⇒ 必须回 `HTTP/1.1 200`（既抓吞掉攒下的那个 `P`、也抓把它重复喂一遍）；⑥ 前 23
  字节对、第 24 字节岔开 ⇒ 整条连接定案 h1；⑦ 两个开关都关时同一段字节 ⇒
  `HTTP/1.1 400`（对照组）；⑧（`1.6.9-dev` 加的）尾随头 —— 它挂在这个文件里是因为
  尾随头只有在"一条连接能承载多条流"的协议上才存在（详见 HTTP/2 那一节）。稳定
  38/38（分段的长度向量跨次逐字节相同，输出逐字节相同）；变异各自把对应用例打红：
  移交时跳过 `matched` ⇒ 第 ⑤ 轮红，跨读段不攒 `h2c_probe` ⇒ 第 ③④ 轮红，
  第 ⑧ 轮把 `send_trailers()` 换成 `end_stream()` ⇒ 尾随头两条红而"收完"那条**照旧绿**
  （这就是"只断言 END_STREAM 是假绿"的实测），h1 那条路不返 `UV_ENOTSUP` ⇒ 红。
  回归：`ctest -R "h2|http|web"` 67/67，
  `build-rel15` 全量 128 项 0 红（一条 DB 用例按惯例 skip）

- **h2 能发尾随头了 —— gRPC 那条路上唯一的库级缺件**（`1.6.9-dev`）。
  `uvcpp_h2_session::submit_trailer()` → `uvcpp_h2_connection::send_trailers()` →
  `uvcpp_http_server::send_trailers()`。发这一段本身只要一次
  `nghttp2_submit_trailer()`；难的是**什么时候发**：尾随头是一个自带 `END_STREAM`
  的头块，抢在响应体前面出去就是"END_STREAM 在前、字节在后"，对端把这条流判成残缺。
  会话层的闸因此只看**待发队列空不空**（`chunks.empty()`）—— 队空 ⟺ provider 手里
  一个字节都不剩。**不能拿 `frame_pending` / `deferred` 当"还有字节欠着"的证据**：
  那两个标志是**粘的**（置上就不再落，最后一次 provider 调用还会再把 `deferred`
  置一遍），拿它们当判据，这条路上永远只会拿到 `UV_EBUSY`，等于整个功能没接上
  （变异 M1 实测 `rc=-16`）。调用方把最后一块的 `done` 当信号 —— 那是那批字节写完之后
  才跑的回调。给欠字节这一档 `UV_EBUSY`（"你现在就该等"）而不是 `UV_EALREADY`
  （"这条流的状态根本不对"），是为了让调用方能一眼分开这两件事。
  **每一种失败都保证"什么都没发生"**：没入队、没动流状态、没置 `ended`，于是
  "先发一下、拿到错再等"这种用法不会把流带进半死状态。收的那一侧本来就在
  （尾随头块 category 是 `HCAT_HEADERS`、字段折进同一个 `response.headers`、
  由它的 `END_STREAM` 收尾），这次是**对称着把发的那侧补齐**。h1 上明说
  `UV_ENOTSUP`，**不"照发一遍"**：后者把尾随头当普通响应头塞进首部，调用方看着
  "发出去了"、对端收到的却是另一份报文
- **尾随头的判据**（`1.6.9-dev`）。会话层三条（`tests/functional/h2_session_func.cpp`）：
  ① 真往返 —— 发一块 `aaaa`、再发一块 `bb`，在第二块的 `done` 里发三字段尾随头
  （故意混一个 `connection: close`），客户端在 `on_response_end` 那一刻的头列表里
  必须看到 `grpc-status: 0`、`grpc-message`（空值）**在**、`connection` **被剥掉**、
  而 `grpc-status` **不在**首部块里（`on_response` 那一刻抓的那份头）；② 一个字节
  的 body 都不发、直接发尾随头（证明它不依赖流式队列被碰过）；③ 四条拒绝路径
  （流不存在/伪首部/空名/值里带 `\n`/队列还欠着字节/已经收尾）。三条变异各自把
  对应用例打红：粘标志当判据 ⇒ `rc=-16`；伪首部改成"剥掉"而不是"拒绝" ⇒
  「伪首部应当 `UV_EINVAL`」红；忘了置 `state`/`ended` ⇒「第二次尾随头应当
  `UV_EALREADY`」红。web 层第 ⑧ 轮走真 socket + 真路由，并且**顺手把反例的形状
  钉住**：把处理函数里的 `send_trailers()` 换成 `end_stream(client, sid, false)`，
  那条"整条响应收完（含 END_STREAM）"**照样绿**（两者带的都是 `END_STREAM`）——
  也就是说**只断言 END_STREAM 是假绿**，真正的判据是"`on_response_end` 那一刻的
  头列表里有 `grpc-status`"。同一轮还拿同一个端口的 h1 那一侧断言 `UV_ENOTSUP`、
  且报文里看不到 `grpc-status`（变异：h1 那条路直接 `return 0` ⇒ 红）。稳定 38/38
- **`:scheme` 不再钉死 `https` —— 明文 h2c 之前整条腿是坏的**（`1.6.12-dev`）。
  1.6.5 加的 h2c（先验知识）在本仓所有用例里都是绿的，但那是**两端同源**的绿：
  收方向把 `:scheme` 钉死成 `https`（`handle_pseudo()`），发方向也钉死成
  `https`（`submit_request()`），两边一起错、恒等抵消 —— 与 h3 那个 CID 长度
  缺陷同一形状。真客户端不这么发：`curl --http2-prior-knowledge http://…` 与
  `h2load -p h2c` 发的是 `:scheme: http`，于是**每一条流**都在那里被
  `RST_STREAM(PROTOCOL_ERROR)` 关掉，症状是"连上了、发出去了、永远等不到响应"。
  修法是让取值跟**实际传输层**走：会话新增 `set_tls()` / `is_tls()`，
  `uvcpp_h2_connection` 一律按 `client->is_tls()` 覆盖（库内唯一的构造者，客户端
  与服务端两条腿都走它），收方向要求"与传输层一致"（TLS ⇒ `https`，明文 ⇒
  `http`），发方向按同一个值填。**不改成"两个都收"**：一条明文连接自称 `https`
  正是 RFC 9113 §8.1.1 要拦的混淆代理入口（缓存键、Cookie 的 `Secure`、HSTS 都会
  按 https 的语义处理它），所以那一格仍判 `RST_STREAM`。判据落在**手拼字节**上
  而不是本库客户端上（`tests/functional/web_h2_cleartext_func.cpp` 的
  `raw_h2_get()`：前言 + 空 SETTINGS + 一条 `0x00` 字面量头块），两轮只差
  `:scheme` 一个字 —— `http` 必须派发（DATA 里出现 body）、`https` 必须被 RST
  且 body 一个字节都不出现；再拿真会话 `drain()` 出来的字节回头打给服务端，
  把"两端各自独立地按传输层取值"闭合。修前那一格实测
  `49 字节：SETTINGS + SETTINGS-ACK + RST_STREAM(stream 1)`；修后
  `97 字节：SETTINGS + ACK + HEADERS(END_HEADERS) + DATA(world, END_STREAM)`

### gRPC（`src/grpc/`）

动机是 HttpArena 的 `unary-grpc` / `unary-grpc-tls` 两档：那条负载发的是
`requests/grpc-sum.bin`（9 个字节 `00 00 00 00 04 08 01 10 02`），服务端要解两个
`int32`、算个和、把结果编回一帧，再按 gRPC 的形状发出去。整条路上**只有两样东西是
协议事实、不随调用方变化** —— 5 字节长度前缀帧与 protobuf 的 varint —— 剩下的都是
应用逻辑。本模块收的正是那两样。

用法与边界见 [`doc/grpc-guide.md`](doc/grpc-guide.md)。

- **`src/grpc/` 是一个模块，而且按依赖面切成两半**（`1.6.10-dev`）。
  `uvcpp_grpc.{h,cpp}` 是编解码：帧（1 字节压缩标志 + 4 字节**大端**长度）、varint、
  以及"只编解 wire type 0 字段 + 跳过别的 wire type"的字段读写。它的每一个参数都是
  `std::string` / `uint32_t` / `const char*`，**不认识连接、不认识响应**，所以
  `UVCPP_BUILD_WEB=OFF` 时照编照装。`uvcpp_grpc_server.{h,cpp}` 把上面那半接到
  `uvcpp_http_server` 上，它的每一个参数都是 web 层的类型，web 关掉时**从构建与安装
  两处都摘掉**（`CMakeLists.txt:2020-2022`、`CMakeLists.txt:3010-3015`）——
  实测：`-DUVCPP_BUILD_WEB=OFF` 的树上 `src/grpc/uvcpp_grpc.cpp` 照编、装到前缀里的
  只有 `include/grpc/uvcpp_grpc.h`；`build-capi-all` 上前缀里两个头都在。
  **不加 `UVCPP_ENABLE_GRPC`**：两半的依赖是死的，多一个旋钮只会多一个能与
  `UVCPP_BUILD_WEB` 打架的开关
- **一条 unary 应答的三段顺序由 `grpc_send_unary()` 一个函数包办**（`1.6.10-dev`）。
  `HEADERS(200 + content-type)` → `DATA(帧)` → `HEADERS(grpc-status)`。第二段的
  `write_stream()` 返回 0 只说明"已入队"，**那批字节真的写完了**的唯一信号是它的
  `done` 回调，所以尾随头必须在 `done` 里发 —— 早一步就是"END_STREAM 在前、字节在
  后"的协议违例，而对端只会表现为解包失败或超时。把这套顺序留给调用方，等于让每个
  写 gRPC 路由的人各踩一次同一个坑、且**踩错时没有任何本地症状**。失败那条更短：
  `deferred = true` → `begin_stream()` → `send_trailers()`，两句背靠背 —— 中间没有
  数据在飞，`send_trailers()` 那条"DATA 队列空不空"的前置条件当场成立，而
  `submit_headers()` 是**同步**把会话置成 `HEADERS_SENT` 的，所以不会撞上
  `UV_EALREADY`（gRPC 里这叫 Trailers-Only）
- **编解码的判据是逐字节的手算字面量，而且每个 fixture 都要先过一道"装置自检"**
  （`1.6.10-dev`）。`tests/functional/grpc_codec_func.cpp` 八轮（帧前缀大端、流式
  `consumed`、上限、varint 表与四类拒绝、字段编解、last-wins、跳过 wire type 1/2/5、
  拒绝 field 0 与 wire type 3），名字不匹配任何一条过滤正则 ⇒ **任何配置下都编、都
  跑**。`tests/functional/web_h2_grpc_func.cpp` 四轮走真 socket + 真路由（名字同时命中
  `web_.*` 与 `/[^/]*h2_[^/]*` ⇒ web 或 nghttp2 关掉时**整个文件不参与构建**，"没测"
  表现为"用例不存在"而不是一个假装通过的 SKIP）。**这份用例第一版是红的**，而红的
  原因值得记下来：手写的请求体字面量在三处各少了一个 `\x00`（5 字节前缀的 4 个长度
  字节写成了 3 个），长度于是从 4 变成 1032，服务端报"载荷还没收全"、照样回
  `INVALID_ARGUMENT(3)` —— **第 3 轮那四条判据全"通过"了，一个字节都没钉住**。
  现在是每个 fixture 先自检"它是不是我以为的那份字节"（`fixture_is_sum_frame()` 让
  **解码器**当权威，`tests/functional/web_h2_grpc_func.cpp:350-373`），响应侧则让手算
  的十六进制字面量与"`decode_exact` 解得开"互为独立的两道口径。第二处错同一类：
  第二组请求体里 `a0 8c 06` 应为 `a0 8d 06`（100000 的第二个 varint 字节是 13 不是 12），
  而"解得开一帧"的自检**照样绿** —— 因为那是一帧完全合法的、只是数不对的字节。
  自检于是改成连**值**一起断言（`1.6.10-dev`）
- **回归**（`1.6.10-dev`）：`build-capi-h2`（WEB + NGHTTP2）116/116；
  `build-capi-all`（CAPI + HTTP3 + WSDL 关，全功能）129/129，两条 DB 用例按惯例
  skip；`-DUVCPP_BUILD_WEB=OFF` 的树编过，且 `test_web_h2_grpc_func` 不在其中。
  门禁：`check_doc_versions` / `check_docs` / `check_doc_lines`（1085 条引用）/
  `check_config_contract`（对着新打的包）/ `check_ci_layout` / `check_capi_symbols`
  全过；`check_doc_snippets` 拿 `build-all` 的包（WSDL 开）编过 103/103 条
  —— 拿 `build-capi-all` 的包跑会退 **3（14 条 WSDL 片段没判）**，那**不是绿**

- **gRPC 也能从 `uvcpp_web_app` 用**（`1.6.11-dev`）。前面那三条
  （`grpc_send_unary` / `grpc_send_status` / `grpc_parse_unary_body(uvcpp_buf)`）都要
  `uvcpp_http_server&` + `uvcpp_tcp_client*`，而框架的路由签名里两样都没有 —— 它拿到的
  是 `uvcpp_web_request&` / `uvcpp_web_response&`，发送者是框架。于是补三条
  （`#if UVCPP_WEBAPP_ENABLE`，同文件第二个开关；webapp 默认关、"web 开 webapp 关"是
  **默认**配置，CI 的 `http3` 那一格就是）：
  `grpc_web_send_unary()` / `grpc_web_send_status()` / `grpc_parse_unary_body(std::string)`。
  两个发送版本的差别只有一处、且被框架结构定死：**谁排三段的顺序**。底层版自己
  `begin_stream → write_stream →（在 done 里）send_trailers`，返回 0 是"已入队"；web 版
  只**声明**形状（status / content-type / body / `set_trailers()`），顺序由
  `uvcpp_web_app::send_response()` 包办 —— 那是整个框架里唯一同时看得见 `write_stream()`
  的 `done` 与 `send_trailers()` 的地方，返回 0 是"已声明"。为此给
  `uvcpp_web_response` 补了文档化的尾随头入口 `set_trailers()` / `has_trailers()` /
  `trailers()`（`src/webapp/uvcpp_web_response.h:523-527`），并在 `send_response()` 里加了
  三段那条分支（`src/webapp/uvcpp_web_app.cpp:2753-2802`）。
  `std::string` 那个重载不是顺手加的：`uvcpp_web_request::take_from()` 把 body **搬**进了
  自己的 `body_`，源请求的 `src_.body` 那之后就空了，所以 `req.raw().body` **不是**
  "另一条拿到同一份字节的路"。判据与量法见下一条
- **尾随头的三条前提各有一个失败模式，三条都有用例**（`1.6.11-dev`）。
  `tests/functional/web_app_h2_grpc_func.cpp` 四轮，走真 socket + 真路由（文件名同时命中
  `web_app_.*` 与 `/[^/]*h2_[^/]*` ⇒ webapp 或 nghttp2 关掉时**整个文件不参与构建**）：
  **① 三段**（首部块里**没有** `grpc-status`、DATA 逐字节是手算的 `00 00 00 00 02 08 03`、
  `on_response_end` 那一刻的头列表里有 `grpc-status: 0`）；**② Trailers-Only**（空请求体与
  截断的帧 → 体为空 + `grpc-status: 3`，另外把 `中文` → `%E4%B8%AD%E6%96%87` 这条编码
  钉在一条**自己给的字面量**上，与解码器的措辞无关）；**③ 流式**上被丢弃（`end_stream`
  那一刻也没有 `grpc-status`）；**④ h1** 上被丢弃（`content-length: 7`、体照发、
  报文里搜不到 `grpc-status`）。变异：把 `set_trailers()` 那一句删掉 ⇒ ① 的前两条照样绿、
  ③④ 照样绿，**只有第 3 条断言红** —— 那正是"尾随头真的到了"的唯一证据
- **写这条用例时逮到库里的一个真缺陷：`content-length` 会漏出去**（`1.6.11-dev`）。
  第一版断言"处理函数设过的头一个都别上线"，红了：`grpc_web_send_*` 当时只管设
  status/content-type/body，**没动头表**。查下去分成两类，处置不同：
  `uvcpp_web_response::sync_meta()` 的规则是"**已存在** `Content-Length` 或
  `Transfer-Encoding: chunked` 时不动它"（对普通响应是对的），所以处理函数先设的
  `content-length: 999` 会**留着** —— 而这条应答只有 7 个字节，对端会一直等剩下 992 个。
  这是**线上错帧**，不是"头表里多个字段"。于是两个发送函数摘掉
  `content-length` / `transfer-encoding` / `content-encoding` 三个，**其余原地保留**
  （它们就是 gRPC 的 initial metadata，服务端捎带应答元数据是规范内的用法）。
  变异复验：把摘 `content-length` 那一句去掉 ⇒ 7 条断言红，其中两条是**整条往返收不完**
  （客户端卡在那 992 个字节上）—— 错帧的后果当场看得见
- **回归**（`1.6.11-dev`）：`build-capi-all`（WEBAPP=ON）上**串行**全量
  `ctest --output-on-failure` **130 条全过、0 failed**（`100% tests passed`，
  149.64 s；两条 DB 用例 `test_db_mysql_func` / `test_db_pgsql_func` 因本机没起
  服务而 Skip，是既有的条件跳过而不是失败。**必须串行**：并行 `-j` 下
  `test_tcp_two_buf_write_func` 会偶发红、单跑两遍都绿，是并行执行的 flake，
  与本节无关）；同树上 `ctest -R
  "grpc|web_app|web_h2|web_stream|web_static|h2_"` 39/39；`build-h3`（**WEB=ON 且
  WEBAPP=OFF**，照 CI 的 `http3` 那一格配的）编过，且该树里 `nm -DC libuvcpp.so` 只导出
  `grpc_send_unary` / `grpc_send_status` 两条、两条 `grpc_web_send_*` **一个符号都没有**，
  `ctest -N` 里也不注册新用例；`build-capi-all` 上四条符号都在、用例注册为 #66
- **门禁**（`1.6.11-dev`）：`check_doc_versions` / `check_docs` / `check_ci_layout` /
  `check_capi_symbols`（8 片判过、`undeclared` 那 1 片照旧「未判」）EXIT=0；
  `check_doc_lines` 5 条判据、1137 条引用全过（其中 **27 条跟着挪** ——
  `grpc_server.h/.cpp` 中间插了内容、`web_app.cpp` 加了 67 行，`doc_line_refs.lock`
  重刷了两次：第一次那两条 `[胀]`/`[歧]` 要先把**文档里**写的行号改对，第二次才把
  两条「平移后被顶掉」的键补回去）；`check_config_contract` 与 `check_doc_snippets`
  都拿 `package_release` 现打的 `1.6.11` 包跑（后者的 7 条 QUIC/HTTP3 片段按
  `DEFAULT_OFF` 跳过，不算前提不满足 —— 拿缺 WSDL 的包跑会退 **3**，那不是绿）；
  `verify_tree`（`build-capi-all`）的 build/mtime 两道 PASS，dll 那道 FAIL 是
  `copy_test_dlls` 这个 target 建在 `CMakeLists.txt` 的 `if(WIN32)` 里、Linux 上恒
  不存在（既有）。这一节改过 `uvcpp_version.h`，**构建树必须重跑一次 `cmake .`**：
  `test_capi_common_func` 就是那条「版本号动过、构建树没重配」的探测器，
  它红了正说明探测器是好的

### 数据库（`src/db/`）

一个连接一个 `uvcpp_db_client`，**同一套接口盖住 SQLite / MySQL / PostgreSQL /
ODBC** 四个后端，结果行按表取（列名或列号）。**默认关**（`UVCPP_ENABLE_DB`），而且是
本库**唯一需要第三方客户端库**的模块（libsqlite3 / libmysqlclient / libpq / 驱动
管理器 + 具体 ODBC 驱动）—— 四个后端一个都没找到时整个模块被强制关掉并打一条说明
怎么修的 warning，而不是留一个"能配置、链不上"的组合（`1.5.3`）

- **PostgreSQL 的异步查询走真异步：查询挂在调用方自己的循环上，不占线程**
  （`1.7.2-dev`）。`uvcpp_db_pool` 的异步接口（`query(loop, …)` / `execute(loop, …)`）
  此前只有一种实现：`uv_queue_work` 把一次阻塞查询丢进 libuv 线程池，**每请求两次
  跨线程交接**。现在 PG 这条腿改用驱动自己带的非阻塞协议 —— `PQsendQueryParams` +
  `PQflush`，`uv_poll` 盯 `PQsocket()`，`PQconsumeInput` / `PQisBusy` / `PQgetResult`
  收结果 —— 回调的形状、线程、返回码**一个都没变**（入口那一侧一行没改），变的只是
  等待发生在哪儿。`uvcpp_db_client::query_async()` 是这条路的公开入口；池子默认用它，
  `set_native_async(false)` 可以按回线程池。MySQL / SQLite / ODBC 没有可用的异步协议，
  照旧走线程池；`insert` 也不在这条路上（要"执行 + 取 `lastval()`"两次往返）。

  读数（仓外量具，8 条循环 × 128 条在途、256 条连接，三次交替 A/B、中间 20 s 冷却）：
  **每请求 CPU 中位 65.69 → 42.56 µs（−35%），最好的一对 −44%**；取锁 9.27 次/请求。
  同并发（8 条在飞）下真异步只比"阻塞在循环里"多 6.6 µs，多出来的 12.8 µs 是
  "在途从 8 条涨到 1024 条"买来的（而 rps 没涨）。逐项账（3 次 `epoll_ctl`、其中 2 次
  是 libuv `uv_poll_init` 的探针、+8.8 次 malloc、+5 次取锁）、坑与复现写在
  `doc/benchmark.md` §4.24。

  **一条必须写进契约的约束**：真异步这条路上查询的 I/O 由那条循环驱动，所以**别带着
  在飞的查询 `loop.stop()`**（或在回调都还没到时就析构循环）—— 那几条永远不会落地，
  池子析构会一直等 `work_in_flight` 归零，症状是进程退不出去。线程池那条路没有这个
  问题。约定写在 `src/db/uvcpp_db_pool.h` 的类注释里，用例
  `tests/functional/db_pgsql_async_func.cpp` 钉住正常收尾那一侧。

  顺带把 `async-database` 那档读法的合规歧义做实了：它的 `standard` 规则原文是
  "Must use an async PostgreSQL driver with standard connection pooling"——现在这条腿
  是真的 async driver，不再是"把阻塞 libpq 包一层"。

- **ODBC 后端：它是第四个后端，但与前三个不同类 —— 它对着的不是一种数据库，是一个
  接口**（`1.7.1-dev`）。前三个各自钉死一种客户端库与一种方言，`odbc://` 连到哪张
  库、哪种方言、甚至哪个厂商，全由连接串里的 `Driver=`（或 `DSN=`）决定；本驱动
  **只试那一个驱动名，不遍历**。进它的直接原因是 SQL Server：微软没给 SQL Server
  一个可链接的 C 客户端库，官方 `msodbcsql18` **本身就是**一个 ODBC 驱动，想接它就
  只能走这条路（这也是本仓第一个不是"为了某一种库"而写的后端）。

  于是有两条**必须分开看**的事实：`UVCPP_DB_ODBC_ENABLE` 说的是"这份产物里有没有这个
  后端"，而"要连的那个数据源有没有装驱动"**只有 `open()` 才知道** —— 驱动名写错时
  拿到的是驱动管理器给的原文（"找不到数据源、也没有默认驱动"），不是我们编的话。
  配置期只要两样：`sql.h` 与驱动管理器（Linux 的 `libodbc`、Windows 的系统 `odbc32`）。
  **它也认 `UVCPP_DB_RUNTIME_LOAD`**：起先这里写的是"驱动管理器本来就该在、而且很小，
  所以不走那条路"，那句话**只在 Windows 上成立** —— 下一节第一条就是按实测改它的
  （本机 `-- db: ODBC 后端开（驱动管理器 …/libodbc.so）`，编、链 rc=0）。

  写这一片时踩到一处值得记下来的头文件陷阱：unixODBC 的 `sqltypes.h` 第 75 行有一句
  `#define BOOL int`（给没有 `<windows.h>` 的平台补齐 ODBC 那套类型表），它会把
  `src/db/uvcpp_db_odbc.cpp` 里唯一一处 `uvcpp_db_type::BOOL` 展开成
  `uvcpp_db_type::int`，报出来的错是"expected unqualified-id before 'int'" —— 离真正
  的原因隔着一层。收尾是一句 `#undef BOOL`（ODBC 头已经预处理完，这个宏使命结束，
  本文件从头到尾不用它）。微软那套 `sqltypes.h` 不发这个宏（Windows 上 `BOOL` 是
  `<windows.h>` 里的 typedef，对 `enum class` 的限定名没影响），所以那行在 Windows 上
  是空操作。

- **ODBC 后端进了共用套件，套件立刻抓出它自己的六处**（`1.7.1-dev`）。用例在
  `tests/functional/db_odbc_func.cpp`：方言按 `Driver=` 认出来的驱动族分派
  （`odbc_sqlite_dialect` / `odbc_mssql_dialect`），认不出的族退 **3**。本机对着
  sqliteodbc + unixODBC 跑：**181 通过 / 0 失败 / 3 不适用**，另有 **57 条 ODBC
  专属**（日志里单独一段）。这一组数全是**量出来的**，不是照着文档推的：

  1. `SQLBindParameter` 只**记指针**（缓冲**和** `StrLen_or_IndPtr` 都记），绑定的
     存储必须活到 `SQLExecute` 之后，而且**每个参数各一份**。共用一份时，四个参数
     `(1, 2, 't0', 'uu0')` 存进去读回来是 `(2, 2, 't', 'u')` —— 每个都只剩最后一次
     写入的一个字节；
  2. `SQL_C_WCHAR` 的长度指示器单位是**字节**：给字符数会把 `"张三"` 截成 `"张"`、
     `'abcdef'` 截成 `'abc'`；
  3. 空串与 NULL 是靠**指针**分的：有效指针 + `ind=0` 是空串，`nullptr` + `ind=0` 是
     NULL。空串上 `std::string::data()` 给不给非空指针是实现自由，所以代码里备了两个
     哑元（`kEmptyByte` / `kEmptyWide`）——这条错了的话，写空串会**响**在非空约束上，
     而不是静默存成 NULL；
  4. 分块读时一个**满**块只给 `BufferUnits - 1` 个数据单元（驱动给 NUL 留位），按整块
     计数会把 3000 字节读成 3001 字节；
  5. `SQLGetInfo` 的 W 形态返回的长度也**是字节**：拿它当单元数会让引号字符 `"` 变成
     2 个"字符"，于是"内层引号双写"那一支被跳过，怪表名 `uvcpp_odd"name` 被拼成
     `"<NUL>uvcpp_odd"name"<NUL>`。改成认尾部的 NUL，不看那个长度；
  6. 无参数那条快路径绕过了 `SQLNumParams` 检查：语句里写了 `?` 却一个参数都不给时
     报的是 `EXEC_FAILED`，而这是**绑定**阶段的错，该报 `BIND_FAILED`。

  另有两处是**驱动的自由**，不是我们的 bug —— 套件改成"按声明的能力判"，不硬凑断言：
  sqliteodbc 的 `SQLPrepare` 只**登记**不解析（语法错与表不存在要到 `SQLExecute` 才
  报），因此 `db_suite.h` 多了一个 `dialect::prepares_eagerly()`；它同样**不接受**
  `$1` 占位符（`parameter marker count incorrect`），所以 ODBC→SQLite 的
  `accepts_dollar_placeholder()` 是 `false`（原来是照"ODBC 应该有"猜的 `true`）。
  这两条改的是共用套件，所以 SQLite 那一家按同一份套件重跑过：**196 通过 / 0 失败 /
  1 不适用**，改动没有扰动别的后端。

  顺手把两条套件原本没走到的路也**量了**并钉成断言（都过）：二进制跨块
  （3 KB BLOB，逐字节相等 —— `SQL_C_BINARY` 没有终止符，满块给满 `kChunk`），以及
  4 字节 UTF-8 字符（UTF-16 是**代理对**）正好骑在块边界上（写路径按代理对编码，
  驱动不会在缓冲边界把它劈开）。

  **没跑的是 ODBC 到 SQL Server 那一族**：`odbc_mssql_dialect` 写好了（`INT
  IDENTITY(1,1) PRIMARY KEY` / `NVARCHAR(MAX)` / `VARBINARY(MAX)` / `DATETIME2` /
  `BIT` / `DECIMAL(18,2)`），但这台机器上没装 msodbcsql18。别把它当成已经对过。

- **ODBC 也接上了 `UVCPP_DB_RUNTIME_LOAD`：它的"客户端库"就是驱动管理器**
  （`1.7.1-dev`，默认 `OFF`）。修的是上面那条里的一句判断：把驱动管理器链上，在
  **Linux / macOS 上就是一条 `DT_NEEDED libodbc.so.2`** —— 正是这个开关要消灭的形状
  （"没装 unixODBC 的机器连加载这份 `.so` 都做不到"）。**只有 Windows 是例外**：
  `odbc32.dll` 属于操作系统，和 `ws2_32` 同类，链接它没有代价。本机两档实测 ——
  两棵树（`/tmp/uvcpp-off` 与 `/tmp/uvcpp-rt`）**只差这一个开关**，其余开关逐条相同：

  ```
  $ readelf -d /tmp/uvcpp-off/libuvcpp.so | grep NEEDED   # 关（默认）
    NEEDED               libuv.so.1
    NEEDED               libsqlite3.so.0
    NEEDED               libodbc.so.2
  $ readelf -d /tmp/uvcpp-rt/libuvcpp.so | grep NEEDED    # -DUVCPP_DB_RUNTIME_LOAD=ON
    NEEDED               libuv.so.1
    NEEDED               libsqlite3.so.0
  $ nm -u …/uvcpp_db_odbc.cpp.o | grep -c ' U SQL'        # 24 → 0
  ```

  那张函数表是**从 `nm -u` 的目标文件里数出来的 24 个入口**，不是照着 grep 抄的：
  `SQLGetInfo` 在文件里只出现在一句注释里（用的是 W 形态 `SQLGetInfoW`），照着文本
  抄会把一个没人提供的符号抄进表里。新私有头 `src/db/uvcpp_db_odbc_api.h` 与
  `uvcpp_db_pgsql_api.h` 同形（进安装排除与 `package_release.py` 的
  `PRIVATE_HEADERS`），64 处调用点改成 `api().SQLxxx(...)`，`open()` 先看
  `uvcpp_db_odbc_load_error()`，非空就报 `NO_DRIVER` 并把候选名与各自的失败原因写在
  里面。**`#error` 那道闸门这次是必需的**：`ODBC_STD` 会把 `SQLAllocHandle` 重定向到
  `SQLAllocHandleStd`、`UNICODE` 会把 `SQLSetStmtAttr` 重定向到 `SQLSetStmtAttrW`
  （两处都在头文件里的条件宏里），按名字取符号的构建方式与直接链的会**分叉**，所以
  编这个 `.cpp` 时两者都必须未定义，否则当场红。负路径也量了：
  `UVCPP_DB_ODBC_LIBRARY=/nonexistent/libodbc.so.2` ⇒ `no_driver` 加那句逐个列出试过
  哪些库名的话。两档各跑一遍套件，读数一致：ODBC **181 / 0 / 3**（另 57 条 ODBC
  专属）、SQLite **196 / 0 / 1**。默认 `OFF` 的行为逐字节不变。

- **"这台机器没装驱动管理器"从一句手工复现的话，变成一个有用例钉住的形状**
  （`1.7.2-dev`）。上面那条里的负路径（`UVCPP_DB_ODBC_LIBRARY=/nonexistent/…` ⇒
  `no_driver`）此前**没有任何自动化**：`tests/`、`bench/` 里三个 `UVCPP_DB_*_LIBRARY`
  旋钮全零命中，它只活在文档里当一段"你可以这样试"。新用例
  `tests/functional/db_odbc_nodrv_func.cpp` 把用户那句要求（"没有 ODBC 动态库时其他
  功能正常运行、用的时候报异常"）逐条钉住：`open("odbc://u:p@127.0.0.1:1/…")` 回的是
  **`NO_DRIVER`**（不是含糊的 `OPEN_FAILED`、更不是崩），`is_open()` 为假，`last_error()`
  是一句说得出**试过哪几个库名**的人话，同一进程里再开一个 `sqlite::memory:` 照常
  CREATE/INSERT/SELECT。

  前提不去卸载任何东西 —— 由 ctest 的 `ENVIRONMENT` 属性把那个旋钮指到一个不存在的
  路径（这正是 `uvcpp_db_runtime_load.h` 里写明这个旋钮的用途）。**它必须是独立的
  可执行文件**：ODBC 的符号表走 magic static、句柄永不释放，同一个进程里成功加载过
  一次之后，再设环境变量就不生效了。前提不成立（变量没传到、或路径居然存在）时返回
  **非零即红**，不是跳过 —— 这个前提是我们自己设的，没有不确定性可言，退回跳过只会
  让这条用例悄悄失效。

  顺手修掉一处同门的老毛病：`test_db_odbc_func` 原来不在 `SKIP_RETURN_CODE 3` 那一串
  里，于是在装了 `unixodbc-dev` 却没配数据源的机器上（CI 就是）它显示成 `Failed`，
  而真相是"未判定"。判据现在归位。

- **两个 ODBC 新文件补上 UTF-8 BOM**（`1.7.2-dev`）。`src/db/uvcpp_db_odbc_api.h`
  里全是中文注释却没带 BOM，而 `src/db/` 下 24 个文件里只有它与
  `uvcpp_db_odbc.cpp` 没带（前两个后端各自的 `*_api.h` 都带）。这条不是风格问题：
  MSVC 读无 BOM 的源文件时按系统 ANSI 代码页（简中 936/GBK）解码，一个三字节汉字被
  当成两个双字节字符、字节边界整体错位，该在注释里的 `*` 变成运算符、`*/` 落到注释
  外，于是其后每个头都在"注释外"被解析、一路级联到 `<algorithm>`，**报错地点与病因
  完全无关**。仓内看不见，因为顶层 `CMakeLists.txt` 的 `/utf-8` 是**目录作用域**的 ——
  它进不了导出集，也到不了"直接把源码树给 msvc"那条消费路径。

  抓到它的是 `config-contract` 腿的 `check_header_encoding()`（判的是 `src/` 下每一个
  `.h`，不只安装集里那些）。它本来就该在分支自己的 CI 上红过：`feature/db-odbc` 没有
  对着 master 开过 PR，而这几个文件是随 `1.7.1-dev` 那几个提交进来的。`package_release.py`
  里那段"含非 ASCII 又没 BOM 就补一个"的兜底**救不了**它 —— 私有头不进包。

- **MySQL / PostgreSQL 的客户端库改成可以运行时加载：`UVCPP_DB_RUNTIME_LOAD`**
  （`1.6.54-dev`，默认 `OFF`）。直接链的后果不是"少个功能"而是**进程起不来**：
  `DT_NEEDED`（Windows 上是 PE 的导入表）是**加载期**解析的，产物上多一条
  `libmysqlclient.so.21` / `libpq.so.5`，没装过那两种客户端库的机器连 `libuvcpp.so`
  都**加载不了** —— 哪怕这个程序只跑 HTTP、一行 `db` 代码都不碰。本机实测（同一个
  只 `printf` 的 `hello`，装好的前缀形态）：

  ```
  $ ldd libuvcpp.so | grep 'not found'
          libmysqlclient.so.21 => not found
          libpq.so.5 => not found
  $ ./hello
  ./hello: error while loading shared libraries: libmysqlclient.so.21:
           cannot open shared object file: No such file or directory      # rc=127
  ```

  开了之后符号由 `dlopen("libmysqlclient.so.21" / "libpq.so.5", RTLD_NOW |
  RTLD_LOCAL)`（Windows 上是 `LoadLibraryA`）取，产物上**一条 `DT_NEEDED` 都不多**，
  而没装库的机器上除"真去连 MySQL/PG"之外一切照常：那种连接在 `open()` 拿到
  `NO_DRIVER`，`last_error()` 里逐个列出**试过哪个库名、各自什么原因**，并给出两条
  出路（装一个客户端库，或 `UVCPP_DB_MYSQL_LIBRARY` / `UVCPP_DB_PGSQL_LIBRARY`
  指路 —— 设了它就**只试那一个**，这也正是"没装库"那条路的确定性旋钮）。

  **`--no-undefined` 是这个开关的必要配套，不是洁癖**：调用点全改成经一张 `dlsym`
  填出来的函数指针表走，而**共享库默认允许未定义符号** —— 原本以为"漏改一处调用点
  ⇒ 链接期 `undefined reference`"能当判据，**实测是错的**：`ld` 不吭声，产物带着一个
  没人提供的 `mysql_query`，于是**每个使用者**在加载这个 `.so` 时炸，正是这个开关要
  消灭的形状。所以这一档下给 ELF 的共享目标补 `LINKER:--no-undefined`，漏一处就红
  （变异实测：`undefined reference to 'mysql_query'`，`make` rc=2）。

  它改的只是**符号从哪来**，不是**有哪些后端**：配置期仍旧要 `mysql.h` /
  `libpq-fe.h`（`MYSQL_BIND` / `PQresult` 这些类型的布局必须从真的头里读），
  `UVCPP_DB_MYSQL_ENABLE` / `UVCPP_DB_PGSQL_ENABLE` 的含义照旧是"头找到了"，
  `uvcpp_db_drivers()` 的输出也不变 —— 于是 `NO_DRIVER` 有**两条**来路（后端没编
  进来 / 编进来了但这台机器没库），`uvcpp_db_status.h` 里那一段文档注释把两条分开
  写清了。三个新私有头（`uvcpp_db_{mysql,pgsql}_api.h`、`uvcpp_db_runtime_load.h`）
  同时进了 `CMakeLists.txt` 的安装排除与 `package_release.py` 的 `PRIVATE_HEADERS`。
  默认 `OFF`，今天的行为逐字节不变（实测：`DT_NEEDED` 两条照旧、`nm -D -u` 里那
  54 个 `mysql_*`/`PQ*` 照旧、`dlopen`/`dlsym` 引用数 0）
- **占位符是方言差异，本模块不换算。** 写 `?` 还是 `$1` 由你选的那个后端决定，
  原样透传给驱动。六种组合（三后端 × 两种写法）逐个实测：
  PG `?`→`prepare_failed`、PG `$1`→ok、MySQL `?`→ok、MySQL `$1`→`prepare_failed`、
  SQLite `?`→ok、**SQLite `$1`→ok（当命名参数收下）** —— 最后那格是坑：只在 SQLite
  上跑绿的 SQL 换到 PG 上就红，而共用套件一律走方言自己的写法，永远写不出错的那种。
  所以加了一条**反向**判据（套件里唯一一条）：另一种写法**必须**在准备阶段失败，
  除非方言声明它收 `$n`（SQLite 那格是量出来的、不是设计）。两处变异各验一次都红。
  不做换算的理由是两边都换不对：PostgreSQL 里 `?` 本身就是合法操作符（jsonb /
  hstore 的"键存在"），也出现在字符串字面量与带引号的标识符里，盲扫会把**合法**的
  SQL 悄悄改成另一句；反方向 SQLite 又收 `$1`。宁可让写错的当场拿到
  `PREPARE_FAILED`，也不替他猜（`1.5.3`）
- **事务里不重试。** 失败后的重试会把语句挪出那个事务本身，所以门面不替你重试
- **`DECIMAL` 有代价**：走字符串进出，不经过浮点 —— 它既到不了浮点也到不了整型，
  是一个"格式化的十进制串"（`1.5.3`）
- **异步门面 `uvcpp_db_async` 不写 `uv_queue_work`。** 绑一条连接但不持有它，
  两种形状：回调式 `a.query(&loop, sql, {params}, cb)`（cb 在循环线程上被调，
  这是 `uv_queue_work` 的 after-work 给的保证，所以**不需要** `uv_async` 邮箱）
  与 future 糖 `a.query(sql).get()`（给不在循环上的调用方，用一条**懒起的私有循环
  线程**，N 个 future ⇒ N 条池线程 ⇒ 真并发）。三条承重语义落在用例里：投递失败
  ⇒ 返回负值、回调不来；在回调里 `delete` 门面是合法的（析构第一件事断存活凭证）；
  析构时先等**私有循环上**的在途活归零再停线程 —— 循环一停那些 after-work 就永远
  发不出来，`get()` 会永久挂住。回调式的活**不等**（它们投在调用方自己的循环上）。
  判据：主线程跑循环、另一个 `std::thread` 提交，回调里比对
  `std::this_thread::get_id()`，并断言 `query()` 返回后 `calls == 0` ——
  后半条抓的是"实现其实是同步跑完再回调"，光比线程 id 抓不到它（`1.5.3`）
- **这一层要交付的是"循环不被卡住"，不是"查询更快"。** 量具（10 条 50 ms 慢查询
  挂在 loop 线程上）给出的两个数必须一起读：1 ms 定时器的相邻到点间隔从 **504 ms**
  回到 **2.1 ms**，而**两臂的总耗时一模一样** —— 一个 client 一个连接，活再异步
  也还是串行（`1.5.3`）
- **连接池把并发从 1 抬到 N。** `uvcpp_db_pool` 管 N 条连接、门面管"在哪条线程上
  阻塞"，两者不是替代关系。判据是**同量级**而不是"夹在中间"：4 线程 × 500 次点查，
  池子（max=4）相对"4 条线程各开 1 个 client"是 MySQL 0.97×、PG 1.11×、SQLite
  0.85× —— 池子买的正是那份并行度，多出来的只有一次借还。SQLite 上略快于"各开一条"
  是正常的：那里唯一的开销就是线程调度，而池子省掉了另外 3 条连接的 open。
  `created_total()` **停在 max（4）上而不是 2000**，这是连接被**复用**的见证。
  另一条判据是"池线程不会互相等死"：max=1 时借走唯一一条、再投一个异步池查询，
  那条池线程必须在**借出超时**后醒来拿到 `NO_CONNECTION`，而不是挂住 —— 没有这层
  防线，被借空的池子会把 4 条池线程连同 `uv_fs_*` / DNS 一起饿死（`1.5.3`）
- **池子先走一步不能崩**：投出异步活之后立刻 `delete` 池子，析构等 work 相收完
  （存活凭证 + 在飞计数），回调不被调、进程不崩。三后端共用套件（`db_suite.h`）
  加一节 `test_pool`，远程后端也吃到（`1.5.3`）

### 横向扩展（多事件循环）

- **链缓存的"首次构建"从请求路径上搬走 —— `n > 1` 下它是框架自己踩的一处数据竞争**
  （`1.6.14-dev`）。链缓存（`chain_cache_` 是 `std::map`、`chain_storage_` 是
  `std::deque`）原本是**惰性**建的：第一个请求派发到哪个路由，就在那条**工作循环的
  线程**上把那条链建出来、落进这两张**全进程共享**的表。`n == 1` 时那条线程就是唯一
  的线程，没问题；`n > 1` 时"第一个请求"落在哪条循环不定，而其余循环马上也会各收到
  请求、各自走进同一段**写**代码。
  **这是框架违反自己的成文契约**（`src/webapp/uvcpp_web_app.h`「四、注册路由要在
  `start()` 之前」）—— 用户完全照契约办事，框架自己在放行之后写共享表。
  现场：`set_loops(8)` + h2c 负载，受害者 `uvcpp_web_context::advance()` 里的 `chain_`
  指到 `chain_storage_` 的**尾后**一格（core 里 `chain_storage_.size() == 7`、`chain_
  == &chain_storage_[7]`），于是在一条越界的 `std::function` 上调用，跳到内存里的垃圾值
  （`0x100000390`）SIGSEGV。**改前 12 轮里 11 轮当场死、其余轮次 2xx 全是 0。**
  修法三段：① 路由器补一个 `for_each_handler()` 观测口（交出的引用与 `match()` 结果里
  的 `handler` 是同一个对象，链缓存正是按那个地址索引的）；② `warm_chain_cache()` 在
  `init_process_once()` 里、**放行工作循环那一句之前**把三条特殊链与两张路由表的每条
  链建完，随即冻结（`chains_frozen_`）；③ 冻结之后**框架自己一个字节都不再写**那张表
  —— 检测到路由表/中间件变了就报一条 ERROR 并把**整张缓存判死**
  （`chain_cache_stale_`：那些键指向路由表内部元素，表被改过之后要么悬垂、要么被新
  处理器复用了地址），此后每条请求的链**现建现用**（`build_chain_copy()` +
  `uvcpp_web_context::run_owned()`）。**降级要答对**：注册 `/late` 会让
  `router_table::routes` 重新分配，最初那版"整表回 404"的设计连**已经建好链的路由**
  一起废掉 —— 判据 9(b) 里"照旧答对"两条把它抓住（变异 M11）。
  **`n == 1` 那条路一个字都没动**（惰性构建本来就没有竞争可防，而预建会让"从没被请求
  过"的路由也永久占一份链）。
  判据分三层，都是「拆掉那处守卫就该红」的对照，不是「跑了 N 遍没崩」：
  **① 确定性那一半** —— `start()` 之后注册路由 ⇒ 日志里恰好一条 ERROR、而整张表照旧
  答对（含 `n == 1` 对照组）。变异 `M10`（拆掉冻结）与 `M11`（退回"整表回 404"）各自
  把这一半的三条判据打红，见 [`tests/tools/multiloop_mutation.py`](tests/tools/multiloop_mutation.py)。
  **② 竞争那一半的规模** —— 手工拆掉 `warm_chain_cache()` 那句（= 修复前的形状）后
  单实例串行 200 次（`taskset -c 0-3`）：**200/200 红、一次没绿**，其中 105 次是上面
  那三条确定性判据、79 次是压力形状那条（`并发首派发全部答对` 实测错 3 / 24）、
  16 次直接 SIGSEGV，另有若干次 glibc 在 `free()` 里抓到堆损坏；不拆则 **200/200 全绿**。
  这条读数没进变异表（每个变异只跑一次、判红绿，概率红进去就是一份时红时绿的门禁），
  记在用例注释与变异脚本的文件头里。
  **③ 真实负载** —— h2c on 8082 × 12 轮、8080 明文前言分流 × 8 轮、h2 走 TLS × 8 轮，
  **全 0 崩溃**、每轮 43–66 万 2xx（改前同一条路子 12 轮里 11 轮当场死、其余轮次 2xx 全 0）。
  `doc/multiloop-design.md` 里那段"让这些写离开请求路径的是两件事叠起来"的论证已按本次
  落地改写。
- **第四端：UDP / QUIC 也扇出了，形状与上面三层不同**（`1.6.6-dev`）。
  `uvcpp_quic_server::set_reuseport()` + `uvcpp_http_server::set_loops(n)`：UDP 没有
  `accept()`，所以不是"转手 socket"，而是**每端点一个 socket、各带 `UV_UDP_REUSEPORT`
  抢同一个端口**，内核按 4 元组哈希分流。**平台给不出的地方（Windows 的 `uv_udp_bind`
  在建 socket 前就返 `ERROR_NOT_SUPPORTED`、macOS 返 `UV_ENOTSUP`、libuv < 1.50 没有
  这个标志）回落成单端点** —— 那就是"从不调用 `set_loops()`"的行为；`is_reuseport()`
  报的是「这次绑定真的带上标志了没有」，不是「调用方请求了没有」。
  判据：本机 h2load-h3 打同一条目，`h3.set_loops(1)` 88 738–91 433 req/s（0.89 核，
  **单核就是它的天花板**）→ `set_loops(8)` 388 024–437 385 req/s（5.62 核），每连接
  响应数 6 933–7 143 → 30 314–34 171；`n=1` 与改动前逐字节相同。A/B 与生成器侧
  共同瓶颈的读数在 [`doc/benchmark-rig.md`](doc/benchmark-rig.md)「扇出 A/B」。
- **这条判据一开始是错的，改的是用例不是脚本**（`1.6.6-dev`）：第一版变异表里
  「端点循环写死 1 条」与「无条件丢掉标志」两种坏实现**全绿活了下来**——因为两条用例的
  判据全部读的是库自己报的数（`quic_fanout()` / `is_reuseport()`），而它们的形状与
  "平台拒绝标志"**一模一样**。补了库外探针（`tests/functional/reuseport_probe.h`：裸
  `uv_loop_t`、同端口带标志绑两次）才分得开。变异表在
  [`tests/tools/quic_fanout_mutation.py`](tests/tools/quic_fanout_mutation.py)
- `uvcpp_tcp_server::set_loops(n)`：把接受的连接摊到 `n` 条事件循环上 —— 一条接受者
  加 `n-1` 条工作循环，每条一个**专用 `std::thread`**（不是 libuv 线程池的线程）。
  不调用它、或 `set_loops(1)`，与旧行为逐字节相同（`1.2.21`）
- socket 转手有自己的一套原语（`net/uvcpp_socket_handoff.h`）：Windows 走
  `WSADuplicateSocketW` + `WSASocketW`，其余走 `dup()`。**Windows 上这条腿压着一个
  已知的 libuv 缺陷** —— 开之前先读 [doc/net-guide.md](doc/net-guide.md) §4 与
  [RELEASE.md](./RELEASE.md)
- `set_handoff_forced(true)`（`1.3.15`）强制走「接受者 + 转手」，好让 POSIX 那条
  `dup()` 腿在 Linux 上也能被跑到（**测试口子**；生产别用）
- `set_loop_start_hook()`（`1.2.22`）与 `set_loop_exit_hook()`（`1.2.24`）在每条工作
  循环上跑，属主可以就地在上面建/拆句柄；启动钩子抛异常会翻成 `UV_ECANCELED`，
  而不是留下一个永远不兑现的承诺
- 连接 id 的高 20 位编码循环号，多循环下 id 仍唯一；`n == 1` 时这个编码是恒等变换，
  id 与旧版逐字节相同（`1.2.22`）
- `uvcpp_web_app::set_loops(n)`（`1.2.23`）把同一件事带到框架层，配套 `loop_count()`
  与 `connection_count_at()`。停机改成**逐槽位扇出** —— 此前那条路只投到 0 号循环，
  工作循环压根不进停机状态机、句柄没人删，`uv_loop_close` 于是撞 `UV_EBUSY`，
  整块循环内存泄漏
- `uvcpp_web_app::connections()` 现在返回**本循环**那一份登记表，`connection_count()`
  变成**所有循环求和**（`1.2.23`）—— 逐循环请用 `connection_count_at(int)`。同名同
  签名：旧代码**编得过**，只有在 `set_loops(n > 1)` 之后才会读到错的那一份


- **在途上限只守 h1 的流水线**。`max_pipelined_requests`（默认 8）原本对 h1/h2 一视同仁，
  但它守的是 h1 **同一读缓冲里排队的请求**；h2 的并发上限是协议内建的
  `SETTINGS_MAX_CONCURRENT_STREAMS`（本库声明 100），对端自己会守规矩。再套一层 8
  只会把合规的流打成 503，而 `reject_pipelining()` 还顺手设 `connection: close` ——
  在 h2 上那是非法头（RFC 9113 §8.2.2）。判据用静态端点（只有走线程池的处理器才会让
  延迟队列堆起来）：`/static/reset.css` 一条连接上发 64 条，改前 8×200 + 56×503，
  改后 64×200；平台那次 `static-h2` 是 2xx 238,313 / 5xx 556,357。（`1.6.1`）

- **多循环停机：`~uvcpp_http_server()` 现在先停工作循环，再动按循环切的那几张表。**
  此前唯一会停 worker 的 `stop_workers()` 只在析构体**最后一行** `delete tcp_server_`
  里被调到，那时 `contexts_` 已经逐张 `clear()` 过 —— 而 worker 的退出路径会关掉自己
  名下的连接，那条路上经由关闭回调跑 `remove_ctx` ⇒ `erase` 落在刚被释放的结点上。
  裸用低层 `uvcpp_http_server` + `set_loops(n > 1)` 的调用方**没有任何 API 能履行**
  析构注释里那句「调用方的责任」，所以它不是偶发：4 核 + 8 路并发下 2000 次崩 18 次，
  改后 0 次。（`1.6.4`）

- **`connection_generation()` / `reply_alive()` 不再解引用传进来的那个指针**（`1.6.19-dev`）。
  前者的文档用法就是「开始时记下这个指针、异步回来再问一次」，那时连接**可能已经
  delete**（文档里连"地址被新连接复用"那种情形都写了）；后者更是专为"连接已经没了"设计的
  入口。而两者的实现都要先读一次 `client->loop_index()` 才知道查哪张表 —— 多循环下
  `contexts_` 按循环切开，这一步就是 use-after-free（单循环有 `contexts_.size() == 1`
  的快路径不读，所以缺陷只在 `set_loops(n > 1)` 下存在）。
  `tests/functional/web_static_server_func.cpp` 的 `multiloop_concurrency` 拿一批已释放的
  `uvcpp_tcp_client*` 来问：Linux + ASan **5/5 次**报
  `heap-buffer-overflow ... READ of size 8`（栈是 `ctxs_of()` 里那句 `client->loop_index()`
  ← `connection_generation()`）；MinGW 那条 CI 腿上就是
  `91/109 Test #91: test_web_static_server_func ....***Exception: SegFault`（Windows 的堆
  把释放掉的页还给系统，读它是访问违例；Linux/macOS 上那块内存通常还映射着，只有 ASan 看得见）。
  **改法**：另立一张 `client -> 代次号` 的登记簿（`ctx_gens_` + 一把锁），它的写入与
  `contexts_` 的增删**成对** —— 只在连接登记 / 摘除那四处，读侧只有这两个入口，两者都
  不在每请求的路上 ⇒ 一次表都不查、`client` 指向的内存一个字节都不读。`reply_alive()`
  的 h1 支路改成用凭据**自己带的循环号**选表，与 h3 那条支路同一个形状（那边一直是用
  `reply.loop_index_`）。中途试过"逐张表扫一遍"，当场被 ASan 换成 `heap-use-after-free`：
  `_Rb_tree::find` 读到的是另一条循环的线程刚在 `remove_ctx` 里 `erase` 掉的结点 ——
  跨循环读别人的表本来就是 `contexts_` 那条注释里写明的 UB，换成四张表并不改变这件事。
  **判据**：同一条用例里改为传一个**必然未映射**的地址 `(uvcpp_tcp_client*)1`（实现只要
  读它就当场段错误，返回值必须是 `0`），于是不用 ASan 也能在普通 CI 上钉住这条契约
  —— 悬垂指针读出来的东西往往还是老值、还被 `ctxs_at()` 的越界兜底夹住，"跑完没崩"
  证明不了实现没读它。ASan 全量 68/68 连跑 4 轮 0 报。

### HTTP 与 WebSocket 语义

- **流水线里末条的 `Connection: close` 会把前面几条的响应一起带走**（`1.6.46-dev`）。
  HTTP/1.1 流水线（一次读缓冲里多条请求）下，**异步响应**的第 1 条在序列化时问的是
  `uvcpp_http_server` 里那句 `ctx.parser->should_keep_alive()`
  （`src/web/uvcpp_http_server.cpp:1691-1691`），而 llhttp 的 flags 是**连接级**的、
  属于「最近一条解析完的报文」—— 一个 `execute()` 会把整批 N 条一次解完，所以第 1 条
  读到的是**第 N 条**的结论。末条写 `close` ⇒ 服务器在第 1 条响应之后关连接，第 2..N 条
  响应随连接一起消失（服务端那时在 `src/webapp/uvcpp_web_app.cpp:2690-2690` 打
  「连接 N 已断开，响应被丢弃」，客户端收到 1 条 + FIN）。**同步处理函数侥幸躲开**：
  它们的响应在解析器前进之前就序列化完了 —— 所以这条缺陷只在「流水线 + 异步响应」
  同时成立的形状里现形，而静态腿（响应要等池）正是这个形状。

  **改法**：webapp 在**发送前**把「**这一条**请求自己」的结论钉成显式 `connection`
  头（`src/webapp/uvcpp_web_app.cpp:2697-2700`）。判据取每请求的那份副本 ——
  `on_http_request()` 里 `take_from()` 早把头部整表搬成了请求自己的，`is_keep_alive()`
  读的就是这一条的 `connection`，与解析器走到哪儿无关；规则与 llhttp 逐字一致
  （1.1 默认保持、1.0 默认关闭），也就是 RFC 9112 §9.6 的「每条报文自己的连接选项」。
  HTTP 层本来就认显式头（`:1624-1628`）、`begin_stream` 也认（`:1745-1746`），所以
  **整包 / 尾随头 / 流式**三条出口一并覆盖，且 `handler` 自己设过 `connection` 的
  仍然说了算。放在「发送前」而不是「派发时」：处理函数跑之前响应表必须还是空的
  （那条既有约定由 `web_app_app_func.cpp` 的 `response_header_capacity_reused` 正对照
  钉着），而 `send_response(ctx)` 是三条出口唯一的共同上游。

  **回归用例** `web_app_pipeline_func.cpp` 的 `close_last_keeps_earlier_responses`：
  一次 `write` 三条流水线请求，末条带 `close`，且**第一条最快**（120 ms vs 后两条
  500 ms）—— 这个快慢差是承重的：后两条的响应必须在第一条**落线之后**才备好，
  否则三条会挤进同一个写批次、关闭要等队列排空，缺陷就藏起来了（实测过：等速写法
  修前修后都回 3 条）。修前实测**只回 1 条**，修后三条齐全、状态码与各自
  `connection` 头都对得上，末条之后对端仍能收到 FIN。

- **`http_compress::decompress()` 解不开任何超过 4 KiB 的东西，而且客户端那条路上是静默的**
  （`1.6.32-dev`）。`decompress()` 里两个 `inflate` 循环写的是 `inflate(&strm, Z_FINISH)`
  + `while (ret == Z_OK)`，而 zlib 对 `Z_FINISH` 的约定是：**输出缓冲被填满**时返回
  `Z_BUF_ERROR`(-5)、**不是** `Z_OK`。于是循环在**第一块**（内部输出块 4096 B）就退出、
  `ret != Z_STREAM_END`，整条判成失败，接着掉进下面那条 raw deflate 回退 —— 同样失败。
  **症状**：解压后超过 4 KiB 的东西一律解不开；4 KiB 以内一次 `inflate()` 就
  `Z_STREAM_END`，全好。**独立证据**（不是推理）：一个只链 `-lz` 的 30 行独立 C 程序，
  喂一段压完 ≈ 7 KB 的流：`Z_NO_FLUSH` 每块返回 `Z_OK`、`Z_FINISH` 在**第 0 次**调用上
  就返回 `-5`。**改法**：参数换 `Z_NO_FLUSH`，退出条件从"只看返回值"补成"返回值 **且**
  缓冲真的被填满"（`Z_NO_FLUSH` 下 `Z_OK` 而 `avail_out != 0` 只剩"输入用完了"一个含义；
  少了后半句，一个恰好整块结束的流会在最后一次白转，更坏的情况是"返回 `Z_OK` 却毫无进展"
  时原地死循环）。
  **判据**：`tests/functional/web_http_client_func.cpp` 的 `compress_large` —— 1500 行
  ≈ 70 KB 的原文，gzip / deflate / 裸流（去掉 zlib 包装走回退分支）三条腿逐字节还原。
  把参数改回 `Z_FINISH` 它立刻变红，而同一个文件里几十字节的 `compress_gzip` /
  `compress_deflate` **仍然是绿的** —— 这正是这个缺陷能活到现在的原因：**旧用例的正文
  从来没有填满过那个 4 KiB 块**，参数写错也测不出来。
  同一提交补上客户端那个"两半承诺只兑现一半"的缺口。`set_compression_enabled(true)` 的
  文档写的是"请求补 `accept-encoding`、响应收全后自动解压"，但这两件事此前**只写在异步
  `send()` 那条路上**：两条阻塞路径（`send_wait_plain` / `send_wait_ssl`）一个都不做，
  请求不补头、响应也不解压，调用方拿到的是贴着 `content-encoding: gzip` 的压缩字节。
  现在两半各收进一个私有助手（`add_accept_encoding()` / `decompress_response_body()`），
  **三条交付路径共用同一份**（三处各写一份必然再次分叉）。**端到端判据**：
  `tests/functional/web_app_app_func.cpp` 的 `compression_wiring` (c) —— 一个开着自动解压的
  客户端取 70 KB 的 `/largetext`，断言 `content-encoding` 被摘掉、正文逐字节等于原文；
  并且**先用默认（不自动解压）的客户端钉住前提**：线上的字节确实是 gzip 流（`1f 8b`）、
  **压缩后**的长度还在客户端那个 1024 的解压门槛之上 —— 否则上面那两条可以靠"服务端根本
  没压"而恒真。变异验证同上：改回 `Z_FINISH`，(c) 的两条主断言红、两条前提断言仍绿。
  公开头 `src/web/uvcpp_http_client.h` 的 `set_compression_enabled()` 文档同步写明它覆盖
  异步与阻塞两条路，并把"正文（压缩后）< 1024 B 不解压、且门槛改不了"这条一起写进去。

- **WebSocket 的发送队列整批合并成一次 `uv_write`，并把泵推迟到一次读派发结束之后**
  （`1.6.21-dev`）。`1.6.13-dev` 把 `TCP_NODELAY` 默认打开（理由见下面「接受连接时默认关
  Nagle」那条：h2 的两段式应答等 40 ms 延迟 ACK），代价是 **ws 批量回声那条路的段数从内核
  手里收回来了** —— 一次读回调里躺着 16 个 `"hello"`，而 `pump_send()` 每帧一次
  `uv_write`：Nagle 一关，16 个十几字节的段各自出去，对端每收一个回一个 ACK。
  平台 2026-10-13 那一轮（pin `2d5fe74`）与此吻合：`echo-ws-pipeline` 三档齐掉
  **−60.4% / −68.8% / −64.2%**（512 / 4096 / 16384），而同一条路的**单深度**档
  （`echo-ws-512/4096`，pipeline=1）逐字节没动 —— 症状只在批量那三档上。
  本机 veth（MTU 1500、8 连接 × 深度 16，两侧 `OutSegs` 直接读 `/proc/net/snmp`，
  帧数取客户端的完成帧，各两轮取区间）复现出**同一方向**并量出根因：

  | 发送形状 | 段/帧 | 服务端 µs/帧 | 帧/秒 |
  |---|---|---|---|
  | 逐帧 `uv_write` + NODELAY（round-B 的 pin `2d5fe74`） | 1.0000 | 3.8 ~ 4.5 | 524k ~ 859k |
  | 逐帧 `uv_write` + Nagle（`v1.6.1`，round-A 的 pin） | 0.62 ~ 0.81 | 3.0 ~ 3.8 | 817k ~ 1,240k |
  | **本批合并 + 派发延后（本次）** | **0.0625** | **0.70 ~ 0.88** | **4,111k ~ 5,215k** |

  段数与每帧的服务端 CPU 一一对应；Nagle 时代那个 0.7 段/帧是**内核替我们**压的，代价是
  延迟不可控，`TCP_NODELAY` 一开就归零。**注意本机只复现方向、不复现幅度**（本机两轮之间
  −15%，平台是 −60%~−69%）—— 平台那一轮是 512~16384 连接、64 条循环，饱和的正是每包成本。
  **改法两条**：`pump_send()` 把队列里装得下的帧拼成一份连续字节（上限 64 KiB，装不下的
  留队列、写完接着泵，帧序不变）一次交出去，完成回调按 `in_flight_frames_` **整批**结算；
  `on_tcp_data()` 派发期间 `send_frame()` 只入队，由派发结束时的 `DispatchGuard` 统一泵
  一次 —— 一次读进来 16 帧就发一段 112 字节出去。**判据**是上表的段数（16 倍）与 `frames/s`
  （4.8~6.1 倍，对 `v1.6.1` 是 4.1~5.1 倍）。
  另加用例 `close_delivered_on_protocol_error`，钉住合并之外的那半个契约：
  `protocol_error()` 是**先置 `protocol_failed_`、再 `send_close()`**，而这一切发生在派发
  期间 —— 守卫若在这个标志上跳过泵，Close 帧就永远留在队列里，**本端 `on_error` 照样响、
  既有那几条协议错误用例全都还是绿的**，只有对端一个字节都收不到。变异验证：把守卫改回
  「`protocol_failed_` 时跳过」，该用例红、其余 18 条全绿。

- **413 那个响应体此前每次都会多读 2 个字节**（`1.6.19-dev`）。两处
  `uvcpp_http_response::make(PAYLOAD_TOO_LARGE, "413 Payload Too Large", 23)` 里的 `23`
  是**手写的长度**，而那个字面量只有 21 个字符 —— `clone_data` 于是从字面量后面多读了
  2 个字节（ASan：`global-buffer-overflow ... READ of size 23`，报在
  `tests/functional/web_stream_func.cpp` 那条**超长请求体**的用例上）。越过去的那 2 个
  字节落在 rodata 里紧挨着的下一个字面量上，所以**不越页、不越段**，只有 ASan 的全局变量
  影子内存看得见 —— 这个缺陷从 `c304a4c` 起就在（不是本版引入的）。**改法**：两处都写成
  `sizeof("413 Payload Too Large") - 1`，让编译器去数，字面量改了长度跟着改。**判据**：
  `ctest -j4` 全量 68/68 连跑 5 轮 0 报；改前那条用例每跑必报。

- **静态器能直接发盘上的预压缩孪生文件了 —— `precompressed` 选项（默认关）**（`1.6.15-dev`）。
  `x.js.br` / `x.js.gz` 在盘上就整份读它、原样发出去，附 `content-encoding` 与
  `vary: accept-encoding`，**不再走实时压缩**。形状同 nginx 的 `gzip_static`（brotli 模块
  那一路）、Caddy 的 `precompressed br gzip` —— 也就是 `max_cached_file_size` 的文档里
  那句"真要做，先考虑的不是流式 deflate"所指的那条路：整份文件 ⇒ 没有块边界、
  `content-length` 已知、与 `transfer-encoding` 没有交互。
  **选择规则**：`br` 的 `q` 不低于 `gzip` 时取 `.br`（同 `q` 时 brotli 更小），否则取
  `.gz`；首选那份不在盘上就试下一个（两份都接受而只有 `.gz` 存在时**退到它**，与 nginx
  双模块、Caddy 同一次序）；客户端一个都没声明、或只声明的那个写了 `q=0`
  （RFC 9110 §12.5.3 的**明确拒绝**）就照旧发原文件。**孪生文件不存在 ⇒ 逐字节等于关掉
  本选项**，所以"没预压的资源"不受影响，开了之后没有"挑选哪些资源"的中间态。
  **四条边界**：① 带 `Range` 的请求一律不用孪生文件（要切的是**原文件**的字节，对孪生
  文件切段会切出一段没有意义的压缩流，与"206 一律不压缩"同源）；② 阈值量的是**孪生文件
  自己**的大小，原文件多大不参与判定 ⇒ 2 MiB 的原文件靠一个 1 KB 的边车下发是正常的，
  孪生文件自己超 `max_cached_file_size` 时才不用它（那条路走分片下发，挂不上
  `content-encoding`）；③ **不套 MIME 排除表** —— 那张表管"要不要**实时**压"，而 `.br`
  已经在盘上了，于是默认被排除的 `image/svg+xml` 只要有孪生文件就会用上；④ 孪生文件
  **自己**的 `mtime`/大小参与全部缓存判定（`ETag`、`Last-Modified`、LRU 的失效比较都
  取它），重压一遍之后对端必然拿到新的那一份。
  **判据**：`tests/functional/web_app_static_func.cpp` 的 `test_precompressed()`
  （十三组，`UVCPP_STATIC_TEST_ONLY=precompressed`；`/pc` 与 `/pcs` 两个挂载分别是默认
  阈值与 4 KiB 阈值），外加一份两臂逐字段的端到端核验（同一份二进制、`precompressed`
  0/1 各跑一遍，比对的字段是**长度 + `content-encoding` + `etag`** —— 长度是主判据，
  "回了 200 但发的是原文件"与"回了孪生文件"只差一个字节数）。
- **同一个 `precompressed` 选项补到 `uvcpp_static_server` 上**（`1.6.18-dev`）。上一版
  只做在 `uvcpp_web_static`（框架层 `serve_static` 那条路）上，而 web 层还有**另一份**
  静态器 `uvcpp_static_server` —— 它挂在裸 `uvcpp_http_server` 的 `on_request()` 上，
  HttpArena 条目里只有 h3 那条腿用它（h3 要挂静态文件只能走它，`uvcpp_web_app` 没有
  裸监听器）。两边**共用同一套挑选规则**：`web/uvcpp_http_common.h` 里的
  `http_pick_precompressed_variants()` 是两个实现唯一的那份 `Accept-Encoding` →
  `.br`/`.gz` 判定，`uvcpp_web_static` 也改成调它（候选与次序逐字段不变）。
  与框架层那条路的**关键差别在于它原先连变体缓存都不挂**：`compress_enabled_` 默认 true，
  而全仓只有 `uvcpp_web_static` 填 `resp.cache_tag`（响应变体缓存按它做键），本类不填 ⇒
  `uvcpp_http_server::apply_compression_for()` 那个缓存对它**不生效**，于是它**每请求在
  循环线程上把整份文件 deflate 一遍**。所以这一个选项对它不是"省带宽"而是"省掉一次白压"，
  实测 **88×**（见「性能」组）。
  **两处与框架层一致的边界**：带 `Range` 的请求不取孪生（要切的是原文件的字节）；
  缓存命中那一条路也照样挂 `content-encoding` / `vary`（否则第二次请求会退化成无编码）。
  **判据**：`tests/functional/web_static_server_func.cpp` 新增的 `test_precompressed()`
  —— 关掉时一个孪生都不碰、开着时按 `Accept-Encoding` 挑 br/gz，含 `q=0` 是拒绝、
  `*` 兜底、无孪生退回原文件、`Range` 不取孪生，以及**缓存命中那一次也要带上
  `content-encoding`**（八行）。该用例**已做变异测试**：分别把变体头摘掉、只留缓存命中那处、
  把挑选整个关掉，三次都变红，且"期望原文件"那几行在最后一次仍然绿（说明它不是恒真）。
- **`uvcpp_http_reply`：与传输无关的「稍后回话」凭据**（`1.6.8-dev`）。h1/h2 上的
  推迟应答本来靠处理函数拿到的 `uvcpp_tcp_client*`，而"这还是当初那条连接吗"只能
  另外问一次（代次号）；h3 那条腿连连接对象都没有。新类型把这层值语义化：
  `uvcpp_http_server::reply_of()` 取一枚、`reply_alive()` 判它还有没有效、
  `send_reply()` 发。里面装的是**代次号 + "在哪张表里查"**，所以传值、拷贝都安全，
  且连接地址被新连接复用时"一个字节都不发"（不是保险，是防把 A 的响应发给 B）。
  **顺带修掉一个潜伏的 h2 缺陷**：h2 上流号在 `resp` 上，而调用方造出来的响应通常
  是"新"的、那一格是 0，凭据里带的那一个才是权威 —— 从前少了这一句，`send_response()`
  就把一份**流号 0** 的响应交给会话层，它归位不了、静默丢弃，客户端一直等到超时。
  **判据**：`tests/functional/web_h2_cleartext_func.cpp` 新增的 `h2c_deferred_reply`
  （`/later` 路由，`req.reply` + `send_reply()`）—— 修前这条超时，修后 200；h3 那半
  在 `http3_web_func.cpp` 里（见上）
- 错误路径不再编造状态码；连接中途断开时不再交付编出来的 `200`（`1.1.10`、`1.1.11`）
- `Accept-Encoding` 里显式写出的 `q=0` 不再被 `*` 覆盖（`1.1.17`）
- `HEAD` 与 `GET` 的头完全一致，压缩响应也一样（`1.1.18`、`1.1.21`）
- `206 Partial Content` 一律不压缩 —— `Content-Range` 与 `Content-Encoding` 自相矛盾（`1.1.26`）
- WebSocket：服务端强制客户端掩码、校验文本帧的 UTF-8，客户端也真的掩码了（`1.1.25`）
- 跨读边界的请求不再被吃掉（`1.1.32`）
- `req.path()` 折叠连续斜杠，与路由切段看齐（`1.1.19`）；请求头与 URL 长度在收的过程中
  就被卡住（`431` / `414`）（`1.1.20`）
- 静态文件服务在缓存命中时不再返回 `503`（`1.1.15`）
- `set_keep_alive(false)` 真的发 `connection: close` 了（调用方自己没设那个头时）；
  并且在「对端正关的连接」上收到响应时，不再报一笔永远兑现不了的写（`1.2.2`）
- `uvcpp_web_request::take_from()` 之后源请求的 `url` 与 `headers` 也空了（原先只承诺
  `body`）；契约写进了头文件，并配了前置断言（`1.2.7`）
- `uvcpp_web_router::match()` 命中路径上不再收集 `allow` / `allowed_methods` ——
  结果是 `MATCHED` 时这两个字段是空的（`1.2.12`）
- 框架序列化出去的每个响应都带 `Date`（HTTP/1.1 与 HTTP/2、整包与流式四条路都在内），
  且按秒格式化一次而不是每响应一次。它**没经手**的两条报文 —— 裸写的 `100 Continue`
  与 WebSocket 的 `101 Switching Protocols` 握手 —— 都不带 `Date`（那两条是字面量
  字节串直接写出去的，不走响应收口）。RFC 9110 §6.6.1 对 2xx/3xx/4xx 是**必发**、
  对 1xx/5xx 只是**可发**，所以这两条不发也合规 —— 那是"没经手"，不是"写了排除"。
  `UVCPP_SERVER_TOKEN` / `uvcpp::server_token()` 给出 `Server` 的默认值（仍然有意
  不带版本号），`uvcpp::version_string()` 则终于把 `UVCPP_VERSION_STRING` 接出来了
  —— 那个宏此前全仓没有一个读者（`1.3.1`）
- HTTP/1.1 缺 `Host`、`Host` 出现多于一行、`Host` 字段值非法，三种形态一律真回一个 `400`
  （静默关连接不算）；`Transfer-Encoding` 整行为空也一样。判据是 RFC 9112 §3.2 那条 ——
  它是**本库此前唯一不拦的一条 MUST-400**：同规范里 §5.1 的"冒号前有空白"、§6.1 的"TE
  不以 `chunked` 收尾"、§6.3 的"多个不等的 CL"也都写着 MUST-400，但拦它们的是 llhttp，
  本库一直是 400（逐条实测过）。外加 §6.1 那条：`transfer-coding = token`、
  `token = 1*tchar`，整行为空一个 tchar 都没有，而 llhttp 在值整行为空时**不把 TE 记成
  "存在"**，于是 `TE:（空）+ Content-Length` 被当成"没有 TE"、退回 CL 框架照常应答，前端
  若按 §6.3 压过 CL 处置就是走私。判据点排在 `set_stream_claim` 钩子**之前** —— 请求被
  那个钩子认领后回调就直接返回了，排它后面等于只有低层路由受检，而 webapp 全走那条路。
  两条用例分工钉住低层与认领路径，次序做过变异验证。**有意保留的宽松**：obs-fold 仍按
  §5.2 的第二个分支（折成 SP）处理而不是回 400，`Host` 值里的 `%` 不核那两位十六进制。（`1.6.2`）

### TLS 与网络

- 加载完整证书链，TLS 版本上下限双向生效（`1.1.13`）
- TLS 握手有超时，且不在超时那条路上留下孤儿定时器（`1.1.14`）
- 在自己的回调里析构 `tcp_client` 不再按连接数累积包装对象（`1.1.16`）
- 对端断开时会触发 HTTP 客户端的关闭观察者，而不是让回调永远不来（`1.1.4`）
- `tls_verify_mode::PEER_STRICT` 在客户端侧真的校验主机名：`connect()` 收到的那个名字
  被钉给证书（数字 IP 字面量走 `X509_VERIFY_PARAM_set1_ip_asc()`，其余走
  `set1_host()`），名字对不上的对端**建立不起来**（`1.2.24`）。它此前与 `PEER`
  **完全等价** —— 服务端侧至今仍然等价：本库的服务端不发 SNI、也不要求客户端证书，
  没有可校验的名字
- 绕开 `tcp_client` / `http_client` 直接用 `uvcpp_ssl` 的调用方**拿不到**这层校验，
  得自己调 `uvcpp_ssl::set_verify_hostname()`
- **一个 `uvcpp_ssl_context` 可以被多条连接、多个线程安全共用了。** 客户端 ALPN 改成一律装到
  **本条连接**的 `SSL` 上（新增 `uvcpp_ssl_context::apply_alpn_to_ssl()`，QUIC 会话是第一个
  改用它的），不再往共享的 `SSL_CTX` 上装 —— `SSL_CTX_set_alpn_protos` 内部是"free 掉 ctx
  上旧的那份名单、再 malloc 一份新的"，两条线程各自建连接时就会读到同一个旧指针、各 free
  一次。ASAN 下是 `attempting double-free`（三字节区域，25 次里 6 次），Release 下是 glibc
  堆损坏中止（30 次里 7 次；`double free or corruption (out)`、`unaligned tcache chunk`，
  **进程被信号打死，不是一行断言失败**）。服务端那条"选择回调"OpenSSL 只有 `SSL_CTX` 级的
  注册口，所以改成**安装时进一把进程内的锁 + 同一份名单幂等**，`has_alpn_select()` 的读侧也
  进同一把锁 —— 它此前是内联的 `!alpn_select_wire_.empty()`，而"谁先装"恰好发生在多条连接
  并发建起来的那一刻，那就是边写边读一个 `std::string`（`1.6.5-dev`）
- **接受连接时默认关 Nagle**（新增 `uvcpp_tcp_server::set_tcp_nodelay()` / `tcp_nodelay()`，
  默认 true）（`1.6.13-dev`）。libuv 的 `uv_tcp_t` **不**设 `TCP_NODELAY`，于是它是开的，
  而 h1 的应答是"整条报文一次 `uv_write`"、一段就够，看不见；**两段式**应答（先
  HEADERS+DATA 再一段 trailers —— h2 上 gRPC 的 `Trailers-Only` 与 `send_trailers()` 两条
  路）第二段会一直等到对端那个 **40 ms 的延迟 ACK** 才出去。指纹很干净：同一台服务器、
  同一个 `h2load`、同一条回环上，h1 那条路（整条报文一次 `uv_write`，一段）`request`
  中位数 **21 µs**，而 h2c 上同一条 gRPC 路由是 **41.00 ms** —— 差的 40 ms 正好是延迟 ACK，
  不是负载（`-c 1 -m 1` 五秒只出 121 个请求）。开关落在 `finish_accept()` —— 两条接受路
  （就地收下、跨循环转手）唯一的汇合点，也是 fd 一定已经就位、用户回调一定还没跑的地方；
  设失败不报错也不关连接（它是**性能**开关，不该因为一个选项设不上就拒绝一条本来能用的
  连接）。判据落在**从 fd 上 `getsockopt` 读回来的那个 int** 上而不是访问器上（访问器绿只
  说明"存进去了" —— 这条缺陷恰恰是"以为设了、其实没设"）：`tests/functional/tcp_server_func.cpp`
  新增三轮，默认 ⇒ `1`、`set_tcp_nodelay(false)` ⇒ `0`，两个方向各一条（只量 true 的话
  "忘了设 false"永远测不出来，只量 false 的话"默认被翻成 false"测不出来）。
  **实测（本机 18 核，条目裸机跑，base = 改动前、fix = 改动后，三轮交替取整段，36 格
  0 失败 0 非 2xx）**：h2c gRPC `-c 1 -m 1` `24.20 rps / request 中位 41.00 ms`（五秒 121 个）
  → **`25 687–29 489 rps / 29 µs`（五秒 13.3–14.7 万个）**；`-c 64 -m 1` `1 541–1 554 rps /
  41.01 ms` → **`175 498–242 849 rps / 149 µs`**；curl（客户端自带 `TCP_NODELAY`）单连接
  打同一条 gRPC 路由，`≥30 ms` 的比例 **34/90 → 0/90**。而 **`-c 256 -m 100`（≈平台上
  `unary-grpc` 的形状）两臂落在彼此的轮间散布里，量不出差别**（h2c 594k vs 592k、
  h2/TLS 587k vs 524k），h1 的两条对照也一格没动（`-c 1 -m 1` 21→22 µs、`-c 256 -m 100`
  1.22–1.57M vs 1.23–1.61M）—— 归因因此落在"应答被写成两段"，而不是"`TCP_NODELAY`
  让什么都快了"；也正因为平台各档都是 `-m 32`/`-m 100`，**这条不是榜单分数的杠杆**，
  它修的是低并发请求/应答的时延

### 内存与缓冲

- 大块分配重新计入 `span->in_use` —— 在那之前每次分配都整段泄漏（`1.1.12`）
- `uvcpp_write` 第 2 块槽位换占用者时不再漏一块（`1.1.29`）
- 内存池的「在用块数」重新跟着分配走（`1.1.33`）
- 压缩变体表的字节账改成从表里算出来的派生量（不再存一个会漂移的计数器），
  字节上限那条腿也终于有了判据（`1.1.34`）
- `memory_pool_config::max_total_memory` 真的限额了：池按**实占**字节记账（含每块的
  块头），超额拒绝分配并计入 `failed_allocations`（`1.2.3`）。默认值 `0` 表示不限额，
  行为零变化
- 池的 SUPER 档（>256 KiB）不再静默泄漏 —— 它那次进线程本地缓存的 push 谎报成功、
  把块丢了，内存回不到全局池、额度也不退，而统计照常记一次「已释放」（`1.2.9`）
- 三条释放路径在 `free` 之后读块头（use-after-free），以及 `static_release_callback`
  的拆除分支拿 `::operator delete` 去还 `_aligned_malloc` 来的块
  （`STATUS_HEAP_CORRUPTION`）—— 两处都已修（`1.2.9`）
- 池可以接全局 `operator new` 了：它的元数据此前自己走 `new`，重入撞上函数局部静态的
  初始化守卫，会**卡死**（`1.2.13`）

### 公开契约与安全

- `uvcpp_handle` 的拷贝构造、拷贝赋值与 `clone()` **从公开接口删除** —— 它们的实现是
  `memcpy` 一个活着的 `uv_handle_t`（连着 loop 指针与邻居指针），结果是双重释放，或把
  活句柄从自己的循环队列里摘掉（`1.2.5`）。`uvcpp_req` 的拷贝操作**保留**（`uv_req_t`
  没有侵入式队列、也没有 loop 指针），只删它的 `clone()`（`1.2.5`）
- `DEFINE_FUNC_REQ_CPP` / `DEFINE_COPY_FUNC_REQ_CPP` 编得过了 —— 这两个公开宏从来没被
  编译过，里面压着三处硬错误；现在它们各自有用例盖住（`1.2.2`）
- `uvcpp_ws_connection` 的默认构造从此**有定义**：`uvcpp_ws_connection c;` 此前编得过、
  **链接不过**（`1.2.2`）

### 性能

- `write()` 先试 `uv_try_write()`，吃得下的部分不再拷进待发缓冲（`1.1.22`）
- `uvcpp_stream::try_write()` 不再白拷一份 `uv_buf_t` 数组（`1.1.23`）
- 静态响应带上压缩变体缓存（`1.1.24`）
- 响应体零拷贝接管，并与头一起作为两块写出去（`nbufs = 2`）（`1.1.28`）；
  变体表按句柄存/取，不再整份拷体（`1.1.31`）
- 响应序列化不再走 `std::ostringstream`：合计 −2.52%、用户态 −10.19%、
  91 330 → 93 688 RPS，九个端点报文逐字节不变（`1.2.4`）
- 读缓冲不再清零 —— libuv 对 TCP 流给 64 KiB、一次请求跑两趟，所以每请求白清
  128 KiB：合计 −13.66%、用户态 −22.88%、QPS +12.02%（`1.2.4`）
- 响应头表改成**按需**预留，`uvcpp_web_context` 的对象与控制块并成一次分配 ——
  每请求 16.00 → 13.00 次分配（`1.2.20`）
- 「在途请求队列」不再每请求重建：收场时把那条空表 `swap` 进本循环的回收槽、下次
  入队再搬回去（两次都是 `swap`，O(1)、不分配），而**键照旧摘掉** —— 留空键会让
  那条连接被永久豁免闲置超时，长跑服务上就是稳定的泄漏。每请求 23.02 → 22.02 次
  分配（`1.3.20`）
- 响应侧的两张每请求表（响应头表 + `on_sent` 回调表）也不再每请求新建：收场时清空
  后 `swap` 进本循环的回收槽、下一条请求**派发之前**再换进来。稳态下 `reserve(4)`、
  第 5 个头的扩容、访问日志那条 `on_sent` 的扩容三笔分配全部消失；代价是留给下一条
  的必须是**空**表，所以那两句 `clear()` 一句都不能少。每请求 22.02 → 19.02 次分配
  （`1.3.21`）
- 写路径的每响应**五笔**堆分配收到**一笔**：新增 `uvcpp_tcp_client::write_owned()`，
  由调用方自带写请求对象 —— `uv_write_t`、`uvcpp_write`、两个 `std::function` 闭包
  都不再每响应构造，头部也不再进临时 `uvcpp_buf`，而是写进调用方自己的缓冲（体走
  已有的 iovec 视图通道，零拷贝）；`conn_ctx` 增一个回收槽，稳态下在途时对象归写
  请求所有。每请求 19.02 → 13.02 次分配（`1.3.22`）
- HTTP 服务器的读路径改走框架层的事件式读回调，不再每次读先 `clone_data()` 白拷
  一份（`data` 只在本次回调期间有效，正是解析器就地吃字节的用法；事件粒度与
  `nread < 0` 的语义一字未变）。每请求 13.02 → 12.02 次分配（`1.3.22`）
- `uvcpp_web_next` 不再是 `std::function<void()>`，而是一个只持一份上下文引用的类：
  `std::function` 只在可调用对象**平凡可拷贝**时才用内联存储（GCC 的
  `__is_location_invariant` = `is_trivially_copyable`，**与 sizeof 无关**），而「闭包
  持一份 `shared_ptr`」天生不是 ⇒ 每处理一环要两次堆分配（造一次 + 按值传参再拷一次），
  本仓每请求跑两环。换类型后这两笔没了。语义一字未改（留副本 = 挂起、按值拷贝**不能**
  移动 —— 判据就是靠拷贝把引用数抬起来、空 next 调用照旧抛）。代价照实说：
  `sizeof(uvcpp_web_next)` **32 → 48**、`uvcpp_web_stream` **288 → 304**，其余公开类型
  逐项不变 ⇒ 这是**源码 + ABI 双重断点**。每请求 12.02 → 8.02 次分配（`1.3.23`）
- `uvcpp_web_context::create()` 那一块（对象与控制块合并出来的定长块）不再还给
  malloc，而是留在**本线程**的自由表里给下一条请求复用（`uvcpp_alloc.h` 的
  `uvcpp_block_cache`：`std::allocate_shared` + 一个只加一层块回收的无状态分配器）。
  **只回收内存块、不回收对象** —— 构造/析构/引用计数一字未改，`shared_from_this()`、
  `user_data_`、`hold_count_` 的语义都没动。★ 代价照实说：这一块不再经过
  `operator delete`，所以 ASAN/valgrind 看不到它的释放（查内存问题时是已知盲区）。
  每请求 8.02 → 7.02 次分配（`1.3.24`）
- 在途请求队列表（`loop_slot::inflight`）的**键**不再每请求重建 —— 键的寿命
  改成「连接还活着 ∪ 队里还有人」，谁后到谁摘（最后一条出队那一处 + `on_close()`
  那一处）。于是那个 `std::map` 结点、以及结点上挂着的那块队列缓冲，都从
  **每请求一次**变成**每连接一次**。★ 配套改的还有**每一处"有在途请求吗"的
  判据**（`idle_sweep()` 的闲置豁免、`shutdown_step()` 第 0 拍）：一律改成
  **逐队判空**，不再拿"表里有键"当在途 —— 两个失败都是静默的：前者让一条
  跑完过请求的 keep-alive 连接被永久豁免闲置超时，后者让停机的看门狗永远等不完。
  M1 那笔引进的"空队列回收槽"（`out_recycle`）随之整个删掉：它换的就是这个形状。
  每请求 7.02 → 6.02 次分配（`1.3.25`）
- 请求头表**搬移的目的地**挂到**连接**上，不再每请求新造：解析器增
  `take_headers_into(http_headers& dst)`（`clear()` + 逐元素搬，`clear()` 只改
  size、不还容量 ⇒ 容量够时一次分配都不做），HTTP 层的两条建视图路与累积那
  条路分别把目的地定成 `conn_ctx::stream_request.headers` /
  `conn_ctx::request.headers`；webapp 增
  `uvcpp_web_request::adopt_headers()` / `yield_headers()`（非虚，与 M2a 那对
  响应表同形状）与每连接一块空表 `loop_slot::req_hdr_recycle`，派发前认领、
  `context_finished()` 里归还。★ 交给处理函数的就是**连接上那一个**请求对象
  了（不再是 `on_request_complete` 里的局部），所以「返回之后不得再读它」这
  条约束违反后的症状从**崩**变成**静默读到下一条请求**；同样地，响应发出之
  后请求头表就是空的（归还时机不早于 `notify_sent()`）。
  每请求 6.02 → 5.02 次分配（`1.3.26`）
- 响应序列化不再每请求造一个串：`uvcpp_http_response` 增**增量** API
  `to_string_into(std::string& out, bool include_body)`（`out.clear()` 之后
  照原样拼写、容量留下；`to_string` 退化成"造一个空串再调它" ⇒ 两条路出来的
  字节逐字节相同），服务端把它接到 `conn_ctx::wire_recycle` 这块**连接级**
  缓冲上 ⇒ 第二次起 `est > capacity()` 恒假，整条响应一次分配都没有。省掉的
  那次分配本来是**纯浪费**：那个临时串的字节立刻被拷进写请求自己的头部缓冲
  （`uvcpp_tcp_client::write_owned` 里的 memcpy），随即析构。`enqueue_write` /
  `start_write` 的头部参数由按值改按 const 引用（两条写路径都在返回前把字节
  拷走），入队那条路自己留一份拷贝。
  每请求 5.02 → 4.02 次分配（`1.3.28`）
- 登记表的两个索引从 `std::map` 换成 `std::unordered_map`，两个键各配一个**显式混合**
  （splitmix64）的哈希器：键本身带低位结构（自增的 `conn_id`；等尺寸分配落在同页偏移的指针），
  而 MSVC 的 `unordered_map` 用 2 的幂桶、用恒等 `std::hash` ⇒ 不混合就**静默**退化成走链，
  且在 Linux（素数桶）上量不出来。孤立微基准：每次请求的键比较 **40.45 → 4.00**（10.11×，n=400）；
  本进程采样器同一轮交错 3 对，登记表那一簇占比 **+1.16 pp 中位（3/3 为正）**；配对 µs
  −0.08/−0.29/−0.46 落在装置自身 ±0.25 µs 地板内 ⇒ 只当旁证。`ids()` 原先靠 `std::map`
  迭代天然升序，换容器后不再成立 ⇒ 改成显式 `std::sort`。
  **公开布局断点：`sizeof(uvcpp_web_connection_registry)` 120 → 136（+16）**
  （libstdc++ 上 `unordered_map` 比 `map` 宽 8 字节，两张表；MSVC 上宽度不同，但同样是一个断点）；
  `uvcpp_web_app` 与其余公开类型 `sizeof` 不变。每请求分配 3.0161（不动，`1.3.30`）
- `uvcpp_buf` 自有块的**取/还**挂到一张**线程局部**的 `malloc` 族自由表上
  （`uvcpp_malloc_block_cache`，按精确尺寸分槽、4 槽 × 每槽 64 块 × 单块不超
  4 KiB ⇒ 每线程最多留 1 MiB）：取在 `resize_impl()` 两条「新造一块」的分支，
  还在 `free_own()` **与写请求的 `release_second()`**。两处都在
  `#if !UVCPP_ENABLE_MEMORY_POOL` 里 —— 与 M6 那张表**不是同一族**
  （`::operator new` 族 vs `malloc` 族，混用是 UB），所以宁可另起一族。
  ★ 为什么还块要落在写请求那一侧：响应体那块是被 `adopt_body()` 把所有权
  接过写请求的（`release_uv_buf()` 会先把 `capacity_` 清零），**不走
  `free_own()`** —— 只挂 `free_own()` 的那一版读数一行不动，原因就在这里。
  每请求 4.02 → 3.02 次分配（`1.3.29`）
- http 层的**响应头表**：那份 `std::vector<http_header>` 的**缓冲**按连接留下来
  （`uvcpp_http_response::adopt_tables()` / `yield_tables()` 一对换手；认领在
  `uvcpp_http_server::on_request_complete()` 造出 `resp` 之后、处理函数跑之前，
  归还在 `send_response()` 的 h1 出口）。同一份容量原本每请求重新长一遍
  ——`reserve(4)` + 两次 `_M_realloc_insert`（2→4→8）+ `resp = uvcpp_http_response::ok(...)`
  那次整体拷贝赋值自带的一次 `operator=`，调用点普查上合起来 **4.00 次/请求**，
  而这些分配一个字节的头都没多存。`GET /` 探针实测 **7.00 → 4.00 次/请求**
  （ABBA 交错各 6 跑；对面 hical 同一个量是 3.00）。
  ★ 表跨请求活着 ⇒ "处理函数拿到的一定是干净的空表"从一句显然的话变成必须守的
  **不变式**：换出去之前先 `clear()`（`yield_tables()` 里那句是主守卫；
  `adopt_tables()` 里那句是第二道网，**单删观察不到**），并补了用例
  `response_headers_do_not_accumulate`（直读处理函数拿到的表 + 端到端两条判据，
  因为只查"第二条看不见第一条的头"在残留**空名字**条目时恒真）。
  ★ 契约面：`uvcpp_http_response` 只**加两个非虚方法**、不加数据成员 ⇒
  `sizeof` 与 vtable 都不变，**无 ABI 断点**；`conn_ctx` 是私有嵌套类型 ⇒
  `sizeof(uvcpp_http_server)` 也不变。**行为**上有一处变化：交还之后对**同一个
  响应对象**再调一次 `send_response()`，第二次发的是没有头的报文（改前是第二份
  完整报文）——一个请求发两份响应在 HTTP/1.1 上本来就是帧序错乱，且本函数自己
  就会 `set_header`，两次调用从来不幂等；这里如实写出，不写成"无行为变化"。
  残留的 4.00 里有两笔来自**处理函数自己造的临时响应**（`ok()` 里那句
  `http_reserve_headers`），那份表不归连接管、回收槽够不到（`1.3.31`）
- 修掉上一笔（`1.3.31`）在 **webapp 那条路**上引进的**每请求 +2.00 次分配**：
  连接上的响应头表回收槽（`conn_ctx::resp_hdr_recycle`）与 webapp 每循环的槽
  （`loop_slot::resp_recycle`）**抢同一块缓冲**。`uvcpp_http_server::send_response()`
  收的是**引用**，而交进去的响应对象**不一定是领走那块缓冲的那一个** —— webapp
  那条路正是反例：`on_request_complete()` 在自己那个**局部** `resp` 上认领，随后
  因为 `resp.deferred` 为真直接返回（那一位由 webapp 只在"派发那一刻传进来的对象"
  上设），真正被发送的却是 **webapp 自己**手上那个对象；于是那句无条件的
  `yield_tables()` 把 webapp **有容量**的表换进连接槽、塞给它一块**容量 0** 的表，
  紧接着 `context_finished()` 又把这块钱 0 的表收进 `resp_recycle` ⇒ **回收槽每请求
  被毒化一次**、下一条请求的表从 0 重新长。验收路由 `/` 实测（`set_loops(1)` 档、
  全局分配计数器）：**5.03 → 3.03 次/请求**（Δ −2.00，两次交错零重叠），落回并略优于
  H1 之前那两笔（`1.3.28` 3.0356、`1.3.30` 3.0326）。
  改法＝连接上记**谁领的**（`conn_ctx::resp_hdr_owner`），只有领走那块缓冲的对象才把它
  还回去；另配一个 RAII 兜底，管 `on_request_complete()` 那几条**没走到
  `send_response()`** 的出口（升级 / deferred）。
  ★ 这一笔的判据是**两套、各管一条路**，因为两条路上的**主守卫是不同的一句**
  （与 `1.3.31` 那对"单删都观察不到"的守卫恰好相反）：
  · **容量面**（新用例 `response_header_capacity_reused`）管 **webapp 路** ——
    掐掉 `send_response()` 里那句 owner 判据 ⇒ 红（读数 `0/0/0`、地址 `nil`）；
  · **分配整数**（`probe_h`）管 **http 层路** —— 掐掉 RAII 那句判据 ⇒ 红
    （4.0051 → 7.0053：同一条路上还了两遍，槽被清空）。
    整段 RAII 拿掉**两套都绿**（计数 4.0052、容量 8）—— 那是**退役的兜底**：
    它只在"认领了却没走到 `send_response()`"时才起作用，而现在两条臂都不走那条路。
  · 新用例**自带正对照**：第一条请求拿到的表容量必须是 **0**（冷的），否则"第二条
    有容量"完全可能只是"容量天生就有"。读的是**容量**而不是 `data()` —— 换过手之后
    `reserve` 常拿回同一个地址，只看地址是瞎子。
  ★ `1.3.31` 那条**行为变化**从此**只在 owner 判据为真的那条路上**成立：归还改成
    `if (ctx.resp_hdr_owner == &resp)` 之后，只有**领过**那块缓冲的响应对象才会还；
    **webapp 路**上 `send_response()` 那一刻什么都不还（那一侧由 M2a 那个每循环的槽在
    `context_finished()` 里收）。这里**只写结构**（两行守卫的真值表），不写「第二次会
    发什么」—— 那是行为断言，本笔没有装置去量它。
  ★ H1 在 **http 层**的收益一分不动（`probe_h` 4.0051，与 `1.3.31` 同值）；响应字节与
  上一笔**逐字节相同**（5 条路由：命中 / 路径参数 / 中间件 / 404，只归一 `date` 的值）。
  ★ 契约面：`conn_ctx` 是**私有嵌套类型** ⇒ 只加一个数据成员；公开类型 `sizeof`
  七型不变、**无新公开名字、无 ABI 断点**（`1.3.32`）
- 头名查找多了一组**不拥有**的 `const char*` 重载（14 个类成员 + 4 个自由函数），
  超过 SSO 上限的字面量不再构造临时 `std::string`（`1.2.16`）；值位置
  `text()` / `html()` / `json()` / `json_str()` 同理（`1.2.17`）
- `uvcpp_http_parser::take_headers()` 改成搬元素而不是搬 vector，解析器的头表
  high-water 因此按连接钉住 —— 每请求 19.00 → 18.00 次分配（`1.2.18`）
- `uvcpp_http_request` 重新有了真正的移动操作：用户声明的拷贝赋值会抑制隐式移动赋值，
  于是 claim 路径上那句 `req = std::move(...)` 在静默地深拷整张头表（`1.2.8`）
- `uvcpp_buf` 那 14 个「先 resize 再 memcpy」的入口不再清零马上要被盖掉的那段 ——
  100 B GET 从 3.4 次 `memset` / 约 200 B 变成 0，1 MiB POST 从 38 次 / 3.00 MiB 变成 0
  （`1.2.15`）
- 写队列把能带走的整批拼成**一次**写，不再每完成一块发一块 —— 段/请求
  3.43 → 2.06（−40%）、同装置服务端每请求 CPU 约 −30%、RPS 38 839 → 57 067（`1.2.14`）
- 空闲连接上的响应不再绕写队列（`1.2.10`）；普通响应不再为完成回调付两次堆分配
  （`1.2.6`）；路由命中路径不再算 `allow`（`1.2.12`）

**实测数据（不是估算）** —— 见 [doc/benchmark.md](doc/benchmark.md)：每条空闲连接
**4.62 KiB**（4 734 B，八档最小二乘，R² = 0.999987，外推 100 万连接 ≈ 4.42 GiB）、单事件循环
75 k RPS、10 分钟长跑 3 840 万请求 0 错误。那一页还把每连接这个数与
[Hical](https://github.com/Hical61/Hical) 自己的报告做了对照，并写明了该对照带的口径问题。
上面 `1.2.x` 那几笔读数来自**别的装置、别的轮次**，不能与这些相加。本仓另有一套压测靶场，
在 `bench/` 下、由 `UVCPP_BUILD_BENCH` 开关控制（默认 OFF，不进 CI），说明见
[doc/benchmark-rig.md](doc/benchmark-rig.md)。


- **压缩不再每请求新建一个 deflate 内核**。level 6 / windowBits 15 的内核要一大块内存
  （两个 32 KB 窗口 + 32 K 条目的 hash 表与 prev 链，约 256 KB），而这块内存在请求之间
  **不携带任何信息** —— `deflateReset()` 之后产出的字节与刚 init 完的逐字节相同，参数
  一个都没变。改成每线程留一份、复用前 reset。判据：产物逐字节相同（三档请求的 gzip 体
  md5 两边一致），服务端 +2.4%（逐轮交替各 5 轮，两边不重叠）。**为什么只有 2.4%**：
  glibc 的 mmap 阈值会自适应，紧密循环里那次 init/end 几乎不要钱，差值只在服务端那种
  混着大量其他分配的真实节奏里才露出来。（`1.6.1`）

- **预压缩孪生文件的实测**（`1.6.15-dev`）—— 量具是 HttpArena 条目的**本机裸机复现**
  （同一份 `build-rel15`，A/B 由环境变量开关、同一份二进制；起停之间断言端口上只有一个
  监听地址、一个持有进程）。改前/改后：**一整轮 `static-h2` 的那 20 个 URI 实际下发
  434,605 → 325,461 字节（−25.1%）**；单资源 `icon-sprite.svg` 19,764 → 64,473 rps
  （**+226.2%**，384.3 → 110.7 µs/请求）；`vendor.js` 17,099 → 21,639（+26.6%，
  445.0 → 349.3 µs）；整轮 `static-h2` 41,899 → 46,645（+11.3%，256 连接）、
  37,108 → 39,779（+7.2%，1024 连接）。四格都是两臂各 4 次的均值（两臂交替起停、
  每臂两次落位），括号里是那 4 次的极值 —— 两臂的区间**不重叠**，但同臂内
  离散可到 ±15%（`static-h2-c256` 那格 36.0k–52.1k），所以只有 `svgbig` 那一格
  够格当"改动的效果"引用，其余三格只报方向与量级。
  **`icon-sprite.svg` 那一格能把归因钉死**：它在
  MIME 排除表里（`image/svg+xml`），改前**根本没被压过**，两臂唯一的差别就是线上
  71,680 → 11,739 字节 ⇒ 那一格量到的是纯带宽，不是省下的 CPU。
  ★ 这一轮还澄清了一个**量具幽灵**：条目里每个 app 都 `set_loops(n)`，于是同一个端口上
  有 n 个 `SO_REUSEPORT` 监听套接字，而**内核按 4 元组哈希在"进程"之间也分流** ——
  上一轮遗留的一个 `server_fastfs` 还绑着 8443 时，curl 的每次新连接被随机分给两个
  进程中的任一个，看上去就是"孪生文件时好时坏"。**库里没有这个缺陷**，是量具里多了一个
  进程。此后所有脚本起停之间都断言"端口上恰好一个监听地址、恰好一个持有进程"。方法与
  读数记在 [`doc/benchmark-rig.md`](doc/benchmark-rig.md)。

- **`font/woff` / `font/woff2` 不再被 deflate**（`1.6.16-dev`）—— 排除表里补两条
  **精确匹配**。这两个容器里装的本来就是压缩流（WOFF 是 gzip 过的 sfnt、WOFF2 是
  brotli 过的），再压一遍不但白烧 CPU，产物还**比原文件大**：本仓语料上
  22 528 → 22 556、18 432 → 18 460 字节。**只排这两个类型，不排 `font/` 前缀** ——
  `font/ttf` / `font/otf` 是未压缩的轮廓数据，deflate 有实打实的收益，前缀匹配会把
  它们一起排掉。Caddy 的 `encode` 跳过表也是把这两个逐个列出来的。判据：
  `tests/functional/web_http_client_func.cpp` 的 `test_mime_exclusion` 补了四条
  （woff/woff2 必须被排除，ttf/otf **必须不被**排除 —— 后两条是防"图省事写成前缀"
  的护栏）；端到端两臂核验见下。
  ★ 实测（同一份条目、同一个驱动器，`UVCPP_ABL_FONT_EXCLUDE=0|1` 两臂**逐轮**交替，
  各 4 轮；两臂都开着 1.6.15 那个 `precompressed`，woff2 没有孪生文件所以这一开关
  只影响 MIME 表）：只发那两个 woff2 时 **13 486 → 52 427 rps（+288.8%）**、
  服务端 **617.3 → 143.3 µs CPU/请求**；整轮 `static-h2`（`-c 256`）
  **43 756 → 52 752（+20.6%）**、175.7 → 139.1 µs。两臂的 woff2 那 4 轮区间
  （9 385–15 356 对 42 202–57 462）**不重叠**。

- 每请求的序列化与查询解析路径上省掉三处重复工作（`1.6.17-dev`）—— 三处都只动**自己这一趟**，
  不改任何分支结果，报文逐字节不变（`to_string()` 与 `to_string_into()` 仍是同一份实现，
  测试里那条逐字节比对继续覆盖）。
  ★ **`web_parse_query` 先按 `&` 的个数预留**。`?a=1&b=1` 这种两个参数的查询，原先第二次
  `push_back` 会撞一次 `vector<pair<string,string>>::_M_realloc_insert` —— 那一次要把已经
  装好的键值对**整体搬走**再释放旧块。段数至多是「`&` 个数 + 1」（空段会被跳过 ⇒ 是上界），
  所以预留之后这一支**归零**。
  ★ **响应头表只走一趟**。原先 `to_string_into` 为了估长度、判 `chunked`、判
  `has_header("content-length")` 把 `headers` 走三遍，加上落笔那趟共四遍；三条判据都只依赖
  `headers` 自己，合成一趟不改任何分支（`has_cl` 只被"落笔之后要不要补"用到，先记住即可）。
  ★ **状态行不再为字面量造 `std::string`**。`version_str` 那两句的重心是那个三元表达式
  `cond ? const char* : const std::string&` —— 公共类型是 `std::string`，于是**每一次响应**
  都要拷一份 `status_message`（哪怕它是空的），配上 `version_str` 就是每响应两次分配、
  两次释放、两次 `strlen`，只为持有 8 字节和 3 字节的字面量；改成 `const char*` + 显式长度。
  同处的 `size_to_string` 由「`reserve(m)` + m 次 `+=`」改成「低位先落地、倒一次进正向缓冲、
  `std::string(fwd, m)` 一次成串」（m ≤ 15 走 SSO，不分配）。
  **判据**（callgrind，h1 `baseline2?a=1&b=1`、`-c 2000`、5 s 窗口，每轮
  `callgrind_control -d` 取**增量**；同一份 `server_ab`、只换 `LD_LIBRARY_PATH` 指向的
  那份 `libuvcpp.so`，两臂交替）：逐函数归因 —— `_M_realloc_insert` **154.9 → 0**、
  `malloc`/`_int_free`/`free`/`operator new` 那簇 **−133.7**、新增的数 `&` 循环
  `web_parse_query` **+46.2** ⇒ 查询预留净 **−242.4**；`has_header(const char*)` −10.1、
  `__strlen_avx2` −14.2 及摊开的部分 ⇒ 头遍历合一 **−44**。合起来
  **11 287.7 / 11 284.9 → 10 927.5 条/请求**，即 **−358.8（−3.18%）**；跨批次复核
  −366.7（−3.25%）。
  ★ **量具本身暴露的一个坑**：per-function 比对的分组键必须拿
  `os.path.basename(ob)` 归一 —— callgrind 在某些 dump 里用绝对路径拼对象名，两臂的目录名
  不同，不归一的话同一个函数会以「−X / +X」**成对**出现，看着像每个函数都被改到了。
  量法与这次一并被**证伪**的假设（`uv_write` 每条响应多一次 `epoll_ctl`；`baseline` 每请求
  多于 1 次 `write`）记在 [`doc/benchmark-rig.md`](doc/benchmark-rig.md)。
  ★ **试过并退回的**：把 `web_url_decode` / `web_collapse_slashes` 改写成原地版本（外加
  `take_from` 少一次拷贝）这一整包在同量具上是 **+328.6 条/请求**（`web_url_decode` 每请求
  被调 5.03 次，其中 4 次是 1 字节的查询键/值，原地版省下的那点拷贝抵不过多出来的一趟扫描），
  已整体还原，只留下上面这三处。

- **`uvcpp_static_server` 发盘上孪生的实测**（`1.6.18-dev`）—— 量具是仓外一支探针
  （`probe.cpp`），**一次只起一条静态腿**、`h2load -p http/1.1 -c 64 -m 2 -n 8000` 打
  同一份 `data/static`（20 个文件 + 30 个孪生，原件合计 1 271 603 B），
  `Accept-Encoding: br;q=1, gzip;q=0.8`、20 个 URI 轮转。两臂：`set_precompressed`
  0 / 1。

  | 腿 | 关 | 开 | 差 | 8000 请求送出字节 |
  |---|---:|---:|---:|---:|
  | `uvcpp_static_server`（`static-h3` 那条腿） | 933.15 rps | **82 451.28** | **88×** | 173 843 446 → 131 194 904（−24.5%） |
  | `uvcpp_web_app::serve_static` | 110 773 | **185 280** | **+67%** | 172 797 487 → 131 865 720（−23.7%） |

  `uvcpp_static_server` 那一格能到 88× 是因为它**原先连变体缓存都没有**（每请求在循环
  线程上 deflate 一整份文件，见「HTTP 与 WebSocket 语义」组），所以开孪生对它等于**删掉
  一整次 zlib**；`uvcpp_web_app` 那条腿本来就"压一次缓一次"，开孪生只省掉"压出来那份比
  盘上那份大"，因此只有 +67%。两条腿在 `pc=1` 上收敛到**同一个字节数**（653 777 076），
  是"发的是盘上同一批孪生"的旁证。**一条口径**：关那一臂发出去的**不是原文件**，是实时
  deflate 出来的 gzip（每轮 426 KB，比盘上那套 `.gz` 孪生还大）—— 所以这张表是"现场压
  vs 发盘上那份"，不是"发原文件 vs 发压缩件"。
  ★ **这一轮量具踩的两个坑都记在 [`doc/benchmark-rig.md`](doc/benchmark-rig.md)**：其一，
  语料的原路径挂在 **ntfs-3g（FUSE）**上，一次 `stat` **45.9 µs**，同一批文件拷到 ext4 上
  是 **1.35 µs（34 倍）** —— 两条静态腿都每请求 stat、开孪生还要多 stat 一次，于是只在
  原路径上量会得到**反的**结论（`app` 腿看似慢 2.4 倍，其实是卡在 stat 上）；其二，
  `h2load -p http/1.1` 的 `-m` 是**流水线深度**，`uvcpp_web_app` 默认 8 的在途上限越界就回
  503 并关连接，`-m 32` 下那一片 5xx 全是驱动器的参数，不是服务端读数。

- **`uvcpp_json_writer` 把「一个键」从 4~5 次追加压成 1 次**（`1.6.22-dev`）—— 量具是仓外
  一根探针，语料是 HttpArena 的 `data/dataset.json`（50 项、整份响应 8 397 B，形状与平台
  `json-50` 那三档一致），四个臂：

  | 臂 | 形状 | 50 项响应（绑核 5 轮） |
  |---|---|---:|
  | A 今天的条目 | 每项 `nlohmann::json::dump()` 后拼进同一个缓冲 | 32.77 ~ 32.91 µs |
  | **W 框架写出器** | **`uvcpp_json_writer` 一趟直写**（本条改的就是它） | 29.31 ~ 29.90 → **20.66 ~ 20.90** |
  | F 手写一趟 | 栈上两位一取的整数表、光标直推预分配缓冲 | 15.50 ~ 16.29 µs |
  | E 预算串（诊断） | 同一批字节，直接来自预先算好的每项字符串 | 3.19 ~ 3.42 µs |

  即**写出器本身 1.42~1.44×**，而条目那条路（每项 `dump()` 后拼接）换成写出器是
  **1.49~1.58×**（两个臂各自的量具都绑了核，见下）。同一台机上两次会话之间还有约 4% 的
  漂移，所以这里一律写成区间，不写单点。

  **根因不是转义、也不是整数转换，是「每次调用开几次 `std::string::append`」**。`key()`
  原先把逗号、开引号、键体、闭引号、冒号**各追加一次**（一项里第一个键 4 次、其余 5 次），
  字符串值 3 次；于是一份 8 397 B 的响应要 **4 073 次** `append_raw` —— 平均**每 2 个字节
  一次**。每一次都得跨出这个翻译单元（`libstdc++` 的 `_M_append` 带容量检查与 `catch` 子句，
  内联不进来），一行一项地累积成整份响应的开销。改法是在 256 字节的**栈**缓冲上把整段拼好
  ——`[`、`"`、转义后的键体、`"`、`:`（字符串值则是 `"`、体、`"`）——再**一次** `append_raw`
  写掉：同一个响应 **4 073 → 1 536 次**。转义表、短转义字符、十六进制小写常量与慢路
  **共用同一批**，所以**产出逐字节相同**（同一个探针在两版上报的都是 8 397 B）。
  快路只在「最坏展开量（每字节 6 B 的 `\u00xx`）塞得进栈缓冲」**且**「按最坏量算也必定
  不触 `max_bytes`」时才走；长键长值（> 42 B）以及逼近上限的一律**逐段回落到原路** ——
  连「触界那一刻缓冲里留下哪几个字节」都逐字节没变。

  | 分族（每次调用） | 改前 | 改后 |
  |---|---:|---:|
  | `key()` | 30.22 ~ 30.81 ns | **16.17 ~ 16.54** |
  | `value(const std::string&)` | 22.91 ~ 23.06 | **16.82 ~ 17.16** |
  | `value(long long)` | 16.95 ~ 17.19 | **16.04 ~ 16.38** |

  **判据**：`tests/functional/json_writer_func.cpp` 原有 867 行**一字未改**，另加 44 行钉住
  两条快路的边界（42/43 字节那道界、210 字节长键走回落、以及「快路按最坏量拒绝、实际却
  塞得下」时仍然成功），这一组断言在**改动前后双双全绿** —— 同一批期望串同时约束快慢两
  条路。全套 79 条 `ctest` 在改动前后逐条**同名同结果**（改动前就红的那 18 条一条不多
  一条不少）。
  ★ **剩下那一截是契约的价格，不是没想到**：E 的 3.3 µs 说明这份响应的开销里 **约 90% 是
  序列化**（拷字节与开缓冲合计只占一成），而 F 的 15.9 µs 与 W 的 20.8 µs 之间差的，正是
  **每次调用还剩的那一次** `std::string::append`。要再吃掉它，只能让写出器在写的过程中把
  `out` 的 `size()` 留在旧值上（写进已分配容量、收尾才 `resize`）—— 而 `str()`/`data()`/
  `size()` 是这个类**公开且文档化**的观测点，那会把「一次调用中途读缓冲读到什么」变成另一
  种语义。实测会话语义至今是：**凡是成功写进去的，都已经在缓冲里**。
  ★ E 那一臂**只是诊断**：预序列化缓存正是 `json` 档规则对**两种 type 都**明令禁止的东西，
  它在这里的作用是给「不序列化的话能做到多少」定一个下界。

- **h2 的每响应出站 body 改成按引用持有**（`1.6.23-dev`）—— `impl::out_body` 原来按值存
  一份 `std::string`，`submit_response` 那一步整块拷一遍，而走到这里的 body 绝大多数本来
  就是共享视图（静态层的 `body_share`、压缩缓存的 `resp.body.share(...)`）。改成
  `shared_ptr<const std::string>`：共享视图直接接引用计数（零拷贝），非共享的仍在原地
  `make_shared` 一次、与旧写法逐字节同价。**判据**是确定性的（不吃压测噪声）：LD_PRELOAD
  的 malloc 包装器数「≥ 32 KiB 大块分配」的次数与字节数 —— h2+TLS 单文件 67 KB 上
  大块/req 1.9183 → **0.8949**、大块字节/req 502 066 → **433 954**（剩下那 0.89 次是 TLS
  密文那一侧的）；20 文件混载大块字节/req −15 542；**阴性对照 h1 明文腿逐字节不动**
  （0.9990 / 65 471 两版一样），说明量的是改动不是环境漂移。
- **h2 `flush()` 的整批克隆改成共享视图，只在 TLS 那一支**（`1.6.24-dev`）—— `flush()`
  把这一批帧从 `out_` swap 出来后原来要拷成一份自有块（本地 `pending` 出了函数就没了，
  而写是异步的）。TLS 连接上 `SSL_write` 只**读**明文（密文由它自己拷进 wbio），
  `out_uv_buf()` 根本不会被走到，于是共享视图可以原地不动；明文那支**照旧克隆**，因为
  `write(uvcpp_buf*)` 是**接走所有权**的，共享视图到那一步会被 `materialize()` 白共享
  一次、照样得拷一遍 —— 留着这一支才能保证 h2c 腿**逐字节不变**。**判据**同上一提交：
  h2+TLS 单文件 67 KB 大块字节/req 433 954 → **365 246**、进程 RSS 285 MB → **208 MB**；
  **阴性对照 h2c 与 h1 明文腿一次都没动**。（另有一条 1 MiB 的读数（每臂 ~1.3 s、
  ±15~35% 散度）分辨不出这个量级，已作废，不作为证据。）
- **静态腿的缓存校验可以搬到循环线程上做**（`1.6.25-dev`）—— `static-*` 三档的每请求
  CPU 是 210 ~ 254 µs，而命中的请求每一条都还**投一次线程池**，只为做重新校验那一次
  `stat`（池往返 + 池锁争用 18 ~ 36 µs）。新增选项 `uvcpp_web_revalidate { POOL,
  LOOP_STAT }`，**默认 `POOL`**（1.6.24 之前唯一的行为，一行没变）；`LOOP_STAT` 把探路径
  与缓存比对提前到 `serve()` 里、在循环线程上同步做完，命中路径**零池往返**。两条判据都
  记在 [`doc/benchmark.md`](doc/benchmark.md) §4.7：**确定性**的 `strace -f -c`（同一份
  4000 请求）`futex`/req 4.51 → **0.23**、`statx`/req 1.00 → **1.00（一个不少 —— 校验
  照样做，只是换了条线程）**、总系统调用/req 7.40 → **1.86**；**性能**的平台形状
  （`-c 256 -m 32 -D 5s`，三次取中位数）`us_per_req` h2tls 84.571 → **64.671**（−23.5%）、
  h1clr 56.084 → **39.448**（−29.7%）、h2c 67.667 → **46.481**（−31.3%），两臂用的是
  **同一个二进制、同一个 `.so`**，唯一变的是环境变量 `UVCPP_ABL_LOOPSTAT`。★ **只在本地
  盘上开**：循环线程上那次 `stat` 是阻塞的（ext4 1.35 µs、FUSE / 网络盘 46 µs 量级），
  后者会拖住同一条循环上的其它连接 —— 两个数都写进了 `uvcpp_web_static.h` 的选项注释。
  语义逐字不变（「下一个请求必带新字节」），`web_app_static_func` 新增的 `loop_stat` 用例
  钉住"改写文件后**第一个**请求就拿到新字节"与"一次命中都不投池"。
- **同一个旋钮接上 h3 那条静态腿**（`1.6.26-dev`）—— 上一提交只覆盖了 h1/h2 两条腿
  （它们走 `webapp/uvcpp_web_static`）；平台上还有第三档 `static-h3-64`，那条腿走的是
  **另一份实现** `web/uvcpp_static_server`（`uvcpp_web_app` 没有 QUIC，h3 上挂静态文件
  只有这一个选择），得单独接一次。新增 `uvcpp_static_revalidate { POOL, LOOP_STAT }`
  与 `set_revalidate()`，**默认 `POOL`**（与 webapp 侧同名同义、默认同档）；`LOOP_STAT`
  把校验那次 `stat` 就地做在循环线程上 —— 目录 index 与 SPA 回退那两跳递归也一起
  （`stat_dispatch()` 在栈上拷一份 task 走完），命中与 404 直接写回调用方的 `resp`，
  **只有缓存未命中**（要读盘）才照旧落到 deferred 那条路。**判据都记在
  [`doc/benchmark.md`](doc/benchmark.md) §4.8**：确定性的 `strace -f -c`（同一份
  3000 请求）`futex`/req 6.28 → **0.66**、`statx`/req 2.24 → **2.24（一个不少）**、
  总系统调用/req 28.81 → **22.62**；平台形状（`-c 64 -m 64 -t 8 -D 5s`，三次取中位数）
  `us_per_req` 125.733 → **117.361**（−6.7%）、rps 60 743 → **68 148**（+12.2%）——
  省得比 h1/h2 那两条腿少，是因为这条腿每请求有 ~19.8 次 `sendmsg`/`recvmsg` 是 QUIC
  自己的、池往返本来就只占小块。**同一轮的尾延迟复核**（`--log-file` 逐请求耗时）：
  p99 170 526 → **140 458 µs**、max 358 108 → **307 201 µs** —— 4.7 里记的"就地 `stat`
  可能把 p99 拖爆"这条风险在本地盘上**没有被证实**。`test_web_static_server_func` 新增
  的 `loop_stat` 用例把七种请求形状逐字节比对（含目录步进与 SPA 回退那两跳递归），
  外加"改写文件后立刻拿到新字节"与"SPA 关掉时 `404` 也就地回"两段，并读**处理函数
  返回之后**的 `resp.deferred` 钉住"命中与 404 就地回、未命中才推迟"这一格
  （这一格正是两档唯一不同的地方）。
- **h3 静态腿的命中路径不再整块拷贝**（`1.6.27-dev`）—— 命中一次原来要把缓存里那份
  字节**拷两遍**（`ok()` 里造一份临时响应，再整块赋给调用方），路径上还多一次 16 KB 的
  `malloc`/`free`。改成就地往调用方选定的那个对象里填、字节**借**缓存里那一份
  （`uvcpp_buf::share()`，缓存项自己已是 `shared_ptr`），于是共享视图一路走到写出去。
  **判据一（确定性）**：`test_web_static_server_func` 的 `loop_stat` 用例多了第 8 条 ——
  再命中一次 `/app.js` 时 `resp.body` 必须与第 2 条**指向同一个 `std::string`**
  （读 `is_shared()` 与 `shared_ref().get()`）；发出去的字节两种实现逐字节相同，这件事
  **只有库内部看得出来**。变异测试：把命中路径改回"临时响应 + 整块赋值"，6 条断言变红、
  变异版退出码 **2**、还原版 **0**。**判据二**：callgrind 每请求指令数（h3 腿
  `-c 16 -m 64 -D 20s`）240 846.3 → **209 438.1（−13.04%）**，其中 Δ 的 **99.7%** 落在
  `__memcpy_avx_unaligned_erms`（−31 305.5 条/请求）上，而 **Δ ÷ 2 = 15 652.8 B** 正好是
  一次响应体的平均字节数 —— 这条差值逐字节等于"两次整块拷贝"。★ **但这不是提速 13%**：
  标定过了，callgrind 把 glibc 的 `rep movsb` 记成**每字节一条 Ir**（16 483 B → 16 515 条），
  而真机上同一段只要 **73 ~ 80 ns**（215 GB/s）⇒ 这一格真实值是 **~0.2 µs/请求（~0.2%）**，
  平台形状的 µs 腿（轮内散度 6.8%、被测效应是它的 1/40）**分辨不出来**，那张表照实记在
  [`doc/benchmark.md`](doc/benchmark.md) §4.9 里但**不作为证据**。附带记下两条机制：
  `uvcpp_buf` 的拷贝构造是**深拷贝且丢共享态**，而 `uvcpp_http_response` 手写了拷贝构造 /
  拷贝赋值 / 析构 ⇒ **没有隐式移动**，`std::move(resp)` 在类型上就退化成一次深拷贝（这正是
  那两次拷贝的来源）；所以 `get_data()` / `get_udata()` 会 `materialize()`，写一个共享视图
  **不可能**污染缓存。⇒ 本条值得单独提交是因为判据确定性、语义逐字不变，**不是**因为涨分；
  它顺带给出的信息更重要：h3 腿每请求指令数里 `memcpy` 占 19.6% 却几乎不花时间，真花钱的是
  分配器那一簇（**14 187 条/请求、6.8%**）与 AES-GCM/QUIC 那条每字节路径
  （`_aesni_ctr32_ghash_6x` **29 745 条/请求**）—— 下一刀砍那里，别继续找"还剩几次整块拷贝"。
  ★ **上一条那句结论的适用范围在 `1.6.28-dev` 里被收窄了**：它是**就 h3 那条腿**说的
  （h3 腿剩下的整块拷贝是每请求 2 次 ≈ 0.2 µs，确实不值得追），**不能外推到 h2 那两条腿** ——
  见下一条。
- **h2 静态腿的 TLS 出口每批拷两遍 → 一遍都不拷**（`1.6.28-dev`）—— `uvcpp_tcp_client::
  tls_flush_out()` 原来两处各拷一遍：① `tls_out_.append()` 把本批密文从 0 长到 ~250 KB，
  每批按 2 倍几何增长；② 交出去时 `uvcpp_buf bufcpp(chunk.data(), chunk.size())` 把
  **整批再拷一遍**。改成 ① `reserve(pending_ciphertext())` 一次到位（用 `reserve` 而不是
  `resize`：后者会把新露出的那段**清零**，等于每批白清 250 KB）、② 用写层已有的零拷贝通道
  `append_uv_buf_view()` 把整批当**共享视图**交出去（`uvcpp_h2_connection::flush()` 的 TLS
  支线用的就是同一个机制）。**为什么先要知道它**：上一轮的采样器把 h2 两条腿上 34.16% /
  34.10% 的样本记给了 `libc.so.6:__nss_database_lookup` —— 那是个**假名字**，真凶是
  `memcpy` 的真实现（`libc+0x188a80`，前面最近的**导出**符号是 `libc+0x188040` 的
  `__nss_database_lookup`，两者之间是 **247 KiB 没有导出符号**的 `.dynsym` 空洞，
  所以任何"最近前一个符号"的解析都会指错）。于是换了一个 `LD_PRELOAD` 的**精确计费**
  装置（按**调用点**分组，不是采样）：同一份 20 文件负载下，h1 明文每请求只拷 **2 711 B**、
  h2 明文 **75 122 B**、h2+TLS **184 141 B**（= 响应体的 11.17 倍）；h1 那条腿证明这件事
  **可以不做**（它的交付路径 `write_owned` 里 **≥16 KB 的拷贝是 0 次**）。**判据一（确定性，
  可复现）**：每请求拷贝字节 **184 141 → 143 461 B（−22.1%）**，另换一轮是 143 029 B
  （两轮差 0.3%）；**变异臂**（只去掉 `reserve`、保留视图）落在中间 **167 641 B**，
  于是 `t0−t2` = 视图那半 **≈16.5 KB/请求**（正好一次响应体：一批密文摊到约 15 个请求上）、
  `t2−t1` = `reserve` 那半 **≈24.2 KB/请求**，两个数加起来正好等于 `t0−t1`。两条独立旁证：
  改动后 `tls_flush_out` 这个调用点在表里**整个消失**（`--top 200` 搜不到），`reserve` 治的
  那两个 `std::string` 站点 `≥16K` 档次数（119 077 / 109 156）在变异臂里**原样回来**
  （114 859 / 106 468）、在改动后掉到 **53 791 / 39 255**。**判据二**：µs 腿**交错**三轮
  （`-m 32 -c 256 -t 8 -D 5s`）**82.1 → 76.0 µs（−6.1 µs，−7.4%，3–0 无交叠）**，
  换算 **6.7 GB/s**，与 §4.3 那条 0.16 ns/B（≈6.2 GB/s）的标定同带。**负对照**：h1 明文两臂
  **逐字节相同**（942 303 166 B）、h2 明文 +0.75%（噪声内）—— 机制上也说得通，
  `tls_flush_out()` 第一行就是 `if (tls_ssl_ == nullptr) return;`。**记一笔踩过的坑**：
  第一次是**非交错**两臂，读出来 +1.2% 像倒退，但同一轮两条负对照也各涨 +1.6% / +1.0%
  ⇒ 那是机器在漂，不是代码在退。真机上的用户态拷贝**确实**是 h2 静态腿的大头
  （`tls_flush_out` 之外，OpenSSL 内存 BIO 两个方向 + `WPACKET_close` + `BUF_MEM_grow_clean`
  各占 9%、合起来 36%，是**架构性**的，本轮不动；h2 明文那条腿上 `uvcpp_h2_connection::
  flush()` 的明文支线是同一形状，占 22.1%，是下一刀）。
  ★ **那条"下一刀"在 `1.6.29-dev` 里落地了**（见下一条）。注意上面把 h2 明文当负对照，
  只在改动**不碰明文支线**时成立，而 1.6.29 动的恰恰就是它。
- **h2 明文腿的 `flush()` 两支线并成一条：交付路径收共享视图**（`1.6.29-dev`）——
  `uvcpp_h2_connection::flush()` 把 `out_` **换**进一份 `shared_ptr<std::string>`（零拷贝），
  但交出去的方式按连接形态二选一：TLS 支线 `write_buf_->share(pending)`（视图），明文支线
  `new uvcpp_buf(pending->data(), pending->size())`（**整块拷一遍**）。明文这一支照抄不了
  TLS，因为 `uvcpp_tcp_client::write(uvcpp_buf*, cb)` 无条件 `buf->out_uv_buf()` **接走块的
  所有权**，而 `out_uv_buf()` 对共享视图会先 `materialize()`（`uvcpp_buf.h` 自己在那一段写着
  "它不是错误，是一次白拷贝"）。⇒ 先让交付路径认 `uvcpp_buf::is_shared()`（与
  `uvcpp_write::adopt_body()` 已经在用的**同一条规则**：共享走 `append_uv_buf_view()` 借出去、
  自有块才 `out_uv_buf()`），再把 `flush()` 的两支并成一条；顺带把这次写的 `get_uv_buf()`/`1`
  换成 `get_uv_bufs()`/`get_uv_nbufs()`，单块与双块走同一个出口。**判据（确定性）**：
  每请求拷贝字节在 h2 明文腿上 **75 078 → 58 866 B（−16 212 B，−21.6%）**，而两条负对照
  **都没动** —— h2+TLS 143 173 → 143 538 B（+0.3%）、h1 明文 2 705.4 → 2 698.9 B（−0.24%），
  机制上也说得通（h2+TLS 本来就走零拷贝的 TLS 支线，h1 腿根本不进
  `uvcpp_h2_connection`）。**旁证**：`t1` 归因表第 2 名正是 `uvcpp_h2_connection::flush()`
  （22 638 次 / 5 599.6 MB / 均 247 356 B / 占该腿 **22.1%**），`t3` 的 `--top 400` 里
  **整个搜不到**；5 599.6 MB ÷ 336 969 = 16 617 B/请求，与被测腿掉的 16 212 B 只差 2.4%。
  **µs 腿如实记**：交错 3 轮分不开（被测腿 −0.6 µs，负对照 −1.6 µs 动得更多），
  h2 明文单腿交错 7 轮得 **68.08 → 66.42 µs（−1.66 µs，−2.4%）**，方向与量级都对
  （16 212 B ÷ 6.7 GB/s ≈ 2.4 µs），但整轮 rps 从 110 467 单调漂到 96 658、7 轮里
  `t3` 更快的只有 4 轮 ⇒ **这一刀的收益就压在噪声带上，判据是字节不是 µs**。
  语义逐字不变：`is_shared()` 为假的 `uvcpp_buf` 仍走 `out_uv_buf()` + `set_uv_buf(raw, true)`；
  视图的存活由 `uvcpp_write` 持有的引用计数履行（与 TLS 支线今天成立的理由相同）；
  `share()` 对空源是 no-op，所以"没有待发帧就不写"这条边界不受影响。
- **h2 两条腿的 `out_`：按上一批大小 `reserve` 一次到位**（`1.6.30-dev`）——
  `flush()` 走 `drain(out_)`，而 `drain()` 是**一段一段 `append`** 进 `out_` 的
  （`nghttp2_session_mem_send2` 每返回一帧追加一次），下一句 `pending->swap(out_)`
  又把整批连缓冲区一起取走交给写层 ⇒ `out_` 每批都从 `capacity() == 0` 重来、
  按 2 倍几何增长。修法是给 `out_` 一个**跨批的容量提示**（上一批的大小，同一个连接上
  批与批高度相关）：`if (out_hint_ > 0) out_.reserve(out_.size() + out_hint_)`，
  上限 1 MiB，超过就不再提示（退化方向是"回到几何增长"，只慢不错）。这个提示
  **不会**把内存钉在连接上：`out_` 的缓冲区每批都被 `swap` 进 `pending` 交给写层，
  函数返回时容量已经回到 0。**判据（确定性）**：每请求拷贝字节 **h2 明文
  58 181.5 → 56 622.5 B（−1 559 B，−2.7%）**、**h2+TLS 142 863.4 → 140 990.7 B
  （−1 873 B，−1.3%）**，两条腿都该掉（两条腿的明文侧都要过 `drain(out_)`），
  负对照 h1 明文 2 701.6 → 2 706.8 B（+0.19%，噪声内，那条腿根本不进
  `uvcpp_h2_connection`）。归因表里 `std::string` 站点 `0x16872f` 掉 550 MB
  （7 508.5 → 6 958.4 MB），其中 `<1K` 档调用 **44 633 → 855（−98%）** —— 正是几何
  增长头几步那几十到几百字节的搬运。µs 一轮非交错读数：h2 明文 112.3 → 109.1、
  h2+TLS 124.0 → 121.8，负对照反着涨 1.0 µs，方向自洽但只当旁证。
  **这一刀顺带证伪了上一条末尾的猜测**：那里把 `0x16872f` 的 39% 猜成"`out_` 的
  几何增长"，按那个模型该掉 ~30 KB/请求，实际只掉 1.6 KB（该站点自身的 7.3%）——
  那一行的大头**不在 `out_` 上**，猜测作废。**下一刀的归因也一并写清**：`cb_read_body`
  的 16.6 KB/请求**不是**会话状态又拷了一份（`stash_body()` 从 1.6.23 起收的就是共享
  句柄、零拷贝），而是 `nghttp2_data_provider2` **接口本身强制**的一次填 `buf`
  （绕开要用 `nghttp2_session_send` + send callback 自己组帧，全库都建在 `mem_send2`
  上，属架构改动，本轮不动）；`0x16a024` 那 25.9% 的站点落在 libstdc++ 内部，
  要继续归因得让量具**多记一层调用点**（`return_address(1)`），而不是接着猜。

- **`uvcpp_web_response::body_take()` / `json_str(std::string&&)`：接管的字节不再
  拷一遍**（`1.6.31-dev`）—— 已有的一族 body 入口全是"拷进来"
  （`body()` → `uvcpp_buf::clone_data()`），最典型的调用点是
  `resp.json_str(build_json(n, m))`：每请求现造的一份 8.4 KB JSON 交进响应时又整块
  拷一次。新入口收**右值**并把那个串的缓冲直接**接管**过来
  （`uvcpp_buf::share()` 的共享手柄，`std::make_shared<const std::string>(std::move(s))`
  只移动、不搬字节）；左值传不进来（编译期错误）—— "共享之后谁都不许写"这条纪律的
  载体是句柄上那个 `const`，不是文档里的一句话。同一个提交补了 `json_str(const char*)`
  （有了 `const&` + `&&` 这一对之后，`json_str("literal")` 在
  「转成 `std::string` 再绑 `const&`」与「再绑 `&&`」之间**没有更优者**，是编译期歧义；
  精确匹配的重载把这个坑填掉，与 `body(const char*, size_t, const char*)` 那一对同源）
  和链式 `cache_tag(std::string)`（契约仍在 `uvcpp_http_response::cache_tag` 一处，
  本函数只把它接到 webapp 的链式 API 上）。
  **判据（确定性，外部量具）**：同一个二进制里只差"这一句怎么交"，
  20000 次响应 —— `memspy`（`LD_PRELOAD` 的 `memcpy` 拦截，按调用点精确记账）
  **拷贝调用 61 400 → 41 400、字节 335 934 791 → 167 994 791**，正好
  每响应少一次 `memcpy(8397)`；`libmcount`（`malloc` 拦截）**调用 21 308 → 41 305、
  字节 +959 128**，即每响应多一次 **48 B** 的控制块分配。**计划里"省一次 malloc"
  那半句是错的** —— 那条路上 `uvcpp_alloc_bytes()` 走企业内存池（
  `UVCPP_ENABLE_MEMORY_POOL`），稳态下池子把 8.4 KB 那块吃掉了，本来就不进
  `malloc`，所以这一刀换到的是"**少搬 8 397 B 字节、多花 48 B 记账**"，不是
  "少两笔分配"。时序旁证（同进程交错、9 轮取中位）：这一步 523.13 → 294.19 ns
  （Δ=229 ns）。回归护栏是 `tests/functional/web_app_response_func.cpp` 的
  `test_body_take_zero_copy()`：接管之后 `body_data()` 必须是**同一块内存**，
  且整条序列化路径上 `uvcpp_buf::share_discard_count()` 必须为 **0**
  （非 0 就是"先共享又要写"的一次白拷贝）；把实现改回 `clone_data()` 这条断言立刻变红。

- **压缩档位成了可配的旋钮，而且它进的是变体缓存的键**（`1.6.33-dev`）—— 此前档位写死
  `Z_DEFAULT_COMPRESSION`。新增 `http_compress::compress(data, len, method, level)`
  重载、`http_compress::clamp_compress_level()`，以及服务器侧
  `uvcpp_http_server::set_compress_level()/compress_level()`；三参数那份重载**逐字节
  不变**（等于显式传默认档），所以不碰它就等于今天。

  **为什么是"进键"**：另外四个压缩旋钮（开关、门槛、排除表、MIME 判据）改的都是
  *要不要压*，做法是"改完要么提前返回、要么查不到"；档位改的是 *压成哪些字节* ——
  压这件事照样发生，于是它只能进键：`编码字符 + 档位字符 + cache_tag`，两个**定长**
  前缀都在前（`-1` → `#`、`0..9` → `'0'+n`），于是 `(编码, 档位, tag) → 键` 是单射。
  换档**不顺手清表**是有意的：清表与进键互相顶替，两条都在的话"档位到底入不入键"
  就再也观测不到了，表里只剩当前档位、把档位从键里删掉照样全绿。

  **顺带修掉一个静默错**：`deflateReset()` 换不了任何*创建内核时定下的*参数 —— 今天
  是窗口与档位两样。槽里只比窗口的话，从 6 档换到 1 档会**继续用 6 档的内核**，产出的
  字节与记录的档位不符；而按档位分键之后，这份错会被**存进 1 档那个键下面**，此后
  1 档那一路拿到的一直是 6 档的字节。

  **读数**（同一台机、zlib 1.3，两条语料）：

  | 语料 | 原文 | 1 档 | 6 档（默认） | 9 档 |
  |---|---:|---:|---:|---:|
  | HttpArena `data/dataset.json` 形状（50 项） | 8 397 B | 1 957 B | 1 566 B | — |
  | `test_compress_level()` 里那份逐行文本 | 69 548 B | 7 062 B | 6 242 B | 5 834 B |

  同一条 8 397 B 上，6 档 → 1 档是 **65.4 → 42.8 µs**（省 22.6 µs），代价是**每响应多
  391 B**。所以这一刀换到的是"**CPU 换带宽**"，不是白赚 —— 值不值看那一条的形状
  （平台 `json-comp` 是每请求 324 µs，才轮得到拿带宽去买）。**默认档一个字节都没动**，
  开不开、开哪一档由部署方或条目自己定。本机 zlib 1.3 上"6 档"与"默认档"逐字节相同，
  但库里把 `-1` 与 `6` 当**两个键**：`Z_DEFAULT_COMPRESSION` 是 zlib 保留改主意的值，
  把 `-1` 归一成 `6` 就等于把那个主意抄进我们的键里，它哪天变了我们会跟着发错字节。

  **判据**是 `tests/functional/web_app_static_func.cpp` 的 `test_compress_level()`，两层。
  ① API 层：三参数与显式默认档逐字节相同、三档产物两两不同且都解得回原文、
  `clamp_compress_level()` 的两端与区间内取值。② 服务器层：在**跑着的实例上**换档
  （默认 → 1 → 9 → 换回 1），五发的字节要两两可分辨，换回 1 档那一发必须拿回第一次
  那份字节（只记"最后一次生效的档位"的实现在这一发上露），计数钉死
  `hits=2 misses=3 stored=3 entries=3`、表内字节总量 == 三发线上长度之和
  （实测 `bytes=19138`，正好 `6242+7062+5834`）。

  两个变异都实测变红：**①** 把档位字符从键里删掉 ⇒ 8 条红，线上字节退化成
  `6242 / 6242 / 6242`；**②** 把槽里的档位比对删掉 ⇒ 5 条红，`entries=3 bytes=18726`
  （三条键装着同一份 6 档字节）。两次变异里"解压回来等于原文"那几条**全绿** —— 这就是
  "每一层看着都对、只有字节数与档位不符"的注脚，也是 ② 的口径只能是**比字节**、
  而且比之前得先钉住"确实走了 gzip"这个前提的原因。

- **静态热路径上的取锁次数：每请求 9 次 → 1 次**（`1.6.36-dev`）。`uvcpp_web_static::Impl`
  里三条改动都是"把锁搬出热路径"，语义一条都不改：**①** `collect_probes()` 从前**每个候选键
  各取一次** `mu_`（一个 URL 一个键，加上索引名与两个压缩后缀，条目那种配置下每请求 6 次），
  现在整段遍历**只取一次**；**②** `retire()` / `drain_retired()` 只在"建桶 / 取桶指针"时进临界区
  —— 桶只被**那一条循环自己的线程**碰（libuv 保证 after_work 回调跑在投递它的那条循环上，
  `drain_retired()` 在 `serve()` 开头同线程调），所以**桶空时一次锁都不取**；`bucket_for()`
  还用线程本地缓存把 `map` 查找一起省掉（桶节点建出来就不再删、地址稳定；缓存靠"`Impl` 身份
  + 单调 `ticket` + 循环指针"三个字段一起校验，防的是 `Impl` 析构后同地址又建一个，而那个
  缓存**只能是 POD** —— emutls 会把非平凡析构的线程本地对象在析构前还回堆）；
  **③** `hits` / `misses` / `rejected` 三个只加不减的计数改 `std::atomic`（`relaxed`）——
  命中路径从前**只为更新计数**进一次临界区。

  **判据是每请求取锁次数**，不是"看着更快"。量法：`LD_PRELOAD` 一个只拦
  `pthread_mutex_lock` / `pthread_mutex_trylock` 的探针（原子总数 + 按调用点直方图，收到
  `SIGUSR1` 打一次快照），**同一个入口二进制、只换 `libuvcpp.so`**（改动前 / 改动后），
  每臂先热一遍缓存（4000 请求）让计时窗口里只剩命中，窗口固定 20000 请求，交替 A/B/A/B
  两轮：**31.23 → 22.98 次/请求（−8.26，−26%）**。把直方图按 `/proc/<pid>/maps` + `nm`
  归到函数上，静态处理器自己那一段在改动前是 `add_probe` 6.000 + `finish_job` 1.000 +
  after_work 闭包 1.000 + `serve` 1.000 = **9.000/req**，改动后是 `collect_probes`
  **1.000/req**（−8.000），与总差的 −7.98 / −8.53 对得上（差的那点是本次没动的项在两次
  跑之间的抖动）。剩下的 22.98 里 17.63 是 `uvcpp_web_app::slot_here()`（`loops_.size() > 1`
  时逐个槽位取 `tid_mutex` 找本线程）、3.00 是 libuv 自己的 `uv_mutex_lock`、1.00 是
  `on_loop_thread()` —— 本次没动，而 `slot_here()` 那一条已经比整条静态腿还贵。

  **判据为什么不是 futex 次数**：`strace` 在这件事上量不出诚实的数 —— 无人争用的
  `std::mutex` 根本不进内核（futex 只在真要排队时才发生），而 `strace` 自己的减速又会
  改变争用。上面这个计数器量的是**取锁次数**，每次调用必计、与机器负载无关，正是
  "这段代码要不要为锁付出代价"的直接读数。

  `us_per_req` 侧**不劣化**：平台 static-h2 形状（`-c 1024 -m 32`）交替 4 轮，合计
  `utime+stime` ÷ 2xx 是 **93.6 → 89.6 µs**（−4.3%）；单轮配对比较的符号来回翻
  （同一臂的四个单轮从 78.5 抖到 116.3 µs）⇒ 这台机器在这个形状上分辨不出 4% 以内的差，
  能说的只有"没劣化"。语义面：全量 `ctest` 绿，静态腿的逐字节断言一条没动。

- **`slot_here()`：上一版剩下的那 18.4 次/请求**（`1.6.37-dev`）。上一条把静态处理器
  自己的取锁从 9.00/req 压到 1.00/req 之后，直方图上的第一名变成了
  `uvcpp_web_app::slot_here()` —— **18.392 次/请求**，一个人比整条静态腿还贵。它在问
  "这个槽位跑的是不是我这条线程"，而问法是**逐个槽位**：每个 `loop_slot` 取一次
  `s.tid_mutex`（条目那种 32 循环的档位下每调用约 16.5 次），而它在**每个请求**上都被
  调一次（`reg_here()` 那条路）。

  改法：新增私有 `slot_here_or_null()`，用**线程本地缓存**记下"本线程属于哪一格"，
  命中时**一次锁都不取**。缓存拿三个字段一起校验：`this` 指针、`loops_[0]->ticket`、
  缓存本身非空。

  `ticket` 是为一种情形专设的：一个 `uvcpp_web_app` 析构后**在同一个地址上又建一个**，
  它的格子会照同一条分配序列落回同一批地址（`new loop_slot(i)` 大小分档与序列都一样），
  于是"实例地址 + 格子地址"这一对**原样复现** —— 只比地址的实现会把新一代的线程认成
  上一代那格。票号是每个 `loop_slot` 一个、函数内 `static std::atomic` 发出的
  **全进程单调、从 1 起、不复用**的序号，`loops_[0]` 的票号一变就说明换代了。读它
  不需要锁：构造函数里写一次，那时对象还没交给任何别的线程。

  `on_loop_thread()` 改成直接调 `slot_here_or_null()`（它原来是先 `slot_here()` 再取
  一次 `tid_mutex`，两个加起来 19.392/req）。`slot_here()` 保留原语义：`n == 1` 时
  恒等且不加锁，不在任何循环线程上时回落 0 号（设计稿 §5.2）。缓存**只缓存命中、
  不缓存未命中** —— 钩子跑的那一刻 `tid_known` 还是假，那条"哪儿都不在"的答案一旦
  记下来就再也翻不了身（设计稿 §4.1.1）。缓存结构**只能是 POD**，同 `bucket_for()`：
  emutls 会把非平凡析构的线程本地对象在 `__cxa_thread_atexit` 的析构器跑之前还回堆。

  **判据还是上一把尺子**（`LD_PRELOAD` 计 `pthread_mutex_lock` 取锁次数，同一个入口
  二进制、只换 `libuvcpp.so`，先 4000 请求热缓存，窗口 20000 请求）。同一轮里两臂：

  | 取锁点 | 改动前（`1.6.36-dev`） | 改动后 | 直方图里的归属 |
  |---|---|---|---|
  | `uvcpp_web_app::slot_here()` | 18.392/req | **0.000** | 改动后**整个消失** |
  | `uvcpp_web_app::on_loop_thread()` | 1.000/req | **0.000** | 改动后**整个消失** |
  | libuv `uv_mutex_lock` | 3.003/req | 3.003/req | 没动 |
  | `Impl::collect_probes` | 1.000/req | 1.000/req | 上一条的成果 |
  | 其余零头（`put` / `work_limit::release` / 四个 `0.013`） | 0.080/req | 0.091/req | 残留 |
  | **合计** | **23.475/req** | **4.094/req** | **−19.381（−82.6%）** |

  两条改动项是**整个消失**（不是变小），与总差的 −19.38 逐位对得上；剩下那 4.09 里
  3.00 是 libuv 自己的锁、1.00 是上一条刚压到 1 次的 `collect_probes`，也就是说
  **静态热路径上本库自己的取锁只剩 1 次/请求**。

  **这条缓存的哪一半有牙、哪一半没有**，如实写在这里：实例内部它可证永远正确
  （运行期槽位不搬家、不重排），唯一的风险是上面那种**跨实例同地址复用**，而它要求
  "同一条 OS 线程先后服务两个 app"**且** glibc 把一块没重新初始化的 TLS 块还回来 ——
  本机构造不出确定性的红。所以 `tests/tools/multiloop_mutation.py` 新增的 **M12** 打的是
  **命中分支**（让它返回 0 号槽位），靠既有的按循环分片断言变红：实测 **8 条**
  （`每条连接都记在它自己那一格上（对不上 16 / 16）`、`id 的高段 == client->loop_index()
  （不一致 16 条）`、`显式轮转：16 条分给 2 条工作循环必然 {8,8}，实际 {0,0}` 等），
  M0 对照组绿、跑完源码按字节还原。`owner` + `ticket` 那一层**是论证，不是测试** ——
  写在这里就是为了别让它哪天被当成"验过了"。

  顺带把那个脚本搬到了 Linux 上：它原先只认作者的 MSVC 多配置树（`Release/` 里的
  `.exe` 加一份要手工同步的 `uvcpp.dll`），而这条线的量测都在 Linux 做。那边是单配置树，
  可执行文件就在 `<tree>/tests/functional/`、自己 rpath 到 `<tree>/libuvcpp.so`，所以
  "测试侧 DLL 停在上一版"那个坑不存在（同步那步变成 no-op）；错误计数也补上了
  `error:` / `Error N` 这两种 GCC/make 的形状。

- **静态热路径的 `realpath`：每请求 2.734 次 → 0.001 次**（`1.6.38-dev`）。
  `uvcpp_web_static` 每次请求都要为 `web_resolve_within_root()` 做一次 `realpath` 才能
  判定"这个落点在不在文档根内"——而 `realpath` 要沿路径**逐分量 `lstat`**。混载量到的是
  **2.734 次 `realpath`/请求**，这是这条热路径上最后、也是最大的一笔文件系统开销。

  改法是把上一次的解析结论记下来（`resolve_hint`：展开后的绝对路径 `fs_path` +
  `st_dev`/`st_ino`），命中时只做**一次 `stat`** 比对指纹，不再 `realpath`。这一份记忆
  与 LRU 条目表**分开**存（键是 URL 而不是 `(路径, mtime, size)`）：负数结论（"这条路径
  上什么都没有"）也要记，否则没有孪生文件的资源每请求都要重新解析
  `url + ".br"` / `url + ".gz"` 这两个**注定失败**的候选 —— 只记正数的那一版实测
  `realpath`/请求只掉到 0.980，正是漏在这一处。

  **判据是每请求的文件系统系统调用次数**，不是"看着更快"。量法：`LD_PRELOAD` 一个拦
  `syscall()`（解出 `statx`/`newfstatat`/`openat`/`readlinkat`）与 `lstat`/`stat`/
  `readlink`/`realpath` 的探针，**同一个入口二进制、只换 `libuvcpp.so`**，先 4000 请求
  热缓存，窗口 20000 请求、每臂两轮（20 个 URL 的混载，`-c 256 -m 32`，ext4 语料）：

  | 臂 | 库 | 入口 `resolve_memo` | `realpath`/req | `statx`/req | 小计/req |
  |---|---|---|---|---|---|
  | C（改动前的整对） | 旧 | — | 2.734 / 2.734 | 2.245 / 2.245 | 4.994 / 5.033 |
  | D | 新 | `true`（默认） | **0.001 / 0.001** | **2.245** / 2.245 | **2.285 / 2.272** |
  | E | 新 | `false` | 2.734 / 2.734 | 2.245 / 2.245 | 5.013 / 4.993 |

  E 是**关掉那个开关**的同一份库：它的系统调用序列与 C 逐列相同 ⇒ "关掉 = 回到不做记忆
  时"这句话是量出来的，不是推的。`statx` 一模一样是**设计如此**——慢路径本来也要 `stat`
  一次拿 mtime/size，快路径那次指纹 `stat` 是把它**换掉**而不是加上去，所以这一步是纯赚。

  这两列数还逐条对得上一个模型（按 URL 拆开量出来的）：带孪生文件的 15 个 URL 是
  `2 realpath + 2 statx`（主文件 1+1，孪生候选命中 1+1），没有孪生的 5 个是
  `5 realpath + 3 statx` —— 因为 `web_real_path(allow_missing=true)` 在整条路径
  `realpath` 失败后**还要再解析一次父目录**（`src/webapp/uvcpp_web_util.cpp:894-894`、`src/webapp/uvcpp_web_util.cpp:911-911`），所以一次
  **失败**的解析是 2 次 `realpath` 而不是 1 次。加权 `(15×2+5×5)/20 = 2.75` 与
  `(15×2+5×3)/20 = 2.25`，与实测的 2.734 / 2.245 对上。

  **为什么这不是"拿路径文本当结论"**：`stat` 与 `realpath` 走的是**同一条 VFS 解析**，
  `(dev, ino)` 没变 ⟺ 还是那个文件 ⟺ 当初"它在根内"那个判断仍然是**对这个文件**的判断。
  新增用例 `test_resolve_hint()`（`tests/functional/web_app_static_func.cpp`）测的正是
  两者不再等价的那一刻：`/hintdir/a.txt` 先正常服务一次（烧进记忆），然后把 `/hintdir`
  **换成指向根外的链接**（路径文本一个字都没变），下一个请求必须重新解析并拒绝；对照组是
  同一棵根上 `resolve_memo = false` 的挂载，它同样拒绝。**变异实测变红**：把指纹比对去掉
  （`(dev == h->dev && ino == h->ino) || true`）⇒ 2 条红，而且红在**内容**上
  （`响应体里出现了 [OUTSIDE-AFTER-DIR-REPOINT]`）—— 根外那个同名文件的字节真的发出去了，
  不是红在状态码上。

  **已知且接受的一处行为差异**（如实写在这里，别当成"没有"）：路径的落点被整体搬到根外、
  原位留下一个指向**同一个 inode** 的链接时（`mv` 到根外 + `ln -s` 回原位），指纹不变 ⇒
  命中路径继续按原 URL 服务它，而完整解析会判越界。差别是"曾经被授权过的那份字节多活
  一会儿"，不是读到没被授权过的字节；`rm` 立即生效，`validate.sh` 的 staleness 探针用
  `mv -f` 换文件（inode 必变）也不受影响。要绝对严格就关掉 `resolve_memo`（一个字段）。

  `us_per_req` 侧**不劣化**：平台 static-h2 形状（`-c 1024 -m 32`）按 C D E E D C 的顺序
  各跑 4 轮取中位数，D **87.9 µs** 对 C 92.2 / E 98.1 —— 但**同一臂内部的散度是
  ±8%**（C 的四轮从 82.5 抖到 97.9），比臂间差还大，所以这台机器在这个形状上只能支持
  "没劣化"这一句，不能声称百分比。能声称的是上面那张系统调用表。

- **静态热路径的每请求分配：ETag / Content-Type / 压缩指纹不再各造一串临时 `std::string`**
  （`1.6.39-dev`）。命中路径上每请求的 `malloc`/`new` 总数实测 **98.6** 次 —— 这个数
  本身就是这条线索的起点：它不是"这里少一次那里少一次"能看出来的，得先量出来。三处在
  这一版里改成**不落堆**：`make_etag()` 那条 `std::string("\"") + hex + "-" + hex +
  "-" + hex + "\""` 的五段拼接链；`content_type_for()` 的"先构造再追加
  `; charset=utf-8`"；以及 `resp.raw().cache_tag` 那条四段的四则运算式拼接。

  改法分别是：`format_etag_into(char*, size_t, ...)` 写进调用方的栈缓冲区
  （`finish_job()` 里 `char etag_buf[64]`），只有客户端真的带了 `If-None-Match` /
  `If-Range` 时才就地建一个 `std::string` 去比 —— 那两条路在一次压测里是 0 次；
  `content_type_for()` 一趟 `std::strstr` + 栈上 64 字节拼好再构造一次（`find()` 会为
  那个 `const char*` 造临时 `std::string`，正好是要省掉的那一次）；`cache_tag` 改成
  `clear()` + 一次 `reserve()` + 几次 `append()`，三个整数走 `append_i64_dec()`
  （`std::to_string` 返回的串对这几个长度本来就走 SSO，白拷一遍）。

  **判据是每请求的分配次数**（`LD_PRELOAD` 一个 `malloc`/`calloc`/`realloc` 计数器，
  窗口前后各发一次 `SIGUSR1` 取差）。同一个入口二进制、只换 `libuvcpp.so`；先 4000 请求
  热缓存，窗口 **20000 请求**（固定请求数而不是固定时长 —— 固定时长时两个进程拿到的
  请求数会差 4%，每连接那些固定分配除以不同的分母，比值就跟着漂），每臂三轮：

  | 臂 | 库 | `malloc`/req（三轮） | 中位数 |
  |---|---|---|---|
  | PRE3 | `HEAD`（1.6.38-dev） | 98.955 / 98.129 / 98.611 | 98.611 |
  | C3 | 本版 | 97.300 / 97.170 / 97.418 | **97.300** |

  **如实写：这条没打到原定的 ≥3，实测 −1.31/req（−1.3%）**，散度 ±0.2 所以这个差是
  真的、但比设计时估的小。原因是估的时候把三处都按"每段拼接各落一次堆"算了，而实际：
  ①`i64_to_hex()` 的返回值（4~8 个字符）与 `std::to_string` 一样走 **SSO**，不落堆；
  ②libstdc++ 的 `operator+(string&&, ...)` 是**就地追加**，那条链只在跨过 15 字节那一步
  落一次堆、之后按几何增长再 realloc 一两次；③`content_type_for()` 里 `std::string ct = t`
  对 `text/html` 这种 9 个字符的类型名也是 SSO —— 它本来就只有"追加上去"那一次分配，
  而新写法"栈上拼好再构造"同样是一次，这一处实际是**零收益**（但也没有多付）。
  留存这一版是因为三处都严格不劣、且 ETag 那处是真省，但**"少造临时串能大幅降分配"
  这个判断在 SSO + 就地追加的 libstdc++ 上不成立** —— 这一句记在这里，免得下一个人
  再按同样的直觉去估。

  语义一个字没改：Content-Type / ETag / `cache_tag` 三个值**逐字节相同**
  （`cache_tag` 的拼接顺序与分隔符 `\x1f` 原样保留，它是压缩缓存的键）。
  `test_web_app_static_func`（约 18 s）与 `test_web_static_server_func`（约 7 s）全绿。

- **静态热路径上的 `job` 对象改走 per-loop 回收站**（`1.6.40-dev`）。上一条量到的
  每请求分配是 **98.6** 次，其中最大的一块是"每请求 `new job()` / `delete j`" ——
  而 `job` 不是个小对象：它身上挂着 6 个 `std::string`（`url`/`final_url`/`fs_path`/
  `mime_path`/`try_variants[2]`/`variant`）、3 个 `vector`（`probes`/`hints`/
  `fresh_hints`），外加两个 `shared_ptr`。`new job()` 只是第一笔；真正贵的是**每一次
  请求都要把这些容器重新长到够用**（`url` 装下一条 URL、`hints` 装下这一批候选键
  ……），而那些容量在请求结束时全被 `~job` 还给了堆。

  改法是按循环分桶的回收站：`job` 用完不 `delete`，而是就地 `reset()` 之后挂进那条
  循环的 `free_jobs`（上限 `k_free_cap = 512`，超了照旧 `delete`）；下一个请求
  `take_job()` 从这里取。`reset()` 用 `clear()` 而**不是** `swap` / `shrink_to_fit()`
  —— 这是整条改动的要点，容量必须留着，还回去就等于没做。松引用的时刻与 `~job`
  **逐条一致**：`next`（那份让链挂起的上下文引用）与 `data`（响应体）都在 `reset()`
  里松开，而调用点就是原来 `delete j` 的那三处 —— 这两份引用按着的是上下文与响应体，
  "晚一拍松"会把已经结束的连接资源多拖一会儿。

  回收站复用的是 `bucket_for()` 那套（桶地址恒定 + 本线程 `thread_local` 缓存），
  所以它的读写**一次锁都不取**。

  判据还是每请求的分配次数（同一把尺子、同一个入口二进制、只换 `libuvcpp.so`，
  窗口 20000 请求，每臂三轮）：

  | 臂 | 库 | `malloc`/req（三轮） | 中位数 |
  |---|---|---|---|
  | C3 | `HEAD`（1.6.39-dev） | 97.041 / 97.392 / 97.386 | 97.386 |
  | C4 | 本版 | 91.696 / 92.408 / 92.322 | **92.322** |

  **−5.06/req（−5.2%）**，高于原定的 ≥2。这里要点明的是：收益主要**不在那一次
  `new`**，而在容器的容量不再每请求重建 —— 单看对象本身只值 1 次分配。

  **`uvcpp_work` 那一半这一版没做，理由是硬的**（同样写在 `retire_bucket` 的注释里）：
  `uvcpp_work::queue_work()` 在 `loop != nullptr` 时**抛异常**（`src/req/uvcpp_work.cpp`
  的 `"error,local loop had value"`），而那个 `loop` 是私有成员、缓存之后没有任何公开
  路径能清掉。复用一个用过的 `uvcpp_work` 会让每个请求都在 `queue_work()` 上抛出去
  （实测：静态用例整组 500）。要做就得先给那个核心类加一条"可以再次 `queue_work()`"
  的契约 —— 那是另一处改动、另一套判据，不塞进这一版。

  ASAN/UBSAN（`build-c4san`：RelWithDebInfo + `-fsanitize=address,undefined`）：
  `test_web_app_static_func` 的泄漏足迹与只带 C3 的对照**逐字节相同**（12477 字节 /
  16 次分配 —— 这是框架侧 `uvcpp_web_context::create` 的既存泄漏，与本次改动无关），
  两臂都**没有一条 `[FAIL]` 断言**。`test_web_static_server_func` 另报 7796 字节 /
  58 次分配：那条腿走的是 `src/web/uvcpp_static_server.cpp`（h3 腿），本版只动了
  `src/webapp/uvcpp_web_static.cpp`，而那个用例连 `webapp/uvcpp_web_static.h` 都没
  include ⇒ 不是本次引入的。

- **h2 每批最多吐 64 KiB —— `static-h2` 那 2.7 GiB 常驻是从"批次大小"里来的**（`1.6.41-dev`）。
  `uvcpp_h2_connection::flush()` 原来是"把 nghttp2 里所有就绪的帧一次榨干"再交给
  `uvcpp_tcp_client::write()`。一批有多大，取决的不是本库，而是**对端通告的窗口** ——
  而 `h2load` 的 `-w`/`-W` 默认都是 2^30−1（约 1 GiB）。于是平台那一格（1024 连接 /
  `-m 32`）下，服务端会把一条连接上所有就绪的响应体攒成**一批约 500 KiB** 一次推出去：
  一批 = 32 条流 × 约 16.5 KiB。

  这批字节在三个地方各留一份，而且**每一份都活到连接结束**：`SSL_write` 把它们拷进写
  BIO 的 `BUF_MEM`（`ssl3_write_bytes` → `tls_retry_write_records` → `BIO_write` →
  `mem_write` → `BUF_MEM_grow_clean` → `CRYPTO_clear_realloc`），而
  `BUF_MEM_grow_clean` **只增不减** —— 一条连接历史上最大的一批就是它容量的下限；
  同一批还作为 `pending` / `write_buf_` 活在 `flush()` 与 `on_write_done()` 之间。

  归因读数（jemalloc 5.3 在**峰值时刻** dump：压测第 25 s，`h2load` 仍在跑，
  `lg_prof_sample:19`）：活堆 816.8 MiB，其中 **470.0 MiB（58%）** 是上面那条写 BIO 的
  `BUF_MEM_grow_clean`，**344.3 MiB（42%）** 是 `flush()` 里 `out_.reserve` 出来的那一批，
  其余全部加起来不到 1 MiB。**不是泄漏**：压测停下 5 s 后再 dump，活堆只剩约 18 MB；
  RSS 不回落是分配器的保留页。

  修法只有一处：`uvcpp_h2_session::drain(std::string& out, size_t max_bytes = 0)` 多一个
  上限，`uvcpp_h2_connection::flush()` 传 `k_max_drain_batch = 64 KiB`。断点**只判"攒够
  没有"，不去切帧** —— `mem_send` 一次给一整帧，切了得自己拼回来，而"没榨完的留在
  nghttp2 自己的出站队列里"本来就是合法状态（下一次 `drain()` 接着吐，`on_write_done()`
  会再冲一次），所以一批最多超出上限**一帧**。上限做在会话里而不是调用方：只有那里知道
  "一批"在哪断，而断点必须落在帧边界上。**它也不是"每连接每请求的上限"** —— 一个发送
  缓冲封 64 KiB，与 `H3_MAX_WRITE_VECS = 16` 是同一种量。

  判据用**分配器自己的精确账**（`mallctl` 的 `stats.allocated` / `active` / `resident`，
  没有抽样误差；同一形状 c1024/`-m 32`、同一时刻 t=25 s、同一条 30 s 压测）：

  | 指标 | 改动前（`1.6.40-dev`） | 本版 | 变化 |
  |---|---|---|---|
  | `Allocated`（活堆） | 1472.8 MiB | 382.0 MiB | **−74.1%** |
  | `active` | 1551.0 MiB | 451.6 MiB | −70.9% |
  | `resident` | 2206.4 MiB | 634.4 MiB | −71.3% |
  | `mapped` | 2266.5 MiB | 698.4 MiB | −69.2% |
  | `retained`（脏页） | 902.5 MiB | 319.6 MiB | −64.6% |

  另一台装置（只看内核的 `VmHWM`，不预载 jemalloc）同一批形状：平台那一格
  **2890.3 → 724.1 MiB**（rps 63,366 → 71,158），256 连接档 **766.0 → 215.6 MiB**
  （63,584 → 69,029），同连接数但不流水线（`-m 1`）492.6 → 436.1 MiB（42,896 → 47,661）。
  不经过静态处理器的对照档 144.2 → 145.1 MiB —— 这一刀只对"多路复用攒批"这个形状生效。

  CPU 上它**不花钱**：交替起服务（改动前 / 本版 轮流）各取 9 个读数，用
  `/proc/<pid>/stat` 的 (utime+stime) ÷ 2xx，**配对差中位 −2.24 µs/请求**
  （三对 −2.56 / −4.20 / −0.84）。单臂连着跑会把机器漂移读成效果（同一臂内部极差 9%），
  所以这一条只认配对差、不认两次中位数之差。

  **留一句老实话**：封顶之后写 BIO 那一栈仍是剖析里最大的单条（约占采样总量的 60%），
  只是绝对值掉了一个数量级；残余的每连接约 373 KiB 里，大头是**每连接固定缓冲**
  （TLS 16 KiB 读 + 16 KiB 写 + nghttp2 出站 buf，约 56 KiB/连接）与分配器保留，
  不是某一次拷贝。**没有**再做"写 BIO 排空后换一个 `BIO_s_mem()`"：那最多省
  64 KiB × 1024 ≈ 64 MiB（HWM 的 ≤9%），代价是给 TLS 写路径加一条新的失败模式，不值。

  采样剖析的绝对量**不能当尺子**：同一份 dump 按 jemalloc 自己存的缩放值求和，比
  `Allocated` 低 3.5×（`lg_prof_sample:16`）到 9.7×（`:19`）—— 它只用来看**排序**，
  总量一律取 `mallctl`。

- **静态整读路径的工作池名额：记账搬到 worker，派发留在 loop 线程 —— 高负载下那批假 503 归零**
  （`1.6.42-dev`）。这是 2026-09-24 那次"分片路改成按块借还"（`1.3.6-dev`）的**后半**，
  两条路是同一个病理、只差半句话：

  | 路 | 改动前 | 病灶 |
  |---|---|---|
  | 分片（1.3.6-dev 已修） | 一个名额从 worker 持到 `after_work` | 持有着是"一个字节都还没读"的请求 |
  | 整读（本版） | 同上 | 持有着是"**已经读完**、只是在等循环线程"的请求 |

  `after_work` 跑在**循环线程**上，而循环线程正是高负载下最忙的那一根。于是名额被
  一批"活儿已经干完"的请求攥着：`in_flight` 顶到上限（`UV_THREADPOOL_SIZE=64` 时是
  256），而真正在读盘的只有 ≤64 个，后来者一律被拒。服务端日志的现场是
  `工作池已满（在途 256 / 256）`，与分片那条路当年的 `在途 0 / 16` 是同一类读数 ——
  都是"账面上的在途"与实际并行度脱钩。

  **修法不是"在 worker 上直接 `release()`"。** 唤醒回调必须在**注册它的那根线程**上
  跑：现有两个注册方（`uvcpp_web_file.cpp` 的分片块闸门 → `resume()`、
  `uvcpp_web_upload.cpp` → `on_slot_available()`）回调里都要续做 loop 上的 I/O，
  从 worker 线程调 `release()` 等于让那些回调跑在 worker 上，那是数据竞争不是慢一点。
  所以把 `uvcpp_web_work_limit` 的"记账"与"派发"拆开：新增 `release_deferred()`
  （**当场记账**，返回"欠不欠一次补派"）与 `dispatch_wakeups()`（补派，只在 loop 线程
  上调）；`release()` 变成两者的组合，语义逐字不变。静态整读的 `run_job()` 在
  **worker 上读完当场** `release_deferred()`，同一笔任务在 loop 线程上的 `after_work`
  补那一刀。

  **不会丢唤醒**（这条是改动能否成立的关键）：等待者只在"在途数已达上限"时才注册，
  那一刻必然**至少有一个在途任务还没还过名额**，它结算时的检查必在注册之后 ⇒ 一定
  看得见这个等待者。与 `release()` 里那句"漏掉这一次唤醒也不会丢事"是同一个论证。

  判据分两层。**确定性的一层**是 `tests/functional/web_app_worklimit_func.cpp` 新增的
  两组：`release_deferred_split` 钉闸门自身（记账当场发生、派发不许在
  `release_deferred()` 的栈上发生、满员时两种还法都不许叫），
  `worker_release_flushed_by_after_work` 钉**接线** —— 上限压到 1、测试线程故意只
  `release_deferred()` 不补派，然后让一个**冷文件**的整读请求去占名额；那条挂住的
  分片传输**只能**被整读任务 `after_work` 里那一刀叫醒。**变异实测变红**：把
  `after_work` 里那两行删掉 ⇒ 该组 2 条红（`整读那次 after_work 的补派必须叫醒挂住的
  分片请求`、`被唤醒之后那条一次性条目应当已被消费掉`），而在此之前，这个接线**一条
  用例都没盖**——删掉它没有任何测试会红，症状却是那条响应再也不来。

  **读数**（本机 8 核、同一份 ext4 语料、同一形状：1024 连接 × `-m 32`、20 个 URI、
  `br;q=1, gzip;q=0.8`、20 s、`UV_THREADPOOL_SIZE=64`）：改动前 / 本版**交替**各跑
  3 轮，5xx **17,883 / 14,747 / 15,126**（约占 2xx 的 **0.94%**）对 **0 / 0 / 0**；
  2xx 1,663,894 / 1,648,941 / 1,635,661 对 1,643,319 / 1,638,128 / 1,658,477 ——
  差异在噪声内（先单跑一轮看到 −2.5% 就当成劣化，交替三轮才知道那是漂移）。
  0.94% 听着不大，但它是**同一台服务在同一个形状下的稳定拒绝**，不是抖动；
  而这一刀同时让名额早一拍变松（后来者不必等那根最忙的线程）。

  顺带记一条**没做的事**：量这个形状时先想用 `uvcpp_http_client::send_wait()` 的
  超时当尺子，实测它在这台机器上**不生效** —— 头文件已经写明那个 `timeout_ms` 靠
  Windows 的 `SO_RCVTIMEO`/`SO_SNDTIMEO` 实现、POSIX 上"传入的值会被忽略"，所以
  新用例的红绿判据建在**有界轮询**上，而不是"等对端超时"（否则变异下是挂住而不是
  变红）。这条既有缺口本版没动，如实留在头文件里。

- **文档与 1.6.42-dev 的行为对齐**（`1.6.43-dev`）。纯文档/注释，**零行为改动**，四处：
  - `src/webapp/uvcpp_web_static.h` 的"缓存命中也要过一次线程池"那句是 `1.6.24-dev`
    写下的，只在那一档成立。改成按 `uvcpp_web_revalidate` 分档说：`POOL`（默认）进
    线程池、换来"校验一定发生在文件系统上"且不挡同循环的其它连接；`LOOP_STAT` 把
    那次 `stat` 搬到循环线程、省掉每请求一次提交 + 唤醒 + `after_work` 三次跨线程
    往返，代价是阻塞 `stat`（ext4 微秒级、FUSE 46 µs 那条读数）。**结尾写死一句
    "两种档的语义逐字相同"** —— 这是两档都合规的那个依据，不能靠读者自己推。
  - `doc/webapp-guide.md` 的"工作池名额"一条补上 `1.6.42-dev` 的记账位置，并把
    "唤醒回调在 `release()` 的调用栈上"扩成"**或 `dispatch_wakeups()`**，
    也就是**注册它的那根线程**"；`doc/benchmark.md` 新增 §4.15（第十套装置）。
  - `doc/webapp-support-guide.md` 里"库内两处**归还点**都在循环线程上（`after_work`
    的整读路径、`on_read_done` 的块级路径）"这句在本版**变成错的** —— 整读那条的
    **记账**已经搬到 worker。改成"两处**派发点**都在循环线程上"，并把 `1.6.42-dev`
    那半句注在括号里。这一条不是措辞问题：`set_chunk_gate()` 那段论证全靠"回调会和
    同循环的 `cancel()` 抢同一个对象"这一句撑着，而它成立的依据是**派发**在哪根
    线程上，不是记账在哪根线程上。

- **h3 那条静态腿也填 `resp.cache_tag` 了，缓存补上界**（`1.6.44-dev`）。`uvcpp_static_server`
  与 `uvcpp_web_static` 是两份实现，后者从 `1.6.23-dev`（`57c91b5`，压缩变体缓存那次）起
  就在响应里填这个指纹，前者一直没填。而 `uvcpp_http_server` 的**压缩变体缓存**判定的就是这个字段（`apply_compression_for`，
  键 = 编码字符 + 档位字符 + `cache_tag`，见 `uvcpp_http_response.h` 的唯一契约）：没有它
  就等于每请求把同一份字节重压一遍。于是同一条腿在两个开关组合下差了近两个数量级 ——

  | 本选项（发盘上孪生） | 填 `resp.cache_tag` | rps（中位） | 8000 请求送出字节 |
  |---|---|---|---|
  | 关 | 否（`1.6.44-dev` **之前**的形状） | 977.00（8.19 s） | 173,843,446 |
  | 关 | 是（`1.6.44-dev` 起） | 66,554.63（120.20 ms） | 173,843,446 |
  | 开 | 是 | **89,791.80**（89.10 ms） | 131,194,904 |

  同一份源码、只把填指纹那一句关掉另编一个 `.so` 的消融臂量到 **68.1 倍**，而"开孪生"
  这一项自身只值 **+34.9%**（同时线上字节 −24.5%）—— 也就是说此前那条"关掉孪生反而更快"
  的读数，主因是缺指纹而不是选项本身。指纹是**文件系统身份**（`路径 \x1f 字节数 \x1f
  mtime 秒 \x1f mtime 纳秒`）而不是 URL 身份，所以 `/` 与 `/index.html` 必须共用同一个；
  发盘上孪生时**不填**（字节不是这个文件的字节）。
- **`uvcpp_static_server` 的缓存有界了**（`1.6.44-dev`）。此前那张表只有插入、没有淘汰，
  也没有上限 —— 每访问过一个文件就常驻一份字节，直到 `clear_cache()`。现在照
  `uvcpp_web_static` 同一形状补了 `set_cache_max_entries()`（默认 1024）与
  `set_cache_max_bytes()`（默认 32 MiB，**`0` = 不限**），LRU 用 `std::list` + 条目的
  迭代器位置，命中即 `splice` 到表头、淘汰从表尾出；**单份字节超过字节预算的条目根本不进表**
  （否则一条大文件就会把整张表清空），`cache_size()`/`cache_bytes()`/`clear_cache()`
  的语义不变，只是现在会随淘汰回落。顺带修掉一个**插入闸门**：`set_cache_enabled(false)`
  此前只挡住命中判定，插入路径没看它，于是"关掉缓存"之后表仍在长。
- **新增用例 `cache_tag_and_bounds`**（`tests/functional/web_static_server_func.cpp`）。
  分五段：① 未命中时 `deferred == true` 且指纹为空、命中时四段齐全且 `/` 与 `/index.html`
  给出**同一个**指纹、改写文件后指纹必须变；② 发盘上孪生时指纹必须为空；③
  `cache_max_entries = 2` 下六步的命中/未命中序列与"任意时刻 `cache_size() <= 2`"；
  ④ `cache_enabled = false` 后表恒为 0；⑤ `cache_max_bytes` 小于某个文件时该文件永不进表、
  也永不挤掉已在表里的小文件。这四条边界各做过一次变异（去掉孪生守卫、去掉淘汰调用、
  去掉插入闸门、指纹退化成 URL 身份），**每条都能让用例变红并给出对应的断言消息**，
  改回即绿 —— 也就是说这套断言是承重的，不是"跟着实现写一遍"。

- **静态腿的池规模不是杠杆（量测结论，`1.6.45-dev`）**。纯文档，**零行为改动**。
  `doc/web-http-guide.md` §9 那节写着「一次池往返十几微秒」，很容易被读成「那就把池开小」。
  本机拿同一份 `.so`、同一份 `main.cc`（只差 `UV_THREADPOOL_SIZE` 一个环境变量）做过一次
  2×2，`static-h2` 形状（20 个 URI、`-c 256 -m 32 -D 5s`、ext4 语料、每格 3 次取中位数，
  单位 µs/req）：

  | 校验档 | 池 8 线程 | 池 64 线程 |
  |---|---|---|
  | `POOL`（默认） | 106.1 / 113.6 / 111.3（中位 **111.3**） | 109.0 / 109.4 / 109.9（中位 **109.4**） |
  | `LOOP_STAT` | 84.2 / 88.7 / 87.1（中位 **87.1**） | 87.9 / 95.3 / 89.6（中位 **89.6**） |

  **池 8 与池 64 之间只差 1.9~2.5 µs，落在 5~8 µs 的次间散布里**（8 核上 256 连接本来就把
  核跑满了，池里排不排队不改变总吞吐）；而同一个池规模下换成 `LOOP_STAT` 省下约
  **22 µs/req（−20 %）**。`baseline2` 两档都在 9.8~10.4 µs，不受影响。结论：要省的是
  「每请求一次池往返」这件事本身，不是池里有几个线程 —— 按档位把 `threadpool_threads()`
  降下来（`main.cc` 里那条 `DATABASE_MAX_CONN` 分支）**不做**：既换不到读数，又会挤掉
  `/async-db` 的槽位。默认 `POOL` 保持不动。

- **静态腿的每请求系统调用 / 分配有门禁了**（`1.6.47-dev`）。新增
  `tests/tools/run_static_fs_gate.py`（判据）、`tests/tools/fs_count_shim.c`（量具）与
  `web_app_static_func.cpp` 的 `fs_count` 用例（采集）—— 用例把 `LOOP_STAT` 档挂在同一个
  app 的 `/loopstat` 上，与默认档（`/`）成对，好让"投不投池"这件事在同一进程、同一份
  负载下有对照。这一批静态优化（`1.6.25-dev` 起的 C1–C5）的判据**不是 rps 而是"每请求
  几次 `realpath`、几次 `stat`、投几次池"**，这类数字确定、不吃压测噪声，所以能当门禁
  用；但响应字节逐字相同，没有计数器就什么都看不见，那些优化下一轮会被无声地退回去。

  量具是一个 `LD_PRELOAD` 的计数器（本机 `ptrace` 被禁，`strace` 用不了，也不该进 CI）。
  它自己就踩过三个坑并写在文件头：libuv 在 Linux 上走 `statx` 且是经 `syscall()` 发出去的
  （只看 `statx` 符号永远是零）；glibc 的 `realpath` 内部走 `__lxstat64` 这类内部别名、
  不过 PLT（所以这一格的口径是**`realpath` 调用次数**，那才是本库能控制的事）；futex 是
  内联指令而本仓的 libuv 是静态链进 `libuvcpp.so` 的，它提交池任务时调的是
  `pthread_cond_signal`（于是这一格恰好等于"每请求投了几次池"，也是 `POOL` 与
  `LOOP_STAT` 两档唯一的差别）。第四个坑是写变异时踩到的：`_FORTIFY_SOURCE` 会把定长
  缓冲区上的 `realpath` 改名成 `__realpath_chk`，那一格就恒为 0 —— `objdump -dr` 上看得
  见；本库的调用点是 `realpath(path, NULL)`，没被改名，如实计到。

  本机读数（100 条命中请求，分母是窗口里**真的回来**的 200 条数，单位：次/请求）：

  | 档 | `realpath` | `lstat` | `newfstatat` | `statx` | `openat`+`open` | `close` | `cond_signal` | `malloc` |
  |---|---|---|---|---|---|---|---|---|
  | `POOL`（默认） | 0.000 | 0.000 | 0.000 | **1.000** | 0.000 | 2.250 | **1.000** | 37.8~38.0 |
  | `LOOP_STAT` | 0.000 | 0.000 | 0.000 | **1.000** | 0.000 | 2.250 | **0.000** | 45.8 |

  门禁判的就是这张表：`realpath`/`lstat`/`newfstatat` 必须为 0、`statx` 必须**恰好一次**
  （少一次是"没校验"——缓存会把旧字节一直发下去；多一次是白做）、命中不许再 `open`、
  `POOL` 的 `cond_signal` 恰好 1 而 `LOOP_STAT` 恰好 0、窗口里必须有 100 条 200（分母缩了
  这张表就不可比）、`malloc` 只当烟感（跨编译器/标准库会漂，界留到 80）。三条变异实测都能
  让它红：`LOOP_STAT` 也投池 ⇒ `cond_signal` 1.000；命中路径加一次 `web_real_path()` ⇒
  两档 `realpath` 1.000；命中路径跳过指纹 `stat` ⇒ 两档 `statx` 0.000。`--self-test`
  是它自己的负对照：不预载量具跑一遍，必须判红而不是"看着全过"。

  **还没进 CI**：它要 `LD_PRELOAD` 加一个 `cc`，且只在 Linux 上有意义（非 Linux 与
  "这棵树没编 webapp" 都是退出码 3 = 没判，不是通过）。跑法与退出码见脚本头。

- **brotli 编码器 + 变体协商（`UVCPP_ENABLE_BROTLI`，默认关）**（`1.6.48-dev`）——
  压缩层此前只有 gzip 与 deflate（`zlib` 那一个后端，变体键只有 `'g'`/`'d'`）。
  这一条补上第三个编码器与它的协商，**默认关**，关掉时逐字节不变（见下面"关掉时的
  判据"）。它只可能影响 `json-comp` 那一档：三条静态腿发的是盘上的孪生文件
  （`http_pick_precompressed_variants()`，与这个开关无关），另两档 json 不发
  `Accept-Encoding`（已逐条核过），而 `json-comp` 的客户端**固定发**
  `Accept-Encoding: gzip, br`。

  **先修掉一个会让这一整条改动净亏的默认档缺陷**。服务器侧的
  `apply_compression_for()` 把 `compress_level()`（默认值 `k_compress_level_default`
  = -1）**原样**喂给四参数那份 `compress()` —— 它对 method 是无知的。负数在
  deflate 侧的含义是"zlib 的默认档"（它本身就是 -1，退化成恒等），可到了 brotli 侧
  `clamp_brotli_quality(-1)` 以前会收成 `k_brotli_quality_min` = **0**，也就是
  "最快档"而不是默认档 5。于是"开 br"实际是"用 q0 编"，而 q0 **比 gzip 还大**：
  平台那三个 round-robin 形状上实测 q0 比 gzip L6 大 25~30%（q5 则小 11%）。
  `json-comp` 是**按字节平方计价**的（见下），开了 br 反而扣分。修法是一行哨兵：
  负数一律 = "该 method 自己的默认档"。两个夹紧函数**仍然不能合并**成一把
  `[a,b]` 尺子：上限 9 vs 11 会把 brotli 的 10 悄悄降成 9。

  **判据（如实写成）**：`cache_tag` 落地之后 br **对 rps 没有贡献**，它换的是
  **线上字节数**，所以判据只能是这两条 —— ① 线上字节更少；② 解压回来**逐字节相同**。
  不是"提分"，更不是"更快"。

  **① 线上字节**。语料就是平台那条 round-robin 的三个真实响应体（`data/dataset.json`
  的前 25/40/50 项，`total = price × quantity × m`，与条目 `build_json()` 逐字节相同），
  同一台机、同一份 `libuvcpp_a.a`：

  | 形状 | 原文 | gzip L6 | deflate L6 | **br q5** | br q0（缺陷下的档） |
  |---|---:|---:|---:|---:|---:|
  | `/json/25?m=4` | 4 190 B | 961 B | 949 B | **847 B**（−11.9%） | 1 202 B（+25.1%） |
  | `/json/40?m=8` | 6 707 B | 1 313 B | 1 301 B | **1 168 B**（−11.0%） | 1 685 B（+28.3%） |
  | `/json/50?m=6` | 8 397 B | 1 546 B | 1 534 B | **1 370 B**（−11.4%） | 2 007 B（+29.8%） |

  百分比那一列是相对 gzip L6。q5 与显式传 5 的那一份**逐字节相同**（用例里钉住）。
  ② 三条的 `decompress()` 结果与原文逐字节相同。

  **③ 代价（这才是这一条为什么不默认开）**。同一次量测的 µs/请求（`-O2`，
  20 次预热后取 4 000 次平均，`taskset -c 2`）：`/json/50` 上 gzip L6 39.9 µs、
  **br q5 144.7 µs（3.6×）**；`/json/40` 上是 30.0 → 121.8（4.1×）。按平台
  `eff = rps × (min_bpr / bpr)²` 算，3.6× 的 CPU 换 11% 的字节是**净亏**
  （0.28 × 1.23 = 0.34）。

  **④ `cache_tag` 一开，上面那个代价就没了**。变体缓存的键是
  `编码字符 + 档位字符 + cache_tag`（`1.6.33-dev` 那条），命中路径只有一次查表
  加一次 `shared_ptr` 递句柄 —— **gzip 与 br 走的是同一条路径，编码只在键里**。
  所以只要响应带 `cache_tag`，br 的压缩代价是**每个 tag 一次**而不是每请求一次。
  本机 A/B（64 条 h1 连接、每臂 5 轮 × 200 000 次、`taskset -c 0-3`、三个
  round-robin URI 轮转、服务端是同一份二进制，只差客户端报不报 `br` 与
  `/json` 要不要填 `cache_tag`）：

  | `/json` 填 `cache_tag` | `Accept-Encoding` | µs/请求（5 轮中位） | 线上字节/请求 |
  |---|---|---:|---:|
  | 是 | `gzip` | 8.85 | 1 498.9 B |
  | 是 | `gzip, br` | **8.55** | **1 350.9 B**（×0.901） |
  | 否 | `gzip` | 51.60 | 1 498.9 B |
  | 否 | `gzip, br` | **88.60**（×1.72） | 1 350.9 B（×0.901） |

  「否」那两行是**今天条目的形状**（`handle_json()` 不填 `cache_tag`）：每请求重压，
  br 的 3.6× 直接落到 rps 上，`eff` 变成 0.58 × 1.23 = **0.72**，也就是**倒扣 28%**。
  「是」那两行里 br 与 gzip 的差（8.55 vs 8.85）落在次间散布内 —— **它在命中路径上
  不比 gzip 贵**，而字节少了 9.9%，`eff` 相对同一名次的对手是 **×1.23**。

  所以这一条的结论写在这里，免得后面有人照着"br 更小"就去开：**编码器与协商是库的
  能力（默认关）；把它在条目上打开必须同时给 `/json` 填 `resp.cache_tag`（库侧从
  `1.6.31-dev` 起就有 `uvcpp_web_response::cache_tag()`），否则是净亏**。这两件事
  分属库与条目，**本次只落库侧那一半**。

  **合规性（一并记下，别当它不存在）**：条目 `meta.json` 的 `mode` 是 `standard`，
  那个档的规则原文里有 "No pre-compressed caches"。`compress_variants_` 是**引擎
  内建**的响应压缩中间件（规则要求的正是 "the framework or engine's built-in
  response compression"），今天全仓只有静态腿在用它、且那条路上的字节**本来就来自
  磁盘上预先压好的孪生文件**；把它用在 `/json` 上是**一次判断，不是一次实现** ——
  这件事必须由条目的维护者去和平台确认，不能由库里这一条默默替他们决定。这也是
  C15 只做库侧、不动条目的第二个理由。

  **关掉时的判据（默认配置，逐条实测）**：① 五个构建格
  （zlib×brotli 的 00/01/10/11 与"brotli 取自源码"那一格）里，**00 与 10** 的
  用例集与产物与改动前逐字相同；② `UVCPP_HTTP_COMPRESS_ENABLE=(1||0)` 与
  `(1||1)`/`(0||1)`/`(0||0)` 四种拼写都由
  `tests/tools/check_config_contract.py` 的契约钉住（brotli 与 zlib 各写各的
  `set(...)`，派生的那个宏才拼得对）；③ 公共头一个不改（`uvcpp_http_compress.h` 里
  `BROTLI` 这个枚举值**无条件存在** —— `content-encoding: br` 在盘上孪生文件那条
  路上一直发得出去，能不能**现场编**才由宏决定），`parse_accept_encoding()` 在
  brotli 关着时不会返回它。

  **判据是哪些用例**。`tests/functional/web_http_client_func.cpp`：`*` 的兜底序
  变成 br > gzip > deflate（与静态腿选孪生的 `http_pick_precompressed_variants()`
  同序），所以凡是靠 `*` 兜底的期望改成读四个按构建算出来的常量；新增
  `test_accept_encoding_no_zlib()`（只编 brotli 时 gzip/deflate 问不出来、`*` 兜到
  br、`*, br;q=0` 什么都不发）与 `test_accept_encoding_no_encoder()`
  （一个编码器都没有时一律 NONE）；`clamp_brotli_quality(-1)` 改成断言
  `k_brotli_quality_default`，并新增一段**不对称**的回归断言 —— 三参数那份、把
  服务器默认档原样透传的那份，都必须与显式 q5 逐字节相同，**且 q0 必须严格更大**
  （前两条恒等断言在"全都退化成同一个档"的实现上也会绿）。把那一行哨兵删掉，
  `compress_brotli` 立刻变红。`tests/functional/web_app_static_func.cpp` 的
  `test_compress_variant_brotli()` 里，凡是需要**第二个编码器**才成立的部分
  （br 与 gzip 是表里两条、字节不同、`br_n < gz_n`）挂在 `UVCPP_ZLIB_ENABLE` 下；
  只编 brotli 的那一格换成另一条断言：客户端指名要 gzip 而我们编不出来时**必须原样
  发原文**，不许拿 br 的字节冒充（头写着 gzip、体是 br，每一层看着都对）。

- **`UVCPP_ENABLE_BROTLI=ON` 且 brotli 取自源码时，`install(EXPORT)` 在 generate
  阶段就失败**（`1.6.48-dev`）。取源码那条支路拿到的是本工程的**普通目标**
  `brotlienc`/`brotlidec`，把它们按目标名写进 `target_link_libraries` （逐项包
  `$<BUILD_INTERFACE:>` 也试过，不是分号展开成裸名的问题）会让
  `install(TARGETS ... EXPORT uvcppTargets)` 报
  `requires target "brotlienc" that is not in any export set`；换成
  `$<TARGET_FILE:...>`（求值成一个**路径字符串**，不再记目标依赖）即过。系统那条
  支路拿到的是 IMPORTED 目标 `PkgConfig::BROTLIENC/DEC`，导出集**不管 IMPORTED
  目标**，所以一直是好的 —— 这条缺陷只在 macOS / Windows / 任何取源码的消费者那里
  现形，而本仓在它之前**没有一格编过这条路**。代价是路径形式不替我们记构建顺序，
  所以补了 `add_dependencies(${_tgt} brotlienc brotlidec)`。至于是导出集检查的哪一层
  没把 `$<BUILD_INTERFACE:>` 剥干净，**没有追到底**（同一个形状在一个两目标的最小
  复现里是被剥成 `$<LINK_ONLY:>` 的，与这里对不上）；CMakeLists 里只记下可复现的
  那条分界，别把它当成解释。量法（把系统 brotli 藏起来，其余依赖用本地克隆喂进去，
  免得配置期联网）写在那个分支的注释里。

- **路径归一化收敛成一个函数，并给它加一条"什么都不用做"的快路**（`1.7.1-dev`）。
  `uvcpp_web_request::take_from()`、`uvcpp_web_app::claim_stream()` 与
  `on_ws_upgrade()` 原先各写一遍等价的
  `web_collapse_slashes(web_url_decode(raw_path, false))`，一致性只靠三处注释互相
  提醒（每处都写着"必须与另外两处完全一致"）。现在收敛到新公开函数
  `web_normalize_path(std::string&)`（**原地**改写，调用方不需要第三块缓冲），
  "同一个 URL 在路由表、流式表、WS 表里看到同一条路径"于是是**构造上**成立的，
  不再靠注释维持。归一的规则与顺序一条没变 —— 解码用 `plus_as_space = false`（路径
  里的 `+` 是字面加号），且**必须先解码再折叠**（`%2F` 解出来就是 `/`、解码后的
  `/` 充当分隔符）。

  同一条路上还去掉了三笔纯浪费的拷贝：`web_url_decode()` 在串里既没有 `%`（也没有
  需要改写的 `+`）时直接返回输入；`web_split_path_query()` 不再无条件整串拷
  `raw_url`（绝对形式与带 `#` 的请求目标都是少数，改成"只读地指回入参、真要改写时
  才落到临时串上"，切出来的两段仍各拷一次，不变）；`web_collapse_slashes()` 与
  `web_normalize_path()` 各自在最简形式上直接返回 —— 三条快路都是把慢路的循环
  **反推成判据**（`%` 与 `+` 是解码唯一会改写的字节；折叠只改"前导 `/` / 中间 `//` /
  尾 `/`"这三件事），不是抽样比对，所以"快路与慢路逐字节同结果"是结构上的性质。

  **判据（callgrind、本机 h1 形态入口、16 000 请求、扣掉空转那一段）**：入口
  22 459.6 → 22 032.4 Ir/请求（**−427.2 Ir/请求，−1.90%**），纯库那一段
  304.8 M → 299.5 M（**−334.7 Ir/请求，−1.76%**）。`web_url_decode` 在这一档每请求
  被调 5 次、545 条指令（查询串里两个参数、键和值各解码一次），正是这条快路要省的
  形状。`web_collapse_slashes()` 此前**一条用例都没有**，现在
  `tests/functional/web_app_util_func.cpp` 里有一张 16 行的期望值表（含快路/慢路的
  分界：`/a` 走快路，`/a/`、`//a`、`/a//b` 走慢路）加一条幂等循环；
  `web_split_path_query()` 补了"绝对形式与 fragment 同时出现"与"输出参数是**覆盖**
  而不是追加"两组。

  **口径（别拿它当平台分数的预测）**：这是**每请求用户态指令数**的账。平台 h1 那八档
  全是 `pipeline = 1`，端到端时间由**每读事件的固定开销**主导（本仓 39.16 µs/请求里
  用户态那约 7.5 µs 才是这条改动能碰到的地方，它与那把 W/F 尺子解出来的 `W` 对得上）。
  −1.90% 折到端到端是 **0.35% 量级**，比平台同一档两次读数之间的散布还小 —— 所以
  这一条按"计数器确定下降"记账，**不按平台读数记账**，本机也无从判断平台读不读得出来。

- **结果集物化那条路上的分配账：每应答 204.78 → 102.77 次分配**（`1.7.2-dev`）。
  `async-db` 那一档每应答 20 行 × 9 列 = 180 个格子，而**每应答 204.78 次 `malloc`**
  里一多半不是"装数据"，是 `std::vector` 的**逐次增长**。两处改掉，第二处才是账。

  ① `value_from_pg()`（`src/db/uvcpp_db_pgsql.cpp`）此前对**每一格**都先建一个
  `const std::string text(data, len)`（`strtoll`/`strtod` 要的是 NUL 结尾的 C 串），
  bool 那格还要为比三个字面量再建一个，文本那格建两次（一次取文本、一次拷进值里）。
  现在数值/bool 走一个 64 字节**栈缓冲**的小工具（`pg_num_text`），文本格**移**进值里。

  这一处的实测只有 **204.78 → 187.77 次分配/应答（−17.0）**，比"每格一次"少得多，
  原因值得写下来：**libstdc++ 的 SSO**（15 个字符）。短数值的 `std::string` 本来就不
  落堆，真正被省掉的只有超过阈值的那几格（`tags` 数组文本、较长的 `name`）。这条改写
  剩下的价值在另外两处：去掉"超过 15 个字符的数值列会突然落堆"这个悬崖（`numeric`
  的长小数会踩到），以及每格少构造一次 `std::string`。

  ② 真正的账在物化那一层：`fill_rows()` 每行 `row.push()` 九次、整表 `add_row()` 二十次，
  两个 `std::vector` 都按 1-2-4-8-… 增长 —— callgrind 的调用边账本里
  `vector<uvcpp_db_value>::_M_realloc_insert` **103.00 次/请求**、`vector<uvcpp_db_row>`
  那条 **6.00 次/请求**，合计占全部 `malloc` 的 56%。而行数与列数**都是 `PGresult`
  直接报出来的**（`PQntuples` / `PQnfields`），所以新增 `uvcpp_db_row::reserve()` 与
  `uvcpp_db_table::reserve_rows()`（都是**新增**的公开方法，`operator[]` 的语义一个字
  没动），调用点一次到位：**187.77 → 102.77 次/应答**，字节 130 680.9 → 96 760.8
  （−26.0%）。四个后端的结果集填充循环都补了这一笔；MySQL 那边**只能**预留列数 ——
  `mysql_num_rows` 在 MySQL 的头里是个宏（`res->row_count`）、不是能进 `dlopen` 表的
  符号，所以那里留了一句注释而不是一行代码。

  **判据是分配计数**（`LD_PRELOAD` 的 `malloc` 计数器、本机 async-db 形态入口、
  `-c 32 -m 1`、6000 请求、同一份入口二进制**只换 `.so`**）：两臂各测三次，
  204.80 / 204.76 / 204.77 → 102.76 / 102.78 / 102.78，**逐次可复现**。同一次读数里
  同一 `/proc/<pid>/stat` 口径下用户态 69.2 → 56.7 µs/请求、合计 93.8 → 81.7（−13%，
  这一项散布 ±2.5 µs，只当旁证）。**顺手更正自己的一条预估**：先按"每格一次分配"
  估 −180，实测 −17 —— 差的那部分是 SSO，不是量具漏了。

  **"应答没变"的判据**：同一 URL 的应答主体逐字节相同；postgres 那套功能用例在两臂上的
  **输出逐字节相同**（`cmp`）；三后端树里 mysql 197 / pgsql 197 / sqlite 全通过、0 失败。
  入口自己那条 writer-vs-DOM 字节校对也在这一轮里照跑（服务端日志里的
  `/async-db answers from the writer path`）。

  **口径**：`async-db` 是 Reference-only 档、对合成奖牌分贡献 0 —— 这一条只落在那一档的
  `cpu_per_req_us` 上；平台那个数还含系统时间，这里改的是用户态那半边。

- **没装日志回调时不再构造那一行日志：每应答 −3.96 次分配**（`1.7.2-dev`）。
  `uvcpp_db_client::query()` / `execute()` 结尾那行
  `log_line("query ok | " + std::to_string(行数) + " 行 | " + …)` 是**急切**求值的：
  有没有装回调都一样，`brief_sql()` 要把整条 SQL 扫一遍再 `reserve` 一次，那条
  `operator+` 链每越过 SSO 的 15 字符还要再分配一次（一行约 250 字节 ⇒ 六七次）。
  而**不装回调是生产路径上的常态** —— HttpArena 入口从头到尾没调过 `set_log` ——
  于是整条账白烧。现在改成惰性：`impl` 上新增一个 `log_lazy(build)`，**装了回调才去
  构造**。这与 web 那侧 `log_level::OFF` 是**阈值**而不是"藏起来"是同一个道理。

  判据（同一份入口二进制只换 `.so`、本机 async-db 形态、`-c 1024`、30 000 请求、
  五条轮转 `limit` 模板）：分配 **106.16 / 106.15 → 102.18 / 102.22 次/应答**
  （A B B A；组内散布 0.01、组间 −3.96），字节 99 112.4 → 98 465.6。另外把**改动前
  那份源码**单独重建了一遍，产物与改动前的 `.so` **逐字节相同** —— 所以这 3.96 次
  全部是这一处，没有混进别的东西。

  **顺带记一条量具的边界**：同一轮 `user+sys` 是 106.2 → 108.3 µs，**没有动**。
  按一次分配约 30 ns 算，3.96 次 ≈ 0.12 µs，而这条量具在同一档上的散布是 ±5 µs ——
  这个改动**落在分辨率之下**，不是"没效果"。所以这一条只按分配计数记账（计划里
  第 1 节的判据本来就是"系统调用数、分配数、`utime/stime` **之一**确定下降"）。
  折到平台 `async-db` 那个 407.2 µs 上是 0.03% 量级，如实写在这里，不假装它值钱。

  行为不变的证据不是"看着像"：`test_db_sqlite_async_func` 装的日志钩子是按**内容**
  断言的（要求行里出现 `A_MARK`，那是 SQL 文本经 `brief_sql()` 之后的残留），
  三后端树里 `db_sqlite_{func,async_func,pool_func}` 全过。

- **`resp.json()` 的应答体不再多拷一遍：每应答 −8 349 字节**（`1.7.2-dev`）。
  `uvcpp_web_response::json()` 此前走 `body(const std::string&)`，而那条路的实现是
  `clone_data()` —— `uvcpp_json_dump()` 刚交出来的那块（`/json/50` 一档 8 397 字节）
  还要再 `malloc` 一块同样大的、`memcpy` 一遍。现在改走 `body_take()`，与
  `json_str(std::string&&)` 同一条路：整块接过来，一次拷贝都不做。两种写法的
  **可见结果逐字相同**（报文、`Content-Length`、`Content-Type` 都不动）。

  顺带补的 `body_take(std::string&&, const char*)` 重载不是顺手加的：`json()` 往
  `ct` 位置传的 `"application/json; charset=utf-8"` 是 34 字符，绑到
  `const std::string&` 上要先建一个**临时串**（MSVC 的 SSO 只有 15 字符）—— 那就是
  "省掉一次 8.4 KB 的 `malloc` + `memcpy`"旁边又添一次小 `malloc`，实测正好
  **+1.00 次/请求**。这一对重载与 `body(const std::string&, const char*)` /
  `body(const char*, size_t, const char*)` 那一对**逐字同理**，后者那处文件注释
  早写着为什么需要它。

  判据一（分配账，`/json/50?m=6`、固定 **20 000 请求**的窗口、A B B A、
  `LD_PRELOAD` 拦 `malloc`/`calloc`/`realloc`）：分配 **1004.0176 / 1004.0178 →
  1004.0179 / 1004.0172 次/请求**（没动，组内散布 ±0.0007），字节 **168 579.3 /
  168 579.7 → 160 230.7 / 160 230.0 /请求**（−8 348.9、−4.95%，散布 ±0.4 B）。

  判据二（拷贝账，同一形状，换成拦 `memcpy`/`memmove` 的 `LD_PRELOAD`，A B B A）：
  次数 **2 592.0014 / 2 591.0015 / 2 591.0015 / 2 592.0013 次/请求**（正好
  **−1.0000** 次），字节 **36 066.0 / 27 669.0 / 27 669.0 / 36 066.0 字节/请求**
  （**−8 397.0**，组内一格不动）。这 −8 397 与同一档 `curl -w %{size_download}`
  量到的应答体长度**逐字节相同** —— 少掉的就是"那块体被整段拷一遍"这件事本身。

  判据三（指令数，callgrind `--collect-jumps=no --cache-sim=no --branch-sim=no`，
  热身之后 `callgrind_control -z` 清零、窗口之后 `-d` 落盘，总账用
  `callgrind_annotate --auto=no` 的 PROGRAM TOTALS 读）：A B B A = **1 138 898.1 /
  1 129 449.9 / 1 128 789.2 / 1 138 237.3 Ir/请求** ⇒ 改后 **−9 448 Ir/请求
  （−0.83%）**，两臂各约 660 的组内散布，间隙是散布的 6 倍以上。这一笔比"少一次
  `memcpy`"该有的量级大，是因为 `clone_data()` 那一路**不只是多一次拷贝**：它还多
  一次 8.4 KB 的 `malloc` + `free`，而那个尺寸超出 tcache（默认上限 1 032 字节），
  走的是 unsorted/small bin 那条更长的路。

  这一笔 −8 349 对得上账、且**两个数字要一起读**：省掉的是 `clone_data()` 那块
  **8 397 字节**（同一档量具 `curl -w %{size_download}` 实测的应答体长度，逐字节
  对上），多出来的是 `body_take` 那条路本来就要的那个 **48 字节**共享控制块
  （`make_shared<const std::string>`：libstdc++ 的 `_Sp_counted_ptr_inplace` = 16 字节
  计数 + `sizeof(std::string)` 32 = 48）—— 8 397 − 48 = **8 349**。所以**分配次数
  一格不动是对的**（2 → 2：少一块大的、多一块小的），变的是**每请求分配出去的
  字节数**与那一次 8.4 KB 的 `memcpy`。拿"次数没降"当"没效果"会读反。

  **如实记一句这笔账的份量**：上面那 −0.83% 是**这段处理器路径**上的指令数，
  不是平台那一档的分。折到平台读数上它比同一档两次读数之间的散布还小（第五轮那组
  对照落在 ±4% 内）—— 所以这一条只按上面那三个**确定性计数**记账，不按平台读数
  记账。真正的护栏是用例：`test_web_app_response_func` 里
  `jr.raw().body.is_shared()` 那条**区分得开**接管与拷贝（报文那两条断言区分不开，
  两种写法谁都过）—— 做了一次**变异**，把 `json()` 改回 `body()`，只有这一条变红。
- 静态腿 `LOOP_STAT` 档的解析记忆与缓存比对各自多一份**每条循环的前台副本**，
  热路径上一把锁都不取。这一档原先每请求要取两次全局 `mu_`，量出来那两次的持有
  （2021~2631 ns / 468~597 ns）里**大头是争用本身** —— 把临界区里的活整段删掉，
  1534 ns 里还剩 **905 ns**，所以并行度的实际上限就是「1/持有」约 24~27 万 rps，
  与平台 `static-h2` 两档停的 ~23.5 万 rps 对得上。修法因此不是在临界区里省时间，
  而是让每条循环把答案拷一份放进自己的桶里（既有的按循环 `retire_bucket`，节点从不
  `erase` ⇒ 地址稳定）。**只有这一档能免锁**：`LOOP_STAT` 的读（`collect_hints()`）
  与写（`probe_job()` 回写）都在循环线程上；`POOL` 档的写发生在 worker 上，那两张表
  一次都不碰。★ 两处**不能想当然动**：
  - 记忆副本的作废靠**一个整数** `front_gen_`（`clear_cache()` 里 +1）—— 那两张表
    没有锁，这条线程走不动它们，一个整数让它们全部失效，同时保住"清缓存之后第一次
    请求必定走完整解析"这条**既有的**性质。
  - 内容副本存的是**弱引用**：它按不住字节 ⇒ `cache_bytes()` 的账仍然只由全局表决定，
    全局表淘汰一份时前台当场失效、`clear_cache()` 对这一半是**免费**的。改成强引用
    就等于把缓存分片（内存翻倍、命中率还掉），那是这张表明确不做的事。
  判据是**确定性的**（同一份入口二进制只换 `.so`，A B B A 交错）：`mutex_lock`
  **2.03 → 0.03 次/请求**、持有 **3679 → 969 ns**、等待 **7761 → 183 ns**；
  `collect_hints()` 那一处在调用点归因表里**整个消失**。本机 rps 中位数 109 997 →
  131 608（+19.6%，9 核、压测端自己顶着 ⇒ 只当旁证，平台效应按 1/持有 那个模型预测）。
  仓内判据：`test_web_app_multiloop_func` 的 LOOP_STAT 双循环用例（前台副本下
  `cache_misses()`/`cache_hits()` 必须精确）、以及 `fs_count` 子用例里的
  `measure_memo_reset()`（`clear_cache()` 之后**第一个**请求必须 `realpath` +1、
  紧接着那一次 +0）。**变异如实记**：只删生成号那一行，两条判据都不变红（字节那半的
  失效是弱引用免费给的）；把弱引用改成强引用**并同时**删掉生成号才红 —— 这条判据
  同时见证两个机制，少一个都不红。（`1.7.2-dev`）
- `uvcpp_buf` 自有块那张**线程局部**自由表的自由链表**从块里搬到表里**。改前它把"下一块"
  写在块自己的头 8 字节上（与 M6 那张 `uvcpp_block_cache` 同形），于是 `size < sizeof(void*)`
  的块一块也留不住：`put()` 只能原地 `std::free`、`try_take()` 也就永远命不中。而 5~7 字节
  正是 WebSocket 那条路上最热的尺寸 —— `"hello"` 回声一次要在 `resize_impl()` 那条生块的
  路上造 **4 块（3 块 5 字节 + 1 块 7 字节，平均 5.5 字节）**，改前这四块**一次都没进过表**。
  每个槽因此改成一张 `void* head[64]` 数组：`put()`/`try_take()` 对块的大小**再无任何假设**，
  也不再**写**取到的块（改前那 8 字节是被链表指针覆盖掉的）。另补了一条必要的规则：
  **已经排空的槽可以改服务新尺寸** —— 改前 `count` 掉到 0 的槽永远不会再被分配，线程一换
  工作形状这张表就等于只剩头几个尺寸。上界不变（每槽 64 块 × 单块 ≤4 KiB × 4 槽 ⇒ 每线程
  最多留 1 MiB 的块，表自身 ≈2 KiB）。★ 只做表外置只走到 −3.000，因为剩下那一块 7 字节的
  **从来没还给这张表** —— 它走的是 `uvcpp_buf::free_buf()` 里的裸 `uvcpp_free`。
  于是另补两处：
  - `uvcpp_tcp_client::write(const char*, size_t, cb)` 的两条分支从 `out_uv_buf()` +
    `set_uv_buf(raw, true)` 换成 `release_uv_buf(&hb)` + `set_uv_buf_block(hb)` —— 块的
    所有权与形状都不变，少掉的只是那个 `uv_buf_t` 包装本身的一次分配（每写一次一笔）。
  - `src/req/uvcpp_write.cpp` 新增 `release_own_block()`（池那档原样 `uvcpp_free`，其余还给
    本线程的表），**四个**自有块的释放点全部走它（`~uvcpp_write` 的第 1 块、`release_second()`、
    `set_uv_buf()`、`set_uv_buf_block()`）；每一处都拿**能看见的长度**（`len`，由 `uvcpp_buf`
    的不变式恒 ≤ 真实容量）做键 ⇒ 只会"少复用"，**绝不发出比要的小的块**。
  判据一（确定性，A B B A 交错，**同一份入口二进制只换 `.so`**，组内零散布）：
  `PIPE=1` **11.255 → 8.255 → 6.255 次分配/消息**（释放 11.163 → 8.163 → 6.163）；
  `PIPE=16`（对齐平台那一档：一个读事件 16 帧）**3.692 → 0.567**（释放 3.645 → 0.520）——
  每消息那 3 块 5 字节**整个消失**，剩下的 0.567 全是**每写一次**那笔固定成本（200/192/32/
  24/24 字节那五笔），在 `PIPE=16` 上被摊薄 16×。**"每消息分配数"这个指标在两条形状上会给出
  5.00 与 3.13 两个不同的答案，引用时要说清是哪一条。**
  判据二（端到端，**不挂**计数器，只用 `/proc/<pid>/stat` 的 `utime+stime` 括窗口，2 000 000
  消息、`PIPE=16`）：**0.775 → 0.730 µs/消息（−5.8%）**、255 891 → **267 986 msg/s**。
  **如实记**：计数器自身给每次 `malloc`/`free` 加的那笔税约 7 ns，正好解释挂表与不挂表之间
  那 ~0.05 µs；而这条腿在本机是**系统调用占优**的（`PIPE=1` 上 sys 5.7~6.1 µs 对 user
  1.8~2.1 µs），`PIPE=1` 那一格的 µs **根本分辨不出来**（11.3 次分配的税约 0.08 µs，压在
  7.7 µs 的总量上）⇒ **验收走上面那两张计数表，µs 只当"方向一致"的旁证，本机 µs 不许拿去
  预测平台那一格。** 读数、计数器自己的税与复现写在 `doc/benchmark.md` §4.19。（`1.7.2-dev`）
- **h1/h2 每条读事件那一次 64 KiB 分配**：`alloc_cb` 的读缓冲改从**每线程两张精确尺寸的槽**
  里发。这条腿与 §4.19 那族小分配**正交** —— 它是**每读事件一次 64 KiB 的 `malloc` + `free`**
  （libuv 给 TCP 流的 `suggested_size` 恒为 65536），`pipeline = 1` 时一个读事件就是一个请求。
  单看这一对分配不值钱，贵的是 64 KiB 落在 large bin 一侧：一次 `free` 要合并相邻块、再按
  `FASTBIN_CONSOLIDATION_THRESHOLD` 触发 fastbin 合并扫描，正好与这条路上真正的大头
  （nlohmann 的 DOM、行物化、WS 回声那些每应答 10²~10³ 次的小分配）正面撞上 ⇒
  **收益与"每应答分配多少次"成正比、与"每事件分配多大"无关**，所以它在 h1 `baseline11` 上
  几乎白搭、在 `json` 上很值钱。
  判据一（callgrind 指令数，同一份入口、同一个 URL）：`/json/50?m=6`（应答 8 397 B）
  **4 550 109 494 → 3 796 143 977 = −16.57%**；`/baseline11?a=1&b=2`
  **501 010 150 → 494 196 681 = −1.36%**。自耗归因（`/json/50?m=6`）：
  `malloc_consolidate` **214 831 713（4.72%）→ 929 527（0.02%）**、`unlink_chunk.isra.0`
  **83 495 443（1.84%）→ 916 593（0.02%）**，而 `_int_free` 330 250 196 → 330 035 400
  **不动** —— 省下的正是合并扫描那一笔，不是"少了一次 free"。
  判据二（**零源码改动的预测臂**，按先量再改的纪律）：先用一个 ≤50 行的 `LD_PRELOAD` shim
  拦 `malloc`/`free`（尺寸正好 64 KiB 时走线程本地自由表）把同一件事在不改源码的情况下量出来，
  4 550 109 494 → **4 062 528 230**，shim 自耗 128.86M（3.2%）⇒ 毛效应 ≈ −13%。
  **源码版（−16.57%）比 shim 版更低那 3.2%**，因为 shim 每次都要过一遍自己的哈希与自旋锁，
  而源码版在 `alloc_cb` 里直接命中。
  判据三（端到端，`/proc/<pid>/stat` 括 CPU，同一份入口二进制只换 `.so`，A B B A）：
  `/json/50?m=6` 两臂不重叠，中位 **147.3 → 126.5 µs（−14.1%）**。★ **如实记这一条是负结果**：
  照 `latency-500k-8cpu` 的形状（srv `0-7`、drv `8-17`、`-c 1024 -t 8`）压 `/baseline11`，
  中位 **11.40 → 11.60 µs（+1.7%，落在散布内）** ⇒ **在 h1 `baseline11` 上判"中性"**，
  只作"没有反向"的旁证。
  修法：`src/net/uvcpp_tcp_client.cpp` 顶部新增匿名命名空间的 `read_block_cache`
  （2 槽 × 每槽 2 块，`4096 ≤ size ≤ 256 KiB`）与两个内联包装 `alloc_read_buf` / `free_read_buf`，
  9 处释放点与 1 处分配各改一行；**只在 `UVCPP_ENABLE_MEMORY_POOL=0` 编译进来**（池那档原样
  走 `uvcpp_buf`），每线程最多留 2 × 64 KiB + 2 × 4 KiB = **136 KiB**，表自身 ~2 KiB。
  三条设计取舍：按**精确尺寸**取而不按尺寸类（实测这条路上只出现 4 KiB 兜底与 64 KiB 两个尺寸；
  "发一块比要的小的"是这个库绝不能出的错）；指针**存在表里、不写在块的头 8 字节上**
  （§4.19 刚为同一件事吃过一次亏）；与 `uvcpp_malloc_block_cache` **分开两张表**（那张的上界是
  4 KiB × 4 槽 × 64 块 = 1 MiB，混进来会让它的 4 KiB 上限变成噪音）。
  ★ **已证伪、不再考虑**：把读缓冲**封顶到 16 KiB**（压在那个 65536 阈值之下）确实确定性生效
  （分配计数器看每请求字节数 65 920 → 16 796），但 **callgrind 指令数一格没动**、
  `malloc_consolidate` 也照旧 ⇒ 贵的不是"块有多大"，是每请求那一次 large-bin 分配/释放**本身**。
  读数、自耗表与复现写在 `doc/benchmark.md` §4.20。（`1.7.2-dev`）
- **h1 动态热路上的三处白拷**：路由查找不再拷 `path`、状态行与响应头一次 `resize` 直写、
  查询串整串只扫一遍。三处**输出一个字节都不动**（18 种请求形状逐字节比对）。真实工作点
  （8 条循环、`h2load -c 1024 -m 1`）上**每请求的用户态指令数 22 836.1 → 20 317.6 = −11.03%**：
  - `uvcpp_http_server::find_handler` / `find_stream_handler`：`path` 直接来自请求行、
    长度常超过 SSO 的 15 字节，所以改前那句 `std::string p = path` 每调用一次就是一次堆分配
    加一次 `free`，而每请求要调**两个**（普通路由一次、流路由一次）。现在只算出剥掉查询串
    之后的**长度**，比较走 `path.compare(0, n, r.path)`，不构造任何子串。分配器那四行
    （`malloc` / `free` / `_int_free` / `operator new`）合计 **−307.5 指令/请求**，正是这两次
    分配消失的独立见证 —— 它们与这一处共变，不依赖任何端到端噪声。
  - `uvcpp_http_response::to_string_into`：改前状态行一次 `append`、**每个响应头再 4 次**
    （`_M_append` 933 次/请求）。现在把 `est` 补成**真上界**（改前漏算了 `content-length`
    那一段，所以只能一路 `append`），一次 `resize` 出整个头部区间，用 `memcpy` 加立即写字节
    直填，最后 `resize` 回真实长度 ⇒ `_M_append` **−776.0**、`__memcpy_avx_unaligned_erms`
    −327.0，而新增的 `_M_replace_aux` +52.0 就是收尾那一次 `resize`。
  - `web_parse_query`：改前先 `substr` 出 `kv` 再切两刀，然后**每个键、每个值各调一次**
    `web_url_decode`。现在整串只扫一遍有没有 `%` 或 `+`，一个都不出现时键值直接在 `query`
    上切片、**完全不解码**（`web_url_decode` 226 次/请求 → **0**）。语义没动，由既有用例
    `tests/functional/web_app_util_func.cpp` 的 "查询串里 + 是空格" 那条断言见证。
  ★ **不吹**：指令数只算用户态，系统调用那半边（10~12.5 µs）一行不动 —— 按本仓的换算，
  这 −2 518 指令只值 **−0.8 ~ −1.3 µs**，压在 30.65 µs 的总量上就是 −3 ~ −4%。
  `latency-500k-8cpu` 那一格要的是 **−48%**（8 线程吃 500k rps ⇒ 每请求 ≤16 µs，我们是
  30.65），**这三处不是通往那一格的杠杆**，它们的位置是"确定的 −4%，顺手做掉"。
  ★ **如实记**：三处合起来只是把指令数那条曲线压低；这也说明 h1 这一层已经到本仓能碰的
  边界 —— 逐函数归因里**没有单一支配项**（每请求 ~20.3 k 指令，头 40 个函数约占 45%），
  剩下还能收的三池（路由匹配 ≈860、头部搬运 ≈1 000、分配器 ≈1 300 指令/请求）加起来
  ~14%，够不到所需的 −39%/−48%。逐函数表、判据与复现写在 `doc/benchmark.md` §4.22。（`1.7.2-dev`）
- **h1 动态热路再收一轮：每请求用户态指令 19 153.0 → 17 952.0 = −6.27%**（`1.7.2-dev`）。
  七处，合计**不改一个字节的应答**（18 种请求形状对着上一版 `.so` 逐字节比对，只掩 `date`）：
  - `uvcpp_web_request::take_from()`：关心的四个头（`host` / `content-type` /
    `content-length` / `accept-encoding`）**一趟扫完**，且值改成**写进成员**而不是按值
    取回 —— `http_get_header()` 返回的是 `std::string`，值一过 SSO 线就是一次 malloc。
  - `uvcpp_http_request::keep_alive` 新增三态字段（`-1` 没算过 / `0` 关 / `1` 保持）。传输层
    在 `extract_metadata()` 里**本来就**算了 `llhttp_should_keep_alive()`，带下来之后 web 层
    不必每请求重扫一遍头表（`uvcpp_web_request::is_keep_alive()` 134 → 0 指令/请求）。
  - `uvcpp_http_server::send_response()`：4 次 `has_header()` + 1 次 `get_header()` 换成
    **一趟扫表**（`has_header` 412 → 94、`get_header` 147 → 0）。
  - `host_header_violates()` 与 `transfer_encoding_is_void()` 合成
    `message_header_violates()`：同一份头表，按名字长度分流（4 / 17），一趟判完。
  - `uvcpp_web_response::raw_after_sync()`：webapp 的发送出口刚同步过一次，不再来第二遍
    （`sync_meta` 310 → 89）。
  - `web_parse_query_into()`：`&`/`=` 一趟找完、整串只判一次 `%`/`+`；`reserve(4)` 消掉
    "空向量从 1 长到 2 扩两次"那两笔分配。
  - `uvcpp_web_router::match()`：**全字面量**路由改走一张 `模式 → 候选下标` 的表，
    跳过 `split_path()` 的 vector 分配与逐段比较（参数路由与通配路由照旧走通例）。
  ★ **公开类型的布局变了**：`uvcpp_http_request` 多一个 `signed char` 字段 —— 消费方要
  重编（1.7.2 尚未发布，按开发版记账）。
  ★ **与 §4.22 同一把尺子**：`cg8.sh`（8 循环、`taskset 10-17`、`c=1024`、`m=1`、
  `/baseline11?a=13&b=42`），五臂的顺序读数 19 153.0 → 19 036.1 → 18 613.0 → 17 985.2 →
  17 952.0 ⇒ **−6.27%**。指令数只算用户态，折到端到端的换算与"这一档为什么仍然不是
  `latency-500k-8cpu` 的杠杆"见 §4.22 与 `doc/benchmark.md` §4.23。

### JSON

- **第二个 JSON 解析后端：yyjson（可选，默认关）**（`1.6.34-dev`）。本仓的 JSON
  **读入**那一侧此前只有一条路：`webapp/uvcpp_web_json.h` 的 nlohmann —— header-only、
  语义完整、也慢（每个值一个 `std::map` / `std::vector` 节点，一份几 KB 的文档解下来
  成百上千次 `operator new`）。新增 `src/yyjson/` 模块：`uvcpp_json_doc` 拿
  [yyjson](https://github.com/ibireme/yyjson) 解析，配一套**与 nlohmann 路径同名**的
  访问器（`uvcpp_json_get_string/bool/int/double`，靠第一参数类型区分重载）、同名的
  遍历（`uvcpp_json_object_iter` / `at()` / `get()`），以及把 DOM 喂回
  `uvcpp_json_writer` 的桥（`uvcpp_json_write`）。

  **这是"另一个可选项"，不是"替换"**：`using uvcpp_json = nlohmann::json` 一个字没动，
  `uvcpp_json_parse` / `uvcpp_json_dump` / `uvcpp_json_get_*` 的行为一个字没改。两者可以
  同时出现在一个工程里，甚至一个函数里。

  **关掉时的判据**（默认配置，逐条实测）：① 库的**导出符号集与 `DT_NEEDED` 逐字相同**
  —— `nm -D --defined-only` 与 `readelf -d` 的 diff 都是**空的**；② 编译单元只多一个：
  深度预扫描从 `webapp/uvcpp_web_json.cpp` 搬到核心层 `src/uvcpp/uvcpp_json_scan.cpp`
  （同一个判定要给两个后端用，而 yyjson 那侧不能依赖默认关着的 webapp），搬的是**代码
  逐行相同**的那一段；③ `uvcpp_config.h` 之外的头一个不改、ctest 用例数一条不多不少
  （两侧都是 92 条）。`.so` 的字节数会动几十~几百字节（多一个目标文件、嵌进去的
  `__FILE__` 串长度也不同）—— 那**不是**判据，拿它当判据的话换个构建目录就会"变红"。

  **读数**（量具在仓外，200 次预热 + 5 轮 × 20,000 次取最好，`taskset -c 0-3`；
  语料是 `{"items":[{id,name,category,price,quantity,active,tags[]}…],"total":n}`，
  键按升序写以便两边产出可比）：

  | 语料 | 库入口 ns/次 | 纯解析 ns/次 | 220 B 小文档 ns/次 |
  |---|---|---|---|
  | 1 147 B / 10 项 | 21 070 → 1 569（**13.4×**） | 15 184 → 1 569（9.7×） | 2 510 → 226（11.1×） |
  | 5 727 B / 50 项 | 114 634 → 7 478（**15.3×**） | 79 311 → 7 478（10.6×） | 2 502 → 228（11.0×） |
  | 22 973 B / 200 项 | 451 279 → 30 863（**14.6×**） | 315 150 → 30 863（10.2×） | 2 487 → 216（11.5×） |

  「库入口」是 `uvcpp_json_parse(s, doc)` 对 `doc.parse(s)`，「纯解析」是
  `nlohmann::json::parse` 对 `doc.parse(s)` —— 前者多含 webapp 那层的门
  （EMPTY/TOO_LARGE/TOO_DEEP 预扫描 + `is_discarded()`）。两个口径都写出来，免得把
  "入口的开销"记成"解析器的差距"。

  **DOM 常驻内存**：同一份文档解完，nlohmann 8 312 / 40 632 / 161 832 B（输入的
  7.0~7.3×），yyjson 4 367 / 21 155 / 84 385 B（输入的 3.67~3.81×），**稳定在 1.9×**；
  分配次数 318 / 1 481 / 5 835 → **2**。★ 这两列**不是同一把尺子的读数**，必须一起写：
  nlohmann 那列来自量具里全局覆盖的 `operator new`（数**申请**字节），yyjson 那列来自
  后端的 `memory_bytes()`（它自己的分配器**实测**）—— 因为 yyjson 走 `std::malloc`，
  全局 `operator new` 覆盖**看不到它**。两个口径都是"这次解析向系统要了多少字节"，
  量级可比，但不是逐字节同源。

  **判据**是 `tests/functional/yyjson_doc_func.cpp`（新，约 900 行）：一张 25 行的输入
  表（16 条坏输入 + 空 / 纯空白 / 超长 / 超深 / BOM / 非法 UTF-8 / 重复键 / 64 与 65 层 /
  又长又深 / 又深又坏 / `max_depth = 0`）**每条先与手写期望值比、再与 nlohmann 逐条比**
  —— 只判"两边一样"是不够的，一个对什么都说 OK 的解析器让两边都错也能全绿；五份键升序的
  语料上 `uvcpp_json_write` 与 nlohmann `dump()` **逐字节相同**（并先断言 nlohmann 侧产出
  非空且不是 `null`，否则两边一起坏也判绿）；数字语料两边输出交给 nlohmann 再比 `==`；
  `memory_bytes()` 空为 0、`reset()` 后回 0、反复失败 200 次后文档**逐字节不变**；以及
  **十万层**的输入把上限抬到它之上就真解析（读入器**不递归** —— 深度是策略、栈是物理，
  两者都要有见证）。深度预扫描搬到核心层之后，它自己的五条断言仍在
  `tests/functional/web_app_request_func.cpp`，**一字未改**。

  它当时**不在发布包里**（`release.yml` 不传这个 `-D`，包里那个头是空的壳，拦住你的是宏
  而不是文件的缺席），所以它需要一条自己的 CI 腿 —— 那三格与它们各自的理由见下面
  「配置、打包与 CI」那一节（`1.6.35-dev`）。**这一句 1.7.0 起作废**：发布腿也传
  `-DUVCPP_ENABLE_YYJSON=ON`，包里那个宏是 1（见「配置、打包与 CI」里 `1.6.55-dev`
  那条）。**注意作废的只是"不在包里"这半句**：`UVCPP_ENABLE_YYJSON` 在 CMake 里
  **默认仍是 `OFF`**，自己 `cmake -S . -B build` 而不传 `-D` 的人拿到的仍旧是
  nlohmann 那条路 —— 变的是发布腿的参数，不是默认值。

  另外四处**刻意的**行为不同（成员按文档顺序而非键排序、重复键取第一个而非最后一个、
  `1.0` 写成 `1`、`uvcpp_json_write` 递归而由 `max_depth` 兜住）逐条写在
  `doc/json-guide.md` §13.3 与模块头里，不是待修的差异。**这一份"四处"在第 1.7.2 缩成
  三处**：写回换成 yyjson 自己的序列化器之后，`1.0` 两边一致了、写回也不再递归 ——
  见下面那条。

- **yyjson 补齐"完整解析 + 完整写入"：构造器 + 用自己的序列化器写回**（`1.7.2-dev`）。
  `src/yyjson/` 此前只会**解析**（模块里 `yyjson_mut_*` 一次都没出现过），写回那道桥
  `uvcpp_json_write(const uvcpp_json_value&, uvcpp_json_writer&)` 走的是**本库手写的
  流写器**。这次把两个方向都落到**同一个生态里已有的生产级库**上：新增构造侧两个类型，
  写回换成 yyjson 自己的序列化器。

  **API 变更（破坏性）**：`uvcpp_json_write(…, uvcpp_json_writer&)` **删除**，换成
  `uvcpp_json_write(const uvcpp_json_value&, std::string&)`，另加一条收构造侧句柄的
  `uvcpp_json_write(const uvcpp_json_node&, std::string&)`。契约：**写之前 `out` 被清空**，
  成功时它就是产出（不含结尾 NUL），失败时为空。仓内调用点只有三条（都在
  `tests/functional/yyjson_doc_func.cpp`），但它是**公开 API** ⇒ 在这里明写。
  旧路那两道闸**随之没了**：过去调用方用 `json_write_options` 的 `max_depth` / `max_bytes`
  兜住写回，换掉写器之后这两道门不存在（解析侧的深度门仍在，默认 64；输出侧只剩一条
  64 MiB 的失控上限 + 一条精确尺寸的分配式慢路）。

  **新增 `uvcpp_json_builder` + `uvcpp_json_node`**（都落在已有的
  `src/yyjson/uvcpp_json_doc.h` 里，不新增源文件 —— 新头会被 `cmake --install` 的 glob
  装出去，而私有头一旦引用 `<yyjson.h>` 就是个装出去的坏头）。用 yyjson 的**可变 DOM**：
  `make_object()` / `make_array()` 造游离容器，`add_*` / `push_*` 填，`set_root()` 挂根，
  `add_copy()` / `push_copy()` 把解析侧的子树整块深拷进来（**两个 DOM 之间唯一的通道**，
  拷完与源文档再无关系，源 `uvcpp_json_doc` 可以立刻析构）。键与字符串值**一律拷进
  builder 自己的池**（传临时 `std::string`、传栈缓冲区、传含 `\0` 的键都安全）；键走
  `yyjson_mut_strncpy` + `yyjson_mut_obj_add`（公开 API、O(1) 追加、允许重复键），
  **不用** `yyjson_mut_obj_add_strncpy`（它的键是**借**的）也**不用** `yyjson_mut_obj_put`
  （同名键线性查找 ⇒ 建 n 键对象是 O(n²)）。失败一律返回 `false` / 无效节点、**不抛异常**、
  不改动已建好的部分（原因在 `last_error()` 里）。

  **写回**：`yyjson_val_write_buf` / `yyjson_mut_val_write_buf` 走快路 —— 缓冲**必须比产出
  **大**：写器把 `yyjson_write_ctx` 栈放在缓冲尾部，而且每写一段之前按**最坏展开量**预留
  （字符串那条是 `str_len * 6 + 16`，见上游 `yyjson.c` 的 `incr_len`），所以"刚好装下"一定
  失败、差的还不是一两个字节（实测 2 692 B 的产出要 > 2 820 B 才写得下）。装不下就**翻倍
  重试**；成功的 `resize(len)` 把多出来的容量留在 `out` 里 ⇒ 同一形状的响应从第二次起
  一遍过、**稳态零分配**。这条路上**只有"缓冲不够"一种可失败原因**（`YYJSON_NULL_ALC` 下
  `realloc` 恒返空），超过 64 MiB 才转精确尺寸的分配式慢路（用模块自己的 `alloc_stats`
  分配器；任何尺寸都能成功，代价是大文档多一次拷贝）。`out` 长大那几步（`resize` /
  `assign`）**可能抛** `std::bad_alloc` —— 本模块不抛异常，那道壳把它收成 `NO_MEMORY`。三个写
  标志 `YYJSON_WRITE_LOWERCASE_HEX | YYJSON_WRITE_INF_AND_NAN_AS_NULL |
  YYJSON_WRITE_ALLOW_INVALID_UNICODE` 是"逐字节对齐旧写器"的关键：yyjson 默认的控制字符
  十六进制是**大写**、非法 UTF-8 默认换成 `�`。**不开** ESCAPE_UNICODE / ESCAPE_SLASH /
  PRETTY。无效视图仍写固定的 4 个字节 `null`（不经过序列化器）。

  **写的形态有两处如实不同（旧的"第 3 处不同"换了形状）**：① 科学记数法的**阈值**是
  ECMAScript 那条线（`1e-6 ≤ |x| < 1e21` 一律定点）⇒ `1e15` 写
  `1000000000000000.0`、`1e-5` 写 `0.00001`；② 正指数**不带 `+`** ⇒ `1e300` 而不是
  `1e+300`。整数、转义、UTF-8 三块与 nlohmann **逐字节相同**（`1.0` 现在两边都写 `1.0`
  —— 旧写器写的是 `1`，这一条**改判**）。**旧的"第 4 处不同"（写回递归）消失**：yyjson
  的写器用显式栈，两侧都不递归，十万层的文档写得出来（用例钉着）。另有一条**不对称**：
  `dump()` 遇到非法 UTF-8 会抛 `type_error.316`，这一侧原样透传（二进制安全）⇒ 含非法
  字节的语料上"逐字节比"没有对照物，用例里那条是单向的。

  **分配账**（本机实测，50 项的树、同一份数据）：构造侧 **13 次** `malloc`（同样的树走
  nlohmann 的 `std::map` DOM 是 **2,625 次** `operator new`；两把尺子都是"向堆要了几次"，
  不是同一个计数器）；写回那一侧得从**进程外面**数（写回用的是它自己那块账本，不记在
  builder 头上）：`LD_PRELOAD` 一个 `malloc` 计数器 ⇒ "建树 + 写回" **14.002 次/调用**、
  "只建树不写" **14.000**，即**写回自己不向堆要内存**（多出来的 0.002 是首次把缓冲长大
  那几次摊下来的）。同一棵树交给 yyjson 自己的 `yyjson_mut_val_write` 是 **27 次**。

  **写回那一侧的耗时**（同一棵 builder 树、8 397 B 的产出，min-of-7×3000、`taskset -c 10`）：
  我们这条路 **7.9 µs**、yyjson 自己的 `yyjson_mut_val_write` **8.9 µs**、直接往足够大的
  缓冲里写 **7.5 µs**；同一棵树的 nlohmann 路（DOM + `dump()`）**129.6 µs**。⇒ 换掉手搓
  写器不是性能牺牲，**写回还比自己调 yyjson 的 one-shot 写器快**（省掉它那条"小缓冲起步、
  按需长大"的路）。

  **用例**：`tests/functional/yyjson_doc_func.cpp` 按新形态重推（第 5/6/7/8 组；
  `1e+300` 那一份从"逐字节"里**拿出来**单列成"值相同、字节在第 3 处不同"的见证，另加
  两侧都不递归的那一条）；新增 `tests/functional/yyjson_build_func.cpp`（九组，见
  `doc/json-guide.md` §13.5/§13.8）。文件名含 `yyjson` ⇒ `tests/functional/CMakeLists.txt`
  那条 glob 过滤器自动覆盖，`YYJSON=OFF` 的树上用例数**不变**。

  **文档重新定位**：此前本仓自己把它叫"第二个后端、默认 OFF"（`tuned.md` 的调优清单里
  也列着 alternative serializer）。现在两侧合起来是"**官方并列后端**：解析与写入都出自
  本库 bundle 的这一份库，一行手搓序列化都没有"。`doc/json-guide.md` §13（含新增 §13.8）、
  `doc/ci-guide.md` 的 `yyjson` 那一节、两张 README 的表格格都按这个改。**CMake 默认值
  不动**（仍是 `OFF`，它面对的是最小源码构建）；"开箱即用"由六条发布腿满足。

### 配置、打包与 CI

- **`package_release.py` 的"Windows 自带模块"表补上 `odbc32.dll`**（`1.7.2-dev`）。
  这张表只用来判断"要不要打进包里"，漏一个的代价不是静默坏包而是**打不出包**：db 的
  ODBC 后端在 `UVCPP_DB_RUNTIME_LOAD=OFF`（**默认档**）的 Windows 构建上就是链
  `odbc32`，于是 `uvcpp.dll` 的导入表里有它，而打包那步的自检会把"非 Windows 自带"
  的导入项逐个去 `runtime_roots` 里找、找不到就 `missing[]` + 退出 2 —— 也就是说
  **默认配置在 Windows 上根本产不出包**（`odbc32.dll` 属操作系统组件，System32 里
  必有，与 `ws2_32.dll` 同类，既不可再分发也不需要分发）。

  这条是 MSVC 那两条 `config-contract` 腿抓到的：它们**不传** `UVCPP_DB_RUNTIME_LOAD`
  （走默认档、链导入库），而六条发布腿传 `ON`（符号由 `LoadLibrary` 取、导入表里没有
  它）—— 所以**发版路径上看不见这个形状**，只有 PR 上那两条腿能看见。这也正是那两条腿
  与发布腿"配置不逐字相同"的价值所在：它们各自量到一个方向的坏包。

- **六条发布腿显式打开 db 的第四个后端（ODBC），并且每一条都显式给出 `sql.h` 在哪**
  （`1.7.2-dev`）。此前 `release.yml` 全文零次提到 odbc：`UVCPP_ENABLE_DB_ODBC` 默认就是
  `ON`，而找不到 `sql.h` 时走的是 `message(WARNING)` + 普通变量 `set(... OFF)` ——
  `CMakeCache.txt` 里照旧写着 `ON`、生成头里却是 `0`、包里少一个后端，**一条断言都不会
  红**。十条 configure 全部补上 `-DUVCPP_ENABLE_DB_ODBC=ON` 与
  `-DUVCPP_DB_ODBC_INCLUDE_DIR=`，头的来源按腿分两种：两条 MinGW 腿用 MSYS2 的
  `mingw-w64-{x86_64,clang-aarch64}-unixodbc`（唯一一套**给 mingw 编的** ODBC 头，
  落在 `$MINGW_PREFIX/include/unixodbc/`；SDK 那份是给 MSVC 的，混进 mingw 是拿一个
  没人试过的组合去赌），两条 MSVC 腿用 Windows SDK 的 `<SDK>/Include/<ver>/um`
  （新脚本 `.github/scripts/win-odbc-headers.sh` 挑出来）；两条 Linux 腿照 MySQL/PG 的
  老规矩 `apt-get download unixodbc-dev` 加 `dpkg -x`，**不 install**。

  三个只有真编一次才会露出来的坑，都写进 `release.yml` 的注释了：SDK 那份**不能按字典
  序**挑（字典序把 `10.0.17763.0` 排在最前，于是 `um` 来自老 SDK 而 `<windows.h>` 来自
  新的，报出来是 `oaidl.h` / `propidlbase.h` / `sqltypes.h` 里成百条 `C4430` /
  `C2144`）；两条 Linux 腿的 `ldd` 名单与 `nm -D -u` 计数各补一条 ODBC 判据，而两条
  MinGW 腿的导入表白名单 **一个字都没动** —— 里面没有 `odbc32.dll`，那正是"运行期加载
  失效就会红"的唯一守卫。六条腿的模块断言（读的是生成头，不是 cache）因此从 17 枚变
  **18 枚**。

- **`unixodbc-dev` 的头布局随发行版变，而第一版判据把它写成了"发行版的属性"**
  （`1.7.2-dev`）。第一次空转（2026-10-17）两条 Linux 腿当场红在
  `linux-odbc-headers::unixodbc-dev 里没摊出 unixodbc.h`——**守卫本身没错**（宁可红，
  也不静默发一个少后端的包），错的是那个谓词。实测两份 `.deb` 的文件表：

  | | `sqltypes.h` 里的 include | `unixodbc.h` |
  |---|---|---|
  | jammy `2.3.9-5ubuntu0.1`（`ubuntu-22.04` / `-arm`） | 只有 `#include <wchar.h>` | **全包没有** |
  | noble `2.3.12`（开发机 Mint 22.3 那份） | 多一句 `#include "unixodbc.h"` | `usr/include/<multiarch>/unixodbc.h` |

  也就是说"少挂一层"这件事**只有 noble 那种布局才有**，在 jammy 上要求 `unixodbc.h`
  存在，等于把一个完全正常的包判红。改法是两半：软链那步变成"**有就挂、没有就算了**"，
  判据本身换成**编一次**——把后端真正 include 的 `<sql.h>` + `<sqlext.h>` 用
  `c++ -std=c++11 -I <摊开的 include 目录> -fsyntax-only` 过一遍。后者与发行版、与布局
  都无关，而且直接就是"这个后端能不能编"本身（本地对 jammy 平布局、noble 拆布局、以及
  故意放一个空 `sql.h` 三种情形各跑过一遍，前两种绿、第三种红并打出编译器原文）。
  软链那步另有一处细节写进注释了：`ln -sfn` 的目标按**链接所在目录**解析，喂相对路径会
  建出自指的坏链接（`$HOME` 派生出来的 `_inc` 本来就是绝对的，所以发布腿上不会犯）。
  **教训**：开发机的发行版不能当普遍规律 —— 这个坑就是照 noble 验出来、在 jammy 上红的。

- **两条 MinGW 发布腿在真编之前一次都没编过，一编就红：unixODBC 那份 `sqltypes.h` 的
  "补 windows 类型"段与 `winnt.h` 硬碰**（`1.7.2-dev`）。同一次空转里 `mingw-x64` /
  `mingw-arm64` 两条腿红在 `Configure & Build`，而整条腿**只有一处** error：

      sqltypes.h 第 109 行: error: conflicting declaration 'typedef short unsigned int WCHAR'
      winnt.h 第 320 行:    note: previous declaration as 'typedef wchar_t WCHAR'

  病灶是那段补丁的**开关**——它不叫"有没有 `<windows.h>`"，叫
  `ALREADY_HAVE_WINDOWS_TYPE`，而**这个宏谁都不定义**，所以 `<windows.h>` 明明已经在前面
  进来过，它照样把 ODBC 那套类型补一遍。补出来的 `CHAR` / `WORD` / `DWORD` / `BYTE` /
  `LPDWORD` 与 `winnt.h` 逐字相同（C++ 里重复 typedef 合法，一声不响），只有 `WCHAR`
  不同：它给 `unsigned short`，`winnt.h` 给的是 `wchar_t`。MSVC 上永远碰不到，因为
  Windows SDK 那份 `sqltypes.h` 是微软自己写的、没有这一整段补丁 —— **所以这个坑与上一段
  的"头布局"同源：都只在一条腿的平台上现形**。

  解法用这段补丁自己的开关 `SQL_WCHART_CONVERT`（`sqltypes.h` 第 106 行）：打开后 `WCHAR` 就是
  `wchar_t`，与 `winnt.h` 逐字相同、冲突消失。这不是权宜之计，语义上本来就该这样 ——
  Windows 的 ODBC 宽字符接口就是 `wchar_t`（2 字节），`unsigned short` 那个是 unixODBC 给
  "没有 `<windows.h>` 的平台"准备的默认值（它的注释自己写的）。`SQLWCHAR` 由
  `typedef WCHAR` 派生，跟着一起对上，**尺寸不变、仍是 2 字节** ⇒ 后端里那条"按
  `sizeof(SQLWCHAR)` 编译期分 UTF-16 / UTF-32"的转换走的还是 MSVC 上已经在跑的那条路，
  **不新增代码路径**。判据写成"`_WIN32` 但不是 `_MSC_VER`"而不是 `__MINGW32__`（真正的分界
  是"手上这份 `sqltypes.h` 是 unixODBC 的还是微软的"；万一有人拿 mingw 配 SDK 的头，这个宏
  在微软那份里一次都没被引用、是空操作）；**Linux 上必须不开** —— 那里 `wchar_t` 是 4
  字节，开了就是把 `SQLWCHAR` 从 2 字节改成 4 字节，那是改 ABI 不是修 bug。

  同一条腿还带一句 `'CALLBACK' redefined` **警告**（同一段补丁的 `#define CALLBACK` 空定义
  盖掉了 `minwindef.h` 的 `#define CALLBACK __stdcall`，而且它**出完 include 不复原**）。
  它不致命，但等于把这个宏借走不还，所以顺手用 `push_macro` / `#undef` / `pop_macro` 借
  出去再还回来；万一哪个编译器不认这两个 pragma，按标准它们是被忽略的，结果退回原样、
  不更坏。

  **教训与上一段同一个，只是换了个维度**：上一段是"开发机的发行版不能当普遍规律"，这一段
  是"**Linux 上编得过不能推出 mingw 上也编得过**"—— 这两条腿的头是按 `packages.msys2.org`
  的文件表核的、不是编出来的，而"文件表里有 `sql.h`"与"这份 `sql.h` 能和 `<windows.h>`
  一起编"是两件事。

- **`RELEASE.md` 的「发布包」那张表补上 1.7.0 那一轮**（`1.6.61-dev`）。表原来只有
  `1.5.2` / `1.5.3` 两行（小标题也写着"两轮扩容"），而 1.7.0 把包里的模块从十三个扩到
  十七个这件事**没进表** —— 按"哪一版多了什么"去查的人会查到一份停在这里的账。补上
  `1.7.0` 那一行，并写清这一轮的形状与前两轮不同：前两轮是"包里多了模块"，这一轮还多
  一层 —— 两个远程后端进了包却**不进链接行**（`UVCPP_DB_RUNTIME_LOAD`，`DT_NEEDED` 是
  加载期解析）；另外 1.7.0 之前那个差集的方向是**反的**，是"发布包比源码默认构建少
  两个模块"。

- **Linux 两条发布腿不再传 `UVCPP_STATIC_RUNTIME`：带它的共享 `.so` 与使用者那份
  `libstdc++` 是互斥的**（`1.6.60-dev`）。上一条把取头那步修好之后，空转第一次真的走到
  `Test` 那一步，两条 Linux 腿当场红在同 9 条用例上（5 条 SEGV、4 条 SIGABRT，x64 与
  arm64 逐条相同），全是"跨线程 + future/promise"那一类。

  根因是**同一个进程里两份 libstdc++**：`UVCPP_STATIC_RUNTIME` 在 UNIX 上把
  `-static-libgcc -static-libstdc++` 同时加进 `CMAKE_SHARED_LINKER_FLAGS` 与
  `CMAKE_EXE_LINKER_FLAGS`（`CMakeLists.txt` 的 `UVCPP_STATIC_RUNTIME` 那一段），于是
  发布的 `libuvcpp.so` 与每一个链它的可执行文件各自带一份。`std::once_flag` /
  `std::__future_base` 那族模板实例是 COMDAT、两边都进导出表，而 ELF 的动态解析是
  **可执行文件优先** ⇒ 一侧的 `__once_proxy` 跳进另一侧的 `_FUN`。gdb 抓到的正是
  `uvcpp_loop_worker::thread_main(std::promise<int>*)` 里 `pthread_once` 那一步；`ldd`
  也证实两边都不需要 `libstdc++.so.6`（各自静态带了一份）。

  **这不是用例特有的毛病**（所以"改用例"这条退路不存在）：把 `.so` 摆成静态
  libstdc++、只让用例去链系统那份 `libstdc++.so.6` —— 也就是**任何一个普通使用者的
  形状** —— 同一条用例照样段错误。本机 `build-relrepro` 两种形状各量了一遍；把开关
  去掉之后 9 条全绿、整棵 136 条用例零失败（2 条 db 用例因没起数据库而跳过）。

  改法是**两条 Linux 腿的四处 configure 都不再传它**（`release.yml`），两条 MinGW 腿
  照旧 —— PE 没有全局符号作用域，两份不会互相顶掉，而 MinGW 用户机器上未必有
  `libstdc++-6.dll`。"可以被别人直接使用"与"静态 libstdc++ 的 .so"两者只能选一个，
  选前者。代价是 Linux 包的 `ldd` 从此列出 `libstdc++.so.6` 与 `libgcc_s.so.1`（装了
  apt 的 Linux 上本来就有，`libstdc++6` 是 apt 自己的依赖）；那个步骤相应地从"警告"
  改成"记录实测值"，并写明它**不在**了才是要回头查的形状。

  **它为什么能藏 68 个提交**：Linux 侧唯一传这个开关的格子是 `config-contract`，而那
  棵树 `UVCPP_BUILD_TESTS=OFF`（`check_doc_snippets.py` 只 `-c` 编译片段、不链接也不
  执行），所以这个形状在 push/PR 上**只被编过、从没被跑过**；发布腿那边 `Test` 一步
  自 `v1.6.4`（`faf1e50`）起就没绿过 —— 前面取头那步先红了，轮不到它。
  `doc/ci-guide.md` 的"已知缺口"表、`doc/build-guide.md` 的 Runtime behaviour 一节与
  两条 Linux 腿的 configure 上方现在都记着这条。

- **两条 Linux 发布腿的客户端头取不到：`apt-get download` 以 100 退出**（`1.6.57-dev`）。
  这是 1.7.0 的空转（`release.yml` 的 `workflow_dispatch` + `dry_run=true`）第一次
  真的跑那一步时红出来的，两条腿同一条：

  ```
  E: Can't find a source to download version '18.6-1.pgdg22.04+2' of 'libpq-dev:amd64'
  E: Can't find a source to download version '18.6-1.pgdg22.04+2' of 'libpq-dev:arm64'
  ```

  根因不在我们这一侧的写法：runner 镜像上另外有一份 PGDG 的 `libpq-dev`
  （`18.6-1.pgdg22.04+2`），版本号比发行版归档里那份高，**却没有可取回的源** ——
  `apt-get download` 挑的是"版本最大的那个"，于是在取包之前就以 100 退出。

  **修法走错过一次，记在这里免得下次重走**：第一版写的是把候选钉回发行版那一档，
  `apt-get download -t jammy …`，结果第二次空转照样红在同一条上。原因是
  **`apt-get download` 不看任何 pin** —— `-t`（990）与一份 `Pin-Priority: 1001` 的
  preferences 都改不动它的候选，本机对同一台机器上 `libpq-dev` 的两个候选
  （noble-updates 16.15 / noble 16.2）量过，两次都取到 16.15。第一次写这条时"本机
  验过 `-t` 可用"是个**不成立的对照**：本机没有第二个来源，取到同一个版本说明不了
  任何事。

  真正的修法是**自己把版本号给出来**、按 `pkg=ver` 交回去，让 apt 只负责取、不负责
  挑（`1.6.58-dev`）。但"怎么挑这个版本号"又走错了一次，**第三次空转**当场证伪
  （`1.6.59-dev` 改正）：第一版写的是"取 `madison` 里源 URL 带 `ubuntu.com` 的那一行"，
  而 runner 把源重写成 `mirror+file:/etc/apt/apt-mirrors.txt`，那一列里根本没有域名
  （`libmysqlclient-dev` 三条全是这个形状），于是守卫每次都报"没有可取回的版本"。
  守卫本身写对了 —— 它宁可红，也不静默发半个包 —— 错的是那个谓词。最终改成
  **把 `madison` 列出的版本一版一版试取**：取不取得到直接问 apt，不从来源名去推断。
  这一段本机用真 apt 整块跑过三条路：正常（第一版就取到，`mysql.h` 与 `libpq-fe.h`
  落在约定路径上）、runner 那个形状（最高那版取不到 → 打印一行、自动换下一版 →
  rc=0）、以及**每一版都取不到**（`::error` + rc=1）。

  同一次空转里另外四条腿（MinGW x64/arm64、MSVC x64/arm64）**全绿**：新加的
  `.github/scripts/win-client-headers.sh` 在真镜像上把两份钉版本的 MSYS2 头取到并用
  上了，六条腿的 17 枚模块断言在 Windows 侧全过，MinGW 那条自包含断言实测
  `libuvcpp.dll` 的导入表里只有 Windows 自带 DLL（没有 `libpq`/`libmariadb`）——
  `UVCPP_DB_RUNTIME_LOAD` 的 dlopen 设计在真发布腿上成立。

- **两版 README 的功能覆盖数字按新的 MinGW 格改准**（`1.6.56-dev`）。上一条把
  `ci-mingw64.yml` 那条**单 job** 也开了 yyjson 与 brotli，而 README 的
  `UVCPP_ENABLE_YYJSON` / `UVCPP_ENABLE_BROTLI` 两行还写着旧的覆盖数（"三个"/"四个
  CI 条目"，且只列矩阵里的那几格）。改成点名那条单 job 的新数字，并给
  「Optional features」表补上一直缺的 yyjson 一行 —— 那张表原先只列 web/net 的开关，
  引子句"`UVCPP_BUILD_WEB=ON` 一个都不会替你打开"却写得像整表都是 web 的，一并
  改准（yyjson 属 `json` 模块）。

- **预编译包第一次真的"全功能"：yyjson、brotli 与 db 的两个远程后端都进包**
  （`1.6.55-dev`）。此前 `release.yml` 十条 configure 里**一条都不传**
  `-DUVCPP_ENABLE_YYJSON` 与 `-DUVCPP_ENABLE_BROTLI`，而 MySQL/PG 是**显式
  `=OFF`** —— 所以"包里是全功能的"这句话有一处差集，且它的方向是**发布包比源码
  默认构建少**。十条 configure 各补：

  ```
  -DUVCPP_ENABLE_YYJSON=ON
  -DUVCPP_ENABLE_BROTLI=ON -DUVCPP_BROTLI_FROM_SOURCE=ON
  -DUVCPP_ENABLE_DB_MYSQL=ON -DUVCPP_ENABLE_DB_PGSQL=ON -DUVCPP_DB_RUNTIME_LOAD=ON
  ```

  brotli 必须**同时**传 `UVCPP_BROTLI_FROM_SOURCE=ON`：它的发现顺序是"先系统后
  源码"，而发布腿不能听天由命 —— 镜像哪天装上 `libbrotli-dev`，配置期就会**静默**
  改走共享库，产物多出三条 `DT_NEEDED` 而**没有任何断言会红**（`ldd` 名单原本也
  没有 brotli，已补上 `libbrotlienc/dec/common` 与 `mysqlclient`/`pq`）。
  MySQL/PG 走运行时加载（上面 1.6.54-dev 那条），所以它们的客户端库仍然
  **不在**链接行上 —— 两条 Linux 腿为此新增一条反向断言：`nm -D -u` 里未定义的
  `mysql_` / `PQ` 符号数必须是 0。

- **六条腿的模块断言从 13 枚扩到 17 枚**（`1.6.55-dev`）。新增
  `UVCPP_YYJSON_ENABLE`、`UVCPP_BROTLI_ENABLE`、`UVCPP_DB_MYSQL_ENABLE`、
  `UVCPP_DB_PGSQL_ENABLE`。判据仍读**生成的头**（`<tree>/include/uvcpp/
  uvcpp_config.h`）而不是 `CMakeCache.txt` —— db 那族在静默降级时缓存照旧写着
  `ON` 而编译器看到 0，只 grep 缓存的断言会放行一个缺功能的包。

- **Windows 上的 MySQL/PG 头**（`1.6.55-dev`）。两条 MSVC 腿的镜像上没有这两个
  客户端库的开发头（`windows-11-arm` 上连 PostgreSQL 都没有），所以四条 Windows
  腿**共用同一份钉死版本的头**：新增 `.github/scripts/win-client-headers.sh`，从
  MSYS2 的 `.pkg.tar.zst`（`mirror.msys2.org` 上的**不可变 URL**，版本号写在文件名
  里）取出来放进 `C:/client-headers/{mysql,pgsql}`，两条 MSVC 腿各加一步调用它。
  Linux 两条腿反过来：`apt-get download libmysqlclient-dev libpq-dev` + `dpkg -x`
  摆一份头出来，**一个包都不装** —— jammy 上这两个都 `Depends: libssl-dev`，而
  发布腿有一条"**不装** libssl-dev"的不变量（22.04 那份是 3.0.2、没有 QUIC API，
  装上会让 `find_package` 静默产出一个坏包）。四个 include 目录用
  `-DUVCPP_DB_{MYSQL,PGSQL}_INCLUDE_DIR=` 直接喂进去（`find_path` 认预置的 cache
  变量），绕开整个发现过程。

- **`check_doc_snippets.py` 的 `DEFAULT_OFF` 里删掉 `UVCPP_YYJSON_ENABLE`**
  （`1.6.55-dev`）。那张表只在宏为 0 时救场；发布腿开了 yyjson 之后它就变成了
  "结构性关着"里的例外。删掉它**必须**与三个 config-contract 档（Linux / MSVC /
  MinGW）同时传 `-DUVCPP_ENABLE_YYJSON=ON` 一起做 —— 否则那三档会在
  `doc/json-guide.md` 上退 3，而症状看起来像文档腐烂。本机实测两条方向都验过：
  yyjson 开着的包 `[绿] doc/json-guide.md 编过 8/8 条`、全库 102/202、`rc=0`；
  换一棵 yyjson 关着的旧包，同一条片段变 `[跳]`、整条**退 3**。

- **新增开关 `UVCPP_DB_RUNTIME_LOAD`（默认 `OFF`）：db 的 MySQL/PG 后端不再把
  客户端库写到链接行上**（`1.6.54-dev`）。理由与实测都在上面「数据库」那一节；
  这一条只记配置/打包面的落点：`CMakeLists.txt` 里 MySQL 那段的 `find_library`
  与 PG 那段的 `find_package(PostgreSQL)` 都被 `if(NOT UVCPP_DB_RUNTIME_LOAD)` 绕开
  （只留 `find_path` 找头，PG 那边**故意不用** `FindPostgreSQL` —— 它的判据一个包
  同时含头与库，还会去读 `PostgreSQL_LIBRARY` 那个 cache 变量），
  `UVCPP_DB_RUNTIME_LOAD` 同时进 `cmake/uvcpp_config.h.in`（带 `#error` 反向守卫）
  与 `check_config_contract.py` 的 `MACROS`。开关名与宏名同名，靠
  `_uvcpp_literal01(UVCPP_DB_RUNTIME_LOAD UVCPP_DB_RUNTIME_LOAD)` 这一句统一。
  打包侧：三个新私有头进了 `package_release.py` 的 `PRIVATE_HEADERS`
  与 `CMakeLists.txt` 的 `install(FILES)` 排除（两处是一对，只改一处不报错）。
  README 两版与 `doc/build-guide.md` 的开关表、`doc/db-guide.md` 新增的
  「客户端库运行时加载」一节一起改
- **静态链接进来的依赖，符号一并导出去了 ⇒ ELF 上给共享目标加
  `--exclude-libs,ALL`，yyjson 那半另修**（`1.6.53-dev`）。本仓在非 Windows 上 `UVCPP_API` 是**空的**
  （`src/uvcpp/uvcpp_export.h:17`），也没有 `-fvisibility=hidden`、没有 version
  script ⇒ 凡是**静态归档**链进来的东西，符号全都进了 `.dynsym`。实测（`build-vfy-cfg`，
  `DT_NEEDED` 里没有 libssl 的那棵树）：

  ```
  nm -D --defined-only build-vfy-cfg/libuvcpp.so | grep -cE ' [TWiDBR] SSL_new$'   → 1
  ```

  `SSL_new` / `SSL_CTX_new` / `EVP_EncryptInit_ex` / `X509_new` 四个一个不少，静态
  nghttp2 的 `nghttp2_session_client_new`、`uvcpp_sqlite3` 的 `sqlite3_open` 同样在。
  也就是说 `libuvcpp.so` 当时在**全局作用域**里对外冒充了一份 OpenSSL。

  为什么这不是洁癖、而是必须在"db 后端运行时加载"之前修掉：那条路要
  `dlopen("libpq.so.5")`，而 **libpq 自己要用 OpenSSL**。它那份按系统 OpenSSL 编的
  `SSL_*` 会先在全局作用域撞上我们导出的这一套，同时它 `NEEDED` 的 `libcrypto.so.3`
  也被加载 ⇒ 同进程两份 libcrypto，`X509`/`BIGNUM` 一手建一手放 ⇒ 堆损坏。（这不是
  dlopen 新造的毛病：今天任何"既链 `libuvcpp.so` 又链 `libpq.so.5`"的程序已经踩它；
  dlopen 只是把这条路变成**每个装了 libpq 的人**的默认路径。）把我们的嵌入依赖藏起来，
  libpq 就会解析到**系统**那份、与它自己的 libcrypto 自洽。

  修法一（`CMakeLists.txt:2516-2558`，`UNIX AND NOT APPLE`，只给共享目标
  `uvcpp`/`uvcpp_s`）：`target_link_options(… PRIVATE "LINKER:--exclude-libs,ALL")`。
  它只藏**来自静态归档**的符号，而这两个目标是
  `add_library(uvcpp SHARED ${UVCPP_SOURCES} …)`（`CMakeLists.txt:2216-2216`）
  **直接拿我们自己的目标文件**建的、不经归档 ⇒ 我们自己的 API 与 `uvcpp_c_*` 那套
  C ABI 照样导出。只给共享目标加是因为静态库没有导出表，藏了反而让 `uvcpp_a` 的消费者
  在最终链接时找不到本该由我们提供的符号；只给 ELF 加是因为 Apple 的 ld 没有这个选项
  （那边是 `-exported_symbols_list`），而发布腿里本来也没有 macOS。

  修法二：`uvcpp_yyjson` 是 **OBJECT** 库，它的目标文件经
  `$<TARGET_OBJECTS:>`（`CMakeLists.txt:2847-2847`）**直接进最终链接、也不经归档** ⇒
  上面那条对它无效（实测加不加都是 43 个 `yyjson_*`）。那边改用
  `C_VISIBILITY_PRESET hidden` **加** `target_compile_definitions(uvcpp_yyjson PRIVATE
  yyjson_api=)`。**第二半是承重的**：上游 `yyjson.h` 给每个公开函数**显式**贴了
  `__attribute__((visibility("default")))`，显式属性压倒 `-fvisibility=hidden` ——
  只加 preset 仍是 43 个；所幸上游把那一整块包在 `#ifndef yyjson_api` 里（那是个有意的
  覆盖点），命令行上先定义成空 ⇒ 头里那段 `#define` 不进编译。藏它对谁都没影响：全仓
  只有 `src/yyjson/uvcpp_json_doc.cpp` 一个文件 include `<yyjson.h>`，公开头里一个
  yyjson 类型都不出现。

  判据 —— **同一批目标文件、同一条链接命令，只差那一个 `-Wl,--exclude-libs,ALL`**
  （从 `build-vfy-cfg` 的 ninja 链接行里删掉该参数重链到 `/tmp`，两边的导出表逐条比）：

  | 量 | 改之前 | 改之后 |
  |---|---|---|
  | `--defined-only` 导出总数 | 18031 | **4012** |
  | `SSL_new` / `SSL_CTX_new` / `EVP_EncryptInit_ex` / `X509_new` | 1/1/1/1 | **0/0/0/0** |
  | `nghttp2_session_client_new` / `sqlite3_open` | 1/1 | **0/0** |
  | 名字里含 `uvcpp` 的导出 | 3674 | **3674**（被藏掉的 14019 个里，0 个是我们的） |
  | `yyjson_*`（`build-yyjson`） | 43 | **0** |

  两条变异都得红，实测都红：① 链接行去掉 `--exclude-libs,ALL` ⇒ 总数回到 18031、
  `SSL_new` 回来；② 编译 `yyjson.c` 时去掉 `-Dyyjson_api=`（保留 preset）⇒ 43 回来；
  去掉 `-fvisibility=hidden`（保留空定义）⇒ 43 也回来 —— 两半各自承重，不是二选一。
  另外 `ctest`：`build-all` 127/127、`build-yyjson` 93/93、`build-brotli2` 92/92；
  `check_capi_symbols.py --tree build-vfy-cfg` 6 片逐条对上（C ABI 锁文件一个字没动）、
  `check_config_contract.py` 的 `.pc` 可链接性与 C 消费方判据仍绿 —— 这两条绿就说明
  藏掉的确实只有"别人的"符号。

- **brotli 到底链系统那份还是自己取源码，从来没被钉住 ⇒ 新增
  `UVCPP_BROTLI_FROM_SOURCE`**（`1.6.52-dev`）。发现顺序是 pkg-config →
  `find_path`/`find_library` → FetchContent，也就是**先系统后源码**：在装了
  `libbrotli-dev` 的机器上，"链的是静态 `.a`"会**静默**变成"链系统那份 `.so`"，
  产物上多出 `libbrotlienc` / `libbrotlidec` / `libbrotlicommon` 三条 `DT_NEEDED`
  —— 与发布包"除 libc 外无运行时依赖"直接冲突。CI 上一直没撞上只是因为那几台
  runner 碰巧没装 brotli；而这一版的发布包要在**全平台**把这个后端打开，
  靠"碰巧"不行。当时更麻烦的是**不会有任何一道断言变红**：`release.yml` 那条 ldd
  黑名单里并没有 `brotli`。

  zlib 那条的挡法在这里**不管用** —— `-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON` 只挡
  `find_package`，挡不住 `pkg_check_modules`；而 `CMAKE_IGNORE_PATH` 本仓已经明文
  记过是陷阱（它只骗得过 CMake 的 `find_*`，骗不过编译器自己的默认搜索路径）。

  修法照 `UVCPP_DB_SQLITE_FROM_SOURCE`（`CMakeLists.txt:189`）的命名与形状加一个
  **默认 OFF** 的选项（`CMakeLists.txt:246-247`），把整段发现包进
  `if(NOT UVCPP_BROTLI_FROM_SOURCE)`（`CMakeLists.txt:422-422`）。注意是**连发现都不
  调用**，不是"找到了也偏不用"：`find_path`/`find_library` 的结果会写进 cache，
  调用过一次，`UVCPP_BROTLI_INCLUDE_DIR` 之类就一直留在那棵树里，同一棵树翻开关时
  会把上一轮的答案漏过来。所以每个"找到系统那份"的判据都**显式**带上
  `NOT UVCPP_BROTLI_FROM_SOURCE`（`CMakeLists.txt:433-433`、`CMakeLists.txt:437-437`、
  `CMakeLists.txt:445-445`），而不是指望"没调用 = 没找到"。取源码那条支路一个字没改：
  选项开着 ⇒ 跳过发现 ⇒ `BROTLIENC_LIB` 仍是 `NOTFOUND` ⇒ 自然落进那一支 ⇒
  那里照旧 `set(UVCPP_BROTLI_FROM_SOURCE TRUE)`（`CMakeLists.txt:523-523`），链接段
  （`CMakeLists.txt:2700-2700`）也就照旧按 `$<TARGET_FILE:...>` 取静态库。默认值一个都没
  动，所以既有使用者的行为逐字节不变。

  判据（本机，`build-brotli2`：`-DUVCPP_ENABLE_BROTLI=ON
  -DUVCPP_BROTLI_FROM_SOURCE=ON -DFETCHCONTENT_SOURCE_DIR_BROTLI=/tmp/brotli-src`）：
  ① 配置打印 `Fetching brotli via FetchContent (tag=v1.1.0)`，configure rc=0；
  ② **`-M` 解析出的头路径**是 `/tmp/brotli-src/c/include/brotli/encode.h`，不是
  `/usr/include/brotli/encode.h`（后者才是"系统那份又顶上来"的样子 —— 这条判据的
  形状是 `1.6.50-dev` 那条教训留下的：`CMAKE_IGNORE_PATH` 那套配方能让本机**全绿**
  而编译行上一个 brotli 目录都没有）；③ `readelf -d libuvcpp.so | grep NEEDED |
  grep -ci brotli` = 0；④ `ctest` 92/92。
  ⑤ 另一半判据被证明是**承重的**，不是想当然：先用
  `-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON` 另建一棵树逼它走 `find_*` 支路
  （cache 里于是留下 `UVCPP_BROTLI_INCLUDE_DIR:PATH=/usr/include` 与
  `UVCPP_BROTLIENC_LIB:FILEPATH=/usr/lib/x86_64-linux-gnu/libbrotlienc.so`），再在
  **同一棵树里**把选项翻成 `ON` 重配 —— 仍然打印 `Fetching brotli via
  FetchContent`。
  MSVC / MinGW 那两条腿本机判不了（没有 Windows 工具链），等 CI。

- **brotli 取源码支路把 DLL 的路径写进了链接行 ⇒ MSVC 的 `brotli` 格
  `LNK1107`**（`1.6.51-dev`）。`1.6.50-dev` 把 include 目录补上之后，那一格从
  `error C1083` 变成

      _deps\brotli-build\Release\brotlienc.dll : fatal error LNK1107:
      invalid or corrupt file: cannot read at 0x2F8

  根因是上游 brotli 的 `option(BUILD_SHARED_LIBS "Build shared libraries" ON)`
  （v1.1.0 上游自己的 `brotli/CMakeLists.txt:13`）**默认就是 ON**，而它是全局的、随使用者的缓存变化
  —— 我们在根上也是 ON，于是取下来的 brotli 建出的是 **DLL**，
  `$<TARGET_FILE:brotlienc>` 求值成 `brotlienc.dll`，MSVC 只接受 `.lib`（静态库或
  导入库）。把 `.so` 的路径丢给链接器在 Linux 上是合法的，所以本机与 Ubuntu 腿
  **全绿**、又是只有 MSVC 红 —— 与 `1.6.50-dev` 那条同一种形状。

  修法照 `nghttp2` 那条抄：用一个 function 缩作用域把 `BUILD_SHARED_LIBS` 压成
  `OFF`（上游 `cmake_minimum_required(VERSION 3.15)` ⇒ CMP0077 是 NEW ⇒ **普通变量**
  就压得住它的 `option()`，不用 `CACHE ... FORCE`、也就不污染根 cache）。顺带这也是
  **刻意的**：链 DLL 会让用我们库的人多两个运行时依赖，nghttp2 那条明确选过"静态是
  刻意的，packager 的 DLL 清单一行都不用改"。

  两处配套：

  1. **`brotlicommon` 必须一起列**。路径形式不带使用要求，`brotlienc` 对
     `brotlicommon` 的那条 PUBLIC 依赖不会跟过来 —— 以前是**碰巧对**的（那时拿到的是
     `.so`，它的 `NEEDED` 自记着 brotlicommon），三个都变静态之后少一个就是
     `undefined reference to BrotliEnsureCapacity` 那一族。
  2. **"没压住"必须当场红**：`add_subdirectory` 之后逐个查
     `get_target_property(... TYPE)`，不是 `STATIC_LIBRARY` 就 `FATAL_ERROR`。
     拖到链接期看到的是 `brotlienc.dll : LNK1107`、指向我们自己写的
     `target_link_libraries`，看着像我们把库名写错了。

  判据（本机，取源码支路的树）：① `CMakeFiles/uvcpp.dir/link.txt` 里三个都是
  `<brotli-build>/libbrotli{enc,dec,common}.a`（改之前是 `.so`）；
  ② `ldd libuvcpp.so` **没有** `libbrotli*`（改之前有三条）—— 这一条同时证明
  "不新增运行时依赖"真的做到了；③ `nm -D --defined-only libuvcpp.so | grep -c brotli`
  = 39，符号确实并进来了；④ `make -j8` rc=0、0 error，`ctest -R "compress|brotli|static"`
  3/3 过；⑤ 系统 brotli 那条支路（`find_library`，Ubuntu 的 `libbrotli-dev`）
  重新配置 + 编 `uvcpp_a` rc=0，**没被这次改动碰到**（`UVCPP_BROTLI_LIBS` 的赋值在
  `UVCPP_BROTLI_FROM_SOURCE` 分支里，链接行那段只改了 that 分支）。
  MSVC 那条腿本机判不了（没有 Windows 工具链），只能等 CI。

- **brotli 的 FetchContent 支路一个 include 目录都没加 ⇒ MSVC 的 `brotli` 格
  `error C1083`**（`1.6.50-dev`）。`UVCPP_ENABLE_BROTLI=ON` 而机器上没有系统 brotli
  时（macOS 与 MSVC 都走这条），`src/web/uvcpp_http_compress.cpp` 的
  `#include <brotli/encode.h>` 编不过。根因是我们自己那套 `install(EXPORT)` 绕法：
  链接写的是 `$<BUILD_INTERFACE:$<TARGET_FILE:brotlienc>>`，它求值成一个**路径
  字符串**、**不带使用要求** —— 而按目标名链接本来会把 `brotlienc` 的
  `INTERFACE_INCLUDE_DIRECTORIES` 一并带过来，换成路径形式就没了。系统那条支路拿到
  的是 `PkgConfig::BROTLIENC`（IMPORTED 目标），一直是好的，所以只有取源码这一支中招；
  这一格从 `1.6.48-dev` 加上起就一直红，只是被同一格的贪婪 `\x` 报错挡在前面。

  ⭐ **这条原来"能在本机量"，但量具本身是坏的。** 原配方用
  `-DCMAKE_IGNORE_PATH='/usr/include;...'` 把系统 brotli 藏起来以强制走取源码支路，
  可 `CMAKE_IGNORE_PATH` **只骗得过 CMake 的 `find_*`，骗不过编译器自己的默认搜索
  路径**：`UVCPP_BROTLI_INCLUDE_DIR` 确实变成 `NOTFOUND`、支路也确实走对了，但
  `#include <brotli/encode.h>` 仍由 `/usr/include/brotli/encode.h`（`libbrotli-dev`）
  解析掉 ⇒ **本机全绿，而编译行上一个 brotli 目录都没有**。判据因此改成**看 `-M`
  解析出的实际路径**，而不是"编过了"：取 `compile_commands.json` 里那条 TU 的命令行
  加 `-M`，修复前解析到 `/usr/include/brotli/encode.h`、修复后解析到 FetchContent 的
  `${brotli_SOURCE_DIR}/c/include/brotli/encode.h`；`flags.make` 的 `CXX_INCLUDES`
  也从"没有 brotli 目录"变成 `-I<brotli 源码>/c/include`。修复是一行
  `set(UVCPP_BROTLI_INCLUDE_DIRS ${brotli_SOURCE_DIR}/c/include)`（下面那条
  `if(UVCPP_BROTLI_INCLUDE_DIRS)` 早就在，只是取源码支路从没喂过它），再加上把这条
  误导性配方与它的教训写进那段注释。

- **`uvcpp_json_write` 的 friend 声明排在了带 `UVCPP_API` 的声明前面 ⇒ MSVC 的
  `yyjson` 格 `error C2375`**（`1.6.50-dev`）。`src/yyjson/uvcpp_json_doc.h` 里这个
  函数要读 `uvcpp_json_value` 的私有成员，于是先在类内以 `friend`（**不带**
  `UVCPP_API`）出现、之后才在命名空间级以 `UVCPP_API` 出现。MSVC 要求
  `__declspec(dllexport/dllimport)` 落在**第一条**声明上，顺序反了就是
  `error C2375: 'uvcpp::uvcpp_json_write': redefinition; different linkage`；GCC 与
  clang 对属性后置不敏感（认作同一实体），所以本机两条腿都绿、只有 MSVC 红。修法是把
  带 `UVCPP_API` 的那条声明**提到类定义之前**（紧跟 `uvcpp_json_value` 的前置声明），
  类里那个 friend 退化成一条普通重声明。

  这一处**是本仓唯一一处这种形状**：扫 `src/**/*.h` 找"同一个函数先是类内 friend、
  后面又有列 0 起的 `UVCPP_API` 声明"，全仓只有 1 个函数，修复后属性声明在前。
  判据：新建一棵 `UVCPP_ENABLE_YYJSON=ON` 的树（`web` + `webapp` + zlib，与 CI 那一格
  逐字相同的 `flags`）编过（`0 error`，`test_yyjson_doc_func` 在其中），
  `ctest -R "yyjson|json"` **5/5** 过；两条 yyjson TU 另过一遍
  `clang++-18 -fsyntax-only`（rc=0）。MSVC 那条腿本身本机判不了（没有 Windows
  工具链），只能等 CI。

- **`\x1f` 字面量被贪婪的 `\x` 吃掉 ⇒ macOS / MSVC 两条腿编挂**（`1.6.49-dev`）。
  `tests/functional/web_app_response_func.cpp` 里那个变体 tag 写成了整串
  `"json\x1f50\x1f6"`。C/C++ 的 `\x` 转义**贪婪** —— 它把后面所有十六进制位都吃掉，
  于是 `\x1f50` 是一个越界码位（0x1f50 > 0xFF），`\x1f6` 同理。**clang 与 MSVC 判
  error**（`error: hex escape sequence out of range` / `error C7744: escape sequence
  '\x1f50' out of range`），**GCC 只给一条 warning 然后按低字节截断**（rc=0）。本机
  开发在 Linux 上，于是这条红从引进它的 `2a622cb`（`1.6.31-dev`）起挂了 17 个开发档，
  直到 macOS 腿报出来 —— 而且是**十二条腿里红六条绿六条**：红的是带
  `-DUVCPP_BUILD_WEBAPP=ON` 的那六格（`web`/`yyjson`/`brotli`/`ssl`/`h2`/`capi`，
  MSVC 上还多一条 `wsdl`），`full`/`quic`/`http3` 没开 WEBAPP、`basic-*`/`db` 关了
  WEB，七格都没编到这个文件。**「某格绿」要问的是这一格编没编到** —— 这一条红能漏
  17 个档，正是因为绿的那几格本来就没编它。

  修的不只是那两处字面量：写入与断言当时用的是**同一串**错字面量，两边一起被 GCC
  截断 ⇒ 比得再严也恒真，**这条判据在 Linux 上根本测不出来**。现在字面量按段拼
  （`"json\x1f" "50\x1f" "6"`，转义在引号处终止），并**断言真实字节**（长度 9、
  `[4]`/`[7]` 是 `'\x1f'`、`"json"`/`"50"`/`'6'` 各就各位）—— 那一条在 GCC 的截断下
  也会红，把「只有非 GCC 工具链能发现」变成「三条腿都能发现」。

  判据（都在本机量的）：① 同一条 TU 的真实命令行、只把编译器换成 `clang++-18`
  （`-fsyntax-only`），HEAD 版 2 个 error（485:20、486:35，与 CI 批注逐字对上）、
  修好后 0 个；② 另起一棵 `clang++-18` 的全量树（WEB + WEBAPP + BROTLI + TESTS）
  编过 —— clang 碰到第一个 error 就停，所以只看 CI 那条批注**说明不了别的 TU** ——
  `0` 个 error、`ctest 92/92`，`test_web_app_response_func` 在其中（`#59 Passed`）；
  ③ GCC 腿不劣化：重编该目标并跑，`[response] ALL PASS`。

- **brotli 有了自己的 CI 腿：四格（Ubuntu 两格 / macOS / MSVC）**（`1.6.48-dev`）。
  `UVCPP_ENABLE_BROTLI` **默认 OFF**，别的格一行 brotli 的代码都不编 —— 与 `wsdl`
  和 `yyjson` 同一个形状的老问题：**一个默认关的模块，第一次被编到的地方不能是
  使用者手上**。四格的形状：

  | 格 | 平台 | `UVCPP_ENABLE_ZLIB` | `UVCPP_ENABLE_BROTLI` | brotli 从哪来 |
  |---|---|---|---|---|
  | `brotli` | ubuntu | ON | ON | 系统的（`libbrotli-dev`，pkg-config） |
  | `brotli` | macOS | ON | ON | FetchContent（不装包） |
  | `brotli` | MSVC | ON | ON | FetchContent（不装包） |
  | `brotli-no-zlib` | ubuntu | **OFF** | ON | 系统的（`libbrotli-dev`） |

  第四格不是凑数：库与用例里**有一整条分支只在"有 brotli、没有 zlib"时才编译**
  （协商里 gzip/deflate 问不出来、`*` 兜到 br；静态腿那条把 `test_compress_variant_brotli`
  的对比项换成"编不出客户端要的编码时必须原样发原文"）。没有这一格，那些断言
  在 CI 里**一条都不会被编译**，而"没编译"与"编了且过"在 check 的颜色上长得
  一模一样。`zlib-off` 那一格覆盖的是反方向（zlib 关、brotli 也关），两格合起来
  才是四象限。

  **门禁串**（写成四个 `gate_*` 字段，配置与测试两步各用一半）：`gate_switch` =
  `UVCPP_ENABLE_BROTLI:BOOL=ON`（缓存里那个开关真的 ON）；`gate_integrated` =
  `brotli enabled`（`CMakeLists.txt` 里那句**与发现路径无关**的消息 —— 系统与
  FetchContent 两条支路的其它消息是**不同**的，拿它们当门禁串会在三格里红两格，
  这一条写在 ci-guide.md §5）；`gate_module` =
  `HTTP body compressor: UVCPP_HTTP_COMPRESS_ENABLE=(1||1)`（派生的那个宏拼对了，
  它才是"两个编码器都在库里"的见证；`web` 那一格是 `(1||0)`，所以这个串区分得开）；
  `gate_test` = `test_web_http_client_func`（**必须是 ctest 的名字** —— 协商与
  q5/q0 那几条断言是那个二进制里的子用例，`ctest -N` 里看不到 `test_compress_brotli`
  这个名字）。

  装包表只在 Ubuntu 那两格多一个 `libbrotli-dev`（就是 `pkg-config` 找的
  `libbrotlienc` / `libbrotlidec` 那一对）；macOS 与 MSVC 那一格**一个新包都没有**
  （brotli 走 FetchContent，`GIT_TAG` 钉在 `BROTLI_VERSION`），装包表与 `web`
  逐字相同 —— 这样两边红绿不同只可能归因到那个开关。**而 macOS / MSVC 那两格正是
  "brotli 取自源码"这条支路在 CI 里的唯一一次运行**，它此前在 generate 阶段就失败
  （见上面「性能」那一节的第二条），本机是把系统 brotli 藏起来才量到的。

  平台覆盖的口径：MinGW **没有**这一格（单 job，且它与 Ubuntu 同为 GCC +
  libstdc++），记进 ci-guide.md §1 的"未覆盖的格"表。

- **`yyjson` 有了一条自己的 CI 腿：三格（Ubuntu / macOS / MSVC）**（`1.6.35-dev`）。
  `src/yyjson/` 上个提交刚进来时，CI 里**没有任何一条腿**开 `UVCPP_ENABLE_YYJSON` ——
  于是那个模块里最重的两条判据（「25 行输入表逐条先与手写期望值比、再与 nlohmann
  逐条比」与「五份键升序语料上 `uvcpp_json_write` 与 nlohmann `dump()` 逐字节相同」）
  **一条都不会被编译**，而"没编译"与"编了且过"在 check 的颜色上长得一模一样。
  这是本仓那条老规矩的又一例（`UVCPP_ENABLE_ZLIB=OFF` + `WEB/WEBAPP=ON` 整整一版没有
  一格覆盖，#35）：**一个默认关的模块，第一次被编到的地方不能是使用者手上**。

  这一格比 `zlib-off` / `wsdl` 那两格更硬一点：本模块当时**不在发布包里**
  （`release.yml` 不传这个 `-D`，包里那个头是空的壳 —— `package_release.py` 是
  **按模块目录整体拷头**的、与开关无关，所以拦住你的是宏，不是"包里没有这个文件"），
  所以这三格是它**唯一**会被编译的地方，一次都没有别的路径兜底。
  （**1.7.0 起发布腿也开它**，于是这三格与 `wsdl` 那三格变成同一句话 —— 发布配置在
  push/PR 阶段的替身，而"唯一"不再成立。`check_doc_snippets.py` 的 `DEFAULT_OFF`
  里 `UVCPP_YYJSON_ENABLE` 那条豁免同批删掉，三个 config-contract 档跟着开它。）
  `flags` = `web` 加一个 `-DUVCPP_ENABLE_YYJSON=ON`，四道门禁串
  `UVCPP_ENABLE_YYJSON:BOOL=ON` / `yyjson integrated` / `Including yyjson module in
  build` / `test_yyjson_doc_func`；装包表与 `web` **逐字相同**（yyjson 走 FetchContent
  —— `CMakeLists.txt:1796`，`GIT_TAG` 钉在 `YYJSON_VERSION` —— **不发包管理器**），
  这样两边红绿不同只可能归因到那个开关。

  **为什么 macOS 有一格、而 `wsdl` 没有**：`ci-guide.md` §1 给缺 macOS `wsdl` 的理由是
  "macOS 不是发布平台，没有一条路径让它未编就到达用户" —— 这条理由在这里**不成立**
  （本模块根本不发版），而 macOS 是唯一用 **libc++** 编这个头的腿，写回逐字节那条
  正好是 libstdc++ / libc++ 会分叉的那类断言。MinGW 仍是缺口（它与 Ubuntu 同为
  GCC + libstdc++），写进 §1 的"未覆盖的格"表。

  **本机先按这一格的原命令行复现过判据**（CI 之外的那一半）：
  `cmake -DUVCPP_BUILD_TESTS=ON -DUVCPP_BUILD_SHARED=ON -DUVCPP_BUILD_WEB=ON
  -DUVCPP_BUILD_WEBAPP=ON -DUVCPP_ENABLE_ZLIB=ON -DUVCPP_ENABLE_YYJSON=ON` →
  配置 rc=0，`UVCPP_ENABLE_YYJSON:BOOL=ON` / `yyjson integrated` /
  `Including yyjson module in build` 三个串**都在**，`ctest -N` 里有
  `test_yyjson_doc_func`；而在 `web` 那一格（同一份命令行去掉最后一个 `-D`）上，
  这三个串**一个都不出现**，只有 `Excluding yyjson module from build` ——
  所以这几道门禁不是恒真的。全量与用例另一侧的读数见上面「JSON」那一节。

- **`uvcpp_loop_index_of_this_thread()` / `uvcpp_set_loop_index_of_this_thread()`
  补上 `UVCPP_API`**（`1.6.20-dev`）。这两个自由函数在安装集里的公开头
  `src/net/uvcpp_loop_worker.h`（`include/net/`）上声明，却一个导出宏都没有 ——
  库在 Windows 上是 DLL，于是**只有本库内部的 TU 链得上它**，外面任何一份调它的
  TU 都是 `LNK2019`。这不是假设：本仓自己的
  `tests/functional/http3_multiloop_func.cpp` 就长在 MSVC 那条 `http3` 腿上
  （`error LNK2019: unresolved external symbol "int __cdecl
  uvcpp::uvcpp_loop_index_of_this_thread(void)" ... referenced in function
  <lambda_...>::operator()`），而 Linux / MinGW / macOS 全绿 ——
  ELF 与 MinGW 默认导出全部符号，这个缺陷**只在 MSVC 上现形**。**判据**：MSVC 那条
  `http3` 腿的 `Configure & Build` 从红转绿（此前它连编都编不过，
  `test_http3_multiloop_func` 因此一次都没在 Windows 上跑过）；Linux / MinGW / macOS
  三档逐字节不变（`UVCPP_API` 在非 Windows 上展开为空）。

- 使能宏随包发出；宏集与 DLL 不一致从静默垃圾值改为编译期硬失败（`1.1.27`）
- 22 个公开头带上 UTF-8 BOM，消费者不传 `/utf-8` 时不再级联报错（`1.1.30`）
- Linux 包里的 `libuvcpp.so` 从 `bin/` 挪到 `lib/`，也就是文档里指的那个位置（`1.1.28`）
- 六个打包 job 补上 `UVCPP_ENABLE_NGHTTP2`，h2 不再从包里缺席（`1.1.6`）
- 每份包里多了一档**调试版**动态库（`uvcppd.dll` / `libuvcppd.so`，`-luvcppd`，
  MSVC 那份还带 `uvcppd.pdb`），方便单步进库内部（`1.1.35`）
- 新增十二篇指南：使用者向八篇（`lowlevel-guide.md`、`net-guide.md`、`ssl-guide.md`、
  `web-http-guide.md`、`web-ws-guide.md`、`http2-guide.md`、`expand-guide.md`、
  `webapp-support-guide.md`）与贡献者向四篇（`CONTRIBUTING.md`、`build-guide.md`、
  `testing-guide.md`、`release-process.md`）（`1.2.1`）
- 三道文档门禁进 CI：构建系统的选项与 README 选项表双向闭合、链接/路径/锚点可解析、
  每篇指南都被正文提到（`check_docs.py`）；22 篇文档里的 `文件:行号` 引用能解析且与
  内容哈希锁一致（`check_doc_lines.py`）；每个 `cpp` 片段都对着一个已 stage 的包、
  不加任何 `-D` 真编译（`check_doc_snippets.py`）（`1.2.1`）
- 两版 README 里 4 处**编不过**的示例已修（`1.2.1`）
- 给消费者的 ABI 提示：`uvcpp_buf`（`1.1.28`）与 `uvcpp_http_server`（`1.1.34`）的布局
  变过，`1.2.x` 这条线改得更多 —— `uvcpp_h2_stream`（`1.2.25`）、`uvcpp_ssl_context`
  （`1.2.24`）、`uvcpp_memory_pool`（`1.2.x`）与五个多循环类（`1.2.21`–`1.2.23`）布局
  都变了；`uvcpp_handle` 的拷贝构造、拷贝赋值与 `clone()`，以及 `uvcpp_req::clone()`
  则被**删除**（`1.2.5`）—— **必须重编，别只换二进制**（见 [RELEASE.md](./RELEASE.md)）

- 修掉 Windows 上编不过的一行：`owned_write_state` 里 `uv_buf_t head = {nullptr, 0}` 用的是
  **Unix** 的成员次序，而 Windows 的 `uv_buf_t` 是 `{ULONG len; char* base;}`（`uv/win.h`）、
  Unix 的是 `{char* base; size_t len;}`（`uv/unix.h`）—— **两个成员次序相反** ⇒ MSVC 报
  C2440（`nullptr` 喂给 `ULONG`）、MinGW 同错，七个 Windows job 自写路径那笔（`1.3.22`）起
  全红。改用 `uv_buf_init(nullptr, 0)`，与成员次序无关（`1.3.27`）
- CI 改成**一个平台一个 workflow 文件** —— `ci-linux-ubuntu.yml`、`ci-windows-msvc.yml`、
  `ci-mingw64.yml`、`ci-macos.yml`，各自带触发条件，文件内按**功能分格**（基础版 / web /
  ssl / h2 / quic / full），骨架每平台只写一次；替掉原来那个横跨四个平台、十个 job 的
  1369 行 `ci.yml`。README 顶上从一枚汇总徽章换成四枚按平台的，红的那条腿自己说得出是哪个
  平台红了。QUIC 补上 **macOS 与 Windows MSVC** 两格（只有 ubuntu 那格自建 OpenSSL 3.5，
  另两格用包管理器给的；macOS 那边因为 `openssl@3` 是 keg-only，必须显式钉
  `-DOPENSSL_ROOT_DIR`）。新门禁 `check_ci_layout.py` 让四个文件与 `doc/ci-guide.md` 的
  布局表**双向**相等：悄悄删掉一格、改掉一格的名字（挂在 `if: matrix.feature == …` 上的
  步骤会跟着无声消失）、或者徽章指向一个已删文件，都会红而不是没人发现（`1.4.1`）
- HTTP/3 补上**同样三格** —— Linux、macOS 与 Windows MSVC（MinGW 那条腿没有：它既没有
  h2 也没有 quic 格）。Linux 与 macOS 那两格第一次跑就绿了；Windows 那格在注释里写明了
  本机验不了（写它的机器上没有 MSVC），所以它的第一次绿只能是一次 CI 运行 —— 而到
  `1.4.1` 为止它还没绿过，因为它的 `Install deps` 那一步在 `Configure & Build` 之前
  就失败了（见下一条）。Linux 那条腿的整个特性在推送前已经在本机端到端跑通（`1.4.1`）
- Windows 的依赖安装收进 `.github/scripts/win-openssl-deps.sh`，由 MSVC 矩阵四格、两处
  `config-contract` 的 Windows 那一半、以及 `release.yml` 的 `msvc-x64` 共用。从
  2026-09-29 20:46 UTC 起，`choco install openssl --no-progress` 在 `windows-latest` 与
  `windows-2022` 上都**每一次**都非 0 退出（148），而 workflow 文件与最后一次全绿的那次
  逐字节相同（`git diff 0addc23f 699d27e -- .github/workflows/ci-windows-msvc.yml` 是空的）
  —— 变的是 runner，不是本工程。这一步真正的要求从来不是"choco 退出 0"，而是"这台机器上
  有一份 `find_package(OpenSSL)` 找得到的 OpenSSL"，所以判据改落在文件系统上：先看那份
  DLL 拷贝步骤用的同样四个目录，再回落到 `choco install openssl --no-progress --yes`
  并退避重试三次，然后重新看一遍 —— 还是没有就 `::error`，把找过的目录与 choco 的原话
  一起写进注解。`check_ci_layout.py` 把这件事**两向**钉住：调用点必须在
  `ci-windows-msvc.yml` 里，而裸 `choco install openssl --no-progress` 一行必须在四个
  平台文件里一个都不出现（`1.4.1`）

- **1.5.0 起六条发布腿的产物都是全功能的**（QUIC + HTTP3 + C API 一起开）。QUIC 的前提是
  一份带 QUIC API 的 OpenSSL ≥ 3.2，而六条腿的来路各不相同，逐条记在 `release.yml`
  文件头：Linux 两条腿**不装** 22.04 的 `libssl-dev`（那份是 3.0.2，没有 QUIC API ——
  装上只是给 `find_package` 添一份不合用的候选），改成在 job 里自建 OpenSSL 3.5.0
  （`no-shared no-tests -fPIC`；`-fPIC` **不是保险是承重的**，本机量过同一份源码两种
  命令行生成的 `CFLAGS`），编完当场用 `nm --defined-only libssl.a` 查
  `SSL_set_quic_tls_cbs` 有定义；MinGW 两条用 MSYS2 包的（3.6.x）；MSVC-x64 用 runner
  脚本给的那份；MSVC-arm64 自己拿 `VC-WIN64-ARM no-asm` 编 3.5.8（`1.5.0`）
- **六条腿的模块断言从读 `CMakeCache.txt` 改成读生成的头**：QUIC/HTTP3 的守卫链是
  `message(WARNING)` + 普通变量 `set(... OFF)`，于是 cache 里**照旧**写着 `=ON` 而编译器
  看到的是 0 —— 只 grep cache 的断言会**放行一个缺功能的包**，而"发出去的包缺功能"正是
  它要拦的那件事。判据落在 `<tree>/include/uvcpp/uvcpp_config.h`：那是编译器真正读到的
  那个数，也是 `config-contract` 门禁读的那个数。名字映射逐条对着 `CMakeLists.txt` 的
  `_uvcpp_literal01(...)` 抄（第一版按名字猜，把 `UVCPP_BUILD_EXPAND` 写成了
  `UVCPP_WSDL_ENABLE`，拿真生成头实测时当场红了一条）；mingw 与 linux 四条腿**两份树都
  量**（发布档与调试档是两次独立 configure，漏传只让其中一份缺功能）（`1.5.0`）
- **1.5.2 起发布包连 WSDL/SOAP 一起带上**，六条腿的断言清单于是从十个宏变成十一个。
  在那之前
  `UVCPP_ENABLE_WSDL` 是六条腿唯一漏掉的模块，而它的失败方式很安静：包里**照样装着**
  `include/wsdl/*.h`（那份清单由 `package_release.py` 的 `MODULES` 决定，与开关无关），
  但生成头里 `UVCPP_WSDL_ENABLE` 是 0，于是那些头的全部内容落在 `#if` 外面 —— **头在、
  功能不在**。现在十个 configure（六条腿，其中四条各多一棵调试树）全传
  `-DUVCPP_ENABLE_WSDL=ON`，六张断言清单全加 `UVCPP_WSDL_ENABLE`。pugixml 走
  `FetchContent`、被压成静态、以 `PRIVATE` 链接，所以它与其余私有依赖一样进 DLL：
  不多一个运行时 dll、`.pc` 不用改、使用方什么都不用装。Ubuntu 与 Windows MSVC 各多一格
  `wsdl` 矩阵，让这份配置在**每次 push** 上被编译**并运行** —— 这一点是承重的：此前那四条
  `wsdl`/`soap` 用例在整个 CI 里**一次都没被 ctest 跑过**，唯二打开该模块的地方都是
  configure-only（`1.5.2`）
- **1.5.3 起发布包再带上数据库模块，而包里的 db 只带 SQLite 一个后端**（六条腿的断言
  清单因此是**十三个宏**）。每条腿各传四个 `-D`：`-DUVCPP_ENABLE_DB=ON`
  `-DUVCPP_DB_SQLITE_FROM_SOURCE=ON -DUVCPP_ENABLE_DB_MYSQL=OFF
  -DUVCPP_ENABLE_DB_PGSQL=OFF`。两个 `OFF` 是**显式**关的，不是"找不到"：发布 runner 上
  装着 `libpq-dev`，`find_package` 会**静默**成功，于是 `libuvcpp.so` 悄悄多一条
  `libpq.so.5` 的 `DT_NEEDED` —— 而"装到别的机器上跑不起来"正是发布腿那条 `ldd` 断言
  要拦的形状。**`PRIVATE` 链接挡不住这件事**：`PRIVATE` 关的是头文件与编译定义，
  共享库上它照样写 `DT_NEEDED`。SQLite 那份走的是源码 amalgamation（哈希钉死、编成
  静态且强制 PIC），不是系统的 `libsqlite3`：链 `.so` 会多一条 `DT_NEEDED`，而 Ubuntu
  24.04 上系统的 `libsqlite3.a` 不是 PIC（实测 `R_X86_64_PC32 against symbol
  'sqlite3CtypeMap' can not be used when making a shared object`）。改这条断言之前
  先看一件事：它读的是**生成的头**而不是 `CMakeCache.txt`，因为"三个后端一个都没成"时
  模块是被普通 `set()` 强制关闭的 —— 缓存里照旧写着 `ON` 而编译器看到 0，
  只 grep 缓存的断言会放行一个 `UVCPP_DB_ENABLE 0` 的包。CI 侧四个平台各有一格：
  Ubuntu / macOS / MSVC 三格开 SQLite，MinGW 那条腿两次 configure 都开；**两个远程
  后端另有一个 job**（Ubuntu 的 `db-servers`，起真 MySQL 8.0 与 PostgreSQL 16 容器，
  `UVCPP_DB_TEST_REQUIRE=1` 把"没连上"从"未判定"变成失败），每一边都带反向断言 ——
  `db` 格不许出现远程后端的开日志，`db-servers` 不许出现 `db: SQLite 后端开`、也不许
  注册 `test_db_sqlite_func`（`1.5.3`）
- **六条腿的 timeout 120→150 分钟**（全开 QUIC/HTTP3 之后，每条腿除了自己的源文件还要
  FetchContent 编 ngtcp2 与 nghttp3，Linux 那两条还要多编一份 OpenSSL；msvc-arm64 是
  180）。Linux 的自包含断言名单加上 ngtcp2 / nghttp3：它们今天由本仓
  `ENABLE_SHARED_LIB=OFF` 压成静态、**不可能**出现在 `ldd` 里，写进名单是为了让"哪天有人
  把上游那个开关改成 ON"变成一条当场看得见的红（`1.5.0`）
- **`cmake --install` 现在也装从源码取的 zlib 那两个头**（`zlib.h`、`zconf.h`）。公开且会被
  安装的 `src/web/uvcpp_ws_parser.h` 在 `#if UVCPP_ZLIB_ENABLE` 里引 `<zlib.h>`，而生成的
  `uvcpp_config.h` 把这个宏**默认定成 1**（还带一条 `#error` 禁止消费者自己定义成 0）——
  关不掉。库自己编得过，是因为那份 zlib 是 FetchContent 取来**静态**链进去的，头只存在于
  构建树；于是"用发布包"与"从源码建库再 `cmake --install`"这两条路分了叉，而后者一编就死在
  `zlib.h` 找不到上（发布包是 `package_release.py` 手工补的这两个头，install 规则里没有）。
  开关用普通变量 `UVCPP_ZLIB_FROM_SOURCE`，**不是** `DEFINED zlib_SOURCE_DIR` —— 后者是
  FetchContent 写进缓存的 `:STATIC`，走过一次取源支路就一直在，翻回系统 zlib 支路照样
  "defined"，会装出一份陈旧的 `zlib.h`（这条假阳性实测过）。系统 zlib 的平台上不触发。
  `cmake --install` 在本仓 CI 里**没有任何一格覆盖**（唯一的 `make install*` 是 OpenSSL 的
  `install_sw`），所以这条分叉此前没人看得见（`1.6.3`）

## 逐个版本

这一节按发版时间倒序，只列**已发布**的 tag。开发档线已折叠进对应的稳定版
（折叠表见开头），所以这里读到的每一版都是一个真实的 tag。

### v1.7.0 (2026-10-14)

`v1.6.4` 之后的第一个 minor —— 开发档 `1.6.5`–`1.6.61`（57 档）折进来。两条主线：
**h3 与 gRPC 补齐、明文 h2 与 brotli 进来**，以及**一轮从头到尾的"每请求代价"削减**
（拷字节、取锁、系统调用、分配）；发布包在这一版第一次把库里**全部**可选功能都装上。

**新增**：`src/grpc/` 成为一个模块（unary 与流式，`grpc_send_unary()` 一个函数包办一
条应答的三段顺序），h2 的**尾随头**通了 —— 那是 gRPC 那条路上最后一个库级缺件；
**明文 h2（先验知识 h2c）**与**第三个内容编码器 brotli**（`UVCPP_ENABLE_BROTLI`）
各自带开关进来，默认都是关的；QUIC 端点跟着 `set_loops(n)` 扇到工作循环上，
`set_scid_len()` 让 n 个端点抢同一个端口；`uvcpp_http_reply` 把「稍后应答」做成
与传输无关的凭据（h1/h2/h3 都有）；静态器能直接发盘上的**预压缩孪生文件**
（`precompressed`）。

**修复**：h3 短包头按**对端**的 SCID 长度解自己的 DCID（`1.6.5`，见「HTTP/3」一节：
两端同源的用例恒等抵消，换一个对端才现形）；流水线里末条的 `Connection: close`
会把前面几条的响应一起带走（`1.6.46`）；明文 h2c 的 `:scheme` 不再钉死 `https`
（`1.6.12`）—— 同一个形状，1.6.7 那版 h2c 在本仓用例里全绿而整条腿是坏的；
`~uvcpp_http_server()` 与工作循环抢同一张表（`1.6.4`，见下一条）；`decompress()`
里两个 `inflate` 循环用 `Z_FINISH` 收尾（`1.6.32`）；413 那个响应体此前每次多读 2 字节
（`1.6.19`）；h2 的 `content-length` 会漏出去（`1.6.11`）。

**性能**：这一段的量具与逐条读数都在 [doc/benchmark.md](doc/benchmark.md)，从 §4.5
起是**平台自己那一套装置**的读数。挑几条确定性最强的：静态热路径上的取锁次数
每请求 9 次 → 1 次（`1.6.36`/`1.6.37`）、`realpath` 每请求 2.734 次 → 0.001 次
（`1.6.38`）、命中路径每请求分配 98.6 次 → per-loop 回收站（`1.6.39`/`1.6.40`）、
JSON 写出器把"一个键"从 4~5 次追加压成 1 次（`1.6.22`，每响应 4073 → 1536 次）、
ws 发送队列整批合并（`1.6.21`）、h2 每批最多吐 64 KiB（`1.6.41`，`static-h2` 的
常驻内存从 3.4 GiB 落到 793 MiB）。h1/h2 那一组高吞吐档的提升（`baseline-4096`
+44%、`pipelined-4096` +88%）**归在 1.6.23–1.6.51 这一段，但没逐提交归因** ——
报告里就是这么写的，不编一个理由给它。

**打包**：`release.yml` 的十条 configure 此前**一条都不传** `UVCPP_ENABLE_YYJSON`
与 `UVCPP_ENABLE_BROTLI`，db 的两个远程后端还是显式 `=OFF` —— 于是"预编译包是全功能的"
这句话有一处三样东西的差集。1.7.0 起差集为空：yyjson 与 brotli 开了，MySQL/PG 改成
**编进去、运行时 `dlopen`**（`UVCPP_DB_RUNTIME_LOAD`），模块断言从 13 枚扩到 17 枚，
ELF 侧加 `--exclude-libs,ALL` 把嵌入依赖的符号收回去。**这是一处行为变更**：此前
"配置期没找到客户端库 ⇒ 那个后端不存在"，此后是"后端存在、有没有库运行时才知道"。
逐条理由见 [RELEASE.md](RELEASE.md) 的 `## v1.7.0 重点`。

### v1.6.4 (2026-10-12)

**修复**:多循环停机 —— `~uvcpp_http_server()` 在停工作循环之前就清了按循环切的表

`set_loops(n > 1)` 下，`~uvcpp_http_server()` 与工作循环线程会同时写同一张 `contexts_`
表。析构体的注释写着「析构发生在**所有**循环线程都已经停了之后（调用方的责任）」，
但那个析构体**自己没先停**：唯一会停 worker 的 `stop_workers()` 是 `uvcpp_tcp_server`
的私有方法，而调它的地方是析构体的**最后一行** `delete tcp_server_` —— 那时代码已经把
`contexts_` 逐张 `clear()` 过一遍了。

worker 的退出路径会关掉自己名下的连接（`close_clients_of_loop`），那条路上经由关闭回调
跑 `remove_ctx` ⇒ `tbl.erase(it)`，而这个 `it` 指向的结点刚被 `clear()` 释放 ⇒ 崩在
`std::_Rb_tree_rebalance_for_erase` 里。core 里两条栈正好对上：一条在
`~uvcpp_loop_worker` 的 `std::thread::join()` 上，另一条在 `remove_ctx` 里 erase 同一张表。

那句「调用方的责任」**今天没人能履行**：`stop_workers()` 私有、`stop()` 只关监听句柄、
不动 worker，`close_all_clients()` 是异步投递。所以裸用低层 `uvcpp_http_server` +
`set_loops(n > 1)` 的调用方必然踩在这条窗口上 —— 这也是它稳定复现（约 1%）而不是
「偶发」的原因。判据与量法：`tests/functional/test_web_multiloop_func` 用
`taskset -c 0-3` 钉在 4 个核上（与 CI runner 同形）、8 路进程并发各跑 250 次，
**改前 2000 次崩 18 次**（rc=139），改后 **0 次**；另一次 8 路 × 500 = 4000 次也是 0 崩。
单进程 `--repeat until-fail:30` 压不出来 —— 那正是它此前被当成 flake 的原因。

改法两处：`stop_workers()` 保持私有、给 `uvcpp_http_server` 加一条 friend（与
`uvcpp_web_stream` 那条「公开它等于允许任意调用方在循环还转着的时候把工作线程拆掉」的
既有约定同形），析构体第一句 `tcp_server_->stop_workers()`。单循环下 `workers_` 是空的
⇒ 空操作，单循环行为逐字节不变。回归：`build-capi` 109/109、`build-capi-all`（全功能）
124/124 全过（两条 DB 用例按惯例 skip）。

顺带说清一条旧账：这个用例在 Ubuntu 的 `ssl`（2026-10-04）与 `capi`（2026-10-12）两格上
都崩过，当时按「空提交重跑」处置、并在仓库里记成间歇性 —— 现在知道它不是 flake。

### v1.6.3 (2026-10-12)

**修复**:`cmake --install` 现在也装从源码取的 zlib 那两个头

从源码建库、再对着 `cmake --install` 出来的前缀编译的消费者，此前一编就死。公开且会被安装的
`src/web/uvcpp_ws_parser.h` 在 `#if UVCPP_ZLIB_ENABLE` 里引 `<zlib.h>`，生成的
`uvcpp_config.h` 又把这个宏**默认定成 1**（附一条 `#error` 禁止消费者自己定义成 0），而 zlib
的那两个头只存在于构建树 —— 它是 FetchContent 取的源码、被**静态**链进 `libuvcpp.so`，
装的时候没人管它。

发布包没这个毛病：`package_release.py` 给包手工补了 `include/zlib.h` / `include/zconf.h`，
注释里直说「install 规则里没有这一条」。所以是同一件事的两条路分了叉，而 `cmake --install`
在本仓 CI 里**没有任何一格覆盖**（唯一的 `make install*` 是 OpenSSL 的 `install_sw`），
分叉因此一直没人看见。它是从 [HttpArena](https://github.com/MDA2AV/HttpArena) 上那份
**从源码建库**的条目里冒出来的：那个入口钉 `v1.6.1`、在容器里自己编库，最后一条
`RUN g++ … main.cc` 就死在这一处，报的是
`include/web/uvcpp_ws_parser.h:31:10: fatal error: zlib.h: No such file or directory`。

install 规则补上这两个头，只在**取源码**那条支路装（系统 zlib 的平台上消费者手里本来就有）。
开关刻意不用 `DEFINED zlib_SOURCE_DIR`：那是 FetchContent 写进**缓存**的
`zlib_SOURCE_DIR:STATIC`，走过一次取源支路就一直在，后来翻回系统 zlib 支路照样 "defined"，
会装出一份陈旧的 `zlib.h`（`zlib.h` 在源码目录、`zconf.h` 由 zlib 自己的 CMake 在配置期
生成到构建目录，两个都得取）。改用普通变量后三条支路实测：取源支路 `cmake_install.cmake`
里 1 条、**同一个构建目录**翻回系统 zlib 支路 0 条、再翻回来又是 1 条。

判据不是平台读数：在一个**没有系统 zlib** 的头树里（`-nostdinc -nostdinc++` 加符号链接农场
顶替 `/usr/include`），只用这个安装前缀编公开头 —— 修完 rc=0 且 `-M` 指明 `zlib.h` 来自前缀；
把这两个头从前缀里抽掉就复现出上面那条 CI 报错，逐字同一处。没有性能面。

### v1.6.2 (2026-10-11)

**修复**:HTTP/1.1 的 `Host` 与空 `Transfer-Encoding` 一律 `400`

一条改动，判据来自 RFC 9112 §3.2：缺 `Host` 字段行、`Host` 多于一行、`Host` 字段值非法，
三种形态都得真回一个 `400`，静默关连接不算。它是该规范里**本库此前唯一不拦的一条
MUST-400** —— §5.1 的"冒号前有空白"、§6.1 的"TE 不以 `chunked` 收尾"、§6.3 的"多个不等的
CL"同样写着 MUST-400，那些是 llhttp 拦的，本库一直是 400。
外加 §6.1 那条空 `Transfer-Encoding`（[HTTP 与 WebSocket 语义](#http-与-websocket-语义)）：
`transfer-coding = token`、`token = 1*tchar`，整行为空一个 tchar 都没有，而 llhttp 在值整行
为空时**不把 TE 记成"存在"** —— 于是 `TE:（空）+ Content-Length` 被当成"没有 TE"、退回
CL 框架照常应答，前端若按 §6.3 压过 CL 处置就是走私。

判据点排在 `set_stream_claim` 钩子**之前**：请求被那个钩子认领后 `on_headers_complete`
就直接返回了，判据放它后面等于只有低层路由受检，而 webapp 才是绝大多数用法。为此两条
用例分工 —— 一条不注册路由走低层（合法请求落 404 当对照，9 条必须 400 + 9 条不许误伤），
一条走 webapp 走认领路径。次序做**变异验证**：给判据加上"只在没有认领钩子时生效"，
低层那条照旧绿、webapp 那条 5 条必须 400 的全部变红（实际 200），随后还原。

**有意保留的宽松**（不合规才修，宽松不动）：obs-fold 仍走 §5.2 的第二个分支（每个折行
换成一个 SP）而不是回 400，那条规范本身给了两个分支；`Host` 值里的 `%` 不核那两位十六进制。

### v1.6.1 (2026-10-11)

**修复**:h2 的控制帧预算把 `WINDOW_UPDATE` 也算进去（一条连接被自己掐在 ~1 MiB/s）、
h1 的在途上限 8 被套在 h2 流上（合规的流被打成 503）、QUIC 流关闭不还额度（一条连接
终生只服务 100 个请求）；**性能**:响应压缩改为每线程一份 deflate 内核（+2.4%）

这四条是同一个形状：**一道被当成"保护"的闸门，量错了对象**。四条都是在
[HttpArena](https://github.com/MDA2AV/HttpArena) 基准上暴露或证实的，逐条判定见上面
各自的主题节，这里只记读数：

- **`WINDOW_UPDATE` 移出控制帧预算**（[HTTP/2](#http2)）。那张单子上的帧满足"回一帧 =
  分配一帧"，而它的正常速率与**我们已经发出去的字节数成正比**，于是任何"帧/秒"的上限
  都成了吞吐上限。判据：默认 64 KiB 窗口的客户端，一条连接稳定在 7 个 200 KiB 响应后
  开始 `RST_STREAM`（≈1.4 MiB，且与并发流数无关 —— 这正是"字节预算穿了帧率外衣"的
  指纹）；移出后 64/64 全通。
- **h2 不再吃 h1 的在途上限**（[横向扩展](#横向扩展多事件循环)）。平台的 `static-h2`
  读数是 2xx 238,313 / **5xx 556,357**（七成请求被扔，`reconnects` 为 0 —— 连接一直
  活着，是流被拒）；本机同形复现 578,096 / 742,441，修后 163,000 里只剩 49–289。
- **QUIC 流关闭把额度还给对端**（[QUIC 传输](#quic-传输net-层)）。修复前 64 连接 × 64 流
  恰好等于 64 × 100，且服务端只烧 0.02 核 —— 不是算不动，是被自己的额度卡住。
- **压缩内核每线程一份**（[性能](#性能)）。产物逐字节相同（三档请求的 gzip 体 md5 两边
  一致），`json-comp` +2.4%（逐轮交替各 5 轮，两边不重叠）。

**两条要写下来免得后人误判收益的**：上面第一和第四条，在本机对 h2load 形状的基准上
**不产生差异** —— h2load 把窗口抬到 2^30-1 且从不发 `WINDOW_UPDATE`，那个桶够不着；
压缩内核那条是因为 glibc 的 mmap 阈值自适应，只在真实节奏里才露出来（2.4%，不是
"每次省 55 µs"那种读数）。它们在本仓的判据是各自的主题节，不是平台读数。

### v1.6.0 (2026-10-11)

**新增**:数据库模块（SQLite / MySQL / PostgreSQL 一套契约）、C ABI 第八片 db（合计 402 / 八片）、发布包再扩容

- **数据库模块落地**（`UVCPP_ENABLE_DB`，**默认 OFF**，本库唯一需要第三方客户端库的模块）。
  一个连接一个 `uvcpp_db_client`，同一套接口盖住三个后端，结果行按表取（列名或列号）。
  占位符**按方言原样透传**、事务里**不重试**、异步门面不写 `uv_queue_work`、连接池把
  并发从 1 抬到 N。逐条与实测见本文件上面的「[数据库](#数据库srcdb)」主题节与
  [`doc/db-guide.md`](doc/db-guide.md)。
- **C ABI 第八片 db**：`uvcpp_c_db.h` **81 个入口**，到此 **402 个函数 / 八片**。
  `UVCPP_C_ABI_VERSION` **仍是 `1`** —— `git diff --stat` 在 `src/capi/` 上只有伞头多了
  一段 `#if UVCPP_DB_ENABLE` 的 include，既有七份头与七份 `.cpp` 一个字节没动。C# 绑定
  仍覆盖前七片（321 of 402），db 那 81 条是它自己 README 里如实列出的缺口。
- **发布包两轮扩容**：`1.5.2` 加 WSDL / SOAP（模块断言十 → 十一），`1.5.3` 加 db 的
  SQLite 一个后端（十一 → 十三）。两轮的判据都**读生成头**
  `<tree>/include/uvcpp/uvcpp_config.h` 而不是 `CMakeCache.txt` —— 强制关闭走的是普通
  `set()`，缓存里照旧写 `ON` 而编译器看到 0，只 grep 缓存的断言会放行一个缺功能的包。
- **QUIC 的四处改动**：分块发送队列（大载荷下交付出去的字节是错的 —— 2 MiB 回显 20 次
  崩 3 次；换回旧存储跑同一量具，6 次运行全在 15 轮内报 `DATA MISMATCH`）、数据报上限
  1200 → 1500（原先把 ngtcp2 自带的 PMTUD 整条关死，2 MiB 单向要 1748 个数据报）、
  ACK 阈值 2 → 16（回环上总数据报 −27%、中位耗时 19.25 → 13.81 ms）、Windows 发送侧
  批量交数据报（`UVCPP_ENABLE_UDP_GSO`）。
- 预编译动态库：6 个平台（Windows / Linux × x64 / arm64 × MinGW-w64 / MSVC / GCC），
  依赖全静态链接；`1.6.0` 起包里多带 WSDL 与 db（db 只带 SQLite 一个后端）。

**破坏性**:无 C++ API 变更；C ABI 版本仍是 `1`，本版只新增、没有删除或改名任何既有符号。
但发布包的**模块集合又变了**：`UVCPP_WSDL_ENABLE` 与 `UVCPP_DB_ENABLE` 由 `0` 变 `1`，
换包的人会多出这两批模块与 db 那 81 个符号。**从源码构建的人不受影响** —— 两个开关的
默认值仍是 `OFF`。逐条见 [`RELEASE.md`](RELEASE.md) 的「v1.6.0 重点」那一节。

### v1.5.0 (2026-10-05)

**新增**:发布包全功能（QUIC + HTTP/3 + C ABI）、C# 绑定与 QUIC 例子

- **六条发布腿的预编译包（发布档 + 调试档）都带上 QUIC + HTTP/3 + C ABI**。判据读
  `<tree>/include/uvcpp/uvcpp_config.h` 而不是 `CMakeCache.txt` —— 静默降级（WARNING +
  普通变量 `set OFF`）在缓存里看不出来，只 grep 缓存的断言会放行一个缺功能的包。
  QUIC 需要带 QUIC API 的 OpenSSL ≥ 3.2：Linux 两条腿自建 3.5.0、MinGW 两条用 MSYS2 的、
  MSVC-x64 用 runner 脚本给的那份、MSVC-arm64 自建 3.5.8
- **C# 绑定**：`bindings/csharp/` 两份 `.cs`（321 条 `DllImport`，与符号面锁双向对账）
  加一个能跑的 QUIC 回显例子；编译门槛是量出来的（`net8.0` 真跑过、`netstandard2.1` +
  C# 10 编得过、`netstandard2.0` 与 C# 9 编不过 —— 所以"Unity / Mono 也编得过"是错的）
- **修复**：QUIC 端点带着活连接 `_server_free()` 会 SIGSEGV（边遍历 `conns` 边
  `detach_conn()` = erase 当前迭代器），改成一趟快照；`uvcpp_c_quic.h` 补上"`on_read` 的
  收尾是两条回调、结束判据是 `PEER_CLOSED` 不是 `fin`"
- 其余是 `1.4.1` → `1.4.4` 那条线上的东西：QUIC 传输层、HTTP/3、C ABI 的三批（地基 +
  net、webapp + web、HTTP/2、QUIC + HTTP/3，共 321 个函数），按主题见本文件上面的「主题清单」
- 预编译动态库：6 个平台（Windows / Linux × x64 / arm64 × MinGW-w64 / MSVC / GCC），
  依赖全静态链接

**破坏性**:无 C++ API 变更；C ABI 版本仍是 `1`，符号面 321 条不变。但发布包的**模块
集合变了**（QUIC / HTTP3 / CAPI 由 0 变 1），换包的人会多出这三批符号与头；从源码构建
的人不受影响（默认仍是 OFF）。逐条见上面「`v1.5.0` 重点」的「换二进制之前」。

### v1.4.0 (2026-09-26)

**新增**:SOAP/WSDL、应用层 JSON、日志模块完善、多循环的 Linux 内核分流

- 新模块 `src/wsdl/`（`UVCPP_ENABLE_WSDL`，默认 OFF）：WSDL 1.1 文档模型与发布层、
  SOAP 1.1/1.2 信封解析 / Fault / 序列化、operation 派发层（`1.3.7-dev`、`1.3.8-dev`）
- 应用层 JSON 构造器与字段表反射（`1.3.3-dev`、`1.3.4-dev`）
- **日志模块**：`UVCPP_LOGF` 带源码位置、`enum class`/`log_level`/`log_category`/`nullptr`
  重载、最短往返浮点、`flush()`、`SOAP`/`WSDL` 两个模块标签、`src/wsdl/` 的 9 处日志点、
  `uvcpp_json_dump` 失败不再静默，以及过滤路径与每条记录的若干次抢锁和堆分配
- `uvcpp_tcp_server::set_loops(n > 1)` 在 Linux/BSD 上改为 `UV_TCP_REUSEPORT` 内核分流
  （`1.3.5-dev`），`is_fanout()` 可问是哪一种；Windows 仍是 socket 转手
- `Date` 响应头（按秒缓存，`1.3.1-dev`）、进程内设 libuv 线程池大小（`1.3.2-dev`）、
  静态分片下发改按块借还工作池名额、503 → 0（`1.3.6-dev`）
- 其余是 `1.3.9-dev` → `1.3.32-dev` 线上约 25 笔「每请求少几次分配」的性能改动
  （webapp 登记表索引换哈希表、响应序列化不再造串、读注册表不再每请求重建、……），
  按主题见本文件上面的「主题清单」
- 预编译动态库：6 个平台（Windows / Linux × x64 / arm64 × MinGW-w64 / MSVC / GCC），
  依赖全静态链接

**破坏性**:日志模块动了一处 vtable、一处对象布局、两个新增导出符号和四个重载集 ——
**必须重编，不能只换二进制**；另有一条 `set_log_level` 的行为变更。
逐条见上面「`v1.4.0` 重点」的「换二进制之前」。

### v1.3.0 (2026-09-23)

**新增**:多循环横向扩展、HTTP/2 流级背压、TLS 主机名校验

- `uvcpp_tcp_server::set_loops(n)`（`1.2.21`）与 `uvcpp_web_app::set_loops(n)`（`1.2.23`）：
  一条接受者 + n−1 条专用线程的工作循环，配套 socket 转手原语与两个新公开头
  （`net/uvcpp_loop_worker.h`、`net/uvcpp_socket_handoff.h`）
  （**Linux 侧自 `1.3.5-dev` 起改为内核分流**：n 条循环各自绑同一端口，`is_fanout()`
  可问是哪一种；Windows 仍是上面这条）
- HTTP/2 流级背压：收方向 `pause_stream()` / `resume_stream()`、发方向单流待发队列
  上界（默认 4 MiB）、`peer_window_size()`（`1.2.25`）。**这是协议层机件，仓内没有
  应用层调用方** —— 框架侧不驱动它，h2 上「边收边给」的流式请求体仍不可达
- `tls_verify_mode::PEER_STRICT` 在客户端侧真的校验主机名（`1.2.24`）——
  此前它与 `PEER` 完全等价
- 其余是 `1.2.1` → `1.2.25` 线上的修复与性能改动，按主题见本文件上面的「主题清单」
- 预编译动态库：6 个平台（Windows / Linux × x64 / arm64 × MinGW-w64 / MSVC / GCC），
  依赖全静态链接

**破坏性**:删了 3 个公开符号、改了 7 处布局或导出符号 —— **必须重编，不能只换二进制**，
逐条见上面「`v1.3.0` 重点」的「换二进制之前」。

### v1.2.0 (2026-09-20)

**新增**:HTTP/2（nghttp2）、发布包里的调试档

- 集成 HTTP/2：nghttp2、ALPN 协商、h2 会话与连接层；`uvcpp_web_app` 零配置自动
  协商版本，低层 `http_client` / `http_server` 要 h2 得显式打开
- 每个预编译包同时提供发布档与调试档（`uvcppd.dll` / `libuvcppd.so`），MSVC 那份
  带 `uvcppd.pdb`
- 打包器改为让产物**自报家门**：两档装反、`.pdb` 对不上源，都拒绝出包
- 其余是 `1.1.1` → `1.1.35` 线上的修复与性能改动，按主题见本文件上面的「主题清单」
- 预编译动态库：6 个平台（Windows / Linux × x64 / arm64 × MinGW-w64 / MSVC / GCC），
  依赖全静态链接

### v1.1.0 (2026-09-19)

**新增**:网络层、HTTP/1.1 与 WebSocket、TLS、Web 应用框架

- 新增 `net` 模块：TCP / UDP 的服务端与客户端
- 新增 `web` 模块：HTTP/1.1（llhttp）、WebSocket（RFC 6455）、gzip/deflate 压缩、静态文件服务
- 新增 `ssl` 模块：基于 OpenSSL 的 TLS
- 新增 `webapp` 模块：路由、中间件、静态资源、流式响应、multipart 上传、文件下发、JSON、日志
- 修复内存池在 MinGW-w64 上的线程退出崩溃（根因与修法见「已知问题」），
  `expand` 模块首次随发布产物一起提供
- 预编译动态库：6 个平台（Windows / Linux × x64 / arm64 × MinGW-w64 / MSVC / GCC），
  依赖全静态链接

### v1.0.0 (2026-02-02)

**首发版本 (Initial Release)**

- ✅ 所有 14 个功能测试通过
- ✅ 支持 Windows / Linux / macOS
- ✅ 完整的 libuv API C++ 封装
- ✅ 现代 C++ 接口设计
- ✅ 智能内存管理
- ✅ 线程池支持
- ✅ 单元测试覆盖
