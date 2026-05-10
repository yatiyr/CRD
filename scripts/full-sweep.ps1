# scripts/full-sweep.ps1
#
# Single-command Definition-of-Done sweep across every Cerid preset:
#   Win x 8: debug, relwithdebinfo, release, asan, clang-cl, debug-scalar
#            (build + ctest) + tidy, shipping (build-only)
#   Linux x 6: linux-gcc-{debug, relwithdebinfo, release, asan, debug-scalar,
#              shipping}  (build + ctest; shipping is build-only, mirroring CI)
#
# This is the script slice closure must run -- bench-target sweeps and
# incremental builds DO NOT count, since they miss release-class LNK errors
# and ctest-only failures (e.g. UTF-8 argv mojibake in test names). See the
# Phase 3.1 v0e post-mortem for the bug class this script is designed to
# prevent.
#
# Usage:
#   .\scripts\full-sweep.ps1                 # full sweep (Win + Linux)
#   .\scripts\full-sweep.ps1 -SkipLinux      # Win only (faster local check)
#   .\scripts\full-sweep.ps1 -SkipWin        # Linux only (CI-mirror lane)
#   .\scripts\full-sweep.ps1 -Reconfigure    # blow away build dirs first
#
# Exit code: 0 if every step PASSed, non-zero (count of failures) otherwise.

[CmdletBinding()]
param(
    [switch]$SkipWin,
    [switch]$SkipLinux,
    [switch]$Reconfigure,
    [string]$VcvarsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$AsanRuntimeDir = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64'
)

$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path "$PSScriptRoot/..").Path
$startTime = Get-Date

# Per-config sweep results: PASS / FAIL : <reason>
$results = [ordered]@{}

# --- Win -------------------------------------------------------------------

