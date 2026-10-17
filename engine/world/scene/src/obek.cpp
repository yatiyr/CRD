// Phase 3.0 v1m — Öbek serialisation: artifact builder, loader, instantiation.
//
// Three closely-coupled pieces in one TU (matches v1k SCEN organisation):
//
//   - ObekArtifactBuilder::build  — walks a World, emits CRDR bytes.
//   - ObekLoader::load            — parses CRDR bytes into an ObekResource.
//   - World::instantiate_obek     — creates entities/components/relations
//                                   from an ObekResource into a target World;
//                                   reparents roots under the supplied parent.
//
// Determinism: every walk is registry-order or entity-table-order; chunk
// order is FourCC-sorted by CrdrWriter. Same World → same bytes.
// Forward-compat: unknown FourCCs skip. The loader refuses a pack whose
// records do not lie inside it; instantiation refuses one this World cannot
// take before it spawns anything (pack_validation.hpp, the SCEN rules).

#include "pack_validation.hpp"

#include <crd/containers/hash_map.hpp>
#include <crd/core/assert.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/world.hpp>

#include <cstring>
#include <type_traits>

namespace crd::scene
{

namespace
{
inline void append_bytes(crd::containers::Array<crd::u8>& dst, const void* src, crd::usize n)
{
    const auto base_size = dst.size();
    dst.resize(base_size + n);
    std::memcpy(&dst[base_size], src, n);
}

inline void append_u32(crd::containers::Array<crd::u8>& dst, crd::u32 v)
{
    append_bytes(dst, &v, sizeof(v));
}

[[nodiscard]] inline crd::usize align_up(crd::usize n, crd::usize a) noexcept
{
    return (n + a - 1U) & ~(a - 1U);
}

inline void pad_to_align(crd::containers::Array<crd::u8>& dst, crd::usize a)
{
    const auto target = align_up(dst.size(), a);
    while (dst.size() < target)
    {
        dst.push_back(0U);
    }
}

[[nodiscard]] crd::u32 strp_intern(crd::containers::Array<crd::u8>& pool, crd::containers::StringView name)
{
    const auto offset = pool.size();
    for (char c : name)
    {
        pool.push_back(static_cast<crd::u8>(c));
    }
    pool.push_back(0U);
    return static_cast<crd::u32>(offset);
}

// Reads a self-pooled chunk (OOVR, OCHN): u32 record_count, u32 pool_size, record_count records of T, then pool_size
// bytes, and nothing after them. The records are copied into typed storage (pack_detail::copy_records), the pool stays
// a view. False when the chunk is not exactly that.
template <typename T>
[[nodiscard]] bool read_pooled_chunk(const crd::resources::CrdrChunk& chunk, crd::containers::Array<T>& records,
                                     crd::containers::ConstSpan<crd::u8>* pool)
{
    static_assert(std::is_trivially_copyable_v<T>, "pack records are copied as bytes");
    constexpr crd::usize header = 2U * sizeof(crd::u32);
    const auto bytes = chunk.payload;
    if (bytes.size() < header)
    {
        return false;
    }
    crd::u32 record_count = 0U;
    crd::u32 pool_size = 0U;
    std::memcpy(&record_count, bytes.data(), sizeof(crd::u32));
    std::memcpy(&pool_size, bytes.data() + sizeof(crd::u32), sizeof(crd::u32));
    crd::usize records_bytes = 0U;
    crd::usize pool_start = 0U;
    crd::usize end = 0U;
    const bool fits = crd::memory::checked_mul(record_count, sizeof(T), &records_bytes) &&
                      crd::memory::checked_add(header, records_bytes, &pool_start) &&
                      crd::memory::checked_add(pool_start, pool_size, &end);
    if (!fits || end != bytes.size())
    {
        return false;
    }
    records.resize(record_count);
    if (records_bytes > 0U)
    {
        std::memcpy(records.data(), bytes.data() + header, records_bytes);
    }
    *pool = crd::containers::ConstSpan<crd::u8>{bytes.data() + pool_start, pool_size};
    return true;
}

// Parses the copy of the artifact in `res.owned_bytes` into `res`'s views. False when the pack is refused: a chunk
// that does not hold exactly its records, or a record that does not lie inside the pack (ObekResource's rules).
[[nodiscard]] bool parse_obek(ObekResource& res, crd::memory::IAllocator* alloc)
{
    namespace pd = pack_detail;
    if (crd::resources::crdr_read(crd::containers::as_const_span(res.owned_bytes), res.parsed_file, alloc) !=
            crd::resources::CrdrError::Ok ||
        res.parsed_file.type_fourcc != kFourCC_OBEK)
    {
        return false;
    }
    const crd::resources::CrdrFile& file = res.parsed_file;

    // OINF chunk — required.
    const crd::resources::CrdrChunk* info_chunk = crd::resources::crdr_find_chunk(file, kFourCC_ObekOINF);
    if (info_chunk == nullptr || info_chunk->payload.size() != sizeof(ObekInfo))
    {
        return false;
    }
    std::memcpy(&res.info, info_chunk->payload.data(), sizeof(ObekInfo));
    if (res.info.schema_version != kObekSchemaVersion || res.info.component_count > kMaxComponents)
    {
        return false;
    }
    res.obek_root_id = res.info.obek_root_id;

    // OSTR chunk — optional.
    if (const auto* strp = crd::resources::crdr_find_chunk(file, kFourCC_ObekSTRP); strp != nullptr)
    {
        res.string_pool = strp->payload;
    }

    // OCMP chunk — required if component_count > 0; every descriptor valid before its payload chunk is read.
    if (res.info.component_count > 0U)
    {
        const auto* cmps = crd::resources::crdr_find_chunk(file, kFourCC_ObekOCMP);
        if (cmps == nullptr || !pd::copy_records(*cmps, res.info.component_count, res.descriptor_records))
        {
            return false;
        }
        for (const ObekComponentDescriptor& d : res.descriptor_records)
        {
            if (!pd::descriptor_is_valid(d))
            {
                return false;
            }
        }
    }
    res.component_descriptors = crd::containers::as_const_span(res.descriptor_records);

    // OETB chunk — required if entity_count > 0; each record names its own position, and its parent hint is null or
    // an entity of the öbek.
    if (res.info.entity_count > 0U)
    {
        const auto* etbl = crd::resources::crdr_find_chunk(file, kFourCC_ObekOETB);
        if (etbl == nullptr || !pd::copy_records(*etbl, res.info.entity_count, res.entity_records))
        {
            return false;
        }
        for (crd::u32 i = 0U; i < res.info.entity_count; ++i)
        {
            const ObekEntityRecord& rec = res.entity_records[i];
            if (rec.file_idx != i ||
                (rec.parent_file_idx != kObekNullParentIdx && rec.parent_file_idx >= res.info.entity_count))
            {
                return false;
            }
        }
    }

    res.entity_table = crd::containers::as_const_span(res.entity_records);

    // ORLS chunk — present exactly when relation_count > 0.
    const auto* rels = crd::resources::crdr_find_chunk(file, kFourCC_ObekORLS);
    if (rels != nullptr ? !pd::copy_records(*rels, res.info.relation_count, res.relation_records)
                        : res.info.relation_count != 0U)
    {
        return false;
    }
    res.relations = crd::containers::as_const_span(res.relation_records);

    // OOVR chunk — optional, present when the cooker baked override patches from `overrides = [...]`; every record
    // names an entity of the öbek and a payload range inside the pool (ADR-0058: invalid patches fail the cook).
    if (const auto* oovr = crd::resources::crdr_find_chunk(file, kFourCC_ObekOOVR); oovr != nullptr)
    {
        if (!read_pooled_chunk(*oovr, res.override_records, &res.cook_override_payload_pool))
        {
            return false;
        }
        res.cook_override_records = crd::containers::as_const_span(res.override_records);
        for (const ObekOverrideRecord& rec : res.cook_override_records)
        {
            if (rec.file_idx >= res.info.entity_count ||
                static_cast<crd::usize>(rec.payload_offset) + rec.payload_size > res.cook_override_payload_pool.size())
            {
                return false;
            }
        }
    }

    // OCHN chunk — optional, present when the cooker recorded chain dependencies (extends parent + nested öbek
    // refs). The path pool is not kept (no reader yet); each entry's path offset must lie inside it, the pool must end
    // in its strings' terminator, and the kind must be known.
    if (const auto* chn = crd::resources::crdr_find_chunk(file, kFourCC_ObekOCHN); chn != nullptr)
    {
        crd::containers::ConstSpan<crd::u8> path_pool{};
        if (!read_pooled_chunk(*chn, res.chain_records, &path_pool))
        {
            return false;
        }
        res.chain_dependencies = crd::containers::as_const_span(res.chain_records);
        if (res.chain_dependencies.size() > 0U && (path_pool.size() == 0U || path_pool[path_pool.size() - 1U] != 0U))
        {
            return false;
        }
        for (const ObekChainEntryRecord& entry : res.chain_dependencies)
        {
            if (entry.path_strp_offset >= path_pool.size() || entry.kind > static_cast<crd::u8>(ObekChainKind::Nested))
            {
                return false;
            }
        }
    }

    // Per-component payload chunks (D000, D001, ...), present exactly when the descriptor has records.
    auto chunk_for = [&file](crd::u32 ci)
    {
        return crd::resources::crdr_find_chunk(file, make_obek_component_chunk_fourcc(ci));
    };
    if (!pd::read_payload_chunks(res.component_descriptors, chunk_for, res.component_payloads, res.index_records))
    {
        return false;
    }

    return pd::components_are_consistent(res.info.entity_count, res.component_descriptors,
                                         crd::containers::as_const_span(res.component_payloads)) &&
           pd::relations_are_consistent(res.info.entity_count, res.relations);
}

// Every instantiation check of an öbek, before its first spawn: its views (MalformedPack), the layouts of the
// components it restores against `registry` (a DontInherit component is never restored, so never compared), then the
// cycles of its Acyclic relations.
[[nodiscard]] PackRefusal check_obek(const ObekResource& res, const ComponentRegistry& registry,
                                     crd::memory::IAllocator* alloc, crd::u32* refused_fourcc)
{
    namespace pd = pack_detail;
    if (res.component_descriptors.size() != res.info.component_count ||
        res.entity_table.size() != res.info.entity_count ||
        !pd::components_are_consistent(res.info.entity_count, res.component_descriptors,
                                       crd::containers::as_const_span(res.component_payloads)) ||
        !pd::relations_are_consistent(res.info.entity_count, res.relations))
    {
        return PackRefusal::MalformedPack;
    }
    const PackRefusal layouts = pd::check_layouts(registry, res.component_descriptors, true, refused_fourcc);
    if (layouts != PackRefusal::None)
    {
        return layouts;
    }
    return pd::check_cycles(registry, res.info.entity_count, res.relations, alloc, refused_fourcc);
}

// FNV-1a 64 mixing — same family as the rest of Cerid's content hashes
// (containers::fnv1a_64, used by SceneCooker's name lookup). Re-implemented
// here over (u64, u32) so make_obek_entity_guid is a pure function not
// depending on the StringView overload.
[[nodiscard]] constexpr crd::u64 mix_fnv1a_64(crd::u64 a, crd::u32 b) noexcept
{
    constexpr crd::u64 prime  = 1099511628211ULL;
    constexpr crd::u64 k_offset = 14695981039346656037ULL;
    crd::u64 h = k_offset;
    for (crd::u32 i = 0; i < 8; ++i)
    {
        h ^= static_cast<crd::u8>((a >> (i * 8U)) & 0xFFU);
        h *= prime;
    }
    for (crd::u32 i = 0; i < 4; ++i)
    {
        h ^= static_cast<crd::u8>((b >> (i * 8U)) & 0xFFU);
        h *= prime;
    }
    return h;
}

} // namespace

// ---- ObekEntityGuid -----------------------------------------------------

ObekEntityGuid make_obek_entity_guid(crd::u64 obek_root_id, crd::u32 file_idx) noexcept
{
    return ObekEntityGuid{mix_fnv1a_64(obek_root_id, file_idx)};
}

// ---- ObekArtifactBuilder ------------------------------------------------

ObekArtifactBuilder::ObekArtifactBuilder(crd::memory::IAllocator* alloc,
                                         crd::resources::ResourceId id,
                                         crd::u64 obek_root_id)
    : m_alloc(alloc), m_id(id), m_obek_root_id(obek_root_id),
      m_pending_deps(alloc), m_pending_overrides(alloc)
{
}

void ObekArtifactBuilder::add_chain_dependency(crd::containers::StringView canonical_path,
                                               crd::u64 content_hash,
                                               ObekChainKind kind)
{
    PendingDep dep{m_alloc};
    dep.path         = crd::containers::String{canonical_path, m_alloc};
    dep.content_hash = content_hash;
    dep.kind         = kind;
    m_pending_deps.push_back(std::move(dep));
}

void ObekArtifactBuilder::add_override(crd::u32 file_idx,
                                       crd::u32 component_fourcc,
                                       crd::u32 field_offset,
                                       crd::containers::ConstSpan<crd::u8> payload)
{
    PendingOverride ov{m_alloc};
    ov.file_idx         = file_idx;
    ov.component_fourcc = component_fourcc;
    ov.field_offset     = field_offset;
    ov.payload.resize(payload.size());
    if (payload.size() > 0)
    {
        std::memcpy(ov.payload.data(), payload.data(), payload.size());
    }
    m_pending_overrides.push_back(std::move(ov));
}

crd::containers::Array<crd::u8> ObekArtifactBuilder::build(const World& world)
{
    // ---- Step 1: Collect alive entities in deterministic SlotMap order. --
    crd::containers::Array<EntityId>             file_entities{m_alloc};
    crd::containers::HashMap<crd::u64, crd::u32> entity_to_idx{m_alloc};
    for (EntityId e : world)
    {
        const crd::u32 idx = static_cast<crd::u32>(file_entities.size());
        entity_to_idx.emplace(e.raw, idx);
        file_entities.push_back(e);
    }

    // ---- Step 2: Collect persistable components (those with serialize trait).
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
        if (info->size == 0U)
        {
            continue;
        }
        PersistableComponent pc{};
        pc.id              = cid;
        pc.info            = info;
        pc.file_local_id   = static_cast<crd::u32>(persistable.size());
        pc.name_strp_offset = strp_intern(string_pool, info->name);
        pc.record_count    = 0;
        persistable.push_back(pc);
    }

