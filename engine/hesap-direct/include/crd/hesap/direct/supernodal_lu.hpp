#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// =======================================================================
// v5b-2 — Supernodal LU (SuperLU-class) with MC64 + threshold STATIC pivoting.
// THE crush + the cross-thread determinism moat. Validated against the v5b-1
// Gilbert-Peierls oracle (sparse_lu.hpp).
//
// THE MOAT THROUGH PIVOTING (the design crux): v5b-1's dynamic partial pivot is
// order-dependent ⇒ a parallel factorization would pick a dispatch-dependent
// pivot sequence ⇒ NOT bit-deterministic. v5b-2 uses MC64 + threshold STATIC
// pivoting (SuperLU_DIST): MC64 (`ordering::mc64_match_and_scale`, v4j-1a)
// permutes the max-weight matching onto the diagonal AND scales toward an
// I-matrix (matched |diagonal| = 1, all |off-diagonals| ≤ 1) ⇒ the diagonal is
// the column max ⇒ a STATIC diagonal pivot is stable in (nearly) every column
// ⇒ the pivot sequence is fixed by the symbolic + MC64 phase, NOT dynamically
// ⇒ the numeric factorization is bit-deterministic across {1,2,4,8} workers.
// Static pivoting trades a little stability ⇒ recovered by iterative refinement
// (v5b-2, later); a tiny pivot gets a deterministic √ε·‖A‖ perturbation.
//
// BUILD ORDER (cluster; each sub-slice measured + verified vs the v5b-1 oracle):
//   v5b-2a  this file — the deterministic FRONT-END: MC64 match+scale transform
//           B = perm_cols(D_r·A·D_c) (matched on diagonal) + the solve transform
//           (b→c, y→x). Validated by factoring B with the v5b-1 GP-LU + checking
//           the residual on the ORIGINAL A — a wrong transform fails the residual,
//           so the composition is self-verifying. Also reports the static-pivot
//           amenability metric (min over columns of |B(j,j)|/max_i|B(i,j)|; → 1
//           ⇒ static pivot is exact). [AMD column reorder + the supernodal numeric
//           + parallel + the moat land in v5b-2b..e.]
// =======================================================================

// The MC64 static-pivot transform: A·x = b ⇔ B·y = c, where
//   B = (D_r·A·D_c) with columns permuted so the matched entry is on the diagonal,
//   c[i]            = d_row[i] · b[i],
//   x[col_match[j]] = d_col[col_match[j]] · y[j].
// `col_match[j]` = the original column that becomes new column j (MC64's match).
template <typename T> struct StaticLuScaling
{
    crd::containers::Array<crd::u32> col_match; // new col j ← original col col_match[j] (matched to diagonal)
    crd::containers::Array<crd::f64> d_row;     // row scaling D_r (length n)
    crd::containers::Array<crd::f64> d_col;     // column scaling D_c (length n)
    // Post-MC64 fill-reducing SYMMETRIC reorder P (AMD on B+Bᵀ): B' = P·B·Pᵀ keeps the matched diagonal
    // (symmetric perm) and cuts fill MC64's matching permutation destroyed (SuperLU_DIST pipeline).
    // perm[i] = the B-index at B'-slot i; inv_perm[perm[i]] = i. Identity if no reorder.
    crd::containers::Array<crd::u32> perm;
    crd::containers::Array<crd::u32> inv_perm;
    double min_diag_dominance = 0.0; // min_j |B(j,j)| / max_i|B(i,j)| (1 = matched-exact, static-safe)
    bool full_rank = true;           // false if MC64 found no perfect matching

    explicit StaticLuScaling(crd::memory::IAllocator* alloc)
        : col_match(alloc), d_row(alloc), d_col(alloc), perm(alloc), inv_perm(alloc)
    {
    }

    // c = transform of the RHS b (in place not required — separate buffers). Caller sizes c to n.
    void transform_rhs(crd::containers::ConstSpan<T> b, crd::containers::Span<T> c) const;
    // x = untransform of the solved y (B·y = c ⇒ A·x = b). Caller sizes x to n.
    void untransform_solution(crd::containers::ConstSpan<T> y, crd::containers::Span<T> x) const;
};

// Build the MC64 static-pivot transform of A (CSR). Writes the transformed matrix
// B = perm_cols(D_r·A·D_c) into `out_b` (CSC, matched entries on the diagonal) and
// returns the scaling/match (+ the static-pivot amenability metric). The v5b-2
// supernodal numeric factors B with a STATIC diagonal pivot; for v5b-2a, factor B
// with the v5b-1 `factor_gp_lu` to validate the transform end-to-end.
// `use_mc64` (default true): true ⇒ MC64 matches the max-weight transversal onto the diagonal + scales toward
// an I-matrix (stable static pivoting on any matrix). false ⇒ the NATURAL diagonal (identity match + unit
// scaling) — still deterministic ⇒ the moat holds; faster + more accurate on strong-diagonal systems
// (CFD/saddle-point), but element growth must be watched (MultifrontalLU falls back to MC64 if it blows up).
// The moat is STATIC pivoting, NOT MC64; MC64 is only a stability aid.
template <typename T>
[[nodiscard]] StaticLuScaling<T> static_lu_prepare(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                                   sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& out_b,
                                                   crd::memory::IAllocator* alloc, bool use_mc64 = true);

