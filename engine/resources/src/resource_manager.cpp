#include <crd/resources/resource_manager.hpp>

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/construct.hpp>
#include <crd/platform/async_file.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/log_channel.hpp>
#include <crd/resources/resource_control_block.hpp>

#include <cstring>
#include <thread>

namespace crd::resources
{

// ── Cycle-detection visiting stack (thread-local) ──────────────────────────
//
// Pushed before dispatching to a loader, popped after. A recursive load_sync
// or run_load_job call that finds its own id on the stack signals a cycle.
//
// Thread-local: each OS thread has its own stack. Fiber migration across an
// await point inside loader->load() would corrupt the stack. Safe because:
//  - load_sync: no await point inside loader->load() in v1d.
//  - run_load_job: runs in a fiber; loader->load() may call load_sync but
//    load_sync releases the mutex before I/O, so no blocking point on THIS
//    thread's visiting stack.

namespace
{

constexpr crd::usize kMaxVisitDepth = 64;
thread_local ResourceId  tl_visiting[kMaxVisitDepth];
thread_local crd::usize  tl_visit_count = 0;

[[nodiscard]] bool visiting_contains(ResourceId id) noexcept
{
    for (crd::usize i = 0; i < tl_visit_count; ++i)
    {
        if (tl_visiting[i] == id)
        {
            return true;
        }
    }
    return false;
}

void visiting_push(ResourceId id) noexcept
{
    CRD_ASSERT_MSG(tl_visit_count < kMaxVisitDepth,
                   "ResourceManager: dependency chain exceeds maximum depth (64)");
    tl_visiting[tl_visit_count++] = id;
}

void visiting_pop() noexcept
{
    CRD_ASSERT_MSG(tl_visit_count > 0, "ResourceManager: visiting stack underflow");
    --tl_visit_count;
}

// Look up a null-terminated string in the STRP pool at byte offset `idx`.
crd::containers::StringView strp_get(
    crd::containers::ConstSpan<crd::u8> pool,
    crd::u32                            idx) noexcept
{
    if (idx >= static_cast<crd::u32>(pool.size()))
    {
        return {};
    }
    const char* begin = reinterpret_cast<const char*>(pool.data() + idx);
    const char* end   = begin;
    const char* limit = reinterpret_cast<const char*>(pool.data() + pool.size());
    while (end < limit && *end != '\0')
    {
        ++end;
    }
    return crd::containers::StringView(begin, static_cast<crd::usize>(end - begin));
}

// Allocate a non-permanent Failed block owned by the caller's handle.
// Used for all error paths where no cached block exists.
ResourceControlBlock* make_failed_block(crd::memory::IAllocator* alloc, ResourceId id)
{
    void* raw = alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
    auto* block = new (raw) ResourceControlBlock();
    block->id    = id;
    block->alloc = alloc;
    block->state.store(LoadState::Failed, std::memory_order_release);
    return block;
}

// Heap-allocated context for an in-flight async load job.
struct AsyncLoadCtx
{
    ResourceManager*          manager;
    ResourceControlBlock*     block;
    ILoader*                  loader;
    crd::containers::String   pack_path;
    crd::u64                  blob_offset;
    crd::u64                  blob_size;
    ResourceId                id;
    crd::u32                  type_fourcc;
    bool                      re_issue = false;
};

// SBO-compatible job callable for async (buffered) loads.
struct LoadJobFn
{
    void* ctx_ptr;
    void operator()() noexcept { ResourceManager::run_load_job(ctx_ptr); }
};

static_assert(sizeof(LoadJobFn)  <= 41U, "LoadJobFn must fit in 41-byte SBO");
static_assert(alignof(LoadJobFn) <= 8U,  "LoadJobFn alignment must be ≤ 8");
static_assert(std::is_trivially_copyable_v<LoadJobFn>);
static_assert(std::is_trivially_destructible_v<LoadJobFn>);

// Heap-allocated context for a streaming load job (v1g).
struct StreamLoadCtx
{
    ResourceManager*          manager;
    ResourceControlBlock*     block;
    ILoader*                  loader;
    crd::containers::String   pack_path;
    crd::u64                  blob_offset;
    crd::u64                  blob_size;
    ResourceId                id;
    crd::u32                  type_fourcc;
    bool                      re_issue = false;
};

// SBO-compatible job callable for streaming loads.
struct StreamLoadJobFn
{
    void* ctx_ptr;
    void operator()() noexcept { ResourceManager::run_stream_load_job(ctx_ptr); }
};

static_assert(sizeof(StreamLoadJobFn)  <= 41U, "StreamLoadJobFn must fit in 41-byte SBO");
static_assert(alignof(StreamLoadJobFn) <= 8U,  "StreamLoadJobFn alignment must be ≤ 8");
static_assert(std::is_trivially_copyable_v<StreamLoadJobFn>);
static_assert(std::is_trivially_destructible_v<StreamLoadJobFn>);

// Maximum number of ghost entries kept in A1out before the oldest is dropped.
constexpr crd::usize kMaxA1out = 256U;

} // anonymous namespace

// ── Construction / destruction ─────────────────────────────────────────────

ResourceManager::ResourceManager(crd::memory::IAllocator* a)
    : m_alloc(a)
    , m_loaders(a)
    , m_mounts(a)
    , m_live(a)
    , m_handles(a)
    , m_in_flight(a)
    , m_reload_subs(a)
    , m_pack_watches(a)
    , m_deferred_frees(a)
    , m_a1in(a)
    , m_am(a)
    , m_a1out(a)
    , m_a1out_set(a)
    , m_pin_counts(a)
{
}

ResourceManager::~ResourceManager()
{
    // Callers must drain all async loads before destruction (e.g. call
    // jobs::shutdown() or wait_ready() on all handles first).
    CRD_ASSERT_MSG(m_in_flight.empty(),
                   "ResourceManager destroyed while async loads are in flight; "
                   "call jobs::shutdown() or wait_ready() on all handles first");

    for (DeferredFree& df : m_deferred_frees)
    {
        df.loader->unload(df.payload);
    }

    // Two-pass teardown: unload payloads before freeing blocks.
    // Payloads (e.g. MaterialResource) may hold ResourceHandles to other blocks
    // in m_handles. Destroying them calls release_block() which touches those
    // blocks' ref counts. All blocks must still be alive during unload or we
    // get UAF on the ref-count field of an already-freed block.
    for (auto it = m_handles.begin(); it != m_handles.end(); ++it)
    {
        ResourceControlBlock* block = it.value();
        void* p = block->payload.exchange(nullptr, std::memory_order_acq_rel);
        if (p && block->loader)
        {
            block->loader->unload(p);
        }
    }
    for (auto it = m_handles.begin(); it != m_handles.end(); ++it)
    {
        ResourceControlBlock* block = it.value();
        block->~ResourceControlBlock();
        m_alloc->deallocate(block);
    }
}

// ── Registration ───────────────────────────────────────────────────────────

void ResourceManager::register_loader(std::unique_ptr<ILoader> loader)
{
    CRD_ASSERT_MSG(loader != nullptr, "ResourceManager::register_loader: null loader");
    const crd::u32 fourcc = loader->type_fourcc();
    CRD_ASSERT_MSG(!m_loaders.contains(fourcc),
                   "ResourceManager::register_loader: duplicate FourCC");
    m_loaders.insert(fourcc, std::move(loader));
}

// ── Mounts ─────────────────────────────────────────────────────────────────

MountId ResourceManager::mount_manifest(crd::containers::StringView path)
{
    const crd::platform::fs::Path fs_path(path);

    crd::containers::Array<crd::u8> file_bytes(m_alloc);
    if (!crd::platform::fs::read_file_binary(fs_path, file_bytes))
    {
        CRD_LOG_ERROR(g_log_resources, "ResourceManager: failed to read manifest '{}'", path);
        return MountId{0U};
    }

    CrdrFile crdr_file(m_alloc);
    const CrdrError err = crdr_read(crd::containers::as_const_span(file_bytes), crdr_file, m_alloc);
    if (err != CrdrError::Ok)
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager: manifest '{}' parse error ({})",
                      path, static_cast<int>(err));
        return MountId{0U};
    }

    if (crdr_file.type_fourcc != kFourCC_PACK)
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager: '{}' is not a PACK container", path);
        return MountId{0U};
    }

    const CrdrChunk* mfst_chunk = crdr_find_chunk(crdr_file, kFourCC_MFST);
    if (mfst_chunk == nullptr)
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager: manifest '{}' missing MFST chunk", path);
        return MountId{0U};
    }

    crd::containers::ConstSpan<crd::u8> strp_data{};
    const CrdrChunk* strp_chunk = crdr_find_chunk(crdr_file, kFourCC_STRP);
    if (strp_chunk != nullptr)
    {
        strp_data = strp_chunk->payload;
    }

    crd::containers::Array<ManifestEntry> entries(m_alloc);
    if (!manifest_read_entries(mfst_chunk->payload, entries, m_alloc))
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager: manifest '{}' MFST chunk is malformed", path);
        return MountId{0U};
    }

    const crd::u32 mount_id = m_next_mount_id++;
    MountRecord record(m_alloc, mount_id, path);

    for (const ManifestEntry& e : entries)
    {
        if (m_live.contains(e.id))
        {
            const MountEntry* existing = m_live.find(e.id);
            CRD_ASSERT(existing != nullptr);
            CRD_LOG_WARN(g_log_resources,
                         "ResourceManager: ResourceId collision on mount '{}' "
                         "(existing mount_id={}); newest mount wins",
                         path, existing->mount_id);
            m_live.erase(e.id);
        }

        MountEntry live;
        live.id           = e.id;
        live.type_fourcc  = e.type_fourcc;
        live.flags        = e.flags;
        live.blob_offset  = e.blob_offset;
        live.blob_size    = e.blob_size;
        live.mount_id     = mount_id;

        const crd::containers::StringView name = strp_get(strp_data, e.name_strp_idx);
        live.name = crd::containers::String(name.data(), name.size(), m_alloc);

        m_live.insert(e.id, std::move(live));
        record.entries.push_back(e.id);
    }

    m_mounts.push_back(std::move(record));

    const crd::i64 initial_mtime = crd::platform::fs::last_modified_unix_seconds(fs_path);
    m_pack_watches.push_back(PackWatch(m_alloc, mount_id, path, initial_mtime));

    CRD_LOG_INFO(g_log_resources,
                 "ResourceManager: mounted '{}' ({} entries, mount_id={})",
                 path, entries.size(), mount_id);

    return MountId{mount_id};
}

