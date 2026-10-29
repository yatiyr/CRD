#!/usr/bin/env python3
"""crd-math log core coefficients (crd/math/exp_log_core.hpp, kLogR0..kLogR7).

log(m) = 2·atanh(t) with t = (m − 1)/(m + 1), m in [√½, √2), so |t| ≤ 3 − 2√2. The core evaluates
2t + 2t·s·R(s), s = t², R(s) = (atanh(t) − t)/t³ = 1/3 + s/5 + s²/7 + ... This prints a degree-7 Chebyshev
(near-minimax) fit of R on [0, (3 − 2√2)²], rounded to doubles, lowest degree first. Its relative error is about
3e-18; the tail it feeds is at most 1% of log(m), so the fit adds below 2^-63 relative.
Run with mpmath installed: python gen_log_coeffs.py"""
import mpmath as mp

mp.mp.prec = 300
S_MAX = (3 - 2 * mp.sqrt(2)) ** 2


def r_exact(s):
    if s == 0:
        return mp.mpf(1) / 3
    t = mp.sqrt(s)
    return (mp.atanh(t) - t) / t**3


coeffs, err = mp.chebyfit(r_exact, [0, S_MAX], 8, error=True)  # highest degree first
print(f"// relative error {mp.nstr(err * 3, 3)}")
for i, c in enumerate(reversed(coeffs)):
    print(f"inline constexpr double kLogR{i} = {float(c):.17e};")
