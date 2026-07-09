// test_ckir_msl.cpp — Phase 3.1.6 v17-d: the CKIR Metal Shading Language emitter (structural, CPU-only). Metal has no
// compiler off macOS, so this validates the emitter PRODUCES well-formed MSL (right kernel signature, buffers, body);
// the compile+run bit-exact gate is `tests/kir-metal` on real Apple silicon at Part C. ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_msl.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstring>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
bool has(const kir::GlslKernel& k, const char* needle) { return std::strstr(k.source.c_str(), needle) != nullptr; }
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