void ResourceManager::unmount(MountId id)
{
    if (!id.is_valid())
    {
        return;
    }

    for (crd::usize i = 0U; i < m_mounts.size(); ++i)
    {
        if (m_mounts[i].id != id.value)
        {
            continue;
        }

        for (const ResourceId& rid : m_mounts[i].entries)
        {
            const MountEntry* e = m_live.find(rid);
            if (e != nullptr && e->mount_id == id.value)
            {
                m_live.erase(rid);
            }
        }

        m_mounts.swap_remove(i);

        for (crd::usize j = 0U; j < m_pack_watches.size(); ++j)
        {
            if (m_pack_watches[j].mount_id == id.value)
            {
                m_pack_watches.swap_remove(j);
                break;
            }
        }
        return;
    }
}

// ── Lookup ─────────────────────────────────────────────────────────────────

const MountEntry* ResourceManager::find_entry(ResourceId id) const noexcept
{
    return m_live.find(id);
}

// ── Diagnostics ────────────────────────────────────────────────────────────

crd::usize ResourceManager::loader_count()    const noexcept { return m_loaders.size(); }
crd::usize ResourceManager::mount_count()     const noexcept { return m_mounts.size(); }
crd::usize ResourceManager::entry_count()     const noexcept { return m_live.size(); }
crd::usize ResourceManager::handle_count()    const noexcept { return m_handles.size(); }
crd::usize ResourceManager::in_flight_count() const noexcept
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_in_flight.size();
}

