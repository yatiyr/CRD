#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/log_channel.hpp>

#include <cstdlib>
#include <cstring>

#if CRD_OS_WINDOWS
#include <malloc.h> // _aligned_malloc, _aligned_free, _aligned_msize
#else
#include <malloc.h> // malloc_usable_size on glibc; harmless elsewhere
#endif

namespace crd::memory
{
namespace
{
// Cross-platform aligned allocation. Always rounds size up to a
// multiple of `alignment` because some libc implementations
// (notably C11 `aligned_alloc`) require it.
void* platform_aligned_alloc(usize size, usize alignment) noexcept
{
#if CRD_OS_WINDOWS
    return ::_aligned_malloc(size, alignment);
#else
    // C11 aligned_alloc requires size % alignment == 0
    const usize rounded = align_up(size, alignment);
    return std::aligned_alloc(alignment, rounded);
#endif
}

void platform_aligned_free(void* p) noexcept
{
#if CRD_OS_WINDOWS
    ::_aligned_free(p);
#else
    std::free(p);
#endif
}

usize platform_allocation_size(const void* p) noexcept
{
    if (!p)
    {
        return 0;
    }
#if CRD_OS_WINDOWS
    // _aligned_msize requires the original alignment+offset; we don't
    // store either, so we can't safely query. Return 0 = unknown.
    (void)p;
    return 0;
#elif defined(__GLIBC__)
    return ::malloc_usable_size(const_cast<void*>(p));
#else
    (void)p;
    return 0;
#endif
}
} // namespace

MallocAllocator::MallocAllocator() noexcept : MallocAllocator("MallocAllocator") {}

MallocAllocator::MallocAllocator(const char* name) noexcept
{
    m_name = name;
}

void* MallocAllocator::allocate(usize size, usize alignment)
{
    CRD_ASSERT(size > 0);
    CRD_ASSERT(is_pow2(alignment));
    if (alignment < kMinAlignment)
    {
        alignment = kMinAlignment;
    }

    void* p = platform_aligned_alloc(size, alignment);
    if (!p)
    {
        CRD_LOG_CRITICAL(g_log_memory, "MallocAllocator OOM (requested {} bytes, alignment {})", size, alignment);
        CRD_FATAL("Out of memory");
    }
    m_stats.on_allocate(size);
    return p;
}

void MallocAllocator::deallocate(void* p) noexcept
{
    if (!p)
    {
        return;
    }
    const usize known = allocation_size(p);
    platform_aligned_free(p);
    m_stats.on_deallocate(known);
}

bool MallocAllocator::owns(const void* p) const noexcept
{
    // We cannot tell. Convention: malloc-style allocators trust the caller.
    // Composite allocators that need real ownership tests should use a
    // range-based wrapper.
    return p != nullptr;
}

usize MallocAllocator::allocation_size(const void* p) const noexcept
{
    return platform_allocation_size(p);
}
} // namespace crd::memory
