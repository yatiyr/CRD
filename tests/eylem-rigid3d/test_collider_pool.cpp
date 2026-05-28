// Phase 3.1 v1b-b -- ColliderPool unit tests.
//
// Coverage:
//   1. Empty state.
//   2. Insert each supported kind (Sphere/Box/Capsule) → contains/read/body_of.
//   3. ColliderId encoding round-trips kind + per-kind index.
//   4. Generation bump on remove + re-insert.
//   5. Free-list reuse: lowest-index slot reused per-kind.
//   6. Capacity exhaustion per-kind.
//   7. ConvexHull/Plane explicitly return null (deferred to v1d).
//   8. Null-body insert returns null.
//   9. Deterministic handle sequence.
//  10. Per-kind size + total size accounting.

#include <crd/eylem/collider.hpp>
#include <crd/eylem/types.hpp>
#include <crd/eylem_rigid3d/collider_pool.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using crd::eylem::BodyId;
using crd::eylem::Collider;
using crd::eylem::ColliderBox;
using crd::eylem::ColliderCapsule;
using crd::eylem::ColliderConvexHull;
using crd::eylem::ColliderId;
using crd::eylem::ColliderPlane;
using crd::eylem::ColliderShape;
using crd::eylem::ColliderSphere;
using crd::eylem_rigid3d::ColliderPool;
using crd::eylem_rigid3d::decode_collider_kind;
using crd::eylem_rigid3d::decode_collider_per_kind_idx;
using crd::eylem_rigid3d::encode_collider_index;

namespace
{
Collider make_sphere(crd::f32 r, crd::math::Vec3f lpos = {0.0F, 0.0F, 0.0F})
{
    Collider c{};
    c.shape          = ColliderShape::Sphere;
    c.local_position = lpos;
    c.local_rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    c.sphere         = ColliderSphere{r};
    return c;
}

Collider make_box(crd::math::Vec3f h, crd::math::Vec3f lpos = {0.0F, 0.0F, 0.0F})
{
    Collider c{};
    c.shape          = ColliderShape::Box;
    c.local_position = lpos;
    c.local_rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    c.box            = ColliderBox{h};
    return c;
}

Collider make_capsule(crd::f32 r, crd::f32 hh, crd::math::Vec3f lpos = {0.0F, 0.0F, 0.0F})
{
    Collider c{};
    c.shape          = ColliderShape::Capsule;
    c.local_position = lpos;
    c.local_rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    c.capsule        = ColliderCapsule{r, hh};
    return c;
}
} // namespace

TEST_CASE("ColliderPool: empty state", "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);

    REQUIRE(pool.size() == 0U);
    REQUIRE(pool.size_of(ColliderShape::Sphere)  == 0U);
    REQUIRE(pool.size_of(ColliderShape::Box)     == 0U);
    REQUIRE(pool.size_of(ColliderShape::Capsule) == 0U);
    REQUIRE(pool.capacity_per_kind() == 64U);
    REQUIRE_FALSE(pool.contains(ColliderId::null()));
}

TEST_CASE("ColliderPool: ColliderId encoding round-trips kind + per-kind idx",
          "[eylem-rigid3d][colliderpool]")
{
    REQUIRE(decode_collider_kind(encode_collider_index(ColliderShape::Sphere,  1U)) == ColliderShape::Sphere);
    REQUIRE(decode_collider_kind(encode_collider_index(ColliderShape::Box,    42U)) == ColliderShape::Box);
    REQUIRE(decode_collider_kind(encode_collider_index(ColliderShape::Capsule, 7U)) == ColliderShape::Capsule);

    REQUIRE(decode_collider_per_kind_idx(encode_collider_index(ColliderShape::Sphere,  1U)) == 1U);
    REQUIRE(decode_collider_per_kind_idx(encode_collider_index(ColliderShape::Box,    42U)) == 42U);
    REQUIRE(decode_collider_per_kind_idx(encode_collider_index(ColliderShape::Capsule, 7U)) == 7U);

    // Largest valid per-kind index packs cleanly. Kind field = 4 bits,
    // per-kind index = 20 bits → 1,048,575 colliders/kind ceiling.
    constexpr crd::u32 kMax = (1U << 20) - 1U;
    REQUIRE(decode_collider_per_kind_idx(encode_collider_index(ColliderShape::Box, kMax)) == kMax);
    REQUIRE(decode_collider_kind        (encode_collider_index(ColliderShape::Box, kMax)) == ColliderShape::Box);

    // Highest-numbered shape kind (Sdf = 7) round-trips. Even though Sdf
    // storage doesn't ship until Phase 3.1.5, the encoding must already
    // handle it for handle stability across the API freeze.
    REQUIRE(decode_collider_kind(encode_collider_index(ColliderShape::Sdf, 1U)) == ColliderShape::Sdf);
}

