@echo off
setlocal
rem Usage: check-deps.bat <build-dir> <object-path-relative-to-build-dir>
if "%~2"=="" exit /b 2
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
pushd "%~1" || exit /b 2
ninja -t deps "%~2"
set "crd_exit=%ERRORLEVEL%"
popd
exit /b %crd_exit%
