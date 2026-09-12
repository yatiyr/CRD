#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/dot_simd.hpp>
#include <crd/hesap/dense/detail/parallel_triangular.hpp>
#include <crd/hesap/dense/detail/syrk_microkernel.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <type_traits>

#if defined(CRD_HESAP_CHOL_PROFILE) || defined(CRD_HESAP_CHOL_SCALE_PROFILE)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#endif

namespace crd::hesap::direct
{
namespace dense = crd::hesap::dense;

#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
// Per-LEVEL wall-clock split for the 8T scaling diagnosis (the advisor's "diagnose WHERE scaling is
// lost"). Times each etree level on the DISPATCHER thread only ⇒ race-free + valid at >1 worker
// (unlike the per-call CHOL_PROFILE, whose shared g_cholprof += races across threads ⇒ 1T-only).
// Splits the factor wall into: node-parallel huge-front levels (lever = within-front parallelism /
// serial-section Amdahl) vs tree-parallel healthy levels vs tree-STARVED levels (cnt<num_workers AND
// no huge front ⇒ workers idle = the tree-starvation lever). max_level_wall = the critical-path peak.
namespace
{
struct ScaleProf
{
    double sym_wall_ns = 0.0;     // symbolic_factorize + build_supernodal_symbolic (serial analyze)
    double setup_wall_ns = 0.0;   // ENTIRE pre-numeric setup: symbolic + scratch alloc + level build
    double tree_wall_ns = 0.0;    // healthy tree-parallel levels (cnt >= num_workers)
    double node_wall_ns = 0.0;    // thin + huge ⇒ node-parallel (gemm_parallel within front)
    double starved_wall_ns = 0.0; // cnt < num_workers AND no huge ⇒ tree-parallel but idle workers
    double max_level_wall_ns = 0.0;
    crd::u32 n_levels = 0;
    crd::u32 n_node_levels = 0;
    crd::u32 n_starved_levels = 0;
    crd::u32 max_level_cnt = 0;
    crd::u32 max_level_nc = 0;
    // Within-NODE-PARALLEL-front phase split (the advisor's (a) serial-chain vs (b) parallel-throughput
    // question). Node-parallel fronts dispatch SEQUENTIALLY (one front owns the whole pool at a time) ⇒
    // the front's dispatcher (worker 0) is the single writer ⇒ non-atomic += is race-free.
    double np_cmod_ns = 0.0;    // between-supernode assembly (cmod two-pass)
    double np_cdivA_ns = 0.0;   // (A) inner panel factor — POTF2 + within-obw solve = the SERIAL chain
    double np_bsolveB_ns = 0.0; // (B) below-outer trsm (b_slab, parallel)
    double np_ctrailC_ns = 0.0; // (C) outer trailing Schur (two-pass, parallel)
    void reset() noexcept { *this = ScaleProf{}; }
};
ScaleProf g_scaleprof;
using ScaleClock = std::chrono::high_resolution_clock;
inline double scale_ns(ScaleClock::time_point a, ScaleClock::time_point b) noexcept
{
    return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(b - a).count();
}
} // namespace
#endif

#ifdef CRD_HESAP_CHOL_PROFILE
// Throwaway per-phase profiler (compile-gated, OFF by default — zero overhead in
// shipped builds). Profile at 1 worker (the per-thread-efficiency regime). Reports
// the ms split scatter/cmod/cdiv + achieved GFLOP/s, so we attack the right lever.
namespace
{
struct CholProf
{
    double scatter_ns = 0.0;
    double cmod_ns = 0.0;
    double cdiv_ns = 0.0;
    double cmod_scatter_ns = 0.0; // the rr[]-indexed scatter-subtract of U into the panel (within cmod)
    crd::u64 cmod_flops = 0;
    crd::u64 cdiv_flops = 0;
    crd::u64 cmod_calls = 0; // # of per-descendant gemm calls (avg flop/call confirms low-K)
    // Per-knc-bin flop + time (bins: <8, 8-16, 17-32, 33-64, 65-128, 129+). Discriminates
    // amalgamation (low-K bandwidth-bound) vs kernel (high-K) — where do the cmod flops live?
    static constexpr int kBins = 6;
    crd::u64 cmod_flop_bin[kBins] = {};
    double cmod_ns_bin[kBins] = {};
    static int knc_bin(crd::u32 knc) noexcept
    {
        return knc < 8 ? 0 : knc < 16 ? 1 : knc < 32 ? 2 : knc < 64 ? 3 : knc < 128 ? 4 : 5;
    }
    // For the K≥128 cmod calls (85% of the flops, the 52-GF/s wall): bin by OUTPUT WIDTH m1
    // (the gemm N dim). Small m1 ⇒ skinny-C ⇒ low A-reuse ⇒ memory-bound (kernel tweaks can't
    // fix it; the lever is widening C via amalgamation). Large m1 ⇒ genuine kernel/shape.
    static constexpr int kM1Bins = 5;
    crd::u64 cmod_hk_m1_flop[kM1Bins] = {}; // m1: <16, 16-63, 64-255, 256-1023, 1024+
    crd::u64 cmod_hk_m1_count[kM1Bins] = {};
    static int m1_bin(crd::u32 m1) noexcept { return m1 < 16 ? 0 : m1 < 64 ? 1 : m1 < 256 ? 2 : m1 < 1024 ? 3 : 4; }
    // cdiv (25 GF/s — worst phase) by panel width nc: is the time in the thin scalar panels
    // (nc ≤ 32) or the fat blocked-BLAS-3 panels? Picks the cdiv lever.
    static constexpr int kCdivBins = 5;
    crd::u64 cdiv_flop_bin[kCdivBins] = {}; // nc: ≤4, 5-32, 33-127, 128-511, 512+
    double cdiv_ns_bin[kCdivBins] = {};
    static int cdiv_bin(crd::u32 nc) noexcept { return nc <= 4 ? 0 : nc <= 32 ? 1 : nc < 128 ? 2 : nc < 512 ? 3 : 4; }
    // Within-cdiv split (v5a-4 cdiv crush): is the 38%-peak loss in the OUTER trailing (step B,
    // K=obw=256, the bulk Schur gemm) or the WITHIN-panel K=64 work (A1 scalar POTF2 + A2
    // below-solve + A3 inner trailing)? cdiv_outertrail_ns times B; the rest = cdiv_ns − that.
    double cdiv_outertrail_ns = 0.0;
    double bcopy_ns = 0.0;        // fused-B snapshot copy
    double bgemm_ns = 0.0;        // fused-B solve gemm
    double ctrail_syrk_ns = 0.0;  // serial (C) diagonal syrk
    double ctrail_below_ns = 0.0; // serial (C) below gemm
    crd::u64 ctrail_calls = 0;
    void reset() noexcept { *this = CholProf{}; }
};
CholProf g_cholprof;
using ProfClock = std::chrono::high_resolution_clock;
inline double prof_ns(ProfClock::time_point a, ProfClock::time_point b) noexcept
{
    return std::chrono::duration<double, std::nano>(b - a).count();
}
} // namespace
#endif

// Real/complex bridges for the LLᵀ (real) ↔ LLᴴ (complex Hermitian) single path.
// For real T every bridge is the identity; for Complex<R> they select the real
// part / conjugate so the SAME factor + solve body computes A = L·Lᴴ. The pivot
// of an HPD matrix is real-positive — we take its real part, sqrt it, and store
// the diagonal as a pure real (zeroing the rounding residual in im so that
// chol_conj(L[j][j]) == L[j][j] downstream).
template <typename T> [[nodiscard]] constexpr dense::RealType<T> chol_real(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return x.re;
    }
    else
    {
        return x;
    }
}

template <typename T> [[nodiscard]] constexpr T chol_from_real(dense::RealType<T> d) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return T{d, dense::RealType<T>{0}};
    }
    else
    {
        return d;
    }
}

template <typename T> [[nodiscard]] constexpr T chol_conj(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return conj(x);
    }
    else
    {
        return x;
    }
}

// cmod Schur update + backward-solve adjoint: Lᴴ for complex, Lᵀ for real.
template <typename T>
inline constexpr dense::Trans kCholAdjoint =
    dense::is_complex_v<T> ? dense::Trans::ConjTranspose : dense::Trans::Transpose;

// Blocked-cdiv OUTER panel width — the K dimension of the trailing Schur gemm (B step).
// Bigger ⇒ higher arithmetic intensity + fewer (larger) trailing-gemm calls (the v5a-4 cdiv
// crush). A NAIVE single-level sweep capped at 128: its scalar diagonal POTF2 is O(bw²·nc)
// (measured — bw=256 REGRESSED vs 128). The TWO-LEVEL panel factorization (inner kCdivInnerBlock
// granularity) removes that O(bw²) cap, so the outer block can be wide. Tuned at v5a-4.
inline constexpr crd::u32 kCdivBlock = 256;

// Blocked-cdiv INNER panel width — the granularity of the scalar diagonal POTF2 + the
// kCdivInnerBlock×kCdivInnerBlock invert inside the outer panel factorization. The ONLY scalar
// work is here, total O(nc·kCdivInnerBlock²) — independent of the outer kCdivBlock (which is the
// whole point of the two-level structure). 64 matches the dense xPOTRF panel.
inline constexpr crd::u32 kCdivInnerBlock = 64;

// v7-e-2: minimum descendant-overlap width m1 for the cmod to split off the SYMMETRIC m1×m1 diagonal-block
// update into a packed lower-triangle SYRK (half flops). Below this, the diagonal block is too small for the
// syrk pack to amortize, so the full gemm is kept (FEA's tiny-supernode descendants stay on the original path).
inline constexpr crd::u32 kCmodSyrkMin = 48;

// Supernodes with nc ≤ this use the pure-scalar rank-1 cdiv (no invert/gemm overhead):
// they hold negligible factor flops but are numerous (bmwcra: 11813/16007 have nc 2-4),
// so the per-supernode BLAS-3 setup cost would dominate their tiny work. Fat supernodes
// (nc > this) use the blocked invert+gemm BLAS-3 path. Tuned at v5a-4 close.
inline constexpr crd::u32 kCdivScalarMin = 32;

// A panel gemm (cmod / cdiv) routes to `gemm_parallel` instead of serial `gemm` only
// when (a) the supernode is processed node-parallel (few-supernode etree level ⇒ idle
// workers) AND (b) its flop count clears this bar (else fiber-dispatch overhead > win).
// This is the within-supernode parallelism for the few huge near-root fronts that
// serialize under pure tree-parallelism (the 8-thread CHOLMOD gap). gemm_parallel is
// bit-identical to serial gemm (disjoint row-slabs) ⇒ the determinism moat holds.
inline constexpr crd::u64 kGemmParallelMinFlop = 1U << 20; // ~1M flop

// A supernode counts as a HUGE front (node-parallel candidate) when its column count
// reaches this — its panel gemms then clear kGemmParallelMinFlop so gemm_parallel engages.
// A thin etree level goes node-parallel ONLY if it holds such a front; a thin level of
// only-small supernodes stays TREE-parallel (serializing it would regress small/medium
// matrices — bcsstk25 measured 1.25×→0.57× at 8 threads under blanket node-parallel).
inline constexpr crd::u32 kNodeParallelMinCols = 512;

// v5a-5: single-RHS parallel-solve fill gate. The level-parallel single-RHS solve only pays on LARGE
// factors; on small ones the per-level dispatch overhead (parallel_for + wait + frame_reset over many
// tiny levels) dwarfs the single-vector work and REGRESSES hard (measured 8-thread vs 1-thread:
// bcsstk25 ~12x, bcsstk13 ~39x slower). Corpus: regression at <=1.5M factor nnz, benefit at >=26.6M;
// conservative cut at 24M (a per-level work gate is the future refinement to capture the mid-range).

// Invert a bw×bw lower-triangular Cholesky diagonal block L (ColMajor, leading dim
// ldl, real-positive diagonal) into linv (bw×bw ColMajor ld=bw, lower-tri; upper
// zeroed): linv = L⁻¹. Lets the cdiv below-block solve L21 = A21·L⁻ᴴ run as a single
// BLAS-3 gemm instead of the AI≈0.33 rank-1 sweep (the v5a-4 cdiv crush). bw ≤ kCdivBlock
// so this O(bw³/6) scalar inverse is negligible; the diagonal block of an SPD factor is
// well-conditioned ⇒ inverting it is numerically safe (residual-validated at slice close).
template <typename T> inline void invert_lower_tri(const T* l, crd::u32 ldl, crd::u32 bw, T* linv) noexcept
{
    for (crd::u32 i = 0; i < bw * bw; ++i)
    {
        linv[i] = T{0};
    }
    for (crd::u32 j = 0; j < bw; ++j)
    {
        linv[static_cast<crd::usize>(j) * bw + j] = T{1} / l[static_cast<crd::usize>(j) * ldl + j];
        for (crd::u32 i = j + 1; i < bw; ++i)
        {
            T s = T{0};
            for (crd::u32 k = j; k < i; ++k)
            {
                s += l[static_cast<crd::usize>(k) * ldl + i] * linv[static_cast<crd::usize>(j) * bw + k];
            }
            linv[static_cast<crd::usize>(j) * bw + i] = -s / l[static_cast<crd::usize>(i) * ldl + i];
        }
    }
}

// ---- Triangular-solve kernels (2026-06-11, the full-scoreboard solve dig). The hand scalar loops left
// HALF the achievable memory bandwidth on the floor (lat32 serial: fwd 30.5 ms ≈ 17.8 GB/s, back 39.6 ms ≈
// 13.7 GB/s vs the ~36 GB/s CHOLMOD's dgemv-based solve streams — the backward's single-accumulator FP-add
// chain can't be auto-vectorized under strict FP, and the forward's plain loops lack unrolling). ----

// y[i] -= a[i]·s (and the += twin): an ELEMENT-INDEPENDENT map — the vector form performs the identical
// per-element mul-then-add/sub (two roundings, no FMA fusion) ⇒ BIT-IDENTICAL to the scalar loop it
// replaces at any SIMD width. Used by both the serial and the level-parallel forward (same kernel ⇒ the
// serial≡parallel solve equality is preserved verbatim).
template <typename T> inline void solve_axpy_minus(T* y, const T* a, T s, crd::u32 n) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        const simd::Vec4d vs(s);
        crd::u32 i = 0;
        for (; i + 16 <= n; i += 16)
        {
            const simd::Vec4d y0 = simd::Vec4d::load(y + i + 0) - simd::Vec4d::load(a + i + 0) * vs;
            const simd::Vec4d y1 = simd::Vec4d::load(y + i + 4) - simd::Vec4d::load(a + i + 4) * vs;
            const simd::Vec4d y2 = simd::Vec4d::load(y + i + 8) - simd::Vec4d::load(a + i + 8) * vs;
            const simd::Vec4d y3 = simd::Vec4d::load(y + i + 12) - simd::Vec4d::load(a + i + 12) * vs;
            y0.store(y + i + 0);
            y1.store(y + i + 4);
            y2.store(y + i + 8);
            y3.store(y + i + 12);
        }
        for (; i + 4 <= n; i += 4)
        {
            (simd::Vec4d::load(y + i) - simd::Vec4d::load(a + i) * vs).store(y + i);
        }
        for (; i < n; ++i)
        {
            y[i] -= a[i] * s;
        }
    }
    else
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            y[i] -= a[i] * s;
        }
    }
}

template <typename T> inline void solve_acc_plus(T* y, const T* a, T s, crd::u32 n) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        const simd::Vec4d vs(s);
        crd::u32 i = 0;
        for (; i + 16 <= n; i += 16)
        {
            const simd::Vec4d y0 = simd::Vec4d::load(y + i + 0) + simd::Vec4d::load(a + i + 0) * vs;
            const simd::Vec4d y1 = simd::Vec4d::load(y + i + 4) + simd::Vec4d::load(a + i + 4) * vs;
            const simd::Vec4d y2 = simd::Vec4d::load(y + i + 8) + simd::Vec4d::load(a + i + 8) * vs;
            const simd::Vec4d y3 = simd::Vec4d::load(y + i + 12) + simd::Vec4d::load(a + i + 12) * vs;
            y0.store(y + i + 0);
            y1.store(y + i + 4);
            y2.store(y + i + 8);
            y3.store(y + i + 12);
        }
        for (; i + 4 <= n; i += 4)
        {
            (simd::Vec4d::load(y + i) + simd::Vec4d::load(a + i) * vs).store(y + i);
        }
        for (; i < n; ++i)
        {
            y[i] += a[i] * s;
        }
    }
    else
    {
        for (crd::u32 i = 0; i < n; ++i)
        {
            y[i] += a[i] * s;
        }
    }
}

// Σ conj(a[i])·b[i] for the BACKWARD substitution: real types route to the canonical `simd_dot` (FMA +
// the fixed 4-accumulator reduction tree — deterministic, and the SAME kernel runs in the serial and the
// level-parallel backward ⇒ x stays bit-identical across worker counts; the dot's reduction order is a
// kernel-version change like any other, residual-validated). Complex keeps the scalar conj chain.
template <typename T> [[nodiscard]] inline T solve_dot_conj(const T* a, const T* b, crd::u32 n) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        return dense::detail::simd_dot(a, b, n);
    }
    else
    {
        T acc{};
        for (crd::u32 i = 0; i < n; ++i)
        {
            acc += chol_conj<T>(a[i]) * b[i];
        }
        return acc;
    }
}

// ---- 4-COLUMN-FUSED solve phase kernels (2026-06-11, the multi-stream dig). MEASURED MECHANISM: one
// sequential read stream tops out at ~22.7 GB/s on this machine while 4 interleaved streams reach
// ~36.9 GB/s (DRAM bank/page-level parallelism) — exactly how OpenBLAS's dgemv-based CHOLMOD solve hits
// ~29 GB/s per pass on the IDENTICAL trapezoid (67.87M doubles BOTH sides, computed from both data
// structures; the old "22% less fill" compared our trapezoid to their rectangle count). Fusing 4 panel
// columns per pass turns each solve phase into 4 concurrent read streams and cuts the cache-side tmp/x
// traffic 4×.
//   · FORWARD fusion is BIT-IDENTICAL: every output element still accumulates its column terms in
//     ascending-k order with the same mul-then-add/sub per term — only the loop interleaving changes.
//   · BACKWARD fusion is a NEW fixed deterministic reduction order (2 FMA accumulators per column + a
//     fixed scalar tail). Serial and level-parallel paths share these helpers BY CONSTRUCTION ⇒ x is
//     bit-identical across worker counts. Non-f64 keeps the single-column kernels.

// acc[i] += Σ_{k<nc} col_k[i]·y[k] (columns at stride ld) — the forward below-accumulate.
template <typename T>
inline void solve_fwd_below_acc(T* acc, const T* panel0, crd::usize ld, crd::u32 nc, const T* y, crd::u32 n) noexcept
{
    crd::u32 k = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        for (; k + 4 <= nc; k += 4)
        {
            const T* c0 = panel0 + static_cast<crd::usize>(k + 0) * ld;
            const T* c1 = panel0 + static_cast<crd::usize>(k + 1) * ld;
            const T* c2 = panel0 + static_cast<crd::usize>(k + 2) * ld;
            const T* c3 = panel0 + static_cast<crd::usize>(k + 3) * ld;
            const simd::Vec4d y0(y[k + 0]);
            const simd::Vec4d y1(y[k + 1]);
            const simd::Vec4d y2(y[k + 2]);
            const simd::Vec4d y3(y[k + 3]);
            crd::u32 i = 0;
            for (; i + 8 <= n; i += 8) // two independent chains hide the add latency
            {
                simd::Vec4d ta = simd::Vec4d::load(acc + i);
                simd::Vec4d tb = simd::Vec4d::load(acc + i + 4);
                ta = ta + simd::Vec4d::load(c0 + i) * y0;
                tb = tb + simd::Vec4d::load(c0 + i + 4) * y0;
                ta = ta + simd::Vec4d::load(c1 + i) * y1;
                tb = tb + simd::Vec4d::load(c1 + i + 4) * y1;
                ta = ta + simd::Vec4d::load(c2 + i) * y2;
                tb = tb + simd::Vec4d::load(c2 + i + 4) * y2;
                ta = ta + simd::Vec4d::load(c3 + i) * y3;
                tb = tb + simd::Vec4d::load(c3 + i + 4) * y3;
                ta.store(acc + i);
                tb.store(acc + i + 4);
            }
            for (; i < n; ++i)
            {
                acc[i] = ((((acc[i] + c0[i] * y[k + 0]) + c1[i] * y[k + 1]) + c2[i] * y[k + 2]) + c3[i] * y[k + 3]);
            }
        }
    }
    for (; k < nc; ++k) // remainder columns (and the whole loop for non-f64) — ascending k preserved
    {
        solve_acc_plus<T>(acc, panel0 + static_cast<crd::usize>(k) * ld, y[k], n);
    }
}