// ── Memory budget and eviction (v1g) ───────────────────────────────────────

void ResourceManager::set_memory_budget(crd::u64 bytes)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_memory_budget = bytes;
    try_evict_to_budget();
}

crd::u64 ResourceManager::current_memory_use() const noexcept
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_memory_used;
}

void ResourceManager::pin(ResourceId id)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    crd::u32* count = m_pin_counts.find(id);
    if (count != nullptr)
    {
        ++(*count);
    }
    else
    {
        m_pin_counts.insert(id, 1U);
    }
    // If the block is already loaded, set pinned immediately.
    ResourceControlBlock** pp = m_handles.find(id);
    if (pp != nullptr)
    {
        (*pp)->pinned = true;
    }
}

void ResourceManager::unpin(ResourceId id)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    crd::u32* count = m_pin_counts.find(id);
    if (count == nullptr || *count == 0U)
    {
        return;
    }
    --(*count);
    if (*count == 0U)
    {
        m_pin_counts.erase(id);
        ResourceControlBlock** pp = m_handles.find(id);
        if (pp != nullptr)
        {
            (*pp)->pinned = false;
        }
    }
}

// ── 2Q helpers (all called under m_mutex) ─────────────────────────────────

void ResourceManager::insert_into_2q(ResourceId id, ResourceControlBlock* block)
{
    if (m_a1out_set.contains(id))
    {
        // A1out ghost hit — promote to Am (MRU position = back).
        m_a1out_set.erase(id);
        for (crd::usize i = 0U; i < m_a1out.size(); ++i)
        {
            if (m_a1out[i] == id)
            {
                m_a1out.erase(i); // order-preserving
                break;
            }
        }
        block->evict_queue = EvictQueue::Am;
        m_am.push_back(id);
    }
    else
    {
        // New resource — add to A1in (FIFO back).
        block->evict_queue = EvictQueue::A1in;
        m_a1in.push_back(id);
    }
}

void ResourceManager::touch_in_am(ResourceId id)
{
    // Move id from wherever it is in m_am to the MRU end (back).
    for (crd::usize i = 0U; i < m_am.size(); ++i)
    {
        if (m_am[i] == id)
        {
            m_am.erase(i); // order-preserving
            m_am.push_back(id);
            return;
        }
    }
}

CRD_NOINLINE void ResourceManager::evict_block_locked(ResourceId id, ResourceControlBlock* block)
{
    CRD_ASSERT_MSG(block->use_count() == 0U, "evict_block_locked: block has active handles");
    CRD_ASSERT_MSG(!block->pinned, "evict_block_locked: block is pinned");
    CRD_ASSERT_MSG(block->state.load(std::memory_order_acquire) == LoadState::Ready,
                   "evict_block_locked: block state is not Ready");

    void* p = block->payload.load(std::memory_order_acquire);
    if (p != nullptr && block->loader != nullptr)
    {
        block->loader->unload(p);
        block->payload.store(nullptr, std::memory_order_release);
    }

    m_memory_used -= block->payload_size;
    block->payload_size = 0U;
    block->state.store(LoadState::Unloaded, std::memory_order_release);
    block->evict_queue = EvictQueue::None;

    // Add id to A1out ghost list (FIFO bounded at kMaxA1out).
    if (!m_a1out_set.contains(id))
    {
        if (m_a1out.size() >= kMaxA1out)
        {
            // Remove the oldest ghost (front of FIFO). Use erase(0) to preserve
            // FIFO order in the remaining entries (Bug-3 fix: not swap_remove).
            const ResourceId oldest = m_a1out[0];
            m_a1out.erase(0);
            m_a1out_set.erase(oldest);
        }
        m_a1out.push_back(id);
        m_a1out_set.insert(id, true);
    }
}

CRD_NOINLINE void ResourceManager::try_evict_to_budget()
{
    // Called under m_mutex. Evict zero-handle, un-pinned, Ready blocks until
    // m_memory_used <= m_memory_budget, preferring A1in (FIFO) over Am (LRU).
    while (m_memory_used > m_memory_budget)
    {
        ResourceId victim_id{};
        bool       found = false;

        // Prefer A1in front (oldest probationary entry).
        for (crd::usize i = 0U; i < m_a1in.size(); ++i)
        {
            const ResourceId rid = m_a1in[i];
            ResourceControlBlock** pp = m_handles.find(rid);
            if (pp == nullptr) { continue; }
            ResourceControlBlock* b = *pp;
            if (b->state.load(std::memory_order_acquire) == LoadState::Ready
                && b->use_count() == 0U
                && !b->pinned)
            {
                victim_id = rid;
                found     = true;
                m_a1in.erase(i); // remove from queue before eviction
                break;
            }
        }

        if (!found)
        {
            // Fall back to Am front (least recently used main entry).
            for (crd::usize i = 0U; i < m_am.size(); ++i)
            {
                const ResourceId rid = m_am[i];
                ResourceControlBlock** pp = m_handles.find(rid);
                if (pp == nullptr) { continue; }
                ResourceControlBlock* b = *pp;
                if (b->state.load(std::memory_order_acquire) == LoadState::Ready
                    && b->use_count() == 0U
                    && !b->pinned)
                {
                    victim_id = rid;
                    found     = true;
                    m_am.erase(i);
                    break;
                }
            }
        }

        if (!found)
        {
            break; // nothing evictable; stop (budget stays violated for pinned/live resources)
        }

        ResourceControlBlock** pp = m_handles.find(victim_id);
        if (pp != nullptr)
        {
            evict_block_locked(victim_id, *pp);
        }
    }
}

// ── Internal helpers ───────────────────────────────────────────────────────

const ResourceManager::MountRecord* ResourceManager::find_mount(crd::u32 mount_id) const noexcept
{
    for (const MountRecord& r : m_mounts)
    {
        if (r.id == mount_id)
        {
            return &r;
        }
    }
    return nullptr;
}

// ── Synchronous load ───────────────────────────────────────────────────────

