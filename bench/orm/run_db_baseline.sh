#!/usr/bin/env bash
# =============================================================================
# bench/orm/run_db_baseline.sh — 现有 db 模块的可重复基线量具驱动
#
# 它**只跑现有** bench/uvcpp_bench_db，不改任何测量语义；脚本负责：
#   * 每次调用建一个**新的**结果目录（永不覆盖上一次）；
#   * 轮转 N 次（默认 3），保存每次的原始 stdout / stderr / GNU time 报告；
#   * 记录机器、编译器、CMake 构建类型、SQLite 版本、命令、git 版本；
#   * 用 shell + awk 生成汇总文本、JSON 和 TSV/CSV（不引入 jq / python 依赖）。
#
# 用法（相对路径按**当前工作目录**解析）：
#
#   bench/orm/run_db_baseline.sh \
#       --build-dir build-db-bench \
#       --reps 3 \
#       --out bench/orm/results
#
#   # 指定连接串（也可以用 UVCPP_DB_BENCH_URL 环境变量）
#   bench/orm/run_db_baseline.sh --url 'sqlite:///tmp/bench.sqlite'
#
# 脚本只找 <build-dir>/bench/uvcpp_bench_db，不替你配置或编译；构建命令见
# bench/orm/README.md。
#
# 退出码：0 = 全部轮次跑完且汇总生成成功；1 = 运行/解析失败；2 = 用法错。
#
# 这是一支量具驱动，不是 CI 单元测试：共享 runner 上的数字不可作为发布结论
# （见 bench/orm/README.md）。
# =============================================================================
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)

ORIG_ARGS=("$@")

BUILD_DIR="build-db-bench"
URL=""
REPS=3
OUT_ROOT="bench/orm/results"
LABEL="db-baseline"
ROWS=10000

die() {
  printf '错误：%s\n' "$*" >&2
  exit 2
}

