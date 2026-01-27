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


