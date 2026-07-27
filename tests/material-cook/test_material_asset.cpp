// test_material_asset.cpp — REN-38-C1/C2/C3 (D-007 row 141): THE MATERIAL AS AN AUTHORED ASSET.
//
// ⛔ WHAT THIS REPLACES. A material was a C++ FUNCTION POINTER (`MaterialTemplate::build_surface`), so inventing
// one meant editing and recompiling the engine — while the REN-37 design diagram already claimed `.crdm` existed.
// These gates are that claim made checkable.

#include <crd/matcook/material_asset.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_material.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

using namespace crd;
namespace mc = crd::matcook;

namespace
{
// A LAYERED material with a blend, authored end to end — no engine code anywhere in it.
// ⭐ This is the C3 gate's subject: ONE graph, and the instances below give it two completely different looks.
constexpr const char* kLayered = R"(
schema = 1
name   = "crd://material/layered"

[[param]]
name  = "tint"
value = [1.0, 0.5, 0.25]

[[param]]
name  = "blend"
value = 0.35

[[param]]
name  = "rough_base"
value = 0.77

[[node]]
name   = "rock"
op     = "multiply"
inputs = ["$tint", [0.5, 0.5, 0.5]]

[[node]]
name   = "moss"
op     = "multiply"
inputs = [[0.15, 0.4, 0.1], [2.0, 2.0, 2.0]]

[[node]]
name   = "albedo"
op     = "mix"
inputs = ["rock", "moss", "$blend"]

[[node]]
name   = "rough"
op     = "clamp01"
inputs = ["$rough_base"]

[[node]]
name   = "metal"
op     = "multiply"
inputs = [0.0, 1.0]

[surface]
base_color = "albedo"
roughness  = "rough"
metallic   = "metal"
)";
} // namespace

TEST_CASE("REN-38-C1: a `.crdm` parses, validates and COOKS to CKIR", "[material-cook][ren38]")
{
    memory::TlsfAllocator alloc(4U << 20U);
    mc::MaterialDesc      desc(&alloc);
    containers::String    where(&alloc);
    REQUIRE(mc::parse_material_toml(containers::StringView(kLayered), desc, &where) == mc::MaterialCookError::Ok);
    CHECK(desc.params.size() == 3U);
    CHECK(desc.nodes.size() == 5U);

    // ⛔⛔ A REUSED DESCRIPTOR IS RESET, not appended to. Parsing a second asset into `desc` used to MERGE it with
    // the first: with overlapping names that surfaced as `DuplicateName` (an error naming the wrong thing), and
    // with distinct names as a silently merged graph that cooks and shades from both. Any tool with a load button
    // reuses its descriptor, so this is the normal path rather than an edge case.
    REQUIRE(mc::parse_material_toml(containers::StringView(kLayered), desc, &where) == mc::MaterialCookError::Ok);
    CHECK(desc.params.size() == 3U);
    CHECK(desc.nodes.size() == 5U);

    // ⭐ THE COOK RETURNS THE SAME THING A C++ `build_surface` DID — an OpenPBR surface struct node, ready for
    // `build_fs_for_pass`. That is what makes this a REPLACEMENT rather than a second, parallel path.
    kir::KGraph g(&alloc);
    const int   sid     = kir::material::define_surface(g);
    const int   surface = mc::cook_material(desc, g, sid);
    CHECK(surface >= 0);
    CHECK(g.size() > 0);
}

