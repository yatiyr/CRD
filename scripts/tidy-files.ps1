# tidy-files.ps1 -- run the CI-faithful LLVM-20 clang-tidy gate on SPECIFIC files (the ones a slice touched), so tidy
# violations are caught PER-SLICE instead of accumulating into a grind later. Uses the pinned gate compiler
# (C:\LLVM-20.1.8 -- NOT a stray LLVM), the repo .clang-tidy (auto-discovered; tests/ inherits it), and
# WarningsAsErrors semantics (any warning is a failure), matching CI. The win-tidy-local build runs the same check
# inline but with warnings NON-fatal; this script makes them fatal so a slice can't close dirty.
#
# Usage (from repo root) -- pass BOTH the .cpp TUs AND any new/edited .hpp headers (each is checked as its own TU;
# self-contained headers compile standalone):
#   powershell -File scripts/tidy-files.ps1 tests/hesap-autodiff/test_foo.cpp engine/hesap-autodiff/include/crd/hesap/autodiff/foo.hpp
# Exit code = number of files with issues (0 = clean).
#
# SCAR (2026-07-09, D-007 B0-1): this gate reported "clean" for files it had never PARSED. The `-I` set was a
# hand-maintained list that omitted whole modules (e.g. engine/kir/include), so `#include <crd/kir/ckir.hpp>` failed;
# "file not found" was then FILTERED OUT of the diagnostics, and a TU that fails to parse emits no check diagnostics at
# all -- so a blind file was indistinguishable from a clean one. `engine/kir-vulkan/src/backend_vulkan.cpp` passed the
# gate without a single line of it ever being analysed, and 89 real violations across crd-kir were invisible.
# Two root fixes, both here:
#   1. UNRESOLVED INCLUDES ARE A HARD FAILURE (never filtered into silence). A file we cannot parse is UNGATED, and an
#      ungated file is a DoD failure -- it must never read as green.
#   2. The include set is DERIVED, not hand-listed: `.cpp` files use the real compile database (exact per-TU flags);
#      headers get every `engine/*/include` dir globbed automatically, so a new module can never silently fall out.

param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)

# NOTE: default ErrorActionPreference (Continue) on purpose -- clang-tidy writes "N warnings generated." to stderr,
# and PS5.1 would treat that as a terminating NativeCommandError under 'Stop'.
$repo = Split-Path -Parent $PSScriptRoot
$tidy = 'C:\LLVM-20.1.8\bin\clang-tidy.exe'
if (-not (Test-Path $tidy)) { Write-Error "clang-tidy gate binary not found at $tidy (the gate is LLVM 20.1.8 - see docs/BUILDING.md)"; exit 99 }
if (-not $Files) { Write-Error 'Usage: tidy-files.ps1 <file.cpp> [<file2.cpp> ...]'; exit 99 }

$buildDir = "$repo\build\win-debug"
$hasDb    = Test-Path "$buildDir\compile_commands.json"

# `.cpp` files are driven from the real compile database so they get the exact per-TU flags (Windows SDK / Vulkan SDK
# include paths a hand-written list would never track). MSVC's precompiled header is the one thing clang cannot consume
# (`/Yu` + `/Fp` + the forced `/FI cmake_pch.hxx`), so mirror the DB into a scratch copy with just those flags stripped.
$dbDir = $buildDir
if ($hasDb) {
  $dbDir = Join-Path ([System.IO.Path]::GetTempPath()) 'crd-tidy-db'
  New-Item -ItemType Directory -Force -Path $dbDir | Out-Null
  $json = [System.IO.File]::ReadAllText("$buildDir\compile_commands.json")
  # CMake emits these unquoted and path-glued: /YuD:/.../cmake_pch.hxx  /FpD:/.../cmake_pch.cxx.pch  /FID:/.../cmake_pch.hxx
  $json = $json -replace '[-/]Yu[^\s"]*\s*', '' -replace '[-/]Fp[^\s"]*\s*', '' -replace '[-/]FI[^\s"]*cmake_pch[^\s"]*\s*', ''
  [System.IO.File]::WriteAllText((Join-Path $dbDir 'compile_commands.json'), $json)
}

# Header include set: GLOB every engine module's include dir, so adding a module never silently un-gates it.
$inc = @("-I$buildDir\engine\core\include",
         "-I$buildDir\_deps\catch2-src\src", "-I$buildDir\_deps\catch2-build\generated-includes")
