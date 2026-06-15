#pragma once

// fft.hpp — Phase 3.1.6 v10: complex FFT. The DETERMINISTIC-PLAN contract (the v10 thesis): the algorithm
// is chosen purely from the size factorization — NO runtime measurement (unlike FFTW MEASURE/PATIENT, which
// varies run-to-run + whose wisdom isn't reproducible across builds). One twiddle table is precomputed per
// plan and shared ⇒ cross-THREAD bit-identical (NOT cross-compiler: sin/cos ±1 ULP). Lower-layer RAW per
// ADR-0078 (Complex<f32/f64>).
//
// v10-a: radix-2 (the correct baseline + the brute-force-DFT gate). v10-b: STOCKHAM AUTOSORT (no
// bit-reversal — kills the large-N cache collapse) on SPLIT SoA buffers + AVX2 SIMD over the unit-stride
// butterfly index ⇒ the beat-MKL throughput. The radix-2 in-place stays as `execute_reference` — the
// trusted oracle the Stockham path is cross-checked against. (radix-4/8 codelets + Bailey four-step extend
// this; mixed-radix → v10-c-prep, Bluestein/Rader → v10-c.)
//
// Normalization (FFTW convention): forward + inverse BOTH UNNORMALIZED; `ifft_normalized` for 1/n.
// THREADING: FftPlan owns its scratch ⇒ one plan per thread (plans are cheap + deterministic to build;
// batched-parallel uses per-worker plans, v10-e).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/detail/codelets.hpp> // generated straight-line leaf codelets (genfft-lite, v10-b)
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h> // non-temporal stores for the four-step scatter (Lever D resurrection)
#include <type_traits>

#ifdef CRD_FFT_PROFILE
#include <cstdio>
#endif

namespace crd::hesap::fft
{

#ifdef CRD_FFT_PROFILE
namespace prof
{
inline unsigned long long g_first = 0, g_combine = 0, g_last = 0;
inline long g_calls = 0;
inline unsigned long long rdtsc() noexcept
{
    unsigned int lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<unsigned long long>(hi) << 32) | lo;
}
inline void dump() noexcept
{
    const double c = static_cast<double>(g_calls > 0 ? g_calls : 1);
    std::fprintf(stderr, "[fft-prof] first=%.0f combine=%.0f last=%.0f cyc/call (%ld calls)\n",
                 static_cast<double>(g_first) / c, static_cast<double>(g_combine) / c,
                 static_cast<double>(g_last) / c, g_calls);
    g_first = g_combine = g_last = 0;
    g_calls = 0;
}
} // namespace prof
#endif

enum class FftDirection : crd::u8
{
    Forward = 0,
    Inverse = 1,
};

[[nodiscard]] constexpr bool is_pow2(crd::usize n) noexcept
{
    return n != 0 && (n & (n - 1)) == 0;
}

