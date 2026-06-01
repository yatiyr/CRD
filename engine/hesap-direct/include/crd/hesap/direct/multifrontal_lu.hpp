#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/dense_lu_kernels.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/direct/lu_symbolic.hpp>
#include <crd/hesap/direct/supernodal_lu.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// =======================================================================
// v5b-3a — MULTIFRONTAL LU SYMBOLIC front structure (the assembly tree).
//
// Crush target: UMFPACK beats Cerid's left-looking supernodal LU (and Eigen's,
// both supernodal) ~2.6x on CFD factor via DENSE FRONTAL matrices + a delayed
// rank-nb BLAS-3 flush. v5b-3 builds that multifrontal numeric; v5b-3a is the
// structural skeleton. Design: docs/research/cerid-hesap-v5b-3-multifrontal-lu.md.
//
// THE MOAT SIMPLIFICATION: UMFPACK DISCOVERS the front structure during the
// numeric (dynamic pivoting). With MC64 STATIC pivoting (SuperLU_DIST model) the
// L/U structure is known UP FRONT (LuSymbolic), so each front's row/col extent is
// DERIVED, not discovered — and the pivot order is fixed ⇒ deterministic numeric
// (the cross-thread bit-identity moat), exactly as v5a Cholesky.
//
// Front f = relaxed supernode f (LuSymbolic::super). Pivot columns Js =
// [pivot_first[f], pivot_first[f+1]).
//   rows[f] = the L row pattern of the supernode's leading column (a dense
//             trapezoid superset of every member column) = pivots + the L foot.
//   cols[f] = Js  ∪  {U columns the front's pivot rows touch} — one transpose
//             pass over LuSymbolic up/ui (U is CSC; a pivot row i touches column
//             k iff i in U(:,k)).
//   front_parent[f] = the front containing col_etree[last pivot col of f]
//             (kNoParent at a root). col_etree parents are always higher-indexed,
//             so ascending front order is a valid postorder (children before parents).
//
// The contribution block of front f (the Schur complement assembled into its
// parent) is rows[f]/cols[f] with the pivots removed: CB rows = rows[f] entries
// >= pivot_first[f+1], CB cols = cols[f] entries >= pivot_first[f+1].
// =======================================================================
struct MultifrontalSymbolic
{
    crd::u32 n = 0;
    crd::u32 nfront = 0;
    crd::containers::Array<crd::u32> pivot_first;  // length nfront+1; front f pivots = [pivot_first[f], [f+1])
    crd::containers::Array<crd::u32> front_parent; // length nfront; ordering::kNoParent at a root
    // Front row/column index sets, CSR-style, ascending global ids (the extend_add merge contract).
    crd::containers::Array<crd::u32> row_ptr; // length nfront+1
    crd::containers::Array<crd::u32> row_idx; // ascending global row ids, per front
    crd::containers::Array<crd::u32> col_ptr; // length nfront+1
    crd::containers::Array<crd::u32> col_idx; // ascending global col ids, per front

    explicit MultifrontalSymbolic(crd::memory::IAllocator* alloc)
        : pivot_first(alloc), front_parent(alloc), row_ptr(alloc), row_idx(alloc), col_ptr(alloc), col_idx(alloc)
    {
    }

    [[nodiscard]] crd::u32 npiv(crd::u32 f) const noexcept { return pivot_first[f + 1] - pivot_first[f]; }
};

// Build the multifrontal front structure from the materialised static-pivot LuSymbolic (UNSYMMETRIC-
// pattern fronts: rows from L, cols from U). Pure function of `sym` (deterministic). No numeric.
// NOTE (v5b-3a finding): these fronts are NOT assembly-safe for genuinely-unsymmetric matrices — a child
// CB can overlap an ANCESTOR (check_multifrontal_containment fails). Use the SYMMETRIC-PATTERN builder
// below for the multifrontal numeric; this stays for the containment experiment + analysis.
[[nodiscard]] MultifrontalSymbolic build_multifrontal_symbolic(const LuSymbolic& sym, crd::memory::IAllocator* alloc);

