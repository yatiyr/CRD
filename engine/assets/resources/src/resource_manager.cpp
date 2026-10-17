#include <crd/resources/resource_manager.hpp>

#include <crd/core/assert.hpp>
#include <crd/core/lifetime_ledger.hpp>
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

// One control block in `state`, holding the caller's ref. Every block the manager hands out comes from here. The
// manager's bookkeeping allocator may be a sub-budget one that refuses by returning nullptr; a block is what the
// manager reports through (a failed load is a Failed block too), so a refusal leaves it out of memory, fatal in every
// build, never a block built on null.
ResourceControlBlock* new_control_block(crd::memory::IAllocator* alloc, ResourceId id, crd::u32 type_fourcc,
                                        ILoader* loader, LoadState state)
{
    auto* const block = crd::memory::construct_or_fatal<ResourceControlBlock>(
        *alloc, "ResourceManager: out of memory for a resource control block");
    block->id = id;
    block->type_fourcc = type_fourcc;
    block->alloc = alloc;
    block->loader = loader;
    block->state.store(state, std::memory_order_release);
    return block;
}

// Allocate a non-permanent Failed block owned by the caller's handle.
// Used for all error paths where no cached block exists.
ResourceControlBlock* make_failed_block(crd::memory::IAllocator* alloc, ResourceId id)
{
    return new_control_block(alloc, id, 0U, nullptr, LoadState::Failed);
}

// A load job's context, from the manager's bookkeeping allocator: out of memory when it refuses, as for a block.
template <typename Ctx> [[nodiscard]] void* allocate_load_context(crd::memory::IAllocator* alloc, const char* oom)
{
    return crd::memory::allocate_or_fatal(*alloc, sizeof(Ctx), alignof(Ctx), oom);
}

// Why a pack was refused by read_pack (mount and both reload paths share it).
enum class PackRefusal : crd::u8
{
    None,
    Unreadable,
    Unparsable,
    NotPack,
    NoManifest,
    MalformedManifest,
    PastEnd,
    DuplicateId,
};

struct PackRead
{
    PackRefusal refusal     = PackRefusal::None;
    CrdrError   crdr_error  = CrdrError::Ok;
    crd::u64    pack_size   = 0U;
    crd::u64    blob_offset = 0U; // the first entry found past the end, or named twice
    crd::u64    blob_size   = 0U;
};