// x[i] −= Σ_{k<nc} col_k[i]·y[k] — the fused-minus twin (the forward diagonal's below-block update).
template <typename T>
inline void solve_fwd_apply_minus(T* x, const T* panel0, crd::usize ld, crd::u32 nc, const T* y, crd::u32 n) noexcept
{
    crd::u32 k = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        for (; k + 4 <= nc; k += 4)
        {
            const T* c0 = panel0 + static_cast<crd::usize>(k + 0) * ld;
            const T* c1 = panel0 + static_cast<crd::usize>(k + 1) * ld;
            const T* c2 = panel0 + static_cast<crd::usize>(k + 2) * ld;
            const T* c3 = panel0 + static_cast<crd::usize>(k + 3) * ld;
            const simd::Vec4d y0(y[k + 0]);
            const simd::Vec4d y1(y[k + 1]);
            const simd::Vec4d y2(y[k + 2]);
            const simd::Vec4d y3(y[k + 3]);
            crd::u32 i = 0;
            for (; i + 8 <= n; i += 8)
            {
                simd::Vec4d ta = simd::Vec4d::load(x + i);
                simd::Vec4d tb = simd::Vec4d::load(x + i + 4);
                ta = ta - simd::Vec4d::load(c0 + i) * y0;
                tb = tb - simd::Vec4d::load(c0 + i + 4) * y0;
                ta = ta - simd::Vec4d::load(c1 + i) * y1;
                tb = tb - simd::Vec4d::load(c1 + i + 4) * y1;
                ta = ta - simd::Vec4d::load(c2 + i) * y2;
                tb = tb - simd::Vec4d::load(c2 + i + 4) * y2;
                ta = ta - simd::Vec4d::load(c3 + i) * y3;
                tb = tb - simd::Vec4d::load(c3 + i + 4) * y3;
                ta.store(x + i);
                tb.store(x + i + 4);
            }
            for (; i < n; ++i)
            {
                x[i] = ((((x[i] - c0[i] * y[k + 0]) - c1[i] * y[k + 1]) - c2[i] * y[k + 2]) - c3[i] * y[k + 3]);
            }
        }
    }
    for (; k < nc; ++k)
    {
        solve_axpy_minus<T>(x, panel0 + static_cast<crd::usize>(k) * ld, y[k], n);
    }
}

// The forward DIAGONAL triangle solve in 4-column blocks: the in-block recurrence is the exact sequential
// scalar form; rows below the block receive the block's 4 columns ascending via the fused update ⇒
// bit-identical for f64; the generic path IS the original per-column loop.
template <typename T> inline void solve_fwd_diag(T* x, const T* panel, crd::usize ld, crd::u32 nc) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        for (crd::u32 j0 = 0; j0 < nc; j0 += 4)
        {
            const crd::u32 jb = (j0 + 4 <= nc) ? 4U : (nc - j0);
            for (crd::u32 jj = j0; jj < j0 + jb; ++jj)
            {
                const T yj = x[jj] / panel[static_cast<crd::usize>(jj) * ld + jj];
                x[jj] = yj;
                const T* colj = panel + static_cast<crd::usize>(jj) * ld;
                for (crd::u32 i = jj + 1; i < j0 + jb; ++i)
                {
                    x[i] -= colj[i] * yj;
                }
            }
            const crd::u32 rest = nc - (j0 + jb);
            if (rest > 0)
            {
                solve_fwd_apply_minus<T>(x + j0 + jb, panel + static_cast<crd::usize>(j0) * ld + j0 + jb, ld, jb,
                                         x + j0, rest);
            }
        }
    }
    else
    {
        for (crd::u32 j = 0; j < nc; ++j)
        {
            const T yj = x[j] / panel[static_cast<crd::usize>(j) * ld + j];
            x[j] = yj;
            solve_axpy_minus<T>(x + j + 1, panel + static_cast<crd::usize>(j) * ld + j + 1, yj, nc - j - 1);
        }
    }
}

// out[k] = Σ_i col_k[i]·b[i] for 4 columns fused (4 read streams; 2 FMA accumulators per column + a fixed
// scalar tail) — a fixed deterministic reduction order.
inline void solve_dot4_f64(const crd::f64* c0, const crd::f64* c1, const crd::f64* c2, const crd::f64* c3,
                           const crd::f64* b, crd::u32 n, crd::f64 out[4]) noexcept
{
    namespace simd = crd::math::simd;
    simd::Vec4d a0l = simd::Vec4d::zero();
    simd::Vec4d a0h = simd::Vec4d::zero();
    simd::Vec4d a1l = simd::Vec4d::zero();
    simd::Vec4d a1h = simd::Vec4d::zero();
    simd::Vec4d a2l = simd::Vec4d::zero();
    simd::Vec4d a2h = simd::Vec4d::zero();
    simd::Vec4d a3l = simd::Vec4d::zero();
    simd::Vec4d a3h = simd::Vec4d::zero();
    crd::u32 i = 0;
    for (; i + 8 <= n; i += 8)
    {
        const simd::Vec4d bl = simd::Vec4d::load(b + i);
        const simd::Vec4d bh = simd::Vec4d::load(b + i + 4);
        a0l = simd::fma(simd::Vec4d::load(c0 + i), bl, a0l);
        a0h = simd::fma(simd::Vec4d::load(c0 + i + 4), bh, a0h);
        a1l = simd::fma(simd::Vec4d::load(c1 + i), bl, a1l);
        a1h = simd::fma(simd::Vec4d::load(c1 + i + 4), bh, a1h);
        a2l = simd::fma(simd::Vec4d::load(c2 + i), bl, a2l);
        a2h = simd::fma(simd::Vec4d::load(c2 + i + 4), bh, a2h);
        a3l = simd::fma(simd::Vec4d::load(c3 + i), bl, a3l);
        a3h = simd::fma(simd::Vec4d::load(c3 + i + 4), bh, a3h);
    }
    crd::f64 t0 = 0.0;
    crd::f64 t1 = 0.0;
    crd::f64 t2 = 0.0;
    crd::f64 t3 = 0.0;
    for (; i < n; ++i)
    {
        t0 += c0[i] * b[i];
        t1 += c1[i] * b[i];
        t2 += c2[i] * b[i];
        t3 += c3[i] * b[i];
    }
    out[0] = simd::horizontal_sum(a0l + a0h) + t0;
    out[1] = simd::horizontal_sum(a1l + a1h) + t1;
    out[2] = simd::horizontal_sum(a2l + a2h) + t2;
    out[3] = simd::horizontal_sum(a3l + a3h) + t3;
}

// x[k] −= Σ_i conj(col_k[i])·tmp[i] over nc columns — the backward below-block, 4-fused for f64.
template <typename T>
inline void solve_back_below(T* x, const T* panel0, crd::usize ld, crd::u32 nc, const T* tmp, crd::u32 below) noexcept
{
    crd::u32 k = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        for (; k + 4 <= nc; k += 4)
        {
            crd::f64 d[4];
            solve_dot4_f64(panel0 + static_cast<crd::usize>(k + 0) * ld, panel0 + static_cast<crd::usize>(k + 1) * ld,
                           panel0 + static_cast<crd::usize>(k + 2) * ld, panel0 + static_cast<crd::usize>(k + 3) * ld,
                           tmp, below, d);
            x[k + 0] -= d[0];
            x[k + 1] -= d[1];
            x[k + 2] -= d[2];
            x[k + 3] -= d[3];
        }
    }
    for (; k < nc; ++k)
    {
        x[k] -= solve_dot_conj<T>(panel0 + static_cast<crd::usize>(k) * ld, tmp, below);
    }
}

// The backward DIAGONAL solve (Lᴴ, columns descending) in 4-column blocks: the block's FAR dots (against
// the already-final x below it) run 4-fused, then the in-block descending recurrence adds the near terms —
// v = (x_j − near…) − far_j is the fixed deterministic order.
template <typename T> inline void solve_back_diag(T* x, const T* panel, crd::usize ld, crd::u32 nc) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        if (nc == 0)
        {
            return;
        }
        crd::u32 q = ((nc - 1) / 4) * 4; // the top (possibly partial) block start, descending
        for (;;)
        {
            const crd::u32 qb = ((q + 4 <= nc) ? 4U : (nc - q));
            const crd::u32 far0 = q + qb; // rows below the block (already final)
            crd::f64 far[4] = {0.0, 0.0, 0.0, 0.0};
            if (far0 < nc)
            {
                if (qb == 4)
                {
                    solve_dot4_f64(panel + static_cast<crd::usize>(q + 0) * ld + far0,
                                   panel + static_cast<crd::usize>(q + 1) * ld + far0,
                                   panel + static_cast<crd::usize>(q + 2) * ld + far0,
                                   panel + static_cast<crd::usize>(q + 3) * ld + far0, x + far0, nc - far0, far);
                }
                else
                {
                    for (crd::u32 t = 0; t < qb; ++t)
                    {
                        far[t] =
                            solve_dot_conj<T>(panel + static_cast<crd::usize>(q + t) * ld + far0, x + far0, nc - far0);
                    }
                }
            }
            for (crd::u32 jj = q + qb; jj-- > q;)
            {
                const T* coljj = panel + static_cast<crd::usize>(jj) * ld;
                T v = x[jj];
                for (crd::u32 kk = jj + 1; kk < q + qb; ++kk) // near terms (within the block)
                {
                    v -= coljj[kk] * x[kk];
                }
                v -= far[jj - q];
                x[jj] = v / coljj[jj];
            }
            if (q == 0)
            {
                break;
            }
            q -= 4;
        }
    }
    else
    {
        for (crd::u32 jj = nc; jj-- > 0;)
        {
            const T* coljj = panel + static_cast<crd::usize>(jj) * ld;
            const T v = x[jj] - solve_dot_conj<T>(coljj + jj + 1, x + jj + 1, nc - jj - 1);
            x[jj] = v / coljj[jj];
        }
    }
}

// Single-rounded scalar fma matching the SIMD kernels' per-lane op (tail elements must carry the same
// rounding as their vectorized siblings to preserve the gemm-path bit-identity contract).
template <typename T> inline T solve_fma1(T a, T b, T c) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        T out[4];
        simd::fma(simd::Vec4d(a), simd::Vec4d(b), simd::Vec4d(c)).store(out);
        return out[0];
    }
    else
    {
        return a * b + c;
    }
}

// ---- MULTI-RHS solve block kernels (2026-06-11, the x16 dig). The multi-RHS paths ran ONE dense::gemm
// per supernode per pass (~2·nsuper calls): each call pays allocator + pack + driver setup — overhead-
// dominated on the thousands of tiny supernodes — and the cold packs were single-stream. These hand
// kernels are allocation-free, pack-free, 4-fused (4 concurrent column streams, the measured 22.7 →
// 36.9 GB/s DRAM mechanism) and keep the EXACT per-element fma chains of the gemm path (k-ascending,
// zero-init, single sweep — vectorization only across INDEPENDENT elements) ⇒ BIT-IDENTICAL to
// dense::gemm for K ≤ kGemmKc, which is the dispatch gate (bigger K keeps dense::gemm, identical values
// via its Kc-chunks... those have K > kGemmKc and stay on the gemm path entirely).

// tm (ColMajor below×nrhs, ld=below) := Σ_{k<nc} col_k ⊗ y(k,:) — the forward below-update block.
// y is ColMajor (ldy = the RHS leading dim). Requires nc ≤ kGemmKc for the bit-identity contract.
template <typename T>
inline void solve_mrhs_fwd_below(T* tm, const T* panel0, crd::usize ld, crd::u32 nc, const T* y, crd::usize ldy,
                                 crd::u32 below, crd::usize nrhs) noexcept
{
    for (crd::usize c = 0; c < nrhs; ++c) // zero-init (the gemm path's beta=0 store)
    {
        T* tc = tm + c * below;
        for (crd::u32 r = 0; r < below; ++r)
        {
            tc[r] = T{0};
        }
    }
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        // r-BLOCKED: the tm block (rblk x nrhs) stays cache-resident across ALL k while each panel
        // column streams exactly once; the per-element k-ascending fma chain is unchanged (f64 memory
        // roundtrips between blocks are exact) => bit-identical at any block size.
        constexpr crd::u32 rblk = 256;
        for (crd::u32 r0 = 0; r0 < below; r0 += rblk)
        {
            const crd::u32 r1 = (r0 + rblk <= below) ? (r0 + rblk) : below;
            crd::u32 k = 0;
            for (; k + 4 <= nc; k += 4) // 4 fused column streams
            {
                const T* c0 = panel0 + static_cast<crd::usize>(k + 0) * ld;
                const T* c1 = panel0 + static_cast<crd::usize>(k + 1) * ld;
                const T* c2 = panel0 + static_cast<crd::usize>(k + 2) * ld;
                const T* c3 = panel0 + static_cast<crd::usize>(k + 3) * ld;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    const simd::Vec4d y0(y[(k + 0) + c * ldy]);
                    const simd::Vec4d y1(y[(k + 1) + c * ldy]);
                    const simd::Vec4d y2(y[(k + 2) + c * ldy]);
                    const simd::Vec4d y3(y[(k + 3) + c * ldy]);
                    T* tc = tm + c * below;
                    crd::u32 r = r0;
                    for (; r + 8 <= r1; r += 8) // two independent chains hide the fma latency
                    {
                        simd::Vec4d ta = simd::Vec4d::load(tc + r);
                        simd::Vec4d tb = simd::Vec4d::load(tc + r + 4);
                        ta = simd::fma(simd::Vec4d::load(c0 + r), y0, ta); // k-ascending fma chain
                        tb = simd::fma(simd::Vec4d::load(c0 + r + 4), y0, tb);
                        ta = simd::fma(simd::Vec4d::load(c1 + r), y1, ta);
                        tb = simd::fma(simd::Vec4d::load(c1 + r + 4), y1, tb);
                        ta = simd::fma(simd::Vec4d::load(c2 + r), y2, ta);
                        tb = simd::fma(simd::Vec4d::load(c2 + r + 4), y2, tb);
                        ta = simd::fma(simd::Vec4d::load(c3 + r), y3, ta);
                        tb = simd::fma(simd::Vec4d::load(c3 + r + 4), y3, tb);
                        ta.store(tc + r);
                        tb.store(tc + r + 4);
                    }
                    for (; r + 4 <= r1; r += 4)
                    {
                        simd::Vec4d t = simd::Vec4d::load(tc + r);
                        t = simd::fma(simd::Vec4d::load(c0 + r), y0, t);
                        t = simd::fma(simd::Vec4d::load(c1 + r), y1, t);
                        t = simd::fma(simd::Vec4d::load(c2 + r), y2, t);
                        t = simd::fma(simd::Vec4d::load(c3 + r), y3, t);
                        t.store(tc + r);
                    }
                    for (; r < r1; ++r)
                    {
                        T t = tc[r];
                        t = solve_fma1<T>(c0[r], y[(k + 0) + c * ldy], t);
                        t = solve_fma1<T>(c1[r], y[(k + 1) + c * ldy], t);
                        t = solve_fma1<T>(c2[r], y[(k + 2) + c * ldy], t);
                        t = solve_fma1<T>(c3[r], y[(k + 3) + c * ldy], t);
                        tc[r] = t;
                    }
                }
            }
            for (; k < nc; ++k) // remainder columns
            {
                const T* ck = panel0 + static_cast<crd::usize>(k) * ld;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    const simd::Vec4d yk(y[k + c * ldy]);
                    T* tc = tm + c * below;
                    crd::u32 r = r0;
                    for (; r + 4 <= r1; r += 4)
                    {
                        simd::fma(simd::Vec4d::load(ck + r), yk, simd::Vec4d::load(tc + r)).store(tc + r);
                    }
                    for (; r < r1; ++r)
                    {
                        tc[r] = solve_fma1<T>(ck[r], y[k + c * ldy], tc[r]);
                    }
                }
            }
        }
    }
    else
    {
        for (crd::u32 k = 0; k < nc; ++k)
        {
            const T* ck = panel0 + static_cast<crd::usize>(k) * ld;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                const T yk = y[k + c * ldy];
                T* tc = tm + c * below;
                for (crd::u32 r = 0; r < below; ++r)
                {
                    tc[r] += ck[r] * yk;
                }
            }
        }
    }
}

