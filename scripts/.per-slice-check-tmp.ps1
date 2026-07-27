$presets = @(
@{ name='win-debug'; runCTest=$true; asan=$false },
@{ name='win-asan'; runCTest=$true; asan=$true },
@{ name='win-shipping'; runCTest=$true; asan=$false },
@{ name='win-tidy'; runCTest=$false; asan=$false }
)
$asanRuntimeDir = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64'
$reconfigure = $false
$results = [ordered]@{}
$perEdgeGb  = @{ 'win-shipping' = 4; 'win-debug' = 2; 'win-tidy' = 1.5; 'win-asan' = 2.5; 'win-release' = 4 }
$reserveGb  = 2
$buildJobsCap = 16

foreach ($p in $presets) {
    Write-Host ''
    Write-Host "===== $($p.name) =====" -ForegroundColor Yellow

    # COMMIT-HEADROOM CLAMP, re-measured per config. A build that outruns the host's commit limit does not
    # fail cleanly: clang-tidy dies with "LLVM ERROR: out of memory", or clang-tidy/cl.exe take a 0xC0000005
    # on a stack growth that cannot commit - on a DIFFERENT random file every run, which reads exactly like
    # an upstream toolchain bug and is not one. Size the job count to the headroom that actually exists.
    if ($buildJobsCap -gt 0) {
        $os   = Get-CimInstance Win32_OperatingSystem
        $free = [math]::Round($os.FreeVirtualMemory / 1MB, 1)
        $per  = if ($perEdgeGb.ContainsKey($p.name)) { $perEdgeGb[$p.name] } else { 2.0 }
        $safe = [Math]::Max(1, [int][Math]::Floor(($free - $reserveGb) / $per))
        $jobs = [Math]::Min($buildJobsCap, $safe)
        $env:CMAKE_BUILD_PARALLEL_LEVEL = "$jobs"
        if ($jobs -lt $buildJobsCap) {
            Write-Host ("  ! commit headroom {0} GB @ {1} GB/edge -> clamping ninja {2} -> {3} (a random clang-tidy/cl crash under low headroom is NOT a code defect)" -f $free, $per, $buildJobsCap, $jobs) -ForegroundColor Yellow
        } else {
            Write-Host ("  commit headroom {0} GB @ {1} GB/edge -> ninja -j{2}" -f $free, $per, $jobs) -ForegroundColor DarkCyan
        }
    }

    if ($reconfigure) {
        Write-Host "[per-slice] cmake --preset $($p.name)"
        & cmake --preset $p.name
        if ($LASTEXITCODE -ne 0) {
            $results[$p.name] = "CONFIGURE-FAIL exit=$LASTEXITCODE"
            continue
        }
    }

    & cmake --build --preset $p.name
    if ($LASTEXITCODE -ne 0) {
        $results[$p.name] = "BUILD-FAIL exit=$LASTEXITCODE"
        continue
    }

    if ($p.runCTest) {
        if ($p.asan) {
            # ASan DLL must be on PATH for ASan-instrumented binaries to start.
            $env:PATH = "$asanRuntimeDir;$env:PATH"
        }
        & ctest --preset $p.name --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            $results[$p.name] = "CTEST-FAIL exit=$LASTEXITCODE"
            continue
        }
        $results[$p.name] = 'PASS (build+ctest)'
    } else {
        $results[$p.name] = 'PASS (build)'
    }
}

Write-Host ''
Write-Host '----- PER-SLICE SUMMARY -----' -ForegroundColor Cyan
$failedCount = 0
foreach ($k in $results.Keys) {
    $status = $results[$k]
    if ($status -like 'PASS*') {
        Write-Host ("  {0,-18} {1}" -f $k, $status) -ForegroundColor Green
    } else {
        Write-Host ("  {0,-18} {1}" -f $k, $status) -ForegroundColor Red
        $failedCount++
    }
}
exit $failedCount
