#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// BLAS Level 3 — matrix-matrix operations. Phase 3.1.6 v0d.
//
// v0d ships the FOUNDATION: Goto/BLIS 5-loop GEMM with scalar +
// AVX2 microkernels (Vec8f based) as compile-time-dispatched hot-swap
// points (`detail::gemm_microkernel<T>`). The 7-op surface — gemm /
// syrk / herk / syr2k / her2k / trmm / trsm — is implemented across
// f32 / f64 / Complex32 / Complex64. Mixed-precision dispatch helper
// for HPL-AI iterative-refinement pattern.
//
// FOLLOW-ONS (filed; not in this slice):
//   - `v0d-microkernel-tune`: AVX-512 + NEON + SVE2 microkernels +
//     ≥70% AVX-512 peak benchmark target.
//   - `v0d-formal-dag`: PLASMA / PaRSEC-style task DAG via
//     `crd-hesap-sched` formal dep tracking.
//
// Determinism (ADR-0063): tile-summation order is fixed at compile time
// by the Mc/Kc/Nc block sizes. Bit-exact across SIMD widths AS LONG AS
// the chosen microkernel walks the same accumulation order (scalar +
// AVX2 already match because Vec8f operations are deterministic via
// crd-math::simd's no-FMA contract).
// -----------------------------------------------------------------------

// ---- gemm: C = alpha * op(A) * op(B) + beta * C ----------------------
// op(A) ∈ {A, A^T, A^H}; same for op(B).
//
// Scratch allocator: `scratch` is used for the internal pack buffers
// (~4-8 MB per call at large N). If nullptr, falls back to
// a PER-THREAD GrowableTlsfAllocator (pooled, non-malloc) -- that fallback
// exists for view-form callers that cannot carry an allocator, but the
// Matrix-form overload below auto-propagates `a.allocator()` and is
// preferred. See memory/feedback_hesap_propagate_allocator.
template <typename T, Layout L>
void gemm(T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b, T beta,
          MatrixView<T, L> c, Trans trans_a = Trans::None, Trans trans_b = Trans::None,
          crd::memory::IAllocator* scratch = nullptr);

template <typename T, Layout L>
inline void gemm(T alpha, const Matrix<T, L>& a, const Matrix<T, L>& b, T beta, Matrix<T, L>& c,
                 Trans trans_a = Trans::None, Trans trans_b = Trans::None)
{
    gemm<T, L>(alpha, a.cview(), b.cview(), beta, c.view(), trans_a, trans_b, a.allocator());
}

// ---- gemm_parallel: BLIS-style outer-loop parallelism ----------------
//
// Same math as `gemm`; dispatches the outer `ic` loop across
// `num_workers` `crd::jobs` worker fibers. Requires `crd::jobs::init()`
// to have been called when `num_workers > 1`. Falls back to serial
// `gemm` when `num_workers <= 1`.
//
// Determinism (ADR-0063): each worker updates a disjoint row-slab of C
// → bit-exact across thread counts. Tested in test_blas3_parallel.cpp.
//
// Per-worker scratch: one Ac pack buffer per worker (~120 KB for f32 /
// 240 KB for f64). Bc is packed once per (jc, pc) by the dispatching
// thread and shared read-only with workers.
template <typename T, Layout L>
void gemm_parallel(crd::u32 num_workers, T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b,
                   T beta, MatrixView<T, L> c, Trans trans_a = Trans::None,
                   Trans trans_b = Trans::None, crd::memory::IAllocator* scratch = nullptr);

template <typename T, Layout L>
inline void gemm_parallel(crd::u32 num_workers, T alpha, const Matrix<T, L>& a, const Matrix<T, L>& b,
                          T beta, Matrix<T, L>& c, Trans trans_a = Trans::None,
                          Trans trans_b = Trans::None)
{
    gemm_parallel<T, L>(num_workers, alpha, a.cview(), b.cview(), beta, c.view(), trans_a, trans_b,
                        a.allocator());
}