ResourceControlBlock* ResourceManager::load_sync_impl(ResourceId id)
{
    // ── Phase 1: Fast-path checks and reservation (under lock) ────────────

    ILoader*                loader      = nullptr;
    crd::u64                blob_offset = 0;
    crd::u64                blob_size   = 0;
    crd::u32                type_fourcc = 0;
    crd::containers::String pack_path(m_alloc);
    ResourceControlBlock*   block       = nullptr;
    bool                    coalescing  = false;
    bool                    re_issue    = false;

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // 1. Cache hit — check state to determine how to respond.
        if (ResourceControlBlock** p = m_handles.find(id))
        {
            ResourceControlBlock* b = *p;
            const LoadState s = b->state.load(std::memory_order_acquire);

            if (s == LoadState::Ready || s == LoadState::Placeholder || s == LoadState::Failed)
            {
                // Terminal state: return immediately with a new ref.
                if (b->evict_queue == EvictQueue::Am)
                {
                    touch_in_am(id); // LRU bump for Am cache hits
                }
                b->add_ref();
                return b;
            }

            if (s == LoadState::Unloaded)
            {
                // Block was evicted — re-issue the load using the existing block.
                re_issue = true;
                // Fall through to resolve loader/manifest below.
            }
            // s == Loading or Queued: fall through to in-flight coalescing.
        }

        // 2. Cycle detection — must happen before m_in_flight check.
        if (visiting_contains(id))
        {
            const auto id_str = id.to_string(m_alloc);
            CRD_LOG_ERROR(g_log_resources,
                          "ResourceManager::load_sync: dependency cycle detected for id {}",
                          id_str.c_str());
            return make_failed_block(m_alloc, id);
        }

        // 3. In-flight coalescing — share the existing in-flight block.
        if (ResourceControlBlock** p = m_in_flight.find(id))
        {
            (*p)->add_ref();
            block      = *p;
            coalescing = true;
        }

        if (!coalescing)
        {
            // 4. Resolve manifest entry.
            const MountEntry* entry = m_live.find(id);
            if (entry == nullptr)
            {
                const auto id_str = id.to_string(m_alloc);
                CRD_LOG_ERROR(g_log_resources,
                              "ResourceManager::load_sync: id {} not found in any mounted pack",
                              id_str.c_str());
                return make_failed_block(m_alloc, id);
            }

            // 5. Resolve loader.
            std::unique_ptr<ILoader>* lp = m_loaders.find(entry->type_fourcc);
            if (lp == nullptr)
            {
                char fc[5];
                fourcc_to_str(entry->type_fourcc, fc);
                CRD_LOG_ERROR(g_log_resources,
                              "ResourceManager::load_sync: no loader registered for FourCC '{}'", fc);
                return make_failed_block(m_alloc, id);
            }
            loader = lp->get();

            // 6. Resolve mount.
            const MountRecord* mount = find_mount(entry->mount_id);
            if (mount == nullptr)
            {
                CRD_LOG_ERROR(g_log_resources,
                              "ResourceManager::load_sync: internal error — mount record missing");
                return make_failed_block(m_alloc, id);
            }

            pack_path   = mount->pack_path;
            blob_offset = entry->blob_offset;
            blob_size   = entry->blob_size;
            type_fourcc = entry->type_fourcc;

            if (re_issue)
            {
                // Reuse the evicted block. Give the caller their ref.
                block         = *m_handles.find(id);
                block->loader = loader;
                block->add_ref(); // caller's ref
                block->state.store(LoadState::Loading, std::memory_order_release);
            }
            else
            {
                // First-ever load: allocate a new block.
                void* raw = m_alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
                block = new (raw) ResourceControlBlock();
                block->id          = id;
                block->type_fourcc = type_fourcc;
                block->alloc       = m_alloc;
                block->loader      = loader;
                block->state.store(LoadState::Loading, std::memory_order_release);
            }

            m_in_flight.insert(id, block);
        }
    } // lock released

    // ── Phase 2: Coalescing wait — spin until the in-flight load finishes ─

    if (coalescing)
    {
        LoadState s = block->state.load(std::memory_order_acquire);
        while (s == LoadState::Queued || s == LoadState::Loading)
        {
            std::this_thread::yield();
            s = block->state.load(std::memory_order_acquire);
        }
        return block;
    }

    // ── Phase 3: I/O (outside lock; enables recursive load_sync for deps) ─

    crd::containers::Array<crd::u8> artifact_bytes(m_alloc);
    const crd::platform::fs::Path   fs_path(pack_path);
    const bool read_ok = crd::platform::fs::read_file_range(
        fs_path, blob_offset, blob_size, artifact_bytes);

    void*     payload     = nullptr;
    LoadState final_state = LoadState::Failed;

    if (read_ok)
    {
        visiting_push(id);
        const LoadContext ctx{id, crd::containers::as_const_span(artifact_bytes), this, m_alloc};
        payload = loader->load(ctx);
        visiting_pop();

        if (payload != nullptr)
        {
            final_state = LoadState::Ready;
        }
        else
        {
            payload     = loader->load_placeholder(ctx);
            final_state = payload ? LoadState::Placeholder : LoadState::Failed;
        }
    }
    else
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::load_sync: failed to read artifact bytes");
    }

    // ── Phase 4: Finalize under lock ──────────────────────────────────────

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        m_in_flight.erase(id);

        if (payload != nullptr)
        {
            block->payload.store(payload, std::memory_order_release);
            block->payload_size = blob_size;

            // Honour pin-before-load: if the resource was pinned before its block
            // was created, apply the flag now.
            if (m_pin_counts.find(id) != nullptr)
            {
                block->pinned = true;
            }

            if (re_issue)
            {
                // Re-issued evicted block: bump generation and re-queue.
                block->generation.fetch_add(1U, std::memory_order_acq_rel);
                insert_into_2q(id, block);
            }
            else
            {
                block->permanent = true;
                m_handles.insert(id, block);
                insert_into_2q(id, block);
            }

            block->state.store(final_state, std::memory_order_release);
            m_memory_used += blob_size;
            try_evict_to_budget();

            CRD_LOG_DEBUG(g_log_resources, "ResourceManager: loaded resource (Ready)");
        }
        else
        {
            block->state.store(LoadState::Failed, std::memory_order_release);
            CRD_LOG_ERROR(g_log_resources, "ResourceManager: hard failure loading resource");
        }
    }

    return block;
}