// Read and check one pack whole: it must read, parse as a PACK container with an MFST chunk of whole entries, name
// only bytes inside the file (a torn pack names bytes past its end) and name each ResourceId once (a second entry
// would make which artifact serves the id depend on manifest order). On success `entries` holds the manifest, named
// from the STRP pool and tagged with `mount_id`, `index` maps each id to its entry, and `file_bytes` holds the pack;
// on a refusal `entries` and `index` are left empty. Nothing outside the arguments is touched.
PackRead read_pack(const crd::platform::fs::Path& path, crd::u32 mount_id, crd::memory::IAllocator* alloc,
                   crd::containers::Array<crd::u8>& file_bytes, crd::containers::Array<MountEntry>& entries,
                   crd::containers::HashMap<ResourceId, crd::usize>& index)
{
    PackRead result;
    if (!crd::platform::fs::read_file_binary(path, file_bytes))
    {
        result.refusal = PackRefusal::Unreadable;
        return result;
    }
    result.pack_size = static_cast<crd::u64>(file_bytes.size());

    CrdrFile crdr_file(alloc);
    result.crdr_error = crdr_read(crd::containers::as_const_span(file_bytes), crdr_file, alloc);
    if (result.crdr_error != CrdrError::Ok)
    {
        result.refusal = PackRefusal::Unparsable;
        return result;
    }
    if (crdr_file.type_fourcc != kFourCC_PACK)
    {
        result.refusal = PackRefusal::NotPack;
        return result;
    }
    const CrdrChunk* mfst_chunk = crdr_find_chunk(crdr_file, kFourCC_MFST);
    if (mfst_chunk == nullptr)
    {
        result.refusal = PackRefusal::NoManifest;
        return result;
    }
    crd::containers::ConstSpan<crd::u8> strp_data{};
    const CrdrChunk* strp_chunk = crdr_find_chunk(crdr_file, kFourCC_STRP);
    if (strp_chunk != nullptr)
    {
        strp_data = strp_chunk->payload;
    }
    crd::containers::Array<ManifestEntry> manifest(alloc);
    if (!manifest_read_entries(mfst_chunk->payload, manifest, alloc))
    {
        result.refusal = PackRefusal::MalformedManifest;
        return result;
    }

    entries.reserve(manifest.size());
    for (const ManifestEntry& e : manifest)
    {
        const bool past_end = e.blob_size > result.pack_size || e.blob_offset > result.pack_size - e.blob_size;
        if (past_end || index.contains(e.id))
        {
            result.refusal     = past_end ? PackRefusal::PastEnd : PackRefusal::DuplicateId;
            result.blob_offset = e.blob_offset;
            result.blob_size   = e.blob_size;
            entries.clear();
            index.clear();
            return result;
        }
        MountEntry live;
        live.id          = e.id;
        live.type_fourcc = e.type_fourcc;
        live.flags       = e.flags;
        live.blob_offset = e.blob_offset;
        live.blob_size   = e.blob_size;
        live.mount_id    = mount_id;
        const crd::containers::StringView name = strp_get(strp_data, e.name_strp_idx);
        live.name = crd::containers::String(name.data(), name.size(), alloc);
        index.insert(e.id, entries.size());
        entries.push_back(std::move(live));
    }
    return result;
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
    // m_thread_safe wraps the caller's allocator; EVERYTHING (bookkeeping containers
    // + the loader-facing m_alloc) routes through it so all access to `a` is
    // serialized by one mutex — concurrent loader jobs (which run without the manager
    // mutex) can't race the manager's container allocations on a non-thread-safe `a`.
    : m_thread_safe(a)
    , m_alloc(&m_thread_safe)
    , m_loaders(&m_thread_safe)
    , m_mounts(&m_thread_safe)
    , m_live(&m_thread_safe)
    , m_handles(&m_thread_safe)
    , m_in_flight(&m_thread_safe)
    , m_reload_subs(&m_thread_safe)
    , m_pack_watches(&m_thread_safe)
    , m_deferred_frees(&m_thread_safe)
    , m_a1in(&m_thread_safe)
    , m_am(&m_thread_safe)
    , m_a1out(&m_thread_safe)
    , m_a1out_set(&m_thread_safe)
    , m_pin_counts(&m_thread_safe)
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

    // Every reload subscription still registered ends with the manager (DIAG.10c lifetime ledger).
    for (auto it = m_reload_subs.begin(); it != m_reload_subs.end(); ++it)
    {
        for (crd::usize i = 0U; i < it.value().size(); ++i)
        {
            crd::lifetime_ledger().released(crd::LifetimeClass::CallbackRegistration);
        }
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

namespace
{
[[nodiscard]] const char* pack_refusal_text(PackRefusal refusal) noexcept
{
    switch (refusal)
    {
    case PackRefusal::Unreadable:
        return "cannot be read";
    case PackRefusal::Unparsable:
        return "does not parse as a CRDR container";
    case PackRefusal::NotPack:
        return "is not a PACK container";
    case PackRefusal::NoManifest:
        return "has no MFST chunk";
    case PackRefusal::MalformedManifest:
        return "has a malformed MFST chunk";
    case PackRefusal::PastEnd:
        return "names bytes past its end (torn)";
    case PackRefusal::DuplicateId:
        return "names one ResourceId twice";
    case PackRefusal::None:
        break;
    }
    return "was accepted";
}

// Log a refused pack with its reason; `verb` is the action refused ("not mounted" or "not reloaded").
void log_pack_refusal(crd::containers::StringView path, const PackRead& read, const char* verb, bool as_error)
{
    if (as_error)
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager: pack '{}' {} (CRDR error {}, entry {} + {}, pack {} bytes); {}", path,
                      pack_refusal_text(read.refusal), static_cast<int>(read.crdr_error), read.blob_offset,
                      read.blob_size, read.pack_size, verb);
    }
    else
    {
        CRD_LOG_WARN(g_log_resources,
                     "ResourceManager: pack '{}' {} (CRDR error {}, entry {} + {}, pack {} bytes); {}", path,
                     pack_refusal_text(read.refusal), static_cast<int>(read.crdr_error), read.blob_offset,
                     read.blob_size, read.pack_size, verb);
    }
}
} // namespace

