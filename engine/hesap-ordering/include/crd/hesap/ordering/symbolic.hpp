#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::ordering
{
// -----------------------------------------------------------------------
// Symbolic fill metrics (v2a minimal subset; full symbolic factorisation —
// L-pattern + supernodes — lands in v2c). Operate on the SYMMETRIC pattern of a
// square matrix (the structure of A is symmetrised internally; only the lower
// triangle drives Cholesky symbolic). `nnz_l` is the fill metric the AMD gate
// (v2b) and ND (v2e) are judged by.
// -----------------------------------------------------------------------

inline constexpr crd::u32 kNoParent = 0xFFFFFFFFU;

// Elimination tree of chol(A). parent[j] = parent column of j (kNoParent for a
// root). Liu's algorithm with path compression on the symmetrised pattern.
[[nodiscard]] crd::containers::Array<crd::u32> elimination_tree(const sparse::SparsePattern& pattern,
                                                                crd::memory::IAllocator* alloc);

// Column counts of the Cholesky factor L: colcount[j] = nnz of column j of L
// (including the diagonal). Sum == nnz(L). Skeleton + etree (Gilbert-Ng-Peyton).
[[nodiscard]] crd::containers::Array<crd::u32> column_counts(const sparse::SparsePattern& pattern,
                                                             crd::containers::ConstSpan<crd::u32> etree,
                                                             crd::memory::IAllocator* alloc);

// Total nonzeros in the Cholesky factor L for this pattern+ordering — THE fill
// metric. Convenience: builds the etree + column counts and sums them.
[[nodiscard]] crd::u64 nnz_l(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc);

// -----------------------------------------------------------------------
// Column (AᵀA) symbolic — the LU / QR fill structure (v5b-2). CSparse cs_etree /
// cs_counts with ata = 1: the elimination tree and Cholesky column counts of AᵀA,
// computed WITHOUT ever forming AᵀA (the whole point — AᵀA is far denser than A).
// These operate on the matrix's COMPRESSED CSC pattern DIRECTLY (outer_ptr = column
// pointers, inner_idx = row indices, canonical-sorted ascending per column) — they
// must NOT be routed through build_adjacency, which symmetrises (that is the SYMMETRIC
// Cholesky path above). Square A assumed (rows == cols) for the sparse-LU consumer;
// the algorithm itself allows rectangular for the v5c QR consumer.
// -----------------------------------------------------------------------

// Column elimination tree = elimination tree of chol(AᵀA). parent[j] = parent column
// of j (kNoParent for a root). The tree that bounds sparse-LU fill (Gilbert-Ng) and
// drives the v5b supernodal LU schedule + the v5c multifrontal QR frontal order.
// `csc_pattern` is the compressed CSC pattern of A. CSparse cs_etree(ata=1): a single
// pass over A's columns with a `prev[row]` array, AᵀA never materialised.
[[nodiscard]] crd::containers::Array<crd::u32> column_elimination_tree(const sparse::SparsePattern& csc_pattern,
                                                                       crd::memory::IAllocator* alloc);

// Column counts of chol(AᵀA): colcount[j] = nnz of column j of the Cholesky factor of
// AᵀA — the all-row-permutations UPPER BOUND on sparse-LU fill (Gilbert-Ng). Sum ==
// nnz(chol(AᵀA)) ≥ nnz(L)+nnz(U) for any static/partial pivot choice. `etree` =
// column_elimination_tree(csc_pattern). CSparse cs_counts(ata=1): transposes A once and
// uses the init_ata head/next row-merge so AᵀA is never formed.
[[nodiscard]] crd::containers::Array<crd::u32> column_counts_ata(const sparse::SparsePattern& csc_pattern,
                                                                 crd::containers::ConstSpan<crd::u32> etree,
                                                                 crd::memory::IAllocator* alloc);

// Matrix bandwidth: max over stored (i,j) of |i - j| (the RCM target metric).
[[nodiscard]] crd::u32 bandwidth(const sparse::SparsePattern& pattern) noexcept;

// Matrix profile (envelope size): sum over rows i of (i - min column in row i).
[[nodiscard]] crd::u64 profile(const sparse::SparsePattern& pattern) noexcept;

// -----------------------------------------------------------------------
// Full symbolic factorisation (v2c) -- the v5 sparse-direct hand-off.
// -----------------------------------------------------------------------

// Postorder of the elimination forest (CSparse cs_post). post[k] is the column
// that is k-th in a postordering: children precede their parent, sibling
// subtrees emitted in ascending root index (deterministic). A postordered
// elimination drives the supernodal/multifrontal numeric factorisation in v5.
[[nodiscard]] crd::containers::Array<crd::u32> postorder(crd::containers::ConstSpan<crd::u32> etree,
                                                         crd::memory::IAllocator* alloc);

// Result of a full symbolic Cholesky factorisation. The factor L is stored
// lower-triangular in COMPRESSED CSC (column-major): `lp` (length n+1) are the
// column pointers, `li` (length lp[n] == nnz(L)) the row indices, ascending and
// duplicate-free within each column, the diagonal entry first. `super`
// (length nsuper+1) is the FUNDAMENTAL supernode partition: supernode s owns the
// contiguous column range [super[s], super[s+1]) and forms a path in the etree
// with a shared lower-triangular row pattern (the unit v5 factors block-by-block).
struct SymbolicFactor
{
    crd::u32 n = 0;
    crd::containers::Array<crd::u32> parent;   // etree parent[j]; kNoParent for a root
    crd::containers::Array<crd::u32> post;     // postorder of the etree
    crd::containers::Array<crd::u32> colcount; // nnz of column j of L (incl. diagonal)
    crd::containers::Array<crd::u32> lp;       // CSC column pointers, length n+1
    crd::containers::Array<crd::u32> li;       // CSC row indices, length lp[n] (EMPTY in supernodal mode)
    crd::containers::Array<crd::u32> super;    // supernode boundaries, length nsuper+1
    crd::u32 nsuper = 0;
    // Supernodal-mode compact pattern: per FUNDAMENTAL supernode g, the LEADING column's L row
    // pattern (== li[lp[super[g]] .. lp[super[g]+1]], ascending, diagonal first). This is the only
    // li slice the supernodal numeric path consumes, so supernodal mode builds it DIRECTLY via the
    // assembly-tree union and skips the O(nnz(L)) full `li`. `slead_ptr` has length nsuper+1; empty
    // when the full `li` was built instead (the default general-API path used by tests/benches).
    crd::containers::Array<crd::u32> slead_ptr;
    crd::containers::Array<crd::u32> slead_idx;

    explicit SymbolicFactor(crd::memory::IAllocator* alloc)
        : parent(alloc), post(alloc), colcount(alloc), lp(alloc), li(alloc), super(alloc), slead_ptr(alloc),
          slead_idx(alloc)
    {
    }

    // Total nonzeros in L (== lp[n]); equals the cs_counts nnz_l() metric.
    [[nodiscard]] crd::u64 nnz() const noexcept { return lp.empty() ? 0U : static_cast<crd::u64>(lp[n]); }
};

// Compute the full symbolic Cholesky factorisation of the symmetric pattern of a
// square matrix: elimination tree (Liu, path-compressed) + postorder + column
// counts (Gilbert-Ng-Peyton) + full L row pattern (cs_ereach row subtrees) +
// fundamental supernode partition (Liu-Ng-Peyton). One adjacency build, shared
// scratch. The emitted L pattern is bit-for-bit the structure of a numeric
// Cholesky factor of chol(PAPᵀ) under the given (already-applied) ordering.
//
// `supernodal_patterns`: when true, SKIP the O(nnz(L)) full `li` and instead build the compact
// per-fundamental-supernode leading-column patterns (`slead_ptr`/`slead_idx`) directly via the
// assembly-tree union — the only pattern slice the supernodal numeric factorisation needs. `li`
// is left empty. Bit-identical leading patterns ⇒ identical supernodal symbolic ⇒ identical factor.
// Default false preserves the general full-`li` contract (tests/benches read `li`).
[[nodiscard]] SymbolicFactor symbolic_factorize(const sparse::SparsePattern& pattern, crd::memory::IAllocator* alloc,
                                                bool supernodal_patterns = false);

} // namespace crd::hesap::ordering
