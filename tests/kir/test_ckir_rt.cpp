// test_ckir_rt.cpp — D-007 B9/RT-1a: the CKIR inline ray-query IR (crd::kir acceleration-structure + trace_ray_closest).
// This gates the CPU ORACLE (the ground-truth brute-force ray-triangle in eval_cpu_kernel) against HAND-COMPUTED hits — an
// independent check that the reference is correct (the GPU rayQuery is then compared against this reference within tolerance
// in the gpu-context tests). A one-thread-per-ray kernel reads a ray from a buffer, traces it at the bound acceleration
// structure (whose binding holds the triangle geometry in the oracle), and writes the closest-hit distance.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_rt.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
// A one-thread-per-ray inline-ray-query kernel. Binding 0 = the acceleration structure (its geometry, in the oracle);
// binding 1 = the rays (6 floats each: origin.xyz, dir.xyz); binding 2 = the output closest-hit distance.
// REN-38: `n_rays` is the tail-thread guard bound — a dispatch rounds up to whole workgroups, so threads
// past the ray count would read OOB (silently on a robustness GPU, loudly in the oracle now).
kir::KEntry build_trace_kernel(kir::KGraph& g, int local_size, crd::u32 n_rays = 0U)
{
    const kir::Shape sh1 = kir::make_shape({1});
    const auto       cf  = [&](double v) { return g.constant(v, sh1, kir::DType::F32); };
    const auto       cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, kir::DType::U32); };

    const int as   = g.accel_struct_decl(0, 0);          // TLAS at (set 0, binding 0)
    const int rays = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int out  = g.buffer_decl(kir::DType::F32, 0, 2, true);

    const int mark = g.kernel_stmt_mark();
    const int tid  = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(static_cast<crd::u32>(local_size))),
                              g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int guard = n_rays > 0U ? g.stmt_if_begin(g.binary(kir::KOp::CmpLt, tid, cu(n_rays))) : -1;
    const int base = g.binary(kir::KOp::Mul, tid, cu(6U));
    const auto ld  = [&](crd::u32 k) { return g.buffer_load(rays, g.binary(kir::KOp::Add, base, cu(k))); };
    const int t    = g.trace_ray_closest(as, ld(0), ld(1), ld(2), ld(3), ld(4), ld(5), cf(0.001), cf(1.0e30));
    g.stmt_buffer_store(out, tid, t);

    if (guard >= 0) { g.stmt_if_end(guard); }
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(local_size);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
} // namespace

