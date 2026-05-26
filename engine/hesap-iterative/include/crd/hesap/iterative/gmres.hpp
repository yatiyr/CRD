#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// FGMRES(m) -- flexible, restarted Generalized Minimal RESidual. Phase 3.1.6 v4b.
//
// FLEXIBLE from the start (Saad 1993): the preconditioner action z_j = M_j⁻¹ v_j
// may CHANGE every iteration (M passed as a LinearOp; nested-Krylov / AMG land as
// M later). The solution update is built from the PRECONDITIONED vectors Z, not V
// -- the defining difference from right-preconditioned GMRES. m = restart length.
// `M_inv == nullptr` ⇒ plain (restarted) GMRES.
//
// Works for GENERAL (nonsymmetric / non-HPD) A -- the regime CG cannot touch.
// Real + complex (Hermitian inner product `dotc` for complex). Determinism: the
// Arnoldi (modified Gram-Schmidt), Givens triangularization, and least-squares
// back-substitution all run SERIALLY on the calling thread; the only parallel
// step is the operator's spmv (bit-exact across threads), so the whole solve is
// thread-count independent (the v4 determinism moat), gated like CG.
// -----------------------------------------------------------------------

namespace detail
{
// -conj(s): for real s this is -s; for complex (re,im) it is (-re, im).
template <typename T>
[[nodiscard]] inline T gmres_neg_conj(T s) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return T{-s.re, s.im};
    }
    else
    {
        return -s;
    }
}

// (magnitude |v| -> R: use the shared detail::krylov_mag from cg.hpp.)
// (GivensRot<T> is the shared plane-rotation result from cg.hpp detail.)

// Plane rotation G = [c, s; -conj(s), c] with c real, c²+|s|²=1, G·[a;b]=[r;0].
// Real path: scaled sqrt (overflow-safe). Complex path mirrors LAPACK zlartg.
template <typename T>
[[nodiscard]] inline GivensRot<T> gmres_givens(T a, T b) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        if (b.re == R{0} && b.im == R{0})
        {
            return GivensRot<T>{R{1}, T{}, a};
        }
        const R bn = crd::hesap::abs(b);
        if (a.re == R{0} && a.im == R{0})
        {
            return GivensRot<T>{R{0}, crd::hesap::conj(b) * (R{1} / bn), T{bn, R{0}}};
        }
        const R an = crd::hesap::abs(a);
        const R d  = crd::hesap::abs(T{an, bn}); // sqrt(an²+bn²)
        const T fs = a * (R{1} / an);            // unit phase of a
        return GivensRot<T>{an / d, (fs * crd::hesap::conj(b)) * (R{1} / d), fs * T{d, R{0}}};
    }
    else
    {
        const R aa    = a < R{0} ? -a : a;
        const R ab    = b < R{0} ? -b : b;
        const R scale = aa > ab ? aa : ab;
        if (scale == R{0})
        {
            return GivensRot<T>{R{1}, R{0}, R{0}};
        }
        const R ra = a / scale;
        const R rb = b / scale;
        const R t  = scale * std::sqrt(ra * ra + rb * rb);
        return GivensRot<T>{a / t, b / t, t};
    }
}

// Apply G (c,s) to the pair (x,y): x ← c·x + s·y ; y ← -conj(s)·x + c·y.
template <typename T>
inline void gmres_rot_apply(T& x, T& y, crd::hesap::dense::RealType<T> c, T s) noexcept
{
    const T nx = c * x + s * y;
    const T ny = gmres_neg_conj<T>(s) * x + c * y;
    x          = nx;
    y          = ny;
}
} // namespace detail

// Pre-allocated workspace for one FGMRES(m) solve. Flat basis buffers (V: m+1
// vectors, Z: m preconditioned vectors) + small host-side Hessenberg/Givens/RHS.
template <typename T>
struct GmresWorkspace
{
    using R = crd::hesap::dense::RealType<T>;

    crd::usize                   n;
    crd::usize                   m; // restart length
    crd::hesap::dense::Vector<T> vbuf; // (m+1)*n  -- orthonormal Arnoldi basis
    crd::hesap::dense::Vector<T> zbuf; // m*n      -- preconditioned vectors (flexible update)
    crd::hesap::dense::Vector<T> w;    // n        -- Arnoldi work vector
    crd::hesap::dense::Vector<T> rvec; // n        -- residual
    crd::containers::Array<T>    rmat; // m*m upper-triangular R (row-major)
    crd::containers::Array<T>    hcol; // m+1 working Hessenberg column
    crd::containers::Array<R>    cs;   // m Givens cosines
    crd::containers::Array<T>    sn;   // m Givens sines
    crd::containers::Array<T>    g;    // m+1 rotated RHS
    crd::containers::Array<T>    y;    // m least-squares solution

    GmresWorkspace(crd::memory::IAllocator* alloc, crd::usize size, crd::usize restart)
        : n(size)
        , m(restart)
        , vbuf(alloc, (restart + 1) * size)
        , zbuf(alloc, restart * size)
        , w(alloc, size)
        , rvec(alloc, size)
        , rmat(alloc)
        , hcol(alloc)
        , cs(alloc)
        , sn(alloc)
        , g(alloc)
        , y(alloc)
    {
        rmat.resize(restart * restart);
        hcol.resize(restart + 1);
        cs.resize(restart);
        sn.resize(restart);
        g.resize(restart + 1);
        y.resize(restart);
    }

    [[nodiscard]] crd::containers::Span<T> v(crd::usize j) noexcept
    {
        return crd::containers::Span<T>{vbuf.data() + j * n, n};
    }
    [[nodiscard]] crd::containers::Span<T> z(crd::usize j) noexcept
    {
        return crd::containers::Span<T>{zbuf.data() + j * n, n};
    }
};

