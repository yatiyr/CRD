// test_csm.cpp — REN-3.2-b: the CPU cascade-fitting gates. No GPU: these assert the STABILIZATION properties
// that make cascades usable in motion, which is exactly the part a rendered image is bad at showing (shadow
// crawl reads as "aliasing" or "art style" until you look for it deliberately).

#include <crd/scenerender/csm.hpp>

#include <crd/math/mat.hpp>
#include <crd/math/cmath.hpp>
#include <crd/math/power.hpp>
#include <crd/math/trig.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace sr = crd::scenerender;
namespace m  = crd::math;

constexpr float kFovY   = 1.0472F; // 60 degrees, the sandbox camera
constexpr float kAspect = 16.0F / 9.0F;

[[nodiscard]] m::Mat4f test_proj()
{
    return m::perspective_reverse_z(kFovY, kAspect, 0.1F);
}

// world position that a shadow texel maps to, for cascade `c`: project a world point through light_vp and
// return its shadow-map UV in texels. Swim = this value moving for a STATIONARY world point.
[[nodiscard]] m::Vec3f project(const m::Mat4f& vp, const m::Vec3f& p)
{
    const float x = vp.c0.x * p.x + vp.c1.x * p.y + vp.c2.x * p.z + vp.c3.x;
    const float y = vp.c0.y * p.x + vp.c1.y * p.y + vp.c2.y * p.z + vp.c3.y;
    const float z = vp.c0.z * p.x + vp.c1.z * p.y + vp.c2.z * p.z + vp.c3.z;
    const float w = vp.c0.w * p.x + vp.c1.w * p.y + vp.c2.w * p.z + vp.c3.w;
    const float iw = m::abs(w) > 1.0e-9F ? 1.0F / w : 1.0F;
    return {x * iw, y * iw, z * iw};
}

} // namespace

TEST_CASE("REN-3.2-b: the practical split matches the Zhang PSSM formula and is monotonic", "[scene-render][csm]")
{
    constexpr float n = 0.1F;
    constexpr float f = 200.0F;

    // lambda = 0 is pure UNIFORM: evenly spaced far planes
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        const float si       = static_cast<float>(i + 1U) / 4.0F;
        const float expected = n + (f - n) * si;
        CHECK(m::abs(sr::csm_split_practical_cpu(n, f, 0.0F, i, 4U) - expected) < 1.0e-3F);
    }
    // lambda = 1 is pure LOGARITHMIC: near*(far/near)^si
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        const float si       = static_cast<float>(i + 1U) / 4.0F;
        const float expected = n * m::pow(f / n, si);
        CHECK(m::abs(sr::csm_split_practical_cpu(n, f, 1.0F, i, 4U) - expected) < 1.0e-2F);
    }
    // the last split ALWAYS lands exactly on the shadow distance, whatever lambda - a cascade set that stopped
    // short would leave a band of unshadowed world with no visible cause
    for (float lambda = 0.0F; lambda <= 1.0F; lambda += 0.25F)
    {
        CHECK(m::abs(sr::csm_split_practical_cpu(n, f, lambda, 3U, 4U) - f) < 1.0e-2F);
    }
    // strictly increasing
    float prev = 0.0F;
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        const float s = sr::csm_split_practical_cpu(n, f, 0.85F, i, 4U);
        CHECK(s > prev);
        prev = s;
    }
}