// B9/RT-1a: the inline-ray-query oracle returns correct closest-hit distances (hand-verified geometry).
TEST_CASE("B9/RT-1a: CKIR inline rayQuery oracle == hand-computed ray-triangle hits", "[kir][rt]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              k_n_rays = 4;
    const kir::KEntry          e      = build_trace_kernel(g, 64, static_cast<crd::u32>(k_n_rays));

    // Scene: ONE triangle at z = 2 spanning (0,0)-(1,0)-(0,1). Geometry buffer layout the oracle reads:
    // [triCount, v0.xyz, v1.xyz, v2.xyz].
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = 1.0;                                   // one triangle
    geo[1] = 0.0; geo[2] = 0.0; geo[3] = 2.0;       // v0 = (0,0,2)
    geo[4] = 1.0; geo[5] = 0.0; geo[6] = 2.0;       // v1 = (1,0,2)
    geo[7] = 0.0; geo[8] = 1.0; geo[9] = 2.0;       // v2 = (0,1,2)

    // Rays: [origin.xyz, dir.xyz] each.
    crd::containers::Array<crd::f64> rays(&alloc);
    rays.resize(static_cast<crd::usize>(k_n_rays) * 6U, 0.0);
    const double ray_data[k_n_rays][6] = {
        {0.2, 0.2, 0.0, 0.0, 0.0, 1.0},  // through the triangle interior, +z ⇒ hit at t = 2
        {0.2, 0.2, 0.0, 0.0, 0.0, -1.0}, // away from the triangle ⇒ miss
        {5.0, 5.0, 0.0, 0.0, 0.0, 1.0},  // parallel-ish but far outside the triangle ⇒ miss
        {0.1, 0.1, 1.0, 0.0, 0.0, 1.0},  // start closer (z=1), +z ⇒ hit at t = 1
    };
    for (int r = 0; r < k_n_rays; ++r)
    {
        for (int c = 0; c < 6; ++c) { rays[static_cast<crd::usize>(r) * 6U + static_cast<crd::usize>(c)] = ray_data[r][c]; }
    }
    crd::containers::Array<crd::f64> out(&alloc);
    out.resize(static_cast<crd::usize>(k_n_rays), 0.0);

    kir::KernelBuffer bufs[3] = {{geo.data(), static_cast<int>(geo.size()), 0, 0},
                                 {rays.data(), static_cast<int>(rays.size()), 0, 1},
                                 {out.data(), k_n_rays, 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, 64U, &alloc, 1U);

    INFO("t = [" << out[0] << ", " << out[1] << ", " << out[2] << ", " << out[3] << "]");
    CHECK(crd::math::abs(out[0] - 2.0) < 1.0e-9); // hit at z=2 from z=0
    CHECK(out[1] > 1.0e29);                       // miss (ray points away) ⇒ tmax
    CHECK(out[2] > 1.0e29);                       // miss (outside the triangle) ⇒ tmax
    CHECK(crd::math::abs(out[3] - 1.0) < 1.0e-9); // hit at z=2 from z=1 ⇒ t = 1
}

namespace
{
// Run the NEE path tracer under one light-sampling strategy on the CPU oracle over a fixed floor + occluder + area-light scene,
// and return the mean radiance (channel 0) averaged over all shading points. The scene: a diffuse floor (the shading points), a
// small occluder quad (prims 0-1) that casts a soft shadow, and a rectangular area light (prims 2-3) matching the analytic light
// in the config. Same geometry + same spp for every strategy ⇒ the means are directly comparable.
double run_pt_strategy(crd::memory::TlsfAllocator& alloc, kir::rt::PtStrategy strategy, crd::u32 spp, crd::u32 bounces = 2U)
{
    kir::rt::PathTraceNeeConfig cfg;
    cfg.samples      = spp;
    cfg.bounces      = bounces;
    cfg.albedo[0]    = 0.6F; cfg.albedo[1] = 0.6F; cfg.albedo[2] = 0.6F;
    cfg.light_p0[0]  = -1.5F; cfg.light_p0[1] = 3.0F; cfg.light_p0[2] = -1.5F;
    cfg.light_eu[0]  = 3.0F;  cfg.light_eu[1] = 0.0F; cfg.light_eu[2] = 0.0F;
    cfg.light_ev[0]  = 0.0F;  cfg.light_ev[1] = 0.0F; cfg.light_ev[2] = 3.0F;
    cfg.light_nl[0]  = 0.0F;  cfg.light_nl[1] = -1.0F; cfg.light_nl[2] = 0.0F;
    cfg.light_le[0]  = 8.0F;  cfg.light_le[1] = 8.0F;  cfg.light_le[2] = 8.0F;
    cfg.ntri         = 4U;
    cfg.light_prim0  = 2U;
    cfg.light_ntri   = 2U;
    cfg.strategy     = strategy;
    cfg.local_size   = 16U; // one workgroup covers exactly the 16 shading points ⇒ the CPU oracle interprets only 16 threads

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_pathtrace_nee_kernel(g, cfg);

    // AS geometry (oracle layout: [triCount, then per-tri v0.xyz v1.xyz v2.xyz]) — occluder (prims 0-1) + light quad (prims 2-3).
    const double tris[4][9] = {
        {-0.5, 2.0, -0.5, 0.5, 2.0, -0.5, 0.5, 2.0, 0.5},   // occluder tri 0 (1×1 quad at y=2)
        {-0.5, 2.0, -0.5, 0.5, 2.0, 0.5, -0.5, 2.0, 0.5},   // occluder tri 1
        {-1.5, 3.0, -1.5, 1.5, 3.0, -1.5, 1.5, 3.0, 1.5},   // light tri 2 (matches p0/eu/ev)
        {-1.5, 3.0, -1.5, 1.5, 3.0, 1.5, -1.5, 3.0, 1.5},   // light tri 3
    };
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + 4U * 9U, 0.0);
    geo[0] = 4.0;
    for (int t = 0; t < 4; ++t)
    {
        for (int c = 0; c < 9; ++c) { geo[1U + static_cast<crd::usize>(t) * 9U + static_cast<crd::usize>(c)] = tris[t][c]; }
    }
    // per-triangle flat normals: occluder faces up (its lit top bounces GI), light faces down.
    const double tn_data[12] = {0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0};

    // floor shading points: a 4×4 grid over [-2,2], normals up.
    constexpr crd::u32 k_n = 16U;
    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> nrm(&alloc);
    crd::containers::Array<crd::f64> tn(&alloc);
    crd::containers::Array<crd::f64> rad(&alloc);
    pos.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    nrm.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    tn.resize(12U, 0.0);
    rad.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    for (int i = 0; i < 12; ++i) { tn[static_cast<crd::usize>(i)] = tn_data[i]; }
    for (crd::u32 j = 0; j < 4U; ++j)
    {
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            const crd::u32 p = j * 4U + i;
            pos[p * 3U + 0U] = -2.0 + 4.0 / 3.0 * static_cast<double>(i);
            pos[p * 3U + 2U] = -2.0 + 4.0 / 3.0 * static_cast<double>(j);
            nrm[p * 3U + 1U] = 1.0;
        }
    }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos.data(), static_cast<int>(pos.size()), 0, 1}, {nrm.data(), static_cast<int>(nrm.size()), 0, 2}, {tn.data(), static_cast<int>(tn.size()), 0, 3}, {rad.data(), static_cast<int>(rad.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, cfg.local_size, &alloc, 1U);

    double mean = 0.0;
    for (crd::u32 p = 0; p < k_n; ++p) { mean += rad[p * 3U]; } // channel 0 (grey scene ⇒ all channels equal)
    return mean / static_cast<double>(k_n);
}

