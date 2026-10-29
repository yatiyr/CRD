#!/usr/bin/env python3
"""crd-math transcendental accuracy refs — mpmath ground truth → correctly-rounded f64 → plain-C arrays.
THE family pattern: add a function = add one entry to FUNCS (name, cerid-call, mpmath fn, grid). The C++ test
(test_transcendental.cpp) reads these and asserts <=1 ulp. mpmath = arbitrary precision = the gold gate.
NO std containers in the emitted file (plain `inline const double[]`)."""
import sys
import numpy as np
import mpmath as mp

mp.mp.dps = 50  # 50 decimal digits ground truth
sys.stdout.reconfigure(encoding='utf-8', newline='\n')  # the same bytes through a Windows pipe


def grid(lo, hi, n, *, log=False, extra=None):
    if log:  # spaced through mpmath: numpy's exp gives other last bits on other platforms
        xs = np.array([float(mp.exp(v)) for v in mp.linspace(mp.log(lo), mp.log(hi), n)])
    else:
        xs = np.linspace(lo, hi, n)
    if extra:
        xs = np.concatenate([xs, np.array(extra, dtype=float)])
    return np.unique(xs.astype(np.float64))


# The log family's hard inputs: m = x·2^-e on both sides of √2 and √½ (the reduction's seam, where t is largest), at
# exponents 0, ±1 (where ed·ln2 and log(m) cancel), ±1000 and -1022, the bands (√2, 2), (½, √½) and near 1, subnormals.
_SQRT2 = float(mp.sqrt(2))
_log_extra = [0.17705993512268989, 5e-310, 1e-320, 5e-324, 2.2250738585072009e-308]
for _seam in (_SQRT2, _SQRT2 / 2):
    _near = [_seam]
    for _ in range(4):
        _near = [float(np.nextafter(_near[0], 0.0))] + _near + [float(np.nextafter(_near[-1], np.inf))]
    _log_extra += [v * 2.0**k for v in _near for k in (0, 1, -1, 1000, -1000, -1022)]
_log_extra += list(np.linspace(_SQRT2, 2.0, 200)) + list(np.linspace(0.5, _SQRT2 / 2, 200))
_log_extra += list(np.linspace(1.0 - 1e-3, 1.0 + 1e-3, 101))

# (name, mpmath fn, grid) — grid is f64 inputs; ref = correctly-rounded f64 of mpmath(input).
FUNCS = [
    ("exp", mp.exp, grid(-700, 700, 400, extra=[0.0, 1.0, -1.0, 1e-8, -1e-8, 709.0, -745.0])),
    ("exp2", lambda v: mp.power(2, v), grid(-1020, 1020, 400, extra=[0.0, 0.5, -0.5, 10.0, 1023.0])),
    ("exp10", lambda v: mp.power(10, v), grid(-300, 300, 400, extra=[0.0, 1.0, 2.0, -1.0, 0.5])),
    ("expm1", mp.expm1, grid(-40.0, 40.0, 600, extra=[1e-10, -1e-10, 1e-6, -1e-6, 1e-3, 0.34, 0.35, 0.5, -0.5, 0.7, -0.7, 2.0, 3.0, 5.0, -5.0, 700.0, -700.0, 0.0])),
    ("log", mp.log, grid(1e-300, 1e300, 400, log=True, extra=[1.0, 2.0, 0.5, 1.0000001, 0.9999999] + _log_extra)),
    ("log2", lambda v: mp.log(v, 2), grid(1e-300, 1e300, 400, log=True, extra=[1.0, 2.0, 4.0, 0.5, 8.0] + _log_extra)),
    ("log10", lambda v: mp.log10(v), grid(1e-300, 1e300, 400, log=True, extra=[1.0, 10.0, 100.0, 0.1] + _log_extra)),
    ("log1p", mp.log1p, grid(-0.9, 1e6, 300, log=False, extra=[1e-12, -1e-12, 1e-8, -1e-8, 1e-3, -0.5, 0.0])),
]

# trig, the Cody-Waite range (|x| ≤ 1e6): dense small-x + sparse large-x.
_trig_extra = list(np.linspace(-6.3, 6.3, 260)) + [0.0, 0.1, 0.5, 1.0, 1.5707963267948966, 3.141592653589793, 6.283185307179586, 100.0, 1000.0, 12345.678, -100.0, -1000.0]
FUNCS += [
    ("sin", mp.sin, grid(-1e6, 1e6, 1500, extra=_trig_extra)),
    ("cos", mp.cos, grid(-1e6, 1e6, 1500, extra=_trig_extra)),
    ("tan", mp.tan, grid(-1e6, 1e6, 1500, extra=_trig_extra)),
]

# hyperbolic: compose expm1/log1p (≤1 ulp anti-cancellation forms).
_he = [0.0, 1e-8, -1e-8, 1e-4, -1e-4, 0.1, 0.5, 1.0, -1.0, 2.0, 5.0, 10.0]
FUNCS += [
    ("sinh", mp.sinh, grid(-709, 709, 600, extra=_he)),
    ("cosh", mp.cosh, grid(-709, 709, 600, extra=_he)),
    ("tanh", mp.tanh, grid(-20, 20, 600, extra=_he)),
    ("asinh", mp.asinh, grid(-1e7, 1e7, 600, extra=_he + [1e-12, -1e-12, 100.0, -100.0])),
    ("acosh", mp.acosh, grid(1.0, 1e7, 600, extra=[1.0, 1.0000001, 1.0001, 1.01, 1.5, 2.0, 10.0, 100.0])),
    ("atanh", mp.atanh, grid(-0.9999, 0.9999, 600, extra=[0.0, 1e-8, -1e-8, 0.1, 0.5, -0.5, 0.9, -0.9, 0.99])),
]

