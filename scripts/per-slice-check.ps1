# scripts/per-slice-check.ps1
#
# Per-slice verification: build + ctest across win-debug + win-asan + win-shipping + win-tidy.
#
# A slice is NOT closed until this script returns exit 0. Per the Strategic Execution
# Plan locked 2026-05-15 + CLAUDE.md DoD #8 + memory feedback_per_slice_run_ctest.md:
#
# Guard tests (crd-no-non-ascii-test-names / crd-simd-emission-check /
# crd-no-std-math-check / crd-no-std-sort-check / future crd-no-untagged-physical-numeric)
# are registered as separate ctest tests via add_test(NAME ...) in tests/math/CMakeLists.txt
# and do NOT appear in any test binary's --list-tests output. The test binary saying
# "All tests passed" can coexist with a failing guard test. Both must be green.
#
# This is the lighter-weight per-slice gate; scripts/full-sweep.ps1 is the close gate
# (10 Win + 7 Linux). Per-slice verifies the four most-load-bearing Windows configs:
#   - win-debug: catches /Od + RTC1 bugs + symbol-export issues
#   - win-asan:  catches use-after-free / leak / OOB
#   - win-shipping: catches LTO miscompiles
#   - win-tidy:  catches clang-tidy rule violations
#
# Usage:
#   .\scripts\per-slice-check.ps1                # all four configs (sequential)
#   .\scripts\per-slice-check.ps1 -Parallel      # all four configs in parallel
#                                                  (each in its own Start-Job;
#                                                   total ninja threads = NumProc/NumJobs;
#                                                   logs to scripts\.per-slice-logs\)
#   .\scripts\per-slice-check.ps1 -Parallel -ParallelJobs 4   # override per-job ninja -j
#   .\scripts\per-slice-check.ps1 -SkipShipping  # skip win-shipping (slow LTO)
#   .\scripts\per-slice-check.ps1 -SkipTidy      # skip win-tidy (slow clang-tidy)
#   .\scripts\per-slice-check.ps1 -SkipAsan      # skip win-asan (rarely needed)
#   .\scripts\per-slice-check.ps1 -Reconfigure   # cmake --preset <X> first
#   .\scripts\per-slice-check.ps1 -IncludeRelease # add win-release (catches LTCG
#                                                  miscompiles like the v0-close
#                                                  vtable-middle-insertion bug)
#                                                  — recommended for any slice
#                                                  touching virtual interfaces
#                                                  or heavily-templated code.
#   .\scripts\per-slice-check.ps1 -BuildJobs 8   # cap Ninja to 8 threads per build.
#                                                  Default = half the logical cores.
#                                                  Lowers peak all-core CPU load —
#                                                  REQUIRED on the i9-14900K host
#                                                  (Raptor Lake instability; see
#                                                  CLAUDE.md Troubleshooting "Host
#                                                  instability"). 0 = uncapped (the
#                                                  old all-core behaviour).
#
# Exit code: 0 if every requested config passed, count of failures otherwise.

[CmdletBinding()]
param(
    [switch]$SkipShipping,
    [switch]$SkipTidy,
    [switch]$SkipAsan,
    [switch]$Reconfigure,
    [switch]$Parallel,
    [switch]$IncludeRelease,
    [int]$ParallelJobs = 0,
    # Cap Ninja's thread count per `cmake --build` (sets CMAKE_BUILD_PARALLEL_LEVEL).
    # Default = half the logical processors, to leave the host headroom. This is a
    # HARDWARE-STABILITY guard, not a speed knob: the i9-14900K host suffers Raptor
    # Lake Vmin-shift instability and bugchecks (0xA) under sustained all-core builds.
    # See CLAUDE.md Troubleshooting "Host instability". 0 = uncapped (old behaviour).
    [int]$BuildJobs = [Math]::Max(1, [int]([Environment]::ProcessorCount / 2)),
    [string]$VcvarsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$AsanRuntimeDir = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64',
    [string]$VswhereDir = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
)

$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path "$PSScriptRoot/..").Path
$startTime = Get-Date

