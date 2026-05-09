// Phase 3.0 v1o2/v1o3 — RenderUploadSystem implementation (ADR-0061 Layer 3).

#include <crd/core/assert.hpp>
#include <crd/renderer/render_mesh_index.hpp>
#include <crd/renderer/render_upload_system.hpp>
#include <crd/scene/async_aware_index.hpp>
#include <crd/scene/commands.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/query.hpp>
#include <crd/scene/world.hpp>

namespace crd::renderer
{

void RenderUploadSystem::run(crd::scene::World& world)
{
    const auto cid_renderable = world.component_id<Renderable>();
    if (cid_renderable.is_null())
    {
        return; // Renderable not registered → nothing to do.
    }

    auto* mesh_index = world.find_index<RenderMeshIndex>();
    CRD_ASSERT_MSG(mesh_index != nullptr,
                   "RenderUploadSystem::run: RenderMeshIndex must be registered "
                   "(call world.register_index<RenderMeshIndex>(alloc) and "
                   "watch(world.component_id<Renderable>()) before stepping the schedule)");

    auto* async_aware = world.find_index<crd::scene::AsyncAwareIndex>();
    auto& cmds        = world.commands();

    auto query = world.query<Renderable, PendingMeshUpload>();
    for (auto&& [entity, renderable, pending] : query)
    {
        if (!pending.handle.is_valid() || !pending.handle.is_ready())
        {
            continue;
        }

        // Move the produced GpuMesh into the World-owned mesh index. The
        // index handles lifetime cleanup automatically via the L5 drop-
        // callback path (see render_mesh_index.hpp).
        [[maybe_unused]] const bool inserted =
            mesh_index->install(entity, pending.handle.take_mesh());
        CRD_ASSERT_MSG(inserted,
                       "RenderUploadSystem: duplicate upload completion for the same entity");
        GpuMesh* stored = mesh_index->find(entity);
        CRD_ASSERT_MSG(stored != nullptr,
                       "RenderUploadSystem: failed to retrieve just-installed mesh");

        // Patch Renderable to point at the now-resident buffers.
        renderable.vertex_buffer = stored->vertex_buffer.get();
        renderable.index_buffer  = stored->index_buffer.get();

        // Tell the AsyncAwareIndex this entity is ready; the standard
        // `query<...>().skip_pending<Renderable>()` filter will start
        // including it next frame. (Index pointer is non-null when
        // Renderable was registered with the AsyncAware{} trait.)
        if (async_aware != nullptr)
        {
            async_aware->mark_loaded(entity, cid_renderable);
        }

        // Drop the marker — the upload state has migrated to the index.
        cmds.remove_component<PendingMeshUpload>(entity);
    }
}

} // namespace crd::renderer
