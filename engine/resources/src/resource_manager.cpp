#include <crd/resources/resource_manager.hpp>

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/construct.hpp>
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
// Captured as a void* in the SBO job closure (8 bytes ≤ 41).
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
};

// SBO-compatible job callable — holds only an 8-byte pointer.
struct LoadJobFn
{
    void* ctx_ptr;
    void operator()() noexcept { ResourceManager::run_load_job(ctx_ptr); }
};

static_assert(sizeof(LoadJobFn)  <= 41U, "LoadJobFn must fit in 41-byte SBO");
static_assert(alignof(LoadJobFn) <= 8U,  "LoadJobFn alignment must be ≤ 8");
static_assert(std::is_trivially_copyable_v<LoadJobFn>);
static_assert(std::is_trivially_destructible_v<LoadJobFn>);

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
{
}

ResourceManager::~ResourceManager()
{
    // Callers must drain all async loads before destruction (e.g. call
    // jobs::shutdown() or wait on every async counter).
    CRD_ASSERT_MSG(m_in_flight.empty(),
                   "ResourceManager destroyed while async loads are in flight; "
                   "call jobs::shutdown() or wait_ready() on all handles first");

    for (DeferredFree& df : m_deferred_frees)
    {
        df.loader->unload(df.payload);
    }

    for (auto it = m_handles.begin(); it != m_handles.end(); ++it)
    {
        ResourceControlBlock* block = it.value();
        void* p = block->payload.load(std::memory_order_acquire);
        if (p && block->loader)
        {
            block->loader->unload(p);
        }
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

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // 1. Cache hit — return the existing permanent block.
        if (ResourceControlBlock** p = m_handles.find(id))
        {
            (*p)->add_ref();
            return *p;
        }

        // 2. Cycle detection — must happen before m_in_flight check so that
        //    recursive calls from inside loader->load() hit this path.
        if (visiting_contains(id))
        {
            const auto id_str = id.to_string(m_alloc);
            CRD_LOG_ERROR(g_log_resources,
                          "ResourceManager::load_sync: dependency cycle detected for id {}",
                          id_str.c_str());
            return make_failed_block(m_alloc, id);
        }

        // 3. In-flight coalescing — another sync or async load is already running.
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

            // Capture everything needed outside the lock.
            pack_path   = mount->pack_path;
            blob_offset = entry->blob_offset;
            blob_size   = entry->blob_size;
            type_fourcc = entry->type_fourcc;

            // Allocate control block and mark it Loading.
            void* raw = m_alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
            block = new (raw) ResourceControlBlock();
            block->id          = id;
            block->type_fourcc = type_fourcc;
            block->alloc       = m_alloc;
            block->loader      = loader;
            block->state.store(LoadState::Loading, std::memory_order_release);

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
            block->permanent = true;  // must be set before state store (memory ordering)
            block->state.store(final_state, std::memory_order_release);
            m_handles.insert(id, block);
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

    {
        std::lock_guard<std::mutex> lk(m_mutex);

        // 1. Cache hit — return terminal block with an extra ref for the caller.
        if (ResourceControlBlock** p = m_handles.find(id))
        {
            (*p)->add_ref();
            return *p;
        }

        // 2. In-flight coalescing — share the existing in-flight block.
        if (ResourceControlBlock** p = m_in_flight.find(id))
        {
            (*p)->add_ref();
            return *p;  // caller uses wait_ready()
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

        // Allocate block (state=Queued). Hold one extra manager ref for the in-flight
        // period; released in run_load_job after the job finishes.
        void* raw = m_alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
        block = new (raw) ResourceControlBlock();
        block->id          = id;
        block->type_fourcc = type_fourcc;
        block->alloc       = m_alloc;
        block->loader      = loader;
        block->state.store(LoadState::Queued, std::memory_order_release);
        block->add_ref();  // manager's in-flight ref

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
        type_fourcc
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

void ResourceManager::run_load_job(void* raw_ctx) noexcept
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
        // visiting_push/pop enables cycle detection for transitive load_sync calls
        // made from inside the loader (which runs on this fiber's thread).
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
            block->permanent = true;  // must be set before state store (memory ordering)
            mgr->m_handles.insert(ctx->id, block);
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
        if (debounce_ms > 0U && elapsed_ms < static_cast<decltype(elapsed_ms)>(debounce_ms))
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