    // ---- Step 3: Per-component, walk file_entities, gather (idx, bytes). --
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
    crd::containers::Array<ObekRelationRecord> relation_records{m_alloc};
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
            crd::u32 target_idx = kObekNullTargetIdx;
            if (!target.is_null())
            {
                if (auto* found = entity_to_idx.find(target.raw); found != nullptr)
                {
                    target_idx = *found;
                }
            }
            ObekRelationRecord rec{};
            rec.src_file_idx    = file_idx;
            rec.target_file_idx = target_idx;
            rec.relation_fourcc = pc.info->serialize.fourcc;
            relation_records.push_back(rec);
        }
    }

    // ---- Step 4b: Build per-entity records. ----------------------------
    //
    // v1m1: name_strp_offset records the empty string at a stable pool
    // offset (subsequent intern of same string returns same offset).
    // parent_file_idx scans relation_records for ChildOf to populate the
    // hint (authoritative parent stays in ORLS; this is short-circuit
    // fodder for v1m3 nested-root detection).
    crd::u32 entity_name_offset = strp_intern(string_pool, crd::containers::StringView{""});
    crd::containers::Array<ObekEntityRecord> entity_records{m_alloc};
    entity_records.reserve(file_entities.size());
    for (crd::u32 file_idx = 0; file_idx < file_entities.size(); ++file_idx)
    {
        ObekEntityRecord rec{};
        rec.file_idx         = file_idx;
        rec.name_strp_offset = entity_name_offset;
        rec.parent_file_idx  = kObekNullParentIdx;
        rec.flags            = 0U;
        entity_records.push_back(rec);
    }
    // Populate parent_file_idx from any ChildOf relation present.
    for (const ObekRelationRecord& rel : relation_records)
    {
        if (rel.relation_fourcc == kFourCC_RelChildOf
            && rel.target_file_idx != kObekNullTargetIdx
            && rel.src_file_idx < entity_records.size())
        {
            entity_records[rel.src_file_idx].parent_file_idx = rel.target_file_idx;
        }
    }

    // ---- Step 5: Build the CRDR chunks. --------------------------------
    crd::resources::CrdrWriter writer{m_alloc, m_id, kFourCC_OBEK};

    // OINF chunk.
    {
        ObekInfo info{};
        info.schema_version  = kObekSchemaVersion;
        info.entity_count    = static_cast<crd::u32>(file_entities.size());
        info.component_count = static_cast<crd::u32>(persistable.size());
        info.relation_count  = static_cast<crd::u32>(relation_records.size());
        info.obek_root_id    = m_obek_root_id;
        writer.add_chunk(kFourCC_ObekOINF,
                         crd::containers::ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&info), sizeof(info)});
    }

    // OSTR chunk (string pool).
    if (string_pool.size() > 0)
    {
        writer.add_chunk(kFourCC_ObekSTRP,
                         crd::containers::ConstSpan<crd::u8>{string_pool.data(), string_pool.size()});
    }

    // OCMP chunk.
    {
        crd::containers::Array<ObekComponentDescriptor> descriptors{m_alloc};
        descriptors.reserve(persistable.size());
        for (const PersistableComponent& pc : persistable)
        {
            ObekComponentDescriptor d{};
            d.fourcc           = pc.info->serialize.fourcc;
            d.version          = pc.info->serialize.version;
            d.name_strp_offset = pc.name_strp_offset;
            d.size             = static_cast<crd::u32>(pc.info->size);
            d.alignment        = static_cast<crd::u32>(pc.info->alignment);
            d.record_count     = pc.record_count;
            d.storage_hint     = static_cast<crd::u8>(pc.info->storage_hint);
            descriptors.push_back(d);
        }
        const crd::usize n_bytes = descriptors.size() * sizeof(ObekComponentDescriptor);
        writer.add_chunk(kFourCC_ObekOCMP,
                         crd::containers::ConstSpan<crd::u8>{
                             reinterpret_cast<const crd::u8*>(descriptors.data()), n_bytes});
    }

    // OETB chunk.
    {
        const crd::usize n_bytes = entity_records.size() * sizeof(ObekEntityRecord);
        writer.add_chunk(kFourCC_ObekOETB,
                         crd::containers::ConstSpan<crd::u8>{
                             reinterpret_cast<const crd::u8*>(entity_records.data()), n_bytes});
    }

    // D### chunks — one per persistable component, in file_local_id order.
    for (crd::usize ci = 0; ci < persistable.size(); ++ci)
    {
        const PersistableComponent& pc  = persistable[ci];
        const ComponentRecord&      rec = records[ci];
        if (rec.indices.size() == 0)
        {
            continue;
        }
        crd::containers::Array<crd::u8> chunk_bytes{m_alloc};
        const crd::u32 record_count = static_cast<crd::u32>(rec.indices.size());
        append_u32(chunk_bytes, record_count);
        append_u32(chunk_bytes, 0U);
        append_bytes(chunk_bytes, rec.indices.data(), rec.indices.size() * sizeof(crd::u32));
        pad_to_align(chunk_bytes, pc.info->alignment > 0 ? pc.info->alignment : 1U);
        append_bytes(chunk_bytes, rec.payload_bytes.data(), rec.payload_bytes.size());

        writer.add_chunk(make_obek_component_chunk_fourcc(pc.file_local_id),
                         crd::containers::ConstSpan<crd::u8>{chunk_bytes.data(), chunk_bytes.size()});
    }

    // ORLS chunk.
    if (relation_records.size() > 0)
    {
        const crd::usize n_bytes = relation_records.size() * sizeof(ObekRelationRecord);
        writer.add_chunk(kFourCC_ObekORLS,
                         crd::containers::ConstSpan<crd::u8>{
                             reinterpret_cast<const crd::u8*>(relation_records.data()), n_bytes});
    }

    // OCHN chunk — extends + nested öbek dependency list.
    //
    // Path strings are interned into a dedicated chain string pool inside
    // the OCHN chunk payload itself (NOT shared with OSTR). Layout:
    //   u32 dep_count
    //   u32 path_pool_byte_size
    //   ObekChainEntryRecord[dep_count]   // path_strp_offset is into the
    //                                     // path pool that follows.
    //   u8 path_pool[path_pool_byte_size]
    //
    // Self-contained pool keeps OCHN parseable without reading OSTR first.
    if (m_pending_deps.size() > 0)
    {
        crd::containers::Array<ObekChainEntryRecord> entry_records{m_alloc};
        crd::containers::Array<crd::u8>              path_pool{m_alloc};
        entry_records.reserve(m_pending_deps.size());

        for (const PendingDep& dep : m_pending_deps)
        {
            ObekChainEntryRecord rec{};
            const auto path_view = crd::containers::StringView{dep.path};
            rec.path_strp_offset = strp_intern(path_pool, path_view);
            rec.content_hash     = dep.content_hash;
            rec.kind             = static_cast<crd::u8>(dep.kind);
            entry_records.push_back(rec);
        }

        crd::containers::Array<crd::u8> chunk_bytes{m_alloc};
        const crd::u32 dep_count = static_cast<crd::u32>(entry_records.size());
        const crd::u32 pool_size = static_cast<crd::u32>(path_pool.size());
        append_u32(chunk_bytes, dep_count);
        append_u32(chunk_bytes, pool_size);
        append_bytes(chunk_bytes, entry_records.data(),
                     entry_records.size() * sizeof(ObekChainEntryRecord));
        append_bytes(chunk_bytes, path_pool.data(), path_pool.size());

        writer.add_chunk(kFourCC_ObekOCHN,
                         crd::containers::ConstSpan<crd::u8>{chunk_bytes.data(), chunk_bytes.size()});
    }

    // OOVR chunk — cook-time override patches.
    if (m_pending_overrides.size() > 0)
    {
        crd::containers::Array<ObekOverrideRecord> ov_records{m_alloc};
        crd::containers::Array<crd::u8>            payload_pool{m_alloc};
        ov_records.reserve(m_pending_overrides.size());

        for (const PendingOverride& pov : m_pending_overrides)
        {
            ObekOverrideRecord rec{};
            rec.file_idx         = pov.file_idx;
            rec.component_fourcc = pov.component_fourcc;
            rec.field_offset     = pov.field_offset;
            rec.payload_offset   = static_cast<crd::u32>(payload_pool.size());
            rec.payload_size     = static_cast<crd::u32>(pov.payload.size());
            for (crd::u8 b : pov.payload)
            {
                payload_pool.push_back(b);
            }
            ov_records.push_back(rec);
        }

        crd::containers::Array<crd::u8> chunk_bytes{m_alloc};
        const crd::u32 record_count = static_cast<crd::u32>(ov_records.size());
        const crd::u32 pool_size    = static_cast<crd::u32>(payload_pool.size());
        append_u32(chunk_bytes, record_count);
        append_u32(chunk_bytes, pool_size);
        append_bytes(chunk_bytes, ov_records.data(),
                     ov_records.size() * sizeof(ObekOverrideRecord));
        append_bytes(chunk_bytes, payload_pool.data(), payload_pool.size());

        writer.add_chunk(kFourCC_ObekOOVR,
                         crd::containers::ConstSpan<crd::u8>{chunk_bytes.data(), chunk_bytes.size()});
    }

    return writer.finish();
}

