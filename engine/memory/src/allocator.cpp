#include <crd/core/assert.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/memory/log_channel.hpp>

#include <cstring>

namespace crd::memory
{
// Module-private log channel definition. (User-facing declaration lives in
// include/crd/memory/log_channel.hpp.)
CRD_DEFINE_LOG_CHANNEL(g_log_memory, "Memory", ::crd::log::LogLevel::Info)

// ---- Default reallocate -------------------------------------------------
// Allocate-copy-deallocate fallback. Specialised allocators (TLSF later)
// can override for in-place growth.
void* IAllocator::reallocate(void* p, usize old_size, usize new_size, usize alignment)
{
    if (new_size == 0)
    {
        deallocate(p);
        return nullptr;
    }
    if (p == nullptr)
    {
        return allocate(new_size, alignment);
    }

    void* new_ptr = allocate(new_size, alignment);
    const usize copy_bytes = (old_size < new_size) ? old_size : new_size;
    if (copy_bytes > 0)
    {
        std::memcpy(new_ptr, p, copy_bytes);
    }
    deallocate(p);
    return new_ptr;
}

// ---- Default global allocator ------------------------------------------
//
// We use a function-local static to dodge static-init-order issues: the
// first caller constructs the instance, every later caller gets the same
// pointer. The MallocAllocator has no destructor work (libc cleans up
// any leaked OS allocations on process exit), so we let it live until the
// OS reaps the process.
IAllocator* default_allocator() noexcept
{
    static MallocAllocator s_instance{"DefaultAllocator"};
    return &s_instance;
}
} // namespace crd::memory
