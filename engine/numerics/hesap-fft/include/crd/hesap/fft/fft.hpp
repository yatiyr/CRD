#pragma once

// fft.hpp — Phase 3.1.6 v10: complex FFT. The DETERMINISTIC-PLAN contract (the v10 thesis): the algorithm
// is chosen purely from the size factorization — NO runtime measurement or autotuning (the plan is a pure
// function of the size, so it is reproducible across builds and runs). One twiddle table is precomputed per
// plan and shared ⇒ cross-THREAD bit-identical (NOT cross-compiler: sin/cos ±1 ULP). Lower-layer RAW per
// ADR-0078 (Complex<f32/f64>).
//
// v10-a: radix-2 (the correct baseline + the brute-force-DFT gate). v10-b: STOCKHAM AUTOSORT (no
// bit-reversal — kills the large-N cache collapse) on SPLIT SoA buffers + AVX2 SIMD over the unit-stride
// butterfly index ⇒ high throughput. The radix-2 in-place stays as `execute_reference` — the trusted oracle
// the Stockham path is cross-checked against. (radix-4/8 codelets + Bailey four-step extend this; mixed-radix
// → v10-c-prep, Bluestein/Rader → v10-c.)
//
// Normalization: forward + inverse BOTH UNNORMALIZED; `ifft_normalized` applies 1/n.
// THREADING: FftPlan owns its scratch ⇒ one plan per thread (plans are cheap + deterministic to build;
// batched-parallel uses per-worker plans, v10-e).
//
// The plan is built here in general code; the transform runs in the per-CPU-level kernels (fft_kernels.hpp,
// SIMD-DISPATCH), which read the plan's FftPlanState.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/detail/block_width.hpp>
#include <crd/hesap/fft/fft_dispatch.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp> // allocate_array_or_fatal / construct_or_fatal (DIAG.3a)

#include <type_traits>

namespace crd::hesap::fft
{

[[nodiscard]] constexpr bool is_pow2(crd::usize n) noexcept
{
    return n != 0 && (n & (n - 1)) == 0;
}

namespace detail
{
// DIAG.3a: a plan cannot be built without its tables and sub-plans, so an allocator that refuses one (an exhausted
// sub-budget allocator) leaves the plan out of memory, fatal in every build, never a null table written
// (crd-hesap-fft-fatal-* modes).
inline constexpr const char* kFftPlanOutOfMemory = "FftPlan: out of memory for its tables";

// THE PLANNER: every per-size choice of a type's plan, measured against MKL row by row (FFT-CRUSH 2026-07,
// docs/research/fft-stockham-v2.md) and a pure function of the size. SIMD-DISPATCH's row-close board times every
// algorithm forward_builds offers at a size against the planned one at every level; a size's winner becomes its plan.

// The four-step's sizes. From 2^19 the direct passes are DRAM-bound (log4(n) full passes) and the Bailey four-step
// keeps the sub-FFTs cache-resident (O(1) DRAM passes). FFT-CRUSH 2026-07-03 opted 128K and 256K in for both types
// (f32 256K 0.33x -> about 0.85x MKL, f32 128K 0.17x as direct Stockham, f64 64K-256K 0.40-0.49x); a forward the
// deep-split takes still runs its inverse here.
[[nodiscard]] constexpr bool four_step_planned(crd::usize n) noexcept
{
    return n == (crd::usize{1} << 17) || n == (crd::usize{1} << 18) || n >= (crd::usize{1} << 19);
}

// The four-step split n = n1 * n2: n1 = 2^ceil(log2/2), the larger factor first (a full-FFT n1/n2 search 2026-06-15
// and the 2026-06-16 factorization sweep: 8M +9%, 2M +13%, f32 8M +10%), except the all-hier splits of the mid band:
// 128K and 256K = 1024 x n2 (P2 = codelet128, or the 16x16 hier at 256), f32 512K = 2048 x 256 (measured 0.55x
// before).
template <typename T> [[nodiscard]] constexpr crd::usize four_step_split(crd::usize n, crd::u32 log2) noexcept
{
    if (n == (crd::usize{1} << 17) || n == (crd::usize{1} << 18))
    {
        return 1024U;
    }
    if (std::is_same_v<T, crd::f32> && n == (crd::usize{1} << 19))
    {
        return 2048U;
    }
    return crd::usize{1} << ((log2 + 1U) / 2U);
}

// IP4-AoS, both directions: f64 wins every row 1K..64K; f32 only at 2K and 16K..64K (the matched-state A/B of
// 2026-07-04: the Vec8f standalone-hier wins 1K, 4K and 8K).
template <typename T> [[nodiscard]] constexpr bool ip4_planned(crd::usize n) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        return n >= 1024 && n <= 65536;
    }
    else
    {
        return n == 2048 || (n >= 16384 && n <= 65536);
    }
}

