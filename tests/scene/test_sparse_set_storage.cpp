// Phase 3.0 v1d — SparseSetStorage tests (ADR-0050 §3).
//
// Coverage:
//   - Lazy pool creation; empty pool reads return null.
//   - Insert / has / get round-trip on a single (entity, component) pair.
//   - UPSERT replaces value; fires on_update (not insert).
//   - Remove on present component; on_remove fires; dense compacts.
//   - Remove on absent component is a no-op (no events fired).
//   - Swap-with-last preserves the trailing entity's data and sparse pointer.
//   - Multiple SparseSet components on one entity live in distinct pools.
//   - on_entity_destroyed sweeps every pool that holds the entity.
//   - for_each_chunk semantics:
//       empty required        → yield every non-empty pool
//       single-bit required   → yield exactly the matching pool
//       multi-bit required    → yield nothing (deferred to v1e)
//   - Large-N stress: 10K inserts and randomised removes.
//   - Per-pool version counter bumps on insert / update / remove.
//   - World-level dispatch: an Archetype-hinted component and a SparseSet-hinted
//     component coexist on the same entity; each goes to its backend; both
//     drain on destroy; the sink sees on_entity_destroyed exactly once.

#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/sparse_set_storage.hpp>
#include <crd/scene/storage_event_sink.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::scene::ChunkView;
using crd::scene::ComponentId;
using crd::scene::ComponentMask;
using crd::scene::ComponentRegistry;
using crd::scene::EntityId;
using crd::scene::IStorageEventSink;
using crd::scene::SparseSetStorage;
using crd::scene::StorageHint;
using crd::scene::World;

namespace
{
struct DialogTrigger
{
    crd::u32 dialog_id = 0;
    crd::u32 priority = 0;
};

struct EditorSelected
{
    crd::u8 marker = 1;
    crd::u8 padding[7]{};
};

struct Position
{
    float x{}, y{}, z{};
};

// Counting sink — records call counts per category. Lets tests assert event
// fire counts and ordering without Catch2 mocks.
struct CountingSink : IStorageEventSink
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

// Visitor scratch used by for_each_chunk tests.
struct VisitedChunk
{
    const EntityId* entities = nullptr;
    crd::u32 entity_count = 0;
    ComponentMask present_mask{};
};

void capture_chunk(const ChunkView& view, void* user_data)
{
    auto* out = static_cast<crd::containers::Array<VisitedChunk>*>(user_data);
    VisitedChunk vc{};
    vc.entities = view.entities;
    vc.entity_count = view.entity_count;
    vc.present_mask = view.present_mask;
    out->push_back(vc);
}

} // namespace

// -----------------------------------------------------------------------------
// Direct SparseSetStorage tests (no World indirection)
// -----------------------------------------------------------------------------

TEST_CASE("SparseSetStorage: empty pool - has=false, get_const=null", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    EntityId e = EntityId::make(7, 1);

    CHECK_FALSE(storage.has(e, id));
    CHECK(storage.get_const(e, id) == nullptr);
    CHECK(storage.entity_count(id) == 0U);
    CHECK(storage.pool_count() == 0U);
    CHECK(storage.pool_version(id) == 0U);
}

TEST_CASE("SparseSetStorage: insert + has + get round-trip", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    EntityId e = EntityId::make(3, 1);

    DialogTrigger value{42U, 7U};
    storage.insert(e, id, &value);

    CHECK(storage.has(e, id));
    CHECK(storage.entity_count(id) == 1U);
    CHECK(storage.pool_count() == 1U);

    const DialogTrigger* got = static_cast<const DialogTrigger*>(storage.get_const(e, id));
    REQUIRE(got != nullptr);
    CHECK(got->dialog_id == 42U);
    CHECK(got->priority == 7U);
}

TEST_CASE("SparseSetStorage: UPSERT replaces value and fires on_update", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    CountingSink sink;
    storage.set_event_sink(&sink);

    EntityId e = EntityId::make(11, 1);

    DialogTrigger v1{1U, 1U};
    storage.insert(e, id, &v1);
    DialogTrigger v2{2U, 2U};
    storage.insert(e, id, &v2);

    CHECK(sink.inserts == 1U);
    CHECK(sink.updates == 1U);
    CHECK(storage.entity_count(id) == 1U);

    const DialogTrigger* got = static_cast<const DialogTrigger*>(storage.get_const(e, id));
    REQUIRE(got != nullptr);
    CHECK(got->dialog_id == 2U);
    CHECK(got->priority == 2U);
}

