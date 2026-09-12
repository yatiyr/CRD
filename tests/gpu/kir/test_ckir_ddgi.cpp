// test_ckir_ddgi.cpp — D-007 B14-b-1: the DDGI octahedral direction mapping (ckir_ddgi.hpp) on the CPU oracle. The gold
// foundation: decode(encode(d)) must recover d for EVERY unit direction (the whole sphere maps to the [−1,1]² square with no
// singularity). Verified by a full round-trip (dot(recovered, d) ≈ 1) + the Chebyshev visibility monotonicity. Portability
// (GPU == oracle) in test_vulkan_context.cpp.

#include <crd/kir/ckir_ddgi.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace kir = crd::kir;

namespace
{
crd::usize uz(int v) { return static_cast<crd::usize>(v); }
} // namespace

TEST_CASE("DDGI octahedral: decode(encode(d)) recovers EVERY unit direction (no singularity)", "[kir][ddgi]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    const int                  n = 1024;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::ddgi::build_ddgi_oct_roundtrip(g, n);

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 3));
    out.resize(uz(n * 3));
    crd::u32 s   = 271U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24) * 2.0 - 1.0; };
    for (int i = 0; i < n; ++i)
    {
        double dx = rnd();
        double dy = rnd();
        double dz = rnd();
        // avoid the degenerate zero vector; a couple of axis directions seeded explicitly for the corner/pole cases
        if (i == 0) { dx = 0.0; dy = 0.0; dz = 1.0; }
        if (i == 1) { dx = 0.0; dy = 0.0; dz = -1.0; }
        if (i == 2) { dx = 1.0; dy = 0.0; dz = 0.0; }
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz) + 1e-9;
        in[uz(i * 3 + 0)] = dx / len;
        in[uz(i * 3 + 1)] = dy / len;
        in[uz(i * 3 + 2)] = dz / len;
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 3, 0, 0}, {out.data(), n * 3, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    double min_dot = 1.0;
    for (int i = 0; i < n; ++i)
    {
        const double dot = in[uz(i * 3 + 0)] * out[uz(i * 3 + 0)] + in[uz(i * 3 + 1)] * out[uz(i * 3 + 1)] + in[uz(i * 3 + 2)] * out[uz(i * 3 + 2)];
        if (dot < min_dot) { min_dot = dot; }
    }
    CHECK(min_dot > 1.0 - 1e-5); // every recovered direction is parallel to its input (round-trip is lossless up to sqrt ULP)
}

// a tiny kernel to exercise chebyshev(): read (mean, mean2, dist) triples → write visibility.
namespace
{
kir::KEntry build_cheb(kir::KGraph& g)
{
    const auto ku = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), kir::make_shape({1}), kir::DType::U32); };
    const auto add = [&](int a, int b) { return g.binary(kir::KOp::Add, a, b); };
    const auto mul = [&](int a, int b) { return g.binary(kir::KOp::Mul, a, b); };
    const int in_buf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int out_buf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int p       = add(mul(g.builtin(kir::KBuiltin::WorkgroupIndex), ku(64)), g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int p3      = mul(p, ku(3));
    const int mark = g.kernel_stmt_mark();
    const int v = kir::ddgi::chebyshev(g, g.buffer_load(in_buf, p3), g.buffer_load(in_buf, add(p3, ku(1))), g.buffer_load(in_buf, add(p3, ku(2))));
    g.stmt_buffer_store(out_buf, p, v);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
} // namespace

namespace
{
// set up the 8-probe sample buffers; `irr_fill(pi,c)` gives probe pi's channel-c irradiance, `depth_mean(pi)` its depth mean.
struct DdgiScene
{
    kir::KGraph                      g;
    kir::KEntry                      e;
    int                              n, r;
    crd::containers::Array<crd::f64> pos, nrm, irr, dpt, out;
    explicit DdgiScene(crd::memory::IAllocator* a, int queries, int res) : g(a), n(queries), r(res), pos(a), nrm(a), irr(a), dpt(a), out(a)
    {
        kir::ddgi::DdgiConfig cfg;
        cfg.oct_res = res;
        e           = kir::ddgi::build_ddgi_sample(g, cfg);
        pos.resize(uz(n * 3));
        nrm.resize(uz(n * 3));
        irr.resize(uz(8 * r * r * 3));
        dpt.resize(uz(8 * r * r * 2));
        out.resize(uz(n * 3));
    }
    void run(crd::memory::IAllocator* a)
    {
        kir::KernelBuffer b[5] = {{pos.data(), n * 3, 0, 0}, {nrm.data(), n * 3, 0, 1}, {irr.data(), 8 * r * r * 3, 0, 2},
                                  {dpt.data(), 8 * r * r * 2, 0, 3}, {out.data(), n * 3, 0, 4}};
        kir::eval_cpu_kernel(g, e, b, 5, e.local_size[0], a, static_cast<crd::u32>(n / 64));
    }
};
} // namespace

