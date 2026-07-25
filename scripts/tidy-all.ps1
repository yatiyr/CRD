# tidy-all.ps1 -- the WHOLE-TREE clang-tidy gate, as a LINT PASS rather than a build.
#
# WHY THIS EXISTS (2026-07-25, user-directed):
#   clang-tidy checks SOURCE. Its result depends on the source file, the compile flags and .clang-tidy -- and on
#   NOTHING ELSE. It does not depend on the build configuration: /Od vs /O2, ASan on or off, LTCG on or off cannot
#   change a lint result. The old `win-tidy` preset ran clang-tidy as part of a FULL COMPILING BUILD
#   (`CMAKE_CXX_CLANG_TIDY`), so every one of ~1450 TUs paid for a cl.exe compile just to re-learn what the other
#   four configs already compile. That is pure waste, and it was ALSO the only place clang-tidy ever crashed:
#   measured 0 crashes in 190 standalone invocations vs 2 in ~26 concurrent BUILD edges. Removing the build removes
#   the crash class along with the waste.
#
# ⛔ THE ONE VIRTUE OF THE OLD BUILD WE MUST NOT LOSE: a build cannot FORGET a file. So this script does not take a
# hand-written list -- it enumerates every translation unit from the build's own `compile_commands.json` and reports
# the COVERAGE COUNT. A file that is not linted must be impossible to mistake for a clean one; that is the
# `tidy_gate_clean_on_unparsed_files` scar, and it is why CRASHED and UNPARSED TUs are reported SEPARATELY from
# findings and fail the run. A crash is an INFRASTRUCTURE failure, never a pass.
#
# Usage (from repo root):
#   powershell -File scripts/tidy-all.ps1                 # lint every TU in the win-debug compile DB
#   powershell -File scripts/tidy-all.ps1 -Jobs 4         # cap parallelism
#   powershell -File scripts/tidy-all.ps1 -Filter engine  # only TUs whose path matches
# Exit code: 0 = every TU linted clean; otherwise (findings + crashed + unparsed).

[CmdletBinding()]
param(
    [int]$Jobs = 0,                       # 0 => derive from commit headroom (see below)
    [string]$Filter = '',
    [string]$BuildDir = '',
    [string]$TidyExe = 'C:\LLVM-20.1.8\bin\clang-tidy.exe'
)

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build\win-debug' }
$db = Join-Path $BuildDir 'compile_commands.json'

if (-not (Test-Path $TidyExe)) { Write-Error "clang-tidy gate binary not found at $TidyExe (the gate is LLVM 20.1.8 -- docs/BUILDING.md)"; exit 99 }
if (-not (Test-Path $db))      { Write-Error "no compile_commands.json at $db -- configure the build first"; exit 99 }

# MSVC's PCH is the one thing clang cannot consume (/Yu + /Fp + the forced /FI cmake_pch.hxx). Mirror the DB with
# just those stripped -- the same treatment scripts/tidy-files.ps1 applies.
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) 'crd-tidy-all-db'
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$json = [System.IO.File]::ReadAllText($db)
$json = $json -replace '[-/]Yu[^\s"]*\s*', '' -replace '[-/]Fp[^\s"]*\s*', '' -replace '[-/]FI[^\s"]*cmake_pch[^\s"]*\s*', ''
[System.IO.File]::WriteAllText((Join-Path $scratch 'compile_commands.json'), $json)

$entries = (ConvertFrom-Json $json) | ForEach-Object { $_.file } | Sort-Object -Unique
# VENDORED THIRD PARTY IS NOT OURS TO STYLE. CPM drops zstd / Catch2 / glfw / imgui sources under build/_deps and
# they appear in the compile DB, but our .clang-tidy encodes CERID's conventions -- holding someone else's library
# to them produces noise we can never act on (measured: the only 2 files with findings in a clean whole-tree run
# were zstd's dictBuilder and a Catch2 reporter). The old win-tidy build excluded them implicitly; do it
# explicitly, and count them as EXCLUDED rather than passed so the coverage number stays honest.
$vendored = @($entries | Where-Object { ($_ -replace '\\','/') -match '/_deps/' }).Count
$entries  = $entries | Where-Object { ($_ -replace '\\','/') -notmatch '/_deps/' }
if ($Filter) { $entries = $entries | Where-Object { $_ -replace '\\','/' -match $Filter } }
$total = $entries.Count
if ($total -eq 0) { Write-Error "no translation units matched"; exit 99 }

# Parallelism: clang-tidy peaks ~0.20-0.30 GB per invocation (measured). Without cl.exe alongside it this is cheap,
# but still size it to real commit headroom rather than assuming (see docs/SANITY.md 2026-07-25).
if ($Jobs -le 0)
{
    $free = [math]::Round((Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory / 1MB, 1)
    $Jobs = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount / 2, [int][Math]::Floor(($free - 2.0) / 0.5)))
}

Write-Host "====================================================================" -ForegroundColor Cyan
Write-Host ("  clang-tidy LINT PASS -- {0} translation units, -j{1} ({2} vendored _deps TUs excluded)" -f $total, $Jobs, $vendored) -ForegroundColor Cyan
Write-Host ("  db: {0}" -f $db) -ForegroundColor DarkCyan
Write-Host "====================================================================" -ForegroundColor Cyan

