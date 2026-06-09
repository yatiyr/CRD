#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// =======================================================================
// v5a-1b — left-looking SUPERNODAL Cholesky (SPD), CHOLMOD-class.
//
// DESIGN — pinned 2026-05-28 (advisor-validated). Implemented in stages
// (see "Build order" below); this header is the locked contract.
//
// Inputs: SPD matrix A (CSC, lower triangle) + the v2c SymbolicFactor
// (etree / postorder / colcount / li / fundamental supernodes), computed
// from A's already-fill-reduced pattern (caller applies amd_order/nd_order
// + apply_symmetric first). The factorization is A = L·Lᵀ.
//
// PINNED DECISIONS (→ ADR-0065 §27):
//
//  D(direct)-1  Left-looking supernodal (CHOLMOD; NOT multifrontal — the
//               determinism moat is non-trivial through the supernodal
//               update mechanism, which is the whole point of v5a-3).
//
//  D(direct)-2  RELAXED AMALGAMATION (MANDATORY for the crush). v2c's
//               fundamental supernodes are narrow (1-2 cols on real
//               matrices); without amalgamation "supernodal" degenerates
//               to column-Cholesky and will NOT beat Eigen SimplicialLLT.
//               Merge child supernode c into its snode-etree parent p iff
//                   extra_zeros ≤ nrelax · (cols_p + cols_c)
//               (CHOLMOD's rule), evaluated in postorder. Default
//               nrelax = 8 (tuned by the close benchmark). CORRECTNESS
//               note: amalgamation errors are PERF bugs, not correctness
//               bugs — the merged panel's row pattern is the exact UNION
//               of the constituents' patterns, so any merge yields a
//               correct factor (extra explicit zeros factor as zeros);
//               the criterion only trades panel size vs explicit zeros.
//
//  PANEL LAYOUT = RowMajor. CHOLMOD/LAPACK use ColMajor, but Cerid's dense
//  syrk/herk/trsm + factor_cholesky are RowMajor-only (verified
//  cholesky.cpp/blas3.hpp 2026-05-28). Using RowMajor panels lets the
//  diagonal-block factor (factor_cholesky), the subdiagonal trsm, and the
//  cmod gemm/syrk all flow through the existing kernels with no layout
//  bridging. (Deliberate divergence from the CHOLMOD ColMajor convention,
//  driven by Cerid's RowMajor-first BLAS-3.)
//
//  DETERMINISM (v5a-3 moat): serial reduction within a supernode + a
//  FIXED update-list traversal order ⇒ bit-exact L across {1,2,4,8,16}
//  threads. Trivial in serial v5a-1b; proven under tree-parallelism in
//  v5a-3.
//
// The cmod update-list mechanism (CHOLMOD cholmod_super_numeric.c names,
// kept for cross-reference):
//   Head[i] = head of the list of supernodes whose NEXT pending
//             contribution is to supernode i.
//   Next[K] = next supernode in the same list as K.
//   Lpos[K] = index into K's row pattern where K's next contribution
//             begins (advances past consumed rows).
//   After factoring supernode J: for each K traversed off Head[J],
//   advance Lpos[K] past J's columns; if K has rows remaining, find the
//   smallest such row, look up its (amalgamated) supernode S, push K onto
//   Head[S]; else K is done.
//
// BUILD ORDER (staged; v5a-1b is NOT "finished" until step 6's bench
// prints the crush — benchmarks-mandatory + never-regress, ADR-0065 §27):
//   1. build_supernodal_symbolic (amalgamation + per-supernode row
//      pattern + panel dims).               [structure; this is next]
//   2. scatter A(:,cols_J) → panel.
//   3. cdiv (factor_cholesky on the diagonal block + trsm on the
//      rectangular below-diagonal part) — single-supernode == dense
//      Cholesky bit-for-bit.
//   4. cmod via Lpos/Head/Next — 2-supernode tree == dense Cholesky.
//   5. solve (supernodal fwd Ly=b + back Lᵀx=y, multi-RHS).
//   6. CRD_BUILD_HESAP_VS_SUITESPARSE oracle + bench factor+solve vs
//      Eigen SimplicialLLT + CHOLMOD on bcsstk25 / nd{6,12,24}k /
//      af_shell8 / ldoor / Flan_1565. CLOSE TARGETS: factor ≥ 1.5×
//      Eigen SimplicialLLT, solve ≥ 1.2×, factor ≤ 1.3× of CHOLMOD.
// =======================================================================

