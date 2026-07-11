// test_ckir_msl.cpp — Phase 3.1.6 v17-d: the CKIR Metal Shading Language emitter (structural, CPU-only). Metal has no
// compiler off macOS, so this validates the emitter PRODUCES well-formed MSL (right kernel signature, buffers, body);
// the compile+run bit-exact gate is `tests/kir-metal` on real Apple silicon at Part C. ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_msl.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cctype>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
bool has(const kir::GlslKernel& k, const char* needle) { return std::strstr(k.source.c_str(), needle) != nullptr; }

// Structural well-formedness of an emitted kernel: every `tN` it REFERENCES must have been DECLARED (`<type> tN = `).
// Metal cannot be compiled off macOS, so this stands in for the compiler on the one class of bug a substring check
// misses: a dangling operand. GLSL shipped exactly that — `mat3(t0, t1, t-1)` — because `MatFromCols` hardcoded three
// columns and a mat2's third operand is -1. A token starts only when the previous char is not alphanumeric, so
// `float2` / `int3` / `transpose` do not masquerade as temps.
bool temps_well_formed(const kir::GlslKernel& k)
{
    const char* p = k.source.c_str();
    if (std::strstr(p, "t-1") != nullptr) { return false; } // a negative operand id reached the emitter
    bool declared[4096] = {};
    // pass 1: declarations are the only place a temp is followed by " = "
    for (crd::usize i = 0; p[i] != '\0'; ++i)
    {
        if (p[i] != 't' || (i > 0 && (std::isalnum(static_cast<unsigned char>(p[i - 1])) != 0 || p[i - 1] == '_'))) { continue; }
        crd::usize j = i + 1;
        int        id = 0;
        while (std::isdigit(static_cast<unsigned char>(p[j])) != 0) { id = id * 10 + (p[j] - '0'); ++j; }
        if (j == i + 1 || id >= 4096) { continue; }
        if (p[j] == ' ' && p[j + 1] == '=' && p[j + 2] == ' ') { declared[id] = true; }
    }
    // pass 2: every reference must resolve to a declaration
    for (crd::usize i = 0; p[i] != '\0'; ++i)
    {
        if (p[i] != 't' || (i > 0 && (std::isalnum(static_cast<unsigned char>(p[i - 1])) != 0 || p[i - 1] == '_'))) { continue; }
        crd::usize j = i + 1;
        int        id = 0;
        while (std::isdigit(static_cast<unsigned char>(p[j])) != 0) { id = id * 10 + (p[j] - '0'); ++j; }
        if (j == i + 1 || id >= 4096) { continue; }
        if (!declared[id]) { return false; }
    }
    return true;
}
} // namespace

TEST_CASE("v17-d: MSL emitter produces well-formed compute kernels", "[kir][msl]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);

    SECTION("elementwise")
    {
        kir::KGraph      g(&alloc);
        const kir::Shape sh  = kir::make_shape({256});
        const int        x   = g.input(sh, kir::DType::F32);
        const int        y   = g.input(sh, kir::DType::F32);
        const int        out = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, y), x), g.binary(kir::KOp::Mul, y, y));
        kir::GlslKernel  k(&alloc);
        REQUIRE(emit_elementwise_msl(g, out, &alloc, k));
        CHECK(k.n_inputs == 2);
        CHECK(has(k, "#include <metal_stdlib>"));
        CHECK(has(k, "kernel void ckir"));
        CHECK(has(k, "thread_position_in_grid"));
        CHECK(has(k, "device const float* in0 [[buffer(0)]]"));
        CHECK(has(k, "device float* outb [[buffer(2)]]"));
        CHECK(has(k, "constant PC& pc [[buffer(3)]]"));
    }
    SECTION("matmul")
    {
        kir::KGraph     g(&alloc);
        const int       a = g.input(kir::make_shape({32, 48}), kir::DType::F32);
        const int       b = g.input(kir::make_shape({48, 24}), kir::DType::F32);
        kir::GlslKernel k(&alloc);
        REQUIRE(emit_contract_msl(g, g.contract(a, b), k));
        CHECK(has(k, "device const float* A [[buffer(0)]]"));
        CHECK(has(k, "device const float* Bm [[buffer(1)]]"));
        CHECK(has(k, "acc = acc + prod"));
    }
    SECTION("fast matmul (T2 tiled, transposed-A, FMA)")
    {
        kir::KGraph     g(&alloc);
        const int       a = g.input(kir::make_shape({128, 64}), kir::DType::F32);
        const int       b = g.input(kir::make_shape({64, 128}), kir::DType::F32);
        kir::GlslKernel k(&alloc);
        REQUIRE(emit_contract_fast_msl(g, g.contract(a, b, kir::DetTier::Fast), k));
        CHECK(k.n_inputs == 2);
        CHECK(has(k, "kernel void ckir"));
        CHECK(has(k, "threadgroup float As[512]"));           // transposed-A shared tile
        CHECK(has(k, "threadgroup_position_in_grid"));        // group-per-tile dispatch
        CHECK(has(k, "threadgroup_barrier(mem_flags::mem_threadgroup)"));
        CHECK(has(k, "fma("));                                // FMA (fast tier)
    }
    SECTION("reduce")
    {
        kir::KGraph     g(&alloc);
        const int       a = g.input(kir::make_shape({40, 96}), kir::DType::F32);
        kir::GlslKernel k(&alloc);
        REQUIRE(emit_reduce_msl(g, g.reduce(kir::KOp::ReduceSum, a, 0x2U), k));
        CHECK(has(k, "kernel void ckir"));
        CHECK(has(k, "for (uint r = 0; r < pc.redsize"));
    }
}

