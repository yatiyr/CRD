# scripts/full-sweep.ps1
#
# Single-command Definition-of-Done sweep across every Cerid preset:
#   Win x 11: debug, relwithdebinfo, release, asan, clang-cl, debug-scalar,
#             debug-sse2, shipping, shipping-profile, clang-cl-shipping
#             (build + ctest + sandbox-smoke) + tidy (build-only)
#   Linux x 7: linux-gcc-{debug, relwithdebinfo, release, asan, debug-scalar,
#              debug-sse2, shipping}  (build + ctest)
#
# shipping-profile = mirror of shipping with CRD_ENABLE_PROFILING=ON. Added
# 2026-05-15 (D-003 v0c) to verify the crd-perf substrate + every gated
# CRD_PERF_* site compiles + runs under MSVC LTCG. See ADR-0079.
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
#   .\scripts\full-sweep.ps1 -BuildJobs 8    # cap Ninja to 8 threads/build (Win+WSL)
#
# -BuildJobs caps the per-build Ninja thread count via CMAKE_BUILD_PARALLEL_LEVEL.
# Default = half the logical cores. This is a HARDWARE-STABILITY guard, not a speed
# knob: the i9-14900K host bugchecks (0xA) under sustained all-core builds (Raptor
# Lake Vmin-shift instability). The 18-config sweep is the single heaviest, longest
# all-core load we run, so the cap matters most here. 0 = uncapped (old behaviour).
# See CLAUDE.md Troubleshooting "Host instability".
#
# Exit code: 0 if every step PASSed, non-zero (count of failures) otherwise.

[CmdletBinding()]
param(
    [switch]$SkipWin,
    [switch]$SkipLinux,
    [switch]$Reconfigure,
    [switch]$SkipSandboxSmoke,
    [double]$SandboxSmokeDurationSeconds = 3.0,
    # Cap Ninja threads per build (CMAKE_BUILD_PARALLEL_LEVEL) on both Win and WSL.
    # Default = half the logical cores. Hardware-stability guard for the i9-14900K
    # host (Raptor Lake instability). 0 = uncapped. See CLAUDE.md "Host instability".
    [int]$BuildJobs = [Math]::Max(1, [int]([Environment]::ProcessorCount / 2)),
    [string]$VcvarsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$AsanRuntimeDir = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64'
)

$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path "$PSScriptRoot/..").Path
$startTime = Get-Date

# ---- COMMIT-HEADROOM REPORT ------------------------------------------------------------------------
# The actual clamp is PER CONFIG and lives in the generated inner script (Set-CrdBuildJobs) -- see the
# scar comment there. Here we only report the starting state so the log records what the sweep was up
# against. Short version: if the host's COMMIT LIMIT is nearly consumed by resident desktop apps,
# whichever build process asks for memory next dies with "LLVM ERROR: out of memory" / 0xC0000005 on a
# random file, which looks exactly like an upstream toolchain bug and is not one.
$os          = Get-CimInstance Win32_OperatingSystem
$commitFree  = [math]::Round($os.FreeVirtualMemory / 1MB, 1)
$commitLimit = [math]::Round($os.TotalVirtualMemorySize / 1MB, 1)
Write-Host ("  Commit: {0} GB free of {1} GB limit" -f $commitFree, $commitLimit) -ForegroundColor DarkCyan
if ($commitFree -lt 8.0)
{
    Write-Host '  ! COMMIT HEADROOM VERY LOW - configs will be clamped hard. Closing Visual Studio / clangd / a DAW' -ForegroundColor Red
    Write-Host '    is the fastest way to make the sweep both fast AND crash-free.' -ForegroundColor Red
}

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
$presets = @('win-debug','win-relwithdebinfo','win-release','win-asan','win-clang-cl','win-debug-scalar','win-debug-sse2','win-shipping','win-shipping-profile','win-clang-cl-shipping')
$buildOnly = @('win-tidy')
$results = [ordered]@{}

# ---- COMMIT-HEADROOM CLAMP, re-measured per config -------------------------------------------------
# A build that outruns the host's COMMIT LIMIT does not fail cleanly: clang-tidy dies with
# "LLVM ERROR: out of memory", or clang-tidy/cl.exe take a 0xC0000005 on a stack growth that cannot
# commit -- on a DIFFERENT random file every run, which reads exactly like an upstream toolchain bug and
# is not one (diagnosed 2026-07-25; see docs/SANITY.md). Budgets are MEASURED, not estimated: a clang-tidy
# edge peaks ~0.20-0.30 GB, a /Od cl.exe ~0.36 GB, but an ⛔ LTCG **link.exe peaks 5.6 GB** -- LINKS are the
# shipping/release constraint. win-shipping CRASHED cl.exe at -j5 (~14.5 GB free) and SUCCEEDED at -j3 while
# bottoming out at 0.26 GB free commit, so the boundary is ~4 GB/edge effective on this desktop.
$CRD_JOBS_CAP  = if ($env:CMAKE_BUILD_PARALLEL_LEVEL) { [int]$env:CMAKE_BUILD_PARALLEL_LEVEL } else { 0 }
$CRD_PER_EDGE  = @{ 'win-debug' = 2.0; 'win-debug-scalar' = 2.0; 'win-debug-sse2' = 2.0; 'win-asan' = 2.5;
                    'win-relwithdebinfo' = 3.0; 'win-release' = 4.0; 'win-shipping' = 4.0;
                    'win-shipping-profile' = 4.0; 'win-clang-cl' = 2.0; 'win-clang-cl-shipping' = 4.0;
                    'win-tidy' = 1.5 }
