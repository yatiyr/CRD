// The geometry-primitives kernel levels (SIMD-DISPATCH, ADR-0134 §5, §7): every level the host runs gives the portable
// level's bits for every batch kernel and the hull support scan, over spans of bundles whose lanes mix ordinary values
// with zeros, signed zeros, subnormals, infinities and NaN (zero-direction rays among them), and writes nothing past
// the span. crd-geometry-primitives-kernels-at-<level> runs these [kernels] cases under each forced level.

#include <crd/core/cpu_features.hpp>
#include <crd/geometry/primitives/primitive_dispatch.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <limits>

namespace gp = crd::geometry::primitives;
using crd::CpuLevel;
using crd::f32;
using crd::u32;
using crd::u64;
using crd::usize;
using crd::math::KernelLevel;
using gp::PrimitiveKernels;

namespace
{

constexpr usize kMaxBundles = 5;
constexpr u32 kGuard = 0x5A5A5A5AU;

bool runs_here(const KernelLevel<PrimitiveKernels>& level)
{
    return crd::cpu_level_includes(crd::cpu_selection().detected, level.level);
}

const KernelLevel<PrimitiveKernels>& portable()
{
    return gp::primitive_kernel_levels()[0];
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
};

// A random value in [-4, 4), or one of the specials about one time in six.
f32 value(Lcg& rng)
{
    constexpr f32 inf = std::numeric_limits<f32>::infinity();
    const f32 specials[] = {
        0.0F, -0.0F, inf, -inf, std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::denorm_min(),
        1.0F, -1.0F};
    const u32 pick = rng.next() % 48U;
    return pick < 8U ? specials[pick] : rng.uni(-4.0F, 4.0F);
}

// Every f32 of a plain bundle from value().
template <typename Bundle> Bundle random_bundle(Lcg& rng)
{
    static_assert(sizeof(Bundle) % sizeof(f32) == 0U);
    f32 words[sizeof(Bundle) / sizeof(f32)];
    for (f32& w : words)
    {
        w = value(rng);
    }
    Bundle b;
    std::memcpy(&b, words, sizeof(b));
    return b;
}

// The results of a call, every word kGuard before it: a call with n bundles writes every word of r[0, n) and none of
// r[n].
template <typename Result> struct Results
{
    static constexpr usize kWords = sizeof(Result) / sizeof(u32);
    Result r[kMaxBundles + 1];

    void guard() noexcept
    {
        for (Result& x : r)
        {
            u32 words[kWords];
            for (u32& w : words)
            {
                w = kGuard;
            }
            std::memcpy(&x, words, sizeof(x));
        }
    }
    [[nodiscard]] u32 word(usize bundle, usize i) const noexcept
    {
        u32 words[kWords];
        std::memcpy(words, &r[bundle], sizeof(words));
        return words[i];
    }
};

// Word i of a result is an f32 lane (compared with any NaN equal to any NaN) unless it is a mask word.
bool same_word(u32 a, u32 b, bool mask)
{
    if (a == b)
    {
        return true;
    }
    return !mask && std::isnan(std::bit_cast<f32>(a)) && std::isnan(std::bit_cast<f32>(b));
}

// The mismatches of `got` against `want` over n bundles (mask words from first_mask on), a word left unwritten and a
// guard word changed.
template <typename Result>
usize mismatches(const Results<Result>& got, const Results<Result>& want, usize n, usize first_mask)
{
    usize bad = 0;
    for (usize b = 0; b < n; ++b)
    {
        for (usize i = 0; i < Results<Result>::kWords; ++i)
        {
            bad += same_word(got.word(b, i), want.word(b, i), i >= first_mask) ? 0U : 1U;
            bad += got.word(b, i) == kGuard ? 1U : 0U;
        }
    }
    for (usize i = 0; i < Results<Result>::kWords; ++i)
    {
        bad += got.word(n, i) == kGuard ? 0U : 1U;
    }
    return bad;
}

gp::RaySlab random_ray(Lcg& rng)
{
    gp::RaySlab ray{};
    for (usize a = 0; a < 3U; ++a)
    {
        ray.origin[a] = value(rng);
        ray.direction[a] = value(rng);
        ray.inv_dir[a] = 1.0F / ray.direction[a];
        ray.sign[a] = ray.inv_dir[a] < 0.0F ? 1U : 0U;
    }
    ray.pad = 1.0000004F;
    return ray;
}

gp::Box3 random_box(Lcg& rng)
{
    gp::Box3 box{};
    for (usize a = 0; a < 3U; ++a)
    {
        box.min[a] = value(rng);
        box.max[a] = value(rng);
    }
    return box;
}

} // namespace

