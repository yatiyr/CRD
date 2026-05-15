#!/usr/bin/env bash
# scripts/per-slice-check.sh
#
# Per-slice verification on Linux: build + ctest across linux-gcc-debug + linux-gcc-asan.
#
# Linux per-slice is a lighter check than Windows (no clang-cl variant; no LTO-shipping
# variant for daily slice gating — that lane runs in scripts/full-sweep.ps1's
# -SkipWin pass). The two configs covered are the most load-bearing:
#   - linux-gcc-debug: catches -O0 + assertion-firing bugs + cross-platform code issues
#   - linux-gcc-asan:  catches UAF / leak / OOB + UBSan integer overflow
#
# Same guard-test discipline as the .ps1: ctest --preset returning 0 is required;
# the test binary's "all passed" output by itself does not count.
#
# Usage:
#   ./scripts/per-slice-check.sh                # both configs
#   ./scripts/per-slice-check.sh --reconfigure  # cmake --preset first
#   ./scripts/per-slice-check.sh --skip-asan    # skip asan (rarely needed)
#
# Exit code: 0 if every requested config passed, count of failures otherwise.

set -u

RECONFIGURE=false
SKIP_ASAN=false

for arg in "$@"; do
    case "$arg" in
        --reconfigure) RECONFIGURE=true ;;
        --skip-asan)   SKIP_ASAN=true ;;
        *)
            echo "Unknown arg: $arg" >&2
            echo "Usage: $0 [--reconfigure] [--skip-asan]" >&2
            exit 2
            ;;
    esac
done

failed=0
declare -A results

run_preset() {
    local preset="$1"
    echo
    echo "===== $preset ====="

    if $RECONFIGURE; then
        echo "[per-slice] cmake --preset $preset"
        if ! cmake --preset "$preset"; then
            results["$preset"]="CONFIGURE-FAIL"
            failed=$((failed+1))
            return
        fi
    fi

    if ! cmake --build --preset "$preset"; then
        results["$preset"]="BUILD-FAIL"
        failed=$((failed+1))
        return
    fi

    if ! ctest --preset "$preset" --output-on-failure; then
        results["$preset"]="CTEST-FAIL"
        failed=$((failed+1))
        return
    fi

    results["$preset"]="PASS (build+ctest)"
}

echo '===================================================================='
echo '  PER-SLICE VERIFICATION (linux)'
echo '===================================================================='

start_time=$(date +%s)

run_preset linux-gcc-debug
if ! $SKIP_ASAN; then
    run_preset linux-gcc-asan
fi

end_time=$(date +%s)
elapsed=$(( end_time - start_time ))
mm=$(( elapsed / 60 ))
ss=$(( elapsed % 60 ))

echo
echo '----- PER-SLICE SUMMARY -----'
for k in "${!results[@]}"; do
    printf '  %-22s %s\n' "$k" "${results[$k]}"
done

echo
echo '===================================================================='
if [[ $failed -eq 0 ]]; then
    printf '  RESULT: PASS  (elapsed %02d:%02d)\n' "$mm" "$ss"
else
    printf '  RESULT: FAIL (%d config(s) failed)  (elapsed %02d:%02d)\n' "$failed" "$mm" "$ss"
fi
echo '===================================================================='

exit "$failed"
