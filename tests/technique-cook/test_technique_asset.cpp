// test_technique_asset.cpp — REN-37.2/37.3/37.4 GATE (D-007 row 140): the LIGHTING TECHNIQUE as an authored asset.
//
// Four claims, each load-bearing for "only authored frame graphs" reaching the FRAGMENT SHADER:
//   1. A valid `.crdt` parses, cooks, and ROUND-TRIPS BYTE-IDENTICALLY (canonical, packed, padding-free — the
//      `ckir_serialize` lesson), and `parse -> emit -> parse -> cook` equals `parse -> cook`.
//   2. Every way a technique can be malformed is REJECTED BY NAME AT COOK TIME.
//   3. ⭐ THE BINDING CONTRACT: a frame-graph pass that names a technique but does not `read` its declared
//      PASS-frequency inputs is rejected — including the SHAPE check that catches a `texture2DArrayShadow` wired
//      to a single-layer resource, which otherwise renders every cascade from slice 0 and looks like art.
//   4. ⭐⭐ THE SPLICE: a technique body shipped as a SERIALIZED CKIR GRAPH inlines into a host graph and
//      evaluates IDENTICALLY to the registered C++ body. That equality is what makes "author a brand-new
//      technique with no engine recompile" a fact rather than a claim.

#include <crd/techniquecook/technique_asset.hpp>

#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_serialize.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace tc = crd::techniquecook;
namespace fc = crd::framecook;
namespace tq = crd::kir::technique;

namespace
{
constexpr const char* kValidTechnique = R"(
schema  = 1
name    = "forward_csm"
surface = "OpenPBRSurface"
body    = "builtin:forward_csm"

[[binding]]
name      = "shadow_atlas"
type      = "texture2DArrayShadow"
frequency = "pass"

[[binding]]
name      = "csm_light_vp"
type      = "mat4[]"
count     = 4
frequency = "pass"

[[binding]]
name      = "csm_map_size"
type      = "float"
frequency = "pass"

[[option]]
name    = "cascade_count"
min     = 1
max     = 4
default = 4

[[option]]
name    = "pcf_taps"
min     = 1
max     = 16
default = 4
)";

// A frame graph whose forward pass names `forward_csm` and reads all three of its declared inputs.
constexpr const char* kGraphWithTechnique = R"(
schema = 1
name   = "forward_csm"

[[resource]]
name    = "shadow_atlas"
format  = "D32Float"
width   = 2048
height  = 2048
layers  = 4
sampled = true

[[draw_list]]
name = "visible"
all  = ["MeshRenderer", "Transform"]

[[pass]]
name          = "cascade"
kind          = "raster.depth_only"
writes        = ["shadow_atlas[$index]"]
draw_list     = "visible"
for_each      = "light.0.cascades"
material_pass = "Shadow"
clear_depth   = 1.0

[[pass]]
name          = "forward"
kind          = "raster.geometry"
reads         = ["shadow_atlas"]
writes        = ["@output"]
draw_list     = "visible"
technique     = "forward_csm"
material_pass = "Forward"
)";

// Evaluate one scalar node of a graph with NO live inputs — after a splice every `Input` leaf has been
// substituted, so the graph is closed and the CPU oracle needs no feed.
[[nodiscard]] crd::f64 eval_closed(const crd::kir::KGraph& g, int node, crd::memory::IAllocator* a)
{
    crd::f64 out = 0.0;
    crd::kir::eval_cpu(g, nullptr, a, node, &out);
    return out;
}

[[nodiscard]] bool bytes_equal(const crd::containers::Array<crd::u8>& a, const crd::containers::Array<crd::u8>& b)
{
    if (a.size() != b.size()) { return false; }
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        if (a[i] != b[i]) { return false; }
    }
    return true;
}

// Find the pass named `n` in a parsed graph.
[[nodiscard]] const fc::FramePassDesc* find_pass(const fc::FrameGraphDesc& g, const char* n)
{
    for (crd::usize i = 0; i < g.passes.size(); ++i)
    {
        crd::usize k = 0;
        while (n[k] != '\0' && k < g.passes[i].name.size() && g.passes[i].name.c_str()[k] == n[k]) { ++k; }
        if (n[k] == '\0' && k == g.passes[i].name.size()) { return &g.passes[i]; }
    }
    return nullptr;
}
} // namespace

