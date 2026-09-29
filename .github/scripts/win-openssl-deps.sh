#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 在 Windows runner 上把一份 CMake 找得到的 OpenSSL 弄到手。
#
# 为什么是一个脚本，而不是三处各自的 `choco install openssl --no-progress`：
# 2026-09-29 20:46 UTC 起，那一行在 windows-latest 与 windows-2022 上都开始
# **恒定**非 0 退出（148 —— 这个码没有查到有出处的解释），而同一份 workflow 在
# 0addc23f 及之前一直是绿的、文件逐字节没改（用 `git diff 0addc23f 699d27e --
# .github/workflows/ci-windows-msvc.yml` 核过是空的）。变的是 runner 侧 /
# 社区源那一边，不是本工程。三处调用点是同一个缺陷，共用一个脚本免得修一处漏两处。
#
# 这一步真正要的不是"choco 退出 0"，而是"这台机器上有一份 CMake 找得到的
# OpenSSL"。choco 的退出码两个方向都会骗人 —— 社区源返 503 时它可能打印
# "Unable to find package 'openssl'" 却退出 0，也可能像现在这样恒非 0 ——
# 所以判据落在**文件在不在**，不落在它的退出码上。
#
# 位置表与 ci-windows-msvc.yml「Copy runtime DLLs」里那段**必须同表**：
# 那边靠它拷 libssl-3-x64.dll / libcrypto-3-x64.dll 到用例旁边。两处一起改。
# （CMake 的 FindOpenSSL 在 MSVC 下同样先搜 `$ENV{PROGRAMFILES}/OpenSSL` ——
# cmake-3.28 的 Modules/FindOpenSSL.cmake:245-262 —— 所以这份 OpenSSL 不进
# PATH 也能被 find_package 找到。）
#
# 退出码：0 = 找得到（路径与版本都打在日志里）；1 = 找不到，且已在注解里给出
# 找过哪几处、choco 的原话是什么（job 日志匿名读不到，注解是唯一能自证的出口）。
# ---------------------------------------------------------------------------
set -u

# **必须是数组，不能是空格分隔的单个字符串。** 这张表里三条路径带空格，写成
# `SSL_DIRS="…"` 再 `for d in $SSL_DIRS` 会被 IFS 按空格切开成 `/c/Program`、
# `Files/OpenSSL/bin` 这种碎片，于是**一次都命不中**，而错误信息里用引号打印出来
# 又长得完全正常。第一版就是这么写的，在 runner 上实测红了一次才看出来
# （本机那次自测的桩件路径里没有空格，恰恰绕过了这个 bug）。
SSL_DIRS=(
  "/c/Program Files/OpenSSL/bin"
  "/c/Program Files/OpenSSL-Win64/bin"
  "/c/Program Files (x86)/OpenSSL/bin"
  "/c/Program Files/OpenSSL-Win32/bin"
)

find_ssl() {
  local d
  for d in "${SSL_DIRS[@]}"; do
    if [ -x "$d/openssl.exe" ]; then
      echo "$d"
      return 0
    fi
  done
  return 1
}

found="$(find_ssl)" || found=""

choco_out=""
if [ -z "$found" ]; then
  # `--yes`：CI 里没有人能回答 choco 的确认提示。release.yml 里 jom 那一行
  # 早就钉了 `-y`，这三处当初漏了 —— 没有它，任何一次"包需要升级/覆盖"都会变成
  # 一次只看环境的偶发红。三次退避重试是社区源抖动的标准对策。
  n=0
  while :; do
    n=$((n + 1))
    rc=0
    choco_out=$(choco install openssl --no-progress --yes 2>&1) || rc=$?
    printf '%s\n' "$choco_out"
    if [ "$rc" = "0" ]; then
      break
    fi
    if [ "$n" -ge 3 ]; then
      break
    fi
    echo "choco install openssl 第 $n 次没成（退出码 $rc），等 $((n * 30)) s 再试"
    sleep $((n * 30))
  done
  found="$(find_ssl)" || found=""
fi

if [ -z "$found" ]; then
  # 注解是这台机器上唯一能自证的出口 —— job 日志匿名读不到
  # （`/actions/jobs/<id>/logs` 是 403），所以 choco 的原话要折进 `::error` 里，
  # 不能只留在日志里。`%` 要先转义成 `%25`，否则注解会被截断。
  #
  # `command -v openssl` 不是判据的一部分（PATH 上有一份 openssl.exe 不等于有头文件
  # 与 .lib），只为了让下一次的注解能自己说清"这台机器上到底有没有"，省一轮猜测。
  echo "::error title=win-openssl-deps::这台 runner 上没有 CMake 找得到的 OpenSSL。找过：$(printf '%s ' "${SSL_DIRS[@]}")。PATH 上的 openssl：$(command -v openssl || echo '(没有)')。choco 最后那次的末 1200 字：$(printf '%s' "${choco_out:-（choco 没留下输出）}" | tr '\n' ' ' | tail -c 1200 | sed 's/%/%25/g')"
  exit 1
fi

"$found/openssl.exe" version
echo "OpenSSL 在这里：$found"