TEST_CASE("DDGI sample: a UNIFORM probe field returns exactly that irradiance (trilinear+weights normalize)", "[kir][ddgi]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    const int                  r = 8;
    DdgiScene                  sc(&alloc, 64, r);
    // every probe, every octahedral texel = the same colour C; depth mean huge ⇒ everything lit.
    const double c[3] = {0.3, 0.5, 0.7};
    for (int pi = 0; pi < 8; ++pi)
    {
        for (int t = 0; t < r * r; ++t)
        {
            for (int ch = 0; ch < 3; ++ch) { sc.irr[uz(pi * r * r * 3 + t * 3 + ch)] = c[ch]; }
            sc.dpt[uz(pi * r * r * 2 + t * 2 + 0)] = 100.0;   // mean
            sc.dpt[uz(pi * r * r * 2 + t * 2 + 1)] = 10000.0; // mean²
        }
    }
    for (int p = 0; p < sc.n; ++p)
    {
        sc.pos[uz(p * 3 + 0)] = 0.3 + 0.01 * (p % 5); // various points inside the cell
        sc.pos[uz(p * 3 + 1)] = 0.5;
        sc.pos[uz(p * 3 + 2)] = 0.6;
        sc.nrm[uz(p * 3 + 1)] = 1.0; // normal +Y
    }
    sc.run(&alloc);
    double maxdev = 0.0;
    for (int p = 0; p < sc.n; ++p)
    {
        for (int ch = 0; ch < 3; ++ch) { maxdev = std::max(maxdev, std::abs(sc.out[uz(p * 3 + ch)] - c[ch])); }
    }
    CHECK(maxdev < 1e-4); // the full 8-probe blend of a constant field IS that constant (up to fp weighted-mean rounding)
}

