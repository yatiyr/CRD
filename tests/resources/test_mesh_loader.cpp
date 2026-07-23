#include <catch2/catch_test_macros.hpp>

#include <crd/cooker/cook_handler.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <new>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>

namespace fs = crd::platform::fs;
using namespace crd::resources;

alignas(crd::memory::GrowableTlsfAllocator) static unsigned char s_mesh_alloc_buf[sizeof(crd::memory::GrowableTlsfAllocator)];
static crd::memory::GrowableTlsfAllocator& s_mesh_alloc = *::new (s_mesh_alloc_buf) crd::memory::GrowableTlsfAllocator(); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// ── Pack assembly helper ──────────────────────────────────────────────────

struct MeshArt
{
    ResourceId                      id;
    crd::u32                        type_fourcc;
    crd::containers::Array<crd::u8> crdr_bytes;
    const char*                     name;
};

static fs::Path write_mesh_pack(crd::containers::Array<MeshArt>& arts)
{
    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8>       pool(&s_mesh_alloc);
    crd::containers::Array<ManifestEntry> entries(&s_mesh_alloc);

    for (const MeshArt& art : arts)
    {
        const crd::u32 off = static_cast<crd::u32>(pool.size());
        for (const char* p = art.name; *p != '\0'; ++p)
        {
            pool.push_back(static_cast<crd::u8>(*p));
        }
        pool.push_back(0U);

        ManifestEntry e;
        e.id            = art.id;
        e.type_fourcc   = art.type_fourcc;
        e.flags         = 0U;
        e.blob_offset   = 0U;
        e.blob_size     = static_cast<crd::u64>(art.crdr_bytes.size());
        e.name_strp_idx = off;
        entries.push_back(e);
    }

    // Pass 1: measure pack header size.
    {
        CrdrWriter p1(&s_mesh_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        const auto b1 = p1.finish();
        crd::u64   pos = static_cast<crd::u64>(b1.size());
        for (crd::usize i = 0U; i < arts.size(); ++i)
        {
            entries[i].blob_offset = pos;
            pos += static_cast<crd::u64>(arts[i].crdr_bytes.size());
        }
    }

    // Pass 2: assemble with real offsets.
    CrdrWriter p2(&s_mesh_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();

    for (const MeshArt& art : arts)
    {
        for (crd::usize i = 0U; i < art.crdr_bytes.size(); ++i)
        {
            pack_bytes.push_back(art.crdr_bytes[i]);
        }
    }

    const auto str_id = pack_id.to_string(&s_mesh_alloc);
    crd::containers::String tmp_name("test_mesh_", &s_mesh_alloc);
    tmp_name.append(str_id);
    tmp_name.append(".crdr");

    const fs::Path path(crd::containers::StringView(tmp_name.data(), tmp_name.size()));
    REQUIRE(fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)));
    return path;
}

// ── MESH artifact builder ─────────────────────────────────────────────────
//
// Builds a MESH CRDR artifact with `prim_count` identical triangle primitives.
// Each primitive has 3 vertices and 3 indices (triangle: 0, 1, 2).
// Vertex data: positions (1,0,0), (0,1,0), (0,0,1); normals (0,0,1); uvs (0,0);
// tangents (1,0,0,1).

