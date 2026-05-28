// Scene-storage stress matrix (detour D-002 v6).
//
// Exercises ONLY the concurrent patterns the scene declares legal — see
// docs/systems/scene-concurrency.md:
//   - parallel read-only iteration: one job per chunk, const component access;
//   - parallel disjoint in-place writes: one job per chunk, each job writes its
//     own entities' component values via get_component_mut (so each chunk's
//     version counter is bumped by exactly one job — the rule until a future
//     par_each defines parallel get_mut);
//   - concurrent reads of frozen scene state (SlotMap::is_alive,
//     ComponentRegistry, get_component) — legal because no structural mutation
//     is in flight.
//
// It deliberately does NOT do concurrent structural mutation (spawn/destroy,
// add/remove component, register from a worker) — that's illegal use, and a
// TSan/ASan flag on it would be a false positive, not a bug.
//
// Pattern note: World::for_each_chunk runs SINGLE-THREADED and the visitor's
// `entities` pointer may be a transient scratch buffer (mixed/sparse path), so
// each chunk's entity IDs are copied out into an owned per-chunk Array during
// the single-threaded walk; the parallel phase then runs one job per chunk over
// those owned lists. main_stress.cpp has already called jobs::init().

#include "stress_harness.hpp"

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/scene/query.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using crd::u32;
using crd::u64;
using crd::usize;
using crd::scene::ChunkView;
using crd::scene::EntityId;
using crd::scene::StorageHint;
using crd::scene::World;

constexpr u64 mix64(u64 x) noexcept
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x;
}

// Two component types — one Archetype-backed, one SparseSet-backed — both a
// single u64 payload so a per-entity expected value is trivial to derive.
struct ArchComp
{
    u64 v = 0;
};
struct SparseComp
{
    u64 v = 0;
};

// EntityId payload for value-derivation: index() + generation() identify it.
u64 entity_key(EntityId e) noexcept
{
    return (static_cast<u64>(e.generation()) << 32) ^ static_cast<u64>(e.index());
}
u64 expected_value(EntityId e, u32 round) noexcept
{
    return mix64(entity_key(e) ^ (static_cast<u64>(round) << 48) ^ 0x5CE2EULL);
}

// --- per-chunk entity-ID collection (runs during the single-threaded walk) ---
struct CollectCtx
{
    crd::containers::Array<crd::containers::Array<EntityId>>* chunks = nullptr;
    crd::memory::IAllocator* alloc = nullptr;
};
void collect_visitor(const ChunkView& cv, void* ud) noexcept
{
    auto* ctx = static_cast<CollectCtx*>(ud);
    crd::containers::Array<EntityId> ids(ctx->alloc);
    ids.reserve(cv.entity_count);
    for (u32 i = 0; i < cv.entity_count; ++i)
    {
        ids.push_back(cv.entities[i]);
    }
    ctx->chunks->push_back(std::move(ids));
}

// Build a world with `n` entities, each carrying ArchComp (and SparseComp if asked).
void build_world(World& w, crd::containers::Array<EntityId>& out_entities, u32 n, bool with_sparse)
{
    w.register_component<ArchComp>();
    if (with_sparse)
    {
        w.register_component<SparseComp>(StorageHint::SparseSet);
    }
    out_entities.reserve(n);
    for (u32 i = 0; i < n; ++i)
    {
        const EntityId e = w.spawn();
        w.add_component<ArchComp>(e, ArchComp{expected_value(e, 0)});
        if (with_sparse)
        {
            w.add_component<SparseComp>(e, SparseComp{expected_value(e, 0)});
        }
        out_entities.push_back(e);
    }
}

// Collect one owned Array<EntityId> per chunk that the query for `T`-presence yields.
template <typename T>
void collect_chunks(World& w, crd::memory::IAllocator* alloc,
                    crd::containers::Array<crd::containers::Array<EntityId>>& chunks)
{
    chunks.clear();
    CollectCtx ctx{&chunks, alloc};
    w.query<T>().for_each_chunk(&collect_visitor, &ctx);
}
} // namespace

TEST_CASE("scene stress -- parallel chunk read (archetype)", "[stress][scene]")
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "scene-stress-read"};
    World w(&alloc);
    crd::containers::Array<EntityId> entities(&alloc);
    constexpr u32 k_n = 6007U; // odd; spans many archetype chunks
    build_world(w, entities, k_n, /*with_sparse=*/false);

    crd::containers::Array<crd::containers::Array<EntityId>> chunks(&alloc);
    collect_chunks<ArchComp>(w, &alloc, chunks);
    REQUIRE(chunks.size() > 1U); // actually exercises the multi-chunk parallel path
    const u32 num_chunks = static_cast<u32>(chunks.size());

    // Per-chunk partial XOR via parallel_for, folded serially — compared against a serial reference.
    u64 serial_xor = 0;
    for (const EntityId e : entities)
    {
        serial_xor ^= w.get_component<ArchComp>(e)->v;
    }

    crd::containers::Array<u64> partials(&alloc);
    partials.resize(num_chunks, 0ULL);
    for (u32 round = 0; round < 4U; ++round)
    {
        crd::jobs::Counter* const c = crd::jobs::parallel_for(
            num_chunks, num_chunks,
            [&w, &chunks, &partials](u32 begin, u32 end)
            {
                for (u32 ci = begin; ci < end; ++ci)
                {
                    u64 acc = 0;
                    for (const EntityId e : chunks[ci])
                    {
                        acc ^= w.get_component<ArchComp>(e)->v; // const access — no version bump, race-free
                    }
                    partials[ci] = acc;
                }
            });
        crd::jobs::wait(c);

        u64 par_xor = 0;
        for (u32 ci = 0; ci < num_chunks; ++ci)
        {
            par_xor ^= partials[ci];
        }
        CHECK(par_xor == serial_xor);
        // The world must be untouched by the readers.
        for (const EntityId e : entities)
        {
            CHECK(w.get_component<ArchComp>(e)->v == expected_value(e, 0));
        }
    }
    crd::jobs::frame_reset();
}

