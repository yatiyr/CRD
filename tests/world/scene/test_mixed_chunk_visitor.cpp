// Phase 3.0 v1e — World::for_each_chunk (mixed-backend chunk visitor).
//
// Coverage:
//   - Empty required yields every archetype chunk AND every non-empty sparse pool.
//   - Pure-archetype required forwards to ArchetypeChunkStorage (no scratch).
//   - Pure-SparseSet single-bit required forwards to SparseSetStorage.
//   - Pure-SparseSet multi-bit required yields entities present in ALL pools
//     (smallest pool as anchor, sparse-check the others).
//   - Mixed required (1 archetype + 1 sparse) yields archetype-chunk entities
//     filtered by sparse presence.
//   - Mixed required (2 archetype + 2 sparse) full integration.
//   - Edge: archetype_bits never present -> no chunks.
//   - Edge: sparse_bits absent (no pool) -> no chunks.
//   - Edge: pure-sparse with one of two pools empty -> no chunks.
//   - Visitor is never called when intersection is empty.
//   - Recursive call from within visitor (proves stack-local scratch).

#include <crd/scene/component.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using crd::scene::ChunkView;
using crd::scene::ComponentMask;
using crd::scene::EntityId;
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
struct DialogTrigger
{
    crd::u32 dialog_id = 0;
};
struct EditorSelected
{
    crd::u8 marker = 1;
};

struct Collected
{
    crd::containers::Array<EntityId> entities{};
    crd::u32 chunks_visited = 0;
};

void collect(const ChunkView& view, void* user_data)
{
    auto* out = static_cast<Collected*>(user_data);
    ++out->chunks_visited;
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        out->entities.push_back(view.entities[i]);
    }
}

bool contains(const crd::containers::Array<EntityId>& xs, EntityId e)
{
    return std::any_of(xs.begin(), xs.end(), [&](EntityId x) { return x.raw == e.raw; });
}

} // namespace

// ---------------------------------------------------------------------------
// Pure-archetype path
// ---------------------------------------------------------------------------

TEST_CASE("World::for_each_chunk: pure-archetype required forwards to ArchetypeChunkStorage",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<Position>(e1, {});
    w.add_component<Position>(e2, {});
    w.add_component<Velocity>(e2, {});

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<Position>());
    w.for_each_chunk(req, &collect, &c);

    // Should yield BOTH archetypes ({P} containing e1, {P,V} containing e2).
    CHECK(c.entities.size() == 2U);
    CHECK(contains(c.entities, e1));
    CHECK(contains(c.entities, e2));
}

// ---------------------------------------------------------------------------
// Pure-SparseSet single-bit path
// ---------------------------------------------------------------------------

TEST_CASE("World::for_each_chunk: pure-SparseSet single-bit required forwards to SparseSetStorage",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<DialogTrigger>(e1, DialogTrigger{1U});
    w.add_component<DialogTrigger>(e2, DialogTrigger{2U});

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<DialogTrigger>());
    w.for_each_chunk(req, &collect, &c);

    CHECK(c.entities.size() == 2U);
    CHECK(contains(c.entities, e1));
    CHECK(contains(c.entities, e2));
}

// ---------------------------------------------------------------------------
// Empty required — yields every archetype chunk + every non-empty sparse pool
// ---------------------------------------------------------------------------

TEST_CASE("World::for_each_chunk: empty required yields every chunk in BOTH backends",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId e1 = w.spawn(); // P only
    EntityId e2 = w.spawn(); // DT only
    EntityId e3 = w.spawn(); // both
    w.add_component<Position>(e1, {});
    w.add_component<DialogTrigger>(e2, DialogTrigger{1U});
    w.add_component<Position>(e3, {});
    w.add_component<DialogTrigger>(e3, DialogTrigger{2U});

    Collected c{};
    w.for_each_chunk(ComponentMask{}, &collect, &c);

    // {P} archetype: e1, e3. DialogTrigger pool: e2, e3.
    // Total visits: 2 (e1, e3) + 2 (e2, e3) = 4. e3 appears twice (once per backend).
    CHECK(c.entities.size() == 4U);
    CHECK(contains(c.entities, e1));
    CHECK(contains(c.entities, e2));
    CHECK(contains(c.entities, e3));
}