static crd::containers::Array<crd::u8> make_mesh_artifact(
    ResourceId id, crd::u32 prim_count)
{
    constexpr crd::u32 k_vc = 3U; // vertices per prim
    constexpr crd::u32 k_ic = 3U; // indices per prim
    constexpr crd::u32 stride = 48U;

    // Interleaved vertex buffer for ONE primitive (3 × 48 = 144 bytes).
    // All three vertices share the same values for simplicity.
    crd::u8 vert_one[k_vc * stride] = {};
    for (crd::u32 vi = 0U; vi < k_vc; ++vi)
    {
        crd::u8* dst = vert_one + vi * stride;
        // position
        const float pos[3] = { static_cast<float>(vi + 1U), 0.0F, 0.0F };
        std::memcpy(dst +  0, pos, 12U);
        // normal (0,0,1)
        const float norm[3] = { 0.0F, 0.0F, 1.0F };
        std::memcpy(dst + 12, norm, 12U);
        // uv (0,0) — already zero
        // tangent (1,0,0,1)
        const float tan[4] = { 1.0F, 0.0F, 0.0F, 1.0F };
        std::memcpy(dst + 32, tan, 16U);
    }

    // Index buffer for ONE primitive (3 × u32 = 12 bytes).
    const crd::u32 idx_one[k_ic] = { 0U, 1U, 2U };

    // Build combined VERT and INDX buffers across all primitives.
    crd::containers::Array<crd::u8> vert_buf(&s_mesh_alloc);
    crd::containers::Array<crd::u8> indx_buf(&s_mesh_alloc);

    for (crd::u32 pi = 0U; pi < prim_count; ++pi)
    {
        const crd::usize vbo = vert_buf.size();
        (void)vbo; // byte offset for this prim's vertices
        for (crd::usize b = 0U; b < sizeof(vert_one); ++b)
        {
            vert_buf.push_back(vert_one[b]);
        }
        for (crd::u32 i : idx_one)
        {
            crd::u8 buf[4] = {};
            std::memcpy(buf, &i, 4U);
            for (crd::u8 b : buf)
            {
                indx_buf.push_back(b);
            }
        }
    }

    // PRIM chunk: 4-byte count + prim_count × 32-byte entries.
    crd::containers::Array<crd::u8> prim_buf(&s_mesh_alloc);
    prim_buf.resize(4U + static_cast<crd::usize>(prim_count) * 32U);

    std::memcpy(prim_buf.data(), &prim_count, 4U);

    for (crd::u32 pi = 0U; pi < prim_count; ++pi)
    {
        crd::u8* entry = prim_buf.data() + 4U + pi * 32U;
        const crd::u32 vc = k_vc;
        const crd::u32 ic = k_ic;
        const crd::u32 vbo = pi * k_vc * stride;
        const crd::u32 ibo = pi * k_ic * sizeof(crd::u32);
        std::memcpy(entry +  0, &vc,  4U);
        std::memcpy(entry +  4, &ic,  4U);
        std::memcpy(entry +  8, &vbo, 4U);
        std::memcpy(entry + 12, &ibo, 4U);
        // material_id: all-zero (null UUID)
    }

    CrdrWriter writer(&s_mesh_alloc, id, kFourCC_MESH);
    writer.add_chunk(kFourCC_VERT, crd::containers::as_const_span(vert_buf));
    writer.add_chunk(kFourCC_INDX, crd::containers::as_const_span(indx_buf));
    writer.add_chunk(kFourCC_PRIM, crd::containers::as_const_span(prim_buf));
    return writer.finish();
}

// ── Test 1: basic loader round-trip ──────────────────────────────────────────

TEST_CASE("MeshResource loads from CRDR artifact", "[resources][mesh][loader]")
{
    const ResourceId mesh_id = ResourceId::mint_random();

    crd::containers::Array<MeshArt> arts(&s_mesh_alloc);
    arts.push_back(MeshArt{
        mesh_id, kFourCC_MESH,
        make_mesh_artifact(mesh_id, 1U),
        "triangle.mesh"
    });

    const fs::Path pack_path = write_mesh_pack(arts);

    ResourceManager rm(&s_mesh_alloc);
    crd::resources::register_mesh_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::resources::MeshResource>(mesh_id);
    CHECK(handle.is_ready());

    const crd::resources::MeshResource* mesh = handle.get();
    REQUIRE(mesh != nullptr);
    CHECK(mesh->primitives.size() == 1U);
    CHECK(mesh->primitives[0].vertex_count == 3U);
    CHECK(mesh->primitives[0].index_count  == 3U);
    CHECK(mesh->vertices.size() == 3U * 48U);
    CHECK(mesh->indices.size()  == 3U * 4U);

    (void)fs::remove_file(pack_path);
}

// ── Test 2: multi-primitive MESH ──────────────────────────────────────────────

TEST_CASE("MeshResource multi-primitive has correct counts", "[resources][mesh][multi_prim]")
{
    const ResourceId mesh_id = ResourceId::mint_random();

    crd::containers::Array<MeshArt> arts(&s_mesh_alloc);
    arts.push_back(MeshArt{
        mesh_id, kFourCC_MESH,
        make_mesh_artifact(mesh_id, 3U),
        "multi.mesh"
    });

    const fs::Path pack_path = write_mesh_pack(arts);

    ResourceManager rm(&s_mesh_alloc);
    crd::resources::register_mesh_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::resources::MeshResource>(mesh_id);
    REQUIRE(handle.is_ready());

    const crd::resources::MeshResource* mesh = handle.get();
    REQUIRE(mesh != nullptr);
    CHECK(mesh->primitives.size() == 3U);
    for (crd::u32 pi = 0U; pi < 3U; ++pi)
    {
        CHECK(mesh->primitives[pi].vertex_count == 3U);
        CHECK(mesh->primitives[pi].index_count  == 3U);
    }

    (void)fs::remove_file(pack_path);
}

// ── Test 3: missing VERT chunk → Failed ──────────────────────────────────────

