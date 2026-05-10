@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
echo ===== reconfigure win-debug =====
cmake --preset win-debug
if errorlevel 1 goto :end
echo ===== build crd-draw + tests =====
cmake --build --preset win-debug --target crd-draw-tests
if errorlevel 1 goto :end
echo ===== ctest draw =====
ctest --preset win-debug -R "v1a-draw" --output-on-failure
:end
exit /b %ERRORLEVEL%
