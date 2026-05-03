#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
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

// Central resource registry (v1c — synchronous load with refcounted handles).
//
// Startup sequence:
//   1. Construct ResourceManager with an IAllocator*.
//   2. register_loader() for each resource type.
//   3. mount_manifest() for each cooked pack (engine → project → DLC order).
//   4. load_sync<T>() to load resources.
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

    // ── Synchronous load ───────────────────────────────────────────────────
    // Reads artifact bytes from the pack file, dispatches to the registered
    // loader, and returns a handle. Handles are refcounted; the payload lives
    // until all handles are dropped (v1g eviction) or the manager is destroyed.
    //
    // Transitive dependency resolution: loaders may call load_sync<Dep> on
    // ctx.manager. Cycles are detected via a thread-local visiting stack and
    // terminate with a Failed handle (logged at Error).
    //
    // Failed loads are NOT cached; a second call will retry the load.
    // Successful loads ARE cached; a second call returns the cached block.
    template <typename T>
    [[nodiscard]] ResourceHandle<T> load_sync(ResourceId id);

    // ── Lookup ─────────────────────────────────────────────────────────────
    // Find the mount entry for a ResourceId, or nullptr if not mounted.
    [[nodiscard]] const MountEntry* find_entry(ResourceId id) const noexcept;

    // ── Diagnostics ────────────────────────────────────────────────────────
    [[nodiscard]] crd::usize loader_count()  const noexcept;
    [[nodiscard]] crd::usize mount_count()   const noexcept;
    [[nodiscard]] crd::usize entry_count()   const noexcept;
    [[nodiscard]] crd::usize handle_count()  const noexcept;

private:
    crd::memory::IAllocator* m_alloc;
    crd::u32                 m_next_mount_id = 1U;

    // Loader registry: FourCC → ILoader.
    crd::containers::HashMap<crd::u32, std::unique_ptr<ILoader>> m_loaders;

    // Per-mount bookkeeping: mount_id → list of ResourceIds in that mount.
    struct MountRecord
    {
        crd::u32                           id = 0;
        crd::containers::String            pack_path;
        crd::containers::Array<ResourceId> entries;

        MountRecord(crd::memory::IAllocator* a, crd::u32 mount_id, crd::containers::StringView path)
            : id(mount_id), pack_path(path.data(), path.size(), a), entries(a)
        {
        }
    };
    crd::containers::Array<MountRecord> m_mounts;

    // Live manifest table: ResourceId → MountEntry (newest-mount-wins).
    crd::containers::HashMap<ResourceId, MountEntry> m_live;

    // Handle table: ResourceId → ResourceControlBlock* (permanent blocks only).
    // Blocks are allocated by the manager and freed in ~ResourceManager().
    crd::containers::HashMap<ResourceId, ResourceControlBlock*> m_handles;

    // Internal helpers.
    [[nodiscard]] ResourceControlBlock* load_sync_impl(ResourceId id);
    [[nodiscard]] const MountRecord*    find_mount(crd::u32 mount_id) const noexcept;
};

// ── Template implementation (must be in header) ─────────────────────────────

template <typename T>
ResourceHandle<T> ResourceManager::load_sync(ResourceId id)
{
    return ResourceHandle<T>(load_sync_impl(id));
}

} // namespace crd::resources
