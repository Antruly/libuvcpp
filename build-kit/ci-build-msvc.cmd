@echo off
rem Build/install uvcpp for a VS kit.
rem   %1 VCVARS  %2 VCARCH  %3 PRESET  %4 ARCH  %5 BUILD_DIR
setlocal
set "ROOT=%APPVEYOR_BUILD_FOLDER%"
call "%~1" %~2 || exit /b 1
set "PATH=%ROOT%\deps\cmake\bin;%PATH%"
cd /d "%ROOT%" || exit /b 1
cmake --preset %~3 -DOPENSSL_ROOT_DIR="%ROOT%\deps\openssl-%~4" -DOPENSSL_USE_STATIC_LIBS=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded || exit /b 1
cmake --build %~5 --config Release --parallel 4 || exit /b 1
cmake --install %~5 --prefix stage --config Release || exit /b 1