// v5b-3a SYMMETRIC-PATTERN (MUMPS-style) front structure — THE v5b-3 path. Fronts come from chol(B+Bᵀ)
// (the proven v5a symmetric symbolic, `ordering::symbolic_factorize`; `build_adjacency` symmetrises B
// internally, so pass B directly). Each front's row extent EQUALS its column extent (the symmetric
// supernode pattern); the unsymmetric L and U fill WITHIN it (the dense-front BLAS-3 numeric, v5b-3b).
// Containment then HOLDS by the Cholesky theorem ⇒ child→parent extend_add is valid (reusing the v5a
// multifrontal assembly + determinism moat), and the symmetrised fill is ~free on the near-structurally-
// symmetric CFD/FEM/NS sim targets (measured 1.00–1.08×). `b` is the MC64+AMD-permuted CSC pattern.
[[nodiscard]] MultifrontalSymbolic build_symmetric_multifrontal_symbolic(const sparse::SparsePattern& b,
                                                                         crd::memory::IAllocator* alloc);

// v5b-3a CONTAINMENT CHECK (the gating verification). Does every child front's contribution
// block fit inside its parent front in BOTH dimensions? i.e. CB_rows(f) subset rows(parent)
// AND CB_cols(f) subset cols(parent). This is the precondition for single-extend_add-into-parent
// assembly (the v5a-1 extend_add ascending-subset contract). It is a THEOREM for symmetric
// Cholesky but NOT automatic for UNSYMMETRIC LU (the UMFPACK LUson/Lson/Uson split exists because
// a block can overlap an ancestor in rows-only or cols-only). If this is not green, the skeleton
// needs symmetric-closure fronts (pad to containment, paying fill) or true split assembly.
struct MfContainmentReport
{
    crd::u32 nfront = 0;
    crd::u32 nchild = 0;        // non-root fronts checked
    crd::u32 row_violations = 0; // child fronts whose CB rows are not subset parent rows
    crd::u32 col_violations = 0; // child fronts whose CB cols are not subset parent cols
    crd::u32 max_front_rows = 0; // largest front row extent (front-size sanity)
    crd::u32 max_front_cols = 0;
    [[nodiscard]] bool ok() const noexcept { return row_violations == 0 && col_violations == 0; }
};

[[nodiscard]] MfContainmentReport check_multifrontal_containment(const MultifrontalSymbolic& mf);

// =======================================================================
// v5b-3b-1 — the COL-MAJOR multifrontal front (the locked layout, § 4a of the design doc) + its
// assembly kernel. The numeric (v5b-3b-2 `factor_front`) factors a front's pivots with the col-major
// BLAS-3 kernels (`dense_lu_nopivot` / `dl::gemm` / `trsm_unit_lower_left`) reading this layout natively.
// =======================================================================

// Dense COL-MAJOR front: element (row i, col j) = data[j*nrows + i]. `row_index`/`col_index` are ASCENDING
// global ids (the extend_add merge + the determinism contract depend on it). The col-major twin of the
// row-major v5a-1 `Frontal<T>` (frontal.hpp); separate so the proven row-major type/tests stay untouched.
template <typename T>
struct MfFront
{
    crd::u32 nrows = 0;
    crd::u32 ncols = 0;
    crd::containers::Array<T> data;             // nrows * ncols, COL-MAJOR
    crd::containers::Array<crd::u32> row_index; // length nrows; ascending global row ids
    crd::containers::Array<crd::u32> col_index; // length ncols; ascending global col ids

    explicit MfFront(crd::memory::IAllocator* alloc) : data(alloc), row_index(alloc), col_index(alloc) {}

    void resize(crd::u32 r, crd::u32 c)
    {
        nrows = r;
        ncols = c;
        data.resize(static_cast<crd::usize>(r) * static_cast<crd::usize>(c));
        row_index.resize(r);
        col_index.resize(c);
    }

    void zero_fill()
    {
        for (crd::usize i = 0; i < data.size(); ++i)
        {
            data[i] = T{0};
        }
    }

    [[nodiscard]] T& at(crd::u32 i, crd::u32 j) noexcept
    {
        return data[static_cast<crd::usize>(j) * static_cast<crd::usize>(nrows) + i];
    }
    [[nodiscard]] const T& at(crd::u32 i, crd::u32 j) const noexcept
    {
        return data[static_cast<crd::usize>(j) * static_cast<crd::usize>(nrows) + i];
    }
};

