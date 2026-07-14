// test_ckir_svgf.cpp — D-007 B14-c-1: the SVGF edge-stopping à-trous denoiser (ckir_svgf.hpp) on the CPU oracle
// (eval_cpu_kernel). The pass uses transcendentals (exp/pow) ⇒ verified through SVGF INVARIANTS that hold regardless of
// the exp/pow implementation: (1) a uniform input is preserved; (2) a noisy signal is smoothed (variance drops); (3) a
// hard depth edge STOPS the blur (no colour bleed across a silhouette). Portability (GPU == oracle) is in test_vulkan_context.cpp.

#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_svgf.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace kir = crd::kir;

namespace
{
crd::usize uz(int v) { return static_cast<crd::usize>(v); }
// run the à-trous kernel through the CPU oracle: color[W·H·3], gbuf[W·H·4], var[W·H] → color_out, var_out.
struct SvgfRun
{
    crd::containers::Array<crd::f64> color, gbuf, var, col_out, var_out;
    explicit SvgfRun(crd::memory::IAllocator* a) : color(a), gbuf(a), var(a), col_out(a), var_out(a) {}
};

void run_svgf(kir::KGraph& g, const kir::SvgfConfig& cfg, SvgfRun& r, crd::memory::IAllocator* alloc)
{
    const kir::KEntry e   = kir::build_svgf_atrous(g, cfg);
    const int         np  = cfg.width * cfg.height;
    r.col_out.resize(uz(np * 3));
    r.var_out.resize(uz(np));
    kir::KernelBuffer bufs[5] = {{r.color.data(), np * 3, 0, 0}, {r.gbuf.data(), np * 4, 0, 1}, {r.var.data(), np, 0, 2},
                                 {r.col_out.data(), np * 3, 0, 3}, {r.var_out.data(), np, 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, e.local_size[0], alloc, static_cast<crd::u32>(np / 64));
}

// fill a flat G-buffer: depth `d`, normal +Z, everywhere.
void flat_gbuf(SvgfRun& r, int np, double d)
{
    r.gbuf.resize(uz(np * 4));
    for (int p = 0; p < np; ++p)
    {
        r.gbuf[uz(p * 4 + 0)] = d;
        r.gbuf[uz(p * 4 + 1)] = 0.0;
        r.gbuf[uz(p * 4 + 2)] = 0.0;
        r.gbuf[uz(p * 4 + 3)] = 1.0;
    }
}
} // namespace

TEST_CASE("SVGF à-trous: a uniform image is PRESERVED (weights normalize away)", "[kir][svgf]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::SvgfConfig            cfg; // 32×32
    const int                  np = cfg.width * cfg.height;
    SvgfRun                    r(&alloc);
    r.color.resize(uz(np * 3));
    r.var.resize(uz(np));
    for (int p = 0; p < np; ++p)
    {
        r.color[uz(p * 3 + 0)] = 0.3;
        r.color[uz(p * 3 + 1)] = 0.5;
        r.color[uz(p * 3 + 2)] = 0.7;
        r.var[uz(p)]           = 0.1;
    }
    flat_gbuf(r, np, 1.0);

    kir::KGraph g(&alloc);
    run_svgf(g, cfg, r, &alloc);
    double maxdev = 0.0;
    for (int p = 0; p < np; ++p)
    {
        const double wants[3] = {0.3, 0.5, 0.7};
        for (int c = 0; c < 3; ++c)
        {
            const double dev = std::abs(r.col_out[uz(p * 3 + c)] - wants[c]);
            if (dev > maxdev) { maxdev = dev; }
        }
    }
    CHECK(maxdev < 1e-5); // Σw·c/Σw == c up to fp rounding of the weighted mean
}

TEST_CASE("SVGF à-trous: a NOISY flat surface is SMOOTHED (variance drops)", "[kir][svgf]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::SvgfConfig            cfg;
    cfg.sigma_l    = 1000.0; // relax the luma edge-stop so the smoother actually averages the noise
    const int      np = cfg.width * cfg.height;
    SvgfRun        r(&alloc);
    r.color.resize(uz(np * 3));
    r.var.resize(uz(np));
    crd::u32 s = 12345U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < np; ++p)
    {
        const double n = 0.5 + 0.4 * (rnd() - 0.5); // mean 0.5 + noise
        r.color[uz(p * 3 + 0)] = n;
        r.color[uz(p * 3 + 1)] = n;
        r.color[uz(p * 3 + 2)] = n;
        r.var[uz(p)]           = 0.05;
    }
    flat_gbuf(r, np, 1.0);

