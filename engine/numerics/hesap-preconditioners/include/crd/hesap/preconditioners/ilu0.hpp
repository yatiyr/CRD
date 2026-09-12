#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/convert.hpp> // transpose (for the adjoint)
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// Ilu0Preconditioner<T> -- Incomplete LU, level 0 (no fill). Phase 3.1.6 v4g.
//
// For GENERAL (nonsymmetric / non-Hermitian) A: A ≈ L·U with L (unit lower) and U
// (upper) sharing A's sparsity pattern (no fill beyond A — "level 0"). The GMRES /
// BiCGSTAB / QMR preconditioner action z = M⁻¹r = U⁻¹(L⁻¹r): forward solve L y = r
// (unit lower, no diagonal division), back solve U z = y. Stored as ONE CSR over A's
// pattern (the IKJ-eliminated combined factor) + an i32 diagonal-position array; the
// strict-lower entries are L, the diagonal+upper are U.
//
// FACTORISATION: the classic IKJ Gaussian elimination restricted to A's pattern
// (Saad, "Iterative Methods" Alg. 10.4), with a dense column-position scratch jpos.
// Requires CSR columns sorted ascending (the compress() invariant) so the k<i pass is
// left-to-right. Real + complex (no conjugation — general LU). The triangular solves are
// inherently SEQUENTIAL ⇒ trivially bit-deterministic + thread-count independent.
//
// PIVOT: a zero pivot (zero diagonal in A's pattern, or produced by the level-0
// truncation) ASSERTS — a diagonal shift is an authoring choice kept out of the engine
// (cf. Eigen's IncompleteLUT, which is dual-threshold with fill, not pure level-0).
//
// ADJOINT (M⁻ᴴ): runs the factorisation/solves on the stored Aᴴ — the true adjoint for
// the two-sided (QMR) consumer. Owns the conj-transpose factor.
// -----------------------------------------------------------------------

template <typename T>
class Ilu0Preconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    Ilu0Preconditioner(const Csr& a, crd::memory::IAllocator* alloc)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_lu(augment_diag(a, alloc))
        , m_luh(build_conj_transpose(a, alloc))
        , m_diag(alloc)
        , m_diag_h(alloc)
        , m_t(alloc)
        , m_jpos(alloc)
        , m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "Ilu0Preconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "Ilu0Preconditioner: requires a compressed CSR matrix");
        m_jpos.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i) { m_jpos[i] = -1; }
        m_t.resize(m_n);
        factor(m_lu, m_diag);    // ILU(0) of A
        factor(m_luh, m_diag_h); // ILU(0) of Aᴴ (for the adjoint apply)
    }

    // z = M⁻¹ r = U⁻¹(L⁻¹ r).
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        solve(m_lu, m_diag, r, z);
        return true;
    }
    // M⁻ᴴ r = ILU(Aᴴ)⁻¹ r (the factor of the conjugate transpose).
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        solve(m_luh, m_diag_h, r, z);
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

