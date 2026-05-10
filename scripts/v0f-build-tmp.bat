@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
echo ===== build crd-math-tests + crd-eylem-tests =====
cmake --build --preset win-debug --target crd-math-tests crd-eylem-tests
if errorlevel 1 goto :end
echo ===== ctest math + eylem =====
ctest --preset win-debug -R "v0f|eylem v1a" --output-on-failure
:end
exit /b %ERRORLEVEL%