    kir::KGraph g(&alloc);
    run_svgf(g, cfg, r, &alloc);
    const auto stddev = [&](const crd::containers::Array<crd::f64>& a) {
        double m = 0.0;
        for (int p = 0; p < np; ++p) { m += a[uz(p * 3)]; }
        m /= np;
        double v = 0.0;
        for (int p = 0; p < np; ++p) { const double d = a[uz(p * 3)] - m; v += d * d; }
        return v / np;
    };
    CHECK(stddev(r.col_out) < stddev(r.color) * 0.5); // the blur cuts the noise variance at least in half
}

TEST_CASE("SVGF temporal: multi-frame accumulation REDUCES noise + grows history; disocclusion RESETS", "[kir][svgf]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::SvgfConfig            cfg; // 32×32
    const int                  wd = cfg.width;
    const int                  np = wd * cfg.height;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::build_svgf_temporal(g, cfg);

    // buffers: 0 cur_color(3) 1 cur_gb(4) 2 motion(2) 3 prev_col(3) 4 prev_st(4) 5 prev_gb(4) 6 out_col(3) 7 out_st(4)
    crd::containers::Array<crd::f64> cc(&alloc);
    crd::containers::Array<crd::f64> cg(&alloc);
    crd::containers::Array<crd::f64> mv(&alloc);
    crd::containers::Array<crd::f64> pc(&alloc);
    crd::containers::Array<crd::f64> ps(&alloc);
    crd::containers::Array<crd::f64> pg(&alloc);
    crd::containers::Array<crd::f64> oc(&alloc);
    crd::containers::Array<crd::f64> os(&alloc);
    cc.resize(uz(np * 3));
    cg.resize(uz(np * 4));
    mv.resize(uz(np * 2));
    pc.resize(uz(np * 3));
    ps.resize(uz(np * 4));
    pg.resize(uz(np * 4));
    oc.resize(uz(np * 3));
    os.resize(uz(np * 4));

    // static scene: depth 1, normal +Z, zero motion. Per-pixel TRUE colour = a gradient; each frame adds fresh noise.
    crd::containers::Array<crd::f64> truth(&alloc);
    truth.resize(uz(np));
    for (int p = 0; p < np; ++p)
    {
        truth[uz(p)]        = 0.3 + 0.4 * static_cast<double>(p % wd) / static_cast<double>(wd);
        cg[uz(p * 4 + 0)]   = 1.0;
        cg[uz(p * 4 + 3)]   = 1.0; // normal +Z
        pg[uz(p * 4 + 0)]   = 0.0; // prev geom zeroed ⇒ frame 1 rejects (no history)
    }
    for (int i = 0; i < np * 2; ++i) { mv[uz(i)] = 0.0; }
    for (int i = 0; i < np * 4; ++i) { ps[uz(i)] = 0.0; }
    for (int i = 0; i < np * 3; ++i) { pc[uz(i)] = 0.0; }

    kir::KernelBuffer bufs[8] = {{cc.data(), np * 3, 0, 0}, {cg.data(), np * 4, 0, 1}, {mv.data(), np * 2, 0, 2},
                                 {pc.data(), np * 3, 0, 3}, {ps.data(), np * 4, 0, 4}, {pg.data(), np * 4, 0, 5},
                                 {oc.data(), np * 3, 0, 6}, {os.data(), np * 4, 0, 7}};
    crd::u32   s   = 7U;
    auto       rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    const auto err = [&]() {
        double e2 = 0.0;
        for (int p = 0; p < np; ++p) { const double d = oc[uz(p * 3)] - truth[uz(p)]; e2 += d * d; }
        return e2 / np;
    };

    double err1 = 0.0;
    double errn = 0.0;
    const int frames = 16;
    for (int f = 0; f < frames; ++f)
    {
        for (int p = 0; p < np; ++p)
        {
            const double n = truth[uz(p)] + 0.3 * (rnd() - 0.5); // true + zero-mean noise
            cc[uz(p * 3 + 0)] = n;
            cc[uz(p * 3 + 1)] = n;
            cc[uz(p * 3 + 2)] = n;
        }
        kir::eval_cpu_kernel(g, e, bufs, 8, e.local_size[0], &alloc, static_cast<crd::u32>(np / 64));
        if (f == 0) { err1 = err(); }
        // ping-pong: out → prev; prev-geometry becomes this frame's geometry (static ⇒ frame 2+ reprojects validly)
        for (int i = 0; i < np * 3; ++i) { pc[uz(i)] = oc[uz(i)]; }
        for (int i = 0; i < np * 4; ++i) { ps[uz(i)] = os[uz(i)]; }
        for (int i = 0; i < np * 4; ++i) { pg[uz(i)] = cg[uz(i)]; }
    }
    errn = err();

    CHECK(errn < err1 * 0.5);                          // accumulation halves the residual noise-vs-truth
    CHECK(os[uz(2)] > 8.0);                             // history length grew (frame ~16, capped by α_min)
    CHECK(os[uz(3)] >= 0.0);                            // variance estimate is non-negative (m2−m1² clamped)

    // DISOCCLUSION: flip the current normal away from +Z ⇒ history must reject ⇒ out == cur, hist == 1
    for (int p = 0; p < np; ++p)
    {
        cg[uz(p * 4 + 1)] = 1.0; // normal +X now (n·n'_prev = 0 < normal_reject)
        cg[uz(p * 4 + 3)] = 0.0;
        cc[uz(p * 3)]     = 0.9;
        cc[uz(p * 3 + 1)] = 0.9;
        cc[uz(p * 3 + 2)] = 0.9;
    }
    kir::eval_cpu_kernel(g, e, bufs, 8, e.local_size[0], &alloc, static_cast<crd::u32>(np / 64));
    CHECK(std::abs(oc[uz(0)] - 0.9) < 1e-6); // history rejected ⇒ output is the fresh sample, no ghost
    CHECK(std::abs(os[uz(2)] - 1.0) < 1e-6); // history reset to 1
}

