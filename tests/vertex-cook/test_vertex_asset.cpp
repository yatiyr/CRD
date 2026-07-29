// test_vertex_asset.cpp — REN-38-D1/D2/D3/D4 (D-007 row 141): THE VERTEX PROGRAM AS AN AUTHORED ASSET.
//
// ⛔ WHAT THIS REPLACES. `build_scene_vs_shadowed` / `_skinned` / `build_shadow_vs` — ~200 lines of C++ with the
// pull layout compiled in. The tangent has been sitting in the vertex buffer (words 8..11 of the cooked 48-byte
// record) with NO SHADER ABLE TO SEE IT, because adding a varying meant editing the engine.

#include <crd/vertexcook/vertex_asset.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_lower.hpp>
#include <crd/kir/ckir_serialize.hpp>
#include <crd/kir/ckir_variant.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring> // memcmp — the round-trip fixed-point comparison

using namespace crd;
namespace vc = crd::vertcook;

namespace
{
// ⭐ THE REAL SCENE LAYOUT, authored. Every number here was a literal inside `scene_renderer.cpp`: the header word
// map, the 12-word vertex record, the 20-word instance record, the varying set.
// ⛔ NO ROOT-LEVEL `transform` HERE, on purpose: in TOML a bare key written after `[[varying]]` belongs to THAT
// table, not to the root. Tests that need a different transform PREPEND it (root keys are legal before the first
// table); tests that need more attributes, varyings, skinning or morphing APPEND a table.
constexpr const char* kScene = R"(
schema    = 1
name      = "crd://vertex/scene"

[header]
index_count   = 0
index_off     = 2
vertex_off    = 3
instance_off  = 4
visible_off   = 5
view_proj     = 6
light_vp      = 32
morph_off     = 100
morph_weights = 104

[vertex]
stride = 12

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[[attribute]]
name   = "normal"
offset = 3
comps  = 3
kind   = "direction"

[[attribute]]
name   = "uv"
offset = 6
comps  = 2
kind   = "value"

[[attribute]]
name   = "tangent"
offset = 8
comps  = 4
kind   = "direction"

[instance]
stride    = 20
transform = 0

[[instance_attribute]]
name   = "color"
offset = 16
comps  = 4
kind   = "value"

[[varying]]
name     = "world_normal"
location = 0
interp   = "smooth"
source   = ["world:normal"]

[[varying]]
name     = "tint"
location = 1
interp   = "flat"
source   = ["instance:color"]

[[varying]]
name     = "world_pos_depth"
location = 2
interp   = "smooth"
source   = ["world:position", "clip.w"]

[[varying]]
name     = "uv"
location = 3
interp   = "smooth"
source   = ["uv"]
)";

[[nodiscard]] bool has_const(const kir::KGraph& g, double want)
{
    for (int i = 0; i < g.size(); ++i)
    {
        const kir::KNode& n = g.node(i);
        if (n.op == kir::KOp::Const && n.cval == want) { return true; }
    }
    return false;
}

// ⛔⛔ REN-38-F6: the DEVICE-truth probes. A cooked entry that reads a builtin its stage cannot see, or storage
// the stage's emitter does not lower, can never create a program on any backend — and the original F1/F2 gates
// never looked, which is how a whole band of device-impossible entries closed green.
[[nodiscard]] bool reads_builtin(const kir::KGraph& g, kir::KBuiltin b)
{
    for (int i = 0; i < g.size(); ++i)
    {
        const kir::KNode& n = g.node(i);
        if (n.op == kir::KOp::Builtin && static_cast<kir::KBuiltin>(n.iidx) == b) { return true; }
    }
    return false;
}
[[nodiscard]] bool has_op(const kir::KGraph& g, kir::KOp op)
{
    for (int i = 0; i < g.size(); ++i)
    {
        if (g.node(i).op == op) { return true; }
    }
    return false;
}

// ⭐ REN-38-F6: THE PROCEDURAL STAGE DECLARATIONS. A domain or mesh stage has NO vertex record — its position is
// GENERATED (the emitter's bilerped patch point / the meshlet grid slot) and the node graph DISPLACES it. The one
// nameable attribute is `position` (the generated point); varyings are `node:`/`clip.w` terms only.
constexpr const char* kDomain = R"(
schema   = 1
name     = "crd://vertex/domain"
stage    = "tess_eval"
displace = "swell"

[vertex]
stride = 3

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[[varying]]
name       = "swell_h"
location   = 0
interp     = "smooth"
source     = ["node:swell", "clip.w"]
node_comps = [3]

[tess]
patch_size = 4
inner      = 12.0
outer      = 6.0

[[node]]
name   = "swell"
op     = "multiply"
inputs = ["@position", [7.77, 7.77, 7.77]]
)";

constexpr const char* kMeshlet = R"(
schema   = 1
name     = "crd://vertex/meshlet"
stage    = "mesh"
displace = "lift"

[vertex]
stride = 3

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[[varying]]
name       = "sheen"
location   = 0
interp     = "smooth"
source     = ["node:lift", "clip.w"]
node_comps = [3]

[mesh]
max_vertices   = 126
max_primitives = 42
workgroup      = 32

[[node]]
name   = "lift"
op     = "multiply"
inputs = ["@position", [7.77, 7.77, 7.77]]
)";

struct Cooked
{
    kir::KGraph g;
    kir::KEntry ve{};
    bool        ok = false;

    explicit Cooked(memory::IAllocator* a) : g(a) {}
};

void cook_text(memory::IAllocator* a, const char* toml, Cooked& out)
{
    vc::VertexProgramDesc desc(a);
    containers::String    where(a);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(toml), desc, &where) == vc::VertexCookError::Ok);
    out.ok = vc::cook_vertex_program(desc, out.g, out.ve);
}
} // namespace

TEST_CASE("REN-38-D1: the scene vertex layout, AUTHORED, cooks to a CKIR vertex entry", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    vc::VertexProgramDesc desc(&alloc);
    containers::String    where(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), desc, &where) == vc::VertexCookError::Ok);
    CHECK(desc.attrs.size() == 4U);
    CHECK(desc.varyings.size() == 4U);
    CHECK(desc.vertex_stride == 12U);

    kir::KGraph g(&alloc);
    kir::KEntry ve{};
    REQUIRE(vc::cook_vertex_program(desc, g, ve));
    CHECK(ve.stage == kir::KStage::Vertex);
    CHECK(ve.position >= 0);
    REQUIRE(ve.n_out == 4);
    // ⛔ The declared LOCATIONS and INTERPOLATION reach the entry. A varying that silently landed at another slot
    // links and binds — the fragment shader just reads a different field.
    CHECK(ve.out[0].location == 0);
    CHECK(ve.out[0].interp == kir::Interp::Smooth);
    CHECK(ve.out[1].location == 1);
    CHECK(ve.out[1].interp == kir::Interp::Flat); // an instance tint is per-instance; interpolating it is a gradient
    CHECK(ve.out[2].location == 2);
    CHECK(ve.out[3].location == 3);
    // widths, from the asset
    CHECK(vc::varying_width(desc, 0) == 3U);
    CHECK(vc::varying_width(desc, 1) == 4U);
    CHECK(vc::varying_width(desc, 2) == 4U); // world position + clip.w, packed — the shadowed path's one extra
    CHECK(vc::varying_width(desc, 3) == 2U);
    CHECK(g.node(ve.out[0].node).comps() == 3);
    CHECK(g.node(ve.out[2].node).comps() == 4);
    CHECK(kir::entry_valid(g, ve));
}

TEST_CASE("REN-38-D1: the TANGENT that no shader could see becomes a varying", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    // ⭐⭐ THE CLAIM OF THE WHOLE BAND, in its smallest form. The tangent has been in the cooked vertex record
    // (`pos3 · normal3 · uv2 · tangent4`) since the mesh cooker was written, and NOTHING could read it: the VS
    // was C++ and stopped at word 8. This asset reaches it with no engine change.
    containers::String toml(&alloc);
    toml.append(kScene);
    toml.append("\n[[varying]]\nname     = \"world_tangent\"\nlocation = 4\ninterp   = \"smooth\"\n"
                "source   = [\"world:tangent\"]\n");

    Cooked c(&alloc);
    cook_text(&alloc, toml.c_str(), c);
    REQUIRE(c.ok);
    REQUIRE(c.ve.n_out == 5);
    CHECK(c.ve.out[4].location == 4);
    // ⛔ FOUR components: xyz rotated into world, and the HANDEDNESS SIGN passed through untouched. Rotating the
    // 4th component would turn a ±1 flag into a scaled number, and a tangent frame with a mangled sign mirrors
    // the normal map on exactly half the mesh — which reads as bad art, not a bad transform.
    CHECK(c.g.node(c.ve.out[4].node).comps() == 4);
    // …and the cooked entry reads word 8, which the old VS never touched.
    CHECK(has_const(c.g, 8.0));
    CHECK(has_const(c.g, 11.0));
}

