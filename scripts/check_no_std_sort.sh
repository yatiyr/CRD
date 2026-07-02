#!/usr/bin/env bash
# check_no_std_sort.sh — bans std::sort / std::stable_sort / std::nth_element
# / std::partial_sort / std::push_heap / std::pop_heap / std::make_heap /
# std::sort_heap in engine/eylem/** + engine/hesap/**.
#
# ADR-0063 §3 deterministic ordering contract. Lights up when eylem v1a o
# hesap v0a creates the directory.

set -uo pipefail

repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

banned='std::(sort|stable_sort|nth_element|partial_sort|push_heap|pop_heap|make_heap|sort_heap)\b'

scopes=(
    "$repo_root/engine/eylem"
    "$repo_root/engine/hesap"
)

failures=()
for scope in "${scopes[@]}"; do
    [[ -d "$scope" ]] || continue
    while IFS= read -r match; do
        if echo "$match" | grep -q 'crd-lint-allow-std-sort'; then continue; fi
        failures+=("  $match")
    done < <(grep -rEn --include='*.cpp' --include='*.hpp' --include='*.h' "$banned" "$scope" 2>/dev/null || true)
done

if [[ ${#failures[@]} -gt 0 ]]; then
    echo "[check_no_std_sort] FAIL: ${#failures[@]} banned std::* sort/heap call(s) found in eylem/hesap source:"
    printf '%s\n' "${failures[@]}"
    echo ""
    echo "  Use crd::containers::sort / stable_sort / nth_element / push_heap / pop_heap / make_heap / sort_heap instead."
    echo "  Justified exceptions can suppress with a 'crd-lint-allow-std-sort' marker on the same line."
    exit 1
fi

echo "[check_no_std_sort] PASS - no banned std::* sort/heap calls in engine/eylem or engine/hesap"
exit 0
