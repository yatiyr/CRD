// tests/resources/test_streaming_loaders.cpp — RET-3 (ADR-0105 + ADR-0085 S5): the resource loaders become the
// STREAMING ALLOCATOR's first real consumer — the integration the S5 slice explicitly deferred "until the shape is
// known". The shape: loaders take an INJECTED payload allocator; a `StreamingCategoryAllocator` view routes every
// loaded payload into the resident store under per-category budgets. Gates: category usage TRACKS the load/unload
// lifecycle (rises on load, returns to zero on unload — proof payloads live in the resident store and release O(1)),
// and a budget too small for even the payload header REFUSES the load GRACEFULLY (a not-ready handle, no fatal).
// The residency-POLICY (evict-until-fits via the RM's 2Q-LRU) stays injected-and-null per the ADR — it lands with
// open-world streaming; the mechanism below is what it will drive.

#include <catch2/catch_test_macros.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/streaming_allocator.hpp>
#include <crd/memory/allocators/streaming_category_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/resources/texture_resource.hpp>

#include <cstring>
#include <new>

namespace fs = crd::platform::fs;
using namespace crd::resources;

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
alignas(crd::memory::GrowableTlsfAllocator) unsigned char g_scratch_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
crd::memory::GrowableTlsfAllocator& g_scratch = *::new (g_scratch_buf) crd::memory::GrowableTlsfAllocator();
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

constexpr crd::memory::CategoryId kCatTextures = 0;
constexpr crd::memory::CategoryId kCatMeshes   = 1;

// a 4×4 single-mip TXTR artifact (64 pixel bytes + header)
crd::containers::Array<crd::u8> make_txtr(const ResourceId& id)
{
    CrdrWriter writer(&g_scratch, id, kFourCC_TXTR);
    crd::u8    head[16] = {};
    const crd::u32 w    = 4U;
    const crd::u32 h    = 4U;
    const crd::u32 mips = 1U;
    std::memcpy(head + 0, &w, 4U);
    std::memcpy(head + 4, &h, 4U);
    std::memcpy(head + 8, &mips, 4U);
    writer.add_chunk(kFourCC_HEAD, crd::containers::ConstSpan<crd::u8>(head, 16U));
    crd::containers::Array<crd::u8> px(&g_scratch);
    px.resize(4U * 4U * 4U, 0x77);
    writer.add_chunk(kFourCC_MIP0, crd::containers::as_const_span(px));
    return writer.finish();
}

// a 3-vertex 1-primitive MESH artifact
crd::containers::Array<crd::u8> make_mesh(const ResourceId& id)
{
    CrdrWriter                      writer(&g_scratch, id, kFourCC_MESH);
    crd::containers::Array<crd::u8> vert(&g_scratch);
    vert.resize(3U * kMeshVertexStride, 0);
    writer.add_chunk(kFourCC_VERT, crd::containers::as_const_span(vert));
    crd::containers::Array<crd::u8> indx(&g_scratch);
    indx.resize(3U * 4U, 0);
    writer.add_chunk(kFourCC_INDX, crd::containers::as_const_span(indx));
    crd::containers::Array<crd::u8> prim(&g_scratch);
    prim.resize(4U + 32U, 0);
    const crd::u32 count = 1U;
    std::memcpy(prim.data(), &count, 4U);
    const crd::u32 vc = 3U;
    const crd::u32 ic = 3U;
    std::memcpy(prim.data() + 4U, &vc, 4U);
    std::memcpy(prim.data() + 8U, &ic, 4U);
    writer.add_chunk(kFourCC_PRIM, crd::containers::as_const_span(prim));
    return writer.finish();
}

// pack one artifact into a mounted-pack file (the loader-test pattern, minimal)
fs::Path write_pack(const ResourceId& id, crd::u32 type_fourcc, const crd::containers::Array<crd::u8>& bytes,
                    const char* tag)
{
    const ResourceId                      pack_id = ResourceId::mint_random();
    crd::containers::Array<crd::u8>       pool(&g_scratch);
    crd::containers::Array<ManifestEntry> entries(&g_scratch);
    for (const char* p = tag; *p != '\0'; ++p) { pool.push_back(static_cast<crd::u8>(*p)); }
    pool.push_back(0U);
    ManifestEntry e;
    e.id            = id;
    e.type_fourcc   = type_fourcc;
    e.flags         = 0U;
    e.blob_offset   = 0U;
    e.blob_size     = static_cast<crd::u64>(bytes.size());
    e.name_strp_idx = 0U;
    entries.push_back(e);

    crd::u64 header_size = 0;
    {
        CrdrWriter p1(&g_scratch, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
        header_size = static_cast<crd::u64>(p1.finish().size());
    }
    entries[0].blob_offset = header_size;
    CrdrWriter p2(&g_scratch, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (crd::usize i = 0; i < bytes.size(); ++i) { pack_bytes.push_back(bytes[i]); }

    const auto              sid = pack_id.to_string(&g_scratch);
    crd::containers::String name("test_stream_", &g_scratch);
    name.append(tag);
    name.append("_");
    name.append(sid);
    name.append(".crdr");
    const fs::Path path(crd::containers::StringView(name.data(), name.size()));
    REQUIRE(fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)));
    return path;
}

} // namespace

