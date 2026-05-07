// Phase 3.0 v1i — IComponentIndex framework tests (ADR-0053).
//
// Coverage:
//   - register_index stores an index; find_index<T> retrieves it.
//   - Auto-registration: ChangeDetect ALWAYS appears after first
//     register_component; AsyncAware appears when AsyncAware{} trait set;
//     reserved no-op shells appear for History/SpatialBVH/GpuResident/
//     Replication/Reflection.
//   - Fan-out: a registered index receives on_insert/on_update/on_remove/
//     on_entity_destroyed for both backends.
//   - observed() filtering: only events for watched components are dispatched.
//   - External sink coexistence: set_storage_event_sink runs alongside indexes.
//   - ChangeDetectIndex: changed_since semantics + .changed<T>() query op.
//   - AsyncAwareIndex: Loading-by-default; mark_loaded transitions; .skip_pending<T>().
//   - Frame counter: current_frame() advances on step().
//   - Reserved no-op indexes accept events without crashing.

#include <crd/scene/async_aware_index.hpp>
#include <crd/scene/change_detect_index.hpp>
#include <crd/scene/component_index.hpp>
#include <crd/scene/reserved_indexes.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::scene::AsyncAware;
using crd::scene::AsyncAwareIndex;
using crd::scene::ChangeDetectIndex;
using crd::scene::ComponentId;
using crd::scene::ComponentMask;
using crd::scene::EntityId;
using crd::scene::GpuResident;
using crd::scene::GpuResidentIndex;
using crd::scene::History;
using crd::scene::HistoryIndex;
using crd::scene::IComponentIndex;
using crd::scene::IStorageEventSink;
using crd::scene::LoadState;
using crd::scene::Replication;
using crd::scene::ReplicationIndex;
using crd::scene::SpatialBVH;
using crd::scene::SpatialBVHIndex;
using crd::scene::StorageHint;
using crd::scene::World;

namespace
{
struct Position
{
    float x{}, y{}, z{};
};

struct Velocity
{
    float dx{}, dy{}, dz{};
};

struct Mesh
{
    crd::u32 id = 0;
};

// CountingSink — measures fan-out behaviour alongside indexes.
struct CountingSink : public IStorageEventSink
{
    crd::u32 inserts = 0;
    crd::u32 updates = 0;
    crd::u32 removes = 0;
    crd::u32 destroys = 0;

    void on_insert(EntityId, ComponentId, const void*) override { ++inserts; }
    void on_update(EntityId, ComponentId, const void*, const void*) override { ++updates; }
    void on_remove(EntityId, ComponentId, const void*) override { ++removes; }
    void on_entity_destroyed(EntityId) override { ++destroys; }
};

// Test-only index that records every event it sees.
class RecorderIndex : public IComponentIndex
{
public:
    crd::u32 inserts = 0;
    crd::u32 updates = 0;
    crd::u32 removes = 0;
    crd::u32 destroys = 0;
    crd::u32 frame_begins = 0;
    crd::u32 frame_ends = 0;

    void watch(ComponentId c) noexcept { m_observed.set(c); }
    [[nodiscard]] ComponentMask observed() const override { return m_observed; }
    [[nodiscard]] crd::containers::StringView name() const override
    {
        return crd::containers::StringView{"RecorderIndex"};
    }

    void on_insert(EntityId, ComponentId, const void*) override { ++inserts; }
    void on_update(EntityId, ComponentId, const void*, const void*) override { ++updates; }
    void on_remove(EntityId, ComponentId, const void*) override { ++removes; }
    void on_entity_destroyed(EntityId) override { ++destroys; }
    void on_frame_begin(crd::u32) override { ++frame_begins; }
    void on_frame_end(crd::u32) override { ++frame_ends; }

private:
    ComponentMask m_observed{};
};
} // namespace

// -----------------------------------------------------------------------------
// register_index + find_index
// -----------------------------------------------------------------------------

TEST_CASE("register_index stores and retrieves an index", "[scene][index][register]")
{
    World w;
    auto* rec = w.register_index<RecorderIndex>();
    CHECK(rec != nullptr);
    CHECK(w.find_index<RecorderIndex>() == rec);
    CHECK(w.index_count() >= 1U);
}

// -----------------------------------------------------------------------------
// Auto-registration via traits (ADR-0053 §2)
// -----------------------------------------------------------------------------

