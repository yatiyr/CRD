@echo off
call "%~dp0build-target.bat" build/win-debug crd-draw-shaders
exit /b %ERRORLEVEL%
