#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/entity.hpp>
#include <crd/scene/serialize.hpp>

namespace crd::scene
{
class World;

// Phase 3.0 v1k — SCEN artifact (ADR-0055).
//
// CRDR container with type_fourcc = 'SCEN' and the following chunks (sorted
// ascending by FourCC at write time per CRDR convention):
//
//   'INFO' (16 bytes)            — schema version, entity/component/relation counts.
//   'STRP' (variable)            — string pool: component names + relation tag names
//                                   for diagnostic / log purposes only. The FourCC is
//                                   the source of truth for type identity.
//   'CMPS' (component_count × sizeof(SceneComponentDescriptor))
//                                — per-component descriptors (FourCC, version,
//                                   size, alignment, storage hint, record count).
//   'ETBL' (entity_count × 4 bytes)
//                                — per-entity reserved-flags slot. Future flags:
//                                   Pinned (don't unload), Static (skip propagation),
//                                   EditorOnly (strip on cook). v1k always emits 0.
//   'C000'…'C0FF' (per component) — per-component payload chunks. SoA layout:
//                                     u32 record_count + u32 reserved
//                                     + u32 indices[record_count]
//                                     + (alignment padding)
//                                     + u8 payloads[record_count * size]
//   'RELS' (relation_count × 16 bytes)
//                                — relation records: (src_file_idx,
//                                   target_file_idx, relation_fourcc, reserved).
//
// Determinism (advisor pin #6):
//   - Chunk order: sorted ascending by FourCC (CRDR enforces).
//   - Entity table order: alive-iteration order from the source World's
//     SlotMap (deterministic; matches spawn order).
//   - Component file-local IDs: ascending registry order at build time.
//   - Component payloads within each C### chunk: in entity-table order.
//   - Relation order: per-entity registry-order traversal, then within
//     each relation: source-entity-table order.
//   - No floating-point reordering, no HashMap iteration leakage, no
//     timestamps. Same World → same SCEN bytes (verified in tests).
//
// Endianness (advisor pin #7): little-endian, inherits CRDR. Cross-
// platform big-endian / ARM-LE-but-different-word-size targets are a
// v1n+ concern; documented here for record.

// Header chunk payload (16 bytes).
struct SceneInfo
{
    crd::u32 schema_version;
    crd::u32 entity_count;
    crd::u32 component_count;
    crd::u32 relation_count;
};
static_assert(sizeof(SceneInfo) == 16, "SceneInfo size pinned at 16 bytes for SCEN v1");

// Per-component descriptor (32 bytes). Chunk 'CMPS' is an array of these.
struct SceneComponentDescriptor
{
    crd::u32 fourcc;            // FourCC source of truth (matches ComponentSerialize::fourcc).
    crd::u32 version;           // ComponentSerialize::version at build time.
    crd::u32 name_strp_offset;  // Diagnostic only — type's textual name in STRP.
    crd::u32 size;              // Bytes per record in the C### payload chunk.
    crd::u32 alignment;         // Alignment of the component type.
    crd::u32 record_count;      // Entities carrying this component.
    crd::u8  storage_hint;      // StorageHint enum (Archetype = 0, SparseSet = 1).
    crd::u8  reserved_u8[3];
    crd::u32 reserved_u32;
};
static_assert(sizeof(SceneComponentDescriptor) == 32, "SceneComponentDescriptor size pinned");

// Relation record (16 bytes). Chunk 'RELS' is an array of these.
struct SceneRelationRecord
{
    crd::u32 src_file_idx;       // File-local entity index of the source.
    crd::u32 target_file_idx;    // File-local entity index of the target. 0xFFFFFFFF = null.
    crd::u32 relation_fourcc;    // FourCC of Relation<Tag>; matches CMPS entry.
    crd::u32 reserved;
};
static_assert(sizeof(SceneRelationRecord) == 16, "SceneRelationRecord size pinned");

inline constexpr crd::u32 kSceneNullTargetIdx = 0xFFFFFFFFU;

// SceneResource — payload owned by ResourceManager after a load. Borrows
// views into the loaded CRDR bytes. Lifetime: valid until the resource
// handle is released.
struct SceneResource
{
    SceneInfo info{};

    // Diagnostic string pool (UTF-8 bytes; component names + relation
    // tag names). Indexed by `name_strp_offset` from the CMPS table.
    crd::containers::ConstSpan<crd::u8> string_pool{};

    // Per-component descriptors, in file-local-index order.
    crd::containers::ConstSpan<SceneComponentDescriptor> component_descriptors{};

    // Per-entity reserved-flags array (one u32 per entity). v1k always
    // contains 0; future flags slot through here without bumping schema.
    crd::containers::ConstSpan<crd::u32> entity_table{};

    // Relation records.
    crd::containers::ConstSpan<SceneRelationRecord> relations{};

    // Per-component-id payload views (indices + raw payload bytes).
    // Parallel to `component_descriptors`. Allocator-owned because the
    // resident SceneResource needs to outlive the parsed CrdrFile that
    // produced these views.
    struct PerComponentPayload
    {
        crd::containers::ConstSpan<crd::u32> indices;   // entity_file_idx[]
        crd::containers::ConstSpan<crd::u8>  payloads;  // raw bytes
    };
    crd::containers::Array<PerComponentPayload> component_payloads;

    explicit SceneResource(crd::memory::IAllocator* alloc) : component_payloads(alloc) {}
};

// SceneLoader — ILoader for the 'SCEN' container. Validates and parses
// into a SceneResource. Does NOT instantiate entities at load time —
// that's `World::instantiate_scene(SceneResource&)`.
class SceneLoader : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override { return kFourCC_SCEN; }
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 1U; }
    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override;
    void unload(void* payload) noexcept override;
};

// SceneInstantiation — return value of `World::instantiate_scene`.
//   - `entities[file_idx]` = the EntityId spawned for that file-local
//     index. Stable mapping for the lifetime of the World.
//   - `components_skipped` = # of (entity, component) pairs whose
//     FourCC was not registered in the target World (forward-compat).
//   - `relations_skipped` = # of relation records whose FourCC was not
//     registered (forward-compat).
//
// Move-only (advisor pin #3): copying duplicates the entity-id array
// for no caller benefit.
struct SceneInstantiation
{
    crd::containers::Array<EntityId> entities;
    crd::u32 components_skipped = 0;
    crd::u32 relations_skipped  = 0;

    explicit SceneInstantiation(crd::memory::IAllocator* alloc) : entities(alloc) {}
    SceneInstantiation(const SceneInstantiation&)            = delete;
    SceneInstantiation& operator=(const SceneInstantiation&) = delete;
    SceneInstantiation(SceneInstantiation&&) noexcept        = default;
    SceneInstantiation& operator=(SceneInstantiation&&) noexcept = default;
};

// SceneArtifactBuilder — emits a SCEN CRDR blob from a World snapshot.
// Test-only public API in v1k (advisor pin #4); v1l's cook_scene cooker
// promotes it to a first-class cooker handler.
class SceneArtifactBuilder
{
public:
    SceneArtifactBuilder(crd::memory::IAllocator* alloc, crd::resources::ResourceId id);

    // Walk every alive entity in `world` and emit a CRDR-formatted blob.
    // Components without a ComponentSerialize trait are silently skipped
    // (their data isn't persistable). Same for relations.
    [[nodiscard]] crd::containers::Array<crd::u8> build(const World& world);

private:
    crd::memory::IAllocator*    m_alloc;
    crd::resources::ResourceId  m_id;
};

} // namespace crd::scene
