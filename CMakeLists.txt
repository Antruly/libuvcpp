cmake_minimum_required(VERSION 3.20)
project(uvcpp LANGUAGES C CXX)

option(UVCPP_BUILD_TESTS "Build tests" ON)
option(UVCPP_USE_SYSTEM_LIBUV "Prefer system libuv if found" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# 扩展模块选项（默认关闭）
# 内存池在 MinGW-w64 下曾让 test_web_app_* 崩溃，那个根因（emutls 下 TLS 析构的
# 次序，见 RELEASE.md §「内存池在 v1.1.0 里修好了」）**已定位并修复**，两个平台
# 现在都全绿；默认仍是就地关闭，要用得显式 -DUVCPP_BUILD_EXPAND=ON。
#
# "使用者忘了定义 UVCPP_ENABLE_MEMORY_POOL ⇒ 两套分配器静默损坏"那个隐患，
# 现在由生成的 uvcpp_config.h 兜住（见下）：预编译包的使用者不传宏就拿包的值，
# 传了冲突的值直接 #error。所以默认关这件事与那个隐患不再有关系了。
option(UVCPP_BUILD_EXPAND "Build expand module (memory pool, page heap)" OFF)

# =========================================================================
# 静态运行时
# =========================================================================
# 发布产物要求动态库除了系统自带库之外**不依赖任何东西** —— 使用者不该为了跑它
# 去装 MinGW 运行时或某个 libstdc++。之前这条只靠"记得传
# -DCMAKE_SHARED_LINKER_FLAGS=-static"维持，属于口头传承：漏了就得到一个能编、
# 能在开发机上跑、但换台机器就起不来的 .dll/.so。这里把它变成显式开关。
#
# MSVC 无涉（它用 /MD，发布包里自带 vcruntime/msvcp）。
option(UVCPP_STATIC_RUNTIME
  "Statically link the compiler runtime (libgcc/libstdc++) into the library" OFF)

if(UVCPP_STATIC_RUNTIME AND MINGW)
  # -static 是主开关；-static-libgcc / -static-libstdc++ 再钉一遍，避免某条
  # 链接路径上 -static 没生效时静默退化回动态。系统库（ws2_32 等）没有静态
  # 版本，ld 会自动回落到导入库 —— 那是想要的：它们本来就是 Windows 自带的。
  foreach(_var CMAKE_SHARED_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS)
    string(APPEND ${_var} " -static -static-libgcc -static-libstdc++")
  endforeach()
  message(STATUS "MinGW static runtime: ${CMAKE_SHARED_LINKER_FLAGS}")
elseif(UVCPP_STATIC_RUNTIME AND UNIX AND NOT APPLE)
  # Linux 上 -static 对共享库不适用（会把 -lc 之类也静态化，多半直接链不过），
  # 但把 libgcc/libstdc++ 静态化正是想要的：这样 .so 只依赖 libc。
  foreach(_var CMAKE_SHARED_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS)
    string(APPEND ${_var} " -static-libgcc -static-libstdc++")
  endforeach()
  message(STATUS "Static runtime: ${CMAKE_SHARED_LINKER_FLAGS}")
endif()

# =========================================================================
# 网络模块和 Web 模块开关
# =========================================================================
option(UVCPP_BUILD_NET  "Build net module (TCP/UDP client/server)"  ON)
option(UVCPP_BUILD_WEB  "Build web module (HTTP/WebSocket)"         OFF)

# uv_try_write 快速路径：套接字一次吃得下整条待发字节时，省掉"拷进自有缓冲"
# 那一份用户态拷贝（吃不下就退回原路径，最坏等于不开）。默认开。
option(UVCPP_ENABLE_TRY_WRITE "Try uv_try_write before copying into a write buffer" ON)
# 试发的**最小长度**（字节）：小于它的写直接走原来的路径，连试都不试。
# 快路径每笔写固定多一次系统调用（整条吃下时那次 uv_write 走 0 长度，libuv
# 两个平台上都不对 0 长度短路），换来的是省掉一份 len 字节的拷贝 —— 小报文上
# 这笔是亏的。零点在 16 KiB 附近而且就在那儿来回翻，所以默认取 32 KiB 留余量。
# 详细的账见 src/net/uvcpp_tcp_client.cpp 里那段注释。
set(UVCPP_TRY_WRITE_MIN_BYTES 32768 CACHE STRING
    "Min payload size (bytes) for the try_write fast path")
# webapp 是建在 web 之上的应用框架层（路由/中间件/请求响应封装/日志）。
# 它依赖 web，所以 web 关掉时它也会被强制关掉。
option(UVCPP_BUILD_WEBAPP "Build web application framework (router/middleware/log)" OFF)

# WSDL/SOAP 模块。它建在 webapp 之上 —— 发布 WSDL 与派发 SOAP 都走 webapp 的路由，
# 所以 webapp 关掉时它会被强制关掉（与 webapp⇒web 同形，见下面那段）。
#
# 依赖：pugixml（XML 解析）。**它是私有依赖** —— `<pugixml.hpp>` 只出现在
# `src/wsdl/uvcpp_wsdl_pugixml.h` 这一个**不装出去**的头里，公开头一个 pugi 类型
# 都不出现，所以使用者既不需要它的头也不需要它的库（与 nghttp2 同一种做法，
# 见 src/http2/ 的 pimpl 说明）。
option(UVCPP_ENABLE_WSDL "Enable WSDL/SOAP module (XML via pugixml, needs UVCPP_BUILD_WEBAPP)" OFF)

# QUIC 传输层 —— net 层的一条**链路协议**，为后面的 HTTP/3 铺路。
#
# 后端是 **ngtcp2**，位置与 nghttp2 之于 HTTP/2 完全相同：协议栈归它，本库只做
# 事件循环、缓冲与生命周期的接线。它也**只经 TLS 的 ALPN 协商使用** —— QUIC 本身
# 就建在 TLS 1.3 上，没有"明文 QUIC"这回事 —— 所以开启它必须同时开启 OpenSSL，
# 而且那份 OpenSSL 必须**带 QUIC API**（mainline ≥ 3.2）。3.0/3.1 没有
# `SSL_set_quic_tls_cbs`，libngtcp2 在它自己的 configure 期就会 FATAL_ERROR。
# 两条都由下面 QUIC 那一节在配置期检查并**强制关闭 + 出声**，不留一个"能配置、
# 链不上、跑不起来"的组合。
#
# 默认 OFF。**1.4.1 里它是一条真能通信的链路协议**：握手、开流、双向收发、连接
# 关闭与空闲超时都通了，`tests/functional/quic_*_func.cpp` 在跑真实的两端点。
# 哪几件事**没做**（0-RTT、迁移、版本协商、无状态重置……）见 doc/quic-guide.md 的
# "§4 真实现"那张表 —— 那张表是这一节唯一的权威，别的地方不要重复它。
#
# 它**不属于 web 层**，所以刻意不放在下面那组"Web 子开关"里：那组的标题写着
# "仅在 UVCPP_BUILD_WEB=ON 时有效"，而 QUIC 在 web 关闭时照样要能用（它住在
# net 层）。它读的是 `UVCPP_ENABLE_OPENSSL`，那个开关虽然在下面那组里声明，
# 但本文件里读它的守卫都在声明**之后**，顺序没问题。
option(UVCPP_ENABLE_QUIC
  "Enable QUIC transport via ngtcp2 (net layer). Needs UVCPP_ENABLE_OPENSSL=ON and an OpenSSL >= 3.2 with the QUIC API" OFF)

# HTTP/3 —— **web 层**的一条传输，架在上面的 QUIC 之上。
#
# 位置与 nghttp2 之于 HTTP/2 完全相同：协议语义归 **nghttp3**（QPACK、帧、流
# 状态机），本库只做"字节从哪来往哪去"的接线与请求/响应的翻译。所以它有两个
# **硬依赖**，下面 HTTP3 那一节会在配置期检查：
#
#   1. **QUIC** —— h3 没有别的承载。而 QUIC 自己已经强制了 OPENSSL，所以
#      HTTP3 ⇒ QUIC ⇒ OPENSSL 这一串是传递的，本文件不再重复检查 SSL。
#   2. **WEB** —— 它是 web 层的传输，接在 `uvcpp_http_client` /
#      `uvcpp_http_server` 上；没有 web 层，这一层建出来没有调用方。
#
# 两条都不满足时**强制关闭 + 出声**（照 QUIC 那一套），不留"能配置、跑不起来"
# 的组合。
#
# 默认 OFF，而且**关掉时产出的二进制与不开这一项时逐字节相同** —— `src/http3/`
# 整个不参与编译，web 层里那些 h3 块全在 `#if UVCPP_HTTP3_ENABLE` 里。h1/h2 跑
# TCP、h3 跑 UDP，两条 socket 不相交，所以"接进 web 层"不等于"往 h1 的路上加
# 东西"；那张逐函数的对照表在 doc/http3-guide.md 里。
option(UVCPP_ENABLE_HTTP3
  "Enable HTTP/3 via nghttp3 (web layer, over the QUIC transport). Needs UVCPP_ENABLE_QUIC=ON and UVCPP_BUILD_WEB=ON" OFF)

# C ABI（`src/capi/`）—— 给 C / C# P-Invoke / Rust FFI 用的一层**纯 C**门面。
#
# 与上面每一个开关的**种类都不同**：那些开关决定"编进哪一个模块"，这一个决定
# "要不要给已经编进去的模块再多一套 C 入口"。所以：
#
#   * 它不引入任何第三方依赖（这一层是薄包装，`uvcpp_c_*.cpp` 只 include 本库
#     自己的头），因此**没有** FetchContent 段，也没有 link 那一段；
#   * 它不改动任何既有的 C++ 导出：`UVCPP_API` 那一套（`uvcpp_export.h`）一个
#     字节都不动，C 面自己有一枚 `UVCPP_C_API`（见 `src/capi/uvcpp_c_common.h`）；
#   * 关掉时产出与"没有这一项"逐字节相同 —— `src/capi/` 整个不参与编译，装出去
#     的包里也不会出现 `include/capi/`。
#
# 默认 OFF。**但它是一个"要么全给"的开关**：C 面覆盖 net / web / webapp /
# http2 / http3 / quic，少一个模块就会有一批句柄类型根本不存在，所以下面那两条
# 守卫把"少模块"的组合直接强制关闭 —— 这与 nghttp2/http3 那两条是同一套做法。
#
# 发布与 `full` 那一格都会打开它（见 RELEASE / .github/workflows/）：
# "支持导出 C 类型接口"是这一版的一条发布判据，不是可选项。
option(UVCPP_ENABLE_CAPI
  "Export the C ABI surface (src/capi/, pure C headers for C#/FFI). Needs UVCPP_BUILD_NET=ON and UVCPP_BUILD_WEB=ON" OFF)

# 可运行示例（examples/）。默认关：它是给使用者看的，不进库本身。
option(UVCPP_BUILD_EXAMPLES "Build the examples in examples/" OFF)

# 压测靶场（bench/）。默认关，且**不进库、不进 CI**：它是量具，不是功能。
# 单开它的理由见 doc/benchmark-rig.md —— 简言之，样例里的服务每个请求都写访问日志，
# 拿它当靶场量到的是日志的价钱；这个靶场默认一个中间件都不装。
option(UVCPP_BUILD_BENCH "Build the benchmark range (bench/)" OFF)

# =========================================================================
# Web 子开关 — 仅在 UVCPP_BUILD_WEB=ON 时有效
# =========================================================================
option(UVCPP_ENABLE_ZLIB    "Enable zlib (gzip, WebSocket Per-Message Deflate)"  OFF)
option(UVCPP_ENABLE_OPENSSL "Enable OpenSSL (HTTPS + WSS)"                     OFF)
option(UVCPP_ENABLE_NGHTTP2 "Enable HTTP/2 via nghttp2 (needs UVCPP_ENABLE_OPENSSL; TLS+ALPN only, no h2c)" OFF)

# =========================================================================
# 依赖库版本覆盖
# =========================================================================
set(LLHTTP_VERSION  "release/v9.2.0" CACHE STRING "llhttp git tag to fetch")
set(ZLIB_VERSION    "v1.3.1"         CACHE STRING "zlib git tag to fetch")
set(OPENSSL_VERSION "openssl-3.4.0"  CACHE STRING "OpenSSL git tag to fetch")
set(NGHTTP2_VERSION "v1.70.0"        CACHE STRING "nghttp2 git tag to fetch")
set(NGTCP2_VERSION  "v1.25.0"        CACHE STRING "ngtcp2 git tag to fetch")
# 仓库在 `https://github.com/ngtcp2/nghttp3.git` —— 组织是 **ngtcp2**，不是
# nghttp2。nghttp3 与 nghttp2 同源（同一个作者、同一套代码风格），但它是为了
# QUIC 重写的另一个库：没有 TCP 那套帧层，多了 QPACK，API 完全不同。
set(NGHTTP3_VERSION "v1.18.0"        CACHE STRING "nghttp3 git tag to fetch")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC：显式指定 /utf-8，避免按系统代码页(936/GBK)误读源码，导致中文注释末字节
# 吞掉换行符、把下一行代码并进注释（表现为“sock/all/hdrEnd 未声明”等诡异报错）。
#
# 实测（2026-09-21）src/ 下 99 个 .h 里 97 个带 BOM，另 2 个是**纯 ASCII**
# （expand/uvcpp_memory_pool_span.h、expand/uvcpp_page_allocator.h）—— 也就是说
# "含非 ASCII 又没 BOM"的头现在是 **0 个**，当年那 22 个已经全部补上了。
# 带 BOM 的那些本来就不靠这个开关。但这个开关是**目录作用域**的，
# 既不进导出集，也到不了预编译包的消费者，所以**新加的**头若含非 ASCII 又忘了
# BOM，MSVC 使用者依然会中招：那道下限只能由打包脚本兜住
# （tests/tools/package_release.py 的 copy_header）。
if(MSVC)
  add_compile_options(/utf-8)
endif()

message(STATUS "UVCPP version (from header will be printed at configure)")

# try find system libuv first
find_path(LIBUV_INCLUDE_DIR uv.h)
find_library(LIBUV_LIB NAMES uv libuv)

include(FetchContent)
# Option to build libuv from source (via FetchContent). Default OFF.
option(UVCPP_BUILD_LIBUV_FROM_SOURCE "Fetch and build libuv from source via FetchContent" OFF)
# Default libuv git tag/branch to fetch when building from source.
# Limit default to v1.51.0 (highest supported by this wrapper). Users may override.
set(LIBUV_VERSION "v1.51.0" CACHE STRING "libuv git tag/branch to fetch when using FetchContent")

# JSON 后端版本（仅 webapp 需要）。v3.11.x 是支持 C++11 的最后一条线，
# 上游从 v3.12 起要求 C++11 之外的特性。
set(NLOHMANN_JSON_VERSION "v3.11.3" CACHE STRING "nlohmann/json tag to fetch when building webapp")

# WSDL/SOAP 的 XML 后端版本（仅 UVCPP_ENABLE_WSDL 需要）。v1.14 是 C++11 兼容的
# 一档；再往上的版本对编译器/标准的要求要单独核一遍才升。
set(PUGIXML_VERSION "v1.14" CACHE STRING "pugixml tag to fetch when building the wsdl module")

# If user requests building libuv from source, or system libuv not found and user didn't opt to use system
if (UVCPP_BUILD_LIBUV_FROM_SOURCE OR NOT LIBUV_INCLUDE_DIR OR NOT LIBUV_LIB OR NOT UVCPP_USE_SYSTEM_LIBUV)
  message(STATUS "Fetching libuv via FetchContent (tag=${LIBUV_VERSION})")
  FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG ${LIBUV_VERSION}
  )
  FetchContent_MakeAvailable(libuv)
  # When fetched, prefer building/using the libuv target provided by libuv CMake
  set(LIBUV_INCLUDE_DIR ${libuv_SOURCE_DIR}/include)
  set(LIBUV_LIB uv)
endif()

