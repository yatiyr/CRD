#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3c-1b-perf — BLAS-3 blocked application of a Householder Q
// to a dense matrix (LAPACK `dlarfb`), the lever that lets pinv / multi-RHS
// least-squares crush Eigen (per-reflector scalar application is the gap).
//
// Q = H_0 · H_1 · … · H_{k-1} is stored columnwise (LAPACK xGEQRF / xGEQP3
// convention): the strict lower triangle of column j of `qpacked` (m×?,
// RowMajor, leading dim `ld`) holds v_j[j+1:m] (v_j[j] = 1 implicit), and
// `taus[j]` the scalar. A block of `nb` consecutive reflectors aggregates
// into H = I − V·T·Vᵀ and is applied to the operand as three GEMMs.
//
//   side == Left :  C (m × ccols)     := op(Q) · C
//   side == Right:  C (crows × m)     := C · op(Q)
//   transpose    :  op(Q) = Qᵀ if true else Q.
//
// Bit-compatible with the scalar `apply_q` / `apply_q_transpose` (verified in
// tests). Real f32/f64. Lower layer: raw scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

template <typename T>
void apply_q_block(const T* qpacked, crd::usize ld, crd::usize m, crd::usize k, const T* taus, T* c,
                   crd::usize ldc, crd::usize crows, crd::usize ccols, bool right, bool transpose,
                   crd::memory::IAllocator* alloc);

} // namespace crd::hesap::dense::detail
