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
  capi_webapp_func.c   # webapp + web：C 写的 app 与 C 写的客户端互相打
)

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

  # 这两个用例都会**真起一个 TCP 端点**并且各自跑一条事件循环。挂死的形态是
  # "循环里还有活跃句柄、谁都没去关"，那种情形下 ctest 只能靠超时救 —— 显式
  # 钉一条短的（别指望命令行那个硬编码的 `--timeout`，它比这大得多）。
  set_tests_properties(${_capi_exe} PROPERTIES TIMEOUT 60)
endforeach()
