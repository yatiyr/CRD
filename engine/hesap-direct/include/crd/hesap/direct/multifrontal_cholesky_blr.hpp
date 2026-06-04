#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/memory/allocator.hpp>

// =======================================================================
// Phase 3.1.6 v5e-3d — BLR-embedded MULTIFRONTAL CHOLESKY (the crush driver).
//
// A symmetric-pattern multifrontal Cholesky (reusing the v5b-3/v5d symbolic
// `build_symmetric_multifrontal_symbolic` = chol(A) supernode fronts) whose
// LARGE fronts are factored in BLR arithmetic (`factor_front_cholesky_blr`,
// v5e-3d core) and small fronts densely — the production-default BLR path that
// closes the gap to MUMPS-BLR at large 3D (n≥110K, where MUMPS-BLR beats
// MUMPS-full ~2×). The factor is APPROXIMATE (BLR truncates at `tol`); the
// solve recovers accuracy with iterative refinement + a backward-error-accept
// guard (the v5d-h discipline) — accurate-or-flagged, never silent garbage.
//
// Cholesky is the cleanest BLR target: no pivoting ⇒ a static block grid ⇒
// the per-block compression is a pure deterministic function (the moat is free,
// no counter-RNG). Factors A AS GIVEN (the caller applies the fill order, like
// v5a/v5c/v5d). Real f32/f64.
// =======================================================================

namespace crd::hesap::direct
{

template <typename T>
class MultifrontalCholeskyBlr
{
public:
    explicit MultifrontalCholeskyBlr(crd::memory::IAllocator* alloc) noexcept;

    // factorize A = L·Lᵀ (A symmetric positive-definite, CSC, AS GIVEN). Large fronts
    // (extent ≥ `blr_min`) use BLR arithmetic at threshold `tol`; small fronts dense.
    // Returns false (and sets info != 0) on a non-positive pivot.
    [[nodiscard]] bool factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& a,
                                 crd::usize block_size = 256,
                                 crd::hesap::dense::RealType<T> tol = static_cast<crd::hesap::dense::RealType<T>>(1e-6),
                                 crd::u32 blr_min = 512, crd::u32 num_workers = 1);

    // Solve A·x = b (single RHS, in place: `x` holds b on entry, solution on exit)
    // via L·Lᵀ + `max_ir` iterative-refinement steps. Returns false on an unfactored
    // or non-converged-beyond-tolerance system (backward-error guard).
    [[nodiscard]] bool solve(crd::containers::Span<T> x, crd::u32 max_ir = 30) const;

    [[nodiscard]] crd::u32 info() const noexcept { return m_info; }
    [[nodiscard]] crd::u32 n() const noexcept { return m_n; }
    [[nodiscard]] crd::u64 nnz() const noexcept { return m_lp.empty() ? 0 : m_lp[m_n]; }

private:
    using RT = crd::hesap::dense::RealType<T>;

    crd::memory::IAllocator* m_alloc;
    crd::u32 m_n = 0;
    crd::u32 m_info = 0;

    // L in CSC (lower triangle WITH the diagonal): column c = [m_lp[c], m_lp[c+1]).
    crd::containers::Array<crd::u32> m_lp;
    crd::containers::Array<crd::u32> m_li;
    crd::containers::Array<T> m_lx;

    // A's lower triangle in CSC (kept for the iterative-refinement residual A·x − b).
    crd::containers::Array<crd::u32> m_ap;
    crd::containers::Array<crd::u32> m_ai;
    crd::containers::Array<T> m_ax;

    void apply_a(const T* x, T* y) const noexcept;       // y = A·x (A symmetric, lower stored)
    void solve_llt(T* x) const noexcept;                 // L·Lᵀ·x = x (in place, no IR)
};

} // namespace crd::hesap::direct