// Run the ReSTIR DI kernel on the CPU oracle over the SAME occluder + area-light scene + floor points as run_pt_strategy, and
// return the mean radiance (channel 0). ReSTIR-DI estimates DIRECT lighting only, so it is compared against pure NEE direct.
double run_restir(crd::memory::TlsfAllocator& alloc, crd::u32 frames, crd::u32 candidates)
{
    kir::rt::RestirDiConfig cfg;
    cfg.frames      = frames;
    cfg.candidates  = candidates;
    cfg.albedo[0]   = 0.6F; cfg.albedo[1] = 0.6F; cfg.albedo[2] = 0.6F;
    cfg.light_p0[0] = -1.5F; cfg.light_p0[1] = 3.0F; cfg.light_p0[2] = -1.5F;
    cfg.light_eu[0] = 3.0F;  cfg.light_eu[1] = 0.0F; cfg.light_eu[2] = 0.0F;
    cfg.light_ev[0] = 0.0F;  cfg.light_ev[1] = 0.0F; cfg.light_ev[2] = 3.0F;
    cfg.light_nl[0] = 0.0F;  cfg.light_nl[1] = -1.0F; cfg.light_nl[2] = 0.0F;
    cfg.light_le[0] = 8.0F;  cfg.light_le[1] = 8.0F;  cfg.light_le[2] = 8.0F;
    cfg.local_size  = 16U;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_restir_di_kernel(g, cfg);

    const double tris[4][9] = {
        {-0.5, 2.0, -0.5, 0.5, 2.0, -0.5, 0.5, 2.0, 0.5}, {-0.5, 2.0, -0.5, 0.5, 2.0, 0.5, -0.5, 2.0, 0.5},
        {-1.5, 3.0, -1.5, 1.5, 3.0, -1.5, 1.5, 3.0, 1.5}, {-1.5, 3.0, -1.5, 1.5, 3.0, 1.5, -1.5, 3.0, 1.5}};
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(1U + 4U * 9U, 0.0);
    geo[0] = 4.0;
    for (int t = 0; t < 4; ++t)
    {
        for (int c = 0; c < 9; ++c) { geo[1U + static_cast<crd::usize>(t) * 9U + static_cast<crd::usize>(c)] = tris[t][c]; }
    }
    constexpr crd::u32 k_n = 16U;
    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> nrm(&alloc);
    crd::containers::Array<crd::f64> rad(&alloc);
    pos.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    nrm.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    rad.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    for (crd::u32 j = 0; j < 4U; ++j)
    {
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            const crd::u32 p = j * 4U + i;
            pos[p * 3U + 0U] = -2.0 + 4.0 / 3.0 * static_cast<double>(i);
            pos[p * 3U + 2U] = -2.0 + 4.0 / 3.0 * static_cast<double>(j);
            nrm[p * 3U + 1U] = 1.0;
        }
    }
    kir::KernelBuffer bufs[4] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos.data(), static_cast<int>(pos.size()), 0, 1}, {nrm.data(), static_cast<int>(nrm.size()), 0, 2}, {rad.data(), static_cast<int>(rad.size()), 0, 3}};
    kir::eval_cpu_kernel(g, e, bufs, 4, cfg.local_size, &alloc, 1U);

    double mean = 0.0;
    for (crd::u32 p = 0; p < k_n; ++p) { mean += rad[p * 3U]; }
    return mean / static_cast<double>(k_n);
}
} // namespace

