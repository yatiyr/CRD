@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
echo ===== reconfigure win-debug =====
cmake --preset win-debug
if errorlevel 1 goto :end
echo ===== build crd-eylem + crd-eylem-tests =====
cmake --build --preset win-debug --target crd-eylem-tests
if errorlevel 1 goto :end
echo ===== ctest eylem =====
ctest --preset win-debug -R "eylem" --output-on-failure
:end
exit /b %ERRORLEVEL%
