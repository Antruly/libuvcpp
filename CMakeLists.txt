cmake_minimum_required(VERSION 3.20)
project(uvcpp LANGUAGES C CXX)

option(UVCPP_BUILD_TESTS "Build tests" ON)
option(UVCPP_USE_SYSTEM_LIBUV "Prefer system libuv if found" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

message(STATUS "UVCPP version (from header will be printed at configure)")

# try find system libuv first
find_path(LIBUV_INCLUDE_DIR uv.h)
find_library(LIBUV_LIB NAMES uv libuv)

include(FetchContent)
# Option to build libuv from source (via FetchContent). Default OFF.
option(UVCPP_BUILD_LIBUV_FROM_SOURCE "Fetch and build libuv from source via FetchContent" OFF)
# Default libuv git tag/branch to fetch when building from source.
# Limit default to v1.51.0 (highest supported by this wrapper). Users may override.
set(LIBUV_VERSION "v1.51.0" CACHE STRING "libuv git tag/branch to fetch when using FetchContent")

# If user requests building libuv from source, or system libuv not found and user didn't opt to use system
if (UVCPP_BUILD_LIBUV_FROM_SOURCE OR NOT LIBUV_INCLUDE_DIR OR NOT LIBUV_LIB OR NOT UVCPP_USE_SYSTEM_LIBUV)
  message(STATUS "Fetching libuv via FetchContent (tag=${LIBUV_VERSION})")
  FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG ${LIBUV_VERSION}
  )
  FetchContent_MakeAvailable(libuv)
  # When fetched, prefer building/using the libuv target provided by libuv CMake
  set(LIBUV_INCLUDE_DIR ${libuv_SOURCE_DIR}/include)
  set(LIBUV_LIB uv)
endif()

# After all tests registered, set ENV for each test so PATH includes uvcpp/libuv build dirs and install bin.
if(WIN32)
  get_property(_cmake_all_tests GLOBAL PROPERTY TESTS)
  if(_cmake_all_tests)
    foreach(_t ${_cmake_all_tests})
      set(_env_items "PATH=$<TARGET_FILE_DIR:uvcpp>;$<TARGET_FILE_DIR:uv>;${CMAKE_BINARY_DIR}/$<CONFIG>;$<SHELL_PATH:${CMAKE_BINARY_DIR}/_deps/libuv-build/$<CONFIG>>;${CMAKE_INSTALL_PREFIX}/bin")
      # Note: $<SHELL_PATH:...> used to ensure proper separators on Windows
      set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${_env_items}")
    endforeach()
  endif()
endif()

if(WIN32 AND TARGET uvcpp)
  # Copy uvcpp DLL next to unit and functional test executables after build.
  add_custom_command(TARGET uvcpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uvcpp> "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uvcpp> "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
    COMMENT "Copying uvcpp DLL next to test executables")
endif()

if(WIN32 AND TARGET uv)
  # Copy libuv DLL next to unit and functional test executables after libuv build.
  add_custom_command(TARGET uv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uv> "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uv> "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
    COMMENT "Copying libuv DLL next to test executables")
endif()

if(WIN32)
  # Ensure PATH for tests includes build output dirs and install bin so DLLs are found.
  get_property(_all_tests GLOBAL PROPERTY TESTS)
  if(_all_tests)
    foreach(_t ${_all_tests})
      set(_env_entries "")
      if(TARGET uvcpp)
        list(APPEND _env_entries "PATH=$<TARGET_FILE_DIR:uvcpp>")
      endif()
      if(TARGET uv)
        list(APPEND _env_entries "PATH=$<TARGET_FILE_DIR:uv>")
      endif()
      # Add install bin directory (configured install runtime destination)
      list(APPEND _env_entries "PATH=${CMAKE_INSTALL_PREFIX}/bin")
      string(JOIN ";" _env_str ${_env_entries})
      if(_env_str)
        set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${_env_str}")
      endif()
    endforeach()
  endif()
endif()

# If libuv exported a CMake target (common when using FetchContent), prefer linking to it.
# This ensures proper linkage on Windows (uv target will provide the correct imported library).
if (TARGET uv)
  message(STATUS "Using libuv CMake target 'uv' for linking")
  set(LIBUV_LIB uv)
elseif(NOT LIBUV_LIB)
  message(FATAL_ERROR "libuv not found and no libuv target available. Set UVCPP_USE_SYSTEM_LIBUV=ON or enable UVCPP_BUILD_LIBUV_FROM_SOURCE.")
endif()

include_directories(${LIBUV_INCLUDE_DIR})

