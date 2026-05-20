#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v0e-b — Cholesky factorization for symmetric positive
// definite (SPD) real matrices. HPD complex extension filed for later.
//
// Algorithm: right-looking blocked Cholesky (LAPACK xPOTRF), block size
// 64. For each block:
//   1. Unblocked Cholesky on the L11 diagonal block (size nb × nb).
//   2. Solve L21 * L11^T = A21 → in-place column-wise division/sub.
//   3. Trailing update A22 -= L21 * L21^T (via gemm_parallel).
//
// Storage (LAPACK xPOTRF convention): output is the lower triangle of a
// packed n × n matrix; upper triangle holds intermediate garbage from
// the trailing-update GEMM but is unused on solve.
//
// Determinism (ADR-0082 §2026-05-20): bit-exact across worker counts
// because the trailing update inherits gemm_parallel's disjoint-row-
// slab guarantee, and the panel + trsm work is single-thread.
//
// Reference-class shootout vs Eigen LLT lands at v0e-g.
//
// FOLLOW-ONS (filed; not in this slice):
//   - `v0e-b-hpd`: Hermitian / complex variant.
//   - `v0e-b-syrk-optim`: replace gemm_parallel trailing update with a
//     true syrk (half the FLOPs since we don't need the upper triangle).
//     Requires a SymmetricView class to address a sub-block.
// -----------------------------------------------------------------------

// Cholesky<T, Layout> — factored representation. The lower triangle of
// `packed()` holds L; the upper triangle is unused. f32 + f64 real for
// v0e-b; HPD complex extension is a filed follow-on.
//
// `info`: 0 on success; k+1 if the (k,k) diagonal element of A − L·L^T
// became non-positive (input was not strictly positive-definite).
template <typename T, Layout L = Layout::RowMajor>
class Cholesky
{
public:
    using value_type = T;
    static constexpr Layout layout = L;

    explicit Cholesky(crd::memory::IAllocator* alloc) noexcept : m_l(alloc) {}

    Cholesky(crd::memory::IAllocator* alloc, crd::usize n) : m_l(alloc, n, n) {}

    Cholesky(const Cholesky&) = delete;
    Cholesky& operator=(const Cholesky&) = delete;

    Cholesky(Cholesky&& other) noexcept : m_l(std::move(other.m_l)), m_info(other.m_info)
    {
        other.m_info = 0;
    }

    Cholesky& operator=(Cholesky&& other) noexcept
    {
        if (this != &other)
        {
            m_l = std::move(other.m_l);
            m_info = other.m_info;
            other.m_info = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize n() const noexcept { return m_l.rows(); }
    [[nodiscard]] crd::usize info() const noexcept { return m_info; }
    [[nodiscard]] bool is_singular() const noexcept { return m_info != 0; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_l.allocator(); }

    [[nodiscard]] Matrix<T, L>& packed() noexcept { return m_l; }
    [[nodiscard]] const Matrix<T, L>& packed() const noexcept { return m_l; }

    void set_info(crd::usize info_value) noexcept { m_info = info_value; }

private:
    Matrix<T, L> m_l;
    crd::usize m_info = 0;
};

// =======================================================================
// factor_cholesky — in-place blocked Cholesky on chol.packed().
//
// Caller must have copied the lower triangle of A into chol.packed()
// before invoking the view-form. The Symmetric-form overload below does
// the copy automatically.
//
// `scratch` carries the allocator for trailing-update GEMM pack buffers.
// nullptr falls back to chol.allocator() (no `default_allocator()` per
// memory/feedback_hesap_propagate_allocator).
// =======================================================================

template <typename T, Layout L>
void factor_cholesky(Cholesky<T, L>& chol, crd::memory::IAllocator* scratch = nullptr);

// Convenience: copy A's lower triangle into chol.packed() and factor.
template <typename T, Layout L>
inline void factor_cholesky(Cholesky<T, L>& chol, const Symmetric<T>& a)
{
    CRD_ASSERT_MSG(chol.n() == a.n(), "factor_cholesky: chol size mismatch with A");
    const crd::usize n = a.n();
    Matrix<T, L>& dst = chol.packed();
    for (crd::usize i = 0; i < n; ++i)
    {
        // Copy lower triangle (including diagonal); upper triangle is
        // garbage / unused.
        for (crd::usize j = 0; j <= i; ++j)
        {
            dst.at(i, j) = a.at(i, j);
        }
        // Zero out the upper triangle so a debug Trail-update-GEMM that
        // reads from L's upper half during inner trsm gets deterministic
        // zeros, not uninitialised memory.
        for (crd::usize j = i + 1; j < n; ++j)
        {
            dst.at(i, j) = T{0};
        }
    }
    factor_cholesky<T, L>(chol, chol.allocator());
}

// =======================================================================
// solve_cholesky — solve A · x = b given Cholesky factor A = L · L^T.
//   1. Forward sub: solve L · y = b
//   2. Back sub:    solve L^T · x = y
// `x` receives b on entry and the solution on exit (single-RHS).
// =======================================================================

template <typename T, Layout L>
void solve_cholesky(const Cholesky<T, L>& chol, crd::containers::Span<T> x);

// Multi-RHS variant — each column of B is solved independently.
template <typename T, Layout L>
void solve_cholesky(const Cholesky<T, L>& chol, MatrixView<T, L> b);

} // namespace crd::hesap::dense
