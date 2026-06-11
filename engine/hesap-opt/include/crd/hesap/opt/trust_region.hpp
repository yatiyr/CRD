#pragma once

// trust_region.hpp — Phase 3.1.6 v7-h: the TRUST-REGION framework (Nocedal & Wright Algorithm 4.1 radius
// management) + the full subproblem ladder for min m(p) = f + gᵀp + ½pᵀHp s.t. ‖p‖ ≤ Δ:
//   • Cauchy        — steepest descent to the model minimizer/boundary (the convergence-theory floor; 1 H·v).
//   • Dogleg        — pU→pB path (N&W §4.1; requires PD H — falls back to Cauchy on a failed factor).
//   • Subspace2D    — exact minimization over span{g, pB} (N&W §4.2; indefinite H uses (H+τI)⁻¹g), via the
//                     2×2 exact solver below.
//   • SteihaugCg    — CG-Steihaug (N&W Alg 7.2): matrix-free, boundary + negative-curvature exits, the
//                     Eisenstat-Walker forcing η = min(0.5, √‖g‖) ⇒ superlinear [gold: scipy 'trust-ncg'].
//   • TrustKrylov   — GLTR (Gould-Lucidi-Roma-Toint 1999): Lanczos with FULL reorthogonalization; at each step
//                     the k×k tridiagonal subproblem is solved EXACTLY (the solver below) and the GLTR residual
//                     β_k·|y_k| tested — boundary solutions handled exactly, unlike Steihaug
//                     [gold: scipy 'trust-krylov' = Gould's GLTR].
//   • Exact         — Moré-Sorensen-exact via EIGENDECOMPOSITION (the v3 `dense::eig_sym` that beats LAPACK):
//                     interior test, safeguarded secular Newton on 1/Δ − 1/‖p(λ)‖, and the HARD CASE (g ⊥ the
//                     minimum eigenspace) closed-form in the eigenbasis [gold: scipy 'trust-exact', GALAHAD].
// `solve_trust_region_subproblem_exact` is PUBLIC — the v7-k QP and eylem want a certified TR subproblem solve.
// ADR-0090; N&W Ch.4 + §7.1; Moré-Sorensen 1983; GLTR 1999.
//
// DETERMINISM MOAT: the framework, the secular iteration, the CG/Lanczos recurrences, and `eig_sym` (RNG-free,
// deterministic by D(dense-eig)-1..5) are all SERIAL fixed-order arithmetic; only the objective eval (value /
// gradient / hessian_vector) may be parallel-but-bit-exact ⇒ the trajectory is bit-identical across workers.
//
// EVAL ACCOUNTING: fn_evals = one trial f per outer iteration; grad_evals = one per ACCEPTED step;
// hess_evals = hessian() per x-refresh (dense paths) or hessian_vector() PRODUCTS (Steihaug / GLTR / Cauchy).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/opt/convergence.hpp>
#include <crd/hesap/opt/levenberg_marquardt.hpp> // detail::chol_solve (dense SPD factor + solve)
#include <crd/hesap/opt/objective.hpp>
#include <crd/hesap/opt/opt_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::opt
{

enum class TrustRegionSubproblem : crd::u8
{
    Cauchy,      // needs hessian_vector
    Dogleg,      // needs hessian (dense)
    Subspace2D,  // needs hessian (dense)
    SteihaugCg,  // needs hessian_vector
    TrustKrylov, // needs hessian_vector
    Exact        // needs hessian (dense)
};

template <typename T> struct TrustRegionOptions
{
    TrustRegionSubproblem subproblem = TrustRegionSubproblem::SteihaugCg;
    T delta0 = static_cast<T>(1);      // initial radius
    T delta_max = static_cast<T>(1e3); // radius cap (the scipy default)
    T eta = static_cast<T>(0.15);      // accept the step iff ρ > η (N&W: η ∈ [0, ¼))
    crd::usize cg_max_iters = 0;       // Steihaug inner cap (0 ⇒ 2n)
    crd::usize krylov_max_dim = 0;     // GLTR Lanczos cap (0 ⇒ min(n, 64))
};

// Result of one exact subproblem solve: the multiplier λ ≥ 0 of ‖p‖ ≤ Δ ((H+λI)p = −g, H+λI ⪰ 0,
// λ·(Δ−‖p‖) = 0 — the Moré-Sorensen KKT certificate), the model reduction m(0)−m(p) ≥ 0, and whether the
// solution sits on the boundary.
template <typename T> struct TrustRegionSubproblemResult
{
    T lambda = static_cast<T>(0);
    T pred = static_cast<T>(0);
    bool hits_boundary = false;
};

// ---------------------------------------------------------------------------------------------------------
// solve_trust_region_subproblem_exact — minimize gᵀp + ½pᵀHp s.t. ‖p‖₂ ≤ Δ, EXACTLY, via the eigendecomposition
// H = QΛQᵀ (dense::eig_sym): interior solve when Λ ≻ 0 and ‖H⁻¹g‖ ≤ Δ; otherwise the boundary root of the
// safeguarded secular Newton on φ(λ) = 1/Δ − 1/‖p(λ)‖ over λ ∈ (max(0, −λ₁), ∞); the HARD CASE (g ⊥ the
// λ₁-eigenspace and the limit norm < Δ) closed-form with a boundary component along q₁. PUBLIC: the certified
// TR solve the v7-k QP and the GLTR/Subspace2D inner steps reuse. O(n³) once per call.
// ---------------------------------------------------------------------------------------------------------
template <typename T>
[[nodiscard]] TrustRegionSubproblemResult<T>
solve_trust_region_subproblem_exact(crd::memory::IAllocator* alloc, const crd::hesap::dense::Symmetric<T>& h,
                                    crd::containers::ConstSpan<T> g, T delta, crd::containers::Span<T> p)
{
    namespace dn = crd::hesap::dense;
    const crd::usize n = h.n();
    CRD_ASSERT_MSG(g.size() == n && p.size() == n, "solve_trust_region_subproblem_exact: span size mismatch");
    CRD_ASSERT_MSG(delta > static_cast<T>(0), "solve_trust_region_subproblem_exact: delta must be > 0");

    TrustRegionSubproblemResult<T> out;
    if (n == 0)
    {
        return out;
    }

    const dn::EigSym<T> es = dn::eig_sym<T>(alloc, h); // values ascending, vectors column k
    const T* lam = es.values.data();
    const T lam1 = lam[0];

    crd::containers::Array<T> d(alloc); // d = Qᵀg
    crd::containers::Array<T> y(alloc); // the solution in the eigenbasis
    d.resize(n);
    y.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        T acc = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            acc += es.vectors.at(j, i) * g[j];
        }
        d[i] = acc;
    }

    // ‖y(λ)‖ for y_i(λ) = −d_i/(λ_i+λ); components with λ_i+λ ≈ 0 contribute per the hard-case logic (skipped
    // here — the caller paths guarantee λ strictly above −λ₁ when this lambda is used).
    auto norm_at = [&](T lambda) -> T
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T den = lam[i] + lambda;
            const T yi = -d[i] / den;
            s += yi * yi;
        }
        return std::sqrt(s);
    };
    auto finish = [&](T lambda, bool boundary) -> void
    {
        // p = Q·y; pred = −(dᵀy + ½Σλ_i y_i²).
        T pred = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            pred += d[i] * y[i] + static_cast<T>(0.5) * lam[i] * y[i] * y[i];
        }
        out.pred = -pred;
        out.lambda = lambda;
        out.hits_boundary = boundary;
        for (crd::usize j = 0; j < n; ++j)
        {
            T acc = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                acc += es.vectors.at(j, i) * y[i];
            }
            p[j] = acc;
        }
    };

    // Interior candidate (λ = 0) when H ≻ 0.
    if (lam1 > static_cast<T>(0))
    {
        T s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = -d[i] / lam[i];
            s += y[i] * y[i];
        }
        if (std::sqrt(s) <= delta)
        {
            finish(static_cast<T>(0), false);
            return out;
        }
    }

    // Boundary. λ ∈ (λ_lo, ∞), λ_lo = max(0, −λ₁). Hard case first: the λ₁-eigenspace components of g vanish
    // and the λ → −λ₁ limit already fits inside Δ ⇒ pad with a boundary component along q₁ (only when λ₁ ≤ 0;
    // for λ₁ > 0 the interior test above failed ⇒ ‖y(0)‖ > Δ and a regular root exists).
    const T lam_lo = lam1 < static_cast<T>(0) ? -lam1 : static_cast<T>(0);
    T scale = std::fabs(lam[n - 1]) > std::fabs(lam1) ? std::fabs(lam[n - 1]) : std::fabs(lam1);
    scale = scale > static_cast<T>(1) ? scale : static_cast<T>(1);
    const T cluster_tol = std::sqrt(std::numeric_limits<T>::epsilon()) * scale;

    if (lam1 <= static_cast<T>(0))
    {
        T limit_sq = static_cast<T>(0); // Σ over λ_i strictly above the λ₁ cluster of d_i²/(λ_i−λ₁)²
        T dmin_sq = static_cast<T>(0);  // Σ over the λ₁ cluster of d_i²
        for (crd::usize i = 0; i < n; ++i)
        {
            if (lam[i] - lam1 <= cluster_tol)
            {
                dmin_sq += d[i] * d[i];
            }
            else
            {
                const T yi = -d[i] / (lam[i] - lam1);
                limit_sq += yi * yi;
            }
        }
        if (dmin_sq <= cluster_tol * cluster_tol && limit_sq <= delta * delta)
        {
            // HARD CASE: y = the limit solution + τ along the FIRST λ₁-cluster direction so ‖y‖ = Δ.
            const T tau = std::sqrt(delta * delta - limit_sq);
            bool tau_placed = false;
            for (crd::usize i = 0; i < n; ++i)
            {
                if (lam[i] - lam1 <= cluster_tol)
                {
                    y[i] = !tau_placed ? tau : static_cast<T>(0);
                    tau_placed = true;
                }
                else
                {
                    y[i] = -d[i] / (lam[i] - lam1);
                }
            }
            finish(lam_lo, true);
            return out;
        }
    }

    // Regular boundary root: safeguarded Newton on φ(λ) = 1/Δ − 1/‖y(λ)‖ (concave-safe; Moré-Sorensen).
    // Bracket: lo just above λ_lo (φ < 0 there: ‖y‖ → ∞ or > Δ), hi = λ_lo + ‖d‖/Δ (‖y(hi)‖ ≤ ‖d‖/(hi−(−λ₁)) ≤ Δ).
    T dnorm = static_cast<T>(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        dnorm += d[i] * d[i];
    }
    dnorm = std::sqrt(dnorm);
    T lo = lam_lo;
    T hi = lam_lo + dnorm / delta + cluster_tol;
    T lambda = lam_lo + (dnorm / delta) * static_cast<T>(0.5) + cluster_tol; // start inside the bracket

    const T secular_tol = static_cast<T>(100) * std::numeric_limits<T>::epsilon(); // relative on ‖p‖ vs Δ
    for (int iter = 0; iter < 200; ++iter)
    {
        const T nrm = norm_at(lambda);
        if (std::fabs(nrm - delta) <= secular_tol * delta)
        {
            break;
        }
        if (nrm > delta)
        {
            lo = lambda; // need larger λ
        }
        else
        {
            hi = lambda;
        }
        // Newton step on φ(λ) = 1/Δ − 1/n(λ):  φ' = n'(λ)/n², n' = −(Σ d_i²/(λ_i+λ)³)/n.
        T dn_sum = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T den = lam[i] + lambda;
            dn_sum += (d[i] * d[i]) / (den * den * den);
        }
        const T nprime = -dn_sum / nrm;
        const T phi = static_cast<T>(1) / delta - static_cast<T>(1) / nrm;
        const T phiprime = nprime / (nrm * nrm);
        T next = lambda - phi / phiprime;
        if (!(next > lo) || !(next < hi) || !std::isfinite(next))
        {
            next = static_cast<T>(0.5) * (lo + hi); // bisection safeguard
        }
        lambda = next;
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        y[i] = -d[i] / (lam[i] + lambda);
    }
    finish(lambda, true);
    return out;
}