# --extra-arg: clang-tidy DROPS every `/`-spelled MSVC flag arriving via the compile command (measured 2026-07-25),
# so /EHsc and /arch:AVX2 never reach the TU -- exceptions look disabled and __AVX2__ is undefined, i.e. the gate
# would analyse a configuration we do not ship. Restate them through the one channel clang-tidy honours.
$extra = @('--extra-arg=/EHsc', '--extra-arg=/arch:AVX2', '--extra-arg=-Wno-unused-command-line-argument')

# A throttled PROCESS POOL. (Windows PowerShell 5.1 has no `ForEach-Object -Parallel`; Start-Process + a bounded
# in-flight list is the portable form and it also lets each TU's output land in its own file.)
$sw      = [Diagnostics.Stopwatch]::StartNew()
$outDir  = Join-Path $scratch 'out'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$results = New-Object System.Collections.ArrayList
$inflight = New-Object System.Collections.ArrayList
$idx = 0
$done = 0

function Complete-CrdTidy($slot)
{
    # `HasExited` can be true while the redirect handles are still held; WaitForExit() returns immediately for an
    # exited process AND guarantees the async output streams are flushed and closed. Without it the reads below
    # race and throw IOException ("used by another process") on a fraction of TUs.
    $slot.Proc.WaitForExit()
    $txt = ''
    foreach ($try in 1..20)
    {
        try
        {
            $txt = ''
            if (Test-Path $slot.Err) { $txt += [System.IO.File]::ReadAllText($slot.Err) }
            if (Test-Path $slot.Out) { $txt += [System.IO.File]::ReadAllText($slot.Out) }
            break
        }
        catch { Start-Sleep -Milliseconds 25 }  # the handle is still closing; retry briefly
    }
    Remove-Item $slot.Err, $slot.Out -ErrorAction SilentlyContinue
    $crashed  = $txt -match 'PLEASE submit a bug report|Access violation|LLVM ERROR'
    $unparsed = $txt -match 'file not found'
    $n        = [regex]::Matches($txt, ':\s(warning|error):\s').Count
    [pscustomobject]@{
        File = $slot.File; Crashed = $crashed; Unparsed = $unparsed
        Findings = $(if ($crashed -or $unparsed) { 0 } else { $n })
        Text     = $(if ($crashed -or $unparsed -or $n -gt 0) { $txt } else { '' })
    }
}

while ($idx -lt $total -or $inflight.Count -gt 0)
{
    while ($inflight.Count -lt $Jobs -and $idx -lt $total)
    {
        $f  = $entries[$idx]
        $so = Join-Path $outDir ("$idx.out"); $se = Join-Path $outDir ("$idx.err")
        $p  = Start-Process -FilePath $TidyExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $so -RedirectStandardError $se `
                            -ArgumentList (@($f, '--quiet', '--warnings-as-errors=*', '-p', $scratch) + $extra)
        [void]$inflight.Add([pscustomobject]@{ Proc = $p; File = $f; Out = $so; Err = $se })
        $idx++
    }
    Start-Sleep -Milliseconds 40
    for ($i = $inflight.Count - 1; $i -ge 0; $i--)
    {
        if ($inflight[$i].Proc.HasExited)
        {
            [void]$results.Add((Complete-CrdTidy $inflight[$i]))
            $inflight.RemoveAt($i)
            $done++
            if (($done % 100) -eq 0) { Write-Host ("  ... {0}/{1}" -f $done, $total) -ForegroundColor DarkGray }
        }
    }
}
$sw.Stop()

$crashed  = @($results | Where-Object { $_.Crashed })
$unparsed = @($results | Where-Object { $_.Unparsed })
$dirty    = @($results | Where-Object { -not $_.Crashed -and -not $_.Unparsed -and $_.Findings -gt 0 })

foreach ($r in $dirty)    { Write-Host ("TIDY ISSUES  " + $r.File) -ForegroundColor Red;     ($r.Text -split "`n" | Select-String ': (warning|error): ' | Select-Object -First 8) | ForEach-Object { Write-Host "  $_" } }
foreach ($r in $unparsed) { Write-Host ("UNGATED      " + $r.File + "  <-- includes did not resolve; NO checks ran") -ForegroundColor Magenta }
foreach ($r in $crashed)  { Write-Host ("CRASHED      " + $r.File + "  <-- clang-tidy died; this TU is UNGATED, not clean") -ForegroundColor Magenta }

Write-Host ''
Write-Host ("  COVERAGE: {0}/{1} translation units linted in {2:N1}s" -f ($total - $crashed.Count - $unparsed.Count), $total, $sw.Elapsed.TotalSeconds) -ForegroundColor DarkCyan
$fail = $dirty.Count + $crashed.Count + $unparsed.Count
if ($fail -eq 0) { Write-Host ("  RESULT: CLEAN -- all {0} TUs gated." -f $total) -ForegroundColor Green }
else             { Write-Host ("  RESULT: FAIL -- {0} with findings, {1} crashed, {2} unparsed." -f $dirty.Count, $crashed.Count, $unparsed.Count) -ForegroundColor Red }
exit $fail
