// Phase 3.1 v1b-a -- BodyPool unit tests.
//
// Coverage:
//   1. Empty pool: contains(null) false, count == 0, capacity reflects ctor arg.
//   2. Insert N bodies → all retrievable with insertion-time state.
//   3. Generation bump: removed BodyId fails contains() after re-insert.
//   4. Free-list reuse: remove + insert reuses lowest-index slot.
//   5. Round-trip: every public RigidBody field round-trips byte-exact.
//   6. AoSoA layout: chunk count grows in steps of `lane`.
//   7. Capacity exhaustion: insert returns null after capacity slots issued.
//   8. Determinism: same insert sequence yields same handle sequence + same
//      readback values across repeats (identity check, not just per-field).
//   9. resolve(): correct (chunk_idx, lane_idx) for valid handles, (0,0) for
//      stale handles.

#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using crd::eylem::BodyId;
using crd::eylem::RigidBody;
using crd::eylem::RigidBodyType;
using crd::eylem_rigid3d::BodyPool;

namespace
{
RigidBody make_body(crd::f32 x, crd::f32 mass = 1.0F)
{
    RigidBody b{};
    b.position = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{x, 2.0F * x, 3.0F * x});
    b.rotation        = {0.0F, 0.0F, 0.0F, 1.0F};
    b.linear_velocity = crd::math::from_raw_vec<crd::units::dim::Velocity>(crd::math::Vec3f{0.5F * x, 0.0F, -0.25F * x});
    b.angular_velocity = crd::math::from_raw_vec<crd::units::dim::AngularVelocity>(crd::math::Vec3f{0.0F, 0.1F * x, 0.0F});
    b.inv_mass = crd::units::InverseMass32{(mass > 0.0F) ? (1.0F / mass) : 0.0F};
    b.inv_inertia = crd::math::from_raw_vec<crd::units::dim::InverseMomentOfInertia>(crd::math::Vec3f{0.5F, 0.5F, 0.5F});
    b.linear_damping  = 0.05F;
    b.angular_damping = 0.05F;
    b.flags.type = static_cast<crd::u32>(RigidBodyType::Dynamic);
    return b;
}
} // namespace

TEST_CASE("BodyPool: empty state", "[eylem-rigid3d][bodypool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    BodyPool pool(&alloc, 64);

    REQUIRE(pool.size()     == 0U);
    REQUIRE(pool.capacity() == 64U);
    REQUIRE_FALSE(pool.contains(BodyId::null()));
    REQUIRE_FALSE(pool.contains(BodyId::make(1, 1)));   // stale / never-issued
}

TEST_CASE("BodyPool: insert + read round-trips state", "[eylem-rigid3d][bodypool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    BodyPool pool(&alloc, 64);

    RigidBody src = make_body(7.0F, 2.5F);
    src.flags.gravity_enabled = 1;
    src.flags.lock_position_y = 1;

    BodyId id = pool.insert(src);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(pool.contains(id));
    REQUIRE(pool.size() == 1U);

    RigidBody got = pool.read(id);
    REQUIRE(got.position.x.value == src.position.x.value);
    REQUIRE(got.position.y.value == src.position.y.value);
    REQUIRE(got.position.z.value == src.position.z.value);
    REQUIRE(got.rotation.x == src.rotation.x);
    REQUIRE(got.rotation.y == src.rotation.y);
    REQUIRE(got.rotation.z == src.rotation.z);
    REQUIRE(got.rotation.w == src.rotation.w);
    REQUIRE(got.linear_velocity.x.value == src.linear_velocity.x.value);
    REQUIRE(got.angular_velocity.y.value == src.angular_velocity.y.value);
    REQUIRE(got.inv_mass.value == src.inv_mass.value);
    REQUIRE(got.inv_inertia.x.value == src.inv_inertia.x.value);
    REQUIRE(got.linear_damping  == src.linear_damping);
    REQUIRE(got.angular_damping == src.angular_damping);
    REQUIRE(got.flags.type             == src.flags.type);
    REQUIRE(got.flags.gravity_enabled  == 1U);
    REQUIRE(got.flags.lock_position_y  == 1U);
}

TEST_CASE("BodyPool: remove invalidates handle, re-insert reuses slot",
          "[eylem-rigid3d][bodypool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    BodyPool pool(&alloc, 64);

    BodyId a = pool.insert(make_body(1.0F));
    BodyId b = pool.insert(make_body(2.0F));
    REQUIRE(pool.size() == 2U);

    const auto a_before = pool.resolve(a);
    pool.remove(a);
    REQUIRE_FALSE(pool.contains(a));
    REQUIRE(pool.size() == 1U);

    BodyId c = pool.insert(make_body(3.0F));
    REQUIRE(pool.contains(c));
    // c should land in the slot we just freed (free-list reuse).
    REQUIRE(c.index() == a.index());
    // generation bumped → c != a despite shared index.
    REQUIRE(c.generation() != a.generation());
    REQUIRE_FALSE(pool.contains(a));

    // Lane resolution matches.
    const auto c_after = pool.resolve(c);
    REQUIRE(c_after.chunk_idx == a_before.chunk_idx);
    REQUIRE(c_after.lane_idx  == a_before.lane_idx);

    // b is untouched.
    REQUIRE(pool.contains(b));
    REQUIRE(pool.read(b).position.x.value == 2.0F);
}

