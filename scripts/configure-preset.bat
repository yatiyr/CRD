@echo off
setlocal
if "%~1"=="" (echo Usage: configure-preset.bat ^<preset^> [CMake options] & exit /b 2)
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0.."
"%CRD_CMAKE%" --preset %*
exit /b %ERRORLEVEL%
