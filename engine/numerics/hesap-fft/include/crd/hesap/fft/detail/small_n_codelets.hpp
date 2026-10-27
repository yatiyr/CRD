#pragma once

// small_n_codelets.hpp — single-transform AoS "lane-trick" FFT leaf codelets for N ∈ {8,16,32}, f64 on Vec4d.
//
// These operate in place on interleaved f64 data (the engine's native [re,im,…] layout), so they replace the
// SoA deinterleave → SoA-codelet → reinterleave round-trip the small-N leaf path used before, and are faster
// than that prior SoA path at these sizes. The single-transform path is latency-bound (the 128-bit-half
// lane-merge combine is a dependency chain); the batched N=8 path is throughput-bound and register-resident.
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

#include <crd/core/types.hpp>
#include <crd/hesap/fft/fft_kernels.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/simd/vec4d.hpp>

namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS
{

using SmallNVec = crd::math::simd::Vec4d;

// Complex multiply of two interleaved [re,im|re,im] lanes by a broadcast/paired twiddle (wr,wi).
[[nodiscard]] inline SmallNVec vcmul(SmallNVec x, SmallNVec wr, SmallNVec wi) noexcept
{
    return crd::math::simd::fmaddsub(wr, x, wi * crd::math::simd::swap_pairs(x));
}

// Multiply each complex lane by -i: (re,im) → (im,-re).
[[nodiscard]] inline SmallNVec vmul_neg_i(SmallNVec x) noexcept
{
    return crd::math::simd::swap_pairs(x) * SmallNVec(1.0, -1.0, 1.0, -1.0);
}

// Conjugate each complex lane: (re,im) → (re,-im).
[[nodiscard]] inline SmallNVec vconj(SmallNVec x) noexcept
{
    return x * SmallNVec(1.0, -1.0, 1.0, -1.0);
}

// Radix-2 butterfly (a,b) → (a+b, a-b).
inline void bfly(SmallNVec& a, SmallNVec& b) noexcept
{
    const SmallNVec t = b;
    b = a - t;
    a = a + t;
}

// Radix-2 butterfly with a -i twist on b: (a,b) → (a + (-i)b, a - (-i)b).
inline void bfly_i(SmallNVec& a, SmallNVec& b) noexcept
{
    const SmallNVec t = vmul_neg_i(b);
    b = a - t;
    a = a + t;
}

// Over-2 split-radix DFT-8 (the two halves carry two independent transforms). Bit-reversed input → natural.
inline void dft8_over2(SmallNVec* d, const SmallNTwiddles& tw) noexcept
{
    SmallNVec e0 = d[0];
    SmallNVec e1 = d[1];
    SmallNVec e2 = d[2];
    SmallNVec e3 = d[3];
    bfly(e0, e1);
    bfly(e2, e3);
    bfly(e0, e2);
    bfly_i(e1, e3);
    SmallNVec z10 = d[4];
    SmallNVec z11 = d[5];
    bfly(z10, z11);
    SmallNVec z30 = d[6];
    SmallNVec z31 = d[7];
    bfly(z30, z31);
    const SmallNVec s0 = z10 + z30;
    const SmallNVec p0 = z10 - z30;
    d[0] = e0 + s0;
    d[4] = e0 - s0;
    d[2] = e2 + vmul_neg_i(p0);
    d[6] = e2 - vmul_neg_i(p0);
    const SmallNVec t1 = vcmul(z11, SmallNVec::load_aligned(tw.w81r), SmallNVec::load_aligned(tw.w81i));
    const SmallNVec t3 = vcmul(z31, SmallNVec::load_aligned(tw.w83r), SmallNVec::load_aligned(tw.w83i));
    const SmallNVec s1 = t1 + t3;
    const SmallNVec p1 = t1 - t3;
    d[1] = e1 + s1;
    d[5] = e1 - s1;
    d[3] = e3 + vmul_neg_i(p1);
    d[7] = e3 - vmul_neg_i(p1);
}

// Over-2 radix-4 DFT-4 (in place, the two halves independent). Bit-reversed input → natural.
inline void dft4_over2(SmallNVec* g) noexcept
{
    bfly(g[0], g[1]);
    bfly(g[2], g[3]);
    bfly(g[0], g[2]);
    bfly_i(g[1], g[3]);
}

// Over-2 split-radix DFT-16 (the two halves independent). Bit-reversed input → natural.
inline void dft16_over2(const SmallNVec* in, SmallNVec* out, const SmallNTwiddles& tw) noexcept
{
    SmallNVec u[8];
    for (int j = 0; j < 8; ++j)
    {
        u[j] = in[j];
    }
    dft8_over2(u, tw);
    SmallNVec z1[4];
    for (int j = 0; j < 4; ++j)
    {
        z1[j] = in[8 + j];
    }
    dft4_over2(z1);
    SmallNVec z3[4];
    for (int j = 0; j < 4; ++j)
    {
        z3[j] = in[12 + j];
    }
    dft4_over2(z3);
    for (int k = 0; k < 4; ++k)
    {
        const SmallNVec t1 = vcmul(z1[k], SmallNVec::load_aligned(tw.w16r[k]), SmallNVec::load_aligned(tw.w16i[k]));
        const SmallNVec t3 = vcmul(z3[k], SmallNVec::load_aligned(tw.w163r[k]), SmallNVec::load_aligned(tw.w163i[k]));
        const SmallNVec s = t1 + t3;
        const SmallNVec p = t1 - t3;
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

// BATCHED N=8 (the v10-e fast path): `b` transforms in element-major layout (element i of transform t at
// i·b+t). Two adjacent transforms (t, t+1) at a fixed element are CONTIGUOUS ⇒ one 4-lane load = the over-2
// vector [transform t | transform t+1]. The whole size-8 transform stays in 8 vectors ⇒ no spill, no intermediate
// memory pass (the SoA-vectorized-over-batch path pays log₂(8) full passes). Requires b even (caller guards).
// Inverse = the conj trick.
inline void small_n_batched8_f64(double* data, crd::usize b, bool forward) noexcept
{
    const SmallNTwiddles& tw = small_n_twiddles();
    constexpr crd::usize idx8[8] = {0, 4, 2, 6, 1, 5, 3, 7}; // bit-reversed size-8 input order
    for (crd::usize t = 0; t + 1 < b; t += 2)
    {
        SmallNVec ig[8];
        for (int j = 0; j < 8; ++j)
        {
            const SmallNVec v = SmallNVec::load(data + (idx8[j] * b + t) * 2);
            ig[j] = forward ? v : vconj(v);
        }
        dft8_over2(ig, tw);
        for (crd::usize k = 0; k < 8; ++k)
        {
            (forward ? ig[k] : vconj(ig[k])).store(data + (k * b + t) * 2);
        }
    }
}

// BATCHED N=16: same over-2 construction as N=8 (two adjacent transforms share one vector), driven by the
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
        SmallNVec ig[16];
        SmallNVec og[16];
        for (int j = 0; j < 16; ++j)
        {
            const SmallNVec v = SmallNVec::load(data + (sig16[j] * b + t) * 2);
            ig[j] = forward ? v : vconj(v);
        }
        dft16_over2(ig, og, tw);
        for (crd::usize k = 0; k < 16; ++k)
        {
            (forward ? og[k] : vconj(og[k])).store(data + (k * b + t) * 2);
        }
    }
}

} // namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS
