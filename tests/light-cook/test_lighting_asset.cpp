// test_lighting_asset.cpp — REN-38-E1..E6 (D-007 row 141): THE LIGHTING VOCABULARY AS AN AUTHORED ASSET.
//
// ⛔⛔ WHAT THIS REPLACES. `ckir_lighting.hpp` is 1100 lines of gold-standard shading — Filament punctual
// attenuation, Heitz LTC rect/line/disk area lights, Karis split-sum IBL, SH-L2 irradiance,
// PCF/PCSS/EVSM/Moment shadows, CSM, contact shadows — and the technique ABI carried EXACTLY ONE DIRECTIONAL
// LIGHT (`kTiLightDir` + `kTiLightColor`). Every one of those functions was unreachable from any asset.

#include <crd/lightcook/lighting_asset.hpp>

#include <crd/kir/ckir_serialize.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd;
namespace lc = crd::lightcook;

namespace
{
// A record wide enough for every declared type — punctual fields, the four area-light corners, a radius, a
// per-light shadow projection, a cube range and an IES row.
constexpr const char* kRecord = R"(
[record]
stride       = 64
position     = 0
color        = 4
direction    = 8
falloff      = 3
spot_scale   = 7
spot_offset  = 11
p0           = 12
p1           = 15
p2           = 18
p3           = 21
radius       = 24
shadow_index = 25
shadow_vp    = 26
shadow_range = 42
ies_index    = 43
)";

struct Rig
{
    kir::KGraph          g;
    lc::LightingInputs   in{};
    lc::LightingBindings b{};

    explicit Rig(memory::IAllocator* a, bool comparison_atlas = true) : g(a)
    {
        const auto sh = kir::make_shape({1});
        const auto kf = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
        in.base_color = g.vec3(kf(0.5), kf(0.5), kf(0.5));
        in.metallic   = kf(0.0);
        in.roughness  = kf(0.5);
        in.normal     = g.vec3(kf(0.0), kf(0.0), kf(1.0));
        in.view_dir   = g.vec3(kf(0.0), kf(0.0), kf(1.0));
        in.world_pos  = g.vec3(kf(0.0), kf(0.0), kf(0.0));
        in.emissive   = g.vec3(kf(0.0), kf(0.0), kf(0.0));
        in.frag_xy    = g.vec2(kf(0.5), kf(0.5));
        // every binding a declaration might require, so a cook failure means the DECLARATION was refused.
        // ⛔ The binding TYPES are part of the contract — the same flags the GLSL emitter keys the combined
        // sampler type on, now enforced by the shape checker. One plain 2-D texture wired into EVERY slot used
        // to cook here, and that mistyping HID a real defect (the PCSS blocker search through the comparison
        // sampler). The atlas is LAYERED; comparison vs moments follows `lighting_shadow_is_comparison`.
        const int atlas = g.texture(0, 0, kir::DType::F32, kir::TexDim::Tex2D, true, false, comparison_atlas);
        b.shadow_atlas         = atlas;
        b.shadow_sampler       = g.sampler(0, 1, comparison_atlas);
        b.shadow_plain_sampler = g.sampler(0, 2, false);
        const int i0 = g.vec4(kf(1.0), kf(0.0), kf(0.0), kf(0.0));
        const int i1 = g.vec4(kf(0.0), kf(1.0), kf(0.0), kf(0.0));
        const int i2 = g.vec4(kf(0.0), kf(0.0), kf(1.0), kf(0.0));
        const int i3 = g.vec4(kf(0.0), kf(0.0), kf(0.0), kf(1.0));
        const int id = g.mat4(i0, i1, i2, i3);
        for (int& v : b.csm_light_vp) { v = id; }
        b.csm_map_size = kf(2048.0);
        for (int& s : b.sh) { s = g.vec3(kf(0.1), kf(0.1), kf(0.1)); }
        const int tex2d = g.texture(0, 3);
        const int samp  = g.sampler(0, 4);
        b.prefiltered   = g.texture(0, 5, kir::DType::F32, kir::TexDim::TexCube); // sampled by DIRECTION
        b.env_sampler   = samp;
        b.ltc_lut       = tex2d;
        b.ltc_sampler   = samp;
        b.ies_atlas     = tex2d;
        b.ies_sampler   = samp;
        b.depth_tex     = tex2d;
        b.depth_sampler = samp;
        b.decal_atlas   = g.texture(0, 6, kir::DType::F32, kir::TexDim::Tex2D, true); // one LAYER per decal
        b.decal_sampler = samp;
        b.cluster_grid  = kf(0.5);
    }
};

