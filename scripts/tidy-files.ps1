# tidy-files.ps1 -- Windows entry to the portable strict LLVM-20 gate for SPECIFIC files (the ones a slice touched).
# The gate itself is scripts/tidy-files.py (scripts/cerid_dev/tidy.py) and runs the same way on every host:
#   * clang-tidy must report LLVM 20 (the pinned C:\LLVM-20.1.8 install, CRD_CLANG_TIDY, or clang-tidy-20/clang-tidy
#     on PATH); any other version is UNAVAILABLE, never a substitute;
#   * every .cpp is driven from the configured build's real compile_commands.json with only the PCH inputs stripped,
#     and the MSVC flags clang-tidy drops (/EHsc, the CMake-owned /arch value) restated through --extra-arg;
#   * every header is analysed as the main file of a translation unit of its owning target or module;
#   * a file that cannot be parsed, a tool exit without diagnostics, a missing file and an unavailable tool are
#     reported as UNGATED/unavailable -- never clean. (SCAR 2026-07-09: a gate that cannot parse a file must FAIL.)
#
# Usage (from repo root) -- pass BOTH the .cpp TUs AND any new/edited .hpp headers:
#   powershell -File scripts/tidy-files.ps1 tests/numerics/hesap-autodiff/test_foo.cpp engine/numerics/hesap-autodiff/include/crd/hesap/autodiff/foo.hpp
# Options: -BuildDir <configured build> (default build\win-debug), -ExportFixesDirectory <dir>.
# Exit code = number of files not gated clean (0 = clean); 99 = clang-tidy 20 or the compile database is unavailable.

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Files,
    [string]$ExportFixesDirectory,
    [string]$BuildDir
)

$repo = Split-Path -Parent $PSScriptRoot
$arguments = @()
if ($BuildDir) { $arguments += @('--build', $BuildDir) }
if ($ExportFixesDirectory) { $arguments += @('--export-fixes-directory', $ExportFixesDirectory) }
& python (Join-Path $repo 'scripts\tidy-files.py') @arguments -- @Files
exit $LASTEXITCODE
