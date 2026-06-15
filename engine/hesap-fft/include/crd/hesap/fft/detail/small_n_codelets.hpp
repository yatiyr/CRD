#pragma once

// small_n_codelets.hpp — single-transform AoS "lane-trick" FFT leaf codelets for N ∈ {8,16,32}, f64 + AVX2.
//
// These operate in place on interleaved f64 data (the engine's native [re,im,…] layout), so they replace the
// SoA deinterleave → SoA-codelet → reinterleave round-trip the small-N leaf path used before, and are faster
// than that prior SoA path at these sizes. The single-transform path is latency-bound (the permute2f128
// lane-merge combine is a dependency chain); the batched N=8 path is throughput-bound and register-resident.
// (The full performance scoreboard, including external-library comparisons, lives in the FFT research dossier
// under docs/research/ — not in engine source.)
//
// Construction (DIT radix-2 with the even/odd decimation living in the two ymm LANES):
//   N = 2·H. A contiguous 256-bit load of [x[2e], x[2e+1]] puts the even subsequence in lane0 and the odd
//   subsequence in lane1, so the half-size sub-DFT (length H) runs "over-2" — two independent DFTs, one per
//   lane — with ZERO permute2f128 inside it (the two lanes are different sub-sequences, never combined). Then
//   a PAIRED lane-merge radix-2 combine emits two outputs per iteration via 256-bit stores (half the store
//   traffic of 128-bit scatters). This is the largest construction that keeps the whole state in 16 ymm.
//   Inverse = the conj trick: ifft_unnormalized(x)[k] = conj( fft( conj(x) )[k] ) — reuses the EXACT gated
//   forward path (forward twiddles unchanged), so the inverse inherits the forward's correctness. Unnormalized
//   to match the engine's codelet convention (the caller scales by 1/N).
//
// Lower-layer RAW f64 by ADR-0078 §5 (SIMD kernel; no Dim tag rides an _mm256 intrinsic). Guarded by the
// CrdSimd AVX2 backend; the SoA codelet path in fft.hpp is the fallback for SSE2/scalar/NEON and for f32.

#include <crd/core/types.hpp>
#include <crd/math/simd/backend.hpp>

#include <cmath>

