@echo off
rem Portable kit entry point. See build-kit\README.md.
rem
rem   build.cmd vs2015-x86
rem   build.cmd vs2013-x86 -Profile web-lite
rem   build.cmd mingw32
if /I "%~1"=="mingw32" goto mingw
if /I "%~1"=="mingw64" goto mingw

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%

:mingw
call "%~dp0build-mingw.cmd" %*
exit /b %ERRORLEVEL%
