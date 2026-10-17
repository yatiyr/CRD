#!/usr/bin/env bash
# crd-ceir invariants (ADR-0109 §4.3): I3 — no shading-language/bytecode name or forbidden-module include crosses into
# crd-ceir (it holds KernelRef identities, never kernels/backends). I5 — the crd-ceir link edge set is exactly the five
# host-only substrate modules (+ crd-warnings). The GPU/jobs stacks are reached via the dependency-inversion bridges
# (crd-ceir-host / crd-ceir-gpu), NEVER a link edge into the core.
set -euo pipefail
root="${1:-.}"
ceir="$root/engine/execution/ceir"
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
forbidden='crd/gpu/|crd/kir|crd/rendergraph|crd/renderpass|crd/renderprogram|crd/rendermaterial|crd/renderasset|crd/resources|crd/framecook|crd/scenerender|crd/jobs/|crd/math/|crd/shader|crd/rhi'
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

# ── U-116 (external-plugin proof, CEIR-9h): the central OPEN-WORLD enums must NOT grow to accommodate a plugin/domain.
# TypeKind/AttrKind's LAST enumerator is the `Extern` door — a new type/attr rides Extern + registration, NEVER a new
# enum value (the 8a/8b thesis). A future slice widening one trips this forever. (EffectFamily is already pinned at
# compile time by its kLastEffectFamily static_assert.)
enum_last() { # $1 file, $2 enum → prints the last enumerator identifier
    awk -v e="$2" '
        $0 ~ ("enum class " e "([^A-Za-z0-9_]|$)") { inenum = 1; next }
        inenum && /^[[:space:]]*}/ { print last; exit }
        inenum {
            line = $0; sub(/\/\/.*/, "", line); gsub(/^[[:space:]]+/, "", line)
            if (match(line, /^[A-Za-z_][A-Za-z0-9_]*/)) { last = substr(line, RSTART, RLENGTH) }
        }
    ' "$1"
}
for pin in "include/crd/ceir/type.hpp:TypeKind:Extern" "include/crd/ceir/attr.hpp:AttrKind:Extern"; do
    IFS=: read -r pf pe px <<<"$pin"
    got="$(enum_last "$ceir/$pf" "$pe")"
    if [[ "$got" != "$px" ]]; then
        violations+=("U-116 $pe last enumerator is '$got', expected '$px' — a central enum grew; a plugin/domain must use the Extern door + registration, not a new enum value")
    fi
done

# ── I7 (DIAG.11b): `func::func_body_block` is the lookup that may miss; every caller tests its result against nullptr
# (on the call's line, or the assigned variable within the next six lines). A caller holding a func.func that has a body
# takes `func::func_body`, fatal in every build. An unchecked lookup is a null path the optimizer can inline into the
# dereference, and GCC's link-time -Wnull-dereference then fails an unrelated edit's link (test_hello.cpp, 2026-10-17).
i7_awk='
{ line[NR] = $0 }
END {
    for (i = 1; i <= NR; i++) {
        code = line[i]; sub(/\/\/.*/, "", code)
        if (code !~ /func_body_block[ \t]*\(/) { continue }
        if (code ~ /func_body_block[ \t]*\([^;]*\)[ \t]*[!=]=[ \t]*nullptr/) { continue }
        var = ""
        if (match(code, /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*[^= \t][^;]*func_body_block/)) {
            lhs = substr(code, RSTART, RLENGTH)
            if (match(lhs, /^[A-Za-z_][A-Za-z0-9_]*/)) { var = substr(lhs, RSTART, RLENGTH) }
        }
        ok = 0
        if (var != "") {
            for (j = i; j <= i + 6 && j <= NR; j++) {
                c = line[j]; sub(/\/\/.*/, "", c)
                if (c ~ ("(^|[^A-Za-z0-9_])" var "[ \t]*[!=]=[ \t]*nullptr") ||
                    c ~ ("nullptr[ \t]*[!=]=[ \t]*" var "([^A-Za-z0-9_]|$)")) { ok = 1; break }
            }
        }
        if (!ok) { t = line[i]; gsub(/^[ \t]+/, "", t); print FILENAME ":" i ": " t }
    }
}'
i7_dirs=()
for d in engine tests tools runtime sandbox; do
    [[ -d "$root/$d" ]] && i7_dirs+=("$root/$d")
done
i7_own='engine/execution/ceir/(include/crd/ceir/func\.hpp|src/func\.cpp)$'
i7_msg='I7 an unchecked func::func_body_block (test it against nullptr, or take func::func_body)'
while IFS= read -r f; do
    while IFS= read -r hit; do
        [[ -n "$hit" ]] && violations+=("$i7_msg: $hit")
    done < <(awk "$i7_awk" "$f")
done < <(grep -rlE 'func_body_block[[:space:]]*\(' "${i7_dirs[@]}" --include='*.cpp' --include='*.hpp' --include='*.h' \
             --include='*.inl' 2>/dev/null | grep -vE "$i7_own" || true)

if [[ ${#violations[@]} -gt 0 ]]; then
    echo "crd-ceir invariant violations (ADR-0109 I3/I5 + open-world I6 + U-116 enum-pins + I7 body lookups):"
    printf '  %s\n' "${violations[@]}"
    exit 1
fi
echo "crd-ceir invariants OK (I3: no shader-lang/forbidden include; I5: host-only links; I6: no switch on op.kind; U-116: TypeKind/AttrKind end at Extern; I7: every func_body_block result tested)."
exit 0
