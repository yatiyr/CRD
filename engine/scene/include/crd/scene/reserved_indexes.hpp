#pragma once

#include <crd/containers/string_view.hpp>
#include <crd/scene/component_index.hpp>
#include <crd/scene/spatial_bvh_index.hpp>  // SpatialBVHIndex promoted from no-op shell at v5-index-bringup; this header re-exports it for the auto-register path

namespace crd::scene
{
// Reserved no-op indexes — Phase 3.0 v1i (ADR-0053 §5–§7 + §2 trait grammar).
//
// These indexes ROUND-TRIP the trait grammar today: when a user calls
// register_component<T>(History{60}) or SpatialBVH{} or GpuResident{} or
// Replication::ServerAuthoritative or Reflection{...}, World auto-
// registers the matching no-op index here. The stub silently accepts
// every event so the framework's contracts are honored end-to-end —
// observed() reports the watched components, on_*() are no-ops.
//
// When the real index ships in its consumer phase (History → 3.2,
// SpatialBVH → 3.5, GpuResident → 3.8, Replication → 4.2, Reflection →
// 7), the user's day-one registration suddenly starts doing real work,
// without any caller code change. That's the ADR-0053 promise.
//
// Each shell is 30 LOC because it's literally five virtual no-ops + the
// observed mask + the name. Shipping all five today closes ADR-0056's
// "reserved L6-L8 slots" loop.

namespace detail
{
// Common base for the no-op shells — keeps each per-trait class a 5-line
// declaration (just name + watch path). The base implements every
// IComponentIndex method as a no-op except observed() and name().
template <const char* kName> class ReservedNoOpIndexBase : public IComponentIndex
{
public:
    void on_insert(EntityId, ComponentId, const void*) override {}
    void on_update(EntityId, ComponentId, const void*, const void*) override {}
    void on_remove(EntityId, ComponentId, const void*) override {}
    void on_entity_destroyed(EntityId) override {}

    [[nodiscard]] ComponentMask                observed() const override { return m_observed; }
    [[nodiscard]] crd::containers::StringView  name() const override
    {
        return crd::containers::StringView{kName};
    }

    void watch(ComponentId c) noexcept { m_observed.set(c); }

private:
    ComponentMask m_observed{};
};

inline constexpr char kHistoryIndexName[]      = "HistoryIndex";
// SpatialBVHIndex name pinned in `spatial_bvh_index.hpp` — it ships as a real
// implementation now (Phase 3.1.7 v5-index-bringup, 2026-05-16).
inline constexpr char kGpuResidentIndexName[]  = "GpuResidentIndex";
inline constexpr char kReplicationIndexName[]  = "ReplicationIndex";
inline constexpr char kReflectionIndexName[]   = "ReflectionIndex";
} // namespace detail

// History-window per-component ring buffer (ADR-0053 §5).
// Shipping shell — actual ring buffer is Phase 3.2 (animation interp,
// rollback, replay).
class HistoryIndex final : public detail::ReservedNoOpIndexBase<detail::kHistoryIndexName>
{
};

// SpatialBVHIndex — promoted from no-op shell to a REAL LooseOctree-backed
// component index at Phase 3.1.7 v5-index-bringup (2026-05-16). Class lives
// in `spatial_bvh_index.hpp` and is re-included above so the auto-register
// path (`World::auto_register_indexes_for`) finds the same class via
// `dynamic_cast`. Day-one promise preserved: when unconfigured (the default
// state from auto-register), all hooks no-op exactly like the shell did.
// Users opt in by calling
// `world.find_index<SpatialBVHIndex>()->configure(extractor, opts)`.

// CPU-mirrored GPU SSBO of component data (ADR-0053 §7).
// Shipping shell — actual GPU residency wiring is Phase 3.8 (GPU-driven
// rendering).
class GpuResidentIndex final : public detail::ReservedNoOpIndexBase<detail::kGpuResidentIndexName>
{
};

// Network replication priority/state index (ADR-0056).
// Shipping shell — actual replication wiring is Phase 4.2.
class ReplicationIndex final : public detail::ReservedNoOpIndexBase<detail::kReplicationIndexName>
{
};

// Editor / introspection index (ADR-0056).
// Shipping shell — actual reflection runtime is Phase 7.
class ReflectionIndex final : public detail::ReservedNoOpIndexBase<detail::kReflectionIndexName>
{
};

} // namespace crd::scene
