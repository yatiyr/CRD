// mesh_resource_loader.cpp — RET-3: the MESH loader, re-homed from crd-renderer (ADR-0105). See mesh_resource.hpp.

#include <crd/resources/mesh_resource.hpp>

#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>
#include <new>

namespace crd::resources
{

namespace
{
// PRIM chunk layout (ADR-0043):
//   +0  u32 primitive_count
//   per primitive (32 bytes each): u32 vertex_count · u32 index_count · u32 vertex_byte_offset ·
//   u32 index_byte_offset · u8[16] material_id (u64 hi LE + u64 lo LE; all-zero = no material)
constexpr crd::u32 kPrimEntrySize  = 32U;
constexpr crd::u32 kPrimHeaderSize = 4U;
constexpr crd::u32 kSkinVertexSize = 24U; // GEO-8: 4×u16 joints + 4×f32 weights
} // namespace

crd::u32 MeshResourceLoader::type_fourcc() const noexcept { return kFourCC_MESH; }

void* MeshResourceLoader::load(const LoadContext& ctx)
{
    // parse SCRATCH on the owned heap; only the RESIDENT payload charges m_payload (the streaming-category rule)
    CrdrFile file(&m_owned);
    if (crdr_read(ctx.bytes, file, &m_owned) != CrdrError::Ok) { return nullptr; }

    const CrdrChunk* vert_chunk = crdr_find_chunk(file, kFourCC_VERT);
    if (vert_chunk == nullptr) { return nullptr; }
    const CrdrChunk* indx_chunk = crdr_find_chunk(file, kFourCC_INDX);
    if (indx_chunk == nullptr) { return nullptr; }
    const CrdrChunk* prim_chunk = crdr_find_chunk(file, kFourCC_PRIM);
    if (prim_chunk == nullptr || prim_chunk->payload.size() < kPrimHeaderSize) { return nullptr; }

    crd::u32 prim_count = 0;
    std::memcpy(&prim_count, prim_chunk->payload.data(), sizeof(crd::u32));
    if (prim_count == 0U) { return nullptr; }
    const crd::usize expected_prim_bytes =
        static_cast<crd::usize>(kPrimHeaderSize)
        + static_cast<crd::usize>(prim_count) * static_cast<crd::usize>(kPrimEntrySize);
    if (prim_chunk->payload.size() < expected_prim_bytes) { return nullptr; }

    void* raw = m_payload->try_allocate(sizeof(MeshResource), alignof(MeshResource));
    if (raw == nullptr) { return nullptr; } // over-budget on a streaming heap — graceful, never fatal
    auto* mesh = new (raw) MeshResource(m_payload);

    mesh->vertices.resize(vert_chunk->payload.size());
    std::memcpy(mesh->vertices.data(), vert_chunk->payload.data(), vert_chunk->payload.size());
    mesh->indices.resize(indx_chunk->payload.size());
    std::memcpy(mesh->indices.data(), indx_chunk->payload.data(), indx_chunk->payload.size());

    // GEO-7: the local AABB in ONE pass over the position stream (bytes 0-11 of each 48-byte record) — the
    // culling substrate every consumer would otherwise recompute
    const crd::usize vcount = mesh->vertices.size() / kMeshVertexStride;
    if (vcount > 0U)
    {
        crd::f32 mn[3] = {0, 0, 0};
        crd::f32 mx[3] = {0, 0, 0};
        std::memcpy(mn, mesh->vertices.data(), 12U);
        std::memcpy(mx, mesh->vertices.data(), 12U);
        for (crd::usize v = 1; v < vcount; ++v)
        {
            crd::f32 pos[3];
            std::memcpy(pos, mesh->vertices.data() + v * kMeshVertexStride, 12U);
            for (int c = 0; c < 3; ++c)
            {
                mn[c] = pos[c] < mn[c] ? pos[c] : mn[c];
                mx[c] = pos[c] > mx[c] ? pos[c] : mx[c];
            }
        }
        std::memcpy(mesh->bounds_min, mn, 12U);
        std::memcpy(mesh->bounds_max, mx, 12U);
    }

    // GEO-8: the optional SKIN stream — refused (not silently dropped) when its vertex count mismatches
    if (const CrdrChunk* skin_chunk = crdr_find_chunk(file, kFourCC_SKNV); skin_chunk != nullptr)
    {
        if (skin_chunk->payload.size() != vcount * kSkinVertexSize)
        {
            mesh->~MeshResource();
            m_payload->deallocate(mesh);
            return nullptr;
        }
        mesh->skin_joints.resize(vcount * 4U);
        mesh->skin_weights.resize(vcount * 4U);
        const crd::u8* sp = skin_chunk->payload.data();
        for (crd::usize v = 0; v < vcount; ++v, sp += kSkinVertexSize)
        {
            std::memcpy(mesh->skin_joints.data() + v * 4U, sp, 8U);
            std::memcpy(mesh->skin_weights.data() + v * 4U, sp + 8U, 16U);
        }
    }

    const crd::u8* p = prim_chunk->payload.data() + kPrimHeaderSize;
    for (crd::u32 pi = 0U; pi < prim_count; ++pi, p += kPrimEntrySize)
    {
        MeshPrimitive prim;
        std::memcpy(&prim.vertex_count, p + 0, sizeof(crd::u32));
        std::memcpy(&prim.index_count, p + 4, sizeof(crd::u32));
        std::memcpy(&prim.vertex_byte_offset, p + 8, sizeof(crd::u32));
        std::memcpy(&prim.index_byte_offset, p + 12, sizeof(crd::u32));
        crd::u64 mat_hi = 0;
        crd::u64 mat_lo = 0;
        std::memcpy(&mat_hi, p + 16, sizeof(crd::u64));
        std::memcpy(&mat_lo, p + 24, sizeof(crd::u64));
        prim.material_id = ResourceId{mat_hi, mat_lo};
        mesh->primitives.push_back(prim);
    }
    return mesh;
}

void MeshResourceLoader::unload(void* payload) noexcept
{
    if (payload == nullptr) { return; }
    auto* mesh = static_cast<MeshResource*>(payload);
    mesh->~MeshResource();
    m_payload->deallocate(mesh);
}

void register_mesh_loader(ResourceManager* rm, crd::memory::IAllocator* payload_alloc)
{
    rm->register_loader(std::make_unique<MeshResourceLoader>(payload_alloc));
}

} // namespace crd::resources
