#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_catalog.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v0e-a — LU factorization with partial row pivoting.
//
// Algorithm: right-looking blocked LU (LAPACK xGETRF). Block size 64.
//   for k = 0; k < n; k += bs:
//     panel_factor(A[k:n, k:k+b])  -- unblocked LU + pivoting on a tall panel
//     swap_rows_outside_panel(A[:, 0:k] and A[:, k+b:n])
//     U12 <- L11^-1 * A[k:k+b, k+b:n]       -- inner trsm (small, in-place)
//     A22 -= L21 * U12                       -- trailing update via gemm_parallel
//
// Storage: LU factor packed in-place into a single n*n matrix per LAPACK
// convention. Lower triangle (excl. diagonal) holds L (unit-diagonal
// implicit); upper triangle (incl. diagonal) holds U. Permutation P
// carries the row-pivot sequence (ipiv-style).
//
// First real consumer of v0d's gemm_parallel. Determinism contract
// (ADR-0063 for eylem; relaxed for hesap per ADR-0082 §2026-05-20):
// bit-exact across worker counts is required — the trailing-update
// gemm_parallel already provides this; tests verify.
// -----------------------------------------------------------------------

// LU<T, Layout> — factored representation. Owns the packed LU matrix
// (RowMajor) and the Permutation; allocator propagated from the input
// Matrix at factor time.
//
// `info`: 0 on success; k+1 if U[k,k] became exactly zero during step k
// (the matrix is exactly singular and the factor cannot be used for
// solve). Hager condition estimation in v0e-e signals near-singular
// cases without setting info.
template <typename T, Layout L = Layout::RowMajor>
class LU
{
public:
    using value_type = T;
    static constexpr Layout layout = L;

    explicit LU(crd::memory::IAllocator* alloc) noexcept
        : m_lu(alloc), m_p(alloc)
    {
    }

    LU(crd::memory::IAllocator* alloc, crd::usize n)
        : m_lu(alloc, n, n), m_p(alloc, n)
    {
    }

    LU(const LU&) = delete;
    LU& operator=(const LU&) = delete;

    LU(LU&& other) noexcept
        : m_lu(std::move(other.m_lu)), m_p(std::move(other.m_p)), m_info(other.m_info)
    {
        other.m_info = 0;
    }

    LU& operator=(LU&& other) noexcept
    {
        if (this != &other)
        {
            m_lu = std::move(other.m_lu);
            m_p = std::move(other.m_p);
            m_info = other.m_info;
            other.m_info = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize n() const noexcept { return m_lu.rows(); }
    [[nodiscard]] crd::usize info() const noexcept { return m_info; }
    [[nodiscard]] bool is_singular() const noexcept { return m_info != 0; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_lu.allocator(); }

    // Direct access to the packed LU matrix and the pivot record.
    [[nodiscard]] Matrix<T, L>& packed() noexcept { return m_lu; }
    [[nodiscard]] const Matrix<T, L>& packed() const noexcept { return m_lu; }
    [[nodiscard]] Permutation& permutation() noexcept { return m_p; }
    [[nodiscard]] const Permutation& permutation() const noexcept { return m_p; }

    // Mutable info field — set by factor_lu during factorization.
    void set_info(crd::usize info) noexcept { m_info = info; }

private:
    Matrix<T, L> m_lu;
    Permutation m_p;
    crd::usize m_info = 0;
};

// =======================================================================
// Permutation apply helpers — replay the pivot sequence on a Vector / Span
// or on the rows of a Matrix.
//
// Forward: for k = 0..n-1, swap row k with row p.pivot_at(k).
// Inverse: for k = n-1..0, swap row k with row p.pivot_at(k).
// (These are the LAPACK ipiv conventions; replaying in reverse undoes.)
// =======================================================================

inline void apply_permutation(const Permutation& p, crd::containers::Span<float> x) noexcept
{
    for (crd::usize k = 0; k < p.n(); ++k)
    {
        const crd::usize r = p.pivot_at(k);
        if (r != k)
        {
            const float tmp = x[k];
            x[k] = x[r];
            x[r] = tmp;
        }
    }
}

inline void apply_permutation(const Permutation& p, crd::containers::Span<double> x) noexcept
{
    for (crd::usize k = 0; k < p.n(); ++k)
    {
        const crd::usize r = p.pivot_at(k);
        if (r != k)
        {
            const double tmp = x[k];
            x[k] = x[r];
            x[r] = tmp;
        }
    }
}

// =======================================================================
// factor_lu — right-looking blocked LU with partial row pivoting.
//
// On entry: `lu.packed()` is filled with the source matrix A (caller
// should clone() A into lu.packed() before calling).
// On exit: `lu.packed()` holds the packed L+U; `lu.permutation()` holds
// the pivot sequence; `lu.info()` is 0 on success or k+1 if U[k,k] = 0.
//
// `scratch` carries the allocator for trailing-update gemm pack buffers.
// If nullptr, falls back to `lu.allocator()` (per
// memory/feedback_hesap_propagate_allocator — no default_allocator).
// =======================================================================

template <typename T, Layout L>
void factor_lu(LU<T, L>& lu, crd::memory::IAllocator* scratch = nullptr);

// Convenience: clone A into lu.packed() and factor.
template <typename T, Layout L>
inline void factor_lu(LU<T, L>& lu, const Matrix<T, L>& a)
{
    CRD_ASSERT_MSG(a.is_square(), "factor_lu: input matrix must be square");
    CRD_ASSERT_MSG(lu.n() == a.rows(), "factor_lu: LU size mismatch with A");
    // Copy A into lu.packed().
    Matrix<T, L>& dst = lu.packed();
    const crd::usize n = a.rows();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            dst.at(i, j) = a.at(i, j);
        }
    }
    factor_lu<T, L>(lu, lu.allocator());
}

// =======================================================================
// solve_lu — apply P, forward-sub L (unit diag), back-sub U.
// `x` receives `b` on entry and the solution on exit (single-RHS).
// =======================================================================

template <typename T, Layout L>
void solve_lu(const LU<T, L>& lu, crd::containers::Span<T> x);

// Multi-RHS variant — each column of B is solved independently.
template <typename T, Layout L>
void solve_lu(const LU<T, L>& lu, MatrixView<T, L> b);

} // namespace crd::hesap::dense
