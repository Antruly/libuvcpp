#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 把 Windows SDK 里那份 **ODBC 头目录**（`<SDK>/Include/<ver>/um`）找出来，打到
# stdout 上（只有路径这一行；诊断全部走 stderr，因为这个脚本的调用方式是
# `ODBC_INC=$(bash .github/scripts/win-odbc-headers.sh)`）。
#
# 给谁用：`release.yml` 的两条 **MSVC** 腿（msvc-x64 / msvc-arm64）。它们要
# `-DUVCPP_DB_ODBC_INCLUDE_DIR=` 指到一份 `sql.h` 上 —— db 的第四个后端（ODBC）
# 从 1.7.2 起进发布包，而它**默认 ON、找不到头只 WARNING 一句就自己关掉**
# （`CMakeLists.txt` 里那段 `message(WARNING)` + `set(... OFF)` 是普通变量赋值 ⇒
# `CMakeCache.txt` 里照旧写着 ON）。所以"这条腿有没有弄到头"这件事**只能**从生成的
# `uvcpp_config.h` 里看出来，而下面这一步必须在"取不到"时**当场红**，不能静默过去
# —— 静默过去就是一个少一个后端的包，而且它会一路走到发版。
#
# 为什么是 SDK 那份、而不是像两条 MinGW 腿那样用 MSYS2 的 unixODBC：
#
#   · 微软的 ODBC 头**本来就是 SDK 的一部分**（`sql.h`/`sqlext.h`/`sqltypes.h`
#     都在 `Include/<ver>/um/` 下），随工具链一起装、版本跟着工具链走，不需要
#     网络、不需要解包、也不引入第二种头来源；
#   · 这份头**已经在这个后端上验过**：wlzaweb 的 CI（`.github/workflows/release-win.yml`）
#     里 libuvcpp 的 ODBC 后端就是在 SDK 头上编出来的，并且连上真 SQL Server
#     （LocalDB + ODBC Driver 17）跑通了业务口径 —— 那套流程里找头的那段逻辑就是
#     下面这段，这次是把它从那边搬过来。
#   · 两条 MinGW 腿不走这条路：SDK 的头是给 MSVC 的（`__declspec`、SAL、`#pragma`），
#     混进 mingw 工具链是拿一整套没人试过的组合去赌。它们用 unixODBC 那份（上游给
#     mingw 编的就是它），运行期反正加载的都是系统 `odbc32.dll`，两种头在这个后端
#     里是等价的（`src/db/uvcpp_db_odbc.cpp` 头部那条"不假设 `SQLWCHAR` 宽度"就是
#     为这件事写的）。
#
# **真正承重的是"挑哪一版 SDK"这一句，别把它改成字典序。** 这是实测出来的，不是
# 推断：`ls` 的字典序会把 `10.0.17763.0`（第一个 compat-only 的 SDK）排在最前，
# 于是 `um` 指向一份老 SDK，而工具箱里那份 `<windows.h>`/`winnt.h` 来自新 SDK ——
# 两边混着用，报出来的是 `oaidl.h` / `propidlbase.h` / `sqltypes.h` 里成百条的
# `C4430` / `C2144`（实测一次是 283 + 166 条），指着 `typedef INT64 SQLLEN;` 这种行，
# 看上去像 ODBC 或 SDK 坏了，其实只是版本对不上。**默认 SDK（VS 生成器自己挑的那份）
# 就是版本号最大的那个**，所以"按版本号取最大"正是"和我们正在用的工具链同一份"。
#
# `-V`（GNU sort 的版本排序）在 Git for Windows 的 `sort` 上有，两条 MSVC 腿的
# `shell: bash` 就是它。
#
# 判据落在**文件在不在**（与 `win-client-headers.sh` / `win-openssl-deps.sh` 同一
# 条规矩），不落在任何命令的退出码上。找不到时把"真实目录长什么样"打出来再退 1 ——
# 镜像升级换 SDK 是常事，那时候要能一眼看出是路径变了还是真没有。
#
# 可选的命令行参数：给一个或多个"搜索根"（默认是两张镜像上的 `Windows Kits/10/Include`）。
# 参数只为**本机自测**存在（在 Linux 上造两棵假树，验证"按版本号取最大"这一条），
# CI 里不传。
# ---------------------------------------------------------------------------
set -u

if [ "$#" -gt 0 ]; then
  ROOTS=("$@")
else
  ROOTS=(
    "/c/Program Files (x86)/Windows Kits/10/Include"
    "/c/Program Files/Windows Kits/10/Include"
  )
fi

cands=""
for root in "${ROOTS[@]}"; do
  [ -d "$root" ] || continue
  for hdr in "$root"/*/um/sql.h; do
    # 没有匹配时 glob 会原样留着（`set -u` 下也不会报），所以这里要判一下。
    [ -f "$hdr" ] || continue
    cands="${cands}$(basename "$(dirname "$(dirname "$hdr")")")	$(dirname "$hdr")
"
  done
done

if [ -z "$cands" ]; then
  echo "::error title=win-no-odbc-headers::在 SDK 里找不到 sql.h —— db 的 ODBC 后端会静默降级（生成头里 UVCPP_DB_ODBC_ENABLE=0），包少一个后端"
  echo "找过的根（这些目录现在长这样）:" >&2
  for root in "${ROOTS[@]}"; do
    echo "--- $root" >&2
    if [ -d "$root" ]; then
      ls -1 "$root" >&2 || true
      echo "    该根下递归命中的 sql.h:" >&2
      find "$root" -name 'sql.h' 2>/dev/null | sed 's/^/      /' >&2 || true
    else
      echo "    （这个目录不存在）" >&2
    fi
  done
  exit 1
fi

# 按版本号取最大（不是字典序 —— 理由见文件头）。`cut -f1` 之后用 `sort -V` 排的是
# 版本号本身，再把对应的那一行取回来；这样同一个版本号出现两次也无所谓。
ver=$(printf '%s' "$cands" | cut -f1 | sort -V | tail -1)
dir=$(printf '%s' "$cands" | awk -F'\t' -v v="$ver" '$1==v {print $2; exit}')

if [ ! -f "$dir/sql.h" ]; then
  echo "::error title=win-odbc-headers-race::挑出来的目录里没有 sql.h：$dir"
  printf '%s' "$cands" >&2
  exit 1
fi

echo "候选（SDK 版本 → um 目录），取版本号最大的那个:" >&2
printf '%s' "$cands" | sed 's/^/    /' >&2
echo "选中: SDK $ver → $dir" >&2

# 路径里有空格与括号（`Program Files (x86)`），所以给 CMake 的是 **Windows 形式**
# 且**带引号**的一份。MSVC 腿的 shell 是 Git Bash（有 cygpath）；万一没有，
# 就退成原样的 POSIX 形式 —— CMake 在 Git Bash 下也认 `/c/...` 这种写法，只是
# 显式给 Windows 形式更稳（也更好在日志里读）。
if command -v cygpath >/dev/null 2>&1; then
  cygpath -m "$dir"
else
  echo "$dir"
fi
