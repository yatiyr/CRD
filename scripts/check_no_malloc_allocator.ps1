# check_no_malloc_allocator.ps1 — bans use of crd::memory::MallocAllocator outside
# the memory module's own definition + the allocator stress/unit tests that
# legitimately exercise it.
#
# Project rule (2026-05-27, user directive): NO MallocAllocator as a data/working
# allocator anywhere. Use a named pooled allocator (TlsfAllocator for bounded
# working sets, GrowableTlsfAllocator for unbounded ones). default_allocator()
# returns MallocAllocator and is likewise discouraged outside the memory module.
# See memory/project_no_malloc_sweep_before_v5 + feedback_no_malloc_no_stdvector_in_benches.
#
# Allowed:
#   - engine/memory/**        (defines MallocAllocator + default_allocator)
#   - tests/memory/test_memory.cpp, tests/stress/test_allocators_stress.cpp,
#     tests/stress/test_allocators_v5_stress.cpp  (test the allocators themselves)
#   - a 'crd-lint-allow-malloc-allocator' marker on the same line (justified exception)
#   - comment lines (doc mentions of the type name)

[CmdletBinding()]
param(
    [string] $RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path
)

$ErrorActionPreference = 'Stop'

$scopes = @(
    "$RepoRoot/engine",
    "$RepoRoot/tests",
    "$RepoRoot/runtime"
)

# Path fragments that are allowed to reference MallocAllocator.
$allowedPathFragments = @(
    '\engine\memory\',
    '\tests\memory\test_memory.cpp',
    '\tests\stress\test_allocators_stress.cpp',
    '\tests\stress\test_allocators_v5_stress.cpp'
)

$failures = @()
foreach ($scope in $scopes)
{
    if (-not (Test-Path $scope)) { continue }
    Get-ChildItem -Path $scope -Recurse -Include *.cpp, *.hpp, *.h -ErrorAction SilentlyContinue | ForEach-Object {
        $path = $_.FullName
        foreach ($frag in $allowedPathFragments) { if ($path -like "*$frag*") { return } }
        $matches = Select-String -Path $path -Pattern 'MallocAllocator' -AllMatches -ErrorAction SilentlyContinue
        foreach ($m in $matches)
        {
            $line = $m.Line
            $trimmed = $line.TrimStart()
            if ($trimmed.StartsWith('//') -or $trimmed.StartsWith('*')) { continue } # comment / doc line
            if ($line -match 'crd-lint-allow-malloc-allocator') { continue }
            $failures += "  $($m.Path):$($m.LineNumber): $($line.Trim())"
        }
    }
}

if ($failures.Count -gt 0)
{
    Write-Host "[check_no_malloc_allocator] FAIL: $($failures.Count) MallocAllocator reference(s) outside the allowed scopes:"
    $failures | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Write-Host "  Use a named pooled allocator instead: crd::memory::TlsfAllocator (bounded working set)"
    Write-Host "  or crd::memory::GrowableTlsfAllocator (unbounded). default_allocator() is also discouraged"
    Write-Host "  outside engine/memory. Justified exceptions: add a 'crd-lint-allow-malloc-allocator' marker."
    exit 1
}

Write-Host "[check_no_malloc_allocator] PASS - no MallocAllocator use outside engine/memory + allocator tests"
exit 0