# =========================================================================
# Web 模块依赖下载
# =========================================================================
if(UVCPP_BUILD_WEB)
  # -------------------------------------------------------------------
  # llhttp — HTTP 解析器, WEB=ON 时必须下载
  # -------------------------------------------------------------------
  # llhttp 只在 `BUILD_SHARED_LIBS` 或它自己的 `BUILD_STATIC_LIBS` 为 ON 时才建
  # `llhttp::llhttp` 这个别名（_local_deps/llhttp/CMakeLists.txt:82-100），而它的
  # BUILD_STATIC_LIBS 默认是 OFF。少了这句，`BUILD_SHARED_LIBS=OFF` 的树上配置阶段
  # 就会死在下面第 590 行附近的 `target_link_libraries(uvcpp ... llhttp::llhttp)`。
  # 必须在 FetchContent_MakeAvailable 之前：llhttp 里用的是 option()，缓存里已经
  # 有这条时 option 不会再动它。
  # 这条以前是靠手工在缓存里设 BUILD_STATIC_LIBS=ON 绕过去的 —— 于是「静态链依赖」
  # 这个配置从零复现不出来，发布产物成了不可复现的。
  if(NOT BUILD_SHARED_LIBS)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "Build static libraries (.lib/.a)" FORCE)
  endif()

  message(STATUS "Fetching llhttp via FetchContent (tag=${LLHTTP_VERSION})")
  FetchContent_Declare(
    llhttp
    GIT_REPOSITORY  https://github.com/nodejs/llhttp.git
    GIT_TAG         ${LLHTTP_VERSION}
    GIT_SHALLOW     TRUE
  )
  FetchContent_MakeAvailable(llhttp)
  message(STATUS "llhttp integrated (tag=${LLHTTP_VERSION})")

  # -------------------------------------------------------------------
  # zlib
  # -------------------------------------------------------------------
  if(UVCPP_ENABLE_ZLIB)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
      message(STATUS "Using system zlib ${ZLIB_VERSION_STRING}")
    else()
      message(STATUS "Fetching zlib via FetchContent (tag=${ZLIB_VERSION})")
      FetchContent_Declare(
        zlib
        GIT_REPOSITORY  https://github.com/madler/zlib.git
        GIT_TAG         ${ZLIB_VERSION}
        GIT_SHALLOW     TRUE
      )
      # 这里**不能**用 FetchContent_MakeAvailable：它等价于
      # add_subdirectory(<src> <bin>)，会把 zlib 自己的 install 规则一并登记。
      # 而 zlib 的规则用的是**配置期**的绝对前缀（本机是
      # `C:/Program Files (x86)/uvcpp`，与 `cmake --install --prefix` 无关），
      # 症状有两个，都实测过：
      #   1. `cmake --install <tree> --prefix X` 除了装到 X，还会把
      #      zlib.h / zconf.h / zlib.lib / zlibstatic.lib / zlib1.dll 装进那个
      #      绝对目录 —— 安装污染到请求的前缀之外；
      #   2. 只编了 uvcpp 这一个目标（`--target uvcpp`，zlib1.dll 没被编出来）时
      #      `file INSTALL cannot find ".../Release/zlib1.dll"`，**整个安装中断**，
      #      连 uvcpp.lib 都装不上（前面 libuv/llhttp 已经装进去了，是个半截安装）。
      # EXCLUDE_FROM_ALL（CMake 3.25+）会把子目录的 install 规则一起排除；目标
      # 本身照样能被我们的链接依赖拉起来（uvcpp 显式链 zlibstatic）。3.20~3.24
      # 上这个参数不影响 install 规则，退回旧行为，不会更糟。
      # zlib 自己的 CMakeLists 把 `ZLIB_BUILD_EXAMPLES` **默认成 ON**，那一支里
      # 除了编 example/minigzip，还有一句 `add_test(example example)`
      # （v1.3.1 的 CMakeLists.txt:8 与 :200-203）。我们这条支路只在**没有系统
      # zlib** 的平台上走，而 `EXCLUDE_FROM_ALL` 让 example 这个可执行文件从来
      # 没被编出来 —— 于是 ctest 名册里多出一条永远不会 Run 的 `example`，
      # 把 windows 的 ssl/web 两个 job 判红（Ubuntu/macOS 有系统 zlib，走的是
      # 上面的 find_package 分支，不经过这里，所以只有 Windows 上看得见）。
      #
      # 必须在 `add_subdirectory` **之前**：zlib 里用的是 `option()`，缓存里
      # 已经有这个条目时 option 不会再动它。关掉的只是它的示例程序，库照编。
      set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Enable Zlib Examples" FORCE)
      FetchContent_GetProperties(zlib)
      if(NOT zlib_POPULATED)
        FetchContent_Populate(zlib)
        add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR} EXCLUDE_FROM_ALL)
      endif()
      message(STATUS "zlib integrated (tag=${ZLIB_VERSION})")
    endif()
  endif()

endif()

# =========================================================================
# OpenSSL 依赖 —— **刻意不在上面那个 if(UVCPP_BUILD_WEB) 里**
# =========================================================================
# 这段原先嵌在 web 块内部，那是个 bug：`src/ssl/` 的编译只受
# `UVCPP_ENABLE_OPENSSL` 控制（见下面的排除规则），与 web 无关；net 层的
# `uvcpp_tcp_client::enable_tls()` 也直接用 OpenSSL。于是
# `-DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_BUILD_WEB=OFF` 这个组合会**把 ssl 编进去、
# 却从不定义 `UVCPP_SSL_LIBS`** —— 而 `UVCPP_SSL_LIBS` 为空时，下面那句
# `target_link_libraries(<t> PRIVATE ${UVCPP_SSL_LIBS})` 展开成
# `target_link_libraries(<t> PRIVATE)`，一个**静默的空操作**：配置期一声不响，
# 报错拖到链接期。症状还分平台 —— Linux 上系统 OpenSSL 头在默认搜索路径里，
# 所以是**编译过、链接炸**；MinGW/MSVC 上没有默认路径，是**编译炸**。
#
# QUIC 要求的正是"net 层 + SSL + web 关闭"这个组合（它住在 net 层），所以这段
# 必须待在 web 块外面。
#
# **位置**：紧跟在上面 web 块的 `endif()` 之后，而不是提到文件顶层 ——
# `if(UVCPP_BUILD_WEB)` 块本身不能整个上提：llhttp 必须仍挂在 web 下，它里面那条
# `set(BUILD_STATIC_LIBS ON CACHE BOOL … FORCE)` 是 llhttp 的钉子（nghttp2 也读
# `BUILD_STATIC_LIBS`），上提会往每个非 web 树的根 cache 里写一条 ON。这一段净
# 长度不变，所以紧随其后的"SSL 模块源文件"块位置不动；但**本文件后面所有行号
# 会随 QUIC 那一大段变大**。仓库里约 39 处文档与脚本按 `CMakeLists.txt:NNN`
# 引用它，改本文件时**必须逐个按内容重校**（`check_doc_lines.py` 只锁 `src/`
# 下的引用，这一类它一条都不判；实测已烂过一轮：2026-09-29 修正 39 处，其中 34 处早于 QUIC）。
if(UVCPP_ENABLE_OPENSSL)
  # Try system OpenSSL first, then FetchContent as fallback
  find_package(OpenSSL QUIET)
  if(OPENSSL_FOUND)
    message(STATUS "Using system OpenSSL ${OPENSSL_VERSION}")
    set(UVCPP_SSL_LIBS OpenSSL::SSL OpenSSL::Crypto)
  else()
    # FetchContent for OpenSSL is complex (requires Perl for config).
    # On Windows, prefer installing via vcpkg or pre-built binaries.
    message(WARNING "System OpenSSL not found. Install OpenSSL or use vcpkg.")
    message(WARNING "  vcpkg: vcpkg install openssl:x64-windows")
    message(WARNING "  Or download from https://slproweb.com/products/Win32OpenSSL.html")
    set(UVCPP_ENABLE_OPENSSL OFF)
  endif()
endif()

# =========================================================================
# SSL 模块源文件
# =========================================================================
if(UVCPP_ENABLE_OPENSSL)
  file(GLOB SSL_SRC_FILES "${CMAKE_SOURCE_DIR}/src/ssl/*.h" "${CMAKE_SOURCE_DIR}/src/ssl/*.cpp")
  source_group("ssl" FILES ${SSL_SRC_FILES})
endif()

# =========================================================================
# HTTP/2 模块源文件
# =========================================================================
if(UVCPP_ENABLE_NGHTTP2)
  # CONFIGURE_DEPENDS：新加一个 .cpp 不重新 configure 就不会被编进去，而
  # 「编过了」和「编了这个文件」在这里长得一模一样（这个坑本批已经踩过一次）。
  file(GLOB HTTP2_SRC_FILES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/src/http2/*.h" "${CMAKE_SOURCE_DIR}/src/http2/*.cpp")
  source_group("http2" FILES ${HTTP2_SRC_FILES})
endif()

# =========================================================================
# nghttp2 —— HTTP/2 后端
# =========================================================================
# 本库的 HTTP/2 **只**经 TLS 的 ALPN 协商得到,没有 h2c / 明文 h2(见 doc)。ALPN 是
# TLS 扩展 ⇒ 没有 OpenSSL 就没有 h2。这里强制关掉并出声,而不是留一个链得上、
# 跑不起来的配置。
if(UVCPP_ENABLE_NGHTTP2 AND NOT UVCPP_ENABLE_OPENSSL)
  message(WARNING
    "UVCPP_ENABLE_NGHTTP2=ON 需要 UVCPP_ENABLE_OPENSSL=ON(HTTP/2 只做 TLS+ALPN,"
    "不做 h2c/明文 h2)。已强制关闭 nghttp2。")
  set(UVCPP_ENABLE_NGHTTP2 OFF)
endif()

# h2 层复用的请求/响应类型(`uvcpp_http_request` / `uvcpp_http_response`)住在 web
# 模块里 —— 关掉 web 就不会有它们,于是 h2 连编都编不过。同样是强制关 + 出声。
if(UVCPP_ENABLE_NGHTTP2 AND NOT UVCPP_BUILD_WEB)
  message(WARNING
    "UVCPP_ENABLE_NGHTTP2=ON 需要 UVCPP_BUILD_WEB=ON(HTTP/2 复用 web 模块的"
    "请求/响应类型)。已强制关闭 nghttp2。")
  set(UVCPP_ENABLE_NGHTTP2 OFF)
endif()

if(UVCPP_ENABLE_NGHTTP2)
  # 必须用 function 造作用域:下面那批是**普通变量**,靠 CMP0077=NEW 压住 nghttp2
  # 自己的 option() —— 同名普通变量在场时 option() 不写 cache。**没被压住的开关
  # 仍会以默认值写进根 cache**(实测那批是 11 条),所以才要把开关名逐个点一遍;
  # 而 cmake_dependent_option 是例外,它内部那条 `set(... CACHE ... FORCE)` 无视
  # CMP0077,BUILD_TESTING 必然落进根 cache —— 只是值被我们压成了 OFF。
  # 压不到的是 find_package / check_* 的内部量(PC_* / LIBXML2_* / HAVE_* / ...),
  # 那是缓存机制本身,不是 nghttp2 的选择。本项目测试靠 enable_testing() +
  # UVCPP_BUILD_TESTS 注册,与 BUILD_TESTING 无关(实测 h2 树 684 条 vs 对照 630 条)。
  #
  # 静态是刻意的:链 nghttp2_static 之后 uvcpp.dll 不新增任何运行时依赖,
  # packager 的 DLL 清单一行都不用改。BUILD_SHARED_LIBS 是全局的、随使用者的
  # 缓存变化,所以必须显式压 OFF;而 BUILD_STATIC_LIBS 是**另一个独立开关**
  # (默认 OFF),两个都要设,否则一个库都不建 —— 这是现场核源码才发现的,
  # 它并不是从 BUILD_SHARED_LIBS 派生的。
  function(uvcpp_nghttp2_configure)
    set(ENABLE_LIB_ONLY    ON)
    set(ENABLE_APP         OFF)
    set(ENABLE_HPACK_TOOLS OFF)
    set(ENABLE_EXAMPLES    OFF)
    set(ENABLE_FAILMALLOC  OFF)
    set(ENABLE_DOC         OFF)
    set(BUILD_SHARED_LIBS  OFF)
    set(BUILD_STATIC_LIBS  ON)
    set(BUILD_TESTING      OFF)
    # 下面这批取值就是上游默认值,压它们纯粹是为了不让它们写进根 cache。
    set(ENABLE_WERROR      OFF)
    set(ENABLE_DEBUG       OFF)
    set(ENABLE_THREADS     ON)
    set(ENABLE_HTTP3       OFF)
    set(ENABLE_STATIC_CRT  OFF)
    set(WITH_LIBXML2       OFF)
    set(WITH_JEMALLOC      OFF)
    set(WITH_MRUBY         OFF)
    set(WITH_NEVERBLEED    OFF)
    set(WITH_LIBBPF        OFF)
    set(WITH_WOLFSSL       OFF)

    # PIC 必须在**建目标之前**定好 —— 属性是 add_library 那一刻初始化的。
    # 设在这里而不是根作用域:function 作用域只罩住 nghttp2 子树,
    # 我们自己的静态库不受影响。理由见下面那段。
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    FetchContent_GetProperties(nghttp2)
    if(NOT nghttp2_POPULATED)
      FetchContent_Populate(nghttp2)
      add_subdirectory(${nghttp2_SOURCE_DIR} ${nghttp2_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
  endfunction()

  # 与 zlib 同样**不能**用 FetchContent_MakeAvailable:它等价于 add_subdirectory,
  # 会把 nghttp2 自己的 install 规则(nghttp2Config.cmake / nghttp2Targets.cmake /
  # libnghttp2.pc)一并登记,污染 `cmake --install`。EXCLUDE_FROM_ALL 一起排除,
  # 而目标照样能被下面的链接依赖拉起来。
  FetchContent_Declare(
    nghttp2
    GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
    GIT_TAG        ${NGHTTP2_VERSION}
    GIT_SHALLOW    TRUE
  )
  uvcpp_nghttp2_configure()

  # nghttp2 是本工程里**唯一被强制静态**的依赖（见上面那个 function 里的
  # `BUILD_SHARED_LIBS OFF` + `BUILD_STATIC_LIBS ON`），而 CMake 给静态库初始化
  # 的 PIC 默认值是 OFF ⇒ 在 Linux 上编出来的 libuvcpp.so 链不上它：
  #
  #   /usr/bin/ld: libnghttp2.a(nghttp2_session.c.o): relocation R_X86_64_PC32
  #   against symbol `nghttp2_enable_strict_preface' can not be used when making
  #   a shared object; recompile with -fPIC
  #
  # 另外两个依赖看不出这条：llhttp 是 **shared**（所以 CI 要拷 llhttp.dll），
  # libuv 在 CI 上走系统包。而 PE 与 Mach-O 根本没有这个约束 —— 所以它**只在
  # Linux 上红**，本机 Windows 复现不了，macOS 那条腿实测也是绿的。
  #
  # 用 `CMAKE_POSITION_INDEPENDENT_CODE` 而不是事后 `set_target_properties`:
  # 前者按名字无关的方式给子树里每个目标初始化属性,上游改库目标名也不会静默失效
  # (库目标名漂移过 —— `lib/CMakeLists.txt:128` 是 `add_library(nghttp2 ALIAS
  # ${LIB_SELECTED})`,而 `release.yml` 对整棵树用的就是这个变量)。
  #
  # 下面这条不是仪式:属性在 MSVC 上照样读得回来(只是生成器不吐 /fPIC),
  # 所以它**在本机就能验**;真失效时会在 configure 阶段响亮地停下来,
  # 而不是等 Linux 的链接器来说话。
  if(NOT TARGET nghttp2_static)
    message(FATAL_ERROR
      "nghttp2 没建出 nghttp2_static —— BUILD_SHARED_LIBS/BUILD_STATIC_LIBS 没压住,"
      "或者上游改了库目标名")
  endif()
  get_target_property(_uvcpp_nghttp2_pic nghttp2_static POSITION_INDEPENDENT_CODE)
  if(NOT _uvcpp_nghttp2_pic)
    message(FATAL_ERROR
      "nghttp2_static 的 POSITION_INDEPENDENT_CODE 没生效(=${_uvcpp_nghttp2_pic}) —— "
      "Linux 上 libuvcpp.so 会链不上它")
  endif()

  message(STATUS "nghttp2 integrated (tag=${NGHTTP2_VERSION}, static)")
endif()

# =========================================================================
# QUIC 传输层 —— ngtcp2 后端
# =========================================================================
# 三条守卫，顺序是**load-bearing** 的：先"有没有 OpenSSL"，再"有没有 net 层"，
# 最后才是"那份 OpenSSL 带不带 QUIC API"（这条要用上一条的结果去探）。
#
# 全部照仓库既有的"静默降级链"风格：`set(UVCPP_ENABLE_QUIC OFF)` 是**普通变量**，
# 不是 cache 写。于是 `CMakeCache.txt` 里仍然是 `UVCPP_ENABLE_QUIC:BOOL=ON` ——
# 这正是 h2 job 要**三道**门禁的原因（cache、configure 日志、用例是否真注册），
# 也是下面那句 `Including quic module in build` 存在的理由：CI 要 grep 它。
#
# 位置也是 load-bearing 的：这几条必须**早于** `_uvcpp_literal01` 那一批（本文件
# 后面），否则生成头里写 1 而编译定义写 0，第一个 TU 就 `#error`。
if(UVCPP_ENABLE_QUIC AND NOT UVCPP_ENABLE_OPENSSL)
  message(WARNING
    "UVCPP_ENABLE_QUIC=ON 需要 UVCPP_ENABLE_OPENSSL=ON（QUIC 建在 TLS 1.3 上，"
    "ALPN 是 TLS 扩展，没有明文 QUIC 这回事）。已强制关闭 quic。")
  set(UVCPP_ENABLE_QUIC OFF)
endif()

if(UVCPP_ENABLE_QUIC AND NOT UVCPP_BUILD_NET)
  message(WARNING
    "UVCPP_ENABLE_QUIC=ON 需要 UVCPP_BUILD_NET=ON（QUIC 是 net 层的一条链路协议，"
    "并且复用 src/net/uvcpp_net_read.h 的 net_read_result 事件契约）。已强制关闭 quic。")
  set(UVCPP_ENABLE_QUIC OFF)
endif()

# -------------------------------------------------------------------------
# OpenSSL 有没有 QUIC API —— **判据必须和 ngtcp2 自己的一致**
# -------------------------------------------------------------------------
# 这一段的顺序是照着 ngtcp2 的 `CMakeLists.txt` 抄的，不是随手写的：它**先**查
# `SSL_provide_quic_data`，**只有它不存在时**才查 `SSL_set_quic_tls_cbs`；前者为真
# 走 quictls/LibreSSL 分支（`HAVE_QUICTLS`），后者为真才是 mainline OpenSSL ≥ 3.2
# （`HAVE_OSSL`）。这个分支决定它**建哪个 crypto 目标**：
#
#   HAVE_OSSL   ⇒ ngtcp2_crypto_ossl_static    ← 我们链的就是这个
#   HAVE_QUICTLS⇒ ngtcp2_crypto_quictls_static ← 目标名不同
#
# 所以"只查 SSL_set_quic_tls_cbs"是错的：quictls 树上它可能也为真，于是预检放行，
# 然后去链一个**从没被建出来**的目标。这里复刻它的顺序，判据才和它一致。
#
# **不要**用 `OPENSSL_VERSION` 做版本比较：本文件上面把它钉成了一个 cache STRING
# （值是 git tag，如 `openssl-3.4.0`），而 FindOpenSSL 在**同名**上设的是普通变量；
# OpenSSL 没找到时比的是 tag 解析出来的 `0.4.0`，`VERSION_LESS 3.2` 恒真。用
# `check_symbol_exists` + 自己的 `UVCPP_` 前缀缓存名，既躲开撞名，又不会把一个
# "打了 QUIC 补丁的 3.0" 误判掉（那种树 ngtcp2 是收的）。
if(UVCPP_ENABLE_QUIC)
  include(CheckSymbolExists)
  include(CMakePushCheckState)
  cmake_push_check_state()
  set(CMAKE_REQUIRED_INCLUDES  "${OPENSSL_INCLUDE_DIR}")
  set(CMAKE_REQUIRED_LIBRARIES "${OPENSSL_LIBRARIES}")
  if(WIN32)
    # 同 ngtcp2：静态 libcrypto 直接调 Cert*/BCrypt 系列，探测链接时得带上。
    list(APPEND CMAKE_REQUIRED_LIBRARIES ws2_32 bcrypt crypt32)
  endif()
  check_symbol_exists(SSL_provide_quic_data "openssl/ssl.h"
                      UVCPP_OPENSSL_HAS_QUIC_PROVIDE)
  if(NOT UVCPP_OPENSSL_HAS_QUIC_PROVIDE)
    check_symbol_exists(SSL_set_quic_tls_cbs "openssl/ssl.h"
                        UVCPP_OPENSSL_HAS_QUIC_CBS)
  endif()
  cmake_pop_check_state()

  # 要的是"mainline、且带 QUIC API"这一格：没有 provide、但有 cbs。
  # 另外两格（有 provide ⇒ quictls 分支；两个都没有 ⇒ 完全没有 QUIC API）都关掉。
  if(UVCPP_OPENSSL_HAS_QUIC_PROVIDE OR NOT UVCPP_OPENSSL_HAS_QUIC_CBS)
    message(WARNING
      "UVCPP_ENABLE_QUIC=ON 需要一份**带 QUIC API 的 mainline OpenSSL ≥ 3.2**，"
      "而找到的那份不满足（头在 ${OPENSSL_INCLUDE_DIR}）。\n"
      "  为什么非 3.2 不可：libngtcp2 在它自己的 configure 期就会因为缺 "
      "SSL_set_quic_tls_cbs 而 FATAL_ERROR，而且我们要链的 ngtcp2_crypto_ossl_static "
      "根本不会被建出来。\n"
      "  怎么修：编一份 3.2+ 到某个前缀，再把它指过来 ——\n"
      "    cmake -S . -B build-quic -DUVCPP_ENABLE_QUIC=ON -DUVCPP_ENABLE_OPENSSL=ON \\\n"
      "          -DOPENSSL_ROOT_DIR=<那份 3.2+ 的前缀>\n"
      "  （3.0/3.1 与 Ubuntu 24.04 自带的 3.0.13 都不行。）已强制关闭 quic。")
    set(UVCPP_ENABLE_QUIC OFF)
  endif()
endif()

if(UVCPP_ENABLE_QUIC)
  # 与 nghttp2 同一个套路：用 `function` 造作用域，靠 CMP0077=NEW 让同名**普通
  # 变量**压住 ngtcp2 自己的 `option()`（它在 `CMakeOptions.txt` 里，由它本文件
  # `include()` 进来，读到的就是这里造的那些普通变量）。`cmake_dependent_option`
  # 是例外 —— 它内部那条 `set(... CACHE ... FORCE)` 无视 CMP0077，所以
  # BUILD_TESTING 照样落进根 cache，只是值被我们压成了 OFF。
  #
  # `ENABLE_LIB_ONLY=ON` 是关键的一条：它让 ngtcp2 跳过 `examples/` 与 `tests/`，
  # 而 `third-party/`（http-parser / urlparse 那些）整段挂在
  # `if(LIBEV_FOUND AND LIBNGHTTP3_FOUND)` 下面也不会建。于是 **libngtcp2 不需要
  # nghttp3、也不需要 libev** —— nghttp3 留到 HTTP/3 那一版再接。
  function(uvcpp_ngtcp2_configure)
    set(ENABLE_STATIC_LIB  ON)
    set(ENABLE_SHARED_LIB  OFF)
    set(ENABLE_LIB_ONLY    ON)
    set(ENABLE_OPENSSL     ON)
    set(ENABLE_WERROR      OFF)
    set(ENABLE_DEBUG       OFF)
    set(ENABLE_ASAN        OFF)
    set(ENABLE_JEMALLOC    OFF)
    set(ENABLE_GNUTLS      OFF)
    set(ENABLE_BORINGSSL   OFF)
    set(ENABLE_PICOTLS     OFF)
    set(ENABLE_WOLFSSL     OFF)
    set(BUILD_TESTING      OFF)

    # PIC 必须在**建目标之前**定好 —— 属性是 add_library 那一刻初始化的。
    # 这不是仪式：ngtcp2 只给**共享**那两个目标设了 POSITION_INDEPENDENT_CODE，
    # 静态的那两个没设 ⇒ Linux 上 libuvcpp.so 链不上（同 nghttp2 那段注释里的
    # R_X86_64_PC32 报错）。设在这里而不是事后改属性：前者按名字无关的方式给整个
    # 子树初始化，上游改库目标名也不会静默失效。
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    FetchContent_GetProperties(ngtcp2)
    if(NOT ngtcp2_POPULATED)
      FetchContent_Populate(ngtcp2)

      # ---------------------------------------------------------------
      # `check` 目标撞名 —— 只能改源，先看清楚为什么
      # ---------------------------------------------------------------
      # ngtcp2 与 nghttp2 **都**无条件地
      #   add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND})
      # （`ngtcp2/CMakeLists.txt:160`、`nghttp2/CMakeLists.txt:174`，都不带守卫）。
      # 于是 **http2 与 quic 同时开**时，后加进来的那个直接 configure 失败：
      #   add_custom_target cannot create target "check" because another target
      #   with the same name already exists.
      # 本文件里 nghttp2 那段在前，所以撞的是 ngtcp2 —— 但成因是"两个上游都建"，
      # 不是"ngtcp2 建错了"。
      #
      # CMake 给的逃生口是 CMP0002=OLD，**在这里救不了**：策略只在没有被子目录
      # 覆盖时继承，而 ngtcp2 自己的 `cmake_minimum_required(VERSION 3.20)`
      # 会把继承下来的 OLD 重置回 NEW —— 那句在第 25 行，`add_custom_target`
      # 在第 160 行。下面三种设法都是**实测**失败的，别再试：
      #   ① 本文件顶层 `cmake_policy(SET CMP0002 OLD)`（含放进上面那个
      #      `function()` 里的变体）；② `set(CMAKE_POLICY_DEFAULT_CMP0002
      #      OLD CACHE STRING "" FORCE)`；③ 指望 `enable_testing()` 关掉就行
      #      （撞名与 testing 无关，只看逻辑目标名）。
      #
      # 于是只剩改源。**不原地改**：`FETCHCONTENT_SOURCE_DIR_NGTCP2` 通常指着
      # 使用者的一份检出（本地是 `_local_deps/ngtcp2`），原地改等于弄脏别人的
      # 工作树，而且下次 configure 会把那处改动当成上游内容。所以：把源码
      # **复制**进构建目录，只动副本，也从副本构建。
      #
      # 复制只在第一次做 —— 副本在就跳过，重复 configure 不再付这份开销。
      # 代价是**改了 `_local_deps/ngtcp2` 之后要删掉副本**（见 doc/quic-guide.md）。
      set(_uvcpp_ngtcp2_src "${CMAKE_BINARY_DIR}/_deps/uvcpp-ngtcp2-src")
      set(_uvcpp_ngtcp2_bin "${CMAKE_BINARY_DIR}/_deps/uvcpp-ngtcp2-build")
      if(NOT EXISTS "${_uvcpp_ngtcp2_src}/CMakeLists.txt")
        file(COPY "${ngtcp2_SOURCE_DIR}/" DESTINATION "${_uvcpp_ngtcp2_src}"
             PATTERN ".git" EXCLUDE)
        file(READ "${_uvcpp_ngtcp2_src}/CMakeLists.txt" _uvcpp_ngtcp2_cml)
        set(_uvcpp_ngtcp2_check_line
            [[add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND})]])
        string(FIND "${_uvcpp_ngtcp2_cml}" "${_uvcpp_ngtcp2_check_line}"
               _uvcpp_ngtcp2_hit)
        if(_uvcpp_ngtcp2_hit EQUAL -1)
          message(FATAL_ERROR
            "补丁没打上：在 ${_uvcpp_ngtcp2_src}/CMakeLists.txt 里找不到 "
            "`add_custom_target(check COMMAND \${CMAKE_CTEST_COMMAND})`。\n"
            "  上游改过这一处。请对着那个文件重新确认：要么它已经不建 `check`"
            "（那这段补丁连同这条断言一起删掉），要么那一行的写法变了（这条 "
            "string(FIND) 要跟着改）。\n"
            "  **别把这条断言删掉了事**：它一失效，补丁就变成静默空转，而 "
            "http2+quic 会在 add_subdirectory 那里报一条看不出所以然的撞名错。")
        endif()
        string(REPLACE "${_uvcpp_ngtcp2_check_line}"
          "# [uvcpp] 摘掉 `check`：它与 nghttp2 的无条件同名目标冲突，见根 CMakeLists.txt 里那段注释"
          _uvcpp_ngtcp2_cml "${_uvcpp_ngtcp2_cml}")
        file(WRITE "${_uvcpp_ngtcp2_src}/CMakeLists.txt" "${_uvcpp_ngtcp2_cml}")
      endif()

      add_subdirectory("${_uvcpp_ngtcp2_src}" "${_uvcpp_ngtcp2_bin}" EXCLUDE_FROM_ALL)
    endif()
  endfunction()

  # 与 nghttp2 同样**不能**用 FetchContent_MakeAvailable：它等价于
  # add_subdirectory，会连上游的 install 规则（ngtcp2Config.cmake、
  # libngtcp2.pc）一并登记，污染 `cmake --install`。EXCLUDE_FROM_ALL 一起排除，
  # 而目标照样能被下面的链接依赖拉起来。
  FetchContent_Declare(
    ngtcp2
    GIT_REPOSITORY https://github.com/ngtcp2/ngtcp2.git
    GIT_TAG        ${NGTCP2_VERSION}
    GIT_SHALLOW    TRUE
  )
  uvcpp_ngtcp2_configure()

  # 存在性断言要**排在 PIC 断言前面**。少了它，目标不存在时
  # `get_target_property` 返回 NOTFOUND，下面那条 FATAL 会谎报"PIC 没生效"，
  # 而真因是"目标压根没建"—— 这种误导性的报错比不报还费时间。
  if(NOT TARGET ngtcp2_static)
    message(FATAL_ERROR
      "ngtcp2 没建出 ngtcp2_static —— ENABLE_STATIC_LIB/ENABLE_SHARED_LIB 没压住,"
      "或者上游改了库目标名")
  endif()
  if(NOT TARGET ngtcp2_crypto_ossl_static)
    message(FATAL_ERROR
      "ngtcp2 没建出 ngtcp2_crypto_ossl_static —— 说明那份 OpenSSL 走的不是 mainline "
      "分支(上游此时只在 quictls/LibreSSL 下建 ngtcp2_crypto_quictls_static),"
      "上面那段 QUIC API 预检和这里对不上了")
  endif()
  get_target_property(_uvcpp_ngtcp2_pic ngtcp2_static POSITION_INDEPENDENT_CODE)
  if(NOT _uvcpp_ngtcp2_pic)
    message(FATAL_ERROR
      "ngtcp2_static 的 POSITION_INDEPENDENT_CODE 没生效(=${_uvcpp_ngtcp2_pic}) —— "
      "Linux 上 libuvcpp.so 会链不上它")
  endif()
  get_target_property(_uvcpp_ngtcp2_crypto_pic ngtcp2_crypto_ossl_static POSITION_INDEPENDENT_CODE)
  if(NOT _uvcpp_ngtcp2_crypto_pic)
    message(FATAL_ERROR
      "ngtcp2_crypto_ossl_static 的 POSITION_INDEPENDENT_CODE 没生效"
      "(=${_uvcpp_ngtcp2_crypto_pic}) —— Linux 上 libuvcpp.so 会链不上它")
  endif()

  message(STATUS "ngtcp2 integrated (tag=${NGTCP2_VERSION}, static)")