// Build a declaration from the shared record plus whatever sections the case needs.
[[nodiscard]] containers::String make(memory::IAllocator* a, const char* sections)
{
    containers::String t(a);
    // ⭐ CEIR-18a-2: viewport_w/h are in the shared header so any clustered section a test appends cooks (the cluster
    // cook REQUIRES them). ⭐ CEIR-18b: slice_bounds_off likewise, so a 3D grid (grid[2] > 1) cooks — the cluster cook
    // REQUIRES it too. Non-clustered sections ignore all three; a 2D grid (grid[2] == 1) ignores slice_bounds_off (never
    // hashed or read there — the 18a byte-stability contract holds).
    t.append("schema = 1\nname   = \"crd://lighting/gate\"\n\n[header]\nview_proj = 6\ncsm_splits = 28\n"
             "light_off = 23\ndecal_off = 24\ncluster_off = 21\nviewport_w = 111\nviewport_h = 112\nslice_bounds_off = 115\n");
    t.append(kRecord);
    t.append(sections);
    return t;
}

[[nodiscard]] lc::LightingCookError parse(memory::IAllocator* a, const char* sections, lc::LightingDesc& out)
{
    containers::String t     = make(a, sections);
    containers::String where(a);
    return lc::parse_lighting_toml(containers::StringView(t.c_str(), t.size()), out, &where);
}

// Cook and return the graph's live size, or -1 when the cook refuses.
[[nodiscard]] int cook_size(memory::IAllocator* a, const char* sections)
{
    lc::LightingDesc d(a);
    if (parse(a, sections, d) != lc::LightingCookError::Ok) { return -1; }
    // The atlas TYPE follows the declared filter — exactly how a live caller builds its bindings from the asset.
    Rig       r(a, lc::lighting_shadow_is_comparison(d));
    const int before = r.g.size();
    const int lit    = lc::cook_lighting(d, r.g, r.in, r.b);
    if (lit < 0) { return -1; }
    return r.g.size() - before;
}
} // namespace

TEST_CASE("REN-38-E1: a LIGHT ARRAY, not one hardcoded directional light", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    // ⛔⛔ THE ENTIRE ABI WAS ONE DIRECTIONAL LIGHT. Every extra light here is a lobe that could not be reached
    // from any asset, at any count, before this row.
    const int one   = cook_size(&alloc, "\n[counts]\ndirectional = 1\n");
    const int four  = cook_size(&alloc, "\n[counts]\ndirectional = 4\n");
    const int mixed = cook_size(&alloc, "\n[counts]\ndirectional = 1\npoint = 3\nspot = 2\n");
    CHECK(one > 0);
    CHECK(four > one);
    CHECK(mixed > one);

    // ⭐ THE BUFFER IS TYPE-SORTED and the cook knows where each run starts — which is what lets it unroll a type
    // without a per-light branch. ⛔ A wrong first index reads another type's records: a point light evaluated
    // with a spotlight's cone words, in a scene that still renders.
    lc::LightingDesc d(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\ndirectional = 2\npoint = 3\nspot = 4\nrect = 1\n", d)
            == lc::LightingCookError::Ok);
    CHECK(lc::lighting_total_lights(d) == 10U);
    CHECK(lc::lighting_type_first(d, lc::LightType::Directional) == 0U);
    CHECK(lc::lighting_type_first(d, lc::LightType::Point) == 2U);
    CHECK(lc::lighting_type_first(d, lc::LightType::Spot) == 5U);
    CHECK(lc::lighting_type_first(d, lc::LightType::Rect) == 9U);

    // ⛔ A LIGHTING TECHNIQUE THAT LIGHTS NOTHING is a black screen with no error anywhere to explain it.
    lc::LightingDesc empty(&alloc);
    CHECK(parse(&alloc, "\n[counts]\ndirectional = 0\n", empty) == lc::LightingCookError::NoLights);
    // ⛔ …and an unrolled loop is CODE SIZE, so the cap is real.
    lc::LightingDesc huge(&alloc);
    CHECK(parse(&alloc, "\n[counts]\npoint = 99\n", huge) == lc::LightingCookError::TooManyLights);
}

TEST_CASE("REN-38-E1: a light field past the record stride is REFUSED", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    containers::String    t(&alloc);
    // ⛔ A field past the stride pulls the NEXT LIGHT's words — a spotlight taking its neighbour's cone, in a
    // scene that renders. Nothing downstream can catch it: the buffer is a flat u32 array.
    t.append("schema = 1\nname = \"m\"\n[record]\nstride = 8\nposition = 0\ncolor = 4\nfalloff = 3\n"
             "spot_scale = 7\nspot_offset = 20\n[counts]\nspot = 1\n");
    lc::LightingDesc   d(&alloc);
    containers::String w(&alloc);
    CHECK(lc::parse_lighting_toml(containers::StringView(t.c_str(), t.size()), d, &w)
          == lc::LightingCookError::FieldOutOfRecord);

    // ⛔ AN AREA LIGHT NEEDS A SHAPE. Without the corner fields the LTC solve integrates over adjacent lights'
    // words — an area light illuminating from a polygon that does not exist.
    containers::String t2(&alloc);
    t2.append("schema = 1\nname = \"m\"\n[record]\nstride = 8\nposition = 0\ncolor = 4\n[counts]\nrect = 1\n");
    lc::LightingDesc d2(&alloc);
    CHECK(lc::parse_lighting_toml(containers::StringView(t2.c_str(), t2.size()), d2, &w)
          == lc::LightingCookError::MissingField);
}

