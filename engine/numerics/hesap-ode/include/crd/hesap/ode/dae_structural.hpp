#pragma once

// dae_structural.hpp — Phase 3.1.6 v9-l: STRUCTURAL index analysis (Pryce's Σ-method, 2001 — the
// signature-matrix dual of Pantelides' algorithm; both compute the same structural index + per-equation
// differentiation offsets). For a square DAE of n equations in n variables, the signature matrix
//   σ_ij = the highest derivative order of variable j appearing in equation i  (kAbsent if it does not),
// the method finds the Highest-Value Transversal (a max-weight assignment) and the dual offsets
//   c_i ≥ 0  (times equation i must be differentiated),   d_j  (order variable j is differentiated to),
// with d_{T(i)} − c_i = σ_{i,T(i)} on the transversal and d_j − c_i ≥ σ_ij elsewhere (Pryce's fixed-point
// iteration). The structural index is ν_S = max_i c_i + (1 if min_j d_j == 0 else 0): for the Cartesian
// pendulum c=[0,0,2], d=[2,2,0] ⇒ ν_S = 3 (hand-verified); semi-explicit index-1 ⇒ 1; the index-2 chain ⇒ 2;
// a pure ODE ⇒ 0. The offsets ARE the differentiation plan that feeds the index-reduction in dae.hpp (the
// dummy-derivative selection for the constrained-mechanical class). ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ode
{

inline constexpr crd::i32 kSigmaAbsent = -1000000; // variable does not appear in the equation

// Square structural DAE: n equations, n variables, signature matrix sigma (n×n, ROW-MAJOR).
struct StructuralDae
{
    crd::usize n = 0;
    crd::containers::Array<crd::i32> sigma; // n*n; sigma[i*n+j] = highest deriv order, or kSigmaAbsent

    explicit StructuralDae(crd::memory::IAllocator* alloc) : sigma(alloc) {}
    void resize(crd::usize dim)
    {
        n = dim;
        sigma.resize(dim * dim);
        for (crd::usize k = 0; k < dim * dim; ++k)
        {
            sigma[k] = kSigmaAbsent;
        }
    }
    void set(crd::usize eq, crd::usize var, crd::i32 order) { sigma[eq * n + var] = order; }
    [[nodiscard]] crd::i32 at(crd::usize eq, crd::usize var) const { return sigma[eq * n + var]; }
};

struct StructuralResult
{
    crd::i32 index = -1;
    bool ok = false; // a finite transversal exists (structurally well-posed)
    crd::containers::Array<crd::i32> c; // equation offsets (differentiation counts)
    crd::containers::Array<crd::i32> d; // variable offsets
    explicit StructuralResult(crd::memory::IAllocator* alloc) : c(alloc), d(alloc) {}
};

namespace detail
{

// Max-weight perfect transversal (assignment) by DFS over permutations — n is tiny for index analysis.
// Returns true and fills `perm` (perm[i] = variable assigned to equation i) if a finite transversal exists.
inline bool sigma_best_transversal(const StructuralDae& dae, crd::containers::Array<crd::usize>& perm,
                                   crd::containers::Array<bool>& used, crd::usize i, crd::i64 sum, crd::i64& best,
                                   crd::containers::Array<crd::usize>& best_perm, bool& found)
{
    const crd::usize n = dae.n;
    if (i == n)
    {
        if (sum > best)
        {
            best = sum;
            for (crd::usize k = 0; k < n; ++k)
            {
                best_perm[k] = perm[k];
            }
            found = true;
        }
        return found;
    }
    for (crd::usize j = 0; j < n; ++j)
    {
        if (used[j])
        {
            continue;
        }
        const crd::i32 s = dae.at(i, j);
        if (s == kSigmaAbsent)
        {
            continue;
        }
        used[j] = true;
        perm[i] = j;
        sigma_best_transversal(dae, perm, used, i + 1, sum + static_cast<crd::i64>(s), best, best_perm, found);
        used[j] = false;
    }
    return found;
}

} // namespace detail

[[nodiscard]] inline StructuralResult structural_index(const StructuralDae& dae, crd::memory::IAllocator* alloc)
{
    const crd::usize n = dae.n;
    StructuralResult res(alloc);
    res.c.resize(n);
    res.d.resize(n);

    crd::containers::Array<crd::usize> perm(alloc);
    perm.resize(n);
    crd::containers::Array<crd::usize> tperm(alloc);
    tperm.resize(n);
    crd::containers::Array<bool> used(alloc);
    used.resize(n);
    for (crd::usize k = 0; k < n; ++k)
    {
        used[k] = false;
    }
    crd::i64 best = -1;
    bool found = false;
    detail::sigma_best_transversal(dae, perm, used, 0, 0, best, tperm, found);
    if (!found)
    {
        res.ok = false; // structurally singular — no finite transversal
        return res;
    }
    res.ok = true;

    // Pryce fixed-point on the offsets. c starts at 0; iterate until c stops changing.
    for (crd::usize i = 0; i < n; ++i)
    {
        res.c[i] = 0;
    }
    const crd::usize max_iter = n + 2;
    for (crd::usize it = 0; it <= max_iter; ++it)
    {
        // d_j = max_i (sigma_ij + c_i) over present entries.
        for (crd::usize j = 0; j < n; ++j)
        {
            crd::i32 dj = kSigmaAbsent;
            for (crd::usize i = 0; i < n; ++i)
            {
                const crd::i32 s = dae.at(i, j);
                if (s == kSigmaAbsent)
                {
                    continue;
                }
                const crd::i32 cand = s + res.c[i];
                if (cand > dj)
                {
                    dj = cand;
                }
            }
            res.d[j] = dj;
        }
        // c_i = d_{T(i)} − sigma_{i,T(i)}.
        bool changed = false;
        for (crd::usize i = 0; i < n; ++i)
        {
            const crd::usize j = tperm[i];
            const crd::i32 ci = res.d[j] - dae.at(i, j);
            if (ci != res.c[i])
            {
                changed = true;
            }
            res.c[i] = ci;
        }
        if (!changed)
        {
            break;
        }
    }

    crd::i32 max_c = 0;
    for (crd::usize i = 0; i < n; ++i)
    {
        if (res.c[i] > max_c)
        {
            max_c = res.c[i];
        }
    }
    crd::i32 min_d = res.d[0];
    for (crd::usize j = 1; j < n; ++j)
    {
        if (res.d[j] < min_d)
        {
            min_d = res.d[j];
        }
    }
    res.index = max_c + (min_d == 0 ? 1 : 0);
    return res;
}

} // namespace crd::hesap::ode
