#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/convert.hpp>    // transpose (for Lᴴ)
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/structural.hpp> // tril
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// Ic0Preconditioner<T> -- Incomplete Cholesky, level 0 (no fill). Phase 3.1.6 v4g.
//
// For SPD / HPD A: A ≈ L·Lᴴ with L on A's LOWER-triangular sparsity pattern (no fill
// beyond A — that is the "level 0"). The CG/MINRES preconditioner action z = M⁻¹r =
// L⁻ᴴ(L⁻¹r): forward solve L y = r, back solve Lᴴ z = y. Both triangular solves are
// inherently SEQUENTIAL (each row depends on earlier ones) ⇒ trivially bit-deterministic
// + thread-count independent. (A parallel level-scheduled triangular solve is a separate
// shared-infrastructure slice; v4g ships the sequential canonical form.)
//
// FACTORISATION: left-looking IC(0) (LAPACK/CSparse row form) over A's lower pattern,
// with a dense column-position scratch jpos for the sparse inner products. For complex
// HPD A the Cholesky update conjugates the right factor (L_im·conj(L_jm)); the diagonal
// pivot is real-positive.
//
// PIVOT ROBUSTNESS: the level-0 truncation can yield a non-positive pivot on matrices
// where the full Cholesky would succeed; we ASSERT (the matrix needs a diagonal shift —
// an authoring choice, A + α·I — kept OUT of the engine; Eigen's IncompleteCholesky does
// the Manteuffel shift internally, so a matrix where Eigen succeeds and this asserts is
// that difference, not a defect). Requires every diagonal present in A's pattern.
// -----------------------------------------------------------------------

