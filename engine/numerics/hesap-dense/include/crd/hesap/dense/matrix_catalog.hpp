#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/memory/allocator.hpp>

#include <type_traits>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Matrix-type catalog. v0a shipped empty shells for 15 dense types; v0c
// populates Matrix / MatrixView / Symmetric / Hermitian / Triangular /
// Banded with full bodies in their own headers (matrix.hpp, structured.hpp,
// triangular.hpp, banded.hpp); the rest stay as shells until their
// consumer arrives (Diagonal / Identity / Permutation / BlockDiagonal /
// BlockTridiagonal / Toeplitz / Hankel / Circulant / Vandermonde).
//
// Sparse catalog (CSR / CSC / BSR / COO / ELL / HYB / DIA / CSR5 /
// Merge-CSR / Sliced ELL / JDS / SkyLine) lives in crd-hesap-sparse,
// which ships v1 per the phase doc.
// -----------------------------------------------------------------------

// 1. Matrix<T, Layout> + 2. MatrixView<T, Layout> live in `matrix.hpp`
// (v0c populated bodies; v0a shipped empty shells here). matrix_catalog.hpp
// kept this slot as a forward-pointer for the catalog index.

// -----------------------------------------------------------------------
// 3. Diagonal<T> — diagonal matrix (only the diagonal is stored).
//
// O(n) storage instead of O(n²). BLAS dispatch sees this type and falls
// through to elementwise scaling kernels.
// -----------------------------------------------------------------------
template <typename T>
class Diagonal
{
public:
    using value_type = T;

    explicit Diagonal(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    [[nodiscard]] crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] bool is_square() const noexcept { return true; }

protected:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize m_n = 0;
};

// -----------------------------------------------------------------------
// 4. Identity<T> — identity matrix (no storage; everything inferred).
// -----------------------------------------------------------------------
template <typename T>
class Identity
{
public:
    using value_type = T;

    constexpr explicit Identity(crd::usize n) noexcept : m_n(n) {}

    [[nodiscard]] constexpr crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] constexpr crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] constexpr crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] constexpr bool is_square() const noexcept { return true; }

private:
    crd::usize m_n = 0;
};