endif()

# =========================================================================
# HTTP/3 —— nghttp3 后端，架在 QUIC 之上
# =========================================================================
# 两条守卫，都照 QUIC 那一套"静默降级链"写：`set(... OFF)` 是**普通变量**，
# 不是 cache 写，所以 cache 里仍然是 `UVCPP_ENABLE_HTTP3:BOOL=ON`，只 grep cache
# 会得到一个假绿 —— 这就是下面那句 `Including http3 module in build` 存在的理由。
#
# 位置：**在这两条之后、`_uvcpp_literal01` 那一批之前**。前者不只是顺序好看：
# QUIC 自己的守卫在这一段**上面**已经跑过，所以"QUIC 被强制关掉了"这件事我们
# 这里直接读结果就行，不需要把 OPENSSL / NET 那两条重写一遍（HTTP3 ⇒ QUIC ⇒
# OPENSSL 是传递的）。后者是硬约束：守卫晚于那一批，生成头里写 1、编译定义写 0，
# 第一个 TU 就 `#error`。
#
# 顺带说清这两条为什么**报错信息里要给出路**：这两个组合都不是使用者的笔误，
# 而是"开关之间的真实依赖"。一句话说清依赖，比让他去读三个 option 的注释快。
if(UVCPP_ENABLE_HTTP3 AND NOT UVCPP_ENABLE_QUIC)
  message(WARNING
    "UVCPP_ENABLE_HTTP3=ON 需要 UVCPP_ENABLE_QUIC=ON（HTTP/3 是 QUIC 之上的一条"
    "应用层协议，它没有别的承载 —— 就像 h2 不能没有 TCP）。已强制关闭 http3。")
  set(UVCPP_ENABLE_HTTP3 OFF)
endif()

if(UVCPP_ENABLE_HTTP3 AND NOT UVCPP_BUILD_WEB)
  message(WARNING
    "UVCPP_ENABLE_HTTP3=ON 需要 UVCPP_BUILD_WEB=ON（HTTP/3 是 web 层的一条传输，"
    "接在 uvcpp_http_client / uvcpp_http_server 上；没有 web 层，这一层建出来没有"
    "调用方）。已强制关闭 http3。")
  set(UVCPP_ENABLE_HTTP3 OFF)
endif()

# =========================================================================
# C ABI —— 两条守卫
# =========================================================================
# 形状与上面 QUIC / HTTP3 那两段逐条相同：`set(... OFF)` 是**普通变量**，不是
# cache 写，所以 `CMakeCache.txt` 里仍然是 `UVCPP_ENABLE_CAPI:BOOL=ON`，只 grep
# cache 会得到一个假绿 —— 这就是下面那句 `Including capi module in build` 存在
# 的理由（CI 与 test 都 grep 它）。
#
# 位置同样是 load-bearing 的：必须在 `_uvcpp_literal01` 那一批**之前**（晚了的话
# 生成头里写 1、编译定义写 0，第一个 TU 就 `#error`），也在下面 `CAPI_SRC_FILES`
# 那个 GLOB 之前（晚了的话"被强制关掉"的那次 configure 仍然会去 glob）。
#
# 为什么是**两条**而不是一条 `NOT WEB OR NOT NET`：这两种缺失是两件不同的事，
# 各自的出路也不同（一个是"上层四块没有宿主"，一个是"地基没编进来"）。分开写
# 才能把话说清楚，这也是本文件里 nghttp2 / wsdl / quic 那几处的既有风格。
if(UVCPP_ENABLE_CAPI AND NOT UVCPP_BUILD_NET)
  message(WARNING
    "UVCPP_ENABLE_CAPI=ON 需要 UVCPP_BUILD_NET=ON（C 门面的地基是 net 那一层："
    "tcp_client / tcp_server 是绝大多数上层句柄的宿主）。已强制关闭 capi。")
  set(UVCPP_ENABLE_CAPI OFF)
endif()

if(UVCPP_ENABLE_CAPI AND NOT UVCPP_BUILD_WEB)
  message(WARNING
    "UVCPP_ENABLE_CAPI=ON 需要 UVCPP_BUILD_WEB=ON（C 门面里 web / webapp / "
    "http2 / http3 四块都接在 web 层上，没有 web 层就没有这些头可导）。"
    "已强制关闭 capi。")
  set(UVCPP_ENABLE_CAPI OFF)
endif()

