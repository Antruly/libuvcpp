# Expand module tests
# Only built when UVCPP_BUILD_EXPAND is ON

add_executable(test_memory_pool memory_pool_test.cpp)
target_link_libraries(test_memory_pool PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
add_test(NAME test_memory_pool COMMAND test_memory_pool)

if(WIN32 AND TARGET uv)
  add_custom_command(TARGET test_memory_pool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uv> "$<TARGET_FILE_DIR:test_memory_pool>/"
  )
endif()

# 池的自足性：池自己的元数据（线程缓存 / span 头 / impl）不能走全局 `new`。
#
# 这个 exe **把池的实现文件编进来，并且不链 `${UVCPP_LIB_TARGET}`** —— 判据要
# 抢过全局 new/delete，而库那份 `.cpp` 里的 `new` 绑的是它自己那个 CRT，exe 的
# 覆盖接不到（于是"库没打补丁"也能跑绿，判据就成了空的）。该文件不引用任何
# `uv_*` 符号，所以也不需要链 libuv —— 头还是要，`uvcpp_export.h` 往下会牵到 `uv.h`。
add_executable(test_pool_self_sufficiency
  pool_self_sufficiency_test.cpp
  ${CMAKE_SOURCE_DIR}/src/expand/uvcpp_page_heap.cpp
)
target_include_directories(test_pool_self_sufficiency PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${LIBUV_INCLUDE_DIR}
  # 生成头（`uvcpp/uvcpp_config.h`）：`uvcpp_alloc.h` 引它。库侧是从构建目录里
  # 拿到它的，这个 exe 不链库，所以得自己挂上。
  ${CMAKE_BINARY_DIR}/include
)
if(WIN32)
  target_link_libraries(test_pool_self_sufficiency PRIVATE ws2_32)
endif()
add_test(NAME test_pool_self_sufficiency COMMAND test_pool_self_sufficiency)

# 缺陷的原始形态是**卡死**而不是崩（函数局部静态的初始化守卫在同一线程里重入
# 不递归），而卡死只能靠超时抓到。用例自己已经把重入抓在手里当场 `abort()`，
# 但万一将来多出一处不经过那条判据的路径，超时是最后一道 —— 所以显式钉一条，
# 别指望 ctest 命令行的 `--timeout`（CI 里那个是硬编码的）。
set_tests_properties(test_pool_self_sufficiency PROPERTIES TIMEOUT 30)