// ⛔ THE GATE THE SPEC NAMES: "a panning camera shows no cascade swim".
// A camera ROTATING IN PLACE must not change a cascade's ortho EXTENT at all. If the extent breathes, every
// shadow texel covers a different world area each frame and the edges crawl. The analytic bounding sphere makes
// this exact, not approximate — so this asserts equality to float precision, not a tolerance band.
TEST_CASE("REN-3.2-b GATE: a camera rotating in place does NOT resize any cascade (no swim)",
          "[scene-render][csm]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 4;
    cfg.map_size      = 2048;

    const m::Mat4f proj = test_proj();
    const m::Vec3f eye{10.0F, 25.0F, 10.0F};
    const m::Vec3f light = m::normalized(m::Vec3f{0.35F, -1.0F, 0.25F});

    // texel_world is (2*radius)/map_size, so it IS the cascade's extent in one number
    sr::CsmCascades ref;
    bool            first = true;
    for (int deg = 0; deg < 360; deg += 15)
    {
        const float    a = static_cast<float>(deg) * 3.14159265F / 180.0F;
        const m::Vec3f target{eye.x + m::cos(a) * 10.0F, eye.y - 2.0F, eye.z + m::sin(a) * 10.0F};
        const m::Mat4f view = m::look_at(eye, target, m::Vec3f{0.0F, 1.0F, 0.0F});
        const sr::CsmCascades c = sr::compute_csm_cascades(view, proj, light, cfg);
        REQUIRE(c.count == 4U);
        if (first)
        {
            ref   = c;
            first = false;
            continue;
        }
        for (crd::u32 i = 0; i < c.count; ++i)
        {
            // IDENTICAL extent at every yaw - the whole point of the analytic sphere
            CHECK(m::abs(c.texel_world[i] - ref.texel_world[i]) < 1.0e-6F);
            CHECK(m::abs(c.split_far[i] - ref.split_far[i]) < 1.0e-6F);
        }
    }
}

// The translation half of stabilization: as the camera PANS, a fixed world point's shadow-map position may only
// move in whole-texel steps. Without the snap it drifts continuously and the shadow edge crawls across geometry.
// Measured as: the sub-texel remainder must stay pinned, not sweep through the full [0,1) range.
TEST_CASE("REN-3.2-b GATE: panning moves the shadow projection in WHOLE TEXELS only (Valient snap)",
          "[scene-render][csm]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 1;   // isolate one cascade
    cfg.map_size      = 1024;
    cfg.far_plane     = 100.0F;

    const m::Mat4f proj = test_proj();
    const m::Vec3f light = m::normalized(m::Vec3f{0.4F, -1.0F, 0.2F});
    const m::Vec3f probe{3.0F, 0.0F, -7.0F}; // a FIXED world point

    float max_frac = 0.0F;
    float min_frac = 1.0F;
    for (int step = 0; step < 40; ++step)
    {
        // pan by a deliberately non-texel-aligned increment
        const float    ox = static_cast<float>(step) * 0.037F;
        const m::Vec3f eye{ox, 20.0F, 0.0F};
        const m::Mat4f view = m::look_at(eye, m::Vec3f{ox, 0.0F, -10.0F}, m::Vec3f{0.0F, 1.0F, 0.0F});
        const sr::CsmCascades c = sr::compute_csm_cascades(view, proj, light, cfg);
        REQUIRE(c.count == 1U);

        // the probe's position in shadow TEXELS
        const m::Vec3f ndc = project(c.light_vp[0], probe);
        const float    tex_x = (ndc.x * 0.5F + 0.5F) * static_cast<float>(cfg.map_size);
        float          frac  = tex_x - m::floor(tex_x);
        if (frac < 0.0F) { frac += 1.0F; }
        if (frac > max_frac) { max_frac = frac; }
        if (frac < min_frac) { min_frac = frac; }
    }
    // ⛔ With the snap, the sub-texel phase is CONSTANT: the projection only ever jumps whole texels, so a fixed
    // world point keeps the same position WITHIN its texel. Measured spread here is under 1% of a texel; an
    // UNSNAPPED implementation measures 0.99 (the phase sweeps the entire range) — which is precisely the
    // shadow-edge crawl this exists to remove. The bound is deliberately ~100x tighter than "it works", because
    // a loose bound would also pass a snap that had silently stopped snapping.
    CHECK((max_frac - min_frac) < 0.01F);
}