// Col-major twin of frontal.hpp `extend_add`: scatter-add the child contribution block into the parent
// front — `parent.at(R[a], C[b]) += child.at(a, b)`. IDENTICAL ascending two-pointer row/col merge and
// the D(direct)-5 FIXED-postorder determinism contract (children extend-added in a fixed order ⇒ the
// parent front is thread-order-independent ⇒ the cross-thread moat); only the inner scatter is col-major.
// PRECONDITION: child.row_index ⊆ parent.row_index AND child.col_index ⊆ parent.col_index, both ascending
// — guaranteed for symmetric-pattern fronts by the v5b-3a containment result (the Cholesky theorem).
// `rmap`/`cmap` are CALLER-OWNED scratch (child-local row/col → parent-local), reused across every child
// of a front so the hot numeric walk does NOT allocate per extend-add (the v5b-3b assembly was ~50-70% of
// the numeric; per-call allocation + a row-major scatter over a col-major front were the cost). The
// scatter-add is col-OUTER / row-inner so the child column read `ccol[a]` is contiguous and the parent
// column base is hoisted once — and it is BIT-IDENTICAL to the row-major form: each child cell maps to a
// DISTINCT parent cell (rmap/cmap are injective), so the += order does not change any stored value (the
// FIXED-postorder determinism contract is across children, untouched).
template <typename T>
void mf_extend_add(MfFront<T>& parent, const MfFront<T>& child, crd::containers::Array<crd::u32>& rmap,
                   crd::containers::Array<crd::u32>& cmap)
{
    cmap.resize(child.ncols);
    rmap.resize(child.nrows);
    crd::u32 p = 0;
    for (crd::u32 b = 0; b < child.ncols; ++b)
    {
        const crd::u32 g = child.col_index[b];
        while (p < parent.ncols && parent.col_index[p] < g)
        {
            ++p;
        }
        CRD_ASSERT_MSG(p < parent.ncols && parent.col_index[p] == g,
                       "mf_extend_add: child column id absent from parent front (precondition violated)");
        cmap[b] = p;
    }
    crd::u32 pr = 0; // row pointer monotonic across child rows (both ascending)
    for (crd::u32 a = 0; a < child.nrows; ++a)
    {
        const crd::u32 g = child.row_index[a];
        while (pr < parent.nrows && parent.row_index[pr] < g)
        {
            ++pr;
        }
        CRD_ASSERT_MSG(pr < parent.nrows && parent.row_index[pr] == g,
                       "mf_extend_add: child row id absent from parent front (precondition violated)");
        rmap[a] = pr;
    }
    const crd::u32* rm = rmap.data();
    for (crd::u32 b = 0; b < child.ncols; ++b)
    {
        T* pcol = parent.data.data() + static_cast<crd::usize>(cmap[b]) * static_cast<crd::usize>(parent.nrows);
        const T* ccol = child.data.data() + static_cast<crd::usize>(b) * static_cast<crd::usize>(child.nrows);
        for (crd::u32 a = 0; a < child.nrows; ++a)
        {
            pcol[rm[a]] += ccol[a];
        }
    }
}

// Convenience overload (one-off callers / tests): allocates its own scratch from `scratch`.
template <typename T>
void mf_extend_add(MfFront<T>& parent, const MfFront<T>& child, crd::memory::IAllocator* scratch)
{
    crd::containers::Array<crd::u32> rmap(scratch);
    crd::containers::Array<crd::u32> cmap(scratch);
    mf_extend_add(parent, child, rmap, cmap);
}

