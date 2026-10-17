param([string]$Root = ".")
# crd-ceir invariants (ADR-0109 sec 4.3). I3: no shading-language/bytecode name or forbidden-module include crosses
# into crd-ceir (it holds KernelRef identities, never kernels/backends). I5: the crd-ceir link edge set is exactly the
# five host-only substrate modules (+ crd-warnings). The GPU/jobs stacks are reached via the dependency-inversion
# bridges (crd-ceir-host / crd-ceir-gpu), NEVER a link edge into the core.
# ASCII-ONLY on purpose: Windows PowerShell 5.1 mangles UTF-8 in string literals (see docs/BUILDING.md Platform notes).
$ErrorActionPreference = "Stop"
$ceir = Join-Path $Root "engine/execution/ceir"
$violations = @()

# ---- I5: link edges ----
$allowed = @("crd-core", "crd-log", "crd-memory", "crd-containers", "crd-units", "crd-warnings", "crd-ceir")
$cml = Join-Path $ceir "CMakeLists.txt"
$lineNo = 0
foreach ($line in (Get-Content $cml)) {
    $lineNo++
    $code = ($line -replace '#.*$', '')   # strip comments (they name the FORBIDDEN modules deliberately)
    foreach ($m in [regex]::Matches($code, 'crd-[A-Za-z0-9_-]+')) {
        if ($allowed -notcontains $m.Value) {
            $violations += ("I5 crd-ceir/CMakeLists.txt:{0} links forbidden target '{1}' (host-only: core/log/memory/containers/units)" -f $lineNo, $m.Value)
        }
    }
}

# ---- I3: (a) forbidden-module includes  (b) NO shading-language/bytecode name ----
# crd-ceir holds KernelRef identities, never kernels/backends, so it must not include a backend module NOR name a
# shading language, its bytecode, or its vendor compiler anywhere (code OR comment: the module stays backend-agnostic
# in spirit, so explanatory prose names "the kernel backend", never a specific one). This is the name-half of I3 that
# the engine-wide crd-no-shader-language-leak gate does NOT cover (that gate checks only the deleted compile API and
# shaderc/dxc includes).
$forbidden  = 'crd/gpu/|crd/kir|crd/rendergraph|crd/renderpass|crd/renderprogram|crd/rendermaterial|crd/renderasset|crd/resources|crd/framecook|crd/scenerender|crd/jobs/|crd/math/|crd/shader|crd/rhi'
$langTokens = '\b(GLSL|HLSL|WGSL|MSL|SPIR-?V|SPIRV|DXIL|DXBC|PTX|NVRTC|glslang|shaderc|dxc|CUDA)\b'
$dirs = @((Join-Path $ceir "include"), (Join-Path $ceir "src")) | Where-Object { Test-Path $_ }
foreach ($f in (Get-ChildItem -Path $dirs -Recurse -Include *.hpp, *.cpp -File)) {
    $n = 0
    foreach ($line in (Get-Content $f.FullName)) {
        $n++
        if ($line -match '#\s*include' -and $line -match $forbidden) {
            $violations += ("I3 {0}:{1} includes a forbidden module: {2}" -f $f.Name, $n, $line.Trim())
        }
        if ($line -cmatch $langTokens) {
            $violations += ("I3 {0}:{1} names a shading language/bytecode/compiler '{2}' (crd-ceir is backend-agnostic): {3}" -f $f.Name, $n, $Matches[1], $line.Trim())
        }
        # ---- I6 (CEIR-1d, §7): open-world dispatch — the core must NEVER switch on an op's kind. Dispatch via traits
        # (has_trait) or interfaces (get_interface). This catches `switch (op.kind())` / `switch (x->kind())` (the
        # method form); a `switch` over a CLOSED value enum member like `attr.kind`/`v.kind` (no parens) is fine.
        if ($line -match 'switch\s*\(.*\bkind\s*\(\s*\)') {
            $violations += ("I6 {0}:{1} switches on an op KIND - dispatch via traits/interfaces, never switch(op.kind()): {2}" -f $f.Name, $n, $line.Trim())
        }
    }
}