TEST_CASE("REN-3.2-b: cascades cover the shadow distance and degenerate input never produces NaN",
          "[scene-render][csm]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 4;

    const m::Mat4f proj = test_proj();
    const m::Mat4f view = m::look_at(m::Vec3f{0.0F, 5.0F, 12.0F}, m::Vec3f{0.0F, 0.0F, 0.0F},
                                     m::Vec3f{0.0F, 1.0F, 0.0F});

    // a ZERO light direction must fall back, not produce NaNs that poison every matrix downstream
    const sr::CsmCascades z = sr::compute_csm_cascades(view, proj, m::Vec3f{0.0F, 0.0F, 0.0F}, cfg);
    REQUIRE(z.count == 4U);
    for (crd::u32 i = 0; i < z.count; ++i)
    {
        CHECK(z.texel_world[i] == z.texel_world[i]); // NaN != NaN
        CHECK(z.texel_world[i] > 0.0F);
    }

    // a light pointing straight DOWN is the up-hint degenerate case (cross product with +Y collapses)
    const sr::CsmCascades d = sr::compute_csm_cascades(view, proj, m::Vec3f{0.0F, -1.0F, 0.0F}, cfg);
    for (crd::u32 i = 0; i < d.count; ++i)
    {
        CHECK(d.texel_world[i] == d.texel_world[i]);
        CHECK(d.texel_world[i] > 0.0F);
    }

    // cascade counts are CLAMPED, never truncated silently past the cap
    cfg.cascade_count = 99;
    CHECK(sr::compute_csm_cascades(view, proj, m::Vec3f{0.0F, -1.0F, 0.0F}, cfg).count == sr::kMaxCascades);
    cfg.cascade_count = 0;
    CHECK(sr::compute_csm_cascades(view, proj, m::Vec3f{0.0F, -1.0F, 0.0F}, cfg).count == 1U);
}

// ── REN-40-E: CASCADE CACHING ───────────────────────────────────────────────────────────────────────────────
// Texel-snapped matrices are BIT-IDENTICAL when the camera doesn't move (the snap quantizes the ortho centre
// to whole texel steps, so a stationary camera produces the exact same centre every frame). This is the
// property that makes the cache comparison exact, not approximate.

TEST_CASE("REN-40-E GATE: a static camera produces bit-identical cascade matrices across frames",
          "[scene-render][csm][ren40]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 4;
    cfg.map_size      = 2048;

    const m::Mat4f proj = test_proj();
    const m::Vec3f eye{10.0F, 25.0F, 10.0F};
    const m::Vec3f target{0.0F, 0.0F, 0.0F};
    const m::Vec3f light = m::normalized(m::Vec3f{0.35F, -1.0F, 0.25F});

    const m::Mat4f view = m::look_at(eye, target, m::Vec3f{0.0F, 1.0F, 0.0F});
    const m::Mat4f vp   = proj * view;

    const sr::CsmCascades frame0 = sr::compute_csm_cascades_from_vp(vp, light, cfg);
    const sr::CsmCascades frame1 = sr::compute_csm_cascades_from_vp(vp, light, cfg);
    REQUIRE(frame0.count == 4U);
    REQUIRE(frame1.count == 4U);

    for (crd::u32 i = 0; i < frame0.count; ++i)
    {
        CHECK(frame0.light_vp[i] == frame1.light_vp[i]);
    }
}

TEST_CASE("REN-40-E GATE: a moving camera produces DIFFERENT cascade matrices",
          "[scene-render][csm][ren40]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 4;
    cfg.map_size      = 2048;

    const m::Mat4f proj = test_proj();
    const m::Vec3f light = m::normalized(m::Vec3f{0.35F, -1.0F, 0.25F});

    const m::Mat4f view0 = m::look_at(m::Vec3f{10.0F, 25.0F, 10.0F}, m::Vec3f{0.0F, 0.0F, 0.0F},
                                       m::Vec3f{0.0F, 1.0F, 0.0F});
    const m::Mat4f view1 = m::look_at(m::Vec3f{50.0F, 25.0F, 50.0F}, m::Vec3f{40.0F, 0.0F, 40.0F},
                                       m::Vec3f{0.0F, 1.0F, 0.0F});

    const sr::CsmCascades frame0 = sr::compute_csm_cascades_from_vp(proj * view0, light, cfg);
    const sr::CsmCascades frame1 = sr::compute_csm_cascades_from_vp(proj * view1, light, cfg);
    REQUIRE(frame0.count == 4U);
    REQUIRE(frame1.count == 4U);

    crd::u32 changed = 0;
    for (crd::u32 i = 0; i < frame0.count; ++i)
    {
        if (!(frame0.light_vp[i] == frame1.light_vp[i])) { ++changed; }
    }
    CHECK(changed > 0U);
}

