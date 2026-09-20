cmake_minimum_required(VERSION 3.20)
project(uvcpp LANGUAGES C CXX)

option(UVCPP_BUILD_TESTS "Build tests" ON)
option(UVCPP_USE_SYSTEM_LIBUV "Prefer system libuv if found" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# 扩展模块选项（默认关闭）
# 内存池在 MinGW-w64 下会让 test_web_app_* 崩溃（根因未定位，见 RELEASE.md 的
# 「已知问题」），所以默认改成就地关闭、要用得显式 -DUVCPP_BUILD_EXPAND=ON。
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

# 可运行示例（examples/）。默认关：它是给使用者看的，不进库本身。
option(UVCPP_BUILD_EXAMPLES "Build the examples in examples/" OFF)

# =========================================================================
# Web 子开关 — 仅在 UVCPP_BUILD_WEB=ON 时有效
# =========================================================================
option(UVCPP_ENABLE_ZLIB    "Enable zlib (gzip, WebSocket Per-Message Deflate)"  OFF)
option(UVCPP_ENABLE_OPENSSL "Enable OpenSSL (HTTPS; WSS not wired yet)"        OFF)
option(UVCPP_ENABLE_NGHTTP2 "Enable HTTP/2 via nghttp2 (needs UVCPP_ENABLE_OPENSSL; TLS+ALPN only, no h2c)" OFF)

# =========================================================================
# 依赖库版本覆盖
# =========================================================================
set(LLHTTP_VERSION  "release/v9.2.0" CACHE STRING "llhttp git tag to fetch")
set(ZLIB_VERSION    "v1.3.1"         CACHE STRING "zlib git tag to fetch")
set(OPENSSL_VERSION "openssl-3.4.0"  CACHE STRING "OpenSSL git tag to fetch")
set(NGHTTP2_VERSION "v1.70.0"        CACHE STRING "nghttp2 git tag to fetch")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC：显式指定 /utf-8，避免按系统代码页(936/GBK)误读源码，导致中文注释末字节
# 吞掉换行符、把下一行代码并进注释（表现为“sock/all/hdrEnd 未声明”等诡异报错）。
#
# 实测 src/ 下 99 个公开头里 75 个带 BOM、24 个不带 —— 不是"全都无 BOM"；那不带的
# 24 个里真正会坏的只有 22 个（含非 ASCII），另 2 个是纯 ASCII。
# 带 BOM 的那些本来就不靠这个开关。但这个开关是**目录作用域**的，
# 既不进导出集，也到不了预编译包的消费者，所以 22 个无 BOM 的头对 MSVC 使用者
# 依然是坏的：只能由打包脚本补 BOM 兜住（tests/tools/package_release.py 的
# copy_header）。根因（源码树里那 22 个自己没 BOM）另记。
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

  # -------------------------------------------------------------------
  # OpenSSL / SSL module
  # -------------------------------------------------------------------
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
_uvcpp_literal01(UVCPP_ZLIB_ENABLE      UVCPP_ENABLE_ZLIB)
_uvcpp_literal01(UVCPP_OPENSSL_ENABLE   UVCPP_ENABLE_OPENSSL)
_uvcpp_literal01(UVCPP_NGHTTP2_ENABLE   UVCPP_ENABLE_NGHTTP2)
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
      UVCPP_ZLIB_ENABLE=${UVCPP_ZLIB_ENABLE}
      UVCPP_OPENSSL_ENABLE=${UVCPP_OPENSSL_ENABLE}
      UVCPP_NGHTTP2_ENABLE=${UVCPP_NGHTTP2_ENABLE}
      UVCPP_TRY_WRITE_ENABLE=${UVCPP_TRY_WRITE_ENABLE}
      UVCPP_TRY_WRITE_MIN_BYTES=${UVCPP_TRY_WRITE_MIN_BYTES}
    )
  endif()
endforeach()

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
  # 示例的输出目录同样需要 DLL：webapp_demo.exe 直接依赖 uvcpp.dll / uv.dll /
  # llhttp.dll，缺一个就是 0xc0000135 静默退出。
  if(TARGET webapp_demo)
    list(APPEND _test_dll_dirs "${CMAKE_BINARY_DIR}/examples${_cfg_subdir}")
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