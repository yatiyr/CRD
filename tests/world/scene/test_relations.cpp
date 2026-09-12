// Phase 3.0 v1f — Relations (ADR-0051).
//
// Coverage by built-in:
//   ChildOf      — Archetype + ReverseIndex + Acyclic + Cascade
//   AttachedTo   — Archetype + ReverseIndex + Acyclic + Detach
//   Owns         — Archetype + ReverseIndex + Acyclic + Cascade
//   Targets      — SparseSet + ReverseIndex + SetNull (no Acyclic)
//   DependsOn    — SparseSet + ReverseIndex + Acyclic + SetNull
//   PossessedBy  — SparseSet + ReverseIndex + Detach (no Acyclic)
//
// Plus generic mechanics: register/round-trip, UPSERT, traverse,
// would_form_cycle predicate, deep-tree cascade safety, multiple relations
// on one entity, registration idempotency.

#include <crd/scene/relation.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::scene::Acyclic;
using crd::scene::ComponentId;
using crd::scene::EntityId;
using crd::scene::OnTargetDestroyed;
using crd::scene::Relation;
using crd::scene::ReverseIndex;
using crd::scene::StorageHint;
using crd::scene::World;

using crd::scene::relations::AttachedTo;
using crd::scene::relations::ChildOf;
using crd::scene::relations::DependsOn;
using crd::scene::relations::Owns;
using crd::scene::relations::PossessedBy;
using crd::scene::relations::Targets;

namespace
{
struct CustomLikes
{
}; // user-defined relation tag for non-built-in coverage
} // namespace

// -----------------------------------------------------------------------------
// Generic mechanics
// -----------------------------------------------------------------------------

TEST_CASE("register_relation: basic registration round-trip", "[scene][relation][register]")
{
    World w;
    const ComponentId id = w.register_relation<ChildOf>(StorageHint::Archetype, ReverseIndex{}, Acyclic{},
                                                        OnTargetDestroyed{OnTargetDestroyed::Policy::Cascade});

    CHECK_FALSE(id.is_null());
    // Idempotent re-registration returns the same id.
    const ComponentId id2 = w.register_relation<ChildOf>(StorageHint::Archetype, ReverseIndex{});
    CHECK(id2 == id);
}

TEST_CASE("register_builtin_relations: registers all six", "[scene][relation][register]")
{
    World w;
    w.register_builtin_relations();
    CHECK_FALSE(w.relation_id<ChildOf>().is_null());
    CHECK_FALSE(w.relation_id<AttachedTo>().is_null());
    CHECK_FALSE(w.relation_id<Owns>().is_null());
    CHECK_FALSE(w.relation_id<Targets>().is_null());
    CHECK_FALSE(w.relation_id<DependsOn>().is_null());
    CHECK_FALSE(w.relation_id<PossessedBy>().is_null());
}

TEST_CASE("add_relation + get_relation_target round-trip", "[scene][relation]")
{
    World w;
    w.register_builtin_relations();

    EntityId child = w.spawn();
    EntityId parent = w.spawn();
    w.add_relation<ChildOf>(child, parent);

    CHECK(w.has_relation<ChildOf>(child));
    CHECK(w.get_relation_target<ChildOf>(child).raw == parent.raw);
    // Parent itself has no ChildOf — it's the root.
    CHECK_FALSE(w.has_relation<ChildOf>(parent));
}

TEST_CASE("add_relation UPSERT updates reverse index", "[scene][relation][upsert]")
{
    World w;
    w.register_builtin_relations();

    EntityId child = w.spawn();
    EntityId parent_a = w.spawn();
    EntityId parent_b = w.spawn();

    w.add_relation<ChildOf>(child, parent_a);
    w.add_relation<ChildOf>(child, parent_b); // re-target

    CHECK(w.get_relation_target<ChildOf>(child).raw == parent_b.raw);

    // Traverse from parent_a — child should NOT appear.
    crd::u32 visits_a = 0;
    w.traverse_relation<ChildOf>(parent_a, [&](EntityId, crd::u32) { ++visits_a; });
    CHECK(visits_a == 1U); // only parent_a itself (root visit)

    // Traverse from parent_b — child appears at depth 1.
    bool seen_child = false;
    w.traverse_relation<ChildOf>(parent_b,
                                 [&](EntityId e, crd::u32 depth)
                                 {
                                     if (e.raw == child.raw && depth == 1U)
                                     {
                                         seen_child = true;
                                     }
                                 });
    CHECK(seen_child);
}