TEST_CASE("REN-38-E2: every light type is reachable", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(128U << 20U);
    // ⭐ Point · spot · rect · tube · disk — Heitz LTC for the three area forms, Filament punctual for the rest.
    // Each was built at B8-c/B8-d and NONE was reachable from an asset.
    const int base = cook_size(&alloc, "\n[counts]\ndirectional = 1\n");
    REQUIRE(base > 0);
    const char* one_of[5] = {"\n[counts]\npoint = 1\n", "\n[counts]\nspot = 1\n", "\n[counts]\nrect = 1\n",
                             "\n[counts]\ntube = 1\n", "\n[counts]\ndisk = 1\n"};
    int         sizes[5]  = {0, 0, 0, 0, 0};
    for (int i = 0; i < 5; ++i)
    {
        sizes[i] = cook_size(&alloc, one_of[i]);
        INFO(one_of[i]);
        CHECK(sizes[i] > 0);
    }
    // ⛔ EACH TYPE IS A DIFFERENT AMOUNT OF MATH, so no two collapse to the same program. If two did, one of them
    // was silently cooked as the other — which is the failure a "did it cook?" check cannot see.
    for (int i = 0; i < 5; ++i)
    {
        for (int k = i + 1; k < 5; ++k)
        {
            INFO(one_of[i] << " vs " << one_of[k]);
            CHECK(sizes[i] != sizes[k]);
        }
    }
    // ⭐ An area light needs the LTC LUT a punctual one does not — the declaration says so, so a caller builds
    // its binding list from the asset rather than from a duplicated constant.
    lc::LightingDesc pt(&alloc);
    lc::LightingDesc rc(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\npoint = 1\n", pt) == lc::LightingCookError::Ok);
    REQUIRE(parse(&alloc, "\n[counts]\nrect = 1\n", rc) == lc::LightingCookError::Ok);
    CHECK_FALSE(lc::lighting_needs_ltc(pt));
    CHECK(lc::lighting_needs_ltc(rc));

    // ⛔ A MISSING BINDING FAILS THE COOK rather than dropping the term. An area light that silently degraded to
    // "no light" is a scene that renders, and nobody looks for a bug in a scene that renders.
    {
        Rig r(&alloc);
        r.b.ltc_lut = -1;
        CHECK(lc::cook_lighting(rc, r.g, r.in, r.b) < 0);
    }
}

TEST_CASE("REN-38-E2: IES PROFILES modulate a luminaire", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    // ⭐ A real luminaire is not a cone: its intensity varies with angle. The profile is what makes a wall washer
    // look like a wall washer rather than a spotlight aimed at a wall.
    //
    // ⛔ THE BASELINE IS A RECORD WITHOUT `ies_index`, not the shared one — which already declares it. Comparing
    // against a record that carries the profile would have compared the feature with itself, and the check would
    // have passed with the profile lookup absent. (It did, on the first attempt.)
    const char* base_toml = "schema = 1\nname = \"ies\"\n[record]\nstride = 64\nposition = 0\ncolor = 4\n"
                        "direction = 8\nfalloff = 3\nspot_scale = 7\nspot_offset = 11\n";
    const auto  cook  = [&](const char* toml) {
        lc::LightingDesc   d(&alloc);
        containers::String w(&alloc);
        REQUIRE(lc::parse_lighting_toml(containers::StringView(toml), d, &w) == lc::LightingCookError::Ok);
        Rig       r(&alloc);
        const int before = r.g.size();
        REQUIRE(lc::cook_lighting(d, r.g, r.in, r.b) >= 0);
        return r.g.size() - before;
    };
    containers::String plain_t(&alloc);
    plain_t.append(base_toml);
    plain_t.append("[counts]\nspot = 1\n");
    containers::String ies_t(&alloc);
    ies_t.append(base_toml);
    ies_t.append("ies_index = 43\n[counts]\nspot = 1\n");

    const int plain = cook(plain_t.c_str());
    const int withp = cook(ies_t.c_str());
    CHECK(plain > 0);
    CHECK(withp > plain); // the profile lookup is real work, not a declaration that does nothing

    lc::LightingDesc   d(&alloc);
    containers::String w(&alloc);
    REQUIRE(lc::parse_lighting_toml(containers::StringView(ies_t.c_str(), ies_t.size()), d, &w)
            == lc::LightingCookError::Ok);
    CHECK(lc::lighting_needs_ies(d));
    // ⛔ and the profile atlas is REQUIRED once declared
    Rig r2(&alloc);
    r2.b.ies_atlas = -1;
    CHECK(lc::cook_lighting(d, r2.g, r2.in, r2.b) < 0);
}

TEST_CASE("REN-38-E3: LIGHT PROBES + IBL become reachable", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    // ⛔ SH-L2 irradiance and the Karis split-sum specular have existed since B8-e with NO BINDING TYPE able to
    // reach them: a scene could not have ambient light from an environment at all.
    const int none = cook_size(&alloc, "\n[counts]\ndirectional = 1\n");
    const int diff = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[ibl]\ndiffuse = true\n");
    const int spec = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[ibl]\nspecular = true\n");
    const int both = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[ibl]\ndiffuse = true\nspecular = true\n");
    CHECK(none > 0);
    CHECK(diff > none);
    CHECK(spec > none);
    CHECK(both > diff);
    CHECK(both > spec);

    // ⭐ IBL ALONE IS A VALID TECHNIQUE — a probe-lit scene with no punctual lights is not "no lights".
    CHECK(cook_size(&alloc, "\n[counts]\ndirectional = 0\n\n[ibl]\ndiffuse = true\n") > 0);

    // ⛔ ALL NINE SH coefficients are required. Eight of nine cooks a band-2 term against a garbage node — an
    // ambient that is subtly the wrong colour, which reads as a grading choice.
    lc::LightingDesc d(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\ndirectional = 1\n\n[ibl]\ndiffuse = true\n", d)
            == lc::LightingCookError::Ok);
    Rig r(&alloc);
    r.b.sh[8] = -1;
    CHECK(lc::cook_lighting(d, r.g, r.in, r.b) < 0);
}

TEST_CASE("REN-38-E6: the shadow vocabulary beyond CSM", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(256U << 20U);
    // ⭐ CSM for directional, a projected MAP for spot, a radial CUBE compare for point — and PCF / PCSS / EVSM /
    // Moment filters, all built at B8-f/g/h and all unreachable.
    const int no_shadow = cook_size(&alloc, "\n[counts]\ndirectional = 1\n");
    const int csm       = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\n"
                                            "filter = \"pcf\"\ntaps = 4\ncascades = 4\n");
    const int spot_map  = cook_size(&alloc, "\n[counts]\nspot = 1\n\n[shadow]\nspot = \"map\"\nfilter = \"pcf\"\n"
                                            "taps = 4\n");
    const int point_cube = cook_size(&alloc, "\n[counts]\npoint = 1\n\n[shadow]\npoint = \"cube\"\n"
                                             "filter = \"pcf\"\ntaps = 4\n");
    CHECK(no_shadow > 0);
    CHECK(csm > no_shadow);
    CHECK(spot_map > 0);
    CHECK(point_cube > 0);

    // ⛔⛔ EVERY FILTER IS DIFFERENT MATH, and none of them can be told apart by "did it cook". EVSM and MSM read
    // MOMENTS from a filterable colour texture; Hard/PCF/PCSS sample a COMPARISON texture where the sample IS the
    // test. Binding one where the other belongs renders wrongly without erroring on either backend.
    // ⛔ Distinctness is judged by CONTENT (the canonical serialized graph), never by node count — PCF and MSM
    // once collided on count by pure coincidence, which is a false alarm a size proxy cannot avoid; and two
    // programs of EQUAL size with different math are exactly what the claim must still tell apart.
    const char*                     filters[5] = {"hard", "pcf", "pcss", "evsm", "msm"};
    containers::Array<crd::u8>      fbytes[5]  = {containers::Array<crd::u8>(&alloc), containers::Array<crd::u8>(&alloc),
                                                  containers::Array<crd::u8>(&alloc), containers::Array<crd::u8>(&alloc),
                                                  containers::Array<crd::u8>(&alloc)};
    for (int i = 0; i < 5; ++i)
    {
        containers::String s(&alloc);
        s.append("\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\ncascades = 2\ntaps = 4\n"
                 "filter = \"");
        s.append(filters[i]);
        s.append("\"\n");
        lc::LightingDesc fd(&alloc);
        REQUIRE(parse(&alloc, s.c_str(), fd) == lc::LightingCookError::Ok);
        Rig       fr(&alloc, lc::lighting_shadow_is_comparison(fd));
        const int lit = lc::cook_lighting(fd, fr.g, fr.in, fr.b);
        INFO(filters[i]);
        REQUIRE(lit >= 0);
        kir::KEntry fe;
        fe.stage  = kir::KStage::Fragment;
        fe.n_out  = 1;
        fe.out[0] = {fr.g.vec_concat(lit, fr.g.constant(1.0, kir::make_shape({1}), kir::DType::F32)), 0,
                     kir::Interp::Smooth};
        fbytes[i] = kir::serialize_graph(fr.g, fe, &alloc);
        CHECK(fbytes[i].size() > 0U);
    }
    for (int i = 0; i < 5; ++i)
    {
        for (int k = i + 1; k < 5; ++k)
        {
            INFO(filters[i] << " vs " << filters[k]);
            bool same = fbytes[i].size() == fbytes[k].size();
            if (same)
            {
                for (crd::usize x = 0; x < fbytes[i].size(); ++x)
                {
                    if (fbytes[i][x] != fbytes[k][x]) { same = false; break; }
                }
            }
            CHECK_FALSE(same);
        }
    }
    // ⭐ …and the declaration SAYS which kind of atlas it needs, so a caller cannot guess wrong.
    lc::LightingDesc pcf(&alloc);
    lc::LightingDesc evsm(&alloc);
    REQUIRE(parse(&alloc,
                  "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\nfilter = \"pcf\"\ntaps = 4\n",
                  pcf)
            == lc::LightingCookError::Ok);
    REQUIRE(parse(&alloc,
                  "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\nfilter = \"evsm\"\ntaps = 4\n",
                  evsm)
            == lc::LightingCookError::Ok);
    CHECK(lc::lighting_shadow_is_comparison(pcf));
    CHECK_FALSE(lc::lighting_shadow_is_comparison(evsm));

    // ⭐ CONTACT SHADOWS: a shadow map at any practical resolution loses the few-pixel darkening where an object
    // meets a surface, and its absence is exactly what makes objects look like they float.
    const int contact = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\n"
                                          "cascades = 2\nfilter = \"pcf\"\ntaps = 4\ncontact = true\n"
                                          "contact_steps = 4\n");
    CHECK(contact > csm);

    // ⛔⛔ A SCHEME THAT DOES NOT APPLY IS REFUSED. CSM is a DIRECTIONAL construction (it splits the view frustum
    // along the camera's depth); a cube shadow is a POINT one (six faces around a position). Accepting a mismatch
    // cooks a projection with no meaning for that light and darkens the scene semi-randomly.
    lc::LightingDesc bad(&alloc);
    CHECK(parse(&alloc, "\n[counts]\npoint = 1\n\n[shadow]\npoint = \"csm\"\n", bad)
          == lc::LightingCookError::BadShadowMode);
    CHECK(parse(&alloc, "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"cube\"\n", bad)
          == lc::LightingCookError::BadShadowMode);
    // ⛔ A tap count that is not 1/4/8/16 is not a filter kernel.
    CHECK(parse(&alloc, "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\ntaps = 7\n", bad)
          == lc::LightingCookError::BadFilter);
    CHECK(parse(&alloc, "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\ntaps = 4\ncascades = 9\n",
                bad)
          == lc::LightingCookError::BadCascades);
    // ⛔ A CUBE shadow compares a RADIAL DISTANCE, so it needs the light's far range as the denominator; without
    // it the compare is against an unnormalised metre value and everything is either fully lit or fully black.
    {
        containers::String s(&alloc);
        s.append("schema = 1\nname = \"m\"\n[record]\nstride = 32\nposition = 0\ncolor = 4\nfalloff = 3\n"
                 "shadow_index = 25\n[counts]\npoint = 1\n[shadow]\npoint = \"cube\"\ntaps = 4\n");
        lc::LightingDesc   dd(&alloc);
        containers::String w(&alloc);
        CHECK(lc::parse_lighting_toml(containers::StringView(s.c_str(), s.size()), dd, &w)
              == lc::LightingCookError::MissingField);
    }
    // ⛔ …and a projected spot map needs the light's OWN projection.
    {
        containers::String s(&alloc);
        s.append("schema = 1\nname = \"m\"\n[record]\nstride = 32\nposition = 0\ncolor = 4\ndirection = 8\n"
                 "falloff = 3\nspot_scale = 7\nspot_offset = 11\nshadow_index = 25\n[counts]\nspot = 1\n"
                 "[shadow]\nspot = \"map\"\ntaps = 4\n");
        lc::LightingDesc   dd(&alloc);
        containers::String w(&alloc);
        CHECK(lc::parse_lighting_toml(containers::StringView(s.c_str(), s.size()), dd, &w)
              == lc::LightingCookError::MissingField);
    }
}

