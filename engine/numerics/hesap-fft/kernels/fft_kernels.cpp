// The hesap-fft kernels of crd/hesap/fft/fft_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS): the engine,
// the generated codelets and the SIMD types it runs on. The plan (FftPlanState) comes from general code; a sub-plan
// runs on the same level, never through the table.
//
// Every level runs one algorithm per size on the level's SIMD types and the generated codelets, whose operations each
// give the same bits on every backend, so the family is bit-exact across levels. A vector step covers tiles of four f64
// or eight f32 columns (Vec4d, Vec8f), two tiles a step at avx512 (Vec8d, Vec16f). Only memory hints (prefetch,
// non-temporal stores, the store fence) depend on the level; they change no value.

#include <crd/core/kernel_level.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/fft/detail/batched_codelets_gen.hpp>
#include <crd/hesap/fft/detail/block_width.hpp>
#include <crd/hesap/fft/detail/codelets.hpp>
#include <crd/hesap/fft/detail/hier_codelets.hpp>
#include <crd/hesap/fft/detail/small_n_codelets.hpp>
#include <crd/hesap/fft/detail/twiddle_codelets.hpp>
#include <crd/hesap/fft/fft_kernels.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#if CRD_SIMD_HAS_AVX512
#include <crd/math/simd/vec16f.hpp>
#include <crd/math/simd/vec8d.hpp>
#endif

#include <cstdint>
#include <cstring>
#include <type_traits>

