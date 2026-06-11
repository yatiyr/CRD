@echo off
rem Helper: configure a preset under vcvars with the STANDALONE CMake
rem (NEVER bare `cmake` under vcvars — the VS fork stores English showIncludes detection on this
rem Turkish-locale host and re-arms the ninja `#deps 0` landmine; CLAUDE.md Troubleshooting).
rem Usage: configure-preset.bat <preset>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d D:\Dev\cerid
"C:\Program Files\CMake\bin\cmake.exe" --preset %1
exit /b %ERRORLEVEL%
