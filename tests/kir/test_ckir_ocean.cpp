// test_ckir_ocean.cpp — D-007 B16-a-1: the FFT-ocean SPECTRUM (Horvath JONSWAP + directional spread + swell), CPU-oracle
// physics. Verifies the frequency-domain h0(k) is a well-formed directional wave spectrum before the a-2 time-evolution + IFFT.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_fft.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_ocean.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
crd::usize uz(int v) { return static_cast<crd::usize>(v); }

// Direct UNNORMALISED inverse 2-D DFT: out[a][b] = sum_{kr,kc} X[kr][kc]·e^{+i 2pi(a·kr/N + b·kc/N)} (square N×N).
void idft2(int n, const crd::f64* xr, const crd::f64* xi, crd::f64* outr, crd::f64* outi)
{
    const double tau = 6.28318530717958647693;
    for (int a = 0; a < n; ++a)
    {
        for (int b = 0; b < n; ++b)
        {
            double sr = 0.0;
            double si = 0.0;
            for (int kr = 0; kr < n; ++kr)
            {
                for (int kc = 0; kc < n; ++kc)
                {
                    const double ang = tau * (static_cast<double>(a * kr) / n + static_cast<double>(b * kc) / n);
                    const double c   = std::cos(ang);
                    const double s   = std::sin(ang);
                    const double vr  = xr[kr * n + kc];
                    const double vi  = xi[kr * n + kc];
                    sr += vr * c - vi * s;
                    si += vr * s + vi * c;
                }
            }
            outr[a * n + b] = sr;
            outi[a * n + b] = si;
        }
    }
}
} // namespace

