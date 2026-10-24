# =========================================================================
# uvcpp compatibility / feature selection
# =========================================================================
#
# This file is include()d early (right after the options) and defines only
# macros.  Nothing runs until CMakeLists.txt calls one of them.
#
# Three levels are handled here:
#
#   1. UVCPP_FEATURES      -- a profile name (net / net-ssl / web-lite / web /
#                             full) expanded into the individual options.
#   2. compiler / OS rules  -- features that cannot be built by this toolchain
#                             are switched OFF *before* their dependencies are
#                             fetched, with a reason kept for the summary.
#   3. libuv rules          -- UVCPP_HAVE_* macros derived from the exact
#                             libuv version; these replace the scattered
#                             `#if UV_VERSION_MINOR >= N` checks in public
#                             headers so a consumer sees exactly the API
#                             surface the library was built with.
#
# UVCPP_REQUIRE_FEATURES is checked after all dependency discovery.  It turns
# "silently missing module" into a configure-time hard failure, which is what
# the official Win7 binaries use to assert their advertised feature set.
# -------------------------------------------------------------------------

# -------------------------------------------------------------------------
# Profiles
# -------------------------------------------------------------------------
macro(uvcpp_apply_profile)
  if(NOT UVCPP_FEATURES)
    # Empty means "use the individual UVCPP_* options" (historical behaviour).
  else()
    string(TOLOWER "${UVCPP_FEATURES}" _uvcpp_profile)
    string(STRIP "${_uvcpp_profile}" _uvcpp_profile)

    if(_uvcpp_profile STREQUAL "net")
      set(UVCPP_BUILD_NET ON)
      set(UVCPP_BUILD_WEB OFF)
      set(UVCPP_ENABLE_OPENSSL OFF)
      set(UVCPP_BUILD_WEBAPP OFF)
      set(UVCPP_WEB_SERVER OFF)
      set(UVCPP_WEB_CLIENT OFF)
      set(UVCPP_WEB_WS OFF)
      set(UVCPP_WEB_STATIC OFF)
      set(UVCPP_ENABLE_ZLIB OFF)
      set(UVCPP_ENABLE_BROTLI OFF)
    elseif(_uvcpp_profile STREQUAL "net-ssl")
      set(UVCPP_BUILD_NET ON)
      set(UVCPP_BUILD_WEB OFF)
      set(UVCPP_ENABLE_OPENSSL ON)
      set(UVCPP_BUILD_WEBAPP OFF)
      set(UVCPP_WEB_SERVER OFF)
      set(UVCPP_WEB_CLIENT OFF)
      set(UVCPP_WEB_WS OFF)
      set(UVCPP_WEB_STATIC OFF)
      set(UVCPP_ENABLE_ZLIB OFF)
      set(UVCPP_ENABLE_BROTLI OFF)
    elseif(_uvcpp_profile STREQUAL "web-lite")
      # Basic HTTP server + client only: no WebSocket, no static server, no
      # gzip/brotli.  This is the VS2013 source-build profile.
      set(UVCPP_BUILD_NET ON)
      set(UVCPP_BUILD_WEB ON)
      set(UVCPP_ENABLE_OPENSSL ON)
      set(UVCPP_BUILD_WEBAPP OFF)
      set(UVCPP_WEB_SERVER ON)
      set(UVCPP_WEB_CLIENT ON)
      set(UVCPP_WEB_WS OFF)
      set(UVCPP_WEB_STATIC OFF)
      set(UVCPP_ENABLE_ZLIB OFF)
      set(UVCPP_ENABLE_BROTLI OFF)
    elseif(_uvcpp_profile STREQUAL "web")
      # Full HTTP/1.1 web layer, gzip only.  This is the official Win7 profile.
      set(UVCPP_BUILD_NET ON)
      set(UVCPP_BUILD_WEB ON)
      set(UVCPP_ENABLE_OPENSSL ON)
      set(UVCPP_BUILD_WEBAPP OFF)
      set(UVCPP_WEB_SERVER ON)
      set(UVCPP_WEB_CLIENT ON)
      set(UVCPP_WEB_WS ON)
      set(UVCPP_WEB_STATIC ON)
      set(UVCPP_ENABLE_ZLIB ON)
      set(UVCPP_ENABLE_BROTLI OFF)
    elseif(_uvcpp_profile STREQUAL "full")
      set(UVCPP_BUILD_NET ON)
      set(UVCPP_BUILD_WEB ON)
      set(UVCPP_ENABLE_OPENSSL ON)
      set(UVCPP_BUILD_WEBAPP ON)
      set(UVCPP_WEB_SERVER ON)
      set(UVCPP_WEB_CLIENT ON)
      set(UVCPP_WEB_WS ON)
      set(UVCPP_WEB_STATIC ON)
      set(UVCPP_ENABLE_ZLIB ON)
      set(UVCPP_ENABLE_BROTLI OFF)
    else()
      message(FATAL_ERROR
        "UVCPP_FEATURES='${UVCPP_FEATURES}' is not a known profile. "
        "Use one of: net, net-ssl, web-lite, web, full. "
        "Leave UVCPP_FEATURES empty to use the individual UVCPP_* options.")
    endif()
    message(STATUS "uvcpp feature profile: ${_uvcpp_profile}")
  endif()
