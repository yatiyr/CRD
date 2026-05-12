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