// Iterative-refinement step cap (Demmel-Li GESP): static pivoting trades a little stability,
// recovered by refining B·y = c on the residual. Capped + early-exit on a tight residual (so well-
// conditioned systems still stop at 1-2 steps). The AMD reorder can change the static-pivot
// conditioning ⇒ a higher cap keeps the crushes at MATCHED machine-precision residual (honest bench).
inline constexpr crd::u32 kLuRefineMax = 8;

// =======================================================================
// SupernodalLU<T> — the deterministic + (v5b-2d) parallel sparse LU. v5b-2c
// builds the SERIAL numeric over the v5b-2b static-pivot symbolic structure:
//   factor: A (CSR) → [v5b-2a] MC64 → B (CSC, matched on diagonal) + D_r/D_c/perm
//                   → [v5b-2b] lu_symbolic(B) → exact static L/U structure
//                   → [v5b-2c] left-looking NUMERIC, STATIC diagonal pivot (no
//                     interchange — MC64 made it safe; tiny pivot → deterministic
//                     √ε·‖B‖ perturbation, Demmel-Li GESP) ⇒ the pivot sequence is
//                     fixed by the symbolic phase ⇒ bit-deterministic (v5b-2d moat).
//   solve:  A·X = B  via  c = D_r·b → B·y = c → iterative refinement (matched true
//           residual; static pivot vs the partial-pivoting peers) → x = D_c·y (perm).
//
// v5b-2c step 1 (this build) is a column-by-column left-looking numeric over the
// precomputed L/U pattern (the correct, banked reference); step 2 swaps in the
// supernodal BLAS-3 panels (the crush kernel) behind this same interface.
// =======================================================================
template <typename T> class SupernodalLU final : public IFactorization<T>
{
public:
    explicit SupernodalLU(crd::memory::IAllocator* alloc) noexcept;

    // Numeric factorization of a general square unsymmetric matrix A (CSR). `num_workers`
    // is reserved for the v5b-2d tree-parallel numeric (serial in v5b-2c).
    void factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a, crd::u32 num_workers = 1);

    [[nodiscard]] bool solve(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    using IFactorization<T>::solve; // un-hide the single-RHS convenience overload
    // v5f: RAW factor apply (NO internal IR) — the mixed-precision IR driver's building block. Like
    // MultifrontalLU, this static-pivot LU's solve() runs internal GESP refinement + an accept-gate, so the
    // base default (apply_inverse = solve) would nest IR under an outer working-precision IR; override it.
    void apply_inverse(crd::containers::Span<T> rhs, crd::usize nrhs) const override;
    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_lnz + m_unz; }
    [[nodiscard]] crd::usize info() const noexcept override { return m_info; }

    // Read-only factor-structure views (CSC, identity row space — static pivot ⇒ P = I).
    // Used by the v5b-2c numeric oracle tests + the v5b-2e bench fill metric.
    [[nodiscard]] crd::u64 l_nnz() const noexcept { return m_lnz; }
    [[nodiscard]] crd::u64 u_nnz() const noexcept { return m_unz; }
    [[nodiscard]] crd::u32 supernode_count() const noexcept { return m_nsuper; }
    // Factor VALUE spans — for the v5b-2d cross-thread determinism moat test (bit-identical L,U
    // across {1,2,4,8} workers). The numeric is a deterministic pure function of the symbolic.
    [[nodiscard]] crd::containers::ConstSpan<T> l_values() const noexcept { return {m_lx.data(), m_lx.size()}; }
    [[nodiscard]] crd::containers::ConstSpan<T> u_values() const noexcept { return {m_ux.data(), m_ux.size()}; }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::u32 m_n = 0;
    crd::usize m_info = 0; // 0 = ok; k+1 = structurally/numerically singular at column k
    crd::u64 m_lnz = 0;
    crd::u64 m_unz = 0;
    StaticLuScaling<T> m_scale;                             // MC64 transform (v5b-2a)
    sparse::SparseMatrix<T, sparse::SparseFormat::Csc> m_b; // B = transformed matrix (kept for IR residual)
    crd::containers::Array<crd::u32> m_lp;                  // L column pointers, length n+1
    crd::containers::Array<crd::u32> m_li;                  // L row indices (ascending, unit diagonal first)
    crd::containers::Array<T> m_lx;                         // L values (unit diagonals stored as 1)
    crd::containers::Array<crd::u32> m_up;                  // U column pointers, length n+1
    crd::containers::Array<crd::u32> m_ui;                  // U row indices (ascending, diagonal last)
    crd::containers::Array<T> m_ux;                         // U values
    crd::containers::Array<crd::u32> m_super;               // supernode boundaries (v5b-2c step 2 / v5b-2d)
    crd::u32 m_nsuper = 0;
};

// Factor a general square unsymmetric matrix A (CSR) into a deterministic static-pivot LU.
// info() != 0 ⇒ singular. num_workers reserved for v5b-2d (serial in v5b-2c).
template <typename T>
[[nodiscard]] SupernodalLU<T> factor_supernodal_lu(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                                   crd::memory::IAllocator* alloc, crd::u32 num_workers = 1);

} // namespace crd::hesap::direct