// y(k,c) −= Σ_{r<below} conj(col_k[r])·wt(r,c) — the backward below-update block. wt is ROW-major
// (below×nrhs, ld=nrhs: the gather writes it that way) so a row's RHS values are contiguous; each
// y-element keeps a sequential r-ascending fma chain (lanes = independent RHS columns) ⇒ bit-identical
// to the gemm path for below ≤ kGemmKc (the dispatch gate).
template <typename T>
inline void solve_mrhs_back_below(T* y, crd::usize ldy, const T* panel0, crd::usize ld, crd::u32 nc, const T* wt,
                                  crd::u32 below, crd::usize nrhs, T* acc) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        // r-BLOCKED accumulation into `acc` (nc x nrhs RowMajor): the wt block and acc stay cache-
        // resident while each panel column streams exactly once; per-element r-ascending fma chains
        // continue across blocks through exact f64 memory roundtrips => deterministic fixed order.
        constexpr crd::u32 rblk = 256;
        for (crd::u32 k = 0; k < nc; ++k)
        {
            T* ak = acc + static_cast<crd::usize>(k) * nrhs;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                ak[c] = T{0};
            }
        }
        for (crd::u32 r0 = 0; r0 < below; r0 += rblk)
        {
            const crd::u32 r1 = (r0 + rblk <= below) ? (r0 + rblk) : below;
            crd::u32 k = 0;
            for (; k + 8 <= nc; k += 8) // 8 fused column streams = 8 independent fma chains
            {
                const T* cp[8];
                T* ap[8];
                for (crd::u32 t = 0; t < 8; ++t)
                {
                    cp[t] = panel0 + static_cast<crd::usize>(k + t) * ld;
                    ap[t] = acc + static_cast<crd::usize>(k + t) * nrhs;
                }
                crd::usize c = 0;
                for (; c + 4 <= nrhs; c += 4)
                {
                    simd::Vec4d a0 = simd::Vec4d::load(ap[0] + c);
                    simd::Vec4d a1 = simd::Vec4d::load(ap[1] + c);
                    simd::Vec4d a2 = simd::Vec4d::load(ap[2] + c);
                    simd::Vec4d a3 = simd::Vec4d::load(ap[3] + c);
                    simd::Vec4d a4 = simd::Vec4d::load(ap[4] + c);
                    simd::Vec4d a5 = simd::Vec4d::load(ap[5] + c);
                    simd::Vec4d a6 = simd::Vec4d::load(ap[6] + c);
                    simd::Vec4d a7 = simd::Vec4d::load(ap[7] + c);
                    for (crd::u32 r = r0; r < r1; ++r)
                    {
                        const simd::Vec4d wrow = simd::Vec4d::load(wt + static_cast<crd::usize>(r) * nrhs + c);
                        a0 = simd::fma(simd::Vec4d(cp[0][r]), wrow, a0);
                        a1 = simd::fma(simd::Vec4d(cp[1][r]), wrow, a1);
                        a2 = simd::fma(simd::Vec4d(cp[2][r]), wrow, a2);
                        a3 = simd::fma(simd::Vec4d(cp[3][r]), wrow, a3);
                        a4 = simd::fma(simd::Vec4d(cp[4][r]), wrow, a4);
                        a5 = simd::fma(simd::Vec4d(cp[5][r]), wrow, a5);
                        a6 = simd::fma(simd::Vec4d(cp[6][r]), wrow, a6);
                        a7 = simd::fma(simd::Vec4d(cp[7][r]), wrow, a7);
                    }
                    a0.store(ap[0] + c);
                    a1.store(ap[1] + c);
                    a2.store(ap[2] + c);
                    a3.store(ap[3] + c);
                    a4.store(ap[4] + c);
                    a5.store(ap[5] + c);
                    a6.store(ap[6] + c);
                    a7.store(ap[7] + c);
                }
                for (; c < nrhs; ++c)
                {
                    for (crd::u32 t = 0; t < 8; ++t)
                    {
                        T s = ap[t][c];
                        for (crd::u32 r = r0; r < r1; ++r)
                        {
                            s = solve_fma1<T>(cp[t][r], wt[static_cast<crd::usize>(r) * nrhs + c], s);
                        }
                        ap[t][c] = s;
                    }
                }
            }
            for (; k + 4 <= nc; k += 4) // 4 fused column streams
            {
                const T* c0 = panel0 + static_cast<crd::usize>(k + 0) * ld;
                const T* c1 = panel0 + static_cast<crd::usize>(k + 1) * ld;
                const T* c2 = panel0 + static_cast<crd::usize>(k + 2) * ld;
                const T* c3 = panel0 + static_cast<crd::usize>(k + 3) * ld;
                T* a0p = acc + static_cast<crd::usize>(k + 0) * nrhs;
                T* a1p = acc + static_cast<crd::usize>(k + 1) * nrhs;
                T* a2p = acc + static_cast<crd::usize>(k + 2) * nrhs;
                T* a3p = acc + static_cast<crd::usize>(k + 3) * nrhs;
                crd::usize c = 0;
                for (; c + 4 <= nrhs; c += 4)
                {
                    simd::Vec4d a0 = simd::Vec4d::load(a0p + c);
                    simd::Vec4d a1 = simd::Vec4d::load(a1p + c);
                    simd::Vec4d a2 = simd::Vec4d::load(a2p + c);
                    simd::Vec4d a3 = simd::Vec4d::load(a3p + c);
                    for (crd::u32 r = r0; r < r1; ++r)
                    {
                        const simd::Vec4d wrow = simd::Vec4d::load(wt + static_cast<crd::usize>(r) * nrhs + c);
                        a0 = simd::fma(simd::Vec4d(c0[r]), wrow, a0);
                        a1 = simd::fma(simd::Vec4d(c1[r]), wrow, a1);
                        a2 = simd::fma(simd::Vec4d(c2[r]), wrow, a2);
                        a3 = simd::fma(simd::Vec4d(c3[r]), wrow, a3);
                    }
                    a0.store(a0p + c);
                    a1.store(a1p + c);
                    a2.store(a2p + c);
                    a3.store(a3p + c);
                }
                for (; c < nrhs; ++c)
                {
                    T s0 = a0p[c];
                    T s1 = a1p[c];
                    T s2 = a2p[c];
                    T s3 = a3p[c];
                    for (crd::u32 r = r0; r < r1; ++r)
                    {
                        const T w = wt[static_cast<crd::usize>(r) * nrhs + c];
                        s0 = solve_fma1<T>(c0[r], w, s0);
                        s1 = solve_fma1<T>(c1[r], w, s1);
                        s2 = solve_fma1<T>(c2[r], w, s2);
                        s3 = solve_fma1<T>(c3[r], w, s3);
                    }
                    a0p[c] = s0;
                    a1p[c] = s1;
                    a2p[c] = s2;
                    a3p[c] = s3;
                }
            }
            for (; k < nc; ++k)
            {
                const T* ck = panel0 + static_cast<crd::usize>(k) * ld;
                T* akp = acc + static_cast<crd::usize>(k) * nrhs;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    T s = akp[c];
                    for (crd::u32 r = r0; r < r1; ++r)
                    {
                        s = solve_fma1<T>(ck[r], wt[static_cast<crd::usize>(r) * nrhs + c], s);
                    }
                    akp[c] = s;
                }
            }
        }
        for (crd::u32 k = 0; k < nc; ++k) // one subtract per element, after the full reduction
        {
            const T* akp = acc + static_cast<crd::usize>(k) * nrhs;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                y[k + c * ldy] -= akp[c];
            }
        }
    }
    else
    {
        (void)acc;
        for (crd::u32 k = 0; k < nc; ++k)
        {
            const T* ck = panel0 + static_cast<crd::usize>(k) * ld;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                T s{};
                for (crd::u32 r = 0; r < below; ++r)
                {
                    s += chol_conj<T>(ck[r]) * wt[static_cast<crd::usize>(r) * nrhs + c];
                }
                y[k + c * ldy] -= s;
            }
        }
    }
}

// dscr (ROW-major nc x nrhs) forward diagonal solve, 4-column-blocked: the in-block recurrence keeps the
// exact sequential op order; rows below the block receive the 4 columns' contributions ascending with the
// same mul-then-sub per term => BIT-IDENTICAL to the unblocked batched loop, with the dscr rmw traffic
// cut 4x and the L-columns read as 4 concurrent streams. Shared by the serial and level-parallel paths.
template <typename T>
inline void solve_mrhs_fwd_diag(T* d, crd::usize nrhs, const T* panel, crd::usize ld, crd::u32 nc) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        for (crd::u32 j0 = 0; j0 < nc; j0 += 4)
        {
            const crd::u32 jb = (j0 + 4 <= nc) ? 4U : (nc - j0);
            for (crd::u32 jj = j0; jj < j0 + jb; ++jj) // exact sequential recurrence within the block
            {
                const T ljj = panel[static_cast<crd::usize>(jj) * ld + jj];
                T* djj = d + static_cast<crd::usize>(jj) * nrhs;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    djj[c] = djj[c] / ljj;
                }
                const T* colj = panel + static_cast<crd::usize>(jj) * ld;
                for (crd::u32 i = jj + 1; i < j0 + jb; ++i)
                {
                    const T lij = colj[i];
                    T* di = d + static_cast<crd::usize>(i) * nrhs;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        di[c] -= lij * djj[c];
                    }
                }
            }
            const T* c0 = panel + static_cast<crd::usize>(j0 + 0) * ld;
            const T* c1 = panel + static_cast<crd::usize>(j0 + 1) * ld;
            const T* c2 = panel + static_cast<crd::usize>(j0 + 2) * ld;
            const T* c3 = panel + static_cast<crd::usize>(j0 + 3) * ld;
            const T* d0 = d + static_cast<crd::usize>(j0 + 0) * nrhs;
            const T* d1 = d + static_cast<crd::usize>(j0 + 1) * nrhs;
            const T* d2 = d + static_cast<crd::usize>(j0 + 2) * nrhs;
            const T* d3 = d + static_cast<crd::usize>(j0 + 3) * nrhs;
            if (jb == 4) // fused 4-stream update of the rows below the block (ascending j per element)
            {
                for (crd::usize ct = 0; ct < nrhs; ct += 8) // RHS tile of 8 (clamped below)
                {
                    const crd::usize cw = (ct + 8 <= nrhs) ? 8 : (nrhs - ct);
                    if (cw == 8)
                    {
                        const simd::Vec4d y0a = simd::Vec4d::load(d0 + ct);
                        const simd::Vec4d y0b = simd::Vec4d::load(d0 + ct + 4);
                        const simd::Vec4d y1a = simd::Vec4d::load(d1 + ct);
                        const simd::Vec4d y1b = simd::Vec4d::load(d1 + ct + 4);
                        const simd::Vec4d y2a = simd::Vec4d::load(d2 + ct);
                        const simd::Vec4d y2b = simd::Vec4d::load(d2 + ct + 4);
                        const simd::Vec4d y3a = simd::Vec4d::load(d3 + ct);
                        const simd::Vec4d y3b = simd::Vec4d::load(d3 + ct + 4);
                        for (crd::u32 i = j0 + 4; i < nc; ++i)
                        {
                            T* di = d + static_cast<crd::usize>(i) * nrhs + ct;
                            simd::Vec4d ta = simd::Vec4d::load(di);
                            simd::Vec4d tb = simd::Vec4d::load(di + 4);
                            const simd::Vec4d l0(c0[i]);
                            ta = ta - l0 * y0a;
                            tb = tb - l0 * y0b;
                            const simd::Vec4d l1(c1[i]);
                            ta = ta - l1 * y1a;
                            tb = tb - l1 * y1b;
                            const simd::Vec4d l2(c2[i]);
                            ta = ta - l2 * y2a;
                            tb = tb - l2 * y2b;
                            const simd::Vec4d l3(c3[i]);
                            ta = ta - l3 * y3a;
                            tb = tb - l3 * y3b;
                            ta.store(di);
                            tb.store(di + 4);
                        }
                    }
                    else
                    {
                        for (crd::u32 i = j0 + 4; i < nc; ++i)
                        {
                            T* di = d + static_cast<crd::usize>(i) * nrhs;
                            for (crd::usize c = ct; c < ct + cw; ++c)
                            {
                                di[c] = (((di[c] - c0[i] * d0[c]) - c1[i] * d1[c]) - c2[i] * d2[c]) - c3[i] * d3[c];
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        for (crd::u32 j = 0; j < nc; ++j)
        {
            const T ljj = panel[static_cast<crd::usize>(j) * ld + j];
            T* dj = d + static_cast<crd::usize>(j) * nrhs;
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                dj[c] = dj[c] / ljj;
            }
            const T* colj = panel + static_cast<crd::usize>(j) * ld;
            for (crd::u32 i = j + 1; i < nc; ++i)
            {
                const T lij = colj[i];
                T* di = d + static_cast<crd::usize>(i) * nrhs;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    di[c] -= lij * dj[c];
                }
            }
        }
    }
}

// dscr (ROW-major nc x nrhs) backward diagonal solve (L^H upper, columns descending), 4-column-blocked:
// per block, the FAR sums (k >= block end, rows already final) are subtracted into the block rows first
// (4 fused L-column streams, per-element sequential k-ascending fma chains), then the exact in-block
// descending recurrence runs. Fixed deterministic order; shared by the serial and parallel paths.
template <typename T>
inline void solve_mrhs_back_diag(T* d, crd::usize nrhs, const T* panel, crd::usize ld, crd::u32 nc) noexcept
{
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        namespace simd = crd::math::simd;
        if (nc == 0)
        {
            return;
        }
        crd::u32 q = ((nc - 1) / 4) * 4;
        for (;;)
        {
            const crd::u32 qb = ((q + 4 <= nc) ? 4U : (nc - q));
            const crd::u32 far0 = q + qb;
            if (far0 < nc) // FAR: d_row(jj) -= sum_{k>=far0} conj(L[k][jj]) * d_row(k), 4 jj fused
            {
                const T* cc[4] = {
                    panel + static_cast<crd::usize>(q + 0) * ld, panel + static_cast<crd::usize>(q + 1) * ld,
                    panel + static_cast<crd::usize>(q + 2) * ld, panel + static_cast<crd::usize>(q + 3) * ld};
                for (crd::usize ct = 0; ct < nrhs; ct += 4)
                {
                    const crd::usize cw = (ct + 4 <= nrhs) ? 4 : (nrhs - ct);
                    if (cw == 4 && qb == 4)
                    {
                        simd::Vec4d a0 = simd::Vec4d::zero();
                        simd::Vec4d a1 = simd::Vec4d::zero();
                        simd::Vec4d a2 = simd::Vec4d::zero();
                        simd::Vec4d a3 = simd::Vec4d::zero();
                        for (crd::u32 k = far0; k < nc; ++k)
                        {
                            const simd::Vec4d dk = simd::Vec4d::load(d + static_cast<crd::usize>(k) * nrhs + ct);
                            a0 = simd::fma(simd::Vec4d(cc[0][k]), dk, a0);
                            a1 = simd::fma(simd::Vec4d(cc[1][k]), dk, a1);
                            a2 = simd::fma(simd::Vec4d(cc[2][k]), dk, a2);
                            a3 = simd::fma(simd::Vec4d(cc[3][k]), dk, a3);
                        }
                        (simd::Vec4d::load(d + static_cast<crd::usize>(q + 0) * nrhs + ct) - a0)
                            .store(d + static_cast<crd::usize>(q + 0) * nrhs + ct);
                        (simd::Vec4d::load(d + static_cast<crd::usize>(q + 1) * nrhs + ct) - a1)
                            .store(d + static_cast<crd::usize>(q + 1) * nrhs + ct);
                        (simd::Vec4d::load(d + static_cast<crd::usize>(q + 2) * nrhs + ct) - a2)
                            .store(d + static_cast<crd::usize>(q + 2) * nrhs + ct);
                        (simd::Vec4d::load(d + static_cast<crd::usize>(q + 3) * nrhs + ct) - a3)
                            .store(d + static_cast<crd::usize>(q + 3) * nrhs + ct);
                    }
                    else
                    {
                        for (crd::u32 t = 0; t < qb; ++t)
                        {
                            for (crd::usize c = ct; c < ct + cw; ++c)
                            {
                                T s{};
                                const T* col = panel + static_cast<crd::usize>(q + t) * ld;
                                for (crd::u32 k = far0; k < nc; ++k)
                                {
                                    s = solve_fma1<T>(col[k], d[static_cast<crd::usize>(k) * nrhs + c], s);
                                }
                                d[static_cast<crd::usize>(q + t) * nrhs + c] -= s;
                            }
                        }
                    }
                }
            }
            for (crd::u32 jj = q + qb; jj-- > q;) // exact in-block descending recurrence (near terms)
            {
                const T* coljj = panel + static_cast<crd::usize>(jj) * ld;
                T* djj = d + static_cast<crd::usize>(jj) * nrhs;
                for (crd::u32 k = jj + 1; k < q + qb; ++k)
                {
                    const T lk = coljj[k];
                    const T* dk = d + static_cast<crd::usize>(k) * nrhs;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        djj[c] -= lk * dk[c];
                    }
                }
                const T ljj = coljj[jj];
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    djj[c] = djj[c] / ljj;
                }
            }
            if (q == 0)
            {
                break;
            }
            q -= 4;
        }
    }
    else
    {
        for (crd::u32 jj = nc; jj-- > 0;)
        {
            const T* coljj = panel + static_cast<crd::usize>(jj) * ld;
            T* djj = d + static_cast<crd::usize>(jj) * nrhs;
            for (crd::u32 k = jj + 1; k < nc; ++k)
            {
                const T lk = chol_conj<T>(coljj[k]);
                const T* dk = d + static_cast<crd::usize>(k) * nrhs;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    djj[c] -= lk * dk[c];
                }
            }
            const T ljj = coljj[jj];
            for (crd::usize c = 0; c < nrhs; ++c)
            {
                djj[c] = djj[c] / ljj;
            }
        }
    }
}

// =====================================================================
// v5a-1b step 1 — relaxed supernode amalgamation (D(direct)-2).
//
// Builds the amalgamated supernodal structure from a v2c SymbolicFactor.
// Fundamental supernodes (sf.super) are merged along etree chains in
// postorder: a fundamental supernode `cur` merges with the NEXT one
// (`cur+1`) iff `cur+1` is `cur`'s etree-parent supernode (so their
// columns are contiguous) AND the explicit zeros introduced stay within
// nrelax · (merged columns). The merged group's row pattern is the
// deepest member's pattern R_g = li[lp[g0] .. lp[g0+1]) (parent patterns
// are subsets of it in a Cholesky factor), so the group panel is
// |R_g| × C, and column o (0-based within the group) carries rows
// R_g[o..] (|R_g|-o structural entries). Hence:
//   merged_struct = Σ_{o=0}^{C-1}(|R_g|-o) = C·|R_g| - C(C-1)/2
//   extra_zeros   = merged_struct - Σ_{j∈group} colcount[j]
// Amalgamation errors are PERF-only (the row pattern is the exact union,
// so any merge yields a correct factor); nrelax trades panel size vs
// explicit zeros and is tuned by the close benchmark.
// =======================================================================

SupernodalSymbolic build_supernodal_symbolic(const ordering::SymbolicFactor& sf, crd::memory::IAllocator* alloc,
                                             crd::u32 nrelax)
{
    SupernodalSymbolic out(alloc);
    out.n = sf.n;
    const crd::u32 nf = sf.nsuper;
    if (sf.n == 0 || nf == 0)
    {
        out.scol.resize(1);
        out.scol[0] = 0;
        out.srowp.resize(1);
        out.srowp[0] = 0;
        out.col_super.resize(sf.n);
        return out;
    }

    // Column → fundamental supernode.
    crd::containers::Array<crd::u32> col_fs(alloc);
    col_fs.resize(sf.n);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        for (crd::u32 c = sf.super[f]; c < sf.super[f + 1]; ++c)
        {
            col_fs[c] = f;
        }
    }

    // Etree-parent supernode of fundamental f (kNoParent if a root).
    auto parent_fs = [&](crd::u32 f) -> crd::u32
    {
        const crd::u32 last = sf.super[f + 1] - 1;
        const crd::u32 p = sf.parent[last];
        return (p == ordering::kNoParent) ? ordering::kNoParent : col_fs[p];
    };
    // colcount[c] == li entries of column c == lp[c+1]-lp[c].
    auto colcount = [&](crd::u32 c) -> crd::u64
    {
        return static_cast<crd::u64>(sf.lp[c + 1] - sf.lp[c]);
    };

    // Chain-merge along the etree in postorder. The merged supernode's row
    // pattern is the genuine UNION of its fundamental members' patterns — a
    // parent's below-rows are NOT a subset of the child's (e.g. tridiagonal:
    // col j = {j,j+1}, parent j+1 = {j+1,j+2}), so the union, not the deepest
    // child, is the panel's row set. The group's columns are the union's
    // smallest |C| entries (each column's pattern starts at its own diagonal),
    // so they form the dense diagonal block in row-pattern order.
    auto union_into = [](const crd::containers::Array<crd::u32>& a, const crd::u32* b, crd::u32 bn,
                         crd::containers::Array<crd::u32>& dst)
    {
        dst.clear();
        crd::u32 i = 0;
        crd::u32 j = 0;
        const crd::u32 an = static_cast<crd::u32>(a.size());
        while (i < an && j < bn)
        {
            if (a[i] < b[j])
            {
                dst.push_back(a[i++]);
            }
            else if (a[i] > b[j])
            {
                dst.push_back(b[j++]);
            }
            else
            {
                dst.push_back(a[i++]);
                ++j;
            }
        }
        while (i < an)
        {
            dst.push_back(a[i++]);
        }
        while (j < bn)
        {
            dst.push_back(b[j++]);
        }
    };

    // Leading-column L pattern of fundamental supernode `fi`: from the compact `slead` (supernodal
    // symbolic, no full li) when present, else from the full `li`. Same ascending diagonal-first
    // entries either way ⇒ identical supernode amalgamation + numeric factor.
    const bool use_slead = !sf.slead_ptr.empty();
    auto lead_pat = [&](crd::u32 fi, const crd::u32*& ptr, crd::u32& cnt)
    {
        if (use_slead)
        {
            const crd::u32 b = sf.slead_ptr[fi];
            ptr = &sf.slead_idx[b];
            cnt = sf.slead_ptr[fi + 1] - b;
        }
        else
        {
            const crd::u32 fc = sf.super[fi];
            ptr = &sf.li[sf.lp[fc]];
            cnt = sf.lp[fc + 1] - sf.lp[fc];
        }
    };

    out.scol.push_back(0);
    out.srowp.push_back(0);
    crd::containers::Array<crd::u32> merged(alloc);
    crd::containers::Array<crd::u32> cand(alloc);
    crd::u32 ns = 0;
    crd::u32 f = 0;
    while (f < nf)
    {
        const crd::u32 g = f;
        merged.clear();
        const crd::u32* gp = nullptr;
        crd::u32 gn = 0;
        lead_pat(g, gp, gn); // fundamental g's leading pattern (ascending, diagonal incl.)
        for (crd::u32 q = 0; q < gn; ++q)
        {
            merged.push_back(gp[q]);
        }
        crd::u64 c_cols = sf.super[g + 1] - sf.super[g];
        crd::u64 fund_struct = 0;
        for (crd::u32 c = sf.super[g]; c < sf.super[g + 1]; ++c)
        {
            fund_struct += colcount(c);
        }
        crd::u32 cur = g;
        while (cur + 1 < nf && parent_fs(cur) == cur + 1)
        {
            const crd::u32 nxt = cur + 1;
            const crd::u32* np = nullptr;
            crd::u32 nn = 0;
            lead_pat(nxt, np, nn);
            union_into(merged, np, nn, cand);
            const crd::u64 cp = c_cols + (sf.super[nxt + 1] - sf.super[nxt]);
            const crd::u64 pp = cand.size();
            crd::u64 fund_nxt = 0;
            for (crd::u32 c = sf.super[nxt]; c < sf.super[nxt + 1]; ++c)
            {
                fund_nxt += colcount(c);
            }
            const crd::u64 trap = cp * pp - cp * (cp - 1) / 2; // merged panel's lower trapezoid
            const crd::u64 extra = trap - (fund_struct + fund_nxt);
            if (extra > static_cast<crd::u64>(nrelax) * cp)
            {
                break;
            }
            merged.clear(); // accept: merged := cand
            for (crd::u32 k = 0; k < cand.size(); ++k)
            {
                merged.push_back(cand[k]);
            }
            c_cols = cp;
            fund_struct += fund_nxt;
            cur = nxt;
        }
        for (crd::u32 k = 0; k < merged.size(); ++k)
        {
            out.srow.push_back(merged[k]);
        }
        out.srowp.push_back(static_cast<crd::u32>(out.srow.size()));
        out.scol.push_back(sf.super[cur + 1]); // one past the group's last column
        ++ns;
        f = cur + 1;
    }
    out.nsuper = ns;
    out.col_super.resize(sf.n);
    for (crd::u32 s = 0; s < ns; ++s)
    {
        for (crd::u32 c = out.scol[s]; c < out.scol[s + 1]; ++c)
        {
            out.col_super[c] = s;
        }
    }
    // Factor fill = the lower TRAPEZOID per panel (column o carries |R_s|-o rows)
    // = |R_s|·cols - cols(cols-1)/2, comparable to sf.nnz() + the frontier fill
    // metric. (The dense-rectangle storage |R_s|·cols is a separate quantity the
    // numeric step allocates.) With amalgamation this includes the explicit zeros,
    // so it is always ≥ the un-amalgamated nnz(L).
    out.lnz = 0;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        const crd::u64 rg = out.srowp[s + 1] - out.srowp[s];
        const crd::u64 cols = out.scol[s + 1] - out.scol[s];
        out.lnz += rg * cols - cols * (cols - 1) / 2;
    }
    return out;
}

