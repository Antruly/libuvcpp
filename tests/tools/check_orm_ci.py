#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核 ORM 模型层（issue #89）在 CI 里的三种配置，**读构建产物说话**。

## 为什么要有这条工具

`orm_model_func.cpp` 不连数据库，所以它的"没测"只有一种诚实表达：**用例不存在**。
`tests/functional/CMakeLists.txt` 在 `UVCPP_BUILD_ORM=OFF` 时把目标摘掉 —— 但"摘掉了"
与"编了且过"在 `ctest` 的摘要里可能是两行不同的话（一条不出现、一条 `Passed`），
而 job 的结论里都是**非红**。只跑一次 `ctest` 分不清这两件事：

  * `UVCPP_BUILD_ORM=ON` 但 DB 被强制关掉 ⇒ ORM 跟着关，`test_orm_model_func`
    不注册，`ctest` 照旧 100% 绿；
  * `UVCPP_BUILD_ORM=OFF` 但有人把过滤器正则改坏 ⇒ 用例反而不该在却在（或者
    该在的 `db_sqlite_func` 被一起摘掉），总数少几条，仍然 100% 绿。

所以这一条同时看**生成头、配置日志、用例注册**三处，并且分模式：

  --mode enabled    这一格是 DB=ON + ORM=ON：`UVCPP_ORM_ENABLE=1`、`src/orm/`
                    进了源列表、`test_orm_model_func` 与 `test_db_sqlite_func`
                    都注册了（后者证明 ORM 没把 DB 带坏）。
  --mode disabled   这一格是 DB=ON + ORM=OFF：`UVCPP_ORM_ENABLE=0`、orm 被排除、
                    `test_orm_model_func` **不存在**，而 `test_db_sqlite_func`
                    仍然在（既有 DB 用例零回归）。
  --mode forced-off DB=OFF + ORM=ON 的**从属规则**：配置期必须 warn + 强制关 ORM，
                    生成头是 0，源列表排除 orm，**构建系统里连 ORM 目标都没有**。
                    这一档只判配置，不需要构建。

判据不靠"某一格的 ctest 报了绿"：本脚本自己找那个名字。这样模块被静默关掉时，
它不会跟着一起绿。

用法：
    python3 tests/tools/check_orm_ci.py --mode enabled \\
        --tree build-orm --configure-log ci-logs/orm-configure.log

