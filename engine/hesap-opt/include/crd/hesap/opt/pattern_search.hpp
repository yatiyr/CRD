#pragma once

// pattern_search.hpp — Phase 3.1.6 v7-p-1: PATTERN SEARCH (GPS / OrthoMADS-style) — mesh-based direct search
// with the convergence machinery direct-search theory rests on (Torczon 1997; Audet-Dennis 2006):
//   • POLL: evaluate x + Δ·d over a positive-spanning direction set; move to the best strict improvement.
//   • MESH: success keeps Δ (optionally grows it); failure halves Δ; terminate when Δ ≤ delta_tol — the
//     mesh-size certificate (at a mesh-refining limit point the Clarke derivative is nonnegative along the
//     poll directions; the nonsmooth-correctness story gradient methods can't offer).
//   • DIRECTIONS: Gps = the fixed coordinate set ±e_i; OrthoMads = a fresh ORTHONORMAL basis each iteration
//     (Householder H = I − 2vvᵀ from a PHILOX-seeded unit vector, polled ±h_i) — the MADS idea of asymptotically
//     dense poll directions, with (seed, iteration)-keyed determinism BY CONSTRUCTION (the v7-i Philox stream
//     discipline: bit-identical runs, any replay).
// Value-only — no gradients anywhere. Honest scope (named): no SEARCH step (poll-only GPS/MADS is the
// convergent core; surrogate search steps are an acceleration, not a correctness piece), simple ×2/÷2 mesh
// update. [gold: NLopt's direct-search family — the v7-z scoreboard]. ADR-0090.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/hesap/stats/philox.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::opt
{

enum class PatternPoll : crd::u8
{
    Gps,       // fixed coordinate poll ±e_i
    OrthoMads, // per-iteration Philox-Householder orthonormal poll ±h_i
};

template <typename T> struct PatternSearchOptions
{
    T delta0 = static_cast<T>(1);       // initial mesh size
    T delta_tol = static_cast<T>(1e-9); // termination mesh size
    T expand = static_cast<T>(2);       // mesh growth on success
    T contract = static_cast<T>(0.5);   // mesh shrink on failure
    PatternPoll poll = PatternPoll::Gps;
    crd::u64 seed = 0x9E3779B97F4A7C15ULL; // OrthoMads direction stream
};

template <typename T>
[[nodiscard]] OptResult<T> minimize_pattern_search(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                   const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                   const PatternSearchOptions<T>& ps = {})
{
    const crd::usize n = obj.n();
    CRD_ASSERT_MSG(x0.size() == n, "minimize_pattern_search: x0 size mismatch");

    OptResult<T> result(alloc);
    result.x.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = x0[i];
    }
    if (n == 0)
    {
        result.status = OptStatus::Success;
        result.converged = true;
        return result;
    }

    T* x = result.x.data();
    auto feval = [&](const T* p) -> T
    {
        ++result.fn_evals;
        return obj.value({p, n});
    };
    T fx = feval(x);

    crd::containers::Array<T> hmat(alloc); // OrthoMads basis (n × n, rows are directions)
    crd::containers::Array<T> xtrial(alloc);
    crd::containers::Array<T> xbest(alloc);
    hmat.resize(n * n);
    xtrial.resize(n);
    xbest.resize(n);

    T delta = ps.delta0;
    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        if (delta <= ps.delta_tol)
        {
            status = OptStatus::Success; // the mesh-size certificate
            break;
        }

        if (ps.poll == PatternPoll::OrthoMads)
        {
            // Householder H = I − 2vvᵀ from a (seed, iteration)-keyed Philox unit vector — a fresh orthonormal
            // positive-spanning set each iteration, bit-reproducible by construction.
            crd::hesap::stats::PhiloxRng rng(ps.seed, /*stream=*/static_cast<crd::u64>(it));
            crd::containers::Array<T> v(alloc);
            v.resize(n);
            T norm_sq = static_cast<T>(0);
            for (crd::usize j = 0; j < n; ++j)
            {
                v[j] = static_cast<T>(rng.next_f64() * 2.0 - 1.0);
                norm_sq += v[j] * v[j];
            }
            if (!(norm_sq > static_cast<T>(1e-30))) // astronomically unlikely all-zero draw
            {
                v[0] = static_cast<T>(1);
                norm_sq = static_cast<T>(1);
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                for (crd::usize j = 0; j < n; ++j)
                {
                    const T id = i == j ? static_cast<T>(1) : static_cast<T>(0);
                    hmat[i * n + j] = id - static_cast<T>(2) * v[i] * v[j] / norm_sq;
                }
            }
        }

        // Full poll over ±d_i (deterministic order); take the BEST strict improvement.
        bool improved = false;
        T fbest = fx;
        for (crd::usize i = 0; i < 2 * n; ++i)
        {
            const crd::usize axis = i / 2;
            const T sign = (i % 2 == 0) ? static_cast<T>(1) : static_cast<T>(-1);
            for (crd::usize j = 0; j < n; ++j)
            {
                const T dj = ps.poll == PatternPoll::Gps ? (j == axis ? static_cast<T>(1) : static_cast<T>(0))
                                                         : hmat[axis * n + j];
                xtrial[j] = x[j] + sign * delta * dj;
            }
            const T ft = feval(xtrial.data());
            if (ft < fbest)
            {
                fbest = ft;
                improved = true;
                for (crd::usize j = 0; j < n; ++j)
                {
                    xbest[j] = xtrial[j];
                }
            }
        }
        if (improved)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                x[j] = xbest[j];
            }
            fx = fbest;
            delta *= ps.expand;
        }
        else
        {
            delta *= ps.contract;
        }
    }

    result.fx = fx;
    result.status = status;
    result.converged = status == OptStatus::Success;
    result.iterations = it;
    return result;
}

} // namespace crd::hesap::opt
