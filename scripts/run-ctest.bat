@echo off
rem Helper: run ctest in a build dir under vcvars (ASan DLL dir prepended for win-asan runs).
rem Usage: run-ctest.bat <build-dir> <regex>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64;%PATH%
cd /d %1
ctest -R "%~2" --output-on-failure
exit /b %ERRORLEVEL%
