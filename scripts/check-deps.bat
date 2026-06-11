@echo off
rem Helper: print ninja's recorded dep count for one object (the `#deps 0` landmine check; CLAUDE.md).
rem Usage: check-deps.bat <build-dir> <obj-path-relative-to-build-dir>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d %1
ninja -t deps %2
exit /b %ERRORLEVEL%
