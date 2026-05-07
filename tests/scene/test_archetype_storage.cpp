#include <crd/scene/archetype_chunk_storage.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/component_registry.hpp>
#include <crd/scene/storage_event_sink.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::scene::ChunkView;
using crd::scene::ComponentId;
using crd::scene::ComponentMask;
using crd::scene::EntityId;
using crd::scene::EntityLocation;
using crd::scene::IStorageEventSink;
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

struct Renderable
{
    crd::u64 mesh{};
    crd::u32 flags{};
};

bool operator==(const Position& a, const Position& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
bool operator==(const Velocity& a, const Velocity& b)
{
    return a.dx == b.dx && a.dy == b.dy && a.dz == b.dz;
}
} // namespace

TEST_CASE("Empty World has no archetypes and storage location is invalid for any entity", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    EntityId e = w.spawn();

    CHECK(w.archetype_count() == 0U);
    CHECK(w.storage().location(e).archetype.is_null());
    CHECK_FALSE(w.has_component<Position>(e));
    CHECK(w.get_component<Position>(e) == nullptr);
}

TEST_CASE("add_component<T> creates archetype, places entity, populates value", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1.0F, 2.0F, 3.0F});

    CHECK(w.has_component<Position>(e));
    CHECK(w.archetype_count() == 1U);

    const Position* p = w.get_component<Position>(e);
    REQUIRE(p != nullptr);
    CHECK(*p == Position{1.0F, 2.0F, 3.0F});

    EntityLocation loc = w.storage().location(e);
    CHECK_FALSE(loc.archetype.is_null());
    CHECK(loc.chunk_index == 0U);
    CHECK(loc.slot_in_chunk == 0U);
}

TEST_CASE("get_component_mut returns mutable pointer; writes round-trip", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{0, 0, 0});

    Position* mut = w.get_component_mut<Position>(e);
    REQUIRE(mut != nullptr);
    mut->x = 42.0F;
    mut->y = -7.5F;

    const Position* p = w.get_component<Position>(e);
    REQUIRE(p != nullptr);
    CHECK(p->x == 42.0F);
    CHECK(p->y == -7.5F);
}

TEST_CASE("add_component is upsert - second add overwrites", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    EntityId e = w.spawn();

    w.add_component<Position>(e, Position{1, 2, 3});
    w.add_component<Position>(e, Position{4, 5, 6});

    const Position* p = w.get_component<Position>(e);
    REQUIRE(p != nullptr);
    CHECK(*p == Position{4, 5, 6});
    CHECK(w.archetype_count() == 1U); // no new archetype on upsert
}

TEST_CASE("Adding a second component moves entity to a new archetype", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();
    EntityId e = w.spawn();

    w.add_component<Position>(e, Position{1, 2, 3});
    EntityLocation loc1 = w.storage().location(e);

    w.add_component<Velocity>(e, Velocity{10, 20, 30});

    EntityLocation loc2 = w.storage().location(e);
    CHECK(loc1.archetype != loc2.archetype);
    CHECK(w.archetype_count() == 2U);

    // Both components survive the move.
    CHECK(w.has_component<Position>(e));
    CHECK(w.has_component<Velocity>(e));
    CHECK(*w.get_component<Position>(e) == Position{1, 2, 3});
    CHECK(*w.get_component<Velocity>(e) == Velocity{10, 20, 30});
}

TEST_CASE("remove_component moves entity to (mask & ~T) archetype", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();
    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});
    w.add_component<Velocity>(e, Velocity{4, 5, 6});

    w.remove_component<Position>(e);

    CHECK_FALSE(w.has_component<Position>(e));
    CHECK(w.has_component<Velocity>(e));
    CHECK(*w.get_component<Velocity>(e) == Velocity{4, 5, 6});
}

TEST_CASE("Removing the last component clears the entity's storage location", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});
    REQUIRE_FALSE(w.storage().location(e).archetype.is_null());

    w.remove_component<Position>(e);

    CHECK(w.storage().location(e).archetype.is_null());
    CHECK_FALSE(w.has_component<Position>(e));
}

