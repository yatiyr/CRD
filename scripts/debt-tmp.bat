@echo off
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
if "%1"=="configure" (
    cmake --preset %2
    exit /b %ERRORLEVEL%
)
if "%1"=="build" (
    cmake --build --preset %2 2>&1
    exit /b %ERRORLEVEL%
)
if "%1"=="ctest" (
    ctest --preset %2 --output-on-failure 2>&1
    exit /b %ERRORLEVEL%
)
if "%1"=="smoke" (
    "D:\Dev\cerid\build\%2\sandbox\crd-sandbox.exe" --smoke-test 3 2>&1
    exit /b %ERRORLEVEL%
)
exit /b 1
