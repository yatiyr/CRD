#pragma once

// scene_renderer.hpp — GEO-7 (D-007 row 72): scene ↔ ECS ↔ renderer INTEGRATION on the ONE graphics layer
// (ADR-0105). The world-class ECS earns its keep: extraction, change tracking, culling, and submission all walk
// ARCHETYPE CHUNKS (SoA) through the GEO-7 ChunkView table — never per-entity handle chasing on the render path.
//
// The pipeline, per frame:
//   sync(world)    — chunk-grain extract of (Transform, MeshRenderer): a structural change (spawn/despawn/archetype
//                    move) rebuilds the instance tables; otherwise ONLY chunks whose Transform chunk-version moved
//                    re-extract, and ONLY their byte ranges re-upload (the change-detection partial re-upload gate).
//   render(target) — frustum culling (6 world-space planes from view_proj; an optional SpatialBVHIndex prunes the
//                    broad phase — the crd-geometry LooseOctree under ADR-0053's index slot) → per mesh group ONE
//                    vertex-pulling instanced draw (`draw_storage_depth`): the CKIR VS fetches index → vertex →
//                    instance matrix from the group's storage buffer by VertexIndex (the GEO-1 idiom, scaled to
//                    instancing), the FS lights N·L + ambient with the instance's material colour.
//
// Data contract per mesh group — ONE u32 storage buffer at set 0 / binding 0 (VERTEX+FRAGMENT visible):
//   words [0..31]  HEADER: [0] index_count · [1] visible_count · [2] indices_off · [3] vertices_off ·
//                  [4] instances_off · [5] visible_off (all in words) · [6..21] view_proj (column-major f32 bits) ·
//                  [22..24] light_dir · [25..31] reserved
//   [indices_off..)   u32 indices
//   [vertices_off..)  12 words per vertex (the cooked 48-byte layout: pos3 · normal3 · uv2 · tangent4)
//   [instances_off..) 20 words per instance: world matrix (column-major, 16) + linear colour (4)
//   [visible_off..)   u32 visible slots (per-frame, culled)
// Geometry uploads ONCE per group; instances upload by dirty run; header + visible list upload per frame.

#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/resources/mesh_resource.hpp>
#include <crd/resources/resource_handle.hpp>
#include <crd/resources/resource_id.hpp>
#include <crd/scene/entity.hpp>

#include <memory>

namespace crd::gpu
{
class IGpuContext;
}
namespace crd::resources
{
class ResourceManager;
}
namespace crd::scene
{
class World;
class SpatialBVHIndex;
}

namespace crd::scenerender
{

inline constexpr crd::u32 kHeaderWords        = 32U;
inline constexpr crd::u32 kVertexWords        = 12U; // the cooked 48-byte vertex
inline constexpr crd::u32 kInstanceWords      = 20U; // world matrix 16 + colour 4

// One per-instance GPU record (kInstanceWords * 4 bytes).
struct InstanceGpu
{
    crd::f32 world[16]; // column-major
    crd::f32 color[4];  // linear RGBA
};
static_assert(sizeof(InstanceGpu) == kInstanceWords * 4U, "GPU layout pinned");

// One chunk's contiguous run inside a group's instance array — the partial-re-upload grain (the ECS is
// chunk-grain; so is the renderer's dirt).
struct ChunkRun
{
    const void* chunk_key = nullptr; // the chunk's entity array pointer — stable while the structure is stable
    crd::u64    version   = 0;       // Transform chunk-version at last extract
    crd::u32    first     = 0;       // slot range [first, first+count) in the group
    crd::u32    count     = 0;
    bool        dirty     = false;   // re-extracted this sync → its byte range re-uploads (cleared after upload)
};

// All instances of one cooked mesh — ONE buffer, ONE draw.
struct MeshGroup
{
    crd::resources::ResourceId                            mesh_id;
    crd::resources::ResourceId                            material; // REN-2 Half B: representative material (drives the base-color map)
    crd::resources::ResourceHandle<crd::resources::MeshResource> mesh; // keeps the payload resident
    crd::u32                                              index_count = 0;