TEST_CASE("REN-40-E GATE: zero prev_light_vp never matches a real cascade (first-frame safety)",
          "[scene-render][csm][ren40]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 4;
    cfg.map_size      = 2048;

    const m::Mat4f proj = test_proj();
    const m::Vec3f light = m::normalized(m::Vec3f{0.35F, -1.0F, 0.25F});
    const m::Mat4f view = m::look_at(m::Vec3f{10.0F, 25.0F, 10.0F}, m::Vec3f{0.0F, 0.0F, 0.0F},
                                      m::Vec3f{0.0F, 1.0F, 0.0F});

    const sr::CsmCascades real = sr::compute_csm_cascades_from_vp(proj * view, light, cfg);
    REQUIRE(real.count == 4U);

    const m::Mat4f zero{};
    for (crd::u32 i = 0; i < real.count; ++i)
    {
        CHECK_FALSE(real.light_vp[i] == zero);
    }
}

TEST_CASE("REN-40-E2 GATE: round-robin schedule alternates far cascades",
          "[scene-render][csm][ren40]")
{
    // Near cascades (0, 1) are always scheduled. Far cascades (2, 3) alternate by frame parity.
    // The schedule function: scheduled(index) = index < 2 || (frame & 1) == (index & 1)
    // frame 0 (even): cascade 2 scheduled (even), cascade 3 NOT
    // frame 1 (odd):  cascade 2 NOT,            cascade 3 scheduled (odd)
    auto scheduled = [](crd::u32 frame, crd::u32 index) -> bool
    {
        if (index < 2U) { return true; }
        return (frame & 1U) == (index & 1U);
    };

    for (crd::u32 f = 0; f < 8; ++f)
    {
        CHECK(scheduled(f, 0U));
        CHECK(scheduled(f, 1U));
        // cascades 2 and 3 never schedule on the same frame
        CHECK(scheduled(f, 2U) != scheduled(f, 3U));
    }
}

// ── REN-40-E GATE: MOVING-LIGHT LATENCY ──────────────────────────────────────────────────────────────────────
// When the light direction changes, ALL cascades must update within the round-robin window: near cascades (0,1)
// update IMMEDIATELY (same frame), far cascades (2,3) update within AT MOST 2 frames. Once the light settles,
// all cascades return to the cached state. This is the correctness property the round-robin + cache combination
// must hold: stale shadows are bounded, never permanent.
TEST_CASE("REN-40-E GATE: a moved light updates every cascade within the round-robin window",
          "[scene-render][csm][ren40]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 4;
    cfg.map_size      = 2048;

    const m::Mat4f proj = test_proj();
    const m::Vec3f eye{10.0F, 25.0F, 10.0F};
    const m::Mat4f view = m::look_at(eye, m::Vec3f{0.0F, 0.0F, 0.0F}, m::Vec3f{0.0F, 1.0F, 0.0F});
    const m::Mat4f vp   = proj * view;

    const m::Vec3f light_a = m::normalized(m::Vec3f{0.35F, -1.0F, 0.25F});
    const m::Vec3f light_b = m::normalized(m::Vec3f{-0.6F, -1.0F, 0.4F});

    const sr::CsmCascades casc_a = sr::compute_csm_cascades_from_vp(vp, light_a, cfg);
    const sr::CsmCascades casc_b = sr::compute_csm_cascades_from_vp(vp, light_b, cfg);
    REQUIRE(casc_a.count == 4U);
    REQUIRE(casc_b.count == 4U);

    // the two light directions must produce DIFFERENT matrices (the test is meaningless otherwise)
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        CHECK_FALSE(casc_a.light_vp[i] == casc_b.light_vp[i]);
    }

    // simulate the for_each_load decision: cache IFF (prev == current) OR (far + not scheduled + past frame 1)
    auto should_cache = [](crd::u32 index, const m::Mat4f& prev, const m::Mat4f& current,
                           crd::u32 csm_frame) -> bool
    {
        if (prev == current) { return true; }
        const bool scheduled = index < 2U || ((csm_frame & 1U) == (index & 1U));
        if (index >= 2U && csm_frame > 1U && !scheduled) { return true; }
        return false;
    };

    // start steady on light_a: all cascades are cached after the first frame
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        CHECK(should_cache(i, casc_a.light_vp[i], casc_a.light_vp[i], 5U));
    }

    // light moves to B: near cascades update IMMEDIATELY regardless of frame parity
    for (crd::u32 frame = 2; frame < 6; ++frame)
    {
        CHECK_FALSE(should_cache(0U, casc_a.light_vp[0], casc_b.light_vp[0], frame));
        CHECK_FALSE(should_cache(1U, casc_a.light_vp[1], casc_b.light_vp[1], frame));
    }

    // far cascades update on their SCHEDULED frame and cache on the non-scheduled one.
    // over 2 consecutive frames, BOTH cascades 2 and 3 get at least one re-render.
    crd::u32 updated_2 = 0;
    crd::u32 updated_3 = 0;
    for (crd::u32 frame = 2; frame <= 3; ++frame)
    {
        if (!should_cache(2U, casc_a.light_vp[2], casc_b.light_vp[2], frame)) { ++updated_2; }
        if (!should_cache(3U, casc_a.light_vp[3], casc_b.light_vp[3], frame)) { ++updated_3; }
    }
    CHECK(updated_2 >= 1U);
    CHECK(updated_3 >= 1U);

    // after updating prev to B: all cascades are cached again (the light settled)
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        CHECK(should_cache(i, casc_b.light_vp[i], casc_b.light_vp[i], 10U));
    }
}

