#pragma once

// crd-hesap-special v12-a — parallel batch evaluation: out[i] = f(in[i]) over crd-jobs disjoint index ranges.
//
// The determinism moat, FOR FREE: each output index is written by exactly one worker and out[i]=f(in[i]) is
// independent of the partition ⇒ the result is BIT-IDENTICAL across {1,4,16} thread counts by construction (no
// cross-thread reduction). This is what MATLAB's auto-multithreaded elementwise array ops give on perf but NOT on
// reproducibility. Below kBatchParallelThreshold (or with jobs uninitialised) the serial path runs — same bytes.
//
// SIMD: the f64 lgamma and gamma batches run the per-CPU-level Lanczos kernels (special_kernels.hpp); the others
// are scalar loops over the public functions, and a vector body is added per function only where a measurement shows
// the compiler left throughput on the table (the bench is the gate, not assumption).

#include <crd/hesap/special/airy.hpp>
#include <crd/hesap/special/bessel.hpp>
#include <crd/hesap/special/erf.hpp>
#include <crd/hesap/special/gamma.hpp>
#include <crd/hesap/special/incomplete.hpp>
#include <crd/hesap/special/special_dispatch.hpp>

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
// memfloor 0.31, lgamma 0.97), which is the app's jobs::init decision, not the batch's. So: njobs = nw.

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
// The Lanczos lgamma and tgamma batches for f64 run the hesap-special kernel level the process selected
// (crd/hesap/special/special_dispatch.hpp), one call per job range; every level gives detail::lanczos_lgamma's and
// lanczos_tgamma's bits, so the result is partition- and level-independent (the {1,4,16} moat). With the fast Lanczos
// compute the batch is bandwidth-bound, so the gain comes from a P-core-sized pool (ADR-0094 policy at init), not from
// fewer jobs (which would mis-place on E-cores).
inline void lanczos_batch_f64(double* out, const double* in, crd::usize n,
                              void (*range)(double*, const double*, crd::usize) noexcept) noexcept
{
    const crd::u32 nw = crd::jobs::num_workers();
    if (n < kBatchParallelThreshold || nw <= 1U)
    {
        range(out, in, n);
        return;
    }
    const auto rf = [out, in, range](crd::u32 b, crd::u32 e) { range(out + b, in + b, e - b); };
    crd::jobs::Counter* const c = crd::jobs::is_pcore_routing()
                                      ? crd::jobs::parallel_for_pcores(static_cast<crd::u32>(n), rf)
                                      : crd::jobs::parallel_for(static_cast<crd::u32>(n), nw, rf);
    crd::jobs::wait(c);
}

// lgamma over a batch: the Lanczos kernels for f64 (within 1e-13 of the public lgamma, a few ulp), the scalar
// generic otherwise.
template <Real T>
void lgamma_batch(T* out, const T* in, crd::usize n) noexcept
{
    if constexpr (std::is_same_v<T, double>)
    {
        lanczos_batch_f64(out, in, n, special_kernels().table->lgamma_lanczos);
    }
    else
    {
        batch_apply<T>(out, in, n, [](T x) { return lgamma(x); });
    }
}

// tgamma over a batch: the Lanczos kernels for f64 (exp of the Lanczos lgamma; within 1e-12 of the public gamma), the
// scalar generic otherwise.
template <Real T>
void gamma_batch(T* out, const T* in, crd::usize n) noexcept
{
    if constexpr (std::is_same_v<T, double>)
    {
        lanczos_batch_f64(out, in, n, special_kernels().table->tgamma_lanczos);
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