TEST_CASE("Two entities in the same archetype get distinct slots", "[scene][storage]")
{
    World w;
    w.register_component<Position>();
    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();

    w.add_component<Position>(e1, Position{1, 0, 0});
    w.add_component<Position>(e2, Position{0, 2, 0});

    EntityLocation l1 = w.storage().location(e1);
    EntityLocation l2 = w.storage().location(e2);
    CHECK(l1.archetype == l2.archetype);
    CHECK(l1.chunk_index == l2.chunk_index);
    CHECK(l1.slot_in_chunk != l2.slot_in_chunk);

    CHECK(*w.get_component<Position>(e1) == Position{1, 0, 0});
    CHECK(*w.get_component<Position>(e2) == Position{0, 2, 0});
}

TEST_CASE("swap_remove updates the trailing entity's location", "[scene][storage]")
{
    World w;
    w.register_component<Position>();
    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    EntityId e3 = w.spawn();
    w.add_component<Position>(e1, Position{1, 0, 0});
    w.add_component<Position>(e2, Position{0, 2, 0});
    w.add_component<Position>(e3, Position{0, 0, 3});

    // Remove e2 from the middle. swap_remove pulls e3 into e2's slot.
    w.remove_component<Position>(e2);
    CHECK_FALSE(w.has_component<Position>(e2));

    // e3's data should still resolve correctly (its location must have been
    // patched to the freed slot).
    REQUIRE(w.has_component<Position>(e3));
    const Position* p3 = w.get_component<Position>(e3);
    REQUIRE(p3 != nullptr);
    CHECK(*p3 == Position{0, 0, 3});
}

TEST_CASE("Chunk fill+spill: more entities than chunk capacity allocate a second chunk", "[scene][storage][chunk]")
{
    World w;
    w.register_component<Position>();

    // With 12-byte Position (~20 B per entity including the EntityId array),
    // one chunk fits ~800 entities; 1500 forces a second chunk.
    crd::containers::Array<EntityId> entities;
    constexpr int kN = 1500;
    for (int i = 0; i < kN; ++i)
    {
        EntityId e = w.spawn();
        w.add_component<Position>(e, Position{static_cast<float>(i), 0, 0});
        entities.push_back(e);
    }

    // All entities resolve correctly.
    for (int i = 0; i < kN; ++i)
    {
        const Position* p = w.get_component<Position>(entities[i]);
        REQUIRE(p != nullptr);
        CHECK(p->x == static_cast<float>(i));
    }

    EntityLocation loc_first = w.storage().location(entities[0]);
    REQUIRE_FALSE(loc_first.archetype.is_null());
    const auto* arch = w.storage().graph().by_id(loc_first.archetype);
    REQUIRE(arch != nullptr);
    CHECK(arch->chunks.size() >= 2U);
}

TEST_CASE("destroy_immediate clears storage location and tears down components", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();
    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});
    w.add_component<Velocity>(e, Velocity{4, 5, 6});

    w.destroy_immediate(e);

    CHECK_FALSE(w.is_alive(e));
    CHECK(w.storage().location(e).archetype.is_null());
}

TEST_CASE("flush_destroys tears down components for queued entities", "[scene][storage][world]")
{
    World w;
    w.register_component<Position>();
    EntityId e1 = w.spawn();
    EntityId e2 = w.spawn();
    w.add_component<Position>(e1, Position{1, 0, 0});
    w.add_component<Position>(e2, Position{0, 2, 0});

    w.destroy(e1);
    w.flush_destroys();

    CHECK_FALSE(w.is_alive(e1));
    CHECK(w.storage().location(e1).archetype.is_null());
    CHECK(w.is_alive(e2));
    CHECK(w.has_component<Position>(e2));
}

TEST_CASE("for_each_chunk visits SUPERSET archetypes only", "[scene][storage][query]")
{
    World w;
    auto pos_id = w.register_component<Position>();
    w.register_component<Velocity>();

    EntityId only_pos = w.spawn();
    EntityId pos_and_vel = w.spawn();
    EntityId only_vel = w.spawn();
    w.add_component<Position>(only_pos, Position{});
    w.add_component<Position>(pos_and_vel, Position{});
    w.add_component<Velocity>(pos_and_vel, Velocity{});
    w.add_component<Velocity>(only_vel, Velocity{});

    // Required mask = {Position}. Should match (Position) and (Position,Velocity)
    // archetypes - but NOT (Velocity)-only.
    ComponentMask required{};
    required.set(pos_id);

    struct VisitState
    {
        int chunk_visits = 0;
        int total_entities = 0;
    };
    VisitState state{};

    w.storage().for_each_chunk(
        required,
        [](const ChunkView& view, void* ud)
        {
            auto* s = static_cast<VisitState*>(ud);
            s->chunk_visits += 1;
            s->total_entities += static_cast<int>(view.entity_count);
        },
        &state);

    // Two archetypes are supersets of {Position}: {Pos} (1 entity) and {Pos,Vel} (1 entity).
    // {Vel}-only archetype must be skipped.
    CHECK(state.chunk_visits == 2);
    CHECK(state.total_entities == 2);
}

