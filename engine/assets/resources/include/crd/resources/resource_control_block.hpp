#pragma once

#include <atomic>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/ref_counted.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{

class ILoader;

// Which 2Q eviction queue this block currently occupies (v1g).
enum class EvictQueue : crd::u8 { None, A1in, Am };

// Internal per-resource control block owned by ResourceManager.
//
// ResourceHandle<T> stores a raw pointer to this block and uses the
// inherited RefCounted refcount to track live handle copies.
//
// Lifetime:
//   - Created by ResourceManager::load_sync_impl on first successful load.
//   - `permanent = true`: block lives in m_handles until the manager is
//     destroyed. Refs may drop to 0; the block stays and may be evicted (v1g).
//   - `permanent = false`: block is NOT in m_handles (Failed load that was
//     not cached). When refs drops to 0 the last handle frees it.
//
// Thread safety: payload and state writes happen under the load mutex (v1d).
struct ResourceControlBlock : public crd::memory::RefCounted<ResourceControlBlock>
{
    ResourceId               id{};
    crd::u32                 type_fourcc = 0;
    bool                     permanent   = false;
    bool                     pinned      = false;       // exempt from eviction (v1g)
    EvictQueue               evict_queue = EvictQueue::None;
    crd::u64                 payload_size = 0;          // bytes counted against memory budget (v1g)
    std::atomic<crd::u32>   generation{0};
    std::atomic<LoadState>   state{LoadState::Unloaded};
    // Stores crd::jobs::Counter* as void* to avoid jobs.hpp in this header.
    // Written once by load_async_impl; claimed by wait_ready() or the leak-fix in load_async_impl.
    std::atomic<void*>       load_counter{nullptr};
    // Atomic so hot-reload can swap the payload concurrently with get() calls.
    // Writers: exchange(acq_rel) then state.store(release); readers: state.load(acquire) then payload.load(acquire).
    std::atomic<void*>       payload{nullptr};
    ILoader*                 loader      = nullptr;
    crd::memory::IAllocator* alloc       = nullptr;
};

} // namespace crd::resources