TEST_CASE("ColliderPool: insert + read sphere", "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);

    BodyId body  = BodyId::make(7U, 3U);
    Collider src = make_sphere(2.5F, {1.0F, 2.0F, 3.0F});
    src.local_rotation = {0.1F, 0.2F, 0.3F, 0.927F};

    ColliderId id = pool.insert(body, src);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(pool.contains(id));
    REQUIRE(pool.size() == 1U);
    REQUIRE(pool.size_of(ColliderShape::Sphere) == 1U);
    REQUIRE(decode_collider_kind(id.index()) == ColliderShape::Sphere);

    REQUIRE(pool.body_of(id) == body);

    Collider got = pool.read(id);
    REQUIRE(got.shape == ColliderShape::Sphere);
    REQUIRE(got.sphere.radius == 2.5F);
    REQUIRE(got.local_position.x == 1.0F);
    REQUIRE(got.local_position.y == 2.0F);
    REQUIRE(got.local_position.z == 3.0F);
    REQUIRE(got.local_rotation.x == 0.1F);
    REQUIRE(got.local_rotation.w == 0.927F);
}

TEST_CASE("ColliderPool: insert + read box", "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);

    BodyId body  = BodyId::make(11U, 2U);
    Collider src = make_box({0.5F, 1.5F, 2.5F}, {-1.0F, -2.0F, -3.0F});

    ColliderId id = pool.insert(body, src);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(decode_collider_kind(id.index()) == ColliderShape::Box);
    REQUIRE(pool.body_of(id) == body);

    Collider got = pool.read(id);
    REQUIRE(got.shape == ColliderShape::Box);
    REQUIRE(got.box.half_extents.x == 0.5F);
    REQUIRE(got.box.half_extents.y == 1.5F);
    REQUIRE(got.box.half_extents.z == 2.5F);
    REQUIRE(got.local_position.x == -1.0F);
}

TEST_CASE("ColliderPool: insert + read capsule", "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);

    BodyId body  = BodyId::make(13U, 5U);
    Collider src = make_capsule(0.4F, 1.2F);

    ColliderId id = pool.insert(body, src);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(decode_collider_kind(id.index()) == ColliderShape::Capsule);

    Collider got = pool.read(id);
    REQUIRE(got.shape == ColliderShape::Capsule);
    REQUIRE(got.capsule.radius      == 0.4F);
    REQUIRE(got.capsule.half_height == 1.2F);
}

TEST_CASE("ColliderPool: per-kind pools are independent",
          "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);

    BodyId b = BodyId::make(1U, 1U);
    ColliderId s = pool.insert(b, make_sphere(1.0F));
    ColliderId x = pool.insert(b, make_box({0.5F, 0.5F, 0.5F}));
    ColliderId c = pool.insert(b, make_capsule(0.3F, 0.6F));

    REQUIRE_FALSE(s.is_null());
    REQUIRE_FALSE(x.is_null());
    REQUIRE_FALSE(c.is_null());

    REQUIRE(pool.size() == 3U);
    REQUIRE(pool.size_of(ColliderShape::Sphere)  == 1U);
    REQUIRE(pool.size_of(ColliderShape::Box)     == 1U);
    REQUIRE(pool.size_of(ColliderShape::Capsule) == 1U);

    // Each kind starts at per-kind index 1 (slot 0 reserved).
    REQUIRE(decode_collider_per_kind_idx(s.index()) == 1U);
    REQUIRE(decode_collider_per_kind_idx(x.index()) == 1U);
    REQUIRE(decode_collider_per_kind_idx(c.index()) == 1U);
}

