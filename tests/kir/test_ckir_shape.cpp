// test_ckir_shape.cpp — REN-38 audit GATE: the SHAPE CHECKER `nodes::detail::bin` had always deferred to.
//
// The claim, stated so a future edit cannot weaken it: a graph that would make the ORACLE read out of bounds
// or the two GPU backends disagree (implicit broadcast, an out-of-range component, a comparison sampler
// through the wrong sample op) is REFUSED BY NAME at check time — never handed to a shader compiler to fail
// far from the asset (the 38-E7 pcf-uv scar, closed as a CLASS rather than one caller).
//
// ⛔ Reachability is part of the claim: a DEAD mis-built node passes, because lowering DCEs it (38-D3's
// unwired-control gate depends on exactly that) — the checker judges what the result actually computes.

#include <crd/kir/ckir_shape.hpp>

#include <crd/kir/ckir_nodes.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
[[nodiscard]] int scalar_of(kir::KGraph& g, double v)
{
    return g.constant(v, kir::make_shape({1}), kir::DType::F32);
}
[[nodiscard]] int vec3_of(kir::KGraph& g, double x, double y, double z)
{
    return g.vec3(scalar_of(g, x), scalar_of(g, y), scalar_of(g, z));
}
} // namespace

TEST_CASE("shape checker: a well-formed graph passes and a dead mis-built node is ignored", "[kir][shape][ren38]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "ckir-shape-test");
    kir::KGraph                g(&alloc);

    const int a   = vec3_of(g, 1.0, 2.0, 3.0);
    const int b   = vec3_of(g, 4.0, 5.0, 6.0);
    const int sum = g.binary(kir::KOp::Add, a, b);
    const int len = g.vlength(sum);

    kir::ShapeIssue issue;
    CHECK(kir::graph_shapes_valid(g, len, &alloc, &issue));

    // A DEAD mismatched binary — built, never wired into `len`'s cone. The checker must not judge it: lowering
    // DCEs it, and refusing a graph for a node the result never computes would break 38-D3's unwired-control
    // pattern.
    const int dead = g.binary(kir::KOp::Mul, a, scalar_of(g, 2.0));
    static_cast<void>(dead);
    CHECK(kir::graph_shapes_valid(g, len, &alloc, &issue));
}

