#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// =======================================================================
// v5c — Multifrontal QR (SuiteSparseQR / SPQR-class, Davis Algorithm 915).
// The sparse-direct QR twin of v5a (Cholesky) / v5b (LU): A·P_c = Q·R.
//
// DESIGN (research dossier docs/research/cerid-hesap-v5c-multifrontal-qr.md):
// the QR work is a tree of DENSE Householder QRs on FRONTAL matrices. The
// frontal tree is the column elimination tree of A (= the etree of chol(AᵀA)),
// and the per-front column pattern is the chol(AᵀA) supernode structure —
// George-Heath-Ng: struct(R(A)) ⊆ struct(chol(AᵀA)) (equality iff strong-Hall).
//
// So a QR front == a chol(AᵀA) supernode. v5c REUSES the PROVEN v5a supernodal
// symbolic machinery: form the structural pattern of AᵀA, run the hardened
// `symbolic_factorize` + `build_supernodal_symbolic` on it — that yields the
// frontal tree, the front partition, and each front's column set in one shot.
// (Correctness-first: AᵀA is formed explicitly here; the perf sub-slice
// switches to the implicit leftmost-merge that never forms AᵀA, like SPQR.)
//
// v5c-1a (THIS slice): the SYMBOLIC only — front structure + the row→front
// merge. v5c-1b adds the numeric (per-front dense Householder QR + extend_add),
// v5c-1c the square + least-squares solve, v5c-1d perf + bench vs SPQR/Eigen,
// v5c-2 rank-revealing + complex + CLI.
// =======================================================================

inline constexpr crd::u32 kQrFrontRelax = 8; // relaxed front amalgamation (BLAS-3 lever; v5a nrelax analogue)

// Multifrontal-QR symbolic structure of an m×n matrix A (CSC pattern, m ≥ n
// for least-squares; square allowed). Frozen contract consumed by v5c-1b+.
//
//   * `fronts` — the QR fronts == chol(AᵀA) amalgamated supernodes (reuses the
//     v5a `SupernodalSymbolic`):
//        front f owns PIVOT columns [fronts.scol[f], fronts.scol[f+1]);
//        its full COLUMN set is fronts.srow[fronts.srowp[f] .. fronts.srowp[f+1])
//        (ascending global column ids; the first nc = scol[f+1]-scol[f] of them
//        are the pivot columns = the R diagonal block, the rest are the
//        non-pivotal "contribution-block" columns passed up to the parent).
//        (`SupernodalSymbolic` names the axis `srow`; for QR it holds R columns,
//        the chol(AᵀA) supernode pattern — same integers, transposed meaning.)
//   * `front_parent` / `front_post` — the assembly tree + its postorder (the
//     numeric walks fronts in `front_post`; extend_add into `front_parent[f]`).
//   * `sleft` / `row_by_leftcol` — the multifrontal ROW MERGE: a row of A enters
//     the front owning its LEFTMOST column. `sleft` (length n+1) indexes
//     `row_by_leftcol` by leftmost column j; front f directly assembles the
//     A-rows row_by_leftcol[sleft[scol[f]] .. sleft[scol[f+1]]). Empty rows
//     (no nonzero) belong to no front and are counted in `n_empty_rows`.
struct QrSymbolic
{
    crd::u32 m = 0; // rows of A
    crd::u32 n = 0; // cols of A

    SupernodalSymbolic fronts; // chol(AᵀA) supernodes (front column structure + col→front map)

    crd::containers::Array<crd::u32> front_parent; // length nf; kNoParent for roots
    crd::containers::Array<crd::u32> front_post;    // length nf; postorder of the front tree

    crd::containers::Array<crd::u32> sleft;          // length n+1; CSR offsets into row_by_leftcol by leftmost col
    crd::containers::Array<crd::u32> row_by_leftcol; // length (m - n_empty_rows); A row ids grouped by leftmost col
    crd::u32 n_empty_rows = 0;

    crd::u64 rblock_storage = 0; // Σ_f nc_f · |fn_f| — dense R-block area (the numeric's panel reserve)

