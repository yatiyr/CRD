// tests/cooker/test_mesh_wave1.cpp — GEO-1: the Wave-1 (.stl/.obj/.ply) mesh cook handler gates. Real temp files cook
// through the REAL handler into MESH CRDR artifacts, read back with crdr_read: the 48-byte interleave is byte-exact, the
// .meta position_scale lands (SI-unit boundary), multi-object OBJ produces sidecar-.meta extra artifacts with STABLE ids
// across recooks, and a point cloud fails with ok=false (honest: no triangle geometry).

#include <catch2/catch_test_macros.hpp>

#include <crd/cooker/cook_handler.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <cmath>
#include <cstring>

namespace fs = crd::platform::fs;

namespace crd::cooker
{
void register_wave1_mesh_handler(); // defined in mesh_wave1.cpp (registration is idempotent per-run: called once below)
}

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

crd::cooker::CookHandlerFn wave1()
{
    static bool registered = false;
    if (!registered)
    {
        crd::cooker::register_wave1_mesh_handler();
        registered = true;
    }
    return crd::cooker::find_cook_handler(crd::containers::StringView(".stl"));
}

// ── binary STL fixture (mirrors tests/asset-io) ────────────────────────────────────────────────────────────────────────
void push_f32(crd::containers::Array<crd::u8>& b, crd::f32 v)
{
    crd::u8 raw[4];
    std::memcpy(raw, &v, 4);
    for (crd::u8 x : raw) { b.push_back(x); }
}
void push_u32(crd::containers::Array<crd::u8>& b, crd::u32 v)
{
    crd::u8 raw[4];
    std::memcpy(raw, &v, 4);
    for (crd::u8 x : raw) { b.push_back(x); }
}
void push_tri(crd::containers::Array<crd::u8>& b, const crd::f32* n, const crd::f32* v0, const crd::f32* v1,
              const crd::f32* v2)
{
    for (int i = 0; i < 3; ++i) { push_f32(b, n[i]); }
    for (int i = 0; i < 3; ++i) { push_f32(b, v0[i]); }
    for (int i = 0; i < 3; ++i) { push_f32(b, v1[i]); }
    for (int i = 0; i < 3; ++i) { push_f32(b, v2[i]); }
    b.push_back(0);
    b.push_back(0); // attr u16
}

void write_text(const char* path, const char* text)
{
    REQUIRE(fs::write_file_text(fs::Path(crd::containers::StringView(path)),
                                crd::containers::StringView(text, std::strlen(text))));
}

[[nodiscard]] crd::f32 read_f32(const crd::u8* p)
{
    crd::f32 v = 0.0F;
    std::memcpy(&v, p, 4);
    return v;
}

} // namespace

