#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/serialize.hpp>

namespace crd::scene
{
class World;

// Phase 3.0 v1m — Öbek artifact (ADR-0058).
//
// Öbek is the engine's cooked, instantiable entity-graph template — what
// other engines call a prefab (Unity), Blueprint (Unreal), PackedScene
// (Godot), or unit (Bitsquid). Cerid's name is "öbek" (Turkish: cluster,
// group; from öbeklenmek "to gather into a cluster"), aligning with the
// engine's Anatolian etymology and the precise technical meaning: a
// clustered set of entities+components+relations that gets packed by the
// cooker and pitched into a World at any anchor point.
//
// Phase 3.0 v1m delivered the full ADR-0058 system across 12 sub-slices
// (v1m1–v1m5b). Three items were explicitly carved out as post-Phase-3.0
// follow-ups — see `docs/debt.md` § "Phase 3.0 v1m Öbek system":
//   - Hot-reload watcher with OCHN graph awareness
//   - `obekc extract` CLI tool (ADR-0058 pillar 14 "Decompose")
//   - InheritPolicy CoW dense-buffer optimization
//
// CRDR container with type_fourcc = 'OBEK' and the following chunks
// (sorted ascending by FourCC at write time per CRDR convention):
//
//   'OINF' (16 bytes)            — schema version, entity/component/
//                                   relation counts.
//   'OSTR' (variable)            — string pool: component / relation tag
//                                   names + entity names (diagnostic +
//                                   override-fallback identity).
//   'OCMP' (component_count × sizeof(ObekComponentDescriptor))
//                                — per-component descriptors.
//   'OETB' (entity_count × sizeof(ObekEntityRecord))
//                                — per-entity record: file_idx, name,
//                                   parent_file_idx, flags.
//   'D000'…'D0FF' (per component) — per-component payload chunks (SoA).
//                                   Same shape as SCEN's 'C###' chunks;
//                                   different FourCC range to keep tools
//                                   unambiguous.
//   'ORLS' (relation_count × sizeof(ObekRelationRecord))
//                                — relation records: src_file_idx,
//                                   target_file_idx, relation_fourcc.
//
// Reserved chunks (v1m1 emits them empty / not at all; consumer slices
// populate):
//   'OOVR' — flattened override patches (variant chain pre-applied).
//            Populated by v1m2.
//   'OCHN' — extends + nested öbek dependency list. Populated by v1m2/v1m3.
//   'OBAT' — batch-instancing hints. Populated by v1m5.
//   'OLNK' — lazy-reference deferred loads. Phase 3.5+ runtime.
//
// Determinism (ADR-0058 pillar 13):
//   - Chunk order: FourCC-sorted by CrdrWriter.
//   - Entity table order: SlotMap iteration (deterministic, slot-asc).
//   - Component file-local IDs: ascending registry order.
//   - Component payloads within each D### chunk: file_idx ascending.
//   - Relation order: per-entity registry-order traversal.
//   - No floats reordered, no HashMap iteration leaked, no timestamps.
//   - Same World → same OBEK bytes (verified by tests).

// Header chunk payload (24 bytes).
//
// `obek_root_id` is the FNV-1a 64 hash of the source öbek's canonical
// path + content version, computed by the cooker. v1m1 stamps whatever
// the ObekArtifactBuilder is constructed with; v1m2's TOML cooker
// derives it from the source file path. ObekEntityGuid combines this
// with file_idx to produce stable cross-machine entity identity.
struct ObekInfo
{
    crd::u32 schema_version;
    crd::u32 entity_count;
    crd::u32 component_count;
    crd::u32 relation_count;
    crd::u64 obek_root_id;
};
static_assert(sizeof(ObekInfo) == 24, "ObekInfo size pinned at 24 bytes for OBEK v1");

// Per-component descriptor (32 bytes). Chunk 'OCMP' is an array of these.
// Same layout as SceneComponentDescriptor — keeping the schema parallel
// minimises cooker code duplication.
struct ObekComponentDescriptor
{
    crd::u32 fourcc;            // FourCC source of truth (matches ComponentSerialize::fourcc).
    crd::u32 version;           // ComponentSerialize::version at build time.
    crd::u32 name_strp_offset;  // Diagnostic — type's textual name in OSTR.
    crd::u32 size;              // Bytes per record in the D### payload chunk.
    crd::u32 alignment;         // Alignment of the component type.
    crd::u32 record_count;      // Entities carrying this component.
    crd::u8  storage_hint;      // StorageHint enum (Archetype = 0, SparseSet = 1).
    crd::u8  reserved_u8[3];
    crd::u32 reserved_u32;
};
static_assert(sizeof(ObekComponentDescriptor) == 32, "ObekComponentDescriptor size pinned");

// Per-entity record (16 bytes). Chunk 'OETB' is an array of these.
//
// `name_strp_offset` points into the OSTR pool — the entity's symbolic
// identity for override-patch fallback (ADR-0058 pillar 3) and editor
// "find by name" lookups. v1m1 stores the name; v1m2 consumes it.
//
// `parent_file_idx` is reserved for v1m1 (always 0xFFFFFFFF — the
// authoritative parent relationship is the Relation<ChildOf> stored in
// ORLS). v1m3 may use it to short-circuit nested öbek root detection.
//
// `flags` is the per-entity reservation field (ADR-0058 pillar 18):
//   bit 0  : instance_only   — added per-instance, not in source öbek.
//                              Populated at instantiate time, not in cooked bytes.
//   bit 1  : disabled        — soft-deleted on this instance (v1m5).
//   bits 2-9   : streaming.lod (8 bits, 0 = always loaded)
//   bits 10-31 : reserved (replication mode, static_bake, etc.)
struct ObekEntityRecord
{
    crd::u32 file_idx;          // == array index; stored for invariant check.
    crd::u32 name_strp_offset;  // Symbolic identity for override fallback.
    crd::u32 parent_file_idx;   // 0xFFFFFFFF == no parent in öbek file.
    crd::u32 flags;             // See bit-allocation above.
};
static_assert(sizeof(ObekEntityRecord) == 16, "ObekEntityRecord size pinned at 16 bytes");

// Relation record (16 bytes). Chunk 'ORLS' is an array of these.
struct ObekRelationRecord
{
    crd::u32 src_file_idx;       // File-local index of the source.
    crd::u32 target_file_idx;    // File-local index of the target. 0xFFFFFFFF = null.
    crd::u32 relation_fourcc;    // FourCC of Relation<Tag>; matches OCMP entry.
    crd::u32 reserved;
};
static_assert(sizeof(ObekRelationRecord) == 16, "ObekRelationRecord size pinned");

inline constexpr crd::u32 kObekNullParentIdx = 0xFFFFFFFFU;
inline constexpr crd::u32 kObekNullTargetIdx = 0xFFFFFFFFU;

// Per-entity flag bits (see ObekEntityRecord doc above).
inline constexpr crd::u32 kObekEntityFlag_InstanceOnly = 1U << 0U;
inline constexpr crd::u32 kObekEntityFlag_Disabled     = 1U << 1U;

// ObekEntityGuid — stable cross-machine 64-bit identity for entities-in-an-öbek.
// Hash of (obek_root_id, file_idx). Survives serialization, replay,
// networking, distributed authoring (ADR-0058 pillar 15c). v1m1 declares
// the type and helper; consumers (replication / replay) light up later.
struct ObekEntityGuid
{
    crd::u64 value;

