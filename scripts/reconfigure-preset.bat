@echo off
rem Helper: re-run a preset CONFIGURE under vcvars with the STANDALONE CMake — the honest repair when a
rem cache lost its make program (a configure that ran outside vcvars leaves CMAKE_MAKE_PROGRAM-NOTFOUND).
rem NEVER hand-edit the cache (the stale-toolset scar); reconfiguring rediscovers the toolchain properly.
rem Usage: reconfigure-preset.bat <preset>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"C:\Program Files\CMake\bin\cmake.exe" --preset %1
exit /b %ERRORLEVEL%
