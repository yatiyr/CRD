// Scene and obek packs at the trust boundary (ADR-0055 §5, ADR-0058; engine/world/scene/src/pack_validation.hpp).
//
// SceneLoader and ObekLoader publish only a pack whose records lie inside it, and World::instantiate_scene /
// instantiate_obek refuse, before the first spawn, a pack whose views contradict themselves or whose records the World
// cannot take. Every refusal case starts from an honest pack (which loads), changes one field, and holds the loader or
// the instantiation to its refusal; each case is the tooth of one rule. The rest pin the defects this file was written
// for: relation payload records restored as stale ids (a missing reverse-index edge), a loaded scene viewing bytes the
// resource manager frees, records staged in a 1024-byte buffer at 16-byte alignment, overrides and reverts writing a
// relation's target, and a shared pool's capacity doubling through u32.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/scene_resource.hpp>
#include <crd/scene/serialize.hpp>
#include <crd/scene/shared_component_pool.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

namespace
{
namespace sc = crd::scene;
namespace res = crd::resources;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::memory::TlsfAllocator;

constexpr crd::usize kHeapBytes = 32U << 20U;

constexpr crd::u32 kProbeFourCC = sc::make_serialize_fourcc('P', 'R', 'B', 'E');
constexpr crd::u32 kUnusedFourCC = sc::make_serialize_fourcc('U', 'N', 'S', 'D');
constexpr crd::u32 kWideFourCC = sc::make_serialize_fourcc('W', 'I', 'D', 'E');

struct Probe
{
    crd::u32 a;
    crd::u32 b;
};

struct Unused
{
    crd::u32 v;
};

// The same FourCC as Probe, with another layout in the target World.
struct ProbeLonger
{
    crd::u32 a;
    crd::u32 b;
    crd::u32 c;
};

struct alignas(8) ProbeAligned
{
    crd::u32 a;
    crd::u32 b;
};

// Wider than the in-place staging (1024 bytes) and more aligned than it (16 bytes).
struct alignas(64) Wide
{
    crd::u8 bytes[1536];
};

// read_blob sees where instantiation staged the record.
bool g_wide_staging_aligned = true;
void wide_read_blob(void* dst, const void* bytes, crd::usize size)
{
    g_wide_staging_aligned = g_wide_staging_aligned && (reinterpret_cast<std::uintptr_t>(dst) % alignof(Wide)) == 0U;
    std::memcpy(dst, bytes, size);
}
void wide_write_blob(const void* src, void* bytes_out, crd::usize cap)
{
    std::memcpy(bytes_out, src, cap);
}

[[nodiscard]] sc::ComponentSerialize wide_trait()
{
    sc::ComponentSerialize cs = sc::default_serialize_trait<Wide>(kWideFourCC);
    cs.read_blob = &wide_read_blob;
    cs.write_blob = &wide_write_blob;
    return cs;
}

void setup_world(sc::World& w)
{
    w.register_component<Probe>(sc::default_serialize_trait<Probe>(kProbeFourCC));
    w.register_component<Unused>(sc::default_serialize_trait<Unused>(kUnusedFourCC));
    w.register_builtin_relations();
}

// A parent (file 0) with `children` children (files 1..), child i carrying Probe{7 + i, 9 + i} and ChildOf(child,
// parent).
void build_family(sc::World& w, crd::u32 children)
{
    const sc::EntityId parent = w.spawn();
    for (crd::u32 i = 0U; i < children; ++i)
    {
        const sc::EntityId child = w.spawn();
        w.add_component<Probe>(child, Probe{7U + i, 9U + i});
        w.add_relation<sc::relations::ChildOf>(child, parent);
    }
}

enum class Format : crd::u8
{
    Scene,
    Obek,
};

[[nodiscard]] const char* name_of(Format f)
{
    return f == Format::Scene ? "SCEN" : "OBEK";
}

// The chunk FourCCs and table records of each format (the descriptor, payload and relation layouts are shared).
struct FormatChunks
{
    crd::u32 info;
    crd::u32 cmps;
    crd::u32 etbl;
    crd::usize etbl_record;
    crd::u32 rels;
};

[[nodiscard]] FormatChunks chunks_of(Format f)
{
    if (f == Format::Scene)
    {
        return FormatChunks{sc::kFourCC_SceneINFO, sc::kFourCC_SceneCMPS, sc::kFourCC_SceneETBL, sizeof(crd::u32),
                            sc::kFourCC_SceneRELS};
    }
    return FormatChunks{sc::kFourCC_ObekOINF, sc::kFourCC_ObekOCMP, sc::kFourCC_ObekOETB, sizeof(sc::ObekEntityRecord),
                        sc::kFourCC_ObekORLS};
}

[[nodiscard]] crd::u32 payload_fourcc(Format f, crd::u32 ci)
{
    return f == Format::Scene ? sc::make_scene_component_chunk_fourcc(ci) : sc::make_obek_component_chunk_fourcc(ci);
}

[[nodiscard]] Array<crd::u8> build_pack(Format f, const sc::World& w, crd::memory::IAllocator* alloc)
{
    if (f == Format::Scene)
    {
        sc::SceneArtifactBuilder builder{alloc, res::ResourceId{0x5CE0ULL, 0x7A57ULL}};
        return builder.build(w);
    }
    sc::ObekArtifactBuilder builder{alloc, res::ResourceId{0x0BE0ULL, 0x7A57ULL}, 0xABCDULL};
    return builder.build(w);
}

[[nodiscard]] res::LoadContext context_for(const Array<crd::u8>& bytes, crd::memory::IAllocator* alloc)
{
    res::LoadContext ctx{};
    ctx.id = res::ResourceId{0x5CE0ULL, 0x7A57ULL};
    ctx.bytes = ConstSpan<crd::u8>{bytes.data(), bytes.size()};
    ctx.allocator = alloc;
    return ctx;
}

// A loaded resource, unloaded when the holder goes (so a failed REQUIRE leaks nothing into the test's heap).
template <typename Resource, typename Loader> class Loaded
{
public:
    Loaded(const Array<crd::u8>& bytes, crd::memory::IAllocator* alloc)
        : m_res(static_cast<Resource*>(m_loader.load(context_for(bytes, alloc))))
    {
    }
    ~Loaded()
    {
        if (m_res != nullptr)
        {
            m_loader.unload(m_res);
        }
    }
    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    Loaded(Loaded&&) = delete;
    Loaded& operator=(Loaded&&) = delete;

    [[nodiscard]] const Resource* get() const noexcept { return m_res; }

private:
    Loader m_loader;
    Resource* m_res;
};

using LoadedScene = Loaded<sc::SceneResource, sc::SceneLoader>;
using LoadedObek = Loaded<sc::ObekResource, sc::ObekLoader>;

[[nodiscard]] bool loads(Format f, const Array<crd::u8>& bytes, crd::memory::IAllocator* alloc)
{
    if (f == Format::Scene)
    {
        const LoadedScene scene{bytes, alloc};
        return scene.get() != nullptr;
    }
    const LoadedObek obek{bytes, alloc};
    return obek.get() != nullptr;
}

// A pack's chunks as editable byte arrays, written back through CrdrWriter (so the container stays valid and only the
// chunk under test changes).
class EditablePack
{
public:
    EditablePack(const Array<crd::u8>& bytes, crd::memory::IAllocator* alloc) : m_alloc(alloc), m_chunks(alloc)
    {
        res::CrdrFile file{alloc};
        REQUIRE(res::crdr_read(crd::containers::as_const_span(bytes), file, alloc) == res::CrdrError::Ok);
        m_id = file.id;
        m_type = file.type_fourcc;
        for (const res::CrdrChunk& c : file.chunks)
        {
            add(c.fourcc, c.payload);
        }
    }