template <typename T> class FftPlan
{
public:
    FftPlan(crd::memory::IAllocator* alloc, crd::usize n)
        : m_alloc(alloc), m_n(n), m_log2(0), m_tw_re(alloc), m_tw_im(alloc), m_rev(alloc), m_re0(alloc), m_im0(alloc),
          m_re1(alloc), m_im1(alloc), m_abuf(alloc), m_bbuf(alloc), m_ftw_hi_re(alloc), m_ftw_hi_im(alloc),
          m_ftw_lo_re(alloc), m_ftw_lo_im(alloc), m_ptw_re(alloc), m_ptw_im(alloc)
    {
        CRD_ASSERT(is_pow2(n)); // v10: power-of-2 (Bluestein/Rader make any size O(n log n) at v10-c)
        while ((crd::usize{1} << m_log2) < n)
        {
            ++m_log2;
        }
        // Above kFourStepMin the direct Stockham is DRAM-bound (log4(n) full passes); switch to the Bailey
        // four-step (n = n1·n2 ≈ √n × √n) so the sub-FFTs are cache-resident — O(1) DRAM passes, not O(log n).
        m_use_four_step = (n >= kFourStepMin);
        m_n1 = crd::usize{1} << (m_log2 / 2); // ≈ √n; n2 = n / n1, both powers of 2
        // Full table W_n^k, k = 0 .. n-1 (radix-4 indexes up to 3·j·r < n). Precomputed ONCE, shared — the
        // determinism contract. Computed in f64 then narrowed (accuracy for the f32 plan).
        m_tw_re.resize(n);
        m_tw_im.resize(n);
        constexpr double two_pi = 6.283185307179586476925286766559;
        for (crd::usize k = 0; k < n; ++k)
        {
            const double ang = -two_pi * static_cast<double>(k) / static_cast<double>(n);
            m_tw_re[k] = static_cast<T>(std::cos(ang)); //  cos(2πk/n)
            m_tw_im[k] = static_cast<T>(std::sin(ang)); // -sin(2πk/n) ⇒ (re,im) == W_n^k
        }
        m_rev.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_rev[i] = bitrev(i, m_log2);
        }
        if (m_use_four_step) // Lever D resurrection: four-step buffers + HOISTED sub-plans + linear twiddle
        {
            // tbuf (transpose intermediate) + scratch (cache-resident block) are RAW 64-BYTE-ALIGNED (NT stores
            // need ≥32B; crd Array gives only 8B). Freed in the dtor. m_abuf/m_bbuf left empty (unused now).
            m_tbuf = static_cast<Complex<T>*>(m_alloc->allocate(n * sizeof(Complex<T>), 64));
            m_scratch = static_cast<Complex<T>*>(m_alloc->allocate(n * sizeof(Complex<T>), 64));
            const crd::usize n2 = n / m_n1;
            // Two ~√n twiddle tables (the read-floor cut): W_n^a = W_n^{a_hi·M}·W_n^{a_lo}, M = 1<<m_ftw_h.
            // h = ⌊log2(n)/2⌋ ⇒ lo size M ≈ √n, hi size n/M ≈ √n; together ~96 KB @8M (L2-resident, not a
            // 128 MB streaming read). Both tables hold the (cos,−sin) convention so their product is W_n^a.
            m_ftw_h = m_log2 / 2U;
            const crd::usize m_split = crd::usize{1} << m_ftw_h; // M
            const crd::usize hi_count = n / m_split;             // n/M
            m_ftw_lo_re.resize(m_split);
            m_ftw_lo_im.resize(m_split);
            for (crd::usize j = 0; j < m_split; ++j)
            {
                m_ftw_lo_re[j] = m_tw_re[j]; // W_n^j
                m_ftw_lo_im[j] = m_tw_im[j];
            }
            m_ftw_hi_re.resize(hi_count);
            m_ftw_hi_im.resize(hi_count);
            for (crd::usize j = 0; j < hi_count; ++j)
            {
                m_ftw_hi_re[j] = m_tw_re[j * m_split]; // W_n^{j·M}
                m_ftw_hi_im[j] = m_tw_im[j * m_split];
            }
            m_p1 = static_cast<FftPlan<T>*>(m_alloc->allocate(sizeof(FftPlan<T>), alignof(FftPlan<T>)));
            ::new (static_cast<void*>(m_p1)) FftPlan<T>(m_alloc, m_n1);
            m_p2 = static_cast<FftPlan<T>*>(m_alloc->allocate(sizeof(FftPlan<T>), alignof(FftPlan<T>)));
            ::new (static_cast<void*>(m_p2)) FftPlan<T>(m_alloc, n2);
        }
        else // direct radix path: SoA ping-pong scratch + Lever A per-pass combine twiddles
        {
            m_re0.resize(n);
            m_im0.resize(n);
            m_re1.resize(n);
            m_im1.resize(n);
            m_rmax_bits = (m_log2 >= 15U && m_log2 <= 20U) ? 5U : (m_log2 >= 12U ? 4U : 3U);
            build_combine_twiddles();
        }
    }

    // Owns the hoisted four-step sub-plans + raw aligned buffers ⇒ the plan is PINNED (never copied/moved
    // in-tree; verified). Deleting copy+move makes any accidental copy a compile error, not a double-free.
    ~FftPlan()
    {
        if (m_p2 != nullptr) { m_p2->~FftPlan(); m_alloc->deallocate(m_p2); }
        if (m_p1 != nullptr) { m_p1->~FftPlan(); m_alloc->deallocate(m_p1); }
        if (m_scratch != nullptr) { m_alloc->deallocate(m_scratch); }
        if (m_tbuf != nullptr) { m_alloc->deallocate(m_tbuf); }
    }
    FftPlan(const FftPlan&) = delete;
    FftPlan& operator=(const FftPlan&) = delete;
    FftPlan(FftPlan&&) = delete;
    FftPlan& operator=(FftPlan&&) = delete;

    // Build the flat per-pass twiddle table (Lever A). Replays the SAME size-aware planner as `execute`, so the
    // table is laid out in pass order; `execute` walks it with a running pointer. m*j*r ≤ n-1 (bounded).
    void build_combine_twiddles()
    {
        if (m_log2 < 3U) // n < 8: leaf-codelet path, no combine passes (also guards m_log2−2 underflow)
        {
            return;
        }
        crd::usize total = 0;
        for (crd::u32 t = (m_log2 & 1U) ? 1U : 2U; t < m_log2 - 2U;)
        {
            const crd::u32 rem = (m_log2 - 2U) - t;
            crd::u32 b = m_rmax_bits < rem ? m_rmax_bits : rem;
            while (b > 2U && (rem - b) == 1U)
            {
                --b;
            }
            total += (crd::usize{1} << t) * ((crd::usize{1} << b) - 1);
            t += b;
        }
        if (total == 0)
        {
            return;
        }
        m_ptw_re.resize(total);
        m_ptw_im.resize(total);
        crd::usize off = 0;
        for (crd::u32 t = (m_log2 & 1U) ? 1U : 2U; t < m_log2 - 2U;)
        {
            const crd::u32 rem = (m_log2 - 2U) - t;
            crd::u32 b = m_rmax_bits < rem ? m_rmax_bits : rem;
            while (b > 2U && (rem - b) == 1U)
            {
                --b;
            }
            const crd::usize lq = crd::usize{1} << t;
            const crd::usize radix = crd::usize{1} << b;
            const crd::usize r = m_n >> (t + b);
            for (crd::usize j = 0; j < lq; ++j)
            {
                for (crd::usize m = 1; m < radix; ++m)
                {
                    const crd::usize idx = m * j * r;
                    m_ptw_re[off] = m_tw_re[idx];
                    m_ptw_im[off] = m_tw_im[idx];
                    ++off;
                }
            }
            t += b;
        }
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_n; }

    // The PRODUCTION transform: Stockham autosort radix-2 on split SoA buffers, AVX2-vectorized over the
    // unit-stride butterfly index. In-place from the caller's view (deinterleave → SoA → reinterleave).
    void execute(crd::containers::Span<Complex<T>> data, FftDirection dir) const
    {
        CRD_ASSERT(data.size() == m_n);
        if (m_n <= 1)
        {
            return;
        }
        if (m_n <= 32) // leaf-codelet fast path: one straight-line block, zero loop overhead (L1-resident)
        {
            T* re = m_re0.data();
            T* im = m_im0.data();
            for (crd::usize i = 0; i < m_n; ++i)
            {
                re[i] = data[i].re;
                im[i] = data[i].im;
            }
            dispatch_codelet(re, im, dir);
            for (crd::usize i = 0; i < m_n; ++i)
            {
                data[i].re = re[i];
                data[i].im = im[i];
            }
            return;
        }
        if (m_use_four_step)
        {
            execute_four_step(data, dir);
            return;
        }
        // NB (2026-06-14): the depth-first recursion (rec_fft_soa) was MEASURED for the mid-band and is dead —
        // worse than this Stockham path at every size (8192 −6%, 65536 −27%, 131072 −33% collapsing). Decisive:
        // at n=1024 (fully L1-resident, ZERO cache penalty) this codelet path is ~0.46× MKL — so the mid-band
        // wall is CODELET QUALITY in isolation, not cache structure; the lever is gen_fft_codelets.py (modified
        // split-radix + scheduling), not a recursive driver. rec_fft_soa kept as a seed only.
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        T* xr = m_re0.data();
        T* xi = m_im0.data();
        T* yr = m_re1.data();
        T* yi = m_im1.data();
        const Complex<T>* din = data.data();
        // RADIX-4 Stockham (half the passes of radix-2 ⇒ half the memory traffic, the pass-bound lever),
        // with a single radix-2 pass FIRST when log2(n) is odd. Each pass ping-pongs x↔y.
        // FUSED interleave: the FIRST pass reads the interleaved `data` straight into the split buffers (its
        // twiddles are trivial, j=0 ⇒ w=1) and the LAST radix-4 pass (r=1, already scalar) writes the split
        // buffers back to interleaved `data`. This folds away the standalone deinterleave + reinterleave
        // passes — 2 full memory passes that MKL/FFTW/PocketFFT (in-place interleaved) never pay either.
        crd::u32 t = 0; // log2 of the sub-DFT size already built
        auto swap_buffers = [&]
        {
            T* tr = xr;
            xr = yr;
            yr = tr;
            T* ti = xi;
            xi = yi;
            yi = ti;
        };
#ifdef CRD_FFT_PROFILE
        const unsigned long long pf0 = prof::rdtsc();
#endif
        if ((m_log2 & 1U) != 0U)
        {
            radix2_first_interleaved(din, xr, xi); // result lands in xr/xi
            t = 1;
        }
        else
        {
            radix4_first_interleaved(din, xr, xi, isign); // result lands in xr/xi
            t = 2;
        }
#ifdef CRD_FFT_PROFILE
        const unsigned long long pf1 = prof::rdtsc();
#endif
        // Combine passes (register-pressure-SCHEDULED codelets), then a final radix-4 (r=1) writing straight
        // to interleaved `data` (the reinterleave fold). MIXED-radix, SIZE-AWARE: cap the per-pass radix by
        // the size's measured register-spill regime (rmax_bits), then greedily take the LARGEST radix ≤ cap
        // that leaves a coverable remainder (mid-bit budget is even; {2,3,4,5}-bit passes cover any even ≥2).
        // Measured sweet spots: radix-8 (3b) tiny + DRAM-bound (fits 16 ymm, no spill); radix-16 (4b) L2-band;
        // radix-32 (5b) where the codelet stays L1-resident so its spill is cheap and packs the bit budget.
        const T* ptw_re = m_ptw_re.data(); // Lever A: walk the precomputed per-pass twiddle table in pass order
        const T* ptw_im = m_ptw_im.data();
        while (t < m_log2 - 2U)
        {
            const crd::u32 remaining = (m_log2 - 2U) - t;
            crd::u32 b = m_rmax_bits < remaining ? m_rmax_bits : remaining;
            while (b > 2U && (remaining - b) == 1U) // never strand an uncoverable 1-bit remainder
            {
                --b;
            }
            const crd::usize lq = crd::usize{1} << t;
            switch (b)
            {
                case 5U:
                    radix32_pass(xr, xi, yr, yi, lq, m_n >> (t + 5U), dir, ptw_re, ptw_im);
                    break;
                case 4U:
                    radix16_pass(xr, xi, yr, yi, lq, m_n >> (t + 4U), dir, ptw_re, ptw_im);
                    break;
                case 3U:
                    radix8_pass(xr, xi, yr, yi, lq, m_n >> (t + 3U), dir, ptw_re, ptw_im);
                    break;
                default:
                    radix4_pass(xr, xi, yr, yi, lq, m_n >> (t + 2U), isign, ptw_re, ptw_im);
                    b = 2U;
                    break;
            }
            const crd::usize used = lq * ((crd::usize{1} << b) - 1); // advance past this pass's (j,m) block
            ptw_re += used;
            ptw_im += used;
            t += b;
            swap_buffers();
        }
#ifdef CRD_FFT_PROFILE
        const unsigned long long pf2 = prof::rdtsc();
#endif
        radix4_last_interleaved(xr, xi, data.data(), crd::usize{1} << t, isign); // last pass (r=1), folds reinterleave
#ifdef CRD_FFT_PROFILE
        const unsigned long long pf3 = prof::rdtsc();
        prof::g_first += pf1 - pf0;
        prof::g_combine += pf2 - pf1;
        prof::g_last += pf3 - pf2;
        ++prof::g_calls;
#endif
    }

    // The radix-2 in-place reference (the v10-a code) — the trusted oracle for cross-checking `execute`.
    // Bit-reversal + log2(n) butterfly stages, precomputed twiddles.
    void execute_reference(crd::containers::Span<Complex<T>> data, FftDirection dir) const
    {
        CRD_ASSERT(data.size() == m_n);
        if (m_n <= 1)
        {
            return;
        }
        for (crd::usize i = 0; i < m_n; ++i)
        {
            const crd::usize j = m_rev[i];
            if (i < j)
            {
                const Complex<T> tmp = data[i];
                data[i] = data[j];
                data[j] = tmp;
            }
        }
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        for (crd::usize len = 2; len <= m_n; len <<= 1)
        {
            const crd::usize half = len >> 1;
            const crd::usize step = m_n / len;
            for (crd::usize base = 0; base < m_n; base += len)
            {
                crd::usize tw = 0;
                for (crd::usize k = 0; k < half; ++k)
                {
                    const T wr = m_tw_re[tw];
                    const T wi = isign * m_tw_im[tw];
                    Complex<T>& a = data[base + k];
                    Complex<T>& b = data[base + k + half];
                    const T vr = wr * b.re - wi * b.im;
                    const T vi = wr * b.im + wi * b.re;
                    const T ar = a.re;
                    const T ai = a.im;
                    a.re = ar + vr;
                    a.im = ai + vi;
                    b.re = ar - vr;
                    b.im = ai - vi;
                    tw += step;
                }
            }
        }
    }

    // BATCHED FFT: `batch` independent size-m_n transforms in ELEMENT-MAJOR layout (element i of transform t
    // at data[i*batch + t]), in place. Radix-2 DIT, vectorized over the CONTIGUOUS batch axis (Vec4d over t
    // via deinterleave) — the efficiency the per-row execute() lacked: ONE kernel, zero per-transform
    // deinterleave overhead, the batch provides the SIMD width with NO register spill. This is the v10-e
    // batched primitive AND the sub-FFT engine for the block four-step/six-step (the large-N MKL technique).
    // Same butterfly + twiddle convention as execute_reference (the oracle), so cross-checkable.
    void execute_batched(crd::containers::Span<Complex<T>> data, crd::usize batch, FftDirection dir) const
    {
        const crd::usize b = batch;
        CRD_ASSERT(data.size() == m_n * b);
        if (m_n <= 1)
        {
            return;
        }
        Complex<T>* d = data.data();
        for (crd::usize i = 0; i < m_n; ++i) // bit-reversal on the element dim (swap whole rows of `b`)
        {
            const crd::usize j = m_rev[i];
            if (i < j)
            {
                for (crd::usize t = 0; t < b; ++t)
                {
                    const Complex<T> tmp = d[i * b + t];
                    d[i * b + t] = d[j * b + t];
                    d[j * b + t] = tmp;
                }
            }
        }
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        // RADIX-8 DIT (⅓ the passes of radix-2). One radix-2 or radix-4 remainder stage first so the rest is
        // pure radix-8 (log2(n) mod 3 → 0/1/2). All butterflies are NESTED (fused radix-2 stages) for the
        // bit-reversed input. q = current sub-size; step = n/(building size); twiddles carry isign.
        crd::usize len = 1;
        const crd::u32 rem = m_log2 % 3U;
        if (rem == 1U) // one radix-2 stage (1→2)
        {
            const crd::usize step = m_n >> 1;
            for (crd::usize base = 0; base < m_n; base += 2)
            {
                batched_butterfly2(d, b, base * b, (base + 1) * b, m_tw_re[0], isign * m_tw_im[0]);
            }
            (void)step;
            len = 2;
        }
        else if (rem == 2U) // one radix-4 stage (1→4): q=1 ⇒ ws=W_2^0=1, w0=W_4^0=1, w1=W_4^1
        {
            const crd::usize tq = m_n >> 2;
            for (crd::usize base = 0; base < m_n; base += 4)
            {
                batched_butterfly4(d, b, base * b, (base + 1) * b, (base + 2) * b, (base + 3) * b, m_tw_re[0],
                                   isign * m_tw_im[0], m_tw_re[0], isign * m_tw_im[0], m_tw_re[tq],
                                   isign * m_tw_im[tq]);
            }
            len = 4;
        }
        for (; len < m_n; len <<= 3) // radix-8 stages: q = len, builds 8·len
        {
            const crd::usize q = len;
            const crd::usize step = m_n / (len << 3);
            const crd::usize n4 = m_n >> 2, n8 = m_n >> 3, n38 = (3 * m_n) >> 3;
            for (crd::usize base = 0; base < m_n; base += (len << 3))
            {
                crd::usize tw = 0;
                for (crd::usize k = 0; k < q; ++k)
                {
                    // Nested radix-8 DIT twiddles: ws=W_{2q}^k; w0=W_{4q}^k, w1=W_{4q}^{k+q};
                    // v0=W_{8q}^k, v1=W_{8q}^{k+q}, v2=W_{8q}^{k+2q}, v3=W_{8q}^{k+3q}.
                    const T wsr = m_tw_re[4 * tw], wsi = isign * m_tw_im[4 * tw];
                    const T w0r = m_tw_re[2 * tw], w0i = isign * m_tw_im[2 * tw];
                    const T w1r = m_tw_re[2 * tw + n4], w1i = isign * m_tw_im[2 * tw + n4];
                    const T v0r = m_tw_re[tw], v0i = isign * m_tw_im[tw];
                    const T v1r = m_tw_re[tw + n8], v1i = isign * m_tw_im[tw + n8];
                    const T v2r = m_tw_re[tw + n4], v2i = isign * m_tw_im[tw + n4];
                    const T v3r = m_tw_re[tw + n38], v3i = isign * m_tw_im[tw + n38];
                    const crd::usize p = (base + k) * b;
                    batched_butterfly8(d, b, p, p + q * b, p + 2 * q * b, p + 3 * q * b, p + 4 * q * b,
                                       p + 5 * q * b, p + 6 * q * b, p + 7 * q * b, wsr, wsi, w0r, w0i, w1r, w1i,
                                       v0r, v0i, v1r, v1i, v2r, v2i, v3r, v3i);
                    tw += step;
                }
            }
        }
    }

    // One radix-2 batched butterfly over the contiguous batch axis (Vec4d over t for f64; scalar tail).
    static void batched_butterfly2(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, T wr, T wi) noexcept
    {
        Complex<T>* arow = d + o0;
        Complex<T>* brow = d + o1;
        crd::usize t = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            namespace simd = crd::math::simd;
            using V = simd::Vec4d;
            const V wrv(wr), wiv(wi);
            for (; t + 4 <= b; t += 4)
            {
                V ar, ai, br, bi;
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(arow + t), ar, ai);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(brow + t), br, bi);
                const V vr = wrv * br - wiv * bi, vi = wrv * bi + wiv * br;
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(arow + t), ar + vr, ai + vi);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(brow + t), ar - vr, ai - vi);
            }
        }
        for (; t < b; ++t)
        {
            const T ar = arow[t].re, ai = arow[t].im;
            const T vr = wr * brow[t].re - wi * brow[t].im, vi = wr * brow[t].im + wi * brow[t].re;
            arow[t] = Complex<T>{ar + vr, ai + vi};
            brow[t] = Complex<T>{ar - vr, ai - vi};
        }
    }

    // One radix-4 batched DIT butterfly over the contiguous batch axis, for BIT-REVERSED input. Two fused
    // radix-2 stages: inner pairs (o0,o1)&(o2,o3) with ws=W_{2q}^k, then outer (e0,f0)&(e1,f1) with
    // w0=W_{4q}^k / w1=W_{4q}^{k+q}. In place; twiddles carry isign (pre-conjugated for inverse).
    static void batched_butterfly4(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, crd::usize o2,
                                   crd::usize o3, T wsr, T wsi, T w0r, T w0i, T w1r, T w1i) noexcept
    {
        Complex<T>* r0 = d + o0;
        Complex<T>* r1 = d + o1;
        Complex<T>* r2 = d + o2;
        Complex<T>* r3 = d + o3;
        crd::usize t = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            namespace simd = crd::math::simd;
            using V = simd::Vec4d;
            const V wsrv(wsr), wsiv(wsi), w0rv(w0r), w0iv(w0i), w1rv(w1r), w1iv(w1i);
            for (; t + 4 <= b; t += 4)
            {
                V x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r0 + t), x0r, x0i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r1 + t), x1r, x1i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r2 + t), x2r, x2i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r3 + t), x3r, x3i);
                const V br = wsrv * x1r - wsiv * x1i, bi = wsrv * x1i + wsiv * x1r;   // ws·x1
                const V er = wsrv * x3r - wsiv * x3i, ei = wsrv * x3i + wsiv * x3r;   // ws·x3
                const V e0r = x0r + br, e0i = x0i + bi, e1r = x0r - br, e1i = x0i - bi;
                const V f0r = x2r + er, f0i = x2i + ei, f1r = x2r - er, f1i = x2i - ei;
                const V g0r = w0rv * f0r - w0iv * f0i, g0i = w0rv * f0i + w0iv * f0r; // w0·f0
                const V g1r = w1rv * f1r - w1iv * f1i, g1i = w1rv * f1i + w1iv * f1r; // w1·f1
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r0 + t), e0r + g0r, e0i + g0i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r1 + t), e1r + g1r, e1i + g1i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r2 + t), e0r - g0r, e0i - g0i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r3 + t), e1r - g1r, e1i - g1i);
            }
        }
        for (; t < b; ++t)
        {
            const T x0r = r0[t].re, x0i = r0[t].im, x2r = r2[t].re, x2i = r2[t].im;
            const T br = wsr * r1[t].re - wsi * r1[t].im, bi = wsr * r1[t].im + wsi * r1[t].re;
            const T er = wsr * r3[t].re - wsi * r3[t].im, ei = wsr * r3[t].im + wsi * r3[t].re;
            const T e0r = x0r + br, e0i = x0i + bi, e1r = x0r - br, e1i = x0i - bi;
            const T f0r = x2r + er, f0i = x2i + ei, f1r = x2r - er, f1i = x2i - ei;
            const T g0r = w0r * f0r - w0i * f0i, g0i = w0r * f0i + w0i * f0r;
            const T g1r = w1r * f1r - w1i * f1i, g1i = w1r * f1i + w1i * f1r;
            r0[t] = Complex<T>{e0r + g0r, e0i + g0i};
            r1[t] = Complex<T>{e1r + g1r, e1i + g1i};
            r2[t] = Complex<T>{e0r - g0r, e0i - g0i};
            r3[t] = Complex<T>{e1r - g1r, e1i - g1i};
        }
    }

    // One radix-8 batched DIT butterfly, BIT-REVERSED input — three fused radix-2 stages: A) ws=W_{2q}^k on
    // pairs (0,1)(2,3)(4,5)(6,7); B) w0=W_{4q}^k on (0,2)(4,6), w1=W_{4q}^{k+q} on (1,3)(5,7); C) v0..v3=
    // W_{8q}^{k,k+q,k+2q,k+3q} on (0,4)(1,5)(2,6)(3,7). In place; twiddles carry isign. Vec4d over the batch +
    // scalar tail (the 8-pt waist = 16 ymm at peak ⇒ some spill to L1, cheap on the L2-resident block).
    static void batched_butterfly8(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, crd::usize o2,
                                   crd::usize o3, crd::usize o4, crd::usize o5, crd::usize o6, crd::usize o7,
                                   T wsr, T wsi, T w0r, T w0i, T w1r, T w1i, T v0r, T v0i, T v1r, T v1i, T v2r,
                                   T v2i, T v3r, T v3i) noexcept
    {
        Complex<T>* r0 = d + o0;
        Complex<T>* r1 = d + o1;
        Complex<T>* r2 = d + o2;
        Complex<T>* r3 = d + o3;
        Complex<T>* r4 = d + o4;
        Complex<T>* r5 = d + o5;
        Complex<T>* r6 = d + o6;
        Complex<T>* r7 = d + o7;
        crd::usize t = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            namespace simd = crd::math::simd;
            using V = simd::Vec4d;
            const V wsrv(wsr), wsiv(wsi), w0rv(w0r), w0iv(w0i), w1rv(w1r), w1iv(w1i);
            const V v0rv(v0r), v0iv(v0i), v1rv(v1r), v1iv(v1i), v2rv(v2r), v2iv(v2i), v3rv(v3r), v3iv(v3i);
            for (; t + 4 <= b; t += 4)
            {
                V x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i, x4r, x4i, x5r, x5i, x6r, x6i, x7r, x7i;
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r0 + t), x0r, x0i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r1 + t), x1r, x1i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r2 + t), x2r, x2i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r3 + t), x3r, x3i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r4 + t), x4r, x4i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r5 + t), x5r, x5i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r6 + t), x6r, x6i);
                simd::load_complex_deinterleaved(reinterpret_cast<const crd::f64*>(r7 + t), x7r, x7i);
                const V s1r = wsrv * x1r - wsiv * x1i, s1i = wsrv * x1i + wsiv * x1r;
                const V s3r = wsrv * x3r - wsiv * x3i, s3i = wsrv * x3i + wsiv * x3r;
                const V s5r = wsrv * x5r - wsiv * x5i, s5i = wsrv * x5i + wsiv * x5r;
                const V s7r = wsrv * x7r - wsiv * x7i, s7i = wsrv * x7i + wsiv * x7r;
                const V a0r = x0r + s1r, a0i = x0i + s1i, a1r = x0r - s1r, a1i = x0i - s1i;
                const V a2r = x2r + s3r, a2i = x2i + s3i, a3r = x2r - s3r, a3i = x2i - s3i;
                const V a4r = x4r + s5r, a4i = x4i + s5i, a5r = x4r - s5r, a5i = x4i - s5i;
                const V a6r = x6r + s7r, a6i = x6i + s7i, a7r = x6r - s7r, a7i = x6i - s7i;
                const V m2r = w0rv * a2r - w0iv * a2i, m2i = w0rv * a2i + w0iv * a2r;
                const V m3r = w1rv * a3r - w1iv * a3i, m3i = w1rv * a3i + w1iv * a3r;
                const V m6r = w0rv * a6r - w0iv * a6i, m6i = w0rv * a6i + w0iv * a6r;
                const V m7r = w1rv * a7r - w1iv * a7i, m7i = w1rv * a7i + w1iv * a7r;
                const V c0r = a0r + m2r, c0i = a0i + m2i, c2r = a0r - m2r, c2i = a0i - m2i;
                const V c1r = a1r + m3r, c1i = a1i + m3i, c3r = a1r - m3r, c3i = a1i - m3i;
                const V c4r = a4r + m6r, c4i = a4i + m6i, c6r = a4r - m6r, c6i = a4i - m6i;
                const V c5r = a5r + m7r, c5i = a5i + m7i, c7r = a5r - m7r, c7i = a5i - m7i;
                const V n4r = v0rv * c4r - v0iv * c4i, n4i = v0rv * c4i + v0iv * c4r;
                const V n5r = v1rv * c5r - v1iv * c5i, n5i = v1rv * c5i + v1iv * c5r;
                const V n6r = v2rv * c6r - v2iv * c6i, n6i = v2rv * c6i + v2iv * c6r;
                const V n7r = v3rv * c7r - v3iv * c7i, n7i = v3rv * c7i + v3iv * c7r;
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r0 + t), c0r + n4r, c0i + n4i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r1 + t), c1r + n5r, c1i + n5i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r2 + t), c2r + n6r, c2i + n6i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r3 + t), c3r + n7r, c3i + n7i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r4 + t), c0r - n4r, c0i - n4i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r5 + t), c1r - n5r, c1i - n5i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r6 + t), c2r - n6r, c2i - n6i);
                simd::store_complex_interleaved(reinterpret_cast<crd::f64*>(r7 + t), c3r - n7r, c3i - n7i);
            }
        }
        for (; t < b; ++t)
        {
            // stage A: a_even = x_even + ws·x_odd, a_odd = x_even - ws·x_odd
            const T s1r = wsr * r1[t].re - wsi * r1[t].im, s1i = wsr * r1[t].im + wsi * r1[t].re;
            const T s3r = wsr * r3[t].re - wsi * r3[t].im, s3i = wsr * r3[t].im + wsi * r3[t].re;
            const T s5r = wsr * r5[t].re - wsi * r5[t].im, s5i = wsr * r5[t].im + wsi * r5[t].re;
            const T s7r = wsr * r7[t].re - wsi * r7[t].im, s7i = wsr * r7[t].im + wsi * r7[t].re;
            const T a0r = r0[t].re + s1r, a0i = r0[t].im + s1i, a1r = r0[t].re - s1r, a1i = r0[t].im - s1i;
            const T a2r = r2[t].re + s3r, a2i = r2[t].im + s3i, a3r = r2[t].re - s3r, a3i = r2[t].im - s3i;
            const T a4r = r4[t].re + s5r, a4i = r4[t].im + s5i, a5r = r4[t].re - s5r, a5i = r4[t].im - s5i;
            const T a6r = r6[t].re + s7r, a6i = r6[t].im + s7i, a7r = r6[t].re - s7r, a7i = r6[t].im - s7i;
            // stage B: w0 on (a0,a2)(a4,a6); w1 on (a1,a3)(a5,a7)
            const T m2r = w0r * a2r - w0i * a2i, m2i = w0r * a2i + w0i * a2r;
            const T m3r = w1r * a3r - w1i * a3i, m3i = w1r * a3i + w1i * a3r;
            const T m6r = w0r * a6r - w0i * a6i, m6i = w0r * a6i + w0i * a6r;
            const T m7r = w1r * a7r - w1i * a7i, m7i = w1r * a7i + w1i * a7r;
            const T c0r = a0r + m2r, c0i = a0i + m2i, c2r = a0r - m2r, c2i = a0i - m2i;
            const T c1r = a1r + m3r, c1i = a1i + m3i, c3r = a1r - m3r, c3i = a1i - m3i;
            const T c4r = a4r + m6r, c4i = a4i + m6i, c6r = a4r - m6r, c6i = a4i - m6i;
            const T c5r = a5r + m7r, c5i = a5i + m7i, c7r = a5r - m7r, c7i = a5i - m7i;
            // stage C: v0..v3 on (c0,c4)(c1,c5)(c2,c6)(c3,c7)
            const T n4r = v0r * c4r - v0i * c4i, n4i = v0r * c4i + v0i * c4r;
            const T n5r = v1r * c5r - v1i * c5i, n5i = v1r * c5i + v1i * c5r;
            const T n6r = v2r * c6r - v2i * c6i, n6i = v2r * c6i + v2i * c6r;
            const T n7r = v3r * c7r - v3i * c7i, n7i = v3r * c7i + v3i * c7r;
            r0[t] = Complex<T>{c0r + n4r, c0i + n4i};
            r1[t] = Complex<T>{c1r + n5r, c1i + n5i};
            r2[t] = Complex<T>{c2r + n6r, c2i + n6i};
            r3[t] = Complex<T>{c3r + n7r, c3i + n7i};
            r4[t] = Complex<T>{c0r - n4r, c0i - n4i};
            r5[t] = Complex<T>{c1r - n5r, c1i - n5i};
            r6[t] = Complex<T>{c2r - n6r, c2i - n6i};
            r7[t] = Complex<T>{c3r - n7r, c3i - n7i};
        }
    }

