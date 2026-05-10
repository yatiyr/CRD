@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
cmake --build --preset win-debug --target crd-draw-shaders
exit /b %ERRORLEVEL%
