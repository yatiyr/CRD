// Phase 3.0 v1g — Query DSL tests (ADR-0052 §1).
//
// Coverage:
//   - Empty query yields all entities (across both backends).
//   - Single + multi-component queries (yield tuple shape).
//   - .with<T>() requires T without yielding T.
//   - .without<T>() excludes entities with T.
//   - .with_relation<Tag>(target) anchors on a specific target.
//   - .with_relation<Tag>() (no target) accepts any non-null relation.
//   - .filter(fn, ud) applies a predicate.
//   - Composed: with + without + with_relation + filter.
//   - Mixed-backend (Archetype + SparseSet components).
//   - Range-for tuple-yields (EntityId, Cs&...).
//   - Chunk visitor yields filtered chunks.
//   - Empty result → empty range / no visits.
//   - Mutation through range-for body round-trips.
//   - Multiple relation filters compose (AND).
//   - Caching: query iterated twice doesn't re-walk.
//   - Pure-SparseSet query.
//   - Forbidden bit covers both backends correctly.

#include <crd/scene/query.hpp>
#include <crd/scene/relation.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::scene::ChunkView;
using crd::scene::EntityId;
using crd::scene::StorageHint;
using crd::scene::World;

using crd::scene::relations::AttachedTo;
using crd::scene::relations::ChildOf;

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
struct Visible
{
    crd::u8 unused{};
};
struct Hidden
{
    crd::u8 unused{};
};
struct DialogTrigger
{
    crd::u32 dialog_id = 0;
};

bool position_x_above_threshold(EntityId e, const World* w, void* ud)
{
    auto* threshold = static_cast<float*>(ud);
    const Position* p = w->get_component<Position>(e);
    if (p == nullptr)
    {
        return false;
    }
    return p->x > *threshold;
}

struct ChunkAccum
{
    crd::u32 chunks_seen = 0;
    crd::u32 entities_seen = 0;
};