private:
    // Recursive DIT core: writes the n-point DFT of the strided SoA input (inr/ini, stride si) into the
    // contiguous output (outr/outi). RADIX-4 split (stride ×4), recurse into the 4 cosets, then a SIMD
    // in-place radix-4 combine. Same work-per-element as the direct radix-4 Stockham, but depth-first so the
    // sub-problems go cache-resident (the cache-oblivious win). Base case n≤32 = the generated codelet
    // reading at stride si. (n is a power of 2 ≥ 64 here ⇒ divisible by 4, and /4 descends to a 16/32 base.)
    void rec_fft_soa(T* outr, T* outi, const T* inr, const T* ini, crd::usize si, crd::usize n,
                     FftDirection dir) const
    {
        if (n <= 32)
        {
            dispatch_codelet_strided(inr, ini, si, outr, outi, n, dir);
            return;
        }
        const crd::usize m = n >> 2;
        const crd::usize s4 = si << 2;
        rec_fft_soa(outr, outi, inr, ini, s4, m, dir);                                     // coset 0 → out[0..m)
        rec_fft_soa(outr + m, outi + m, inr + si, ini + si, s4, m, dir);                   // coset 1
        rec_fft_soa(outr + 2 * m, outi + 2 * m, inr + 2 * si, ini + 2 * si, s4, m, dir);   // coset 2
        rec_fft_soa(outr + 3 * m, outi + 3 * m, inr + 3 * si, ini + 3 * si, s4, m, dir);   // coset 3
        radix4_combine_inplace(outr, outi, m, n, dir);
    }

    // In-place radix-4 DIT combine of 4 size-m sub-results stored contiguously at out[p·m..]. For each k2:
    // t_p = W_n^{p·k2}·X_p (p=1,2,3), then the radix-4 butterfly → Y[k1]=out[k1·m+k2]. SIMD over k2 (Vec4d);
    // the per-k2 combine-twiddles W_n^{p·k2}=m_tw[p·k2·(m_n/n)] are strided gathers. Same butterfly + sign
    // convention as radix4_row (verified). Element-independent over k2 ⇒ moat holds.
    void radix4_combine_inplace(T* outr, T* outi, crd::usize m, crd::usize n, FftDirection dir) const
    {
        const crd::usize ts = m_n / n; // W_n^idx = m_tw[idx·ts]
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        crd::usize k2 = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            namespace simd = crd::math::simd;
            using V = simd::Vec4d;
            const V sg(isign);
            for (; k2 + 4 <= m; k2 += 4)
            {
                const V x0r = V::load(outr + k2), x0i = V::load(outi + k2);
                const V x1r = V::load(outr + m + k2), x1i = V::load(outi + m + k2);
                const V x2r = V::load(outr + 2 * m + k2), x2i = V::load(outi + 2 * m + k2);
                const V x3r = V::load(outr + 3 * m + k2), x3i = V::load(outi + 3 * m + k2);
                const crd::usize a = k2 * ts, b = (k2 + 1) * ts, c = (k2 + 2) * ts, d = (k2 + 3) * ts;
                const V w1r(m_tw_re[a], m_tw_re[b], m_tw_re[c], m_tw_re[d]);
                const V w1i = V(m_tw_im[a], m_tw_im[b], m_tw_im[c], m_tw_im[d]) * sg;
                const V w2r(m_tw_re[2 * a], m_tw_re[2 * b], m_tw_re[2 * c], m_tw_re[2 * d]);
                const V w2i = V(m_tw_im[2 * a], m_tw_im[2 * b], m_tw_im[2 * c], m_tw_im[2 * d]) * sg;
                const V w3r(m_tw_re[3 * a], m_tw_re[3 * b], m_tw_re[3 * c], m_tw_re[3 * d]);
                const V w3i = V(m_tw_im[3 * a], m_tw_im[3 * b], m_tw_im[3 * c], m_tw_im[3 * d]) * sg;
                const V t1r = w1r * x1r - w1i * x1i, t1i = w1r * x1i + w1i * x1r;
                const V t2r = w2r * x2r - w2i * x2i, t2i = w2r * x2i + w2i * x2r;
                const V t3r = w3r * x3r - w3i * x3i, t3i = w3r * x3i + w3i * x3r;
                const V a0r = x0r + t2r, a0i = x0i + t2i, a1r = x0r - t2r, a1i = x0i - t2i;
                const V a2r = t1r + t3r, a2i = t1i + t3i, a3r = t1r - t3r, a3i = t1i - t3i;
                const V rsr = V(0.0) - sg * a3i, rsi = sg * a3r;
                (a0r + a2r).store(outr + k2);
                (a0i + a2i).store(outi + k2);
                (a1r - rsr).store(outr + m + k2);
                (a1i - rsi).store(outi + m + k2);
                (a0r - a2r).store(outr + 2 * m + k2);
                (a0i - a2i).store(outi + 2 * m + k2);
                (a1r + rsr).store(outr + 3 * m + k2);
                (a1i + rsi).store(outi + 3 * m + k2);
            }
        }
        for (; k2 < m; ++k2)
        {
            const crd::usize a = k2 * ts;
            const T w1r = m_tw_re[a], w1i = isign * m_tw_im[a];
            const T w2r = m_tw_re[2 * a], w2i = isign * m_tw_im[2 * a];
            const T w3r = m_tw_re[3 * a], w3i = isign * m_tw_im[3 * a];
            const T x0r = outr[k2], x0i = outi[k2];
            const T t1r = w1r * outr[m + k2] - w1i * outi[m + k2], t1i = w1r * outi[m + k2] + w1i * outr[m + k2];
            const T t2r = w2r * outr[2 * m + k2] - w2i * outi[2 * m + k2],
                    t2i = w2r * outi[2 * m + k2] + w2i * outr[2 * m + k2];
            const T t3r = w3r * outr[3 * m + k2] - w3i * outi[3 * m + k2],
                    t3i = w3r * outi[3 * m + k2] + w3i * outr[3 * m + k2];
            const T a0r = x0r + t2r, a0i = x0i + t2i, a1r = x0r - t2r, a1i = x0i - t2i;
            const T a2r = t1r + t3r, a2i = t1i + t3i, a3r = t1r - t3r, a3i = t1i - t3i;
            const T rsr = -isign * a3i, rsi = isign * a3r;
            outr[k2] = a0r + a2r;
            outi[k2] = a0i + a2i;
            outr[m + k2] = a1r - rsr;
            outi[m + k2] = a1i - rsi;
            outr[2 * m + k2] = a0r - a2r;
            outi[2 * m + k2] = a0i - a2i;
            outr[3 * m + k2] = a1r + rsr;
            outi[3 * m + k2] = a1i + rsi;
        }
    }

    // The codelet leaf for the recursion: n-point DFT of strided input (stride si) → contiguous output.
    void dispatch_codelet_strided(const T* inr, const T* ini, crd::usize si, T* outr, T* outi, crd::usize n,
                                  FftDirection dir) const
    {
        const bool fwd = (dir == FftDirection::Forward);
        switch (n)
        {
            case 2:
                fwd ? detail::codelet_2_fwd<T>(inr, ini, si, outr, outi, 1)
                    : detail::codelet_2_inv<T>(inr, ini, si, outr, outi, 1);
                break;
            case 4:
                fwd ? detail::codelet_4_fwd<T>(inr, ini, si, outr, outi, 1)
                    : detail::codelet_4_inv<T>(inr, ini, si, outr, outi, 1);
                break;
            case 8:
                fwd ? detail::codelet_8_fwd<T>(inr, ini, si, outr, outi, 1)
                    : detail::codelet_8_inv<T>(inr, ini, si, outr, outi, 1);
                break;
            case 16:
                fwd ? detail::codelet_16_fwd<T>(inr, ini, si, outr, outi, 1)
                    : detail::codelet_16_inv<T>(inr, ini, si, outr, outi, 1);
                break;
            case 32:
                fwd ? detail::codelet_32_fwd<T>(inr, ini, si, outr, outi, 1)
                    : detail::codelet_32_inv<T>(inr, ini, si, outr, outi, 1);
                break;
            default:
                break; // unreachable (n is a power of two in [2,32])
        }
    }
    // Six-step (Bailey) DISABLED — MEASURED a loss TWICE on the 14900K: with the scalar blocked transpose
    // (4M 3.58 vs direct 4.89) AND with the AVX2 register transpose (transpose_simd_c64; 4M 3.63 vs direct
    // 7.5, 2M 4.49 vs 7.8). So transpose SPEED was never the issue: 3 full-array transposes + scattered
    // tile access can't beat the direct radix-4's prefetcher-friendly SEQUENTIAL streaming on this hardware.
    // ⭐ Lever D RESURRECTION (2026-06-14): the prior floor (~51 ms ≈ MKL @8M) was the SCALAR/strided transpose
    // running at ~8 GB/s. A blocked transpose with NON-TEMPORAL STORES measured 25.7 GB/s on this box (3× that,
    // above MKL's ~20) ⇒ the 4n transpose floor drops to ~21 ms ≪ MKL's 52 ⇒ four-step viable at large N. The
    // block four-step below uses NT-store scatter + the hoisted batched sub-FFTs. Enabled above the crossover
    // (SCAN value — narrow once the kernel + transpose are tuned). Gated vs the radix-2 oracle.
    static constexpr crd::usize kFourStepMin = crd::usize{1} << 19; // 512K+ = the DRAM-wall trough (SCAN)
    static constexpr crd::usize kGatherPf = 8; // four-step gather prefetch distance (rows ahead; probe-tuned)

    // Dispatch the in-place leaf transform to its generated straight-line codelet (m_n ∈ {2,4,8,16,32}).
    void dispatch_codelet(T* re, T* im, FftDirection dir) const
    {
        const bool fwd = (dir == FftDirection::Forward);
        switch (m_n)
        {
            case 2:
                fwd ? detail::codelet_2_fwd<T>(re, im, 1, re, im, 1) : detail::codelet_2_inv<T>(re, im, 1, re, im, 1);
                break;
            case 4:
                fwd ? detail::codelet_4_fwd<T>(re, im, 1, re, im, 1) : detail::codelet_4_inv<T>(re, im, 1, re, im, 1);
                break;
            case 8:
                fwd ? detail::codelet_8_fwd<T>(re, im, 1, re, im, 1) : detail::codelet_8_inv<T>(re, im, 1, re, im, 1);
                break;
            case 16:
                fwd ? detail::codelet_16_fwd<T>(re, im, 1, re, im, 1)
                    : detail::codelet_16_inv<T>(re, im, 1, re, im, 1);
                break;
            case 32:
                fwd ? detail::codelet_32_fwd<T>(re, im, 1, re, im, 1)
                    : detail::codelet_32_inv<T>(re, im, 1, re, im, 1);
                break;
            default:
                break; // unreachable (m_n is a power of two in [2,32])
        }
    }

    // Cache-blocked transpose: dst (cols×rows) = src (rows×cols)ᵀ, in B×B tiles so the strided access stays
    // cache-resident (a NAIVE strided transpose thrashes cache and regressed the whole four-step — measured).
    static void transpose_blocked(const Complex<T>* src, Complex<T>* dst, crd::usize rows, crd::usize cols) noexcept
    {
        constexpr crd::usize b = 16;
        for (crd::usize i0 = 0; i0 < rows; i0 += b)
        {
            const crd::usize iend = (i0 + b < rows) ? i0 + b : rows;
            for (crd::usize j0 = 0; j0 < cols; j0 += b)
            {
                const crd::usize jend = (j0 + b < cols) ? j0 + b : cols;
                for (crd::usize i = i0; i < iend; ++i)
                {
                    for (crd::usize j = j0; j < jend; ++j)
                    {
                        dst[j * rows + i] = src[i * cols + j];
                    }
                }
            }
        }
    }

    // Bandwidth-efficient transpose of a (rows×cols) complex<f64> matrix into dst (cols×rows): 4×4-tiled with
    // an AVX2 in-register transpose (deinterleave-load → transpose4x4 re & im → interleave-store) so dst rows
    // are written CONTIGUOUSLY (full cache lines), not strided 16-byte scatters — the fix that makes the
    // six-step's transposes run near DRAM bandwidth. rows,cols are powers of 2 ≥ 4 here (always 4-divisible).
    static void transpose_simd_c64(const Complex<crd::f64>* src, Complex<crd::f64>* dst, crd::usize rows,
                                   crd::usize cols) noexcept
    {
        namespace simd = crd::math::simd;
        const auto* s = reinterpret_cast<const crd::f64*>(src);
        auto* d = reinterpret_cast<crd::f64*>(dst);
        for (crd::usize i = 0; i < rows; i += 4)
        {
            for (crd::usize j = 0; j < cols; j += 4)
            {
                simd::Vec4d ar, ai, br, bi, cr, ci, dr, di;
                simd::load_complex_deinterleaved(s + 2 * ((i + 0) * cols + j), ar, ai);
                simd::load_complex_deinterleaved(s + 2 * ((i + 1) * cols + j), br, bi);
                simd::load_complex_deinterleaved(s + 2 * ((i + 2) * cols + j), cr, ci);
                simd::load_complex_deinterleaved(s + 2 * ((i + 3) * cols + j), dr, di);
                simd::transpose4x4(ar, br, cr, dr); // re: src rows → dst rows
                simd::transpose4x4(ai, bi, ci, di); // im
                simd::store_complex_interleaved(d + 2 * ((j + 0) * rows + i), ar, ai);
                simd::store_complex_interleaved(d + 2 * ((j + 1) * rows + i), br, bi);
                simd::store_complex_interleaved(d + 2 * ((j + 2) * rows + i), cr, ci);
                simd::store_complex_interleaved(d + 2 * ((j + 3) * rows + i), dr, di);
            }
        }
    }

    // Transpose dispatch: fast AVX2 path for f64, the generic blocked path otherwise.
    static void transpose(const Complex<T>* src, Complex<T>* dst, crd::usize rows, crd::usize cols) noexcept
    {
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            transpose_simd_c64(src, dst, rows, cols);
        }
        else
        {
            transpose_blocked(src, dst, rows, cols);
        }
    }

    // Block four-step RESURRECTION (n = n1·n2): cache-resident BATCHED sub-FFTs + NON-TEMPORAL-STORE scatter
    // (the 25.7 GB/s transpose lever). Phase 1 = n2 column FFTs (length n1) + inter-stage twiddle, scattered to
    // m_tbuf with NT stores; phase 2 = n1 row FFTs (length n2), scattered to data with NT stores → natural
    // order. Gather = block-contiguous (memcpy); the implicit transpose is fused into gather+NT-scatter.
    //   X[k1 + n1·k2] = Σ_{i2} W_n^{k1·i2} · W_{n2}^{i2·k2} · ( Σ_{i1} x[i1·n2 + i2] · W_{n1}^{i1·k1} ).
    void execute_four_step(crd::containers::Span<Complex<T>> data, FftDirection dir) const
    {
        namespace cont = crd::containers;
        const crd::usize n = m_n;
        const crd::usize n1 = m_n1;
        const crd::usize n2 = n / n1;
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        Complex<T>* const tbuf = m_tbuf;
        Complex<T>* const scratch = m_scratch;
        Complex<T>* const din = data.data();
        const crd::usize b1 = block_width(n1);
        const crd::usize b2 = block_width(n2);

        // ---- PHASE 1: column FFTs (length n1) + twiddle, NT-scatter to tbuf[i2·n1 + k1] ----
        for (crd::usize i2 = 0; i2 < n2; i2 += b1)
        {
            const crd::usize bw = (i2 + b1 < n2) ? b1 : (n2 - i2);
            for (crd::usize i1 = 0; i1 < n1; ++i1) // gather B columns (each a contiguous bw-run) → element-major
            {
                if (i1 + kGatherPf < n1) // prefetch the strided next row (hint-only ⇒ bit-identical; latency hide)
                {
                    _mm_prefetch(reinterpret_cast<const char*>(din + (i1 + kGatherPf) * n2 + i2), _MM_HINT_T0);
                }
                std::memcpy(scratch + i1 * bw, din + i1 * n2 + i2, bw * sizeof(Complex<T>));
            }
            m_p1->execute_batched(cont::Span<Complex<T>>(scratch, n1 * bw), bw, dir);
            // twiddle scratch[k1·bw+bb] *= W_n^{k1·col}; NT-store to tbuf row. The twiddle is FACTORED from two
            // ~√n L2-resident tables (no 128 MB streaming read): W_n^a = W_n^{a_hi·M}·W_n^{a_lo}, a = col·k1 < n.
            const crd::usize mmask = (crd::usize{1} << m_ftw_h) - 1;
            const T* const hir = m_ftw_hi_re.data();
            const T* const hii = m_ftw_hi_im.data();
            const T* const lor = m_ftw_lo_re.data();
            const T* const loi = m_ftw_lo_im.data();
            for (crd::usize bb = 0; bb < bw; ++bb)
            {
                const crd::usize col = i2 + bb;
                Complex<T>* const trow = tbuf + col * n1;
                for (crd::usize k1 = 0; k1 < n1; ++k1)
                {
                    const crd::usize a = col * k1; // < n
                    const T hr = hir[a >> m_ftw_h];
                    const T hi = hii[a >> m_ftw_h];
                    const T lr = lor[a & mmask];
                    const T li = loi[a & mmask];
                    const T wr = hr * lr - hi * li;  // Re W_n^a
                    const T wim = hr * li + hi * lr; // Im W_n^a  (cos,−sin convention preserved by the product)
                    const T wi = isign * wim;
                    const Complex<T> z = scratch[k1 * bw + bb];
                    store_complex(trow + k1, z.re * wr - z.im * wi, z.re * wi + z.im * wr, true); // tbuf 64B
                }
            }
        }

        // ---- PHASE 2: row FFTs (length n2), NT-scatter to data (natural order) ----
        // NT-store to `data` only if the caller's buffer is ≥16B aligned (Complex<f64> array is only 8B-aligned
        // by alignof); else a plain store. Checked ONCE; all per-element offsets preserve the base alignment.
        const bool nt_out = (reinterpret_cast<std::uintptr_t>(din) % 16U) == 0U;
        for (crd::usize k1 = 0; k1 < n1; k1 += b2)
        {
            const crd::usize bw = (k1 + b2 < n1) ? b2 : (n1 - k1);
            for (crd::usize i2 = 0; i2 < n2; ++i2) // gather B rows (each a contiguous bw-run from tbuf)
            {
                if (i2 + kGatherPf < n2) // prefetch the strided next row (hint-only ⇒ bit-identical; latency hide)
                {
                    _mm_prefetch(reinterpret_cast<const char*>(tbuf + (i2 + kGatherPf) * n1 + k1), _MM_HINT_T0);
                }
                std::memcpy(scratch + i2 * bw, tbuf + i2 * n1 + k1, bw * sizeof(Complex<T>));
            }
            m_p2->execute_batched(cont::Span<Complex<T>>(scratch, n2 * bw), bw, dir);
            for (crd::usize k2 = 0; k2 < n2; ++k2) // X[(k1+bb)+n1·k2] = scratch[k2·bw+bb] → data, NT-store
            {
                const Complex<T>* const srow = scratch + k2 * bw;
                Complex<T>* const drow = din + k2 * n1 + k1;
                for (crd::usize bb = 0; bb < bw; ++bb)
                {
                    store_complex(drow + bb, srow[bb].re, srow[bb].im, nt_out);
                }
            }
        }
        _mm_sfence(); // order the non-temporal stores before the caller reads the result
    }

    // Store one Complex<T>, optionally non-temporal (skips read-for-ownership — the 25.7 GB/s lever). NT path
    // taken only when nt && f64 && dst is ≥16B aligned (m_tbuf is 64B; the caller's `data` is checked once).
    static void store_complex(Complex<T>* dst, T re, T im, bool nt) noexcept
    {
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            if (nt) { _mm_stream_pd(reinterpret_cast<double*>(dst), _mm_set_pd(im, re)); return; }
        }
        *dst = Complex<T>{re, im};
    }

    // Block width so the batched sub-FFT working set (size·B complex) stays cache-resident (~512 KB L2 budget).
    static crd::usize block_width(crd::usize size) noexcept
    {
        constexpr crd::usize kBudget = crd::usize{1} << 20; // 1 MB block (bigger ⇒ larger contiguous gather runs)
        const crd::usize per = size * sizeof(Complex<T>);
        const crd::usize b = (per >= kBudget) ? crd::usize{1} : (kBudget / per);
        return b == 0 ? crd::usize{1} : b;
    }

    // One radix-2 Stockham pass: l_half (j) groups, each r unit-stride butterflies. r = n/L, L = 2·l_half.
    void radix2_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize l_half, crd::usize r, T isign) const
    {
        const crd::usize half_n = m_n >> 1;
        for (crd::usize j = 0; j < l_half; ++j)
        {
            const T wr = m_tw_re[j * r];
            const T wi = isign * m_tw_im[j * r];
            const crd::usize i0 = 2 * j * r;
            const crd::usize i1 = i0 + r;
            const crd::usize o0 = j * r;
            const crd::usize o1 = o0 + half_n;
            butterfly_row(xr, xi, yr, yi, i0, i1, o0, o1, r, wr, wi);
        }
    }

    // One radix-4 Stockham pass: lq (j) groups, each r unit-stride radix-4 butterflies. r = n/L, L = 4·lq.
    // Half the passes of radix-2 ⇒ half the memory traffic (the pass-bound lever). Three twiddles per group
    // (W_L^j, W_L^{2j}, W_L^{3j} = the full table at j·r, 2j·r, 3j·r — all < n).
    void radix4_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, T isign,
                     const T* ptw_re, const T* ptw_im) const
    {
        const crd::usize q = m_n >> 2; // n/4 (output-quarter stride)
        crd::usize off = 0; // Lever A: precomputed linear twiddles (w1,w2,w3 per group)
        for (crd::usize j = 0; j < lq; ++j)
        {
            const T w1r = ptw_re[off];
            const T w1i = isign * ptw_im[off];
            const T w2r = ptw_re[off + 1];
            const T w2i = isign * ptw_im[off + 1];
            const T w3r = ptw_re[off + 2];
            const T w3i = isign * ptw_im[off + 2];
            off += 3;
            const crd::usize i0 = 4 * j * r;
            const crd::usize o0 = j * r;
            radix4_row(xr, xi, yr, yi, i0, i0 + r, i0 + 2 * r, i0 + 3 * r, o0, o0 + q, o0 + 2 * q, o0 + 3 * q, r, w1r,
                       w1i, w2r, w2i, w3r, w3i, isign);
        }
    }

    // FIRST pass, radix-2, twiddle-free (j=0 ⇒ w=1): reads interleaved `data` directly into the split buffers
    // — folds the deinterleave into pass 1. Scalar: the pass is bandwidth-bound (SIMD bought ~1.2× anywhere),
    // so the eliminated full memory pass dwarfs the lost vectorization. Used when log2(n) is odd.
    void radix2_first_interleaved(const Complex<T>* d, T* yr, T* yi) const
    {
        const crd::usize r = m_n >> 1; // half_n; the two input streams are d[k] and d[r+k]
        for (crd::usize k = 0; k < r; ++k)
        {
            const T ar = d[k].re;
            const T ai = d[k].im;
            const T br = d[r + k].re;
            const T bi = d[r + k].im;
            yr[k] = ar + br;
            yi[k] = ai + bi;
            yr[r + k] = ar - br;
            yi[r + k] = ai - bi;
        }
    }

    // FIRST pass, radix-4, twiddle-free (lq=1, j=0 ⇒ w1=w2=w3=1): reads interleaved `data` into the split
    // buffers. Same butterfly as radix4_row with the twiddle multiplies dropped. Used when log2(n) is even.
    void radix4_first_interleaved(const Complex<T>* d, T* yr, T* yi, T isign) const
    {
        const crd::usize r = m_n >> 2; // n/4 = the input-stream stride
        const crd::usize q = r;        // output quarter stride = n/4
        crd::usize k = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            // SIMD over k with an AoS→SoA deinterleave-load (recovers SIMD on this full pass; the fold made
            // it scalar). r = n/4 is large ⇒ this loop dominates. f32 keeps the scalar path below.
            namespace simd = crd::math::simd;
            using V = simd::Vec4d;
            const V sg(isign);
            const auto* dp = reinterpret_cast<const crd::f64*>(d); // interleaved {re,im} pairs
            for (; k + 4 <= r; k += 4)
            {
                V ar, ai, br, bi, cr, ci, dr, di;
                simd::load_complex_deinterleaved(dp + 2 * k, ar, ai);
                simd::load_complex_deinterleaved(dp + 2 * (r + k), br, bi);
                simd::load_complex_deinterleaved(dp + 2 * (2 * r + k), cr, ci);
                simd::load_complex_deinterleaved(dp + 2 * (3 * r + k), dr, di);
                const V t0r = ar + cr, t0i = ai + ci, t1r = ar - cr, t1i = ai - ci;
                const V t2r = br + dr, t2i = bi + di, t3r = br - dr, t3i = bi - di;
                const V rsr = V(0.0) - sg * t3i, rsi = sg * t3r;
                (t0r + t2r).store(yr + k);
                (t0i + t2i).store(yi + k);
                (t1r - rsr).store(yr + q + k);
                (t1i - rsi).store(yi + q + k);
                (t0r - t2r).store(yr + 2 * q + k);
                (t0i - t2i).store(yi + 2 * q + k);
                (t1r + rsr).store(yr + 3 * q + k);
                (t1i + rsi).store(yi + 3 * q + k);
            }
        }
        for (; k < r; ++k)
        {
            const T ar = d[k].re;
            const T ai = d[k].im;
            const T cr = d[2 * r + k].re;
            const T ci = d[2 * r + k].im;
            const T br = d[r + k].re;
            const T bi = d[r + k].im;
            const T dr = d[3 * r + k].re;
            const T di = d[3 * r + k].im;
            const T t0r = ar + cr;
            const T t0i = ai + ci;
            const T t1r = ar - cr;
            const T t1i = ai - ci;
            const T t2r = br + dr;
            const T t2i = bi + di;
            const T t3r = br - dr;
            const T t3i = bi - di;
            const T rsr = -isign * t3i;
            const T rsi = isign * t3r;
            yr[k] = t0r + t2r;
            yi[k] = t0i + t2i;
            yr[q + k] = t1r - rsr;
            yi[q + k] = t1i - rsi;
            yr[2 * q + k] = t0r - t2r;
            yi[2 * q + k] = t0i - t2i;
            yr[3 * q + k] = t1r + rsr;
            yi[3 * q + k] = t1i + rsi;
        }
    }

    // LAST radix-4 pass (lq = n/4 groups, r=1 ⇒ already one butterfly per group, no SIMD-over-k anyway):
    // reads the split buffers and writes straight to interleaved `data` — folds away the reinterleave pass.
    void radix4_last_interleaved(const T* xr, const T* xi, Complex<T>* d, crd::usize lq, T isign) const
    {
        const crd::usize q = m_n >> 2; // output quarter stride
        crd::usize j = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            // SIMD over j (4 groups/iter): r=1 ⇒ over-k degenerates to scalar, so vectorize the GROUP axis
            // instead. Load 4 groups' 16 contiguous values, transpose4x4 into per-point vectors (no cross-lane
            // butterfly shuffles), per-lane combine-twiddles, then contiguous interleaved stores. Recovers SIMD
            // on this otherwise-scalar full pass. w1 loads contiguous (idx=j); w2/w3 are stride-2/3 gathers.
            namespace simd = crd::math::simd;
            using V = simd::Vec4d;
            const V sg(isign);
            auto* dp = reinterpret_cast<crd::f64*>(d);
            for (; j + 4 <= lq; j += 4)
            {
                V p0r = V::load(xr + 4 * j), p1r = V::load(xr + 4 * j + 4);
                V p2r = V::load(xr + 4 * j + 8), p3r = V::load(xr + 4 * j + 12);
                simd::transpose4x4(p0r, p1r, p2r, p3r); // → p_m_r = re of point m for groups j..j+3
                V p0i = V::load(xi + 4 * j), p1i = V::load(xi + 4 * j + 4);
                V p2i = V::load(xi + 4 * j + 8), p3i = V::load(xi + 4 * j + 12);
                simd::transpose4x4(p0i, p1i, p2i, p3i);
                const V w1r = V::load(&m_tw_re[j]);
                const V w1i = V::load(&m_tw_im[j]) * sg;
                const V w2r(m_tw_re[2 * j], m_tw_re[2 * j + 2], m_tw_re[2 * j + 4], m_tw_re[2 * j + 6]);
                const V w2i = V(m_tw_im[2 * j], m_tw_im[2 * j + 2], m_tw_im[2 * j + 4], m_tw_im[2 * j + 6]) * sg;
                const V w3r(m_tw_re[3 * j], m_tw_re[3 * j + 3], m_tw_re[3 * j + 6], m_tw_re[3 * j + 9]);
                const V w3i = V(m_tw_im[3 * j], m_tw_im[3 * j + 3], m_tw_im[3 * j + 6], m_tw_im[3 * j + 9]) * sg;
                const V br = w1r * p1r - w1i * p1i, bi = w1r * p1i + w1i * p1r;
                const V cr = w2r * p2r - w2i * p2i, ci = w2r * p2i + w2i * p2r;
                const V dr = w3r * p3r - w3i * p3i, di = w3r * p3i + w3i * p3r;
                const V t0r = p0r + cr, t0i = p0i + ci, t1r = p0r - cr, t1i = p0i - ci;
                const V t2r = br + dr, t2i = bi + di, t3r = br - dr, t3i = bi - di;
                const V rsr = V(0.0) - sg * t3i, rsi = sg * t3r;
                simd::store_complex_interleaved(dp + 2 * j, t0r + t2r, t0i + t2i);
                simd::store_complex_interleaved(dp + 2 * (q + j), t1r - rsr, t1i - rsi);
                simd::store_complex_interleaved(dp + 2 * (2 * q + j), t0r - t2r, t0i - t2i);
                simd::store_complex_interleaved(dp + 2 * (3 * q + j), t1r + rsr, t1i + rsi);
            }
        }
        for (; j < lq; ++j)
        {
            const T w1r = m_tw_re[j];
            const T w1i = isign * m_tw_im[j];
            const T w2r = m_tw_re[2 * j];
            const T w2i = isign * m_tw_im[2 * j];
            const T w3r = m_tw_re[3 * j];
            const T w3i = isign * m_tw_im[3 * j];
            const crd::usize i0 = 4 * j;
            const T ar = xr[i0];
            const T ai = xi[i0];
            const T b0r = xr[i0 + 1];
            const T b0i = xi[i0 + 1];
            const T c0r = xr[i0 + 2];
            const T c0i = xi[i0 + 2];
            const T d0r = xr[i0 + 3];
            const T d0i = xi[i0 + 3];
            const T br = w1r * b0r - w1i * b0i;
            const T bi = w1r * b0i + w1i * b0r;
            const T cr = w2r * c0r - w2i * c0i;
            const T ci = w2r * c0i + w2i * c0r;
            const T dr = w3r * d0r - w3i * d0i;
            const T di = w3r * d0i + w3i * d0r;
            const T t0r = ar + cr;
            const T t0i = ai + ci;
            const T t1r = ar - cr;
            const T t1i = ai - ci;
            const T t2r = br + dr;
            const T t2i = bi + di;
            const T t3r = br - dr;
            const T t3i = bi - di;
            const T rsr = -isign * t3i;
            const T rsi = isign * t3r;
            d[j] = Complex<T>{t0r + t2r, t0i + t2i};
            d[q + j] = Complex<T>{t1r - rsr, t1i - rsi};
            d[2 * q + j] = Complex<T>{t0r - t2r, t0i - t2i};
            d[3 * q + j] = Complex<T>{t1r + rsr, t1i + rsi};
        }
    }

    // One radix-8 Stockham COMBINE pass via the register-pressure-SCHEDULED SIMD twiddle-codelet (genfft
    // engine): lq groups, each r unit-stride radix-8 butterflies (vectorized over k inside the codelet, with
    // loads-late/stores-early scheduling so the 8-point waist fits the 16-ymm file). Builds 3 bits/pass ⇒
    // log₈(N) passes vs radix-4's log₄(N). Combine-twiddles W_{8·lq}^{m·j} = m_tw[m·j·r] (isign-conjugated
    // for inverse); DFT-internal twiddles are compile-time inside the fwd/inv codelet. r = n/(8·lq); q = n/8.
    void radix8_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, FftDirection dir,
                     const T* ptw_re, const T* ptw_im) const
    {
        const crd::usize q = m_n >> 3; // n/8
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        T w_re[8];
        T w_im[8];
        w_re[0] = static_cast<T>(1);
        w_im[0] = static_cast<T>(0);
        crd::usize off = 0; // Lever A: per-pass twiddles precomputed in linear (j,m) order ⇒ sequential reads, no imul
        for (crd::usize j = 0; j < lq; ++j)
        {
            for (crd::usize m = 1; m < 8; ++m)
            {
                w_re[m] = ptw_re[off];
                w_im[m] = isign * ptw_im[off];
                ++off;
            }
            const crd::usize ibase = 8 * j * r;
            const crd::usize obase = j * r;
            if (dir == FftDirection::Forward)
            {
                detail::twiddle8_fwd<T>(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
            else
            {
                detail::twiddle8_inv<T>(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
        }
    }

    // One radix-16 Stockham COMBINE pass via the scheduled SIMD twiddle-codelet (genfft engine): builds 4
    // bits/pass ⇒ log₁₆(N) passes (half of radix-4's). r = n/(16·lq); output stride q = n/16. The 16-point
    // codelet is register-pressure-scheduled (loads late / stores early) to keep peak live tolerable.
    void radix16_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, FftDirection dir,
                      const T* ptw_re, const T* ptw_im) const
    {
        const crd::usize q = m_n >> 4; // n/16
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        T w_re[16];
        T w_im[16];
        w_re[0] = static_cast<T>(1);
        w_im[0] = static_cast<T>(0);
        crd::usize off = 0; // Lever A: precomputed linear twiddles
        for (crd::usize j = 0; j < lq; ++j)
        {
            for (crd::usize m = 1; m < 16; ++m)
            {
                w_re[m] = ptw_re[off];
                w_im[m] = isign * ptw_im[off];
                ++off;
            }
            const crd::usize ibase = 16 * j * r;
            const crd::usize obase = j * r;
            if (dir == FftDirection::Forward)
            {
                detail::twiddle16_fwd<T>(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
            else
            {
                detail::twiddle16_inv<T>(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
        }
    }

    // One radix-32 Stockham COMBINE pass (scheduled twiddle-codelet): 5 bits/pass ⇒ log32(N) passes. The
    // 32-point codelet spills (32 complex > 16 ymm) but at L1/L2-resident sizes the spill hits cache (cheap),
    // so fewer passes can win there even though it loses DRAM-bound. r = n/(32·lq); output stride q = n/32.
    void radix32_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, FftDirection dir,
                      const T* ptw_re, const T* ptw_im) const
    {
        const crd::usize q = m_n >> 5; // n/32
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        T w_re[32];
        T w_im[32];
        w_re[0] = static_cast<T>(1);
        w_im[0] = static_cast<T>(0);
        crd::usize off = 0; // Lever A: precomputed linear twiddles
        for (crd::usize j = 0; j < lq; ++j)
        {
            for (crd::usize m = 1; m < 32; ++m)
            {
                w_re[m] = ptw_re[off];
                w_im[m] = isign * ptw_im[off];
                ++off;
            }
            const crd::usize ibase = 32 * j * r;
            const crd::usize obase = j * r;
            if (dir == FftDirection::Forward)
            {
                detail::twiddle32_fwd<T>(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
            else
            {
                detail::twiddle32_inv<T>(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
        }
    }

    // r unit-stride radix-4 butterflies sharing (w1,w2,w3). AVX2 over k (element-independent ⇒ bit-identical
    // to the scalar remainder, so the {1..16}/SIMD-width determinism holds, ADR-0063).
    static void radix4_row(const T* xr, const T* xi, T* yr, T* yi, crd::usize i0, crd::usize i1, crd::usize i2,
                           crd::usize i3, crd::usize o0, crd::usize o1, crd::usize o2, crd::usize o3, crd::usize r,
                           T w1r, T w1i, T w2r, T w2i, T w3r, T w3i, T isign) noexcept
    {
        crd::usize k = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            namespace simd = crd::math::simd;
            using V = simd::Vec4d;
            const V w1rv(w1r), w1iv(w1i), w2rv(w2r), w2iv(w2i), w3rv(w3r), w3iv(w3i), sg(isign);
            for (; k + 4 <= r; k += 4)
            {
                const V ar = V::load(xr + i0 + k), ai = V::load(xi + i0 + k);
                const V b0r = V::load(xr + i1 + k), b0i = V::load(xi + i1 + k);
                const V c0r = V::load(xr + i2 + k), c0i = V::load(xi + i2 + k);
                const V d0r = V::load(xr + i3 + k), d0i = V::load(xi + i3 + k);
                const V br = w1rv * b0r - w1iv * b0i, bi = w1rv * b0i + w1iv * b0r;
                const V cr = w2rv * c0r - w2iv * c0i, ci = w2rv * c0i + w2iv * c0r;
                const V dr = w3rv * d0r - w3iv * d0i, di = w3rv * d0i + w3iv * d0r;
                const V t0r = ar + cr, t0i = ai + ci, t1r = ar - cr, t1i = ai - ci;
                const V t2r = br + dr, t2i = bi + di, t3r = br - dr, t3i = bi - di;
                const V rsr = V(0.0) - sg * t3i, rsi = sg * t3r;
                (t0r + t2r).store(yr + o0 + k);
                (t0i + t2i).store(yi + o0 + k);
                (t1r - rsr).store(yr + o1 + k);
                (t1i - rsi).store(yi + o1 + k);
                (t0r - t2r).store(yr + o2 + k);
                (t0i - t2i).store(yi + o2 + k);
                (t1r + rsr).store(yr + o3 + k);
                (t1i + rsi).store(yi + o3 + k);
            }
        }
        else if constexpr (std::is_same_v<T, crd::f32>)
        {
            namespace simd = crd::math::simd;
            using V = simd::Vec8f;
            const V w1rv(w1r), w1iv(w1i), w2rv(w2r), w2iv(w2i), w3rv(w3r), w3iv(w3i), sg(isign);
            for (; k + 8 <= r; k += 8)
            {
                const V ar = V::load(xr + i0 + k), ai = V::load(xi + i0 + k);
                const V b0r = V::load(xr + i1 + k), b0i = V::load(xi + i1 + k);
                const V c0r = V::load(xr + i2 + k), c0i = V::load(xi + i2 + k);
                const V d0r = V::load(xr + i3 + k), d0i = V::load(xi + i3 + k);
                const V br = w1rv * b0r - w1iv * b0i, bi = w1rv * b0i + w1iv * b0r;
                const V cr = w2rv * c0r - w2iv * c0i, ci = w2rv * c0i + w2iv * c0r;
                const V dr = w3rv * d0r - w3iv * d0i, di = w3rv * d0i + w3iv * d0r;
                const V t0r = ar + cr, t0i = ai + ci, t1r = ar - cr, t1i = ai - ci;
                const V t2r = br + dr, t2i = bi + di, t3r = br - dr, t3i = bi - di;
                const V rsr = V(0.0F) - sg * t3i, rsi = sg * t3r;
                (t0r + t2r).store(yr + o0 + k);
                (t0i + t2i).store(yi + o0 + k);
                (t1r - rsr).store(yr + o1 + k);
                (t1i - rsi).store(yi + o1 + k);
                (t0r - t2r).store(yr + o2 + k);
                (t0i - t2i).store(yi + o2 + k);
                (t1r + rsr).store(yr + o3 + k);
                (t1i + rsi).store(yi + o3 + k);
            }
        }
        for (; k < r; ++k)
        {
            const T ar = xr[i0 + k];
            const T ai = xi[i0 + k];
            const T b0r = xr[i1 + k];
            const T b0i = xi[i1 + k];
            const T c0r = xr[i2 + k];
            const T c0i = xi[i2 + k];
            const T d0r = xr[i3 + k];
            const T d0i = xi[i3 + k];
            const T br = w1r * b0r - w1i * b0i;
            const T bi = w1r * b0i + w1i * b0r;
            const T cr = w2r * c0r - w2i * c0i;
            const T ci = w2r * c0i + w2i * c0r;
            const T dr = w3r * d0r - w3i * d0i;
            const T di = w3r * d0i + w3i * d0r;
            const T t0r = ar + cr;
            const T t0i = ai + ci;
            const T t1r = ar - cr;
            const T t1i = ai - ci;
            const T t2r = br + dr;
            const T t2i = bi + di;
            const T t3r = br - dr;
            const T t3i = bi - di;
            const T rsr = -isign * t3i; // isign · (i·t3)
            const T rsi = isign * t3r;
            yr[o0 + k] = t0r + t2r;
            yi[o0 + k] = t0i + t2i;
            yr[o1 + k] = t1r - rsr;
            yi[o1 + k] = t1i - rsi;
            yr[o2 + k] = t0r - t2r;
            yi[o2 + k] = t0i - t2i;
            yr[o3 + k] = t1r + rsr;
            yi[o3 + k] = t1i + rsi;
        }
    }

    // One radix-2 Stockham (j) group: r unit-stride butterflies sharing the twiddle (wr,wi). SIMD over k.
    static void butterfly_row(const T* xr, const T* xi, T* yr, T* yi, crd::usize i0, crd::usize i1, crd::usize o0,
                              crd::usize o1, crd::usize r, T wr, T wi) noexcept
    {
        crd::usize k = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            namespace simd = crd::math::simd;
            const simd::Vec4d wrv(wr);
            const simd::Vec4d wiv(wi);
            for (; k + 4 <= r; k += 4)
            {
                const simd::Vec4d ar = simd::Vec4d::load(xr + i0 + k);
                const simd::Vec4d ai = simd::Vec4d::load(xi + i0 + k);
                const simd::Vec4d br = simd::Vec4d::load(xr + i1 + k);
                const simd::Vec4d bi = simd::Vec4d::load(xi + i1 + k);
                const simd::Vec4d trv = wrv * br - wiv * bi; // w·b
                const simd::Vec4d tiv = wrv * bi + wiv * br;
                (ar + trv).store(yr + o0 + k);
                (ai + tiv).store(yi + o0 + k);
                (ar - trv).store(yr + o1 + k);
                (ai - tiv).store(yi + o1 + k);
            }
        }
        else if constexpr (std::is_same_v<T, crd::f32>)
        {
            namespace simd = crd::math::simd;
            const simd::Vec8f wrv(wr);
            const simd::Vec8f wiv(wi);
            for (; k + 8 <= r; k += 8)
            {
                const simd::Vec8f ar = simd::Vec8f::load(xr + i0 + k);
                const simd::Vec8f ai = simd::Vec8f::load(xi + i0 + k);
                const simd::Vec8f br = simd::Vec8f::load(xr + i1 + k);
                const simd::Vec8f bi = simd::Vec8f::load(xi + i1 + k);
                const simd::Vec8f trv = wrv * br - wiv * bi;
                const simd::Vec8f tiv = wrv * bi + wiv * br;
                (ar + trv).store(yr + o0 + k);
                (ai + tiv).store(yi + o0 + k);
                (ar - trv).store(yr + o1 + k);
                (ai - tiv).store(yi + o1 + k);
            }
        }
        for (; k < r; ++k) // scalar remainder (the last passes have r < SIMD width)
        {
            const T are = xr[i0 + k];
            const T aim = xi[i0 + k];
            const T bre = xr[i1 + k];
            const T bim = xi[i1 + k];
            const T trv = wr * bre - wi * bim;
            const T tiv = wr * bim + wi * bre;
            yr[o0 + k] = are + trv;
            yi[o0 + k] = aim + tiv;
            yr[o1 + k] = are - trv;
            yi[o1 + k] = aim - tiv;
        }
    }

    [[nodiscard]] static crd::usize bitrev(crd::usize x, crd::u32 bits) noexcept
    {
        crd::usize res = 0;
        for (crd::u32 b = 0; b < bits; ++b)
        {
            res = (res << 1) | (x & 1U);
            x >>= 1;
        }
        return res;
    }

    crd::memory::IAllocator* m_alloc; // owns the hoisted four-step sub-plans + raw aligned buffers (dtor frees)
    crd::usize m_n;
    crd::u32 m_log2;
    crd::usize m_n1 = 0;              // four-step split ≈ √n (n2 = n/n1)
    bool m_use_four_step = false;
    FftPlan<T>* m_p1 = nullptr;       // Lever D: hoisted length-n1 sub-FFT (ctor-built ONCE)
    FftPlan<T>* m_p2 = nullptr;       // Lever D: hoisted length-n2 sub-FFT
    Complex<T>* m_tbuf = nullptr;     // Lever D: transpose intermediate, 64B-aligned (NT-store target)
    Complex<T>* m_scratch = nullptr;  // Lever D: cache-resident block scratch, 64B-aligned
    crd::containers::Array<T> m_tw_re;
    crd::containers::Array<T> m_tw_im;
    crd::containers::Array<crd::usize> m_rev;
    mutable crd::containers::Array<T> m_re0; // SoA ping-pong scratch (one plan per thread)
    mutable crd::containers::Array<T> m_im0;
    mutable crd::containers::Array<T> m_re1;
    mutable crd::containers::Array<T> m_im1;
    mutable crd::containers::Array<Complex<T>> m_abuf; // six-step transpose buffers (ctor-allocated)
    mutable crd::containers::Array<Complex<T>> m_bbuf;
    // Lever D twiddle FACTORIZATION (the read-floor cut): the inter-stage twiddle W_n^{i2·k1} is NOT a stored
    // n-sized table (that was 128 MB of streaming DRAM read per transform @8M — a third of the four-step read
    // floor, pure overhead MKL never pays). Instead W_n^a = W_n^{a_hi·M}·W_n^{a_lo} from two ~√n L2-resident
    // tables (a = i2·k1 < n, a_lo = a & (M−1), a_hi = a >> m_ftw_h, M = 1<<m_ftw_h) + one complex multiply.
    crd::containers::Array<T> m_ftw_hi_re; // W_n^{j·M}, j ∈ [0, n/M)
    crd::containers::Array<T> m_ftw_hi_im;
    crd::containers::Array<T> m_ftw_lo_re; // W_n^{j},   j ∈ [0, M)
    crd::containers::Array<T> m_ftw_lo_im;
    crd::u32 m_ftw_h = 0; // split shift: M = 1<<m_ftw_h ≈ √n
    crd::containers::Array<T> m_ptw_re; // Lever A: per-pass combine twiddles, flat in (j,m) pass order
    crd::containers::Array<T> m_ptw_im;
    crd::u32 m_rmax_bits = 3U; // size-aware max radix bits/pass (set in ctor; mirrored by build + execute)
};

// One-shot: builds a transient plan, transforms in place. Forward + inverse UNNORMALIZED.
template <typename T>
void fft(crd::memory::IAllocator* alloc, crd::containers::Span<Complex<T>> data,
         FftDirection dir = FftDirection::Forward)
{
    const FftPlan<T> plan(alloc, data.size());
    plan.execute(data, dir);
}

// Normalized inverse: ifft_normalized(fft(x)) == x.
template <typename T> void ifft_normalized(crd::memory::IAllocator* alloc, crd::containers::Span<Complex<T>> data)
{
    const crd::usize n = data.size();
    fft<T>(alloc, data, FftDirection::Inverse);
    if (n == 0)
    {
        return;
    }
    const T inv = static_cast<T>(1) / static_cast<T>(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        data[i].re *= inv;
        data[i].im *= inv;
    }
}

} // namespace crd::hesap::fft
