@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=%PATH%;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja

echo ============================================================
echo Building win-relwithdebinfo
echo ============================================================
cmake --preset win-relwithdebinfo
if %ERRORLEVEL% neq 0 ( echo CONFIGURE FAILED: win-relwithdebinfo && exit /b 1 )
cmake --build --preset win-relwithdebinfo
if %ERRORLEVEL% neq 0 ( echo BUILD FAILED: win-relwithdebinfo && exit /b 1 )
ctest --preset win-relwithdebinfo --output-on-failure
if %ERRORLEVEL% neq 0 ( echo CTEST FAILED: win-relwithdebinfo && exit /b 1 )
echo PASS: win-relwithdebinfo

echo ============================================================
echo Building win-release
echo ============================================================
cmake --preset win-release
if %ERRORLEVEL% neq 0 ( echo CONFIGURE FAILED: win-release && exit /b 1 )
cmake --build --preset win-release
if %ERRORLEVEL% neq 0 ( echo BUILD FAILED: win-release && exit /b 1 )
ctest --preset win-release --output-on-failure
if %ERRORLEVEL% neq 0 ( echo CTEST FAILED: win-release && exit /b 1 )
echo PASS: win-release

echo ============================================================
echo Building win-clang-cl
echo ============================================================
cmake --preset win-clang-cl
if %ERRORLEVEL% neq 0 ( echo CONFIGURE FAILED: win-clang-cl && exit /b 1 )
cmake --build --preset win-clang-cl
if %ERRORLEVEL% neq 0 ( echo BUILD FAILED: win-clang-cl && exit /b 1 )
ctest --preset win-clang-cl --output-on-failure
if %ERRORLEVEL% neq 0 ( echo CTEST FAILED: win-clang-cl && exit /b 1 )
echo PASS: win-clang-cl

echo ============================================================
echo Building win-asan
echo ============================================================
cmake --preset win-asan
if %ERRORLEVEL% neq 0 ( echo CONFIGURE FAILED: win-asan && exit /b 1 )
cmake --build --preset win-asan
if %ERRORLEVEL% neq 0 ( echo BUILD FAILED: win-asan && exit /b 1 )
set ASAN_DLL_DIR=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64
set PATH=%ASAN_DLL_DIR%;%PATH%
ctest --preset win-asan --output-on-failure
if %ERRORLEVEL% neq 0 ( echo CTEST FAILED: win-asan && exit /b 1 )
echo PASS: win-asan

echo ============================================================
echo All configurations PASSED
echo ============================================================
exit /b 0