TEST_CASE("RET-3: loaders on the STREAMING allocator -- category usage tracks the load/unload lifecycle",
          "[resources][streaming][ret]")
{
    crd::memory::StreamingAllocator::Config cfg;
    cfg.reserve_bytes        = crd::usize{64} << 20; // a small reservation — the test's whole world
    cfg.resident_chunk_bytes = crd::usize{4} << 20;
    cfg.staging_bytes        = crd::usize{1} << 20;
    cfg.num_categories       = 4;
    crd::memory::StreamingAllocator sa(cfg, nullptr, "test-streaming");
    sa.set_budget(kCatTextures, crd::u64{8} << 20);
    sa.set_budget(kCatMeshes, crd::u64{8} << 20);

    crd::memory::StreamingCategoryAllocator tex_view(&sa, kCatTextures);
    crd::memory::StreamingCategoryAllocator mesh_view(&sa, kCatMeshes);

    const ResourceId tex_id  = ResourceId::mint_random();
    const ResourceId mesh_id = ResourceId::mint_random();
    const fs::Path   tex_pack  = write_pack(tex_id, kFourCC_TXTR, make_txtr(tex_id), "tex");
    const fs::Path   mesh_pack = write_pack(mesh_id, kFourCC_MESH, make_mesh(mesh_id), "mesh");

    CHECK(sa.used(kCatTextures) == 0U);
    CHECK(sa.used(kCatMeshes) == 0U);

    {
        ResourceManager rm(&g_scratch);
        register_texture_loader(&rm, &tex_view);  // the injected seam: payloads → the RESIDENT store, per category
        register_mesh_loader(&rm, &mesh_view);
        REQUIRE(rm.mount_manifest(tex_pack.generic()).is_valid());
        REQUIRE(rm.mount_manifest(mesh_pack.generic()).is_valid());

        auto tex = rm.load_sync<TextureResource>(tex_id);
        REQUIRE(tex.is_ready());
        REQUIRE(tex.get() != nullptr);
        CHECK(tex.get()->mips[0].pixels.size() == 64U);
        const crd::u64 tex_used = sa.used(kCatTextures);
        CHECK(tex_used >= 64U); // ≥ the pixel payload (plus the resource header + array blocks) — resident and charged
        CHECK(sa.used(kCatMeshes) == 0U); // categories are ISOLATED — the texture never charges the mesh budget

        auto mesh = rm.load_sync<MeshResource>(mesh_id);
        REQUIRE(mesh.is_ready());
        REQUIRE(mesh.get() != nullptr);
        CHECK(mesh.get()->vertices.size() == 3U * kMeshVertexStride);
        CHECK(sa.used(kCatMeshes) >= 3U * kMeshVertexStride);
        CHECK(sa.used(kCatTextures) == tex_used); // and vice versa
    } // handles + the manager die → every payload unloads through the loaders
    CHECK(sa.used(kCatTextures) == 0U); // O(1) release back to the resident store, fully un-charged
    CHECK(sa.used(kCatMeshes) == 0U);

    // a budget too small for even the payload header REFUSES the load GRACEFULLY — no fatal, a not-ready handle
    {
        sa.set_budget(kCatTextures, 8U); // smaller than sizeof(TextureResource)
        ResourceManager rm(&g_scratch);
        register_texture_loader(&rm, &tex_view);
        REQUIRE(rm.mount_manifest(tex_pack.generic()).is_valid());
        auto tex = rm.load_sync<TextureResource>(tex_id);
        CHECK(!tex.is_ready());
        CHECK(tex.get() == nullptr);
        CHECK(sa.used(kCatTextures) == 0U); // nothing leaked by the refused load
    }

    (void)fs::remove_file(tex_pack);
    (void)fs::remove_file(mesh_pack);
}
