# C ABI 的用例（`src/capi/`）。
#
# 只在 `UVCPP_ENABLE_CAPI=ON` 时被 add_subdirectory（判据在顶层 CMakeLists.txt）：
# 关掉时这些用例**不存在**，而不是"存在但跳过"。这一条是本仓既有约定
# （tests/functional/CMakeLists.txt 里那几条过滤器讲的同一件事）——「没测」必须
# 表现为失败或目标不存在，绝不能表现为通过。
#
# ★ 这些用例**全部是纯 C 编的**（`.c` 文件），这是本目录唯一一条承重的纪律。
#   这一层的承诺是"C 能看见、C 能调"，那么判据就必须由 **C 编译器**来量：
#   一个用 C++ 编出来的用例会把"头里混进了 C++ 的东西"整件事藏起来 ——
#   那份头在 C++ 里编得过，问题只会烧到真正拿它做 P/Invoke 的人身上，而且表现
#   为"某个类型不认识"，不是一个 ABI 错误。
#
#   所以本目录**不许 include 任何 C++ 头**（`uvcpp_version.h` 就不行，它有
#   namespace 与 inline 函数）。头要的宏由 CMake 以编译定义传进来，见下面
#   `UVCPP_TEST_VERSION`。

# 版本串的交叉核对：库里的 `uvcpp_c_version_string()` 说的必须与
# `src/uvcpp/uvcpp_version.h` 里的版本号一致。那个头是 C++ 的（namespace +
# inline 函数），C 用例 include 不了，所以这里由 CMake 把它转成一条 -D 传进来
# —— CMake 那份 `UVCPP_VERSION` 本身就是 configure 期从同一个头里正则抓出来的，
# 于是链条是"头 → CMake → 用例 → 库返回的串"，四处只有一份真相。
#
# 注意 `UVCPP_VERSION` 不带 `-dev` 后缀（那个后缀是头里按 IS_RELEASE 拼的），
# 所以用例比的是**前缀**，不是全等。
set(UVCPP_CAPI_TEST_VERSION_DEF "UVCPP_TEST_VERSION=\"${UVCPP_VERSION}\"")

# 用例清单。显式列出而不是 `file(GLOB)`：这一层要的是"哪几个文件在跑"看得见，
# 新加一个用例必须顺手改这一行（glob 会让"没被编进去"和"编进去了"长得一样，
# 而那正是本仓踩过好几次的坑）。
set(UVCPP_CAPI_TESTS
  capi_common_func.c   # 地基：ABI 自洽、错误码、句柄生死、事件表 size 规则
  capi_net_func.c      # net：真起两端、真收字节的回显
)

# 用例**跟着它量的那个模块的开关走**。
#
# 为什么必须写这一条，而不是"反正 CAPI 开着就一起编"：C 面的守卫链只保证
# NET + WEB 开着（见顶层 CMakeLists.txt 那段），webapp / h2 / quic / h3 都是
# 它的**可选下层**。所以 `-DUVCPP_ENABLE_CAPI=ON` 而 `WEBAPP` 取默认 OFF 是一个
# **合法组合**（CI 的 `full` 格就是它），那个组合下 `uvcpp_c_app_new` 这个符号
# 根本不存在 —— 把用例注册进去就是链不上（Linux/macOS）或者链接期报未解析外部
# （MSVC），红的是一个"这条腿本来就没有的能力"，而不是代码坏了。
#
# 反过来说，**不许**把这段写成"编不过就跳过"：模块关着时这个用例不**存在**
# （`--target` 找不到、`ctest -R` 报没有匹配），与上面讲的"没测不许长得像测过"
# 是同一条。想让这条用例在某个配置下跑起来，办法是把那个模块打开，不是让它变绿。
if(UVCPP_BUILD_WEBAPP)
  list(APPEND UVCPP_CAPI_TESTS
    capi_webapp_func.c   # webapp + web：C 写的 app 与 C 写的客户端互相打
  )
endif()

# h2 那一份跟着 `NGHTTP2` 走，**不是**跟着 `WEB` 走：`src/capi/uvcpp_c_http2.*`
# 本来就在顶层那张 `UVCPP_CAPI_DISABLED` 表里由 `UVCPP_ENABLE_NGHTTP2` 决定
# 编不编（CMakeLists.txt 里那一段）。这条用例量的是那一份，所以开关必须是同一个
# —— 两个都写 `NGHTTP2` 的话，`CAPI=ON, NGHTTP2=ON` 与 `CAPI=ON, NGHTTP2=OFF`
# 两种配置下"库里有几份文件、ctest 里有几个用例"是同步的。
if(UVCPP_ENABLE_NGHTTP2)
  list(APPEND UVCPP_CAPI_TESTS
    capi_h2_func.c       # http2：C 写的服务端与 C 写的客户端在一条连接上跑三条流
  )
endif()

