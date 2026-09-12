// test_ckir_scan.cpp -- B-cmp: the CKIR device-wide PREFIX SUM (ckir_scan.hpp) on the CPU oracle. Validates the block-scan
// index map (coalesced striped I/O + blocked shared scan, Hillis-Steele cross-thread) single-workgroup AND the 3-pass device
// scan, inclusive + exclusive, vs a direct sequential reference. GPU bit-exactness (Vulkan/DX12) is the next slice.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_scan.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
// Single-workgroup scan: run the block kernel (grid 1) over `n` elements → out[0..n-1]. bsum is a dummy (unused).
void run_scan_block(kir::KGraph& g, const kir::KEntry& e, int n, const crd::f64* xin, crd::f64* out, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::f64> in(alloc);
    in.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i) { in[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xin[i])); }
    crd::f64          bsum   = 0.0;
    kir::KernelBuffer kb[3]  = {{in.data(), n, 0, 0}, {out, n, 0, 1}, {&bsum, 1, 0, 2}};
    kir::eval_cpu_kernel(g, e, kb, 3, e.local_size[0], alloc, 1U);
}

// Full device scan through the oracle: pass 0 → local scan + blocksums; pass 1 → offsets; pass 2 → final.
void run_scan(const kir::ScanPlan& plan, const crd::f64* xin, crd::f64* out, crd::memory::IAllocator* alloc)
{
    const int n = plan.n;
    crd::containers::Array<crd::f64> in(alloc);
    in.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i) { in[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xin[i])); }

    if (plan.single_pass)
    {
        run_scan_block(*plan.block_graph, plan.block, n, xin, out, alloc);
        return;
    }
    crd::containers::Array<crd::f64> local(alloc); local.resize(static_cast<crd::usize>(n), 0.0);
    crd::containers::Array<crd::f64> bsum(alloc);  bsum.resize(static_cast<crd::usize>(plan.nblocks), 0.0);
    crd::containers::Array<crd::f64> off(alloc);   off.resize(static_cast<crd::usize>(plan.nblocks), 0.0);

    kir::KernelBuffer p0[3] = {{in.data(), n, 0, 0}, {local.data(), n, 0, 1}, {bsum.data(), plan.nblocks, 0, 2}};
    kir::eval_cpu_kernel(*plan.block_graph, plan.block, p0, 3, plan.block.local_size[0], alloc, static_cast<crd::u32>(plan.nblocks));
    crd::f64          dummy   = 0.0;
    kir::KernelBuffer p1[3] = {{bsum.data(), plan.nblocks, 0, 0}, {off.data(), plan.nblocks, 0, 1}, {&dummy, 1, 0, 2}};
    kir::eval_cpu_kernel(*plan.sums_graph, plan.scan_sums, p1, 3, plan.scan_sums.local_size[0], alloc, 1U);
    kir::KernelBuffer p2[3] = {{local.data(), n, 0, 0}, {off.data(), plan.nblocks, 0, 1}, {out, n, 0, 2}};
    kir::eval_cpu_kernel(*plan.addoff_graph, plan.add_off, p2, 3, plan.add_off.local_size[0], alloc, static_cast<crd::u32>(plan.nblocks));
}
} // namespace

