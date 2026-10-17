// Phase 3.0 v1k — SCEN serialisation: artifact builder, loader, instantiation.
//
// Three closely-coupled pieces in one TU:
//
//   - SceneArtifactBuilder::build  — walks a World, emits CRDR bytes.
//   - SceneLoader::load            — parses CRDR bytes into a SceneResource.
//   - World::instantiate_scene     — creates entities/components/relations
//                                    from a SceneResource into a target World.
//
// Determinism: every walk is registry-order or entity-table-order; chunk
// order is FourCC-sorted by CrdrWriter. Same World → same bytes.
// Forward-compat: unknown FourCCs skip. The loader refuses a pack whose
// records do not lie inside it; instantiation refuses one this World cannot
// take (a known FourCC with another size, alignment or version, or a cycle in
// an Acyclic relation) before it spawns anything (pack_validation.hpp).

#include "pack_validation.hpp"

#include <crd/core/assert.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/world.hpp>

#include <cstring>

namespace crd::scene
{

namespace
{
// Helper: append `n` bytes from `src` to the back of `dst`.
inline void append_bytes(crd::containers::Array<crd::u8>& dst, const void* src, crd::usize n)
{
    const auto base_size = dst.size();
    dst.resize(base_size + n);
    std::memcpy(&dst[base_size], src, n);
}

// Helper: append a u32 little-endian (host == LE on x86/ARM).
inline void append_u32(crd::containers::Array<crd::u8>& dst, crd::u32 v)
{
    append_bytes(dst, &v, sizeof(v));
}

// Round up to alignment.
[[nodiscard]] inline crd::usize align_up(crd::usize n, crd::usize a) noexcept
{
    return (n + a - 1U) & ~(a - 1U);
}

// Pad `dst` with zeros up to alignment `a`.
inline void pad_to_align(crd::containers::Array<crd::u8>& dst, crd::usize a)
{
    const auto target = align_up(dst.size(), a);
    while (dst.size() < target)
    {
        dst.push_back(0U);
    }
}

// Write a string into the pool; return the byte offset.
[[nodiscard]] crd::u32 strp_intern(crd::containers::Array<crd::u8>& pool, crd::containers::StringView name)
{
    const auto offset = pool.size();
    for (char c : name)
    {
        pool.push_back(static_cast<crd::u8>(c));
    }
    pool.push_back(0U); // null terminator for diagnostic readability
    return static_cast<crd::u32>(offset);
}

// Parses `bytes` (the loader's copy of the artifact, `res.owned_bytes`) into `res`'s views. False when the pack is
// refused: a chunk that does not hold exactly its records, or a record that does not lie inside the pack
// (SceneResource's rules).
[[nodiscard]] bool parse_scene(SceneResource& res, crd::containers::ConstSpan<crd::u8> bytes,
                               crd::memory::IAllocator* alloc)
{
    namespace pd = pack_detail;
    if (crd::resources::crdr_read(bytes, res.parsed_file, alloc) != crd::resources::CrdrError::Ok ||
        res.parsed_file.type_fourcc != kFourCC_SCEN)
    {
        return false;
    }
    const crd::resources::CrdrFile& file = res.parsed_file;

    // INFO chunk — required.
    const crd::resources::CrdrChunk* info_chunk = crd::resources::crdr_find_chunk(file, kFourCC_SceneINFO);
    if (info_chunk == nullptr || info_chunk->payload.size() != sizeof(SceneInfo))
    {
        return false;
    }
    std::memcpy(&res.info, info_chunk->payload.data(), sizeof(SceneInfo));
    if (res.info.schema_version != kSceneSchemaVersion || res.info.component_count > kMaxComponents)
    {
        return false;
    }

    // Optional STRP chunk.
    if (const auto* strp = crd::resources::crdr_find_chunk(file, kFourCC_SceneSTRP); strp != nullptr)
    {
        res.string_pool = strp->payload;
    }

    // CMPS chunk — required if component_count > 0; every descriptor valid before its payload chunk is read.
    if (res.info.component_count > 0U)
    {
        const auto* cmps = crd::resources::crdr_find_chunk(file, kFourCC_SceneCMPS);
        if (cmps == nullptr || !pd::copy_records(*cmps, res.info.component_count, res.descriptor_records))
        {
            return false;
        }
        for (const SceneComponentDescriptor& d : res.descriptor_records)
        {
            if (!pd::descriptor_is_valid(d))
            {
                return false;
            }
        }
    }
    res.component_descriptors = crd::containers::as_const_span(res.descriptor_records);

    // ETBL chunk — required if entity_count > 0.
    if (res.info.entity_count > 0U)
    {
        const auto* etbl = crd::resources::crdr_find_chunk(file, kFourCC_SceneETBL);
        if (etbl == nullptr || !pd::copy_records(*etbl, res.info.entity_count, res.entity_records))
        {
            return false;
        }
    }
    res.entity_table = crd::containers::as_const_span(res.entity_records);

    // RELS chunk — present exactly when relation_count > 0.
    const auto* rels = crd::resources::crdr_find_chunk(file, kFourCC_SceneRELS);
    if (rels != nullptr ? !pd::copy_records(*rels, res.info.relation_count, res.relation_records)
                        : res.info.relation_count != 0U)
    {
        return false;
    }
    res.relations = crd::containers::as_const_span(res.relation_records);

    // Per-component payload chunks (C000, C001, ...), present exactly when the descriptor has records.
    auto chunk_for = [&file](crd::u32 ci)
    {
        return crd::resources::crdr_find_chunk(file, make_scene_component_chunk_fourcc(ci));
    };
    if (!pd::read_payload_chunks(res.component_descriptors, chunk_for, res.component_payloads, res.index_records))
    {
        return false;
    }

    return pd::components_are_consistent(res.info.entity_count, res.component_descriptors,
                                         crd::containers::as_const_span(res.component_payloads)) &&
           pd::relations_are_consistent(res.info.entity_count, res.relations);
}

} // namespace

// ---- SceneArtifactBuilder ----------------------------------------------

SceneArtifactBuilder::SceneArtifactBuilder(crd::memory::IAllocator* alloc, crd::resources::ResourceId id)
    : m_alloc(alloc), m_id(id)
{
}

crd::containers::Array<crd::u8> SceneArtifactBuilder::build(const World& world)
{
    // ---- Step 1: Collect alive entities in deterministic order. ----------
    //
    // SlotMap iteration order is slot-index ascending — deterministic and
    // matches insertion order modulo any frees. file_idx 0..N-1.
    crd::containers::Array<EntityId>                      file_entities{m_alloc};
    crd::containers::HashMap<crd::u64, crd::u32>          entity_to_idx{m_alloc};
    for (EntityId e : world)
    {
        const crd::u32 idx = static_cast<crd::u32>(file_entities.size());
        entity_to_idx.emplace(e.raw, idx);
        file_entities.push_back(e);
    }

    // ---- Step 2: Collect persistable components (those with serialize trait). ----
    //
    // file_local_id = position in this list. Registry-order traversal
    // ensures determinism. Skip components whose serialize.fourcc is 0
    // (no opt-in) and skip the TransformDirtyFlag-style frame-scoped
    // markers (caller didn't tag them with a serialize trait).
    struct PersistableComponent
    {
        ComponentId id;
        const ComponentInfo* info;
        crd::u32 file_local_id;
        crd::u32 name_strp_offset;
        crd::u32 record_count;
    };
    crd::containers::Array<PersistableComponent> persistable{m_alloc};
    crd::containers::Array<crd::u8>              string_pool{m_alloc};

    const auto& reg = world.components();
    for (crd::u32 i = 0; i < reg.size(); ++i)
    {
        const ComponentId cid{static_cast<crd::u16>(i)};
        const ComponentInfo* info = reg.info(cid);
        if (info == nullptr || info->serialize.fourcc == 0U)
        {
            continue;
        }
        // Empty marker components (size == 0) are not persisted.
        if (info->size == 0U)
        {
            continue;
        }
        PersistableComponent pc{};
        pc.id              = cid;
        pc.info            = info;
        pc.file_local_id   = static_cast<crd::u32>(persistable.size());
        pc.name_strp_offset = strp_intern(string_pool, info->name);
        pc.record_count    = 0; // populated in Step 3
        persistable.push_back(pc);
    }

    // ---- Step 3: Per-component, walk file_entities in order, gather (idx, bytes). ----
    //
    // SoA payload chunk per component: indices array + payload byte
    // array. Records are collected in entity-table order so identical
    // worlds produce identical bytes.
    struct ComponentRecord
    {
        crd::containers::Array<crd::u32> indices;
        crd::containers::Array<crd::u8>  payload_bytes;
    };
    crd::containers::Array<ComponentRecord> records{m_alloc};
    records.resize(persistable.size());
    for (crd::usize i = 0; i < persistable.size(); ++i)
    {
        records[i].indices       = crd::containers::Array<crd::u32>{m_alloc};
        records[i].payload_bytes = crd::containers::Array<crd::u8>{m_alloc};
    }

    for (crd::u32 file_idx = 0; file_idx < file_entities.size(); ++file_idx)
    {
        const EntityId e = file_entities[file_idx];
        for (crd::usize ci = 0; ci < persistable.size(); ++ci)
        {
            PersistableComponent& pc = persistable[ci];
            // Read the component bytes via the appropriate storage backend.
            const void* bytes = nullptr;
            if (pc.info->storage_hint == StorageHint::SparseSet)
            {
                bytes = world.sparse_storage().get_const(e, pc.id);
            }
            else
            {
                bytes = world.storage().get_const(e, pc.id);
            }
            if (bytes == nullptr)
            {
                continue;
            }
            records[ci].indices.push_back(file_idx);
            // Custom write_blob hook? Use it. Else memcpy.
            if (pc.info->serialize.write_blob != nullptr)
            {
                const auto base = records[ci].payload_bytes.size();
                records[ci].payload_bytes.resize(base + pc.info->size);
                pc.info->serialize.write_blob(bytes, &records[ci].payload_bytes[base], pc.info->size);
            }
            else
            {
                append_bytes(records[ci].payload_bytes, bytes, pc.info->size);
            }
            ++pc.record_count;
        }
    }

    // ---- Step 4: Collect relations. -------------------------------------
    //
    // Relations are components in our model — every persistable
    // component with `is_relation == true` is a relation. For each
    // entity in file order, for each relation in registry order, read
    // the target via get_const (the Relation<Tag> bytes ARE just the
    // EntityId target). Emit one record per (src, target, fourcc).
    crd::containers::Array<SceneRelationRecord> relation_records{m_alloc};
    for (crd::u32 file_idx = 0; file_idx < file_entities.size(); ++file_idx)
    {
        const EntityId src = file_entities[file_idx];
        for (const PersistableComponent& pc : persistable)
        {
            if (!pc.info->is_relation)
            {
                continue;
            }
            const void* payload = nullptr;
            if (pc.info->storage_hint == StorageHint::SparseSet)
            {
                payload = world.sparse_storage().get_const(src, pc.id);
            }
            else
            {
                payload = world.storage().get_const(src, pc.id);
            }
            if (payload == nullptr)
            {
                continue;
            }
            const EntityId target = *static_cast<const EntityId*>(payload);
            crd::u32 target_idx = kSceneNullTargetIdx;
            if (!target.is_null())
            {
                if (auto* found = entity_to_idx.find(target.raw); found != nullptr)
                {
                    target_idx = *found;
                }
            }
            SceneRelationRecord rec{};
            rec.src_file_idx    = file_idx;
            rec.target_file_idx = target_idx;
            rec.relation_fourcc = pc.info->serialize.fourcc;
            relation_records.push_back(rec);
        }
    }

    // ---- Step 5: Build the CRDR chunks. --------------------------------
    crd::resources::CrdrWriter writer{m_alloc, m_id, kFourCC_SCEN};

    // INFO chunk.
    {
        SceneInfo info{};
        info.schema_version  = kSceneSchemaVersion;
        info.entity_count    = static_cast<crd::u32>(file_entities.size());
        info.component_count = static_cast<crd::u32>(persistable.size());
        info.relation_count  = static_cast<crd::u32>(relation_records.size());
        writer.add_chunk(kFourCC_SceneINFO,
                         crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&info), sizeof(info)});
    }

    // STRP chunk.
    if (string_pool.size() > 0)
    {
        writer.add_chunk(kFourCC_SceneSTRP,
                         crd::containers::ConstSpan<crd::u8>{string_pool.data(), string_pool.size()});
    }

    // CMPS chunk.
    {
        crd::containers::Array<SceneComponentDescriptor> descriptors{m_alloc};
        descriptors.reserve(persistable.size());
        for (const PersistableComponent& pc : persistable)
        {
            SceneComponentDescriptor d{};
            d.fourcc           = pc.info->serialize.fourcc;
            d.version          = pc.info->serialize.version;
            d.name_strp_offset = pc.name_strp_offset;
            d.size             = static_cast<crd::u32>(pc.info->size);
            d.alignment        = static_cast<crd::u32>(pc.info->alignment);
            d.record_count     = pc.record_count;
            d.storage_hint     = static_cast<crd::u8>(pc.info->storage_hint);
            descriptors.push_back(d);
        }
        const crd::usize n_bytes = descriptors.size() * sizeof(SceneComponentDescriptor);
        writer.add_chunk(kFourCC_SceneCMPS,
                         crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(descriptors.data()),
                                                             n_bytes});
    }

    // ETBL chunk — one u32 per entity (reserved flags).
    {
        crd::containers::Array<crd::u32> etbl{m_alloc};
        etbl.resize(file_entities.size(), 0U);
        const crd::usize n_bytes = etbl.size() * sizeof(crd::u32);
        writer.add_chunk(kFourCC_SceneETBL,
                         crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(etbl.data()),
                                                             n_bytes});
    }

    // C### chunks — one per persistable component, in file_local_id order.
    for (crd::usize ci = 0; ci < persistable.size(); ++ci)
    {
        const PersistableComponent& pc = persistable[ci];
        const ComponentRecord&      rec = records[ci];
        if (rec.indices.size() == 0)
        {
            continue;
        }
        // SoA payload: u32 record_count + u32 reserved + indices[] + (pad) + payloads[]
        crd::containers::Array<crd::u8> chunk_bytes{m_alloc};
        const crd::u32 record_count = static_cast<crd::u32>(rec.indices.size());
        append_u32(chunk_bytes, record_count);
        append_u32(chunk_bytes, 0U); // reserved
        append_bytes(chunk_bytes, rec.indices.data(), rec.indices.size() * sizeof(crd::u32));
        // Align to component alignment before the payload bytes.
        pad_to_align(chunk_bytes, pc.info->alignment > 0 ? pc.info->alignment : 1U);
        append_bytes(chunk_bytes, rec.payload_bytes.data(), rec.payload_bytes.size());

        writer.add_chunk(make_scene_component_chunk_fourcc(pc.file_local_id),
                         crd::containers::ConstSpan<crd::u8>{chunk_bytes.data(), chunk_bytes.size()});
    }

    // RELS chunk.
    if (relation_records.size() > 0)
    {
        const crd::usize n_bytes = relation_records.size() * sizeof(SceneRelationRecord);
        writer.add_chunk(kFourCC_SceneRELS,
                         crd::containers::ConstSpan<crd::u8>{
                             reinterpret_cast<const crd::u8*>(relation_records.data()), n_bytes});
    }

    return writer.finish();
}