// The forward algorithm of a size. The deep-split runs 8K..1M as four passes (the 2026-07-04 K-stage sweep; its 4096
// = 16x16x16 lost, f32 8K lost 10% at eight lanes, and f64 1M takes the six-step, 5.38 against 5.88 ms); past L3 the
// four-step's blocked structure wins (2M flat, 4M and 8M large losses under the deep-split).
template <typename T> [[nodiscard]] constexpr FftAlgorithm planned_forward(crd::usize n) noexcept
{
    constexpr bool is_f32 = std::is_same_v<T, crd::f32>;
    if (n < 1024)
    {
        return FftAlgorithm::Stockham;
    }
    if (ip4_planned<T>(n))
    {
        return FftAlgorithm::Ip4;
    }
    if (n >= (crd::usize{1} << 13) && n <= (crd::usize{1} << 20) && n != (crd::usize{1} << (is_f32 ? 13 : 20)))
    {
        return FftAlgorithm::DeepSplit4;
    }
    if (!is_f32 && n == (crd::usize{1} << 20))
    {
        return FftAlgorithm::SixStep;
    }
    if (four_step_planned(n))
    {
        return FftAlgorithm::FourStep;
    }
    return FftAlgorithm::StandaloneHier; // f32 1K, 4K and 8K
}

// The inverse algorithm of a size: IP4-AoS where it takes the forward, the four-step at its sizes, Stockham below.
template <typename T> [[nodiscard]] constexpr FftAlgorithm planned_inverse(crd::usize n) noexcept
{
    if (ip4_planned<T>(n))
    {
        return FftAlgorithm::Ip4;
    }
    return four_step_planned(n) ? FftAlgorithm::FourStep : FftAlgorithm::Stockham;
}

// Whether a plan of n can run `forward`, the sizes each algorithm's generated codelets cover: the standalone-hier's
// splits and IP4-AoS 1K..64K, the deep-split's factor tables (three passes 8K..512K, four 8K..8M), the six-step's
// square blocks of 64 rows, the tiled four-step's f32 1M.
template <typename T> [[nodiscard]] constexpr bool forward_builds(crd::usize n, FftAlgorithm forward) noexcept
{
    const bool even = is_pow2(n) && (n & crd::usize{0x5555555555555555ULL}) != 0;
    switch (forward)
    {
        case FftAlgorithm::Stockham:
            return n >= 1;
        case FftAlgorithm::StandaloneHier:
        case FftAlgorithm::Ip4:
            return n >= 1024 && n <= 65536;
        case FftAlgorithm::DeepSplit3:
            return n >= (crd::usize{1} << 13) && n <= (crd::usize{1} << 19);
        case FftAlgorithm::DeepSplit4:
            return n >= (crd::usize{1} << 13) && n <= (crd::usize{1} << 23);
        case FftAlgorithm::FourStep:
            return n >= 1024;
        case FftAlgorithm::SixStep:
            return even && n >= 4096;
        case FftAlgorithm::TiledFourStep:
            return std::is_same_v<T, crd::f32> && n == (crd::usize{1} << 20);
    }
    return false;
}

// Every algorithm, in FftAlgorithm order: the board and the tests walk the ones forward_builds offers at a size.
inline constexpr FftAlgorithm kFftAlgorithms[] = {
    FftAlgorithm::Stockham,   FftAlgorithm::StandaloneHier, FftAlgorithm::Ip4,     FftAlgorithm::DeepSplit3,
    FftAlgorithm::DeepSplit4, FftAlgorithm::FourStep,       FftAlgorithm::SixStep, FftAlgorithm::TiledFourStep,
};
} // namespace detail

