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

# Exclude WSDL/SOAP tests when the wsdl module is not enabled.
# 命名约定：**文件名里含 `wsdl` 的用例就需要 wsdl 模块**。`[^/]*` 前缀同样是必须的
# （file(GLOB) 给的是全路径，写 `^wsdl_` 永远匹配不上 —— 上面 h2 那条踩过）。
#
# 两条用例都要摘：`wsdl_document_func.cpp`（纯文档模型）与
# `web_app_wsdl_func.cpp`（经 app 发布）。后者同时命中上面那条 `web_app_` 规则，
# 所以「webapp 关、wsdl 开」下它已经走了；这里补的是「webapp 开、wsdl 关」。
if(NOT UVCPP_ENABLE_WSDL)
  # `soap` 那一半（SOAP 信封、按 operation 派发）与 `wsdl` 是同一个模块
  # （`UVCPP_ENABLE_WSDL`），只是文件名按件名走，所以两条都要摘。
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*(wsdl|soap)[^/]*\.cpp$")
endif()

# Exclude QUIC tests when the quic module is not enabled.
# 命名约定：**文件名里含 `quic` 的用例就需要 quic 模块**（现在只有一条
# `quic_api_func.cpp`）。`[^/]*` 前缀同样是必须的 —— `file(GLOB)` 给的是全路径，
# 写 `^quic_` 永远匹配不上（h2 那条踩过，注释在上面）。
#
# 这一条另外还挡住一个**不显眼**的失败模式：`UVCPP_ENABLE_QUIC=ON` 但那份
# OpenSSL 不带 QUIC API 时，配置期会把它**降级成 OFF**（普通变量 `set()`，不是
# cache 写）。那时 `UVCPP_QUIC_ENABLE` 是 0，公开头整个落在 `#if` 外面 ——
# 用例照常注册、照常 PASS 的话，就会是"这个模块一次都没被测过"表现为全绿。
if(NOT UVCPP_ENABLE_QUIC)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*quic[^/]*\.cpp$")
endif()

# Exclude HTTP/3 tests when the http3 module is not enabled.
# 命名约定：**文件名里含 `http3` 的用例就需要 http3 模块**（现在只有两条
# `http3_request_func.cpp` / `http3_web_func.cpp`）。`[^/]*` 前缀同样是必须的
# —— `file(GLOB)` 给的是全路径，写 `^http3_` 永远匹配不上（h2 那条踩过）。
#
# **这一条不能靠上面 quic 那条顺带摘掉**：`http3` 这个词里没有 `quic` 这四个
# 字母连在一起，两条正则互不覆盖。`UVCPP_ENABLE_HTTP3=ON` 而 QUIC 被守卫链
# 降级成 OFF 时，上面那条会摘掉 `quic_*`、这条摘掉 `http3_*`，两条各管各的。
if(NOT UVCPP_ENABLE_HTTP3)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*http3[^/]*\.cpp$")
endif()

# Exclude yyjson tests when the yyjson module is not enabled.
# 命名约定：**文件名里含 `yyjson` 的用例就需要 yyjson 模块**（现在两条：
# `yyjson_doc_func.cpp` 测解析侧、`yyjson_build_func.cpp` 测构造侧 —— 两侧各一条，
# 与库里的两组类型对应）。`[^/]*` 前缀同样是必须的 —— `file(GLOB)` 给的是全路径，
# 写 `^yyjson_` 永远匹配不上（h2 那条踩过，注释在上面）。
#
# 两条都是 GLOB 进来的：**新增 `yyjson_*` 用例不需要改这里**，过滤器自动覆盖。
# （反过来说：改了这个 glob 之后要重新 configure 才看得见新文件。）
#
# 这一条不能省：yyjson 模块默认 **OFF**，而 `src/yyjson/uvcpp_json_doc.h` 的整段
# 声明都在 `#if UVCPP_YYJSON_ENABLE` 里面 —— 不摘掉的话，默认配置下这两条用例
# 会因为"`uvcpp_json_doc` 没有声明"而**编译失败**（不是跳过，是整个 job 红）。
# 关掉模块时"没测"必须表现为**用例不存在**：所以 `YYJSON=OFF` 的树上用例数不变，
# 而不是多出两条恒真的绿。
#
# 另外注意：这两条用例**都不需要** webapp（本模块在核心层），所以它**不**带
# `web_app_` 前缀 —— 带了的话，`WEBAPP=OFF, YYJSON=ON` 这个组合下它会被上面
# 那条 `web_app_.*` 顺手摘掉，表现为"模块开着却一次都没测"。两条里各自与
# nlohmann 对照的那几组落在 `#if UVCPP_WEBAPP_ENABLE` 里并**打印一行 SKIP**，
# 其余分组在 `WEBAPP=OFF` 的树上也真跑。
if(NOT UVCPP_ENABLE_YYJSON)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*yyjson[^/]*\.cpp$")
endif()