// ── Asynchronous load ──────────────────────────────────────────────────────

ResourceControlBlock* ResourceManager::load_async_impl(ResourceId id)
{
    ILoader*                loader      = nullptr;
    crd::u64                blob_offset = 0;
    crd::u64                blob_size   = 0;
    crd::u32                type_fourcc = 0;
    crd::containers::String pack_path(m_alloc);
    ResourceControlBlock*   block       = nullptr;
    bool                    re_issue    = false;

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // 1. Cache hit — check state.
        if (ResourceControlBlock** p = m_handles.find(id))
        {
            ResourceControlBlock* b = *p;
            const LoadState s = b->state.load(std::memory_order_acquire);

            if (s == LoadState::Ready || s == LoadState::Placeholder || s == LoadState::Failed)
            {
                if (b->evict_queue == EvictQueue::Am)
                {
                    touch_in_am(id);
                }
                b->add_ref();
                return b;
            }

            if (s == LoadState::Unloaded)
            {
                re_issue = true;
            }
            // Loading/Queued: fall through to in-flight coalescing.
        }

        // 2. In-flight coalescing — share the existing in-flight block.
        if (ResourceControlBlock** p = m_in_flight.find(id))
        {
            (*p)->add_ref();
            return *p;
        }

        // 3. Resolve manifest, loader, and mount.
        const MountEntry* entry = m_live.find(id);
        if (entry == nullptr)
        {
            const auto id_str = id.to_string(m_alloc);
            CRD_LOG_ERROR(g_log_resources,
                          "ResourceManager::load_async: id {} not found in any mounted pack",
                          id_str.c_str());
            return make_failed_block(m_alloc, id);
        }

        std::unique_ptr<ILoader>* lp = m_loaders.find(entry->type_fourcc);
        if (lp == nullptr)
        {
            char fc[5];
            fourcc_to_str(entry->type_fourcc, fc);
            CRD_LOG_ERROR(g_log_resources,
                          "ResourceManager::load_async: no loader registered for FourCC '{}'", fc);
            return make_failed_block(m_alloc, id);
        }
        loader = lp->get();

        const MountRecord* mount = find_mount(entry->mount_id);
        if (mount == nullptr)
        {
            CRD_LOG_ERROR(g_log_resources,
                          "ResourceManager::load_async: internal error — mount record missing");
            return make_failed_block(m_alloc, id);
        }

        pack_path   = mount->pack_path;
        blob_offset = entry->blob_offset;
        blob_size   = entry->blob_size;
        type_fourcc = entry->type_fourcc;

        if (re_issue)
        {
            // Reuse the evicted block.
            block         = *m_handles.find(id);
            block->loader = loader;
            block->add_ref(); // caller's ref
            block->add_ref(); // manager's in-flight ref
            block->state.store(LoadState::Queued, std::memory_order_release);
        }
        else
        {
            // Allocate block (state=Queued). Ref=1 (initial, becomes caller's);
            // manager adds one more for the in-flight period.
            void* raw = m_alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
            block = new (raw) ResourceControlBlock();
            block->id          = id;
            block->type_fourcc = type_fourcc;
            block->alloc       = m_alloc;
            block->loader      = loader;
            block->state.store(LoadState::Queued, std::memory_order_release);
            block->add_ref(); // manager's in-flight ref
        }

        m_in_flight.insert(id, block);
    } // lock released

    // Heap-allocate the context so only the 8-byte pointer lives in the SBO closure.
    void* ctx_raw = m_alloc->allocate(sizeof(AsyncLoadCtx), alignof(AsyncLoadCtx));
    auto* ctx = new (ctx_raw) AsyncLoadCtx{
        this,
        block,
        loader,
        crd::containers::String(pack_path.data(), pack_path.size(), m_alloc),
        blob_offset,
        blob_size,
        id,
        type_fourcc,
        re_issue
    };

    // Submit job and store the returned Counter so wait_ready() can claim it.
    const LoadJobFn fn{ctx};
    crd::jobs::Counter* c = crd::jobs::run(crd::jobs::make_job(fn));
    block->load_counter.store(c, std::memory_order_release);

    // Counter leak fix: if the job completed before we stored the counter,
    // claim and wait immediately so the counter slot is not orphaned.
    const LoadState s = block->state.load(std::memory_order_acquire);
    if (s != LoadState::Queued && s != LoadState::Loading)
    {
        void* raw_c = block->load_counter.exchange(nullptr, std::memory_order_acquire);
        if (raw_c != nullptr)
        {
            crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw_c));
        }
    }

    return block;
}

// ── Async job body ─────────────────────────────────────────────────────────

