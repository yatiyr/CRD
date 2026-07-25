// test_ckir_hair_grazing.cpp — B18-a: the BCSDF at the GRAZING ENTRY POINT, |h| -> 1.
//
// h is where the ray enters the fibre, in radius units, so |h| = 1 is a ray clipping the very edge. The white-furnace
// test integrates over all h and so barely notices a narrow bad band at the limit — but a REALISTIC fibre is ~68 um
// and therefore almost entirely silhouette, so at film scale that band is where most rays actually land. This probes
// it directly instead of averaging it away.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace kir = crd::kir;

TEST_CASE("B18-a probe: hair BCSDF across the grazing limit", "[.][ckir][hair][probe]")
{
    crd::memory::TlsfAllocator     alloc(96U << 20U, nullptr, "grazing");
    crd::containers::Array<double> hbuf(&alloc);
    crd::containers::Array<double> out(&alloc);
    constexpr int k_n = 64;
    hbuf.resize(static_cast<crd::usize>(k_n), 0.0);
    out.resize(static_cast<crd::usize>(k_n) * 4U, 0.0);
    for (int i = 0; i < k_n; ++i) { hbuf[static_cast<crd::usize>(i)] = static_cast<double>(i) / static_cast<double>(k_n - 1); }

    kir::KGraph      g(&alloc);
    const kir::Shape shu = kir::make_shape({1});
    const auto       cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, kir::DType::U32); };
    const auto       ks  = [&](double v) { return g.constant(v, shu, kir::DType::F32); };
    const int        hb  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int        ob  = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int        tid = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(64)),
                              g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int mark = g.kernel_stmt_mark();
    const int hv   = g.buffer_load(hb, tid);
    g.stmt_materialize(hv);

    // four light azimuths, so a narrow lobe cannot masquerade as a global collapse
    const double phis[4] = {0.0, 1.0471975512, 2.0943951024, 3.1415926536};
    const int    b4      = g.binary(kir::KOp::Mul, tid, cu(4));
    for (int k = 0; k < 4; ++k)
    {
        const int f = kir::hair::hair_bcsdf_eval_angles(g, ks(0.0), ks(1.0), ks(0.0), ks(0.08), ks(0.996795), ks(phis[k]),
                                                        hv, ks(1.55), ks(0.20), ks(0.25), ks(0.30), ks(2.8));
        g.stmt_buffer_store(ob, g.binary(kir::KOp::Add, b4, cu(static_cast<crd::u32>(k))), f);
    }
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    kir::KernelBuffer bb[2] = {{hbuf.data(), k_n, 0, 0}, {out.data(), k_n * 4, 0, 1}};
    kir::eval_cpu_kernel(g, e, bb, 2, 64U, &alloc, 1U);

    std::printf("     h        f(phi=0)     f(60deg)     f(120deg)    f(180deg)\n");
    for (int i = 0; i < k_n; ++i)
    {
        const crd::usize o = static_cast<crd::usize>(i) * 4U;
        if (i < 48 && (i % 6) != 0) { continue; } // coarse below 0.75, every sample above
        std::printf("  %7.4f  %11.5f  %11.5f  %11.5f  %11.5f\n", hbuf[static_cast<crd::usize>(i)], out[o], out[o + 1U],
                    out[o + 2U], out[o + 3U]);
    }
}