# ---- U-116 (external-plugin proof, CEIR-9h): the central OPEN-WORLD enums must NOT grow to accommodate a plugin/domain.
# TypeKind/AttrKind's LAST enumerator is the `Extern` door - a new type/attr rides Extern + registration, NEVER a new
# enum value (the whole 8a/8b thesis). A future slice widening one trips this forever. (EffectFamily is already pinned at
# compile time by its kLastEffectFamily static_assert; EvalDomain/RealtimeClass likewise.)
function Get-EnumLastMember([string]$file, [string]$enum) {
    $inEnum = $false; $last = ""
    foreach ($line in (Get-Content $file)) {
        if ($line -match ("enum class {0}\b" -f $enum)) { $inEnum = $true; continue }
        if (-not $inEnum) { continue }
        if ($line -match '^\s*\}') { break }
        $code = ($line -replace '//.*$', '').Trim()
        if ($code -match '^([A-Za-z_][A-Za-z0-9_]*)') { $last = $Matches[1] }
    }
    return $last
}
foreach ($pin in @(, @("include/crd/ceir/type.hpp", "TypeKind", "Extern")) + @(, @("include/crd/ceir/attr.hpp", "AttrKind", "Extern"))) {
    $last = Get-EnumLastMember (Join-Path $ceir $pin[0]) $pin[1]
    if ($last -ne $pin[2]) {
        $violations += ("U-116 {0} last enumerator is '{1}', expected '{2}' - a central enum grew; a plugin/domain must use the Extern door + registration, not a new enum value" -f $pin[1], $last, $pin[2])
    }
}

# ---- I7 (DIAG.11b): func::func_body_block is the lookup that may miss; every caller tests its result against nullptr
# (on the call's line, or the assigned variable within the next six lines). A caller holding a func.func that has a body
# takes func::func_body, fatal in every build. An unchecked lookup is a null path the optimizer can inline into the
# dereference, and GCC's link-time -Wnull-dereference then fails an unrelated edit's link (test_hello.cpp, 2026-10-17).
$i7Dirs = @("engine", "tests", "tools", "runtime", "sandbox") | ForEach-Object { Join-Path $Root $_ } |
    Where-Object { Test-Path $_ }
$i7Own = 'engine[\\/]execution[\\/]ceir[\\/](include[\\/]crd[\\/]ceir[\\/]func\.hpp|src[\\/]func\.cpp)$'
$i7Files = Get-ChildItem -Path $i7Dirs -Recurse -Include *.cpp, *.hpp, *.h, *.inl -File |
    Select-String -Pattern 'func_body_block\s*\(' -List | ForEach-Object { $_.Path } |
    Where-Object { $_ -notmatch $i7Own }
foreach ($path in $i7Files) {
    $lines = @(Get-Content $path)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $code = $lines[$i] -replace '//.*$', ''
        if ($code -notmatch 'func_body_block\s*\(') { continue }
        if ($code -match 'func_body_block\s*\([^;]*\)\s*[!=]=\s*nullptr') { continue }
        $var = ""
        if ($code -match '([A-Za-z_][A-Za-z0-9_]*)\s*=\s*[^=\s][^;]*func_body_block') { $var = $Matches[1] }
        $ok = $false
        if ($var -ne "") {
            $last = [Math]::Min($i + 6, $lines.Count - 1)
            for ($j = $i; $j -le $last; $j++) {
                $c = $lines[$j] -replace '//.*$', ''
                if ($c -match ('(^|[^A-Za-z0-9_])' + $var + '\s*[!=]=\s*nullptr') -or
                    $c -match ('nullptr\s*[!=]=\s*' + $var + '([^A-Za-z0-9_]|$)')) { $ok = $true; break }
            }
        }
        if (-not $ok) {
            $violations += ("I7 an unchecked func::func_body_block (test it against nullptr, or take " +
                "func::func_body): {0}:{1}: {2}" -f $path, ($i + 1), $lines[$i].Trim())
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "crd-ceir invariant violations (ADR-0109 I3/I5 + open-world I6 + U-116 enum-pins + I7 body lookups):"
    $violations | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "crd-ceir invariants OK (I3: no shader-lang name/forbidden include; I5: host-only links; I6: no switch on op.kind; U-116: TypeKind/AttrKind end at Extern; I7: every func_body_block result tested)."
exit 0
