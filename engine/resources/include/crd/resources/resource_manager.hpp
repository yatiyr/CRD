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

#include <chrono>
#include <memory>
#include <mutex>

namespace crd::resources
{

// Callback fired after a successful hot-reload.
// Called on the thread that invokes poll_hot_reload() or reload_mount_now().
// `new_generation` matches ResourceHandle::generation() after the reload.
// Fired outside the manager's internal mutex — safe to call load_sync() inside.
using ReloadCallback = void (*)(ResourceId id, crd::u32 new_generation, void* user);

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

// Central resource registry.
//
// Startup sequence:
//   1. Construct ResourceManager with an IAllocator*.
//   2. register_loader() for each resource type.
//   3. mount_manifest() for each cooked pack (engine → project → DLC order).
//   4. load_sync<T>() or load_async<T>() to load resources.
//
// Multi-mount precedence: newest mount wins on ResourceId collision.
// Collisions are logged at Warn level.
//
// Thread safety (v1d):
//   - Registration and mounting are NOT thread-safe; perform them on the main
//     thread before any worker fibers call load_*.
//   - load_sync() and load_async() are thread-safe after registration/mount.
//   - The ResourceManager must outlive all in-flight async loads. Callers must
//     wait() on every Counter returned by read_async() (or call jobs::shutdown())
//     before destroying the manager.
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
    [[nodiscard]] MountId mount_manifest(crd::containers::StringView path);

    // Remove a mounted pack. Entries from this mount are evicted from the
    // live table only when no other mount provides the same ResourceId.
    void unmount(MountId id);

    // ── Synchronous load ───────────────────────────────────────────────────
    // Blocks the calling thread. Safe from any context (fiber or thread).
    // Coalesces with in-flight async loads for the same id.
    template <typename T>
    [[nodiscard]] ResourceHandle<T> load_sync(ResourceId id);

    // ── Asynchronous load ──────────────────────────────────────────────────
    // Submits a load job to the crd-jobs pool. Returns immediately with a
    // handle whose state() is Queued (or Ready if cached).
    // Call handle.wait_ready() to block until the load completes.
    //
    // Coalescing: if another load for the same id is already in flight, the
    // returned handle shares the same control block — no duplicate I/O.
    //
    // Requires crd::jobs to be initialised before the call.
    template <typename T>
    [[nodiscard]] ResourceHandle<T> load_async(ResourceId id);

    // ── Hot-reload ─────────────────────────────────────────────────────────
    // Subscribe a callback for a specific resource. Returns a token for
    // unsubscribe. Safe to call before mount or load. Callback fires on the
    // poll_hot_reload / reload_mount_now caller thread after a successful swap.
    crd::u32 subscribe_reload(ResourceId id, ReloadCallback cb, void* user);

    // Remove a previously-registered callback. Token from subscribe_reload().
    // No-op if the token is invalid or already removed.
    void unsubscribe_reload(ResourceId id, crd::u32 token);

    // Poll all mounted PACK files for mtime changes. Reloads resources after
    // the debounce window expires. Call once per frame from the main thread.
    // Returns the number of resource payloads successfully swapped.
    // Old payloads are freed at the START of the next poll_hot_reload() or
    // reload_mount_now() call (one-frame grace period for raw-pointer holders).
    crd::usize poll_hot_reload(crd::u32 debounce_ms = 200U);

    // Force-reload all loaded resources from a mounted pack, bypassing mtime
    // detection. Useful in tests and external tooling. Returns the number of
    // resources successfully reloaded.
    crd::usize reload_mount_now(MountId id);

    // ── Lookup ─────────────────────────────────────────────────────────────
    [[nodiscard]] const MountEntry* find_entry(ResourceId id) const noexcept;

    // ── Diagnostics ────────────────────────────────────────────────────────
    [[nodiscard]] crd::usize loader_count()   const noexcept;
    [[nodiscard]] crd::usize mount_count()    const noexcept;
    [[nodiscard]] crd::usize entry_count()    const noexcept;
    [[nodiscard]] crd::usize handle_count()   const noexcept;
    [[nodiscard]] crd::usize in_flight_count() const noexcept;

private:
    crd::memory::IAllocator* m_alloc;
    crd::u32                 m_next_mount_id = 1U;

    // Guards m_handles, m_in_flight, m_live, m_loaders, and m_mounts
    // for concurrent load_sync / load_async calls. The mutex is NOT held
    // during I/O or loader dispatch, so recursive load_sync (transitive
    // dependency resolution) does not deadlock.
    mutable std::mutex m_mutex;

    // Loader registry: FourCC → ILoader.
    crd::containers::HashMap<crd::u32, std::unique_ptr<ILoader>> m_loaders;

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
    crd::containers::HashMap<ResourceId, ResourceControlBlock*> m_handles;

    // In-flight table: ResourceId → ResourceControlBlock* (loads in progress).
    // Both load_sync and load_async insert here before starting I/O; removed on completion.
    // Enables coalescing: a second request for the same id shares the same block.
    crd::containers::HashMap<ResourceId, ResourceControlBlock*> m_in_flight;

    // ── Hot-reload private state ────────────────────────────────────────────
    struct ReloadSub
    {
        ReloadCallback cb    = nullptr;
        void*          user  = nullptr;
        crd::u32       token = 0U;
    };

    struct PackWatch
    {
        crd::u32                              mount_id             = 0U;
        crd::containers::String               pack_path;
        crd::i64                              last_processed_mtime = 0;
        crd::i64                              pending_mtime        = 0;
        bool                                  has_pending          = false;
        std::chrono::steady_clock::time_point pending_since{};

        PackWatch(crd::memory::IAllocator* a, crd::u32 id,
                  crd::containers::StringView path, crd::i64 initial_mtime)
            : mount_id(id)
            , pack_path(path.data(), path.size(), a)
            , last_processed_mtime(initial_mtime)
        {}
    };

    struct DeferredFree
    {
        void*    payload = nullptr;
        ILoader* loader  = nullptr;
    };

    crd::u32                                                                   m_next_reload_token = 1U;
    crd::containers::HashMap<ResourceId, crd::containers::Array<ReloadSub>>   m_reload_subs;
    crd::containers::Array<PackWatch>                                          m_pack_watches;
    crd::containers::Array<DeferredFree>                                       m_deferred_frees;

    // Internal helpers.
    [[nodiscard]] ResourceControlBlock* load_sync_impl(ResourceId id);
    [[nodiscard]] ResourceControlBlock* load_async_impl(ResourceId id);
    [[nodiscard]] const MountRecord*    find_mount(crd::u32 mount_id) const noexcept;
    crd::usize                          do_reload_mount(PackWatch& watch);

public:
    // Job entry point for async loads. Receives a heap-allocated AsyncLoadCtx*.
    // Public so the internal SBO-job closure (anonymous namespace) can call it.
    // Not part of the user-facing API; do not call directly.
    static void run_load_job(void* ctx) noexcept;
};

// ── Template implementations (must be in header) ────────────────────────────

template <typename T>
ResourceHandle<T> ResourceManager::load_sync(ResourceId id)
{
    return ResourceHandle<T>(load_sync_impl(id));
}

template <typename T>
ResourceHandle<T> ResourceManager::load_async(ResourceId id)
{
    return ResourceHandle<T>(load_async_impl(id));
}

} // namespace crd::resources
