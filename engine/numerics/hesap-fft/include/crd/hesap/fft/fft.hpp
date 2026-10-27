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

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/detail/codelets.hpp>         // generated straight-line leaf codelets (genfft-lite, v10-b)
#include <crd/hesap/fft/detail/small_n_codelets.hpp> // AoS lane-trick small-N codelets (N≤32 f64; see header)
#include <crd/hesap/fft/detail/batched_codelets_gen.hpp>
#include <crd/hesap/fft/detail/hier_codelets.hpp> // hierarchical generated sub-FFTs (M2 4096 + M3 2048)
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp> // allocate_array_or_fatal / construct_or_fatal (DIAG.3a)

#include <crd/math/cmath.hpp>
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
// four-step phase counters (large-N path): P1 = column FFTs, P2 = row FFTs
inline unsigned long long g_p1_gather = 0, g_p1_sub = 0, g_p1_tw = 0;
inline unsigned long long g_p2_gather = 0, g_p2_sub = 0, g_p2_scatter = 0;
inline long g_fs_calls = 0;
// deep-split pass counters (S1 fused leaf / S2 notr leaf / S3 strided leaf)
inline unsigned long long g_ds1 = 0, g_ds2 = 0, g_ds3 = 0;
inline long g_ds_calls = 0;
// ip4-AoS phase counters (gather+len4 / the combine passes)
inline unsigned long long g_ip_gather = 0, g_ip_pass = 0;
inline long g_ip_calls = 0;
inline void dump_ip() noexcept
{
    const double c = static_cast<double>(g_ip_calls > 0 ? g_ip_calls : 1);
    std::fprintf(stderr, "[fft-prof ip4aos, %ld calls] gather %.1f Kcyc  passes %.1f Kcyc\n", g_ip_calls,
                 g_ip_gather / c / 1e3, g_ip_pass / c / 1e3);
    g_ip_gather = g_ip_pass = 0;
    g_ip_calls = 0;
}
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
                 static_cast<double>(g_first) / c, static_cast<double>(g_combine) / c, static_cast<double>(g_last) / c,
                 g_calls);
    g_first = g_combine = g_last = 0;
    g_calls = 0;
}
inline void dump_four_step() noexcept
{
    const double c = static_cast<double>(g_fs_calls > 0 ? g_fs_calls : 1);
    const double tot = static_cast<double>(g_p1_gather + g_p1_sub + g_p1_tw + g_p2_gather + g_p2_sub + g_p2_scatter);
    const double pct = tot > 0 ? 100.0 / tot : 0.0;
    std::fprintf(stderr, "[fft-prof four-step, %ld calls, Mcyc/call]\n", g_fs_calls);
    std::fprintf(stderr, "  P1 gather   %8.1f  (%4.1f%%)\n", g_p1_gather / c / 1e6, g_p1_gather * pct);
    std::fprintf(stderr, "  P1 sub-FFT  %8.1f  (%4.1f%%)\n", g_p1_sub / c / 1e6, g_p1_sub * pct);
    std::fprintf(stderr, "  P1 twid+NT  %8.1f  (%4.1f%%)\n", g_p1_tw / c / 1e6, g_p1_tw * pct);
    std::fprintf(stderr, "  P2 gather   %8.1f  (%4.1f%%)\n", g_p2_gather / c / 1e6, g_p2_gather * pct);
    std::fprintf(stderr, "  P2 sub-FFT  %8.1f  (%4.1f%%)\n", g_p2_sub / c / 1e6, g_p2_sub * pct);
    std::fprintf(stderr, "  P2 scatter  %8.1f  (%4.1f%%)\n", g_p2_scatter / c / 1e6, g_p2_scatter * pct);
    g_p1_gather = g_p1_sub = g_p1_tw = g_p2_gather = g_p2_sub = g_p2_scatter = 0;
    g_fs_calls = 0;
    if (g_ds_calls > 0)
    {
        const double dc = static_cast<double>(g_ds_calls);
        std::fprintf(stderr, "[fft-prof deep-split, %ld calls, Mcyc/call]\n", g_ds_calls);
        std::fprintf(stderr, "  S1 fused    %8.2f\n", g_ds1 / dc / 1e6);
        std::fprintf(stderr, "  S2 notr     %8.2f\n", g_ds2 / dc / 1e6);
        std::fprintf(stderr, "  S3 strided  %8.2f\n", g_ds3 / dc / 1e6);
        g_ds1 = g_ds2 = g_ds3 = 0;
        g_ds_calls = 0;
    }
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

namespace detail
{
// DIAG.3a: a plan cannot be built without its tables and sub-plans, so an allocator that refuses one (an exhausted
// sub-budget allocator) leaves the plan out of memory, fatal in every build, never a null table written
// (crd-hesap-fft-fatal-* modes).
inline constexpr const char* kFftPlanOutOfMemory = "FftPlan: out of memory for its tables";
} // namespace detail

template <typename T> class FftPlan
{
public:
    FftPlan(crd::memory::IAllocator* alloc, crd::usize n)
        : m_alloc(alloc), m_n(n), m_tw_re(alloc), m_tw_im(alloc), m_rev(alloc), m_re0(alloc), m_im0(alloc),
          m_re1(alloc), m_im1(alloc), m_ftw_hi_re(alloc), m_ftw_hi_im(alloc), m_ftw_lo_re(alloc), m_ftw_lo_im(alloc),
          m_ptw_re(alloc), m_ptw_im(alloc)
    {
        CRD_ASSERT(is_pow2(n)); // v10: power-of-2 (Bluestein/Rader make any size O(n log n) at v10-c)
        while ((crd::usize{1} << m_log2) < n)
        {
            ++m_log2;
        }
        // Above kFourStepMin the direct Stockham is DRAM-bound (log4(n) full passes); switch to the Bailey
        // four-step (n = n1·n2 ≈ √n × √n) so the sub-FFTs are cache-resident — O(1) DRAM passes, not O(log n).
        m_use_four_step = (n >= kFourStepMin);
        // f32 256K (2^18): enable the four-step (1024×256 split below) — the P2 256-axis then uses the default 16×16
        // hier codelet + gather/scatter fusion, fixing the small-N Stockham trough (0.33→~0.85× MKL, 3.6× CRD). f64
        // 256K stays direct (there is no f64 256-pt hier codelet). Larger sizes keep the square split.
        if (n == (crd::usize{1} << 18) && std::is_same_v<T, crd::f32>)
        {
            m_use_four_step = true;
        }
        // FFT-CRUSH 2026-07-03 session 6: f32 128K measured 0.17x MKL as direct Stockham (the sh band tops at
        // 64K) — opt into the four-step 1024x128 like f64: P1 = the gather-fused 1024 hier (bw==128), P2 = the
        // generated f32 codelet128_batched via the batched-leaf gate.
        if (n == (crd::usize{1} << 17) && std::is_same_v<T, crd::f32>)
        {
            m_use_four_step = true;
        }
        // FFT-CRUSH 2026-07-03: the f64 mid-band trough (64K-256K measured 0.40-0.49x MKL as DIRECT Stockham —
        // the June "parity regime" rows were unbenched). Opt the four-step in for f64 too: P1 = batched 1024-hier
        // (exists, f64 default-on), P2 = batched small-N (codelet64_batched exists f64; 128/256 run batched
        // Stockham, still cache-resident). Same structure that took f32 256K 0.33 -> ~0.85x.
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            // 64K now rides the standalone-hier 2-pass (256×256) — better than the four-step there.
            if (n == (crd::usize{1} << 17) || n == (crd::usize{1} << 18))
            {
                m_use_four_step = true;
            }
        }
        // n1 ≈ √n (n2 = n/n1, both powers of 2) — the square split, confirmed near-optimal by a full-FFT n1/n2
        // plan search 2026-06-15 (±1 shift is marginal and size-dependent: 8M ~+1.5%, 4M worse).
        // n1 = 2^ceil(log2/2): the LARGER factor first. Square split for even log2 (unchanged); for ODD log2 the
        // larger factor as n1 measures faster — 8M (2048×4096→4096×2048) +9%, 2M +13%, f32 8M +10%, 4M square
        // unchanged, accuracy preserved (~1e-15). Found by the black-box MKL-archaeology factorization sweep
        // (2026-06-16): the old floor(log2/2) put the SMALLER factor first for odd log2, underusing pass-1.
        m_n1 = crd::usize{1} << ((m_log2 + 1) / 2);
        if (m_use_four_step && n == (crd::usize{1} << 18)) // 256K = 1024×256 (n2=256 → 16×16 hier P2)
        {
            m_n1 = 1024;
        }
        if constexpr (std::is_same_v<T, crd::f64>) // FFT-CRUSH: ALL-HIER splits for the f64 mid-band
        {
            if (m_use_four_step && n == (crd::usize{1} << 17)) // 128K = 1024×128 (P2 = codelet128)
            {
                m_n1 = 1024;
            }
            if (m_use_four_step && n == (crd::usize{1} << 18)) // 256K = 1024×256 (P2 radix-8 until codelet256)
            {
                m_n1 = 1024;
            }
        }
        if constexpr (std::is_same_v<T, crd::f32>) // FFT-CRUSH: f32 512K trough (measured 0.55x) — all-hier split
        {
            if (m_use_four_step && n == (crd::usize{1} << 19)) // 512K = 2048×256: P2 = 16×16 hier
            {
                m_n1 = 2048;
            }
            if (m_use_four_step && n == (crd::usize{1} << 17)) // 128K = 1024×128 (P2 = codelet128)
            {
                m_n1 = 1024;
            }
        }
        // Full table W_n^k, k = 0 .. n-1 (radix-4 indexes up to 3·j·r < n). Precomputed ONCE, shared — the
        // determinism contract. Computed in f64 then narrowed (accuracy for the f32 plan).
        m_tw_re.resize(n);
        m_tw_im.resize(n);
        constexpr double two_pi = 6.283185307179586476925286766559;
        for (crd::usize k = 0; k < n; ++k)
        {
            const double ang = -two_pi * static_cast<double>(k) / static_cast<double>(n);
            m_tw_re[k] = static_cast<T>(crd::math::cos(ang)); //  cos(2πk/n)
            m_tw_im[k] = static_cast<T>(crd::math::sin(ang)); // -sin(2πk/n) ⇒ (re,im) == W_n^k
        }
        m_rev.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_rev[i] = bitrev(i, m_log2);
        }
        // The hierarchical generated-codelet sub-FFTs. 4096 = 64×64,
        // 2048 = 64×32, 1024 = 32×32; N1 = stage-1 leaf (twiddle stride), N2 = inner stage-2 leaf. Build the
        // transposed intermediate (n·BB complex) + the inner twiddle W_N^{n2·k1} (index n2·N1+k1). Dispatched
        // Forward, b==block_width(n), f64 and f32 (M9-fixed: the dispatch gate matches the ctor; f32 had been
        // silently radix-8). The h_n1/h_n2 + block_width
        // reproduce the f64 BBs byte-identically (4096→16, 2048→32, 1024→64), so the f64 path is unchanged.
        constexpr bool k_hier = std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>;
        if constexpr (k_hier)
        {
            crd::usize h_n2 = 0U; // N2 = inner count
            if (n == 4096)
            {
                h_n2 = 64U;
            }
            else if (n == 2048)
            {
                h_n2 = 32U;
            }
            crd::usize h_n1 = 64U;                                          // N1 = stage-1 leaf = twiddle stride
            crd::usize h_bb = block_width(n);                               // BB (T-aware: f32 = 2× f64)
            if (n == 1024) // 1024 = 32×32 — 1M/2M sub-FFT
            {
                h_n2 = 32U;
                h_n1 = 32U;
            }
            // 256 = 16×16 hier sub-FFT for the 256K four-step P2 (f32 only — the codelet16 stage-1 is Vec8f).
            if (n == 256 && std::is_same_v<T, crd::f32>)
            {
                h_n2 = 16U;
                h_n1 = 16U;
            }

            if (h_n2 != 0)
            {
                m_hier_bbuf = plan_table<Complex<T>>(n * h_bb, 64);
                m_hier_twr = plan_table<T>(h_n2 * h_n1, 32);
                m_hier_twi = plan_table<T>(h_n2 * h_n1, 32);
                constexpr double tp = 6.283185307179586476925286766559;
                for (crd::usize n2 = 0; n2 < h_n2; ++n2)
                {
                    for (crd::usize k1 = 0; k1 < h_n1; ++k1)
                    {
                        const double th = tp * static_cast<double>(n2 * k1) / static_cast<double>(n);
                        m_hier_twr[n2 * h_n1 + k1] = static_cast<T>(crd::math::cos(th));
                        m_hier_twi[n2 * h_n1 + k1] = static_cast<T>(-crd::math::sin(th));
                    }
                }
            }
        }
        // FFT-CRUSH 2026-07-03 (standalone-HIER, forward): a single n=n1·n2 transform IS an
        // element-major batch matrix — stage-1 = codelet_n1_stage1_fused_sh (leaf + twiddle +
        // transposed store, ONE pass), stage-2 = codelet_n2_batched → natural order. Covers
        // 1024..65536 for f64 (Vec4d) AND f32 (Vec8f, same splits). Buffers: s,t (n complex each)
        // + the full W_n^{k1·i2} table in the stage-1-output layout.
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            bool sh = !m_use_four_step && n >= 1024 && n <= 65536; // 2-pass hier band (65536 = 256×256)
            // deep-split band (session 6): n = A·B·C, THREE generated passes — S1 = codeletA_stage1_fused_sh
            // (leaf + full W_n twiddle + transposed store), S2 = codeletB_fused_notr (leaf + broadcast
            // W_{BC} + natural store), S3 = codeletC_batched_strided per kB (L1-resident reads, natural-order
            // writes d[kA + A·kB + AB·kC]). Forward only; inverse keeps the four-step (both stay allocated).
            // 1M re-measured 2026-07-04 WITH the factored twiddle (the 16 MB table stream removed):
            // STILL loses (f64 9.7 vs 6.3 ms, f32 3.23 vs ~3.1) ⇒ the 3×full-size DRAM round-trips
            // themselves are the wall at ≥1M — the four-step's L2-blocked structure stays.
            // Band extended DOWN to 32K/64K 2026-07-04: their sh 2-pass rode the spill-heavy 256
            // leaves (f64 0.48-0.59x, the worst rows); the 3-pass all-small-leaf form replaces it.
            // 4096 = 16·16·16 MEASURED WORSE both types (f64 30.5 vs 34.4 GF/s, f32 0.72x vs 0.82x,
            // 2026-07-04): at L1-resident sizes the 3rd pass costs more than tiny leaves save — the
            // 2-pass sh stays below 8K.
            // ≥2M re-measured under the 4-stage (2026-07-04): 2M flat, 4M/8M BIG losses (f64 4M 41.8
            // vs 29.9, 8M 97 vs 63) — past L3 the full-array multi-pass form loses to the four-step's
            // blocked structure (the thrice-confirmed law: five-step, six-step@4M, 4-stage@4M).
            bool ds = n >= (crd::usize{1} << 13) && n <= (crd::usize{1} << 20);
            if constexpr (std::is_same_v<T, crd::f64>)
            {
                if (n == (crd::usize{1} << 20))
                {
                    ds = false; // f64 1M: the six-step wins (5.38 vs 5.88 ms) — per-type winner
                }
            }
            // (f32 512K re-measured 2026-07-04 WITH the factored-twiddle S1 + AoSoA + FMA pipeline —
            // the 2026-07-03 −5% exclusion no longer holds: 0.953 ms / 0.84x, +21%. See the bench doc.)
            if constexpr (std::is_same_v<T, crd::f32>)
            {
                // f32 8192 ds (32·16·16) MEASURED −10% (0.65x -> 0.58x): the 16-point leaves are only
                // 2 Vec8f rows — per-call overhead beats the pass savings at 8 lanes. sh keeps the row.
                if (n == (crd::usize{1} << 13))
                {
                    ds = false;
                }
            }
            if (sh || ds)
            {
                if (ds)
                {
                    // A·B·C splits: 4K = 16·16·16 · 8K = 32·16·16 · 16K = 32·32·16 · 32K = 32·32·32 ·
                    // 64K = 32·32·64 · 128K = 32·64·64 · 256K = 64·64·64 · 512K = 64·64·128 (B=128
                    // measured WORSE at 512K: S2's 128-leaf runs 128+128 r/w streams ⇒ 6.96 vs ~4.4).
                    m_sh_n1 = (n <= (crd::usize{1} << 17)) ? 32U : 64U; // A (the S1 leaf = stream count)
                    m_ds_b = 64U;
                    if (n <= (crd::usize{1} << 13))
                    {
                        m_ds_b = 16U;
                    }
                    else if (n <= (crd::usize{1} << 16))
                    {
                        m_ds_b = 32U;
                    }
                    m_ds_c = 64U;
                    if (n <= (crd::usize{1} << 14))
                    {
                        m_ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 15))
                    {
                        m_ds_c = 32U;
                    }
                    else if (n == (crd::usize{1} << 19))
                    {
                        m_ds_c = 128U;
                    }
                    // 4-STAGE trials (the K-stage sweep, 2026-07-04): n = A·B1·B2·C with tiny factors
                    // — the MKL pass shape (small live sets per stage). Measured per row vs the
                    // banked 3-stage; losers revert here.
                    if (n == (crd::usize{1} << 13))
                    {
                        m_sh_n1 = 16U; // 8K = 16·8·8·8
                        m_ds_b = 8U;
                        m_ds_b2 = 8U;
                        m_ds_c = 8U;
                    }
                    else if (n == (crd::usize{1} << 14))
                    {
                        m_sh_n1 = 16U; // 16K = 16·16·8·8
                        m_ds_b = 16U;
                        m_ds_b2 = 8U;
                        m_ds_c = 8U;
                    }
                    else if (n == (crd::usize{1} << 15))
                    {
                        m_sh_n1 = 16U; // 32K = 16·16·16·8
                        m_ds_b = 16U;
                        m_ds_b2 = 16U;
                        m_ds_c = 8U;
                    }
                    else if (n == (crd::usize{1} << 16))
                    {
                        m_sh_n1 = 16U; // 64K = 16·16·16·16
                        m_ds_b = 16U;
                        m_ds_b2 = 16U;
                        m_ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 17))
                    {
                        m_sh_n1 = 32U; // 128K = 32·16·16·16
                        m_ds_b = 16U;
                        m_ds_b2 = 16U;
                        m_ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 18))
                    {
                        m_sh_n1 = 32U; // 256K = 32·32·16·16
                        m_ds_b = 32U;
                        m_ds_b2 = 16U;
                        m_ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 19))
                    {
                        m_sh_n1 = 32U; // 512K = 32·32·32·16
                        m_ds_b = 32U;
                        m_ds_b2 = 32U;
                        m_ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 20))
                    {
                        m_sh_n1 = 32U; // 1M = 32·32·32·32: f32 WINNER (2.24 ms, 0.91×); f64 measured
                        m_ds_b = 32U;  // 5.88 vs the six-step's 5.38 — f64 1M keeps fs6 (gate below).
                        m_ds_b2 = 32U;
                        m_ds_c = 32U;
                    }
                    else if (n == (crd::usize{1} << 21))
                    {
                        m_sh_n1 = 32U; // 2M = 32·32·32·64 (vs four-step 0.83-0.90×)
                        m_ds_b = 32U;
                        m_ds_b2 = 32U;
                        m_ds_c = 64U;
                    }
                    else if (n == (crd::usize{1} << 22))
                    {
                        m_sh_n1 = 32U; // 4M = 32·32·64·64
                        m_ds_b = 32U;
                        m_ds_b2 = 64U;
                        m_ds_c = 64U;
                    }
                    else if (n == (crd::usize{1} << 23))
                    {
                        m_sh_n1 = 64U; // 8M = 64·32·64·64
                        m_ds_b = 32U;
                        m_ds_b2 = 64U;
                        m_ds_c = 64U;
                    }
                    while ((crd::usize{1} << m_ds_ash) < m_sh_n1)
                    {
                        ++m_ds_ash;
                    }
                    m_sh_s = plan_table<Complex<T>>(n, 64);
                    constexpr double tpd = 6.283185307179586476925286766559;
                    // last-notr-stage table: W_{B·C} (3-stage) or W_{B2·C} (4-stage), [k*C + v]
                    const crd::usize lb = (m_ds_b2 != 0) ? m_ds_b2 : m_ds_b;
                    const crd::usize bc = lb * m_ds_c;
                    m_ds_twr = plan_table<T>(bc, 64);
                    m_ds_twi = plan_table<T>(bc, 64);
                    for (crd::usize kb = 0; kb < lb; ++kb)
                    {
                        for (crd::usize v = 0; v < m_ds_c; ++v)
                        {
                            const double th = tpd * static_cast<double>((kb * v) % bc) / static_cast<double>(bc);
                            m_ds_twr[kb * m_ds_c + v] = static_cast<T>(crd::math::cos(th));
                            m_ds_twi[kb * m_ds_c + v] = static_cast<T>(-crd::math::sin(th));
                        }
                    }
                    if (m_ds_b2 != 0) // S2 table for the 4-stage: W_{B1·B2·C}, [k*(B2·C) + v]
                    {
                        const crd::usize b2c = m_ds_b2 * m_ds_c;
                        const crd::usize e = m_ds_b * b2c;
                        m_ds_tw2r = plan_table<T>(e, 64);
                        m_ds_tw2i = plan_table<T>(e, 64);
                        for (crd::usize kb = 0; kb < m_ds_b; ++kb)
                        {
                            for (crd::usize v = 0; v < b2c; ++v)
                            {
                                const double th = tpd * static_cast<double>((kb * v) % e) / static_cast<double>(e);
                                m_ds_tw2r[kb * b2c + v] = static_cast<T>(crd::math::cos(th));
                                m_ds_tw2i[kb * b2c + v] = static_cast<T>(-crd::math::sin(th));
                            }
                        }
                    }
                }
                else
                {
                    m_sh_n1 = 256U; // 32768 = 256×128, 65536 = 256×256
                    if (n == 1024)
                    {
                        m_sh_n1 = 32U;
                    }
                    else if (n == 2048 || n == 4096 || n == 8192)
                    {
                        m_sh_n1 = 64U;
                    }
                    else if (n == 16384)
                    {
                        m_sh_n1 = 128U;
                    }
                }
                const crd::usize sn2 = n / m_sh_n1;
                m_sh_t = plan_table<Complex<T>>(n, 64);
                constexpr double tp2 = 6.283185307179586476925286766559;