TEST_CASE("technique asset parses cooks and round-trips byte-identically", "[technique][ren37]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "technique-cook-test");
    tc::TechniqueDesc          d(&alloc);
    crd::containers::String    where(&alloc);

    REQUIRE(tc::parse_technique_toml(crd::containers::StringView(kValidTechnique), d, &where)
            == tc::TechniqueCookError::Ok);
    REQUIRE(d.bindings.size() == 3U);
    REQUIRE(d.options.size() == 2U);
    CHECK(d.body_kind == tc::TechniqueBodyKind::Builtin);
    CHECK(d.bindings[0].type == tq::BindType::Texture2DArrayShadow);
    CHECK(d.bindings[1].type == tq::BindType::Mat4Array);
    CHECK(d.bindings[1].count == 4U);
    CHECK(d.bindings[2].freq == tq::BindFrequency::Pass);

    // ⭐ The ABI the splice depends on: a texture is TWO nodes (texture + sampler), an array is `count`, a scalar
    // is one. 2 + 4 + 1 = 7 — and `forward_csm`'s body asserts exactly that count.
    crd::u32 nodes = 0;
    for (crd::usize i = 0; i < d.bindings.size(); ++i)
    {
        nodes += tq::bind_type_node_count(d.bindings[i].type, d.bindings[i].count);
    }
    CHECK(nodes == 7U);

    const crd::containers::Array<crd::u8> blob = tc::cook_technique(d, &alloc);
    tc::TechniqueDesc                     back(&alloc);
    REQUIRE(tc::read_technique(crd::containers::ConstSpan<crd::u8>(blob.data(), blob.size()), back));
    CHECK(tc::validate_technique(back) == tc::TechniqueCookError::Ok);
    CHECK(bytes_equal(blob, tc::cook_technique(back, &alloc)));

    // The EDITOR ROUND-TRIP: emit -> parse -> cook must reproduce the same bytes, or a node-editor save could
    // silently drop a field.
    const crd::containers::String emitted = tc::emit_technique_toml(d, &alloc);
    tc::TechniqueDesc             reparsed(&alloc);
    REQUIRE(tc::parse_technique_toml(crd::containers::StringView(emitted.c_str(), emitted.size()), reparsed, &where)
            == tc::TechniqueCookError::Ok);
    CHECK(bytes_equal(blob, tc::cook_technique(reparsed, &alloc)));
}

TEST_CASE("technique asset rejects every malformed shape by name", "[technique][ren37]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "technique-reject-test");
    crd::containers::String    where(&alloc);
    const auto                 err = [&](const char* toml) {
        tc::TechniqueDesc d(&alloc);
        return tc::parse_technique_toml(crd::containers::StringView(toml), d, &where);
    };

    CHECK(err("name = \"x\"\nbody = \"builtin:y\"\n") == tc::TechniqueCookError::BadSchema);
    CHECK(err("schema = 1\nbody = \"builtin:y\"\n") == tc::TechniqueCookError::MissingName);
    CHECK(err("schema = 1\nname = \"x\"\n") == tc::TechniqueCookError::MissingBody);
    // a body with no recognized provenance prefix is NOT a body
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"shade_forward\"\n") == tc::TechniqueCookError::MissingBody);
    CHECK(err("schema = 1\nname = \"x\"\nsurface = \"Phong\"\nbody = \"builtin:y\"\n")
          == tc::TechniqueCookError::UnknownSurface);
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[binding]]\nname=\"a\"\ntype=\"quat\"\n")
          == tc::TechniqueCookError::UnknownBindType);
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[binding]]\nname=\"a\"\ntype=\"float\"\nfrequency=\"draw\"\n")
          == tc::TechniqueCookError::UnknownFrequency);
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[binding]]\nname=\"a\"\ntype=\"float\"\n[[binding]]\nname=\"a\"\ntype=\"vec3\"\n")
          == tc::TechniqueCookError::DuplicateBinding);
    // ⛔ an "array" of one is not an array, and a scalar with a count lies about its shape — either silently
    // produces the wrong ABI slot count when the body is spliced.
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[binding]]\nname=\"a\"\ntype=\"mat4[]\"\ncount=1\n")
          == tc::TechniqueCookError::BadArrayCount);
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[binding]]\nname=\"a\"\ntype=\"float\"\ncount=4\n")
          == tc::TechniqueCookError::BadArrayCount);
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[option]]\nname=\"o\"\nmin=4\nmax=1\n")
          == tc::TechniqueCookError::BadOptionRange);
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[option]]\nname=\"o\"\nmin=1\nmax=4\ndefault=9\n")
          == tc::TechniqueCookError::BadOptionRange);
    CHECK(err("schema = 1\nname = \"x\"\nbody = \"builtin:y\"\n[[option]]\nname=\"o\"\nmin=1\nmax=4\n[[option]]\nname=\"o\"\nmin=0\nmax=1\n")
          == tc::TechniqueCookError::DuplicateOption);
}

