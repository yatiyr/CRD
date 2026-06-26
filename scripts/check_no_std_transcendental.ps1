# check_no_std_transcendental.ps1 -- the Cerid Math Mandate guard (Windows; mirror of the .sh). Engine + tool code
# must use crd::math::* (include <crd/math/cmath.hpp>), never std:: transcendentals -- for cross-platform bit-
# determinism (the moat) + speed. Exempts engine/math (the kernel + its std:: edge fallbacks); tests/ and
# runtime/examples live outside engine/ and keep std:: deliberately (gold oracles / bench peers).
# ASCII-only on purpose: Windows PowerShell 5.1 reads no-BOM scripts as the ANSI code page, so non-ASCII breaks it.
$ErrorActionPreference = "Stop"
$root = if ($args.Count -ge 1) { $args[0] } else { (Get-Location).Path }
$fns = "sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh|exp|exp2|exp10|expm1|log|log2|log10|log1p|pow|cbrt|hypot"
$files = Get-ChildItem -Path (Join-Path $root "engine") -Recurse -Include *.hpp, *.cpp |
    Where-Object { $_.FullName -notmatch "[\\/]math[\\/]" }
$hits = $files | Select-String -Pattern "std::($fns)\b" -AllMatches
if ($hits) {
    Write-Host "FAIL: std:: transcendentals in engine code ($($hits.Count) sites) -- route to crd::math::*"
    $hits | Select-Object -First 30 | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber)" }
    exit 1
}
Write-Host "PASS: no std:: transcendentals in engine code (the Cerid Math Mandate holds)."
exit 0
