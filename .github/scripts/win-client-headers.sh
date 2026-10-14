#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 在 Windows runner 上把 **MySQL / PostgreSQL 客户端库的头文件** 弄到手。
#
# 为什么需要这一步（这是 1.7.0 才发现的事，不是历史遗留）：
#
#   · `release.yml` 的六条腿从 1.7.0 起都要带 db 模块的**三个**后端，而
#     MySQL/PG 走的是 `UVCPP_DB_RUNTIME_LOAD=ON` —— 符号运行时 `LoadLibraryA`
#     取，**链接行上不出现这两个库**。但"运行时加载"省掉的是**链接**，
#     省不掉**编译**：`MYSQL_BIND`/`MYSQL_FIELD` 这些按值、按布局用到的类型
#     要从真的 `mysql.h` 里读，`PQresultStatus` 那套枚举要从真的 `libpq-fe.h`
#     里读（手抄一份结构体等于赌 ABI，那正是这条路要躲开的东西）。
#
#   · 两条 **MinGW** 腿不缺头：它们本来就用 MSYS2，`mingw-w64-*-libmariadbclient`
#     与 `mingw-w64-*-postgresql` 在 job 的 `install:` 列表里，头落在
#     `$MINGW_PREFIX/include/{mysql,}` 下。
#
#   · 两条 **MSVC** 腿什么都没有：镜像上 MySQL 那一项是 `Install-MysqlCli.ps1`
#     装的**命令行工具**（没有头），而 `windows-11-arm` 那张镜像里
#     **连 PostgreSQL 都没有**（`Windows11-Arm64-Readme.md` 里 `postgres` 零命中）。
#     `windows-2022` 上倒是有一份 PostgreSQL 14，但只有 x64 那一条腿用得上，
#     另一条腿没有 —— 两边各拿一份不同来源的头，等于给"六条腿同一个包"这句话
#     打折扣。所以四条 Windows 腿**共用同一份钉死版本的头**。
#
# 来路：MSYS2 的 `.pkg.tar.zst`。选它有三个理由 ——
#   ① 它是 `mirror.msys2.org` 上的**不可变 URL**，版本号写在文件名里（不是
#      "latest"），今天取到的那一份与打标签那天取到的逐字节相同；
#   ② 里面的头是**上游原样**（MariaDB 的 `mysql.h` 与 PostgreSQL 的 `libpq-fe.h`
#      都由 MSYS2 的构建期生成/拷贝好，不需要我们再跑一次 configure 去造
#      `mariadb_version.h`、`pg_config.h` 这些生成物）；
#   ③ 两条镜像上都有 `zstd`（1.5.7）与 `tar`，不需要装任何东西。
#
# **头是跨编译器可用的，这一点是核过的、不是想当然**（2026-10-14 在本机对这两个包
# 逐条验过）：
#   · `mysql.h` 的包含链只有 `<stdarg.h> <time.h> <sys/types.h> <ctype.h>` 加
#     `mariadb_com.h`/`mariadb_version.h`/`ma_list.h`/`mariadb_ctype.h`/
#     `mariadb_stmt.h`（后四个自身不收任何系统头），`STDCALL` 在 `_WIN32` 下是
#     `__stdcall`、`my_socket` 按 `_WIN64`/`_WIN32` 分支 —— 全是 MSVC 认的写法；
#   · `libpq-fe.h` 只收 `<stdint.h> <stdio.h>` 与 `"postgres_ext.h"`，
#     而 `postgres_ext.h` 也只收 `<stdint.h>` —— PostgreSQL 18 起 `libpq-fe.h`
#     **不再**牵 `pg_config.h`/`pg_config_ext.h`（本机 grep 过它的 include 行），
#     所以没有"为 mingw 生成的那份 `pg_config.h` 被 MSVC 读到"这种事。
#   · 这条库要用的 33 个 MySQL 符号、21 个 libpq 符号，在这个版本的头里**一个不缺**
#     （逐个 grep 过；缺一个的话 `decltype(&::name)` 会当场编不过，也算个兜底）。
#
# 判据落在**文件在不在**，不落在 curl 的退出码上：与 `win-openssl-deps.sh` 同一条
# 规矩 —— 这一步真正要的是"这台机器上有一份能编过的头"，网络抖动、镜像改版都可能
# 让退出码在两个方向上说谎。取不到就 `exit 1`（`::error` 里带上找过哪些 URL），
# 绝不"跳过这一步继续编" —— 那样编出来的包缺两个后端，而它会一路走到发版。
#
# 目录约定：头落在 `C:/client-headers/{mysql,pgsql}`，与 `release.yml` 里两条
# MSVC 腿的 `-DUVCPP_DB_MYSQL_INCLUDE_DIR=` / `-DUVCPP_DB_PGSQL_INCLUDE_DIR=`
# **必须同表**。改这里就要改那里。
# ---------------------------------------------------------------------------
set -u

