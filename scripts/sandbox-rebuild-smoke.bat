@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL
cmake --build --preset win-debug --target crd-sandbox
if errorlevel 1 goto :end
rem ⛔ CRD_ASSETS_DIR must point at the repo assets/ tree, or the scene renderer cannot find
rem `assets/vertex/scene.crdv` (+ the shadow/cull/skin cook inputs) and falls back to overlay-only —
rem the smoke test then PASSES while validating an EMPTY frame (0 instances drawn). The RAF migration
rem invariant needs the REAL 11-pass frame, so set it here.
set "CRD_ASSETS_DIR=D:\Dev\cerid\assets"
"D:\Dev\cerid\build\win-debug\sandbox\crd-sandbox.exe" --smoke-test 2
:end
exit /b %ERRORLEVEL%
