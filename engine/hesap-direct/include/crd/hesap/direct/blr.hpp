#pragma once

#include <crd/containers/array.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

// =======================================================================
// Phase 3.1.6 v5e-3 — BLR (Block Low-Rank, Amestoy 2015 / MUMPS-BLR).
//
// FLAT block-low-rank, the production-default complement to the hierarchical
// HSS of v5e-1/2: a front is tiled into a FLAT grid of `block_size` blocks; the
// diagonal blocks stay dense, the off-diagonal blocks are compressed to low rank
// `u·vᵀ` INDEPENDENTLY (no nesting). Simpler + more robust than HSS, less
// asymptotic gain — but the MUMPS production workhorse for large 3D fronts.
//
// This file ships the v5e-3a SUBSTRATE: the symmetric BLR representation +
// `compress_blr_sym` (per-block interpolative-decomposition compression) +
// `blr_to_dense_sym` (reconstruction, for testing). The BLR factorization
// (block Cholesky/LDLᵀ with low-rank arithmetic) is v5e-3b+.
//
// Determinism: per-block compression of EXPLICIT data is a pure deterministic
// function (RRQR column-ID, no RNG) — naturally moat-safe, cleaner than HSS
// which needed the counter-RNG sample. The moat-sensitive operation is the
// low-rank update accumulation order in the factor (v5e-3c), not here.
//
// Lower layer: raw f32/f64 scalars (ADR-0078 §5).
// =======================================================================

namespace crd::hesap::direct
{

// One block of a BLR matrix: either DENSE (`dense` is rows×cols) or LOW-RANK
// (block ≈ u·vᵀ with u: rows×rank, v: cols×rank).
template <typename T>
struct BlrBlock
{
    bool is_lowrank = false;
    crd::usize rows = 0;
    crd::usize cols = 0;
    crd::usize rank = 0;
    crd::hesap::dense::Matrix<T> dense;  // rows×cols  (valid when !is_lowrank)
    crd::hesap::dense::Matrix<T> u;      // rows×rank  (valid when is_lowrank)
    crd::hesap::dense::Matrix<T> v;      // cols×rank  (valid when is_lowrank)

    explicit BlrBlock(crd::memory::IAllocator* alloc) noexcept : dense(alloc), u(alloc), v(alloc) {}
    BlrBlock(BlrBlock&&) noexcept = default;
    BlrBlock& operator=(BlrBlock&&) noexcept = default;
    BlrBlock(const BlrBlock&) = delete;
    BlrBlock& operator=(const BlrBlock&) = delete;
};

// A symmetric BLR matrix: only the LOWER block-triangle (i ≥ j) is stored;
// diagonal blocks are dense, the (i > j) blocks are compressed. The upper
// triangle is the transpose (symmetric).
template <typename T>
struct BlrMatrix
{
    crd::usize n = 0;                            // overall dimension
    crd::usize nb = 0;                           // number of blocks per dimension
    crd::containers::Array<crd::usize> bstart;   // nb+1 block boundaries (bstart[nb] == n)
    crd::containers::Array<BlrBlock<T>> blocks;  // nb×nb row-major grid; only i ≥ j populated

    explicit BlrMatrix(crd::memory::IAllocator* alloc) noexcept : bstart(alloc), blocks(alloc) {}
    BlrMatrix(BlrMatrix&&) noexcept = default;
    BlrMatrix& operator=(BlrMatrix&&) noexcept = default;
    BlrMatrix(const BlrMatrix&) = delete;
    BlrMatrix& operator=(const BlrMatrix&) = delete;