TEST_CASE("SparseSetStorage: remove on present component compacts dense", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    CountingSink sink;
    storage.set_event_sink(&sink);

    EntityId e1 = EntityId::make(1, 1);
    EntityId e2 = EntityId::make(2, 1);
    EntityId e3 = EntityId::make(3, 1);

    DialogTrigger v1{10U, 0U};
    DialogTrigger v2{20U, 0U};
    DialogTrigger v3{30U, 0U};
    storage.insert(e1, id, &v1);
    storage.insert(e2, id, &v2);
    storage.insert(e3, id, &v3);
    CHECK(storage.entity_count(id) == 3U);

    storage.remove(e2, id);

    CHECK_FALSE(storage.has(e2, id));
    CHECK(storage.has(e1, id));
    CHECK(storage.has(e3, id));
    CHECK(storage.entity_count(id) == 2U);
    CHECK(sink.removes == 1U);

    // Swap-with-last must preserve the trailing entity's data.
    const DialogTrigger* got3 = static_cast<const DialogTrigger*>(storage.get_const(e3, id));
    REQUIRE(got3 != nullptr);
    CHECK(got3->dialog_id == 30U);
}

TEST_CASE("SparseSetStorage: remove on absent component is a no-op", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    CountingSink sink;
    storage.set_event_sink(&sink);

    EntityId e = EntityId::make(5, 1);
    storage.remove(e, id); // no pool yet
    DialogTrigger v{1U, 1U};
    storage.insert(e, id, &v);

    EntityId other = EntityId::make(6, 1);
    storage.remove(other, id); // pool exists, entity absent

    CHECK(sink.removes == 0U);
    CHECK(storage.entity_count(id) == 1U);
}

TEST_CASE("SparseSetStorage: multiple components on one entity - distinct pools", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId dt_id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);
    const ComponentId es_id = registry.register_type<EditorSelected>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    EntityId e = EntityId::make(4, 1);

    DialogTrigger dt{99U, 1U};
    EditorSelected es{};
    storage.insert(e, dt_id, &dt);
    storage.insert(e, es_id, &es);

    CHECK(storage.has(e, dt_id));
    CHECK(storage.has(e, es_id));
    CHECK(storage.pool_count() == 2U);
}

TEST_CASE("SparseSetStorage: on_entity_destroyed sweeps every pool", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId dt_id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);
    const ComponentId es_id = registry.register_type<EditorSelected>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    CountingSink sink;
    storage.set_event_sink(&sink);

    EntityId e = EntityId::make(2, 1);
    DialogTrigger dt{1U, 1U};
    EditorSelected es{};
    storage.insert(e, dt_id, &dt);
    storage.insert(e, es_id, &es);

    storage.on_entity_destroyed(e);

    CHECK_FALSE(storage.has(e, dt_id));
    CHECK_FALSE(storage.has(e, es_id));
    CHECK(sink.removes == 2U);
    // Storage backend MUST NOT fire sink->on_entity_destroyed itself —
    // World drives that exactly once per destroy.
    CHECK(sink.destroys == 0U);
}

TEST_CASE("SparseSetStorage: for_each_chunk single-bit required yields exactly the matching pool",
          "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId dt_id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);
    const ComponentId es_id = registry.register_type<EditorSelected>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};

    EntityId a = EntityId::make(1, 1);
    EntityId b = EntityId::make(2, 1);
    DialogTrigger dt1{1U, 0U};
    DialogTrigger dt2{2U, 0U};
    EditorSelected es{};
    storage.insert(a, dt_id, &dt1);
    storage.insert(b, dt_id, &dt2);
    storage.insert(a, es_id, &es);

    crd::containers::Array<VisitedChunk> visited{alloc};
    ComponentMask required{};
    required.set(dt_id);
    storage.for_each_chunk(required, capture_chunk, &visited);

    REQUIRE(visited.size() == 1U);
    CHECK(visited[0].entity_count == 2U);
    CHECK(visited[0].present_mask.test(dt_id));
    CHECK_FALSE(visited[0].present_mask.test(es_id));
}

TEST_CASE("SparseSetStorage: for_each_chunk empty required yields every non-empty pool",
          "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId dt_id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);
    const ComponentId es_id = registry.register_type<EditorSelected>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};

    EntityId e = EntityId::make(1, 1);
    DialogTrigger dt{1U, 0U};
    EditorSelected es{};
    storage.insert(e, dt_id, &dt);
    storage.insert(e, es_id, &es);

    crd::containers::Array<VisitedChunk> visited{alloc};
    storage.for_each_chunk(ComponentMask{}, capture_chunk, &visited);

    CHECK(visited.size() == 2U); // both pools yielded
}

TEST_CASE("SparseSetStorage: for_each_chunk multi-bit required yields nothing", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId dt_id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);
    const ComponentId es_id = registry.register_type<EditorSelected>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};

    EntityId e = EntityId::make(1, 1);
    DialogTrigger dt{1U, 0U};
    EditorSelected es{};
    storage.insert(e, dt_id, &dt);
    storage.insert(e, es_id, &es);

    crd::containers::Array<VisitedChunk> visited{alloc};
    ComponentMask required{};
    required.set(dt_id);
    required.set(es_id);
    storage.for_each_chunk(required, capture_chunk, &visited);

    CHECK(visited.size() == 0U); // multi-bit deferred to v1e mixed visitor
}