// IN-PLACE (no-copy) extend-add: scatter `child`'s TRAILING Schur block — rows/cols [child_npiv, end) of the
// FULL factored child front `child` — into `parent`. The Schur is read in place from the child's own buffer
// (leading dim = child.nrows), so the driver never COPIES the Schur to a separate contribution-block buffer
// (UMFPACK's chain / in-place technique; umf_extend_front). BIT-IDENTICAL to copy-out-then-mf_extend_add:
// same Schur values, same injective (rmap,cmap) scatter ⇒ same parent front (the FIXED-postorder moat holds).
// PRECONDITION: child.row_index[child_npiv:] ⊆ parent.row_index AND child.col_index[child_npiv:] ⊆
// parent.col_index, both ascending (the v5b-3a containment theorem for symmetric-pattern fronts).
// `par` (default false): parallelize the column scatter across workers — each child Schur column `b` writes a
// DISTINCT parent column (`cmap` injective ⇒ disjoint parent columns) ⇒ race-free + BIT-IDENTICAL to serial,
// and children stay extend-added in fixed order ⇒ the cross-thread moat holds. Set ONLY on the main thread
// (the multifrontal narrow path); never inside a parallel_for over fronts (that would nest).
template <typename T>
void mf_extend_add_trailing(MfFront<T>& parent, const MfFront<T>& child, crd::u32 child_npiv,
                            crd::containers::Array<crd::u32>& rmap, crd::containers::Array<crd::u32>& cmap,
                            bool par = false)
{
    const crd::u32 sr = child.nrows - child_npiv; // Schur (contribution-block) dimension
    const crd::u32 sc = child.ncols - child_npiv;
    cmap.resize(sc);
    rmap.resize(sr);
    crd::u32 p = 0;
    for (crd::u32 b = 0; b < sc; ++b)
    {
        const crd::u32 g = child.col_index[child_npiv + b];
        while (p < parent.ncols && parent.col_index[p] < g)
        {
            ++p;
        }
        CRD_ASSERT_MSG(p < parent.ncols && parent.col_index[p] == g,
                       "mf_extend_add_trailing: child CB column id absent from parent (precondition violated)");
        cmap[b] = p;
    }
    crd::u32 pr = 0;
    for (crd::u32 a = 0; a < sr; ++a)
    {
        const crd::u32 g = child.row_index[child_npiv + a];
        while (pr < parent.nrows && parent.row_index[pr] < g)
        {
            ++pr;
        }
        CRD_ASSERT_MSG(pr < parent.nrows && parent.row_index[pr] == g,
                       "mf_extend_add_trailing: child CB row id absent from parent (precondition violated)");
        rmap[a] = pr;
    }
    // Scatter context bundled into one struct so the parallel_for closure is a single pointer (SBO-safe).
    struct EaScatter
    {
        T* pdata;
        const T* cdata;
        const crd::u32* rm;
        const crd::u32* cm;
        crd::usize pnr;
        crd::usize cld; // child front leading dim (Schur is a strided sub-block)
        crd::u32 child_npiv;
        crd::u32 sr;
        void run(crd::u32 b0, crd::u32 b1) const noexcept
        {
            for (crd::u32 b = b0; b < b1; ++b)
            {
                T* pcol = pdata + static_cast<crd::usize>(cm[b]) * pnr;
                const T* ccol = cdata + (static_cast<crd::usize>(child_npiv) + b) * cld + child_npiv;
                for (crd::u32 a = 0; a < sr; ++a)
                {
                    pcol[rm[a]] += ccol[a];
                }
            }
        }
    };
    const EaScatter ctx{parent.data.data(),        child.data.data(), rmap.data(),     cmap.data(),
                        static_cast<crd::usize>(parent.nrows), static_cast<crd::usize>(child.nrows), child_npiv, sr};
    // `jobs::num_workers()` is evaluated LAST (short-circuit) so the serial path (par == false — e.g. the
    // one-shot CLI factor with no jobs::init) never touches the job system.
    if (par && sc >= 256 && crd::jobs::num_workers() > 1)
    {
        crd::jobs::Counter* c = crd::jobs::parallel_for(
            sc, crd::jobs::num_workers(), [&ctx](crd::u32 b0, crd::u32 b1) noexcept { ctx.run(b0, b1); });
        crd::jobs::wait(c);
    }
    else
    {
        ctx.run(0, sc);
    }
}

