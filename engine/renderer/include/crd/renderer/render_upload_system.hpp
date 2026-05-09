#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/renderer/gpu_uploader.hpp>
#include <crd/renderer/renderer.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/system.hpp>

namespace crd::scene
{
class World;
}

namespace crd::renderer
{
// Phase 3.0 v1o2 / v1o3 — async-GPU-upload ECS surface (ADR-0061 Layer 3).
//
// PendingMeshUpload — attached alongside Renderable while a GPU mesh upload
// is in flight. The component carries the UploadHandle that owns the
// in-flight Fence + staging buffers + recorded command buffer + the
// pending GpuMesh.
//
// Lifecycle:
//   1. Caller spawns an entity, adds Renderable (with AsyncAware{}), and
//      adds PendingMeshUpload{ GpuUploader::upload_mesh_async(...) }.
//      AsyncAwareIndex auto-marks the Renderable as Loading.
//   2. RenderUploadSystem runs in RenderExtract phase each frame. For
//      every (Renderable, PendingMeshUpload) entity it polls
//      handle.is_ready(). If true:
//         - take_mesh() into the World-registered RenderMeshIndex;
//         - patch the Renderable's vertex_buffer / index_buffer pointers
//           to the just-stored GpuMesh's buffers;
//         - mark AsyncAware Loaded for Renderable;
//         - queue Commands::remove_component<PendingMeshUpload>(e).
//   3. After the system runs, the schedule flushes Commands at the phase
//      boundary; the marker component is gone, the entity is "ready", and
//      `world.query<...>().skip_pending<Renderable>()` automatically
//      includes it from the next frame onwards.
//
// Storage policy (v1o3): GpuMesh ownership lives in `RenderMeshIndex`,
// a separate `IComponentIndex` registered with the World. The index
// observes `Renderable` lifecycle events; on `on_remove` /
// `on_entity_destroyed` the matching GpuMesh is evicted automatically
// — meaning `World::destroy(e)` releases the GPU resources without any
// explicit cleanup call from the consumer. This is the "proper drop
// callback" hook the v1o2 session log promised v1o3 would land.
//
// Registration order (consumers MUST do):
//   world.register_component<Renderable>(crd::scene::AsyncAware{});
//   world.register_component<PendingMeshUpload>();
//   auto* mesh_idx = world.register_index<RenderMeshIndex>(allocator);
//   mesh_idx->watch(world.component_id<Renderable>());
//   world.register_system(std::make_unique<RenderUploadSystem>());
//
struct PendingMeshUpload
{
    UploadHandle handle;
};

class RenderUploadSystem final : public crd::scene::ISystem
{
public:
    using Reads  = crd::scene::ComponentSet<>;
    using Writes = crd::scene::ComponentSet<Renderable, PendingMeshUpload>;

    RenderUploadSystem() noexcept = default;

    [[nodiscard]] crd::scene::SchedulePhase phase() const override
    {
        return crd::scene::SchedulePhase::RenderExtract;
    }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"RenderUploadSystem"};
    }

    void run(crd::scene::World& world) override;
};

} // namespace crd::renderer