    [[nodiscard]] constexpr bool operator==(const ObekEntityGuid& other) const noexcept
    {
        return value == other.value;
    }
    [[nodiscard]] constexpr bool operator!=(const ObekEntityGuid& other) const noexcept
    {
        return value != other.value;
    }
};

// Compute ObekEntityGuid from (öbek root identity, file_idx). Uses
// FNV-1a 64 mixing — same family as the rest of Cerid's content hashes.
[[nodiscard]] ObekEntityGuid make_obek_entity_guid(crd::u64 obek_root_id, crd::u32 file_idx) noexcept;

// ---- AAAA-tier reservations (ADR-0058 pillar 15) -----------------------
//
// v1m5b ships the format + API surface; the renderer-side instanced-draw
// path lands in Phase 3.5+ when the renderer needs it. The contract:
// the API works today (calling code can declare batched instantiation
// and tag entities); the optimisation lights up later. No format break
// expected when the optimisation ships — OBAT chunk is reserved at v1m5b.

// Hints supplied to `instantiate_obek_batch` to bias future renderer
// optimisations (Phase 3.5+).
struct BatchHints
{
    bool     gpu_instanced = false; // suggests routing to instanced-draw path
    bool     static_bake   = false; // entities never mutated post-spawn
    crd::u8  lod_bucket    = 0U;    // 0 = always loaded; higher = streaming-conditional
    crd::u8  reserved_u8[5]{};
};

// Returned by `instantiate_obek_batch`. Wraps a unique per-call handle the
// renderer (Phase 3.5+) reads to identify batched instances. Currently a
// monotonic per-process counter (no persistence concerns at v1m5b).
struct ObekBatchHandle
{
    crd::u32 value = 0U;
};

// Reserved component carried by entities spawned via `instantiate_obek_batch`.
// Renderer (Phase 3.5+) reads this to detect shared-draw eligibility. Must
// be registered with the target World by the caller (typical pattern: add
// the registration in your render-path's setup). Not registered by default
// to avoid forcing every World to spend a ComponentId slot.
struct BatchInstanceTag
{
    ObekBatchHandle batch;     // which batch this entity came from
    crd::u32        slot;      // 0..N-1 within the batch
};

// FourCC for the BatchInstanceTag component (ADR-0058 pillar 15a).
inline constexpr crd::u32 kFourCC_BatchInstanceTag =
    make_serialize_fourcc('B', 'I', 'T', 'G');

// ObekChainDependency — one entry in the OCHN chunk (ADR-0058 pillar 11
// "hot-reload graph-aware"). Recorded at cook time for every transitive
// dependency the öbek consumes (extends parent + nested öbek refs). A
// hot-reload watcher inspects this list to detect upstream changes that
// invalidate this öbek's cooked bytes.
//
// OCHN chunk layout (self-contained pool — does NOT reuse OSTR):
//   u32 dep_count
//   u32 path_pool_size
//   ObekChainEntryRecord[dep_count]
//   u8  path_pool[path_pool_size]
//
// `content_hash` is the FNV-1a 64 of the dependency's source bytes (for
// `kind = Extends`) or its CRDR cooked bytes (for `kind = Nested`,
// reserved for v1m3+).
//
// v1m2 ships the OCHN format + the ObekArtifactBuilder API. v1m3 (TOML
// cooker) is the first writer that emits non-empty OCHN. v1m5 is the
// consumer (hot-reload watcher).
enum class ObekChainKind : crd::u8
{
    Extends = 0,   // `extends = "..."` — variant inheritance link
    Nested  = 1,   // nested öbek reference (v1m3+)
};

struct ObekChainEntryRecord
{
    crd::u32 path_strp_offset;
    crd::u32 reserved_u32;
    crd::u64 content_hash;
    crd::u8  kind;             // ObekChainKind
    crd::u8  reserved_u8[7];
};
static_assert(sizeof(ObekChainEntryRecord) == 24, "ObekChainEntryRecord size pinned at 24 bytes");

// ObekOverrideRecord — one entry in the OOVR chunk (ADR-0058 pillar 3
// at the cook-time level). Recorded by the cooker when the öbek's
// source TOML has `overrides = [...]` block; each record carries a
// pre-baked component-replacement patch that World::instantiate_obek
// applies automatically BEFORE any caller-supplied overrides (so caller
// patches still win — caller overrides are deepest).
//
// OOVR chunk layout (self-contained payload pool — does NOT reuse OSTR):
//   u32 record_count
//   u32 payload_pool_size
//   ObekOverrideRecord[record_count]
//   u8  payload_pool[payload_pool_size]
//
// `payload_offset` indexes into `payload_pool`; `payload_size` bytes
// at that offset are memcpy'd into the target component at
// `field_offset`. v1m3d ships only whole-component overrides:
// `field_offset = 0`, `payload_size = sizeof(Component)`. Field-level
// overrides (sub-component slices) reserved for v1m5+.
struct ObekOverrideRecord
{
    crd::u32 file_idx;
    crd::u32 component_fourcc;
    crd::u32 field_offset;
    crd::u32 payload_offset;       // into ObekResource::cook_override_payload_pool
    crd::u32 payload_size;
    crd::u32 reserved;
};
static_assert(sizeof(ObekOverrideRecord) == 24, "ObekOverrideRecord size pinned at 24 bytes");

// ObekResource — payload owned by ResourceManager after a load. Same
// shape as SceneResource (ADR-0055): borrows views into the loaded CRDR
// bytes, holds an owned `component_payloads` array of view pairs.
//
// `obek_root_id` is the FNV-1a 64 hash of the source öbek's canonical
// path + content version, computed by the cooker and stored in OINF.
// v1m1 stores 0; v1m2 populates it from the cooker.
struct ObekResource
{
    ObekInfo info{};
    crd::u64 obek_root_id = 0U;