if(UVCPP_ENABLE_HTTP3)
  # 这两条路径在 `function` 作用域**外面**先定一份：nghttp3_static 不给
  # build-interface 的 include 目录（见下面那段），我们的目标必须自己把它加上，
  # 而那些 `target_include_directories` 调用在 function 外面。CMake 的函数能读
  # 调用者的变量，所以 function 里面照样用得到。
  set(_uvcpp_nghttp3_src "${CMAKE_BINARY_DIR}/_deps/uvcpp-nghttp3-src")
  set(_uvcpp_nghttp3_bin "${CMAKE_BINARY_DIR}/_deps/uvcpp-nghttp3-build")

  # 与 nghttp2 / ngtcp2 同一个套路：用 `function` 造作用域，靠 CMP0077=NEW 让同名
  # **普通变量**压住 nghttp3 自己的 `option()`（它在 `CMakeOptions.txt` 里，由它
  # 本文件 `include()` 进来，读到的就是这里造的那些普通变量）。
  #
  # `ENABLE_LIB_ONLY=ON` 是关键的一条：它让 nghttp3 跳过 `examples/`（`tests/` 那
  # 边另有 BUILD_TESTING 管着）。于是 **libnghttp3 不依赖 libev、也不依赖 c-ares**。
  function(uvcpp_nghttp3_configure)
    set(ENABLE_STATIC_LIB  ON)
    set(ENABLE_SHARED_LIB  OFF)
    set(ENABLE_LIB_ONLY    ON)
    set(ENABLE_WERROR      OFF)
    set(ENABLE_DEBUG       OFF)
    set(ENABLE_ASAN        OFF)
    set(ENABLE_STATIC_CRT  OFF)
    set(BUILD_TESTING      OFF)

    # PIC 必须在**建目标之前**定好 —— 属性是 add_library 那一刻初始化的。理由与
    # ngtcp2 那段逐字相同：nghttp3 只给 `nghttp3`（共享那个）设了
    # POSITION_INDEPENDENT_CODE，`nghttp3_static` 没设 ⇒ Linux 上 libuvcpp.so
    # 链不上（R_X86_64_PC32）。
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    FetchContent_GetProperties(nghttp3)
    if(NOT nghttp3_POPULATED)
      FetchContent_Populate(nghttp3)

      # ---------------------------------------------------------------
      # `check` 目标撞名 —— **第三个**来源
      # ---------------------------------------------------------------
      # `nghttp3/CMakeLists.txt:71` 与 nghttp2:174、ngtcp2:160 是**同一行**：
      #   add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND})
      # 一样不带守卫，一样在 `cmake_minimum_required(VERSION 3.20)`（:26）之后 ——
      # 所以 CMP0002=OLD 那三条设法在这里同样救不了，理由与 ngtcp2 那段逐字相同，
      # 不再重复。
      #
      # 撞名只在"两个都开"时出现，而 h3 一定要开 quic ⇒ nghttp3 与 ngtcp2 必然
      # 同框 ⇒ **http3 这一格必然撞**（ngtcp2 在前，所以撞的是 nghttp3）。这不是
      # "也许会发生"，是"确定了会发生"。
      #
      # 处理与 ngtcp2 完全一致：拷一份源码树进构建目录，只动副本。**不原地改** ——
      # `FETCHCONTENT_SOURCE_DIR_NGHTTP3` 通常指着使用者的一份检出。
      if(NOT EXISTS "${_uvcpp_nghttp3_src}/CMakeLists.txt")
        file(COPY "${nghttp3_SOURCE_DIR}/" DESTINATION "${_uvcpp_nghttp3_src}"
             PATTERN ".git" EXCLUDE)
        file(READ "${_uvcpp_nghttp3_src}/CMakeLists.txt" _uvcpp_nghttp3_cml)
        set(_uvcpp_nghttp3_check_line
            [[add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND})]])
        string(FIND "${_uvcpp_nghttp3_cml}" "${_uvcpp_nghttp3_check_line}"
               _uvcpp_nghttp3_hit)
        if(_uvcpp_nghttp3_hit EQUAL -1)
          message(FATAL_ERROR
            "补丁没打上：在 ${_uvcpp_nghttp3_src}/CMakeLists.txt 里找不到 "
            "`add_custom_target(check COMMAND \${CMAKE_CTEST_COMMAND})`。\n"
            "  上游改过这一处。请对着那个文件重新确认：要么它已经不建 `check`"
            "（那这段补丁连同这条断言一起删掉），要么那一行的写法变了（这条 "
            "string(FIND) 要跟着改）。\n"
            "  **别把这条断言删掉了事**：它一失效，补丁就变成静默空转，而 "
            "quic+http3 会在 add_subdirectory 那里报一条看不出所以然的撞名错。")
        endif()
        string(REPLACE "${_uvcpp_nghttp3_check_line}"
          "# [uvcpp] 摘掉 `check`：它与 nghttp2/ngtcp2 的无条件同名目标冲突，见根 CMakeLists.txt 里那段注释"
          _uvcpp_nghttp3_cml "${_uvcpp_nghttp3_cml}")
        file(WRITE "${_uvcpp_nghttp3_src}/CMakeLists.txt" "${_uvcpp_nghttp3_cml}")
      endif()

      add_subdirectory("${_uvcpp_nghttp3_src}" "${_uvcpp_nghttp3_bin}" EXCLUDE_FROM_ALL)
    endif()
  endfunction()

  # 同 nghttp2 / ngtcp2：**不能**用 FetchContent_MakeAvailable —— 它等价于
  # add_subdirectory，会连上游的 install 规则（nghttp3Config.cmake、libnghttp3.pc）
  # 一并登记，污染 `cmake --install`。EXCLUDE_FROM_ALL 一起排除，目标照样能被
  # 下面的链接依赖拉起来。
  FetchContent_Declare(
    nghttp3
    GIT_REPOSITORY https://github.com/ngtcp2/nghttp3.git
    GIT_TAG        ${NGHTTP3_VERSION}
    GIT_SHALLOW    TRUE
  )
  uvcpp_nghttp3_configure()

  # 存在性断言排在 PIC 断言前面，理由与 ngtcp2 那段逐字相同：目标不存在时
  # `get_target_property` 返回 NOTFOUND，PIC 那条 FATAL 会谎报"PIC 没生效"。
  #
  # 目标名是**裸的 `nghttp3_static`，没有 ALIAS** —— 与 `ngtcp2_static` 同形，
  # 与 `nghttp2_static` 也同形。别去猜 `nghttp3::nghttp3_static` 那种带命名空间
  # 的名字：上游只在**导出**（`install(EXPORT)`）时才造 `nghttp3::` 前缀，而
  # 我们走的是 add_subdirectory，拿到的是构建树里的原始目标名。
  if(NOT TARGET nghttp3_static)
    message(FATAL_ERROR
      "nghttp3 没建出 nghttp3_static —— ENABLE_STATIC_LIB/ENABLE_SHARED_LIB 没压住,"
      "或者上游改了库目标名")
  endif()
  get_target_property(_uvcpp_nghttp3_pic nghttp3_static POSITION_INDEPENDENT_CODE)
  if(NOT _uvcpp_nghttp3_pic)
    message(FATAL_ERROR
      "nghttp3_static 的 POSITION_INDEPENDENT_CODE 没生效(=${_uvcpp_nghttp3_pic}) —— "
      "Linux 上 libuvcpp.so 会链不上它")
  endif()

  message(STATUS "nghttp3 integrated (tag=${NGHTTP3_VERSION}, static)")
endif()

# NOTE: PATH for Windows tests is set *after* add_subdirectory(tests) below
# to ensure tesets are registered before we query TESTS property.

if(WIN32 AND TARGET uvcpp)
  # Copy uvcpp DLL next to unit and functional test executables after build.
  add_custom_command(TARGET uvcpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uvcpp> "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uvcpp> "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
    COMMENT "Copying uvcpp DLL next to test executables")
endif()

## NOTE:
## We must not call add_custom_command(TARGET uv ...) from this directory because
## the `uv` target is created inside the libuv subproject when FetchContent
## brings it in. CMake requires add_custom_command(TARGET ...) to be invoked in
## the same directory that created the target. Instead we rely on the
## `copy_test_dlls` custom target below which adds PRE_BUILD copy steps and may
## DEPEND on the `uv` target if it exists.

if(WIN32)
  # Ensure PATH for tests includes build output dirs so DLLs are found.
  # Build a single PATH=… entry — CMake treats ';' as list separator,
  # so we escape intra-value semicolons with '\;' to keep them literal.
  get_property(_all_tests GLOBAL PROPERTY TESTS)
  if(_all_tests)
    foreach(_t ${_all_tests})
      set(_path "")
      if(TARGET uvcpp)
        set(_path "$<TARGET_FILE_DIR:uvcpp>")
      endif()
      if(TARGET uv)
        if(_path)
          set(_path "${_path}\\;$<TARGET_FILE_DIR:uv>")
        else()
          set(_path "$<TARGET_FILE_DIR:uv>")
        endif()
      endif()
      # Also include libuv build dir (statically-built libuv may not have a TARGET_FILE_DIR)
      if(TARGET uv_a)
        if(_path)
          set(_path "${_path}\\;$<TARGET_FILE_DIR:uv_a>")
        else()
          set(_path "$<TARGET_FILE_DIR:uv_a>")
        endif()
      endif()
      if(_path)
        set_tests_properties(${_t} PROPERTIES ENVIRONMENT "PATH=${_path}")
      endif()
    endforeach()
  endif()
endif()

# If libuv exported a CMake target (common when using FetchContent), prefer linking to it.
# This ensures proper linkage on Windows (uv target will provide the correct imported library).
# 发布用的动态库要把 libuv 静态链进 uvcpp.dll（最终产物只有单个 dll），所以
# BUILD_SHARED_LIBS=OFF 时选静态的 uv_a。libuv 1.36 无条件同时建 uv 与 uv_a，
# 1.51 起 uv 受 LIBUV_BUILD_SHARED 控制，因此必须判 TARGET 而不是假定名字。
if (NOT BUILD_SHARED_LIBS AND TARGET uv_a)
  message(STATUS "Using libuv CMake target 'uv_a' (static) for linking")
  set(LIBUV_LIB uv_a)
elseif (TARGET uv)
  message(STATUS "Using libuv CMake target 'uv' for linking")
  set(LIBUV_LIB uv)
elseif (TARGET uv_a)
  message(STATUS "Using libuv CMake target 'uv_a' for linking")
  set(LIBUV_LIB uv_a)
elseif(NOT LIBUV_LIB)
  message(FATAL_ERROR "libuv not found and no libuv target available. Set UVCPP_USE_SYSTEM_LIBUV=ON or enable UVCPP_BUILD_LIBUV_FROM_SOURCE.")
endif()

include_directories(${LIBUV_INCLUDE_DIR})

# Option to follow libuv's BUILD_SHARED_LIBS to set sensible defaults for uvcpp.
# Define this option before deciding default values for UVCPP_BUILD_SHARED/STATIC.
option(UVCPP_FOLLOW_LIBUV_BUILD "If ON, set UVCPP_BUILD_SHARED/STATIC default to follow libuv's BUILD_SHARED_LIBS" ON)

# Determine default for building shared/static uvcpp based on libuv (if requested).
set(_default_uvcpp_build_shared ON)
if(UVCPP_FOLLOW_LIBUV_BUILD AND DEFINED BUILD_SHARED_LIBS)
  if(BUILD_SHARED_LIBS)
    set(_default_uvcpp_build_shared ON)
  else()
    set(_default_uvcpp_build_shared OFF)
  endif()
endif()

# Provide per-library build options for uvcpp. Defaults follow libuv if requested.
option(UVCPP_BUILD_SHARED "Build shared uvcpp library" ${_default_uvcpp_build_shared})
if(_default_uvcpp_build_shared)
  set(_default_uvcpp_build_static OFF)
else()
  set(_default_uvcpp_build_static ON)
endif()
option(UVCPP_BUILD_STATIC "Build static uvcpp library" ${_default_uvcpp_build_static})

# Organize sources into Visual Studio source groups for readability.
file(GLOB UVCPP_SRC_FILES "${CMAKE_SOURCE_DIR}/src/uvcpp/*.h" "${CMAKE_SOURCE_DIR}/src/uvcpp/*.cpp")
file(GLOB HANDLE_SRC_FILES "${CMAKE_SOURCE_DIR}/src/handle/*.h" "${CMAKE_SOURCE_DIR}/src/handle/*.cpp")
file(GLOB REQ_SRC_FILES "${CMAKE_SOURCE_DIR}/src/req/*.h" "${CMAKE_SOURCE_DIR}/src/req/*.cpp")
file(GLOB EXPAND_SRC_FILES "${CMAKE_SOURCE_DIR}/src/expand/*.h" "${CMAKE_SOURCE_DIR}/src/expand/*.cpp")
file(GLOB NET_SRC_FILES "${CMAKE_SOURCE_DIR}/src/net/*.h" "${CMAKE_SOURCE_DIR}/src/net/*.cpp")
source_group("uvcpp" FILES ${UVCPP_SRC_FILES})
source_group("handle" FILES ${HANDLE_SRC_FILES})
source_group("req" FILES ${REQ_SRC_FILES})
source_group("net" FILES ${NET_SRC_FILES})
if(UVCPP_BUILD_EXPAND)
  source_group("expand" FILES ${EXPAND_SRC_FILES})
endif()

# web module source_group
file(GLOB WEB_SRC_FILES "${CMAKE_SOURCE_DIR}/src/web/*.h" "${CMAKE_SOURCE_DIR}/src/web/*.cpp")
if(UVCPP_BUILD_WEB)
  source_group("web" FILES ${WEB_SRC_FILES})
endif()

# webapp module source_group
file(GLOB WEBAPP_SRC_FILES "${CMAKE_SOURCE_DIR}/src/webapp/*.h" "${CMAKE_SOURCE_DIR}/src/webapp/*.cpp")
if(UVCPP_BUILD_WEBAPP)
  source_group("webapp" FILES ${WEBAPP_SRC_FILES})
endif()

# wsdl module source_group
file(GLOB WSDL_SRC_FILES "${CMAKE_SOURCE_DIR}/src/wsdl/*.h" "${CMAKE_SOURCE_DIR}/src/wsdl/*.cpp")
if(UVCPP_ENABLE_WSDL)
  source_group("wsdl" FILES ${WSDL_SRC_FILES})
endif()

# quic module source_group
# CONFIGURE_DEPENDS：新加一个 .cpp 不重新 configure 就不会被编进去，而
# 「编过了」和「编了这个文件」在这里长得一模一样（这个坑本批已经踩过一次）。
# 同一个 QUIC_SRC_FILES 还在下面被用来做"空模块"判据 —— 见那里的注释。
if(UVCPP_ENABLE_QUIC)
  file(GLOB QUIC_SRC_FILES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/src/quic/*.h" "${CMAKE_SOURCE_DIR}/src/quic/*.cpp")
  source_group("quic" FILES ${QUIC_SRC_FILES})
endif()

# http3 module source_group
# CONFIGURE_DEPENDS 的理由与 quic 那段逐字相同。同一个 HTTP3_SRC_FILES 还在下面
# 被用来做"空模块"判据。
if(UVCPP_ENABLE_HTTP3)
  file(GLOB HTTP3_SRC_FILES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/src/http3/*.h" "${CMAKE_SOURCE_DIR}/src/http3/*.cpp")
  source_group("http3" FILES ${HTTP3_SRC_FILES})
endif()

# capi module source_group
# CONFIGURE_DEPENDS 的理由与 quic/http3 那两段逐字相同（新加一个 .cpp 不重新
# configure 就不会被编进去，而"编过了"与"编了这个文件"长得一模一样）。同一个
# CAPI_SRC_FILES 还在下面被用来做"空模块"判据。
#
# 这里 glob 的是**头 + 源**，而 .h 那几份**不进** UVCPP_SOURCES 的编译，只是拿
# 来做 source_group 与"目录非空"判据 —— 与 quic/http3 那两处的形状一致。
if(UVCPP_ENABLE_CAPI)
  file(GLOB CAPI_SRC_FILES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/src/capi/*.h" "${CMAKE_SOURCE_DIR}/src/capi/*.cpp")
  source_group("capi" FILES ${CAPI_SRC_FILES})
endif()

# read version from header (src/uvcpp/uvcpp_version.h)
file(READ "${CMAKE_SOURCE_DIR}/src/uvcpp/uvcpp_version.h" UVCPP_VERSION_H)
string(REGEX MATCH "#define[ \t]+UVCPP_VERSION_MAJOR[ \t]+([0-9]+)" _ ${UVCPP_VERSION_H})
set(UVCPP_VERSION_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+UVCPP_VERSION_MINOR[ \t]+([0-9]+)" _ ${UVCPP_VERSION_H})
set(UVCPP_VERSION_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+UVCPP_VERSION_PATCH[ \t]+([0-9]+)" _ ${UVCPP_VERSION_H})
set(UVCPP_VERSION_PATCH ${CMAKE_MATCH_1})
set(UVCPP_VERSION "${UVCPP_VERSION_MAJOR}.${UVCPP_VERSION_MINOR}.${UVCPP_VERSION_PATCH}")
message(STATUS "uvcpp version: ${UVCPP_VERSION}")

# sources
file(GLOB_RECURSE UVCPP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*/*.cpp)
file(GLOB_RECURSE UVCPP_HEADERS CONFIGURE_DEPENDS src/*.h src/*/*.h)

# 定义宏：当启用 expand 模块时启用内存池
if(UVCPP_BUILD_EXPAND)
  add_compile_definitions(UVCPP_ENABLE_MEMORY_POOL=1)
  message(STATUS "Memory pool enabled (UVCPP_ENABLE_MEMORY_POOL=1)")
else()
  add_compile_definitions(UVCPP_ENABLE_MEMORY_POOL=0)
  message(STATUS "Memory pool disabled (UVCPP_ENABLE_MEMORY_POOL=0)")
endif()

