#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// The fallback / default allocator. Wraps the C library's malloc / free on every platform, aligning inside the block
// itself: each block carries a 16-byte header just below the returned pointer (the requested size and the offset to
// the malloc'd block), plus alignment - alignof(std::max_align_t) bytes of padding when the alignment exceeds what
// malloc guarantees.
//
// Properties:
//  - Thread-safe (delegates to libc/CRT, which serialises internally).
//  - `owns()` always returns true. We don't track our pointers; if you
//    pass us something we didn't allocate, deallocate reads below it. A
//    header that fails its check is a named fatal, but that is a check, not
//    ownership: the read itself may fault. Composite allocators that need
//    real ownership tests should use a range-based or page-based wrapper.
//  - `allocation_size()` is the exact requested size on every platform, so the statistics count the same bytes at
//    allocation and at free (DIAG.11b (p): Windows' CRT cannot size an _aligned_malloc block, glibc reports the
//    usable size, and bytes_in_use never returned to its baseline).
//  - Under AddressSanitizer the header and padding are poisoned: a run before or past the block is reported.
//  - OOM is fatal via `CRD_FATAL`.
class MallocAllocator : public IAllocator
{
public:
    MallocAllocator() noexcept;
    explicit MallocAllocator(const char* name) noexcept;

    ~MallocAllocator() override = default;

    MallocAllocator(const MallocAllocator&) = delete;
    MallocAllocator& operator=(const MallocAllocator&) = delete;

    void* allocate(usize size, usize alignment = kDefaultAlignment) override;
    void deallocate(void* p) noexcept override;
    bool owns(const void* p) const noexcept override;

    usize allocation_size(const void* p) const noexcept override;

    // Non-throwing: returns nullptr on size==0 or malloc failure (no CRD_FATAL).
    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment) override;
};
} // namespace crd::memory