// =======================================================================
// v5b-3b-3 — MultifrontalLU<T> : the symmetric-pattern (MUMPS-style) multifrontal LU.
// THE crush vs UMFPACK on CFD/NS factor: assemble DENSE col-major fronts (from chol(B+Bᵀ)
// supernodes) and factor each with the rank-nb BLAS-3 `factor_front` (vs the supernodal
// left-looking skinny cmod GEMMs). Same MC64 static-pivot front-end + IR solve as SupernodalLU
// ⇒ the same cross-thread bit-determinism moat (fronts/pivot order fixed by the symbolic phase).
//
//   factorize: A (CSR) → [MC64+AMD] static_lu_prepare → B (CSC, matched on diagonal) + D_r/D_c/perm
//                      → build_symmetric_multifrontal_symbolic(B) = chol(B+Bᵀ) fronts
//                      → postorder front walk: assemble B's entries + children's contribution blocks
//                        (extend-add) into a dense col-major front, factor_front (static pivot+GESP),
//                        emit the Schur CB to the parent, store the pivot L cols / U rows into CSC.
//   solve:     reuses the SAME transform + lu_lu_solve + stagnation-fixed IR as SupernodalLU
//              (L/U are CSC, diagonal-first L / diagonal-last U).
//
// The L/U fill = the symmetric-pattern (relaxed-supernode) fill (measured ~free on the CFD/FEM/NS
// sim targets, 1.00–1.08x; design doc § v5b-3a SYMMETRIZED-FILL). Selected by a dispatcher for the
// structured/CFD regime; circuit matrices keep the supernodal/scalar path (already wins there).
// =======================================================================
template <typename T> class MultifrontalLU final : public IFactorization<T>
{
public:
    explicit MultifrontalLU(crd::memory::IAllocator* alloc) noexcept;

    // Numeric factorization of a general square unsymmetric matrix A (CSR). Tree-parallel over the
    // assembly tree; the result is bit-identical across worker counts (the determinism moat). Uses ADAPTIVE
    // static pivoting: tries the natural diagonal first (faster + better-conditioned on strong-diagonal sim
    // matrices) and falls back to MC64 only if element growth blows up — both deterministic ⇒ moat-safe.
    void factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a, crd::u32 num_workers = 1);

    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    using IFactorization<T>::solve; // un-hide the single-RHS convenience overload
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_lnz + m_unz; }
    [[nodiscard]] crd::usize info() const noexcept override { return m_info; }

    [[nodiscard]] crd::u64 l_nnz() const noexcept { return m_lnz; }
    [[nodiscard]] crd::u64 u_nnz() const noexcept { return m_unz; }
    [[nodiscard]] crd::u32 front_count() const noexcept { return m_nfront; }
    // Factor VALUE spans — for the cross-thread determinism moat test (bit-identical L,U across
    // {1,2,4,8} workers). The numeric is a deterministic pure function of the symbolic.
    [[nodiscard]] crd::containers::ConstSpan<T> l_values() const noexcept { return {m_lx.data(), m_lx.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<T> u_values() const noexcept { return {m_ux.data(), m_ux.size()}; }

private:
    // One factorization attempt with a fixed pivoting choice (use_mc64). Writes L/U/scale into the members
    // and RETURNS the element-growth ratio max|stored L,U| / ‖B‖ — the caller (factorize) re-runs with MC64
    // if the natural-diagonal attempt's growth exceeds the stability threshold. Assumes m_n > 0.
    [[nodiscard]] double factor_attempt(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                        crd::u32 num_workers, bool use_mc64);

    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_n = 0;
    crd::usize m_info = 0; // 0 = ok; k+1 = structurally/numerically singular
    crd::u64 m_lnz = 0;
    crd::u64 m_unz = 0;
    crd::u32 m_nfront = 0;
    StaticLuScaling<T> m_scale;                             // MC64 transform (shared with SupernodalLU)
    sparse::SparseMatrix<T, sparse::SparseFormat::Csc> m_b; // B = transformed matrix (kept for IR residual)
    crd::containers::Array<crd::u32> m_lp;                  // L column pointers, length n+1
    crd::containers::Array<crd::u32> m_li;                  // L row indices (ascending, unit diagonal first)
    crd::containers::Array<T> m_lx;                         // L values (unit diagonals stored as 1)
    crd::containers::Array<crd::u32> m_up;                  // U column pointers, length n+1
    crd::containers::Array<crd::u32> m_ui;                  // U row indices (ascending, diagonal last)
    crd::containers::Array<T> m_ux;                         // U values
};

// Factor a general square unsymmetric matrix A (CSR) into a deterministic static-pivot
// multifrontal LU. info() != 0 ⇒ singular. num_workers reserved for v5b-3c (serial in v5b-3b).
template <typename T>
[[nodiscard]] MultifrontalLU<T> factor_multifrontal_lu(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                                       crd::memory::IAllocator* alloc, crd::u32 num_workers = 1);

} // namespace crd::hesap::direct