// ---- gemm_parallel_auto: pick num_workers automatically (v0d-parallelism-
//      auto-dispatch). Heuristic: serial for tiny matrices (mnk < 256K),
//      else `crd::jobs::num_workers()`. The internal Mc auto-tune + small-
//      gemm fast-path then handle load-balance + per-size optimization.
//
//      Use this when you don't have specific knowledge about the workload
//      size or the hardware (e.g. v0e solvers calling gemm_parallel many
//      times across factorization steps).
template <typename T, Layout L>
void gemm_parallel_auto(T alpha, MatrixView<const T, L> a, MatrixView<const T, L> b, T beta,
                        MatrixView<T, L> c, Trans trans_a = Trans::None,
                        Trans trans_b = Trans::None,
                        crd::memory::IAllocator* scratch = nullptr);

template <typename T, Layout L>
inline void gemm_parallel_auto(T alpha, const Matrix<T, L>& a, const Matrix<T, L>& b, T beta,
                               Matrix<T, L>& c, Trans trans_a = Trans::None,
                               Trans trans_b = Trans::None)
{
    gemm_parallel_auto<T, L>(alpha, a.cview(), b.cview(), beta, c.view(), trans_a, trans_b,
                             a.allocator());
}

// ---- syrk: C = alpha * A * A^T + beta * C (symmetric, real) ----------
template <typename T>
void syrk(T alpha, MatrixView<const T, Layout::RowMajor> a, T beta, Symmetric<T>& c,
          Trans trans = Trans::None);

// ---- herk: C = alpha * A * A^H + beta * C (Hermitian; alpha real) ----
template <typename T>
void herk(T alpha, MatrixView<const Complex<T>, Layout::RowMajor> a, T beta,
          Hermitian<Complex<T>>& c, Trans trans = Trans::None);

// ---- syr2k: C = alpha * (A*B^T + B*A^T) + beta * C (real) ------------
template <typename T>
void syr2k(T alpha, MatrixView<const T, Layout::RowMajor> a, MatrixView<const T, Layout::RowMajor> b,
           T beta, Symmetric<T>& c, Trans trans = Trans::None);

// ---- her2k: C = alpha * A * B^H + conj(alpha) * B * A^H + beta * C ---
template <typename T>
void her2k(Complex<T> alpha, MatrixView<const Complex<T>, Layout::RowMajor> a,
           MatrixView<const Complex<T>, Layout::RowMajor> b, T beta, Hermitian<Complex<T>>& c,
           Trans trans = Trans::None);

// ---- trmm: B = alpha * op(A) * B  (A triangular; in-place B) ---------
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trmm(T alpha, const Triangular<T, Side, Diag>& a, MatrixView<T, Layout::RowMajor> b,
          Trans trans = Trans::None);

// ---- trsm: B = alpha * op(A)^-1 * B  (in-place B; A triangular) ------
template <typename T, TriangularSide Side, TriangularDiag Diag>
void trsm(T alpha, const Triangular<T, Side, Diag>& a, MatrixView<T, Layout::RowMajor> b,
          Trans trans = Trans::None);

// ---- Mixed-precision gemm: C(TAcc) = alpha * A(TIn) * B(TIn) + ... ---
//
// `gemm_mixed<TIn, TAcc>` reads A and B in TIn (e.g. f32), accumulates in
// TAcc (e.g. f64), writes C in TAcc. Implements the HPL-AI iterative-
// refinement pattern (factor in lower precision, refine in higher) per
// D5 of ADR-0065 §13.
template <typename TIn, typename TAcc, Layout L>
void gemm_mixed(TAcc alpha, MatrixView<const TIn, L> a, MatrixView<const TIn, L> b, TAcc beta,
                MatrixView<TAcc, L> c);

} // namespace crd::hesap::dense
