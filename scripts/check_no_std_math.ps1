# check_no_std_math.ps1 — bans std::sin / std::cos / std::tan / std::atan2 /
# std::exp / std::log / std::pow / std::asin / std::acos / std::atan in the
# determinism-contract modules (ADR-0063 §2): they must use
# crd::math::deterministic::* substitutes for cross-platform bit-exact results.
# (std::sqrt is NOT banned — IEEE-754 mandates correctly-rounded single-rounding
# sqrt everywhere, so it is deterministic.)
#
# Scoped to engine/eylem, engine/hesap (ADR-0063), and engine/geometry-primitives
# (ADR-0076 §4 — crd-geometry inherits the determinism contract). Sub-modules in
# sibling directories (engine/eylem-rigid3d, engine/geometry-bvh, ...) are added
# here as they land.
#
# Run as a CTest test (registered in tests/math/CMakeLists.txt next to
# the SIMD-emission check) so any CI build catches the regression.

[CmdletBinding()]
param(
    [string] $RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path
)

$ErrorActionPreference = 'Stop'

$banned = @(
    'std::sinf?\b',
    'std::cosf?\b',
    'std::tanf?\b',
    'std::asinf?\b',
    'std::acosf?\b',
    'std::atanf?\b',
    'std::atan2f?\b',
    'std::expf?\b',
    'std::exp2f?\b',
    'std::logf?\b',
    'std::log2f?\b',
    'std::log10f?\b',
    'std::powf?\b',
    'std::fmodf?\b'
) -join '|'

$scopes = @(
    "$RepoRoot/engine/eylem",
    "$RepoRoot/engine/hesap",
    "$RepoRoot/engine/geometry-primitives",
    "$RepoRoot/engine/geometry-bvh",
    "$RepoRoot/engine/geometry-shader-helpers"
)

$failures = @()
foreach ($scope in $scopes)
{
    if (-not (Test-Path $scope)) { continue }

    Get-ChildItem -Path $scope -Recurse -Include *.cpp, *.hpp, *.h -ErrorAction SilentlyContinue | ForEach-Object {
        $matches = Select-String -Path $_.FullName -Pattern $banned -AllMatches -ErrorAction SilentlyContinue
        foreach ($m in $matches)
        {
            # Allow opt-out via comment marker on the same line.
            if ($m.Line -match 'crd-lint-allow-std-math') { continue }
            $failures += "  $($m.Path):$($m.LineNumber): $($m.Line.Trim())"
        }
    }
}

if ($failures.Count -gt 0)
{
    Write-Host "[check_no_std_math] FAIL: $($failures.Count) banned std::* math call(s) found in eylem/hesap source:"
    $failures | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Write-Host "  Use crd::math::deterministic::sin / cos / tan / atan2 / exp / log / pow etc. instead."
    Write-Host "  Justified exceptions can suppress with a 'crd-lint-allow-std-math' marker on the same line."
    exit 1
}

Write-Host "[check_no_std_math] PASS - no banned std::* math calls in engine/eylem or engine/hesap"
exit 0
