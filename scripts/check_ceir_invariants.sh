#!/usr/bin/env bash
# crd-ceir invariants (ADR-0109 §4.3): I3 — no shading-language/bytecode name or forbidden-module include crosses into
# crd-ceir (it holds KernelRef identities, never kernels/backends). I5 — the crd-ceir link edge set is exactly the five
# host-only substrate modules (+ crd-warnings). The GPU/jobs stacks are reached via the dependency-inversion bridges
# (crd-ceir-host / crd-ceir-gpu), NEVER a link edge into the core.
set -euo pipefail
root="${1:-.}"
ceir="$root/engine/ceir"
violations=()

# ── I5: link edges ──
allowed=" crd-core crd-log crd-memory crd-containers crd-units crd-warnings crd-ceir "
lineno=0
while IFS= read -r line; do
    lineno=$((lineno + 1))
    code="${line%%#*}" # strip comments (they name the FORBIDDEN modules deliberately)
    for tok in $(grep -oE 'crd-[A-Za-z0-9_-]+' <<<"$code" || true); do
        if [[ "$allowed" != *" $tok "* ]]; then
            violations+=("I5 crd-ceir/CMakeLists.txt:${lineno} links forbidden target '${tok}' (host-only: core/log/memory/containers/units)")
        fi
    done
done <"$ceir/CMakeLists.txt"

# ── I3: (a) forbidden-module includes  (b) NO shading-language/bytecode name ──
# crd-ceir holds KernelRef identities, never kernels/backends — so it must not include a backend module NOR name a
# shading language, its bytecode, or its vendor compiler anywhere (code OR comment). This is the name-half of I3 that
# the engine-wide crd-no-shader-language-leak gate does NOT cover.
forbidden='crd/gpu/|crd/kir|crd/rendergraph|crd/renderpass|crd/renderprogram|crd/rendermaterial|crd/renderasset|crd/framecook|crd/scenerender|crd/jobs/|crd/math/|crd/shader|crd/rhi'
lang_tokens='\b(GLSL|HLSL|WGSL|MSL|SPIR-?V|SPIRV|DXIL|DXBC|PTX|NVRTC|glslang|shaderc|dxc|CUDA)\b'
while IFS= read -r hit; do
    [[ -n "$hit" ]] && violations+=("I3 $hit")
done < <(grep -rnE "#[[:space:]]*include.*($forbidden)" "$ceir/include" "$ceir/src" 2>/dev/null || true)
while IFS= read -r hit; do
    [[ -n "$hit" ]] && violations+=("I3 names a shading language/bytecode/compiler: $hit")
done < <(grep -rnE "$lang_tokens" "$ceir/include" "$ceir/src" 2>/dev/null || true)

# I6 (CEIR-1d, §7): open-world dispatch — the core must NEVER switch on an op's kind. `switch (op.kind())` (method
# form); a `switch` over a CLOSED value enum member like `attr.kind` (no parens) is fine.
while IFS= read -r hit; do
    [[ -n "$hit" ]] && violations+=("I6 switches on an op KIND (dispatch via traits/interfaces): $hit")
done < <(grep -rnE "switch[[:space:]]*\(.*\bkind[[:space:]]*\([[:space:]]*\)" "$ceir/include" "$ceir/src" 2>/dev/null || true)

if [[ ${#violations[@]} -gt 0 ]]; then
    echo "crd-ceir invariant violations (ADR-0109 I3/I5 + open-world I6):"
    printf '  %s\n' "${violations[@]}"
    exit 1
fi
echo "crd-ceir invariants OK (I3: no shader-lang/forbidden include; I5: host-only links; I6: no switch on op.kind)."
exit 0
