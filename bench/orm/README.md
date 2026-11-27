# 数据库 / ORM 基准：基线与方法

本目录是 issue [#95](https://github.com/Antruly/libuvcpp/issues/95)（父任务
[#88](https://github.com/Antruly/libuvcpp/issues/88)）的**第一步**：给现有 `db`
模块建立可重复的基线量具和方法学。

> **当前只有 `db` 基线，没有任何 ORM 数字。**
> ORM 尚未实现，`uvcpp_db_client` / `uvcpp_db_async` / `uvcpp_db_pool` 之外的
> `bench_orm` 端到端与纯开销，要等 #91（结果映射与 CRUD）和 #92（事务与异步集成）
> 落地后再补。在那之前，本页里的每个数字都可以由 `bench_db.cpp` 重新跑出来；
> 不要用别的基准或估算值冒充 ORM 读数。

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

## 8. 后续 ORM 基准怎么接（#91 / #92 之后）

目标不是“跑出一个好看的数”，而是能回答两件事：

1. **ORM 端到端**相对等价手写 SQL 多花多少；
2. **ORM 纯开销**（只生成 SQL、只映射结果，不连数据库）每行多少，是否随字段数
   超线性增长。

### 8.1 同一套量具、同一套落盘

`bench/bench_orm.cpp` 复用 `bench_db.cpp` 的计时与报表形状（min/mean/p50/p99、
`--label` / `--rows`），并由本目录的脚本以同一套 `bench/orm/` 结果格式落盘。
`summary.json` 增加一个 `"orm"` 段，**保留现有 `"cases"` / `"bulk"` /
`"stalls"` / `"concurrency"` 段**；不要用 ORM 数字覆盖 db 基线。

### 8.2 端到端 A/B

* A 臂：与 ORM 等价的**手写 SQL**（同一条语句、同一组参数、同一结果形状）；
* B 臂：ORM 生成 SQL + 绑定 + 映射 + 数据库调用；
* 同一台机器、同一个数据库版本、同一编译配置、同一连接数；
* 每轮内**交替** A/B（A B B A 或逐查询交替），按配对差取中位数，抵消机器漂移；
* 分别报告点查、小结果集高频查询、事务提交/回滚、大结果集；
* 连接池 1 / 4 / 16 条连接，并发 1 / 16 / 64；
* 门禁（来自 #95）：端到端 p50/p99 额外开销目标 ≤ 15%，不得 > 25%。

### 8.3 纯开销

* 只生成 SQL（不连库）：按查询计，报告 ns/查询；
* 只映射结果（准备一份内存里的结果表）：按行计，报告 µs/行；
* 字段数梯度至少 1 / 4 / 16 / 64，行数梯度覆盖小结果集与 10× 结果集；
* 门禁：目标 ≤ 10 µs/行（以基准机为准），且**不得随字段数超线性增长**；
* 同时记录分配次数（如果需要，用 `getrusage`/分配器统计另开一列；当前 db 基线
  没有这一列，不要拿 RSS 冒充）。

### 8.4 与 db 基线的关系

ORM 的端到端数字必须和**同一次运行里的等价手写 SQL** 比较，而不是和本页的
SQLite 绝对数比较。本页的 db 基线用于回答“底层 db 路径在 1.12.0 有没有回退”：
吞吐回退不得 > 5%，p99 回退不得 > 10%（#95）。两套判据不要混用。

---

## 9. CI 规则

* **共享 CI runner：只做冒烟。** 用同一组 CMake 选项构建
  `uvcpp_bench_db`，然后：

  ```bash
  bench/orm/run_db_baseline.sh --reps 1 --rows 100 --out "$RUNNER_TEMP/db-smoke"
  ```

  判据只有“退出码 0、日志里有 `完成。`、没有崩溃/超时”。**不设性能阈值**，
  也不把共享 runner 的 p50/p99 当发布结论。
* **固定机器 / 夜间任务：才做稳定基准。** 同一台机器、同一数据库版本、
  `performance` governor、尽量无其他负载、至少 5 轮；与记录的基线比较 #95 的
  阈值。超阈值发告警/开 issue，**不要伪装成单元测试失败**。
* 夜间任务的原始日志、`summary.json`、`meta.txt` 作为 artifact 保留；报告里写
  日期、commit、SQLite/MySQL/PG 版本、命令和硬件。

---

## 10. 快速自检

```bash
# 1. 脚本语法
bash -n bench/orm/run_db_baseline.sh

# 2. 连续跑两次：应得到两个不同的时间戳目录
bench/orm/run_db_baseline.sh --reps 1 --rows 100 --out /tmp/db-baseline-check
bench/orm/run_db_baseline.sh --reps 1 --rows 100 --out /tmp/db-baseline-check
ls -1 /tmp/db-baseline-check

# 3. 失败必须非 0
bench/orm/run_db_baseline.sh --url 'bogus://x' --reps 1 \
  --out /tmp/db-baseline-check-fail
echo $?    # 1

# 4. JSON 可解析（python3 只用于检查，不是脚本依赖）
python3 -I -m json.tool \
  "$(ls -d bench/orm/results/db-baseline-* | tail -n 1)/summary.json" > /dev/null
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
* **每轮都在同一个 URL 上 DROP + CREATE 表并重新灌数据**；“第 2 轮比第 1 轮快”
  可能是 SQLite 页缓存/文件系统缓存，不是代码变了。基线记录里逐轮值全部保留。
* **`bench_db.cpp` 第 8 节的 `fetch 10k rows` 按 `LIMIT 10000` 取**；`--rows`
  大于 10000 时仍然只取 10000 行。表里 `rows` 列是实际取回行数。
* **不要用 `git add -A` 把 `bench/orm/results/` 吞进去**：它已在 `.gitignore`；
  提交前仍应 `git status` 确认。
