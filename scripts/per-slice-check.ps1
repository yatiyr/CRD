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
#
# Exit code: 0 if every requested config passed, count of failures otherwise.

[CmdletBinding()]
param(
    [switch]$SkipShipping,
    [switch]$SkipTidy,
    [switch]$SkipAsan,
    [switch]$Reconfigure,
    [switch]$Parallel,
    [int]$ParallelJobs = 0,
    [string]$VcvarsPath = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    [string]$AsanRuntimeDir = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64',
    [string]$VswhereDir = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
)

$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path "$PSScriptRoot/..").Path
$startTime = Get-Date

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

    $presets = @(@{ name = 'win-debug'; runCTest = $true; asan = $false })
    if (-not $SkipAsan)     { $presets += @{ name = 'win-asan';     runCTest = $true;  asan = $true  } }
    if (-not $SkipShipping) { $presets += @{ name = 'win-shipping'; runCTest = $true;  asan = $false } }
    if (-not $SkipTidy)     { $presets += @{ name = 'win-tidy';     runCTest = $false; asan = $false } }

    # Default ninja parallelism per job = (NumProc / NumJobs), min 1, so total
    # CPU pressure is bounded. Override via -ParallelJobs.
    $totalCpu = [Environment]::ProcessorCount
    $perJobJobs = if ($ParallelJobs -gt 0) { $ParallelJobs } else { [Math]::Max(1, [Math]::Floor($totalCpu / $presets.Count)) }

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
if (-not $SkipTidy)     { $presetSpec += ",`r`n@{ name='win-tidy'; runCTest=`$false; asan=`$false }" }

$reconfigStr = if ($Reconfigure.IsPresent) { '$true' } else { '$false' }

Write-Host '====================================================================' -ForegroundColor Cyan
Write-Host '  PER-SLICE VERIFICATION                                            ' -ForegroundColor Cyan
Write-Host '====================================================================' -ForegroundColor Cyan

# Inline script run under vcvars-sourced cmd so Ninja + cl.exe + clang-cl are on PATH.
# Done as a temp file to avoid nested-quoting nightmares (same pattern as full-sweep.ps1).
$tmpScript = Join-Path $repoRoot 'scripts\.per-slice-check-tmp.ps1'
$batShim   = Join-Path $repoRoot 'scripts\.per-slice-check-shim.bat'

@"
`$presets = @(
$presetSpec
)
`$asanRuntimeDir = '$AsanRuntimeDir'
`$reconfigure = $reconfigStr
`$results = [ordered]@{}

foreach (`$p in `$presets) {
    Write-Host ''
    Write-Host "===== `$(`$p.name) =====" -ForegroundColor Yellow

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

@"
@echo off
set "PATH=$VswhereDir;%PATH%"
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
