#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/mesh_resource_loader.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstdio>
#include <cstring>

namespace fs = crd::platform::fs;
using namespace crd::resources;

int main()
{
    crd::memory::MallocAllocator alloc;

    // ── Build a MESH CRDR artifact with two primitives ─────────────────────
    //
    // Each primitive: 3 vertices (48B each), 3 u32 indices.
    // Vertex layout (zeros for simplicity except position + tangent w):
    //   bytes  0–11: float3 position (incremented per vertex)
    //   bytes 12–23: float3 normal   (0,0,1)
    //   bytes 24–31: float2 uv0      (0,0)
    //   bytes 32–47: float4 tangent  (1,0,0,1)

    constexpr crd::u32 kVc     = 3U;
    constexpr crd::u32 kIc     = 3U;
    constexpr crd::u32 kStride = 48U;
    constexpr crd::u32 kNumPrims = 2U;

    crd::containers::Array<crd::u8> vert_buf(&alloc);
    crd::containers::Array<crd::u8> indx_buf(&alloc);
    vert_buf.resize(kNumPrims * kVc * kStride);
    indx_buf.resize(kNumPrims * kIc * sizeof(crd::u32));

    for (crd::u32 pi = 0U; pi < kNumPrims; ++pi)
    {
        for (crd::u32 vi = 0U; vi < kVc; ++vi)
        {
            crd::u8* dst = vert_buf.data()
                         + (pi * kVc + vi) * kStride;
            const float pos[3]  = { static_cast<float>(vi), 0.0F, 0.0F };
            const float norm[3] = { 0.0F, 0.0F, 1.0F };
            const float tan[4]  = { 1.0F, 0.0F, 0.0F, 1.0F };
            std::memcpy(dst +  0, pos,  12U);
            std::memcpy(dst + 12, norm, 12U);
            std::memcpy(dst + 32, tan,  16U);
        }
        const crd::u32 base = pi * kVc;
        crd::u8* idst = indx_buf.data() + pi * kIc * sizeof(crd::u32);
        for (crd::u32 ii = 0U; ii < kIc; ++ii)
        {
            const crd::u32 idx = base + ii;
            std::memcpy(idst + ii * sizeof(crd::u32), &idx, sizeof(crd::u32));
        }
    }

    // PRIM chunk.
    crd::containers::Array<crd::u8> prim_buf(&alloc);
    prim_buf.resize(4U + kNumPrims * 32U);
    std::memcpy(prim_buf.data(), &kNumPrims, 4U);
    for (crd::u32 pi = 0U; pi < kNumPrims; ++pi)
    {
        crd::u8* e  = prim_buf.data() + 4U + pi * 32U;
        const crd::u32 vbo = pi * kVc * kStride;
        const crd::u32 ibo = pi * kIc * static_cast<crd::u32>(sizeof(crd::u32));
        std::memcpy(e +  0, &kVc, 4U);
        std::memcpy(e +  4, &kIc, 4U);
        std::memcpy(e +  8, &vbo, 4U);
        std::memcpy(e + 12, &ibo, 4U);
        // material_id: all-zero (null UUID)
    }

    const ResourceId mesh_id = ResourceId::mint_random();
    const ResourceId pack_id = ResourceId::mint_random();

    CrdrWriter writer(&alloc, mesh_id, kFourCC_MESH);
    writer.add_chunk(kFourCC_VERT, crd::containers::as_const_span(vert_buf));
    writer.add_chunk(kFourCC_INDX, crd::containers::as_const_span(indx_buf));
    writer.add_chunk(kFourCC_PRIM, crd::containers::as_const_span(prim_buf));
    auto mesh_bytes = writer.finish();

    // ── Assemble PACK ──────────────────────────────────────────────────────

    const char* mesh_name = "smoke.mesh";
    crd::containers::Array<crd::u8>       strp(&alloc);
    crd::containers::Array<ManifestEntry> entries(&alloc);

    const crd::u32 name_off = static_cast<crd::u32>(strp.size());
    for (const char* p = mesh_name; *p != '\0'; ++p)
    {
        strp.push_back(static_cast<crd::u8>(*p));
    }
    strp.push_back(0U);

    ManifestEntry me;
    me.id            = mesh_id;
    me.type_fourcc   = kFourCC_MESH;
    me.flags         = 0U;
    me.blob_offset   = 0U;
    me.blob_size     = static_cast<crd::u64>(mesh_bytes.size());
    me.name_strp_idx = name_off;
    entries.push_back(me);

    // Pass 1: measure header.
    {
        CrdrWriter p1(&alloc, pack_id, kFourCC_PACK);
        manifest_write(p1, crd::containers::as_const_span(entries),
                       crd::containers::as_const_span(strp));
        auto b1 = p1.finish();
        entries[0].blob_offset = static_cast<crd::u64>(b1.size());
    }

    CrdrWriter p2(&alloc, pack_id, kFourCC_PACK);
    manifest_write(p2, crd::containers::as_const_span(entries),
                   crd::containers::as_const_span(strp));
    auto pack_bytes = p2.finish();
    for (crd::u8 b : mesh_bytes)
    {
        pack_bytes.push_back(b);
    }

    const fs::Path pack_path("smoke_mesh_tmp.crdr");
    CRD_VERIFY(fs::write_file_binary(pack_path, crd::containers::as_const_span(pack_bytes)));

    // ── Load ───────────────────────────────────────────────────────────────

    ResourceManager rm(&alloc);
    crd::renderer::register_mesh_loader(&rm);
    CRD_VERIFY(rm.mount_manifest(pack_path.generic()).is_valid());

    auto handle = rm.load_sync<crd::renderer::MeshResource>(mesh_id);
    CRD_VERIFY(handle.is_ready());

    const crd::renderer::MeshResource* mesh = handle.get();
    CRD_VERIFY(mesh != nullptr);
    CRD_VERIFY(mesh->primitives.size() == kNumPrims);
    CRD_VERIFY(mesh->primitives[0].vertex_count == kVc);
    CRD_VERIFY(mesh->primitives[0].index_count  == kIc);
    CRD_VERIFY(mesh->primitives[1].vertex_count == kVc);
    CRD_VERIFY(mesh->primitives[1].index_count  == kIc);
    CRD_VERIFY(mesh->vertices.size() == kNumPrims * kVc * kStride);
    CRD_VERIFY(mesh->indices.size()  == kNumPrims * kIc * sizeof(crd::u32));

    // Verify null material UUIDs.
    CRD_VERIFY(mesh->primitives[0].material_id.is_null());
    CRD_VERIFY(mesh->primitives[1].material_id.is_null());

    (void)fs::remove_file(pack_path);

    std::printf("smoke_mesh: OK — 2 primitives loaded, vertex/index buffers correct\n");
    return 0;
}