    [[nodiscard]] Array<crd::u8>& chunk(crd::u32 fourcc)
    {
        Chunk* found = nullptr;
        for (Chunk& c : m_chunks)
        {
            if (c.fourcc == fourcc)
            {
                found = &c;
            }
        }
        REQUIRE(found != nullptr);
        return found->bytes;
    }

    void add(crd::u32 fourcc, ConstSpan<crd::u8> bytes)
    {
        Chunk c{m_alloc};
        c.fourcc = fourcc;
        for (const crd::u8 b : bytes)
        {
            c.bytes.push_back(b);
        }
        m_chunks.push_back(std::move(c));
    }

    void remove(crd::u32 fourcc)
    {
        Array<Chunk> kept{m_alloc};
        for (Chunk& c : m_chunks)
        {
            if (c.fourcc != fourcc)
            {
                kept.push_back(std::move(c));
            }
        }
        m_chunks = std::move(kept);
    }

    // Writes `value` at byte `offset` of the chunk tagged `tag`.
    template <typename T> void set(crd::u32 tag, crd::usize offset, T value)
    {
        Array<crd::u8>& b = chunk(tag);
        REQUIRE(offset + sizeof(T) <= b.size());
        std::memcpy(b.data() + offset, &value, sizeof(T));
    }

    // The index of the descriptor for `fourcc` in the format's descriptor table.
    [[nodiscard]] crd::u32 descriptor_index(Format f, crd::u32 fourcc)
    {
        const Array<crd::u8>& cmps = chunk(chunks_of(f).cmps);
        const crd::usize count = cmps.size() / sizeof(sc::SceneComponentDescriptor);
        crd::usize found = count;
        for (crd::usize i = 0U; i < count; ++i)
        {
            crd::u32 candidate = 0U;
            std::memcpy(&candidate, cmps.data() + i * sizeof(sc::SceneComponentDescriptor), sizeof(candidate));
            if (candidate == fourcc)
            {
                found = i;
            }
        }
        REQUIRE(found < count);
        return static_cast<crd::u32>(found);
    }

    // Where field `field` of the descriptor for `fourcc` sits in the descriptor table.
    [[nodiscard]] crd::usize descriptor_field(Format f, crd::u32 fourcc, crd::usize field)
    {
        return descriptor_index(f, fourcc) * sizeof(sc::SceneComponentDescriptor) + field;
    }

    [[nodiscard]] Array<crd::u8> write(ConstSpan<crd::u32> compressed = {}) const
    {
        res::CrdrWriter writer{m_alloc, m_id, m_type};
        for (const Chunk& c : m_chunks)
        {
            bool compress = false;
            for (const crd::u32 fourcc : compressed)
            {
                compress = compress || fourcc == c.fourcc;
            }
            if (compress)
            {
                writer.add_chunk_compressed(c.fourcc, crd::containers::as_const_span(c.bytes));
            }
            else
            {
                writer.add_chunk(c.fourcc, crd::containers::as_const_span(c.bytes));
            }
        }
        return writer.finish();
    }

private:
    struct Chunk
    {
        crd::u32 fourcc = 0U;
        Array<crd::u8> bytes;

        explicit Chunk(crd::memory::IAllocator* a) : bytes(a) {}
    };

