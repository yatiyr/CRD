#!/usr/bin/env python3
"""crd-math minimax coefficients: exp (exp_log_core.hpp kE2..kE11), expm1 (transcendental.hpp kQ1..kQ11), the
sin/cos cores (trig.hpp kSin1..kSin6, kCos2..kCos7) and pow's log series tail (exp_log_core.hpp kLogPairR3..).

Each is the best relative-error approximation of its form over the domain the evaluation reaches, found by a Remez
exchange on a dense Chebyshev grid. Coefficients are rounded to doubles lowest degree first, each rounding followed
by a refit of the higher ones, so the low terms' rounding is compensated where it can be. The printed error is the
rounded polynomial's relative error in units of 2^-53, measured exactly on a finer grid; the evaluation's own
roundings come on top. Run with mpmath installed: python gen_minimax_coeffs.py"""
import mpmath as mp

mp.mp.prec = 128
GRID = 3000


def chebyshev_grid(a, b, n):
    return [(a + b) / 2 - (b - a) / 2 * mp.cos(mp.pi * i / (n - 1)) for i in range(n)]


def solve(cols, rhs, ref):
    """Coefficients and signed level h with error (-1)^r h at each reference point."""
    n = len(cols)
    a = mp.matrix(n + 1, n + 1)
    y = mp.matrix(n + 1, 1)
    for r, i in enumerate(ref):
        for j in range(n):
            a[r, j] = cols[j][i]
        a[r, n] = (-1) ** r
        y[r] = rhs[i]
    sol = mp.lu_solve(a, y)
    return [sol[j] for j in range(n)], abs(sol[n])


def remez(xs, fs, base, powers, ref):
    """Best relative error of base(x) + sum c_j x^p_j against f on the grid xs, from the n + 1 point reference
    ref; returns (c, max |error|, final reference)."""
    n = len(powers)
    rhs = [(f - b) / f for f, b in zip(fs, base)]
    cols = [[x**p / f for x, f in zip(xs, fs)] for p in powers]
    best = None
    for _ in range(60):
        c, level = solve(cols, rhs, ref)
        err = [sum(c[j] * cols[j][i] for j in range(n)) - rhs[i] for i in range(len(xs))]
        peak = max(abs(e) for e in err)
        if best is None or peak < best[1]:
            best = (c, peak, ref)
        # On a discrete grid the exchange can cycle among references of equal level: keep the best seen.
        if peak - level <= level * mp.mpf(10) ** -3:
            break
        # One extremum per run of equal sign, then drop the smaller end until n + 1 alternate.
        ext = []
        for i, e in enumerate(err):
            if e == 0:
                continue
            if ext and (err[ext[-1]] > 0) == (e > 0):
                if abs(e) > abs(err[ext[-1]]):
                    ext[-1] = i
            else:
                ext.append(i)
        if len(ext) < n + 1:
            break
        while len(ext) > n + 1:
            ext.pop(0 if abs(err[ext[0]]) < abs(err[ext[-1]]) else -1)
        ref = ext
    return best


def fit(name, f, base, powers, lo, hi):
    xs = chebyshev_grid(lo, hi, GRID)
    fs = [f(x) for x in xs]
    fixed = []
    bases = [base(x) for x in xs]
    ref = None
    for k in range(len(powers)):
        # Each round starts both from an evenly spread reference and from the last round's; the better one wins.
        cold = [round(i * (GRID - 1) / (len(powers) - k)) for i in range(len(powers) - k + 1)]
        c, _, ref = min((remez(xs, fs, bases, powers[k:], r) for r in (cold, ref) if r), key=lambda t: t[1])
        fixed.append(float(c[0]))
        bases = [b + mp.mpf(fixed[-1]) * x ** powers[k] for b, x in zip(bases, xs)]
        if k + 1 < len(powers):
            # Warm start: of the references one point shorter, the one with the largest level is closest to the
            # next problem's optimum (the exchange maximises the level).
            rhs = [(fx - b) / fx for fx, b in zip(fs, bases)]
            cols = [[x**p / fx for x, fx in zip(xs, fs)] for p in powers[k + 1 :]]
            ref = max((ref[:d] + ref[d + 1 :] for d in range(len(ref))), key=lambda r: solve(cols, rhs, r)[1])
    fine = chebyshev_grid(lo, hi, 4 * GRID)
    peak = 0
    for x in fine:
        fx = f(x)
        if fx != 0:
            peak = max(peak, abs((base(x) + sum(mp.mpf(c) * x**p for c, p in zip(fixed, powers)) - fx) / fx))
    print(f"// {name}: relative error {mp.nstr(peak * mp.mpf(2) ** 53, 3)} x 2^-53")
    return fixed


# The domains carry the reductions' slack: |r| <= ln2/2 after exp's Cody-Waite step (k from a rounded x*log2e),
# |r| <= pi/4 after the trig reduction, s = t^2 <= (3 - 2 sqrt 2)^2 after the log reduction; sin and cos start at
# 2^-40, as their relative error is 0/0 at r = 0, and the log tail at s = 2^-80.
LN2_HALF = mp.log(2) / 2 * (1 + mp.mpf(2) ** -30)
PI_4 = mp.pi / 4 * (1 + mp.mpf(2) ** -30)
TINY = mp.mpf(2) ** -40
LOG_S = (3 - 2 * mp.sqrt(2)) ** 2 * (1 + mp.mpf(2) ** -30)


def atanh_ratio(s):
    """atanh(t)/t at s = t^2: log(m) = 2t * atanh_ratio(s)."""
    t = mp.sqrt(s)
    return mp.atanh(t) / t


kE = fit("exp", mp.exp, lambda x: 1 + x, list(range(2, 12)), -LN2_HALF, LN2_HALF)
kQ = fit("expm1", mp.expm1, lambda x: x, list(range(2, 13)), -LN2_HALF, LN2_HALF)
kSin = fit("sin", mp.sin, lambda r: r, [2 * j + 1 for j in range(1, 7)], TINY, PI_4)
kCos = fit("cos", mp.cos, lambda r: 1 - r * r / 2, [2 * j for j in range(2, 8)], TINY, PI_4)
# pow's log2 pair: atanh(t)/t = 1 + s/3 + s^2/5 + s^3 R3(s), the first two terms as double-doubles in the evaluation.
kLogPairR = fit("log tail", atanh_ratio, lambda s: 1 + s / 3 + s * s / 5, list(range(3, 10)), TINY * TINY, LOG_S)
for prefix, first, coeffs in (("kE", 2, kE), ("kQ", 1, kQ), ("kSin", 1, kSin), ("kCos", 2, kCos),
                              ("kLogPairR", 3, kLogPairR)):
    for i, c in enumerate(coeffs):
        print(f"{prefix}{first + i} = {c:.17e}")
