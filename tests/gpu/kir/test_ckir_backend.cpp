// test_ckir_backend.cpp — Phase 3.1.6 v17-b: the KirBackend SEAM. Runs a fused-elementwise graph through the backend
// interface (the CPU backend / oracle here; Vulkan/CUDA/… implement the same interface + get compared against this).
// Proves the runtime seam is wired: build graph → run_elementwise → result. ADR-0098.

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

TEST_CASE("v17-b: KirBackend seam runs a fused-elementwise graph on the CPU backend", "[kir][backend]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({8});
    const int                  x  = g.input(sh, kir::DType::F32);
    const int                  y  = g.input(sh, kir::DType::F32);
    // out = (x + y) * x - y   (pure IEEE arithmetic ⇒ f32-exact ⇒ a GPU backend must bit-match this)
    const int out = g.binary(kir::KOp::Sub, g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, y), x), y);

    float             xv[8] = {1.0F, 2.0F, -3.0F, 0.5F, 4.0F, -1.5F, 2.25F, 0.0F};
    float             yv[8] = {0.5F, -1.0F, 2.0F, 3.0F, -0.25F, 1.0F, -2.0F, 5.0F};
    const float*      inputs[] = {xv, yv};
    float             o[8]  = {};

    kir::KirBackendCpu cpu(&alloc);
    REQUIRE(cpu.run(g, out, inputs, 2, o));
    for (int i = 0; i < 8; ++i)
    {
        const float expect = (xv[i] + yv[i]) * xv[i] - yv[i]; // f32 arithmetic, same rounding
        CHECK(o[i] == expect);
    }
}