template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> fgmres(const crd::hesap::LinearOp<T>&  a,
                                                       const crd::hesap::LinearOp<T>*  m_inv,
                                                       crd::containers::ConstSpan<T>   b,
                                                       crd::containers::Span<T>        x,
                                                       const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                       GmresWorkspace<T>&              ws,
                                                       crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const crd::usize   n = a.n_rows();
    const crd::usize   m = ws.m;
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "fgmres: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.n == n, "fgmres: span/workspace size mismatch");
    CRD_ASSERT_MSG(m >= 1, "fgmres: restart length must be >= 1");

    const auto rv = ws.rvec.span();
    const auto wv = ws.w.span();

    // r = b - A·x
    (void)a.apply(x, wv);
    for (crd::usize i = 0; i < n; ++i)
    {
        rv[i] = b[i] - wv[i];
    }
    R res0 = nrm2<T>(rv);
    R res  = res0;
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

    while (result.iterations < opts.max_iter)
    {
        // ---- start a restart cycle: v_0 = r / ‖r‖ ----
        const R beta = nrm2<T>(rv);
        {
            const auto v0 = ws.v(0);
            const T    inv = T(1) / T(beta);
            for (crd::usize i = 0; i < n; ++i)
            {
                v0[i] = rv[i] * inv;
            }
        }
        for (crd::usize i = 0; i <= m; ++i)
        {
            ws.g[i] = T{};
        }
        ws.g[0] = T(beta);

        crd::usize k = 0; // columns built this cycle
        for (crd::usize j = 0; j < m && result.iterations < opts.max_iter; ++j)
        {
            // z_j = M⁻¹ v_j (flexible); plain GMRES ⇒ z_j = v_j.
            const auto zj = ws.z(j);
            if (m_inv != nullptr)
            {
                (void)m_inv->apply(ws.v(j), zj);
            }
            else
            {
                dense::copy<T>(ws.v(j), zj);
            }
            // w = A z_j
            (void)a.apply(zj, wv);

            // Modified Gram-Schmidt against v_0..v_j.
            for (crd::usize i = 0; i <= j; ++i)
            {
                const T hij = detail::krylov_inner<T>(ws.v(i), wv); // <v_i, w>
                ws.hcol[i]  = hij;
                dense::axpy<T>(-hij, ws.v(i), wv); // w -= hij v_i
            }
            const R hnext = nrm2<T>(wv);
            ws.hcol[j + 1] = T(hnext);

            const bool lucky = (hnext == R{0});
            if (!lucky)
            {
                const auto vn  = ws.v(j + 1);
                const T    inv = T(1) / T(hnext);
                for (crd::usize i = 0; i < n; ++i)
                {
                    vn[i] = wv[i] * inv;
                }
            }

            // Apply previous Givens to the new Hessenberg column.
            for (crd::usize i = 0; i + 1 <= j; ++i)
            {
                detail::gmres_rot_apply<T>(ws.hcol[i], ws.hcol[i + 1], ws.cs[i], ws.sn[i]);
            }
            // New Givens zeroing hcol[j+1].
            const auto rot = detail::gmres_givens<T>(ws.hcol[j], ws.hcol[j + 1]);
            ws.cs[j]       = rot.c;
            ws.sn[j]       = rot.s;
            ws.hcol[j]     = rot.r;
            ws.hcol[j + 1] = T{};
            for (crd::usize i = 0; i <= j; ++i) // store R column j (rows 0..j)
            {
                ws.rmat[i * m + j] = ws.hcol[i];
            }
            // Apply Givens to g: g[j+1] = -conj(s)·g[j]; g[j] = c·g[j].
            const T gj  = ws.g[j];
            ws.g[j]     = ws.cs[j] * gj;
            ws.g[j + 1] = detail::gmres_neg_conj<T>(ws.sn[j]) * gj;

            res = detail::krylov_mag<T>(ws.g[j + 1]);
            ++result.iterations;
            ++k;
            if (opts.record_residuals)
            {
                result.residual_history.push_back(res);
            }
            if (is_converged<R>(res, res0, opts) || lucky)
            {
                ++j; // include this column in the solve
                k = j;
                break;
            }
        }

        // Back-substitution: solve R y = g (k×k upper-triangular).
        for (crd::usize ii = 0; ii < k; ++ii)
        {
            const crd::usize i = k - 1 - ii;
            T                acc = ws.g[i];
            for (crd::usize jj = i + 1; jj < k; ++jj)
            {
                acc = acc - ws.rmat[i * m + jj] * ws.y[jj];
            }
            ws.y[i] = acc / ws.rmat[i * m + i];
        }
        // x += sum_{j<k} y_j z_j   (flexible: preconditioned vectors).
        for (crd::usize j = 0; j < k; ++j)
        {
            dense::axpy<T>(ws.y[j], ws.z(j), x);
        }

        // Recompute the true residual for the next cycle / convergence test.
        (void)a.apply(x, wv);
        for (crd::usize i = 0; i < n; ++i)
        {
            rv[i] = b[i] - wv[i];
        }
        res = nrm2<T>(rv);
        if (is_converged<R>(res, res0, opts))
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}

// Plain restarted GMRES(m) (no preconditioner).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> gmres(const crd::hesap::LinearOp<T>&                          a,
                                                      crd::containers::ConstSpan<T>                          b,
                                                      crd::containers::Span<T>                               x,
                                                      const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                      GmresWorkspace<T>&                                     ws,
                                                      crd::memory::IAllocator* result_alloc)
{
    return fgmres<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