    explicit QrSymbolic(crd::memory::IAllocator* alloc)
        : fronts(alloc), front_parent(alloc), front_post(alloc), sleft(alloc), row_by_leftcol(alloc)
    {
    }

    [[nodiscard]] crd::u32 nf() const noexcept { return fronts.nsuper; }
};

// Build the structural pattern of AᵀA (n×n symmetric CSC, full + diagonal) from
// A (m×n CSC pattern). Two-pass symbolic AᵀA = (Aᵀ)·A via row access (Gustavson).
// Correctness-first (the perf sub-slice avoids forming AᵀA via the leftmost
// merge); used by `multifrontal_qr_symbolic` and as a symbolic test oracle.
[[nodiscard]] sparse::SparsePattern ata_pattern(const sparse::SparsePattern& a_csc, crd::memory::IAllocator* alloc);

// Compute the multifrontal-QR symbolic of A (CSC pattern, m ≥ n recommended).
// `nrelax` is the relaxed front amalgamation budget (BLAS-3 lever; correctness-
// safe — bigger fronts only add explicit zeros). Deterministic pure function of
// A's pattern. A is assumed already fill-reduced (consumer applies amd_order on
// the AᵀA / COLAMD pattern; P_c lives at the consumer/solve boundary in v5c-1).
[[nodiscard]] QrSymbolic multifrontal_qr_symbolic(const sparse::SparsePattern& a_csc, crd::memory::IAllocator* alloc,
                                                  crd::u32 nrelax = kQrFrontRelax);