    crd::containers::ConstSpan<crd::u8>                    string_pool{};
    crd::containers::ConstSpan<ObekComponentDescriptor>    component_descriptors{};
    crd::containers::ConstSpan<ObekEntityRecord>           entity_table{};
    crd::containers::ConstSpan<ObekRelationRecord>         relations{};
    crd::containers::ConstSpan<ObekChainEntryRecord>       chain_dependencies{};
    crd::containers::ConstSpan<ObekOverrideRecord>         cook_override_records{};
    crd::containers::ConstSpan<crd::u8>                    cook_override_payload_pool{};

    struct PerComponentPayload
    {
        crd::containers::ConstSpan<crd::u32> indices;   // entity_file_idx[]
        crd::containers::ConstSpan<crd::u8>  payloads;  // raw bytes
    };
    crd::containers::Array<PerComponentPayload> component_payloads;

    // Owned copy of the source CRDR bytes + its parsed chunk view.
    // Every ConstSpan above (string_pool, component_descriptors,
    // entity_table, relations, chain_dependencies, override records +
    // payload pool, per-component indices/payloads) ultimately points
    // into one of these two containers. Loaders MUST populate spans
    // AFTER copying the bytes here and re-parsing — references into
    // the LoadContext's transient byte buffer would dangle as soon as
    // ResourceManager drops it (v1o3 fix; bug surfaced when the
    // sandbox started calling instantiate_obek across the load
    // boundary).
    crd::containers::Array<crd::u8> owned_bytes;
    crd::resources::CrdrFile        parsed_file;

