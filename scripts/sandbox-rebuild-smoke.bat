@echo off
setlocal
call "%~dp0build-target.bat" build/win-debug crd-sandbox
if errorlevel 1 exit /b %ERRORLEVEL%
rem Exercise the real authored asset path, not an empty overlay-only frame.
set "CRD_ASSETS_DIR=%~dp0..\assets"
cd /d "%~dp0..\build\win-debug\sandbox"
"%~dp0..\build\win-debug\sandbox\crd-sandbox.exe" --smoke-test 2
exit /b %ERRORLEVEL%
