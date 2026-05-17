# Mechanical rename helper for clang-tidy GlobalConstantCase violations.
# Reads error names from the win-tidy log, builds k_snake_case -> kCamelCase
# mapping per file, applies via Get-Content / Set-Content with .NET regex
# \b word boundary (reliable on Windows, unlike msys sed).
#
# Usage: ./.tidy-rename-helper.ps1 -LogFile <path> [-DryRun]

[CmdletBinding()]
param(
    [string]$LogFile = 'D:\Dev\cerid\scripts\.win-tidy-fresh.log',
    [switch]$DryRun
)

function Convert-Name([string]$name)
{
    if ($name -match '^(k64|kS|k)_(.+)$')
    {
        $prefix = $matches[1]
        $rest = $matches[2]
        $parts = $rest -split '_'
        $cameled = ($parts | ForEach-Object {
            if ($_.Length -gt 0) { $_.Substring(0,1).ToUpper() + $_.Substring(1) }
        }) -join ''
        return $prefix + $cameled
    }
    return $name
}

# Parse error log: extract (file, name) pairs for naming errors. Cover both
# `global constant` and `local constexpr variable` categories — same rename
# rule applies (k_snake_case -> kCamelCase).
$lines = Get-Content $LogFile
$entries = $lines | Select-String -Pattern "^([A-Z]:\\[^:]+):\d+:\d+: error: invalid case style for (?:global constant|local constexpr variable) '([^']+)'" | ForEach-Object {
    [pscustomobject]@{
        File = $_.Matches[0].Groups[1].Value
        Name = $_.Matches[0].Groups[2].Value
    }
}

# Build per-file mapping (unique names per file).
$byFile = $entries | Group-Object -Property File
foreach ($g in $byFile)
{
    $file = $g.Name
    $uniqueNames = $g.Group.Name | Sort-Object -Unique
    Write-Host "----- $file -----"
    Write-Host "  $($uniqueNames.Count) distinct names to rename"

    $content = Get-Content -Raw -Path $file
    $originalContent = $content
    $totalSubs = 0

    foreach ($name in $uniqueNames)
    {
        $new = Convert-Name $name
        if ($new -eq $name)
        {
            Write-Warning "  No conversion for '$name' (regex didn't match prefix pattern)"
            continue
        }
        # \b word boundary on .NET regex — reliable on Windows.
        $pattern = '\b' + [regex]::Escape($name) + '\b'
        $matchCount = [regex]::Matches($content, $pattern).Count
        if ($matchCount -eq 0)
        {
            Write-Warning "  '$name' not found in file (already renamed?)"
            continue
        }
        $content = [regex]::Replace($content, $pattern, $new)
        Write-Host ("  {0,-30} -> {1,-30} ({2} sites)" -f $name, $new, $matchCount)
        $totalSubs += $matchCount
    }

    Write-Host "  Total substitutions: $totalSubs"

    if ($DryRun)
    {
        Write-Host "  [DryRun] not writing back"
    } else {
        if ($content -ne $originalContent)
        {
            Set-Content -Path $file -Value $content -NoNewline -Encoding utf8
            Write-Host "  Written"
        } else {
            Write-Host "  No changes (already clean)"
        }
    }
}
