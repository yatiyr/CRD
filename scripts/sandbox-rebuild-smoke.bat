@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
cmake --build --preset win-debug --target crd-sandbox
if errorlevel 1 goto :end
"D:\Dev\cerid\build\win-debug\sandbox\crd-sandbox.exe" --smoke-test 2
:end
exit /b %ERRORLEVEL%
