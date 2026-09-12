#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner / krylov_mag / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// IDR(s) -- Induced Dimension Reduction (Sonneveld-van Gijzen 2008). Phase 3.1.6 v4d-2b.
//
// SHORT-recurrence Krylov solver for GENERAL (nonsymmetric / non-Hermitian) A that
// needs ONLY A·x (no Aᴴ, unlike QMR). It drives the residual into a shrinking
// sequence of nested subspaces G_j of codimension s, defined by a fixed s-dim
// "shadow space" P; for s ≥ 2 it typically beats BiCGSTAB on hard nonsymmetric
// systems. Eigen ships IDRS (in the unsupported IterativeSolvers module) -- this is
// the apples-to-apples crush target.
//
// SHADOW SPACE P (s columns) is generated deterministically (seeded LCG) and
// MGS-ORTHONORMALIZED in the workspace ctor (van Gijzen-Sonneveld + PETSc/MATLAB
// all orthonormalize; skipping it weakens convergence). Because P is owned by the
// workspace and fixed, two solves over the same workspace size + seed are
// bit-identical ⇒ the determinism moat (the only parallel step is the operator's
// spmv, bit-exact across threads).
//
// PRECONDITIONING: optional LEFT preconditioner Pl = m_inv (applied to V/R as
// M⁻¹). Unpreconditioned (m_inv == nullptr) skips the application. The omega step
// uses the van Gijzen ρ-safeguard (angle = √2/2) to avoid stagnation. Recurrence
// transcribed VERBATIM from IterativeSolvers.jl `idrs.jl`.
// -----------------------------------------------------------------------

namespace detail
{
// Forward-substitution solve of the lower-triangular trailing block
// M[k:s, k:s] · c = f[k:s], with M row-major s×s. Writes c[0 .. (s-k)-1] (local
// indexing: c[j] pairs with G[k+j]). Standalone + tested for the off-by-one.
template <typename T>
inline void idrs_ltri_solve(const T* m, crd::usize s, crd::usize k, const T* f, T* c) noexcept
{
    const crd::usize mm = s - k;
    for (crd::usize r = 0; r < mm; ++r)
    {
        T acc = f[k + r];
        for (crd::usize cc = 0; cc < r; ++cc)
        {
            acc = acc - m[(k + r) * s + (k + cc)] * c[cc];
        }
        c[r] = acc / m[(k + r) * s + (k + r)];
    }
}
} // namespace detail

template <typename T>
struct IdrsWorkspace
{
    using R = crd::hesap::dense::RealType<T>;

    crd::usize                   s; // shadow-space dimension
    crd::usize                   n; // problem size
    crd::hesap::dense::Vector<T> p_all, g_all, u_all; // s·n each (column-major-of-vectors via offset)
    crd::hesap::dense::Vector<T> r, v, q, pv;          // n each (residual, work, A·V, precond scratch)
    crd::hesap::dense::Vector<T> mmat;                 // s·s row-major reduced matrix M
    crd::hesap::dense::Vector<T> f, c;                 // s each

    IdrsWorkspace(crd::memory::IAllocator* alloc, crd::usize n_, crd::usize s_ = 4,
                  crd::u64 seed = 0x9E3779B97F4A7C15ULL)
        : s(s_), n(n_), p_all(alloc, s_ * n_), g_all(alloc, s_ * n_), u_all(alloc, s_ * n_), r(alloc, n_), v(alloc, n_),
          q(alloc, n_), pv(alloc, n_), mmat(alloc, s_ * s_), f(alloc, s_), c(alloc, s_)
    {
        CRD_ASSERT_MSG(s_ >= 1, "IdrsWorkspace: s must be >= 1");
        generate_shadow(seed);
    }

    [[nodiscard]] crd::usize size() const noexcept { return n; }
    [[nodiscard]] crd::containers::Span<T> p_col(crd::usize i) noexcept { return {p_all.data() + i * n, n}; }
    [[nodiscard]] crd::containers::Span<T> g_col(crd::usize i) noexcept { return {g_all.data() + i * n, n}; }
    [[nodiscard]] crd::containers::Span<T> u_col(crd::usize i) noexcept { return {u_all.data() + i * n, n}; }

private:
    // Deterministic seeded shadow space, MGS-orthonormalized. Same seed + n + s ⇒
    // identical P, so any two solves are bit-comparable.
    void generate_shadow(crd::u64 seed) noexcept
    {
        using namespace crd::hesap::dense;
        crd::u64 state = seed;
        auto     next  = [&state]() -> R {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<R>(static_cast<crd::f64>(state >> 11) / static_cast<crd::f64>(1ULL << 53) - 0.5);
        };
        for (crd::usize i = 0; i < s; ++i)
        {
            auto pi = p_col(i);
            for (crd::usize r2 = 0; r2 < n; ++r2)
            {
                if constexpr (is_complex_v<T>)
                {
                    pi[r2] = T{next(), next()};
                }
                else
                {
                    pi[r2] = next();
                }
            }
            // MGS against the already-orthonormal columns.
            for (crd::usize j = 0; j < i; ++j)
            {
                auto    pj  = p_col(j);
                const T dij = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{pj.data(), n},
                                                      crd::containers::ConstSpan<T>{pi.data(), n}); // pjᴴ pi
                dense::axpy<T>(-dij, crd::containers::ConstSpan<T>{pj.data(), n}, pi);
            }
            const R nrm = nrm2<T>(crd::containers::ConstSpan<T>{pi.data(), n});
            if (nrm > R(0))
            {
                dense::scal<T>(T(R(1) / nrm), pi);
            }
        }
    }
};

