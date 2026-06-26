#pragma once

// conic.hpp — Phase 3.1.6 v7-m: CONIC PROGRAMMING (SCS-class operator splitting, O'Donoghue et al. 2016) —
//
//     min cᵀx   s.t.   Ax + s = b,   s ∈ K        (the SCS data form; dual: max −bᵀy, Aᵀy + c = 0, y ∈ K*)
//
// K is a product of cone blocks, in row order:
//   • Zero    — s = 0 (equality rows Ax = b); dual cone = free.
//   • Nonneg  — s ≥ 0 componentwise (inequality rows Ax ≤ b); self-dual.
//   • Soc     — ‖s₂..d‖₂ ≤ s₁ (second-order / Lorentz, closed-form projection); self-dual.
//   • Psd     — s holds a FULL k×k symmetric matrix row-major (k² rows; symmetrize + eigenvalue clamp via the
//               v3 `dense::eig_sym`); self-dual. NAMED divergence from SCS: SCS packs the scaled lower
//               triangle (off-diagonals ×√2) — same cone, different vectorization; convert at any bridge.
//
// `solve_conic_admm` is the v7-k OSQP iteration with the box projection swapped for the cone projection:
// z tracks Ax over C = {b} − K (Π_C(v) = b − Π_K(b − v)), quasi-definite KKT [σI Aᵀ; A −diag(1/ρ)] factored
// once per ρ (Zero blocks get the 1e3·ρ equality boost), over-relaxation α, deterministic interval-based
// adaptive ρ, and the O'Donoghue/OSQP infeasibility certificates in conic form (δy ∈ K* with bᵀδy < 0 ⇒
// primal infeasible; −Aδx ∈ K with cᵀδx < 0 ⇒ dual infeasible). Termination adds the DUALITY GAP to the
// primal/dual residual pair (the SCS criterion). Duals come out in the SCS convention: y ∈ K*, sᵀy = 0,
// Aᵀy + c = 0 — which IS the OSQP sign of the v7-k family on the shared rows. Honest scope: no polish step
// (SCS has none), no homogeneous self-dual embedding (certificates come from iterate differences instead).
// [gold: SCS, ECOS — the v7-z scoreboard]. DENSE scope, like v7-k/l. ADR-0090.
//
// DETERMINISM: serial fixed-order arithmetic + RNG-free eig_sym ⇒ bit-identical runs.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/opt/qp.hpp> // QpStatus + the detail mat-vec/inf-norm helpers + the LDLT factor seam
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::opt
{

enum class ConeType : crd::u8
{
    Zero,   // s = 0 (dim rows)
    Nonneg, // s ≥ 0 (dim rows)
    Soc,    // ‖s₂..dim‖ ≤ s₁ (dim rows, dim ≥ 1)
    Psd,    // full dim×dim symmetric matrix row-major (dim² rows)
};

struct ConeDesc
{
    ConeType type = ConeType::Zero;
    crd::usize dim = 0; // rows for Zero/Nonneg/Soc; the matrix SIDE for Psd (consumes dim² rows)

    [[nodiscard]] crd::usize rows() const noexcept { return type == ConeType::Psd ? dim * dim : dim; }
};

template <typename T> struct ConicProblem
{
    crd::containers::ConstSpan<T> c; // n
    crd::containers::ConstSpan<T> a; // m×n row-major
    crd::containers::ConstSpan<T> b; // m
    crd::containers::ConstSpan<ConeDesc> cones;
    crd::usize n = 0;
    crd::usize m = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        if (c.size() != n || a.size() != m * n || b.size() != m)
        {
            return false;
        }
        crd::usize rows = 0;
        for (crd::usize k = 0; k < cones.size(); ++k)
        {
            if (cones[k].dim == 0)
            {
                return false;
            }
            rows += cones[k].rows();
        }
        return rows == m;
    }
};

