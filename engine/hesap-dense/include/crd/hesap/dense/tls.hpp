#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3c-2 — total least squares (TLS) via SVD.
//
// Ordinary least squares assumes error only in b; TLS (Golub-Van Loan, the
// "errors-in-variables" model) allows error in BOTH A and b. The solution
// minimises ‖[ΔA Δb]‖_F subject to (A+ΔA)·x = b+Δb. It is read off the SVD
// of the augmented matrix C = [A | B]:
//
//   C = U·Σ·Vᴴ,  partition V's LAST d columns as [V12 ; V22]
//   (V12 = top n rows, V22 = bottom d rows)  ⟹  X = −V12·V22⁻¹.
//
// The solution exists & is unique iff V22 is non-singular (equivalently the
// smallest singular value of A exceeds that of C). `exists == false` flags
// the degenerate case (b ⟂ range(A), or σ_n(A) ≤ σ_{n+d}(C)).
//
// 4 type variants (f32/f64/c32/c64) via the shipped (complex) SVD. The d×d
// V22 inverse uses a type-generic Gauss-Jordan (works for real + complex).
// Lower layer: raw scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

template <typename T>
struct TLS
{
    Matrix<T> x;        // n × d TLS solution (valid iff `exists`)
    bool exists = false;

    explicit TLS(crd::memory::IAllocator* alloc) noexcept : x(alloc) {}
    TLS(TLS&&) noexcept = default;
    TLS& operator=(TLS&&) noexcept = default;
    TLS(const TLS&) = delete;
    TLS& operator=(const TLS&) = delete;
};

// =======================================================================
// tls (matrix RHS) — total least squares for A (m×n), B (m×d). Requires
// m >= n+d (overdetermined augmented system). Returns X (n×d) + existence.
// =======================================================================
template <typename T>
[[nodiscard]] TLS<T> tls(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Matrix<T>& b);

// =======================================================================
// tls (vector RHS) — single-column convenience (d = 1).
// =======================================================================
template <typename T>
[[nodiscard]] TLS<T> tls(crd::memory::IAllocator* alloc, const Matrix<T>& a, const Vector<T>& b);

} // namespace crd::hesap::dense
