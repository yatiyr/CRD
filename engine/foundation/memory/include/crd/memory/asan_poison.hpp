#pragma once

// crd-memory -- AddressSanitizer poisoning of logical (sub-)allocations (DIAG.3b).
//
// A bump/arena allocator hands out slices of one big parent allocation, so the parent's ASan
// redzones sit only at the ends of the whole buffer -- ASan cannot see a use of a freed arena
// slice or an over-read into the next slice. Manually poisoning freed/unhanded ranges and
// unpoisoning live ones restores per-logical-allocation boundaries. Every helper compiles to
// nothing unless the build defines AddressSanitizer, so production layout/performance is
// unchanged (DIAG.3a/3b: diagnostic metadata as side storage, not fatter allocations).
//
// Do NOT use these on an offset/GPU allocator whose "address" is an unmapped device range --
// that is a logical-range check, not CPU memory (see DIAG.3a). These are CPU-memory only.
//
// Contract: docs/design/runtime-diagnostics.md#diag-3b; ADR-0133; DG02.

#include <crd/core/types.hpp>

#if defined(__SANITIZE_ADDRESS__)
#define CRD_MEM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_MEM_ASAN 1
#endif
#endif
#ifndef CRD_MEM_ASAN
#define CRD_MEM_ASAN 0
#endif

#if CRD_MEM_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace crd::memory
{

// Mark [p, p+n) as poisoned: any load/store ASan-faults until unpoisoned. No-op without ASan.
inline void asan_poison(const void* p, usize n) noexcept
{
#if CRD_MEM_ASAN
    if (n != 0U)
        __asan_poison_memory_region(p, n);
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
        __asan_unpoison_memory_region(p, n);
#else
    (void)p;
    (void)n;
#endif
}

} // namespace crd::memory