    crd::memory::IAllocator* m_alloc;
    res::ResourceId m_id{};
    crd::u32 m_type = 0U;
    Array<Chunk> m_chunks;
};

constexpr crd::usize kDescFourCC = offsetof(sc::SceneComponentDescriptor, fourcc);
constexpr crd::usize kDescSize = offsetof(sc::SceneComponentDescriptor, size);
constexpr crd::usize kDescAlign = offsetof(sc::SceneComponentDescriptor, alignment);
constexpr crd::usize kDescStorage = offsetof(sc::SceneComponentDescriptor, storage_hint);
constexpr crd::usize kRelSrc = offsetof(sc::SceneRelationRecord, src_file_idx);
constexpr crd::usize kRelTarget = offsetof(sc::SceneRelationRecord, target_file_idx);
constexpr crd::usize kRelFourCC = offsetof(sc::SceneRelationRecord, relation_fourcc);
constexpr crd::usize kInfoComponents = offsetof(sc::SceneInfo, component_count);
constexpr crd::usize kInfoRelations = offsetof(sc::SceneInfo, relation_count);
static_assert(offsetof(sc::ObekInfo, component_count) == kInfoComponents, "INFO and OINF share their counts' offsets");
static_assert(offsetof(sc::ObekInfo, relation_count) == kInfoRelations, "INFO and OINF share their counts' offsets");
static_assert(sizeof(sc::ObekComponentDescriptor) == sizeof(sc::SceneComponentDescriptor), "shared descriptor layout");
static_assert(sizeof(sc::ObekRelationRecord) == sizeof(sc::SceneRelationRecord), "shared relation layout");

// The Probe payload chunk of the family pack.
[[nodiscard]] crd::u32 probe_chunk(EditablePack& pack, Format f)
{
    return payload_fourcc(f, pack.descriptor_index(f, kProbeFourCC));
}

// The descriptor table's field `field` of the descriptor for `fourcc`, set to `value`.
template <typename T> void set_descriptor(EditablePack& pack, Format f, crd::u32 fourcc, crd::usize field, T value)
{
    pack.set<T>(chunks_of(f).cmps, pack.descriptor_field(f, fourcc, field), value);
}

// One corruption of an honest family pack (a parent and two children): the rule it breaks, and the edit.
struct Corruption
{
    const char* rule;
    void (*apply)(EditablePack& pack, Format f);
};

// Valid, distinct filler descriptors bring the descriptor table to `total` entries.
void grow_descriptor_table(EditablePack& pack, Format f, crd::u32 total)
{
    const FormatChunks c = chunks_of(f);
    Array<crd::u8>& cmps = pack.chunk(c.cmps);
    crd::u32 count = static_cast<crd::u32>(cmps.size() / sizeof(sc::SceneComponentDescriptor));
    for (crd::u32 filler = 0U; count < total; ++count, ++filler)
    {
        sc::SceneComponentDescriptor d{};
        d.fourcc =
            sc::make_serialize_fourcc('F', static_cast<char>('A' + filler / 26U % 26U),
                                      static_cast<char>('A' + filler % 26U), static_cast<char>('0' + filler / 676U));
        d.version = 1U;
        d.size = 4U;
        d.alignment = 4U;
        const auto* bytes = reinterpret_cast<const crd::u8*>(&d);
        for (crd::usize i = 0U; i < sizeof(d); ++i)
        {
            cmps.push_back(bytes[i]);
        }
    }
    pack.set<crd::u32>(c.info, kInfoComponents, total);
}

constexpr crd::usize kSecondRelation = sizeof(sc::SceneRelationRecord);

const Corruption kCorruptions[] = {
    {"at most kMaxComponents descriptors",
     [](EditablePack& p, Format f)
     {
         grow_descriptor_table(p, f, sc::kMaxComponents + 1U);
     }},
    {"a descriptor's FourCC is not 0",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescFourCC, 0U);
     }},
    {"descriptor FourCCs are distinct",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescFourCC, kProbeFourCC);
     }},
    {"a descriptor's size is not 0",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescSize, 0U);
     }},
    {"a descriptor's alignment is not 0",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescAlign, 0U);
     }},
    {"a descriptor's alignment is a power of two",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescSize, 12U);
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescAlign, 6U);
     }},
    {"a descriptor's size is a multiple of its alignment",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescSize, 12U);
         set_descriptor<crd::u32>(p, f, kUnusedFourCC, kDescAlign, 8U);
     }},
    {"a descriptor's storage hint is known",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u8>(p, f, kUnusedFourCC, kDescStorage, 2U);
     }},
    {"a payload chunk's record count is its descriptor's",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(probe_chunk(p, f), 0U, 1U);
     }},
    {"a payload chunk ends with its records (one byte short)",
     [](EditablePack& p, Format f)
     {
         Array<crd::u8>& b = p.chunk(probe_chunk(p, f));
         b.resize(b.size() - 1U);
     }},
    {"a payload chunk ends with its records (one byte past)",
     [](EditablePack& p, Format f)
     {
         p.chunk(probe_chunk(p, f)).push_back(0U);
     }},
    {"a descriptor with records has its payload chunk",
     [](EditablePack& p, Format f)
     {
         p.remove(probe_chunk(p, f));
     }},
    {"a payload's byte size is computed without wrapping (2 records of 2^31 bytes)",
     [](EditablePack& p, Format f)
     {
         set_descriptor<crd::u32>(p, f, kProbeFourCC, kDescSize, 0x80000000U);
     }},
    {"a record's entity index is below entity_count",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(probe_chunk(p, f), 12U, 3U);
     }},
    {"a payload's entity indices ascend (swapped)",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(probe_chunk(p, f), 8U, 2U);
         p.set<crd::u32>(probe_chunk(p, f), 12U, 1U);
     }},
    {"a payload names each entity once",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(probe_chunk(p, f), 12U, 1U);
     }},
    {"the entity table is present when entity_count > 0",
     [](EditablePack& p, Format f)
     {
         p.remove(chunks_of(f).etbl);
     }},
    {"the entity table holds entity_count records",
     [](EditablePack& p, Format f)
     {
         Array<crd::u8>& b = p.chunk(chunks_of(f).etbl);
         b.resize(b.size() - chunks_of(f).etbl_record);
     }},
    {"the relation table is present when relation_count > 0",
     [](EditablePack& p, Format f)
     {
         p.remove(chunks_of(f).rels);
     }},
    {"the relation table holds relation_count records",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(chunks_of(f).info, kInfoRelations, 3U);
     }},
    {"a relation's source is below entity_count",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(chunks_of(f).rels, kSecondRelation + kRelSrc, 3U);
     }},
    {"a relation's target is null or below entity_count",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(chunks_of(f).rels, kRelTarget, 3U);
     }},
    {"a relation's FourCC is not 0",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(chunks_of(f).rels, kRelFourCC, 0U);
     }},
    {"relations ascend by source",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(chunks_of(f).rels, kRelSrc, 2U);
         p.set<crd::u32>(chunks_of(f).rels, kSecondRelation + kRelSrc, 1U);
     }},
    {"a source names each relation once",
     [](EditablePack& p, Format f)
     {
         p.set<crd::u32>(chunks_of(f).rels, kSecondRelation + kRelSrc, 1U);
     }},
};

// A file under the temporary directory, removed when the holder goes.
struct TempFile
{
    crd::platform::fs::Path path;

    explicit TempFile(crd::platform::fs::Path p) : path(std::move(p)) {}
    ~TempFile() { (void)crd::platform::fs::remove_file(path); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;
};

[[nodiscard]] Array<crd::u8> family_pack(Format f, crd::memory::IAllocator* alloc)
{
    sc::World source{alloc};
    setup_world(source);
    build_family(source, 2U);
    return build_pack(f, source, alloc);
}
} // namespace