# ---- COMMIT-HEADROOM PREFLIGHT (determinism guard) --------------------------------------------------
# SCAR (2026-07-25): the win-tidy build kept dying with `LLVM ERROR: out of memory` / a 0xC0000005
# access violation on a DIFFERENT file and a DIFFERENT check every run. It looked exactly like an
# upstream clang-tidy matcher bug and was "fixed" twice by disabling checks in .clang-tidy. It was
# NEITHER. Measured: a clang-tidy edge peaks at ~0.3 GB, and the very files that crashed under the
# build ran 0/5 crashes standalone -- the host was simply sitting at 83 GB of its 96 GB COMMIT LIMIT
# (Visual Studio alone held 8.7 GB, plus a DAW/browsers), so whichever process asked for memory when
# the headroom ran out was the one that died. That is the whole "non-deterministic AVX-512 crash".
#
# RULE: a random OOM must never again masquerade as a code failure. Measure the headroom, SAY SO, and
# clamp build parallelism to what actually fits. Budget ~1.5 GB per concurrent edge (a tidy edge runs
# clang-tidy AND cl.exe; the heavy hesap-direct/hesap-fft TUs are the spikes) + a 4 GB reserve for the
# test binaries and the rest of the desktop.
function Get-CrdCommitState
{
    $os = Get-CimInstance Win32_OperatingSystem
    [pscustomobject]@{
        LimitGb    = [math]::Round($os.TotalVirtualMemorySize / 1MB, 1)
        FreeGb     = [math]::Round($os.FreeVirtualMemory / 1MB, 1)
        TopHogs    = (Get-Process | Sort-Object PagedMemorySize64 -Descending | Select-Object -First 3 |
                      ForEach-Object { '{0} {1:N1}GB' -f $_.Name, ($_.PagedMemorySize64 / 1GB) }) -join ', '
    }
}

# Per-EDGE commit budget by config. MEASURED on this host 2026-07-25 (not estimated -- the numbers below are
# file-captured from an instrumented build):
#   * a clang-tidy edge peaks  ~0.20-0.30 GB;  a /Od cl.exe edge  ~0.36 GB;
#   * ⛔ an LTCG **link.exe** peaks **5.6 GB** -- LINKS, not compiles, are the win-shipping/release constraint;
#   * win-shipping at -j5 CRASHED cl.exe (0xC0000005, a different file each time) with ~14.5 GB free;
#   * win-shipping at -j3 SUCCEEDED but bottomed out at **0.26 GB free commit** -- 260 MB from the limit.
# So the boundary on this desktop sits between 3 and 5 concurrent shipping edges at ~13 GB free => ~4 GB/edge
# effective. Re-measured per preset at build time, because the desktop's own footprint moves (Visual Studio +
# three clangd instances alone held ~20 GB while this was being diagnosed).
$CrdPerEdgeGb = @{ 'win-debug' = 2.0; 'win-asan' = 2.5; 'win-shipping' = 4.0; 'win-release' = 4.0; 'win-tidy' = 1.5 }
$CrdReserveGb = 2.0

$commit = Get-CrdCommitState
Write-Host ("  Commit: {0} GB free of {1} GB limit (top: {2})" -f $commit.FreeGb, $commit.LimitGb, $commit.TopHogs) -ForegroundColor DarkCyan
if ($commit.FreeGb -lt ($CrdReserveGb * 2))
{
    Write-Host '  ! COMMIT HEADROOM VERY LOW - builds will be clamped hard. Closing Visual Studio / clangd / a DAW' -ForegroundColor Red
    Write-Host '    is the fastest way to make the sweep both fast AND crash-free.' -ForegroundColor Red
}