$inc += (Get-ChildItem "$repo\engine" -Directory | ForEach-Object { "-I$($_.FullName)\include" } | Where-Object { Test-Path ($_ -replace '^-I','') })
if ($env:VULKAN_SDK) { $inc += "-I$env:VULKAN_SDK\Include" }

$dirty   = 0
$ungated = 0
$missing = 0
foreach ($f in $Files) {
  $path = if ([System.IO.Path]::IsPathRooted($f)) { $f } else { Join-Path $repo $f }
  # A file we were ASKED to gate but cannot find is a failure, not a skip. (A silent skip is how a mistyped path -- or
  # an arg-splatting bug -- turns a whole sweep into a false green; same disease as the UNGATED case below.)
  if (-not (Test-Path $path)) { Write-Host "MISSING      $f  <-- asked to gate a file that does not exist" -ForegroundColor Magenta; $missing++; continue }

  # No --header-filter: check ONLY the passed file as its own TU (default matches nothing but the main file), so no
  # transitive-header noise. Pass headers explicitly to get them checked.
  # .cpp -> drive from the compile database (exact flags incl. SDK paths). Headers have no TU in the DB -> synthesize.
  # A .cpp with no DB entry (a target not configured on this host, e.g. the Metal/HIP backends on Windows) ALSO falls
  # back to synthesized flags rather than clang-tidy's bare default -- otherwise it would report UNGATED forever.
  $isSource = $path -match '\.(cpp|cc|cxx)$'
  $inDb     = $hasDb -and $isSource -and (Select-String -Path "$buildDir\compile_commands.json" -SimpleMatch -Quiet -Pattern ((Resolve-Path $path).Path -replace '\\','/'))
  # SCAR (2026-07-25): clang-tidy DROPS `/`-spelled MSVC flags coming from the compile database, so `/EHsc`
  # and `/arch:AVX2` never reach the TU -- exceptions look disabled (any `try` is a hard error) and `__AVX2__`
  # is undefined, so AVX2-guarded code is preprocessed out and NEVER ANALYSED. `--extra-arg` is the one channel
  # clang-tidy honors; restate them here so this gate sees the configuration we actually ship. (Same fix as the
  # CRD_ENABLE_CLANG_TIDY block in the root CMakeLists -- keep the two in step.)
  if ($inDb) {
    $raw = & $tidy $path --warnings-as-errors="*" --quiet -p $dbDir `
        --extra-arg=/EHsc --extra-arg=/arch:AVX2 --extra-arg=-Wno-unused-command-line-argument 2>&1
  }
  else {
    $raw = & $tidy $path --warnings-as-errors="*" --quiet -- `
        -std=c++20 -xc++ -mavx2 -mfma -mf16c -DCRD_DETERMINISTIC_FP=1 -DCRD_SIMD_TARGET=2 $inc 2>&1
  }

  # (1) An unresolved include means the TU never parsed -> ZERO checks ran -> this file is UNGATED, not clean.
  $missing = $raw | Select-String "file not found"
  if ($missing) {
    Write-Host "UNGATED      $f  <-- includes did not resolve; NO checks ran (this is a DoD failure, not a pass)" -ForegroundColor Magenta
    $missing | Select-Object -First 5 | ForEach-Object { Write-Host "  $_" }
    $ungated++
    continue
  }

  $out = $raw | Select-String "warning:|error:"
  if ($out) { Write-Host "TIDY ISSUES  $f" -ForegroundColor Red; $out | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }; $dirty++ }
  else { Write-Host "clean        $f" -ForegroundColor Green }
}

if ($missing -gt 0) { Write-Host "`n$missing file(s) MISSING - check the paths you passed; a skipped file is not a clean one." -ForegroundColor Magenta }
if ($ungated -gt 0) { Write-Host "`n$ungated file(s) UNGATED - the gate could not parse them. Fix the include set; never treat this as clean." -ForegroundColor Magenta }
if ($dirty -gt 0) { Write-Host "`n$dirty file(s) with tidy issues - fix before closing the slice." -ForegroundColor Red }
if ($dirty -eq 0 -and $ungated -eq 0 -and $missing -eq 0) { Write-Host "`nAll $($Files.Count) file(s) gated and tidy-clean." -ForegroundColor Green }
exit ($dirty + $ungated + $missing)
