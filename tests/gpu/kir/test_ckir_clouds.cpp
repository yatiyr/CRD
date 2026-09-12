// test_ckir_clouds.cpp — D-007 B15-b: volumetric clouds (ckir_clouds.hpp) on the CPU oracle. B15-b-1 verifies the Nubis DENSITY
// FIELD: (1) density ∈ [0,1] everywhere; (2) the HEIGHT GRADIENT — density vanishes at the layer floor and ceiling, clouds live
// in the mid-layer; (3) COVERAGE control — more coverage ⇒ more cloud; (4) determinism. Portability rides test_vulkan_context.

#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_clouds.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace kir = crd::kir;

namespace
{
crd::usize uz(int v) { return static_cast<crd::usize>(v); }
} // namespace

TEST_CASE("B15-b cloud density: in [0,1], vanishes at the slab floor/ceiling, grows with coverage, deterministic", "[kir][clouds]")
{
    crd::memory::TlsfAllocator alloc(128U << 20U);
    kir::clouds::CloudConfig   cfg;
    const int                  n = 64; // the Nubis density stacks many noise octaves ⇒ heavy on the recursive oracle; a small
                                       // point set suffices (the field is per-position independent). Full res runs on the GPU.

    // positions spanning the cloud slab: an EVEN height sweep (so the floor/mid/ceiling bins are guaranteed populated), x,y random.
    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> h01(&alloc); // the normalised height per position (for the height-gradient checks)
    pos.resize(uz(n * 3));
    h01.resize(uz(n));
    crd::u32 s   = 4242U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        const double hf = static_cast<double>(i) / static_cast<double>(n - 1);
        const double z  = cfg.cloud_base + hf * (cfg.cloud_top - cfg.cloud_base);
        pos[uz(i * 3 + 0)] = rnd() * 6.0;
        pos[uz(i * 3 + 1)] = rnd() * 6.0;
        pos[uz(i * 3 + 2)] = z;
        h01[uz(i)]         = hf;
    }

    const auto run = [&](double coverage, crd::containers::Array<crd::f64>& out) {
        kir::clouds::CloudConfig c = cfg;
        c.coverage                 = coverage;
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::clouds::build_cloud_density(g, c);
        out.resize(uz(n));
        kir::KernelBuffer b[2] = {{pos.data(), n * 3, 0, 0}, {out.data(), n, 0, 1}};
        kir::eval_cpu_kernel(g, e, b, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));
    };

    crd::containers::Array<crd::f64> d_lo(&alloc);
    crd::containers::Array<crd::f64> d_hi(&alloc);
    run(0.5, d_lo);
    run(0.85, d_hi);

    // (1) density ∈ [0,1].
    double lo = 1e30;
    double hi = -1e30;
    for (int i = 0; i < n; ++i) { lo = d_hi[uz(i)] < lo ? d_hi[uz(i)] : lo; hi = d_hi[uz(i)] > hi ? d_hi[uz(i)] : hi; }
    CHECK(lo >= 0.0);
    CHECK(hi <= 1.0 + 1e-6);

    // (2) HEIGHT GRADIENT: density vanishes at the floor (h<0.05) and ceiling (h>0.98); the mid-layer actually clouds.
    double edge_max = 0.0;
    double mid_max  = 0.0;
    for (int i = 0; i < n; ++i)
    {
        if (h01[uz(i)] < 0.05 || h01[uz(i)] > 0.98) { edge_max = d_hi[uz(i)] > edge_max ? d_hi[uz(i)] : edge_max; }
        if (h01[uz(i)] > 0.25 && h01[uz(i)] < 0.4) { mid_max = d_hi[uz(i)] > mid_max ? d_hi[uz(i)] : mid_max; }
    }
    CHECK(edge_max < 0.02); // the slab boundaries are clear sky
    CHECK(mid_max > 0.1);   // mid-layer has real cloud

    // (3) COVERAGE: more coverage ⇒ more cloud (higher mean density).
    double mean_lo = 0.0;
    double mean_hi = 0.0;
    for (int i = 0; i < n; ++i) { mean_lo += d_lo[uz(i)]; mean_hi += d_hi[uz(i)]; }
    mean_lo /= n;
    mean_hi /= n;
    CHECK(mean_hi > mean_lo);
    CHECK(mean_lo > 0.0);

    // (4) determinism: re-run at the SAME coverage as d_hi ⇒ byte-identical.
    crd::containers::Array<crd::f64> again(&alloc);
    run(0.85, again);
    double ddiff = 0.0;
    for (int i = 0; i < n; ++i) { ddiff = std::fabs(again[uz(i)] - d_hi[uz(i)]) > ddiff ? std::fabs(again[uz(i)] - d_hi[uz(i)]) : ddiff; }
    CHECK(ddiff == 0.0);
}

