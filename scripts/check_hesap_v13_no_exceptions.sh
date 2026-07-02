#!/bin/bash
# check_hesap_v13_no_exceptions.sh [repo-root] -- v13 safety-critical conformance guard (mirror of the .ps1).
# The v13 numerical-analysis + motion cluster (crd-hesap-{interp,quadrature,diff,motion}) is STATUS-NOT-EXCEPTION by
# contract (ADR-0095, moat pillar 3): every entry point returns a status enum ({value, status, ...}); errors NEVER
# escape as C++ exceptions. That is the DO-178C / ISO 26262 ASIL-D / MISRA-C++ no-exception-escape property the
# incumbents (Boost.Math throws) structurally lack. Guard: no throw/try/catch in the four module headers (trailing
# // comments stripped before matching).
set -uo pipefail
root="${1:-.}"
hits=""
for d in hesap-quadrature hesap-interp hesap-diff hesap-motion; do
    while IFS= read -r line; do
        code="${line%%//*}" # drop trailing // comment (paths under engine/ carry no //)
        if echo "$code" | grep -qE "\b(throw|try|catch)\b"; then
            hits+="$line"$'\n'
        fi
    done < <(grep -rnE "\b(throw|try|catch)\b" "$root/engine/$d/include" --include=*.hpp --include=*.cpp 2>/dev/null || true)
done
if [ -n "$hits" ]; then
    echo "FAIL: exception constructs in v13 hesap headers -- v13 is status-not-exception (ADR-0095 pillar 3):"
    echo "$hits" | head -30
    exit 1
fi
echo "PASS: no throw/try/catch in v13 hesap headers (status-not-exception holds)."