#ifdef CRD_FFT_PROFILE
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS
{
namespace
{

#ifdef CRD_FFT_PROFILE
namespace prof
{
using namespace crd::hesap::fft::prof; // NOLINT(google-build-using-namespace): the counters of fft_kernels.hpp
[[nodiscard]] inline unsigned long long rdtsc() noexcept
{
    return __rdtsc();
}
} // namespace prof
#endif

// The four-step's memory hints on the levels with SSE2; the portable level reads and stores plainly (the same bits).
CRD_FORCEINLINE void prefetch_row(const void* p) noexcept
{
#if CRD_SIMD_HAS_SSE2
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#else
    (void)p;
#endif
}

CRD_FORCEINLINE void store_fence() noexcept
{
#if CRD_SIMD_HAS_SSE2
    _mm_sfence();
#endif
}

namespace simd = crd::math::simd;

// The vector steps' lanes. A tile of four f64 or eight f32 columns (TileVec) is the unit of every step's column range
// on every level; at avx512 a step takes two tiles in one Vec8d or Vec16f (WideVec), an odd last tile one TileVec.
// Which columns the steps and a scalar tail compute, and so their bits, never depend on the level.
template <typename T> using TileVec = std::conditional_t<std::is_same_v<T, crd::f64>, simd::Vec4d, simd::Vec8f>;
#if CRD_SIMD_HAS_AVX512
template <typename T> using WideVec = std::conditional_t<std::is_same_v<T, crd::f64>, simd::Vec8d, simd::Vec16f>;
#else
template <typename T> using WideVec = TileVec<T>;
#endif
// The columns a step on V computes.
template <typename T, typename V> inline constexpr crd::usize kCols = sizeof(V) / sizeof(T);
static_assert(kCols<crd::f64, TileVec<crd::f64>> == 4 && kCols<crd::f32, TileVec<crd::f32>> == 8);

// run<V>(first, last) over the whole tiles of [0, n), kCols<T, V> columns a step: two tiles a step on WideVec at
// avx512, then an odd last tile on TileVec. A run hoists its constants. Returns where the caller's scalar tail starts:
// n rounded down to a tile on every level.
template <typename T, typename Run> CRD_FORCEINLINE crd::usize for_tiles(crd::usize n, Run run) noexcept
{
    using W = WideVec<T>;
    using H = TileVec<T>;
    const crd::usize tiles = n - n % kCols<T, H>;
    const crd::usize wide = tiles - tiles % kCols<T, W>;
    run.template operator()<W>(crd::usize{0}, wide);
    if constexpr (!std::is_same_v<W, H>)
    {
        run.template operator()<H>(wide, tiles);
    }
    return tiles;
}

// A step's f64 lanes at(0) .. at(kCols - 1), one Vec4d a tile.
template <typename V, typename At> CRD_FORCEINLINE V gather_f64(At at) noexcept
{
    const auto tile = [&](crd::usize c) noexcept
    {
        return simd::Vec4d(at(c), at(c + 1), at(c + 2), at(c + 3));
    };
    if constexpr (std::is_same_v<V, simd::Vec4d>)
    {
        return tile(0);
    }
    else
    {
        return V(tile(0), tile(4));
    }
}

// A step's groups of four contiguous f64 at p, one vector per point of the groups: a 4x4 transpose a tile.
template <typename V> CRD_FORCEINLINE void load_points4(const crd::f64* p, V& x0, V& x1, V& x2, V& x3) noexcept
{
    const auto tile = [](const crd::f64* q, simd::Vec4d& a, simd::Vec4d& b, simd::Vec4d& c, simd::Vec4d& d) noexcept
    {
        a = simd::Vec4d::load(q);
        b = simd::Vec4d::load(q + 4);
        c = simd::Vec4d::load(q + 8);
        d = simd::Vec4d::load(q + 12);
        simd::transpose4x4(a, b, c, d);
    };
    if constexpr (std::is_same_v<V, simd::Vec4d>)
    {
        tile(p, x0, x1, x2, x3);
    }
    else
    {
        simd::Vec4d lo[4];
        simd::Vec4d hi[4];
        tile(p, lo[0], lo[1], lo[2], lo[3]);
        tile(p + 16, hi[0], hi[1], hi[2], hi[3]);
        x0 = V(lo[0], hi[0]);
        x1 = V(lo[1], hi[1]);
        x2 = V(lo[2], hi[2]);
        x3 = V(lo[3], hi[3]);
    }
}

// The in-register transpose of a micro-tile's rows: lane j of row i becomes lane i of row j.
template <typename V, crd::usize N> CRD_FORCEINLINE void transpose_rows(V (&x)[N]) noexcept
{
    if constexpr (N == 4)
    {
        simd::transpose4x4(x[0], x[1], x[2], x[3]);
    }
    else if constexpr (N == 8)
    {
        simd::transpose8x8(x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
    }
    else
    {
        static_assert(N == 16, "a micro-tile is four, eight or sixteen rows");
#if CRD_SIMD_HAS_AVX512
        simd::transpose16x16(x);
#endif
    }
}

// The transform engine of one plan.
template <typename T> struct Engine
{
    const FftPlanState<T>& plan;

    // The PRODUCTION transform: Stockham autosort radix-2 on split SoA buffers, vectorized over the unit-stride
    // butterfly index. In-place from the caller's view (deinterleave → SoA → reinterleave).
    void execute(Complex<T>* data, FftDirection dir) const
    {
        if (plan.n <= 1)
        {
            return;
        }
        if (plan.n <= 32) // leaf-codelet fast path: one straight-line block, zero loop overhead (L1-resident)
        {
            // f64 N∈{8,16,32}: the AoS lane-trick codelet operates in place on the interleaved data (exactly its
            // [re,im,…] layout), avoiding the SoA deinterleave/reinterleave round-trip — faster than the SoA leaf
            // it replaces at these sizes. The SoA path below stays for n=2/4 and f32. → small_n_codelets.hpp.
            if constexpr (std::is_same_v<T, crd::f64>)
            {
                if (plan.n == 8 || plan.n == 16 || plan.n == 32)
                {
                    detail::small_n_fft_f64(reinterpret_cast<crd::f64*>(data), plan.n, dir == FftDirection::Forward);
                    return;
                }
            }
            T* re = plan.re0;
            T* im = plan.im0;
            for (crd::usize i = 0; i < plan.n; ++i)
            {
                re[i] = data[i].re;
                im[i] = data[i].im;
            }
            dispatch_codelet(re, im, dir);
            for (crd::usize i = 0; i < plan.n; ++i)
            {
                data[i].re = re[i];
                data[i].im = im[i];
            }
            return;
        }
        switch (dir == FftDirection::Forward ? plan.forward : plan.inverse)
        {
            case FftAlgorithm::Ip4:
                if (dir == FftDirection::Forward)
                {
                    execute_ip4aos<false>(data);
                }
                else
                {
                    execute_ip4aos<true>(data);
                }
                return;
            case FftAlgorithm::DeepSplit3:
            case FftAlgorithm::DeepSplit4:
                execute_deep_split(data);
                return;
            case FftAlgorithm::TiledFourStep:
                if constexpr (std::is_same_v<T, crd::f32>) // the planner offers it only to f32
                {
                    execute_tiled_four_step(data);
                }
                return;
            case FftAlgorithm::SixStep:
                execute_six_step(data);
                return;
            case FftAlgorithm::FourStep:
                execute_four_step(data, dir);
                return;
            case FftAlgorithm::StandaloneHier:
                execute_standalone_hier(data);
                return;
            case FftAlgorithm::Stockham:
                break;
        }
        // NB (2026-06-14): the depth-first recursion (rec_fft_soa) was MEASURED for the mid-band and is dead —
        // worse than this Stockham path at every size (8192 −6%, 65536 −27%, 131072 −33% collapsing). Decisive:
        // even fully L1-resident at n=1024 (ZERO cache penalty) this codelet path leaves throughput on the table,
        // so the mid-band wall is CODELET QUALITY in isolation, not cache structure; the lever is
        // gen_fft_codelets.py (modified split-radix + scheduling), not a recursive driver. rec_fft_soa is a seed only.
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        T* xr = plan.re0;
        T* xi = plan.im0;
        T* yr = plan.re1;
        T* yi = plan.im1;
        const Complex<T>* din = data;
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
        if ((plan.log2 & 1U) != 0U)
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
        const T* ptw_re = plan.ptw_re; // Lever A: walk the precomputed per-pass twiddle table in pass order
        const T* ptw_im = plan.ptw_im;
        while (t < plan.log2 - 2U)
        {
            const crd::u32 remaining = (plan.log2 - 2U) - t;
            crd::u32 b = plan.rmax_bits < remaining ? plan.rmax_bits : remaining;
            while (b > 2U && (remaining - b) == 1U) // never strand an uncoverable 1-bit remainder
            {
                --b;
            }
            const crd::usize lq = crd::usize{1} << t;
            switch (b)
            {
                case 5U:
                    radix32_pass(xr, xi, yr, yi, lq, plan.n >> (t + 5U), dir, ptw_re, ptw_im);
                    break;
                case 4U:
                    radix16_pass(xr, xi, yr, yi, lq, plan.n >> (t + 4U), dir, ptw_re, ptw_im);
                    break;
                case 3U:
                    radix8_pass(xr, xi, yr, yi, lq, plan.n >> (t + 3U), dir, ptw_re, ptw_im);
                    break;
                default:
                    radix4_pass(xr, xi, yr, yi, lq, plan.n >> (t + 2U), isign, ptw_re, ptw_im);
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
        radix4_last_interleaved(xr, xi, data, crd::usize{1} << t, isign); // last pass (r=1), folds reinterleave
#ifdef CRD_FFT_PROFILE
        const unsigned long long pf3 = prof::rdtsc();
        prof::g_first += pf1 - pf0;
        prof::g_combine += pf2 - pf1;
        prof::g_last += pf3 - pf2;
        ++prof::g_calls;
#endif
    }

    // FFT-CRUSH 2026-07-03 DEEP-SPLIT (forward): n = A·B·C, three generated passes — S1 leaf+W_n+transpose → S2
    // leaf+broadcast-W_{BC} → S3 strided leaf, natural order out (d[kA + A·kB + AB·kC]; derivation at the plan's
    // build_deep_split) — or n = A·B1·B2·C, four passes.
    void execute_deep_split(Complex<T>* data) const
    {
        Complex<T>* const d = data;
        const crd::usize a = plan.sh_n1;
        const crd::usize ca = plan.ds_c * a;
        // 4-stage: rest = B2·C (the elements remaining after the S2 leaf); 3-stage: rest = C.
        const crd::usize rest = (plan.ds_b2 != 0 ? plan.ds_b2 : 1U) * plan.ds_c;
        const crd::usize s2b = rest * a; // S2 batch width
#ifdef CRD_FFT_PROFILE
        const unsigned long long d0 = prof::rdtsc();
#endif
        // AoSoA (2026-07-04 crush): S1 deinterleaves ONCE into [L×re|L×im] block rows over
        // plan.sh_t; S2 runs pure block form (zero shuffles, one stream per row); S3
        // reinterleaves once on the strided final store.
        T* const tb = reinterpret_cast<T*>(plan.sh_t); // n complex = exactly 2n T of AoSoA blocks
        T* const sb = reinterpret_cast<T*>(plan.sh_s);
        if (plan.sh_msh != 0U) // S1: A-leaf + FACTORED W_n twiddle + transpose (n2 >= 1024)
        {
            switch (a)
            {
                case 16U:
                    gen::codelet16_stage1_fused_sh_csf(d, tb, plan.ds_b * rest, plan.sh_twr, plan.sh_twi, plan.sh_hir,
                                                       plan.sh_hii, plan.sh_msh);
                    break;
                case 32U:
                    gen::codelet32_stage1_fused_sh_csf(d, tb, plan.ds_b * rest, plan.sh_twr, plan.sh_twi, plan.sh_hir,
                                                       plan.sh_hii, plan.sh_msh);
                    break;
                default:
                    gen::codelet64_stage1_fused_sh_csf(d, tb, plan.ds_b * rest, plan.sh_twr, plan.sh_twi, plan.sh_hir,
                                                       plan.sh_hii, plan.sh_msh);
                    break;
            }
        }
        else // S1 with the full [k*n2+u] table (the small-n2 ds sizes: 4K/8K/16K)
        {
            switch (a)
            {
                case 16U:
                    gen::codelet16_stage1_fused_sh_cs(d, tb, plan.ds_b * rest, plan.sh_twr, plan.sh_twi);
                    break;
                case 32U:
                    gen::codelet32_stage1_fused_sh_cs(d, tb, plan.ds_b * rest, plan.sh_twr, plan.sh_twi);
                    break;
                default:
                    gen::codelet64_stage1_fused_sh_cs(d, tb, plan.ds_b * rest, plan.sh_twr, plan.sh_twi);
                    break;
            }
        }
#ifdef CRD_FFT_PROFILE
        const unsigned long long d1 = prof::rdtsc();
#endif
        {
            // S2: B-leaf over the (rest·A)-wide batch + broadcast twiddle (W_{BC} 3-stage /
            // W_{B1B2C} 4-stage)
            const T* const t2r = (plan.ds_b2 != 0) ? plan.ds_tw2r : plan.ds_twr;
            const T* const t2i = (plan.ds_b2 != 0) ? plan.ds_tw2i : plan.ds_twi;
            switch (plan.ds_b)
            {
                case 8U:
                    gen::codelet8_fused_notr_ss(tb, sb, s2b, plan.ds_ash, t2r, t2i);
                    break;
                case 16U:
                    gen::codelet16_fused_notr_ss(tb, sb, s2b, plan.ds_ash, t2r, t2i);
                    break;
                case 32U:
                    gen::codelet32_fused_notr_ss(tb, sb, s2b, plan.ds_ash, t2r, t2i);
                    break;
                case 64U:
                    gen::codelet64_fused_notr_ss(tb, sb, s2b, plan.ds_ash, t2r, t2i);
                    break;
                default:
                    gen::codelet128_fused_notr_ss(tb, sb, s2b, plan.ds_ash, t2r, t2i);
                    break;
            }
        }
#ifdef CRD_FFT_PROFILE
        const unsigned long long d2 = prof::rdtsc();
#endif
        if (plan.ds_b2 != 0) // 4-STAGE: S3 = notr B2 per k1 block (sb -> tb), S4 = strided C
        {
            const crd::usize blk2 = 2 * plan.ds_b2 * ca; // T units per k1 block
            for (crd::usize k1 = 0; k1 < plan.ds_b; ++k1)
            {
                switch (plan.ds_b2)
                {
                    case 8U:
                        gen::codelet8_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, plan.ds_ash, plan.ds_twr,
                                                    plan.ds_twi);
                        break;
                    case 16U:
                        gen::codelet16_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, plan.ds_ash, plan.ds_twr,
                                                     plan.ds_twi);
                        break;
                    case 32U:
                        gen::codelet32_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, plan.ds_ash, plan.ds_twr,
                                                     plan.ds_twi);
                        break;
                    default:
                        gen::codelet64_fused_notr_ss(sb + k1 * blk2, tb + k1 * blk2, ca, plan.ds_ash, plan.ds_twr,
                                                     plan.ds_twi);
                        break;
                }
            }
            const crd::usize os4 = a * plan.ds_b * plan.ds_b2;
            for (crd::usize k1 = 0; k1 < plan.ds_b; ++k1) // S4: d[kA + A·k1 + AB1·k2 + AB1B2·kC]
            {
                for (crd::usize k2 = 0; k2 < plan.ds_b2; ++k2)
                {
                    const T* const src = tb + k1 * blk2 + k2 * 2 * ca;
                    Complex<T>* const dst = d + (k1 + plan.ds_b * k2) * a;
                    switch (plan.ds_c)
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
            for (crd::usize kb = 0; kb < plan.ds_b; ++kb) // S3: C-leaf per kB block, natural order
            {
                switch (plan.ds_c)
                {
                    case 16U:
                        gen::codelet16_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * plan.ds_b);
                        break;
                    case 32U:
                        gen::codelet32_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * plan.ds_b);
                        break;
                    case 64U:
                        gen::codelet64_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * plan.ds_b);
                        break;
                    default:
                        gen::codelet128_batched_strided_sc(sb + kb * 2 * ca, d + kb * a, a, a * plan.ds_b);
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
    }

    // FFT-CRUSH 2026-07-03 standalone-HIER (forward, 1024..65536, f64 Vec4d + f32 Vec8f): one stage-1-fused pass (leaf
    // + twiddle + transposed store) + one batched-leaf pass — TWO passes total, replacing ~log4(n) Stockham passes.
    // Natural-order output by the four-step identity.
    void execute_standalone_hier(Complex<T>* data) const
    {
        const crd::usize n1 = plan.sh_n1;
        const crd::usize n2 = plan.n / n1;
        Complex<T>* din2 = data;
        // AoSoA (2026-07-04 crush): stage-1 deinterleaves ONCE into [L×re|L×im] block rows
        // over plan.sh_t (transposed block stores, no interleave shuffles, one stream per row);
        // stage-2 reads blocks shuffle-free and reinterleaves once on the final store. FMA
        // butterflies throughout.
        T* const tb = reinterpret_cast<T*>(plan.sh_t); // n complex = exactly 2n T of AoSoA blocks
        if (plan.sh_msh != 0U)                         // stage-1 FUSED, FACTORED L1-resident twiddle (n2 >= 128)
        {
            switch (n1)
            {
                case 32U:
                    gen::codelet32_stage1_fused_sh_csf(din2, tb, n2, plan.sh_twr, plan.sh_twi, plan.sh_hir, plan.sh_hii,
                                                       plan.sh_msh);
                    break;
                case 64U:
                    gen::codelet64_stage1_fused_sh_csf(din2, tb, n2, plan.sh_twr, plan.sh_twi, plan.sh_hir, plan.sh_hii,
                                                       plan.sh_msh);
                    break;
                case 128U:
                    gen::codelet128_stage1_fused_sh_csf(din2, tb, n2, plan.sh_twr, plan.sh_twi, plan.sh_hir,
                                                        plan.sh_hii, plan.sh_msh);
                    break;
                default:
                    gen::codelet256_stage1_fused_sh_csf(din2, tb, n2, plan.sh_twr, plan.sh_twi, plan.sh_hir,
                                                        plan.sh_hii, plan.sh_msh);
                    break;
            }
        }
        else // stage-1 FUSED (leaf + full-table twiddle + transposed block store): ONE pass
        {
            switch (n1)
            {
                case 32U:
                    gen::codelet32_stage1_fused_sh_cs(din2, tb, n2, plan.sh_twr, plan.sh_twi);
                    break;
                case 64U:
                    gen::codelet64_stage1_fused_sh_cs(din2, tb, n2, plan.sh_twr, plan.sh_twi);
                    break;
                case 128U:
                    gen::codelet128_stage1_fused_sh_cs(din2, tb, n2, plan.sh_twr, plan.sh_twi);
                    break;
                default:
                    gen::codelet256_stage1_fused_sh_cs(din2, tb, n2, plan.sh_twr, plan.sh_twi);
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
    }

    // BATCHED FFT: `batch` independent size-plan.n transforms in ELEMENT-MAJOR layout (element i of transform t
    // at data[i*batch + t]), in place. Radix-2 DIT, vectorized over the CONTIGUOUS batch axis (Vec4d over t
    // via deinterleave) — the efficiency the per-row execute() lacked: ONE kernel, zero per-transform
    // deinterleave overhead, the batch provides the SIMD width with NO register spill. This is the v10-e
    // batched primitive AND the sub-FFT engine for the block four-step/six-step (the large-N block technique).
    // Same butterfly + twiddle convention as execute_reference (the oracle), so cross-checkable.
    void execute_batched(Complex<T>* data, crd::usize batch, FftDirection dir) const
    {
        const crd::usize b = batch;
        if (plan.n <= 1)
        {
            return;
        }
        Complex<T>* d = data;
        // f64 N=8, even batch: the AoS over-2 codelet is register-resident with no per-pass memory traffic (the
        // SoA path below pays log₂(n) passes). Adjacent transforms (t,t+1) are contiguous in element-major layout
        // = the over-2 vector. four-step never calls this with n≤32 (it splits n≥2¹⁹ into ~2⁹ factors), so this
        // fast path is reached only by direct small-N batched callers (v10-e). N=32 batched stays on the SoA path.
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            if (plan.n == 8 && (b & 1U) == 0U)
            {
                detail::small_n_batched8_f64(reinterpret_cast<crd::f64*>(d), b, dir == FftDirection::Forward);
                return;
            }
            // N=16 even-batch: the over-2 register-resident codelet, but only while the working set stays
            // cache-resident — the strided over-2 gather streams ~n·b complex per transform-pair, so beyond
            // batch_cache_budget the strided access thrashes L1 and the SoA path below wins. (N=8 has 8 strided
            // loads/pair and stays within L1 associativity for any batch, so it needs no such guard.)
            constexpr crd::usize batch_cache_budget = 2048; // n·b complex (~32 KiB f64) — measured cache-resident
            if (plan.n == 16 && (b & 1U) == 0U && plan.n * b <= batch_cache_budget)
            {
                detail::small_n_batched16_f64(reinterpret_cast<crd::f64*>(d), b, dir == FftDirection::Forward);
                return;
            }
        }
        // The hierarchical generated-codelet sub-FFTs replace the
        // radix-8 path for the f64 four-step sub-FFTs — 4096 = 64×64 (b==16: 8M-n1 + both 16M), 2048 = 64×32
        // (b==32: 8M-n2 + both 4M), 1024 = 32×32 (b==64: M5, 2M-n2 + both 1M). Stage-1 FUSED (split-radix leaf +
        // inner twiddle + transposed store into plan.hier_bbuf) then a plain leaf stage-2 back into d (natural order).
        // Isolated 4096 1.24× / 2048 1.26× / 1024 1.23×. Full f64 vs MKL (CANONICAL bench_fft_vs_refs best-of-reps):
        // 4M ~1.04× (beats), 8M ~0.91×, 16M ~0.90×, 2M ~0.75×, 1M ~0.60× — the hier improves every size vs radix-8
        // (e.g. 1M 0.53→0.60, 2M 0.68→0.75) but MKL is very fast at the small cache-resident sizes. machine-eps
        // (~1e-15), deterministic. M7: f32 FORWARD also uses the hier (Vec8f, same decompositions, BB=block_width;
        // f32 8M 0.79→~1.0× MKL parity band, ~3e-7). Inverse (f32+f64) falls through to radix-8. See the
        // 2026-06-17 FFT logs. f64 + f32 (M9-FIXED: an M7 indentation miss had left the f32 dispatch off while the
        // ctor allocated the hier buffers). Stage b's = N2·BB (stage-1) / N1·BB
        // (stage-2), BB = block_width<T>(plan.n) (T-aware ⇒ f64 reproduces the old 1024/2048 literals exactly).
        constexpr bool k_hier_d = std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>;
        if constexpr (k_hier_d)
        {
            if (dir == FftDirection::Forward && plan.hier_bbuf != nullptr)
            {
                if (plan.n == 4096 && b == block_width<T>(4096)) // 64×64; stage-1 b=64·BB, stage-2 b=64·BB
                {
                    gen::codelet64_stage1_fused_64x64(d, plan.hier_bbuf, 64 * b, plan.hier_twr, plan.hier_twi);
                    gen::codelet64_batched(plan.hier_bbuf, d, 64 * b);
                    return;
                }
                if (plan.n == 2048 && b == block_width<T>(2048)) // 64×32; stage-1 b=32·BB, stage-2 b=64·BB
                {
                    gen::codelet64_stage1_fused_64x32(d, plan.hier_bbuf, 32 * b, plan.hier_twr, plan.hier_twi);
                    gen::codelet32_batched(plan.hier_bbuf, d, 64 * b);
                    return;
                }
                if (plan.n == 1024 && b == block_width<T>(1024)) // 32×32; both stages b=32·BB
                {
                    gen::codelet32_stage1_fused_32x32(d, plan.hier_bbuf, 32 * b, plan.hier_twr, plan.hier_twi);
                    gen::codelet32_batched(plan.hier_bbuf, d, 32 * b);
                    return;
                }
                // 256 = 16×16 hier sub-FFT (256K four-step P2); f32 only (the codelet16 stage-1 is Vec8f).
                if constexpr (std::is_same_v<T, crd::f32>)
                {
                    if (plan.n == 256 && b == block_width<T>(256)) // both stages b=16·BB
                    {
                        gen::codelet16_stage1_fused_16x16(d, plan.hier_bbuf, 16 * b, plan.hier_twr, plan.hier_twi);
                        gen::codelet16_batched(plan.hier_bbuf, d, 16 * b);
                        return;
                    }
                }
            }
        }
        // FFT-CRUSH 2026-07-03 (batched-LEAF gate): n in {16,32,64} batched forward goes straight to the
        // generated split-radix leaf codelets (the hier stage-2 kernels, both types) instead of
        // bit-reversal + batched radix-8. IN-PLACE-safe (verified: every generated tile loads all inputs
        // before its first store) and tail-free (b stays a power of two and whole tiles of codelet_lanes.hpp here,
        // so every level picks this path at the same sizes). This is what makes the all-hier four-step splits
        // (128K = 2048x64, 256K = 4096x64) fast on the P2 side.
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            constexpr crd::usize kw = std::is_same_v<T, crd::f32> ? 8U : 4U; // the tile, 32 bytes
            if (dir == FftDirection::Forward && (b % kw) == 0U && b >= kw)
            {
                if (plan.n == 256)
                {
                    gen::codelet256_batched(d, d, b); // GENERATED 2026-07-03 (rebuilt generator; f64 + f32)
                    return;
                }
                if (plan.n == 128)
                {
                    gen::codelet128_batched(d, d, b); // GENERATED 2026-07-03 (rebuilt generator; f64 + f32)
                    return;
                }
                if (plan.n == 64)
                {
                    gen::codelet64_batched(d, d, b);
                    return;
                }
                if (plan.n == 32)
                {
                    gen::codelet32_batched(d, d, b);
                    return;
                }
                if (plan.n == 16)
                {
                    gen::codelet16_batched(d, d, b);
                    return;
                }
            }
        }
        for (crd::usize i = 0; i < plan.n; ++i) // bit-reversal on the element dim (swap whole rows of `b`)
        {
            const crd::usize j = plan.rev[i];
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
        const crd::u32 rem = plan.log2 % 3U;
        if (rem == 1U) // one radix-2 stage (1→2)
        {
            const crd::usize step = plan.n >> 1;
            for (crd::usize base = 0; base < plan.n; base += 2)
            {
                batched_butterfly2(d, b, base * b, (base + 1) * b, plan.tw_re[0], isign * plan.tw_im[0]);
            }
            (void)step;
            len = 2;
        }
        else if (rem == 2U) // one radix-4 stage (1→4): q=1 ⇒ ws=W_2^0=1, w0=W_4^0=1, w1=W_4^1
        {
            const crd::usize tq = plan.n >> 2;
            for (crd::usize base = 0; base < plan.n; base += 4)
            {
                batched_butterfly4(d, b, base * b, (base + 1) * b, (base + 2) * b, (base + 3) * b, plan.tw_re[0],
                                   isign * plan.tw_im[0], plan.tw_re[0], isign * plan.tw_im[0], plan.tw_re[tq],
                                   isign * plan.tw_im[tq]);
            }
            len = 4;
        }
        for (; len < plan.n; len <<= 3) // radix-8 stages: q = len, builds 8·len
        {
            const crd::usize q = len;
            const crd::usize step = plan.n / (len << 3);
            const crd::usize n4 = plan.n >> 2;
            const crd::usize n8 = plan.n >> 3;
            const crd::usize n38 = (3 * plan.n) >> 3;
            for (crd::usize base = 0; base < plan.n; base += (len << 3))
            {
                crd::usize tw = 0;
                for (crd::usize k = 0; k < q; ++k)
                {
                    // Nested radix-8 DIT twiddles: ws=W_{2q}^k; w0=W_{4q}^k, w1=W_{4q}^{k+q};
                    // v0=W_{8q}^k, v1=W_{8q}^{k+q}, v2=W_{8q}^{k+2q}, v3=W_{8q}^{k+3q}.
                    const T wsr = plan.tw_re[4 * tw];
                    const T wsi = isign * plan.tw_im[4 * tw];
                    const T w0r = plan.tw_re[2 * tw];
                    const T w0i = isign * plan.tw_im[2 * tw];
                    const T w1r = plan.tw_re[2 * tw + n4];
                    const T w1i = isign * plan.tw_im[2 * tw + n4];
                    const T v0r = plan.tw_re[tw];
                    const T v0i = isign * plan.tw_im[tw];
                    const T v1r = plan.tw_re[tw + n8];
                    const T v1i = isign * plan.tw_im[tw + n8];
                    const T v2r = plan.tw_re[tw + n4];
                    const T v2i = isign * plan.tw_im[tw + n4];
                    const T v3r = plan.tw_re[tw + n38];
                    const T v3i = isign * plan.tw_im[tw + n38];
                    const crd::usize p = (base + k) * b;
                    batched_butterfly8(d, b, p, p + q * b, p + 2 * q * b, p + 3 * q * b, p + 4 * q * b, p + 5 * q * b,
                                       p + 6 * q * b, p + 7 * q * b, wsr, wsi, w0r, w0i, w1r, w1i, v0r, v0i, v1r, v1i,
                                       v2r, v2i, v3r, v3i);
                    tw += step;
                }
            }
        }
    }

    // One radix-2 batched butterfly over the contiguous batch axis (whole tiles of transforms a step; scalar tail).
    static void batched_butterfly2(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, T wr, T wi) noexcept
    {
        Complex<T>* arow = d + o0;
        Complex<T>* brow = d + o1;
        // SoA SIMD over the batch, a transform a lane; the complex deinterleave/interleave helpers are pure shuffles
        // (bit-identical to the scalar tail).
        const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
        {
            const V wrv(wr);
            const V wiv(wi);
            for (crd::usize t = first; t < last; t += kCols<T, V>)
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
        };
        for (crd::usize t = for_tiles<T>(b, step); t < b; ++t)
        {
            const T ar = arow[t].re;
            const T ai = arow[t].im;
            const T vr = wr * brow[t].re - wi * brow[t].im;
            const T vi = wr * brow[t].im + wi * brow[t].re;
            arow[t].re = ar + vr;
            arow[t].im = ai + vi;
            brow[t].re = ar - vr;
            brow[t].im = ai - vi;
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
    static void
    batched_butterfly4(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, crd::usize o2, crd::usize o3, T wsr,
                       T wsi, T w0r, T w0i, T w1r, T w1i) noexcept
    {
        Complex<T>* r0 = d + o0;
        Complex<T>* r1 = d + o1;
        Complex<T>* r2 = d + o2;
        Complex<T>* r3 = d + o3;
        const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
        {
            const V wsrv(wsr);
            const V wsiv(wsi);
            const V w0rv(w0r);
            const V w0iv(w0i);
            const V w1rv(w1r);
            const V w1iv(w1i);
            for (crd::usize t = first; t < last; t += kCols<T, V>)
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
        };
        for (crd::usize t = for_tiles<T>(b, step); t < b; ++t)
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
            r0[t].re = e0r + g0r;
            r0[t].im = e0i + g0i;
            r1[t].re = e1r + g1r;
            r1[t].im = e1i + g1i;
            r2[t].re = e0r - g0r;
            r2[t].im = e0i - g0i;
            r3[t].re = e1r - g1r;
            r3[t].im = e1i - g1i;
        }
    }

    // One radix-8 batched DIT butterfly, BIT-REVERSED input — three fused radix-2 stages: A) ws=W_{2q}^k on
    // pairs (0,1)(2,3)(4,5)(6,7); B) w0=W_{4q}^k on (0,2)(4,6), w1=W_{4q}^{k+q} on (1,3)(5,7); C) v0..v3=
    // W_{8q}^{k,k+q,k+2q,k+3q} on (0,4)(1,5)(2,6)(3,7). In place; twiddles carry isign. Whole tiles over the batch
    // + scalar tail (the 8-pt waist = 16 ymm at peak ⇒ some spill to L1, cheap on the L2-resident block).
#if defined(_MSC_VER) && !defined(__clang__)
    // Same MSVC LTCG heap-exhaustion guard as batched_butterfly4: keep this radix-8 SIMD codelet out-of-line so
    // the optimizer does not stack it onto the radix driver (C1001). Batch-amortized call -> perf-neutral.
    __declspec(noinline)
#endif
    static void
    batched_butterfly8(Complex<T>* d, crd::usize b, crd::usize o0, crd::usize o1, crd::usize o2, crd::usize o3,
                       crd::usize o4, crd::usize o5, crd::usize o6, crd::usize o7, T wsr, T wsi, T w0r, T w0i, T w1r,
                       T w1i, T v0r, T v0i, T v1r, T v1i, T v2r, T v2i, T v3r, T v3i) noexcept
    {
        Complex<T>* r0 = d + o0;
        Complex<T>* r1 = d + o1;
        Complex<T>* r2 = d + o2;
        Complex<T>* r3 = d + o3;
        Complex<T>* r4 = d + o4;
        Complex<T>* r5 = d + o5;
        Complex<T>* r6 = d + o6;
        Complex<T>* r7 = d + o7;
        const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
        {
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
            for (crd::usize t = first; t < last; t += kCols<T, V>)
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
        };
        for (crd::usize t = for_tiles<T>(b, step); t < b; ++t)
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
            r0[t].re = c0r + n4r;
            r0[t].im = c0i + n4i;
            r1[t].re = c1r + n5r;
            r1[t].im = c1i + n5i;
            r2[t].re = c2r + n6r;
            r2[t].im = c2i + n6i;
            r3[t].re = c3r + n7r;
            r3[t].im = c3i + n7i;
            r4[t].re = c0r - n4r;
            r4[t].im = c0i - n4i;
            r5[t].re = c1r - n5r;
            r5[t].im = c1i - n5i;
            r6[t].re = c2r - n6r;
            r6[t].im = c2i - n6i;
            r7[t].re = c3r - n7r;
            r7[t].im = c3i - n7i;
        }
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

    // execute_ip4aos' fold output: the u-th complex pair of y to ob[u] + off.
    template <typename W> CRD_FORCEINLINE static void store_units(Complex<T>* const* ob, crd::usize off, W y) noexcept
    {
        constexpr crd::usize units = kCols<T, W> / 4;
        if constexpr (units == 1)
        {
            y.store(reinterpret_cast<T*>(ob[0] + off));
        }
        else if constexpr (units == 2)
        {
            simd::store_c_lo(reinterpret_cast<T*>(ob[0] + off), y);
            simd::store_c_hi(reinterpret_cast<T*>(ob[1] + off), y);
        }
        else
        {
            store_units(ob, off, low_half(y));
            store_units(ob + units / 2, off, high_half(y));
        }
    }

    // execute_ip4aos' staging copy of count T, one W a step.
    template <typename W> CRD_FORCEINLINE static void stage(const T* src, T* dst, crd::usize count) noexcept
    {
        for (crd::usize o = 0; o < count; o += kCols<T, W>)
        {
            W::load(src + o).store(dst + o);
        }
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
    // t_p = W_n^{p·k2}·X_p (p=1,2,3), then the radix-4 butterfly → Y[k1]=out[k1·m+k2]. SIMD over k2 (f64, whole
    // tiles); the per-k2 combine-twiddles W_n^{p·k2}=plan.tw[p·k2·(plan.n/n)] are strided gathers. Same butterfly +
    // sign convention as radix4_row (verified). Element-independent over k2 ⇒ moat holds.
    void radix4_combine_inplace(T* outr, T* outi, crd::usize m, crd::usize n, FftDirection dir) const
    {
        const crd::usize ts = plan.n / n; // W_n^idx = plan.tw[idx·ts]
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        crd::usize tail = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
            {
                const V sg(isign);
                for (crd::usize k2 = first; k2 < last; k2 += kCols<T, V>)
                {
                    const V x0r = V::load(outr + k2);
                    const V x0i = V::load(outi + k2);
                    const V x1r = V::load(outr + m + k2);
                    const V x1i = V::load(outi + m + k2);
                    const V x2r = V::load(outr + 2 * m + k2);
                    const V x2i = V::load(outi + 2 * m + k2);
                    const V x3r = V::load(outr + 3 * m + k2);
                    const V x3i = V::load(outi + 3 * m + k2);
                    const auto tw = [&](const T* table, crd::usize p) noexcept
                    {
                        const auto at = [&](crd::usize c) noexcept
                        {
                            return table[p * (k2 + c) * ts];
                        };
                        return gather_f64<V>(at);
                    };
                    const V w1r = tw(plan.tw_re, 1);
                    const V w1i = tw(plan.tw_im, 1) * sg;
                    const V w2r = tw(plan.tw_re, 2);
                    const V w2i = tw(plan.tw_im, 2) * sg;
                    const V w3r = tw(plan.tw_re, 3);
                    const V w3i = tw(plan.tw_im, 3) * sg;
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
            };
            tail = for_tiles<T>(m, step);
        }
        for (crd::usize k2 = tail; k2 < m; ++k2)
        {
            const crd::usize a = k2 * ts;
            const T w1r = plan.tw_re[a];
            const T w1i = isign * plan.tw_im[a];
            const T w2r = plan.tw_re[2 * a];
            const T w2i = isign * plan.tw_im[2 * a];
            const T w3r = plan.tw_re[3 * a];
            const T w3i = isign * plan.tw_im[3 * a];
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
    static constexpr crd::usize kGatherPf = 8; // four-step gather prefetch distance (rows ahead; probe-tuned)

    // Dispatch the in-place leaf transform to its generated straight-line codelet (plan.n ∈ {2,4,8,16,32}).
    void dispatch_codelet(T* re, T* im, FftDirection dir) const
    {
        const bool fwd = (dir == FftDirection::Forward);
        switch (plan.n)
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
                break; // unreachable (plan.n is a power of two in [2,32])
        }
    }

    // Block four-step RESURRECTION (n = n1·n2): cache-resident BATCHED sub-FFTs + NON-TEMPORAL-STORE scatter
    // (the 25.7 GB/s transpose lever). Phase 1 = n2 column FFTs (length n1) + inter-stage twiddle, scattered to
    // plan.tbuf with NT stores; phase 2 = n1 row FFTs (length n2), scattered to data with NT stores → natural
    // order. Gather = block-contiguous (memcpy); the implicit transpose is fused into gather+NT-scatter.
    //   X[k1 + n1·k2] = Σ_{i2} W_n^{k1·i2} · W_{n2}^{i2·k2} · ( Σ_{i1} x[i1·n2 + i2] · W_{n1}^{i1·k1} ).

    // SIX-STEP FFT (Takahashi 2019 §6.3 / Bailey; built 2026-07-04) — the CACHE-machine framework,
    // SQUARE splits only (n = m·m): every FFT runs on CONTIGUOUS data (the L1-resident standalone
    // 2-pass engine, 30-39 GF/s) and every transpose is IN-PLACE tile-pairwise — six passes, zero
    // scratch copies, natural-order output in the caller's buffer. The step-3 twiddle w_n^{j1·k2}
    // fuses into the middle transpose via the four-step's FACTORED hi/lo tables (no n-sized stream).
    // This replaces the four-step's stride-bound gather/scatter phases; the five-step's strided
    // multirow failure (stream-count wall) is what this variant structurally avoids.
    // v2 (2026-07-04): SIMD micro-tile pair-swap transpose — square complex micro-tiles moved via
    // deinterleaved loads + per-plane register transpose + interleaved CONTIGUOUS stores on BOTH
    // halves (the v14-d tensor-permute discipline; v1's element-scalar swaps were the measured
    // wall). The fused T2 twiddle W_n^{r·c} evolves by per-row recurrence along c (step W_n^{r·tile}),
    // reseeded from the FACTORED hi/lo tables at every kTb block (≤16 steps — the proven four-step
    // reseed class). Symmetry w(r,c)=w(c,r) twiddles the U-half via the transposed twiddle tile.
    // A micro-tile is kCols complex square: one tile, or at avx512 a 2 by 2 block of tiles moved as one. Every element
    // keeps the one-tile pair-swap's twiddle: row p's recurrence along the columns, one step a tile, and on a diagonal
    // micro-tile below its tile diagonal the transposed element's (row q at column p), so every level gives the same
    // bits.
    void fs6_transpose_inplace(Complex<T>* a, crd::usize m, bool twiddle) const
    {
        using V = WideVec<T>;
        using H = TileVec<T>;
        constexpr crd::usize lanes = kCols<T, V>;
        constexpr crd::usize tile = kCols<T, H>;
        constexpr crd::usize steps = lanes / tile; // recurrence steps a micro-tile
        // block so the tile-PAIR footprint (2 · ktb rows · m·sizeof(Complex<T>) row stride) stays
        // L2-resident: 64 at m=1024 (1 MB pair), 32 at m>=2048 (4M measured −13% with 64: 2 MB pair).
        const crd::usize ktb = (m >= 2048) ? 32U : 64U;
        static_assert(32U % lanes == 0U, "a block holds whole micro-tiles");
        const crd::usize mmask = (crd::usize{1} << plan.ftw_h) - 1;
        const T* const hir = plan.ftw_hi_re;
        const T* const hii = plan.ftw_hi_im;
        const T* const lor = plan.ftw_lo_re;
        const T* const loi = plan.ftw_lo_im;
        const auto tw_of = [&](crd::usize r, crd::usize c, T& wre, T& wim) noexcept
        {
            const crd::usize aa = r * c; // < n (r,c < m, m·m == n)
            const T hr = hir[aa >> plan.ftw_h];
            const T hi = hii[aa >> plan.ftw_h];
            const T lr = lor[aa & mmask];
            const T li = loi[aa & mmask];
            wre = hr * lr - hi * li; // W_n^{r·c} (cos, −sin)
            wim = hr * li + hi * lr;
        };
        // One recurrence step of every tile of a row's twiddle state: w <- w · W_n^{row·tile}.
        const auto advance = [](auto& wr, auto& wi, auto sr, auto si) noexcept
        {
            const auto nr = simd::fnmadd(wi, si, wr * sr);
            const auto ni = simd::fma(wi, sr, wr * si);
            wr = nr;
            wi = ni;
        };
        const auto tp = [](V(&xr)[lanes], V(&xi)[lanes]) noexcept
        {
            transpose_rows(xr);
            transpose_rows(xi);
        };
        const auto twiddle_by = [](V& xr, V& xi, V wr, V wi) noexcept
        {
            const V yr = simd::fnmadd(xi, wi, xr * wr);
            const V yi = simd::fma(xr, wi, xi * wr);
            xr = yr;
            xi = yi;
        };
        for (crd::usize i0 = 0; i0 < m; i0 += ktb)
        {
            for (crd::usize j0 = i0; j0 < m; j0 += ktb)
            {
                for (crd::usize r = i0; r < i0 + ktb; r += lanes)
                {
                    // per-row twiddle state for rows r..r+lanes, the micro-tile's tiles at columns j0.. (recurrence
                    // along c): a tile's state is its predecessor's advanced one step
                    V wr[lanes];
                    V wi[lanes];
                    V sr[lanes];
                    V si[lanes];
                    if (twiddle)
                    {
                        for (crd::usize i = 0; i < lanes; ++i)
                        {
                            alignas(64) T row_r[tile];
                            alignas(64) T row_i[tile];
                            for (crd::usize j = 0; j < tile; ++j)
                            {
                                tw_of(r + i, j0 + j, row_r[j], row_i[j]);
                            }
                            T step_r;
                            T step_i;
                            tw_of(r + i, tile, step_r, step_i); // step W_n^{(r+i)·tile} — exact table product
                            sr[i] = V(step_r);
                            si[i] = V(step_i);
                            H lo_r = H::load(row_r);
                            H lo_i = H::load(row_i);
                            if constexpr (steps == 1U)
                            {
                                wr[i] = lo_r;
                                wi[i] = lo_i;
                            }
                            else
                            {
                                H hi_r = lo_r;
                                H hi_i = lo_i;
                                advance(hi_r, hi_i, H(step_r), H(step_i));
                                wr[i] = V(lo_r, hi_r);
                                wi[i] = V(lo_i, hi_i);
                            }
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
                                for (crd::usize k = 0; k < steps; ++k)
                                {
                                    advance(wr[i], wi[i], sr[i], si[i]);
                                }
                            }
                        }
                    }
                    for (crd::usize c = cbeg; c < j0 + ktb; c += lanes)
                    {
                        V ur[lanes];
                        V ui[lanes];
                        for (crd::usize i = 0; i < lanes; ++i)
                        {
                            simd::load_complex_deinterleaved(reinterpret_cast<const T*>(a + (r + i) * m + c), ur[i],
                                                             ui[i]);
                        }
                        // the TRANSPOSED twiddle micro-tile: twt[j] lane i = w(r+i, c+j) of row r+i's recurrence.
                        // Each use declares and fills its own, so no path reads a micro-tile it did not build.
                        const auto transposed_state = [&](V(&tr)[lanes], V(&ti)[lanes]) noexcept
                        {
                            for (crd::usize i = 0; i < lanes; ++i)
                            {
                                tr[i] = wr[i];
                                ti[i] = wi[i];
                            }
                            tp(tr, ti);
                        };
                        if (r == c) // diagonal micro-tile: transpose + twiddle in place
                        {
                            tp(ur, ui);
                            if (twiddle)
                            {
                                if constexpr (steps == 1U)
                                {
                                    for (crd::usize i = 0; i < lanes; ++i)
                                    {
                                        twiddle_by(ur[i], ui[i], wr[i], wi[i]);
                                    }
                                }
                                else
                                {
                                    // Row tile 0 takes its own recurrence on both tiles; row tile 1 the transposed
                                    // element's on tile 0 and its own on tile 1. The halves exist only on the
                                    // two-tile types: found at instantiation.
                                    V twtr[lanes];
                                    V twti[lanes];
                                    transposed_state(twtr, twti);
                                    for (crd::usize i = 0; i < lanes; ++i)
                                    {
                                        const bool own = i < tile;
                                        const V tr = own ? wr[i] : V(low_half(twtr[i]), high_half(wr[i]));
                                        const V ti = own ? wi[i] : V(low_half(twti[i]), high_half(wi[i]));
                                        twiddle_by(ur[i], ui[i], tr, ti);
                                    }
                                }
                            }
                            for (crd::usize i = 0; i < lanes; ++i)
                            {
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
                                simd::load_complex_deinterleaved(reinterpret_cast<const T*>(a + (c + j) * m + r), vr[j],
                                                                 vi[j]);
                            }
                            tp(ur, ui); // Ut row j = destination row (c+j) content across r..r+lanes
                            tp(vr, vi); // Vt row i = destination row (r+i) content across c..c+lanes
                            if (twiddle)
                            {
                                V twtr[lanes];
                                V twti[lanes];
                                transposed_state(twtr, twti);
                                for (crd::usize i = 0; i < lanes; ++i)
                                {
                                    twiddle_by(vr[i], vi[i], wr[i], wi[i]);     // V-half: the recurrence rows directly
                                    twiddle_by(ur[i], ui[i], twtr[i], twti[i]); // U-half: w(c+j, r+i) = twt[j][i]
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
                                for (crd::usize k = 0; k < steps; ++k)
                                {
                                    advance(wr[i], wi[i], sr[i], si[i]);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // The tiled four-step of the f32 1M forward (FftAlgorithm::TiledFourStep, gen_fft_batched.py's tiled
    // emitters): P1 takes each block of 128 columns through its two leaves into the tiled intermediate, P2 each block
    // of 16 k1 through its two leaves into the output; a block's stage sits in the scratch, in L2.
    void execute_tiled_four_step(Complex<T>* data) const
    {
        T* const stage = reinterpret_cast<T*>(plan.scratch);
        for (crd::usize blk = 0; blk < 8; ++blk)
        {
            gen::codelet32_tiled_p1_leaf(data, stage, blk, plan.tf_twr, plan.tf_twi);
            gen::codelet32_tiled_p1_corner(stage, plan.tf_nt, blk);
        }
        for (crd::usize k0 = 0; k0 < 1024; k0 += 16)
        {
            gen::codelet32_tiled_p2_leaf(plan.tf_nt, stage, k0, plan.tf_bsr, plan.tf_bsi, plan.tf_lnr, plan.tf_lni,
                                         plan.tf_twr, plan.tf_twi);
            gen::codelet32_tiled_p2_final(stage, data, k0);
        }
    }

    void execute_six_step(Complex<T>* data) const
    {
        const crd::usize m = plan.n1; // square: n = m·m; row FFTs via the hoisted sub-plan
        Complex<T>* const x = data;
        // ALL-IN-PLACE chain (re-verified 2026-07-04): at 1M the whole array is L3-RESIDENT — the
        // in-place transposes keep every pass L3-hot. An out-of-place rechain MEASURED WORSE everywhere
        // (1M 5.38→6.69 ms, 4M 33.5→40.4): the x↔t ping-pong doubles the live footprint and thrashes L3.
        fs6_transpose_inplace(x, m, false); // T1
        for (crd::usize r = 0; r < m; ++r)  // sweep A: m contiguous m-point FFTs
        {
            Engine<T>{*plan.p1}.execute(x + r * m, FftDirection::Forward);
        }
        fs6_transpose_inplace(x, m, true); // T2 fused with w_n^{j1·k2}
        for (crd::usize r = 0; r < m; ++r) // sweep B
        {
            Engine<T>{*plan.p1}.execute(x + r * m, FftDirection::Forward);
        }
        fs6_transpose_inplace(x, m, false); // T3 -> natural order
    }

    // IP4-AoS (PROMOTED): the INTERLEAVED in-place radix-4 DIT engine, f64 1K..64K and f32 2K and
    // 16K..64K, both parities and both DIRECTIONS. INV = inverse: conjugated twiddles throughout — folded into the
    // fold's sgn vector (zero extra ops: only t.hi feeds mix_lo_hi), compile-time-negated fold constants, an X1/X3 swap
    // in bf4, and negate-at-load for table twiddles (measured free, round 15). Design record:
    // docs/research/fft-stockham-v2.md rounds 1-17.
    template <bool INV>
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC LTCG inlines both instantiations into execute(), stacking this ~600-line SIMD driver
    // on top of the codelet mass there — part of the C1002 pass-2 heap exhaustion (2026-07-05).
    // A call at n >= 1024 is noise. gcc/clang keep their own inlining choice (measured baseline).
    __declspec(noinline)
#endif
    void
    execute_ip4aos(Complex<T>* data) const
    {
        constexpr bool is_f32 = std::is_same_v<T, crd::f32>;
        // Every column, fold unit and twiddle is its own lane chain, so a wider vector only packs more of them: at
        // avx512 each step and fold call takes two tiles' worth on WideVec, with every lane's operations unchanged.
        using V = WideVec<T>;
        using H = TileVec<T>;
        constexpr crd::usize per_vec = kCols<T, V> / 2; // complex per vector
        // A fold unit is a complex PAIR (128 bits of f32, 256 of f64): one per Vec4d, two per Vec8f or Vec8d (twin
        // units), four per Vec16f.
        constexpr crd::usize units = kCols<T, V> / 4;
        // 4-tuple constant builder: one value per T-lane over a complex PAIR, duplicated across every unit.
        const auto mkv = [](double a, double b, double c, double d) noexcept -> V
        {
            H h;
            if constexpr (is_f32)
            {
                h = H(static_cast<T>(a), static_cast<T>(b), static_cast<T>(c), static_cast<T>(d), static_cast<T>(a),
                      static_cast<T>(b), static_cast<T>(c), static_cast<T>(d));
            }
            else
            {
                h = H(a, b, c, d);
            }
            if constexpr (std::is_same_v<V, H>)
            {
                return h;
            }
            else
            {
                return V(h, h);
            }
        };
        constexpr double ns = INV ? -1.0 : 1.0; // conjugation sign (applied inside mkv, T-cast there)
        const crd::usize n = plan.n;
        const bool ipodd = (plan.log2 & 1U) != 0U; // n = 2·4^k: two half transforms + radix-2 combine
        const crd::usize nh = ipodd ? (n >> 1) : n;
        Complex<T>* const io = data;
        Complex<T>* const tb = plan.sh_t;
        // Slot map (round 10): 64B pad per 4KB. On 4K pages this measured neutral-to-worse (round
        // 9) because frame randomization already breaks set conflicts — but VTune named the 4K-page
        // cost (DTLB overhead = 24.5% of clockticks), and on 2MB pages (the DTLB fix) the pad is
        // ESSENTIAL: huge pages map virtual≡physical set bits, so unpadded power-of-2 strides hit
        // identical L2 sets. Pad + huge pages fix BOTH; either alone loses.
        const auto ps = [](crd::usize s) noexcept -> crd::usize
        {
            return s;
        };
        // radix-4 DIT butterfly on interleaved vectors (twiddles pre-dup'd, w applied to x1..x3):
        const auto bf4 = [](V& x0, V& x1, V& x2, V& x3, V w1r, V w1i, V w2r, V w2i, V w3r, V w3i) noexcept
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
            // fold-unit: 16 complex per unit from staged lbuf (group step gs, role-m step ms, unit u at
            // lbase + 2u) through len-4 (P-trick) + len-16 (constant twiddles) into 16 sequential slots
            // at ob[u]. A call takes `units` units, unit u in the vector's u-th complex pair.
            const auto fold16 = [&](const T* lbase, crd::usize gs, crd::usize ms, Complex<T>* const* ob) noexcept
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
                    if constexpr (units > 1)
                    {
                        ab = V::load_c_quad(l0, l0 + ms);
                        cd = V::load_c_quad(l0 + 2 * ms, l0 + 3 * ms);
                    }
                    else
                    {
                        ab = simd::load_pair128(l0, l0 + ms);
                        cd = simd::load_pair128(l0 + 2 * ms, l0 + 3 * ms);
                    }
                    const V u0 = ab + cd; // [t0, t2] per unit
                    const V u1 = ab - cd; // [t1, t3]
                    if constexpr (units > 1)
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
                    if constexpr (units > 1)
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
                store_units(ob, 0, y00);
                store_units(ob, 2, y01);
                store_units(ob, 4, y10);
                store_units(ob, 6, y11);
                store_units(ob, 8, y20);
                store_units(ob, 10, y21);
                store_units(ob, 12, y30);
                store_units(ob, 14, y31);
            };
            alignas(64) T lbuf[256];
            if (!ipodd)
            {
                for (crd::usize s = 0; s < nq; s += 16)
                {
                    const crd::usize j0 = plan.ip_rev[s];
                    for (crd::usize t = 0; t < 16; ++t) // line-complete loads → L1 staging (8 T)
                    {
                        stage<std::conditional_t<(kCols<T, V> <= 8), V, H>>(
                            reinterpret_cast<const T*>(io + j0 + t * n16), lbuf + 8 * t, 8);
                    }
                    for (crd::usize v = 0; v < 4; v += units) // unit v at lbuf + 2v
                    {
                        Complex<T>* ob[units];
                        for (crd::usize u = 0; u < units; ++u)
                        {
                            ob[u] = tb + ps(s + (v + u) * nq);
                        }
                        fold16(lbuf + 2 * v, 8, 32, ob);
                    }
                }
            }
            else
            {
                // odd log2: each io line holds 2 even + 2 odd complex — co-process BOTH halves'
                // units per stream (8 complex staged per stream; unit i = 2v + h at offset 2i).
                const crd::usize nqh = nh >> 2;
                for (crd::usize s = 0; s < nqh; s += 16)
                {
                    const crd::usize j0 = plan.ip_rev[s]; // = 2·rev(s)
                    for (crd::usize t = 0; t < 16; ++t)   // 16 T per stream
                    {
                        stage<V>(reinterpret_cast<const T*>(io + j0 + t * n16), lbuf + 16 * t, 16);
                    }
                    for (crd::usize i = 0; i < 8; i += units) // twin units pair (v, 0) with (v, 1)
                    {
                        Complex<T>* ob[units];
                        for (crd::usize u = 0; u < units; ++u)
                        {
                            const crd::usize j = i + u;
                            ob[u] = tb + ps(s + (j >> 1U) * nqh + (j & 1U) * nh);
                        }
                        fold16(lbuf + 2 * i, 16, 64, ob);
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
        const T* twr = plan.ip_twr + 12; // skip the len-16 3-set table (3·q = 12 entries): folded above
        const T* twi = plan.ip_twi + 12;
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
        const crd::u32 log2h = ipodd ? plan.log2 - 1U : plan.log2;
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
                const auto bf8 = [&](Complex<T>* const* p, crd::usize k, V w1r, V w1i, V w2r, V w2i, V w3r, V w3i,
                                     V w4r, V w4i, V w5r, V w5i, V w6r, V w6i, V w7r, V w7i) noexcept
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
                    if constexpr (INV)    // conjugated W8 diagonal
                    {
                        x3 = simd::addsub(x3, simd::swap_pairs(x3)) * w8r2; // (1+i)/√2 · O1
                        x5 = simd::swap_pairs(x5) * (V::zero() - w8ni);     // +i · O2
                        const V q3 = simd::addsub(simd::swap_pairs(x7), x7);
                        x7 = simd::swap_pairs(q3) * (V::zero() - w8r2); // −(1−i)/√2 · O3
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
                        const V w1r = V::load_dup_pairs(twr + k);
                        const V w2r = V::load_dup_pairs(twr + q + k);
                        const V w3r = V::load_dup_pairs(twr + 2 * q + k);
                        const V w4r = V::load_dup_pairs(twr + 3 * q + k);
                        const V w5r = V::load_dup_pairs(twr + 4 * q + k);
                        const V w6r = V::load_dup_pairs(twr + 5 * q + k);
                        const V w7r = V::load_dup_pairs(twr + 6 * q + k);
                        V w1i = V::load_dup_pairs(twi + k);
                        V w2i = V::load_dup_pairs(twi + q + k);
                        V w3i = V::load_dup_pairs(twi + 2 * q + k);
                        V w4i = V::load_dup_pairs(twi + 3 * q + k);
                        V w5i = V::load_dup_pairs(twi + 4 * q + k);
                        V w6i = V::load_dup_pairs(twi + 5 * q + k);
                        V w7i = V::load_dup_pairs(twi + 6 * q + k);
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
                            V w1r = V::load_dup_pairs(t1r + k);
                            V w1i = V::load_dup_pairs(t1i + k);
                            V u1r = V::load_dup_pairs(t1r + k + per_vec);
                            V u1i = V::load_dup_pairs(t1i + k + per_vec);
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
                                w2r = V::load_dup_pairs(t2r + k);
                                w2i = V::load_dup_pairs(t2i + k);
                                w3r = V::load_dup_pairs(t3r + k);
                                w3i = V::load_dup_pairs(t3i + k);
                                u2r = V::load_dup_pairs(t2r + k + per_vec);
                                u2i = V::load_dup_pairs(t2i + k + per_vec);
                                u3r = V::load_dup_pairs(t3r + k + per_vec);
                                u3i = V::load_dup_pairs(t3i + k + per_vec);
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
                            V w1r = V::load_dup_pairs(t1r + k);
                            V w1i = V::load_dup_pairs(t1i + k);
                            V u1r = V::load_dup_pairs(t1r + k + per_vec);
                            V u1i = V::load_dup_pairs(t1i + k + per_vec);
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
                                w2r = V::load_dup_pairs(t1r + q + k);
                                w2i = V::load_dup_pairs(t1i + q + k);
                                w3r = V::load_dup_pairs(t1r + 2 * q + k);
                                w3i = V::load_dup_pairs(t1i + 2 * q + k);
                                u2r = V::load_dup_pairs(t1r + q + k + per_vec);
                                u2i = V::load_dup_pairs(t1i + q + k + per_vec);
                                u3r = V::load_dup_pairs(t1r + 2 * q + k + per_vec);
                                u3i = V::load_dup_pairs(t1i + 2 * q + k + per_vec);
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
                const V w0r = V::load_dup_pairs(twr + j);
                const V w1r = V::load_dup_pairs(twr + j + per_vec);
                V w0i = V::load_dup_pairs(twi + j);
                V w1i = V::load_dup_pairs(twi + j + per_vec);
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

    void execute_four_step(Complex<T>* data, FftDirection dir) const
    {
        const crd::usize n = plan.n;
        const crd::usize n1 = plan.n1;
        const crd::usize n2 = n / n1;
        const T isign = (dir == FftDirection::Inverse) ? static_cast<T>(-1) : static_cast<T>(1);
        Complex<T>* const tbuf = plan.tbuf;
        Complex<T>* const scratch = plan.scratch;
        Complex<T>* const din = data;
        const crd::usize b1 = block_width<T>(n1);
        const crd::usize b2 = block_width<T>(n2);

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
                if (dir == FftDirection::Forward && n1 == 1024 && bw == 128 && plan.p1->hier_bbuf != nullptr)
                {
#ifdef CRD_FFT_PROFILE
                    const unsigned long long fp0 = prof::rdtsc();
#endif
                    gen::codelet32_stage1_fused_32x32_gather(din + i2, plan.p1->hier_bbuf, n2, plan.p1->hier_twr,
                                                             plan.p1->hier_twi);
#ifdef CRD_FFT_PROFILE
                    const unsigned long long fp1 = prof::rdtsc();
                    prof::g_p1_gather += fp1 - fp0; // M14: fused stage-1 (gather + leaf + inner twiddle)
#endif
                    gen::codelet32_batched(plan.p1->hier_bbuf, scratch, 32 * bw);
#ifdef CRD_FFT_PROFILE
                    prof::g_p1_sub += prof::rdtsc() - fp1; // M14: stage-2
#endif
                    m13_fused = true;
                }
            }
            else if constexpr (std::is_same_v<T, crd::f64>) // M13 f64 1M port (bw=block_width<T>(1024)=64 for f64)
            {
                if (dir == FftDirection::Forward && n1 == 1024 && bw == 64 && plan.p1->hier_bbuf != nullptr)
                {
                    gen::codelet32_stage1_fused_32x32_gather(din + i2, plan.p1->hier_bbuf, n2, plan.p1->hier_twr,
                                                             plan.p1->hier_twi);
                    gen::codelet32_batched(plan.p1->hier_bbuf, scratch, 32 * bw);
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
                        prefetch_row(din + (i1 + kGatherPf) * n2 + i2);
                    }
                    std::memcpy(scratch + i1 * bw, din + i1 * n2 + i2, bw * sizeof(Complex<T>));
                }
#ifdef CRD_FFT_PROFILE
                const unsigned long long pg1 = prof::rdtsc();
                prof::g_p1_gather += pg1 - pg0;
#endif
                Engine<T>{*plan.p1}.execute_batched(scratch, bw, dir);
#ifdef CRD_FFT_PROFILE
                prof::g_p1_sub += prof::rdtsc() - pg1;
#endif
            }
            // twiddle scratch[k1·bw+bb] *= W_n^{k1·col}; NT-store to tbuf row. The twiddle is FACTORED from two
            // ~√n L2-resident tables (no 128 MB streaming read): W_n^a = W_n^{a_hi·M}·W_n^{a_lo}, a = col·k1 < n.
            const crd::usize mmask = (crd::usize{1} << plan.ftw_h) - 1;
            const T* const hir = plan.ftw_hi_re;
            const T* const hii = plan.ftw_hi_im;
            const T* const lor = plan.ftw_lo_re;
            const T* const loi = plan.ftw_lo_im;
#ifdef CRD_FFT_PROFILE
            const unsigned long long pt0 = prof::rdtsc();
#endif
            // SIMD twiddle over the bb axis (whole tiles: a contiguous scratch read; the transposed tbuf write hits
            // L2-resident bands of a step's rows). Twiddle by recurrence (w*=W_n^col), reseeded from the factored
            // table every `period` steps to bound the drift (f32 ~1.5e-7, f64 ~1e-15, in class). Measured 2.15x on
            // the f32 twiddle phase and +5.6% on f64 8M (the twiddle ~1.5x).
            const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
            {
                constexpr crd::usize lanes = kCols<T, V>;
                constexpr crd::usize period = std::is_same_v<T, crd::f32> ? 8 : 32;
                for (crd::usize g = first; g < last; g += lanes)
                {
                    alignas(64) T wsr[lanes];
                    alignas(64) T wsi[lanes];
                    for (crd::usize l = 0; l < lanes; ++l)
                    {
                        const crd::usize c = i2 + g + l;
                        const crd::usize hh = c >> plan.ftw_h;
                        const crd::usize ll = c & mmask;
                        wsr[l] = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                        wsi[l] = isign * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                    }
                    const V wsrv = V::load_aligned(wsr);
                    const V wsiv = V::load_aligned(wsi);
                    V wr = V::zero();
                    V wi = V::zero();
                    // A micro-tile of `lanes` k1 steps a pass (n1 is a power of two of at least 32, so whole ones):
                    // on the whole-line levels step kk's vector holds the tile's columns at k1 = k0 + kk, and
                    // transposed in registers each column's run of k1 reaches its tbuf row as whole cache lines; the
                    // other levels store each step's elements into their rows at once.
                    for (crd::usize k0 = 0; k0 < n1; k0 += lanes)
                    {
                        [[maybe_unused]] V xr[lanes];
                        [[maybe_unused]] V xi[lanes];
                        for (crd::usize kk = 0; kk < lanes; ++kk)
                        {
                            const crd::usize k1 = k0 + kk;
                            if ((k1 & (period - 1)) == 0) // reseed from the exact factored table (bounds the drift)
                            {
                                alignas(64) T sr[lanes];
                                alignas(64) T si[lanes];
                                for (crd::usize l = 0; l < lanes; ++l)
                                {
                                    const crd::usize a = (i2 + g + l) * k1;
                                    const crd::usize hh = a >> plan.ftw_h;
                                    const crd::usize ll = a & mmask;
                                    sr[l] = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                                    si[l] = isign * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                                }
                                wr = V::load_aligned(sr);
                                wi = V::load_aligned(si);
                            }
                            V zr;
                            V zi;
                            simd::load_complex_deinterleaved(reinterpret_cast<const T*>(scratch + k1 * bw + g), zr,
                                                             zi);
                            if constexpr (kWholeLineTiles)
                            {
                                xr[kk] = zr * wr - zi * wi;
                                xi[kk] = zr * wi + zi * wr;
                            }
                            else
                            {
                                alignas(64) T orr[lanes];
                                alignas(64) T oii[lanes];
                                (zr * wr - zi * wi).store_aligned(orr);
                                (zr * wi + zi * wr).store_aligned(oii);
                                for (crd::usize l = 0; l < lanes; ++l)
                                {
                                    store_complex(tbuf + (i2 + g + l) * n1 + k1, orr[l], oii[l], true);
                                }
                            }
                            if ((k1 & (period - 1)) != period - 1) // advance recurrence: w *= W_n^col
                            {
                                const V nr = wr * wsrv - wi * wsiv;
                                const V ni = wr * wsiv + wi * wsrv;
                                wr = nr;
                                wi = ni;
                            }
                        }
                        if constexpr (kWholeLineTiles)
                        {
                            transpose_rows(xr);
                            transpose_rows(xi);
                            for (crd::usize l = 0; l < lanes; ++l)
                            {
                                store_tile_row(tbuf + (i2 + g + l) * n1 + k0, xr[l], xi[l]);
                            }
                        }
                    }
                }
            };
            // bw-tail (bw is a multiple of a tile at the four-step sizes, but keep correct)
            for (crd::usize g = for_tiles<T>(bw, step); g < bw; ++g)
            {
                const crd::usize col = i2 + g;
                Complex<T>* const trow = tbuf + col * n1;
                for (crd::usize k1 = 0; k1 < n1; ++k1)
                {
                    const crd::usize a = col * k1;
                    const crd::usize hh = a >> plan.ftw_h;
                    const crd::usize ll = a & mmask;
                    const T wr = hir[hh] * lor[ll] - hii[hh] * loi[ll];
                    const T wi = isign * (hir[hh] * loi[ll] + hii[hh] * lor[ll]);
                    const Complex<T> z = scratch[k1 * bw + g];
                    store_complex(trow + k1, z.re * wr - z.im * wi, z.re * wi + z.im * wr, true);
                }
            }
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
                if (dir == FftDirection::Forward && n2 == 1024 && bw == 128 && plan.p2->hier_bbuf != nullptr)
                {
                    gen::codelet32_stage1_fused_32x32_gather(tbuf + k1, plan.p2->hier_bbuf, n1, plan.p2->hier_twr,
                                                             plan.p2->hier_twi);
                    gen::codelet32_batched(plan.p2->hier_bbuf, scratch, 32 * bw);
                    m13_fused2 = true;
                }
                // 256K small-N fix (f32): 256-axis P2 = 16×16 hier, GATHER-fused (reads tbuf directly, rs=n1, no
                // memcpy round-trip) + SCATTER-fused (stage-2 writes the final output directly, no scratch pass).
                // Default-on for the 1024×256 four-step split → 256K ~0.85× MKL (3.6× CRD over direct Stockham).
                if (dir == FftDirection::Forward && n2 == 256 && bw == 512 && plan.p2->hier_bbuf != nullptr)
                {
                    gen::codelet16_stage1_fused_16x16_gather(tbuf + k1, plan.p2->hier_bbuf, n1, plan.p2->hier_twr,
                                                             plan.p2->hier_twi);
                    gen::codelet16_batched_scatter(plan.p2->hier_bbuf, din, k1, n1, 16 * bw);
                    m17_fused = true;
                    m13_fused2 = true;
                }
            }
            else if constexpr (std::is_same_v<T, crd::f64>) // M13 f64 1M port (bw=block_width<T>(1024)=64 for f64)
            {
                if (dir == FftDirection::Forward && n2 == 1024 && bw == 64 && plan.p2->hier_bbuf != nullptr)
                {
                    gen::codelet32_stage1_fused_32x32_gather(tbuf + k1, plan.p2->hier_bbuf, n1, plan.p2->hier_twr,
                                                             plan.p2->hier_twi);
                    gen::codelet32_batched(plan.p2->hier_bbuf, scratch, 32 * bw);
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
                        prefetch_row(tbuf + (i2 + kGatherPf) * n1 + k1);
                    }
                    std::memcpy(scratch + i2 * bw, tbuf + i2 * n1 + k1, bw * sizeof(Complex<T>));
                }
#ifdef CRD_FFT_PROFILE
                const unsigned long long qg1 = prof::rdtsc();
                prof::g_p2_gather += qg1 - qg0;
#endif
                Engine<T>{*plan.p2}.execute_batched(scratch, bw, dir);
#ifdef CRD_FFT_PROFILE
                qg2 = prof::rdtsc();
                prof::g_p2_sub += qg2 - qg1;
#endif
            }
            // X[(k1+bb)+n1·k2] = scratch[k2·bw+bb] → data, NT-store
            for (crd::usize k2 = 0; !m17_fused && k2 < n2; ++k2)
            {
                const Complex<T>* const srow = scratch + k2 * bw;
                Complex<T>* const drow = din + k2 * n1 + k1;
                crd::usize bb = 0;
                // f32: 1 complex = 8B < 16B NT minimum ⇒ pair two complex into one 16B NT store (drow 16B-aligned:
                // k2·n1+k1 even, bb steps 2). Gives the f32 scatter the RFO-skipping store f64 already had — the
                // store-bound f32 scatter phase HALVES (measured 18.0→10.8 Mcyc @8M). Found by f32 phase archaeology.
                if constexpr (std::is_same_v<T, crd::f32>)
                {
                    if (nt_out)
                    {
                        for (; bb + 2 <= bw; bb += 2)
                        {
                            store_complex_pair_nt(drow + bb, srow + bb);
                        }
                    }
                }
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
        store_fence(); // order the non-temporal stores before the caller reads the result
    }

    // Store one Complex<T>, optionally non-temporal (skips read-for-ownership — the 25.7 GB/s lever). NT path
    // taken only when nt && f64 && dst is ≥16B aligned (plan.tbuf is 64B; the caller's `data` is checked once).
    static void store_complex(Complex<T>* dst, T re, T im, bool nt) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            if (nt)
            {
                _mm_stream_pd(reinterpret_cast<double*>(dst), _mm_set_pd(im, re));
                return;
            }
        }
#else
        (void)nt;
#endif
        dst->re = re;
        dst->im = im;
    }

    // Whether the four-step's phase-1 step writes whole lines from a transposed micro-tile of k1 steps (the levels
    // whose tile is one register, avx2, and two, avx512) or one complex a column's row a step (the levels whose tile
    // spans two narrower registers, where the micro-tile does not fit the register file). The benchmark machine's
    // boards (docs/bench/2026-10-31-simd-perf-fft-four-step-lines.md): the line path 1.6x to 2.7x faster at avx512,
    // 1% to 6% at avx2, 1% to 3% slower at sse2; the same bits either way.
#if CRD_SIMD_HAS_AVX2
    static constexpr bool kWholeLineTiles = true;
#else
    static constexpr bool kWholeLineTiles = false;
#endif

    // One row of a transposed micro-tile: kCols<T, V> complex at dst, a whole number of 64-byte lines on a 64-byte
    // boundary (a tbuf row starts on one, and the run's first k1 is a multiple of the tile). f64 as the four-step's
    // non-temporal stores, now whole lines; f32 as plain stores, as before (the same bits either way).
    template <typename V> static void store_tile_row(Complex<T>* dst, V re, V im) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            constexpr crd::usize count = 2U * kCols<T, V>;
            alignas(64) crd::f64 line[count];
            simd::store_complex_interleaved(line, re, im);
            for (crd::usize i = 0; i < count; i += 2U)
            {
                _mm_stream_pd(reinterpret_cast<double*>(dst) + i, _mm_load_pd(line + i));
            }
        }
        else
        {
            simd::store_complex_interleaved(reinterpret_cast<T*>(dst), re, im);
        }
#else
        simd::store_complex_interleaved(reinterpret_cast<T*>(dst), re, im);
#endif
    }

    // Store two adjacent Complex<f32> as one 16-byte non-temporal store (dst 16B-aligned) on the levels with SSE2;
    // plain stores elsewhere (the same values).
    static void store_complex_pair_nt(Complex<crd::f32>* dst, const Complex<crd::f32>* src) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        const __m128 v = _mm_set_ps(src[1].im, src[1].re, src[0].im, src[0].re);
        _mm_stream_pd(reinterpret_cast<double*>(dst), _mm_castps_pd(v));
#else
        dst[0].re = src[0].re;
        dst[0].im = src[0].im;
        dst[1].re = src[1].re;
        dst[1].im = src[1].im;
#endif
    }

    // One radix-4 Stockham pass: lq (j) groups, each r unit-stride radix-4 butterflies. r = n/L, L = 4·lq.
    // Half the passes of radix-2 ⇒ half the memory traffic (the pass-bound lever). Three twiddles per group
    // (W_L^j, W_L^{2j}, W_L^{3j} = the full table at j·r, 2j·r, 3j·r — all < n).
    void radix4_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, T isign, const T* ptw_re,
                     const T* ptw_im) const
    {
        const crd::usize q = plan.n >> 2; // n/4 (output-quarter stride)
        crd::usize off = 0;               // Lever A: precomputed linear twiddles (w1,w2,w3 per group)
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
        const crd::usize r = plan.n >> 1; // half_n; the two input streams are d[k] and d[r+k]
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
        const crd::usize r = plan.n >> 2; // n/4 = the input-stream stride
        const crd::usize q = r;           // output quarter stride = n/4
        crd::usize tail = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            // SIMD over k with an AoS→SoA deinterleave-load (recovers SIMD on this full pass; the fold made
            // it scalar). r = n/4 is large ⇒ this loop dominates. f32 keeps the scalar path below.
            const auto* dp = reinterpret_cast<const crd::f64*>(d); // interleaved {re,im} pairs
            const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
            {
                const V sg(isign);
                for (crd::usize k = first; k < last; k += kCols<T, V>)
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
            };
            tail = for_tiles<T>(r, step);
        }
        for (crd::usize k = tail; k < r; ++k)
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
        const crd::usize q = plan.n >> 2; // output quarter stride
        crd::usize tail = 0;
        if constexpr (std::is_same_v<T, crd::f64>)
        {
            // SIMD over j (a step's groups): r=1 ⇒ over-k degenerates to scalar, so vectorize the GROUP axis
            // instead. Load the step's groups (four contiguous values each), transpose4x4 a tile into per-point
            // vectors (no cross-lane butterfly shuffles), per-lane combine-twiddles, then contiguous interleaved
            // stores. Recovers SIMD on this otherwise-scalar full pass. w1 loads contiguous (idx=j); w2/w3 are
            // stride-2/3 gathers.
            auto* dp = reinterpret_cast<crd::f64*>(d);
            const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
            {
                const V sg(isign);
                for (crd::usize j = first; j < last; j += kCols<T, V>)
                {
                    V p0r;
                    V p1r;
                    V p2r;
                    V p3r;
                    load_points4(xr + 4 * j, p0r, p1r, p2r, p3r); // → p_m_r = re of point m for the step's groups
                    V p0i;
                    V p1i;
                    V p2i;
                    V p3i;
                    load_points4(xi + 4 * j, p0i, p1i, p2i, p3i);
                    const auto tw = [&](const T* table, crd::usize p) noexcept
                    {
                        const auto at = [&](crd::usize c) noexcept
                        {
                            return table[p * (j + c)];
                        };
                        return gather_f64<V>(at);
                    };
                    const V w1r = V::load(&plan.tw_re[j]);
                    const V w1i = V::load(&plan.tw_im[j]) * sg;
                    const V w2r = tw(plan.tw_re, 2);
                    const V w2i = tw(plan.tw_im, 2) * sg;
                    const V w3r = tw(plan.tw_re, 3);
                    const V w3i = tw(plan.tw_im, 3) * sg;
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
            };
            tail = for_tiles<T>(lq, step);
        }
        for (crd::usize j = tail; j < lq; ++j)
        {
            const T w1r = plan.tw_re[j];
            const T w1i = isign * plan.tw_im[j];
            const T w2r = plan.tw_re[2 * j];
            const T w2i = isign * plan.tw_im[2 * j];
            const T w3r = plan.tw_re[3 * j];
            const T w3i = isign * plan.tw_im[3 * j];
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
            d[j].re = t0r + t2r;
            d[j].im = t0i + t2i;
            d[q + j].re = t1r - rsr;
            d[q + j].im = t1i - rsi;
            d[2 * q + j].re = t0r - t2r;
            d[2 * q + j].im = t0i - t2i;
            d[3 * q + j].re = t1r + rsr;
            d[3 * q + j].im = t1i + rsi;
        }
    }

    // One radix-8 Stockham COMBINE pass via the register-pressure-SCHEDULED SIMD twiddle-codelet (genfft
    // engine): lq groups, each r unit-stride radix-8 butterflies (vectorized over k inside the codelet, with
    // loads-late/stores-early scheduling so the 8-point waist fits the 16-ymm file). Builds 3 bits/pass ⇒
    // log₈(N) passes vs radix-4's log₄(N). Combine-twiddles W_{8·lq}^{m·j} = plan.tw[m·j·r] (isign-conjugated
    // for inverse); DFT-internal twiddles are compile-time inside the fwd/inv codelet. r = n/(8·lq); q = n/8.
    void radix8_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, FftDirection dir,
                     const T* ptw_re, const T* ptw_im) const
    {
        const crd::usize q = plan.n >> 3; // n/8
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
                detail::twiddle8_fwd(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
            else
            {
                detail::twiddle8_inv(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
        }
    }

    // One radix-16 Stockham COMBINE pass via the scheduled SIMD twiddle-codelet (genfft engine): builds 4
    // bits/pass ⇒ log₁₆(N) passes (half of radix-4's). r = n/(16·lq); output stride q = n/16. The 16-point
    // codelet is register-pressure-scheduled (loads late / stores early) to keep peak live tolerable.
    void radix16_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, FftDirection dir,
                      const T* ptw_re, const T* ptw_im) const
    {
        const crd::usize q = plan.n >> 4; // n/16
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
                detail::twiddle16_fwd(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
            else
            {
                detail::twiddle16_inv(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
        }
    }

    // One radix-32 Stockham COMBINE pass (scheduled twiddle-codelet): 5 bits/pass ⇒ log32(N) passes. The
    // 32-point codelet spills (32 complex > 16 ymm) but at L1/L2-resident sizes the spill hits cache (cheap),
    // so fewer passes can win there even though it loses DRAM-bound. r = n/(32·lq); output stride q = n/32.
    void radix32_pass(const T* xr, const T* xi, T* yr, T* yi, crd::usize lq, crd::usize r, FftDirection dir,
                      const T* ptw_re, const T* ptw_im) const
    {
        const crd::usize q = plan.n >> 5; // n/32
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
                detail::twiddle32_fwd(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
            else
            {
                detail::twiddle32_inv(xr, xi, yr, yi, ibase, r, obase, q, w_re, w_im);
            }
        }
    }

    // r unit-stride radix-4 butterflies sharing (w1,w2,w3), whole tiles over k. The tiles' twiddle products round once
    // (fma), the scalar tail's twice; the tile decides which columns each computes on every level, so the
    // {1..16}/SIMD-width determinism holds (ADR-0063).
    static void radix4_row(const T* xr, const T* xi, T* yr, T* yi, crd::usize i0, crd::usize i1, crd::usize i2,
                           crd::usize i3, crd::usize o0, crd::usize o1, crd::usize o2, crd::usize o3, crd::usize r,
                           T w1r, T w1i, T w2r, T w2i, T w3r, T w3i, T isign) noexcept
    {
        const auto step = [&]<typename V>(crd::usize first, crd::usize last) noexcept
        {
            // FMA butterflies (2026-07-04 strided-v3): the MKL-archaeology build — cmul = 2 mul +
            // fnmadd + fma on split registers (fewer ops than MKL's own mul+mul+add+sub idiom).
            const V w1rv(w1r);
            const V w1iv(w1i);
            const V w2rv(w2r);
            const V w2iv(w2i);
            const V w3rv(w3r);
            const V w3iv(w3i);
            const V sg(isign);
            for (crd::usize k = first; k < last; k += kCols<T, V>)
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
                const V rsr = V(T{0}) - sg * t3i;
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
        };
        for (crd::usize k = for_tiles<T>(r, step); k < r; ++k)
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
};

template <typename T> void execute_kernel(const FftPlanState<T>& plan, Complex<T>* data, FftDirection dir) noexcept
{
    Engine<T>{plan}.execute(data, dir);
}

template <typename T>
void execute_batched_kernel(const FftPlanState<T>& plan, Complex<T>* data, crd::usize batch, FftDirection dir) noexcept
{
    Engine<T>{plan}.execute_batched(data, batch, dir);
}

} // namespace

template <typename T> constexpr FftTypeKernels<T> type_kernels() noexcept
{
    return {&execute_kernel<T>, &execute_batched_kernel<T>};
}

} // namespace crd::hesap::fft::detail::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::fft
{

namespace level = detail::CRD_MATH_LEVEL_NS;

const FftKernels CRD_KERNEL_TABLE(kFftKernels) = {
    level::type_kernels<crd::f64>(),
    level::type_kernels<crd::f32>(),
};

} // namespace crd::hesap::fft
