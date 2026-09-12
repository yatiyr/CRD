// test_backend_metal.cpp — Phase 3.1.6 v17-d: the CKIR Metal backend on real Apple silicon. Guarded on the Metal
// backend target (APPLE); on this Windows box it's skipped — built + run at Part C (GitHub Actions macOS). Same graphs
// as the other backends, gated BIT-EXACT vs the CPU oracle (Metal math-mode SAFE ⇒ no FMA fusion). ADR-0098.

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/metal/backend_metal.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr int kN = 1024;
void fill(float* v, int n, float base) { for (int i = 0; i < n; ++i) { v[i] = base + 0.013F * static_cast<float>(i) - 0.5F * static_cast<float>(i % 7); } }
} // namespace

TEST_CASE("v17-d: Metal elementwise + matmul + reduce bit-match the CPU oracle", "[kir][metal][gpu]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KirBackendMetal       mt(&alloc);
    if (!mt.valid()) { WARN("no Metal device available; skipping (validated at Part C)"); return; }
    kir::KirBackendCpu cpu(&alloc);

    SECTION("elementwise arith")
    {
        kir::KGraph      g(&alloc);
        const kir::Shape sh  = kir::make_shape({kN});
        const int        x   = g.input(sh, kir::DType::F32);
        const int        y   = g.input(sh, kir::DType::F32);
        const int        out = g.binary(kir::KOp::Add, g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, y), x), g.binary(kir::KOp::Mul, y, y)), g.binary(kir::KOp::Mul, x, x));
        float            xv[kN];
        float            yv[kN];
        fill(xv, kN, 1.0F);
        fill(yv, kN, -0.7F);
        const float* inputs[] = {xv, yv};
        float        gpu_out[kN];
        float        cpu_out[kN];
        REQUIRE(mt.run(g, out, inputs, 2, gpu_out));
        REQUIRE(cpu.run(g, out, inputs, 2, cpu_out));
        for (int i = 0; i < kN; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
    }
    SECTION("matmul 32x48 @ 48x24")
    {
        constexpr int mm = 32;
        constexpr int kk = 48;
        constexpr int nn = 24;
        kir::KGraph   g(&alloc);
        const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
        const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
        const int     c = g.contract(a, b);
        float         av[mm * kk];
        float         bv[kk * nn];
        fill(av, mm * kk, 0.2F);
        fill(bv, kk * nn, -0.15F);
        const float* inputs[] = {av, bv};
        float        gpu_out[mm * nn];
        float        cpu_out[mm * nn];
        REQUIRE(mt.run(g, c, inputs, 2, gpu_out));
        REQUIRE(cpu.run(g, c, inputs, 2, cpu_out));
        for (int i = 0; i < mm * nn; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
    }
    SECTION("reduce-sum over rows")
    {
        constexpr int rows = 40;
        constexpr int cols = 96;
        kir::KGraph   g(&alloc);
        const int     a   = g.input(kir::make_shape({rows, cols}), kir::DType::F32);
        const int     red = g.reduce(kir::KOp::ReduceSum, a, 0x2U);
        float         xv[rows * cols];
        fill(xv, rows * cols, 0.25F);
        const float* inputs[] = {xv};
        float        gpu_out[rows];
        float        cpu_out[rows];
        REQUIRE(mt.run(g, red, inputs, 1, gpu_out));
        REQUIRE(cpu.run(g, red, inputs, 1, cpu_out));
        for (int i = 0; i < rows; ++i) { CHECK(gpu_out[i] == cpu_out[i]); }
    }
}