# -Parallel mode: build each requested config in its own background job, then
# collect outcomes. The 4 presets have independent `build/<preset>/` dirs so
# they don't fight each other on filesystem state; ASan PATH and ctest are
# per-job so env mutation is isolated. Wall-time win is biggest when win-
# shipping (LTO-link-bound, single-threaded) overlaps with the CPU-bound
# debug/asan/tidy builds.
if ($Parallel)
{
    Write-Host '====================================================================' -ForegroundColor Cyan
    Write-Host '  PER-SLICE VERIFICATION (parallel mode)                            ' -ForegroundColor Cyan
    Write-Host '====================================================================' -ForegroundColor Cyan
    Write-Host ''
    Write-Host '  WARNING: -Parallel stacks config-level build jobs on top of each' -ForegroundColor Yellow
    Write-Host '  config''s Ninja threads - the heaviest all-core load this script can' -ForegroundColor Yellow
    Write-Host '  produce. On the i9-14900K host this is the workload most likely to' -ForegroundColor Yellow
    Write-Host '  trip a Raptor Lake bugcheck (0xA). Prefer the sequential default.' -ForegroundColor Yellow
    Write-Host '  See CLAUDE.md Troubleshooting "Host instability". Per-job threads' -ForegroundColor Yellow
    Write-Host '  are clamped to -BuildJobs below.' -ForegroundColor Yellow
    Write-Host ''

    $presets = @(@{ name = 'win-debug'; runCTest = $true; asan = $false })
    if (-not $SkipAsan)     { $presets += @{ name = 'win-asan';     runCTest = $true;  asan = $true  } }
    if (-not $SkipShipping) { $presets += @{ name = 'win-shipping'; runCTest = $true;  asan = $false } }
    if ($IncludeRelease)    { $presets += @{ name = 'win-release';  runCTest = $true;  asan = $false } }
    if (-not $SkipTidy)     { $presets += @{ name = 'win-tidy';     runCTest = $false; asan = $false } }

    # Default ninja parallelism per job = (NumProc / NumJobs), min 1, so total
    # CPU pressure is bounded. Override via -ParallelJobs.
    $totalCpu = [Environment]::ProcessorCount
    $perJobJobs = if ($ParallelJobs -gt 0) { $ParallelJobs } else { [Math]::Max(1, [Math]::Floor($totalCpu / $presets.Count)) }
    # Hardware-stability clamp: never let a per-job thread count exceed -BuildJobs,
    # so the warning above is not load-bearing. -BuildJobs 0 disables the clamp.
    if ($BuildJobs -gt 0) { $perJobJobs = [Math]::Min($perJobJobs, $BuildJobs) }

    $logsDir = Join-Path $repoRoot 'scripts\.per-slice-logs'
    if (-not (Test-Path $logsDir)) { New-Item -ItemType Directory -Force $logsDir | Out-Null }

    $jobs = @()
    foreach ($p in $presets)
    {
        $cfgName       = $p.name
        $cfgRunCTest   = [bool]$p.runCTest
        $cfgIsAsan     = [bool]$p.asan
        $cfgReconfigure = $Reconfigure.IsPresent
        $cfgLogPath    = Join-Path $logsDir ("{0}.log" -f $cfgName)
        # Truncate the log up-front so a re-run doesn't show stale output.
        Set-Content -Path $cfgLogPath -Value '' -Encoding utf8

        $jobs += Start-Job -Name "perslice-$cfgName" -ScriptBlock {
            param($name, $runCTest, $isAsan, $doReconfig, $perJob,
                  $vcvarsPath, $vswhereDir, $asanRuntimeDir, $repoRoot, $logPath)

            # Compose the cmd line that sources vcvars then runs build/ctest
            # for THIS preset. Each parallel branch must source vcvars itself
            # — env from the parent process isn't inherited by cmd /c here.
            $configStep = if ($doReconfig) { "cmake --preset $name && " } else { '' }
            $asanPathStep = if ($isAsan) { "set ""PATH=$asanRuntimeDir;%PATH%"" && " } else { '' }
            $ctestStep = if ($runCTest) { "&& ${asanPathStep}ctest --preset $name --output-on-failure --parallel $perJob" } else { '' }

            $cmdLine =
                "set ""PATH=$vswhereDir;%PATH%"" && " +
                "call ""$vcvarsPath"" >nul && " +
                "cd /d ""$repoRoot"" && " +
                "${configStep}cmake --build --preset $name --parallel $perJob $ctestStep"

            cmd /c $cmdLine *> $logPath
            $ec = $LASTEXITCODE

            # Distinguish build-fail vs ctest-fail by re-scanning the log tail.
            $status = 'PASS'
            if ($ec -ne 0)
            {
                $tail = Get-Content -Path $logPath -Tail 200 -ErrorAction SilentlyContinue
                if ($tail -match 'tests failed|failed during compilation')
                {
                    $status = "CTEST-FAIL exit=$ec"
                } else {
                    $status = "BUILD-FAIL exit=$ec"
                }
            }
            elseif ($runCTest) { $status = 'PASS (build+ctest)' }
            else               { $status = 'PASS (build)' }

            [pscustomobject]@{ name = $name; status = $status; exit = $ec; log = $logPath }
        } -ArgumentList $cfgName, $cfgRunCTest, $cfgIsAsan, $cfgReconfigure, $perJobJobs,
                        $VcvarsPath, $VswhereDir, $AsanRuntimeDir, $repoRoot, $cfgLogPath
    }

    Write-Host ("Spawned {0} parallel jobs, {1} ninja threads each (CPUs={2})..." -f $presets.Count, $perJobJobs, $totalCpu)
    Write-Host '(Logs are streamed to scripts\.per-slice-logs\<preset>.log.)'

    $results = $jobs | Wait-Job | Receive-Job
    $jobs | Remove-Job -Force

    Write-Host ''
    Write-Host '----- PER-SLICE SUMMARY (parallel) -----' -ForegroundColor Cyan
    $failedCount = 0
    foreach ($r in $results)
    {
        if ($r.status -like 'PASS*')
        {
            Write-Host ("  {0,-18} {1}" -f $r.name, $r.status) -ForegroundColor Green
        } else {
            Write-Host ("  {0,-18} {1}    [log: {2}]" -f $r.name, $r.status, $r.log) -ForegroundColor Red
            $failedCount++
        }
    }

    $elapsed = (Get-Date) - $startTime
    $elapsedStr = '{0:mm}:{0:ss}' -f $elapsed
    Write-Host ''
    Write-Host '====================================================================' -ForegroundColor Cyan
    if ($failedCount -eq 0)
    {
        Write-Host ('  RESULT: PASS  (elapsed ' + $elapsedStr + ', parallel)') -ForegroundColor Green
    } else {
        Write-Host ('  RESULT: FAIL (' + $failedCount + ' config(s) failed)  (elapsed ' + $elapsedStr + ', parallel)') -ForegroundColor Red
    }
    Write-Host '====================================================================' -ForegroundColor Cyan
    exit $failedCount
}

