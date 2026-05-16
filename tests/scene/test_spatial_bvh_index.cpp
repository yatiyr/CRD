// Phase 3.1.7 v5-index-bringup — SpatialBVHIndex tests.
//
// Covers the promotion of the no-op shell to a real LooseOctree-backed
// component index (ADR-0053 §6 + ADR-0076 §20). The day-one promise from
// Phase 3.0 v1i (registering for the trait silently no-ops at runtime) is
// preserved: an unconfigured index dispatches storage events to no-op
// handlers. Once configured, queries return real entities.

#include <crd/containers/array.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <random>

using crd::f32;
using crd::u32;
using crd::usize;
using crd::geometry::primitives::AABB3;
using crd::geometry::primitives::Ray3;
using crd::geometry::spatial::OctreeBuildOptions;
using crd::math::Vec3f;
using crd::scene::EntityId;
using crd::scene::IAabbExtractor;
using crd::scene::SpatialBVH;
using crd::scene::SpatialBVHIndex;
using crd::scene::StorageHint;
using crd::scene::World;

// Binary-wide job listener — concurrent fiber test in this file needs it.
// Same pattern as `BvhJobsListener` + v5d's `GeometrySpatialJobsListener`.
namespace
{
struct SceneJobsListener final : Catch::EventListenerBase
{
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(Catch::TestRunInfo const&) override
    {
        crd::jobs::init(crd::jobs::Config{.num_threads = 4, .frame_alloc_bytes = 16U << 20U});
    }
    void testCaseEnded(Catch::TestCaseStats const&) override { crd::jobs::frame_reset(); }
    void testRunEnded(Catch::TestRunStats const&) override { crd::jobs::shutdown(); }
};
} // namespace
CATCH_REGISTER_LISTENER(SceneJobsListener)

namespace
{
// Test bounds component: each entity carries its own world AABB. The
// extractor reads this directly. Simplest possible AABB-bearing component
// — real consumers (renderer Renderable, eylem Collider) would compute
// world AABB from Transform + local bounds.
struct BoundsComponent
{
    AABB3<f32> world_aabb{};
};

// Extractor that reads BoundsComponent::world_aabb directly from the
// IStorageEventSink-supplied `data` pointer (which is the live component
// bytes per the v1i fan-out contract — see spatial_bvh_index.hpp).
class BoundsExtractor final : public IAabbExtractor
{
public:
    AABB3<f32> extract(EntityId /*e*/, crd::scene::ComponentId /*c*/, const void* data) const override
    {
        CRD_ASSERT(data != nullptr);
        const auto* bounds = static_cast<const BoundsComponent*>(data);
        return bounds->world_aabb;
    }
};

struct AllocFixture { crd::memory::TlsfAllocator alloc{32U << 20}; };

OctreeBuildOptions<f32> default_octree_opts()
{
    return OctreeBuildOptions<f32>{
        AABB3<f32>{Vec3f{-100, -100, -100}, Vec3f{100, 100, 100}}, 2.0F, 8U, 8U};
}

AABB3<f32> aabb_around(const Vec3f& c, f32 h)
{
    return AABB3<f32>{Vec3f{c.x - h, c.y - h, c.z - h}, Vec3f{c.x + h, c.y + h, c.z + h}};
}
} // namespace

TEST_CASE("SpatialBVHIndex: auto-registers as a no-op when SpatialBVH{} trait set",
          "[scene][index][spatial-bvh][bringup]")
{
    AllocFixture f{};
    World world{&f.alloc};
    world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});

    auto* idx = world.find_index<SpatialBVHIndex>();
    REQUIRE(idx != nullptr);
    REQUIRE_FALSE(idx->is_configured());  // day-one promise: silently no-op
    REQUIRE(idx->tracked_entity_count() == 0U);

    // Insert + remove an entity — hooks fire but do nothing.
    auto e = world.spawn();
    world.add_component(e, BoundsComponent{aabb_around(Vec3f{1, 2, 3}, 0.5F)});
    REQUIRE(idx->tracked_entity_count() == 0U);  // still no-op
    world.destroy_immediate(e);
}

TEST_CASE("SpatialBVHIndex: configure wires up real LooseOctree-backed indexing",
          "[scene][index][spatial-bvh][bringup]")
{
    AllocFixture f{};
    World world{&f.alloc};
    world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});

    auto* idx = world.find_index<SpatialBVHIndex>();
    REQUIRE(idx != nullptr);

    BoundsExtractor extractor{};
    idx->configure(&extractor, default_octree_opts());
    REQUIRE(idx->is_configured());

    // Now inserts hook through to the octree.
    auto e1 = world.spawn();
    world.add_component(e1, BoundsComponent{aabb_around(Vec3f{5, 5, 5}, 0.4F)});
    REQUIRE(idx->tracked_entity_count() == 1U);

    auto e2 = world.spawn();
    world.add_component(e2, BoundsComponent{aabb_around(Vec3f{-5, -5, -5}, 0.4F)});
    REQUIRE(idx->tracked_entity_count() == 2U);
}