CRD_NOINLINE void ResourceManager::run_load_job(void* raw_ctx) noexcept
{
    auto* ctx  = static_cast<AsyncLoadCtx*>(raw_ctx);
    ResourceControlBlock* block = ctx->block;
    ResourceManager*      mgr   = ctx->manager;

    // Signal that the job is actively running.
    block->state.store(LoadState::Loading, std::memory_order_release);

    // I/O — outside any lock, so concurrent load_sync calls can progress.
    crd::containers::Array<crd::u8> artifact_bytes(mgr->m_alloc);
    const crd::platform::fs::Path   fs_path(ctx->pack_path);
    const bool read_ok = crd::platform::fs::read_file_range(
        fs_path, ctx->blob_offset, ctx->blob_size, artifact_bytes);

    void*     payload     = nullptr;
    LoadState final_state = LoadState::Failed;

    if (read_ok)
    {
        visiting_push(ctx->id);
        const LoadContext load_ctx{
            ctx->id,
            crd::containers::as_const_span(artifact_bytes),
            mgr,
            mgr->m_alloc
        };
        payload = ctx->loader->load(load_ctx);
        visiting_pop();

        if (payload != nullptr)
        {
            final_state = LoadState::Ready;
        }
        else
        {
            payload     = ctx->loader->load_placeholder(load_ctx);
            final_state = payload ? LoadState::Placeholder : LoadState::Failed;
        }
    }
    else
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::run_load_job: failed to read artifact bytes");
    }

    // Finalize under lock: update state, move from in_flight to handles.
    {
        std::lock_guard<std::mutex> lk(mgr->m_mutex);

        mgr->m_in_flight.erase(ctx->id);

        if (payload != nullptr)
        {
            block->payload.store(payload, std::memory_order_release);
            block->payload_size = ctx->blob_size;

            if (mgr->m_pin_counts.find(ctx->id) != nullptr)
            {
                block->pinned = true;
            }

            if (ctx->re_issue)
            {
                block->generation.fetch_add(1U, std::memory_order_acq_rel);
                mgr->insert_into_2q(ctx->id, block);
            }
            else
            {
                block->permanent = true;
                mgr->m_handles.insert(ctx->id, block);
                mgr->insert_into_2q(ctx->id, block);
            }

            mgr->m_memory_used += ctx->blob_size;
            mgr->try_evict_to_budget();
        }

        // Release store — visible to wait_ready()'s acquire load after jobs::wait() returns.
        block->state.store(final_state, std::memory_order_release);
    }

    // Release manager's in-flight ref. If this was the last ref AND the block is
    // non-permanent (failed), free it here.
    const crd::u32 remaining = block->release();
    if (remaining == 0U && !block->permanent)
    {
        void* p = block->payload.load(std::memory_order_acquire);
        if (p && block->loader)
        {
            block->loader->unload(p);
            block->payload.store(nullptr, std::memory_order_release);
        }
        crd::memory::IAllocator* blk_alloc = block->alloc;
        block->~ResourceControlBlock();
        blk_alloc->deallocate(block);
    }

    // Free the AsyncLoadCtx.
    crd::memory::IAllocator* ctx_alloc = mgr->m_alloc;
    ctx->~AsyncLoadCtx();
    ctx_alloc->deallocate(ctx);
}

// ── Streamed load (v1g) ────────────────────────────────────────────────────

ResourceControlBlock* ResourceManager::load_streamed_impl(ResourceId id)
{
    ILoader*                loader      = nullptr;
    crd::u64                blob_offset = 0;
    crd::u64                blob_size   = 0;
    crd::u32                type_fourcc = 0;
    crd::containers::String pack_path(m_alloc);
    ResourceControlBlock*   block       = nullptr;
    bool                    re_issue    = false;

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // 1. Cache hit.
        if (ResourceControlBlock** p = m_handles.find(id))
        {
            ResourceControlBlock* b = *p;
            const LoadState s = b->state.load(std::memory_order_acquire);

            if (s == LoadState::Ready || s == LoadState::Placeholder || s == LoadState::Failed)
            {
                if (b->evict_queue == EvictQueue::Am)
                {
                    touch_in_am(id);
                }
                b->add_ref();
                return b;
            }

            if (s == LoadState::Unloaded)
            {
                re_issue = true;
            }
        }

        // 2. In-flight coalescing.
        if (ResourceControlBlock** p = m_in_flight.find(id))
        {
            (*p)->add_ref();
            return *p;
        }

        // 3. Resolve manifest, loader, and mount.
        const MountEntry* entry = m_live.find(id);
        if (entry == nullptr)
        {
            return make_failed_block(m_alloc, id);
        }

        std::unique_ptr<ILoader>* lp = m_loaders.find(entry->type_fourcc);
        if (lp == nullptr)
        {
            return make_failed_block(m_alloc, id);
        }
        loader = lp->get();

        const MountRecord* mount = find_mount(entry->mount_id);
        if (mount == nullptr)
        {
            return make_failed_block(m_alloc, id);
        }

        pack_path   = mount->pack_path;
        blob_offset = entry->blob_offset;
        blob_size   = entry->blob_size;
        type_fourcc = entry->type_fourcc;

        if (re_issue)
        {
            block         = *m_handles.find(id);
            block->loader = loader;
            block->add_ref(); // caller's ref
            block->add_ref(); // manager's in-flight ref
            block->state.store(LoadState::Queued, std::memory_order_release);
        }
        else
        {
            void* raw = m_alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
            block = new (raw) ResourceControlBlock();
            block->id          = id;
            block->type_fourcc = type_fourcc;
            block->alloc       = m_alloc;
            block->loader      = loader;
            block->state.store(LoadState::Queued, std::memory_order_release);
            block->add_ref(); // manager's in-flight ref
        }

        m_in_flight.insert(id, block);
    }

    // Heap-allocate the context.
    void* ctx_raw = m_alloc->allocate(sizeof(StreamLoadCtx), alignof(StreamLoadCtx));
    auto* ctx = new (ctx_raw) StreamLoadCtx{
        this,
        block,
        loader,
        crd::containers::String(pack_path.data(), pack_path.size(), m_alloc),
        blob_offset,
        blob_size,
        id,
        type_fourcc,
        re_issue
    };

    // Submit job and store Counter.
    const StreamLoadJobFn fn{ctx};
    crd::jobs::Counter* c = crd::jobs::run(crd::jobs::make_job(fn));
    block->load_counter.store(c, std::memory_order_release);

    // Counter leak fix.
    const LoadState s = block->state.load(std::memory_order_acquire);
    if (s != LoadState::Queued && s != LoadState::Loading)
    {
        void* raw_c = block->load_counter.exchange(nullptr, std::memory_order_acquire);
        if (raw_c != nullptr)
        {
            crd::jobs::wait(static_cast<crd::jobs::Counter*>(raw_c));
        }
    }

    return block;
}