# Build the preset list
$presetSpec = "@{ name='win-debug'; runCTest=`$true; asan=`$false }"
if (-not $SkipAsan)     { $presetSpec += ",`r`n@{ name='win-asan'; runCTest=`$true; asan=`$true }" }
if (-not $SkipShipping) { $presetSpec += ",`r`n@{ name='win-shipping'; runCTest=`$true; asan=`$false }" }
if ($IncludeRelease)    { $presetSpec += ",`r`n@{ name='win-release'; runCTest=`$true; asan=`$false }" }
if (-not $SkipTidy)     { $presetSpec += ",`r`n@{ name='win-tidy'; runCTest=`$false; asan=`$false }" }

$reconfigStr = if ($Reconfigure.IsPresent) { '$true' } else { '$false' }

Write-Host '====================================================================' -ForegroundColor Cyan
Write-Host '  PER-SLICE VERIFICATION                                            ' -ForegroundColor Cyan
Write-Host '====================================================================' -ForegroundColor Cyan
if ($BuildJobs -gt 0) {
    Write-Host ("  Ninja capped to {0} threads/build (CPUs={1}) - Raptor Lake stability guard." -f $BuildJobs, [Environment]::ProcessorCount) -ForegroundColor DarkCyan
} else {
    Write-Host '  Ninja UNCAPPED (-BuildJobs 0) - all cores. Host-instability risk on i9-14900K.' -ForegroundColor Yellow
}

# Inline script run under vcvars-sourced cmd so Ninja + cl.exe + clang-cl are on PATH.
# Done as a temp file to avoid nested-quoting nightmares (same pattern as full-sweep.ps1).
$tmpScript = Join-Path $repoRoot 'scripts\.per-slice-check-tmp.ps1'
$batShim   = Join-Path $repoRoot 'scripts\.per-slice-check-shim.bat'

$perEdgeSpec = ($CrdPerEdgeGb.GetEnumerator() | ForEach-Object { "'{0}' = {1}" -f $_.Key, $_.Value }) -join '; '

