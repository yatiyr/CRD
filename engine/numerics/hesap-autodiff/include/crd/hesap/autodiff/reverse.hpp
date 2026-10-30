#pragma once

// reverse.hpp — Phase 3.1.6 v16-a: the reverse-mode DRIVERS + the deterministic data-parallel batched gradient.
// `gradient` returns the WHOLE ∇f in ONE backward pass (the reverse advantage — O(1) passes vs forward's n vs FD's
// n+1). `batch_gradient` is the MOAT: it computes Σ_s ∇loss_s across a batch data-parallel (each worker its own tape,
// no shared adjoints), then folds the per-sample gradients in a FIXED sample order — so the result is BIT-IDENTICAL
// across `{1..16}` workers and batch layouts (PyTorch/JAX can't: their scatter-add is atomic ⇒ order-dependent).
// ADR-0097.

#include <crd/hesap/autodiff/rules_reverse.hpp> // the full VJP surface (+ tape.hpp)

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>

namespace crd::hesap::autodiff::reverse
{

using crd::containers::ConstSpan;
using crd::containers::Span;

// ∇f of a SCALAR functor f: Rⁿ→R at x, into g[0..n), in ONE reverse pass. f: `Var f(const Var* x, int n)`.
// `scratch` (length ≥ n) is the caller-owned leaf array; the tape is reset + reused.
template <class F>
inline void gradient(const F& f, ConstSpan<crd::f64> x, Span<crd::f64> g, Tape& tape, Span<Var> scratch) noexcept
{
    tape.reset();
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        scratch[i] = make_leaf(tape, x[i]);
    }
    const Var y = f(scratch.data(), n);
    tape.seed(y.node, 1.0);
    tape.backward();
    for (int i = 0; i < n; ++i)
    {
        g[i] = tape.grad(scratch[i].node);
    }
}

// Jacobian of f: Rⁿ→Rᵐ into row-major jac[m*n]: build the graph ONCE, then ONE backward per output row (m passes —
// reverse wins when m ≪ n). f: `void f(const Var* x, int n, Var* y, int m)`. The m passes stay serial on the one
// tape: a pass per job would need a tape per job.
template <class F>
inline void jacobian(const F& f, ConstSpan<crd::f64> x, int m, Span<crd::f64> jac, Tape& tape, Span<Var> xscratch,
                     Span<Var> yscratch) noexcept
{
    tape.reset();
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i)
    {
        xscratch[i] = make_leaf(tape, x[i]);
    }
    f(xscratch.data(), n, yscratch.data(), m);
    for (int j = 0; j < m; ++j)
    {
        tape.zero_adjoints();
        tape.seed(yscratch[j].node, 1.0);
        tape.backward();
        for (int i = 0; i < n; ++i)
        {
            jac[j * n + i] = tape.grad(xscratch[i].node);
        }
    }
}

