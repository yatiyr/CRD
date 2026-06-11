@echo off
rem Helper: build a single target in a preset build dir under vcvars, with the STANDALONE CMake
rem (NEVER bare `cmake` under vcvars — the VS fork carries locale-broken showIncludes detection; CLAUDE.md).
rem Usage: build-target.bat <build-dir> <target>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"C:\Program Files\CMake\bin\cmake.exe" --build %1 --target %2
exit /b %ERRORLEVEL%
