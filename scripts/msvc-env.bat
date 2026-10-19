@echo off
rem Shared x64 tool discovery. Pin standalone CMake before vcvars can shadow it.
set "CRD_CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
set "CRD_CTEST=%ProgramFiles%\CMake\bin\ctest.exe"
if not exist "%CRD_CMAKE%" (
    echo Standalone CMake is required. See docs/BUILDING.md.
    exit /b 91
)
set "CRD_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%CRD_VSWHERE%" (
    echo Visual Studio Installer vswhere.exe was not found.
    exit /b 92
)
set "CRD_VS_INSTALL="
for /f "usebackq tokens=*" %%I in (`"%CRD_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "CRD_VS_INSTALL=%%I"
if not defined CRD_VS_INSTALL (
    echo Install the Visual Studio x64 C++ build tools.
    exit /b 93
)
rem VsDevCmd runs vswhere.exe by bare name from the installer directory; a shell that sets
rem NoDefaultCurrentDirectoryInExePath (agent shells do) does not search the current directory, so it printed
rem "'vswhere.exe' is not recognized" and left VSCMD_VER unset. The installer directory on PATH finds it either way.
for %%I in ("%CRD_VSWHERE%") do set "PATH=%%~dpI;%PATH%"
call "%CRD_VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b %ERRORLEVEL%