TEST_CASE("Auto-registration: ChangeDetectIndex appears after first register_component",
          "[scene][index][auto-register]")
{
    World w;
    CHECK(w.find_index<ChangeDetectIndex>() == nullptr); // not yet
    w.register_component<Position>();
    CHECK(w.find_index<ChangeDetectIndex>() != nullptr); // auto-registered
}

TEST_CASE("Auto-registration: AsyncAwareIndex appears when AsyncAware{} trait set",
          "[scene][index][auto-register]")
{
    World w;
    w.register_component<Position>();
    CHECK(w.find_index<AsyncAwareIndex>() == nullptr);
    w.register_component<Mesh>(AsyncAware{});
    CHECK(w.find_index<AsyncAwareIndex>() != nullptr);
}

TEST_CASE("Auto-registration: reserved no-op indexes appear via traits",
          "[scene][index][auto-register][reserved]")
{
    World w;
    w.register_component<Position>(History{60}, SpatialBVH{}, GpuResident{});
    CHECK(w.find_index<HistoryIndex>() != nullptr);
    CHECK(w.find_index<SpatialBVHIndex>() != nullptr);
    CHECK(w.find_index<GpuResidentIndex>() != nullptr);
}

TEST_CASE("Auto-registration: ReplicationIndex appears for non-Local replication",
          "[scene][index][auto-register][reserved]")
{
    World w;
    w.register_component<Position>(Replication::ServerAuthoritative);
    CHECK(w.find_index<ReplicationIndex>() != nullptr);
}

// -----------------------------------------------------------------------------
// Event fan-out
// -----------------------------------------------------------------------------

TEST_CASE("Fan-out: registered index receives on_insert/on_update/on_remove for both backends",
          "[scene][index][fan-out]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Mesh>(StorageHint::SparseSet);

    auto* rec = w.register_index<RecorderIndex>();
    rec->watch(w.component_id<Position>());
    rec->watch(w.component_id<Mesh>());

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3}); // archetype on_insert
    w.add_component<Mesh>(e, Mesh{42});             // sparse on_insert

    CHECK(rec->inserts == 2U);

    // Both get_component_mut() and add_component() (UPSERT) fire on_update;
    // the storage path treats every write as a tracked update — confirmed
    // by archetype_chunk_storage.cpp which fires on_update from get_mut.
    *w.get_component_mut<Position>(e) = Position{4, 5, 6}; // fires on_update
    w.add_component<Position>(e, Position{7, 8, 9});       // UPSERT → on_update
    CHECK(rec->updates == 2U);

    w.remove_component<Mesh>(e);
    CHECK(rec->removes == 1U);

    w.destroy_immediate(e);
    CHECK(rec->destroys == 1U);
}

TEST_CASE("Fan-out: observed() mask filters events", "[scene][index][fan-out][observed]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();

    auto* rec = w.register_index<RecorderIndex>();
    rec->watch(w.component_id<Position>()); // only Position

    EntityId e = w.spawn();
    w.add_component<Position>(e, {});
    w.add_component<Velocity>(e, {}); // NOT watched

    CHECK(rec->inserts == 1U); // only Position
}

TEST_CASE("Fan-out: multiple indexes receive events in registration order",
          "[scene][index][fan-out][order]")
{
    World w;
    w.register_component<Position>();

    auto* a = w.register_index<RecorderIndex>();
    auto* b = w.register_index<RecorderIndex>();
    a->watch(w.component_id<Position>());
    b->watch(w.component_id<Position>());

    EntityId e = w.spawn();
    w.add_component<Position>(e, {});

    CHECK(a->inserts == 1U);
    CHECK(b->inserts == 1U);
}

// -----------------------------------------------------------------------------
// External sink coexistence
// -----------------------------------------------------------------------------

TEST_CASE("set_storage_event_sink coexists with registered indexes", "[scene][index][external-sink]")
{
    World w;
    w.register_component<Position>();

    auto* rec = w.register_index<RecorderIndex>();
    rec->watch(w.component_id<Position>());

    CountingSink ext;
    w.set_storage_event_sink(&ext);

    EntityId e = w.spawn();
    w.add_component<Position>(e, {});

    CHECK(rec->inserts == 1U);
    CHECK(ext.inserts == 1U);

    w.destroy_immediate(e);
    CHECK(rec->destroys == 1U);
    CHECK(ext.destroys == 1U);
}

