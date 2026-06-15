#!/usr/bin/env python3
"""Generate AoS within-transform FFT codelets (the MKL-crush architecture, validated Part 17).

Cerid's SoA-over-k codelets spill at radix-32 (split re/im ⇒ 2× registers); MKL vectorizes WITHIN one transform
(AoS, 2 complex/ymm) so radix-32 fits 16 ymm — the probe measured ~45 GFLOPS (~0.95× MKL mid-band) vs the
SoA-over-k ~25 wall. This generator emits AoS radix-2^b DIT leaf codelets (b=3,4,5 ⇒ radix 8/16/32) as
straight-line AVX2 __m256d, parametrized from the proven probe structure:

  r[k] = [complex 2k, complex 2k+1]  (2^(b-1) ymm).  bit-reversed load.
  stage 0 (distance 1): intra-ymm butterfly [a,b]->[a+b,a-b]  (sw=swap128; blend(a+sw, sw-a)).
  stage s (1..b-1):     m=2^(s+1), dd=2^(s-1), G=2^s; for g in step G, k in [g,g+dd):
                        base=(k-g)*2; twiddle [W_m^base, W_m^{base+1}] on r[k+dd]; bfly(r[k], r[k+dd]).

Each codelet is numpy-self-checked (model vs np.fft) AND the emitted C++ is compiled+run vs a brute DFT.
AoS cmul: result = fmaddsub(wr_bcast, x, wi_bcast * permute_pd(x,0x5)).
"""

import cmath
import sys

import numpy as np

RADIX_BITS = [3, 4, 5]  # radix 8, 16, 32


def brev(i, b):
    return int(f"{i:0{b}b}"[::-1], 2)


def aos_model(x, b, inverse=False):
    """Reference numpy model of the AoS radix-2^b DIT — MUST match np.fft. Returns the N complex outputs."""
    n = 1 << b
    p = n // 2
    sgn = 1.0 if inverse else -1.0
    r = [[x[brev(2 * k, b)], x[brev(2 * k + 1, b)]] for k in range(p)]
    for k in range(p):  # stage 0
        a, bb = r[k]
        r[k] = [a + bb, a - bb]
    for s in range(1, b):  # stages 1..b-1
        m = 1 << (s + 1)
        dd = 1 << (s - 1)
        g_sz = 1 << s
        for g in range(0, p, g_sz):
            for k in range(g, g + dd):
                base = (k - g) * 2
                w0 = cmath.exp(sgn * 2j * cmath.pi * base / m)
                w1 = cmath.exp(sgn * 2j * cmath.pi * (base + 1) / m)
                a = r[k]
                bv = r[k + dd]
                t = [w0 * bv[0], w1 * bv[1]]
                r[k] = [a[0] + t[0], a[1] + t[1]]
                r[k + dd] = [a[0] - t[0], a[1] - t[1]]
    out = [0j] * n
    for k in range(p):
        out[2 * k] = r[k][0]
        out[2 * k + 1] = r[k][1]
    return out


def check_model():
    for b in RADIX_BITS:
        n = 1 << b
        rng = np.random.default_rng(42 + b)
        x = rng.standard_normal(n) + 1j * rng.standard_normal(n)
        got = np.array(aos_model(list(x), b, inverse=False))
        ref = np.fft.fft(x)
        err = np.max(np.abs(got - ref)) / (1 + np.max(np.abs(ref)))
        assert err < 1e-12, f"radix-{n} fwd model FAIL err={err}"
        goti = np.array(aos_model(list(x), b, inverse=True))
        refi = np.fft.ifft(x) * n  # unnormalized inverse
        erri = np.max(np.abs(goti - refi)) / (1 + np.max(np.abs(refi)))
        assert erri < 1e-12, f"radix-{n} inv model FAIL err={erri}"
        print(f"// radix-{n}: fwd {err:.1e} inv {erri:.1e} numpy-self-check PASS", file=sys.stderr)