TEST_CASE("primitive kernels: the archive holds portable and each body level the build compiles", "[geometry][kernels]")
{
    const auto levels = gp::primitive_kernel_levels();
    REQUIRE(!levels.empty());
    CHECK(levels[0].level == CpuLevel::Portable);
    bool sse2 = false;
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
        sse2 = sse2 || levels[i].level == CpuLevel::Sse2;
        avx2 = avx2 || levels[i].level == CpuLevel::Avx2;
        avx512 = avx512 || levels[i].level == CpuLevel::Avx512;
    }
    const bool x64 = crd::cpu_selection().arch == crd::CpuArch::X64;
    CHECK(sse2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Sse2)));
    CHECK(avx2 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx2)));
    CHECK(avx512 == (x64 && CRD_TEST_KERNEL_LEVELS_TOP >= static_cast<int>(CpuLevel::Avx512)));
}

TEST_CASE("primitive kernels: every level the host runs gives the portable bits of each batch kernel",
          "[geometry][kernels]")
{
    const PrimitiveKernels& ref = *portable().table;
    usize ran = 0;
    for (const KernelLevel<PrimitiveKernels>& level : gp::primitive_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        ++ran;
        INFO("level " << crd::cpu_level_name(level.level));
        const PrimitiveKernels& k = *level.table;
        Lcg rng{0x6E0C0DE5ULL};
        usize aabb_bad = 0;
        usize packet_bad = 0;
        usize tri_bad = 0;
        usize overlap_bad = 0;
        usize sphere_bad = 0;
        usize segment_bad = 0;
        for (usize round = 0; round < 64U; ++round)
        {
            const usize n = round % (kMaxBundles + 1U);
            gp::Aabb8 boxes[kMaxBundles];
            gp::Triangle38 tris[kMaxBundles];
            gp::Sphere8 spheres[kMaxBundles];
            gp::Segment38Pair pairs[kMaxBundles];
            gp::Box3 single[kMaxBundles];
            for (usize i = 0; i < kMaxBundles; ++i)
            {
                boxes[i] = random_bundle<gp::Aabb8>(rng);
                tris[i] = random_bundle<gp::Triangle38>(rng);
                spheres[i] = random_bundle<gp::Sphere8>(rng);
                pairs[i] = random_bundle<gp::Segment38Pair>(rng);
                single[i] = random_box(rng);
            }
            const gp::RaySlab ray = random_ray(rng);
            const gp::RayPacket8 packet = random_bundle<gp::RayPacket8>(rng);
            const gp::Box3 query = random_box(rng);
            const f32 center[3] = {value(rng), value(rng), value(rng)};
            const f32 radius = value(rng);
            const f32 t0 = round % 3U == 0U ? 0.0F : value(rng);
            const f32 t1 = round % 3U == 1U ? std::numeric_limits<f32>::infinity() : value(rng);
            const bool cull = (round & 1U) != 0U;

            Results<gp::RayAabb8Result> got_a;
            Results<gp::RayAabb8Result> want_a;
            got_a.guard();
            want_a.guard();
            k.ray_vs_aabb8(ray, boxes, n, t0, t1, got_a.r);
            ref.ray_vs_aabb8(ray, boxes, n, t0, t1, want_a.r);
            aabb_bad += mismatches(got_a, want_a, n, gp::kBatchLanes);

            got_a.guard();
            want_a.guard();
            k.packet8_vs_aabb(packet, single, n, ray.pad, t0, t1, got_a.r);
            ref.packet8_vs_aabb(packet, single, n, ray.pad, t0, t1, want_a.r);
            packet_bad += mismatches(got_a, want_a, n, gp::kBatchLanes);

            Results<gp::RayTri8Result> got_t;
            Results<gp::RayTri8Result> want_t;
            got_t.guard();
            want_t.guard();
            k.ray_vs_triangle8(ray, tris, n, cull, t0, 1.0e-7F, got_t.r);
            ref.ray_vs_triangle8(ray, tris, n, cull, t0, 1.0e-7F, want_t.r);
            tri_bad += mismatches(got_t, want_t, n, 3U * gp::kBatchLanes);

            Results<gp::Mask8> got_m;
            Results<gp::Mask8> want_m;
            got_m.guard();
            want_m.guard();
            k.aabb8_vs_aabb(boxes, n, query, got_m.r);
            ref.aabb8_vs_aabb(boxes, n, query, want_m.r);
            overlap_bad += mismatches(got_m, want_m, n, 0U);

            got_m.guard();
            want_m.guard();
            k.sphere8_vs_sphere(spheres, n, center, radius, got_m.r);
            ref.sphere8_vs_sphere(spheres, n, center, radius, want_m.r);
            sphere_bad += mismatches(got_m, want_m, n, 0U);

            Results<gp::Lanes8> got_s;
            Results<gp::Lanes8> want_s;
            got_s.guard();
            want_s.guard();
            k.segment8_distsq(pairs, n, std::numeric_limits<f32>::min(), got_s.r);
            ref.segment8_distsq(pairs, n, std::numeric_limits<f32>::min(), want_s.r);
            segment_bad += mismatches(got_s, want_s, n, gp::kBatchLanes);
        }
        CHECK(aabb_bad == 0U);
        CHECK(packet_bad == 0U);
        CHECK(tri_bad == 0U);
        CHECK(overlap_bad == 0U);
        CHECK(sphere_bad == 0U);
        CHECK(segment_bad == 0U);
    }
    CHECK(ran >= 1U); // portable runs everywhere
}

