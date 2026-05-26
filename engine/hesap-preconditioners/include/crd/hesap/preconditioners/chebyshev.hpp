#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// ChebyshevPreconditioner<T> -- polynomial (Chebyshev) preconditioner. Phase 3.1.6 v4i-2.
//
// M⁻¹ ≈ p_{deg}(A), the Chebyshev polynomial that approximates A⁻¹ on the spectral
// interval [λmin, λmax] of an SPD/HPD A. The action z = M⁻¹·r is the `deg`-term
// Chebyshev iteration (textbook three-term recurrence): `deg-1` MATRIX-FREE spmv +
// axpy/scal -- NO factorization, NO triangular solve. This is the GPU-mappable,
// perfectly-parallel, determinism-friendly preconditioner (every op is a parallel
// SELL spmv or a KBN-deterministic blas1 reduction), and it is the smoother AMG
// reuses (v4k). M⁻¹ = p(A) is symmetric for SPD A (real polynomial of a symmetric
// matrix) ⇒ a valid CG/MINRES/SYMMLQ preconditioner; apply_adjoint == apply.
//
// Spectral bounds: λmax is estimated by a deterministic power iteration (fixed
// NON-uniform seed -- a uniform vector lies in the near-nullspace of Laplacian-like
// operators and collapses the estimate -- + fixed iteration count, Rayleigh
// quotient). hi = 1.05·λmax (safety); lo = hi·lo_ratio. The caller may override
// either bound. lo_ratio brackets the low end of the spectrum: too large under-
// preconditions the low modes, too small wastes the polynomial budget -- pinned
// empirically (NOT Saad's AMG-smoother ρ≈30, which deliberately misses the low end).
//
// Determinism: the power iteration (fixed seed/iters, sequential Rayleigh dot, KBN
// nrm2) and the apply (parallel SELL spmv bit-exact across threads + KBN blas1) are
// thread-count-independent ⇒ the moat holds. Real + complex/HPD.
// -----------------------------------------------------------------------

template <typename T>
class ChebyshevPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    ChebyshevPreconditioner(const Csr& a, crd::memory::IAllocator* alloc, crd::u32 degree = 4,
                            R lo_ratio = R(1) / R(30), R lambda_max_override = R(-1),
                            R lambda_min_override = R(-1))
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_op(a, alloc)
        , m_x(alloc), m_res(alloc), m_dx(alloc), m_tmp(alloc)
        , m_n(a.rows())
        , m_deg(degree < 1 ? 1 : degree)
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "ChebyshevPreconditioner: matrix must be square");
        m_x.resize(m_n);
        m_res.resize(m_n);
        m_dx.resize(m_n);
        m_tmp.resize(m_n);

        const R lam_max = (lambda_max_override > R(0)) ? lambda_max_override : estimate_lambda_max(alloc);
        m_hi = (lambda_max_override > R(0)) ? lambda_max_override : R(1.05) * lam_max;
        m_lo = (lambda_min_override > R(0)) ? lambda_min_override : m_hi * lo_ratio;
        if (m_lo <= R(0)) { m_lo = m_hi * (R(1) / R(30)); }
        if (m_hi <= m_lo) { m_hi = m_lo * R(2); } // degenerate spectrum guard
    }

    // z = p_{deg}(A)·r ≈ A⁻¹·r (Chebyshev iteration over [lo, hi]; deg-1 spmv).
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        const R theta = (m_hi + m_lo) / R(2);
        const R delta = (m_hi - m_lo) / R(2);
        const R sigma = theta / delta;

        const auto xsp  = crd::containers::Span<T>{m_x.data(), m_n};
        const auto ressp = crd::containers::Span<T>{m_res.data(), m_n};
        const auto dxsp = crd::containers::Span<T>{m_dx.data(), m_n};
        const auto tmpsp = crd::containers::Span<T>{m_tmp.data(), m_n};

        // x_1 = (1/theta)·r   (x_0 = 0 ⇒ residual r_0 = r)
        for (crd::u32 i = 0; i < m_n; ++i) { m_dx[i] = r[i] * T(R(1) / theta); m_x[i] = m_dx[i]; }
        R rho = R(1) / sigma;

        for (crd::u32 k = 1; k < m_deg; ++k)
        {
            // res = r − A·x
            (void)m_op.apply(crd::containers::ConstSpan<T>{m_x.data(), m_n}, tmpsp);
            for (crd::u32 i = 0; i < m_n; ++i) { m_res[i] = r[i] - m_tmp[i]; }

            const R rho_new = R(1) / (R(2) * sigma - rho);
            // dx = (rho·rho_new)·dx + (2·rho_new/delta)·res
            const T c1 = T(rho * rho_new);
            const T c2 = T(R(2) * rho_new / delta);
            for (crd::u32 i = 0; i < m_n; ++i) { m_dx[i] = c1 * m_dx[i] + c2 * m_res[i]; }
            for (crd::u32 i = 0; i < m_n; ++i) { m_x[i] = m_x[i] + m_dx[i]; }
            rho = rho_new;
        }
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = m_x[i]; }
        return true;
    }

    // M⁻¹ = p(A) with real coefficients; for SPD/HPD A (A = Aᴴ) ⇒ Mᴴ = M.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        return apply(r, z);
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    [[nodiscard]] R lambda_max() const noexcept { return m_hi; }
    [[nodiscard]] R lambda_min() const noexcept { return m_lo; }

