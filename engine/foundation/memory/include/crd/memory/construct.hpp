#pragma once

#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <new> // placement new
#include <type_traits>
#include <utility> // std::forward

namespace crd::memory
{
// construct<T>(allocator, args...) — allocate one T and call its constructor.
// The pointer is owned by `alloc`; release it with destroy<T>(alloc, p).
template <typename T, typename... Args> [[nodiscard]] T* construct(IAllocator& alloc, Args&&... args)
{
    void* raw = alloc.allocate(sizeof(T), alignof(T));
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
    if (count == 0)
    {
        return nullptr;
    }
    void* raw = alloc.allocate(sizeof(T) * count, alignof(T));
    return static_cast<T*>(raw);
}

// deallocate_array<T>(allocator, p) — counterpart for allocate_array.
// Does NOT call destructors. Use destroy_array if elements were constructed.
template <typename T> void deallocate_array(IAllocator& alloc, T* p) noexcept
{
    alloc.deallocate(p);
}

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
        for (usize i = 0; i < count; ++i)
        {
            ::new (static_cast<void*>(p + i)) T(args...);
        }
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
