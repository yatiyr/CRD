#pragma once

#include <crd/hesap/direct/multifrontal_lu.hpp> // MultifrontalSymbolic + build_symmetric_multifrontal_symbolic
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// =======================================================================
// v5d — Multifrontal LDLᵀ (symmetric INDEFINITE), the sparse-direct twin of v5a (Cholesky, SPD only) for
// matrices that are symmetric but NOT positive-definite (saddle-point / KKT systems, FE shells, contact).
// A = P·L·D·Lᵀ·Pᵀ with L unit-lower-triangular and D block-diagonal mixing 1×1 and 2×2 pivots
// (Bunch-Kaufman), so an indefinite/zero diagonal is handled where Cholesky's √ fails.
//
// STRUCTURE: the LDLᵀ front tree IS the chol(A) supernode tree (the SAME symmetric pattern v5a/v5b-3 use)
// — so v5d REUSES the proven v5b-3 symmetric multifrontal substrate wholesale: the symbolic
// (`build_symmetric_multifrontal_symbolic` → `MultifrontalSymbolic`, fronts = chol(A) supernodes, each
// front's row extent == its column extent), the col-major `MfFront<T>`, and `mf_extend_add` assembly
// (the Cholesky-theorem containment makes child→parent extend-add valid).
//
// The ONLY genuinely-new algorithm is the per-front INDEFINITE factor (1×1/2×2 Bunch-Kaufman, storing L +
// the block-diagonal D) + the determinism moat (within-front BK is a deterministic pure function of the
// front ⇒ L,D bit-identical across worker counts via the fixed postorder assembly, the same mechanism as
// v5a/v5b/v5c — no global static pivoting needed). Duff-Reid DELAYED PIVOTS (a front whose fully-summed
// block is singular/ill-conditioned pushes the pivot to its parent; MA57-class) are a robustness
// follow-on. v5d-b adds the front factor + driver; v5d-d the L·D·Lᵀ solve; v5d-e the tree-parallel moat;
// v5d-f complex-symmetric (LDLᵀ) + Hermitian-indefinite (LDLᴴ); v5d-g CLI `hesap.direct.ldlt.*`.
//
// v5d-a (THIS slice): the unit + the symbolic entry point + the structure validation. No numeric.
// =======================================================================

// LDLᵀ multifrontal symbolic of a symmetric square matrix `a` (CSC pattern; only the structure is read).
// Returns the chol(A) supernode front tree (`MultifrontalSymbolic`) — the SAME structure v5a Cholesky /
// v5b-3 LU use, since LDLᵀ has the symmetric Cholesky fill. Delegates to the proven
// `build_symmetric_multifrontal_symbolic` (which symmetrises A∪Aᵀ internally — for a symmetric A that is
// A's own pattern). Deterministic pure function of A's pattern; the assembly precondition
// (each child's contribution block ⊆ its parent front in BOTH dims) holds by the Cholesky theorem and is
// verifiable with `check_multifrontal_containment`.
// `relax` = the relaxed-front amalgamation parameter (collapse each maximal subtree with ≤ relax pivots into
// one front — the MUMPS-class lever vs tiny fundamental-supernode fronts). relax ≤ 1 ⇒ fundamental (no-op).
[[nodiscard]] MultifrontalSymbolic build_ldlt_symbolic(const sparse::SparsePattern& a, crd::memory::IAllocator* alloc,
                                                       crd::u32 relax = 32);

// =======================================================================
// v5d-c — symmetric lower-triangle in-place extend-add.
//
// Scatter the LOWER TRIANGLE of `child`'s trailing Schur block [child_npiv:, child_npiv:] into `parent`
// (both are symmetric lower-triangle col-major fronts). The child Schur is symmetric ⇒ its row id set ==
// its col id set; both ⊆ parent ids, both ascending ⇒ ONE monotone map (child-Schur-local → parent-local),
// and a ≥ b ⇒ map[a] ≥ map[b], so every lower-triangle child cell lands in a parent lower-triangle slot.
// Reads the Schur IN PLACE from the child buffer (leading dim = child.nrows) — no copy. `factor_front_ldlt`
// wrote only the lower triangle of the Schur, so we read only a ≥ b. BIT-IDENTICAL across worker counts
// (injective map ⇒ disjoint cells ⇒ the fixed-postorder determinism moat holds). `map` is caller-owned
// scratch reused across a front's children. The col-major / lower-triangle twin of `mf_extend_add_trailing`.
template <typename T>
void mf_extend_add_trailing_sym(MfFront<T>& parent, const MfFront<T>& child, crd::u32 child_npiv,
                                crd::containers::Array<crd::u32>& map)
{
    const crd::u32 s = child.nrows - child_npiv; // Schur dimension (rows == cols, symmetric front)
    map.resize(s);
    crd::u32 p = 0;
    for (crd::u32 a = 0; a < s; ++a)
    {
        const crd::u32 g = child.row_index[child_npiv + a];
        while (p < parent.nrows && parent.row_index[p] < g)
        {
            ++p;
        }
        CRD_ASSERT_MSG(p < parent.nrows && parent.row_index[p] == g,
                       "mf_extend_add_trailing_sym: child Schur id absent from parent (precondition violated)");
        map[a] = p;
    }
    const crd::usize cld = child.nrows;
    const T* cdata = child.data.data();
    T* pdata = parent.data.data();
    const crd::usize pnr = parent.nrows;
    for (crd::u32 b = 0; b < s; ++b)
    {
        T* pcol = pdata + static_cast<crd::usize>(map[b]) * pnr;
        const T* ccol = cdata + (static_cast<crd::usize>(child_npiv) + b) * cld + child_npiv;
        for (crd::u32 a = b; a < s; ++a) // LOWER triangle only (a >= b)
        {
            pcol[map[a]] += ccol[a];
        }
    }
}

