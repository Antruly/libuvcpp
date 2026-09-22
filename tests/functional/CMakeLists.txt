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

# `test_web_app_static_func` 是整棵树里最重的一条（CI 上 ubuntu 16.5 s、Windows
# `web` 21.4 s），而 workflow 给的默认上限是 `--timeout 30`。它的量级由被测面本身
# 决定 —— 每个请求一条连接、串行走完整条响应链 —— 不是哪条用例写慢了，想减负只能
# 改被测代码。余量只剩 9 s 的时候，它在 runner 上就是**时红时绿**的（同一个 SHA
# 换个 runner 就过），这比稳定红更难查：它会训练人「重跑一遍当没事」。
#
# 所以给**这一条**单独一份预算。命令行那个 `--timeout 30` 只是**默认值**，会被这里
# 的属性盖过（两头都实测过：带属性 + `ctest --timeout 1` 照样跑满并通过；撤掉属性、
# 同参数当场 `Timeout`），于是其余每条用例的挂死检出时间一点没松 —— 这也比把整个
# job 的 `--timeout` 抬到 60 诚实，那是把每一条的检出时间一起抬了。
#
# `if(TARGET ...)` 不是装饰：`UVCPP_BUILD_WEBAPP` 默认 OFF，那种配置下这个目标已被
# 上面的过滤器摘掉，直接 `set_tests_properties` 会报「找不到这个测试」。
if(TARGET test_web_app_static_func)
  set_tests_properties(test_web_app_static_func PROPERTIES TIMEOUT 90)
endif()

# `test_web_app_multiloop_func` 起停好几轮 App（每轮 n 条循环 = n 条线程 + 一次
# 优雅停机），量级由被测面本身决定。它**自己**的每条等待都带墙钟兜底并会
# `_Exit(2)` 自报家门（见文件里的 `require_within` / `stop_and_join_within`），
# 所以这里的 TIMEOUT 只是**兜底中的兜底** —— 兜那些不经过那两个帮手的挂死
# （例如 App 析构里）。给 120 是因为它比默认的 30 宽，但比"抬整个 job 的
# `--timeout`"窄：其余每一条用例的挂死检出时间一点没松。
if(TARGET test_web_app_multiloop_func)
  set_tests_properties(test_web_app_multiloop_func PROPERTIES TIMEOUT 120)
endif()


