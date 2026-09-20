#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核「使能宏与 dll 一致」这条契约是不是真的立住了（issue #6 第 3 条）。

三条判据，每条都能当场变成红的：

  1. **ABI 形态的正例**：拿包里的头 + 包里的 dll，用**裸编译器调用**编一个消费方
     （只要 `-I`，**一个 `-D` 都不加**），构造一个 `uvcpp_web_app`，断言
     `connection_count() == 0`。
     只断言"编得过、跑得起来"是**空转**的：头自己自洽也能过。真正的病是
     `uvcpp_web_app` 的成员布局随宏而变（`ssl_ctx_` 在 `http_`/`registry_` 之前），
     布局对不上时内联访问器**编得过、链得过、跑出一个天文数字**。

  2. **反例**：同一条裸编译命令**追加**一个与包冲突的 `-D`，必须**编不过**，且
     报错里要有生成头那句文案（不是级联语法错误）。
     必须走裸调用：走 CMake 时 `target_compile_definitions` 发在 `CMAKE_CXX_FLAGS`
     之后，gcc/clang/MSVC 都取**最后一个** `-D`（MSVC 只给 C4005 警告），
     `#error` 根本不触发 —— 那样反例也是空转的。

  3. **静态**：① 每个含真实模块守卫的公开头，那一行 include 都在**第一个模块守卫
     之前**（"含不含"这种判据会放过插进 `#if` 里的坏版本）；② 源码树里没有同名
     手写副本；③ 包里的生成头与构建树的**逐字节相同**，且与构建树导出的
     `INTERFACE_COMPILE_DEFINITIONS` 取值一致。

判据 ③ 不拿 `CMakeCache.txt` 对：OpenSSL/nghttp2/webapp 在依赖缺失时会被**静默
降级为 OFF**（`CMakeLists.txt` 里 `set(UVCPP_ENABLE_OPENSSL OFF)` /
`set(UVCPP_ENABLE_NGHTTP2 OFF)` / `set(UVCPP_BUILD_WEBAPP OFF)` 那几处 —— 都是
set() 一个普通变量，cache 里仍是 ON），拿 cache 对会造出假失败。导出文件是降级
**之后**的值，与生成头同源。

用法：
    python tests/tools/check_config_contract.py --pkg dist/uvcpp-...-msvc-x64 --tree build-msvc
    ... --cxx cl --config Release            # MSVC
    ... --cxx g++                            # gcc / mingw

