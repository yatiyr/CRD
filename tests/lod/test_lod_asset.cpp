// test_lod_asset.cpp — REN-40-C1: the `.crdlod` policy as an authored asset.

#include <crd/containers/string.hpp>
#include <crd/lod/lod_asset.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
constexpr const char* kGood = R"CRDLOD(
schema = 1
name   = "crd://lod/test"
boundary_weight = 750.0

[[level]]
ratio         = 0.5
screen_height = 512.0

[[level]]
ratio         = 0.25
screen_height = 128.0

[[level]]
ratio         = 0.08
screen_height = 40.0
)CRDLOD";
} // namespace

TEST_CASE("REN-40-C1 GATE: a .crdlod policy parses into the levels it declares", "[lod][ren40][asset]")
{
    crd::lod::LodPolicy p{};
    REQUIRE(crd::lod::parse_lod_toml(crd::containers::StringView(kGood), p) == crd::lod::LodCookError::Ok);
    CHECK(p.extra_levels == 3U);
    CHECK(p.boundary_weight == 750.0F);
    CHECK(p.ratio[0] == 0.5F);
    CHECK(p.ratio[2] == 0.08F);
    CHECK(p.screen_height[0] == 512.0F);
    CHECK(p.screen_height[2] == 40.0F);
    // ⛔ the coarsest level must stay selected all the way to zero height, or an
    // instance smaller than the last threshold has NO level at all
    CHECK(p.screen_height[3] == 0.0F);
}

// ⛔⛔ EVERY REJECTION IS A NAMED ONE. A policy whose ratios ascend, or whose
// thresholds do, does not "mostly work" — it makes selection ambiguous, and an
// ambiguous selector oscillates between levels at the boundary, which is the
// visible popping the whole band exists to remove.
TEST_CASE("REN-40-C1 GATE: an ambiguous or useless policy is REFUSED by name", "[lod][ren40][asset]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::containers::String    where(&alloc);
    crd::lod::LodPolicy        p{};

    SECTION("a ratio that does not reduce")
    {
        constexpr const char* t = "schema = 1\n[[level]]\nratio = 1.5\nscreen_height = 100.0\n";
        CHECK(crd::lod::parse_lod_toml(crd::containers::StringView(t), p, &where)
              == crd::lod::LodCookError::RatioOutOfRange);
    }
    SECTION("ratios that ascend")
    {
        constexpr const char* t = "schema = 1\n[[level]]\nratio = 0.2\nscreen_height = 200.0\n"
                                  "[[level]]\nratio = 0.6\nscreen_height = 100.0\n";
        CHECK(crd::lod::parse_lod_toml(crd::containers::StringView(t), p, &where)
              == crd::lod::LodCookError::RatiosNotDescending);
    }
    SECTION("thresholds that ascend")
    {
        constexpr const char* t = "schema = 1\n[[level]]\nratio = 0.6\nscreen_height = 100.0\n"
                                  "[[level]]\nratio = 0.2\nscreen_height = 200.0\n";
        CHECK(crd::lod::parse_lod_toml(crd::containers::StringView(t), p, &where)
              == crd::lod::LodCookError::ThresholdsNotDescending);
    }
    SECTION("no levels at all")
    {
        CHECK(crd::lod::parse_lod_toml(crd::containers::StringView("schema = 1\n"), p, &where)
              == crd::lod::LodCookError::NoLevels);
    }
    SECTION("the wrong schema")
    {
        CHECK(crd::lod::parse_lod_toml(crd::containers::StringView("schema = 2\n"), p, &where)
              == crd::lod::LodCookError::BadSchema);
    }
}