// ── B0 fan-out (2026-07-10): the TYPE-AWARE value layer on MSL, gated STRUCTURALLY ───────────────────────────────────
// Metal has no compiler off macOS, so this proves the emitter produces well-formed MSL with the right types and helpers,
// and — via `temps_well_formed` — that no operand dangles. The compile + bit-exact run is ADR-0098 §3 v17-n on real
// Apple silicon. MSL is the closest dialect to GLSL (native float3/float3x3/bool3, real `?:`, column-first `floatCxR`),
// but like WGSL it lacks `inverse()` and `outerProduct()`.
TEST_CASE("v17 B0 fan-out: MSL type-aware emitter (vec/mat/bool/struct)", "[kir][msl][typelayer]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    const kir::Shape           sh = kir::make_shape({128});

    SECTION("vec3 corpus + mat3 inverse -> float3 / float3x3 / crd_inv3")
    {
        kir::KGraph g(&alloc);
        const int   c0  = g.input_vec(sh, kir::DType::F32, 3);
        const int   c1  = g.input_vec(sh, kir::DType::F32, 3);
        const int   c2  = g.input_vec(sh, kir::DType::F32, 3);
        const int   vv  = g.input_vec(sh, kir::DType::F32, 3);
        const int   mat = g.mat3(c0, c1, c2);
        const int   out = g.normalize(g.mat_mul_vec(g.mat_mul(mat, g.mat_inverse(mat)), g.cross(vv, c0)));

        kir::GlslKernel k(&alloc);
        REQUIRE(emit_vec_msl(g, out, &alloc, k));
        CHECK(k.out_comps == 3);
        CHECK(has(k, "#include <metal_stdlib>"));
        CHECK(has(k, "kernel void ckir"));
        CHECK(has(k, "float3x3"));               // column-first matCxR spelling, as in GLSL
        CHECK(has(k, "static float3x3 crd_inv3")); // MSL has no inverse() builtin
        CHECK(has(k, "normalize("));
        CHECK(has(k, "cross("));
        CHECK(has(k, "outb[gid*3+0]"));          // interleaved vec3 write-back
        CHECK(temps_well_formed(k));
    }

    SECTION("mat2 (comps==4, not a float4) + non-square outer product built from columns")
    {
        kir::KGraph g(&alloc);
        const int   c0  = g.input_vec(sh, kir::DType::F32, 2);
        const int   c1  = g.input_vec(sh, kir::DType::F32, 2);
        const int   mat = g.mat2(c0, c1);
        const int   inv = g.mat_inverse(mat);

        kir::GlslKernel k(&alloc);
        REQUIRE(emit_vec_msl(g, inv, &alloc, k));
        CHECK(has(k, "float2x2"));
        CHECK(has(k, "static float2x2 crd_inv2"));
        CHECK_FALSE(has(k, "float4 t")); // a mat2 must never be spelled as a float4
        CHECK(temps_well_formed(k));

        kir::KGraph g2(&alloc);
        const int   a2 = g2.input_vec(sh, kir::DType::F32, 2);
        const int   b3 = g2.input_vec(sh, kir::DType::F32, 3);
        const int   op = g2.outer_product(a2, b3); // 2 rows x 3 cols -> float3x2, built column-by-column
        kir::GlslKernel k2(&alloc);
        REQUIRE(emit_vec_msl(g2, op, &alloc, k2));
        CHECK(k2.out_comps == 6);
        CHECK(has(k2, "float3x2("));   // no outerProduct() in MSL: three columns, each `a * b.k`
        CHECK(has(k2, ".x, "));
        CHECK(temps_well_formed(k2));
    }

    SECTION("bool3 comparisons + any/all, and a Light struct lowered by SROA")
    {
        kir::KGraph g(&alloc);
        const int   av = g.input_vec(sh, kir::DType::F32, 3);
        const int   bv = g.input_vec(sh, kir::DType::F32, 3);
        const int   an = g.vany(g.binary(kir::KOp::CmpLt, av, bv));
        kir::GlslKernel k(&alloc);
        REQUIRE(emit_vec_msl(g, an, &alloc, k));
        CHECK(has(k, "bool3 t"));      // componentwise relational yields boolN (like HLSL, unlike GLSL)
        CHECK(has(k, "any("));
        CHECK(has(k, "? 1.0 : 0.0"));  // a bool result stores as float 0/1
        CHECK(temps_well_formed(k));

        kir::KGraph      g2(&alloc);
        const kir::KType fields[3] = {kir::KType::vec(kir::DType::F32, 3), kir::KType::make_scalar(kir::DType::F32),
                                      kir::KType::vec(kir::DType::F32, 3)};
        const int        light   = g2.define_struct(fields, 3);
        const int        pos     = g2.input_vec(sh, kir::DType::F32, 3);
        const int        rad     = g2.input(sh, kir::DType::F32);
        const int        col     = g2.input_vec(sh, kir::DType::F32, 3);
        const int        flds[3] = {pos, rad, col};
        const int        lite    = g2.struct_make(light, flds, 3);
        const int        out = g2.binary(kir::KOp::Add, g2.binary(kir::KOp::Mul, g2.field_get(lite, 2), g2.splat(g2.field_get(lite, 1), 3)), g2.field_get(lite, 0));

        kir::GlslKernel k2(&alloc);
        REQUIRE(emit_vec_msl(g2, out, &alloc, k2));
        CHECK(k2.n_inputs == 3);
        CHECK_FALSE(has(k2, "struct Light")); // SROA: the aggregate is never materialized
        CHECK(temps_well_formed(k2));
    }

    // A structural checker that has never rejected anything proves nothing (SANITY #2). Show it bites, on the two
    // shapes it exists to catch: a dangling `t-1` operand, and a reference to a temp that was never declared.
    SECTION("the well-formedness checker itself rejects malformed kernels")
    {
        kir::GlslKernel bad1(&alloc);
        bad1.source.append("  float3 t0 = float3(1.0);\n  float3x3 t1 = float3x3(t0, t0, t-1);\n");
        CHECK_FALSE(temps_well_formed(bad1)); // the exact bug GLSL's MatFromCols shipped for mat2

        kir::GlslKernel bad2(&alloc);
        bad2.source.append("  float t0 = in0[gid];\n  float t1 = t0 + t9;\n"); // t9 never declared
        CHECK_FALSE(temps_well_formed(bad2));

        kir::GlslKernel good(&alloc);
        good.source.append("  float t0 = in0[gid];\n  float3 t1 = float3(t0);\n  outb[gid] = t1.x;\n");
        CHECK(temps_well_formed(good)); // `float3` must not masquerade as a temp `t3`
    }

    SECTION("dynamic control flow and mat4 inverse are refused LOUDLY, never mislowered")
    {
        kir::KGraph g(&alloc);
        const int   x    = g.input(sh, kir::DType::F32);
        const int   vecx = g.splat(x, 3);
        const int   cnt  = g.constant(4.0, sh, kir::DType::F32);
        const int   loop = g.for_loop(cnt, vecx, [&](int /*idx*/, int acc) { return g.binary(kir::KOp::Add, acc, vecx); });
        kir::GlslKernel k(&alloc);
        CHECK_FALSE(emit_vec_msl(g, loop, &alloc, k)); // `For` not mirrored yet -> refuse, do not guess

        kir::KGraph g2(&alloc);
        const int   m4 = g2.input_mat(sh, kir::DType::F32, 4, 4);
        kir::GlslKernel k2(&alloc);
        CHECK_FALSE(emit_vec_msl(g2, g2.mat_inverse(m4), &alloc, k2)); // mat4 inverse deferred, as on HLSL/WGSL/CUDA
    }
}