namespace crd::hesap::fft::detail
{
#if CRD_SIMD_HAS_AVX2

// Precomputed twiddles for the lane-trick codelets. One shared, immutable, read-only table (cross-THREAD
// bit-identical by construction; not cross-compiler — sin/cos differ ±1 ULP). Built once via a magic static.
struct SmallNTwiddles
{
    __m256d w81r, w81i, w83r, w83i;               // size-8 split-radix leaf twiddles (W8^1, W8^3)
    __m256d w16r[4], w16i[4], w163r[4], w163i[4]; // size-16 split-radix leaf twiddles (W16^k, W16^3k)
    __m256d wp8r[2], wp8i[2];                     // N=8  combine pairs  [W8^{2j},  W8^{2j+1}]
    __m256d wp16r[4], wp16i[4];                   // N=16 combine pairs  [W16^{2j}, W16^{2j+1}]
    __m256d wp32r[8], wp32i[8];                   // N=32 combine pairs  [W32^{2j}, W32^{2j+1}]
};

[[nodiscard]] inline SmallNTwiddles make_small_n_twiddles() noexcept
{
    constexpr double pi = 3.141592653589793238;
    SmallNTwiddles t{};
    t.w81r = _mm256_set1_pd(std::cos(-2.0 * pi / 8.0));
    t.w81i = _mm256_set1_pd(std::sin(-2.0 * pi / 8.0));
    t.w83r = _mm256_set1_pd(std::cos(-2.0 * pi * 3.0 / 8.0));
    t.w83i = _mm256_set1_pd(std::sin(-2.0 * pi * 3.0 / 8.0));
    for (int k = 0; k < 4; ++k)
    {
        t.w16r[k] = _mm256_set1_pd(std::cos(-2.0 * pi * k / 16.0));
        t.w16i[k] = _mm256_set1_pd(std::sin(-2.0 * pi * k / 16.0));
        t.w163r[k] = _mm256_set1_pd(std::cos(-2.0 * pi * 3.0 * k / 16.0));
        t.w163i[k] = _mm256_set1_pd(std::sin(-2.0 * pi * 3.0 * k / 16.0));
    }
    // Combine pair j packs [W_N^{2j} | W_N^{2j+1}] across the two 128-bit lanes (set_pd is high→low).
    auto fill = [](int n, int count, __m256d* wr, __m256d* wi)
    {
        for (int j = 0; j < count; ++j)
        {
            const double c0 = std::cos(-2.0 * pi * (2 * j) / n);
            const double s0 = std::sin(-2.0 * pi * (2 * j) / n);
            const double c1 = std::cos(-2.0 * pi * (2 * j + 1) / n);
            const double s1 = std::sin(-2.0 * pi * (2 * j + 1) / n);
            wr[j] = _mm256_set_pd(c1, c1, c0, c0);
            wi[j] = _mm256_set_pd(s1, s1, s0, s0);
        }
    };
    fill(8, 2, t.wp8r, t.wp8i);
    fill(16, 4, t.wp16r, t.wp16i);
    fill(32, 8, t.wp32r, t.wp32i);
    return t;
}

[[nodiscard]] inline const SmallNTwiddles& small_n_twiddles() noexcept
{
    static const SmallNTwiddles kTw = make_small_n_twiddles();
    return kTw;
}

// Complex multiply of two interleaved [re,im|re,im] lanes by a broadcast/paired twiddle (wr,wi).
[[nodiscard]] inline __m256d vcmul(__m256d x, __m256d wr, __m256d wi) noexcept
{
    return _mm256_fmaddsub_pd(wr, x, _mm256_mul_pd(wi, _mm256_permute_pd(x, 0x5)));
}

// Multiply each complex lane by -i: (re,im) → (im,-re).
[[nodiscard]] inline __m256d vmul_neg_i(__m256d x) noexcept
{
    return _mm256_mul_pd(_mm256_permute_pd(x, 0x5), _mm256_set_pd(-1.0, 1.0, -1.0, 1.0));
}

// Conjugate each complex lane: (re,im) → (re,-im).
[[nodiscard]] inline __m256d vconj(__m256d x) noexcept
{
    return _mm256_mul_pd(x, _mm256_set_pd(-1.0, 1.0, -1.0, 1.0));
}

// Radix-2 butterfly (a,b) → (a+b, a-b).
inline void bfly(__m256d& a, __m256d& b) noexcept
{
    const __m256d t = b;
    b = _mm256_sub_pd(a, t);
    a = _mm256_add_pd(a, t);
}

// Radix-2 butterfly with a -i twist on b: (a,b) → (a + (-i)b, a - (-i)b).
inline void bfly_i(__m256d& a, __m256d& b) noexcept
{
    const __m256d t = vmul_neg_i(b);
    b = _mm256_sub_pd(a, t);
    a = _mm256_add_pd(a, t);
}

// Over-2 split-radix DFT-8 (lane0 / lane1 carry two independent transforms). Bit-reversed input → natural.
inline void dft8_over2(__m256d* d, const SmallNTwiddles& tw) noexcept
{
    __m256d e0 = d[0];
    __m256d e1 = d[1];
    __m256d e2 = d[2];
    __m256d e3 = d[3];
    bfly(e0, e1);
    bfly(e2, e3);
    bfly(e0, e2);
    bfly_i(e1, e3);
    __m256d z10 = d[4];
    __m256d z11 = d[5];
    bfly(z10, z11);
    __m256d z30 = d[6];
    __m256d z31 = d[7];
    bfly(z30, z31);
    const __m256d s0 = _mm256_add_pd(z10, z30);
    const __m256d p0 = _mm256_sub_pd(z10, z30);
    d[0] = _mm256_add_pd(e0, s0);
    d[4] = _mm256_sub_pd(e0, s0);
    d[2] = _mm256_add_pd(e2, vmul_neg_i(p0));
    d[6] = _mm256_sub_pd(e2, vmul_neg_i(p0));
    const __m256d t1 = vcmul(z11, tw.w81r, tw.w81i);
    const __m256d t3 = vcmul(z31, tw.w83r, tw.w83i);
    const __m256d s1 = _mm256_add_pd(t1, t3);
    const __m256d p1 = _mm256_sub_pd(t1, t3);
    d[1] = _mm256_add_pd(e1, s1);
    d[5] = _mm256_sub_pd(e1, s1);
    d[3] = _mm256_add_pd(e3, vmul_neg_i(p1));
    d[7] = _mm256_sub_pd(e3, vmul_neg_i(p1));
}

// Over-2 radix-4 DFT-4 (in place, lane0 / lane1 independent). Bit-reversed input → natural.
inline void dft4_over2(__m256d* g) noexcept
{
    bfly(g[0], g[1]);
    bfly(g[2], g[3]);
    bfly(g[0], g[2]);
    bfly_i(g[1], g[3]);
}

// Over-2 split-radix DFT-16 (lane0 / lane1 independent). Bit-reversed input → natural.
inline void dft16_over2(const __m256d* in, __m256d* out, const SmallNTwiddles& tw) noexcept
{
    __m256d u[8];
    for (int j = 0; j < 8; ++j)
    {
        u[j] = in[j];
    }
    dft8_over2(u, tw);
    __m256d z1[4];
    for (int j = 0; j < 4; ++j)
    {
        z1[j] = in[8 + j];
    }
    dft4_over2(z1);
    __m256d z3[4];
    for (int j = 0; j < 4; ++j)
    {
        z3[j] = in[12 + j];
    }
    dft4_over2(z3);
    for (int k = 0; k < 4; ++k)
    {
        const __m256d t1 = vcmul(z1[k], tw.w16r[k], tw.w16i[k]);
        const __m256d t3 = vcmul(z3[k], tw.w163r[k], tw.w163i[k]);
        const __m256d s = _mm256_add_pd(t1, t3);
        const __m256d p = _mm256_sub_pd(t1, t3);
        out[k] = _mm256_add_pd(u[k], s);
        out[k + 8] = _mm256_sub_pd(u[k], s);
        out[k + 4] = _mm256_add_pd(u[k + 4], vmul_neg_i(p));
        out[k + 12] = _mm256_sub_pd(u[k + 4], vmul_neg_i(p));
    }
}

// Single transform of length N = 2·H (N ∈ {8,16,32}), in/out interleaved f64 AoS (in == out is safe — every
// input is loaded into registers before any store). INV selects the conj-trick inverse (unnormalized).
template <int N, bool INV> inline void srn_lane(const double* xt, double* outt, const SmallNTwiddles& tw) noexcept
{
    constexpr int kHalf = N / 2;  // lane sub-DFT length
    constexpr int kPairs = N / 4; // paired combine iterations
    // Bit-reversal of the half-size index set (the even subsequence order fed to the over-2 sub-DFT).
    constexpr int ev4[4] = {0, 2, 1, 3};
    constexpr int ev8[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    constexpr int ev16[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

    __m256d ig[16];
    __m256d df[16];
    for (int i = 0; i < kHalf; ++i)
    {
        int e = 0;
        if constexpr (kHalf == 4)
        {
            e = ev4[i];
        }
        else if constexpr (kHalf == 8)
        {
            e = ev8[i];
        }
        else
        {
            e = ev16[i];
        }
        // One 256-bit load = [x[2e] | x[2e+1]] = [even-subseq element | odd-subseq element].
        const __m256d v = _mm256_loadu_pd(xt + (2 * e) * 2);
        ig[i] = INV ? vconj(v) : v;
    }

    if constexpr (kHalf == 4)
    {
        for (int i = 0; i < 4; ++i)
        {
            df[i] = ig[i];
        }
        dft4_over2(df);
    }
    else if constexpr (kHalf == 8)
    {
        dft8_over2(ig, tw);
        for (int i = 0; i < 8; ++i)
        {
            df[i] = ig[i];
        }
    }
    else
    {
        dft16_over2(ig, df, tw);
    }

    const __m256d* wr = nullptr;
    const __m256d* wi = nullptr;
    if constexpr (N == 8)
    {
        wr = tw.wp8r;
        wi = tw.wp8i;
    }
    else if constexpr (N == 16)
    {
        wr = tw.wp16r;
        wi = tw.wp16i;
    }
    else
    {
        wr = tw.wp32r;
        wi = tw.wp32i;
    }

    for (int j = 0; j < kPairs; ++j)
    {
        const __m256d a = df[2 * j];                           // [E[2j]   | O[2j]  ]
        const __m256d b = df[2 * j + 1];                       // [E[2j+1] | O[2j+1]]
        const __m256d ep = _mm256_permute2f128_pd(a, b, 0x20); // [E[2j] | E[2j+1]]
        const __m256d op = _mm256_permute2f128_pd(a, b, 0x31); // [O[2j] | O[2j+1]]
        const __m256d wo = vcmul(op, wr[j], wi[j]);            // forward twiddle (inverse uses conj-in/conj-out)
        const __m256d lo = _mm256_add_pd(ep, wo);
        const __m256d hi = _mm256_sub_pd(ep, wo);
        _mm256_storeu_pd(outt + (2 * j) * 2, INV ? vconj(lo) : lo);         // X[2j],   X[2j+1]
        _mm256_storeu_pd(outt + (2 * j + kHalf) * 2, INV ? vconj(hi) : hi); // X[2j+H], X[2j+H+1]
    }
}

// Engine entry: in-place single transform on interleaved f64 data, n ∈ {8,16,32}. `forward` false ⇒ the
// unnormalized inverse (caller scales by 1/n). For any other n this is a no-op (caller dispatches the SoA path).
inline void small_n_fft_f64(double* data, crd::usize n, bool forward) noexcept
{
    const SmallNTwiddles& tw = small_n_twiddles();
    if (n == 8)
    {
        if (forward)
        {
            srn_lane<8, false>(data, data, tw);
        }
        else
        {
            srn_lane<8, true>(data, data, tw);
        }
    }
    else if (n == 16)
    {
        if (forward)
        {
            srn_lane<16, false>(data, data, tw);
        }
        else
        {
            srn_lane<16, true>(data, data, tw);
        }
    }
    else if (n == 32)
    {
        if (forward)
        {
            srn_lane<32, false>(data, data, tw);
        }
        else
        {
            srn_lane<32, true>(data, data, tw);
        }
    }
}

// BATCHED N=8 (the v10-e fast path): `b` transforms in element-major layout (element i of transform t at
// i·b+t). Two adjacent transforms (t, t+1) at a fixed element are CONTIGUOUS ⇒ one 256-bit load = the over-2
// ymm [transform t | transform t+1]. The whole size-8 transform stays in 8 ymm ⇒ no spill, no intermediate
// memory pass (the SoA-vectorized-over-batch path pays log₂(8) full passes). Requires b even (caller guards).
// Inverse = the conj trick.
inline void small_n_batched8_f64(double* data, crd::usize b, bool forward) noexcept
{
    const SmallNTwiddles& tw = small_n_twiddles();
    constexpr crd::usize idx8[8] = {0, 4, 2, 6, 1, 5, 3, 7}; // bit-reversed size-8 input order
    for (crd::usize t = 0; t + 1 < b; t += 2)
    {
        __m256d ig[8];
        for (int j = 0; j < 8; ++j)
        {
            const __m256d v = _mm256_loadu_pd(data + (idx8[j] * b + t) * 2);
            ig[j] = forward ? v : vconj(v);
        }
        dft8_over2(ig, tw);
        for (crd::usize k = 0; k < 8; ++k)
        {
            const __m256d r = forward ? ig[k] : vconj(ig[k]);
            _mm256_storeu_pd(data + (k * b + t) * 2, r);
        }
    }
}

// BATCHED N=16: same over-2 construction as N=8 (two adjacent transforms share a 256-bit ymm), driven by the
// split-radix `dft16_over2` register-resident leaf — the whole size-16 transform stays in registers, no
// log₂(n) memory passes. `sig16` is the split-radix input order (even half bit-reversed-8 = 2·{0,4,2,6,1,5,3,7};
// odd quarters {1,9,5,13} and {3,11,7,15}). Requires b even; the caller ALSO guards the working set to stay
// cache-resident (the strided over-2 gather streams ~n·b complex per transform-pair, so very large batches
// fall back to the SoA path). Inverse = the conj trick.
inline void small_n_batched16_f64(double* data, crd::usize b, bool forward) noexcept
{
    const SmallNTwiddles& tw = small_n_twiddles();
    constexpr crd::usize sig16[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    for (crd::usize t = 0; t + 1 < b; t += 2)
    {
        __m256d ig[16];
        __m256d og[16];
        for (int j = 0; j < 16; ++j)
        {
            const __m256d v = _mm256_loadu_pd(data + (sig16[j] * b + t) * 2);
            ig[j] = forward ? v : vconj(v);
        }
        dft16_over2(ig, og, tw);
        for (crd::usize k = 0; k < 16; ++k)
        {
            const __m256d r = forward ? og[k] : vconj(og[k]);
            _mm256_storeu_pd(data + (k * b + t) * 2, r);
        }
    }
}

#endif // CRD_SIMD_HAS_AVX2
} // namespace crd::hesap::fft::detail