// B9/RT NEE+MIS: the Veach MULTIPLE-IMPORTANCE-SAMPLING validation — light-sampling (NEE), BSDF-sampling, and their MIS
// combination are three unbiased estimators of the SAME light-transport integral, so at high sample count their means must
// agree. MIS≈NEE tightly (both low-variance for a visible area light); MIS≈BSDF within BSDF's larger Monte-Carlo error. If MIS
// were mis-weighted (weights not summing to 1, or the geometry/pdf terms wrong) this would fail — it is the correctness gate.
TEST_CASE("B9/RT NEE+MIS: light/BSDF/MIS strategies converge to the same radiance (unbiased MIS)", "[kir][rt]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    const double mis  = run_pt_strategy(alloc, kir::rt::PtStrategy::Mis, 256U);
    const double nee  = run_pt_strategy(alloc, kir::rt::PtStrategy::Nee, 256U);
    const double bsdf = run_pt_strategy(alloc, kir::rt::PtStrategy::Bsdf, 256U);

    INFO("mean radiance  MIS=" << mis << "  NEE=" << nee << "  BSDF=" << bsdf);
    CHECK(mis > 0.05);                                  // the light actually illuminates the floor (not a dark/degenerate scene)
    CHECK(crd::math::abs(mis - nee) / mis < 0.03);      // MIS == NEE (both low-variance, tight at any spp) ⇒ MIS weights are correct
    CHECK(crd::math::abs(mis - bsdf) / mis < 0.15);     // MIS == BSDF within BSDF's larger variance ⇒ all three estimate the same integral
}

// B9/RT ReSTIR DI: the reservoir estimator is UNBIASED — averaged over many independent frames it must converge to the same
// DIRECT lighting as pure NEE light-sampling. RIS resamples M candidates toward the target p̂=f·Le·G, then the W = Σwᵢ/(M·p̂(y))
// contribution weight makes the single survivor an unbiased estimate. If the reservoir math (WRS replace test, the W weight, the
// p̂/source measure) were wrong the mean would be biased away from NEE — this is the correctness gate for the ReSTIR core.
TEST_CASE("B9/RT ReSTIR DI: RIS reservoir converges to the NEE direct-lighting reference (unbiased)", "[kir][rt]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U);
    const double ref    = run_pt_strategy(alloc, kir::rt::PtStrategy::Nee, 512U, 0U); // pure direct (bounces=0) NEE ground truth
    const double restir = run_restir(alloc, 192U, 16U);                               // ReSTIR DI, 192 frames × M=16 candidates

    INFO("direct radiance  NEE(ref)=" << ref << "  ReSTIR-DI=" << restir);
    CHECK(ref > 0.05);                                     // the direct light illuminates the floor
    CHECK(crd::math::abs(restir - ref) / ref < 0.05);      // ReSTIR-DI == NEE direct ⇒ the RIS reservoir + W weight are unbiased
}

