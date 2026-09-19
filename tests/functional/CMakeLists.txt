cmake_minimum_required(VERSION 3.20)

project(uvcpp_functional_tests)

file(GLOB FUNC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

# Exclude web tests when web module is not built
if(NOT UVCPP_BUILD_WEB)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "web_.*\.cpp$")
endif()

# Exclude webapp tests when the application framework is not built.
# 命名约定：框架测试一律 web_app_*.cpp —— 这样 web 关掉时上面的 web_.* 规则
# 已经把它们排除，这里再补一层处理「web 开、webapp 关」的配置。
if(NOT UVCPP_BUILD_WEBAPP)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "web_app_.*\.cpp$")
  # 同时需要 webapp 和 OpenSSL 的用例（`web_ssl_app_*`）在命名上必须命中下面
  # 那条 SSL 过滤器才能被正确摘掉，所以这里补一条 —— 否则「web 开、OpenSSL 开、
  # webapp 关」这个组合下它会去编译一个不存在的模块。
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "web_ssl_app_.*\.cpp$")
  # 需要「webapp + OpenSSL + nghttp2」三样的用例，文件名**必须**写成
  # `web_ssl_app_h2_*` —— 也就是同时含 `web_ssl_` 与 `web_ssl_app_` 两个字面
  # 前缀。它原本叫 `web_ssl_h2_app_func.cpp`，两条正则都匹配不上（`web_app_` 要
  # 连续出现，`web_ssl_app_` 也被中间那个 `h2_` 打断），于是「WEBAPP=关、
  # NGHTTP2=开」下它既不在这条里、也不在下面 nghttp2 那条里，直接落到 `#else`
  # 的 SKIP main 上：**注册、通过、一个断言都没跑**。
  # 判据是"每一条相关的过滤器都能命中它"，不是"名字里带了个 webapp 的词"。
endif()

# Exclude SSL tests when OpenSSL is not enabled.
#
# 不加这一条的话，关掉 OpenSSL 时那个用例**仍然编译、仍然被 ctest 计入、
# 而且仍然 PASS** —— 它的 `#else` 分支就是一个打印 SKIP 的 main()。于是
# 「这个模块一次都没被测过」表现为「全绿」。TLS 的异步缺陷能在仓库里躺那么
# 久，这正是原因之一：`build-webapp`（OpenSSL=OFF）里的"55/55 全绿"包含它。
#
# 按仓库既有约定，关掉某模块就把它的用例**从目标列表里摘掉**，让「没测」
# 表现为**用例不存在**，而不是表现为**通过**。
if(NOT UVCPP_ENABLE_OPENSSL)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "web_ssl_.*\.cpp$")
endif()

# Exclude HTTP/2 tests when nghttp2 is not enabled.
# 命名约定：**文件名里含 `h2_` 的用例就需要 nghttp2**（`h2_session_func.cpp`、
# `web_ssl_h2_server_func.cpp`）。**不能**靠 `web_ssl_*` 那条顺带摘掉 —— h2 会话层
# 本身不需要 TLS（只有接 socket 的那层才要），所以用例不会都带 ssl 前缀，于是
# 「OpenSSL 开、nghttp2 关」这个组合下它会去链一个不存在的模块。
#
# `[^/]*` 这个前缀是**必须的**：`file(GLOB)` 给的是**全路径**，写 `^h2_` 永远匹配
# 不上（这条曾经真的写在仓库里，于是 `test_h2_session_func` 在 nghttp2 关掉的树里
# 照常注册、照常 PASS —— 它的 `#else` 分支就是个打印 SKIP 的 main()，正好是上面
# 那段注释警告的"没测表现为通过"）。
if(NOT UVCPP_ENABLE_NGHTTP2)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*h2_[^/]*\.cpp$")
endif()

foreach(test_src ${FUNC_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  string(REGEX REPLACE "[^a-zA-Z0-9_]" "_" exe_name "test_${test_name}")
  add_executable(${exe_name} ${test_src})
  target_include_directories(${exe_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${exe_name} PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
  if(WIN32)
    target_link_libraries(${exe_name} PRIVATE ws2_32)
  endif()
  # SSL 用例直接用 OpenSSL 的 `SSL_ERROR_*` 常量（光看 uvcpp_ssl 的返回值
  # 分不出「还没完」和「真出错」），所以需要它的头文件与库。
  if(UVCPP_ENABLE_OPENSSL AND test_name MATCHES "^web_ssl_")
    target_link_libraries(${exe_name} PRIVATE ${UVCPP_SSL_LIBS})
    if(WIN32)
      # 与根 CMakeLists 给库的那三条同理：静态 libcrypto 里的 winstore_store.obj
      # 直接调 Cert* 系列（crypt32）。
      #
      # FindOpenSSL 只在 `OPENSSL_USE_STATIC_LIBS` 为真时才把 crypt32 挂到
      # `OpenSSL::*` 的 INTERFACE 上，而那个开关是使用者的配置责任 —— 本机
      # OpenSSL 恰好是静态的，于是「换个没传该开关的新树」就 6 个用例 LNK2019×72，
      # 报错还落在 libcrypto 内部。这里自己挂上，别让一个没人写进文档的 -D
      # 成为「测试编不编得出来」的隐藏前提。
      target_link_libraries(${exe_name} PRIVATE crypt32)
    endif()
  endif()
  add_test(NAME ${exe_name} COMMAND ${exe_name})
endforeach()