    explicit ObekResource(crd::memory::IAllocator* alloc)
        : component_payloads(alloc), owned_bytes(alloc), parsed_file(alloc)
    {
    }
};

// ObekLoader — ILoader for the 'OBEK' container. Parses CRDR bytes into
// an ObekResource. Does NOT instantiate entities at load time — that's
// `World::instantiate_obek(const ObekResource&, EntityId parent)`.
class ObekLoader : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_OBEK; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override;
    void unload(void* payload) noexcept override;
};

// Sentinel: when ObekOverride::file_idx == this value, the runtime
// looks up the target entity by symbolic name (`ObekOverride::name`)
// against the öbek's OSTR pool + OETB name_strp_offsets. ADR-0058
// pillar 3 ("symbolic name fallback").
inline constexpr crd::u32 kObekOverrideUseName = 0xFFFFFFFFU;

// ObekOverride — typed runtime override patch (ADR-0058 pillar 3).
//
// Applied at instantiation by `World::instantiate_obek(...)`. Caller owns
// the payload memory; ObekOverride is a non-owning view that lives only
// for the duration of the instantiate call.
//
// Identity resolution:
//   - If `file_idx < res.info.entity_count`, that entity is used directly.
//   - Else if `file_idx == kObekOverrideUseName`, the runtime walks OSTR/OETB
//     looking for an entity whose stored name matches `name`. This is the
//     "öbek source restructured; file_idx no longer authoritative" path.
//   - Anything else → patch silently skipped (counted in
//     `ObekInstantiation::overrides_skipped`).
//
// Field path (v1m2): `field_offset` is a byte offset within the target
// component. `payload.size()` MUST equal the field's declared width as
// understood by the caller; the runtime memcpy's `payload.size()` bytes
// at `(component_start + field_offset)` after a bounds check against
// `info->size`. Out-of-bounds patches are skipped.
//
// The full ADR-0058 spec encodes (16-bit offset + 8-bit element_idx +
// 8-bit kind) into a single 32-bit field_path. v1m2 ships only
// `field_offset` (the 16-bit half); element_idx + kind reserved as
// trailing zero bits in the OOVR disk format and unused at runtime.
//
// `component_fourcc == 0` is reserved for relation overrides (v1m3+).
// v1m2 only applies component-field overrides.
struct ObekOverride
{
    crd::u32                       file_idx        = kObekOverrideUseName;
    crd::containers::StringView    name            = crd::containers::StringView{};
    crd::u32                       component_fourcc = 0U;
    crd::u32                       field_offset    = 0U;
    crd::containers::ConstSpan<crd::u8> payload{};
};