void accum_chunk(const ChunkView& view, void* user_data)
{
    auto* acc = static_cast<ChunkAccum*>(user_data);
    ++acc->chunks_seen;
    acc->entities_seen += view.entity_count;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction + base
// ---------------------------------------------------------------------------

TEST_CASE("query<>() returns Query<> with no required components", "[scene][query][construction]")
{
    World w;
    w.register_component<Position>();

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<Position>(e1, {});
    w.add_component<Position>(e2, {});

    auto q = w.query<>();
    CHECK(q.count() >= 2U); // empty required matches every chunk in both backends
}

TEST_CASE("query<T>(): single-component query yields matching entities", "[scene][query]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();

    EntityId e_p = w.spawn();
    EntityId e_pv = w.spawn();
    w.add_component<Position>(e_p, Position{1, 0, 0});
    w.add_component<Position>(e_pv, Position{2, 0, 0});
    w.add_component<Velocity>(e_pv, {});

    auto q = w.query<Position>();
    CHECK(q.count() == 2U);
}

TEST_CASE("query<T1, T2>(): multi-component query requires all", "[scene][query]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();

    EntityId only_p = w.spawn();
    EntityId both = w.spawn();
    w.add_component<Position>(only_p, {});
    w.add_component<Position>(both, {});
    w.add_component<Velocity>(both, {});

    auto q = w.query<Position, Velocity>();
    CHECK(q.count() == 1U);
    REQUIRE(q.matches().size() == 1U);
    CHECK(q.matches()[0].raw == both.raw);
}

// ---------------------------------------------------------------------------
// Filter chain
// ---------------------------------------------------------------------------

TEST_CASE(".with<T>() adds requirement without yielding T", "[scene][query][with]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Visible>();

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<Position>(e1, {});
    w.add_component<Position>(e2, {});
    w.add_component<Visible>(e2, {});

    auto q = w.query<Position>().with<Visible>();
    CHECK(q.count() == 1U);
    CHECK(q.matches()[0].raw == e2.raw);
}

TEST_CASE(".without<T>() excludes entities with T", "[scene][query][without]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Hidden>();

    EntityId visible = w.spawn();
    EntityId hidden = w.spawn();
    w.add_component<Position>(visible, {});
    w.add_component<Position>(hidden, {});
    w.add_component<Hidden>(hidden, {});

    auto q = w.query<Position>().without<Hidden>();
    CHECK(q.count() == 1U);
    CHECK(q.matches()[0].raw == visible.raw);
}

TEST_CASE(".with<>() and .without<>() compose", "[scene][query][compose]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Visible>();
    w.register_component<Hidden>();

    EntityId only_p = w.spawn();
    EntityId p_visible = w.spawn();
    EntityId p_hidden = w.spawn();
    EntityId p_visible_hidden = w.spawn();

    for (EntityId e : {only_p, p_visible, p_hidden, p_visible_hidden})
    {
        w.add_component<Position>(e, {});
    }
    w.add_component<Visible>(p_visible, {});
    w.add_component<Hidden>(p_hidden, {});
    w.add_component<Visible>(p_visible_hidden, {});
    w.add_component<Hidden>(p_visible_hidden, {});

    auto q = w.query<Position>().with<Visible>().without<Hidden>();
    CHECK(q.count() == 1U);
    CHECK(q.matches()[0].raw == p_visible.raw);
}

// ---------------------------------------------------------------------------
// Relation filters
// ---------------------------------------------------------------------------

TEST_CASE(".with_relation<ChildOf>(parent) filters to children of parent", "[scene][query][relation]")
{
    World w;
    w.register_component<Position>();
    w.register_builtin_relations();

    EntityId parent = w.spawn();
    EntityId other_parent = w.spawn();
    EntityId child_a = w.spawn();
    EntityId child_b = w.spawn();
    EntityId other_child = w.spawn();
    w.add_component<Position>(child_a, {});
    w.add_component<Position>(child_b, {});
    w.add_component<Position>(other_child, {});
    w.add_relation<ChildOf>(child_a, parent);
    w.add_relation<ChildOf>(child_b, parent);
    w.add_relation<ChildOf>(other_child, other_parent);

    auto q = w.query<Position>().with_relation<ChildOf>(parent);
    CHECK(q.count() == 2U);
}

TEST_CASE(".with_relation<ChildOf>() (no target) accepts any non-null relation", "[scene][query][relation]")
{
    World w;
    w.register_component<Position>();
    w.register_builtin_relations();

    EntityId root = w.spawn();
    EntityId child_a = w.spawn();
    EntityId child_b = w.spawn();
    w.add_component<Position>(root, {});
    w.add_component<Position>(child_a, {});
    w.add_component<Position>(child_b, {});
    w.add_relation<ChildOf>(child_a, root);
    w.add_relation<ChildOf>(child_b, root);

    auto q = w.query<Position>().with_relation<ChildOf>();
    CHECK(q.count() == 2U); // root has no ChildOf — excluded
}

TEST_CASE("Multiple relation filters compose (AND)", "[scene][query][relation][compose]")
{
    World w;
    w.register_component<Position>();
    w.register_builtin_relations();

    EntityId scene_parent = w.spawn();
    EntityId socket = w.spawn();
    EntityId attached_child = w.spawn(); // both ChildOf scene_parent + AttachedTo socket
    EntityId only_child = w.spawn();     // only ChildOf scene_parent
    EntityId only_attached = w.spawn();  // only AttachedTo socket

    for (EntityId e : {attached_child, only_child, only_attached})
    {
        w.add_component<Position>(e, {});
    }
    w.add_relation<ChildOf>(attached_child, scene_parent);
    w.add_relation<ChildOf>(only_child, scene_parent);
    w.add_relation<AttachedTo>(attached_child, socket);
    w.add_relation<AttachedTo>(only_attached, socket);

    auto q =
        w.query<Position>().with_relation<ChildOf>(scene_parent).with_relation<AttachedTo>(socket);
    CHECK(q.count() == 1U);
    CHECK(q.matches()[0].raw == attached_child.raw);
}

TEST_CASE("Relation target that doesn't exist yields empty", "[scene][query][relation][edge]")
{
    World w;
    w.register_component<Position>();
    w.register_builtin_relations();

    EntityId real_parent = w.spawn();
    EntityId child = w.spawn();
    w.add_component<Position>(child, {});
    w.add_relation<ChildOf>(child, real_parent);

    EntityId fake_parent = EntityId::make(99999, 1); // never spawned
    auto q = w.query<Position>().with_relation<ChildOf>(fake_parent);
    CHECK(q.count() == 0U);
}

// ---------------------------------------------------------------------------
// Predicate filter
// ---------------------------------------------------------------------------

TEST_CASE(".filter(predicate) applies arbitrary entity predicate", "[scene][query][filter]")
{
    World w;
    w.register_component<Position>();

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    EntityId e3 = w.spawn();
    w.add_component<Position>(e1, Position{1, 0, 0});
    w.add_component<Position>(e2, Position{2, 0, 0});
    w.add_component<Position>(e3, Position{3, 0, 0});

    float threshold = 1.5F;
    auto q = w.query<Position>().filter(&position_x_above_threshold, &threshold);
    CHECK(q.count() == 2U); // e2 (x=2), e3 (x=3)
}

// ---------------------------------------------------------------------------
// Mixed-backend
// ---------------------------------------------------------------------------

TEST_CASE("Mixed-backend query: Archetype + SparseSet components", "[scene][query][mixed]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId only_p = w.spawn();
    EntityId both = w.spawn();
    w.add_component<Position>(only_p, {});
    w.add_component<Position>(both, {});
    w.add_component<DialogTrigger>(both, DialogTrigger{42});

    auto q = w.query<Position>().with<DialogTrigger>();
    CHECK(q.count() == 1U);
    CHECK(q.matches()[0].raw == both.raw);
}

TEST_CASE("Pure-SparseSet query", "[scene][query][sparseset]")
{
    World w;
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<DialogTrigger>(e1, DialogTrigger{1});
    w.add_component<DialogTrigger>(e2, DialogTrigger{2});

    auto q = w.query<DialogTrigger>();
    CHECK(q.count() == 2U);
}

TEST_CASE("Forbidden bit on SparseSet-stored component", "[scene][query][forbidden]")
{
    World w;
    w.register_component<Position>();
    w.register_component<DialogTrigger>(StorageHint::SparseSet);

    EntityId clean = w.spawn();
    EntityId tagged = w.spawn();
    w.add_component<Position>(clean, {});
    w.add_component<Position>(tagged, {});
    w.add_component<DialogTrigger>(tagged, {});

    auto q = w.query<Position>().without<DialogTrigger>();
    CHECK(q.count() == 1U);
    CHECK(q.matches()[0].raw == clean.raw);
}

// ---------------------------------------------------------------------------
// Iteration shapes
// ---------------------------------------------------------------------------

TEST_CASE("Range-for yields tuple of EntityId and component refs", "[scene][query][range-for]")
{
    World w;
    w.register_component<Position>();

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<Position>(e1, Position{1, 0, 0});
    w.add_component<Position>(e2, Position{2, 0, 0});

    auto q = w.query<Position>();
    crd::u32 visit_count = 0;
    float sum_x = 0.0F;
    for (auto&& [entity, p] : q)
    {
        ++visit_count;
        sum_x += p.x;
        (void)entity;
    }
    CHECK(visit_count == 2U);
    CHECK(sum_x == 3.0F);
}

TEST_CASE("Range-for body mutates components and writes round-trip", "[scene][query][mutate]")
{
    World w;
    w.register_component<Position>();

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});

    auto q = w.query<Position>();
    for (auto&& [_, p] : q)
    {
        p.x = 100.0F;
        p.y = 200.0F;
    }

    const Position* read = w.get_component<Position>(e);
    REQUIRE(read != nullptr);
    CHECK(read->x == 100.0F);
    CHECK(read->y == 200.0F);
}