# Option to follow libuv's BUILD_SHARED_LIBS to set sensible defaults for uvcpp.
# Define this option before deciding default values for UVCPP_BUILD_SHARED/STATIC.
option(UVCPP_FOLLOW_LIBUV_BUILD "If ON, set UVCPP_BUILD_SHARED/STATIC default to follow libuv's BUILD_SHARED_LIBS" ON)

# Determine default for building shared/static uvcpp based on libuv (if requested).
set(_default_uvcpp_build_shared ON)
if(UVCPP_FOLLOW_LIBUV_BUILD AND DEFINED BUILD_SHARED_LIBS)
  if(BUILD_SHARED_LIBS)
    set(_default_uvcpp_build_shared ON)
  else()
    set(_default_uvcpp_build_shared OFF)
  endif()
endif()

# Provide per-library build options for uvcpp. Defaults follow libuv if requested.
option(UVCPP_BUILD_SHARED "Build shared uvcpp library" ${_default_uvcpp_build_shared})
if(_default_uvcpp_build_shared)
  set(_default_uvcpp_build_static OFF)
else()
  set(_default_uvcpp_build_static ON)
endif()
option(UVCPP_BUILD_STATIC "Build static uvcpp library" ${_default_uvcpp_build_static})

# Organize sources into Visual Studio source groups for readability.
file(GLOB UVCPP_SRC_FILES "${CMAKE_SOURCE_DIR}/src/uvcpp/*.h" "${CMAKE_SOURCE_DIR}/src/uvcpp/*.cpp")
file(GLOB HANDLE_SRC_FILES "${CMAKE_SOURCE_DIR}/src/handle/*.h" "${CMAKE_SOURCE_DIR}/src/handle/*.cpp")
file(GLOB REQ_SRC_FILES "${CMAKE_SOURCE_DIR}/src/req/*.h" "${CMAKE_SOURCE_DIR}/src/req/*.cpp")
source_group("uvcpp" FILES ${UVCPP_SRC_FILES})
source_group("handle" FILES ${HANDLE_SRC_FILES})
source_group("req" FILES ${REQ_SRC_FILES})