# inverse trig.
FUNCS += [
    ("atan", mp.atan, grid(-1e6, 1e6, 800, extra=[0.0, 0.1, 0.5, 1.0, -1.0, 2.0, 0.41421356, -0.41421356, 1e-8, -1e-8, 100.0])),
    ("asin", mp.asin, grid(-1.0, 1.0, 800, extra=[0.0, 0.5, -0.5, 0.9999, -0.9999, 1e-8, 0.70710678, -0.70710678])),
    ("acos", mp.acos, grid(-1.0, 1.0, 800, extra=[0.0, 0.5, -0.5, 0.9999, -0.9999, 0.70710678, -0.70710678])),
]

# power/root.
FUNCS += [
    ("cbrt", lambda v: mp.cbrt(v) if v >= 0 else -mp.cbrt(-v), grid(-1e6, 1e6, 700, extra=[1.0, 8.0, 27.0, 64.0, 125.0, 0.001, 1e-9, -1e-9, 1e9, -1e9, -8.0, -27.0, 2.0])),
    ("rsqrt", lambda v: 1 / mp.sqrt(v), grid(1e-300, 1e300, 500, log=True, extra=[1.0, 4.0, 0.25, 2.0, 9.0])),
]


# trig, the Payne–Hanek range: from just below the Cody-Waite seam (|k| = 2^20, x ≈ 1.6471e6) to the largest double,
# both signs, with the doubles nearest k·π/2 for large k (the reduction's hardest inputs) and Muller's worst case.
# Separate arrays, so the arrays above and their golden checksum stay as they are; spaced through mpmath, so the
# points do not depend on the platform's exp and log.
_K2_OVER_PI = 6.36619772367581382433e-01
_seam = (2.0**20 + 0.5) / _K2_OVER_PI
_trig_large = [float(mp.exp(v)) for v in mp.linspace(mp.log(1.0e6), mp.log(1.7e308), 600)] + [1.0e22, 1.0e300, 2.0**1023, sys.float_info.max,
                                                         3.4028234663852886e38, 6381956970095103 * 2.0**797]
for _ in range(2):
    _trig_large += [float(np.nextafter(_seam, 0.0)), _seam, float(np.nextafter(_seam, np.inf))]
    _seam = float(np.nextafter(np.nextafter(_seam, np.inf), np.inf))
with mp.workdps(400):
    _trig_large += [float(k * mp.pi / 2) for k in (2**20, 2**20 + 1, 2**21 + 3, 2**30 + 1, 2**52 + 5, 2**60 + 1,
                                                  2**100 + 1, 2**500 + 1, 2**1000 + 1)]
_trig_large = np.array(_trig_large + [-v for v in _trig_large], dtype=np.float64)
LARGE = [("sin_large", mp.sin, np.unique(_trig_large)), ("cos_large", mp.cos, np.unique(_trig_large)),
         ("tan_large", mp.tan, np.unique(_trig_large))]

# The quadrant k mod 4, k = round(x·2/π), where k fits no integer type.
_quadrant_x = [1.5e16, 6e16, 1e17, -1e17, 1e300, -1e300, sys.float_info.max, -sys.float_info.max,
               6381956970095103 * 2.0**797, -6381956970095103 * 2.0**797]


def emit(name, fn, xs):
    gx, gy = [], []
    for x in xs:
        if not np.isfinite(x):
            continue
        try:
            y = float(fn(mp.mpf(float(x))))  # float(x): numpy 2.0 repr() is "np.float64(..)" — breaks mp.mpf
        except (ValueError, ZeroDivisionError, OverflowError):
            continue
        if not np.isfinite(y):  # skip overflow/underflow-to-edge — ulp gate is for the finite range
            continue
        gx.append(float(x))
        gy.append(y)
    xf = ", ".join(f"{v:.17e}" for v in gx)
    yf = ", ".join(f"{v:.17e}" for v in gy)
    print(f"inline const double ref_{name}_x[] = {{{xf}}};")
    print(f"inline const double ref_{name}_y[] = {{{yf}}};")
    print(f"inline constexpr int ref_{name}_n = {len(gx)};")


print("// GENERATED by gen_transcendental_refs.py — mpmath(50-dps) correctly-rounded f64 refs for crd::math (tx-a).")
print("#pragma once")
print("namespace crd::math::txref {")
for name, fn, xs in FUNCS:
    emit(name, fn, xs)
with mp.workdps(400):
    for name, fn, xs in LARGE:
        emit(name, fn, xs)
    quadrants = [int(mp.nint(mp.mpf(x) * 2 / mp.pi)) % 4 for x in _quadrant_x]
    two_over_pi = mp.mpf(2) / mp.pi
    words = []
    for _ in range(37):
        two_over_pi *= 2**32
        words.append(int(mp.floor(two_over_pi)))
        two_over_pi -= words[-1]
print(f"inline const double ref_quadrant_x[] = {{{', '.join(f'{v:.17e}' for v in _quadrant_x)}}};")
print(f"inline const int ref_quadrant_q[] = {{{', '.join(str(q) for q in quadrants)}}};")
print(f"inline constexpr int ref_quadrant_n = {len(_quadrant_x)};")
print(f"inline const unsigned ref_two_over_pi_bits[] = {{{', '.join(f'0x{w:08X}U' for w in words)}}};")
print("} // namespace")
sys.exit(0)