    crd::containers::Array<InstanceGpu>                             instances;
    crd::containers::Array<crd::scene::EntityId>                    slot_entity;
    crd::containers::Array<crd::geometry::primitives::AABB3<crd::f32>> world_bounds; // per slot — the cull input
    crd::containers::Array<ChunkRun>                                runs;
    crd::containers::Array<crd::u32>                                visible; // per-frame culled slot list

    std::unique_ptr<crd::gpu::IStorageBuffer> buffer;
    crd::u32 indices_off = 0; // word offsets into `buffer`
    crd::u32 vertices_off = 0;
    crd::u32 instances_off = 0;
    crd::u32 visible_off = 0;
    crd::u32 capacity = 0;           // instance slots the buffer holds
    bool     geometry_uploaded = false;

    // GEO-8: the SKINNED path — groups whose mesh carries the SKNV stream draw through the skinned program:
    // a packed skin stream (6 words/vertex: 2×(u16 pair) joints + 4 weights, uploaded once) and a per-instance
    // BONE PALETTE section (joint_count 4×4 matrices per slot, re-sampled + re-uploaded every frame — animation
    // is always dirty by definition). Per-slot animator state mirrors the ECS component at extract time.
    bool     skinned      = false;
    crd::u32 joint_count  = 0;
    crd::u32 skin_off     = 0; // word offset of the packed skin stream
    crd::u32 palette_off  = 0; // word offset of the palette section
    crd::containers::Array<crd::resources::ResourceId> slot_skeleton; // per slot (null = static instance)
    crd::containers::Array<crd::resources::ResourceId> slot_clip;
    crd::containers::Array<crd::f32>                    slot_time;

