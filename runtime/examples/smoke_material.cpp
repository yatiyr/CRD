// smoke_material.cpp — Phase 2.7 v1c smoke test (ADR-0048).
//
// Verifies the full material system foundation:
//   1. Hand-assembles a MATR artifact with INFO + PASS + PRMS + DFLT chunks.
//   2. Loads MaterialTemplate (two pass shader pairs + Float4 parameter).
//   3. Creates MaterialInstance, calls set_vec4, verifies values_blob.
//   4. Calls variant_for_pass for Forward and DepthPrepass, verifies handles.
//   5. Tests backward-compat: legacy META chunk → synthesized Forward entry.
//
// Exits 0 on success. No GPU or window required.

#include <crd/containers/array.hpp>
#include <crd/log/log.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/material_resource_loader.hpp>
#include <crd/renderer/material_template.hpp>
#include <crd/renderer/pass_type.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/load_state.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/shader/shader_resource_loader.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = crd::platform::fs;
using namespace crd::resources;

static crd::memory::TlsfAllocator g_alloc{256ULL << 20};

// ── Minimal SPIRV (5 words) ───────────────────────────────────────────────

static constexpr crd::u32 kMinSpirv[] = {
    0x07230203U, 0x00010000U, 0x00000000U, 0x00000001U, 0x00000000U
};

static crd::containers::ConstSpan<crd::u8> min_spirv()
{
    return crd::containers::ConstSpan<crd::u8>(
        reinterpret_cast<const crd::u8*>(kMinSpirv), sizeof(kMinSpirv));
}

// ── Artifact builders ─────────────────────────────────────────────────────

static crd::containers::Array<crd::u8> make_shdr(ResourceId id, crd::u32 stage_fourcc)
{
    CrdrWriter w(&g_alloc, id, kFourCC_SHDR);
    w.add_chunk(stage_fourcc, min_spirv());
    return w.finish();
}

// Build a v1c MATR artifact with INFO + PASS + PRMS + DFLT chunks.
// Accepts up to 2 pass entries (Forward + optional DepthPrepass).
static crd::containers::Array<crd::u8> make_matr_v2(
    ResourceId id,
    ResourceId fwd_vert,  ResourceId fwd_frag,
    ResourceId dep_vert,  ResourceId dep_frag, // both null → no DepthPrepass entry
    const crd::renderer::CookedParameter* params, crd::u32 param_count,
    const crd::u8* defaults, crd::u32 defaults_size)
{
    // INFO chunk
    crd::u8 info[4] = {2U, 0U, 0U, 0U};

    // PASS chunk
    constexpr crd::usize entry_size = 36U;
    const crd::u32 count = (!dep_vert.is_null() && !dep_frag.is_null()) ? 2U : 1U;
    crd::containers::Array<crd::u8> pass_bytes(&g_alloc);
    pass_bytes.resize(sizeof(crd::u32) + count * entry_size);
    std::memcpy(pass_bytes.data(), &count, sizeof(crd::u32));

    auto write_entry = [](crd::u8* dst, crd::u8 pt, ResourceId vert, ResourceId frag)
    {
        dst[0] = pt; dst[1] = 0U; dst[2] = 0U; dst[3] = 0U;
        std::memcpy(dst + 4,  &vert.hi, 8);
        std::memcpy(dst + 12, &vert.lo, 8);
        std::memcpy(dst + 20, &frag.hi, 8);
        std::memcpy(dst + 28, &frag.lo, 8);
    };

    write_entry(pass_bytes.data() + sizeof(crd::u32),
                static_cast<crd::u8>(crd::renderer::PassType::Forward), fwd_vert, fwd_frag);

    if (count == 2U)
    {
        write_entry(pass_bytes.data() + sizeof(crd::u32) + entry_size,
                    static_cast<crd::u8>(crd::renderer::PassType::DepthPrepass), dep_vert, dep_frag);
    }

    CrdrWriter w(&g_alloc, id, kFourCC_MATR);
    w.add_chunk(kFourCC_INFO, crd::containers::ConstSpan<crd::u8>(info, 4U));
    w.add_chunk(kFourCC_PASS, crd::containers::as_const_span(pass_bytes));

    if (param_count > 0U && params != nullptr)
    {
        crd::containers::Array<crd::u8> prms_bytes(&g_alloc);
        prms_bytes.resize(sizeof(crd::u32) + param_count * sizeof(crd::renderer::CookedParameter));
        std::memcpy(prms_bytes.data(), &param_count, sizeof(crd::u32));
        std::memcpy(prms_bytes.data() + sizeof(crd::u32),
                    params, param_count * sizeof(crd::renderer::CookedParameter));
        w.add_chunk(kFourCC_PRMS, crd::containers::as_const_span(prms_bytes));
    }

    if (defaults_size > 0U && defaults != nullptr)
    {
        w.add_chunk(kFourCC_DFLT, crd::containers::ConstSpan<crd::u8>(defaults, defaults_size));
    }

    return w.finish();
}