TEST_CASE("REN-38-D1: every way a `.crdv` can be wrong is a NAMED error", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    const auto            parse = [&](const char* toml) {
        vc::VertexProgramDesc d(&alloc);
        containers::String    where(&alloc);
        return vc::parse_vertex_toml(containers::StringView(toml), d, &where);
    };
    using E = vc::VertexCookError;

    const auto with = [&](const char* extra) {
        containers::String t(&alloc);
        t.append(kScene);
        t.append(extra);
        vc::VertexProgramDesc d(&alloc);
        containers::String    where(&alloc);
        return vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &where);
    };

    // ⛔ AN ATTRIBUTE PAST THE STRIDE pulls the NEXT vertex's words. The mesh still draws — every vertex wearing
    // its neighbour's data, which reads as a corrupt mesh rather than a bad offset.
    CHECK(with("\n[[attribute]]\nname = \"oops\"\noffset = 10\ncomps = 4\nkind = \"value\"\n") == E::AttrOutOfRecord);
    CHECK(with("\n[[attribute]]\nname = \"wide\"\noffset = 0\ncomps = 7\nkind = \"value\"\n") == E::BadComponentCount);
    // ⛔ TWO VARYINGS AT ONE LOCATION: one silently wins, and which one depends on emission order.
    CHECK(with("\n[[varying]]\nname = \"dup\"\nlocation = 1\nsource = [\"uv\"]\n") == E::DuplicateLocation);
    CHECK(with("\n[[varying]]\nname = \"ghost\"\nlocation = 5\nsource = [\"nosuch\"]\n") == E::UnknownSource);
    CHECK(with("\n[[varying]]\nname = \"empty\"\nlocation = 5\nsource = []\n") == E::EmptySource);
    // ⛔ Wider than a vec4 — there is no such interpolant, and a silent truncation drops the tail.
    CHECK(with("\n[[varying]]\nname = \"fat\"\nlocation = 5\nsource = [\"world:position\", \"instance:color\"]\n")
          == E::VaryingTooWide);
    CHECK(with("\n[[varying]]\nname = \"twin\"\nlocation = 5\nsource = [\"uv\"]\n"
               "\n[[varying]]\nname = \"twin\"\nlocation = 6\nsource = [\"uv\"]\n")
          == E::DuplicateName);
    // ⛔ NOTHING TO PROJECT. Without an attribute of kind `position` the entry would emit a clip position built
    // from whatever came first — geometry somewhere, which is worse than no geometry.
    CHECK(parse("schema = 1\nname = \"m\"\n[vertex]\nstride = 4\n[[attribute]]\nname = \"uv\"\noffset = 0\n"
                "comps = 2\nkind = \"value\"\n[[varying]]\nname = \"uv\"\nlocation = 0\nsource = [\"uv\"]\n")
          == E::NoPosition);
    // ⛔ A CASCADE the header cannot hold reads past the light_vp run — a shadow map projected by uninitialised
    // memory, which FLICKERS rather than failing.
    {
        containers::String t(&alloc);
        t.append("transform = \"light_vp\"\ncascade = 9\n");
        t.append(kScene);
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &w) == E::BadTransform);
    }
    // ⛔ The instance TRANSFORM is sixteen words; a mat4 that does not fit reads the next instance's rows and
    // every object inherits a neighbour's orientation.
    {
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), d, &w) == E::Ok);
        d.instance.transform = 8U; // 8 + 16 > 20
        CHECK(vc::validate_vertex_program(d, &w) == E::AttrOutOfRecord);
    }
}

TEST_CASE("REN-38-D1: the LAYOUT ID fills the variant key's reserved `vertex` axis", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    // ⛔⛔ `VariantKey::vertex` has been a RESERVED FIELD THAT NOTHING FILLED. Two different layouts therefore
    // hashed to the SAME variant key, so the cache would hand the second one the FIRST one's program — a dedup
    // COLLISION, exactly the failure `ckir_variant.hpp` names for an undeclared axis.
    vc::VertexProgramDesc a(&alloc);
    vc::VertexProgramDesc b(&alloc);
    containers::String    w(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), a, &w) == vc::VertexCookError::Ok);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), b, &w) == vc::VertexCookError::Ok);
    // ⛔ STABLE across two independent parses. Hashing the descriptor's BYTES would fold PADDING — uninitialised
    // stack history — into the id, so the same layout would hash differently every run and the variant cache
    // would miss every time (the 2026-07-25 cook-dedup scar).
    CHECK(vc::vertex_layout_id(a) == vc::vertex_layout_id(b));

    // ⛔⛔ A REUSED DESCRIPTOR IS RESET, not appended to. Parsing a second asset into `b` used to MERGE it with
    // the first: with overlapping names it surfaced as `DuplicateName` (an error naming the wrong thing), and
    // with distinct names as a silently merged layout reading two assets' words out of one vertex record. Any
    // tool with a load button reuses its descriptor, so this is the normal path, not an edge case.
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), b, &w) == vc::VertexCookError::Ok);
    CHECK(b.attrs.size() == a.attrs.size());
    CHECK(b.varyings.size() == a.varyings.size());
    CHECK(vc::vertex_layout_id(b) == vc::vertex_layout_id(a));

    const u64 base = vc::vertex_layout_id(a);
    // …and EVERY axis that changes what the shader reads or emits changes the id.
    b.attrs[1].offset = 4U;
    CHECK(vc::vertex_layout_id(b) != base);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), b, &w) == vc::VertexCookError::Ok);
    b.attrs[1].kind = vc::AttrKind::Value; // a normal that stops being transformed is a different program
    CHECK(vc::vertex_layout_id(b) != base);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), b, &w) == vc::VertexCookError::Ok);
    b.transform = vc::VertexTransform::LightVp;
    CHECK(vc::vertex_layout_id(b) != base);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), b, &w) == vc::VertexCookError::Ok);
    b.cascade = 2U;
    b.transform = vc::VertexTransform::LightVp;
    const u64 casc2 = vc::vertex_layout_id(b);
    b.cascade       = 3U;
    CHECK(vc::vertex_layout_id(b) != casc2); // per-cascade variants are DIFFERENT programs, not one with a uniform
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), b, &w) == vc::VertexCookError::Ok);
    b.varyings[0].location = 6U;
    CHECK(vc::vertex_layout_id(b) != base);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), b, &w) == vc::VertexCookError::Ok);
    b.instance.stride = 24U;
    CHECK(vc::vertex_layout_id(b) != base);

    // ⭐⭐ AND THE ID IS THE VALUE THE VARIANT KEY TAKES. `VariantKey::vertex` is a u32 axis, so the layout id is
    // FOLDED to 32 bits on the way in — which is the only place a collision could be reintroduced, and therefore
    // the place to check it. ⛔ Two layouts that hashed to one key would make the variant cache serve the second
    // one the FIRST one's program: the dedup collision `ckir_variant.hpp` names as the failure an undeclared axis
    // produces, arriving through a declared one.
    const auto fold = [](u64 h) { return static_cast<u32>(h ^ (h >> 32)); };
    kir::technique::VariantKey ka;
    kir::technique::VariantKey kb;
    ka.vertex = fold(base);
    kb.vertex = fold(vc::vertex_layout_id(b));
    CHECK(ka.vertex != kb.vertex);
    CHECK(kir::technique::variant_key_hash(ka) != kir::technique::variant_key_hash(kb));
    // …and the SAME layout must reach the SAME key, or every cook misses its cache.
    kb.vertex = fold(base);
    CHECK(kir::technique::variant_key_hash(ka) == kir::technique::variant_key_hash(kb));
}

TEST_CASE("REN-38-D3: the SHADOW pass is the same declaration with a different transform", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⭐ The per-cascade `light_vp` transform was a HAND-WRITTEN VS variant (`build_shadow_vs(g, ve, cascade)`).
    // It is now the same asset with `transform = "light_vp"` and a cascade — a cook-time parameter.
    const auto shadow = [&](const char* prefix, Cooked& out) {
        containers::String t(&alloc);
        t.append(prefix);
        t.append(kScene);
        cook_text(&alloc, t.c_str(), out);
    };
    Cooked cam(&alloc);
    Cooked c0(&alloc);
    Cooked c1(&alloc);
    shadow("", cam);
    shadow("transform = \"light_vp\"\ncascade = 0\n", c0);
    shadow("transform = \"light_vp\"\ncascade = 1\n", c1);
    REQUIRE(cam.ok);
    REQUIRE(c0.ok);
    REQUIRE(c1.ok);

    // ⛔ THE DIFFERENCE IS THE HEADER SLICE READ, not the node count — two programs of identical shape that read
    // different words are exactly what a count cannot tell apart. Cascade 0 reads header words 32..47, cascade 1
    // reads 48..63; the camera path reads 6..21.
    CHECK(has_const(cam.g, 21.0));
    CHECK_FALSE(has_const(cam.g, 47.0));
    CHECK(has_const(c0.g, 32.0));
    CHECK(has_const(c0.g, 47.0));
    CHECK_FALSE(has_const(c0.g, 48.0)); // cascade 0 must not reach into cascade 1's matrix
    CHECK(has_const(c1.g, 48.0));
    CHECK(has_const(c1.g, 63.0));
}

TEST_CASE("REN-38-D2: SKINNING is a declared SCHEME, not a compiled-in one", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    const auto            skinned = [&](const char* skin, Cooked& out) {
        containers::String t(&alloc);
        t.append(kScene);
        t.append(skin);
        cook_text(&alloc, t.c_str(), out);
    };

    Cooked lbs(&alloc);
    Cooked dqs(&alloc);
    Cooked wide(&alloc);
    skinned("\n[skin]\nscheme = \"linear_blend\"\ninfluences = 4\nstride = 6\njoint_words = 2\n"
            "weight_off = 2\npalette_stride = 16\n",
            lbs);
    // ⭐ DQS: LBS interpolates MATRICES, so a joint twisted 180° averages toward a degenerate one and the limb
    // collapses to its axis — the candy-wrapper artifact. Choosing the scheme was an engine edit; it is a line.
    skinned("\n[skin]\nscheme = \"dual_quaternion\"\ninfluences = 4\nstride = 6\njoint_words = 2\n"
            "weight_off = 2\npalette_stride = 8\n",
            dqs);
    // ⭐ EIGHT influences — a rig the four-influence C++ could not express at all.
    skinned("\n[skin]\nscheme = \"linear_blend\"\ninfluences = 8\nstride = 12\njoint_words = 4\n"
            "weight_off = 4\npalette_stride = 16\n",
            wide);
    REQUIRE(lbs.ok);
    REQUIRE(dqs.ok);
    REQUIRE(wide.ok);

    // ⛔ DISTINGUISHABLE SIGNALS, not node counts. DQS normalizes the blended quaternion (its 1e-12 guard) and
    // sign-corrects against the first joint (-1.0); LBS does neither, and no other part of the program emits them.
    CHECK(has_const(dqs.g, 1e-12));
    CHECK(has_const(dqs.g, -1.0));
    CHECK_FALSE(has_const(lbs.g, 1e-12));
    CHECK_FALSE(has_const(lbs.g, -1.0));
    // The palette STRIDE reaches the address arithmetic — a DQS program pointed at a 16-word palette would read
    // every joint at the wrong place and animate, smoothly, wrongly.
    CHECK(has_const(dqs.g, 8.0));
    // 8 influences read weights at 4..11; 4 influences read 2..5. Word 11 exists only in the wide rig.
    CHECK(has_const(wide.g, 11.0));
    CHECK(wide.g.size() > lbs.g.size());
    // …and each scheme is a DIFFERENT program identity, so the variant cache cannot serve one for the other.
    {
        vc::VertexProgramDesc a(&alloc);
        vc::VertexProgramDesc b(&alloc);
        containers::String    w(&alloc);
        containers::String    ta(&alloc);
        containers::String    tb(&alloc);
        ta.append(kScene);
        ta.append("\n[skin]\nscheme = \"linear_blend\"\ninfluences = 4\nstride = 6\njoint_words = 2\n"
                  "weight_off = 2\npalette_stride = 16\n");
        tb.append(kScene);
        tb.append("\n[skin]\nscheme = \"dual_quaternion\"\ninfluences = 4\nstride = 6\njoint_words = 2\n"
                  "weight_off = 2\npalette_stride = 8\n");
        REQUIRE(vc::parse_vertex_toml(containers::StringView(ta.c_str(), ta.size()), a, &w)
                == vc::VertexCookError::Ok);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(tb.c_str(), tb.size()), b, &w)
                == vc::VertexCookError::Ok);
        CHECK(vc::vertex_layout_id(a) != vc::vertex_layout_id(b));
    }

    // ⛔ THE PALETTE STRIDE IS PART OF THE SCHEME. A DQS program pointed at a matrix palette is refused.
    {
        containers::String t(&alloc);
        t.append(kScene);
        t.append("\n[skin]\nscheme = \"dual_quaternion\"\ninfluences = 4\nstride = 6\njoint_words = 2\n"
                 "weight_off = 2\npalette_stride = 16\n");
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &w)
              == vc::VertexCookError::BadSkin);
    }
    // ⛔ …and a record whose weights do not fit reads past the vertex's skin entry.
    {
        containers::String t(&alloc);
        t.append(kScene);
        t.append("\n[skin]\nscheme = \"linear_blend\"\ninfluences = 8\nstride = 6\njoint_words = 4\n"
                 "weight_off = 4\npalette_stride = 16\n");
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &w)
              == vc::VertexCookError::BadSkin);
    }
}