TEST_CASE("add_relation UPSERT to same target is a no-op", "[scene][relation][upsert]")
{
    World w;
    w.register_builtin_relations();

    EntityId child = w.spawn();
    EntityId parent = w.spawn();
    w.add_relation<ChildOf>(child, parent);
    w.add_relation<ChildOf>(child, parent); // same target

    // No duplicate entries in reverse index → traverse visits child exactly once.
    crd::u32 child_visits = 0;
    w.traverse_relation<ChildOf>(parent,
                                 [&](EntityId e, crd::u32)
                                 {
                                     if (e.raw == child.raw)
                                     {
                                         ++child_visits;
                                     }
                                 });
    CHECK(child_visits == 1U);
}

TEST_CASE("remove_relation clears + reverse index reflects", "[scene][relation]")
{
    World w;
    w.register_builtin_relations();

    EntityId child = w.spawn();
    EntityId parent = w.spawn();
    w.add_relation<ChildOf>(child, parent);
    CHECK(w.has_relation<ChildOf>(child));

    w.remove_relation<ChildOf>(child);
    CHECK_FALSE(w.has_relation<ChildOf>(child));

    // Traverse from parent — child no longer visited.
    crd::u32 child_visits = 0;
    w.traverse_relation<ChildOf>(parent,
                                 [&](EntityId e, crd::u32)
                                 {
                                     if (e.raw == child.raw)
                                     {
                                         ++child_visits;
                                     }
                                 });
    CHECK(child_visits == 0U);
}

// -----------------------------------------------------------------------------
// Cycle detection (predicate-based — never trips CRD_ASSERT)
// -----------------------------------------------------------------------------

TEST_CASE("would_form_cycle: self-cycle detected", "[scene][relation][cycle]")
{
    World w;
    w.register_builtin_relations();

    EntityId e = w.spawn();
    CHECK(w.would_form_cycle<ChildOf>(e, e));
}

TEST_CASE("would_form_cycle: indirect cycle detected", "[scene][relation][cycle]")
{
    World w;
    w.register_builtin_relations();

    EntityId a = w.spawn();
    EntityId b = w.spawn();
    EntityId c = w.spawn();
    w.add_relation<ChildOf>(a, b); // a -> b
    w.add_relation<ChildOf>(b, c); // b -> c (forms a chain a→b→c)

    // Adding c→a would close the cycle.
    CHECK(w.would_form_cycle<ChildOf>(c, a));
    // Adding c→b would also close (b→c→...→b? no, c→b would create b→c→b).
    CHECK(w.would_form_cycle<ChildOf>(c, b));
    // Standalone target — no cycle.
    EntityId d = w.spawn();
    CHECK_FALSE(w.would_form_cycle<ChildOf>(d, c));
}

TEST_CASE("would_form_cycle: no cycle when target is unrelated", "[scene][relation][cycle]")
{
    World w;
    w.register_builtin_relations();

    EntityId a = w.spawn();
    EntityId b = w.spawn();
    CHECK_FALSE(w.would_form_cycle<ChildOf>(a, b));
}

// -----------------------------------------------------------------------------
// traverse_relation
// -----------------------------------------------------------------------------

TEST_CASE("traverse_relation<ChildOf>: DFS pre-order with depth", "[scene][relation][traverse]")
{
    World w;
    w.register_builtin_relations();

    // Tree:
    //   root
    //   ├── a
    //   │   ├── a1
    //   │   └── a2
    //   └── b
    EntityId root = w.spawn();
    EntityId a = w.spawn();
    EntityId b = w.spawn();
    EntityId a1 = w.spawn();
    EntityId a2 = w.spawn();
    w.add_relation<ChildOf>(a, root);
    w.add_relation<ChildOf>(b, root);
    w.add_relation<ChildOf>(a1, a);
    w.add_relation<ChildOf>(a2, a);

    crd::containers::Array<EntityId> visited;
    crd::containers::Array<crd::u32> depths;
    w.traverse_relation<ChildOf>(root,
                                 [&](EntityId e, crd::u32 d)
                                 {
                                     visited.push_back(e);
                                     depths.push_back(d);
                                 });

    REQUIRE(visited.size() == 5U);
    CHECK(visited[0].raw == root.raw);
    CHECK(depths[0] == 0U);
    // Depths of remaining four: at least one at depth 1, two at depth 2.
    crd::u32 d1 = 0;
    crd::u32 d2 = 0;
    for (crd::usize i = 1; i < depths.size(); ++i)
    {
        if (depths[i] == 1U)
        {
            ++d1;
        }
        if (depths[i] == 2U)
        {
            ++d2;
        }
    }
    CHECK(d1 == 2U);
    CHECK(d2 == 2U);
}