// =======================================================================
// v5a-1b steps 2-5 — numeric factorization + solve (left-looking
// supernodal). cmod uses dense `gemm` (build on our kernels); cdiv uses
// `factor_cholesky` for the diagonal block + a hand-rolled subdiagonal
// forward-solve (the lu.cpp inner-panel precedent). Pending updates routed
// via the CHOLMOD-style Lpos/Head/Next lists. Serial ⇒ deterministic; the
// cross-thread moat is proven under tree-parallelism in v5a-3.
// =======================================================================

template <typename T>
SupernodalCholesky<T>::SupernodalCholesky(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_sym(alloc), m_lx(alloc), m_lxp(alloc), m_lvl_ptr(alloc), m_lvl_list(alloc), m_upd_ptr(alloc),
      m_upd_list(alloc)
{
}

template <typename T>
void SupernodalCholesky<T>::factorize(const sparse::SparsePattern& pattern, crd::containers::ConstSpan<T> values,
                                      crd::u32 nrelax, crd::u32 num_workers, bool reuse_symbolic)
{
    m_info = 0;
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    g_scaleprof.reset();
    const auto fac_t0 = ScaleClock::now();
#endif
    // SYMBOLIC PHASE (the v5a CHOLMOD-gap cost — AMD + etree + supernode amalgamation). Skipped on
    // refactorize(): m_sym persists from the prior factorize() on the structurally-identical pattern, so the LM
    // normal-equations loop pays this ONCE across all λ-trials (the gate to matching Ceres's cached symbolic). The
    // cheap O(nnz) rebuilds below (panel layout, update-lists, etree levels) rerun every call — negligible vs the
    // numeric factor in the factorization-dominated crush regime. Bit-identical to a fresh factorize() (same m_sym).
    if (!reuse_symbolic)
    {
        const ordering::SymbolicFactor sf =
            ordering::symbolic_factorize(pattern, m_alloc, /*supernodal_patterns=*/true);
        m_sym = build_supernodal_symbolic(sf, m_alloc, nrelax);
    }
    else
    {
        // Reuse path (refactorize): the caller guarantees `pattern` is structurally identical to the analyzed one
        // (fixed-sparsity — the same contract Ceres relies on caching its symbolic). A dimension mismatch means the
        // numeric scatter would place values in the wrong panels → a SILENTLY-wrong factor; catch misuse here.
        CRD_ASSERT_MSG(m_sym.n == pattern.n_outer(),
                       "refactorize: pattern dimension differs from the analyzed symbolic (fixed-sparsity violated)");
    }
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    g_scaleprof.sym_wall_ns = scale_ns(fac_t0, ScaleClock::now());
#endif
    const SupernodalSymbolic& sym = m_sym;
    m_n = sym.n;
    m_lnz = sym.lnz;
    const crd::u32 ns = sym.nsuper;
    crd::u32 cdiv_block = kCdivBlock;
#ifdef CRD_HESAP_CHOL_PROFILE
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv — temp profiling only
#endif
    if (const char* cb = std::getenv("CRD_CDIV_BLOCK"))
    {
        cdiv_block = static_cast<crd::u32>(std::strtoul(cb, nullptr, 10));
    }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#endif

    m_lxp.resize(ns + 1);
    m_lxp[0] = 0;
    crd::u32 max_nr = 0;
    crd::u32 max_nc = 0;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        const crd::u32 nr = sym.srowp[s + 1] - sym.srowp[s];
        const crd::u32 nc = sym.scol[s + 1] - sym.scol[s];
        m_lxp[s + 1] = m_lxp[s] + static_cast<crd::u32>(static_cast<crd::u64>(nr) * nc);
        max_nr = nr > max_nr ? nr : max_nr;
        max_nc = nc > max_nc ? nc : max_nc;
    }
    m_lx.resize(m_lxp[ns]);
    for (crd::usize i = 0; i < m_lx.size(); ++i)
    {
        m_lx[i] = T{0};
    }
    if (ns == 0)
    {
        return;
    }

    // Static per-supernode update lists (parallel-safe — replaces the serial Head/Next
    // dynamic lists). upd_list[upd_ptr[s] .. upd_ptr[s+1]) = the descendant supernodes
    // K that contribute to s (ascending K), i.e. K has a below-diagonal row in s's
    // columns. Built in one pass over each K's below-pattern (rows ascending ⇒
    // col_super non-decreasing ⇒ distinct ancestors dedup by run). Read-only during
    // factorization ⇒ each supernode's cmod is independent (the v5a-3 parallelism).
    crd::containers::Array<crd::u32> upd_ptr(m_alloc);
    crd::containers::Array<crd::u32> upd_list(m_alloc);
    upd_ptr.resize(ns + 1);
    for (crd::u32 s = 0; s <= ns; ++s)
    {
        upd_ptr[s] = 0;
    }
    for (crd::u32 k = 0; k < ns; ++k)
    {
        const crd::u32 krb = sym.srowp[k];
        const crd::u32 knr = sym.srowp[k + 1] - krb;
        const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
        crd::u32 prev = ns; // sentinel
        for (crd::u32 i = knc; i < knr; ++i)
        {
            const crd::u32 anc = sym.col_super[sym.srow[krb + i]];
            if (anc != prev)
            {
                ++upd_ptr[anc + 1];
                prev = anc;
            }
        }
    }
    for (crd::u32 s = 0; s < ns; ++s)
    {
        upd_ptr[s + 1] += upd_ptr[s];
    }
    upd_list.resize(upd_ptr[ns]);
    {
        crd::containers::Array<crd::u32> ufill(m_alloc);
        ufill.resize(ns);
        for (crd::u32 s = 0; s < ns; ++s)
        {
            ufill[s] = upd_ptr[s];
        }
        for (crd::u32 k = 0; k < ns; ++k)
        {
            const crd::u32 krb = sym.srowp[k];
            const crd::u32 knr = sym.srowp[k + 1] - krb;
            const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
            crd::u32 prev = ns;
            for (crd::u32 i = knc; i < knr; ++i)
            {
                const crd::u32 anc = sym.col_super[sym.srow[krb + i]];
                if (anc != prev)
                {
                    upd_list[ufill[anc]++] = k;
                    prev = anc;
                }
            }
        }
    }
    // Per-worker scratch (v5a-3): relrow (global row → local panel row) + the cmod
    // Schur-update buffer, one slice per worker so concurrent supernodes never share.
    // Size by jobs::num_workers() (the pool worker_index() ranges over), index by
    // worker_index() — per feedback_jobs_worker_index_aliasing. Serial ⇒ 1 slice.
    const crd::u32 sw = (num_workers <= 1) ? 1U : crd::jobs::num_workers();
    const crd::usize ustride = static_cast<crd::usize>(max_nr) * max_nc;
    crd::containers::Array<crd::u32> relrow(m_alloc);
    relrow.resize(static_cast<crd::usize>(sw) * m_n);
    for (crd::usize i = 0; i < relrow.size(); ++i)
    {
        relrow[i] = m_n; // sentinel
    }
    // Per-worker lrmap: descendant-row → target-local-row, precomputed once per cmod
    // descendant so the scatter-subtract inner loop reads lrmap[pr] (1 load) instead of
    // rr[srow[..]] (2 loads) recomputed m1× per row.
    crd::containers::Array<crd::u32> relmap(m_alloc);
    relmap.resize(static_cast<crd::usize>(sw) * max_nr);
    // PER-WORKER slices, one Array each (2026-06-11, the lat32@16T find): a single sw*ustride
    // block exceeds the TLSF structural ~4 GB per-chunk cap at 16 workers on n~100K matrices
    // (16 x max_nr*max_nc*8B ~ 4.6 GB) -> GrowableTlsfAllocator OOM. Each per-worker slice is
    // a few hundred MB and always fits; the slice pointers replace the old w*ustride indexing.
    crd::containers::Array<crd::containers::Array<T>> ubufs(m_alloc);
    // UNINITIALIZED — ubuf is per-worker GEMM/copy scratch: every element is WRITTEN before it is read
    // (gemm beta=0 overwrites its output block; the cmod-syrk path zeroes its lower triangle at 796 before
    // the syrk accumulates and reads only that range at 810; the (A2)/(B) paths copy-then-read their used
    // sub-range). The value-initialising resize() memset of this ~sw·max_nr·max_nc buffer (~2.2 GB / ~180 ms
    // on lat32) was 13% of the factor wall AND re-paid every refactorize under LM. Audited write-before-read
    // for all consumers; NaN-poison + ASan validated. (A read-before-write here is a UMR the {1..16} moat
    // CANNOT catch — reused resident pages read coincidentally-identical bytes — so this is audit-gated.)
    ubufs.reserve(sw);
    for (crd::u32 i = 0; i < sw; ++i)
    {
        ubufs.emplace_back(m_alloc).resize_uninitialized(ustride);
    }
    // Per-worker bw×bw triangular-inverse scratch (cdiv below-block BLAS-3 path):
    // L11⁻¹ of the diagonal block, so the below-block solve L21 = A21·L11⁻ᴴ is a gemm.
    crd::containers::Array<T> linvbuf(m_alloc);
    const crd::usize linv_stride = static_cast<crd::usize>(cdiv_block) * cdiv_block;
    linvbuf.resize(static_cast<crd::usize>(sw) * linv_stride);
    std::atomic<crd::u32> fail_col{0xFFFFFFFFU};

    // Factor one supernode into its OWN panel: scatter A + cmod (reads descendants'
    // already-factored panels, fixed ascending-K order ⇒ deterministic) + in-place
    // cdiv. Writes only panel s + the worker's scratch ⇒ race-free across supernodes.
    // `par_workers` > 1 ⇒ this supernode is factored NODE-PARALLEL (it sits in a thin
    // etree level): its big panel gemms use `gemm_parallel` across the idle worker pool.
    // par_workers ≤ 1 ⇒ serial gemm (the tree-parallel path; one worker per supernode).
    // gemm_parallel ≡ serial gemm bit-for-bit ⇒ the factor is identical either way.
    auto chol_gemm = [&](T alpha, dense::MatrixView<const T, dense::Layout::ColMajor> a,
                         dense::MatrixView<const T, dense::Layout::ColMajor> b, T beta,
                         dense::MatrixView<T, dense::Layout::ColMajor> c, dense::Trans ta, dense::Trans tb,
                         crd::u32 par_workers)
    {
        const crd::u64 flop = static_cast<crd::u64>(2) * c.rows() * c.cols() * a.cols();
        if (par_workers > 1 && flop >= kGemmParallelMinFlop)
        {
            dense::gemm_parallel<T, dense::Layout::ColMajor>(par_workers, alpha, a, b, beta, c, ta, tb, nullptr);
            crd::jobs::frame_reset(); // reclaim this gemm's JobDecls (node-parallel runs many serially on main)
        }
        else
        {
            dense::gemm<T, dense::Layout::ColMajor>(alpha, a, b, beta, c, ta, tb, nullptr);
        }
    };
    auto factor_one = [&](crd::u32 s, crd::u32 worker, crd::u32 par_workers)
    {
        crd::u32* rr = relrow.data() + static_cast<crd::usize>(worker) * m_n;
        T* ub = ubufs[worker].data();
        T* linv = linvbuf.data() + static_cast<crd::usize>(worker) * linv_stride; // cdiv L11⁻¹ scratch
        // (cmod's scatter row-map now lives in cmod_slab as per-LANE lrm_w; the old front-worker
        // lrm was unused once cmod went row-slab-parallel.)
        const crd::u32 rb = sym.srowp[s];
        const crd::u32 nr = sym.srowp[s + 1] - rb;
        const crd::u32 firstcol = sym.scol[s];
        const crd::u32 nc = sym.scol[s + 1] - firstcol;
        T* panel = &m_lx[m_lxp[s]];
        for (crd::u32 a = 0; a < nr; ++a)
        {
            rr[sym.srow[rb + a]] = a;
        }
        // Panels are COLUMN-MAJOR (ld = nr): entry (row, col) at panel[col*nr + row].
        // ColMajor gives contiguous COLUMNS, which the right-looking cdiv writes and the
        // solve's subdiagonal axpy read as long SIMD runs (the solve-crush layout).
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t0 = ProfClock::now();
#endif
        for (crd::u32 c = firstcol; c < firstcol + nc; ++c) // scatter A's lower triangle
        {
            const crd::u32 lc = c - firstcol;
            const crd::u32 st = pattern.outer_ptr[c];
            const crd::u32 cnt = pattern.inner_count(c);
            for (crd::u32 kk = 0; kk < cnt; ++kk)
            {
                const crd::u32 i = pattern.inner_idx[st + kk];
                if (i >= c)
                {
                    // outer = rows (CSR): this reads A[c][i] (i>=c, upper triangle by row).
                    // L's column c needs the LOWER-triangle source A[i][c] = conj(A[c][i])
                    // for Hermitian (identity for real-symmetric).
                    panel[static_cast<crd::usize>(lc) * nr + rr[i]] = chol_conj<T>(values[st + kk]);
                }
            }
        }
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t1 = ProfClock::now();
        g_cholprof.scatter_ns += prof_ns(prof_t0, prof_t1);
#endif
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
        const auto sp_cmod0 = ScaleClock::now();
#endif
        // cmod — Schur updates from descendants. NODE-PARALLEL fronts (par_workers>1) partition the
        // front's ROWS across the worker pool with ONE fork per front — NOT gemm_parallel per
        // descendant, which paid fork/join per call and capped the huge-front cmod scaling at ~1.86×
        // (measured). The cmod gemm SHAPE parallelizes 4.5–6.8× under OpenBLAS (ob_probe), so the cap
        // was Cerid's per-descendant fork/join, not the shape. Each lane owns a front-local row-slab
        // [r0,r1); for every descendant it does the sub-gemm over the descendant rows mapping into the
        // slab — a CONTIGUOUS pr-range because lrm[pr]=rr[srow[..]] is strictly increasing (rr is the
        // position-in-sorted-front-rows; the descendant's rows are a sorted subset) — and scatters
        // ONLY into its own rows. BIT-IDENTICAL to serial: each front row is owned by exactly one lane
        // and its subtracts are applied in ascending-descendant order = the serial order; each gemm
        // output element's K-reduction is independent of the row-slab (the property gemm_parallel
        // already relies on). rr is filled once (this front's worker slot, 615–618) and read-only
        // across lanes; ub/lrm are per-LANE scratch (lane worker_index, never the front's worker).
        // col_limit caps the descendant columns considered to s's columns [0, col_limit) — used by the
        // node-parallel DIAGONAL pass (col_limit = the band's r1) to skip the cross-band upper triangle (the
        // node-parallel cmod-syrk saving); = nc for the serial path and the below pass (no restriction). Writes
        // into the within-band-square upper are dead storage (panel upper triangle, never read) ⇒ harmless.
        auto cmod_slab = [&](crd::u32 r0, crd::u32 r1, crd::u32 w, crd::u32 col_limit)
        {
            T* ub_w = ubufs[w].data();
            crd::u32* lrm_w = relmap.data() + static_cast<crd::usize>(w) * max_nr;
            const crd::u32 g_lo = sym.srow[rb + r0]; // slab global-row lower bound (inclusive)
            const crd::u32 g_hi = (r1 < nr) ? sym.srow[rb + r1] : (sym.srow[rb + nr - 1] + 1); // exclusive
            for (crd::u32 ui = upd_ptr[s]; ui < upd_ptr[s + 1]; ++ui)
            {
                const crd::u32 k = upd_list[ui];
                const crd::u32 krb = sym.srowp[k];
                const crd::u32 knr = sym.srowp[k + 1] - krb;
                const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
                const T* kpanel = &m_lx[m_lxp[k]];
                crd::u32 lo = knc;
                crd::u32 hi = knr;
                while (lo < hi) // p0 = first K-row ≥ firstcol
                {
                    const crd::u32 mid = lo + (hi - lo) / 2;
                    if (sym.srow[krb + mid] < firstcol)
                    {
                        lo = mid + 1;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                const crd::u32 p0 = lo;
                crd::u32 m1 = 0;
                while (p0 + m1 < knr && sym.srow[krb + p0 + m1] < firstcol + col_limit)
                {
                    ++m1;
                }
                const crd::u32 msz = knr - p0;
                // Contiguous pr sub-range of this descendant's U-rows (srow[krb+p0 .. krb+knr), sorted)
                // mapping into the slab's global-row window [g_lo, g_hi). Binary-search both ends.
                crd::u32 a = 0;
                crd::u32 b = msz;
                while (a < b)
                {
                    const crd::u32 m = a + (b - a) / 2;
                    if (sym.srow[krb + p0 + m] < g_lo)
                    {
                        a = m + 1;
                    }
                    else
                    {
                        b = m;
                    }
                }
                const crd::u32 pr_lo = a;
                b = msz;
                a = pr_lo;
                while (a < b)
                {
                    const crd::u32 m = a + (b - a) / 2;
                    if (sym.srow[krb + p0 + m] < g_hi)
                    {
                        a = m + 1;
                    }
                    else
                    {
                        b = m;
                    }
                }
                const crd::u32 pr_hi = a;
                if (pr_lo >= pr_hi)
                {
                    continue; // descendant touches no row in this slab
                }
                const crd::u32 sub = pr_hi - pr_lo;
                for (crd::u32 pr = pr_lo; pr < pr_hi; ++pr) // descendant-row → target-local-row (both paths)
                {
                    lrm_w[pr] = rr[sym.srow[krb + p0 + pr]];
                }
                // v7-e-2 SYRK: on the SERIAL path (par_workers≤1 ⇒ pr_lo=0, sub=msz = the descendant's whole pattern)
                // with a large m1, the descendant's update to s's panel splits into the SYMMETRIC m1×m1 DIAGONAL
                // block (s-cols × s-cols = am1·am1ᵀ) + a rectangular BELOW block. The diagonal is computed
                // LOWER-TRIANGLE-ONLY via the packed syrk microkernel (half flops) — this removes the gate-measured
                // ~81e9 redundancy (= the WHOLE 1.39× excess) that the old single full-gemm paid. Node-parallel
                // (par_workers>1) keeps the full gemm below (byte-identical ⇒ FEA 8T wins untouched). Determinism:
                // serial uses syrk, parallel uses gemm, but both produce the SAME lower-triangle values (the same
                // Σ_k am1[·,k]am1[·,k] reduction) and the 1T moat compares serial-vs-serial run-twice; the v5a-4
                // cross-thread moat huge fronts go node-parallel (full gemm) ⇒ unaffected.
                if constexpr (std::is_same_v<T, crd::f64>)
                {
                    if (par_workers <= 1 && m1 >= kCmodSyrkMin)
                    {
                        // DIAGONAL block: ub_w (m1×m1) ← zero lower, syrk_lower_minus(am1·am1ᵀ) ⇒ lower = -U_diag,
                        // then scatter the lower triangle into s's panel (+=, since ub_w holds the negated update).
                        const dense::MatrixView<const T, dense::Layout::ColMajor> am1c(
                            kpanel + static_cast<crd::usize>(p0), m1, knc, knr); // am1 (m1×knc) ColMajor
                        for (crd::u32 i = 0; i < m1; ++i)
                        {
                            for (crd::u32 j = 0; j <= i; ++j)
                            {
                                ub_w[static_cast<crd::usize>(i) * m1 + j] = T{0};
                            }
                        }
                        const dense::MatrixView<T, dense::Layout::ColMajor> cdg(ub_w, m1, m1, m1);
#ifdef CRD_HESAP_CHOL_PROFILE
                        const auto pf_s0 = ProfClock::now();
#endif
                        // scratch=nullptr ⇒ thread-safe default_allocator (tree-parallel workers call this
                        // concurrently — m_alloc is NOT concurrent-safe; matches the cmod gemm's nullptr).
                        dense::detail::syrk_lower_minus<T, dense::Layout::ColMajor>(am1c, cdg, nullptr,
                                                                                    /*allow_parallel=*/false);
#ifdef CRD_HESAP_CHOL_PROFILE
                        const auto pf_s1 = ProfClock::now();
                        {
                            const int bn = CholProf::knc_bin(knc);
                            const crd::u64 fl = static_cast<crd::u64>(m1) * (m1 + 1) * knc;
                            g_cholprof.cmod_ns_bin[bn] += prof_ns(pf_s0, pf_s1);
                            g_cholprof.cmod_flop_bin[bn] += fl;
                            g_cholprof.cmod_flops += fl;
                            ++g_cholprof.cmod_calls;
                        }
#endif
                        for (crd::u32 pc = 0; pc < m1; ++pc)
                        {
                            const crd::u32 gcol = sym.srow[krb + p0 + pc];
                            T* pcoldst = panel + static_cast<crd::usize>(gcol - firstcol) * nr;
                            for (crd::u32 pr = pc; pr < m1; ++pr) // lower triangle pr ≥ pc
                            {
                                pcoldst[lrm_w[pr]] += ub_w[static_cast<crd::usize>(pr) * m1 + pc];
                            }
                        }
#ifdef CRD_HESAP_CHOL_PROFILE
                        const auto pf_s2 = ProfClock::now();
                        g_cholprof.cmod_scatter_ns += prof_ns(pf_s1, pf_s2);
#endif
                        // BELOW block: rows [m1:msz] (= [m1:sub] since sub=msz here). Uᵀ_below = am1·am_belowᵀ.
                        const crd::u32 nb = sub - m1;
                        if (nb > 0)
                        {
                            const dense::MatrixView<const T, dense::Layout::RowMajor> am1r(
                                kpanel + static_cast<crd::usize>(p0), knc, m1, knr);
                            const dense::MatrixView<const T, dense::Layout::RowMajor> ambr(
                                kpanel + static_cast<crd::usize>(p0 + m1), knc, nb, knr);
                            const dense::MatrixView<T, dense::Layout::RowMajor> ubr(ub_w, m1, nb, nb);
#ifdef CRD_HESAP_CHOL_PROFILE
                            const auto pf_b0 = ProfClock::now();
#endif
                            dense::gemm<T, dense::Layout::RowMajor>(
                                T{1}, am1r, ambr, T{0}, ubr, dense::Trans::Transpose, dense::Trans::None, nullptr);
#ifdef CRD_HESAP_CHOL_PROFILE
                            const auto pf_b1 = ProfClock::now();
                            {
                                const int bn = CholProf::knc_bin(knc);
                                const crd::u64 fl = 2ULL * m1 * nb * knc;
                                g_cholprof.cmod_ns_bin[bn] += prof_ns(pf_b0, pf_b1);
                                g_cholprof.cmod_flop_bin[bn] += fl;
                                g_cholprof.cmod_flops += fl;
                                ++g_cholprof.cmod_calls;
                                g_cholprof.cmod_hk_m1_flop[CholProf::m1_bin(m1)] += fl;
                                ++g_cholprof.cmod_hk_m1_count[CholProf::m1_bin(m1)];
                            }
#endif
                            for (crd::u32 pc = 0; pc < m1; ++pc)
                            {
                                const crd::u32 gcol = sym.srow[krb + p0 + pc];
                                T* pcoldst = panel + static_cast<crd::usize>(gcol - firstcol) * nr;
                                const T* ubc = ub_w + static_cast<crd::usize>(pc) * nb;
                                for (crd::u32 prb = 0; prb < nb; ++prb)
                                {
                                    pcoldst[lrm_w[m1 + prb]] -= ubc[prb];
                                }
                            }
#ifdef CRD_HESAP_CHOL_PROFILE
                            g_cholprof.cmod_scatter_ns += prof_ns(pf_b1, ProfClock::now());
#endif
                        }
                    }
                    else
                    {
                        // f64 REROUTE: Uᵀ(m1×sub) = am1·am[slab]ᵀ on the fast RowMajor-C path (full, no symmetry).
                        const dense::MatrixView<const T, dense::Layout::RowMajor> am1r(
                            kpanel + static_cast<crd::usize>(p0), knc, m1, knr);
                        const dense::MatrixView<const T, dense::Layout::RowMajor> amr(
                            kpanel + static_cast<crd::usize>(p0 + pr_lo), knc, sub, knr);
                        const dense::MatrixView<T, dense::Layout::RowMajor> utr(ub_w, m1, sub, sub);
#ifdef CRD_HESAP_CHOL_PROFILE
                        const auto pf_g0 = ProfClock::now();
#endif
                        dense::gemm<T, dense::Layout::RowMajor>(T{1}, am1r, amr, T{0}, utr, dense::Trans::Transpose,
                                                                dense::Trans::None, nullptr);
#ifdef CRD_HESAP_CHOL_PROFILE
                        const auto pf_g1 = ProfClock::now();
                        {
                            const int bn = CholProf::knc_bin(knc);
                            const crd::u64 fl = 2ULL * m1 * sub * knc;
                            g_cholprof.cmod_ns_bin[bn] += prof_ns(pf_g0, pf_g1);
                            g_cholprof.cmod_flop_bin[bn] += fl;
                            g_cholprof.cmod_flops += fl;
                            ++g_cholprof.cmod_calls;
                            g_cholprof.cmod_hk_m1_flop[CholProf::m1_bin(m1)] += fl;
                            ++g_cholprof.cmod_hk_m1_count[CholProf::m1_bin(m1)];
                        }
#endif
                        for (crd::u32 pc = 0; pc < m1; ++pc)
                        {
                            const crd::u32 gcol = sym.srow[krb + p0 + pc];
                            T* pcoldst = panel + static_cast<crd::usize>(gcol - firstcol) * nr;
                            const T* ubc = ub_w + static_cast<crd::usize>(pc) * sub;
                            for (crd::u32 pr = pr_lo; pr < pr_hi; ++pr)
                            {
                                pcoldst[lrm_w[pr]] -= ubc[pr - pr_lo];
                            }
                        }
#ifdef CRD_HESAP_CHOL_PROFILE
                        g_cholprof.cmod_scatter_ns += prof_ns(pf_g1, ProfClock::now());
#endif
                    }
                }
                else
                {
                    const dense::MatrixView<const T, dense::Layout::ColMajor> am(
                        kpanel + static_cast<crd::usize>(p0 + pr_lo), sub, knc, knr);
                    const dense::MatrixView<const T, dense::Layout::ColMajor> am1(kpanel + static_cast<crd::usize>(p0),
                                                                                  m1, knc, knr);
                    const dense::MatrixView<T, dense::Layout::ColMajor> uvw(ub_w, sub, m1,
                                                                            sub); // U sub ColMajor ld=sub
                    dense::gemm<T, dense::Layout::ColMajor>(T{1}, am, am1, T{0}, uvw, dense::Trans::None,
                                                            kCholAdjoint<T>, nullptr);
                    for (crd::u32 pc = 0; pc < m1; ++pc)
                    {
                        const crd::u32 gcol = sym.srow[krb + p0 + pc];
                        T* pcoldst = panel + static_cast<crd::usize>(gcol - firstcol) * nr;
                        const T* ubc = ub_w + static_cast<crd::usize>(pc) * sub;
                        for (crd::u32 pr = pr_lo; pr < pr_hi; ++pr)
                        {
                            pcoldst[lrm_w[pr]] -= ubc[pr - pr_lo];
                        }
                    }
                }
            }
        };
        if (par_workers <= 1)
        {
            cmod_slab(0, nr, worker, nc); // serial / tree-parallel: one slab = the whole front (full col range)
        }
        else
        {
            // Node-parallel huge front — v7-e-2 TWO-PASS symmetry split. The DIAGONAL block (front rows [0:nc] =
            // s's columns) is partitioned by the BALANCED-triangular primitive (work per diagonal row ∝ row;
            // equal-count row-slabs imbalance ~W× and REGRESSED earlier), each band passing col_limit=r1 so it
            // skips the cross-band upper triangle (the cmod-syrk saving at 8T). The strictly-BELOW rows [nc:nr]
            // are rectangular (uniform work) ⇒ the regular equal-count parallel_for, full col range. Each panel
            // entry is written by exactly one pass/band (its row's owner) with the same K-reduction as serial ⇒
            // bit-identical, {1..16} moat holds.
            dense::detail::parallel_for_triangular(nc, par_workers, [&](crd::u32 r0, crd::u32 r1, crd::u32 lane)
                                                   { cmod_slab(r0, r1, lane, r1); });
            if (nr > nc)
            {
                auto* counter =
                    crd::jobs::parallel_for(nr - nc, par_workers, [&](crd::u32 bb, crd::u32 ee)
                                            { cmod_slab(nc + bb, nc + ee, crd::jobs::worker_index(), nc); });
                crd::jobs::wait(counter);
                crd::jobs::frame_reset();
            }
        }
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
        if (par_workers > 1)
        {
            g_scaleprof.np_cmod_ns += scale_ns(sp_cmod0, ScaleClock::now()); // between-supernode assembly
        }
#endif
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t2 = ProfClock::now();
        g_cholprof.cmod_ns += prof_ns(prof_t1, prof_t2);
        const int prof_cdiv_bin = CholProf::cdiv_bin(nc);
        crd::u64 prof_cdiv_flop = 0;
        for (crd::u32 jp = 0; jp < nc; ++jp) // lower-trapezoid Cholesky multiply-adds
        {
            prof_cdiv_flop += (nr - jp - 1); // scale
            for (crd::u32 jjp = jp + 1; jjp < nc; ++jjp)
            {
                prof_cdiv_flop += static_cast<crd::u64>(2) * (nr - jjp); // rank-1
            }
        }
        g_cholprof.cdiv_flops += prof_cdiv_flop;
        g_cholprof.cdiv_flop_bin[prof_cdiv_bin] += prof_cdiv_flop;
#endif
        // cdiv — factor the supernode panel. The old whole-panel unblocked rank-1 sweep
        // was O(nc²·nr) BLAS-1 (AI≈0.33) — the CHOLMOD per-thread gap on fat supernodes
        // (bmwcra maxnc=2406). Two paths: THIN supernodes (nc ≤ kCdivScalarMin) keep the
        // pure-scalar rank-1 (negligible flops, but numerous ⇒ no per-supernode BLAS-3
        // overhead); FAT supernodes go BLOCKED — scalar bw×bw diagonal + BLAS-3 below-block
        // (L21 = A21·L11⁻ᴴ via invert(L11)+gemm, the AI win) + BLAS-3 trailing Schur gemm.
        bool cdiv_failed = false;
        if (nc <= kCdivScalarMin)
        {
            for (crd::u32 j = 0; j < nc; ++j) // THIN: full-panel scalar rank-1 (pre-v5a-4 path)
            {
                const crd::usize cj = static_cast<crd::usize>(j) * nr;
                const dense::RealType<T> djj = chol_real<T>(panel[cj + j]);
                if (!(djj > dense::RealType<T>{0}))
                {
                    const crd::u32 fc = firstcol + j + 1;
                    crd::u32 cur = fail_col.load(std::memory_order_relaxed);
                    while (fc < cur && !fail_col.compare_exchange_weak(cur, fc, std::memory_order_relaxed))
                    {
                    }
                    cdiv_failed = true;
                    break;
                }
                const dense::RealType<T> d = std::sqrt(djj);
                panel[cj + j] = chol_from_real<T>(d);
                const dense::RealType<T> invd = dense::RealType<T>{1} / d;
                for (crd::u32 i = j + 1; i < nr; ++i)
                {
                    panel[cj + i] *= invd;
                }
                for (crd::u32 jj = j + 1; jj < nc; ++jj)
                {
                    const crd::usize cjj = static_cast<crd::usize>(jj) * nr;
                    const T f = chol_conj<T>(panel[cj + jj]);
                    for (crd::u32 i = jj; i < nr; ++i)
                    {
                        panel[cjj + i] -= panel[cj + i] * f;
                    }
                }
            }
        }
        else
        {
            // TWO-LEVEL blocked panel Cholesky (LAPACK xPOTRF structure; v5a-4 cdiv crush).
            // OUTER block obw (= cdiv_block) is the trailing Schur gemm's K dimension (step B):
            // wide ⇒ high arithmetic intensity + few large gemm calls. The OUTER panel
            // [ko:koend] (diagonal + below over ALL rows) is itself factored by an INNER
            // kCdivInnerBlock-blocked right-looking sweep (step A) whose ONLY scalar work is the
            // ibw×ibw POTF2 + ibw×ibw invert — total O(nc·ibw²), INDEPENDENT of obw. This is what
            // the naive single-level sweep lacked: its O(obw²·nc) scalar diagonal capped it at
            // bw=128 (bw=256 measured WORSE). Every gemm is bit-identical serial/parallel
            // (gemm_parallel ≡ serial, disjoint row-slabs) ⇒ the cross-thread determinism moat holds.
            const crd::u32 inner_bw = kCdivInnerBlock;
            for (crd::u32 ko = 0; ko < nc && !cdiv_failed; ko += cdiv_block)
            {
                const crd::u32 obw = (ko + cdiv_block < nc) ? cdiv_block : (nc - ko);
                const crd::u32 koend = ko + obw;
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
                const auto sp_ko0 = ScaleClock::now();
#endif
                // (A) factor the outer panel P[ko:nr, ko:koend], INNER-blocked right-looking.
                for (crd::u32 ki = ko; ki < koend && !cdiv_failed; ki += inner_bw)
                {
                    const crd::u32 ibw = (ki + inner_bw < koend) ? inner_bw : (koend - ki);
                    const crd::u32 iend = ki + ibw;
                    // (A1) scalar POTF2 of the ibw×ibw diagonal block [ki:iend, ki:iend].
                    for (crd::u32 j = ki; j < iend; ++j)
                    {
                        const crd::usize cj = static_cast<crd::usize>(j) * nr;
                        const dense::RealType<T> djj = chol_real<T>(panel[cj + j]);
                        if (!(djj > dense::RealType<T>{0}))
                        {
                            const crd::u32 fc = firstcol + j + 1;
                            crd::u32 cur = fail_col.load(std::memory_order_relaxed);
                            while (fc < cur && !fail_col.compare_exchange_weak(cur, fc, std::memory_order_relaxed))
                            {
                            }
                            cdiv_failed = true;
                            break;
                        }
                        const dense::RealType<T> d = std::sqrt(djj);
                        panel[cj + j] = chol_from_real<T>(d);
                        const dense::RealType<T> invd = dense::RealType<T>{1} / d;
                        for (crd::u32 i = j + 1; i < iend; ++i) // scale WITHIN the inner diagonal block
                        {
                            panel[cj + i] *= invd;
                        }
                        for (crd::u32 jj = j + 1; jj < iend; ++jj) // rank-1 WITHIN the inner diagonal block
                        {
                            const crd::usize cjj = static_cast<crd::usize>(jj) * nr;
                            const T f = chol_conj<T>(panel[cj + jj]);
                            for (crd::u32 i = jj; i < iend; ++i)
                            {
                                panel[cjj + i] -= panel[cj + i] * f;
                            }
                        }
                    }
                    if (cdiv_failed)
                    {
                        break;
                    }
                    // (A2) below-solve WITHIN the obw block: [iend:koend]×[ki:iend] = A·L11⁻ᴴ (K=ibw).
                    // (Below-OUTER rows [koend:nr] are deferred to the blocked trsm (B) — at K=obw.)
                    const crd::u32 below = koend - iend;
                    if (below > 0)
                    {
                        invert_lower_tri<T>(panel + static_cast<crd::usize>(ki) * nr + ki, nr, ibw, linv);
                        T* a21 = panel + static_cast<crd::usize>(ki) * nr + iend; // L[iend:koend, ki:iend], ld=nr
                        for (crd::u32 c = 0; c < ibw; ++c) // copy A21 (below×ibw) → ub; avoids gemm alias
                        {
                            const T* src = a21 + static_cast<crd::usize>(c) * nr;
                            T* dst = ub + static_cast<crd::usize>(c) * below;
                            for (crd::u32 r = 0; r < below; ++r)
                            {
                                dst[r] = src[r];
                            }
                        }
                        const dense::MatrixView<const T, dense::Layout::ColMajor> a21v(ub, below, ibw, below);
                        const dense::MatrixView<const T, dense::Layout::ColMajor> linvv(linv, ibw, ibw, ibw);
                        const dense::MatrixView<T, dense::Layout::ColMajor> l21v(a21, below, ibw, nr);
                        // FORCE SERIAL (par_workers→1): A2 is bounded small (below≤obw−ibw≤192, K=ibw=64), so
                        // node-parallel gemm_parallel here only paid fork/join — a chief 8T cdiv-chain inflator.
                        chol_gemm(T{1}, a21v, linvv, T{0}, l21v, dense::Trans::None, kCholAdjoint<T>, 1U);
                    }
                    // (A3) trailing update WITHIN the obw block: cols [iend:koend], rows [iend:koend] (K=ibw).
                    if (iend < koend)
                    {
                        const crd::u32 tr = koend - iend;                                  // within-obw rows AND cols
                        const T* a_base = panel + static_cast<crd::usize>(ki) * nr + iend; // L[iend:koend, ki:iend]
                        T* c_base = panel + static_cast<crd::usize>(iend) * nr + iend;     // P[iend:koend, iend:koend]
                        const dense::MatrixView<const T, dense::Layout::ColMajor> av(a_base, tr, ibw, nr);
                        const dense::MatrixView<const T, dense::Layout::ColMajor> bv(a_base, tr, ibw, nr);
                        const dense::MatrixView<T, dense::Layout::ColMajor> cv(c_base, tr, tr, nr);
                        // FORCE SERIAL (par_workers→1): A3 within-obw trailing is bounded small (tr≤obw−ibw≤192,
                        // K=ibw=64) ⇒ gemm_parallel only paid fork/join (8T cdiv-chain inflator).
                        chol_gemm(T{-1}, av, bv, T{1}, cv, dense::Trans::None, kCholAdjoint<T>, 1U);
                    }
                }
                if (cdiv_failed)
                {
                    break;
                }
                const crd::u32 below_o = nr - koend;
                // (B) the staged jb walk in WIDE-N RowMajor IN-PLACE form (the lattice-crush TRSM lever,
                // 2026-06-11, PACKED-TRSM DRIVER): the previous gemm-driver forms (staged jb walk, wide-N
                // RowMajor in-place, whole-obw fused inverse) all re-streamed X or L3-resident packs and
                // capped at ~50 GF/s. This is the OpenBLAS-class structure instead: each MR-row panel of X
                // stays RESIDENT (L1) across the whole triangular walk —
                //   pack the panel rows once → for each 64-block q ascending: UPDATE the q-cols from the
                //   panel's OWN already-solved cols (micro tiles, K=kacc, A read in place at lda=obw) then
                //   SOLVE them against the packed inverted diagonal (K=qw) → write the panel back once.
                // The L11-derived operands (−W packs would change rounding ⇒ W and linv64 packed as-is,
                // conj-transposed into the microkernel's Bc format) are built ONCE per outer block on the
                // dispatcher (shared READ-ONLY across lanes). BIT-IDENTICAL by construction: every element
                // keeps the staged reduction — zero-init micro, the same gemm_microkernel p-ascending
                // single-K sweep (kacc ≤ obw−ibw and qw ≤ ibw are single chunks), merge c −= micro (update)
                // / c = micro (solve), ascending q. Row chunking never touches a reduction ⇒ bit-identical
                // across row slabs and worker counts.
                constexpr crd::u32 max_bblk = kCdivBlock / kCdivInnerBlock; // ≤ 4 inner blocks per outer
                crd::usize wpack_off[max_bblk] = {};                        // offsets into the front's linv slot
                crd::usize lpack_off[max_bblk] = {};
                if (below_o > 0)
                {
                    // Shared per-block pack into the front worker's linv slot (capacity kCdivBlock²):
                    // for block q: wpack = conj(W)ᵀ in Bc col-panel format (kacc×qw) + lpack = conj(linv_q)ᵀ
                    // (qw×qw). Worst case Σ = obw²/2 + obw·ibw ≤ kCdivBlock² ✓. `ub` is free as invert tmp.
                    crd::usize off = 0;
                    crd::u32 t = 0;
                    for (crd::u32 q = 0; q < obw; q += inner_bw, ++t)
                    {
                        const crd::u32 qw = (q + inner_bw < obw) ? inner_bw : (obw - q);
                        const crd::u32 kacc = q;
                        if (kacc > 0)
                        {
                            wpack_off[t] = off;
                            const T* wbase = panel + static_cast<crd::usize>(ko) * nr + (ko + q); // W rows at col ko
                            const dense::MatrixView<const T, dense::Layout::ColMajor> wv(wbase, qw, kacc, nr);
                            dense::detail::pack_b(wv, 0, 0, kacc, qw, kCholAdjoint<T>, linv + off);
                            off += static_cast<crd::usize>((qw + dense::detail::GemmTraits<T>::NR - 1) /
                                                           dense::detail::GemmTraits<T>::NR) *
                                   dense::detail::GemmTraits<T>::NR * kacc;
                        }
                        invert_lower_tri<T>(panel + static_cast<crd::usize>(ko + q) * nr + (ko + q), nr, qw, ub);
                        lpack_off[t] = off;
                        const dense::MatrixView<const T, dense::Layout::ColMajor> lv(ub, qw, qw, qw);
                        dense::detail::pack_b(lv, 0, 0, qw, qw, kCholAdjoint<T>, linv + off);
                        off += static_cast<crd::usize>((qw + dense::detail::GemmTraits<T>::NR - 1) /
                                                       dense::detail::GemmTraits<T>::NR) *
                               dense::detail::GemmTraits<T>::NR * qw;
                    }
                }
                auto b_slab = [&](crd::u32 r0, crd::u32 r1, crd::u32 w)
                {
                    constexpr crd::usize mr = dense::detail::GemmTraits<T>::MR;
                    constexpr crd::usize nrr = dense::detail::GemmTraits<T>::NR;
                    // Row-block = 96 rows (16 f64 / 12 f32 micro-panels): big enough that each strided
                    // column read/write streams 96 contiguous elements (the MR-sized block was 48 B per
                    // 43 KB jump — pack-latency-bound, measured no gain), small enough that pbuf
                    // (96×obw ≤ 192 KB f64) stays L2-resident across the whole triangular walk.
                    constexpr crd::u32 row_blk = 96;
                    static_assert(row_blk % mr == 0, "row block must be a whole number of micro-panels");
                    T* ub_w = ubufs[w].data();
                    T* pbuf = ub_w; // RB×obw resident row-block (row-major, lda=obw)
                    T* snap = ub_w + static_cast<crd::usize>(row_blk) * kCdivBlock; // MR×ibw pre-solve snapshot
                    for (crd::u32 rc = r0; rc < r1; rc += row_blk)
                    {
                        const crd::u32 rows_blk = (rc + row_blk <= r1) ? row_blk : (r1 - rc);
                        T* x0 = panel + static_cast<crd::usize>(ko) * nr + koend + rc; // X rows rc.., cols ko..
                        for (crd::u32 p = 0; p < obw; ++p) // pack the row block (96-long contiguous column reads)
                        {
                            const T* src = x0 + static_cast<crd::usize>(p) * nr;
                            for (crd::u32 i = 0; i < rows_blk; ++i)
                            {
                                pbuf[static_cast<crd::usize>(i) * obw + p] = src[i];
                            }
                            for (crd::u32 i = rows_blk; i < row_blk; ++i) // zero-pad tail rows (NaN hygiene)
                            {
                                pbuf[static_cast<crd::usize>(i) * obw + p] = T{0};
                            }
                        }
                        const crd::u32 num_panels =
                            (rows_blk + static_cast<crd::u32>(mr) - 1) / static_cast<crd::u32>(mr);
                        crd::u32 t = 0;
                        for (crd::u32 q = 0; q < obw; q += inner_bw, ++t)
                        {
                            const crd::u32 qw = (q + inner_bw < obw) ? inner_bw : (obw - q);
                            const crd::u32 kacc = q;
                            for (crd::u32 pi = 0; pi < num_panels; ++pi)
                            {
                                T* prow0 = pbuf + static_cast<crd::usize>(pi) * mr * obw;
                                const crd::u32 rows_in = (pi + 1 < num_panels || rows_blk % mr == 0)
                                                             ? static_cast<crd::u32>(mr)
                                                             : (rows_blk % static_cast<crd::u32>(mr));
                                if (kacc > 0) // UPDATE: panel[:, q-block] −= Σ panel[:, 0:kacc]·conj(W) (K=kacc)
                                {
                                    for (crd::u32 c0 = 0; c0 < qw; c0 += static_cast<crd::u32>(nrr))
                                    {
                                        const crd::u32 cols_in =
                                            (c0 + nrr <= qw) ? static_cast<crd::u32>(nrr) : (qw - c0);
                                        T micro[mr * nrr]{};
                                        dense::detail::gemm_microkernel<T>(
                                            kacc, obw, prow0,
                                            linv + wpack_off[t] + static_cast<crd::usize>(c0 / nrr) * kacc * nrr, micro,
                                            nrr);
                                        for (crd::u32 i = 0; i < rows_in; ++i)
                                        {
                                            T* prow = prow0 + static_cast<crd::usize>(i) * obw + q + c0;
                                            const T* mrow = micro + static_cast<crd::usize>(i) * nrr;
                                            for (crd::u32 j = 0; j < cols_in; ++j)
                                            {
                                                prow[j] -= mrow[j];
                                            }
                                        }
                                    }
                                }
                                // SOLVE: snapshot the panel's q-block, then panel[:, q-block] = snap·conj(linv_q).
                                for (crd::u32 i = 0; i < rows_in; ++i)
                                {
                                    const T* prow = prow0 + static_cast<crd::usize>(i) * obw + q;
                                    T* srow = snap + static_cast<crd::usize>(i) * inner_bw;
                                    for (crd::u32 p = 0; p < qw; ++p)
                                    {
                                        srow[p] = prow[p];
                                    }
                                }
                                for (crd::usize i = rows_in; i < mr; ++i) // zero-pad (the kernel reads all MR rows)
                                {
                                    for (crd::u32 p = 0; p < qw; ++p)
                                    {
                                        snap[i * inner_bw + p] = T{0};
                                    }
                                }
                                for (crd::u32 c0 = 0; c0 < qw; c0 += static_cast<crd::u32>(nrr))
                                {
                                    const crd::u32 cols_in = (c0 + nrr <= qw) ? static_cast<crd::u32>(nrr) : (qw - c0);
                                    T micro[mr * nrr]{};
                                    dense::detail::gemm_microkernel<T>(
                                        qw, inner_bw, snap,
                                        linv + lpack_off[t] + static_cast<crd::usize>(c0 / nrr) * qw * nrr, micro, nrr);
                                    for (crd::u32 i = 0; i < rows_in; ++i)
                                    {
                                        T* prow = prow0 + static_cast<crd::usize>(i) * obw + q + c0;
                                        const T* mrow = micro + static_cast<crd::usize>(i) * nrr;
                                        for (crd::u32 j = 0; j < cols_in; ++j)
                                        {
                                            prow[j] = mrow[j];
                                        }
                                    }
                                }
                            }
                        }
                        for (crd::u32 p = 0; p < obw; ++p) // write the solved block back (contiguous columns)
                        {
                            T* dst = x0 + static_cast<crd::usize>(p) * nr;
                            for (crd::u32 i = 0; i < rows_blk; ++i)
                            {
                                dst[i] = pbuf[static_cast<crd::usize>(i) * obw + p];
                            }
                        }
                    }
                };
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
                const auto sp_koA = ScaleClock::now();
                if (par_workers > 1)
                {
                    g_scaleprof.np_cdivA_ns += scale_ns(sp_ko0, sp_koA); // (A) inner factor = the serial chain
                }
#endif
                if (below_o > 0)
                {
                    if (par_workers <= 1)
                    {
                        b_slab(0, below_o, worker); // serial / tree-parallel: one slab = all below-outer rows
                    }
                    else
                    {
                        // Node-parallel huge front: ONE fork over the below_o rows (not per-jb-block).
                        auto* counter = crd::jobs::parallel_for(below_o, par_workers, [&](crd::u32 bb, crd::u32 ee)
                                                                { b_slab(bb, ee, crd::jobs::worker_index()); });
                        crd::jobs::wait(counter);
                        crd::jobs::frame_reset();
                    }
                }
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
                const auto sp_koB = ScaleClock::now();
                if (par_workers > 1)
                {
                    g_scaleprof.np_bsolveB_ns += scale_ns(sp_koA, sp_koB); // (B) below-outer trsm
                }
#endif
                // (C) OUTER trailing Schur update: cols [koend:nc], rows [koend:nr] (K=obw, high AI).
                if (koend < nc)
                {
                    const crd::u32 trail_rows = nr - koend;
                    const crd::u32 trail_cols = nc - koend;
                    const T* a_base = panel + static_cast<crd::usize>(ko) * nr + koend; // L[koend:nr, ko:koend]
                    T* c_base = panel + static_cast<crd::usize>(koend) * nr + koend;    // P[koend:nr, koend:nc]
#ifdef CRD_HESAP_CHOL_PROFILE
                    const auto prof_ot0 = ProfClock::now();
#endif
                    if constexpr (std::is_same_v<T, crd::f64>)
                    {
                        // f64 REROUTE (v5a-4 cdiv-B): the in-place ColMajor trailing gemm runs at Cerid's
                        // weak ColMajor-NT rate (~38 GF/s; OpenBLAS does it at 84). Compute the update Tᵀ =
                        // bv·avᵀ as a RowMajor-C gemm into ub — RowMajor Tᵀ(trail_cols×trail_rows,ld=trail_rows)
                        // IS T ColMajor(trail_rows×trail_cols,ld=trail_rows) in the SAME memory — onto Cerid's
                        // FAST RowMajor path (~60), then subtract T from the panel (ColMajor, contiguous per
                        // column). gemm bit-identical serial/parallel + element-independent subtract ⇒ moat holds.
                        const crd::u64 flop = static_cast<crd::u64>(2) * trail_rows * trail_cols * obw;
                        if (par_workers > 1)
                        {
                            // NODE-PARALLEL — v7-e-2 SYMMETRY split via the BALANCED-triangular primitive (fixes the
                            // imbalance that regressed the earlier row-slab attempt). DIAGONAL block [koend:nc]:
                            // parallel_for_triangular bands; band [r0,r1) computes cols [0,r1) only (col-restrict —
                            // skips the cross-band upper) + lower-subtract (panel row ≥ col), each lane its own ubuf
                            // scratch. BELOW block [nc:nr]: regular parallel_for, full cols. Bit-identical to serial
                            // on the used lower triangle (same K=obw reduction per entry) ⇒ the {1..16} moat holds.
                            const crd::u32 ndiag = trail_cols;
                            const crd::u32 nbelow = trail_rows - ndiag;
                            (void)flop;
                            // DIAGONAL block [koend:nc]: lower-triangle-only, balanced row-bands; band [r0,r1)
                            // computes cols [0,r1) (col-restrict) + lower-subtract; per-lane ubuf scratch.
                            auto diag_band = [&](crd::u32 r0, crd::u32 r1, crd::u32 lane)
                            {
                                T* ub_l = ubufs[lane].data();
                                const crd::u32 nrow = r1 - r0;
                                const crd::u32 ncol = r1; // cols [0,r1) — lower triangle needs col ≤ row < r1
                                const dense::MatrixView<const T, dense::Layout::RowMajor> av(a_base + r0, obw, nrow,
                                                                                             nr);
                                const dense::MatrixView<const T, dense::Layout::RowMajor> bv(a_base, obw, ncol, nr);
                                const dense::MatrixView<T, dense::Layout::RowMajor> tt(ub_l, ncol, nrow, nrow);
                                dense::gemm<T, dense::Layout::RowMajor>(T{1}, bv, av, T{0}, tt, dense::Trans::Transpose,
                                                                        dense::Trans::None, nullptr);
                                for (crd::u32 j = 0; j < ncol; ++j)
                                {
                                    T* cc = c_base + static_cast<crd::usize>(j) * nr + r0;
                                    const T* ttc = ub_l + static_cast<crd::usize>(j) * nrow;
                                    const crd::u32 i0 = (j > r0) ? (j - r0) : 0; // panel row r0+i ≥ col j ⟺ i ≥ j-r0
                                    for (crd::u32 i = i0; i < nrow; ++i)
                                    {
                                        cc[i] -= ttc[i];
                                    }
                                }
                            };
                            // SIZE-GATE the fork: the giant front's many LATE panels have a small ndiag; an 8-way fork
                            // over a tiny triangle is pure overhead that caps within-front scaling (~1.5×). Below
                            // threshold run ONE serial band (lane=worker ⇒ ub). Bit-identical: same per-entry K=obw
                            // reduction whether one band or many; the partition only chooses WHO computes a row.
                            const crd::u64 dflop = static_cast<crd::u64>(ndiag) * ndiag * obw; // ≈ 2·(ndiag²/2)·obw
                            if (dflop >= kGemmParallelMinFlop)
                            {
                                dense::detail::parallel_for_triangular(ndiag, par_workers, diag_band);
                            }
                            else
                            {
                                diag_band(0U, ndiag, worker);
                            }
                            if (nbelow > 0)
                            {
                                const dense::MatrixView<const T, dense::Layout::RowMajor> avb(a_base + ndiag, obw,
                                                                                              nbelow, nr);
                                const dense::MatrixView<const T, dense::Layout::RowMajor> bvb(a_base, obw, trail_cols,
                                                                                              nr);
                                const dense::MatrixView<T, dense::Layout::RowMajor> ttb(ub, trail_cols, nbelow, nbelow);
                                const crd::u64 bflop = static_cast<crd::u64>(2) * nbelow * trail_cols * obw;
                                const bool below_par = bflop >= kGemmParallelMinFlop;
                                if (below_par)
                                {
                                    dense::gemm_parallel<T, dense::Layout::RowMajor>(par_workers, T{1}, bvb, avb, T{0},
                                                                                     ttb, dense::Trans::Transpose,
                                                                                     dense::Trans::None, nullptr);
                                    crd::jobs::frame_reset();
                                }
                                else
                                {
                                    dense::gemm<T, dense::Layout::RowMajor>(T{1}, bvb, avb, T{0}, ttb,
                                                                            dense::Trans::Transpose, dense::Trans::None,
                                                                            nullptr);
                                }
                                auto sub_below = [&](crd::u32 j)
                                {
                                    T* cc = c_base + static_cast<crd::usize>(j) * nr + ndiag;
                                    const T* tt = ub + static_cast<crd::usize>(j) * nbelow;
                                    for (crd::u32 i = 0; i < nbelow; ++i)
                                    {
                                        cc[i] -= tt[i];
                                    }
                                };
                                if (below_par) // subtract scales with the gemm; serial below threshold avoids the fork
                                {
                                    auto* sc = crd::jobs::parallel_for(trail_cols, par_workers,
                                                                       [&](crd::u32 b, crd::u32 e)
                                                                       {
                                                                           for (crd::u32 j = b; j < e; ++j)
                                                                           {
                                                                               sub_below(j);
                                                                           }
                                                                       });
                                    crd::jobs::wait(sc);
                                    crd::jobs::frame_reset();
                                }
                                else
                                {
                                    for (crd::u32 j = 0; j < trail_cols; ++j)
                                    {
                                        sub_below(j);
                                    }
                                }
                            }
                        }
                        else
                        {
                            // SERIAL/tree-parallel — v7-e-2 SYMMETRY + the 2026-06-11 IN-PLACE rewrite: the
                            // trailing's DIAGONAL block [koend:nc]² is SYMMETRIC ⇒ computed LOWER-TRIANGLE-ONLY
                            // by the rebuilt packed syrk MERGING STRAIGHT INTO THE PANEL (col_indexed_out — no
                            // ub T-staging write+read+subtract passes, and the syrk's Goto blocking replaces the
                            // old 128-col panel-gemm walk that ran ~48 GF/s); the strictly-below block [nc:nr]
                            // runs as ONE in-place ColMajor gemm (the merge fix made the direct form fast — the
                            // old ub-reroute existed to dodge the strided ColMajor merge). BIT-IDENTICAL: each
                            // lower-triangle entry keeps the SAME single K=obw microkernel reduction (operand
                            // commutativity is exact in IEEE), merged as c −= Σ exactly as before.
                            const crd::u32 ndiag = trail_cols;          // diagonal rows == trailing cols
                            const crd::u32 nbelow = trail_rows - ndiag; // strictly-below-diagonal rows [nc:nr]
                            (void)flop;
#ifdef CRD_HESAP_CHOL_PROFILE
                            const auto pf_ct0 = ProfClock::now();
                            ++g_cholprof.ctrail_calls;
#endif
                            if (nbelow > 0)
                            {
                                const dense::MatrixView<const T, dense::Layout::ColMajor> avb(a_base + ndiag, nbelow,
                                                                                              obw, nr);
                                const dense::MatrixView<const T, dense::Layout::ColMajor> bvb(a_base, trail_cols, obw,
                                                                                              nr);
                                const dense::MatrixView<T, dense::Layout::ColMajor> cvb(c_base + ndiag, nbelow,
                                                                                        trail_cols, nr);
                                dense::gemm<T, dense::Layout::ColMajor>(T{-1}, avb, bvb, T{1}, cvb, dense::Trans::None,
                                                                        kCholAdjoint<T>, nullptr);
                            }
#ifdef CRD_HESAP_CHOL_PROFILE
                            const auto pf_ct1 = ProfClock::now();
                            g_cholprof.ctrail_below_ns += prof_ns(pf_ct0, pf_ct1);
#endif
                            const dense::MatrixView<const T, dense::Layout::ColMajor> avd(a_base, ndiag, obw, nr);
                            const dense::MatrixView<T, dense::Layout::ColMajor> cvd(c_base, ndiag, ndiag, nr);
                            dense::detail::syrk_lower_minus<T, dense::Layout::ColMajor>(
                                avd, cvd, nullptr, /*allow_parallel=*/false, /*col_indexed_out=*/true);
#ifdef CRD_HESAP_CHOL_PROFILE
                            g_cholprof.ctrail_syrk_ns += prof_ns(pf_ct1, ProfClock::now());
#endif
                        }
                    }
                    else
                    {
                        const dense::MatrixView<const T, dense::Layout::ColMajor> av(a_base, trail_rows, obw, nr);
                        const dense::MatrixView<const T, dense::Layout::ColMajor> bv(a_base, trail_cols, obw, nr);
                        const dense::MatrixView<T, dense::Layout::ColMajor> cv(c_base, trail_rows, trail_cols, nr);
                        chol_gemm(T{-1}, av, bv, T{1}, cv, dense::Trans::None, kCholAdjoint<T>, par_workers);
                    }
#ifdef CRD_HESAP_CHOL_PROFILE
                    g_cholprof.cdiv_outertrail_ns += prof_ns(prof_ot0, ProfClock::now());
#endif
                }
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
                if (par_workers > 1)
                {
                    g_scaleprof.np_ctrailC_ns += scale_ns(sp_koB, ScaleClock::now()); // (C) outer trailing Schur
                }
#endif
            }
        }
#ifdef CRD_HESAP_CHOL_PROFILE
        const auto prof_t3 = ProfClock::now();
        g_cholprof.cdiv_ns += prof_ns(prof_t2, prof_t3);
        g_cholprof.cdiv_ns_bin[prof_cdiv_bin] += prof_ns(prof_t2, prof_t3);
#endif
        for (crd::u32 a = 0; a < nr; ++a)
        {
            rr[sym.srow[rb + a]] = m_n; // restore sentinel for the next supernode on this worker
        }
    };

    // Supernode-etree levels: level[s] = 1 + max level over its update-list descendants
    // (each K < s, so processed in order). Same-level supernodes are mutually
    // independent ⇒ factor concurrently; levels run sequentially (ascending).
    crd::containers::Array<crd::u32> level(m_alloc);
    level.resize(ns);
    crd::u32 nlevels = 0;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        crd::u32 lv = 0;
        for (crd::u32 ui = upd_ptr[s]; ui < upd_ptr[s + 1]; ++ui)
        {
            const crd::u32 lk = level[upd_list[ui]] + 1;
            lv = lk > lv ? lk : lv;
        }
        level[s] = lv;
        nlevels = (lv + 1) > nlevels ? (lv + 1) : nlevels;
    }
    crd::containers::Array<crd::u32> lvl_ptr(m_alloc);
    crd::containers::Array<crd::u32> lvl_list(m_alloc);
    lvl_ptr.resize(nlevels + 1);
    for (crd::u32 l = 0; l <= nlevels; ++l)
    {
        lvl_ptr[l] = 0;
    }
    for (crd::u32 s = 0; s < ns; ++s)
    {
        ++lvl_ptr[level[s] + 1];
    }
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        lvl_ptr[l + 1] += lvl_ptr[l];
    }
    lvl_list.resize(ns);
    {
        crd::containers::Array<crd::u32> lf(m_alloc);
        lf.resize(nlevels);
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            lf[l] = lvl_ptr[l];
        }
        for (crd::u32 s = 0; s < ns; ++s)
        {
            lvl_list[lf[level[s]]++] = s; // ascending s within a level (determinism)
        }
    }
    // Store the etree levels for the level-parallel solve (copy out of the local scratch).
    m_nlevels = nlevels;
    m_lvl_ptr.resize(static_cast<crd::usize>(nlevels) + 1);
    for (crd::u32 l = 0; l <= nlevels; ++l)
    {
        m_lvl_ptr[l] = lvl_ptr[l];
    }
    m_lvl_list.resize(ns);
    for (crd::u32 s = 0; s < ns; ++s)
    {
        m_lvl_list[s] = lvl_list[s];
    }
    // Store the update-lists for the level-parallel LEFT-LOOKING forward solve (copy out of scratch).
    m_upd_ptr.resize(static_cast<crd::usize>(ns) + 1);
    for (crd::u32 s = 0; s <= ns; ++s)
    {
        m_upd_ptr[s] = upd_ptr[s];
    }
    m_upd_list.resize(upd_ptr[ns]);
    for (crd::u32 ui = 0; ui < upd_ptr[ns]; ++ui)
    {
        m_upd_list[ui] = upd_list[ui];
    }
    // Per-level flag: does the level hold a HUGE front (nc ≥ kNodeParallelMinCols)? Only such
    // levels go node-parallel; thin levels of only-small supernodes stay tree-parallel.
    crd::containers::Array<crd::u8> level_has_huge(m_alloc);
    level_has_huge.resize(nlevels == 0 ? 1 : nlevels);
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        level_has_huge[l] = 0;
    }
    for (crd::u32 s = 0; s < ns; ++s)
    {
        if (sym.scol[s + 1] - sym.scol[s] >= kNodeParallelMinCols)
        {
            level_has_huge[level[s]] = 1;
        }
    }