TEST_CASE("REN-38-D2: MORPH TARGETS, which did not exist at all", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⛔ Zero code, zero data path before this row — a blend-shape face was unreachable.
    containers::String full(&alloc);
    full.append(kScene);
    full.append("\n[morph]\ntargets  = 3\nstride   = 3\napply_to = [\"position\", \"normal\"]\n");

    Cooked plain(&alloc);
    Cooked morphed(&alloc);
    cook_text(&alloc, kScene, plain);
    cook_text(&alloc, full.c_str(), morphed);
    REQUIRE(plain.ok);
    REQUIRE(morphed.ok);
    CHECK(has_const(morphed.g, 100.0));
    CHECK(has_const(morphed.g, 104.0));
    CHECK_FALSE(has_const(plain.g, 100.0));
    CHECK(morphed.g.size() > plain.g.size());

    // ⛔ A morph must say WHICH attributes it drives. One that displaced the position while the normals stayed
    // put would light a moving face as if it were still — a lighting bug with a geometry cause.
    {
        containers::String bad(&alloc);
        bad.append(kScene);
        bad.append("\n[morph]\ntargets = 2\nstride = 3\napply_to = [\"nosuch\"]\n"
                   );
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(bad.c_str(), bad.size()), d, &w)
              == vc::VertexCookError::UnknownSource);
    }
    // ⛔ …and a delta narrower than the attribute it drives leaves the tail at rest.
    {
        containers::String bad(&alloc);
        bad.append(kScene);
        bad.append("\n[morph]\ntargets = 2\nstride = 2\napply_to = [\"position\"]\n"
                   );
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(bad.c_str(), bad.size()), d, &w)
              == vc::VertexCookError::BadMorph);
    }
    // ⛔ …and a morph with NO DECLARED STREAM would read from word 0 — the header itself. Driven through
    // `validate_vertex_program` rather than the text, because the PROGRAMMATIC path is held to the same rules:
    // a check only the parser performed would make the ergonomic path the unsafe one.
    {
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(full.c_str(), full.size()), d, &w)
                == vc::VertexCookError::Ok);
        d.header.morph_off = 0U;
        CHECK(vc::validate_vertex_program(d, &w) == vc::VertexCookError::BadMorph);
    }
}

TEST_CASE("REN-38-D3: VS DISPLACEMENT is an authored node graph", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⭐ The SAME node vocabulary a material uses (`crd-material-cook`'s registry) — a second node library would
    // be two vocabularies that drift, and an author could not tell which asset understood which node.
    containers::String t(&alloc);
    t.append("displace = \"pushed\"\n");
    t.append(kScene);
    t.append("\n[[node]]\nname   = \"pushed\"\nop     = \"multiply\"\n"
             "inputs = [\"@position\", [7.77, 7.77, 7.77]]\n");

    Cooked plain(&alloc);
    Cooked disp(&alloc);
    cook_text(&alloc, kScene, plain);
    cook_text(&alloc, t.c_str(), disp);
    REQUIRE(plain.ok);
    REQUIRE(disp.ok);

    // ⛔⛔ THE CLAIM IS THAT THE POSITION ACTUALLY DEPENDS ON IT, not that the node exists. Building the node and
    // never wiring it would leave the literal in the graph and pass a naive presence check — so both graphs are
    // run through B7 `lower_entry`, whose DCE removes anything the entry cannot reach. The constant survives only
    // if the clip position is genuinely computed from it.
    kir::lower::lower_entry(disp.g, disp.ve);
    kir::lower::lower_entry(plain.g, plain.ve);
    CHECK(has_const(disp.g, 7.77));
    CHECK_FALSE(has_const(plain.g, 7.77));

    // …and the control: the same node DECLARED but not named as the displacement is DCE'd away.
    {
        containers::String unwired(&alloc);
        unwired.append(kScene);
        unwired.append("\n[[node]]\nname   = \"pushed\"\nop     = \"multiply\"\n"
                       "inputs = [\"@position\", [7.77, 7.77, 7.77]]\n");
        Cooked u(&alloc);
        cook_text(&alloc, unwired.c_str(), u);
        REQUIRE(u.ok);
        CHECK(has_const(u.g, 7.77)); // built…
        kir::lower::lower_entry(u.g, u.ve);
        CHECK_FALSE(has_const(u.g, 7.77)); // …and gone once nothing reads it
    }

    // ⛔ The displacement graph is held to the SAME rules a material's is — the registry is shared, so its
    // checks must be too, or the vertex side would be the unsafe way to reach the same nodes.
    const auto bad = [&](const char* extra) {
        containers::String s(&alloc);
        s.append(kScene);
        s.append(extra);
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        return vc::parse_vertex_toml(containers::StringView(s.c_str(), s.size()), d, &w);
    };
    using E = vc::VertexCookError;
    CHECK(bad("\n[[node]]\nname = \"a\"\nop = \"frobnicate\"\ninputs = [1.0]\n") == E::UnknownOp);
    CHECK(bad("\n[[node]]\nname = \"a\"\nop = \"mix\"\ninputs = [1.0, 2.0]\n") == E::WrongArity);
    CHECK(bad("\n[[node]]\nname = \"a\"\nop = \"absval\"\ninputs = [\"b\"]\n"
              "\n[[node]]\nname = \"b\"\nop = \"absval\"\ninputs = [1.0]\n")
          == E::NodeCycle);
    CHECK(bad("\n[[node]]\nname = \"a\"\nop = \"absval\"\ninputs = [\"@nosuch\"]\n") == E::UnknownSource);
    CHECK(bad("\n[[node]]\nname = \"v\"\nop = \"combine4\"\ninputs = [1.0, 0.0, 0.0, 1.0]\n"
              "\n[[node]]\nname = \"a\"\nop = \"extract\"\ninputs = [\"v\", \"v\"]\n")
          == E::AttrNotConstant);
    {
        containers::String s(&alloc);
        s.append("displace = \"ghost\"\n");
        s.append(kScene);
        vc::VertexProgramDesc dd(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(s.c_str(), s.size()), dd, &w) == E::UnknownSource);
    }
}

TEST_CASE("REN-38-D3: a displacement node can feed a VARYING", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    containers::String    t(&alloc);
    t.append(kScene);
    t.append("\n[[node]]\nname   = \"wave\"\nop     = \"multiply\"\ninputs = [\"@uv\", [3.5, 3.5]]\n"
             "\n[[varying]]\nname       = \"wave_uv\"\nlocation   = 5\ninterp     = \"smooth\"\n"
             "source     = [\"node:wave\"]\nnode_comps = [2]\n");
    Cooked c(&alloc);
    cook_text(&alloc, t.c_str(), c);
    REQUIRE(c.ok);
    REQUIRE(c.ve.n_out == 5);
    CHECK(c.g.node(c.ve.out[4].node).comps() == 2);

    // ⛔⛔ THE DECLARED WIDTH IS CROSS-CHECKED against what the graph actually built. 38-D4 answers "how wide is
    // this varying?" from the ASSET — so a declaration that disagreed with the shader would make the contract
    // check confirm a match that does not exist, which is worse than no check.
    containers::String lying(&alloc);
    lying.append(kScene);
    lying.append("\n[[node]]\nname   = \"wave\"\nop     = \"multiply\"\ninputs = [\"@uv\", [3.5, 3.5]]\n"
                 "\n[[varying]]\nname       = \"wave_uv\"\nlocation   = 5\ninterp     = \"smooth\"\n"
                 "source     = [\"node:wave\"]\nnode_comps = [4]\n");
    vc::VertexProgramDesc d(&alloc);
    containers::String    w(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(lying.c_str(), lying.size()), d, &w)
            == vc::VertexCookError::Ok); // the DECLARATION is well-formed…
    kir::KGraph g(&alloc);
    kir::KEntry ve{};
    CHECK_FALSE(vc::cook_vertex_program(d, g, ve)); // …and the COOK refuses it
}