    explicit MeshGroup(crd::memory::IAllocator* a)
        : instances(a), slot_entity(a), world_bounds(a), runs(a), visible(a), slot_skeleton(a), slot_clip(a),
          slot_time(a)
    {
    }
    MeshGroup(MeshGroup&&) noexcept            = default;
    MeshGroup& operator=(MeshGroup&&) noexcept = default;
};

struct SyncStats
{
    crd::u32 groups            = 0;
    crd::u32 total_instances   = 0;
    bool     structural_rebuild = false;
    crd::u32 dirty_runs        = 0; // chunk runs re-extracted this sync
    crd::u64 uploaded_bytes    = 0; // instance-payload bytes uploaded (the partial-re-upload gate metric)
    crd::u32 meshes_pending    = 0; // MeshRenderers whose mesh resource is not loadable yet
};

struct RenderStats
{
    crd::u32 draws            = 0;
    crd::u32 drawn_instances  = 0;
    crd::u32 culled_instances = 0;
    crd::u64 uploaded_bytes   = 0; // per-frame header + visible-list bytes
    // REN-8: what the DEVICE actually spent, from frame-graph timestamp queries, vs the CPU wall-clock of the
    // whole render call. The GAP between them is the answer to "why is the sandbox 12 ms/frame when neither
    // optimization nor unlocking vsync moves it" — a large gap means the frame is dominated by the
    // submit-and-WAIT that REN-1 deliberately kept, not by rendering work.
    double   gpu_ms           = 0.0; // first pass start → last pass end, one submission
    double   cpu_ms           = 0.0; // wall-clock of render(), including the fence wait
    crd::u32 timed_passes     = 0;
};

class SceneRenderer
{
public:
    explicit SceneRenderer(crd::memory::IAllocator* alloc);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&)            = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Capture the raster + resource seams (sync()'s buffers/uploads work after this). render() additionally
    // needs init_programs(). Split deliberately: extraction/upload logic is testable against a stub raster
    // context with no shader compiler in the loop.
    [[nodiscard]] bool init(crd::gpu::IRasterContext& raster, crd::resources::ResourceManager& rm);

    // Compile the CKIR forward VS/FS + assemble the raster program. False when the backend cannot build them.
    [[nodiscard]] bool init_programs(crd::gpu::IGpuContext& ctx);

    // REN-8: does the frame graph copy the rendered target back to host memory every frame? That copy exists
    // only so `read_pixel` works, so a PRESENTING app should turn it off — it costs a full-target PCIe transfer
    // per frame that nothing reads. Default true, because every readback-asserting gate depends on it and a
    // silent default-off would turn those gates green-on-nothing.
    void set_readback_enabled(bool on) noexcept;

    // ⛔ HARD RULE (AGENTS.md): EVERY render pass goes through our own frame-graph machinery. An overlay — the
    // infinite grid, gizmos, debug viz, editor chrome — is a RENDER PASS, so it belongs in the frame's graph as
    // a pass, not as a separate `draw_*` sequence with its own submit.
    //
    // Registering it here makes it the second pass of the SAME graph, sharing the one command buffer and the one
    // submission with the scene. Before this the sandbox's grid submitted independently and cost ~2.6 ms/frame —
    // nearly twice what the entire 4 100-instance scene cost on the GPU (~1.5 ms) — because each `draw_overlay`
    // outside a pass does its own submit+wait. Inside a pass the same call RECORDS instead (`frame_recording()`).
    //
    // The callback receives the frame context; `ctx.raster()` is in recording mode, so any existing draw helper
    // (`crd::draw::submit_overlay`, …) works unchanged from inside it.
    using FramePassFn = void (*)(crd::gpu::IFrameContext& ctx, void* user);
    void set_overlay_pass(FramePassFn fn, void* user) noexcept;

    // Chunk-grain extract + change-driven upload. Call once per frame AFTER transform propagation (world.step).
    SyncStats sync(crd::scene::World& world);

    // Cull + submit into `target` (a create_color_depth_target). Reverse-Z: depth clears to 0, GreaterEqual.
    // `bvh`: an optional configured SpatialBVHIndex — its overlap query prunes the broad phase before the exact
    // 6-plane test (pass nullptr to plane-test every instance).
    RenderStats render(crd::gpu::IRasterTarget& target, const crd::math::Mat4f& view_proj,
                       const crd::math::Vec3f& light_dir, crd::gpu::ClearColor clear,
                       const crd::scene::SpatialBVHIndex* bvh = nullptr);

    [[nodiscard]] const crd::containers::Array<MeshGroup>& mesh_groups() const noexcept { return m_groups; }

    struct Impl; // opaque (defined in scene_renderer.cpp); public so the file-local extraction visitors reach it

private:
    std::unique_ptr<Impl> m_impl;
    crd::containers::Array<MeshGroup> m_groups;
};

// Extract the 6 world-space frustum planes (ax+by+cz+d ≥ 0 = inside) from a view-projection matrix
// (Gribb–Hartmann; clip z in [0,1] — the Vulkan/reverse-Z convention). Plane order: L R B T N F.
void frustum_planes(const crd::math::Mat4f& view_proj, crd::math::Vec4f out_planes[6]);

// Positive-vertex AABB-vs-plane-set test: true iff `box` intersects the frustum described by 6 planes.
[[nodiscard]] bool aabb_in_frustum(const crd::geometry::primitives::AABB3<crd::f32>& box,
                                   const crd::math::Vec4f planes[6]) noexcept;

// The world-space AABB of the frustum (the BVH broad-phase query box): the 8 clip corners through the inverse
// view-projection. Degenerate matrices yield an everything-box (the broad phase then prunes nothing — safe).
[[nodiscard]] crd::geometry::primitives::AABB3<crd::f32> frustum_aabb(const crd::math::Mat4f& view_proj);

} // namespace crd::scenerender