// ---- SceneLoader -------------------------------------------------------

void* SceneLoader::load(const crd::resources::LoadContext& ctx)
{
    auto* res = static_cast<SceneResource*>(
        ctx.allocator->allocate(sizeof(SceneResource), alignof(SceneResource)));
    if (res == nullptr)
    {
        return nullptr;
    }
    ::new (res) SceneResource(ctx.allocator);

    // The views must not point into `ctx.bytes`: ResourceManager frees an artifact's bytes as soon as load()
    // returns. Copy them first and parse the copy (as ObekLoader does).
    res->owned_bytes.resize(ctx.bytes.size());
    if (ctx.bytes.size() > 0U)
    {
        std::memcpy(res->owned_bytes.data(), ctx.bytes.data(), ctx.bytes.size());
    }
    if (!parse_scene(*res, crd::containers::as_const_span(res->owned_bytes), ctx.allocator))
    {
        res->~SceneResource();
        ctx.allocator->deallocate(res);
        return nullptr;
    }
    return res;
}

void SceneLoader::unload(void* payload) noexcept
{
    if (payload == nullptr)
    {
        return;
    }
    auto* res = static_cast<SceneResource*>(payload);
    auto* alloc = res->component_payloads.allocator();
    res->~SceneResource();
    alloc->deallocate(res);
}

