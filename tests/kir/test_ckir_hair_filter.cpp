// test_ckir_hair_filter.cpp — D-007 B18-e: the hair COMPOSITING filter (Lipp et al. 2026 §3.3.1).
//
// A reconstruction filter is only worth its cost if it is anisotropic in the RIGHT frame. The gates below pin every term of
//   w_PQ = exp(-dpar^2/sp^2 - dperp^2/sq^2) * exp(-||Cp-Cq||^2/sc^2)
// independently: the spatial ellipse must be tangent-ALIGNED (not merely "blurry"), the depth guard must be ABSOLUTE (not a
// soft falloff, which would halo every silhouette), and the whole kernel must be a CONVEX combination — a filter that can
// overshoot would bloom bright strands into rings on every frame.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_hair_geom.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/containers/array.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr int kFW = 32;
constexpr int kFH = 32;

void run_filter(crd::memory::IAllocator& alloc, const kir::hairgeom::HairFilterConfig& cfg,
                crd::containers::Array<double>& col, crd::containers::Array<double>& tan,
                crd::containers::Array<double>& dep, crd::containers::Array<double>& out)
{
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hairgeom::build_hair_filter_kernel(g, cfg);
    const int         n = cfg.width * cfg.height;
    out.resize(static_cast<crd::usize>(n) * 4U, 0.0);
    kir::KernelBuffer b[4] = {{col.data(), n * 3, 0, 0},
                              {tan.data(), n * 2, 0, 1},
                              {dep.data(), n, 0, 2},
                              {out.data(), n * 4, 0, 3}};
    // one lane per pixel — dispatch CEIL(n / local_size) groups (the guard in the kernel absorbs the tail lanes)
    const crd::u32 groups = (static_cast<crd::u32>(n) + e.local_size[0] - 1U) / e.local_size[0];
    kir::eval_cpu_kernel(g, e, b, 4, e.local_size[0], &alloc, groups);
}

// A flat field: uniform depth, uniform tangent direction, black colour.
void make_field(crd::containers::Array<double>& col, crd::containers::Array<double>& tan,
                crd::containers::Array<double>& dep, double tdx, double tdy)
{
    const int n = kFW * kFH;
    col.resize(static_cast<crd::usize>(n) * 3U, 0.0);
    tan.resize(static_cast<crd::usize>(n) * 2U, 0.0);
    dep.resize(static_cast<crd::usize>(n), 0.5);
    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < 3; ++c) { col[static_cast<crd::usize>(i) * 3U + static_cast<crd::usize>(c)] = 0.0; }
        dep[static_cast<crd::usize>(i)] = 0.5;
    }
    const double inv = 1.0 / crd::math::sqrt(tdx * tdx + tdy * tdy);
    for (int i = 0; i < n; ++i)
    {
        tan[static_cast<crd::usize>(i) * 2U + 0U] = tdx * inv;
        tan[static_cast<crd::usize>(i) * 2U + 1U] = tdy * inv;
    }
}
} // namespace

TEST_CASE("ckir hair filter preserves a constant field exactly", "[ckir][hair][filter]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-filter-const");
    crd::containers::Array<double> col(&alloc);
    crd::containers::Array<double> tan(&alloc);
    crd::containers::Array<double> dep(&alloc);
    crd::containers::Array<double> out(&alloc);
    make_field(col, tan, dep, 1.0, 0.0);
    for (int i = 0; i < kFW * kFH; ++i)
    {
        col[static_cast<crd::usize>(i) * 3U + 0U] = 0.37;
        col[static_cast<crd::usize>(i) * 3U + 1U] = 0.62;
        col[static_cast<crd::usize>(i) * 3U + 2U] = 0.14;
    }
    kir::hairgeom::HairFilterConfig cfg;
    cfg.width  = kFW;
    cfg.height = kFH;
    run_filter(alloc, cfg, col, tan, dep, out);

    // PARTITION OF UNITY. Normalised non-negative weights over a constant field must return that constant — INCLUDING at the
    // borders, where the window is clipped. A filter that fails here darkens or brightens every silhouette edge.
    double worst = 0.0;
    for (int i = 0; i < kFW * kFH; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double e = crd::math::abs(out[static_cast<crd::usize>(i) * 4U + static_cast<crd::usize>(c)]
                                            - col[static_cast<crd::usize>(i) * 3U + static_cast<crd::usize>(c)]);
            if (e > worst) { worst = e; }
        }
        CHECK(out[static_cast<crd::usize>(i) * 4U + 3U] > 0.0); // every pixel gathers SOMETHING (itself, at minimum)
    }
    // F32 buffers, and the oracle rounds EVERY elementary op — so the bar is float epsilon over a 121-tap sum, not double.
    INFO("worst constant-field deviation " << worst << " (F32 121-tap budget 2e-6)");
    CHECK(worst < 2.0e-6);
}