// ⭐ The canonical form must be a FIXED POINT — parse, write, parse gives the same
// policy — because that round-trip is what makes the identity hash a hash of the
// MEANING rather than of the author's whitespace.
TEST_CASE("REN-40-C1 GATE: the canonical form round-trips and the identity hash follows the meaning",
          "[lod][ren40][asset]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::lod::LodPolicy        a{};
    REQUIRE(crd::lod::parse_lod_toml(crd::containers::StringView(kGood), a) == crd::lod::LodCookError::Ok);

    crd::containers::String canon(&alloc);
    crd::lod::write_lod_toml(a, canon);
    crd::lod::LodPolicy b{};
    REQUIRE(crd::lod::parse_lod_toml(crd::containers::StringView(canon.c_str(), canon.size()), b)
            == crd::lod::LodCookError::Ok);
    CHECK(crd::lod::lod_policy_identity(a) == crd::lod::lod_policy_identity(b));

    // ⛔ and every field that changes the cooked chain must MOVE the hash — a field
    // left out of the identity collides two different chains onto one cache entry,
    // and the wrong one ships with the wrong switch distances baked in.
    const crd::u64 base = crd::lod::lod_policy_identity(a);
    {
        crd::lod::LodPolicy m = a;
        m.ratio[1] += 0.01F;
        CHECK(crd::lod::lod_policy_identity(m) != base);
    }
    {
        crd::lod::LodPolicy m = a;
        m.screen_height[1] += 1.0F;
        CHECK(crd::lod::lod_policy_identity(m) != base);
    }
    {
        crd::lod::LodPolicy m = a;
        m.boundary_weight += 1.0F;
        CHECK(crd::lod::lod_policy_identity(m) != base);
    }
    {
        // the LAST threshold decides where the coarsest level takes over
        crd::lod::LodPolicy m       = a;
        m.screen_height[a.extra_levels] = 4.0F;
        CHECK(crd::lod::lod_policy_identity(m) != base);
    }
    {
        crd::lod::LodPolicy m = a;
        m.extra_levels        = 2U;
        CHECK(crd::lod::lod_policy_identity(m) != base);
    }
}

// ── ⭐⭐ REN-40-C3 GATE: the PER-VIEW BIAS parses, survives the canonical round-trip, and moves the identity. ──
// ⛔ All three matter, for different reasons. Parsing is the feature; SURVIVAL is the field-survival scar (a field
// dropped by BOTH the writer and the reader round-trips "byte-identically" and is silently lost); and the
// IDENTITY is the cache — two policies that select different levels per view must not collide onto one entry.
TEST_CASE("REN-40-C3 GATE: the per-view LOD bias parses, round-trips and moves the policy identity",
          "[lod][ren40][asset]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::containers::String    text(&alloc);
    text.append("schema = 1\nview_bias = [1.0, 0.7, 0.45, 0.25, 0.12]\n");
    text.append("[[level]]\nratio = 0.5\nscreen_height = 512.0\n");
    text.append("[[level]]\nratio = 0.25\nscreen_height = 128.0\n");

    crd::lod::LodPolicy     p{};
    crd::containers::String where(&alloc);
    REQUIRE(crd::lod::parse_lod_toml(crd::containers::StringView(text.c_str()), p, &where)
            == crd::lod::LodCookError::Ok);
    CHECK(p.view_bias[0] == 1.0F);
    CHECK(p.view_bias[1] == 0.7F);
    CHECK(p.view_bias[4] == 0.12F);
    // ⛔ an UNSTATED view is 1.0, never 0 — a zero bias drives the projected height to zero and pins that whole
    // view to the coarsest level, which reads as broken shadows rather than as a policy that forgot an entry.
    CHECK(p.view_bias[5] == 1.0F);

    // it SURVIVES the canonical form
    crd::containers::String canon(&alloc);
    crd::lod::write_lod_toml(p, canon);
    crd::lod::LodPolicy back{};
    REQUIRE(crd::lod::parse_lod_toml(crd::containers::StringView(canon.c_str()), back, &where)
            == crd::lod::LodCookError::Ok);
    for (crd::u32 v = 0; v < crd::lod::kMaxLodLevels; ++v)
    {
        INFO("view " << v);
        CHECK(back.view_bias[v] == p.view_bias[v]);
    }

    // ...and it MOVES the identity
    crd::lod::LodPolicy other = p;
    other.view_bias[3]        = 0.5F;
    CHECK(crd::lod::lod_policy_identity(other) != crd::lod::lod_policy_identity(p));

    // ⛔ a NON-POSITIVE bias is refused back to 1.0 rather than pinning the view
    crd::containers::String bad(&alloc);
    bad.append("schema = 1\nview_bias = [1.0, 0.0, -2.0]\n");
    bad.append("[[level]]\nratio = 0.5\nscreen_height = 512.0\n");
    crd::lod::LodPolicy bp{};
    REQUIRE(crd::lod::parse_lod_toml(crd::containers::StringView(bad.c_str()), bp, &where)
            == crd::lod::LodCookError::Ok);
    CHECK(bp.view_bias[1] == 1.0F);
    CHECK(bp.view_bias[2] == 1.0F);
}