TEST_CASE("pack trust: every corruption of a scene or obek pack is refused by its loader",
          "[scene][pack-trust][loader]")
{
    TlsfAllocator alloc{kHeapBytes};
    for (const Format f : {Format::Scene, Format::Obek})
    {
        const Array<crd::u8> honest = family_pack(f, &alloc);
        REQUIRE(loads(f, honest, &alloc));
        {
            const EditablePack untouched{honest, &alloc};
            CHECK(loads(f, untouched.write(), &alloc)); // the edit round trip itself changes nothing
        }
        for (const Corruption& c : kCorruptions)
        {
            INFO(name_of(f) << ": " << c.rule);
            EditablePack pack{honest, &alloc};
            c.apply(pack, f);
            CHECK_FALSE(loads(f, pack.write(), &alloc));
        }
    }
}

TEST_CASE("pack trust: a full descriptor table of kMaxComponents entries loads", "[scene][pack-trust][loader]")
{
    TlsfAllocator alloc{kHeapBytes};
    for (const Format f : {Format::Scene, Format::Obek})
    {
        INFO(name_of(f));
        EditablePack pack{family_pack(f, &alloc), &alloc};
        grow_descriptor_table(pack, f, sc::kMaxComponents);
        CHECK(loads(f, pack.write(), &alloc));
    }
}

TEST_CASE("pack trust: a compressed table decoded at a misaligned offset loads into typed records",
          "[scene][pack-trust][loader]")
{
    TlsfAllocator alloc{kHeapBytes};
    for (const Format f : {Format::Scene, Format::Obek})
    {
        INFO(name_of(f));
        EditablePack pack{family_pack(f, &alloc), &alloc};
        // A compressible chunk of 1002 bytes the loaders do not know sorts first and decodes first, so the descriptor
        // table decodes 2 bytes past a 4-byte boundary of the backing.
        const crd::u32 filler = res::make_fourcc('p', 'a', 'd', static_cast<char>(1));
        Array<crd::u8> pad{&alloc};
        pad.resize(1002U, static_cast<crd::u8>('a'));
        pack.add(filler, crd::containers::as_const_span(pad));
        const crd::u32 compressed[] = {filler, chunks_of(f).cmps};
        const Array<crd::u8> bytes = pack.write(ConstSpan<crd::u32>{compressed, 2U});

        res::CrdrFile file{&alloc};
        REQUIRE(res::crdr_read(crd::containers::as_const_span(bytes), file, &alloc) == res::CrdrError::Ok);
        const res::CrdrChunk* cmps = res::crdr_find_chunk(file, chunks_of(f).cmps);
        REQUIRE(cmps != nullptr);
        REQUIRE((cmps->flags & 1U) != 0U); // stored compressed
        REQUIRE(reinterpret_cast<std::uintptr_t>(cmps->payload.data()) % alignof(sc::SceneComponentDescriptor) != 0U);

        sc::World target{&alloc};
        setup_world(target);
        if (f == Format::Scene)
        {
            const LoadedScene scene{bytes, &alloc};
            REQUIRE(scene.get() != nullptr);
            const sc::SceneInstantiation inst = target.instantiate_scene(*scene.get());
            REQUIRE_FALSE(inst.refused());
            REQUIRE(inst.entities.size() == 3U);
            const Probe* const probe = target.get_component<Probe>(inst.entities[2]);
            REQUIRE(probe != nullptr);
            CHECK(probe->a == 8U);
        }
        else
        {
            const LoadedObek obek{bytes, &alloc};
            REQUIRE(obek.get() != nullptr);
            const sc::ObekInstantiation inst = target.instantiate_obek(*obek.get());
            REQUIRE_FALSE(inst.refused());
            REQUIRE(inst.entities.size() == 3U);
            const Probe* const probe = target.get_component<Probe>(inst.entities[2]);
            REQUIRE(probe != nullptr);
            CHECK(probe->a == 8U);
        }
    }
}

namespace
{
// An obek family with one cook-time override (Probe of file 1) and one chain dependency.
[[nodiscard]] Array<crd::u8> obek_with_extras(crd::memory::IAllocator* alloc)
{
    sc::World source{alloc};
    setup_world(source);
    build_family(source, 2U);
    sc::ObekArtifactBuilder builder{alloc, res::ResourceId{0x0BE0ULL, 0x7A57ULL}, 0xABCDULL};
    const Probe patched{70U, 90U};
    builder.add_override(1U, kProbeFourCC, 0U,
                         ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&patched), sizeof(patched)});
    builder.add_chain_dependency(crd::containers::StringView{"obek/base.obek.toml"}, 0x1234ULL,
                                 sc::ObekChainKind::Extends);
    return builder.build(source);
}

constexpr crd::usize kPoolHeader = 2U * sizeof(crd::u32);
constexpr crd::usize kOverrideFileIdx = kPoolHeader + offsetof(sc::ObekOverrideRecord, file_idx);
constexpr crd::usize kOverrideOffset = kPoolHeader + offsetof(sc::ObekOverrideRecord, payload_offset);
constexpr crd::usize kOverrideSize = kPoolHeader + offsetof(sc::ObekOverrideRecord, payload_size);
constexpr crd::usize kChainOffset = kPoolHeader + offsetof(sc::ObekChainEntryRecord, path_strp_offset);
constexpr crd::usize kChainKind = kPoolHeader + offsetof(sc::ObekChainEntryRecord, kind);
constexpr crd::usize kEntityFileIdx = offsetof(sc::ObekEntityRecord, file_idx);
constexpr crd::usize kEntityParent = offsetof(sc::ObekEntityRecord, parent_file_idx);

struct ObekCorruption
{
    const char* rule;
    void (*apply)(EditablePack& pack);
};