// ---- World::instantiate_scene ------------------------------------------

SceneInstantiation World::instantiate_scene(const SceneResource& res)
{
    namespace pd = pack_detail;
    SceneInstantiation result{allocator()};

    // Step A: every check before the first spawn; a refused pack spawns nothing.
    if (res.component_descriptors.size() != res.info.component_count ||
        !pd::components_are_consistent(res.info.entity_count, res.component_descriptors,
                                       crd::containers::as_const_span(res.component_payloads)) ||
        !pd::relations_are_consistent(res.info.entity_count, res.relations))
    {
        result.refusal = PackRefusal::MalformedPack;
        return result;
    }
    result.refusal = pd::check_layouts(m_components, res.component_descriptors, false, &result.refused_fourcc);
    if (result.refusal == PackRefusal::None)
    {
        result.refusal =
            pd::check_cycles(m_components, res.info.entity_count, res.relations, allocator(), &result.refused_fourcc);
    }
    if (result.refused())
    {
        return result;
    }

    // Step B: spawn one entity per file_idx.
    result.entities.reserve(res.info.entity_count);
    for (crd::u32 i = 0; i < res.info.entity_count; ++i)
    {
        result.entities.push_back(spawn());
    }

    // Step C: restore components. Relation components are restored from the RELS records in step D: their payload
    // records hold source-World entity ids.
    for (crd::u32 ci = 0; ci < res.info.component_count; ++ci)
    {
        const SceneComponentDescriptor& d = res.component_descriptors[ci];
        const ComponentId registered_id = pd::find_by_fourcc(m_components, d.fourcc);
        if (registered_id.is_null())
        {
            // Unknown FourCC — forward-compat skip (advisor pin #1).
            result.components_skipped += d.record_count;
            continue;
        }
        const ComponentInfo& info = m_components.require_info(registered_id); // found by FourCC: registered
        if (info.is_relation || d.record_count == 0U)
        {
            continue;
        }

        // The storage backends take a mutable pointer and move from it, so each record is staged at the type's
        // alignment: read_blob deserializes into the staging (then the staged object is destroyed after the move),
        // a trivially copyable type is copied into it.
        pd::ComponentStaging staging{*allocator(), info.size, info.alignment};
        const auto& pp = res.component_payloads[ci];
        for (crd::u32 r = 0; r < d.record_count; ++r)
        {
            const EntityId e = result.entities[pp.indices[r]];
            const crd::u8* src = pp.payloads.data() + static_cast<crd::usize>(r) * d.size;
            if (info.serialize.read_blob != nullptr)
            {
                info.serialize.read_blob(staging.data(), src, d.size);
                backend_for(registered_id).insert(e, registered_id, staging.data());
                if (info.destruct != nullptr)
                {
                    info.destruct(staging.data());
                }
            }
            else
            {
                std::memcpy(staging.data(), src, d.size);
                backend_for(registered_id).insert(e, registered_id, staging.data());
            }
        }
    }

    // Step D: restore relations.
    for (const SceneRelationRecord& rec : res.relations)
    {
        const ComponentId registered_id = pd::find_by_fourcc(m_components, rec.relation_fourcc);
        if (registered_id.is_null() || !m_components.require_info(registered_id).is_relation)
        {
            ++result.relations_skipped;
            continue;
        }
        const EntityId src = result.entities[rec.src_file_idx];
        const EntityId tgt =
            rec.target_file_idx == kSceneNullTargetIdx ? EntityId::null() : result.entities[rec.target_file_idx];
        add_relation_via_id(registered_id, src, tgt);
    }

    return result;
}

} // namespace crd::scene