MIRROR="https://mirror.msys2.org/mingw/mingw64"
MYSQL_PKG="mingw-w64-x86_64-libmariadbclient-3.4.9-1-any.pkg.tar.zst"
PGSQL_PKG="mingw-w64-x86_64-postgresql-18.0-1-any.pkg.tar.zst"

# Git Bash 下 `C:/client-headers` 与 `/c/client-headers` 指的是同一处；分两个变量
# 写是为了不依赖 MSYS2 那套"把看起来像路径的参数自动改写"的行为（那是 msys2-runtime
# 的路径转换，判据不该建在它上面）。
DEST_WIN="C:/client-headers"
DEST="/c/client-headers"

work="$(mktemp -d)" || exit 1
trap 'rm -rf "$work"' EXIT

fetch() {
  # $1 = 包名，$2 = 解出来的子目录名
  local pkg="$1" sub="$2"
  echo "--- 取 $MIRROR/$pkg"
  if ! curl -fsSL --retry 3 --retry-delay 5 -o "$work/$pkg" "$MIRROR/$pkg"; then
    echo "::error title=win-client-headers::取不到 $MIRROR/$pkg（curl 已重试 3 次）"
    return 1
  fi
  mkdir -p "$work/$sub"
  # 不用 `tar --zstd`：Git Bash 里的 tar 是 GNU tar，它把 `--zstd` 交给外部程序时
  # 找的是 PATH 上那个 zstd（镜像上有，1.5.7），但管道写法不依赖那层猜测。
  if ! zstd -d -c "$work/$pkg" | tar -x -C "$work/$sub"; then
    echo "::error title=win-client-headers::解不开 $pkg（zstd 或 tar 那一步失败了）"
    return 1
  fi
  return 0
}

fetch "$MYSQL_PKG" mariadb || exit 1
fetch "$PGSQL_PKG" pgsql || exit 1

rm -rf "$DEST"
mkdir -p "$DEST/mysql" "$DEST/pgsql"

# MySQL：整棵 `include/mysql/` 拷过去（里面有 `mysql.h` 与它 include 的那几个
# `mariadb_*.h`/`ma_*.h`，还有 `mysql/` 子目录 —— `client_plugin.h` 那些要按
# `<mysql/xxx.h>` 找到，少一层就会在编译 `mysql.h` 时报 no such file）。
cp -r "$work/mariadb/mingw64/include/mysql/." "$DEST/mysql/"

# PostgreSQL：**只拷三个头**，不整棵 `include/` 端过去 —— 那一层里还有
# `pg_config.h`/`ecpg_config.h`/`pgtypes*.h` 与整个 `postgresql/server/`，
# 把它们摊进一个 include 目录只会让下次有人 `#include "pg_config.h"` 时
# 拿到一份**为别的构建**生成的配置。这三个是我们实际要的那条链。
for h in libpq-fe.h libpq-events.h postgres_ext.h; do
  if ! cp "$work/pgsql/mingw64/include/$h" "$DEST/pgsql/"; then
    echo "::error title=win-client-headers::$PGSQL_PKG 里没有 $h —— 包内容变了，钉的版本号要重挑"
    exit 1
  fi
done

rc=0
for f in "$DEST/mysql/mysql.h" "$DEST/pgsql/libpq-fe.h"; do
  if [ ! -f "$f" ]; then
    echo "::error title=win-client-headers::解完之后 $f 不在"
    rc=1
  fi
done
if [ "$rc" != "0" ]; then
  exit 1
fi

echo "MySQL 头在 $DEST_WIN/mysql：" "$(grep -m1 'MARIADB_PACKAGE_VERSION ' "$DEST/mysql/mariadb_version.h" | tr -d '\r')"
echo "PostgreSQL 头在 $DEST_WIN/pgsql：libpq-fe.h / libpq-events.h / postgres_ext.h（$(ls "$DEST/pgsql" | wc -l | tr -d ' ') 个文件）"
