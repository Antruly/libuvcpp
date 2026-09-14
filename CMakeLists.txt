cmake_minimum_required(VERSION 3.20)
project(uvcpp LANGUAGES C CXX)

option(UVCPP_BUILD_TESTS "Build tests" ON)
option(UVCPP_USE_SYSTEM_LIBUV "Prefer system libuv if found" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

# 扩展模块选项（默认开启）
option(UVCPP_BUILD_EXPAND "Build expand module (memory pool, page heap)" ON)

# =========================================================================
# 网络模块和 Web 模块开关
# =========================================================================
option(UVCPP_BUILD_NET  "Build net module (TCP/UDP client/server)"  ON)
option(UVCPP_BUILD_WEB  "Build web module (HTTP/WebSocket)"         OFF)
# webapp 是建在 web 之上的应用框架层（路由/中间件/请求响应封装/日志）。
# 它依赖 web，所以 web 关掉时它也会被强制关掉。
option(UVCPP_BUILD_WEBAPP "Build web application framework (router/middleware/log)" OFF)

# =========================================================================
# Web 子开关 — 仅在 UVCPP_BUILD_WEB=ON 时有效
# =========================================================================
option(UVCPP_ENABLE_ZLIB    "Enable zlib (gzip, WebSocket Per-Message Deflate)"  OFF)
option(UVCPP_ENABLE_OPENSSL "Enable OpenSSL (HTTPS; WSS not wired yet)"        OFF)

# =========================================================================
# 依赖库版本覆盖
# =========================================================================
set(LLHTTP_VERSION  "release/v9.2.0" CACHE STRING "llhttp git tag to fetch")
set(ZLIB_VERSION    "v1.3.1"         CACHE STRING "zlib git tag to fetch")
set(OPENSSL_VERSION "openssl-3.4.0"  CACHE STRING "OpenSSL git tag to fetch")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC：源码为 UTF-8 无 BOM，显式指定 /utf-8，避免按系统代码页(936/GBK)
# 误读源码，导致中文注释末字节吞掉换行符、把下一行代码并进注释（表现为
# “sock/all/hdrEnd 未声明”等诡异报错）。
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
      FetchContent_MakeAvailable(zlib)
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
if (TARGET uv)
  message(STATUS "Using libuv CMake target 'uv' for linking")
  set(LIBUV_LIB uv)
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

# 根据 UVCPP_BUILD_EXPAND 选项排除 expand 模块
if(NOT UVCPP_BUILD_EXPAND)
  message(STATUS "Excluding expand module from build")
  list(REMOVE_ITEM UVCPP_SOURCES "${CMAKE_SOURCE_DIR}/src/expand/uvcpp_page_allocator.cpp")
  list(REMOVE_ITEM UVCPP_SOURCES "${CMAKE_SOURCE_DIR}/src/expand/uvcpp_memory_pool_span.cpp")
  list(REMOVE_ITEM UVCPP_SOURCES "${CMAKE_SOURCE_DIR}/src/expand/uvcpp_page_heap.cpp")
  list(REMOVE_ITEM UVCPP_HEADERS "${CMAKE_SOURCE_DIR}/src/expand/uvcpp_page_allocator.h")
  list(REMOVE_ITEM UVCPP_HEADERS "${CMAKE_SOURCE_DIR}/src/expand/uvcpp_memory_pool_span.h")
  list(REMOVE_ITEM UVCPP_HEADERS "${CMAKE_SOURCE_DIR}/src/expand/uvcpp_page_heap.h")
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
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_a)
  target_include_directories(uvcpp_a PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_s)
  target_include_directories(uvcpp_s PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_a_s)
  target_include_directories(uvcpp_a_s PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()

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
    target_compile_definitions(${_uvcpp_t} PUBLIC
      # 注意：宏叫 UVCPP_ENABLE_MEMORY_POOL，但开关选项是 UVCPP_BUILD_EXPAND
      # （见上面 add_compile_definitions 那段的条件）。
      UVCPP_ENABLE_MEMORY_POOL=$<BOOL:${UVCPP_BUILD_EXPAND}>
      UVCPP_NET_ENABLE=$<BOOL:${UVCPP_BUILD_NET}>
      UVCPP_WEB_ENABLE=$<BOOL:${UVCPP_BUILD_WEB}>
      UVCPP_WEBAPP_ENABLE=$<BOOL:${UVCPP_BUILD_WEBAPP}>
      UVCPP_ZLIB_ENABLE=$<BOOL:${UVCPP_ENABLE_ZLIB}>
      UVCPP_OPENSSL_ENABLE=$<BOOL:${UVCPP_ENABLE_OPENSSL}>
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
        if(TARGET zlibstatic)
          target_link_libraries(${_tgt} PRIVATE zlibstatic)
        elseif(ZLIB_FOUND)
          target_link_libraries(${_tgt} PRIVATE ${ZLIB_LIBRARIES})
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
  add_custom_target(copy_test_dlls
    COMMENT "Copy uvcpp/libuv DLLs into test folders"
  )

  # 需要被拷入 DLL 的测试输出目录。**tests/expand 曾经不在这份清单里**，于是
  # `test_memory_pool` 缺少 uvcpp.dll/llhttp.dll/zlib1.dll，在打印任何东西之前就以
  # STATUS_DLL_NOT_FOUND (0xc0000135) 退出 —— 看起来像崩溃，实际是漏拷。
  # 当时是靠在 ctest 命令里 `--exclude-regex test_memory_pool` 绕开的，但绕开不是
  # 修复：那个 exe 单独跑仍然起不来。这里把目录抽成一个清单，拷贝只写一份。
  set(_test_dll_dirs
    "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>"
    "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>"
  )
  if(TARGET test_memory_pool)
    list(APPEND _test_dll_dirs "${CMAKE_BINARY_DIR}/tests/expand/$<CONFIG>")
  endif()

  # 各目录先建出来：`cmake -E copy_if_different` 不会创建目标目录，而全新工作区里
  # 这些目录尚未由任何一次构建产生，直接拷会以 MSB3073 失败。
  foreach(_dir ${_test_dll_dirs})
    add_custom_command(TARGET copy_test_dlls PRE_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
    )
  endforeach()

  # 把每个文件复制到**全部**测试目录。函数体里能读到父作用域的 _test_dll_dirs。
  function(_copy_test_dlls_to_all_dirs)
    foreach(_f ${ARGN})
      foreach(_dir ${_test_dll_dirs})
        add_custom_command(TARGET copy_test_dlls PRE_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "${_dir}/"
        )
      endforeach()
    endforeach()
  endfunction()

  if(TARGET uvcpp)
    _copy_test_dlls_to_all_dirs($<TARGET_FILE:uvcpp>)
    add_dependencies(copy_test_dlls uvcpp)
  endif()
  if(TARGET uv)
    _copy_test_dlls_to_all_dirs($<TARGET_FILE:uv>)
    add_dependencies(copy_test_dlls uv)
  endif()
  # llhttp 与 zlib 同样是 uvcpp.dll 的运行时依赖：缺失时测试进程会以
  # STATUS_DLL_NOT_FOUND (0xc0000135) 直接退出，且不打印任何测试输出。
  if(TARGET llhttp_shared)
    _copy_test_dlls_to_all_dirs($<TARGET_FILE:llhttp_shared>)
    add_dependencies(copy_test_dlls llhttp_shared)
  endif()
  if(UVCPP_ENABLE_ZLIB)
    if(TARGET zlib AND TARGET zlib_shared)
      _copy_test_dlls_to_all_dirs($<TARGET_FILE:zlib_shared>)
      add_dependencies(copy_test_dlls zlib_shared)
    elseif(ZLIB_FOUND AND ZLIB_LIBRARY_RELEASE)
      # 系统 zlib：导入库在 lib/，运行时 DLL 在其同级的 bin/ 下
      get_filename_component(_zlib_lib_dir "${ZLIB_LIBRARY_RELEASE}" DIRECTORY)
      get_filename_component(_zlib_prefix "${_zlib_lib_dir}" DIRECTORY)
      file(GLOB _zlib_dlls "${_zlib_prefix}/bin/zlib*.dll" "${_zlib_lib_dir}/zlib*.dll")
      _copy_test_dlls_to_all_dirs(${_zlib_dlls})
      if(NOT _zlib_dlls)
        message(WARNING
          "UVCPP_ENABLE_ZLIB=ON with system zlib, but no zlib DLL was found near "
          "${ZLIB_LIBRARY_RELEASE}. Windows tests will fail with 0xc0000135 until "
          "zlib1.dll is on PATH or copied next to the test executables.")
      endif()
    endif()
  endif()
  # OpenSSL 运行时 DLL 位于 OPENSSL_ROOT_DIR/bin（导入库在 lib/），且文件名为版本化
  # （libssl-3-x64.dll / libcrypto-3-x64.dll），故用 glob 定位后复制到测试目录。
  if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    if(OPENSSL_ROOT_DIR AND IS_DIRECTORY "${OPENSSL_ROOT_DIR}/bin")
      file(GLOB _openssl_dlls "${OPENSSL_ROOT_DIR}/bin/libssl-*.dll" "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll")
      _copy_test_dlls_to_all_dirs(${_openssl_dlls})
    endif()
  endif()

  add_dependencies(run_uvcpp_tests copy_test_dlls)
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