#!/usr/bin/env bash
# check_no_untagged_physical_numeric.sh -- Linux/macOS sibling of the
# .ps1. Bans bare-f32/f64 for fields representing physical quantities.

set -u

REPO_ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
ENGINE_DIR="$REPO_ROOT/engine"

if [[ ! -d "$ENGINE_DIR" ]]; then
    echo "[check_no_untagged_physical_numeric] PASS - no engine directory yet"
    exit 0
fi

# Scope-out patterns
EXCLUDE_PATTERN='engine/math/src/simd/|engine/math/include/crd/math/simd/|engine/rhi-vulkan/'

# Physical-quantity field name patterns
NAME_PATTERN='length|distance|radius|diameter|width|height|depth|mass|weight|velocity|speed|acceleration|force|torque|pressure|energy|power|temperature|duration|voltage|current|resistance|capacitance|inductance|frequency'

# Bare-scalar type tokens
TYPE_PATTERN='\b(f32|f64|float|double)\b'

# Field-declaration heuristic. See the .ps1 sibling for the rationale: only
# match real struct/class field decls, not function-parameter list members
# (which end with `,` or `);` and contain `(` / `)` on the line).
FIELD_REGEX="^[[:space:]]*${TYPE_PATTERN}[[:space:]]+[a-zA-Z_][a-zA-Z_0-9]*(${NAME_PATTERN})[a-zA-Z_0-9]*[[:space:]]*(=[[:space:]]*[^;()]*)?;[[:space:]]*(//.*)?$"

failures=()
while IFS= read -r -d '' file; do
    while IFS=: read -r lineno line; do
        if [[ "$line" =~ crd-lint-allow-untagged-physical ]]; then
            continue
        fi
        failures+=("  $file:$lineno: $line")
    done < <(grep -nE "$FIELD_REGEX" "$file" 2>/dev/null || true)
done < <(find "$ENGINE_DIR" \( -name '*.cpp' -o -name '*.hpp' \) -print0 | LC_ALL=C grep -zvE "$EXCLUDE_PATTERN")

if [[ ${#failures[@]} -gt 0 ]]; then
    echo "[check_no_untagged_physical_numeric] FAIL: ${#failures[@]} field(s) with bare-scalar physical-quantity type:"
    printf '%s\n' "${failures[@]}"
    echo ""
    echo "  Replace bare-f32/f64 fields with Quantity<D, T> from crd-units."
    echo "  E.g.: 'f32 length' -> 'Quantity<dim::Length, f32> length' (or 'Length<f32> length')."
    echo "  Per Strategic Execution Plan 2026-05-15: every physical/scientific quantity carries a unit."
    echo "  Suppress with a 'crd-lint-allow-untagged-physical' marker on the same line if truly justified."
    exit 1
fi

echo "[check_no_untagged_physical_numeric] PASS - no bare-scalar physical-quantity fields"
exit 0