TEST_CASE("traverse_relation: empty subtree visits only root", "[scene][relation][traverse]")
{
    World w;
    w.register_builtin_relations();

    EntityId loner = w.spawn();
    crd::u32 count = 0;
    w.traverse_relation<ChildOf>(loner, [&](EntityId, crd::u32) { ++count; });
    CHECK(count == 1U);
}

// -----------------------------------------------------------------------------
// OnTargetDestroyed: Cascade (ChildOf, Owns)
// -----------------------------------------------------------------------------

TEST_CASE("ChildOf Cascade: destroying parent destroys all descendants", "[scene][relation][cascade]")
{
    World w;
    w.register_builtin_relations();

    EntityId root = w.spawn();
    EntityId a = w.spawn();
    EntityId b = w.spawn();
    EntityId a1 = w.spawn();
    w.add_relation<ChildOf>(a, root);
    w.add_relation<ChildOf>(b, root);
    w.add_relation<ChildOf>(a1, a);

    CHECK(w.entity_count() == 4U);

    w.destroy_immediate(root);

    CHECK_FALSE(w.is_alive(root));
    CHECK_FALSE(w.is_alive(a));
    CHECK_FALSE(w.is_alive(b));
    CHECK_FALSE(w.is_alive(a1));
    CHECK(w.entity_count() == 0U);
}

TEST_CASE("Owns Cascade: identical defaults to ChildOf", "[scene][relation][cascade]")
{
    World w;
    w.register_builtin_relations();

    EntityId emitter = w.spawn();
    EntityId p1 = w.spawn();
    EntityId p2 = w.spawn();
    w.add_relation<Owns>(p1, emitter);
    w.add_relation<Owns>(p2, emitter);

    w.destroy_immediate(emitter);

    CHECK_FALSE(w.is_alive(emitter));
    CHECK_FALSE(w.is_alive(p1));
    CHECK_FALSE(w.is_alive(p2));
}

TEST_CASE("Cascade is iterative (deep tree never overflows stack)", "[scene][relation][cascade][stress]")
{
    World w;
    w.register_builtin_relations();

    // Build a 500-deep chain — recursive cascade would blow the stack on
    // many configurations. Iterative worklist must handle it.
    crd::containers::Array<EntityId> chain;
    chain.push_back(w.spawn()); // root
    for (int i = 1; i < 500; ++i)
    {
        EntityId e = w.spawn();
        w.add_relation<ChildOf>(e, chain[i - 1]);
        chain.push_back(e);
    }

    CHECK(w.entity_count() == 500U);
    w.destroy_immediate(chain[0]);
    CHECK(w.entity_count() == 0U);
}

// -----------------------------------------------------------------------------
// OnTargetDestroyed: Detach (AttachedTo, PossessedBy)
// -----------------------------------------------------------------------------

TEST_CASE("AttachedTo Detach: source survives, relation removed", "[scene][relation][detach]")
{
    World w;
    w.register_builtin_relations();

    EntityId socket = w.spawn();
    EntityId weapon = w.spawn();
    w.add_relation<AttachedTo>(weapon, socket);

    CHECK(w.has_relation<AttachedTo>(weapon));
    CHECK(w.is_alive(weapon));

    w.destroy_immediate(socket);

    CHECK_FALSE(w.is_alive(socket));
    CHECK(w.is_alive(weapon)); // survived — Detach policy
    CHECK_FALSE(w.has_relation<AttachedTo>(weapon));
}

TEST_CASE("PossessedBy Detach: SparseSet-stored Detach", "[scene][relation][detach]")
{
    World w;
    w.register_builtin_relations();

    EntityId character = w.spawn();
    EntityId controller = w.spawn();
    w.add_relation<PossessedBy>(character, controller);

    w.destroy_immediate(controller);

    CHECK(w.is_alive(character));
    CHECK_FALSE(w.has_relation<PossessedBy>(character));
}

// -----------------------------------------------------------------------------
// OnTargetDestroyed: SetNull (Targets, DependsOn)
// -----------------------------------------------------------------------------

