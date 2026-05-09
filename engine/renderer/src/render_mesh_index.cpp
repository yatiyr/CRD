// Phase 3.0 v1o3 — RenderMeshIndex implementation.

#include <crd/core/assert.hpp>
#include <crd/renderer/render_mesh_index.hpp>

namespace crd::renderer
{

RenderMeshIndex::RenderMeshIndex(crd::memory::IAllocator* alloc) : m_owned(alloc)
{
    CRD_ASSERT_MSG(alloc != nullptr, "RenderMeshIndex: null allocator");
}

bool RenderMeshIndex::install(crd::scene::EntityId e, GpuMesh mesh)
{
    return m_owned.emplace(e.raw, std::move(mesh));
}

GpuMesh* RenderMeshIndex::find(crd::scene::EntityId e) noexcept
{
    return m_owned.find(e.raw);
}

const GpuMesh* RenderMeshIndex::find(crd::scene::EntityId e) const noexcept
{
    return m_owned.find(e.raw);
}

bool RenderMeshIndex::release(crd::scene::EntityId e) noexcept
{
    return m_owned.erase(e.raw);
}

void RenderMeshIndex::on_remove(crd::scene::EntityId e, crd::scene::ComponentId c,
                                const void* /*data*/)
{
    // Only react when the touched component is one we observe. The
    // fan-out already filters by `observed()` mask, so reaching this
    // point means `c` is in our mask — but a defensive guard is cheap
    // and lets the test suite register multiple watch()'d components
    // without surprises.
    if (m_observed.test(c))
    {
        m_owned.erase(e.raw);
    }
}

void RenderMeshIndex::on_entity_destroyed(crd::scene::EntityId e)
{
    // Per-component on_remove fires first (per IStorageEventSink contract),
    // so by the time this lands the entry is usually already gone. Erase
    // anyway as the closing belt-and-suspenders sweep — entities removed
    // via paths that bypass per-component `on_remove` (none today, but
    // future storage backends may differ) still get cleaned up.
    m_owned.erase(e.raw);
}

} // namespace crd::renderer