const ObekCorruption kObekCorruptions[] = {
    {"an entity record names its own position",
     [](EditablePack& p)
     {
         p.set<crd::u32>(sc::kFourCC_ObekOETB, sizeof(sc::ObekEntityRecord) + kEntityFileIdx, 2U);
     }},
    {"an entity record's parent hint is null or an entity of the obek",
     [](EditablePack& p)
     {
         p.set<crd::u32>(sc::kFourCC_ObekOETB, kEntityParent, 3U);
     }},
    {"a cook-time override names an entity of the obek",
     [](EditablePack& p)
     {
         p.set<crd::u32>(sc::kFourCC_ObekOOVR, kOverrideFileIdx, 3U);
     }},
    {"a cook-time override's payload range lies in its pool, computed without wrapping",
     [](EditablePack& p)
     {
         p.set<crd::u32>(sc::kFourCC_ObekOOVR, kOverrideOffset, 0xFFFFFFFCU);
         p.set<crd::u32>(sc::kFourCC_ObekOOVR, kOverrideSize, 8U);
     }},
    {"the override chunk ends with its pool",
     [](EditablePack& p)
     {
         p.chunk(sc::kFourCC_ObekOOVR).push_back(0U);
     }},
    {"a chain entry's path lies in its pool",
     [](EditablePack& p)
     {
         p.set<crd::u32>(sc::kFourCC_ObekOCHN, kChainOffset, 0x1000U);
     }},
    {"a chain entry's kind is known",
     [](EditablePack& p)
     {
         p.set<crd::u8>(sc::kFourCC_ObekOCHN, kChainKind, 2U);
     }},
    {"the chain pool ends in a terminator",
     [](EditablePack& p)
     {
         Array<crd::u8>& b = p.chunk(sc::kFourCC_ObekOCHN);
         b[b.size() - 1U] = static_cast<crd::u8>('x');
     }},
};
} // namespace

TEST_CASE("pack trust: every corruption of an obek's entity, override and chain tables is refused",
          "[scene][pack-trust][loader]")
{
    TlsfAllocator alloc{kHeapBytes};
    const Array<crd::u8> honest = obek_with_extras(&alloc);
    REQUIRE(loads(Format::Obek, honest, &alloc));
    for (const ObekCorruption& c : kObekCorruptions)
    {
        INFO(c.rule);
        EditablePack pack{honest, &alloc};
        c.apply(pack);
        CHECK_FALSE(loads(Format::Obek, pack.write(), &alloc));
    }
}

TEST_CASE("pack trust: a loaded scene does not view the bytes it was loaded from", "[scene][pack-trust][lifetime]")
{
    TlsfAllocator alloc{kHeapBytes};
    Array<crd::u8> bytes = family_pack(Format::Scene, &alloc);
    const LoadedScene scene{bytes, &alloc};
    REQUIRE(scene.get() != nullptr);
    // The resource manager frees an artifact's bytes as soon as load() returns; overwrite them as a stand-in.
    for (crd::u8& byte : bytes)
    {
        byte = 0xFFU;
    }
    sc::World target{&alloc};
    setup_world(target);
    const sc::SceneInstantiation inst = target.instantiate_scene(*scene.get());
    REQUIRE(inst.entities.size() == 3U);
    const Probe* const probe = target.get_component<Probe>(inst.entities[1]);
    REQUIRE(probe != nullptr);
    CHECK(probe->a == 7U);
    CHECK(probe->b == 9U);
    CHECK(target.get_relation_target<sc::relations::ChildOf>(inst.entities[2]) == inst.entities[0]);
}

TEST_CASE("pack trust: a scene loaded through the resource manager instantiates after its artifact is freed",
          "[scene][pack-trust][lifetime]")
{
    TlsfAllocator alloc{kHeapBytes};
    const Array<crd::u8> scene_bytes = family_pack(Format::Scene, &alloc);
    const res::ResourceId scene_id = res::ResourceId::mint_random();
    Array<crd::u8> names{&alloc};
    for (const char c : crd::containers::StringView{"pack-trust.scen"})
    {
        names.push_back(static_cast<crd::u8>(c));
    }
    names.push_back(0U);
    Array<res::ManifestEntry> entries{&alloc};
    res::ManifestEntry entry;
    entry.id = scene_id;
    entry.type_fourcc = sc::kFourCC_SCEN;
    entry.blob_size = scene_bytes.size();
    entries.push_back(entry);
    const res::ResourceId pack_id = res::ResourceId::mint_random();
    {
        res::CrdrWriter measure{&alloc, pack_id, res::kFourCC_PACK};
        res::manifest_write(measure, crd::containers::as_const_span(entries), crd::containers::as_const_span(names));
        entries[0].blob_offset = measure.finish().size();
    }
    res::CrdrWriter writer{&alloc, pack_id, res::kFourCC_PACK};
    res::manifest_write(writer, crd::containers::as_const_span(entries), crd::containers::as_const_span(names));
    Array<crd::u8> pack = writer.finish();
    for (const crd::u8 b : scene_bytes)
    {
        pack.push_back(b);
    }
    const crd::containers::String stem = pack_id.to_string(&alloc);
    crd::containers::String file_name{stem.data(), stem.size(), &alloc};
    file_name.append(".crdr");
    const TempFile file{crd::platform::fs::temp_directory() /
                        crd::containers::StringView{file_name.data(), file_name.size()}};
    REQUIRE(crd::platform::fs::write_file_binary(file.path, crd::containers::as_const_span(pack)));

    res::ResourceManager manager{&alloc};
    manager.register_loader(std::make_unique<sc::SceneLoader>());
    REQUIRE(manager.mount_manifest(file.path.generic()).is_valid());
    const res::ResourceHandle<sc::SceneResource> handle = manager.load_sync<sc::SceneResource>(scene_id);
    const sc::SceneResource* const scene = handle.get();
    REQUIRE(scene != nullptr);
    sc::World target{&alloc};
    setup_world(target);
    const sc::SceneInstantiation inst = target.instantiate_scene(*scene);
    REQUIRE(inst.entities.size() == 3U);
    const Probe* const probe = target.get_component<Probe>(inst.entities[2]);
    REQUIRE(probe != nullptr);
    CHECK(probe->a == 8U);
    CHECK(probe->b == 10U);
}