MountId ResourceManager::mount_manifest(crd::containers::StringView path)
{
    const crd::platform::fs::Path fs_path(path);

    // A pack is checked whole before the live table is touched: a torn pack (cut short on disk) installed would
    // shadow the entries an earlier mount still serves, and every later load of those ids would fail.
    crd::containers::Array<crd::u8> file_bytes(m_alloc);
    MountRecord                     record(m_alloc, m_next_mount_id, path);
    const PackRead read = read_pack(fs_path, record.id, m_alloc, file_bytes, record.entries, record.index);
    if (read.refusal != PackRefusal::None)
    {
        log_pack_refusal(path, read, "not mounted", true);
        return MountId{0U};
    }

    const crd::u32 mount_id = m_next_mount_id++;
    for (const MountEntry& e : record.entries)
    {
        if (const MountEntry* existing = m_live.find(e.id); existing != nullptr)
        {
            CRD_LOG_WARN(g_log_resources,
                         "ResourceManager: ResourceId collision on mount '{}' "
                         "(existing mount_id={}); newest mount wins",
                         path, existing->mount_id);
        }
    }
    const crd::usize entry_total = record.entries.size();
    m_mounts.push_back(std::move(record));
    for (const MountEntry& e : m_mounts.back().entries)
    {
        refresh_live(e.id);
    }

    const crd::i64 initial_mtime = crd::platform::fs::last_modified_unix_seconds(fs_path);
    m_pack_watches.push_back(PackWatch(m_alloc, mount_id, path, initial_mtime));

    CRD_LOG_INFO(g_log_resources,
                 "ResourceManager: mounted '{}' ({} entries, mount_id={})",
                 path, entry_total, mount_id);

    return MountId{mount_id};
}

void ResourceManager::refresh_live(ResourceId id)
{
    const MountEntry* newest = nullptr;
    for (const MountRecord& r : m_mounts)
    {
        const crd::usize* pos = r.index.find(id);
        if (pos != nullptr && (newest == nullptr || r.id > newest->mount_id))
        {
            newest = &r.entries[*pos];
        }
    }
    m_live.erase(id);
    if (newest != nullptr)
    {
        m_live.insert(id, MountEntry(*newest));
    }
}

