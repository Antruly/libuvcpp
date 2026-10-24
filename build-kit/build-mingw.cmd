@echo off
rem Plain-batch MinGW builder for a portable kit (no PowerShell needed).
rem   build-mingw.cmd mingw32|mingw64 [profile]
setlocal
for %%I in ("%~dp0..") do set "KIT=%%~fI"
set "ARCH=%~1"
set "PROFILE=%~2"
if "%PROFILE%"=="" set "PROFILE=web"
if /I "%ARCH%"=="mingw32" set "MINGW_DIR=mingw32"
if /I "%ARCH%"=="mingw64" set "MINGW_DIR=mingw64"
if "%MINGW_DIR%"=="" goto usage

set "PATH=%KIT%\deps\%MINGW_DIR%\bin;%KIT%\deps\cmake\bin;%PATH%"
set "KITF=%KIT:\=/%"
set "BUILD=%KIT%\build-kit-%ARCH%"
set "BUILDF=%KITF%/build-kit-%ARCH%"
set "OUT=%KIT%\out\%ARCH%"
set "OUTF=%KITF%/out/%ARCH%"

cd /d "%KIT%"
if errorlevel 1 exit /b 1
cmake -S . -B "%BUILDF%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DUVCPP_FEATURES=%PROFILE% ^
  -DUVCPP_WINVER=0x0601 ^
  -DUVCPP_BUILD_TESTS=OFF ^
  -DUVCPP_STATIC_RUNTIME=ON ^
  -DBUILD_SHARED_LIBS=OFF -DUVCPP_BUILD_SHARED=ON -DUVCPP_BUILD_STATIC=OFF ^
  -DUVCPP_USE_SYSTEM_LIBUV=ON -DUVCPP_BUILD_LIBUV_FROM_SOURCE=OFF ^
  -DLIBUV_INCLUDE_DIR="%KITF%/deps/libuv/include" ^
  -DLIBUV_LIB="%KITF%/deps/libuv/lib/libuv_a.a" ^
  -DOPENSSL_ROOT_DIR="%KITF%/deps/openssl" -DOPENSSL_USE_STATIC_LIBS=ON ^
  -DZLIB_ROOT="%KITF%/deps/zlib" ^
  -DZLIB_LIBRARY="%KITF%/deps/zlib/lib/libzlibstatic.a" ^
  -DZLIB_INCLUDE_DIR="%KITF%/deps/zlib/include" ^
  -DFETCHCONTENT_SOURCE_DIR_LLHTTP="%KITF%/deps/llhttp" ^
  -DCMAKE_INSTALL_PREFIX="%OUTF%"
if errorlevel 1 exit /b 1

cmake --build "%BUILD%" --parallel 4
if errorlevel 1 exit /b 1
cmake --install "%BUILD%"
if errorlevel 1 exit /b 1

if exist "%KIT%\deps\libuv\include" xcopy /E /I /Y "%KIT%\deps\libuv\include\*" "%OUT%\include\" >nul
if exist "%KIT%\deps\openssl\include" xcopy /E /I /Y "%KIT%\deps\openssl\include\*" "%OUT%\include\" >nul
if exist "%KIT%\deps\zlib\include" xcopy /E /I /Y "%KIT%\deps\zlib\include\*" "%OUT%\include\" >nul

echo built: %OUT%

exit /b 0

:usage
echo usage: build-mingw.cmd mingw32 or mingw64 [profile]
exit /b 2
