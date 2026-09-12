#pragma once

#include <crd/memory/allocator.hpp>

namespace crd::memory
{
// The fallback / default allocator. Wraps the platform's aligned-malloc:
//   Windows : _aligned_malloc / _aligned_free
//   POSIX   : aligned_alloc + free  (or posix_memalign on older glibc)
//
// Properties:
//  - Thread-safe (delegates to libc/CRT, which serialises internally).
//  - `owns()` always returns true. We don't track our pointers; if you
//    pass us something we didn't allocate, you'll crash on deallocate.
//    Composite allocators that need real ownership tests should use a
//    range-based or page-based wrapper instead.
//  - `allocation_size()` queries the platform allocator when possible
//    (`_msize` on Windows, `malloc_usable_size` on glibc); returns 0 otherwise.
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
