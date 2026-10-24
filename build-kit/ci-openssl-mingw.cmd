@echo off
rem Build OpenSSL 1.1.1w for a MinGW kit.
rem   %1 MINGW_DIR  %2 OPENSSL_TARGET  %3 ARCH
setlocal
set "ROOT=%APPVEYOR_BUILD_FOLDER%"
set "MINGW_DIR=%~1"
set "OPENSSL_TARGET=%~2"
set "ARCH=%~3"
set "PATH=%ROOT%\deps\%MINGW_DIR%\bin;C:\Strawberry\perl\bin;%PATH%"
cd /d "%ROOT%\deps\openssl-1.1.1w" || exit /b 1
perl Configure %OPENSSL_TARGET% no-shared no-asm --prefix="%ROOT%\deps\openssl-%ARCH%" --openssldir="%ROOT%\deps\openssl-%ARCH%" || exit /b 1
mingw32-make || exit /b 1
mingw32-make install_sw || exit /b 1