TEST_CASE("pack trust: instantiating into a fresh world keeps every relation's reverse index",
          "[scene][pack-trust][relations]")
{
    // A fresh World issues the source's entity ids again, so a relation payload record restored as raw bytes would
    // name the new target already, and the relation's re-add would skip its reverse-index edge as a no-op.
    TlsfAllocator alloc{kHeapBytes};
    for (const Format f : {Format::Scene, Format::Obek})
    {
        INFO(name_of(f));
        const Array<crd::u8> bytes = family_pack(f, &alloc);
        sc::World target{&alloc};
        setup_world(target);
        Array<sc::EntityId> spawned{&alloc};
        if (f == Format::Scene)
        {
            const LoadedScene scene{bytes, &alloc};
            REQUIRE(scene.get() != nullptr);
            const sc::SceneInstantiation inst = target.instantiate_scene(*scene.get());
            for (const sc::EntityId e : inst.entities)
            {
                spawned.push_back(e);
            }
        }
        else
        {
            const LoadedObek obek{bytes, &alloc};
            REQUIRE(obek.get() != nullptr);
            const sc::ObekInstantiation inst = target.instantiate_obek(*obek.get());
            for (const sc::EntityId e : inst.entities)
            {
                spawned.push_back(e);
            }
        }
        REQUIRE(spawned.size() == 3U);
        CHECK(target.get_relation_target<sc::relations::ChildOf>(spawned[1]) == spawned[0]);
        CHECK(target.get_relation_target<sc::relations::ChildOf>(spawned[2]) == spawned[0]);
        target.destroy_immediate(spawned[0]); // ChildOf cascades through its reverse index
        CHECK_FALSE(target.is_alive(spawned[1]));
        CHECK_FALSE(target.is_alive(spawned[2]));
    }
}

namespace
{
// Loads `bytes` and instantiates them into `target`, returning the refusal; a refused instantiation is held to
// spawning nothing.
[[nodiscard]] sc::PackRefusal instantiate_refusal(Format f, const Array<crd::u8>& bytes, sc::World& target,
                                                  crd::memory::IAllocator* alloc, crd::u32* refused_fourcc)
{
    const crd::u32 before = target.entity_count();
    if (f == Format::Scene)
    {
        const LoadedScene scene{bytes, alloc};
        REQUIRE(scene.get() != nullptr);
        const sc::SceneInstantiation inst = target.instantiate_scene(*scene.get());
        *refused_fourcc = inst.refused_fourcc;
        if (inst.refused())
        {
            CHECK(inst.entities.size() == 0U);
            CHECK(target.entity_count() == before);
        }
        return inst.refusal;
    }
    const LoadedObek obek{bytes, alloc};
    REQUIRE(obek.get() != nullptr);
    const sc::ObekInstantiation inst = target.instantiate_obek(*obek.get());
    *refused_fourcc = inst.refused_fourcc;
    if (inst.refused())
    {
        CHECK(inst.entities.size() == 0U);
        CHECK(inst.source == nullptr);
        CHECK(target.entity_count() == before);
    }
    return inst.refusal;
}

// The family pack with its two relation records rewired to the relation FourCC `relation`: source 1 to `first_target`
// and source 2 to `second_target`.
[[nodiscard]] Array<crd::u8> rewired_family(Format f, crd::memory::IAllocator* alloc, crd::u32 relation,
                                            crd::u32 first_target, crd::u32 second_target)
{
    EditablePack pack{family_pack(f, alloc), alloc};
    const crd::u32 table = chunks_of(f).rels;
    pack.set<crd::u32>(table, kRelSrc, 1U);
    pack.set<crd::u32>(table, kRelTarget, first_target);
    pack.set<crd::u32>(table, kRelFourCC, relation);
    pack.set<crd::u32>(table, kSecondRelation + kRelSrc, 2U);
    pack.set<crd::u32>(table, kSecondRelation + kRelTarget, second_target);
    pack.set<crd::u32>(table, kSecondRelation + kRelFourCC, relation);
    return pack.write();
}
} // namespace

TEST_CASE("pack trust: a pack this world cannot take is refused before the first spawn",
          "[scene][pack-trust][instantiate]")
{
    TlsfAllocator alloc{kHeapBytes};
    for (const Format f : {Format::Scene, Format::Obek})
    {
        INFO(name_of(f));
        const Array<crd::u8> honest = family_pack(f, &alloc);
        crd::u32 refused = 0U;
        {
            sc::World target{&alloc};
            setup_world(target);
            CHECK(instantiate_refusal(f, honest, target, &alloc, &refused) == sc::PackRefusal::None);
            CHECK(target.entity_count() == 3U);
        }
        {
            sc::World target{&alloc};
            target.register_component<ProbeLonger>(sc::default_serialize_trait<ProbeLonger>(kProbeFourCC));
            target.register_builtin_relations();
            CHECK(instantiate_refusal(f, honest, target, &alloc, &refused) == sc::PackRefusal::ComponentSizeMismatch);
            CHECK(refused == kProbeFourCC);
        }
        {
            sc::World target{&alloc};
            target.register_component<ProbeAligned>(sc::default_serialize_trait<ProbeAligned>(kProbeFourCC));
            target.register_builtin_relations();
            CHECK(instantiate_refusal(f, honest, target, &alloc, &refused) ==
                  sc::PackRefusal::ComponentAlignmentMismatch);
            CHECK(refused == kProbeFourCC);
        }
        {
            sc::World target{&alloc};
            target.register_component<Probe>(sc::default_serialize_trait<Probe>(kProbeFourCC, 2U));
            target.register_builtin_relations();
            CHECK(instantiate_refusal(f, honest, target, &alloc, &refused) ==
                  sc::PackRefusal::ComponentVersionMismatch);
            CHECK(refused == kProbeFourCC);
        }
        {
            // ChildOf 1 -> 2 and 2 -> 1: a cycle in an Acyclic relation.
            const Array<crd::u8> cycle = rewired_family(f, &alloc, sc::kFourCC_RelChildOf, 2U, 1U);
            REQUIRE(loads(f, cycle, &alloc)); // structurally sound: only the World knows ChildOf is Acyclic
            sc::World target{&alloc};
            setup_world(target);
            CHECK(instantiate_refusal(f, cycle, target, &alloc, &refused) == sc::PackRefusal::RelationCycle);
            CHECK(refused == sc::kFourCC_RelChildOf);
        }
        {
            // ChildOf 1 -> 1: a self-relation.
            const Array<crd::u8> self = rewired_family(f, &alloc, sc::kFourCC_RelChildOf, 1U, 0U);
            sc::World target{&alloc};
            setup_world(target);
            CHECK(instantiate_refusal(f, self, target, &alloc, &refused) == sc::PackRefusal::RelationCycle);
        }
        {
            // Targets 1 -> 2 and 2 -> 1: Targets is not Acyclic, so the same shape is restored.
            const Array<crd::u8> mutual = rewired_family(f, &alloc, sc::kFourCC_RelTargets, 2U, 1U);
            sc::World target{&alloc};
            setup_world(target);
            CHECK(instantiate_refusal(f, mutual, target, &alloc, &refused) == sc::PackRefusal::None);
            CHECK(target.entity_count() == 3U);
        }
    }
}