TEST_CASE("ColliderPool: remove invalidates handle, re-insert reuses slot",
          "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);

    BodyId body = BodyId::make(2U, 1U);
    ColliderId a = pool.insert(body, make_sphere(1.0F));
    ColliderId b = pool.insert(body, make_sphere(2.0F));
    REQUIRE(pool.size() == 2U);

    pool.remove(a);
    REQUIRE_FALSE(pool.contains(a));
    REQUIRE(pool.size() == 1U);

    ColliderId c = pool.insert(body, make_sphere(3.0F));
    REQUIRE(pool.contains(c));
    REQUIRE(decode_collider_per_kind_idx(c.index()) == decode_collider_per_kind_idx(a.index()));
    REQUIRE(c.generation() != a.generation());
    REQUIRE_FALSE(pool.contains(a));

    REQUIRE(pool.read(c).sphere.radius == 3.0F);
    REQUIRE(pool.read(b).sphere.radius == 2.0F);
}

TEST_CASE("ColliderPool: capacity exhaustion per kind is independent",
          "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 4); // 3 user slots per kind (slot 0 reserved)

    BodyId body = BodyId::make(1U, 1U);

    ColliderId s1 = pool.insert(body, make_sphere(1.0F));
    ColliderId s2 = pool.insert(body, make_sphere(2.0F));
    ColliderId s3 = pool.insert(body, make_sphere(3.0F));
    REQUIRE_FALSE(s1.is_null());
    REQUIRE_FALSE(s2.is_null());
    REQUIRE_FALSE(s3.is_null());

    ColliderId s4 = pool.insert(body, make_sphere(4.0F));
    REQUIRE(s4.is_null()); // sphere pool full

    // Box pool independent — should still accept.
    ColliderId x1 = pool.insert(body, make_box({1.0F, 1.0F, 1.0F}));
    REQUIRE_FALSE(x1.is_null());
}

TEST_CASE("ColliderPool: every non-v1b-b shape kind explicitly unsupported",
          "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);
    BodyId body = BodyId::make(1U, 1U);

    Collider hull{};
    hull.shape       = ColliderShape::ConvexHull;
    hull.convex_hull = ColliderConvexHull{0U, 0U};
    REQUIRE(pool.insert(body, hull).is_null()); // → v1d

    Collider plane{};
    plane.shape = ColliderShape::Plane;
    plane.plane = ColliderPlane{{0.0F, 1.0F, 0.0F}, 0.0F};
    REQUIRE(pool.insert(body, plane).is_null()); // → v1d

    Collider tri{};
    tri.shape         = ColliderShape::TriangleMesh;
    tri.triangle_mesh = crd::eylem::ColliderTriangleMesh{0U, 0U};
    REQUIRE(pool.insert(body, tri).is_null()); // → v1d-mesh

    Collider hf{};
    hf.shape       = ColliderShape::Heightfield;
    hf.heightfield = crd::eylem::ColliderHeightfield{};
    REQUIRE(pool.insert(body, hf).is_null()); // → v1d-hf

    Collider sdf{};
    sdf.shape = ColliderShape::Sdf;
    sdf.sdf   = crd::eylem::ColliderSdf{};
    REQUIRE(pool.insert(body, sdf).is_null()); // → Phase 3.1.5

    REQUIRE(pool.size() == 0U);
}

TEST_CASE("ColliderPool: null body returns null collider",
          "[eylem-rigid3d][colliderpool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    ColliderPool pool(&alloc, 64);
    REQUIRE(pool.insert(BodyId::null(), make_sphere(1.0F)).is_null());
}

TEST_CASE("ColliderPool: deterministic handle sequence",
          "[eylem-rigid3d][colliderpool]")
{
    auto run = []() {
        crd::memory::GrowableTlsfAllocator alloc;
        ColliderPool pool(&alloc, 32);
        BodyId body = BodyId::make(1U, 1U);

        std::vector<ColliderId> ids;
        ids.push_back(pool.insert(body, make_sphere(1.0F)));
        ids.push_back(pool.insert(body, make_box({1.0F, 1.0F, 1.0F})));
        ids.push_back(pool.insert(body, make_sphere(2.0F)));
        ids.push_back(pool.insert(body, make_capsule(0.5F, 1.0F)));
        pool.remove(ids[0]);
        ids.push_back(pool.insert(body, make_sphere(3.0F))); // reuses slot
        pool.remove(ids[2]);
        ids.push_back(pool.insert(body, make_box({2.0F, 2.0F, 2.0F})));
        return ids;
    };

    auto a = run();
    auto b = run();
    REQUIRE(a.size() == b.size());
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        REQUIRE(a[i].raw == b[i].raw);
    }
}
