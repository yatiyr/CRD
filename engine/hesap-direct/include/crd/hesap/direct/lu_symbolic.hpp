#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// =======================================================================
// v5b-2b — Supernodal-LU SYMBOLIC: the EXACT static-pivot L/U structure,
// materialised UP FRONT. Consumed by the v5b-2c/d supernodal numeric.
//
// WHY UP-FRONT (the moat, not a perf nicety): v5b-1's Gilbert-Peierls LU
// discovers the L/U structure DURING the numeric, because dynamic partial
// pivoting only knows a pivot once the column's values exist — that is
// sequential SuperLU, and it is exactly why sequential SuperLU is NOT
// bit-identical across thread counts. Our model is SuperLU_DIST: MC64 +
// threshold STATIC pivoting (the v5b-2a front-end already made the matched
// entry the column-max, so the diagonal is a safe pivot in every column).
// Static pivoting ⇒ NO row interchanges ⇒ the pivot order is the identity in
// the MC64-permuted space ⇒ the L/U structure is a deterministic PURE FUNCTION
// of B's pattern, computable pattern-only with no values. Materialising it here
// gives the numeric a FIXED structure to fill — the same separation that earned
// the v5a Cholesky cross-thread determinism moat ({1,2,4,8} bit-identical).
//
// ALGORITHM: the structure of column k of (L,U) is the set of rows REACHABLE
// from B(:,k)'s nonzeros in the directed graph of the partial L (Gilbert-Peierls
// reachability) — run pattern-only with the fixed diagonal pivot (pinv = identity).
// Reachable rows < k are U(:,k) (above + on the diagonal); rows > k are L(:,k)
// (below); the diagonal pivot k is always present (MC64-matched). Each column's
// row list is SORTED ascending (canonical ⇒ deterministic ⇒ panel-ready), L with
// the unit diagonal first, U with the diagonal last. The column elimination tree
// (etree of BᵀB, `ordering::column_elimination_tree`) drives the relaxed supernode
// partition; the chol(BᵀB) column counts (`ordering::column_counts_ata`) give the
// Gilbert-Ng all-row-permutations fill UPPER BOUND, used to pre-reserve L/U and as
// an independent symbolic oracle (exact nnz ≤ bound).
// =======================================================================

// The materialised static-pivot symbolic factorisation of B (the MC64-transformed,
// matched-on-diagonal matrix from v5b-2a). L and U are stored CSC in the identity
// row space (P = I because the pivot is static): `lp`/`li` the unit-lower L (diagonal
// first per column), `up`/`ui` the upper U (diagonal last per column). Row lists are
// ascending within each column.
struct LuSymbolic
{
    crd::u32 n = 0;
    crd::containers::Array<crd::u32> col_etree; // column elimination tree (etree of BᵀB); kNoParent = root
    crd::containers::Array<crd::u32> col_post;  // postorder of the column etree
    crd::containers::Array<crd::u32> lp;        // L column pointers, length n+1
    crd::containers::Array<crd::u32> li;        // L row indices (ascending, unit diagonal first)
    crd::containers::Array<crd::u32> up;        // U column pointers, length n+1
    crd::containers::Array<crd::u32> ui;        // U row indices (ascending, diagonal last)
    crd::containers::Array<crd::u32> super;     // relaxed supernode boundaries over columns, length nsuper+1
    crd::u32 nsuper = 0;
    crd::u64 lnz = 0;        // nnz(L) incl. unit diagonals
    crd::u64 unz = 0;        // nnz(U) incl. diagonals
    crd::u64 fill_bound = 0; // Σ chol(BᵀB) column counts — Gilbert-Ng upper bound on max(nnz L, nnz U)

    explicit LuSymbolic(crd::memory::IAllocator* alloc)
        : col_etree(alloc), col_post(alloc), lp(alloc), li(alloc), up(alloc), ui(alloc), super(alloc)
    {
    }

    [[nodiscard]] crd::u64 nnz() const noexcept { return lnz + unz; }
};

// Build the exact static-pivot L/U symbolic structure of B (compressed CSC, square,
// matched-on-diagonal — i.e. the `out_b` produced by v5b-2a `static_lu_prepare`).
// Pattern-only (no values, no template T): the structure is identical for any value
// type. Deterministic pure function of `b_csc`.
[[nodiscard]] LuSymbolic lu_symbolic(const sparse::SparsePattern& b_csc, crd::memory::IAllocator* alloc);

} // namespace crd::hesap::direct