// Build a legacy META artifact (backward-compat path).
static crd::containers::Array<crd::u8> make_matr_meta(
    ResourceId id, ResourceId vert, ResourceId frag)
{
    crd::u8 meta[32];
    std::memcpy(meta +  0, &vert.hi, 8);
    std::memcpy(meta +  8, &vert.lo, 8);
    std::memcpy(meta + 16, &frag.hi, 8);
    std::memcpy(meta + 24, &frag.lo, 8);

    CrdrWriter w(&g_alloc, id, kFourCC_MATR);
    w.add_chunk(kFourCC_META, crd::containers::ConstSpan<crd::u8>(meta, 32U));
    return w.finish();
}

// ── Pack helpers ──────────────────────────────────────────────────────────

struct Art
{
    ResourceId id; crd::u32 type_fourcc;
    crd::containers::Array<crd::u8> bytes;
    const char* name;
};

static fs::Path make_pack(crd::containers::Array<Art>& arts, const char* label)
{
    const ResourceId pack_id = ResourceId::mint_random();

    crd::containers::Array<crd::u8>       pool(&g_alloc);
    crd::containers::Array<ManifestEntry> entries(&g_alloc);

    for (const Art& a : arts)
    {
        const crd::u32 off = static_cast<crd::u32>(pool.size());
        for (const char* p = a.name; *p; ++p) { pool.push_back(static_cast<crd::u8>(*p)); }
        pool.push_back(0U);
        ManifestEntry e;
        e.id           = a.id;
        e.type_fourcc  = a.type_fourcc;
        e.flags        = 0U;
        e.blob_offset  = 0U;
        e.blob_size    = static_cast<crd::u64>(a.bytes.size());
        e.name_strp_idx = off;
        entries.push_back(e);
    }

    // Two-pass to fix blob offsets.
    auto measure = [&]() -> crd::u64
    {
        CrdrWriter p1(&g_alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
        return static_cast<crd::u64>(p1.finish().size());
    };
    crd::u64 off = measure();
    for (crd::usize i = 0; i < arts.size(); ++i)
    {
        entries[i].blob_offset = off;
        off += static_cast<crd::u64>(arts[i].bytes.size());
    }

    CrdrWriter p2(&g_alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries), crd::containers::as_const_span(pool));
    auto pack_bytes = p2.finish();
    for (const Art& a : arts)
    {
        for (crd::u8 b : a.bytes) { pack_bytes.push_back(b); }
    }

    const auto str = pack_id.to_string(&g_alloc);
    crd::containers::String tmp("smoke_material_", &g_alloc);
    tmp.append(str); tmp.append("_"); tmp.append(label); tmp.append(".crdr");
    const fs::Path path(crd::containers::StringView(tmp.data(), tmp.size()));
    if (!fs::write_file_binary(path, crd::containers::as_const_span(pack_bytes)))
    {
        std::fprintf(stderr, "smoke_material: failed to write pack '%s'\n", label);
        std::exit(1);
    }
    return path;
}

// ── main ─────────────────────────────────────────────────────────────────

