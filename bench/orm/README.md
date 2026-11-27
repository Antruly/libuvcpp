# 数据库 / ORM 基准：基线与方法

本目录是 issue [#95](https://github.com/Antruly/libuvcpp/issues/95)（父任务
[#88](https://github.com/Antruly/libuvcpp/issues/88)）的两套量具与方法学：

* **db 模块基线**：`bench/bench_db.cpp` + `run_db_baseline.sh`（第 1–7 节）；
* **ORM 端到端 / 纯开销**：`bench/orm/bench_orm.cpp` +
  `run_orm_overhead.sh`（第 8 节），只针对 SQLite。

> ORM 的模型、SQL 构造、CRUD 与事务/异步已在 #89/#90/#91/#92 落地，因此
> **本页现在有 ORM 读数**。第 6 节的 db 基线数字和第 8 节的 ORM 数字是两套
> 判据，不能混用；第 8 节的本机读数也**不是**发布门禁结论，原因见 8.7/8.8。

---

## 1. 口径

基线记录使用下面这一组固定条件。换机器、换编译器、换 SQLite 版本、换连接数时，
**不能**把新数字直接和本页比较。

| 项 | 值 |
|---|---|
| 构建 | `CMAKE_BUILD_TYPE=Release`（`-O3 -DNDEBUG`） |
| DB 开关 | `UVCPP_ENABLE_DB=ON`，只编 SQLite 后端 |
| 量具 | `bench/bench_db.cpp`（**未改测量语义**） |
| 数据库 | SQLite 3.45.1，系统 `libsqlite3.so.0` |
| 种子行数 | 10000（`--rows=10000`） |
| 轮转 | 5 轮（脚本默认 3 轮、允许 `--reps 1` 冒烟；基线至少跑 3 轮） |
| 连接串 | 每轮一个结果目录里的 `sqlite://<run-dir>/bench.sqlite` |
| 指标来源 | 每轮原始日志 + GNU time 进程账 + 脚本聚合的 JSON/TSV |

ORM 基准有自己的一组条件（ORM 开关、样本数、raw 臂的 SQL 复用方式等），
见 8.1；**不要把第 6 节的 db 基线条件套到第 8 节上**。

`bench_db.cpp` 里的用例、循环次数、事务边界、卡顿臂、并发臂全部保持原样。
脚本只做驱动、采样、落盘和聚合，不改任何测量语义。

### 这条基线不是线上延迟

SQLite 是进程内后端，没有网络往返。本页的 µs 是「SQLite + 本库封装 + 本机」的
成本下界，不是“线上一次查询多久”。对“同步调用卡循环多久”那条结论，网络越慢
只会让卡顿更严重，因此这里的卡顿读数是偏乐观的下界。

---

## 2. 构建

从仓库根执行（本目录的脚本不替你配置或编译）：

```bash
cmake -S . -B build-db-bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ENABLE_DB=ON \
  -DUVCPP_ENABLE_DB_SQLITE=ON \
  -DUVCPP_ENABLE_DB_MYSQL=OFF \
  -DUVCPP_ENABLE_DB_PGSQL=OFF \
  -DUVCPP_ENABLE_DB_ODBC=OFF \
  -DUVCPP_ENABLE_DB_MONGODB=OFF \
  -DUVCPP_BUILD_BENCH=ON \
  -DUVCPP_BUILD_TESTS=OFF \
  -DUVCPP_BUILD_WEB=OFF \
  -DUVCPP_BUILD_WEBAPP=OFF

cmake --build build-db-bench -j"$(nproc)"
```

产出：`build-db-bench/bench/uvcpp_bench_db`。

没有 Ninja 时去掉 `-G Ninja`。如果机器上没有系统 SQLite 开发包，保留
`-DUVCPP_ENABLE_DB_SQLITE=ON`，再加
`-DUVCPP_DB_SQLITE_FROM_SOURCE=ON`（改用仓内钉死哈希的源码 amalgamation，
见根 `CMakeLists.txt` 的 DB 发现段）。

---

## 3. 跑

最短命令（默认构建目录 `build-db-bench`、3 轮、10000 行、结果写到
`bench/orm/results/`）：

```bash
bench/orm/run_db_baseline.sh
```

基线记录用的 5 轮：

```bash
bench/orm/run_db_baseline.sh \
  --build-dir build-db-bench \
  --reps 5 \
  --rows 10000 \
  --label sqlite-baseline \
  --out bench/orm/results
```

指定连接串：

```bash
bench/orm/run_db_baseline.sh --url 'sqlite:///tmp/uvcpp-bench.sqlite'
```

脚本也认 `UVCPP_DB_BENCH_URL`（与量具同一条环境变量口径）：

```bash
UVCPP_DB_BENCH_URL='sqlite:///tmp/uvcpp-bench.sqlite' \
  bench/orm/run_db_baseline.sh --reps 5
```

`--help` 是完整参数表。相对路径按**当前工作目录**解析；脚本只找
`<build-dir>/bench/uvcpp_bench_db`。

### 失败时必返回非 0

```bash
bench/orm/run_db_baseline.sh --url 'bogus://not-a-db' \
  --reps 1 --out /tmp/uvcpp-db-baseline-fail
echo $?    # 1
```

SQLite 会自己建父目录，所以“路径不存在”不是失败；要触发失败用一个没有对应
后端的 scheme（如上）或一个连不上的服务端 URL。失败结果目录里仍保留
`run-01.err`、`run-01.log`、`runs.tsv`、`meta.*` 和说明失败的 `summary.txt`。

### 每次调用不覆盖已有结果

`--out` 是**根目录**。每次调用在里面新建
`db-baseline-<UTC 时间戳>[-NN]/`；创建用原子的 `mkdir`，同一秒的并发调用也会拿到
不同后缀。连续两次执行同一条命令会得到两个目录，旧目录原样保留。

---

## 4. 结果目录里有什么

```text
bench/orm/results/db-baseline-<UTC>/
  meta.txt / meta.tsv          机器、编译器、CMake、SQLite、git、命令
  build-config.txt             从 CMakeCache.txt 抠出的相关开关
  run-01.log .. run-NN.log     每轮量具 stdout（原始）
  run-01.err .. run-NN.err     每轮 stderr（原始；干净时为空）
  run-01.time.txt ..           每轮 GNU time -v 报告
  runs.tsv                     每轮 exit / user / sys / wall / maxRSS
  parsed.tsv                   四类记录的合并中间表（C/B/S/T）
  cases.tsv / cases.csv        单次调用逐轮明细
  cases.agg.tsv / summary.csv  单次调用跨轮聚合
  bulk.tsv / bulk.agg.tsv      整批操作（事务内 INSERT、大结果集）
  stalls.tsv / stalls.agg.tsv  事件循环卡顿三臂
  concurrency.tsv / .agg.tsv   并发三臂
  summary.txt                  人读汇总
  summary.json                 机读汇总（schema = uvcpp-db-baseline/1）
  bench.sqlite                 本轮量具用的 SQLite 文件（每轮 DROP/CREATE 表）
```

`bench/orm/results/` 已加入 `.gitignore`：原始日志和 SQLite 文件是本机产物，
不进仓库。需要引用时提交 `summary.txt` / `summary.json` 或把关键表抄进报告，
并同时给出 `meta.txt` 里的机器与版本信息。

### 4.1 怎么记录 / 引用 JSON 与 CSV

* **不要改写旧结果目录。** 每次比较都保留整棵目录；新一次运行会拿到新的时间戳
  目录，旧目录是历史证据。
* **机器可读的汇总**用 `summary.json`（`schema = "uvcpp-db-baseline/1"`）。
  它把 `meta`、每轮 `runs`、单次调用 `cases`、整批 `bulk`、卡顿 `stalls`、
  并发 `concurrency` 放在同一份文件里；`raw_files` 指向同目录的明细。
* **逐轮原始行**用 `cases.tsv`（awk 友好）或 `cases.csv`（表格工具友好）。
  每行是 `(run, case, samples, min_us, mean_us, p50_us, p99_us, qps,
  rows_per_s)`；比较 A/B 时按 `(run, case)` 配对，不要只拿聚合值对比。
* **聚合表**在 `cases.agg.tsv` / `summary.csv`；整批、卡顿、并发分别在
  `*.agg.tsv`。CSV 只是对应 TSV 的逗号版本（case 名里没有逗号）。
* 报告里至少附：`summary.json`、`meta.txt`、完整命令行、原始
  `run-*.log`；共享 CI 的冒烟结果不要写成性能结论。

```bash
# 看逐轮点查（示例）
column -t -s $'\t' bench/orm/results/db-baseline-*/cases.tsv | head

# 校验 JSON 结构（python3 只用于检查，不是脚本依赖）
python3 -I -m json.tool \
  "$(ls -d bench/orm/results/db-baseline-* | tail -n 1)/summary.json" > /dev/null
```


---

## 5. 指标定义

单次调用（`cases`）：

* `min_us`：各轮 `min` 的**最小值**；
* `mean_us`：各轮 `mean` 的**算术平均**（不是把 10000 个样本混在一起重算）；
* `p50_us`：各轮 `p50` 的**中位数**；
* `p99_us`：各轮 `p99` 的**最大值**（保守；不是合并样本的 p99）；
* `qps`：由聚合 `mean_us` 反算的 `1e6 / mean_us`，与量具每轮的 calls/s 同式；
* `samples`：各轮样本数之和。

因此会出现 `p50 > mean`（各轮 p50 的中位数 vs 各轮 mean 的平均），这是聚合
口径的结果，不是笔误。原始逐轮值在 `cases.tsv` / `cases.csv` 里，任何阈值判断
都应先看逐轮值。

整批与并发：每轮样本数少，脚本同时给 `mean` 与 `median`；**比值判据用
median**。卡顿三臂本来就是每轮一个值，给 `median/min/max`。

当前 `bench_db.cpp` **不输出 p95**。本步骤没有为了补 p95 改测量语义；
#95 要求的 p95 需要在后续量具改动里另开一列，届时 baseline 需要重跑。

---

## 6. 基线记录（2026-11-27，SQLite）

原始结果目录（本机，未提交）：

```text
bench/orm/results/db-baseline-20261127T054834Z/
```

### 6.1 机器与版本

| 项 | 值 |
|---|---|
| 主机 / 内核 | `ZWY-FLMH-MINT` / Linux 6.14.0-37-generic, Ubuntu 24.04 |
| CPU | Intel(R) Core(TM) Ultra 5 125H；18 CPUs，2 threads/core，14 cores/socket，1 socket，max 4500 MHz |
| CPU governor | `powersave`（18 个 CPU 都是） |
| 起始 loadavg | `3.48 4.35 3.84`（开发机，仍有后台负载） |
| 编译器 | GCC 13.3.0（`c++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`） |
| CMake / 构建类型 | 3.28.3 / Release，`-O3 -DNDEBUG` |
| SQLite | pkg-config 3.45.1，头 3.45.1，链接 `libsqlite3.so.0` |
| git | `perf/orm-bench` @ `fa96380c317a6e5feabded0099df5f01acc800e0`（`dirty=yes` 只因本轮新增的文档/脚本/.gitignore；`bench_db.cpp` 与库源码未改） |
| 量具 sha256 | `2b2ee7f06d199808fe14fdf8d9fe1e361204c70256f1e124d0d100a66d41fb53` |
| 轮转 / 种子行 | 5 / 10000 |
| 每轮进程账 | user 3.77–4.10 s，sys 0.18–0.22 s，wall 4.83–5.16 s，maxRSS 8.0–8.3 MB |

**这是开发机上的临时基线，不是发布门禁读数。** 机器没有空闲下来（loadavg
3.48，governor 是 `powersave`），且只有 SQLite 一个后端。发布前必须在固定机器、
`performance` governor、无其他负载的条件下重跑，并用同一轮交替 A/B 复核。

### 6.2 单次调用（`cases.agg.tsv`）

| case | samples | min µs | mean µs | p50 µs | p99 µs | calls/s |
|---|---:|---:|---:|---:|---:|---:|
| open+close | 1000 | 20.2 | 29.1 | 27.9 | 126.9 | 34 364 |
| ping | 10 000 | 0.6 | 0.8 | 0.8 | 1.3 | 1 250 000 |
| select 1 | 10 000 | 0.8 | 1.0 | 1.0 | 1.8 | 961 538 |
| point select (inlined) | 10 000 | 4.7 | 6.3 | 6.4 | 10.4 | 158 730 |
| point select (param) | 10 000 | 4.7 | 6.2 | 6.3 | 10.1 | 160 772 |
| insert autocommit | 5000 | 826.7 | 1030.5 | 994.5 | 1806.3 | 970 |

读法：SQLite 的 `insert autocommit` 每条一次事务/提交，约 1 ms 是磁盘与
`fsync` 的钱；同一个事务里 1000 行的价格见下一节。参数化点查与内联点查在本机
SQLite 上同量级（每次都会重新 prepare/finalize）。

### 6.3 整批操作（`bulk.agg.tsv`，每轮内 3 次，共 15 个样本）

| case | samples | mean ms | median ms | min ms | max ms | rows/s（mean） | rows |
|---|---:|---:|---:|---:|---:|---:|---:|
| insert in txn | 15 | 4.2 | 4.1 | 3.0 | 6.3 | 252 661 | 1000 |
| fetch 10k rows | 15 | 3.1 | 3.0 | 2.5 | 4.0 | 3 348 482 | 10 000 |

`insert in txn` 是同一个事务里 1000 行后一次提交；`fetch 10k rows` 是
`SELECT k, s, d ... LIMIT 10000` 取回 10000 行 × 3 列。二者都是整批耗时，
不是单行延迟。

### 6.4 事件循环卡顿（`stalls.agg.tsv`）

1 ms 定时器 + 10 条慢查询（SQLite 用递归 CTE 烧 CPU，标定到约 118 ms/条）；
三条臂用同一个 client、同一条连接，差别只在“在哪个线程上阻塞”。

| arm | runs | max gap median µs | min µs | max µs | ticks median | batch median ms | timeouts |
|---|---:|---:|---:|---:|---:|---:|---:|
| sync（loop 线程） | 5 | 1 164 093 | 1 116 388 | 1 236 526 | 51 | 1163 | 0 |
| async（手写 `uvcpp_work`） | 5 | 1 720 | 1357 | 1856 | 1111 | 1176 | 0 |
| async（`uvcpp_db_async`） | 5 | 1 775 | 1200 | 1954 | 1149 | 1216 | 0 |

门面 / 手写 work 的 gap 中位数之比：**1.03×**（判据：同量级，约 1×）。
同步臂的 batch 与两条异步臂同量级（约 1.16–1.22 s）：异步买的是“循环不被卡住”，
不是“查询变快”。SQLite 的慢查询是本机烧 CPU，异步臂会与 worker 抢 CPU，
所以 gap 不会像 MySQL/PG 的服务端睡眠那样干净；这里 1.7–1.8 ms 对 1 ms 定时器
已经是可接受的量级。

### 6.5 并发（`concurrency.agg.tsv`）

4 线程 × 500 次参数化点查，同一批活换连接来源：

| 连接怎么来 | runs | mean ms | median ms | calls/s（mean） | calls/s（median） | created last | reused last |
|---|---:|---:|---:|---:|---:|---:|---:|
| 4 线程共享 1 个 client | 5 | 22.7 | 23.1 | 89 770 | 86 426 | — | — |
| 4 线程各开 1 个 client | 5 | 7.9 | 7.8 | 253 506 | 257 999 | — | — |
| 4 线程走池子（max=4） | 5 | 7.4 | 7.4 | 272 019 | 270 466 | 4 | 2000 |

池子 / 各开一条 = **0.95×**（判据：同量级，约 1×）；池子 / 共享一条 = 0.32×。
方括号里的 `created_last = 4` 是复用的见证：2000 次查询只开了 4 条连接；
`reused_last = 2000`。本机 SQLite 上池子略快于“各开一条”，因为四条连接的
`open` 是白付的，与 `doc/db-guide.md` 第 3.1 节的结论一致。

---

## 7. 未验证项（不要补数字）

截至 2026-11-27，本机只跑了 SQLite。以下都**没有**读数：

| 项 | 状态 |
|---|---|
| MySQL 基线 | **未验证。** 本机 `mysqld` 在 3306/33060 监听，但没有 `mysql.h` / `mariadb/mysql.h`，MySQL 后端未编进构建；`mysql -uroot` 返回 `Access denied`。 |
| PostgreSQL 基线 | **未验证。** 本机没有 PostgreSQL cluster（`/etc/postgresql` 不存在），服务 inactive，`pg_isready` 不可用；虽然 `libpq-fe.h` 在，但没有服务端。 |
| ODBC 到 SQLite | 未验证（本构建 `UVCPP_ENABLE_DB_ODBC=OFF`）。 |
| Windows / MSVC | 未验证。 |
| p95 | 量具当前不输出；本页只有 p50/p99。 |
| 每请求 CPU / 分配次数 | 未测量。只记录了整支量具的 user/sys/wall 和 maxRSS，不能拆成每请求 CPU 或分配次数。 |
| BLOB / 列数扩展 / 10× 结果集内存 | 未测量。 |
| `uvcpp_db_pool` 的异步回调接口 | 未单独测量。第 10 节用的是同步 `pool.query()` 的 4 线程并发；第 9 节测的是 `uvcpp_db_async` 门面派发。`pool.query(loop, ...)` / `pool.transaction(loop, ...)` 需要后续量具项。 |
| PostgreSQL 真异步（`query_async()` / `uv_poll`）与线程池路径的对比 | 未验证（没有 PostgreSQL 服务；本机只有 SQLite）。 |
| ORM 端到端 / 纯开销 | **未实现，等 #91/#92。** |

MySQL / PostgreSQL 有可用服务后，用同一个脚本即可补跑，前提是构建里真的编进了
对应后端（看 `meta.txt` 的 `uvcpp_enable_db_mysql` / `_pgsql`，以及运行时
`uvcpp_db_drivers()`）：

```bash
# 需要构建时打开对应后端，并且服务端可达
UVCPP_DB_BENCH_URL='mysql://user:pass@127.0.0.1:3306/uvcpp_test' \
  bench/orm/run_db_baseline.sh --reps 5 --label mysql
UVCPP_DB_BENCH_URL='postgresql://uvcpp@127.0.0.1:5432/uvcpp_test' \
  bench/orm/run_db_baseline.sh --reps 5 --label postgres
```

同一 URL 不要并发跑两支脚本：量具会在同一张表上 DROP/CREATE。SQLite 用每轮结果
目录里的独立文件，互不干扰；服务端后端请给每次调用不同的数据库或串行执行。

---

## 8. ORM 端到端与纯开销基准（已落地）

量具：`bench/orm/bench_orm.cpp`（目标 `uvcpp_bench_orm`）；
驱动：`bench/orm/run_orm_overhead.sh`。它只回答两件事：

1. **端到端**：同一个 `uvcpp_db_client`、同一条连接、同一个 SQLite 文件上，
   `find_by_pk` / `insert` / `update` / `remove` / `find_all` 比**等价手写 SQL**
   多花多少；
2. **纯开销**（不碰数据库）：SQL 构造与结果映射各差多少。

> 本阶段**只验证 SQLite**。DDL 与 raw SQL 按 SQLite 写死，脚本不会给出
> MySQL/PostgreSQL/Windows 数字；没有对应实测就写“未验证”。

### 8.1 口径

| 项 | 值 |
|---|---|
| 构建 | `CMAKE_BUILD_TYPE=Release`（`-O3 -DNDEBUG`），`UVCPP_BUILD_ORM=ON` |
| 后端 | 只验证 SQLite；驱动默认 pragma（不设 WAL/synchronous/cache） |
| 表 | `bench_user`，5 列：`id` 自增主键、`name` TEXT、`age` INT、`score` REAL、`active` INT |
| 连接 | 全量用例复用**同一个** `uvcpp_db_client`；不开池、不开事务 |
| 端到端样本 | 每臂 1000 次（`--samples=1000`），每臂 warm-up 50 次 |
| 纯构造样本 | 每臂 100000 次（`--pure-ops=100000`），按 1000 次一批计时 |
| 纯映射样本 | 每臂 2000 个样本 × 每样本 100 行 = 200000 行 |
| 轮转 | 3 轮（默认）；聚合取各轮的**中位数** |
| 种子 | 3150 行（`3 × samples + 2 × warm-up + 50`），一次性准备，不计时 |

**A/B 口径（关键）：**

* **raw 臂**：SQL 文本提前拼好并复用（手写热路径的写法）。参数里带变化值的
  （点查主键、删除主键）每次重建 `uvcpp_db_params`；插入复用一组固定参数；
  更新在两个预构造的参数集之间交替（两臂都每轮换值，逼出真实写盘）。结果按
  已知列序手工映射（点查 1 行、`find_all` 100 行）。
* **ORM 臂**：每次调用都重新构造 SQL 与参数 —— 那正是 ORM 的开销。点查走
  `find_by_pk<T>`；100 行走 `find_all<T>(db, builder, ...)`（不传 `total`，
  避免多一条 COUNT）；写操作走 `insert<T>` / `update<T>` / `remove<T>`。
* 每一对样本**交替**顺序（先 raw 后 ORM / 先 ORM 后 raw），抵消机器漂移；
  不是“raw 跑完再 ORM 跑完”。
* 计时单位：端到端一次调用；`build_select` 一次构造；`map_rows` 一行。
  纯开销按批计时再除以批内单位数，避免把 `steady_clock` 的几十纳秒放大成
  结果的一部分。TSV 的 `samples` 是**计时批数**，`ops` 是单位总数。
* 启动时会逐字比对 raw SQL / 参数与 ORM 生成结果（`pk`、`insert`、`update`、
  `remove`、`select_all`、`build_select`），不一致直接失败，不会“大概等价”。

### 8.2 构建

```bash
cmake -S . -B build-orm-bench -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUVCPP_ENABLE_DB=ON \
  -DUVCPP_ENABLE_DB_SQLITE=ON \
  -DUVCPP_BUILD_ORM=ON \
  -DUVCPP_BUILD_BENCH=ON \
  -DUVCPP_BUILD_TESTS=OFF \
  -DUVCPP_BUILD_WEB=OFF \
  -DUVCPP_BUILD_WEBAPP=OFF

cmake --build build-orm-bench --target uvcpp_bench_orm -j"$(nproc)"
```

产出：`build-orm-bench/bench/uvcpp_bench_orm`。`UVCPP_BUILD_WEBAPP=OFF` 不会
跳过它：和 db 量具一样，它只依赖 db 与 ORM。

### 8.3 跑

最短命令（默认构建目录 `build-orm-bench`、3 轮、1000 次端到端、100000 次
纯构造、结果写到 `bench/orm/results/`）：

```bash
bench/orm/run_orm_overhead.sh
```

本页 8.7 的读数用的命令：

```bash
bench/orm/run_orm_overhead.sh \
  --build-dir build-orm-bench \
  --reps 3 \
  --samples 1000 \
  --warmup 50 \
  --pure-ops 100000 \
  --pure-batch 1000 \
  --map-batches 2000 \
  --map-rows 100 \
  --label sqlite-orm-overhead \
  --out bench/orm/results
```

指定 SQLite 文件：

```bash
bench/orm/run_orm_overhead.sh --url 'sqlite:///tmp/uvcpp-orm-bench.sqlite'
```

脚本也认 `UVCPP_ORM_BENCH_URL`，其次 `UVCPP_DB_BENCH_URL`。`--help` 是完整参数表。
相对路径按**当前工作目录**解析；脚本只找
`<build-dir>/bench/uvcpp_bench_orm`。

`--samples` 下限 100；`--pure-ops` 下限 1000 且必须被 `--pure-batch` 整除。
跑不足 #95 要求的样本数时，输出里仍会如实写实际样本数，不要拿冒烟数字当结论。

### 8.4 用例

| case | unit | raw 臂 | ORM 臂 |
|---|---|---|---|
| `pk` | call | `SELECT ... WHERE id = ? LIMIT ?` + 手工读 5 列 | `find_by_pk<bench_user>` |
| `find_all100` | call | `SELECT ... ORDER BY id LIMIT ?` + 手工映射 100 行 | 构造器 + `find_all<bench_user>` |
| `update` | call | `UPDATE ... SET ... WHERE id = ?` | `update<bench_user>` |
| `insert` | call | `INSERT ... VALUES (...)` + `db.insert` 回填 id | `insert<bench_user>`（回填 `new_id`） |
| `remove` | call | `DELETE ... WHERE id = ?` | `remove<bench_user>` |
| `build_select` | query | 手写字符串拼接 + `uvcpp_db_params` | `select<T>(dialect).where...build()` |
| `map_rows` | row | 内存 `uvcpp_db_table` 的手工映射 | `orm_detail::map_table<T>` |

两个写用例的细节：`update` 两臂各更新**自己的一行**并每轮换值（值不变时 SQLite
可能不写 journal，量到的就不是真实更新）；`remove` 两臂删各自预置的行，warm-up
也用独立行，不让 warm-up 吃掉被测行。

生成的 SQL（量具启动时会逐字比对 raw 和 ORM 这两侧，不一致直接失败）：

```sql
-- pk
SELECT "id", "name", "age", "score", "active" FROM "bench_user"
 WHERE "id" = ? LIMIT ?
-- find_all100
SELECT "id", "name", "age", "score", "active" FROM "bench_user"
 ORDER BY "id" ASC LIMIT ?
-- insert
INSERT INTO "bench_user" ("name", "age", "score", "active")
 VALUES (?, ?, ?, ?)
-- update
UPDATE "bench_user" SET "name" = ?, "age" = ?, "score" = ?, "active" = ?
 WHERE "id" = ?
-- remove
DELETE FROM "bench_user" WHERE "id" = ?
-- build_select
SELECT "id", "name", "age", "score", "active" FROM "bench_user"
 WHERE "age" >= ? AND "active" = ? ORDER BY "id" ASC LIMIT ?
```

`map_rows` 调的是 `uvcpp::orm_detail::map_table<T>`。它在公开头里、可直接调用，
也是 `find_all` 内部用的同一条映射路径，所以这里量的是真代码，不是复制品。
它是 `orm_detail` 命名空间（实现细节），升级时不保证源码兼容 —— 升级后先跑
这个量具，编译/等价性自检会立刻报出来。

### 8.5 指标与“额外开销百分比”

* 端到端指标：min / mean / p50 / p99（微秒）、ops/s（`1e6 / mean_us`）、样本数。
  p50/p99 的取法与 `bench_db.cpp`/`bench_router.cpp` 一致：
  `values[n * p / 100]`（因此小样本时 p99 接近最大值）。
* 纯构造：单位是“一次构造”，报 µs/查询。纯映射：单位是“一行”，报 µs/行
  （本文表格里的 0.111 µs/行 = 111 ns/行）。
* **额外开销**：`extra_% = (ORM / raw - 1) * 100`。负值是可能的：那一轮 ORM
  更快或抖动，按原样报，不截断成 0。
* 聚合：每轮的 min/mean/p50/p99 先各自算好，跨轮取**中位数**；不是把 3000 个
  样本倒在一起重算。`p99_extra_pct_max` 是单轮最坏值，只作诊断。
  因此“聚合 p99 额外”是**两个中位数之比**，不必然等于逐轮 p99 额外百分比的中
  位数；逐轮配对值在 `overhead_runs.tsv`，两种视角都要看。
* 判据（来自 issue #95，只对 `unit=call` 的端到端用例）：

  | gate | 含义 |
  |---|---|
  | `target_met` | 聚合 p50/p99 额外开销 ≤ 15% |
  | `within_hard_gate` | ≤ 25%，未达目标但没有超过硬门禁 |
  | `exceeds_hard_gate` | > 25%，按 #95 **不得视为通过** |
  | `n/a` | 纯构造/纯映射，没有端到端百分比门禁 |

  gate 取聚合 p50 与聚合 p99 的较大值；不用单轮 p99 最大值翻转结论（一次调度
  抖动不应改写中位数），但 `p99_extra_pct_max` 列会把它显示出来。

### 8.6 结果目录

```text
bench/orm/results/orm-overhead-<UTC>/
  meta.txt / meta.tsv       机器、编译器、CMake、SQLite、git、命令、样本数
  build-config.txt          从 CMakeCache.txt 抠出的相关开关
  run-01.log .. run-NN.log  每轮量具 stdout（原始）
  run-01.err ..             每轮 stderr（干净时为空）
  run-01.time.txt ..        每轮 GNU time 报告（没有 /usr/bin/time 时不生成）
  run-01.tsv ..             每轮 raw/ORM 的原始 TSV
  runs.tsv                  每轮 exit / wall / user / sys / maxRSS / 状态
  cases.tsv                 逐轮明细合并（带 run 列）
  agg.tsv                   跨轮聚合（中位数）
  overhead.tsv              跨轮 ORM-vs-raw 额外开销 + gate
  overhead_runs.tsv         每轮 ORM-vs-raw 额外开销
  aggregate.awk             聚合器源码（现场证据）
  agg_json.awk / overhead_json.awk / runs_json.awk
                            summary.json 的片段生成器（现场证据）
  summary.txt               人读汇总
  summary.json              机读汇总（schema = uvcpp-orm-overhead/1）
  bench.sqlite              默认 URL 时本轮的 SQLite 文件
```

`bench/orm/results/` 已在 `.gitignore`。原始目录不进仓库；引用时把
`summary.json` / `summary.txt` 的关键表抄进报告，并附 `meta.txt`。

**不覆盖已有结果**：每次调用在 `--out` 下新建
`orm-overhead-<UTC>[-NN]/`，用原子 `mkdir`；同一秒的并发调用也会拿到不同后缀。

**失败必返回非 0**：

```bash
bench/orm/run_orm_overhead.sh --url 'bogus://not-a-db' \
  --reps 1 --out /tmp/uvcpp-orm-fail
echo $?    # 1（量具退出 2：没有可用的 bogus 驱动）
```

缺可执行量具返回 2；失败目录保留 `run-01.err`、`runs.tsv`、`meta.*` 和说明
失败的 `summary.txt`。

### 8.7 本机优化后读数（2026-11-27，SQLite）

原始结果目录（本机，未提交，`.gitignore` 已覆盖）：

```text
/tmp/orm-overhead-final2/orm-overhead-20261127T063811Z/
```

机器与版本：Intel Core Ultra 5 125H（18 CPU）、Ubuntu 24.04、GCC 13.3.0、
CMake 3.28.3、`Release (-O3 -DNDEBUG)`、SQLite 3.45.1（系统
`libsqlite3.so.0`）。governor 是 `powersave`，开跑时 loadavg ≈ 3.0；
**不是安静的性能机，也没有绑核**。3 轮，每轮每臂 **500** 次端到端、
**50000** 次纯构造、**2000 × 100 行**纯映射。

这轮读数是在 #103/#104 的 ORM 热路径优化之后（SQL 形状缓存、行 move、
列转换函数指针、延迟错误标签、模型校验缓存）。

跨轮聚合（中位数）：

| case | unit | raw p50 µs | ORM p50 µs | p50 额外 | raw p99 µs | ORM p99 µs | p99 额外 | gate |
|---|---|---:|---:|---:|---:|---:|---:|---|
| `pk` | call | 6.25299 | 6.547 | +4.70% | 9.11902 | 9.90204 | +8.59% | `target_met` |
| `find_all100` | call | 39.248 | 42.643 | +8.65% | 48.337 | 54.675 | +13.11% | `target_met` |
| `update` | call | 963.477 | 969.465 | +0.62% | 1511.64 | 1538.22 | +1.76% | `target_met` |
| `insert` | call | 975.899 | 976.125 | +0.02% | 1557.91 | 1429.66 | −8.23% | `target_met` |
| `remove` | call | 968.644 | 972.337 | +0.38% | 1463.41 | 1667.15 | +13.92% | `target_met` |

| case | unit | raw mean µs | ORM mean µs | mean 额外 | ops/s raw → ORM |
|---|---|---:|---:|---:|---|
| `pk` | call | 6.29048 | 6.61715 | +5.19% | 158970 → 151122 |
| `find_all100` | call | 39.7683 | 43.1754 | +8.57% | 25145.7 → 23161.3 |
| `build_select` | query | 0.124449 | 1.01252 | +714%（约 8×） | 8.04M → 988k |
| `map_rows` | row | 0.02629 | 0.052171 | +98.4% | 38.0M → 19.2M |

结论，按 #95 的判据：

* **端到端 `pk` 与 `find_all100` 的 p50/p99 都在 15% 目标内。** 这只说明本机
  这一次 3 轮读数；不是固定性能机的发布门禁结论。p99 仍有单轮抖动，逐轮值在
  `overhead_runs.tsv`，不要只看聚合。
* **写路径 `update`/`insert`/`remove` 也在目标内**（p50 +0.0%~+0.6%，p99
  −8.2%~+13.9%）。SQLite 默认 pragma 下每条写语句约 1 ms 的 fsync 把 ORM 的
  微秒级开销淹没了；这不说明写路径 ORM 开销为零，也不能把写路径的结论借给
  读路径。
* **纯构造约 1.01 µs/查询**（raw 0.124 µs），相对约 8×，绝对成本约 0.9 µs；
  #95 对构造只要求按查询报告，没有设相对百分比门禁。
* **纯映射约 52 ns/行**（raw 26 ns/行），远低于 #95 的 10 µs/行目标；这是
  5 字段模型，没有做 1/4/16/64 字段梯度，不能据此声称“不随字段数超线性”。

**这些是本机开发环境下的读数，不是发布门禁结论。** 共享 CI / 开发机的抖动、
powersave governor、未绑核、3 轮样本都不足以支撑发布判断；固定机器 +
performance governor + 更多轮次 + 与 1.11.0 db 基线同条件，才是 #95 的发布口径。

### 8.8 未验证项与限制

* 只在 **SQLite 3.45.1 / Linux / GCC 13.3 / x86_64** 上跑过。**没有**验证
  MySQL、PostgreSQL、ODBC、Windows/MSVC；脚本不会生成这些后端的结果。
* **没有**测连接池（1/4/16）、并发（1/16/64）、事务提交/回滚/冲突/超时、
  异步路径、大结果集/BLOB、10× 结果集、字段数梯度、分配次数/CPU 周期。
  这些是 #95 的完整范围，不在本阶段。
* `map_rows` 用的是 `orm_detail::map_table<T>`（实现细节命名空间）；它不是
  “伪造的公开 API”，但也不承诺源码级稳定。
* SQLite 用的是驱动默认 pragma。写路径的数字主要由 fsync 决定，换
  `synchronous=NORMAL`/WAL 会得到另一组数，本页不做那种比较。
* `p99` 在每轮 1000 个样本上仍然对调度抖动敏感；3 轮不足以给置信区间。
  逐轮原始值在 `overhead_runs.tsv` / `run-*.tsv`。
* 量具在启动时 `DROP TABLE IF EXISTS "bench_user"`，URL 指向的库里同名表会被
  删掉。请给它一个专用库或结果目录里的临时文件。

### 8.9 与 db 基线的关系

ORM 的端到端数字必须和**同一次运行里的等价手写 SQL** 比较，而不是和第 6 节
的 SQLite 绝对数比较。第 6 节的 db 基线用于回答“底层 db 路径在 1.12.0 有没有
回退”：吞吐回退不得 > 5%，p99 回退不得 > 10%（#95）。两套判据不要混用。

---

## 9. CI 规则

* **共享 CI runner：只做冒烟。** 用同一组 CMake 选项构建
  `uvcpp_bench_db`（以及 `UVCPP_BUILD_ORM=ON` 时的 `uvcpp_bench_orm`），然后：

  ```bash
  bench/orm/run_db_baseline.sh --reps 1 --rows 100 --out "$RUNNER_TEMP/db-smoke"
  bench/orm/run_orm_overhead.sh --reps 1 \
    --samples 100 --warmup 5 \
    --pure-ops 5000 --pure-batch 500 \
    --map-batches 50 --map-rows 100 \
    --out "$RUNNER_TEMP/orm-smoke"
  ```

  判据只有“退出码 0、日志里有 `完成。`、没有崩溃/超时”。**不设性能阈值**，
  也不把共享 runner 的 p50/p99 当发布结论；ORM 冒烟的 `--samples` 低于 #95
  的 1000 次要求，只用于确认能跑。
* **固定机器 / 夜间任务：才做稳定基准。** 同一台机器、同一数据库版本、
  `performance` governor、尽量无其他负载、至少 5 轮；与记录的基线比较 #95 的
  阈值。超阈值发告警/开 issue，**不要伪装成单元测试失败**。ORM 的 gate 列
  只是报告，不是 CI 退出码。
* 夜间任务的原始日志、`summary.json`、`meta.txt` 作为 artifact 保留；报告里写
  日期、commit、SQLite/MySQL/PG 版本、命令和硬件。

---

## 10. 快速自检

```bash
# 1. 脚本语法
bash -n bench/orm/run_db_baseline.sh
bash -n bench/orm/run_orm_overhead.sh

# 2. 连续跑两次：应得到两个不同的时间戳目录
bench/orm/run_db_baseline.sh --reps 1 --rows 100 --out /tmp/db-baseline-check
bench/orm/run_db_baseline.sh --reps 1 --rows 100 --out /tmp/db-baseline-check
ls -1 /tmp/db-baseline-check

bench/orm/run_orm_overhead.sh --reps 1 --samples 100 --warmup 5 \
  --pure-ops 5000 --pure-batch 500 --map-batches 50 --map-rows 100 \
  --out /tmp/orm-check
bench/orm/run_orm_overhead.sh --reps 1 --samples 100 --warmup 5 \
  --pure-ops 5000 --pure-batch 500 --map-batches 50 --map-rows 100 \
  --out /tmp/orm-check
ls -1 /tmp/orm-check

# 3. 失败必须非 0（两条脚本都是）
bench/orm/run_db_baseline.sh --url 'bogus://x' --reps 1 \
  --out /tmp/db-baseline-check-fail
echo $?    # 1
bench/orm/run_orm_overhead.sh --url 'bogus://x' --reps 1 \
  --samples 100 --pure-ops 1000 --pure-batch 1000 --map-batches 1 \
  --out /tmp/orm-check-fail
echo $?    # 1
bench/orm/run_orm_overhead.sh --build-dir /nonexistent --out /tmp/orm-check-fail
echo $?    # 2

# 4. JSON 可解析（python3 只用于检查，不是脚本依赖）
python3 -I -m json.tool \
  "$(ls -d bench/orm/results/db-baseline-* | tail -n 1)/summary.json" > /dev/null
python3 -I -m json.tool \
  "$(ls -d bench/orm/results/orm-overhead-* | tail -n 1)/summary.json" > /dev/null
```

脚本只依赖 bash、POSIX `awk`、`sed`、`grep`、`sort`、`paste`、`date`、
`sha256sum` 和 GNU coreutils；`/usr/bin/time` 用于 CPU/RSS，找不到时自动降级
（该轮 `runs.tsv` 的 CPU/RSS 为空，但基准照跑）。

---

## 11. 已知坑

* **`--rows` 下限是 100**，与 `bench_db.cpp` 的参数校验一致；少于 100 会被
  bench 自己拒绝。
* **仪表本身的“慢查询”在 SQLite 上是递归 CTE**，不是 sleep；它烧本机 CPU，
  所以 SQLite 的异步卡顿臂会和 worker 抢 CPU。
* **ORM 的 `--pure-ops` 必须被 `--pure-batch` 整除**，否则脚本在开跑前退出 2。
  `--samples` 下限 100，但 #95 的端到端结论要求至少 1000。
* **`update` 两臂故意每轮换值并要求 `affected == 1`**。SQLite 在值没变时可能
  不写 journal；如果把它改成“一直写同一个值”，写路径会量成几微秒的假象。
* **`insert`/`update`/`remove` 的 µs 主要由 fsync 决定**（默认 pragma 下约
  1 ms）。ORM 的额外开销因此看起来很小；不要据此推断读路径也一样。
* **点查 `pk` 在本机超过 #95 的 25% 硬门禁**（第 8.7 节）。这是量具的结论，
  不是量具的故障；不要用调低样本数、换 pragma 或改用例把它“修”掉。
* **`p99` 的取法是 `values[n*99/100]`**，与 `bench_db.cpp` 相同；每轮 1000 个
  样本时它不是严格的最近秩 p99，单轮值会抖。跨轮看中位数，逐轮看
  `overhead_runs.tsv`。
* **每轮都在同一个 URL 上 DROP + CREATE 表并重新灌数据**；“第 2 轮比第 1 轮快”
  可能是 SQLite 页缓存/文件系统缓存，不是代码变了。基线记录里逐轮值全部保留。
* **`bench_db.cpp` 第 8 节的 `fetch 10k rows` 按 `LIMIT 10000` 取**；`--rows`
  大于 10000 时仍然只取 10000 行。表里 `rows` 列是实际取回行数。
* **不要用 `git add -A` 把 `bench/orm/results/` 吞进去**：它已在 `.gitignore`；
  提交前仍应 `git status` 确认。