# 解压（br/gzip）用例需要 zlib 与 brotli **两者**：缺任一就摘掉 —— 与上面
# "没测表现为用例不存在"同一条规矩（用例自己的 #else 分支只是打印 SKIP）。
if(NOT UVCPP_ENABLE_ZLIB OR NOT UVCPP_ENABLE_BROTLI)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*http_client_br[^/]*\.cpp$")
endif()

# Exclude database tests when the db module is not enabled.
# 命名约定：**文件名里含 `db` 的用例就需要 db 模块**（`db_sqlite_func.cpp` 等）。
# `[^/]*` 前缀是必须的 —— `file(GLOB)` 给的是全路径，写 `^db_` 永远匹配不上
# （h2 那条踩过，注释在上面）。db 模块默认 **OFF**，所以这条过滤器默认是
# **生效**的：不摘的话，那几条用例会在 `UVCPP_DB_ENABLE=0`（公开头整个落在
# `#if` 外面）的树上编不过 —— 而不是"跳过"，是编译失败，整个 job 红。
if(NOT UVCPP_ENABLE_DB)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*db[^/]*\.cpp$")
endif()

# 模块开着、但某一个后端没编进来时，那一条用例也要摘掉。
#
# 上面那条只摘"整个模块关掉"的情形。`-DUVCPP_ENABLE_DB=ON` 而没有
# libmysqlclient 时，MySQL 后端会在配置期被降级成 OFF（普通变量 `set()`，
# 不是 cache 写）—— 那时 `db_mysql_func.cpp` 里的 `open()` 会拿到
# `NO_DRIVER`，用例**红**。而真相是"这个后端不在这份包里"，它该表现为
# **用例不存在**，不是"用例失败"（`tests/functional/CMakeLists.txt` 开头那段
# 注释里的老坑：没测过 ≠ 测过）。
if(NOT UVCPP_ENABLE_DB_SQLITE)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*db_sqlite[^/]*\.cpp$")
endif()
if(NOT UVCPP_ENABLE_DB_MYSQL)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*db_mysql[^/]*\.cpp$")
endif()
if(NOT UVCPP_ENABLE_DB_PGSQL)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*db_pgsql[^/]*\.cpp$")
endif()
if(NOT UVCPP_ENABLE_DB_ODBC)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*db_odbc[^/]*\\.cpp$")
endif()
# MongoDB 是**两条**用例、两种摘法：
#
#   * `db_mongodb_func.cpp` 要一个真的 mongod，后端没编进来时它一次都跑不了
#     ⇒ 摘掉（"没编进来"该表现为用例不存在，不是失败）。
#   * `db_mongodb_nodrv_func.cpp` **必须留下**：它验的正是"后端/客户端库不在时
#     调用方看到什么"，桩构建里同样有意义（那时它断言的是
#     `backend_available()==false` + `NO_DRIVER`）。**唯一例外**是"后端编进来
#     但直接链接"那种形态，见下面第二段。
if(NOT UVCPP_ENABLE_DB_MONGODB)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*db_mongodb_func\\.cpp$")
endif()