TEST_CASE("a REUSED descriptor holds ONLY the second asset after a second parse or blob read", "[technique][ren38]")
{
    // ⛔ THE LOAD-BUTTON SCAR (REN-38 audit): parsing into a descriptor that already held a technique APPENDED
    // to it — `DuplicateBinding` naming the wrong thing when names overlapped, a silently MERGED technique when
    // they did not. Any tool with a load button hits this on the normal path, not as an edge case.
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "technique-reuse-test");
    crd::containers::String    where(&alloc);
    tc::TechniqueDesc          d(&alloc);

    REQUIRE(tc::parse_technique_toml(crd::containers::StringView(kValidTechnique), d, &where)
            == tc::TechniqueCookError::Ok);
    REQUIRE(d.bindings.size() == 3U);
    REQUIRE(d.options.size() == 2U);

    // Second parse into the SAME descriptor: one binding, no options, a graph body. Everything from the first
    // asset must be GONE — including scalars the second file never writes (body_kind must not survive).
    constexpr const char* second_toml =
        "schema = 1\nname = \"unlit2\"\nbody = \"crd://technique/unlit\"\n"
        "[[binding]]\nname=\"tint\"\ntype=\"vec3\"\nfrequency=\"pass\"\n";
    REQUIRE(tc::parse_technique_toml(crd::containers::StringView(second_toml), d, &where)
            == tc::TechniqueCookError::Ok);
    CHECK(d.bindings.size() == 1U);
    CHECK(d.options.size() == 0U);
    CHECK(d.body_kind == tc::TechniqueBodyKind::Graph);

    // The BLOB path carries the same scar: read A, then read B into the same descriptor — only B may remain.
    tc::TechniqueDesc a(&alloc);
    tc::TechniqueDesc b(&alloc);
    REQUIRE(tc::parse_technique_toml(crd::containers::StringView(kValidTechnique), a, &where)
            == tc::TechniqueCookError::Ok);
    REQUIRE(tc::parse_technique_toml(crd::containers::StringView(second_toml), b, &where)
            == tc::TechniqueCookError::Ok);
    const crd::containers::Array<crd::u8> blob_a = tc::cook_technique(a, &alloc);
    const crd::containers::Array<crd::u8> blob_b = tc::cook_technique(b, &alloc);
    tc::TechniqueDesc                     reused(&alloc);
    REQUIRE(tc::read_technique(crd::containers::ConstSpan<crd::u8>(blob_a.data(), blob_a.size()), reused));
    REQUIRE(tc::read_technique(crd::containers::ConstSpan<crd::u8>(blob_b.data(), blob_b.size()), reused));
    CHECK(reused.bindings.size() == 1U);
    CHECK(reused.options.size() == 0U);
    CHECK(bytes_equal(blob_b, tc::cook_technique(reused, &alloc)));
}

TEST_CASE("REN-37.3 binding contract rejects a pass that does not supply a declared input", "[technique][ren37]")
{
    crd::memory::TlsfAllocator alloc(1U << 21U, nullptr, "technique-contract-test");
    crd::containers::String    where(&alloc);

    tc::TechniqueDesc tech(&alloc);
    REQUIRE(tc::parse_technique_toml(crd::containers::StringView(kValidTechnique), tech, &where)
            == tc::TechniqueCookError::Ok);

    fc::FrameGraphDesc graph(&alloc);
    REQUIRE(fc::parse_frame_toml(crd::containers::StringView(kGraphWithTechnique), graph, &where)
            == fc::FrameCookError::Ok);
    const fc::FramePassDesc* fwd = find_pass(graph, "forward");
    REQUIRE(fwd != nullptr);
    // the pass carries the technique NAME through parse -> the folded param the whole contract hangs on
    CHECK(fc::pass_str(*fwd, crd::containers::StringView(fc::pp::kTechnique)).size() == 11U);
    CHECK(tc::verify_technique_bindings(tech, graph, *fwd, &where) == tc::TechniqueCookError::Ok);

    // Drop the declared RESOURCE read: rejected BY NAME, at cook time, not as a black screen. (Value bindings
    // like `csm_light_vp` are engine state, not graph resources — they are checked one layer down, by the
    // renderer's resolver, and a renderer that cannot supply one fails `init_programs`.)
    {
        fc::FrameGraphDesc g2(&alloc);
        REQUIRE(fc::parse_frame_toml(crd::containers::StringView(kGraphWithTechnique), g2, &where)
                == fc::FrameCookError::Ok);
        fc::FramePassDesc* p = nullptr;
        for (crd::usize i = 0; i < g2.passes.size(); ++i)
        {
            if (fc::pass_str(g2.passes[i], crd::containers::StringView(fc::pp::kTechnique)).size() > 0U) { p = &g2.passes[i]; }
        }
        REQUIRE(p != nullptr);
        p->reads.pop_back(); // shadow_atlas — the pass no longer declares the atlas it shades with
        CHECK(tc::verify_technique_bindings(tech, g2, *p, &where) == tc::TechniqueCookError::PassMissingBinding);
    }

    // ⛔ SHAPE, not just presence. A `texture2DArrayShadow` wired to a single-LAYER resource compiles, binds and
    // renders EVERY CASCADE FROM SLICE 0 — a failure that looks like art direction on screen. Rejected here.
    {
        fc::FrameGraphDesc g3(&alloc);
        REQUIRE(fc::parse_frame_toml(crd::containers::StringView(kGraphWithTechnique), g3, &where)
                == fc::FrameCookError::Ok);
        for (crd::usize i = 0; i < g3.resources.size(); ++i) { g3.resources[i].layers = 1U; }
        const fc::FramePassDesc* p = find_pass(g3, "forward");
        REQUIRE(p != nullptr);
        CHECK(tc::verify_technique_bindings(tech, g3, *p, &where)
              == tc::TechniqueCookError::PassBindingNotLayered);
    }
}

