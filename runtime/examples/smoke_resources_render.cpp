// smoke_resources_render.cpp — Phase 2.6 v1e / Phase 2.7 v1c regression smoke.
//
// Programmatically cooks vertex + fragment GLSL shaders into SHDR artifacts,
// assembles a legacy MATR artifact (META chunk, backward-compat path), mounts
// everything in a single in-memory PACK, then loads the MaterialTemplate and
// verifies all handles reach Ready state via pass_shaders[Forward].
//
// Does NOT do actual rendering — verifies the resource loading path only.
// Exits 0 on success.

#include <crd/containers/array.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/material_resource_loader.hpp>
#include <crd/renderer/pass_type.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp> // D-008: GLSL->SPIR-V lives in the Vulkan backend (was crd::shader::compile_glsl)
#include <crd/shader/shader_resource_loader.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;
using namespace crd::resources;

static crd::memory::TlsfAllocator g_alloc{256ULL << 20};

// ── Minimal GLSL sources ──────────────────────────────────────────────────

static constexpr const char* kVertGlsl = R"glsl(
#version 450
layout(location = 0) in  vec3 inPosition;
layout(location = 0) out vec3 outColor;
void main()
{
    outColor    = vec3(1.0, 0.5, 0.0);
    gl_Position = vec4(inPosition, 1.0);
}
)glsl";

static constexpr const char* kFragGlsl = R"glsl(
#version 450
layout(location = 0) in  vec3 inColor;
layout(location = 0) out vec4 outFragColor;
void main()
{
    outFragColor = vec4(inColor, 1.0);
}
)glsl";

// ── Build helpers ─────────────────────────────────────────────────────────

static crd::containers::Array<crd::u8> build_shdr_artifact(
    ResourceId id, crd::shader::Stage stage,
    crd::containers::ConstSpan<crd::u8> spirv)
{
    const crd::u32 spirv_cc =
        (stage == crd::shader::Stage::Fragment) ? kFourCC_SPVF : kFourCC_SPVV;

    CrdrWriter writer(&g_alloc, id, kFourCC_SHDR);
    writer.add_chunk(spirv_cc, spirv);
    return writer.finish();
}

static crd::containers::Array<crd::u8> build_matr_artifact(
    ResourceId id, ResourceId vert_id, ResourceId frag_id)
{
    crd::u8 meta[32];
    std::memcpy(meta +  0, &vert_id.hi, 8);
    std::memcpy(meta +  8, &vert_id.lo, 8);
    std::memcpy(meta + 16, &frag_id.hi, 8);
    std::memcpy(meta + 24, &frag_id.lo, 8);

    CrdrWriter writer(&g_alloc, id, kFourCC_MATR);
    writer.add_chunk(kFourCC_META, crd::containers::ConstSpan<crd::u8>(meta, 32U));
    return writer.finish();
}

struct Artifact
{
    ResourceId                      id;
    crd::u32                        type_fourcc;
    crd::containers::Array<crd::u8> crdr_bytes;
    const char*                     name;
};

static fs::Path assemble_pack(crd::containers::Array<Artifact>& arts)
{
    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8>        pool(&g_alloc);
    crd::containers::Array<ManifestEntry>  entries(&g_alloc);

    for (const Artifact& art : arts)
    {
        const crd::u32 name_off = static_cast<crd::u32>(pool.size());
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
        e.name_strp_idx = name_off;
        entries.push_back(e);
    }

    // Pass 1: measure CRDR header size.
    {
        CrdrWriter p1(&g_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1,
                       crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(pool));
        const auto b1  = p1.finish();
        crd::u64 off   = static_cast<crd::u64>(b1.size());
        for (crd::usize i = 0; i < arts.size(); ++i)
        {
            entries[i].blob_offset = off;
            off += static_cast<crd::u64>(arts[i].crdr_bytes.size());
        }
    }

    // Pass 2: real offsets.
    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2,
                   crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();

    for (const Artifact& art : arts)
    {
        for (crd::usize i = 0; i < art.crdr_bytes.size(); ++i)
        {
            pack_bytes.push_back(art.crdr_bytes[i]);
        }
    }

    const auto str_id = pack_id.to_string(&g_alloc);
    crd::containers::String tmp("smoke_resources_render_", &g_alloc);
    tmp.append(str_id);
    tmp.append(".crdr");

    const fs::Path path(crd::containers::StringView(tmp.data(), tmp.size()));
    if (!fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_resources_render: failed to write pack\n");
        std::exit(1);
    }
    return path;
}