// -----------------------------------------------------------------------
// 5. Permutation — row-permutation expressed in LAPACK xGETRF pivot form.
//
// Storage: m_pivots[k] = row index r (>= k) that was swapped with row k
// during factorization step k. Applying the permutation means replaying
// the swap sequence in order; inverse means replaying in reverse. This
// matches LAPACK ipiv conventions and is the storage Eigen uses too.
//
// v0e populates the body to support LU factor row-pivoting + solve.
// Vector / Matrix-form apply / apply_inverse helpers in `permutation.hpp`
// (forward-declared here; consumers include that header for the bodies).
// -----------------------------------------------------------------------
class Permutation
{
public:
    explicit Permutation(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    Permutation(crd::memory::IAllocator* alloc, crd::usize n) : m_alloc(alloc), m_n(n)
    {
        if (n > 0)
        {
            m_pivots = static_cast<crd::usize*>(m_alloc->allocate(n * sizeof(crd::usize), alignof(crd::usize)));
            for (crd::usize i = 0; i < n; ++i)
            {
                m_pivots[i] = i;
            }
        }
    }

    ~Permutation() { release(); }

    Permutation(const Permutation&) = delete;
    Permutation& operator=(const Permutation&) = delete;

    Permutation(Permutation&& other) noexcept
        : m_alloc(other.m_alloc), m_pivots(other.m_pivots), m_n(other.m_n)
    {
        other.m_pivots = nullptr;
        other.m_n = 0;
    }

    Permutation& operator=(Permutation&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_alloc = other.m_alloc;
            m_pivots = other.m_pivots;
            m_n = other.m_n;
            other.m_pivots = nullptr;
            other.m_n = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] bool is_square() const noexcept { return true; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // Raw pivot array; entry k is the row index that step k swapped with.
    [[nodiscard]] crd::usize* pivots() noexcept { return m_pivots; }
    [[nodiscard]] const crd::usize* pivots() const noexcept { return m_pivots; }

    [[nodiscard]] crd::usize& pivot_at(crd::usize k) noexcept
    {
        CRD_ASSERT_MSG(k < m_n, "Permutation::pivot_at out of range");
        return m_pivots[k];
    }

    [[nodiscard]] crd::usize pivot_at(crd::usize k) const noexcept
    {
        CRD_ASSERT_MSG(k < m_n, "Permutation::pivot_at out of range");
        return m_pivots[k];
    }

    // Resets to identity (pivot_at(k) = k for all k).
    void set_identity() noexcept
    {
        for (crd::usize i = 0; i < m_n; ++i)
        {
            m_pivots[i] = i;
        }
    }

protected:
    void release() noexcept
    {
        if (m_pivots != nullptr)
        {
            m_alloc->deallocate(m_pivots);
            m_pivots = nullptr;
        }
    }

    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize* m_pivots = nullptr;
    crd::usize m_n = 0;
};

// 6. Triangular<T, Side, Diag>, 7. Symmetric<T>, 8. Hermitian<T>, 9. Banded<T>
// — v0c populated bodies in `matrix_types.hpp`. Slot reserved here for the
// catalog index.

// -----------------------------------------------------------------------
// 10. BlockDiagonal<T> — block-diagonal matrix (n_blocks square blocks
// along the diagonal). Used by domain-decomposition preconditioners.
// -----------------------------------------------------------------------
template <typename T>
class BlockDiagonal
{
public:
    using value_type = T;

    explicit BlockDiagonal(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    [[nodiscard]] crd::usize rows() const noexcept { return m_total_rows; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_total_cols; }
    [[nodiscard]] crd::usize num_blocks() const noexcept { return m_num_blocks; }
    [[nodiscard]] bool is_square() const noexcept { return m_total_rows == m_total_cols; }

protected:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize m_total_rows = 0;
    crd::usize m_total_cols = 0;
    crd::usize m_num_blocks = 0;
};

// -----------------------------------------------------------------------
// 11. BlockTridiagonal<T> — three diagonals of square blocks.
// Used by FEM stiffness matrices on regular meshes + 1D PDE solvers.
// -----------------------------------------------------------------------
template <typename T>
class BlockTridiagonal
{
public:
    using value_type = T;

    explicit BlockTridiagonal(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    [[nodiscard]] crd::usize rows() const noexcept { return m_total_rows; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_total_cols; }
    [[nodiscard]] crd::usize num_blocks() const noexcept { return m_num_blocks; }
    [[nodiscard]] bool is_square() const noexcept { return m_total_rows == m_total_cols; }

protected:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize m_total_rows = 0;
    crd::usize m_total_cols = 0;
    crd::usize m_num_blocks = 0;
};

// -----------------------------------------------------------------------
// 12. Toeplitz<T> — constant along each diagonal. O(n) storage.
// FFT-based matvec via circulant embedding.
// -----------------------------------------------------------------------
template <typename T>
class Toeplitz
{
public:
    using value_type = T;

    explicit Toeplitz(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    [[nodiscard]] crd::usize rows() const noexcept { return m_rows; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_cols; }
    [[nodiscard]] bool is_square() const noexcept { return m_rows == m_cols; }

protected:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize m_rows = 0;
    crd::usize m_cols = 0;
};

// -----------------------------------------------------------------------
// 13. Hankel<T> — constant along each anti-diagonal. O(n) storage.
// -----------------------------------------------------------------------
template <typename T>
class Hankel
{
public:
    using value_type = T;

    explicit Hankel(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    [[nodiscard]] crd::usize rows() const noexcept { return m_rows; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_cols; }
    [[nodiscard]] bool is_square() const noexcept { return m_rows == m_cols; }

protected:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize m_rows = 0;
    crd::usize m_cols = 0;
};

// -----------------------------------------------------------------------
// 14. Circulant<T> — Toeplitz with wrap-around. FFT-diagonalisable.
// matvec is O(n log n) via FFT (lands once v10 FFT slice arrives).
// -----------------------------------------------------------------------
template <typename T>
class Circulant
{
public:
    using value_type = T;

    explicit Circulant(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    [[nodiscard]] crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] bool is_square() const noexcept { return true; }

protected:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize m_n = 0;
};

// -----------------------------------------------------------------------
// 15. Vandermonde<T> — V_ij = x_i^j. Polynomial-interpolation matrix.
// O(n) storage (just the xs); O(n²) entries inferred.
// -----------------------------------------------------------------------
template <typename T>
class Vandermonde
{
public:
    using value_type = T;

    explicit Vandermonde(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    [[nodiscard]] crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] bool is_square() const noexcept { return true; }

protected:
    crd::memory::IAllocator* m_alloc = nullptr;
    crd::usize m_n = 0;
};

} // namespace crd::hesap::dense