int main()
{
    // ── Test 1: v1c PASS chunk with two passes + Float4 parameter ──────────

    const ResourceId fwd_vert_id  = ResourceId::mint_random();
    const ResourceId fwd_frag_id  = ResourceId::mint_random();
    const ResourceId dep_vert_id  = ResourceId::mint_random();
    const ResourceId dep_frag_id  = ResourceId::mint_random();
    const ResourceId matr_id      = ResourceId::mint_random();

    constexpr crd::u64 param_hash = 0xABCDEF0123456789ULL;

    crd::renderer::CookedParameter param{};
    param.name_hash  = param_hash;
    param.type       = crd::renderer::ParameterType::Float4;
    param.ubo_offset = 0U;

    const crd::u8 defaults[16] = {};

    {
        crd::containers::Array<Art> arts(&g_alloc);

        arts.push_back({fwd_vert_id, kFourCC_SHDR, make_shdr(fwd_vert_id, kFourCC_SPVV), "fwd.vert"});
        arts.push_back({fwd_frag_id, kFourCC_SHDR, make_shdr(fwd_frag_id, kFourCC_SPVF), "fwd.frag"});
        arts.push_back({dep_vert_id, kFourCC_SHDR, make_shdr(dep_vert_id, kFourCC_SPVV), "dep.vert"});
        arts.push_back({dep_frag_id, kFourCC_SHDR, make_shdr(dep_frag_id, kFourCC_SPVF), "dep.frag"});
        arts.push_back({matr_id, kFourCC_MATR,
            make_matr_v2(matr_id,
                         fwd_vert_id, fwd_frag_id,
                         dep_vert_id, dep_frag_id,
                         &param, 1U, defaults, 16U),
            "mat.material"});

        const auto path = make_pack(arts, "v2");

        ResourceManager rm(&g_alloc);
        crd::shader::register_shader_loader(&rm);
        crd::renderer::register_material_loader(&rm);

        const MountId mid = rm.mount_manifest(path.generic());
        if (!mid.is_valid())
        {
            std::fprintf(stderr, "smoke_material: mount_manifest failed (test 1)\n");
            (void)fs::remove_file(path);
            return 1;
        }

        auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(matr_id);
        if (!handle.is_ready())
        {
            std::fprintf(stderr, "smoke_material: MaterialTemplate load failed (test 1)\n");
            (void)fs::remove_file(path);
            return 1;
        }

        const crd::renderer::MaterialTemplate* mat = handle.get();
        const auto fwd_idx   = static_cast<crd::u8>(crd::renderer::PassType::Forward);
        const auto depth_idx = static_cast<crd::u8>(crd::renderer::PassType::DepthPrepass);

        if (!mat->pass_shaders[fwd_idx].vert.is_ready() ||
            !mat->pass_shaders[fwd_idx].frag.is_ready())
        {
            std::fprintf(stderr, "smoke_material: Forward pass shaders not ready\n");
            (void)fs::remove_file(path);
            return 1;
        }

        if (!mat->pass_shaders[depth_idx].vert.is_ready() ||
            !mat->pass_shaders[depth_idx].frag.is_ready())
        {
            std::fprintf(stderr, "smoke_material: DepthPrepass shaders not ready\n");
            (void)fs::remove_file(path);
            return 1;
        }

        if (mat->parameters.size() != 1U ||
            mat->parameters[0].name_hash != param_hash ||
            mat->parameters[0].type != crd::renderer::ParameterType::Float4)
        {
            std::fprintf(stderr, "smoke_material: parameter schema mismatch\n");
            (void)fs::remove_file(path);
            return 1;
        }

        // Create MaterialInstance, set_vec4, verify values_blob.
        crd::renderer::MaterialInstance inst(handle, &g_alloc);
        inst.set_vec4(param_hash, 1.0F, 2.0F, 3.0F, 4.0F);

        const auto& blob = inst.values_blob();
        if (blob.size() < 16U)
        {
            std::fprintf(stderr, "smoke_material: values_blob too small (%zu)\n", blob.size());
            (void)fs::remove_file(path);
            return 1;
        }

        float v[4];
        std::memcpy(v, blob.data(), sizeof(v));
        if (v[0] != 1.0F || v[1] != 2.0F || v[2] != 3.0F || v[3] != 4.0F)
        {
            std::fprintf(stderr, "smoke_material: values_blob wrong values %.1f %.1f %.1f %.1f\n",
                         static_cast<double>(v[0]), static_cast<double>(v[1]),
                         static_cast<double>(v[2]), static_cast<double>(v[3]));
            (void)fs::remove_file(path);
            return 1;
        }

        // variant_for_pass: Forward → Forward pair.
        const auto& fwd_pair = inst.variant_for_pass(crd::renderer::PassType::Forward);
        if (!fwd_pair.vert.is_ready() || !fwd_pair.frag.is_ready())
        {
            std::fprintf(stderr, "smoke_material: variant_for_pass(Forward) not ready\n");
            (void)fs::remove_file(path);
            return 1;
        }

        // variant_for_pass: DepthPrepass → DepthPrepass pair.
        const auto& dep_pair = inst.variant_for_pass(crd::renderer::PassType::DepthPrepass);
        if (!dep_pair.vert.is_ready() || !dep_pair.frag.is_ready())
        {
            std::fprintf(stderr, "smoke_material: variant_for_pass(DepthPrepass) not ready\n");
            (void)fs::remove_file(path);
            return 1;
        }

        // Distinct shader objects for different passes.
        if (fwd_pair.vert.get() == dep_pair.vert.get())
        {
            std::fprintf(stderr, "smoke_material: Forward and DepthPrepass share vert shader\n");
            (void)fs::remove_file(path);
            return 1;
        }

        (void)fs::remove_file(path);
        std::printf("smoke_material: test 1 (v2 PASS chunk + MaterialInstance) OK\n");
    }

    // ── Test 2: legacy META backward-compat ────────────────────────────────

    {
        const ResourceId meta_vert = ResourceId::mint_random();
        const ResourceId meta_frag = ResourceId::mint_random();
        const ResourceId meta_matr = ResourceId::mint_random();

        crd::containers::Array<Art> arts(&g_alloc);
        arts.push_back({meta_vert, kFourCC_SHDR, make_shdr(meta_vert, kFourCC_SPVV), "mv.shader"});
        arts.push_back({meta_frag, kFourCC_SHDR, make_shdr(meta_frag, kFourCC_SPVF), "mf.shader"});
        arts.push_back({meta_matr, kFourCC_MATR,
            make_matr_meta(meta_matr, meta_vert, meta_frag), "legacy.material"});

        const auto path = make_pack(arts, "meta");

        ResourceManager rm(&g_alloc);
        crd::shader::register_shader_loader(&rm);
        crd::renderer::register_material_loader(&rm);

        const MountId mid = rm.mount_manifest(path.generic());
        if (!mid.is_valid())
        {
            std::fprintf(stderr, "smoke_material: mount_manifest failed (test 2)\n");
            (void)fs::remove_file(path);
            return 1;
        }

        auto handle = rm.load_sync<crd::renderer::MaterialTemplate>(meta_matr);
        if (!handle.is_ready())
        {
            std::fprintf(stderr, "smoke_material: legacy META load failed\n");
            (void)fs::remove_file(path);
            return 1;
        }

        const crd::renderer::MaterialTemplate* mat = handle.get();
        const auto fwd_idx = static_cast<crd::u8>(crd::renderer::PassType::Forward);

        if (!mat->pass_shaders[fwd_idx].vert.is_ready() ||
            !mat->pass_shaders[fwd_idx].frag.is_ready())
        {
            std::fprintf(stderr, "smoke_material: legacy META Forward pair not ready\n");
            (void)fs::remove_file(path);
            return 1;
        }

        // variant_for_pass(DepthPrepass) → should fall back to Forward.
        crd::renderer::MaterialInstance inst(handle, &g_alloc);
        const auto& dep_pair = inst.variant_for_pass(crd::renderer::PassType::DepthPrepass);
        if (!dep_pair.vert.is_ready())
        {
            std::fprintf(stderr, "smoke_material: fallback for missing DepthPrepass failed\n");
            (void)fs::remove_file(path);
            return 1;
        }

        (void)fs::remove_file(path);
        std::printf("smoke_material: test 2 (legacy META backward-compat) OK\n");
    }

    std::printf("smoke_material: all tests passed\n");
    return 0;
}