// =======================================================================
// MultifrontalQR<T> — the numeric factor A·P_c = Q·R (v5c-1b/c). Factor-once /
// solve-many. f32/f64 in v5c-1; complex Qᴴ at v5c-2.
//
// NUMERIC (v5c-1b): postorder front walk. Each front is a COLUMN-MAJOR dense
// fm×fn buffer assembled by SCATTER-PLACE (not the symmetric `extend_add`): the
// front's own A-rows (`row_by_leftcol`) scatter their values by column, then
// each child's contribution block (rows [npiv,fm) × cols [nc,fn)) is APPENDED as
// new rows, its columns (⊆ parent fn) scattered into the matching parent columns
// — the canonical assembly order (own rows, then children in front-tree order)
// that v5c-1c's Qᵀ re-walk reproduces, giving the determinism moat for free. A
// partial Householder QR eliminates the npiv=min(nc,fm) pivot columns → the top
// npiv rows are the R block (emitted to the global CSR R), the trailing
// rows×cols are the contribution block consumed by the parent. The stored front
// blocks (R upper + Householder vectors below) feed the v5c-1c implicit Qᵀ-apply
// + back-substitution solve.
//
// DETERMINISM (the QR moat): postorder walk + the fixed canonical row-stacking
// order + per-front-local Householder ⇒ R and the Householder vectors are
// bit-identical across worker counts (tree-parallel at v5c-1d). No
// SPQR/SparseQR carries cross-thread bit-exact factors.
// =======================================================================
template <typename T> class MultifrontalQR final : public IFactorization<T>
{
public:
    explicit MultifrontalQR(crd::memory::IAllocator* alloc) noexcept;

    // Numeric factorization of an m×n matrix A (CSC `pattern` + `values` parallel to
    // pattern.inner_idx). m ≥ n for least-squares; square allowed. A is assumed
    // full column rank in v5c-1 (rank-revealing is v5c-2).
    //
    // `num_workers`: cross-front tree-parallel factor (v5c). 0/1 ⇒ serial (no jobs dispatch); >1 ⇒
    // level-scheduled `parallel_for` over the assembly tree with per-worker scratch. The factor is
    // BIT-IDENTICAL across worker counts (the determinism moat — R + Householder vectors are a pure
    // function of the pattern, independent of how the front tree is scheduled).
    void factorize(const sparse::SparsePattern& pattern, crd::containers::ConstSpan<T> values,
                   crd::u32 nrelax = kQrFrontRelax, crd::u32 num_workers = 0);

    // Square solve A·X = B in place (asserts m == n): X = R⁻¹·(Qᵀ·B). v5c-1c.
    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    using IFactorization<T>::solve; // single-RHS convenience overload

    // Least-squares min‖A·X − B‖ for m ≥ n. `b` is column-major m×nrhs (input);
    // `x` receives the n×nrhs solution. v5c-1c.
    [[nodiscard]] bool least_squares(crd::containers::ConstSpan<T> b, crd::containers::Span<T> x,
                                     crd::usize nrhs) const;

    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }        // column count
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_rnnz + m_hnnz; }
    // 0 after a successful factor (rank-revealing v5c-2b: rank deficiency is NORMAL — query rank()/dead());
    // reserved for a genuine hard failure.
    [[nodiscard]] crd::usize info() const noexcept override { return m_info; }

    // Rank-revealing (v5c-2b, Heath: NO column pivoting ⇒ the fill order + the determinism moat hold).
    // A pivot column is DEAD when |R diagonal| ≤ rcond·max|R diagonal| (rcond = max(m,n)·eps) or when the
    // front could not pivot it (structural). `rank()` = #live columns; full column rank ⇔ rank()==n().
    // The least-squares solve returns the BASIC solution (dead variables = 0). `dead()` flags per column.
    [[nodiscard]] crd::usize rank() const noexcept { return m_rank; }
    [[nodiscard]] crd::containers::ConstSpan<crd::u8> dead() const noexcept { return {m_dead.data(), m_dead.size()}; }

    // Introspection (tests/benches): the global R in CSR (row r ascending columns,
    // upper-triangular) + dims.
    [[nodiscard]] crd::u32 rows() const noexcept { return m_m; }
    [[nodiscard]] const crd::containers::Array<crd::u32>& rp() const noexcept { return m_rp; }
    [[nodiscard]] const crd::containers::Array<crd::u32>& rj() const noexcept { return m_rj; }
    [[nodiscard]] const crd::containers::Array<T>& rx() const noexcept { return m_rx; }
    [[nodiscard]] crd::u64 r_nnz() const noexcept { return m_rnnz; }
    [[nodiscard]] const QrSymbolic& symbolic() const noexcept { return m_sym; }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_m = 0;
    crd::u32 m_n = 0;
    crd::usize m_info = 0; // 0 = ok; nonzero = rank deficiency / failure code
    QrSymbolic m_sym;

    // Per-front factored blocks (column-major fm_f × fn_f): R in the upper triangle of
    // the top npiv rows, the Householder vectors below the diagonal of the pivot columns.
    crd::containers::Array<T> m_fb;          // concatenated front blocks
    crd::containers::Array<crd::u32> m_fboff; // length nf+1; offsets into m_fb
    crd::containers::Array<crd::u32> m_fm;    // length nf; rows per front
    crd::containers::Array<crd::u32> m_npiv;  // length nf; reflectors per front
    crd::containers::Array<T> m_tau;          // concatenated taus
    crd::containers::Array<crd::u32> m_tauoff; // length nf+1

    // Front children (CSR over the front tree), for the assembly + the Qᵀ re-walk.
    crd::containers::Array<crd::u32> m_childp; // length nf+1
    crd::containers::Array<crd::u32> m_child;  // length nf

    // Global R, CSR (row r = pivot column r, ascending column ids, upper-triangular).
    crd::containers::Array<crd::u32> m_rp;
    crd::containers::Array<crd::u32> m_rj;
    crd::containers::Array<T> m_rx;
    crd::u64 m_rnnz = 0;
    crd::u64 m_hnnz = 0;

    // Rank-revealing (v5c-2b): per-column dead flag (length n; 1 = rank-deficient pivot) + numerical rank.
    crd::containers::Array<crd::u8> m_dead;
    crd::usize m_rank = 0;
};

// Convenience: factor A (CSC `pattern` + `values`) into a MultifrontalQR.
template <typename T>
[[nodiscard]] MultifrontalQR<T> factor_multifrontal_qr(const sparse::SparsePattern& pattern,
                                                       crd::containers::ConstSpan<T> values,
                                                       crd::memory::IAllocator* alloc,
                                                       crd::u32 nrelax = kQrFrontRelax, crd::u32 num_workers = 0);

} // namespace crd::hesap::direct