#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    g_scaleprof.setup_wall_ns = scale_ns(fac_t0, ScaleClock::now()); // symbolic + alloc + level build
#endif
#ifdef CRD_HESAP_CHOL_PROFILE
    g_cholprof.reset();
#endif
    if (num_workers <= 1)
    {
        for (crd::u32 s = 0; s < ns; ++s)
        {
            factor_one(s, 0, 1);
        }
    }
    else
    {
        // 2D-HYBRID schedule (the within-supernode-parallelism lever): a level is TREE-parallel
        // (one worker per supernode, serial gemm) UNLESS it is thin (cnt < num_workers) AND
        // holds a huge front — only then NODE-parallel (supernodes sequential on the dispatcher,
        // each driving gemm_parallel across the otherwise-idle pool for its big panel gemms).
        // This targets the few near-root huge fronts that serialize under pure tree-parallelism
        // WITHOUT serializing thin levels of small supernodes. Same factor either way
        // (gemm_parallel ≡ serial gemm — disjoint row-slabs ⇒ the determinism moat holds).
        const crd::u32* list = lvl_list.data();
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            const crd::u32 off = lvl_ptr[l];
            const crd::u32 cnt = lvl_ptr[l + 1] - off;
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
            const auto lvl_t0 = ScaleClock::now();
#endif
            if (cnt >= num_workers || level_has_huge[l] == 0)
            {
                auto* counter = crd::jobs::parallel_for(cnt, num_workers,
                                                        [&, off, list](crd::u32 b, crd::u32 e)
                                                        {
                                                            const crd::u32 w = crd::jobs::worker_index();
                                                            for (crd::u32 t = b; t < e; ++t)
                                                            {
                                                                factor_one(list[off + t], w, 1);
                                                            }
                                                        });
                crd::jobs::wait(counter);
            }
            else
            {
                for (crd::u32 t = 0; t < cnt; ++t)
                {
                    factor_one(list[off + t], 0, num_workers); // node-parallel big gemms
                }
            }
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
            {
                const double w = scale_ns(lvl_t0, ScaleClock::now());
                ++g_scaleprof.n_levels;
                if (cnt < num_workers && level_has_huge[l] == 1)
                {
                    g_scaleprof.node_wall_ns += w;
                    ++g_scaleprof.n_node_levels;
                }
                else if (cnt < num_workers)
                {
                    g_scaleprof.starved_wall_ns += w; // tree-parallel but workers idle (starvation)
                    ++g_scaleprof.n_starved_levels;
                }
                else
                {
                    g_scaleprof.tree_wall_ns += w; // healthy: cnt >= num_workers
                }
                if (w > g_scaleprof.max_level_wall_ns)
                {
                    g_scaleprof.max_level_wall_ns = w;
                    g_scaleprof.max_level_cnt = cnt;
                    crd::u32 mnc = 0;
                    for (crd::u32 t = 0; t < cnt; ++t)
                    {
                        const crd::u32 s = list[off + t];
                        const crd::u32 sc = sym.scol[s + 1] - sym.scol[s];
                        mnc = sc > mnc ? sc : mnc;
                    }
                    g_scaleprof.max_level_nc = mnc;
                }
            }
#endif
            // Reclaim this level's JobDecls from the per-thread frame arena; a deep etree
            // (ldoor ~1M) issues thousands of parallel_for batches and the 1 MB frame arena
            // exhausts without a reset at each level boundary. (chol_gemm also resets per
            // node-parallel gemm_parallel; this bounds the tree-parallel batches too.)
            crd::jobs::frame_reset();
        }
    }

    const crd::u32 fc = fail_col.load(std::memory_order_relaxed);
    m_info = (fc == 0xFFFFFFFFU) ? 0 : static_cast<crd::usize>(fc);