TEST_CASE("A-SVGF: adaptive-α RESETS on a real step change (faster response) but NOT on noise", "[kir][svgf]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);

    // run `stable` frames of a steady signal (0.3 + noise), then ONE step frame (0.8 + noise); return |out − 0.8| at pixel 0
    // (how far the accumulator lags the new value) AND the final history length.
    const auto run = [&](bool asvgf, double& lag, double& histlen) {
        kir::SvgfConfig cfg;
        cfg.asvgf   = asvgf;
        const int wd = cfg.width;
        const int np = wd * cfg.height;
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::build_svgf_temporal(g, cfg);
        crd::containers::Array<crd::f64> cc(&alloc);
        crd::containers::Array<crd::f64> cg(&alloc);
        crd::containers::Array<crd::f64> mv(&alloc);
        crd::containers::Array<crd::f64> pc(&alloc);
        crd::containers::Array<crd::f64> ps(&alloc);
        crd::containers::Array<crd::f64> pg(&alloc);
        crd::containers::Array<crd::f64> oc(&alloc);
        crd::containers::Array<crd::f64> os(&alloc);
        cc.resize(uz(np * 3)); cg.resize(uz(np * 4)); mv.resize(uz(np * 2)); pc.resize(uz(np * 3));
        ps.resize(uz(np * 4)); pg.resize(uz(np * 4)); oc.resize(uz(np * 3)); os.resize(uz(np * 4));
        for (int p = 0; p < np; ++p) { cg[uz(p * 4 + 0)] = 1.0; cg[uz(p * 4 + 3)] = 1.0; pg[uz(p * 4 + 0)] = 0.0; }
        kir::KernelBuffer bufs[8] = {{cc.data(), np * 3, 0, 0}, {cg.data(), np * 4, 0, 1}, {mv.data(), np * 2, 0, 2},
                                     {pc.data(), np * 3, 0, 3}, {ps.data(), np * 4, 0, 4}, {pg.data(), np * 4, 0, 5},
                                     {oc.data(), np * 3, 0, 6}, {os.data(), np * 4, 0, 7}};
        crd::u32 s   = 55U;
        auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
        const int frames = 24;
        for (int f = 0; f < frames; ++f)
        {
            const double base = (f == frames - 1) ? 0.8 : 0.3; // the LAST frame steps the true value up
            for (int p = 0; p < np; ++p)
            {
                const double n = base + 0.15 * (rnd() - 0.5); // noise ±0.075 ⇒ σ ≈ 0.04 ≪ the 0.5 step
                cc[uz(p * 3 + 0)] = n; cc[uz(p * 3 + 1)] = n; cc[uz(p * 3 + 2)] = n;
            }
            kir::eval_cpu_kernel(g, e, bufs, 8, e.local_size[0], &alloc, static_cast<crd::u32>(np / 64));
            for (int i = 0; i < np * 3; ++i) { pc[uz(i)] = oc[uz(i)]; }
            for (int i = 0; i < np * 4; ++i) { ps[uz(i)] = os[uz(i)]; pg[uz(i)] = cg[uz(i)]; }
        }
        lag     = std::abs(oc[uz(0)] - 0.8);
        histlen = os[uz(2)];
    };

    double lag_fixed     = 0.0;
    double hist_fixed    = 0.0;
    double lag_adaptive  = 0.0;
    double hist_adaptive = 0.0;
    run(false, lag_fixed, hist_fixed);
    run(true, lag_adaptive, hist_adaptive);

    CHECK(hist_fixed > 15.0);              // fixed-α accumulated a long history over the stable phase (noise didn't reset it)
    CHECK(hist_adaptive > 15.0);           // A-SVGF ALSO kept the history through the stable phase (noise ≪ σ_hi ⇒ no reset)
    CHECK(lag_adaptive < lag_fixed * 0.2); // on the STEP, A-SVGF resets ⇒ tracks the new value ≫5× closer than fixed-α lags
}