TEST_CASE("pack trust: a DontInherit component an obek never restores does not refuse it",
          "[scene][pack-trust][instantiate]")
{
    TlsfAllocator alloc{kHeapBytes};
    const Array<crd::u8> honest = family_pack(Format::Obek, &alloc);
    sc::World target{&alloc};
    target.register_component<ProbeLonger>(sc::default_serialize_trait<ProbeLonger>(kProbeFourCC),
                                           sc::InheritPolicy::DontInherit);
    target.register_builtin_relations();
    const LoadedObek obek{honest, &alloc};
    REQUIRE(obek.get() != nullptr);
    const sc::ObekInstantiation inst = target.instantiate_obek(*obek.get());
    CHECK_FALSE(inst.refused());
    REQUIRE(inst.entities.size() == 3U);
    CHECK(inst.components_skipped == 2U);
    CHECK_FALSE(target.has_component<ProbeLonger>(inst.entities[1]));
}

namespace
{
// A hand-built scene resource over copies of a loaded one's views, for the views a loader never publishes.
struct HandBuilt
{
    sc::SceneResource resource;
    Array<sc::SceneComponentDescriptor> descriptors;
    Array<crd::u32> indices;
    Array<sc::SceneRelationRecord> relations;

    HandBuilt(const sc::SceneResource& loaded, crd::memory::IAllocator* alloc)
        : resource(alloc), descriptors(alloc), indices(alloc), relations(alloc)
    {
        resource.info = loaded.info;
        for (const sc::SceneComponentDescriptor& d : loaded.component_descriptors)
        {
            descriptors.push_back(d);
        }
        for (const sc::SceneRelationRecord& r : loaded.relations)
        {
            relations.push_back(r);
        }
        crd::usize total = 0U;
        for (const sc::SceneResource::PerComponentPayload& p : loaded.component_payloads)
        {
            total += p.indices.size();
        }
        indices.reserve(total); // the views below point into it, so it never grows past this
        for (const sc::SceneResource::PerComponentPayload& p : loaded.component_payloads)
        {
            sc::SceneResource::PerComponentPayload copy{};
            copy.indices = ConstSpan<crd::u32>{indices.data() + indices.size(), p.indices.size()};
            copy.payloads = p.payloads;
            for (const crd::u32 i : p.indices)
            {
                indices.push_back(i);
            }
            resource.component_payloads.push_back(copy);
        }
        resource.component_descriptors = crd::containers::as_const_span(descriptors);
        resource.relations = crd::containers::as_const_span(relations);
    }
};

struct HandBuiltCase
{
    const char* what;
    void (*apply)(HandBuilt& h, crd::u32 probe_ci);
};

const HandBuiltCase kHandBuiltCases[] = {
    {"a record index past the entity table",
     [](HandBuilt& h, crd::u32 ci)
     {
         const crd::usize at =
             static_cast<crd::usize>(h.resource.component_payloads[ci].indices.data() - h.indices.data());
         h.indices[at] = 3U;
     }},
    {"fewer descriptors than component_count",
     [](HandBuilt& h, crd::u32)
     {
         ++h.resource.info.component_count;
     }},
    {"a payload view shorter than its records",
     [](HandBuilt& h, crd::u32 ci)
     {
         auto& p = h.resource.component_payloads[ci].payloads;
         p = ConstSpan<crd::u8>{p.data(), p.size() - 1U};
     }},
    {"a relation source past the entity table",
     [](HandBuilt& h, crd::u32)
     {
         h.relations[0].src_file_idx = 3U;
     }},
};
} // namespace

TEST_CASE("pack trust: hand-built scene views that contradict themselves are refused as malformed",
          "[scene][pack-trust][instantiate]")
{
    TlsfAllocator alloc{kHeapBytes};
    const Array<crd::u8> bytes = family_pack(Format::Scene, &alloc);
    const LoadedScene scene{bytes, &alloc};
    REQUIRE(scene.get() != nullptr);
    crd::u32 probe_ci = scene.get()->info.component_count;
    for (crd::u32 ci = 0U; ci < scene.get()->info.component_count; ++ci)
    {
        if (scene.get()->component_descriptors[ci].fourcc == kProbeFourCC)
        {
            probe_ci = ci;
        }
    }
    REQUIRE(probe_ci < scene.get()->info.component_count);

    sc::World target{&alloc};
    setup_world(target);
    {
        const HandBuilt unchanged{*scene.get(), &alloc};
        const sc::SceneInstantiation inst = target.instantiate_scene(unchanged.resource);
        CHECK_FALSE(inst.refused()); // the copy itself is sound
    }
    const crd::u32 before = target.entity_count();
    for (const HandBuiltCase& c : kHandBuiltCases)
    {
        INFO(c.what);
        HandBuilt h{*scene.get(), &alloc};
        c.apply(h, probe_ci);
        const sc::SceneInstantiation inst = target.instantiate_scene(h.resource);
        CHECK(inst.refusal == sc::PackRefusal::MalformedPack);
        CHECK(inst.entities.size() == 0U);
        CHECK(target.entity_count() == before);
    }
}

TEST_CASE("pack trust: a refused obek batch spawns nothing and takes no handle", "[scene][pack-trust][batch]")
{
    TlsfAllocator alloc{kHeapBytes};
    const Array<crd::u8> honest = family_pack(Format::Obek, &alloc);
    const LoadedObek obek{honest, &alloc};
    REQUIRE(obek.get() != nullptr);

    sc::World mismatched{&alloc};
    mismatched.register_component<Probe>(sc::default_serialize_trait<Probe>(kProbeFourCC, 2U));
    mismatched.register_builtin_relations();
    CHECK(mismatched.instantiate_obek_batch(*obek.get(), 4U).value == 0U);
    CHECK(mismatched.entity_count() == 0U);

    sc::World first{&alloc};
    setup_world(first);
    sc::World second{&alloc};
    setup_world(second);
    CHECK(first.instantiate_obek_batch(*obek.get(), 2U).value == 1U); // each World numbers its own batches
    CHECK(first.instantiate_obek_batch(*obek.get(), 1U).value == 2U);
    CHECK(second.instantiate_obek_batch(*obek.get(), 1U).value == 1U);
    CHECK(first.entity_count() == 9U);
}

