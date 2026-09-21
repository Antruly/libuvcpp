cmake_minimum_required(VERSION 3.20)

project(uvcpp_unit_tests)

set(HANDLE_UNIT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/handle_unit.cpp)
set(REQ_UNIT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/req_unit.cpp)
set(UVCPP_UNIT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/uvcpp_unit.cpp)

add_executable(test_handle_unit ${HANDLE_UNIT_SRC})
target_include_directories(test_handle_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_handle_unit PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
if(WIN32)
  target_link_libraries(test_handle_unit PRIVATE ws2_32)
endif()
add_test(NAME test_handle_unit COMMAND test_handle_unit)

add_executable(test_req_unit ${REQ_UNIT_SRC})
target_include_directories(test_req_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_req_unit PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
if(WIN32)
  target_link_libraries(test_req_unit PRIVATE ws2_32)
endif()
add_test(NAME test_req_unit COMMAND test_req_unit)

add_executable(test_uvcpp_unit ${UVCPP_UNIT_SRC})
target_include_directories(test_uvcpp_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_uvcpp_unit PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})
if(WIN32)
  target_link_libraries(test_uvcpp_unit PRIVATE ws2_32)
endif()
add_test(NAME test_uvcpp_unit COMMAND test_uvcpp_unit)

# 头名查找的分配数门禁。**故意不链 ${UVCPP_LIB_TARGET}**：判据要靠覆盖全局
# operator new 计数，而 DLL 里的分配走它自己那份 CRT，exe 里覆盖接不到 —— 链库
# 会让这个用例在"库根本没改"的情况下也跑绿。要证的四个函数全是头内 inline，
# 编进本 exe 才看得见（同一个坑见 tests/expand/pool_self_sufficiency_test.cpp）。
# uvcpp_http_common.h 整个落在 #if UVCPP_WEB_ENABLE 内，所以跟着 WEB 开关走。
if(UVCPP_BUILD_WEB)
  add_executable(test_http_name_alloc_unit ${CMAKE_CURRENT_SOURCE_DIR}/http_name_alloc_unit.cpp)
  target_include_directories(test_http_name_alloc_unit PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/include)
  add_test(NAME test_http_name_alloc_unit COMMAND test_http_name_alloc_unit)
  set_tests_properties(test_http_name_alloc_unit PROPERTIES TIMEOUT 30)
endif()


