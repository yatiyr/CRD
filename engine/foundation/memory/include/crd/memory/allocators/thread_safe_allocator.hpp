#pragma once

#include <crd/core/assert.hpp>
#include <crd/memory/allocator.hpp>

#include <mutex>

namespace crd::memory
{
// ThreadSafeAllocator — serializes every call to an inner IAllocator behind one
// mutex, making a non-thread-safe allocator (TlsfAllocator, GrowableTlsfAllocator,
// VirtualMemoryAllocator, ...) safe for concurrent allocate/free from multiple
// threads.
//
// Why it exists: most crd allocators are single-threaded by convention (the
// branch-free hot path is the point). But some consumers allocate concurrently —
// e.g. the ResourceManager dispatches loader jobs that allocate payloads from the
// manager's allocator WITHOUT holding the manager mutex (so transitive loads don't
// deadlock). Wrapping that allocator here makes concurrent loader allocation safe
// regardless of which allocator the caller supplied.
//
// Cost: one mutex acquire per call. Use only where concurrent access is real (the
// resource-load path, async staging) — never on a per-frame hot loop, where each
// thread should own its own arena instead. The inner allocator must outlive this.
class ThreadSafeAllocator final : public IAllocator
{
public:
    explicit ThreadSafeAllocator(IAllocator* inner, const char* name = "ThreadSafeAllocator") : m_inner(inner)
    {
        CRD_ASSERT(inner != nullptr);
        m_name = name;
    }

    ThreadSafeAllocator(const ThreadSafeAllocator&)            = delete;
    ThreadSafeAllocator& operator=(const ThreadSafeAllocator&) = delete;

    void* allocate(usize size, usize alignment = kDefaultAlignment) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_inner->allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_inner->deallocate(p);
    }
    [[nodiscard]] bool owns(const void* p) const noexcept override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_inner->owns(p);
    }
    void* reallocate(void* p, usize old_size, usize new_size, usize alignment = kDefaultAlignment) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_inner->reallocate(p, old_size, new_size, alignment);
    }
    [[nodiscard]] usize allocation_size(const void* p) const noexcept override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_inner->allocation_size(p);
    }
    [[nodiscard]] void* try_allocate(usize size, usize alignment = kDefaultAlignment) override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_inner->try_allocate(size, alignment);
    }

    [[nodiscard]] IAllocator* inner() const noexcept { return m_inner; }

private:
    IAllocator*        m_inner;
    mutable std::mutex m_mutex;
};
} // namespace crd::memory