#ifdef CRD_HESAP_CHOL_SCALE_PROFILE
    if (num_workers > 1)
    {
        const double symw = g_scaleprof.sym_wall_ns * 1e-6;
        const double setupw = g_scaleprof.setup_wall_ns * 1e-6;
        const double allocw = setupw - symw; // scratch alloc + level building
        const double tw = g_scaleprof.tree_wall_ns * 1e-6;
        const double nw = g_scaleprof.node_wall_ns * 1e-6;
        const double stw = g_scaleprof.starved_wall_ns * 1e-6;
        const double numw = tw + nw + stw;
        const double tot = setupw + numw;
        std::printf("  [CHOLSCALE n=%u W=%u] SETUP=%.1fms(sym=%.1f alloc+lvl=%.1f) NUMERIC=%.1fms(tree=%.1f "
                    "node=%.1f STARVED=%.1f/%ulev) | max-lev=%.1fms cnt=%u nc=%u | SETUP%%=%.0f node%%=%.0f\n",
                    m_n, num_workers, setupw, symw, allocw, numw, tw, nw, stw, g_scaleprof.n_starved_levels,
                    g_scaleprof.max_level_wall_ns * 1e-6, g_scaleprof.max_level_cnt, g_scaleprof.max_level_nc,
                    tot > 0 ? 100.0 * setupw / tot : 0.0, tot > 0 ? 100.0 * nw / tot : 0.0);
        // Within-node-parallel-front phase split — answers (a) serial-chain (cdivA) vs (b) parallel-throughput
        // (bsolveB/ctrailC/cmod). cdivA is the per-panel POTF2+within-obw solve dependency chain that CANNOT
        // parallelize across the worker pool ⇒ if it dominates `node`, the front is Amdahl-serial-bound (the
        // honest ceiling); if bsolveB/ctrailC dominate, those are parallel and the lever is their scaling.
        const double cmodw = g_scaleprof.np_cmod_ns * 1e-6;
        const double cdivAw = g_scaleprof.np_cdivA_ns * 1e-6;
        const double bsolveBw = g_scaleprof.np_bsolveB_ns * 1e-6;
        const double ctrailCw = g_scaleprof.np_ctrailC_ns * 1e-6;
        std::printf("  [CHOLSCALE-NP n=%u W=%u] node-front phases: cmod=%.1f cdivA(serial-chain)=%.1f "
                    "bsolveB=%.1f ctrailC=%.1f ms | cdivA%%-of-node=%.0f\n",
                    m_n, num_workers, cmodw, cdivAw, bsolveBw, ctrailCw, nw > 0 ? 100.0 * cdivAw / nw : 0.0);
    }