if (-not $SkipWin) {
    Write-Host '====================================================================' -ForegroundColor Cyan
    Write-Host '  WINDOWS x 8                                                       ' -ForegroundColor Cyan
    Write-Host '====================================================================' -ForegroundColor Cyan

    $winSweepScript = Join-Path $repoRoot 'scripts\.full-sweep-win-tmp.ps1'

    # Inline PS script invoked under vcvars-sourced cmd.exe so MSVC env (INCLUDE
    # / LIB / LIBPATH) is set. Done as a temp file because nesting cmd /c
    # "...powershell -Command \"...\"..." blows up with quoting on non-trivial
    # scripts (the v0e closure attempted that and failed silently every time).
    @'
$presets = @('win-debug','win-relwithdebinfo','win-release','win-asan','win-clang-cl','win-debug-scalar')
$buildOnly = @('win-tidy','win-shipping')
$results = [ordered]@{}

foreach ($p in $presets) {
    Write-Host "===== $p =====" -ForegroundColor Yellow
    if ($USE_RECONFIGURE) {
        Write-Host "[full-sweep] Reconfiguring $p..."
        & cmake --preset $p
        if ($LASTEXITCODE -ne 0) {
            $results[$p] = "CONFIGURE-FAIL exit=$LASTEXITCODE"
            continue
        }
    }
    & cmake --build --preset $p
    if ($LASTEXITCODE -ne 0) {
        $results[$p] = "BUILD-FAIL exit=$LASTEXITCODE"
        continue
    }
    Write-Host "----- ctest $p -----"
    & ctest --preset $p --output-on-failure
    $ec = $LASTEXITCODE
    if ($ec -ne 0) { $results[$p] = "CTEST-FAIL exit=$ec" } else { $results[$p] = 'PASS' }
}

foreach ($p in $buildOnly) {
    Write-Host "===== $p (build-only) =====" -ForegroundColor Yellow
    if ($USE_RECONFIGURE) {
        & cmake --preset $p
        if ($LASTEXITCODE -ne 0) { $results[$p] = "CONFIGURE-FAIL exit=$LASTEXITCODE"; continue }
    }
    & cmake --build --preset $p
    if ($LASTEXITCODE -ne 0) { $results[$p] = "BUILD-FAIL exit=$LASTEXITCODE" } else { $results[$p] = 'PASS (build)' }
}

Write-Host ''
Write-Host '----- WIN SUMMARY -----' -ForegroundColor Cyan
foreach ($k in $results.Keys) { Write-Host ("  {0,-22} {1}" -f $k, $results[$k]) }

# Emit a parseable result line at the very end the orchestrator can grep.
foreach ($k in $results.Keys) { Write-Host ("CRD_RESULT {0} {1}" -f $k, $results[$k]) }
'@ | Out-File -FilePath $winSweepScript -Encoding utf8

    # Inject -Reconfigure preference via env var (script reads $USE_RECONFIGURE).
    $reconfFlag = if ($Reconfigure) { '$true' } else { '$false' }

    # Bat shim sources vcvars + ASan PATH then runs the PS script.
    $batShim = Join-Path $repoRoot 'scripts\.full-sweep-win-tmp.bat'
    @"
@echo off
call "$VcvarsPath" >NUL
set "PATH=$AsanRuntimeDir;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "`$USE_RECONFIGURE = $reconfFlag; & '$winSweepScript'"
exit /b %ERRORLEVEL%
"@ | Out-File -FilePath $batShim -Encoding ascii

    $winOutput = & cmd /c $batShim
    $winOutput | ForEach-Object { Write-Host $_ }

    # Parse CRD_RESULT lines for the unified summary.
    foreach ($line in $winOutput) {
        if ($line -match '^CRD_RESULT (\S+) (.+)$') {
            $results[$Matches[1]] = $Matches[2]
        }
    }

    Remove-Item $winSweepScript, $batShim -Force -ErrorAction SilentlyContinue
}

# --- Linux -----------------------------------------------------------------

if (-not $SkipLinux) {
    Write-Host '====================================================================' -ForegroundColor Cyan
    Write-Host '  LINUX x 6 (via WSL)                                               ' -ForegroundColor Cyan
    Write-Host '====================================================================' -ForegroundColor Cyan

    $linuxPresets = @(
        'linux-gcc-debug',
        'linux-gcc-relwithdebinfo',
        'linux-gcc-release',
        'linux-gcc-asan',
        'linux-gcc-debug-scalar',
        'linux-gcc-shipping'
    )

    $wslScript = Join-Path $repoRoot 'scripts\wsl-build.ps1'

    foreach ($p in $linuxPresets) {
        Write-Host "===== $p =====" -ForegroundColor Yellow
        $args = @('-Preset', $p)
        if ($Reconfigure) { $args += '-Reconfigure' }
        & $wslScript @args
        $ec = $LASTEXITCODE
        if ($ec -ne 0) { $results[$p] = "FAIL exit=$ec" } else { $results[$p] = 'PASS' }
    }
}

# --- Unified summary -------------------------------------------------------

$elapsed = (Get-Date) - $startTime

Write-Host ''
Write-Host '====================================================================' -ForegroundColor Cyan
Write-Host '  FULL SWEEP SUMMARY                                                ' -ForegroundColor Cyan
Write-Host '====================================================================' -ForegroundColor Cyan
$failCount = 0
foreach ($k in $results.Keys) {
    $status = $results[$k]
    $color = if ($status -like 'PASS*') { 'Green' } else { 'Red'; $failCount++ }
    Write-Host ("  {0,-26} {1}" -f $k, $status) -ForegroundColor $color
}
Write-Host ''
Write-Host ("  Total: $($results.Count) configs, $failCount failed, elapsed {0:mm\:ss}" -f $elapsed)

if ($failCount -gt 0) {
    Write-Host '  RESULT: FAIL' -ForegroundColor Red
    exit $failCount
}
Write-Host '  RESULT: PASS' -ForegroundColor Green
exit 0