TEST_CASE("cooker: wave1 STL cooks to a MESH CRDR -- interleave exact, position_scale applied", "[cooker][wave1]")
{
    // a 1-triangle STL authored in MILLIMETRES (1000 mm legs); .meta position_scale 0.001 -> 1 m in the artifact
    crd::containers::Array<crd::u8> stl(&g_alloc);
    for (int i = 0; i < 80; ++i) { stl.push_back(0); }
    push_u32(stl, 1);
    const crd::f32 nz[3] = {0.0F, 0.0F, 1.0F};
    const crd::f32 a[3]  = {0.0F, 0.0F, 0.0F};
    const crd::f32 b[3]  = {1000.0F, 0.0F, 0.0F};
    const crd::f32 c[3]  = {0.0F, 1000.0F, 0.0F};
    push_tri(stl, nz, a, b, c);

    const char* src_path  = "cerid_wave1_tri.stl";
    const char* meta_path = "cerid_wave1_tri.stl.meta";
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView(src_path)),
                                  crd::containers::as_const_span(stl)));
    write_text(meta_path, "[id]\nuuid = \"0123456789abcdef0123456789abcdef\"\n[cook]\nposition_scale = 0.001\n");

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.meta_path   = crd::containers::StringView(meta_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;

    const crd::cooker::CookResult result = wave1()(ctx);
    REQUIRE(result.ok);
    CHECK(result.type_fourcc == crd::resources::kFourCC_MESH);

    crd::resources::CrdrFile file(&g_alloc);
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(result.cooked_bytes), file, &g_alloc)
            == crd::resources::CrdrError::Ok);
    CHECK(file.type_fourcc == crd::resources::kFourCC_MESH);
    CHECK(file.id == ctx.id);

    const crd::resources::CrdrChunk* vert = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_VERT);
    const crd::resources::CrdrChunk* indx = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_INDX);
    const crd::resources::CrdrChunk* prim = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_PRIM);
    REQUIRE(vert != nullptr);
    REQUIRE(indx != nullptr);
    REQUIRE(prim != nullptr);

    REQUIRE(vert->payload.size() == 3U * 48U); // 3 soup vertices, 48-byte stride
    const crd::u8* v1 = vert->payload.data() + 48U; // second vertex = (1000mm,0,0) -> (1m,0,0)
    CHECK(read_f32(v1 + 0) == 1.0F);               // position_scale APPLIED (the SI boundary)
    CHECK(read_f32(v1 + 4) == 0.0F);
    CHECK(read_f32(v1 + 12 + 8) == 1.0F); // normal.z
    CHECK(read_f32(v1 + 44) == 1.0F);     // tangent.w = +1 (GEO-2 fills xyz)

    REQUIRE(indx->payload.size() == 3U * 4U);
    crd::u32 i2 = 0;
    std::memcpy(&i2, indx->payload.data() + 8U, 4U);
    CHECK(i2 == 2U);

    REQUIRE(prim->payload.size() == 4U + 32U);
    crd::u32 prim_count = 0;
    crd::u32 vc         = 0;
    crd::u32 ic         = 0;
    std::memcpy(&prim_count, prim->payload.data(), 4U);
    std::memcpy(&vc, prim->payload.data() + 4U, 4U);
    std::memcpy(&ic, prim->payload.data() + 8U, 4U);
    CHECK(prim_count == 1U);
    CHECK(vc == 3U);
    CHECK(ic == 3U);

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
    (void)fs::remove_file(fs::Path(crd::containers::StringView(meta_path)));
}

TEST_CASE("cooker: wave1 multi-object OBJ -- sidecar-.meta extra artifacts with STABLE ids across recooks", "[cooker][wave1]")
{
    const char* obj_text = "o first\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"
                           "o second\nv 0 0 1\nv 1 0 1\nv 0 1 1\nf 4 5 6\n";
    const char* src_path     = "cerid_wave1_two.obj";
    const char* sidecar_path = "cerid_wave1_two.obj.mesh.second.meta";
    write_text(src_path, obj_text);

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;

    const crd::cooker::CookResult first = wave1()(ctx);
    REQUIRE(first.ok);
    REQUIRE(first.extra_artifacts.size() == 1U);                     // mesh "second" -> extra artifact
    CHECK(fs::is_file(fs::Path(crd::containers::StringView(sidecar_path)))); // sidecar .meta minted
    const crd::resources::ResourceId first_extra_id = first.extra_artifacts[0].id;
    CHECK(!first_extra_id.is_null());

    // RECOOK: the sidecar .meta must replay the SAME id (id stability across recooks — the glTF pattern)
    const crd::cooker::CookResult second = wave1()(ctx);
    REQUIRE(second.ok);
    REQUIRE(second.extra_artifacts.size() == 1U);
    CHECK(second.extra_artifacts[0].id == first_extra_id);

    // both artifacts parse as MESH CRDRs
    crd::resources::CrdrFile main_file(&g_alloc);
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(second.cooked_bytes), main_file, &g_alloc)
            == crd::resources::CrdrError::Ok);
    crd::resources::CrdrFile extra_file(&g_alloc);
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(second.extra_artifacts[0].cooked_bytes),
                                      extra_file, &g_alloc)
            == crd::resources::CrdrError::Ok);
    CHECK(extra_file.id == first_extra_id);

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
    (void)fs::remove_file(fs::Path(crd::containers::StringView(sidecar_path)));
}