TEST_CASE("SpatialBVHIndex overlap query returns matching entities",
          "[scene][index][spatial-bvh][overlap]")
{
    AllocFixture f{};
    World world{&f.alloc};
    world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});

    auto* idx = world.find_index<SpatialBVHIndex>();
    BoundsExtractor extractor{};
    idx->configure(&extractor, default_octree_opts());

    // Three entities at distinct positions.
    auto e_near  = world.spawn();
    world.add_component(e_near, BoundsComponent{aabb_around(Vec3f{1, 0, 0}, 0.3F)});
    auto e_far   = world.spawn();
    world.add_component(e_far, BoundsComponent{aabb_around(Vec3f{50, 0, 0}, 0.3F)});
    auto e_other = world.spawn();
    world.add_component(e_other, BoundsComponent{aabb_around(Vec3f{1.5F, 0, 0}, 0.3F)});

    // Query the near region: should find e_near + e_other but not e_far.
    crd::containers::Array<EntityId> hits{&f.alloc};
    idx->overlap(AABB3<f32>{Vec3f{0, -1, -1}, Vec3f{3, 1, 1}}, hits);
    REQUIRE(hits.size() == 2U);
    // Order isn't guaranteed by the API; check membership.
    bool saw_near = false, saw_other = false;
    for (auto id : hits) { if (id == e_near) saw_near = true; if (id == e_other) saw_other = true; }
    REQUIRE(saw_near);
    REQUIRE(saw_other);

    // Query far region: only e_far.
    hits.clear();
    idx->overlap(AABB3<f32>{Vec3f{49, -1, -1}, Vec3f{51, 1, 1}}, hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == e_far);
}

TEST_CASE("SpatialBVHIndex update reflects component changes via on_update",
          "[scene][index][spatial-bvh][update]")
{
    AllocFixture f{};
    World world{&f.alloc};
    world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});
    auto* idx = world.find_index<SpatialBVHIndex>();
    BoundsExtractor extractor{};
    idx->configure(&extractor, default_octree_opts());

    auto e = world.spawn();
    world.add_component(e, BoundsComponent{aabb_around(Vec3f{1, 1, 1}, 0.3F)});

    // Find at original position.
    crd::containers::Array<EntityId> hits{&f.alloc};
    idx->overlap(aabb_around(Vec3f{1, 1, 1}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);

    // Mutate bounds via add_component (UPSERT) — this fires per-entity
    // on_update through the storage event sink. `get_component_mut` only
    // bumps chunk-version (ChangeDetect's hint path) — it does NOT fire
    // per-entity on_update, so the index wouldn't see the change. UPSERT
    // is the right pattern for spatial-index consumers; document this in
    // the index header.
    world.add_component(e, BoundsComponent{aabb_around(Vec3f{40, 40, 40}, 0.3F)});

    hits.clear();
    idx->overlap(aabb_around(Vec3f{1, 1, 1}, 1.0F), hits);
    REQUIRE(hits.size() == 0U);  // moved away
    hits.clear();
    idx->overlap(aabb_around(Vec3f{40, 40, 40}, 1.0F), hits);
    REQUIRE(hits.size() == 1U);
    REQUIRE(hits[0] == e);
}

TEST_CASE("SpatialBVHIndex remove via destroy_entity drops the entry",
          "[scene][index][spatial-bvh][remove]")
{
    AllocFixture f{};
    World world{&f.alloc};
    world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});
    auto* idx = world.find_index<SpatialBVHIndex>();
    BoundsExtractor extractor{};
    idx->configure(&extractor, default_octree_opts());

    auto e = world.spawn();
    world.add_component(e, BoundsComponent{aabb_around(Vec3f{1, 1, 1}, 0.3F)});
    REQUIRE(idx->tracked_entity_count() == 1U);

    world.destroy_immediate(e);
    REQUIRE(idx->tracked_entity_count() == 0U);

    crd::containers::Array<EntityId> hits{&f.alloc};
    idx->overlap(aabb_around(Vec3f{1, 1, 1}, 1.0F), hits);
    REQUIRE(hits.size() == 0U);
}

TEST_CASE("SpatialBVHIndex raycast picks nearest entity",
          "[scene][index][spatial-bvh][raycast]")
{
    AllocFixture f{};
    World world{&f.alloc};
    world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});
    auto* idx = world.find_index<SpatialBVHIndex>();
    BoundsExtractor extractor{};
    idx->configure(&extractor, default_octree_opts());

    auto e_far  = world.spawn();
    world.add_component(e_far, BoundsComponent{aabb_around(Vec3f{15, 0, 0}, 0.5F)});
    auto e_near = world.spawn();
    world.add_component(e_near, BoundsComponent{aabb_around(Vec3f{5, 0, 0}, 0.5F)});
    auto e_mid  = world.spawn();
    world.add_component(e_mid, BoundsComponent{aabb_around(Vec3f{10, 0, 0}, 0.5F)});

    auto hit = idx->raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE(hit.has_value());
    REQUIRE(hit->payload == e_near);
    REQUIRE(hit->t >= 4.4F);
    REQUIRE(hit->t <= 4.6F);
}

