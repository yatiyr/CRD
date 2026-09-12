#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Vector<T> — owning, allocator-aware dense vector.
//
// Per Phase 3.1.6 v0a (substrate) + v0b (BLAS L1 first consumer): the
// upper-layer typed entry point that BLAS L1 / L2 / L3 ops consume; the
// lower-layer kernels operate on `ConstSpan<T>` / `Span<T>` (ADR-0078 §5
// two-layer typed architecture — kernels are raw, the surface is typed).
//
// Design:
//   - `IAllocator*` at construction (not a template parameter); container
//     type stays stable when the allocator changes (matches crd-containers
//     discipline).
//   - Move-only by default. Copy via explicit `clone(alloc)`. Reason: a
//     thousand-element f64 vector has the same shape as `Array<f64>`;
//     accidental copies are a perf trap. Per D15 (queued for ADR-0065 §14).
//   - Storage is contiguous `T[size]`, aligned for SIMD by the allocator
//     (TLSF returns 16-byte-aligned blocks; v0b's SIMD path doesn't
//     require 32-byte yet).
//   - No exceptions; OOM is fatal via IAllocator's contract.
//
// Public surface tracks the v0a phase-doc declaration with one addition:
// `clone(alloc)` for explicit deep copies. `data()` / `span()` return raw
// pointers / spans for the lower-layer kernel boundary.
// -----------------------------------------------------------------------

template <typename T>
class Vector
{
public:
    using value_type = T;

    explicit Vector(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc) {}

    Vector(crd::memory::IAllocator* alloc, crd::usize n) : m_alloc(alloc), m_size(n)
    {
        if (n > 0)
        {
            m_data = static_cast<T*>(m_alloc->allocate(n * sizeof(T), alignof(T) > 16 ? alignof(T) : 16));
            if constexpr (std::is_trivially_constructible_v<T>)
            {
                std::memset(m_data, 0, n * sizeof(T));
            }
            else
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    new (m_data + i) T{};
                }
            }
        }
    }

    Vector(crd::memory::IAllocator* alloc, std::initializer_list<T> il)
        : Vector(alloc, il.size())
    {
        crd::usize i = 0;
        for (const T& v : il)
        {
            m_data[i++] = v;
        }
    }

    Vector(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> data)
        : Vector(alloc, data.size())
    {
        for (crd::usize i = 0; i < data.size(); ++i)
        {
            m_data[i] = data[i];
        }
    }

    ~Vector() { release(); }

    // Move-only — see class comment + D15.
    Vector(const Vector&) = delete;
    Vector& operator=(const Vector&) = delete;

    Vector(Vector&& other) noexcept
        : m_alloc(other.m_alloc), m_data(other.m_data), m_size(other.m_size)
    {
        other.m_data = nullptr;
        other.m_size = 0;
    }

    Vector& operator=(Vector&& other) noexcept
    {
        if (this != &other)
        {
            release();
            m_alloc = other.m_alloc;
            m_data = other.m_data;
            m_size = other.m_size;
            other.m_data = nullptr;
            other.m_size = 0;
        }
        return *this;
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] T* data() noexcept { return m_data; }
    [[nodiscard]] const T* data() const noexcept { return m_data; }

    [[nodiscard]] T& operator()(crd::usize i) noexcept
    {
        CRD_ASSERT_MSG(i < m_size, "Vector::operator() out of range");
        return m_data[i];
    }

    [[nodiscard]] const T& operator()(crd::usize i) const noexcept
    {
        CRD_ASSERT_MSG(i < m_size, "Vector::operator() out of range");
        return m_data[i];
    }

    [[nodiscard]] crd::containers::Span<T> span() noexcept
    {
        return crd::containers::Span<T>{m_data, m_size};
    }

    [[nodiscard]] crd::containers::ConstSpan<T> span() const noexcept
    {
        return crd::containers::ConstSpan<T>{m_data, m_size};
    }

    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    [[nodiscard]] Vector clone(crd::memory::IAllocator* dst_alloc = nullptr) const
    {
        Vector out(dst_alloc != nullptr ? dst_alloc : m_alloc, m_size);
        for (crd::usize i = 0; i < m_size; ++i)
        {
            out.m_data[i] = m_data[i];
        }
        return out;
    }

    void fill(const T& v) noexcept
    {
        for (crd::usize i = 0; i < m_size; ++i)
        {
            m_data[i] = v;
        }
    }

private:
    void release() noexcept
    {
        if (m_data != nullptr)
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                for (crd::usize i = 0; i < m_size; ++i)
                {
                    m_data[i].~T();
                }
            }
            m_alloc->deallocate(m_data);
            m_data = nullptr;
            m_size = 0;
        }
    }

    crd::memory::IAllocator* m_alloc = nullptr;
    T* m_data = nullptr;
    crd::usize m_size = 0;
};

} // namespace crd::hesap::dense