退出码 0 全过；1 有判据红了；3 配置不满足（比如包里 webapp 是关的，正例无从谈起
—— 这时**不能**当绿，否则这个门禁在那种 job 上就是个摆设）。
"""

import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

MARKER = "remove your own definition of it"  # 生成头里 #error 的文案
CONFIG_REL = os.path.join("include", "uvcpp", "uvcpp_config.h")

# 生成头里应有的全部宏。少一个，判据 ③ 就不是在核完整的契约。
MACROS = [
    "UVCPP_ENABLE_MEMORY_POOL",
    "UVCPP_NET_ENABLE",
    "UVCPP_WEB_ENABLE",
    "UVCPP_WEBAPP_ENABLE",
    "UVCPP_ZLIB_ENABLE",
    "UVCPP_OPENSSL_ENABLE",
    "UVCPP_NGHTTP2_ENABLE",
    "UVCPP_TRY_WRITE_ENABLE",
    "UVCPP_TRY_WRITE_MIN_BYTES",
]

DEF_RE = re.compile(r"^#\s*define\s+(UVCPP_[A-Z0-9_]+)\s+(\S+)\s*$", re.M)
GUARD_RE = re.compile(
    r"^\s*#\s*(?:if|elif|ifdef|ifndef)\b[^\n]*"
    r"\b(UVCPP_[A-Z0-9_]+_ENABLE|UVCPP_ENABLE_MEMORY_POOL)\b", re.M)
INC_LINE = "#include <uvcpp/uvcpp_config.h>"
PLACEHOLDER_RE = re.compile(r"@[A-Za-z_][A-Za-z0-9_]*@")

failures = []


def fail(what):
    failures.append(what)
    print("  [红] %s" % what)


def ok(what):
    print("  [绿] %s" % what)


# --------------------------------------------------------------------------
# 判据 3
# --------------------------------------------------------------------------

def strip_comments(text):
    """去注释但**保留行号**（换成等量空行），否则行号判据全错。"""
    text = re.sub(r"/\*.*?\*/",
                  lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def public_headers():
    out = []
    for dirpath, _dirs, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if f.endswith(".h"):
                out.append(os.path.join(dirpath, f))
    return sorted(out)


def check_positions():
    """含真实模块守卫的头，必须在第一个守卫之前包含生成头。"""
    checked = 0
    bad = 0
    for path in public_headers():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        with open(path, "rb") as fh:
            raw = fh.read()
        if raw[:3] == b"\xef\xbb\xbf":
            raw = raw[3:]
        text = strip_comments(raw.decode("utf-8", errors="replace"))

        incs = [i for i, line in enumerate(text.splitlines(), 1)
                if line.strip() == INC_LINE]
        guards = [text[:m.start()].count("\n") + 1
                  for m in GUARD_RE.finditer(text)]
        if not guards:
            continue
        checked += 1
        first = min(guards)
        if not incs:
            bad += 1
            fail("%s 有模块守卫却没有 %s" % (rel, INC_LINE))
        elif len(incs) > 1:
            bad += 1
            fail("%s 里 %s 出现了 %d 次" % (rel, INC_LINE, len(incs)))
        elif incs[0] > first:
            bad += 1
            fail("%s 的 %s 在第 %d 行，第一个模块守卫在第 %d 行"
                 "（插进 #if 里了 —— 那个宏这时候还没定义）"
                 % (rel, INC_LINE, incs[0], first))
    # 绿字必须挂在"本判据没记过 fail"上：挂在计数上会在**红之后照样印绿**
    # （之前 parse_export 那条就是这么骗过我的 —— 红一行绿一行，看汇总才知道）。
    if not bad:
        ok("位置：%d 个含模块守卫的公开头，include 都在第一个守卫之前" % checked)
    if checked < 25:
        fail("只有 %d 个含模块守卫的头？扫描判据本身失效了" % checked)

    # src/uvcpp.h 是文档上的消费者入口，单包含它也该拿到宏。
    entry = os.path.join(ROOT, "src", "uvcpp.h")
    with open(entry, "rb") as fh:
        raw = fh.read()
    if INC_LINE.encode() not in raw:
        fail("src/uvcpp.h（消费者单包含入口）里没有 %s" % INC_LINE)
    else:
        ok("src/uvcpp.h 里有 %s" % INC_LINE)


def check_no_handwritten_copy():
    hits = []
    for dirpath, _dirs, files in os.walk(os.path.join(ROOT, "src")):
        for f in files:
            if f == "uvcpp_config.h":
                hits.append(os.path.relpath(os.path.join(dirpath, f), ROOT))
    if hits:
        fail("源码树里有手写的 uvcpp_config.h：%s —— 会静默遮蔽生成的那份"
             % ", ".join(hits))
    else:
        ok("源码树里没有同名手写副本")


def read_values(path):
    with open(path, "rb") as fh:
        raw = fh.read()
    text = raw.decode("utf-8", errors="replace")
    # 只找 `@VAR@` 形状的占位符。裸的 `"@" in text` 会在**每一份**正常生成头上
    # 误报 —— 文件头的 Doxygen 注释里有 `@file` / `@brief`。
    ph = PLACEHOLDER_RE.search(text)
    if ph:
        fail("%s 里还留着未替换的占位符 %s" % (path, ph.group(0)))
    return {m.group(1): m.group(2) for m in DEF_RE.finditer(text)}, raw


def parse_export(tree):
    """从导出集里取 INTERFACE_COMPILE_DEFINITIONS（降级之后的值）。"""
    found = []
    pat = re.compile(r'INTERFACE_COMPILE_DEFINITIONS\s+"([^"]*)"')
    for path in glob.glob(os.path.join(tree, "CMakeFiles", "Export", "*",
                                      "uvcppTargets.cmake")):
        with open(path, "rb") as fh:
            text = fh.read().decode("utf-8", errors="replace")
        for m in pat.finditer(text):
            d = {}
            for item in m.group(1).split(";"):
                if "=" in item:
                    k, v = item.split("=", 1)
                    d[k.strip()] = v.strip()
            # 只要含 UVCPP_ 宏的那些：同一个导出集里还有 nlohmann_json 的
            # 编译定义（带 `$<...>` 生成器表达式），混进来会把"× N 个目标"说多。
            if any(k in d for k in MACROS):
                found.append((os.path.basename(os.path.dirname(path)), d))
    return found


def check_generated_vs_tree(tree, pkg):
    tree_h = os.path.join(tree, CONFIG_REL)
    pkg_h = os.path.join(pkg, CONFIG_REL)
    if not os.path.exists(tree_h):
        fail("构建树里没有 %s —— 先跑一次 cmake" % tree_h)
        return None
    if not os.path.exists(pkg_h):
        fail("包里没有 %s" % pkg_h)
        return None

    tree_vals, tree_raw = read_values(tree_h)
    _pkg_vals, pkg_raw = read_values(pkg_h)
    if tree_raw != pkg_raw:
        fail("包里的生成头与构建树的**不是同一份**（包是旧的/取自另一棵树）")
    else:
        ok("包里的生成头与构建树逐字节相同")

    missing = [m for m in MACROS if m not in tree_vals]
    if missing:
        fail("生成头里缺宏：%s" % ", ".join(missing))
        return tree_vals

    defs = parse_export(tree)
    if not defs:
        fail("构建树里找不到 uvcppTargets.cmake 导出集，判据 ③ 无从谈起")
        return tree_vals
    covered = set()
    bad = 0
    for name, d in defs:
        for mac in MACROS:
            if mac in d:
                covered.add(mac)
                if d[mac] != tree_vals[mac]:
                    bad += 1
                    fail("导出集 %s 里 %s=%s，生成头里是 %s（宏有两份真相源）"
                         % (name, mac, d[mac], tree_vals[mac]))
    # ok 只在**没查出任何不一致**时打：只看覆盖率的话，上面已经红了的行后面
    # 会紧跟着一条"取值一致"的绿，读日志的人正好会被它骗过去。
    if len(covered) != len(MACROS):
        fail("导出集只覆盖 %d/%d 个宏，判据 ③ 是空转的"
             % (len(covered), len(MACROS)))
    elif not bad:
        ok("导出集的编译定义与生成头取值一致（%d 个宏 × %d 个目标）"
           % (len(MACROS), len(defs)))
    return tree_vals


# --------------------------------------------------------------------------
# 判据 1 / 2
# --------------------------------------------------------------------------

# 路径是 `<webapp/...>` 不是 `<uvcpp/...>`：包里的 `include/<模块>/` 是脚本按
# `src/<模块>/` 原样拼的，`include/uvcpp/` 只放 `src/uvcpp/` 那几个 + 生成头。
CONSUMER = r"""
#include <webapp/uvcpp_web_app.h>
#include <cstdio>
int main() {
  uvcpp::uvcpp_web_app app;
  const size_t n = app.connection_count();
  std::printf("connection_count=%zu sizeof=%zu\n", n, sizeof(app));
  return n == 0 ? 0 : 1;
}
"""


def is_msvc(cxx):
    base = os.path.basename(cxx).lower()
    return base.startswith("cl") and "clang" not in base


def compile_cmd(cxx, pkg, work, defs, out, link=True):
    inc = os.path.join(pkg, "include")
    lib = os.path.join(pkg, "lib")
    src = os.path.join(work, "consumer.cpp")
    if is_msvc(cxx):
        cmd = [cxx, "/nologo", "/std:c++14", "/EHsc", "/I" + inc]
        cmd += ["/D" + d for d in defs]
        if not link:
            return cmd + ["/c", src, "/Fo:" + out]
        cmd += [src, "/Fe:" + out]
        return cmd + ["/link", "/LIBPATH:" + lib, "uvcpp.lib"]
    cmd = [cxx, "-std=c++11", "-I" + inc]
    cmd += ["-D" + d for d in defs]
    if not link:
        return cmd + ["-c", src, "-o", out]
    cmd += [src, "-L" + lib, "-luvcpp", "-o", out]
    # MinGW 的 ld 没有 rpath，那边靠 PATH 找 dll（见 runtime_env）。
    if sys.platform.startswith("linux"):
        cmd += ["-Wl,-rpath," + os.path.abspath(lib)]
    return cmd


def run(cmd, cwd, env):
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


def runtime_env(pkg):
    """让消费方加载**包里那个** dll，而不是别处的同名文件。"""
    env = dict(os.environ)
    bindir = os.path.join(pkg, "bin")
    libdir = os.path.join(pkg, "lib")
    env["PATH"] = bindir + os.pathsep + env.get("PATH", "")
    if sys.platform != "win32":
        env["LD_LIBRARY_PATH"] = libdir + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    return env


def check_abi(tree_vals, pkg, work, cxx):
    webapp = tree_vals.get("UVCPP_WEBAPP_ENABLE")
    web = tree_vals.get("UVCPP_WEB_ENABLE")
    if webapp != "1" or web != "1":
        print("  [跳] 包的 WEBAPP=%s / WEB=%s，构造不出 uvcpp_web_app。" % (webapp, web))
        print("       这一档必须在 webapp 打开的 job 上跑，否则这条门禁是个摆设。")
        return 3

    with open(os.path.join(work, "consumer.cpp"), "w", encoding="utf-8",
              newline="\n") as f:
        f.write(CONSUMER)

    exe = os.path.join(work, "consumer.exe" if is_msvc(cxx) else "consumer")
    cmd = compile_cmd(cxx, pkg, work, [], exe)
    rc, out = run(cmd, work, runtime_env(pkg))
    if rc != 0:
        fail("正例编不过（只给了 -I，一个 -D 都没加）：\n%s" % out.strip()[-2000:])
        return 1
    ok("正例编过（裸调用，零个 -D）")

    rc, out = run([exe], work, runtime_env(pkg))
    print("       消费方输出：%s" % out.strip())
    if rc != 0 or "connection_count=0" not in out:
        fail("正例跑出 %s（rc=%d）—— 内联访问器读的偏移与 dll 不是同一套布局"
             % (out.strip(), rc))
        return 1
    ok("connection_count()==0，且链的是包里的 dll")

    # ---- 反例 ----
    # 挑一个与包冲突的值。挑 0/1 开关里第一个，全 1 就把最后一个压成 0。
    switch = [m for m in MACROS
              if m != "UVCPP_TRY_WRITE_MIN_BYTES" and tree_vals.get(m) in ("0", "1")]
    target = None
    for m in switch:
        if tree_vals[m] == "0":
            target = (m, "1")
            break
    if target is None:
        target = (switch[-1], "0")
    conflict = "%s=%s" % target
    print("       反例注入：-D%s（包里是 %s）" % (conflict, tree_vals[target[0]]))

    rc, out = run(compile_cmd(cxx, pkg, work, [conflict],
                              os.path.join(work, "neg.obj"), link=False),
                  work, runtime_env(pkg))
    if rc == 0:
        fail("反例**编过了**：给了冲突的 -D%s，生成头的 #error 没触发"
             "（这条判据是空转的）" % conflict)
        return 1
    if MARKER not in out:
        fail("反例编不过，但报错里没有生成头那句文案 —— 是级联错误，不是我们的 #error：\n%s"
             % out.strip()[-2000:])
        return 1
    ok("反例被生成头的 #error 拦下（且报错文案是它的）")
    return 0


def check_pc_linkable(pkg):
    """包里 `.pc` 指的那个目录，必须真有**它自己那份**可链接的库。

    为什么要单独一条：`-luvcpp` 只要求链接器**找得到**这个名字，找不到才报错 ——
    "能链上"与"链的是包里那份"是两件事。ELF 平台曾把 `libuvcpp.so` 放进 `bin/`，
    于是包里的 `lib/` 是空的；此时**如果构建机上恰好装了一个系统 libuvcpp**，
    `-luvcpp` 会悄悄链上系统那份，正例照跑照绿，而这一档要验的东西（包里那个
    动态库）一个字节都没碰到。所以必须钉住"包里的 `lib/` 有它"。

    这个洞是 Linux 那一档第一次跑出来的（`ld: cannot find -luvcpp`），本机
    MSVC 永远看不见 —— MSVC/MinGW 各有一份导入库落在 `lib/`。
    """
    pc = os.path.join(pkg, "lib", "pkgconfig", "uvcpp.pc")
    if not os.path.exists(pc):
        fail("包里没有 lib/pkgconfig/uvcpp.pc")
        return
    with open(pc, encoding="utf-8") as fh:
        text = fh.read()
    libs = next((ln for ln in text.splitlines() if ln.startswith("Libs:")), "")
    # 我们自己在 package_release.py 里生成这个 .pc，形状是已知的；按它用到的
    # 变量逐个展开即可，不必写一个通用的 .pc 求值器。
    for k, v in {"${pcfiledir}": os.path.join(pkg, "lib", "pkgconfig"),
                 "${prefix}": pkg, "${exec_prefix}": pkg,
                 "${libdir}": os.path.join(pkg, "lib"),
                 "${includedir}": os.path.join(pkg, "include")}.items():
        libs = libs.replace(k, v)
    dirs = [t[2:] for t in libs.split() if t.startswith("-L")]
    names = [t[2:] for t in libs.split() if t.startswith("-l")]
    if not dirs or not names:
        fail(".pc 的 Libs 行里没有 -L/-l：%r" % libs.strip())
        return

    missing = []
    for d in dirs:
        for n in names:
            cands = [n + ".lib", n + ".a", "lib" + n + ".a", "lib" + n + ".dll.a",
                     "lib" + n + ".so", "lib" + n + ".dylib"]
            cands += [os.path.basename(p)
                      for p in glob.glob(os.path.join(d, "lib" + n + ".so.*"))]
            if any(os.path.exists(os.path.join(d, c)) for c in cands):
                break
        else:
            seen = sorted(os.listdir(d)) if os.path.isdir(d) else ["（目录不存在）"]
            missing.append("%s -> %s" % (d, seen[:6]))

    if missing:
        fail(".pc 写的是 -L%s -l%s，但那个目录里没有包自己那份库：%s\n"
             "      链接器这时会去系统目录找同名库 —— 正例照绿，链的却不是包里的"
             % (dirs[0], "/".join(names), "; ".join(missing)))
        return
    ok(".pc 的 -L%s -l%s 指得到包里那份库" % (dirs[0], names[0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pkg", required=True, help="package_release.py 的 stage 目录")
    ap.add_argument("--tree", required=True, help="产出这个包的构建树")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--work", default="", help="临时目录（默认建在系统临时区）")
    args = ap.parse_args()

    pkg = os.path.abspath(args.pkg)
    tree = os.path.abspath(args.tree)
    work = os.path.abspath(args.work) if args.work else os.path.join(
        pkg, "_contract")
    os.makedirs(work, exist_ok=True)
    print("cxx=%s\npkg=%s\ntree=%s\n" % (args.cxx, pkg, tree))

    print("[3] 静态判据")
    check_positions()
    check_no_handwritten_copy()
    check_pc_linkable(pkg)
    vals = check_generated_vs_tree(tree, pkg)
    if vals is None:
        print("\n判据 ③ 拿不到生成头的值，后两条无从跑起")
        return 1

    print("\n[1][2] ABI 正例 / 反例")
    rc = check_abi(vals, pkg, work, args.cxx)

    print("\n==== 汇总 ====")
    if failures:
        print("红 %d 条：" % len(failures))
        for f in failures:
            print("  - %s" % f.splitlines()[0])
        return 1
    print("全过。")
    return rc


if __name__ == "__main__":
    sys.exit(main())
