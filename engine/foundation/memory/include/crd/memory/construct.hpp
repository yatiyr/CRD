#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/checked_math.hpp>

#include <new> // placement new
#include <type_traits>
#include <utility> // std::forward

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define CRD_MEMORY_CONSTRUCT_EXCEPTIONS 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#else
#define CRD_MEMORY_CONSTRUCT_EXCEPTIONS 0 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#endif

namespace crd::memory
{
// Partial construction (DIAG.3a): every helper below returns nullptr, constructing nothing, when the allocator
// refuses (an exhausted arena returns nullptr from allocate) or when the array size overflows. When a constructor
// throws, the objects already built are destroyed in reverse order and the storage goes back to the allocator before
// the exception propagates, so a failed construction leaks neither objects nor memory.

// construct<T>(allocator, args...) — allocate one T and call its constructor.
// The pointer is owned by `alloc`; release it with destroy<T>(alloc, p).
template <typename T, typename... Args> [[nodiscard]] T* construct(IAllocator& alloc, Args&&... args)
{
    void* raw = alloc.allocate(sizeof(T), alignof(T));
    if (raw == nullptr)
    {
        return nullptr; // never placement-new into a refused allocation
    }
#if CRD_MEMORY_CONSTRUCT_EXCEPTIONS
    if constexpr (!std::is_nothrow_constructible_v<T, Args&&...>)
    {
        try
        {
            return ::new (raw) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            alloc.deallocate(raw);
            throw;
        }
    }
#endif
    return ::new (raw) T(std::forward<Args>(args)...);
}

// destroy<T>(allocator, p) — call T's destructor, then free.
// Safe with nullptr; safe with trivially-destructible T (skips dtor call).
template <typename T> void destroy(IAllocator& alloc, T* p) noexcept
{
    if (!p)
    {
        return;
    }
    if constexpr (!std::is_trivially_destructible_v<T>)
    {
        p->~T();
    }
    alloc.deallocate(p);
}

// allocate_array<T>(allocator, count) — allocate uninitialised storage
// for `count` Ts. Caller is responsible for constructing/destructing.
// Use `construct_array` if you want default-construction included.
template <typename T> [[nodiscard]] T* allocate_array(IAllocator& alloc, usize count)
{
    usize bytes = 0;
    if (count == 0 || !checked_mul(sizeof(T), count, &bytes))
    {
        return nullptr; // a count whose byte size wraps would allocate too little for `count` objects
    }
    void* raw = alloc.allocate(bytes, alignof(T));
    return static_cast<T*>(raw);
}

// deallocate_array<T>(allocator, p) — counterpart for allocate_array.
// Does NOT call destructors. Use destroy_array if elements were constructed.
template <typename T> void deallocate_array(IAllocator& alloc, T* p) noexcept
{
    alloc.deallocate(p);
}

namespace detail
{
// Constructs p[0, count) in order; `built` counts the objects completed, so a throwing constructor leaves it at the
// index that threw.
template <typename T, typename... Args> void construct_each(T* p, usize count, usize& built, const Args&... args)
{
    for (; built < count; ++built)
    {
        ::new (static_cast<void*>(p + built)) T(args...);
    }
}
} // namespace detail

// construct_array<T>(allocator, count, args...) — allocate AND
// default/copy-construct `count` Ts forwarding `args` to each.
// For trivially-default-constructible T, skips the construction loop.
template <typename T, typename... Args>
[[nodiscard]] T* construct_array(IAllocator& alloc, usize count, const Args&... args)
{
    T* p = allocate_array<T>(alloc, count);
    if (!p)
    {
        return nullptr;
    }
    if constexpr (sizeof...(Args) == 0 && std::is_trivially_default_constructible_v<T>)
    {
        // Leave uninitialised — caller asked for value-init, but trivial
        // types' value-init is just zero, and we're not promising zero
        // here. If you need zeroing, do it explicitly.
    }
    else
    {
        usize built = 0;
#if CRD_MEMORY_CONSTRUCT_EXCEPTIONS
        try
        {
            detail::construct_each(p, count, built, args...);
        }
        catch (...)
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                for (usize i = built; i > 0; --i)
                {
                    p[i - 1].~T();
                }
            }
            alloc.deallocate(p);
            throw;
        }
#else
        detail::construct_each(p, count, built, args...);
#endif
    }
    return p;
}

// destroy_array<T>(allocator, p, count) — counterpart for construct_array.
template <typename T> void destroy_array(IAllocator& alloc, T* p, usize count) noexcept
{
    if (!p)
    {
        return;
    }
    if constexpr (!std::is_trivially_destructible_v<T>)
    {
        for (usize i = count; i > 0; --i)
        {
            p[i - 1].~T();
        }
    }
    alloc.deallocate(p);
}
} // namespace crd::memory