# 第三种形态：后端编进来了、但走的是**直接链接**（`UVCPP_DB_RUNTIME_LOAD=OFF`）。
# 这时 `db_mongodb_nodrv_func` 的前提（"两份客户端库打不开"）**表达不出来** ——
# 库就在链接行上，`UVCPP_DB_MONGOC_LIBRARY` 那套覆盖变量根本不会被读，用例会拿
# 真实驱动去连一个不存在的服务端、看到 `OPEN_FAILED` 而不是 `NO_DRIVER`。
# 所以这种构建里**不注册**它：不是失败，是这一格没有它要测的那个形态。
# 桩构建（`UVCPP_ENABLE_DB_MONGODB=OFF`）**不在此列** —— 那里 `backend_available()`
# 是 false，用例断言的是另一句话，同样有意义。
if(UVCPP_ENABLE_DB_MONGODB AND NOT UVCPP_DB_RUNTIME_LOAD)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*db_mongodb_nodrv_func\\.cpp$")
endif()

# Redis 用例同理：`UVCPP_ENABLE_REDIS=0` 时公开头整段落在 `#if` 外面，不摘
# 就是编译失败而不是跳过。
if(NOT UVCPP_ENABLE_REDIS)
  list(FILTER FUNC_SOURCES EXCLUDE REGEX "/[^/]*redis[^/]*\.cpp$")
endif()

