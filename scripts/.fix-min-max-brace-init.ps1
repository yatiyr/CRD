# Convert `std::min({a, b, c})` / `std::max({a, b, c})` brace-init to nested
# binary form `std::min(std::min(a, b), c)`. GCC rejects the brace-init form
# in template-deduction contexts even though the standard's
# `std::min(initializer_list)` overload exists — converting to nested form is
# portable across MSVC + clang + GCC.
#
# Usage: ./.fix-min-max-brace-init.ps1

$files = @(
    'D:\Dev\cerid\engine\geometry-spatial\src\uniform_grid.cpp',
    'D:\Dev\cerid\engine\geometry-spatial\src\spatial_hash.cpp',
    'D:\Dev\cerid\engine\geometry-spatial\src\loose_octree.cpp',
    'D:\Dev\cerid\engine\geometry-mesh-processing\src\remove_self_intersections.cpp',
    'D:\Dev\cerid\engine\geometry-mesh\src\mesh_bvh.cpp'
)

# Pattern matches std::min({A, B, C}) or std::max({A, B, C}) where A/B/C are
# simple identifiers/dot-expressions/whitespace.
$pattern = 'std::(min|max)\(\{\s*([^,}]+?),\s*([^,}]+?),\s*([^,}]+?)\s*\}\)'

foreach ($file in $files)
{
    $content = Get-Content -Raw -Path $file
    $original = $content
    $matches = [regex]::Matches($content, $pattern)
    if ($matches.Count -eq 0)
    {
        Write-Host "  (no matches)  $file"
        continue
    }
    # Replace via lambda — .NET regex MatchEvaluator.
    $rx = [regex]::new($pattern)
    $content = $rx.Replace($content, {
        param($m)
        $fn = $m.Groups[1].Value
        $a = $m.Groups[2].Value
        $b = $m.Groups[3].Value
        $c = $m.Groups[4].Value
        return "std::${fn}(std::${fn}($a, $b), $c)"
    })
    Set-Content -Path $file -Value $content -NoNewline -Encoding utf8
    Write-Host ("  {0} {1} substitution(s)" -f $matches.Count, $file)
}