endmacro()

# -------------------------------------------------------------------------
# Feature disable helper
# -------------------------------------------------------------------------
# Macro (not function): the option variables live in the including scope.
macro(uvcpp_disable_feature _opt _name _reason)
  if(${_opt})
    set(${_opt} OFF)
    list(APPEND UVCPP_DISABLED_FEATURES "${_name}: ${_reason}")
  endif()
endmacro()

# -------------------------------------------------------------------------
# Compiler / OS rules, and profile-implied dependencies
# -------------------------------------------------------------------------
# Runs before dependency discovery so an impossible feature does not fetch
# its third-party source at all.
macro(uvcpp_apply_compiler_compat)
  set(UVCPP_DISABLED_FEATURES "")

  # Hard floors.  These are not "turn the feature off": below this the core
  # library itself does not build.
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC_VERSION LESS 1800)
    message(FATAL_ERROR
      "uvcpp requires Visual Studio 2013 (MSVC 18.00) or newer. "
      "Current MSVC_VERSION=${MSVC_VERSION}. VS2013 source support is limited "
      "to the net+ssl and web-lite profiles; see doc/compatibility.md.")
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_CXX_COMPILER_VERSION VERSION_LESS "5.3")
    message(FATAL_ERROR
      "uvcpp requires GCC 5.3 or newer. GCC 4.x uses the old libstdc++ ABI "
      "and is not supported; see doc/compatibility.md.")
  endif()

  set(_uvcpp_is_vs2013 FALSE)
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
     MSVC_VERSION GREATER_EQUAL 1800 AND MSVC_VERSION LESS 1900)
    set(_uvcpp_is_vs2013 TRUE)
  endif()

  if(_uvcpp_is_vs2013)
    # VS2013 has no constexpr / thread_local / magic statics / snprintf.
    # The net+ssl and web-lite source profiles work through the shims in
    # uvcpp_compat.h; everything below needs more than that.
    uvcpp_disable_feature(UVCPP_BUILD_WEBAPP "webapp"
      "requires MSVC >= 1900 (VS2015)")
    uvcpp_disable_feature(UVCPP_ENABLE_BROTLI "brotli"
      "requires MSVC >= 1900 (thread_local arena has a constructor)")
    uvcpp_disable_feature(UVCPP_ENABLE_NGHTTP2 "http2"
      "requires MSVC >= 1900 (UCRT vsnprintf and nghttp2's C99 code)")
    uvcpp_disable_feature(UVCPP_ENABLE_QUIC "quic"
      "requires a C11 toolchain and OpenSSL >= 3.2")
    uvcpp_disable_feature(UVCPP_ENABLE_HTTP3 "http3"
      "requires the QUIC transport")
    uvcpp_disable_feature(UVCPP_ENABLE_YYJSON "yyjson"
      "requires MSVC >= 1900 (alignas/std::max_align_t)")
    uvcpp_disable_feature(UVCPP_ENABLE_WSDL "wsdl"
      "requires the webapp module")
    uvcpp_disable_feature(UVCPP_ENABLE_DB "db"
      "not validated on MSVC 2013")
    uvcpp_disable_feature(UVCPP_BUILD_EXPAND "expand"
      "not validated on MSVC 2013")
    uvcpp_disable_feature(UVCPP_ENABLE_CAPI "capi"
      "not validated on MSVC 2013")
  endif()

  # Feature -> module dependencies.
  if(NOT UVCPP_BUILD_WEB)
    # Sub-features are meaningless without the web module.  Do not spam the
    # summary if the user simply chose the default (web OFF).
    set(UVCPP_WEB_SERVER OFF)
    set(UVCPP_WEB_CLIENT OFF)
    set(UVCPP_WEB_WS OFF)
    set(UVCPP_WEB_STATIC OFF)
    uvcpp_disable_feature(UVCPP_BUILD_WEBAPP "webapp" "requires the web module")
    uvcpp_disable_feature(UVCPP_ENABLE_NGHTTP2 "http2" "requires the web module")
    uvcpp_disable_feature(UVCPP_ENABLE_HTTP3 "http3" "requires the web module")
  endif()

  if(NOT UVCPP_WEB_SERVER)
    uvcpp_disable_feature(UVCPP_WEB_STATIC "web-static"
      "requires the web server")
  endif()

  if(NOT UVCPP_ENABLE_OPENSSL)
    uvcpp_disable_feature(UVCPP_ENABLE_QUIC "quic" "requires OpenSSL")
  endif()

  if(NOT UVCPP_ENABLE_QUIC)
    uvcpp_disable_feature(UVCPP_ENABLE_HTTP3 "http3" "requires the QUIC transport")
  endif()

  if(NOT UVCPP_BUILD_WEBAPP)
    uvcpp_disable_feature(UVCPP_ENABLE_WSDL "wsdl" "requires the webapp module")
  endif()

  # The repository test suite assumes the full web profile. Reduced profiles
  # (web-lite, VS2013) should turn tests off; say so instead of failing in a
  # test TU that includes a header which is now empty.
  if(UVCPP_BUILD_TESTS AND UVCPP_BUILD_WEB AND
     (NOT UVCPP_WEB_WS OR NOT UVCPP_WEB_STATIC OR NOT UVCPP_ENABLE_ZLIB))
    message(WARNING
      "UVCPP_BUILD_TESTS=ON but this web profile is reduced "
      "(ws=${UVCPP_WEB_WS} static=${UVCPP_WEB_STATIC} gzip=${UVCPP_ENABLE_ZLIB}). "
      "The test suite assumes the full 'web' profile; pass "
      "-DUVCPP_BUILD_TESTS=OFF for reduced profiles.")
  endif()