inline constexpr crd::u32 kSupernodeRelax = 8; // D(direct)-2 nrelax (bench-tuned)

// Amalgamated supernodal symbolic structure: the v5a-1b numeric storage
// layout, built from a v2c SymbolicFactor. Each supernode s owns columns
// [scol[s], scol[s+1]) and a row pattern srow[srowp[s] .. srowp[s+1])
// (ascending global rows; the first (scol[s+1]-scol[s]) of them are the
// dense diagonal block). The dense panel for s is therefore
// (|srow_s|) rows × (cols_s) columns, RowMajor.
struct SupernodalSymbolic
{
    crd::u32 n = 0;                             // matrix dimension
    crd::u32 nsuper = 0;                        // amalgamated supernode count
    crd::containers::Array<crd::u32> scol;      // length nsuper+1; column boundaries
    crd::containers::Array<crd::u32> srowp;     // length nsuper+1; row-pattern CSR offsets
    crd::containers::Array<crd::u32> srow;      // length srowp[nsuper]; ascending global rows
    crd::containers::Array<crd::u32> col_super; // length n; col → owning supernode
    crd::u64 lnz = 0;                           // Σ |srow_s|·cols_s = dense factor storage

    explicit SupernodalSymbolic(crd::memory::IAllocator* alloc)
        : scol(alloc), srowp(alloc), srow(alloc), col_super(alloc)
    {
    }
};

// Build the amalgamated supernodal symbolic from a v2c SymbolicFactor
// (D(direct)-2 relaxed amalgamation, nrelax). Step 1 of the build order.
[[nodiscard]] SupernodalSymbolic build_supernodal_symbolic(const ordering::SymbolicFactor& sf,
                                                           crd::memory::IAllocator* alloc,
                                                           crd::u32 nrelax = kSupernodeRelax);