template <typename T>
class Ic0Preconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    Ic0Preconditioner(const Csr& a, crd::memory::IAllocator* alloc)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_l(crd::hesap::sparse::tril<T>(a, 0, alloc)) // lower triangle incl. diagonal (A's pattern)
        , m_lh(alloc_empty(alloc))
        , m_dinv(alloc)
        , m_a_lower(alloc)
        , m_t(alloc)
        , m_jpos(alloc)
        , m_n(a.rows())
        , m_shift(R(0))
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "Ic0Preconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "Ic0Preconditioner: requires a compressed CSR matrix");
        // Keep the original lower-triangle values, SYMMETRICALLY DIAGONAL-SCALED to unit
        // diagonal: the stored matrix is D⁻¹ᐟ²·A·D⁻¹ᐟ² (D = diag A). Stiffness matrices have a
        // ~1e9 diagonal dynamic range; scaling to unit diagonal makes level-0 IC + a small
        // shift well-posed (an unscaled α∝max|diag| over-shifts the small eigenvalues into a
        // useless M ≈ huge·I). The apply re-bakes the scaling: z = D⁻¹ᐟ²·(L·Lᴴ)⁻¹·D⁻¹ᐟ²·r.
        // The shift A+α·I (Manteuffel/MATLAB-ichol diagcomp) is then on the unit-diagonal
        // matrix, so α is a small dimensionless number doubled on a non-positive pivot.
        const auto*  outer = m_l.pattern().outer_ptr.data();
        const auto*  inner = m_l.pattern().inner_idx.data();
        const auto&  lv    = m_l.values().values;
        m_dinv.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const crd::u32 hi = outer[i + 1];
            CRD_ASSERT_MSG(hi > outer[i] && inner[hi - 1] == i, "Ic0Preconditioner: missing diagonal entry");
            const R dii = ic_real(lv[hi - 1]);
            CRD_ASSERT_MSG(dii > R(0), "Ic0Preconditioner: non-positive diagonal (A not SPD)");
            m_dinv[i] = T(R(1) / std::sqrt(dii));
        }
        m_a_lower.resize(lv.size());
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            for (crd::u32 p = outer[i]; p < outer[i + 1]; ++p)
            {
                m_a_lower[p] = lv[p] * m_dinv[i] * m_dinv[inner[p]]; // (D⁻¹ᐟ²·A·D⁻¹ᐟ²)_ij
            }
        }
        m_jpos.resize(m_n);

        const R alpha0 = manteuffel_alpha0();
        R       alpha  = R(0);
        bool    ok     = false;
        for (int attempt = 0; attempt < 64 && !ok; ++attempt)
        {
            ok = try_factor(alpha);
            if (!ok) { alpha = (alpha == R(0)) ? alpha0 : alpha * R(2); }
            else { m_shift = alpha; }
        }
        CRD_ASSERT_MSG(ok, "Ic0Preconditioner: diagonal shift failed to yield an SPD level-0 factor");
        m_lh = build_conj_transpose(m_l, alloc); // Lᴴ (upper CSR) for the back solve
        m_t.resize(m_n);
    }

    // The diagonal shift α actually used (0 when pure level-0 succeeded). Diagnostic.
    [[nodiscard]] R shift() const noexcept { return m_shift; }

    // z = D⁻¹ᐟ²·(L·Lᴴ)⁻¹·D⁻¹ᐟ²·r (L is the factor of the SCALED matrix). M is HPD ⇒
    // M⁻ᴴ = M⁻¹, so apply_adjoint == apply.
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = r[i] * m_dinv[i]; } // scale in: D⁻¹ᐟ² r
        forward_solve(z, m_t);                                          // L y = (scaled r)
        back_solve(m_t, z);                                            // Lᴴ z = y
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = z[i] * m_dinv[i]; } // scale out: D⁻¹ᐟ²
        return true;
    }
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        return apply(r, z);
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    [[nodiscard]] static T ic_conj(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
        else { return v; }
    }
    [[nodiscard]] static R ic_real(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return v.re; }
        else { return v; }
    }
    static Csr alloc_empty(crd::memory::IAllocator* alloc) // placeholder, reassigned after factor()
    {
        crd::hesap::sparse::SparsePattern   pat(alloc);
        crd::hesap::sparse::SparseValues<T> vals(alloc);
        return Csr(std::move(pat), std::move(vals));
    }
    [[nodiscard]] static Csr build_conj_transpose(const Csr& l, crd::memory::IAllocator* alloc)
    {
        Csr   lt   = crd::hesap::sparse::transpose<T>(l, alloc);
        auto& vals = lt.values().values;
        for (crd::usize k = 0; k < vals.size(); ++k) { vals[k] = ic_conj(vals[k]); }
        return lt;
    }

    // Initial Manteuffel shift on the UNIT-DIAGONAL scaled matrix: a small dimensionless
    // value, doubled on each failed attempt (α=0 tried first, so a well-posed level-0
    // factor uses no shift at all).
    [[nodiscard]] R manteuffel_alpha0() const { return R(1e-3); }

    // Left-looking IC(0) of (A + shift·I) in place into m_l. Returns false on a non-positive
    // pivot (so the caller can retry with a larger shift) instead of asserting.
    [[nodiscard]] bool try_factor(R shift)
    {
        const auto* outer = m_l.pattern().outer_ptr.data();
        const auto* inner = m_l.pattern().inner_idx.data();
        T*          lval  = m_l.values().values.data();
        // Reload the original lower values + apply the diagonal shift.
        for (crd::usize k = 0; k < m_a_lower.size(); ++k) { lval[k] = m_a_lower[k]; }
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const crd::u32 hi = outer[i + 1];
            CRD_ASSERT_MSG(hi > outer[i] && inner[hi - 1] == i, "Ic0Preconditioner: missing diagonal entry");
            lval[hi - 1] = lval[hi - 1] + T(shift);
        }
        for (crd::u32 i = 0; i < m_n; ++i) { m_jpos[i] = -1; }

        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const crd::u32 lo = outer[i];
            const crd::u32 hi = outer[i + 1];
            for (crd::u32 p = lo; p < hi; ++p) { m_jpos[inner[p]] = static_cast<crd::i32>(p); }

            for (crd::u32 p = lo; p < hi; ++p) // ascending columns (CSR sorted)
            {
                const crd::u32 j = inner[p];
                if (j >= i) { break; } // reached the diagonal
                // L_ij = (A_ij − Σ_{m<j, common} L_im·conj(L_jm)) / L_jj
                T s = lval[p];
                for (crd::u32 q = outer[j]; q < outer[j + 1]; ++q)
                {
                    const crd::u32 m = inner[q];
                    if (m >= j) { break; }
                    const crd::i32 ip = m_jpos[m];
                    if (ip >= 0) { s = s - lval[static_cast<crd::u32>(ip)] * ic_conj(lval[q]); }
                }
                lval[p] = s / lval[outer[j + 1] - 1]; // / L_jj (real-positive)
            }
            // L_ii = sqrt(A_ii − Σ_{j<i} |L_ij|²)
            T sii = lval[hi - 1];
            for (crd::u32 p = lo; p < hi - 1; ++p) { sii = sii - lval[p] * ic_conj(lval[p]); }
            const R d = ic_real(sii);
            if (!(d > R(0))) { return false; } // non-positive pivot ⇒ retry with a larger shift
            lval[hi - 1] = T(std::sqrt(d));

            for (crd::u32 p = lo; p < hi; ++p) { m_jpos[inner[p]] = -1; }
        }
        return true;
    }

    // L y = r (lower-triangular, explicit diagonal; m_l).
    void forward_solve(crd::containers::ConstSpan<T> r, crd::containers::Span<T> y) const
    {
        const auto* outer = m_l.pattern().outer_ptr.data();
        const auto* inner = m_l.pattern().inner_idx.data();
        const T*    lval  = m_l.values().values.data();
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            T acc = r[i];
            const crd::u32 hi = outer[i + 1];
            for (crd::u32 p = outer[i]; p < hi - 1; ++p) { acc = acc - lval[p] * y[inner[p]]; } // cols j<i
            y[i] = acc / lval[hi - 1];                                                          // / L_ii
        }
    }

    // Lᴴ z = y (upper-triangular, m_lh; diagonal = conj(L_ii) is the FIRST entry of each row).
    void back_solve(crd::containers::ConstSpan<T> y, crd::containers::Span<T> z) const
    {
        const auto* outer = m_lh.pattern().outer_ptr.data();
        const auto* inner = m_lh.pattern().inner_idx.data();
        const T*    hval  = m_lh.values().values.data();
        for (crd::u32 ii = 0; ii < m_n; ++ii)
        {
            const crd::u32 i  = m_n - 1 - ii;
            T              acc = y[i];
            const crd::u32 lo = outer[i];
            for (crd::u32 p = lo + 1; p < outer[i + 1]; ++p) { acc = acc - hval[p] * z[inner[p]]; } // cols j>i
            z[i] = acc / hval[lo];                                                                  // / conj(L_ii)
        }
    }

    Csr                               m_l;      // L (lower CSR, owned; factored in place)
    Csr                               m_lh;     // Lᴴ (upper CSR, owned)
    crd::containers::Array<T>         m_dinv;    // D⁻¹ᐟ² (1/sqrt(diag A)) per row, as T
    crd::containers::Array<T>         m_a_lower; // SCALED tril(A) values (for shifted retries)
    mutable crd::containers::Array<T> m_t;      // solve scratch
    crd::containers::Array<crd::i32>  m_jpos;   // factorization column-position scratch
    crd::u32                          m_n;
    R                                 m_shift;  // Manteuffel diagonal shift used (0 if none)
};

} // namespace crd::hesap::preconditioners