// ---- Version counter semantics --------------------------------------------

TEST_CASE("Version counter bumps on insert and on get_mut, not on get_const", "[scene][storage][version]")
{
    World w;
    auto pos_id = w.register_component<Position>();

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});

    EntityLocation loc = w.storage().location(e);
    auto* arch = w.storage().graph().by_id(loc.archetype);
    REQUIRE(arch != nullptr);

    auto layout_index_of = [arch](ComponentId c) -> crd::u32
    {
        for (crd::u32 i = 0; i < arch->layout.component_count(); ++i)
        {
            if (arch->layout.components_sorted[i] == c)
            {
                return i;
            }
        }
        return crd::scene::kMaxComponents;
    };

    const crd::u32 li = layout_index_of(pos_id);
    REQUIRE(li != crd::scene::kMaxComponents);
    const crd::u64 v_after_insert = arch->chunks[loc.chunk_index].header()->version_counter[li];
    CHECK(v_after_insert >= 1U);

    // get_const must NOT bump
    (void)w.get_component<Position>(e);
    CHECK(arch->chunks[loc.chunk_index].header()->version_counter[li] == v_after_insert);

    // get_mut MUST bump
    (void)w.get_component_mut<Position>(e);
    CHECK(arch->chunks[loc.chunk_index].header()->version_counter[li] == v_after_insert + 1);
}

// ---- Storage event sink ---------------------------------------------------

namespace
{
class CountingSink final : public IStorageEventSink
{
public:
    void on_insert(EntityId, ComponentId, const void*) override { ++m_inserts; }
    void on_update(EntityId, ComponentId, const void*, const void*) override { ++m_updates; }
    void on_remove(EntityId, ComponentId, const void*) override { ++m_removes; }
    void on_entity_destroyed(EntityId) override { ++m_destroys; }

    int m_inserts = 0;
    int m_updates = 0;
    int m_removes = 0;
    int m_destroys = 0;
};
} // namespace

TEST_CASE("Default storage uses NullStorageEventSink (no-op)", "[scene][storage][sink]")
{
    World w;
    w.register_component<Position>();
    EntityId e = w.spawn();
    // Nothing to assert directly - the test verifies only that no-op sink does
    // not crash on real mutations.
    w.add_component<Position>(e, Position{});
    (void)w.get_component_mut<Position>(e);
    w.remove_component<Position>(e);
    w.destroy_immediate(e);
    SUCCEED();
}

TEST_CASE("Custom event sink receives insert / update / remove / destroyed", "[scene][storage][sink]")
{
    World w;
    w.register_component<Position>();
    CountingSink sink;
    w.set_storage_event_sink(&sink);

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3}); // -> on_insert
    (void)w.get_component_mut<Position>(e);          // -> on_update
    w.add_component<Position>(e, Position{4, 5, 6}); // upsert -> on_update
    w.remove_component<Position>(e);                 // -> on_remove
    w.destroy_immediate(e);                          // -> on_entity_destroyed

    CHECK(sink.m_inserts == 1);
    CHECK(sink.m_updates == 2);
    CHECK(sink.m_removes == 1);
    CHECK(sink.m_destroys == 1);
}

TEST_CASE("Sink receives on_entity_destroyed even when entity has components", "[scene][storage][sink]")
{
    World w;
    w.register_component<Position>();
    w.register_component<Velocity>();

    CountingSink sink;
    w.set_storage_event_sink(&sink);

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{1, 2, 3});
    w.add_component<Velocity>(e, Velocity{4, 5, 6});

    w.destroy_immediate(e);

    // on_entity_destroyed exactly once
    CHECK(sink.m_destroys == 1);
    // on_remove fires for every component still on the entity at destroy time
    CHECK(sink.m_removes == 2);
}