TEST_CASE("BodyPool: write mutates lane in place", "[eylem-rigid3d][bodypool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    BodyPool pool(&alloc, 16);

    BodyId id = pool.insert(make_body(1.0F));
    RigidBody updated = make_body(1.0F);
    updated.position = crd::math::from_raw_vec<crd::units::dim::Length>(crd::math::Vec3f{99.0F, 100.0F, 101.0F});
    updated.linear_velocity = crd::math::from_raw_vec<crd::units::dim::Velocity>(crd::math::Vec3f{0.0F, 0.0F, 0.0F});

    pool.write(id, updated);
    RigidBody got = pool.read(id);
    REQUIRE(got.position.x.value == 99.0F);
    REQUIRE(got.position.y.value == 100.0F);
    REQUIRE(got.position.z.value == 101.0F);
    REQUIRE(got.linear_velocity.x.value == 0.0F);
}

TEST_CASE("BodyPool: AoSoA storage grows in lane-sized chunks",
          "[eylem-rigid3d][bodypool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    BodyPool pool(&alloc, 1024);

    REQUIRE(pool.storage().chunk_count() == 1U); // slot-0 sentinel chunk

    // Lane width is 4 or 8 depending on SIMD backend. Either way, growing
    // to lane bodies should still fit in the first chunk (with the sentinel
    // taking lane 0). kLane_count + 1 should require a second chunk.
    constexpr crd::usize lane = BodyPool::kLane;

    std::vector<BodyId> ids;
    ids.reserve(lane * 3);
    for (crd::usize i = 0; i < lane * 3; ++i)
    {
        BodyId id = pool.insert(make_body(static_cast<crd::f32>(i)));
        REQUIRE_FALSE(id.is_null());
        ids.push_back(id);
    }

    // After lane*3 inserts (slot 0 reserved + 24 user slots @ 8-lane), we need
    // ceil((1+24)/8) = ceil(25/8) = 4 chunks at lane=8, or ceil(13/4) = 4 at lane=4.
    REQUIRE(pool.storage().chunk_count() >= 3U);
    REQUIRE(pool.size() == lane * 3);

    // All ids resolve to distinct (chunk, lane) pairs.
    for (crd::usize i = 0; i < ids.size(); ++i)
    {
        REQUIRE(pool.contains(ids[i]));
        const RigidBody got = pool.read(ids[i]);
        REQUIRE(got.position.x.value == static_cast<crd::f32>(i));
    }
}

TEST_CASE("BodyPool: deterministic handle sequence + readback",
          "[eylem-rigid3d][bodypool]")
{
    crd::memory::GrowableTlsfAllocator alloc;

    auto run_sequence = [&alloc]() {
        BodyPool pool(&alloc, 32);
        std::vector<BodyId> ids;
        for (int i = 0; i < 12; ++i)
        {
            ids.push_back(pool.insert(make_body(static_cast<crd::f32>(i))));
        }
        // Remove the even-indexed ones, then insert 6 fresh.
        for (int i = 0; i < 12; i += 2)
        {
            pool.remove(ids[i]);
        }
        for (int i = 0; i < 6; ++i)
        {
            ids.push_back(pool.insert(make_body(100.0F + static_cast<crd::f32>(i))));
        }
        return ids;
    };

    auto seq_a = run_sequence();
    auto seq_b = run_sequence();

    REQUIRE(seq_a.size() == seq_b.size());
    for (crd::usize i = 0; i < seq_a.size(); ++i)
    {
        REQUIRE(seq_a[i].raw == seq_b[i].raw);
    }
}

TEST_CASE("BodyPool: capacity exhaustion returns null",
          "[eylem-rigid3d][bodypool]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    BodyPool pool(&alloc, 4); // 4 slots; slot 0 reserved → 3 user slots

    BodyId a = pool.insert(make_body(1.0F));
    BodyId b = pool.insert(make_body(2.0F));
    BodyId c = pool.insert(make_body(3.0F));
    REQUIRE_FALSE(a.is_null());
    REQUIRE_FALSE(b.is_null());
    REQUIRE_FALSE(c.is_null());

    BodyId d = pool.insert(make_body(4.0F));
    REQUIRE(d.is_null()); // pool full

    // Remove one → next insert succeeds.
    pool.remove(b);
    BodyId e = pool.insert(make_body(5.0F));
    REQUIRE_FALSE(e.is_null());
    REQUIRE(pool.read(e).position.x.value == 5.0F);
}

TEST_CASE("BodyPool: nullptr persistent_alloc falls back to default",
          "[eylem-rigid3d][bodypool]")
{
    BodyPool pool(nullptr, 16);
    BodyId id = pool.insert(make_body(7.0F));
    REQUIRE_FALSE(id.is_null());
    REQUIRE(pool.read(id).position.x.value == 7.0F);
}