bool ResourceManager::reissue_type_matches(ResourceId id, const MountEntry& entry, const char* caller) const
{
    ResourceControlBlock* const* pp = m_handles.find(id);
    if (pp == nullptr || (*pp)->type_fourcc == entry.type_fourcc)
    {
        return true;
    }
    char was[5];
    char now[5];
    fourcc_to_str((*pp)->type_fourcc, was);
    fourcc_to_str(entry.type_fourcc, now);
    const auto id_str = id.to_string(m_alloc);
    CRD_LOG_ERROR(g_log_resources,
                  "ResourceManager::{}: id {} was loaded as '{}' but is now served as '{}'; not re-issued", caller,
                  id_str.c_str(), was, now);
    return false;
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

        // Take the record out first, so each id it served falls back to the newest remaining mount naming it.
        const MountRecord removed(std::move(m_mounts[i]));
        m_mounts.swap_remove(i);
        for (const MountEntry& e : removed.entries)
        {
            refresh_live(e.id);
        }

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

crd::BudgetReading ResourceManager::memory_pressure() const noexcept
{
    std::lock_guard<std::mutex> lk(m_mutex);
    crd::BudgetReading r;
    r.read               = true;
    r.bounded            = m_memory_budget != ~0ULL;
    r.capacity           = m_memory_budget;
    r.in_use             = m_memory_used;
    r.high_water_tracked = true;
    r.high_water         = m_memory_peak;
    r.overruns           = m_budget_overruns;
    return r;
}

void ResourceManager::note_budget_settled() noexcept
{
    if (m_memory_used > m_memory_peak)
    {
        m_memory_peak = m_memory_used;
    }
    if (m_memory_used > m_memory_budget)
    {
        ++m_budget_overruns;
    }
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
            if (pp == nullptr)
            {
                continue;
            }
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
                if (pp == nullptr)
                {
                    continue;
                }
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
    note_budget_settled(); // DIAG.10c: the settled use is the budget's peak; still above it is an overrun
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
    // The Unloaded block step 1 found; a re-issue reuses it.
    ResourceControlBlock* evicted = nullptr;

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
                evicted = b;
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
            if (re_issue && !reissue_type_matches(id, *entry, "load_sync"))
            {
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
                // Reuse the evicted block (m_handles is unchanged under this lock). Give the caller their ref.
                block = evicted;
                block->loader = loader;
                block->add_ref(); // caller's ref
                block->state.store(LoadState::Loading, std::memory_order_release);
            }
            else
            {
                // First-ever load: allocate a new block.
                block = new_control_block(m_alloc, id, type_fourcc, loader, LoadState::Loading);
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
    // The Unloaded block step 1 found; a re-issue reuses it.
    ResourceControlBlock* evicted = nullptr;

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
                evicted = b;
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
        if (re_issue && !reissue_type_matches(id, *entry, "load_async"))
        {
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
            // Reuse the evicted block (m_handles is unchanged under this lock).
            block = evicted;
            block->loader = loader;
            block->add_ref(); // caller's ref
            block->add_ref(); // manager's in-flight ref
            block->state.store(LoadState::Queued, std::memory_order_release);
        }
        else
        {
            // Allocate block (state=Queued). Ref=1 (initial, becomes caller's);
            // manager adds one more for the in-flight period.
            block = new_control_block(m_alloc, id, type_fourcc, loader, LoadState::Queued);
            block->add_ref(); // manager's in-flight ref
        }

        m_in_flight.insert(id, block);
    } // lock released

    // Heap-allocate the context so only the 8-byte pointer lives in the SBO closure.
    void* ctx_raw =
        allocate_load_context<AsyncLoadCtx>(m_alloc, "ResourceManager: out of memory for an async load context");
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
    // The Unloaded block step 1 found; a re-issue reuses it.
    ResourceControlBlock* evicted = nullptr;

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
                evicted = b;
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
        if (re_issue && !reissue_type_matches(id, *entry, "load_streamed"))
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
            block = evicted; // m_handles is unchanged under this lock
            block->loader = loader;
            block->add_ref(); // caller's ref
            block->add_ref(); // manager's in-flight ref
            block->state.store(LoadState::Queued, std::memory_order_release);
        }
        else
        {
            block = new_control_block(m_alloc, id, type_fourcc, loader, LoadState::Queued);
            block->add_ref(); // manager's in-flight ref
        }

        m_in_flight.insert(id, block);
    }

    // Heap-allocate the context.
    void* ctx_raw =
        allocate_load_context<StreamLoadCtx>(m_alloc, "ResourceManager: out of memory for a streamed load context");
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

        crd::platform::AsyncReadStatus read_status = crd::platform::AsyncReadStatus::Pending;
        crd::jobs::Counter* c = af.read_async(
            ctx->blob_offset,
            crd::containers::Span<crd::u8>(buf.data(), buf.size()),
            read_status);

        if (c != nullptr)
        {
            crd::jobs::wait(c); // suspend fiber or spin until read completes
        }

        if (c != nullptr && read_status != crd::platform::AsyncReadStatus::Complete)
        {
            // DIAG.9b: a short, failed or cancelled read never reaches the loader as zero-filled bytes; the load
            // fails like the sync and async paths do on a failed read.
            CRD_LOG_ERROR(g_log_resources,
                          "ResourceManager::run_stream_load_job: artifact read ended {}",
                          crd::platform::async_read_status_name(read_status));
        }
        else if (c != nullptr)
        {
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
    crd::lifetime_ledger().acquired(crd::LifetimeClass::CallbackRegistration);
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
            crd::lifetime_ledger().released(crd::LifetimeClass::CallbackRegistration);
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
    // Step 1: re-read and check the pack whole, as mount_manifest does. A pack refused here (unreadable, unparsable,
    // torn, or naming an id twice) changes nothing: the live table, the mount's manifest and every payload stay.
    const crd::containers::StringView pv{watch.pack_path.data(), watch.pack_path.size()};
    const crd::platform::fs::Path     fs_path(watch.pack_path);
    crd::containers::Array<crd::u8>   file_bytes(m_alloc);
    MountRecord                       fresh(m_alloc, watch.mount_id, pv);
    const PackRead read = read_pack(fs_path, watch.mount_id, m_alloc, file_bytes, fresh.entries, fresh.index);
    if (read.refusal != PackRefusal::None)
    {
        log_pack_refusal(pv, read, "not reloaded", false);
        return 0U;
    }

    // Step 2: under lock -- the pack becomes the mount's manifest, every id it named before or names now is
    // re-resolved (newest mount wins; a dropped id falls back to an older mount), and the reload tasks are collected.
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

        MountRecord* record = nullptr;
        for (MountRecord& r : m_mounts)
        {
            if (r.id == watch.mount_id)
            {
                record = &r;
            }
        }
        if (record == nullptr)
        {
            return 0U;
        }

        crd::containers::Array<ResourceId> affected(m_alloc);
        affected.reserve(record->entries.size() + fresh.entries.size());
        for (const MountEntry& e : record->entries)
        {
            affected.push_back(e.id);
        }
        for (const MountEntry& e : fresh.entries)
        {
            affected.push_back(e.id);
        }
        record->entries = std::move(fresh.entries);
        record->index   = std::move(fresh.index);
        for (const ResourceId& id : affected)
        {
            refresh_live(id);
        }

        for (const MountEntry& e : record->entries)
        {
            // Only an id this mount serves is reloaded from it; a newer mount's id keeps its payload.
            const MountEntry* live = m_live.find(e.id);
            if (live == nullptr || live->mount_id != watch.mount_id)
            {
                continue;
            }
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
            // A payload is only ever produced and released by its own type's loader: a handle's get() casts it to
            // that type. An entry retyped in the new pack keeps the last-good payload.
            if (block->type_fourcc != e.type_fourcc)
            {
                char was[5];
                char now[5];
                fourcc_to_str(block->type_fourcc, was);
                fourcc_to_str(e.type_fourcc, now);
                CRD_LOG_WARN(g_log_resources,
                             "ResourceManager: hot-reload: '{}' retypes a loaded resource ('{}' to '{}'); keeping the "
                             "last-good payload",
                             pv, was, now);
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

    // Step 3: reload each task -- loader dispatch outside the lock. Every range lies inside file_bytes (step 1).
    crd::usize reloaded = 0U;
    for (ReloadTask& task : tasks)
    {
        const crd::containers::ConstSpan<crd::u8> artifact_span(
            file_bytes.data() + task.blob_offset, static_cast<crd::usize>(task.blob_size));

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

        // Account the new payload's size (the budget counts what is resident), then copy the subscribers under the
        // lock and fire them outside it.
        crd::containers::Array<ReloadSub> subs_copy(m_alloc);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_memory_used             = m_memory_used - task.block->payload_size + task.blob_size;
            task.block->payload_size  = task.blob_size;
            note_budget_settled(); // DIAG.10c: a reload does not evict, so a larger payload can overrun
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
