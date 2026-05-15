# check_no_untagged_physical_numeric.ps1 -- bans bare-f32/f64 for fields
# representing physical quantities. Per the Strategic Execution Plan
# 2026-05-15 + CLAUDE.md cornerstone (every physical/scientific quantity
# carries a unit via crd-units Quantity<D, T>).
#
# Scans engine/**/*.{hpp,cpp} except scoped-out dirs (crd-math/src/simd/
# and crd-rhi-vulkan/ where raw scalars are intentional).
#
# Flags struct/class field declarations matching:
#     <type> <field_name>;
# where type is bare f32/f64/float/double AND field_name matches a
# physical-quantity name (length / mass / time / force / velocity / etc.).
#
# Suppression: 'crd-lint-allow-untagged-physical' marker on the same line.
#
# Initial state: PASS (the guard ships in v0a-3; v0b/c adoption passes
# will repair existing offenders).

[CmdletBinding()]
param(
    [string] $RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($RepoRoot)) {
    $RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path
}

$engineDir = Join-Path $RepoRoot 'engine'
if (-not (Test-Path $engineDir)) {
    Write-Host "[check_no_untagged_physical_numeric] PASS - no engine directory yet"
    exit 0
}

# Scope-out directories where raw scalars are intentional (SIMD kernels,
# RHI raw-buffer upload).
$excludePatterns = @(
    '\\engine\\math\\src\\simd\\',
    '\\engine\\math\\include\\crd\\math\\simd\\',
    '\\engine\\rhi-vulkan\\'
)

# Physical-quantity field name patterns. Conservative initial list -- can
# tighten as adoption proceeds. Matches when the field name CONTAINS the
# token (case-insensitive) so `m_length` / `linear_velocity` / etc. all match.
$physicalNamePatterns = @(
    'length', 'distance', 'radius', 'diameter', 'width', 'height', 'depth',
    'mass', 'weight',
    'velocity', 'speed', 'acceleration',
    'force', 'torque', 'pressure',
    'energy', 'power',
    'temperature', 'duration',
    'voltage', 'current', 'resistance', 'capacitance', 'inductance',
    'frequency'
    # Note: 'angle', 'time', 'position' deliberately omitted from v0a-3
    # initial list -- too many false positives from non-physical uses
    # (timestamp / view-angle / cursor-position). Tighten when ready.
)
$nameRegex = '(' + ($physicalNamePatterns -join '|') + ')'

# Bare-scalar type tokens. Matches `f32 m_length;` and `float length_;` etc.
$typeRegex = '\b(f32|f64|float|double)\b'

# Field declaration pattern (heuristic; not a full C++ parser). Matches only
# lines that look like real struct/class field declarations:
#   - Start with optional whitespace then a bare scalar type
#   - Followed by a name containing a physical-quantity token
#   - Optional default-value initializer
#   - MUST end with `;` (function-parameter list members end with `,` or `)`)
#   - MUST NOT contain `(` or `)` on the same line (excludes single-line
#     function signatures + multi-line continuation parameters)
$fieldRegex = "^\s*$typeRegex\s+\w*$nameRegex\w*\s*(=\s*[^;()]*)?;\s*(//.*)?$"

$failures = @()
Get-ChildItem -Path $engineDir -Recurse -Include *.cpp, *.hpp -ErrorAction SilentlyContinue | ForEach-Object {
    $path = $_.FullName
    foreach ($p in $excludePatterns) {
        if ($path -match $p) { return }
    }
    $lineNo = 0
    foreach ($line in [System.IO.File]::ReadAllLines($path, [System.Text.UTF8Encoding]::new($false))) {
        $lineNo++
        if ($line -match 'crd-lint-allow-untagged-physical') { continue }
        if ($line -match $fieldRegex) {
            $failures += "  $($path):${lineNo}: $($line.Trim())"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "[check_no_untagged_physical_numeric] FAIL: $($failures.Count) field(s) with bare-scalar physical-quantity type:"
    $failures | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Write-Host "  Replace bare-f32/f64 fields with Quantity<D, T> from crd-units."
    Write-Host "  E.g.: `f32 length` -> `Quantity<dim::Length, f32> length` (or `Length<f32> length`)."
    Write-Host "  Per Strategic Execution Plan 2026-05-15: every physical/scientific quantity carries a unit."
    Write-Host "  Suppress with a 'crd-lint-allow-untagged-physical' marker on the same line if truly justified."
    exit 1
}

Write-Host "[check_no_untagged_physical_numeric] PASS - no bare-scalar physical-quantity fields"
exit 0