TEST_CASE("for_each_chunk yields filtered chunks", "[scene][query][chunk-visitor]")
{
    World w;
    w.register_component<Position>();

    for (int i = 0; i < 5; ++i)
    {
        EntityId e = w.spawn();
        w.add_component<Position>(e, Position{static_cast<float>(i), 0, 0});
    }

    ChunkAccum acc{};
    w.query<Position>().for_each_chunk(&accum_chunk, &acc);
    CHECK(acc.chunks_seen >= 1U);
    CHECK(acc.entities_seen == 5U);
}

// ---------------------------------------------------------------------------
// Edges
// ---------------------------------------------------------------------------

TEST_CASE("Empty result yields empty range and no visits", "[scene][query][edge]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Visible>();

    EntityId e = w.spawn();
    w.add_component<Position>(e, {}); // no Visible

    auto q = w.query<Position>().with<Visible>();
    CHECK(q.count() == 0U);

    crd::u32 visit_count = 0;
    for ([[maybe_unused]] auto&& tup : q)
    {
        ++visit_count;
    }
    CHECK(visit_count == 0U);

    ChunkAccum acc{};
    q.for_each_chunk(&accum_chunk, &acc);
    CHECK(acc.chunks_seen == 0U);
}

TEST_CASE("Empty world yields empty query", "[scene][query][edge]")
{
    World w;
    w.register_component<Position>();

    auto q = w.query<Position>();
    CHECK(q.count() == 0U);
}