// =======================================================================
// v5d-c — MultifrontalLDLT<T> : the symmetric-indefinite multifrontal driver.
//
// Factor A = P·L·D·Lᵀ·Pᵀ (A symmetric, CSC, lower triangle read) by a SERIAL postorder front walk:
// assemble A's lower triangle for each front's pivot columns + symmetric extend-add the children's Schur
// (mf_extend_add_trailing_sym) → factor the fully-summed pivots with the per-front Bunch-Kaufman kernel
// `factor_front_ldlt` (v5d-b) → store L (unit-lower multipliers, CSC, factor-position order) + D (block
// diagonal, kept SEPARATE from L: 1×1 value or 2×2 {d11,d21,d22}) + the block-local permutation P. P is the
// direct sum of each front's within-front BK swaps (which stay inside the front's contiguous pivot range
// [pivot_first[f], pivot_first[f+1]) — the v5d-a invariant), so P permutes only WITHIN fronts. L is stored
// keyed by global id during the walk, then remapped to factor-position order once P is complete (a CB row's
// factor position depends on its owning ancestor's BK swaps, known only after that ancestor is factored).
//
// Factors A AS GIVEN (no internal AMD — the consumer applies the fill-reducing order, like v5c QR). Serial +
// deterministic (the cross-thread moat is v5d-e: within-front BK is a pure function of the assembled front).
// REFUSE-ON-DELAY: if any front's fully-summed block cannot be fully eliminated within itself
// (`factor_front_ldlt` returns < npiv — a Duff-Reid delayed pivot, not yet supported), `info() != 0` and the
// factor is INVALID. v5d-d adds the L·D·Lᵀ solve (this slice validates the factor by reconstruction).
// =======================================================================
template <typename T> class MultifrontalLDLT final : public IFactorization<T>
{
public:
    explicit MultifrontalLDLT(crd::memory::IAllocator* alloc) noexcept;

    // Numeric LDLᵀ/LDLᴴ factorization of a symmetric matrix A (CSC; only the lower triangle, row >= col, is
    // read). num_workers > 1 enables the tree-parallel front walk (v5d-e); the result (L, D, perm) is
    // BIT-IDENTICAL across worker counts — the cross-thread determinism moat. `hermitian` (v5d-f, complex T
    // only — ignored for real T): false ⇒ A = P·L·D·Lᵀ·Pᵀ (complex-SYMMETRIC, unconjugated, D complex);
    // true ⇒ A = P·L·D·Lᴴ·Pᵀ (HERMITIAN-indefinite, conjugated, D real-diagonal). The caller asserts A is of
    // the matching kind (symmetric vs Hermitian) — only the lower triangle is read either way.
    void factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a, crd::u32 num_workers = 1,
                   bool hermitian = false);

    // Bunch-Kaufman pivot threshold (v5d-h indefinite-perf lever). Default = textbook (1+√17)/8 ≈ 0.64 (tightest
    // growth bound, most delays). A SMALLER value (MA57/MUMPS-style threshold pivoting, e.g. 0.1 or 0.01)
    // accepts weaker 1×1 pivots ⇒ far fewer Duff-Reid delays ⇒ no front blowup on indefinite fronts, at the cost
    // of looser element growth (validate via the solve residual). Set BEFORE factorize. Moat-safe (static).
    void set_pivot_threshold(double alpha) noexcept { m_pivot_alpha = alpha; }

    // Relaxed-front amalgamation parameter (v5d-h): collapse each maximal subtree with ≤ relax pivots into one
    // front (the MUMPS-class lever — fat BLAS-3 fronts vs tiny fundamental-supernode singletons). Set BEFORE
    // factorize. relax ≤ 1 ⇒ fundamental supernodes (no amalgamation). Deterministic ⇒ the moat holds.
    void set_amalgamation_relax(crd::u32 relax) noexcept { m_amalg_relax = relax; }

    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override; // v5d-d
    using IFactorization<T>::solve;                                                         // single-RHS overload
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_lnz + m_n; } // + the unit diagonal
    [[nodiscard]] crd::usize info() const noexcept override { return m_info; }

    [[nodiscard]] crd::u32 front_count() const noexcept { return m_nfront; }
    [[nodiscard]] bool hermitian() const noexcept { return m_hermitian; } // LDLᴴ (true) vs LDLᵀ (false)
    // LOWER-BOUND indicator of DELAYED pivots (Duff-Reid v5d-h): counts pivots eliminated at a non-owning
    // ancestor front (one per delayed block-start, so a 2×2 whose delayed member is the partner undercounts).
    // > 0 ⇒ the delayed-pivot path was genuinely exercised + is worker-count-invariant (the moat test asserts
    // both). Do NOT read as an exact delayed-pivot total.
    [[nodiscard]] crd::u32 delayed_count() const noexcept { return m_ndelay; }
    // Largest front dimension (fnr) seen — a delay-driven "front blowup" indicator (delayed columns fatten
    // ancestor fronts beyond their symbolic separator size). Diagnostic for the indefinite-perf grind.
    [[nodiscard]] crd::u32 max_front_dim() const noexcept { return m_max_front; }
    // Factor accessors — for the v5d-c reconstruction validation (and consumed by v5d-d's solve). All in
    // FACTOR-POSITION order. L is strictly-lower (unit diagonal implicit; for a 2×2 pivot L[k+1,k] is 0).
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> perm() const noexcept { return {m_perm.data(), m_perm.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> l_col_ptr() const noexcept { return {m_lp.data(), m_lp.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u32> l_row_idx() const noexcept { return {m_li.data(), m_li.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<T> l_values() const noexcept { return {m_lx.data(), m_lx.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<T> d_diag() const noexcept { return {m_dd.data(), m_dd.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<T> d_offdiag() const noexcept { return {m_doff.data(), m_doff.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> block_kinds() const noexcept
    {
        return {m_block_kinds.data(), m_block_kinds.size()};
    }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_n = 0;
    crd::u32 m_nfront = 0;
    crd::u32 m_ndelay = 0;     // count of pivots eliminated at an ancestor (Duff-Reid delayed pivots, v5d-h)
    crd::u32 m_max_front = 0;   // largest front dimension seen (delay-blowup diagnostic)
    // BK pivot threshold. Default 0.001: measured to drive Duff-Reid delays → ~0 (like MUMPS) on indefinite
    // 3D-FEM fronts ⇒ no front blowup ⇒ the indefinite-vs-MUMPS gap stops growing with n (stable ~1.7–1.9×,
    // the SPD-characterized gap, vs 6.9×-and-rising at 0.01). The looser element growth is recovered + verified
    // by the solve's iterative refinement + backward-error guard (accurate-or-flagged). Textbook BK = 0.64
    // (kBunchKaufmanAlpha); raise via set_pivot_threshold for ill-conditioned systems if IR fails to converge.
    double m_pivot_alpha = 0.001;
    // Relaxed-front amalgamation parameter (v5d-h) = CHOLMOD's nrelax0 (merge adjacent chain fronts with ≤ this
    // many combined cols unconditionally; bigger merges via the fill-aware zrelax test). Default 4 = CHOLMOD's
    // default — fat BLAS-3 fronts vs tiny fundamental-supernode singletons, fill-bounded (no OOM). 1 ⇒ off.
    crd::u32 m_amalg_relax = 4;
    crd::usize m_info = 0; // 0 = ok; else (unresolved-pivot global index + 1) — factor INVALID (singular)
    bool m_hermitian = false; // LDLᴴ (true) vs LDLᵀ (false); always false for real T
    crd::u64 m_lnz = 0;
    crd::containers::Array<crd::u32> m_lp;         // L column pointers, length n+1 (factor-position)
    crd::containers::Array<crd::u32> m_li;         // L row indices, ascending per column (factor-position)
    crd::containers::Array<T> m_lx;                // L values (strictly-lower multipliers; unit diag implicit)
    crd::containers::Array<T> m_dd;                // D diagonal: 1×1 value, or 2×2 d11 at k / d22 at k+1
    crd::containers::Array<T> m_doff;              // D sub-diagonal d21 at a 2×2 start k (else 0)
    crd::containers::Array<crd::u8> m_block_kinds; // 1 = 1×1 at k; 2 = 2×2 start; 0 = 2×2 continuation
    crd::containers::Array<crd::u32> m_perm;       // factor position -> global id (block-local BK permutation)
    // A's lower triangle (as given to factorize) — kept for iterative refinement: the relaxed pivot threshold
    // (default 0.01) trades element growth for speed, so the solve runs IR with a backward-error guard
    // (accurate-or-flagged) to recover/verify accuracy — the MA57/MUMPS design that makes the relaxed default safe.
    crd::containers::Array<crd::u32> m_a_outer;
    crd::containers::Array<crd::u32> m_a_inner;
    crd::containers::Array<T> m_a_values;
};

// Factor a symmetric matrix A (CSC) into a multifrontal LDLᵀ. info() != 0 ⇒ a delayed pivot was needed
// (Duff-Reid, not yet supported) or the matrix is singular — the factor is INVALID in that case.
template <typename T>
[[nodiscard]] MultifrontalLDLT<T> factor_multifrontal_ldlt(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a,
                                                           crd::memory::IAllocator* alloc, crd::u32 num_workers = 1,
                                                           bool hermitian = false);

} // namespace crd::hesap::direct
