@echo off
rem Build/install uvcpp for a MinGW kit.
rem   %1 MINGW_DIR  %2 ARCH  %3 BUILD_DIR
setlocal
set "ROOT=%APPVEYOR_BUILD_FOLDER%"
set "MINGW_DIR=%~1"
set "ARCH=%~2"
set "BUILD_DIR=%~3"
set "PATH=%ROOT%\deps\%MINGW_DIR%\bin;%ROOT%\deps\cmake\bin;%PATH%"
cd /d "%ROOT%" || exit /b 1
cmake -S . -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DUVCPP_FEATURES=full ^
  -DUVCPP_REQUIRE_FEATURES=net;ssl;web-server;web-client;web-ws;web-static;http-compress;webapp ^
  -DUVCPP_WINVER=0x0601 ^
  -DUVCPP_BUILD_TESTS=OFF ^
  -DUVCPP_STATIC_RUNTIME=ON ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DUVCPP_BUILD_SHARED=ON ^
  -DUVCPP_BUILD_STATIC=OFF ^
  -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON ^
  -DLIBUV_VERSION=v1.40.0 ^
  -DOPENSSL_ROOT_DIR="%ROOT%\deps\openssl-%ARCH%" ^
  -DOPENSSL_USE_STATIC_LIBS=ON || exit /b 1
cmake --build "%BUILD_DIR%" --parallel 4 || exit /b 1
cmake --install "%BUILD_DIR%" --prefix stage || exit /b 1

