#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/ordering/mc64.hpp>
#include <crd/hesap/preconditioners/ilut.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// MultilevelIlu<T> -- multilevel ILU (ILUPACK-class, Bollhöfer-Saad). Phase 3.1.6 v4j.
//
// v4j-1b (this slice) ships the SCAFFOLD: the MC64 (v4j-1a) match+scale robustness
// front-end + a single-level ILUT of the transformed matrix. v4j-2 replaces the
// single level with the inverse-based-pivoting recursion (rows whose elimination would
// make ‖L⁻¹‖/‖U⁻¹‖ exceed κ are DEFERRED to a coarser level, factored recursively) --
// the actual "multilevel". So this is a working, robust preconditioner now (MC64
// scaling fixes the conditioning of hard, badly-scaled, non-diagonally-dominant
// matrices that plain ILUT chokes on) and the structure the recursion plugs into.
//
// Transform (MC64): B = D_r·A·D_c·Pᶜ has the matched (largest) entries on its diagonal
// and is scaled toward an I-matrix (|diag|=1, |off-diag|≤1). Then
//   M_inner ≈ B⁻¹ (an ILUT of B)  ⇒  A⁻¹ = D_c·Pᶜ·B⁻¹·D_r , so
//   apply(r)         = D_c·Pᶜ·( ILUT(B)⁻¹·(D_r·r) )
//   apply_adjoint(r) = D_r·( ILUT(B)⁻ᴴ·(Pᶜᵀ·(D_c·r)) )   (D_r, D_c real ⇒ Dᴴ = D)
//
// Deterministic (MC64 + the triplet build + ILUT are all deterministic; the moat is
// carried by the operator's parallel spmv). Real + complex.
// -----------------------------------------------------------------------

template <typename T>
class MultilevelIlu final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    MultilevelIlu(const Csr& a, crd::memory::IAllocator* alloc, crd::u32 lfil = 0, R droptol = R(-1))
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_mc64(crd::hesap::ordering::mc64_match_and_scale<T>(a, alloc))
        , m_b(build_transformed(a, m_mc64, alloc))
        , m_ilut(m_b, alloc, lfil, droptol)
        , m_rs(alloc)
        , m_yb(alloc)
        , m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "MultilevelIlu: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "MultilevelIlu: requires a compressed CSR matrix");
        m_rs.resize(m_n);
        m_yb.resize(m_n);
    }

    // z = D_c·Pᶜ·( ILUT(B)⁻¹·(D_r·r) ).
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        for (crd::u32 i = 0; i < m_n; ++i) { m_rs[i] = T(static_cast<R>(m_mc64.dr[i])) * r[i]; } // D_r·r
        (void)m_ilut.apply(crd::containers::ConstSpan<T>{m_rs.data(), m_n},
                           crd::containers::Span<T>{m_yb.data(), m_n}); // yb ≈ B⁻¹·(D_r·r)
        const auto* cp = m_mc64.colperm.data();
        for (crd::u32 k = 0; k < m_n; ++k) { z[cp[k]] = T(static_cast<R>(m_mc64.dc[cp[k]])) * m_yb[k]; } // D_c·Pᶜ·yb
        return true;
    }

    // z = D_r·( ILUT(B)⁻ᴴ·(Pᶜᵀ·(D_c·r)) ).
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        const auto* cp = m_mc64.colperm.data();
        for (crd::u32 k = 0; k < m_n; ++k) { m_yb[k] = T(static_cast<R>(m_mc64.dc[cp[k]])) * r[cp[k]]; } // Pᶜᵀ·D_c·r
        (void)m_ilut.apply_adjoint(crd::containers::ConstSpan<T>{m_yb.data(), m_n},
                                   crd::containers::Span<T>{m_rs.data(), m_n}); // rs ≈ B⁻ᴴ·yb
        for (crd::u32 i = 0; i < m_n; ++i) { z[i] = T(static_cast<R>(m_mc64.dr[i])) * m_rs[i]; } // D_r·rs
        return true;
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    [[nodiscard]] crd::usize factor_nnz() const noexcept { return m_ilut.factor_nnz(); }
    [[nodiscard]] crd::u32   num_levels() const noexcept { return 1; } // v4j-2: the recursion depth

private:
    // B = D_r·A·D_c·Pᶜ : scale (rows D_r, cols D_c) then permute columns by colperm so the matched
    // (max-weight) entries land on the diagonal. B[i, invperm[j]] = D_r[i]·a_ij·D_c[j].
    [[nodiscard]] static Csr build_transformed(const Csr& a, const crd::hesap::ordering::Mc64Scaling& mc,
                                               crd::memory::IAllocator* alloc)
    {
        const crd::u32 n     = a.rows();
        const auto*    outer = a.pattern().outer_ptr.data();
        const auto*    inner = a.pattern().inner_idx.data();
        const T*       vals  = a.values().values.data();
        crd::containers::Array<crd::u32> invperm(alloc); // invperm[colperm[k]] = k
        invperm.resize(n);
        for (crd::u32 k = 0; k < n; ++k) { invperm[mc.colperm[k]] = k; }
        crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            const T dri = T(static_cast<R>(mc.dr[i]));
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
            {
                const crd::u32 j = inner[q];
                tb.add(i, invperm[j], dri * vals[q] * T(static_cast<R>(mc.dc[j])));
            }
        }
        return tb.compress();
    }

    crd::hesap::ordering::Mc64Scaling m_mc64; // colperm + D_r + D_c
    Csr                               m_b;    // B = D_r·A·D_c·Pᶜ (matched on diagonal)
    IlutPreconditioner<T>             m_ilut; // ILUT of B  (v4j-2: the multilevel hierarchy)
    mutable crd::containers::Array<T> m_rs;   // D_r·r scratch
    mutable crd::containers::Array<T> m_yb;   // B-space solution scratch
    crd::u32                          m_n;
};

} // namespace crd::hesap::preconditioners
