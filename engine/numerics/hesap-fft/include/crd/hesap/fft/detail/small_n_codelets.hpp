#pragma once

// small_n_codelets.hpp — single-transform AoS "lane-trick" FFT leaf codelets for N ∈ {8,16,32}, f64 on Vec4d.
//
// These operate in place on interleaved f64 data (the engine's native [re,im,…] layout), so they replace the
// SoA deinterleave → SoA-codelet → reinterleave round-trip the small-N leaf path used before, and are faster
// than that prior SoA path at these sizes. The single-transform path is latency-bound (the 128-bit-half
// lane-merge combine is a dependency chain); the batched N=8 path is throughput-bound and register-resident.
// The batched paths run per complex lane, four transforms a Vec8d at avx512. The single transform stays on Vec4d at
// every level: its two halves are its even and odd subsequences, so a wider register is another decomposition.
// (The full performance scoreboard, including external-library comparisons, lives in the FFT research dossier
// under docs/research/ — not in engine source.)
//
// Construction (DIT radix-2 with the even/odd decimation living in the two 128-bit HALVES of a Vec4d):
//   N = 2·H. A contiguous 4-lane load of [x[2e], x[2e+1]] puts the even subsequence in the low half and the odd
//   subsequence in the high half, so the half-size sub-DFT (length H) runs "over-2" — two independent DFTs, one
//   per half — with ZERO cross-half shuffle inside it (the two halves are different sub-sequences, never
//   combined). Then a PAIRED lane-merge radix-2 combine emits two outputs per iteration via 4-lane stores (half
//   the store traffic of 2-lane scatters). This is the largest construction that keeps the whole state in 16 registers.
//   Inverse = the conj trick: ifft_unnormalized(x)[k] = conj( fft( conj(x) )[k] ) — reuses the EXACT forward
//   path (forward twiddles unchanged), so the inverse inherits the forward's correctness. Unnormalized to match
//   the engine's codelet convention (the caller scales by 1/N).
//
// Lower-layer RAW f64 by ADR-0078 §5 (SIMD kernel; no Dim tag rides a SIMD lane). Every op is exact per lane on
// every backend (shuffles, IEEE add/sub/mul, the single-rounded fmaddsub), so each kernel unit gives the same bits
// (kernels/fft_kernels.cpp). The twiddles are plain data (SmallNTwiddles, fft_kernels.hpp) built once in general code.

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/fft/fft_kernels.hpp>
#include <crd/math/simd/vec4d.hpp>
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec8d.hpp>
#endif

namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS
{

using SmallNVec = crd::math::simd::Vec4d;
// The batched codelets' step: two transforms of the batch, four in one Vec8d at avx512. The leaf codelets below
// run per complex lane, so a step's width never changes a transform's bits.
#if CRD_SIMD_HAS_AVX512
using SmallNWide = crd::math::simd::Vec8d;
#else
using SmallNWide = SmallNVec;
#endif

// A Vec4d's four lanes on every Vec4d of V.
template <typename V> [[nodiscard]] inline V small_n_lanes(SmallNVec x) noexcept
{
    if constexpr (sizeof(V) == sizeof(SmallNVec))
    {
        return x;
    }
    else
    {
        return V(x, x);
    }
}

// A leaf twiddle of SmallNTwiddles: the same complex value on every complex lane.
template <typename V> [[nodiscard]] inline V leaf_twiddle(const crd::f64* p) noexcept
{
    return small_n_lanes<V>(SmallNVec::load_aligned(p));
}

// Complex multiply of interleaved [re,im|re,im] lanes by a broadcast/paired twiddle (wr,wi).
template <typename V> [[nodiscard]] inline V vcmul(V x, V wr, V wi) noexcept
{
    return crd::math::simd::fmaddsub(wr, x, wi * crd::math::simd::swap_pairs(x));
}

// Multiply each complex lane by -i: (re,im) → (im,-re).
template <typename V> [[nodiscard]] inline V vmul_neg_i(V x) noexcept
{
    return crd::math::simd::swap_pairs(x) * small_n_lanes<V>(SmallNVec(1.0, -1.0, 1.0, -1.0));
}

// Conjugate each complex lane: (re,im) → (re,-im).
template <typename V> [[nodiscard]] inline V vconj(V x) noexcept
{
    return x * small_n_lanes<V>(SmallNVec(1.0, -1.0, 1.0, -1.0));
}

// Radix-2 butterfly (a,b) → (a+b, a-b).
template <typename V> inline void bfly(V& a, V& b) noexcept
{
    const V t = b;
    b = a - t;
    a = a + t;
}

// Radix-2 butterfly with a -i twist on b: (a,b) → (a + (-i)b, a - (-i)b).
template <typename V> inline void bfly_i(V& a, V& b) noexcept
{
    const V t = vmul_neg_i(b);
    b = a - t;
    a = a + t;
}

// Over-2 split-radix DFT-8 (each complex lane carries its own transform, two a Vec4d). Bit-reversed input → natural.
template <typename V> inline void dft8_over2(V* d, const SmallNTwiddles& tw) noexcept
{
    V e0 = d[0];
    V e1 = d[1];
    V e2 = d[2];
    V e3 = d[3];
    bfly(e0, e1);
    bfly(e2, e3);
    bfly(e0, e2);
    bfly_i(e1, e3);
    V z10 = d[4];
    V z11 = d[5];
    bfly(z10, z11);
    V z30 = d[6];
    V z31 = d[7];
    bfly(z30, z31);
    const V s0 = z10 + z30;
    const V p0 = z10 - z30;
    d[0] = e0 + s0;
    d[4] = e0 - s0;
    d[2] = e2 + vmul_neg_i(p0);
    d[6] = e2 - vmul_neg_i(p0);
    const V t1 = vcmul(z11, leaf_twiddle<V>(tw.w81r), leaf_twiddle<V>(tw.w81i));
    const V t3 = vcmul(z31, leaf_twiddle<V>(tw.w83r), leaf_twiddle<V>(tw.w83i));
    const V s1 = t1 + t3;
    const V p1 = t1 - t3;
    d[1] = e1 + s1;
    d[5] = e1 - s1;
    d[3] = e3 + vmul_neg_i(p1);
    d[7] = e3 - vmul_neg_i(p1);
}

// Over-2 radix-4 DFT-4 (in place, every complex lane independent). Bit-reversed input → natural.
template <typename V> inline void dft4_over2(V* g) noexcept
{
    bfly(g[0], g[1]);
    bfly(g[2], g[3]);
    bfly(g[0], g[2]);
    bfly_i(g[1], g[3]);
}

// Over-2 split-radix DFT-16 (every complex lane independent). Bit-reversed input → natural.
template <typename V> inline void dft16_over2(const V* in, V* out, const SmallNTwiddles& tw) noexcept
{
    V u[8];
    for (int j = 0; j < 8; ++j)
    {
        u[j] = in[j];
    }
    dft8_over2(u, tw);
    V z1[4];
    for (int j = 0; j < 4; ++j)
    {
        z1[j] = in[8 + j];
    }
    dft4_over2(z1);
    V z3[4];
    for (int j = 0; j < 4; ++j)
    {
        z3[j] = in[12 + j];
    }
    dft4_over2(z3);
    for (int k = 0; k < 4; ++k)
    {
        const V t1 = vcmul(z1[k], leaf_twiddle<V>(tw.w16r[k]), leaf_twiddle<V>(tw.w16i[k]));
        const V t3 = vcmul(z3[k], leaf_twiddle<V>(tw.w163r[k]), leaf_twiddle<V>(tw.w163i[k]));
        const V s = t1 + t3;
        const V p = t1 - t3;
        out[k] = u[k] + s;
        out[k + 8] = u[k] - s;
        out[k + 4] = u[k + 4] + vmul_neg_i(p);
        out[k + 12] = u[k + 4] - vmul_neg_i(p);
    }
}

// Single transform of length N = 2·H (N ∈ {8,16,32}), in/out interleaved f64 AoS (in == out is safe — every
// input is loaded into registers before any store). INV selects the conj-trick inverse (unnormalized).
template <int N, bool INV> inline void srn_lane(const double* xt, double* outt, const SmallNTwiddles& tw) noexcept
{
    constexpr int kHalf = N / 2;  // half sub-DFT length
    constexpr int kPairs = N / 4; // paired combine iterations
    // Bit-reversal of the half-size index set (the even subsequence order fed to the over-2 sub-DFT).
    constexpr int ev4[4] = {0, 2, 1, 3};
    constexpr int ev8[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    constexpr int ev16[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

    SmallNVec ig[16];
    SmallNVec df[16];
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
        // One 4-lane load = [x[2e] | x[2e+1]] = [even-subseq element | odd-subseq element].
        const SmallNVec v = SmallNVec::load(xt + (2 * e) * 2);
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

    const crd::f64(*wr)[4] = nullptr;
    const crd::f64(*wi)[4] = nullptr;
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
        const SmallNVec a = df[2 * j];                         // [E[2j]   | O[2j]  ]
        const SmallNVec b = df[2 * j + 1];                     // [E[2j+1] | O[2j+1]]
        const SmallNVec ep = crd::math::simd::concat_lo(a, b); // [E[2j] | E[2j+1]]
        const SmallNVec op = crd::math::simd::concat_hi(a, b); // [O[2j] | O[2j+1]]
        // forward twiddle (inverse uses conj-in/conj-out)
        const SmallNVec wo = vcmul(op, SmallNVec::load_aligned(wr[j]), SmallNVec::load_aligned(wi[j]));
        const SmallNVec lo = ep + wo;
        const SmallNVec hi = ep - wo;
        (INV ? vconj(lo) : lo).store(outt + (2 * j) * 2);         // X[2j],   X[2j+1]
        (INV ? vconj(hi) : hi).store(outt + (2 * j + kHalf) * 2); // X[2j+H], X[2j+H+1]
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

// run<V>(t) over the transforms of an even batch b: four a step on SmallNWide at avx512, then a last pair on
// SmallNVec, so every transform takes the same operations at every level.
template <typename Run> inline void for_transform_steps(crd::usize b, Run run) noexcept
{
    constexpr crd::usize wide = sizeof(SmallNWide) / (2 * sizeof(crd::f64));
    crd::usize t = 0;
    for (; t + wide <= b; t += wide)
    {
        run.template operator()<SmallNWide>(t);
    }
    if constexpr (wide != 2U)
    {
        if (t + 1 < b)
        {
            run.template operator()<SmallNVec>(t);
        }
    }
}

// BATCHED N=8 (the v10-e fast path): `b` transforms in element-major layout (element i of transform t at
// i·b+t). Adjacent transforms (t, t+1, ...) at a fixed element are CONTIGUOUS ⇒ one load = the over-2 vector
// [transform t | transform t+1 | ...]. The whole size-8 transform stays in 8 vectors ⇒ no spill, no intermediate
// memory pass (the SoA-vectorized-over-batch path pays log₂(8) full passes). Requires b even (caller guards).
// Inverse = the conj trick.
inline void small_n_batched8_f64(double* data, crd::usize b, bool forward) noexcept
{
    const SmallNTwiddles& tw = small_n_twiddles();
    for_transform_steps(b,
                        [&]<typename V>(crd::usize t) noexcept
                        {
                            constexpr crd::usize idx8[8] = {0, 4, 2, 6, 1, 5, 3, 7}; // bit-reversed size-8 order
                            V ig[8];
                            for (int j = 0; j < 8; ++j)
                            {
                                const V v = V::load(data + (idx8[j] * b + t) * 2);
                                ig[j] = forward ? v : vconj(v);
                            }
                            dft8_over2(ig, tw);
                            for (crd::usize k = 0; k < 8; ++k)
                            {
                                (forward ? ig[k] : vconj(ig[k])).store(data + (k * b + t) * 2);
                            }
                        });
}

// BATCHED N=16: same over-2 construction as N=8 (adjacent transforms share one vector), driven by the
// split-radix `dft16_over2` register-resident leaf — the whole size-16 transform stays in registers, no
// log₂(n) memory passes. `sig16` is the split-radix input order (even half bit-reversed-8 = 2·{0,4,2,6,1,5,3,7};
// odd quarters {1,9,5,13} and {3,11,7,15}). Requires b even; the caller ALSO guards the working set to stay
// cache-resident (the strided over-2 gather streams ~n·b complex per transform-pair, so very large batches
// fall back to the SoA path). Inverse = the conj trick.
inline void small_n_batched16_f64(double* data, crd::usize b, bool forward) noexcept
{
    const SmallNTwiddles& tw = small_n_twiddles();
    for_transform_steps(b,
                        [&]<typename V>(crd::usize t) noexcept
                        {
                            constexpr crd::usize sig16[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
                            V ig[16];
                            V og[16];
                            for (int j = 0; j < 16; ++j)
                            {
                                const V v = V::load(data + (sig16[j] * b + t) * 2);
                                ig[j] = forward ? v : vconj(v);
                            }
                            dft16_over2(ig, og, tw);
                            for (crd::usize k = 0; k < 16; ++k)
                            {
                                (forward ? og[k] : vconj(og[k])).store(data + (k * b + t) * 2);
                            }
                        });
}

} // namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS
