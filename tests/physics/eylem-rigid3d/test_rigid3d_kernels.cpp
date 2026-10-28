// The eylem-rigid3d kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): the portable body step gives the bits of the
// scalar integrator (the v1b-c EylemSystem body on RigidBody and crd::math, kept here as the oracle), and every level
// the host runs gives the portable bits, over 0 to 5 chunks with guard chunks after them and lanes mixing free and
// live slots, inv_mass of 0, -0, negative, NaN, infinite and subnormal, signed-zero, subnormal, infinite and NaN
// velocities, damping past 1/dt, quaternion lengths about try_normalize's epsilon and dt of 0, subnormal and 1/60.
// EylemSystem runs the selected level once per substep over the whole pool: an unattached body integrates and a body
// two entities share integrates once. crd-eylem-rigid3d-kernels-at-<level> runs these [kernels] cases under each
// forced level.

#include <crd/core/cpu_features.hpp>
#include <crd/eylem/components.hpp>
#include <crd/eylem/physics_config.hpp>
#include <crd/eylem/rigid_body.hpp>
#include <crd/eylem_rigid3d/body_pool.hpp>
#include <crd/eylem_rigid3d/eylem_system.hpp>
#include <crd/eylem_rigid3d/rigid3d_dispatch.hpp>
#include <crd/math/quat.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace er = crd::eylem_rigid3d;
using crd::f32;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::eylem::RigidBody;
using crd::math::KernelLevel;
using er::BodyChunk;
using er::kChunkLanes;
using er::Rigid3dKernels;

namespace
{

constexpr usize kFloatColumns = 26;
constexpr usize kFloatBytes = kFloatColumns * kChunkLanes * sizeof(f32);
constexpr usize kChunkDataBytes = kFloatBytes + kChunkLanes * (sizeof(u32) + 2U * sizeof(u8));
static_assert(offsetof(BodyChunk, flags) == kFloatBytes, "the float columns are contiguous");

bool runs_here(const KernelLevel<Rigid3dKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

struct Lcg
{
    u64 s;
    u32 next() noexcept
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(s >> 32);
    }
    f32 uni(f32 lo, f32 hi) noexcept { return lo + static_cast<f32>(next() >> 8) * (1.0F / 16777216.0F) * (hi - lo); }
    // One of the special values, or a uniform one in [lo, hi).
    f32 pick(const f32* specials, usize n, f32 lo, f32 hi) noexcept
    {
        const u32 r = next() % static_cast<u32>(2U * n);
        return r < n ? specials[r] : uni(lo, hi);
    }
};

constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr f32 kNan = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kSub = 1.0e-40F;

// Column c of lane i, in BodyChunk's member order.
f32* column(BodyChunk& b, usize c) noexcept
{
    return &b.pos_x[0] + c * kChunkLanes;
}

// Values that reach the kernel through memory, never as literals the compiler could fold.
void fill(BodyChunk& b, Lcg& rng) noexcept
{
    b = BodyChunk{};
    static constexpr f32 kMass[] = {0.0F, -0.0F, -1.0F, kNan, kInf, kSub, 1.0F, 0.25F};
    static constexpr f32 kVel[] = {0.0F, -0.0F, kSub, -kSub, kInf, -kInf, kNan};
    static constexpr f32 kDamp[] = {0.0F, 0.05F, 100.0F, -3.0F, kNan};
    static constexpr f32 kPos[] = {0.0F, -0.0F, kInf, kNan, 1.0e30F};
    for (usize i = 0; i < kChunkLanes; ++i)
    {
        b.pos_x[i] = rng.pick(kPos, 5, -100.0F, 100.0F);
        b.pos_y[i] = rng.pick(kPos, 5, -100.0F, 100.0F);
        b.pos_z[i] = rng.pick(kPos, 5, -100.0F, 100.0F);
        const u32 shape = rng.next() % 8U;
        if (shape < 4U)
        {
            // About unit length, or scaled to just below, at and just above try_normalize's epsilon.
            static constexpr f32 kScale[] = {1.0F, 0.99999F * 1.0e-5F, 1.0e-5F, 1.00001F * 1.0e-5F};
            f32 q[4] = {rng.uni(-1.0F, 1.0F), rng.uni(-1.0F, 1.0F), rng.uni(-1.0F, 1.0F), rng.uni(-1.0F, 1.0F)};
            f32 len = 0.0F;
            for (const f32 v : q)
            {
                len += v * v;
            }
            len = std::sqrt(len);
            for (f32& v : q)
            {
                v = v / len * kScale[shape];
            }
            b.rot_x[i] = q[0];
            b.rot_y[i] = q[1];
            b.rot_z[i] = q[2];
            b.rot_w[i] = q[3];
        }
        else if (shape == 4U)
        {
            b.rot_x[i] = 0.0F;
            b.rot_y[i] = -0.0F;
            b.rot_z[i] = 0.0F;
            b.rot_w[i] = 0.0F;
        }
        else if (shape == 5U)
        {
            b.rot_x[i] = kInf;
            b.rot_w[i] = 1.0F;
        }
        else
        {
            b.rot_x[i] = 1.0e-5F * rng.uni(0.5F, 1.5F); // a length about epsilon on one axis
        }
        b.linvel_x[i] = rng.pick(kVel, 7, -50.0F, 50.0F);
        b.linvel_y[i] = rng.pick(kVel, 7, -50.0F, 50.0F);
        b.linvel_z[i] = rng.pick(kVel, 7, -50.0F, 50.0F);
        const bool spin = rng.next() % 4U != 0U; // a still lane keeps q's length into the epsilon test
        b.angvel_x[i] = spin ? rng.pick(kVel, 7, -10.0F, 10.0F) : 0.0F;
        b.angvel_y[i] = spin ? rng.pick(kVel, 7, -10.0F, 10.0F) : 0.0F;
        b.angvel_z[i] = spin ? rng.pick(kVel, 7, -10.0F, 10.0F) : 0.0F;
        b.inv_mass[i] = rng.pick(kMass, 8, 0.0F, 4.0F);
        b.inv_inertia_x[i] = rng.uni(0.0F, 2.0F);
        b.inv_inertia_y[i] = rng.uni(0.0F, 2.0F);
        b.inv_inertia_z[i] = rng.uni(0.0F, 2.0F);
        b.linear_damping[i] = rng.pick(kDamp, 5, 0.0F, 1.0F);
        b.angular_damping[i] = rng.pick(kDamp, 5, 0.0F, 1.0F);
        for (usize c = 19; c < kFloatColumns; ++c)
        {
            column(b, c)[i] = rng.uni(-5.0F, 5.0F); // a stale prev pose the step overwrites
        }
        b.flags[i] = rng.next();
        b.generation[i] = static_cast<u8>(rng.next());
        b.live[i] = static_cast<u8>(rng.next() % 3U != 0U ? 1U : 0U);
    }
}

// The v1b-c EylemSystem body (eylem_system.cpp before the kernel) on one lane, operation for operation.
void oracle_lane(BodyChunk& b, usize i, const crd::math::Vec3<crd::units::Acceleration32>& gravity,
                 crd::units::Duration32 dt)
{
    b.prev_pos_x[i] = b.pos_x[i];
    b.prev_pos_y[i] = b.pos_y[i];
    b.prev_pos_z[i] = b.pos_z[i];
    b.prev_rot_x[i] = b.rot_x[i];
    b.prev_rot_y[i] = b.rot_y[i];
    b.prev_rot_z[i] = b.rot_z[i];
    b.prev_rot_w[i] = b.rot_w[i];
    if (b.live[i] == 0U)
    {
        return;
    }
    RigidBody body{};
    body.position.x = crd::units::Length32{b.pos_x[i]};
    body.position.y = crd::units::Length32{b.pos_y[i]};
    body.position.z = crd::units::Length32{b.pos_z[i]};
    body.rotation = crd::math::Quatf{b.rot_x[i], b.rot_y[i], b.rot_z[i], b.rot_w[i]};
    body.linear_velocity.x = crd::units::Velocity32{b.linvel_x[i]};
    body.linear_velocity.y = crd::units::Velocity32{b.linvel_y[i]};
    body.linear_velocity.z = crd::units::Velocity32{b.linvel_z[i]};
    body.angular_velocity.x = crd::units::AngularVelocity32{b.angvel_x[i]};
    body.angular_velocity.y = crd::units::AngularVelocity32{b.angvel_y[i]};
    body.angular_velocity.z = crd::units::AngularVelocity32{b.angvel_z[i]};
    body.inv_mass = crd::units::InverseMass32{b.inv_mass[i]};
    body.linear_damping = b.linear_damping[i];
    body.angular_damping = b.angular_damping[i];
    if (body.inv_mass.value > 0.0F)
    {
        body.linear_velocity += gravity * dt;
        const f32 lin_damp_factor = 1.0F - body.linear_damping * dt.value;
        body.linear_velocity = body.linear_velocity * lin_damp_factor;
        body.position += body.linear_velocity * dt;
        const f32 ang_damp_factor = 1.0F - body.angular_damping * dt.value;
        body.angular_velocity = body.angular_velocity * ang_damp_factor;
        const f32 half_dt = 0.5F * dt.value;
        const crd::math::Quatf omega_q{body.angular_velocity.x.value, body.angular_velocity.y.value,
                                       body.angular_velocity.z.value, 0.0F};
        const crd::math::Quatf q_dot = omega_q * body.rotation;
        crd::math::Quatf q_new{body.rotation.x + half_dt * q_dot.x, body.rotation.y + half_dt * q_dot.y,
                               body.rotation.z + half_dt * q_dot.z, body.rotation.w + half_dt * q_dot.w};
        (void)crd::math::try_normalize(q_new);
        body.rotation = q_new;
    }
    b.pos_x[i] = body.position.x.value;
    b.pos_y[i] = body.position.y.value;
    b.pos_z[i] = body.position.z.value;
    b.rot_x[i] = body.rotation.x;
    b.rot_y[i] = body.rotation.y;
    b.rot_z[i] = body.rotation.z;
    b.rot_w[i] = body.rotation.w;
    b.linvel_x[i] = body.linear_velocity.x.value;
    b.linvel_y[i] = body.linear_velocity.y.value;
    b.linvel_z[i] = body.linear_velocity.z.value;
    b.angvel_x[i] = body.angular_velocity.x.value;
    b.angvel_y[i] = body.angular_velocity.y.value;
    b.angvel_z[i] = body.angular_velocity.z.value;
}

// The float columns equal bit for bit, two NaNs counting as equal (ADR-0134 §7), the side-bands byte for byte.
bool same(const BodyChunk& a, const BodyChunk& b) noexcept
{
    u32 x[kFloatColumns * kChunkLanes];
    u32 y[kFloatColumns * kChunkLanes];
    std::memcpy(x, &a, kFloatBytes);
    std::memcpy(y, &b, kFloatBytes);
    for (usize k = 0; k < kFloatColumns * kChunkLanes; ++k)
    {
        const bool nan_x = (x[k] & 0x7FFFFFFFU) > 0x7F800000U;
        const bool nan_y = (y[k] & 0x7FFFFFFFU) > 0x7F800000U;
        if (nan_x != nan_y || (!nan_x && x[k] != y[k]))
        {
            return false;
        }
    }
    return std::memcmp(reinterpret_cast<const unsigned char*>(&a) + kFloatBytes,
                       reinterpret_cast<const unsigned char*>(&b) + kFloatBytes, kChunkDataBytes - kFloatBytes) == 0;
}

constexpr usize kMaxChunks = 5;
constexpr usize kGuards = 2;

struct Case
{
    f32 gravity[3];
    f32 dt;
};

// Gravity and dt through memory: earth gravity at 1/60, a non-finite gravity component, dt of 0 and subnormal.
void make_cases(Case* cases, Lcg& rng) noexcept
{
    const f32 dts[] = {1.0F / 60.0F, 0.0F, kSub, rng.uni(0.001F, 0.1F)};
    for (usize k = 0; k < 4U; ++k)
    {
        cases[k] = Case{{rng.uni(-1.0F, 1.0F), -9.81F, rng.uni(-1.0F, 1.0F)}, dts[k]};
    }
    cases[4] = Case{{kNan, -9.81F, kInf}, 1.0F / 60.0F};
}

} // namespace

TEST_CASE("rigid3d kernels: the archive holds portable and each body level the build compiles", "[eylem][kernels]")
{
    const auto levels = er::rigid3d_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == crd::CpuLevel::Portable);
    bool avx2 = false;
    bool avx512 = false;
    for (usize i = 0; i < levels.size(); ++i)
    {
        CHECK(levels[i].table != nullptr);
        CHECK(static_cast<int>(levels[i].level) <= CRD_TEST_KERNEL_LEVELS_TOP);
        if (i > 0U)
        {
            CHECK(static_cast<int>(levels[i - 1U].level) < static_cast<int>(levels[i].level));
        }
        avx2 = avx2 || levels[i].level == crd::CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == crd::CpuLevel::Avx512;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(crd::CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(crd::CpuLevel::Avx512)));
}

TEST_CASE("rigid3d kernels: portable is the scalar integrator and every level the host runs gives its bits",
          "[eylem][kernels]")
{
    const Rigid3dKernels& portable = *er::rigid3d_kernel_levels()[0].table;
    Lcg rng{0x5EEDULL};
    Case cases[5];
    make_cases(cases, rng);
    usize ran = 0;
    usize lanes_integrated = 0;
    for (const Case& cs : cases)
    {
        const crd::math::Vec3<crd::units::Acceleration32> gravity{crd::units::Acceleration32{cs.gravity[0]},
                                                                  crd::units::Acceleration32{cs.gravity[1]},
                                                                  crd::units::Acceleration32{cs.gravity[2]}};
        for (usize count = 0; count <= kMaxChunks; ++count)
        {
            BodyChunk input[kMaxChunks + kGuards];
            for (BodyChunk& b : input)
            {
                fill(b, rng);
            }
            BodyChunk want[kMaxChunks + kGuards];
            std::memcpy(static_cast<void*>(want), input, sizeof(input));
            for (usize c = 0; c < count; ++c)
            {
                for (usize i = 0; i < kChunkLanes; ++i)
                {
                    oracle_lane(want[c], i, gravity, crd::units::Duration32{cs.dt});
                    lanes_integrated += want[c].live[i] != 0U && input[c].inv_mass[i] > 0.0F ? 1U : 0U;
                }
            }
            INFO("dt " << cs.dt << " chunks " << count);

            BodyChunk got[kMaxChunks + kGuards];
            std::memcpy(static_cast<void*>(got), input, sizeof(input));
            portable.step_bodies(got, count, cs.gravity, cs.dt);
            usize bad = 0;
            for (usize c = 0; c < kMaxChunks + kGuards; ++c)
            {
                bad += same(got[c], want[c]) ? 0U : 1U; // the chunks past count are guards the step leaves alone
            }
            CHECK(bad == 0U);

            for (const KernelLevel<Rigid3dKernels>& level : er::rigid3d_kernel_levels())
            {
                if (!runs_here(level))
                {
                    continue;
                }
                ++ran;
                INFO("level " << crd::cpu_level_name(level.level));
                BodyChunk out[kMaxChunks + kGuards];
                std::memcpy(static_cast<void*>(out), input, sizeof(input));
                level.table->step_bodies(out, count, cs.gravity, cs.dt);
                usize level_bad = 0;
                for (usize c = 0; c < kMaxChunks + kGuards; ++c)
                {
                    level_bad += same(out[c], got[c]) ? 0U : 1U;
                }
                CHECK(level_bad == 0U);
            }
        }
    }
    CHECK(ran >= 1U);
    CHECK(lanes_integrated > 200U); // the corpus integrates often enough to compare
}

TEST_CASE("rigid3d kernels: EylemSystem steps the whole pool once a substep on the selected level", "[eylem][kernels]")
{
    constexpr f32 k_dt = 1.0F / 60.0F;
    er::BodyPool pool{crd::memory::default_allocator(), 64U};
    crd::eylem::PhysicsConfig cfg{};
    cfg.fixed_dt = crd::units::Duration32{k_dt};
    cfg.gravity = crd::math::from_raw_vec<crd::units::dim::Acceleration>(crd::math::Vec3f{0.5F, -9.81F, 0.25F});
    er::EylemSystem system{pool, cfg};

    crd::scene::World world{crd::memory::default_allocator()};
    world.register_component<crd::scene::Transform>(crd::scene::StorageHint::Archetype);
    world.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    world.register_component<crd::eylem::RigidBodyComponent>(crd::scene::StorageHint::SparseSet);

    // Twenty bodies over three chunks: the one with index 3 has no entity, the one with index 7 two, the rest one.
    Lcg rng{0xB0D1ULL};
    crd::eylem::BodyId ids[20];
    crd::scene::EntityId shared[2]{};
    for (usize k = 0; k < 20U; ++k)
    {
        RigidBody body{};
        body.position = crd::math::from_raw_vec<crd::units::dim::Length>(
            crd::math::Vec3f{rng.uni(-5.0F, 5.0F), rng.uni(0.0F, 10.0F), rng.uni(-5.0F, 5.0F)});
        body.linear_velocity = crd::math::from_raw_vec<crd::units::dim::Velocity>(
            crd::math::Vec3f{rng.uni(-1.0F, 1.0F), rng.uni(-1.0F, 1.0F), rng.uni(-1.0F, 1.0F)});
        body.angular_velocity = crd::math::from_raw_vec<crd::units::dim::AngularVelocity>(
            crd::math::Vec3f{rng.uni(-2.0F, 2.0F), rng.uni(-2.0F, 2.0F), rng.uni(-2.0F, 2.0F)});
        body.inv_mass = crd::units::InverseMass32{k % 5U == 0U ? 0.0F : rng.uni(0.1F, 2.0F)};
        ids[k] = pool.insert(body);
        REQUIRE_FALSE(ids[k].is_null());
        usize entities = 1;
        if (k == 3U)
        {
            entities = 0;
        }
        else if (k == 7U)
        {
            entities = 2;
        }
        for (usize e = 0; e < entities; ++e)
        {
            const auto entity = world.spawn();
            world.add_component(entity, crd::scene::Transform{});
            crd::eylem::RigidBodyComponent rbc{};
            rbc.body_id = ids[k];
            world.add_component(entity, rbc);
            if (k == 7U)
            {
                shared[e] = entity;
            }
        }
    }
    pool.remove(ids[12]); // a free lane among live ones

    const er::BodyPool::Storage& storage = pool.storage();
    REQUIRE(storage.chunk_count() == 3U);
    BodyChunk want[3];
    std::memcpy(static_cast<void*>(want), storage.chunks().data(), sizeof(want));
    const RigidBody before = pool.read(ids[3]);
    const f32 gravity[3] = {0.5F, -9.81F, 0.25F};

    for (usize step = 0; step < 3U; ++step)
    {
        system.run(world);
        er::rigid3d_kernels().table->step_bodies(want, 3U, gravity, k_dt);
    }
    usize bad = 0;
    for (usize c = 0; c < 3U; ++c)
    {
        bad += std::memcmp(&storage.chunks().data()[c], &want[c], kChunkDataBytes) == 0 ? 0U : 1U;
    }
    CHECK(bad == 0U);

    // The unattached body integrated; the shared one integrated once a substep, and both its entities hold its pose
    // (World::set_rotation_quat stores the rotation its own way, so the two are compared with each other).
    CHECK(std::bit_cast<u32>(pool.read(ids[3]).position.y.value) != std::bit_cast<u32>(before.position.y.value));
    const er::BodyPool::Pose pose = pool.read_pose(ids[7]);
    const auto* first = world.get_component<crd::scene::Transform>(shared[0]);
    const auto* second = world.get_component<crd::scene::Transform>(shared[1]);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(std::bit_cast<u32>(first->translation.y.value) == std::bit_cast<u32>(pose.position.y.value));
    CHECK(std::bit_cast<u32>(second->translation.y.value) == std::bit_cast<u32>(pose.position.y.value));
    CHECK(std::bit_cast<u32>(first->rotation.w) == std::bit_cast<u32>(second->rotation.w));
}
