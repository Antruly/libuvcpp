# 单表 ORM 示例。和 webapp_demo 不同，它只依赖 db + orm，所以只要
# UVCPP_BUILD_ORM=ON 就构建，不需要 webapp。
add_executable(orm_crud main.cpp)

# 树内构建用仓库的 src/ 作为 include 根；链接 uvcpp 还会带来生成的
# uvcpp_config.h 目录与 libuv 的 include。
target_include_directories(orm_crud PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(orm_crud PRIVATE ${UVCPP_LIB_TARGET} ${LIBUV_LIB})

# 静态库构建下，db 的客户端库（这里只有 libsqlite3）不会随导入目标传播，
# 与 tests/functional/CMakeLists.txt 里 db/orm 用例补的那条是同一个原因。
if(UVCPP_DB_LINK_LIBS)
  target_link_libraries(orm_crud PRIVATE ${UVCPP_DB_LINK_LIBS})
endif()

if(WIN32)
  target_link_libraries(orm_crud PRIVATE ws2_32)
endif()
