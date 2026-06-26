#!/bin/bash
# check_no_std_transcendental.sh [repo-root] — the Cerid Math Mandate guard. Engine + tool code must use crd::math::*
# (include <crd/math/cmath.hpp>), never std:: transcendentals — for cross-platform bit-determinism (the moat) +
# speed. Exempts engine/math (the kernel IS the implementation + its std:: edge fallbacks). tests/ (gold oracles)
# and runtime/examples (benchmark peers) keep std:: deliberately and live outside engine/, so they're not scanned.
#
# tx-route status: registered as a ctest guard at the route CLOSE, once every module is migrated. Until then this is
# a PROGRESS tracker — run it to see which modules still carry std:: transcendentals. Routed so far: hesap-comms,
# hesap-wavelet, hesap-dsp (re-gated green). Remaining: hesap-stats/-special/-opt/-dense/-ode/-fft, geometry-primitives.
set -uo pipefail
root="${1:-.}"
fns='sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|asinh|acosh|atanh|exp|exp2|exp10|expm1|log|log2|log10|log1p|pow|cbrt|hypot'
hits=$(grep -rnE "std::($fns)\b" "$root/engine" --include=*.hpp --include=*.cpp 2>/dev/null | grep -vE "/math/" || true)
if [ -n "$hits" ]; then
    echo "std:: transcendentals still in engine code ($(echo "$hits" | wc -l) sites) — route to crd::math::*:"
    echo "$hits" | grep -oE "engine/[a-z-]+/" | sort | uniq -c | sort -rn
    exit 1
fi
echo "PASS: no std:: transcendentals in engine code (the Cerid Math Mandate holds)."
