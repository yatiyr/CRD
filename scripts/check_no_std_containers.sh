#!/usr/bin/env bash
# check_no_std_containers.sh — bans OWNING STL containers everywhere in the repo
# (engine, tools, tests, sandbox), including test code and generated `.inc`
# reference-data headers.
#
# Contract: docs/CODING.md ("No owning STL containers. Use Cerid
# Array/String/HashMap; non-owning span/string_view/optional and standard
# algorithms are permitted"), docs/PRINCIPLES.md. This is the machine
# enforcement of a longstanding rule.
#
# BANNED (owning): std::string / basic_string / wstring / u*string, std::vector,
#   std::array, std::deque, std::forward_list, std::list, std::map / multimap /
#   unordered_map / unordered_multimap, std::set / multiset / unordered_set /
#   unordered_multiset, std::stack, std::queue, std::priority_queue, std::valarray.
# ALLOWED (not owning containers): std::span, std::string_view, std::optional,
#   std::pair, std::tuple, and everything in <algorithm>.
#
# Replacements: std::vector -> crd::containers::Array; std::string ->
#   crd::containers::String; std::map/unordered_map -> crd::containers::HashMap;
#   std::set/unordered_set -> crd::containers::HashSet; std::array ->
#   crd::containers::FixedArray or a plain C array (T x[N]) for local literal
#   tables and generated reference data.
#
# A justified exception at a genuine third-party boundary may suppress a single
# line with a 'crd-lint-allow-std-container' marker on that line.

set -uo pipefail

repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

# Word boundary after each token keeps std::string from matching std::string_view
# and std::array from matching any *_view sibling (g/y are word chars, so no
# boundary precedes the underscore).
banned='std::(string|basic_string|wstring|u8string|u16string|u32string|vector|array|deque|forward_list|list|multimap|unordered_map|unordered_multimap|map|multiset|unordered_set|unordered_multiset|set|stack|priority_queue|queue|valarray)\b'

scopes=(
    "$repo_root/engine"
    "$repo_root/tools"
    "$repo_root/tests"
    "$repo_root/sandbox"
)

present=()
for scope in "${scopes[@]}"; do
    [[ -d "$scope" ]] && present+=("$scope")
done

# One grep pass over the tree finds candidate lines (file:line:content); the
# post-filter re-checks each candidate with comments stripped and honours the
# marker. This is far faster than a sed+grep process per file.
failures=()
if [[ ${#present[@]} -gt 0 ]]; then
    while IFS= read -r cand; do
        [[ -z "$cand" ]] && continue
        # cand is "path:line:content".
        content="${cand#*:}"; content="${content#*:}"
        if echo "$content" | grep -q 'crd-lint-allow-std-container'; then continue; fi
        # Strip // and single-line /* */ comments; keep only real-code matches.
        stripped="$(echo "$content" | sed -e 's://.*$::' -e 's:/\*[^*]*\*/::g')"
        if echo "$stripped" | grep -qE "$banned"; then
            failures+=("  ${cand#"$repo_root"/}")
        fi
    done < <(grep -rEn --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.inc' --include='*.cc' --include='*.cxx' "$banned" "${present[@]}" 2>/dev/null || true)
fi

if [[ ${#failures[@]} -gt 0 ]]; then
    echo "[check_no_std_containers] FAIL: ${#failures[@]} owning STL container use(s) found:"
    printf '%s\n' "${failures[@]}"
    echo ""
    echo "  Use crd::containers::Array / String / HashMap / HashSet / FixedArray, or a plain C array for"
    echo "  fixed local tables and generated reference data. Non-owning std::span/string_view/optional and"
    echo "  <algorithm> are allowed. A genuine third-party boundary may mark one line 'crd-lint-allow-std-container'."
    exit 1
fi

echo "[check_no_std_containers] PASS - no owning STL containers in engine/tools/tests/sandbox"
exit 0