TEST_CASE("Targets SetNull: target becomes null on destruction", "[scene][relation][setnull]")
{
    World w;
    w.register_builtin_relations();

    EntityId tracker = w.spawn();
    EntityId tracked = w.spawn();
    w.add_relation<Targets>(tracker, tracked);

    w.destroy_immediate(tracked);

    CHECK(w.is_alive(tracker));
    // Relation component still present, but target is now null.
    CHECK(w.has_relation<Targets>(tracker) == false); // null target → has_relation is false
    const Relation<Targets>* r = w.get_component<Relation<Targets>>(tracker);
    REQUIRE(r != nullptr);
    CHECK(r->target.is_null());
}

TEST_CASE("DependsOn SetNull on a chain", "[scene][relation][setnull]")
{
    World w;
    w.register_builtin_relations();

    EntityId asset = w.spawn();
    EntityId dep = w.spawn();
    w.add_relation<DependsOn>(asset, dep);

    w.destroy_immediate(dep);

    CHECK(w.is_alive(asset));
    const Relation<DependsOn>* r = w.get_component<Relation<DependsOn>>(asset);
    REQUIRE(r != nullptr);
    CHECK(r->target.is_null());
}

// -----------------------------------------------------------------------------
// Multiple relations on one entity
// -----------------------------------------------------------------------------

TEST_CASE("Multiple relations on one entity work independently", "[scene][relation]")
{
    World w;
    w.register_builtin_relations();

    EntityId entity = w.spawn();
    EntityId scene_parent = w.spawn();
    EntityId socket = w.spawn();
    EntityId controller = w.spawn();

    w.add_relation<ChildOf>(entity, scene_parent);
    w.add_relation<AttachedTo>(entity, socket);
    w.add_relation<PossessedBy>(entity, controller);

    CHECK(w.get_relation_target<ChildOf>(entity).raw == scene_parent.raw);
    CHECK(w.get_relation_target<AttachedTo>(entity).raw == socket.raw);
    CHECK(w.get_relation_target<PossessedBy>(entity).raw == controller.raw);

    // Destroying socket detaches AttachedTo only — others untouched.
    w.destroy_immediate(socket);

    CHECK(w.is_alive(entity));
    CHECK(w.has_relation<ChildOf>(entity));
    CHECK_FALSE(w.has_relation<AttachedTo>(entity));
    CHECK(w.has_relation<PossessedBy>(entity));
}

// -----------------------------------------------------------------------------
// User-defined relations
// -----------------------------------------------------------------------------

TEST_CASE("User-defined relation works without traits", "[scene][relation][user-defined]")
{
    World w;
    w.register_relation<CustomLikes>(StorageHint::SparseSet, ReverseIndex{});

    EntityId a = w.spawn();
    EntityId b = w.spawn();
    w.add_relation<CustomLikes>(a, b);

    CHECK(w.get_relation_target<CustomLikes>(a).raw == b.raw);

    // No OnTargetDestroyed → destroying b leaves a's relation dangling
    // (target field still points at b's old EntityId — caller's problem).
    w.destroy_immediate(b);
    CHECK(w.is_alive(a));
    CHECK(w.has_relation<CustomLikes>(a)); // still present (no cleanup policy)
}

// -----------------------------------------------------------------------------
// flush_destroys cascade
// -----------------------------------------------------------------------------

TEST_CASE("flush_destroys runs cascades correctly", "[scene][relation][flush]")
{
    World w;
    w.register_builtin_relations();

    EntityId parent = w.spawn();
    EntityId child = w.spawn();
    w.add_relation<ChildOf>(child, parent);

    w.destroy(parent); // queued
    CHECK(w.is_alive(parent));
    CHECK(w.is_alive(child));

    w.flush_destroys();
    CHECK_FALSE(w.is_alive(parent));
    CHECK_FALSE(w.is_alive(child)); // cascaded
}

// -----------------------------------------------------------------------------
// Outgoing-relation cleanup on plain destroy (no policy)
// -----------------------------------------------------------------------------

TEST_CASE("Destroying a child cleans up its reverse-index entry", "[scene][relation]")
{
    World w;
    w.register_builtin_relations();

    EntityId parent = w.spawn();
    EntityId child = w.spawn();
    w.add_relation<ChildOf>(child, parent);

    w.destroy_immediate(child);

    // Parent survives. Traverse from parent must NOT report the dead child.
    CHECK(w.is_alive(parent));
    crd::u32 child_visits = 0;
    w.traverse_relation<ChildOf>(parent, [&](EntityId, crd::u32 d) { if (d > 0U) { ++child_visits; } });
    CHECK(child_visits == 0U);
}
