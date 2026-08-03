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
