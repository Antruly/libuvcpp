#!/usr/bin/env bash
# =============================================================================
# bench/orm/run_orm_overhead.sh — ORM 端到端 / 纯开销基准的可重复驱动
#
# 它**只跑** `bench/orm/bench_orm.cpp`（目标 `uvcpp_bench_orm`），不替你配置或
# 编译；脚本负责：
#   * 每次调用建一个**新的**结果目录（绝不覆盖上一次）；
#   * 轮转 N 次（默认 3），保存每轮 stdout / stderr / GNU time / 原始 TSV；
#   * 记录机器、编译器、CMake、SQLite、git、命令；
#   * 用 shell + POSIX awk 聚合逐轮数据，算出 ORM 相对 raw 的 mean/p50/p99
#     额外开销，并生成 summary.txt 与 summary.json（不依赖 jq / python）。
#
# 用法（相对路径按**当前工作目录**解析）：
#
#   bench/orm/run_orm_overhead.sh \
#       --build-dir build-orm-bench \
#       --reps 3 \
#       --out bench/orm/results
#
#   # 指定 SQLite 文件（也可以用 UVCPP_ORM_BENCH_URL 环境变量）
#   bench/orm/run_orm_overhead.sh --url 'sqlite:///tmp/orm-bench.sqlite'
#
# 退出码：0 = 全部轮次跑完且汇总生成成功；1 = 运行/聚合失败；2 = 用法错 /
# 缺可执行量具。
#
# 这是一支量具驱动，不是 CI 单元测试。本机读数不能写成发布门禁结论，原因见
# bench/orm/README.md 的“ORM 基准”一节。
# =============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)

ORIG_ARGS=("$@")

BUILD_DIR="build-orm-bench"
URL=""
REPS=3
OUT_ROOT="bench/orm/results"
LABEL="orm-overhead"
SAMPLES=1000
WARMUP=50
PURE_OPS=100000
PURE_BATCH=1000
PURE_WARMUP=1000
MAP_BATCHES=2000
MAP_ROWS=100
MAP_WARMUP=2

die() {
  printf '错误：%s\n' "$*" >&2
  exit 2
}

usage() {
  cat <<'EOF'
用法：run_orm_overhead.sh [选项]

  -b, --build-dir DIR   CMake 构建目录（默认 build-orm-bench；相对当前目录）
  -u, --url URL         SQLite 连接串（默认取 UVCPP_ORM_BENCH_URL，再取
                        UVCPP_DB_BENCH_URL；都缺则在本次结果目录里建
                        sqlite://<run-dir>/bench.sqlite）
  -n, --reps N          轮转次数，>= 1（默认 3）
  -o, --out DIR         结果根目录（默认 bench/orm/results）
  -l, --label NAME      传给量具的 --label（默认 orm-overhead）
  -s, --samples N       端到端每臂样本数，>= 100（默认 1000）
      --warmup N        端到端每臂 warm-up 调用数（默认 50）
      --pure-ops N      build_select 每臂构造次数，>= 1000（默认 100000）
      --pure-batch N    每个计时样本的构造次数，需整除 pure-ops（默认 1000）
      --pure-warmup N   build_select 每臂 warm-up 构造次数（默认 1000）
      --map-batches N   map_rows 每臂计时样本数（默认 2000）
      --map-rows N      每个 map_rows 样本映射的行数（默认 100）
      --map-warmup N    map_rows 每臂 warm-up 整表映射次数（默认 2）
  -h, --help            显示本帮助

每次调用都会在 --out 下新建 orm-overhead-<UTC 时间戳>[-NN]/，不覆盖已有结果。
量具只支持 SQLite：DDL 与 raw SQL 按 SQLite 写死，其它后端没有在这里验证。
EOF
}