// ---- ObekLoader ---------------------------------------------------------

void* ObekLoader::load(const crd::resources::LoadContext& ctx)
{
    auto* res = static_cast<ObekResource*>(
        ctx.allocator->allocate(sizeof(ObekResource), alignof(ObekResource)));
    if (res == nullptr)
    {
        return nullptr;
    }
    ::new (res) ObekResource(ctx.allocator);

    // Copy the source bytes into the resource so our parsed chunk
    // payloads (which are non-owning ConstSpan views) live as long as
    // the resource itself. ctx.bytes points into a transient buffer
    // owned by ResourceManager that is freed as soon as load() returns.
    res->owned_bytes.resize(ctx.bytes.size());
    if (ctx.bytes.size() > 0U)
    {
        std::memcpy(res->owned_bytes.data(), ctx.bytes.data(), ctx.bytes.size());
    }

    if (!parse_obek(*res, ctx.allocator))
    {
        res->~ObekResource();
        ctx.allocator->deallocate(res);
        return nullptr;
    }
    return res;
}

void ObekLoader::unload(void* payload) noexcept
{
    if (payload == nullptr)
    {
        return;
    }
    auto* res    = static_cast<ObekResource*>(payload);
    auto* alloc  = res->component_payloads.allocator();
    res->~ObekResource();
    alloc->deallocate(res);
}

