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
} // namespace detail

template <typename T> class FftPlan
{
public:
    FftPlan(crd::memory::IAllocator* alloc, crd::usize n)
        : m_alloc(alloc), m_tw_re(alloc), m_tw_im(alloc), m_rev(alloc), m_re0(alloc), m_im0(alloc), m_re1(alloc),
          m_im1(alloc), m_ftw_hi_re(alloc), m_ftw_hi_im(alloc), m_ftw_lo_re(alloc), m_ftw_lo_im(alloc), m_ptw_re(alloc),
          m_ptw_im(alloc)
    {
        CRD_ASSERT(is_pow2(n)); // v10: power-of-2 (Bluestein/Rader make any size O(n log n) at v10-c)
        m_s.n = n;
        while ((crd::usize{1} << m_s.log2) < n)
        {
            ++m_s.log2;
        }
        // Above kFourStepMin the direct Stockham is DRAM-bound (log4(n) full passes); switch to the Bailey
        // four-step (n = n1·n2 ≈ √n × √n) so the sub-FFTs are cache-resident — O(1) DRAM passes, not O(log n).
        m_s.use_four_step = (n >= kFourStepMin);
        // f32 256K (2^18): enable the four-step (1024×256 split below) — the P2 256-axis then uses the default 16×16
        // hier codelet + gather/scatter fusion, fixing the small-N Stockham trough (0.33→~0.85× MKL, 3.6× CRD). f64
        // 256K stays direct (there is no f64 256-pt hier codelet). Larger sizes keep the square split.
        if (n == (crd::usize{1} << 18) && std::is_same_v<T, crd::f32>)
        {
            m_s.use_four_step = true;
        }
        // FFT-CRUSH 2026-07-03 session 6: f32 128K measured 0.17x MKL as direct Stockham (the sh band tops at
        // 64K) — opt into the four-step 1024x128 like f64: P1 = the gather-fused 1024 hier (bw==128), P2 = the
        // generated f32 codelet128_batched via the batched-leaf gate.
        if (n == (crd::usize{1} << 17) && std::is_same_v<T, crd::f32>)
        {
            m_s.use_four_step = true;
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
                m_s.use_four_step = true;
            }
        }
        // n1 ≈ √n (n2 = n/n1, both powers of 2) — the square split, confirmed near-optimal by a full-FFT n1/n2
        // plan search 2026-06-15 (±1 shift is marginal and size-dependent: 8M ~+1.5%, 4M worse).
        // n1 = 2^ceil(log2/2): the LARGER factor first. Square split for even log2 (unchanged); for ODD log2 the
        // larger factor as n1 measures faster — 8M (2048×4096→4096×2048) +9%, 2M +13%, f32 8M +10%, 4M square
        // unchanged, accuracy preserved (~1e-15). Found by the black-box MKL-archaeology factorization sweep
        // (2026-06-16): the old floor(log2/2) put the SMALLER factor first for odd log2, underusing pass-1.
        m_s.n1 = crd::usize{1} << ((m_s.log2 + 1) / 2);
        if (m_s.use_four_step && n == (crd::usize{1} << 18)) // 256K = 1024×256 (n2=256 → 16×16 hier P2)
        {
            m_s.n1 = 1024;
        }
        if constexpr (std::is_same_v<T, crd::f64>) // FFT-CRUSH: ALL-HIER splits for the f64 mid-band
        {
            if (m_s.use_four_step && n == (crd::usize{1} << 17)) // 128K = 1024×128 (P2 = codelet128)
            {
                m_s.n1 = 1024;
            }
            if (m_s.use_four_step && n == (crd::usize{1} << 18)) // 256K = 1024×256 (P2 radix-8 until codelet256)
            {
                m_s.n1 = 1024;
            }
        }
        if constexpr (std::is_same_v<T, crd::f32>) // FFT-CRUSH: f32 512K trough (measured 0.55x) — all-hier split
        {
            if (m_s.use_four_step && n == (crd::usize{1} << 19)) // 512K = 2048×256: P2 = 16×16 hier
            {
                m_s.n1 = 2048;
            }
            if (m_s.use_four_step && n == (crd::usize{1} << 17)) // 128K = 1024×128 (P2 = codelet128)
            {
                m_s.n1 = 1024;
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
            m_rev[i] = bitrev(i, m_s.log2);
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
            crd::usize h_n1 = 64U;                       // N1 = stage-1 leaf = twiddle stride
            crd::usize h_bb = detail::block_width<T>(n); // BB (T-aware: f32 = 2× f64)
            if (n == 1024)                               // 1024 = 32×32 — 1M/2M sub-FFT
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
                m_s.hier_bbuf = plan_table<Complex<T>>(n * h_bb, 64);
                m_s.hier_twr = plan_table<T>(h_n2 * h_n1, 32);
                m_s.hier_twi = plan_table<T>(h_n2 * h_n1, 32);
                constexpr double tp = 6.283185307179586476925286766559;
                for (crd::usize n2 = 0; n2 < h_n2; ++n2)
                {
                    for (crd::usize k1 = 0; k1 < h_n1; ++k1)
                    {
                        const double th = tp * static_cast<double>(n2 * k1) / static_cast<double>(n);
                        m_s.hier_twr[n2 * h_n1 + k1] = static_cast<T>(crd::math::cos(th));
                        m_s.hier_twi[n2 * h_n1 + k1] = static_cast<T>(-crd::math::sin(th));
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
            bool sh = !m_s.use_four_step && n >= 1024 && n <= 65536; // 2-pass hier band (65536 = 256×256)
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
                    m_s.sh_n1 = (n <= (crd::usize{1} << 17)) ? 32U : 64U; // A (the S1 leaf = stream count)
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
                    // 4-STAGE trials (the K-stage sweep, 2026-07-04): n = A·B1·B2·C with tiny factors
                    // — the MKL pass shape (small live sets per stage). Measured per row vs the
                    // banked 3-stage; losers revert here.
                    if (n == (crd::usize{1} << 13))
                    {
                        m_s.sh_n1 = 16U; // 8K = 16·8·8·8
                        m_s.ds_b = 8U;
                        m_s.ds_b2 = 8U;
                        m_s.ds_c = 8U;
                    }
                    else if (n == (crd::usize{1} << 14))
                    {
                        m_s.sh_n1 = 16U; // 16K = 16·16·8·8
                        m_s.ds_b = 16U;
                        m_s.ds_b2 = 8U;
                        m_s.ds_c = 8U;
                    }
                    else if (n == (crd::usize{1} << 15))
                    {
                        m_s.sh_n1 = 16U; // 32K = 16·16·16·8
                        m_s.ds_b = 16U;
                        m_s.ds_b2 = 16U;
                        m_s.ds_c = 8U;
                    }
                    else if (n == (crd::usize{1} << 16))
                    {
                        m_s.sh_n1 = 16U; // 64K = 16·16·16·16
                        m_s.ds_b = 16U;
                        m_s.ds_b2 = 16U;
                        m_s.ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 17))
                    {
                        m_s.sh_n1 = 32U; // 128K = 32·16·16·16
                        m_s.ds_b = 16U;
                        m_s.ds_b2 = 16U;
                        m_s.ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 18))
                    {
                        m_s.sh_n1 = 32U; // 256K = 32·32·16·16
                        m_s.ds_b = 32U;
                        m_s.ds_b2 = 16U;
                        m_s.ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 19))
                    {
                        m_s.sh_n1 = 32U; // 512K = 32·32·32·16
                        m_s.ds_b = 32U;
                        m_s.ds_b2 = 32U;
                        m_s.ds_c = 16U;
                    }
                    else if (n == (crd::usize{1} << 20))
                    {
                        m_s.sh_n1 = 32U; // 1M = 32·32·32·32: f32 WINNER (2.24 ms, 0.91×); f64 measured
                        m_s.ds_b = 32U;  // 5.88 vs the six-step's 5.38 — f64 1M keeps fs6 (gate below).
                        m_s.ds_b2 = 32U;
                        m_s.ds_c = 32U;
                    }
                    else if (n == (crd::usize{1} << 21))
                    {
                        m_s.sh_n1 = 32U; // 2M = 32·32·32·64 (vs four-step 0.83-0.90×)
                        m_s.ds_b = 32U;
                        m_s.ds_b2 = 32U;
                        m_s.ds_c = 64U;
                    }
                    else if (n == (crd::usize{1} << 22))
                    {
                        m_s.sh_n1 = 32U; // 4M = 32·32·64·64
                        m_s.ds_b = 32U;
                        m_s.ds_b2 = 64U;
                        m_s.ds_c = 64U;
                    }
                    else if (n == (crd::usize{1} << 23))
                    {
                        m_s.sh_n1 = 64U; // 8M = 64·32·64·64
                        m_s.ds_b = 32U;
                        m_s.ds_b2 = 64U;
                        m_s.ds_c = 64U;
                    }
                    while ((crd::usize{1} << m_s.ds_ash) < m_s.sh_n1)
                    {
                        ++m_s.ds_ash;
                    }
                    m_s.sh_s = plan_table<Complex<T>>(n, 64);
                    constexpr double tpd = 6.283185307179586476925286766559;
                    // last-notr-stage table: W_{B·C} (3-stage) or W_{B2·C} (4-stage), [k*C + v]
                    const crd::usize lb = (m_s.ds_b2 != 0) ? m_s.ds_b2 : m_s.ds_b;
                    const crd::usize bc = lb * m_s.ds_c;
                    m_s.ds_twr = plan_table<T>(bc, 64);
                    m_s.ds_twi = plan_table<T>(bc, 64);
                    for (crd::usize kb = 0; kb < lb; ++kb)
                    {
                        for (crd::usize v = 0; v < m_s.ds_c; ++v)
                        {
                            const double th = tpd * static_cast<double>((kb * v) % bc) / static_cast<double>(bc);
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
                                const double th = tpd * static_cast<double>((kb * v) % e) / static_cast<double>(e);
                                m_s.ds_tw2r[kb * b2c + v] = static_cast<T>(crd::math::cos(th));
                                m_s.ds_tw2i[kb * b2c + v] = static_cast<T>(-crd::math::sin(th));
                            }
                        }
                    }
                }
                else
                {
                    m_s.sh_n1 = 256U; // 32768 = 256×128, 65536 = 256×256
                    if (n == 1024)
                    {
                        m_s.sh_n1 = 32U;
                    }
                    else if (n == 2048 || n == 4096 || n == 8192)
                    {
                        m_s.sh_n1 = 64U;
                    }
                    else if (n == 16384)
                    {
                        m_s.sh_n1 = 128U;
                    }
                }
                const crd::usize sn2 = n / m_s.sh_n1;
                m_s.sh_t = plan_table<Complex<T>>(n, 64);
                constexpr double tp2 = 6.283185307179586476925286766559;
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
                        m_s.ip_n = n;
                        const bool ipodd = (m_s.log2 & 1U) != 0U;
                        const crd::usize nh = ipodd ? (n >> 1) : n; // 4^k length per half
                        const crd::u32 dg = (ipodd ? m_s.log2 - 1U : m_s.log2) / 2U;
                        // Radix plan (round 17), innermost→outermost: [4,4 (the gather fold)],
                        // then TWO radix-8 passes for nh ≥ 4096, then radix-4 to nh (the LAST pass
                        // must be radix-4 — the COBRA quad identity needs a base-4 top slot digit).
                        crd::u32 rads[16];
                        crd::u32 nr = 0;
                        rads[nr++] = 4U;
                        rads[nr++] = 4U;
                        const crd::u32 log2h = ipodd ? m_s.log2 - 1U : m_s.log2;
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
                            // INVERSE map (mixed-radix reversal is NOT an involution): the gather
                            // wants io-index per SLOT ⇒ rev[slot(j)] = j (×2+parity for odd).
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
                        // HYBRID tables (round 15): full 3-set (w1,w2,w3) runs for len ≤ 1024 —
                        // tiny (≤8KB), no in-register twiddle powers needed — and w1-only for the
                        // BIG passes (their tables were the DTLB bulk; w2/w3 computed in-register
                        // there). Non-duplicated storage throughout; dup expansion at load.
                        m_s.ip_twr = plan_table<T>(n, 64);
                        m_s.ip_twi = plan_table<T>(n, 64);
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
                                        m_s.ip_twr[off] = static_cast<T>(crd::math::cos(th));
                                        m_s.ip_twi[off] = static_cast<T>(-crd::math::sin(th));
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
                                m_s.ip_twr[off] = static_cast<T>(crd::math::cos(th));
                                m_s.ip_twi[off] = static_cast<T>(-crd::math::sin(th));
                                ++off;
                            }
                        }
                    }
                }
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
                            const double th = tp2 * static_cast<double>((k1 * ul) % n) / static_cast<double>(n);
                            m_s.sh_twr[k1 * mm + ul] = static_cast<T>(crd::math::cos(th));
                            m_s.sh_twi[k1 * mm + ul] = static_cast<T>(-crd::math::sin(th));
                        }
                        for (crd::usize uh = 0; uh < uhc; ++uh)
                        {
                            const double th =
                                tp2 * static_cast<double>((k1 * (uh << m_s.sh_msh)) % n) / static_cast<double>(n);
                            m_s.sh_hir[k1 * uhc + uh] = static_cast<T>(crd::math::cos(th));
                            m_s.sh_hii[k1 * uhc + uh] = static_cast<T>(-crd::math::sin(th));
                        }
                    }
                }
                else
                {
                    m_s.sh_twr = plan_table<T>(n, 64);
                    m_s.sh_twi = plan_table<T>(n, 64);
                    for (crd::usize k1 = 0; k1 < m_s.sh_n1; ++k1)
                    {
                        for (crd::usize i2 = 0; i2 < sn2; ++i2)
                        {
                            const double th = tp2 * static_cast<double>((k1 * i2) % n) / static_cast<double>(n);
                            // stored in the STAGE-1-OUTPUT layout s[k1*n2 + i2] (applied before the transpose)
                            m_s.sh_twr[k1 * sn2 + i2] = static_cast<T>(crd::math::cos(th));
                            m_s.sh_twi[k1 * sn2 + i2] = static_cast<T>(-crd::math::sin(th));
                        }
                    }
                }
            }
        }
        if (m_s.use_four_step) // Lever D resurrection: four-step buffers + HOISTED sub-plans + linear twiddle
        {
            // tbuf (transpose intermediate) + scratch (cache-resident block) are RAW 64-BYTE-ALIGNED (NT stores
            // need ≥32B; crd Array gives only 8B). Freed in the dtor.
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
        }
        else // direct radix path: SoA ping-pong scratch + Lever A per-pass combine twiddles
        {
            m_re0.resize(n);
            m_im0.resize(n);
            m_re1.resize(n);
            m_im1.resize(n);
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
        if (m_s.scratch != nullptr)
        {
            m_alloc->deallocate(m_s.scratch);
        }
        if (m_s.tbuf != nullptr)
        {
            m_alloc->deallocate(m_s.tbuf);
        }
        if (m_s.sh_t != nullptr)
        {
            if (m_s.sh_s != nullptr)
            {
                m_alloc->deallocate(m_s.sh_s);
            }
            m_alloc->deallocate(m_s.sh_t);
            m_alloc->deallocate(m_s.sh_twr);
            m_alloc->deallocate(m_s.sh_twi);
            if (m_s.sh_hir != nullptr)
            {
                m_alloc->deallocate(m_s.sh_hir);
                m_alloc->deallocate(m_s.sh_hii);
            }
        }
        if (m_s.ds_twr != nullptr)
        {
            m_alloc->deallocate(m_s.ds_twr);
            m_alloc->deallocate(m_s.ds_twi);
        }
        if (m_s.ds_tw2r != nullptr)
        {
            m_alloc->deallocate(m_s.ds_tw2r);
            m_alloc->deallocate(m_s.ds_tw2i);
        }
        if (m_s.ip_rev != nullptr)
        {
            m_alloc->deallocate(m_s.ip_rev);
            m_alloc->deallocate(m_s.ip_twr);
            m_alloc->deallocate(m_s.ip_twi);
        }
        if (m_s.hier_bbuf != nullptr)
        {
            m_alloc->deallocate(m_s.hier_bbuf);
        }
        if (m_s.hier_twr != nullptr)
        {
            m_alloc->deallocate(m_s.hier_twr);
        }
        if (m_s.hier_twi != nullptr)
        {
            m_alloc->deallocate(m_s.hier_twi);
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
    // Above this size the direct Stockham is DRAM-bound and the blocked four-step (NT-store scatter + the hoisted
    // batched sub-FFTs, Lever D 2026-06-14) wins; the ctor opts some smaller sizes in.
    static constexpr crd::usize kFourStepMin = crd::usize{1} << 19;

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
