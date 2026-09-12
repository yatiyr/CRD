@echo off
setlocal
if "%~2"=="" (echo Usage: run-ctest.bat ^<build-dir^> ^<regex^> & exit /b 2)
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0.."
"%CRD_CTEST%" --test-dir "%~1" -R "%~2" --timeout 180 --no-tests=error --output-on-failure
exit /b %ERRORLEVEL%
