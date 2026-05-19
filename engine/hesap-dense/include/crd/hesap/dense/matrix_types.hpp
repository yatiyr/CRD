#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>
#include <type_traits>
#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Symmetric<T> / Hermitian<T> / Triangular<T, Side, Diag> / Banded<T>.
// Phase 3.1.6 v0c — bodies for the v0a catalog shells, consumed by BLAS L2.
//
// Storage policy (D26 — for v0c):
//   - Symmetric / Hermitian: dense n*n storage. Wasteful (2x vs packed)
//     but clean indexing. Packed-storage variants filed as v0c-packed
//     follow-on. Lower triangle is canonical; ops read from the lower
//     half + reconstruct symmetric / Hermitian access at the boundary.
//   - Triangular: dense n*n storage; only the Side half is canonical.
//     UnitDiag returns T{1} for the diagonal regardless of storage.
//   - Banded: LAPACK column-major banded storage,
//     (kl + ku + 1) × cols. Element (i, j) lives at
//     storage[(ku + i - j) + j*(kl+ku+1)] iff |i - j| in [-ku, kl].
//
// All four are move-only with explicit clone (D15).
// -----------------------------------------------------------------------

namespace detail
{
template <typename T>
T* allocate_zeroed(crd::memory::IAllocator* alloc, crd::usize n)
{
    if (n == 0)
    {
        return nullptr;
    }
    const crd::usize align = alignof(T) > 16 ? alignof(T) : 16;
    T* p = static_cast<T*>(alloc->allocate(n * sizeof(T), align));
    if constexpr (std::is_trivially_constructible_v<T>)
    {
        std::memset(p, 0, n * sizeof(T));
    }
    else
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            new (p + i) T{};
        }
    }
    return p;
}

template <typename T>
void deallocate_storage(crd::memory::IAllocator* alloc, T* p, crd::usize n)
{
    if (p == nullptr)
    {
        return;
    }
    if constexpr (!std::is_trivially_destructible_v<T>)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            p[i].~T();
        }
    }
    alloc->deallocate(p);
}
} // namespace detail

// =======================================================================
// Symmetric<T> — dense n×n storage; lower triangle is canonical.
// =======================================================================
template <typename T>
class Symmetric
{
public:
    using value_type = T;

