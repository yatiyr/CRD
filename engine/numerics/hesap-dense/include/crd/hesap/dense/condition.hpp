#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/cholesky.hpp>
#include <crd/hesap/dense/linear_op_dense.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v0e-e — Hager 1-norm condition estimator.
//
// Estimates κ_1(A) = ||A||_1 · ||A^{-1}||_1 without forming A^{-1}.
// Uses Hager's algorithm (LAPACK xLACON / xLACN2 pattern): power
// iteration that requires only `solve(A, x)` and `solve(A^T, x)`
// applications. Cost = ~5 solver calls.
//
// For symmetric A factored via Cholesky: solve(A, x) == solve(A^T, x)
// (A is symmetric, so A^{-T} = A^{-1}). This is the simple case shipped
// in v0e-e-MVP.
//
// Filed for v0e-e2: LU + LDLT + QR condition estimators (need
// solve_transpose paths).
// -----------------------------------------------------------------------

namespace detail
{
template <typename T>
inline T abs_v(T x) noexcept
{
    return x < T{0} ? -x : x;
}
} // namespace detail

// Hager 1-norm estimate of ||op||_1 given a callable `apply_op` that
// computes y := op · x in-place (i.e., overwrites x with op·x).
// `apply_op_transpose` computes x := op^T · x in-place.
//
// `work` and `sign_buf` must each be n-sized scratch arrays. For
// symmetric op (A == A^T), `apply_op_transpose` and `apply_op` are the
// same closure.
//
// Returns the estimate γ. Worst case: γ ≤ ||op||_1 ≤ n · γ.
template <typename T, typename ApplyOp, typename ApplyOpTranspose>
[[nodiscard]] T hager_1norm_estimate(crd::usize n, ApplyOp apply_op,
                                      ApplyOpTranspose apply_op_transpose,
                                      crd::memory::IAllocator* alloc,
                                      crd::u32 max_iter = 5)
{
    if (n == 0)
    {
        return T{0};
    }
    crd::containers::Array<T> v(alloc);
    v.resize(n);
    // Initial guess: v = (1/n, 1/n, ..., 1/n).
    const T one_over_n = T{1} / static_cast<T>(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i] = one_over_n;
    }

    T gamma = T{0};
    for (crd::u32 iter = 0; iter < max_iter; ++iter)
    {
        // Step 1: v := op · v.
        crd::containers::Span<T> vs(v.data(), n);
        apply_op(vs);

        // γ_new = ||v||_1
        T gamma_new = T{0};
        for (crd::usize i = 0; i < n; ++i)
        {
            gamma_new += detail::abs_v<T>(v[i]);
        }

        if (iter > 0 && gamma_new <= gamma)
        {
            return gamma;
        }
        gamma = gamma_new;

        // Step 2: build sign vector ξ_i = sign(v_i) and apply op^T.
        for (crd::usize i = 0; i < n; ++i)
        {
            v[i] = v[i] >= T{0} ? T{1} : T{-1};
        }
        apply_op_transpose(vs);

        // Step 3: find j = argmax |v_i|. If max is small, converged.
        T max_abs = detail::abs_v<T>(v[0]);
        crd::usize jmax = 0;
        for (crd::usize i = 1; i < n; ++i)
        {
            const T va = detail::abs_v<T>(v[i]);
            if (va > max_abs)
            {
                max_abs = va;
                jmax = i;
            }
        }
        // Restart v as unit vector in direction jmax.
        for (crd::usize i = 0; i < n; ++i)
        {
            v[i] = T{0};
        }
        v[jmax] = T{1};
    }
    return gamma;
}

// Condition number κ_1(A) for symmetric A with Cholesky factor.
// Returns ||A||_1 * ||A^{-1}||_1_estimate.
//
// `a` is the original symmetric matrix; `chol` is its factored form.
template <typename T, Layout L>
[[nodiscard]] T condition_estimate_1norm_symmetric(const Symmetric<T>& a,
                                                    const Cholesky<T, L>& chol,
                                                    crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(a.n() == chol.n(), "condition_estimate_1norm: size mismatch");
    const T norm_a = compute_1norm(a);
    const crd::usize n = a.n();

    // Symmetric A → solve == solve_transpose. Closures capture chol by ref.
    auto solve_closure = [&](crd::containers::Span<T> x)
    {
        solve_cholesky(chol, x);
    };
    auto solve_transpose_closure = [&](crd::containers::Span<T> x)
    {
        solve_cholesky(chol, x);
    };

    const T norm_ainv =
        hager_1norm_estimate<T>(n, solve_closure, solve_transpose_closure, alloc);
    return norm_a * norm_ainv;
}

} // namespace crd::hesap::dense
