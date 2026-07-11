#!/usr/bin/env bash
# check_no_shader_language_leak.sh — Linux mirror of check_no_shader_language_leak.ps1 (ADR-0103 invariant I1).
# A shading language (GLSL/HLSL) and its vendor compiler (shaderc/dxc) live ONLY inside a backend
# (engine/gpu-context-vulkan). crd-shader must know no language; the deleted crd::shader::compile_* must not reappear.
# Scope: I1 (language) only; I2 (rhi bytecode surface) was closed structurally in D-008 C2-d4 by retiring the rhi
# ShaderModule surface, and is not re-asserted here.
# I1 FULLY CLOSED (D-008 C2-e): the allowlist is EMPTY — the Effect frontend (engine/shader/src/runtime.cpp) takes an
# injected crd::shader::ISpirvCompiler (crd-shader-vulkan) and names no shading language.
set -u
repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

transitional=""  # EMPTY as of D-008 C2-e
fail=0
emit() { echo "  $1"; fail=1; }

# Enumerate source files under the tracked roots.
mapfile -t files < <(find "$repo_root/engine" "$repo_root/tools" "$repo_root/tests" "$repo_root/sandbox" \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' -o -name '*.cxx' \) 2>/dev/null)

for f in "${files[@]}"; do
    rel="${f#"$repo_root"/}"
    # A — the deleted portable compile API must not be referenced.
    while IFS=: read -r ln _; do
        [ -n "$ln" ] && emit "${rel}:${ln}: references the removed crd::shader::compile_* (use crd::gpu::compile_*_to_spirv in a backend)"
    done < <(grep -nE 'crd::shader::compile_(glsl|hlsl)' "$f" 2>/dev/null | cut -d: -f1 | sed 's/$/:/')
    [ -n "$transitional" ] && [ "$rel" = "$transitional" ] && continue  # allowlist (empty since D-008 C2-e)
    # C — shaderc/dxc includes only in the Vulkan backend.
    case "$rel" in
        engine/gpu-context-vulkan/*) : ;;
        *)
            while IFS=: read -r ln _; do
                [ -n "$ln" ] && emit "${rel}:${ln}: shaderc/dxc include outside engine/gpu-context-vulkan"
            done < <(grep -nE '#[[:space:]]*include[[:space:]]*[<\"](shaderc/|dxc/)' "$f" 2>/dev/null | cut -d: -f1 | sed 's/$/:/')
            ;;
    esac
    # B — crd-shader must name no shaderc/dxc symbol.
    case "$rel" in
        engine/shader/*)
            while IFS=: read -r ln _; do
                [ -n "$ln" ] && emit "${rel}:${ln}: crd-shader must not name a shading-language compiler"
            done < <(grep -nE '\bshaderc_[a-z_]+|\bDxcCreateInstance\b|\bIDxcCompiler' "$f" 2>/dev/null | cut -d: -f1 | sed 's/$/:/')
            ;;
    esac
done

if [ "$fail" -ne 0 ]; then
    echo "[check_no_shader_language_leak] FAIL (ADR-0103 I1). See ADR-0103 / docs/detours/D-008."
    exit 1
fi
echo "[check_no_shader_language_leak] PASS - I1 fully closed; no shading language outside a backend (${#files[@]} files scanned)"
exit 0
