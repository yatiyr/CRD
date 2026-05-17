// SIMD substrate micro-benchmarks — Phase 3.1 v0e.
//
// AoSoA-8 vs scalar throughput for the four hot operations eylem v1+ will
// rely on per physics tick:
//   - Vec3f dot        — constraint normal/velocity projection
//   - Vec3f cross      — torque computation
//   - Mat4f multiply   — body-to-world transform composition
//   - Quatf compose    — orientation accumulation
//
// Each benchmark runs the same workload once via the scalar API and once via
// the SIMD API. The SIMD version processes 8 logical entities per call by
// laying out the data AoSoA-8 (one Vec8f per axis); the scalar version
// processes them one Vec3f at a time. Catch2's BENCHMARK macro reports
// median + variance.
//
// Goal-line numbers (win-release on AVX2 desktop, captured 2026-05-10 —
// regenerate when changing target hardware):
//   Vec3f dot scalar (8 ops):      ~3.5–4.5 ns
//   Vec3f dot AoSoA-8 SIMD:        ~0.5–1.0 ns  ← ~6× speedup
//   Vec3f cross scalar (8 ops):    ~10–15 ns
//   Vec3f cross AoSoA-8 SIMD:      ~2–3 ns      ← ~5–6× speedup
//   Mat4f multiply scalar (8 ops): ~100–200 ns
//   Mat4f multiply SIMD (8 ops):   ~7–15 ns     ← ~12× speedup (the scalar
//                                                   path doesn't auto-vectorise
//                                                   the row × column inner loop)
//   Quatf compose scalar (8 ops):  ~2–4 ns
//   Quatf compose SIMD (8 ops):    ~3–6 ns      ← REGRESSION: per-instance
//                                                   Quatf SIMD has Vec4f
//                                                   store-to-stack overhead
//                                                   without amortisation.
//                                                   For the SIMD speedup,
//                                                   batch 8 quaternions in
//                                                   an AoSoA-8 layout and
//                                                   compute the Hamilton
//                                                   product per-axis. (Reserved
//                                                   for eylem v4 articulation
//                                                   joint composition.)
//
// Regressions > ~30% on AVX2 builds for the dot/cross/mat4 cases →
// investigate (compiler change, flags drift, CrdSimd.cmake refactor).
// The Quatf SIMD-vs-scalar gap is expected and documented.
//
// To run: build crd-bench in any preset, then `./crd-bench [bench-simd]`.
// crd-bench is intentionally NOT registered with CTest (timing is
// environment-dependent; bench numbers are for human inspection).

#include <crd/math/math.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/math/simd/mat4f.hpp>
#include <crd/math/simd/quatf.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::math::Vec3f;
using crd::math::Mat4f;
using crd::math::Quatf;
namespace simd = crd::math::simd;

namespace
{
// Eight Vec3f pairs in AoS layout (scalar baseline).
struct AoSeight
{
    Vec3f a[8];
    Vec3f b[8];
};

// Same eight Vec3f pairs in AoSoA-8 layout (SIMD baseline).
struct AoSoA8
{
    simd::Vec8f ax, ay, az;
    simd::Vec8f bx, by, bz;
};

[[nodiscard]] AoSeight make_aos_pairs() noexcept
{
    AoSeight p;
    for (int i = 0; i < 8; ++i)
    {
        const f32 t = 0.1F + static_cast<f32>(i) * 0.13F;
        p.a[i] = Vec3f(t,        t * 1.5F, t * 0.7F);
        p.b[i] = Vec3f(t * 0.3F, t * 1.1F, t * 0.9F);
    }
    return p;
}

[[nodiscard]] AoSoA8 make_aosoa_pairs() noexcept
{
    const AoSeight p = make_aos_pairs();
    AoSoA8 r;
    r.ax = simd::Vec8f(p.a[0].x, p.a[1].x, p.a[2].x, p.a[3].x,
                       p.a[4].x, p.a[5].x, p.a[6].x, p.a[7].x);
    r.ay = simd::Vec8f(p.a[0].y, p.a[1].y, p.a[2].y, p.a[3].y,
                       p.a[4].y, p.a[5].y, p.a[6].y, p.a[7].y);
    r.az = simd::Vec8f(p.a[0].z, p.a[1].z, p.a[2].z, p.a[3].z,
                       p.a[4].z, p.a[5].z, p.a[6].z, p.a[7].z);
    r.bx = simd::Vec8f(p.b[0].x, p.b[1].x, p.b[2].x, p.b[3].x,
                       p.b[4].x, p.b[5].x, p.b[6].x, p.b[7].x);
    r.by = simd::Vec8f(p.b[0].y, p.b[1].y, p.b[2].y, p.b[3].y,
                       p.b[4].y, p.b[5].y, p.b[6].y, p.b[7].y);
    r.bz = simd::Vec8f(p.b[0].z, p.b[1].z, p.b[2].z, p.b[3].z,
                       p.b[4].z, p.b[5].z, p.b[6].z, p.b[7].z);
    return r;
}
}  // namespace