endmacro()

# -------------------------------------------------------------------------
# libuv detection and per-API capability macros
# -------------------------------------------------------------------------
# 1.33.0 = 0x012100.  The comparison constants below are decimal
# (major*65536 + minor*256 + patch) because CMake's if() arithmetic is
# decimal; UVCPP_LIBUV_HEX is only for display / the generated header.
macro(uvcpp_detect_libuv)
  set(UVCPP_LIBUV_NUM 0)
  set(UVCPP_LIBUV_HEX "0x000000")
  set(_uvcpp_uv_major "")
  set(_uvcpp_uv_minor "")
  set(_uvcpp_uv_patch "")

  if(DEFINED LIBUV_INCLUDE_DIR AND EXISTS "${LIBUV_INCLUDE_DIR}/uv/version.h")
    file(STRINGS "${LIBUV_INCLUDE_DIR}/uv/version.h" _line
         REGEX "^#define[ \t]+UV_VERSION_MAJOR[ \t]+[0-9]+")
    string(REGEX REPLACE ".*[ \t]([0-9]+).*" "\\1" _uvcpp_uv_major "${_line}")
    file(STRINGS "${LIBUV_INCLUDE_DIR}/uv/version.h" _line
         REGEX "^#define[ \t]+UV_VERSION_MINOR[ \t]+[0-9]+")
    string(REGEX REPLACE ".*[ \t]([0-9]+).*" "\\1" _uvcpp_uv_minor "${_line}")
    file(STRINGS "${LIBUV_INCLUDE_DIR}/uv/version.h" _line
         REGEX "^#define[ \t]+UV_VERSION_PATCH[ \t]+[0-9]+")
    string(REGEX REPLACE ".*[ \t]([0-9]+).*" "\\1" _uvcpp_uv_patch "${_line}")
  endif()

  # Fallback: the FetchContent cache value.  Useful before the source tree
  # exists, and for system libuv trees without uv/version.h (none today).
  if(_uvcpp_uv_major STREQUAL "" AND LIBUV_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(_uvcpp_uv_major "${CMAKE_MATCH_1}")
    set(_uvcpp_uv_minor "${CMAKE_MATCH_2}")
    set(_uvcpp_uv_patch "${CMAKE_MATCH_3}")
  endif()

  if(NOT _uvcpp_uv_major STREQUAL "")
    math(EXPR UVCPP_LIBUV_NUM
         "(${_uvcpp_uv_major} * 65536) + (${_uvcpp_uv_minor} * 256) + ${_uvcpp_uv_patch}")
    math(EXPR UVCPP_LIBUV_HEX
         "(${_uvcpp_uv_major} * 65536) + (${_uvcpp_uv_minor} * 256) + ${_uvcpp_uv_patch}"
         OUTPUT_FORMAT HEXADECIMAL)
    message(STATUS "uvcpp: libuv ${_uvcpp_uv_major}.${_uvcpp_uv_minor}.${_uvcpp_uv_patch} (${UVCPP_LIBUV_HEX})")
  else()
    message(WARNING
      "uvcpp: could not determine the libuv version (no uv/version.h and no "
      "LIBUV_VERSION). Version-gated APIs will be compiled out.")
  endif()

  # 1.34 = 74240, 1.36 = 74752, 1.38 = 75264, 1.39 = 75520,
  # 1.42 = 76288, 1.44 = 76800, 1.45 = 77056, 1.49 = 78080,
  # 1.50 = 78336.
  set(UVCPP_HAVE_FS_MKSTEMP 0)
  set(UVCPP_HAVE_FS_LUTIME 0)
  set(UVCPP_HAVE_FS_SYSTEM_ERROR 0)
  set(UVCPP_HAVE_UDP_RECVMMSG 0)
  set(UVCPP_HAVE_TRY_WRITE2 0)
  set(UVCPP_HAVE_AVAILABLE_PARALLELISM 0)
  set(UVCPP_HAVE_METRICS 0)
  set(UVCPP_HAVE_OS_GROUP 0)
  set(UVCPP_HAVE_PASSWD2 0)
  set(UVCPP_HAVE_TCP_REUSEPORT 0)
  set(UVCPP_HAVE_UDP_REUSEPORT 0)
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 74240)
    set(UVCPP_HAVE_FS_MKSTEMP 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 74752)
    set(UVCPP_HAVE_FS_LUTIME 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 75264)
    set(UVCPP_HAVE_FS_SYSTEM_ERROR 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 75520)
    set(UVCPP_HAVE_UDP_RECVMMSG 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 76288)
    set(UVCPP_HAVE_TRY_WRITE2 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 76800)
    set(UVCPP_HAVE_AVAILABLE_PARALLELISM 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 77056)
    set(UVCPP_HAVE_METRICS 1)
    set(UVCPP_HAVE_OS_GROUP 1)
    set(UVCPP_HAVE_PASSWD2 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 78080)
    set(UVCPP_HAVE_TCP_REUSEPORT 1)
  endif()
  if(UVCPP_LIBUV_NUM GREATER_EQUAL 78336)
    set(UVCPP_HAVE_UDP_REUSEPORT 1)
  endif()
