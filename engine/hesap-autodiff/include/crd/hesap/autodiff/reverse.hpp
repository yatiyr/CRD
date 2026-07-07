#pragma once

// reverse.hpp — Phase 3.1.6 v16-a: the reverse-mode DRIVERS + the deterministic data-parallel batched gradient.
// `gradient` returns the WHOLE ∇f in ONE backward pass (the reverse advantage — O(1) passes vs forward's n vs FD's
// n+1). `batch_gradient` is the MOAT: it computes Σ_s ∇loss_s across a batch data-parallel (each worker its own tape,
// no shared adjoints), then folds the per-sample gradients in a FIXED sample order — so the result is BIT-IDENTICAL
// across `{1..16}` workers and batch layouts (PyTorch/JAX can't: their scatter-add is atomic ⇒ order-dependent).
// ADR-0097.

#include <crd/hesap/autodiff/rules_reverse.hpp> // the full VJP surface (+ tape.hpp)

#include <crd/containers/span.hpp>
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
    for (int i = 0; i < n; ++i) { scratch[i] = make_leaf(tape, x[i]); }
    const Var y = f(scratch.data(), n);
    tape.seed(y.node, 1.0);
    tape.backward();
    for (int i = 0; i < n; ++i) { g[i] = tape.grad(scratch[i].node); }
}

// Jacobian of f: Rⁿ→Rᵐ into row-major jac[m*n]: build the graph ONCE, then ONE backward per output row (m passes —
// reverse wins when m ≪ n). f: `void f(const Var* x, int n, Var* y, int m)`.
template <class F>
inline void jacobian(const F& f, ConstSpan<crd::f64> x, int m, Span<crd::f64> jac, Tape& tape, Span<Var> xscratch,
                     Span<Var> yscratch) noexcept
{
    tape.reset();
    const int n = static_cast<int>(x.size());
    for (int i = 0; i < n; ++i) { xscratch[i] = make_leaf(tape, x[i]); }
    f(xscratch.data(), n, yscratch.data(), m);
    for (int j = 0; j < m; ++j)
    {
        tape.zero_adjoints();
        tape.seed(yscratch[j].node, 1.0);
        tape.backward();
        for (int i = 0; i < n; ++i) { jac[j * n + i] = tape.grad(xscratch[i].node); }
    }
}

// ★ The deterministic data-parallel BATCHED gradient — the moat gate.
// grad[0..n) = Σ_{s=0}^{S-1} ∇_θ loss(θ, s), the per-sample gradients computed DATA-PARALLEL (each job owns a sample
// range + its OWN tape — no shared adjoints, no atomics) then folded in FIXED ascending sample order ⇒ bit-identical
// for any `num_jobs`. `loss`: `Var loss(const Var* theta, int n, int s)`. `tapes` (≥ num_jobs, one per job) +
// `gbuf` (length S*n, per-sample gradients) are caller-owned. Requires the jobs pool to be init'd; n ≤ 32.
template <class LossFn>
inline void batch_gradient(const LossFn& loss, ConstSpan<crd::f64> theta, int S, Span<crd::f64> grad,
                           Span<Tape*> tapes, Span<crd::f64> gbuf, crd::u32 num_jobs) noexcept
{
    const int n = static_cast<int>(theta.size());
    struct Ctx
    {
        const LossFn*   loss;
        const crd::f64* theta;
        Tape**          tapes;
        crd::f64*       gbuf;
        int             n;
        int             S;
        crd::u32        njobs;
    };
    Ctx ctx{&loss, theta.data(), tapes.data(), gbuf.data(), n, S, num_jobs};

    crd::jobs::Counter* c = crd::jobs::parallel_for(
        num_jobs, num_jobs, [pc = &ctx](crd::u32 jb, crd::u32 je) noexcept {
            for (crd::u32 j = jb; j < je; ++j)
            {
                Tape&          tp = *pc->tapes[j];
                const crd::u32 s0 = j * static_cast<crd::u32>(pc->S) / pc->njobs;
                const crd::u32 s1 = (j + 1) * static_cast<crd::u32>(pc->S) / pc->njobs;
                for (crd::u32 s = s0; s < s1; ++s)
                {
                    tp.reset();
                    Var th[32];
                    for (int i = 0; i < pc->n; ++i) { th[i] = make_leaf(tp, pc->theta[i]); }
                    const Var y = (*pc->loss)(th, pc->n, static_cast<int>(s));
                    tp.seed(y.node, 1.0);
                    tp.backward();
                    for (int i = 0; i < pc->n; ++i)
                    {
                        pc->gbuf[static_cast<crd::usize>(s) * pc->n + i] = tp.grad(th[i].node);
                    }
                }
            }
        });
    crd::jobs::wait(c);

    // FIXED-ORDER fold (ascending sample, ascending component) — the deterministic, worker-count-independent sum.
    for (int i = 0; i < n; ++i) { grad[i] = 0.0; }
    for (int s = 0; s < S; ++s)
    {
        for (int i = 0; i < n; ++i) { grad[i] += gbuf[static_cast<crd::usize>(s) * n + i]; }
    }
}

} // namespace crd::hesap::autodiff::reverse
