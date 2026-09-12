#pragma once

// crd-render-asset-core — the canonical COOKED-asset envelope + generational RUNTIME handle (RAF-3, mission §6).
//
// Every render-asset family (shader · material · technique · frame graph) splits into THREE distinct forms:
//   authoring DESC  →  deterministic COOKED bytes  →  immutable RUNTIME object (generation-tagged).
// This header defines the SHARED cooked/runtime primitives so identity + versioning + hashing + dependency +
// generation are defined ONCE, not per cooker:
//   • `CookedHeader` — the fixed prefix EVERY cooked blob begins with (magic · type · schema · interface hash ·
//     content hash · asset id · dependency list). Serialized FIELD-BY-FIELD in little-endian (never a struct memcpy —
//     the ⛔ struct-padding-in-content-hash scar), so cooked bytes are deterministic across compilers/platforms.
//   • `SchemaVersion` / `InterfaceHash` / `ContentHash` — the versioning + invalidation vocabulary. Interface hash
//     changing invalidates every dependent's cached variant (RAF-4); content hash keys the cook cache.
//   • `RuntimeHandle<T>` / `RuntimeSlot<T>` — a generation-tagged handle whose staleness is detectable after a
//     hot-reload replacement (the RAF-11 safety property, established here).
//
// Each family ADOPTS this in its own phase (shader RAF-4 · material/technique RAF-5 · frame RAF-7): its cooked blob
// gains the `CookedHeader` prefix + a dependency record, and its loader validates type/schema before use. Additive
// until then — this module is a leaf and changing nothing existing.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/renderasset/diagnostic.hpp>
#include <crd/renderasset/identity.hpp>

#include <compare>

namespace crd::renderasset
{
using crd::containers::Array;

// A cooked-blob-layout version for one family. Bumped whenever the byte layout changes; the loader rejects a blob
// whose schema differs from what it expects (old-schema handling).
struct SchemaVersion
{
    u32 value = 0;
    friend constexpr bool operator==(SchemaVersion, SchemaVersion) noexcept = default;
};

// A hash of an asset's PUBLIC INTERFACE (stage I/O · binding layout · output signature). When it changes, every
// dependent's cached program variant must be rebuilt (RAF-4). Distinct from ContentHash.
struct InterfaceHash
{
    u64 value = 0;
    friend constexpr bool operator==(InterfaceHash, InterfaceHash) noexcept = default;
};

// A hash of an asset's full cooked CONTENT — the cook-cache key (identical content ⇒ reuse the cached blob).
struct ContentHash
{
    u64 value = 0;
    friend constexpr bool operator==(ContentHash, ContentHash) noexcept = default;
};

// A monotonically-increasing runtime generation. A replacement (hot reload) bumps it; an older handle is stale.
struct Generation
{
    u64 value = 0;
    friend constexpr bool operator==(Generation, Generation) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Generation, Generation) noexcept = default;
};

// 'CRDA' little-endian — the first four bytes of every cooked render-asset blob.
inline constexpr u32 kCookedMagic = 0x41445243U;
// Fixed header size in bytes (excluding the trailing dependency ids): magic(4) + type(4) + schema(4) + iface(8) +
// content(8) + id(8) + dependency_count(4) = 40. Each dependency adds 8 bytes (a u64 AssetId).
inline constexpr usize kCookedHeaderBytes = 40;
inline constexpr usize kCookedDependencyBytes = 8;

// The canonical prefix of every cooked blob. POD; serialized field-by-field (NOT memcpy'd — padding is not in the
// bytes), so `write_cooked_header` is byte-deterministic.
struct CookedHeader
{
    u32 magic = kCookedMagic;
    AssetType type = AssetType::Unknown;
    SchemaVersion schema{};
    InterfaceHash iface{};
    ContentHash content{};
    AssetId id{};
    u32 dependency_count = 0; // followed by this many u64 AssetIds in the blob
};

// FNV-1a interface / content hashes over a byte range (deterministic).
[[nodiscard]] InterfaceHash interface_hash_of(const void* bytes, usize n) noexcept;
[[nodiscard]] ContentHash content_hash_of(const void* bytes, usize n) noexcept;

// Total serialized size of a header carrying `dependency_count` dependencies.
[[nodiscard]] usize cooked_blob_header_size(u32 dependency_count) noexcept;

// Serialize `h` (+ `deps`, which must have `h.dependency_count` entries) into `dst[0..cap)`. Returns the number of
// bytes written, or 0 if `cap` is too small. `deps` may be null iff `h.dependency_count == 0`.
[[nodiscard]] usize write_cooked_header(u8* dst, usize cap, const CookedHeader& h, const AssetId* deps) noexcept;

// Parse + VALIDATE a cooked header from `src[0..size)`. Rejects (with a diagnostic) a bad magic (MalformedBlob), a
// short blob (TruncatedBlob), a type ≠ `expected_type` (TypeMismatch), or a schema ≠ `expected_schema`
// (SchemaMismatch). On success fills `out` + `out_deps` (deterministic order) and returns true.
[[nodiscard]] bool read_cooked_header(const u8* src, usize size, AssetType expected_type, SchemaVersion expected_schema,
                                      CookedHeader& out, Array<AssetId>& out_deps, DiagnosticList& diags);

// A generation-tagged handle to an immutable runtime asset. `generation` lets a holder detect that the asset was
// replaced out from under it (hot reload) without dereferencing a freed pointer.
template <typename T> struct RuntimeHandle
{
    T* ptr = nullptr;
    AssetId id{};
    Generation generation{};

    [[nodiscard]] bool valid() const noexcept { return ptr != nullptr && id.valid(); }
};

// The live slot for one runtime asset. `install` publishes a new immutable object and bumps the generation; a handle
// minted before the last install is detectably stale (`is_current` == false) — the hot-reload safety contract.
template <typename T> class RuntimeSlot
{
public:
    RuntimeHandle<T> install(T* asset, AssetId id) noexcept
    {
        m_current = asset;
        m_id = id;
        m_generation.value += 1;
        return RuntimeHandle<T>{asset, id, m_generation};
    }

    [[nodiscard]] bool is_current(const RuntimeHandle<T>& h) const noexcept
    {
        return h.id == m_id && h.generation == m_generation && m_current != nullptr;
    }

    [[nodiscard]] T* current() const noexcept { return m_current; }
    [[nodiscard]] AssetId id() const noexcept { return m_id; }
    [[nodiscard]] Generation generation() const noexcept { return m_generation; }

private:
    T* m_current = nullptr;
    AssetId m_id{};
    Generation m_generation{};
};
} // namespace crd::renderasset
