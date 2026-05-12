# scripts/full-sweep.ps1
#
# Single-command Definition-of-Done sweep across every Cerid preset:
#   Win x 10: debug, relwithdebinfo, release, asan, clang-cl, debug-scalar,
#             debug-sse2, shipping, clang-cl-shipping  (build + ctest + sandbox-smoke)
#             + tidy (build-only)
#   Linux x 7: linux-gcc-{debug, relwithdebinfo, release, asan, debug-scalar,
#              debug-sse2, shipping}  (build + ctest)
#
# Shipping configs (win-shipping, win-clang-cl-shipping, linux-gcc-shipping)
# now run tests + sandbox-smoke per the 2026-05-11 shipping hardening pass —
# they were build-only previously. Note: win-clang-cl-shipping has thin-LTO
# DISABLED pending investigation of a clang-cl LTO miscompile in the async
# resource path (see docs/debt.md). MSVC + GCC shipping retain full LTO.
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
    [switch]$SkipSandboxSmoke,
    [double]$SandboxSmokeDurationSeconds = 3.0,
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
    Write-Host '  WINDOWS x 9                                                       ' -ForegroundColor Cyan
    Write-Host '====================================================================' -ForegroundColor Cyan

    $winSweepScript = Join-Path $repoRoot 'scripts\.full-sweep-win-tmp.ps1'

    # Inline PS script invoked under vcvars-sourced cmd.exe so MSVC env (INCLUDE
    # / LIB / LIBPATH) is set. Done as a temp file because nesting cmd /c
    # "...powershell -Command \"...\"..." blows up with quoting on non-trivial
    # scripts (the v0e closure attempted that and failed silently every time).
    @'
$presets = @('win-debug','win-relwithdebinfo','win-release','win-asan','win-clang-cl','win-debug-scalar','win-debug-sse2','win-shipping','win-clang-cl-shipping')
$buildOnly = @('win-tidy')
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
    if ($ec -ne 0) { $results[$p] = "CTEST-FAIL exit=$ec"; continue }

    # ---- Sandbox smoke-test ----
    # Boots the sandbox normally and runs the main loop for $SMOKE_DURATION
    # seconds. Catches Vulkan validation, resource init order, profile/preset
    # apply-cycle bugs that ctest doesn't (those only fire when actual draws
    # are issued + actual assets load + render path executes end-to-end).
    # Skipped if -SkipSandboxSmoke or sandbox exe missing (e.g. running on a
    # headless CI box without GPU; gate-on-presence rather than per-config).
    if (-not $SKIP_SANDBOX) {
        $sandboxExe = Join-Path $REPO_ROOT "build/$p/sandbox/crd-sandbox.exe"
        if (Test-Path $sandboxExe) {
            Write-Host "----- sandbox-smoke $p ($SMOKE_DURATION s) -----"
            & $sandboxExe --smoke-test $SMOKE_DURATION
            $ssec = $LASTEXITCODE
            if ($ssec -ne 0) {
                $results[$p] = "SANDBOX-SMOKE-FAIL exit=$ssec"
                continue
            }
            $results[$p] = 'PASS (build+ctest+sandbox)'
        } else {
            Write-Host "[full-sweep] sandbox exe not found at $sandboxExe -- skipping smoke" -ForegroundColor DarkYellow
            $results[$p] = 'PASS (build+ctest, no sandbox)'
        }
    } else {
        $results[$p] = 'PASS (build+ctest, sandbox skipped)'
    }
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

    # Inject -Reconfigure / -SkipSandboxSmoke / smoke duration / repo root via
    # variables read by the inner script (single-quoted here-string protects
    # the rest from PS expansion).
    $reconfFlag    = if ($Reconfigure)       { '$true' } else { '$false' }
    $skipSandFlag  = if ($SkipSandboxSmoke)  { '$true' } else { '$false' }
    $smokeSecs     = $SandboxSmokeDurationSeconds
    $repoRootEsc   = $repoRoot.Replace('\', '/')

    # Bat shim sources vcvars + ASan PATH then runs the PS script.
    $batShim = Join-Path $repoRoot 'scripts\.full-sweep-win-tmp.bat'
    @"
@echo off
call "$VcvarsPath" >NUL
set "PATH=$AsanRuntimeDir;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "`$USE_RECONFIGURE = $reconfFlag; `$SKIP_SANDBOX = $skipSandFlag; `$SMOKE_DURATION = $smokeSecs; `$REPO_ROOT = '$repoRootEsc'; & '$winSweepScript'"
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
        'linux-gcc-debug-sse2',
        'linux-gcc-shipping'
    )

    $wslScript = Join-Path $repoRoot 'scripts\wsl-build.ps1'

    foreach ($p in $linuxPresets) {
        Write-Host "===== $p =====" -ForegroundColor Yellow
        # Note: never use `$args` here — it's a PowerShell automatic variable
        # bound to the enclosing script's arguments; splatting it ignores
        # local assignment and breaks wsl-build.ps1's -Preset binding.
        # Also: use a HASHTABLE for splatting (named args), not an array.
        # Array splatting is POSITIONAL — `@('-Preset', $p)` would bind the
        # literal string "-Preset" to wsl-build.ps1's first positional
        # parameter (which is `Preset`), tripping its ValidateSet.
        $wslArgs = @{Preset = $p}
        if ($Reconfigure) { $wslArgs['Reconfigure'] = $true }
        # Defensive: clear $LASTEXITCODE before the call so a PowerShell-
        # side error (e.g. parameter binding failure that prevents the
        # target script from ever running) is caught via $? rather than
        # silently inheriting the previous command's success code. The
        # 2026-05-10 sweep silently reported PASS for Linux configs that
        # never built because $args was being shadowed; never trust
        # $LASTEXITCODE alone here.
        $LASTEXITCODE = 0
        & $wslScript @wslArgs
        $ec = $LASTEXITCODE
        $ok = $?
        if (-not $ok -or $ec -ne 0) {
            $results[$p] = "FAIL exit=$ec"
        } else {
            $results[$p] = 'PASS'
        }
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