#endif
#ifdef CRD_HESAP_CHOL_PROFILE
    {
        const double sc = g_cholprof.scatter_ns * 1e-6;
        const double cm = g_cholprof.cmod_ns * 1e-6;
        const double cd = g_cholprof.cdiv_ns * 1e-6;
        const double cm_gf =
            g_cholprof.cmod_ns > 0 ? static_cast<double>(g_cholprof.cmod_flops) / g_cholprof.cmod_ns : 0.0;
        const double cd_gf =
            g_cholprof.cdiv_ns > 0 ? static_cast<double>(g_cholprof.cdiv_flops) / g_cholprof.cdiv_ns : 0.0;
        const double flop_per_call = g_cholprof.cmod_calls > 0 ? static_cast<double>(g_cholprof.cmod_flops) /
                                                                     static_cast<double>(g_cholprof.cmod_calls)
                                                               : 0.0;
        std::printf("  [CHOLPROF n=%u ns=%u] scatter=%.1fms cmod=%.1fms(%.1f GF/s) cdiv=%.1fms(%.1f GF/s) "
                    "| cmod_flop=%.2fe9 cdiv_flop=%.2fe9 | cmod_calls=%llu flop/call=%.0f\n",
                    m_n, ns, sc, cm, cm_gf, cd, cd_gf, static_cast<double>(g_cholprof.cmod_flops) * 1e-9,
                    static_cast<double>(g_cholprof.cdiv_flops) * 1e-9,
                    static_cast<unsigned long long>(g_cholprof.cmod_calls), flop_per_call);
        const char* binlbl[CholProf::kBins] = {"<8", "8-15", "16-31", "32-63", "64-127", "128+"};
        std::printf("  [CHOLPROF-KBIN knc:");
        for (int bn = 0; bn < CholProf::kBins; ++bn)
        {
            const double pct = g_cholprof.cmod_flops > 0 ? 100.0 * static_cast<double>(g_cholprof.cmod_flop_bin[bn]) /
                                                               static_cast<double>(g_cholprof.cmod_flops)
                                                         : 0.0;
            const double gf = g_cholprof.cmod_ns_bin[bn] > 0
                                  ? static_cast<double>(g_cholprof.cmod_flop_bin[bn]) / g_cholprof.cmod_ns_bin[bn]
                                  : 0.0;
            std::printf(" %s=%.0f%%/%.0fGF/%.0fms", binlbl[bn], pct, gf, g_cholprof.cmod_ns_bin[bn] * 1e-6);
        }
        std::printf("]\n");
        // cmod internal split: where does the cmod time actually go — the gemm, the
        // rr[]-indexed scatter-subtract of U into the panel, or per-descendant overhead
        // (binary search + loop control)? Decides whether the next lever is the gemm
        // kernel (already at the square ceiling) or fusing the scatter / cutting overhead.
        double cmod_gemm_ms = 0.0;
        for (int bn = 0; bn < CholProf::kBins; ++bn)
        {
            cmod_gemm_ms += g_cholprof.cmod_ns_bin[bn] * 1e-6;
        }
        const double cmod_scat_ms = g_cholprof.cmod_scatter_ns * 1e-6;
        std::printf("  [CHOLPROF-CMOD cmod=%.1fms = gemm=%.1fms + scatter_sub=%.1fms + overhead=%.1fms]\n", cm,
                    cmod_gemm_ms, cmod_scat_ms, cm - cmod_gemm_ms - cmod_scat_ms);
        // The K≥128 calls (the 52-GF/s wall) by output width m1: small m1 ⇒ skinny-C ⇒
        // memory-bound ⇒ widen-C (amalgamation) is the lever, not the kernel.
        const char* m1lbl[CholProf::kM1Bins] = {"<16", "16-63", "64-255", "256-1023", "1024+"};
        crd::u64 hk_total = 0;
        for (int bn = 0; bn < CholProf::kM1Bins; ++bn)
        {
            hk_total += g_cholprof.cmod_hk_m1_flop[bn];
        }
        std::printf("  [CHOLPROF-M1 (K>=128) m1:");
        for (int bn = 0; bn < CholProf::kM1Bins; ++bn)
        {
            const double pct = hk_total > 0 ? 100.0 * static_cast<double>(g_cholprof.cmod_hk_m1_flop[bn]) /
                                                  static_cast<double>(hk_total)
                                            : 0.0;
            std::printf(" %s=%.0f%%(%lluc)", m1lbl[bn], pct,
                        static_cast<unsigned long long>(g_cholprof.cmod_hk_m1_count[bn]));
        }
        std::printf("]\n");
        // cdiv (25 GF/s — worst phase) by panel width nc: where is the cdiv time?
        const char* cdlbl[CholProf::kCdivBins] = {"<=4", "5-32", "33-127", "128-511", "512+"};
        std::printf("  [CHOLPROF-CDIV nc:");
        for (int bn = 0; bn < CholProf::kCdivBins; ++bn)
        {
            const double pct = g_cholprof.cdiv_flops > 0 ? 100.0 * static_cast<double>(g_cholprof.cdiv_flop_bin[bn]) /
                                                               static_cast<double>(g_cholprof.cdiv_flops)
                                                         : 0.0;
            const double gf = g_cholprof.cdiv_ns_bin[bn] > 0
                                  ? static_cast<double>(g_cholprof.cdiv_flop_bin[bn]) / g_cholprof.cdiv_ns_bin[bn]
                                  : 0.0;
            const double ms = g_cholprof.cdiv_ns_bin[bn] * 1e-6;
            std::printf(" %s=%.0f%%/%.0fGF/%.0fms", cdlbl[bn], pct, gf, ms);
        }
        std::printf("]\n");
        // Within-cdiv split: OUTER trailing (B, K=256 bulk gemm) vs the rest (A1 scalar POTF2 +
        // A2 below-solve + A3 inner trailing, all K=64). Decides which cdiv sub-lever to cut.
        const double ot_ms = g_cholprof.cdiv_outertrail_ns * 1e-6;
        std::printf("  [CHOLPROF-CDIV-SPLIT cdiv=%.1fms = outer_trail(B,K=256)=%.1fms + within_panel(A,K=64)=%.1fms]\n",
                    cd, ot_ms, cd - ot_ms);
        std::printf("  [CHOLPROF-CTRAIL serial calls=%llu below=%.1fms syrk=%.1fms | rest_of_outer_trail=%.1fms]\n",
                    static_cast<unsigned long long>(g_cholprof.ctrail_calls), g_cholprof.ctrail_below_ns * 1e-6,
                    g_cholprof.ctrail_syrk_ns * 1e-6,
                    ot_ms - g_cholprof.ctrail_below_ns * 1e-6 - g_cholprof.ctrail_syrk_ns * 1e-6);
    }
#endif
}

template <typename T> bool SupernodalCholesky<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    // Solve dispatch (2026-06-11, the full-scoreboard solve dig — every rule below is bench-measured on
    // the lattice corpus at {1,8,16} workers):
    //  · Single-RHS: ALWAYS serial. A triangular solve is memory-streaming-bound with a serial dependency
    //    chain — one core saturates the achievable bandwidth, and the level-parallel path's per-level
    //    fork/join made big solves SLOWER as workers grew (lat32: 69 ms serial ≈ 69 @8T → 98 @16T).
    //  · Multi-RHS: level-parallel only when the factor carries enough work per fork (lnz·nrhs against
    //    the measured break-even — lat20×16 lost parallel, lat24×16 won), and CAPPED at 8 lanes: the
    //    solve's gemms saturate the memory system there, and 16 lanes measured strictly worse than 8 on
    //    every size (lat24 x16: 46 ms @8 → 74 ms @16).
    // solve_with_workers still honors a forced nw, so the determinism moat test can exercise the parallel
    // path on any matrix regardless of this gate (parallel ≡ serial bit-identically by construction).
    constexpr crd::u64 solve_parallel_min_work = 160'000'000; // lnz·nrhs break-even (≈ lat20×16..lat24×16)
    constexpr crd::u32 solve_max_workers = 8;                 // memory-saturation cap (i9-14900K measured)
    crd::u32 nw = 1U;
    if (nrhs > 1 && static_cast<crd::u64>(m_lnz) * nrhs >= solve_parallel_min_work)
    {
        const crd::u32 pool = crd::jobs::num_workers();
        nw = pool < solve_max_workers ? pool : solve_max_workers;
    }
    return solve_with_workers(rhs, nrhs, nw);
}

