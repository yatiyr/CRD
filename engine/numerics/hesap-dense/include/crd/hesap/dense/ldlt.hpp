#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_catalog.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v0e-c — LDLT factorization with Bunch-Kaufman partial
// pivoting for symmetric indefinite matrices.
//
// A = P · L · D · L^T · P^T
//   where L is unit-lower-triangular, D is block-diagonal mixing 1×1
//   and 2×2 blocks. Used for KKT systems / saddle-point problems where
//   a Cholesky factor doesn't exist.
//
// Pivot selection (Bunch-Kaufman, UPLO=Lower, ALPHA = (1+√17)/8 ≈ 0.6404):
//   At each step k:
//     - colmax = max |A[i, k]| for i > k
//     - if |A[k, k]| ≥ ALPHA · colmax: 1×1 pivot at (k, k), no swap
//     - else find rowmax in the imax row; pick between (k, k) 1×1,
//       (imax, imax) 1×1 with row swap, or 2×2 pivot
//
// Storage (LAPACK xSYTRF convention):
//   - Lower triangle of `packed()` holds L (unit-diag implicit; the
//     diagonal of `packed()` stores D's 1×1 blocks). For 2×2 pivot at
//     (k, k+1), packed.at(k, k) = D[k,k], packed.at(k+1, k) = D[k+1,k],
//     packed.at(k+1, k+1) = D[k+1, k+1], and L[k+1, k] is implicitly 0.
//   - `block_kinds[k]` is 1 for a 1×1 pivot at position k, and 2 for
//     the first element of a 2×2 block (with `block_kinds[k+1] = 0` as
//     the continuation marker).
//
// f32 + f64 RowMajor. v0e-c-MVP uses an unblocked trailing update
// (nested loops). `v0e-c-blocked` is filed to route the trailing
// update through `gemm_parallel` once the 2×2-pivot bookkeeping is
// stable. Reference-class shootout vs Eigen LDLT lands at v0e-g.
// -----------------------------------------------------------------------

template <typename T, Layout L = Layout::RowMajor>
class LDLT
{
public:
    using value_type = T;
    static constexpr Layout layout = L;

    explicit LDLT(crd::memory::IAllocator* alloc) noexcept
        : m_factor(alloc), m_p(alloc), m_block_kinds(alloc)
    {
    }

    LDLT(crd::memory::IAllocator* alloc, crd::usize n)
        : m_factor(alloc, n, n), m_p(alloc, n), m_block_kinds(alloc)
    {
        m_block_kinds.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_block_kinds[i] = 0U;
        }
    }

    LDLT(const LDLT&) = delete;
    LDLT& operator=(const LDLT&) = delete;

    LDLT(LDLT&& other) noexcept
        : m_factor(std::move(other.m_factor)),
          m_p(std::move(other.m_p)),
          m_block_kinds(std::move(other.m_block_kinds)),
          m_info(other.m_info)
    {
        other.m_info = 0;
    }

    LDLT& operator=(LDLT&& other) noexcept
    {
        if (this != &other)
        {
            m_factor = std::move(other.m_factor);
            m_p = std::move(other.m_p);
            m_block_kinds = std::move(other.m_block_kinds);
            m_info = other.m_info;
            other.m_info = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize n() const noexcept { return m_factor.rows(); }
    [[nodiscard]] crd::usize info() const noexcept { return m_info; }
    [[nodiscard]] bool is_singular() const noexcept { return m_info != 0; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept
    {
        return m_factor.allocator();
    }

    [[nodiscard]] Matrix<T, L>& packed() noexcept { return m_factor; }
    [[nodiscard]] const Matrix<T, L>& packed() const noexcept { return m_factor; }
    [[nodiscard]] Permutation& permutation() noexcept { return m_p; }
    [[nodiscard]] const Permutation& permutation() const noexcept { return m_p; }

    // 1 = 1×1 pivot at position k; 2 = start of 2×2 pivot at (k, k+1);
    // 0 = continuation marker (second row of a 2×2 block).
    [[nodiscard]] crd::u8 block_kind(crd::usize k) const noexcept
    {
        return m_block_kinds[k];
    }
    [[nodiscard]] crd::containers::Array<crd::u8>& block_kinds() noexcept
    {
        return m_block_kinds;
    }

    void set_info(crd::usize v) noexcept { m_info = v; }

private:
    Matrix<T, L> m_factor;
    Permutation m_p;
    crd::containers::Array<crd::u8> m_block_kinds;
    crd::usize m_info = 0;
};

// =======================================================================
// factor_ldlt — Bunch-Kaufman LDLT factor (UPLO=Lower).
//
// On entry: caller copied lower triangle of A into ldlt.packed().
// The Symmetric-form overload below does the copy automatically.
// =======================================================================

template <typename T, Layout L>
void factor_ldlt(LDLT<T, L>& ldlt);

// Convenience: copy A's lower triangle into ldlt.packed() and factor.
template <typename T, Layout L>
inline void factor_ldlt(LDLT<T, L>& ldlt, const Symmetric<T>& a)
{
    CRD_ASSERT_MSG(ldlt.n() == a.n(), "factor_ldlt: ldlt size mismatch with A");
    const crd::usize n = a.n();
    Matrix<T, L>& dst = ldlt.packed();
    for (crd::usize i = 0; i < n; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            dst.at(i, j) = a.at(i, j);
        }
        for (crd::usize j = i + 1; j < n; ++j)
        {
            dst.at(i, j) = T{0};
        }
    }
    factor_ldlt<T, L>(ldlt);
}

// =======================================================================
// solve_ldlt — solve A · x = b given factor A = P · L · D · L^T · P^T.
//   1. Apply P: x = P · b
//   2. Forward sub: solve L · y = x  (block-aware: 1×1 + 2×2 pivots)
//   3. D · z = y    (scalar divide for 1×1; 2×2 inverse for 2×2 block)
//   4. Back sub: solve L^T · w = z
//   5. Apply P^T: x = P^T · w  (replay pivots in reverse)
// =======================================================================

template <typename T, Layout L>
void solve_ldlt(const LDLT<T, L>& ldlt, crd::containers::Span<T> x);

} // namespace crd::hesap::dense
