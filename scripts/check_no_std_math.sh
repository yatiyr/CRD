#!/usr/bin/env bash
# check_no_std_math.sh — bans std::sin / std::cos / std::tan / std::atan2 /
# std::exp / std::log / std::pow / std::asin / std::acos / std::atan in
# engine/eylem/** and engine/hesap/**. Per ADR-0063 §2: those modules must
# use crd::math::deterministic::* substitutes for cross-platform bit-exact
# results.
#
# Today (Phase 3.1 v0c) eylem and hesap don't exist yet, so this script
# is a no-op. It lights up the moment eylem v1a or hesap v0a lands.

set -uo pipefail

repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

banned='std::(sin|cos|tan|asin|acos|atan|atan2|exp|exp2|log|log2|log10|pow|fmod)f?\b'

scopes=(
    "$repo_root/engine/eylem"
    "$repo_root/engine/hesap"
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