// ---- World::instantiate_obek -------------------------------------------

ObekInstantiation World::instantiate_obek(const ObekResource& res,
                                          EntityId parent,
                                          crd::containers::ConstSpan<ObekOverride> overrides)
{
    ObekInstantiation result{allocator()};
    result.parent = parent;
    result.obek_root_id = res.obek_root_id;
    result.refusal = check_obek(res, m_components, allocator(), &result.refused_fourcc);
    if (result.refused())
    {
        return result; // nothing spawned, `source` left null
    }
    return instantiate_checked_obek(res, parent, overrides);
}

ObekInstantiation World::instantiate_checked_obek(const ObekResource& res, EntityId parent,
                                                  crd::containers::ConstSpan<ObekOverride> overrides)
{
    namespace pd = pack_detail;
    ObekInstantiation result{allocator()};
    result.entities.reserve(res.info.entity_count);
    result.parent        = parent;
    result.obek_root_id  = res.obek_root_id;
    result.source        = &res;  // v1m5a: link for revert/enumerate/unpack

    // Step A: spawn one entity per file_idx.
    for (crd::u32 i = 0; i < res.info.entity_count; ++i)
    {
        result.entities.push_back(spawn());
    }

    // Step C: restore components. Relation components are restored from the ORLS records in step D: their payload
    // records hold source-World entity ids.
    for (crd::u32 ci = 0; ci < res.info.component_count; ++ci)
    {
        const ObekComponentDescriptor& d = res.component_descriptors[ci];
        const ComponentId registered_id = pd::find_by_fourcc(m_components, d.fourcc);
        if (registered_id.is_null())
        {
            result.components_skipped += d.record_count;
            continue;
        }
        const ComponentInfo& info = m_components.require_info(registered_id); // a registered id, found above

        // ADR-0058 pillar 5: DontInherit components are skipped entirely.
        // The entity is still spawned (in step A) but never receives this
        // component — typical for runtime-only state (NetworkId, LoadState,
        // EditorSelectionFlag) authored on the source but undesired on
        // cooked-instance restore.
        if (info.inherit_policy == InheritPolicy::DontInherit)
        {
            result.components_skipped += d.record_count;
            continue;
        }
        if (info.is_relation || d.record_count == 0U)
        {
            continue;
        }

        // Staged at the type's alignment (the backends move from a mutable pointer); see instantiate_scene.
        pd::ComponentStaging staging{*allocator(), info.size, info.alignment};
        const auto& pp = res.component_payloads[ci];
        for (crd::u32 r = 0; r < d.record_count; ++r)
        {
            const EntityId e = result.entities[pp.indices[r]];
            const crd::u8* src = pp.payloads.data() + static_cast<crd::usize>(r) * d.size;
            if (info.serialize.read_blob != nullptr)
            {
                info.serialize.read_blob(staging.data(), src, d.size);
            }
            else
            {
                std::memcpy(staging.data(), src, d.size);
            }
            // v1m4b2: Inherit components share via the SparseSetStorage
            // shared-pool path (force-SparseSet at registration ensures
            // the backend is SparseSet here).
            if (info.inherit_policy == InheritPolicy::Inherit)
            {
                m_sparse_storage.insert_shared(e, registered_id, staging.data());
            }
            else
            {
                backend_for(registered_id).insert(e, registered_id, staging.data());
            }
            if (info.serialize.read_blob != nullptr && info.destruct != nullptr)
            {
                info.destruct(staging.data());
            }
        }
    }

    // Step D: restore relations + remember which entities have ChildOf
    // (so we can skip reparenting them under `parent`).
    crd::containers::Array<crd::u8> has_childof_in_obek{allocator()};
    has_childof_in_obek.resize(res.info.entity_count, 0U);

    for (const ObekRelationRecord& rec : res.relations)
    {
        const ComponentId registered_id = pd::find_by_fourcc(m_components, rec.relation_fourcc);
        if (registered_id.is_null() || !m_components.require_info(registered_id).is_relation)
        {
            ++result.relations_skipped;
            continue;
        }
        const EntityId src = result.entities[rec.src_file_idx];
        const EntityId tgt =
            rec.target_file_idx == kObekNullTargetIdx ? EntityId::null() : result.entities[rec.target_file_idx];
        add_relation_via_id(registered_id, src, tgt);

        if (rec.relation_fourcc == kFourCC_RelChildOf)
        {
            has_childof_in_obek[rec.src_file_idx] = 1U;
        }
    }

    // A patch writes component bytes; a relation's are its target, which only add_relation may change (its reverse
    // index follows), so a patch naming a relation component is skipped like an unknown one.
    auto patchable = [&](crd::u32 fourcc) -> ComponentId
    {
        const ComponentId id = pd::find_by_fourcc(m_components, fourcc);
        return (id.is_null() || m_components.require_info(id).is_relation) ? ComponentId{} : id;
    };

    // Step D2: apply COOK-TIME override patches from OOVR chunk first
    // (ADR-0058 pillar 3 cook-time half). Caller-supplied patches in
    // step E run AFTER and therefore win on overlap — caller is deepest.
    // ObekLoader refuses a record whose entity or payload range lies outside
    // the öbek; the checks below hold a hand-built resource to the same.
    for (const ObekOverrideRecord& rec : res.cook_override_records)
    {
        if (rec.file_idx >= res.info.entity_count)
        {
            ++result.overrides_skipped;
            continue;
        }
        const ComponentId target_cid = patchable(rec.component_fourcc);
        if (target_cid.is_null())
        {
            ++result.overrides_skipped;
            continue;
        }
        const ComponentInfo& info = m_components.require_info(target_cid);
        const crd::usize end = static_cast<crd::usize>(rec.field_offset) + rec.payload_size;
        if (end > info.size ||
            static_cast<crd::usize>(rec.payload_offset) + rec.payload_size > res.cook_override_payload_pool.size())
        {
            ++result.overrides_skipped;
            continue;
        }
        const EntityId e = result.entities[rec.file_idx];
        void* dst = backend_for(target_cid).get_mut(e, target_cid);
        if (dst == nullptr)
        {
            ++result.overrides_skipped;
            continue;
        }
        std::memcpy(static_cast<crd::u8*>(dst) + rec.field_offset,
                    res.cook_override_payload_pool.data() + rec.payload_offset,
                    rec.payload_size);
        ++result.overrides_applied;
    }

    // Step E: apply override patches (ADR-0058 pillar 3).
    //
    // Each patch resolves to (entity, component) → memcpy `payload` bytes
    // at `field_offset`. file_idx wins; symbolic-name fallback consulted
    // when file_idx == kObekOverrideUseName. Out-of-range or unknown
    // patches are silently skipped (counted in overrides_skipped).
    //
    // Symbolic-name lookup: walk OETB; for each record, compare the OSTR
    // bytes at name_strp_offset against the patch's name. Linear O(N)
    // search — acceptable for v1m2 (typical öbek N << 1000); v1m3+ may
    // build a HashMap if the cooker emits a sorted name index.
    auto resolve_entity = [&](const ObekOverride& patch) -> crd::u32
    {
        if (patch.file_idx < res.info.entity_count)
        {
            return patch.file_idx;
        }
        if (patch.file_idx == kObekOverrideUseName && !patch.name.empty())
        {
            for (crd::u32 i = 0; i < res.info.entity_count; ++i)
            {
                const crd::u32 off = res.entity_table[i].name_strp_offset;
                if (off >= res.string_pool.size())
                {
                    continue;
                }
                const auto* base = reinterpret_cast<const char*>(res.string_pool.data() + off);
                const crd::usize remaining = res.string_pool.size() - off;
                crd::usize len = 0;
                while (len < remaining && base[len] != '\0')
                {
                    ++len;
                }
                if (len == patch.name.size()
                    && std::memcmp(base, patch.name.data(), len) == 0)
                {
                    return i;
                }
            }
        }
        return res.info.entity_count;  // sentinel for "not found"
    };

    for (const ObekOverride& patch : overrides)
    {
        const crd::u32 file_idx = resolve_entity(patch);
        if (file_idx >= res.info.entity_count)
        {
            ++result.overrides_skipped;
            continue;
        }
        const ComponentId target_cid = patchable(patch.component_fourcc);
        if (target_cid.is_null())
        {
            ++result.overrides_skipped;
            continue;
        }
        const ComponentInfo& info = m_components.require_info(target_cid);
        // Bounds-check: field_offset + payload.size() must fit inside the
        // component layout. Out-of-range is a programming error in
        // calling code; debug assert in debug, count + skip in release.
        const crd::usize end = static_cast<crd::usize>(patch.field_offset) + patch.payload.size();
        if (end > info.size)
        {
            CRD_ASSERT_UNREACHABLE("ObekOverride: field_offset + payload.size() exceeds component size");
            ++result.overrides_skipped;
            continue;
        }
        const EntityId e = result.entities[file_idx];
        void* dst_component = backend_for(target_cid).get_mut(e, target_cid);
        if (dst_component == nullptr)
        {
            ++result.overrides_skipped;
            continue;
        }
        std::memcpy(static_cast<crd::u8*>(dst_component) + patch.field_offset,
                    patch.payload.data(),
                    patch.payload.size());
        ++result.overrides_applied;
    }

    // Step F: reparent öbek roots under the supplied parent (ADR-0058 sub-instance).
    //
    // A "root" is any entity with no ChildOf relation in the source öbek.
    // We install Relation<ChildOf>(root → parent) for each root, but only
    // when ChildOf is actually registered in the target World. If it isn't,
    // skip silently (matches the relation-skip semantics for forward-compat).
    if (!parent.is_null() && is_alive(parent))
    {
        const ComponentId childof_id = pd::find_by_fourcc(m_components, kFourCC_RelChildOf);
        if (!childof_id.is_null())
        {
            for (crd::u32 i = 0; i < res.info.entity_count; ++i)
            {
                if (has_childof_in_obek[i] == 0U)
                {
                    add_relation_via_id(childof_id, result.entities[i], parent);
                }
            }
        }
    }

    return result;
}