// SupernodalCholesky<T> — the factored representation + solve. Real SPD
// (f32/f64) in v5a-1b; complex Hermitian LLᴴ at v5a-2 (needs a complex
// dense Cholesky + herk, added there). Produced by factor_supernodal_cholesky.
template <typename T> class SupernodalCholesky final : public IFactorization<T>
{
public:
    explicit SupernodalCholesky(crd::memory::IAllocator* alloc) noexcept;

    // Numeric factorization (steps 1-4). `pattern`/`values` describe a SYMMETRIC
    // SPD matrix (any consistent storage — full or one triangle); column o's lower
    // triangle = the entries of outer-index o with inner ≥ o. Called by
    // factor_supernodal_cholesky; public so the free entry can fill it.
    // `num_workers` (v5a-3): 1 = serial (no jobs dependency); >1 = TREE-PARALLEL —
    // supernode-etree level scheduling over `crd::jobs` (caller must have
    // jobs::init()'d). Bit-identical L across worker counts (dataflow: each
    // supernode writes only its own panel, reads already-factored descendants,
    // per-worker scratch) — the determinism moat.
    void factorize(const sparse::SparsePattern& pattern, crd::containers::ConstSpan<T> values,
                   crd::u32 nrelax = kSupernodeRelax, crd::u32 num_workers = 1, bool reuse_symbolic = false);

    // refactorize — NUMERIC re-factorization reusing the symbolic analysis (m_sym) from a prior factorize() on a
    // STRUCTURALLY-IDENTICAL `pattern` (same nonzero structure; new `values`). Skips the expensive symbolic phase
    // (symbolic_factorize: AMD + etree + supernode amalgamation — the v5a CHOLMOD-gap cost). The LM normal-equations
    // loop calls this every λ-trial (JᵀJ pattern is constant across the whole solve) so it pays symbolic ONCE — the
    // gate to matching Ceres (which caches symbolic). Bit-identical to a fresh factorize() on the same matrix.
    void refactorize(const sparse::SparsePattern& pattern, crd::containers::ConstSpan<T> values,
                     crd::u32 num_workers = 1)
    {
        factorize(pattern, values, kSupernodeRelax, num_workers, /*reuse_symbolic=*/true);
    }

    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    using IFactorization<T>::solve; // un-hide the single-RHS convenience overload
    // Tuning/testing hook: run the multi-RHS solve with a FORCED worker count. nw≤1 → serial (RIGHT-looking
    // forward + serial backward); nw>1 → level-parallel (LEFT-looking forward + backward). The two forward
    // variants share the k-ascending accumulation order ⇒ bit-identical x across all nw (the solve
    // determinism moat — verified by the [v5a-4][moat] serial-vs-parallel bitwise test). solve(rhs,nrhs)
    // calls this with crd::jobs::num_workers().
    [[nodiscard]] bool solve_with_workers(crd::containers::Span<T> rhs, crd::usize nrhs, crd::u32 nw) const;
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_lnz; }
    [[nodiscard]] crd::usize info() const noexcept override { return m_info; }

    // Read-only view of the amalgamated supernode structure (column boundaries +
    // row patterns). Lets a consumer introspect the factor shape (supernode count
    // / size distribution) without touching numeric storage.
    [[nodiscard]] const SupernodalSymbolic& symbolic() const noexcept { return m_sym; }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_n = 0;
    crd::u64 m_lnz = 0;
    crd::usize m_info = 0; // 0 = SPD ok; k+1 = non-positive pivot in column k
    // Supernodal factor storage (RowMajor panels + structure) lands with step 3.
    SupernodalSymbolic m_sym;
    crd::containers::Array<T> m_lx;         // concatenated dense panels, lnz entries
    crd::containers::Array<crd::u32> m_lxp; // length nsuper+1; panel offsets into m_lx
    // Supernode-etree levels (stored from factorize) for the level-parallel solve: same-level
    // supernodes are mutually independent. Backward solve is left-looking (each supernode writes
    // only its own columns, reads completed ancestors) ⇒ level-parallel is race-free + bit-identical.
    crd::containers::Array<crd::u32> m_lvl_ptr;  // length m_nlevels+1
    crd::containers::Array<crd::u32> m_lvl_list; // length nsuper; supernodes grouped by level
    crd::u32 m_nlevels = 0;
    // Supernode update-lists (stored from factorize) for the level-parallel LEFT-LOOKING forward
    // solve: upd_list[upd_ptr[s]..upd_ptr[s+1]) = the descendant supernodes whose row pattern reaches
    // s's columns (k-ascending). The forward gathers Σ_k L_{s,k}·Y_k from them (race-free, vs the
    // right-looking scatter); k-ascending order == the right-looking accumulation order ⇒ bit-identical.
    crd::containers::Array<crd::u32> m_upd_ptr;  // length nsuper+1
    crd::containers::Array<crd::u32> m_upd_list; // length upd_ptr[nsuper]
};

// Factor a SPD matrix into a supernodal Cholesky. `pattern`/`values` describe a
// symmetric SPD matrix (full or one-triangle storage). Computes the v2c symbolic
// internally, amalgamates, then numerically factorizes. info() != 0 ⇒ not SPD.
template <typename T>
[[nodiscard]] SupernodalCholesky<T>
factor_supernodal_cholesky(const sparse::SparsePattern& pattern, crd::containers::ConstSpan<T> values,
                           crd::memory::IAllocator* alloc, crd::u32 nrelax = kSupernodeRelax, crd::u32 num_workers = 1);

} // namespace crd::hesap::direct