namespace
{
// Run the many-lights NEE kernel over `nlights` of the 4-light set (a dummy triangle far below keeps every upward shadow ray
// unoccluded, isolating the light-SELECTION math from shadows) and return the mean channel-0 radiance over the floor points.
double run_manylight(crd::memory::TlsfAllocator& alloc, const double* lights, crd::u32 nlights, crd::u32 spp)
{
    kir::rt::ManyLightConfig cfg;
    cfg.samples = spp;
    cfg.nlights = nlights;
    cfg.albedo[0] = 0.6F; cfg.albedo[1] = 0.6F; cfg.albedo[2] = 0.6F;
    cfg.local_size = 16U;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::rt::build_manylight_nee_kernel(g, cfg);

    // AS = one dummy triangle far below the floor (y=-10) — upward shadow rays never hit it ⇒ V=1 everywhere.
    crd::containers::Array<crd::f64> geo(&alloc);
    geo.resize(10U, 0.0);
    geo[0] = 1.0;
    const double dumb[9] = {-1.0, -10.0, -1.0, 1.0, -10.0, -1.0, 0.0, -10.0, 1.0};
    for (int i = 0; i < 9; ++i) { geo[static_cast<crd::usize>(i) + 1U] = dumb[i]; }

    constexpr crd::u32 k_n = 16U;
    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> nrm(&alloc);
    crd::containers::Array<crd::f64> lbuf(&alloc);
    crd::containers::Array<crd::f64> rad(&alloc);
    pos.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    nrm.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    lbuf.resize(static_cast<crd::usize>(nlights) * 15U, 0.0);
    rad.resize(static_cast<crd::usize>(k_n) * 3U, 0.0);
    for (crd::u32 i = 0; i < nlights * 15U; ++i) { lbuf[i] = lights[i]; }
    for (crd::u32 j = 0; j < 4U; ++j)
    {
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            const crd::u32 p = j * 4U + i;
            pos[p * 3U + 0U] = -2.0 + 4.0 / 3.0 * static_cast<double>(i);
            pos[p * 3U + 2U] = -2.0 + 4.0 / 3.0 * static_cast<double>(j);
            nrm[p * 3U + 1U] = 1.0;
        }
    }
    kir::KernelBuffer bufs[5] = {{geo.data(), static_cast<int>(geo.size()), 0, 0}, {pos.data(), static_cast<int>(pos.size()), 0, 1}, {nrm.data(), static_cast<int>(nrm.size()), 0, 2}, {lbuf.data(), static_cast<int>(lbuf.size()), 0, 3}, {rad.data(), static_cast<int>(rad.size()), 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, cfg.local_size, &alloc, 1U);

    double mean = 0.0;
    for (crd::u32 p = 0; p < k_n; ++p) { mean += rad[p * 3U]; }
    return mean / static_cast<double>(k_n);
}
} // namespace

// B9/RT many-lights: uniform light selection is UNBIASED — sampling one of N lights per sample (with the 1/N·1/area pdf ⇒ ×N·area
// weight) must, over many samples, equal the SUM of the per-light direct-lighting integrals. If the N·area weighting or the
// ⌊u·N⌋ selection were wrong the total would drift from the sum. The substrate a light-BVH / multi-light ReSTIR resamples over.
TEST_CASE("B9/RT many-lights: uniform light-selection NEE == sum of per-light direct (unbiased)", "[kir][rt]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U);
    // 4 grey area lights (15 floats each: p0, eu, ev, nl(down), Le) above the floor at different spots + brightnesses.
    const double lights[4 * 15] = {
        -3.0, 3.0, -3.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, 0.0, 6.0, 6.0, 6.0,
         2.0, 4.0, -2.0, 1.5, 0.0, 0.0, 0.0, 0.0, 1.5, 0.0, -1.0, 0.0, 4.0, 4.0, 4.0,
        -2.0, 3.5,  2.0, 1.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, -1.0, 0.0, 5.0, 5.0, 5.0,
         2.5, 5.0,  2.5, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, 0.0, 3.0, 3.0, 3.0};

    // The CPU oracle is too slow for a high-spp convergence run of this heavier kernel (light select + cross-product + trace per
    // sample), so here we only gate that the many-lights kernel EVALUATES correctly in the oracle (Floor/Cast selection, the
    // 15-float light-buffer reads, the |eu×ev| area) and lands in the right ballpark vs the per-light sum at a modest spp. The
    // rigorous high-spp unbiasedness (N-light == Σ per-light to a few %) runs on the fast GPU — see the Vulkan RT-7 test.
    double sum_of_lights = 0.0;
    for (crd::u32 l = 0; l < 4U; ++l) { sum_of_lights += run_manylight(alloc, &lights[l * 15U], 1U, 64U); }
    const double all = run_manylight(alloc, lights, 4U, 128U);

    INFO("many-lights (smoke): Σ per-light=" << sum_of_lights << "  N-light kernel=" << all);
    CHECK(sum_of_lights > 0.05);                                  // the lights illuminate the floor (kernel evaluates + is finite)
    CHECK(all > 0.05);
    CHECK(crd::math::abs(all - sum_of_lights) / sum_of_lights < 0.20); // right ballpark at low spp (tight check is the GPU test)
}
