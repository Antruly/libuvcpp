cmake_minimum_required(VERSION 3.20)

project(uvcpp_bench)

# 靶场建在 webapp 之上，所以只有 webapp 打开时才建得出来。
if(UVCPP_BUILD_WEBAPP)
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
else()
  message(STATUS "bench: UVCPP_BUILD_WEBAPP=OFF，跳过靶场与微基准"
                 "（这两个都建在 webapp 之上）")
endif()

# QUIC 吞吐量具。**不挂在 webapp 下**：QUIC 住在 net 层，`UVCPP_BUILD_WEBAPP=OFF`
# 时它照样能用，而它恰恰是"webapp 关着"那棵树上唯一能跑的量具。
# 它量的是大载荷吞吐，而那个数字此前只能引库外一份靶场 —— 见文件头。
if(UVCPP_ENABLE_QUIC)
  add_executable(uvcpp_bench_quic bench_quic.cpp)
  target_include_directories(uvcpp_bench_quic PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(uvcpp_bench_quic PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
  if(WIN32)
    target_link_libraries(uvcpp_bench_quic PRIVATE ws2_32)
  endif()
else()
  message(STATUS "bench: UVCPP_ENABLE_QUIC=OFF，跳过 QUIC 吞吐量具")
endif()