// ---------------------------------------------------------------------------
// Pure-SparseSet multi-bit — anchor + intersection
// ---------------------------------------------------------------------------

TEST_CASE("World::for_each_chunk: pure-SparseSet multi-bit yields the intersection",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<DialogTrigger>(StorageHint::SparseSet);
    w.register_component<EditorSelected>(StorageHint::SparseSet);

    EntityId only_dt = w.spawn();
    EntityId only_es = w.spawn();
    EntityId both = w.spawn();
    w.add_component<DialogTrigger>(only_dt, DialogTrigger{1U});
    w.add_component<EditorSelected>(only_es, EditorSelected{});
    w.add_component<DialogTrigger>(both, DialogTrigger{2U});
    w.add_component<EditorSelected>(both, EditorSelected{});

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<DialogTrigger>());
    req.set(w.component_id<EditorSelected>());
    w.for_each_chunk(req, &collect, &c);

    CHECK(c.entities.size() == 1U);
    CHECK(contains(c.entities, both));
    CHECK_FALSE(contains(c.entities, only_dt));
    CHECK_FALSE(contains(c.entities, only_es));
}

TEST_CASE("World::for_each_chunk: pure-SparseSet multi-bit with one pool empty yields nothing",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<DialogTrigger>(StorageHint::SparseSet);
    w.register_component<EditorSelected>(StorageHint::SparseSet);

    EntityId e = w.spawn();
    w.add_component<DialogTrigger>(e, DialogTrigger{1U});
    // EditorSelected pool never populated.

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<DialogTrigger>());
    req.set(w.component_id<EditorSelected>());
    w.for_each_chunk(req, &collect, &c);

    CHECK(c.entities.size() == 0U);
    CHECK(c.chunks_visited == 0U);
}

// ---------------------------------------------------------------------------
// Mixed-backend
// ---------------------------------------------------------------------------

TEST_CASE("World::for_each_chunk: mixed (Position + DialogTrigger) yields filtered intersection",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId only_p = w.spawn();
    EntityId only_dt = w.spawn();
    EntityId both = w.spawn();
    w.add_component<Position>(only_p, Position{1, 0, 0});
    w.add_component<DialogTrigger>(only_dt, DialogTrigger{1U});
    w.add_component<Position>(both, Position{2, 0, 0});
    w.add_component<DialogTrigger>(both, DialogTrigger{2U});

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<Position>());
    req.set(w.component_id<DialogTrigger>());
    w.for_each_chunk(req, &collect, &c);

    CHECK(c.entities.size() == 1U);
    CHECK(contains(c.entities, both));
    CHECK_FALSE(contains(c.entities, only_p));
    CHECK_FALSE(contains(c.entities, only_dt));
}

TEST_CASE("World::for_each_chunk: mixed with two archetype + two sparse bits",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);
    w.register_component<EditorSelected>(StorageHint::SparseSet);

    EntityId all_four = w.spawn();
    EntityId no_dt = w.spawn();
    EntityId no_es = w.spawn();

    w.add_component<Position>(all_four, {});
    w.add_component<Velocity>(all_four, {});
    w.add_component<DialogTrigger>(all_four, {});
    w.add_component<EditorSelected>(all_four, {});

    w.add_component<Position>(no_dt, {});
    w.add_component<Velocity>(no_dt, {});
    w.add_component<EditorSelected>(no_dt, {});
    // missing DialogTrigger

    w.add_component<Position>(no_es, {});
    w.add_component<Velocity>(no_es, {});
    w.add_component<DialogTrigger>(no_es, {});
    // missing EditorSelected

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<Position>());
    req.set(w.component_id<Velocity>());
    req.set(w.component_id<DialogTrigger>());
    req.set(w.component_id<EditorSelected>());
    w.for_each_chunk(req, &collect, &c);

    CHECK(c.entities.size() == 1U);
    CHECK(contains(c.entities, all_four));
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

