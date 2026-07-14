// test_ckir_reduce.cpp -- B-cmp: the CKIR device-wide PARALLEL REDUCTION (ckir_reduce.hpp) on the CPU oracle. Proves the
// authored reduce KGraph computes a correct sum / min / max (vs a direct reference), single-workgroup AND the 2-pass
// device reduce. GPU bit-exactness (Vulkan/DX12) is the next slice; here the oracle validates the tree/serial index map.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_reduce.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
crd::f64 fabs64(crd::f64 x) { return x < 0.0 ? -x : x; }

// Drive a ReducePlan through the CPU oracle: allocate one arena per buffer, run pass 0 (grid = nblocks) into partials, then
// (if 2-pass) run pass 1 (grid = 1) reducing the partials → out[0]. Returns the scalar result. Input is f32-rounded.
crd::f64 run_reduce(const kir::ReducePlan& plan, const crd::f64* xin, crd::memory::IAllocator* alloc)
{
    const int n = plan.n;
    crd::containers::Array<crd::f64> in(alloc);
    in.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i) { in[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xin[i])); }

    if (plan.single_pass)
    {
        crd::f64          out = -1234.0;
        kir::KernelBuffer kb[2] = {{in.data(), n, 0, 0}, {&out, 1, 0, 1}};
        kir::eval_cpu_kernel(*plan.block_graph, plan.block, kb, 2, plan.block.local_size[0], alloc, 1U);
        return out;
    }
    crd::containers::Array<crd::f64> partials(alloc);
    partials.resize(static_cast<crd::usize>(plan.nblocks), 0.0);
    kir::KernelBuffer kb0[2] = {{in.data(), n, 0, 0}, {partials.data(), plan.nblocks, 0, 1}};
    kir::eval_cpu_kernel(*plan.block_graph, plan.block, kb0, 2, plan.block.local_size[0], alloc, static_cast<crd::u32>(plan.nblocks));
    crd::f64          out    = -1234.0;
    kir::KernelBuffer kb1[2] = {{partials.data(), plan.nblocks, 0, 0}, {&out, 1, 0, 1}};
    kir::eval_cpu_kernel(*plan.final_graph, plan.final_pass, kb1, 2, plan.final_pass.local_size[0], alloc, 1U);
    return out;
}
} // namespace

TEST_CASE("B-cmp: CKIR single-workgroup reduction == sum/min/max reference (f32)", "[kir][kernel][reduce]")
{
    constexpr int              n = 1024;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (int i = 0; i < n; ++i) { x[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 13 + 7) % 97) - 48.0 + 0.5 * static_cast<crd::f64>(i % 3); }

    SECTION("sum")
    {
        kir::KGraph g0(&alloc); kir::KGraph* graphs[2] = {&g0, &g0};
        const kir::ReducePlan plan = kir::build_reduce(graphs, n, kir::KOp::Add, 256);
        REQUIRE(plan.single_pass);
        const crd::f64 got = run_reduce(plan, x.data(), &alloc);
        crd::f64       ref = 0.0; // tree/serial order differs from a linear sum only by f32 rounding
        for (int i = 0; i < n; ++i) { ref += static_cast<crd::f64>(static_cast<float>(x[static_cast<crd::usize>(i)])); }
        crd::f64 mag = 1.0;
        for (int i = 0; i < n; ++i) { mag = mag > fabs64(x[static_cast<crd::usize>(i)]) ? mag : fabs64(x[static_cast<crd::usize>(i)]); }
        CHECK(fabs64(got - ref) < 1e-4 * static_cast<crd::f64>(n) * mag);
    }
    SECTION("max")
    {
        kir::KGraph g0(&alloc); kir::KGraph* graphs[2] = {&g0, &g0};
        const kir::ReducePlan plan = kir::build_reduce(graphs, n, kir::KOp::Max, 256);
        const crd::f64        got  = run_reduce(plan, x.data(), &alloc);
        crd::f64              ref  = x[0];
        for (int i = 1; i < n; ++i) { ref = ref > x[static_cast<crd::usize>(i)] ? ref : x[static_cast<crd::usize>(i)]; }
        CHECK(got == static_cast<crd::f64>(static_cast<float>(ref)));
    }
    SECTION("min")
    {
        kir::KGraph g0(&alloc); kir::KGraph* graphs[2] = {&g0, &g0};
        const kir::ReducePlan plan = kir::build_reduce(graphs, n, kir::KOp::Min, 256);
        const crd::f64        got  = run_reduce(plan, x.data(), &alloc);
        crd::f64              ref  = x[0];
        for (int i = 1; i < n; ++i) { ref = ref < x[static_cast<crd::usize>(i)] ? ref : x[static_cast<crd::usize>(i)]; }
        CHECK(got == static_cast<crd::f64>(static_cast<float>(ref)));
    }
}

TEST_CASE("B-cmp: CKIR 2-pass device reduction (65536 elems) == sum/min/max reference (f32)", "[kir][kernel][reduce]")
{
    constexpr int              n = 65536;
    crd::memory::TlsfAllocator alloc(128U << 20U);
    crd::containers::Array<crd::f64> x(&alloc);
    x.resize(n);
    for (int i = 0; i < n; ++i) { x[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 31 + 5) % 251) - 125.0; }

    kir::KGraph g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph* graphs[2] = {&g0, &g1};

    SECTION("sum")
    {
        const kir::ReducePlan plan = kir::build_reduce(graphs, n, kir::KOp::Add, 256, 64);
        REQUIRE_FALSE(plan.single_pass);
        const crd::f64 got = run_reduce(plan, x.data(), &alloc);
        crd::f64       ref = 0.0;
        for (int i = 0; i < n; ++i) { ref += static_cast<crd::f64>(static_cast<float>(x[static_cast<crd::usize>(i)])); }
        crd::f64 mag = 1.0;
        for (int i = 0; i < n; ++i) { mag = mag > fabs64(x[static_cast<crd::usize>(i)]) ? mag : fabs64(x[static_cast<crd::usize>(i)]); }
        CHECK(fabs64(got - ref) < 1e-4 * static_cast<crd::f64>(n) * mag);
    }
    SECTION("max")
    {
        const kir::ReducePlan plan = kir::build_reduce(graphs, n, kir::KOp::Max, 256, 64);
        const crd::f64        got  = run_reduce(plan, x.data(), &alloc);
        crd::f64              ref  = x[0];
        for (int i = 1; i < n; ++i) { ref = ref > x[static_cast<crd::usize>(i)] ? ref : x[static_cast<crd::usize>(i)]; }
        CHECK(got == static_cast<crd::f64>(static_cast<float>(ref)));
    }
    SECTION("min")
    {
        const kir::ReducePlan plan = kir::build_reduce(graphs, n, kir::KOp::Min, 256, 64);
        const crd::f64        got  = run_reduce(plan, x.data(), &alloc);
        crd::f64              ref  = x[0];
        for (int i = 1; i < n; ++i) { ref = ref < x[static_cast<crd::usize>(i)] ? ref : x[static_cast<crd::usize>(i)]; }
        CHECK(got == static_cast<crd::f64>(static_cast<float>(ref)));
    }
}