TEST_CASE("B15-b cloud ray-march: Beer-Powder transmittance + phase inscatter over the baked density volume", "[kir][clouds]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    kir::clouds::CloudConfig   cfg;
    const int                  dim = cfg.vol_dim;
    const int                  sw  = cfg.screen;
    const int                  nv  = dim * dim * dim;
    const int                  ns  = sw * sw;
    const double               ext = 6.0;
    const double               sc  = ext / static_cast<double>(dim - 1);
    const double               hsc = (cfg.cloud_top - cfg.cloud_base) / static_cast<double>(dim - 1);

    // volume grid positions: cell p = (zi·dim + yi)·dim + xi ⇒ world (xi·sc, yi·sc, cloud_base + zi·hsc) [z = the noise/height axis].
    crd::containers::Array<crd::f64> pos(&alloc);
    pos.resize(uz(nv * 3));
    for (int zi = 0; zi < dim; ++zi)
    {
        for (int yi = 0; yi < dim; ++yi)
        {
            for (int xi = 0; xi < dim; ++xi)
            {
                const int cell = (zi * dim + yi) * dim + xi;
                pos[uz(cell * 3 + 0)] = static_cast<double>(xi) * sc;
                pos[uz(cell * 3 + 1)] = static_cast<double>(yi) * sc;
                pos[uz(cell * 3 + 2)] = cfg.cloud_base + static_cast<double>(zi) * hsc;
            }
        }
    }

    // bake the density volume at a coverage, then march it → (RGB inscatter, view transmittance) per screen texel.
    const auto bake_and_march = [&](double coverage, crd::containers::Array<crd::f64>& out) {
        kir::clouds::CloudConfig c = cfg;
        c.coverage                 = coverage;
        kir::KGraph       gd(&alloc);
        const kir::KEntry ed = kir::clouds::build_cloud_density(gd, c);
        crd::containers::Array<crd::f64> vol(&alloc);
        vol.resize(uz(nv));
        kir::KernelBuffer db[2] = {{pos.data(), nv * 3, 0, 0}, {vol.data(), nv, 0, 1}};
        kir::eval_cpu_kernel(gd, ed, db, 2, ed.local_size[0], &alloc, static_cast<crd::u32>(nv / 64));

        kir::KGraph       gm(&alloc);
        const kir::KEntry em = kir::clouds::build_cloud_march(gm, c);
        out.resize(uz(ns * 4));
        kir::KernelBuffer mb[2] = {{vol.data(), nv, 0, 0}, {out.data(), ns * 4, 0, 1}};
        kir::eval_cpu_kernel(gm, em, mb, 2, em.local_size[0], &alloc, static_cast<crd::u32>(ns / 64));
    };

    crd::containers::Array<crd::f64> o_lo(&alloc);
    crd::containers::Array<crd::f64> o_hi(&alloc);
    bake_and_march(0.6, o_lo);
    bake_and_march(0.9, o_hi);

    // (1) transmittance ∈ (0,1], inscatter ≥ 0.
    double tmin = 2.0;
    double tmax = -1.0;
    double imin = 1e30;
    for (int i = 0; i < ns; ++i)
    {
        const double t = o_hi[uz(i * 4 + 3)];
        tmin = t < tmin ? t : tmin;
        tmax = t > tmax ? t : tmax;
        imin = o_hi[uz(i * 4 + 0)] < imin ? o_hi[uz(i * 4 + 0)] : imin;
    }
    CHECK(tmin > 0.0);
    CHECK(tmax <= 1.0 + 1e-6);
    CHECK(imin >= 0.0);

    // (2) the cloud is OPAQUE somewhere (a ray is blocked) AND scatters light (inscatter > 0 somewhere).
    double tmin_any = 2.0;
    double imax     = 0.0;
    for (int i = 0; i < ns; ++i)
    {
        tmin_any = o_hi[uz(i * 4 + 3)] < tmin_any ? o_hi[uz(i * 4 + 3)] : tmin_any;
        imax     = o_hi[uz(i * 4 + 0)] > imax ? o_hi[uz(i * 4 + 0)] : imax;
    }
    CHECK(tmin_any < 0.9); // some ray is meaningfully attenuated
    CHECK(imax > 0.0);     // the clouds scatter sunlight

    // (3) more COVERAGE ⇒ denser ⇒ lower mean transmittance (more opaque).
    double mt_lo = 0.0;
    double mt_hi = 0.0;
    for (int i = 0; i < ns; ++i) { mt_lo += o_lo[uz(i * 4 + 3)]; mt_hi += o_hi[uz(i * 4 + 3)]; }
    mt_lo /= ns;
    mt_hi /= ns;
    CHECK(mt_hi < mt_lo);
}
