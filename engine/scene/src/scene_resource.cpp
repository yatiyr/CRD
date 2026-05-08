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
// Forward-compat: unknown FourCCs skip; known FourCC + size/version
// mismatch is a hard fail.

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

// Parse a const span of N-byte structs out of a CRDR chunk's payload.
template <typename T>
[[nodiscard]] crd::containers::ConstSpan<T> chunk_as_array(const crd::resources::CrdrChunk& chunk) noexcept
{
    const auto bytes = chunk.payload;
    const auto count = bytes.size() / sizeof(T);
    return crd::containers::ConstSpan<T>{reinterpret_cast<const T*>(bytes.data()), count};
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
    crd::resources::CrdrFile file{ctx.allocator};
    if (crd::resources::crdr_read(ctx.bytes, file, ctx.allocator) != crd::resources::CrdrError::Ok)
    {
        return nullptr;
    }
    if (file.type_fourcc != kFourCC_SCEN)
    {
        return nullptr;
    }

    auto* res = static_cast<SceneResource*>(
        ctx.allocator->allocate(sizeof(SceneResource), alignof(SceneResource)));
    if (res == nullptr)
    {
        return nullptr;
    }
    ::new (res) SceneResource(ctx.allocator);

    // INFO chunk — required.
    const crd::resources::CrdrChunk* info_chunk = crd::resources::crdr_find_chunk(file, kFourCC_SceneINFO);
    if (info_chunk == nullptr || info_chunk->payload.size() != sizeof(SceneInfo))
    {
        res->~SceneResource();
        ctx.allocator->deallocate(res);
        return nullptr;
    }
    std::memcpy(&res->info, info_chunk->payload.data(), sizeof(SceneInfo));
    if (res->info.schema_version != kSceneSchemaVersion)
    {
        res->~SceneResource();
        ctx.allocator->deallocate(res);
        return nullptr;
    }

    // Optional STRP chunk.
    if (const auto* strp = crd::resources::crdr_find_chunk(file, kFourCC_SceneSTRP); strp != nullptr)
    {
        res->string_pool = strp->payload;
    }

    // CMPS chunk — required if component_count > 0.
    if (res->info.component_count > 0U)
    {
        const auto* cmps = crd::resources::crdr_find_chunk(file, kFourCC_SceneCMPS);
        if (cmps == nullptr || cmps->payload.size() != res->info.component_count * sizeof(SceneComponentDescriptor))
        {
            res->~SceneResource();
            ctx.allocator->deallocate(res);
            return nullptr;
        }
        res->component_descriptors = chunk_as_array<SceneComponentDescriptor>(*cmps);
    }

    // ETBL chunk — required if entity_count > 0.
    if (res->info.entity_count > 0U)
    {
        const auto* etbl = crd::resources::crdr_find_chunk(file, kFourCC_SceneETBL);
        if (etbl == nullptr || etbl->payload.size() != res->info.entity_count * sizeof(crd::u32))
        {
            res->~SceneResource();
            ctx.allocator->deallocate(res);
            return nullptr;
        }
        res->entity_table = chunk_as_array<crd::u32>(*etbl);
    }

    // RELS chunk — optional.
    if (const auto* rels = crd::resources::crdr_find_chunk(file, kFourCC_SceneRELS); rels != nullptr)
    {
        if (rels->payload.size() != res->info.relation_count * sizeof(SceneRelationRecord))
        {
            res->~SceneResource();
            ctx.allocator->deallocate(res);
            return nullptr;
        }
        res->relations = chunk_as_array<SceneRelationRecord>(*rels);
    }

    // Per-component payload chunks (C000, C001, ...).
    res->component_payloads.resize(res->info.component_count);
    for (crd::u32 ci = 0; ci < res->info.component_count; ++ci)
    {
        const SceneComponentDescriptor& d = res->component_descriptors[ci];
        const auto* chunk = crd::resources::crdr_find_chunk(file, make_scene_component_chunk_fourcc(ci));
        if (chunk == nullptr)
        {
            // No payloads for this descriptor → record_count must be 0.
            if (d.record_count != 0U)
            {
                res->~SceneResource();
                ctx.allocator->deallocate(res);
                return nullptr;
            }
            continue;
        }
        const auto bytes = chunk->payload;
        if (bytes.size() < 8U) // u32 record_count + u32 reserved
        {
            res->~SceneResource();
            ctx.allocator->deallocate(res);
            return nullptr;
        }
        crd::u32 record_count = 0;
        std::memcpy(&record_count, bytes.data(), sizeof(crd::u32));
        if (record_count != d.record_count)
        {
            res->~SceneResource();
            ctx.allocator->deallocate(res);
            return nullptr;
        }
        const crd::usize indices_offset = 8U;
        const crd::usize indices_size   = record_count * sizeof(crd::u32);
        const crd::usize payloads_start = align_up(indices_offset + indices_size,
                                                   d.alignment > 0 ? d.alignment : 1U);
        const crd::usize payloads_size  = record_count * d.size;
        if (payloads_start + payloads_size > bytes.size())
        {
            res->~SceneResource();
            ctx.allocator->deallocate(res);
            return nullptr;
        }
        res->component_payloads[ci].indices  = crd::containers::ConstSpan<crd::u32>{
            reinterpret_cast<const crd::u32*>(bytes.data() + indices_offset), record_count};
        res->component_payloads[ci].payloads = crd::containers::ConstSpan<crd::u8>{
            bytes.data() + payloads_start, payloads_size};
    }

    // CRDR chunks borrow `ctx.bytes` directly (no compression in v1k SCEN
    // emit path). The SceneResource holds spans into those bytes; the
    // resource handle keeps the bytes alive. ResourceManager guarantees
    // both stay together.
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
    SceneInstantiation result{allocator()};
    result.entities.reserve(res.info.entity_count);

    // Step A: spawn one entity per file_idx.
    for (crd::u32 i = 0; i < res.info.entity_count; ++i)
    {
        result.entities.push_back(spawn());
    }

    // Step B: build a fourcc → ComponentId map for the registered
    // components in this World. O(N) over the registry — kMaxComponents
    // = 256, fine.
    auto find_component_by_fourcc = [&](crd::u32 fourcc) -> ComponentId
    {
        for (crd::u32 i = 0; i < m_components.size(); ++i)
        {
            const ComponentId cid{static_cast<crd::u16>(i)};
            const ComponentInfo* info = m_components.info(cid);
            if (info != nullptr && info->serialize.fourcc == fourcc)
            {
                return cid;
            }
        }
        return ComponentId{};
    };

    // Step C: restore components.
    for (crd::u32 ci = 0; ci < res.info.component_count; ++ci)
    {
        const SceneComponentDescriptor& d = res.component_descriptors[ci];
        const ComponentId               registered_id = find_component_by_fourcc(d.fourcc);
        if (registered_id.is_null())
        {
            // Unknown FourCC — forward-compat skip (advisor pin #1).
            result.components_skipped += d.record_count;
            continue;
        }
        const ComponentInfo* info = m_components.info(registered_id);
        CRD_ASSERT(info != nullptr);
        // Hard-fail rejects (advisor pin #5).
        CRD_ASSERT(static_cast<crd::u32>(info->size) == d.size &&
                   "instantiate_scene: component size mismatch — registered type changed layout");
        CRD_ASSERT(static_cast<crd::u32>(info->alignment) == d.alignment &&
                   "instantiate_scene: component alignment mismatch");
        CRD_ASSERT(info->serialize.version == d.version &&
                   "instantiate_scene: component version mismatch — explicit migration required");

        const auto& pp = res.component_payloads[ci];
        for (crd::u32 r = 0; r < d.record_count; ++r)
        {
            const crd::u32 file_idx = pp.indices[r];
            CRD_ASSERT(file_idx < result.entities.size());
            const EntityId e = result.entities[file_idx];
            const crd::u8* src = pp.payloads.data() + r * static_cast<crd::usize>(d.size);

            // Use the component's serialize.read_blob hook when present;
            // otherwise the storage backend's insert path will memcpy via
            // move_construct.
            if (info->serialize.read_blob != nullptr)
            {
                // Allocate a temporary stack-aligned buffer, deserialize
                // into it, then insert. Trivially-copyable types skip
                // this — they go through the memcpy fallback below.
                alignas(16) crd::u8 staging[1024];
                CRD_ASSERT(d.size <= sizeof(staging) &&
                           "instantiate_scene: component size exceeds staging buffer (1024 bytes)");
                info->serialize.read_blob(staging, src, d.size);
                backend_for(registered_id).insert(e, registered_id, staging);
                if (info->destruct != nullptr)
                {
                    info->destruct(staging);
                }
            }
            else
            {
                // Trivially-copyable: hand the storage backend the bytes
                // directly. backend.insert's move_construct callback
                // memcpys (info->move_construct is captured for trivially
                // movable types in component_registry's lifecycle hook).
                // Stage into an aligned buffer to give the backend a
                // mutable pointer (its API takes void* because some
                // backends move-from); for trivially-copyable types this
                // is just two memcpys (src→staging→storage).
                alignas(16) crd::u8 staging[1024];
                CRD_ASSERT(d.size <= sizeof(staging) &&
                           "instantiate_scene: component size exceeds staging buffer (1024 bytes)");
                std::memcpy(staging, src, d.size);
                backend_for(registered_id).insert(e, registered_id, staging);
            }
        }
    }

    // Step D: restore relations.
    for (const SceneRelationRecord& rec : res.relations)
    {
        const ComponentId registered_id = find_component_by_fourcc(rec.relation_fourcc);
        if (registered_id.is_null())
        {
            ++result.relations_skipped;
            continue;
        }
        const ComponentInfo* info = m_components.info(registered_id);
        if (info == nullptr || !info->is_relation)
        {
            ++result.relations_skipped;
            continue;
        }
        CRD_ASSERT(rec.src_file_idx < result.entities.size());
        const EntityId src = result.entities[rec.src_file_idx];
        EntityId       tgt = EntityId::null();
        if (rec.target_file_idx != kSceneNullTargetIdx)
        {
            CRD_ASSERT(rec.target_file_idx < result.entities.size());
            tgt = result.entities[rec.target_file_idx];
        }
        add_relation_via_id(registered_id, src, tgt);
    }

    return result;
}

} // namespace crd::scene