TEST_CASE("REN-38-E6 GATE: N MIXED LIGHTS, EACH CASTING, with no engine change", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(256U << 20U);
    // ⭐⭐ THE ROW'S OWN GATE, verbatim: "N mixed lights, each casting, no engine change." One asset, three light
    // types, three DIFFERENT shadow schemes, IBL underneath — and not a line of C++ anywhere in it.
    const int mixed = cook_size(&alloc,
                                "\n[counts]\ndirectional = 1\npoint = 2\nspot = 2\nrect = 1\n"
                                "\n[shadow]\ndirectional = \"csm\"\npoint = \"cube\"\nspot = \"map\"\n"
                                "filter = \"pcf\"\ntaps = 8\ncascades = 3\ncontact = true\ncontact_steps = 4\n"
                                "\n[ibl]\ndiffuse = true\nspecular = true\n");
    CHECK(mixed > 0);
    // …and it is a bigger program than any one of its parts, so nothing collapsed away unnoticed.
    const int just_dir = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[shadow]\ndirectional = \"csm\"\n"
                                           "filter = \"pcf\"\ntaps = 8\ncascades = 3\n");
    CHECK(mixed > just_dir);
}

TEST_CASE("REN-38-E4: DECALS are a projected material", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    const int none = cook_size(&alloc, "\n[counts]\ndirectional = 1\n");
    const int one  = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[decal]\ncount = 1\nstride = 20\n"
                                       "projection = 0\ntint = 16\n");
    const int four = cook_size(&alloc, "\n[counts]\ndirectional = 1\n\n[decal]\ncount = 4\nstride = 20\n"
                                       "projection = 0\ntint = 16\n");
    CHECK(none > 0);
    CHECK(one > none);
    CHECK(four > one);
    // ⛔ A decal record that does not fit its stride projects by rows read from the NEXT decal.
    lc::LightingDesc bad(&alloc);
    CHECK(parse(&alloc, "\n[counts]\ndirectional = 1\n\n[decal]\ncount = 1\nstride = 8\nprojection = 0\n"
                        "tint = 16\n",
                bad)
          == lc::LightingCookError::BadDecal);
    // ⛔ …and the atlas is REQUIRED once decals are declared.
    lc::LightingDesc d(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\ndirectional = 1\n\n[decal]\ncount = 1\nstride = 20\nprojection = 0\n"
                          "tint = 16\n",
                  d)
            == lc::LightingCookError::Ok);
    Rig r(&alloc);
    r.b.decal_atlas = -1;
    CHECK(lc::cook_lighting(d, r.g, r.in, r.b) < 0);
}