template <typename T> class FftPlan
{
public:
    FftPlan(crd::memory::IAllocator* alloc, crd::usize n) : FftPlan(alloc, n, detail::planned_forward<T>(n)) {}

    // A plan whose forward runs `forward`, an algorithm detail::forward_builds offers at n (the row-close board and
    // the tests run each against the planned one); its inverse stays the planner's. Only the six-step changes the
    // four-step split its inverse shares, to the square one.
    FftPlan(crd::memory::IAllocator* alloc, crd::usize n, FftAlgorithm forward)
        : m_alloc(alloc), m_tw_re(alloc), m_tw_im(alloc), m_rev(alloc), m_re0(alloc), m_im0(alloc), m_re1(alloc),
          m_im1(alloc), m_ftw_hi_re(alloc), m_ftw_hi_im(alloc), m_ftw_lo_re(alloc), m_ftw_lo_im(alloc), m_ptw_re(alloc),
          m_ptw_im(alloc)
    {
        CRD_ASSERT(is_pow2(n)); // v10: power-of-2 (Bluestein/Rader make any size O(n log n) at v10-c)
        CRD_ASSERT(detail::forward_builds<T>(n, forward));
        m_s.n = n;
        while ((crd::usize{1} << m_s.log2) < n)
        {
            ++m_s.log2;
        }
        m_s.forward = forward;
        m_s.inverse = detail::planned_inverse<T>(n);
        m_s.n1 = detail::four_step_split<T>(n, m_s.log2);
        if (forward == FftAlgorithm::SixStep)
        {
            m_s.n1 = crd::usize{1} << (m_s.log2 / 2U);
        }
        // Full table W_n^k, k = 0 .. n-1 (radix-4 indexes up to 3·j·r < n). Precomputed ONCE, shared — the
        // determinism contract. Computed in f64 then narrowed (accuracy for the f32 plan).
        m_tw_re.resize(n);
        m_tw_im.resize(n);
        for (crd::usize k = 0; k < n; ++k)
        {
            const double ang = -kTwoPi * static_cast<double>(k) / static_cast<double>(n);
            m_tw_re[k] = static_cast<T>(crd::math::cos(ang)); //  cos(2πk/n)
            m_tw_im[k] = static_cast<T>(crd::math::sin(ang)); // -sin(2πk/n) ⇒ (re,im) == W_n^k
        }
        m_rev.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_rev[i] = bitrev(i, m_s.log2);
        }
        build_hier_sub_fft();
        const bool deep = forward == FftAlgorithm::DeepSplit3 || forward == FftAlgorithm::DeepSplit4;
        const bool hier = forward == FftAlgorithm::StandaloneHier;
        const bool ip = forward == FftAlgorithm::Ip4 || m_s.inverse == FftAlgorithm::Ip4;
        if (deep || hier || ip) // the passes' intermediate (IP4-AoS's gather target)
        {
            m_s.sh_t = plan_table<Complex<T>>(n, 64);
        }
        if (deep)
        {
            build_deep_split(forward == FftAlgorithm::DeepSplit4);
        }
        else if (hier)
        {
            m_s.sh_n1 = hier_split(n);
        }
        if (deep || hier)
        {
            build_stage1_twiddles();
        }
        if (ip)
        {
            build_ip4();
        }
        if (m_s.inverse == FftAlgorithm::FourStep || forward == FftAlgorithm::FourStep ||
            forward == FftAlgorithm::SixStep || forward == FftAlgorithm::TiledFourStep)
        {
            build_four_step(forward == FftAlgorithm::TiledFourStep);
        }
        if (forward == FftAlgorithm::Stockham || m_s.inverse == FftAlgorithm::Stockham)
        {
            build_stockham();
        }
        bind_state();
    }

    // Owns the hoisted four-step sub-plans + raw aligned buffers ⇒ the plan is PINNED (never copied/moved
    // in-tree; verified). Deleting copy+move makes any accidental copy a compile error, not a double-free.
    ~FftPlan()
    {
        if (m_p2 != nullptr)
        {
            m_p2->~FftPlan();
            m_alloc->deallocate(m_p2);
        }
        if (m_p1 != nullptr)
        {
            m_p1->~FftPlan();
            m_alloc->deallocate(m_p1);
        }
        release(m_s.scratch);
        release(m_s.tbuf);
        release(m_s.sh_s);
        release(m_s.sh_t);
        release(m_s.sh_twr);
        release(m_s.sh_twi);
        release(m_s.sh_hir);
        release(m_s.sh_hii);
        release(m_s.ds_twr);
        release(m_s.ds_twi);
        release(m_s.ds_tw2r);
        release(m_s.ds_tw2i);
        release(m_s.ip_rev);
        release(m_s.ip_twr);
        release(m_s.ip_twi);
        release(m_s.hier_bbuf);
        release(m_s.hier_twr);
        release(m_s.hier_twi);
        release(m_s.tf_nt);
        release(m_s.tf_twr);
        release(m_s.tf_twi);
        release(m_s.tf_bsr);
        release(m_s.tf_bsi);
        release(m_s.tf_lnr);
        release(m_s.tf_lni);
    }
    FftPlan(const FftPlan&) = delete;
    FftPlan& operator=(const FftPlan&) = delete;
    FftPlan(FftPlan&&) = delete;
    FftPlan& operator=(FftPlan&&) = delete;

    // Build the flat per-pass twiddle table (Lever A). Replays the SAME size-aware planner as `execute`, so the
    // table is laid out in pass order; `execute` walks it with a running pointer. m*j*r ≤ n-1 (bounded).
    void build_combine_twiddles()
    {
        if (m_s.log2 < 3U) // n < 8: leaf-codelet path, no combine passes (also guards m_s.log2−2 underflow)
        {
            return;
        }
        crd::usize total = 0;
        for (crd::u32 t = (m_s.log2 & 1U) ? 1U : 2U; t < m_s.log2 - 2U;)
        {
            const crd::u32 rem = (m_s.log2 - 2U) - t;
            crd::u32 b = m_s.rmax_bits < rem ? m_s.rmax_bits : rem;
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
        for (crd::u32 t = (m_s.log2 & 1U) ? 1U : 2U; t < m_s.log2 - 2U;)
        {
            const crd::u32 rem = (m_s.log2 - 2U) - t;
            crd::u32 b = m_s.rmax_bits < rem ? m_s.rmax_bits : rem;
            while (b > 2U && (rem - b) == 1U)
            {
                --b;
            }
            const crd::usize lq = crd::usize{1} << t;
            const crd::usize radix = crd::usize{1} << b;
            const crd::usize r = m_s.n >> (t + b);
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

    [[nodiscard]] crd::usize size() const noexcept { return m_s.n; }

    // The PRODUCTION transform (Stockham autosort, split SoA, the generated codelets, the four-step at large n), in
    // place, at the kernel level the process runs.
    void execute(crd::containers::Span<Complex<T>> data, FftDirection dir) const
    {
        CRD_ASSERT(data.size() == m_s.n);
        fft_kernels_of<T>().execute(m_s, data.data(), dir);
    }

    // The radix-2 in-place reference (the v10-a code) — the trusted oracle for cross-checking `execute`.
    // Bit-reversal + log2(n) butterfly stages, precomputed twiddles.
    void execute_reference(crd::containers::Span<Complex<T>> data, FftDirection dir) const
    {
        CRD_ASSERT(data.size() == m_s.n);
        if (m_s.n <= 1)
        {
            return;
        }
        for (crd::usize i = 0; i < m_s.n; ++i)
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
        for (crd::usize len = 2; len <= m_s.n; len <<= 1)
        {
            const crd::usize half = len >> 1;
            const crd::usize step = m_s.n / len;
            for (crd::usize base = 0; base < m_s.n; base += len)
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

    // BATCHED FFT: `batch` independent size-n transforms in ELEMENT-MAJOR layout (element i of transform t at
    // data[i*batch + t]), in place, vectorized over the contiguous batch axis. This is the v10-e batched primitive
    // AND the sub-FFT engine of the four-step. Same butterfly + twiddle convention as execute_reference (the
    // oracle), so cross-checkable.
    void execute_batched(crd::containers::Span<Complex<T>> data, crd::usize batch, FftDirection dir) const
    {
        CRD_ASSERT(data.size() == m_s.n * batch);
        fft_kernels_of<T>().execute_batched(m_s, data.data(), batch, dir);
    }

    // The plan as the kernels see it.
    [[nodiscard]] const FftPlanState<T>& state() const noexcept { return m_s; }

private:
    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    // A raw table of `count` Us at `alignment` (64 or 32: the aligned SIMD loads and the streaming stores need it). A
    // refused table, and a count whose byte size overflows, are the plan's out-of-memory fatal (DIAG.3a).
    template <typename U> [[nodiscard]] U* plan_table(crd::usize count, crd::usize alignment)
    {
        return crd::memory::allocate_array_or_fatal<U>(*m_alloc, count, alignment, detail::kFftPlanOutOfMemory);
    }
    // A table plan_table gave, or null.
    template <typename U> void release(U* table) noexcept
    {
        if (table != nullptr)
        {
            m_alloc->deallocate(table);
        }
    }
    // A four-step sub-plan of size `n`, built from the same allocator.
    [[nodiscard]] FftPlan* sub_plan(crd::usize n)
    {
        return crd::memory::construct_or_fatal<FftPlan>(*m_alloc, detail::kFftPlanOutOfMemory, m_alloc, n);
    }

    // The hierarchical generated-codelet sub-FFTs, which execute_batched runs when this plan is a four-step's
    // sub-plan: 4096 = 64×64, 2048 = 64×32, 1024 = 32×32, and f32 256 = 16×16 (the 256K four-step's P2; the codelet16
    // stage 1 is Vec8f). N1 = stage-1 leaf (twiddle stride), N2 = inner stage-2 leaf: the transposed intermediate
    // (n·BB complex) and the inner twiddle W_N^{n2·k1} (index n2·N1+k1). block_width reproduces the f64 BBs (4096→16,
    // 2048→32, 1024→64); f32's is twice that.
    void build_hier_sub_fft()
    {
        const crd::usize n = m_s.n;
        crd::usize h_n2 = 0U; // N2 = inner count
        crd::usize h_n1 = 64U;
        if (n == 4096)
        {
            h_n2 = 64U;
        }
        else if (n == 2048)
        {
            h_n2 = 32U;
        }
        else if (n == 1024)
        {
            h_n2 = 32U;
            h_n1 = 32U;
        }
        else if (n == 256 && std::is_same_v<T, crd::f32>)
        {
            h_n2 = 16U;
            h_n1 = 16U;
        }
        if (h_n2 == 0)
        {
            return;
        }
        m_s.hier_bbuf = plan_table<Complex<T>>(n * detail::block_width<T>(n), 64);
        m_s.hier_twr = plan_table<T>(h_n2 * h_n1, 32);
        m_s.hier_twi = plan_table<T>(h_n2 * h_n1, 32);
        for (crd::usize n2 = 0; n2 < h_n2; ++n2)
        {
            for (crd::usize k1 = 0; k1 < h_n1; ++k1)
            {
                const double th = kTwoPi * static_cast<double>(n2 * k1) / static_cast<double>(n);
                m_s.hier_twr[n2 * h_n1 + k1] = static_cast<T>(crd::math::cos(th));
                m_s.hier_twi[n2 * h_n1 + k1] = static_cast<T>(-crd::math::sin(th));
            }
        }
    }

    // The standalone-hier split n = n1·n2 (FFT-CRUSH 2026-07-03): stage 1 = codelet_n1_stage1_fused_sh (leaf +
    // twiddle + transposed store, one pass), stage 2 = codelet_n2_batched in natural order.
    [[nodiscard]] static constexpr crd::usize hier_split(crd::usize n) noexcept
    {
        if (n == 1024)
        {
            return 32U;
        }
        if (n <= 8192)
        {
            return 64U;
        }
        return n == 16384 ? 128U : 256U; // 32768 = 256×128, 65536 = 256×256
    }

    // The deep-split n = A·B·C (three generated passes: S1 = codeletA_stage1_fused_sh, leaf + full W_n twiddle +
    // transposed store; S2 = codeletB_fused_notr, leaf + broadcast W_{BC} + natural store; S3 =
    // codeletC_batched_strided per kB, L1-resident reads, natural-order writes d[kA + A·kB + AB·kC]) or n = A·B1·B2·C
    // (four passes, MKL's pass shape: small live sets per stage). Forward only.
    void build_deep_split(bool four_stage)
    {
        const crd::usize n = m_s.n;
        if (four_stage)
        {
            // The 2026-07-04 K-stage sweep's factors (1M = 32·32·32·32 was the f32 winner, 2.24 ms, 0.91x MKL).
            struct Factors
            {
                crd::u16 a, b1, b2, c;
            };
            constexpr Factors four_stage_factors[] = {
                {16, 8, 8, 8},    {16, 16, 8, 8},   {16, 16, 16, 8},  {16, 16, 16, 16},
                {32, 16, 16, 16}, {32, 32, 16, 16}, {32, 32, 32, 16}, {32, 32, 32, 32},
                {32, 32, 32, 64}, {32, 32, 64, 64}, {64, 32, 64, 64},
            };
            const Factors& f = four_stage_factors[m_s.log2 - 13U]; // 8K .. 8M
            m_s.sh_n1 = f.a;
            m_s.ds_b = f.b1;
            m_s.ds_b2 = f.b2;
            m_s.ds_c = f.c;
        }
        else
        {
            // 8K = 32·16·16, 16K = 32·32·16, 32K = 32·32·32, 64K = 32·32·64, 128K = 32·64·64, 256K = 64·64·64, 512K =
            // 64·64·128 (B = 128 measured worse at 512K: S2's 128-leaf runs 128 + 128 streams, 6.96 against 4.4 ms).
            m_s.sh_n1 = (n <= (crd::usize{1} << 17)) ? 32U : 64U;
            m_s.ds_b = 64U;
            if (n <= (crd::usize{1} << 13))
            {
                m_s.ds_b = 16U;
            }
            else if (n <= (crd::usize{1} << 16))
            {
                m_s.ds_b = 32U;
            }
            m_s.ds_c = 64U;
            if (n <= (crd::usize{1} << 14))
            {
                m_s.ds_c = 16U;
            }
            else if (n == (crd::usize{1} << 15))
            {
                m_s.ds_c = 32U;
            }
            else if (n == (crd::usize{1} << 19))
            {
                m_s.ds_c = 128U;
            }
        }
        // 2^ash = A >= 16 columns, so S2's broadcast twiddle is one value over a codelet step of two tiles (sixteen
        // f32 columns at avx512).
        while ((crd::usize{1} << m_s.ds_ash) < m_s.sh_n1)
        {
            ++m_s.ds_ash;
        }
        m_s.sh_s = plan_table<Complex<T>>(n, 64);
        // last-notr-stage table: W_{B·C} (3-stage) or W_{B2·C} (4-stage), [k*C + v]
        const crd::usize lb = (m_s.ds_b2 != 0) ? m_s.ds_b2 : m_s.ds_b;
        const crd::usize bc = lb * m_s.ds_c;
        m_s.ds_twr = plan_table<T>(bc, 64);
        m_s.ds_twi = plan_table<T>(bc, 64);
        for (crd::usize kb = 0; kb < lb; ++kb)
        {
            for (crd::usize v = 0; v < m_s.ds_c; ++v)
            {
                const double th = kTwoPi * static_cast<double>((kb * v) % bc) / static_cast<double>(bc);
                m_s.ds_twr[kb * m_s.ds_c + v] = static_cast<T>(crd::math::cos(th));
                m_s.ds_twi[kb * m_s.ds_c + v] = static_cast<T>(-crd::math::sin(th));
            }
        }
        if (m_s.ds_b2 != 0) // S2 table for the 4-stage: W_{B1·B2·C}, [k*(B2·C) + v]
        {
            const crd::usize b2c = m_s.ds_b2 * m_s.ds_c;
            const crd::usize e = m_s.ds_b * b2c;
            m_s.ds_tw2r = plan_table<T>(e, 64);
            m_s.ds_tw2i = plan_table<T>(e, 64);
            for (crd::usize kb = 0; kb < m_s.ds_b; ++kb)
            {
                for (crd::usize v = 0; v < b2c; ++v)
                {
                    const double th = kTwoPi * static_cast<double>((kb * v) % e) / static_cast<double>(e);
                    m_s.ds_tw2r[kb * b2c + v] = static_cast<T>(crd::math::cos(th));
                    m_s.ds_tw2i[kb * b2c + v] = static_cast<T>(-crd::math::sin(th));
                }
            }
        }
    }

    // The standalone-hier's and the deep-split's stage-1 twiddle W_n^{k1·i2}, in the stage-1-output layout. Where n2
    // >= 1024 the full n-entry table streams as many bytes as the data every call, so it is FACTORED: W_n^{k·u} =
    // hi[k,u>>msh] · lo[k,u&(M-1)] from L1-resident tables (an exact index split, two extra FMAs an output). It wins
    // only where the table leaves L2 (f64 128K +4%, 256K +3%, f32 128K +14%); at n2 <= 256 the extra operations lose
    // 3-6%.
    void build_stage1_twiddles()
    {
        const crd::usize n = m_s.n;
        const crd::usize sn2 = n / m_s.sh_n1;
        if (sn2 >= 1024)
        {
            crd::u32 n2log = 0;
            while ((crd::usize{1} << n2log) < sn2)
            {
                ++n2log;
            }
            // M >= a codelet step of two tiles (16 f32 or 8 f64 columns at avx512): its lo loads stay in one row of
            // the table and its hi value is one broadcast.
            const crd::u32 lmin = std::is_same_v<T, crd::f32> ? 4U : 3U;
            m_s.sh_msh = (n2log + 1U) / 2U;
            if (m_s.sh_msh < lmin)
            {
                m_s.sh_msh = lmin;
            }
            const crd::usize mm = crd::usize{1} << m_s.sh_msh;
            const crd::usize uhc = sn2 >> m_s.sh_msh;
            m_s.sh_twr = plan_table<T>(m_s.sh_n1 * mm, 64); // LO re
            m_s.sh_twi = plan_table<T>(m_s.sh_n1 * mm, 64); // LO im
            m_s.sh_hir = plan_table<T>(m_s.sh_n1 * uhc, 64);
            m_s.sh_hii = plan_table<T>(m_s.sh_n1 * uhc, 64);
            for (crd::usize k1 = 0; k1 < m_s.sh_n1; ++k1)
            {
                for (crd::usize ul = 0; ul < mm; ++ul)
                {
                    const double th = kTwoPi * static_cast<double>((k1 * ul) % n) / static_cast<double>(n);
                    m_s.sh_twr[k1 * mm + ul] = static_cast<T>(crd::math::cos(th));
                    m_s.sh_twi[k1 * mm + ul] = static_cast<T>(-crd::math::sin(th));
                }
                for (crd::usize uh = 0; uh < uhc; ++uh)
                {
                    const double th =
                        kTwoPi * static_cast<double>((k1 * (uh << m_s.sh_msh)) % n) / static_cast<double>(n);
                    m_s.sh_hir[k1 * uhc + uh] = static_cast<T>(crd::math::cos(th));
                    m_s.sh_hii[k1 * uhc + uh] = static_cast<T>(-crd::math::sin(th));
                }
            }
            return;
        }
        m_s.sh_twr = plan_table<T>(n, 64);
        m_s.sh_twi = plan_table<T>(n, 64);
        for (crd::usize k1 = 0; k1 < m_s.sh_n1; ++k1)
        {
            for (crd::usize i2 = 0; i2 < sn2; ++i2)
            {
                const double th = kTwoPi * static_cast<double>((k1 * i2) % n) / static_cast<double>(n);
                // stored in the STAGE-1-OUTPUT layout s[k1*n2 + i2] (applied before the transpose)
                m_s.sh_twr[k1 * sn2 + i2] = static_cast<T>(crd::math::cos(th));
                m_s.sh_twi[k1 * sn2 + i2] = static_cast<T>(-crd::math::sin(th));
            }
        }
    }

    // IP4-AoS's digit reversal and per-pass DIT twiddles (the interleaved in-place engine, both parities: odd log2 (n =
    // 2·4^k) is two half-length transforms on the even and odd decimations, the rev table absorbing the ×2+parity io
    // map, and one final radix-2 combine pass with a W_n^j table).
    void build_ip4()
    {
        const crd::usize n = m_s.n;
        const bool ipodd = (m_s.log2 & 1U) != 0U;
        const crd::usize nh = ipodd ? (n >> 1) : n; // 4^k length per half
        // Radix plan (round 17), innermost→outermost: [4,4 (the gather fold)], then nr8 radix-8 passes, then radix-4
        // to nh (the LAST pass must be radix-4 — the COBRA quad identity needs a base-4 top slot digit). Radix-8
        // MEASURED SLOWER than pure radix-4 on Raptor Cove (round 17: −3% v1, −1% block-paired — the W8 shuffle
        // diagonal + 7-set twiddle traffic outweigh the saved pass). Machinery kept, tested, oracle-green; 2U
        // re-enables it for future uarches.
        const crd::u32 nr8 = 0U;
        crd::u32 rads[16];
        crd::u32 nr = 0;
        rads[nr++] = 4U;
        rads[nr++] = 4U;
        const crd::u32 log2h = ipodd ? m_s.log2 - 1U : m_s.log2;
        for (crd::u32 i = 0; i < nr8; ++i)
        {
            rads[nr++] = 8U;
        }
        for (crd::u32 bits = log2h - 4U - 3U * nr8; bits != 0U; bits -= 2U)
        {
            rads[nr++] = 4U;
        }
        m_s.ip_rev = plan_table<crd::usize>(n, 64);
        for (crd::usize j = 0; j < nh; ++j)
        {
            // mixed-radix digit reverse: top slot digit = j mod r_last, Horner down
            crd::usize s = 0;
            crd::usize x = j;
            for (crd::u32 i = nr; i-- > 0U;)
            {
                const crd::usize d = x % rads[i];
                x /= rads[i];
                s = s * rads[i] + d;
            }
            // INVERSE map (mixed-radix reversal is NOT an involution): the gather wants io-index per SLOT ⇒
            // rev[slot(j)] = j (×2+parity for odd).
            if (ipodd)
            {
                m_s.ip_rev[s] = 2 * j;          // half 0 = even decimation
                m_s.ip_rev[s + nh] = 2 * j + 1; // half 1 = odd decimation
            }
            else
            {
                m_s.ip_rev[s] = j;
            }
        }
        // HYBRID tables (round 15): full 3-set (w1,w2,w3) runs for len ≤ 1024 — tiny (≤8KB), no in-register twiddle
        // powers needed — and w1-only for the BIG passes (their tables were the DTLB bulk; w2/w3 computed in-register
        // there); radix-8 passes get FULL 7-set tables. Non-duplicated storage throughout; dup expansion at load.
        m_s.ip_twr = plan_table<T>(n, 64);
        m_s.ip_twi = plan_table<T>(n, 64);
        crd::usize off = 0;
        crd::usize len = 4; // after the fold's first stage
        for (crd::u32 st = 1; st < nr; ++st)
        {
            const crd::usize r = rads[st];
            len *= r;
            const crd::usize q = len / r;
            crd::usize msets = 1U;
            if (r == 8U)
            {
                msets = 7U;
            }
            else if (len <= 1024)
            {
                msets = 3U;
            }
            for (crd::usize mset = 1; mset <= msets; ++mset)
            {
                for (crd::usize k = 0; k < q; ++k)
                {
                    const double th = kTwoPi * static_cast<double>((mset * k) % len) / static_cast<double>(len);
                    m_s.ip_twr[off] = static_cast<T>(crd::math::cos(th));
                    m_s.ip_twi[off] = static_cast<T>(-crd::math::sin(th));
                    ++off;
                }
            }
        }
        if (ipodd)
        {
            for (crd::usize j = 0; j < nh; ++j)
            {
                const double th = kTwoPi * static_cast<double>(j) / static_cast<double>(n);
                m_s.ip_twr[off] = static_cast<T>(crd::math::cos(th));
                m_s.ip_twi[off] = static_cast<T>(-crd::math::sin(th));
                ++off;
            }
        }
    }

    // The four-step (Lever D): the transpose intermediate and the cache-resident block scratch (raw 64-byte-aligned
    // tables: the NT stores need ≥32B), the factored twiddle and the hoisted sub-plans; the tiled four-step's tables
    // when `tiled`.
    void build_four_step(bool tiled)
    {
        const crd::usize n = m_s.n;
        m_s.tbuf = plan_table<Complex<T>>(n, 64);
        m_s.scratch = plan_table<Complex<T>>(n, 64);
        const crd::usize n2 = n / m_s.n1;
        // Two ~√n twiddle tables (the read-floor cut): W_n^a = W_n^{a_hi·M}·W_n^{a_lo}, M = 1<<m_s.ftw_h.
        // h = ⌊log2(n)/2⌋ ⇒ lo size M ≈ √n, hi size n/M ≈ √n; together ~96 KB @8M (L2-resident, not a
        // 128 MB streaming read). Both tables hold the (cos,−sin) convention so their product is W_n^a.
        m_s.ftw_h = m_s.log2 / 2U;
        const crd::usize m_split = crd::usize{1} << m_s.ftw_h; // M
        const crd::usize hi_count = n / m_split;               // n/M
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
        m_p1 = sub_plan(m_s.n1);
        m_p2 = sub_plan(n2);
        if (tiled)
        {
            build_tiled_four_step();
        }
    }

    // The Stockham passes: the SoA ping-pong scratch and Lever A's per-pass combine twiddles.
    void build_stockham()
    {
        m_re0.resize(m_s.n);
        m_im0.resize(m_s.n);
        m_re1.resize(m_s.n);
        m_im1.resize(m_s.n);
        m_s.rmax_bits = 3U;
        if (m_s.log2 >= 15U && m_s.log2 <= 20U)
        {
            m_s.rmax_bits = 5U;
        }
        else if (m_s.log2 >= 12U)
        {
            m_s.rmax_bits = 4U;
        }
        build_combine_twiddles();
    }

    // The tiled four-step's intermediate and twiddles (FftPlanState::tf_nt and its tables), each entry rounded once
    // from the angle of its exponent mod its period.
    void build_tiled_four_step()
    {
        constexpr crd::usize n = crd::usize{1} << 20;
        constexpr crd::usize half = 1024; // n1 = n2
        m_s.tf_nt = plan_table<T>(kTiledFourStepNt, 64);
        m_s.tf_twr = plan_table<T>(half, 64);
        m_s.tf_twi = plan_table<T>(half, 64);
        m_s.tf_bsr = plan_table<T>(64 * half, 64);
        m_s.tf_bsi = plan_table<T>(64 * half, 64);
        m_s.tf_lnr = plan_table<T>(16 * half, 64);
        m_s.tf_lni = plan_table<T>(16 * half, 64);
        const auto put = [](T* re, T* im, crd::usize at, crd::usize exponent, crd::usize period) noexcept
        {
            const double th = kTwoPi * static_cast<double>(exponent % period) / static_cast<double>(period);
            re[at] = static_cast<T>(crd::math::cos(th));
            im[at] = static_cast<T>(-crd::math::sin(th));
        };
        for (crd::usize q = 0; q < 32; ++q)
        {
            for (crd::usize c = 0; c < 32; ++c)
            {
                put(m_s.tf_twr, m_s.tf_twi, q * 32 + c, q * c, half);
            }
        }
        for (crd::usize i2 = 0; i2 < half; ++i2)
        {
            for (crd::usize kh = 0; kh < 64; ++kh)
            {
                put(m_s.tf_bsr, m_s.tf_bsi, kh * half + i2, i2 * kh * 16, n);
            }
            for (crd::usize kl = 0; kl < 16; ++kl)
            {
                put(m_s.tf_lnr, m_s.tf_lni, i2 * 16 + kl, i2 * kl, n);
            }
        }
    }

    // Points the state at the arrays and at the sub-plans' states once the plan is built.
    void bind_state() noexcept
    {
        m_s.p1 = m_p1 != nullptr ? &m_p1->m_s : nullptr;
        m_s.p2 = m_p2 != nullptr ? &m_p2->m_s : nullptr;
        m_s.tw_re = m_tw_re.data();
        m_s.tw_im = m_tw_im.data();
        m_s.rev = m_rev.data();
        m_s.re0 = m_re0.data();
        m_s.im0 = m_im0.data();
        m_s.re1 = m_re1.data();
        m_s.im1 = m_im1.data();
        m_s.ftw_hi_re = m_ftw_hi_re.data();
        m_s.ftw_hi_im = m_ftw_hi_im.data();
        m_s.ftw_lo_re = m_ftw_lo_re.data();
        m_s.ftw_lo_im = m_ftw_lo_im.data();
        m_s.ptw_re = m_ptw_re.data();
        m_s.ptw_im = m_ptw_im.data();
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
    FftPlanState<T> m_s;              // the sizes, the raw tables and the views the kernels read
    FftPlan<T>* m_p1 = nullptr;       // Lever D: hoisted length-n1 sub-FFT (ctor-built ONCE)
    FftPlan<T>* m_p2 = nullptr;       // Lever D: hoisted length-n2 sub-FFT
    crd::containers::Array<T> m_tw_re;
    crd::containers::Array<T> m_tw_im;
    crd::containers::Array<crd::usize> m_rev;
    crd::containers::Array<T> m_re0; // SoA ping-pong scratch (one plan per thread)
    crd::containers::Array<T> m_im0;
    crd::containers::Array<T> m_re1;
    crd::containers::Array<T> m_im1;
    // Lever D twiddle FACTORIZATION (the read-floor cut): the inter-stage twiddle W_n^{i2·k1} is NOT a stored
    // n-sized table (that was 128 MB of streaming DRAM read per transform @8M — a third of the four-step read
    // floor, pure overhead). Instead W_n^a = W_n^{a_hi·M}·W_n^{a_lo} from two ~√n L2-resident tables + one
    // complex multiply (FftPlanState::ftw_h).
    crd::containers::Array<T> m_ftw_hi_re; // W_n^{j·M}, j ∈ [0, n/M)
    crd::containers::Array<T> m_ftw_hi_im;
    crd::containers::Array<T> m_ftw_lo_re; // W_n^{j},   j ∈ [0, M)
    crd::containers::Array<T> m_ftw_lo_im;
    crd::containers::Array<T> m_ptw_re; // Lever A: per-pass combine twiddles, flat in (j,m) pass order
    crd::containers::Array<T> m_ptw_im;
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
