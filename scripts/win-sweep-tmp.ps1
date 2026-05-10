$presets = @('win-debug','win-relwithdebinfo','win-release','win-asan','win-clang-cl','win-debug-scalar')
$results = @()
foreach ($p in $presets) {
  Write-Host "===== $p ====="
  & cmake --build --preset $p
  if ($LASTEXITCODE -ne 0) {
    $results += "$p BUILD-FAIL exit=$LASTEXITCODE"
    Write-Host "BUILD-FAIL $p"
    continue
  }
  Write-Host "----- ctest $p -----"
  & ctest --preset $p --output-on-failure | Select-Object -Last 8
  $ec = $LASTEXITCODE
  $results += "$p ctest=$ec"
  Write-Host "CTEST-EXIT $p = $ec"
  Write-Host ''
}
Write-Host '===== win-tidy build-only ====='
& cmake --build --preset win-tidy
$results += "win-tidy build=$LASTEXITCODE"
Write-Host "WIN-TIDY-EXIT = $LASTEXITCODE"
Write-Host '===== win-shipping build-only ====='
& cmake --build --preset win-shipping
$results += "win-shipping build=$LASTEXITCODE"
Write-Host "WIN-SHIPPING-EXIT = $LASTEXITCODE"
Write-Host '===== SUMMARY ====='
$results | ForEach-Object { Write-Host $_ }