TEST_CASE("CEIR-18b: the clustered 3D froxel z-slice is an authored boundary table", "[light-cook][ceir18b]")
{
    memory::TlsfAllocator alloc(256U << 20U);

    // ⭐⭐ A 3D grid (grid[2] > 1) cooks the EXPONENTIAL z-slice — clip.w (view_proj row 3 · world_pos) counted against the
    // published boundary table (the branchless Step-sum). It adds nodes OVER the 2D tiled grid (which bins slice 0): if it
    // did NOT, the depth term was declared and ignored and every fragment in a screen column would share one froxel (distant
    // lights leaking into the foreground — the exact failure the 3D dimension exists to kill). Both cook; 3D is strictly larger.
    const int tiled_2d = cook_size(&alloc, "\n[counts]\npoint = 4\n"
                                           "\n[cluster]\nenabled = true\ngrid = [4, 4, 1]\nmax_per_cluster = 4\n");
    const int clustered_3d = cook_size(&alloc, "\n[counts]\npoint = 4\n"
                                               "\n[cluster]\nenabled = true\ngrid = [4, 4, 4]\nmax_per_cluster = 4\n");
    CHECK(tiled_2d > 0);
    CHECK(clustered_3d > 0);
    CHECK(clustered_3d > tiled_2d);

    // ⭐⭐ BYTE-STABILITY (the 18a contract). A 2D grid's identity is IDENTICAL regardless of slice_bounds_off — grid[2] == 1
    // never hashes or reads it. ⛔ If this drifts, every existing forward-clustered variant id churns and the whole 2D cook
    // moves. Prove it by MUTATING the word on a parsed 2D desc: the id must not budge.
    lc::LightingDesc d2(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\npoint = 4\n\n[cluster]\nenabled = true\ngrid = [4, 4, 1]\nmax_per_cluster = 4\n", d2)
            == lc::LightingCookError::Ok);
    const u64 id_2d = lc::lighting_variant_id(d2);
    d2.header.slice_bounds_off = 0U;   // as if undeclared
    CHECK(lc::lighting_variant_id(d2) == id_2d);
    d2.header.slice_bounds_off = 987U; // any other location
    CHECK(lc::lighting_variant_id(d2) == id_2d);

    // ⭐⭐ …and the MIRROR: a 3D grid BAKES the table location into the FS (it reads header[slice_bounds_off + i]), so moving
    // the word is a different cooked program — the id MUST move, or the variant cache serves a 3D FS that Steps against the
    // wrong words.
    lc::LightingDesc d3(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\npoint = 4\n\n[cluster]\nenabled = true\ngrid = [4, 4, 4]\nmax_per_cluster = 4\n", d3)
            == lc::LightingCookError::Ok);
    const u64 id_3d = lc::lighting_variant_id(d3);
    d3.header.slice_bounds_off = 200U;
    CHECK(lc::lighting_variant_id(d3) != id_3d);

    // ⛔ A 3D grid with NO slice_bounds_off is REFUSED — the FS would Step against header word 0 (view_proj[0]), mis-binning
    // every fragment. Built RAW (bypassing make(), which now injects slice_bounds_off) so the word is genuinely absent (0).
    // ⛔ CONDITIONAL: a 2D grid (grid[2] == 1) with the same absent word cooks fine (proven above) — the reject is 3D-only.
    {
        lc::LightingDesc   bad(&alloc);
        containers::String wb(&alloc);
        CHECK(lc::parse_lighting_toml(
                  containers::StringView("schema = 1\nname = \"nb\"\n\n[header]\nview_proj = 6\ncluster_off = 21\n"
                                         "viewport_w = 111\nviewport_h = 112\n\n[counts]\npoint = 4\n"
                                         "\n[cluster]\nenabled = true\ngrid = [4, 4, 4]\nmax_per_cluster = 4\n"),
                  bad, &wb)
              == lc::LightingCookError::BadCluster);
    }
}

