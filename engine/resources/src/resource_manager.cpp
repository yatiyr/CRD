#include <crd/resources/resource_manager.hpp>

#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/construct.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/log_channel.hpp>
#include <crd/resources/resource_control_block.hpp>

#include <cstring>

namespace crd::resources
{

// ── Cycle-detection visiting stack (v1c; thread-local) ────────────────────────
//
// When load_sync_impl is called, it pushes `id` before dispatching to the loader
// and pops it after. A recursive call from inside the loader that finds its id
// already on the stack signals a cycle.
//
// v1c limitation: thread-local storage is per-OS-thread. crd-jobs fibers may
// migrate between OS threads at await points. This is safe in v1c because all
// loads are synchronous and no fiber suspend can occur mid-load. Revisit when
// load_async runs on fibers (v1d).

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

} // anonymous namespace

// ── Construction / destruction ─────────────────────────────────────────────

ResourceManager::ResourceManager(crd::memory::IAllocator* a)
    : m_alloc(a)
    , m_loaders(a)
    , m_mounts(a)
    , m_live(a)
    , m_handles(a)
{
}

ResourceManager::~ResourceManager()
{
    // Unload and free all permanent control blocks.
    for (auto it = m_handles.begin(); it != m_handles.end(); ++it)
    {
        ResourceControlBlock* block = it.value();
        if (block->payload && block->loader)
        {
            block->loader->unload(block->payload);
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
        return;
    }
}

// ── Lookup ─────────────────────────────────────────────────────────────────

const MountEntry* ResourceManager::find_entry(ResourceId id) const noexcept
{
    return m_live.find(id);
}

// ── Diagnostics ────────────────────────────────────────────────────────────

crd::usize ResourceManager::loader_count() const noexcept { return m_loaders.size(); }
crd::usize ResourceManager::mount_count()  const noexcept { return m_mounts.size(); }
crd::usize ResourceManager::entry_count()  const noexcept { return m_live.size(); }
crd::usize ResourceManager::handle_count() const noexcept { return m_handles.size(); }

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

// Allocate a non-permanent Failed control block owned by the caller's handle.
// Used for all error paths where no cached block exists.
static ResourceControlBlock* make_failed_block(crd::memory::IAllocator* alloc, ResourceId id)
{
    void* raw = alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
    auto* block = new (raw) ResourceControlBlock();
    block->id    = id;
    block->alloc = alloc;
    // permanent = false (default) → freed by the last handle when refs drops to 0
    block->state.store(LoadState::Failed, std::memory_order_release);
    return block;
}

ResourceControlBlock* ResourceManager::load_sync_impl(ResourceId id)
{
    // Return cached block if already loaded successfully.
    ResourceControlBlock** existing = m_handles.find(id);
    if (existing != nullptr)
    {
        (*existing)->add_ref();
        return *existing;
    }

    // Cycle detection.
    if (visiting_contains(id))
    {
        const auto id_str = id.to_string(m_alloc);
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::load_sync: dependency cycle detected for id {}",
                      id_str.c_str());
        return make_failed_block(m_alloc, id);
    }

    // Resolve manifest entry.
    const MountEntry* entry = m_live.find(id);
    if (entry == nullptr)
    {
        const auto id_str = id.to_string(m_alloc);
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::load_sync: id {} not found in any mounted pack",
                      id_str.c_str());
        return make_failed_block(m_alloc, id);
    }

    // Resolve loader.
    std::unique_ptr<ILoader>* loader_ptr = m_loaders.find(entry->type_fourcc);
    if (loader_ptr == nullptr)
    {
        char fc[5];
        fourcc_to_str(entry->type_fourcc, fc);
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::load_sync: no loader registered for FourCC '{}'", fc);
        return make_failed_block(m_alloc, id);
    }
    ILoader* loader = loader_ptr->get();

    // Resolve pack file path.
    const MountRecord* mount = find_mount(entry->mount_id);
    if (mount == nullptr)
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::load_sync: internal error — mount record missing");
        return make_failed_block(m_alloc, id);
    }

    // Read artifact bytes from pack file.
    crd::containers::Array<crd::u8> artifact_bytes(m_alloc);
    const crd::platform::fs::Path pack_path(mount->pack_path);
    if (!crd::platform::fs::read_file_range(pack_path,
                                            entry->blob_offset,
                                            entry->blob_size,
                                            artifact_bytes))
    {
        CRD_LOG_ERROR(g_log_resources,
                      "ResourceManager::load_sync: failed to read artifact bytes");
        return make_failed_block(m_alloc, id);
    }

    // Allocate control block.
    void* raw = m_alloc->allocate(sizeof(ResourceControlBlock), alignof(ResourceControlBlock));
    auto* block = new (raw) ResourceControlBlock();
    block->id          = id;
    block->type_fourcc = entry->type_fourcc;
    block->alloc       = m_alloc;
    block->loader      = loader;

    // Push onto the visiting stack before dispatching to the loader so that
    // any transitive load_sync call for the same id is detected as a cycle.
    visiting_push(id);

    const LoadContext ctx{id, crd::containers::as_const_span(artifact_bytes), this, m_alloc};
    void* payload = loader->load(ctx);

    visiting_pop();

    if (payload != nullptr)
    {
        block->payload   = payload;
        block->permanent = true;
        block->state.store(LoadState::Ready, std::memory_order_release);
        m_handles.insert(id, block);
        CRD_LOG_DEBUG(g_log_resources, "ResourceManager: loaded resource (Ready)");
    }
    else
    {
        // Attempt placeholder fallback.
        void* placeholder = loader->load_placeholder(ctx);
        if (placeholder != nullptr)
        {
            block->payload   = placeholder;
            block->permanent = true;
            block->state.store(LoadState::Placeholder, std::memory_order_release);
            m_handles.insert(id, block);
            CRD_LOG_WARN(g_log_resources, "ResourceManager: loaded resource with placeholder");
        }
        else
        {
            // Hard failure — non-permanent block; freed by the last handle.
            block->state.store(LoadState::Failed, std::memory_order_release);
            CRD_LOG_ERROR(g_log_resources, "ResourceManager: hard failure loading resource");
        }
    }

    return block;
}

} // namespace crd::resources