#if CRD_SIMD_HAS_AVX2
                // IP4-AoS plan tables (PROMOTED 2026-07-04): digit-reversal + per-pass twiddles
                // for the interleaved in-place engine (f64+f32 1K..64K, both directions).
                if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
                {
                    // f64: ip4-AoS wins every row 1K..64K. f32: matched-state A/B (2026-07-04)
                    // shows wins at 2K/16K/32K/64K but losses to the Vec8f sh path at 1K/4K/8K —
                    // per-size dispatch keeps each row on its measured winner (still a pure
                    // function of size ⇒ deterministic plans).
                    const bool ip_take = std::is_same_v<T, crd::f64>
                                             ? (n >= 1024 && n <= 65536)
                                             : (n == 2048 || (n >= 16384 && n <= 65536));
                    if (ip_take)
                    {
                        // Round 13: BOTH parities. Odd log2 (n = 2·4^k) = two half-length ip4
                        // transforms on the even/odd decimations (the rev table absorbs the ×2+par
                        // io mapping) + one final radix-2 combine pass with a W_n^j table.
                        m_ip_n = n;
                        const bool ipodd = (m_log2 & 1U) != 0U;
                        const crd::usize nh = ipodd ? (n >> 1) : n; // 4^k length per half
                        const crd::u32 dg = (ipodd ? m_log2 - 1U : m_log2) / 2U;
                        // Radix plan (round 17), innermost→outermost: [4,4 (the gather fold)],
                        // then TWO radix-8 passes for nh ≥ 4096, then radix-4 to nh (the LAST pass
                        // must be radix-4 — the COBRA quad identity needs a base-4 top slot digit).
                        crd::u32 rads[16];
                        crd::u32 nr = 0;
                        rads[nr++] = 4U;
                        rads[nr++] = 4U;
                        const crd::u32 log2h = ipodd ? m_log2 - 1U : m_log2;
                        // Radix-8 MEASURED SLOWER than pure radix-4 on Raptor Cove (round 17:
                        // −3% v1, −1% block-paired — the W8 shuffle diagonal + 7-set twiddle
                        // traffic outweigh the saved pass). Machinery kept, tested, oracle-green;
                        // flip to 2U to re-enable for future uarches.
                        const crd::u32 nr8 = 0U;
                        (void)nh;
                        for (crd::u32 i = 0; i < nr8; ++i)
                        {
                            rads[nr++] = 8U;
                        }
                        for (crd::u32 bits = log2h - 4U - 3U * nr8; bits != 0U; bits -= 2U)
                        {
                            rads[nr++] = 4U;
                        }
                        (void)dg;
                        m_ip_rev = plan_table<crd::usize>(n, 64);
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
                            // INVERSE map (mixed-radix reversal is NOT an involution): the gather
                            // wants io-index per SLOT ⇒ rev[slot(j)] = j (×2+parity for odd).
                            if (ipodd)
                            {
                                m_ip_rev[s] = 2 * j;          // half 0 = even decimation
                                m_ip_rev[s + nh] = 2 * j + 1; // half 1 = odd decimation
                            }
                            else
                            {
                                m_ip_rev[s] = j;
                            }
                        }
                        // HYBRID tables (round 15): full 3-set (w1,w2,w3) runs for len ≤ 1024 —
                        // tiny (≤8KB), no in-register twiddle powers needed — and w1-only for the
                        // BIG passes (their tables were the DTLB bulk; w2/w3 computed in-register
                        // there). Non-duplicated storage throughout; dup expansion at load.
                        m_ip_twr = plan_table<T>(n, 64);
                        m_ip_twi = plan_table<T>(n, 64);
                        crd::usize off = 0;
                        {
                            // len-16 fold table (3-set) + per-stage tables in PLAN order:
                            // radix-8 passes get FULL 7-set tables (their lens are small — tiny);
                            // radix-4 passes: 3-set for len ≤ 1024, w1-only above (hybrid rule).
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
                                        const double th = tp2 * static_cast<double>((mset * k) % len) /
                                                          static_cast<double>(len);
                                        m_ip_twr[off] = static_cast<T>(crd::math::cos(th));
                                        m_ip_twi[off] = static_cast<T>(-crd::math::sin(th));
                                        ++off;
                                    }
                                }
                            }
                        }
                        if (ipodd)
                        {
                            for (crd::usize j = 0; j < nh; ++j)
                            {
                                const double th = tp2 * static_cast<double>(j) / static_cast<double>(n);
                                m_ip_twr[off] = static_cast<T>(crd::math::cos(th));
                                m_ip_twi[off] = static_cast<T>(-crd::math::sin(th));
                                ++off;
                            }
                        }
                    }
                }
#endif
                // FACTORED twiddle (2026-07-04 crush): for the deep-split band (n2 >= 1024) the full
                // n-entry stage-1 table streams as many bytes as the data every call — factor
                // W_n^{k·u} = hi[k,u>>msh] · lo[k,u&(M-1)] into L1-resident tables (EXACT index split,
                // 2 extra FMAs per output). MEASURED: wins ONLY where the table leaves L2 (f64 128K +4%
                // / 256K +3% / f32 128K +14%); at n2 <= 256 (the sh band, tables <= 1MB) the extra ops
                // LOSE 3-6% — the gate stays at the ds sizes.
                if (sn2 >= 1024)
                {
                    crd::u32 n2log = 0;
                    while ((crd::usize{1} << n2log) < sn2)
                    {
                        ++n2log;
                    }
                    const crd::u32 lmin = std::is_same_v<T, crd::f32> ? 3U : 2U; // M >= lane width
                    m_sh_msh = (n2log + 1U) / 2U;
                    if (m_sh_msh < lmin)
                    {
                        m_sh_msh = lmin;
                    }
                    const crd::usize mm = crd::usize{1} << m_sh_msh;
                    const crd::usize uhc = sn2 >> m_sh_msh;
                    m_sh_twr = plan_table<T>(m_sh_n1 * mm, 64); // LO re
                    m_sh_twi = plan_table<T>(m_sh_n1 * mm, 64); // LO im
                    m_sh_hir = plan_table<T>(m_sh_n1 * uhc, 64);
                    m_sh_hii = plan_table<T>(m_sh_n1 * uhc, 64);
                    for (crd::usize k1 = 0; k1 < m_sh_n1; ++k1)
                    {
                        for (crd::usize ul = 0; ul < mm; ++ul)
                        {
                            const double th = tp2 * static_cast<double>((k1 * ul) % n) / static_cast<double>(n);
                            m_sh_twr[k1 * mm + ul] = static_cast<T>(crd::math::cos(th));
                            m_sh_twi[k1 * mm + ul] = static_cast<T>(-crd::math::sin(th));
                        }
                        for (crd::usize uh = 0; uh < uhc; ++uh)
                        {
                            const double th =
                                tp2 * static_cast<double>((k1 * (uh << m_sh_msh)) % n) / static_cast<double>(n);
                            m_sh_hir[k1 * uhc + uh] = static_cast<T>(crd::math::cos(th));
                            m_sh_hii[k1 * uhc + uh] = static_cast<T>(-crd::math::sin(th));
                        }
                    }
                }
                else
                {
                    m_sh_twr = plan_table<T>(n, 64);
                    m_sh_twi = plan_table<T>(n, 64);
                    for (crd::usize k1 = 0; k1 < m_sh_n1; ++k1)
                    {
                        for (crd::usize i2 = 0; i2 < sn2; ++i2)
                        {
                            const double th = tp2 * static_cast<double>((k1 * i2) % n) / static_cast<double>(n);
                            // stored in the STAGE-1-OUTPUT layout s[k1*n2 + i2] (applied before the transpose)
                            m_sh_twr[k1 * sn2 + i2] = static_cast<T>(crd::math::cos(th));
                            m_sh_twi[k1 * sn2 + i2] = static_cast<T>(-crd::math::sin(th));
                        }
                    }
                }
            }
        }
        if (m_use_four_step) // Lever D resurrection: four-step buffers + HOISTED sub-plans + linear twiddle
        {
            // tbuf (transpose intermediate) + scratch (cache-resident block) are RAW 64-BYTE-ALIGNED (NT stores
            // need ≥32B; crd Array gives only 8B). Freed in the dtor.
            m_tbuf = plan_table<Complex<T>>(n, 64);
            m_scratch = plan_table<Complex<T>>(n, 64);
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
            m_p1 = sub_plan(m_n1);
            m_p2 = sub_plan(n2);
        }
        else // direct radix path: SoA ping-pong scratch + Lever A per-pass combine twiddles
        {
            m_re0.resize(n);
            m_im0.resize(n);
            m_re1.resize(n);
            m_im1.resize(n);
            m_rmax_bits = 3U;
            if (m_log2 >= 15U && m_log2 <= 20U)
            {
                m_rmax_bits = 5U;
            }
            else if (m_log2 >= 12U)
            {
                m_rmax_bits = 4U;
            }
            build_combine_twiddles();
        }
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
        if (m_scratch != nullptr)
        {
            m_alloc->deallocate(m_scratch);
        }
        if (m_tbuf != nullptr)
        {
            m_alloc->deallocate(m_tbuf);
        }
        if (m_sh_t != nullptr)
        {
            if (m_sh_s != nullptr)
            {
                m_alloc->deallocate(m_sh_s);
            }
            m_alloc->deallocate(m_sh_t);
            m_alloc->deallocate(m_sh_twr);
            m_alloc->deallocate(m_sh_twi);
            if (m_sh_hir != nullptr)
            {
                m_alloc->deallocate(m_sh_hir);
                m_alloc->deallocate(m_sh_hii);
            }
        }
        if (m_ds_twr != nullptr)
        {
            m_alloc->deallocate(m_ds_twr);
            m_alloc->deallocate(m_ds_twi);
        }
        if (m_ds_tw2r != nullptr)
        {
            m_alloc->deallocate(m_ds_tw2r);
            m_alloc->deallocate(m_ds_tw2i);
        }
        if (m_ip_rev != nullptr)
        {
            m_alloc->deallocate(m_ip_rev);
            m_alloc->deallocate(m_ip_twr);
            m_alloc->deallocate(m_ip_twi);
        }
        if (m_hier_bbuf != nullptr)
        {
            m_alloc->deallocate(m_hier_bbuf);
        }
        if (m_hier_twr != nullptr)
        {
            m_alloc->deallocate(m_hier_twr);
        }
        if (m_hier_twi != nullptr)
        {
            m_alloc->deallocate(m_hier_twi);
        }
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
#if CRD_SIMD_HAS_AVX2
            // f64 N∈{8,16,32}: the AoS lane-trick codelet operates in place on the interleaved data (exactly its
            // [re,im,…] layout), avoiding the SoA deinterleave/reinterleave round-trip — faster than the SoA leaf
            // it replaces at these sizes. The SoA path below stays the fallback for n=2/4, f32, and non-AVX2
            // backends. → small_n_codelets.hpp.
            if constexpr (std::is_same_v<T, crd::f64>)
            {
                if (m_n == 8 || m_n == 16 || m_n == 32)
                {
                    detail::small_n_fft_f64(reinterpret_cast<crd::f64*>(data.data()), m_n,
                                            dir == FftDirection::Forward);
                    return;
                }
            }
#endif
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
#if CRD_SIMD_HAS_AVX2
        // IP4-AoS (PROMOTED 2026-07-04, the VTune-guided strided-v3 engine): interleaved in-place
        // radix-4 with COBRA gather + 3-layer fold; f64 1K..64K, forward AND inverse. Beats the
        // prior sh/ds paths on every row (see docs/research/fft-stockham-v2.md rounds 7-17).
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            if (m_ip_n != 0)
            {
                if (dir == FftDirection::Forward)
                {
                    execute_ip4aos<false>(data);
                }
                else
                {
                    execute_ip4aos<true>(data);
                }
                return;
            }
        }
#endif
#if CRD_SIMD_HAS_AVX2
        // FFT-CRUSH 2026-07-03 DEEP-SPLIT (forward, 128K/256K): n = A·B·C, three generated passes —
        // S1 leaf+W_n+transpose → S2 leaf+broadcast-W_{BC} → S3 strided leaf, natural order out
        // (d[kA + A·kB + AB·kC]; derivation at the ctor). Inverse falls through to the four-step.
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            if (dir == FftDirection::Forward && m_ds_b != 0)
            {
                Complex<T>* const d = data.data();
                const crd::usize a = m_sh_n1;
                const crd::usize ca = m_ds_c * a;
                // 4-stage: rest = B2·C (the elements remaining after the S2 leaf); 3-stage: rest = C.
                const crd::usize rest = (m_ds_b2 != 0 ? m_ds_b2 : 1U) * m_ds_c;
                const crd::usize s2b = rest * a; // S2 batch width
#ifdef CRD_FFT_PROFILE
                const unsigned long long d0 = prof::rdtsc();
#endif
                // AoSoA (2026-07-04 crush): S1 deinterleaves ONCE into [L×re|L×im] block rows over
                // m_sh_t; S2 runs pure block form (zero shuffles, one stream per row); S3
                // reinterleaves once on the strided final store.
                T* const tb = reinterpret_cast<T*>(m_sh_t); // n complex = exactly 2n T of AoSoA blocks
                T* const sb = reinterpret_cast<T*>(m_sh_s);
                if (m_sh_msh != 0U) // S1: A-leaf + FACTORED W_n twiddle + transpose (n2 >= 1024)
                {
                    switch (a)
                    {
                        case 16U:
                            gen::codelet16_stage1_fused_sh_csf(d, tb, m_ds_b * rest, m_sh_twr, m_sh_twi,
                                                               m_sh_hir, m_sh_hii, m_sh_msh);
                            break;
                        case 32U:
                            gen::codelet32_stage1_fused_sh_csf(d, tb, m_ds_b * rest, m_sh_twr, m_sh_twi,
                                                               m_sh_hir, m_sh_hii, m_sh_msh);
                            break;
                        default:
                            gen::codelet64_stage1_fused_sh_csf(d, tb, m_ds_b * rest, m_sh_twr, m_sh_twi,
                                                               m_sh_hir, m_sh_hii, m_sh_msh);
                            break;
                    }
                }
                else // S1 with the full [k*n2+u] table (the small-n2 ds sizes: 4K/8K/16K)
                {
                    switch (a)
                    {
                        case 16U:
                            gen::codelet16_stage1_fused_sh_cs(d, tb, m_ds_b * rest, m_sh_twr, m_sh_twi);
                            break;
                        case 32U:
                            gen::codelet32_stage1_fused_sh_cs(d, tb, m_ds_b * rest, m_sh_twr, m_sh_twi);
                            break;
                        default:
                            gen::codelet64_stage1_fused_sh_cs(d, tb, m_ds_b * rest, m_sh_twr, m_sh_twi);
                            break;
                    }
                }
#ifdef CRD_FFT_PROFILE
                const unsigned long long d1 = prof::rdtsc();
#endif
                {
                    // S2: B-leaf over the (rest·A)-wide batch + broadcast twiddle (W_{BC} 3-stage /
                    // W_{B1B2C} 4-stage)
                    const T* const t2r = (m_ds_b2 != 0) ? m_ds_tw2r : m_ds_twr;
                    const T* const t2i = (m_ds_b2 != 0) ? m_ds_tw2i : m_ds_twi;
                    switch (m_ds_b)
                    {
                        case 8U:
                            gen::codelet8_fused_notr_ss(tb, sb, s2b, m_ds_ash, t2r, t2i);
                            break;
                        case 16U:
                            gen::codelet16_fused_notr_ss(tb, sb, s2b, m_ds_ash, t2r, t2i);
                            break;
                        case 32U:
                            gen::codelet32_fused_notr_ss(tb, sb, s2b, m_ds_ash, t2r, t2i);
                            break;
                        case 64U:
                            gen::codelet64_fused_notr_ss(tb, sb, s2b, m_ds_ash, t2r, t2i);
                            break;
                        default:
                            gen::codelet128_fused_notr_ss(tb, sb, s2b, m_ds_ash, t2r, t2i);
                            break;
                    }
                }
#ifdef CRD_FFT_PROFILE
                const unsigned long long d2 = prof::rdtsc();
#endif
                if (m_ds_b2 != 0) // 4-STAGE: S3 = notr B2 per k1 block (sb -> tb), S4 = strided C
                {
                    const crd::usize blk2 = 2 * m_ds_b2 * ca; // T units per k1 block
                    for (crd::usize k1 = 0; k1 < m_ds_b; ++k1)
                    {
                        switch (m_ds_b2)
                        {
                            case 8U:
                                gen::codelet8_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, m_ds_ash,
                                                            m_ds_twr, m_ds_twi);
                                break;
                            case 16U:
                                gen::codelet16_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, m_ds_ash,
                                                             m_ds_twr, m_ds_twi);
                                break;
                            case 32U:
                                gen::codelet32_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, m_ds_ash,
                                                             m_ds_twr, m_ds_twi);
                                break;
                            default:
                                gen::codelet64_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, m_ds_ash,
                                                             m_ds_twr, m_ds_twi);
                                break;
                        }
                    }
                    const crd::usize os4 = a * m_ds_b * m_ds_b2;
                    for (crd::usize k1 = 0; k1 < m_ds_b; ++k1) // S4: d[kA + A·k1 + AB1·k2 + AB1B2·kC]
                    {
                        for (crd::usize k2 = 0; k2 < m_ds_b2; ++k2)
                        {
                            const T* const src = tb + k1 * blk2 + k2 * 2 * ca;
                            Complex<T>* const dst = d + (k1 + m_ds_b * k2) * a;
                            switch (m_ds_c)
                            {
                                case 8U:
                                    gen::codelet8_batched_strided_sc(src, dst, a, os4);
                                    break;
                                case 16U:
                                    gen::codelet16_batched_strided_sc(src, dst, a, os4);
                                    break;
                                case 32U:
                                    gen::codelet32_batched_strided_sc(src, dst, a, os4);
                                    break;
                                default:
                                    gen::codelet64_batched_strided_sc(src, dst, a, os4);
                                    break;
                            }
                        }
                    }
                }
                else
                {
                    for (crd::usize kb = 0; kb < m_ds_b; ++kb) // S3: C-leaf per kB block, natural order
                    {
                        switch (m_ds_c)
                        {
                            case 16U:
                                gen::codelet16_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * m_ds_b);
                                break;
                            case 32U:
                                gen::codelet32_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * m_ds_b);
                                break;
                            case 64U:
                                gen::codelet64_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * m_ds_b);
                                break;
                            default:
                                gen::codelet128_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * m_ds_b);
                                break;
                        }
                    }
                }