TEST_CASE("ckir hair filter ellipse is aligned to the strand tangent", "[ckir][hair][filter]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-filter-aniso");
    crd::containers::Array<double> col(&alloc);
    crd::containers::Array<double> tan(&alloc);
    crd::containers::Array<double> dep(&alloc);
    crd::containers::Array<double> out(&alloc);
    const int                      cx = kFW / 2;
    const int                      cy = kFH / 2;
    make_field(col, tan, dep, 1.0, 0.0); // tangent along +x
    col[(static_cast<crd::usize>(cy) * kFW + static_cast<crd::usize>(cx)) * 3U + 0U] = 1.0; // one bright pixel

    kir::hairgeom::HairFilterConfig cfg;
    cfg.width       = kFW;
    cfg.height      = kFH;
    cfg.sigma_color = 1.0e6; // neutralise the colour term to ISOLATE the spatial ellipse

    run_filter(alloc, cfg, col, tan, dep, out);
    const auto rd = [&](int x, int y) { return out[(static_cast<crd::usize>(y) * kFW + static_cast<crd::usize>(x)) * 4U + 0U]; };

    // ANISOTROPY, quantitatively. With the colour term neutral and the normaliser translation-invariant, the response ratio
    // at distance k is EXACTLY exp(k^2 (1/sq^2 - 1/sp^2)). This is the claim an isotropic blur cannot make at any radius.
    for (int k = 1; k <= 3; ++k)
    {
        const double along = rd(cx + k, cy);
        const double perp  = rd(cx, cy + k);
        const double pred  = crd::math::exp(static_cast<double>(k * k) * (1.0 / (cfg.sigma_perp * cfg.sigma_perp)
                                                                         - 1.0 / (cfg.sigma_par * cfg.sigma_par)));
        INFO("k=" << k << " along " << along << " perp " << perp << " ratio " << (along / perp) << " predicted " << pred);
        CHECK(along > perp); // wide ALONG the strand, narrow ACROSS it
        CHECK(crd::math::abs(along / perp - pred) < pred * 1.0e-6);
    }

    // ...and the ellipse must ROTATE with the tangent, not sit axis-locked.
    make_field(col, tan, dep, 0.0, 1.0); // tangent along +y
    col[(static_cast<crd::usize>(cy) * kFW + static_cast<crd::usize>(cx)) * 3U + 0U] = 1.0;
    run_filter(alloc, cfg, col, tan, dep, out);
    CHECK(rd(cx, cy + 2) > rd(cx + 2, cy)); // the LONG axis is now vertical
}

TEST_CASE("ckir hair filter never blends across a depth discontinuity", "[ckir][hair][filter]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-filter-depth");
    crd::containers::Array<double> col(&alloc);
    crd::containers::Array<double> tan(&alloc);
    crd::containers::Array<double> dep(&alloc);
    crd::containers::Array<double> out(&alloc);
    make_field(col, tan, dep, 1.0, 0.0);
    for (int y = 0; y < kFH; ++y)
    {
        for (int x = 0; x < kFW; ++x)
        {
            const crd::usize i = (static_cast<crd::usize>(y) * kFW + static_cast<crd::usize>(x));
            const bool       left = x < kFW / 2;
            dep[i]             = left ? 0.5 : 0.9; // a silhouette straight down the middle
            col[i * 3U + 0U]   = left ? 1.0 : 0.0;
        }
    }
    kir::hairgeom::HairFilterConfig cfg;
    cfg.width       = kFW;
    cfg.height      = kFH;
    cfg.sigma_color = 1.0e6; // colour must NOT be what saves us here — the DEPTH guard alone must
    run_filter(alloc, cfg, col, tan, dep, out);

    // HARD rejection. Right up against the edge the near surface stays exactly 1.0 and the far exactly 0.0. A soft depth
    // falloff would leak a visible halo here — the classic bilateral-blur silhouette artefact.
    double leak = 0.0;
    for (int y = 5; y < kFH - 5; ++y)
    {
        const double nearv = out[(static_cast<crd::usize>(y) * kFW + (kFW / 2 - 1)) * 4U + 0U];
        const double farv  = out[(static_cast<crd::usize>(y) * kFW + (kFW / 2)) * 4U + 0U];
        if (crd::math::abs(nearv - 1.0) > leak) { leak = crd::math::abs(nearv - 1.0); }
        if (crd::math::abs(farv) > leak) { leak = crd::math::abs(farv); }
    }
    INFO("worst cross-silhouette leak " << leak);
    CHECK(leak < 2.0e-6);
}