TEST_CASE("shape checker: every definite violation is refused with a reason naming the node", "[kir][shape][ren38]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "ckir-shape-reject-test");

    // ⛔ THE `detail::bin` ARM — the exact 38-E7 mechanism: vec3 · scalar without an explicit splat. The GPU
    // broadcasts, the oracle reads out of bounds, and before this checker the cook returned a valid id.
    {
        kir::KGraph     g(&alloc);
        const int       v   = vec3_of(g, 1.0, 2.0, 3.0);
        const int       s   = scalar_of(g, 2.0);
        const int       bad = g.binary(kir::KOp::Mul, v, s);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
        CHECK(issue.why != nullptr);
    }
    // A ternary with one narrow arm — `detail::tern` splats, a direct `g.ternary` does not.
    {
        kir::KGraph     g(&alloc);
        const int       v   = vec3_of(g, 1.0, 2.0, 3.0);
        const int       s   = scalar_of(g, 0.5);
        const int       bad = g.ternary(kir::KOp::Mix, v, v, s);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // The C2 attribute scar: a component index outside the operand — "swizzles component 47".
    {
        kir::KGraph     g(&alloc);
        const int       v   = vec3_of(g, 1.0, 2.0, 3.0);
        const int       bad = g.vec_comp(v, 47);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // A swizzle lane past the operand's width.
    {
        kir::KGraph     g(&alloc);
        const int       v   = g.vec2(scalar_of(g, 1.0), scalar_of(g, 2.0));
        const int       bad = g.swizzle(v, 0, 3);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // A vector select condition — the oracle reads ONE cond per element.
    {
        kir::KGraph     g(&alloc);
        const int       v    = vec3_of(g, 1.0, 2.0, 3.0);
        const int       w    = vec3_of(g, 4.0, 5.0, 6.0);
        const int       vcnd = g.binary(kir::KOp::CmpLt, v, w); // bvec3
        const int       bad  = g.select(vcnd, v, w);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // A vec3 constructor fed a vector — each operand must be a scalar lane.
    {
        kir::KGraph     g(&alloc);
        const int       v   = g.vec2(scalar_of(g, 1.0), scalar_of(g, 2.0));
        const int       bad = g.vec3(v, scalar_of(g, 3.0), scalar_of(g, 4.0));
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // Cross of two vec2 — the op is defined on vec3 only.
    {
        kir::KGraph     g(&alloc);
        const int       v   = g.vec2(scalar_of(g, 1.0), scalar_of(g, 2.0));
        const int       bad = g.cross(v, v);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
}

TEST_CASE("shape checker: the sampler-pairing and uv-width scars are refused at check time", "[kir][shape][ren38]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "ckir-shape-tex-test");

    // ⛔ THE LAYERED-ATLAS SCAR: a 2-D-ARRAY texture takes a vec3 (u, v, layer); sampling it with a vec2
    // compiles on nothing and used to surface as a shader-compiler error with no pointer to the uv width.
    {
        kir::KGraph g(&alloc);
        const int tex = g.texture(1, 0, kir::DType::F32, kir::TexDim::Tex2D, true);
        const int samp = g.sampler(1, 1, false);
        const int uv   = g.vec2(scalar_of(g, 0.5), scalar_of(g, 0.5));
        const int bad  = g.tex_sample(tex, samp, uv);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // ⛔ THE `Hard`-FILTER SCAR: a comparison sampler through `tex_sample` — the sample "works" and answers a
    // depth test nobody wrote. The pairing is checkable from the types alone, so it is checked.
    {
        kir::KGraph g(&alloc);
        const int tex  = g.texture(1, 0, kir::DType::F32, kir::TexDim::Tex2D, false, false, true);
        const int samp = g.sampler(1, 1, true); // comparison sampler
        const int uv   = g.vec2(scalar_of(g, 0.5), scalar_of(g, 0.5));
        const int bad  = g.tex_sample(tex, samp, uv);
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // The inverse pairing: `tex_sample_cmp` through a PLAIN sampler.
    {
        kir::KGraph g(&alloc);
        const int tex  = g.texture(1, 0, kir::DType::F32, kir::TexDim::Tex2D, false, false, true);
        const int samp = g.sampler(1, 1, false);
        const int uv   = g.vec2(scalar_of(g, 0.5), scalar_of(g, 0.5));
        const int bad  = g.tex_sample_cmp(tex, samp, uv, scalar_of(g, 0.4));
        kir::ShapeIssue issue;
        CHECK_FALSE(kir::graph_shapes_valid(g, bad, &alloc, &issue));
        CHECK(issue.node == bad);
    }
    // And the CORRECT arrayed compare sample passes: vec3 uv (u, v, layer) + scalar ref + comparison sampler.
    {
        kir::KGraph g(&alloc);
        const int tex  = g.texture(1, 0, kir::DType::F32, kir::TexDim::Tex2D, true, false, true);
        const int samp = g.sampler(1, 1, true);
        const int uvw  = vec3_of(g, 0.5, 0.5, 0.0);
        const int good = g.tex_sample_cmp(tex, samp, uvw, scalar_of(g, 0.4));
        kir::ShapeIssue issue;
        CHECK(kir::graph_shapes_valid(g, good, &alloc, &issue));
    }
}

TEST_CASE("shape checker: an entry is judged through every root it can reach", "[kir][shape][ren38]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "ckir-shape-entry-test");
    kir::KGraph                g(&alloc);

    kir::KEntry e;
    e.stage = kir::KStage::Fragment;
    const int v    = vec3_of(g, 1.0, 2.0, 3.0);
    const int s    = scalar_of(g, 2.0);
    const int bad  = g.binary(kir::KOp::Mul, v, s); // the detail::bin arm again
    const int col  = g.vec_concat(bad, scalar_of(g, 1.0));
    e.n_out        = 1;
    e.out[0]       = {col, 0, kir::Interp::Smooth};

    kir::ShapeIssue issue;
    CHECK_FALSE(kir::entry_shapes_valid(g, e, &alloc, &issue));
    CHECK(issue.node == bad);

    // Repair the width and the same entry passes.
    kir::KGraph g2(&alloc);
    kir::KEntry e2;
    e2.stage      = kir::KStage::Fragment;
    const int v2  = vec3_of(g2, 1.0, 2.0, 3.0);
    const int s2  = scalar_of(g2, 2.0);
    const int ok2 = g2.binary(kir::KOp::Mul, v2, g2.splat(s2, 3));
    e2.n_out      = 1;
    e2.out[0]     = {g2.vec_concat(ok2, scalar_of(g2, 1.0)), 0, kir::Interp::Smooth};
    CHECK(kir::entry_shapes_valid(g2, e2, &alloc, &issue));
}