#ifdef CRD_FFT_PROFILE
                prof::g_ds1 += d1 - d0;
                prof::g_ds2 += d2 - d1;
                prof::g_ds3 += prof::rdtsc() - d2;
                ++prof::g_ds_calls;
#endif
                return;
            }
        }
#endif
        // SIX-STEP v2 forward (square 1M/4M): SIMD micro-tile in-place transposes (v1's
        // element-scalar swaps were the measured wall: f64 1M 8.8 vs 6.3 ms) + contiguous
        // L1-resident row FFTs. See docs/research/fft-stockham-v2.md.
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            // 1M ONLY (L3-resident: the all-in-place chain wins there, +17%/+22%). 4M measured worse
            // under BOTH chains (in-place 33.5 / oop-rechain 40.4 vs four-step 29.9) — beyond L3 the
            // four-step's NT-scatter structure stays; 4M/8M already run 0.84-0.99× there.
            if (dir == FftDirection::Forward && m_use_four_step && m_n1 * m_n1 == m_n &&
                m_n == (crd::usize{1} << 20))
            {
                execute_six_step(data);
                return;
            }
        }
        if (m_use_four_step)
        {
            execute_four_step(data, dir);
            return;
        }
#if CRD_SIMD_HAS_AVX2
        // FFT-CRUSH 2026-07-03 standalone-HIER (forward, 1024..65536, f64 Vec4d + f32 Vec8f): one
        // stage-1-fused pass (leaf + twiddle + transposed store) + one batched-leaf pass — TWO passes
        // total, replacing ~log4(n) Stockham passes. Natural-order output by the four-step identity.
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            if (dir == FftDirection::Forward && m_sh_t != nullptr && m_ds_b == 0)
            {
                const crd::usize n1 = m_sh_n1;
                const crd::usize n2 = m_n / n1;
                Complex<T>* din2 = data.data();
                // AoSoA (2026-07-04 crush): stage-1 deinterleaves ONCE into [L×re|L×im] block rows
                // over m_sh_t (transposed block stores, no interleave shuffles, one stream per row);
                // stage-2 reads blocks shuffle-free and reinterleaves once on the final store. FMA
                // butterflies throughout.
                T* const tb = reinterpret_cast<T*>(m_sh_t); // n complex = exactly 2n T of AoSoA blocks
                if (m_sh_msh != 0U) // stage-1 FUSED, FACTORED L1-resident twiddle (n2 >= 128)
                {
                    switch (n1)
                    {
                        case 32U:
                            gen::codelet32_stage1_fused_sh_csf(din2, tb, n2, m_sh_twr, m_sh_twi, m_sh_hir,
                                                               m_sh_hii, m_sh_msh);
                            break;
                        case 64U:
                            gen::codelet64_stage1_fused_sh_csf(din2, tb, n2, m_sh_twr, m_sh_twi, m_sh_hir,
                                                               m_sh_hii, m_sh_msh);
                            break;
                        case 128U:
                            gen::codelet128_stage1_fused_sh_csf(din2, tb, n2, m_sh_twr, m_sh_twi, m_sh_hir,
                                                                m_sh_hii, m_sh_msh);
                            break;
                        default:
                            gen::codelet256_stage1_fused_sh_csf(din2, tb, n2, m_sh_twr, m_sh_twi, m_sh_hir,
                                                                m_sh_hii, m_sh_msh);
                            break;
                    }
                }
                else // stage-1 FUSED (leaf + full-table twiddle + transposed block store): ONE pass
                {
                    switch (n1)
                    {
                        case 32U:
                            gen::codelet32_stage1_fused_sh_cs(din2, tb, n2, m_sh_twr, m_sh_twi);
                            break;
                        case 64U:
                            gen::codelet64_stage1_fused_sh_cs(din2, tb, n2, m_sh_twr, m_sh_twi);
                            break;
                        case 128U:
                            gen::codelet128_stage1_fused_sh_cs(din2, tb, n2, m_sh_twr, m_sh_twi);
                            break;
                        default:
                            gen::codelet256_stage1_fused_sh_cs(din2, tb, n2, m_sh_twr, m_sh_twi);
                            break;
                    }
                }
                switch (n2) // stage-2: n1-wide batch of n2-point row FFTs → natural order out
                {
                    case 32U:
                        gen::codelet32_batched_sc(tb, din2, n1);
                        break;
                    case 64U:
                        gen::codelet64_batched_sc(tb, din2, n1);
                        break;
                    case 128U:
                        gen::codelet128_batched_sc(tb, din2, n1);
                        break;
                    default:
                        gen::codelet256_batched_sc(tb, din2, n1);
                        break;
                }
                return;
            }
        }
#endif
        // NB (2026-06-14): the depth-first recursion (rec_fft_soa) was MEASURED for the mid-band and is dead —
        // worse than this Stockham path at every size (8192 −6%, 65536 −27%, 131072 −33% collapsing). Decisive:
        // even fully L1-resident at n=1024 (ZERO cache penalty) this codelet path leaves throughput on the table,
        // so the mid-band wall is CODELET QUALITY in isolation, not cache structure; the lever is
        // gen_fft_codelets.py (modified split-radix + scheduling), not a recursive driver. rec_fft_soa is a seed only.
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
        // passes — 2 full memory passes that an in-place interleaved transform never pays.
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
    // batched primitive AND the sub-FFT engine for the block four-step/six-step (the large-N block technique).
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
#if CRD_SIMD_HAS_AVX2
        // f64 N=8, even batch: the AoS over-2 codelet is register-resident with no per-pass memory traffic (the
        // SoA path below pays log₂(n) passes). Adjacent transforms (t,t+1) are contiguous in element-major layout
        // = the over-2 ymm. four-step never calls this with n≤32 (it splits n≥2¹⁹ into ~2⁹ factors), so this fast
        // path is reached only by direct small-N batched callers (v10-e). N=16/32 batched stay on the SoA path.
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            if (m_n == 8 && (b & 1U) == 0U)
            {
                detail::small_n_batched8_f64(reinterpret_cast<crd::f64*>(d), b, dir == FftDirection::Forward);
                return;
            }
            // N=16 even-batch: the over-2 register-resident codelet, but only while the working set stays
            // cache-resident — the strided over-2 gather streams ~n·b complex per transform-pair, so beyond
            // batch_cache_budget the strided access thrashes L1 and the SoA path below wins. (N=8 has 8 strided
            // loads/pair and stays within L1 associativity for any batch, so it needs no such guard.)
            constexpr crd::usize batch_cache_budget = 2048; // n·b complex (~32 KiB f64) — measured cache-resident
            if (m_n == 16 && (b & 1U) == 0U && m_n * b <= batch_cache_budget)
            {
                detail::small_n_batched16_f64(reinterpret_cast<crd::f64*>(d), b, dir == FftDirection::Forward);
                return;
            }
        }
#endif
        // The hierarchical generated-codelet sub-FFTs replace the
        // radix-8 path for the f64 four-step sub-FFTs — 4096 = 64×64 (b==16: 8M-n1 + both 16M), 2048 = 64×32
        // (b==32: 8M-n2 + both 4M), 1024 = 32×32 (b==64: M5, 2M-n2 + both 1M). Stage-1 FUSED (split-radix leaf +
        // inner twiddle + transposed store into m_hier_bbuf) then a plain leaf stage-2 back into d (natural order).
        // Isolated 4096 1.24× / 2048 1.26× / 1024 1.23×. Full f64 vs MKL (CANONICAL bench_fft_vs_refs best-of-reps):
        // 4M ~1.04× (beats), 8M ~0.91×, 16M ~0.90×, 2M ~0.75×, 1M ~0.60× — the hier improves every size vs radix-8
        // (e.g. 1M 0.53→0.60, 2M 0.68→0.75) but MKL is very fast at the small cache-resident sizes. machine-eps
        // (~1e-15), deterministic. M7: f32 FORWARD also uses the hier (Vec8f, same decompositions, BB=block_width;
        // f32 8M 0.79→~1.0× MKL parity band, ~3e-7). Inverse (f32+f64) falls through to radix-8. See the
        // 2026-06-17 FFT logs. f64 + f32 (M9-FIXED: an M7 indentation miss had left the f32 dispatch off while the
        // ctor allocated the hier buffers). Stage b's = N2·BB (stage-1) / N1·BB
        // (stage-2), BB = block_width(m_n) (T-aware ⇒ f64 reproduces the old 1024/2048 literals exactly).
        constexpr bool k_hier_d = std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>;
        if constexpr (k_hier_d)
        {
            if (dir == FftDirection::Forward && m_hier_bbuf != nullptr)
            {
                if (m_n == 4096 && b == block_width(4096)) // 64×64; stage-1 b=64·BB, stage-2 b=64·BB
                {
                    gen::codelet64_stage1_fused_64x64(d, m_hier_bbuf, 64 * b, m_hier_twr, m_hier_twi);
                    gen::codelet64_batched(m_hier_bbuf, d, 64 * b);
                    return;
                }
                if (m_n == 2048 && b == block_width(2048)) // 64×32; stage-1 b=32·BB, stage-2 b=64·BB
                {
                    gen::codelet64_stage1_fused_64x32(d, m_hier_bbuf, 32 * b, m_hier_twr, m_hier_twi);
                    gen::codelet32_batched(m_hier_bbuf, d, 64 * b);
                    return;
                }
                if (m_n == 1024 && b == block_width(1024)) // 32×32; both stages b=32·BB
                {
                    gen::codelet32_stage1_fused_32x32(d, m_hier_bbuf, 32 * b, m_hier_twr, m_hier_twi);
                    gen::codelet32_batched(m_hier_bbuf, d, 32 * b);
                    return;
                }
                // 256 = 16×16 hier sub-FFT (256K four-step P2); f32 only (the codelet16 stage-1 is Vec8f).
                if constexpr (std::is_same_v<T, crd::f32>)
                {
                    if (m_n == 256 && b == block_width(256)) // both stages b=16·BB
                    {
                        gen::codelet16_stage1_fused_16x16(d, m_hier_bbuf, 16 * b, m_hier_twr, m_hier_twi);
                        gen::codelet16_batched(m_hier_bbuf, d, 16 * b);
                        return;
                    }
                }
            }
        }
        // FFT-CRUSH 2026-07-03 (batched-LEAF gate): n in {16,32,64} batched forward goes straight to the
        // generated split-radix leaf codelets (the hier stage-2 kernels, both types) instead of
        // bit-reversal + batched radix-8. IN-PLACE-safe (verified: every generated tile loads all inputs
        // before its first store) and tail-free (b stays a power of two >= the vector width here). This is
        // what makes the all-hier four-step splits (128K = 2048x64, 256K = 4096x64) fast on the P2 side.
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            constexpr crd::usize kw = std::is_same_v<T, crd::f32> ? 8U : 4U;
            if (dir == FftDirection::Forward && (b % kw) == 0U && b >= kw)
            {
                if (m_n == 256)
                {
                    gen::codelet256_batched(d, d, b); // GENERATED 2026-07-03 (rebuilt generator; f64 + f32)
                    return;
                }
                if (m_n == 128)
                {
                    gen::codelet128_batched(d, d, b); // GENERATED 2026-07-03 (rebuilt generator; f64 + f32)
                    return;
                }
                if (m_n == 64)
                {
                    gen::codelet64_batched(d, d, b);
                    return;
                }
                if (m_n == 32)
                {
                    gen::codelet32_batched(d, d, b);
                    return;
                }
                if (m_n == 16)
                {
                    gen::codelet16_batched(d, d, b);
                    return;
                }
            }
        }
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
            const crd::usize n4 = m_n >> 2;
            const crd::usize n8 = m_n >> 3;
            const crd::usize n38 = (3 * m_n) >> 3;
            for (crd::usize base = 0; base < m_n; base += (len << 3))
            {
                crd::usize tw = 0;
                for (crd::usize k = 0; k < q; ++k)
                {
                    // Nested radix-8 DIT twiddles: ws=W_{2q}^k; w0=W_{4q}^k, w1=W_{4q}^{k+q};
                    // v0=W_{8q}^k, v1=W_{8q}^{k+q}, v2=W_{8q}^{k+2q}, v3=W_{8q}^{k+3q}.
                    const T wsr = m_tw_re[4 * tw];
                    const T wsi = isign * m_tw_im[4 * tw];
                    const T w0r = m_tw_re[2 * tw];
                    const T w0i = isign * m_tw_im[2 * tw];
                    const T w1r = m_tw_re[2 * tw + n4];
                    const T w1i = isign * m_tw_im[2 * tw + n4];
                    const T v0r = m_tw_re[tw];
                    const T v0i = isign * m_tw_im[tw];
                    const T v1r = m_tw_re[tw + n8];
                    const T v1i = isign * m_tw_im[tw + n8];
                    const T v2r = m_tw_re[tw + n4];
                    const T v2i = isign * m_tw_im[tw + n4];
                    const T v3r = m_tw_re[tw + n38];
                    const T v3i = isign * m_tw_im[tw + n38];
                    const crd::usize p = (base + k) * b;
                    batched_butterfly8(d, b, p, p + q * b, p + 2 * q * b, p + 3 * q * b, p + 4 * q * b, p + 5 * q * b,
                                       p + 6 * q * b, p + 7 * q * b, wsr, wsi, w0r, w0i, w1r, w1i, v0r, v0i, v1r, v1i,
                                       v2r, v2i, v3r, v3i);
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
        {
            // SoA SIMD over the batch: f64 → Vec4d (4 transforms/vec), f32 → Vec8f (8/vec). Same body; the
            // complex deinterleave/interleave helpers are pure shuffles (bit-identical to the scalar tail).
            namespace simd = crd::math::simd;
            using V = std::conditional_t<std::is_same_v<T, crd::f64>, simd::Vec4d, simd::Vec8f>;
            constexpr crd::usize lanes = std::is_same_v<T, crd::f64> ? 4U : 8U;
            const V wrv(wr);
            const V wiv(wi);
            for (; t + lanes <= b; t += lanes)
            {
                V ar;
                V ai;
                V br;
                V bi;
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(arow + t), ar, ai);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(brow + t), br, bi);
                const V vr = wrv * br - wiv * bi;
                const V vi = wrv * bi + wiv * br;
                simd::store_complex_interleaved(reinterpret_cast<T*>(arow + t), ar + vr, ai + vi);
                simd::store_complex_interleaved(reinterpret_cast<T*>(brow + t), ar - vr, ai - vi);
            }
        }
        for (; t < b; ++t)
        {
            const T ar = arow[t].re;
            const T ai = arow[t].im;
            const T vr = wr * brow[t].re - wi * brow[t].im;
            const T vi = wr * brow[t].im + wi * brow[t].re;
            arow[t] = Complex<T>{ar + vr, ai + vi};
            brow[t] = Complex<T>{ar - vr, ai - vi};
        }
    }

    // One radix-4 batched DIT butterfly over the contiguous batch axis, for BIT-REVERSED input. Two fused
    // radix-2 stages: inner pairs (o0,o1)&(o2,o3) with ws=W_{2q}^k, then outer (e0,f0)&(e1,f1) with
    // w0=W_{4q}^k / w1=W_{4q}^{k+q}. In place; twiddles carry isign (pre-conjugated for inverse).
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC LTCG else inlines this ~24-live-Vec codelet into the radix-4 driver, stacking onto the codelet mass
    // there -> C1001 optimizer heap exhaustion (deterministically eliminated 2026-07-24; same fix as
    // execute_ip4aos / batched_codelets_gen). The call is amortized over the batch loop -> perf-neutral.
    __declspec(noinline)