namespace detail
{
inline constexpr int kBatchMaxParams = 32; // a job's leaves live on its stack
inline constexpr const char* kBatchTooManyParams = "batch_gradient: theta has more than 32 parameters";
inline constexpr const char* kBatchShortSpan = "batch_gradient: grad, gbuf or tapes is shorter than the batch needs";

// batch_gradient's crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE). batch_gradient is the default pick,
// so production and the arms share one body; the thread-sweep board (bench_hesap_autodiff_parallel) sets the
// defaults, and a lever that loses goes with its field. The job count stays the caller's: the tapes are one per job.
// Determinism (ADR-0097): both levers are scheduling. Each sample's gradient is taken on a reset tape, and each
// component is summed over the samples in ascending order from 0.0 on every path, so every pick gives the same bits.
struct BatchGradientPick
{
    crd::u32 jobs_min_samples = 1; // scheduling: a batch with fewer samples runs on the calling thread on tapes[0]
    crd::u32 fold_jobs = 1;        // scheduling: the fold splits the components over this many jobs; 1 folds serially
};

template <class LossFn> struct BatchContext
{
    const LossFn* loss;
    const crd::f64* theta;
    Tape* const* tapes;
    crd::f64* grad;
    crd::f64* gbuf;
    int n;
    crd::u32 samples;
    crd::u32 jobs;
};

// Job j's samples [j S / jobs, (j + 1) S / jobs) on tapes[j], each into its row of gbuf.
template <class LossFn> inline void batch_samples(const BatchContext<LossFn>& c, crd::u32 j) noexcept
{
    Tape& tp = *c.tapes[j];
    const crd::u64 s0 = crd::u64{j} * c.samples / c.jobs;
    const crd::u64 s1 = (crd::u64{j} + 1U) * c.samples / c.jobs;
    for (crd::u64 s = s0; s < s1; ++s)
    {
        tp.reset();
        Var th[kBatchMaxParams];
        for (int i = 0; i < c.n; ++i)
        {
            th[i] = make_leaf(tp, c.theta[i]);
        }
        const Var y = (*c.loss)(th, c.n, static_cast<int>(s));
        tp.seed(y.node, 1.0);
        tp.backward();
        for (int i = 0; i < c.n; ++i)
        {
            c.gbuf[s * static_cast<crd::u64>(c.n) + static_cast<crd::u64>(i)] = tp.grad(th[i].node);
        }
    }
}

// grad[i0, i1) = the per-sample gradients' components summed in ascending sample order.
template <class LossFn> inline void batch_fold(const BatchContext<LossFn>& c, crd::u32 i0, crd::u32 i1) noexcept
{
    for (crd::u32 i = i0; i < i1; ++i)
    {
        c.grad[i] = 0.0;
    }
    for (crd::u64 s = 0; s < c.samples; ++s)
    {
        const crd::f64* row = c.gbuf + s * static_cast<crd::u64>(c.n);
        for (crd::u32 i = i0; i < i1; ++i)
        {
            c.grad[i] += row[i];
        }
    }
}

template <class LossFn>
inline void batch_gradient_arm(const BatchGradientPick& pick, const LossFn& loss, ConstSpan<crd::f64> theta, int S,
                               Span<crd::f64> grad, Span<Tape*> tapes, Span<crd::f64> gbuf, crd::u32 num_jobs) noexcept
{
    const int n = static_cast<int>(theta.size());
    if (n > kBatchMaxParams)
    {
        CRD_FATAL(kBatchTooManyParams);
    }
    const crd::u32 samples = S > 0 ? static_cast<crd::u32>(S) : 0U;
    const bool pool = crd::jobs::num_workers() != 0;
    crd::u32 jobs = num_jobs < samples ? num_jobs : samples;
    if (!pool || samples < pick.jobs_min_samples || jobs == 0)
    {
        jobs = 1;
    }
    if (grad.size() < static_cast<crd::usize>(n) ||
        gbuf.size() < static_cast<crd::usize>(samples) * static_cast<crd::usize>(n) || tapes.size() < jobs)
    {
        CRD_FATAL(kBatchShortSpan);
    }
    const BatchContext<LossFn> ctx{&loss, theta.data(), tapes.data(), grad.data(), gbuf.data(), n, samples, jobs};
    if (jobs == 1)
    {
        batch_samples(ctx, 0);
    }
    else
    {
        crd::jobs::Counter* done = crd::jobs::parallel_for(jobs, jobs,
                                                           [pc = &ctx](crd::u32 jb, crd::u32 je) noexcept
                                                           {
                                                               for (crd::u32 j = jb; j < je; ++j)
                                                               {
                                                                   batch_samples(*pc, j);
                                                               }
                                                           });
        crd::jobs::wait(done);
    }

    const crd::u32 components = static_cast<crd::u32>(n);
    const crd::u32 fold_jobs = pick.fold_jobs < components ? pick.fold_jobs : components;
    if (!pool || fold_jobs <= 1)
    {
        batch_fold(ctx, 0, components);
        return;
    }
    crd::jobs::Counter* done = crd::jobs::parallel_for(
        components, fold_jobs, [pc = &ctx](crd::u32 ib, crd::u32 ie) noexcept { batch_fold(*pc, ib, ie); });
    crd::jobs::wait(done);
}
} // namespace detail

// ★ The deterministic data-parallel BATCHED gradient — the moat gate.
// grad[0..n) = Σ_{s=0}^{S-1} ∇_θ loss(θ, s), the per-sample gradients computed DATA-PARALLEL (each job owns a sample
// range + its OWN tape — no shared adjoints, no atomics) then folded in FIXED ascending sample order ⇒ bit-identical
// for any `num_jobs`. `loss`: `Var loss(const Var* theta, int n, int s)`. `tapes` (one per job; min(num_jobs, S) jobs
// run, one on the calling thread when the pool is down) + `gbuf` (length S*n, per-sample gradients) are caller-owned.
// n ≤ 32; more parameters, or a span shorter than that, is fatal in every build.
template <class LossFn>
inline void batch_gradient(const LossFn& loss, ConstSpan<crd::f64> theta, int S, Span<crd::f64> grad,
                           Span<Tape*> tapes, Span<crd::f64> gbuf, crd::u32 num_jobs) noexcept
{
    detail::batch_gradient_arm(detail::BatchGradientPick{}, loss, theta, S, grad, tapes, gbuf, num_jobs);
}

} // namespace crd::hesap::autodiff::reverse
