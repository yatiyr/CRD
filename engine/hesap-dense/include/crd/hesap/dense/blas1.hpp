#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// BLAS Level 1 — vector-vector operations. Phase 3.1.6 v0b.
//
// 9 operations × 4 type variants (f32 / f64 / Complex32 / Complex64).
// Per BLAS convention:
//   - `dot<T>`  : real-only (T ∈ {f32, f64}). Sum of x_i * y_i.
//   - `dotu<T>` : complex-only. Sum of x_i * y_i (UNCONJUGATED).
//   - `dotc<T>` : complex-only. Sum of conj(x_i) * y_i (Hermitian inner product).
//   - Everything else templates over all 4 types.
//
// Determinism (ADR-0063 + D10): all sums route through
// `detail::pairwise_sum` (KBN-compensated pairwise tree). Bit-exact
// across SIMD widths AS LONG AS each backend walks the same canonical
// tree topology (kPairwiseLeafBlock = 8 = Vec8f lane count).
//
// Two-layer typed architecture (ADR-0078 §5): the kernels take raw
// `ConstSpan<T>` / `Span<T>` at the lower layer. `Vector<T>` overloads
// at the upper layer delegate to the span entry points. Typed
// quantities (`Length<T>`, `Velocity<T>`, etc.) bridge via `.value` at
// the consumer's call site — hesap math is dimensionless.
//
// Norm / abs-sum return type is the real type underlying T:
//   nrm2<f32>           -> f32
//   nrm2<Complex32>     -> f32  (real magnitude even for complex input)
// -----------------------------------------------------------------------

// ---- 1. axpy: y += alpha * x -----------------------------------------
template <typename T>
void axpy(T alpha, crd::containers::ConstSpan<T> x, crd::containers::Span<T> y);

template <typename T>
inline void axpy(T alpha, const Vector<T>& x, Vector<T>& y)
{
    axpy<T>(alpha, x.span(), y.span());
}

// ---- 2. dot: real dot product (T must be real) -----------------------
template <typename T>
[[nodiscard]] T dot(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y);

template <typename T>
[[nodiscard]] inline T dot(const Vector<T>& x, const Vector<T>& y)
{
    return dot<T>(x.span(), y.span());
}

// ---- 3. dotu: complex unconjugated dot product -----------------------
template <typename T>
[[nodiscard]] Complex<T> dotu(
    crd::containers::ConstSpan<Complex<T>> x,
    crd::containers::ConstSpan<Complex<T>> y);

template <typename T>
[[nodiscard]] inline Complex<T> dotu(const Vector<Complex<T>>& x, const Vector<Complex<T>>& y)
{
    return dotu<T>(x.span(), y.span());
}

// ---- 4. dotc: complex Hermitian (conjugated x) dot product -----------
template <typename T>
[[nodiscard]] Complex<T> dotc(
    crd::containers::ConstSpan<Complex<T>> x,
    crd::containers::ConstSpan<Complex<T>> y);

template <typename T>
[[nodiscard]] inline Complex<T> dotc(const Vector<Complex<T>>& x, const Vector<Complex<T>>& y)
{
    return dotc<T>(x.span(), y.span());
}

// ---- 5. nrm2: Euclidean norm; returns real type ----------------------
template <typename T>
[[nodiscard]] RealType<T> nrm2(crd::containers::ConstSpan<T> x);

template <typename T>
[[nodiscard]] inline RealType<T> nrm2(const Vector<T>& x)
{
    return nrm2<T>(x.span());
}

// ---- 6. scal: x *= alpha ---------------------------------------------
template <typename T>
void scal(T alpha, crd::containers::Span<T> x);

template <typename T>
inline void scal(T alpha, Vector<T>& x)
{
    scal<T>(alpha, x.span());
}

// ---- 7. copy: dst = src ----------------------------------------------
template <typename T>
void copy(crd::containers::ConstSpan<T> src, crd::containers::Span<T> dst);

template <typename T>
inline void copy(const Vector<T>& src, Vector<T>& dst)
{
    copy<T>(src.span(), dst.span());
}

// ---- 8. swap: exchange x and y ---------------------------------------
template <typename T>
void swap(crd::containers::Span<T> x, crd::containers::Span<T> y);

template <typename T>
inline void swap(Vector<T>& x, Vector<T>& y)
{
    swap<T>(x.span(), y.span());
}

// ---- 9. asum: sum of magnitudes; returns real type -------------------
template <typename T>
[[nodiscard]] RealType<T> asum(crd::containers::ConstSpan<T> x);

template <typename T>
[[nodiscard]] inline RealType<T> asum(const Vector<T>& x)
{
    return asum<T>(x.span());
}

// ---- 10. iamax: argmax |x_i|; ties broken by FIRST index (D16) -------
template <typename T>
[[nodiscard]] crd::usize iamax(crd::containers::ConstSpan<T> x);

template <typename T>
[[nodiscard]] inline crd::usize iamax(const Vector<T>& x)
{
    return iamax<T>(x.span());
}

} // namespace crd::hesap::dense
