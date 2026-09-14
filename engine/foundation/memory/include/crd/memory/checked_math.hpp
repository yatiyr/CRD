#pragma once

// crd-memory -- overflow-checked size/stride/alignment/page arithmetic (DIAG.3a).
//
// Allocation sizing computes count*stride, size+header, align-up-to-boundary and pages-for-bytes;
// each can overflow near the address-width boundary and silently return a too-small size, the
// classic allocator integer-overflow bug. These helpers do the arithmetic and report overflow
// instead of wrapping. All are constexpr and branch-cheap; they add no per-allocation heap cost,
// so production layout/performance is preserved (DIAG.3a: diagnostic metadata as side storage,
// not fatter allocations).
//
// Convention: every function returns true on success and writes *out; on overflow it returns
// false and leaves *out unspecified. Callers must check the bool before using the size.
//
// Contract: docs/design/runtime-diagnostics.md#diag-3a; ADR-0133; DG02/DG03.

#include <crd/core/types.hpp>
#include <crd/memory/alignment.hpp>

#include <cstdint> // SIZE_MAX

namespace crd::memory
{

// a + b, overflow-checked.
[[nodiscard]] constexpr bool checked_add(usize a, usize b, usize* out) noexcept
{
    if (a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

// a * b, overflow-checked (the count*stride case).
[[nodiscard]] constexpr bool checked_mul(usize a, usize b, usize* out) noexcept
{
    if (a != 0U && b > SIZE_MAX / a)
        return false;
    *out = a * b;
    return true;
}

// count elements of `stride` bytes each -- an array's byte size, overflow-checked.
[[nodiscard]] constexpr bool checked_array_size(usize count, usize stride, usize* out) noexcept
{
    return checked_mul(count, stride, out);
}

// Round `value` up to `alignment` (a power of two), detecting the wrap that plain
// (value + alignment - 1) & ~(alignment - 1) suffers when value is within (alignment-1) of
// SIZE_MAX. Returns false for a non-power-of-two alignment or on overflow.
[[nodiscard]] constexpr bool checked_align_up(usize value, usize alignment, usize* out) noexcept
{
    if (alignment == 0U || !is_pow2(alignment))
        return false;
    usize sum = 0U;
    if (!checked_add(value, alignment - 1U, &sum))
        return false;
    *out = sum & ~(alignment - 1U);
    return true;
}

// Number of `page_size`-byte pages needed to hold `bytes`, overflow-checked. `page_size` must be
// non-zero (need not be a power of two -- some page/chunk sizes are not).
[[nodiscard]] constexpr bool checked_page_count(usize bytes, usize page_size, usize* out) noexcept
{
    if (page_size == 0U)
        return false;
    usize sum = 0U;
    if (!checked_add(bytes, page_size - 1U, &sum))
        return false;
    *out = sum / page_size;
    return true;
}

// The byte size of `count` elements of `stride`, each rounded up so successive elements keep
// `alignment` -- i.e. checked_align_up(stride) then checked_mul(count). Overflow-checked end to
// end. Useful for pool/slab sizing where element stride is padded to an alignment.
[[nodiscard]] constexpr bool checked_padded_array_size(usize count, usize stride, usize alignment,
                                                       usize* out) noexcept
{
    usize padded = 0U;
    if (!checked_align_up(stride, alignment, &padded))
        return false;
    return checked_mul(count, padded, out);
}

} // namespace crd::memory
