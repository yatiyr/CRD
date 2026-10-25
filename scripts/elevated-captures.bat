@echo off
rem The user's one elevated step (2026-10-18), run from an elevated prompt in the checkout:
rem   scripts\elevated-captures.bat [step]   (step: all, the default; sampling; build-insights)
rem 1. sampling: DIAG.11b clause (8), WPR samples the overhead workloads in every arm (sample-overhead-wpr.py record),
rem    and the analysis runs at once; the trace stays under build\diag-overhead-wpr\ for unelevated re-analysis.
rem    Build crd-diag-overhead on win-shipping-profile and win-shipping at the same commit first: record refuses a
rem    tree that still has work for it (exit 2), so the two binaries come from one source.
rem 2. build-insights: C++ Build Insights (vcperf) around an uncached win-debug build in the cached shape (PCH off),
rem    with the MSVC timing switches of build-profile.py; the traces go to build\build-insights\. Refused while the
rem    host is busy.
rem Each step is refused, naming what to close, while Visual Studio runs: its project sync conflicts with the configure,
rem and its servers ran inside the 2026-10-19 sampling session (one under a harness process's reused PID).
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
call :visual_studio_closed sampling
if errorlevel 1 (
    set "CRD_WPR_EXIT=7"
    goto sampling_done
)
echo [elevated] 1 of 2: external sampling of the overhead workloads
python scripts\sample-overhead-wpr.py record --progress
set "CRD_WPR_EXIT=%ERRORLEVEL%"
:sampling_done
if /i "%CRD_STEP%"=="sampling" goto report
:build_insights
call :visual_studio_closed build-insights
if errorlevel 1 (
    set "CRD_VCPERF_EXIT=7"
    goto report
)
echo [elevated] 2 of 2: C++ Build Insights around an uncached win-debug build
if exist build\build-insights\win-debug rmdir /s /q build\build-insights\win-debug
python scripts\build-profile.py run --compiler msvc --preset win-debug --jobs 16 --build-dir build\build-insights\win-debug --label build-insights --define CRD_ENABLE_PCH=OFF --output build\build-insights\win-debug.json --vcperf CeridBuildInsights
set "CRD_VCPERF_EXIT=%ERRORLEVEL%"
:report
echo [elevated] sampling exit %CRD_WPR_EXIT% (0 agrees, 6 disagrees, 4 unresolved, 2 harness missing or not built at this commit, 3 permission, 7 Visual Studio open), build insights exit %CRD_VCPERF_EXIT% (0 captured, 1 busy host or failure, 7 Visual Studio open)
if not "%CRD_WPR_EXIT%"=="skipped" if not "%CRD_WPR_EXIT%"=="0" if not "%CRD_WPR_EXIT%"=="6" exit /b %CRD_WPR_EXIT%
if "%CRD_VCPERF_EXIT%"=="skipped" exit /b 0
exit /b %CRD_VCPERF_EXIT%

:visual_studio_closed
tasklist /nh /fi "imagename eq devenv.exe" | "%SystemRoot%\System32\find.exe" /i "devenv.exe" 1>nul
if errorlevel 1 exit /b 0
echo [elevated] %1 refused: Visual Studio is running; close every Visual Studio window, then run
echo [elevated]   scripts\elevated-captures.bat %1
exit /b 1
