#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v3c-1a — column-pivoting Householder QR.
//
//     A * P = Q * R
//
// where P is a permutation chosen by Businger-Golub pivoting (the column of
// largest remaining 2-norm is moved to the front at each step), Q is m×m
// orthogonal, and R is m×n upper-trapezoidal with |R[0,0]| >= |R[1,1]| >= ...
// non-increasing along the diagonal — this is what reveals the numerical
// rank and what the complete-orthogonal decomposition (COD, `cod.hpp`)
// builds on for the fast rank-deficient least-squares path (LAPACK dgelsy).
//
// Faithful port of LAPACK `dgeqp3`/`dlaqp2`: the unblocked column-pivoting
// loop with the partial-column-norm DOWNDATE (and recompute when the
// downdated norm degrades past sqrt(eps)) that keeps pivoting stable without
// recomputing every trailing column norm each step. The panel is factored on
// a TRANSPOSED scratch so every per-column op (norm / dot / axpy / swap) is a
// contiguous SIMD sweep (same trick as `qr.cpp`).
//
// Storage (LAPACK xGEQP3 convention, like `QR`):
//   - `packed()` is m×n. Upper triangle (incl. diagonal) holds R.
//   - Strict lower triangle of column k holds v_k[k+1:m] (implicit v_k[k]=1).
//   - `taus()` is length min(m,n).
//   - `jpvt()` is length n: column k of A*P is column `jpvt()[k]` of A.
//   - `rank()` is the numerical rank for the rcond used at factor time.
//
// Real f32/f64 (the COD fast path is real; complex least-squares routes
// through the complex SVD — see `lstsq`). Lower layer: raw f32/f64
// (ADR-0078 §5).
// -----------------------------------------------------------------------

template <typename T, Layout L = Layout::RowMajor>
class QRColPiv
{
public:
    using value_type = T;
    static constexpr Layout layout = L;

    explicit QRColPiv(crd::memory::IAllocator* alloc) noexcept
        : m_qr(alloc), m_taus(alloc), m_jpvt(alloc)
    {
    }

    QRColPiv(crd::memory::IAllocator* alloc, crd::usize m, crd::usize n)
        : m_qr(alloc, m, n), m_taus(alloc), m_jpvt(alloc)
    {
        m_taus.resize(m < n ? m : n);
        m_jpvt.resize(n);
    }

    QRColPiv(const QRColPiv&) = delete;
    QRColPiv& operator=(const QRColPiv&) = delete;

    QRColPiv(QRColPiv&& other) noexcept
        : m_qr(std::move(other.m_qr)), m_taus(std::move(other.m_taus)),
          m_jpvt(std::move(other.m_jpvt)), m_rank(other.m_rank)
    {
        other.m_rank = 0;
    }

    QRColPiv& operator=(QRColPiv&& other) noexcept
    {
        if (this != &other)
        {
            m_qr = std::move(other.m_qr);
            m_taus = std::move(other.m_taus);
            m_jpvt = std::move(other.m_jpvt);
            m_rank = other.m_rank;
            other.m_rank = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_qr.rows(); }
    [[nodiscard]] crd::usize cols() const noexcept { return m_qr.cols(); }
    [[nodiscard]] crd::usize num_reflectors() const noexcept { return m_taus.size(); }
    [[nodiscard]] crd::usize rank() const noexcept { return m_rank; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_qr.allocator(); }

    [[nodiscard]] Matrix<T, L>& packed() noexcept { return m_qr; }
    [[nodiscard]] const Matrix<T, L>& packed() const noexcept { return m_qr; }
    [[nodiscard]] crd::containers::Array<T>& taus() noexcept { return m_taus; }
    [[nodiscard]] const crd::containers::Array<T>& taus() const noexcept { return m_taus; }
    [[nodiscard]] crd::containers::Array<crd::usize>& jpvt() noexcept { return m_jpvt; }
    [[nodiscard]] const crd::containers::Array<crd::usize>& jpvt() const noexcept { return m_jpvt; }

    void set_rank(crd::usize r) noexcept { m_rank = r; }

private:
    Matrix<T, L> m_qr;
    crd::containers::Array<T> m_taus;
    crd::containers::Array<crd::usize> m_jpvt;
    crd::usize m_rank = 0;
};

// =======================================================================
// factor_qr_colpiv — column-pivoting Householder QR on qr.packed() in-place.
// `rcond < 0` selects the default tolerance max(m,n) * eps for the rank
// count; the diagonal threshold is `rcond * |R[0,0]|`. The view-form assumes
// the caller copied A into qr.packed(); the Matrix-form copies for the caller.
// =======================================================================
template <typename T, Layout L>
void factor_qr_colpiv(QRColPiv<T, L>& qr, RealType<T> rcond = RealType<T>{-1});

template <typename T, Layout L>
inline void factor_qr_colpiv(QRColPiv<T, L>& qr, const Matrix<T, L>& a,
                             RealType<T> rcond = RealType<T>{-1})
{
    CRD_ASSERT_MSG(qr.rows() == a.rows() && qr.cols() == a.cols(),
                   "factor_qr_colpiv: shape mismatch");
    Matrix<T, L>& dst = qr.packed();
    const crd::usize n = a.rows() * a.cols();
    const T* src = a.data();
    T* d = dst.data();
    for (crd::usize i = 0; i < n; ++i)
    {
        d[i] = src[i];
    }
    factor_qr_colpiv<T, L>(qr, rcond);
}

// =======================================================================
// apply_q_transpose / apply_q — x := Q^T · x  /  x := Q · x. Same reflector
// walk as the plain `QR` (the column permutation lives in `jpvt`, applied by
// the caller — these touch only Q).
// =======================================================================
template <typename T, Layout L>
void apply_q_transpose(const QRColPiv<T, L>& qr, crd::containers::Span<T> x);

template <typename T, Layout L>
void apply_q(const QRColPiv<T, L>& qr, crd::containers::Span<T> x);

} // namespace crd::hesap::dense
