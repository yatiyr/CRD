#include <crd/renderer/mesh_resource.hpp>
#include <crd/renderer/mesh_resource_loader.hpp>

#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_manager.hpp>

#include <cstring>
#include <memory>

namespace crd::renderer
{

namespace
{

constexpr crd::u32 kMeshLoaderVersion = 1U;

// PRIM chunk layout (ADR-0043):
//   +0  u32 primitive_count
//   per primitive (32 bytes each):
//     +0  u32  vertex_count
//     +4  u32  index_count
//     +8  u32  vertex_byte_offset
//     +12 u32  index_byte_offset
//     +16 u8[16] material_id (u64 hi LE + u64 lo LE; all-zero = no material)
constexpr crd::u32 kPrimEntrySize  = 32U;
constexpr crd::u32 kPrimHeaderSize = 4U;

class MeshResourceLoaderImpl final : public crd::resources::ILoader
{
public:
    [[nodiscard]] crd::u32 type_fourcc() const noexcept override
    {
        return crd::resources::kFourCC_MESH;
    }

    [[nodiscard]] crd::u32 loader_version() const noexcept override
    {
        return kMeshLoaderVersion;
    }

    [[nodiscard]] void* load(const crd::resources::LoadContext& ctx) override
    {
        using namespace crd::resources;

        CrdrFile file(&m_alloc);
        if (crdr_read(ctx.bytes, file, &m_alloc) != CrdrError::Ok)
        {
            return nullptr;
        }

        const CrdrChunk* vert_chunk = crdr_find_chunk(file, kFourCC_VERT);
        if (vert_chunk == nullptr)
        {
            return nullptr;
        }

        const CrdrChunk* indx_chunk = crdr_find_chunk(file, kFourCC_INDX);
        if (indx_chunk == nullptr)
        {
            return nullptr;
        }

        const CrdrChunk* prim_chunk = crdr_find_chunk(file, kFourCC_PRIM);
        if (prim_chunk == nullptr || prim_chunk->payload.size() < kPrimHeaderSize)
        {
            return nullptr;
        }

        crd::u32 prim_count = 0;
        std::memcpy(&prim_count, prim_chunk->payload.data(), sizeof(crd::u32));

        if (prim_count == 0U)
        {
            return nullptr;
        }

        const crd::usize expected_prim_bytes =
            static_cast<crd::usize>(kPrimHeaderSize)
            + static_cast<crd::usize>(prim_count) * static_cast<crd::usize>(kPrimEntrySize);
        if (prim_chunk->payload.size() < expected_prim_bytes)
        {
            return nullptr;
        }

        void* raw = m_alloc.allocate(sizeof(MeshResource), alignof(MeshResource));
        if (raw == nullptr)
        {
            return nullptr;
        }
        auto* mesh = new (raw) MeshResource(&m_alloc);

        mesh->vertices.resize(vert_chunk->payload.size());
        std::memcpy(mesh->vertices.data(),
                    vert_chunk->payload.data(),
                    vert_chunk->payload.size());

        mesh->indices.resize(indx_chunk->payload.size());
        std::memcpy(mesh->indices.data(),
                    indx_chunk->payload.data(),
                    indx_chunk->payload.size());

        const crd::u8* p = prim_chunk->payload.data() + kPrimHeaderSize;
        for (crd::u32 pi = 0U; pi < prim_count; ++pi, p += kPrimEntrySize)
        {
            MeshPrimitive prim;
            std::memcpy(&prim.vertex_count,       p +  0, sizeof(crd::u32));
            std::memcpy(&prim.index_count,        p +  4, sizeof(crd::u32));
            std::memcpy(&prim.vertex_byte_offset, p +  8, sizeof(crd::u32));
            std::memcpy(&prim.index_byte_offset,  p + 12, sizeof(crd::u32));

            crd::u64 mat_hi = 0;
            crd::u64 mat_lo = 0;
            std::memcpy(&mat_hi, p + 16, sizeof(crd::u64));
            std::memcpy(&mat_lo, p + 24, sizeof(crd::u64));
            prim.material_id = crd::resources::ResourceId{mat_hi, mat_lo};

            mesh->primitives.push_back(prim);
        }

        return mesh;
    }

    void unload(void* payload) noexcept override
    {
        if (payload == nullptr)
        {
            return;
        }
        auto* mesh = static_cast<MeshResource*>(payload);
        mesh->~MeshResource();
        m_alloc.deallocate(mesh);
    }

private:
    // Concurrent async loads share this single loader instance; wrapper serializes the
    // single-threaded TLSF heap while keeping per-type pool locality (see texture loader).
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_alloc{&m_inner};
};

} // anonymous namespace

void register_mesh_loader(crd::resources::ResourceManager* rm)
{
    rm->register_loader(std::make_unique<MeshResourceLoaderImpl>());
}

} // namespace crd::renderer
