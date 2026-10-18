@echo off
rem The user's one elevated step (2026-10-18), run from an elevated prompt in the checkout:
rem   scripts\elevated-captures.bat
rem 1. DIAG.11b clause (8): WPR samples the overhead workloads in every arm (sample-overhead-wpr.py record), and the
rem    analysis runs at once; the trace stays under build\diag-overhead-wpr\ for unelevated re-analysis.
rem 2. C++ Build Insights (vcperf) around an uncached win-debug build in the cached shape (PCH off), with the MSVC
rem    timing switches of build-profile.py; the traces go to build\build-insights\.
rem Each step reports its own result; one failing does not skip the other. Nothing is installed or granted.
setlocal
call "%~dp0msvc-env.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
echo [elevated] 1 of 2: external sampling of the overhead workloads
python scripts\sample-overhead-wpr.py record --progress
set "CRD_WPR_EXIT=%ERRORLEVEL%"
echo [elevated] 2 of 2: C++ Build Insights around an uncached win-debug build
if exist build\build-insights\win-debug rmdir /s /q build\build-insights\win-debug
python scripts\build-profile.py run --compiler msvc --preset win-debug --jobs 16 --build-dir build\build-insights\win-debug --label build-insights --define CRD_ENABLE_PCH=OFF --output build\build-insights\win-debug.json --vcperf CeridBuildInsights --allow-contended
set "CRD_VCPERF_EXIT=%ERRORLEVEL%"
echo [elevated] sampling exit %CRD_WPR_EXIT% (0 agrees, 6 disagrees, 3 permission), build insights exit %CRD_VCPERF_EXIT% (0 captured)
if not "%CRD_WPR_EXIT%"=="0" if not "%CRD_WPR_EXIT%"=="6" exit /b %CRD_WPR_EXIT%
exit /b %CRD_VCPERF_EXIT%
