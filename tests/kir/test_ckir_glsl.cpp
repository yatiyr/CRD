// test_ckir_glsl.cpp — Phase 3.1.6 v17-b: the CKIR GLSL emitter gate. Emits fused-elementwise compute shaders from
// CKIR subgraphs and PROVES them valid by compiling to SPIR-V through crd-shader (glslang) — no GPU needed, so the
// codegen is gated independently of the runtime. Also checks the fusion boundary (non-elementwise ⇒ rejected). ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>

#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/shader/compile.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
bool compiles(const kir::GlslKernel& k, crd::memory::IAllocator* a)
{
    const auto res = crd::shader::compile_glsl(crd::shader::Stage::Compute, crd::containers::to_view(k.source), "ckir_ew", a);
    INFO(res.error_message.c_str());
    CHECK(res.ok);
    return res.ok && res.spirv.size() > 0;
}
} // namespace

TEST_CASE("v17-b: fused-elementwise GLSL emitter -> valid SPIR-V", "[kir][glsl]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({256});
    const int                  x  = g.input(sh, kir::DType::F32);
    const int                  y  = g.input(sh, kir::DType::F32);
    // out = (x + y) * exp(x) - abs(y)  — a 4-op elementwise chain that fuses into ONE kernel
    const int s   = g.binary(kir::KOp::Add, x, y);
    const int e   = g.unary(kir::KOp::Exp, x);
    const int p   = g.binary(kir::KOp::Mul, s, e);
    const int ay  = g.unary(kir::KOp::Abs, y);
    const int out = g.binary(kir::KOp::Sub, p, ay);

    kir::GlslKernel k(&alloc);
    REQUIRE(kir::emit_elementwise_glsl(g, out, &alloc, k));
    CHECK(k.n_inputs == 2); // x, y — one global load each; the whole chain fused
    CHECK(compiles(k, &alloc));
}

TEST_CASE("v17-b: emitter covers select / min-max / transcendentals", "[kir][glsl]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({1024});
    const int                  x  = g.input(sh, kir::DType::F32);
    const int                  y  = g.input(sh, kir::DType::F32);
    // out = select(x<y, max(sin(x), tanh(y)), sqrt(abs(x)) / (1 + |y|))
    const int lt   = g.binary(kir::KOp::CmpLt, x, y);
    const int mx   = g.binary(kir::KOp::Max, g.unary(kir::KOp::Sin, x), g.unary(kir::KOp::Tanh, y));
    const int one  = g.constant(1.0, sh, kir::DType::F32);
    const int den  = g.binary(kir::KOp::Add, one, g.unary(kir::KOp::Abs, y));
    const int alt  = g.binary(kir::KOp::Div, g.unary(kir::KOp::Sqrt, g.unary(kir::KOp::Abs, x)), den);
    const int out  = g.select(lt, mx, alt);

    kir::GlslKernel k(&alloc);
    REQUIRE(kir::emit_elementwise_glsl(g, out, &alloc, k));
    CHECK(compiles(k, &alloc));
}

TEST_CASE("v17-b: emitter rejects non-elementwise subtrees (the fusion boundary)", "[kir][glsl]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const int a = g.input(kir::make_shape({4, 4}), kir::DType::F32);
    const int b = g.input(kir::make_shape({4, 4}), kir::DType::F32);
    const int mm = g.contract(a, b);          // matmul — not elementwise
    const int rd = g.reduce(kir::KOp::ReduceSum, g.binary(kir::KOp::Mul, a, b), 0x3U); // reduce — not elementwise

    kir::GlslKernel k(&alloc);
    CHECK_FALSE(kir::emit_elementwise_glsl(g, mm, &alloc, k));
    CHECK_FALSE(kir::emit_elementwise_glsl(g, rd, &alloc, k));
    // but the pure-elementwise product IS emittable
    const int prod = g.binary(kir::KOp::Mul, a, b);
    CHECK(kir::emit_elementwise_glsl(g, prod, &alloc, k));
}