endmacro()

# -------------------------------------------------------------------------
# libuv-version rules
# -------------------------------------------------------------------------
macro(uvcpp_apply_libuv_compat)
  if(UVCPP_ENABLE_QUIC AND UVCPP_LIBUV_NUM LESS 74240)
    uvcpp_disable_feature(UVCPP_ENABLE_QUIC "quic"
      "requires libuv >= 1.34 (uv_sleep)")
  endif()
  if(NOT UVCPP_ENABLE_QUIC)
    uvcpp_disable_feature(UVCPP_ENABLE_HTTP3 "http3" "requires the QUIC transport")
  endif()
endmacro()

# -------------------------------------------------------------------------
# libuv 1.33-1.47 security backport (CVE-2024-24806)
# -------------------------------------------------------------------------
# libuv's IDNA writer truncated a hostname without NUL-terminating it. The
# fix landed in 1.48.0; the Win7 line is pinned to 1.40.0 because that is the
# last libuv release that officially supports Windows 7. FetchContent users
# get the backport applied automatically; a system libuv (< 1.48) must be
# patched by whoever built it.
set(UVCPP_LIBUV_PATCH_FILE
    "${CMAKE_CURRENT_LIST_DIR}/patches/libuv-idna-cve-2024-24806.patch")

macro(uvcpp_patch_libuv)
  if(UVCPP_LIBUV_FROM_SOURCE AND
     LIBUV_VERSION MATCHES "^v?1\\.(3[3-9]|4[0-7])(\\.|$)")
    if(NOT EXISTS "${UVCPP_LIBUV_PATCH_FILE}")
      message(FATAL_ERROR
        "libuv CVE-2024-24806 backport is missing: ${UVCPP_LIBUV_PATCH_FILE}")
    endif()
    if(NOT EXISTS "${libuv_SOURCE_DIR}/.git")
      message(WARNING
        "libuv source override ${libuv_SOURCE_DIR} is not a git checkout; "
        "the CVE-2024-24806 backport was NOT applied. Patch that tree yourself.")
    else()
    execute_process(
      COMMAND git apply --check "${UVCPP_LIBUV_PATCH_FILE}"
      WORKING_DIRECTORY "${libuv_SOURCE_DIR}"
      RESULT_VARIABLE _uvcpp_cve_check
      OUTPUT_QUIET ERROR_QUIET)
    if(_uvcpp_cve_check EQUAL 0)
      execute_process(
        COMMAND git apply --ignore-whitespace "${UVCPP_LIBUV_PATCH_FILE}"
        WORKING_DIRECTORY "${libuv_SOURCE_DIR}"
        RESULT_VARIABLE _uvcpp_cve_rc
        OUTPUT_VARIABLE _uvcpp_cve_out
        ERROR_VARIABLE _uvcpp_cve_err)
      if(NOT _uvcpp_cve_rc EQUAL 0)
        message(FATAL_ERROR
          "Failed to apply the libuv CVE-2024-24806 backport to "
          "${libuv_SOURCE_DIR}: ${_uvcpp_cve_err}")
      endif()
      message(STATUS "libuv: applied CVE-2024-24806 backport (1.33-1.47)")
    else()
      execute_process(
        COMMAND git apply --reverse --check "${UVCPP_LIBUV_PATCH_FILE}"
        WORKING_DIRECTORY "${libuv_SOURCE_DIR}"
        RESULT_VARIABLE _uvcpp_cve_rev
        OUTPUT_QUIET ERROR_QUIET)
      if(NOT _uvcpp_cve_rev EQUAL 0)
        message(FATAL_ERROR
          "libuv ${LIBUV_VERSION} needs the CVE-2024-24806 backport but the "
          "patch neither applies nor is already applied in "
          "${libuv_SOURCE_DIR}. Check the pinned source tree.")
      endif()
    endif()
    endif()
  endif()
