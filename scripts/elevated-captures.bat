@echo off
rem The user's one elevated step (2026-10-18), run from an elevated prompt in the checkout:
rem   scripts\elevated-captures.bat [step]   (step: all, the default; sampling; build-insights)
rem 1. sampling: DIAG.11b clause (8), WPR samples the overhead workloads in every arm (sample-overhead-wpr.py record),
rem    and the analysis runs at once; the trace stays under build\diag-overhead-wpr\ for unelevated re-analysis.
rem 2. build-insights: C++ Build Insights (vcperf) around an uncached win-debug build in the cached shape (PCH off),
rem    with the MSVC timing switches of build-profile.py; the traces go to build\build-insights\. Refused, naming what
rem    to close, while Visual Studio runs (its project sync conflicts with the configure) or the host is busy.
rem Each step reports its own result; one failing does not skip the other. Nothing is installed or granted.
setlocal
set "CRD_STEP=%~1"
if "%CRD_STEP%"=="" set "CRD_STEP=all"
if /i not "%CRD_STEP%"=="all" if /i not "%CRD_STEP%"=="sampling" if /i not "%CRD_STEP%"=="build-insights" (
    echo usage: scripts\elevated-captures.bat [all^|sampling^|build-insights]
    exit /b 64
)
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
set "CRD_WPR_EXIT=skipped"
set "CRD_VCPERF_EXIT=skipped"
if /i "%CRD_STEP%"=="build-insights" goto build_insights
echo [elevated] 1 of 2: external sampling of the overhead workloads
python scripts\sample-overhead-wpr.py record --progress
set "CRD_WPR_EXIT=%ERRORLEVEL%"
if /i "%CRD_STEP%"=="sampling" goto report
:build_insights
tasklist /nh /fi "imagename eq devenv.exe" | "%SystemRoot%\System32\find.exe" /i "devenv.exe" 1>nul
if errorlevel 1 goto build_insights_run
echo [elevated] build insights refused: Visual Studio is running; close every Visual Studio window, then run
echo [elevated]   scripts\elevated-captures.bat build-insights
set "CRD_VCPERF_EXIT=7"
goto report
:build_insights_run
echo [elevated] 2 of 2: C++ Build Insights around an uncached win-debug build
if exist build\build-insights\win-debug rmdir /s /q build\build-insights\win-debug
python scripts\build-profile.py run --compiler msvc --preset win-debug --jobs 16 --build-dir build\build-insights\win-debug --label build-insights --define CRD_ENABLE_PCH=OFF --output build\build-insights\win-debug.json --vcperf CeridBuildInsights
set "CRD_VCPERF_EXIT=%ERRORLEVEL%"
:report
echo [elevated] sampling exit %CRD_WPR_EXIT% (0 agrees, 6 disagrees, 3 permission), build insights exit %CRD_VCPERF_EXIT% (0 captured, 1 busy host or failure, 7 Visual Studio open)
if not "%CRD_WPR_EXIT%"=="skipped" if not "%CRD_WPR_EXIT%"=="0" if not "%CRD_WPR_EXIT%"=="6" exit /b %CRD_WPR_EXIT%
if "%CRD_VCPERF_EXIT%"=="skipped" exit /b 0
exit /b %CRD_VCPERF_EXIT%
