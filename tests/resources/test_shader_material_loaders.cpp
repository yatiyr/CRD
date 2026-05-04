#include <catch2/catch_test_macros.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/material_resource_loader.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/shader/compile.hpp>
#include <crd/shader/shader_resource_loader.hpp>

#include <cstring>

using namespace crd::resources;

static crd::memory::MallocAllocator s_alloc;

// ── Pack assembly helpers (mirror test_resource_manager.cpp) ──────────────

struct TestArt
{
    ResourceId                      id;
    crd::u32                        type_fourcc;
    crd::containers::Array<crd::u8> crdr_bytes;
    const char*                     name;
};

static crd::platform::fs::Path write_pack(crd::containers::Array<TestArt>& arts)
{
    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8>        pool(&s_alloc);
    crd::containers::Array<ManifestEntry>  entries(&s_alloc);

    for (const TestArt& art : arts)
    {
        const crd::u32 off = static_cast<crd::u32>(pool.size());
        for (const char* p = art.name; *p; ++p)
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

    {
        CrdrWriter p1(&s_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        const auto b1 = p1.finish();
        crd::u64 pos  = static_cast<crd::u64>(b1.size());
        for (crd::usize i = 0; i < arts.size(); ++i)
        {
            entries[i].blob_offset = pos;
            pos += static_cast<crd::u64>(arts[i].crdr_bytes.size());
        }
    }

    CrdrWriter p2(&s_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (const TestArt& art : arts)
    {
        for (crd::usize i = 0; i < art.crdr_bytes.size(); ++i)
        {
            pack_bytes.push_back(art.crdr_bytes[i]);
        }
    }

    const auto str_id = pack_id.to_string(&s_alloc);
    crd::containers::String tmp("test_shader_matr_", &s_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");

    const crd::platform::fs::Path path(
        crd::containers::StringView(tmp.data(), tmp.size()));
    const bool ok = crd::platform::fs::write_file_binary(
        path, crd::containers::as_const_span(pack_bytes));
    REQUIRE(ok);
    return path;
}

// Build a SHDR artifact with the given SPIRV bytes and stage FourCC.
static crd::containers::Array<crd::u8> make_shdr_artifact(
    ResourceId id, crd::u32 spirv_fourcc,
    crd::containers::ConstSpan<crd::u8> spirv)
{
    CrdrWriter w(&s_alloc, id, kFourCC_SHDR);
    w.add_chunk(spirv_fourcc, spirv);
    return w.finish();
}

// Build a MATR artifact referencing two shader UUIDs.
static crd::containers::Array<crd::u8> make_matr_artifact(
    ResourceId id, ResourceId vert_id, ResourceId frag_id)
{
    crd::u8 meta[32];
    std::memcpy(meta +  0, &vert_id.hi, 8);
    std::memcpy(meta +  8, &vert_id.lo, 8);
    std::memcpy(meta + 16, &frag_id.hi, 8);
    std::memcpy(meta + 24, &frag_id.lo, 8);

    CrdrWriter w(&s_alloc, id, kFourCC_MATR);
    w.add_chunk(kFourCC_META, crd::containers::ConstSpan<crd::u8>(meta, 32U));
    return w.finish();
}

// ── Minimal SPIRV (magic + 4 header words) — spirv-reflect may return empty ──

// A 5-word SPIRV: magic, version, generator, bound=1, schema.
// spirv-reflect may not parse it fully, but the loader should still store the bytes.
static constexpr crd::u32 kMinSpirv[] = {
    0x07230203U, // magic
    0x00010000U, // version 1.0
    0x00000000U, // generator
    0x00000001U, // bound
    0x00000000U, // schema
};

static crd::containers::ConstSpan<crd::u8> min_spirv_span()
{
    return crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(kMinSpirv),
        sizeof(kMinSpirv));
}

// ── Tests ─────────────────────────────────────────────────────────────────

TEST_CASE("ShaderResourceLoader: vertex SHDR artifact round-trip", "[resources][shader][v1e]")
{
    const ResourceId id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);
    TestArt art;
    art.id          = id;
    art.type_fourcc = kFourCC_SHDR;
    art.crdr_bytes  = make_shdr_artifact(id, kFourCC_SPVV, min_spirv_span());
    art.name        = "vert.shader";
    arts.push_back(std::move(art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::shader::ShaderResource>(id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::shader::ShaderResource* res = handle.get();
    REQUIRE(res != nullptr);
    CHECK(res->stage == crd::shader::Stage::Vertex);
    CHECK(res->spirv.size() == sizeof(kMinSpirv));
    // Verify the SPIRV bytes were copied faithfully.
    CHECK(std::memcmp(res->spirv.data(), kMinSpirv, sizeof(kMinSpirv)) == 0);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("ShaderResourceLoader: fragment SHDR artifact round-trip", "[resources][shader][v1e]")
{
    const ResourceId id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);
    TestArt art;
    art.id          = id;
    art.type_fourcc = kFourCC_SHDR;
    art.crdr_bytes  = make_shdr_artifact(id, kFourCC_SPVF, min_spirv_span());
    art.name        = "frag.shader";
    arts.push_back(std::move(art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::shader::ShaderResource>(id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::shader::ShaderResource* res = handle.get();
    REQUIRE(res != nullptr);
    CHECK(res->stage == crd::shader::Stage::Fragment);
    CHECK(res->spirv.size() == sizeof(kMinSpirv));

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("ShaderResourceLoader: missing SPIRV chunk returns Failed", "[resources][shader][v1e]")
{
    const ResourceId id = ResourceId::mint_random();

    // Artifact with no SPVV/SPVF/SPVC chunk — just a dummy META chunk.
    const crd::u8 dummy = 0;
    CrdrWriter w(&s_alloc, id, kFourCC_SHDR);
    w.add_chunk(kFourCC_META, crd::containers::ConstSpan<crd::u8>(&dummy, 1));
    crd::containers::Array<crd::u8> artifact_bytes = w.finish();

    crd::containers::Array<TestArt> arts(&s_alloc);
    TestArt art;
    art.id          = id;
    art.type_fourcc = kFourCC_SHDR;
    art.crdr_bytes  = std::move(artifact_bytes);
    art.name        = "bad.shader";
    arts.push_back(std::move(art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::shader::ShaderResource>(id);
    CHECK(handle.state() == LoadState::Failed);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("MaterialResourceLoader: loads material and resolves shader deps", "[resources][material][v1e]")
{
    const ResourceId vert_id = ResourceId::mint_random();
    const ResourceId frag_id = ResourceId::mint_random();
    const ResourceId matr_id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);

    TestArt vert_art;
    vert_art.id          = vert_id;
    vert_art.type_fourcc = kFourCC_SHDR;
    vert_art.crdr_bytes  = make_shdr_artifact(vert_id, kFourCC_SPVV, min_spirv_span());
    vert_art.name        = "vert.shader";
    arts.push_back(std::move(vert_art));

    TestArt frag_art;
    frag_art.id          = frag_id;
    frag_art.type_fourcc = kFourCC_SHDR;
    frag_art.crdr_bytes  = make_shdr_artifact(frag_id, kFourCC_SPVF, min_spirv_span());
    frag_art.name        = "frag.shader";
    arts.push_back(std::move(frag_art));

    TestArt matr_art;
    matr_art.id          = matr_id;
    matr_art.type_fourcc = kFourCC_MATR;
    matr_art.crdr_bytes  = make_matr_artifact(matr_id, vert_id, frag_id);
    matr_art.name        = "mat.material";
    arts.push_back(std::move(matr_art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MaterialResource>(matr_id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::renderer::MaterialResource* mat = handle.get();
    REQUIRE(mat != nullptr);
    CHECK(mat->vertex_shader.is_ready());
    CHECK(mat->fragment_shader.is_ready());

    const crd::shader::ShaderResource* vert = mat->vertex_shader.get();
    const crd::shader::ShaderResource* frag = mat->fragment_shader.get();
    REQUIRE(vert != nullptr);
    REQUIRE(frag != nullptr);
    CHECK(vert->stage == crd::shader::Stage::Vertex);
    CHECK(frag->stage == crd::shader::Stage::Fragment);

    // Transitive dep handles also available via direct load (cached).
    auto vert_h = rm.load_sync<crd::shader::ShaderResource>(vert_id);
    CHECK(vert_h.is_ready());
    CHECK(vert_h.get() == vert); // same block, same payload

    CHECK(rm.handle_count() == 3U); // vert + frag + material

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("MaterialResourceLoader: missing META chunk returns Failed", "[resources][material][v1e]")
{
    const ResourceId id = ResourceId::mint_random();

    const crd::u8 dummy = 0;
    CrdrWriter w(&s_alloc, id, kFourCC_MATR);
    w.add_chunk(kFourCC_BLOB, crd::containers::ConstSpan<crd::u8>(&dummy, 1));
    auto artifact_bytes = w.finish();

    crd::containers::Array<TestArt> arts(&s_alloc);
    TestArt art;
    art.id          = id;
    art.type_fourcc = kFourCC_MATR;
    art.crdr_bytes  = std::move(artifact_bytes);
    art.name        = "bad.material";
    arts.push_back(std::move(art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MaterialResource>(id);
    CHECK(handle.state() == LoadState::Failed);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("compile_glsl + ShaderResourceLoader: real SPIRV round-trip", "[resources][shader][v1e][shaderc]")
{
    static constexpr const char* kSrc = R"glsl(
#version 450
layout(location = 0) in vec3 inPos;
void main() { gl_Position = vec4(inPos, 1.0); }
)glsl";

    crd::shader::CompileResult compiled = crd::shader::compile_glsl(
        crd::shader::Stage::Vertex,
        crd::containers::StringView(kSrc, std::strlen(kSrc)),
        "test_vert",
        &s_alloc);

    if (!compiled.ok)
    {
        // shaderc not available in this environment — skip gracefully.
        WARN("shaderc unavailable: " << compiled.error_message.c_str());
        return;
    }

    REQUIRE(!compiled.spirv.empty());

    const ResourceId id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);
    TestArt art;
    art.id          = id;
    art.type_fourcc = kFourCC_SHDR;
    art.crdr_bytes  = make_shdr_artifact(
        id, kFourCC_SPVV, crd::containers::as_const_span(compiled.spirv));
    art.name        = "real.vert";
    arts.push_back(std::move(art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::shader::ShaderResource>(id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::shader::ShaderResource* res = handle.get();
    REQUIRE(res != nullptr);
    CHECK(res->stage == crd::shader::Stage::Vertex);
    CHECK(res->spirv.size() == compiled.spirv.size());
    // Reflection should have found the vertex input (inPos).
    CHECK(!res->vertex_attributes.empty());

    (void)crd::platform::fs::remove_file(path);
}