TEST_CASE("primitive kernels: the masks are all ones or zero on every level", "[geometry][kernels]")
{
    // A ray along +x from -5 through seven unit boxes at the origin and one far box.
    gp::Aabb8 boxes{};
    for (usize l = 0; l < gp::kBatchLanes; ++l)
    {
        const f32 lo = l == 3U ? 99.0F : -1.0F;
        const f32 hi = l == 3U ? 100.0F : 1.0F;
        boxes.min_x[l] = lo;
        boxes.min_y[l] = lo;
        boxes.min_z[l] = lo;
        boxes.max_x[l] = hi;
        boxes.max_y[l] = hi;
        boxes.max_z[l] = hi;
    }
    gp::RaySlab ray{};
    ray.origin[0] = -5.0F;
    ray.direction[0] = 1.0F;
    for (usize a = 0; a < 3U; ++a)
    {
        ray.inv_dir[a] = 1.0F / ray.direction[a];
    }
    ray.pad = 1.0F;
    for (const KernelLevel<PrimitiveKernels>& level : gp::primitive_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        INFO("level " << crd::cpu_level_name(level.level));
        gp::RayAabb8Result r{};
        level.table->ray_vs_aabb8(ray, &boxes, 1, 0.0F, std::numeric_limits<f32>::infinity(), &r);
        for (usize l = 0; l < gp::kBatchLanes; ++l)
        {
            CHECK(r.hit_mask[l] == (l == 3U ? 0U : 0xFFFFFFFFU));
        }
        CHECK(r.t_enter[0] == 4.0F);
    }
}

TEST_CASE("primitive kernels: every level's hull support scan picks the portable vertex", "[geometry][kernels]")
{
    const PrimitiveKernels& ref = *portable().table;
    constexpr usize max_padded = 40;
    for (const KernelLevel<PrimitiveKernels>& level : gp::primitive_kernel_levels())
    {
        if (!runs_here(level))
        {
            continue;
        }
        INFO("level " << crd::cpu_level_name(level.level));
        Lcg rng{0x5C0FFEEULL};
        usize bad = 0;
        for (usize round = 0; round < 256U; ++round)
        {
            f32 vx[max_padded];
            f32 vy[max_padded];
            f32 vz[max_padded];
            const usize padded = 8U * (1U + round % 5U);
            for (usize i = 0; i < padded; ++i)
            {
                // Coarse values in a quarter of the rounds, so exact ties between vertices are common.
                vx[i] = round % 4U == 0U ? static_cast<f32>(rng.next() % 3U) : value(rng);
                vy[i] = round % 4U == 0U ? static_cast<f32>(rng.next() % 3U) : value(rng);
                vz[i] = round % 4U == 0U ? static_cast<f32>(rng.next() % 3U) : value(rng);
            }
            const f32 dx = value(rng);
            const f32 dy = value(rng);
            const f32 dz = value(rng);
            const u32 got = level.table->hull_support(vx, vy, vz, padded, dx, dy, dz);
            const u32 want = ref.hull_support(vx, vy, vz, padded, dx, dy, dz);
            bad += got == want ? 0U : 1U;
            // The scan's rule: no earlier vertex projects at least as far, no later one strictly farther.
            const f32 best = vx[want] * dx + vy[want] * dy + vz[want] * dz;
            for (usize i = 0; i < padded && !std::isnan(best); ++i)
            {
                const f32 p = vx[i] * dx + vy[i] * dy + vz[i] * dz;
                bad += (i < want && p >= best) || (i > want && p > best) ? 1U : 0U;
            }
        }
        CHECK(bad == 0U);
    }
}

TEST_CASE("primitive kernels: the process runs the level its CPU selection resolves", "[geometry][kernels]")
{
    const crd::CpuSelection& s = crd::cpu_selection();
    const auto levels = gp::primitive_kernel_levels();
    const KernelLevel<PrimitiveKernels>& chosen = gp::primitive_kernels();
    INFO("selected " << crd::cpu_level_name(s.selected) << ", running " << crd::cpu_level_name(chosen.level));
    CHECK(&chosen == &crd::math::select_kernel_level<PrimitiveKernels>(levels, s.selected, s.kernel_features,
                                                                       crd::math::kernel_level_cap()));
    CHECK(crd::cpu_level_includes(s.selected, chosen.level));
    CHECK(crd::cpu_level_includes(crd::math::kernel_level_cap(), chosen.level));
}