foreach(test_src ${FUNC_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  string(REGEX REPLACE "[^a-zA-Z0-9_]" "_" exe_name "test_${test_name}")
  add_executable(${exe_name} ${test_src})
  target_include_directories(${exe_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${exe_name} PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
  if(WIN32)
    # iphlpapi/psapi/userenv 是**静态 libuv** 的 util.c 要的（GetAdaptersAddresses /
    # GetProcessMemoryInfo / GetUserProfileDirectoryW）：LIBUV_LIB 是裸路径时（系统
    # libuv，uv_a.lib）它不带使用要求，只挂 ws2_32 的用例就会 LNK2019。
    target_link_libraries(${exe_name} PRIVATE ws2_32 iphlpapi psapi userenv)
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
  # ⚠️ 想在这棵树下写**白盒**功能用例（include `src/quic/*.h` 那种私有头）的话，
  # 光有 `-I ${CMAKE_SOURCE_DIR}/src` 还不够：私有头里 include 了 ngtcp2，而
  # **ngtcp2 的 include 目录不会顺着链接传过来**（根 CMakeLists 把
  # `ngtcp2_static` 挂在 `$<BUILD_INTERFACE:>` + PRIVATE 下，静态/共享库上都会以
  # `$<LINK_ONLY:...>` 进 `INTERFACE_LINK_LIBRARIES`，而它**不带** include 目录，
  # 见 `doc/quic-guide.md` §7.2）。要挂就照 `web_ssl_*` 自己挂 OpenSSL 那样写：
  #
  #     target_include_directories(${exe_name} PRIVATE
  #       $<TARGET_PROPERTY:ngtcp2_static,INTERFACE_INCLUDE_DIRECTORIES>)
  #
  # 不挂的话 MinGW 的 g++ 会**静默**捞到 `C:/msys64/mingw64/include/ngtcp2/` 那份
  # **别人装的、版本不同**的头，报出来的是"`ngtcp2_stateless_reset_token` 没有
  # 声明" —— 看着像本库的私有头写错了，其实是被换了一份头。
  #
  # 现在没有用例走这条路（`quic_gso_func.cpp` 用的是公开 API），所以这里只是一条
  # 给下一个人的提示，不挂任何目标。另有一层理由不要碰那条路：**库里那些私有
  # helper 没有导出**（`libuvcpp.dll` 的导出表里没有 `quic_detail::*`），白盒用例
  # 即使编得过也**链接不到** —— 症状是 `undefined reference`。

  add_test(NAME ${exe_name} COMMAND ${exe_name})
endforeach()

# 两条"手写 POSIX socket 对端"的用例（外部 TLS 对端 / 阻塞超时）在 Windows
# 上返回 3：ctest 需要这一格才把它们报成 `***Skipped` 而不是 `Failed`。
foreach(_posix_peer_test
        test_web_ssl_client_external_func
        test_web_ssl_client_timeout_func)
  if(TARGET ${_posix_peer_test})
    set_tests_properties(${_posix_peer_test} PROPERTIES SKIP_RETURN_CODE 3)
  endif()
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



# ---------------------------------------------------------------------------
# 数据库模块的用例
# ---------------------------------------------------------------------------

# 静态库构建下，db 用例必须**自己**把客户端库链上。
#
# 根 CMakeLists 给库挂的是 `PRIVATE $<BUILD_INTERFACE:${_db_lib}>`：共享库上
# libsqlite3 / libmysqlclient / libpq 会以 `DT_NEEDED` 跟着走，用例什么都不用
# 做；但静态库上那一条**不会**传给消费者（`$<BUILD_INTERFACE:>` 就是这么设的，
# 与 zlib / nghttp2 那几处同一个形状，见 `doc/db-guide.md` 里记的那个已知缺口）。
# 不补这一句，`-DUVCPP_BUILD_SHARED=OFF -DUVCPP_ENABLE_DB=ON` 那条配置下这里的
# 每条 db 用例都是 `undefined reference to sqlite3_open_v2`。
if(UVCPP_ENABLE_DB AND UVCPP_DB_LINK_LIBS)
  foreach(_db_src ${FUNC_SOURCES})
    get_filename_component(_db_name ${_db_src} NAME_WE)
    if(_db_name MATCHES "db")
      target_link_libraries(test_${_db_name} PRIVATE ${UVCPP_DB_LINK_LIBS})
    endif()
  endforeach()
endif()

# MySQL / PostgreSQL / ODBC 那三条要一个**真的服务端**（ODBC 要的是一份它的
# `UVCPP_DB_TEST_ODBC_URL` 指得到的、装得出驱动的数据源），所以它们自己按环境变量
# 决定跑还是判"未判定"（退出码 3）。ctest 得知道 3 是什么意思，否则"这台机器上
# 没有 MySQL"会显示成 `Failed` —— 而把它改成 `return 0` 又会显示成 `Passed`，
# 那是本仓最忌讳的那种绿：**没测过**长得和**测过了**一模一样。
#
# `SKIP_RETURN_CODE` 是 ctest 自带的那一格（3.0 起），报出来的是
# `***Skipped`，与 `Passed` 在日志里是两个词。
#
# `test_db_odbc_func` 是 1.7.2 补进这一串的：原来这份构建里**只有它漏了**这一格，
# 而它的漏法和另两条不同 —— 另两条在 CI 里从来都是"URL 已经给了"（服务容器起来
# 了），所以没设 `SKIP_RETURN_CODE` 时是**当场红**、改对了才有跳过语义；ODBC 这条
# 到今天为止**没有任何一格 CI 给过 `UVCPP_DB_TEST_ODBC_URL`**，于是谁在装了
# `unixodbc-dev` 的机器上跑一次 `ctest`（这正好是"ODBC 后端在、只是没数据源"这个
# 最常见的开发机形态），看到的都是 `Failed` —— 而真相是"未判定"。它不属于下面
# 那条"永远真跑"的例外：没有数据源时它连一条 SQL 都发不出去。
#
# SQLite 那条**不设**这个属性：它不需要服务端，永远真跑。给它也设上的话，
# 将来某条路径误返回 3 就会从"红"退化成"跳过"。
#
# CI 上还会额外设 `UVCPP_DB_TEST_REQUIRE=1`：那时缺 URL 是**失败**（退出码 1），
# 不是跳过 —— 服务容器都起了还跳过，说明配置写错了，必须红。（这一条对 ODBC 同样
# 成立：哪天有哪一格真起了个数据源，就该把那格的 `UVCPP_DB_TEST_REQUIRE=1` 也配上。）
foreach(_db_remote test_db_mysql_func test_db_pgsql_func
        test_db_pgsql_async_func test_db_odbc_func test_db_mongodb_func)
  if(TARGET ${_db_remote})
    set_tests_properties(${_db_remote} PROPERTIES SKIP_RETURN_CODE 3)
  endif()
endforeach()

# Redis 的真实服务端用例同一档：本机没起 `redis-server` 时返回 3（未判定），
# CI 的腿上起了服务器并设 `UVCPP_REDIS_TEST_REQUIRE=1` —— 那时缺服务器是失败。
# 假服务端用例（`redis_client_func` / `redis_resp_func`）不设这一格：它们不依赖
# 任何外部服务，误返回 3 只会把真红伪装成跳过。
if(TARGET test_redis_real_func)
  set_tests_properties(test_redis_real_func PROPERTIES SKIP_RETURN_CODE 3)
endif()

# 两条 IPv6 回环用例：绑 `::1` 失败时返回 3（= 这台机器没开 IPv6 栈）。
#
# **跳过与通过必须能分开**：没有 IPv6 时用例一个断言都跑不到，若 `return 0`
# 就会和"真跑过并且对了"长得一模一样 —— 那正是本仓最忌讳的那种绿。用
# `SKIP_RETURN_CODE` 把它报成 `***Skipped`。判据在用例自己手里（`bindIpv6`
# 的返回值），不是"看 runner 上装没装什么"。
foreach(_v6 test_tcp_client_ipv6_func test_udp_client_ipv6_func)
  if(TARGET ${_v6})
    set_tests_properties(${_v6} PROPERTIES SKIP_RETURN_CODE 3)
  endif()
endforeach()

# `test_db_odbc_nodrv_func` 的前提是"这台机器上**没有** ODBC 驱动管理器"，而它
# **不去卸载任何东西**：把 `UVCPP_DB_ODBC_LIBRARY` 指到一个不存在的路径就行
# （那个旋钮写在文档里的用途就是这个，见 `src/db/uvcpp_db_runtime_load.h`）。
# 于是"没装 unixODBC"这件事在这里是**确定性的**，与 runner 上装没装无关 ——
# 这也是为什么它不设 `SKIP_RETURN_CODE`：没有"机器没配好所以跳过"这一档，
# 前提完全在我们手里，不成立就该红（用例自己会检查那个路径真的不存在）。
#
# 它是**独立目标**而不是并进 `db_odbc_func.cpp`：ODBC 的符号加载走 magic static，
# 进程内一旦成功加载过管理器，之后再设环境变量也不生效（"句柄永不释放"的后果）。
# 所以要测"加载失败"，必须在一个还没碰过 ODBC 的进程里设好这个变量 —— ctest 的
# `ENVIRONMENT` 属性恰好是唯一能保证这件事的地方。
if(TARGET test_db_odbc_nodrv_func)
  if(WIN32)
    # Z: 在 CI 的 Windows 镜像上不存在（`win-client-headers.sh` 那些步骤都不用盘符）。
    set(_uvcpp_odbc_nodrv_path "Z:/nonexistent/odbc32.dll")
  else()
    set(_uvcpp_odbc_nodrv_path "/nonexistent/libodbc.so.2")
  endif()
  set_tests_properties(test_db_odbc_nodrv_func PROPERTIES
    ENVIRONMENT "UVCPP_DB_ODBC_LIBRARY=${_uvcpp_odbc_nodrv_path}")
endif()

# `test_db_mongodb_nodrv_func` 同一条形状：两个客户端库变量各指一个不存在的
# 路径。与 ODBC 那条一样是**独立目标**（加载走 magic static，进程内第一次
# 接触之后环境变量就不再生效），也一样**不设** `SKIP_RETURN_CODE`。
#
# 桩构建（没有 mongoc.h）里这两个变量根本不会被读 —— 用例自己知道
# `backend_available()==false`，于是断言的是另一句话。所以这一段对两种构建
# 都成立，不需要按 `UVCPP_ENABLE_DB_MONGODB` 分支。
if(TARGET test_db_mongodb_nodrv_func)
  if(WIN32)
    set(_uvcpp_mongoc_nodrv_path "Z:/nonexistent/mongoc-1.0.dll")
    set(_uvcpp_bson_nodrv_path "Z:/nonexistent/bson-1.0.dll")
  else()
    set(_uvcpp_mongoc_nodrv_path "/nonexistent/libmongoc-1.0.so.0")
    set(_uvcpp_bson_nodrv_path "/nonexistent/libbson-1.0.so.0")
  endif()
  set_tests_properties(test_db_mongodb_nodrv_func PROPERTIES
    ENVIRONMENT "UVCPP_DB_MONGOC_LIBRARY=${_uvcpp_mongoc_nodrv_path};UVCPP_DB_BSON_LIBRARY=${_uvcpp_bson_nodrv_path}")
endif()
