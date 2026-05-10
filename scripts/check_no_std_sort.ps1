# check_no_std_sort.ps1 — bans std::sort / std::stable_sort / std::nth_element
# / std::partial_sort / std::push_heap / std::pop_heap / std::make_heap /
# std::sort_heap in engine/eylem/** + engine/hesap/**.
#
# ADR-0063 §3: those modules must use crd::containers::* substitutes for
# cross-platform deterministic ordering. libc++ / libstdc++ / Microsoft CRT
# tie-break differently on equal keys, breaking the eylem v9b replay-hash
# CI matrix.
#
# Today (Phase 3.1 v0d) eylem and hesap don't exist yet, so this is a
# no-op. It lights up the moment eylem v1a or hesap v0a lands.

[CmdletBinding()]
param(
    [string] $RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path
)

$ErrorActionPreference = 'Stop'

$banned = @(
    'std::sort\b',
    'std::stable_sort\b',
    'std::nth_element\b',
    'std::partial_sort\b',
    'std::push_heap\b',
    'std::pop_heap\b',
    'std::make_heap\b',
    'std::sort_heap\b'
) -join '|'

$scopes = @(
    "$RepoRoot/engine/eylem",
    "$RepoRoot/engine/hesap"
)

$failures = @()
foreach ($scope in $scopes)
{
    if (-not (Test-Path $scope)) { continue }

    Get-ChildItem -Path $scope -Recurse -Include *.cpp, *.hpp, *.h -ErrorAction SilentlyContinue | ForEach-Object {
        $matches = Select-String -Path $_.FullName -Pattern $banned -AllMatches -ErrorAction SilentlyContinue
        foreach ($m in $matches)
        {
            if ($m.Line -match 'crd-lint-allow-std-sort') { continue }
            $failures += "  $($m.Path):$($m.LineNumber): $($m.Line.Trim())"
        }
    }
}

if ($failures.Count -gt 0)
{
    Write-Host "[check_no_std_sort] FAIL: $($failures.Count) banned std::* sort/heap call(s) found in eylem/hesap source:"
    $failures | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Write-Host "  Use crd::containers::sort / stable_sort / nth_element / push_heap / pop_heap / make_heap / sort_heap instead."
    Write-Host "  Justified exceptions can suppress with a 'crd-lint-allow-std-sort' marker on the same line."
    exit 1
}

Write-Host "[check_no_std_sort] PASS - no banned std::* sort/heap calls in engine/eylem or engine/hesap"
exit 0
