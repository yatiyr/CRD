// test_ckir_curve_rt.cpp — D-007 B18-f: the PROCEDURAL CURVE ray-query statement (TraceRayCurves).
//
// The CPU oracle for this statement brute-forces every segment; the GPU narrows the same query to candidate AABBs and
// runs the intersector per candidate. Both must land on the same hit. These CPU-side gates pin the ORACLE itself against
// closed-form geometry first — an oracle that agrees with a GPU only because both are wrong the same way proves nothing,
// and the LSS maths is duplicated between ckir_lss.hpp (IR nodes) and the oracle (plain C++), so they must be shown to
// agree with each other AND with hand-computed answers.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_lss.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>

namespace kir = crd::kir;

namespace
{
constexpr int kRays = 64;

// A kernel whose only job is one TraceRayCurves: out[tid] = {t, u}.
[[nodiscard]] kir::KEntry build_curve_query(kir::KGraph& g)
{
    const kir::Shape shu = kir::make_shape({1});
    const auto       cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, kir::DType::U32); };
    const auto       ks  = [&](double v) { return g.constant(v, shu, kir::DType::F32); };

    const int as    = g.accel_struct_decl(0, 0);
    const int seg_b = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int ray_b = g.buffer_decl(kir::DType::F32, 0, 2, false);
    const int out_b = g.buffer_decl(kir::DType::F32, 0, 3, true);
    const int tid   = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(kir::KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int rb   = g.binary(kir::KOp::Mul, tid, cu(6));
    const auto rl  = [&](int k) {
        const int v = g.buffer_load(ray_b, g.binary(kir::KOp::Add, rb, cu(static_cast<crd::u32>(k))));
        g.stmt_materialize(v);
        return v;
    };
    const int ox = rl(0);
    const int oy = rl(1);
    const int oz = rl(2);
    const int dx = rl(3);
    const int dy = rl(4);
    const int dz = rl(5);
    const auto hit = g.trace_ray_curves(as, seg_b, ox, oy, oz, dx, dy, dz, ks(1.0e-4), ks(1.0e30));

    const int ob = g.binary(kir::KOp::Mul, tid, cu(2));
    g.stmt_buffer_store(out_b, ob, hit.t);
    g.stmt_buffer_store(out_b, g.binary(kir::KOp::Add, ob, cu(1)), hit.u);

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

void run(crd::memory::IAllocator& alloc, crd::containers::Array<double>& segs, crd::containers::Array<double>& rays,
         crd::containers::Array<double>& out)
{
    kir::KGraph       g(&alloc);
    const kir::KEntry e = build_curve_query(g);
    out.resize(static_cast<crd::usize>(kRays) * 2U, 0.0);
    // binding 0 is the AS; in the oracle it carries no geometry for curves (the segments do), so a stub suffices.
    crd::containers::Array<double> as_stub(&alloc);
    as_stub.resize(1U, 0.0);
    kir::KernelBuffer bb[4] = {{as_stub.data(), 1, 0, 0},
                               {segs.data(), static_cast<int>(segs.size()), 0, 1},
                               {rays.data(), kRays * 6, 0, 2},
                               {out.data(), kRays * 2, 0, 3}};
    kir::eval_cpu_kernel(g, e, bb, 4, e.local_size[0], &alloc, 1U);
}
} // namespace

TEST_CASE("ckir TraceRayCurves oracle matches closed-form capsule geometry", "[ckir][lss][curvert]")
{
    crd::memory::TlsfAllocator     alloc(32U << 20U, nullptr, "curve-rt");
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> out(&alloc);

    // one capsule along +x, constant radius — the same configuration the ckir_lss.hpp gates use, so the IR-node maths
    // and this plain-C++ oracle are being held to the SAME hand-computed answer.
    const double r = 0.5;
    segs.resize(8U, 0.0);
    segs[0U] = 0.0; segs[1U] = 0.0; segs[2U] = 0.0; segs[3U] = r;
    segs[4U] = 4.0; segs[5U] = 0.0; segs[6U] = 0.0; segs[7U] = r;
    rays.resize(static_cast<crd::usize>(kRays) * 6U, 0.0);
    for (int i = 0; i < 8; ++i)
    {
        const crd::usize o = static_cast<crd::usize>(i) * 6U;
        rays[o + 0U] = 0.5 + 0.4 * static_cast<double>(i);
        rays[o + 1U] = -3.0;
        rays[o + 3U] = 0.0; rays[o + 4U] = 1.0; rays[o + 5U] = 0.0;
    }
    // misses, offset in z beyond the radius
    for (int i = 8; i < 12; ++i)
    {
        const crd::usize o = static_cast<crd::usize>(i) * 6U;
        rays[o + 0U] = 2.0; rays[o + 1U] = -3.0; rays[o + 2U] = r + 0.05 + 0.1 * static_cast<double>(i - 8);
        rays[o + 4U] = 1.0;
    }
    run(alloc, segs, rays, out);

    for (int i = 0; i < 8; ++i)
    {
        const double t = out[static_cast<crd::usize>(i) * 2U + 0U];
        const double u = out[static_cast<crd::usize>(i) * 2U + 1U];
        INFO("ray " << i << ": t = " << t << " want " << (3.0 - r) << ", u = " << u);
        CHECK(crd::math::abs(t - (3.0 - r)) < 1.0e-6);
        CHECK(crd::math::abs(u - (0.5 + 0.4 * static_cast<double>(i)) / 4.0) < 1.0e-5);
    }
    for (int i = 8; i < 12; ++i) { CHECK(out[static_cast<crd::usize>(i) * 2U + 0U] > 1.0e29); }
}

