#pragma once

// shift_invert.hpp — Phase 3.1.6 v6-d: SHIFT-INVERT spectral transformation (the FIRST algorithmic crush
// lever; the v5↔v6 bridge). The operator (A − σI)⁻¹ has eigenvalues μ = 1/(λ − σ): eigenvalues λ NEAR the
// shift σ map to the LARGEST-magnitude μ ⇒ they converge in FAR fewer matvecs than a plain Krylov method
// could reach them — and INTERIOR eigenvalues (which v6-a/b cannot target at all from the spectrum ends)
// become accessible. (A − σI) is factored once by the v5 PARTIAL-PIVOT multifrontal LU (accurate on the
// indefinite shifted matrix); each Lanczos matvec is one back-substitution.
//
// MOAT: the factor is bit-identical across {1,2,4,8} build-workers (the v5f-e moat); the solve apply +
// thick-restart Lanczos are deterministic ⇒ the recovered eigenpairs are bit-identical.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/eigen/eigen_problem.hpp>
#include <crd/hesap/eigen/thick_restart.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <type_traits>
#include <utility>

namespace crd::hesap::eigen
{
namespace dir = crd::hesap::direct;

// LinearOp wrapping a direct factor of (A − σI): apply(x) = (A − σI)⁻¹·x (one back-substitution, RAW — no IR).
template <typename T, typename Fac> class ShiftInvertOp final : public crd::hesap::LinearOp<T>
{
public:
    ShiftInvertOp(const Fac& f, crd::usize n, crd::memory::IAllocator* alloc) noexcept
        : m_f(&f), m_n(n), m_tmp(alloc)
    {
        m_tmp.resize(n);
    }
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const override
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            y[i] = x[i];
        }
        m_f->apply_inverse({y.data(), m_n}, 1); // y = (A − σI)⁻¹·x
        return true;
    }
    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    const Fac*                m_f;
    crd::usize                m_n;
    mutable crd::containers::Array<T> m_tmp;
};

// Compute `opts.nev` eigenpairs of a REAL SYMMETRIC sparse A (CSR) CLOSEST to the shift σ, via shift-invert
// thick-restart Lanczos. Recovers λ = σ + 1/μ + the TRUE residual ‖A·x − λ·x‖ (using A, not the SI operator).
// `info != 0` on the returned result's nconv==0 ⇒ (A − σI) was singular (σ ≈ an eigenvalue) — caller nudges σ.
template <typename T>
[[nodiscard]] EigenResult<T> eigs_sym_shift_invert(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                                   T sigma, const EigenOptions<T>& opts,
                                                   crd::memory::IAllocator* alloc, crd::u32 num_workers = 1)
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>,
                  "eigs_sym_shift_invert: real symmetric");
    using R = T;
    EigenResult<T> result(alloc);
    const crd::u32 n = a.pattern().rows;
    result.n = n;
    if (n == 0 || opts.nev == 0)
    {
        return result;
    }

    // Build (A − σI) with exactly one diagonal entry per row (robust to missing/duplicate diagonals).
    sparse::TripletBuilder<T> tb(alloc, n, n);
    const crd::u32* rp = a.pattern().outer_ptr.data();
    const crd::u32* ci = a.pattern().inner_idx.data();
    const T* av = a.values().values.data();
    for (crd::u32 r = 0; r < n; ++r)
    {
        T diag = T{0};
        for (crd::u32 p = rp[r]; p < rp[r + 1]; ++p)
        {
            if (ci[p] == r)
            {
                diag = av[p];
            }
            else
            {
                tb.add(r, ci[p], av[p]);
            }
        }
        tb.add(r, r, diag - sigma);
    }
    auto ashift = tb.compress();
    auto fac = dir::factor_multifrontal_lu_pp<T>(ashift, alloc, num_workers); // accurate on the indefinite shift
    if (fac.info() != 0)
    {
        return result; // (A − σI) singular — σ hit an eigenvalue
    }

    ShiftInvertOp<T, dir::MultifrontalLU<T>> si(fac, n, alloc);
    EigenOptions<T> sio = opts;
    sio.which = Which::LargestMagnitude; // largest |μ| ⇔ λ closest to σ
    sio.compute_vectors = true;
    // The SI residual transforms to ‖A·x − λ·x‖ ≈ (‖A − σI‖/|μ|)·‖r_si‖ on A; converge the inner eigensolve
    // TIGHTER than the final tol so the transformed A-residual lands comfortably below it.
    sio.tol = opts.effective_tol() * static_cast<R>(0.01);
    auto rsi = eigs_sym_tr<T>(si, sio, alloc);

    const sparse::SparseLinearOp<T> aop(a);
    const crd::u32 k = static_cast<crd::u32>(rsi.values.size());
    result.values.resize(k);
    result.residuals.resize(k);
    if (opts.compute_vectors)
    {
        result.vectors.resize(static_cast<crd::usize>(n) * k);
    }
    crd::containers::Array<T> x(alloc);
    crd::containers::Array<T> ax(alloc);
    x.resize(n);
    ax.resize(n);
    const R tol = opts.effective_tol();
    crd::u32 nconv = 0;
    for (crd::u32 s = 0; s < k; ++s)
    {
        const R mu = rsi.values[s].re; // SI operator is symmetric ⇒ μ real
        if (mu == R{0})
        {
            continue;
        }
        const R lam = sigma + R{1} / mu;
        result.values[s] = crd::hesap::Complex<R>{lam, R{0}};
        const T* xv = rsi.vectors.data() + static_cast<crd::usize>(s) * n;
        for (crd::u32 i = 0; i < n; ++i)
        {
            x[i] = xv[i];
        }
        (void)aop.apply({x.data(), n}, {ax.data(), n}); // true residual on A (not the SI operator)
        R rn = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            const R d = static_cast<R>(ax[i]) - lam * static_cast<R>(x[i]);
            rn += d * d;
        }
        rn = std::sqrt(rn);
        result.residuals[s] = rn;
        if (rn <= tol * (std::fabs(lam) > R{1} ? std::fabs(lam) : R{1}))
        {
            ++nconv;
        }
        if (opts.compute_vectors)
        {
            T* xc = result.vectors.data() + static_cast<crd::usize>(s) * n;
            for (crd::u32 i = 0; i < n; ++i)
            {
                xc[i] = x[i];
            }
        }
    }
    result.nconv = nconv;
    result.converged = nconv >= opts.nev;
    result.iterations = rsi.iterations;
    return result;
}

} // namespace crd::hesap::eigen
