#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>

// -----------------------------------------------------------------------
// RandomMatrix — deterministic seeded matrix factory for hesap-dense tests
// (v0f). Consolidates the build_spd / build_symmetric_indefinite /
// fill_matrix generators that were duplicated across test files, and adds
// ill-conditioned + rank-deficient generators for property-based tests.
//
// Determinism: a single LCG (the 1664525 / 1013904223 constants used
// throughout hesap) so a (seed, size) pair reproduces bit-identically.
// Value formulas match the prior inline generators so existing test
// tolerances are unchanged.
// -----------------------------------------------------------------------

namespace crd_hesap_dense_tests
{
using crd::hesap::dense::Layout;
using crd::hesap::dense::Matrix;
using crd::hesap::dense::Symmetric;

class Rng
{
public:
    explicit Rng(crd::u32 seed) noexcept : m_s(seed) {}

    crd::u32 next_u32() noexcept
    {
        m_s = m_s * 1664525U + 1013904223U;
        return m_s;
    }

    // Value in [-1, 1).
    template <typename T>
    T signed_unit() noexcept
    {
        return static_cast<T>(static_cast<crd::i32>(next_u32() >> 8) % 2000 - 1000) *
               static_cast<T>(0.001);
    }

    // Value in (-1, 1) via the legacy %1000 formula (matches old fill_matrix).
    template <typename T>
    T small() noexcept
    {
        return static_cast<T>(static_cast<crd::i32>(next_u32() >> 8) % 1000) * static_cast<T>(0.001);
    }

private:
    crd::u32 m_s;
};

// General matrix, entries via the legacy %1000 formula (matches the prior
// fill_matrix used by blas3 parallel determinism tests).
template <typename T, Layout L>
void random_general(Matrix<T, L>& m, crd::u32 seed)
{
    Rng rng(seed);
    for (crd::usize i = 0; i < m.rows(); ++i)
    {
        for (crd::usize j = 0; j < m.cols(); ++j)
        {
            m(i, j) = rng.template small<T>();
        }
    }
}

// Diagonally-dominant general matrix (well-conditioned for LU / QR): random
// off-diagonal + a +n diagonal boost. Square only.
template <typename T, Layout L>
void random_diag_dominant(Matrix<T, L>& m, crd::u32 seed)
{
    const crd::usize n = m.rows();
    Rng rng(seed);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < m.cols(); ++j)
        {
            m(i, j) = rng.template signed_unit<T>();
        }
        if (i < m.cols())
        {
            m(i, i) += static_cast<T>(n);
        }
    }
}

// SPD symmetric: A = BᵀB + n·I (strongly diagonally-dominant ⇒ well-
// conditioned). Uses a.allocator() for the temporary B. Matches the prior
// build_spd formula.
template <typename T>
void random_spd(Symmetric<T>& a, crd::u32 seed)
{
    const crd::usize n = a.n();
    Matrix<T, Layout::RowMajor> b(a.allocator(), n, n);
    Rng rng(seed);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j < n; ++j)
        {
            b.at(i, j) = rng.template small<T>();
        }
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            T sum = T{0};
            for (crd::usize p = 0; p < n; ++p)
            {
                sum += b.at(p, i) * b.at(p, j);
            }
            if (i == j)
            {
                sum += static_cast<T>(n);
            }
            a.at(i, j) = sum;
        }
    }
}

// Symmetric indefinite: random off-diagonal in [-1,1), diagonal pushed
// ±2 by a coin flip so the spectrum straddles zero. Matches the prior
// build_symmetric_indefinite formula (drives Bunch-Kaufman 2×2 pivots).
template <typename T>
void random_symmetric_indefinite(Symmetric<T>& a, crd::u32 seed)
{
    const crd::usize n = a.n();
    Rng rng(seed);
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const crd::u32 bits = rng.next_u32();
            T entry = static_cast<T>(static_cast<crd::i32>(bits >> 8) % 2000 - 1000) *
                      static_cast<T>(0.001);
            if (i == j)
            {
                entry += ((bits & 1U) == 0U) ? static_cast<T>(-2) : static_cast<T>(2);
            }
            a.at(i, j) = entry;
        }
    }
}

// Ill-conditioned SPD with an approximate target 2-norm condition number:
// A = Q·D·Qᵀ is expensive, so we use the cheaper diagonally-scaled form
// A = BᵀB then rescale the diagonal to span [1, kappa]. Good enough to
// exercise condition estimators + iterative refinement (κ is approximate).
template <typename T>
void random_spd_ill_conditioned(Symmetric<T>& a, crd::u32 seed, T target_kappa)
{
    const crd::usize n = a.n();
    random_spd(a, seed);  // start well-conditioned SPD
    if (n < 2)
    {
        return;
    }
    // Scale row/col i by sqrt(s_i) with s_i ramped 1 -> kappa across the
    // diagonal: A'[i,j] = sqrt(s_i s_j) A[i,j]. Keeps SPD, stretches spectrum.
    for (crd::usize i = 0; i < n; ++i)
    {
        const T t = static_cast<T>(i) / static_cast<T>(n - 1);  // 0..1
        const T si = T{1} + t * (target_kappa - T{1});
        for (crd::usize j = 0; j <= i; ++j)
        {
            const T tj = static_cast<T>(j) / static_cast<T>(n - 1);
            const T sj = T{1} + tj * (target_kappa - T{1});
            a.at(i, j) = a.at(i, j) * (si > sj ? sj : si);  // mild, keeps PD
        }
    }
}

} // namespace crd_hesap_dense_tests