TEST_CASE("REN-38-D4: the VS-to-FS varying CONTRACT is checked, by name", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    vc::VertexProgramDesc desc(&alloc);
    containers::String    where(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), desc, &where) == vc::VertexCookError::Ok);

    // ⭐ Exactly what the cooked scene FS reads (`build_scene_fs_cooked`): world_normal vec3 @0 smooth · tint vec4
    // @1 flat · world_pos+depth vec4 @2 smooth · uv vec2 @3 smooth.
    const vc::VaryingRequirement fs[4] = {
        {"world_normal", 0U, 3U, false},
        {"tint", 1U, 4U, true},
        {"world_pos_depth", 2U, 4U, false},
        {"uv", 3U, 2U, false},
    };
    CHECK(vc::verify_varying_contract(desc, static_cast<const vc::VaryingRequirement*>(fs), 4U, &where)
          == vc::VertexCookError::Ok);

    // ⛔⛔ EVERY WAY THE PAIR CAN DISAGREE. None of these fails to link, none produces a validation message on
    // either backend, and every one of them renders — from the wrong field.
    const auto rejects = [&](const vc::VaryingRequirement& r) {
        return vc::verify_varying_contract(desc, &r, 1U, &where) == vc::VertexCookError::ContractMismatch;
    };
    CHECK(rejects({"worldNormal", 0U, 3U, false}));      // RENAMED — the FS asks for something that is not emitted
    CHECK(rejects({"world_normal", 2U, 3U, false}));     // MOVED — reads the world position instead
    CHECK(rejects({"world_normal", 0U, 4U, false}));     // WRONG WIDTH — reads a component that is not there
    // ⛔ A SMOOTH varying read as FLAT takes the provoking vertex's value across the whole triangle: faceted
    // output that reads as a normals bug, in a shader whose normals are fine.
    CHECK(rejects({"world_normal", 0U, 3U, true}));
    CHECK(rejects({"vertex_color", 7U, 4U, false}));     // DROPPED — the VS never emitted it at all

    // ⭐ And the check is a real gate on the asset, not a formality: remove a varying the FS needs and the pair
    // stops verifying, which is the moment an author finds out — at cook time, not in a screenshot.
    vc::VertexProgramDesc trimmed(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), trimmed, &where) == vc::VertexCookError::Ok);
    trimmed.varyings.pop_back(); // drop `uv`
    CHECK(vc::verify_varying_contract(trimmed, static_cast<const vc::VaryingRequirement*>(fs), 4U, &where)
          == vc::VertexCookError::ContractMismatch);
}

TEST_CASE("REN-38-D1: a `.crdv` survives an editor ROUND TRIP", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    containers::String    t(&alloc);
    t.append(kScene);
    t.append("\n[skin]\nscheme = \"dual_quaternion\"\ninfluences = 4\nstride = 6\njoint_words = 2\n"
             "weight_off = 2\npalette_stride = 8\n"
             "\n[[node]]\nname   = \"wave\"\nop     = \"multiply\"\ninputs = [\"@uv\", [3.5, 3.5]]\n"
             "\n[[varying]]\nname       = \"wave_uv\"\nlocation   = 5\ninterp     = \"smooth\"\n"
             "source     = [\"node:wave\"]\nnode_comps = [2]\n");

    vc::VertexProgramDesc a(&alloc);
    containers::String    where(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), a, &where)
            == vc::VertexCookError::Ok);

    // ⛔ A tool's save must not silently drop what it did not understand — and a dropped attribute is a shader
    // that reads the wrong words. The IDENTITY is the strongest single statement of that: if anything the cook
    // consumes were lost, the layout id would move.
    containers::String    text = vc::emit_vertex_toml(a, &alloc);
    vc::VertexProgramDesc b(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(text.c_str(), text.size()), b, &where)
            == vc::VertexCookError::Ok);
    CHECK(vc::vertex_layout_id(b) == vc::vertex_layout_id(a));
    REQUIRE(b.attrs.size() == a.attrs.size());
    for (usize i = 0; i < a.attrs.size(); ++i)
    {
        CHECK(b.attrs[i].name == a.attrs[i].name);
        CHECK(b.attrs[i].offset == a.attrs[i].offset);
        CHECK(b.attrs[i].comps == a.attrs[i].comps);
        CHECK(b.attrs[i].kind == a.attrs[i].kind);
    }
    REQUIRE(b.varyings.size() == a.varyings.size());
    for (usize i = 0; i < a.varyings.size(); ++i)
    {
        CHECK(b.varyings[i].name == a.varyings[i].name);
        CHECK(b.varyings[i].location == a.varyings[i].location);
        CHECK(b.varyings[i].flat == a.varyings[i].flat);
        REQUIRE(b.varyings[i].source.size() == a.varyings[i].source.size());
        for (usize k = 0; k < a.varyings[i].source.size(); ++k)
        {
            CHECK(b.varyings[i].source[k].kind == a.varyings[i].source[k].kind);
            CHECK(b.varyings[i].source[k].name == a.varyings[i].source[k].name);
            CHECK(b.varyings[i].source[k].comps == a.varyings[i].source[k].comps);
        }
    }
    CHECK(b.skin.scheme == a.skin.scheme);
    CHECK(b.skin.palette_stride == a.skin.palette_stride);
    CHECK(b.instance.stride == a.instance.stride);
    CHECK(b.header.light_vp == a.header.light_vp);
}

// ── ⭐⭐ REN-38-F1..F5: THE ADVANCED STAGES. ────────────────────────────────
// ⛔⛔ CKIR HAS FOURTEEN STAGES AND THE ASSET REACHED TWO. The A band gave the raster context every verb —
// `draw_tess_load`, `draw_mesh_load`, `trace_rays`, `dispatch_kernel_indirect`, `draw_visbuffer_load` — and
// nothing could AUTHOR a program for any of them.

TEST_CASE("REN-38-F1: TESSELLATION is an authored stage pair", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⛔ ROOT KEYS BEFORE the first table, TABLES AFTER — in TOML a bare key written after `[tess]` belongs to
    // that table, so a `[tess]` section placed ahead of `schema` swallows it and the asset reports BadSchema.
    const auto            staged = [&](const char* stage_key, const char* section, Cooked& out) {
        containers::String t(&alloc);
        t.append(stage_key);
        t.append(kScene);
        t.append(section);
        cook_text(&alloc, t.c_str(), out);
    };
    Cooked hull(&alloc);
    Cooked domain(&alloc);
    staged("stage = \"tess_control\"\n", "\n[tess]\npatch_size = 4\ninner = 12.0\nouter = 6.0\n", hull);
    cook_text(&alloc, kDomain, domain);
    REQUIRE(hull.ok);
    REQUIRE(domain.ok);
    CHECK(hull.ve.stage == kir::KStage::TessControl);
    CHECK(domain.ve.stage == kir::KStage::TessEval);
    // ⛔ THE HULL SETS THE LEVELS; the domain does not. A hull entry without them is refused by `entry_valid`,
    // and a level of 0 collapses the patch to nothing — geometry that silently disappears.
    CHECK(hull.ve.tess_patch_size == 4U);
    REQUIRE(hull.ve.tess_inner >= 0);
    REQUIRE(hull.ve.tess_outer >= 0);
    CHECK(hull.g.node(hull.ve.tess_inner).cval == 12.0);
    CHECK(hull.g.node(hull.ve.tess_outer).cval == 6.0);
    CHECK(kir::entry_valid(hull.g, hull.ve));

    // ⛔⛔ REN-38-F6: THE DOMAIN ENTRY IS DEVICE-TRUE. The F1 cook pulled by `VertexIndex` — legal only in a
    // vertex stage, over an emitter that lowers no storage loads — so the cooked entry could never create a
    // program on ANY backend, and the cook-only gate never noticed. The claims that make it real: the position
    // comes from the emitter's bilerped patch point, the graph touches nothing the stage cannot see, and
    // `entry_valid` accepts the whole thing.
    CHECK(domain.ve.tess_patch_size == 4U);
    REQUIRE(domain.ve.position >= 0);
    CHECK(domain.g.node(domain.ve.position).comps() == 4);
    CHECK(reads_builtin(domain.g, kir::KBuiltin::TessPatchPosition));
    CHECK_FALSE(reads_builtin(domain.g, kir::KBuiltin::VertexIndex));
    CHECK_FALSE(has_op(domain.g, kir::KOp::StorageLoad));
    const char* why      = nullptr;
    const bool  valid_te = kir::entry_valid(domain.g, domain.ve, &why);
    INFO((why == nullptr ? "" : why));
    CHECK(valid_te);
    // …the DISPLACEMENT graph genuinely feeds the clip position (B7 DCE would eat an unwired constant)…
    kir::lower::lower_entry(domain.g, domain.ve);
    CHECK(has_const(domain.g, 7.77));
    // …and the varyings are the authored `node:`/`clip.w` terms at their declared location.
    REQUIRE(domain.ve.n_out == 1);
    CHECK(domain.ve.out[0].location == 0);

    // ⛔ A varying a domain stage cannot honour — an attribute or instance source with no vertex record behind
    // it — is REFUSED, not silently dropped: dropping it would bind a fragment program to garbage.
    Cooked refused(&alloc);
    staged("stage = \"tess_eval\"\n", "\n[tess]\npatch_size = 4\ninner = 12.0\nouter = 6.0\n", refused);
    CHECK_FALSE(refused.ok);

    // ⛔ The levels are VALIDATED, not clamped: a zero or absurd level is a named error, not a silent fix.
    const auto bad = [&](const char* stage_key, const char* section) {
        containers::String t(&alloc);
        t.append(stage_key);
        t.append(kScene);
        t.append(section);
        vc::VertexProgramDesc dd(&alloc);
        containers::String    w(&alloc);
        return vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), dd, &w);
    };
    CHECK(bad("stage = \"tess_control\"\n", "\n[tess]\npatch_size = 4\ninner = 0.0\nouter = 4.0\n")
          == vc::VertexCookError::BadTess);
    CHECK(bad("stage = \"tess_control\"\n", "\n[tess]\npatch_size = 3\ninner = 4.0\nouter = 4.0\n")
          == vc::VertexCookError::BadTess);
}