    [[nodiscard]] crd::usize block_dim(crd::usize i) const noexcept { return bstart[i + 1] - bstart[i]; }
    [[nodiscard]] BlrBlock<T>& at(crd::usize i, crd::usize j) noexcept { return blocks[i * nb + j]; }
    [[nodiscard]] const BlrBlock<T>& at(crd::usize i, crd::usize j) const noexcept { return blocks[i * nb + j]; }
};

// compress_blr_sym — tile the symmetric matrix A (its LOWER triangle is read)
// into a BLR matrix with block size ~`block_size`. Diagonal blocks are kept
// dense; each (i > j) off-diagonal block is compressed via `interp_decomp`
// (column ID, rcond = `tol`) and stored low-rank IFF that saves storage
// (rank·(rows+cols) < rows·cols), else kept dense. `tol` bounds the per-block
// reconstruction error ‖A_ij − u·vᵀ‖ ≲ tol·‖A_ij‖. Real f32/f64.
template <typename T>
[[nodiscard]] BlrMatrix<T> compress_blr_sym(crd::memory::IAllocator* alloc, const crd::hesap::dense::Matrix<T>& a,
                                            crd::usize block_size, crd::hesap::dense::RealType<T> tol);

// blr_to_dense_sym — reconstruct the full dense symmetric matrix (both triangles)
// from a symmetric BLR matrix. For validation. Real f32/f64.
template <typename T>
[[nodiscard]] crd::hesap::dense::Matrix<T> blr_to_dense_sym(crd::memory::IAllocator* alloc, const BlrMatrix<T>& b);

// blr_cholesky_factor — BLR Cholesky (v5e-3b, FSCU simple variant). Factor the
// dense SPD matrix `a` (its LOWER triangle is read) into a BLR lower-triangular
// factor L with A ≈ L·Lᵀ: the diagonal L blocks are kept dense, the off-diagonal
// L blocks are compressed to low rank (rcond = `tol`). This SIMPLE pass computes
// the factor densely then compresses L (compress-for-memory, dense update); the
// flop-saving LR×LR factor update + recompression is v5e-3c. Returns false on a
// non-positive pivot (A not numerically SPD). Real f32/f64.
template <typename T>
[[nodiscard]] bool blr_cholesky_factor(crd::memory::IAllocator* alloc, const crd::hesap::dense::Matrix<T>& a,
                                       crd::usize block_size, crd::hesap::dense::RealType<T> tol, BlrMatrix<T>& l_out);

// blr_cholesky_solve — solve L·Lᵀ·x = b (single RHS, in place: `x` holds b on
// entry, the solution on exit) using a BLR Cholesky factor `l` from
// blr_cholesky_factor. The off-diagonal low-rank blocks are applied as `u·(vᵀ··)`
// (forward) / `v·(uᵀ··)` (backward) — O(block·rank), the BLR solve win. Real f32/f64.
template <typename T>
void blr_cholesky_solve(const BlrMatrix<T>& l, T* x) noexcept;

namespace detail
{
// low_rank_recompress (v5e-3c, the LUAR core) — recompress a low-rank matrix
// `uc·vcᵀ` (uc: m×rc, vc: n×rc) to rank r ≤ rc with relative truncation `tol`:
// outputs u_out (m×r), v_out (n×r), rank_out=r, with ‖uc·vcᵀ − u·vᵀ‖ ≲ tol·‖uc·vcᵀ‖.
// Method: QR(uc)=Qu·Ru, QR(vc)=Qv·Rv, SVD the TINY rc×rc `Ru·Rvᵀ`, truncate, then
// u = Qu·U_svd·Σ, v = Qv·V_svd (implicit-Q applies). Falls back to a dense ID when
// rc ≥ min(m,n) (the QR-trick gives no gain). Deterministic ⇒ moat-safe. Real f32/f64.
template <typename T>
void low_rank_recompress(crd::memory::IAllocator* alloc, const crd::hesap::dense::Matrix<T>& uc,
                         const crd::hesap::dense::Matrix<T>& vc, crd::hesap::dense::RealType<T> tol,
                         crd::usize max_rank, crd::hesap::dense::Matrix<T>& u_out,
                         crd::hesap::dense::Matrix<T>& v_out, crd::usize& rank_out);
} // namespace detail

// factor_front_cholesky_dense — partial DENSE Cholesky of a symmetric front (the
// fast BLAS-3 path for fronts below the BLR threshold, or as the non-compressed
// baseline). In place on `front` (m×m, RowMajor, lower): factor the leading `npiv`
// pivots = dpotrf(A11) + dtrsm(L21) + dsyrk/gemm(A22 −= L21·L21ᵀ) — three BLAS-3
// kernels (≈50 GF/s) instead of the naive scalar loops. No compression (no
// interp_decomp). Returns false on a non-positive pivot. Real f32/f64.
template <typename T>
[[nodiscard]] bool factor_front_cholesky_dense(crd::memory::IAllocator* alloc, crd::hesap::dense::Matrix<T>& front,
                                               crd::usize npiv);

// factor_front_cholesky_blr — partial BLR Cholesky of a dense symmetric FRONT
// (v5e-3d, the multifrontal driver's core kernel). In place on `front` (m×m,
// RowMajor, lower triangle read): factor the leading `npiv` fully-summed Cholesky
// pivots, leaving L in columns [0,npiv) (L11 npiv×npiv lower + L21 (m−npiv)×npiv)
// and the dense Schur complement S = A22 − L21·L21ᵀ in the trailing
// [npiv,m)×[npiv,m) lower block. The elimination runs in BLR arithmetic (compress +
// LR-preserving TRSM + LR×LR updates with recompression, through the fast gemm) but
// the L panel + Schur are written back DENSE (advisor: dense Schur first), so the
// multifrontal driver's extend-add + L-extraction are unchanged. The block grid is
// aligned so `npiv` is a hard boundary. Returns false on a non-positive pivot.
// Real f32/f64. The flop-saving win lives here for large fronts (gate: 2.33× at 4096).
template <typename T>
[[nodiscard]] bool factor_front_cholesky_blr(crd::memory::IAllocator* alloc, crd::hesap::dense::Matrix<T>& front,
                                             crd::usize npiv, crd::usize block_size,
                                             crd::hesap::dense::RealType<T> tol);

// blr_cholesky_factor_lr — BLR-arithmetic Cholesky (v5e-3c, the FLOP-SAVING FSCU).
// Unlike blr_cholesky_factor (dense factor, compress-for-memory), this factors the
// COMPRESSED front directly: LR-preserving panel solves + LR×LR Schur updates with
// recompression (detail::low_rank_recompress) — O(block·rank²) per off-diagonal
// update instead of O(block²·col). The Schur updates accumulate in FIXED k-ascending
// order ⇒ deterministic (moat-safe). Same BLR L output + `blr_cholesky_solve`-able.
// Returns false on a non-positive pivot. Real f32/f64.
template <typename T>
[[nodiscard]] bool blr_cholesky_factor_lr(crd::memory::IAllocator* alloc, const crd::hesap::dense::Matrix<T>& a,
                                          crd::usize block_size, crd::hesap::dense::RealType<T> tol,
                                          BlrMatrix<T>& l_out);

} // namespace crd::hesap::direct