TEST_CASE("REN-37.2 an AUTHORED technique graph splices and evaluates identically to a registered body",
          "[technique][ren37][splice]")
{
    // ⭐⭐ THE CLAIM: a technique shipped as DATA (a serialized CKIR graph) produces the same value as the same
    // shading written in C++. Without this, "author a new technique with no engine recompile" would be a slogan.
    crd::memory::TlsfAllocator alloc(1U << 22U, nullptr, "technique-splice-test");
    using namespace crd::kir;

    // The "authored" technique: emissive + base, expressed over the fixed ABI inputs. Inputs 0 and 4 ARE the ABI
    // slots `kTiBaseColor` and `kTiEmissive`, which is the whole convention.
    crd::containers::Array<crd::u8> blob(&alloc);
    {
        KGraph     src(&alloc);
        KEntry     se;
        const auto sh = make_shape({1});
        const int  base = src.input(sh, DType::F32); // iidx 0 = kTiBaseColor
        for (int i = 1; i < technique::kTiEmissive; ++i) { (void)src.input(sh, DType::F32); }
        const int emis = src.input(sh, DType::F32);  // iidx 4 = kTiEmissive
        const int sum  = src.binary(KOp::Add, base, emis);
        se.stage       = KStage::Fragment;
        se.n_out       = 1;
        se.out[0]      = {sum, 0};
        blob           = serialize_graph(src, se, &alloc);
    }
    REQUIRE(blob.size() > 0U);

    // Host graph: two constants standing in for the surface's base colour and emissive.
    KGraph     g(&alloc);
    const auto sh   = make_shape({1});
    const int  b    = g.constant(0.25, sh, DType::F32);
    const int  e    = g.constant(0.5, sh, DType::F32);

    technique::TechniqueContext ctx;
    for (int i = 0; i < technique::kTechFixedInputs; ++i) { ctx.fixed[i] = b; }
    ctx.fixed[technique::kTiBaseColor] = b;
    ctx.fixed[technique::kTiEmissive]  = e;

    technique::Technique authored;
    authored.name      = "authored_add";
    authored.blob      = blob.data();
    authored.blob_size = blob.size();
    REQUIRE(authored.valid());

    const int spliced = technique::apply_technique(g, authored, ctx);
    REQUIRE(spliced >= 0);

    // The same thing built directly — the control whose expected value is stated in advance.
    const int direct = g.binary(KOp::Add, b, e);

    CHECK(g.operands_valid()); // the splice must preserve the push-order invariant, or every later pass breaks
    CHECK(eval_closed(g, spliced, &alloc) == eval_closed(g, direct, &alloc));
    CHECK(eval_closed(g, spliced, &alloc) == 0.75);

    // ⛔ A technique blob carrying KERNEL STATEMENTS is REJECTED, not silently stripped: a fragment technique is a
    // value expression by construction, and a dropped statement would be a miscompile.
    {
        KGraph src2(&alloc);
        KEntry se2;
        const int buf = src2.buffer_decl(DType::F32, 0, 0, true);
        const int idx = src2.constant(0.0, sh, DType::U32);
        src2.stmt_buffer_store(buf, idx, src2.input(sh, DType::F32));
        se2.stage  = KStage::Fragment;
        se2.n_out  = 1;
        se2.out[0] = {buf, 0};
        const crd::containers::Array<crd::u8> bad = serialize_graph(src2, se2, &alloc);
        technique::Technique                  t2;
        t2.name      = "bad";
        t2.blob      = bad.data();
        t2.blob_size = bad.size();
        KGraph host2(&alloc);
        const int hb = host2.constant(1.0, sh, DType::F32);
        technique::TechniqueContext c2;
        for (int i = 0; i < technique::kTechFixedInputs; ++i) { c2.fixed[i] = hb; }
        CHECK(technique::apply_technique(host2, t2, c2) < 0);
    }
}
