cmake_minimum_required(VERSION 3.20)

project(uvcpp_examples)

# 示例用的都是公开 API。webapp_demo 建在 webapp 之上，所以只有它打开时才建 ——
# 这样 UVCPP_BUILD_EXAMPLES=ON 配一个 web 关掉的构建也能过去（只是没有示例）。
if(NOT UVCPP_BUILD_WEBAPP)
  message(STATUS "examples: UVCPP_BUILD_WEBAPP=OFF，跳过（暂无不依赖 webapp 的示例）")
  return()
endif()

file(GLOB EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(ex_src ${EXAMPLE_SOURCES})
  get_filename_component(ex_name ${ex_src} NAME_WE)
  add_executable(${ex_name} ${ex_src})
  target_include_directories(${ex_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(${ex_name} PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
  if(WIN32)
    target_link_libraries(${ex_name} PRIVATE ws2_32)
  endif()
endforeach()
