#pragma once

// crd-memory -- AddressSanitizer poisoning of logical (sub-)allocations.
//
// A bump/arena allocator hands out slices of one big parent allocation, so the parent's ASan
// redzones sit only at the ends of the whole buffer -- ASan cannot see a use of a freed arena
// slice or an over-read into the next slice. Manually poisoning freed/unhanded ranges and
// unpoisoning live ones restores per-logical-allocation boundaries. Every helper compiles to
// nothing unless the build defines AddressSanitizer, so production layout/performance is
// unchanged (diagnostic metadata as side storage, not fatter allocations).
//
// Shadow granularity: ASan tracks addressability per 8-byte granule, recording only how many leading bytes of a
// granule are addressable. Poisoning therefore rounds inward (a granule's live prefix stays addressable) and
// unpoisoning rounds outward (a granule's whole prefix becomes addressable). Neither can raise a false report, but a
// boundary is byte-exact only when the live range starts on a granule (8-byte aligned) and the poisoned bytes run to
// the end of their granule. Allocators that hand out 8-aligned slices get exact one-byte over/underrun detection.
// ASan names a fault in a partially addressable granule after the next granule's shadow: a slice followed by poisoned
// padding reports use-after-poison, one packed against a live neighbour still faults but reports unknown-crash.
//
// Do NOT use these on an offset/GPU allocator whose "address" is an unmapped device range --
// that is a logical-range check, not CPU memory. These are CPU-memory only.
//
// Contract: docs/design/runtime-diagnostics.md; ADR-0133.

#include <crd/core/types.hpp>

#include <cstdint>

#if defined(__SANITIZE_ADDRESS__)
#define CRD_MEM_ASAN 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_MEM_ASAN 1 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#endif
#endif
#ifndef CRD_MEM_ASAN
#define CRD_MEM_ASAN 0 // NOLINT(cppcoreguidelines-macro-usage): controls #if branches.
#endif

#if CRD_MEM_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

namespace crd::memory
{

// Mark [p, p+n) as poisoned: any load/store ASan-faults until unpoisoned. No-op without ASan.
inline void asan_poison(const void* p, usize n) noexcept
{
#if CRD_MEM_ASAN
    if (n != 0U)
    {
        __asan_poison_memory_region(p, n);
    }
#else
    (void)p;
    (void)n;
#endif
}

// Mark [p, p+n) as addressable again. No-op without ASan.
inline void asan_unpoison(const void* p, usize n) noexcept
{
#if CRD_MEM_ASAN
    if (n != 0U)
    {
        __asan_unpoison_memory_region(p, n);
    }
#else
    (void)p;
    (void)n;
#endif
}

// Container live range (DIAG.3b): moves the boundary between the live prefix and the unused capacity of the
// contiguous storage [storage, storage + capacity_bytes) from `old_live_bytes` to `new_live_bytes`. The unused
// capacity carries ASan's container-overflow marking, so a read past the live prefix reports container-overflow.
// The annotation covers only the whole granules inside the storage (start rounded up, end rounded down, both marks
// clamped into that range): ASan cannot mark part of a granule shared with a neighbour, and an older runtime refuses
// an unaligned start. Detection is therefore byte-exact when the storage starts on a granule and its byte size is a
// multiple of 8; otherwise the edge granules stay as the allocator left them. Fresh storage is fully live, each call
// starts from the boundary the previous call left, and the owner moves the boundary back to `capacity_bytes` before
// it frees the storage, so the allocator gets back exactly what it handed out. No-op without ASan.
inline void asan_annotate_live_range(const void* storage, usize capacity_bytes, usize old_live_bytes,
                                     usize new_live_bytes) noexcept
{
#if CRD_MEM_ASAN
    if (storage == nullptr || old_live_bytes == new_live_bytes)
    {
        return;
    }
    constexpr std::uintptr_t granule = 8U;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(storage);
    const std::uintptr_t low = (begin + granule - 1U) & ~(granule - 1U);
    const std::uintptr_t high = (begin + capacity_bytes) & ~(granule - 1U);
    if (high <= low)
    {
        return;
    }
    const auto clamp = [low, high](std::uintptr_t mark) noexcept
    {
        if (mark < low)
        {
            return low;
        }
        return mark > high ? high : mark;
    };
    const std::uintptr_t old_mid = clamp(begin + old_live_bytes);
    const std::uintptr_t new_mid = clamp(begin + new_live_bytes);
    if (old_mid != new_mid)
    {
        // NOLINTBEGIN(performance-no-int-to-ptr): the sanitizer interface takes addresses as pointers.
        const void* const first = reinterpret_cast<const void*>(low);
        const void* const last = reinterpret_cast<const void*>(high);
        __sanitizer_annotate_contiguous_container(first, last, reinterpret_cast<const void*>(old_mid),
                                                  reinterpret_cast<const void*>(new_mid));
        // NOLINTEND(performance-no-int-to-ptr)
    }
#else
    (void)storage;
    (void)capacity_bytes;
    (void)old_live_bytes;
    (void)new_live_bytes;
#endif
}

// True when the byte at `p` is poisoned. Always false without ASan, so a caller can assert the poisoned state only
// where the instrument exists. Querying the shadow is not an access: it never reports.
[[nodiscard]] inline bool asan_is_poisoned(const void* p) noexcept
{
#if CRD_MEM_ASAN
    return __asan_address_is_poisoned(p) != 0;
#else
    (void)p;
    return false;
#endif
}

} // namespace crd::memory