// -----------------------------------------------------------------------------
// Frame counter
// -----------------------------------------------------------------------------

TEST_CASE("World::current_frame advances on step", "[scene][index][frame-counter]")
{
    World w;
    CHECK(w.current_frame() == 0U);
    w.step(1.0 / 60.0);
    CHECK(w.current_frame() == 1U);
    w.step(1.0 / 60.0);
    CHECK(w.current_frame() == 2U);
}

TEST_CASE("Frame lifecycle hooks fire once per step", "[scene][index][frame-counter]")
{
    World w;
    auto* rec = w.register_index<RecorderIndex>();
    w.step(1.0 / 60.0);
    CHECK(rec->frame_begins == 1U);
    CHECK(rec->frame_ends == 1U);
    w.step_fixed(0.5, 0.1, 4); // 5 substeps but ONE frame_begin/end
    CHECK(rec->frame_begins == 2U);
    CHECK(rec->frame_ends == 2U);
}

// -----------------------------------------------------------------------------
// ChangeDetectIndex
// -----------------------------------------------------------------------------

TEST_CASE("ChangeDetectIndex: changed_since semantics", "[scene][index][change-detect]")
{
    World w;
    w.register_component<Position>();
    auto* idx = w.find_index<ChangeDetectIndex>();
    REQUIRE(idx != nullptr);

    w.step(1.0 / 60.0); // frame 1

    EntityId e = w.spawn();
    w.add_component<Position>(e, {});

    const ComponentId pid = w.component_id<Position>();
    CHECK(idx->changed_since(e, pid, 1U)); // changed during frame 1
    CHECK_FALSE(idx->changed_since(e, pid, 2U)); // not changed at frame 2 yet

    w.step(1.0 / 60.0); // frame 2 — no further changes
    CHECK(idx->changed_since(e, pid, 1U)); // still changed since frame 1
    CHECK_FALSE(idx->changed_since(e, pid, 2U)); // not changed during frame 2
}

TEST_CASE("Query::changed<T>() filters to entities modified during current frame",
          "[scene][index][change-detect][query]")
{
    World w;
    w.register_component<Position>();

    EntityId e_old = w.spawn();
    w.add_component<Position>(e_old, {});

    w.step(1.0 / 60.0); // frame 1; old entity already in storage

    EntityId e_new = w.spawn();
    w.add_component<Position>(e_new, {}); // touched during frame 1

    auto q = w.query<Position>().changed<Position>();
    CHECK(q.count() == 1U);
    REQUIRE(q.matches().size() == 1U);
    CHECK(q.matches()[0].raw == e_new.raw);
}

TEST_CASE("ChangeDetectIndex: on_entity_destroyed clears entries", "[scene][index][change-detect][cleanup]")
{
    World w;
    w.register_component<Position>();
    auto* idx = w.find_index<ChangeDetectIndex>();
    REQUIRE(idx != nullptr);

    EntityId e = w.spawn();
    w.add_component<Position>(e, {});
    CHECK(idx->tracked_entries() >= 1U);

    w.destroy_immediate(e);
    CHECK(idx->tracked_entries() == 0U);
}

// -----------------------------------------------------------------------------
// AsyncAwareIndex
// -----------------------------------------------------------------------------

TEST_CASE("AsyncAwareIndex: on_insert sets Loading; mark_loaded transitions to Loaded",
          "[scene][index][async-aware]")
{
    World w;
    w.register_component<Mesh>(AsyncAware{});
    auto* idx = w.find_index<AsyncAwareIndex>();
    REQUIRE(idx != nullptr);

    EntityId e = w.spawn();
    w.add_component<Mesh>(e, Mesh{42});

    const ComponentId mid = w.component_id<Mesh>();
    CHECK(idx->is_pending(e, mid));
    CHECK_FALSE(idx->is_loaded(e, mid));
    CHECK(idx->load_state(e, mid) == LoadState::Loading);

    idx->mark_loaded(e, mid);
    CHECK_FALSE(idx->is_pending(e, mid));
    CHECK(idx->is_loaded(e, mid));
}