namespace detail
{

// m(p) = gᵀp + ½pᵀ(Hp) given Hp; pred = −m(p).
template <typename T>
[[nodiscard]] inline T model_reduction(crd::containers::ConstSpan<T> g, crd::containers::ConstSpan<T> p,
                                       crd::containers::ConstSpan<T> hp) noexcept
{
    namespace dn = crd::hesap::dense;
    return -(dn::dot<T>(g, p) + static_cast<T>(0.5) * dn::dot<T>(p, hp));
}

// τ ≥ 0 with ‖z + τ·d‖ = Δ (the positive root) — the Steihaug boundary crossing.
template <typename T>
[[nodiscard]] inline T boundary_tau(crd::containers::ConstSpan<T> z, crd::containers::ConstSpan<T> d, T delta) noexcept
{
    namespace dn = crd::hesap::dense;
    const T zd = dn::dot<T>(z, d);
    const T dd = dn::dot<T>(d, d);
    const T zz = dn::dot<T>(z, z);
    const T disc = zd * zd + dd * (delta * delta - zz);
    const T root = std::sqrt(disc > static_cast<T>(0) ? disc : static_cast<T>(0));
    return (-zd + root) / dd;
}

} // namespace detail

// Minimize `obj` from `x0` by trust-region (N&W Algorithm 4.1). Capability requirements depend on the
// subproblem: Cauchy/SteihaugCg/TrustKrylov need hessian_vector; Dogleg/Subspace2D/Exact need the dense hessian.
template <typename T>
[[nodiscard]] OptResult<T> minimize_trust_region(const Objective<T>& obj, crd::containers::ConstSpan<T> x0,
                                                 const OptOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                 const TrustRegionOptions<T>& tr = {})
{
    namespace dn = crd::hesap::dense;
    using Sub = TrustRegionSubproblem;
    const bool dense_h =
        tr.subproblem == Sub::Dogleg || tr.subproblem == Sub::Subspace2D || tr.subproblem == Sub::Exact;
    CRD_ASSERT_MSG(obj.has_gradient(), "minimize_trust_region needs an analytic gradient");
    CRD_ASSERT_MSG(!dense_h || obj.has_hessian(),
                   "Dogleg/Subspace2D/Exact trust-region need the dense hessian capability");
    CRD_ASSERT_MSG(dense_h || obj.has_hessian_vector(),
                   "Cauchy/SteihaugCg/TrustKrylov trust-region need the hessian_vector capability");
    const crd::usize n = obj.n();

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
    const crd::usize cg_cap = tr.cg_max_iters > 0 ? tr.cg_max_iters : 2 * n;
    crd::usize kry_cap = tr.krylov_max_dim > 0 ? tr.krylov_max_dim : (n < 64 ? n : 64);
    kry_cap = kry_cap < n ? kry_cap : n;

    auto inf_nrm = [](crd::containers::ConstSpan<T> w) -> T
    {
        T mx = static_cast<T>(0);
        for (crd::usize i = 0; i < w.size(); ++i)
        {
            const T a = std::fabs(w[i]);
            mx = a > mx ? a : mx;
        }
        return mx;
    };

    crd::containers::Array<T> g(alloc);
    crd::containers::Array<T> p(alloc);
    crd::containers::Array<T> x_new(alloc);
    crd::containers::Array<T> scratch_a(alloc); // Hv / Hp / CG residual workspaces
    crd::containers::Array<T> scratch_b(alloc);
    crd::containers::Array<T> scratch_c(alloc);
    crd::containers::Array<T> h(alloc);   // dense Hessian (dense paths)
    crd::containers::Array<T> mtx(alloc); // dense factor scratch
    g.resize(n);
    p.resize(n);
    x_new.resize(n);
    scratch_a.resize(n);
    scratch_b.resize(n);
    scratch_c.resize(n);
    if (dense_h)
    {
        h.resize(n * n);
        mtx.resize(n * n);
    }

    T* x = result.x.data();
    T fx = obj.value({x, n});
    ++result.fn_evals;
    (void)obj.gradient({x, n}, {g.data(), n});
    ++result.grad_evals;
    T grad_norm = inf_nrm({g.data(), n});

    // Hessian-vector through either capability (the dense paths multiply the stored H — no extra eval).
    auto hv = [&](crd::containers::ConstSpan<T> v, crd::containers::Span<T> out_hv) -> void
    {
        if (dense_h)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                T acc = static_cast<T>(0);
                for (crd::usize j = 0; j < n; ++j)
                {
                    acc += h[i * n + j] * v[j];
                }
                out_hv[i] = acc;
            }
        }
        else
        {
            (void)obj.hessian_vector({x, n}, v, out_hv);
            ++result.hess_evals; // H·v products = the matrix-free cost metric
        }
    };

    bool need_h = true; // refresh the dense Hessian only when x moved
    T delta = tr.delta0;
    OptStatus status = OptStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        if (opts.record_history)
        {
            result.history.push_back(fx);
        }
        if (grad_norm <= opts.grad_tol)
        {
            status = OptStatus::Success;
            break;
        }
        if (dense_h && need_h)
        {
            (void)obj.hessian({x, n}, {h.data(), n * n});
            ++result.hess_evals;
            need_h = false;
        }

        // ---- Solve the subproblem: p + pred + boundary flag ----
        T pred = static_cast<T>(0);
        bool boundary = false;
        switch (tr.subproblem)
        {
            case Sub::Cauchy:
            {
                const T gnorm2 = std::sqrt(dn::dot<T>({g.data(), n}, {g.data(), n}));
                hv({g.data(), n}, {scratch_a.data(), n}); // Hg
                const T ghg = dn::dot<T>({g.data(), n}, {scratch_a.data(), n});
                T tau = static_cast<T>(1);
                boundary = true;
                if (ghg > static_cast<T>(0))
                {
                    const T t_star = gnorm2 * gnorm2 * gnorm2 / (delta * ghg);
                    tau = t_star < static_cast<T>(1) ? t_star : static_cast<T>(1);
                    boundary = tau >= static_cast<T>(1);
                }
                const T coef = -tau * delta / gnorm2;
                for (crd::usize i = 0; i < n; ++i)
                {
                    p[i] = coef * g[i];
                    scratch_a[i] *= coef; // Hp
                }
                pred = detail::model_reduction<T>({g.data(), n}, {p.data(), n}, {scratch_a.data(), n});
                break;
            }
            case Sub::Dogleg:
            {
                // pB = −H⁻¹g (PD path); on a failed factor fall back to the Cauchy point (dense Hg available).
                for (crd::usize k = 0; k < n * n; ++k)
                {
                    mtx[k] = h[k];
                }
                for (crd::usize i = 0; i < n; ++i)
                {
                    scratch_b[i] = -g[i];
                }
                const bool pd = detail::chol_solve<T>(mtx.data(), n, scratch_b.data()); // scratch_b = pB
                if (!pd)
                {
                    const T gnorm2 = std::sqrt(dn::dot<T>({g.data(), n}, {g.data(), n}));
                    hv({g.data(), n}, {scratch_a.data(), n});
                    const T ghg = dn::dot<T>({g.data(), n}, {scratch_a.data(), n});
                    T tau = static_cast<T>(1);
                    if (ghg > static_cast<T>(0))
                    {
                        const T t_star = gnorm2 * gnorm2 * gnorm2 / (delta * ghg);
                        tau = t_star < static_cast<T>(1) ? t_star : static_cast<T>(1);
                    }
                    const T coef = -tau * delta / gnorm2;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        p[i] = coef * g[i];
                    }
                    boundary = true;
                }
                else
                {
                    const T pb_norm = std::sqrt(dn::dot<T>({scratch_b.data(), n}, {scratch_b.data(), n}));
                    if (pb_norm <= delta)
                    {
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            p[i] = scratch_b[i]; // the full (Newton) step is inside
                        }
                        boundary = false;
                    }
                    else
                    {
                        hv({g.data(), n}, {scratch_a.data(), n}); // Hg
                        const T gg = dn::dot<T>({g.data(), n}, {g.data(), n});
                        const T ghg = dn::dot<T>({g.data(), n}, {scratch_a.data(), n});
                        const T pu_coef = -gg / ghg; // PD ⇒ ghg > 0
                        const T pu_norm = -pu_coef * std::sqrt(gg);
                        boundary = true;
                        if (pu_norm >= delta)
                        {
                            const T coef = -delta / std::sqrt(gg);
                            for (crd::usize i = 0; i < n; ++i)
                            {
                                p[i] = coef * g[i]; // boundary along −g
                            }
                        }
                        else
                        {
                            // p(τ) = pU + τ(pB − pU), ‖p(τ)‖ = Δ, τ ∈ (0, 1].
                            for (crd::usize i = 0; i < n; ++i)
                            {
                                scratch_c[i] = scratch_b[i] - pu_coef * g[i]; // pB − pU
                                p[i] = pu_coef * g[i];                        // pU (reused as z below)
                            }
                            const T tau = detail::boundary_tau<T>({p.data(), n}, {scratch_c.data(), n}, delta);
                            for (crd::usize i = 0; i < n; ++i)
                            {
                                p[i] += tau * scratch_c[i];
                            }
                        }
                    }
                }
                hv({p.data(), n}, {scratch_a.data(), n});
                pred = detail::model_reduction<T>({g.data(), n}, {p.data(), n}, {scratch_a.data(), n});
                break;
            }
            case Sub::Subspace2D:
            {
                // Basis {g, s} with s = (H + τI)⁻¹g (τ = 0 when PD; else the N&W 3.4 ladder), orthonormalized;
                // the projected 2×2 (or 1×1 when collinear) subproblem is solved EXACTLY.
                T mindiag = h[0];
                T maxabsdiag = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    const T dgi = h[i * n + i];
                    mindiag = dgi < mindiag ? dgi : mindiag;
                    const T a = std::fabs(dgi);
                    maxabsdiag = a > maxabsdiag ? a : maxabsdiag;
                }
                const T beta_reg =
                    static_cast<T>(1e-3) * (maxabsdiag > static_cast<T>(0) ? maxabsdiag : static_cast<T>(1));
                T tau_reg = mindiag > static_cast<T>(0) ? static_cast<T>(0) : (-mindiag + beta_reg);
                bool fok = false;
                for (int attempt = 0; attempt < 60 && !fok; ++attempt)
                {
                    for (crd::usize k = 0; k < n * n; ++k)
                    {
                        mtx[k] = h[k];
                    }
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        mtx[i * n + i] += tau_reg;
                        scratch_b[i] = g[i];
                    }
                    fok = detail::chol_solve<T>(mtx.data(), n, scratch_b.data()); // scratch_b = (H+τI)⁻¹g
                    if (!fok)
                    {
                        const T doubled = static_cast<T>(2) * tau_reg;
                        tau_reg = doubled > beta_reg ? doubled : beta_reg; // τ ← max(2τ, β)
                    }
                }
                // Orthonormal basis: b1 = g/‖g‖; b2 = s − (b1ᵀs)b1, normalized (dropped if collinear).
                const T gnorm2 = std::sqrt(dn::dot<T>({g.data(), n}, {g.data(), n}));
                for (crd::usize i = 0; i < n; ++i)
                {
                    scratch_a[i] = g[i] / gnorm2; // b1
                }
                crd::usize dim2 = 1;
                if (fok)
                {
                    const T proj = dn::dot<T>({scratch_a.data(), n}, {scratch_b.data(), n});
                    T res = static_cast<T>(0);
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        scratch_c[i] = scratch_b[i] - proj * scratch_a[i];
                        res += scratch_c[i] * scratch_c[i];
                    }
                    res = std::sqrt(res);
                    const T s_norm = std::sqrt(dn::dot<T>({scratch_b.data(), n}, {scratch_b.data(), n}));
                    if (res > std::sqrt(std::numeric_limits<T>::epsilon()) * (s_norm + static_cast<T>(1)))
                    {
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            scratch_c[i] /= res; // b2
                        }
                        dim2 = 2;
                    }
                }
                // Projected subproblem over B = [b1 (b2)]: H̃ = BᵀHB, g̃ = Bᵀg; solve exactly (1×1 or 2×2).
                dn::Symmetric<T> hsmall(alloc, dim2);
                crd::containers::Array<T> gsmall(alloc);
                crd::containers::Array<T> ysmall(alloc);
                gsmall.resize(dim2);
                ysmall.resize(dim2);
                const T* basis[2] = {scratch_a.data(), scratch_c.data()};
                crd::containers::Array<T> hb(alloc);
                hb.resize(n);
                for (crd::usize c = 0; c < dim2; ++c)
                {
                    hv({basis[c], n}, {hb.data(), n});
                    for (crd::usize r = c; r < dim2; ++r)
                    {
                        T acc = static_cast<T>(0);
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            acc += basis[r][i] * hb[i];
                        }
                        hsmall.at(r, c) = acc;
                    }
                    T gacc = static_cast<T>(0);
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        gacc += basis[c][i] * g[i];
                    }
                    gsmall[c] = gacc;
                }
                const auto sub = solve_trust_region_subproblem_exact<T>(alloc, hsmall, {gsmall.data(), dim2}, delta,
                                                                        {ysmall.data(), dim2});
                for (crd::usize i = 0; i < n; ++i)
                {
                    p[i] = ysmall[0] * basis[0][i] + (dim2 == 2 ? ysmall[1] * basis[1][i] : static_cast<T>(0));
                }
                pred = sub.pred; // B orthonormal ⇒ the projected model value IS the full model value
                boundary = sub.hits_boundary;
                break;
            }
            case Sub::SteihaugCg:
            {
                // N&W Alg 7.2. z = 0, r = g, d = −g; exits: negative curvature → boundary; ‖z⁺‖ ≥ Δ → boundary;
                // ‖r‖ ≤ η‖g‖ (η = min(0.5, √‖g‖)) → interior truncation.
                T* z = p.data();
                T* r = scratch_a.data();
                T* dvec = scratch_b.data();
                T* hd = scratch_c.data();
                const T gnorm2 = std::sqrt(dn::dot<T>({g.data(), n}, {g.data(), n}));
                const T eta_f = std::sqrt(gnorm2) < static_cast<T>(0.5) ? std::sqrt(gnorm2) : static_cast<T>(0.5);
                const T inner_tol = eta_f * gnorm2;
                for (crd::usize i = 0; i < n; ++i)
                {
                    z[i] = static_cast<T>(0);
                    r[i] = g[i];
                    dvec[i] = -g[i];
                }
                T rr = dn::dot<T>({r, n}, {r, n});
                boundary = false;
                for (crd::usize j = 0; j < cg_cap; ++j)
                {
                    hv({dvec, n}, {hd, n});
                    const T dhd = dn::dot<T>({dvec, n}, {hd, n});
                    if (!(dhd > static_cast<T>(0)))
                    {
                        const T tau = detail::boundary_tau<T>({z, n}, {dvec, n}, delta);
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            z[i] += tau * dvec[i];
                        }
                        boundary = true;
                        break;
                    }
                    const T alpha = rr / dhd;
                    T znew_sq = static_cast<T>(0);
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        const T zi = z[i] + alpha * dvec[i];
                        znew_sq += zi * zi;
                    }
                    if (std::sqrt(znew_sq) >= delta)
                    {
                        const T tau = detail::boundary_tau<T>({z, n}, {dvec, n}, delta);
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            z[i] += tau * dvec[i];
                        }
                        boundary = true;
                        break;
                    }
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        z[i] += alpha * dvec[i];
                        r[i] += alpha * hd[i];
                    }
                    const T rr_new = dn::dot<T>({r, n}, {r, n});
                    if (std::sqrt(rr_new) <= inner_tol)
                    {
                        break;
                    }
                    const T beta_cg = rr_new / rr;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        dvec[i] = -r[i] + beta_cg * dvec[i];
                    }
                    rr = rr_new;
                }
                hv({p.data(), n}, {scratch_c.data(), n});
                pred = detail::model_reduction<T>({g.data(), n}, {p.data(), n}, {scratch_c.data(), n});
                break;
            }
            case Sub::TrustKrylov:
            {
                // GLTR: Lanczos (FULL reorthogonalization, V stored) + the k×k tridiagonal subproblem solved
                // EXACTLY each step; converged when the GLTR residual β_k·|y_k| ≤ η‖g‖ (same forcing as Steihaug).
                const T gnorm2 = std::sqrt(dn::dot<T>({g.data(), n}, {g.data(), n}));
                const T eta_f = std::sqrt(gnorm2) < static_cast<T>(0.5) ? std::sqrt(gnorm2) : static_cast<T>(0.5);
                const T inner_tol = eta_f * gnorm2;
                crd::containers::Array<T> vmat(alloc); // V columns, n each
                crd::containers::Array<T> alphas(alloc);
                crd::containers::Array<T> betas(alloc); // β_k = ‖w‖ after step k
                crd::containers::Array<T> w(alloc);
                crd::containers::Array<T> ty(alloc);
                crd::containers::Array<T> tg(alloc);
                vmat.resize(n * kry_cap);
                alphas.resize(kry_cap);
                betas.resize(kry_cap);
                w.resize(n);
                ty.resize(kry_cap);
                tg.resize(kry_cap);
                for (crd::usize i = 0; i < n; ++i)
                {
                    vmat[i] = g[i] / gnorm2; // v₁
                }
                crd::usize kdim = 0;
                TrustRegionSubproblemResult<T> sub;
                for (crd::usize k = 0; k < kry_cap; ++k)
                {
                    const T* vk = vmat.data() + k * n;
                    hv({vk, n}, {w.data(), n});
                    const T ak = dn::dot<T>({vk, n}, {w.data(), n});
                    alphas[k] = ak;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        w[i] -= ak * vk[i];
                    }
                    if (k > 0)
                    {
                        const T* vprev = vmat.data() + (k - 1) * n;
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            w[i] -= betas[k - 1] * vprev[i];
                        }
                    }
                    for (crd::usize j = 0; j <= k; ++j) // full reorthogonalization
                    {
                        const T* vj = vmat.data() + j * n;
                        const T proj = dn::dot<T>({w.data(), n}, {vj, n});
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            w[i] -= proj * vj[i];
                        }
                    }
                    const T bk = std::sqrt(dn::dot<T>({w.data(), n}, {w.data(), n}));
                    betas[k] = bk;
                    kdim = k + 1;

                    // Solve the kdim×kdim tridiagonal subproblem exactly: m(y) = ‖g‖e₁ᵀy + ½yᵀT y, ‖y‖ ≤ Δ.
                    dn::Symmetric<T> tsmall(alloc, kdim);
                    for (crd::usize i = 0; i < kdim; ++i)
                    {
                        tsmall.at(i, i) = alphas[i];
                        if (i + 1 < kdim)
                        {
                            tsmall.at(i + 1, i) = betas[i];
                        }
                        tg[i] = i == 0 ? gnorm2 : static_cast<T>(0);
                    }
                    sub = solve_trust_region_subproblem_exact<T>(alloc, tsmall, {tg.data(), kdim}, delta,
                                                                 {ty.data(), kdim});
                    const T residual = bk * std::fabs(ty[kdim - 1]); // the GLTR convergence estimate
                    if (residual <= inner_tol || bk <= std::numeric_limits<T>::epsilon() * gnorm2 || k + 1 == kry_cap)
                    {
                        break;
                    }
                    T* vnext = vmat.data() + (k + 1) * n;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        vnext[i] = w[i] / bk;
                    }
                }
                for (crd::usize i = 0; i < n; ++i)
                {
                    T acc = static_cast<T>(0);
                    for (crd::usize j = 0; j < kdim; ++j)
                    {
                        acc += ty[j] * vmat[j * n + i];
                    }
                    p[i] = acc;
                }
                pred = sub.pred; // V orthonormal ⇒ the Krylov model value IS the full model value
                boundary = sub.hits_boundary;
                break;
            }
            case Sub::Exact:
            default:
            {
                dn::Symmetric<T> hsym(alloc, n);
                for (crd::usize i = 0; i < n; ++i)
                {
                    for (crd::usize j = 0; j <= i; ++j)
                    {
                        hsym.at(i, j) = h[i * n + j];
                    }
                }
                const auto sub =
                    solve_trust_region_subproblem_exact<T>(alloc, hsym, {g.data(), n}, delta, {p.data(), n});
                pred = sub.pred;
                boundary = sub.hits_boundary;
                break;
            }
        }

        if (!(pred > static_cast<T>(0)) || !std::isfinite(pred))
        {
            status = OptStatus::SmallStep; // the model cannot be reduced further at this scale
            break;
        }

        // ---- ρ-test + radius update (N&W Alg 4.1) ----
        for (crd::usize i = 0; i < n; ++i)
        {
            x_new[i] = x[i] + p[i];
        }
        const T fx_trial = obj.value({x_new.data(), n});
        ++result.fn_evals;
        const T rho = (fx - fx_trial) / pred;

        if (!(rho > static_cast<T>(0.25)) || !std::isfinite(fx_trial))
        {
            delta *= static_cast<T>(0.25);
        }
        else if (rho > static_cast<T>(0.75) && boundary)
        {
            const T grown = static_cast<T>(2) * delta;
            delta = grown < tr.delta_max ? grown : tr.delta_max;
        }

        if (rho > tr.eta && std::isfinite(fx_trial))
        {
            T step_norm_sq = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                step_norm_sq += p[i] * p[i];
                x[i] = x_new[i];
            }
            const T df = std::fabs(fx_trial - fx);
            fx = fx_trial;
            (void)obj.gradient({x, n}, {g.data(), n});
            ++result.grad_evals;
            grad_norm = inf_nrm({g.data(), n});
            need_h = true;
            const auto stop = check_convergence<T>(grad_norm, std::sqrt(step_norm_sq), df, inf_nrm({x, n}), fx, opts);
            if (stop.has_value())
            {
                status = *stop;
                break;
            }
        }

        // Stall guard: a radius collapsed to rounding scale can never produce an acceptable step again.
        if (delta < std::numeric_limits<T>::epsilon() * (static_cast<T>(1) + inf_nrm({x, n})))
        {
            status = OptStatus::SmallStep;
            break;
        }
    }

    result.fx = fx;
    result.grad_norm = grad_norm;
    result.iterations = it;
    result.status = status;
    result.converged = (status == OptStatus::Success);
    return result;
}

} // namespace crd::hesap::opt
