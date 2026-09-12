#pragma once

// v12-o — Parallel resampling over crd-jobs. Each resample r draws from Threefry stream r, so its value depends only on
// (seed, r) and NOT on which worker computes it: the parallel bootstrap distribution is BIT-IDENTICAL to the serial one
// (the determinism moat holds under threading — what scipy/R/MATLAB cannot guarantee). Separate header so the core
// resampling.hpp stays jobs-free; consumers that want parallelism opt in here.

#include <crd/hesap/stats/resampling.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

namespace crd::hesap::stats
{

// Parallel bootstrap distribution: B resampled statistics computed across crd-jobs workers. Bit-identical to
// bootstrap_distribution(data, stat, n_resamples, seed, alloc). num_jobs == 0 → use the whole pool; small batches fall
// back to serial. Requires crd::jobs::init() to have been called.
template <Real T, typename Stat>
[[nodiscard]] crd::containers::Array<T> bootstrap_distribution_parallel(crd::containers::ConstSpan<T> data, Stat stat,
                                                                       crd::usize n_resamples, crd::u64 seed,
                                                                       crd::memory::IAllocator* alloc,
                                                                       crd::u32 num_jobs = 0)
{
    const crd::usize n = data.size();
    crd::containers::Array<T> out(alloc);
    out.resize(n_resamples);
    if (num_jobs == 0)
    {
        num_jobs = crd::jobs::num_workers();
    }
    if (num_jobs <= 1 || n_resamples < 256)
    {
        return bootstrap_distribution(data, stat, n_resamples, seed, alloc); // serial fallback
    }

    crd::memory::ThreadSafeAllocator ts(alloc);
    struct Ctx
    {
        const T* data;
        T* out;
        crd::usize n;
        crd::u64 seed;
        crd::memory::ThreadSafeAllocator* ts;
        Stat stat;
    };
    Ctx ctx{data.data(), out.data(), n, seed, &ts, stat};
    Ctx* const cp = &ctx;

    auto* const counter =
        crd::jobs::parallel_for(static_cast<crd::u32>(n_resamples), num_jobs, [cp](crd::u32 begin, crd::u32 end) {
            crd::containers::Array<T> resample(cp->ts); // one scratch per task (thread-safe alloc, contended once)
            resample.resize(cp->n);
            for (crd::u32 r = begin; r < end; ++r)
            {
                PhiloxRng rng(cp->seed, static_cast<crd::u64>(r));
                for (crd::usize i = 0; i < cp->n; ++i)
                {
                    resample[i] = cp->data[detail::bounded_u32(rng, static_cast<crd::u32>(cp->n))];
                }
                cp->out[r] = cp->stat(crd::containers::ConstSpan<T>{resample.data(), cp->n});
            }
        });
    crd::jobs::wait(counter);
    return out;
}

// Parallel bootstrap confidence interval (percentile / basic / BCa) — same result as the serial bootstrap_ci.
template <Real T, typename Stat>
[[nodiscard]] CiResult<T> bootstrap_ci_parallel(crd::containers::ConstSpan<T> data, Stat stat, crd::usize n_resamples,
                                                BootMethod method, T alpha, crd::u64 seed,
                                                crd::memory::IAllocator* alloc, crd::u32 num_jobs = 0)
{
    const T theta_hat = stat(data);
    const auto dist = bootstrap_distribution_parallel(data, stat, n_resamples, seed, alloc, num_jobs);
    const auto ds = crd::containers::ConstSpan<T>{dist.data(), dist.size()};
    if (method == BootMethod::Percentile)
    {
        return bootstrap_ci_percentile(ds, alpha, alloc);
    }
    if (method == BootMethod::Basic)
    {
        return bootstrap_ci_basic(theta_hat, ds, alpha, alloc);
    }
    const auto jv = jackknife_values(data, stat, alloc);
    return bootstrap_ci_bca(theta_hat, ds, crd::containers::ConstSpan<T>{jv.data(), jv.size()}, alpha, alloc);
}

} // namespace crd::hesap::stats