// ---- v1m5b — batch instantiation (ADR-0058 pillar 15a) ----------------

ObekBatchHandle World::instantiate_obek_batch(const ObekResource& res, crd::u32 count,
                                              EntityId parent, BatchHints hints,
                                              crd::containers::ConstSpan<Transform> transforms)
{
    (void)hints; // reserved for Phase 3.5+ renderer integration

    // One check for every slot; a refused öbek spawns nothing and gets the null handle.
    crd::u32 refused_fourcc = 0U;
    if (check_obek(res, m_components, allocator(), &refused_fourcc) != PackRefusal::None)
    {
        return ObekBatchHandle{};
    }

    ObekBatchHandle batch{};
    batch.value = m_next_obek_batch;
    m_next_obek_batch = (m_next_obek_batch == 0xFFFFFFFFU) ? 1U : m_next_obek_batch + 1U;

    // Look up BatchInstanceTag's ComponentId — present only if the user
    // registered it on this World. v1m5b's tag-add path is opt-in; absent
    // registration = no tagging (entities still spawn correctly).
    const ComponentId tag_cid = pack_detail::find_by_fourcc(m_components, kFourCC_BatchInstanceTag);

    for (crd::u32 slot = 0; slot < count; ++slot)
    {
        ObekInstantiation inst = instantiate_checked_obek(res, parent, {});
        if (!tag_cid.is_null())
        {
            BatchInstanceTag tag{};
            tag.batch = batch;
            tag.slot  = slot;
            for (EntityId e : inst.entities)
            {
                backend_for(tag_cid).insert(e, tag_cid, &tag);
            }
        }
        // GEO-7: place the slot — UPSERT the slot's local TRS onto this instance's ROOT entities (source-roots:
        // ChildOf target is `parent`, or none). add_component fires on_update through the storage event sink, so
        // a configured SpatialBVHIndex refreshes; the dirty mark queues one propagation pass for the whole batch.
        if (slot < transforms.size())
        {
            for (EntityId e : inst.entities)
            {
                const EntityId p = get_relation_target<relations::ChildOf>(e);
                const bool     is_root = p.is_null() || p == parent;
                if (!is_root)
                {
                    continue;
                }
                add_component(e, transforms[static_cast<crd::usize>(slot)]);
                mark_transform_subtree_dirty(e);
            }
        }
    }
    return batch;
}

