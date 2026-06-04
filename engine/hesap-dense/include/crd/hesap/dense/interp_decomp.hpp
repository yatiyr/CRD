#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v5e-1a — interpolative decomposition (column ID).
//
//     A  ≈  A[:, J] · Z   =   cols · proj
//
// J is a set of `rank` SKELETON column indices into A; `cols` is the m×rank
// matrix of those skeleton columns (gathered from the original A so the
// result reconstructs without re-reading A); `proj` is the rank×n
// interpolation matrix with the defining property  proj[:, J] = I_rank  (the
// skeleton columns are reproduced exactly), and every other column expressed
// as a combination of the skeleton.
//
// This is THE rank-revealing primitive the HSS kernel (v5e-1c) leans on:
// off-diagonal blocks of a rank-structured front are low-rank, and a column
// ID gives both the compressed basis (cols) and the nested translation
// operator (proj) STRUMPACK/HSS need — without ever forming an SVD.
//
// Construction = Businger-Golub column-pivoting QR (`QRColPiv`, a faithful
// LAPACK dgeqp3 port that already reveals the numerical rank), then the
// interpolation coefficients fall out of one upper-triangular back-solve:
//
//     A·P = Q·R,   R = [ R11  R12 ]   (R11 = rank×rank upper-triangular)
//                      [  0   R22 ]
//     T = R11^{-1} · R12              (rank × (n-rank))
//     A·P ≈ Q·[R11 R12; 0 0] = A[:,J] · [ I_rank | T ]
//   ⇒ proj (in the original column order) is the scatter of [ I_rank | T ].
//
// The reconstruction error ‖A − cols·proj‖ equals ‖R22‖ — bounded by the
// pivoted-QR rank tolerance. NOTE: column-pivoted QR does NOT guarantee the
// strong-RRQR bound |T_ij| ≤ 2 (that needs Gu-Eisenstat strong RRQR); in
// practice |T| is modest on benign inputs but can grow on adversarial
// (Kahan-type) matrices. Strong RRQR is the documented upgrade path if
// v5e-1c finds HSS needs the guaranteed bound. The reconstruction error is
// the contract; bounded interpolation is best-effort.
//
// Determinism: built entirely on the deterministic `QRColPiv` (no RNG); the
// rank threshold |R_kk| ≤ rcond·|R_00| is a serial, reproducible reduction.
// The counter-based-RNG moat applies to the randomized sampler (v5e-1b), not
// here.
//
// Real f32/f64, RowMajor (matches `QRColPiv` / `COD`). Lower layer: raw
// scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

template <typename T, Layout L = Layout::RowMajor>
struct InterpDecomp
{
    crd::containers::Array<crd::usize> skeleton;  // J: `rank` column indices into A (pivot order)
    Matrix<T, L> cols;                            // m × rank: the gathered skeleton columns A[:, J]
    Matrix<T, L> proj;                            // rank × n: interpolation matrix Z, proj[:, J] = I
    crd::usize m = 0;
    crd::usize n = 0;
    crd::usize rank = 0;

    explicit InterpDecomp(crd::memory::IAllocator* alloc) noexcept
        : skeleton(alloc), cols(alloc), proj(alloc)
    {
    }

    InterpDecomp(InterpDecomp&&) noexcept = default;
    InterpDecomp& operator=(InterpDecomp&&) noexcept = default;
    InterpDecomp(const InterpDecomp&) = delete;
    InterpDecomp& operator=(const InterpDecomp&) = delete;
};

// =======================================================================
// interp_decomp — column interpolative decomposition of A (m×n).
//
//   rcond  < 0  ⇒ default rank tolerance max(m,n)·eps (same as QRColPiv/COD);
//               otherwise the diagonal threshold is rcond·|R[0,0]|.
//   max_rank > 0 ⇒ additionally cap the kept rank at max_rank (the HSS
//               adaptive-rank / block-size cap); 0 ⇒ no cap.
//
// Returns an InterpDecomp with A ≈ cols·proj. A zero (or numerically
// rank-0) matrix yields rank 0: empty skeleton, m×0 cols, 0×n proj,
// reconstruction = 0. Real f32/f64.
// =======================================================================
template <typename T, Layout L>
[[nodiscard]] InterpDecomp<T, L> interp_decomp(crd::memory::IAllocator* alloc, const Matrix<T, L>& a,
                                               RealType<T> rcond = RealType<T>{-1}, crd::usize max_rank = 0);

} // namespace crd::hesap::dense