TEST_CASE("SVGF PIPELINE (temporal accumulate → à-trous ×5): the full gold denoiser crushes noise end-to-end", "[kir][svgf]")
{
    crd::memory::TlsfAllocator alloc(128U << 20U);
    kir::SvgfConfig            cfg;
    const int                  wd = cfg.width;
    const int                  np = wd * cfg.height;

    // ── temporal accumulation over T frames (static scene) → integrated colour + stat(m1,m2,hist,var) ──
    kir::KGraph       gt(&alloc);
    const kir::KEntry et = kir::build_svgf_temporal(gt, cfg);
    crd::containers::Array<crd::f64> cc(&alloc);
    crd::containers::Array<crd::f64> cg(&alloc);
    crd::containers::Array<crd::f64> mv(&alloc);
    crd::containers::Array<crd::f64> pc(&alloc);
    crd::containers::Array<crd::f64> ps(&alloc);
    crd::containers::Array<crd::f64> pg(&alloc);
    crd::containers::Array<crd::f64> oc(&alloc);
    crd::containers::Array<crd::f64> os(&alloc);
    cc.resize(uz(np * 3)); cg.resize(uz(np * 4)); mv.resize(uz(np * 2)); pc.resize(uz(np * 3));
    ps.resize(uz(np * 4)); pg.resize(uz(np * 4)); oc.resize(uz(np * 3)); os.resize(uz(np * 4));
    crd::containers::Array<crd::f64> truth(&alloc);
    truth.resize(uz(np));
    for (int p = 0; p < np; ++p)
    {
        truth[uz(p)]      = 0.35 + 0.3 * static_cast<double>(p % wd) / static_cast<double>(wd); // a SMOOTH gradient (GI-like)
        cg[uz(p * 4 + 0)] = 1.0; cg[uz(p * 4 + 3)] = 1.0;
        pg[uz(p * 4 + 0)] = 0.0;
    }
    kir::KernelBuffer tb[8] = {{cc.data(), np * 3, 0, 0}, {cg.data(), np * 4, 0, 1}, {mv.data(), np * 2, 0, 2},
                               {pc.data(), np * 3, 0, 3}, {ps.data(), np * 4, 0, 4}, {pg.data(), np * 4, 0, 5},
                               {oc.data(), np * 3, 0, 6}, {os.data(), np * 4, 0, 7}};
    crd::u32 s = 101U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    const auto stddev = [&](const crd::containers::Array<crd::f64>& a) {
        double e2 = 0.0;
        for (int p = 0; p < np; ++p) { const double d = a[uz(p * 3)] - truth[uz(p)]; e2 += d * d; }
        return e2 / np;
    };
    double noisy_err = 0.0;
    for (int f = 0; f < 4; ++f) // SHORT history ⇒ residual noise remains for the spatial à-trous to clean (the SVGF regime)
    {
        for (int p = 0; p < np; ++p)
        {
            const double n = truth[uz(p)] + 0.35 * (rnd() - 0.5);
            cc[uz(p * 3 + 0)] = n; cc[uz(p * 3 + 1)] = n; cc[uz(p * 3 + 2)] = n;
        }
        if (f == 0) { for (int p = 0; p < np; ++p) { noisy_err += (cc[uz(p * 3)] - truth[uz(p)]) * (cc[uz(p * 3)] - truth[uz(p)]); } noisy_err /= np; }
        kir::eval_cpu_kernel(gt, et, tb, 8, et.local_size[0], &alloc, static_cast<crd::u32>(np / 64));
        for (int i = 0; i < np * 3; ++i) { pc[uz(i)] = oc[uz(i)]; }
        for (int i = 0; i < np * 4; ++i) { ps[uz(i)] = os[uz(i)]; pg[uz(i)] = cg[uz(i)]; }
    }
    const double temporal_err = stddev(oc);

    // ── à-trous ×5 (steps 1,2,4,8,16) on the integrated colour, guided by the temporal variance ──
    crd::containers::Array<crd::f64> col(&alloc);
    crd::containers::Array<crd::f64> var(&alloc);
    crd::containers::Array<crd::f64> col2(&alloc);
    crd::containers::Array<crd::f64> var2(&alloc);
    col.resize(uz(np * 3)); var.resize(uz(np)); col2.resize(uz(np * 3)); var2.resize(uz(np));
    for (int i = 0; i < np * 3; ++i) { col[uz(i)] = oc[uz(i)]; }
    for (int p = 0; p < np; ++p) { var[uz(p)] = os[uz(p * 4 + 3)]; } // extract variance from the temporal stat
    for (int it = 0; it < 5; ++it)
    {
        kir::SvgfConfig ac = cfg;
        ac.step           = 1 << it;
        kir::KGraph       ga(&alloc);
        const kir::KEntry ea = kir::build_svgf_atrous(ga, ac);
        kir::KernelBuffer ab[5] = {{col.data(), np * 3, 0, 0}, {cg.data(), np * 4, 0, 1}, {var.data(), np, 0, 2},
                                   {col2.data(), np * 3, 0, 3}, {var2.data(), np, 0, 4}};
        kir::eval_cpu_kernel(ga, ea, ab, 5, ea.local_size[0], &alloc, static_cast<crd::u32>(np / 64));
        for (int i = 0; i < np * 3; ++i) { col[uz(i)] = col2[uz(i)]; }
        for (int p = 0; p < np; ++p) { var[uz(p)] = var2[uz(p)]; }
    }
    double final_err = 0.0;
    for (int p = 0; p < np; ++p) { const double d = col[uz(p * 3)] - truth[uz(p)]; final_err += d * d; }
    final_err /= np;

    CHECK(temporal_err < noisy_err);        // temporal accumulation reduces the error
    CHECK(final_err < temporal_err * 0.5);  // the à-trous spatial pass cleans up the residual ≥2× further (smooth GI signal)
    CHECK(final_err < noisy_err * 0.1);     // the FULL pipeline crushes the noise ≥10× vs the raw frame
}

