#pragma once

// mesh_resource.hpp — RET-3 (ADR-0105): the CPU-side cooked mesh, RE-HOMED from crd-renderer into crd-resources (the
// GPU-free home; ADR-0043's artifact contract unchanged). Upload goes through the gpu-context seam
// (`upload_storage` / the coming GEO-7 render path), never an rhi uploader. The loader's payload heap is INJECTABLE —
// a `StreamingCategoryAllocator` view puts every loaded mesh in the ADR-0085 resident store under streaming budgets.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>
#include <crd/resources/loader.hpp>
#include <crd/resources/resource_id.hpp>

namespace crd::resources
{

// Interleaved vertex stride used by all MESH CRDR artifacts (ADR-0043).
//   bytes  0–11: float3  position
//   bytes 12–23: float3  normal
//   bytes 24–31: float2  uv0
//   bytes 32–47: float4  tangent  (xyz = direction, w = bitangent sign: +1 or -1)
//
// ── Dimensional-type contract (ADR-0078 §4 D29) ──────────────────────────────────────────────────────────────────────
// MeshResource stores raw bytes; the SI-meter interpretation lives at two boundaries: the COOKER multiplies positions
// into SI metres via `.meta position_scale` (ADR-0078 §2 D18), and the RUNTIME's `scene::Transform::translation`
// (Vec3<Length32>) carries the dim tag. The byte buffer stays raw — typed access would re-tag every vertex read and
// defeat the SIMD upload path (ADR-0078 §3 D22).
inline constexpr crd::u32 kMeshVertexStride = 48U;

// One contiguous triangle-list draw range within a MeshResource.
struct MeshPrimitive
{
    crd::u32   vertex_count       = 0;
    crd::u32   index_count        = 0;
    crd::u32   vertex_byte_offset = 0; // byte offset into MeshResource::vertices
    crd::u32   index_byte_offset  = 0; // byte offset into MeshResource::indices
    ResourceId material_id;            // null UUID = no material
};

struct MeshResource
{
    crd::containers::Array<crd::u8>       vertices; // interleaved, kMeshVertexStride per vertex
    crd::containers::Array<crd::u8>       indices;  // u32 index buffer
    crd::containers::Array<MeshPrimitive> primitives;

    explicit MeshResource(crd::memory::IAllocator* a) : vertices(a), indices(a), primitives(a) {}

    MeshResource(const MeshResource&)            = delete;
    MeshResource& operator=(const MeshResource&) = delete;
    MeshResource(MeshResource&&)                 = default;
    MeshResource& operator=(MeshResource&&)      = default;
};

// ILoader for 'MESH' → MeshResource. Payload heap injectable (the texture-loader pattern; a streaming category view
// is safe under concurrent async loads — its resident path is internally serialized).
class MeshResourceLoader final : public ILoader
{
public:
    MeshResourceLoader() = default;
    explicit MeshResourceLoader(crd::memory::IAllocator* payload_alloc) noexcept
    {
        if (payload_alloc != nullptr) { m_payload = payload_alloc; }
    }

    [[nodiscard]] crd::u32 type_fourcc() const noexcept override;
    [[nodiscard]] crd::u32 loader_version() const noexcept override { return 2U; } // v2 = the crd-resources re-home
    [[nodiscard]] void*    load(const LoadContext& ctx) override;
    void                   unload(void* payload) noexcept override;

private:
    crd::memory::GrowableTlsfAllocator m_inner;
    crd::memory::ThreadSafeAllocator   m_owned{&m_inner};
    crd::memory::IAllocator*           m_payload = &m_owned;
};

class ResourceManager;

// Register the MESH loader. `payload_alloc` = nullptr → the loader's owned heap; a StreamingCategoryAllocator view →
// budgeted resident-store payloads (ADR-0085).
void register_mesh_loader(ResourceManager* rm, crd::memory::IAllocator* payload_alloc = nullptr);

} // namespace crd::resources
