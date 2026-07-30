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

// ⭐⭐ REN-40-C1: ONE LEVEL OF THE MESH'S LOD CHAIN.
// ⛔ A range into the SAME index buffer, with ABSOLUTE vertex indices into the same vertex stream. A decimated
// level has NEW vertices (`v_opt` is a solved optimum, not one of the endpoints), so each level appends its own
// vertex block and its indices point at it directly — which is what keeps `base_vertex` unrepresentable, the
// `IRasterContext::IndexedDraw` contract (Vulkan folds firstInstance into gl_InstanceIndex, D3D12's SV_InstanceID
// does not, so a non-zero base would make the two backends read different instances).
// ⛔ POD only — `crd-resources` stays math-module-free; consumers build their own types at the seam.
struct MeshLod
{
    crd::u32 first_index  = 0U;   // u32 index of the level's first index, into MeshResource::indices
    crd::u32 index_count  = 0U;   // indices in the level (3 per triangle)
    crd::f32 error        = 0.0F; // the decimator's quadric error at this level, in OBJECT units
    crd::f32 screen_height = 0.0F; // pick this level while the instance's projected height (px) is BELOW this
};

struct MeshResource
{
    crd::containers::Array<crd::u8>       vertices; // interleaved, kMeshVertexStride per vertex
    crd::containers::Array<crd::u8>       indices;  // u32 index buffer
    crd::containers::Array<MeshPrimitive> primitives;

    // GEO-7 (appended): the mesh's LOCAL-space AABB, computed by the loader in one pass over the position stream
    // (bytes 0-11 of every 48-byte record) — the culling substrate. Raw floats deliberately: crd-resources stays
    // math-module-free; consumers (crd-scene-render) build their AABB3 from these at the seam. min > max (the
    // default) = no vertices / not computed.
    crd::f32 bounds_min[3] = {1.0F, 1.0F, 1.0F};
    crd::f32 bounds_max[3] = {-1.0F, -1.0F, -1.0F};
    [[nodiscard]] bool has_bounds() const noexcept { return bounds_min[0] <= bounds_max[0]; }

    // GEO-8 (appended): the optional SKIN vertex stream ('SKNV' chunk — 4×u16 joints + 4×f32 weights per vertex,
    // 24 bytes). Joint indices are TOPOLOGICAL skeleton indices (the cook remapped them); empty = unskinned.
    crd::containers::Array<crd::u16> skin_joints;  // 4 per vertex
    crd::containers::Array<crd::f32> skin_weights; // 4 per vertex

    // ⭐⭐ REN-40-C1: the LOD CHAIN, coarsest last. EMPTY = a one-level mesh, which is why every existing asset
    // keeps rendering exactly as it did — the renderer reads `lods[0]` when the chain exists and the whole index
    // buffer when it does not. Appended at the END (the append-only discipline this struct already follows).
    [[nodiscard]] bool has_skin() const noexcept
    {
        const crd::usize vc = vertices.size() / kMeshVertexStride;
        return vc > 0U && skin_joints.size() == vc * 4U && skin_weights.size() == vc * 4U;
    }

    crd::containers::Array<MeshLod> lods;
    [[nodiscard]] bool has_lods() const noexcept { return lods.size() > 1U; }

    explicit MeshResource(crd::memory::IAllocator* a) : vertices(a), indices(a), primitives(a), lods(a) {}

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
