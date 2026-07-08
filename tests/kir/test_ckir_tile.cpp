// test_ckir_tile.cpp — Phase 3.1.6 v17-b: the CKIR-Tile schedule IR + Graph→Tile lowering (`select_schedule`). CPU-only
// (no GPU) — validates that lowering picks the v17-e WarpTiled crush schedule exactly when its shape constraints hold,
// and falls back to Naive (the bit-exact reference lowering) otherwise. ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_tile.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

TEST_CASE("v17-b: Graph->Tile lowering picks WarpTiled only when the shape allows", "[kir][tile]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);

    SECTION("large 128-multiple square matmul -> WarpTiled (the crush schedule)")
    {
        kir::KGraph g(&alloc);
        const int   a   = g.input(kir::make_shape({256, 256}), kir::DType::F32);
        const int   b   = g.input(kir::make_shape({256, 256}), kir::DType::F32);
        const int   c   = g.contract(a, b);
        const auto  sch = kir::select_schedule(g, c);
        REQUIRE(sch.kind == kir::Sched::WarpTiled);
        CHECK(sch.bm == 128);
        CHECK(sch.bn == 128);
        CHECK(sch.bk == 8);
        CHECK(sch.nt == 256);
        CHECK(sch.double_buffer);
        CHECK(sch.fma); // fast tier
    }
    SECTION("rectangular 128-multiple with K multiple of 8 -> WarpTiled")
    {
        kir::KGraph g(&alloc);
        const int   a = g.input(kir::make_shape({256, 64}), kir::DType::F32);
        const int   b = g.input(kir::make_shape({64, 384}), kir::DType::F32);
        CHECK(kir::select_schedule(g, g.contract(a, b)).kind == kir::Sched::WarpTiled);
    }
    SECTION("small / non-128-multiple matmul -> Naive (stays bit-exact)")
    {
        kir::KGraph g(&alloc);
        const int   a = g.input(kir::make_shape({32, 48}), kir::DType::F32);
        const int   b = g.input(kir::make_shape({48, 24}), kir::DType::F32);
        CHECK(kir::select_schedule(g, g.contract(a, b)).kind == kir::Sched::Naive);
    }
    SECTION("M not a multiple of the block tile -> Naive")
    {
        kir::KGraph g(&alloc);
        const int   a = g.input(kir::make_shape({200, 128}), kir::DType::F32);
        const int   b = g.input(kir::make_shape({128, 128}), kir::DType::F32);
        CHECK(kir::select_schedule(g, g.contract(a, b)).kind == kir::Sched::Naive);
    }
    SECTION("K not a multiple of 8 -> Naive")
    {
        kir::KGraph g(&alloc);
        const int   a = g.input(kir::make_shape({128, 130}), kir::DType::F32);
        const int   b = g.input(kir::make_shape({130, 128}), kir::DType::F32);
        CHECK(kir::select_schedule(g, g.contract(a, b)).kind == kir::Sched::Naive);
    }
    SECTION("batched matmul -> Naive (batched tiled not yet scheduled)")
    {
        kir::KGraph g(&alloc);
        const int   a = g.input(kir::make_shape({4, 128, 128}), kir::DType::F32);
        const int   b = g.input(kir::make_shape({4, 128, 128}), kir::DType::F32);
        CHECK(kir::select_schedule(g, g.contract(a, b)).kind == kir::Sched::Naive);
    }
    SECTION("non-Contract op -> Naive")
    {
        kir::KGraph g(&alloc);
        const int   a = g.input(kir::make_shape({256}), kir::DType::F32);
        const int   b = g.input(kir::make_shape({256}), kir::DType::F32);
        CHECK(kir::select_schedule(g, g.binary(kir::KOp::Add, a, b)).kind == kir::Sched::Naive);
    }
}
