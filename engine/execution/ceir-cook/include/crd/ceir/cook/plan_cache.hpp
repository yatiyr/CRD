#pragma once

// crd-ceir-cook — CEIR-10b EXECUTION-PLAN CACHE. A generic content-addressed store keyed by (content hash × target ×
// compiler version); the CALLER compiles an opaque artifact and `put()`s it, `get()` returns it — but ONLY after
// VALIDATING it against live truth (each recorded dependency's CURRENT interface hash, via a resolver), so the cache
// NEVER trusts itself: "compiled artifacts are caches, never truth" made mechanical. ⛔ 10b does NOT produce plans and
// never names the compile→plan interface (that is deferred to CEIR-21/26, per provider.hpp): store + validate only, the
// artifact is opaque bytes this slice. In-memory ONLY (serialized/persistent form → CEIR-11/13 — the real plan payload,
// variants, chunking don't exist yet). Design: docs/design/ceir-10b-execution-plan-cache.md.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/identity.hpp> // AssetId

namespace crd::ceir::cook
{
using AssetId = crd::renderasset::AssetId;

// The cache KEY: content-address × target × compiler version. `content_hash` = stable_hash(module) (the truth the plan
// derives from); `target` = an interned FNV of the IExecutionProvider name (`plan_target`); `compiler_version` bump ⇒
// every entry misses. Equality by all three fields.
struct PlanKey
{
    crd::u64 content_hash     = 0U;
    crd::u64 target           = 0U;
    crd::u32 compiler_version = 0U;
};

// A recorded DEPENDENCY of a cached plan: the callee asset + the `interface_hash` it was compiled against. ⛔ Bridge-local
// — NOT a crd-ceir `KernelRef` mint (crd-ceir is asset-free, I5; the eventual `KernelRef` holds a raw u64, CEIR-13's
// problem, as program_asset.hpp says). Corresponds to ADR-0109 §85: `KernelRef = {asset_id, interface_hash}`. ⛔ Keys on
// `interface_hash` (NOT `contract_hash`) — cache-validity wants CONSERVATISM (a stale plan is corruption; a spurious
// recompile is cheap — the 7a under/over-inclusion asymmetry).
struct PlanDep
{
    AssetId  id;
    crd::u64 interface_hash = 0U;
};

// The RESOLVER: the CURRENT `interface_hash` of a dependency asset. ⛔ EMPTY≠UNKNOWN — a return of 0 (gone / unresolvable)
// means the recorded dep is STALE, never valid. (In the ReloadSet pairing, this reads `set.program(id)->interface_hash`.)
using InterfaceResolver = crd::u64 (*)(AssetId id, void* user);

// NOLINTNEXTLINE(performance-enum-size)
enum class PlanStatus : crd::u8
{
    Hit,       // key matched AND every recorded dep still validates
    Miss,      // no entry for the key
    StaleDeps, // key matched but a recorded dep's interface drifted (or resolved to 0) — the caller must recompile
};

struct PlanLookup
{
    PlanStatus     status   = PlanStatus::Miss;
    const crd::u8* artifact = nullptr; // valid ONLY on Hit; points into cache storage until the entry is evicted/replaced
    crd::usize     size     = 0U;
    [[nodiscard]] bool hit() const noexcept { return status == PlanStatus::Hit; }
};

// Intern a provider name (IExecutionProvider::name()) into a `target` key component — FNV-1a (the CapabilityId pattern).
[[nodiscard]] crd::u64 plan_target(containers::StringView provider_name) noexcept;

// A content-addressed execution-plan cache. ⛔ NOT thread-safe. Artifacts + dep records are stored as HEAP buffers owned
// by the cache (never interior pointers into the growable entry vector — the push-back-UAF discipline).
class PlanCache
{
public:
    PlanCache(memory::IAllocator* alloc, InterfaceResolver resolver, void* user);
    ~PlanCache();
    PlanCache(const PlanCache&)            = delete;
    PlanCache& operator=(const PlanCache&) = delete;
    PlanCache(PlanCache&&)                 = delete;
    PlanCache& operator=(PlanCache&&)      = delete;

    // Look up `key`: no entry ⇒ Miss; entry found + EVERY recorded dep validates (resolver == recorded, != 0) ⇒ Hit;
    // else ⇒ StaleDeps (and the stale entry is dropped — lazy eviction). Hit/StaleDeps update the counters.
    [[nodiscard]] PlanLookup get(const PlanKey& key);
    // Store `artifact` (compiled by the caller) for `key` against `deps`, under `owner` (provenance for `evict`). Copies
    // both. Replaces any existing same-key entry.
    void put(AssetId owner, const PlanKey& key, containers::ConstSpan<crd::u8> artifact, containers::ConstSpan<PlanDep> deps);
    // Drop every entry whose provenance owner is `owner` (memory hygiene on an asset reload/removal).
    void evict(AssetId owner);
    // Drop every entry.
    void clear();

    [[nodiscard]] crd::usize hits() const noexcept { return m_hits; }
    [[nodiscard]] crd::usize misses() const noexcept { return m_misses; }   // Miss + StaleDeps (both ⇒ a recompile)
    [[nodiscard]] crd::usize size() const noexcept { return m_entries.size(); }

private:
    struct Entry
    {
        AssetId    owner{};
        PlanKey    key{};
        crd::u8*   artifact      = nullptr; // heap buffer (stable across entry-vector growth — the vector moves the POD)
        crd::usize artifact_size = 0U;
        PlanDep*   deps          = nullptr; // heap buffer
        crd::usize dep_count     = 0U;
    };

    void                       free_entry(Entry& e) noexcept;
    [[nodiscard]] crd::usize   find(const PlanKey& key) const noexcept; // index, or m_entries.size() if absent
    [[nodiscard]] bool         deps_valid(const Entry& e) const;

    memory::IAllocator*      m_alloc;
    InterfaceResolver        m_resolver;
    void*                    m_user;
    containers::Array<Entry> m_entries;
    crd::usize               m_hits   = 0U;
    crd::usize               m_misses = 0U;
};
} // namespace crd::ceir::cook
