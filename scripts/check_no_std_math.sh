#!/usr/bin/env bash
# check_no_std_math.sh — bans std::sin / std::cos / std::tan / std::atan2 /
# std::exp / std::log / std::pow / std::asin / std::acos / std::atan in the
# determinism-contract modules (ADR-0063 §2): they must use
# crd::math::deterministic::* substitutes for cross-platform bit-exact results.
# (std::sqrt is NOT banned — IEEE-754 mandates correctly-rounded single-rounding
# sqrt everywhere, so it is deterministic.)
#
# Scoped to engine/eylem, engine/hesap (ADR-0063), and engine/geometry-primitives
# (ADR-0076 §4 — crd-geometry inherits the determinism contract). Sub-modules in
# sibling directories (engine/eylem-rigid3d, engine/geometry-bvh, ...) are added
# here as they land.

set -uo pipefail

repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

banned='std::(sin|cos|tan|asin|acos|atan|atan2|exp|exp2|log|log2|log10|pow|fmod)f?\b'

scopes=(
    "$repo_root/engine/eylem"
    "$repo_root/engine/hesap"
    "$repo_root/engine/geometry-primitives"
    "$repo_root/engine/geometry-bvh"
    "$repo_root/engine/geometry-shader-helpers"
)

failures=()
for scope in "${scopes[@]}"; do
    [[ -d "$scope" ]] || continue
    while IFS= read -r match; do
        # Allow opt-out via 'crd-lint-allow-std-math' marker on the same line.
        if echo "$match" | grep -q 'crd-lint-allow-std-math'; then continue; fi
        failures+=("  $match")
    done < <(grep -rEn --include='*.cpp' --include='*.hpp' --include='*.h' "$banned" "$scope" 2>/dev/null || true)
done

if [[ ${#failures[@]} -gt 0 ]]; then
    echo "[check_no_std_math] FAIL: ${#failures[@]} banned std::* math call(s) found in eylem/hesap source:"
    printf '%s\n' "${failures[@]}"
    echo ""
    echo "  Use crd::math::deterministic::sin / cos / tan / atan2 / exp / log / pow etc. instead."
    echo "  Justified exceptions can suppress with a 'crd-lint-allow-std-math' marker on the same line."
    exit 1
fi

echo "[check_no_std_math] PASS - no banned std::* math calls in engine/eylem or engine/hesap"
exit 0
