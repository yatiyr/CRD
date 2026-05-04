#include <catch2/catch_test_macros.hpp>

#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/material_resource_loader.hpp>
#include <crd/renderer/material_template.hpp>
#include <crd/renderer/pass_type.hpp>
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

// Build a MATR artifact with INFO + PASS chunks (v1c format).
// Each entry in `pass_entries` is {pass_type, vert_id, frag_id}.
struct PassEntry
{
    crd::u8                    pass_type;
    crd::resources::ResourceId vert_id;
    crd::resources::ResourceId frag_id;
};

static crd::containers::Array<crd::u8> make_matr_v2_artifact(
    ResourceId id,
    crd::containers::ConstSpan<PassEntry> entries,
    crd::containers::ConstSpan<crd::renderer::CookedParameter> params = {},
    crd::containers::ConstSpan<crd::u8> defaults = {})
{
    // INFO chunk (4 bytes)
    crd::u8 info[4] = {2U, 0U, 0U, 0U}; // version=2, domain=Surface

    // PASS chunk
    const crd::u32 count = static_cast<crd::u32>(entries.size());
    constexpr crd::usize kEntrySize = 36U;
    crd::containers::Array<crd::u8> pass_bytes(&s_alloc);
    pass_bytes.resize(sizeof(crd::u32) + count * kEntrySize);
    std::memcpy(pass_bytes.data(), &count, sizeof(crd::u32));
    for (crd::u32 i = 0; i < count; ++i)
    {
        crd::u8* e = pass_bytes.data() + sizeof(crd::u32) + i * kEntrySize;
        e[0] = entries[i].pass_type;
        e[1] = 0U; e[2] = 0U; e[3] = 0U;
        std::memcpy(e + 4,  &entries[i].vert_id.hi, 8);
        std::memcpy(e + 12, &entries[i].vert_id.lo, 8);
        std::memcpy(e + 20, &entries[i].frag_id.hi, 8);
        std::memcpy(e + 28, &entries[i].frag_id.lo, 8);
    }

    CrdrWriter w(&s_alloc, id, kFourCC_MATR);
    w.add_chunk(kFourCC_INFO, crd::containers::ConstSpan<crd::u8>(info, 4U));
    w.add_chunk(kFourCC_PASS, crd::containers::as_const_span(pass_bytes));

    if (!params.empty())
    {
        crd::containers::Array<crd::u8> prms_bytes(&s_alloc);
        const crd::u32 pc = static_cast<crd::u32>(params.size());
        prms_bytes.resize(sizeof(crd::u32) + pc * sizeof(crd::renderer::CookedParameter));
        std::memcpy(prms_bytes.data(), &pc, sizeof(crd::u32));
        std::memcpy(prms_bytes.data() + sizeof(crd::u32),
                    params.data(),
                    pc * sizeof(crd::renderer::CookedParameter));
        w.add_chunk(kFourCC_PRMS, crd::containers::as_const_span(prms_bytes));
    }

    if (!defaults.empty())
    {
        w.add_chunk(kFourCC_DFLT, defaults);
    }

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

    auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(matr_id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::renderer::MaterialTemplate* mat = handle.get();
    REQUIRE(mat != nullptr);

    // Legacy META artifact → synthesized into pass_shaders[Forward].
    const auto fwd_idx = static_cast<crd::u8>(crd::renderer::PassType::Forward);
    CHECK(mat->pass_shaders[fwd_idx].vert.is_ready());
    CHECK(mat->pass_shaders[fwd_idx].frag.is_ready());

    const crd::shader::ShaderResource* vert = mat->pass_shaders[fwd_idx].vert.get();
    const crd::shader::ShaderResource* frag = mat->pass_shaders[fwd_idx].frag.get();
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

TEST_CASE("MaterialResourceLoader: missing PASS and META chunks returns Failed", "[resources][material][v1e]")
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

    auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(id);
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

// =============================================================================
// MaterialTemplate v1c tests — PASS chunk, MaterialInstance (ADR-0048)
// =============================================================================

TEST_CASE("MaterialResourceLoader: PASS chunk artifact loads Forward shader pair", "[resources][material][v1c]")
{
    const ResourceId vert_id = ResourceId::mint_random();
    const ResourceId frag_id = ResourceId::mint_random();
    const ResourceId matr_id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);

    TestArt vert_art;
    vert_art.id          = vert_id;
    vert_art.type_fourcc = kFourCC_SHDR;
    vert_art.crdr_bytes  = make_shdr_artifact(vert_id, kFourCC_SPVV, min_spirv_span());
    vert_art.name        = "v.shader";
    arts.push_back(std::move(vert_art));

    TestArt frag_art;
    frag_art.id          = frag_id;
    frag_art.type_fourcc = kFourCC_SHDR;
    frag_art.crdr_bytes  = make_shdr_artifact(frag_id, kFourCC_SPVF, min_spirv_span());
    frag_art.name        = "f.shader";
    arts.push_back(std::move(frag_art));

    const PassEntry entries[] = {
        {static_cast<crd::u8>(crd::renderer::PassType::Forward), vert_id, frag_id}
    };

    TestArt matr_art;
    matr_art.id          = matr_id;
    matr_art.type_fourcc = kFourCC_MATR;
    matr_art.crdr_bytes  = make_matr_v2_artifact(
        matr_id, crd::containers::ConstSpan<PassEntry>(entries, 1U));
    matr_art.name        = "mat.material";
    arts.push_back(std::move(matr_art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(matr_id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::renderer::MaterialTemplate* mat = handle.get();
    REQUIRE(mat != nullptr);

    const auto fwd_idx = static_cast<crd::u8>(crd::renderer::PassType::Forward);
    CHECK(mat->pass_shaders[fwd_idx].vert.is_ready());
    CHECK(mat->pass_shaders[fwd_idx].frag.is_ready());
    CHECK(mat->pass_shaders[fwd_idx].vert.get()->stage == crd::shader::Stage::Vertex);
    CHECK(mat->pass_shaders[fwd_idx].frag.get()->stage == crd::shader::Stage::Fragment);
    CHECK(mat->domain == crd::renderer::MaterialDomain::Surface);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("MaterialResourceLoader: PASS chunk with DepthPrepass loads two pairs", "[resources][material][v1c]")
{
    const ResourceId v_fwd  = ResourceId::mint_random();
    const ResourceId f_fwd  = ResourceId::mint_random();
    const ResourceId v_dep  = ResourceId::mint_random();
    const ResourceId f_dep  = ResourceId::mint_random();
    const ResourceId mat_id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);

    auto push_shader = [&](ResourceId id, crd::u32 fourcc)
    {
        TestArt art;
        art.id          = id;
        art.type_fourcc = kFourCC_SHDR;
        art.crdr_bytes  = make_shdr_artifact(id, fourcc, min_spirv_span());
        art.name        = "s.shader";
        arts.push_back(std::move(art));
    };

    push_shader(v_fwd, kFourCC_SPVV);
    push_shader(f_fwd, kFourCC_SPVF);
    push_shader(v_dep, kFourCC_SPVV);
    push_shader(f_dep, kFourCC_SPVF);

    const PassEntry entries[] = {
        {static_cast<crd::u8>(crd::renderer::PassType::Forward),      v_fwd, f_fwd},
        {static_cast<crd::u8>(crd::renderer::PassType::DepthPrepass), v_dep, f_dep},
    };

    TestArt matr_art;
    matr_art.id          = mat_id;
    matr_art.type_fourcc = kFourCC_MATR;
    matr_art.crdr_bytes  = make_matr_v2_artifact(
        mat_id, crd::containers::ConstSpan<PassEntry>(entries, 2U));
    matr_art.name        = "mat2.material";
    arts.push_back(std::move(matr_art));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(mat_id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::renderer::MaterialTemplate* mat = handle.get();
    REQUIRE(mat != nullptr);

    const auto fwd_idx   = static_cast<crd::u8>(crd::renderer::PassType::Forward);
    const auto depth_idx = static_cast<crd::u8>(crd::renderer::PassType::DepthPrepass);

    CHECK(mat->pass_shaders[fwd_idx].vert.is_ready());
    CHECK(mat->pass_shaders[fwd_idx].frag.is_ready());
    CHECK(mat->pass_shaders[depth_idx].vert.is_ready());
    CHECK(mat->pass_shaders[depth_idx].frag.is_ready());

    // Different shader blocks for different passes.
    CHECK(mat->pass_shaders[fwd_idx].vert.get() != mat->pass_shaders[depth_idx].vert.get());

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("MaterialTemplate: PRMS chunk populates parameter schema", "[resources][material][v1c]")
{
    const ResourceId vert_id = ResourceId::mint_random();
    const ResourceId frag_id = ResourceId::mint_random();
    const ResourceId matr_id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);

    TestArt va;
    va.id = vert_id; va.type_fourcc = kFourCC_SHDR;
    va.crdr_bytes = make_shdr_artifact(vert_id, kFourCC_SPVV, min_spirv_span());
    va.name = "v.shader";
    arts.push_back(std::move(va));

    TestArt fa;
    fa.id = frag_id; fa.type_fourcc = kFourCC_SHDR;
    fa.crdr_bytes = make_shdr_artifact(frag_id, kFourCC_SPVF, min_spirv_span());
    fa.name = "f.shader";
    arts.push_back(std::move(fa));

    crd::renderer::CookedParameter param{};
    param.name_hash  = 0xDEADBEEFCAFEBABEULL;
    param.type       = crd::renderer::ParameterType::Float4;
    param.ubo_offset = 0U;

    const crd::u8 defaults[16] = {};

    const PassEntry entries[] = {
        {static_cast<crd::u8>(crd::renderer::PassType::Forward), vert_id, frag_id}
    };
    const crd::renderer::CookedParameter params_span[] = {param};

    TestArt ma;
    ma.id          = matr_id;
    ma.type_fourcc = kFourCC_MATR;
    ma.crdr_bytes  = make_matr_v2_artifact(
        matr_id,
        crd::containers::ConstSpan<PassEntry>(entries, 1U),
        crd::containers::ConstSpan<crd::renderer::CookedParameter>(params_span, 1U),
        crd::containers::ConstSpan<crd::u8>(defaults, 16U));
    ma.name = "mat3.material";
    arts.push_back(std::move(ma));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(matr_id);
    REQUIRE(handle.state() == LoadState::Ready);

    const crd::renderer::MaterialTemplate* mat = handle.get();
    REQUIRE(mat != nullptr);
    REQUIRE(mat->parameters.size() == 1U);
    CHECK(mat->parameters[0].name_hash == 0xDEADBEEFCAFEBABEULL);
    CHECK(mat->parameters[0].type      == crd::renderer::ParameterType::Float4);
    CHECK(mat->parameters[0].ubo_offset == 0U);
    CHECK(mat->defaults_blob.size() == 16U);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("MaterialInstance: set_vec4 writes to values_blob at ubo_offset", "[resources][material][v1c]")
{
    const ResourceId vert_id = ResourceId::mint_random();
    const ResourceId frag_id = ResourceId::mint_random();
    const ResourceId matr_id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);

    TestArt va;
    va.id = vert_id; va.type_fourcc = kFourCC_SHDR;
    va.crdr_bytes = make_shdr_artifact(vert_id, kFourCC_SPVV, min_spirv_span());
    va.name = "v.shader";
    arts.push_back(std::move(va));

    TestArt fa;
    fa.id = frag_id; fa.type_fourcc = kFourCC_SHDR;
    fa.crdr_bytes = make_shdr_artifact(frag_id, kFourCC_SPVF, min_spirv_span());
    fa.name = "f.shader";
    arts.push_back(std::move(fa));

    crd::renderer::CookedParameter param{};
    param.name_hash  = 0x1122334455667788ULL;
    param.type       = crd::renderer::ParameterType::Float4;
    param.ubo_offset = 0U;

    const crd::u8 defaults[16] = {};

    const PassEntry entries[] = {
        {static_cast<crd::u8>(crd::renderer::PassType::Forward), vert_id, frag_id}
    };
    const crd::renderer::CookedParameter params_span[] = {param};

    TestArt ma;
    ma.id          = matr_id;
    ma.type_fourcc = kFourCC_MATR;
    ma.crdr_bytes  = make_matr_v2_artifact(
        matr_id,
        crd::containers::ConstSpan<PassEntry>(entries, 1U),
        crd::containers::ConstSpan<crd::renderer::CookedParameter>(params_span, 1U),
        crd::containers::ConstSpan<crd::u8>(defaults, 16U));
    ma.name = "mat4.material";
    arts.push_back(std::move(ma));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(matr_id);
    REQUIRE(handle.state() == LoadState::Ready);

    crd::renderer::MaterialInstance inst(handle, &s_alloc);
    inst.set_vec4(0x1122334455667788ULL, 1.0F, 2.0F, 3.0F, 4.0F);

    const auto& blob = inst.values_blob();
    REQUIRE(blob.size() >= 16U);

    float v[4];
    std::memcpy(v, blob.data(), sizeof(v));
    CHECK(v[0] == 1.0F);
    CHECK(v[1] == 2.0F);
    CHECK(v[2] == 3.0F);
    CHECK(v[3] == 4.0F);

    (void)crd::platform::fs::remove_file(path);
}

TEST_CASE("MaterialInstance: variant_for_pass falls back to Forward for missing DepthPrepass", "[resources][material][v1c]")
{
    const ResourceId vert_id = ResourceId::mint_random();
    const ResourceId frag_id = ResourceId::mint_random();
    const ResourceId matr_id = ResourceId::mint_random();

    crd::containers::Array<TestArt> arts(&s_alloc);

    TestArt va;
    va.id = vert_id; va.type_fourcc = kFourCC_SHDR;
    va.crdr_bytes = make_shdr_artifact(vert_id, kFourCC_SPVV, min_spirv_span());
    va.name = "v.shader";
    arts.push_back(std::move(va));

    TestArt fa;
    fa.id = frag_id; fa.type_fourcc = kFourCC_SHDR;
    fa.crdr_bytes = make_shdr_artifact(frag_id, kFourCC_SPVF, min_spirv_span());
    fa.name = "f.shader";
    arts.push_back(std::move(fa));

    // Only Forward pass — no DepthPrepass entry.
    const PassEntry entries[] = {
        {static_cast<crd::u8>(crd::renderer::PassType::Forward), vert_id, frag_id}
    };

    TestArt ma;
    ma.id          = matr_id;
    ma.type_fourcc = kFourCC_MATR;
    ma.crdr_bytes  = make_matr_v2_artifact(
        matr_id, crd::containers::ConstSpan<PassEntry>(entries, 1U));
    ma.name = "mat5.material";
    arts.push_back(std::move(ma));

    const auto path = write_pack(arts);

    ResourceManager rm(&s_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);
    REQUIRE(rm.mount_manifest(path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(matr_id);
    REQUIRE(handle.state() == LoadState::Ready);

    crd::renderer::MaterialInstance inst(handle, &s_alloc);

    // Requesting DepthPrepass — no such shader → should fall back to Forward.
    const auto& pair = inst.variant_for_pass(crd::renderer::PassType::DepthPrepass);
    CHECK(pair.vert.is_ready());
    CHECK(pair.frag.is_ready());

    // Same handles as the Forward pair.
    const auto fwd_idx = static_cast<crd::u8>(crd::renderer::PassType::Forward);
    const auto* mat = handle.get();
    CHECK(pair.vert.get() == mat->pass_shaders[fwd_idx].vert.get());

    (void)crd::platform::fs::remove_file(path);
}
