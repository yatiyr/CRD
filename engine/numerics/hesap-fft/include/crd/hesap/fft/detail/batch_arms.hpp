// The batched transforms' crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE): a batch of independent
// transforms of one size split into contiguous ranges, one range per job, each job on its own plan and scratch (a plan
// writes its scratch through const: one plan per thread). The consumers: hesap-dsp's segment periodograms (Welch, the
// spectrogram) and stft, hesap-wavelet's cwt and NdFftPlan's lines of an axis. The thread-sweep board
// (bench_hesap_fft_parallel) sets the defaults, and a lever that loses goes with its field.
//
// Not a lever: FftPlan::execute_batched (element-major, SIMD over the batch axis). Its engine caller is the four-step,
// inside one transform, at a batch of exactly the sub-plan's block width (the hierarchical route, whose stage-1 strides
// are the whole batch); no engine batch of independent transforms reaches it.
//
// Determinism (ADR-0063): every lever is scheduling. Each transform runs on an identical plan (a pure function of its
// size) and writes only its own output, so every pick gives the serial bits.

#pragma once

#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>

namespace crd::hesap::fft::detail
{

struct BatchPick
{
    crd::usize min_transforms = 4; // scheduling: a batch of fewer transforms runs on the calling thread
    crd::usize min_work = 0; // scheduling: so does one whose batch_work is below this (0: the transform count alone)
    crd::u32 workers = 0;    // scheduling: 0 is crd::jobs::num_workers(), otherwise the forced jobs
};

// A cut no batch reaches.
inline constexpr crd::usize kBatchNever = ~crd::usize{0};

// A batch's work: transforms x n x ceil(log2 n), at least one a transform element.
[[nodiscard]] constexpr crd::usize batch_work(crd::usize count, crd::usize n) noexcept
{
    crd::usize lg = 1;
    while (lg < 64U && (crd::usize{1} << lg) < n)
    {
        ++lg;
    }
    return count * n * lg;
}

// The jobs a batch of `count` size-`n` transforms runs on: 1 (the calling thread, no parallel_for) on a pool that is
// down, below either cut or for one job; otherwise the forced or the pool's jobs, at most one a transform.
[[nodiscard]] inline crd::u32 batch_jobs(const BatchPick& pick, crd::usize count, crd::usize n) noexcept
{
    const crd::u32 pool = crd::jobs::num_workers();
    if (pool <= 1U || count < pick.min_transforms || batch_work(count, n) < pick.min_work)
    {
        return 1U;
    }
    const crd::u32 jobs = pick.workers == 0U ? pool : pick.workers;
    return count < jobs ? static_cast<crd::u32>(count) : jobs;
}

// Runs fn(job, first, last) over the transforms [0, count) in `jobs` contiguous ranges, job j taking
// [j count / jobs, (j + 1) count / jobs) on its plan and scratch j; one job runs on the calling thread.
template <typename F> void run_batch(crd::u32 jobs, crd::usize count, crd::jobs::StackSize stack, const F& fn)
{
    if (jobs <= 1U)
    {
        fn(0U, crd::usize{0}, count);
        return;
    }
    crd::jobs::Counter* c = crd::jobs::parallel_for(
        jobs, jobs,
        [&fn, count, jobs](crd::u32 jb, crd::u32 je)
        {
            for (crd::u32 job = jb; job < je; ++job)
            {
                fn(job, static_cast<crd::usize>(job) * count / jobs, static_cast<crd::usize>(job + 1U) * count / jobs);
            }
        },
        stack);
    crd::jobs::wait(c);
}

} // namespace crd::hesap::fft::detail
