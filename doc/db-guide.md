# 数据库模块指南

`src/db/` 是**库外的一层薄封装**：一个连接 = 一个 `uvcpp_db_client`，底下挂
SQLite / MySQL / PostgreSQL / ODBC 四个后端，SQL 由调用方写，结果按**表**取
（列名 → 值）。前三个各对着**一种**数据库，第四个对着的是一个**接口** —— 连什么库由
连接串里的驱动名决定（见 [§1](#1-怎么开) 的「ODBC：驱动管理器与**具体驱动**」）。
它不参与事件循环，也不是 ORM —— 没有模型、没有迁移、没有关系映射。
（建在它上面、替你把 `uv_queue_work` 那段样板写掉的 `uvcpp_db_async` 与
`uvcpp_db_pool` 才碰循环，见 [§3](#3-公开面)；**底座本身**不认识 `uvcpp_loop`。）

> ## 一句话说清它是什么
>
> **同步的底座**（异步门面与连接池建在它上面）、**一个 client 一个连接**、
> **不自动重连**、**返回码不抛异常**，
> 把 SQLite / MySQL / PostgreSQL 三个后端在"同一条 SQL 该得到同一个结果"这件事上
> 钉到一套契约里（第四个后端 ODBC 也进了这套套件，但它的方言按数据源分派 ——
> [§6](#6-三个后端钉住的一致与如实列出的三处不同) 写了它偏在哪、
> [§11](#11-没做的如实列出) 写了哪一族还没跑过），并把各家**真不一样**的地方如实
> 写在这里，而不是假装它们一样。
>
> 它默认**关**（`UVCPP_ENABLE_DB=OFF`）：它是全仓唯一一个必需第三方客户端库的
> 模块。发布配置**显式打开**它，所以预编译包里带着这个模块 —— `1.7.0` 那个包里
> SQLite / MySQL / PostgreSQL **三个都在**（SQLite 从源码编，MySQL/PG 编进来但库走
> 运行时加载，见 §1 与 §3）。ODBC 是 `1.7.1-dev` 才加的，**还没有哪个已发布的包
> 带过它**；它和 MySQL/PG 走同一条运行时加载（链上的是**驱动管理器**，也是"客户端
> 库"那一类）。

## 目录

- [1. 怎么开](#1-怎么开)
- [2. 连接串](#2-连接串)
- [3. 公开面](#3-公开面)
- [4. 返回码](#4-返回码)
- [5. 参数绑定与转义](#5-参数绑定与转义)
- [6. 三个后端：钉住的一致，与如实列出的三处不同](#6-三个后端钉住的一致与如实列出的三处不同)
- [7. 事务，以及"事务里不重试"](#7-事务以及事务里不重试)
- [8. 类型与精度：`DECIMAL` 的钱会丢](#8-类型与精度decimal-的钱会丢)
- [9. 跨后端共用套件抓到过的四处](#9-跨后端共用套件抓到过的四处)
- [10. 本地怎么跑](#10-本地怎么跑)
- [11. 没做的（如实列出）](#11-没做的如实列出)

---

## 1. 怎么开

```bash
cmake -S . -B build-db -DUVCPP_ENABLE_DB=ON
cmake --build build-db -j
```

| 开关 | 默认 | 语义 |
|---|---|---|
| `UVCPP_ENABLE_DB` | `OFF` | 主开关。关掉时 `src/db/` 整个不进构建，公开头的内容整段落在 `#if UVCPP_DB_ENABLE` 外面 |
| `UVCPP_ENABLE_DB_SQLITE` | `ON` | 编 SQLite 后端（要 `sqlite3.h` + libsqlite3） |
| `UVCPP_DB_SQLITE_FROM_SOURCE` | `OFF` | 上面那个 SQLite 后端改用**钉死哈希的源码 amalgamation**自己编，不走 `find_package`。**六条发布腿都是这个**（见下一节） |
| `UVCPP_ENABLE_DB_MYSQL` | `ON` | 编 MySQL 后端（要 `mysql.h` + libmysqlclient） |
| `UVCPP_ENABLE_DB_PGSQL` | `ON` | 编 PostgreSQL 后端（要 `libpq-fe.h` + libpq） |
| `UVCPP_ENABLE_DB_ODBC` | `ON` | 编 ODBC 后端（要 `sql.h` + 驱动管理器）。**连什么库不在这里定** —— 那是连接串里 `Driver=` 的事（见下面「ODBC：驱动管理器与**具体驱动**」） |
| `UVCPP_DB_RUNTIME_LOAD` | `OFF` | MySQL/PG/ODBC 的**客户端库**改成运行时加载（`dlopen`/`LoadLibrary`），只是**不在链接行上出现** —— 头还是要（见下面「运行时加载」一节）。ODBC 那一份加载的是**驱动管理器**，具体驱动仍由它自己加载 |

四个子开关的语义是**"找得到就编、找不到就降级并出声"**，不是"我要不要用它"：
想明确裁掉哪一个才写 `=OFF`（那时连"找不到"的警告都不会有）。

降级链是**两级**，两级都是 `message(WARNING)` + 普通变量 `set(... OFF)`：

1. 某一个后端找不到 → 关掉**那一个**，其余照编；
2. 四个都关掉了 → 关掉**整个模块** —— 那样的库能编出一个 `uvcpp_db_client`，
   但它谁也打不开，属于"能配置、跑不起来"。

所以 `CMakeCache.txt` 里 `UVCPP_ENABLE_DB_MYSQL:BOOL=ON` **不代表**它编进去了
—— 缓存不是判据。判据是配置期的这几行，CI 与用例都 grep 它们：

```
-- db: SQLite 后端开（3.45.1）
-- db: MySQL 后端开（/usr/lib/x86_64-linux-gnu/libmysqlclient.so）
-- db: PostgreSQL 后端开（libpq 16.15）
-- db: ODBC 后端开（驱动管理器 /usr/lib/x86_64-linux-gnu/libodbc.so）
-- Including db module in build (database integration)
```

ODBC 那一行报的是**驱动管理器**（Windows 上是 `-- db: ODBC 后端开（Windows 系统
odbc32）`）—— 它**不是**"这台机器连得上某个数据源"：那件事只有 `open()` 才知道，
见下面「ODBC：驱动管理器与**具体驱动**」。找不到 `sql.h`、或只找到头没找到
`libodbc` 时，这一行换成一条 warning 并**只关掉 ODBC 这一个后端**，其余照编。

开了 `UVCPP_DB_RUNTIME_LOAD` 时这两行会换个说法（后半截是**头**的出处，
不再是库的出处）：

```
-- db: MySQL 后端开（运行时加载，不链 libmysqlclient；头来自 /usr/include/mysql）
-- db: PostgreSQL 后端开（运行时加载，不链 libpq；头来自 /usr/include/postgresql）
```

运行期问一句更直接：

```cpp
#include <cstdio>

#include <db/uvcpp_db.h>

int main() {
  // "sqlite,mysql,postgres,odbc" —— 只列**真的编进来**的那几个
  std::printf("%s\n", uvcpp::uvcpp_db_drivers().c_str());
  return 0;
}
```

自己编的包少哪个后端时（**官方预编译包 1.7.0 里三个都在**，不会有这一行缺项；
ODBC 从 `1.7.1-dev` 起进默认配置，还没有哪个已发布的包带过它），
**这一行是第一个该看的地方**。

> **注意这一行说的是"编进来没有"，不是"这台机器上能不能用"。**
> 开了 `UVCPP_DB_RUNTIME_LOAD` 之后这两件事分了家：MySQL 打印在清单里，
> 只代表**这个库带了这个后端**；客户端库在不在是**运行时**才知道的
> （下一节）。要问"现在真能用吗"，看 `open()` 的返回码。

### 客户端库运行时加载：`UVCPP_DB_RUNTIME_LOAD`

**为什么要有它。** 直接链客户端库时，`libpq.so.5` / `libmysqlclient.so.21` /
`libodbc.so.2` 会变成产物的一条 `DT_NEEDED`，而 **`DT_NEEDED` 是加载期解析的**：
装了 libuvcpp 的机器上只要缺这一条，**进程根本起不来** —— 一行 db 代码都没跑，
甚至这个程序压根不碰数据库。Windows 同理（PE 的导入表），**只有 `odbc32.dll` 是
例外**：它属于操作系统，链接它不算代价 —— 所以这一档对 ODBC 的意义集中在
Linux / macOS 的产物上（那上面 ODBC 的"客户端库"是普通包 unixODBC）。

本机实测（同一个 `hello`，只 `printf`，一行 db 代码不跑）：

```
$ ldd libuvcpp.so | grep 'not found'
        libmysqlclient.so.21 => not found
        libpq.so.5 => not found
$ ./hello
./hello: error while loading shared libraries: libmysqlclient.so.21:
         cannot open shared object file: No such file or directory     # rc=127
```

开了这个开关之后，改由**运行时**去找：

```bash
cmake -S . -B build-db -DUVCPP_ENABLE_DB=ON \
      -DUVCPP_ENABLE_DB_MYSQL=ON -DUVCPP_ENABLE_DB_PGSQL=ON \
      -DUVCPP_ENABLE_DB_ODBC=ON \
      -DUVCPP_DB_RUNTIME_LOAD=ON
```

| | 直接链（默认，`OFF`） | 运行时加载（`ON`） |
|---|---|---|
| 链接期要 | `mysql.h`/`libpq-fe.h`/`sql.h` **+** 客户端库 | 只要头（`libmysqlclient-dev` / `libpq-dev` / `unixodbc-dev`） |
| 产物的 `DT_NEEDED` | 多三条 | **一条都不多** |
| 机器上没装那些库 | **进程起不来** | 进程照常跑；HTTP、SQLite 一切照常 |
| 真去连 MySQL/PG/ODBC | 能连 | 返回 `NO_DRIVER` + `last_error()` 里写明**试过哪几个库名** |
| 找库的办法 | 链接器（`/etc/ld.so.cache`、`RPATH`） | 固定的候选名列表，或 `UVCPP_DB_MYSQL_LIBRARY` / `UVCPP_DB_PGSQL_LIBRARY` / `UVCPP_DB_ODBC_LIBRARY` |

候选名（按顺序试，全试完才报错）：

| 平台 | MySQL | libpq | ODBC（驱动管理器） |
|---|---|---|---|
| Linux | `libmysqlclient.so.21`、`libmysqlclient.so.24`、`libmariadb.so.3`、`libmysqlclient.so` | `libpq.so.5`、`libpq.so` | `libodbc.so.2`、`libodbc.so` |
| Windows | `libmysql.dll`、`libmariadb.dll`、`mysqlclient.dll` | `libpq.dll` | `odbc32.dll`（系统件，一定在） |
| macOS | `libmysqlclient.21.dylib`、`libmariadb.3.dylib`、`libmysqlclient.dylib` | `libpq.5.dylib`、`libpq.dylib` | `libodbc.2.dylib`、`libodbc.dylib` |

两个环境变量是**显式指定**，设了它就**只试这一个**（不再走候选名列表）——
装在非标准前缀下的人用它，而它同时是**"没装库"那条路的确定性旋钮**：
指向一个不存在的路径，就能在装好了库的机器上复现"没装"的样子。

```bash
UVCPP_DB_PGSQL_LIBRARY=/nonexistent/libpq.so.5 ./myapp
# last_error(): 加载 PostgreSQL 客户端库失败，试过这些名字都没打开：
#   UVCPP_DB_PGSQL_LIBRARY=/nonexistent/libpq.so.5（...: cannot open shared object file: ...）。
#   装一份 libpq（...），或者把环境变量 UVCPP_DB_PGSQL_LIBRARY 指到 libpq 的完整路径。
```

拿到的句柄**永不释放**：没有哪个时刻能保证"再也不会有人用它"（活连接、活语句
都可能还在），卸掉比留着危险。

**它只改"符号从哪来"，不改后端在不在**：`UVCPP_DB_MYSQL_ENABLE` /
`UVCPP_DB_PGSQL_ENABLE` / `UVCPP_DB_ODBC_ENABLE` 那三个宏照旧是"头找得到就是 1"，
`uvcpp_db_drivers()` 的输出也不变。换句话说 `NO_DRIVER` 现在有**两条**来路
（后端没编进来 / 编进来了但这台机器没库），`last_error()` 会说清是哪一条 ——
详见 `uvcpp_db_status.h` 里 `NO_DRIVER` 那一段的文档注释。

**共享库上这一档还多一条链接选项 `--no-undefined`**（ELF）：调用点全改成经
`api()` 那张 `dlsym` 函数指针表走，而**共享库默认允许未定义符号** ——
漏绑/漏改一处，`ld` 不吭声，产物却带着一个没人提供的 `mysql_query`（或
`SQLExecDirectW`），于是**每个使用者**在加载这个 `.so` 时就炸。`--no-undefined`
把这条不变式交还给链接器：漏一处就是链接期红。加 ODBC 那一份时这条兜底直接生效
过 —— `nm -u …/uvcpp_db_odbc.cpp.o` 里剩下 **0 个** `SQL*` 未定义符号。

### ODBC：驱动管理器与**具体驱动**是两件事

前三个后端各自钉死一种客户端库（libsqlite3 / libmysqlclient / libpq），ODBC 对着
的是一个**接口**：编进来的是 ODBC **驱动管理器**（Linux 的 `libodbc`、Windows 的
系统 `odbc32`），**连到哪张库**由连接串里的 `Driver=`（或 `DSN=`）决定。那个**具体
驱动**（比如 SQL Server 的 `ODBC Driver 18 for SQL Server`，官方 `msodbcsql18`）
**既不归构建管、也不归这个库管**：装没装、装的是哪一版，只有 `open()` 那一刻才
知道。接 SQL Server 走的就是这条路 —— 微软没给它一个可链接的 C 客户端库，它给的
就是 ODBC 驱动，所以在 C 层没有第二个选择；而这个后端本身是**按通用写的**：能连
SQL Server，也能连任何装得出 ODBC 驱动的数据源（Oracle、DB2、达梦……），全文件
没有一处"是 SQL Server 就怎么着"。

由此有三条与前三个后端不同的、值得先知道的：

* **开关说的是"这份产物有没有这个后端"，不是"能不能连上"。** 配置期只要 `sql.h`
  与驱动管理器两样。缺 `sql.h` 时打一条 warning（Debian/Ubuntu 上装 `unixodbc-dev`，
  RHEL 系 `unixODBC-devel`）并只关掉 ODBC 这一个后端。**只装运行时 `unixodbc` 的
  机器上也会走到这条**：那个包给的是 `libodbc.so.2`，而 `.so` 这个**开发符号链接**
  是 `unixodbc-dev` 才有的 —— 所以要的是 `unixodbc-dev`，不是 `unixodbc`。
* **连接失败时报的是驱动管理器给的原文**（驱动名写错时就是它那句"找不到数据源、
  也没有默认驱动"），本模块不把它改写成自己的话：这句话是判"我的连接串写错了"
  还是"这台机器没装驱动"唯一有用的信息。它**不走 `NO_DRIVER` 那条路** ——
  `NO_DRIVER` 的含义是"这个后端没编进这份包"（见 [§2](#2-连接串)）。
* **它也走 `UVCPP_DB_RUNTIME_LOAD`：链上的是驱动管理器，那也是"客户端库"。**
  起先这里写的是"ODBC 不走这条路，因为驱动管理器本来就该在、而且很小" —— 那句话
  **只在 Windows 上成立**（`odbc32.dll` 属于操作系统，和 `ws2_32` 同类，链接它没有
  代价）；Linux / macOS 上驱动管理器是一个普通包（unixODBC），链上它就是在产物里
  加一条 `DT_NEEDED libodbc.so.2`，正是上面「运行时加载」那一节要消灭的形状：

  ```
  两棵树只差 `-DUVCPP_DB_RUNTIME_LOAD` 这一个开关（其余逐条相同）：

  $ readelf -d /tmp/uvcpp-off/libuvcpp.so | grep NEEDED  # 关（默认）
    NEEDED               libuv.so.1
    NEEDED               libsqlite3.so.0
    NEEDED               libodbc.so.2
    NEEDED               libstdc++.so.6

  $ readelf -d /tmp/uvcpp-rt/libuvcpp.so | grep NEEDED   # -DUVCPP_DB_RUNTIME_LOAD=ON
    NEEDED               libuv.so.1
    NEEDED               libsqlite3.so.0
    NEEDED               libstdc++.so.6
  ```

  于是它和 MySQL/PG 同一条规矩：**开** ⇒ 产物里不出现这个库，"这台机器没装
  unixODBC"推迟到**真去连**那一次（拿到的是 `NO_DRIVER` 加一句写清了试过哪些库名
  的话）；**关**（默认）⇒ 老实链上。两件事别混：**驱动管理器**是这一层的依赖，
  它下面的**具体驱动**由它自己 `dlopen`，那是 `open()` 那一刻的事（上面第一条）。

  候选名是各平台的惯例名（Linux `libodbc.so.2` / `libodbc.so`、macOS
  `libodbc.2.dylib` / `libodbc.dylib`、Windows `odbc32.dll`），要指到别处用
  `UVCPP_DB_ODBC_LIBRARY`。**iODBC 有意不在候选名里**：它是另一个驱动管理器实现，
  符号名一样但行为没在本仓量过 —— 想用它就显式指路，别让"打开成功"盖住"换了一个
  没量过的实现"。

连接串里的默认驱动名（`ODBC Driver 18 for SQL Server`）**只是默认**：`?Driver=` 或
`?DSN=` 一写就换掉（见 [§2](#2-连接串)）。

### 预编译包里的 SQLite 是**从源码编的**：`UVCPP_DB_SQLITE_FROM_SOURCE`

六条发布腿各传这几个 `-D`（**1.7.0 起**；在那之前后两行是 `=OFF`）：

```
-DUVCPP_ENABLE_DB=ON -DUVCPP_DB_SQLITE_FROM_SOURCE=ON \
-DUVCPP_ENABLE_DB_MYSQL=ON -DUVCPP_ENABLE_DB_PGSQL=ON \
-DUVCPP_DB_RUNTIME_LOAD=ON
```

也就是说 —— **`1.7.0` 那个预编译包里的 db 模块三个后端都在**（`uvcpp_db_drivers()`
会打印 `sqlite,mysql,postgres` —— 是**驱动的稳定短名**，`pgsql` 只是 `scheme` 的
别名之一，见下面的别名表），但 MySQL/PG 的**客户端库**是运行时才打开的，链接行上
不出现（见下面「客户端库运行时加载」那一节）。

**ODBC 是 `1.7.1-dev` 才加的，这六条腿的 `-D` 清单里还没有它。**
它默认 `ON`，但决定它在不在包里的不止 `-D`：**Linux 那几条腿上还得装
`unixodbc-dev`** —— 现在的 `apt-get install` 只装 `zlib1g-dev ninja-build`，runner
上没有 `sql.h`，上面那条 warning 会把 ODBC 后端**强制关掉**，于是 Linux 包照样没有
它（Windows 腿上 `sql.h` 来自 SDK，所以 Windows 包里会有）。补上之后不必担心多出
依赖：腿里已经有 `-DUVCPP_DB_RUNTIME_LOAD=ON`，ODBC 跟着它走，**不会**往产物里添
`DT_NEEDED libodbc.so.2`，那六条腿只需要装头。这件事该由发布配置**显式**写出来
（`-DUVCPP_ENABLE_DB_ODBC=ON` + runner 上的 `unixodbc-dev`），而不是靠默认值漂进去
—— 留在这里当一条待办。

SQLite 那一半必须从源码编，不是漏配，
是两条实测理由把另两条路都堵死了：

| 想链系统的那份 | 实测结果 |
|---|---|
| `libsqlite3.so` | 产物多一条 `DT_NEEDED`。发布腿有一条 `ldd` 断言在拦这个形状（原本的名单里还没有 `sqlite3` —— 已经补上，否则它会一条条放行） |
| `libsqlite3.a` | Ubuntu 24.04 上那份**不是 PIC**：`gcc -shared -fPIC … /usr/lib/x86_64-linux-gnu/libsqlite3.a` 当场报 `relocation R_X86_64_PC32 against symbol 'sqlite3CtypeMap' can not be used when making a shared object; recompile with -fPIC` |

MySQL / PostgreSQL 在 1.6.x 的包里是**显式关**的，不是"找不到"：runner 上装着
`libpq-dev` 时 `find_package` 会**静默**成功，发布包就悄悄多一条 `libpq.so.5` 的
`DT_NEEDED`。1.7.0 起放着 D 那条路不用了 —— 后端编进去、库运行时取 —— 因为
`DT_NEEDED` 是加载期解析的：链上就等于"没装客户端库的机器连**加载**这份包都做不到"。

打开这个开关之后，配置期会做这几件事（都可以在本机复现）：

```
-- db: 下载 SQLite amalgamation 3540000（sqlite.org，钉死哈希）
-- db: SQLite 后端开（3.54.0，源码 amalgamation，静态）
```

1. 从 `https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip` 下那份
   **amalgamation**（一个 `sqlite3.c` + 一个 `sqlite3.h`，没有别的依赖），
   `EXPECTED_HASH SHA3_256=7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93`
   ——这个值就是 sqlite.org 下载页上印在那一行旁边的那个，逐字对上。
   `TLS_VERIFY ON`。zip 落在 `<build>/_deps/uvcpp-sqlite/`，下次配置**再核一遍哈希**
   （那个文件就躺在构建目录里、谁都能改；只核一次等于把"上次下对了"当成"这次也对"）。
2. 解出 `sqlite-amalgamation-3540000/sqlite3.c`，编成静态目标 `uvcpp_sqlite3`，
   **显式 `POSITION_INDEPENDENT_CODE ON`** —— 不靠全局那个
   `CMAKE_POSITION_INDEPENDENT_CODE`：这一份一定会进共享库，而使用者那棵树未必传了
   全局开关，那时症状是链接期一句 `recompile with -fPIC`，离原因很远。
3. 版本号从解出来的 `sqlite3.h` 里读（`SQLITE_VERSION`），不在 CMake 里再抄一遍。
   所以上面那行 STATUS 里的 `3.54.0` 是**编进去的那个版本**，不是想象的。

**这条路不调用 `find_package`。** 与 `UVCPP_BUILD_LIBUV_FROM_SOURCE` 同一条纪律：
不猜机器上有什么。"优先源码、找不到再看系统"是不够的——runner 上装着
`libsqlite3-dev` 时它会静默链上系统那份，而那种产物在开发机上照样跑得动。

离线机器：把 `sqlite-amalgamation-3540000.zip` 自己放到
`<build>/_deps/uvcpp-sqlite/` 下（哈希要对得上），或者直接
`-DUVCPP_DB_SQLITE_FROM_SOURCE=OFF` 回到系统那份（本地开发用完全够）。

**这条路只影响 SQLite 后端怎么来，不动公开面**：`uvcpp_db_client` 的方法、
返回码、连接串格式一个字不变。

### 客户端库装在非标准位置

**默认（直接链）**：不用新开关，走 CMake 的常规搜索路径：

```bash
cmake -DUVCPP_ENABLE_DB=ON -DCMAKE_PREFIX_PATH=/opt/mysql ..
# 或者直接点名（find_path/find_library 会尊重预先设好的同名 cache 变量）
cmake -DUVCPP_ENABLE_DB=ON \
      -DUVCPP_DB_MYSQL_INCLUDE_DIR=/opt/mysql/include \
      -DUVCPP_DB_MYSQL_LIBRARY=/opt/mysql/lib/libmysqlclient.so ..
# PostgreSQL 走 CMake 自带的 FindPostgreSQL
cmake -DUVCPP_ENABLE_DB=ON \
      -DPostgreSQL_INCLUDE_DIR=/opt/pgsql/include \
      -DPostgreSQL_LIBRARY=/opt/pgsql/lib/libpq.so ..
# ODBC 是本模块自己的两个 find_*
cmake -DUVCPP_ENABLE_DB=ON \
      -DUVCPP_DB_ODBC_INCLUDE_DIR=/opt/unixodbc/include \
      -DUVCPP_DB_ODBC_LIBRARY=/opt/unixodbc/lib/libodbc.so ..
```

**开了 `UVCPP_DB_RUNTIME_LOAD`**：位置的找法分成两截 ——

* **头**（编译期）仍旧走上面那套，但 PG 那边**不再用 `find_package(PostgreSQL)`**
  （它一个判据同时含头与库，而"库在不在"这条路上与我们无关；它还会去读
  `PostgreSQL_LIBRARY` 那个 cache 变量）。改成一个 `find_path`，所以给的是
  `-DUVCPP_DB_PGSQL_INCLUDE_DIR=/opt/pgsql/include`（MySQL 那个同名变量照旧）。
* **库**（运行期）与构建**完全无关**，构建期给什么都没用。装库的人在**跑的时候**
  用环境变量指路，而且设了就**只试这一个**：

  ```bash
  UVCPP_DB_MYSQL_LIBRARY=/opt/mysql/lib/libmysqlclient.so.21 \
  UVCPP_DB_PGSQL_LIBRARY=/opt/pgsql/lib/libpq.so.5 \
    ./myapp
  ```

  注意这**不是** CMake cache 变量 —— 与构建期那两个同名变量是**同名不同物**，
  别指望 `cmake -DUVCPP_DB_PGSQL_LIBRARY=…` 会影响运行时（那时链接行上根本没有它）。

### 静态库上的已知缺口

四个后端用到的库（`sqlite3` / `pq` / `mysqlclient` / ODBC 的驱动管理器 `odbc`）
挂在 `$<BUILD_INTERFACE:>` + `PRIVATE` 下：**共享库**上 `DT_NEEDED` 会跟着走，
使用方什么都不用做；**静态库**（`uvcpp_a` / `uvcpp_a_s`）上那一条不传给消费者，
使用方要自己链：

```cmake
target_link_libraries(myapp PRIVATE uvcpp_a sqlite3 pq mysqlclient odbc)
```

这是**故意**的取舍，与 zlib / nghttp2 那几处同形：把绝对路径写进导出集会过得了
`install(EXPORT)` 却搬到别的机器才断，比缺一条 `target_link_libraries` 更难查
（后者是链接期一句 `undefined reference to sqlite3_open_v2`，指向明确）。

**开了 `UVCPP_DB_RUNTIME_LOAD` 时这一节不适用**：MySQL/PG 的客户端库根本不在
`UVCPP_DB_LINK_LIBS` 里（那正是那个开关干的事），所以静态库的使用方也**不用**
再补 `pq`/`mysqlclient` —— 只剩 SQLite 那一条要不要补，取决于它是系统那份还是
`UVCPP_DB_SQLITE_FROM_SOURCE` 编进来的静态目标。**ODBC 那一条不受这个开关影响**：
驱动管理器照旧在链接行上，所以静态库的使用方照旧要补（Windows 上是 `odbc32`）。

---

## 2. 连接串

```
sqlite:///abs/path/app.db        sqlite://rel/path.db       sqlite::memory:
mysql://user:pass@host:3306/dbname
postgres://user:pass@host:5432/dbname
odbc://user:pass@host:1433/dbname?Driver={ODBC Driver 18 for SQL Server}
odbc:?DSN=MyDsn
```

| 项 | 规则 |
|---|---|
| scheme 别名 | `postgres` / `postgresql` / `pgsql` / `pg` → `postgres`；`mariadb` → `mysql`。**别名归一化之后**才去做"认不认得"的判断 |
| 缺省端口 | MySQL 3306、PostgreSQL 5432。**ODBC 没有缺省端口**：不写就不是 `tcp:host,port` 而是 `Server=tcp:host` |
| 密码 | 可省（`mysql://root@host/db`）。URL 里带特殊字符要自己转义，本模块不做 URL 解码 |
| SQLite 的路径 | `sqlite://` 后面**原样**当一个路径用（不解析 user/host/port）。父目录不存在时驱动会**建**出来 |
| ODBC 的 host / 库名 | `host:port` → `Server=tcp:host,port`，`/dbname` → `Database=`，`user:pass@` → `Uid=` / `Pwd=` |
| ODBC 的 `?` 之后 | **原样**追加到 ODBC 连接串末尾（本模块不解析它，`;` 分隔），所以那里能写任何驱动认得的键 |
| 没带 scheme | `BAD_URL`。报错原文里会带上传进来的整串 |

ODBC 那两行的**默认值只补缺**：连接串里已经写了 `Driver=` / `DSN=` / `FILEDSN=`
就**一个都不补**（`?DSN=MyDsn` 那行就是完全不碰主机与库名的用法）；没写时补的是
`Driver={ODBC Driver 18 for SQL Server}` 加一条 `Encrypt=no`（18 版默认强制加密，
而内网库多半没配证书 —— 不显式关掉的话，使用者会先撞上一个"证书不受信任"的握手
错误，然后去怀疑连接串写错了）。同样，没给账号时补 `Trusted_Connection=yes`
（Windows 集成认证：同域部署下更该用的方式，而且没有口令进连接串，也就不会进日志），
`?Uid=` / `?User=` 里写了一个就不补。

`Server=tcp:host,port` 这个**带逗号的写法是 SQL Server 的**。别的驱动的连接串文法
各认各的，认不出 `,port` 的那个会忽略它或报错 —— 这是"通用驱动"在连接串这一层的
固有代价，写在这里而不是藏起来：换数据源时 `?` 之后通常也要跟着换。

### `BAD_URL` 与 `NO_DRIVER` 是两件事

这是本模块唯一一处"两个返回码看着像、含义差很远"的分界，值得单说：

* `BAD_URL` —— 这个 **scheme 根本不存在**（`oracle://…`、少写 scheme）。是调用方写错了。
* `NO_DRIVER` —— 这个后端**存在、但这份包没编进来**（`mysql://…` 而包里只有
  SQLite）。换一份包就好，SQL 一个字不用改。

判据是「这个 scheme 是不是本模块支持过的后端」（`uvcpp_db_driver_known()`），
**不是**「这份构建里有没有」。用后者的话两种情况都是假，分不开 —— 而"打不开连接"
这个笼统说法，恰恰是最没用的一句错误信息。用例里两条都有断言。

ODBC 上还有**第三种**"打不开"，它在返回码上不算前两种：后端编进来了、驱动管理器
也在，缺的是**那个数据源的驱动**（或者驱动名写错了）。这一种要到 `open()` 才知道，
返回的是驱动管理器给的原话，`last_error()` 里就是它 —— 别去 `NO_DRIVER` 那边找
（那条只会在"这份包没编 ODBC 后端"时出现）。

---

## 3. 公开面

```cpp
#include <cstdio>

#include <db/uvcpp_db.h>

int main() {
  uvcpp::uvcpp_db_client db;
  uvcpp::uvcpp_db_status st = db.open("sqlite:///tmp/app.db");
  if (!uvcpp::uvcpp_db_ok(st)) {
    std::fprintf(stderr, "%s\n", db.last_error().c_str());
    return 1;
  }

  uvcpp::uvcpp_db_table t;
  db.query("SELECT id, name FROM users WHERE age > ?", {18}, &t);
  for (const uvcpp::uvcpp_db_row& row : t.rows()) {
    std::printf("%s\n", row["name"].to_text().c_str());
  }
  return 0;
}
```

| 组 | 成员 |
|---|---|
| 连接 | `open` / `close` / `is_open` / `reconnect` / `ping` / `url` / `driver_name` / `last_error` |
| 读 | `query(sql, &t)`、`query(sql, params, &t)` |
| 写 | `execute(sql, [affected])`、`execute(sql, params, [affected])`、`insert(sql, params, &new_id)` |
| 自增 | `last_insert_id()` / `last_insert_id(&v)` |
| 事务 | `begin` / `commit` / `rollback` |
| 元信息 | `table_names(&v)` / `table_schema(table, &t)` |
| 转义 | `escape(text)` / `escape_identifier(ident)` |
| 配置 | `set_timeout_ms` / `timeout_ms` / `set_log` |
| 自由函数 | `uvcpp_db_open(url, &status, &error)`、`uvcpp_db_drivers()` |

四条容易踩的：

* **`last_error()` 不会因为一次成功就清空** —— 失败现场比成功更值得留着。
  所以判"这次行不行"看返回码，别去看 `last_error()` 空不空。
* **`uvcpp_db_open()` 的 `error` 出口是必需的**，不是可选装饰：失败时那个
  client 当场就被销毁了，`last_error()` 跟着没 —— 只给返回码的话，"认证失败"
  与"库不存在"在调用方眼里是同一个 `OPEN_FAILED`。
* **`row["name"]` 保证不崩**：找不到列时给一个静态的 NULL 值。
  行/列越界同样返回 NULL 值，不抛也不越界读。
* **`set_log()` 的钩子是在「锁里面」被调的，别在里面阻塞。** 它在**发起这次
  查询的那条线程**上被调（异步门面下就是池线程），而调用时 client 的互斥量还在
  手上 —— 日志是 `query()` 体内记的，锁罩着整个函数体。在钩子里等一件事 = 替整个
  client 攥着锁：同一个 client 上别的线程的查询会**全部**停住。异步门面下这足以
  凑出死锁（同一个 client 上投两条活，其中一条的钩子里等另一条 —— 另一条正卡在
  锁上等它）。要记日志就往缓冲区里塞，别在钩子里做慢事。

### 同步底座 + 异步门面

三个客户端库全是阻塞 API，没有异步版本。把它们塞进事件循环只有两条路：丢线程池
（`uv_queue_work`）或者自己实现协议。`uvcpp_db_client` 是**第一条路的底座**，而
`uvcpp_db_async`（`#include <db/uvcpp_db_async.h>`）就是建在它上面、替你把那段
样板写掉的那一层。

`uvcpp_db_client` 内部有一把递归锁：同一个 client 可以被多个线程调，但**同一时刻
只有一个在真的用连接**。要并发就开多个 client（或者 [§3.1 的连接池](#31-连接池)）。

#### 为什么它是"底座"而不是"就该同步"

同步底座本身不是缺陷，缺陷是**在事件循环线程上直接调它**。量具
（`bench/bench_db.cpp` 第 9 节）把代价量出来了 —— 一条 1 ms 的定时器挂在循环上，
同期跑 10 条 50 ms 的慢查询，记相邻两次到点的最大间隔：

| 这批查询怎么跑 | 循环被卡多久（max gap） | 同批工作总耗时 |
|---|---|---|
| 直接在循环线程上同步跑 | **504 ms**（MySQL） | 504 ms |
| 手写 `uvcpp_work` 派到线程池 | **2.1 ms** | 506 ms |
| `uvcpp_db_async` 派到线程池 | **2.0 ms** | 505 ms |

三行里最要紧的是**后两行的总耗时和第一行一样**：异步买的是"循环不被卡住"，
**不是**"查询变快"。同一个 client、同一条连接、同一把锁，总时间当然一样。

#### `uvcpp_db_async`：两种形状

```cpp
#include <cstdio>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_async.h>
#include <handle/uvcpp_loop.h>

// ① 回调式：cb 在 loop 的线程上被调（loop 由调用方自己 run）
void submit_on_loop(uvcpp::uvcpp_db_client* db, uvcpp::uvcpp_loop* loop) {
  uvcpp::uvcpp_db_async a(db);        // 绑连接，不持有
  a.query(loop, "SELECT id, name FROM users WHERE city = ?", {"杭州"},
          [](uvcpp::uvcpp_db_status st, const uvcpp::uvcpp_db_table& t) {
            if (!uvcpp::uvcpp_db_ok(st)) return;   // 失败时 t 是空表
            for (const uvcpp::uvcpp_db_row& row : t.rows()) {
              std::printf("%s\n", row["name"].to_text().c_str());
            }
          });
  a.execute(loop, "UPDATE users SET age = ? WHERE id = ?", {30, 7},
            [](uvcpp::uvcpp_db_status, int64_t) {});
}

// ② future 糖：给不在循环上的调用方（门面懒起一条私有循环线程）
uvcpp::uvcpp_db_result fetch_from_worker(uvcpp::uvcpp_db_client* db) {
  uvcpp::uvcpp_db_async a(db);
  auto f = a.query("SELECT COUNT(*) FROM users");
  return f.get();                     // 阻塞**调用方这条线程**
}
```

契约有四条，都是承重的：

* **完成回调一定在 `loop` 的线程上被调，且只调一次。** 这不是本库拼的，是
  `uv_queue_work` 的 after-work 由 libuv 从 `uv__work_done` 里在循环线程上发出的
  结果 —— 所以这里**没有** `uv_async` 邮箱。
* **投递失败（loop 传空、同一个 work 复用）**：`query()`/`execute()` **返回负值**，
  **回调不会被调**。别等回调，看返回值。
* **在回调里 `delete` 门面是合法的。** 门面析构第一件事就断掉存活凭证，之后跑完的
  活不再调用户闭包，也不会二次回调。
* **future 的 `get()` 会阻塞调用方自己那条线程** —— 在事件循环线程上用它就是把
  刚躲开的阻塞又请回来。不在循环上的线程（工作线程、C# 的线程池）才用它。

#### 它用的是**全库唯一**那条线程池，和 `uv_fs_*` / DNS 共用

`uv_queue_work` 丢进的是 libuv 的默认线程池，**默认只有 4 条线程**
（`UV_THREADPOOL_SIZE` 可改，1..1024，**第一次投递时**读进缓存）。同一条池子还被
`uv_fs_*`、`uv_getaddrinfo`、`uv_getnameinfo`、`uv_random` 用着 —— 所以
**一条 30 秒的慢查询会把文件读写和 DNS 一起堵住**。要变就在任何投递之前调
`uvcpp_set_threadpool_size()`。这是这套模型的已知代价，不是门面漏做了什么。

门面**不做背压**（不限制在途条数）。一条连接本来就是串行的，在途多条只会排队等那把
递归锁，不会更快；真正的并发看下一节的连接池。

**别把异步和同步混着用来"省时间"**：在循环线程上调一次同步 `db.query()`，会去等
worker 手里的锁 —— 不会死锁，但会把循环卡住，正好抵消掉异步的全部意义。

### 3.1 连接池

`uvcpp_db_pool`（`#include <db/uvcpp_db_pool.h>`）是一组 `uvcpp_db_client`，按需借出、
用完归还。它买的是**连接复用**（不再每次付 `open` 的 1.9~2.4 ms）与**多条连接**
（上一段那把递归锁不再互相等）—— 正好补上异步门面补不了的那一半：异步买的是"循环
不被卡住"，池子买的是"并发"。**单线程串行跑的场景它一分钱都省不下来**，还会多一层
借还，那就别用，一个 `uvcpp_db_client` 加门面就够。

量具 `bench/bench_db.cpp` 第 10 节，4 条线程 × 500 次点查：

| 连接怎么来 | MySQL | PostgreSQL | SQLite |
|---|---|---|---|
| 4 条线程共享 1 个 client | 211.5 ms | 101.3 ms | 16.4 ms |
| 4 条线程各开 1 个 client | 64.1 ms | 29.6 ms | 7.1 ms |
| 4 条线程走池子（`max=4`） | **61.9 ms** | **32.8 ms** | **6.0 ms** |
| 池子 / 各开一条 | 0.97× | 1.11× | 0.85× |

判据就是最后一行的**同量级**（1× 附近）：池子买的正是"各开一条"那份并行度，多出来的
只有一次借还（一把锁 + 一次出队）。同时打印的 `created_total()` 是**复用的见证** ——
它停在 `max`（4）上而不是 2000，说明连接是借来借去，不是每次新开。

```cpp
#include <cstdio>

#include <db/uvcpp_db.h>
#include <db/uvcpp_db_pool.h>

// 建池：min 条**当场**开好 —— 连不上在这里就报出来，不留到第一次查询
int open_pool(uvcpp::uvcpp_db_pool* pool) {
  if (!uvcpp::uvcpp_db_ok(pool->init("sqlite:///tmp/app.db", 2, 8))) {
    std::fprintf(stderr, "%s\n", pool->last_error().c_str());
    return 1;
  }
  return 0;
}

// ① 代借代还：单条语句用这个 —— 一次调用借一条、用完立刻还
void count_users(uvcpp::uvcpp_db_pool* pool) {
  uvcpp::uvcpp_db_table t;
  if (!uvcpp::uvcpp_db_ok(pool->query("SELECT COUNT(*) AS n FROM users", &t))) {
    std::fprintf(stderr, "%s\n", pool->last_error().c_str());
    return;
  }
  for (const uvcpp::uvcpp_db_row& row : t.rows()) {
    std::printf("%s\n", row["n"].to_text().c_str());
  }
}

// ② 借还：借到还之间这条连接**归你独占** —— 事务只能这么写
void transfer(uvcpp::uvcpp_db_pool* pool, int64_t from, int64_t to, int64_t amount) {
  uvcpp::uvcpp_db_client* c = pool->acquire();  // 会阻塞（默认 5 s）
  if (c == nullptr) return;                     // 借不到：原因在 pool->last_error() 里
  c->begin();
  c->execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", {amount, from});
  c->execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", {amount, to});
  c->commit();
  pool->release(c);                             // 一定要还
}
```

| 组 | 成员 |
|---|---|
| 起停 | `init(url, min=1, max=8)` / `close()` |
| 借还 | `acquire()` / `acquire(timeout_ms)` / `release(c)` / `discard(c)` |
| 代借代还 | `query(...)` / `execute(...)` / `insert(...)` / `ping()` |
| 异步 | `query(loop, ...)` / `execute(loop, ...)` / `insert(loop, ...)`（回调形状与门面**完全一致**） |
| 调参 | `set_acquire_timeout_ms`(5000) / `set_idle_timeout_ms`(60000) / `set_check_on_acquire`(false) / `set_timeout_ms` |
| 观测 | `size` / `in_use` / `idle` / `max_size` / `min_size` / `created_total` / `reused_total` / `last_error` / `close_idle` |

**异步池查询每次调用自己借还一条连接**，所以单条语句走它最省事；而**事务走不了异步的**
—— 它要独占一条连接直到 `commit`，异步那套的借还在回调之前就结束了。事务只有
`acquire()`/`release()` 这一条路，池子也因此**不提供** `begin()`。

四条承重语义：

* **`min` 条立刻开**，失败当场报 `OPEN_FAILED`（`open` 一次 1.9~2.4 ms，留到第一次
  查询再开是把延迟藏起来，不是消掉）。`min` 默认 1。
* **增长发生在 `acquire()` 的调用线程上**（`total < max` 就新开一条）。异步路径下那
  就是池线程 —— 不卡循环；同步路径下就是调用方自己。
* **坏连接不回池子**：借出去的那条一旦返回 `NOT_CONNECTED`，池子把它 `discard` 掉
  （代借代还那组自己就是这么做的）。**不自动重试** —— 写操作重试就是重复写，与
  [§7](#7-事务以及事务里不重试)同一条原则。默认也不在借出时 `ping`（`ping` 一次约
  20 µs，每次借出都付会吃掉池子省下的一部分），要开用 `set_check_on_acquire(true)`。
* **池子不回收自己**：不起后台线程、不挂定时器。缩容只在 `acquire()` / `release()`
  顺手做（按 `idle_timeout_ms`），或者你显式 `close_idle()`；两者都**不会缩到 `min`
  以下**。一个池子挂着 `min` 条空闲连接是设计，不是漏回收。

三条禁令，都是这套接口形状的直接后果（详见 `src/db/uvcpp_db_pool.h` 的类注释）：

1. **`acquire()` 是阻塞的，绝不能在事件循环线程上调。** 要在循环上取连接就用异步
   接口（借还在池线程里做）。
2. **代借代还的同步方法（`pool.query` 等）同样是阻塞的**，它们是给工作线程用的。
3. **别在借出期间去投异步池查询。** 异步池查询自己要借一条连接：`max=1` 时那唯一一条
   在你手里，池线程会一直等到借出超时。它**会**醒（超时是硬的，默认 5 s），但你会拿到
   `NO_CONNECTION` 而不是结果。这条同时也是"libuv 默认池只有 4 条线程、
   `uv_fs_*` / DNS 共用"那条代价的另一面：4 条池线程都去等连接，文件 IO 就一起饿着。
   用例 `tests/functional/db_sqlite_pool_func.cpp` 钉的就是这个 —— 借走唯一一条、投
   异步池查询、断言它在超时后拿到 `NO_CONNECTION` 而**不是挂住**。

### 3.2 C 接口（`include/capi/uvcpp_c_db.h`，1.5.3 起）

C# / Rust / Python 的 `ctypes` 用数据库模块**不必**先写一层 C++。这一片是上面几组的
C 面，一格一格对着：

| C 面 | C++ 那侧 |
|---|---|
| `uvcpp_c_db_client_*` | `uvcpp_db_client` 的同名方法 |
| `uvcpp_c_db_params_*` | `uvcpp_db_params`（八个 `add_*` 逐个对上它的重载）|
| `uvcpp_c_db_table_*` | `uvcpp_db_table` 的读侧 + `to_json()` / `to_csv()` |
| `uvcpp_c_db_value_*` | `uvcpp_db_value` 的读侧（写侧走参数）|
| `uvcpp_c_db_pool_*` | `uvcpp_db_pool`（借还 + 代借代还 + 观测）|
| `uvcpp_c_db_async_*` | `uvcpp_db_async`，以及池子那一组异步 |

三个开关与这一片的关系，一句话：**`UVCPP_ENABLE_DB` 是它的开关**（关了就没有
`uvcpp_c_db.h`，一个函数都不导出）；**后端那一级开关与它无关** —— 关掉 MySQL 只是让
`uvcpp_c_db_drivers()` 不再报出那个名字、让 `mysql://…` 报 `NO_DRIVER`，符号面一个
不多一个不少。C 面的完整清单与所有权规则在
[`doc/capi-guide.md` §4](capi-guide.md#4-提供什么明确不提供什么) 那张表里。

C 这一侧有四条与 C++ 不同、**必须**知道的：

1. **一个 `int` 上住着两套编码。** `>= 0` 是 `uvcpp_c_db_status`（与
   [§4](#4-返回码)那张表**数值逐条对齐**，0 = OK）；`< 0` 是 C 层错误
   （`UVCPP_C_E_STALE` 句柄失效 / `_E_INVALID_ARG` / `_E_EXCEPTION`）或者 libuv 码
   原样穿过。两段不会撞：db 状态码全在 `0..11`。
2. **结果集是调用方拥有的句柄。** `_query` / `_table_schema` 交出来的、以及异步回调
   收到的那个 `uvcpp_c_db_table*`，用完都要 `uvcpp_c_db_table_free()`；而
   `_cell()` 给出的 `uvcpp_c_db_value*` 是**借来的视图**（属于那张表，没有 `free`）。
   池子借出的 `uvcpp_c_db_client*` 同理归池子 —— 对它调 `_client_free()` 是 `E_STATE`。
3. **C 的异步没有循环参数。** C++ 的 `uvcpp_db_async::query()` 收一枚 `uvcpp_loop*`，
   而 C 面没有合法的东西可填（公开头里造不出 `uvcpp_loop*`；把调用方自己的
   `uv_loop_t*` 借进来会让 `~uvcpp_loop()` 去 `uv_loop_close()` 并释放别人的内存）。
   所以 `uvcpp_c_db_async_*` 的完成回调一律在**门面自带的那条懒起的循环线程**上被调；
   要回到自己的循环，就在回调里投一次 `uvcpp_c_net.h` 的 `*_post()`。
4. **不提供 `set_log()`。** C++ 那侧收 `std::function`，跨 FFI 每行日志都要另立一份
   字符串生命周期约定；诊断走 `_client_last_error()` 与 `uvcpp_c_db_status_name()`。

真跑的判据是 `tests/capi/capi_db_func.c`（纯 C 编译）：把**后端从环境变量**里拿
（`UVCPP_DB_TEST_PGSQL_URL` / `UVCPP_DB_TEST_MYSQL_URL`），有哪个就连哪个，逐后端把
建表 / 插入 / 查询 / 事务 / 池 / 异步各跑一遍；一个后端都没有时退 3（**未判定**，
`tests/functional/` 那几条 db 用例是同一个约定）。本机 `build-capi-all` 上 296 条断言，
三个后端都给连接串时 792 条。

---

## 4. 返回码

| 返回码 | 含义 |
|---|---|
| `OK` | 成功 |
| `BAD_URL` | 连接串解析不了 |
| `NO_DRIVER` | 这个后端的驱动没编进这份库 |
| `OPEN_FAILED` | 主机/端口不可达、认证失败、库不存在、超时 |
| `NOT_CONNECTED` | 没开就调，或执行途中连接掉了 |
| `NO_CONNECTION` | **连接池借不出**：已到 `max` 且全在外借，等到借出超时也没等到 |
| `PREPARE_FAILED` | 语句准备失败：**SQL 语法错、表/列不存在** |
| `EXEC_FAILED` | 语句执行失败：约束冲突、类型不匹配、死锁重试用尽、权限不足 |
| `BIND_FAILED` | 参数与占位符对不上（**个数**、或名字找不到） |
| `UNSUPPORTED` | 这个后端不支持该操作（SQLite 没有存储过程、没有 `lastval()`） |
| `MISUSE` | 用法错误：空 SQL、空结果集指针 |
| `OUT_OF_MEMORY` | 内存不足 |

返回码只区分**哪一类失败**，不枚举各家数据库自己的错误号 —— 那属于驱动的私有
词汇表。要 SQLSTATE / errno 时看 `last_error()` 里的原文（它里面就有）。

**`PREPARE_FAILED` 与 `EXEC_FAILED` 的分界线是「服务端有没有接受这条语句」**，
不是「哪一步失败」：语法错、表/列不存在属于前者；唯一键冲突、除零、死锁属于后者。
这条线在三家上**都成立**，包括**裸通道**（MySQL 的文本协议、PostgreSQL 的
`PQexec`）—— 那两条路是服务端一步做完"解析 + 执行"，两种错从同一个出口回来，
驱动按错误号/`SQLSTATE` 分开（判据见 [§9](#9-跨后端共用套件抓到过的四处)）。
为什么值得分：调用方的下一步动作不同 —— `PREPARE_FAILED` 改 SQL，`EXEC_FAILED`
可能是重试（死锁、锁等待）或改数据。

**`NO_CONNECTION` 与 `NOT_CONNECTED` 必须分开**：后者说的是"这条连接没打开"，拿它顶
前者就是撒谎 —— 池子里每条连接都是好的，只是**没有空闲的**。两者的处置办法也完全
不同：前者该重连，后者该扩容（或把借出的还回来）。这个码只有 `uvcpp_db_pool` 会产生
（见 [§3.1](#31-连接池)）。

---

## 5. 参数绑定与转义

**值一律走参数，SQL 文本里永远不拼接外部输入。** 但**占位符是方言的一部分** ——
本模块把 SQL **原样**交给驱动，**不换算**：

| 后端 | 占位符 | 第 n 个参数 |
|---|---|---|
| SQLite | `?` | 按出现次序 |
| MySQL | `?` | 按出现次序 |
| PostgreSQL | `$1`…`$n` | `$1` = 参数表第 0 个 |

```cpp
#include <db/uvcpp_db.h>

uvcpp::uvcpp_db_status find_by_id(uvcpp::uvcpp_db_client& db,
                                  uvcpp::uvcpp_db_table& t) {
  // SQLite / MySQL：
  return db.query("SELECT * FROM users WHERE id = ? AND name = ?", {1, "abc"}, &t);
  // PostgreSQL 上同一条要写成 "... WHERE id = $1 AND name = $2"
}
```

在 PostgreSQL 与 MySQL 上，拿错写法不会静默走偏 —— 它在**准备阶段**就报出来
（本机对着真服务端量过，六种组合一个不漏）：

| 连接 | 写法 | 结果 |
|---|---|---|
| PostgreSQL | `?` | `PREPARE_FAILED` —— `syntax error at end of input` |
| PostgreSQL | `$1` | ok |
| MySQL | `?` | ok |
| MySQL | `$1` | `PREPARE_FAILED` —— `Unknown column '$1' in 'where clause'` |
| SQLite | `?` | ok |
| SQLite | `$1` | **ok** —— 这里有个坑，见下 |

**SQLite 把 `$1` 当命名参数收下**（能跑通），所以**只测 SQLite 永远发现不了写法
错**：同一段 SQL 在 PG 上必红。跨后端的代码请走共用套件的 `dialect::ph(n)`，别
自己写死 —— 那是方言必须暴露的差异，也是
[`tests/functional/db_suite.h`](../tests/functional/db_suite.h) 里的
`dialect::ph()` 与 `test_placeholder_dialect()` 钉住的。

**为什么驱动不做换算**：PostgreSQL 里 `?` 本身就是合法的**操作符**（jsonb / hstore
的"键存在"），`?` 也能出现在字符串字面量和带引号的标识符里 —— 盲扫一遍会把
**合法**的 SQL 悄悄改成语义不同的另一句。反方向同样不通：SQLite 收下 `$1`（当命名
参数，语义与位置参数不同），所以没有哪个统一形式能安全地翻译成两家。宁可让写错
的人**当场**拿到 `PREPARE_FAILED`，也不替他猜。

* `uvcpp_db_params` 从 `{1, "a", 2.5}` 这种初始化列表构造，元素类型由
  `uvcpp_db_value` 的构造函数定；`add()` / `add_null()` 是追加式写法。
* 个数对不上 → `BIND_FAILED`（**三家一致**，且都**不会**执行语句）。
* 字符串默认是 `TEXT`，要按字节存（图片、加密后的 payload）用
  `uvcpp_db_value::blob(p, n)` —— 它按数据长度走，含 `0x00` 也不会被截断。

**转义只在拼标识符时才需要**，而且它是"加引号"，不是"消毒"：

```cpp
#include <string>

#include <db/uvcpp_db.h>

std::string quote_table(uvcpp::uvcpp_db_client& db,
                        const std::string& user_supplied) {
  const std::string tbl = db.escape_identifier(user_supplied);
  // 表名来自外部输入时，仍然要对着白名单查一遍再用
  return tbl;
}
```

`escape()` 转义的是**字面量内容**（不含两侧引号），MySQL 用反斜杠、SQLite/PG
用双写单引号 —— 期望值各家不同，所以用例判的是**往返相等**：
把一段装着引号、反斜杠、分号与 `--` 的文本存进去，再原样读回来。

> 顺带一条与安全有关的实事：MySQL 连接开着 `CLIENT_MULTI_STATEMENTS`，
> 一次 `execute()` 里可以放多条以分号隔开的语句。那是便利也是风险 ——
> 拼接 SQL 就等于把第二条语句的权力交给数据。**能参数化就参数化。**
> 另外，含 `?` 的语句走的是预处理通道，而预处理协议**不接受**多语句：
> 想用多语句就别用占位符。

---

## 6. 三个后端：钉住的一致，与如实列出的三处不同

> **这一节的标题说的是 SQLite / MySQL / PostgreSQL 三家**，ODBC 单独放在紧接着的
> 「ODBC 换了一列」里。ODBC **已经进了同一个共用套件**
> （`tests/functional/db_odbc_func.cpp`；方言按 `Driver=` 认出来的驱动族挑，读数见
> 下面表格的最后一行），但它**跑过的那条路是 ODBC 到 SQLite**：SQL Server 那一族
> 仍然是**没跑**的（见 [§11](#11-没做的如实列出)）。读到"三个后端"、"三家"时，
> ODBC 不在里面。

### 钉住的一致（共用套件 `tests/functional/db_suite.h`，三家各跑一遍）

本机实测的读数（数字各不相同是**设计如此**，见下面"三处真不一样"）：

| 后端 | 通过 | 不适用 | 不适用的那几行 |
|---|---|---|---|
| SQLite | 196 | 1 | `原生时间类型标签`（它没有原生时间类型） |
| ODBC（到 SQLite） | 181 | 3 | `原生时间类型标签`、`last_insert_id`、`外键约束` |
| MySQL | 159 | 1 | ——（沿用旧读数） |
| PostgreSQL | 159 | 1 | 同上 |

ODBC 那一行之外还有**另一组 ODBC 专属用例**（日志里单独一段，`-- ODBC 专属 --`，
57 通过 / 0 失败）：共用套件判"三家一致"的那些契约，ODBC 专属那一组判它自己的坑。

口径写清楚：读数是**这台机器、这份构建**（SQLite 与 ODBC 编进来，MySQL /
PostgreSQL 没编）跑出来的；SQLite 那一行在 ODBC 关掉重编重跑时同样是 196 / 1，
所以它不随"编不编 ODBC"变。MySQL / PostgreSQL 两行**是上一轮的读数**，本机没有
可跑的 MySQL/PG 服务端，这一轮只重测了 SQLite 与 ODBC。SQLite 那一行同样和更早的
文档对不上（旧数字是 157 / 2，本树上复现不出来）—— 复现不出来的旧读数不留着当
"约等于"，直接换成现测数字。

"不适用"是**独立的一格计数器**，与"通过"分开打印 —— 跳过永远不会在日志里
伪装成通过。

套件钉住的是：

* 同一条 DDL/DML/SELECT 在三个后端上得到**同一份结果**：列名、列序、行数、
  值的文本形态逐字节相等；
* 空串**不是** NULL，NULL **不是**空串（DDL 里带 `NOT NULL DEFAULT ''`，
  把空串折成 NULL 的实现会撞在非空约束上，失败得响亮）；
* 长值（32 KB 文本、含 `0x00`/`0xFF` 的 BLOB）往返逐字节相等；
* 参数个数对不上 → `BIND_FAILED`；语法错/表不存在 → `PREPARE_FAILED`；
* 事务：`begin` 之后 `rollback`，写进去的行**不在**；`commit` 之后在；
* 外键约束真的生效（SQLite 在 `open()` 里执行了 `PRAGMA foreign_keys = ON`
  —— 它**默认是关的**，不显式打开的话外键是画上去的）；
* `table_names()` / `table_schema()` 能报出刚建的表与列。

共用套件的形状是「一个 `dialect` 虚基类 + 一个 `harness`」：不同后端只能**提供
方言**（占位符写法、自增主键的列声明、定点的列声明），**不能**分叉断言。
任何 `if (backend == ...)` 都被视为"差异还没收敛"，不接受。

### 三处**真**不一样（如实列出）

| 项 | SQLite | MySQL | PostgreSQL |
|---|---|---|---|
| 布尔 | 没有布尔类型，`INTEGER` 的 0/1 | `TINYINT(1)`，读回来是**整数** | 有真 `BOOL`，读回来是 `BOOL` |
| 日期时间 | 没有原生类型，存**文本** | 原生 `DATETIME`/`DATE`/`TIME` | 原生 `timestamp`/`date`/`time` |
| 定点（钱） | 没有，只能 `TEXT` 存字符串 | 原生 `DECIMAL`，读成 `double` | 原生 `NUMERIC`，读成 `double` |

所以套件里判**值**（`to_bool()`、`to_text()`）而不是判**类型标签** ——
判类型标签的断言在 MySQL/SQLite 上必然红，而那红的不是被测代码。

日期时间一律以**文本**形态保存（`"YYYY-MM-DD"`、`"HH:MM:SS"`、
`"YYYY-MM-DD HH:MM:SS"`）：这是三家都能无损往返、且人眼直接可读的表示。
SQLite 本来就没有日期类型，PostgreSQL 的文本格式恰好就是这个形状。

### ODBC 换了一列（差别在**驱动**里，不在后端里）

前三家的差别在**库里**；ODBC 的差别在**驱动**里。同一个 `odbc` 后端，`Driver=` 指的
是 SQLite 的 ODBC 驱动还是 SQL Server 的，占位符写法、自增主键声明、定点列声明就跟着
变 —— 这也是它必须有两个 `dialect` 实现（`odbc_sqlite_dialect` / `odbc_mssql_dialect`）
的原因：断开的是"后端"与"方言"的绑定。共用套件里"不能分叉断言"这条规矩对它同样成立，
多出来的自由度**只**体现在方言上。

| 项 | ODBC 到 SQLite（sqliteodbc，跑过） | ODBC 到 SQL Server（**没跑**） |
|---|---|---|
| `$1` 占位符 | **不接受**（`parameter marker count incorrect`） | —— |
| 语法错 / 表不存在 | 报 `EXEC_FAILED`：`SQLPrepare` 只**登记**，不解析 SQL | —— |
| 自增主键 | `INTEGER PRIMARY KEY AUTOINCREMENT` | —— |
| 定点（钱） | 没有，`TEXT` 存字符串 | —— |
| 外键 | 连接里不开 `PRAGMA foreign_keys` → **不**强制 | —— |
| `last_insert_id()` | 没有这条路 → 不适用 | —— |

右列的破折号**不是"待填"**，是"没跑" —— 那台机器上没装 msodbcsql18，右列的列声明
只是写在 `odbc_mssql_dialect` 里等着跑，别当成已经对过。

第二行那条是 `dialect::prepares_eagerly()` 这个钩子存在的理由：`SQLPrepare` 是当下
解析 SQL、还是只把语句登记下来等 `SQLExecute` 才报错，是驱动的自由。套件不假定它，
只按声明的能力判返回码 —— 说"预处理阶段失败"的地方在这类驱动上报的是 `EXEC_FAILED`，
而那同样是真的失败，只是**晚一步**。

### 后端各自怎么读结果（想看清"为什么某条断言长这样"时看这里）

* **SQLite**：`sqlite3_column_*` 按**运行期**类型取，与列的声明类型无关
  （SQLite 是动态类型，声明只是"亲和性"）。整型超出 `INT64_MAX` 时驱动**拒绝
  绑定**（`BIND_FAILED`），不截断成负数存进去 —— 用例里有这条见证。
* **MySQL**：两条通道。非空参数走预处理协议（二进制），空参数走文本协议
  （`mysql_query`）—— 后者不只是"少一次 prepare"：旧服务端的预处理协议**拒绝**
  DDL。结果读回来时，**`TEXT` 与 `BLOB` 是同一个类型标签**，分它们的是字符集
  （见 [§9](#9-跨后端共用套件抓到过的四处)）。
* **PostgreSQL**：`libpq` 全部以**文本**形态收发，参数也按文本传
  （`PQexecParams` 的 `paramFormats` 全 0），类型让服务端从上下文推断。
  `BYTEA` 走文本形态时必须**十六进制编码**成 `\x...`（`open()` 里设了
  `bytea_output = 'hex'`），读回来再解码 —— 两头都不做的话，`blob("a\0b")`
  会以原始文本发出去，服务端报 `invalid input syntax for type bytea`，
  而读回来的是一个 10 字符长的假 blob，**不报任何错**。

---

## 7. 事务，以及"事务里不重试"

```cpp
#include <db/uvcpp_db.h>

uvcpp::uvcpp_db_status transfer(uvcpp::uvcpp_db_client& db) {
  uvcpp::uvcpp_db_status st = db.begin();
  if (uvcpp::uvcpp_db_ok(st)) {
    st = db.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", {100, 1});
  }
  if (uvcpp::uvcpp_db_ok(st)) {
    st = db.execute("UPDATE accounts SET balance = balance + ? WHERE id = 2", {100});
  }
  if (!uvcpp::uvcpp_db_ok(st)) {
    db.rollback();     // 出错就回滚
    return st;
  }
  return db.commit();
}
```

* 三家都是同一个形状，**不嵌套**：重复 `begin()` 由后端报错（不在这里拦，
  因为 MySQL 会静默提交前一个 —— 拦了反而给出一种它做不到的保证）。
* `reconnect()` 在事务里调 = **放弃那个事务**。
* **MySQL 的死锁/锁等待超时在事务里不重试。** InnoDB 报 `ER_LOCK_DEADLOCK`
  （1213）时会**回滚整个事务** —— 那时重放这一条语句只会写进一个"已经不在事务
  里"的连接，调用方以为还在交易中，实际每条语句都在自动提交。所以那种情况下
  驱动把错误交回调用方，由它决定要不要**重做整笔交易**。事务外（单条语句）才
  重试，退避 50/100/200 ms，共 3 次。这条规矩写在驱动里，也有注释说明为什么。

---

## 8. 类型与精度：`DECIMAL` 的钱会丢

| 本模块 | MySQL | SQLite | PostgreSQL |
|---|---|---|---|
| `NIL` | NULL | NULL | NULL |
| `INT64` / `UINT64` | BIGINT 及以下整型 | INTEGER | int2/int4/int8 |
| `DOUBLE` | FLOAT / DOUBLE / **DECIMAL** | REAL | float4 / float8 / **numeric** |
| `BOOL` | TINYINT(1) / BOOL | INTEGER(0/1) | bool |
| `TEXT` | CHAR / VARCHAR / TEXT | TEXT | text / varchar / char |
| `BLOB` | BLOB | BLOB | bytea |
| `DATE` / `TIME` / `DATETIME` | DATE / TIME / DATETIME | —（文本） | date / time / timestamp |

**`DECIMAL` / `NUMERIC` 读进来是 `double`** —— 金额在那一步就丢精度了
（`0.1 + 0.2` 那件事）。本模块没有定点类型，所以：

> **要精确的钱就用 `TEXT` 存字符串**（或存最小货币单位的整数）。
> 三个后端都支持这条路，而且 SQLite 上它本来就是唯一的路。

这条不是"设计缺陷说明"，是**取值建议**：反过来做（把 `DECIMAL` 读成 double
再算账）在测试里也不会红，只会在对账时差一分钱。

---

## 9. 跨后端共用套件抓到过的四处

这一节是"共用套件为什么值这个钱"的账本。四条都是**共用套件在真服务端上抓出来
的**，不是设计时想到的；抓出来之前，每一条都是"看着没事"。

1. **`TEXT` 列读回来是个 blob。** MySQL 里 `TEXT` 与 `BLOB` 在协议上是**同一个
   类型标签**（服务端两个都报 `MYSQL_TYPE_BLOB` = 252），唯一的区别是
   `charsetnr`：`TEXT` 报列自己的排序规则（utf8mb4 上是 255），`BLOB` 报 63。
   驱动早先按类型标签一刀切，于是 `s_val TEXT` 读出来是 blob、`to_json()` 打出
   `[blob 5 bytes]`，而**不报任何错**。本机实测：

   ```
   s_val TEXT          -> type=252 charsetnr=255
   b_val BLOB          -> type=252 charsetnr=63
   v_val VARBINARY(32) -> type=253 charsetnr=63
   ```

   顺带一条反面教训：判据也**不能**写成"`charsetnr == 63` 就是字节" ——
   `DECIMAL` / `INT` 这些**数值**列的 `charsetnr` 同样是 63，那样一刀切的话
   金额列会以 blob 抛出来，而 `to_double()` 得到 0。两层判据缺一不可。

2. **多给参数被静默忽略。** `mysql_stmt_bind_param` 按服务端报的
   `param_count` 读绑定数组，多给的元素**直接不看** —— `WHERE id = ?` 配两个
   参数照常执行、照常返回 `OK`。套件里那条"多给参数 → `BIND_FAILED`"当初拿到
   的是 `ok`，于是补上 `mysql_stmt_param_count` 的显式校验。

3. **少给参数被当成语法错。** `WHERE id = ?` 配 0 个参数时，驱动会走"空参数 =
   裸通道"那条路，服务端把 `?` 当语法错报回来（1064）—— 于是"参数个数不对"
   表现成"SQL 写错了"。现在的判据是：**SQL 里出现 `?` 就走预处理通道**，
   让 `param_count` 做权威判断（字面量里的 `?` 也走那条路，代价只是这一次用
   二进制协议，结果一样）。

4. **`PREPARE_FAILED` 在裸通道上丢过。** 上面两条修完之后还剩一条：语法错与
   表不存在在 MySQL/PG 的裸通道上都报 `EXEC_FAILED`，而 SQLite 报
   `PREPARE_FAILED`。裸通道是服务端一步做完"解析 + 执行"，两种错从同一个出口
   回来，所以驱动得按错误号分开：MySQL 看 `ER_PARSE_ERROR`(1064) /
   `ER_NO_SUCH_TABLE`(1146) / `ER_BAD_FIELD_ERROR`(1054) 等，PostgreSQL 看
   `SQLSTATE` 的 **42 类**（`syntax_error_or_access_rule_violation`）——
   两个例外是 `42P02`（`$1` 没给参数）与 `08P01`（Bind 参数个数对不上），
   它们归 `BIND_FAILED`，不是 `PREPARE_FAILED`。

还有一条不是"抓到的"、但值得记：**PostgreSQL 的 `BYTEA` 两头都要编解码**
（见 [§6](#6-三个后端钉住的一致与如实列出的三处不同)），早先是"写进去报错、
读回来不报错但内容错"——后者更坏。

再一条**反向**的教训，关于"套件绿了"能推出什么：**SQLite 收下 `$1`**（当命名
参数），所以一段把 `?` 写死、又在 SQLite 上跑绿了的 SQL，拿到 PostgreSQL 上是
`PREPARE_FAILED`。共用套件本身用 `dialect::ph()` 取占位符，**永远不会**写出错
的那种写法 —— 也就是说这一条不是"套件抓到的"，是**套件覆盖不到**的：想过这一关
得专门写一条"拿错写法必须失败"的用例（`db_suite.h` 的 `test_placeholder_dialect`，
三家各跑一遍）。文档里那句"驱动会自己换算"活了很久没被发现，正是这个盲区。

---

## 10. 本地怎么跑

四条用例在 `tests/functional/`：`db_sqlite_func.cpp`（**不需要服务端**，永远真
跑）、`db_mysql_func.cpp`、`db_pgsql_func.cpp`、`db_odbc_func.cpp`。共用套件在
`db_suite.h`。

**ODBC 那一条要一个真的数据源**（服务器、或一个本地文件库的驱动），所以它排在第
四：给不出 URL 时退 3（未判定），而不是拿一份默认方言硬跑。它的**方言按 `Driver=`
挑** —— ODBC 的方言属于数据源、不属于这个后端，认不出就退 3：

```bash
cmake -S . -B build-db3 -DUVCPP_BUILD_TESTS=ON -DUVCPP_ENABLE_DB=ON
cmake --build build-db3 -j"$(nproc)"

# SQLite：什么都不用给
./build-db3/tests/functional/test_db_sqlite_func

# MySQL / PostgreSQL：给一个**一次性**的库的连接串
export UVCPP_DB_TEST_MYSQL_URL='mysql://root@127.0.0.1:3306/uvcpp_test'
export UVCPP_DB_TEST_PGSQL_URL='postgresql://uvcpp@127.0.0.1:5432/uvcpp_test'
./build-db3/tests/functional/test_db_mysql_func
./build-db3/tests/functional/test_db_pgsql_func

# ODBC：SQL Server（本后端存在的理由）
export UVCPP_DB_TEST_ODBC_URL='odbc://sa:pw@127.0.0.1:1433/uvcpp_test?Driver={ODBC Driver 18 for SQL Server}'
./build-db3/tests/functional/test_db_odbc_func

# ODBC：也可以走 sqliteodbc —— 不用服务端，一条命令就能把这条用例跑起来
# （Ubuntu：apt-get install unixodbc libsqliteodbc，驱动落在
#  /usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so）
export UVCPP_DB_TEST_ODBC_URL='odbc:?Driver=/usr/lib/x86_64-linux-gnu/odbc/libsqlite3odbc.so;Database=/tmp/uvcpp_odbc.db'
./build-db3/tests/functional/test_db_odbc_func
```

**套件里有 `DROP TABLE` / `CREATE TABLE`，URL 必须指向一个一次性的库。**
用例自己的表名都是 `uvcpp_db_*` 前缀，但别拿生产库试。

| 环境变量 | 作用 |
|---|---|
| `UVCPP_DB_TEST_MYSQL_URL` | MySQL 用例的连接串。没有它 → 退出码 **3**（未判定） |
| `UVCPP_DB_TEST_PGSQL_URL` | PostgreSQL 用例的连接串，同上 |
| `UVCPP_DB_TEST_ODBC_URL` | ODBC 用例的连接串，同上（认不出 `Driver=` 是哪一族也退 3） |
| `UVCPP_DB_TEST_REQUIRE` | 设了它，缺 URL 就是**失败**（退出码 1），不是跳过 |

**ODBC 这一条不在 CI 里**：那几台 runner 上没有任何 ODBC 数据源，装了驱动管理器也
只够跑"驱动名不存在时报 `OPEN_FAILED`"（那是它自己的一条断言），跑不了共用套件。
所以它的读数只能在本地产出 —— 见 [§11](#11-没做的如实列出) 里哪些还真没有。

退出码 3 是照本仓 gate 脚本的规矩来的：**「未判定」不是「通过」**。ctest 那边用
`SKIP_RETURN_CODE 3` 把它报成 `***Skipped` —— 与 `Passed` 在日志里是两个词。
CI 上服务容器都起了还缺 URL，说明是配置写错了，所以 CI 里额外设
`UVCPP_DB_TEST_REQUIRE=1`，那时跳过就变成红。

不设 `UVCPP_DB_TEST_REQUIRE` 时，本地没起 MySQL 也能得到一句诚实的
`***Skipped`，而不是一条假绿。

**CI 上这四条腿摆在哪**：Ubuntu 与 macOS 各一条 `db` 矩阵格、MSVC 一条、MinGW 那条腿
（两次 configure 都开），四者都只跑 SQLite —— 那三个 runner 上没有数据库服务端，而
Windows 上走的是 `UVCPP_DB_SQLITE_FROM_SOURCE`（MSYS2 若装了
`mingw-w64-x86_64-sqlite3`，`find_library` 会挑中 `libsqlite3.dll.a`，dll 就多一个
`sqlite3.dll` 依赖）。**远程后端另有一个 job**：Ubuntu 上单独的 `db-servers`，用两个
`services:` 容器跑真 MySQL 8.0 与 PostgreSQL 16 —— `services:` 是 **job 级**的，塞进矩阵
会把两个容器拖进全部十一格。那三格把两个远程后端**显式关掉**（不关也只是两条 `***Skipped`，
买不到覆盖），`db-servers` 反过来把 SQLite 显式关掉：**每一边都有一条反向断言**，
少了它，"开关被改坏了"是一条全绿的路。逐格的四道门禁与两个 service 容器的配置见
[`ci-guide.md`](ci-guide.md) §5。

---

## 11. 没做的（如实列出）

* **ODBC 到 SQL Server 本身。** 这个后端进了共用套件（`db_odbc_func.cpp`，
  `UVCPP_DB_TEST_ODBC_URL`），但**跑过的那条路是 ODBC 到 SQLite**（sqliteodbc，
  Ubuntu，驱动管理器 unixODBC；读数 181 通过 / 0 失败 / 3 不适用 + 57 条 ODBC 专属
  断言）。SQL Server 那一族的方言是**照语法写的、没跑过**：`msodbcsql18`、Windows、
  DSN 形式的连接串、`SQL Server` 的 `IDENTITY` / `DATETIME2` / `BIT` 逐条落到读数
  上都还没有。要跑真 SQL Server 时，红的第一条先怀疑那份方言（`odbc_mssql_dialect`）
  的写法，而不是后端本身。**在那之前，别把"ODBC 到 SQL Server"当成与另外三家同等
  "验证过"的后端。**
* **ODBC 的 `last_insert_id()`（除了 SQL Server 那一族）。** 它发的就是
  `SCOPE_IDENTITY()`，别的数据源不认，如实返回 `UNSUPPORTED`（实测：sqliteodbc 上
  报 `UNSUPPORTED` 并留下原因，不是编一个 0 出来）。别的数据源要报自增值，得先有
  一条按数据源分派的写法。
* **协程（`co_await` / C++20 那套）。** 异步门面给的是**回调 + future**两种形状
  （见 [§3](#3-公开面)），本库是 C++11 的，不引入协程。
* **门面的背压。** 不限制在途条数 —— 一个连接本来就是串行的，排队等锁不会更快。
  池子那边也一样：它在途条数的上界就是 `max`（借不出来就等），没有额外的闸。
* **池子的后台回收。** 不起线程、不挂定时器（见 [§3.1](#31-连接池)）：不调
  `acquire()` / `release()` / `close_idle()`，池子就不会缩。这是有意的取舍 ——
  池子要保持事件循环无关，多一条线程去扫空闲表不值得。
* **独立的数据库线程池。** 门面复用的是 libuv 那条默认池（4 条线程，与
  `uv_fs_*` / DNS 共用）。慢查询会把文件 IO 挤掉，要缓解只能调
  `uvcpp_set_threadpool_size()`。
* **ORM 那套东西。** 没有模型、没有迁移、没有关系映射、没有查询构造器 ——
  它是"能安全地执行 SQL 并拿到表"，不是 ActiveRecord。命名上叫 `db` 而不是
  `orm`，就是因为后面那半句不打算假装。
* **准备语句缓存 / 语句复用。** 每次 `query`/`execute` 都是新的
  `mysql_stmt_prepare` / `PQexecParams`。省掉它换来的是"状态不跨语句泄漏"，
  这个取舍是有意的；要批量插入时请用事务包住循环。
* **`DECIMAL` 的定点类型**（见 [§8](#8-类型与精度decimal-的钱会丢)）。
* **SQLite 上的日期时间语义。** 它是文本，`date()` / `datetime()` 这些函数能用，
  但本模块不替你做时区换算 —— 存进去什么字符串，取出来就是什么字符串。
* **`UINT64` 超过 `INT64_MAX` 的往返**：MySQL 有 unsigned 绑定、SQLite 会拒绝、
  PostgreSQL 的 `int8` 是有符号的。跨后端要存那么大的数请用 `TEXT`。