// ---------------------------------------------------------------------------
// Caching
// ---------------------------------------------------------------------------

TEST_CASE("Query iterated twice reuses the materialised cache", "[scene][query][cache]")
{
    World w;
    w.register_component<Position>();

    EntityId e = w.spawn();
    w.add_component<Position>(e, {});

    auto q = w.query<Position>();
    const auto& first = q.matches();
    const auto& second = q.matches();

    // Same backing pointer — cache reused (not re-materialised).
    CHECK(first.data() == second.data());
    CHECK(first.size() == 1U);
}

TEST_CASE("Adding a filter after iteration invalidates the cache", "[scene][query][cache][invalidate]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Visible>();

    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<Position>(e1, {});
    w.add_component<Position>(e2, {});
    w.add_component<Visible>(e1, {});

    auto q = w.query<Position>();
    CHECK(q.count() == 2U); // before filter
    q.with<Visible>();
    CHECK(q.count() == 1U); // re-materialised with new filter
}

// ---------------------------------------------------------------------------
// Composed maximum-coverage test
// ---------------------------------------------------------------------------

TEST_CASE("Composed: with + without + with_relation + filter", "[scene][query][compose]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Visible>();
    w.register_component<Hidden>();
    w.register_builtin_relations();

    EntityId parent = w.spawn();

    EntityId match = w.spawn(); // ChildOf parent + Position + Visible, no Hidden, x > threshold
    EntityId no_relation = w.spawn();
    EntityId hidden_one = w.spawn();
    EntityId low_x = w.spawn();

    for (EntityId e : {match, no_relation, hidden_one, low_x})
    {
        w.add_component<Visible>(e, {});
    }
    w.add_component<Position>(match, Position{10, 0, 0});
    w.add_component<Position>(no_relation, Position{10, 0, 0});
    w.add_component<Position>(hidden_one, Position{10, 0, 0});
    w.add_component<Position>(low_x, Position{0, 0, 0});

    w.add_component<Hidden>(hidden_one, {});

    w.add_relation<ChildOf>(match, parent);
    w.add_relation<ChildOf>(hidden_one, parent);
    w.add_relation<ChildOf>(low_x, parent);

    float threshold = 5.0F;
    auto q = w.query<Position>()
                 .with<Visible>()
                 .without<Hidden>()
                 .with_relation<ChildOf>(parent)
                 .filter(&position_x_above_threshold, &threshold);

    CHECK(q.count() == 1U);
    CHECK(q.matches()[0].raw == match.raw);
}