template <typename T> struct ConicResult
{
    crd::containers::Array<T> x;
    crd::containers::Array<T> y; // dual: y ∈ K*, Aᵀy + c = 0, sᵀy = 0
    crd::containers::Array<T> s; // primal slack: Ax + s = b, s ∈ K
    T obj = static_cast<T>(0);   // cᵀx
    QpStatus status = QpStatus::MaxIterations;
    crd::usize iterations = 0;
    T primal_res = static_cast<T>(0); // ‖Ax + s − b‖∞ at exit
    T dual_res = static_cast<T>(0);   // ‖Aᵀy + c‖∞ at exit
    T gap = static_cast<T>(0);        // |cᵀx + bᵀy| at exit

    explicit ConicResult(crd::memory::IAllocator* alloc) noexcept : x(alloc), y(alloc), s(alloc) {}
};

namespace detail
{

// In-place Π_K over the cone blocks; `dual` selects Π_{K*} (Zero ⇒ identity; the rest are self-dual).
template <typename T>
inline void project_cone(crd::containers::ConstSpan<ConeDesc> cones, crd::containers::Span<T> v, bool dual,
                         crd::memory::IAllocator* alloc)
{
    namespace dn = crd::hesap::dense;
    crd::usize off = 0;
    for (crd::usize k = 0; k < cones.size(); ++k)
    {
        const ConeDesc cone = cones[k];
        switch (cone.type)
        {
            case ConeType::Zero:
                if (!dual) // K* of {0} is everything — leave untouched
                {
                    for (crd::usize i = 0; i < cone.dim; ++i)
                    {
                        v[off + i] = static_cast<T>(0);
                    }
                }
                break;
            case ConeType::Nonneg:
                for (crd::usize i = 0; i < cone.dim; ++i)
                {
                    v[off + i] = v[off + i] < static_cast<T>(0) ? static_cast<T>(0) : v[off + i];
                }
                break;
            case ConeType::Soc:
            {
                T wn = static_cast<T>(0);
                for (crd::usize i = 1; i < cone.dim; ++i)
                {
                    wn += v[off + i] * v[off + i];
                }
                wn = crd::math::sqrt(wn);
                const T t = v[off];
                if (wn <= t)
                {
                    break; // inside
                }
                if (wn <= -t)
                {
                    for (crd::usize i = 0; i < cone.dim; ++i) // polar interior ⇒ project to the apex
                    {
                        v[off + i] = static_cast<T>(0);
                    }
                    break;
                }
                const T scale = (t + wn) / (static_cast<T>(2) * wn); // boundary projection
                v[off] = (t + wn) / static_cast<T>(2);
                for (crd::usize i = 1; i < cone.dim; ++i)
                {
                    v[off + i] *= scale;
                }
                break;
            }
            case ConeType::Psd:
            {
                const crd::usize kdim = cone.dim;
                dn::Symmetric<T> sym(alloc, kdim);
                for (crd::usize i = 0; i < kdim; ++i)
                {
                    for (crd::usize j = 0; j <= i; ++j)
                    {
                        sym.at(i, j) =
                            (v[off + i * kdim + j] + v[off + j * kdim + i]) / static_cast<T>(2); // symmetrize
                    }
                }
                const dn::EigSym<T> es = dn::eig_sym<T>(alloc, sym); // values ascending, vectors column k
                const T* lam = es.values.data();
                for (crd::usize i = 0; i < kdim; ++i)
                {
                    for (crd::usize j = 0; j < kdim; ++j)
                    {
                        T acc = static_cast<T>(0);
                        for (crd::usize e = 0; e < kdim; ++e)
                        {
                            if (lam[e] > static_cast<T>(0))
                            {
                                acc += lam[e] * es.vectors.at(i, e) * es.vectors.at(j, e);
                            }
                        }
                        v[off + i * kdim + j] = acc;
                    }
                }
                break;
            }
        }
        off += cone.rows();
    }
}

// ‖v − Π_K(v)‖∞ (or the dual cone) — the certificate distance.
template <typename T>
[[nodiscard]] inline T cone_dist(crd::containers::ConstSpan<ConeDesc> cones, crd::containers::ConstSpan<T> v, bool dual,
                                 crd::memory::IAllocator* alloc)
{
    crd::containers::Array<T> w(alloc);
    w.resize(v.size());
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        w[i] = v[i];
    }
    project_cone<T>(cones, {w.data(), w.size()}, dual, alloc);
    T d = static_cast<T>(0);
    for (crd::usize i = 0; i < v.size(); ++i)
    {
        const T a = crd::math::fabs(v[i] - w[i]);
        d = a > d ? a : d;
    }
    return d;
}

} // namespace detail