@"
`$presets = @(
$presetSpec
)
`$asanRuntimeDir = '$AsanRuntimeDir'
`$reconfigure = $reconfigStr
`$results = [ordered]@{}
`$perEdgeGb  = @{ $perEdgeSpec }
`$reserveGb  = $CrdReserveGb
`$buildJobsCap = $BuildJobs

foreach (`$p in `$presets) {
    Write-Host ''
    Write-Host "===== `$(`$p.name) =====" -ForegroundColor Yellow

    # COMMIT-HEADROOM CLAMP, re-measured per config. A build that outruns the host's commit limit does not
    # fail cleanly: clang-tidy dies with "LLVM ERROR: out of memory", or clang-tidy/cl.exe take a 0xC0000005
    # on a stack growth that cannot commit - on a DIFFERENT random file every run, which reads exactly like
    # an upstream toolchain bug and is not one. Size the job count to the headroom that actually exists.
    if (`$buildJobsCap -gt 0) {
        `$os   = Get-CimInstance Win32_OperatingSystem
        `$free = [math]::Round(`$os.FreeVirtualMemory / 1MB, 1)
        `$per  = if (`$perEdgeGb.ContainsKey(`$p.name)) { `$perEdgeGb[`$p.name] } else { 2.0 }
        `$safe = [Math]::Max(1, [int][Math]::Floor((`$free - `$reserveGb) / `$per))
        `$jobs = [Math]::Min(`$buildJobsCap, `$safe)
        `$env:CMAKE_BUILD_PARALLEL_LEVEL = "`$jobs"
        if (`$jobs -lt `$buildJobsCap) {
            Write-Host ("  ! commit headroom {0} GB @ {1} GB/edge -> clamping ninja {2} -> {3} (a random clang-tidy/cl crash under low headroom is NOT a code defect)" -f `$free, `$per, `$buildJobsCap, `$jobs) -ForegroundColor Yellow
        } else {
            Write-Host ("  commit headroom {0} GB @ {1} GB/edge -> ninja -j{2}" -f `$free, `$per, `$jobs) -ForegroundColor DarkCyan
        }
    }

    if (`$reconfigure) {
        Write-Host "[per-slice] cmake --preset `$(`$p.name)"
        & cmake --preset `$p.name
        if (`$LASTEXITCODE -ne 0) {
            `$results[`$p.name] = "CONFIGURE-FAIL exit=`$LASTEXITCODE"
            continue
        }
    }

    & cmake --build --preset `$p.name
    if (`$LASTEXITCODE -ne 0) {
        `$results[`$p.name] = "BUILD-FAIL exit=`$LASTEXITCODE"
        continue
    }

    if (`$p.runCTest) {
        if (`$p.asan) {
            # ASan DLL must be on PATH for ASan-instrumented binaries to start.
            `$env:PATH = "`$asanRuntimeDir;`$env:PATH"
        }
        & ctest --preset `$p.name --output-on-failure
        if (`$LASTEXITCODE -ne 0) {
            `$results[`$p.name] = "CTEST-FAIL exit=`$LASTEXITCODE"
            continue
        }
        `$results[`$p.name] = 'PASS (build+ctest)'
    } else {
        `$results[`$p.name] = 'PASS (build)'
    }
}

Write-Host ''
Write-Host '----- PER-SLICE SUMMARY -----' -ForegroundColor Cyan
`$failedCount = 0
foreach (`$k in `$results.Keys) {
    `$status = `$results[`$k]
    if (`$status -like 'PASS*') {
        Write-Host ("  {0,-18} {1}" -f `$k, `$status) -ForegroundColor Green
    } else {
        Write-Host ("  {0,-18} {1}" -f `$k, `$status) -ForegroundColor Red
        `$failedCount++
    }
}
exit `$failedCount
"@ | Out-File -FilePath $tmpScript -Encoding utf8

# CMAKE_BUILD_PARALLEL_LEVEL caps Ninja for every `cmake --build` in the inner
# script (which passes no explicit --parallel). Integer-only — no quoting hazard.
$buildJobsEnvLine = if ($BuildJobs -gt 0) { "set ""CMAKE_BUILD_PARALLEL_LEVEL=$BuildJobs""" } else { "rem CMAKE_BUILD_PARALLEL_LEVEL uncapped (-BuildJobs 0)" }

@"
@echo off
set "PATH=$VswhereDir;%PATH%"
$buildJobsEnvLine
call "$VcvarsPath" >nul
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "$tmpScript"
exit /b %errorlevel%
"@ | Out-File -FilePath $batShim -Encoding ascii

& cmd /c $batShim
$exitCode = $LASTEXITCODE

Remove-Item -Force -ErrorAction SilentlyContinue $tmpScript
Remove-Item -Force -ErrorAction SilentlyContinue $batShim

$elapsed = (Get-Date) - $startTime
$elapsedStr = '{0:mm}:{0:ss}' -f $elapsed
Write-Host ''
Write-Host '====================================================================' -ForegroundColor Cyan
if ($exitCode -eq 0) {
    Write-Host ('  RESULT: PASS  (elapsed ' + $elapsedStr + ')') -ForegroundColor Green
} else {
    Write-Host ('  RESULT: FAIL (' + $exitCode + ' config(s) failed)  (elapsed ' + $elapsedStr + ')') -ForegroundColor Red
}
Write-Host '====================================================================' -ForegroundColor Cyan

exit $exitCode