private:
    [[nodiscard]] static T cheb_conj(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
        else { return v; }
    }
    [[nodiscard]] static R cheb_real(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return v.re; }
        else { return v; }
    }

    // Deterministic power iteration for λmax (fixed non-uniform seed + fixed iters, Rayleigh
    // quotient). Non-uniform seed avoids the constant-nullspace collapse on Laplacian-like A.
    [[nodiscard]] R estimate_lambda_max(crd::memory::IAllocator* alloc) const
    {
        crd::containers::Array<T> v(alloc), av(alloc);
        v.resize(m_n);
        av.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const R s = ((i & 1U) ? R(-1) : R(1)) * (R(1) + static_cast<R>(i) / static_cast<R>(m_n));
            v[i]      = T(s);
        }
        const auto vsp  = crd::containers::Span<T>{v.data(), m_n};
        const auto avsp = crd::containers::Span<T>{av.data(), m_n};
        R          n0   = crd::hesap::dense::nrm2<T>(crd::containers::ConstSpan<T>{v.data(), m_n});
        if (n0 > R(0)) { crd::hesap::dense::scal<T>(T(R(1) / n0), vsp); }

        R              lam     = R(0);
        const crd::u32 kIters  = 20;
        const R        smlnum  = std::numeric_limits<R>::min();
        for (crd::u32 k = 0; k < kIters; ++k)
        {
            (void)m_op.apply(crd::containers::ConstSpan<T>{v.data(), m_n}, avsp); // av = A·v
            // Rayleigh quotient vᴴ·A·v (v normalized ⇒ denominator 1); sequential ⇒ deterministic.
            T num{};
            for (crd::u32 i = 0; i < m_n; ++i) { num = num + cheb_conj(v[i]) * av[i]; }
            lam = cheb_real(num);
            const R nrm = crd::hesap::dense::nrm2<T>(crd::containers::ConstSpan<T>{av.data(), m_n});
            if (nrm < smlnum) { break; }
            crd::hesap::dense::scal<T>(T(R(1) / nrm), avsp);
            crd::hesap::dense::copy<T>(crd::containers::ConstSpan<T>{av.data(), m_n}, vsp);
        }
        return lam > R(0) ? lam : R(1);
    }

    crd::hesap::sparse::ParallelSparseLinearOp<T> m_op; // A as a size-adaptive parallel spmv
    mutable crd::containers::Array<T>             m_x;  // running solution
    mutable crd::containers::Array<T>             m_res; // residual r − A·x
    mutable crd::containers::Array<T>             m_dx;  // Chebyshev correction direction
    mutable crd::containers::Array<T>             m_tmp; // A·x scratch
    crd::u32                                      m_n;
    crd::u32                                      m_deg;
    R                                             m_hi = R(1);
    R                                             m_lo = R(1) / R(30);
};

} // namespace crd::hesap::preconditioners