CRD_NOINLINE void ResourceManager::run_stream_load_job(void* raw_ctx) noexcept
{
    auto* ctx  = static_cast<StreamLoadCtx*>(raw_ctx);
    ResourceControlBlock* block = ctx->block;
    ResourceManager*      mgr   = ctx->manager;

    block->state.store(LoadState::Loading, std::memory_order_release);

    // Open the pack file via AsyncFile and submit a read for the artifact range.
    crd::platform::AsyncFile af = crd::platform::AsyncFile::open(
        crd::containers::StringView(ctx->pack_path.data(), ctx->pack_path.size()));

    void*     payload     = nullptr;
    LoadState final_state = LoadState::Failed;

    if (af.is_open() && ctx->blob_offset + ctx->blob_size <= af.size())
    {
        crd::containers::Array<crd::u8> buf(mgr->m_alloc);
        buf.resize(ctx->blob_size);

        crd::jobs::Counter* c = af.read_async(
            ctx->blob_offset,
            crd::containers::Span<crd::u8>(buf.data(), buf.size()));

        if (c != nullptr)
        {
            crd::jobs::wait(c); // suspend fiber or spin until read completes

            LoadContext load_ctx{};
            load_ctx.id            = ctx->id;
            load_ctx.bytes         = crd::containers::as_const_span(buf);
            load_ctx.manager       = mgr;
            load_ctx.allocator     = mgr->m_alloc;
            load_ctx.stream_file   = &af;
            load_ctx.stream_offset = ctx->blob_offset;
            load_ctx.stream_size   = ctx->blob_size;

            visiting_push(ctx->id);
            payload = ctx->loader->load(load_ctx);
            visiting_pop();

            if (payload != nullptr)
            {
                final_state = LoadState::Ready;
            }
            else
            {
                payload     = ctx->loader->load_placeholder(load_ctx);
                final_state = payload ? LoadState::Placeholder : LoadState::Failed;
            }
        }
        else
        {
            CRD_LOG_ERROR(g_log_resources,
                          "ResourceManager::run_stream_load_job: read_async returned null counter");
        }
    }
    else
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::run_stream_load_job: failed to open pack or invalid range");
    }

    // Finalize under lock.
    {
        std::lock_guard<std::mutex> lk(mgr->m_mutex);

        mgr->m_in_flight.erase(ctx->id);

        if (payload != nullptr)
        {
            block->payload.store(payload, std::memory_order_release);
            block->payload_size = ctx->blob_size;

            if (mgr->m_pin_counts.find(ctx->id) != nullptr)
            {
                block->pinned = true;
            }

            if (ctx->re_issue)
            {
                block->generation.fetch_add(1U, std::memory_order_acq_rel);
                mgr->insert_into_2q(ctx->id, block);
            }
            else
            {
                block->permanent = true;
                mgr->m_handles.insert(ctx->id, block);
                mgr->insert_into_2q(ctx->id, block);
            }

            mgr->m_memory_used += ctx->blob_size;
            mgr->try_evict_to_budget();
        }

        block->state.store(final_state, std::memory_order_release);
    }

    // Release manager's in-flight ref.
    const crd::u32 remaining = block->release();
    if (remaining == 0U && !block->permanent)
    {
        void* p = block->payload.load(std::memory_order_acquire);
        if (p && block->loader)
        {
            block->loader->unload(p);
            block->payload.store(nullptr, std::memory_order_release);
        }
        crd::memory::IAllocator* blk_alloc = block->alloc;
        block->~ResourceControlBlock();
        blk_alloc->deallocate(block);
    }

    crd::memory::IAllocator* ctx_alloc = mgr->m_alloc;
    ctx->~StreamLoadCtx();
    ctx_alloc->deallocate(ctx);
}

// ── Hot-reload ─────────────────────────────────────────────────────────────

crd::u32 ResourceManager::subscribe_reload(ResourceId id, ReloadCallback cb, void* user)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const crd::u32 token = m_next_reload_token++;

    crd::containers::Array<ReloadSub>* subs = m_reload_subs.find(id);
    if (subs == nullptr)
    {
        crd::containers::Array<ReloadSub> arr(m_alloc);
        arr.push_back(ReloadSub{cb, user, token});
        m_reload_subs.insert(id, std::move(arr));
    }
    else
    {
        subs->push_back(ReloadSub{cb, user, token});
    }
    return token;
}

void ResourceManager::unsubscribe_reload(ResourceId id, crd::u32 token)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    crd::containers::Array<ReloadSub>* subs = m_reload_subs.find(id);
    if (subs == nullptr)
    {
        return;
    }
    for (crd::usize i = 0U; i < subs->size(); ++i)
    {
        if ((*subs)[i].token == token)
        {
            subs->swap_remove(i);
            return;
        }
    }
}

crd::usize ResourceManager::poll_hot_reload(crd::u32 debounce_ms)
{
    // Drain deferred frees from the previous reload (one-frame grace period).
    for (DeferredFree& df : m_deferred_frees)
    {
        df.loader->unload(df.payload);
    }
    m_deferred_frees.clear();

    const auto now = std::chrono::steady_clock::now();
    crd::usize reloaded = 0U;

    for (PackWatch& watch : m_pack_watches)
    {
        const crd::platform::fs::Path fs_path(watch.pack_path);
        const crd::i64 current_mtime = crd::platform::fs::last_modified_unix_seconds(fs_path);

        if (current_mtime > 0 && current_mtime != watch.last_processed_mtime)
        {
            if (!watch.has_pending || current_mtime != watch.pending_mtime)
            {
                watch.pending_mtime = current_mtime;
                watch.has_pending   = true;
                watch.pending_since = now;
            }
        }

        if (!watch.has_pending)
        {
            continue;
        }

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - watch.pending_since).count();
        // Cast to the underlying integer type rather than `decltype(elapsed_ms)` —
        // the latter carries the `const` qualifier from the variable declaration,
        // which trips GCC's `-Wignored-qualifiers` (and `-Werror` on CI).
        if (debounce_ms > 0U && elapsed_ms < static_cast<std::chrono::milliseconds::rep>(debounce_ms))
        {
            continue;
        }

        reloaded += do_reload_mount(watch);
        watch.last_processed_mtime = watch.pending_mtime;
        watch.has_pending          = false;
    }

    return reloaded;
}