TEST_CASE("cooker: wave1 GLB cooks via OUR glTF parser -- authored tangents PRESERVED byte-exact", "[cooker][wave1]")
{
    // a GLB triangle with positions + normals + uvs + AUTHORED tangents. The tangent is deliberately (0,1,0,-1) —
    // NOT what our generator would produce for this straight chart (+X, w=+1) — so the cooked bytes PROVE the authored
    // frame passed through the conditioning chain untouched (the GEO-3 authored-data rule).
    crd::containers::Array<crd::u8> bin(&g_alloc);
    const crd::f32 pos[9]  = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const crd::f32 nrm[9]  = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    const crd::f32 uv[6]   = {0, 0, 1, 0, 0, 1};
    const crd::f32 tan[12] = {0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1};
    for (crd::f32 v : pos) { push_f32(bin, v); }
    for (crd::f32 v : nrm) { push_f32(bin, v); }
    for (crd::f32 v : uv) { push_f32(bin, v); }
    for (crd::f32 v : tan) { push_f32(bin, v); }

    const char* json = "{\"asset\":{\"version\":\"2.0\"},"
                       "\"buffers\":[{\"byteLength\":144}],"
                       "\"bufferViews\":["
                       "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                       "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
                       "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
                       "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":48}],"
                       "\"accessors\":["
                       "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                       "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                       "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
                       "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}],"
                       "\"meshes\":[{\"name\":\"authored\",\"primitives\":[{\"attributes\":"
                       "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"TANGENT\":3}}]}]}";

    // GLB wrap
    crd::containers::Array<crd::u8> glb(&g_alloc);
    const crd::u32                  jlen = static_cast<crd::u32>(std::strlen(json));
    const crd::u32                  jpad = (4U - (jlen % 4U)) % 4U;
    const crd::u32                  blen = static_cast<crd::u32>(bin.size());
    const crd::u32                  bpad = (4U - (blen % 4U)) % 4U;
    push_u32(glb, 0x46546C67U);
    push_u32(glb, 2U);
    push_u32(glb, 12U + 8U + jlen + jpad + 8U + blen + bpad);
    push_u32(glb, jlen + jpad);
    push_u32(glb, 0x4E4F534AU);
    for (crd::u32 i = 0; i < jlen; ++i) { glb.push_back(static_cast<crd::u8>(json[i])); }
    for (crd::u32 i = 0; i < jpad; ++i) { glb.push_back(' '); }
    push_u32(glb, blen + bpad);
    push_u32(glb, 0x004E4942U);
    for (crd::usize i = 0; i < bin.size(); ++i) { glb.push_back(bin[i]); }
    for (crd::u32 i = 0; i < bpad; ++i) { glb.push_back(0); }

    const char* src_path = "cerid_wave1_authored.glb";
    REQUIRE(fs::write_file_binary(fs::Path(crd::containers::StringView(src_path)), crd::containers::as_const_span(glb)));

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;
    const crd::cooker::CookResult result = wave1()(ctx); // OUR handler owns .glb now (first-wins over cgltf)
    REQUIRE(result.ok);

    crd::resources::CrdrFile file(&g_alloc);
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(result.cooked_bytes), file, &g_alloc)
            == crd::resources::CrdrError::Ok);
    const crd::resources::CrdrChunk* vert = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_VERT);
    REQUIRE(vert != nullptr);
    REQUIRE(vert->payload.size() == 3U * 48U);
    // vertex 0: tangent at bytes 32..47 must be the AUTHORED (0,1,0,-1) — regeneration would have produced (1,0,0,+1)
    CHECK(read_f32(vert->payload.data() + 32U) == 0.0F);
    CHECK(read_f32(vert->payload.data() + 36U) == 1.0F);
    CHECK(read_f32(vert->payload.data() + 40U) == 0.0F);
    CHECK(read_f32(vert->payload.data() + 44U) == -1.0F);
    // and the uv landed
    CHECK(read_f32(vert->payload.data() + 48U + 24U) == 1.0F); // vertex 1 u

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
}

TEST_CASE("cooker: wave1 PLY point cloud fails honestly (no triangle geometry)", "[cooker][wave1]")
{
    const char* ply_text = "ply\nformat ascii 1.0\nelement vertex 2\n"
                           "property float x\nproperty float y\nproperty float z\nend_header\n"
                           "0 0 0\n1 2 3\n";
    const char* src_path = "cerid_wave1_cloud.ply";
    write_text(src_path, ply_text);

    crd::cooker::CookContext ctx;
    ctx.source_path = crd::containers::StringView(src_path);
    ctx.id          = crd::resources::ResourceId::mint_random();
    ctx.allocator   = &g_alloc;

    const crd::cooker::CookResult result = wave1()(ctx);
    CHECK_FALSE(result.ok); // a mesh cook of a point cloud must FAIL loudly, not emit an empty artifact

    (void)fs::remove_file(fs::Path(crd::containers::StringView(src_path)));
}