TEST_CASE("SparseSetStorage: per-pool version bumps on insert/update/remove", "[scene][storage][sparseset]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};
    EntityId e = EntityId::make(1, 1);

    CHECK(storage.pool_version(id) == 0U);

    DialogTrigger v{1U, 0U};
    storage.insert(e, id, &v);
    const auto v_after_insert = storage.pool_version(id);
    CHECK(v_after_insert > 0U);

    DialogTrigger v2{2U, 0U};
    storage.insert(e, id, &v2); // UPSERT
    CHECK(storage.pool_version(id) > v_after_insert);

    (void)storage.get_mut(e, id);
    const auto v_after_get_mut = storage.pool_version(id);
    CHECK(v_after_get_mut > v_after_insert);

    storage.remove(e, id);
    CHECK(storage.pool_version(id) > v_after_get_mut);
}

TEST_CASE("SparseSetStorage: 10K-entity stress with random removes preserves data integrity",
          "[scene][storage][sparseset][stress]")
{
    crd::memory::IAllocator* alloc = crd::memory::default_allocator();
    ComponentRegistry registry{alloc};
    const ComponentId id = registry.register_type<DialogTrigger>(StorageHint::SparseSet);

    SparseSetStorage storage{alloc, registry};

    constexpr crd::u32 kN = 10000;

    // Phase 1: insert N entities, value = entity index. Hash in the bits
    // for variety so we can probe later.
    for (crd::u32 i = 0; i < kN; ++i)
    {
        EntityId e = EntityId::make(i + 1U, 1);
        DialogTrigger v{i, i ^ 0xA5A5U};
        storage.insert(e, id, &v);
    }
    CHECK(storage.entity_count(id) == kN);

    // Phase 2: remove every 3rd entity in a deterministic pattern.
    crd::u32 removed = 0;
    for (crd::u32 i = 0; i < kN; i += 3)
    {
        EntityId e = EntityId::make(i + 1U, 1);
        storage.remove(e, id);
        ++removed;
    }
    CHECK(storage.entity_count(id) == kN - removed);

    // Phase 3: every remaining entity must still resolve to its original value.
    for (crd::u32 i = 0; i < kN; ++i)
    {
        EntityId e = EntityId::make(i + 1U, 1);
        if ((i % 3) == 0)
        {
            CHECK_FALSE(storage.has(e, id));
        }
        else
        {
            const DialogTrigger* got = static_cast<const DialogTrigger*>(storage.get_const(e, id));
            REQUIRE(got != nullptr);
            CHECK(got->dialog_id == i);
            CHECK(got->priority == (i ^ 0xA5A5U));
        }
    }
}

// -----------------------------------------------------------------------------
// World-level routing — the "two backends, one entity" property.
// -----------------------------------------------------------------------------

TEST_CASE("World routes Archetype-hinted and SparseSet-hinted components to their backends",
          "[scene][storage][sparseset][world]")
{
    World w;
    // Position defaults to Archetype, DialogTrigger explicitly SparseSet.
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1.0F, 2.0F, 3.0F});
    w.add_component<DialogTrigger>(e, DialogTrigger{42U, 7U});

    CHECK(w.has_component<Position>(e));
    CHECK(w.has_component<DialogTrigger>(e));

    // Archetype storage carries Position; archetype_count reflects Position alone.
    CHECK(w.archetype_count() == 1U);
    // Sparse storage carries DialogTrigger; one pool exists.
    CHECK(w.sparse_storage().pool_count() == 1U);

    const Position* p = w.get_component<Position>(e);
    REQUIRE(p != nullptr);
    CHECK(p->x == 1.0F);

    const DialogTrigger* d = w.get_component<DialogTrigger>(e);
    REQUIRE(d != nullptr);
    CHECK(d->dialog_id == 42U);
}

TEST_CASE("World fires sink->on_entity_destroyed exactly once across both backends",
          "[scene][storage][sparseset][world][events]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    CountingSink sink;
    w.set_storage_event_sink(&sink);

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});
    w.add_component<DialogTrigger>(e, DialogTrigger{1, 1});

    CHECK(sink.inserts == 2U); // one per backend
    CHECK(sink.destroys == 0U);

    w.destroy_immediate(e);

    // Sink sees on_entity_destroyed exactly ONCE despite the entity having
    // components in two backends.
    CHECK(sink.destroys == 1U);
    // And one on_remove per stored component.
    CHECK(sink.removes == 2U);
}

TEST_CASE("World remove_component routes to the right backend", "[scene][storage][sparseset][world]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{0, 0, 0});
    w.add_component<DialogTrigger>(e, DialogTrigger{1, 1});

    w.remove_component<DialogTrigger>(e);
    CHECK_FALSE(w.has_component<DialogTrigger>(e));
    CHECK(w.has_component<Position>(e));

    w.remove_component<Position>(e);
    CHECK_FALSE(w.has_component<Position>(e));
}
