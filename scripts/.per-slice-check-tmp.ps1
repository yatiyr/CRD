$presets = @(
@{ name='win-debug'; runCTest=$true; asan=$false },
@{ name='win-asan'; runCTest=$true; asan=$true },
@{ name='win-shipping'; runCTest=$true; asan=$false },
@{ name='win-tidy'; runCTest=$false; asan=$false }
)
$asanRuntimeDir = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64'
$reconfigure = $true
$results = [ordered]@{}

foreach ($p in $presets) {
    Write-Host ''
    Write-Host "===== $($p.name) =====" -ForegroundColor Yellow

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