TEST_CASE("World::for_each_chunk: archetype bit never present yields nothing",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    // No entity with both Position AND Velocity.
    EntityId e = w.spawn();
    w.add_component<Position>(e, {});
    w.add_component<DialogTrigger>(e, {});

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<Position>());
    req.set(w.component_id<Velocity>());
    req.set(w.component_id<DialogTrigger>());
    w.for_each_chunk(req, &collect, &c);

    // No archetype matches {P,V}, so mixed loop never visits any chunk.
    CHECK(c.entities.size() == 0U);
    CHECK(c.chunks_visited == 0U);
}

TEST_CASE("World::for_each_chunk: empty world yields nothing for any required",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<Position>());
    req.set(w.component_id<DialogTrigger>());
    w.for_each_chunk(req, &collect, &c);

    CHECK(c.entities.size() == 0U);
    CHECK(c.chunks_visited == 0U);
}

// ---------------------------------------------------------------------------
// Visitor never called when intersection is empty
// ---------------------------------------------------------------------------

TEST_CASE("World::for_each_chunk: visitor is not invoked for empty filtered chunks",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    // Many P entities but ZERO that also have DialogTrigger.
    for (int i = 0; i < 10; ++i)
    {
        EntityId e = w.spawn();
        w.add_component<Position>(e, Position{static_cast<float>(i), 0, 0});
    }

    Collected c{};
    ComponentMask req{};
    req.set(w.component_id<Position>());
    req.set(w.component_id<DialogTrigger>());
    w.for_each_chunk(req, &collect, &c);

    CHECK(c.chunks_visited == 0U);
    CHECK(c.entities.size() == 0U);
}

// ---------------------------------------------------------------------------
// Recursive call from inside visitor — proves scratch is stack-local
// ---------------------------------------------------------------------------

namespace
{
struct RecursiveCtx
{
    World* world{};
    Collected outer{};
    Collected inner{};
};

void recursive_visitor(const ChunkView& view, void* user_data)
{
    auto* rc = static_cast<RecursiveCtx*>(user_data);
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        rc->outer.entities.push_back(view.entities[i]);
    }
    ++rc->outer.chunks_visited;

    // Re-enter for_each_chunk from inside the visitor — would corrupt a member
    // scratch. Stack-local scratch makes this safe.
    ComponentMask inner_req{};
    inner_req.set(rc->world->component_id<DialogTrigger>());
    rc->world->for_each_chunk(inner_req, &collect, &rc->inner);
}
} // namespace

TEST_CASE("World::for_each_chunk: recursive call from visitor body is safe (stack-local scratch)",
          "[scene][world][mixed-visitor]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<Position>(e1, {});
    w.add_component<DialogTrigger>(e1, DialogTrigger{1U});
    w.add_component<Position>(e2, {});
    w.add_component<DialogTrigger>(e2, DialogTrigger{2U});

    RecursiveCtx rc{};
    rc.world = &w;
    ComponentMask outer_req{};
    outer_req.set(w.component_id<Position>());
    outer_req.set(w.component_id<DialogTrigger>());
    w.for_each_chunk(outer_req, &recursive_visitor, &rc);

    // Outer: filtered chunk with both entities.
    CHECK(rc.outer.entities.size() == 2U);
    // Inner: pure-sparse forward, both entities.
    // Visitor is invoked once per chunk in outer (one filtered chunk), so the
    // inner walk runs once with both entities.
    CHECK(rc.inner.entities.size() == 2U);
}