// IDR(s) with optional LEFT preconditioner Pl = m_inv (nullptr ⇒ plain IDR(s)).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> idrs(const crd::hesap::LinearOp<T>&  a,
                                                     const crd::hesap::LinearOp<T>*  m_inv,
                                                     crd::containers::ConstSpan<T>   b,
                                                     crd::containers::Span<T>        x,
                                                     const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                     IdrsWorkspace<T>&               ws,
                                                     crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const crd::usize   s      = ws.s;
    const bool         prec   = (m_inv != nullptr);
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "idrs: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "idrs: span/workspace size mismatch");

    const auto r  = ws.r.span();
    const auto vv = ws.v.span();
    const auto qq = ws.q.span();
    const auto pv = ws.pv.span();
    T*         m  = ws.mmat.data(); // s·s row-major
    T*         f  = ws.f.data();
    T*         c  = ws.c.data();

    auto precond_inplace = [&](crd::containers::Span<T> z) {
        if (prec)
        {
            (void)m_inv->apply(crd::containers::ConstSpan<T>{z.data(), z.size()}, pv);
            dense::copy<T>(crd::containers::ConstSpan<T>{pv.data(), pv.size()}, z);
        }
    };

    // r = b - A·x ; G = U = 0 ; M = I ; omega = 1.
    (void)a.apply(x, qq);
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i] = b[i] - qq[i];
    }
    for (crd::usize i = 0; i < s; ++i)
    {
        for (crd::usize jj = 0; jj < n; ++jj)
        {
            ws.g_col(i)[jj] = T{};
            ws.u_col(i)[jj] = T{};
        }
        for (crd::usize j = 0; j < s; ++j)
        {
            m[i * s + j] = (i == j) ? T(1) : T{};
        }
    }
    T omega = T(1);

    const R res0 = nrm2<T>(r);
    R       res  = res0;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(res0);
    }
    if (is_converged<R>(res, res0, opts) || n == 0)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = res;
        return result;
    }

    crd::usize iter    = 0;
    bool       stop    = false;
    auto       finish  = [&](StopReason why) {
        result.reason              = why;
        result.final_residual_norm = res;
        result.iterations          = iter;
        if (why == StopReason::Converged)
        {
            result.converged = true;
        }
        stop = true;
    };

    while (!stop && iter < opts.max_iter)
    {
        // f[i] = ⟨P_i, r⟩ for the whole cycle.
        for (crd::usize i = 0; i < s; ++i)
        {
            f[i] = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ws.p_col(i).data(), n},
                                           crd::containers::ConstSpan<T>{r.data(), n});
        }

        for (crd::usize k = 0; k < s && !stop; ++k)
        {
            // Solve the lower-tri trailing block M[k:s,k:s]·c = f[k:s].
            detail::idrs_ltri_solve<T>(m, s, k, f, c);

            // V = r − Σ_{j} c[j]·G[k+j] ; Q = Σ_{j} c[j]·U[k+j].
            const crd::usize mm = s - k;
            for (crd::usize jj = 0; jj < n; ++jj)
            {
                vv[jj] = T{};
                qq[jj] = T{};
            }
            for (crd::usize j = 0; j < mm; ++j)
            {
                dense::axpy<T>(c[j], crd::containers::ConstSpan<T>{ws.g_col(k + j).data(), n}, vv);
                dense::axpy<T>(c[j], crd::containers::ConstSpan<T>{ws.u_col(k + j).data(), n}, qq);
            }
            for (crd::usize jj = 0; jj < n; ++jj)
            {
                vv[jj] = r[jj] - vv[jj];
            }
            precond_inplace(vv); // V = M⁻¹ V

            // U[k] = Q + omega·V ; G[k] = A·U[k].
            auto uk = ws.u_col(k);
            auto gk = ws.g_col(k);
            for (crd::usize jj = 0; jj < n; ++jj)
            {
                uk[jj] = qq[jj] + omega * vv[jj];
            }
            (void)a.apply(crd::containers::ConstSpan<T>{uk.data(), n}, gk);
            ++iter;

            // Bi-orthogonalize G[k], U[k] against P[0..k-1].
            for (crd::usize i = 0; i < k; ++i)
            {
                const T mii = m[i * s + i];
                const T alpha =
                    detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ws.p_col(i).data(), n},
                                            crd::containers::ConstSpan<T>{gk.data(), n})
                    / mii;
                dense::axpy<T>(-alpha, crd::containers::ConstSpan<T>{ws.g_col(i).data(), n}, gk);
                dense::axpy<T>(-alpha, crd::containers::ConstSpan<T>{ws.u_col(i).data(), n}, uk);
            }

            // Update column k of M: M[i,k] = ⟨P_i, G[k]⟩ for i = k..s-1.
            for (crd::usize i = k; i < s; ++i)
            {
                m[i * s + k] = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{ws.p_col(i).data(), n},
                                                       crd::containers::ConstSpan<T>{gk.data(), n});
            }

            // beta = f[k]/M[k,k] ; r −= beta·G[k] ; x += beta·U[k].
            if (detail::krylov_mag<T>(m[k * s + k]) < smlnum)
            {
                finish(StopReason::Breakdown); // shadow space lost rank
                break;
            }
            const T beta = f[k] / m[k * s + k];
            dense::axpy<T>(-beta, crd::containers::ConstSpan<T>{gk.data(), n}, r);
            dense::axpy<T>(beta, crd::containers::ConstSpan<T>{uk.data(), n}, x);

            res = nrm2<T>(r);
            if (opts.record_residuals)
            {
                result.residual_history.push_back(res);
            }
            result.iterations = iter;
            if (is_converged<R>(res, res0, opts))
            {
                finish(StopReason::Converged);
                break;
            }
            if (iter >= opts.max_iter)
            {
                finish(StopReason::MaxIterations);
                break;
            }

            // f[k+1:s] −= beta·M[k+1:s, k].
            if (k + 1 < s)
            {
                for (crd::usize i = k + 1; i < s; ++i)
                {
                    f[i] = f[i] - beta * m[i * s + k];
                }
            }
        }
        if (stop)
        {
            break;
        }

        // omega step: V = M⁻¹ r ; Q = A·V ; omega = ρ-safeguarded ⟨Q,r⟩/⟨Q,Q⟩.
        dense::copy<T>(crd::containers::ConstSpan<T>{r.data(), n}, vv);
        precond_inplace(vv);
        (void)a.apply(crd::containers::ConstSpan<T>{vv.data(), n}, qq);
        ++iter;
        {
            const T ts = detail::krylov_inner<T>(crd::containers::ConstSpan<T>{qq.data(), n},
                                                 crd::containers::ConstSpan<T>{r.data(), n}); // ⟨Q,r⟩
            const R nt = nrm2<T>(qq);
            const R ns = nrm2<T>(r);
            const R tt = nt * nt;
            if (tt < smlnum)
            {
                finish(StopReason::Breakdown);
                break;
            }
            omega                 = ts / T(tt);                              // ⟨Q,r⟩/‖Q‖²
            const R angle         = static_cast<R>(0.7071067811865476);      // √2/2
            const R denom         = nt * ns;
            if (denom > R(0))
            {
                const R rho = detail::krylov_mag<T>(ts) / denom;
                if (rho < angle && rho > R(0))
                {
                    omega = omega * T(angle / rho);
                }
            }
        }
        dense::axpy<T>(-omega, crd::containers::ConstSpan<T>{qq.data(), n}, r);
        dense::axpy<T>(omega, crd::containers::ConstSpan<T>{vv.data(), n}, x);

        res = nrm2<T>(r);
        if (opts.record_residuals)
        {
            result.residual_history.push_back(res);
        }
        result.iterations = iter;
        if (is_converged<R>(res, res0, opts))
        {
            finish(StopReason::Converged);
            break;
        }
        if (iter >= opts.max_iter)
        {
            finish(StopReason::MaxIterations);
            break;
        }
    }

    if (!stop)
    {
        result.reason              = StopReason::MaxIterations;
        result.final_residual_norm = res;
        result.iterations          = iter;
    }
    return result;
}

// Plain (unpreconditioned) IDR(s) convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> idrs(const crd::hesap::LinearOp<T>&                          a,
                                                     crd::containers::ConstSpan<T>                          b,
                                                     crd::containers::Span<T>                               x,
                                                     const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                     IdrsWorkspace<T>&                                      ws,
                                                     crd::memory::IAllocator* result_alloc)
{
    return idrs<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