TEST_CASE("ckir TraceRayCurves oracle agrees with the ckir_lss trace kernel", "[ckir][lss][curvert]")
{
    crd::memory::TlsfAllocator     alloc(64U << 20U, nullptr, "curve-xcheck");
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> out_rt(&alloc);
    crd::containers::Array<double> out_lss(&alloc);

    // ⭐ CROSS-CHECK OF TWO INDEPENDENT IMPLEMENTATIONS. The same intersection is written twice: as CKIR nodes in
    //   ckir_lss.hpp (which is what the GPU shader is generated from) and as plain C++ in the oracle's TraceRayCurves
    //   case. Duplicated maths that drifts apart is exactly the failure a single-implementation test cannot see, so hold
    //   them against each other over a randomised scene as well as against the closed-form cases above.
    constexpr int nseg = 8;
    segs.resize(static_cast<crd::usize>(nseg) * 8U, 0.0);
    crd::u32   st  = 0xC0FFEE11U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int s = 0; s < nseg; ++s)
    {
        const crd::usize o = static_cast<crd::usize>(s) * 8U;
        for (int k = 0; k < 3; ++k) { segs[o + static_cast<crd::usize>(k)] = rnd() * 2.0 - 1.0; }
        segs[o + 3U] = 0.05 + rnd() * 0.25;
        for (int k = 0; k < 3; ++k) { segs[o + 4U + static_cast<crd::usize>(k)] = rnd() * 2.0 - 1.0; }
        segs[o + 7U] = 0.05 + rnd() * 0.25;
    }
    rays.resize(static_cast<crd::usize>(kRays) * 6U, 0.0);
    for (int i = 0; i < kRays; ++i)
    {
        const crd::usize o = static_cast<crd::usize>(i) * 6U;
        rays[o + 0U] = rnd() * 4.0 - 2.0;
        rays[o + 1U] = -3.0;
        rays[o + 2U] = rnd() * 4.0 - 2.0;
        rays[o + 3U] = rnd() * 0.4 - 0.2;
        rays[o + 4U] = 1.0;
        rays[o + 5U] = rnd() * 0.4 - 0.2;
    }
    run(alloc, segs, rays, out_rt);

    kir::lss::LssTraceConfig lc;
    lc.segments = nseg;
    kir::KGraph       g2(&alloc);
    const kir::KEntry e2 = kir::lss::build_lss_trace_kernel(g2, lc);
    out_lss.resize(static_cast<crd::usize>(kRays) * 2U, 0.0);
    kir::KernelBuffer bb[3] = {{rays.data(), kRays * 6, 0, 0},
                               {segs.data(), nseg * 8, 0, 1},
                               {out_lss.data(), kRays * 2, 0, 2}};
    kir::eval_cpu_kernel(g2, e2, bb, 3, e2.local_size[0], &alloc, 1U);

    int    hits    = 0;
    int    ties    = 0;
    double worst_t = 0.0;
    double worst_u = 0.0;
    for (int i = 0; i < kRays; ++i)
    {
        const double a = out_rt[static_cast<crd::usize>(i) * 2U + 0U];
        const double b = out_lss[static_cast<crd::usize>(i) * 2U + 0U];
        const bool   ha = a < 1.0e29;
        const bool   hb = b < 1.0e29;
        INFO("ray " << i << ": TraceRayCurves t = " << a << ", ckir_lss t = " << b);
        CHECK(ha == hb); // the two implementations must agree on WHICH rays hit, first of all
        if (!ha) { continue; }
        ++hits;
        const double dt = crd::math::abs(a - b);
        const double du = crd::math::abs(out_rt[static_cast<crd::usize>(i) * 2U + 1U]
                                         - out_lss[static_cast<crd::usize>(i) * 2U + 1U]);
        if (dt > worst_t) { worst_t = dt; }
        // ⚠ A TIE IS NOT A DEFECT. When two DIFFERENT segments' end-caps sit at numerically equal distance, strict `<`
        //   resolves differently under different f32 association, and the winner's axial coordinate flips between the
        //   tip of one (u = 1) and the root of another (u = 0). Both answers are geometrically correct — which
        //   primitive wins an exact tie is implementation-defined, the same contract this codebase already states for
        //   RT traversal. Recognise a tie precisely (t agreeing to ulp AND both u at cap endpoints) and count it;
        //   anything else is a real disagreement and must fail.
        const double ua = out_rt[static_cast<crd::usize>(i) * 2U + 1U];
        const double ub = out_lss[static_cast<crd::usize>(i) * 2U + 1U];
        const bool   caps = (ua < 1.0e-6 || ua > 1.0 - 1.0e-6) && (ub < 1.0e-6 || ub > 1.0 - 1.0e-6);
        if (du > 1.0e-4 && dt < 1.0e-5 && caps) { ++ties; continue; }
        if (du > worst_u) { worst_u = du; }
    }
    INFO("hits " << hits << " of " << kRays << ", worst |dt| = " << worst_t << ", worst |du| = " << worst_u
                 << ", cap ties " << ties);
    CHECK(hits > 4);           // the randomised scene must actually be hit, or the cross-check is vacuous
    CHECK(worst_t < 1.0e-4);   // DISTANCE is the load-bearing claim and must agree tightly
    CHECK(worst_u < 1.0e-4);   // ...as must the axial coordinate, outside a genuine cap tie
    CHECK(ties <= 2);          // and ties must stay RARE — a flood of them would mean something systematic
}