TEST_CASE("B-cmp: CKIR single-workgroup SCAN == sequential prefix sum (exact for unit input)", "[kir][kernel][scan]")
{
    constexpr int              n = 1024;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f64> x(&alloc); x.resize(n);
    for (int i = 0; i < n; ++i) { x[static_cast<crd::usize>(i)] = 1.0; } // unit ⇒ scan is EXACT in f32 (n < 2^24)
    crd::f64 out[n];

    SECTION("inclusive: out[i] == i+1")
    {
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::build_scan_block(g, n, 256, true, false);
        run_scan_block(g, e, n, x.data(), out, &alloc);
        int bad = 0;
        for (int i = 0; i < n; ++i) { if (out[i] != static_cast<crd::f64>(i + 1)) { ++bad; } }
        CHECK(bad == 0);
    }
    SECTION("exclusive: out[i] == i")
    {
        kir::KGraph g(&alloc);
        const kir::KEntry e = kir::build_scan_block(g, n, 256, false, false);
        run_scan_block(g, e, n, x.data(), out, &alloc);
        int bad = 0;
        for (int i = 0; i < n; ++i) { if (out[i] != static_cast<crd::f64>(i)) { ++bad; } }
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp: CKIR 3-pass DEVICE SCAN == sequential prefix sum", "[kir][kernel][scan]")
{
    constexpr int              n = 65536;
    crd::memory::TlsfAllocator alloc(128U << 20U);
    crd::containers::Array<crd::f64> x(&alloc); x.resize(n);
    for (int i = 0; i < n; ++i) { x[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 7 + 3) % 5); } // small ints ⇒ exact

    crd::f64* out = static_cast<crd::f64*>(alloc.allocate(sizeof(crd::f64) * n, alignof(crd::f64)));

    SECTION("inclusive")
    {
        kir::KGraph g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc); kir::KGraph* gs[3] = {&g0, &g1, &g2};
        const kir::ScanPlan plan = kir::build_scan(gs, n, true, 256, 64);
        REQUIRE_FALSE(plan.single_pass);
        run_scan(plan, x.data(), out, &alloc);
        crd::f64 run = 0.0; int bad = 0;
        for (int i = 0; i < n; ++i) { run += x[static_cast<crd::usize>(i)]; if (out[i] != run) { ++bad; } }
        CHECK(bad == 0);
    }
    SECTION("exclusive")
    {
        kir::KGraph g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc); kir::KGraph* gs[3] = {&g0, &g1, &g2};
        const kir::ScanPlan plan = kir::build_scan(gs, n, false, 256, 64);
        run_scan(plan, x.data(), out, &alloc);
        crd::f64 run = 0.0; int bad = 0;
        for (int i = 0; i < n; ++i) { if (out[i] != run) { ++bad; } run += x[static_cast<crd::usize>(i)]; }
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp: CKIR SINGLE-PASS chained scan == sequential prefix sum (oracle)", "[kir][kernel][scan]")
{
    constexpr int n       = 65536;
    constexpr int threads = 256;
    constexpr int nblocks = 64;
    constexpr int epb     = n / nblocks; // elems per block
    crd::memory::TlsfAllocator alloc(128U << 20U);
    crd::containers::Array<crd::f64> x(&alloc); x.resize(n);
    for (int i = 0; i < n; ++i) { x[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 7 + 3) % 5); }

    for (int incl = 0; incl < 2; ++incl)
    {
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::build_scan_single_pass(g, epb, threads, incl != 0);

        crd::containers::Array<crd::f64> in(&alloc);  in.resize(n);
        for (int i = 0; i < n; ++i) { in[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(x[static_cast<crd::usize>(i)])); }
        crd::containers::Array<crd::f64> out(&alloc); out.resize(n, -1.0);
        crd::containers::Array<crd::f64> agg(&alloc); agg.resize(static_cast<crd::usize>(nblocks), 0.0);
        crd::containers::Array<crd::f64> flg(&alloc); flg.resize(static_cast<crd::usize>(nblocks), 0.0);
        kir::KernelBuffer kb[4] = {{in.data(), n, 0, 0}, {out.data(), n, 0, 1}, {agg.data(), nblocks, 0, 2}, {flg.data(), nblocks, 0, 3}};
        kir::eval_cpu_kernel(g, e, kb, 4, e.local_size[0], &alloc, static_cast<crd::u32>(nblocks));

        crd::f64 run = 0.0; int bad = 0;
        for (int i = 0; i < n; ++i)
        {
            if (incl != 0) { run += x[static_cast<crd::usize>(i)]; if (out[static_cast<crd::usize>(i)] != run) { ++bad; } }
            else { if (out[static_cast<crd::usize>(i)] != run) { ++bad; } run += x[static_cast<crd::usize>(i)]; }
        }
        CHECK(bad == 0);
    }
}