TEST_CASE("MeshResource fails when VERT chunk is absent", "[resources][mesh][missing_vert]")
{
    const ResourceId mesh_id = ResourceId::mint_random();

    // Build a MESH artifact with only INDX + PRIM (no VERT).
    constexpr crd::u32 k_ic = 3U;
    crd::u32 idx_data[k_ic] = { 0U, 1U, 2U };

    crd::containers::Array<crd::u8> indx_buf(&s_mesh_alloc);
    indx_buf.resize(k_ic * sizeof(crd::u32));
    std::memcpy(indx_buf.data(), idx_data, sizeof(idx_data));

    crd::containers::Array<crd::u8> prim_buf(&s_mesh_alloc);
    prim_buf.resize(4U + 32U);
    crd::u32 prim_count = 1U;
    crd::u32 vc = 3U;
    crd::u32 ic = 3U;
    crd::u32 vbo = 0U;
    crd::u32 ibo = 0U;
    std::memcpy(prim_buf.data(), &prim_count, 4U);
    std::memcpy(prim_buf.data() + 4,  &vc,  4U);
    std::memcpy(prim_buf.data() + 8,  &ic,  4U);
    std::memcpy(prim_buf.data() + 12, &vbo, 4U);
    std::memcpy(prim_buf.data() + 16, &ibo, 4U);

    CrdrWriter writer(&s_mesh_alloc, mesh_id, kFourCC_MESH);
    writer.add_chunk(kFourCC_INDX, crd::containers::as_const_span(indx_buf));
    writer.add_chunk(kFourCC_PRIM, crd::containers::as_const_span(prim_buf));
    auto crdr_bytes = writer.finish();

    crd::containers::Array<MeshArt> arts(&s_mesh_alloc);
    arts.push_back(MeshArt{
        mesh_id, kFourCC_MESH, std::move(crdr_bytes), "novert.mesh"
    });

    const fs::Path pack_path = write_mesh_pack(arts);

    ResourceManager rm(&s_mesh_alloc);
    crd::resources::register_mesh_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::resources::MeshResource>(mesh_id);
    CHECK(handle.state() == LoadState::Failed);

    (void)fs::remove_file(pack_path);
}

// ── Test 4: cook a GLB via the cook handler ───────────────────────────────────

// Builds a minimal GLB binary for a single triangle mesh.
// Vertex data: 3 vertices with POSITION, NORMAL, TEXCOORD_0, TANGENT.
// Index data:  3 x u16 indices (0, 1, 2).
static crd::containers::Array<crd::u8> make_triangle_glb()
{
    // Vertex attributes in binary buffer:
    //   positions (VEC3 float): 3 * 12 = 36 bytes  at offset 0
    //   normals   (VEC3 float): 3 * 12 = 36 bytes  at offset 36
    //   texcoords (VEC2 float): 3 *  8 = 24 bytes  at offset 72
    //   tangents  (VEC4 float): 3 * 16 = 48 bytes  at offset 96
    //   indices   (SCALAR u16): 3 *  2 =  6 bytes  at offset 144 (padded to 8)
    // Total binary buffer: 152 bytes (aligned to 4)

    const float pos[9]  = { 0.0F,0.0F,0.0F,  1.0F,0.0F,0.0F,  0.0F,1.0F,0.0F };
    const float norm[9] = { 0.0F,0.0F,1.0F,  0.0F,0.0F,1.0F,  0.0F,0.0F,1.0F };
    const float uv[6]   = { 0.0F,0.0F,  1.0F,0.0F,  0.5F,1.0F };
    const float tan[12] = { 1.0F,0.0F,0.0F,1.0F,  1.0F,0.0F,0.0F,1.0F,  1.0F,0.0F,0.0F,1.0F };
    const crd::u16 idx[3] = { 0U, 1U, 2U };

    crd::containers::Array<crd::u8> bin_buf(&s_mesh_alloc);
    bin_buf.resize(152U); // 144 + 6 idx + 2 pad

    std::memcpy(bin_buf.data() +   0, pos,  36U);
    std::memcpy(bin_buf.data() +  36, norm, 36U);
    std::memcpy(bin_buf.data() +  72, uv,   24U);
    std::memcpy(bin_buf.data() +  96, tan,  48U);
    std::memcpy(bin_buf.data() + 144, idx,   6U);

    // JSON chunk (componentType 5126 = float, 5123 = unsigned short).
    const char* json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"meshes\":[{\"name\":\"Triangle\",\"primitives\":"
        "[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"TANGENT\":3},"
        "\"indices\":4,\"mode\":4}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":144,\"byteLength\":6}],"
        "\"buffers\":[{\"byteLength\":152}]}";

    const crd::u32 json_len_raw  = static_cast<crd::u32>(std::strlen(json));
    const crd::u32 json_len_pad  = (json_len_raw + 3U) & ~3U;
    const crd::u32 bin_len       = static_cast<crd::u32>(bin_buf.size());
    const crd::u32 total_len     = 12U                        // GLB header
                                 + 8U + json_len_pad           // JSON chunk
                                 + 8U + bin_len;               // BIN  chunk

    crd::containers::Array<crd::u8> glb(&s_mesh_alloc);

    // GLB header (12 bytes).
    constexpr crd::u32 glb_magic   = 0x46546C67U; // 'glTF' LE
    constexpr crd::u32 glb_version = 2U;
    glb.resize(12U);
    std::memcpy(glb.data() + 0, &glb_magic,   4U);
    std::memcpy(glb.data() + 4, &glb_version, 4U);
    std::memcpy(glb.data() + 8, &total_len,   4U);

    // JSON chunk header.
    constexpr crd::u32 chunk_json = 0x4E4F534AU;
    glb.resize(glb.size() + 8U);
    std::memcpy(glb.data() + 12, &json_len_pad, 4U);
    std::memcpy(glb.data() + 16, &chunk_json,   4U);

    // JSON payload (space-padded to json_len_pad).
    for (crd::u32 i = 0U; i < json_len_raw; ++i)
    {
        glb.push_back(static_cast<crd::u8>(json[i]));
    }
    for (crd::u32 i = json_len_raw; i < json_len_pad; ++i)
    {
        glb.push_back(static_cast<crd::u8>(' '));
    }

    // BIN chunk header.
    constexpr crd::u32 chunk_bin = 0x004E4942U;
    const crd::usize bin_hdr_off = glb.size();
    glb.resize(glb.size() + 8U);
    std::memcpy(glb.data() + bin_hdr_off,     &bin_len,   4U);
    std::memcpy(glb.data() + bin_hdr_off + 4, &chunk_bin, 4U);

    // BIN payload.
    for (crd::u8 b : bin_buf)
    {
        glb.push_back(b);
    }

    return glb;
}

