#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// =======================================================================
// v5b-1 — Gilbert-Peierls left-looking sparse LU (Davis 2006, `cs_lu`).
//
// THE SERIAL CORRECTNESS ORACLE / dynamic-pivot reference. This is NOT the
// crush slice and NOT determinism-moat-bound across worker counts: dynamic
// partial pivoting is order-dependent, so a parallel factorization would
// pick a pivot sequence that depends on dispatch order. v5b-1 is therefore
// purely SERIAL (no `num_workers`, no `parallel_for` — structurally, not by
// convention) and is the reference against which v5b-2's deterministic
// SUPERNODAL LU (SuperLU-class + MC64 + threshold static pivot, parallel,
// bit-identical {1,2,4,8}) is validated. Run-to-run deterministic for a
// fixed matrix (a pure function of the input); NOT claimed bit-identical
// across worker counts because there are none.
//
// ALGORITHM (faithful CSparse `cs_lu`): factor P·A = L·U, A square in CSC.
// Per column k: x = L \ A(:,k) via a sparse lower-triangular solve whose
// nonzero pattern is found by DFS-reachability on L's graph (`cs_reach` /
// `cs_dfs`) and walked in topological order (`cs_spsolve`); then DYNAMIC
// PARTIAL PIVOT — the unpivoted row of max |x| becomes the k-th pivot (with
// a threshold `tol` ∈ (0,1]: a diagonal/incumbent is preferred when its
// magnitude ≥ tol·max, the SuperLU threshold-partial-pivot knob; tol = 1 =
// pure partial pivot). U(:,k) = the already-pivoted entries + U(k,k) = pivot;
// L(:,k) = the unpivoted entries / pivot, unit diagonal.
//
// COLUMN ORDER: identity (no fill-reducing column permutation inside v5b-1).
// The caller pre-permutes columns for fill (COLAMD/AMD-on-AᵀA), exactly as
// the Cholesky took an AMD-permuted matrix. v5b-2 wires the column reorder.
//
// DIVERGENCES from textbook (pinned): D(lu)-1 storage is CSC L/U mirroring
// CSparse (vs the Cholesky's amalgamated supernodal panels — supernodal LU
// is v5b-2). D(lu)-2 singular/zero-pivot → info = k+1, graceful (no assert),
// matching the Cholesky non-SPD contract.
// =======================================================================

inline constexpr double kLuPartialPivot = 1.0; // tol = 1 ⇒ pure partial pivot (max magnitude)

// SparseLU<T> — the factored representation (CSC unit-lower L + CSC upper U +
// row permutation) + solve. Produced by factor_gp_lu. f32/f64 + complex
// (c32/c64; pivot uses the modulus). Move-only (owns its Arrays).
template <typename T> class SparseLU final : public IFactorization<T>
{
public:
    explicit SparseLU(crd::memory::IAllocator* alloc) noexcept;

    // Gilbert-Peierls numeric factorization of A (CSC, square). `tol` ∈ (0,1]
    // is the threshold-partial-pivot factor (1 = pure partial pivot). Called by
    // factor_gp_lu; public so the free entry can fill it. SERIAL by construction.
    void factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a, double tol = kLuPartialPivot);

    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    using IFactorization<T>::solve; // un-hide the single-RHS convenience overload
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_lnz + m_unz; }
    [[nodiscard]] crd::usize info() const noexcept override { return m_info; }

    // Factor structure (CSC, PERMUTED row space P·A = L·U). The row indices are in the
    // pivoted space; for a static/diagonal pivot sequence (tol → 0 on an MC64-matched B)
    // P = I, so they coincide with the original rows. Used by the v5b-2 symbolic oracle.
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> l_colptr() const noexcept { return {m_lp.data(), m_lp.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> l_rowidx() const noexcept
    {
        return {m_li.data(), static_cast<crd::usize>(m_lnz)};
    }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> u_colptr() const noexcept { return {m_up.data(), m_up.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> u_rowidx() const noexcept
    {
        return {m_ui.data(), static_cast<crd::usize>(m_unz)};
    }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_n = 0;
    crd::usize m_info = 0; // 0 = ok; k+1 = singular (zero pivot) at column k
    crd::u64 m_lnz = 0;    // nnz(L) incl. unit diagonal
    crd::u64 m_unz = 0;    // nnz(U) incl. diagonal
    // CSC factors in the PERMUTED row space (P·A = L·U). L is unit-lower (the
    // diagonal 1 is stored as the first entry of each column); U is upper with
    // U(k,k) the last entry of column k.
    crd::containers::Array<crd::u32> m_lp;   // length n+1; L column pointers
    crd::containers::Array<crd::u32> m_li;   // L row indices (permuted)
    crd::containers::Array<T> m_lx;          // L values
    crd::containers::Array<crd::u32> m_up;   // length n+1; U column pointers
    crd::containers::Array<crd::u32> m_ui;   // U row indices (permuted)
    crd::containers::Array<T> m_ux;          // U values
    crd::containers::Array<crd::u32> m_pinv; // length n; pinv[orig_row] = pivot step (permuted row)
};

// Factor a square sparse matrix A (CSC) into P·A = L·U via Gilbert-Peierls
// with dynamic partial pivoting. SERIAL reference oracle (v5b-1). info() != 0
// ⇒ singular. `tol` ∈ (0,1] is the threshold-partial-pivot factor.
template <typename T>
[[nodiscard]] SparseLU<T> factor_gp_lu(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a,
                                       crd::memory::IAllocator* alloc, double tol = kLuPartialPivot);

} // namespace crd::hesap::direct
