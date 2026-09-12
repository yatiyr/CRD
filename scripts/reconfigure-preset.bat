@echo off
rem Compatibility entry: configuration has one implementation.
call "%~dp0configure-preset.bat" %*
exit /b %ERRORLEVEL%