crd::usize ResourceManager::reload_mount_now(MountId id)
{
    if (!id.is_valid())
    {
        return 0U;
    }

    // Drain deferred frees so the grace period contract is consistent with poll_hot_reload.
    for (DeferredFree& df : m_deferred_frees)
    {
        df.loader->unload(df.payload);
    }
    m_deferred_frees.clear();

    for (PackWatch& watch : m_pack_watches)
    {
        if (watch.mount_id != id.value)
        {
            continue;
        }

        const crd::usize n = do_reload_mount(watch);
        // Reset debounce state — the forced reload is now the baseline.
        const crd::platform::fs::Path fs_path(watch.pack_path);
        watch.last_processed_mtime = crd::platform::fs::last_modified_unix_seconds(fs_path);
        watch.has_pending          = false;
        return n;
    }
    return 0U;
}

crd::usize ResourceManager::do_reload_mount(PackWatch& watch)
{
    // Step 1: re-read and parse the pack file.
    const crd::containers::StringView pv{watch.pack_path.data(), watch.pack_path.size()};
    crd::containers::Array<crd::u8> file_bytes(m_alloc);
    const crd::platform::fs::Path   fs_path(watch.pack_path);
    if (!crd::platform::fs::read_file_binary(fs_path, file_bytes))
    {
        CRD_LOG_WARN(g_log_resources,
                     "ResourceManager: hot-reload: failed to read '{}'", pv);
        return 0U;
    }

    CrdrFile crdr_file(m_alloc);
    const CrdrError err = crdr_read(crd::containers::as_const_span(file_bytes), crdr_file, m_alloc);
    if (err != CrdrError::Ok || crdr_file.type_fourcc != kFourCC_PACK)
    {
        CRD_LOG_WARN(g_log_resources,
                     "ResourceManager: hot-reload: '{}' parse error ({})",
                     pv, static_cast<int>(err));
        return 0U;
    }

    const CrdrChunk* mfst_chunk = crdr_find_chunk(crdr_file, kFourCC_MFST);
    if (mfst_chunk == nullptr)
    {
        CRD_LOG_WARN(g_log_resources,
                     "ResourceManager: hot-reload: '{}' missing MFST chunk", pv);
        return 0U;
    }

    crd::containers::Array<ManifestEntry> new_entries(m_alloc);
    if (!manifest_read_entries(mfst_chunk->payload, new_entries, m_alloc))
    {
        CRD_LOG_WARN(g_log_resources,
                     "ResourceManager: hot-reload: '{}' MFST chunk malformed", pv);
        return 0U;
    }

    // Step 2: under lock — update m_live blob offsets and collect reload tasks.
    struct ReloadTask
    {
        ResourceControlBlock* block;
        ILoader*              loader;
        crd::u64              blob_offset;
        crd::u64              blob_size;
        ResourceId            id;
    };
    crd::containers::Array<ReloadTask> tasks(m_alloc);

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        for (const ManifestEntry& e : new_entries)
        {
            MountEntry* existing = m_live.find(e.id);
            if (existing != nullptr && existing->mount_id == watch.mount_id)
            {
                existing->blob_offset = e.blob_offset;
                existing->blob_size   = e.blob_size;
            }
        }

        for (const ManifestEntry& e : new_entries)
        {
            ResourceControlBlock** pp = m_handles.find(e.id);
            if (pp == nullptr)
            {
                continue;
            }
            ResourceControlBlock* block = *pp;
            const LoadState s = block->state.load(std::memory_order_acquire);
            if (s != LoadState::Ready && s != LoadState::Placeholder)
            {
                continue;
            }
            std::unique_ptr<ILoader>* lp = m_loaders.find(e.type_fourcc);
            if (lp == nullptr)
            {
                continue;
            }
            block->add_ref();
            tasks.push_back(ReloadTask{block, lp->get(), e.blob_offset, e.blob_size, e.id});
        }
    }

    // Step 3: reload each task — I/O and loader dispatch outside the lock.
    crd::usize reloaded = 0U;
    for (ReloadTask& task : tasks)
    {
        if (task.blob_offset + task.blob_size > static_cast<crd::u64>(file_bytes.size()))
        {
            CRD_LOG_WARN(g_log_resources,
                         "ResourceManager: hot-reload: artifact bounds out of range; skipping");
            [[maybe_unused]] const crd::u32 r = task.block->release();
            continue;
        }

        const crd::containers::ConstSpan<crd::u8> artifact_span(
            file_bytes.data() + task.blob_offset, task.blob_size);

        const LoadContext ctx{task.id, artifact_span, this, m_alloc};
        void* new_payload = task.loader->load(ctx);

        if (new_payload == nullptr)
        {
            CRD_LOG_WARN(g_log_resources,
                         "ResourceManager: hot-reload: loader returned null; preserving last-good payload");
            [[maybe_unused]] const crd::u32 r = task.block->release();
            continue;
        }

        // Atomic swap: exchange old payload for new before re-publishing the state.
        void* old_payload = task.block->payload.exchange(new_payload, std::memory_order_acq_rel);
        const crd::u32 new_gen =
            task.block->generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
        task.block->state.store(LoadState::Ready, std::memory_order_release);

        if (old_payload != nullptr)
        {
            m_deferred_frees.push_back(DeferredFree{old_payload, task.loader});
        }

        ++reloaded;

        // Copy subscribers under lock, then fire outside.
        crd::containers::Array<ReloadSub> subs_copy(m_alloc);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            crd::containers::Array<ReloadSub>* subs = m_reload_subs.find(task.id);
            if (subs != nullptr)
            {
                for (const ReloadSub& sub : *subs)
                {
                    subs_copy.push_back(sub);
                }
            }
        }
        for (const ReloadSub& sub : subs_copy)
        {
            sub.cb(task.id, new_gen, sub.user);
        }

        [[maybe_unused]] const crd::u32 r = task.block->release();
    }

    return reloaded;
}

} // namespace crd::resources