#endif
    static void batched_butterfly4(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, crd::usize o2,
                                   crd::usize o3, T wsr, T wsi, T w0r, T w0i, T w1r, T w1i) noexcept
    {
        Complex<T>* r0 = d + o0;
        Complex<T>* r1 = d + o1;
        Complex<T>* r2 = d + o2;
        Complex<T>* r3 = d + o3;
        crd::usize t = 0;
        {
            namespace simd = crd::math::simd;
            using V = std::conditional_t<std::is_same_v<T, crd::f64>, simd::Vec4d, simd::Vec8f>;
            constexpr crd::usize lanes = std::is_same_v<T, crd::f64> ? 4U : 8U;
            const V wsrv(wsr);
            const V wsiv(wsi);
            const V w0rv(w0r);
            const V w0iv(w0i);
            const V w1rv(w1r);
            const V w1iv(w1i);
            for (; t + lanes <= b; t += lanes)
            {
                V x0r;
                V x0i;
                V x1r;
                V x1i;
                V x2r;
                V x2i;
                V x3r;
                V x3i;
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r0 + t), x0r, x0i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r1 + t), x1r, x1i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r2 + t), x2r, x2i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r3 + t), x3r, x3i);
                const V br = wsrv * x1r - wsiv * x1i; // ws·x1
                const V bi = wsrv * x1i + wsiv * x1r;
                const V er = wsrv * x3r - wsiv * x3i; // ws·x3
                const V ei = wsrv * x3i + wsiv * x3r;
                const V e0r = x0r + br;
                const V e0i = x0i + bi;
                const V e1r = x0r - br;
                const V e1i = x0i - bi;
                const V f0r = x2r + er;
                const V f0i = x2i + ei;
                const V f1r = x2r - er;
                const V f1i = x2i - ei;
                const V g0r = w0rv * f0r - w0iv * f0i; // w0·f0
                const V g0i = w0rv * f0i + w0iv * f0r;
                const V g1r = w1rv * f1r - w1iv * f1i; // w1·f1
                const V g1i = w1rv * f1i + w1iv * f1r;
                simd::store_complex_interleaved(reinterpret_cast<T*>(r0 + t), e0r + g0r, e0i + g0i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r1 + t), e1r + g1r, e1i + g1i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r2 + t), e0r - g0r, e0i - g0i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r3 + t), e1r - g1r, e1i - g1i);
            }
        }
        for (; t < b; ++t)
        {
            const T x0r = r0[t].re;
            const T x0i = r0[t].im;
            const T x2r = r2[t].re;
            const T x2i = r2[t].im;
            const T br = wsr * r1[t].re - wsi * r1[t].im;
            const T bi = wsr * r1[t].im + wsi * r1[t].re;
            const T er = wsr * r3[t].re - wsi * r3[t].im;
            const T ei = wsr * r3[t].im + wsi * r3[t].re;
            const T e0r = x0r + br;
            const T e0i = x0i + bi;
            const T e1r = x0r - br;
            const T e1i = x0i - bi;
            const T f0r = x2r + er;
            const T f0i = x2i + ei;
            const T f1r = x2r - er;
            const T f1i = x2i - ei;
            const T g0r = w0r * f0r - w0i * f0i;
            const T g0i = w0r * f0i + w0i * f0r;
            const T g1r = w1r * f1r - w1i * f1i;
            const T g1i = w1r * f1i + w1i * f1r;
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
#if defined(_MSC_VER) && !defined(__clang__)
    // Same MSVC LTCG heap-exhaustion guard as batched_butterfly4: keep this radix-8 SIMD codelet out-of-line so
    // the optimizer does not stack it onto the radix driver (C1001). Batch-amortized call -> perf-neutral.
    __declspec(noinline)
#endif
    static void batched_butterfly8(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, crd::usize o2,
                                   crd::usize o3, crd::usize o4, crd::usize o5, crd::usize o6, crd::usize o7, T wsr,
                                   T wsi, T w0r, T w0i, T w1r, T w1i, T v0r, T v0i, T v1r, T v1i, T v2r, T v2i, T v3r,
                                   T v3i) noexcept
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
        {
            namespace simd = crd::math::simd;
            using V = std::conditional_t<std::is_same_v<T, crd::f64>, simd::Vec4d, simd::Vec8f>;
            constexpr crd::usize lanes = std::is_same_v<T, crd::f64> ? 4U : 8U;
            const V wsrv(wsr);
            const V wsiv(wsi);
            const V w0rv(w0r);
            const V w0iv(w0i);
            const V w1rv(w1r);
            const V w1iv(w1i);
            const V v0rv(v0r);
            const V v0iv(v0i);
            const V v1rv(v1r);
            const V v1iv(v1i);
            const V v2rv(v2r);
            const V v2iv(v2i);
            const V v3rv(v3r);
            const V v3iv(v3i);
            for (; t + lanes <= b; t += lanes)
            {
                V x0r;
                V x0i;
                V x1r;
                V x1i;
                V x2r;
                V x2i;
                V x3r;
                V x3i;
                V x4r;
                V x4i;
                V x5r;
                V x5i;
                V x6r;
                V x6i;
                V x7r;
                V x7i;
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r0 + t), x0r, x0i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r1 + t), x1r, x1i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r2 + t), x2r, x2i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r3 + t), x3r, x3i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r4 + t), x4r, x4i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r5 + t), x5r, x5i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r6 + t), x6r, x6i);
                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(r7 + t), x7r, x7i);
                const V s1r = wsrv * x1r - wsiv * x1i;
                const V s1i = wsrv * x1i + wsiv * x1r;
                const V s3r = wsrv * x3r - wsiv * x3i;
                const V s3i = wsrv * x3i + wsiv * x3r;
                const V s5r = wsrv * x5r - wsiv * x5i;
                const V s5i = wsrv * x5i + wsiv * x5r;
                const V s7r = wsrv * x7r - wsiv * x7i;
                const V s7i = wsrv * x7i + wsiv * x7r;
                const V a0r = x0r + s1r;
                const V a0i = x0i + s1i;
                const V a1r = x0r - s1r;
                const V a1i = x0i - s1i;
                const V a2r = x2r + s3r;
                const V a2i = x2i + s3i;
                const V a3r = x2r - s3r;
                const V a3i = x2i - s3i;
                const V a4r = x4r + s5r;
                const V a4i = x4i + s5i;
                const V a5r = x4r - s5r;
                const V a5i = x4i - s5i;
                const V a6r = x6r + s7r;
                const V a6i = x6i + s7i;
                const V a7r = x6r - s7r;
                const V a7i = x6i - s7i;
                const V m2r = w0rv * a2r - w0iv * a2i;
                const V m2i = w0rv * a2i + w0iv * a2r;
                const V m3r = w1rv * a3r - w1iv * a3i;
                const V m3i = w1rv * a3i + w1iv * a3r;
                const V m6r = w0rv * a6r - w0iv * a6i;
                const V m6i = w0rv * a6i + w0iv * a6r;
                const V m7r = w1rv * a7r - w1iv * a7i;
                const V m7i = w1rv * a7i + w1iv * a7r;
                const V c0r = a0r + m2r;
                const V c0i = a0i + m2i;
                const V c2r = a0r - m2r;
                const V c2i = a0i - m2i;
                const V c1r = a1r + m3r;
                const V c1i = a1i + m3i;
                const V c3r = a1r - m3r;
                const V c3i = a1i - m3i;
                const V c4r = a4r + m6r;
                const V c4i = a4i + m6i;
                const V c6r = a4r - m6r;
                const V c6i = a4i - m6i;
                const V c5r = a5r + m7r;
                const V c5i = a5i + m7i;
                const V c7r = a5r - m7r;
                const V c7i = a5i - m7i;
                const V n4r = v0rv * c4r - v0iv * c4i;
                const V n4i = v0rv * c4i + v0iv * c4r;
                const V n5r = v1rv * c5r - v1iv * c5i;
                const V n5i = v1rv * c5i + v1iv * c5r;
                const V n6r = v2rv * c6r - v2iv * c6i;
                const V n6i = v2rv * c6i + v2iv * c6r;
                const V n7r = v3rv * c7r - v3iv * c7i;
                const V n7i = v3rv * c7i + v3iv * c7r;
                simd::store_complex_interleaved(reinterpret_cast<T*>(r0 + t), c0r + n4r, c0i + n4i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r1 + t), c1r + n5r, c1i + n5i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r2 + t), c2r + n6r, c2i + n6i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r3 + t), c3r + n7r, c3i + n7i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r4 + t), c0r - n4r, c0i - n4i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r5 + t), c1r - n5r, c1i - n5i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r6 + t), c2r - n6r, c2i - n6i);
                simd::store_complex_interleaved(reinterpret_cast<T*>(r7 + t), c3r - n7r, c3i - n7i);
            }
        }
        for (; t < b; ++t)
        {
            // stage A: a_even = x_even + ws·x_odd, a_odd = x_even - ws·x_odd
            const T s1r = wsr * r1[t].re - wsi * r1[t].im;
            const T s1i = wsr * r1[t].im + wsi * r1[t].re;
            const T s3r = wsr * r3[t].re - wsi * r3[t].im;
            const T s3i = wsr * r3[t].im + wsi * r3[t].re;
            const T s5r = wsr * r5[t].re - wsi * r5[t].im;
            const T s5i = wsr * r5[t].im + wsi * r5[t].re;
            const T s7r = wsr * r7[t].re - wsi * r7[t].im;
            const T s7i = wsr * r7[t].im + wsi * r7[t].re;
            const T a0r = r0[t].re + s1r;
            const T a0i = r0[t].im + s1i;
            const T a1r = r0[t].re - s1r;
            const T a1i = r0[t].im - s1i;
            const T a2r = r2[t].re + s3r;
            const T a2i = r2[t].im + s3i;
            const T a3r = r2[t].re - s3r;
            const T a3i = r2[t].im - s3i;
            const T a4r = r4[t].re + s5r;
            const T a4i = r4[t].im + s5i;
            const T a5r = r4[t].re - s5r;
            const T a5i = r4[t].im - s5i;
            const T a6r = r6[t].re + s7r;
            const T a6i = r6[t].im + s7i;
            const T a7r = r6[t].re - s7r;
            const T a7i = r6[t].im - s7i;
            // stage B: w0 on (a0,a2)(a4,a6); w1 on (a1,a3)(a5,a7)
            const T m2r = w0r * a2r - w0i * a2i;
            const T m2i = w0r * a2i + w0i * a2r;
            const T m3r = w1r * a3r - w1i * a3i;
            const T m3i = w1r * a3i + w1i * a3r;
            const T m6r = w0r * a6r - w0i * a6i;
            const T m6i = w0r * a6i + w0i * a6r;
            const T m7r = w1r * a7r - w1i * a7i;
            const T m7i = w1r * a7i + w1i * a7r;
            const T c0r = a0r + m2r;
            const T c0i = a0i + m2i;
            const T c2r = a0r - m2r;
            const T c2i = a0i - m2i;
            const T c1r = a1r + m3r;
            const T c1i = a1i + m3i;
            const T c3r = a1r - m3r;
            const T c3i = a1i - m3i;
            const T c4r = a4r + m6r;
            const T c4i = a4i + m6i;
            const T c6r = a4r - m6r;
            const T c6i = a4i - m6i;
            const T c5r = a5r + m7r;
            const T c5i = a5i + m7i;
            const T c7r = a5r - m7r;
            const T c7i = a5i - m7i;
            // stage C: v0..v3 on (c0,c4)(c1,c5)(c2,c6)(c3,c7)
            const T n4r = v0r * c4r - v0i * c4i;
            const T n4i = v0r * c4i + v0i * c4r;
            const T n5r = v1r * c5r - v1i * c5i;
            const T n5i = v1r * c5i + v1i * c5r;
            const T n6r = v2r * c6r - v2i * c6i;
            const T n6i = v2r * c6i + v2i * c6r;
            const T n7r = v3r * c7r - v3i * c7i;
            const T n7i = v3r * c7i + v3i * c7r;
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
    // A raw table of `count` Us at `alignment` (64 or 32: the aligned SIMD loads and the streaming stores need it). A
    // refused table, and a count whose byte size overflows, are the plan's out-of-memory fatal (DIAG.3a).
    template <typename U> [[nodiscard]] U* plan_table(crd::usize count, crd::usize alignment)
    {
        return crd::memory::allocate_array_or_fatal<U>(*m_alloc, count, alignment, detail::kFftPlanOutOfMemory);
    }
    // A four-step sub-plan of size `n`, built from the same allocator.
    [[nodiscard]] FftPlan* sub_plan(crd::usize n)
    {
        return crd::memory::construct_or_fatal<FftPlan>(*m_alloc, detail::kFftPlanOutOfMemory, m_alloc, n);
    }
    // The g-th of four references (g < 4): execute_ip4aos' choice of a len-4 block's output pair.
    template <typename U> [[nodiscard]] static U& pick_of_four(crd::usize g, U& a, U& b, U& c, U& d) noexcept
    {
        if (g == 0)
        {
            return a;
        }
        if (g == 1)
        {
            return b;
        }
        if (g == 2)
        {
            return c;
        }
        return d;
    }

    // Recursive DIT core: writes the n-point DFT of the strided SoA input (inr/ini, stride si) into the
    // contiguous output (outr/outi). RADIX-4 split (stride ×4), recurse into the 4 cosets, then a SIMD
    // in-place radix-4 combine. Same work-per-element as the direct radix-4 Stockham, but depth-first so the
    // sub-problems go cache-resident (the cache-oblivious win). Base case n≤32 = the generated codelet
    // reading at stride si. (n is a power of 2 ≥ 64 here ⇒ divisible by 4, and /4 descends to a 16/32 base.)
    void rec_fft_soa(T* outr, T* outi, const T* inr, const T* ini, crd::usize si, crd::usize n, FftDirection dir) const
    {
        if (n <= 32)
        {
            dispatch_codelet_strided(inr, ini, si, outr, outi, n, dir);
            return;
        }
        const crd::usize m = n >> 2;
        const crd::usize s4 = si << 2;
        rec_fft_soa(outr, outi, inr, ini, s4, m, dir);                                   // coset 0 → out[0..m)
        rec_fft_soa(outr + m, outi + m, inr + si, ini + si, s4, m, dir);                 // coset 1
        rec_fft_soa(outr + 2 * m, outi + 2 * m, inr + 2 * si, ini + 2 * si, s4, m, dir); // coset 2
        rec_fft_soa(outr + 3 * m, outi + 3 * m, inr + 3 * si, ini + 3 * si, s4, m, dir); // coset 3
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
                const V x0r = V::load(outr + k2);
                const V x0i = V::load(outi + k2);
                const V x1r = V::load(outr + m + k2);
                const V x1i = V::load(outi + m + k2);
                const V x2r = V::load(outr + 2 * m + k2);
                const V x2i = V::load(outi + 2 * m + k2);
                const V x3r = V::load(outr + 3 * m + k2);
                const V x3i = V::load(outi + 3 * m + k2);
                const crd::usize a = k2 * ts;
                const crd::usize b = (k2 + 1) * ts;
                const crd::usize c = (k2 + 2) * ts;
                const crd::usize d = (k2 + 3) * ts;
                const V w1r(m_tw_re[a], m_tw_re[b], m_tw_re[c], m_tw_re[d]);
                const V w1i = V(m_tw_im[a], m_tw_im[b], m_tw_im[c], m_tw_im[d]) * sg;
                const V w2r(m_tw_re[2 * a], m_tw_re[2 * b], m_tw_re[2 * c], m_tw_re[2 * d]);
                const V w2i = V(m_tw_im[2 * a], m_tw_im[2 * b], m_tw_im[2 * c], m_tw_im[2 * d]) * sg;
                const V w3r(m_tw_re[3 * a], m_tw_re[3 * b], m_tw_re[3 * c], m_tw_re[3 * d]);
                const V w3i = V(m_tw_im[3 * a], m_tw_im[3 * b], m_tw_im[3 * c], m_tw_im[3 * d]) * sg;
                const V t1r = w1r * x1r - w1i * x1i;
                const V t1i = w1r * x1i + w1i * x1r;
                const V t2r = w2r * x2r - w2i * x2i;
                const V t2i = w2r * x2i + w2i * x2r;
                const V t3r = w3r * x3r - w3i * x3i;
                const V t3i = w3r * x3i + w3i * x3r;
                const V a0r = x0r + t2r;
                const V a0i = x0i + t2i;
                const V a1r = x0r - t2r;
                const V a1i = x0i - t2i;
                const V a2r = t1r + t3r;
                const V a2i = t1i + t3i;
                const V a3r = t1r - t3r;
                const V a3i = t1i - t3i;
                const V rsr = V(0.0) - sg * a3i;
                const V rsi = sg * a3r;
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
            const T w1r = m_tw_re[a];
            const T w1i = isign * m_tw_im[a];
            const T w2r = m_tw_re[2 * a];
            const T w2i = isign * m_tw_im[2 * a];
            const T w3r = m_tw_re[3 * a];
            const T w3i = isign * m_tw_im[3 * a];
            const T x0r = outr[k2];
            const T x0i = outi[k2];
            const T t1r = w1r * outr[m + k2] - w1i * outi[m + k2];
            const T t1i = w1r * outi[m + k2] + w1i * outr[m + k2];
            const T t2r = w2r * outr[2 * m + k2] - w2i * outi[2 * m + k2];
            const T t2i = w2r * outi[2 * m + k2] + w2i * outr[2 * m + k2];
            const T t3r = w3r * outr[3 * m + k2] - w3i * outi[3 * m + k2];
            const T t3i = w3r * outi[3 * m + k2] + w3i * outr[3 * m + k2];
            const T a0r = x0r + t2r;
            const T a0i = x0i + t2i;
            const T a1r = x0r - t2r;
            const T a1i = x0i - t2i;
            const T a2r = t1r + t3r;
            const T a2i = t1i + t3i;
            const T a3r = t1r - t3r;
            const T a3i = t1i - t3i;
            const T rsr = -isign * a3i;
            const T rsi = isign * a3r;
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
    // ⭐ Lever D RESURRECTION (2026-06-14): the prior floor (~51 ms @8M) was the SCALAR/strided transpose
    // running at ~8 GB/s. A blocked transpose with NON-TEMPORAL STORES measured 25.7 GB/s on this box (3× that)
    // ⇒ the 4n transpose floor drops to ~21 ms ⇒ four-step viable at large N. The
    // block four-step below uses NT-store scatter + the hoisted batched sub-FFTs. Enabled above the crossover
    // (SCAN value — narrow once the kernel + transpose are tuned). Gated vs the radix-2 oracle.
    static constexpr crd::usize kFourStepMin = crd::usize{1} << 19; // 512K+ DRAM trough (f32 256K opted in via the ctor)
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
                fwd ? detail::codelet_16_fwd<T>(re, im, 1, re, im, 1) : detail::codelet_16_inv<T>(re, im, 1, re, im, 1);
                break;
            case 32:
                fwd ? detail::codelet_32_fwd<T>(re, im, 1, re, im, 1) : detail::codelet_32_inv<T>(re, im, 1, re, im, 1);
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
                simd::Vec4d ar;
                simd::Vec4d ai;
                simd::Vec4d br;
                simd::Vec4d bi;
                simd::Vec4d cr;
                simd::Vec4d ci;
                simd::Vec4d dr;
                simd::Vec4d di;
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

    // SIX-STEP FFT (Takahashi 2019 §6.3 / Bailey; built 2026-07-04) — the CACHE-machine framework,
    // SQUARE splits only (n = m·m): every FFT runs on CONTIGUOUS data (the L1-resident standalone
    // 2-pass engine, 30-39 GF/s) and every transpose is IN-PLACE tile-pairwise — six passes, zero
    // scratch copies, natural-order output in the caller's buffer. The step-3 twiddle w_n^{j1·k2}
    // fuses into the middle transpose via the four-step's FACTORED hi/lo tables (no n-sized stream).
    // This replaces the four-step's stride-bound gather/scatter phases; the five-step's strided
    // multirow failure (stream-count wall) is what this variant structurally avoids.
    // v2 (2026-07-04): SIMD micro-tile pair-swap transpose — lanes×lanes complex tiles moved via
    // deinterleaved loads + per-plane register transpose + interleaved CONTIGUOUS stores on BOTH
    // halves (the v14-d tensor-permute discipline; v1's element-scalar swaps were the measured
    // wall). The fused T2 twiddle W_n^{r·c} evolves by per-row recurrence along c (step W_n^{r·lanes}),
    // reseeded from the FACTORED hi/lo tables at every kTb block (≤16 steps — the proven four-step
    // reseed class). Symmetry w(r,c)=w(c,r) twiddles the U-half via the transposed twiddle tile.
    void fs6_transpose_inplace(Complex<T>* a, crd::usize m, bool twiddle) const
    {
        namespace simd = crd::math::simd;
        using V = std::conditional_t<std::is_same_v<T, crd::f64>, simd::Vec4d, simd::Vec8f>;
        constexpr crd::usize lanes = std::is_same_v<T, crd::f64> ? 4U : 8U;
        // block so the tile-PAIR footprint (2 · ktb rows · m·sizeof(Complex<T>) row stride) stays
        // L2-resident: 64 at m=1024 (1 MB pair), 32 at m>=2048 (4M measured −13% with 64: 2 MB pair).
        const crd::usize ktb = (m >= 2048) ? 32U : 64U;
        const crd::usize mmask = (crd::usize{1} << m_ftw_h) - 1;
        const T* const hir = m_ftw_hi_re.data();
        const T* const hii = m_ftw_hi_im.data();
        const T* const lor = m_ftw_lo_re.data();
        const T* const loi = m_ftw_lo_im.data();
        const auto tw_of = [&](crd::usize r, crd::usize c) noexcept -> Complex<T>
        {
            const crd::usize aa = r * c; // < n (r,c < m, m·m == n)
            const T hr = hir[aa >> m_ftw_h];
            const T hi = hii[aa >> m_ftw_h];
            const T lr = lor[aa & mmask];
            const T li = loi[aa & mmask];
            return Complex<T>{hr * lr - hi * li, hr * li + hi * lr}; // W_n^{r·c} (cos, −sin)
        };
        const auto tp = [](V* xr, V* xi) noexcept
        {
            if constexpr (lanes == 4U)
            {
                simd::transpose4x4(xr[0], xr[1], xr[2], xr[3]);
                simd::transpose4x4(xi[0], xi[1], xi[2], xi[3]);
            }
            else
            {
                simd::transpose8x8(xr[0], xr[1], xr[2], xr[3], xr[4], xr[5], xr[6], xr[7]);
                simd::transpose8x8(xi[0], xi[1], xi[2], xi[3], xi[4], xi[5], xi[6], xi[7]);
            }
        };
        for (crd::usize i0 = 0; i0 < m; i0 += ktb)
        {
            for (crd::usize j0 = i0; j0 < m; j0 += ktb)
            {
                for (crd::usize r = i0; r < i0 + ktb; r += lanes)
                {
                    // per-row twiddle state for rows r..r+lanes over columns j0.. (recurrence along c)
                    V wr[lanes];
                    V wi[lanes];
                    V sr[lanes];
                    V si[lanes];
                    if (twiddle)
                    {
                        for (crd::usize i = 0; i < lanes; ++i)
                        {
                            alignas(64) T row_r[lanes];
                            alignas(64) T row_i[lanes];
                            for (crd::usize j = 0; j < lanes; ++j)
                            {
                                const Complex<T> w = tw_of(r + i, j0 + j);
                                row_r[j] = w.re;
                                row_i[j] = w.im;
                            }
                            wr[i] = V::load(row_r);
                            wi[i] = V::load(row_i);
                            const Complex<T> s = tw_of(r + i, lanes); // step W_n^{(r+i)·lanes} — exact table product
                            sr[i] = V(s.re);
                            si[i] = V(s.im);
                        }
                    }
                    const crd::usize cbeg = (j0 == i0) ? r : j0; // diagonal block: upper micro-tiles only
                    // advance the recurrence to cbeg if it starts past j0 (diagonal block rows)
                    if (twiddle && cbeg != j0)
                    {
                        for (crd::usize c = j0; c < cbeg; c += lanes)
                        {
                            for (crd::usize i = 0; i < lanes; ++i)
                            {
                                const V nr = simd::fnmadd(wi[i], si[i], wr[i] * sr[i]);
                                const V ni = simd::fma(wi[i], sr[i], wr[i] * si[i]);
                                wr[i] = nr;
                                wi[i] = ni;
                            }
                        }
                    }
                    for (crd::usize c = cbeg; c < j0 + ktb; c += lanes)
                    {
                        V ur[lanes];
                        V ui[lanes];
                        for (crd::usize i = 0; i < lanes; ++i)
                        {
                            simd::load_complex_deinterleaved(reinterpret_cast<const T*>(a + (r + i) * m + c),
                                                             ur[i], ui[i]);
                        }
                        if (r == c) // diagonal micro-tile: transpose + twiddle in place
                        {
                            tp(ur, ui);
                            for (crd::usize i = 0; i < lanes; ++i)
                            {
                                if (twiddle)
                                {
                                    const V xr = simd::fnmadd(ui[i], wi[i], ur[i] * wr[i]);
                                    const V xi = simd::fma(ur[i], wi[i], ui[i] * wr[i]);
                                    ur[i] = xr;
                                    ui[i] = xi;
                                }
                                simd::store_complex_interleaved(reinterpret_cast<T*>(a + (r + i) * m + c), ur[i],
                                                                ui[i]);
                            }
                        }
                        else
                        {
                            V vr[lanes];
                            V vi[lanes];
                            for (crd::usize j = 0; j < lanes; ++j)
                            {
                                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(a + (c + j) * m + r),
                                                                 vr[j], vi[j]);
                            }
                            tp(ur, ui); // Ut row j = destination row (c+j) content across r..r+lanes
                            tp(vr, vi); // Vt row i = destination row (r+i) content across c..c+lanes
                            if (twiddle)
                            {
                                // V-half: destination rows r+i twiddle with the recurrence rows directly
                                for (crd::usize i = 0; i < lanes; ++i)
                                {
                                    const V xr = simd::fnmadd(vi[i], wi[i], vr[i] * wr[i]);
                                    const V xi = simd::fma(vr[i], wi[i], vi[i] * wr[i]);
                                    vr[i] = xr;
                                    vi[i] = xi;
                                }
                                // U-half needs the TRANSPOSED twiddle tile (w(c+j, r+i) = Tw[i][j])
                                V twtr[lanes];
                                V twti[lanes];
                                for (crd::usize i = 0; i < lanes; ++i)
                                {
                                    twtr[i] = wr[i];
                                    twti[i] = wi[i];
                                }
                                tp(twtr, twti);
                                for (crd::usize j = 0; j < lanes; ++j)
                                {
                                    const V xr = simd::fnmadd(ui[j], twti[j], ur[j] * twtr[j]);
                                    const V xi = simd::fma(ur[j], twti[j], ui[j] * twtr[j]);
                                    ur[j] = xr;
                                    ui[j] = xi;
                                }
                            }
                            for (crd::usize i = 0; i < lanes; ++i) // contiguous stores, BOTH halves
                            {
                                simd::store_complex_interleaved(reinterpret_cast<T*>(a + (r + i) * m + c), vr[i],
                                                                vi[i]);
                            }
                            for (crd::usize j = 0; j < lanes; ++j)
                            {
                                simd::store_complex_interleaved(reinterpret_cast<T*>(a + (c + j) * m + r), ur[j],
                                                                ui[j]);
                            }
                        }
                        if (twiddle) // advance the row recurrence by lanes columns
                        {
                            for (crd::usize i = 0; i < lanes; ++i)
                            {
                                const V nr = simd::fnmadd(wi[i], si[i], wr[i] * sr[i]);
                                const V ni = simd::fma(wi[i], sr[i], wr[i] * si[i]);
                                wr[i] = nr;
                                wi[i] = ni;
                            }
                        }
                    }
                }
            }
        }
    }

    // OUT-OF-PLACE micro-tile transpose (T3-fusion rechain, 2026-07-04): src rows -> dst rows, both
    // sides CONTIGUOUS per lanes×lanes tile, one-directional streams — the DRAM-page-friendly form the
    // in-place pair-swap lacks at ≥4M. Optional fused W_n^{r·c} twiddle (same recurrence as in-place).
    void fs6_transpose_oop(const Complex<T>* src, Complex<T>* dst, crd::usize m, bool twiddle) const
    {
        namespace simd = crd::math::simd;
        using V = std::conditional_t<std::is_same_v<T, crd::f64>, simd::Vec4d, simd::Vec8f>;
        constexpr crd::usize lanes = std::is_same_v<T, crd::f64> ? 4U : 8U;
        const crd::usize ktb = (m >= 2048) ? 32U : 64U;
        const crd::usize mmask = (crd::usize{1} << m_ftw_h) - 1;
        const T* const hir = m_ftw_hi_re.data();
        const T* const hii = m_ftw_hi_im.data();
        const T* const lor = m_ftw_lo_re.data();
        const T* const loi = m_ftw_lo_im.data();
        const auto tw_of = [&](crd::usize r, crd::usize c) noexcept -> Complex<T>
        {
            const crd::usize aa = r * c;
            const T hr = hir[aa >> m_ftw_h];
            const T hi = hii[aa >> m_ftw_h];
            const T lr = lor[aa & mmask];
            const T li = loi[aa & mmask];
            return Complex<T>{hr * lr - hi * li, hr * li + hi * lr};
        };
        const auto tp = [](V* xr, V* xi) noexcept
        {
            if constexpr (lanes == 4U)
            {
                simd::transpose4x4(xr[0], xr[1], xr[2], xr[3]);
                simd::transpose4x4(xi[0], xi[1], xi[2], xi[3]);
            }
            else
            {
                simd::transpose8x8(xr[0], xr[1], xr[2], xr[3], xr[4], xr[5], xr[6], xr[7]);
                simd::transpose8x8(xi[0], xi[1], xi[2], xi[3], xi[4], xi[5], xi[6], xi[7]);
            }
        };
        for (crd::usize i0 = 0; i0 < m; i0 += ktb)
        {
            for (crd::usize j0 = 0; j0 < m; j0 += ktb)
            {
                for (crd::usize r = i0; r < i0 + ktb; r += lanes)
                {
                    V wr[lanes];
                    V wi[lanes];
                    V sr[lanes];
                    V si[lanes];
                    if (twiddle) // per-row recurrence over c, reseeded per block (proven class)
                    {
                        for (crd::usize i = 0; i < lanes; ++i)
                        {
                            alignas(64) T row_r[lanes];
                            alignas(64) T row_i[lanes];
                            for (crd::usize j = 0; j < lanes; ++j)
                            {
                                const Complex<T> w = tw_of(r + i, j0 + j);
                                row_r[j] = w.re;
                                row_i[j] = w.im;
                            }
                            wr[i] = V::load(row_r);
                            wi[i] = V::load(row_i);
                            const Complex<T> s = tw_of(r + i, lanes);
                            sr[i] = V(s.re);
                            si[i] = V(s.im);
                        }
                    }
                    for (crd::usize c = j0; c < j0 + ktb; c += lanes)
                    {
                        V ur[lanes];
                        V ui[lanes];
                        for (crd::usize i = 0; i < lanes; ++i)
                        {
                            simd::load_complex_deinterleaved(reinterpret_cast<const T*>(src + (r + i) * m + c),
                                                             ur[i], ui[i]);
                        }
                        if (twiddle) // dst[c+j][r+i] = src[r+i][c+j]·w(r+i,c+j): apply BEFORE transposing
                        {
                            for (crd::usize i = 0; i < lanes; ++i)
                            {
                                const V xr2 = simd::fnmadd(ui[i], wi[i], ur[i] * wr[i]);
                                const V xi2 = simd::fma(ur[i], wi[i], ui[i] * wr[i]);
                                ur[i] = xr2;
                                ui[i] = xi2;
                            }
                        }
                        tp(ur, ui);
                        for (crd::usize j = 0; j < lanes; ++j)
                        {
                            simd::store_complex_interleaved(reinterpret_cast<T*>(dst + (c + j) * m + r), ur[j],
                                                            ui[j]);
                        }
                        if (twiddle)
                        {
                            for (crd::usize i = 0; i < lanes; ++i)
                            {
                                const V nr = simd::fnmadd(wi[i], si[i], wr[i] * sr[i]);
                                const V ni = simd::fma(wi[i], sr[i], wr[i] * si[i]);
                                wr[i] = nr;
                                wi[i] = ni;
                            }
                        }
                    }
                }
            }
        }
    }

    void execute_six_step(crd::containers::Span<Complex<T>> data) const
    {
        const crd::usize m = m_n1; // square: n = m·m; row FFTs via the hoisted sub-plan
        Complex<T>* const x = data.data();
        // ALL-IN-PLACE chain (re-verified 2026-07-04): at 1M the whole array is L3-RESIDENT — the
        // in-place transposes keep every pass L3-hot. The out-of-place rechain (fs6_transpose_oop,
        // kept for A/B) MEASURED WORSE everywhere (1M 5.38→6.69 ms, 4M 33.5→40.4): the x↔t ping-pong
        // doubles the live footprint and thrashes L3.
        fs6_transpose_inplace(x, m, false); // T1
        for (crd::usize r = 0; r < m; ++r)  // sweep A: m contiguous m-point FFTs
        {
            m_p1->execute(crd::containers::Span<Complex<T>>(x + r * m, m), FftDirection::Forward);
        }
        fs6_transpose_inplace(x, m, true); // T2 fused with w_n^{j1·k2}
        for (crd::usize r = 0; r < m; ++r) // sweep B
        {
            m_p1->execute(crd::containers::Span<Complex<T>>(x + r * m, m), FftDirection::Forward);
        }
        fs6_transpose_inplace(x, m, false); // T3 -> natural order
    }

#if CRD_SIMD_HAS_AVX2
    // IP4-AoS (PROMOTED): the INTERLEAVED in-place radix-4 DIT engine, f64 1K..64K both parities
    // and both DIRECTIONS. INV = inverse: conjugated twiddles throughout — folded into the fold's
    // sgn vector (zero extra ops: only t.hi feeds mix_lo_hi), compile-time-negated fold constants,
    // an X1/X3 swap in bf4, and negate-at-load for table twiddles (measured free, round 15).
    // Design record: docs/research/fft-stockham-v2.md rounds 1-17.
    template <bool INV>
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC LTCG inlines both instantiations into execute(), stacking this ~600-line SIMD driver
    // on top of the codelet mass there — part of the C1002 pass-2 heap exhaustion (2026-07-05).
    // A call at n >= 1024 is noise. gcc/clang keep their own inlining choice (measured baseline).
    __declspec(noinline)
#endif
    void execute_ip4aos(crd::containers::Span<Complex<T>> data) const
    {
        namespace simd = crd::math::simd;
        constexpr bool is_f32 = std::is_same_v<T, crd::f32>;
        using V = std::conditional_t<is_f32, simd::Vec8f, simd::Vec4d>;
        constexpr crd::usize per_vec = is_f32 ? 4 : 2; // complex per vector
        // 4-tuple constant builder: one value per T-lane over a complex PAIR; the f32 vector holds
        // two such pairs (twin units) ⇒ the tuple duplicates across 128-bit halves.
        const auto mkv = [](double a, double b, double c, double d) noexcept -> V
        {
            if constexpr (is_f32)
            {
                return V(static_cast<T>(a), static_cast<T>(b), static_cast<T>(c), static_cast<T>(d),
                         static_cast<T>(a), static_cast<T>(b), static_cast<T>(c), static_cast<T>(d));
            }
            else
            {
                return V(a, b, c, d);
            }
        };
        constexpr double ns = INV ? -1.0 : 1.0; // conjugation sign (applied inside mkv, T-cast there)
        const crd::usize n = m_n;
        const bool ipodd = (m_log2 & 1U) != 0U; // n = 2·4^k: two half transforms + radix-2 combine
        const crd::usize nh = ipodd ? (n >> 1) : n;
        Complex<T>* const io = data.data();
        Complex<T>* const tb = m_sh_t;
        // Slot map (round 10): 64B pad per 4KB. On 4K pages this measured neutral-to-worse (round
        // 9) because frame randomization already breaks set conflicts — but VTune named the 4K-page
        // cost (DTLB overhead = 24.5% of clockticks), and on 2MB pages (the DTLB fix) the pad is
        // ESSENTIAL: huge pages map virtual≡physical set bits, so unpadded power-of-2 strides hit
        // identical L2 sets. Pad + huge pages fix BOTH; either alone loses.
        const auto ps = [](crd::usize s) noexcept -> crd::usize { return s; };
        // radix-4 DIT butterfly on interleaved vectors (twiddles pre-dup'd, w applied to x1..x3):
        const auto bf4 = [](V& x0, V& x1, V& x2, V& x3, V w1r, V w1i, V w2r, V w2i, V w3r,
                            V w3i) noexcept
        {
            const V b1 = simd::fmaddsub(x1, w1r, simd::swap_pairs(x1) * w1i);
            const V c1 = simd::fmaddsub(x2, w2r, simd::swap_pairs(x2) * w2i);
            const V d1 = simd::fmaddsub(x3, w3r, simd::swap_pairs(x3) * w3i);
            const V t0 = x0 + c1;
            const V t1 = x0 - c1;
            const V t2 = b1 + d1;
            const V t3 = b1 - d1;
            const V ts = simd::swap_pairs(t3);
            x0 = t0 + t2;
            x2 = t0 - t2;
            if constexpr (INV) // inverse: ±i swap → X1 = t1 + i·t3, X3 = t1 - i·t3
            {
                x1 = simd::addsub(t1, ts);
                x3 = simd::addsub(t1, V::zero() - ts);
            }
            else
            {
                x1 = simd::addsub(t1, V::zero() - ts); // X1 = t1 - i·t3
                x3 = simd::addsub(t1, ts);             // X3 = t1 + i·t3
            }
        };
        // digit-reverse gather + len-4 + len-16, THREE layers fused (round 7): rev(s+m) = rev(s) +
        // m·n/4 ⇒ quarter-spaced loads; the len-4 DFT in 2-complex form (P-trick, signed-zero-exact);
        // the len-16 layer runs on the 16 in-register values with COMPILE-TIME twiddles W16^m
        // (correctly-rounded literals, zero table loads) — deletes the whole len-16 combine pass and
        // its memory round-trip. Verified: len-16 DIT at position j combines groups g with W16^{jm},
        // outputs to slot s + 4t + j — positions pair (0,1)/(2,3) = vector halves.
        {
#ifdef CRD_FFT_PROFILE
            const unsigned long long ip0 = prof::rdtsc();
#endif
            const crd::usize nq = n >> 2;
            // Inverse: only t.hi feeds mix_lo_hi ⇒ conjugating the len-4 stage costs ZERO ops —
            // flip sgn's high pair instead of negating t. Fold constants conjugate via ns.
            const V sgn = INV ? mkv(1.0, 1.0, -1.0, 1.0) : mkv(1.0, 1.0, 1.0, -1.0);
            // W16^m: forward (cos(mπ/8), −sin(mπ/8)); inverse = conjugate ⇒ every i-part × ns.
            constexpr double c1 = 0.9238795325112867; // cos(π/8) = sin(3π/8)
            constexpr double s1 = 0.3826834323650898; // sin(π/8) = cos(3π/8)
            constexpr double r2 = 0.7071067811865476; // √2/2
            const V w1r_a = mkv(1.0, 1.0, c1, c1);    // [W⁰|W¹]
            const V w1i_a = mkv(0.0, 0.0, -s1 * ns, -s1 * ns);
            const V w1r_b = mkv(r2, r2, s1, s1); // [W²|W³]
            const V w1i_b = mkv(-r2 * ns, -r2 * ns, -c1 * ns, -c1 * ns);
            const V w2r_a = mkv(1.0, 1.0, r2, r2); // [W⁰|W²]
            const V w2i_a = mkv(0.0, 0.0, -r2 * ns, -r2 * ns);
            const V w2r_b = mkv(0.0, 0.0, -r2, -r2); // [W⁴|W⁶]
            const V w2i_b = mkv(-ns, -ns, -r2 * ns, -r2 * ns);
            const V w3r_a = mkv(1.0, 1.0, s1, s1); // [W⁰|W³]
            const V w3i_a = mkv(0.0, 0.0, -c1 * ns, -c1 * ns);
            const V w3r_b = mkv(-r2, -r2, -c1, -c1); // [W⁶|W⁹]
            const V w3i_b = mkv(-r2 * ns, -r2 * ns, s1 * ns, s1 * ns);
            // COBRA quad-unit (round 7b): rev(s + v·nq) = rev(s) + v ⇒ the four tb-quads
            // {s, s+nq, s+2nq, s+3nq} together consume COMPLETE io cache lines (io[rev(s)+t·n/16
            // + 0..3], t = 0..15). Stage the 64 complex through a 1 KB L1 buffer: every io line is
            // fetched from L2 exactly ONCE (the scattered-unit form re-fetched each line 4×).
            const crd::usize n16 = n >> 4; // io stream stride — SAME expression for both parities
            // fold-unit: 16 complex from staged lbuf (group step gs, role-m step ms, lane base l)
            // through len-4 (P-trick) + len-16 (constant twiddles) into 16 sequential tb slots.
            // fold-unit: 16 complex per unit through len-4 (P-trick) + len-16 (constant twiddles)
            // into 16 sequential tb slots. f64: one unit per call (obB unused). f32: TWIN units per
            // call — low 128-half of every vector = unit A → obA, high = unit B → obB.
            const auto fold16 = [&](const T* lbase, crd::usize gs, crd::usize ms, Complex<T>* obA,
                                    Complex<T>* obB) noexcept
            {
                V y00; // y{group}{pair-half}
                V y01;
                V y10;
                V y11;
                V y20;
                V y21;
                V y30;
                V y31;
                for (crd::usize g = 0; g < 4; ++g)
                {
                    const T* const l0 = lbase + gs * g;
                    V ab;
                    V cd;
                    V u;
                    V w;
                    if constexpr (is_f32)
                    {
                        ab = simd::load_c_quad(l0, l0 + ms);
                        cd = simd::load_c_quad(l0 + 2 * ms, l0 + 3 * ms);
                    }
                    else
                    {
                        ab = simd::load_pair128(l0, l0 + ms);
                        cd = simd::load_pair128(l0 + 2 * ms, l0 + 3 * ms);
                    }
                    const V u0 = ab + cd; // [t0, t2] per unit
                    const V u1 = ab - cd; // [t1, t3]
                    if constexpr (is_f32)
                    {
                        u = simd::unpack_c_lo(u0, u1); // [t0, t1] per unit
                        w = simd::unpack_c_hi(u0, u1); // [t2, t3]
                    }
                    else
                    {
                        u = simd::concat_lo(u0, u1);
                        w = simd::concat_hi(u0, u1);
                    }
                    const V t = simd::swap_pairs(w) * sgn; // [t2i, t2r, t3i, ∓t3r]
                    V p;
                    if constexpr (is_f32)
                    {
                        p = simd::blend_c_odd(w, t); // [t2, (t3i, ∓t3r)]
                    }
                    else
                    {
                        p = simd::mix_lo_hi(w, t);
                    }
                    V& yh0 = pick_of_four(g, y00, y10, y20, y30);
                    V& yh1 = pick_of_four(g, y01, y11, y21, y31);
                    yh0 = u + p; // [X0, X1] of the len-4 block
                    yh1 = u - p; // [X2, X3]
                }
                bf4(y00, y10, y20, y30, w1r_a, w1i_a, w2r_a, w2i_a, w3r_a, w3i_a); // positions 0,1
                bf4(y01, y11, y21, y31, w1r_b, w1i_b, w2r_b, w2i_b, w3r_b, w3i_b); // positions 2,3
                if constexpr (is_f32)
                {
                    simd::store_c_lo(reinterpret_cast<T*>(obA), y00);
                    simd::store_c_hi(reinterpret_cast<T*>(obB), y00);
                    simd::store_c_lo(reinterpret_cast<T*>(obA + 2), y01);
                    simd::store_c_hi(reinterpret_cast<T*>(obB + 2), y01);
                    simd::store_c_lo(reinterpret_cast<T*>(obA + 4), y10);
                    simd::store_c_hi(reinterpret_cast<T*>(obB + 4), y10);
                    simd::store_c_lo(reinterpret_cast<T*>(obA + 6), y11);
                    simd::store_c_hi(reinterpret_cast<T*>(obB + 6), y11);
                    simd::store_c_lo(reinterpret_cast<T*>(obA + 8), y20);
                    simd::store_c_hi(reinterpret_cast<T*>(obB + 8), y20);
                    simd::store_c_lo(reinterpret_cast<T*>(obA + 10), y21);
                    simd::store_c_hi(reinterpret_cast<T*>(obB + 10), y21);
                    simd::store_c_lo(reinterpret_cast<T*>(obA + 12), y30);
                    simd::store_c_hi(reinterpret_cast<T*>(obB + 12), y30);
                    simd::store_c_lo(reinterpret_cast<T*>(obA + 14), y31);
                    simd::store_c_hi(reinterpret_cast<T*>(obB + 14), y31);
                }
                else
                {
                    (void)obB;
                    y00.store(reinterpret_cast<T*>(obA));
                    y01.store(reinterpret_cast<T*>(obA + 2));
                    y10.store(reinterpret_cast<T*>(obA + 4));
                    y11.store(reinterpret_cast<T*>(obA + 6));
                    y20.store(reinterpret_cast<T*>(obA + 8));
                    y21.store(reinterpret_cast<T*>(obA + 10));
                    y30.store(reinterpret_cast<T*>(obA + 12));
                    y31.store(reinterpret_cast<T*>(obA + 14));
                }
            };
            alignas(64) T lbuf[256];
            if (!ipodd)
            {
                for (crd::usize s = 0; s < nq; s += 16)
                {
                    const crd::usize j0 = m_ip_rev[s];
                    for (crd::usize t = 0; t < 16; ++t) // line-complete loads → L1 staging (8 T)
                    {
                        const T* const src = reinterpret_cast<const T*>(io + j0 + t * n16);
                        if constexpr (is_f32)
                        {
                            V::load(src).store(lbuf + 8 * t);
                        }
                        else
                        {
                            V::load(src).store(lbuf + 8 * t);
                            V::load(src + 4).store(lbuf + 8 * t + 4);
                        }
                    }
                    if constexpr (is_f32) // twin over v: units (v, v+1) share each vector
                    {
                        for (crd::usize v = 0; v < 4; v += 2)
                        {
                            fold16(lbuf + 2 * v, 8, 32, tb + ps(s + v * nq), tb + ps(s + (v + 1) * nq));
                        }
                    }
                    else
                    {
                        for (crd::usize v = 0; v < 4; ++v)
                        {
                            fold16(lbuf + 2 * v, 8, 32, tb + ps(s + v * nq), nullptr);
                        }
                    }
                }
            }
            else
            {
                // odd log2: each io line holds 2 even + 2 odd complex — co-process BOTH halves'
                // units per stream (8 complex staged per stream; lane (v,h) at offset 2·(2v+h)).
                const crd::usize nqh = nh >> 2;
                for (crd::usize s = 0; s < nqh; s += 16)
                {
                    const crd::usize j0 = m_ip_rev[s]; // = 2·rev(s)
                    for (crd::usize t = 0; t < 16; ++t) // 16 T per stream
                    {
                        const T* const src = reinterpret_cast<const T*>(io + j0 + t * n16);
                        if constexpr (is_f32)
                        {
                            V::load(src).store(lbuf + 16 * t);
                            V::load(src + 8).store(lbuf + 16 * t + 8);
                        }
                        else
                        {
                            V::load(src).store(lbuf + 16 * t);
                            V::load(src + 4).store(lbuf + 16 * t + 4);
                            V::load(src + 8).store(lbuf + 16 * t + 8);
                            V::load(src + 12).store(lbuf + 16 * t + 12);
                        }
                    }
                    if constexpr (is_f32) // twin over h: lanes (v,h=0)|(v,h=1) are adjacent (4v, 4v+2)
                    {
                        for (crd::usize v = 0; v < 4; ++v)
                        {
                            fold16(lbuf + 4 * v, 16, 64, tb + ps(s + v * nqh), tb + ps(s + v * nqh + nh));
                        }
                    }
                    else
                    {
                        for (crd::usize h = 0; h < 2; ++h)
                        {
                            for (crd::usize v = 0; v < 4; ++v)
                            {
                                fold16(lbuf + 2 * (2 * v + h), 16, 64, tb + ps(s + v * nqh + h * nh), nullptr);
                            }
                        }
                    }
                }
            }
#ifdef CRD_FFT_PROFILE
            prof::g_ip_gather += prof::rdtsc() - ip0;
#endif
        }
#ifdef CRD_FFT_PROFILE
        const unsigned long long ip1 = prof::rdtsc();
#endif
        const T* twr = m_ip_twr + 12; // skip the len-16 3-set table (3·q = 12 entries): folded above
        const T* twi = m_ip_twi + 12;
        // In-register twiddle powers on the dup'd form (elementwise, no shuffles):
        //   w2 = w1²: re = w1r²−w1i², im = 2·w1r·w1i;  w3 = w1·w2 (fnmadd/fma).
        const auto twpow = [](V w1r, V w1i, V& w2r, V& w2i, V& w3r, V& w3i) noexcept
        {
            w2r = simd::fnmadd(w1i, w1i, w1r * w1r);
            w2i = (w1r + w1r) * w1i;
            w3r = simd::fnmadd(w1i, w2i, w1r * w2r);
            w3i = simd::fma(w1i, w2r, w1r * w2i);
        };
        // twiddle-free radix-4 DIT core (the bf4 butterfly with w = 1):
        const auto bf4c = [](V& x0, V& x1, V& x2, V& x3) noexcept
        {
            const V t0 = x0 + x2;
            const V t1 = x0 - x2;
            const V t2 = x1 + x3;
            const V t3 = x1 - x3;
            const V ts = simd::swap_pairs(t3);
            x0 = t0 + t2;
            x2 = t0 - t2;
            if constexpr (INV)
            {
                x1 = simd::addsub(t1, ts);
                x3 = simd::addsub(t1, V::zero() - ts);
            }
            else
            {
                x1 = simd::addsub(t1, V::zero() - ts); // X1 = t1 - i·t3
                x3 = simd::addsub(t1, ts);             // X3 = t1 + i·t3
            }
        };
        // Combine passes in PLAN order (round 17): [8,8] then radix-4 to nh for nh ≥ 4096 —
        // one fewer full store sweep (the last counter with daylight vs MKL was Store Bound).
        const crd::u32 log2h = ipodd ? m_log2 - 1U : m_log2;
        const crd::u32 nr8 = 0U; // radix-8 measured slower (see ctor note); machinery retained
        const crd::u32 nstages = 2U + nr8 + (log2h - 4U - 3U * nr8) / 2U;
        constexpr double r2c = 0.7071067811865476;
        const V w8r2 = mkv(r2c, r2c, r2c, r2c);
        const V w8r2n = mkv(r2c, -r2c, r2c, -r2c);
        const V w8ni = mkv(1.0, -1.0, 1.0, -1.0);
        crd::usize len = 16;
        for (crd::u32 st = 2; st < nstages; ++st)
        {
            const crd::usize r = (st < 2U + nr8) ? 8U : 4U;
            len *= r;
            const crd::usize q = len / r;
            const bool tab3 = (r == 4U && len <= 1024); // 3-set r4 table (r8 has its own 7-set)
            // Last pass writes the caller's buffer directly — unless a radix-2 combine follows.
            Complex<T>* const dstbase = (len == nh && !ipodd) ? io : tb;
            if (r == 8U) // radix-8 pass: never the last (len ≤ 1024 < nh), in place, 7-set table
            {
                // BLOCK-PAIRS: two blocks per k-iteration share all seven twiddle loads and give
                // two independent chains (the same cure that fixed the radix-4 loop's ILP).
                const auto bf8 = [&](Complex<T>* const* p, crd::usize k, V w1r, V w1i, V w2r, V w2i,
                                     V w3r, V w3i, V w4r, V w4i, V w5r, V w5i, V w6r, V w6i, V w7r,
                                     V w7i) noexcept
                {
                    V x0 = V::load(reinterpret_cast<const T*>(p[0] + k));
                    V x1 = V::load(reinterpret_cast<const T*>(p[1] + k));
                    V x2 = V::load(reinterpret_cast<const T*>(p[2] + k));
                    V x3 = V::load(reinterpret_cast<const T*>(p[3] + k));
                    V x4 = V::load(reinterpret_cast<const T*>(p[4] + k));
                    V x5 = V::load(reinterpret_cast<const T*>(p[5] + k));
                    V x6 = V::load(reinterpret_cast<const T*>(p[6] + k));
                    V x7 = V::load(reinterpret_cast<const T*>(p[7] + k));
                    x1 = simd::fmaddsub(x1, w1r, simd::swap_pairs(x1) * w1i);
                    x2 = simd::fmaddsub(x2, w2r, simd::swap_pairs(x2) * w2i);
                    x3 = simd::fmaddsub(x3, w3r, simd::swap_pairs(x3) * w3i);
                    x4 = simd::fmaddsub(x4, w4r, simd::swap_pairs(x4) * w4i);
                    x5 = simd::fmaddsub(x5, w5r, simd::swap_pairs(x5) * w5i);
                    x6 = simd::fmaddsub(x6, w6r, simd::swap_pairs(x6) * w6i);
                    x7 = simd::fmaddsub(x7, w7r, simd::swap_pairs(x7) * w7i);
                    bf4c(x0, x2, x4, x6); // E = DFT4(even) → x0,x2,x4,x6
                    bf4c(x1, x3, x5, x7); // O = DFT4(odd)  → x1,x3,x5,x7
                    if constexpr (INV) // conjugated W8 diagonal
                    {
                        x3 = simd::addsub(x3, simd::swap_pairs(x3)) * w8r2; // (1+i)/√2 · O1
                        x5 = simd::swap_pairs(x5) * (V::zero() - w8ni);     // +i · O2
                        const V q3 = simd::addsub(simd::swap_pairs(x7), x7);
                        x7 = simd::swap_pairs(q3) * (V::zero() - w8r2);     // −(1−i)/√2 · O3
                    }
                    else
                    {
                        const V s1 = simd::swap_pairs(x3);
                        x3 = simd::swap_pairs(simd::addsub(s1, x3)) * w8r2; // (1−i)/√2 · O1
                        x5 = simd::swap_pairs(x5) * w8ni;                   // −i · O2
                        const V s3 = simd::swap_pairs(x7);
                        x7 = simd::addsub(s3, x7) * w8r2n; // −(1+i)/√2 · O3
                    }
                    (x0 + x1).store(reinterpret_cast<T*>(p[0] + k));
                    (x2 + x3).store(reinterpret_cast<T*>(p[1] + k));
                    (x4 + x5).store(reinterpret_cast<T*>(p[2] + k));
                    (x6 + x7).store(reinterpret_cast<T*>(p[3] + k));
                    (x0 - x1).store(reinterpret_cast<T*>(p[4] + k));
                    (x2 - x3).store(reinterpret_cast<T*>(p[5] + k));
                    (x4 - x5).store(reinterpret_cast<T*>(p[6] + k));
                    (x6 - x7).store(reinterpret_cast<T*>(p[7] + k));
                };
                for (crd::usize base = 0; base < n; base += 2 * len)
                {
                    Complex<T>* pa[8];
                    Complex<T>* pb[8];
                    for (crd::usize m = 0; m < 8; ++m)
                    {
                        pa[m] = tb + ps(base + m * q);
                        pb[m] = tb + ps(base + len + m * q);
                    }
                    for (crd::usize k = 0; k < q; k += per_vec)
                    {
                        const V w1r = simd::load_dup_pairs(twr + k);
                        const V w2r = simd::load_dup_pairs(twr + q + k);
                        const V w3r = simd::load_dup_pairs(twr + 2 * q + k);
                        const V w4r = simd::load_dup_pairs(twr + 3 * q + k);
                        const V w5r = simd::load_dup_pairs(twr + 4 * q + k);
                        const V w6r = simd::load_dup_pairs(twr + 5 * q + k);
                        const V w7r = simd::load_dup_pairs(twr + 6 * q + k);
                        V w1i = simd::load_dup_pairs(twi + k);
                        V w2i = simd::load_dup_pairs(twi + q + k);
                        V w3i = simd::load_dup_pairs(twi + 2 * q + k);
                        V w4i = simd::load_dup_pairs(twi + 3 * q + k);
                        V w5i = simd::load_dup_pairs(twi + 4 * q + k);
                        V w6i = simd::load_dup_pairs(twi + 5 * q + k);
                        V w7i = simd::load_dup_pairs(twi + 6 * q + k);
                        if constexpr (INV)
                        {
                            w1i = V::zero() - w1i;
                            w2i = V::zero() - w2i;
                            w3i = V::zero() - w3i;
                            w4i = V::zero() - w4i;
                            w5i = V::zero() - w5i;
                            w6i = V::zero() - w6i;
                            w7i = V::zero() - w7i;
                        }
                        bf8(pa, k, w1r, w1i, w2r, w2i, w3r, w3i, w4r, w4i, w5r, w5i, w6r, w6i, w7r, w7i);
                        bf8(pb, k, w1r, w1i, w2r, w2i, w3r, w3i, w4r, w4i, w5r, w5i, w6r, w6i, w7r, w7i);
                    }
                }
                twr += 7 * q;
                twi += 7 * q;
                continue;
            }
            // k-inner, row order (block-inner MEASURED WORSE at every stride: each k-sweep re-walks
            // the array through L1 at 32B/line utilization ⇒ L1→L2 traffic × q/2. MKL's stride-reg
            // stores are just quarter addressing inside a k-inner loop — same order as this).
            // Lever (b): BLOCK-PAIRS in k-inner order — two butterflies per iteration share ONE
            // twiddle load (halves twiddle traffic) and give two independent dependency chains,
            // while both blocks stream forward in k (prefetch-friendly, unlike block-inner).
            if (len < nh) // block-pairs (blocks never straddle the half boundary: 2·len ≤ nh)
            {
                for (crd::usize base = 0; base < n; base += 2 * len)
                {
                    for (crd::usize kc = 0; kc < q; kc += 256) // pad-chunked (runs stay in-block)
                    {
                        const crd::usize ke = (q - kc < 256) ? q - kc : 256;
                        const T* const t1r = twr + kc;
                        const T* const t1i = twi + kc;
                        Complex<T>* const p0 = tb + ps(base + kc);
                        Complex<T>* const p1 = tb + ps(base + q + kc);
                        Complex<T>* const p2 = tb + ps(base + 2 * q + kc);
                        Complex<T>* const p3 = tb + ps(base + 3 * q + kc);
                        Complex<T>* const p4 = tb + ps(base + len + kc);
                        Complex<T>* const p5 = tb + ps(base + len + q + kc);
                        Complex<T>* const p6 = tb + ps(base + len + 2 * q + kc);
                        Complex<T>* const p7 = tb + ps(base + len + 3 * q + kc);
                        const T* const t2r = t1r + q; // valid when tab3
                        const T* const t2i = t1i + q;
                        const T* const t3r = t1r + 2 * q;
                        const T* const t3i = t1i + 2 * q;
                        for (crd::usize k = 0; k < ke; k += 2 * per_vec) // k-unroll×2 × block-pair: 4 chains
                        {
                            V w1r = simd::load_dup_pairs(t1r + k);
                            V w1i = simd::load_dup_pairs(t1i + k);
                            V u1r = simd::load_dup_pairs(t1r + k + per_vec);
                            V u1i = simd::load_dup_pairs(t1i + k + per_vec);
                            if constexpr (INV) // conjugate at load (measured free — port headroom)
                            {
                                w1i = V::zero() - w1i;
                                u1i = V::zero() - u1i;
                            }
                            V w2r;
                            V w2i;
                            V w3r;
                            V w3i;
                            V u2r;
                            V u2i;
                            V u3r;
                            V u3i;
                            if (tab3) // small pass: full table (branch is pass-invariant, predicted)
                            {
                                w2r = simd::load_dup_pairs(t2r + k);
                                w2i = simd::load_dup_pairs(t2i + k);
                                w3r = simd::load_dup_pairs(t3r + k);
                                w3i = simd::load_dup_pairs(t3i + k);
                                u2r = simd::load_dup_pairs(t2r + k + per_vec);
                                u2i = simd::load_dup_pairs(t2i + k + per_vec);
                                u3r = simd::load_dup_pairs(t3r + k + per_vec);
                                u3i = simd::load_dup_pairs(t3i + k + per_vec);
                                if constexpr (INV)
                                {
                                    w2i = V::zero() - w2i;
                                    w3i = V::zero() - w3i;
                                    u2i = V::zero() - u2i;
                                    u3i = V::zero() - u3i;
                                }
                            }
                            else // conjugated w1 propagates: w2 = w1², w3 = w1·w2 conjugate along
                            {
                                twpow(w1r, w1i, w2r, w2i, w3r, w3i);
                                twpow(u1r, u1i, u2r, u2i, u3r, u3i);
                            }
                            V a0 = V::load(reinterpret_cast<const T*>(p0 + k));
                            V b0 = V::load(reinterpret_cast<const T*>(p1 + k));
                            V c0 = V::load(reinterpret_cast<const T*>(p2 + k));
                            V d0 = V::load(reinterpret_cast<const T*>(p3 + k));
                            V a1 = V::load(reinterpret_cast<const T*>(p4 + k));
                            V b1 = V::load(reinterpret_cast<const T*>(p5 + k));
                            V c1 = V::load(reinterpret_cast<const T*>(p6 + k));
                            V d1 = V::load(reinterpret_cast<const T*>(p7 + k));
                            V a2 = V::load(reinterpret_cast<const T*>(p0 + k + per_vec));
                            V b2 = V::load(reinterpret_cast<const T*>(p1 + k + per_vec));
                            V c2 = V::load(reinterpret_cast<const T*>(p2 + k + per_vec));
                            V d2 = V::load(reinterpret_cast<const T*>(p3 + k + per_vec));
                            V a3 = V::load(reinterpret_cast<const T*>(p4 + k + per_vec));
                            V b3 = V::load(reinterpret_cast<const T*>(p5 + k + per_vec));
                            V c3 = V::load(reinterpret_cast<const T*>(p6 + k + per_vec));
                            V d3 = V::load(reinterpret_cast<const T*>(p7 + k + per_vec));
                            bf4(a0, b0, c0, d0, w1r, w1i, w2r, w2i, w3r, w3i);
                            bf4(a1, b1, c1, d1, w1r, w1i, w2r, w2i, w3r, w3i);
                            bf4(a2, b2, c2, d2, u1r, u1i, u2r, u2i, u3r, u3i);
                            bf4(a3, b3, c3, d3, u1r, u1i, u2r, u2i, u3r, u3i);
                            a0.store(reinterpret_cast<T*>(p0 + k));
                            b0.store(reinterpret_cast<T*>(p1 + k));
                            c0.store(reinterpret_cast<T*>(p2 + k));
                            d0.store(reinterpret_cast<T*>(p3 + k));
                            a1.store(reinterpret_cast<T*>(p4 + k));
                            b1.store(reinterpret_cast<T*>(p5 + k));
                            c1.store(reinterpret_cast<T*>(p6 + k));
                            d1.store(reinterpret_cast<T*>(p7 + k));
                            a2.store(reinterpret_cast<T*>(p0 + k + per_vec));
                            b2.store(reinterpret_cast<T*>(p1 + k + per_vec));
                            c2.store(reinterpret_cast<T*>(p2 + k + per_vec));
                            d2.store(reinterpret_cast<T*>(p3 + k + per_vec));
                            a3.store(reinterpret_cast<T*>(p4 + k + per_vec));
                            b3.store(reinterpret_cast<T*>(p5 + k + per_vec));
                            c3.store(reinterpret_cast<T*>(p6 + k + per_vec));
                            d3.store(reinterpret_cast<T*>(p7 + k + per_vec));
                        }
                    }
                }
            }
            else // the single-block final combine pass, per half
            {
                for (crd::usize hh = 0; hh < (ipodd ? 2U : 1U); ++hh)
                {
                    for (crd::usize kc = 0; kc < q; kc += 256)
                    {
                        const crd::usize ke = (q - kc < 256) ? q - kc : 256;
                        const T* const t1r = twr + kc;
                        const T* const t1i = twi + kc;
                        const crd::usize hb = hh * nh;
                        const Complex<T>* const p0 = tb + ps(hb + kc);
                        const Complex<T>* const p1 = tb + ps(hb + q + kc);
                        const Complex<T>* const p2 = tb + ps(hb + 2 * q + kc);
                        const Complex<T>* const p3 = tb + ps(hb + 3 * q + kc);
                        const bool topad = (dstbase != io); // tb stores go through the slot map
                        Complex<T>* const o0 = topad ? tb + ps(hb + kc) : io + hb + kc;
                        Complex<T>* const o1 = topad ? tb + ps(hb + q + kc) : io + hb + q + kc;
                        Complex<T>* const o2 = topad ? tb + ps(hb + 2 * q + kc) : io + hb + 2 * q + kc;
                        Complex<T>* const o3 = topad ? tb + ps(hb + 3 * q + kc) : io + hb + 3 * q + kc;
                        for (crd::usize k = 0; k < ke; k += 2 * per_vec)
                        {
                            V a0 = V::load(reinterpret_cast<const T*>(p0 + k));
                            V b0 = V::load(reinterpret_cast<const T*>(p1 + k));
                            V c0 = V::load(reinterpret_cast<const T*>(p2 + k));
                            V d0 = V::load(reinterpret_cast<const T*>(p3 + k));
                            V a1 = V::load(reinterpret_cast<const T*>(p0 + k + per_vec));
                            V b1 = V::load(reinterpret_cast<const T*>(p1 + k + per_vec));
                            V c1 = V::load(reinterpret_cast<const T*>(p2 + k + per_vec));
                            V d1 = V::load(reinterpret_cast<const T*>(p3 + k + per_vec));
                            V w1r = simd::load_dup_pairs(t1r + k);
                            V w1i = simd::load_dup_pairs(t1i + k);
                            V u1r = simd::load_dup_pairs(t1r + k + per_vec);
                            V u1i = simd::load_dup_pairs(t1i + k + per_vec);
                            if constexpr (INV)
                            {
                                w1i = V::zero() - w1i;
                                u1i = V::zero() - u1i;
                            }
                            V w2r;
                            V w2i;
                            V w3r;
                            V w3i;
                            V u2r;
                            V u2i;
                            V u3r;
                            V u3i;
                            if (tab3)
                            {
                                w2r = simd::load_dup_pairs(t1r + q + k);
                                w2i = simd::load_dup_pairs(t1i + q + k);
                                w3r = simd::load_dup_pairs(t1r + 2 * q + k);
                                w3i = simd::load_dup_pairs(t1i + 2 * q + k);
                                u2r = simd::load_dup_pairs(t1r + q + k + per_vec);
                                u2i = simd::load_dup_pairs(t1i + q + k + per_vec);
                                u3r = simd::load_dup_pairs(t1r + 2 * q + k + per_vec);
                                u3i = simd::load_dup_pairs(t1i + 2 * q + k + per_vec);
                                if constexpr (INV)
                                {
                                    w2i = V::zero() - w2i;
                                    w3i = V::zero() - w3i;
                                    u2i = V::zero() - u2i;
                                    u3i = V::zero() - u3i;
                                }
                            }
                            else
                            {
                                twpow(w1r, w1i, w2r, w2i, w3r, w3i);
                                twpow(u1r, u1i, u2r, u2i, u3r, u3i);
                            }
                            bf4(a0, b0, c0, d0, w1r, w1i, w2r, w2i, w3r, w3i);
                            bf4(a1, b1, c1, d1, u1r, u1i, u2r, u2i, u3r, u3i);
                            a0.store(reinterpret_cast<T*>(o0 + k));
                            b0.store(reinterpret_cast<T*>(o1 + k));
                            c0.store(reinterpret_cast<T*>(o2 + k));
                            d0.store(reinterpret_cast<T*>(o3 + k));
                            a1.store(reinterpret_cast<T*>(o0 + k + per_vec));
                            b1.store(reinterpret_cast<T*>(o1 + k + per_vec));
                            c1.store(reinterpret_cast<T*>(o2 + k + per_vec));
                            d1.store(reinterpret_cast<T*>(o3 + k + per_vec));
                        }
                    }
                }
            }
            twr += tab3 ? 3 * q : q;
            twi += tab3 ? 3 * q : q;
        }
        if (ipodd) // final radix-2 combine: X[j] = E[j] + W_n^j·O[j]; X[j+nh] = E[j] − W_n^j·O[j]
        {
            for (crd::usize j = 0; j < nh; j += 2 * per_vec) // 2 vectors per iteration (two chains)
            {
                const V e0 = V::load(reinterpret_cast<const T*>(tb + ps(j)));
                const V o0 = V::load(reinterpret_cast<const T*>(tb + ps(nh + j)));
                const V e1 = V::load(reinterpret_cast<const T*>(tb + ps(j + per_vec)));
                const V o1 = V::load(reinterpret_cast<const T*>(tb + ps(nh + j + per_vec)));
                const V w0r = simd::load_dup_pairs(twr + j);
                const V w1r = simd::load_dup_pairs(twr + j + per_vec);
                V w0i = simd::load_dup_pairs(twi + j);
                V w1i = simd::load_dup_pairs(twi + j + per_vec);
                if constexpr (INV)
                {
                    w0i = V::zero() - w0i;
                    w1i = V::zero() - w1i;
                }
                const V ow0 = simd::fmaddsub(o0, w0r, simd::swap_pairs(o0) * w0i);
                const V ow1 = simd::fmaddsub(o1, w1r, simd::swap_pairs(o1) * w1i);
                (e0 + ow0).store(reinterpret_cast<T*>(io + j));
                (e1 + ow1).store(reinterpret_cast<T*>(io + j + per_vec));
                (e0 - ow0).store(reinterpret_cast<T*>(io + nh + j));
                (e1 - ow1).store(reinterpret_cast<T*>(io + nh + j + per_vec));
            }
        }
#ifdef CRD_FFT_PROFILE
        prof::g_ip_pass += prof::rdtsc() - ip1;
        ++prof::g_ip_calls;
#endif
    }
#endif


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
            // 1024 GATHER FUSION (banked default-on). f32 1M ~1.085× / f64 1M ~1.086×
            // vs the phase-separated path: the gather-fused stage-1 reads din DIRECTLY (rs=n2) — no gather memcpy, no
            // scratch round-trip (A moves 32 MB, this 16 MB) — then stage-2 → scratch. BIT-equivalent (verified
            // m13_equiv). M15: 2048/4096 gather + per-tile/split orchestrators all rejected (regress) — see docs.
            bool m13_fused = false;
            if constexpr (std::is_same_v<T, crd::f32>)
            {
                if (dir == FftDirection::Forward && n1 == 1024 && bw == 128 && m_p1->m_hier_bbuf != nullptr)
                {
#ifdef CRD_FFT_PROFILE
                    const unsigned long long fp0 = prof::rdtsc();
#endif
                    gen::codelet32_stage1_fused_32x32_gather(din + i2, m_p1->m_hier_bbuf, n2, m_p1->m_hier_twr,
                                                             m_p1->m_hier_twi);
#ifdef CRD_FFT_PROFILE
                    const unsigned long long fp1 = prof::rdtsc();
                    prof::g_p1_gather += fp1 - fp0; // M14: fused stage-1 (gather + leaf + inner twiddle)
#endif
                    gen::codelet32_batched(m_p1->m_hier_bbuf, scratch, 32 * bw);
#ifdef CRD_FFT_PROFILE
                    prof::g_p1_sub += prof::rdtsc() - fp1; // M14: stage-2
#endif
                    m13_fused = true;
                }
            }
            else if constexpr (std::is_same_v<T, crd::f64>) // M13 f64 1M port (bw=block_width(1024)=64 for f64)
            {
                if (dir == FftDirection::Forward && n1 == 1024 && bw == 64 && m_p1->m_hier_bbuf != nullptr)
                {
                    gen::codelet32_stage1_fused_32x32_gather(din + i2, m_p1->m_hier_bbuf, n2, m_p1->m_hier_twr,
                                                             m_p1->m_hier_twi);
                    gen::codelet32_batched(m_p1->m_hier_bbuf, scratch, 32 * bw);
                    m13_fused = true;
                }
            }
            if (!m13_fused)
            {
#ifdef CRD_FFT_PROFILE
                const unsigned long long pg0 = prof::rdtsc();
#endif
                for (crd::usize i1 = 0; i1 < n1; ++i1) // gather B columns (each a contiguous bw-run) → element-major
                {
                    if (i1 + kGatherPf < n1) // prefetch the strided next row (hint-only ⇒ bit-identical; latency hide)
                    {
                        _mm_prefetch(reinterpret_cast<const char*>(din + (i1 + kGatherPf) * n2 + i2), _MM_HINT_T0);
                    }
                    std::memcpy(scratch + i1 * bw, din + i1 * n2 + i2, bw * sizeof(Complex<T>));
                }
#ifdef CRD_FFT_PROFILE
                const unsigned long long pg1 = prof::rdtsc();
                prof::g_p1_gather += pg1 - pg0;
#endif
                m_p1->execute_batched(cont::Span<Complex<T>>(scratch, n1 * bw), bw, dir);
#ifdef CRD_FFT_PROFILE
                prof::g_p1_sub += prof::rdtsc() - pg1;
#endif
            }
            // twiddle scratch[k1·bw+bb] *= W_n^{k1·col}; NT-store to tbuf row. The twiddle is FACTORED from two
            // ~√n L2-resident tables (no 128 MB streaming read): W_n^a = W_n^{a_hi·M}·W_n^{a_lo}, a = col·k1 < n.
            const crd::usize mmask = (crd::usize{1} << m_ftw_h) - 1;
            const T* const hir = m_ftw_hi_re.data();
            const T* const hii = m_ftw_hi_im.data();
            const T* const lor = m_ftw_lo_re.data();
            const T* const loi = m_ftw_lo_im.data();
#ifdef CRD_FFT_PROFILE
            const unsigned long long pt0 = prof::rdtsc();
#endif
#if CRD_SIMD_HAS_AVX2
            // f32 SIMD twiddle (Vec8f over the bb axis = contiguous scratch read; transposed tbuf write hits
            // 8-row L2-resident bands). Twiddle by recurrence (w*=W_n^col), reseeded from the factored table every
            // K=8 to bound f32 drift (~1.5e-7, in class). Measured 2.15× on the f32 twiddle phase. f64 stays scalar.
            if constexpr (std::is_same_v<T, crd::f32>)
            {
                constexpr crd::usize period = 8;
                const crd::f32 fis = static_cast<crd::f32>(isign);
                const __m256i didx = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
                crd::usize g = 0;
                for (; g + 8 <= bw; g += 8)
                {
                    alignas(32) crd::f32 wsr[8];
                    alignas(32) crd::f32 wsi[8];
                    for (int l = 0; l < 8; ++l)
                    {
                        const crd::usize c = i2 + g + (crd::usize)l;
                        const crd::usize hh = c >> m_ftw_h;
                        const crd::usize ll = c & mmask;
                        wsr[l] = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                        wsi[l] = fis * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                    }
                    const __m256 wsrv = _mm256_load_ps(wsr);
                    const __m256 wsiv = _mm256_load_ps(wsi);
                    __m256 wr = _mm256_setzero_ps();
                    __m256 wi = _mm256_setzero_ps();
                    for (crd::usize k1 = 0; k1 < n1; ++k1)
                    {
                        if ((k1 & (period - 1)) == 0) // reseed from the exact factored table (bounds recurrence drift)
                        {
                            alignas(32) crd::f32 sr[8];
                            alignas(32) crd::f32 si[8];
                            for (int l = 0; l < 8; ++l)
                            {
                                const crd::usize a = (i2 + g + (crd::usize)l) * k1;
                                const crd::usize hh = a >> m_ftw_h;
                                const crd::usize ll = a & mmask;
                                sr[l] = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                                si[l] = fis * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                            }
                            wr = _mm256_load_ps(sr);
                            wi = _mm256_load_ps(si);
                        }
                        const crd::f32* const zp = reinterpret_cast<const crd::f32*>(scratch + k1 * bw + g);
                        const __m256 a0 = _mm256_loadu_ps(zp);
                        const __m256 b0 = _mm256_loadu_ps(zp + 8);
                        const __m256 ev = _mm256_shuffle_ps(a0, b0, _MM_SHUFFLE(2, 0, 2, 0));
                        const __m256 od = _mm256_shuffle_ps(a0, b0, _MM_SHUFFLE(3, 1, 3, 1));
                        const __m256 zr = _mm256_permutevar8x32_ps(ev, didx);
                        const __m256 zi = _mm256_permutevar8x32_ps(od, didx);
                        const __m256 outr = _mm256_sub_ps(_mm256_mul_ps(zr, wr), _mm256_mul_ps(zi, wi));
                        const __m256 outi = _mm256_add_ps(_mm256_mul_ps(zr, wi), _mm256_mul_ps(zi, wr));
                        alignas(32) crd::f32 orr[8];
                        alignas(32) crd::f32 oii[8];
                        _mm256_store_ps(orr, outr);
                        _mm256_store_ps(oii, outi);
                        for (int l = 0; l < 8; ++l)
                        {
                            (tbuf + (i2 + g + (crd::usize)l) * n1)[k1] = Complex<T>{orr[l], oii[l]};
                        }
                        if ((k1 & (period - 1)) != period - 1) // advance recurrence: w *= W_n^col
                        {
                            const __m256 nr = _mm256_sub_ps(_mm256_mul_ps(wr, wsrv), _mm256_mul_ps(wi, wsiv));
                            const __m256 ni = _mm256_add_ps(_mm256_mul_ps(wr, wsiv), _mm256_mul_ps(wi, wsrv));
                            wr = nr;
                            wi = ni;
                        }
                    }
                }
                for (; g < bw; ++g) // bw-tail (bw multiple of 8 at 8M, but keep correct)
                {
                    const crd::usize col = i2 + g;
                    Complex<T>* const trow = tbuf + col * n1;
                    for (crd::usize k1 = 0; k1 < n1; ++k1)
                    {
                        const crd::usize a = col * k1;
                        const crd::usize hh = a >> m_ftw_h;
                        const crd::usize ll = a & mmask;
                        const T wr = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                        const T wi = isign * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                        const Complex<T> z = scratch[k1 * bw + g];
                        store_complex(trow + k1, z.re * wr - z.im * wi, z.re * wi + z.im * wr, true);
                    }
                }
            }
            else if constexpr (std::is_same_v<T, crd::f64>)
            {
                // f64 bb-axis Vec4d twiddle (4 cols/lane), recurrence reseeded every K=32 (f64 drift ~1e-15). NT
                // per-lane store (matches the f64 NT path); transposed tbuf write hits L2-resident 4-row bands.
                // Measured +5.6% f64 8M (twiddle ~1.5×), checksum bit-identical — the f64 mirror of the f32 win.
                constexpr crd::usize period = 32;
                const double fis = static_cast<double>(isign);
                crd::usize g = 0;
                for (; g + 4 <= bw; g += 4)
                {
                    alignas(32) double wsr[4];
                    alignas(32) double wsi[4];
                    for (int l = 0; l < 4; ++l)
                    {
                        const crd::usize c = i2 + g + (crd::usize)l;
                        const crd::usize hh = c >> m_ftw_h;
                        const crd::usize ll = c & mmask;
                        wsr[l] = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                        wsi[l] = fis * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                    }
                    const __m256d wsrv = _mm256_load_pd(wsr);
                    const __m256d wsiv = _mm256_load_pd(wsi);
                    __m256d wr = _mm256_setzero_pd();
                    __m256d wi = _mm256_setzero_pd();
                    for (crd::usize k1 = 0; k1 < n1; ++k1)
                    {
                        if ((k1 & (period - 1)) == 0)
                        {
                            alignas(32) double sr[4];
                            alignas(32) double si[4];
                            for (int l = 0; l < 4; ++l)
                            {
                                const crd::usize a = (i2 + g + (crd::usize)l) * k1;
                                const crd::usize hh = a >> m_ftw_h;
                                const crd::usize ll = a & mmask;
                                sr[l] = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                                si[l] = fis * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                            }
                            wr = _mm256_load_pd(sr);
                            wi = _mm256_load_pd(si);
                        }
                        const double* const zp = reinterpret_cast<const double*>(scratch + k1 * bw + g);
                        const __m256d a0 = _mm256_loadu_pd(zp);
                        const __m256d b0 = _mm256_loadu_pd(zp + 4);
                        const __m256d zr = _mm256_permute4x64_pd(_mm256_unpacklo_pd(a0, b0), _MM_SHUFFLE(3, 1, 2, 0));
                        const __m256d zi = _mm256_permute4x64_pd(_mm256_unpackhi_pd(a0, b0), _MM_SHUFFLE(3, 1, 2, 0));
                        const __m256d outr = _mm256_sub_pd(_mm256_mul_pd(zr, wr), _mm256_mul_pd(zi, wi));
                        const __m256d outi = _mm256_add_pd(_mm256_mul_pd(zr, wi), _mm256_mul_pd(zi, wr));
                        alignas(32) double orr[4];
                        alignas(32) double oii[4];
                        _mm256_store_pd(orr, outr);
                        _mm256_store_pd(oii, outi);
                        for (int l = 0; l < 4; ++l)
                        {
                            _mm_stream_pd(reinterpret_cast<double*>((tbuf + (i2 + g + (crd::usize)l) * n1) + k1), _mm_set_pd(oii[l], orr[l]));
                        }
                        if ((k1 & (period - 1)) != period - 1)
                        {
                            const __m256d nr = _mm256_sub_pd(_mm256_mul_pd(wr, wsrv), _mm256_mul_pd(wi, wsiv));
                            const __m256d ni = _mm256_add_pd(_mm256_mul_pd(wr, wsiv), _mm256_mul_pd(wi, wsrv));
                            wr = nr;
                            wi = ni;
                        }
                    }
                }
                for (; g < bw; ++g)
                {
                    const crd::usize col = i2 + g;
                    Complex<T>* const trow = tbuf + col * n1;
                    for (crd::usize k1 = 0; k1 < n1; ++k1)
                    {
                        const crd::usize a = col * k1;
                        const crd::usize hh = a >> m_ftw_h;
                        const crd::usize ll = a & mmask;
                        const T wr = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                        const T wi = isign * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                        const Complex<T> z = scratch[k1 * bw + g];
                        store_complex(trow + k1, z.re * wr - z.im * wi, z.re * wi + z.im * wr, true);
                    }
                }
            }
            else
#endif
            {
              {
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
            } // close Lane A1 block
#ifdef CRD_FFT_PROFILE
            prof::g_p1_tw += prof::rdtsc() - pt0;
#endif
        }

        // ---- PHASE 2: row FFTs (length n2), NT-scatter to data (natural order) ----
        // NT-store to `data` only if the caller's buffer is ≥16B aligned (Complex<f64> array is only 8B-aligned
        // by alignof); else a plain store. Checked ONCE; all per-element offsets preserve the base alignment.
        const bool nt_out = (reinterpret_cast<std::uintptr_t>(din) % 16U) == 0U;
        for (crd::usize k1 = 0; k1 < n1; k1 += b2)
        {
            const crd::usize bw = (k1 + b2 < n1) ? b2 : (n1 - k1);
            bool m17_fused = false; // M17: the P2 stage-2 wrote final output directly ⇒ skip the scatter pass
#ifdef CRD_FFT_PROFILE
            unsigned long long qg2 = prof::rdtsc(); // M14: loop-scope so the scatter timer survives the fused branch
#endif
            // 1024 GATHER FUSION P2 (banked default-on): symmetric to P1 — the
            // gather-fused stage-1 reads tbuf DIRECTLY (rs=n1), no gather memcpy / scratch round-trip, then stage-2 →
            // scratch. Same codelet (source=tbuf+k1). BIT-equivalent; f32/f64 1M win composes with P1.
            bool m13_fused2 = false;
            if constexpr (std::is_same_v<T, crd::f32>)
            {
                if (dir == FftDirection::Forward && n2 == 1024 && bw == 128 && m_p2->m_hier_bbuf != nullptr)
                {
                    gen::codelet32_stage1_fused_32x32_gather(tbuf + k1, m_p2->m_hier_bbuf, n1, m_p2->m_hier_twr,
                                                             m_p2->m_hier_twi);
                    gen::codelet32_batched(m_p2->m_hier_bbuf, scratch, 32 * bw);
                    m13_fused2 = true;
                }
                // 256K small-N fix (f32): 256-axis P2 = 16×16 hier, GATHER-fused (reads tbuf directly, rs=n1, no
                // memcpy round-trip) + SCATTER-fused (stage-2 writes the final output directly, no scratch pass).
                // Default-on for the 1024×256 four-step split → 256K ~0.85× MKL (3.6× CRD over direct Stockham).
                if (dir == FftDirection::Forward && n2 == 256 && bw == 512 && m_p2->m_hier_bbuf != nullptr)
                {
                    gen::codelet16_stage1_fused_16x16_gather(tbuf + k1, m_p2->m_hier_bbuf, n1, m_p2->m_hier_twr,
                                                             m_p2->m_hier_twi);
                    gen::codelet16_batched_scatter(m_p2->m_hier_bbuf, din, k1, n1, 16 * bw);
                    m17_fused = true;
                    m13_fused2 = true;
                }
            }
            else if constexpr (std::is_same_v<T, crd::f64>) // M13 f64 1M port (bw=block_width(1024)=64 for f64)
            {
                if (dir == FftDirection::Forward && n2 == 1024 && bw == 64 && m_p2->m_hier_bbuf != nullptr)
                {
                    gen::codelet32_stage1_fused_32x32_gather(tbuf + k1, m_p2->m_hier_bbuf, n1, m_p2->m_hier_twr,
                                                             m_p2->m_hier_twi);
                    gen::codelet32_batched(m_p2->m_hier_bbuf, scratch, 32 * bw);
                    m13_fused2 = true;
                }
            }
            if (!m13_fused2)
            {
#ifdef CRD_FFT_PROFILE
                const unsigned long long qg0 = prof::rdtsc();
#endif
                for (crd::usize i2 = 0; i2 < n2; ++i2) // gather B rows (each a contiguous bw-run from tbuf)
                {
                    if (i2 + kGatherPf < n2) // prefetch the strided next row (hint-only ⇒ bit-identical; latency hide)
                    {
                        _mm_prefetch(reinterpret_cast<const char*>(tbuf + (i2 + kGatherPf) * n1 + k1), _MM_HINT_T0);
                    }
                    std::memcpy(scratch + i2 * bw, tbuf + i2 * n1 + k1, bw * sizeof(Complex<T>));
                }
#ifdef CRD_FFT_PROFILE
                const unsigned long long qg1 = prof::rdtsc();
                prof::g_p2_gather += qg1 - qg0;
#endif
                m_p2->execute_batched(cont::Span<Complex<T>>(scratch, n2 * bw), bw, dir);
#ifdef CRD_FFT_PROFILE
                qg2 = prof::rdtsc();
                prof::g_p2_sub += qg2 - qg1;
#endif
            }
            for (crd::usize k2 = 0; !m17_fused && k2 < n2; ++k2) // X[(k1+bb)+n1·k2] = scratch[k2·bw+bb] → data, NT-store
            {
                const Complex<T>* const srow = scratch + k2 * bw;
                Complex<T>* const drow = din + k2 * n1 + k1;
                crd::usize bb = 0;
#if CRD_SIMD_HAS_AVX2
                // f32: 1 complex = 8B < 16B NT minimum ⇒ pair two complex into one 16B NT store (drow 16B-aligned:
                // k2·n1+k1 even, bb steps 2). Gives the f32 scatter the RFO-skipping store f64 already had — the
                // store-bound f32 scatter phase HALVES (measured 18.0→10.8 Mcyc @8M). Found by f32 phase archaeology.
                if constexpr (std::is_same_v<T, crd::f32>)
                {
                    if (nt_out)
                    {
                        for (; bb + 2 <= bw; bb += 2)
                        {
                            const __m128 v = _mm_set_ps(srow[bb + 1].im, srow[bb + 1].re, srow[bb].im, srow[bb].re);
                            _mm_stream_pd(reinterpret_cast<double*>(drow + bb), _mm_castps_pd(v));
                        }
                    }
                }
#endif
                for (; bb < bw; ++bb)
                {
                    store_complex(drow + bb, srow[bb].re, srow[bb].im, nt_out);
                }
            }
#ifdef CRD_FFT_PROFILE
            prof::g_p2_scatter += prof::rdtsc() - qg2;
#endif
        }
#ifdef CRD_FFT_PROFILE
        ++prof::g_fs_calls;
#endif
        _mm_sfence(); // order the non-temporal stores before the caller reads the result
    }

    // Store one Complex<T>, optionally non-temporal (skips read-for-ownership — the 25.7 GB/s lever). NT path
    // taken only when nt && f64 && dst is ≥16B aligned (m_tbuf is 64B; the caller's `data` is checked once).
    static void store_complex(Complex<T>* dst, T re, T im, bool nt) noexcept
    {
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            if (nt)
            {
                _mm_stream_pd(reinterpret_cast<double*>(dst), _mm_set_pd(im, re));
                return;
            }
        }
        *dst = Complex<T>{re, im};
    }

    // Block width so the batched sub-FFT working set (size·B complex) stays cache-resident (~512 KB L2 budget).
    static crd::usize block_width(crd::usize size) noexcept
    {
        // 1 MB block — the full-FFT-swept optimum 2026-06-15 (gather wants larger contiguous runs; the sub-FFT
        // wants L2-resident scratch; 512KB/2MB/4MB all measured worse). The isolated gather is faster at larger
        // blocks but the full FFT regresses once the sub-FFT scratch exceeds L2.
        constexpr crd::usize scratch_budget = crd::usize{1} << 20;
        const crd::usize per = size * sizeof(Complex<T>);
        const crd::usize b = (per >= scratch_budget) ? crd::usize{1} : (scratch_budget / per);
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
    void radix4_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, T isign, const T* ptw_re,
                     const T* ptw_im) const
    {
        const crd::usize q = m_n >> 2; // n/4 (output-quarter stride)
        crd::usize off = 0;            // Lever A: precomputed linear twiddles (w1,w2,w3 per group)
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
                V ar;
                V ai;
                V br;
                V bi;
                V cr;
                V ci;
                V dr;
                V di;
                simd::load_complex_deinterleaved(dp + 2 * k, ar, ai);
                simd::load_complex_deinterleaved(dp + 2 * (r + k), br, bi);
                simd::load_complex_deinterleaved(dp + 2 * (2 * r + k), cr, ci);
                simd::load_complex_deinterleaved(dp + 2 * (3 * r + k), dr, di);
                const V t0r = ar + cr;
                const V t0i = ai + ci;
                const V t1r = ar - cr;
                const V t1i = ai - ci;
                const V t2r = br + dr;
                const V t2i = bi + di;
                const V t3r = br - dr;
                const V t3i = bi - di;
                const V rsr = V(0.0) - sg * t3i;
                const V rsi = sg * t3r;
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
                V p0r = V::load(xr + 4 * j);
                V p1r = V::load(xr + 4 * j + 4);
                V p2r = V::load(xr + 4 * j + 8);
                V p3r = V::load(xr + 4 * j + 12);
                simd::transpose4x4(p0r, p1r, p2r, p3r); // → p_m_r = re of point m for groups j..j+3
                V p0i = V::load(xi + 4 * j);
                V p1i = V::load(xi + 4 * j + 4);
                V p2i = V::load(xi + 4 * j + 8);
                V p3i = V::load(xi + 4 * j + 12);
                simd::transpose4x4(p0i, p1i, p2i, p3i);
                const V w1r = V::load(&m_tw_re[j]);
                const V w1i = V::load(&m_tw_im[j]) * sg;
                const V w2r(m_tw_re[2 * j], m_tw_re[2 * j + 2], m_tw_re[2 * j + 4], m_tw_re[2 * j + 6]);
                const V w2i = V(m_tw_im[2 * j], m_tw_im[2 * j + 2], m_tw_im[2 * j + 4], m_tw_im[2 * j + 6]) * sg;
                const V w3r(m_tw_re[3 * j], m_tw_re[3 * j + 3], m_tw_re[3 * j + 6], m_tw_re[3 * j + 9]);
                const V w3i = V(m_tw_im[3 * j], m_tw_im[3 * j + 3], m_tw_im[3 * j + 6], m_tw_im[3 * j + 9]) * sg;
                const V br = simd::fnmadd(w1i, p1i, w1r * p1r);
                const V bi = simd::fma(w1i, p1r, w1r * p1i);
                const V cr = simd::fnmadd(w2i, p2i, w2r * p2r);
                const V ci = simd::fma(w2i, p2r, w2r * p2i);
                const V dr = simd::fnmadd(w3i, p3i, w3r * p3r);
                const V di = simd::fma(w3i, p3r, w3r * p3i);
                const V t0r = p0r + cr;
                const V t0i = p0i + ci;
                const V t1r = p0r - cr;
                const V t1i = p0i - ci;
                const V t2r = br + dr;
                const V t2i = bi + di;
                const V t3r = br - dr;
                const V t3i = bi - di;
                const V rsr = V(0.0) - sg * t3i;
                const V rsi = sg * t3r;
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
            // FMA butterflies (2026-07-04 strided-v3): the MKL-archaeology build — cmul = 2 mul +
            // fnmadd + fma on split registers (fewer ops than MKL's own mul+mul+add+sub idiom).
            const V w1rv(w1r);
            const V w1iv(w1i);
            const V w2rv(w2r);
            const V w2iv(w2i);
            const V w3rv(w3r);
            const V w3iv(w3i);
            const V sg(isign);
            for (; k + 4 <= r; k += 4)
            {
                const V ar = V::load(xr + i0 + k);
                const V ai = V::load(xi + i0 + k);
                const V b0r = V::load(xr + i1 + k);
                const V b0i = V::load(xi + i1 + k);
                const V c0r = V::load(xr + i2 + k);
                const V c0i = V::load(xi + i2 + k);
                const V d0r = V::load(xr + i3 + k);
                const V d0i = V::load(xi + i3 + k);
                const V br = simd::fnmadd(w1iv, b0i, w1rv * b0r);
                const V bi = simd::fma(w1iv, b0r, w1rv * b0i);
                const V cr = simd::fnmadd(w2iv, c0i, w2rv * c0r);
                const V ci = simd::fma(w2iv, c0r, w2rv * c0i);
                const V dr = simd::fnmadd(w3iv, d0i, w3rv * d0r);
                const V di = simd::fma(w3iv, d0r, w3rv * d0i);
                const V t0r = ar + cr;
                const V t0i = ai + ci;
                const V t1r = ar - cr;
                const V t1i = ai - ci;
                const V t2r = br + dr;
                const V t2i = bi + di;
                const V t3r = br - dr;
                const V t3i = bi - di;
                const V rsr = V(0.0) - sg * t3i;
                const V rsi = sg * t3r;
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
            const V w1rv(w1r);
            const V w1iv(w1i);
            const V w2rv(w2r);
            const V w2iv(w2i);
            const V w3rv(w3r);
            const V w3iv(w3i);
            const V sg(isign);
            for (; k + 8 <= r; k += 8)
            {
                const V ar = V::load(xr + i0 + k);
                const V ai = V::load(xi + i0 + k);
                const V b0r = V::load(xr + i1 + k);
                const V b0i = V::load(xi + i1 + k);
                const V c0r = V::load(xr + i2 + k);
                const V c0i = V::load(xi + i2 + k);
                const V d0r = V::load(xr + i3 + k);
                const V d0i = V::load(xi + i3 + k);
                const V br = simd::fnmadd(w1iv, b0i, w1rv * b0r);
                const V bi = simd::fma(w1iv, b0r, w1rv * b0i);
                const V cr = simd::fnmadd(w2iv, c0i, w2rv * c0r);
                const V ci = simd::fma(w2iv, c0r, w2rv * c0i);
                const V dr = simd::fnmadd(w3iv, d0i, w3rv * d0r);
                const V di = simd::fma(w3iv, d0r, w3rv * d0i);
                const V t0r = ar + cr;
                const V t0i = ai + ci;
                const V t1r = ar - cr;
                const V t1i = ai - ci;
                const V t2r = br + dr;
                const V t2i = bi + di;
                const V t3r = br - dr;
                const V t3i = bi - di;
                const V rsr = V(0.0F) - sg * t3i;
                const V rsi = sg * t3r;
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
    crd::u32 m_log2 = 0U;
    crd::usize m_n1 = 0; // four-step split ≈ √n (n2 = n/n1)
    bool m_use_four_step = false;
    // standalone-hier (1024..64K, f64+f32): scratches + the full stage-twiddle table + the n1 split
    Complex<T>* m_sh_s = nullptr; // allocated only for the deep-split band (the 2-pass sh runs din→t→din)
    Complex<T>* m_sh_t = nullptr;
    T* m_sh_twr = nullptr; // full [k*n2+u] table, OR the factored LO table when m_sh_msh != 0
    T* m_sh_twi = nullptr;
    T* m_sh_hir = nullptr; // factored HI table (W_n^{k·M·uh}); null when the full table is used
    T* m_sh_hii = nullptr;
    crd::u32 m_sh_msh = 0;  // factored-twiddle shift (M = 1<<msh); 0 = full table
    crd::usize m_sh_n1 = 0; // sh: the stage-1 leaf; deep-split: A (the S1 leaf)
    // deep-split (128K/256K, session 6): n = A·B·C, three generated passes. 0 = not deep.
    crd::usize m_ds_b = 0;  // B (the S2 leaf); C = m_ds_c (the S3 leaf); A = m_sh_n1
    crd::usize m_ds_c = 0;
    crd::u32 m_ds_ash = 0;  // log2(A) — the S2 broadcast-twiddle lane-repeat shift
    T* m_ds_twr = nullptr;  // compact twiddle of the LAST notr stage: W_{BC} (3-stage) / W_{B2C} (4-stage)
    T* m_ds_twi = nullptr;
    // 4-STAGE deep-split (the K-stage sweep, 2026-07-04): n = A·B1·B2·C. 0 = 3-stage.
    crd::usize m_ds_b2 = 0; // B2; when set: S2 = notr B1 (W_{B1B2C} below), S3 = notr B2 per k1 block
    T* m_ds_tw2r = nullptr; // S2 twiddle W_{B1·B2·C}^{k·v}, [k*(B2*C) + v] (B1·B2·C entries)
    T* m_ds_tw2i = nullptr;
    // IP4-AoS (2026-07-04): the interleaved in-place radix-4 engine's digit-reversal and per-pass DIT twiddles
    // (execute_ip4aos). 0 = off.
    crd::usize m_ip_n = 0;
    crd::usize* m_ip_rev = nullptr; // base-4 digit-reversal permutation (slot of input j)
    T* m_ip_twr = nullptr;          // per-pass DIT twiddles, concatenated: per pass 3·q entries each
    T* m_ip_twi = nullptr;          // (w1[k],w2[k],w3[k] as three k-runs), vector-consumable
    FftPlan<T>* m_p1 = nullptr;      // Lever D: hoisted length-n1 sub-FFT (ctor-built ONCE)
    FftPlan<T>* m_p2 = nullptr;      // Lever D: hoisted length-n2 sub-FFT
    Complex<T>* m_tbuf = nullptr;    // Lever D: transpose intermediate, 64B-aligned (NT-store target)
    Complex<T>* m_scratch = nullptr; // Lever D: cache-resident block scratch, 64B-aligned
    crd::containers::Array<T> m_tw_re;
    crd::containers::Array<T> m_tw_im;
    crd::containers::Array<crd::usize> m_rev;
    mutable crd::containers::Array<T> m_re0; // SoA ping-pong scratch (one plan per thread)
    mutable crd::containers::Array<T> m_im0;
    mutable crd::containers::Array<T> m_re1;
    mutable crd::containers::Array<T> m_im1;
    // Lever D twiddle FACTORIZATION (the read-floor cut): the inter-stage twiddle W_n^{i2·k1} is NOT a stored
    // n-sized table (that was 128 MB of streaming DRAM read per transform @8M — a third of the four-step read
    // floor, pure overhead). Instead W_n^a = W_n^{a_hi·M}·W_n^{a_lo} from two ~√n L2-resident
    // tables (a = i2·k1 < n, a_lo = a & (M−1), a_hi = a >> m_ftw_h, M = 1<<m_ftw_h) + one complex multiply.
    crd::containers::Array<T> m_ftw_hi_re; // W_n^{j·M}, j ∈ [0, n/M)
    crd::containers::Array<T> m_ftw_hi_im;
    crd::containers::Array<T> m_ftw_lo_re; // W_n^{j},   j ∈ [0, M)
    crd::containers::Array<T> m_ftw_lo_im;
    crd::u32 m_ftw_h = 0;               // split shift: M = 1<<m_ftw_h ≈ √n
    crd::containers::Array<T> m_ptw_re; // Lever A: per-pass combine twiddles, flat in (j,m) pass order
    crd::containers::Array<T> m_ptw_im;
    crd::u32 m_rmax_bits = 3U; // size-aware max radix bits/pass (set in ctor; mirrored by build + execute)
    Complex<T>* m_hier_bbuf = nullptr; // M2: transposed intermediate for the hierarchical 64x64 4096 sub-FFT
    T* m_hier_twr = nullptr;           // M2: inner 64x64 twiddle  cos(2π·n2·k1/4096)
    T* m_hier_twi = nullptr;           // M2: inner 64x64 twiddle −sin(2π·n2·k1/4096)
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
