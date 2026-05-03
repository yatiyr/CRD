#include <crd/resources/resource_handle.hpp>

#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_control_block.hpp>

#include <thread>

namespace crd::resources
{

void ResourceHandleBase::release_block() noexcept
{
    if (!m_block)
    {
        return;
    }
    if (m_block->release() == 0U && !m_block->permanent)
    {
        // Non-permanent (failed) block: free it.
        if (m_block->payload && m_block->loader)
        {
            m_block->loader->unload(m_block->payload);
            m_block->payload = nullptr;
        }
        crd::memory::IAllocator* alloc = m_block->alloc;
        m_block->~ResourceControlBlock();
        alloc->deallocate(m_block);
    }
    m_block = nullptr;
}

LoadState ResourceHandleBase::wait_ready()
{
    if (!m_block)
    {
        return LoadState::Unloaded;
    }

    // Check state. Whether terminal or not, always attempt to claim the counter:
    // - If the job finished before wait_ready() was called, load_async_impl's leak-fix
    //   may or may not have reclaimed it depending on timing. Claim it here if it wasn't.
    // - If sync-loaded (no counter), exchange returns nullptr safely.
    LoadState s = m_block->state.load(std::memory_order_acquire);
    if (s != LoadState::Queued && s != LoadState::Loading)
    {
        // Terminal state — try to claim any counter that load_async_impl left behind.
        void* raw = m_block->load_counter.exchange(nullptr, std::memory_order_acquire);
        if (raw != nullptr)
        {
            crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw));
        }
        return m_block->state.load(std::memory_order_acquire);
    }

    // Try to claim the async job counter for a fiber-cooperative wait.
    // load_async_impl stores the Counter* here after submitting the job.
    void* raw = m_block->load_counter.exchange(nullptr, std::memory_order_acquire);
    if (raw != nullptr)
    {
        crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw));
        return m_block->state.load(std::memory_order_acquire);
    }

    // Counter not yet stored (load_async_impl is between run() and the store),
    // or already claimed by another wait_ready() caller. Spin until terminal.
    while (s == LoadState::Queued || s == LoadState::Loading)
    {
        std::this_thread::yield();
        s = m_block->state.load(std::memory_order_acquire);
    }

    // One final claim attempt: if load_async_impl stored the counter after our
    // first exchange (and its leak-fix path didn't reclaim it), claim it now.
    raw = m_block->load_counter.exchange(nullptr, std::memory_order_acquire);
    if (raw != nullptr)
    {
        crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw));
    }

    return m_block->state.load(std::memory_order_acquire);
}

} // namespace crd::resources
