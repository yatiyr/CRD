# tidy-files.ps1 -- run the CI-faithful LLVM-20 clang-tidy gate on SPECIFIC files (the ones a slice touched), so tidy
# violations are caught PER-SLICE instead of accumulating into a grind later. Uses the pinned gate compiler
# (C:\LLVM-20.1.8 -- NOT a stray LLVM), the repo .clang-tidy (auto-discovered; tests/ inherits it), and
# WarningsAsErrors semantics (any warning is a failure), matching CI. The win-tidy-local build runs the same check
# inline but with warnings NON-fatal; this script makes them fatal so a slice can't close dirty.
#
# Usage (from repo root) -- pass BOTH the .cpp TUs AND any new/edited .hpp headers (each is checked as its own TU;
# self-contained headers compile standalone):
#   powershell -ExecutionPolicy Bypass -File scripts/tidy-files.ps1 tests/hesap-autodiff/test_foo.cpp engine/hesap-autodiff/include/crd/hesap/autodiff/foo.hpp
# Exit code = number of files with issues (0 = clean).

param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)

# NOTE: default ErrorActionPreference (Continue) on purpose -- clang-tidy writes "N warnings generated." to stderr,
# and PS5.1 would treat that as a terminating NativeCommandError under 'Stop'.
$repo = Split-Path -Parent $PSScriptRoot
$tidy = 'C:\LLVM-20.1.8\bin\clang-tidy.exe'
if (-not (Test-Path $tidy)) { Write-Error "clang-tidy gate binary not found at $tidy (the gate is LLVM 20.1.8 - see docs/BUILDING.md)"; exit 99 }
if (-not $Files) { Write-Error 'Usage: tidy-files.ps1 <file.cpp> [<file2.cpp> ...]'; exit 99 }

# The engine include set (mirrors the test targets) + the generated build_config.hpp under win-debug.
$inc = @(
  "-I$repo\engine\core\include", "-I$repo\build\win-debug\engine\core\include",
  "-I$repo\engine\containers\include", "-I$repo\engine\log\include", "-I$repo\engine\memory\include",
  "-I$repo\engine\vm\include", "-I$repo\engine\math\include", "-I$repo\engine\units\include",
  "-I$repo\engine\jobs\include", "-I$repo\engine\hesap\include",
  "-I$repo\engine\hesap-autodiff\include", "-I$repo\engine\hesap-tensor\include",
  "-I$repo\engine\hesap-dense\include", "-I$repo\engine\hesap-stats\include",
  "-I$repo\build\win-debug\_deps\catch2-src\src", "-I$repo\build\win-debug\_deps\catch2-build\generated-includes")

$dirty = 0
foreach ($f in $Files) {
  $path = if ([System.IO.Path]::IsPathRooted($f)) { $f } else { Join-Path $repo $f }
  if (-not (Test-Path $path)) { Write-Host "SKIP (not found): $f" -ForegroundColor Yellow; continue }
  # No --header-filter: check ONLY the passed file as its own TU (default matches nothing but the main file), so no
  # transitive-header noise. Pass headers explicitly to get them checked.
  $out = & $tidy $path --warnings-as-errors="*" --quiet -- `
      -std=c++20 -xc++ -DCRD_DETERMINISTIC_FP=1 -DCRD_SIMD_TARGET=2 $inc 2>&1 |
    Select-String "warning:|error:" | Where-Object { $_ -notmatch "file not found" }
  if ($out) { Write-Host "TIDY ISSUES  $f" -ForegroundColor Red; $out | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }; $dirty++ }
  else { Write-Host "clean        $f" -ForegroundColor Green }
}
if ($dirty -gt 0) { Write-Host "`n$dirty file(s) with tidy issues - fix before closing the slice." -ForegroundColor Red }
else { Write-Host "`nAll passed files are tidy-clean." -ForegroundColor Green }
exit $dirty
