#include <crd/resources/resource_manager.hpp>

#include <crd/core/assert.hpp>
#include <crd/log/log.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/log_channel.hpp>

#include <cstring>

namespace crd::resources
{

namespace
{

// Look up a null-terminated string in the STRP pool at byte offset `idx`.
// Returns an empty StringView if `idx` is out of range.
crd::containers::StringView strp_get(
    crd::containers::ConstSpan<crd::u8> pool,
    crd::u32                            idx) noexcept
{
    if (idx >= static_cast<crd::u32>(pool.size()))
    {
        return {};
    }
    const char* begin = reinterpret_cast<const char*>(pool.data() + idx);
    // Find null terminator, but do not walk past the pool boundary.
    const char* end = begin;
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
{
}

ResourceManager::~ResourceManager() = default;

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
    MountRecord record(m_alloc, mount_id);

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

        // Remove live entries that belong only to this mount.
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

crd::usize ResourceManager::loader_count() const noexcept
{
    return m_loaders.size();
}

crd::usize ResourceManager::mount_count() const noexcept
{
    return m_mounts.size();
}

crd::usize ResourceManager::entry_count() const noexcept
{
    return m_live.size();
}

} // namespace crd::resources
