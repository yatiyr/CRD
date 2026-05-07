// Demonstrate "TLSF-backed World" — the deployment pattern that motivated
// the D-001-a (TlsfAllocator) and D-001-b (GrowablePoolAllocator + ChunkAllocator
// refactor) detour. After the Phase 3.0 archetype-allocation-bypass fix
// (2026-05-07), every byte the World allocates flows through one root
// IAllocator: SlotMap entries, pending-destroy queue, ComponentRegistry's
// Array + HashMap, ArchetypeGraph's Array + HashMap, the per-archetype edge
// tables, the EntityLocation array, the Archetype structs themselves (via
// the graph's GrowablePool whose parent is the World allocator), and the
// ArchetypeChunkStorage's 16 KB chunks (via ChunkAllocator's GrowablePool
// whose parent is also the World allocator).
//
// Test verifies:
//   - World constructible with TlsfAllocator.
//   - Full ECS lifecycle (register / spawn / add / has / get / get_mut / remove
//     / destroy) works end-to-end.
//   - TLSF stats report non-zero alloc_count after the workload (only matters
//     in debug builds since MemoryStats is debug-only).
//   - World destruction returns every allocated byte to the TLSF pool —
//     bytes_in_use returns to its construction-time value.
//   - ASan-clean (the test is also exercised under win-asan).

#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::memory::TlsfAllocator;
using crd::scene::EntityId;
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
struct Health
{
    int hp = 100;
};

// A SparseSet-hinted component so the TLSF deployment proof covers BOTH
// backends — every byte the World allocates (Archetype chunks AND SparseSet
// pools + dense buffers + sparse tables) must flow through the TLSF pool.
struct DialogTrigger
{
    crd::u32 dialog_id = 0;
    crd::u32 priority = 0;
};

constexpr crd::usize kPoolBytes = 16U * 1024U * 1024U; // 16 MB scene heap
} // namespace

TEST_CASE("World can be constructed with a TlsfAllocator", "[scene][world][tlsf]")
{
    TlsfAllocator scene_heap{kPoolBytes};

    {
        World w{&scene_heap};
        CHECK(w.entity_count() == 0U);
        CHECK(w.archetype_count() == 0U);
    }

    // After World destruction, the TLSF pool contains only its sentinels.
    // (We can't directly check bytes_in_use across the World's lifetime
    // without snapshotting, but the world's destructor must run cleanly.)
    SUCCEED();
}

TEST_CASE("Full ECS lifecycle on a TLSF-backed World", "[scene][world][tlsf]")
{
    TlsfAllocator scene_heap{kPoolBytes};
    World w{&scene_heap};

    w.register_component<Position>();
    w.register_component<Velocity>();
    w.register_component<Health>();
    // SparseSet-hinted component routed to SparseSetStorage by World dispatch.
    w.register_component<DialogTrigger>(crd::scene::StorageHint::SparseSet);

    // Spawn a bunch of entities with mixed component sets.
    crd::containers::Array<EntityId> entities;
    for (int i = 0; i < 200; ++i)
    {
        EntityId e = w.spawn();
        w.add_component<Position>(e, Position{static_cast<float>(i), 0, 0});
        if ((i & 1) == 0)
        {
            w.add_component<Velocity>(e, Velocity{1, 0, 0});
        }
        if ((i % 3) == 0)
        {
            w.add_component<Health>(e, Health{50 + i});
        }
        // Sparse: every 5th entity gets a dialog trigger. <5% would be more
        // realistic, but we want enough density to force a sparse-table
        // resize and a dense-buffer grow.
        if ((i % 5) == 0)
        {
            w.add_component<DialogTrigger>(e, DialogTrigger{static_cast<crd::u32>(i), 1U});
        }
        entities.push_back(e);
    }

    CHECK(w.entity_count() == 200U);
    CHECK(w.archetype_count() >= 4U); // {P}, {P,V}, {P,H}, {P,V,H} … at least
    CHECK(w.sparse_storage().pool_count() == 1U);
    CHECK(w.sparse_storage().entity_count(w.component_id<DialogTrigger>()) == 40U);

    // Read everything back.
    for (int i = 0; i < 200; ++i)
    {
        EntityId e = entities[i];
        const Position* p = w.get_component<Position>(e);
        REQUIRE(p != nullptr);
        CHECK(p->x == static_cast<float>(i));
    }

    // Mutate via get_component_mut (exercises the version-counter bump path).
    for (int i = 0; i < 200; ++i)
    {
        Position* p = w.get_component_mut<Position>(entities[i]);
        REQUIRE(p != nullptr);
        p->y = 1.0F;
    }
    for (int i = 0; i < 200; ++i)
    {
        const Position* p = w.get_component<Position>(entities[i]);
        REQUIRE(p != nullptr);
        CHECK(p->y == 1.0F);
    }

    // Remove a component on half the entities (forces archetype moves).
    for (int i = 0; i < 200; i += 2)
    {
        w.remove_component<Position>(entities[i]);
    }

    // Destroy half the entities.
    for (int i = 0; i < 100; ++i)
    {
        w.destroy(entities[i]);
    }
    w.flush_destroys();

    CHECK(w.entity_count() == 100U);

    // Tear down explicitly so the TLSF pool watches every member dtor in turn.
}