template <typename T>
bool SupernodalCholesky<T>::solve_with_workers(crd::containers::Span<T> rhs, crd::usize nrhs, crd::u32 nw) const
{
    if (m_info != 0)
    {
        return false;
    }
    const SupernodalSymbolic& sym = m_sym;
    const crd::u32 ns = sym.nsuper;
    if (ns == 0)
    {
        return true;
    }
    crd::u32 max_below = 1;
    crd::u32 max_nc = 1;
    for (crd::u32 s = 0; s < ns; ++s)
    {
        const crd::u32 ncs = sym.scol[s + 1] - sym.scol[s];
        const crd::u32 bw = (sym.srowp[s + 1] - sym.srowp[s]) - ncs;
        max_below = bw > max_below ? bw : max_below;
        max_nc = ncs > max_nc ? ncs : max_nc;
    }
    // Multi-RHS diagonal-block solve goes BATCHED (c-contiguous scratch, L_diag read once) above this
    // width; below it the per-RHS path avoids the copy overhead (bmwcra's ~12k tiny nc=2-4 fronts).
    constexpr crd::u32 solve_batch_min_nc = 48;
    if (nrhs == 1 && nw <= 1)
    {
        // Single-RHS, SERIAL (nw<=1): ColMajor hand-axpy. At the latency-bound frontier; a block
        // gemm's small-K overhead would regress this (the failed-gemv lesson), so serial single-RHS
        // keeps the hand path. v5a-5: at nw>1 single-RHS instead takes the level-parallel hand path
        // in fwd_one/back_one below (same hand-axpy kernel + k-ascending reduction order => bit-
        // identical to this), so the determinism moat holds across worker counts.
        crd::containers::Array<T> tmp(m_alloc);
        tmp.resize(max_below);
        T* x = rhs.data();
        for (crd::u32 s = 0; s < ns; ++s) // forward L·y = b
        {
            const crd::u32 rb = sym.srowp[s];
            const crd::u32 nr = sym.srowp[s + 1] - rb;
            const crd::u32 firstcol = sym.scol[s];
            const crd::u32 nc = sym.scol[s + 1] - firstcol;
            const T* panel = &m_lx[m_lxp[s]];
            solve_fwd_diag<T>(x + firstcol, panel, nr, nc); // 4-col-blocked, bit-identical
            const crd::u32 below = nr - nc;
            if (below > 0)
            {
                for (crd::u32 r = 0; r < below; ++r)
                {
                    tmp[r] = T{0};
                }
                solve_fwd_below_acc<T>(tmp.data(), panel + nc, nr, nc, x + firstcol, below); // 4-stream fused
                for (crd::u32 r = 0; r < below; ++r)
                {
                    x[sym.srow[rb + nc + r]] -= tmp[r];
                }
            }
        }
        for (crd::u32 si = ns; si-- > 0;) // backward Lᴴ·x = y (Lᵀ for real)
        {
            const crd::u32 rb = sym.srowp[si];
            const crd::u32 nr = sym.srowp[si + 1] - rb;
            const crd::u32 firstcol = sym.scol[si];
            const crd::u32 nc = sym.scol[si + 1] - firstcol;
            const T* panel = &m_lx[m_lxp[si]];
            const crd::u32 below = nr - nc;
            if (below > 0)
            {
                for (crd::u32 r = 0; r < below; ++r)
                {
                    tmp[r] = x[sym.srow[rb + nc + r]];
                }
                solve_back_below<T>(x + firstcol, panel + nc, nr, nc, tmp.data(), below); // 4-stream fused dots
            }
            solve_back_diag<T>(x + firstcol, panel, nr, nc); // 4-col-blocked descending
        }
        return true;
    }

    if (nrhs > 1)
    {
    }
    // Multi-RHS: column-major n × nrhs, ld = m_n. `nw` (caller-forced, = num_workers() in the public solve)
    // selects the path: nw>1 ⇒ level-PARALLEL race-free LEFT-looking (each supernode writes only its own
    // columns — the forward gathers from descendants ascending, the backward from ancestors descending);
    // nw≤1 ⇒ SERIAL RIGHT-looking forward (few big gemms — ~15-20% faster than the left-looking gather's
    // many tiny gemms at 1T, bench-confirmed). Both forwards share the k-ascending accumulation order ⇒
    // bit-identical x. Concurrent same-level supernodes need DISJOINT scratch ⇒ per-worker tmp/dscr slices,
    // SIZED BY THE POOL (not nw): parallel_for(cnt, nw) dispatches onto the shared pool, so worker_index()
    // is POOL-GLOBAL (0..num_workers()-1) regardless of nw — sizing by nw heap-overflows for 1<nw<pool.
    const crd::u32 sw = nw > 1 ? crd::jobs::num_workers() : 1;
    crd::containers::Array<T> tmp(m_alloc); // [sw] × (below × nrhs) work block (ColMajor, ld = below)
    tmp.resize(static_cast<crd::usize>(sw) * max_below * nrhs);
    crd::containers::Array<T> dscr(m_alloc); // [sw] × (nc × nrhs) RowMajor diag scratch — batched diag solve
    dscr.resize(static_cast<crd::usize>(sw) * max_nc * nrhs);
    T* xb = rhs.data();
    const crd::usize ldx = m_n;
    // Forward L·Y = B — LEFT-LOOKING (race-free, level-parallel like the backward). Each supernode s
    // GATHERS Σ_{k ∈ upd_list[s]} L_{s,k}·Y_k from its already-solved descendants and writes ONLY its own
    // columns (vs the right-looking scatter into shared ancestor rows, which races same-level supernodes).
    // L_{s,k} is exactly descendant k's panel rows that fall in s's column range [firstcol, firstcol+nc)
    // — a contiguous sub-block found by binary search (the cmod gather mirror) — and k-ascending upd_list
    // order == the right-looking accumulation order ⇒ bit-identical to the serial right-looking forward
    // (moat + residual hold). No subdiagonal scatter: each ancestor gathers s's contribution in its turn.
    auto fwd_one = [&](crd::u32 s, crd::u32 worker)
    {
        T* wdscr = dscr.data() + static_cast<crd::usize>(worker) * max_nc * nrhs; // gather C + diag scratch
        const crd::u32 rb = sym.srowp[s];
        const crd::u32 nr = sym.srowp[s + 1] - rb;
        const crd::u32 firstcol = sym.scol[s];
        const crd::u32 nc = sym.scol[s + 1] - firstcol;
        const T* panel = &m_lx[m_lxp[s]];
        // v5a-5: single-RHS takes the hand-axpy LEFT-looking gather + right-looking diagonal solve below
        // (no N=1 gemm — the failed-gemv lesson). BIT-IDENTICAL to the dedicated nw<=1 forward: each
        // descendant's contribution is grouped and summed over that descendant's columns ascending, and
        // descendants are visited in k-ascending upd_list order == the dedicated right-looking order.
        if (nrhs == 1)
        {
            for (crd::u32 ui = m_upd_ptr[s]; ui < m_upd_ptr[s + 1]; ++ui)
            {
                const crd::u32 k = m_upd_list[ui];
                const crd::u32 krb = sym.srowp[k];
                const crd::u32 knr = sym.srowp[k + 1] - krb;
                const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
                const crd::u32 kfirstcol = sym.scol[k];
                const T* kpanel = &m_lx[m_lxp[k]];
                crd::u32 lo = knc;
                crd::u32 hi = knr;
                while (lo < hi)
                {
                    const crd::u32 mid = lo + (hi - lo) / 2;
                    if (sym.srow[krb + mid] < firstcol)
                    {
                        lo = mid + 1;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                const crd::u32 p0 = lo;
                crd::u32 m1 = 0;
                while (p0 + m1 < knr && sym.srow[krb + p0 + m1] < firstcol + nc)
                {
                    ++m1;
                }
                if (m1 == 0)
                {
                    continue;
                }
                for (crd::u32 i = 0; i < m1; ++i)
                {
                    wdscr[i] = T{0};
                }
                // The SAME fused kernel as the serial path (ascending descendant-columns ⇒ bit-identical).
                solve_fwd_below_acc<T>(wdscr, kpanel + p0, knr, knc, xb + kfirstcol, m1);
                for (crd::u32 i = 0; i < m1; ++i)
                {
                    xb[sym.srow[krb + p0 + i]] -= wdscr[i];
                }
            }
            solve_fwd_diag<T>(xb + firstcol, panel, nr, nc); // the shared 4-col-blocked diagonal solve
            return;
        }
        for (crd::u32 ui = m_upd_ptr[s]; ui < m_upd_ptr[s + 1]; ++ui) // gather from descendants (k-ascending)
        {
            const crd::u32 k = m_upd_list[ui];
            const crd::u32 krb = sym.srowp[k];
            const crd::u32 knr = sym.srowp[k + 1] - krb;
            const crd::u32 knc = sym.scol[k + 1] - sym.scol[k];
            const crd::u32 kfirstcol = sym.scol[k];
            const T* kpanel = &m_lx[m_lxp[k]];
            crd::u32 lo = knc; // p0 = first k-row ≥ firstcol (k's dense diagonal block is rows [0,knc))
            crd::u32 hi = knr;
            while (lo < hi)
            {
                const crd::u32 mid = lo + (hi - lo) / 2;
                if (sym.srow[krb + mid] < firstcol)
                {
                    lo = mid + 1;
                }
                else
                {
                    hi = mid;
                }
            }
            const crd::u32 p0 = lo;
            crd::u32 m1 = 0; // # of k-rows in [firstcol, firstcol+nc) = the rows L_{s,k} touches
            while (p0 + m1 < knr && sym.srow[krb + p0 + m1] < firstcol + nc)
            {
                ++m1;
            }
            if (m1 == 0)
            {
                continue;
            }
            // C(m1×nrhs, ColMajor ld=m1) = kpanel[p0.., :knc] (m1×knc) · Y_k(knc×nrhs). Forward uses L
            // (NOT Lᴴ — the conj is the backward's); here None/None. wdscr holds C (consumed before diag).
            solve_mrhs_fwd_below<T>(wdscr, kpanel + p0, knr, knc, xb + kfirstcol, ldx, m1, nrhs);
            for (crd::usize c = 0; c < nrhs; ++c) // B_s -= C at global rows srow[k][p0+i]
            {
                T* xc = xb + c * ldx;
                const T* cc = wdscr + c * static_cast<crd::usize>(m1);
                for (crd::u32 i = 0; i < m1; ++i)
                {
                    xc[sym.srow[krb + p0 + i]] -= cc[i];
                }
            }
        }
        if (nc >= solve_batch_min_nc)
        {
            // Batched diagonal forward solve (wide fronts): c-contiguous scratch, all nrhs per L-element
            // (reads L_diag ONCE vs nrhs×; inner c-loop vectorizes), DIVIDE (not ×reciprocal) ⇒ same
            // per-(i,c) op sequence as the per-RHS path ⇒ bit-identical (moat/residual unchanged).
            for (crd::u32 j = 0; j < nc; ++j)
            {
                T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                const T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    dj[c] = xj[c * ldx];
                }
            }
            solve_mrhs_fwd_diag<T>(wdscr, nrhs, panel, nr, nc); // 4-col-blocked, bit-identical order
            for (crd::u32 j = 0; j < nc; ++j)
            {
                const T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    xj[c * ldx] = dj[c];
                }
            }
        }
        else
        {
            for (crd::usize c = 0; c < nrhs; ++c) // narrow fronts: per-RHS (avoids the copy overhead)
            {
                T* xc = xb + c * ldx;
                for (crd::u32 j = 0; j < nc; ++j) // contiguous column axpy on the ColMajor panel
                {
                    const T yj = xc[firstcol + j] / panel[static_cast<crd::usize>(j) * nr + j];
                    xc[firstcol + j] = yj;
                    const T* colj = panel + static_cast<crd::usize>(j) * nr;
                    for (crd::u32 i = j + 1; i < nc; ++i)
                    {
                        xc[firstcol + i] -= colj[i] * yj; // L[i,j] unit-stride in i — vectorizes
                    }
                }
            }
        }
    };
    if (nw > 1) // level-parallel forward: levels ASCENDING (leaves first), same-level concurrent
    {
        for (crd::u32 li = 0; li < m_nlevels; ++li)
        {
            const crd::u32 off = m_lvl_ptr[li];
            const crd::u32 cnt = m_lvl_ptr[li + 1] - off;
            if (cnt >= 2)
            {
                auto* counter = crd::jobs::parallel_for(cnt, nw,
                                                        [&, off](crd::u32 b, crd::u32 e)
                                                        {
                                                            const crd::u32 w = crd::jobs::worker_index();
                                                            for (crd::u32 t = b; t < e; ++t)
                                                            {
                                                                fwd_one(m_lvl_list[off + t], w);
                                                            }
                                                        });
                crd::jobs::wait(counter);
                crd::jobs::frame_reset();
            }
            else // thin level (near-root big front): serial, no spawn
            {
                for (crd::u32 t = 0; t < cnt; ++t)
                {
                    fwd_one(m_lvl_list[off + t], 0);
                }
            }
        }
    }
    else
    {
        // SERIAL forward: RIGHT-looking (few big subdiagonal gemms — ~15-20% faster than the left-looking
        // gather's many tiny per-descendant gemms at 1T). Each supernode solves its diagonal then ONE gemm
        // scatters L_below·Y_s into ancestor rows. Bit-identical to the parallel left-looking path: both
        // accumulate descendant contributions into B_s in k-ascending order before the diagonal solve, and
        // the per-row gemm values are layout-independent (the solve determinism moat). Scratch slice 0.
        for (crd::u32 s = 0; s < ns; ++s)
        {
            const crd::u32 rb = sym.srowp[s];
            const crd::u32 nr = sym.srowp[s + 1] - rb;
            const crd::u32 firstcol = sym.scol[s];
            const crd::u32 nc = sym.scol[s + 1] - firstcol;
            const T* panel = &m_lx[m_lxp[s]];
            if (nc >= solve_batch_min_nc)
            {
                for (crd::u32 j = 0; j < nc; ++j) // batched diag (c-contiguous scratch, DIVIDE)
                {
                    T* dj = dscr.data() + static_cast<crd::usize>(j) * nrhs;
                    const T* xj = xb + firstcol + j;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        dj[c] = xj[c * ldx];
                    }
                }
                solve_mrhs_fwd_diag<T>(dscr.data(), nrhs, panel, nr, nc); // 4-col-blocked
                for (crd::u32 j = 0; j < nc; ++j)
                {
                    const T* dj = dscr.data() + static_cast<crd::usize>(j) * nrhs;
                    T* xj = xb + firstcol + j;
                    for (crd::usize c = 0; c < nrhs; ++c)
                    {
                        xj[c * ldx] = dj[c];
                    }
                }
            }
            else
            {
                for (crd::usize c = 0; c < nrhs; ++c) // narrow fronts: per-RHS
                {
                    T* xc = xb + c * ldx;
                    for (crd::u32 j = 0; j < nc; ++j)
                    {
                        const T yj = xc[firstcol + j] / panel[static_cast<crd::usize>(j) * nr + j];
                        xc[firstcol + j] = yj;
                        const T* colj = panel + static_cast<crd::usize>(j) * nr;
                        for (crd::u32 i = j + 1; i < nc; ++i)
                        {
                            xc[firstcol + i] -= colj[i] * yj;
                        }
                    }
                }
            }
            const crd::u32 below = nr - nc;
            if (below > 0)
            {
                solve_mrhs_fwd_below<T>(tmp.data(), panel + nc, nr, nc, xb + firstcol, ldx, below, nrhs);
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    T* xc = xb + c * ldx;
                    const T* tc = tmp.data() + c * static_cast<crd::usize>(below);
                    for (crd::u32 r = 0; r < below; ++r)
                    {
                        xc[sym.srow[rb + nc + r]] -= tc[r];
                    }
                }
            }
        }
    }
    // Backward Lᴴ·X = Y (Lᵀ for real). LEFT-LOOKING: each supernode reads completed ANCESTORS (its own
    // panel below-rows × already-solved x) and writes ONLY its own columns ⇒ level-parallel is race-free
    // + bit-identical across worker counts (per-supernode op order fixed; same-level supernodes
    // independent). Per-worker tmp/dscr slices avoid scratch races.
    auto back_one = [&](crd::u32 si, crd::u32 worker)
    {
        T* wtmp = tmp.data() + static_cast<crd::usize>(worker) * max_below * nrhs;
        T* wdscr = dscr.data() + static_cast<crd::usize>(worker) * max_nc * nrhs;
        const crd::u32 rb = sym.srowp[si];
        const crd::u32 nr = sym.srowp[si + 1] - rb;
        const crd::u32 firstcol = sym.scol[si];
        const crd::u32 nc = sym.scol[si + 1] - firstcol;
        const T* panel = &m_lx[m_lxp[si]];
        const crd::u32 below = nr - nc;
        // v5a-5: single-RHS hand path (no N=1 gemm). BIT-IDENTICAL to the dedicated nw<=1 backward:
        // the below-block gather sums over below-rows ascending, then the descending-jj diagonal back-solve.
        if (nrhs == 1)
        {
            if (below > 0)
            {
                for (crd::u32 r = 0; r < below; ++r) // gather ONCE (was re-gathered per column k)
                {
                    wtmp[r] = xb[sym.srow[rb + nc + r]];
                }
                solve_back_below<T>(xb + firstcol, panel + nc, nr, nc, wtmp, below); // shared fused kernel
            }
            solve_back_diag<T>(xb + firstcol, panel, nr, nc); // shared 4-col-blocked descending solve
            return;
        }
        if (below > 0)
        {
            for (crd::u32 r = 0; r < below; ++r) // ROW-major gather (a row's RHS values contiguous)
            {
                T* wrow = wtmp + static_cast<crd::usize>(r) * nrhs;
                const crd::u32 g = sym.srow[rb + nc + r];
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    wrow[c] = xb[g + c * ldx];
                }
            }
            solve_mrhs_back_below<T>(xb + firstcol, ldx, panel + nc, nr, nc, wtmp, below, nrhs, wdscr);
        }
        if (nc >= solve_batch_min_nc)
        {
            // Batched diagonal back-solve (Lᴴ upper): c-contiguous scratch, all nrhs per L-element,
            // DIVIDE for bit-identity. Same descending-jj / ascending-k op order as the per-RHS path.
            for (crd::u32 j = 0; j < nc; ++j)
            {
                T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                const T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    dj[c] = xj[c * ldx];
                }
            }
            solve_mrhs_back_diag<T>(wdscr, nrhs, panel, nr, nc); // 4-col-blocked descending
            for (crd::u32 j = 0; j < nc; ++j)
            {
                const T* dj = wdscr + static_cast<crd::usize>(j) * nrhs;
                T* xj = xb + firstcol + j;
                for (crd::usize c = 0; c < nrhs; ++c)
                {
                    xj[c * ldx] = dj[c];
                }
            }
        }
        else
        {
            for (crd::usize c = 0; c < nrhs; ++c) // narrow fronts: per-RHS
            {
                T* xc = xb + c * ldx;
                for (crd::u32 jj = nc; jj-- > 0;)
                {
                    const T* coljj = panel + static_cast<crd::usize>(jj) * nr;
                    T v = xc[firstcol + jj];
                    for (crd::u32 k = jj + 1; k < nc; ++k)
                    {
                        v -= chol_conj<T>(coljj[k]) * xc[firstcol + k]; // Lᴴ entry = conj(L)
                    }
                    xc[firstcol + jj] = v / coljj[jj]; // coljj[jj] = L[jj][jj] is real
                }
            }
        }
    };
    if (nw > 1) // level-parallel backward: levels DESCENDING (root level first), same-level concurrent
    {
        for (crd::u32 li = m_nlevels; li-- > 0;)
        {
            const crd::u32 off = m_lvl_ptr[li];
            const crd::u32 cnt = m_lvl_ptr[li + 1] - off;
            if (cnt >= 2)
            {
                auto* counter = crd::jobs::parallel_for(cnt, nw,
                                                        [&, off](crd::u32 b, crd::u32 e)
                                                        {
                                                            const crd::u32 w = crd::jobs::worker_index();
                                                            for (crd::u32 t = b; t < e; ++t)
                                                            {
                                                                back_one(m_lvl_list[off + t], w);
                                                            }
                                                        });
                crd::jobs::wait(counter);
                crd::jobs::frame_reset();
            }
            else // thin level (near-root big front): serial, no spawn
            {
                for (crd::u32 t = 0; t < cnt; ++t)
                {
                    back_one(m_lvl_list[off + t], 0);
                }
            }
        }
    }
    else
    {
        for (crd::u32 si = ns; si-- > 0;) // serial: reverse supernode order (valid topological order)
        {
            back_one(si, 0);
        }
    }
    return true;
}

template <typename T>
SupernodalCholesky<T> factor_supernodal_cholesky(const sparse::SparsePattern& pattern,
                                                 crd::containers::ConstSpan<T> values, crd::memory::IAllocator* alloc,
                                                 crd::u32 nrelax, crd::u32 num_workers)
{
    SupernodalCholesky<T> f(alloc);
    f.factorize(pattern, values, nrelax, num_workers);
    return f;
}

template class SupernodalCholesky<crd::f32>;
template class SupernodalCholesky<crd::f64>;
template class SupernodalCholesky<Complex32>; // complex Hermitian LLᴴ (v5a-2)
template class SupernodalCholesky<Complex64>;
template SupernodalCholesky<crd::f32> factor_supernodal_cholesky<crd::f32>(const sparse::SparsePattern&,
                                                                           crd::containers::ConstSpan<crd::f32>,
                                                                           crd::memory::IAllocator*, crd::u32,
                                                                           crd::u32);
template SupernodalCholesky<crd::f64> factor_supernodal_cholesky<crd::f64>(const sparse::SparsePattern&,
                                                                           crd::containers::ConstSpan<crd::f64>,
                                                                           crd::memory::IAllocator*, crd::u32,
                                                                           crd::u32);
template SupernodalCholesky<Complex32> factor_supernodal_cholesky<Complex32>(const sparse::SparsePattern&,
                                                                             crd::containers::ConstSpan<Complex32>,
                                                                             crd::memory::IAllocator*, crd::u32,
                                                                             crd::u32);
template SupernodalCholesky<Complex64> factor_supernodal_cholesky<Complex64>(const sparse::SparsePattern&,
                                                                             crd::containers::ConstSpan<Complex64>,
                                                                             crd::memory::IAllocator*, crd::u32,
                                                                             crd::u32);

} // namespace crd::hesap::direct
