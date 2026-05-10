#!/usr/bin/env bash
# check_no_non_ascii_test_names.sh - bans non-ASCII characters in Catch2
# TEST_CASE names across tests/**/*.cpp.
#
# Why: ctest invokes test binaries with the test name as argv. On Windows
# the binary's argv is decoded via the Active Code Page (CP1254 etc on
# Turkish Windows), not UTF-8, so any non-ASCII byte in the TEST_CASE name
# gets mojibake'd, Catch2's filter no longer matches, and ctest reports the
# test as failed even though the test itself is fine. Linux uses UTF-8 argv
# so it works there, but the test name is the same on every platform: keep
# it ASCII and the issue cannot recur.
#
# Bug seed: Phase 3.1 v0e shipped with `simd Quatf rotate vector by 90°
# around Z` (degree symbol U+00B0) and four em-dash bench cases that bit
# us on Windows ctest only.
#
# Justified exceptions can suppress with a 'crd-lint-allow-non-ascii-test-name'
# marker on the same line.

set -uo pipefail

repo_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
tests_dir="$repo_root/tests"

if [[ ! -d "$tests_dir" ]]; then
    echo "[check_no_non_ascii_test_names] PASS - no tests directory yet"
    exit 0
fi

# LC_ALL=C makes grep operate on raw bytes; [^[:print:][:space:]] would also
# match control chars we don't want to flag. Use a byte-range pattern that
# matches anything outside basic ASCII (0x80-0xFF).
failures=()
while IFS= read -r line; do
    # Skip lines that don't look like a TEST_CASE invocation.
    if ! echo "$line" | grep -q 'TEST_CASE[[:space:]]*('; then continue; fi
    # Skip the opt-out marker.
    if echo "$line" | grep -q 'crd-lint-allow-non-ascii-test-name'; then continue; fi
    failures+=("  $line")
done < <(LC_ALL=C grep -rEn --include='*.cpp' --include='*.hpp' $'[\x80-\xff]' "$tests_dir" 2>/dev/null || true)

if [[ ${#failures[@]} -gt 0 ]]; then
    echo "[check_no_non_ascii_test_names] FAIL: ${#failures[@]} TEST_CASE name(s) with non-ASCII characters:"
    printf '%s\n' "${failures[@]}"
    echo ""
    echo "  Replace non-ASCII characters with ASCII equivalents (e.g. degree symbol -> 'deg', em-dash -> '--')."
    echo "  Reason: Windows ctest mojibake's argv via the Active Code Page; Catch2 filter then misses the test."
    echo "  Suppress with a 'crd-lint-allow-non-ascii-test-name' marker on the same line if truly justified."
    exit 1
fi

echo "[check_no_non_ascii_test_names] PASS - all TEST_CASE names are ASCII-only"
exit 0