// ---- v1m5a — revert / unpack / enumerate APIs (ADR-0058 pillar 7) ------

namespace
{
// Find the per-component-payloads index (ci) in res for a given fourcc.
// Returns res.info.component_count if not found.
[[nodiscard]] crd::u32 find_payload_ci(const ObekResource& res, crd::u32 fourcc) noexcept
{
    for (crd::u32 ci = 0; ci < res.info.component_count; ++ci)
    {
        if (res.component_descriptors[ci].fourcc == fourcc)
        {
            return ci;
        }
    }
    return res.info.component_count;
}

// Find the record_idx within payloads[ci] whose indices array entry == file_idx.
// Returns d.record_count if not found.
[[nodiscard]] crd::u32 find_payload_record(const ObekComponentDescriptor& d,
                                            const ObekResource::PerComponentPayload& pp,
                                            crd::u32 file_idx) noexcept
{
    for (crd::u32 r = 0; r < d.record_count; ++r)
    {
        if (pp.indices[r] == file_idx)
        {
            return r;
        }
    }
    return d.record_count;
}
} // namespace

void World::revert_field(ObekInstantiation& inst, crd::u32 file_idx,
                         crd::u32 component_fourcc, crd::u32 field_offset, crd::u32 field_size)
{
    if (inst.source == nullptr)
    {
        return;
    }
    if (file_idx >= inst.entities.size())
    {
        return;
    }
    const ObekResource& res = *inst.source;

    const crd::u32 ci = find_payload_ci(res, component_fourcc);
    if (ci >= res.info.component_count)
    {
        return;
    }

    const ObekComponentDescriptor& d = res.component_descriptors[ci];
    const auto& pp = res.component_payloads[ci];
    const crd::u32 r = find_payload_record(d, pp, file_idx);
    if (r >= d.record_count)
    {
        return;
    }

    if (static_cast<crd::usize>(field_offset) + field_size > d.size)
    {
        return;
    }

    // The registered component this record restored: not a relation (only add_relation may change a target) and not
    // a DontInherit component (its bytes never came from the öbek).
    const ComponentId target_cid = pack_detail::restored_component(m_components, component_fourcc, true);
    if (target_cid.is_null() ||
        static_cast<crd::usize>(field_offset) + field_size > m_components.require_info(target_cid).size)
    {
        return;
    }

    const EntityId e = inst.entities[file_idx];
    void* dst = backend_for(target_cid).get_mut(e, target_cid);
    if (dst == nullptr)
    {
        return;
    }

    // 1. Restore source bytes for the field range.
    const crd::u8* src_bytes = pp.payloads.data() + static_cast<crd::usize>(r) * d.size;
    std::memcpy(static_cast<crd::u8*>(dst) + field_offset, src_bytes + field_offset, field_size);

    // 2. Re-apply any cook-time overrides that overlap this field range (in usize: a record's u32 offset plus its
    //    size can pass 2^32).
    const crd::usize fld_start = field_offset;
    const crd::usize fld_end = fld_start + field_size;
    for (const ObekOverrideRecord& rec : res.cook_override_records)
    {
        if (rec.file_idx != file_idx || rec.component_fourcc != component_fourcc)
        {
            continue;
        }
        const crd::usize ov_start = rec.field_offset;
        const crd::usize ov_end = ov_start + rec.payload_size;
        if (ov_start < fld_end && ov_end > fld_start)
        {
            const crd::usize effective_start = (ov_start > fld_start) ? ov_start : fld_start;
            const crd::usize effective_end = (ov_end < fld_end) ? ov_end : fld_end;
            const crd::usize ov_payload_offset =
                static_cast<crd::usize>(rec.payload_offset) + (effective_start - ov_start);
            const crd::usize ov_payload_size = effective_end - effective_start;
            if (ov_payload_offset + ov_payload_size > res.cook_override_payload_pool.size())
            {
                continue;
            }
            std::memcpy(static_cast<crd::u8*>(dst) + effective_start,
                        res.cook_override_payload_pool.data() + ov_payload_offset, ov_payload_size);
        }
    }
}