TEST_CASE("REN-38-F2: MESH + TASK amplification are authored stages", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⛔ ROOT KEYS BEFORE the first table, TABLES AFTER — in TOML a bare key written after `[tess]` belongs to
    // that table, so a `[tess]` section placed ahead of `schema` swallows it and the asset reports BadSchema.
    const auto            staged = [&](const char* stage_key, const char* section, Cooked& out) {
        containers::String t(&alloc);
        t.append(stage_key);
        t.append(kScene);
        t.append(section);
        cook_text(&alloc, t.c_str(), out);
    };
    Cooked mesh(&alloc);
    Cooked task(&alloc);
    cook_text(&alloc, kMeshlet, mesh);
    staged("stage = \"task\"\n", "\n[mesh]\nworkgroup = 32\n\n[task]\nemit = 7\n", task);
    REQUIRE(mesh.ok);
    REQUIRE(task.ok);
    CHECK(mesh.ve.stage == kir::KStage::Mesh);
    CHECK(mesh.ve.mesh_vertices == 126U);
    CHECK(mesh.ve.mesh_primitives == 42U);
    CHECK(mesh.ve.local_size[0] == 32U);
    // ⛔ A mesh entry MUST emit a primitive — three LOCAL vertex indices. Global ones would index past the
    // vertices this workgroup actually emitted.
    REQUIRE(mesh.ve.mesh_prim >= 0);
    CHECK(mesh.g.node(mesh.ve.mesh_prim).comps() == 3);
    // ⛔⛔ REN-38-F6: THE MESH ENTRY IS DEVICE-TRUE — the grid position comes from the workgroup builtins its
    // stage can actually read, never from a `VertexIndex` pull the mesh emitter cannot lower.
    CHECK(reads_builtin(mesh.g, kir::KBuiltin::LocalInvocationIndex));
    CHECK(reads_builtin(mesh.g, kir::KBuiltin::WorkgroupIndex));
    CHECK_FALSE(reads_builtin(mesh.g, kir::KBuiltin::VertexIndex));
    CHECK_FALSE(has_op(mesh.g, kir::KOp::StorageLoad));
    {
        const char* why      = nullptr;
        const bool  valid_ms = kir::entry_valid(mesh.g, mesh.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(valid_ms);
    }
    // …the displacement graph genuinely moves the generated grid point…
    kir::lower::lower_entry(mesh.g, mesh.ve);
    CHECK(has_const(mesh.g, 7.77));

    // ⛔⛔ REN-38-F6: A TASK ENTRY EMITS NO GEOMETRY — `entry_valid` refuses one carrying a position or outputs,
    // and the F2 cook gave it BOTH by riding the vertex pull path.
    CHECK(task.ve.stage == kir::KStage::Task);
    REQUIRE(task.ve.task_emit >= 0);
    CHECK(task.g.node(task.ve.task_emit).cval == 7.0);
    CHECK(task.ve.position < 0);
    CHECK(task.ve.n_out == 0);
    CHECK_FALSE(reads_builtin(task.g, kir::KBuiltin::VertexIndex));
    {
        const char* why      = nullptr;
        const bool  valid_tk = kir::entry_valid(task.g, task.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(valid_tk);
    }

    const auto bad = [&](const char* stage_key, const char* section) {
        containers::String t(&alloc);
        t.append(stage_key);
        t.append(kScene);
        t.append(section);
        vc::VertexProgramDesc dd(&alloc);
        containers::String    w(&alloc);
        return vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), dd, &w);
    };
    // ⛔ A budget above what a mesh pipeline can promise writes past its own output arrays.
    CHECK(bad("stage = \"mesh\"\n", "\n[mesh]\nmax_vertices = 9999\n") == vc::VertexCookError::BadMesh);
    // ⛔ REN-38-F6: budgets that DISAGREE with the grid mechanic (vertices != 3 * primitives) leave vertices no
    // primitive indexes, or primitives indexing vertices that never existed — refused, not rendered wrong.
    CHECK(bad("stage = \"mesh\"\n", "\n[mesh]\nmax_vertices = 64\nmax_primitives = 42\nworkgroup = 32\n")
          == vc::VertexCookError::BadMesh);
    // ⛔ …and an amplification factor of ZERO launches no work at all: an empty frame with no error anywhere.
    CHECK(bad("stage = \"task\"\n", "\n[task]\nemit = 0\n") == vc::VertexCookError::BadTask);
    // ⛔ A mesh stage cannot honour attribute/instance varyings — no vertex record exists behind it. Refused at
    // cook, never silently dropped (kScene's varying set is exactly that).
    Cooked refused(&alloc);
    staged("stage = \"mesh\"\n", "\n[mesh]\nmax_vertices = 126\nmax_primitives = 42\nworkgroup = 32\n", refused);
    CHECK_FALSE(refused.ok);
}

TEST_CASE("REN-38-F3: the RAY-TRACING stages are authored", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⭐ A raygen/miss/closest-hit TRIO from three declarations. ⛔ These are IMPERATIVE entries — a raygen
    // TRACES and STORES, which are effects, not a value — so they take the statement path, not the DAG one.
    const auto rt = [&](const char* stage, Cooked& out) {
        containers::String t(&alloc);
        t.append("schema = 1\nname = \"rt\"\nstage = \"");
        t.append(stage);
        t.append("\"\n\n[rt]\npayload_words = 1\nas_binding = 0\nout_binding = 1\n");
        cook_text(&alloc, t.c_str(), out);
    };
    Cooked rg(&alloc);
    Cooked ms(&alloc);
    Cooked ch(&alloc);
    rt("raygen", rg);
    rt("miss", ms);
    rt("closest_hit", ch);
    REQUIRE(rg.ok);
    REQUIRE(ms.ok);
    REQUIRE(ch.ok);
    CHECK(rg.ve.stage == kir::KStage::RayGen);
    CHECK(ms.ve.stage == kir::KStage::Miss);
    CHECK(ch.ve.stage == kir::KStage::ClosestHit);
    // every one of them is a real statement body, not an empty entry that happens to carry a stage tag
    CHECK(rg.ve.kernel_body_count > 0);
    CHECK(ms.ve.kernel_body_count > 0);
    CHECK(ch.ve.kernel_body_count > 0);
    // ⛔⛔ THE HIT AND MISS RECORDS MUST DIFFER. A miss shader that wrote the same value as the hit one would
    // report every ray as a hit — an image that renders, with no ray-tracing error anywhere to explain it.
    CHECK(has_const(ms.g, -1.0));
    CHECK_FALSE(has_const(ch.g, -1.0));
    // ⛔ tmin is NOT zero: a ray starting exactly on the surface self-intersects at t=0 and every pixel reports
    // a hit against its own geometry.
    CHECK(has_const(rg.g, 0.001));

    // ⛔ The payload width is the pipeline contract the three stages share; outside 1..8 it is refused.
    {
        vc::VertexProgramDesc dd(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(
                  containers::StringView("schema = 1\nname = \"rt\"\nstage = \"raygen\"\n\n[rt]\n"
                                         "payload_words = 0\n"),
                  dd, &w)
              == vc::VertexCookError::BadRt);
    }
}

TEST_CASE("REN-38-F4: GPU-DRIVEN culling is an authored compute stage", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⛔ ROOT KEYS BEFORE the first table, TABLES AFTER — in TOML a bare key written after `[tess]` belongs to
    // that table, so a `[tess]` section placed ahead of `schema` swallows it and the asset reports BadSchema.
    const auto            staged = [&](const char* stage_key, const char* section, Cooked& out) {
        containers::String t(&alloc);
        t.append(stage_key);
        t.append(kScene);
        t.append(section);
        cook_text(&alloc, t.c_str(), out);
    };
    Cooked culled(&alloc);
    Cooked passthrough(&alloc);
    staged("stage = \"cull\"\n", "\n[cull]\nfrustum = true\nworkgroup = 64\n", culled);
    staged("stage = \"cull\"\n", "\n[cull]\nfrustum = false\nworkgroup = 64\n", passthrough);
    REQUIRE(culled.ok);
    REQUIRE(passthrough.ok);
    CHECK(culled.ve.stage == kir::KStage::Compute);
    CHECK(culled.ve.local_size[0] == 64U);
    CHECK(culled.ve.kernel_body_count > 0);
    // ⛔⛔ THE FRUSTUM TEST IS DECLARED, NOT ASSUMED. A cull pass that silently kept everything costs exactly
    // what no culling costs and looks identical on screen — so the two declarations must cook DIFFERENT
    // programs, or the flag was read and ignored.
    CHECK(culled.g.size() > passthrough.g.size());

    {
        containers::String t(&alloc);
        t.append("stage = \"cull\"\n");
        t.append(kScene);
        t.append("\n[cull]\nworkgroup = 0\n");
        vc::VertexProgramDesc dd(&alloc);
        containers::String    w(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), dd, &w)
              == vc::VertexCookError::BadCull);
    }
}

