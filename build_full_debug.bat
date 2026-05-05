@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=%PATH%;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja
cmake --build D:\Dev\cerid\build\win-debug
exit /b %ERRORLEVEL%