TEST_CASE("REN-38-C1: every way a `.crdm` can be wrong is a NAMED error", "[material-cook][ren38]")
{
    memory::TlsfAllocator alloc(4U << 20U);
    const auto            cook = [&](const char* toml) {
        mc::MaterialDesc   d(&alloc);
        containers::String where(&alloc);
        return mc::parse_material_toml(containers::StringView(toml), d, &where);
    };
    using E = mc::MaterialCookError;

    // ⛔ ARITY. A `mix` with two inputs would wire a garbage node id into the third slot — CKIR does not
    // bounds-check a node index, so the result is a graph that BUILDS and computes something unrelated.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"a\"\nop = \"mix\"\ninputs = [0.0, 1.0]\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::WrongArity);
    // ⛔ An op the registry does not have. Skipping it silently would leave the surface wired to nothing.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"a\"\nop = \"frobnicate\"\ninputs = [0.0]\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::UnknownOp);
    // ⛔ A DAG, enforced by DECLARATION ORDER — a node may only read one declared BEFORE it. Stricter than "no
    // cycles" and deliberately so: it makes the cook a single forward pass and a cycle impossible to WRITE.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"a\"\nop = \"absval\"\ninputs = [\"b\"]\n"
               "[[node]]\nname = \"b\"\nop = \"absval\"\ninputs = [1.0]\n[surface]\nbase_color = \"a\"\n")
          == E::NodeCycle);
    // ⛔ BASE COLOUR is required: every other field has a defensible default, and defaulting this one to white
    // renders a plausible object that is not the one the author described.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"a\"\nop = \"absval\"\ninputs = [1.0]\n[surface]\n"
               "roughness = \"a\"\n")
          == E::NoBaseColor);
    // ⛔ The surface naming a node that does not exist.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"a\"\nop = \"absval\"\ninputs = [1.0]\n[surface]\n"
               "base_color = \"nope\"\n")
          == E::SurfaceUnbound);
    // ⛔ An input naming a parameter that was never declared.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"a\"\nop = \"absval\"\ninputs = [\"$ghost\"]\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::UnknownInput);
    // ⛔⛔ ADR-0102, ENFORCED. A material describes SURFACE RESPONSE; a node reaching for lighting or shadow state
    // would make every material carry a copy of the lighting model — the exact coupling REN-37 removed.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"a\"\nop = \"shadow\"\ninputs = [1.0]\n[surface]\n"
               "base_color = \"a\"\n")
          == E::ForbiddenLighting);
    // ⛔ An instance overriding a parameter that does not exist — otherwise the artist sees the default with
    // nothing on screen or in the file to explain why.
    CHECK(cook("schema = 1\nname = \"m\"\n[[param]]\nname = \"k\"\nvalue = 1.0\n[[node]]\nname = \"a\"\n"
               "op = \"absval\"\ninputs = [\"$k\"]\n[[instance]]\nname = \"i\"\nset = { typo = 2.0 }\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::UnknownOverride);
}

TEST_CASE("REN-38-C2: the MaterialX-class node set is reachable AS DATA", "[material-cook][ren38]")
{
    memory::TlsfAllocator alloc(64U << 20U);
    // ⭐⭐ COVERAGE IS THE CLAIM. B6 built ~90 MaterialX-class nodes in CKIR; a registry that silently exposed
    // half of them would let a material cook and render the wrong thing, with nothing in the asset looking wrong.
    // ⛔ The registry carries the WHOLE public node library of `ckir_nodes.hpp` — everything outside its `detail`
    // namespace, which holds shared tails (`mix_tail`, the Porter-Duff compose) rather than authorable nodes.
    const u32 n = mc::material_op_count();
    UNSCOPED_INFO("registry ops = " << n);
    CHECK(n >= 94U);

    // EVERY registered op must actually BUILD from an asset — not merely be listed. A name in the table the
    // dispatch cannot construct is the same lie as a missing one, one layer down. ⛔⛔ And the inputs are built
    // FROM THE REGISTRY's own account of each slot: an ATTRIBUTE gets an integer in range, a WIRE gets a literal of
    // the slot's width. Feeding 0.5 to everything would swizzle `.a` out of a float and index a vec4 by a node id.
    u32 built = 0U;
    for (u32 i = 0; i < n; ++i)
    {
        const char* op    = mc::material_op_name(i);
        const u32   arity = mc::material_op_arity(i);
        REQUIRE(op != nullptr);
        CHECK(mc::material_op_exists(containers::StringView(op)));

        containers::String toml(&alloc);
        toml.append("schema = 1\nname = \"cov\"\n\n[[node]]\nname = \"n\"\nop = \"");
        toml.append(op);
        toml.append("\"\ninputs = [");
        for (u32 k = 0; k < arity; ++k)
        {
            if (k > 0U) { toml.append(", "); }
            if (mc::material_op_arg_is_attr(i, k))
            {
                char buf[16];
                (void)std::snprintf(static_cast<char*>(buf), sizeof(buf), "%d", mc::material_op_attr_min(i));
                toml.append(static_cast<const char*>(buf));
                continue;
            }
            const u32 w = mc::material_op_arg_width(i, k);
            CHECK(w >= 1U);
            if (w <= 1U) { toml.append("0.5"); continue; }
            toml.append("[0.5");
            for (u32 c = 1; c < w; ++c) { toml.append(", 0.5"); }
            toml.append("]");
        }
        toml.append("]\n\n[surface]\nbase_color = \"n\"\n");

        mc::MaterialDesc   d(&alloc);
        containers::String where(&alloc);
        INFO(op);
        REQUIRE(mc::parse_material_toml(containers::StringView(toml.c_str(), toml.size()), d, &where)
                == mc::MaterialCookError::Ok);
        kir::KGraph g(&alloc);
        const int   sid = kir::material::define_surface(g);
        if (mc::cook_material(d, g, sid) >= 0) { ++built; }
    }
    UNSCOPED_INFO("ops that cooked = " << built << " / " << n);
    CHECK(built == n);

    // ⛔ THE WIDEST NODE IS SEVEN INPUTS (`gooch_shade`, `range`). A cap set below it does not "leave headroom" —
    // it makes those nodes unauthorable and reports the wrong reason. Asserted so a later cap change cannot
    // quietly delete them from the library.
    u32 widest = 0U;
    for (u32 i = 0; i < n; ++i) { widest = mc::material_op_arity(i) > widest ? mc::material_op_arity(i) : widest; }
    CHECK(widest <= mc::kMaxNodeInputs);
    CHECK(widest == 7U);
}

TEST_CASE("REN-38-C2: a COMPILE-TIME argument may not be wired", "[material-cook][ren38]")
{
    memory::TlsfAllocator alloc(4U << 20U);
    const auto            cook = [&](const char* toml) {
        mc::MaterialDesc   d(&alloc);
        containers::String where(&alloc);
        return mc::parse_material_toml(containers::StringView(toml), d, &where);
    };
    using E = mc::MaterialCookError;

    // ⛔⛔ IN C++ A SWIZZLE INDEX AND A NODE ID ARE BOTH `int`. `extract(v, index)` takes a channel; wiring a node
    // there type-checks, builds, and pulls out component 47. The registry says which slots are attributes, so the
    // asset layer can refuse it — the only layer that still can.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"v\"\nop = \"combine4\"\n"
               "inputs = [1.0, 0.0, 0.0, 1.0]\n[[node]]\nname = \"a\"\nop = \"extract\"\ninputs = [\"v\", \"v\"]\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::AttrNotConstant);
    // ⛔ Nor a PARAMETER: an instance changes VALUES, not topology — two instances of one material must not be two
    // different graphs.
    CHECK(cook("schema = 1\nname = \"m\"\n[[param]]\nname = \"i\"\nvalue = 1.0\n[[node]]\nname = \"v\"\n"
               "op = \"combine4\"\ninputs = [1.0, 0.0, 0.0, 1.0]\n[[node]]\nname = \"a\"\nop = \"extract\"\n"
               "inputs = [\"v\", \"$i\"]\n[surface]\nbase_color = \"a\"\n")
          == E::AttrNotConstant);
    // ⛔ And it is RANGE-CHECKED. `extract(v, 9)` on a vec4 is a swizzle CKIR does not bounds-check.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"v\"\nop = \"combine4\"\n"
               "inputs = [1.0, 0.0, 0.0, 1.0]\n[[node]]\nname = \"a\"\nop = \"extract\"\ninputs = [\"v\", 9]\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::AttrOutOfRange);
    // …and a fractional index is not an index at all.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"v\"\nop = \"combine4\"\n"
               "inputs = [1.0, 0.0, 0.0, 1.0]\n[[node]]\nname = \"a\"\nop = \"extract\"\ninputs = [\"v\", 1.5]\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::AttrOutOfRange);
    // ⭐ The same slot written correctly cooks — so the rule is a CHECK, not a ban on the node.
    CHECK(cook("schema = 1\nname = \"m\"\n[[node]]\nname = \"v\"\nop = \"combine4\"\n"
               "inputs = [1.0, 0.0, 0.0, 1.0]\n[[node]]\nname = \"a\"\nop = \"extract\"\ninputs = [\"v\", 3]\n"
               "[surface]\nbase_color = \"a\"\n")
          == E::Ok);
}

TEST_CASE("REN-38-C3: material INSTANCES give one graph N looks", "[material-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    containers::String    toml(&alloc);
    toml.append(kLayered);
    toml.append("\n[[instance]]\nname = \"mossy\"\nset = { blend = 0.9, rough_base = 0.2 }\n"
                "\n[[instance]]\nname = \"bare\"\nset = { blend = 0.05, rough_base = 0.65 }\n");

    mc::MaterialDesc   desc(&alloc);
    containers::String where(&alloc);
    REQUIRE(mc::parse_material_toml(containers::StringView(toml.c_str(), toml.size()), desc, &where)
            == mc::MaterialCookError::Ok);
    REQUIRE(desc.instances.size() == 2U);

    // ⭐⭐ ONE GRAPH, TWO PARAMETER SETS. ⛔ Resolved to VALUES at cook time rather than left as a runtime
    // indirection — which is what makes an instance a SPECIALISATION: the constant folder then sees a literal, so
    // a layer at weight 0 costs nothing instead of costing a mix every pixel.
    //
    // ⛔⛔ THE DIFFERENCE IS THE CLAIM, not "it cooked". Every instance shares the graph's TOPOLOGY, so node counts
    // are identical by construction and could never tell two instances apart — an override that never reached the
    // cook would pass a size check while every instance rendered the base look. So the gate reads the cooked
    // CONSTANTS back and asserts each instance's override is the value that actually landed in the IR.
    const auto has_const = [&](const kir::KGraph& g, double want) {
        for (int i = 0; i < g.size(); ++i)
        {
            const kir::KNode& nd = g.node(i);
            if (nd.op == kir::KOp::Const && nd.cval == want) { return true; }
        }
        return false;
    };
    const auto cook_into = [&](kir::KGraph& g, const char* inst) {
        const int sid = kir::material::define_surface(g);
        const int s   = mc::cook_material(desc, g, sid, containers::StringView(inst));
        CHECK(s >= 0);
        return g.size();
    };

    kir::KGraph gm(&alloc);
    kir::KGraph gb(&alloc);
    kir::KGraph gd(&alloc);
    const int   nm = cook_into(gm, "mossy");
    const int   nb = cook_into(gb, "bare");
    const int   nd = cook_into(gd, "");
    CHECK(nm > 0);
    CHECK(nb > 0);
    CHECK(nd > 0);

    // ⛔ Every probe value is UNIQUE to one instance. A value the base graph also contains (0.0, 1.0, or a `tint`
    // component — 0.25 is one, which is how this gate first caught itself) would pass whether the override reached
    // the cook or not, and a check that cannot fail is not a check.
    CHECK(has_const(gm, 0.9));         // mossy's blend
    CHECK(has_const(gm, 0.2));         // mossy's roughness
    CHECK_FALSE(has_const(gm, 0.35));  // …and the DEFAULT blend is GONE, not merely joined by the override
    CHECK_FALSE(has_const(gm, 0.77));
    CHECK(has_const(gb, 0.05));        // bare's blend
    CHECK(has_const(gb, 0.65));        // bare's roughness
    CHECK_FALSE(has_const(gb, 0.9));   // …and nothing of mossy's leaked across
    CHECK_FALSE(has_const(gb, 0.35));
    CHECK(has_const(gd, 0.35));        // no instance = the declared defaults, unmodified
    CHECK(has_const(gd, 0.77));
    CHECK_FALSE(has_const(gd, 0.2));
    CHECK_FALSE(has_const(gd, 0.05));

    // ⛔ AN UNKNOWN INSTANCE FAILS. Falling back to the defaults would hide a typo behind a material that renders
    // — the artist would see the base look with nothing to tell them their variant never existed.
    {
        kir::KGraph g(&alloc);
        const int   sid = kir::material::define_surface(g);
        CHECK(mc::cook_material(desc, g, sid, containers::StringView("ghost")) < 0);
    }
}

TEST_CASE("REN-38-C1: a `.crdm` survives an editor ROUND TRIP", "[material-cook][ren38]")
{
    memory::TlsfAllocator alloc(16U << 20U);
    mc::MaterialDesc      a(&alloc);
    containers::String    where(&alloc);
    REQUIRE(mc::parse_material_toml(containers::StringView(kLayered), a, &where) == mc::MaterialCookError::Ok);

    // ⛔ The frame asset's rule, one asset over: a node-editor save must not silently drop what it did not
    // understand. Parameters, node ops, wiring KIND (literal vs node vs `$param`) and the surface mapping all
    // have to survive, because each one is a different wrong picture when it does not.
    containers::String text = mc::emit_material_toml(a, &alloc);
    mc::MaterialDesc   b(&alloc);
    REQUIRE(mc::parse_material_toml(containers::StringView(text.c_str(), text.size()), b, &where)
            == mc::MaterialCookError::Ok);
    REQUIRE(b.params.size() == a.params.size());
    REQUIRE(b.nodes.size() == a.nodes.size());
    for (usize i = 0; i < a.nodes.size(); ++i)
    {
        CHECK(b.nodes[i].name == a.nodes[i].name);
        CHECK(b.nodes[i].op == a.nodes[i].op);
        REQUIRE(b.nodes[i].inputs.size() == a.nodes[i].inputs.size());
        for (usize k = 0; k < a.nodes[i].inputs.size(); ++k)
        {
            CHECK(b.nodes[i].inputs[k].kind == a.nodes[i].inputs[k].kind);
            CHECK(b.nodes[i].inputs[k].name == a.nodes[i].inputs[k].name);
        }
    }
    CHECK(b.surface.base_color == a.surface.base_color);
    CHECK(b.surface.roughness == a.surface.roughness);
    CHECK(b.surface.metallic == a.surface.metallic);
}