TEST_CASE("REN-38-F5 GATE: a MESH-SHADER pipeline AND a RAY-TRACED pass, assets only",
          "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    // ⭐⭐ THE F BAND’S OWN GATE, verbatim. Four declarations — task, mesh, raygen, miss — and not one line of
    // C++ describing any of them. Before this row every one of these stages needed a hand-written builder.
    const auto one = [&](const char* stage_key, const char* section, bool pulls, Cooked& out) {
        containers::String t(&alloc);
        t.append(stage_key);
        if (pulls) { t.append(kScene); }
        t.append(section);
        cook_text(&alloc, t.c_str(), out);
    };
    Cooked task(&alloc);
    Cooked mesh(&alloc);
    Cooked rg(&alloc);
    Cooked ms(&alloc);
    one("stage = \"task\"\n", "\n[mesh]\nworkgroup = 32\n\n[task]\nemit = 4\n", true, task);
    one(kMeshlet, "", false, mesh);
    one("schema = 1\nname = \"rt\"\nstage = \"raygen\"\n\n[rt]\npayload_words = 2\n", "", false, rg);
    one("schema = 1\nname = \"rt\"\nstage = \"miss\"\n\n[rt]\npayload_words = 2\n", "", false, ms);
    REQUIRE(task.ok);
    REQUIRE(mesh.ok);
    REQUIRE(rg.ok);
    REQUIRE(ms.ok);

    // the MESH pipeline: amplification feeding meshlet emission
    CHECK(task.ve.stage == kir::KStage::Task);
    CHECK(mesh.ve.stage == kir::KStage::Mesh);
    CHECK(mesh.ve.mesh_vertices > 0U);
    CHECK(mesh.ve.mesh_prim >= 0);
    // ⭐ REN-38-F6: the varyings are the AUTHORED `node:`/`clip.w` terms at their declared locations — a fragment
    // program is cooked against THIS declaration's contract (38-D4), not against the vertex path's. The old
    // claim ("the same declared varying set as the vertex path") was exactly the device-impossible part: a mesh
    // stage has no vertex record to source attribute varyings from.
    REQUIRE(mesh.ve.n_out == 1);
    CHECK(mesh.ve.out[0].location == 0);
    // ⛔⛔ …and every one of the four is an entry a backend can actually CREATE.
    CHECK(kir::entry_valid(task.g, task.ve));
    CHECK(kir::entry_valid(mesh.g, mesh.ve));
    CHECK(kir::entry_valid(rg.g, rg.ve));
    CHECK(kir::entry_valid(ms.g, ms.ve));

    // the RAY-TRACED pass: a raygen that traces, and a miss that answers
    CHECK(rg.ve.stage == kir::KStage::RayGen);
    CHECK(ms.ve.stage == kir::KStage::Miss);
    CHECK(rg.ve.kernel_body_count > 0);
    CHECK(ms.ve.kernel_body_count > 0);

    // ⛔ The STAGE is part of the program identity. Two stages of one declaration are different programs, and a
    // cache that folded them would hand a mesh pipeline the task program.
    vc::VertexProgramDesc a(&alloc);
    vc::VertexProgramDesc b(&alloc);
    containers::String    w(&alloc);
    containers::String    ta(&alloc);
    containers::String    tb(&alloc);
    ta.append("stage = \"mesh\"\n");
    ta.append(kScene);
    tb.append("stage = \"task\"\n");
    tb.append(kScene);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(ta.c_str(), ta.size()), a, &w)
            == vc::VertexCookError::Ok);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(tb.c_str(), tb.size()), b, &w)
            == vc::VertexCookError::Ok);
    CHECK(a.stage != b.stage);
}

TEST_CASE("REN-38 audit: the LIVE varying contract -- a cooked FS read set verifies against the .crdv",
          "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U, nullptr, "vc-live-contract");
    // A tiny fragment entry that reads location 0 as a SMOOTH vec3 and location 3 as a SMOOTH vec2 — the shape
    // of the real scene FS's normal + uv reads, built directly so the derivation is judged on its own.
    kir::KGraph g(&alloc);
    kir::KEntry fe;
    fe.stage       = kir::KStage::Fragment;
    const int nrm  = g.stage_in(kir::KType::vec(kir::DType::F32, 3), 0, kir::Interp::Smooth);
    const int uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 3, kir::Interp::Smooth);
    const int lum  = g.dot(nrm, nrm);
    const int col  = g.vec_concat(g.vec_concat(g.splat(lum, 2), uv),
                                  g.vec_concat(g.constant(0.0, kir::make_shape({1}), kir::DType::F32),
                                               g.constant(1.0, kir::make_shape({1}), kir::DType::F32)));
    fe.n_out  = 1;
    fe.out[0] = {col, 0, kir::Interp::Smooth};

    vc::VaryingRequirement reqs[vc::kMaxVaryings];
    crd::u32               n_reqs = 0U;
    REQUIRE(vc::fs_varying_requirements(g, fe, static_cast<vc::VaryingRequirement*>(reqs), vc::kMaxVaryings,
                                        &n_reqs, &alloc));
    REQUIRE(n_reqs == 2U);

    // The real scene declaration emits 0..3 — a superset of the read set — and must PASS by location.
    vc::VertexProgramDesc full(&alloc);
    containers::String    w(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), full, &w) == vc::VertexCookError::Ok);
    CHECK(vc::verify_varying_contract(full, static_cast<const vc::VaryingRequirement*>(reqs), n_reqs, &w)
          == vc::VertexCookError::Ok);

    // ⛔⛔ THE SKINNED-VS SCAR, replayed against the live check: a declaration that stops after two varyings must
    // be REFUSED — before this check ran at the join, that pair linked, bound and shaded from undefined
    // interpolants at locations 2 and 3, and no validation layer on either backend can see it.
    vc::VertexProgramDesc trimmed(&alloc);
    containers::String    tr(&alloc);
    tr.append(kScene);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(tr.c_str(), tr.size()), trimmed, &w)
            == vc::VertexCookError::Ok);
    while (trimmed.varyings.size() > 2U) { trimmed.varyings.pop_back(); }
    CHECK(vc::verify_varying_contract(trimmed, static_cast<const vc::VaryingRequirement*>(reqs), n_reqs, &w)
          == vc::VertexCookError::ContractMismatch);

    // A location read at TWO widths is a fragment program that disagrees with itself — the derivation itself
    // refuses, so the contract check can never blame the .crdv for it.
    const int nrm2 = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0, kir::Interp::Smooth);
    fe.out[0]      = {g.vec_concat(nrm, g.dot(nrm2, nrm2)), 0, kir::Interp::Smooth};
    crd::u32 n2 = 0U;
    CHECK_FALSE(vc::fs_varying_requirements(g, fe, static_cast<vc::VaryingRequirement*>(reqs), vc::kMaxVaryings,
                                            &n2, &alloc));
}

TEST_CASE("REN-38 audit: the ANY-HIT stage cooks from the declaration and its cutoff reaches the graph",
          "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U, nullptr, "vc-anyhit");
    containers::String    w(&alloc);
    const auto            cook_ah = [&](const char* rt_section, kir::KGraph& g, kir::KEntry& e) {
        containers::String t(&alloc);
        t.append("stage = \"any_hit\"\n");
        t.append(kScene);
        t.append(rt_section);
        vc::VertexProgramDesc d(&alloc);
        if (vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &w) != vc::VertexCookError::Ok)
        {
            return false;
        }
        return vc::cook_vertex_program(d, g, e);
    };

    // The declared any-hit cooks to an ANY-HIT entry with a real IGNORE body.
    kir::KGraph g1(&alloc);
    kir::KEntry e1;
    REQUIRE(cook_ah("\n[rt]\npayload_words = 1\nalpha_cutoff = 0.25\n", g1, e1));
    CHECK(e1.stage == kir::KStage::AnyHit);
    CHECK(e1.kernel_body_count > 0);

    // ⛔ The CUTOFF is the declaration's whole point, so the gate asserts it REACHED the graph: two cutoffs
    // must cook different content (a declared-but-ignored cutoff cooks identical bytes — the E5 lesson).
    kir::KGraph g2(&alloc);
    kir::KEntry e2;
    REQUIRE(cook_ah("\n[rt]\npayload_words = 1\nalpha_cutoff = 0.75\n", g2, e2));
    const crd::containers::Array<crd::u8> b1 = kir::serialize_graph(g1, e1, &alloc);
    const crd::containers::Array<crd::u8> b2 = kir::serialize_graph(g2, e2, &alloc);
    bool same = b1.size() == b2.size();
    if (same)
    {
        for (crd::usize i = 0; i < b1.size(); ++i)
        {
            if (b1[i] != b2[i]) { same = false; break; }
        }
    }
    CHECK_FALSE(same);

    // A cutoff outside [0, 2] can never change a hit (u+v spans 0..1) — refused, never obeyed.
    kir::KGraph g3(&alloc);
    kir::KEntry e3;
    CHECK_FALSE(cook_ah("\n[rt]\npayload_words = 1\nalpha_cutoff = 3.0\n", g3, e3));

    // The editor round trip preserves the declaration: parse -> emit -> parse keeps stage AND cutoff.
    containers::String t(&alloc);
    t.append("stage = \"any_hit\"\n");
    t.append(kScene);
    t.append("\n[rt]\npayload_words = 1\nalpha_cutoff = 0.25\n");
    vc::VertexProgramDesc d(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &w) == vc::VertexCookError::Ok);
    const containers::String  emitted = vc::emit_vertex_toml(d, &alloc);
    vc::VertexProgramDesc     re(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(emitted.c_str(), emitted.size()), re, &w)
            == vc::VertexCookError::Ok);
    CHECK(re.stage == vc::StageKind::AnyHit);
    CHECK(re.rt.alpha_cutoff == 0.25);
}