TEST_CASE("MeshResource cooked from GLB round-trip", "[resources][mesh][cook]")
{
    // Write the minimal GLB to a temp file.
    const ResourceId glb_id = ResourceId::mint_random();
    const auto       str_id = glb_id.to_string(&s_mesh_alloc);
    crd::containers::String glb_name("test_cook_", &s_mesh_alloc);
    glb_name.append(str_id);
    glb_name.append(".glb");

    const fs::Path glb_path(crd::containers::StringView(glb_name.data(), glb_name.size()));
    auto glb_bytes = make_triangle_glb();
    REQUIRE(fs::write_file_binary(glb_path, crd::containers::as_const_span(glb_bytes)));

    // Find and invoke the GLB cook handler.
    crd::cooker::register_builtin_handlers();

    const crd::cooker::CookHandlerFn fn = crd::cooker::find_cook_handler(".glb");
    REQUIRE(fn != nullptr);

    const ResourceId mesh_id = ResourceId::mint_random();
    crd::cooker::CookContext cook_ctx;
    cook_ctx.source_path = crd::containers::StringView(glb_name.data(), glb_name.size());
    cook_ctx.id          = mesh_id;
    cook_ctx.allocator   = &s_mesh_alloc;

    crd::cooker::CookResult cook_result = fn(cook_ctx);
    REQUIRE(cook_result.ok);
    CHECK(cook_result.type_fourcc == kFourCC_MESH);

    (void)fs::remove_file(glb_path);

    // Mount and load.
    crd::containers::Array<MeshArt> arts(&s_mesh_alloc);
    arts.push_back(MeshArt{
        mesh_id, kFourCC_MESH,
        std::move(cook_result.cooked_bytes),
        "cooked.mesh"
    });

    const fs::Path pack_path = write_mesh_pack(arts);

    ResourceManager rm(&s_mesh_alloc);
    crd::resources::register_mesh_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::resources::MeshResource>(mesh_id);
    REQUIRE(handle.is_ready());

    const crd::resources::MeshResource* mesh = handle.get();
    REQUIRE(mesh != nullptr);
    CHECK(mesh->primitives.size() == 1U);
    CHECK(mesh->primitives[0].vertex_count == 3U);
    CHECK(mesh->primitives[0].index_count  == 3U);
    CHECK(!mesh->vertices.empty());
    CHECK(!mesh->indices.empty());

    // Verify first vertex position bytes (0.0f, 0.0f, 0.0f).
    REQUIRE(mesh->vertices.size() >= 12U);
    float px = 0.0F;
    std::memcpy(&px, mesh->vertices.data(), 4U);
    CHECK(px == 0.0F);

    (void)fs::remove_file(pack_path);
}

