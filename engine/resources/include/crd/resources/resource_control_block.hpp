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

// Internal per-resource control block owned by ResourceManager.
//
// ResourceHandle<T> stores a raw pointer to this block and uses the
// inherited RefCounted refcount to track live handle copies.
//
// Lifetime (v1c):
//   - Created by ResourceManager::load_sync_impl on first successful load.
//   - `permanent = true`: block lives in m_handles until the manager is
//     destroyed. Refs may drop to 0; the block stays (eviction is v1g).
//   - `permanent = false`: block is NOT in m_handles (Failed load that was
//     not cached). When refs drops to 0 the last handle frees it.
//
// Thread safety: payload and state writes happen under the load mutex (v1d).
// In v1c all loads are synchronous on the calling thread.
struct ResourceControlBlock : public crd::memory::RefCounted<ResourceControlBlock>
{
    ResourceId               id{};
    crd::u32                 type_fourcc = 0;
    bool                     permanent   = false;
    std::atomic<crd::u32>   generation{0};
    std::atomic<LoadState>   state{LoadState::Unloaded};
    // Stores crd::jobs::Counter* as void* to avoid jobs.hpp in this header.
    // Written once by load_async_impl; claimed by wait_ready() or the leak-fix in load_async_impl.
    std::atomic<void*>       load_counter{nullptr};
    void*                    payload     = nullptr;
    ILoader*                 loader      = nullptr;
    crd::memory::IAllocator* alloc       = nullptr;
};

} // namespace crd::resources