退出码：0 全过；1 有判据红了；3 前提不满足（**没判**，不是"红了"）。
"""

import argparse
import glob
import os
import re
import subprocess
import sys

# Windows 的 runner 默认按 cp1252 解 stdout，中文判据会打成乱码 —— 红的那几行
# 恰好是最需要看清的。只在能改的时候改（3.7+），失败也不影响判据。
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (ValueError, OSError):
        pass

ORM_TEST = "test_orm_model_func"
DB_SQLITE_TEST = "test_db_sqlite_func"
ORM_ENABLED_MSG = "ORM module enabled (UVCPP_ORM_ENABLE=1)"
ORM_DISABLED_MSG = "ORM module disabled (UVCPP_ORM_ENABLE=0)"
ORM_INCLUDED_MSG = "Including orm module in build"
ORM_EXCLUDED_MSG = "Excluding orm module from build"
ORM_FORCED_OFF_MSG = "已强制关闭 ORM 模块"

RE_DEFINE = re.compile(r"^#\s*define\s+UVCPP_ORM_ENABLE\s+(\S+)\s*$", re.M)

failures = []
premises = []


def fail(what):
    failures.append(what)
    print("  [红] %s" % what)


def ok(what):
    print("  [绿] %s" % what)


def halt(what):
    """前提不满足：这条判据**没判**。它不是红，但也绝不是绿。"""
    premises.append(what)
    print("  [停] %s" % what)


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except (IOError, OSError):
        return None


def run_capture(argv):
    try:
        # 老 python（3.6）也认 `encoding=`；Windows 上尤其不能靠 locale。
        proc = subprocess.run(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, encoding="utf-8",
                              errors="replace")
    except (OSError, ValueError) as exc:
        return None, str(exc)
    return proc.returncode, proc.stdout or ""


def config_macro(tree):
    path = os.path.join(tree, "include", "uvcpp", "uvcpp_config.h")
    text = read_text(path)
    if text is None:
        return None, "找不到生成头 %s" % path
    m = RE_DEFINE.search(text)
    if not m:
        return None, "生成头 %s 里没有 UVCPP_ORM_ENABLE" % path
    return m.group(1), None


def ctest_names(tree):
    rc, out = run_capture(["ctest", "--test-dir", tree, "-N"])
    if rc is None or rc != 0:
        return None, "`ctest --test-dir %s -N` 跑不起来（rc=%s）" % (tree, rc)
    return out, None


def ctest_has(name, text):
    return name in text


def target_present(tree, name):
    """构建系统里有没有名为 `name` 的目标。

    返回 `(True/False/None, 依据)`；None = 两处都读不到，这条**没判**。
    先问 `cmake --build … --target help`（Makefiles / Ninja 都支持），读不到再
    退到生成物里的目标清单 —— 只做**负向**判断，所以"名字没出现"就是没这个目标。
    """
    rc, out = run_capture(["cmake", "--build", tree, "--target", "help"])
    if rc == 0:
        return (name in out), "cmake --build --target help"

    for rel in ("CMakeFiles/TargetDirectories.txt", "build.ninja", "Makefile"):
        p = os.path.join(tree, rel)
        text = read_text(p)
        if text is not None:
            return (name in text), "生成物 %s" % rel

    for pattern in ("*.vcxproj", "*.sln"):
        hits = glob.glob(os.path.join(tree, pattern))
        if hits:
            text = "\n".join(t for t in (read_text(h) for h in hits)
                             if t is not None)
            return (name in text), "生成物 %s" % pattern
    return None, "目标清单与生成物都读不到"


def expect(cond, good, bad):
    if cond:
        ok(good)
    else:
        fail(bad)


def mode_enabled(tree, log):
    macro, err = config_macro(tree)
    if err:
        halt(err)
    else:
        expect(macro == "1", "生成头 UVCPP_ORM_ENABLE=1",
               "生成头 UVCPP_ORM_ENABLE=%s，这一格没在编 ORM" % macro)

    expect(ORM_ENABLED_MSG in log, "配置日志：%s" % ORM_ENABLED_MSG,
           "配置日志里没有 '%s' —— ORM 没被启用" % ORM_ENABLED_MSG)
    expect(ORM_INCLUDED_MSG in log, "配置日志：%s" % ORM_INCLUDED_MSG,
           "配置日志里没有 '%s' —— src/orm/ 没进源列表" % ORM_INCLUDED_MSG)
    expect(ORM_EXCLUDED_MSG not in log, "配置日志没有排除 ORM 的那一句",
           "配置日志里同时有 '%s' —— ORM 被排除了" % ORM_EXCLUDED_MSG)

    tests, err = ctest_names(tree)
    if err:
        halt(err)
    else:
        expect(ctest_has(ORM_TEST, tests),
               "%s 已注册" % ORM_TEST,
               "%s 没注册 —— ORM=ON 却被过滤掉了？" % ORM_TEST)
        expect(ctest_has(DB_SQLITE_TEST, tests),
               "%s 已注册（同一棵树里 DB 没被连累）" % DB_SQLITE_TEST,
               "%s 没注册 —— 开 ORM 把既有 DB 用例挤掉了？" % DB_SQLITE_TEST)


def mode_disabled(tree, log):
    macro, err = config_macro(tree)
    if err:
        halt(err)
    else:
        expect(macro == "0", "生成头 UVCPP_ORM_ENABLE=0",
               "生成头 UVCPP_ORM_ENABLE=%s，ORM=OFF 没生效" % macro)

    expect(ORM_DISABLED_MSG in log, "配置日志：%s" % ORM_DISABLED_MSG,
           "配置日志里没有 '%s'" % ORM_DISABLED_MSG)
    expect(ORM_EXCLUDED_MSG in log, "配置日志：%s" % ORM_EXCLUDED_MSG,
           "配置日志里没有 '%s' —— src/orm/ 没被排除" % ORM_EXCLUDED_MSG)
    expect(ORM_INCLUDED_MSG not in log, "配置日志没有 'Including orm' 那一句",
           "配置日志里出现了 '%s' —— ORM=OFF 却进了源列表" % ORM_INCLUDED_MSG)

    tests, err = ctest_names(tree)
    if err:
        halt(err)
    else:
        expect(not ctest_has(ORM_TEST, tests),
               "%s 不存在（关掉 ORM 时这是正确的）" % ORM_TEST,
               "%s 却注册了 —— ORM=OFF 没生效，或者过滤器正则坏了" % ORM_TEST)
        expect(ctest_has(DB_SQLITE_TEST, tests),
               "%s 仍然注册（既有 DB 用例零回归）" % DB_SQLITE_TEST,
               "%s 不见了 —— 把 ORM 关掉连带摘掉了 DB 用例" % DB_SQLITE_TEST)


def mode_forced_off(tree, log):
    macro, err = config_macro(tree)
    if err:
        halt(err)
    else:
        expect(macro == "0", "生成头 UVCPP_ORM_ENABLE=0（DB 关掉时被强制置 0）",
               "生成头 UVCPP_ORM_ENABLE=%s —— 从属规则没生效" % macro)

    expect(ORM_FORCED_OFF_MSG in log,
           "配置期打了 '%s'" % ORM_FORCED_OFF_MSG,
           "配置日志里没有 '%s' —— 守卫没生效" % ORM_FORCED_OFF_MSG)
    expect(ORM_DISABLED_MSG in log, "配置日志：%s" % ORM_DISABLED_MSG,
           "配置日志里没有 '%s'" % ORM_DISABLED_MSG)
    expect(ORM_EXCLUDED_MSG in log, "配置日志：%s" % ORM_EXCLUDED_MSG,
           "配置日志里没有 '%s' —— 强制关了却没把 src/orm/ 排除" % ORM_EXCLUDED_MSG)
    expect(ORM_INCLUDED_MSG not in log, "配置日志没有 'Including orm' 那一句",
           "配置日志里出现了 '%s' —— 强制关了却还在编 ORM" % ORM_INCLUDED_MSG)

    tests, err = ctest_names(tree)
    if err:
        halt(err)
    else:
        expect(not ctest_has(ORM_TEST, tests),
               "%s 不存在（这一档的正确结果）" % ORM_TEST,
               "%s 却注册了 —— 强制关闭没挡住测试目标" % ORM_TEST)

    present, how = target_present(tree, ORM_TEST)
    if present is None:
        halt("ORM 目标是否存在**没判**：%s" % how)
    else:
        expect(not present,
               "构建系统里没有 %s 这个目标（依据：%s）" % (ORM_TEST, how),
               "构建系统里还有 %s 目标（依据：%s）" % (ORM_TEST, how))


def main():
    ap = argparse.ArgumentParser(
        description="核 ORM 模型层在 CI 里的三种配置（enabled / disabled / forced-off）")
    ap.add_argument("--mode", required=True,
                    choices=["enabled", "disabled", "forced-off"])
    ap.add_argument("--tree", required=True, help="构建目录")
    ap.add_argument("--configure-log", required=True, help="配置期日志")
    args = ap.parse_args()

    print("ORM CI 判据：mode=%s tree=%s" % (args.mode, args.tree))
    print("  日志=%s" % args.configure_log)

    if not os.path.isdir(args.tree):
        print("  [停] 构建目录 %s 不存在 —— 什么都没判" % args.tree)
        return 3
    log = read_text(args.configure_log)
    if log is None:
        print("  [停] 读不到配置日志 %s —— 什么都没判" % args.configure_log)
        return 3

    if args.mode == "enabled":
        mode_enabled(args.tree, log)
    elif args.mode == "disabled":
        mode_disabled(args.tree, log)
    else:
        mode_forced_off(args.tree, log)

    print("\n==== 汇总 ====")
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    if premises:
        print("没判 %d 条（前提不满足，不是绿）：" % len(premises))
        for p in premises:
            print("  - %s" % p)
        return 3
    print("全过（mode=%s）" % args.mode)
    return 0


if __name__ == "__main__":
    sys.exit(main())
