#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas2.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/linear_op_dense.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v0e-f — iterative refinement for direct solvers.
//
// Algorithm (Wilkinson 1948):
//   Given factored A, RHS b, and initial x_0 := solve(b):
//     for k = 0, 1, ...:
//       r_k = b - A · x_k
//       dx  = solve(r_k)
//       x_{k+1} = x_k + dx
//       if ||r_k|| / ||b|| < tol: break
//
// Drives the iterate to backward-stable accuracy in a small number of
// steps when the factor is "good enough" (e.g., LU with partial
// pivoting and well-conditioned A). Single-precision factor + double-
// precision refine (HPL-AI / mixed-precision) is the canonical
// 3-4× speedup pattern — filed as `v0e-f2` (needs cross-precision
// solve wrapper).
//
// f32 + f64 RowMajor; LU + Cholesky variants. Returns final
// `||r||_2 / ||b||_2` relative residual + iteration count via the
// `RefinementResult` struct.
// -----------------------------------------------------------------------

template <typename T>
struct RefinementResult
{
    crd::u32 iterations = 0;
    T initial_rel_residual = T{0};
    T final_rel_residual = T{0};
    bool converged = false;
};

namespace detail
{
template <typename T>
inline T two_norm(crd::containers::ConstSpan<T> x) noexcept
{
    T s = T{0};
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        s += x[i] * x[i];
    }
    return std::sqrt(s);
}
} // namespace detail

// =======================================================================
// refine_lu — iterative refinement on an LU factor.
//
// `a` is the ORIGINAL matrix (needed to compute the residual r = b - A·x);
// `lu` is its factor; `b` is the RHS; `x` carries the initial guess on
// entry (caller solved it once already) and the refined solution on exit.
// =======================================================================

template <typename T, Layout L>
RefinementResult<T> refine_lu(
    const Matrix<T, L>& a,
    const LU<T, L>& lu,
    crd::containers::ConstSpan<T> b,
    crd::containers::Span<T> x,
    crd::u32 max_iter = 5,
    T tol = T{1e-13})
{
    const crd::usize n = a.rows();
    CRD_ASSERT_MSG(a.is_square() && lu.n() == n && b.size() == n && x.size() == n,
                   "refine_lu: size mismatch");

    RefinementResult<T> result{};
    crd::memory::IAllocator* alloc = a.allocator();
    crd::containers::Array<T> r(alloc);
    r.resize(n);
    crd::containers::Array<T> dx(alloc);
    dx.resize(n);

    const T norm_b = detail::two_norm<T>(b);
    if (norm_b == T{0})
    {
        // Trivial RHS — no work to do.
        result.converged = true;
        return result;
    }

    // Initial residual r_0 = b - A · x_0.
    MatrixLinearOp<T, L> op(a);
    (void)op.apply(crd::containers::ConstSpan<T>(x.data(), n),
                    crd::containers::Span<T>(r.data(), n));
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i] = b[i] - r[i];
    }
    const T initial_residual = detail::two_norm<T>(crd::containers::ConstSpan<T>(r.data(), n));
    result.initial_rel_residual = initial_residual / norm_b;
    result.final_rel_residual = result.initial_rel_residual;

    if (result.initial_rel_residual < tol)
    {
        result.converged = true;
        return result;
    }

    for (crd::u32 it = 0; it < max_iter; ++it)
    {
        // dx := solve(r).
        for (crd::usize i = 0; i < n; ++i)
        {
            dx[i] = r[i];
        }
        solve_lu(lu, crd::containers::Span<T>(dx.data(), n));

        // x := x + dx.
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x[i] + dx[i];
        }

        // r := b - A · x.
        (void)op.apply(crd::containers::ConstSpan<T>(x.data(), n),
                        crd::containers::Span<T>(r.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            r[i] = b[i] - r[i];
        }
        const T rel_res =
            detail::two_norm<T>(crd::containers::ConstSpan<T>(r.data(), n)) / norm_b;
        result.iterations = it + 1;
        result.final_rel_residual = rel_res;

        if (rel_res < tol)
        {
            result.converged = true;
            return result;
        }
    }
    return result;
}

// =======================================================================
// refine_cholesky — iterative refinement on a Cholesky factor.
// `a` is the original Symmetric; `chol` is its factor.
// =======================================================================

template <typename T, Layout L>
RefinementResult<T> refine_cholesky(
    const Symmetric<T>& a,
    const Cholesky<T, L>& chol,
    crd::containers::ConstSpan<T> b,
    crd::containers::Span<T> x,
    crd::u32 max_iter = 5,
    T tol = T{1e-13})
{
    const crd::usize n = a.n();
    CRD_ASSERT_MSG(chol.n() == n && b.size() == n && x.size() == n,
                   "refine_cholesky: size mismatch");

    RefinementResult<T> result{};
    crd::memory::IAllocator* alloc = chol.allocator();
    crd::containers::Array<T> r(alloc);
    r.resize(n);
    crd::containers::Array<T> dx(alloc);
    dx.resize(n);

    const T norm_b = detail::two_norm<T>(b);
    if (norm_b == T{0})
    {
        result.converged = true;
        return result;
    }

    SymmetricLinearOp<T> op(a);
    (void)op.apply(crd::containers::ConstSpan<T>(x.data(), n),
                    crd::containers::Span<T>(r.data(), n));
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i] = b[i] - r[i];
    }
    const T initial_residual = detail::two_norm<T>(crd::containers::ConstSpan<T>(r.data(), n));
    result.initial_rel_residual = initial_residual / norm_b;
    result.final_rel_residual = result.initial_rel_residual;

    if (result.initial_rel_residual < tol)
    {
        result.converged = true;
        return result;
    }

    for (crd::u32 it = 0; it < max_iter; ++it)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            dx[i] = r[i];
        }
        solve_cholesky(chol, crd::containers::Span<T>(dx.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            x[i] = x[i] + dx[i];
        }
        (void)op.apply(crd::containers::ConstSpan<T>(x.data(), n),
                        crd::containers::Span<T>(r.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            r[i] = b[i] - r[i];
        }
        const T rel_res =
            detail::two_norm<T>(crd::containers::ConstSpan<T>(r.data(), n)) / norm_b;
        result.iterations = it + 1;
        result.final_rel_residual = rel_res;

        if (rel_res < tol)
        {
            result.converged = true;
            return result;
        }
    }
    return result;
}

} // namespace crd::hesap::dense