TEST_CASE("REN-38-F7: a PROCEDURAL vertex stage -- node-computed clip over an expansion contract",
          "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    // ⭐⭐ THE VOCABULARY THAT RETIRES THE C++ BUILDERS: `position = "node:…"` + `[expand]`. The first fixture
    // is the corner-table quad VS (the B4-tess control-point generator) as ifequal chains over `@corner`; the
    // second pulls a per-instance record (`field:`), unpacks a packed colour (`fieldc:`), projects through the
    // header matrix (`view_proj`) and applies the category mask (`@category`) — the debug-draw contract.
    constexpr const char* quad_vs = R"(
schema   = 1
name     = "crd://vertex/quad_corners"
position = "node:clip"

[expand]
verts_per_instance = 4

[[node]]
name   = "x_hi"
op     = "ifequal"
inputs = ["@corner", 2.0, 0.6, -0.6]

[[node]]
name   = "x"
op     = "ifequal"
inputs = ["@corner", 1.0, 0.6, "x_hi"]

[[node]]
name   = "y"
op     = "ifgreater"
inputs = ["@corner", 1.5, 0.6, -0.6]

[[node]]
name   = "clip"
op     = "combine4"
inputs = ["x", "y", 0.0, 1.0]
)";
    Cooked quad(&alloc);
    cook_text(&alloc, quad_vs, quad);
    REQUIRE(quad.ok);
    CHECK(quad.ve.stage == kir::KStage::Vertex);
    REQUIRE(quad.ve.position >= 0);
    CHECK(quad.g.node(quad.ve.position).comps() == 4);
    CHECK(reads_builtin(quad.g, kir::KBuiltin::VertexIndex));
    // a pure corner table touches NO storage — it can feed `draw_tess`, which binds no buffer at all
    CHECK_FALSE(has_op(quad.g, kir::KOp::StorageLoad));
    {
        const char* why      = nullptr;
        const bool  valid_pq = kir::entry_valid(quad.g, quad.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(valid_pq);
    }

    constexpr const char* field_vs = R"(
schema   = 1
name     = "crd://vertex/field_tri"
position = "node:clip"

[expand]
verts_per_instance = 3
instance_words     = 11
instance_off       = 32
category_field     = 10
category_mask_word = 18

[[node]]
name   = "p"
op     = "combine3"
inputs = ["field:0", "field:1", "field:2"]

[[node]]
name   = "wp"
op     = "view_proj"
inputs = ["p"]

[[node]]
name   = "tint"
op     = "multiply"
inputs = ["fieldc:9", [1.0, 1.0, 1.0, 1.0]]

[[node]]
name   = "vis"
op     = "multiply"
inputs = ["@category", 1.0]

[[node]]
name   = "vis4"
op     = "convert_f_vec"
inputs = ["vis", 4.0]

[[node]]
name   = "clip"
op     = "multiply"
inputs = ["wp", "vis4"]

[[varying]]
name       = "tint"
location   = 0
interp     = "smooth"
source     = ["node:tint"]
node_comps = [4]
)";
    Cooked fld(&alloc);
    cook_text(&alloc, field_vs, fld);
    REQUIRE(fld.ok);
    REQUIRE(fld.ve.position >= 0);
    CHECK(fld.g.node(fld.ve.position).comps() == 4);
    CHECK(has_op(fld.g, kir::KOp::StorageLoad)); // the record pull
    CHECK(has_op(fld.g, kir::KOp::BitAnd));      // the category scheme + the colour unpack
    REQUIRE(fld.ve.n_out == 1);
    CHECK(fld.ve.out[0].location == 0);
    {
        const char* why      = nullptr;
        const bool  valid_pf = kir::entry_valid(fld.g, fld.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(valid_pf);
    }

    // ⛔ the declaration SURVIVES the editor round trip — canonical emit is a fixed point
    {
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(field_vs), d, &w) == vc::VertexCookError::Ok);
        CHECK(d.expand.verts_per_instance == 3U);
        CHECK(d.expand.instance_words == 11U);
        CHECK(d.expand.instance_off == 32U);
        CHECK(d.expand.has_category);
        containers::String e1 = vc::emit_vertex_toml(d, &alloc);
        vc::VertexProgramDesc d2(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(e1.c_str(), e1.size()), d2, &w)
                == vc::VertexCookError::Ok);
        containers::String e2 = vc::emit_vertex_toml(d2, &alloc);
        REQUIRE(e1.size() == e2.size());
        CHECK(memcmp(e1.c_str(), e2.c_str(), e1.size()) == 0);
    }

    // ⛔ every way this vocabulary can lie is a NAMED refusal
    const auto perr = [&](const char* toml) {
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        return vc::parse_vertex_toml(containers::StringView(toml), d, &w);
    };
    using E = vc::VertexCookError;
    // a position key that is not `node:…`
    CHECK(perr("schema = 1\nname = \"p\"\nposition = \"clip\"\n") == E::BadPositionNode);
    // a position node that does not exist
    CHECK(perr("schema = 1\nname = \"p\"\nposition = \"node:nosuch\"\n") == E::BadPositionNode);
    // a record term outside the declared record (word 11 of an 11-word record)
    CHECK(perr("schema = 1\nname = \"p\"\nposition = \"node:c\"\n"
               "[expand]\nverts_per_instance = 3\ninstance_words = 11\n"
               "[[node]]\nname = \"c\"\nop = \"multiply\"\ninputs = [\"field:11\", 1.0]\n")
          == E::BadExpand);
    // a procedural input in a PULL declaration (no position node to give it meaning)
    {
        containers::String t(&alloc);
        t.append(kScene);
        t.append("\n[[node]]\nname = \"c\"\nop = \"multiply\"\ninputs = [\"@corner\", 1.0]\n");
        CHECK(perr(t.c_str()) == E::BadExpand);
    }
    // `view_proj` outside the procedural mode — the pull path projects via its own transform contract
    {
        containers::String t(&alloc);
        t.append(kScene);
        t.append("\n[[node]]\nname = \"c\"\nop = \"view_proj\"\ninputs = [\"@position\"]\n");
        CHECK(perr(t.c_str()) == E::UnknownOp);
    }
    // a non-vertex stage with a position node — every other stage has its own position contract
    CHECK(perr("schema = 1\nname = \"p\"\nstage = \"mesh\"\nposition = \"node:c\"\n"
               "[[node]]\nname = \"c\"\nop = \"multiply\"\ninputs = [1.0, 1.0]\n")
          == E::BadPositionNode);
    // a malformed word index is a refusal, not a silent word 0
    CHECK(perr("schema = 1\nname = \"p\"\nposition = \"node:c\"\n"
               "[[node]]\nname = \"c\"\nop = \"multiply\"\ninputs = [\"field:abc\", 1.0]\n")
          == E::BadExpand);
}

TEST_CASE("REN-38-F15: GARBAGE text is a named parse refusal, never a crash", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(4U << 20U);
    vc::VertexProgramDesc d(&alloc);
    containers::String    w(&alloc);
    CHECK(vc::parse_vertex_toml(containers::StringView("this is not a vertex declaration"), d, &w)
          == vc::VertexCookError::ParseFailed);
}


TEST_CASE("REN-38-F13: INTERSECTION and CALLABLE are authored stages", "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(8U << 20U);
    const auto rt = [&](const char* stage, const char* extra, Cooked& out) {
        containers::String t(&alloc);
        t.append("schema = 1\nname = \"rt\"\nstage = \"");
        t.append(stage);
        t.append("\"\n\n[rt]\npayload_words = 2\n");
        t.append(extra);
        cook_text(&alloc, t.c_str(), out);
    };
    Cooked is(&alloc);
    Cooked cl(&alloc);
    rt("intersection", "sphere_radius = 0.5\n", is);
    rt("callable", "callable_scale = 2.0\ncallable_bias = 1.0\n", cl);
    REQUIRE(is.ok);
    REQUIRE(cl.ok);
    CHECK(is.ve.stage == kir::KStage::Intersection);
    CHECK(cl.ve.stage == kir::KStage::Callable);
    CHECK(is.ve.kernel_body_count > 0);
    CHECK(cl.ve.kernel_body_count > 0);
    // the intersection tests the OBJECT ray its stage can actually read, and both entries are device-creatable
    CHECK(reads_builtin(is.g, kir::KBuiltin::ObjectRayOrigin));
    CHECK(reads_builtin(is.g, kir::KBuiltin::ObjectRayDirection));
    {
        const char* why   = nullptr;
        const bool  ok_is = kir::entry_valid(is.g, is.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(ok_is);
    }
    {
        const char* why   = nullptr;
        const bool  ok_cl = kir::entry_valid(cl.g, cl.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(ok_cl);
    }
    // ⛔ the [rt] section ROUND-TRIPS for the two newest stages (the emit switch had them falling out of the
    // RT arm — a canonical form that silently lost sphere_radius / callable_scale, the fields these stages
    // exist for; the inert-copy-rot scar on the emit side)
    {
        vc::VertexProgramDesc di(&alloc);
        containers::String    wi(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView("schema = 1\nname = \"rt\"\nstage = "
                                                             "\"intersection\"\n\n[rt]\npayload_words = "
                                                             "2\nsphere_radius = 0.75\n"),
                                      di, &wi)
                == vc::VertexCookError::Ok);
        containers::String    ei = vc::emit_vertex_toml(di, &alloc);
        vc::VertexProgramDesc di2(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(ei.c_str(), ei.size()), di2, &wi)
                == vc::VertexCookError::Ok);
        CHECK(di2.rt.sphere_radius == 0.75);
        vc::VertexProgramDesc dc(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView("schema = 1\nname = \"rt\"\nstage = "
                                                             "\"callable\"\n\n[rt]\npayload_words = "
                                                             "2\ncallable_scale = 3.0\ncallable_bias = 0.5\n"),
                                      dc, &wi)
                == vc::VertexCookError::Ok);
        containers::String    ec = vc::emit_vertex_toml(dc, &alloc);
        vc::VertexProgramDesc dc2(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(ec.c_str(), ec.size()), dc2, &wi)
                == vc::VertexCookError::Ok);
        CHECK(dc2.rt.callable_scale == 3.0);
        CHECK(dc2.rt.callable_bias == 0.5);
    }
    // a sphere of no radius reports no hit for any ray — refused by name
    vc::VertexProgramDesc d(&alloc);
    containers::String    w(&alloc);
    CHECK(vc::parse_vertex_toml(
              containers::StringView(
                  "schema = 1\nname = \"rt\"\nstage = \"intersection\"\n\n[rt]\nsphere_radius = 0.0\n"),
              d, &w)
          == vc::VertexCookError::BadRt);
}


