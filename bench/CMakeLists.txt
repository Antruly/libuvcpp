cmake_minimum_required(VERSION 3.20)

project(uvcpp_bench)

# 靶场建在 webapp 之上，所以只有 webapp 打开时才建得出来。
if(NOT UVCPP_BUILD_WEBAPP)
  message(STATUS "bench: UVCPP_BUILD_WEBAPP=OFF，跳过（靶场是 webapp 上的一个服务）")
  return()
endif()

add_executable(uvcpp_bench_server bench_server.cpp)
target_include_directories(uvcpp_bench_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uvcpp_bench_server PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
if(WIN32)
  target_link_libraries(uvcpp_bench_server PRIVATE ws2_32)
endif()

# 微基准：不起网络，纯 CPU。端到端靶场量不出库内的改动（见 bench_router.cpp 顶上），
# 这个才量得出来。和靶场一样是量具，不进 CI。
add_executable(uvcpp_bench_router bench_router.cpp)
target_include_directories(uvcpp_bench_router PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uvcpp_bench_router PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
if(WIN32)
  target_link_libraries(uvcpp_bench_router PRIVATE ws2_32)
endif()