function Set-CrdBuildJobs([string]$preset) {
    if ($CRD_JOBS_CAP -le 0) { return }
    $free = [math]::Round((Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory / 1MB, 1)
    $per  = if ($CRD_PER_EDGE.ContainsKey($preset)) { $CRD_PER_EDGE[$preset] } else { 2.5 }
    $safe = [Math]::Max(1, [int][Math]::Floor(($free - 2.0) / $per))
    $jobs = [Math]::Min($CRD_JOBS_CAP, $safe)
    $env:CMAKE_BUILD_PARALLEL_LEVEL = "$jobs"
    if ($jobs -lt $CRD_JOBS_CAP) {
        Write-Host ("  ! commit headroom {0} GB @ {1} GB/edge -> ninja {2} -> {3} (a random clang-tidy/cl crash under low headroom is NOT a code defect)" -f $free, $per, $CRD_JOBS_CAP, $jobs) -ForegroundColor Yellow
    } else {
        Write-Host ("  commit headroom {0} GB @ {1} GB/edge -> ninja -j{2}" -f $free, $per, $jobs) -ForegroundColor DarkCyan
    }
}

foreach ($p in $presets) {
    Write-Host "===== $p =====" -ForegroundColor Yellow
    Set-CrdBuildJobs $p
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
    # Per-config ctest log + a generous per-test timeout (300s — long enough for the
    # ASan-instrumented N=512 eigensolvers, short enough to catch a genuine hang).
    # The log captures any CRD assertion text ("...exceeded", "min_pool", etc.) so a
    # failing config can be pinpointed after the sweep without re-running.
    & ctest --preset $p --output-on-failure --timeout 300 -O "$REPO_ROOT/build/ctest-$p.log"
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
    Set-CrdBuildJobs $p
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
    # CMAKE_BUILD_PARALLEL_LEVEL caps Ninja for every `cmake --build` in the inner
    # win script (none pass explicit --parallel). Integer-only — no quoting hazard.
    $buildJobsEnvLine = if ($BuildJobs -gt 0) { "set ""CMAKE_BUILD_PARALLEL_LEVEL=$BuildJobs""" } else { "rem CMAKE_BUILD_PARALLEL_LEVEL uncapped (-BuildJobs 0)" }
    $batShim = Join-Path $repoRoot 'scripts\.full-sweep-win-tmp.bat'
    @"
@echo off
call "$VcvarsPath" >NUL
set "PATH=$AsanRuntimeDir;%PATH%"
rem Perf budgets are SOFT during the sweep (like CI): the sweep is a build+correctness
rem gate, and absolute timings vary with host load/thermal/Raptor-Lake state. An
rem over-budget result logs a warning + the measured number rather than hard-aborting
rem a whole config. Review the printed CRD_PERF warnings for real regressions.
set "CRD_PERF_BUDGET_SOFT=1"
$buildJobsEnvLine
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
        # Same Raptor Lake stability cap applies to WSL builds — they run on the
        # same physical CPU. wsl-build.ps1 exports CMAKE_BUILD_PARALLEL_LEVEL.
        if ($BuildJobs -gt 0) { $wslArgs['BuildJobs'] = $BuildJobs }
        # Defensive: clear $LASTEXITCODE before the call so a PowerShell-
        # side error (e.g. parameter binding failure that prevents the
        # target script from ever running) is caught via $? rather than
        # silently inheriting the previous command's success code. The
        # 2026-05-10 sweep silently reported PASS for Linux configs that
        # never built because $args was being shadowed; never trust
        # $LASTEXITCODE alone here.
        # Clear any prior status file so a stale PASS doesn't survive a
        # wsl-build.ps1 launch failure.
        $statusFile = Join-Path $repoRoot "build/.wsl-build-status-$p"
        if (Test-Path $statusFile) { Remove-Item -Force $statusFile -ErrorAction SilentlyContinue }
        $LASTEXITCODE = 0
        & $wslScript @wslArgs
        $ec = $LASTEXITCODE
        $ok = $?
        # `exit N` from a child .ps1 does NOT reliably propagate to caller's
        # $LASTEXITCODE — verified empirically by the 2026-05-16 v5-close
        # sweep where wsl-build.ps1 printed "FAILED (exit code 1)" yet the
        # parent saw $LASTEXITCODE=0. The status file is authoritative.
        if (Test-Path $statusFile) {
            $statusCode = [int]((Get-Content $statusFile -Raw).Trim())
            if ($statusCode -ne 0) {
                $results[$p] = "FAIL exit=$statusCode"
            } else {
                $results[$p] = 'PASS'
            }
        } elseif (-not $ok -or $ec -ne 0) {
            $results[$p] = "FAIL exit=$ec (no status file)"
        } else {
            $results[$p] = 'PASS (no status file)'
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
