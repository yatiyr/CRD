#pragma once

// crd-hesap-special v12-a — parallel batch evaluation: out[i] = f(in[i]) over crd-jobs disjoint index ranges.
//
// The determinism moat, FOR FREE: each output index is written by exactly one worker and out[i]=f(in[i]) is
// independent of the partition ⇒ the result is BIT-IDENTICAL across {1,4,16} thread counts by construction (no
// cross-thread reduction). This is what MATLAB's auto-multithreaded elementwise array ops give on perf but NOT on
// reproducibility. Below kBatchParallelThreshold (or with jobs uninitialised) the serial path runs — same bytes.
//
// SIMD: the scalar kernels (Cody calerf erf/erfc, Stirling lgamma, digamma) auto-vectorise well under -O3; an
// explicit Vec4d path is added per-function only where a measurement shows the compiler left throughput on the
// table (the bench is the gate, not assumption).

#include <crd/hesap/special/airy.hpp>
#include <crd/hesap/special/bessel.hpp>
#include <crd/hesap/special/detail/simd_lanczos.hpp>
#include <crd/hesap/special/erf.hpp>
#include <crd/hesap/special/gamma.hpp>
#include <crd/hesap/special/incomplete.hpp>

#include <crd/jobs/jobs.hpp>

#include <type_traits>

namespace crd::hesap::special
{

// Below this element count the parallel dispatch overhead outweighs the speedup → run serial.
inline constexpr crd::usize kBatchParallelThreshold = 16384;

// Job count = the worker count. NOTE (measured 2026-06-22): a fixed cap (e.g. 8) was tried — it helps the pure
// memory-bound floor (memfloor 1.46→0.41) but STARVES the compute-bound batches (gammainc_p 5.4→12.4) because the
// surplus workers spin idle instead of computing. No single cap is optimal for both regimes on a 32-thread pool
// (8 P + 16 E + HT); the right lever is the GLOBAL pool size (a P-core-sized pool wins both — sweep: 16 workers →
// memfloor 0.31, lgamma 0.97), which is the app's jobs::init decision, not the batch's. So: njobs = nw. The
// lgamma/tgamma gap to MATLAB-MT (MKL-VML) on the as-given 32-thread pool needs a SIMD lgamma (Vec4d log) to go
// memory-bound; tracked, not faked.

// Core: apply f to every element, parallel over the job pool when worth it (disjoint index ranges ⇒ bit-identical
// across thread counts by construction = the determinism moat). F must be trivially copyable (stateless lambda, or
// one capturing only scalars) and fit the parallel_for SBO.
//
// NOTE (perf, measured 2026-06-22): a non-temporal (`_mm256_stream_pd`) store path was tried to drop the
// read-for-ownership traffic and beat MATLAB/MKL-VML's sub-floor lgamma — it REGRESSED on this box (memfloor
// 1.29→1.41 ns/elem; the set_pd+sfence overhead exceeded the RFO saving) and was rejected. The remaining
// lgamma/tgamma gap vs MATLAB-MT is the MKL-VML memory floor; closing it is a deeper threading/prefetch dig.
template <Real T, class F>
void batch_apply(T* out, const T* in, crd::usize n, F f,
                 crd::jobs::WorkerPreference pref = crd::jobs::WorkerPreference::Default) noexcept
{
    const crd::u32 nw = crd::jobs::num_workers();
    if (n < kBatchParallelThreshold || nw <= 1U)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i] = f(in[i]);
        }
        return;
    }
    const auto range_fn = [out, in, f](crd::u32 b, crd::u32 e)
    {
        for (crd::u32 i = b; i < e; ++i)
        {
            out[i] = f(in[i]);
        }
    };
    // Result is partition-independent (out[i]=f(in[i])) ⇒ ANY job count / placement is bit-identical ⇒ moat preserved.
    // On a pcore_routing pool, a bandwidth-bound batch routes to the P-core-pinned workers (ADR-0094); otherwise it
    // is one job per worker exactly as before (default path unchanged).
    crd::jobs::Counter* c = nullptr;
    if (pref == crd::jobs::WorkerPreference::MemoryBoundElementwise && crd::jobs::is_pcore_routing())
    {
        c = crd::jobs::parallel_for_pcores(static_cast<crd::u32>(n), range_fn);
    }
    else
    {
        c = crd::jobs::parallel_for(static_cast<crd::u32>(n), nw, range_fn);
    }
    crd::jobs::wait(c);
}