// ── ⭐⭐ REN-38-F6+: the MESHLET-FETCH mesh stage and the GPU-DRIVEN task count. ─────────────────────────────
// The F6 rewrite made mesh/task procedural because their emitters lowered no storage loads. They now lower the
// sbuf seam on both backends, so `fetch = true` pulls REAL geometry through the SAME scene contract as the
// pulling VS, and `emit_header = N` reads the amplification count from a header word at dispatch time.
TEST_CASE("REN-38-F6+: a FETCH mesh stage pulls real geometry and a task reads its count from the buffer",
          "[vertex-cook][ren38]")
{
    memory::TlsfAllocator alloc(32U << 20U);
    constexpr const char* fetch_toml = R"(
schema = 1
name   = "crd://vertex/meshfetch"
stage  = "mesh"

[vertex]
stride = 3

[[attribute]]
name   = "position"
offset = 0
comps  = 3
kind   = "position"

[instance]
stride    = 20
transform = 0

[mesh]
max_vertices   = 6
max_primitives = 2
workgroup      = 6
fetch          = true
)";
    Cooked fetch(&alloc);
    cook_text(&alloc, fetch_toml, fetch);
    REQUIRE(fetch.ok);
    CHECK(fetch.ve.stage == kir::KStage::Mesh);
    CHECK(fetch.ve.mesh_vertices == 6U);
    // ⭐ THE PULL IS REAL: the graph reads the storage buffer through the workgroup builtins its stage can
    // lower — and never through VertexIndex, which no mesh emitter has.
    CHECK(has_op(fetch.g, kir::KOp::StorageLoad));
    CHECK(reads_builtin(fetch.g, kir::KBuiltin::LocalInvocationIndex));
    CHECK(reads_builtin(fetch.g, kir::KBuiltin::WorkgroupIndex));
    CHECK_FALSE(reads_builtin(fetch.g, kir::KBuiltin::VertexIndex));
    {
        const char* why  = nullptr;
        const bool  okay = kir::entry_valid(fetch.g, fetch.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(okay);
    }

    // ── the GPU-driven task: `emit_header` replaces the constant with a header-word read ──
    constexpr const char* task_toml = R"(
schema = 1
name   = "crd://vertex/taskhdr"
stage  = "task"

[mesh]
workgroup = 1

[task]
emit        = 1
emit_header = 130
)";
    Cooked task(&alloc);
    cook_text(&alloc, task_toml, task);
    REQUIRE(task.ok);
    CHECK(task.ve.stage == kir::KStage::Task);
    REQUIRE(task.ve.task_emit >= 0);
    CHECK(has_op(task.g, kir::KOp::StorageLoad)); // the count comes from the BUFFER, not the constant
    {
        const char* why  = nullptr;
        const bool  okay = kir::entry_valid(task.g, task.ve, &why);
        INFO((why == nullptr ? "" : why));
        CHECK(okay);
    }

    // ── the vocabulary ROUND-TRIPS (the inert-copy-rot rule: the canonical form carries every field) ──
    {
        vc::VertexProgramDesc d(&alloc);
        containers::String    w(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(fetch_toml), d, &w) == vc::VertexCookError::Ok);
        containers::String    e1 = vc::emit_vertex_toml(d, &alloc);
        vc::VertexProgramDesc d2(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(e1.c_str(), e1.size()), d2, &w)
                == vc::VertexCookError::Ok);
        CHECK(d2.mesh.fetch);
        vc::VertexProgramDesc t1(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(task_toml), t1, &w) == vc::VertexCookError::Ok);
        containers::String    e2 = vc::emit_vertex_toml(t1, &alloc);
        vc::VertexProgramDesc t2(&alloc);
        REQUIRE(vc::parse_vertex_toml(containers::StringView(e2.c_str(), e2.size()), t2, &w)
                == vc::VertexCookError::Ok);
        CHECK(t2.task.emit_header == 130);
    }

    // ── the refusals: pulling with nothing to pull, and a count word outside any sane header ──
    const auto err_of = [&](const char* toml) {
        vc::VertexProgramDesc dd(&alloc);
        containers::String    w(&alloc);
        return vc::parse_vertex_toml(containers::StringView(toml), dd, &w);
    };
    // no [vertex] record at all -> nothing to fetch
    constexpr const char* no_record = R"(
schema = 1
name   = "x"
stage  = "mesh"

[mesh]
max_vertices   = 6
max_primitives = 2
workgroup      = 6
fetch          = true
)";
    CHECK(err_of(no_record) == vc::VertexCookError::BadMesh);
    // a vertex record with no position attribute -> nothing to place
    constexpr const char* no_pos = R"(
schema = 1
name   = "x"
stage  = "mesh"

[vertex]
stride = 2

[[attribute]]
name   = "uv"
offset = 0
comps  = 2
kind   = "value"

[mesh]
max_vertices   = 6
max_primitives = 2
workgroup      = 6
fetch          = true
)";
    CHECK(err_of(no_pos) == vc::VertexCookError::BadMesh);
    // an emit_header outside any sane header -> refused at cook, never a device-side garbage dispatch
    constexpr const char* wild_hdr = R"(
schema = 1
name   = "x"
stage  = "task"

[task]
emit        = 1
emit_header = 5000
)";
    CHECK(err_of(wild_hdr) == vc::VertexCookError::BadTask);
}

// ── ⭐⭐ REN-39-B2 GATE: the INDEXED pull mode — the load-set diff is EXACTLY the documented one. ────────────
// `indexed = true` re-addresses the pull chain for an indexed draw: the per-vertex `indices[]` load is GONE
// (the IA did it — VertexIndex arrives as the index VALUE) and the `vid / index_count` division is GONE (the
// instance rides InstanceIndex). The gate cooks the SAME declaration both ways and asserts the diff by graph
// STRUCTURE, plus the identity/round-trip/refusal contracts. ⛔ STATED, NOT ASSUMED: the vertex stage has no
// CPU oracle (eval_cpu has no Builtin/StorageLoad arms), so numerical parity is proven ON DEVICE by the
// pull-vs-indexed bit-identical pixel gate in the Vulkan suite — the proven pull mode IS the reference.
TEST_CASE("REN-39-B2: `indexed = true` drops exactly the index load and the instance division", "[vertex-cook][ren39]")
{
    memory::TlsfAllocator alloc(16U << 20U);

    containers::String pull_toml(&alloc);
    pull_toml.append(kScene);
    containers::String idx_toml(&alloc);
    idx_toml.append("indexed = true\n");
    idx_toml.append(kScene);

    Cooked pull(&alloc);
    cook_text(&alloc, pull_toml.c_str(), pull);
    REQUIRE(pull.ok);
    Cooked idx(&alloc);
    cook_text(&alloc, idx_toml.c_str(), idx);
    REQUIRE(idx.ok);
    CHECK(kir::entry_valid(pull.g, pull.ve));
    CHECK(kir::entry_valid(idx.g, idx.ve));

    // the DOCUMENTED diff, by structure: the division is gone, InstanceIndex appears, VertexIndex stays
    CHECK(has_op(pull.g, kir::KOp::Div));
    CHECK_FALSE(has_op(idx.g, kir::KOp::Div));
    CHECK_FALSE(reads_builtin(pull.g, kir::KBuiltin::InstanceIndex));
    CHECK(reads_builtin(idx.g, kir::KBuiltin::InstanceIndex));
    CHECK(reads_builtin(pull.g, kir::KBuiltin::VertexIndex));
    CHECK(reads_builtin(idx.g, kir::KBuiltin::VertexIndex));

    // …and EXACTLY THREE storage loads disappear: the per-vertex index fetch AND the two header words only it
    // referenced — `index_count` (the divisor) and `index_off` (the section base). The indexed chain reads
    // neither: the IA owns the section and InstanceIndex owns the instance.
    const auto count_loads = [](const kir::KGraph& g)
    {
        int n = 0;
        for (int i = 0; i < g.size(); ++i)
        {
            if (g.node(i).op == kir::KOp::StorageLoad)
            {
                ++n;
            }
        }
        return n;
    };
    CHECK(count_loads(idx.g) == count_loads(pull.g) - 3);
}

TEST_CASE("REN-39-B2: `indexed` is cooked identity, survives the round-trip, and composes with rebase",
          "[vertex-cook][ren39]")
{
    memory::TlsfAllocator alloc(16U << 20U);

    vc::VertexProgramDesc desc(&alloc);
    containers::String where(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(kScene), desc, &where) == vc::VertexCookError::Ok);

    // ── layout identity: pull and indexed cooks of ONE asset are DIFFERENT programs ──
    const crd::u64 id_pull = vc::vertex_layout_id(desc);
    desc.indexed = true;
    const crd::u64 id_idx = vc::vertex_layout_id(desc);
    CHECK(id_pull != id_idx);

    // ⛔⛔ the identity fix this slice surfaced: rebase_table and instance_capacity_word change EVERY load
    // address and were NOT hashed — a rebased and an absolute cook of one asset COLLIDED to one variant key.
    desc.rebase_table = 120U;
    const crd::u64 id_rebase = vc::vertex_layout_id(desc);
    CHECK(id_rebase != id_idx);
    desc.instance_capacity_word = 99U;
    CHECK(vc::vertex_layout_id(desc) != id_rebase);

    // ── the FIELD-SURVIVAL round-trip (parse → emit → parse), covering the two fields the emitter DROPPED ──
    desc.cascade = 2U;
    const containers::String out = vc::emit_vertex_toml(desc, &alloc);
    vc::VertexProgramDesc back(&alloc);
    REQUIRE(vc::parse_vertex_toml(containers::StringView(out.c_str(), out.size()), back, &where) ==
            vc::VertexCookError::Ok);
    CHECK(back.indexed);
    CHECK(back.rebase_table == 120U);
    CHECK(back.instance_capacity_word == 99U);
    CHECK(back.cascade == 2U);

    // ── `indexed` composes UNCHANGED with the rebase chain: DrawIndex + rebased loads + no division ──
    Cooked reb(&alloc);
    {
        containers::String t(&alloc);
        t.append("indexed = true\nrebase_table = 120\n");
        t.append(kScene);
        cook_text(&alloc, t.c_str(), reb);
    }
    REQUIRE(reb.ok);
    CHECK(reads_builtin(reb.g, kir::KBuiltin::DrawIndex));
    CHECK(reads_builtin(reb.g, kir::KBuiltin::InstanceIndex));
    CHECK_FALSE(has_op(reb.g, kir::KOp::Div));

    // ── refusals: `indexed` on a procedural or non-vertex stage is REFUSED BY NAME ──
    {
        containers::String t(&alloc);
        t.append("indexed = true\nstage = \"mesh\"\n");
        t.append(kScene);
        vc::VertexProgramDesc d(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &where) ==
              vc::VertexCookError::BadIndexed);
    }
    {
        containers::String t(&alloc);
        t.append("indexed = true\nposition  = \"node:p\"\n");
        t.append(kScene);
        t.append("\n[[node]]\nname   = \"p\"\nop     = \"combine4\"\ninputs = [\"@corner\", \"@corner\", "
                 "\"@corner\", \"@corner\"]\n");
        vc::VertexProgramDesc d(&alloc);
        CHECK(vc::parse_vertex_toml(containers::StringView(t.c_str(), t.size()), d, &where) ==
              vc::VertexCookError::BadIndexed);
    }
}
