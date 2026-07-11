# check_no_shader_language_leak.ps1 — enforces ADR-0103 invariant I1:
#   "No module outside a backend names a shading language."
#
# GLSL/HLSL/WGSL/MSL/CUDA text and the vendor compilers (shaderc/dxc) live ONLY inside a backend, between our emitter
# and the vendor compiler. The portable module `crd-shader` must not know any shading language; the OLD compile API
# (`crd::shader::compile_glsl` / `compile_hlsl`) is gone and must not reappear. This is the grep-gate that keeps the
# next hand-written `crd::shader::compile_glsl` from quietly re-inverting the source-of-truth (ADR-0101).
#
# SCOPE (honest): this gate covers I1 (no shading LANGUAGE outside a backend). I2 (no SPIR-V/DXIL bytecode in a public
# header) was closed structurally in D-008 C2-d4 by RETIRING the rhi `ShaderModule`/`ShaderModuleDesc::code` surface —
# consumers hold opaque `crd::gpu::IGpuProgram`s. This gate does not re-assert I2; the deleted surface is its proof.
#
# I1 FULLY CLOSED (D-008 C2-e): the allowlist is now EMPTY. The Effect/Module RENDERING frontend
# (`engine/shader/src/runtime.cpp`) no longer compiles GLSL — it takes an injected `crd::shader::ISpirvCompiler`
# (crd-shader-vulkan wraps `crd::gpu::compile_glsl_to_spirv`). No module outside `engine/gpu-context-vulkan` names a
# shading language or a vendor compiler. If a NEW leak is ever a genuine, tracked transition, add it here with a reason
# and a removal slice — never silently.
#
# Checks:
#   A. `crd::shader::compile_glsl` / `crd::shader::compile_hlsl` appear NOWHERE (the deleted API is not referenced).
#   B. engine/shader/** contains no shaderc/dxc identifier (crd-shader owns no language compiler) — except the allowlist.
#   C. `#include <shaderc/...>` / `<dxc/...>` appear ONLY under engine/gpu-context-vulkan/ (or the allowlist).

[CmdletBinding()]
param([string] $RepoRoot = '')

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrEmpty($RepoRoot)) { $RepoRoot = (Resolve-Path "$PSScriptRoot/..").Path }

# Files where a shaderc/dxc reference is a KNOWN, TRACKED transitional leak. EMPTY as of D-008 C2-e (I1 fully closed).
$transitional = @()

$roots = @('engine', 'tools', 'tests', 'sandbox') | ForEach-Object { Join-Path $RepoRoot $_ } | Where-Object { Test-Path $_ }
$srcExt = @('.cpp', '.hpp', '.h', '.cc', '.cxx')

$files = @()
foreach ($r in $roots) {
    $files += Get-ChildItem -Path $r -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $srcExt -contains $_.Extension }
}

$failures = @()
foreach ($f in $files) {
    $rel  = $f.FullName.Substring($RepoRoot.Length).TrimStart('\', '/').Replace('\', '/')
    $isVulkanBackend = $rel -like 'engine/gpu-context-vulkan/*'
    $isShaderModule  = $rel -like 'engine/shader/*'
    $isTransitional  = $transitional -contains $rel
    $lineNo = 0
    foreach ($line in [System.IO.File]::ReadAllLines($f.FullName, [System.Text.UTF8Encoding]::new($false))) {
        $lineNo++
        # A — the deleted portable compile API must not be referenced anywhere.
        if ($line -match 'crd::shader::compile_(glsl|hlsl)') {
            $failures += "  ${rel}:${lineNo}: references the removed crd::shader::compile_* (use crd::gpu::compile_*_to_spirv in a backend)"
        }
        # C — shaderc/dxc includes only in the Vulkan backend (or a tracked transitional site).
        if ($line -match '#\s*include\s*[<"](shaderc/|dxc/)') {
            if (-not $isVulkanBackend -and -not $isTransitional) {
                $failures += "  ${rel}:${lineNo}: shaderc/dxc include outside engine/gpu-context-vulkan (the language compiler's only home)"
            }
        }
        # B — crd-shader must name no shaderc/dxc symbol (except the tracked transitional Effect frontend).
        if ($isShaderModule -and -not $isTransitional -and ($line -match '\bshaderc_[a-z_]+' -or $line -match '\bDxcCreateInstance\b' -or $line -match '\bIDxcCompiler')) {
            $failures += "  ${rel}:${lineNo}: crd-shader must not name a shading-language compiler (moved to gpu-context-vulkan)"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "[check_no_shader_language_leak] FAIL (ADR-0103 I1): $($failures.Count) violation(s):"
    $failures | ForEach-Object { Write-Host $_ }
    Write-Host ""
    Write-Host "  A shading language (GLSL/HLSL) and its vendor compiler (shaderc/dxc) live ONLY inside a backend"
    Write-Host "  (engine/gpu-context-vulkan). crd-shader must not know any language; the graph→program seam is"
    Write-Host "  crd::gpu::IGpuContext / crd::gpu::compile_*_to_spirv. See ADR-0103 / docs/detours/D-008."
    exit 1
}

Write-Host "[check_no_shader_language_leak] PASS - I1 fully closed; no shading language outside a backend ($($files.Count) files scanned)"
if ($transitional.Count -gt 0) { Write-Host "  Tracked transitional leak(s): $($transitional -join ', ')." }
exit 0