TEST_CASE("SpatialBVHIndex raycast unconfigured returns nullopt",
          "[scene][index][spatial-bvh][raycast]")
{
    AllocFixture f{};
    World world{&f.alloc};
    world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});
    auto* idx = world.find_index<SpatialBVHIndex>();
    REQUIRE_FALSE(idx->is_configured());
    auto hit = idx->raycast(Ray3<f32>{Vec3f{0, 0, 0}, Vec3f{1, 0, 0}});
    REQUIRE_FALSE(hit.has_value());
}

// =============================================================================
// Concurrent overlap queries via crd-jobs fiber pool — proves naturally-const-safe
// =============================================================================
//
// SpatialBVHIndex queries are const-safe by construction: LooseOctree is
// const-safe (object lives in exactly one cell — Ulrich's invariant) and
// the entity-handle table is const-iterated during queries. This test
// validates the claim empirically: many fibers run concurrent overlap
// against the SHARED index, each with its own output Array. ASan instrumentation
// catches any silent race the impl might have despite the construction-level
// claim.

namespace
{
struct SceneConcurrencyCorpus
{
    static constexpr u32 k_queries = 16U;
    static constexpr u32 k_iters_per_query = 25U;

    crd::memory::TlsfAllocator alloc{32U << 20};
    World                      world{&alloc};
    std::unique_ptr<BoundsExtractor> extractor{};
    SpatialBVHIndex*           idx{nullptr};
    AABB3<f32>                 queries[k_queries]{};
    crd::containers::Array<EntityId> ref[k_queries] = {
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc},
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc},
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc},
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc},
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc},
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc},
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc},
        crd::containers::Array<EntityId>{&alloc}, crd::containers::Array<EntityId>{&alloc}};
    std::atomic<crd::u32> mismatches{0};
};
} // namespace

TEST_CASE("SpatialBVHIndex concurrent overlap queries via crd-jobs (proves naturally-const-safe)",
          "[scene][index][spatial-bvh][concurrent][jobs]")
{
    auto corpus = std::make_unique<SceneConcurrencyCorpus>();
    corpus->world.register_component<BoundsComponent>(StorageHint::Archetype, SpatialBVH{});
    corpus->idx = corpus->world.find_index<SpatialBVHIndex>();
    corpus->extractor = std::make_unique<BoundsExtractor>();
    corpus->idx->configure(corpus->extractor.get(), default_octree_opts());

    // Build a scene with 300 entities at random positions.
    std::mt19937 rng(101U);
    std::uniform_real_distribution<f32> uc(-80.0F, 80.0F);
    std::uniform_real_distribution<f32> uh(0.3F, 1.0F);
    for (u32 i = 0; i < 300U; ++i)
    {
        auto e = corpus->world.spawn();
        corpus->world.add_component(e, BoundsComponent{aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uh(rng))});
    }

    std::uniform_real_distribution<f32> uqh(2.0F, 8.0F);
    for (u32 q = 0; q < SceneConcurrencyCorpus::k_queries; ++q)
    {
        corpus->queries[q] = aabb_around(Vec3f{uc(rng), uc(rng), uc(rng)}, uqh(rng));
        corpus->idx->overlap(corpus->queries[q], corpus->ref[q]);
        // Sort for stable comparison (EntityId by raw bits).
        std::sort(corpus->ref[q].data(), corpus->ref[q].data() + corpus->ref[q].size(),
                    [](EntityId a, EntityId b) { return a.raw < b.raw; });
    }

    constexpr u32 total_tasks = SceneConcurrencyCorpus::k_queries * SceneConcurrencyCorpus::k_iters_per_query;
    auto* corpus_ptr = corpus.get();
    crd::jobs::Counter* counter = crd::jobs::parallel_for(
        total_tasks, /*num_jobs=*/16U,
        [corpus_ptr](crd::u32 begin, crd::u32 end) noexcept {
            for (crd::u32 task_idx = begin; task_idx < end; ++task_idx)
            {
                const u32 q = task_idx % SceneConcurrencyCorpus::k_queries;
                crd::memory::TlsfAllocator local_alloc{1U << 17};
                crd::containers::Array<EntityId> got(&local_alloc);
                corpus_ptr->idx->overlap(corpus_ptr->queries[q], got);
                std::sort(got.data(), got.data() + got.size(),
                            [](EntityId a, EntityId b) { return a.raw < b.raw; });
                bool ok = (got.size() == corpus_ptr->ref[q].size());
                if (ok)
                {
                    for (usize i = 0; i < got.size(); ++i)
                    {
                        if (got[i].raw != corpus_ptr->ref[q][i].raw) { ok = false; break; }
                    }
                }
                if (!ok) { corpus_ptr->mismatches.fetch_add(1U, std::memory_order_relaxed); }
            }
        });
    crd::jobs::wait(counter);
    REQUIRE(corpus->mismatches.load() == 0U);
}