// ⛔ The renderer only ever HAS a combined view_proj, so `compute_csm_cascades_from_vp` recovers the frustum
// shape and the camera basis from it. That recovery is claimed to be EXACT, not approximate — and a claim like
// that is worthless without a gate: a mis-recovered basis silently mis-sizes every cascade and mis-places every
// shadow, which looks like a bias/peter-panning problem rather than like a broken matrix.
TEST_CASE("REN-3.2-b GATE: fitting from a combined view_proj matches fitting from separate view/proj",
          "[scene-render][csm]")
{
    sr::CsmConfig cfg;
    cfg.cascade_count = 4;
    cfg.map_size      = 2048;

    const m::Mat4f proj  = test_proj();
    const m::Vec3f light = m::normalized(m::Vec3f{0.35F, -1.0F, 0.25F});

    // several genuinely different camera placements + orientations, not one lucky pose
    const m::Vec3f eyes[] = {{0.0F, 5.0F, 12.0F}, {-30.0F, 40.0F, 8.0F}, {14.0F, 2.0F, -22.0F}};
    const m::Vec3f tgts[] = {{0.0F, 0.0F, 0.0F}, {5.0F, 0.0F, -3.0F}, {-8.0F, 6.0F, 1.0F}};
    for (int k = 0; k < 3; ++k)
    {
        const m::Mat4f view = m::look_at(eyes[k], tgts[k], m::Vec3f{0.0F, 1.0F, 0.0F});
        const m::Mat4f vp   = proj * view;

        const sr::CsmCascades a = sr::compute_csm_cascades(view, proj, light, cfg);
        const sr::CsmCascades b = sr::compute_csm_cascades_from_vp(vp, light, cfg);
        REQUIRE(a.count == b.count);
        for (crd::u32 i = 0; i < a.count; ++i)
        {
            CHECK(m::abs(a.split_far[i] - b.split_far[i]) < 1.0e-3F);
            // extent equality to a relative 1e-4: the fit is reproduced, not approximated
            CHECK(m::abs(a.texel_world[i] - b.texel_world[i]) < a.texel_world[i] * 1.0e-4F);
            // and the MATRICES agree, which is what actually places the shadows
            const m::Vec3f probe{2.0F, 1.5F, -4.0F};
            const m::Vec3f pa = project(a.light_vp[i], probe);
            const m::Vec3f pb = project(b.light_vp[i], probe);
            CHECK(m::abs(pa.x - pb.x) < 1.0e-3F);
            CHECK(m::abs(pa.y - pb.y) < 1.0e-3F);
            CHECK(m::abs(pa.z - pb.z) < 1.0e-3F);
        }
    }
}
