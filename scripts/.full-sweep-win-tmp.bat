@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$USE_RECONFIGURE = $false; $SKIP_SANDBOX = $false; $SMOKE_DURATION = 3; $REPO_ROOT = 'D:/Dev/cerid'; & 'D:\Dev\cerid\scripts\.full-sweep-win-tmp.ps1'"
exit /b %ERRORLEVEL%