    explicit Symmetric(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    Symmetric(crd::memory::IAllocator* alloc, crd::usize n) : m_alloc(alloc), m_n(n)
    {
        m_data = detail::allocate_zeroed<T>(m_alloc, n * n);
    }

    ~Symmetric() { detail::deallocate_storage(m_alloc, m_data, m_n * m_n); }

    Symmetric(const Symmetric&) = delete;
    Symmetric& operator=(const Symmetric&) = delete;

    Symmetric(Symmetric&& other) noexcept : m_alloc(other.m_alloc), m_data(other.m_data), m_n(other.m_n)
    {
        other.m_data = nullptr;
        other.m_n = 0;
    }

    Symmetric& operator=(Symmetric&& other) noexcept
    {
        if (this != &other)
        {
            detail::deallocate_storage(m_alloc, m_data, m_n * m_n);
            m_alloc = other.m_alloc;
            m_data = other.m_data;
            m_n = other.m_n;
            other.m_data = nullptr;
            other.m_n = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] bool is_square() const noexcept { return true; }
    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // Symmetric access: at(i, j) == at(j, i). Read from the canonical lower
    // half; if (i < j) swap so the read comes from the lower triangle.
    [[nodiscard]] T& at(crd::usize i, crd::usize j) noexcept
    {
        CRD_ASSERT_MSG(i < m_n && j < m_n, "Symmetric::at out of range");
        const crd::usize r = i >= j ? i : j;
        const crd::usize c = i >= j ? j : i;
        return m_data[r * m_n + c];
    }
    [[nodiscard]] const T& at(crd::usize i, crd::usize j) const noexcept
    {
        CRD_ASSERT_MSG(i < m_n && j < m_n, "Symmetric::at out of range");
        const crd::usize r = i >= j ? i : j;
        const crd::usize c = i >= j ? j : i;
        return m_data[r * m_n + c];
    }

    [[nodiscard]] Symmetric clone(crd::memory::IAllocator* dst_alloc = nullptr) const
    {
        Symmetric out(dst_alloc != nullptr ? dst_alloc : m_alloc, m_n);
        const crd::usize total = m_n * m_n;
        for (crd::usize i = 0; i < total; ++i)
        {
            out.m_data[i] = m_data[i];
        }
        return out;
    }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    crd::usize m_n = 0;
};

// =======================================================================
// Hermitian<T> — dense n×n storage; lower triangle is canonical. For
// complex T: at(i, j) for j > i returns conj(at(j, i)) by VALUE — NOT
// a reference (the upper half is a view, not stored). Writes go through
// `at_lower(i, j)` requiring i ≥ j; the diagonal must be real-valued by
// the Hermitian invariant.
// =======================================================================
template <typename T>
class Hermitian
{
public:
    using value_type = T;

    explicit Hermitian(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    Hermitian(crd::memory::IAllocator* alloc, crd::usize n) : m_alloc(alloc), m_n(n)
    {
        m_data = detail::allocate_zeroed<T>(m_alloc, n * n);
    }

    ~Hermitian() { detail::deallocate_storage(m_alloc, m_data, m_n * m_n); }

    Hermitian(const Hermitian&) = delete;
    Hermitian& operator=(const Hermitian&) = delete;

    Hermitian(Hermitian&& other) noexcept : m_alloc(other.m_alloc), m_data(other.m_data), m_n(other.m_n)
    {
        other.m_data = nullptr;
        other.m_n = 0;
    }

    Hermitian& operator=(Hermitian&& other) noexcept
    {
        if (this != &other)
        {
            detail::deallocate_storage(m_alloc, m_data, m_n * m_n);
            m_alloc = other.m_alloc;
            m_data = other.m_data;
            m_n = other.m_n;
            other.m_data = nullptr;
            other.m_n = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] bool is_square() const noexcept { return true; }
    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // Mutable lower-triangle access. CRD_ASSERT enforces i >= j.
    [[nodiscard]] T& at_lower(crd::usize i, crd::usize j) noexcept
    {
        CRD_ASSERT_MSG(i < m_n && j < m_n && i >= j, "Hermitian::at_lower requires i >= j");
        return m_data[i * m_n + j];
    }

    // Read-by-value: upper half is conj(lower). For real T, conj is a no-op.
    [[nodiscard]] T at_value(crd::usize i, crd::usize j) const noexcept
    {
        CRD_ASSERT_MSG(i < m_n && j < m_n, "Hermitian::at_value out of range");
        if (i >= j)
        {
            return m_data[i * m_n + j];
        }
        if constexpr (is_complex_value<T>::value)
        {
            return crd::hesap::conj(m_data[j * m_n + i]);
        }
        else
        {
            return m_data[j * m_n + i];
        }
    }

    [[nodiscard]] Hermitian clone(crd::memory::IAllocator* dst_alloc = nullptr) const
    {
        Hermitian out(dst_alloc != nullptr ? dst_alloc : m_alloc, m_n);
        const crd::usize total = m_n * m_n;
        for (crd::usize i = 0; i < total; ++i)
        {
            out.m_data[i] = m_data[i];
        }
        return out;
    }

private:
    // Local complex-detection trait (avoids pulling real_type.hpp; that
    // header includes dense headers and we want this to be standalone).
    template <typename U>
    struct is_complex_value : std::false_type
    {
    };
    template <typename U>
    struct is_complex_value<crd::hesap::Complex<U>> : std::true_type
    {
    };

    crd::memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    crd::usize m_n = 0;
};

// =======================================================================
// Triangular<T, Side, Diag> — dense n×n storage; only the Side half is
// canonical. UnitDiag returns T{1} for diagonal regardless of storage.
// =======================================================================
template <typename T, TriangularSide Side = TriangularSide::Lower, TriangularDiag Diag = TriangularDiag::Explicit>
class Triangular
{
public:
    using value_type = T;
    static constexpr TriangularSide side = Side;
    static constexpr TriangularDiag diag = Diag;

    explicit Triangular(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    Triangular(crd::memory::IAllocator* alloc, crd::usize n) : m_alloc(alloc), m_n(n)
    {
        m_data = detail::allocate_zeroed<T>(m_alloc, n * n);
    }

    ~Triangular() { detail::deallocate_storage(m_alloc, m_data, m_n * m_n); }

    Triangular(const Triangular&) = delete;
    Triangular& operator=(const Triangular&) = delete;

    Triangular(Triangular&& other) noexcept : m_alloc(other.m_alloc), m_data(other.m_data), m_n(other.m_n)
    {
        other.m_data = nullptr;
        other.m_n = 0;
    }

    Triangular& operator=(Triangular&& other) noexcept
    {
        if (this != &other)
        {
            detail::deallocate_storage(m_alloc, m_data, m_n * m_n);
            m_alloc = other.m_alloc;
            m_data = other.m_data;
            m_n = other.m_n;
            other.m_data = nullptr;
            other.m_n = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_n; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_n; }
    [[nodiscard]] crd::usize n() const noexcept { return m_n; }
    [[nodiscard]] bool is_square() const noexcept { return true; }
    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // Mutable access to the triangular half. CRD_ASSERT enforces side constraints.
    // For UnitDiag, writing to the diagonal is forbidden (diagonal is implicit).
    [[nodiscard]] T& at(crd::usize i, crd::usize j) noexcept
    {
        CRD_ASSERT_MSG(i < m_n && j < m_n, "Triangular::at out of range");
        if constexpr (Side == TriangularSide::Lower)
        {
            CRD_ASSERT_MSG(j <= i, "Triangular(Lower)::at requires j <= i");
        }
        else
        {
            CRD_ASSERT_MSG(j >= i, "Triangular(Upper)::at requires j >= i");
        }
        if constexpr (Diag == TriangularDiag::UnitDiag)
        {
            CRD_ASSERT_MSG(i != j, "Triangular(UnitDiag)::at cannot mutate the diagonal");
        }
        return m_data[i * m_n + j];
    }

    // Read-by-value: returns T{1} for the diagonal when UnitDiag; returns T{}
    // for the off-canonical half (it's logically zero).
    [[nodiscard]] T at_value(crd::usize i, crd::usize j) const noexcept
    {
        CRD_ASSERT_MSG(i < m_n && j < m_n, "Triangular::at_value out of range");
        if constexpr (Diag == TriangularDiag::UnitDiag)
        {
            if (i == j)
            {
                return T{1};
            }
        }
        if constexpr (Side == TriangularSide::Lower)
        {
            if (j > i)
            {
                return T{};
            }
        }
        else
        {
            if (j < i)
            {
                return T{};
            }
        }
        return m_data[i * m_n + j];
    }

    [[nodiscard]] Triangular clone(crd::memory::IAllocator* dst_alloc = nullptr) const
    {
        Triangular out(dst_alloc != nullptr ? dst_alloc : m_alloc, m_n);
        const crd::usize total = m_n * m_n;
        for (crd::usize i = 0; i < total; ++i)
        {
            out.m_data[i] = m_data[i];
        }
        return out;
    }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    crd::usize m_n = 0;
};

// =======================================================================
// Banded<T> — LAPACK column-major banded storage. (kl + ku + 1) × cols
// entries. Element (i, j) where (i - j) ∈ [-ku, kl] lives at
//   storage[(ku + i - j) + j*(kl+ku+1)]
// Outside the band, at_value returns T{}; the band-buffer never aliases.
// =======================================================================
template <typename T>
class Banded
{
public:
    using value_type = T;

    explicit Banded(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    Banded(crd::memory::IAllocator* alloc, crd::usize rows, crd::usize cols, crd::usize kl, crd::usize ku)
        : m_alloc(alloc), m_rows(rows), m_cols(cols), m_kl(kl), m_ku(ku)
    {
        m_band_height = kl + ku + 1;
        m_data = detail::allocate_zeroed<T>(m_alloc, m_band_height * cols);
    }

    ~Banded() { detail::deallocate_storage(m_alloc, m_data, m_band_height * m_cols); }

    Banded(const Banded&) = delete;
    Banded& operator=(const Banded&) = delete;

    Banded(Banded&& other) noexcept
        : m_alloc(other.m_alloc), m_data(other.m_data), m_rows(other.m_rows), m_cols(other.m_cols),
          m_kl(other.m_kl), m_ku(other.m_ku), m_band_height(other.m_band_height)
    {
        other.m_data = nullptr;
        other.m_rows = 0;
        other.m_cols = 0;
        other.m_band_height = 0;
    }

    Banded& operator=(Banded&& other) noexcept
    {
        if (this != &other)
        {
            detail::deallocate_storage(m_alloc, m_data, m_band_height * m_cols);
            m_alloc = other.m_alloc;
            m_data = other.m_data;
            m_rows = other.m_rows;
            m_cols = other.m_cols;
            m_kl = other.m_kl;
            m_ku = other.m_ku;
            m_band_height = other.m_band_height;
            other.m_data = nullptr;
            other.m_rows = 0;
            other.m_cols = 0;
            other.m_band_height = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize rows() const noexcept { return m_rows; }
    [[nodiscard]] crd::usize cols() const noexcept { return m_cols; }
    [[nodiscard]] crd::usize kl() const noexcept { return m_kl; }
    [[nodiscard]] crd::usize ku() const noexcept { return m_ku; }
    [[nodiscard]] crd::usize band_height() const noexcept { return m_band_height; }
    [[nodiscard]] bool is_square() const noexcept { return m_rows == m_cols; }
    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    [[nodiscard]] bool in_band(crd::usize i, crd::usize j) const noexcept
    {
        if (i >= m_rows || j >= m_cols)
        {
            return false;
        }
        // (i - j) ∈ [-ku, kl] means j - i ≤ ku and i - j ≤ kl
        const auto i_int = static_cast<crd::i64>(i);
        const auto j_int = static_cast<crd::i64>(j);
        const auto diff = i_int - j_int;
        return diff >= -static_cast<crd::i64>(m_ku) && diff <= static_cast<crd::i64>(m_kl);
    }

    [[nodiscard]] T& at(crd::usize i, crd::usize j) noexcept
    {
        CRD_ASSERT_MSG(in_band(i, j), "Banded::at out of band");
        const crd::usize row_in_band = m_ku + i - j;
        return m_data[row_in_band + j * m_band_height];
    }

    [[nodiscard]] T at_value(crd::usize i, crd::usize j) const noexcept
    {
        if (!in_band(i, j))
        {
            return T{};
        }
        const crd::usize row_in_band = m_ku + i - j;
        return m_data[row_in_band + j * m_band_height];
    }

    [[nodiscard]] Banded clone(crd::memory::IAllocator* dst_alloc = nullptr) const
    {
        Banded out(dst_alloc != nullptr ? dst_alloc : m_alloc, m_rows, m_cols, m_kl, m_ku);
        const crd::usize total = m_band_height * m_cols;
        for (crd::usize i = 0; i < total; ++i)
        {
            out.m_data[i] = m_data[i];
        }
        return out;
    }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    crd::usize m_rows = 0;
    crd::usize m_cols = 0;
    crd::usize m_kl = 0;
    crd::usize m_ku = 0;
    crd::usize m_band_height = 0;
};

} // namespace crd::hesap::dense