template <typename T> struct ConicAdmmOptions
{
    T rho = static_cast<T>(0.1);    // base step (Zero blocks get 1e3·ρ, the equality boost)
    T sigma = static_cast<T>(1e-6); // x-regularization
    T alpha = static_cast<T>(1.6);  // over-relaxation
    T eps_abs = static_cast<T>(1e-7);
    T eps_rel = static_cast<T>(1e-7);
    T eps_infeas = static_cast<T>(1e-7);
    crd::usize max_iters = 50000;
    bool adaptive_rho = true;
    crd::usize adaptive_interval = 100;
};

template <typename T>
[[nodiscard]] ConicResult<T> solve_conic_admm(const ConicProblem<T>& prob, crd::memory::IAllocator* alloc,
                                              const ConicAdmmOptions<T>& opts = {})
{
    namespace dn = crd::hesap::dense;
    CRD_ASSERT_MSG(prob.valid(), "solve_conic_admm: inconsistent problem spans / cone tiling");
    const crd::usize n = prob.n;
    const crd::usize m = prob.m;

    ConicResult<T> result(alloc);
    result.x.resize(n);
    result.y.resize(m);
    result.s.resize(m);
    for (crd::usize i = 0; i < n; ++i)
    {
        result.x[i] = static_cast<T>(0);
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        result.y[i] = static_cast<T>(0);
        result.s[i] = static_cast<T>(0);
    }
    if (n == 0)
    {
        result.status = QpStatus::Solved;
        return result;
    }

    // Per-row ρ with the Zero-block boost.
    crd::containers::Array<T> rho(alloc);
    rho.resize(m);
    T base_rho = opts.rho;
    auto fill_rho = [&]()
    {
        crd::usize off = 0;
        for (crd::usize k = 0; k < prob.cones.size(); ++k)
        {
            const T r = prob.cones[k].type == ConeType::Zero ? static_cast<T>(1e3) * base_rho : base_rho;
            for (crd::usize i = 0; i < prob.cones[k].rows(); ++i)
            {
                rho[off + i] = r;
            }
            off += prob.cones[k].rows();
        }
    };
    fill_rho();

    const crd::usize nk = n + m;
    dn::LDLT<T> kkt(alloc, nk);
    auto refactor = [&]() -> bool
    {
        dn::Symmetric<T> kmat(alloc, nk);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < i; ++j)
            {
                kmat.at(i, j) = static_cast<T>(0);
            }
            kmat.at(i, i) = opts.sigma;
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                kmat.at(n + i, j) = prob.a[i * n + j];
            }
            for (crd::usize j = 0; j < i; ++j)
            {
                kmat.at(n + i, n + j) = static_cast<T>(0);
            }
            kmat.at(n + i, n + i) = -static_cast<T>(1) / rho[i];
        }
        dn::factor_ldlt<T, dn::Layout::RowMajor>(kkt, kmat);
        return kkt.info() == 0;
    };
    if (!refactor())
    {
        result.status = QpStatus::NumericalError;
        return result;
    }

    crd::containers::Array<T> z(alloc); // tracks Ax over C = {b} − K
    crd::containers::Array<T> ax(alloc);
    crd::containers::Array<T> rhs(alloc);
    crd::containers::Array<T> ztilde(alloc);
    crd::containers::Array<T> aty(alloc);
    crd::containers::Array<T> x_prev(alloc);
    crd::containers::Array<T> y_prev(alloc);
    crd::containers::Array<T> tmp_m(alloc);
    z.resize(m);
    ax.resize(m);
    rhs.resize(nk);
    ztilde.resize(m);
    aty.resize(n);
    x_prev.resize(n);
    y_prev.resize(m);
    tmp_m.resize(m);

    auto project_c = [&](crd::containers::Span<T> v) // Π_C(v) = b − Π_K(b − v)
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            tmp_m[i] = prob.b[i] - v[i];
        }
        detail::project_cone<T>(prob.cones, {tmp_m.data(), m}, /*dual=*/false, alloc);
        for (crd::usize i = 0; i < m; ++i)
        {
            v[i] = prob.b[i] - tmp_m[i];
        }
    };

    for (crd::usize i = 0; i < m; ++i) // z₀ = Π_C(Ax₀) = Π_C(0)
    {
        z[i] = static_cast<T>(0);
    }
    project_c({z.data(), m});

    T* x = result.x.data();
    T* y = result.y.data();

    QpStatus status = QpStatus::MaxIterations;
    crd::usize it = 0;
    for (; it < opts.max_iters; ++it)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            x_prev[i] = x[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            y_prev[i] = y[i];
        }

        // (1) the KKT solve: rhs = [σx − c ; z − y/ρ] → x̃ = rhs[0:n], ν = rhs[n:]; z̃ = z + (ν − y)/ρ.
        for (crd::usize i = 0; i < n; ++i)
        {
            rhs[i] = opts.sigma * x[i] - prob.c[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            rhs[n + i] = z[i] - y[i] / rho[i];
        }
        dn::solve_ldlt<T, dn::Layout::RowMajor>(kkt, {rhs.data(), nk});
        for (crd::usize i = 0; i < m; ++i)
        {
            ztilde[i] = z[i] + (rhs[n + i] - y[i]) / rho[i];
        }

        // (2)-(4) relaxed updates + the CONE projection + dual ascent.
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = opts.alpha * rhs[i] + (static_cast<T>(1) - opts.alpha) * x[i];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            const T zr = opts.alpha * ztilde[i] + (static_cast<T>(1) - opts.alpha) * z[i];
            ztilde[i] = zr;                // reuse as the relaxed value
            tmp_m[i] = zr + y[i] / rho[i]; // pre-projection point
        }
        project_c({tmp_m.data(), m});
        for (crd::usize i = 0; i < m; ++i)
        {
            y[i] = y[i] + rho[i] * (ztilde[i] - tmp_m[i]);
            z[i] = tmp_m[i];
        }

        // Residuals + gap (SCS §3.5; computed every iteration — deterministic termination).
        detail::qp_mat_vec<T>(prob.a, m, n, {x, n}, {ax.data(), m});
        detail::qp_mat_tvec<T>(prob.a, m, n, {y, m}, {aty.data(), n});
        T pres = static_cast<T>(0);
        T pscale = static_cast<T>(0);
        for (crd::usize i = 0; i < m; ++i)
        {
            const T d = crd::math::fabs(ax[i] - z[i]);
            pres = d > pres ? d : pres;
            const T s1 = crd::math::fabs(ax[i]);
            const T s2 = crd::math::fabs(z[i]);
            pscale = s1 > pscale ? s1 : pscale;
            pscale = s2 > pscale ? s2 : pscale;
        }
        T dres = static_cast<T>(0);
        T dscale = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            const T d = crd::math::fabs(prob.c[j] + aty[j]);
            dres = d > dres ? d : dres;
            T s = crd::math::fabs(prob.c[j]);
            s = crd::math::fabs(aty[j]) > s ? crd::math::fabs(aty[j]) : s;
            dscale = s > dscale ? s : dscale;
        }
        T ctx = static_cast<T>(0);
        T bty = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            ctx += prob.c[j] * x[j];
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            bty += prob.b[i] * y[i];
        }
        const T gap = crd::math::fabs(ctx + bty);
        const T gscale = crd::math::fabs(ctx) > crd::math::fabs(bty) ? crd::math::fabs(ctx) : crd::math::fabs(bty);
        if (pres <= opts.eps_abs + opts.eps_rel * pscale && dres <= opts.eps_abs + opts.eps_rel * dscale &&
            gap <= opts.eps_abs + opts.eps_rel * gscale)
        {
            status = QpStatus::Solved;
            ++it;
            break;
        }

        // Infeasibility certificates on the iterate differences (conic form).
        {
            T dy_norm = static_cast<T>(0);
            for (crd::usize i = 0; i < m; ++i)
            {
                const T d = crd::math::fabs(y[i] - y_prev[i]);
                dy_norm = d > dy_norm ? d : dy_norm;
            }
            if (dy_norm > static_cast<T>(0))
            {
                crd::containers::Array<T> dy(alloc);
                dy.resize(m);
                T bdy = static_cast<T>(0);
                for (crd::usize i = 0; i < m; ++i)
                {
                    dy[i] = (y[i] - y_prev[i]) / dy_norm;
                    bdy += prob.b[i] * dy[i];
                }
                detail::qp_mat_tvec<T>(prob.a, m, n, {dy.data(), m}, {aty.data(), n});
                if (detail::qp_inf_norm<T>({aty.data(), n}) <= opts.eps_infeas && bdy <= -opts.eps_infeas &&
                    detail::cone_dist<T>(prob.cones, {dy.data(), m}, /*dual=*/true, alloc) <= opts.eps_infeas)
                {
                    status = QpStatus::PrimalInfeasible;
                    ++it;
                    break;
                }
            }
            T dx_norm = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T d = crd::math::fabs(x[i] - x_prev[i]);
                dx_norm = d > dx_norm ? d : dx_norm;
            }
            if (dx_norm > static_cast<T>(0))
            {
                crd::containers::Array<T> dx(alloc);
                dx.resize(n);
                T cdx = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    dx[i] = (x[i] - x_prev[i]) / dx_norm;
                    cdx += prob.c[i] * dx[i];
                }
                detail::qp_mat_vec<T>(prob.a, m, n, {dx.data(), n}, {tmp_m.data(), m});
                for (crd::usize i = 0; i < m; ++i)
                {
                    tmp_m[i] = -tmp_m[i]; // need −Aδx ∈ K (the recession direction keeps s in the cone)
                }
                if (cdx <= -opts.eps_infeas &&
                    detail::cone_dist<T>(prob.cones, {tmp_m.data(), m}, /*dual=*/false, alloc) <= opts.eps_infeas)
                {
                    status = QpStatus::DualInfeasible;
                    ++it;
                    break;
                }
            }
        }

        // Deterministic adaptive ρ (fixed interval, residual-ratio rule — no wall clock).
        if (opts.adaptive_rho && (it + 1) % opts.adaptive_interval == 0)
        {
            const T pr = pres / (pscale > static_cast<T>(1e-30) ? pscale : static_cast<T>(1));
            const T dr = dres / (dscale > static_cast<T>(1e-30) ? dscale : static_cast<T>(1));
            if (pr > static_cast<T>(0) && dr > static_cast<T>(0))
            {
                const T ratio = crd::math::sqrt(pr / dr);
                if (ratio > static_cast<T>(5) || ratio < static_cast<T>(0.2))
                {
                    T next = base_rho * ratio;
                    next = next < static_cast<T>(1e-6) ? static_cast<T>(1e-6)
                                                       : (next > static_cast<T>(1e6) ? static_cast<T>(1e6) : next);
                    base_rho = next;
                    fill_rho();
                    if (!refactor())
                    {
                        status = QpStatus::NumericalError;
                        break;
                    }
                }
            }
        }
    }

    // Exit bookkeeping: s = b − z, obj = cᵀx, the three optimality measures at the final iterate.
    detail::qp_mat_vec<T>(prob.a, m, n, {x, n}, {ax.data(), m});
    detail::qp_mat_tvec<T>(prob.a, m, n, {y, m}, {aty.data(), n});
    T obj = static_cast<T>(0);
    T bty = static_cast<T>(0);
    for (crd::usize j = 0; j < n; ++j)
    {
        obj += prob.c[j] * x[j];
        const T d = crd::math::fabs(prob.c[j] + aty[j]);
        result.dual_res = d > result.dual_res ? d : result.dual_res;
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        result.s[i] = prob.b[i] - z[i];
        const T d = crd::math::fabs(ax[i] + result.s[i] - prob.b[i]);
        result.primal_res = d > result.primal_res ? d : result.primal_res;
        bty += prob.b[i] * y[i];
    }
    result.obj = obj;
    result.gap = crd::math::fabs(obj + bty);
    result.status = status;
    result.iterations = it;
    return result;
}

} // namespace crd::hesap::opt
