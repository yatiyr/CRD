@echo off
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
set "CMAKE_BUILD_PARALLEL_LEVEL=4"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\Dev\cerid\scripts\.per-slice-check-tmp.ps1"
exit /b %errorlevel%