TEST_CASE("B16-a-1: ocean spectrum is a well-formed directional wave spectrum (CPU physics)", "[ocean][spectrum]")
{
    crd::memory::TlsfAllocator alloc(96U << 20U);
    kir::KGraph                g(&alloc);
    kir::ocean::OceanConfig    cfg;
    cfg.n            = 64;    // small grid for the oracle
    cfg.patch_length = crd::units::Length64{250.0}; // ⇒ dk = 2π/250 ≈ 0.0251, peak kp ≈ 0.10 ⇒ ~4 texels from centre
    cfg.wind_dir     = crd::units::Angle64{0.0};   // wind along +kx
    cfg.swell        = 0.25;
    REQUIRE(cfg.valid());

    const kir::KEntry e  = kir::ocean::build_ocean_spectrum(g, cfg);
    const int         n  = cfg.n;
    const int         nt = n * n;

    crd::containers::Array<crd::f64> h0(&alloc);
    crd::containers::Array<crd::f64> amp(&alloc);
    h0.resize(uz(nt * 4));
    amp.resize(uz(nt));
    kir::KernelBuffer bufs[2] = {{h0.data(), nt * 4, 0, 0}, {amp.data(), nt, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(nt / 64));

    const auto at = [&](int ci, int ri) { return amp[uz(ri * n + ci)]; }; // p = row·n + col

    // (1) non-negative, finite, non-trivial total energy.
    double tot     = 0.0;
    bool   allfin  = true;
    for (int i = 0; i < nt; ++i)
    {
        REQUIRE(amp[uz(i)] >= 0.0);
        if (!std::isfinite(amp[uz(i)])) { allfin = false; }
        tot += amp[uz(i)] * amp[uz(i)];
    }
    REQUIRE(allfin);
    CHECK(tot > 0.0);

    // (2) the DC cell (centred k=0) carries no wave energy.
    CHECK(at(n / 2, n / 2) < 1e-9);

    // (3) DIRECTIONALITY — at equal |k|, energy concentrates along the wind axis (θ=θ0=0) vs perpendicular.
    const int    off   = 4;                       // near the spectral peak (~4 texels)
    const double along = at(n / 2 + off, n / 2);  // kx-axis (θ≈0, wind)
    const double perp  = at(n / 2, n / 2 + off);  // kz-axis (θ≈π/2)
    CHECK(along > perp);

    // (4) HIGH-k ROLLOFF — the JONSWAP peak (+capillary suppression) decays toward Nyquist.
    const double near_peak = at(n / 2 + 3, n / 2);
    const double near_nyq  = at(n - 1, n / 2);
    CHECK(near_nyq < near_peak);

    // (5) PEAK LOCATION — the wind-axis magnitude peaks at an intermediate wavenumber, not at DC or Nyquist.
    double best   = 0.0;
    int    best_o = 0;
    for (int o = 1; o < n / 2; ++o)
    {
        const double v = at(n / 2 + o, n / 2);
        if (v > best) { best = v; best_o = o; }
    }
    CHECK(best_o >= 1);
    CHECK(best_o <= n / 4); // peak sits in the low-to-mid band, not out at Nyquist

    // (6) HERMITIAN precompute — buffer packs [h0(k).re, h0(k).im, conj(h0(−k)).re, conj(h0(−k)).im]; the stored h0(k) at a
    //     texel equals conj(h0(−k)) read from its MIRROR texel's slot (the a-2 evolution relies on this identity).
    const int ci = n / 2 + 5;
    const int ri = n / 2 + 3;
    const int p  = ri * n + ci;
    const int rp = ((n - ri) & (n - 1)) * n + ((n - ci) & (n - 1));
    CHECK(std::fabs(h0[uz(p * 4 + 0)] - h0[uz(rp * 4 + 2)]) < 1e-6);           // h0(k).re == conj(h0(−k)).re at mirror
    CHECK(std::fabs(h0[uz(p * 4 + 1)] - (-h0[uz(rp * 4 + 3)])) < 1e-6);        // h0(k).im == −conj(h0(−k)).im at mirror

    // (7) DETERMINISM — re-evaluating the same graph reproduces the spectrum bit-for-bit.
    crd::containers::Array<crd::f64> amp2(&alloc);
    crd::containers::Array<crd::f64> h02(&alloc);
    amp2.resize(uz(nt));
    h02.resize(uz(nt * 4));
    kir::KernelBuffer bufs2[2] = {{h02.data(), nt * 4, 0, 0}, {amp2.data(), nt, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs2, 2, e.local_size[0], &alloc, static_cast<crd::u32>(nt / 64));
    bool same = true;
    for (int i = 0; i < nt; ++i)
    {
        if (amp2[uz(i)] != amp[uz(i)]) { same = false; }
    }
    CHECK(same);
}

TEST_CASE("B16-a-2: ocean time-evolution packs 8 real fields into 4 Hermitian complex spectra (CPU physics)", "[ocean][evolve]")
{
    crd::memory::TlsfAllocator alloc(160U << 20U);
    kir::ocean::OceanConfig    cfg;
    cfg.n            = 64;
    cfg.patch_length = crd::units::Length64{250.0};
    cfg.wind_dir     = crd::units::Angle64{0.0};
    REQUIRE(cfg.valid());

    const int    n    = cfg.n;
    const int    nt   = n * n;
    const int    half = n / 2;
    const double dk   = 2.0 * 3.14159265358979323846 / cfg.patch_length.value;
    const double t    = 3.0; // an arbitrary evolution time

    // (a) build h0(k) via a-1.
    kir::KGraph       gs(&alloc);
    const kir::KEntry es = kir::ocean::build_ocean_spectrum(gs, cfg);
    crd::containers::Array<crd::f64> h0(&alloc);
    crd::containers::Array<crd::f64> amp(&alloc);
    h0.resize(uz(nt * 4));
    amp.resize(uz(nt));
    kir::KernelBuffer sbufs[2] = {{h0.data(), nt * 4, 0, 0}, {amp.data(), nt, 0, 1}};
    kir::eval_cpu_kernel(gs, es, sbufs, 2, es.local_size[0], &alloc, static_cast<crd::u32>(nt / 64));

    // (b) evolve h0 to time t ⇒ the 4 packed complex spectra (FFT order).
    kir::KGraph       ge(&alloc);
    const kir::KEntry ee = kir::ocean::build_ocean_evolve(ge, cfg);
    crd::containers::Array<crd::f64> par(&alloc);
    crd::containers::Array<crd::f64> sr(&alloc);
    crd::containers::Array<crd::f64> si(&alloc);
    par.resize(1U, t);
    sr.resize(uz(nt * 4), 0.0);
    si.resize(uz(nt * 4), 0.0);
    kir::KernelBuffer ebufs[4] = {
        {h0.data(), nt * 4, 0, 0}, {par.data(), 1, 0, 1}, {sr.data(), nt * 4, 0, 2}, {si.data(), nt * 4, 0, 3}};
    kir::eval_cpu_kernel(ge, ee, ebufs, 4, ee.local_size[0], &alloc, static_cast<crd::u32>(nt / 64));

    // (c) analytic reference: same time-evolution + packing, in f64, compared within f32 tolerance.
    const double grav  = cfg.gravity.value;
    const double depth = cfg.depth.value;
    const double kcap  = 7.4e-5;
    double       maxerr = 0.0;
    for (int mi = 0; mi < n; ++mi)
    {
        for (int ni = 0; ni < n; ++ni)
        {
            const int    p  = mi * n + ni;
            const double kx = (ni - half) * dk;
            const double kz = (mi - half) * dk;
            const double km = std::max(std::sqrt(kx * kx + kz * kz), 1e-6);
            const double th = std::tanh(km * depth);
            const double gk = km * grav + km * km * km * kcap;
            const double w  = std::sqrt(std::max(gk * th, 1e-12));
            const double c  = std::cos(w * t);
            const double s  = std::sin(w * t);
            const double ar = h0[uz(p * 4 + 0)];
            const double ai = h0[uz(p * 4 + 1)];
            const double br = h0[uz(p * 4 + 2)];
            const double bi = h0[uz(p * 4 + 3)];
            const double hr = (ar * c - ai * s) + (br * c + bi * s);
            const double hi = (ar * s + ai * c) + (bi * c - br * s);
            const double cx = kx / km;
            const double cz = kz / km;
            const double dxx = kx * cx;
            const double dzz = kz * cz;
            const double dxz = kx * cz;
            const double cr[4] = {hr + cx * hr, cz * hi - kx * hr, -kz * hi - dxx * hi, dzz * hr - dxz * hi};
            const double ci[4] = {hi + cx * hi, -cz * hr - kx * hi, kz * hr + dxx * hr, dzz * hi + dxz * hr};
            const int    oc  = (ni + half) & (n - 1);
            const int    orw = (mi + half) & (n - 1);
            const int    op  = orw * n + oc;
            for (int f = 0; f < 4; ++f)
            {
                const double scale = std::max(1.0, std::fabs(cr[f]) + std::fabs(ci[f]));
                maxerr = std::max(maxerr, std::fabs(sr[uz(f * nt + op)] - cr[f]) / scale);
                maxerr = std::max(maxerr, std::fabs(si[uz(f * nt + op)] - ci[f]) / scale);
            }
        }
    }
    CHECK(maxerr < 1e-4);

    // (d) determinism — same graph, same params ⇒ bit-identical spectra.
    crd::containers::Array<crd::f64> sr2(&alloc);
    crd::containers::Array<crd::f64> si2(&alloc);
    sr2.resize(uz(nt * 4), 0.0);
    si2.resize(uz(nt * 4), 0.0);
    kir::KernelBuffer ebufs2[4] = {
        {h0.data(), nt * 4, 0, 0}, {par.data(), 1, 0, 1}, {sr2.data(), nt * 4, 0, 2}, {si2.data(), nt * 4, 0, 3}};
    kir::eval_cpu_kernel(ge, ee, ebufs2, 4, ee.local_size[0], &alloc, static_cast<crd::u32>(nt / 64));
    bool same_e = true;
    for (int i = 0; i < nt * 4; ++i)
    {
        if (sr2[uz(i)] != sr[uz(i)] || si2[uz(i)] != si[uz(i)]) { same_e = false; }
    }
    CHECK(same_e);
}

TEST_CASE("B16-a-2: full FFT-ocean pipeline (evolve -> batched IFFT -> assemble) matches a direct reference", "[ocean][pipeline]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    kir::ocean::OceanConfig    cfg;
    cfg.n            = 16; // small square grid for the O(N^4) direct-DFT reference
    cfg.patch_length = crd::units::Length64{250.0};
    cfg.choppiness   = 1.0;
    cfg.foam_bias    = 0.4;
    cfg.foam_scale   = 2.0;
    REQUIRE(cfg.valid());

    const int    n   = cfg.n;
    const int    rc  = n * n;
    const double t   = 2.0;
    const double lam = cfg.choppiness;
    const double tau = 6.28318530717958647693;
    const auto   f32 = [](double v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    // (1) a-1 spectrum -> h0.
    kir::KGraph       gs(&alloc);
    const kir::KEntry es = kir::ocean::build_ocean_spectrum(gs, cfg);
    crd::containers::Array<crd::f64> h0(&alloc);
    crd::containers::Array<crd::f64> amp(&alloc);
    h0.resize(uz(rc * 4));
    amp.resize(uz(rc));
    kir::KernelBuffer sb[2] = {{h0.data(), rc * 4, 0, 0}, {amp.data(), rc, 0, 1}};
    kir::eval_cpu_kernel(gs, es, sb, 2, es.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    // (2) evolve -> 4 packed complex spectra (FFT order).
    kir::KGraph       ge(&alloc);
    const kir::KEntry ee = kir::ocean::build_ocean_evolve(ge, cfg);
    crd::containers::Array<crd::f64> par(&alloc);
    crd::containers::Array<crd::f64> sr(&alloc);
    crd::containers::Array<crd::f64> si(&alloc);
    par.resize(1U, t);
    sr.resize(uz(rc * 4), 0.0);
    si.resize(uz(rc * 4), 0.0);
    kir::KernelBuffer eb[4] = {
        {h0.data(), rc * 4, 0, 0}, {par.data(), 1, 0, 1}, {sr.data(), rc * 4, 0, 2}, {si.data(), rc * 4, 0, 3}};
    kir::eval_cpu_kernel(ge, ee, eb, 4, ee.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    // (3) batched inverse 2-D FFT of the 4 packed fields.
    kir::KGraph  g0(&alloc);
    kir::KGraph  g1(&alloc);
    kir::KGraph* graphs[2] = {&g0, &g1};
    const kir::Fft2dPlan plan = kir::build_fft2d_c2c_batched(graphs, n, n, 4, true, 8);
    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(uz(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    for (int i = 0; i < rc * 4; ++i) { buf(plan.in_re)[i] = sr[uz(i)]; buf(plan.in_im)[i] = si[uz(i)]; }
    for (int k = 0; k < n; ++k)
    {
        const double a         = tau * k / n;
        buf(plan.tw_col_re)[k] = f32(std::cos(a));
        buf(plan.tw_col_im)[k] = f32(-std::sin(a));
        buf(plan.tw_row_re)[k] = buf(plan.tw_col_re)[k];
        buf(plan.tw_row_im)[k] = buf(plan.tw_col_im)[k];
    }
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    // (4) assemble -> displacement + normal maps.
    kir::KGraph       ga(&alloc);
    const kir::KEntry ea = kir::ocean::build_ocean_assemble(ga, cfg);
    crd::containers::Array<crd::f64> disp(&alloc);
    crd::containers::Array<crd::f64> norm(&alloc);
    disp.resize(uz(rc * 4), 0.0);
    norm.resize(uz(rc * 4), 0.0);
    kir::KernelBuffer ab[4] = {{buf(plan.res_re), rc * 4, 0, 0},
                               {buf(plan.res_im), rc * 4, 0, 1},
                               {disp.data(), rc * 4, 0, 2},
                               {norm.data(), rc * 4, 0, 3}};
    kir::eval_cpu_kernel(ga, ea, ab, 4, ea.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    // (5) reference: direct inverse DFT of the evolve output per field, unpack + assemble math in f64.
    crd::containers::Array<crd::f64> fr(&alloc);
    crd::containers::Array<crd::f64> fi(&alloc);
    fr.resize(uz(rc * 4));
    fi.resize(uz(rc * 4));
    for (int f = 0; f < 4; ++f) { idft2(n, sr.data() + f * rc, si.data() + f * rc, fr.data() + f * rc, fi.data() + f * rc); }

    double maxerr = 0.0;
    for (int q = 0; q < rc; ++q)
    {
        const double hh  = fr[uz(0 * rc + q)];
        const double dx  = fi[uz(0 * rc + q)];
        const double dz  = fr[uz(1 * rc + q)];
        const double sxv = fi[uz(1 * rc + q)];
        const double szv = fr[uz(2 * rc + q)];
        const double jxx = fi[uz(2 * rc + q)];
        const double jzz = fr[uz(3 * rc + q)];
        const double jxz = fi[uz(3 * rc + q)];
        const double jac = (1.0 + lam * jxx) * (1.0 + lam * jzz) - (lam * jxz) * (lam * jxz);
        const double fo  = std::max(0.0, std::min(1.0, (cfg.foam_bias - jac) * cfg.foam_scale));
        const double invn = 1.0 / std::sqrt(sxv * sxv + szv * szv + 1.0);
        const double rd[4] = {lam * dx, hh, lam * dz, fo};
        const double rn[4] = {-sxv * invn, invn, -szv * invn, jac};
        for (int c = 0; c < 4; ++c)
        {
            const double sd = std::max(1.0, std::fabs(rd[c]));
            const double sn = std::max(1.0, std::fabs(rn[c]));
            maxerr          = std::max(maxerr, std::fabs(disp[uz(q * 4 + c)] - rd[c]) / sd);
            maxerr          = std::max(maxerr, std::fabs(norm[uz(q * 4 + c)] - rn[c]) / sn);
        }
    }
    CHECK(maxerr < 5e-3);

    // (6) foam is a valid coverage in [0,1]; the shading normal is unit length everywhere.
    bool foam_ok   = true;
    bool norm_unit = true;
    for (int q = 0; q < rc; ++q)
    {
        const double fo = disp[uz(q * 4 + 3)];
        if (fo < -1e-6 || fo > 1.0 + 1e-6) { foam_ok = false; }
        const double nx = norm[uz(q * 4 + 0)];
        const double ny = norm[uz(q * 4 + 1)];
        const double nz = norm[uz(q * 4 + 2)];
        if (std::fabs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0) > 2e-3) { norm_unit = false; }
    }
    CHECK(foam_ok);
    CHECK(norm_unit);
}

TEST_CASE("B16-a-2 FUSION: fused evolve+row-IFFT == un-fused (evolve then radix-4 row IFFT), bit-exact", "[ocean][fusion]")
{
    crd::memory::TlsfAllocator alloc(192U << 20U);
    kir::ocean::OceanConfig    cfg;
    cfg.n            = 64; // power of four (the fused row IFFT is radix-4)
    cfg.patch_length = crd::units::Length64{250.0};
    REQUIRE(cfg.valid());

    const int    n   = cfg.n;
    const int    rc  = n * n;
    const double t   = 2.0;
    const double tau = 6.28318530717958647693;
    const auto   f32 = [](double v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    // a-1 -> h0.
    kir::KGraph       gs(&alloc);
    const kir::KEntry es = kir::ocean::build_ocean_spectrum(gs, cfg);
    crd::containers::Array<crd::f64> h0(&alloc);
    crd::containers::Array<crd::f64> amp(&alloc);
    h0.resize(uz(rc * 4));
    amp.resize(uz(rc));
    kir::KernelBuffer sb[2] = {{h0.data(), rc * 4, 0, 0}, {amp.data(), rc, 0, 1}};
    kir::eval_cpu_kernel(gs, es, sb, 2, es.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    // un-fused: evolve -> spec.
    kir::KGraph       ge(&alloc);
    const kir::KEntry ee = kir::ocean::build_ocean_evolve(ge, cfg);
    crd::containers::Array<crd::f64> par(&alloc);
    crd::containers::Array<crd::f64> sr(&alloc);
    crd::containers::Array<crd::f64> si(&alloc);
    par.resize(1U, t);
    sr.resize(uz(rc * 4), 0.0);
    si.resize(uz(rc * 4), 0.0);
    kir::KernelBuffer eb[4] = {
        {h0.data(), rc * 4, 0, 0}, {par.data(), 1, 0, 1}, {sr.data(), rc * 4, 0, 2}, {si.data(), rc * 4, 0, 3}};
    kir::eval_cpu_kernel(ge, ee, eb, 4, ee.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    // cols-point twiddles W_N.
    crd::containers::Array<crd::f64> twr(&alloc);
    crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(uz(n));
    twi.resize(uz(n));
    for (int k = 0; k < n; ++k)
    {
        twr[uz(k)] = f32(std::cos(tau * k / n));
        twi[uz(k)] = f32(-std::sin(tau * k / n));
    }

    // un-fused row IFFT: radix-4 batched inverse over the 4 packed fields (grid = rows·batch = n·4).
    kir::KGraph          gr(&alloc);
    const kir::Fft1dPlan rp = kir::build_fft1d_radix4(gr, n, true, true);
    crd::containers::Array<crd::f64> xur(&alloc);
    crd::containers::Array<crd::f64> xui(&alloc);
    xur.resize(uz(rc * 4), 0.0);
    xui.resize(uz(rc * 4), 0.0);
    kir::KernelBuffer rb[6] = {{sr.data(), rc * 4, 0, 0}, {si.data(), rc * 4, 0, 1}, {twr.data(), n, 0, 2},
                               {twi.data(), n, 0, 3}, {xur.data(), rc * 4, 0, 4}, {xui.data(), rc * 4, 0, 5}};
    kir::eval_cpu_kernel(gr, rp.entry, rb, 6, rp.entry.local_size[0], &alloc, static_cast<crd::u32>(n * 4));

    // fused evolve+row-IFFT: computes h~ inline at the load (grid = field·n + row = 4·n).
    kir::KGraph       gf(&alloc);
    const kir::KEntry ef = kir::ocean::build_ocean_evolve_rowfft(gf, cfg);
    crd::containers::Array<crd::f64> xfr(&alloc);
    crd::containers::Array<crd::f64> xfi(&alloc);
    xfr.resize(uz(rc * 4), 0.0);
    xfi.resize(uz(rc * 4), 0.0);
    kir::KernelBuffer fb[6] = {{h0.data(), rc * 4, 0, 0}, {par.data(), 1, 0, 1}, {twr.data(), n, 0, 2},
                               {twi.data(), n, 0, 3}, {xfr.data(), rc * 4, 0, 4}, {xfi.data(), rc * 4, 0, 5}};
    kir::eval_cpu_kernel(gf, ef, fb, 6, ef.local_size[0], &alloc, static_cast<crd::u32>(n * 4));

    // BIT-EXACT: the fusion must reproduce the un-fused row-IFFT output exactly (shared helpers + identical radix-4 stages).
    int bad = 0;
    for (int i = 0; i < rc * 4; ++i)
    {
        if (xfr[uz(i)] != xur[uz(i)] || xfi[uz(i)] != xui[uz(i)]) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("B16-a-3: multi-cascade batched FFT-ocean -- each cascade matches a direct reference, cascades distinct", "[ocean][cascade]")
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    kir::ocean::OceanCascadeConfig cc;
    cc.base.n          = 16; // small square grid for the O(N^4) direct DFT reference
    cc.base.foam_bias  = 0.4;
    cc.base.foam_scale = 2.0;
    cc.count           = 3; // L = 1000 / 250 / 60 m
    REQUIRE(cc.valid());

    const int    n   = cc.base.n;
    const int    rc  = n * n;
    const int    nc  = cc.count;
    const int    nf  = nc * 4; // 4 packed fields per cascade
    const double t   = 2.0;
    const double tau = 6.28318530717958647693;

    // --- per cascade: a-1 spectrum -> h0[cascade] ; evolve(t) -> spec[cascade] (4 packed fields at field offset 4c) ---
    crd::containers::Array<crd::f64> h0(&alloc);
    crd::containers::Array<crd::f64> amp(&alloc);
    crd::containers::Array<crd::f64> par(&alloc);
    crd::containers::Array<crd::f64> sr(&alloc);
    crd::containers::Array<crd::f64> si(&alloc);
    h0.resize(uz(nf * rc));
    amp.resize(uz(rc));
    par.resize(1U, t);
    sr.resize(uz(nf * rc), 0.0);
    si.resize(uz(nf * rc), 0.0);
    for (int c = 0; c < nc; ++c)
    {
        const kir::ocean::OceanConfig cfg = cc.cascade(c);
        kir::KGraph                   gs(&alloc);
        const kir::KEntry             es = kir::ocean::build_ocean_spectrum(gs, cfg);
        kir::KernelBuffer sb[2] = {{h0.data() + 4 * c * rc, 4 * rc, 0, 0}, {amp.data(), rc, 0, 1}};
        kir::eval_cpu_kernel(gs, es, sb, 2, es.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

        kir::KGraph       ge(&alloc);
        const kir::KEntry ee = kir::ocean::build_ocean_evolve(ge, cfg);
        kir::KernelBuffer eb[4] = {{h0.data() + 4 * c * rc, 4 * rc, 0, 0}, {par.data(), 1, 0, 1},
                                   {sr.data() + 4 * c * rc, 4 * rc, 0, 2}, {si.data() + 4 * c * rc, 4 * rc, 0, 3}};
        kir::eval_cpu_kernel(ge, ee, eb, 4, ee.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));
    }

    // --- ONE batched inverse 2-D FFT over ALL 4*C fields (the DRAM-bound regime the crush infra targets) ---
    kir::KGraph  g0(&alloc);
    kir::KGraph  g1(&alloc);
    kir::KGraph* graphs[2] = {&g0, &g1};
    const kir::Fft2dPlan plan = kir::build_fft2d_c2c_batched(graphs, n, n, nf, true, 8);
    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(uz(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    for (int i = 0; i < nf * rc; ++i) { buf(plan.in_re)[i] = sr[uz(i)]; buf(plan.in_im)[i] = si[uz(i)]; }
    for (int k = 0; k < n; ++k)
    {
        const double a         = tau * k / n;
        buf(plan.tw_col_re)[k] = static_cast<crd::f64>(static_cast<float>(std::cos(a)));
        buf(plan.tw_col_im)[k] = static_cast<crd::f64>(static_cast<float>(-std::sin(a)));
        buf(plan.tw_row_re)[k] = buf(plan.tw_col_re)[k];
        buf(plan.tw_row_im)[k] = buf(plan.tw_col_im)[k];
    }
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    // --- per cascade: assemble -> disp[cascade], norm[cascade] ---
    crd::containers::Array<crd::f64> disp(&alloc);
    crd::containers::Array<crd::f64> norm(&alloc);
    disp.resize(uz(nf * rc), 0.0);
    norm.resize(uz(nf * rc), 0.0);
    for (int c = 0; c < nc; ++c)
    {
        kir::KGraph       ga(&alloc);
        const kir::KEntry ea = kir::ocean::build_ocean_assemble(ga, cc.cascade(c));
        kir::KernelBuffer ab[4] = {{buf(plan.res_re) + 4 * c * rc, 4 * rc, 0, 0}, {buf(plan.res_im) + 4 * c * rc, 4 * rc, 0, 1},
                                   {disp.data() + 4 * c * rc, 4 * rc, 0, 2}, {norm.data() + 4 * c * rc, 4 * rc, 0, 3}};
        kir::eval_cpu_kernel(ga, ea, ab, 4, ea.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));
    }

    // --- verify each cascade against a direct inverse-DFT reference of its OWN spectrum, and that cascades differ ---
    crd::containers::Array<crd::f64> fr(&alloc);
    crd::containers::Array<crd::f64> fi(&alloc);
    fr.resize(uz(4 * rc));
    fi.resize(uz(4 * rc));
    double maxerr   = 0.0;
    bool   foam_ok  = true;
    bool   norm_ok  = true;
    for (int c = 0; c < nc; ++c)
    {
        const double lam = cc.choppiness[c];
        for (int f = 0; f < 4; ++f)
        {
            idft2(n, sr.data() + (4 * c + f) * rc, si.data() + (4 * c + f) * rc, fr.data() + f * rc, fi.data() + f * rc);
        }
        for (int q = 0; q < rc; ++q)
        {
            const double hh  = fr[uz(0 * rc + q)];
            const double dx  = fi[uz(0 * rc + q)];
            const double dz  = fr[uz(1 * rc + q)];
            const double sxv = fi[uz(1 * rc + q)];
            const double szv = fr[uz(2 * rc + q)];
            const double jxx = fi[uz(2 * rc + q)];
            const double jzz = fr[uz(3 * rc + q)];
            const double jxz = fi[uz(3 * rc + q)];
            const double jac = (1.0 + lam * jxx) * (1.0 + lam * jzz) - (lam * jxz) * (lam * jxz);
            const double fo  = std::max(0.0, std::min(1.0, (cc.base.foam_bias - jac) * cc.base.foam_scale));
            const double invn = 1.0 / std::sqrt(sxv * sxv + szv * szv + 1.0);
            const double rd[4] = {lam * dx, hh, lam * dz, fo};
            const double rn[4] = {-sxv * invn, invn, -szv * invn, jac};
            for (int k = 0; k < 4; ++k)
            {
                const int    dq = 4 * c * rc + q * 4 + k; // cascade c's float4/pixel block starts at 4c*rc
                const double sd = std::max(1.0, std::fabs(rd[k]));
                const double sn = std::max(1.0, std::fabs(rn[k]));
                maxerr = std::max(maxerr, std::fabs(disp[uz(dq)] - rd[k]) / sd);
                maxerr = std::max(maxerr, std::fabs(norm[uz(dq)] - rn[k]) / sn);
            }
            const double fov = disp[uz(4 * c * rc + q * 4 + 3)];
            if (fov < -1e-6 || fov > 1.0 + 1e-6) { foam_ok = false; }
            const double nx = norm[uz(4 * c * rc + q * 4 + 0)];
            const double ny = norm[uz(4 * c * rc + q * 4 + 1)];
            const double nz = norm[uz(4 * c * rc + q * 4 + 2)];
            if (std::fabs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0) > 2e-3) { norm_ok = false; }
        }
    }
    CHECK(maxerr < 5e-3);
    CHECK(foam_ok);
    CHECK(norm_ok);

    // cascades have DIFFERENT patch length ⇒ distinct displacement fields.
    double diff = 0.0;
    for (int q = 0; q < rc; ++q) { diff += std::fabs(disp[uz(0 * rc + q * 4 + 1)] - disp[uz(4 * 2 * rc + q * 4 + 1)]); }
    CHECK(diff > 1e-6);
}

TEST_CASE("B16-a-3: temporal foam accumulation persists, decays, and re-injects", "[ocean][foam]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::ocean::OceanConfig    cfg;
    cfg.n           = 8;
    cfg.foam_decay  = 0.9;
    const int         rc = cfg.n * cfg.n; // 64 = one workgroup
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::ocean::build_ocean_foam_accumulate(g, cfg);

    crd::containers::Array<crd::f64> inject(&alloc);
    crd::containers::Array<crd::f64> zero(&alloc);
    crd::containers::Array<crd::f64> prev(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    inject.resize(uz(rc), 0.0);
    zero.resize(uz(rc), 0.0);
    prev.resize(uz(rc), 0.0);
    out.resize(uz(rc), 0.0);
    for (int i = 0; i < rc; ++i) { inject[uz(i)] = (i % 3 == 0) ? 0.8 : 0.0; }

    const auto step = [&](crd::containers::Array<crd::f64>& p, crd::containers::Array<crd::f64>& inj, crd::containers::Array<crd::f64>& o) {
        kir::KernelBuffer b[3] = {{p.data(), rc, 0, 0}, {inj.data(), rc, 0, 1}, {o.data(), rc, 0, 2}};
        kir::eval_cpu_kernel(g, e, b, 3, e.local_size[0], &alloc, 1U);
    };

    // frame 1: prev=0, inject ⇒ foam = inject.
    step(prev, inject, out);
    bool f1 = true;
    for (int i = 0; i < rc; ++i) { if (std::fabs(out[uz(i)] - inject[uz(i)]) > 1e-5) { f1 = false; } }
    CHECK(f1);

    // frames 2,3: no inject ⇒ foam decays by 0.9 each frame ⇒ inject·0.81.
    for (int i = 0; i < rc; ++i) { prev[uz(i)] = out[uz(i)]; }
    step(prev, zero, out);
    for (int i = 0; i < rc; ++i) { prev[uz(i)] = out[uz(i)]; }
    step(prev, zero, out);
    bool f3 = true;
    for (int i = 0; i < rc; ++i) { if (std::fabs(out[uz(i)] - inject[uz(i)] * 0.81) > 1e-4) { f3 = false; } }
    CHECK(f3);

    // frame 4: re-inject ⇒ max(decayed, inject) = inject (inject > inject·0.9·0.81), whitecaps refresh.
    for (int i = 0; i < rc; ++i) { prev[uz(i)] = out[uz(i)]; }
    step(prev, inject, out);
    bool f4 = true;
    for (int i = 0; i < rc; ++i) { if (std::fabs(out[uz(i)] - inject[uz(i)]) > 1e-5) { f4 = false; } }
    CHECK(f4);
}