usage() {
  cat <<'EOF'
用法：run_db_baseline.sh [选项]

  -b, --build-dir DIR   CMake 构建目录（默认 build-db-bench；相对当前目录）
  -u, --url URL         数据库连接串（默认取 UVCPP_DB_BENCH_URL；再缺则在
                        本次结果目录里建 sqlite://<run-dir>/bench.sqlite）
  -n, --reps N          轮转次数，>= 1（默认 3）
  -o, --out DIR         结果根目录（默认 bench/orm/results）
  -l, --label NAME      传给量具的 --label（默认 db-baseline）
  -r, --rows N          量具灌入的行数，>= 100（默认 10000）
  -h, --help            显示本帮助

每次调用都会在 --out 下新建 db-baseline-<UTC 时间戳>[-NN]/，不覆盖已有结果。
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
    -r|--rows)
      (($# >= 2)) || die "$1 缺少参数"
      ROWS=$2
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
[[ "$ROWS" =~ ^[0-9]+$ ]] && ((ROWS >= 100)) || die "--rows 必须是 >= 100 的整数：$ROWS"

if [[ -z "$URL" && -n "${UVCPP_DB_BENCH_URL:-}" ]]; then
  URL=$UVCPP_DB_BENCH_URL
fi

# 相对的 build dir 按当前工作目录解析；找不到时再按仓库根解析（方便在任意目录
# 调用默认值 build-db-bench）。
if [[ "$BUILD_DIR" != /* ]]; then
  if [[ -d "$PWD/$BUILD_DIR" ]]; then
    BUILD_DIR="$PWD/$BUILD_DIR"
  else
    BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
  fi
fi
BUILD_DIR=$(cd -- "$BUILD_DIR" 2>/dev/null && pwd) || die "构建目录不存在：$BUILD_DIR"
BIN="$BUILD_DIR/bench/uvcpp_bench_db"
[[ -x "$BIN" ]] || die "找不到可执行量具：$BIN
先按 bench/orm/README.md 里的命令配置并编译，或改用 --build-dir。"

mkdir -p -- "$OUT_ROOT"
OUT_ROOT=$(cd -- "$OUT_ROOT" && pwd)

# 每次调用一个全新目录。用 mkdir（原子）而不是 test-then-create，两个并发调用
# 也不会拿到同一个名字。
UTC=$(date -u +%Y%m%dT%H%M%SZ)
BASE="$OUT_ROOT/db-baseline-$UTC"
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

export LC_ALL=C  # lscpu / time / 数字格式稳定；量具的中文是字面量，不受影响

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
first_nonempty() {
  local v
  for v in "$@"; do [[ -n "$v" ]] && { printf '%s' "$v"; return; }; done
  printf 'unavailable'
}

HOSTNAME_V=$(hostname 2>/dev/null || printf 'unavailable')
UNAME_V=$(uname -a 2>/dev/null || printf 'unavailable')
CPU_MODEL=$(lscpu_get 'Model name')
[[ "$CPU_MODEL" == "unavailable" ]] && CPU_MODEL=$(lscpu_get '型号名称')
CPU_COUNT=$(lscpu_get 'CPU(s)')
THREADS_PER_CORE=$(lscpu_get 'Thread(s) per core')
CORES_PER_SOCKET=$(lscpu_get 'Core(s) per socket')
SOCKETS=$(lscpu_get 'Socket(s)')
CPU_MAX_MHZ=$(lscpu_get 'CPU max MHz')
[[ "$CPU_MAX_MHZ" == "unavailable" ]] && CPU_MAX_MHZ=$(lscpu_get 'CPU 最大 MHz')

GOVERNOR=$(first_nonempty "$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | paste -sd, -)")
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
[[ -n "$SQLITE_HEADER" ]] || SQLITE_HEADER=$(first_nonempty "$SQLITE_PKGCONFIG")
SQLITE_LINKED=$(ldd "$BIN" 2>/dev/null | awk '/sqlite/ { print $1 " => " $3; exit }')
[[ -n "$SQLITE_LINKED" ]] || SQLITE_LINKED="static / not a DT_NEEDED"

SQLITE_FROM_SOURCE=$(cache_get UVCPP_DB_SQLITE_FROM_SOURCE)
DB_ENABLED=$(cache_get UVCPP_ENABLE_DB)
DB_SQLITE_ENABLED=$(cache_get UVCPP_ENABLE_DB_SQLITE)
DB_MYSQL_ENABLED=$(cache_get UVCPP_ENABLE_DB_MYSQL)
DB_PGSQL_ENABLED=$(cache_get UVCPP_ENABLE_DB_PGSQL)
DB_ODBC_ENABLED=$(cache_get UVCPP_ENABLE_DB_ODBC)
DB_MONGO_ENABLED=$(cache_get UVCPP_ENABLE_DB_MONGODB)
BUILD_BENCH=$(cache_get UVCPP_BUILD_BENCH)
BUILD_TESTS=$(cache_get UVCPP_BUILD_TESTS)

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

RUN_CMD=$(printf '%q ' "$BIN" "$URL" "--label=$LABEL" "--rows=$ROWS")
SCRIPT_CMD=$(printf '%q ' "$SCRIPT_DIR/run_db_baseline.sh" "${ORIG_ARGS[@]}")

{
  printf 'generated_utc\t%s\n' "$UTC"
  printf 'hostname\t%s\n' "$HOSTNAME_V"
  printf 'uname\t%s\n' "$UNAME_V"
  printf 'cpu_model\t%s\n' "$CPU_MODEL"
  printf 'cpu_count\t%s\n' "$CPU_COUNT"
  printf 'threads_per_core\t%s\n' "$THREADS_PER_CORE"
  printf 'cores_per_socket\t%s\n' "$CORES_PER_SOCKET"
  printf 'sockets\t%s\n' "$SOCKETS"
  printf 'cpu_max_mhz\t%s\n' "$CPU_MAX_MHZ"
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
  printf 'uvcpp_db_sqlite_from_source\t%s\n' "$SQLITE_FROM_SOURCE"
  printf 'uvcpp_enable_db_mysql\t%s\n' "$DB_MYSQL_ENABLED"
  printf 'uvcpp_enable_db_pgsql\t%s\n' "$DB_PGSQL_ENABLED"
  printf 'uvcpp_enable_db_odbc\t%s\n' "$DB_ODBC_ENABLED"
  printf 'uvcpp_enable_db_mongodb\t%s\n' "$DB_MONGO_ENABLED"
  printf 'uvcpp_build_bench\t%s\n' "$BUILD_BENCH"
  printf 'uvcpp_build_tests\t%s\n' "$BUILD_TESTS"
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
  printf 'rows\t%s\n' "$ROWS"
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
  printf 'seed rows:     %s\n\n' "$ROWS"
  printf 'hostname:      %s\n' "$HOSTNAME_V"
  printf 'uname:         %s\n' "$UNAME_V"
  printf 'cpu:           %s (%s CPUs, %s threads/core, %s cores/socket, %s socket(s), max %s MHz)\n' \
    "$CPU_MODEL" "$CPU_COUNT" "$THREADS_PER_CORE" "$CORES_PER_SOCKET" "$SOCKETS" "$CPU_MAX_MHZ"
  printf 'cpu governor:  %s\n' "$GOVERNOR"
  printf 'loadavg start: %s\n' "$LOADAVG"
  printf 'compiler:      %s\n' "$CXX_VERSION"
  printf 'cmake:         %s\n' "$CMAKE_VERSION"
  printf 'build type:    %s\n' "$CMAKE_BUILD_TYPE"
  printf 'release flags: %s\n' "$CMAKE_CXX_FLAGS_RELEASE"
  printf 'sqlite:        pkg-config %s / header %s / %s\n' "$SQLITE_PKGCONFIG" "$SQLITE_HEADER" "$SQLITE_LINKED"
  printf 'git:           %s @ %s (dirty=%s, untracked=%s)\n' "$GIT_BRANCH" "$GIT_COMMIT" "$GIT_DIRTY" "$GIT_UNTRACKED"
  printf 'bench binary:  %s\n' "$BIN"
  printf 'sha256:        %s\n' "$BIN_SHA256"
  printf 'bench command: %s\n' "$RUN_CMD"
} > "$RUN_DIR/meta.txt"

if [[ -f "$CM_CACHE" ]]; then
  grep -E '^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS_RELEASE|UVCPP_ENABLE_DB|UVCPP_ENABLE_DB_SQLITE|UVCPP_DB_SQLITE_FROM_SOURCE|UVCPP_ENABLE_DB_MYSQL|UVCPP_ENABLE_DB_PGSQL|UVCPP_ENABLE_DB_ODBC|UVCPP_ENABLE_DB_MONGODB|UVCPP_DB_RUNTIME_LOAD|UVCPP_BUILD_BENCH|UVCPP_BUILD_TESTS|UVCPP_BUILD_WEB|UVCPP_BUILD_WEBAPP):' \
    "$CM_CACHE" > "$RUN_DIR/build-config.txt" || true
else
  printf 'CMakeCache.txt 不存在：%s\n' "$CM_CACHE" > "$RUN_DIR/build-config.txt"
fi

# -----------------------------------------------------------------------------
# 解析器（必须在轮转开始前写盘）
# -----------------------------------------------------------------------------
cat > "$RUN_DIR/parse.awk" <<'AWK'
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function isint(s) { return s ~ /^[0-9]+$/ }
function isnum(s) { return s ~ /^-?[0-9]+(\.[0-9]+)?$/ }
function ajoin(a, from, to,   s, i) {
  s = ""
  for (i = from; i <= to; ++i) s = s (i > from ? " " : "") a[i]
  return trim(s)
}
function fjoin(from, to,   s, i) {
  s = ""
  for (i = from; i <= to; ++i) s = s (i > from ? " " : "") $i
  return trim(s)
}
{
  line = $0

  # 第 9 节：卡顿臂。例：
  #   async (via db_async) max gap   1795 us   ticks 1204 / expected 1274 ... batch 1273 ms
  if (line ~ /max gap +[0-9]/ && line ~ / us / && line ~ /ticks +[0-9]/) {
    split(line, a, "max gap ")
    arm = trim(a[1])
    split(trim(a[2]), b, " ")
    gap = b[1]
    split(line, c, "ticks ")
    split(trim(c[2]), d, " ")
    ticks = d[1]
    split(line, e, "expected ")
    split(trim(e[2]), f, " ")
    expected = f[1]
    split(line, g, "batch ")
    split(trim(g[2]), h, " ")
    batch = h[1]
    timeout = (line ~ /watchdog fired/) ? 1 : 0
    printf "S\t%s\t%s\t%s\t%s\t%s\t%s\t%d\n", run, arm, gap, ticks, expected, batch, timeout
    next
  }

  # 第 10 节：并发臂。例：
  #   4 threads / pool(4)  8.5 ms  235096 calls/s   [created 4 / reused 2000 / ...]
  if (line ~ / calls\/s/ && line ~ /^  [0-9]+ threads \//) {
    split(line, a, " ms")
    n = split(trim(a[1]), w, " ")
    ms = w[n]
    name = ajoin(w, 1, n - 1)
    right = trim(a[2])
    split(right, r, " ")
    qps = r[1]
    created = ""; reused = ""
    if (right ~ /created /) {
      split(right, x, "created ")
      split(trim(x[2]), y, " ")
      created = y[1]
      split(right, x2, "reused ")
      split(trim(x2[2]), y2, " ")
      reused = y2[1]
    }
    printf "T\t%s\t%s\t%s\t%s\t%s\t%s\n", run, name, ms, qps, created, reused
    next
  }

  # 整批操作。例：
  #   fetch 10k rows   2.7 ms   3686923 rows/s  (10000 rows)
  if (line ~ / rows\/s/ && line ~ / ms /) {
    split(line, a, " ms")
    n = split(trim(a[1]), w, " ")
    ms = w[n]
    name = ajoin(w, 1, n - 1)
    right = trim(a[2])
    split(right, r, " ")
    rows_s = r[1]
    rows = ""
    if (match(right, /\([0-9]+ rows\)/)) {
      tok = substr(right, RSTART + 1, RLENGTH - 2)
      split(tok, z, " ")
      rows = z[1]
    }
    printf "B\t%s\t%s\t%s\t%s\t%s\n", run, name, ms, rows_s, rows
    next
  }

  # 单次调用统计。名字里可能有空格，所以从右边数字列反推。
  # 带 rows/s 的是 7 个数字尾；不带的是 6 个。
  if (NF >= 8 && isint($NF) && isint($(NF - 1)) && isnum($(NF - 2)) &&
      isnum($(NF - 3)) && isnum($(NF - 4)) && isnum($(NF - 5)) && isint($(NF - 6))) {
    name = fjoin(1, NF - 7)
    printf "C\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", run, name, $(NF - 6), $(NF - 5), \
      $(NF - 4), $(NF - 3), $(NF - 2), $(NF - 1), $NF
    next
  }
  if (NF >= 7 && isint($NF) && isnum($(NF - 1)) && isnum($(NF - 2)) &&
      isnum($(NF - 3)) && isnum($(NF - 4)) && isint($(NF - 5))) {
    name = fjoin(1, NF - 6)
    printf "C\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t0\n", run, name, $(NF - 5), $(NF - 4), \
      $(NF - 3), $(NF - 2), $(NF - 1), $NF
    next
  }
}
AWK

# -----------------------------------------------------------------------------
# 轮转
# -----------------------------------------------------------------------------
TIME_BIN=""
if command -v /usr/bin/time >/dev/null 2>&1 && /usr/bin/time -v true >/dev/null 2>&1; then
  TIME_BIN=/usr/bin/time
fi

printf 'uvcpp db baseline: %s\n' "$RUN_DIR"
printf '  bench: %s\n' "$BIN"
printf '  url:   %s\n' "$URL"
printf '  reps:  %s, rows: %s, label: %s\n' "$REPS" "$ROWS" "$LABEL"
printf '  time:  %s\n\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"

printf 'run\texit_code\tuser_s\tsystem_s\twall_s\tmax_rss_kb\n' > "$RUN_DIR/runs.tsv"

for ((i = 1; i <= REPS; ++i)); do
  tag=$(printf '%02d' "$i")
  log="$RUN_DIR/run-$tag.log"
  err="$RUN_DIR/run-$tag.err"
  time_file="$RUN_DIR/run-$tag.time.txt"
  rc=0

  printf '[%s/%s] run-%s ... ' "$i" "$REPS" "$tag"
  if [[ -n "$TIME_BIN" ]]; then
    "$TIME_BIN" -v -o "$time_file" \
      "$BIN" "$URL" "--label=$LABEL" "--rows=$ROWS" \
      > "$log" 2> "$err" || rc=$?
  else
    "$BIN" "$URL" "--label=$LABEL" "--rows=$ROWS" \
      > "$log" 2> "$err" || rc=$?
    printf '（未找到 GNU time；本轮的 CPU/RSS 不可用）\n' > "$time_file"
  fi

  user_s=""; system_s=""; wall_s=""; maxrss_kb=""
  if [[ -f "$time_file" ]]; then
    user_s=$(awk -F': ' '/User time \(seconds\)/ { print $2; exit }' "$time_file")
    system_s=$(awk -F': ' '/System time \(seconds\)/ { print $2; exit }' "$time_file")
    # GNU time 的 wall 是 [h:]mm:ss.ss；换成秒，保证 summary.json 里是数字。
    wall_s=$(awk -F': ' '
      /Elapsed \(wall clock\)/ {
        n = split($2, a, ":")
        if (n == 2) print a[1] * 60 + a[2]
        else if (n == 3) print a[1] * 3600 + a[2] * 60 + a[3]
        else print $2
        exit
      }' "$time_file")
    maxrss_kb=$(awk -F': ' '/Maximum resident set size/ { print $2; exit }' "$time_file")
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$i" "$rc" "$user_s" "$system_s" "$wall_s" "$maxrss_kb" \
    >> "$RUN_DIR/runs.tsv"

  if ((rc != 0)); then
    printf '失败（exit %s）\n' "$rc"
    {
      printf 'status=failed\n'
      printf 'failed_run=%s\n' "$i"
      printf 'exit_code=%s\n' "$rc"
    } >> "$RUN_DIR/meta.tsv"
    {
      printf '\nFAILED: run-%s exit=%s\n' "$tag" "$rc"
      printf '\n--- %s (stderr) ---\n' "$err"
      sed -n '1,80p' "$err" 2>/dev/null || true
      printf '\n--- %s (stdout tail) ---\n' "$log"
      tail -n 40 "$log" 2>/dev/null || true
    } > "$RUN_DIR/summary.txt"
    printf '\n失败。原始输出保留在：%s\n' "$RUN_DIR" >&2
    exit 1
  fi

  if [[ -s "$err" ]]; then
    printf '（stderr 非空，见 run-%s.err）' "$tag"
  fi
  printf 'ok'
  if [[ -n "$wall_s" ]]; then
    printf '  wall=%ss maxrss=%sKB' "$wall_s" "$maxrss_kb"
  fi
  printf '\n'
done

# -----------------------------------------------------------------------------
# 原始解析：把每轮日志拆成 C/B/S/T 四类记录
# -----------------------------------------------------------------------------
: > "$RUN_DIR/parsed.tsv"
for ((i = 1; i <= REPS; ++i)); do
  tag=$(printf '%02d' "$i")
  awk -v run="$i" -f "$RUN_DIR/parse.awk" "$RUN_DIR/run-$tag.log" >> "$RUN_DIR/parsed.tsv"
done

{
  printf 'run\tcase\tsamples\tmin_us\tmean_us\tp50_us\tp99_us\tqps\trows_per_s\n'
  awk -F'\t' 'BEGIN { OFS="\t" } $1 == "C" { print $2, $3, $4, $5, $6, $7, $8, $9, $10 }' \
    "$RUN_DIR/parsed.tsv"
} > "$RUN_DIR/cases.tsv"

{
  printf 'run\tcase\tms\trows_per_s\trows\n'
  awk -F'\t' 'BEGIN { OFS="\t" } $1 == "B" { print $2, $3, $4, $5, $6 }' \
    "$RUN_DIR/parsed.tsv"
} > "$RUN_DIR/bulk.tsv"

{
  printf 'run\tarm\tmax_gap_us\tticks\texpected\tbatch_ms\ttimeout\n'
  awk -F'\t' 'BEGIN { OFS="\t" } $1 == "S" { print $2, $3, $4, $5, $6, $7, $8 }' \
    "$RUN_DIR/parsed.tsv"
} > "$RUN_DIR/stalls.tsv"

{
  printf 'run\tcase\tms\tcalls_per_s\tcreated\treused\n'
  awk -F'\t' 'BEGIN { OFS="\t" } $1 == "T" { print $2, $3, $4, $5, $6, $7 }' \
    "$RUN_DIR/parsed.tsv"
} > "$RUN_DIR/concurrency.tsv"

if (( $(wc -l < "$RUN_DIR/cases.tsv") <= 1 )); then
  printf '解析失败：原始日志里没有识别到任何单次调用统计\n' >&2
  printf 'status=failed\nreason=no_per_call_samples\n' >> "$RUN_DIR/meta.tsv"
  exit 1
fi

# -----------------------------------------------------------------------------
# 汇总聚合
# -----------------------------------------------------------------------------
cat > "$RUN_DIR/aggregate-cases.awk" <<'AWK'
BEGIN { FS = "\t"; OFS = "\t" }
FNR == 1 { next }
{
  c = $2
  if (!(c in idx)) { idx[c] = ++nc; order[nc] = c }
  i = idx[c]
  k = ++runs[i]
  samples[i] += $3 + 0
  if (k == 1 || $4 + 0 < minv[i]) minv[i] = $4 + 0
  summean[i] += $5 + 0
  p50[i, k] = $6 + 0
  if (k == 1 || $7 + 0 > p99max[i]) p99max[i] = $7 + 0
  sumrows[i] += $9 + 0
}
END {
  print "case", "runs", "samples", "min_us", "mean_us", "p50_us", "p99_us", "qps", "rows_per_s"
  for (i = 1; i <= nc; ++i) {
    r = runs[i]
    for (a = 1; a <= r; ++a) tmp[a] = p50[i, a]
    for (a = 2; a <= r; ++a) {
      v = tmp[a]; b = a - 1
      while (b >= 1 && tmp[b] > v) { tmp[b + 1] = tmp[b]; --b }
      tmp[b + 1] = v
    }
    med = (r % 2) ? tmp[(r + 1) / 2] : (tmp[r / 2] + tmp[r / 2 + 1]) / 2
    mean = summean[i] / r
    qps = (mean > 0) ? 1000000 / mean : 0
    printf "%s\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.0f\t%.0f\n", \
      order[i], r, samples[i], minv[i], mean, med, p99max[i], qps, sumrows[i] / r
  }
}
AWK
awk -f "$RUN_DIR/aggregate-cases.awk" "$RUN_DIR/cases.tsv" > "$RUN_DIR/cases.agg.tsv"

cat > "$RUN_DIR/aggregate-bulk.awk" <<'AWK'
function median(a, n,   t, i, j, v) {
  for (i = 1; i <= n; ++i) t[i] = a[i]
  for (i = 2; i <= n; ++i) {
    v = t[i]; j = i - 1
    while (j >= 1 && t[j] > v) { t[j + 1] = t[j]; --j }
    t[j + 1] = v
  }
  return (n % 2) ? t[(n + 1) / 2] : (t[n / 2] + t[n / 2 + 1]) / 2
}
BEGIN { FS = "\t"; OFS = "\t" }
FNR == 1 { next }
{
  c = $2
  if (!(c in idx)) { idx[c] = ++nc; order[nc] = c }
  i = idx[c]
  k = ++runs[i]
  summs[i] += $3 + 0
  ms[i, k] = $3 + 0
  if (k == 1 || $3 + 0 < minms[i]) minms[i] = $3 + 0
  if (k == 1 || $3 + 0 > maxms[i]) maxms[i] = $3 + 0
  sumrows[i] += $4 + 0
  rowcount[i] = $5
}
END {
  print "case", "samples", "mean_ms", "median_ms", "min_ms", "max_ms", "mean_rows_per_s", "rows"
  for (i = 1; i <= nc; ++i) {
    r = runs[i]
    for (j = 1; j <= r; ++j) t[j] = ms[i, j]
    printf "%s\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%.0f\t%s\n", \
      order[i], r, summs[i] / r, median(t, r), minms[i], maxms[i], sumrows[i] / r, rowcount[i]
  }
}
AWK
awk -f "$RUN_DIR/aggregate-bulk.awk" "$RUN_DIR/bulk.tsv" > "$RUN_DIR/bulk.agg.tsv"

cat > "$RUN_DIR/aggregate-stalls.awk" <<'AWK'
function median(a, n,   t, i, j, v) {
  for (i = 1; i <= n; ++i) t[i] = a[i]
  for (i = 2; i <= n; ++i) {
    v = t[i]; j = i - 1
    while (j >= 1 && t[j] > v) { t[j + 1] = t[j]; --j }
    t[j + 1] = v
  }
  return (n % 2) ? t[(n + 1) / 2] : (t[n / 2] + t[n / 2 + 1]) / 2
}
BEGIN { FS = "\t"; OFS = "\t" }
FNR == 1 { next }
{
  c = $2
  if (!(c in idx)) { idx[c] = ++nc; order[nc] = c }
  i = idx[c]
  k = ++runs[i]
  gap[i, k] = $3 + 0
  ticks[i, k] = $4 + 0
  batch[i, k] = $6 + 0
  if (k == 1 || $3 + 0 < gmin[i]) gmin[i] = $3 + 0
  if (k == 1 || $3 + 0 > gmax[i]) gmax[i] = $3 + 0
  timeouts[i] += $7 + 0
}
END {
  print "arm", "runs", "max_gap_median_us", "max_gap_min_us", "max_gap_max_us", "ticks_median", "batch_median_ms", "timeouts"
  for (i = 1; i <= nc; ++i) {
    r = runs[i]
    for (j = 1; j <= r; ++j) {
      ga[j] = gap[i, j]
      ta[j] = ticks[i, j]
      ba[j] = batch[i, j]
    }
    printf "%s\t%d\t%.0f\t%.0f\t%.0f\t%.0f\t%.1f\t%d\n", \
      order[i], r, median(ga, r), gmin[i], gmax[i], median(ta, r), median(ba, r), timeouts[i]
  }
}
AWK
awk -f "$RUN_DIR/aggregate-stalls.awk" "$RUN_DIR/stalls.tsv" > "$RUN_DIR/stalls.agg.tsv"

cat > "$RUN_DIR/aggregate-concurrency.awk" <<'AWK'
function median(a, n,   t, i, j, v) {
  for (i = 1; i <= n; ++i) t[i] = a[i]
  for (i = 2; i <= n; ++i) {
    v = t[i]; j = i - 1
    while (j >= 1 && t[j] > v) { t[j + 1] = t[j]; --j }
    t[j + 1] = v
  }
  return (n % 2) ? t[(n + 1) / 2] : (t[n / 2] + t[n / 2 + 1]) / 2
}
BEGIN { FS = "\t"; OFS = "\t" }
FNR == 1 { next }
{
  c = $2
  if (!(c in idx)) { idx[c] = ++nc; order[nc] = c }
  i = idx[c]
  k = ++runs[i]
  summs[i] += $3 + 0
  sumqps[i] += $4 + 0
  ms[i, k] = $3 + 0
  qps[i, k] = $4 + 0
  created[i] = $5
  reused[i] = $6
}
END {
  print "case", "runs", "mean_ms", "median_ms", "calls_per_s_mean", "calls_per_s_median", "created_last", "reused_last"
  for (i = 1; i <= nc; ++i) {
    r = runs[i]
    for (j = 1; j <= r; ++j) { mt[j] = ms[i, j]; qt[j] = qps[i, j] }
    printf "%s\t%d\t%.1f\t%.1f\t%.0f\t%.0f\t%s\t%s\n", \
      order[i], r, summs[i] / r, median(mt, r), sumqps[i] / r, median(qt, r), created[i], reused[i]
  }
}
AWK
awk -f "$RUN_DIR/aggregate-concurrency.awk" "$RUN_DIR/concurrency.tsv" > "$RUN_DIR/concurrency.agg.tsv"

# CSV 版（单次调用的原始行 + 聚合行）。名字里没有逗号，直接由 TSV 转。
awk -F'\t' 'BEGIN { OFS="," } { print }' "$RUN_DIR/cases.tsv" > "$RUN_DIR/cases.csv"
awk -F'\t' 'BEGIN { OFS="," } { print }' "$RUN_DIR/cases.agg.tsv" > "$RUN_DIR/summary.csv"

# -----------------------------------------------------------------------------
# summary.txt
# -----------------------------------------------------------------------------
{
  cat "$RUN_DIR/meta.txt"

  printf '\n每轮进程账（GNU time；wall 是整支量具的墙钟）\n'
  awk -F'\t' 'BEGIN { printf "  %-6s %8s %10s %10s %12s %12s\n", "run", "exit", "user(s)", "sys(s)", "wall(s)", "maxRSS(KB)" }
    NR > 1 { printf "  run-%-2s %8s %10s %10s %12s %12s\n", $1, $2, $3, $4, $5, $6 }' \
    "$RUN_DIR/runs.tsv"

  printf '\n单次调用统计（跨 %s 轮聚合；样本数是各轮样本数之和）\n' "$REPS"
  awk -F'\t' 'BEGIN {
      printf "  %-26s %5s %9s %10s %10s %10s %10s %12s\n", "case", "runs", "samples", "min(us)", "mean(us)", "p50(us)", "p99(us)", "calls/s"
    }
    NR > 1 { printf "  %-26s %5s %9s %10.1f %10.1f %10.1f %10.1f %12.0f\n", $1, $2, $3, $4, $5, $6, $7, $8 }' \
    "$RUN_DIR/cases.agg.tsv"

  printf '\n整批操作（事务内 INSERT / 大结果集；每轮内 3 次，共 %s 个样本）\n' "$((REPS * 3))"
  awk -F'\t' 'BEGIN {
      printf "  %-26s %5s %10s %12s %10s %10s %16s\n", "case", "samples", "mean(ms)", "median(ms)", "min(ms)", "max(ms)", "rows/s(mean)"
    }
    NR > 1 { printf "  %-26s %5s %10.1f %12.1f %10.1f %10.1f %16.0f\n", $1, $2, $3, $4, $5, $6, $7 }' \
    "$RUN_DIR/bulk.agg.tsv"

  printf '\n事件循环卡顿（1 ms 定时器；max gap 越小越好，理想 1000 us）\n'
  awk -F'\t' 'BEGIN {
      printf "  %-26s %5s %14s %14s %14s %10s %14s %8s\n", "arm", "runs", "gap_med(us)", "gap_min(us)", "gap_max(us)", "ticks_med", "batch_med(ms)", "timeouts"
    }
    NR > 1 { printf "  %-26s %5s %14.0f %14.0f %14.0f %10.0f %14.1f %8s\n", $1, $2, $3, $4, $5, $6, $7, $8 }' \
    "$RUN_DIR/stalls.agg.tsv"
  awk -F'\t' '
    NR > 1 {
      if ($1 == "async (via vcpp_work)") w = $3 + 0
      if ($1 == "async (via db_async)") f = $3 + 0
    }
    END {
      if (w > 0 && f > 0) printf "  门面 / 手写 work 的 gap 中位数之比：%.2fx（判据：同量级，约 1x）\n", f / w
      else printf "  门面 / 手写 work 的比值：n/a（缺一条臂）\n"
    }' "$RUN_DIR/stalls.agg.tsv"

  printf '\n并发（4 线程 x 500 次点查；跨 %s 轮聚合）\n' "$REPS"
  awk -F'\t' 'BEGIN {
      printf "  %-26s %5s %10s %12s %16s %16s %12s %12s\n", "case", "runs", "mean(ms)", "median(ms)", "calls/s(mean)", "calls/s(med)", "created", "reused"
    }
    NR > 1 { printf "  %-26s %5s %10.1f %12.1f %16.0f %16.0f %12s %12s\n", $1, $2, $3, $4, $5, $6, $7, $8 }' \
    "$RUN_DIR/concurrency.agg.tsv"
  awk -F'\t' '
    NR > 1 {
      if ($1 == "4 threads / 1 client") one = $4 + 0
      if ($1 == "4 threads / 4 clients") four = $4 + 0
      if ($1 == "4 threads / pool(4)") pool = $4 + 0
    }
    END {
      if (four > 0 && pool > 0) printf "  池子 / 各开一条 的 median(ms) 之比：%.2fx（判据：同量级，约 1x）\n", pool / four
      if (one > 0 && pool > 0) printf "  池子 / 共享一条 的 median(ms) 之比：%.2fx\n", pool / one
      if (four <= 0 || pool <= 0) printf "  池子 / 各开一条 的比值：n/a（缺一条臂）\n"
    }' "$RUN_DIR/concurrency.agg.tsv"

  printf '\n口径与注意\n'
  printf '  * min = 各轮 min 的最小值；mean = 各轮 mean 的算术平均；\n'
  printf '    p50 = 各轮 p50 的中位数；p99 = 各轮 p99 的最大值（保守，不是合并样本的 p99）。\n'
  printf '  * 整批/并发两组样本少且受外部负载影响大，因此同时给 mean 与 median；比值判据用 median。\n'
  printf '  * calls/s 由聚合 mean 反算（1e6 / mean_us），与量具每轮的 calls/s 同式。\n'
  printf '  * 当前 bench_db 不输出 p95；需要 p95 时另开量具改动，不在本轮范围内。\n'
  printf '  * SQLite 是进程内后端：没有网络往返，下面的数是「客户端库 + 本库封装 + 本机」的下界。\n'
  printf '  * 第 9 节的 SQLite 慢查询是本机递归 CTE 烧 CPU；异步臂会与 worker 抢 CPU，\n'
  printf '    最大间隔不可能像 MySQL/PG 的服务端睡眠那样干净。\n'
  printf '  * 每轮都在同一个 URL 上 DROP + CREATE 表并重新灌数据；轮次之间不是冷缓存。\n'
  printf '\n原始文件\n'
  printf '  runs/日志   run-01.log .. run-%02d.log（stdout）; run-*.err（stderr）; run-*.time.txt\n' "$REPS"
  printf '  逐轮明细   cases.tsv / cases.csv, bulk.tsv, stalls.tsv, concurrency.tsv, runs.tsv\n'
  printf '  聚合明细   cases.agg.tsv, summary.csv, bulk.agg.tsv, stalls.agg.tsv, concurrency.agg.tsv\n'
} > "$RUN_DIR/summary.txt"

# -----------------------------------------------------------------------------
# summary.json
# -----------------------------------------------------------------------------
cat > "$RUN_DIR/aggregate-json.awk" <<'AWK'
function esc(s,   out, i, c) {
  out = ""
  for (i = 1; i <= length(s); ++i) {
    c = substr(s, i, 1)
    if (c == "\\") out = out "\\\\"
    else if (c == "\"") out = out "\\\""
    else if (c == "\t") out = out "\\t"
    else if (c == "\r") out = out ""
    else if (c == "\n") out = out "\\n"
    else out = out c
  }
  return out
}
function jstr(s) { return "\"" esc(s) "\"" }
function jnum(s) { return (s == "" || s == "unavailable") ? "null" : s }
function jopt(s) { return (s == "") ? "null" : jstr(s) }

FILENAME == meta {
  if (FNR == 1 || $1 == "") next
  if (!($1 in mseen)) { mseen[$1] = 1; morder[++mc] = $1 }
  mval[$1] = $2
  next
}
FILENAME == runs {
  if (FNR == 1) next
  nruns++
  rrun[nruns] = $1; rrc[nruns] = $2; ruser[nruns] = $3; rsys[nruns] = $4
  rwall[nruns] = $5; rrss[nruns] = $6
  next
}
FILENAME == cases {
  if (FNR == 1) next
  nc++
  ccase[nc] = $1; cruns[nc] = $2; cn[nc] = $3; cmin[nc] = $4; cmean[nc] = $5
  cp50[nc] = $6; cp99[nc] = $7; cqps[nc] = $8; crows[nc] = $9
  next
}
FILENAME == bulk {
  if (FNR == 1) next
  nb++
  bcase[nb] = $1; bruns[nb] = $2; bmean[nb] = $3; bmedian[nb] = $4
  bmin[nb] = $5; bmax[nb] = $6; brows[nb] = $7; browcnt[nb] = $8
  next
}
FILENAME == stalls {
  if (FNR == 1) next
  ns++
  sarm[ns] = $1; sruns[ns] = $2; sgmed[ns] = $3; sgmin[ns] = $4
  sgmax[ns] = $5; sticks[ns] = $6; sbatch[ns] = $7; stimeout[ns] = $8
  next
}
FILENAME == conc {
  if (FNR == 1) next
  nt++
  tcase[nt] = $1; truns[nt] = $2; tmean[nt] = $3; tmedian[nt] = $4
  tqpsmean[nt] = $5; tqpsmedian[nt] = $6; tcreated[nt] = $7; treused[nt] = $8
  next
}
END {
  printf "{\n"
  printf "  \"schema\": \"uvcpp-db-baseline/1\",\n"
  printf "  \"status\": \"ok\",\n"
  printf "  \"generated_utc\": %s,\n", jstr(mval["generated_utc"])
  printf "  \"rotations\": %s,\n", jnum(mval["reps"])
  printf "  \"seed_rows\": %s,\n", jnum(mval["rows"])
  printf "  \"meta\": {\n"
  for (i = 1; i <= mc; ++i) {
    k = morder[i]
    printf "    %s: %s%s\n", jstr(k), jstr(mval[k]), (i < mc ? "," : "")
  }
  printf "  },\n"

  printf "  \"runs\": [\n"
  for (i = 1; i <= nruns; ++i) {
    printf "    {\"run\": %s, \"exit_code\": %s, \"user_s\": %s, \"system_s\": %s, \"wall_s\": %s, \"max_rss_kb\": %s}%s\n", \
      jnum(rrun[i]), jnum(rrc[i]), jnum(ruser[i]), jnum(rsys[i]), jnum(rwall[i]), jnum(rrss[i]), \
      (i < nruns ? "," : "")
  }
  printf "  ],\n"

  printf "  \"cases\": [\n"
  for (i = 1; i <= nc; ++i) {
    printf "    {\"case\": %s, \"runs\": %s, \"samples\": %s, \"min_us\": %s, \"mean_us\": %s, \"p50_us\": %s, \"p99_us\": %s, \"qps\": %s, \"rows_per_s\": %s}%s\n", \
      jstr(ccase[i]), jnum(cruns[i]), jnum(cn[i]), jnum(cmin[i]), jnum(cmean[i]), \
      jnum(cp50[i]), jnum(cp99[i]), jnum(cqps[i]), jnum(crows[i]), (i < nc ? "," : "")
  }
  printf "  ],\n"

  printf "  \"bulk\": [\n"
  for (i = 1; i <= nb; ++i) {
    printf "    {\"case\": %s, \"samples\": %s, \"mean_ms\": %s, \"median_ms\": %s, \"min_ms\": %s, \"max_ms\": %s, \"mean_rows_per_s\": %s, \"rows\": %s}%s\n", \
      jstr(bcase[i]), jnum(bruns[i]), jnum(bmean[i]), jnum(bmedian[i]), jnum(bmin[i]), jnum(bmax[i]), \
      jnum(brows[i]), jnum(browcnt[i]), (i < nb ? "," : "")
  }
  printf "  ],\n"

  printf "  \"stalls\": [\n"
  for (i = 1; i <= ns; ++i) {
    printf "    {\"arm\": %s, \"runs\": %s, \"max_gap_median_us\": %s, \"max_gap_min_us\": %s, \"max_gap_max_us\": %s, \"ticks_median\": %s, \"batch_median_ms\": %s, \"timeouts\": %s}%s\n", \
      jstr(sarm[i]), jnum(sruns[i]), jnum(sgmed[i]), jnum(sgmin[i]), jnum(sgmax[i]), \
      jnum(sticks[i]), jnum(sbatch[i]), jnum(stimeout[i]), (i < ns ? "," : "")
  }
  printf "  ],\n"

  printf "  \"concurrency\": [\n"
  for (i = 1; i <= nt; ++i) {
    printf "    {\"case\": %s, \"runs\": %s, \"mean_ms\": %s, \"median_ms\": %s, \"calls_per_s_mean\": %s, \"calls_per_s_median\": %s, \"created_last\": %s, \"reused_last\": %s}%s\n", \
      jstr(tcase[i]), jnum(truns[i]), jnum(tmean[i]), jnum(tmedian[i]), \
      jnum(tqpsmean[i]), jnum(tqpsmedian[i]), jopt(tcreated[i]), jopt(treused[i]), (i < nt ? "," : "")
  }
  printf "  ],\n"

  printf "  \"raw_files\": {\n"
  printf "    \"meta\": \"meta.tsv\",\n"
  printf "    \"build_config\": \"build-config.txt\",\n"
  printf "    \"runs\": \"runs.tsv\",\n"
  printf "    \"per_call\": \"cases.tsv\",\n"
  printf "    \"per_call_csv\": \"cases.csv\",\n"
  printf "    \"per_call_aggregate\": \"cases.agg.tsv\",\n"
  printf "    \"summary_csv\": \"summary.csv\",\n"
  printf "    \"bulk\": \"bulk.tsv\",\n"
  printf "    \"stalls\": \"stalls.tsv\",\n"
  printf "    \"concurrency\": \"concurrency.tsv\"\n"
  printf "  }\n"
  printf "}\n"
}
AWK
awk -F'\t' \
  -v meta="$RUN_DIR/meta.tsv" \
  -v runs="$RUN_DIR/runs.tsv" \
  -v cases="$RUN_DIR/cases.agg.tsv" \
  -v bulk="$RUN_DIR/bulk.agg.tsv" \
  -v stalls="$RUN_DIR/stalls.agg.tsv" \
  -v conc="$RUN_DIR/concurrency.agg.tsv" \
  -f "$RUN_DIR/aggregate-json.awk" \
  "$RUN_DIR/meta.tsv" "$RUN_DIR/runs.tsv" "$RUN_DIR/cases.agg.tsv" \
  "$RUN_DIR/bulk.agg.tsv" "$RUN_DIR/stalls.agg.tsv" "$RUN_DIR/concurrency.agg.tsv" \
  > "$RUN_DIR/summary.json"

printf '\n完成。\n'
printf '  结果目录：%s\n' "$RUN_DIR"
printf '  原始日志：run-01.log .. run-%02d.log（另见 run-*.err / run-*.time.txt）\n' "$REPS"
printf '  汇总文本：%s/summary.txt\n' "$RUN_DIR"
printf '  汇总 JSON：%s/summary.json\n' "$RUN_DIR"
printf '  逐轮明细：%s/cases.tsv（另见 cases.csv / summary.csv）\n' "$RUN_DIR"
