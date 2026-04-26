#pragma once

#include <crd/core/types.hpp>

namespace crd::memory
{
// Default allocation alignment. Big enough for SSE/AVX SIMD, all standard
// primitive types, and most engine vertex/struct layouts. If you don't
// know what alignment to ask for, this is the right answer.
inline constexpr usize kDefaultAlignment = 16;

// Common cache line size on x86-64 / ARM64. Used to pad hot/cold fields
// apart and to size cacheline-aligned ring buffers.
inline constexpr usize kCachelineSize = 64;

// Smallest valid alignment any allocator will accept.
inline constexpr usize kMinAlignment = alignof(void*);

// True iff `x` is a power of two (and non-zero).
constexpr bool is_pow2(usize x) noexcept
{
    return x != 0 && (x & (x - 1)) == 0;
}

// Round `value` up to the next multiple of `alignment`.
// Precondition: alignment is a power of two.
constexpr usize align_up(usize value, usize alignment) noexcept
{
    return (value + (alignment - 1)) & ~(alignment - 1);
}

// Round `value` down to the previous multiple of `alignment`.
// Precondition: alignment is a power of two.
constexpr usize align_down(usize value, usize alignment) noexcept
{
    return value & ~(alignment - 1);
}

// True if `p` is `alignment`-aligned.
inline bool is_aligned(const void* p, usize alignment) noexcept
{
    return (reinterpret_cast<usize>(p) & (alignment - 1)) == 0;
}

// Adjust a raw pointer up to the next `alignment` boundary.
inline void* align_up_ptr(void* p, usize alignment) noexcept
{
    return reinterpret_cast<void*>(align_up(reinterpret_cast<usize>(p), alignment));
}
} // namespace crd::memory