TEST_CASE("Query::skip_pending<T>() excludes entities in Loading state",
          "[scene][index][async-aware][query]")
{
    World w;
    w.register_component<Mesh>(AsyncAware{});
    auto* idx = w.find_index<AsyncAwareIndex>();
    REQUIRE(idx != nullptr);

    EntityId loading = w.spawn();
    EntityId loaded = w.spawn();
    w.add_component<Mesh>(loading, Mesh{1});
    w.add_component<Mesh>(loaded, Mesh{2});

    idx->mark_loaded(loaded, w.component_id<Mesh>());

    auto q = w.query<Mesh>().skip_pending<Mesh>();
    CHECK(q.count() == 1U);
    REQUIRE(q.matches().size() == 1U);
    CHECK(q.matches()[0].raw == loaded.raw);
}

TEST_CASE("AsyncAwareIndex: mark_failed transitions; skip_pending excludes Loading only",
          "[scene][index][async-aware]")
{
    World w;
    w.register_component<Mesh>(AsyncAware{});
    auto* idx = w.find_index<AsyncAwareIndex>();
    REQUIRE(idx != nullptr);

    EntityId e = w.spawn();
    w.add_component<Mesh>(e, Mesh{42});
    idx->mark_failed(e, w.component_id<Mesh>());

    CHECK(idx->load_state(e, w.component_id<Mesh>()) == LoadState::Failed);
    CHECK_FALSE(idx->is_pending(e, w.component_id<Mesh>()));
    // skip_pending excludes Loading only — Failed entities pass.
    auto q = w.query<Mesh>().skip_pending<Mesh>();
    CHECK(q.count() == 1U);
}

TEST_CASE("AsyncAwareIndex: on_remove drops state", "[scene][index][async-aware][cleanup]")
{
    World w;
    w.register_component<Mesh>(AsyncAware{});
    auto* idx = w.find_index<AsyncAwareIndex>();
    REQUIRE(idx != nullptr);

    auto* rec = w.register_index<RecorderIndex>();
    rec->watch(w.component_id<Mesh>());

    EntityId e = w.spawn();
    w.add_component<Mesh>(e, {});
    CHECK(w.has_component<Mesh>(e));
    CHECK(rec->inserts == 1U);
    CHECK(idx->tracked_entries() == 1U);

    w.remove_component<Mesh>(e);
    CHECK_FALSE(w.has_component<Mesh>(e));
    CHECK(rec->removes == 1U); // confirms on_remove fired through fan-out
    CHECK(idx->tracked_entries() == 0U);
}

// -----------------------------------------------------------------------------
// Reserved no-op indexes
// -----------------------------------------------------------------------------

TEST_CASE("Reserved no-op indexes accept all events without crashing",
          "[scene][index][reserved]")
{
    World w;
    w.register_component<Position>(History{30}, SpatialBVH{}, GpuResident{});

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});
    *w.get_component_mut<Position>(e) = Position{4, 5, 6};
    w.remove_component<Position>(e);
    w.destroy_immediate(e);

    // No assertion needed — the test passes if no crash occurs and the
    // index pointers exist.
    CHECK(w.find_index<HistoryIndex>() != nullptr);
    CHECK(w.find_index<SpatialBVHIndex>() != nullptr);
    CHECK(w.find_index<GpuResidentIndex>() != nullptr);
}

// -----------------------------------------------------------------------------
// Composed: changed + skip_pending
// -----------------------------------------------------------------------------

TEST_CASE("Composed query: changed<T> + skip_pending<U>", "[scene][index][query][compose]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Mesh>(AsyncAware{});

    auto* aaidx = w.find_index<AsyncAwareIndex>();
    REQUIRE(aaidx != nullptr);

    EntityId a = w.spawn();
    EntityId b = w.spawn();
    w.add_component<Position>(a, {});
    w.add_component<Position>(b, {});
    w.add_component<Mesh>(a, {});
    w.add_component<Mesh>(b, {});
    aaidx->mark_loaded(a, w.component_id<Mesh>());
    aaidx->mark_loaded(b, w.component_id<Mesh>());

    // Both are loaded and both are "changed during frame 0" (before any step()).
    auto q = w.query<Position, Mesh>().changed<Position>().skip_pending<Mesh>();
    CHECK(q.count() == 2U);

    // After a step(), neither has changed; both remain loaded.
    w.step(1.0 / 60.0);
    auto q2 = w.query<Position, Mesh>().changed<Position>().skip_pending<Mesh>();
    CHECK(q2.count() == 0U); // no writes during frame 1
}