// ObekInstantiation — return value of `World::instantiate_obek`.
//
// `entities[file_idx]` = the EntityId spawned for that file-local index.
// Stable mapping for the lifetime of the World.
//
// `parent` = the entity supplied at instantiation; root entities of the
// öbek (those with no ChildOf relation in the source) get a ChildOf
// relation to this entity installed during instantiation. EntityId::null()
// = no reparenting (öbek roots remain top-level).
//
// `obek_root_id` = ObekResource::obek_root_id at the time of the call;
// kept here so callers can compute ObekEntityGuids without re-fetching
// the resource.
//
// Move-only (ADR-0058 pillar matches v1k's Query / SceneInstantiation).
struct ObekInstantiation
{
    crd::containers::Array<EntityId> entities;
    EntityId parent = EntityId::null();
    crd::u64 obek_root_id = 0U;
    crd::u32 components_skipped = 0U;
    crd::u32 relations_skipped  = 0U;
    crd::u32 overrides_applied  = 0U;
    crd::u32 overrides_skipped  = 0U;

    // v1m5a — link back to the ObekResource the instance was built from.
    // Used by revert / enumerate / unpack APIs. Caller is responsible for
    // keeping the resource alive (typical pattern: hold the ResourceHandle).
    // Cleared by unpack_obek / unpack_obek_keep_overrides.
    const ObekResource* source = nullptr;

    explicit ObekInstantiation(crd::memory::IAllocator* alloc) : entities(alloc) {}
    ObekInstantiation(const ObekInstantiation&)            = delete;
    ObekInstantiation& operator=(const ObekInstantiation&) = delete;
    ObekInstantiation(ObekInstantiation&&) noexcept        = default;
    ObekInstantiation& operator=(ObekInstantiation&&) noexcept = default;

    // ObekEntityGuid for the spawned entity at file_idx. Caller can use
    // this to key into save files, network snapshots, replay logs.
    [[nodiscard]] ObekEntityGuid guid_for(crd::u32 file_idx) const noexcept
    {
        return make_obek_entity_guid(obek_root_id, file_idx);
    }
};

// ObekArtifactBuilder — emits an OBEK CRDR blob from a World snapshot.
// Test-only public API in v1m1 (matches v1k's SceneArtifactBuilder
// pattern); v1m2 cooker handler extends it with extends-chain flattening
// and override patch resolution.
class ObekArtifactBuilder
{
public:
    ObekArtifactBuilder(crd::memory::IAllocator* alloc,
                        crd::resources::ResourceId id,
                        crd::u64 obek_root_id = 0U);

    // Record an upstream dependency for the OCHN chunk. Called once per
    // `extends` link (kind=Extends) and once per nested-öbek reference
    // (kind=Nested) by the cooker before `build()`. Order is preserved
    // in the cooked OCHN chunk → deterministic byte output.
    void add_chain_dependency(crd::containers::StringView canonical_path,
                              crd::u64 content_hash,
                              ObekChainKind kind);

    // Record a cook-time override patch (ADR-0058 pillar 3 cook-time half).
    // Called by the cooker once per `overrides = [...]` TOML entry. The
    // builder copies `payload` bytes into an internal payload pool;
    // emits one OOVR record per call at `build()` time.
    //
    // v1m3d ships whole-component overrides (`field_offset = 0`,
    // `payload.size() = sizeof(Component)`). Field-level overrides
    // reserved for v1m5+ when the cooker can route field-name → offset
    // through a registered schema.
    void add_override(crd::u32 file_idx,
                      crd::u32 component_fourcc,
                      crd::u32 field_offset,
                      crd::containers::ConstSpan<crd::u8> payload);

    // Walk every alive entity in `world` and emit a CRDR-formatted blob.
    // Components without a ComponentSerialize trait are silently skipped.
    // Same for relations whose target is outside the World.
    [[nodiscard]] crd::containers::Array<crd::u8> build(const World& world);

private:
    struct PendingDep
    {
        crd::containers::String path;
        crd::u64                content_hash;
        ObekChainKind           kind;

        explicit PendingDep(crd::memory::IAllocator* a) : path(a) {}
        PendingDep(PendingDep&&) = default;
        PendingDep& operator=(PendingDep&&) = default;
    };

    struct PendingOverride
    {
        crd::u32                        file_idx;
        crd::u32                        component_fourcc;
        crd::u32                        field_offset;
        crd::containers::Array<crd::u8> payload;

        explicit PendingOverride(crd::memory::IAllocator* a)
            : file_idx(0U), component_fourcc(0U), field_offset(0U), payload(a) {}
        PendingOverride(PendingOverride&&)            = default;
        PendingOverride& operator=(PendingOverride&&) = default;
    };

    crd::memory::IAllocator*                m_alloc;
    crd::resources::ResourceId              m_id;
    crd::u64                                m_obek_root_id;
    crd::containers::Array<PendingDep>      m_pending_deps;
    crd::containers::Array<PendingOverride> m_pending_overrides;
};

} // namespace crd::scene