// ---- unary batches (the distribution-layer hot kernels) ----
// The erf family + digamma are light-compute / bandwidth-bound ⇒ MemoryBoundElementwise (ADR-0094).
inline constexpr auto kMemBound = crd::jobs::WorkerPreference::MemoryBoundElementwise;

template <Real T>
void erf_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T x) { return erf(x); }, kMemBound);
}
template <Real T>
void erfc_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T x) { return erfc(x); }, kMemBound);
}
template <Real T>
void erfcx_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T x) { return erfcx(x); }, kMemBound);
}
// erfinv / erfcinv are COMPUTE-heavy (Giles rational + Halley + an erf evaluation), NOT bandwidth-bound — they
// scale with cores, so they stay Default (all workers). Measured: routed to the P-core subset they LOSE MATLAB-MT
// (0.84×); on the full pool they WIN (1.35×). (ADR-0094 — classify by the compute/bandwidth ratio, not by family.)
template <Real T>
void erfinv_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T y) { return erfinv(y); });
}
template <Real T>
void erfcinv_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T y) { return erfcinv(y); });
}
// SIMD Lanczos lgamma over [b,e): full quads via crd_lgamma_lz4, scalar crd_lgamma_lz1 tail (bit-identical twins ⇒
// partition-independent ⇒ the {1,4,16} moat holds). The Lanczos sum is recurrence-free so it vectorizes (unlike
// the Stirling form whose per-lane shift count defeated SIMD at 0.65×).
inline void lgamma_lz_range_f64(double* out, const double* in, crd::u32 b, crd::u32 e) noexcept
{
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    crd::u32 i = b;
    for (; i + 4U <= e; i += 4U)
    {
        _mm256_storeu_pd(out + i, detail::crd_lgamma_lz4(_mm256_loadu_pd(in + i)));
    }
    for (; i < e; ++i)
    {
        out[i] = detail::crd_lgamma_lz1(in[i]);
    }
#else
    for (crd::u32 i = b; i < e; ++i)
    {
        out[i] = detail::crd_lgamma_lz1(in[i]);
    }
#endif
}

// lgamma over a batch — SIMD Lanczos + parallel for f64; scalar generic otherwise. Default worker preference: with
// fast Lanczos compute this is bandwidth-bound, so the WIN comes from a P-core-sized pool (ADR-0094 policy at init),
// not from reducing njobs (which would mis-place on E-cores). Deterministic ⇒ the moat holds. This is the crd_log
// Lanczos lgamma; it agrees with the public sf::lgamma to <1e-13 (≤ a few ulp).
template <Real T>
void lgamma_batch(T* out, const T* in, crd::usize n) noexcept
{
    if constexpr (std::is_same_v<T, double>)
    {
        const crd::u32 nw = crd::jobs::num_workers();
        if (n < kBatchParallelThreshold || nw <= 1U)
        {
            lgamma_lz_range_f64(out, in, 0U, static_cast<crd::u32>(n));
            return;
        }
        const auto rf = [out, in](crd::u32 b, crd::u32 e) { lgamma_lz_range_f64(out, in, b, e); };
        crd::jobs::Counter* const c = crd::jobs::is_pcore_routing()
                                          ? crd::jobs::parallel_for_pcores(static_cast<crd::u32>(n), rf)
                                          : crd::jobs::parallel_for(static_cast<crd::u32>(n), nw, rf);
        crd::jobs::wait(c);
    }
    else
    {
        batch_apply<T>(out, in, n, [](T x) { return lgamma(x); });
    }
}
// SIMD Lanczos tgamma over [b,e): full quads via crd_tgamma_lz4, scalar crd_tgamma_lz1 tail (bit-identical twins).
inline void tgamma_lz_range_f64(double* out, const double* in, crd::u32 b, crd::u32 e) noexcept
{
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
    crd::u32 i = b;
    for (; i + 4U <= e; i += 4U)
    {
        _mm256_storeu_pd(out + i, detail::crd_tgamma_lz4(_mm256_loadu_pd(in + i)));
    }
    for (; i < e; ++i)
    {
        out[i] = detail::crd_tgamma_lz1(in[i]);
    }
#else
    for (crd::u32 i = b; i < e; ++i)
    {
        out[i] = detail::crd_tgamma_lz1(in[i]);
    }
#endif
}