TEST_CASE("DDGI sample: a FULL GRID indexes the right cell -- a bright probe lights only its own cell", "[kir][ddgi]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    const int                  r = 8;
    kir::ddgi::DdgiConfig      cfg;
    cfg.oct_res = r;
    cfg.grid_x = 4;
    cfg.grid_y = 4;
    cfg.grid_z = 4; // a 4³ probe field, unit spacing, origin 0 ⇒ world ∈ [0,3]³
    kir::KGraph       g(&alloc);
    const kir::KEntry e  = kir::ddgi::build_ddgi_sample(g, cfg);
    const int         np = cfg.probe_count();

    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> nrm(&alloc);
    crd::containers::Array<crd::f64> irr(&alloc);
    crd::containers::Array<crd::f64> dpt(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    const int nq = 64;
    pos.resize(uz(nq * 3));
    nrm.resize(uz(nq * 3));
    irr.resize(uz(np * r * r * 3));
    dpt.resize(uz(np * r * r * 2));
    out.resize(uz(nq * 3));
    // only probe (2,1,1) — flat index (1·4+1)·4+2 = 22 — is bright RED (uniform over its octahedral map); all else dark.
    const int bright = (1 * 4 + 1) * 4 + 2;
    for (int t = 0; t < r * r; ++t) { irr[uz(bright * r * r * 3 + t * 3 + 0)] = 1.0; }
    for (int pi = 0; pi < np; ++pi)
    {
        for (int t = 0; t < r * r; ++t)
        {
            dpt[uz(pi * r * r * 2 + t * 2 + 0)] = 50.0;
            dpt[uz(pi * r * r * 2 + t * 2 + 1)] = 2500.0;
        }
    }
    const auto probe_red = [&](double qx, double qy, double qz) {
        for (int p = 0; p < nq; ++p)
        {
            pos[uz(p * 3 + 0)] = qx;
            pos[uz(p * 3 + 1)] = qy;
            pos[uz(p * 3 + 2)] = qz;
            nrm[uz(p * 3 + 0)] = -0.577; // face the −x/−y/−z octant (toward probe (2,1,1) from just above it)
            nrm[uz(p * 3 + 1)] = -0.577;
            nrm[uz(p * 3 + 2)] = -0.577;
        }
        kir::KernelBuffer b[5] = {{pos.data(), nq * 3, 0, 0}, {nrm.data(), nq * 3, 0, 1}, {irr.data(), np * r * r * 3, 0, 2},
                                  {dpt.data(), np * r * r * 2, 0, 3}, {out.data(), nq * 3, 0, 4}};
        kir::eval_cpu_kernel(g, e, b, 5, e.local_size[0], &alloc, static_cast<crd::u32>(nq / 64));
        return out[uz(0)];
    };
    const double near_red = probe_red(2.1, 1.1, 1.1); // inside cell (2,1,1) — probe 22 is its (0,0,0) corner
    const double far_red  = probe_red(0.5, 0.5, 0.5); // cell (0,0,0) — probe 22 is NOT a corner
    CHECK(near_red > 0.1);            // the query in probe 22's cell picks up its red
    CHECK(far_red < near_red * 0.05); // a far cell does NOT — the grid indexing is correct (no cross-cell bleed)
}

TEST_CASE("DDGI sample: Chebyshev OCCLUSION stops light leak from an occluded probe", "[kir][ddgi]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    const int                  r = 8;
    // probe 0 (corner 0,0,0) is bright RED; the other 7 are dark. A query near probe 0 leaks red WITHOUT occlusion; WITH the
    // depth moments saying probe 0 is behind a wall (mean ≪ dist), its weight → 0 and the red does NOT leak.
    const auto build = [&](double p0_mean, double& red) {
        DdgiScene sc(&alloc, 64, r);
        for (int pi = 0; pi < 8; ++pi)
        {
            const double rr = (pi == 0) ? 1.0 : 0.0; // probe 0 red, others black
            for (int t = 0; t < r * r; ++t)
            {
                sc.irr[uz(pi * r * r * 3 + t * 3 + 0)] = rr;
                sc.dpt[uz(pi * r * r * 2 + t * 2 + 0)] = (pi == 0) ? p0_mean : 100.0;
                sc.dpt[uz(pi * r * r * 2 + t * 2 + 1)] = (pi == 0) ? (p0_mean * p0_mean + 0.0004) : 10000.0; // σ²≈4e-4 for probe 0
            }
        }
        for (int p = 0; p < sc.n; ++p)
        {
            sc.pos[uz(p * 3 + 0)] = 0.15;
            sc.pos[uz(p * 3 + 1)] = 0.15;
            sc.pos[uz(p * 3 + 2)] = 0.15;
            sc.nrm[uz(p * 3 + 1)] = 1.0;
        }
        sc.run(&alloc);
        red = sc.out[uz(0)];
    };
    double red_lit    = 0.0;
    double red_occl   = 0.0;
    build(100.0, red_lit);  // probe 0 lit ⇒ red leaks in
    build(0.02, red_occl);  // probe 0 occluded (dist ~0.26 ≫ mean 0.02) ⇒ Chebyshev kills its weight
    CHECK(red_lit > 0.05);              // the lit case genuinely picks up red from probe 0
    CHECK(red_occl < red_lit * 0.25);   // occlusion cuts the leaked red ≥4× (no light through the wall)
}

TEST_CASE("DDGI probe update: aligned rays ACCUMULATE irradiance + set the depth mean, with temporal hysteresis", "[kir][ddgi]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    const int                  r  = 8;
    const int                  nr = 32;
    kir::ddgi::DdgiConfig      cfg;
    cfg.oct_res  = r;
    cfg.num_rays = nr;
    kir::KGraph       g(&alloc);
    const kir::KEntry e   = kir::ddgi::build_ddgi_probe_update(g, cfg);
    const int         ntx = 8 * r * r; // total probe texels

    crd::containers::Array<crd::f64> rdir(&alloc);
    crd::containers::Array<crd::f64> rrad(&alloc);
    crd::containers::Array<crd::f64> rdst(&alloc);
    crd::containers::Array<crd::f64> pir(&alloc);
    crd::containers::Array<crd::f64> pdp(&alloc);
    crd::containers::Array<crd::f64> oir(&alloc);
    crd::containers::Array<crd::f64> odp(&alloc);
    rdir.resize(uz(8 * nr * 3));
    rrad.resize(uz(8 * nr * 3));
    rdst.resize(uz(8 * nr));
    pir.resize(uz(ntx * 3));
    pdp.resize(uz(ntx * 2));
    oir.resize(uz(ntx * 3));
    odp.resize(uz(ntx * 2));
    // every ray of every probe points +Y, radiance (0.8,0.2,0.1), hit distance 5.
    for (int i = 0; i < 8 * nr; ++i)
    {
        rdir[uz(i * 3 + 1)] = 1.0; // +Y
        rrad[uz(i * 3 + 0)] = 0.8;
        rrad[uz(i * 3 + 1)] = 0.2;
        rrad[uz(i * 3 + 2)] = 0.1;
        rdst[uz(i)]         = 5.0;
    }
    kir::KernelBuffer bufs[7] = {{rdir.data(), 8 * nr * 3, 0, 0}, {rrad.data(), 8 * nr * 3, 0, 1}, {rdst.data(), 8 * nr, 0, 2},
                                 {pir.data(), ntx * 3, 0, 3}, {pdp.data(), ntx * 2, 0, 4},
                                 {oir.data(), ntx * 3, 0, 5}, {odp.data(), ntx * 2, 0, 6}};
    const auto max_r = [&]() {
        double m = 0.0;
        int    at = 0;
        for (int t = 0; t < r * r; ++t) { if (oir[uz(t * 3)] > m) { m = oir[uz(t * 3)]; at = t; } } // probe 0's texels
        return std::pair<double, int>(m, at);
    };
    double r1 = 0.0;
    for (int f = 0; f < 40; ++f)
    {
        kir::eval_cpu_kernel(g, e, bufs, 7, e.local_size[0], &alloc, static_cast<crd::u32>(ntx / 64));
        if (f == 0) { r1 = max_r().first; }
        for (int i = 0; i < ntx * 3; ++i) { pir[uz(i)] = oir[uz(i)]; }
        for (int i = 0; i < ntx * 2; ++i) { pdp[uz(i)] = odp[uz(i)]; }
    }
    const auto rn = max_r();
    CHECK(r1 > 0.0);                          // one frame already deposits some irradiance in the +Y texel
    CHECK(rn.first > r1 * 3.0);               // temporal accumulation grows it substantially over 40 frames
    CHECK(rn.first > 0.3);                    // converging toward the ray radiance 0.8 (cosine-weighted, hysteresis-limited)
    CHECK(odp[uz(rn.second * 2)] > 2.0);      // the +Y texel's depth MEAN converged toward the 5.0 hit distance
    // a texel facing AWAY from +Y (e.g. the −Y direction) receives no aligned rays ⇒ stays ~0
    double min_r = 1.0;
    for (int t = 0; t < r * r; ++t) { if (oir[uz(t * 3)] < min_r) { min_r = oir[uz(t * 3)]; } }
    CHECK(min_r < 0.05);                      // back-facing texels never lit (cosine weight 0)
}

TEST_CASE("DDGI Chebyshev visibility: lit when nearer than the mean, occluded (->0) when far beyond it", "[kir][ddgi]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const int                  n = 64;
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = build_cheb(g);

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 3));
    out.resize(uz(n));
    // probe stored mean depth 10, mean² = 101 ⇒ σ² = 1. Vary the surface distance across pixels.
    for (int p = 0; p < n; ++p)
    {
        in[uz(p * 3 + 0)] = 10.0;                          // mean
        in[uz(p * 3 + 1)] = 101.0;                         // mean² (σ²=1)
        in[uz(p * 3 + 2)] = 5.0 + 0.25 * static_cast<double>(p); // dist: 5 .. ~21
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 3, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    CHECK(std::abs(out[uz(0)] - 1.0) < 1e-6);          // dist 5 ≤ mean 10 ⇒ fully lit
    CHECK(std::abs(out[uz(20)] - 1.0) < 1e-6);         // dist 10 == mean ⇒ still lit (boundary)
    CHECK(out[uz(n - 1)] < 0.05);                       // dist ~21 ≫ mean+σ ⇒ occluded (Chebyshev → ~0, no leak)
    for (int p = 21; p < n; ++p) { CHECK(out[uz(p)] <= out[uz(p - 1)] + 1e-9); } // monotone non-increasing past the mean
}
