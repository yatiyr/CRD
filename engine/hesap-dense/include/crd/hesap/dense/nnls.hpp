#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3c-2 — non-negative least squares (NNLS).
//
//     minimise ‖A·x − b‖₂   subject to   x ≥ 0
//
// Lawson-Hanson 1974 active-set method. The passive set P (free, currently
// positive variables) is solved as an UNCONSTRAINED least-squares problem;
// variables that would go negative are moved to the active set Z (held at 0).
//
// The passive-set least-squares solve is maintained by an INCREMENTAL thin
// QR of A_P (Björck §5.8): adding a column is one (re-orthogonalised)
// Gram-Schmidt step; removing a column re-triangularises R + the transformed
// RHS via a sweep of Givens rotations (the "up/downdate" — no full refactor
// per active-set change). Deterministic: the entering variable is chosen by
// largest gradient with ascending-index tie-break (D(lstsq)-1).
//
// Real f32/f64 only — the x ≥ 0 ordering is meaningless for complex. Lower
// layer: raw scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

template <typename T>
struct NNLS
{
    Vector<T> x;                  // length n, x ≥ 0
    crd::usize iterations = 0;    // outer active-set iterations
    bool converged = false;

    explicit NNLS(crd::memory::IAllocator* alloc) noexcept : x(alloc) {}
    NNLS(NNLS&&) noexcept = default;
    NNLS& operator=(NNLS&&) noexcept = default;
    NNLS(const NNLS&) = delete;
    NNLS& operator=(const NNLS&) = delete;
};

// =======================================================================
// nnls — solve min‖A·x − b‖₂ s.t. x ≥ 0 for A (m×n), b (length m). `tol < 0`
// selects a problem-scaled default. `max_iter == 0` selects 3·(n+1).
// =======================================================================
template <typename T>
[[nodiscard]] NNLS<T> nnls(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Vector<T>& b,
                           T tol = T{-1}, crd::usize max_iter = 0);

} // namespace crd::hesap::dense
