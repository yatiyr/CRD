@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
echo ===== reconfigure win-debug =====
cmake --preset win-debug
if errorlevel 1 goto :end
echo ===== build crd-draw + shader pack =====
cmake --build --preset win-debug --target crd-draw
if errorlevel 1 goto :end
echo ===== check pack file =====
dir "D:\Dev\cerid\build\win-debug\assets\cooked\draw_shaders.crdr"
:end
exit /b %ERRORLEVEL%