// ── GEO-1: the RM-mounted IMPORT → COOK → LOAD end-to-end ──────────────────────────────────────────────────────────────
// A millimetre-authored STL cooks through the REAL wave1 handler (crd-asset-io parse → validate → 48-byte interleave with
// `.meta position_scale`), the artifact is packed + MOUNTED, and `load_sync<MeshResource>` delivers the scaled geometry —
// the full resource-system path an imported file takes in production.

namespace crd::cooker
{
void register_wave1_mesh_handler(); // mesh_wave1.cpp (crd-cooker)
} // namespace crd::cooker

TEST_CASE("GEO-1: an imported STL cooks, packs, mounts, and LOADS via ResourceManager", "[resources][mesh][geo]")
{
    // 1. the source STL (one triangle, 1000 mm legs) + its .meta (uuid + mm→m position_scale)
    crd::containers::Array<crd::u8> stl(&s_mesh_alloc);
    {
        const auto pushf = [&](float v) {
            crd::u8 raw[4];
            std::memcpy(raw, &v, 4);
            for (crd::u8 x : raw) { stl.push_back(x); }
        };
        for (int i = 0; i < 80; ++i) { stl.push_back(0); }
        const crd::u32 count = 1;
        crd::u8        raw[4];
        std::memcpy(raw, &count, 4);
        for (crd::u8 x : raw) { stl.push_back(x); }
        const float tri[12] = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1000.0F, 0.0F, 0.0F, 0.0F, 1000.0F, 0.0F};
        for (float v : tri) { pushf(v); }
        stl.push_back(0);
        stl.push_back(0);
    }
    const char* src_path  = "cerid_geo1_rm.stl";
    const char* meta_path = "cerid_geo1_rm.stl.meta";
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView(src_path)), crd::containers::as_const_span(stl)));
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(meta_path)),
                                crd::containers::StringView("[cook]\nposition_scale = 0.001\n")));

    // 2. cook through the REAL wave1 handler
    static bool s_wave1_registered = false;
    if (!s_wave1_registered)
    {
        crd::cooker::register_wave1_mesh_handler();
        s_wave1_registered = true;
    }
    crd::cooker::CookHandlerFn handler = crd::cooker::find_cook_handler(crd::containers::StringView(".stl"));
    REQUIRE(handler != nullptr);
    const ResourceId         mesh_id = ResourceId::mint_random();
    crd::cooker::CookContext cctx;
    cctx.source_path = crd::containers::StringView(src_path);
    cctx.meta_path   = crd::containers::StringView(meta_path);
    cctx.id          = mesh_id;
    cctx.allocator   = &s_mesh_alloc;
    crd::cooker::CookResult cooked = handler(cctx);
    REQUIRE(cooked.ok);

    // 3. pack + MOUNT + load_sync — the production path
    crd::containers::Array<MeshArt> arts(&s_mesh_alloc);
    arts.push_back(MeshArt{mesh_id, kFourCC_MESH, std::move(cooked.cooked_bytes), "imported.stl"});
    const fs::Path pack_path = write_mesh_pack(arts);

    ResourceManager rm(&s_mesh_alloc);
    crd::resources::register_mesh_loader(&rm);
    REQUIRE(rm.mount_manifest(pack_path.generic()).is_valid());
    auto handle = rm.load_sync<crd::resources::MeshResource>(mesh_id);
    CHECK(handle.is_ready());
    const crd::resources::MeshResource* mesh = handle.get();
    REQUIRE(mesh != nullptr);

    // 4. the loaded resource IS the imported triangle, in SI metres
    REQUIRE(mesh->primitives.size() == 1U);
    CHECK(mesh->primitives[0].vertex_count == 3U);
    CHECK(mesh->primitives[0].index_count == 3U);
    REQUIRE(mesh->vertices.size() == 3U * 48U);
    float v1x = 0.0F;
    std::memcpy(&v1x, mesh->vertices.data() + 48U, 4U); // second vertex: 1000 mm → 1 m
    CHECK(v1x == 1.0F);
    float n0z = 0.0F;
    std::memcpy(&n0z, mesh->vertices.data() + 20U, 4U); // first vertex normal.z
    CHECK(n0z == 1.0F);

    (void)fs::remove_file(pack_path);
    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
    (void)fs::remove_file(fs::Path(crd::containers::StringView(meta_path)));
}