while (($#)); do
  case "$1" in
    -b|--build-dir)
      (($# >= 2)) || die "$1 缺少参数"
      BUILD_DIR=$2
      shift 2
      ;;
    -u|--url)
      (($# >= 2)) || die "$1 缺少参数"
      URL=$2
      shift 2
      ;;
    -n|--reps)
      (($# >= 2)) || die "$1 缺少参数"
      REPS=$2
      shift 2
      ;;
    -o|--out)
      (($# >= 2)) || die "$1 缺少参数"
      OUT_ROOT=$2
      shift 2
      ;;
    -l|--label)
      (($# >= 2)) || die "$1 缺少参数"
      LABEL=$2
      shift 2
      ;;
    -s|--samples)
      (($# >= 2)) || die "$1 缺少参数"
      SAMPLES=$2
      shift 2
      ;;
    --warmup)
      (($# >= 2)) || die "$1 缺少参数"
      WARMUP=$2
      shift 2
      ;;
    --pure-ops)
      (($# >= 2)) || die "$1 缺少参数"
      PURE_OPS=$2
      shift 2
      ;;
    --pure-batch)
      (($# >= 2)) || die "$1 缺少参数"
      PURE_BATCH=$2
      shift 2
      ;;
    --pure-warmup)
      (($# >= 2)) || die "$1 缺少参数"
      PURE_WARMUP=$2
      shift 2
      ;;
    --map-batches)
      (($# >= 2)) || die "$1 缺少参数"
      MAP_BATCHES=$2
      shift 2
      ;;
    --map-rows)
      (($# >= 2)) || die "$1 缺少参数"
      MAP_ROWS=$2
      shift 2
      ;;
    --map-warmup)
      (($# >= 2)) || die "$1 缺少参数"
      MAP_WARMUP=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      usage >&2
      die "未知选项：$1"
      ;;
    *)
      usage >&2
      die "多余的位置参数：$1"
      ;;
  esac
done
(($# == 0)) || { usage >&2; die "多余的位置参数：$*"; }

[[ "$REPS" =~ ^[0-9]+$ ]] && ((REPS >= 1)) || die "--reps 必须是 >= 1 的整数：$REPS"
[[ "$SAMPLES" =~ ^[0-9]+$ ]] && ((SAMPLES >= 100)) \
  || die "--samples 必须是 >= 100 的整数：$SAMPLES"
[[ "$WARMUP" =~ ^[0-9]+$ ]] || die "--warmup 必须是 >= 0 的整数：$WARMUP"
[[ "$PURE_OPS" =~ ^[0-9]+$ ]] && ((PURE_OPS >= 1000)) \
  || die "--pure-ops 必须是 >= 1000 的整数：$PURE_OPS"
[[ "$PURE_BATCH" =~ ^[0-9]+$ ]] && ((PURE_BATCH >= 1)) \
  || die "--pure-batch 必须是 >= 1 的整数：$PURE_BATCH"
((PURE_OPS % PURE_BATCH == 0)) \
  || die "--pure-ops 必须被 --pure-batch 整除：$PURE_OPS % $PURE_BATCH"
[[ "$MAP_BATCHES" =~ ^[0-9]+$ ]] && ((MAP_BATCHES >= 1)) \
  || die "--map-batches 必须是 >= 1 的整数：$MAP_BATCHES"
[[ "$MAP_ROWS" =~ ^[0-9]+$ ]] && ((MAP_ROWS >= 1)) \
  || die "--map-rows 必须是 >= 1 的整数：$MAP_ROWS"
[[ "$PURE_WARMUP" =~ ^[0-9]+$ ]] || die "--pure-warmup 必须是 >= 0 的整数：$PURE_WARMUP"
[[ "$MAP_WARMUP" =~ ^[0-9]+$ ]] || die "--map-warmup 必须是 >= 0 的整数：$MAP_WARMUP"

if [[ -z "$URL" && -n "${UVCPP_ORM_BENCH_URL:-}" ]]; then
  URL=$UVCPP_ORM_BENCH_URL
fi
if [[ -z "$URL" && -n "${UVCPP_DB_BENCH_URL:-}" ]]; then
  URL=$UVCPP_DB_BENCH_URL
fi

# 相对的 build dir 按当前工作目录解析；找不到时再按仓库根解析（方便在任意目录
# 调用默认值 build-orm-bench）。
if [[ "$BUILD_DIR" != /* ]]; then
  if [[ -d "$PWD/$BUILD_DIR" ]]; then
    BUILD_DIR="$PWD/$BUILD_DIR"
  else
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
  fi
fi
BUILD_DIR_ABS=$(cd -- "$BUILD_DIR" 2>/dev/null && pwd) || die "构建目录不存在：$BUILD_DIR"
BUILD_DIR=$BUILD_DIR_ABS
BIN="$BUILD_DIR/bench/uvcpp_bench_orm"
[[ -x "$BIN" ]] || die "找不到可执行量具：$BIN
先按 bench/orm/README.md 里的命令配置并编译，或改用 --build-dir。"

mkdir -p -- "$OUT_ROOT"
OUT_ROOT=$(cd -- "$OUT_ROOT" && pwd)

# 每次调用一个全新目录。用 mkdir（原子）而不是 test-then-create，两个并发调用
# 也不会拿到同一个名字。
UTC=$(date -u +%Y%m%dT%H%M%SZ)
BASE="$OUT_ROOT/orm-overhead-$UTC"
RUN_DIR="$BASE"
suffix=0
while ! mkdir -- "$RUN_DIR" 2>/dev/null; do
  suffix=$((suffix + 1))
  ((suffix <= 99)) || die "无法在 $OUT_ROOT 下创建新结果目录（尝试了 $BASE-01..99）"
  RUN_DIR=$(printf '%s-%02d' "$BASE" "$suffix")
done

if [[ -z "$URL" ]]; then
  URL="sqlite://$RUN_DIR/bench.sqlite"
fi

export LC_ALL=C

# -----------------------------------------------------------------------------
# 元信息
# -----------------------------------------------------------------------------
CM_CACHE="$BUILD_DIR/CMakeCache.txt"
cache_get() {
  [[ -f "$CM_CACHE" ]] || { printf 'unavailable'; return; }
  local v
  v=$(sed -n "s/^$1:[^=]*=//p" "$CM_CACHE" | head -n 1)
  [[ -n "$v" ]] && printf '%s' "$v" || printf 'unavailable'
}
lscpu_get() {
  lscpu 2>/dev/null | awk -F: -v k="$1" '$1 == k { sub(/^[ \t]+/, "", $2); print $2; exit }'
}

HOSTNAME_V=$(hostname 2>/dev/null || printf 'unavailable')
UNAME_V=$(uname -a 2>/dev/null || printf 'unavailable')
CPU_MODEL=$(lscpu_get 'Model name')
[[ "$CPU_MODEL" == "unavailable" ]] && CPU_MODEL=$(lscpu_get '型号名称')
CPU_COUNT=$(lscpu_get 'CPU(s)')
GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || printf 'unavailable')
LOADAVG=$(cat /proc/loadavg 2>/dev/null || printf 'unavailable')

CXX=$(cache_get CMAKE_CXX_COMPILER)
if [[ "$CXX" != "unavailable" && -x "$CXX" ]]; then
  CXX_VERSION=$("$CXX" --version 2>/dev/null | head -n 1)
else
  CXX_VERSION="unavailable"
fi
CMAKE_VERSION=$(cmake --version 2>/dev/null | head -n 1)
CMAKE_BUILD_TYPE=$(cache_get CMAKE_BUILD_TYPE)
CMAKE_CXX_FLAGS_RELEASE=$(cache_get CMAKE_CXX_FLAGS_RELEASE)

SQLITE_PKGCONFIG=$(pkg-config --modversion sqlite3 2>/dev/null || printf 'unavailable')
SQLITE_HEADER=""
for h in /usr/include/sqlite3.h /usr/local/include/sqlite3.h; do
  if [[ -f "$h" ]]; then
    SQLITE_HEADER=$(awk '/^#define SQLITE_VERSION / { gsub(/"/, "", $3); print $3; exit }' "$h")
    break
  fi
done
[[ -n "$SQLITE_HEADER" ]] || SQLITE_HEADER=$(printf 'unavailable')
SQLITE_LINKED=$(ldd "$BIN" 2>/dev/null | awk '/sqlite/ { print $1 " => " $3; exit }')
[[ -n "$SQLITE_LINKED" ]] || SQLITE_LINKED="static / not a DT_NEEDED"

DB_ENABLED=$(cache_get UVCPP_ENABLE_DB)
DB_SQLITE_ENABLED=$(cache_get UVCPP_ENABLE_DB_SQLITE)
DB_MYSQL_ENABLED=$(cache_get UVCPP_ENABLE_DB_MYSQL)
DB_PGSQL_ENABLED=$(cache_get UVCPP_ENABLE_DB_PGSQL)
DB_ODBC_ENABLED=$(cache_get UVCPP_ENABLE_DB_ODBC)
DB_MONGO_ENABLED=$(cache_get UVCPP_ENABLE_DB_MONGODB)
BUILD_ORM=$(cache_get UVCPP_BUILD_ORM)
BUILD_BENCH=$(cache_get UVCPP_BUILD_BENCH)

GIT_COMMIT=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || printf 'unavailable')
GIT_BRANCH=$(git -C "$REPO_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'unavailable')
GIT_DIRTY=no
if ! git -C "$REPO_ROOT" diff --quiet --ignore-submodules -- 2>/dev/null ||
   ! git -C "$REPO_ROOT" diff --cached --quiet --ignore-submodules -- 2>/dev/null; then
  GIT_DIRTY=yes
fi
GIT_UNTRACKED=$(git -C "$REPO_ROOT" ls-files --others --exclude-standard 2>/dev/null | wc -l | tr -d ' ')

BIN_SHA256=$(sha256sum "$BIN" 2>/dev/null | awk '{print $1}')
[[ -n "$BIN_SHA256" ]] || BIN_SHA256="unavailable"

COMMON_ARGS=(
  "$URL"
  "--label=$LABEL"
  "--samples=$SAMPLES"
  "--warmup=$WARMUP"
  "--pure-ops=$PURE_OPS"
  "--pure-batch=$PURE_BATCH"
  "--pure-warmup=$PURE_WARMUP"
  "--map-batches=$MAP_BATCHES"
  "--map-rows=$MAP_ROWS"
  "--map-warmup=$MAP_WARMUP"
)
RUN_CMD=$(printf '%q ' "$BIN" "${COMMON_ARGS[@]}")
SCRIPT_CMD=$(printf '%q ' "$SCRIPT_DIR/run_orm_overhead.sh" "${ORIG_ARGS[@]}")

{
  printf 'generated_utc\t%s\n' "$UTC"
  printf 'hostname\t%s\n' "$HOSTNAME_V"
  printf 'uname\t%s\n' "$UNAME_V"
  printf 'cpu_model\t%s\n' "$CPU_MODEL"
  printf 'cpu_count\t%s\n' "$CPU_COUNT"
  printf 'cpu_governor\t%s\n' "$GOVERNOR"
  printf 'loadavg_start\t%s\n' "$LOADAVG"
  printf 'compiler\t%s\n' "$CXX"
  printf 'compiler_version\t%s\n' "$CXX_VERSION"
  printf 'cmake_version\t%s\n' "$CMAKE_VERSION"
  printf 'cmake_build_type\t%s\n' "$CMAKE_BUILD_TYPE"
  printf 'cmake_cxx_flags_release\t%s\n' "$CMAKE_CXX_FLAGS_RELEASE"
  printf 'build_dir\t%s\n' "$BUILD_DIR"
  printf 'bench_binary\t%s\n' "$BIN"
  printf 'bench_binary_sha256\t%s\n' "$BIN_SHA256"
  printf 'uvcpp_enable_db\t%s\n' "$DB_ENABLED"
  printf 'uvcpp_enable_db_sqlite\t%s\n' "$DB_SQLITE_ENABLED"
  printf 'uvcpp_enable_db_mysql\t%s\n' "$DB_MYSQL_ENABLED"
  printf 'uvcpp_enable_db_pgsql\t%s\n' "$DB_PGSQL_ENABLED"
  printf 'uvcpp_enable_db_odbc\t%s\n' "$DB_ODBC_ENABLED"
  printf 'uvcpp_enable_db_mongodb\t%s\n' "$DB_MONGO_ENABLED"
  printf 'uvcpp_build_orm\t%s\n' "$BUILD_ORM"
  printf 'uvcpp_build_bench\t%s\n' "$BUILD_BENCH"
  printf 'sqlite_version_pkgconfig\t%s\n' "$SQLITE_PKGCONFIG"
  printf 'sqlite_version_header\t%s\n' "$SQLITE_HEADER"
  printf 'sqlite_linked\t%s\n' "$SQLITE_LINKED"
  printf 'git_commit\t%s\n' "$GIT_COMMIT"
  printf 'git_branch\t%s\n' "$GIT_BRANCH"
  printf 'git_dirty\t%s\n' "$GIT_DIRTY"
  printf 'git_untracked_files\t%s\n' "$GIT_UNTRACKED"
  printf 'url\t%s\n' "$URL"
  printf 'label\t%s\n' "$LABEL"
  printf 'reps\t%s\n' "$REPS"
  printf 'e2e_samples_per_arm\t%s\n' "$SAMPLES"
  printf 'e2e_warmup_per_arm\t%s\n' "$WARMUP"
  printf 'pure_ops_per_arm\t%s\n' "$PURE_OPS"
  printf 'pure_batch\t%s\n' "$PURE_BATCH"
  printf 'pure_warmup_ops_per_arm\t%s\n' "$PURE_WARMUP"
  printf 'map_batches_per_arm\t%s\n' "$MAP_BATCHES"
  printf 'map_rows\t%s\n' "$MAP_ROWS"
  printf 'map_warmup_calls_per_arm\t%s\n' "$MAP_WARMUP"
  printf 'run_dir\t%s\n' "$RUN_DIR"
  printf 'bench_command\t%s\n' "$RUN_CMD"
  printf 'script_command\t%s\n' "$SCRIPT_CMD"
} > "$RUN_DIR/meta.tsv"

{
  printf 'generated_utc: %s\n' "$UTC"
  printf 'run_dir:       %s\n' "$RUN_DIR"
  printf 'url:           %s\n' "$URL"
  printf 'label:         %s\n' "$LABEL"
  printf 'rotations:     %s\n' "$REPS"
  printf 'e2e samples:   %s per arm (warm-up %s)\n' "$SAMPLES" "$WARMUP"
  printf 'pure build:    %s ops per arm in %s-op batches\n' "$PURE_OPS" "$PURE_BATCH"
  printf 'pure map:      %s rows x %s samples per arm\n\n' "$MAP_ROWS" "$MAP_BATCHES"
  printf 'hostname:      %s\n' "$HOSTNAME_V"
  printf 'uname:         %s\n' "$UNAME_V"
  printf 'cpu:           %s (%s CPUs)\n' "$CPU_MODEL" "$CPU_COUNT"
  printf 'cpu governor:  %s\n' "$GOVERNOR"
  printf 'loadavg start: %s\n' "$LOADAVG"
  printf 'compiler:      %s\n' "$CXX_VERSION"
  printf 'cmake:         %s\n' "$CMAKE_VERSION"
  printf 'build type:    %s\n' "$CMAKE_BUILD_TYPE"
  printf 'release flags: %s\n' "$CMAKE_CXX_FLAGS_RELEASE"
  printf 'sqlite:        pkg-config %s / header %s / %s\n' \
    "$SQLITE_PKGCONFIG" "$SQLITE_HEADER" "$SQLITE_LINKED"
  printf 'git:           %s @ %s (dirty=%s, untracked=%s)\n' \
    "$GIT_BRANCH" "$GIT_COMMIT" "$GIT_DIRTY" "$GIT_UNTRACKED"
  printf 'bench binary:  %s\n' "$BIN"
  printf 'sha256:        %s\n' "$BIN_SHA256"
  printf 'bench command: %s\n' "$RUN_CMD"
} > "$RUN_DIR/meta.txt"

if [[ -f "$CM_CACHE" ]]; then
  grep -E '^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS_RELEASE|UVCPP_ENABLE_DB|UVCPP_ENABLE_DB_SQLITE|UVCPP_ENABLE_DB_MYSQL|UVCPP_ENABLE_DB_PGSQL|UVCPP_ENABLE_DB_ODBC|UVCPP_ENABLE_DB_MONGODB|UVCPP_BUILD_ORM|UVCPP_BUILD_BENCH|UVCPP_BUILD_WEB|UVCPP_BUILD_WEBAPP):' \
    "$CM_CACHE" > "$RUN_DIR/build-config.txt" || true
else
  printf 'CMakeCache.txt 不存在：%s\n' "$CM_CACHE" > "$RUN_DIR/build-config.txt"
fi

# -----------------------------------------------------------------------------
# 聚合器（轮转开始前写盘，出问题时也是现场证据）
# -----------------------------------------------------------------------------
cat > "$RUN_DIR/aggregate.awk" <<'AWK'
BEGIN { FS = "\t"; OFS = "\t" }
NR == 1 { next }
{
  run = $1 + 0
  c = $2; arm = $3; unit = $4
  ops = $5 + 0; samples = $6 + 0
  key = c SUBSEP arm
  if (!(key in seen)) {
    seen[key] = 1
    order[++nkeys] = key
    kcase[key] = c
    karm[key] = arm
    kunit[key] = unit
  }
  if (!(c in case_seen)) {
    case_seen[c] = 1
    case_order[++ncase] = c
  }
  ku[c] = unit
  if (arm == "orm") has_orm[c] = 1
  else has_raw[c] = 1

  cnt[key]++
  idx = cnt[key]
  v["min", key, idx] = $7 + 0
  v["mean", key, idx] = $8 + 0
  v["p50", key, idx] = $9 + 0
  v["p99", key, idx] = $10 + 0
  ops_sum[key] += ops
  samples_sum[key] += samples

  rv[c, run, arm, "mean"] = $8 + 0
  rv[c, run, arm, "p50"] = $9 + 0
  rv[c, run, arm, "p99"] = $10 + 0
  runs[run] = 1
  if (run > maxrun) maxrun = run
}
function median(metric, key, n,   i, j, t, a) {
  for (i = 1; i <= n; ++i) a[i] = v[metric, key, i]
  for (i = 2; i <= n; ++i) {
    t = a[i]
    for (j = i - 1; j >= 1 && a[j] > t; --j) a[j + 1] = a[j]
    a[j + 1] = t
  }
  if (n % 2 == 1) return a[(n + 1) / 2]
  return (a[n / 2] + a[n / 2 + 1]) / 2.0
}
function pct(orm, raw) {
  return raw > 0 ? (orm / raw - 1.0) * 100.0 : 0.0
}
END {
  print "case", "arm", "unit", "ops", "samples", "min_us", "mean_us", \
        "p50_us", "p99_us", "ops_per_s" > "agg.tsv"
  for (i = 1; i <= nkeys; ++i) {
    key = order[i]
    n = cnt[key]
    mn = median("min", key, n)
    me = median("mean", key, n)
    p50 = median("p50", key, n)
    p99 = median("p99", key, n)
    q = me > 0 ? 1000000.0 / me : 0
    print kcase[key], karm[key], kunit[key], ops_sum[key], samples_sum[key], \
          mn, me, p50, p99, q > "agg.tsv"
  }

  print "case", "unit", "raw_mean_us", "orm_mean_us", "mean_extra_pct", \
        "raw_p50_us", "orm_p50_us", "p50_extra_pct", "raw_p99_us", \
        "orm_p99_us", "p99_extra_pct", "p99_extra_pct_max", "gate" > "overhead.tsv"
  for (i = 1; i <= ncase; ++i) {
    c = case_order[i]
    if (!(c in has_raw) || !(c in has_orm)) continue
    rk = c SUBSEP "raw"
    ok = c SUBSEP "orm"
    rmean = median("mean", rk, cnt[rk]); omean = median("mean", ok, cnt[ok])
    rp50 = median("p50", rk, cnt[rk]); op50 = median("p50", ok, cnt[ok])
    rp99 = median("p99", rk, cnt[rk]); op99 = median("p99", ok, cnt[ok])
    me_pct = pct(omean, rmean)
    p50_pct = pct(op50, rp50)
    p99_pct = pct(op99, rp99)

    maxp99 = -1e300
    for (r = 1; r <= maxrun; ++r) {
      if (!(r in runs)) continue
      rr = rv[c, r, "raw", "p99"]
      oo = rv[c, r, "orm", "p99"]
      if (rr > 0) {
        x = pct(oo, rr)
        if (x > maxp99) maxp99 = x
      }
    }
    if (maxp99 < -1e299) maxp99 = 0

    gate = "n/a"
    if (ku[c] == "call") {
      # 判据用**聚合后的** p50/p99（各轮中位数）。单轮 p99 异常放在
      # p99_extra_pct_max 里作诊断，不翻转结论 —— 这正是 issue #95 对
      # 噪声机器要求的“交替 A/B、取中位数”。
      worst = p50_pct > p99_pct ? p50_pct : p99_pct
      if (worst <= 15.0) gate = "target_met"
      else if (worst <= 25.0) gate = "within_hard_gate"
      else gate = "exceeds_hard_gate"
    }
    print c, ku[c], rmean, omean, me_pct, rp50, op50, p50_pct, rp99, op99, \
          p99_pct, maxp99, gate > "overhead.tsv"
  }

  print "run", "case", "unit", "raw_mean_us", "orm_mean_us", "mean_extra_pct", \
        "raw_p50_us", "orm_p50_us", "p50_extra_pct", "raw_p99_us", \
        "orm_p99_us", "p99_extra_pct" > "overhead_runs.tsv"
  for (r = 1; r <= maxrun; ++r) {
    if (!(r in runs)) continue
    for (i = 1; i <= ncase; ++i) {
      c = case_order[i]
      if (!(c in has_raw) || !(c in has_orm)) continue
      rmean = rv[c, r, "raw", "mean"]; omean = rv[c, r, "orm", "mean"]
      rp50 = rv[c, r, "raw", "p50"]; op50 = rv[c, r, "orm", "p50"]
      rp99 = rv[c, r, "raw", "p99"]; op99 = rv[c, r, "orm", "p99"]
      print r, c, ku[c], rmean, omean, pct(omean, rmean), rp50, op50, \
            pct(op50, rp50), rp99, op99, pct(op99, rp99) > "overhead_runs.tsv"
    }
  }
}
AWK

cat > "$RUN_DIR/agg_json.awk" <<'AWK'
BEGIN { FS = "\t"; first = 1; printf "[\n" }
NR == 1 { next }
{
  if (!first) printf ",\n"
  first = 0
  printf "    {\"case\":\"%s\",\"arm\":\"%s\",\"unit\":\"%s\",\"ops\":%s,\"samples\":%s,\"min_us\":%s,\"mean_us\":%s,\"p50_us\":%s,\"p99_us\":%s,\"ops_per_s\":%s}", $1, $2, $3, $4, $5, $6, $7, $8, $9, $10
}
END { if (!first) printf "\n"; printf "  ]" }
AWK

cat > "$RUN_DIR/overhead_json.awk" <<'AWK'
BEGIN { FS = "\t"; first = 1; printf "[\n" }
NR == 1 { next }
{
  if (!first) printf ",\n"
  first = 0
  printf "    {\"case\":\"%s\",\"unit\":\"%s\",\"raw_mean_us\":%s,\"orm_mean_us\":%s,\"mean_extra_pct\":%s,\"raw_p50_us\":%s,\"orm_p50_us\":%s,\"p50_extra_pct\":%s,\"raw_p99_us\":%s,\"orm_p99_us\":%s,\"p99_extra_pct\":%s,\"p99_extra_pct_max\":%s,\"gate\":\"%s\"}", $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13
}
END { if (!first) printf "\n"; printf "  ]" }
AWK

cat > "$RUN_DIR/runs_json.awk" <<'AWK'
BEGIN { FS = "\t"; first = 1; printf "[\n" }
NR == 1 { next }
{
  if (!first) printf ",\n"
  first = 0
  printf "    {\"run\":%s,\"exit\":%s,\"wall_s\":%s,\"user_s\":%s,\"sys_s\":%s,\"maxrss_kb\":%s,\"status\":\"%s\",\"results\":\"run-%02d.tsv\",\"log\":\"run-%02d.log\"}", $1, $2, jnum($3), jnum($4), jnum($5), jnum($6), $7, $1, $1
}
function jnum(v) { return (v ~ /^-?[0-9]+(\.[0-9]+)?$/) ? v : "\"unavailable\"" }
END { if (!first) printf "\n"; printf "  ]" }
AWK

# -----------------------------------------------------------------------------
# 轮转
# -----------------------------------------------------------------------------
printf 'run\texit\twall_s\tuser_s\tsys_s\tmaxrss_kb\tstatus\n' > "$RUN_DIR/runs.tsv"
FAILED=0
FAILED_RUN=""

for ((r = 1; r <= REPS; ++r)); do
  rp=$(printf '%02d' "$r")
  log="$RUN_DIR/run-$rp.log"
  err="$RUN_DIR/run-$rp.err"
  results="$RUN_DIR/run-$rp.tsv"
  time_file="$RUN_DIR/run-$rp.time.txt"
  rm -f -- "$results" "$time_file"

  printf '[%d/%d] %s ... ' "$r" "$REPS" "$(basename "$log")"
  start=$(date +%s.%N)
  set +e
  if [[ -x /usr/bin/time ]]; then
    /usr/bin/time -v -o "$time_file" \
      "$BIN" "${COMMON_ARGS[@]}" "--results=$results" >"$log" 2>"$err"
    rc=$?
  else
    "$BIN" "${COMMON_ARGS[@]}" "--results=$results" >"$log" 2>"$err"
    rc=$?
  fi
  set -e
  end=$(date +%s.%N)
  wall=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')

  user_s="unavailable"; sys_s="unavailable"; maxrss_kb="unavailable"
  if [[ -f "$time_file" ]]; then
    user_s=$(awk -F': ' '/User time \(seconds\)/ { print $2; exit }' "$time_file")
    sys_s=$(awk -F': ' '/System time \(seconds\)/ { print $2; exit }' "$time_file")
    maxrss_kb=$(awk -F': ' '/Maximum resident set size/ { print $2; exit }' "$time_file")
    [[ -n "$user_s" ]] || user_s="unavailable"
    [[ -n "$sys_s" ]] || sys_s="unavailable"
    [[ -n "$maxrss_kb" ]] || maxrss_kb="unavailable"
  fi

  if ((rc == 0)) && [[ -s "$results" ]]; then
    status="OK"
    printf 'ok (%ss)\n' "$wall"
  else
    status="FAIL"
    FAILED=1
    FAILED_RUN=$rp
    printf 'FAILED (exit %s, %ss)\n' "$rc" "$wall"
  fi
  printf '%d\t%d\t%s\t%s\t%s\t%s\t%s\n' \
    "$r" "$rc" "$wall" "$user_s" "$sys_s" "$maxrss_kb" "$status" \
    >> "$RUN_DIR/runs.tsv"

  if ((FAILED)); then
    {
      printf 'ORM overhead benchmark FAILED\n\n'
      printf 'run:     %s\n' "$rp"
      printf 'exit:    %s\n' "$rc"
      printf 'command: %s\n\n' "$RUN_CMD"
      printf 'stderr (run-%s.err):\n' "$rp"
      sed 's/^/  /' "$err" 2>/dev/null || true
      printf '\nstdout tail (run-%s.log):\n' "$rp"
      tail -n 25 "$log" 2>/dev/null | sed 's/^/  /' || true
      printf '\n部分结果保存在：%s\n' "$RUN_DIR"
    } > "$RUN_DIR/summary.txt"
    printf '\n失败：第 %d 轮退出码 %d。原始输出保留在 %s\n' "$r" "$rc" "$RUN_DIR" >&2
    exit 1
  fi
done

# -----------------------------------------------------------------------------
# 聚合
# -----------------------------------------------------------------------------
printf 'run\tcase\tarm\tunit\tops\tsamples\tmin_us\tmean_us\tp50_us\tp99_us\tops_per_s\n' \
  > "$RUN_DIR/cases.tsv"
for ((r = 1; r <= REPS; ++r)); do
  rp=$(printf '%02d' "$r")
  tail -n +2 "$RUN_DIR/run-$rp.tsv" \
    | awk -v run="$r" 'BEGIN { FS = OFS = "\t" } { print run, $0 }' \
    >> "$RUN_DIR/cases.tsv"
done

(
  cd "$RUN_DIR"
  awk -f aggregate.awk cases.tsv
)

# -----------------------------------------------------------------------------
# summary.txt
# -----------------------------------------------------------------------------
align() {
  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t'
  else
    cat
  fi
}

{
  cat "$RUN_DIR/meta.txt"
  printf '\n每轮原始明细：\n'
  cat "$RUN_DIR/runs.tsv" | align
  printf '\n每轮 ORM 相对 raw 的额外开销（%%；负值表示 ORM 更快或噪声）：\n'
  cat "$RUN_DIR/overhead_runs.tsv" | align
  printf '\n跨轮聚合（各指标取轮中位数；ops/s 由聚合 mean 反算）：\n'
  cat "$RUN_DIR/agg.tsv" | align
  printf '\nORM 相对 raw 的额外开销（跨轮聚合）：\n'
  cat "$RUN_DIR/overhead.tsv" | align
  printf '\n判据（只对 unit=call 的端到端用例；来自 issue #95）：\n'
  printf '  target_met        p50/p99 额外开销 <= 15%%\n'
  printf '  within_hard_gate  <= 25%%（未达目标，但没有超过硬门禁）\n'
  printf '  exceeds_hard_gate > 25%%（按 issue #95 不得视为通过）\n'
  printf '  n/a               纯构造 / 纯映射，没有端到端百分比门禁\n'
  printf '  gate 取聚合后的 p50_extra_pct 与 p99_extra_pct 的较大值（各轮中位数）。\n'
  printf '  p99_extra_pct_max 是单轮最坏值，只作诊断，不参与 gate：一次调度抖动\n'
  printf '  不应把中位数结论翻成“超标”。\n'
  printf '\nORIGIN（不要写成发布结论）：本机 SQLite + 本机编译配置的一次读数。\n'
  printf '只支持 SQLite；没有验证 MySQL / PostgreSQL / Windows。写路径走 SQLite\n'
  printf '默认 pragma（每条语句 fsync），ORM 的微秒级开销会被磁盘账淹没，读/写\n'
  printf '要分开看。详见 bench/orm/README.md 的 ORM 基准一节。\n'
} > "$RUN_DIR/summary.txt"

# -----------------------------------------------------------------------------
# summary.json
# -----------------------------------------------------------------------------
json_escape() {
  local s=$1
  s=${s//\\/\\\\}
  s=${s//\"/\\\"}
  s=${s//$'\n'/\\n}
  s=${s//$'\r'/\\r}
  s=${s//$'\t'/\\t}
  printf '%s' "$s"
}

meta_json() {
  local first=1 k v esc
  printf '{'
  while IFS=$'\t' read -r k v || [[ -n "$k" ]]; do
    [[ -z "$k" ]] && continue
    esc=$(json_escape "$v")
    if ((first)); then first=0; else printf ','; fi
    printf '\n    "%s": "%s"' "$(json_escape "$k")" "$esc"
  done < "$RUN_DIR/meta.tsv"
  printf '\n  }'
}

{
  printf '{\n'
  printf '  "schema": "uvcpp-orm-overhead/1",\n'
  printf '  "meta": '
  meta_json
  printf ',\n  "runs": '
  awk -f "$RUN_DIR/runs_json.awk" "$RUN_DIR/runs.tsv"
  printf ',\n  "cases": '
  awk -f "$RUN_DIR/agg_json.awk" "$RUN_DIR/agg.tsv"
  printf ',\n  "overhead": '
  awk -f "$RUN_DIR/overhead_json.awk" "$RUN_DIR/overhead.tsv"
  printf '\n}\n'
} > "$RUN_DIR/summary.json"

printf '\n完成。结果目录：%s\n' "$RUN_DIR"
printf '  summary.txt  人读汇总（含额外开销）\n'
printf '  summary.json 机读汇总（schema=uvcpp-orm-overhead/1）\n'
printf '  agg.tsv / overhead.tsv / overhead_runs.tsv  明细\n'