TEST_CASE("ckir hair filter fills gaps along a strand without smearing across strands", "[ckir][hair][filter]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-filter-reconnect");
    crd::containers::Array<double> col(&alloc);
    crd::containers::Array<double> tan(&alloc);
    crd::containers::Array<double> dep(&alloc);
    crd::containers::Array<double> out(&alloc);
    make_field(col, tan, dep, 1.0, 0.0);
    const int row_a = kFH / 2;
    const int row_b = row_a + 2; // a SECOND strand, only two pixels away — the case that punishes an isotropic kernel

    // Empty pixels sit at the far plane so the depth guard rejects them. This matters: if background counted as a legitimate
    // black sample it would dominate the normaliser and no filter could ever lift a gap out of black.
    for (int i = 0; i < kFW * kFH; ++i) { dep[static_cast<crd::usize>(i)] = 1.0; }
    for (int x = 0; x < kFW; ++x)
    {
        dep[(static_cast<crd::usize>(row_a) * kFW + static_cast<crd::usize>(x))] = 0.5; // strand A is COVERED everywhere (the conservative layer)...
        dep[(static_cast<crd::usize>(row_b) * kFW + static_cast<crd::usize>(x))] = 0.5;
        // ...but only every other pixel got a shaded sample — the dotted result of 1-spp deferred rasterization.
        if (x % 2 == 0) { col[(static_cast<crd::usize>(row_a) * kFW + static_cast<crd::usize>(x)) * 3U + 0U] = 1.0; } // A is RED
        col[(static_cast<crd::usize>(row_b) * kFW + static_cast<crd::usize>(x)) * 3U + 1U] = 1.0;                     // B is GREEN, solid
    }
    kir::hairgeom::HairFilterConfig cfg;
    cfg.width  = kFW;
    cfg.height = kFH;
    run_filter(alloc, cfg, col, tan, dep, out);

    double gmin = 1.0e30;
    double hmin = 1.0e30;
    for (int x = 8; x < kFW - 8; ++x)
    {
        const double v = out[(static_cast<crd::usize>(row_a) * kFW + static_cast<crd::usize>(x)) * 4U + 0U];
        if (x % 2 == 0) { if (v < hmin) { hmin = v; } }
        else            { if (v < gmin) { gmin = v; } }
    }
    // GAP FILL. A gap is exactly 0 in the input; the kernel must pull its along-tangent neighbours in. It cannot reach parity
    // with a hit, and that is BY DESIGN — the colour term deliberately down-weights dissimilar neighbours, which is what stops
    // the filter hallucinating strands. Closing the last of the gap is the job of the dual-layer conservative rasterization
    // (which shades the covered-but-unsampled pixel directly); the filter's share is lifting it out of black.
    INFO("gap fill " << gmin << "  hit " << hmin << "  ratio " << (gmin / hmin));
    CHECK(gmin > 0.25 * hmin);

    // NO CROSS-STRAND SMEAR. Strand B is two pixels away at the SAME depth, so only the narrow sigma_perp separates them —
    // the depth guard cannot help here. An isotropic blur of comparable reach would bleed B's green straight into A.
    double gbleed = 0.0;
    double gown   = 0.0;
    for (int x = 8; x < kFW - 8; ++x)
    {
        const double b = out[(static_cast<crd::usize>(row_a) * kFW + static_cast<crd::usize>(x)) * 4U + 1U];
        const double o = out[(static_cast<crd::usize>(row_b) * kFW + static_cast<crd::usize>(x)) * 4U + 1U];
        if (b > gbleed) { gbleed = b; }
        if (o > gown) { gown = o; }
    }
    INFO("green bled onto strand A " << gbleed << " vs strand B's own " << gown << " ratio " << (gbleed / gown));
    CHECK(gbleed < 0.05 * gown);
}

TEST_CASE("ckir hair filter is a convex combination", "[ckir][hair][filter]")
{
    crd::memory::TlsfAllocator     alloc(24U << 20U, nullptr, "hair-filter-convex");
    crd::containers::Array<double> col(&alloc);
    crd::containers::Array<double> tan(&alloc);
    crd::containers::Array<double> dep(&alloc);
    crd::containers::Array<double> out(&alloc);
    make_field(col, tan, dep, 0.7, 0.3);
    crd::u32   st  = 0x1234567U;
    const auto rnd = [&]() {
        st = st * 1664525U + 1013904223U;
        return static_cast<double>(st >> 8U) / 16777216.0;
    };
    for (int i = 0; i < kFW * kFH; ++i)
    {
        for (int c = 0; c < 3; ++c) { col[static_cast<crd::usize>(i) * 3U + static_cast<crd::usize>(c)] = rnd(); }
    }
    kir::hairgeom::HairFilterConfig cfg;
    cfg.width  = kFW;
    cfg.height = kFH;
    run_filter(alloc, cfg, col, tan, dep, out);

    // NO OVERSHOOT. Non-negative normalised weights imply the output is a convex combination of its inputs, so it can never
    // leave [min,max]. A filter that rings would halo every bright strand — and ringing is invisible in an L2 metric.
    double lo = 1.0e30;
    double hi = -1.0e30;
    for (int i = 0; i < kFW * kFH; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double v = col[static_cast<crd::usize>(i) * 3U + static_cast<crd::usize>(c)];
            if (v < lo) { lo = v; }
            if (v > hi) { hi = v; }
        }
    }
    for (int i = 0; i < kFW * kFH; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            const double v = out[static_cast<crd::usize>(i) * 4U + static_cast<crd::usize>(c)];
            CHECK(v >= lo - 1.0e-12);
            CHECK(v <= hi + 1.0e-12);
        }
    }
}
