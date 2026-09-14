# check_no_std_containers.ps1 — bans OWNING STL containers everywhere in the repo
# (engine, tools, tests, sandbox), including test code and generated `.inc`
# reference-data headers.
#
# Contract: docs/CODING.md ("No owning STL containers. Use Cerid
# Array/String/HashMap; non-owning span/string_view/optional and standard
# algorithms are permitted"), docs/PRINCIPLES.md. Machine enforcement of a
# longstanding rule.
#
# BANNED (owning): std::string / basic_string / wstring / u*string, std::vector,
#   std::array, std::deque, std::forward_list, std::list, std::map / multimap /
#   unordered_map / unordered_multimap, std::set / multiset / unordered_set /
#   unordered_multiset, std::stack, std::queue, std::priority_queue, std::valarray.
# ALLOWED: std::span, std::string_view, std::optional, std::pair, std::tuple, <algorithm>.
#
# Replacements: std::vector -> crd::containers::Array; std::string ->
#   crd::containers::String; std::map/unordered_map -> crd::containers::HashMap;
#   std::set/unordered_set -> crd::containers::HashSet; std::array ->
#   crd::containers::FixedArray or a plain C array for local literal tables.
#
# A justified third-party-boundary exception may suppress one line with a
# 'crd-lint-allow-std-container' marker on that line.

[CmdletBinding()]
param(
    [string] $RepoRoot = ''
)

$ErrorActionPreference = 'Stop'

# Resolve RepoRoot in the body, never the param default: $PSScriptRoot is empty
# where the default is evaluated, so Resolve-Path "/.." silently becomes the drive
# root and every scope is skipped (defect recorded in the sibling guards).
if ([string]::IsNullOrEmpty($RepoRoot)) {
    $RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path
}

# Word boundary keeps std::string from matching std::string_view etc.
$banned = 'std::(string|basic_string|wstring|u8string|u16string|u32string|vector|array|deque|forward_list|list|multimap|unordered_map|unordered_multimap|map|multiset|unordered_set|unordered_multiset|set|stack|priority_queue|queue|valarray)\b'

$scopes = @(
    "$RepoRoot/engine",
    "$RepoRoot/tools",
    "$RepoRoot/tests",
    "$RepoRoot/sandbox"
)

$failures = @()
foreach ($scope in $scopes)
{
    if (-not (Test-Path $scope)) { continue }

    Get-ChildItem -Path $scope -Recurse -Include *.cpp, *.hpp, *.h, *.inc, *.cc, *.cxx -ErrorAction SilentlyContinue | ForEach-Object {
        $path = $_.FullName
        $lineNo = 0
        foreach ($raw in [System.IO.File]::ReadAllLines($path))
        {
            $lineNo++
            # Strip // line comments and single-line /* */ so prose never trips the guard.
            $code = [regex]::Replace($raw, '//.*$', '')
            $code = [regex]::Replace($code, '/\*[^*]*\*/', '')
            if ($code -match $banned)
            {
                if ($raw -match 'crd-lint-allow-std-container') { continue }
                $rel = $path.Substring($RepoRoot.Length).TrimStart('\','/')
                $failures += "  ${rel}:${lineNo}: $($raw.Trim())"
            }
        }
    }
}

if ($failures.Count -gt 0)
{
    Write-Host "[check_no_std_containers] FAIL: $($failures.Count) owning STL container use(s) found:"
    $failures | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Write-Host "  Use crd::containers::Array / String / HashMap / HashSet / FixedArray, or a plain C array for"
    Write-Host "  fixed local tables and generated reference data. std::span/string_view/optional and <algorithm> are allowed."
    Write-Host "  A genuine third-party boundary may mark one line 'crd-lint-allow-std-container'."
    exit 1
}

Write-Host "[check_no_std_containers] PASS - no owning STL containers in engine/tools/tests/sandbox"
exit 0
