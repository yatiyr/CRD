#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>

#include <memory>

namespace crd::resources
{

// Opaque handle to an active manifest mount.
struct MountId
{
    crd::u32 value = 0;

    [[nodiscard]] bool is_valid() const noexcept { return value != 0; }
};

// Internal per-entry record stored in the live manifest table.
struct MountEntry
{
    ResourceId                   id{};
    crd::u32                     type_fourcc  = 0;
    crd::u32                     flags        = 0;
    crd::u64                     blob_offset  = 0;
    crd::u64                     blob_size    = 0;
    crd::containers::String      name;
    crd::u32                     mount_id     = 0;
};

// Central resource registry (v1a shell — no loading yet).
//
// Startup sequence:
//   1. Construct ResourceManager with an IAllocator*.
//   2. register_loader() for each resource type.
//   3. mount_manifest() for each cooked pack (engine → project → DLC order).
//   4. (v1c+) load_sync / load_async.
//
// Multi-mount precedence: newest mount wins on ResourceId collision.
// Collisions are logged at Warn level.
//
// Thread safety: registration and mounting are NOT thread-safe; perform them
// on the main thread before any worker fibers call load_*.
class ResourceManager
{
public:
    explicit ResourceManager(crd::memory::IAllocator* a);
    ~ResourceManager();

    ResourceManager(const ResourceManager&)            = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;
    ResourceManager(ResourceManager&&)                 = delete;
    ResourceManager& operator=(ResourceManager&&)      = delete;

    // ── Registration ───────────────────────────────────────────────────────
    // Register a typed loader. Asserts if a loader for the same FourCC is
    // already registered. Call before mount_manifest().
    void register_loader(std::unique_ptr<ILoader> loader);

    // ── Mounts ─────────────────────────────────────────────────────────────
    // Mount a cooked PACK file. Returns an invalid MountId on failure.
    // Caller must keep the returned MountId to unmount later if needed.
    [[nodiscard]] MountId mount_manifest(crd::containers::StringView path);

    // Remove a mounted pack. Entries from this mount are evicted from the
    // live table only when no other mount provides the same ResourceId.
    void unmount(MountId id);

    // ── Lookup (v1c+: these become load_sync / load_async) ─────────────────
    // Find the mount entry for a ResourceId, or nullptr if not mounted.
    [[nodiscard]] const MountEntry* find_entry(ResourceId id) const noexcept;

    // ── Diagnostics ────────────────────────────────────────────────────────
    [[nodiscard]] crd::usize loader_count()  const noexcept;
    [[nodiscard]] crd::usize mount_count()   const noexcept;
    [[nodiscard]] crd::usize entry_count()   const noexcept;

private:
    crd::memory::IAllocator* m_alloc;
    crd::u32                 m_next_mount_id = 1U;

    // Loader registry: FourCC → ILoader.
    crd::containers::HashMap<crd::u32, std::unique_ptr<ILoader>> m_loaders;

    // Per-mount bookkeeping: mount_id → list of ResourceIds in that mount.
    struct MountRecord
    {
        crd::u32                           id = 0;
        crd::containers::Array<ResourceId> entries;

        explicit MountRecord(crd::memory::IAllocator* a, crd::u32 mount_id)
            : id(mount_id), entries(a)
        {
        }
    };
    crd::containers::Array<MountRecord> m_mounts;

    // Live table: ResourceId → MountEntry (newest-mount-wins).
    crd::containers::HashMap<ResourceId, MountEntry> m_live;
};

} // namespace crd::resources
