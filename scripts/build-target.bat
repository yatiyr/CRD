@echo off
setlocal
if "%~2"=="" (echo Usage: build-target.bat ^<build-dir^> ^<target^> & exit /b 2)
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0.."
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=4"
rem A tree configured with the compiler launcher builds through an sccache server on the local cache; the helper
rem starts it with its streams closed and prints SCCACHE_DIR and SCCACHE_CACHE_SIZE (nothing for an uncached tree).
for /f "usebackq delims=" %%L in (`python "%~dp0compiler-cache.py" server "%~1" --shell cmd`) do set "%%L"
"%CRD_CMAKE%" --build "%~1" --target "%~2"
exit /b %ERRORLEVEL%
