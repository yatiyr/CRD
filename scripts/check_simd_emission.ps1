# check_simd_emission.ps1 — verifies the compiled obj file contains the SIMD
# instructions implied by CRD_SIMD_LEVEL_RESOLVED. Runs as a CTest test;
# guards against regressions where /arch:AVX2 silently stops being passed
# (e.g. CrdSimd.cmake refactor breaks the flag plumbing).
#
# Usage: check_simd_emission.ps1 -Obj <path> -Expect <avx2|sse2|neon|scalar> [-Ipo <0|1>]

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Obj,
    [Parameter(Mandatory)] [ValidateSet('avx2', 'sse2', 'neon', 'scalar')] [string] $Expect,
    [ValidateSet('0', '1')] [string] $Ipo = '0'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Obj -PathType Leaf))
{
    Write-Host "[check_simd_emission] FAIL: obj not found: $Obj"
    exit 2
}

if ($Expect -eq 'neon')
{
    Write-Host "[check_simd_emission] expect=neon - ARM disasm parity check not implemented; skipping"
    exit 77
}

$dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
if (-not $dumpbin)
{
    Write-Host "[check_simd_emission] FAIL: dumpbin not on PATH (run from a VS dev prompt or after vcvars64.bat)"
    exit 2
}

$global:LASTEXITCODE = 0
$savedPreference = $ErrorActionPreference
try
{
    # Windows PowerShell otherwise throws on redirected native stderr before exposing the decoder exit code.
    $ErrorActionPreference = 'Continue'
    $disasm = & $dumpbin.Source /disasm $Obj 2>&1
    $decoderExit = $LASTEXITCODE
}
finally
{
    $ErrorActionPreference = $savedPreference
}
if ($decoderExit -ne 0)
{
    Write-Host "[check_simd_emission] FAIL: dumpbin exited $decoderExit"
    $disasm | Write-Host
    exit 2
}
$instr_total = ($disasm | Select-String -Pattern '^[ \t]*[0-9A-F]{16}: [0-9A-F ]+\s+[a-z]' -AllMatches).Matches.Count
$ymm_total  = ($disasm | Select-String -Pattern '\bymm\d+\b' -AllMatches).Matches.Count
$ymm_fp     = ($disasm | Select-String -Pattern '\bv(add|sub|mul|div|sqrt|min|max)ps\s+ymm' -AllMatches).Matches.Count

Write-Host "[check_simd_emission] obj         : $Obj"
Write-Host "[check_simd_emission] expect      : $Expect"
Write-Host "[check_simd_emission] instr_total : $instr_total"
Write-Host "[check_simd_emission] ymm_total   : $ymm_total"
Write-Host "[check_simd_emission] ymm_fp_ops  : $ymm_fp"

# Only the configured target can establish IPO. Empty output alone is not evidence of IL-only code.
# Return CTest's explicit skip code for IPO; a non-IPO sibling still needs its own passing evidence.
if ($instr_total -lt 100)
{
    if ($Ipo -eq '1')
    {
        Write-Host "[check_simd_emission] SKIP - configured IPO object has insufficient native code; non-IPO proof required"
        exit 77
    }
    Write-Host "[check_simd_emission] FAIL: insufficient disassembly in a configured non-IPO object"
    exit 1
}

switch ($Expect)
{
    'avx2'
    {
        # Require any 256-bit reference (ymm), not specifically FP
        # arithmetic. GCC at -O0 emits AVX-encoded vaddps with the xmm
        # form (register allocator avoids ymm); still proves -mavx2 is
        # being passed because ymm refs appear in moves/broadcasts.
        # If the flag were silently dropped, ymm_total would be 0
        # (verified by the scalar preset).
        if ($ymm_total -eq 0)
        {
            Write-Host "[check_simd_emission] FAIL: expected AVX2 build but no ymm references found in obj"
            Write-Host "[check_simd_emission]       (likely /arch:AVX2 / -mavx2 is not being passed)"
            exit 1
        }
        Write-Host "[check_simd_emission] PASS - $ymm_total ymm references ($ymm_fp 256-bit FP ops) emitted"
        exit 0
    }
    'sse2'
    {
        if ($ymm_total -gt 0)
        {
            Write-Host "[check_simd_emission] FAIL: expected SSE2-only but ymm references found in obj"
            exit 1
        }
        Write-Host "[check_simd_emission] PASS - no ymm/AVX2 instructions emitted"
        exit 0
    }
    'scalar'
    {
        if ($ymm_total -gt 0)
        {
            Write-Host "[check_simd_emission] FAIL: expected scalar build but ymm references found in obj"
            exit 1
        }
        Write-Host "[check_simd_emission] PASS - no ymm/AVX2 instructions emitted"
        exit 0
    }
}