TEST_CASE("pack trust: overrides and reverts never write a relation's target", "[scene][pack-trust][relations]")
{
    TlsfAllocator alloc{kHeapBytes};
    sc::World source{&alloc};
    setup_world(source);
    build_family(source, 2U);
    sc::ObekArtifactBuilder builder{&alloc, res::ResourceId{0x0BE0ULL, 0x7A57ULL}, 0xABCDULL};
    const sc::EntityId forged = sc::EntityId::make(77U, 1U);
    builder.add_override(1U, sc::kFourCC_RelChildOf, 0U,
                         ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&forged), sizeof(forged)});
    const Array<crd::u8> bytes = builder.build(source);
    const LoadedObek obek{bytes, &alloc};
    REQUIRE(obek.get() != nullptr);

    // Spawn first so the instance's ids differ from the source's (a stale source id then names another entity).
    sc::World target{&alloc};
    setup_world(target);
    for (crd::u32 i = 0U; i < 5U; ++i)
    {
        (void)target.spawn();
    }
    sc::ObekInstantiation inst = target.instantiate_obek(*obek.get());
    REQUIRE(inst.entities.size() == 3U);
    CHECK(inst.overrides_skipped == 1U);
    CHECK(inst.overrides_applied == 0U);
    CHECK(target.get_relation_target<sc::relations::ChildOf>(inst.entities[1]) == inst.entities[0]);

    sc::ObekOverride patch{};
    patch.file_idx = 2U;
    patch.component_fourcc = sc::kFourCC_RelChildOf;
    patch.payload = ConstSpan<crd::u8>{reinterpret_cast<const crd::u8*>(&forged), sizeof(forged)};
    sc::World second{&alloc};
    setup_world(second);
    const sc::ObekInstantiation patched =
        second.instantiate_obek(*obek.get(), sc::EntityId::null(), ConstSpan<sc::ObekOverride>{&patch, 1U});
    CHECK(patched.overrides_skipped == 2U); // the cook-time one and the caller's
    CHECK(second.get_relation_target<sc::relations::ChildOf>(patched.entities[2]) == patched.entities[0]);

    target.revert_component(inst, 1U, sc::kFourCC_RelChildOf);
    target.revert_entity(inst, 2U);
    CHECK(target.get_relation_target<sc::relations::ChildOf>(inst.entities[1]) == inst.entities[0]);
    CHECK(target.get_relation_target<sc::relations::ChildOf>(inst.entities[2]) == inst.entities[0]);
    target.destroy_immediate(inst.entities[0]);
    CHECK_FALSE(target.is_alive(inst.entities[1]));
    CHECK_FALSE(target.is_alive(inst.entities[2]));
}

TEST_CASE("pack trust: a component wider and more aligned than the in-place staging round-trips",
          "[scene][pack-trust][staging]")
{
    TlsfAllocator alloc{kHeapBytes};
    for (const Format f : {Format::Scene, Format::Obek})
    {
        INFO(name_of(f));
        sc::World source{&alloc};
        source.register_component<Wide>(wide_trait());
        for (crd::u32 i = 0U; i < 2U; ++i)
        {
            Wide w{};
            for (crd::usize b = 0U; b < sizeof(w.bytes); ++b)
            {
                w.bytes[b] = static_cast<crd::u8>((b * 7U + i) & 0xFFU);
            }
            source.add_component<Wide>(source.spawn(), w);
        }
        const Array<crd::u8> bytes = build_pack(f, source, &alloc);

        sc::World target{&alloc};
        target.register_component<Wide>(wide_trait());
        g_wide_staging_aligned = true;
        crd::u32 refused = 0U;
        REQUIRE(instantiate_refusal(f, bytes, target, &alloc, &refused) == sc::PackRefusal::None);
        CHECK(g_wide_staging_aligned);
        crd::u32 checked = 0U;
        for (const sc::EntityId e : target)
        {
            const Wide* const w = target.get_component<Wide>(e);
            REQUIRE(w != nullptr);
            const crd::u32 i = w->bytes[0];
            bool same = true;
            for (crd::usize b = 0U; b < sizeof(w->bytes); ++b)
            {
                same = same && w->bytes[b] == static_cast<crd::u8>((b * 7U + i) & 0xFFU);
            }
            CHECK(same);
            ++checked;
        }
        CHECK(checked == 2U);
    }
}

TEST_CASE("pack trust: a shared pool's capacity doubles from 8 and stops at its index range",
          "[scene][pack-trust][shared-pool]")
{
    using Pool = sc::SharedComponentPool;
    CHECK(Pool::grown_capacity(0U, 1U) == 8U);
    CHECK(Pool::grown_capacity(8U, 9U) == 16U);
    CHECK(Pool::grown_capacity(8U, 100U) == 128U);
    CHECK(Pool::grown_capacity(0x40000000U, 0x40000001U) == 0x80000000U);
    // 2^31 doubled is 2^32, which u32 wraps to 0 (and a doubling loop from 0 never ends): clamped to the range.
    CHECK(Pool::grown_capacity(0x80000000U, 0x80000001U) == Pool::kMaxEntries);
    CHECK(Pool::grown_capacity(0x40000000U, 0xC0000000U) == Pool::kMaxEntries);
    CHECK(Pool::grown_capacity(Pool::kMaxEntries - 1U, Pool::kMaxEntries) == Pool::kMaxEntries);
    CHECK(Pool::kMaxEntries == Pool::kInvalidIdx); // the last index, kMaxEntries - 1, stays distinct from it
    CHECK(Pool::incremented_refcount(5U) == 6U);
    CHECK(Pool::incremented_refcount(0xFFFFFFFEU) == 0xFFFFFFFFU);
}
