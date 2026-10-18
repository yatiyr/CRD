@echo off
setlocal
if "%~1"=="" (echo Usage: configure-preset.bat ^<preset^> [CMake options] & exit /b 2)
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0.."
rem The local compiler cache (docs/design/build-performance.md): the pinned sccache as the launcher for the Ninja
rem presets it qualifies, MSVC precompiled headers off. CRD_SCCACHE=off returns a tree to the default graph; options
rem given here come later on the command line and win.
set "CRD_CACHE_ARGS="
for /f "usebackq delims=" %%A in (`python "%~dp0compiler-cache.py" configure-args "%~1"`) do set "CRD_CACHE_ARGS=%%A"
if defined CRD_CACHE_ARGS echo [configure-preset] compiler cache: %CRD_CACHE_ARGS%
"%CRD_CMAKE%" %CRD_CACHE_ARGS% --preset %*
exit /b %ERRORLEVEL%