TEST_CASE("REN-38-E5: CLUSTERED culling bounds the unrolled loop", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(256U << 20U);
    // ⭐⭐ THE POINT OF CLUSTERING is that the unrolled bound stops being the SCENE's light count. Sixteen point
    // lights culled to four per froxel must cook a SMALLER program than sixteen walked directly — if it did not,
    // the cluster list was declared and ignored, and the only symptom would be a frame-time nobody expected.
    const int direct = cook_size(&alloc, "\n[counts]\npoint = 16\n");
    // ⭐ CEIR-18a-2: clustering now REQUIRES the viewport-dims header words (the FS normalizes FragCoord by them); the
    // shared header (make()) declares them, so a clustered section cooks. They add a handful of nodes, far fewer than
    // the 16→4 light savings.
    const int clustered = cook_size(&alloc, "\n[counts]\npoint = 16\n"
                                            "\n[cluster]\nenabled = true\ngrid = [16, 9, 24]\nmax_per_cluster = 4\n");
    CHECK(direct > 0);
    CHECK(clustered > 0);
    CHECK(clustered < direct);

    // ⛔ A froxel grid with a zero axis divides the screen into nothing; a per-cluster cap above the light cap is
    // an unrolled loop with no bound.
    lc::LightingDesc bad(&alloc);
    CHECK(parse(&alloc, "\n[counts]\npoint = 4\n\n[cluster]\nenabled = true\ngrid = [16, 0, 24]\n", bad)
          == lc::LightingCookError::BadCluster);
    CHECK(parse(&alloc, "\n[counts]\npoint = 4\n\n[cluster]\nenabled = true\nmax_per_cluster = 999\n", bad)
          == lc::LightingCookError::BadCluster);
    // ⛔ CEIR-18a-2: a VALID froxel grid + cap but NO viewport-dims header words is still refused — the clustered FS
    // divides FragCoord by those words, and word 0 (unset) is garbage. ⛔ Built RAW (bypassing make()'s shared header,
    // which now declares viewport_w/h) so the words are genuinely absent (default 0). The cook rejects the incomplete desc.
    {
        containers::String w2(&alloc);
        CHECK(lc::parse_lighting_toml(
                  containers::StringView("schema = 1\nname = \"nv\"\n\n[counts]\npoint = 4\n"
                                         "\n[cluster]\nenabled = true\ngrid = [4, 4, 1]\nmax_per_cluster = 4\n"),
                  bad, &w2)
              == lc::LightingCookError::BadCluster);
    }
}