endmacro()

# -------------------------------------------------------------------------
# UVCPP_REQUIRE_FEATURES
# -------------------------------------------------------------------------
macro(uvcpp_check_required_features)
  if(UVCPP_REQUIRE_FEATURES)
    foreach(_uvcpp_req IN LISTS UVCPP_REQUIRE_FEATURES)
      string(STRIP "${_uvcpp_req}" _uvcpp_req)
      string(TOLOWER "${_uvcpp_req}" _uvcpp_req)
      set(_uvcpp_ok FALSE)
      if(_uvcpp_req STREQUAL "net")
        if(UVCPP_BUILD_NET)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "ssl")
        if(UVCPP_ENABLE_OPENSSL)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "web")
        if(UVCPP_BUILD_WEB)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "web-server")
        if(UVCPP_BUILD_WEB AND UVCPP_WEB_SERVER)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "web-client")
        if(UVCPP_BUILD_WEB AND UVCPP_WEB_CLIENT)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "web-ws")
        if(UVCPP_BUILD_WEB AND UVCPP_WEB_WS)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "web-static")
        if(UVCPP_BUILD_WEB AND UVCPP_WEB_STATIC)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "http-compress")
        if(UVCPP_ENABLE_ZLIB OR UVCPP_ENABLE_BROTLI)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "webapp")
        if(UVCPP_BUILD_WEBAPP)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "http2")
        if(UVCPP_ENABLE_NGHTTP2)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "quic")
        if(UVCPP_ENABLE_QUIC)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "http3")
        if(UVCPP_ENABLE_HTTP3)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "yyjson")
        if(UVCPP_ENABLE_YYJSON)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "capi")
        if(UVCPP_ENABLE_CAPI)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "db")
        if(UVCPP_ENABLE_DB)
          set(_uvcpp_ok TRUE)
        endif()
      elseif(_uvcpp_req STREQUAL "redis")
        if(UVCPP_ENABLE_REDIS)
          set(_uvcpp_ok TRUE)
        endif()
      else()
        message(FATAL_ERROR
          "UVCPP_REQUIRE_FEATURES: unknown feature '${_uvcpp_req}'. "
          "Known: net, ssl, web, web-server, web-client, web-ws, web-static, "
          "http-compress, webapp, http2, quic, http3, yyjson, capi, db, redis.")
      endif()
      if(NOT _uvcpp_ok)
        message(FATAL_ERROR
          "UVCPP_REQUIRE_FEATURES: '${_uvcpp_req}' is not enabled in this build. "
          "See the 'uvcpp: disabling ...' lines above for the reason, and "
          "doc/compatibility.md for the feature/toolchain matrix.")
      endif()
    endforeach()
  endif()
endmacro()

# -------------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------------
macro(uvcpp_print_summary)
  message(STATUS "uvcpp features: net=${UVCPP_BUILD_NET} ssl=${UVCPP_ENABLE_OPENSSL} "
                 "web=${UVCPP_BUILD_WEB} server=${UVCPP_WEB_SERVER} "
                 "client=${UVCPP_WEB_CLIENT} ws=${UVCPP_WEB_WS} "
                 "static=${UVCPP_WEB_STATIC} gzip=${UVCPP_ENABLE_ZLIB} "
                 "webapp=${UVCPP_BUILD_WEBAPP} http2=${UVCPP_ENABLE_NGHTTP2} "
                 "quic=${UVCPP_ENABLE_QUIC} http3=${UVCPP_ENABLE_HTTP3}")
  if(UVCPP_DISABLED_FEATURES)
    foreach(_uvcpp_disabled IN LISTS UVCPP_DISABLED_FEATURES)
      message(STATUS "uvcpp: disabled ${_uvcpp_disabled}")
    endforeach()
  endif()
endmacro()
