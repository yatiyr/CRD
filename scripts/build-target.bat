@echo off
setlocal
if "%~2"=="" (echo Usage: build-target.bat ^<build-dir^> ^<target^> & exit /b 2)
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0.."
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=4"
"%CRD_CMAKE%" --build "%~1" --target "%~2"
exit /b %ERRORLEVEL%