TEST_CASE("REN-38-E1: the VARIANT IDENTITY covers every axis", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    lc::LightingDesc      a(&alloc);
    lc::LightingDesc      b(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\ndirectional = 1\npoint = 2\n", a) == lc::LightingCookError::Ok);
    REQUIRE(parse(&alloc, "\n[counts]\ndirectional = 1\npoint = 2\n", b) == lc::LightingCookError::Ok);
    // ⛔ STABLE across two parses: hashing the descriptor's BYTES would fold PADDING — uninitialised stack
    // history — into the id, so the same declaration would hash differently every run.
    CHECK(lc::lighting_variant_id(a) == lc::lighting_variant_id(b));
    const u64 base = lc::lighting_variant_id(a);

    // ⛔ EVERY axis that changes the cooked graph must change the id, or the variant cache serves one lighting
    // program where another belongs — a scene lit by the wrong technique, silently.
    const char* axes[7] = {
        "\n[counts]\ndirectional = 1\npoint = 3\n",
        "\n[counts]\ndirectional = 1\npoint = 2\n\n[shadow]\ndirectional = \"csm\"\ntaps = 4\n",
        "\n[counts]\ndirectional = 1\npoint = 2\n\n[ibl]\ndiffuse = true\n",
        "\n[counts]\ndirectional = 1\npoint = 2\n\n[ibl]\nspecular = true\n",
        "\n[counts]\ndirectional = 1\npoint = 2\n\n[cluster]\nenabled = true\nmax_per_cluster = 4\n",
        "\n[counts]\ndirectional = 1\npoint = 2\n\n[decal]\ncount = 2\nstride = 20\nprojection = 0\ntint = 16\n",
        "\n[counts]\ndirectional = 1\npoint = 2\n\n[shadow]\ndirectional = \"csm\"\ntaps = 4\ncontact = true\n",
    };
    for (const char* ax : axes)
    {
        lc::LightingDesc v(&alloc);
        INFO(ax);
        REQUIRE(parse(&alloc, ax, v) == lc::LightingCookError::Ok);
        CHECK(lc::lighting_variant_id(v) != base);
    }
    // …and the RECORD LAYOUT is part of the identity too — the same lights read from different words are a
    // different program.
    lc::LightingDesc moved(&alloc);
    REQUIRE(parse(&alloc, "\n[counts]\ndirectional = 1\npoint = 2\n", moved) == lc::LightingCookError::Ok);
    moved.record.color = 32U;
    CHECK(lc::lighting_variant_id(moved) != base);
}

TEST_CASE("REN-38-E1: a lighting declaration survives an editor ROUND TRIP", "[light-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    lc::LightingDesc      a(&alloc);
    REQUIRE(parse(&alloc,
                  "\n[counts]\ndirectional = 1\npoint = 2\nspot = 1\nrect = 1\ntube = 1\ndisk = 1\n"
                  "\n[shadow]\ndirectional = \"csm\"\npoint = \"cube\"\nspot = \"map\"\nfilter = \"pcss\"\n"
                  "taps = 16\ncascades = 3\ncontact = true\ncontact_steps = 6\n"
                  "\n[ibl]\ndiffuse = true\nspecular = true\n"
                  "\n[cluster]\nenabled = true\ngrid = [8, 8, 16]\nmax_per_cluster = 6\n"
                  "\n[decal]\ncount = 2\nstride = 20\nprojection = 0\ntint = 16\n",
                  a)
            == lc::LightingCookError::Ok);

    // ⛔ A tool's save must not silently drop what it did not understand. The IDENTITY is the strongest single
    // statement of that: anything the cook consumes that failed to survive moves the id.
    containers::String   text = lc::emit_lighting_toml(a, &alloc);
    lc::LightingDesc     b(&alloc);
    containers::String   w(&alloc);
    REQUIRE(lc::parse_lighting_toml(containers::StringView(text.c_str(), text.size()), b, &w)
            == lc::LightingCookError::Ok);
    CHECK(lc::lighting_variant_id(b) == lc::lighting_variant_id(a));
    for (u32 i = 0; i < lc::kLightTypeCount; ++i)
    {
        CHECK(b.set.count[i] == a.set.count[i]);
        CHECK(b.shadow.mode[i] == a.shadow.mode[i]);
    }
    CHECK(b.shadow.filter == a.shadow.filter);
    CHECK(b.shadow.taps == a.shadow.taps);
    CHECK(b.shadow.contact == a.shadow.contact);
    CHECK(b.ibl.diffuse == a.ibl.diffuse);
    CHECK(b.ibl.specular == a.ibl.specular);
    CHECK(b.cluster.enabled == a.cluster.enabled);
    CHECK(b.cluster.max_per_cluster == a.cluster.max_per_cluster);
    CHECK(b.decal.count == a.decal.count);
    CHECK(b.record.stride == a.record.stride);
    CHECK(b.record.has_points == a.record.has_points);
    CHECK(b.record.has_shadow_vp == a.record.has_shadow_vp);
}

TEST_CASE("REN-38 audit: a corrupt .crdl is REFUSED cleanly, never a crash", "[light-cook][ren38]")
{
    // ⛔ The exact bytes the drift gate found shipped in `assets/lighting/scene_forward.crdl` — a codegen
    // artifact wrapped every line in quote-comma garbage, and because the file was an INERT COPY nothing had
    // ever parsed it. A parser handed garbage must answer ParseFailed; anything else is a robustness bug.
    memory::TlsfAllocator alloc(16U << 20U);
    lc::LightingDesc      d(&alloc);
    containers::String    w(&alloc);
    constexpr const char* garbage =
        "# comment\n',\n'schema = 1',\n'name   = \"x\"',\n'',\n'[header]',\n'view_proj   = 6',\n";
    CHECK(lc::parse_lighting_toml(containers::StringView(garbage), d, &w) == lc::LightingCookError::ParseFailed);
}