# read version from header (src/uvcpp/uvcpp_version.h)
file(READ "${CMAKE_SOURCE_DIR}/src/uvcpp/uvcpp_version.h" UVCPP_VERSION_H)
string(REGEX MATCH "#define[ \t]+UVCPP_VERSION_MAJOR[ \t]+([0-9]+)" _ ${UVCPP_VERSION_H})
set(UVCPP_VERSION_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+UVCPP_VERSION_MINOR[ \t]+([0-9]+)" _ ${UVCPP_VERSION_H})
set(UVCPP_VERSION_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+UVCPP_VERSION_PATCH[ \t]+([0-9]+)" _ ${UVCPP_VERSION_H})
set(UVCPP_VERSION_PATCH ${CMAKE_MATCH_1})
set(UVCPP_VERSION "${UVCPP_VERSION_MAJOR}.${UVCPP_VERSION_MINOR}.${UVCPP_VERSION_PATCH}")
message(STATUS "uvcpp version: ${UVCPP_VERSION}")

# sources
file(GLOB_RECURSE UVCPP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*/*.cpp)
file(GLOB_RECURSE UVCPP_HEADERS CONFIGURE_DEPENDS src/*.h src/*/*.h)

# Create uvcpp targets according to user selection.
# Shared target
if(UVCPP_BUILD_SHARED)
  add_library(uvcpp SHARED ${UVCPP_SOURCES} ${UVCPP_HEADERS})
  target_link_libraries(uvcpp PRIVATE ${LIBUV_LIB})
  if (WIN32)
    target_compile_definitions(uvcpp PRIVATE UVCPP_EXPORTS)
  endif()
  if (UVCPP_BUILD_LIBUV_FROM_SOURCE)
    add_library(uvcpp_s SHARED ${UVCPP_SOURCES} ${UVCPP_HEADERS})
    target_link_libraries(uvcpp_s PRIVATE ${LIBUV_LIB})
    if (WIN32)
      target_compile_definitions(uvcpp_s PRIVATE UVCPP_EXPORTS)
    endif()
  endif()
endif()

# Static target
if(UVCPP_BUILD_STATIC)
  add_library(uvcpp_a STATIC ${UVCPP_SOURCES} ${UVCPP_HEADERS})
  target_link_libraries(uvcpp_a PRIVATE ${LIBUV_LIB})
  if (UVCPP_BUILD_LIBUV_FROM_SOURCE)
    add_library(uvcpp_a_s STATIC ${UVCPP_SOURCES} ${UVCPP_HEADERS})
    target_link_libraries(uvcpp_a_s PRIVATE ${LIBUV_LIB})
  endif()
endif()

# Use target-based include directories so exported/installable targets do not
# contain absolute source-tree paths. During build use the source include
# directory; when installed consumers should use the installed "include".
if (TARGET uvcpp)
  target_include_directories(uvcpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_a)
  target_include_directories(uvcpp_a PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_s)
  target_include_directories(uvcpp_s PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()
if (TARGET uvcpp_a_s)
  target_include_directories(uvcpp_a_s PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  )
endif()

# Add debug postfix 'd' for debug builds so debug artifacts are distinguishable.
if (TARGET uvcpp)
  set_target_properties(uvcpp PROPERTIES DEBUG_POSTFIX "d")
endif()
if (TARGET uvcpp_s)
  set_target_properties(uvcpp_s PROPERTIES DEBUG_POSTFIX "d")
endif()
if (TARGET uvcpp_a)
  set_target_properties(uvcpp_a PROPERTIES DEBUG_POSTFIX "d")
endif()
if (TARGET uvcpp_a_s)
  set_target_properties(uvcpp_a_s PROPERTIES DEBUG_POSTFIX "d")
endif()

# Select the library target that tests/consumers should link to by default.
# Default to the shared non-source `uvcpp` target if present.
if (TARGET uvcpp)
  set(UVCPP_LIB_TARGET uvcpp CACHE INTERNAL "Which uvcpp target tests should link to")
elseif(TARGET uvcpp_s)
  set(UVCPP_LIB_TARGET uvcpp_s CACHE INTERNAL "Which uvcpp target tests should link to")
elseif(TARGET uvcpp_a)
  set(UVCPP_LIB_TARGET uvcpp_a CACHE INTERNAL "Which uvcpp target tests should link to")
elseif(TARGET uvcpp_a_s)
  set(UVCPP_LIB_TARGET uvcpp_a_s CACHE INTERNAL "Which uvcpp target tests should link to")
else()
  message(FATAL_ERROR "No uvcpp target was created.")
endif()

if (UVCPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  option(UVCPP_BUILD_FUNCTIONAL "Build functional tests" ON)
  if (UVCPP_BUILD_FUNCTIONAL)
    add_subdirectory(tests/functional)
  endif()
endif()

# Only add the legacy req tests subdirectory if it still exists (we consolidated tests into unit/)
if(EXISTS "${CMAKE_SOURCE_DIR}/tests/req/CMakeLists.txt")
  add_subdirectory(tests/req)
endif()

# install and export
include(CMakePackageConfigHelpers)
set(CONFIG_INSTALL_DIR lib/cmake/uvcpp)

if (TARGET uvcpp)
  install(TARGETS uvcpp
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()
if (TARGET uvcpp_s)
  install(TARGETS uvcpp_s
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()
if (TARGET uvcpp_a)
  install(TARGETS uvcpp_a
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()
if (TARGET uvcpp_a_s)
  install(TARGETS uvcpp_a_s
    EXPORT uvcppTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
endif()

# Install header files (flat) to avoid accidental nested `include/uvcpp/uvcpp/...`
file(GLOB UVCPP_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/uvcpp/*.h")
if(UVCPP_HEADER_FILES)
  install(FILES ${UVCPP_HEADER_FILES} DESTINATION include/uvcpp)
endif()

file(GLOB HANDLE_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/handle/*.h")
if(HANDLE_HEADER_FILES)
  install(FILES ${HANDLE_HEADER_FILES} DESTINATION include/handle)
endif()

file(GLOB REQ_HEADER_FILES "${CMAKE_SOURCE_DIR}/src/req/*.h")
if(REQ_HEADER_FILES)
  install(FILES ${REQ_HEADER_FILES} DESTINATION include/req)
endif()

# If we built libuv from source as part of this project, install its public headers too
if(UVCPP_BUILD_LIBUV_FROM_SOURCE AND DEFINED libuv_SOURCE_DIR)
  message(STATUS "Installing libuv headers from ${libuv_SOURCE_DIR}/include into include/libuv")
  install(DIRECTORY ${libuv_SOURCE_DIR}/include/ DESTINATION include/libuv FILES_MATCHING PATTERN "*.h")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/uvcpp.h")
  install(FILES ${CMAKE_SOURCE_DIR}/src/uvcpp.h DESTINATION include)
endif()

write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/uvcppConfigVersion.cmake"
  VERSION ${UVCPP_VERSION}
  COMPATIBILITY AnyNewerVersion)

configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/uvcppConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/uvcppConfig.cmake"
  INSTALL_DESTINATION ${CONFIG_INSTALL_DIR})

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/uvcppConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/uvcppConfigVersion.cmake"
        DESTINATION ${CONFIG_INSTALL_DIR})

install(EXPORT uvcppTargets FILE uvcppTargets.cmake NAMESPACE uvcpp:: DESTINATION ${CONFIG_INSTALL_DIR})

message(STATUS "Configured uvcpp (shared=${BUILD_SHARED_LIBS})")

# RUN_TESTS target: builds available test executables and runs CTest.
# It will only run tests that were added during configure (so it respects
# UVCPP_BUILD_TESTS / UVCPP_BUILD_FUNCTIONAL and which test targets exist).
if (UVCPP_BUILD_TESTS)
  # Collect test executables that may exist in this build.
  set(_run_tests_deps)
  if (TARGET test_handle_unit)
    list(APPEND _run_tests_deps test_handle_unit)
  endif()
  if (TARGET test_req_unit)
    list(APPEND _run_tests_deps test_req_unit)
  endif()
  if (TARGET test_uvcpp_unit)
    list(APPEND _run_tests_deps test_uvcpp_unit)
  endif()
  # Collect functional test executables created under tests/functional
  file(GLOB _func_test_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/functional/*.cpp)
  set(_run_func_deps)
  foreach(_fsrc ${_func_test_sources})
    get_filename_component(_name ${_fsrc} NAME_WE)
    string(REGEX REPLACE "[^a-zA-Z0-9_]" "_" exe_name "test_${_name}")
    if(TARGET ${exe_name})
      list(APPEND _run_func_deps ${exe_name})
    endif()
  endforeach()
  # Functional tests are registered via add_test; ensure ctest is invoked.
  # Note: use generator-expression $<CONFIG> so multi-config generators (Visual Studio)
  # pass the correct configuration to ctest (-C <config>).
  add_custom_target(run_uvcpp_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
    DEPENDS ${_run_tests_deps} ${_run_func_deps}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Run all configured uvcpp tests (unit + functional)"
  )
endif()

if(WIN32)
  add_custom_target(copy_test_dlls
    COMMENT "Copy uvcpp/libuv DLLs into test folders"
  )

  if(TARGET uvcpp)
    add_custom_command(TARGET copy_test_dlls PRE_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uvcpp> "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uvcpp> "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
      DEPENDS uvcpp
    )
  endif()
  if(TARGET uv)
    add_custom_command(TARGET copy_test_dlls PRE_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uv> "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:uv> "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/"
      DEPENDS uv
    )
  endif()
  # Also attempt to copy libuv DLL directly from the populated _deps build folder if present.
  add_custom_command(TARGET copy_test_dlls PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/_deps/libuv-build/$<CONFIG>/uv.dll" "${CMAKE_BINARY_DIR}/tests/unit/$<CONFIG>/" || ${CMAKE_COMMAND} -E echo "libuv dll not found in _deps build folder"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/_deps/libuv-build/$<CONFIG>/uv.dll" "${CMAKE_BINARY_DIR}/tests/functional/$<CONFIG>/" || ${CMAKE_COMMAND} -E echo ""
  )

  add_dependencies(run_uvcpp_tests copy_test_dlls)
endif()

# Ensure test executables can find uvcpp/libuv DLLs at runtime (Windows).
# For each test that exists, add PATH entries pointing to the uvcpp and libuv target dirs.
if(WIN32)
  set(_test_list test_handle_unit test_req_unit test_uvcpp_unit)
  foreach(_t ${_test_list})
    if (EXISTS "${CMAKE_BINARY_DIR}/tests/unit/${CMAKE_CFG_INTDIR}/${_t}.exe" OR TARGET ${_t})
      # Build environment string using generator expressions if targets exist.
      set(_env "")
      if(TARGET uvcpp)
        list(APPEND _env "PATH=$<TARGET_FILE_DIR:uvcpp>")
      endif()
      if(TARGET uv)
        list(APPEND _env "PATH=$<TARGET_FILE_DIR:uv>")
      endif()
      if(_env)
        string(JOIN ";" _env_str ${_env})
        # Only set properties for tests that exist in the current CTest registry.
        get_property(_cmake_all_tests GLOBAL PROPERTY TESTS)
        list(FIND _cmake_all_tests ${_t} _test_index)
        if(NOT _test_index EQUAL -1)
          set_tests_properties(${_t} PROPERTIES ENVIRONMENT "${_env_str}")
        endif()
      endif()
    endif()
  endforeach()
endif()
