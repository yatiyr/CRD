#pragma once

#include <crd/containers/hash_map.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderer/gpu_uploader.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_index.hpp>
#include <crd/scene/entity.hpp>

namespace crd::renderer
{
// Phase 3.0 v1o3 — RenderMeshIndex (ADR-0061 Layer 3, lifetime contract).
//
// `IComponentIndex` that owns the GPU meshes produced by completed
// `UploadHandle`s. Lives alongside `RenderUploadSystem`; the system
// promotes a pending upload by calling `install(entity, mesh)`, the
// index keeps the GpuMesh resident keyed by `EntityId.raw`, and any
// later `on_remove(Renderable)` / `on_entity_destroyed(e)` event evicts
// the entry — meaning a `World::destroy(e)` (or even just a stray
// `remove_component<Renderable>(e)`) automatically releases the GPU
// resources without the sandbox or any other consumer remembering to
// call a manual cleanup.
//
// This is the proper drop-callback hook the v1o2 session log promised
// for v1o3: a per-component lifecycle observer that participates in the
// same fan-out the rest of the L5 indexes use (ChangeDetect,
// AsyncAware, ...). Adding it required no changes to `World::destroy`
// — the storage backends already emit `on_remove` + `on_entity_destroyed`
// to every index whose `observed()` mask includes the touched
// component.
//
// Registration:
//   1. Register `Renderable` (with `AsyncAware{}` trait if you want the
//      pending-upload skip filter).
//   2. `auto* idx = world.register_index<RenderMeshIndex>(alloc);`
//   3. `idx->watch(world.component_id<Renderable>());`
//   4. (Optional) `idx->watch(world.component_id<PendingMeshUpload>());`
//      — if you want to observe pending state for diagnostics.
//
// `RenderUploadSystem::run` looks up the index via
// `world.find_index<RenderMeshIndex>()` on each invocation; missing
// index = no-op (logged once via assert in debug).
class RenderMeshIndex final : public crd::scene::IComponentIndex
{
public:
    explicit RenderMeshIndex(crd::memory::IAllocator* alloc);

    // ---- Public API consumed by RenderUploadSystem and tests -----------

    // Move-install a freshly-uploaded mesh keyed by entity. Returns true
    // on insert, false if an entry already existed (caller must not
    // overwrite — inserting twice for the same entity is a logic bug).
    [[nodiscard]] bool install(crd::scene::EntityId e, GpuMesh mesh);

    // Lookup. Returns null if no entry.
    [[nodiscard]] GpuMesh*       find(crd::scene::EntityId e) noexcept;
    [[nodiscard]] const GpuMesh* find(crd::scene::EntityId e) const noexcept;

    // Manual eviction (tests + advanced consumers). Returns true if an
    // entry was erased. The drop-callback path (on_remove /
    // on_entity_destroyed) calls into this.
    bool release(crd::scene::EntityId e) noexcept;

    // Add `c` to the observed mask. Typically called once with the
    // `Renderable` component id after the component is registered with
    // the World.
    void watch(crd::scene::ComponentId c) noexcept { m_observed.set(c); }

    // Diagnostics — count of resident meshes.
    [[nodiscard]] crd::usize count() const noexcept { return m_owned.size(); }

    // ---- IStorageEventSink (via IComponentIndex) -----------------------

    void on_insert(crd::scene::EntityId /*e*/, crd::scene::ComponentId /*c*/,
                   const void* /*data*/) override
    {
    }
    void on_update(crd::scene::EntityId /*e*/, crd::scene::ComponentId /*c*/,
                   const void* /*old_data*/, const void* /*new_data*/) override
    {
    }
    void on_remove(crd::scene::EntityId e, crd::scene::ComponentId c, const void* /*data*/) override;
    void on_entity_destroyed(crd::scene::EntityId e) override;

    // ---- IComponentIndex -----------------------------------------------

    [[nodiscard]] crd::scene::ComponentMask observed() const override { return m_observed; }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"RenderMeshIndex"};
    }

private:
    crd::scene::ComponentMask                  m_observed{};
    crd::containers::HashMap<crd::u64, GpuMesh> m_owned;
};

} // namespace crd::renderer