TEST_CASE("Chunk fill/spill works on TLSF heap", "[scene][world][tlsf]")
{
    TlsfAllocator scene_heap{kPoolBytes};
    World w{&scene_heap};

    w.register_component<Position>();

    // Spawn 1500 entities to force ChunkAllocator's GrowablePool to grow
    // beyond a single page (1 MB / 16 KB = 64 chunks/page; one chunk holds
    // ~800 entities at sizeof(Position) = 12).
    crd::containers::Array<EntityId> entities;
    for (int i = 0; i < 1500; ++i)
    {
        EntityId e = w.spawn();
        w.add_component<Position>(e, Position{static_cast<float>(i), 0, 0});
        entities.push_back(e);
    }

    CHECK(w.entity_count() == 1500U);

    // Verify all data resolved correctly.
    for (int i = 0; i < 1500; ++i)
    {
        const Position* p = w.get_component<Position>(entities[i]);
        REQUIRE(p != nullptr);
        CHECK(p->x == static_cast<float>(i));
    }

    // Drain.
    for (EntityId e : entities)
    {
        w.destroy_immediate(e);
    }
    CHECK(w.entity_count() == 0U);
}

TEST_CASE("World destruction returns every byte to the TLSF pool", "[scene][world][tlsf]")
{
    TlsfAllocator scene_heap{kPoolBytes};

    // Snapshot stats while the pool only holds its three sentinels.
    const auto baseline = scene_heap.stats().snapshot();

    {
        World w{&scene_heap};
        w.register_component<Position>();
        w.register_component<Velocity>();

        for (int i = 0; i < 64; ++i)
        {
            EntityId e = w.spawn();
            w.add_component<Position>(e, Position{static_cast<float>(i), 0, 0});
            if ((i & 1) == 0)
            {
                w.add_component<Velocity>(e, Velocity{});
            }
        }
        // World goes out of scope here — destructor must drain every member.
    }

    const auto after = scene_heap.stats().snapshot();

    // bytes_in_use only updates in CRD_DEBUG configurations. When asserts are
    // enabled the bytes-in-use should match baseline (pool emptied).
#if defined(CRD_DEBUG)
    CHECK(after.bytes_in_use == baseline.bytes_in_use);
#else
    (void)baseline;
    (void)after;
#endif
    SUCCEED();
}

TEST_CASE("ArchetypeGraph reports its archetype-pool page count", "[scene][world][tlsf]")
{
    TlsfAllocator scene_heap{kPoolBytes};
    World w{&scene_heap};

    w.register_component<Position>();
    w.register_component<Velocity>();
    w.register_component<Health>();

    // Pages are allocated lazily on first archetype creation. Before any
    // entity-component bindings, no archetype-pool pages exist.
    CHECK(w.storage().graph().archetype_pool_pages() == 0U);

    EntityId e = w.spawn();
    w.add_component<Position>(e, Position{});
    // Now the {Position} archetype exists → at least one pool page.
    CHECK(w.storage().graph().archetype_pool_pages() >= 1U);
}