void World::revert_component(ObekInstantiation& inst, crd::u32 file_idx, crd::u32 component_fourcc)
{
    if (inst.source == nullptr)
    {
        return;
    }
    const ObekResource& res = *inst.source;
    const crd::u32 ci = find_payload_ci(res, component_fourcc);
    if (ci >= res.info.component_count)
    {
        return;
    }
    revert_field(inst, file_idx, component_fourcc, 0U, res.component_descriptors[ci].size);
}

void World::revert_entity(ObekInstantiation& inst, crd::u32 file_idx)
{
    if (inst.source == nullptr)
    {
        return;
    }
    const ObekResource& res = *inst.source;
    for (crd::u32 ci = 0; ci < res.info.component_count; ++ci)
    {
        const ObekComponentDescriptor& d = res.component_descriptors[ci];
        revert_field(inst, file_idx, d.fourcc, 0U, d.size);
    }
}

void World::revert_all(ObekInstantiation& inst)
{
    if (inst.source == nullptr)
    {
        return;
    }
    for (crd::u32 i = 0; i < inst.entities.size(); ++i)
    {
        revert_entity(inst, i);
    }
}

void World::unpack_obek(ObekInstantiation& inst)
{
    revert_all(inst);
    inst.source = nullptr;
}

void World::unpack_obek_keep_overrides(ObekInstantiation& inst)
{
    inst.source = nullptr;
}

crd::containers::ConstSpan<ObekOverrideRecord>
World::enumerate_overrides(const ObekInstantiation& inst) const noexcept
{
    if (inst.source == nullptr)
    {
        return crd::containers::ConstSpan<ObekOverrideRecord>{};
    }
    return inst.source->cook_override_records;
}

} // namespace crd::scene
