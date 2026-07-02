#!/bin/bash
# route-to-crd-math.sh <module-dir> — migrate a module's engine source from std:: math to crd::math:: (the Cerid
# Math Mandate; docs/phases/crd-math-transcendental.md tx-route). Scope: include/ + src/ ONLY — never tests/ (thei
# std:: is the gold-standard oracle) or runtime/examples (their std:: is the benchmark peer). After running:
#   rebuild the module's tests and RE-GATE (the ~1-ulp shift must keep every gold gate green); only then is it routed.
# Proven on engine/hesap-comms (39663-assertion suite green post-route). Idempotent.
set -euo pipefail
dir="${1:?usage: route-to-crd-math.sh <module-dir>}"

# the transcendental + select functions crd::math provides (longer names first so \b backtracking is unambiguous).
fns='atan2|asinh|acosh|atanh|expm1|exp10|exp2|log10|log1p|log2|sinh|cosh|tanh|asin|acos|atan|sin|cos|tan|exp|log|pow|sqrt|cbrt|hypot|rsqrt|polar|arg|fabs|floor|ceil|round|trunc|nearbyint|fmod|lround|copysign'

for file in $(grep -rlE "std::($fns)\b" "$dir/include" "$dir/src" 2>/dev/null || true); do
    # point the first <cmath> at the unified umbrella (transitively still provides std::isnan/isfinite/isinf etc.)
    if grep -q '#include <cmath>' "$file"; then
        sed -i -E '0,/#include <cmath>/s@#include <cmath>@#include <crd/math/cmath.hpp>@' "$file"
    else
        # no <cmath> — inject the umbrella after the first #include
        sed -i -E '0,/^#include /s@^(#include .*)$@\1\n#include <crd/math/cmath.hpp>@' "$file"
    fi
    sed -i -E "s/std::($fns)\b/crd::math::\1/g" "$file"
    echo "routed: $file"
done
echo "done. now: build the module's tests + RE-GATE before considering it routed."