// tgamma over a batch — SIMD Lanczos-exp + parallel for f64 (= exp(lgamma); the SIMD log + SIMD exp primitives both
// apply). Scalar generic otherwise. Deterministic ⇒ moat holds; agrees with public sf::gamma to <1e-12.
template <Real T>
void gamma_batch(T* out, const T* in, crd::usize n) noexcept
{
    if constexpr (std::is_same_v<T, double>)
    {
        const crd::u32 nw = crd::jobs::num_workers();
        if (n < kBatchParallelThreshold || nw <= 1U)
        {
            tgamma_lz_range_f64(out, in, 0U, static_cast<crd::u32>(n));
            return;
        }
        const auto rf = [out, in](crd::u32 b, crd::u32 e) { tgamma_lz_range_f64(out, in, b, e); };
        crd::jobs::Counter* const c = crd::jobs::is_pcore_routing()
                                          ? crd::jobs::parallel_for_pcores(static_cast<crd::u32>(n), rf)
                                          : crd::jobs::parallel_for(static_cast<crd::u32>(n), nw, rf);
        crd::jobs::wait(c);
    }
    else
    {
        batch_apply<T>(out, in, n, [](T x) { return gamma(x); });
    }
}
template <Real T>
void digamma_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T x) { return digamma(x); }, kMemBound);
}

// ---- parametrised batches (cdf use: fixed shape, varying argument) ----
template <Real T>
void gammainc_p_batch(T* out, const T* in, crd::usize n, T a) noexcept
{
    batch_apply<T>(out, in, n, [a](T x) { return gammainc_p(a, x); });
}
template <Real T>
void gammainc_q_batch(T* out, const T* in, crd::usize n, T a) noexcept
{
    batch_apply<T>(out, in, n, [a](T x) { return gammainc_q(a, x); });
}
template <Real T>
void betainc_batch(T* out, const T* in, crd::usize n, T a, T b) noexcept
{
    batch_apply<T>(out, in, n, [a, b](T x) { return betainc(a, b, x); });
}

// ---- Bessel / Airy batches (fixed order ν, array of arguments — MATLAB besselj(nu,X) shape). COMPUTE-heavy ⇒
// Default preference (all workers); the {1,4,16} moat holds by construction (out[i]=f(in[i]), disjoint ranges). ----
template <Real T>
void cyl_bessel_j_batch(T* out, const T* in, crd::usize n, T nu) noexcept
{
    batch_apply<T>(out, in, n, [nu](T x) { return cyl_bessel_j(nu, x); });
}
template <Real T>
void cyl_neumann_batch(T* out, const T* in, crd::usize n, T nu) noexcept
{
    batch_apply<T>(out, in, n, [nu](T x) { return cyl_neumann(nu, x); });
}
template <Real T>
void cyl_bessel_i_batch(T* out, const T* in, crd::usize n, T nu) noexcept
{
    batch_apply<T>(out, in, n, [nu](T x) { return cyl_bessel_i(nu, x); });
}
template <Real T>
void cyl_bessel_k_batch(T* out, const T* in, crd::usize n, T nu) noexcept
{
    batch_apply<T>(out, in, n, [nu](T x) { return cyl_bessel_k(nu, x); });
}
template <Real T>
void airy_ai_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T x) { return airy_ai(x); });
}
template <Real T>
void airy_bi_batch(T* out, const T* in, crd::usize n) noexcept
{
    batch_apply<T>(out, in, n, [](T x) { return airy_bi(x); });
}

} // namespace crd::hesap::special