// ── main ─────────────────────────────────────────────────────────────────

int main()
{
    // Compile shaders.
    auto vert_result = crd::gpu::compile_glsl_to_spirv(
        crd::gpu::ShaderStage::Vertex,
        crd::containers::StringView(kVertGlsl, std::strlen(kVertGlsl)),
        "triangle.vert.glsl",
        &g_alloc);

    if (!vert_result.ok)
    {
        std::fprintf(stderr, "smoke_resources_render: vertex compile failed: %s\n",
                     vert_result.error_message.c_str());
        return 1;
    }

    auto frag_result = crd::gpu::compile_glsl_to_spirv(
        crd::gpu::ShaderStage::Fragment,
        crd::containers::StringView(kFragGlsl, std::strlen(kFragGlsl)),
        "triangle.frag.glsl",
        &g_alloc);

    if (!frag_result.ok)
    {
        std::fprintf(stderr, "smoke_resources_render: fragment compile failed: %s\n",
                     frag_result.error_message.c_str());
        return 1;
    }

    // Assign stable UUIDs.
    const ResourceId vert_id = ResourceId::mint_random();
    const ResourceId frag_id = ResourceId::mint_random();
    const ResourceId matr_id = ResourceId::mint_random();

    crd::containers::Array<Artifact> arts(&g_alloc);

    arts.push_back(Artifact{
        vert_id, kFourCC_SHDR,
        build_shdr_artifact(vert_id, crd::shader::Stage::Vertex,
                            crd::containers::as_const_span(vert_result.spirv)),
        "triangle.vert"
    });

    arts.push_back(Artifact{
        frag_id, kFourCC_SHDR,
        build_shdr_artifact(frag_id, crd::shader::Stage::Fragment,
                            crd::containers::as_const_span(frag_result.spirv)),
        "triangle.frag"
    });

    arts.push_back(Artifact{
        matr_id, kFourCC_MATR,
        build_matr_artifact(matr_id, vert_id, frag_id),
        "triangle.material"
    });

    const auto pack_path = assemble_pack(arts);

    ResourceManager rm(&g_alloc);
    crd::shader::register_shader_loader(&rm);
    crd::renderer::register_material_loader(&rm);

    const MountId mid = rm.mount_manifest(pack_path.generic());
    if (!mid.is_valid())
    {
        std::fprintf(stderr, "smoke_resources_render: mount_manifest failed\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    // Load material (triggers transitive shader loads).
    auto matr_handle = rm.load_sync<crd::renderer::MaterialTemplate>(matr_id);
    if (!matr_handle.is_ready())
    {
        std::fprintf(stderr, "smoke_resources_render: material load failed (state=%d)\n",
                     static_cast<int>(matr_handle.state()));
        (void)fs::remove_file(pack_path);
        return 1;
    }

    const crd::renderer::MaterialTemplate* mat = matr_handle.get();
    if (mat == nullptr)
    {
        std::fprintf(stderr, "smoke_resources_render: material payload is null\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    // Legacy META artifact synthesizes into pass_shaders[Forward].
    const auto& fwd = mat->pass_shaders[static_cast<crd::u8>(crd::renderer::PassType::Forward)];

    if (!fwd.vert.is_ready())
    {
        std::fprintf(stderr, "smoke_resources_render: Forward vert shader not ready\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (!fwd.frag.is_ready())
    {
        std::fprintf(stderr, "smoke_resources_render: Forward frag shader not ready\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    const crd::shader::ShaderResource* vert = fwd.vert.get();
    const crd::shader::ShaderResource* frag = fwd.frag.get();

    if (vert->spirv.empty() || frag->spirv.empty())
    {
        std::fprintf(stderr, "smoke_resources_render: empty SPIRV\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (vert->stage != crd::shader::Stage::Vertex)
    {
        std::fprintf(stderr, "smoke_resources_render: vertex stage mismatch\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    if (frag->stage != crd::shader::Stage::Fragment)
    {
        std::fprintf(stderr, "smoke_resources_render: fragment stage mismatch\n");
        (void)fs::remove_file(pack_path);
        return 1;
    }

    matr_handle = {};
    (void)fs::remove_file(pack_path);

    std::printf("smoke_resources_render: OK — MaterialTemplate loaded with vert+frag SPIRV "
                "(vert=%zu bytes, frag=%zu bytes)\n",
                vert->spirv.size(), frag->spirv.size());
    return 0;
}