private:
    [[nodiscard]] static T ilu_conj(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
        else { return v; }
    }
    // Copy A into a CSR with EVERY diagonal entry present (a zero is inserted where A's
    // pattern lacks (i,i)). ILU(0) needs an explicit diagonal in each row (D-pin: the factor
    // pattern is A's pattern ∪ diagonals); real nonsymmetric matrices — e.g. gemat11 — have
    // structurally-absent diagonals. Columns stay sorted ascending (the CSR invariant).
    [[nodiscard]] static Csr augment_diag(const Csr& a, crd::memory::IAllocator* alloc)
    {
        const auto& src   = a.pattern();
        const auto* outer = src.outer_ptr.data();
        const auto* inner = src.inner_idx.data();
        const T*    av    = a.values().values.data();
        const crd::u32 n  = src.rows;
        crd::hesap::sparse::SparsePattern   pat(alloc);
        pat.rows   = src.rows;
        pat.cols   = src.cols;
        pat.format = crd::hesap::sparse::SparseFormat::Csr;
        crd::hesap::sparse::SparseValues<T> vals(alloc);
        pat.outer_ptr.push_back(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            bool has_diag = false;
            for (crd::u32 p = outer[i]; p < outer[i + 1]; ++p) { if (inner[p] == i) { has_diag = true; break; } }
            bool inserted = has_diag; // if already present, nothing to insert
            for (crd::u32 p = outer[i]; p < outer[i + 1]; ++p)
            {
                if (!inserted && inner[p] > i) { pat.inner_idx.push_back(i); vals.values.push_back(T{}); inserted = true; }
                pat.inner_idx.push_back(inner[p]);
                vals.values.push_back(av[p]);
            }
            if (!inserted) { pat.inner_idx.push_back(i); vals.values.push_back(T{}); } // diagonal is the largest col
            pat.outer_ptr.push_back(static_cast<crd::u32>(pat.inner_idx.size()));
        }
        pat.recompute_topology_hash();
        return Csr(std::move(pat), std::move(vals));
    }
    [[nodiscard]] static Csr build_conj_transpose(const Csr& a, crd::memory::IAllocator* alloc)
    {
        Csr   aug  = augment_diag(a, alloc); // diagonals present before transpose ⇒ Aᴴ has them too
        Csr   at   = crd::hesap::sparse::transpose<T>(aug, alloc);
        auto& vals = at.values().values;
        for (crd::usize k = 0; k < vals.size(); ++k) { vals[k] = ilu_conj(vals[k]); }
        return at;
    }

    [[nodiscard]] static R ilu_mag(T v) noexcept
    {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
        else { return v < R(0) ? -v : v; }
    }

    // IKJ ILU(0) of `mat` (factored in place); fills `diag` with each row's diagonal position.
    // PIVOT FLOOR (Saad, "Iterative Methods" §10.3.3): a pivot whose magnitude collapses below
    // τ = √ε·max|A_ij| is replaced by τ — pure ILU(0) can produce a zero/near-zero pivot on
    // matrices with structurally-absent or tiny diagonals (e.g. gemat11), and an unfloored
    // divide blows the factor up. The floored factor is an approximate preconditioner (never
    // a wrong solve — the Krylov solver corrects it); a true zero diagonal needs a shift the
    // caller supplies, as for IC(0).
    void factor(Csr& mat, crd::containers::Array<crd::i32>& diag)
    {
        const auto* outer = mat.pattern().outer_ptr.data();
        const auto* inner = mat.pattern().inner_idx.data();
        T*          lu    = mat.values().values.data();
        R           amax  = R(0);
        for (crd::usize k = 0; k < mat.values().values.size(); ++k) { const R m = ilu_mag(lu[k]); amax = m > amax ? m : amax; }
        const R tau = std::sqrt(std::numeric_limits<R>::epsilon()) * amax + std::numeric_limits<R>::min();
        diag.resize(m_n);
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            crd::i32 dp = -1;
            for (crd::u32 p = outer[i]; p < outer[i + 1]; ++p) { if (inner[p] == i) { dp = static_cast<crd::i32>(p); break; } }
            CRD_ASSERT_MSG(dp >= 0, "Ilu0Preconditioner: missing diagonal entry (augment_diag should guarantee one)");
            diag[i] = dp;
        }
        for (crd::u32 i = 0; i < m_n; ++i)
        {
            const crd::u32 lo = outer[i];
            const crd::u32 hi = outer[i + 1];
            for (crd::u32 p = lo; p < hi; ++p) { m_jpos[inner[p]] = static_cast<crd::i32>(p); }
            for (crd::u32 p = lo; p < hi; ++p) // ascending columns
            {
                const crd::u32 k = inner[p];
                if (k >= i) { break; }                       // L part done (reached diagonal/upper)
                const T piv = lu[p] / lu[static_cast<crd::u32>(diag[k])]; // u_kk already floored when row k closed
                lu[p]       = piv;
                for (crd::u32 q = static_cast<crd::u32>(diag[k]) + 1; q < outer[k + 1]; ++q) // row k, cols > k
                {
                    const crd::i32 jp = m_jpos[inner[q]];
                    if (jp >= 0) { lu[static_cast<crd::u32>(jp)] = lu[static_cast<crd::u32>(jp)] - piv * lu[q]; }
                }
            }
            const crd::u32 dpi = static_cast<crd::u32>(diag[i]);
            if (ilu_mag(lu[dpi]) < tau) { lu[dpi] = T(tau); } // floor a collapsed pivot
            for (crd::u32 p = lo; p < hi; ++p) { m_jpos[inner[p]] = -1; }
        }
    }

    // z = U⁻¹(L⁻¹ r): forward L y=r (unit lower), back U z=y.
    void solve(const Csr& mat, const crd::containers::Array<crd::i32>& diag, crd::containers::ConstSpan<T> r,
               crd::containers::Span<T> z) const
    {
        const auto* outer = mat.pattern().outer_ptr.data();
        const auto* inner = mat.pattern().inner_idx.data();
        const T*    lu    = mat.values().values.data();
        for (crd::u32 i = 0; i < m_n; ++i) // L y = r (unit diagonal)
        {
            T acc = r[i];
            for (crd::u32 p = outer[i]; p < static_cast<crd::u32>(diag[i]); ++p) { acc = acc - lu[p] * m_t[inner[p]]; }
            m_t[i] = acc;
        }
        for (crd::u32 ii = 0; ii < m_n; ++ii) // U z = y
        {
            const crd::u32 i   = m_n - 1 - ii;
            T              acc = m_t[i];
            for (crd::u32 p = static_cast<crd::u32>(diag[i]) + 1; p < outer[i + 1]; ++p) { acc = acc - lu[p] * z[inner[p]]; }
            z[i] = acc / lu[static_cast<crd::u32>(diag[i])];
        }
    }

    Csr                               m_lu;     // ILU(0) factor of A (combined L\U, owned)
    Csr                               m_luh;    // ILU(0) factor of Aᴴ (for adjoint, owned)
    crd::containers::Array<crd::i32>  m_diag;   // diagonal position per row in m_lu
    crd::containers::Array<crd::i32>  m_diag_h; // diagonal position per row in m_luh
    mutable crd::containers::Array<T> m_t;      // solve scratch
    crd::containers::Array<crd::i32>  m_jpos;   // factorization column-position scratch
    crd::u32                          m_n;
};

} // namespace crd::hesap::preconditioners
