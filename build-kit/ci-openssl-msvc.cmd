@echo off
rem Build OpenSSL 1.1.1w for a VS kit.
rem   %1 VCVARS  %2 VCARCH  %3 OPENSSL_TARGET  %4 ARCH
setlocal
set "ROOT=%APPVEYOR_BUILD_FOLDER%"
call "%~1" %~2 || exit /b 1
set "PATH=C:\Strawberry\perl\bin;%PATH%"
cd /d "%ROOT%\deps\openssl-1.1.1w" || exit /b 1
perl Configure %~3 no-shared no-asm --prefix="%ROOT%\deps\openssl-%~4" --openssldir="%ROOT%\deps\openssl-%~4" || exit /b 1
nmake || (
  echo [ci-openssl-msvc] nmake failed, dumping diagnostics
  where nmake
  where perl
  perl -v
  powershell -NoProfile -Command "Get-Content Makefile -TotalCount 40"
  exit /b 1
)
nmake install_sw || exit /b 1