# 定义宏：net / web 模块开关
if(UVCPP_BUILD_NET)
  add_compile_definitions(UVCPP_NET_ENABLE=1)
  message(STATUS "Net module enabled (UVCPP_NET_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_NET_ENABLE=0)
  message(STATUS "Net module disabled (UVCPP_NET_ENABLE=0)")
endif()

if(UVCPP_BUILD_WEB)
  add_compile_definitions(UVCPP_WEB_ENABLE=1)
  message(STATUS "Web module enabled (UVCPP_WEB_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_WEB_ENABLE=0)
  message(STATUS "Web module disabled (UVCPP_WEB_ENABLE=0)")
endif()

# webapp 依赖 web：web 关掉时强制关掉 webapp，而不是留一个编译不过的配置。
if(UVCPP_BUILD_WEBAPP AND NOT UVCPP_BUILD_WEB)
  message(WARNING
    "UVCPP_BUILD_WEBAPP=ON 需要 UVCPP_BUILD_WEB=ON（webapp 建在 web 层之上）。"
    "已强制关闭 webapp。")
  set(UVCPP_BUILD_WEBAPP OFF)
endif()

if(UVCPP_BUILD_WEBAPP)
  add_compile_definitions(UVCPP_WEBAPP_ENABLE=1)
  message(STATUS "Web app framework enabled (UVCPP_WEBAPP_ENABLE=1)")

  # -------------------------------------------------------------------
  # nlohmann/json —— webapp 的 JSON 后端（header-only）
  # -------------------------------------------------------------------
  # 关键：webapp 关闭时**完全不碰**它。没有这个 if 守卫，关掉 webapp 的构建
  # 也会去 clone 一个几十 MB 的仓库，而那个配置根本不需要 JSON。
  #
  # 这两个 CACHE INTERNAL 必须在 FetchContent_MakeAvailable 之前设置：
  # nlohmann/json 的 CMakeLists 会读它们，不设就会连它自己那一大堆测试
  # 一起构建（多几百个目标 + 拉 doctest）。
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  # JSON_Install 我们不要 —— 它装的是 nlohmann 自己那套 config 包，
  # 而我们要的是"跟着 uvcpp 一起装"（见文件末尾的安装段）。
  set(JSON_Install OFF CACHE INTERNAL "")

  message(STATUS "Fetching nlohmann/json via FetchContent (tag=${NLOHMANN_JSON_VERSION})")
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY  https://github.com/nlohmann/json.git
    GIT_TAG         ${NLOHMANN_JSON_VERSION}
    GIT_SHALLOW     TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
  message(STATUS "nlohmann/json integrated (tag=${NLOHMANN_JSON_VERSION})")
else()
  add_compile_definitions(UVCPP_WEBAPP_ENABLE=0)
  message(STATUS "Web app framework disabled (UVCPP_WEBAPP_ENABLE=0)")
endif()

# wsdl 依赖 webapp：发布 WSDL 与派发 SOAP 都要走 webapp 的路由与请求/响应封装。
# 与 nghttp2 那两条同样处理：强制关 + 出声，而不是留一个链得上、跑不起来的配置。
if(UVCPP_ENABLE_WSDL AND NOT UVCPP_BUILD_WEBAPP)
  message(WARNING
    "UVCPP_ENABLE_WSDL=ON 需要 UVCPP_BUILD_WEBAPP=ON（wsdl 建在 webapp 之上，"
    "它复用 webapp 的路由与请求/响应类型）。已强制关闭 wsdl。")
  set(UVCPP_ENABLE_WSDL OFF)
endif()

if(UVCPP_ENABLE_WSDL)
  add_compile_definitions(UVCPP_WSDL_ENABLE=1)
  message(STATUS "WSDL/SOAP module enabled (UVCPP_WSDL_ENABLE=1)")

  # -------------------------------------------------------------------
  # pugixml —— wsdl 的 XML 后端
  # -------------------------------------------------------------------
  # 与 nghttp2 同一个形状（理由逐条相同，见上面那段长注释）：
  #   * function 造作用域，用**普通变量**压住上游的 option()；
  #   * BUILD_SHARED_LIBS 是全局的（我们默认 ON），必须显式压 OFF —— 否则
  #     pugixml 建的是 `pugixml-shared`，uvcpp.dll 就多出一条运行时依赖；
  #   * BUILD_TESTING 压 OFF：pugixml 的 CMakeLists 里有 include(CTest)，它会
  #     把测试开关带进来；
  #   * PIC 必须在建目标之前定好。
  #
  # ★ **nghttp2 能靠普通变量压住，pugixml 不能**，这是实测出来的分叉，不是风格
  #   选择：普通变量压 option() 要 CMP0077=NEW（CMake 3.13），而策略是**按各
  #   子目录自己的 cmake_minimum_required** 定的 —— 上游那句会把它重新按自己
  #   声明的版本重放一遍。nghttp2 声明 `VERSION 3.14`（≥3.13 ⇒ NEW，压得住），
  #   pugixml 声明 `VERSION 3.5`（<3.13 ⇒ CMP0077 未设 ⇒ 退回旧行为）。
  #   旧行为的动作是**把我们那个普通变量清掉**、转而用 cache 值，并在 configure
  #   期对每个这样的 option 印一条 `CMake Warning (dev)`。所以照着 nghttp2 的
  #   样子把 WCHAR_MODE/COMPACT/NO_XPATH/NO_STL/NO_EXCEPTIONS 逐个点一遍，效果
  #   是**五条警告 + 一点也没压住**（值仍是上游默认值，因为那些就是默认值）。
  #   这五条因此**故意不写**：要钉就只能用 cache+FORCE，那与"与 nghttp2 同形"
  #   相矛盾，而它们本来就是我们想要的那个值。
  #   **我们真正需要的三条都不受影响**：BUILD_SHARED_LIBS 不是 option()（上游
  #   直接读它），PIC 是普通变量且不经过 option()，PUGIXML_BUILD_TESTS 是
  #   cmake_dependent_option（条件里就有 `CMAKE_SOURCE_DIR 就是 PROJECT_SOURCE_DIR`
  #   这一条，add_subdirectory 进来必然为假）。下面那两条硬后置条件就是在证这件事。
  #
  # **异常保持开着**（上游默认）：pugixml 只在 OOM 时抛 bad_alloc，关掉它之后 OOM
  #   走的是 `assert` —— release 里 NDEBUG 一开就是空操作，接着解引用空指针。抛
  #   异常至少是定义良好的失败。这与 nlohmann 那条暴露面相同（见 uvcpp_web_json.h
  #   顶部），我们的封装只是保证**解析失败**（畸形 XML / 超限）一律走返回值。
  function(uvcpp_pugixml_configure)
    set(BUILD_SHARED_LIBS   OFF)
    set(PUGIXML_BUILD_TESTS OFF)
    set(BUILD_TESTING       OFF)

    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    FetchContent_GetProperties(pugixml)
    if(NOT pugixml_POPULATED)
      FetchContent_Populate(pugixml)
      add_subdirectory(${pugixml_SOURCE_DIR} ${pugixml_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
  endfunction()

  FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG        ${PUGIXML_VERSION}
    GIT_SHALLOW    TRUE
  )
  uvcpp_pugixml_configure()

  # 硬后置条件（与 nghttp2 那两条同形）：把"压住了没有"变成 configure 期的
  # 一句话，而不是等 Linux 的链接器或 Windows 的加载器来说话。
  if(NOT TARGET pugixml-static)
    message(FATAL_ERROR
      "pugixml 没建出 pugixml-static —— BUILD_SHARED_LIBS 没压住，或者上游改了库目标名")
  endif()
  get_target_property(_uvcpp_pugixml_pic pugixml-static POSITION_INDEPENDENT_CODE)
  if(NOT _uvcpp_pugixml_pic)
    message(FATAL_ERROR
      "pugixml-static 的 POSITION_INDEPENDENT_CODE 没生效(=${_uvcpp_pugixml_pic}) —— "
      "Linux 上 libuvcpp.so 会链不上它")
  endif()

  message(STATUS "pugixml integrated (tag=${PUGIXML_VERSION}, static)")
else()
  add_compile_definitions(UVCPP_WSDL_ENABLE=0)
  message(STATUS "WSDL/SOAP module disabled (UVCPP_WSDL_ENABLE=0)")
endif()

if(UVCPP_ENABLE_ZLIB)
  add_compile_definitions(UVCPP_ZLIB_ENABLE=1)
  message(STATUS "zlib enabled (UVCPP_ZLIB_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_ZLIB_ENABLE=0)
  message(STATUS "zlib disabled (UVCPP_ZLIB_ENABLE=0)")
endif()

if(UVCPP_ENABLE_OPENSSL)
  add_compile_definitions(UVCPP_OPENSSL_ENABLE=1)
  message(STATUS "OpenSSL enabled (UVCPP_OPENSSL_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_OPENSSL_ENABLE=0)
  message(STATUS "OpenSSL disabled (UVCPP_OPENSSL_ENABLE=0)")
endif()

if(UVCPP_ENABLE_NGHTTP2)
  add_compile_definitions(UVCPP_NGHTTP2_ENABLE=1)
  message(STATUS "HTTP/2 enabled (UVCPP_NGHTTP2_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_NGHTTP2_ENABLE=0)
  message(STATUS "HTTP/2 disabled (UVCPP_NGHTTP2_ENABLE=0)")
endif()

# 目录作用域这一对与下面 `_uvcpp_literal01` + `target_compile_definitions` 那两处
# **不是重复**：后者管的是库本身与 find_package 的消费者，而 tests/、examples/、
# bench/ 里有些 TU 在 include 公开头**之前**就用这个宏，靠的正是目录作用域。
# 八个既有模块都有这一对，照做。
if(UVCPP_ENABLE_QUIC)
  add_compile_definitions(UVCPP_QUIC_ENABLE=1)
  message(STATUS "QUIC enabled (UVCPP_QUIC_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_QUIC_ENABLE=0)
  message(STATUS "QUIC disabled (UVCPP_QUIC_ENABLE=0)")
endif()

if(UVCPP_ENABLE_HTTP3)
  add_compile_definitions(UVCPP_HTTP3_ENABLE=1)
  message(STATUS "HTTP/3 enabled (UVCPP_HTTP3_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_HTTP3_ENABLE=0)
  message(STATUS "HTTP/3 disabled (UVCPP_HTTP3_ENABLE=0)")
endif()

if(UVCPP_ENABLE_CAPI)
  add_compile_definitions(UVCPP_CAPI_ENABLE=1)
  message(STATUS "C ABI enabled (UVCPP_CAPI_ENABLE=1)")
else()
  add_compile_definitions(UVCPP_CAPI_ENABLE=0)
  message(STATUS "C ABI disabled (UVCPP_CAPI_ENABLE=0)")
endif()

if(UVCPP_ENABLE_TRY_WRITE)
  add_compile_definitions(UVCPP_TRY_WRITE_ENABLE=1
                          UVCPP_TRY_WRITE_MIN_BYTES=${UVCPP_TRY_WRITE_MIN_BYTES})
  message(STATUS "try_write fast path enabled (UVCPP_TRY_WRITE_ENABLE=1, min ${UVCPP_TRY_WRITE_MIN_BYTES} bytes)")
else()
  add_compile_definitions(UVCPP_TRY_WRITE_ENABLE=0)
  message(STATUS "try_write fast path disabled (UVCPP_TRY_WRITE_ENABLE=0)")
endif()

# 根据 UVCPP_BUILD_EXPAND 选项排除 expand 模块
if(NOT UVCPP_BUILD_EXPAND)
  message(STATUS "Excluding expand module from build")
  # 整目录排除，别再逐文件列举：uvcpp_memory_pool.cpp 就是这么漏掉的，
  # EXPAND=OFF 时它仍被编进库。下面的 net/web/webapp/ssl 都用 FILTER 整目录。
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/expand/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/expand/")
else()
  message(STATUS "Including expand module in build (TCMalloc-like allocator)")
endif()

# 根据 UVCPP_BUILD_NET 选项排除 net 模块
if(NOT UVCPP_BUILD_NET)
  message(STATUS "Excluding net module from build")
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/net/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/net/")
else()
  message(STATUS "Including net module in build")
endif()

# 根据 UVCPP_BUILD_WEB 选项排除 web 模块
if(NOT UVCPP_BUILD_WEB)
  message(STATUS "Excluding web module from build")
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/web/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/web/")
else()
  message(STATUS "Including web module in build")
endif()

# 根据 UVCPP_BUILD_WEBAPP 选项排除 webapp 模块
if(NOT UVCPP_BUILD_WEBAPP)
  message(STATUS "Excluding webapp module from build")
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/webapp/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/webapp/")
else()
  message(STATUS "Including webapp module in build")
endif()

# 根据 UVCPP_ENABLE_OPENSSL 排除 ssl 模块
if(NOT UVCPP_ENABLE_OPENSSL)
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/ssl/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/ssl/")
else()
  message(STATUS "Including ssl module in build (OpenSSL ${OPENSSL_VERSION})")
endif()

# 根据 UVCPP_ENABLE_NGHTTP2 排除 http2 模块
if(NOT UVCPP_ENABLE_NGHTTP2)
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/http2/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/http2/")
else()
  message(STATUS "Including http2 module in build (nghttp2 ${NGHTTP2_VERSION})")
endif()

# 根据 UVCPP_ENABLE_WSDL 排除 wsdl 模块
if(NOT UVCPP_ENABLE_WSDL)
  message(STATUS "Excluding wsdl module from build")
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/wsdl/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/wsdl/")
else()
  message(STATUS "Including wsdl module in build")
endif()

# 根据 UVCPP_ENABLE_QUIC 排除 quic 模块。
# 两个列表都要排：UVCPP_SOURCES 管编不编，UVCPP_HEADERS 管这些头会不会被当成
# 「本库的公开头」参与后续的安装/导出判断。
if(NOT UVCPP_ENABLE_QUIC)
  message(STATUS "Excluding quic module from build")
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/quic/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/quic/")
else()
  # CI 要 grep 这一行 —— 开关的降级是普通变量 set()，降级之后 CMakeCache.txt 里
  # 仍然是 `UVCPP_ENABLE_QUIC:BOOL=ON`，只 grep cache 会得到一个假绿。
  message(STATUS "Including quic module in build (ngtcp2 ${NGTCP2_VERSION})")

  # 「开了 QUIC 但目录是空的」必须当场红，不能绿。
  # 否则那棵树能同时骗过三道看起来很像门禁的东西：cache 里 QUIC=ON、
  # configure 日志里有 "ngtcp2 integrated"、编译也过 —— 而 src/quic/ 一个 .cpp
  # 都没有，**零行 QUIC 代码被编译过**。这与 h2 job 立三道门禁是同一个教训：
  # 「没测」必须表现为**失败**，而不是表现为**通过**。
  if(NOT QUIC_SRC_FILES)
    message(FATAL_ERROR
      "UVCPP_ENABLE_QUIC=ON 但 src/quic/ 下一个文件都没有 —— 这个配置会编出"
      "一个不含任何 QUIC 代码的库，而且一路绿。要么把源文件补上，要么关掉这个开关。")
  endif()
endif()

# 根据 UVCPP_ENABLE_HTTP3 排除 http3 模块。形状与上面 quic 那段逐字相同 ——
# 包括为什么两个列表都要排。
if(NOT UVCPP_ENABLE_HTTP3)
  message(STATUS "Excluding http3 module from build")
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/http3/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/http3/")
else()
  # CI 要 grep 这一行，理由与 quic 那句相同（降级是普通变量 set()，cache 里
  # 仍然是 ON）。
  message(STATUS "Including http3 module in build (nghttp3 ${NGHTTP3_VERSION})")

  # 「开了 HTTP3 但目录是空的」同样必须当场红。这一格比 quic 那格更值得防：
  # h3 的公开面在 web 层里是**可选接线**，一个空的 src/http3/ 不会让任何东西
  # 编译失败 —— 它会编出一个"h3 开关是 ON、但一条 h3 代码都没有"的库，
  # 而配置日志、cache、构建全绿。
  if(NOT HTTP3_SRC_FILES)
    message(FATAL_ERROR
      "UVCPP_ENABLE_HTTP3=ON 但 src/http3/ 下一个文件都没有 —— 这个配置会编出"
      "一个不含任何 HTTP/3 代码的库，而且一路绿。要么把源文件补上，要么关掉这个开关。")
  endif()
endif()

# 根据 UVCPP_ENABLE_CAPI 排除 capi 模块。形状与上面 quic/http3 那两段逐字相同
# —— 包括为什么两个列表都要排（SOURCES 管编不编，HEADERS 管会不会进安装/导出）。
if(NOT UVCPP_ENABLE_CAPI)
  message(STATUS "Excluding capi module from build")
  list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/capi/")
  list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/capi/")
else()
  # CI 要 grep 这一行，理由与 quic/http3 那两句相同（降级是普通变量 set()，
  # cache 里仍然是 ON）。
  message(STATUS "Including capi module in build (C ABI)")

  # 「开了 CAPI 但目录是空的」必须当场红，不能绿 —— 这一格比 quic/http3 那两格
  # 更值得防：C 面是**纯增量**的（它不接进任何既有编译路径，没有它别的模块照样
  # 编得过、跑得动），所以一个空的 src/capi/ 会编出一个"开关是 ON、一行 C 面
  # 代码都没有"的库，而 cache、configure 日志、构建、测试**全绿**。
  if(NOT CAPI_SRC_FILES)
    message(FATAL_ERROR
      "UVCPP_ENABLE_CAPI=ON 但 src/capi/ 下一个文件都没有 —— 这个配置会编出"
      "一个不含任何 C ABI 代码的库，而且一路绿。要么把源文件补上，要么关掉这个开关。")
  endif()

  # ---------------------------------------------------------------------------
  # 子模块过滤：C 面**按模块分文件**，每一份子头该不该存在，由它背后那个模块
  # 的开关说了算 —— 与上面 `src/webapp/`、`src/http2/` 那两段的纪律是同一条，
  # 只是这一层多了一级（capi 挂在 web 上，而 webapp / h2 / quic / h3 是它的
  # 可选下层）。`UVCPP_ENABLE_CAPI` 的守卫链只保证 NET + WEB 必然开着，
  # **不保证** webapp 开着（它默认 OFF），更不保证 h2 / quic / h3 开着。
  #
  # 这一段是**量出来的**，不是预防性加上去的。批 2 推上去之后，Linux 与 macOS
  # 的 `full` 格（`WEB=ON`、`WEBAPP` 取默认 OFF、`CAPI=ON`）第一次就红了，红的
  # 是 `src/webapp/uvcpp_web_json.h:39: fatal error: nlohmann/json.hpp: No such
  # file or directory`，编译的单元是 `src/capi/uvcpp_c_webapp.cpp` —— 看起来像
  # "依赖装漏了"，实际是"模块过滤漏了"：nlohmann 是 `if(UVCPP_BUILD_WEBAPP)`
  # 那段 `target_link_libraries` 挂上去的，而 cpp 那边照样把这个 TU 塞进了源列表。
  # 与 1.4.2 那次 `9ee0027` 是同一条教训的第三次：**C 面不许依赖一个它够不着的
  # 特性** —— 前两次是"代码里 include 了一个没开的模块的头"，这一次是"构建图上
  # 编了一个没开的模块的文件"。
  #
  # 为什么过滤写在 CMake 而不是文件里的 `#if UVCPP_XXX_ENABLE`：与 C++ 那边同一个
  # 理由 —— "哪些 .cpp 进了这一趟编译"在 `cmake --build` 的源列表里看得见，写在
  # `#if` 里则要编一遍才知道，而"编了一个空的 TU"是**全绿**的。
  #
  # 这张表是唯一一处写这件事的地方：源列表与安装头两处都从 `UVCPP_CAPI_DISABLED`
  # 派生（安装那处在文件末尾，见 `CAPI_HEADER_FILES`）。
  set(UVCPP_CAPI_DISABLED "")
  foreach(_uvcpp_capi_pair
      "UVCPP_BUILD_WEBAPP:uvcpp_c_webapp"     # app / 路由 / 中间件 / 静态 / ws 路由
      "UVCPP_ENABLE_NGHTTP2:uvcpp_c_http2"    # h2 session / connection
      "UVCPP_ENABLE_QUIC:uvcpp_c_quic"        # QUIC client / server / connection
      "UVCPP_ENABLE_HTTP3:uvcpp_c_http3"      # h3 connection
    )
    string(REPLACE ":" ";" _uvcpp_capi_mv "${_uvcpp_capi_pair}")
    list(GET _uvcpp_capi_mv 0 _uvcpp_capi_opt)
    list(GET _uvcpp_capi_mv 1 _uvcpp_capi_base)
    if(NOT ${_uvcpp_capi_opt})
      list(FILTER UVCPP_SOURCES EXCLUDE REGEX "src/capi/${_uvcpp_capi_base}\\.cpp$")
      list(FILTER UVCPP_HEADERS EXCLUDE REGEX "src/capi/${_uvcpp_capi_base}\\.h$")
      list(APPEND UVCPP_CAPI_DISABLED "${_uvcpp_capi_base}")
      message(STATUS "  capi: 排除 ${_uvcpp_capi_base}（${_uvcpp_capi_opt}=OFF）")
    endif()
  endforeach()
endif()

# Create uvcpp targets according to user selection.
# Shared target
if(UVCPP_BUILD_SHARED)
  add_library(uvcpp SHARED ${UVCPP_SOURCES} ${UVCPP_HEADERS})
  target_link_libraries(uvcpp PRIVATE ${LIBUV_LIB})
  if (WIN32)
    target_compile_definitions(uvcpp PRIVATE UVCPP_EXPORTS)
    target_link_libraries(uvcpp PRIVATE ws2_32)
  endif()
  if (UVCPP_BUILD_LIBUV_FROM_SOURCE)
    add_library(uvcpp_s SHARED ${UVCPP_SOURCES} ${UVCPP_HEADERS})
    target_link_libraries(uvcpp_s PRIVATE ${LIBUV_LIB})
    if (WIN32)
      target_compile_definitions(uvcpp_s PRIVATE UVCPP_EXPORTS)
      target_link_libraries(uvcpp_s PRIVATE ws2_32)
    endif()
  endif()
endif()

# Static target
if(UVCPP_BUILD_STATIC)
  add_library(uvcpp_a STATIC ${UVCPP_SOURCES} ${UVCPP_HEADERS})
  target_link_libraries(uvcpp_a PRIVATE ${LIBUV_LIB})
  if (WIN32)
    target_link_libraries(uvcpp_a PRIVATE ws2_32)
  endif()
  if (UVCPP_BUILD_LIBUV_FROM_SOURCE)
    add_library(uvcpp_a_s STATIC ${UVCPP_SOURCES} ${UVCPP_HEADERS})
    target_link_libraries(uvcpp_a_s PRIVATE ${LIBUV_LIB})
    if (WIN32)
      target_link_libraries(uvcpp_a_s PRIVATE ws2_32)
    endif()
  endif()
endif()

# Use target-based include directories so exported/installable targets do not
# contain absolute source-tree paths. During build use the source include
# directory; when installed consumers should use the installed "include".
if (TARGET uvcpp)
  target_include_directories(uvcpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_a)
  target_include_directories(uvcpp_a PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_s)
  target_include_directories(uvcpp_s PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_a_s)
  target_include_directories(uvcpp_a_s PUBLIC
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()

# HTTP/3：nghttp3 的 include 目录**得我们自己挂**,不能指望链接传递。
#
# 与 ngtcp2 那一处**实质不同**,别照着它以为"链上就有了":ngtcp2 的
# `lib/CMakeLists.txt:131` 给 `ngtcp2_static` 挂了 `PUBLIC ${ngtcp2_INCLUDE_DIRS}`,
# 所以 ngtcp2 的头顺着 target_link_libraries 自己流过来;nghttp3 的
# `lib/CMakeLists.txt:115` 只写了 `INTERFACE $<INSTALL_INTERFACE:include>` ——
# build 侧**一个字都没有**,它自己那些头是靠 `lib/CMakeLists.txt:27-30` 的
# **目录级** `include_directories()` 找到的,那个作用域出不了 nghttp3 自己的目录。
# 不挂这两条,`uvcpp_h3_nghttp3.h` 里那句 `#include <nghttp3/nghttp3.h>` 直接
# 找不到文件。两条都要:前者是源码树里的 `lib/includes`,后者是 configure_file
# 生成的 `nghttp3/version.h`(它落在构建树里)。
#
# PRIVATE:只有 `src/http3/*.cpp` 需要它,公开头一个字都不引用 nghttp3 —— 这正是
# `uvcpp_h3_nghttp3.h` 存在且**不装出去**的理由。这里**不套** `$<BUILD_INTERFACE:>`,
# 而上面那些链接依赖套了:PRIVATE 的 include 目录根本不会进
# `INTERFACE_INCLUDE_DIRECTORIES`,绝对路径没有机会被 `install(EXPORT)` 看见;
# 而 PRIVATE 的**链接**依赖会在静态库上以 `$<LINK_ONLY:...>` 写进
# `INTERFACE_LINK_LIBRARIES`,那才是上面必须包起来的原因。包在这里只会让人以为
# 它在防一件不存在的事。
if(UVCPP_ENABLE_HTTP3)
  foreach(_tgt uvcpp uvcpp_s uvcpp_a uvcpp_a_s)
    if(TARGET ${_tgt})
      target_include_directories(${_tgt} PRIVATE
        "${_uvcpp_nghttp3_src}/lib/includes"
        "${_uvcpp_nghttp3_bin}/lib/includes")
    endif()
  endforeach()
endif()

# MSVC：`/Zc:preprocessor`（符合标准的预处理器）必须**跟着导出的目标走**。
#
# 理由见 `src/uvcpp/uvcpp_json_reflect.h` 里 `UVCPP_JSON_FIELDS` 上面那段：
# 传统预处理器转发不了 `__VA_ARGS__`，那个宏在传统模式下编不过。上面 `/utf-8`
# 那处用的是 `add_compile_options`，而它是**目录作用域**的、进不了导出集
# （那段注释里已经写明这条）—— 所以这里按目标挂 INTERFACE：`find_package(uvcpp)`
# 的消费者自动继承，本仓的测试/示例/bench 也走同一条路（它们都 link 这四个
# 目标之一，INTERFACE 选项会跟着传下去）。
#
# **故意不用 `add_compile_options`**：那会顺着 `add_subdirectory` 连 zlib / libuv
# 的 **C 源**一起带上（`/utf-8` 带上无妨，替别人的构建改预处理器不是本仓的事）。
# `clang-cl` 不认这个开关，所以只给 cl —— 而且**判的是消费者的编译器**：
# 生成器表达式在拿到这个目标的工程里求值，`find_package(uvcpp)` 的人是 clang-cl
# 就一个字节都收不到。
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  foreach(_uvcpp_tgt uvcpp uvcpp_a uvcpp_s uvcpp_a_s)
    if(TARGET ${_uvcpp_tgt})
      # 生成器表达式！`INTERFACE` 选项会被原样写进导出集、在**消费者**那边
      # 求值，写死字面量的话 clang-cl 的消费者会白挨一个它不认的开关。
      target_compile_options(${_uvcpp_tgt} INTERFACE
                             $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:preprocessor>)
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# 生成 uvcpp_config.h —— 让「宏集与 DLL 不一致」从运行时垃圾值变成编译期硬失败
# ---------------------------------------------------------------------------
# 下面那批 PUBLIC 编译定义只覆盖 `find_package(uvcpp)` 这条路径。走**预编译包 +
# pkg-config** 的消费者不经过 CMake，一个宏都拿不到：宏未定义 ⇒ 公开头里的
# `#if UVCPP_XXX_ENABLE` 求值为 0 ⇒ 被它守卫的**成员布局**与 DLL 不一致，
# 内联访问器按错误偏移读成员，**静默**返回垃圾值（无编译错误、无链接错误、
# 无运行时告警）。所以把这份宏集再落成一个头，由每个公开头在**自己的模块守卫
# 之前**包含：
#     外部已定义且与本包不同 -> #error（硬失败）
#     外部未定义             -> 取本包的构建值
# 后一半才是让"一个宏都不写"的消费者**正确**的那一半。
#
# 位置必须在**所有静默降级之后**（UVCPP_ENABLE_OPENSSL/_NGHTTP2/_BUILD_WEBAPP
# 在依赖缺失时会被改成 OFF，见上面）：放在选项区会生成一份说 1、而编译定义说 0
# 的头，默认构建的第一个 TU 就 #error。
#
# 值先落成字面量，**同时**喂给 configure_file 与 target_compile_definitions：
# 两处从结构上不可能漂移 —— 这份头存在的理由就是消掉"多份真相源"，它自己不能
# 变成新的一份。
function(_uvcpp_literal01 _out _var)
  if(${_var})
    set(${_out} 1 PARENT_SCOPE)
  else()
    set(${_out} 0 PARENT_SCOPE)
  endif()
endfunction()

# 注意：宏叫 UVCPP_ENABLE_MEMORY_POOL，但开关选项是 UVCPP_BUILD_EXPAND。
_uvcpp_literal01(UVCPP_ENABLE_MEMORY_POOL UVCPP_BUILD_EXPAND)
_uvcpp_literal01(UVCPP_NET_ENABLE       UVCPP_BUILD_NET)
_uvcpp_literal01(UVCPP_WEB_ENABLE       UVCPP_BUILD_WEB)
_uvcpp_literal01(UVCPP_WEBAPP_ENABLE    UVCPP_BUILD_WEBAPP)
_uvcpp_literal01(UVCPP_WSDL_ENABLE      UVCPP_ENABLE_WSDL)
_uvcpp_literal01(UVCPP_ZLIB_ENABLE      UVCPP_ENABLE_ZLIB)
_uvcpp_literal01(UVCPP_OPENSSL_ENABLE   UVCPP_ENABLE_OPENSSL)
_uvcpp_literal01(UVCPP_NGHTTP2_ENABLE   UVCPP_ENABLE_NGHTTP2)
_uvcpp_literal01(UVCPP_QUIC_ENABLE      UVCPP_ENABLE_QUIC)
_uvcpp_literal01(UVCPP_HTTP3_ENABLE     UVCPP_ENABLE_HTTP3)
_uvcpp_literal01(UVCPP_CAPI_ENABLE      UVCPP_ENABLE_CAPI)
_uvcpp_literal01(UVCPP_TRY_WRITE_ENABLE UVCPP_ENABLE_TRY_WRITE)

# 值宏：空字面量会让 `#if X != ` 变成语法错误，宁可在 configure 期就停下。
if("${UVCPP_TRY_WRITE_MIN_BYTES}" STREQUAL "")
  message(FATAL_ERROR "UVCPP_TRY_WRITE_MIN_BYTES 为空，生成头会产出空字面量")
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include/uvcpp")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uvcpp_config.h.in"
  "${CMAKE_BINARY_DIR}/include/uvcpp/uvcpp_config.h"
  @ONLY)

# nlohmann/json 必须 **PUBLIC** 链接：webapp 的公开头（uvcpp_web_json.h、
# uvcpp_web_request.h、uvcpp_web_response.h）里出现了 uvcpp_json（即
# nlohmann::json），使用者的翻译单元也要能看到它的头文件。
#
# 它是 INTERFACE 库（header-only），所以不产生 DLL，CI 的 DLL 拷贝步骤
# 不需要为它加项。
if (UVCPP_BUILD_WEBAPP)
  foreach (_uvcpp_t uvcpp uvcpp_a uvcpp_s uvcpp_a_s)
    if (TARGET ${_uvcpp_t})
      target_link_libraries(${_uvcpp_t} PUBLIC nlohmann_json::nlohmann_json)
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# 模块使能宏必须作为 **PUBLIC 编译定义**挂在库目标上
# ---------------------------------------------------------------------------
# 上面那批 `add_compile_definitions(UVCPP_XXX_ENABLE=...)` 是**目录作用域**的：
# 它们只作用于本工程内编译的单元，**不会进入导出的 uvcppTargets.cmake**。
#
# 而公开头文件里到处是这种守卫：
#     src/web/uvcpp_http_request.h:  #if UVCPP_WEB_ENABLE
#     src/uvcpp/uvcpp_alloc.h:       #if UVCPP_ENABLE_MEMORY_POOL
#     src/ssl/uvcpp_ssl.h:           #if UVCPP_OPENSSL_ENABLE
#     src/web/uvcpp_http_compress.h: #if UVCPP_ZLIB_ENABLE
#
# 外部工程 `find_package(uvcpp)` 之后这些宏都是**未定义**的，`#if` 求值为 0，
# 于是：
#   - web/ssl 的类被整段编译掉 —— 使用者拿到的是"类不存在"的级联语法错误；
#   - `UVCPP_ENABLE_MEMORY_POOL` 更要命：uvcpp_alloc.h 会走另一条分支，
#     使用者 TU 里的分配器与已编译的 uvcpp.dll **不是同一套**，是 ABI 不一致。
#
# 这一条只有真正被外部工程引用时才会暴露：本仓库自己的构建和测试都在同一个
# 目录作用域下，宏永远是有定义的，所以怎么测都是绿的。
foreach (_uvcpp_t uvcpp uvcpp_a uvcpp_s uvcpp_a_s)
  if (TARGET ${_uvcpp_t})
    # 值取自上面 `_uvcpp_literal01` 算出的那一份，与 uvcpp_config.h 同源。
    target_compile_definitions(${_uvcpp_t} PUBLIC
      UVCPP_ENABLE_MEMORY_POOL=${UVCPP_ENABLE_MEMORY_POOL}
      UVCPP_NET_ENABLE=${UVCPP_NET_ENABLE}
      UVCPP_WEB_ENABLE=${UVCPP_WEB_ENABLE}
      UVCPP_WEBAPP_ENABLE=${UVCPP_WEBAPP_ENABLE}
      UVCPP_WSDL_ENABLE=${UVCPP_WSDL_ENABLE}
      UVCPP_ZLIB_ENABLE=${UVCPP_ZLIB_ENABLE}
      UVCPP_OPENSSL_ENABLE=${UVCPP_OPENSSL_ENABLE}
      UVCPP_NGHTTP2_ENABLE=${UVCPP_NGHTTP2_ENABLE}
      UVCPP_QUIC_ENABLE=${UVCPP_QUIC_ENABLE}
      UVCPP_HTTP3_ENABLE=${UVCPP_HTTP3_ENABLE}
      UVCPP_CAPI_ENABLE=${UVCPP_CAPI_ENABLE}
      UVCPP_TRY_WRITE_ENABLE=${UVCPP_TRY_WRITE_ENABLE}
      UVCPP_TRY_WRITE_MIN_BYTES=${UVCPP_TRY_WRITE_MIN_BYTES}
    )
  endif()
endforeach()

# ---------------------------------------------------------------------------
# C ABI 的导出宏：`UVCPP_C_API`（**不是** `UVCPP_API`）
# ---------------------------------------------------------------------------
# 上面那一批 `UVCPP_XXX_ENABLE` 说的是"这份库里有哪些模块"；这一条说的是"这份
# 产物是 DLL 还是静态库"，而 C 面的那枚宏需要这个信息（见
# `src/capi/uvcpp_c_common.h` 里它自己的注释）：
#
#   - `UVCPP_C_BUILD`  → **私有**定义在 SHARED 目标上：编 DLL 自己那一趟，
#                        `extern "C" __declspec(dllexport)` 出去；
#   - `UVCPP_C_SHARED` → **PUBLIC** 定义在 SHARED 目标上：消费一份 DLL 的人
#                        （`find_package(uvcpp)` 之后）一路继承到
#                        `__declspec(dllimport)`；
#   - 静态目标两个都不定义：`UVCPP_C_API` 展开成空，与今天的行为一致。
#
# 为什么**不复用** `UVCPP_EXPORTS` / `UVCPP_IMPORTS`：那一对是本工程自己的
# "编库/用库"标记（`uvcpp_export.h` 里解释过 `UVCPP_IMPORTS` 为什么从来不定义
# ——本仓自己的 TU 全在 `UVCPP_EXPORTS` 这一趟里）。C 面多要一条信息：**跨 DLL
# 边界**。混用会让"静态链接"和"用一份 DLL"这两种情况互相看不出区别，而这正是
# MSVC 上会退化成一跳间接调用、并与导入库对不上时症状极难认的那一格。
#
# 只在 WIN32 上定义：那两个宏的判据本身就是 `defined(_WIN32) && ...`，在别的
# 平台上定义它们只是往 INTERFACE 里塞噪音。
if(WIN32)
  foreach (_uvcpp_t uvcpp uvcpp_s)
    if (TARGET ${_uvcpp_t})
      target_compile_definitions(${_uvcpp_t} PRIVATE UVCPP_C_BUILD)
      target_compile_definitions(${_uvcpp_t} PUBLIC  UVCPP_C_SHARED)
    endif()
  endforeach()
endif()

# Add debug postfix 'd' for debug builds so debug artifacts are distinguishable.
if (TARGET uvcpp)
  set_target_properties(uvcpp PROPERTIES DEBUG_POSTFIX "d")
endif()
if (TARGET uvcpp_s)
  set_target_properties(uvcpp_s PROPERTIES DEBUG_POSTFIX "d")
endif()
if (TARGET uvcpp_a)
  set_target_properties(uvcpp_a PROPERTIES DEBUG_POSTFIX "d")
endif()
if (TARGET uvcpp_a_s)
  set_target_properties(uvcpp_a_s PROPERTIES DEBUG_POSTFIX "d")
endif()

# =========================================================================
# Web 模块依赖链接 — 为所有 uvcpp target 附加 llhttp / zlib
# =========================================================================
if(UVCPP_BUILD_WEB)
  foreach(_tgt uvcpp uvcpp_s uvcpp_a uvcpp_a_s)
    if(TARGET ${_tgt})
      target_link_libraries(${_tgt} PRIVATE llhttp::llhttp)
      if(UVCPP_ENABLE_ZLIB)
        # zlib 必须 PUBLIC（不能像 llhttp 那样 PRIVATE）：公开且会被安装的
        # `src/web/uvcpp_ws_parser.h` 在 `#if UVCPP_ZLIB_ENABLE` 里直接
        # `#include <zlib.h>`，而该宏是 PUBLIC 编译定义，所以每个引用 uvcpp 的
        # TU 都需要 zlib 的头与符号。PRIVATE 的后果是 Windows C1083 ×14、
        # POSIX 链接失败（测试 TU 自己直接调 zlib）。
        #
        # 包 `$<BUILD_INTERFACE:>` 是必需的：PUBLIC 会写进
        # INTERFACE_LINK_LIBRARIES，而 install(EXPORT uvcppTargets) 要求导出集里
        # 每个非 IMPORTED 目标也在导出集里，FetchContent 的 zlibstatic 不是，
        # generate 阶段会直接失败。包上之后本工程内照常传播，导出里被剥成空。
        #
        # 优先 ZLIB::ZLIB：自带 INTERFACE 包含目录，且正确处理 Debug/Release
        # （${ZLIB_LIBRARIES} 是带 optimized/debug 关键字的裸路径）。
        #
        # 既有缺口（本次不动）：导出集里没有 zlib，而安装的
        # uvcpp_ws_parser.h 会 #include <zlib.h>。补它要加
        # find_dependency(ZLIB)，会让没装 zlib 的使用者 find_package 就失败。
        if(TARGET zlibstatic)
          target_link_libraries(${_tgt} PUBLIC $<BUILD_INTERFACE:zlibstatic>)
        elseif(TARGET ZLIB::ZLIB)
          target_link_libraries(${_tgt} PUBLIC $<BUILD_INTERFACE:ZLIB::ZLIB>)
        elseif(ZLIB_FOUND)
          target_link_libraries(${_tgt} PUBLIC $<BUILD_INTERFACE:${ZLIB_LIBRARIES}>)
          target_include_directories(${_tgt} PUBLIC $<BUILD_INTERFACE:${ZLIB_INCLUDE_DIRS}>)
        endif()
      endif()
    endif()
  endforeach()
endif()

if(UVCPP_ENABLE_OPENSSL)
  foreach(_tgt uvcpp uvcpp_s uvcpp_a uvcpp_a_s)
    if(TARGET ${_tgt})
      target_link_libraries(${_tgt} PRIVATE ${UVCPP_SSL_LIBS})
      if(WIN32)
        target_link_libraries(${_tgt} PRIVATE crypt32 user32 advapi32)
      endif()
    endif()
  endforeach()
endif()

# =========================================================================
# HTTP/2 依赖链接 — nghttp2
# =========================================================================
if(UVCPP_ENABLE_NGHTTP2)
  foreach(_tgt uvcpp uvcpp_s uvcpp_a uvcpp_a_s)
    if(TARGET ${_tgt})
      # 用 nghttp2 自己给的那个别名,而不是 `nghttp2_static` 这个名字:
      # STATIC_LIB 是它内部的私有变量,漂过(1.60/1.61 那次同名缺陷);而
      # `add_library(nghttp2 ALIAS ${LIB_SELECTED})` 是它对外承诺的稳定名字。
      # 我们已在 function 里把 BUILD_SHARED_LIBS 压成 OFF,所以它必然指向静态那个。
      #
      # PRIVATE:nghttp2 只出现在 src/http2/ 的 .cpp 里(pimpl),公开头一个字都不
      # 引用它 —— 使用者既不需要它的 include 也不需要它的 lib。
      #
      # 包 $<BUILD_INTERFACE:> 的理由与上面 zlib 那段逐字相同:PRIVATE 在**静态**
      # 库上会以 $<LINK_ONLY:...> 写进 INTERFACE_LINK_LIBRARIES,而
      # install(EXPORT uvcppTargets) 要求导出集里每个非 IMPORTED 目标也在导出集
      # 里,nghttp2_static 不在。包上之后本工程内照常传播,导出里被剥成空。
      target_link_libraries(${_tgt} PRIVATE $<BUILD_INTERFACE:nghttp2>)
    endif()
  endforeach()
endif()

# =========================================================================
# QUIC 依赖链接 — ngtcp2
# =========================================================================
if(UVCPP_ENABLE_QUIC)
  foreach(_tgt uvcpp uvcpp_s uvcpp_a uvcpp_a_s)
    if(TARGET ${_tgt})
      # 两个目标,不是一个,而且**都必须写死在这里**:
      #
      # * `ngtcp2_static` —— 协议状态机本身;
      # * `ngtcp2_crypto_ossl_static` —— 把 ngtcp2 的 crypto 回调接到 OpenSSL
      #   EVP/QUIC 那套上。少了后者,`ngtcp2_crypto_*` 的符号在 `uvcpp_quic_ngtcp2.cpp`
      #   里全成未定义;只写 `ngtcp2_static` 编得过、链不过,报错还落在别人家里。
      #
      # **这里用的是 ngtcp2 的裸目标名,没有别名可用** —— 与上面 nghttp2 那段不同,
      # ngtcp2 的 `lib/CMakeLists.txt` 只 `add_library(ngtcp2_static ...)`,从头到尾
      # 没有一句 `add_library(... ALIAS ...)`。所以名字漂了的风险只能靠上面配置段
      # 那两条 `if(NOT TARGET ...)` 的 FATAL_ERROR 兜住(照 nghttp2 那处 :416/:421 的
      # 两道断言),不能靠"用别名更稳"。
      #
      # PRIVATE 与 $<BUILD_INTERFACE:> 的理由与上面 nghttp2 那段逐字相同:
      # 只出现在 `src/quic/` 的 .cpp 里(pimpl),公开头一个字都不引用 ngtcp2;
      # 而 PRIVATE 在**静态**库上会以 $<LINK_ONLY:...> 写进
      # INTERFACE_LINK_LIBRARIES,`install(EXPORT uvcppTargets)` 要求导出集里每个
      # 非 IMPORTED 目标也在导出集里 —— 这两个 ngtcp2 目标都不在。
      target_link_libraries(${_tgt} PRIVATE
        $<BUILD_INTERFACE:ngtcp2_static>
        $<BUILD_INTERFACE:ngtcp2_crypto_ossl_static>)
    endif()
  endforeach()
endif()

# =========================================================================
# HTTP/3 依赖链接 — nghttp3
# =========================================================================
if(UVCPP_ENABLE_HTTP3)
  foreach(_tgt uvcpp uvcpp_s uvcpp_a uvcpp_a_s)
    if(TARGET ${_tgt})
      # 一个目标,裸名,**没有 ALIAS** —— 与上面 ngtcp2 那段同形,与 nghttp2 不同
      # (nghttp2 有 `add_library(nghttp2 ALIAS ...)`)。所以名字漂了只能靠上面配置段
      # 那条 `if(NOT TARGET nghttp3_static)` 的 FATAL_ERROR 兜住。
      #
      # **include 目录不在这里,在上面那段 `target_include_directories` 里** ——
      # 这是我们与 ngtcp2 那段唯一的实质差别:ngtcp2_static 挂了 PUBLIC 的 include
      # 目录,链上就顺过来了;nghttp3_static 只给 INSTALL_INTERFACE,链**不**带
      # include。别以为"照 ngtcp2 抄一行 link 就够了"。
      #
      # PRIVATE 与 $<BUILD_INTERFACE:> 的理由与上面 nghttp3 的 include 段逐字相同:
      # nghttp3 只出现在 `src/http3/` 的 .cpp 与那个**不装出去**的
      # `uvcpp_h3_nghttp3.h` 里,公开头一个字都不引用它;而 PRIVATE 在**静态**库上
      # 会以 $<LINK_ONLY:...> 写进 INTERFACE_LINK_LIBRARIES,
      # `install(EXPORT uvcppTargets)` 要求导出集里每个非 IMPORTED 目标也在导出集
      # 里 —— nghttp3_static 不在。
      target_link_libraries(${_tgt} PRIVATE $<BUILD_INTERFACE:nghttp3_static>)
    endif()
  endforeach()
endif()

# =========================================================================
# WSDL 依赖链接 — pugixml
# =========================================================================
if(UVCPP_ENABLE_WSDL)
  foreach(_tgt uvcpp uvcpp_s uvcpp_a uvcpp_a_s)
    if(TARGET ${_tgt})
      # PRIVATE：pugixml 只出现在 src/wsdl/ 的 .cpp 与那个**不装出去**的
      # uvcpp_wsdl_pugixml.h 里，公开头一个字都不引用它 —— 使用者既不需要它的
      # include 也不需要它的 lib。
      #
      # 包 $<BUILD_INTERFACE:> 的理由与 zlib/nghttp2 那两段逐字相同（PRIVATE 在
      # 静态库上会以 $<LINK_ONLY:> 进 INTERFACE_LINK_LIBRARIES，而 install(EXPORT)
      # 要求导出集里每个非 IMPORTED 目标也在导出集里，pugixml-static 不在）。
      #
      # 用上游自己承诺的别名 `pugixml::pugixml`，而不是 `pugixml-static` 这个
      # 库目标名 —— 别名是它对外的稳定接口，目标名漂过（nghttp2 那次同名缺陷）。
      target_link_libraries(${_tgt} PRIVATE $<BUILD_INTERFACE:pugixml::pugixml>)
    endif()
  endforeach()
endif()

# Select the library target that tests/consumers should link to by default.
# Default to the shared non-source `uvcpp` target if present.
if (TARGET uvcpp)
  set(UVCPP_LIB_TARGET uvcpp CACHE INTERNAL "Which uvcpp target tests should link to")
elseif(TARGET uvcpp_s)
  set(UVCPP_LIB_TARGET uvcpp_s CACHE INTERNAL "Which uvcpp target tests should link to")
elseif(TARGET uvcpp_a)
  set(UVCPP_LIB_TARGET uvcpp_a CACHE INTERNAL "Which uvcpp target tests should link to")
elseif(TARGET uvcpp_a_s)
  set(UVCPP_LIB_TARGET uvcpp_a_s CACHE INTERNAL "Which uvcpp target tests should link to")
else()
  message(FATAL_ERROR "No uvcpp target was created.")
endif()

if (UVCPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  option(UVCPP_BUILD_FUNCTIONAL "Build functional tests" ON)
  if (UVCPP_BUILD_FUNCTIONAL)
    add_subdirectory(tests/functional)
  endif()
  if (UVCPP_BUILD_EXPAND)
    add_subdirectory(tests/expand)
  endif()
  # C ABI 的用例（tests/capi/）。只在开关打开时**存在**：关掉时它们不进 ctest，
  # 于是 `ctest -R capi` 会报"没有匹配的用例"而不是"通过" —— 那正是本仓要的形状
  # （「没测」不许长得像「测过且通过」）。守卫放在这里而不是 tests/capi/CMakeLists.txt
  # 里，是为了让那个文件只讲一件事：这些用例长什么样。
  if (UVCPP_ENABLE_CAPI)
    add_subdirectory(tests/capi)
  endif()
endif()

# Only add the legacy req tests subdirectory if it still exists (we consolidated tests into unit/)
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/req/CMakeLists.txt")
  add_subdirectory(tests/req)
endif()

# 示例。放在这里（而不是靠前）是因为上面那些 add_compile_definitions 是按目录
# 生效的，示例目标得建在它们之后才拿得到 UVCPP_WEBAPP_ENABLE。
if(UVCPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# 压测靶场。和示例同样放在这里（要拿上面的按目录生效的 add_compile_definitions）。
if(UVCPP_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# install and export
include(CMakePackageConfigHelpers)
set(CONFIG_INSTALL_DIR lib/cmake/uvcpp)

if (TARGET uvcpp)
  install(TARGETS uvcpp
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()
if (TARGET uvcpp_s)
  install(TARGETS uvcpp_s
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()
if (TARGET uvcpp_a)
  install(TARGETS uvcpp_a
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()
if (TARGET uvcpp_a_s)
  install(TARGETS uvcpp_a_s
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()

# Install header files (flat) to avoid accidental nested `include/uvcpp/uvcpp/...`
file(GLOB UVCPP_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/uvcpp/*.h")
if(UVCPP_HEADER_FILES)
  install(FILES ${UVCPP_HEADER_FILES} DESTINATION include/uvcpp)
endif()

# configure_file 生成的使能宏头（见上面那个 configure_file）。它**刻意**不落在
# src/uvcpp/ 下 —— 落那儿就会被上面这个 glob 收进去，跟手写头混作一谈，而且哪天
# 有人往源码树里放一份手写副本就会静默遮蔽生成的那份。
install(FILES "${CMAKE_BINARY_DIR}/include/uvcpp/uvcpp_config.h"
        DESTINATION include/uvcpp)

file(GLOB HANDLE_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/handle/*.h")
if(HANDLE_HEADER_FILES)
  install(FILES ${HANDLE_HEADER_FILES} DESTINATION include/handle)
endif()

file(GLOB REQ_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/req/*.h")
if(REQ_HEADER_FILES)
  install(FILES ${REQ_HEADER_FILES} DESTINATION include/req)
endif()

file(GLOB EXPAND_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/expand/*.h")
if(EXPAND_HEADER_FILES AND UVCPP_BUILD_EXPAND)
  install(FILES ${EXPAND_HEADER_FILES} DESTINATION include/expand)
endif()

file(GLOB NET_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/net/*.h")
if(NET_HEADER_FILES)
  install(FILES ${NET_HEADER_FILES} DESTINATION include/net)
endif()

file(GLOB WEB_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/web/*.h")
if(WEB_HEADER_FILES AND UVCPP_BUILD_WEB)
  install(FILES ${WEB_HEADER_FILES} DESTINATION include/web)
endif()

file(GLOB WEBAPP_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/webapp/*.h")
if(WEBAPP_HEADER_FILES AND UVCPP_BUILD_WEBAPP)
  install(FILES ${WEBAPP_HEADER_FILES} DESTINATION include/webapp)
endif()

# src/wsdl/ 的公开头不依赖 pugixml（`<pugixml.hpp>` 只出现在下面那个私有头里），
# 所以只装我们自己的头 —— 与 http2 那条同一个理由（CMakeLists.txt 里 http2 那段）。
file(GLOB WSDL_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/wsdl/*.h")
# `uvcpp_wsdl_pugixml.h` 是**私有**头（它把 <pugixml.hpp> 拉进来，正是为了让别的头
# 不拉），装出去等于把"使用者不需要 pugixml"这个结论作废。
list(FILTER WSDL_HEADER_FILES EXCLUDE REGEX "uvcpp_wsdl_pugixml\\.h$")
if(WSDL_HEADER_FILES AND UVCPP_ENABLE_WSDL)
  install(FILES ${WSDL_HEADER_FILES} DESTINATION include/wsdl)
endif()

# ---------------------------------------------------------------------------
# nlohmann/json：跟着 uvcpp 一起装
# ---------------------------------------------------------------------------
# 为什么必须动它：`install(EXPORT uvcppTargets)` 要求**导出集里出现的每个
# 非 IMPORTED 目标自己也在这个导出集里**。FetchContent 拉进来的
# nlohmann_json 是真实的 INTERFACE 目标（不是 IMPORTED），而 uvcpp 的
# INTERFACE_LINK_LIBRARIES 里有它（webapp 的公开头要 include 它，所以只能
# PUBLIC）。不处理的话 CMake **generate 阶段**就报错，构建根本不开始：
#
#   install(EXPORT "uvcppTargets" ...) includes target "uvcpp" which requires
#   target "nlohmann_json" that is not in any export set.
#
# 这里的选择是「一并导出 + 一并装头文件」，而不是 `find_dependency`：
# nlohmann/json 是 header-only 的，且 `uvcpp_web_json.h` 把 `nlohmann::json`
# 直接暴露成了 `uvcpp_json` —— 两个翻译单元必须用**同一个版本、同一组配置
# 宏**编译它，否则 ODR 违规（详见 uvcpp_web_json.h 顶部的说明）。让使用者
# 自己 find_package 一个版本，等于把「版本必须一致」这件事交给运气。
# 装进我们自己的 include/ 树之后，使用者的 `#include <nlohmann/json.hpp>`
# 由 uvcpp 的 include 路径解析到，版本天然锁死。
#
# 它自带的 JSON_Install 装的是它自己那套 config 包（我们不需要，也不希望
# 它去写别的 find_package 搜索路径），所以在上面已经关掉了。
if(TARGET nlohmann_json AND UVCPP_BUILD_WEBAPP)
  install(TARGETS nlohmann_json EXPORT uvcppTargets)
  install(DIRECTORY "${nlohmann_json_SOURCE_DIR}/include/nlohmann"
          DESTINATION include)

  # MSVC 调试器的 json 可视化文件。
  #
  # nlohmann 的目标里有 `$<INSTALL_INTERFACE:nlohmann_json.natvis>` —— 这是个
  # **相对**路径，CMake 会把它解析成 `${prefix}/nlohmann_json.natvis`，于是
  # 它会出现在导出的 INTERFACE_SOURCES 里。而 nlohmann 自己那条把它装到
  # prefix 根的 install 规则被我们关掉了（JSON_Install=OFF），所以不补这一步
  # 的话，下游 `find_package(uvcpp)` 之后 **generate 阶段**会直接报
  # "Cannot find source file: .../nlohmann_json.natvis" —— 一个只有真正被
  # 别的工程引用时才会暴露的错。装到 `.` 是跟上游自己的做法一致的。
  if(MSVC AND EXISTS "${nlohmann_json_SOURCE_DIR}/nlohmann_json.natvis")
    install(FILES "${nlohmann_json_SOURCE_DIR}/nlohmann_json.natvis"
            DESTINATION .)
  endif()
endif()

file(GLOB SSL_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/ssl/*.h")
if(SSL_HEADER_FILES AND UVCPP_ENABLE_OPENSSL)
  install(FILES ${SSL_HEADER_FILES} DESTINATION include/ssl)
endif()

# src/http2/ 的公开头是 pimpl 形状的（<nghttp2/nghttp2.h> 只出现在 .cpp 里），
# 使用者拿到的头不依赖 nghttp2 本身，所以这里只装我们自己的头。
file(GLOB HTTP2_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/http2/*.h")
# `uvcpp_h2_nghttp2.h` 是**私有**头（它把 <nghttp2/nghttp2.h> 拉进来，正是为了
# 让别的头不拉），装出去等于把"使用者不需要 nghttp2"这个结论作废。
list(FILTER HTTP2_HEADER_FILES EXCLUDE REGEX "uvcpp_h2_nghttp2\\.h$")
if(HTTP2_HEADER_FILES AND UVCPP_ENABLE_NGHTTP2)
  install(FILES ${HTTP2_HEADER_FILES} DESTINATION include/http2)
endif()

# src/quic/ 同理：公开头是 pimpl 形状的（<ngtcp2/ngtcp2.h> 只出现在 .cpp 里），
# 使用者拿到的头不依赖 ngtcp2 本身。
file(GLOB QUIC_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/quic/*.h")
# `uvcpp_quic_ngtcp2.h` 与 `uvcpp_quic_session.h` 是**私有**头：
#
# - 前者把 <ngtcp2/ngtcp2.h> 拉进来（正是为了让别的头不拉）；
# - 后者是 `quic_session` 的定义，里面直接写着 `ngtcp2_conn*`、`SSL*`、
#   `ngtcp2_path_storage` 这些字段 —— 它包含前者，所以两者要么一起装要么
#   一起不装，而正确答案是**一起不装**：装出去等于把"使用者不需要 ngtcp2"
#   这个结论作废，而且会把一个根本没有稳定 ABI 的类摆进公开面。
#
# **这处排除与 `tests/tools/package_release.py` 的 `PRIVATE_HEADERS` 是一对**：
# 那个脚本自己按文件名清单摘私有头，两处必须一起改 —— 只改这里的话，
# `package_release.py` 打出来的包里照样会带上它。
list(FILTER QUIC_HEADER_FILES EXCLUDE REGEX "uvcpp_quic_(ngtcp2|session)\\.h$")
if(QUIC_HEADER_FILES AND UVCPP_ENABLE_QUIC)
  install(FILES ${QUIC_HEADER_FILES} DESTINATION include/quic)
endif()

# src/http3/ 同理：公开头是 pimpl 形状的（<nghttp3/nghttp3.h> 只出现在 .cpp 里），
# 使用者拿到的头不依赖 nghttp3 本身。
file(GLOB HTTP3_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/http3/*.h")
# `uvcpp_h3_nghttp3.h` 与 `uvcpp_h3_session.h` 是**私有**头：
#
# - 前者把 <nghttp3/nghttp3.h> 拉进来（正是为了让别的头不拉）；
# - 后者是 `h3_session` 的定义，跳板与成员里直接写着 `nghttp3_conn*`、
#   `nghttp3_rcbuf*`、`nghttp3_vec`、`nghttp3_ssize`（它包含前者，所以两者要么
#   一起装要么一起不装，而正确答案是**一起不装**：装出去等于把"使用者不需要
#   nghttp3"这个结论作废，而且会把一个根本没有稳定 ABI 的类摆进公开面）。
#
# **只装 `uvcpp_h3_common.h` 与 `uvcpp_h3_connection.h`**（后者在 P3 落地）——
# 这两个是"使用者真的要用"的那一层：常量、`h3_header`、驱动类。
#
# **这处排除与 `tests/tools/package_release.py` 的 `PRIVATE_HEADERS` 是一对**：
# 那个脚本自己按文件名清单摘私有头，两处必须一起改 —— 只改这里的话，
# `package_release.py` 打出来的包里照样会带上它。
list(FILTER HTTP3_HEADER_FILES EXCLUDE REGEX "uvcpp_h3_(nghttp3|session)\\.h$")
if(HTTP3_HEADER_FILES AND UVCPP_ENABLE_HTTP3)
  install(FILES ${HTTP3_HEADER_FILES} DESTINATION include/http3)
endif()

# src/capi/ —— C ABI 那一层。它是**纯 C** 的头，装给 C / C# P-Invoke / 别的
# 语言用，所以装到 `include/capi/` 而不是并进 `include/uvcpp/`：这一层的
# 稳定性承诺与 C++ 面是**两条线**（见 `uvcpp_c_common.h` 的“版本与兼容承诺”），
# 放在目录上也看得见这条分界。
#
# 与 quic/http3 那两处不同的是：capi 的头**不依赖任何第三方**（这一层是薄包装，
# 连 libuv 的类型都不出现在公开面上 —— 都是不透明指针 + 标量），所以没有"私有头
# 摘出去"以外的顾虑。但**私有头照样有一个**：`uvcpp_c_internal.h` 里是魔数、
# 收口宏和 `uvcpp_c_detail` 的那些件，全都**没有任何稳定性承诺**，装出去等于把
# "这一层内部长什么样"摆进公开面。
#
# **这处排除与 `tests/tools/package_release.py` 的 `PRIVATE_HEADERS` 是一对**：
# 那个脚本自己按文件名清单摘私有头，两处必须一起改 —— 只改这里的话，
# `package_release.py` 打出来的包里照样会带上它。
file(GLOB CAPI_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/capi/*.h")
list(FILTER CAPI_HEADER_FILES EXCLUDE REGEX "uvcpp_c_internal\\.h$")
# 下层模块关着的子头**不装**。这不是洁癖：装了它就等于承诺一份符号根本不存在的
# 接口（`UVCPP_WEBAPP_ENABLE=0` 的库里没有 `uvcpp_c_app_new`），而使用方要到
# **运行时**才发现（`EntryPointNotFoundException`）—— 与 `capi_symbols.lock`
# 要防的是同一件事，只是这里防在"头装不装"那一侧。伞头 `uvcpp_c.h` 自己也按
# `uvcpp_config.h` 里的宏决定拉哪几份，两边说的是同一句话。
#
# `UVCPP_CAPI_DISABLED` 由上面 capi 那一段的**同一张表**填出来（见那里的注释）。
foreach(_uvcpp_capi_base ${UVCPP_CAPI_DISABLED})
  list(FILTER CAPI_HEADER_FILES EXCLUDE REGEX "${_uvcpp_capi_base}\\.h$")
endforeach()
if(CAPI_HEADER_FILES AND UVCPP_ENABLE_CAPI)
  install(FILES ${CAPI_HEADER_FILES} DESTINATION include/capi)
endif()

# If we built libuv from source as part of this project, install its public headers too
if(UVCPP_BUILD_LIBUV_FROM_SOURCE AND DEFINED libuv_SOURCE_DIR)
  message(STATUS "Installing libuv headers from ${libuv_SOURCE_DIR}/include into include/libuv")
  install(DIRECTORY ${libuv_SOURCE_DIR}/include/ DESTINATION include/libuv FILES_MATCHING PATTERN "*.h")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/uvcpp.h")
  install(FILES ${CMAKE_SOURCE_DIR}/src/uvcpp.h DESTINATION include)
endif()

write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/uvcppConfigVersion.cmake"
  VERSION ${UVCPP_VERSION}
  COMPATIBILITY AnyNewerVersion)

configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/uvcppConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/uvcppConfig.cmake"
  INSTALL_DESTINATION ${CONFIG_INSTALL_DIR})

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/uvcppConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/uvcppConfigVersion.cmake"
        DESTINATION ${CONFIG_INSTALL_DIR})

install(EXPORT uvcppTargets FILE uvcppTargets.cmake NAMESPACE uvcpp:: DESTINATION ${CONFIG_INSTALL_DIR})

message(STATUS "Configured uvcpp (shared=${BUILD_SHARED_LIBS})")

# RUN_TESTS target: builds available test executables and runs CTest.
# It will only run tests that were added during configure (so it respects
# UVCPP_BUILD_TESTS / UVCPP_BUILD_FUNCTIONAL and which test targets exist).
if (UVCPP_BUILD_TESTS)
  # Collect test executables that may exist in this build.
  set(_run_tests_deps)
  if (TARGET test_handle_unit)
    list(APPEND _run_tests_deps test_handle_unit)
  endif()
  if (TARGET test_req_unit)
    list(APPEND _run_tests_deps test_req_unit)
  endif()
  if (TARGET test_uvcpp_unit)
    list(APPEND _run_tests_deps test_uvcpp_unit)
  endif()
  # Collect functional test executables created under tests/functional
  file(GLOB _func_test_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/functional/*.cpp)
  set(_run_func_deps)
  foreach(_fsrc ${_func_test_sources})
    get_filename_component(_name ${_fsrc} NAME_WE)
    string(REGEX REPLACE "[^a-zA-Z0-9_]" "_" exe_name "test_${_name}")
    if(TARGET ${exe_name})
      list(APPEND _run_func_deps ${exe_name})
    endif()
  endforeach()
  # Functional tests are registered via add_test; ensure ctest is invoked.
  # Note: use generator-expression $<CONFIG> so multi-config generators (Visual Studio)
  # pass the correct configuration to ctest (-C <config>).
  add_custom_target(run_uvcpp_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
    DEPENDS ${_run_tests_deps} ${_run_func_deps}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Run all configured uvcpp tests (unit + functional)"
  )
endif()

if(WIN32)
  # 需要被拷入 DLL 的测试输出目录。**tests/expand 曾经不在这份清单里**，于是
  # `test_memory_pool` 缺少 uvcpp.dll/llhttp.dll/zlib1.dll，在打印任何东西之前就以
  # STATUS_DLL_NOT_FOUND (0xc0000135) 退出 —— 看起来像崩溃，实际是漏拷。
  #
  # 配置子目录只在多配置生成器（VS）下存在：`$<CONFIG>` 在 Ninja 单配置树上会
  # 展开成 `Release`，于是拷贝全部落进一个**没人读**的 tests/unit/Release/，
  # 表面上"拷成功了"，跑起来照样 0xc0000135。这正是本机 Ninja 树上 copy_test_dlls
  # 一直静默失效的原因（不是缺 pwsh）。
  if(CMAKE_CONFIGURATION_TYPES)
    set(_cfg_subdir "/$<CONFIG>")
  else()
    set(_cfg_subdir "")
  endif()

  set(_test_dll_dirs
    "${CMAKE_BINARY_DIR}/tests/unit${_cfg_subdir}"
    "${CMAKE_BINARY_DIR}/tests/functional${_cfg_subdir}"
  )
  if(TARGET test_memory_pool)
    list(APPEND _test_dll_dirs "${CMAKE_BINARY_DIR}/tests/expand${_cfg_subdir}")
  endif()
  # `tests/capi` 是 1.4.1 加的（`UVCPP_ENABLE_CAPI=ON` 才有目标），加的时候
  # **漏了这一行** —— 与 `tests/expand` 当初漏掉是同一件事、同一个后果：那三支
  # 用例全是**纯 C**（`.c`），链的照样是 `uvcpp.dll` / `uv.dll` / `llhttp.dll`，
  # 少一个就是在进 `main()` 之前就被加载器拦下、一个字节都不打印。
  #
  # 挂 `if(TARGET test_capi_net_func)` 而不是 `if(UVCPP_ENABLE_CAPI)`：这一排
  # 邻居全是 TARGET 判据，而"这个目录里到底有没有 .exe"由**目标存不存在**说了
  # 算 —— 模块开着但某个用例被按模块过滤掉时，前者问的还是对的那件事。
  # (`test_capi_net_func` 是那一支里**无条件**登记的一支，见 tests/capi/CMakeLists.txt。)
  if(TARGET test_capi_net_func)
    list(APPEND _test_dll_dirs "${CMAKE_BINARY_DIR}/tests/capi${_cfg_subdir}")
  endif()
  # 示例的输出目录同样需要 DLL：webapp_demo.exe 直接依赖 uvcpp.dll / uv.dll /
  # llhttp.dll，缺一个就是 0xc0000135 静默退出。
  if(TARGET webapp_demo)
    list(APPEND _test_dll_dirs "${CMAKE_BINARY_DIR}/examples${_cfg_subdir}")
  endif()
  # 压测靶场同理：uvcpp_bench_server.exe 也直接依赖 uvcpp.dll / uv.dll /
  # llhttp.dll，缺一个就是那个 0xc0000135 的静默退出 —— 而静默退出的靶场
  # 在压测脚本眼里和"服务起不来"是同一件事，会被误读成服务端的锅。
  if(TARGET uvcpp_bench_server)
    list(APPEND _test_dll_dirs "${CMAKE_BINARY_DIR}/bench${_cfg_subdir}")
  endif()

  # 收集所有要拷的 DLL 与其依赖目标
  set(_dll_files "")
  set(_dll_deps "")
  if(TARGET uvcpp)
    list(APPEND _dll_files "$<TARGET_FILE:uvcpp>")
    list(APPEND _dll_deps uvcpp)
  endif()
  if(TARGET uv)
    list(APPEND _dll_files "$<TARGET_FILE:uv>")
    list(APPEND _dll_deps uv)
  endif()
  # llhttp 与 zlib 同样是 uvcpp.dll 的运行时依赖：缺失时测试进程会以
  # STATUS_DLL_NOT_FOUND (0xc0000135) 直接退出，且不打印任何测试输出。
  if(TARGET llhttp_shared)
    list(APPEND _dll_files "$<TARGET_FILE:llhttp_shared>")
    list(APPEND _dll_deps llhttp_shared)
  endif()
  if(UVCPP_ENABLE_ZLIB)
    if(TARGET zlib AND TARGET zlib_shared)
      list(APPEND _dll_files "$<TARGET_FILE:zlib_shared>")
      list(APPEND _dll_deps zlib_shared)
    elseif(ZLIB_FOUND AND ZLIB_LIBRARY_RELEASE)
      # 系统 zlib：导入库在 lib/，运行时 DLL 在其同级的 bin/ 下
      get_filename_component(_zlib_lib_dir "${ZLIB_LIBRARY_RELEASE}" DIRECTORY)
      get_filename_component(_zlib_prefix "${_zlib_lib_dir}" DIRECTORY)
      file(GLOB _zlib_dlls "${_zlib_prefix}/bin/zlib*.dll" "${_zlib_lib_dir}/zlib*.dll")
      list(APPEND _dll_files ${_zlib_dlls})
      if(NOT _zlib_dlls)
        message(WARNING
          "UVCPP_ENABLE_ZLIB=ON with system zlib, but no zlib DLL was found near "
          "${ZLIB_LIBRARY_RELEASE}. Windows tests will fail with 0xc0000135 until "
          "zlib1.dll is on PATH or copied next to the test executables.")
      endif()
    endif()
  endif()
  # nghttp2 刻意不进这张表：它被压成静态（见上面 nghttp2_configure 的注释），
  # 链进 uvcpp.dll 后不新增任何运行时依赖，所以这里**不该**出现 nghttp2.dll。
  #
  # OpenSSL 运行时 DLL 位于 OPENSSL_ROOT_DIR/bin（导入库在 lib/），且文件名为版本化
  # （libssl-3-x64.dll / libcrypto-3-x64.dll），故用 glob 定位后复制到测试目录。
  if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    if(OPENSSL_ROOT_DIR AND IS_DIRECTORY "${OPENSSL_ROOT_DIR}/bin")
      file(GLOB _openssl_dlls "${OPENSSL_ROOT_DIR}/bin/libssl-*.dll" "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll")
      list(APPEND _dll_files ${_openssl_dlls})
    endif()
  endif()

  # 各目录先建出来：`cmake -E copy_if_different` 不会创建目标目录，而全新工作区里
  # 这些目录尚未由任何一次构建产生，直接拷会失败。
  #
  # 用单个 add_custom_target 串起有序 COMMAND 列表，而不是 add_custom_command
  # (TARGET ... PRE_BUILD)：后者在非 VS 生成器上被当成 PRE_LINK，挂在一个没有
  # 链接步骤的自定义目标上语义不明。
  set(_copy_cmds "")
  foreach(_dir ${_test_dll_dirs})
    list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}")
  endforeach()
  foreach(_f ${_dll_files})
    foreach(_dir ${_test_dll_dirs})
      list(APPEND _copy_cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "${_dir}/")
    endforeach()
  endforeach()

  # **必须带 ALL**：不带的话 `cmake --build .` 根本不跑这个目标（它是自定义目标，
  # 不在默认的 `all` 里），测试目录里一个 DLL 都没有，ctest 全表以
  # STATUS_DLL_NOT_FOUND (0xc0000135) 在 0.01 秒内退出 —— 看起来像全盘崩溃，
  # 实际只是没拷。本机之所以没暴露，是因为验证时敲的是
  # `cmake --build . --target copy_test_dlls`；换到 CI 上用默认目标就全军覆没。
  # 带 ALL 之后它每次构建都跑，copy_if_different 让它基本不花时间。
  add_custom_target(copy_test_dlls ALL ${_copy_cmds}
    DEPENDS ${_dll_deps}
    COMMENT "Copy uvcpp/libuv DLLs into test folders"
    VERBATIM
  )

  # run_uvcpp_tests 只在 UVCPP_BUILD_TESTS=ON 时建（见上方 if），而 copy_test_dlls
  # 在 WIN32 下恒建。少了这个守卫，`-DUVCPP_BUILD_TESTS=OFF` 在 Windows/MinGW 上
  # 会在 generate 阶段直接失败（add_dependencies 指向不存在的目标），选项等于不可用。
  if(TARGET run_uvcpp_tests)
    add_dependencies(run_uvcpp_tests copy_test_dlls)
  endif()
endif()

# Ensure test executables can find uvcpp/libuv DLLs at runtime (Windows).
# For each test that exists, add PATH entries pointing to the uvcpp and libuv target dirs.
if(WIN32)
  set(_test_list test_handle_unit test_req_unit test_uvcpp_unit)
  foreach(_t ${_test_list})
    if (EXISTS "${CMAKE_BINARY_DIR}/tests/unit/${CMAKE_CFG_INTDIR}/${_t}.exe" OR TARGET ${_t})
      # Build environment string using generator expressions if targets exist.
      set(_env "")
      if(TARGET uvcpp)
        list(APPEND _env "PATH=$<TARGET_FILE_DIR:uvcpp>")
      endif()
      if(TARGET uv)
        list(APPEND _env "PATH=$<TARGET_FILE_DIR:uv>")
      endif()
      if(_env)
        string(JOIN ";" _env_str ${_env})
        # Only set properties for tests that exist in the current CTest registry.
        get_property(_cmake_all_tests GLOBAL PROPERTY TESTS)
        list(FIND _cmake_all_tests ${_t} _test_index)
        if(NOT _test_index EQUAL -1)
          set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${_env_str}")
        endif()
      endif()
    endif()
  endforeach()
endif()