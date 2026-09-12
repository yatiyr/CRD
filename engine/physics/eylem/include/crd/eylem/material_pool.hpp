#pragma once

// MaterialPool — scene-owned storage for `Material` records. Phase 3.1
// v1a-material-b (ADR-0069 §3 + §11).
//
// Lives in `crd-eylem` (interface module), NOT in `crd-eylem-rigid3d`,
// because every `IPhysicsScene` impl reuses it (NullPhysicsScene today,
// concrete eylem-rigid3d Scene at v1b-c, cooker handlers at v1k) and the
// pool's API surface is part of the v1l API freeze.
//
// Layout:
//   slot 0 — reserved as null sentinel; never accessed for material data
//   slot 1 — `default_material()` (always allocated at construction time;
//            never invalidated). Resolves `MaterialId::default_material()`.
//   slot 2..N — user materials, allocated by `insert()`
//
// IDs follow the standard `[generation:8 | index:24]` layout (matching
// BodyId / ColliderId / JointId per ADR-0069 §3). v1 uses generation = 1
// for every allocated slot — materials are scene-lifetime; remove is not
// supported in v1 (the `update_material` path covers in-place mutation
// without invalidating the handle, which is the common author workflow).
// If a future need for remove appears, we add it then via the standard
// generation-bump pattern; the read API stays stable.
//
// Determinism: insert order is preserved; `MaterialId.index()` is the
// slot's position in the underlying Array — append-only; deterministic
// across runs given the same sequence of `insert` calls. The cooker
// path (v1k-material-cooker per ADR-0069 §10) computes a content-
// addressed `MaterialId` via FNV-1a-64 over canonical parameter bytes
// and uses `update` to replace any pool slot whose hash matches — same
// content-addressed discipline as `FieldId` (ADR-0067 §3).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/eylem/material.hpp>
#include <crd/eylem/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::eylem
{
class MaterialPool
{
public:
    // The 24-bit `MaterialId` index field caps at 16,777,215 slots.
    // Slot 0 is null, slot 1 is default — effective user capacity is
    // (kIndexMax - 1).
    static constexpr crd::u32 kIndexMax = (1U << 24) - 1U;

    // Constructed with two slots pre-populated:
    //   [0] = null sentinel (never read)
    //   [1] = default_material_value() — resolves MaterialId::default_material()
    // `alloc` may be nullptr → falls back to crd::memory::default_allocator().
    explicit MaterialPool(crd::memory::IAllocator* alloc = crd::memory::default_allocator());

    // Allocate a new slot for `material`. Returns the assigned MaterialId
    // (generation = 1 in v1; the slot index is the new pool position).
    // Returns MaterialId::null() if the pool is at capacity (kIndexMax).
    [[nodiscard]] MaterialId insert(const Material& material);

    // In-place mutation. No-op if `id` is invalid (null / out-of-range /
    // wrong generation). The handle remains stable — same MaterialId
    // continues to resolve to the new bytes.
    void update(MaterialId id, const Material& material) noexcept;

    // Returns the material at `id`. If `id` is invalid, returns a
    // reference to the default material (slot 1) so callers do not need
    // to null-check on the read path. Stable for the pool's lifetime.
    [[nodiscard]] const Material& get(MaterialId id) const noexcept;

    [[nodiscard]] bool       contains(MaterialId id) const noexcept;
    [[nodiscard]] crd::usize size()     const noexcept; // count of live materials INCLUDING the default (slot 1)
    [[nodiscard]] crd::u32   capacity() const noexcept { return kIndexMax; }

    // Direct const access for cookers / debug viz that iterate the pool.
    // Returns the underlying span; index 0 is the null sentinel — skip it.
    [[nodiscard]] crd::containers::ConstSpan<Material> all() const noexcept;

private:
    crd::containers::Array<Material> m_materials;
};

} // namespace crd::eylem