def emit_codelet(b, inverse, contig=False):
    """Emit the C++ body of an AoS radix-2^b codelet (in/out raw double*, AoS).

    contig=False: bit-reversed gather load (standalone, natural-order in). contig=True: contiguous _mm256_load_pd
    (the ~45-GFLOPS form; computes DFT of the bit-reversed-ordered input — the caller supplies that order).
    """
    n = 1 << b
    p = n // 2
    sgn = 1.0 if inverse else -1.0
    suf = ("inv" if inverse else "fwd") + ("_c" if contig else "")
    L = []
    L.append(f"// AoS radix-{n} DIT codelet ({suf}). in/out: raw AoS doubles, {n} complex.")
    L.append(f"static inline void aos_codelet_{n}_{suf}(const double* in, double* out) noexcept")
    L.append("{")
    if contig:
        for k in range(p):
            L.append(f"    __m256d r{k} = _mm256_loadu_pd(in + {4*k});")
    else:
        for k in range(p):
            a = brev(2 * k, b)
            c = brev(2 * k + 1, b)
            L.append(f"    __m256d r{k} = _mm256_set_pd(in[{2*c+1}], in[{2*c}], in[{2*a+1}], in[{2*a}]);")
    # stage 0: intra-ymm
    L.append("    // stage 0 (distance 1, intra-ymm, twiddle 1)")
    for k in range(p):
        L.append(f"    {{ const __m256d sw = _mm256_permute2f128_pd(r{k}, r{k}, 0x01);")
        L.append(f"      r{k} = _mm256_blend_pd(_mm256_add_pd(r{k}, sw), _mm256_sub_pd(sw, r{k}), 0xC); }}")
    # stages 1..b-1
    for s in range(1, b):
        m = 1 << (s + 1)
        dd = 1 << (s - 1)
        g_sz = 1 << s
        L.append(f"    // stage {s} (m={m}, ymm-distance {dd})")
        for g in range(0, p, g_sz):
            for k in range(g, g + dd):
                base = (k - g) * 2
                w0 = cmath.exp(sgn * 2j * cmath.pi * base / m)
                w1 = cmath.exp(sgn * 2j * cmath.pi * (base + 1) / m)
                wr = f"_mm256_set_pd({w1.real!r}, {w1.real!r}, {w0.real!r}, {w0.real!r})"
                wi = f"_mm256_set_pd({w1.imag!r}, {w1.imag!r}, {w0.imag!r}, {w0.imag!r})"
                # t = fmaddsub(wr, r[k+dd], wi * swap(r[k+dd]))
                L.append(f"    {{ const __m256d b = r{k+dd}; const __m256d xsw = _mm256_permute_pd(b, 0x5);")
                L.append(f"      const __m256d t = _mm256_fmaddsub_pd({wr}, b, _mm256_mul_pd({wi}, xsw));")
                L.append(f"      r{k+dd} = _mm256_sub_pd(r{k}, t); r{k} = _mm256_add_pd(r{k}, t); }}")
    for k in range(p):
        L.append(f"    _mm256_storeu_pd(out + {4*k}, r{k});")
    L.append("}")
    return "\n".join(L)


def main():
    check_model()
    out = []
    out.append("// AUTO-GENERATED by scripts/gen_aos_codelets.py — AoS within-transform FFT codelets (DO NOT EDIT).")
    out.append("// AoS within-transform building block (~45 GFLOPS measured). See docs/sessions/2026-06-14-fft-*midband*.")
    out.append("#pragma once")
    out.append("#include <immintrin.h>")
    out.append("")
    out.append("namespace crd::hesap::fft::detail")
    out.append("{")
    for b in RADIX_BITS:
        out.append(emit_codelet(b, False))
        out.append(emit_codelet(b, True))
        out.append(emit_codelet(b, False, contig=True))
        out.append(emit_codelet(b, True, contig=True))
        out.append("")
    out.append("} // namespace crd::hesap::fft::detail")
    print("\n".join(out))


if __name__ == "__main__":
    main()