TEST_CASE("SVGF à-trous: a hard DEPTH EDGE stops the blur (no colour bleed across a silhouette)", "[kir][svgf]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::SvgfConfig            cfg;
    const int                  np = cfg.width * cfg.height;
    const int                  wd = cfg.width;

    // black left half / white right half; vertical edge at x=16. Measure a left-boundary pixel (x=15).
    const auto build = [&](double right_depth, double& lum_at_boundary) {
        SvgfRun r(&alloc);
        r.color.resize(uz(np * 3));
        r.var.resize(uz(np));
        r.gbuf.resize(uz(np * 4));
        for (int y = 0; y < cfg.height; ++y)
        {
            for (int x = 0; x < wd; ++x)
            {
                const int    p = y * wd + x;
                const double c = (x >= 16) ? 1.0 : 0.0;
                r.color[uz(p * 3 + 0)] = c;
                r.color[uz(p * 3 + 1)] = c;
                r.color[uz(p * 3 + 2)] = c;
                r.var[uz(p)]           = 0.1;
                r.gbuf[uz(p * 4 + 0)]  = (x >= 16) ? right_depth : 1.0;
                r.gbuf[uz(p * 4 + 1)]  = 0.0;
                r.gbuf[uz(p * 4 + 2)]  = 0.0;
                r.gbuf[uz(p * 4 + 3)]  = 1.0;
            }
        }
        kir::KGraph g(&alloc);
        run_svgf(g, cfg, r, &alloc);
        lum_at_boundary = r.col_out[uz((0 * wd + 15) * 3)]; // grayscale ⇒ any channel = luma
    };

    double no_edge   = 0.0;
    double with_edge = 0.0;
    build(1.0, no_edge);     // uniform depth ⇒ the white half bleeds left
    build(1000.0, with_edge); // hard depth gap ⇒ the bleed is STOPPED
    CHECK(with_edge < no_edge * 0.5); // the edge keeps the boundary pixel far darker (white did not cross the silhouette)
}