TEST_CASE("scene stress -- parallel disjoint chunk write (archetype + sparse)", "[stress][scene]")
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "scene-stress-write"};
    World w(&alloc);
    crd::containers::Array<EntityId> entities(&alloc);
    constexpr u32 k_n = 6007U;
    build_world(w, entities, k_n, /*with_sparse=*/true);

    crd::containers::Array<crd::containers::Array<EntityId>> arch_chunks(&alloc);
    crd::containers::Array<crd::containers::Array<EntityId>> sparse_chunks(&alloc);
    collect_chunks<ArchComp>(w, &alloc, arch_chunks);
    collect_chunks<SparseComp>(w, &alloc, sparse_chunks);
    REQUIRE(arch_chunks.size() > 1U);
    REQUIRE(sparse_chunks.size() >= 1U);

    for (u32 round = 1U; round <= 4U; ++round)
    {
        // Archetype: one job per chunk, each writes its entities' ArchComp.
        {
            const u32 nc = static_cast<u32>(arch_chunks.size());
            crd::jobs::Counter* const c = crd::jobs::parallel_for(nc, nc,
                                                                  [&w, &arch_chunks, round](u32 begin, u32 end)
                                                                  {
                                                                      for (u32 ci = begin; ci < end; ++ci)
                                                                      {
                                                                          for (const EntityId e : arch_chunks[ci])
                                                                          {
                                                                              w.get_component_mut<ArchComp>(e)->v =
                                                                                  expected_value(e, round);
                                                                          }
                                                                      }
                                                                  });
            crd::jobs::wait(c);
        }
        // SparseSet: same shape on the other backend.
        {
            const u32 nc = static_cast<u32>(sparse_chunks.size());
            crd::jobs::Counter* const c = crd::jobs::parallel_for(nc, nc,
                                                                  [&w, &sparse_chunks, round](u32 begin, u32 end)
                                                                  {
                                                                      for (u32 ci = begin; ci < end; ++ci)
                                                                      {
                                                                          for (const EntityId e : sparse_chunks[ci])
                                                                          {
                                                                              w.get_component_mut<SparseComp>(e)->v =
                                                                                  expected_value(e, round);
                                                                          }
                                                                      }
                                                                  });
            crd::jobs::wait(c);
        }

        for (const EntityId e : entities)
        {
            CHECK(w.get_component<ArchComp>(e)->v == expected_value(e, round));
            CHECK(w.get_component<SparseComp>(e)->v == expected_value(e, round));
        }
    }
    crd::jobs::frame_reset();
}

TEST_CASE("scene stress -- concurrent reads of frozen scene state", "[stress][scene]")
{
    crd::memory::GrowableTlsfAllocator alloc{256ULL << 20, nullptr, "scene-stress-frozen"};
    World w(&alloc);
    crd::containers::Array<EntityId> entities(&alloc);
    constexpr u32 k_n = 4096U;
    build_world(w, entities, k_n, /*with_sparse=*/true);
    const crd::u16 reg_count = w.registered_component_count();

    crd::stress::FailSink sink;
    const auto work = [&w, &entities, reg_count, &sink](u32 worker, u64 iters, crd::stress::Rng& rng)
    {
        u64 acc = 0;
        for (u64 it = 0; it < iters; ++it)
        {
            const EntityId e = entities[rng.next_u32(static_cast<u32>(entities.size()))];
            CRD_STRESS_FAIL_IF(sink, worker, it, w.is_alive(e), "SlotMap::is_alive false for a live entity");
            const ArchComp* a = w.get_component<ArchComp>(e);
            const SparseComp* s = w.get_component<SparseComp>(e);
            CRD_STRESS_FAIL_IF(sink, worker, it, a != nullptr && s != nullptr, "get_component returned null");
            if (a != nullptr)
            {
                acc ^= a->v;
            }
            if (s != nullptr)
            {
                acc ^= s->v;
            }
            CRD_STRESS_FAIL_IF(sink, worker, it, w.registered_component_count() == reg_count,
                               "ComponentRegistry count changed under concurrent reads");
        }
        // Touch `acc` so the loop isn't optimised away; value doesn't matter.
        if (acc == 0xDEADBEEFULL)
        {
            sink.fail("(impossible) acc sentinel", 0U, worker);
        }
    };
    const auto oracle = [&w, &entities](u32 /*round*/)
    {
        // Frozen state intact.
        for (const EntityId e : entities)
        {
            CHECK(w.is_alive(e));
            CHECK(w.get_component<ArchComp>(e)->v == expected_value(e, 0));
        }
    };

    crd::stress::run(crd::stress::bounded(crd::stress::RunMode::Fibers), work, oracle);
    CRD_STRESS_ORACLE_OK(sink);
}