TEST_CASE("bench Vec3f dot -- scalar vs AoSoA-8", "[bench][bench-simd][!benchmark]")
{
    const AoSeight aos    = make_aos_pairs();
    const AoSoA8   aosoa  = make_aosoa_pairs();

    BENCHMARK("Vec3f dot scalar (8 ops)")
    {
        f32 acc = 0.0F;
        for (int i = 0; i < 8; ++i)
        {
            acc += crd::math::dot(aos.a[i], aos.b[i]);
        }
        return acc;
    };

    BENCHMARK("Vec3f dot AoSoA-8 SIMD")
    {
        const simd::Vec8f result = aosoa.ax * aosoa.bx + aosoa.ay * aosoa.by + aosoa.az * aosoa.bz;
        return result.lane(0);
    };
}

TEST_CASE("bench Vec3f cross -- scalar vs AoSoA-8", "[bench][bench-simd][!benchmark]")
{
    const AoSeight aos    = make_aos_pairs();
    const AoSoA8   aosoa  = make_aosoa_pairs();

    BENCHMARK("Vec3f cross scalar (8 ops)")
    {
        f32 acc = 0.0F;
        for (int i = 0; i < 8; ++i)
        {
            const Vec3f c = crd::math::cross(aos.a[i], aos.b[i]);
            acc += c.x + c.y + c.z;
        }
        return acc;
    };

    BENCHMARK("Vec3f cross AoSoA-8 SIMD")
    {
        // cross.x = a.y*b.z - a.z*b.y
        // cross.y = a.z*b.x - a.x*b.z
        // cross.z = a.x*b.y - a.y*b.x
        const simd::Vec8f cx = aosoa.ay * aosoa.bz - aosoa.az * aosoa.by;
        const simd::Vec8f cy = aosoa.az * aosoa.bx - aosoa.ax * aosoa.bz;
        const simd::Vec8f cz = aosoa.ax * aosoa.by - aosoa.ay * aosoa.bx;
        return cx.lane(0) + cy.lane(0) + cz.lane(0);
    };
}

TEST_CASE("bench Mat4f multiply -- public API (SIMD-routed) vs explicit simd::Mat4f", "[bench][bench-simd][!benchmark]")
{
    // Phase 3.1 v0f shipped: the public `Mat4f operator*` now routes through
    // SIMD internally (mat_simd_f32.hpp). So both benchmarks below are SIMD
    // paths — the "public API" call is no longer scalar. Both should clock
    // ~9-10 ns on AVX2 desktop, proving v0f delivered. The pre-v0f baseline
    // of 122.97 ns scalar is documented as "the speedup that v0f shipped".
    Mat4f a;
    Mat4f b;
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            f32* a_lane = reinterpret_cast<f32*>(&a[static_cast<crd::usize>(c)]);
            f32* b_lane = reinterpret_cast<f32*>(&b[static_cast<crd::usize>(c)]);
            a_lane[r] = 0.1F + 0.07F * static_cast<f32>(r * 4 + c);
            b_lane[r] = 0.2F + 0.05F * static_cast<f32>(c * 4 + r);
        }
    }

    BENCHMARK("Mat4f multiply public API (now SIMD-routed, 8 ops)")
    {
        f32 acc = 0.0F;
        for (int i = 0; i < 8; ++i)
        {
            const Mat4f c = a * b;
            acc += c[0].x;  // sink to prevent dead-code elim
        }
        return acc;
    };

    // Build SIMD versions of the same matrices (column-major copy).
    simd::Mat4f sa(simd::Vec4f(a[0].x, a[0].y, a[0].z, a[0].w),
                   simd::Vec4f(a[1].x, a[1].y, a[1].z, a[1].w),
                   simd::Vec4f(a[2].x, a[2].y, a[2].z, a[2].w),
                   simd::Vec4f(a[3].x, a[3].y, a[3].z, a[3].w));
    simd::Mat4f sb(simd::Vec4f(b[0].x, b[0].y, b[0].z, b[0].w),
                   simd::Vec4f(b[1].x, b[1].y, b[1].z, b[1].w),
                   simd::Vec4f(b[2].x, b[2].y, b[2].z, b[2].w),
                   simd::Vec4f(b[3].x, b[3].y, b[3].z, b[3].w));

    BENCHMARK("Mat4f multiply SIMD (8 ops)")
    {
        f32 acc = 0.0F;
        for (int i = 0; i < 8; ++i)
        {
            const simd::Mat4f c = sa * sb;
            acc += c.cols[0].lane(0);
        }
        return acc;
    };
}

TEST_CASE("bench Quatf compose -- scalar vs SIMD", "[bench][bench-simd][!benchmark]")
{
    // Fixed test quaternions (normalized, ~30° rotation each).
    const Quatf qa(0.0F, 0.0F, 0.2588F, 0.9659F);  // 30° about Z
    const Quatf qb(0.0F, 0.2588F, 0.0F, 0.9659F);  // 30° about Y

    BENCHMARK("Quatf compose scalar (8 ops)")
    {
        f32 acc = 0.0F;
        for (int i = 0; i < 8; ++i)
        {
            const Quatf c = qa * qb;
            acc += c.w;
        }
        return acc;
    };

    const simd::Quatf sqa(qa.x, qa.y, qa.z, qa.w);
    const simd::Quatf sqb(qb.x, qb.y, qb.z, qb.w);

    BENCHMARK("Quatf compose SIMD (8 ops)")
    {
        f32 acc = 0.0F;
        for (int i = 0; i < 8; ++i)
        {
            const simd::Quatf c = sqa * sqb;
            acc += c.w();
        }
        return acc;
    };
}
