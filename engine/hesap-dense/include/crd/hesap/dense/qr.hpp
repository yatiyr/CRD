#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v0e-d — Householder QR factorization for real m×n matrices.
//
// A = Q · R  where Q is m×m orthogonal and R is m×n upper-triangular.
// We store Q implicitly via the Householder reflectors v_k + tau_k:
//   Q = H_0 · H_1 · ... · H_{n-1}     where H_k = I - tau_k · v_k · v_k^T
//
// Storage (LAPACK xGEQRF convention):
//   - `packed()` is m×n. Upper triangle (incl. diagonal) holds R.
//   - Strict lower triangle of column k holds v_k[k+1:m] (the "implicit"
//     elements of the Householder vector; v_k[k] is implicitly 1).
//   - `taus()` is a length-`min(m,n)` array storing tau_k.
//
// f32 + f64 RowMajor. Unblocked Householder for v0e-d-MVP.
// v0e-d-blocked (WY-representation + trailing-update via gemm_parallel)
// and v0e-d-colpiv (rank-revealing column-pivoting QR) filed as follow-ons.
// -----------------------------------------------------------------------

template <typename T, Layout L = Layout::RowMajor>
class QR
{
public:
    using value_type = T;
    static constexpr Layout layout = L;

    explicit QR(crd::memory::IAllocator* alloc) noexcept
        : m_qr(alloc), m_taus(alloc)
    {
    }

    QR(crd::memory::IAllocator* alloc, crd::usize m, crd::usize n)
        : m_qr(alloc, m, n), m_taus(alloc)
    {
        m_taus.resize(m < n ? m : n);
    }

    QR(const QR&) = delete;
    QR& operator=(const QR&) = delete;

    QR(QR&& other) noexcept
        : m_qr(std::move(other.m_qr)), m_taus(std::move(other.m_taus))
    {
    }

    QR& operator=(QR&& other) noexcept
    {
        if (this != &other)
        {
            m_qr = std::move(other.m_qr);
            m_taus = std::move(other.m_taus);
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_qr.rows(); }
    [[nodiscard]] crd::usize cols() const noexcept { return m_qr.cols(); }
    [[nodiscard]] crd::usize num_reflectors() const noexcept { return m_taus.size(); }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept
    {
        return m_qr.allocator();
    }

    [[nodiscard]] Matrix<T, L>& packed() noexcept { return m_qr; }
    [[nodiscard]] const Matrix<T, L>& packed() const noexcept { return m_qr; }
    [[nodiscard]] crd::containers::Array<T>& taus() noexcept { return m_taus; }
    [[nodiscard]] const crd::containers::Array<T>& taus() const noexcept { return m_taus; }

private:
    Matrix<T, L> m_qr;
    crd::containers::Array<T> m_taus;
};

// =======================================================================
// factor_qr — unblocked Householder QR on qr.packed() in-place.
//
// Caller must have copied A into qr.packed() before calling the view-form.
// The Matrix-form overload copies for the caller.
// =======================================================================

template <typename T, Layout L>
void factor_qr(QR<T, L>& qr);

template <typename T, Layout L>
inline void factor_qr(QR<T, L>& qr, const Matrix<T, L>& a)
{
    CRD_ASSERT_MSG(qr.rows() == a.rows() && qr.cols() == a.cols(),
                   "factor_qr: shape mismatch");
    Matrix<T, L>& dst = qr.packed();
    const crd::usize m = a.rows();
    const crd::usize n = a.cols();
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            dst.at(i, j) = a.at(i, j);
        }
    }
    factor_qr<T, L>(qr);
}

// =======================================================================
// apply_q_transpose — y := Q^T · x. Walks reflectors in forward order:
//   x := H_{n-1} · ... · H_1 · H_0 · x.
// Used for the least-squares solve step Q^T · b → c, then back-sub R · y = c.
// =======================================================================

template <typename T, Layout L>
void apply_q_transpose(const QR<T, L>& qr, crd::containers::Span<T> x);

// =======================================================================
// apply_q — y := Q · x. Walks reflectors in reverse order:
//   x := H_0 · H_1 · ... · H_{n-1} · x.
// =======================================================================

template <typename T, Layout L>
void apply_q(const QR<T, L>& qr, crd::containers::Span<T> x);

// =======================================================================
// solve_qr_least_squares — solve min_x ||A·x − b||₂ for an m×n A (m ≥ n).
//   1. c := Q^T · b   (apply reflectors forward)
//   2. y := R⁻¹ · c[0:n]   (back substitution on the n×n upper R)
//   3. x := y    (the LS solution).
// Caller passes `b` of size m; on exit, the first n entries are the
// solution. For square A (m == n), this collapses to A·x = b solve.
// =======================================================================

template <typename T, Layout L>
void solve_qr(const QR<T, L>& qr, crd::containers::Span<T> b);

} // namespace crd::hesap::dense
