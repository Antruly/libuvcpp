# 变更日志

本文件是 libuvcpp 的**唯一**变更清单。两版 README 只留一个指针指向这里；
`RELEASE.md` 是逐版本的发布叙事（每版「重点」与「破坏性」那两段），但**它本身也不是
任何一版的 Release 正文** —— 正文由 `tests/tools/release_notes.py` 按 tag 现切：
一张六平台的下载表 + 那一版的「重点」小节 + 一个指回本文件的链接，整份归档不再被
当成正文贴出去。想按版本读发布叙事，翻 `RELEASE.md`；想按主题读逐条改动，读本文件。

读法：**先看主题清单**。它按模块分组，每条括号里写的是这处改动**首次出现**的开发档号
（`1.1.7`、`1.3.5-dev` 这种）。开发档不单独发布 —— 它们按线折叠进稳定版：
`1.1.x` → `v1.2.0`、`1.2.x` → `v1.3.0`、`1.3.x` → `v1.4.0`、`1.4.1`–`1.4.4` → `v1.5.0`、
`1.5.1`–`1.5.3` → `v1.6.0`。想按时间线读，翻到最后的「逐个版本」。

当前源码树是 **1.5.3-dev** —— 即 `src/uvcpp/uvcpp_version.h` 里 `UVCPP_VERSION_STRING`
报告的那个串。已打过的 tag：`v1.0.0`、`v1.1.0`、`v1.2.0`、`v1.3.0`、`v1.4.0`、`v1.5.0`。
其中若干条来自本仓第一位外部贡献者 [@sercebr](https://github.com/sercebr) 报的 issue。

## 目录

- [主题清单](#主题清单)
  - [C ABI（`uvcpp_c_*`）](#c-abiuvcpp_c_)
  - [C# 绑定（`bindings/csharp/`）](#c-绑定bindingscsharp)
  - [QUIC 传输（net 层）](#quic-传输net-层)
  - [HTTP/3（web 层）](#http3web-层)
  - [HTTP/2](#http2)
  - [数据库（`src/db/`）](#数据库srcdb)
  - [横向扩展（多事件循环）](#横向扩展多事件循环)
  - [HTTP 与 WebSocket 语义](#http-与-websocket-语义)
  - [TLS 与网络](#tls-与网络)
  - [内存与缓冲](#内存与缓冲)
  - [公开契约与安全](#公开契约与安全)
  - [性能](#性能)
  - [配置、打包与 CI](#配置打包与-ci)
- [逐个版本](#逐个版本)
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
  `uvcpp_quic_ngtcp2.h` 并列。两个都不安装（`CMakeLists.txt:2389-2389`）、打包也排除
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

### HTTP/3（web 层）

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

### 数据库（`src/db/`）

一个连接一个 `uvcpp_db_client`，**同一套接口盖住 SQLite / MySQL / PostgreSQL**
三个后端，结果行按表取（列名或列号）。**默认关**（`UVCPP_ENABLE_DB`），而且是本库
**唯一需要第三方客户端库**的模块（libsqlite3 / libmysqlclient / libpq）—— 三个后端
一个都没找到时整个模块被强制关掉并打一条说明怎么修的 warning，而不是留一个
"能配置、链不上"的组合（`1.5.3`）

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

### HTTP 与 WebSocket 语义

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

### 配置、打包与 CI

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

## 逐个版本

这一节按发版时间倒序，只列**已发布**的 tag。开发档线已折叠进对应的稳定版
（折叠表见开头），所以这里读到的每一版都是一个真实的 tag。

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