# quic + h3 那一份只写 `HTTP3` 一个开关，因为顶层守卫链里 `HTTP3 ⇒ QUIC` 是
# **强制过的**（CMakeLists.txt 里那两条 `set(... OFF)`：没有 QUIC、没有 web，
# http3 都会被关掉）。这条用例量的两份源文件正好被这一个开关一起盖住
# —— `uvcpp_c_quic.*` 由 `UVCPP_ENABLE_QUIC` 决定、`uvcpp_c_http3.*` 由
# `UVCPP_ENABLE_HTTP3` 决定，而后者蕴含前者。写成 `QUIC AND HTTP3` 也对，但那是
# 把守卫链已经保证过的传递关系又抄一遍，抄漂的那天两个开关就会打架。
if(UVCPP_ENABLE_HTTP3)
  list(APPEND UVCPP_CAPI_TESTS
    capi_quic_h3_func.c  # quic + http3：C 写的两端在一条 UDP 上真跑三次请求
  )
endif()

# db 那一份跟着 `UVCPP_ENABLE_DB` 走 —— 这是**唯一一片有自己开关的**：别的几片
# 都被 CAPI 的守卫链顺手拉起来了，db 不是任何片的依赖，也不是任何片的下层，所以
# `CAPI=ON, DB=OFF` 是个合法组合，那时 `uvcpp_c_db.h` 里的声明一个都不存在。
#
# 后端的开关**不**参与这里的判断：本用例的后端是从**环境变量里的连接串**来的
# （见文件开头那段），一个后端都没编进来时它自己退 3（未判定）。写成
# `if(UVCPP_ENABLE_DB AND UVCPP_ENABLE_DB_SQLITE)` 的话，`db-servers` 那条腿
# （SQLite 关着、只有远端两个）会**看不到这个用例** —— 而那正是最该跑它的一格。
if(UVCPP_ENABLE_DB)
  list(APPEND UVCPP_CAPI_TESTS
    capi_db_func.c       # db：连接 / 参数 / 结果集 / 事务 / 池 / 异步，逐后端一遍
  )
endif()

foreach(_capi_src ${UVCPP_CAPI_TESTS})
  get_filename_component(_capi_name ${_capi_src} NAME_WE)
  set(_capi_exe "test_${_capi_name}")

  add_executable(${_capi_exe} ${_capi_src})

  # 99：这一层的头本来就是 C99 形状（`stdint.h`、`//` 注释）。**不设
  # C_EXTENSIONS OFF**：MSVC 没有 `-std=c99` 这条，CMake 落到默认 C 模式即可
  # （`.c` 后缀已经保证它按 C 编）。严格到 `-pedantic-errors` 的那一档检查不在
  # 这里做，它在 CI 的 capi 格上直接调 gcc（见 .github/workflows/ 里那段）。
  set_target_properties(${_capi_exe} PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
  )

  target_include_directories(${_capi_exe} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(${_capi_exe} PRIVATE ${UVCPP_CAPI_TEST_VERSION_DEF})

  # 与别的测试同一个形状：链库目标 + libuv（后者的 INTERFACE 里带着 uv.h 的
  # include 目录，所以 `#include <uv.h>` 在本目录不需要额外挂路径）。
  target_link_libraries(${_capi_exe} PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})

  # Windows 上还要 winsock —— C 面的用例直接用 socket 相关的声明（`uv.h`
  # 里那些），libuv 的导入库不替我们带上它。别的测试目录里是同一条。
  #
  # 顺带说明**这里为什么不手动定义任何导出宏**：编 DLL 那一趟由 `uvcpp` 目标
  # 自己带 `UVCPP_C_BUILD`（私有），消费一份 DLL 的人由它的 PUBLIC
  # `UVCPP_C_SHARED` 一路继承到 `dllimport` —— 两个都在库里挂好了，用例只要
  # 链上那个目标就自动站到正确的那一侧。
  if(WIN32)
    target_link_libraries(${_capi_exe} PRIVATE ws2_32)
  endif()

  add_test(NAME ${_capi_exe} COMMAND ${_capi_exe})

  # 这几个用例都会**真起端点**（net / webapp / h2 是 TCP，quic + h3 是 UDP）并且
  # 各自跑事件循环。挂死的形态是"循环里还有活跃句柄、谁都没去关"，那种情形下
  # ctest 只能靠超时救 —— 显式钉一条短的（别指望命令行那个硬编码的 `--timeout`，
  # 它比这大得多）。60s 对最重的那一份（quic+h3：握手 + 三条流 + 关连接，本机
  # 实测 1s 以内）留了两个数量级。
  set_tests_properties(${_capi_exe} PROPERTIES TIMEOUT 60)
endforeach()

if(UVCPP_ENABLE_DB)
  # db 那份用例在"这台机器一个后端都没有"时退 3（未判定），而不是退 0 —— 它
  # **不许**装成跑过了。3 在这条链上的含义与 `tests/functional/` 里那几个
  # `db_*_func` 用例完全一样（`UVCPP_DB_TEST_REQUIRE` 一设就把它变成红），所以
  # ctest 这一侧要照同一套读法：3 = 跳过，不是通过。
  #
  # 认下 `SKIP_RETURN_CODE 3` 会不会把"真判不了"洗成绿？不会：**本机与 CI 的三条
  # `capi` 腿都开着 SQLite**（临时库文件，不需要任何服务端），所以那三步里
  # 它必然是真跑的。能落到 3 的只有"连 SQLite 都没开"的构造，而那种构造下
  # `uvcpp_c_db.h` 里那批同步函数本来就是"没有后端可连" —— 那时跳过是如实的。
  set_tests_properties(test_capi_db_func PROPERTIES SKIP_RETURN_CODE 3)
endif()
