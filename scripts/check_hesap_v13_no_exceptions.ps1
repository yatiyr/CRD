# check_hesap_v13_no_exceptions.ps1 -- v13 safety-critical conformance guard (Windows; mirror of the .sh).
# The v13 numerical-analysis + motion cluster (crd-hesap-{interp,quadrature,diff,motion}) is STATUS-NOT-EXCEPTION
# by contract (ADR-0095, moat pillar 3): every entry point returns a status enum ({value, status, ...}); errors
# NEVER escape as C++ exceptions. That is the DO-178C / ISO 26262 ASIL-D / MISRA-C++ no-exception-escape property
# the incumbents (Boost.Math throws) structurally lack. Guard: no throw/try/catch in the four module headers
# (trailing // comments stripped before matching). ASCII-only (PS 5.1 reads no-BOM scripts as the ANSI code page).
$ErrorActionPreference = "Stop"
$root = if ($args.Count -ge 1) { $args[0] } else { (Get-Location).Path }
$dirs = @(
    "engine/hesap-quadrature/include",
    "engine/hesap-interp/include",
    "engine/hesap-diff/include",
    "engine/hesap-motion/include")
$files = foreach ($d in $dirs) {
    Get-ChildItem -Path (Join-Path $root $d) -Recurse -Include *.hpp, *.cpp -ErrorAction SilentlyContinue
}
$hits = $files | Select-String -Pattern "\b(throw|try|catch)\b" | Where-Object {
    ($_.Line -replace "//.*", "") -match "\b(throw|try|catch)\b"
}
if ($hits) {
    Write-Host "FAIL: exception constructs in v13 hesap headers ($($hits.Count) sites) -- v13 is status-not-exception (ADR-0095 pillar 3)"
    $hits | Select-Object -First 30 | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
    exit 1
}
Write-Host "PASS: no throw/try/catch in v13 hesap headers (status-not-exception holds)."
exit 0
