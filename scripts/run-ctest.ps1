# Run scoped CTest with native argument forwarding (including regex |) and the MSVC runtime tools on Windows.
# Usage: ./scripts/run-ctest.ps1 -CtestArguments @('--test-dir','build/win-debug/tests/math','--timeout','180')
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$CtestArguments)
$ErrorActionPreference = 'Stop'
if ($env:OS -eq 'Windows_NT') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Get-Command dumpbin.exe -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $vswhere)) {
        $found = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe'
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        $dumpbin = @($found | Sort-Object -Descending)[0]
        if ($dumpbin) { $env:PATH = (Split-Path -Parent $dumpbin) + [IO.Path]::PathSeparator + $env:PATH }
    }
    $cmakeCtest = Join-Path $env:ProgramFiles 'CMake/bin/ctest.exe'
} else {
    $cmakeCtest = ''
}
if (-not $cmakeCtest -or -not (Test-Path -LiteralPath $cmakeCtest)) {
    $cmakeCtest = (Get-Command ctest -ErrorAction Stop).Source
}
$ErrorActionPreference = 'Continue' # Native stderr is diagnostic text; the native exit code is authoritative.
& $cmakeCtest @CtestArguments
exit $LASTEXITCODE
